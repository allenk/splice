// ─── x86_64 atomic patch sequence ─────────────────────────────────────────
//
// Replaces the unsafe Phase 1 `memcpy(target, hook, 5)` with an aligned
// 8-byte atomic store, per Intel SDM Vol 3A §8.1.1 ("Guaranteed Atomic
// Operations") — a quadword aligned on an 8-byte boundary is read/written
// atomically with respect to instruction fetch.
//
// What we install (5-byte relative jump):
//
//   [target+0]   E9 rel32        (5 bytes: 1-byte opcode + 4-byte rel32)
//   [target+5]   <preserved>     (3 bytes of original prologue, never executed
//                                 because target+0..4 redirects flow first)
//
// Atomic install order:
//
//   1. Read original 8 bytes at target (data load)
//   2. Construct new 8-byte word in registers:
//        bytes 0..4 = E9 rel32      (the new jump)
//        bytes 5..7 = original 5..7 (preserved)
//   3. Atomic 8-byte write at target (single aligned `mov qword ptr`)
//
// Why this works:
//   - x86 has Total Store Order (TSO): the aligned 8-byte store appears
//     atomic to all observers, no extra memory barriers needed
//   - Intel SDM §8.1.3 (cross-modifying code): on modern CPUs (Pentium 4+),
//     an aligned 8-byte instruction-stream write is atomic with respect
//     to the I-cache + pipeline; no explicit serialization required on
//     the modifying CPU. (Pre-Pentium 4 advice was different; we don't
//     target those.)
//   - bytes 5..7 are dead from execution standpoint after the patch lands
//     (target+0 is now `E9 rel32` redirecting flow before reaching them);
//     preserving them avoids ever creating a "torn" intermediate where
//     bytes 5..7 contain partial garbage
//
// Constraint: target MUST be 8-byte aligned. Function entries on x86_64
// are typically 16-byte aligned by both MSVC and Clang/GCC default
// codegen, so this holds for the overwhelming majority of hook sites.
// Returns false if alignment isn't met so the caller can fall back to
// non-atomic install (with a logged warning).
// ───────────────────────────────────────────────────────────────────────────
#pragma once

namespace splice::arch::x86_64 {

// Atomically install a 5-byte `E9 rel32` jump at `target` redirecting to
// `new_func`. The caller must have already arranged for [target..target+8)
// to be RW-mapped (typically via splice::os::make_executable_writable).
//
// Returns true on success.
// Returns false if any of these preconditions are violated:
//   - target is not 8-byte aligned
//   - displacement(new_func − (target + 5)) doesn't fit in int32
//
// On false, the caller is responsible for falling back to a non-atomic
// install (with appropriate diagnostic logging).
bool atomic_install_jmp_rel32(void* target, void* new_func) noexcept;

// Reverse of `atomic_install_jmp_rel32` — restore the first `len` bytes
// at `target` from `pre_hook_bytes`. FR-013 Tier 2 disable path.
//
// Restore order (mirrors install symmetry):
//   1. If len > 8: memcpy(target+8, pre_hook_bytes+8, len-8) — these
//      bytes are unreachable while the active patch at [0..4] redirects
//      flow, so a non-atomic copy is safe.
//   2. Build the new 8-byte word from pre_hook_bytes[0..7] (current
//      bytes 5..7 may already match — if so, the word is identical to
//      saved bytes; if install used the abs64 fallback, bytes 5..7 were
//      part of the active jmp and MUST come from the saved copy).
//   3. Aligned 8-byte atomic store at [target+0..7]. After this lands,
//      flow follows the original prologue again.
//
// Preconditions:
//   - len <= 16 (the record buffer cap)
//   - [target..target+len) is currently RW-mapped
//   - pre_hook_bytes contains the exact bytes that were originally at
//     target before install_inline_patch fired
//
// If target is 8-byte aligned, the restore uses the aligned quadword atomic
// store. If not (rare — tightly-packed tiny functions in some -O0 ELF
// builds), it falls back to a non-atomic memcpy restore with a SPLICE_LOGW,
// symmetric with the non-atomic install fallback in patcher.cpp.
//
// Returns true on success, false only if len violates the contract.
bool atomic_disable_inline(void* target,
                           const unsigned char* pre_hook_bytes,
                           unsigned int len) noexcept;

} // namespace splice::arch::x86_64
