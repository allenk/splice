// ─── x86_64 instruction analyser ───────────────────────────────────────────
//
// Thin C++ wrapper over the vendored `nmd` length-disassembler. Unlike
// ARM64's fixed 4-byte encoding, x86 instructions are 1–15 bytes long, so
// we actually need a decoder to walk a prologue safely.
//
// The API mirrors `src/arch/arm64/disasm.h` so the engine's arch dispatch
// stays symmetric.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstddef>
#include <cstdint>

namespace splice::arch::x86_64 {

// Classes of x86_64 instructions that need PC-relative fixup when relocated
// into a trampoline. Anything outside this list is treated as Regular.
enum class InstructionType {
    Regular,            // No PC-relative state — copy bytes verbatim
    CallRel32,          // E8 rel32
    JmpRel32,           // E9 rel32
    JccRel32,           // 0F 8x rel32
    JmpRel8,            // EB rel8
    JccRel8,            // 70..7F rel8
    RipRelative,        // ModR/M with RIP-relative addressing (disp32)
    Unknown,            // Decoder rejected the buffer
};

struct InstructionInfo {
    std::uint8_t length;        // Total instruction length in bytes (0 on decode failure)
    InstructionType type;
    std::int64_t displacement;  // rel8 / rel32 / RIP-relative disp, sign-extended
    std::uint8_t opcode;        // Primary opcode byte (for diagnostics)
};

// Decode a single instruction starting at `buffer`. `max_size` must be at
// least 15 (the architectural maximum instruction length) to guarantee a
// successful decode even at the edge of a buffer.
InstructionInfo analyze_instruction(const void* buffer, std::size_t max_size);

// Walk instructions from `target` until at least `min_bytes` are covered,
// rounding up to the nearest whole instruction. Returns 0 on decode failure.
//
// `min_bytes` is the length of the patch that will actually be written, and
// asking for more than that is not free: every extra byte demanded here is
// another instruction relocated into the trampoline, another chance that the
// relocation cannot be expressed, and a longer original-bytes record for
// splice_disable to restore.
//
// The default is the worst case, a 14-byte `FF 25` absolute jmp rounded up to
// 16. A patcher that has arranged for its destination to be within rel32 --
// see the relay in arch/x86_64/patcher.cpp -- should pass 5 instead. The
// difference is not cosmetic: MSVC entry sequences of 17 and 19 bytes are
// common, and those overflow the 16-byte disable record, which is why
// splice_disable used to be unavailable on ordinary functions.
std::size_t calculate_copy_size(const void* target, std::size_t min_bytes = 16);

// Relocate a PC-relative instruction to fire correctly at `new_pc` instead
// of `old_pc`. Writes the rewritten instruction to `out_buffer` and
// returns the number of bytes written, or 0 if the new displacement
// overflows the instruction's immediate field (caller must emit an
// indirect jump instead).
//
// For Jcc rel8, the caller must promote to Jcc rel32 up front because
// rel8 only reaches ±128 bytes — we can't rewrite in place without
// changing length.
std::size_t relocate_instruction(const InstructionInfo& info,
                                 const std::uint8_t* old_bytes,
                                 const void* old_pc, const void* new_pc,
                                 std::uint8_t* out_buffer);

// Emit a 5-byte relative jmp (E9 rel32). Returns 5 on success, 0 if
// `target - (source + 5)` doesn't fit in int32.
std::size_t emit_jmp_rel32(std::uint8_t* buffer, const void* source, const void* target);

// Emit a 14-byte absolute jmp (FF 25 00 00 00 00  <8-byte abs addr>).
// Always fits; always 14 bytes.
std::size_t emit_jmp_abs64(std::uint8_t* buffer, const void* target);

// Pattern probe — does this look like a Splice x86_64 hook jump?
bool looks_hooked(const std::uint8_t* bytes);

} // namespace splice::arch::x86_64
