// ─── ARM64 disassembler golden tests ──────────────────────────────────────
//
// Validates every InstructionType enum value with ≥ 2 test vectors each.
// These are pure bit-manipulation tests — they run on any platform
// including Windows x86_64, because the disasm module has zero POSIX deps.
//
// Test vectors sourced from the ARM Architecture Reference Manual (ARMv8-A)
// and cross-checked against `objdump -d` output from real ARM64 binaries.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

// Private header — test has access via the `src` include dir on the
// splice target. Tests link against splice::splice.
#include <arch/arm64/disasm.h>

using namespace splice::arch::arm64;

// ─── B (unconditional branch) ───────────────────────────────────────────────
// Encoding: 0001 01ii iiii iiii iiii iiii iiii iiii  (26-bit signed imm × 4)

TEST(Arm64Disasm, b_forward_small_offset) {
    // B #+0x1000  →  imm26 = 0x400  →  insn = 0x14000400
    std::uint32_t insn = 0x14000400u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::B);
    EXPECT_EQ(info.offset, 0x1000);
}

TEST(Arm64Disasm, b_backward_negative_offset) {
    // B #-0x100  →  imm26 = 0x03FFFFC0 >> 2 = ...
    // -0x100 / 4 = -64 = 0x03FFFFC0 in 26-bit  →  insn = 0x17FFFFC0
    std::uint32_t insn = 0x17FFFFC0u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::B);
    EXPECT_LT(info.offset, 0);  // negative
}

// ─── BL (branch with link) ─────────────────────────────────────────────────
// Encoding: 1001 01ii ...  (same immediate as B)

TEST(Arm64Disasm, bl_forward) {
    // BL #+0x20  →  imm26 = 8  →  insn = 0x94000008
    std::uint32_t insn = 0x94000008u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::Bl);
    EXPECT_EQ(info.offset, 0x20);
}

TEST(Arm64Disasm, bl_backward) {
    // BL #-4  →  imm26 = 0x03FFFFFF  →  insn = 0x97FFFFFF
    std::uint32_t insn = 0x97FFFFFFu;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::Bl);
    EXPECT_EQ(info.offset, -4);
}

// ─── B.cond (conditional branch) ────────────────────────────────────────────
// Encoding: 0101 0100 iiii iiii iiii iiii iii0 cccc  (19-bit signed imm × 4)

TEST(Arm64Disasm, bcond_forward) {
    // B.EQ #+0x100  →  imm19 = 0x40  →  insn = 0x54000800  (cond = EQ = 0)
    std::uint32_t insn = 0x54000800u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::BCond);
    EXPECT_EQ(info.offset, 0x100);
}

TEST(Arm64Disasm, bcond_backward) {
    // B.NE #-8  →  imm19 = 0x7FFFE  →  insn = 0x54FFFFC1  (cond = NE = 1)
    std::uint32_t insn = 0x54FFFFC1u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::BCond);
    EXPECT_LT(info.offset, 0);
}

// ─── ADR (PC-relative address, 21-bit) ──────────────────────────────────────
// Encoding: 0ii1 0000 iiii iiii iiii iiii iiir rrrr

TEST(Arm64Disasm, adr_positive_offset) {
    // ADR X0, #+0x10
    // immhi = 4, immlo = 0  →  insn = 0x10000080
    std::uint32_t insn = 0x10000080u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::Adr);
    EXPECT_EQ(info.reg, 0u);
    EXPECT_EQ(info.offset, 0x10);
}

TEST(Arm64Disasm, adr_negative_offset) {
    // ADR X1, #-1  →  immhi=0x7FFFF, immlo=3  →  insn = 0x70FFFFE1
    std::uint32_t insn = 0x70FFFFE1u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::Adr);
    EXPECT_EQ(info.reg, 1u);
    EXPECT_EQ(info.offset, -1);
}

TEST(Arm64Disasm, adr_zero_offset) {
    // ADR X2, #+0  →  insn = 0x10000002
    std::uint32_t insn = 0x10000002u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::Adr);
    EXPECT_EQ(info.reg, 2u);
    EXPECT_EQ(info.offset, 0);
}

// ─── ADRP (PC-relative page address, 21-bit × 4096) ────────────────────────
// Encoding: 1ii1 0000 iiii iiii iiii iiii iiir rrrr

