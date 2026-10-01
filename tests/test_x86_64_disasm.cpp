// ─── x86_64 disassembler golden tests ─────────────────────────────────────
//
// Every InstructionType classification gets ≥ 2 vectors. The byte encodings
// are cross-checked against `objdump -d -M intel` and real ARM64/x86 tools.
// Runs on any host — the disasm TU has no OS-dependent code path.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <arch/x86_64/disasm.h>

#include <array>
#include <cstdint>
#include <cstring>   // std::memcpy — libstdc++ does not pull it in transitively

using namespace splice::arch::x86_64;

namespace {

// Inline constructor for byte arrays — avoids the initializer-list ugliness
// of passing literal `std::uint8_t[N]` into helper functions.
template <std::size_t N>
auto make_bytes(const std::uint8_t (&arr)[N]) {
    std::array<std::uint8_t, N> out{};
    for (std::size_t i = 0; i < N; ++i) out[i] = arr[i];
    return out;
}

} // namespace

// ─── CALL rel32 (E8 xx xx xx xx) ────────────────────────────────────────────

TEST(X86Disasm, call_rel32_positive_offset) {
    // call +0x10   →  E8 10 00 00 00
    const std::uint8_t bytes[] = {0xE8, 0x10, 0x00, 0x00, 0x00};
    auto info = analyze_instruction(bytes, sizeof(bytes));
    EXPECT_EQ(info.type, InstructionType::CallRel32);
    EXPECT_EQ(info.length, 5);
    EXPECT_EQ(info.displacement, 0x10);
}

TEST(X86Disasm, call_rel32_negative_offset) {
    // call -1      →  E8 FF FF FF FF
    const std::uint8_t bytes[] = {0xE8, 0xFF, 0xFF, 0xFF, 0xFF};
    auto info = analyze_instruction(bytes, sizeof(bytes));
    EXPECT_EQ(info.type, InstructionType::CallRel32);
    EXPECT_EQ(info.length, 5);
    EXPECT_EQ(info.displacement, -1);
}

// ─── JMP rel32 (E9 xx xx xx xx) ─────────────────────────────────────────────

TEST(X86Disasm, jmp_rel32_positive) {
    const std::uint8_t bytes[] = {0xE9, 0x20, 0x00, 0x00, 0x00};
    auto info = analyze_instruction(bytes, sizeof(bytes));
    EXPECT_EQ(info.type, InstructionType::JmpRel32);
    EXPECT_EQ(info.length, 5);
    EXPECT_EQ(info.displacement, 0x20);
}

TEST(X86Disasm, jmp_rel32_negative) {
    const std::uint8_t bytes[] = {0xE9, 0x00, 0xFF, 0xFF, 0xFF};
    auto info = analyze_instruction(bytes, sizeof(bytes));
    EXPECT_EQ(info.type, InstructionType::JmpRel32);
    EXPECT_EQ(info.length, 5);
    EXPECT_EQ(info.displacement, -256);  // 0xFFFFFF00 sign-extended
}

// ─── Jcc rel32 (0F 8x xx xx xx xx) ──────────────────────────────────────────

TEST(X86Disasm, jcc_rel32_je) {
    // je +0x100    →  0F 84 00 01 00 00
    const std::uint8_t bytes[] = {0x0F, 0x84, 0x00, 0x01, 0x00, 0x00};
    auto info = analyze_instruction(bytes, sizeof(bytes));
    EXPECT_EQ(info.type, InstructionType::JccRel32);
    EXPECT_EQ(info.length, 6);
    EXPECT_EQ(info.displacement, 0x100);
}

TEST(X86Disasm, jcc_rel32_jne) {
    // jne -4       →  0F 85 FC FF FF FF
    const std::uint8_t bytes[] = {0x0F, 0x85, 0xFC, 0xFF, 0xFF, 0xFF};
    auto info = analyze_instruction(bytes, sizeof(bytes));
    EXPECT_EQ(info.type, InstructionType::JccRel32);
    EXPECT_EQ(info.length, 6);
    EXPECT_EQ(info.displacement, -4);
}

// ─── JMP rel8 (EB xx) ───────────────────────────────────────────────────────

TEST(X86Disasm, jmp_rel8_positive) {
    const std::uint8_t bytes[] = {0xEB, 0x10};
    auto info = analyze_instruction(bytes, sizeof(bytes));
    EXPECT_EQ(info.type, InstructionType::JmpRel8);
    EXPECT_EQ(info.length, 2);
    EXPECT_EQ(info.displacement, 0x10);
}

