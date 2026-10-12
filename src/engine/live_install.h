// ─── The group transaction: N sites, one suspension ────────────────────────
//
// This is the only layer that knows both the architecture and the OS, which is
// where it is allowed to be. Everything above it sees a C ABI; everything
// below it sees either a prepared patch or a thread reference.
//
// Phases, in order, with the allocation rule encoded in the shape rather than
// documented beside it:
//
//   prepare   ALL allocation happens here: trampolines, boundary maps, thread
//             references. Nothing is written to any target. A refusal here
//             leaves every target untouched.
//   enlist    suspend every enlisted thread, in forward order
//   map       capture each program counter and map it through every site's
//             boundary map; refuse if any is unmappable
//   migrate   move the program counters that have to move
//   write     write every site
//   flush     flush each site, with that site's own length
//   resume    resume in reverse order
//
// Three properties follow from doing it this way, and none of them is
// available from installing one site at a time:
//
//   * **No allocation, no logging, no loader call and no lock acquisition
//     happens between enlist and resume.** A suspended thread may hold the
//     heap lock, the loader lock or the logging lock, so taking any of them
//     while it is suspended is a deadlock waiting for the wrong interleaving.
//     Making prepare own every allocation is how the code enforces that rather
//     than asking the reader to.
//   * **Group atomicity is real.** If site 5 fails to write, sites 0..4 are
//     restored *while the threads are still suspended*, and the outcome says
//     `recovered` with nothing published. Seven sites installed in seven
//     suspensions, with a refusal at the fifth leaving five hooks live and
//     the module unloadable, becomes impossible by construction.
//   * **One suspension instead of N.** Seven suspend/resume cycles of every
//     thread in a process collapse to one, and the enumeration -- which costs
//     ~118 ms because Toolhelp walks every thread on the machine -- happens
//     once.
//
// Migrate before write, which is NOT the order the design document listed.
// Both orders are safe while the threads are suspended, because a suspended
// thread executes nothing; the difference is what a failure costs. Migrating
// first means a failed set_pc needs only the contexts put back, with no target
// yet modified. Writing first would mean undoing writes as well, for no gain.
// A thread migrated into a trampoline whose patch is then abandoned is still
// correct -- the trampoline holds a faithful copy of the prologue and jumps
// back -- but it does not come up, because the contexts are restored too.
//
// Publication happens in prepare, which is also not what the primitive this
// replaces did. It stored the original pointer inside the frozen window, one
// instruction before the write, so that a refusal could withdraw it. That is
// not available here: publishing an original through splice's registry takes a
// lock, and a lock is the one thing that must not be taken while another
// thread is suspended. So the caller publishes before commit, and the price is
// that a group which fails keeps its trampolines forever -- which is the price
// splice already pays for every trampoline it ever allocates.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include "../os/memory.h"
#include "../os/threads.h"

#include <splice/engine.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#if defined(SPLICE_HAS_X86_64_BACKEND)
#   include "../arch/x86_64/patcher.h"
#endif

