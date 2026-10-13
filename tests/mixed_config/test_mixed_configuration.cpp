// ─── a mixed-configuration program, which must link and work ──────────────
//
// This is the fixture that held publication. The configuration the library was
// built with had never been asserted by anything: a refactor dropping one
// CMake line, or one `#define` in a consumer's header, would have restored the
// original defect -- a consumer reading the library's shared `HookContext` at
// the wrong offsets, with no diagnostic, until a function pointer taken from
// the wrong place was called.
//
// Three mechanisms for *detecting* that were designed and refuted in review:
//
//   1. compare `sizeof(HookContext)` at first use. The comparison cannot be
//      made reliably private to one translation unit -- one specialisation
//      identity is shared across all of them -- and equal size never meant
//      equal layout anyway.
//   2. put a configuration tag in the mangled names. A non-member function's
//      linker name does not include its return type, and an untagged inline
//      caller in the consumer's own header defeats a tagged callee.
//   3. bake a fingerprint into each macro expansion. It is discarded along
//      with the body that carried it, so a coalesced inline wrapper hands the
//      surviving configuration's value to a library that agrees with it.
//
// So the dependency was removed instead. The capacity lives behind a pointer
// and a runtime count inside the library; no header-visible layout, inline
// expression or template argument mentions it; and the two translation units
// linked into this program disagree about it on purpose.
//
// The assertion is therefore **that it works**, which is a stronger statement
// than a link failure and one that can fail for a real reason: if any
// header-side bound were left, the sibling compiled with 64 would refuse or
// misread a slot above 64 that the library considers valid.
#include "mixed_config.h"

#include <gtest/gtest.h>

#include <splice/context.h>

using splice_mixed_config::Report;

TEST(MixedConfiguration, the_two_translation_units_disagree_about_the_capacity) {
    // `small` is a macro on Windows -- rpcndr.h defines it as `char` -- which
    // is a good reminder that a test is a consumer and lives in the same
    // preprocessor as everyone else.
    const Report narrow = splice_mixed_config::from_small_capacity_tu();
    const Report wide = splice_mixed_config::from_default_capacity_tu();

    // The premise. If these were equal the test would be asserting nothing.
    ASSERT_NE(narrow.compiled_with, wide.compiled_with)
        << "both translation units were compiled with the same capacity, so "
           "this fixture is not testing a mixed build";
    EXPECT_EQ(narrow.compiled_with, 64);
}

TEST(MixedConfiguration, every_view_of_the_context_layout_agrees) {
    const Report narrow = splice_mixed_config::from_small_capacity_tu();
    const Report wide = splice_mixed_config::from_default_capacity_tu();

    // The property, from the only side that matters: what each translation
    // unit computes, and what the library reports, are one number.
    EXPECT_EQ(narrow.sizeof_context, narrow.library_context_size);
    EXPECT_EQ(wide.sizeof_context, wide.library_context_size);
    EXPECT_EQ(narrow.sizeof_context, wide.sizeof_context)
        << "the capacity is back in the ABI: two translation units that "
           "disagree about it see different layouts";

    // And the shared inline function in the header -- the shape a
    // configuration tag cannot reach -- sees the same thing.
    EXPECT_EQ(splice_mixed_config::shared_inline_view_of_the_context(),
              narrow.sizeof_context);
}

TEST(MixedConfiguration, the_capacity_is_the_librarys_at_runtime_in_both) {
    const Report narrow = splice_mixed_config::from_small_capacity_tu();
    const Report wide = splice_mixed_config::from_default_capacity_tu();

    EXPECT_EQ(narrow.configured_capacity, wide.configured_capacity);
    EXPECT_EQ(narrow.configured_capacity, splice::detail::configured_capacity());

    // The macro a translation unit was compiled with has no say in it, which
    // is the whole mechanism.
    EXPECT_NE(narrow.configured_capacity, narrow.compiled_with)
        << "if the library were built at 64 this test cannot distinguish the "
           "mechanism from a coincidence; rebuild it with another capacity";
}

TEST(MixedConfiguration, both_registries_round_trip_above_the_smaller_macro) {
    const Report narrow = splice_mixed_config::from_small_capacity_tu();

    // The id a leftover header-side bound would have refused: above the
    // sibling's own macro value, inside the library's real capacity.
    ASSERT_GT(narrow.slot_used, narrow.compiled_with - 1);
    ASSERT_LT(narrow.slot_used, narrow.configured_capacity);

    EXPECT_EQ(narrow.set_original, splice::Refusal::ok);
    EXPECT_EQ(narrow.get_original, splice::Refusal::ok);
    EXPECT_TRUE(narrow.original_round_trips);

    EXPECT_EQ(narrow.get_hook, splice::Refusal::ok);
    EXPECT_TRUE(narrow.hook_is_usable);
    EXPECT_TRUE(narrow.hook_dispatches)
        << "a hook published from the translation unit compiled with 64 did "
           "not dispatch at a slot above 64";
}

TEST(MixedConfiguration, the_other_translation_unit_round_trips_too) {
    const Report wide = splice_mixed_config::from_default_capacity_tu();

    EXPECT_EQ(wide.set_original, splice::Refusal::ok);
    EXPECT_EQ(wide.get_original, splice::Refusal::ok);
    EXPECT_TRUE(wide.original_round_trips);
    EXPECT_EQ(wide.get_hook, splice::Refusal::ok);
    EXPECT_TRUE(wide.hook_dispatches);
}