TEST(X86Disasm, jmp_rel8_negative) {
    const std::uint8_t bytes[] = {0xEB, 0xF0};
    auto info = analyze_instruction(bytes, sizeof(bytes));
    EXPECT_EQ(info.type, InstructionType::JmpRel8);
    EXPECT_EQ(info.length, 2);
    EXPECT_EQ(info.displacement, -16);
}

// ─── Jcc rel8 (70..7F xx) ───────────────────────────────────────────────────

TEST(X86Disasm, jcc_rel8_je) {
    // je +0x08     →  74 08
    const std::uint8_t bytes[] = {0x74, 0x08};
    auto info = analyze_instruction(bytes, sizeof(bytes));
    EXPECT_EQ(info.type, InstructionType::JccRel8);
    EXPECT_EQ(info.length, 2);
    EXPECT_EQ(info.displacement, 0x08);
}

TEST(X86Disasm, jcc_rel8_jne_negative) {
    // jne -3       →  75 FD
    const std::uint8_t bytes[] = {0x75, 0xFD};
    auto info = analyze_instruction(bytes, sizeof(bytes));
    EXPECT_EQ(info.type, InstructionType::JccRel8);
    EXPECT_EQ(info.length, 2);
    EXPECT_EQ(info.displacement, -3);
}

// ─── RIP-relative load ─────────────────────────────────────────────────────
// mov rax, [rip + disp32]  →  48 8B 05 xx xx xx xx

TEST(X86Disasm, rip_relative_mov_positive) {
    const std::uint8_t bytes[] = {0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00};
    auto info = analyze_instruction(bytes, sizeof(bytes));
    EXPECT_EQ(info.type, InstructionType::RipRelative);
    EXPECT_EQ(info.length, 7);
    EXPECT_EQ(info.displacement, 0x10);
}

TEST(X86Disasm, rip_relative_lea_negative) {
    // lea rax, [rip - 8]  →  48 8D 05 F8 FF FF FF
    const std::uint8_t bytes[] = {0x48, 0x8D, 0x05, 0xF8, 0xFF, 0xFF, 0xFF};
    auto info = analyze_instruction(bytes, sizeof(bytes));
    EXPECT_EQ(info.type, InstructionType::RipRelative);
    EXPECT_EQ(info.length, 7);
    EXPECT_EQ(info.displacement, -8);
}

// ─── Regular instructions (no PC-relative fixup needed) ────────────────────

TEST(X86Disasm, regular_push_rbp) {
    const std::uint8_t bytes[] = {0x55};
    auto info = analyze_instruction(bytes, sizeof(bytes));
    EXPECT_EQ(info.type, InstructionType::Regular);
    EXPECT_EQ(info.length, 1);
}

TEST(X86Disasm, regular_mov_rbp_rsp) {
    // mov rbp, rsp  →  48 89 E5
    const std::uint8_t bytes[] = {0x48, 0x89, 0xE5};
    auto info = analyze_instruction(bytes, sizeof(bytes));
    EXPECT_EQ(info.type, InstructionType::Regular);
    EXPECT_EQ(info.length, 3);
}

TEST(X86Disasm, regular_sub_rsp_imm8) {
    // sub rsp, 0x28  →  48 83 EC 28
    const std::uint8_t bytes[] = {0x48, 0x83, 0xEC, 0x28};
    auto info = analyze_instruction(bytes, sizeof(bytes));
    EXPECT_EQ(info.type, InstructionType::Regular);
    EXPECT_EQ(info.length, 4);
}

// ─── emit_jmp_rel32 ────────────────────────────────────────────────────────

TEST(X86Emit, jmp_rel32_forward) {
    std::uint8_t buf[5]{};
    const auto* source = reinterpret_cast<const void*>(0x10000);
    const auto* target = reinterpret_cast<const void*>(0x10100);  // +0x100 ahead
    auto written = emit_jmp_rel32(buf, source, target);
    EXPECT_EQ(written, 5);
    EXPECT_EQ(buf[0], 0xE9);
    // disp = target - (source + 5) = 0x100 - 5 = 0xFB
    std::int32_t disp;
    std::memcpy(&disp, buf + 1, 4);
    EXPECT_EQ(disp, 0xFB);
}

TEST(X86Emit, jmp_rel32_returns_zero_when_out_of_range) {
    std::uint8_t buf[5]{};
    const auto* source = reinterpret_cast<const void*>(0x1000);
    const auto* target = reinterpret_cast<const void*>(
        static_cast<std::uintptr_t>(0x1000) + 0x80000000ULL + 0x100);  // >2GB forward
    EXPECT_EQ(emit_jmp_rel32(buf, source, target), 0);
}

