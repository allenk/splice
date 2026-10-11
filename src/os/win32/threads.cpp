// ─── Win32 thread coordination ────────────────────────────────────────────
//
// Toolhelp enumeration, suspend / resume, and program-counter read and write.
// Everything Windows-shaped about coordinating the threads of this process
// lives here and stops here.
//
// Two races are handled rather than left to the caller, because both are
// properties of these OS calls and nothing above this layer can see them:
//
//   * A thread listed by a snapshot can be gone by the time it is opened.
//     Windows reports that as ERROR_INVALID_PARAMETER from OpenThread. A
//     thread that has exited needs no coordination, so that is a skip, not a
//     failure -- see the note on inferring it below. This was 28 lines in the
//     one consumer that ever got this far.
//
//   * CreateToolhelp32Snapshot fails with ERROR_BAD_LENGTH when the process
//     changes shape mid-snapshot, and the documented remedy is to call it
//     again. A bounded retry here is the difference between a reliable
//     primitive and an intermittent one.
//
// Measured cost, 2026-10-11, optimised build on this machine: a thread
// snapshot takes ~21 ms and walking it ~97 ms, because TH32CS_SNAPTHREAD
// ignores its process argument and lists **every thread on the machine** --
// 22,466 of them in that measurement, one of which was ours. So enumeration
// is O(threads on the system), and the number gets worse on a loaded machine,
// not better.
//
// That is affordable exactly once, in the transaction's prepare phase, before
// anything is suspended: it does not lengthen the window in which threads are
// frozen and it is nowhere near a hooked function's call path. It is also the
// measurement that prices the old one-site-at-a-time shape, where seven sites
// meant seven enumerations -- most of a second of install time spent walking
// other processes' threads. Detours enumerates the same way; the answer is to
// do it once per group, not to find a faster snapshot.
// ───────────────────────────────────────────────────────────────────────────
#ifndef NOMINMAX
#   define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
#endif

#include "../threads.h"

#include <windows.h>
#include <tlhelp32.h>

namespace splice::os {

namespace {

// Everything the transaction does to a thread, and nothing else: no terminate,
// no token, no memory access through the thread.
constexpr DWORD kThreadRights = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION;

// A snapshot of a process that is actively creating or exiting threads can be
// refused outright; the documented answer is to ask again.
constexpr int kSnapshotAttempts = 8;

// How many threads may exit between being listed and being opened before the
// thread set is called unstable. A process churning threads that fast cannot
// be coordinated from a snapshot, and saying so beats proceeding.
constexpr int kVanishedThreadBudget = 64;

[[nodiscard]] HANDLE to_handle(ThreadRef ref) noexcept {
    return reinterpret_cast<HANDLE>(ref.value);
}

[[nodiscard]] std::uintptr_t read_pc(const CONTEXT& context) noexcept {
#if defined(_M_X64)
    return static_cast<std::uintptr_t>(context.Rip);
#elif defined(_M_ARM64)
    return static_cast<std::uintptr_t>(context.Pc);
#elif defined(_M_IX86)
    return static_cast<std::uintptr_t>(context.Eip);
#elif defined(_M_ARM)
    return static_cast<std::uintptr_t>(context.Pc);
#else
#   error "splice::os::read_pc: unknown architecture for this OS"
#endif
}

void write_pc(CONTEXT& context, std::uintptr_t pc) noexcept {
#if defined(_M_X64)
    context.Rip = static_cast<DWORD64>(pc);
#elif defined(_M_ARM64)
    context.Pc = static_cast<DWORD64>(pc);
#elif defined(_M_IX86)
    context.Eip = static_cast<DWORD>(pc);
#elif defined(_M_ARM)
    context.Pc = static_cast<DWORD>(pc);
#else
#   error "splice::os::write_pc: unknown architecture for this OS"
#endif
}

// A Toolhelp thread snapshot, retried over ERROR_BAD_LENGTH.
class ThreadSnapshot {
public:
    ThreadSnapshot() noexcept {
        for (int attempt = 0; attempt < kSnapshotAttempts; ++attempt) {
            m_snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (m_snapshot != INVALID_HANDLE_VALUE) return;
            if (::GetLastError() != ERROR_BAD_LENGTH) return;
        }
    }

    ~ThreadSnapshot() {
        if (m_snapshot != INVALID_HANDLE_VALUE) ::CloseHandle(m_snapshot);
    }

    ThreadSnapshot(const ThreadSnapshot&) = delete;
    ThreadSnapshot& operator=(const ThreadSnapshot&) = delete;

    [[nodiscard]] bool valid() const noexcept { return m_snapshot != INVALID_HANDLE_VALUE; }

