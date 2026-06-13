// ─── ARM64 atomic patch sequence ──────────────────────────────────────────
//
// Replaces the unsafe `memcpy(target, hook_jump, 16)` from Phase 1 with the
// architecturally-correct write order per the ARMv8-A Architecture Reference
// Manual §B2.2.1 ("Single-copy atomicity") and the Android ART runtime's
// concurrent-modify code-patching pattern.
//
// Why memcpy is unsafe:
//   - 16-byte non-atomic store; another thread's instruction fetch can
//     observe a partially-patched prologue → SIGILL on lucky days, silent
//     misexecution on unlucky days
//   - No memory barriers between literal-pool data and branch instruction
//   - No instruction-cache maintenance — other CPUs keep prefetching the
//     stale bytes
//
// What we install (16-byte indirect branch):
//
//   [target+0]   ldr  x17, #8       ; load x17 from [PC+8]  (4 bytes, instruction)
//   [target+4]   br   x17           ; branch to whatever's in x17 (4 bytes)
//   [target+8]   .quad new_func     ; literal pool, 8-byte address
//
// Atomic install order:
//
//   1. Write the literal at [target+8..15] (8 bytes)        — data, not yet observable
//   2. Write `br x17` at [target+4..7]                      — instruction, not yet reachable
//   3. DMB ISH                                              — make 1+2 globally visible
//   4. ATOMIC 32-bit write of `ldr x17, #8` at [target+0]   — this is THE moment
//   5. DC CVAU + DSB ISH + IC IVAU + DSB ISH + ISB         — I-cache coherency
//
// Step 4 is the only step that's observable to executing threads. ARMv8
// guarantees that a 4-byte aligned instruction-word write is atomic with
// respect to instruction fetch — every concurrent fetcher sees either the
// old word or the new word, never a torn middle. So before step 4, all
// threads execute the original prologue; after step 4 (and the I-cache
// flush), all threads execute the new indirect branch.
//
// Reference reading: ARMv8 ARM §B2.2.1, §D7.5.6; ART runtime's
// `Patchoat` and `Atomic::CompareAndSetStrongRelaxed` for code patching.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstddef>

namespace splice::arch::arm64 {

// Atomically install a 16-byte indirect branch at `target` that jumps to
// `new_func`. `target` MUST be 4-byte aligned (always true for function
// entries) and the [target..target+16) range MUST already be RW-mapped
// (caller responsibility — typically via splice::os::make_executable_writable).
//
// Returns: void. Cannot fail under correct usage; alignment / mapping are
// pre-conditions.
void atomic_install_indirect_branch(void* target, void* new_func);

// Reverse of `atomic_install_indirect_branch` — restore the original 16
// bytes at `target` from `pre_hook_bytes`. FR-013 Tier 2 disable path.
//
// Restore order (mirror of install, run backwards):
//   1. Write pre_hook_bytes[4..15] non-atomically into [target+4..15] —
//      these bytes are unreachable while the patched [target+0..3]
//      (`ldr x17, #8`) still redirects flow.
//   2. DMB ISH — make step 1 globally visible before the instruction-word
//      store in step 3 lands.
//   3. Atomic 32-bit store of saved [target+0..3] → [target+0..3]. Single-
//      copy atomicity (ARMv8 ARM §B2.2.1) means every concurrent fetcher
//      sees either the patched word or the original word, never partial.
//   4. DC CVAU + DSB ISH + IC IVAU + DSB ISH + ISB over [target..target+16).
//
// Preconditions:
//   - target is 4-byte aligned (always true for function entries)
//   - [target..target+16) is currently RW-mapped
//   - pre_hook_bytes contains exactly the 16 bytes that were at target
//     before install_inline_patch fired
//
// Returns: void. Cannot fail under correct usage.
void atomic_disable_indirect_branch(void* target,
                                    const unsigned char* pre_hook_bytes);

} // namespace splice::arch::arm64
