// ─── .non_reentrant() — the re-entrancy gate ──────────────────────────────
//
// The case this exists for: one hooked method's implementation calls a sibling
// method that is also hooked. Without a gate the callback work runs twice, or
// the hooks recurse. Every test below that claims the gate suppresses a nested
// call is paired with a control showing the nested call really happens without
// it — a nested call that never occurs would make the gate look correct while
// testing nothing.
//
// Driven through the trampolines directly, as test_hook_modifiers.cpp does, so
// the gate logic runs on every platform. The originals call trampolines, which
// is how a patched function's implementation reaches a sibling hook.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/splice.h>

#include "helpers/test_targets.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace {

template <typename Gen>
int slot_of() {
    return splice::default_context().slot_for(Gen::get_trampoline_ptr());
}

int double_it(int x) { return x * 2; }

// ── A family of two: outer's implementation calls inner through its hook ──
using InnerGen = splice::TrampolineGenerator<int (*)(int), 70201>;
using OuterGen = splice::TrampolineGenerator<int (*)(int), 70202>;

int outer_impl(int x) { return InnerGen::trampoline(x) + 1; }

// ── The same family again, for the control, on separate slots ─────────────
using InnerCtlGen = splice::TrampolineGenerator<int (*)(int), 70203>;
using OuterCtlGen = splice::TrampolineGenerator<int (*)(int), 70204>;

int outer_ctl_impl(int x) { return InnerCtlGen::trampoline(x) + 1; }

// ── A function that recurses through its own hook ──────────────────────────
using RecurseGen    = splice::TrampolineGenerator<int (*)(int), 70205>;
using RecurseCtlGen = splice::TrampolineGenerator<int (*)(int), 70206>;
using BudgetGen     = splice::TrampolineGenerator<int (*)(int), 70207>;

int recurse_impl(int n) { return n <= 0 ? 0 : 1 + RecurseGen::trampoline(n - 1); }
int recurse_ctl_impl(int n) { return n <= 0 ? 0 : 1 + RecurseCtlGen::trampoline(n - 1); }
int budget_impl(int n) { return n <= 0 ? 0 : 1 + BudgetGen::trampoline(n - 1); }

using ThreadGen = splice::TrampolineGenerator<int (*)(int), 70208>;

template <typename Gen>
splice::InterceptorEntry<int (*)(int)>* make_entry(int id, int (*impl)(int)) {
    auto* entry = new splice::InterceptorEntry<int (*)(int)>(
        nullptr, reinterpret_cast<void*>(impl), "reentrancy_test", id,
        Gen::get_trampoline_ptr());
    splice::OriginalRegistry::set_original<int (*)(int)>(slot_of<Gen>(), impl);
    return entry;
}

} // namespace

// ─── The sibling case: the reason the gate exists ─────────────────────────

// Control first. Without a gate, a call to outer runs inner's callback as well.
TEST(Reentrancy, without_the_gate_a_sibling_hook_runs_inside_the_outer_one) {
    std::unique_ptr<splice::InterceptorEntry<int (*)(int)>> inner(
        make_entry<InnerCtlGen>(70203, &double_it));
    std::unique_ptr<splice::InterceptorEntry<int (*)(int)>> outer(
        make_entry<OuterCtlGen>(70204, &outer_ctl_impl));

    std::atomic<int> inner_hits{0};
    std::atomic<int> outer_hits{0};
    inner->before([&](int) { inner_hits.fetch_add(1); });
    outer->before([&](int) { outer_hits.fetch_add(1); });

    EXPECT_EQ(OuterCtlGen::trampoline(5), 11);   // inner(5) = 10, + 1
    EXPECT_EQ(outer_hits.load(), 1);
    EXPECT_EQ(inner_hits.load(), 1) << "the control must re-enter, or the gate test proves nothing";
}

