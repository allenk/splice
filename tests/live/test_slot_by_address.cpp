// ─── Naming the vtable slot itself, with no object to read it out of ──────
//
// `splice::Vtable` takes an instance and reads the table pointer out of it.
// That covers the caller who has an object, which is most of them, and it
// cannot cover the caller who has only an address: one that resolved the slot
// in another process, translated it across module bases, and is installing
// before any instance of the interface exists in this one. `at_slot` is for
// that caller, and these cases are what it owes them.
//
// Two things are being checked and they are different. First, that naming the
// slot reaches the same site as naming the object -- which is why the slot's
// address comes from inside the provider module rather than from arithmetic
// the test does itself, since two copies of the same arithmetic agreeing
// proves nothing. Second, that `.expect` is load-bearing here in a way it is
// not for `Vtable`: nothing in the library can tell whether a slot address
// handed to it is right, so the comparison against an independently obtained
// function address is the whole of the admission, and a disagreement has to
// write nothing at all.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/live.h>

#include "vtable_provider.h"

#include <atomic>
#include <cstring>

namespace {

using splice::test::ICalculator;

std::atomic<int> g_scale_calls{0};

void reset_counters() { g_scale_calls.store(0); }

int slot_of_scale() {
    auto pmf = &ICalculator::scale;
    unsigned char raw[sizeof(pmf)];
    std::memcpy(raw, &pmf, sizeof(pmf));
    return splice_vtable_slot_of_pmf(raw, static_cast<unsigned int>(sizeof(pmf)));
}

using ScaleFn = int (*)(ICalculator*, int);

class SlotByAddress : public ::testing::Test {
protected:
    void SetUp() override {
        reset_counters();
        splice::test::calculator()->set_factor(3);
    }

    void TearDown() override {
        if (m_outcome.active()) EXPECT_TRUE(m_outcome.disable());
        splice::test::calculator()->set_factor(3);
    }

    splice::Outcome m_outcome;
};

} // namespace

TEST_F(SlotByAddress, a_slot_named_by_its_address_is_swapped_and_put_back) {
    ICalculator* calc = splice::test::calculator();
    const int index = slot_of_scale();
    ASSERT_GE(index, 0);
    void** const slot = splice::test::table_slot(index);
    ASSERT_NE(slot, nullptr);
    void* const original = splice::test::table_entry(index);

    m_outcome = splice::Live<>{}
        .at_slot(slot, ScaleFn{}, [](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value) + 1;
        })
        .install(splice::slot_swap);

    ASSERT_EQ(m_outcome.state(), splice::State::installed)
        << "op=" << static_cast<int>(m_outcome.failure().operation);
    EXPECT_EQ(calc->scale(5), 16);                 // 5*3 then +1 from the callback
    EXPECT_EQ(g_scale_calls.load(), 1);

    // The slot, not the function: the provider's own entry has moved.
    EXPECT_NE(splice::test::table_entry(index), original);

    ASSERT_TRUE(m_outcome.disable());
    EXPECT_EQ(splice::test::table_entry(index), original);
    EXPECT_EQ(calc->scale(5), 15);
}

// The one assertion that proves `at_slot` does the same arithmetic the object
// route does. Both are installed in turn against the same method, and the
// address the provider reports has to move for each of them.
TEST_F(SlotByAddress, naming_the_slot_reaches_the_same_site_as_naming_the_object) {
    ICalculator* calc = splice::test::calculator();
    const int index = slot_of_scale();
    ASSERT_GE(index, 0);
    void* const original = splice::test::table_entry(index);

    splice::Outcome by_object = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale, [](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value);
        })
        .install(splice::slot_swap);
    ASSERT_EQ(by_object.state(), splice::State::installed);
    void* const through_object = splice::test::table_entry(index);
    EXPECT_NE(through_object, original);
    ASSERT_TRUE(by_object.disable());
    ASSERT_EQ(splice::test::table_entry(index), original);

    m_outcome = splice::Live<>{}
        .at_slot(splice::test::table_slot(index), ScaleFn{},
                 [](auto orig, ICalculator* self, int value) {
                     g_scale_calls.fetch_add(1);
                     return orig(self, value);
                 })
        .install(splice::slot_swap);
    ASSERT_EQ(m_outcome.state(), splice::State::installed);

    // Not the same stub -- each group claims its own -- but the same cell,
    // which is the claim: `table_slot(index)` addresses what `.on(pmf)`
    // resolves to.
    EXPECT_NE(splice::test::table_entry(index), original);
    EXPECT_EQ(calc->scale(5), 15);
    EXPECT_EQ(g_scale_calls.load(), 1);
}

