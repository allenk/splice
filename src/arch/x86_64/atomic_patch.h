// ─── x86_64 atomic patch sequence ─────────────────────────────────────────
//
// Replaces the unsafe Phase 1 `memcpy(target, hook, 5)` with an aligned
// 8-byte atomic store, per Intel SDM Vol 3A §8.1.1 ("Guaranteed Atomic
// Operations") — a quadword aligned on an 8-byte boundary supports an atomic
// memory update. This does not establish safe concurrent instruction execution.
//
// What we install (5-byte relative jump):
//
//   [target+0]   E9 rel32        (5 bytes: 1-byte opcode + 4-byte rel32)
//   [target+5]   <preserved>     (remaining original prologue bytes)
//
// Atomic install order:
//
//   1. Read the containing aligned quadword (data load)
//   2. Construct new 8-byte word in registers:
//        bytes 0..4 = E9 rel32      (the new jump)
//        bytes 5..7 = original 5..7 (preserved)
//   3. Compare-exchange the containing quadword, preserving its other bytes
//
// This preserves neighboring bytes but does not migrate an instruction pointer
// already inside the overwritten instructions, or drain old invocations.
//
// Constraint: all five patch bytes must fit in the containing aligned
// quadword (target offset <= 3). A successful atomic write does NOT coordinate
// a thread already executing displaced instructions. Caller quiescence remains
// required; this primitive is not an execution-safety guarantee.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

namespace splice::arch::x86_64 {

// Internal two-phase write primitive. Preparation does not access target bytes.
// Commit requires an unchanged successful plan and writable containing quadword.
// Neither phase coordinates instruction pointers or establishes live safety.
struct PreparedRel32Patch {
    void* target;
    unsigned char bytes[5];
};
bool prepare_atomic_jmp_rel32(void* target, void* new_func,
                              PreparedRel32Patch& prepared) noexcept;
void commit_atomic_jmp_rel32(const PreparedRel32Patch& prepared) noexcept;

// Atomically install a 5-byte `E9 rel32` jump at `target` redirecting to
// `new_func`. The caller must have already arranged for the containing aligned
// quadword to be RW-mapped (typically via splice::os::make_executable_writable).
//
// Returns true on success.
// Returns false if any of these preconditions are violated:
//   - the five-byte patch crosses its containing aligned quadword
//   - displacement(new_func − (target + 5)) doesn't fit in int32
//
// On false, strict callers refuse; legacy callers may use their documented
// non-atomic fallback. No target bytes are written on false.
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
