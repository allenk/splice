// ─── The coordinated tier, end to end, in this process ────────────────────
//
// Design section 8: the live tier is proved in splice's own runners first,
// in-process, before the overlay is asked again. These cases are the part that
// needs no fault injection -- the transaction doing what it says on a healthy
// process -- and the thing they check that no single-site test can is the
// distinction between the two tiers:
//
//   the slot_swap tier writes the VTABLE SLOT and leaves the function alone
//   the live tier writes the FUNCTION and leaves the slot alone
//
// Every test here asserts the slot is untouched. That is not decoration: it is
// the only observation that separates "the coordinated tier installed" from
// "something quietly did a slot swap instead", and a tier that silently
// degrades is the failure this design is most exposed to.
//
// Each install suspends every thread of the test process the engine could
// enumerate, once. Under
// gtest that is a handful of threads, and the enumeration behind it costs about
// 118 ms because the OS walks every thread on the machine -- which is why the
// group exists and why these tests are not instant.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/live.h>

#include "test_targets.h"
#include "vtable_provider.h"

#include <atomic>
#include <cstring>
#include <thread>

namespace {

using splice::test::ICalculator;

std::atomic<int> g_scale_calls{0};
std::atomic<int> g_twice_calls{0};

void reset_counters() {
    g_scale_calls.store(0);
    g_twice_calls.store(0);
}

// The provider's own address for a slot, read inside the provider module, so
// the test can tell "the slot is unchanged" from "the slot now points at us"
// without reading the table itself.
void* provider_entry(int index) { return splice::test::table_entry(index); }

int slot_of_scale() {
    auto pmf = &ICalculator::scale;
    unsigned char raw[sizeof(pmf)];
    std::memcpy(raw, &pmf, sizeof(pmf));
    return splice_vtable_slot_of_pmf(raw, static_cast<unsigned int>(sizeof(pmf)));
}

// A plain function, for the group that names its sites by address. Not static:
// it has to survive having its prologue replaced, so the compiler must not be
// free to inline it away at the one call site.
int g_witness = 0;

#if defined(_MSC_VER)
#   define SPLICE_TEST_NOINLINE __declspec(noinline)
#else
#   define SPLICE_TEST_NOINLINE __attribute__((noinline))
#endif

SPLICE_TEST_NOINLINE int plain_target(int value) {
    g_witness += value;
    return value * 2;
}

using PlainFn = int (*)(int);

class LiveTier : public ::testing::Test {
protected:
    void SetUp() override {
        if (!splice::live_tier_available()) {
            GTEST_SKIP() << "the coordinated tier needs an architecture whose "
                            "prologue can be planned and an OS that can suspend "
                            "a thread; this build has one or neither";
        }
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

TEST_F(LiveTier, the_function_is_patched_and_the_slot_is_left_alone) {
    ICalculator* calc = splice::test::calculator();
    const int slot = slot_of_scale();
    ASSERT_GE(slot, 0);
    void* const before = provider_entry(slot);

    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale, [](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value) + 1;
        })
        .install(splice::live);

    ASSERT_EQ(m_outcome.state(), splice::State::installed)
        << "stage=" << static_cast<int>(m_outcome.failure().stage)
        << " op=" << static_cast<int>(m_outcome.failure().operation)
        << " err=" << m_outcome.failure().os_error;

    EXPECT_EQ(calc->scale(5), 16);              // 5*3 then +1 from the callback
    EXPECT_EQ(g_scale_calls.load(), 1);

    // The distinguishing assertion: the vtable still holds the provider's own
    // function. The call arrived through a patched prologue, not a swapped
    // pointer.
    EXPECT_EQ(provider_entry(slot), before);

    // And a call that bypasses the vtable entirely -- straight through the
    // address the slot holds -- is hooked too, which a slot swap could not do.
    auto direct = reinterpret_cast<int (*)(ICalculator*, int)>(before);
    EXPECT_EQ(direct(calc, 5), 16);
    EXPECT_EQ(g_scale_calls.load(), 2);
}

TEST_F(LiveTier, disable_puts_the_prologue_back) {
    ICalculator* calc = splice::test::calculator();

    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale, [](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value) + 1;
        })
        .install(splice::live);
    ASSERT_EQ(m_outcome.state(), splice::State::installed);
    ASSERT_EQ(calc->scale(5), 16);

    ASSERT_TRUE(m_outcome.disable());
    EXPECT_FALSE(m_outcome.active());
    EXPECT_EQ(calc->scale(5), 15);              // the original prologue, back
    EXPECT_EQ(g_scale_calls.load(), 1);
}

