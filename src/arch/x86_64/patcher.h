// ─── x86_64 inline-patch installer ────────────────────────────────────────
//
// Mirrors `src/arch/arm64/patcher.h`. Chooses between:
//   - 5-byte relative jmp (E9 rel32)           — when trampoline is within ±2GB
//   - 14-byte absolute jmp (FF 25 + abs64)     — unconditional fallback
//
// Atomicity (Phase 4.5b): the 5-byte case uses `atomic_install_jmp_rel32`
// (aligned 8-byte mov per Intel SDM §8.1.1). The 14-byte fallback is
// non-atomic with a SPLICE_LOGW; documented as a residual hazard.
//
// Pre-patch callback (Phase 4.5d hardening): same semantics as the ARM64
// version — invoked with the trampoline pointer before the atomic patch
// lands, so callers can update global state (e.g. OriginalRegistry) and
// avoid the race where threads observe the new patch before the
// trampoline pointer is registered.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include "atomic_patch.h"
#include <cstddef>
#include <cstdint>

namespace splice::arch::x86_64 {

using PrePatchFn = void (*)(void* trampoline, void* user_data);

// Internal transaction building block, NOT a live-safe public hook API.
// Preparation allocates/decodes/flushes but never changes target bytes or
// permissions. Caller keeps the target alive and excludes other patch writers.
// Before commit_write, caller must make the containing word writable, exclude
// execution, migrate pending IPs and publish original(). Afterward caller must
// flush the target instruction cache before resuming and restore permissions.
// commit_write performs no allocation, OS calls, logging or callback. A stale
// snapshot is refused; caller then rolls back any context/pointer publication
// BEFORE destroying this object. Failure of that rollback requires retention.
// Committed storage is process-lifetime, matching the existing hook ownership.
class PreparedStrictPatch {
public:
    PreparedStrictPatch() = default;
    ~PreparedStrictPatch();
    PreparedStrictPatch(const PreparedStrictPatch&) = delete;
    PreparedStrictPatch& operator=(const PreparedStrictPatch&) = delete;
    bool prepare(void* target, void* replacement, const unsigned char* expected,
                 std::size_t expected_size);
    void* original() const noexcept { return storage_; }
    std::size_t copy_size() const noexcept { return copy_size_; }
    void* target() const noexcept { return target_; }
    bool prepared() const noexcept { return storage_ && !committed_; }
    // Recovery could leave a context pointing at unpublished storage. Retain
    // it permanently and prevent further commits; this is not commit success.
    void retain_storage() noexcept { committed_ = true; }
    // False means invalid plan or interior non-boundary IP; output unchanged.
    // IPs outside the displaced prefix are returned unchanged.
    bool map_ip(std::uintptr_t ip, std::uintptr_t& mapped) const noexcept;
    bool commit_write() noexcept;
private:
    void* target_{};
    void* storage_{};
    PreparedRel32Patch write_{};
    unsigned char expected_[16]{};
    unsigned char source_offsets_[5]{};
    unsigned char relocated_offsets_[5]{};
    std::size_t boundary_count_{};
    std::size_t copy_size_{};
    bool committed_{};
};

// Install an inline patch at `target` that redirects to `new_func`.
// `*original_func` is set to a trampoline that invokes the original
// behaviour. `on_trampoline_ready` (optional) is called with that same
// pointer before the atomic patch is applied.
//
// FR-013 Tier 2 support — `pre_hook_bytes_out` (nullable) receives a
// snapshot of the first `*pre_hook_byte_len_out` bytes of `target` taken
// BEFORE the patch lands. Disable can later replay these bytes via
// `atomic_disable_inline`. The buffer must be at least 16 bytes; the
// installer caps copy_size at 16 and only fills the buffer when the
// prologue fits. If copy_size > 16, len is set to 0 (disable will fail).
//
// Returns `target` on success, nullptr on failure.
void* install_inline_patch(void* target, void* new_func, void** original_func,
                           PrePatchFn on_trampoline_ready = nullptr,
                           void* user_data = nullptr,
                           unsigned char* pre_hook_bytes_out = nullptr,
                           unsigned int* pre_hook_byte_len_out = nullptr,
                           const unsigned char* strict_expected_bytes = nullptr);

} // namespace splice::arch::x86_64
