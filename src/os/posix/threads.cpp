// ─── POSIX thread coordination — refused, and why ─────────────────────────
//
// There is no portable POSIX equivalent of SuspendThread, and on Android there
// is no equivalent at all: Bionic exposes no thread-suspension call, and the
// one mechanism that could stand in for it -- a signal handler that parks the
// target and publishes its own ucontext -- is a design, not a port. So this
// file refuses, honestly and in one place, and `kCoordinationSupported` is
// false so the engine refuses the live tier at compile time rather than one
// call at a time.
//
// What the eventual implementation looks like, recorded here because the shape
// is the research and the file is where it lands (S-17):
//
//   enumerate   walk /proc/self/task, which lists tids rather than handles, so
//               ThreadRef would carry a tid and release_thread would be a
//               no-op. The same race exists -- a tid can exit between the walk
//               and its use -- and the same answer applies: a thread that has
//               gone needs no coordination.
//   suspend     send a dedicated real-time signal and wait on a futex the
//               handler posts. There is no suspend COUNT, so prior_count is
//               either synthesised or the surface admits POSIX has no such
//               notion. Nesting is the caller's problem either way.
//   capture_pc  read the handler's ucontext_t: uc_mcontext.pc on AArch64,
//               .gregs[REG_RIP] on x86_64 glibc. That is the same (OS,
//               architecture) pairing the Windows side has, in the same place.
//   set_pc      write that same field before the handler returns, which is
//               the only moment it can be written at all -- a real difference
//               from Windows, where a suspended thread's context is writable
//               at any time, and the reason this is research.
//
// Until then, every function here reports failure without side effects, so a
// caller that ignores kCoordinationSupported still cannot do damage.
// ───────────────────────────────────────────────────────────────────────────
#include "../threads.h"

#include <cerrno>

namespace splice::os {

Enumeration enumerate_other_threads(std::span<ThreadRef> out) noexcept {
    (void)out;
    return Enumeration{EnumerateStatus::unsupported, 0};
}

void release_thread(ThreadRef ref) noexcept {
    (void)ref;
}

std::uint32_t thread_id(ThreadRef ref) noexcept {
    (void)ref;
    return 0;
}

bool suspend(ThreadRef ref, std::uint32_t& prior_count) noexcept {
    (void)ref;
    (void)prior_count;
    return false;
}

bool resume(ThreadRef ref, std::uint32_t& prior_count) noexcept {
    (void)ref;
    (void)prior_count;
    return false;
}

bool capture_pc(ThreadRef ref, std::uintptr_t& pc) noexcept {
    (void)ref;
    (void)pc;
    return false;
}

bool set_pc(ThreadRef ref, std::uintptr_t pc) noexcept {
    (void)ref;
    (void)pc;
    return false;
}

std::uint32_t last_error() noexcept {
    // Reported even though every call above refuses without touching errno:
    // when the posix implementation arrives, the one thing that must not also
    // need adding is the field the failure record already promises.
    return static_cast<std::uint32_t>(errno);
}

} // namespace splice::os