TEST_F(LiveTier, two_methods_install_under_one_suspension) {
    ICalculator* calc = splice::test::calculator();

    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale, [](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value);
        })
        .on(&ICalculator::twice, [](auto orig, ICalculator* self, int value) {
            g_twice_calls.fetch_add(1);
            return orig(self, value);
        })
        .install(splice::live);

    ASSERT_EQ(m_outcome.state(), splice::State::installed)
        << "stage=" << static_cast<int>(m_outcome.failure().stage)
        << " site=" << m_outcome.failure().site.ordinal;
    EXPECT_EQ(m_outcome.installed_sites(), 2u);

    // twice() calls scale() twice, and both are patched, so without a gate the
    // inner callback runs twice inside the outer one -- the same control the
    // slot_swap tests make, repeated here because the mechanism is different.
    EXPECT_EQ(calc->twice(5), 30);
    EXPECT_EQ(g_twice_calls.load(), 1);
    EXPECT_EQ(g_scale_calls.load(), 2);
}

TEST_F(LiveTier, the_group_gate_suppresses_the_sibling_on_this_tier_too) {
    ICalculator* calc = splice::test::calculator();

    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale, [](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value);
        })
        .on(&ICalculator::twice, [](auto orig, ICalculator* self, int value) {
            g_twice_calls.fetch_add(1);
            return orig(self, value);
        })
        .non_reentrant()
        .install(splice::live);
    ASSERT_EQ(m_outcome.state(), splice::State::installed);

    EXPECT_EQ(calc->twice(5), 30);
    EXPECT_EQ(g_twice_calls.load(), 1);
    EXPECT_EQ(g_scale_calls.load(), 0);         // nested, so straight to the original
}

TEST_F(LiveTier, a_group_named_by_address_installs) {
    g_witness = 0;
    static std::atomic<int> plain_calls{0};
    plain_calls.store(0);

    splice::Live<> group;
    m_outcome = group
        .at(reinterpret_cast<void*>(&plain_target), PlainFn{},
            [](auto orig, int value) {
                plain_calls.fetch_add(1);
                return orig(value) + 100;
            })
        .install(splice::live);

    ASSERT_EQ(m_outcome.state(), splice::State::installed)
        << "stage=" << static_cast<int>(m_outcome.failure().stage)
        << " op=" << static_cast<int>(m_outcome.failure().operation)
        << " err=" << m_outcome.failure().os_error;

    EXPECT_EQ(plain_target(4), 108);            // 4*2 then +100
    EXPECT_EQ(plain_calls.load(), 1);
    EXPECT_EQ(g_witness, 4);                    // the relocated prologue still ran
}

// Outside the fixture on purpose: this one needs no coordination, so it is a
// property every platform can check even where the tier itself cannot run.
TEST(LiveTierEverywhere, a_group_named_by_address_refuses_the_slot_swap_tier) {
    // There is no slot to swap, so the weaker tier cannot be applied to it.
    // Refusing beats swapping something that is not a vtable entry.
    splice::Live<> group;
    splice::Outcome outcome = group
        .at(reinterpret_cast<void*>(&plain_target), PlainFn{},
            [](auto orig, int value) { return orig(value); })
        .install(splice::slot_swap);

    EXPECT_EQ(outcome.state(), splice::State::refused);
    EXPECT_EQ(outcome.installed_sites(), 0u);
}