TEST(Reentrancy, a_shared_scope_suppresses_the_sibling_inside_the_outer_call) {
    static splice::Reentrancy family;
    ASSERT_TRUE(family.valid());

    std::unique_ptr<splice::InterceptorEntry<int (*)(int)>> inner(
        make_entry<InnerGen>(70201, &double_it));
    std::unique_ptr<splice::InterceptorEntry<int (*)(int)>> outer(
        make_entry<OuterGen>(70202, &outer_impl));

    std::atomic<int> inner_hits{0};
    std::atomic<int> outer_hits{0};
    inner->non_reentrant(family).before([&](int) { inner_hits.fetch_add(1); });
    outer->non_reentrant(family).before([&](int) { outer_hits.fetch_add(1); });

    EXPECT_EQ(OuterGen::trampoline(5), 11) << "the original still runs, nested call included";
    EXPECT_EQ(outer_hits.load(), 1);
    EXPECT_EQ(inner_hits.load(), 0) << "reached from inside outer's callback: straight to the original";

    // Called on its own, inner is a new call and its callback runs.
    EXPECT_EQ(InnerGen::trampoline(5), 10);
    EXPECT_EQ(inner_hits.load(), 1);

    // And the scope is released on the way out, so the next outer call is gated
    // exactly like the first.
    EXPECT_EQ(OuterGen::trampoline(1), 3);
    EXPECT_EQ(outer_hits.load(), 2);
    EXPECT_EQ(inner_hits.load(), 1);
}

// ─── Direct recursion: the no-argument form ───────────────────────────────

TEST(Reentrancy, without_the_gate_recursion_runs_the_callback_at_every_level) {
    std::unique_ptr<splice::InterceptorEntry<int (*)(int)>> entry(
        make_entry<RecurseCtlGen>(70206, &recurse_ctl_impl));

    std::atomic<int> hits{0};
    entry->before([&](int) { hits.fetch_add(1); });

    EXPECT_EQ(RecurseCtlGen::trampoline(4), 4);
    EXPECT_EQ(hits.load(), 5) << "levels 4, 3, 2, 1, 0 all enter through the hook";
}

TEST(Reentrancy, a_private_scope_runs_the_callback_once_per_outermost_call) {
    std::unique_ptr<splice::InterceptorEntry<int (*)(int)>> entry(
        make_entry<RecurseGen>(70205, &recurse_impl));

    std::atomic<int> hits{0};
    entry->non_reentrant().before([&](int) { hits.fetch_add(1); });

    EXPECT_EQ(RecurseGen::trampoline(4), 4);
    EXPECT_EQ(hits.load(), 1);

    EXPECT_EQ(RecurseGen::trampoline(2), 2);
    EXPECT_EQ(hits.load(), 2);
}

// A nested call is not a new call to decide about, so it must not spend a
// budget. With the budget checked before the gate, the nested calls of the
// first outer call would exhaust .times(2) and the second outer call would
// not fire.
TEST(Reentrancy, a_nested_call_does_not_spend_a_times_budget) {
    std::unique_ptr<splice::InterceptorEntry<int (*)(int)>> entry(
        make_entry<BudgetGen>(70207, &budget_impl));

    std::atomic<int> hits{0};
    entry->times(2).non_reentrant().before([&](int) { hits.fetch_add(1); });

    EXPECT_EQ(BudgetGen::trampoline(3), 3);
    EXPECT_EQ(BudgetGen::trampoline(3), 3);
    EXPECT_EQ(hits.load(), 2) << "two outermost calls, budget of two";

    EXPECT_EQ(BudgetGen::trampoline(3), 3);
    EXPECT_EQ(hits.load(), 2) << "budget spent";
}

// ─── The scope is per thread ──────────────────────────────────────────────