namespace splice::engine {

// A group larger than this is not a group, it is a design problem. The
// shipping consumer hooks six methods on one interface.
inline constexpr std::size_t kMaxGroupSites = 64;

// Every thread of the process, bounded. A process with more live threads than
// this cannot be coordinated from one snapshot and is told so.
inline constexpr std::size_t kMaxEnlistedThreads = 4096;

// Spelled with the C ABI's own constants as the initialisers, so there is one
// set of values rather than two that agree today.
enum class LiveStage : int {
    prepare  = SPLICE_LIVE_STAGE_PREPARE,
    enlist   = SPLICE_LIVE_STAGE_ENLIST,
    map      = SPLICE_LIVE_STAGE_MAP,
    migrate  = SPLICE_LIVE_STAGE_MIGRATE,
    write    = SPLICE_LIVE_STAGE_WRITE,
    flush    = SPLICE_LIVE_STAGE_FLUSH,
    resume   = SPLICE_LIVE_STAGE_RESUME,
    complete = SPLICE_LIVE_STAGE_COMPLETE,
};

enum class LiveOperation : int {
    none          = SPLICE_LIVE_OP_NONE,
    prepare_site  = SPLICE_LIVE_OP_PREPARE_SITE,
    enumerate     = SPLICE_LIVE_OP_ENUMERATE,
    suspend       = SPLICE_LIVE_OP_SUSPEND,
    capture_pc    = SPLICE_LIVE_OP_CAPTURE_PC,
    set_pc        = SPLICE_LIVE_OP_SET_PC,
    write_site    = SPLICE_LIVE_OP_WRITE_SITE,
    restore_site  = SPLICE_LIVE_OP_RESTORE_SITE,
    flush_site    = SPLICE_LIVE_OP_FLUSH_SITE,
    resume_thread = SPLICE_LIVE_OP_RESUME_THREAD,
};

enum class LiveState : int {
    installed = SPLICE_LIVE_INSTALLED,  // every site written, every thread resumed
    refused   = SPLICE_LIVE_REFUSED,    // nothing written; threads resumed
    recovered = SPLICE_LIVE_RECOVERED,  // undone before resuming
    lost      = SPLICE_LIVE_LOST,       // not restored -- the caller's problem
};

struct LiveFailure {
    std::size_t   site      = 0;
    LiveStage     stage     = LiveStage::prepare;
    LiveOperation operation = LiveOperation::none;
    std::uint32_t thread_id = 0;
    std::uint32_t os_error  = 0;
};

struct LiveResult {
    LiveState   state = LiveState::refused;
    LiveFailure failure{};
};

// What the transaction does to a thread. Production uses NativeThreadOps and
// nothing else; the template parameter exists so the fault matrix can be
// driven deterministically, which no real process will do on request.
//
// It is a TEST seam, never a consumer's instrumentation hook. That distinction
// is the lesson of the primitive this replaces, whose seam was documented as
// internal and then used from outside anyway.
struct NativeThreadOps {
    os::Enumeration enumerate(std::span<os::ThreadRef> out) const noexcept {
        return os::enumerate_other_threads(out);
    }
    void release(os::ThreadRef ref) const noexcept { os::release_thread(ref); }
    std::uint32_t id(os::ThreadRef ref) const noexcept { return os::thread_id(ref); }
    bool suspend(os::ThreadRef ref, std::uint32_t& prior) const noexcept {
        return os::suspend(ref, prior);
    }
    bool resume(os::ThreadRef ref, std::uint32_t& prior) const noexcept {
        return os::resume(ref, prior);
    }
    bool capture_pc(os::ThreadRef ref, std::uintptr_t& pc) const noexcept {
        return os::capture_pc(ref, pc);
    }
    bool set_pc(os::ThreadRef ref, std::uintptr_t pc) const noexcept {
        return os::set_pc(ref, pc);
    }
    // On the seam rather than called directly, so that a flush failure can be
    // driven deterministically. It is the one operation after the writes have
    // landed, which makes it the only one whose failure cannot be answered by
    // undoing anything -- so it is also the one most worth a test.
    bool flush(void* addr, std::size_t size) const noexcept {
        return os::flush_instruction_cache(addr, size);
    }
    std::uint32_t last_error() const noexcept { return os::last_error(); }
};

#if defined(SPLICE_HAS_X86_64_BACKEND)

using PreparedPlan = arch::x86_64::PreparedStrictPatch;

// One enlisted thread, and the bookkeeping needed to put it back exactly as it
// was found.
struct EnlistedThread {
    os::ThreadRef  ref{};
    std::uint32_t  id = 0;
    std::uint32_t  prior_suspend = 0;
    std::uintptr_t captured_pc = 0;
    std::uintptr_t mapped_pc = 0;
    bool           suspended = false;        // our increment is outstanding
    bool           context_changed = false;  // a set_pc was attempted
};

// One site of the group, prepared and not yet written.
struct GroupSite {
    PreparedPlan  plan;
    void*         target = nullptr;
    unsigned char snapshot[16]{};   // the prologue as prepare found it
    std::size_t   copy_size = 0;    // how much of it the patch displaces
    bool          made_writable = false;
    bool          written = false;
};

// Everything the transaction owns. Allocated in prepare, so the commit path
// allocates nothing at all.
struct LiveGroup {
    std::vector<GroupSite>      sites;
    std::vector<EnlistedThread> threads;
    bool                        prepared = false;
    bool                        committed = false;
};

// A plan whose trampoline a thread or a registry might still point at is never
// freed. Called on every path that does not end `installed`.
inline void retain_all(LiveGroup& group) noexcept {
    for (auto& site : group.sites) site.plan.retain_storage();
}

// Suspend every enlisted thread, write every site, resume. The enlisted set is
// what prepare captured; this cannot see a thread that appeared since. See the
// phase list at
// the top of this file.
//
// `group` must come from a successful prepare, and the caller must already
// have published everything a reader could need behind the new code: the
// original function pointer and the callback. A reader can observe the first
// site's write before this function returns.
template <typename Ops>
LiveResult commit_group(LiveGroup& group, const Ops& ops) noexcept {
    LiveResult result{};

    if (!group.prepared || group.committed || group.sites.empty()) {
        result.failure.stage = LiveStage::prepare;
        result.failure.operation = LiveOperation::none;
        return result;        // refused, nothing touched
    }
    if (group.sites.size() > kMaxGroupSites ||
        group.threads.size() > kMaxEnlistedThreads) {
        result.failure.stage = LiveStage::prepare;
        result.failure.operation = LiveOperation::none;
        return result;
    }
    for (std::size_t s = 0; s < group.sites.size(); ++s) {
        if (!group.sites[s].plan.prepared() || group.sites[s].target == nullptr) {
            result.failure.site = s;
            result.failure.stage = LiveStage::prepare;
            result.failure.operation = LiveOperation::prepare_site;
            return result;
        }
    }
    // No thread may appear twice, or in a state that says it is already ours.
    // A thread suspended twice and resumed once stays suspended forever, and
    // the bookkeeping would not show it: both entries would look resumed.
    //
    // Pairwise, which is O(n^2) in the number of threads in the process -- a
    // few hundred for a game, so tens of thousands of register compares. It
    // happens before the first suspension, it allocates nothing, and the
    // enumeration it is checking already cost about 118 ms.
    for (std::size_t i = 0; i < group.threads.size(); ++i) {
        const auto& thread = group.threads[i];
        if (!thread.ref || thread.id == 0 || thread.suspended ||
            thread.context_changed) {
            result.failure.stage = LiveStage::prepare;
            result.failure.operation = LiveOperation::enumerate;
            result.failure.thread_id = thread.id;
            return result;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (group.threads[j].id == thread.id) {
                result.failure.stage = LiveStage::prepare;
                result.failure.operation = LiveOperation::enumerate;
                result.failure.thread_id = thread.id;
                return result;
            }
        }
    }
    group.committed = true;   // one attempt per group, successful or not

    // ── resume, in reverse order, reporting whether the world came back ────
    //
    // Reverse because that is the order that undoes the suspensions, and the
    // count check is not decoration: anything other than prior + 1 coming back
    // means something outside this transaction is suspending the same threads,
    // and this bookkeeping is no longer the truth.
    const auto resume_all = [&](LiveFailure& failure) noexcept -> bool {
        bool all = true;
        for (std::size_t i = group.threads.size(); i != 0; --i) {
            auto& thread = group.threads[i - 1];
            if (!thread.suspended) continue;
            std::uint32_t prior = 0;
            if (!ops.resume(thread.ref, prior)) {
                failure = LiveFailure{0, LiveStage::resume,
                                      LiveOperation::resume_thread,
                                      thread.id, ops.last_error()};
                all = false;
                continue;     // never decrement twice after a failed call
            }
            thread.suspended = false;
            if (prior != thread.prior_suspend + 1) {
                failure = LiveFailure{0, LiveStage::resume,
                                      LiveOperation::resume_thread, thread.id, 0};
                all = false;
            }
        }
        return all;
    };

    // ── put the world back, then say what that cost ───────────────────────
    //
    // Contexts first and writes second, in the reverse of the order they were
    // applied. A context that cannot be restored is `lost` on its own: there is
    // no point resuming a thread at a program counter that no longer names an
    // instruction, and nothing above here can fix it either.
    const auto undo = [&](LiveFailure failure, LiveState if_clean) noexcept -> LiveResult {
        bool restored = true;
        for (std::size_t i = group.sites.size(); i != 0; --i) {
            auto& site = group.sites[i - 1];
            if (!site.written) continue;
            if (!site.plan.restore_write()) {
                restored = false;
                continue;
            }
            site.written = false;
        }
        for (auto& thread : group.threads) {
            if (!thread.context_changed) continue;
            if (!ops.set_pc(thread.ref, thread.captured_pc)) {
                restored = false;
                continue;
            }
            thread.context_changed = false;
        }
        if (!restored) {
            // Deliberately does NOT resume. A thread whose program counter
            // could not be put back would resume at an address that is no
            // longer an instruction, and nothing above here can fix that
            // either. Leaving it suspended is the only honest answer, and it
            // is what `lost` means: the caller has to decide, and the module
            // must not be unloaded.
            retain_all(group);
            return LiveResult{LiveState::lost, failure};
        }
        LiveFailure resume_failure{};
        if (!resume_all(resume_failure)) {
            retain_all(group);
            return LiveResult{LiveState::lost, resume_failure};
        }
        return LiveResult{if_clean, failure};
    };

    // ── enlist ────────────────────────────────────────────────────────────
    for (auto& thread : group.threads) {
        if (!ops.suspend(thread.ref, thread.prior_suspend)) {
            return undo(LiveFailure{0, LiveStage::enlist, LiveOperation::suspend,
                                    thread.id, ops.last_error()},
                        LiveState::refused);
        }
        thread.suspended = true;
    }

    // ── map ───────────────────────────────────────────────────────────────
    //
    // Every program counter is read and mapped before the first one is moved,
    // so a thread that cannot be mapped costs nothing but the suspension.
    for (auto& thread : group.threads) {
        if (!ops.capture_pc(thread.ref, thread.captured_pc)) {
            return undo(LiveFailure{0, LiveStage::map, LiveOperation::capture_pc,
                                    thread.id, ops.last_error()},
                        LiveState::refused);
        }
        thread.mapped_pc = thread.captured_pc;
        for (std::size_t s = 0; s < group.sites.size(); ++s) {
            std::uintptr_t mapped = thread.mapped_pc;
            if (!group.sites[s].plan.map_ip(thread.mapped_pc, mapped)) {
                return undo(LiveFailure{s, LiveStage::map, LiveOperation::capture_pc,
                                        thread.id, 0},
                            LiveState::refused);
            }
            thread.mapped_pc = mapped;
        }
    }

    // ── migrate ───────────────────────────────────────────────────────────
    for (auto& thread : group.threads) {
        if (thread.mapped_pc == thread.captured_pc) continue;
        // Marked before the call, not after: a set_pc that reports failure is
        // not assumed to have changed nothing.
        thread.context_changed = true;
        if (!ops.set_pc(thread.ref, thread.mapped_pc)) {
            return undo(LiveFailure{0, LiveStage::migrate, LiveOperation::set_pc,
                                    thread.id, ops.last_error()},
                        LiveState::recovered);
        }
    }

    // ── write ─────────────────────────────────────────────────────────────
    for (std::size_t s = 0; s < group.sites.size(); ++s) {
        auto& site = group.sites[s];
        if (!site.plan.commit_write()) {
            return undo(LiveFailure{s, LiveStage::write, LiveOperation::write_site,
                                    0, ops.last_error()},
                        LiveState::recovered);
        }
        site.written = true;
    }

    // ── flush ─────────────────────────────────────────────────────────────
    //
    // Each site with its own length. The primitive this replaces passed a
    // hardcoded 5, which was the patch length of the day.
    for (std::size_t s = 0; s < group.sites.size(); ++s) {
        auto& site = group.sites[s];
        if (!ops.flush(site.target, site.plan.copy_size())) {
            // The writes are in memory but the instruction stream may not have
            // seen them. Resuming a thread now is resuming it into an unknown
            // mixture of old and new instructions, so it stays suspended and
            // this is `lost`. Same answer as the primitive this replaces gave,
            // for the same reason.
            retain_all(group);
            return LiveResult{LiveState::lost,
                              LiveFailure{s, LiveStage::flush,
                                          LiveOperation::flush_site, 0,
                                          ops.last_error()}};
        }
    }

    // ── resume ────────────────────────────────────────────────────────────
    //
    // Past this point the writes are live and visible, so a thread that will
    // not resume cannot be answered by undoing anything. It is `lost`, which
    // is the honest word for it, and it is the one outcome the caller must
    // decide something about.
    LiveFailure resume_failure{};
    if (!resume_all(resume_failure)) {
        retain_all(group);
        return LiveResult{LiveState::lost, resume_failure};
    }
    return LiveResult{LiveState::installed, LiveFailure{}};
}

#endif  // SPLICE_HAS_X86_64_BACKEND

} // namespace splice::engine
