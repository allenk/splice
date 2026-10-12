// ─── The group transaction — the C ABI around it ───────────────────────────
//
// Two calls rather than one, and the split is the whole point: everything that
// allocates, opens a handle or takes a lock happens in prepare, and the caller
// publishes what a reader will need in between. By the time commit suspends
// anything, there is nothing left to do but write.
//
// It is also why page protection is changed in prepare and put back in
// release, not around the write. A VirtualProtect while another thread is
// suspended is a kernel operation on the address space, and a suspended thread
// holding the wrong kernel lock turns it into a wait that never ends. The
// primitive this replaces said the same thing in its own contract -- "target is
// writable; permissions are restored AFTER this call outside the frozen path" --
// it just left both duties to its caller.
// ───────────────────────────────────────────────────────────────────────────
#ifndef NOMINMAX
#   define NOMINMAX
#endif

#include "live_install.h"

#include "../os/memory.h"
#include "install_lock.h"
#include "patch_sites.h"

#include <splice/log.h>

#include <cstring>
#include <mutex>
#include <new>

namespace splice::engine {

// Shared with engine.cpp, where every other install and disable serialises.
std::mutex& install_mutex();

namespace {

void fill_outcome(splice_live_outcome* out, LiveState state,
                  const LiveFailure& failure, unsigned int threads = 0) noexcept {
    if (out == nullptr) return;
    out->state = static_cast<int>(state);
    out->site = static_cast<unsigned int>(failure.site);
    out->stage = static_cast<int>(failure.stage);
    out->operation = static_cast<int>(failure.operation);
    out->thread_id = failure.thread_id;
    out->os_error = failure.os_error;
    out->threads = threads;
}

#if defined(SPLICE_HAS_X86_64_BACKEND)

// The window every site's prologue needs to be writable through. Sixteen
// because that is what the strict snapshot covers, and the patch itself is
// five of those bytes.
constexpr std::size_t kSiteWindow = 16;

void restore_site_protection(LiveGroup& group) noexcept {
    for (auto& site : group.sites) {
        if (!site.made_writable) continue;
        os::restore_executable(site.target, kSiteWindow);
        site.made_writable = false;
    }
}

void release_threads(LiveGroup& group, const NativeThreadOps& ops) noexcept {
    for (auto& thread : group.threads) {
        if (thread.ref) ops.release(thread.ref);
        thread.ref = os::ThreadRef{};
    }
    group.threads.clear();
}

#endif  // SPLICE_HAS_X86_64_BACKEND

} // namespace
} // namespace splice::engine

