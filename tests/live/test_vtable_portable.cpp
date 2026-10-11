// ─── The portable vtable runner ───────────────────────────────────────────
//
// Design section 8.2: the `live` tier is Windows-only on day one, but the API
// must not be. This runner drives the whole user-facing chain -- pointer-to-
// member to slot index, the group transaction, the per-method gates, the
// group-wide re-entrancy scope, all-or-nothing recovery and disable -- against
// a real C++ interface whose implementation and function table live in a
// separate shared library, on every platform splice builds for.
//
// Every suppression claim here is paired with a control that proves the thing
// being suppressed really happens without the gate. A test that only asserts
// "the callback did not fire twice" is also satisfied by a callback that could
// never have fired twice in the first place.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/live.h>

#include "vtable_provider.h"

#include <atomic>
#include <cstring>
#include <vector>

namespace {

using splice::test::ICalculator;

// Counters live at namespace scope because the callbacks are plain lambdas
// with no captured state -- a hook outlives the chain that installed it, so
// capturing a fixture member by reference would be a dangling read the moment
// a test returned without disabling.
std::atomic<int> g_scale_calls{0};
std::atomic<int> g_twice_calls{0};
std::atomic<int> g_add_calls{0};

void reset_counters() {
    g_scale_calls.store(0);
    g_twice_calls.store(0);
    g_add_calls.store(0);
}

class VtableGroup : public ::testing::Test {
protected:
    void SetUp() override {
        reset_counters();
        splice::test::calculator()->set_factor(3);
    }

    void TearDown() override {
        // A group that is never disabled keeps its stubs for the life of the
        // process, by the same rule that makes a trampoline unfreeable. In a
        // test binary that would exhaust the pool and the failure would land
        // on whichever test ran eighth.
        if (m_outcome.active()) {
            EXPECT_TRUE(m_outcome.disable());
        }
        splice::test::calculator()->set_factor(3);
    }

    splice::Outcome m_outcome;
};

} // namespace

TEST_F(VtableGroup, a_group_hooks_every_method_it_names) {
    ICalculator* calc = splice::test::calculator();

    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale, [](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value) + 1;
        })
        .on(static_cast<int (ICalculator::*)(int, int)>(&ICalculator::add),
            [](auto orig, ICalculator* self, int a, int b) {
                g_add_calls.fetch_add(1);
                return orig(self, a, b) * 10;
            })
        .install(splice::slot_swap);

    ASSERT_EQ(m_outcome.state(), splice::State::installed);
    EXPECT_EQ(m_outcome.installed_sites(), 2u);

    EXPECT_EQ(calc->scale(5), 16);        // 5*3 then +1 from the callback
    EXPECT_EQ(calc->add(3, 4), 70);       // (3+4)*10
    EXPECT_EQ(g_scale_calls.load(), 1);
    EXPECT_EQ(g_add_calls.load(), 1);

    // A method that was not named is untouched.
    EXPECT_EQ(calc->factor(), 3);
}

TEST_F(VtableGroup, disable_puts_every_slot_back) {
    ICalculator* calc = splice::test::calculator();

    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale, [](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value) + 1;
        })
        .install(splice::slot_swap);
    ASSERT_EQ(m_outcome.state(), splice::State::installed);
    ASSERT_EQ(calc->scale(5), 16);

    ASSERT_TRUE(m_outcome.disable());
    EXPECT_FALSE(m_outcome.active());              // no longer in place
    EXPECT_EQ(m_outcome.state(), splice::State::installed);  // but that is still the verdict
    EXPECT_EQ(calc->scale(5), 15);                 // the original, exactly
    EXPECT_EQ(g_scale_calls.load(), 1);            // and the callback is gone
    EXPECT_FALSE(m_outcome.disable());             // idempotent, reports nothing to do
}

TEST_F(VtableGroup, the_escape_hatch_installs_against_a_slot_named_by_hand) {
    ICalculator* calc = splice::test::calculator();

    // Resolve it the ordinary way first, then install through the by-hand
    // path using that index: the two routes have to agree, which is the only
    // thing worth asserting about an escape hatch.
    unsigned char raw[sizeof(&ICalculator::scale)];
    auto pmf = &ICalculator::scale;
    std::memcpy(raw, &pmf, sizeof(pmf));
    const int index = splice_vtable_slot_of_pmf(raw, static_cast<unsigned int>(sizeof(pmf)));
    ASSERT_GE(index, 0);

    m_outcome = splice::Vtable<ICalculator>{calc}
        .on_slot(index, &ICalculator::scale,
                 [](auto orig, ICalculator* self, int value) {
                     g_scale_calls.fetch_add(1);
                     return orig(self, value) + 1;
                 })
        .install(splice::slot_swap);

    ASSERT_EQ(m_outcome.state(), splice::State::installed);
    EXPECT_EQ(calc->scale(5), 16);
    EXPECT_EQ(g_scale_calls.load(), 1);
}

