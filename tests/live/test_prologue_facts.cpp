// ─── splice_prologue_profile — the question a consumer's admission asks ────
//
// The shipping overlay refuses to patch its two hottest targets unless the
// patch window is a single instruction. That is stricter than this library
// requires -- the coordinated tier maps an interior program counter through
// the boundary map rather than refusing it -- and the stricter rule is the
// consumer's to keep: relaxing a safe-admission rule is a decision recorded
// on that side, not one this library makes by withdrawing the facts.
//
// Until now the only way to ask was to build a `PreparedStrictPatch`, which
// is a private arch type, so the consumer reached across the submodule
// boundary into `src/` for it. That is the coupling one deletion broke. These
// are the same facts, asked through the public ABI.
//
// The assertions here are arithmetic rather than magic numbers wherever they
// can be, because the lengths belong to whatever the compiler emitted today.
// Two of them are not: a hand-written body whose first instruction is five
// bytes, and one whose first instruction is shorter, so both answers to the
// consumer's question are produced on purpose rather than hoped for.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/engine.h>
#include <splice/log.h>

#include "test_targets.h"

#include <cstdint>
#include <cstring>

namespace {

// mov eax, 0x11223344   (B8 44 33 22 11)   five bytes, then ret.
// The patch window is five bytes, so it is exactly this one instruction and
// there is no interior boundary -- the shape the overlay admits.
alignas(16) unsigned char g_one_instruction_entry[] = {
    0xB8, 0x44, 0x33, 0x22, 0x11,   // mov eax, imm32
    0xC3,                            // ret
};

// add eax, 4 / add eax, 1 / ret   (83 C0 04 | 83 C0 01 | C3)
// Three-byte instructions, so covering five bytes takes two of them and the
// start of the second lies inside the window -- the shape it refuses.
alignas(16) unsigned char g_interior_boundary[] = {
    0x83, 0xC0, 0x04,
    0x83, 0xC0, 0x01,
    0xC3,
};

} // namespace

TEST(PrologueFacts, a_five_byte_entry_has_no_interior_boundary) {
    splice_prologue_facts facts{};
    ASSERT_EQ(splice_prologue_profile(g_one_instruction_entry, &facts), 0);
    EXPECT_EQ(facts.first_instruction, 5u);
    EXPECT_EQ(facts.patch_bytes, 5u);
    EXPECT_EQ(facts.interior_boundaries, 0u)
        << "a window that is one instruction cannot have a boundary inside it";
}

TEST(PrologueFacts, a_short_entry_puts_a_boundary_inside_the_window) {
    splice_prologue_facts facts{};
    ASSERT_EQ(splice_prologue_profile(g_interior_boundary, &facts), 0);
    EXPECT_EQ(facts.first_instruction, 3u);
    EXPECT_EQ(facts.patch_bytes, 6u) << "five bytes rounded up to whole instructions";
    EXPECT_EQ(facts.interior_boundaries, 1u)
        << "the second instruction starts at offset 3, inside the window";
}

TEST(PrologueFacts, the_window_is_always_whole_instructions_and_covers_the_patch) {
    // The arithmetic that must hold for any prologue that decodes at all, so
    // this one runs against a real compiled function rather than a fixture.
    splice_prologue_facts facts{};
    ASSERT_EQ(splice_prologue_profile(
                  reinterpret_cast<const void*>(&splice::test::batch_target_a), &facts), 0);
    EXPECT_GE(facts.patch_bytes, 5u) << "the window must cover the patch";
    EXPECT_GE(facts.patch_bytes, facts.first_instruction)
        << "the window cannot be shorter than its first instruction";
    EXPECT_EQ(facts.interior_boundaries == 0u, facts.patch_bytes == facts.first_instruction)
        << "no interior boundary is the same statement as one instruction";
}

TEST(PrologueFacts, a_thunk_is_followed_so_the_facts_describe_the_function) {
    // `&batch_target_a` is an ILT entry under incremental linking -- a bare
    // `E9 rel32`, five bytes, which would answer "one instruction, no interior
    // boundary" about the thunk and say nothing about the function. The facts
    // have to be about what gets patched.
    //
    // Measured rather than asserted: whether this build routes through a thunk
    // depends on the linker. When it does, the profile must not report the
    // thunk's own shape; when it does not, there is nothing to follow and the
    // case still proves the arithmetic above on a real function.
    const auto* entry = reinterpret_cast<const std::uint8_t*>(&splice::test::batch_target_a);
    splice_prologue_facts facts{};
    ASSERT_EQ(splice_prologue_profile(entry, &facts), 0);

    if (entry[0] == 0xE9) {
        std::int32_t rel = 0;
        std::memcpy(&rel, entry + 1, sizeof(rel));
        const auto* behind = entry + 5 + rel;
        splice_prologue_facts direct{};
        ASSERT_EQ(splice_prologue_profile(behind, &direct), 0);
        EXPECT_EQ(facts.patch_bytes, direct.patch_bytes)
            << "the thunk was not followed";
        EXPECT_EQ(facts.first_instruction, direct.first_instruction);
        SPLICE_LOGI("PrologueFacts: followed a thunk at %p to %p",
                    static_cast<const void*>(entry), static_cast<const void*>(behind));
    } else {
        SPLICE_LOGI("PrologueFacts: no thunk in this build; %p is the function",
                    static_cast<const void*>(entry));
    }
}

TEST(PrologueFacts, an_address_that_names_nothing_is_refused_not_read) {
    splice_prologue_facts facts{};
    facts.patch_bytes = 0xDEADu;
    // A page that is not mapped. The decoder would fault on it, so the
    // readability question is asked first -- the same repair the install paths
    // took when following a thunk started decoding caller-supplied bytes.
    EXPECT_EQ(splice_prologue_profile(reinterpret_cast<const void*>(0x10000), &facts), -1);
    EXPECT_EQ(facts.patch_bytes, 0xDEADu) << "a refusal wrote to the output";

    EXPECT_EQ(splice_prologue_profile(nullptr, &facts), -1);
    splice_prologue_facts* const nowhere = nullptr;
    EXPECT_EQ(splice_prologue_profile(g_one_instruction_entry, nowhere), -1);
}
