#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <algorithm>
#include <array>
#include <span>
#include "../../arch/x86_64/patcher.h"

namespace splice::os::win32 {
inline constexpr std::size_t max_enlisted_threads = 4096;
enum class TransactionOutcome { refused, installed, recovery_required };
enum class TransactionStage { input, identity, suspend, context, boundary, migrate,
                              commit, rollback, flush, resume, complete };
struct TransactionResult {
    TransactionOutcome outcome;
    TransactionStage stage;
};
struct ThreadSlot {
    HANDLE handle{};  // Borrowed, pre-opened; never closed here.
    DWORD id{};
    DWORD prior_suspend{};
    DWORD resume_result{};
    CONTEXT saved{};
    std::uintptr_t mapped_ip{};
    bool suspended{};  // Our increment is still outstanding.
    bool context_attempted{};
};
struct NativeThreadOps {
    DWORD thread_id(HANDLE h) const noexcept { return GetThreadId(h); }
    DWORD process_id(HANDLE h) const noexcept { return GetProcessIdOfThread(h); }
    DWORD current_thread() const noexcept { return GetCurrentThreadId(); }
    DWORD current_process() const noexcept { return GetCurrentProcessId(); }
    DWORD suspend(HANDLE h) const noexcept { return SuspendThread(h); }
    DWORD resume(HANDLE h) const noexcept { return ResumeThread(h); }
    bool get(HANDLE h, CONTEXT& c) const noexcept { return GetThreadContext(h, &c) != FALSE; }
    bool set(HANDLE h, const CONTEXT& c) const noexcept { return SetThreadContext(h, &c) != FALSE; }
    bool flush(void* target) const noexcept {
        return FlushInstructionCache(GetCurrentProcess(), target, 5) != FALSE;
    }
};

// Internal enlisted-thread primitive, not an arbitrary-process live-safe API.
// Caller guarantees: no unenlisted invocation/new threads/other patch writers;
// no concurrent suspend/resume on enlisted threads; target is writable;
// plan, publication, slots, handles and replacement module
// remain valid. Permissions are restored AFTER this call outside the frozen
// path. Replacement reads publication with acquire. Inputs are single-owner.
// Slots must be fresh; result preserves outstanding suspensions for recovery.
// recovery_required forbids DLL unload and requires explicit caller recovery.
// Ops is a compile-time test seam; production uses only NativeThreadOps.
template<class Ops = NativeThreadOps>
TransactionResult commit_enlisted(arch::x86_64::PreparedStrictPatch& plan,
    std::span<ThreadSlot> slots, std::atomic<void*>& publication, Ops& ops) noexcept {
    static_assert(std::atomic<void*>::is_always_lock_free);
    if (!plan.prepared() || slots.empty() || slots.size() > max_enlisted_threads || publication.load())
        return {TransactionOutcome::refused, TransactionStage::input};
    const auto self = ops.current_thread();
    const auto process = ops.current_process();
    // ID-only scratch preserves slot order and recovery bookkeeping. All
    // validation/sorting occurs before the first suspension; no heap allocation.
    std::array<DWORD, max_enlisted_threads> ids;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        auto& slot = slots[i];
        if (!slot.handle || slot.suspended || slot.context_attempted)
            return {TransactionOutcome::refused, TransactionStage::input};
        slot.id = ops.thread_id(slot.handle);
        if (!slot.id || slot.id == self || ops.process_id(slot.handle) != process)
            return {TransactionOutcome::refused, TransactionStage::identity};
        ids[i] = slot.id;
    }
    const auto end = ids.begin() + slots.size();
    std::sort(ids.begin(), end);
    if (std::adjacent_find(ids.begin(), end) != end) {
        return {TransactionOutcome::refused, TransactionStage::identity};
    }
    const auto resume_all = [&]() noexcept {
        bool ok = true;
        for (std::size_t i = slots.size(); i != 0; --i) {
            auto& slot = slots[i - 1];
            if (!slot.suspended) continue;
            slot.resume_result = ops.resume(slot.handle);
            if (slot.resume_result == MAXDWORD) { ok = false; continue; }
            slot.suspended = false;  // Never decrement twice after a successful call.
            if (slot.resume_result != slot.prior_suspend + 1) ok = false;
        }
        return ok;
    };
    const auto abort = [&](TransactionStage reason) noexcept -> TransactionResult {
        bool restored = true;
        for (std::size_t i = slots.size(); i != 0; --i) {
            auto& slot = slots[i - 1];
            if (!slot.context_attempted) continue;
            if (!ops.set(slot.handle, slot.saved)) restored = false;
            else slot.context_attempted = false;
        }
        if (!restored) {
            plan.retain_storage();
            return {TransactionOutcome::recovery_required, TransactionStage::rollback};
        }
        if (!resume_all()) {
            plan.retain_storage();
            return {TransactionOutcome::recovery_required, TransactionStage::resume};
        }
        return {TransactionOutcome::refused, reason};
    };
    for (auto& slot : slots) {
        slot.prior_suspend = ops.suspend(slot.handle);
        if (slot.prior_suspend == MAXDWORD) return abort(TransactionStage::suspend);
        slot.suspended = true;
    }
    // Validate every IP before attempting the first context mutation.
    for (auto& slot : slots) {
        slot.saved = {};
        slot.saved.ContextFlags = CONTEXT_CONTROL;
        if (!ops.get(slot.handle, slot.saved)) return abort(TransactionStage::context);
        if (!plan.map_ip(slot.saved.Rip, slot.mapped_ip)) return abort(TransactionStage::boundary);
    }
    for (auto& slot : slots) {
        if (slot.mapped_ip == slot.saved.Rip) continue;
        CONTEXT migrated = slot.saved;
        migrated.Rip = slot.mapped_ip;
        // A failed set is not assumed side-effect-free.
        slot.context_attempted = true;
        if (!ops.set(slot.handle, migrated)) return abort(TransactionStage::migrate);
    }
    publication.store(plan.original(), std::memory_order_release);
    if (!plan.commit_write()) {
        publication.store(nullptr, std::memory_order_release);
        return abort(TransactionStage::commit);
    }
    if (!ops.flush(plan.target()))
        return {TransactionOutcome::recovery_required, TransactionStage::flush};
    if (!resume_all())
        return {TransactionOutcome::recovery_required, TransactionStage::resume};
    return {TransactionOutcome::installed, TransactionStage::complete};
}
}
