// ─── OS thread coordination primitives ────────────────────────────────────
//
// What the OS can tell us about the other threads in this process, and what it
// will let us do to them: enumerate, suspend, read and move the program
// counter, resume. One declaration, implemented once per OS, exposing no
// platform type — the same shape as src/os/memory.h, which this file follows
// deliberately.
//
// Two things are deliberately NOT here, because they are what made the
// previous version of this layer unable to serve the platform Splice ships on:
//
//   * No register names. The program counter is a std::uintptr_t, not a field
//     of a platform CONTEXT. An implementation knows how its own OS spells the
//     program counter for the architecture it is being compiled for -- that
//     pairing is unavoidable and belongs here -- but nothing above this line
//     knows a register name, and nothing here knows which registers a patch
//     cares about.
//
//   * No patch lengths. The previous NativeThreadOps::flush took a target and
//     flushed exactly 5 bytes: one architecture's patch size, inside a struct
//     whose only job was to abstract the OS. Instruction-cache flushing
//     already lives in src/os/memory.h, taking the size from the caller, which
//     is the engine, which is the only layer entitled to know it.
//
// Allocation: enumerate_other_threads may allocate (an OS thread snapshot
// does). It is called during the transaction's prepare phase, before anything
// is suspended. Every other call here allocates nothing, takes no lock of
// ours, and logs nothing, because they run while threads are suspended and a
// suspended thread may hold the heap, loader or logging lock.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace splice::os {

// Whether this OS offers thread coordination at all. False on POSIX today:
// Android has no SuspendThread, and the shape that would replace it -- a
// /proc/self/task walk plus signal-based rendezvous -- is research, not a
// port. The engine refuses the live tier at compile time where this is false,
// rather than discovering it one call at a time.
#if defined(_WIN32)
inline constexpr bool kCoordinationSupported = true;
#else
inline constexpr bool kCoordinationSupported = false;
#endif

// An opaque reference to a thread of this process, owning whatever the OS
// handed out for it. Zero is "none". Only enumerate_other_threads produces
// one, which is why nothing here re-validates that a reference belongs to this
// process: the check the previous primitive performed existed because its
// caller opened the handles.
struct ThreadRef {
    std::uintptr_t value = 0;

    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
};

enum class EnumerateStatus {
    ok,          // out[0, count) is every other thread of this process
    truncated,   // the buffer was too small; the set is incomplete
    unstable,    // threads kept appearing or dying faster than they could be opened
    failed,      // the OS would not enumerate or would not open a live thread
    unsupported, // this OS offers no thread coordination
};

struct Enumeration {
    EnumerateStatus status = EnumerateStatus::unsupported;
    std::size_t     count  = 0;   // references written to the buffer, even on failure
};

// Open every thread of this process except the calling one, writing them into
// `out`. The caller's own thread is excluded because every caller needs it
// excluded and only the OS can identify it.
//
// On any status other than `ok` the caller must still release the `count`
// references that were written, and must not proceed: a thread set that is
// incomplete or stale is the one thing coordination cannot be built on. A
// thread that dies between being listed and being opened is not a failure --
// it needs no coordination -- but a thread that cannot be opened for any other
// reason is.
[[nodiscard]] Enumeration enumerate_other_threads(std::span<ThreadRef> out) noexcept;

// Release a reference. Harmless on a zero reference, so a partially filled
// buffer can be released in one loop.
void release_thread(ThreadRef ref) noexcept;

// The OS thread id, for reporting which thread a failure happened on. Zero
// when unavailable.
[[nodiscard]] std::uint32_t thread_id(ThreadRef ref) noexcept;

// Suspend, reporting the suspend count the thread had BEFORE this call. The
// count matters on resume: anything other than prior_count + 1 coming back
// means someone else is suspending the same threads, and the transaction's
// bookkeeping is no longer the truth.
[[nodiscard]] bool suspend(ThreadRef ref, std::uint32_t& prior_count) noexcept;

// Resume, reporting the suspend count the thread had before this call -- so a
// successful resume of a thread suspended once reports 1.
[[nodiscard]] bool resume(ThreadRef ref, std::uint32_t& prior_count) noexcept;

// Read the program counter of a suspended thread. Undefined for a running
// thread, which is a property of the OS calls underneath, not a choice here.
[[nodiscard]] bool capture_pc(ThreadRef ref, std::uintptr_t& pc) noexcept;

// Move the program counter of a suspended thread, changing nothing else. The
// inverse of a set_pc is another set_pc back to the captured value: since the
// program counter is the only thing this surface ever changes, restoring it
// restores everything.
[[nodiscard]] bool set_pc(ThreadRef ref, std::uintptr_t pc) noexcept;

// Whatever the OS last said about why one of the calls above failed, as a
// number worth putting in a report. GetLastError on Windows, errno elsewhere.
//
// Read it immediately after the call that failed and nowhere else. It exists
// because a failure record that says "suspend failed on thread 4132" is a
// bug report and one that says "suspend failed" is a shrug, and the number is
// the only part of that a layer above the OS cannot reconstruct.
[[nodiscard]] std::uint32_t last_error() noexcept;

} // namespace splice::os