TEST(Arm64Disasm, adrp_positive_page) {
    // ADRP X3, #+0x1000  →  imm = 1 page  →  immhi=0, immlo=1
    // insn = 0x90000003 | (immlo << 29)  = 0x90000003 | 0x20000000 = 0xB0000003
    std::uint32_t insn = 0xB0000003u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::Adrp);
    EXPECT_EQ(info.reg, 3u);
    EXPECT_EQ(info.offset, 0x1000);  // 1 page = 4096
}

TEST(Arm64Disasm, adrp_negative_page) {
    // ADRP X4, #-0x1000  →  imm = -1 page  →  immhi=0x7FFFF, immlo=3
    // insn = 0x90000004 | (3 << 29) | (0x7FFFF << 5)
    //      = 0x90000004 | 0x60000000 | 0x00FFFFE0 = 0xF0FFFFE4
    std::uint32_t insn = 0xF0FFFFE4u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::Adrp);
    EXPECT_EQ(info.reg, 4u);
    EXPECT_EQ(info.offset, -0x1000);
}

// ─── LDR literal (PC-relative load, 19-bit × 4) ────────────────────────────
// Encoding: 0101 1000 iiii iiii iiii iiii iiir rrrr

TEST(Arm64Disasm, ldr_literal_forward) {
    // LDR X5, #+0x40  →  imm19 = 0x10  →  insn = 0x58000205
    std::uint32_t insn = 0x58000205u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::LdrLiteral);
    EXPECT_EQ(info.reg, 5u);
    EXPECT_EQ(info.offset, 0x40);
}

TEST(Arm64Disasm, ldr_literal_large_forward) {
    // LDR X0, #+0x100  →  imm19 = 0x40  →  insn = 0x58000800
    std::uint32_t insn = 0x58000800u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::LdrLiteral);
    EXPECT_EQ(info.reg, 0u);
    EXPECT_EQ(info.offset, 0x100);
}

// ─── CBZ / CBNZ (compare and branch, 19-bit × 4) ───────────────────────────
// Encoding: s011 010o iiii iiii iiii iiii iiir rrrr

TEST(Arm64Disasm, cbz_forward) {
    // CBZ W0, #+0x20  →  imm19 = 8  →  insn = 0x34000100
    std::uint32_t insn = 0x34000100u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::CbzCbnz);
    EXPECT_EQ(info.offset, 0x20);
}

TEST(Arm64Disasm, cbnz_backward) {
    // CBNZ X1, #-4  →  imm19 = 0x7FFFF  →  insn = 0xB5FFFFE1
    std::uint32_t insn = 0xB5FFFFE1u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::CbzCbnz);
    EXPECT_LT(info.offset, 0);
}

// ─── TBZ / TBNZ (test and branch, 14-bit × 4) ──────────────────────────────
// Encoding: s011 011o bbbb iiii iiii iiii iiir rrrr

TEST(Arm64Disasm, tbz_forward) {
    // TBZ W0, #0, #+0x10  →  imm14 = 4  →  insn = 0x36000080
    std::uint32_t insn = 0x36000080u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::TbzTbnz);
    EXPECT_EQ(info.offset, 0x10);
}

TEST(Arm64Disasm, tbnz_backward) {
    // TBNZ W0, #0, #-4  →  imm14 = 0x3FFF  →  insn = 0x377FFFE0
    std::uint32_t insn = 0x377FFFE0u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::TbzTbnz);
    EXPECT_LT(info.offset, 0);
}

// ─── Regular instruction (non-PC-relative) ──────────────────────────────────

TEST(Arm64Disasm, regular_mov) {
    // MOV X0, X1  →  0xAA0103E0
    std::uint32_t insn = 0xAA0103E0u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::Regular);
}

TEST(Arm64Disasm, regular_add) {
    // ADD X0, X0, #1  →  0x91000400
    std::uint32_t insn = 0x91000400u;
    auto info = analyze_instruction(&insn);
    EXPECT_EQ(info.type, InstructionType::Regular);
}

// ─── Indirect branch generation ─────────────────────────────────────────────