    // Walk the threads of this process, calling `visit(tid)` for each. Stops
    // early and returns false if `visit` returns false.
    template <typename Visit>
    [[nodiscard]] bool for_each_thread_of_this_process(Visit visit) const noexcept {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (!::Thread32First(m_snapshot, &entry)) return true;
        const DWORD self_process = ::GetCurrentProcessId();
        do {
            // dwSize is the field count, not a sanity check: an entry that
            // does not reach th32OwnerProcessID cannot be attributed to a
            // process, so it cannot be ours.
            if (entry.dwSize < offsetof(THREADENTRY32, th32OwnerProcessID) +
                                   sizeof(entry.th32OwnerProcessID)) {
                continue;
            }
            if (entry.th32OwnerProcessID != self_process) continue;
            if (!visit(entry.th32ThreadID)) return false;
        } while (::Thread32Next(m_snapshot, &entry));
        return true;
    }

private:
    HANDLE m_snapshot = INVALID_HANDLE_VALUE;
};

// Why ERROR_INVALID_PARAMETER is read as "it exited", with no second snapshot.
//
// OpenThread returns that code when the id does not name an existing thread.
// The consumer this replaces could not tell that from a genuinely bad argument,
// so it took a fresh snapshot and re-checked. Here the id cannot be a bad
// argument: it came out of a snapshot of this process moments earlier, it was
// non-zero, and a thread of our own process is never denied to us. The only
// remaining way for it to name no thread is that it has since exited.
//
// Confirming that with another snapshot would cost a second system-wide walk
// per vanished thread -- on the measurement above, about 118 ms each, up to
// kVanishedThreadBudget times -- to re-establish something already known. The
// bounded budget stays, because a process shedding dozens of threads during
// one enumeration is churning too fast for a snapshot to describe.
} // namespace

Enumeration enumerate_other_threads(std::span<ThreadRef> out) noexcept {
    Enumeration result{EnumerateStatus::ok, 0};

    const ThreadSnapshot snapshot;
    if (!snapshot.valid()) {
        result.status = EnumerateStatus::failed;
        return result;
    }

    const DWORD self_thread = ::GetCurrentThreadId();
    int vanished = 0;

    // The visitor sets the status it stops for, so the walk's own result adds
    // nothing. One snapshot is the authoritative list: a thread created after
    // it was taken is not enlisted, which is this tier's documented limit and
    // not something another snapshot would fix.
    (void)snapshot.for_each_thread_of_this_process([&](DWORD tid) noexcept {
        if (tid == self_thread || tid == 0) return true;

        const HANDLE handle = ::OpenThread(kThreadRights, FALSE, tid);
        if (handle == nullptr) {
            if (::GetLastError() == ERROR_INVALID_PARAMETER) {
                if (++vanished <= kVanishedThreadBudget) return true;
                result.status = EnumerateStatus::unstable;
                return false;
            }
            result.status = EnumerateStatus::failed;
            return false;
        }

        if (result.count >= out.size()) {
            ::CloseHandle(handle);
            result.status = EnumerateStatus::truncated;
            return false;
        }
        out[result.count++] = ThreadRef{reinterpret_cast<std::uintptr_t>(handle)};
        return true;
    });

    // A count of zero with status ok is an ordinary single-threaded answer.
    return result;
}

void release_thread(ThreadRef ref) noexcept {
    if (!ref) return;
    ::CloseHandle(to_handle(ref));
}

std::uint32_t thread_id(ThreadRef ref) noexcept {
    if (!ref) return 0;
    return static_cast<std::uint32_t>(::GetThreadId(to_handle(ref)));
}

bool suspend(ThreadRef ref, std::uint32_t& prior_count) noexcept {
    if (!ref) return false;
    const DWORD previous = ::SuspendThread(to_handle(ref));
    if (previous == MAXDWORD) return false;
    prior_count = static_cast<std::uint32_t>(previous);
    return true;
}

bool resume(ThreadRef ref, std::uint32_t& prior_count) noexcept {
    if (!ref) return false;
    const DWORD previous = ::ResumeThread(to_handle(ref));
    if (previous == MAXDWORD) return false;
    prior_count = static_cast<std::uint32_t>(previous);
    return true;
}

bool capture_pc(ThreadRef ref, std::uintptr_t& pc) noexcept {
    if (!ref) return false;
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    if (!::GetThreadContext(to_handle(ref), &context)) return false;
    pc = read_pc(context);
    return true;
}

bool set_pc(ThreadRef ref, std::uintptr_t pc) noexcept {
    if (!ref) return false;
    // Read-modify-write rather than writing a context captured earlier: the
    // program counter is the only field this surface changes, and a context
    // read at suspend time would carry a stack pointer and flags that are
    // nobody's business to write back.
    //
    // The extra read costs nothing on the path that matters: the transaction
    // only moves a program counter that is inside the bytes being replaced, so
    // in the ordinary install no thread needs this call at all.
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL;
    const HANDLE handle = to_handle(ref);
    if (!::GetThreadContext(handle, &context)) return false;
    write_pc(context, pc);
    return ::SetThreadContext(handle, &context) != FALSE;
}

std::uint32_t last_error() noexcept {
    return static_cast<std::uint32_t>(::GetLastError());
}

} // namespace splice::os
