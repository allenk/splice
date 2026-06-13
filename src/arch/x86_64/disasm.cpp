// ─── x86_64 instruction analyser — implementation ────────────────────────
//
// Activates nmd's implementation in this TU (NMD_ASSEMBLY_IMPLEMENTATION)
// so the entire decoder lives in this object file. nmd is C89; we compile
// it through a C++ TU by leaning on its `bool` / `uint*_t` usage already
// being C99-compatible.
// ───────────────────────────────────────────────────────────────────────────

// NOMINMAX must precede ANY include chain that might reach <windows.h>,
// otherwise Windows headers win the race and define `min`/`max` as macros
// that clash with `std::numeric_limits<...>::max()` further down.
// splice/log.h → detail/platform_log.h → <windows.h> (on MSVC) pulls it in.
#ifndef NOMINMAX
#   define NOMINMAX
#endif

#include "disasm.h"

#include <splice/log.h>

#include <cstring>
#include <limits>

// nmd is included once, with its implementation, in this TU only.
#define NMD_ASSEMBLY_IMPLEMENTATION
#include <nmd_assembly.h>

namespace splice::arch::x86_64 {

namespace {

// nmd's opcode ids we care about. Full list is ~1300 entries (NMD_X86_INSTRUCTION_*);
// we only need to classify branch / call / RIP-relative.
//
// Rather than pull in the whole enum, we dispatch on:
//   1. nmd's computed `group` (e.g. NMD_GROUP_JUMP, NMD_GROUP_CALL, ...)
//   2. primary opcode byte for branch-flavour disambiguation
// This keeps us decoupled from nmd's internal id numbering.

constexpr std::uint8_t kOp_Jcc_Rel8_Lo  = 0x70;  // 70..7F
constexpr std::uint8_t kOp_Jcc_Rel8_Hi  = 0x7F;
constexpr std::uint8_t kOp_Jmp_Rel8     = 0xEB;
constexpr std::uint8_t kOp_Jmp_Rel32    = 0xE9;
constexpr std::uint8_t kOp_Call_Rel32   = 0xE8;

InstructionType classify(const nmd_x86_instruction& insn) {
    // Two-byte opcode map: 0F 8x = Jcc rel32
    if (insn.opcode_size == 2 && (insn.opcode & 0xF0) == 0x80) {
        return InstructionType::JccRel32;
    }

    // One-byte primaries
    if (insn.opcode_size == 1) {
        const std::uint8_t op = insn.opcode;
        if (op == kOp_Jmp_Rel32)  return InstructionType::JmpRel32;
        if (op == kOp_Call_Rel32) return InstructionType::CallRel32;
        if (op == kOp_Jmp_Rel8)   return InstructionType::JmpRel8;
        if (op >= kOp_Jcc_Rel8_Lo && op <= kOp_Jcc_Rel8_Hi) return InstructionType::JccRel8;
    }

    // RIP-relative addressing: ModR/M with mod=00, rm=101 means [RIP + disp32].
    // Only applies in 64-bit mode. nmd sets `has_modrm` and we inspect modrm.
    if (insn.has_modrm &&
        insn.modrm.fields.mod == 0 && insn.modrm.fields.rm == 0b101) {
        return InstructionType::RipRelative;
    }

    return InstructionType::Regular;
}

// Sign-extend an N-bit immediate from the low bits of `value`.
template <int Bits>
std::int64_t sign_extend(std::uint64_t value) {
    static_assert(Bits > 0 && Bits < 64);
    const std::uint64_t mask = (std::uint64_t{1} << Bits) - 1;
    const std::uint64_t sign = std::uint64_t{1} << (Bits - 1);
    const std::uint64_t v = value & mask;
    return static_cast<std::int64_t>((v ^ sign) - sign);
}

// Extract the PC-relative immediate from a decoded instruction.
std::int64_t extract_pc_relative_imm(const nmd_x86_instruction& insn,
                                     InstructionType type) {
    switch (type) {
        case InstructionType::JmpRel8:
        case InstructionType::JccRel8:
            return sign_extend<8>(insn.immediate);
        case InstructionType::JmpRel32:
        case InstructionType::CallRel32:
        case InstructionType::JccRel32:
            return sign_extend<32>(insn.immediate);
        case InstructionType::RipRelative:
            return sign_extend<32>(insn.displacement);
        default:
            return 0;
    }
}

} // namespace

InstructionInfo analyze_instruction(const void* buffer, std::size_t max_size) {
    InstructionInfo info{};
    info.type = InstructionType::Unknown;

    nmd_x86_instruction insn{};
    if (!nmd_x86_decode(buffer, max_size, &insn, NMD_X86_MODE_64, 0)) {
        SPLICE_LOGW("x86_64 decode failed at %p", buffer);
        return info;
    }

    info.length = insn.length;
    info.opcode = insn.opcode;
    info.type = classify(insn);
    info.displacement = extract_pc_relative_imm(insn, info.type);
    return info;
}

std::size_t calculate_copy_size(const void* target) {
    const auto* bytes = static_cast<const std::uint8_t*>(target);
    std::size_t size = 0;

    // Max prologue walk: 15 bytes/insn × 4 insns = 60 is plenty for a
    // function prologue that covers 16 bytes of patch space.
    constexpr std::size_t kWalkLimit = 64;

    while (size < 16 && size < kWalkLimit) {
        const auto info = analyze_instruction(bytes + size, kWalkLimit - size);
        if (info.length == 0 || info.type == InstructionType::Unknown) {
            SPLICE_LOGE("calculate_copy_size: decode failed at offset %zu", size);
            return 0;
        }
        size += info.length;
    }
    SPLICE_LOGV("x86_64 calculate_copy_size: %zu", size);
    return size;
}

std::size_t relocate_instruction(const InstructionInfo& info,
                                 const std::uint8_t* old_bytes,
                                 const void* old_pc, const void* new_pc,
                                 std::uint8_t* out_buffer) {
    const auto old_addr = reinterpret_cast<std::int64_t>(old_pc);
    const auto new_addr = reinterpret_cast<std::int64_t>(new_pc);

    switch (info.type) {
        case InstructionType::Regular:
            std::memcpy(out_buffer, old_bytes, info.length);
            return info.length;

        case InstructionType::CallRel32:
        case InstructionType::JmpRel32: {
            // One-byte opcode + 4-byte rel32 = 5 bytes total.
            const std::int64_t orig_target = old_addr + info.length + info.displacement;
            const std::int64_t new_disp = orig_target - (new_addr + info.length);
            if (new_disp < std::numeric_limits<std::int32_t>::min() ||
                new_disp > std::numeric_limits<std::int32_t>::max()) {
                SPLICE_LOGE("rel32 out of range after relocate: %lld",
                            static_cast<long long>(new_disp));
                return 0;
            }
            out_buffer[0] = old_bytes[0];
            const std::int32_t d32 = static_cast<std::int32_t>(new_disp);
            std::memcpy(out_buffer + 1, &d32, 4);
            return 5;
        }

        case InstructionType::JccRel32: {
            // Two-byte opcode (0F 8x) + 4-byte rel32 = 6 bytes.
            const std::int64_t orig_target = old_addr + info.length + info.displacement;
            const std::int64_t new_disp = orig_target - (new_addr + info.length);
            if (new_disp < std::numeric_limits<std::int32_t>::min() ||
                new_disp > std::numeric_limits<std::int32_t>::max()) {
                return 0;
            }
            out_buffer[0] = old_bytes[0];  // 0x0F
            out_buffer[1] = old_bytes[1];  // 0x8x
            const std::int32_t d32 = static_cast<std::int32_t>(new_disp);
            std::memcpy(out_buffer + 2, &d32, 4);
            return 6;
        }

        case InstructionType::JmpRel8:
        case InstructionType::JccRel8:
            // rel8 only reaches ±128 bytes. After moving into a trampoline
            // that's generally farther than 128B from the original target,
            // it can't be rewritten in place. Caller must promote to rel32
            // (Jmp rel8 → Jmp rel32, or Jcc rel8 → Jcc rel32) or emit an
            // indirect jump. Signal "can't fix in place".
            return 0;

        case InstructionType::RipRelative: {
            // ModR/M + optional SIB + disp32. nmd tells us the length; the
            // disp32 sits at `length - 4` (no immediate operand for loads;
            // for instructions with both disp32 and immediate, this still
            // holds because nmd reports length inclusive of everything).
            // For Phase 3 we only handle loads/stores — instructions that
            // have no immediate after the displacement. Same as ARM64 ADRP,
            // a conservative check guards against misclassification.
            const std::int64_t orig_target = old_addr + info.length + info.displacement;
            const std::int64_t new_disp = orig_target - (new_addr + info.length);
            if (new_disp < std::numeric_limits<std::int32_t>::min() ||
                new_disp > std::numeric_limits<std::int32_t>::max()) {
                return 0;
            }
            std::memcpy(out_buffer, old_bytes, info.length);
            const std::int32_t d32 = static_cast<std::int32_t>(new_disp);
            std::memcpy(out_buffer + info.length - 4, &d32, 4);
            return info.length;
        }

        case InstructionType::Unknown:
        default:
            return 0;
    }
}

std::size_t emit_jmp_rel32(std::uint8_t* buffer, const void* source, const void* target) {
    const auto src = reinterpret_cast<std::int64_t>(source);
    const auto tgt = reinterpret_cast<std::int64_t>(target);
    const std::int64_t disp = tgt - (src + 5);
    if (disp < std::numeric_limits<std::int32_t>::min() ||
        disp > std::numeric_limits<std::int32_t>::max()) {
        return 0;
    }
    buffer[0] = 0xE9;
    const std::int32_t d32 = static_cast<std::int32_t>(disp);
    std::memcpy(buffer + 1, &d32, 4);
    return 5;
}

std::size_t emit_jmp_abs64(std::uint8_t* buffer, const void* target) {
    // FF 25 00 00 00 00         jmp qword ptr [rip+0]
    // <8 byte absolute address>
    buffer[0] = 0xFF;
    buffer[1] = 0x25;
    buffer[2] = 0x00; buffer[3] = 0x00; buffer[4] = 0x00; buffer[5] = 0x00;
    const auto addr = reinterpret_cast<std::uint64_t>(target);
    std::memcpy(buffer + 6, &addr, 8);
    return 14;
}

bool looks_hooked(const std::uint8_t* bytes) {
    if (bytes == nullptr) return false;
    // Either 5-byte rel32 jmp or 14-byte absolute jmp signature.
    if (bytes[0] == 0xE9) return true;
    if (bytes[0] == 0xFF && bytes[1] == 0x25) return true;
    return false;
}

} // namespace splice::arch::x86_64