// Thread A is held inside the callback while thread B calls the same hook. B
// is not nested inside anything, so its callback must run.
TEST(Reentrancy, one_thread_inside_the_scope_does_not_gate_another) {
    std::unique_ptr<splice::InterceptorEntry<int (*)(int)>> entry(
        make_entry<ThreadGen>(70208, &double_it));

    std::atomic<bool> a_inside{false};
    std::atomic<bool> release_a{false};
    std::atomic<int> hits{0};

    entry->non_reentrant().before([&](int x) {
        hits.fetch_add(1);
        if (x == 1) {
            a_inside.store(true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!release_a.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
        }
    });

    std::thread a([&] { (void)ThreadGen::trampoline(1); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!a_inside.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(a_inside.load()) << "thread A never reached the callback";

    int b_result = 0;
    std::thread b([&] { b_result = ThreadGen::trampoline(2); });
    b.join();

    release_a.store(true);
    a.join();

    EXPECT_EQ(b_result, 4);
    EXPECT_EQ(hits.load(), 2) << "B's callback ran while A was inside the scope";
}

// ─── The limit, and what happens at it ────────────────────────────────────

TEST(Reentrancy, scopes_are_finite_and_a_released_one_can_be_reserved_again) {
    std::vector<std::unique_ptr<splice::Reentrancy>> held;
    for (int i = 0; i < 65; ++i) {
        held.push_back(std::make_unique<splice::Reentrancy>());
        if (!held.back()->valid()) break;
    }
    ASSERT_FALSE(held.back()->valid()) << "64 bits cannot yield 65 scopes";
    EXPECT_LE(held.size(), 65u);

    // Distinct bits: two live scopes must never share one.
    std::uint64_t seen = 0;
    for (const auto& s : held) {
        if (!s->valid()) continue;
        EXPECT_EQ(seen & s->bit(), 0u);
        seen |= s->bit();
    }

    held.erase(held.begin());   // release one valid scope
    held.pop_back();            // and the invalid one
    splice::Reentrancy again;
    EXPECT_TRUE(again.valid()) << "a released bit must be reusable";
}

#if defined(__x86_64__) || defined(_M_X64) || \
    ((defined(__aarch64__) || defined(__arm64__)) && (defined(__linux__) || defined(__ANDROID__)))

// Asked for a scope that does not exist, a hook refuses to install rather than
// installing without the protection it was asked for. The control installs the
// identical chain with a valid scope, so the refusal is shown to come from the
// scope and not from the target.
TEST(Reentrancy, a_hook_told_to_use_an_invalid_scope_refuses_to_install) {
    using splice::test::reentry_control_target;
    using splice::test::reentry_refusal_target;

    ASSERT_EQ(reentry_refusal_target(20), 21);
    ASSERT_EQ(reentry_control_target(20), 23);

    std::vector<std::unique_ptr<splice::Reentrancy>> held;
    for (int i = 0; i < 65; ++i) {
        held.push_back(std::make_unique<splice::Reentrancy>());
        if (!held.back()->valid()) break;
    }
    ASSERT_FALSE(held.back()->valid());

    using RefuseGen  = splice::TrampolineGenerator<int (*)(int), 70210>;
    using ControlGen = splice::TrampolineGenerator<int (*)(int), 70211>;
    splice::InterceptorEntry<int (*)(int)> refused(
        nullptr, reinterpret_cast<void*>(&reentry_refusal_target), "reentry_refusal",
        70210, RefuseGen::get_trampoline_ptr());
    splice::InterceptorEntry<int (*)(int)> control(
        nullptr, reinterpret_cast<void*>(&reentry_control_target), "reentry_control",
        70211, ControlGen::get_trampoline_ptr());

    static std::atomic<int> refused_hits{0};
    static std::atomic<int> control_hits{0};
    refused.non_reentrant(*held.back()).onInvoke([](auto orig, int x) {
        refused_hits.fetch_add(1);
        return orig(x) + 1000;
    });

    held.erase(held.begin());   // free exactly one bit for the control
    splice::Reentrancy control_scope;
    ASSERT_TRUE(control_scope.valid());
    control.non_reentrant(control_scope).onInvoke([](auto orig, int x) {
        control_hits.fetch_add(1);
        return orig(x) + 1000;
    });

    splice::install_all();

    EXPECT_FALSE(refused.is_installed());
    EXPECT_EQ(reentry_refusal_target(20), 21) << "the target must be untouched";
    EXPECT_EQ(refused_hits.load(), 0);

    ASSERT_TRUE(control.is_installed()) << "the control chain must install, or the refusal proves nothing";
    EXPECT_EQ(reentry_control_target(20), 1023);
    EXPECT_EQ(control_hits.load(), 1);
    EXPECT_TRUE(control.disable());
    EXPECT_EQ(reentry_control_target(20), 23);
}

#endif
