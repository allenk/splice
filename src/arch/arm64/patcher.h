// ─── ARM64 inline-patch installer ──────────────────────────────────────────
//
// Given a target function address and a replacement function pointer,
// allocates a trampoline, relocates the first N bytes of the prologue into
// it (with PC-relative fixup), and overwrites the prologue with an indirect
// branch to `new_func`.
//
// Atomicity (Phase 4.5a / FR-011): the prologue overwrite uses
// `atomic_install_indirect_branch` (DMB ISH + atomic 32-bit instruction
// store + DC CVAU + IC IVAU + ISB) per ARMv8 ARM §B2.2.1.
//
// Pre-patch callback (Phase 4.5d hardening): `on_trampoline_ready` is
// invoked AFTER the trampoline is built and ready BUT BEFORE the atomic
// patch lands. This is the only safe point to register the trampoline
// with downstream lookup tables (e.g. OriginalRegistry) so that any
// thread observing the new patched prologue can immediately resolve a
// valid trampoline pointer.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

namespace splice::arch::arm64 {

// Pre-patch hook signature. Called once with the freshly-built trampoline
// pointer; receives the user's opaque pointer for context.
using PrePatchFn = void (*)(void* trampoline, void* user_data);

// Install an inline patch at `target` that redirects to `new_func`.
//
// On success:
//   - `*original_func` (if non-null) is set to a trampoline address that
//     invokes the original function behaviour.
//   - `on_trampoline_ready` (if non-null) is invoked with that same
//     trampoline pointer BEFORE the atomic patch is applied. Use this
//     to update any global registry that other threads will consult.
//   - `pre_hook_bytes_out` (if non-null, ≥16-byte buffer) is filled with
//     the original 16 bytes of `target` BEFORE the patch lands. Disable
//     can replay these bytes via `atomic_disable_indirect_branch`.
//     `*pre_hook_byte_len_out` is set to 16 on success.
//   - returns `target` (the patched location).
//
// On failure, returns nullptr and leaves `target` and `*original_func`
// untouched. `on_trampoline_ready` will not have been invoked.
void* install_inline_patch(void* target, void* new_func, void** original_func,
                           PrePatchFn on_trampoline_ready = nullptr,
                           void* user_data = nullptr,
                           unsigned char* pre_hook_bytes_out = nullptr,
                           unsigned int* pre_hook_byte_len_out = nullptr);

} // namespace splice::arch::arm64