// The admission rule, in the smallest form it has. A slot address the caller
// computed wrongly would otherwise be a pointer-sized write to somewhere it
// had no business being, and `.expect` is what turns that into a refusal.
TEST_F(SlotByAddress, a_slot_that_does_not_hold_the_expected_function_is_refused) {
    ICalculator* calc = splice::test::calculator();
    const int index = slot_of_scale();
    ASSERT_GE(index, 0);
    void** const slot = splice::test::table_slot(index);
    void* const original = splice::test::table_entry(index);

    m_outcome = splice::Live<>{}
        .at_slot(slot, ScaleFn{})
        .expect(reinterpret_cast<void*>(&reset_counters))
        .onInvoke([](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value) + 1;
        })
        .install(splice::slot_swap);

    EXPECT_EQ(m_outcome.state(), splice::State::refused);
    EXPECT_EQ(m_outcome.failure().operation, splice::Operation::read_slot);
    EXPECT_EQ(splice::test::table_entry(index), original)
        << "a refusal writes nothing";
    EXPECT_EQ(calc->scale(5), 15);
    EXPECT_EQ(g_scale_calls.load(), 0);
}

// And the agreeing case, stated separately, because a refusal test passes just
// as well against a form that refuses everything.
TEST_F(SlotByAddress, the_expected_function_is_the_one_the_slot_holds) {
    ICalculator* calc = splice::test::calculator();
    const int index = slot_of_scale();
    ASSERT_GE(index, 0);

    m_outcome = splice::Live<>{}
        .at_slot(splice::test::table_slot(index), ScaleFn{})
        .expect(splice::test::table_entry(index))
        .onInvoke([](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value) + 1;
        })
        .install(splice::slot_swap);

    ASSERT_EQ(m_outcome.state(), splice::State::installed);
    EXPECT_EQ(calc->scale(5), 16);
    EXPECT_EQ(g_scale_calls.load(), 1);
}

TEST_F(SlotByAddress, a_null_slot_address_is_refused_before_anything_is_claimed) {
    m_outcome = splice::Live<>{}
        .at_slot(nullptr, ScaleFn{}, [](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value);
        })
        .install(splice::slot_swap);

    EXPECT_EQ(m_outcome.state(), splice::State::refused);
    EXPECT_EQ(m_outcome.failure().operation, splice::Operation::resolve_slot);
    EXPECT_EQ(splice::test::calculator()->scale(5), 15);
}

// The tier distinction, for a site named this way. The coordinated tier reads
// the slot to find the function and then patches the function, so the slot
// must come back unchanged -- the same observation the Vtable tests make, and
// the only one that separates "the coordinated tier installed" from "something
// quietly did a slot swap instead".
TEST_F(SlotByAddress, the_coordinated_tier_reads_the_slot_and_patches_what_it_finds) {
    if (!splice::live_tier_available()) {
        GTEST_SKIP() << "the coordinated tier needs an architecture whose "
                        "prologue can be planned and an OS that can suspend a "
                        "thread; this build has one or neither";
    }
    ICalculator* calc = splice::test::calculator();
    const int index = slot_of_scale();
    ASSERT_GE(index, 0);
    void* const original = splice::test::table_entry(index);

    m_outcome = splice::Live<>{}
        .at_slot(splice::test::table_slot(index), ScaleFn{},
                 [](auto orig, ICalculator* self, int value) {
                     g_scale_calls.fetch_add(1);
                     return orig(self, value) + 1;
                 })
        .install(splice::live);

    ASSERT_EQ(m_outcome.state(), splice::State::installed)
        << "stage=" << static_cast<int>(m_outcome.failure().stage)
        << " op=" << static_cast<int>(m_outcome.failure().operation)
        << " err=" << m_outcome.failure().os_error;

    EXPECT_EQ(calc->scale(5), 16);
    EXPECT_EQ(g_scale_calls.load(), 1);
    EXPECT_EQ(splice::test::table_entry(index), original)
        << "the call arrived through a patched prologue, not a swapped pointer";
}

