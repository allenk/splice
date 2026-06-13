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

// Walk instructions from `target` until we've covered at least 16 bytes
// (enough to overwrite with a 14-byte absolute jmp). Rounds up to the
// nearest full instruction boundary. Returns 0 on decode failure.
std::size_t calculate_copy_size(const void* target);

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