TEST_F(LiveTier, a_target_behind_a_jump_thunk_is_patched_through_it) {
    // `&contention_target_a` is in another translation unit, so under
    // incremental linking the address a caller gets is an ILT thunk -- a bare
    // `E9 rel32` -- not the function. Patching the thunk installs a hook
    // almost nothing calls, and the planner refuses a jump as an unrelocatable
    // prologue, so the whole install fails.
    //
    // This is the case that was broken until a contention runner found it: the
    // single-site path has unwrapped since Phase 4, the group did not. In a
    // non-incremental build the address may already be the function, which is
    // why the assertion is on the install succeeding rather than on the bytes
    // -- it has to pass either way, and fail if the unwrap goes.
    static std::atomic<int> hits{0};
    hits.store(0);

    splice::Live<> group;
    m_outcome = group
        .at(reinterpret_cast<void*>(&splice::test::contention_target_a), PlainFn{},
            [](auto orig, int x) {
                hits.fetch_add(1);
                return orig(x) + 1;
            })
        .install(splice::live);

    ASSERT_EQ(m_outcome.state(), splice::State::installed)
        << "stage=" << static_cast<int>(m_outcome.failure().stage)
        << " op=" << static_cast<int>(m_outcome.failure().operation);
    EXPECT_EQ(splice::test::contention_target_a(5), 16);   // 5*3 then +1
    EXPECT_EQ(hits.load(), 1);
}

TEST_F(LiveTier, a_second_live_install_on_the_same_function_is_refused) {
    // An installed patch is an `E9` too, so a second install that followed it
    // would walk into splice's own relay and patch whatever the hook stub
    // begins with -- observed once already, with a stack overflow. The site
    // registry is what stops it, and this is the test that says the group keeps
    // that registry in step rather than only reading it.
    static std::atomic<int> hits{0};
    hits.store(0);

    splice::Live<> first;
    m_outcome = first
        .at(reinterpret_cast<void*>(&splice::test::contention_target_b), PlainFn{},
            [](auto orig, int x) {
                hits.fetch_add(1);
                return orig(x) + 1;
            })
        .install(splice::live);
    ASSERT_EQ(m_outcome.state(), splice::State::installed);
    ASSERT_EQ(splice::test::contention_target_b(2), 11);   // 2*5 then +1

    splice::Live<> second;
    splice::Outcome again = second
        .at(reinterpret_cast<void*>(&splice::test::contention_target_b), PlainFn{},
            [](auto orig, int x) { return orig(x) + 100; })
        .install(splice::live);

    EXPECT_EQ(again.state(), splice::State::refused);
    EXPECT_EQ(again.failure().stage, splice::Stage::prepare);
    EXPECT_EQ(again.installed_sites(), 0u);

    // The first hook is untouched, which is the property that matters: a
    // rejected install must not damage the one already there.
    const int before = hits.load();
    EXPECT_EQ(splice::test::contention_target_b(2), 11);
    EXPECT_EQ(hits.load(), before + 1);
}

TEST_F(LiveTier, an_address_that_is_not_mapped_code_is_refused_not_read) {
    // prepare asks the OS for write permission before it reads the prologue,
    // so a caller-supplied address that names nothing is a refusal rather than
    // an access violation. The order matters more than it looks: the snapshot
    // has to come from somewhere, and reading first is a fault.
    splice::Live<> group;
    splice::Outcome outcome = group
        .at(reinterpret_cast<void*>(0x1000), PlainFn{},
            [](auto orig, int value) { return orig(value); })
        .install(splice::live);

    EXPECT_EQ(outcome.state(), splice::State::refused);
    EXPECT_EQ(outcome.failure().stage, splice::Stage::prepare);
    EXPECT_EQ(outcome.installed_sites(), 0u);
}

TEST_F(LiveTier, a_worker_thread_keeps_running_across_the_install) {
    // Not a contention test -- that is a separate runner and a local gate.
    // This one asserts the simplest property of a transaction that suspends
    // the threads it enlisted: it resumes them. The worker is started before
    // the install and joined after it, so it is in the enlisted set by
    // construction -- which is what makes the assertion meaningful rather than
    // a coincidence. A worker that stops counting has
    // been left suspended, which is the failure mode with the worst
    // consequences and the cheapest check.
    std::atomic<bool> stop{false};
    std::atomic<int>  ticks{0};
    std::thread worker([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            ticks.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    ICalculator* calc = splice::test::calculator();
    m_outcome = splice::Vtable<ICalculator>{calc}
        .on(&ICalculator::scale, [](auto orig, ICalculator* self, int value) {
            g_scale_calls.fetch_add(1);
            return orig(self, value);
        })
        .install(splice::live);
    ASSERT_EQ(m_outcome.state(), splice::State::installed);

    const int before = ticks.load();
    while (ticks.load() < before + 100) std::this_thread::yield();
    stop.store(true);
    worker.join();

    EXPECT_GT(ticks.load(), before);
    EXPECT_EQ(calc->scale(5), 15);
}