// ─── .replace_with() on a site named by its slot ──────────────────────────
//
// The verb for a caller whose own function is the hook, so splice wraps
// nothing and publishes the displaced pointer into the caller's atomic. It
// reaches the group through `adopt_site` rather than `add_pending`, and until
// `at_slot` existed nothing had ever combined the two: every site in the
// shipping consumer was named by address, and the tests above this line all
// use `.onInvoke`. `adopt_site` took a target and no slot, so this
// combination refused in prepare with "names neither a slot nor an address"
// -- for `Vtable` as much as for `at_slot`, since a method named by a
// pointer-to-member also has a null target.
//
// The replacement has to be a real function with a real body: `replace_with`
// puts it in the call path, so the provider's method dispatches straight into
// it.

std::atomic<void*> g_scale_original{nullptr};

int replaced_scale(ICalculator* self, int value) {
    g_scale_calls.fetch_add(1);
    auto original = reinterpret_cast<ScaleFn>(
        g_scale_original.load(std::memory_order_acquire));
    return original == nullptr ? -1 : original(self, value) + 100;
}

class SlotReplaceWith : public ::testing::Test {
protected:
    void SetUp() override {
        reset_counters();
        g_scale_original.store(nullptr, std::memory_order_release);
        splice::test::calculator()->set_factor(3);
    }

    void TearDown() override {
        if (m_outcome.active()) EXPECT_TRUE(m_outcome.disable());
        splice::test::calculator()->set_factor(3);
    }

    splice::Outcome m_outcome;
};

TEST_F(SlotReplaceWith, a_slot_named_by_address_takes_the_callers_own_replacement) {
    ICalculator* calc = splice::test::calculator();
    const int index = slot_of_scale();
    ASSERT_GE(index, 0);
    void* const original = splice::test::table_entry(index);

    m_outcome = splice::Live<>{}
        .at_slot(splice::test::table_slot(index), ScaleFn{})
        .expect(original)
        .replace_with(&replaced_scale, g_scale_original)
        .install(splice::slot_swap);

    ASSERT_EQ(m_outcome.state(), splice::State::installed)
        << "stage=" << static_cast<int>(m_outcome.failure().stage)
        << " op=" << static_cast<int>(m_outcome.failure().operation);

    // Published before the write, which is what makes the first call safe.
    EXPECT_EQ(g_scale_original.load(std::memory_order_acquire), original);
    // And the cell holds our function itself, not a splice stub, because that
    // is what this verb means.
    EXPECT_EQ(splice::test::table_entry(index), reinterpret_cast<void*>(&replaced_scale));

    EXPECT_EQ(calc->scale(5), 115);                // 5*3 then +100
    EXPECT_EQ(g_scale_calls.load(), 1);

    ASSERT_TRUE(m_outcome.disable());
    EXPECT_EQ(splice::test::table_entry(index), original);
    EXPECT_EQ(calc->scale(5), 15);
}

// `.expect` has to be honoured on this route too. It travels through a
// different function than it does for `.onInvoke`, so a fix that carried the
// slot and forgot the expectation would pass the test above and install over
// another overlay here.
TEST_F(SlotReplaceWith, the_expectation_still_refuses_a_slot_that_moved) {
    ICalculator* calc = splice::test::calculator();
    const int index = slot_of_scale();
    ASSERT_GE(index, 0);
    void* const original = splice::test::table_entry(index);

    m_outcome = splice::Live<>{}
        .at_slot(splice::test::table_slot(index), ScaleFn{})
        .expect(reinterpret_cast<void*>(&reset_counters))
        .replace_with(&replaced_scale, g_scale_original)
        .install(splice::slot_swap);

    EXPECT_EQ(m_outcome.state(), splice::State::refused);
    EXPECT_EQ(m_outcome.failure().operation, splice::Operation::read_slot);
    EXPECT_EQ(splice::test::table_entry(index), original);
    EXPECT_EQ(g_scale_original.load(std::memory_order_acquire), nullptr);
    EXPECT_EQ(calc->scale(5), 15);
}