extern "C" {

#if defined(SPLICE_HAS_X86_64_BACKEND) && defined(SPLICE_HAS_WIN32_OS)

int splice_live_supported(void) { return 1; }

void* splice_live_prepare(const splice_live_site* sites, unsigned int count,
                          void** originals_out, splice_live_outcome* out) {
    using namespace splice;
    using namespace splice::engine;

    const auto refuse = [&](LiveOperation operation, std::size_t site,
                            std::uint32_t os_error) -> void* {
        fill_outcome(out, LiveState::refused,
                     LiveFailure{site, LiveStage::prepare, operation, 0, os_error});
        return nullptr;
    };

    if (sites == nullptr || count == 0 || count > kMaxGroupSites) {
        return refuse(LiveOperation::none, 0, 0);
    }
    if (!os::kCoordinationSupported) {
        SPLICE_LOGE("splice_live_prepare: this OS has no thread coordination");
        return refuse(LiveOperation::enumerate, 0, 0);
    }

    const NativeThreadOps ops;
    auto group = new (std::nothrow) LiveGroup();
    if (group == nullptr) return refuse(LiveOperation::none, 0, 0);

    // Every target made writable, snapshotted and planned. Nothing is written.
    group->sites.resize(count);
    for (unsigned int i = 0; i < count; ++i) {
        auto& site = group->sites[i];

        // The address a caller hands us is often not the function. An
        // incremental-linked build routes a cross-translation-unit `&function`
        // through an ILT thunk -- a bare `E9 rel32` -- and an import lands on
        // an `FF 25` thunk; patching the thunk installs a hook almost nothing
        // calls, and the planner refuses a jump as an unrelocatable prologue
        // anyway. The single-site path has unwrapped since Phase 4; this one
        // did not until a contention runner pointed it at a target in another
        // translation unit and every install refused.
        //
        // It stops at anything splice itself wrote, which is the whole reason
        // the site registry below has to be kept in step: an installed patch is
        // an `E9` too, and following one walks into our own relay.
        site.target = splice::engine::unwrap_jump_stub(sites[i].target);
        if (site.target == nullptr || sites[i].replacement == nullptr) {
            restore_site_protection(*group);
            delete group;
            return refuse(LiveOperation::prepare_site, i, 0);
        }

        // Already ours. This check survives mutation, and that is the honest
        // description of it: removing it changes nothing a test can see,
        // because the planner refuses the same input one step later -- an
        // installed `E9` is not a relocatable prologue. It stays because the
        // two refusals do not mean the same thing to whoever reads the log.
        // "Already patched by splice; chaining is not implemented" is a
        // decision; "prologue not relocatable" is a measurement that happens
        // to coincide with it, and would send the next reader looking at the
        // wrong thing.
        if (splice::engine::is_own_patch_site(site.target)) {
            SPLICE_LOGE("splice_live_prepare: site %u at %p is already patched "
                        "by splice; chaining is not implemented", i, site.target);
            restore_site_protection(*group);
            delete group;
            return refuse(LiveOperation::prepare_site, i, 0);
        }

        // Permission first, and then the read -- in that order, because the
        // address came from a caller and may not name mapped memory at all.
        // Reading it first to snapshot the prologue would fault on a bad
        // address instead of refusing; VirtualProtect and mprotect both fail
        // cleanly on one. It is also the order S-13 settled for the single-site
        // path: acquire permission before doing anything that can be observed.
        if (!os::make_executable_writable(site.target, kSiteWindow)) {
            SPLICE_LOGE("splice_live_prepare: site %u at %p cannot be made "
                        "writable; it may not be mapped executable code",
                        i, site.target);
            restore_site_protection(*group);
            delete group;
            return refuse(LiveOperation::prepare_site, i, os::last_error());
        }
        site.made_writable = true;

        std::memcpy(site.snapshot, site.target, kSiteWindow);
        if (sites[i].expected != nullptr &&
            std::memcmp(site.snapshot, sites[i].expected, kSiteWindow) != 0) {
            SPLICE_LOGE("splice_live_prepare: site %u at %p does not hold the "
                        "bytes the caller expected", i, site.target);
            restore_site_protection(*group);
            delete group;
            return refuse(LiveOperation::prepare_site, i, 0);
        }

        if (!site.plan.prepare(site.target, sites[i].replacement,
                               site.snapshot, kSiteWindow)) {
            SPLICE_LOGE("splice_live_prepare: site %u at %p could not be planned "
                        "(prologue not relocatable, or the patch does not fit one "
                        "aligned quadword)", i, site.target);
            restore_site_protection(*group);
            delete group;
            return refuse(LiveOperation::prepare_site, i, 0);
        }
        site.copy_size = site.plan.copy_size();
        if (originals_out != nullptr) originals_out[i] = site.plan.original();
    }

    // Open every other thread the OS names right now -- which is not provably
    // every thread of the process, and nothing here can tell the difference.
    // Once, here, because a thread
    // snapshot costs about 118 ms -- Toolhelp walks every thread on the
    // machine, not this process's -- and because an incomplete thread set is
    // the one thing coordination cannot be built on.
    std::vector<os::ThreadRef> refs(kMaxEnlistedThreads);
    const auto enumeration = ops.enumerate(std::span<os::ThreadRef>(refs));
    group->threads.resize(enumeration.count);
    for (std::size_t i = 0; i < enumeration.count; ++i) {
        group->threads[i].ref = refs[i];
        group->threads[i].id = ops.id(refs[i]);
    }
    if (enumeration.status != os::EnumerateStatus::ok) {
        SPLICE_LOGE("splice_live_prepare: thread enumeration is not usable "
                    "(status=%d, %zu opened)", static_cast<int>(enumeration.status),
                    enumeration.count);
        release_threads(*group, ops);
        restore_site_protection(*group);
        delete group;
        return refuse(LiveOperation::enumerate, 0, os::last_error());
    }

    group->prepared = true;
    fill_outcome(out, LiveState::refused, LiveFailure{});   // not committed yet
    return group;
}

void splice_live_commit(void* handle, splice_patch_record* records_out,
                        splice_live_outcome* out) {
    using namespace splice;
    using namespace splice::engine;

    if (handle == nullptr) {
        fill_outcome(out, LiveState::refused, LiveFailure{});
        return;
    }
    auto* group = static_cast<LiveGroup*>(handle);
    const NativeThreadOps ops;

    // The same lock every other install and disable takes, and taken here --
    // before the first suspension -- on purpose. Page protection is not
    // reference counted, so two installs on one page can revoke each other's
    // write access; and nothing inside the frozen window may wait for a lock a
    // suspended thread might hold.
    LiveResult result{};
    {
        const std::lock_guard<std::mutex> installing(install_mutex());
        result = commit_group(*group, ops);
    }

    if (result.state == LiveState::installed) {
        // Register every site, for the same reason the single-site path does:
        // the next install that unwraps a jump has to stop at ours instead of
        // following it into the relay. splice_disable forgets the site when the
        // original bytes go back.
        for (auto& site : group->sites) {
            splice::engine::remember_patched_site(site.target,
                                                  site.plan.original());
        }
    }
    if (result.state == LiveState::installed && records_out != nullptr) {
        for (std::size_t i = 0; i < group->sites.size(); ++i) {
            auto& site = group->sites[i];
            records_out[i] = splice_patch_record{};
            records_out[i].strategy = SPLICE_PATCH_STRATEGY_INLINE;
            records_out[i].hook_site = site.target;
            records_out[i].trampoline = site.plan.original();
            std::memcpy(records_out[i].pre_hook_bytes, site.snapshot,
                        sizeof(records_out[i].pre_hook_bytes));
            records_out[i].pre_hook_byte_len =
                static_cast<unsigned int>(site.copy_size);
        }
    }
    if (result.state == LiveState::installed) {
        SPLICE_LOGI("splice_live_commit: %zu site(s) installed under one "
                    "suspension of %zu thread(s)",
                    group->sites.size(), group->threads.size());
    } else {
        SPLICE_LOGE("splice_live_commit: state=%d site=%zu stage=%d op=%d tid=%u err=%u",
                    static_cast<int>(result.state), result.failure.site,
                    static_cast<int>(result.failure.stage),
                    static_cast<int>(result.failure.operation),
                    result.failure.thread_id, result.failure.os_error);
    }
    fill_outcome(out, result.state, result.failure,
                 static_cast<unsigned int>(group->threads.size()));
}

void splice_live_release(void* handle) {
    using namespace splice::engine;
    if (handle == nullptr) return;
    auto* group = static_cast<LiveGroup*>(handle);
    const NativeThreadOps ops;
    // Thread references and page protection, both of which prepare took out.
    // Never the trampolines: a group that failed has retained them, and a group
    // that succeeded is using them.
    release_threads(*group, ops);
    restore_site_protection(*group);
    delete group;
}

#else   // no x86_64 backend, or no win32 OS layer

int splice_live_supported(void) { return 0; }

void* splice_live_prepare(const splice_live_site*, unsigned int,
                          void**, splice_live_outcome* out) {
    SPLICE_LOGW_ONCE("splice_live_prepare: the coordinated tier needs the x86_64 "
                     "backend and an OS that can suspend a thread; this build has "
                     "one or neither");
    splice::engine::fill_outcome(out, splice::engine::LiveState::refused,
                                 splice::engine::LiveFailure{});
    return nullptr;
}

void splice_live_commit(void*, splice_patch_record*, splice_live_outcome* out) {
    splice::engine::fill_outcome(out, splice::engine::LiveState::refused,
                                 splice::engine::LiveFailure{});
}

void splice_live_release(void*) {}

#endif

} // extern "C"