// ─── emit_jmp_abs64 ────────────────────────────────────────────────────────

TEST(X86Emit, jmp_abs64_bytes_match) {
    std::uint8_t buf[14]{};
    const auto* target = reinterpret_cast<const void*>(0xDEADBEEFCAFE'BABEull);
    auto written = emit_jmp_abs64(buf, target);
    EXPECT_EQ(written, 14);
    EXPECT_EQ(buf[0], 0xFF);
    EXPECT_EQ(buf[1], 0x25);
    EXPECT_EQ(buf[2], 0x00);
    EXPECT_EQ(buf[3], 0x00);
    EXPECT_EQ(buf[4], 0x00);
    EXPECT_EQ(buf[5], 0x00);
    std::uint64_t addr{};
    std::memcpy(&addr, buf + 6, 8);
    EXPECT_EQ(addr, 0xDEADBEEFCAFE'BABEull);
}

// ─── looks_hooked probe ───────────────────────────────────────────────────

TEST(X86Disasm, looks_hooked_detects_rel32_jmp) {
    const std::uint8_t bytes[] = {0xE9, 0x00, 0x00, 0x00, 0x00};
    EXPECT_TRUE(looks_hooked(bytes));
}

TEST(X86Disasm, looks_hooked_detects_abs64_jmp) {
    const std::uint8_t bytes[] = {0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_TRUE(looks_hooked(bytes));
}

TEST(X86Disasm, looks_hooked_false_on_push_rbp) {
    const std::uint8_t bytes[] = {0x55};
    EXPECT_FALSE(looks_hooked(bytes));
}

TEST(X86Disasm, looks_hooked_null_is_safe) {
    EXPECT_FALSE(looks_hooked(nullptr));
}

// ─── relocate_instruction: the trampoline's distance is load-bearing ───────
//
// Relocating a PC-relative instruction means recomputing its operand so that,
// executed from the trampoline, it still reaches what it reached from the
// original address. The operand is a signed 32-bit displacement, so that is
// only expressible while the trampoline is within 2 GB of the original target.
//
// Beyond that, relocate_instruction returns 0 -- "cannot fix in place" -- which
// emit_prologue_copy turns into a fatal SIZE_MAX and install_inline_patch
// refuses the hook. No warning names the distance; the symptom is a function
// that will not hook.
//
// These are direct tests of the mechanism rather than of any real prologue.
// Whether a given function trips it depends on whether a rel32 or RIP-relative
// instruction happens to fall inside the copied bytes, and that window is now
// 5 bytes rather than 16 -- see the min_bytes test below.

namespace {

// `mov eax, dword ptr [rip + 0]` — the shape of any access to a global in
// position-independent x86_64 code.
InstructionInfo rip_relative_info() {
    const std::uint8_t bytes[] = {0x8B, 0x05, 0x00, 0x00, 0x00, 0x00};
    return analyze_instruction(bytes, sizeof(bytes));
}

} // namespace

TEST(X86Disasm, relocate_rip_relative_succeeds_within_rel32) {
    const auto info = rip_relative_info();
    ASSERT_EQ(info.type, InstructionType::RipRelative);
    ASSERT_EQ(info.length, 6u);

    const std::uint8_t src[] = {0x8B, 0x05, 0x00, 0x00, 0x00, 0x00};
    // An arbitrary but plausible pair: the trampoline one page below the
    // target, which is what the near-search actually produces.
    const auto old_pc = reinterpret_cast<const void*>(0x00007FF890000000ULL);
    const auto new_pc = reinterpret_cast<const void*>(0x00007FF88FFFF000ULL);

    std::uint8_t out[16]{};
    const std::size_t written =
        relocate_instruction(info, src, old_pc, new_pc, out);

    ASSERT_EQ(written, 6u);
    EXPECT_EQ(out[0], 0x8B);
    EXPECT_EQ(out[1], 0x05);

    // The rewritten displacement must land on the same absolute address the
    // original would have reached: old_pc + length + 0.
    std::int32_t new_disp = 0;
    std::memcpy(&new_disp, out + 2, 4);
    const auto reached = reinterpret_cast<std::uintptr_t>(new_pc) + 6 +
                         static_cast<std::intptr_t>(new_disp);
    EXPECT_EQ(reached, reinterpret_cast<std::uintptr_t>(old_pc) + 6);
}

TEST(X86Disasm, relocate_rip_relative_refuses_beyond_rel32) {
    const auto info = rip_relative_info();
    ASSERT_EQ(info.type, InstructionType::RipRelative);

    const std::uint8_t src[] = {0x8B, 0x05, 0x00, 0x00, 0x00, 0x00};
    const auto old_pc = reinterpret_cast<const void*>(0x00005646043D6000ULL);
    // 45 TB away — the separation actually measured between a Linux
    // executable's code and an mmap with no address hint, in this very test
    // binary. See tests/test_near_alloc.cpp.
    const auto new_pc = reinterpret_cast<const void*>(0x00007FDDC3747000ULL);

    std::uint8_t out[16]{0xCC};
    EXPECT_EQ(relocate_instruction(info, src, old_pc, new_pc, out), 0u)
        << "a displacement that cannot fit in int32 must be refused, not truncated";
}

TEST(X86Disasm, relocate_call_rel32_refuses_beyond_rel32) {
    // Same boundary for a direct call, which is what a prologue that begins by
    // calling a stack-check or profiling helper looks like.
    const std::uint8_t src[] = {0xE8, 0x00, 0x00, 0x00, 0x00};
    const auto info = analyze_instruction(src, sizeof(src));
    ASSERT_EQ(info.type, InstructionType::CallRel32);

    const auto old_pc = reinterpret_cast<const void*>(0x00005646043D6000ULL);
    std::uint8_t out[16]{};

    // Near: fine.
    EXPECT_EQ(relocate_instruction(
                  info, src, old_pc,
                  reinterpret_cast<const void*>(0x00005646043D5000ULL), out),
              5u);
    // Far: refused.
    EXPECT_EQ(relocate_instruction(
                  info, src, old_pc,
                  reinterpret_cast<const void*>(0x00007FDDC3747000ULL), out),
              0u);
}

// ─── calculate_copy_size ───────────────────────────────────────────────────

TEST(X86Disasm, calculate_copy_size_covers_typical_prologue) {
    // push rbp; mov rbp, rsp; sub rsp, 0x20; push rbx; push r12; push r13
    // 1  + 3            + 4               + 1      + 2      + 2 = 13 — too short
    // Add one more 4-byte insn to exceed 16 bytes total.
    const std::uint8_t bytes[] = {
        0x55,                          // push rbp             (1)
        0x48, 0x89, 0xE5,              // mov rbp, rsp         (3)  total 4
        0x48, 0x83, 0xEC, 0x20,        // sub rsp, 0x20        (4)  total 8
        0x53,                          // push rbx             (1)  total 9
        0x41, 0x54,                    // push r12             (2)  total 11
        0x41, 0x55,                    // push r13             (2)  total 13
        0x48, 0x89, 0x7D, 0xF8,        // mov [rbp-8], rdi     (4)  total 17
        0x90,                          // padding nop
    };
    auto size = calculate_copy_size(bytes);
    EXPECT_GE(size, 16u);
    EXPECT_EQ(size, 17u);  // ends at the natural instruction boundary
}

TEST(X86Disasm, calculate_copy_size_walks_only_as_far_as_the_patch_needs) {
    // The very same prologue as above. Asked for 16 it answers 17, which does
    // not fit the 16-byte record splice_disable restores from -- that is how a
    // perfectly ordinary MSVC entry sequence used to arrive with disable
    // switched off. Asked for 5, which is what an install through a relay
    // actually writes, one instruction is enough.
    const std::uint8_t bytes[] = {
        0x55,                          // push rbp             (1)
        0x48, 0x89, 0xE5,              // mov rbp, rsp         (3)  total 4
        0x48, 0x83, 0xEC, 0x20,        // sub rsp, 0x20        (4)  total 8
        0x53,                          // push rbx             (1)  total 9
        0x41, 0x54,                    // push r12             (2)  total 11
        0x41, 0x55,                    // push r13             (2)  total 13
        0x48, 0x89, 0x7D, 0xF8,        // mov [rbp-8], rdi     (4)  total 17
        0x90,                          // padding nop
    };

    EXPECT_EQ(calculate_copy_size(bytes, 16), 17u);   // the old worst case
    EXPECT_EQ(calculate_copy_size(bytes, 5), 8u);     // push+mov+sub = 8 >= 5
    EXPECT_EQ(calculate_copy_size(bytes), 17u);       // default is still 16

    // Never short of what was asked for: the patcher writes patch_len bytes
    // over the prologue, so a result below min_bytes would mean patching past
    // the instructions that were moved aside.
    for (std::size_t want : {1u, 5u, 8u, 14u, 16u}) {
        EXPECT_GE(calculate_copy_size(bytes, want), want) << "want=" << want;
    }
}