// The other route to a slot-named site, which had the same hole for the same
// reason: a method named by a pointer-to-member has a null target too.
TEST_F(SlotReplaceWith, a_method_named_by_pointer_to_member_takes_one_too) {
    ICalculator* calc = splice::test::calculator();
    const int index = slot_of_scale();
    ASSERT_GE(index, 0);
    void* const original = splice::test::table_entry(index);

    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale)
        .replace_with(&replaced_scale, g_scale_original)
        .install(splice::slot_swap);

    ASSERT_EQ(m_outcome.state(), splice::State::installed)
        << "stage=" << static_cast<int>(m_outcome.failure().stage)
        << " op=" << static_cast<int>(m_outcome.failure().operation);
    EXPECT_EQ(g_scale_original.load(std::memory_order_acquire), original);
    EXPECT_EQ(calc->scale(5), 115);
    EXPECT_EQ(g_scale_calls.load(), 1);
}

// ─── One site named twice ──────────────────────────────────────────────────
//
// The group used to admit it. Installation writes `original -> first stub ->
// second stub` and records the original and the first stub as the two displaced
// values; `disable()` restores in the order it installed, so it puts back the
// original and then the first stub, and the slot ends up holding a stub that has
// just been returned to the pool and may already be bound to a different
// callback. The group reported success.
//
// `at_slot` makes the shape easy to write, which is why these cases live here.
// It is not what created it: naming the same method twice through `Vtable`
// arrives at the same loops. Nothing about it needs a competing writer or a
// thread at an awkward moment -- it is two lines of a fluent chain.
namespace {

int duplicate_target(int value) { return value * 2; }
int other_target(int value) { return value * 3; }
using DupFn = int (*)(int);

} // namespace

TEST_F(SlotByAddress, one_slot_named_twice_is_refused_before_anything_is_written) {
    const int index = slot_of_scale();
    ASSERT_GE(index, 0);
    void** const slot = splice::test::table_slot(index);
    ASSERT_NE(slot, nullptr);
    void* const original = splice::test::table_entry(index);

    m_outcome = splice::Live<>{}
        .at_slot(slot, ScaleFn{}, [](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value) + 1;
        })
        .at_slot(slot, ScaleFn{}, [](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value) + 2;
        })
        .install(splice::slot_swap);

    EXPECT_EQ(m_outcome.state(), splice::State::refused)
        << "refused, not installed and then undone -- undoing it is the defect";
    EXPECT_EQ(m_outcome.failure().stage, splice::Stage::prepare)
        << "and refused before anything was written";
    EXPECT_FALSE(m_outcome.active());

    // The cell is untouched and the method still does what the provider wrote.
    EXPECT_EQ(splice::test::table_entry(index), original);
    EXPECT_EQ(splice::test::calculator()->scale(5), 15);
    EXPECT_EQ(g_scale_calls.load(), 0) << "neither callback was ever reachable";
}

// The same check on the shape the shipping consumer actually writes: every site
// named by address. The refusal lives in the admission both tiers share, which
// runs before the coordinated tier allocates, suspends, or asks whether it is
// available on this build at all -- so this case needs no live tier and no skip,
// and it names `splice::live` in order to assert exactly that.
// It also names the clash as the first and the third site rather than two
// adjacent ones, which is the difference between comparing every pair and
// comparing neighbours. Both of those detect the case above; only one of them
// detects this, and without it the pairwise comparison would be an untested
// claim in a comment.
TEST_F(SlotByAddress, one_target_named_twice_is_refused_too) {
    void* const target = reinterpret_cast<void*>(&duplicate_target);
    void* const other  = reinterpret_cast<void*>(&other_target);

    splice::Outcome outcome = splice::Live<>{}
        .at(target, DupFn{}, [](auto orig, int value) { return orig(value); })
        .at(other,  DupFn{}, [](auto orig, int value) { return orig(value); })
        .at(target, DupFn{}, [](auto orig, int value) { return orig(value) + 1; })
        .install(splice::live);

    EXPECT_EQ(outcome.state(), splice::State::refused);
    EXPECT_EQ(outcome.failure().stage, splice::Stage::prepare)
        << "the shared admission, not the tier's own work";
    EXPECT_FALSE(outcome.active());
}
