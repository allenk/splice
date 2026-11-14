// ─── A slot swap that honours what the caller read ────────────────────────
//
// The group install reads a slot in one phase to learn the original function,
// publishes that original where the hook will look for it, and stores the new
// pointer in a later phase. Everything here is about the gap between those two
// moments: another overlay can take the slot inside it, and an unconditional
// store would overwrite them while publishing an original that predates them,
// so calling the "original" would skip their hook. Neither is visible
// afterwards from the bytes.
//
// The slots below are a plain array rather than a real function table. The
// entry point takes a `void**`, the comparison is on the pointer value, and a
// synthetic slot can be put into states a real table cannot be talked into.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/engine.h>

namespace {

// Three distinguishable addresses. Their contents never run; only their
// identity matters.
int original_function(int v) { return v; }
int our_hook(int v) { return v + 1; }
int somebody_elses_hook(int v) { return v + 2; }

void* as_ptr(int (*fn)(int)) { return reinterpret_cast<void*>(fn); }

// A one-slot table, aligned as a pointer so the atomic access is legal.
struct Table {
    void* slot[1];
    explicit Table(void* initial) { slot[0] = initial; }
    void** cell() { return &slot[0]; }
};

} // namespace

TEST(VtableSwapExpect, a_matching_slot_is_replaced_and_reports_what_it_matched) {
    Table table(as_ptr(original_function));
    void* found = nullptr;
    splice_patch_record record{};

    ASSERT_EQ(0, splice_vtable_swap_expect(table.cell(), as_ptr(original_function),
                                           as_ptr(our_hook), &found, &record));
    EXPECT_EQ(as_ptr(our_hook), table.slot[0]);
    EXPECT_EQ(as_ptr(original_function), found);
    EXPECT_EQ(SPLICE_PATCH_STRATEGY_DATA_SLOT, record.strategy);
    EXPECT_EQ(static_cast<void*>(table.cell()), record.hook_site);
    EXPECT_EQ(as_ptr(original_function), record.pre_hook_pointer);
}

// The reason this entry point exists. With an unconditional store this passes
// the install and silently evicts the other overlay.
TEST(VtableSwapExpect, a_slot_somebody_else_took_is_refused_and_left_alone) {
    Table table(as_ptr(original_function));

    // What a competing overlay does between our read and our write.
    table.slot[0] = as_ptr(somebody_elses_hook);

    void* found = nullptr;
    splice_patch_record record{};
    record.strategy = -1;
    ASSERT_EQ(-1, splice_vtable_swap_expect(table.cell(), as_ptr(original_function),
                                            as_ptr(our_hook), &found, &record));
    EXPECT_EQ(as_ptr(somebody_elses_hook), table.slot[0])
        << "the other overlay's pointer must still be there";
    EXPECT_EQ(as_ptr(somebody_elses_hook), found)
        << "the caller is told what it actually found, not what it asked for";
    EXPECT_EQ(-1, record.strategy)
        << "a refusal fills no record";
}

TEST(VtableSwapExpect, the_matched_value_is_what_disable_puts_back) {
    Table table(as_ptr(original_function));
    splice_patch_record record{};
    ASSERT_EQ(0, splice_vtable_swap_expect(table.cell(), as_ptr(original_function),
                                           as_ptr(our_hook), nullptr, &record));
    ASSERT_EQ(as_ptr(our_hook), table.slot[0]);

    ASSERT_EQ(0, splice_disable(&record));
    EXPECT_EQ(as_ptr(original_function), table.slot[0]);
}

// A restore is deliberately unconditional -- the reason to put the original
// back is that something failed, and refusing because the slot does not look
// the way we expect would leave a hook installed on exactly the path that
// exists to remove it. Same argument the inline tier's restore makes.
TEST(VtableSwapExpect, disable_puts_the_original_back_even_over_a_stranger) {
    Table table(as_ptr(original_function));
    splice_patch_record record{};
    ASSERT_EQ(0, splice_vtable_swap_expect(table.cell(), as_ptr(original_function),
                                           as_ptr(our_hook), nullptr, &record));
    table.slot[0] = as_ptr(somebody_elses_hook);

    EXPECT_EQ(0, splice_disable(&record));
    EXPECT_EQ(as_ptr(original_function), table.slot[0]);
}

TEST(VtableSwapExpect, a_null_slot_or_replacement_is_refused) {
    Table table(as_ptr(original_function));
    EXPECT_EQ(-1, splice_vtable_swap_expect(nullptr, as_ptr(original_function),
                                            as_ptr(our_hook), nullptr, nullptr));
    EXPECT_EQ(-1, splice_vtable_swap_expect(table.cell(), as_ptr(original_function),
                                            nullptr, nullptr, nullptr));
    EXPECT_EQ(as_ptr(original_function), table.slot[0]);
}

// A slot holding null is a legitimate state to expect, so null cannot be a
// sentinel for "do not compare".
TEST(VtableSwapExpect, an_empty_slot_can_be_the_expected_value) {
    Table table(nullptr);
    void* found = &table;   // something non-null, to prove it is overwritten
    ASSERT_EQ(0, splice_vtable_swap_expect(table.cell(), nullptr,
                                           as_ptr(our_hook), &found, nullptr));
    EXPECT_EQ(as_ptr(our_hook), table.slot[0]);
    EXPECT_EQ(nullptr, found);
}
