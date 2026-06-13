// ─── ARM64 instruction analyser ────────────────────────────────────────────
//
// Ported verbatim from the predecessor framework.h v3.0 — this logic is battle-
// tested in production game-enhancement.
// Do NOT rewrite.
//
// Covers: B / BL / B.cond / ADR / ADRP / LDR literal / CBZ/CBNZ / TBZ/TBNZ
// with correct sign-extension on 19/21/26-bit immediates.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstddef>
#include <cstdint>

namespace splice::arch::arm64 {

enum class InstructionType {
    Regular,      // Regular instruction, can be copied as-is
    BCond,        // Conditional branch (B.cond)
    B,            // Unconditional branch
    Bl,           // Branch with link
    Adr,          // PC-relative address
    Adrp,         // PC-relative page address
    LdrLiteral,   // PC-relative load
    CbzCbnz,      // Compare and branch
    TbzTbnz,      // Test and branch
    Unknown,
};

struct InstructionInfo {
    std::uint32_t instruction;
    InstructionType type;
    std::int64_t offset;  // For PC-relative instructions
    std::uint32_t reg;    // Target register for ADR/ADRP/LDR
};

// Decode a single ARM64 instruction word.
InstructionInfo analyze_instruction(const std::uint32_t* insn_ptr);

// How many bytes of prologue we need to relocate (always multiple of 4, at
// least 16). Walks instructions until `size >= 16`.
std::size_t calculate_copy_size(const void* target);

// Relocate a PC-relative instruction to fire correctly at `new_pc` instead
// of `old_pc`. Returns the rewritten instruction, or 0 if the new offset
// exceeds the instruction's immediate field (indicating the caller must
// emit an indirect branch instead).
std::uint32_t fix_pc_relative_instruction(std::uint32_t insn, InstructionType type,
                                          const void* old_pc, const void* new_pc);

// Emit an indirect branch sequence (ldr x17, #8 ; br/blr x17 ; .quad target).
// Writes 16 bytes to `buffer` and returns 16.
std::size_t generate_indirect_branch(std::uint32_t* buffer, const void* target,
                                     bool link = false);

// Pattern probe — does `insn[0]` look like Splice's `ldr x17, #8` marker?
bool looks_hooked(const std::uint32_t* insn);

} // namespace splice::arch::arm64