TEST(Arm64Disasm, generate_indirect_branch_br) {
    std::uint32_t buf[4] = {};
    const void* target = reinterpret_cast<const void*>(0xDEADBEEFCAFEull);
    auto size = generate_indirect_branch(buf, target, false);

    EXPECT_EQ(size, 16u);
    EXPECT_EQ(buf[0], 0x58000051u);  // ldr x17, #8
    EXPECT_EQ(buf[1], 0xD61F0220u);  // br x17

    auto stored = *reinterpret_cast<std::uint64_t*>(&buf[2]);
    EXPECT_EQ(stored, 0xDEADBEEFCAFEull);
}

TEST(Arm64Disasm, generate_indirect_branch_blr) {
    std::uint32_t buf[4] = {};
    const void* target = reinterpret_cast<const void*>(0x1234ull);
    auto size = generate_indirect_branch(buf, target, true);

    EXPECT_EQ(size, 16u);
    EXPECT_EQ(buf[0], 0x58000051u);  // ldr x17, #8
    EXPECT_EQ(buf[1], 0xD63F0220u);  // blr x17 (link variant)
}

// ─── Hook-detection probe ───────────────────────────────────────────────────

TEST(Arm64Disasm, looks_hooked_positive) {
    std::uint32_t buf[4] = {};
    generate_indirect_branch(buf, nullptr, false);
    EXPECT_TRUE(looks_hooked(buf));
}

TEST(Arm64Disasm, looks_hooked_negative) {
    std::uint32_t normal = 0x91000400u;  // ADD X0, X0, #1
    EXPECT_FALSE(looks_hooked(&normal));
}

TEST(Arm64Disasm, looks_hooked_null) {
    EXPECT_FALSE(looks_hooked(nullptr));
}

// ─── fix_pc_relative_instruction: the trampoline's distance is load-bearing ─
//
// ARM64 needs no relay -- its patch is `ldr x17, #8 / br x17 / .quad target`,
// which reaches anywhere, and its atomicity comes from writing the literal
// before the single 32-bit instruction. But relocating the *prologue* into the
// trampoline is a different matter, and the budgets are smaller than x86_64's:
//
//   copied B / BL   +/-128 MB
//   copied ADRP     +/-4 GB
//   copied ADR      +/-1 MB
//
// B and BL have a fallback: emit_prologue_copy replaces an unrelocatable one
// with an indirect branch. **ADRP and ADR do not.** The patcher logs "Complex
// PC-relative fixup not implemented" and returns SIZE_MAX, and the install is
// refused. So on ARM64 a trampoline beyond 4 GB of its target cannot host a
// prologue containing an ADRP -- and ADRP is how AArch64 reaches any global,
// so it is common in the first four instructions.
//
// These tests pin the arithmetic on any host. What they do NOT establish is
// how far `mmap(nullptr)` actually lands from a loaded library on Android;
// that needs a device, and is measured separately in test_arm64_reach.cpp.

namespace {

// ADRP X3, #+0x1000 — the vector from adrp_positive_page above.
constexpr std::uint32_t kAdrpX3PlusOnePage = 0xB0000003u;

// Recover the page displacement an ADRP encodes, so a relocation can be
// checked against the address it resolves to rather than against a bit pattern.
std::int64_t adrp_offset_of(std::uint32_t insn) {
    const std::int64_t immlo = (insn >> 29) & 0x3;
    const std::int64_t immhi = (insn >> 5) & 0x7FFFF;
    std::int64_t offset = ((immhi << 2) | immlo) << 12;
    if (offset & 0x100000000LL) {
        offset |= static_cast<std::int64_t>(0xFFFFFFFE00000000ULL);
    }
    return offset;
}

// Both boundary calls use the same target; this keeps the literal in one place.
const void* old_pc_of(const void*) {
    return reinterpret_cast<const void*>(0x0000100000000000ULL);
}

} // namespace

TEST(Arm64Disasm, fix_adrp_succeeds_within_4gb) {
    const auto old_pc = reinterpret_cast<const void*>(0x0000007000001000ULL);
    const auto new_pc = reinterpret_cast<const void*>(0x0000007000000000ULL);

    const std::uint32_t fixed = fix_pc_relative_instruction(
        kAdrpX3PlusOnePage, InstructionType::Adrp, old_pc, new_pc);
    ASSERT_NE(fixed, 0u) << "one page away must be relocatable";

    // Same register, and it must still resolve to the same page.
    EXPECT_EQ(fixed & 0x1Fu, 3u);
    const std::int64_t old_page =
        static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(old_pc)) & ~0xFFFLL;
    const std::int64_t new_page =
        static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(new_pc)) & ~0xFFFLL;
    EXPECT_EQ(new_page + adrp_offset_of(fixed), old_page + 0x1000);
}

