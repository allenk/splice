// ─── ARM64 instruction analyser — implementation ──────────────────────────
//
// Carried over from the predecessor framework.h v3.0. The bit-pattern match tree
// and sign-extension masks are preserved verbatim; only log macros, naming,
// and namespace have been updated.
// ───────────────────────────────────────────────────────────────────────────
#include "disasm.h"

#include <splice/log.h>

namespace splice::arch::arm64 {

InstructionInfo analyze_instruction(const std::uint32_t* insn_ptr) {
    const std::uint32_t insn = *insn_ptr;
    InstructionInfo info{insn, InstructionType::Regular, 0, 0};

    // B (unconditional branch) — 0001 01ii ... (26-bit signed offset * 4)
    if ((insn & 0xFC000000u) == 0x14000000u) {
        info.type = InstructionType::B;
        info.offset = (static_cast<std::int32_t>(insn & 0x03FFFFFFu) << 6) >> 4;
        SPLICE_LOGV("B   offset=%lld", static_cast<long long>(info.offset));
    }
    // BL (branch with link) — 1001 01ii ...
    else if ((insn & 0xFC000000u) == 0x94000000u) {
        info.type = InstructionType::Bl;
        info.offset = (static_cast<std::int32_t>(insn & 0x03FFFFFFu) << 6) >> 4;
        SPLICE_LOGV("BL  offset=%lld", static_cast<long long>(info.offset));
    }
    // B.cond — 0101 0100 iiii iiii iiii iiii iii0 cccc (19-bit signed offset * 4)
    else if ((insn & 0xFF000010u) == 0x54000000u) {
        info.type = InstructionType::BCond;
        info.offset = (static_cast<std::int32_t>(insn & 0x00FFFFE0u) << 8) >> 11;
        SPLICE_LOGV("B.cond offset=%lld", static_cast<long long>(info.offset));
    }
    // ADR — 0ii1 0000 iiii iiii iiii iiii iiir rrrr (21-bit signed)
    else if ((insn & 0x9F000000u) == 0x10000000u) {
        info.type = InstructionType::Adr;
        info.reg = insn & 0x1Fu;
        const std::int32_t immlo = (insn >> 29) & 0x3;
        const std::int32_t immhi = (insn >> 5) & 0x7FFFF;
        info.offset = (immhi << 2) | immlo;
        if (info.offset & 0x100000) {
            info.offset |= static_cast<std::int64_t>(0xFFFFFFFFFFE00000ULL);
        }
        SPLICE_LOGV("ADR  x%u offset=%lld", info.reg, static_cast<long long>(info.offset));
    }
    // ADRP — 1ii1 0000 iiii iiii iiii iiii iiir rrrr (21-bit signed * 4096)
    else if ((insn & 0x9F000000u) == 0x90000000u) {
        info.type = InstructionType::Adrp;
        info.reg = insn & 0x1Fu;
        const std::int32_t immlo = (insn >> 29) & 0x3;
        const std::int32_t immhi = (insn >> 5) & 0x7FFFF;
        info.offset = (static_cast<std::int64_t>((immhi << 2) | immlo)) << 12;
        if (info.offset & 0x100000000LL) {
            info.offset |= static_cast<std::int64_t>(0xFFFFFFFE00000000ULL);
        }
        SPLICE_LOGV("ADRP x%u offset=%lld", info.reg, static_cast<long long>(info.offset));
    }
    // LDR (literal) — 01011000 iiii iiii iiii iiii iiir rrrr (19-bit signed * 4)
    else if ((insn & 0xFF000000u) == 0x58000000u) {
        info.type = InstructionType::LdrLiteral;
        info.reg = insn & 0x1Fu;
        info.offset = static_cast<std::int64_t>((insn >> 5) & 0x7FFFF) * 4;
        if (info.offset & 0x40000) {
            info.offset |= static_cast<std::int64_t>(0xFFFFFFFFFFF80000ULL);
        }
        SPLICE_LOGV("LDR  lit x%u offset=%lld", info.reg, static_cast<long long>(info.offset));
    }
    // CBZ / CBNZ — s011 010o iiii iiii iiii iiii iiir rrrr
    else if ((insn & 0x7E000000u) == 0x34000000u) {
        info.type = InstructionType::CbzCbnz;
        info.offset = static_cast<std::int64_t>((insn >> 5) & 0x7FFFF) * 4;
        if (info.offset & 0x40000) {
            info.offset |= static_cast<std::int64_t>(0xFFFFFFFFFFF80000ULL);
        }
    }
    // TBZ / TBNZ — s011 011o bbbb iiii iiii iiii iiir rrrr
    else if ((insn & 0x7E000000u) == 0x36000000u) {
        info.type = InstructionType::TbzTbnz;
        info.offset = static_cast<std::int64_t>((insn >> 5) & 0x3FFF) * 4;
        if (info.offset & 0x2000) {
            info.offset |= static_cast<std::int64_t>(0xFFFFFFFFFFFF8000ULL);
        }
    }

    return info;
}

std::size_t calculate_copy_size(const void* target) {
    const auto* insn_ptr = static_cast<const std::uint32_t*>(target);
    std::size_t size = 0;

    // Need at least 16 bytes for our indirect-branch patch, but only full
    // 4-byte instructions can be copied. Walk forward until we're there.
    while (size < 16) {
        InstructionInfo info = analyze_instruction(insn_ptr);
        size += 4;
        ++insn_ptr;
        if (info.type == InstructionType::B || info.type == InstructionType::Bl ||
            info.type == InstructionType::BCond) {
            SPLICE_LOGV("Branch at offset %zu; trampoline may need extra fixup space",
                        size - 4);
        }
    }
    SPLICE_LOGV("calculate_copy_size: %zu", size);
    return size;
}

std::uint32_t fix_pc_relative_instruction(std::uint32_t insn, InstructionType type,
                                          const void* old_pc, const void* new_pc) {
    const auto old_addr = reinterpret_cast<std::int64_t>(old_pc);
    const auto new_addr = reinterpret_cast<std::int64_t>(new_pc);

    switch (type) {
        case InstructionType::B:
        case InstructionType::Bl: {
            const std::int64_t offset =
                (static_cast<std::int32_t>(insn & 0x03FFFFFFu) << 6) >> 4;
            const std::int64_t orig_target = old_addr + offset;
            const std::int64_t new_offset = orig_target - new_addr;
            if (new_offset < -0x8000000LL || new_offset > 0x7FFFFFCLL) {
                SPLICE_LOGE("B/BL out of ±128MB range: %lld", static_cast<long long>(new_offset));
                return 0;
            }
            return (insn & 0xFC000000u)
                 | (static_cast<std::uint32_t>((new_offset >> 2) & 0x03FFFFFFLL));
        }

        case InstructionType::Adr: {
            const std::int32_t immlo = (insn >> 29) & 0x3;
            const std::int32_t immhi = (insn >> 5) & 0x7FFFF;
            std::int64_t offset = (immhi << 2) | immlo;
            if (offset & 0x100000) offset |= static_cast<std::int64_t>(0xFFFFFFFFFFE00000ULL);
            const std::int64_t orig_target = old_addr + offset;
            const std::int64_t new_offset = orig_target - new_addr;
            if (new_offset < -0x100000LL || new_offset > 0xFFFFFLL) {
                SPLICE_LOGE("ADR offset out of range: %lld", static_cast<long long>(new_offset));
                return 0;
            }
            const std::uint32_t new_immlo = static_cast<std::uint32_t>(new_offset & 0x3);
            const std::uint32_t new_immhi =
                static_cast<std::uint32_t>((new_offset >> 2) & 0x7FFFF);
            return (insn & 0x9F00001Fu) | (new_immlo << 29) | (new_immhi << 5);
        }

        case InstructionType::Adrp: {
            const std::int32_t immlo = (insn >> 29) & 0x3;
            const std::int32_t immhi = (insn >> 5) & 0x7FFFF;
            std::int64_t offset = static_cast<std::int64_t>((immhi << 2) | immlo) << 12;
            if (offset & 0x100000000LL) {
                offset |= static_cast<std::int64_t>(0xFFFFFFFE00000000ULL);
            }
            const std::int64_t old_page = old_addr & ~0xFFFLL;
            const std::int64_t orig_target = old_page + offset;
            const std::int64_t new_page = new_addr & ~0xFFFLL;
            const std::int64_t new_offset = (orig_target - new_page) >> 12;
            if (new_offset < -0x100000LL || new_offset > 0xFFFFFLL) {
                SPLICE_LOGE("ADRP offset out of range: %lld", static_cast<long long>(new_offset));
                return 0;
            }
            const std::uint32_t new_immlo = static_cast<std::uint32_t>(new_offset & 0x3);
            const std::uint32_t new_immhi =
                static_cast<std::uint32_t>((new_offset >> 2) & 0x7FFFF);
            return (insn & 0x9F00001Fu) | (new_immlo << 29) | (new_immhi << 5);
        }

        default:
            return insn;  // No modification needed
    }
}

std::size_t generate_indirect_branch(std::uint32_t* buffer, const void* target, bool link) {
    // ldr  x17, #8
    // br/blr x17
    // .quad <target>
    buffer[0] = 0x58000051u;                       // ldr x17, #8
    buffer[1] = link ? 0xD63F0220u : 0xD61F0220u;  // blr x17 : br x17

    auto* addr_slot = reinterpret_cast<std::uint64_t*>(&buffer[2]);
    *addr_slot = reinterpret_cast<std::uint64_t>(target);

    return 16;
}

bool looks_hooked(const std::uint32_t* insn) {
    return insn != nullptr && insn[0] == 0x58000051u;  // ldr x17, #8
}

} // namespace splice::arch::arm64