TEST_F(VtableGroup, expect_refuses_when_the_slot_does_not_hold_what_was_named) {
    ICalculator* calc = splice::test::calculator();
    void* not_the_slot_contents = reinterpret_cast<void*>(&reset_counters);

    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale).expect(not_the_slot_contents)
        .onInvoke([](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value) + 1;
        })
        .install(splice::slot_swap);

    // The first site failed, so nothing was written and nothing was undone:
    // `refused` rather than `recovered`, and the method behaves exactly as it
    // did before the attempt.
    EXPECT_EQ(m_outcome.state(), splice::State::refused);
    EXPECT_EQ(m_outcome.failure().operation, splice::Operation::read_slot);
    EXPECT_EQ(calc->scale(5), 15);
    EXPECT_EQ(g_scale_calls.load(), 0);
}

TEST_F(VtableGroup, a_failure_at_the_second_site_puts_the_first_one_back) {
    ICalculator* calc = splice::test::calculator();
    void* not_the_slot_contents = reinterpret_cast<void*>(&reset_counters);

    // Site 0 installs; site 1 refuses on its expected-contents check. The
    // group has to undo site 0 before returning, and report `recovered` --
    // this is the case that makes "seven sites installed in seven suspensions,
    // a refusal at site five leaving five hooks live forever" impossible by
    // construction rather than by argument.
    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale, [](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value) + 1;
        })
        .on(&ICalculator::twice).expect(not_the_slot_contents)
        .onInvoke([](auto orig, ICalculator* self, int value) {
            g_twice_calls.fetch_add(1);
            return orig(self, value);
        })
        .install(splice::slot_swap);

    EXPECT_EQ(m_outcome.state(), splice::State::recovered);
    EXPECT_EQ(m_outcome.failure().site.ordinal, 1u);
    EXPECT_EQ(m_outcome.installed_sites(), 0u);

    // Both methods behave as if nothing had been attempted.
    EXPECT_EQ(calc->scale(5), 15);
    EXPECT_EQ(calc->twice(5), 30);
    EXPECT_EQ(g_scale_calls.load(), 0);
    EXPECT_EQ(g_twice_calls.load(), 0);
}

TEST_F(VtableGroup, a_group_with_an_unresolvable_method_installs_nothing) {
    ICalculator* calc = splice::test::calculator();

    // `twice` resolves; the second site names a slot index that cannot be, so
    // the group refuses before writing anything -- including the site that was
    // perfectly fine.
    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::twice, [](auto orig, ICalculator* self, int value) {
            g_twice_calls.fetch_add(1);
            return orig(self, value);
        })
        .on_slot(-1, &ICalculator::scale,
                 [](auto orig, ICalculator* self, int value) {
                     g_scale_calls.fetch_add(1);
                     return orig(self, value);
                 })
        .install(splice::slot_swap);

    EXPECT_EQ(m_outcome.state(), splice::State::refused);
    EXPECT_EQ(m_outcome.failure().operation, splice::Operation::resolve_slot);
    EXPECT_EQ(m_outcome.failure().site.ordinal, 1u);

    EXPECT_EQ(calc->twice(5), 30);           // 5*3 twice, unhooked
    EXPECT_EQ(g_twice_calls.load(), 0);      // the good site was never published
    EXPECT_EQ(g_scale_calls.load(), 0);
}

TEST_F(VtableGroup, without_the_group_gate_a_delegating_method_reenters_its_sibling) {
    ICalculator* calc = splice::test::calculator();

    // The control. `twice` calls `scale` through the table, so with both
    // hooked and no gate the inner hook fires twice inside the outer one.
    // Everything the next test asserts depends on this being true.
    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::twice, [](auto orig, ICalculator* self, int value) {
            g_twice_calls.fetch_add(1);
            return orig(self, value);
        })
        .on(&ICalculator::scale, [](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value);
        })
        .install(splice::slot_swap);
    ASSERT_EQ(m_outcome.state(), splice::State::installed);

    EXPECT_EQ(calc->twice(5), 30);
    EXPECT_EQ(g_twice_calls.load(), 1);
    EXPECT_EQ(g_scale_calls.load(), 2);      // the re-entry this exists to show
}

TEST_F(VtableGroup, the_group_gate_suppresses_the_sibling_inside_the_delegating_call) {
    ICalculator* calc = splice::test::calculator();

    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::twice, [](auto orig, ICalculator* self, int value) {
            g_twice_calls.fetch_add(1);
            return orig(self, value);
        })
        .on(&ICalculator::scale, [](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value);
        })
        .non_reentrant()                     // one scope for the whole group
        .install(splice::slot_swap);
    ASSERT_EQ(m_outcome.state(), splice::State::installed);

    EXPECT_EQ(calc->twice(5), 30);
    EXPECT_EQ(g_twice_calls.load(), 1);
    EXPECT_EQ(g_scale_calls.load(), 0);      // nested, so straight to the original

    // And the gate is per outermost call, not a one-shot: a direct call to
    // `scale` from outside any hook still runs its callback.
    EXPECT_EQ(calc->scale(5), 15);
    EXPECT_EQ(g_scale_calls.load(), 1);
}

