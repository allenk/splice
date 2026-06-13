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