TEST(Arm64Disasm, fix_adrp_refuses_beyond_4gb) {
    const auto old_pc = reinterpret_cast<const void*>(0x0000007000001000ULL);
    // ~45 TB away, the order of separation an unhinted anonymous mapping gets
    // from loaded code on a 64-bit Linux.
    const auto new_pc = reinterpret_cast<const void*>(0x00007F2C48000000ULL);

    EXPECT_EQ(fix_pc_relative_instruction(kAdrpX3PlusOnePage,
                                          InstructionType::Adrp, old_pc, new_pc),
              0u)
        << "beyond ADRP's +/-4 GB the displacement must be refused, not truncated";
}

TEST(Arm64Disasm, fix_adrp_boundary_is_four_gigabytes) {
    // ADRP encodes a signed 21-bit page count, so the relocated displacement
    // must satisfy -0x100000 <= (orig_target - new_page) >> 12 <= 0xFFFFF.
    //
    // Derived from orig_target rather than from old_pc, because the
    // instruction carries its own +1 page and forgetting it moves the boundary
    // by exactly one page -- which is how the first version of this test
    // failed.
    constexpr std::uint64_t kOldPc = 0x0000100000000000ULL;   // page-aligned
    constexpr std::uint64_t kOrigTarget = kOldPc + 0x1000ULL;  // ADRP X3, #+0x1000

    const auto at_limit =
        reinterpret_cast<const void*>(kOrigTarget - (0xFFFFFULL << 12));
    const auto one_page_past =
        reinterpret_cast<const void*>(kOrigTarget - (0x100000ULL << 12));

    EXPECT_NE(fix_pc_relative_instruction(kAdrpX3PlusOnePage,
                                          InstructionType::Adrp, old_pc_of(at_limit),
                                          at_limit),
              0u) << "0xFFFFF pages is the last representable displacement";
    EXPECT_EQ(fix_pc_relative_instruction(kAdrpX3PlusOnePage,
                                          InstructionType::Adrp,
                                          old_pc_of(one_page_past), one_page_past),
              0u) << "one page further must be refused";
}

TEST(Arm64Disasm, fix_b_refuses_beyond_128mb) {
    // B #+0x1000 — 0x14000000 | (0x1000 >> 2)
    const std::uint32_t b_insn = 0x14000000u | (0x1000u >> 2);
    ASSERT_EQ(analyze_instruction(&b_insn).type, InstructionType::B);

    const auto old_pc = reinterpret_cast<const void*>(0x0000007000001000ULL);
    const auto near_pc = reinterpret_cast<const void*>(0x0000007000000000ULL);
    const auto far_pc = reinterpret_cast<const void*>(0x00007F2C48000000ULL);

    EXPECT_NE(fix_pc_relative_instruction(b_insn, InstructionType::B, old_pc, near_pc), 0u);
    // Unlike ADRP, this one has a fallback: emit_prologue_copy replaces an
    // unrelocatable B with an indirect branch rather than failing the install.
    // The refusal here is what triggers that path.
    EXPECT_EQ(fix_pc_relative_instruction(b_insn, InstructionType::B, old_pc, far_pc), 0u);
}

// ─── calculate_copy_size ────────────────────────────────────────────────────

TEST(Arm64Disasm, copy_size_minimum_16_bytes) {
    // 4 regular instructions → exactly 16 bytes
    std::uint32_t code[] = {0x91000400, 0x91000400, 0x91000400, 0x91000400, 0x91000400};
    EXPECT_EQ(calculate_copy_size(code), 16u);
}

TEST(Arm64Disasm, copy_size_rounds_up_to_instruction_boundary) {
    // Should always be a multiple of 4
    std::uint32_t code[8] = {};
    auto size = calculate_copy_size(code);
    EXPECT_GE(size, 16u);
    EXPECT_EQ(size % 4, 0u);
}