TEST_F(VtableGroup, a_per_method_gate_works_inside_a_group) {
    ICalculator* calc = splice::test::calculator();
    static std::atomic<bool> enabled{false};
    enabled.store(false);

    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale)
            .when([] { return enabled.load(); })
            .onInvoke([](auto orig, ICalculator* self, int value) {
                g_scale_calls.fetch_add(1);
                return orig(self, value) + 1;
            })
        .install(splice::slot_swap);
    ASSERT_EQ(m_outcome.state(), splice::State::installed);

    EXPECT_EQ(calc->scale(5), 15);           // gated off: the original, untouched
    EXPECT_EQ(g_scale_calls.load(), 0);

    enabled.store(true);
    EXPECT_EQ(calc->scale(5), 16);
    EXPECT_EQ(g_scale_calls.load(), 1);
}

TEST_F(VtableGroup, once_spends_its_budget_inside_a_group) {
    ICalculator* calc = splice::test::calculator();

    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale)
            .once()
            .onInvoke([](auto orig, ICalculator* self, int value) {
                g_scale_calls.fetch_add(1);
                return orig(self, value) + 1;
            })
        .install(splice::slot_swap);
    ASSERT_EQ(m_outcome.state(), splice::State::installed);

    EXPECT_EQ(calc->scale(5), 16);
    EXPECT_EQ(calc->scale(5), 15);           // spent, so transparent
    EXPECT_EQ(g_scale_calls.load(), 1);
}

TEST_F(VtableGroup, before_and_after_read_the_call_without_replacing_it) {
    ICalculator* calc = splice::test::calculator();
    static std::atomic<int> seen_argument{0};
    static std::atomic<int> seen_result{0};
    seen_argument.store(0);
    seen_result.store(0);

    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale)
            .before([](ICalculator*, int value) { seen_argument.store(value); })
        .on(static_cast<int (ICalculator::*)(int, int)>(&ICalculator::add))
            .after([](int result, ICalculator*, int, int) { seen_result.store(result); })
        .install(splice::slot_swap);
    ASSERT_EQ(m_outcome.state(), splice::State::installed);

    EXPECT_EQ(calc->scale(5), 15);           // unchanged behaviour
    EXPECT_EQ(calc->add(3, 4), 7);
    EXPECT_EQ(seen_argument.load(), 5);
    EXPECT_EQ(seen_result.load(), 7);
}

// The coordinated tier is tested in test_live_tier.cpp, where the assertion
// that matters is that the vtable SLOT is left alone -- the one observation
// that separates it from this tier doing the work.

TEST_F(VtableGroup, a_group_with_no_methods_refuses) {
    splice::Outcome outcome =
        splice::Vtable<ICalculator>{splice::test::calculator()}.install(splice::slot_swap);
    EXPECT_EQ(outcome.state(), splice::State::refused);
}

TEST_F(VtableGroup, a_null_object_refuses_instead_of_reading_through_it) {
    splice::Outcome outcome = splice::Vtable<ICalculator>{nullptr}
        .on(&ICalculator::scale, [](auto orig, ICalculator* self, int value) {
            return orig(self, value);
        })
        .install(splice::slot_swap);
    EXPECT_EQ(outcome.state(), splice::State::refused);
    EXPECT_EQ(outcome.failure().stage, splice::Stage::prepare);
}

TEST_F(VtableGroup, the_stub_pool_is_finite_and_a_released_stub_can_be_claimed_again) {
    ICalculator* calc = splice::test::calculator();
    constexpr int kPool = SPLICE_SLOT_STUBS_PER_SIGNATURE;

    // One signature, one stub each: int(ICalculator*, int). Claim every stub
    // the pool has, each in its own group on the same slot -- which is legal,
    // the last one written wins, and all of them are undone below.
    std::vector<splice::Outcome> groups;
    for (int i = 0; i < kPool; ++i) {
        groups.push_back(splice::Vtable<ICalculator>{calc}
            .on(&ICalculator::scale, [](auto orig, ICalculator* self, int value) {
                return orig(self, value);
            })
            .install(splice::slot_swap));
        ASSERT_EQ(groups.back().state(), splice::State::installed) << "at claim " << i;
    }

    // The next claim has nothing left to hand out, so the group refuses.
    splice::Outcome exhausted = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale, [](auto orig, ICalculator* self, int value) {
            return orig(self, value);
        })
        .install(splice::slot_swap);
    EXPECT_EQ(exhausted.state(), splice::State::refused);

    // Give them back in reverse, so each disable restores what the one before
    // it had seen, and the slot ends up holding the provider's own function.
    for (auto it = groups.rbegin(); it != groups.rend(); ++it) {
        EXPECT_TRUE(it->disable());
    }
    EXPECT_EQ(calc->scale(5), 15);

    // And now a claim succeeds again.
    splice::Outcome after = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale, [](auto orig, ICalculator* self, int value) {
            return orig(self, value);
        })
        .install(splice::slot_swap);
    EXPECT_EQ(after.state(), splice::State::installed);
    EXPECT_TRUE(after.disable());
}
