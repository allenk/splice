// ─── install_all(splice::live) — the batch verb with a tier ────────────────
//
// `install_all()` has always meant "install every hook queued above", one at a
// time. Given a tier it means the same thing in one transaction: one thread
// enumeration and one suspension for the whole set, and all-or-nothing across
// it. The queue is type erased, so the interesting part is not the transaction
// -- that is the same code the interface groups use, deliberately -- but what
// each queued entry has to hand over, and what happens when one of them
// cannot.
//
// **This runner needs its own binary**, and the reason is the thing it tests.
// `install_all` reaches the whole process's installer queue, so in a shared
// test binary it would install every hook every other test file queued, which
// is both chaos and a dishonest result: the group would be refused by some
// unrelated target's prologue and the refusal would look like this feature's.
// A dedicated executable makes "everything queued" mean "what this test
// queued".
//
// Even here the queue is process-global and gtest runs these cases in one
// process, so each one wipes the queue first and builds its entries as locals.
// That is also why the entries are constructed directly rather than through
// SPLICE_HOOK_ADDR: the macro's entry is a function-local static, registered
// once per process, and a wipe would leave it queued nowhere for every case
// after the first.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/core.h>
#include <splice/live.h>
#include <splice/registry.h>

#include "test_targets.h"

#include <atomic>
#include <memory>
#include <vector>

namespace {

using splice::test::batch_target_a;
using splice::test::batch_target_b;

using TargetFn = int (*)(int);

constexpr int kArg    = 6;
constexpr int kPlainA = kArg * 7;      // 42
constexpr int kPlainB = kArg * 11;     // 66

std::atomic<int> g_a_calls{0};
std::atomic<int> g_b_calls{0};

// One queued hook, with its own trampoline. `Id` is a template parameter
// because that is what selects the trampoline instantiation, and two entries
// sharing one trampoline would share one registry slot.
template <int Id>
class QueuedHook {
public:
    explicit QueuedHook(void* target, const char* label)
        : m_entry(nullptr, target, label, Id,
                  splice::TrampolineGenerator<TargetFn, Id>::get_trampoline_ptr()) {}

    // By symbol, for the resolution path.
    QueuedHook(const char* lib, const char* symbol)
        : m_entry(lib, symbol, symbol, Id,
                  splice::TrampolineGenerator<TargetFn, Id>::get_trampoline_ptr()) {}

    splice::InterceptorEntry<TargetFn>& operator*() { return m_entry; }
    splice::InterceptorEntry<TargetFn>* operator->() { return &m_entry; }

private:
    splice::InterceptorEntry<TargetFn> m_entry;
};

class InstallAllTier : public ::testing::Test {
protected:
    void SetUp() override {
        // Wipes the four registries and the installer queue, and invalidates
        // every outstanding token. Each case therefore starts from an empty
        // queue and fills it itself.
        splice::default_context().reset();
        g_a_calls.store(0);
        g_b_calls.store(0);
    }

    void TearDown() override {
        if (outcome.active()) {
            EXPECT_TRUE(outcome.disable());
        }
        splice::default_context().reset();
        EXPECT_EQ(batch_target_a(kArg), kPlainA) << "target A was left hooked";
        EXPECT_EQ(batch_target_b(kArg), kPlainB) << "target B was left hooked";
    }

    splice::Outcome outcome;
};

} // namespace

// A macro and not a function, because GTEST_SKIP expands to a `return`: inside
// a helper it returns from the helper and the case carries on into assertions
// about a tier that is not there. That is how the first version of this file
// failed on Linux -- loudly, which was luck. On a platform where the tier
// exists the same mistake is invisible.
#define SPLICE_REQUIRE_LIVE_TIER()                                                do {                                                                              if (!splice::live_tier_available()) {                                              GTEST_SKIP() << "the coordinated tier needs an OS that can "                                   "suspend a thread and move its program counter";           }                                                                         } while (false)

// ─── the tier that has nothing to swap ────────────────────────────────────

TEST_F(InstallAllTier, the_slot_swap_tier_is_refused_and_writes_nothing) {
    // A queued hook names a function, by symbol or by address. There is no
    // vtable slot behind it, so this tier has nothing to do -- and the right
    // answer is to say so rather than to quietly install the weaker thing,
    // which is the same call the interface group already makes.
    QueuedHook<0x90101> a(reinterpret_cast<void*>(&batch_target_a), "batch_a");
    (*a).onInvoke([](TargetFn orig, int x) {
        g_a_calls.fetch_add(1, std::memory_order_relaxed);
        return orig(x) + 1;
    });

    const splice::Outcome swapped = splice::install_all(splice::slot_swap);
    EXPECT_EQ(swapped.state(), splice::State::refused);
    EXPECT_EQ(swapped.installed_sites(), 0u);
    EXPECT_FALSE(swapped.active());

    EXPECT_EQ(batch_target_a(kArg), kPlainA) << "a refused tier wrote anyway";
    EXPECT_EQ(g_a_calls.load(), 0);
    EXPECT_FALSE((*a).is_installed());
}

TEST_F(InstallAllTier, an_empty_queue_is_refused_rather_than_called_installed) {
    SPLICE_REQUIRE_LIVE_TIER();
    // Nothing queued. `installed` would be a claim about work that did not
    // happen, and `ok()` is what a caller branches on.
    const splice::Outcome empty = splice::install_all(splice::live);
    EXPECT_EQ(empty.state(), splice::State::refused);
    EXPECT_FALSE(empty.ok());
    EXPECT_EQ(empty.installed_sites(), 0u);
}

// ─── the thing it is for ──────────────────────────────────────────────────

TEST_F(InstallAllTier, two_queued_hooks_install_as_one_transaction) {
    SPLICE_REQUIRE_LIVE_TIER();
    QueuedHook<0x90201> a(reinterpret_cast<void*>(&batch_target_a), "batch_a");
    QueuedHook<0x90202> b(reinterpret_cast<void*>(&batch_target_b), "batch_b");
    (*a).onInvoke([](TargetFn orig, int x) {
        g_a_calls.fetch_add(1, std::memory_order_relaxed);
        return orig(x) + 1;
    });
    (*b).onInvoke([](TargetFn orig, int x) {
        g_b_calls.fetch_add(1, std::memory_order_relaxed);
        return orig(x) + 2;
    });

    outcome = splice::install_all(splice::live);
    ASSERT_EQ(outcome.state(), splice::State::installed)
        << "stage=" << static_cast<int>(outcome.failure().stage)
        << " op=" << static_cast<int>(outcome.failure().operation)
        << " site=" << outcome.failure().site.ordinal;
    EXPECT_EQ(outcome.installed_sites(), 2u) << "one transaction, two sites";

    EXPECT_EQ(batch_target_a(kArg), kPlainA + 1);
    EXPECT_EQ(batch_target_b(kArg), kPlainB + 2);
    EXPECT_EQ(g_a_calls.load(), 1);
    EXPECT_EQ(g_b_calls.load(), 1);

    // The entries know they are installed, which is what makes their own
    // disable(), is_installed() and original() keep working after a group
    // install -- they were not bypassed, they were driven.
    EXPECT_TRUE((*a).is_installed());
    EXPECT_TRUE((*b).is_installed());

    ASSERT_TRUE(outcome.disable());
    EXPECT_FALSE(outcome.active());
    EXPECT_EQ(batch_target_a(kArg), kPlainA);
    EXPECT_EQ(batch_target_b(kArg), kPlainB);
    EXPECT_EQ(g_a_calls.load(), 1) << "the callback still ran after disable";
    EXPECT_EQ(g_b_calls.load(), 1);
}

TEST_F(InstallAllTier, the_group_disable_tells_the_entries_they_are_not_installed) {
    SPLICE_REQUIRE_LIVE_TIER();
    // The hazard this closes: the entry owns a record too, so after the group
    // puts a site back, an entry that still believed it was installed would
    // restore bytes that are already original -- and report success for it.
    QueuedHook<0x90301> a(reinterpret_cast<void*>(&batch_target_a), "batch_a");
    (*a).onInvoke([](TargetFn orig, int x) {
        g_a_calls.fetch_add(1, std::memory_order_relaxed);
        return orig(x) + 1;
    });

    splice::Outcome installed = splice::install_all(splice::live);
    ASSERT_EQ(installed.state(), splice::State::installed);
    ASSERT_TRUE((*a).is_installed());

    ASSERT_TRUE(installed.disable());
    EXPECT_FALSE((*a).is_installed())
        << "the entry was not told, so it would disable an already-restored site";
    EXPECT_FALSE((*a).disable()) << "a second disable reported success";
    EXPECT_EQ(batch_target_a(kArg), kPlainA);
}

TEST_F(InstallAllTier, a_second_call_finds_nothing_left_to_install) {
    SPLICE_REQUIRE_LIVE_TIER();
    // install_all() has always been safe to call again, so an already-installed
    // hook must not join -- a group that re-patched a function it already owns
    // would be refused for being a second install on the same address, and
    // would take every other site down with it.
    QueuedHook<0x90401> a(reinterpret_cast<void*>(&batch_target_a), "batch_a");
    (*a).onInvoke([](TargetFn orig, int x) {
        g_a_calls.fetch_add(1, std::memory_order_relaxed);
        return orig(x) + 1;
    });

    outcome = splice::install_all(splice::live);
    ASSERT_EQ(outcome.state(), splice::State::installed);

    const splice::Outcome again = splice::install_all(splice::live);
    EXPECT_EQ(again.state(), splice::State::refused)
        << "the second call claimed to install something";
    EXPECT_EQ(again.installed_sites(), 0u);

    // `refused` alone does not say the guard works, and that distinction is
    // the whole case: without it the second call builds a one-site group on a
    // function splice has already patched, and the ENGINE refuses it -- same
    // state, different reason, and on a bigger set it would take every
    // untouched site down with it. The failure record is what tells them
    // apart, so it is what gets asserted.
    EXPECT_EQ(again.failure().operation, splice::Operation::none)
        << "the second call reached the engine instead of finding nothing to do";

    // And the first install is untouched by the second call's refusal.
    EXPECT_TRUE(outcome.active());
    EXPECT_EQ(batch_target_a(kArg), kPlainA + 1);
    EXPECT_TRUE((*a).is_installed());
}

// ─── all-or-nothing, which is the whole claim ─────────────────────────────

TEST_F(InstallAllTier, an_installer_with_no_group_path_refuses_the_whole_set) {
    SPLICE_REQUIRE_LIVE_TIER();
    // register_global_installer(fn) is public API and predates groups. An
    // installer registered that way can still install on its own; what it
    // cannot do is join one transaction. Leaving it out would hand back a
    // group the caller believes is complete, so the set is refused instead.
    QueuedHook<0x90501> a(reinterpret_cast<void*>(&batch_target_a), "batch_a");
    (*a).onInvoke([](TargetFn orig, int x) {
        g_a_calls.fetch_add(1, std::memory_order_relaxed);
        return orig(x) + 1;
    });

    int ran = 0;
    const splice::InstallerToken hand_rolled =
        splice::register_global_installer([&ran]() { ++ran; });

    const splice::Outcome refused = splice::install_all(splice::live);
    EXPECT_EQ(refused.state(), splice::State::refused);
    EXPECT_EQ(refused.installed_sites(), 0u);

    EXPECT_EQ(ran, 0) << "a refused transaction ran an installer anyway";
    EXPECT_EQ(batch_target_a(kArg), kPlainA)
        << "the set was refused but a site was installed";
    EXPECT_FALSE((*a).is_installed());

    // The untiered form still installs it, which is the point of saying no
    // rather than dropping it.
    splice::install_all();
    EXPECT_EQ(ran, 1);
    EXPECT_TRUE((*a).is_installed());
    EXPECT_EQ(batch_target_a(kArg), kPlainA + 1);
    EXPECT_TRUE((*a).disable());
}

TEST_F(InstallAllTier, a_symbol_that_will_not_resolve_refuses_the_whole_set) {
    SPLICE_REQUIRE_LIVE_TIER();
    // The symbol path, and the ordering that makes it worth a case of its own:
    // resolution happens while the group is being filled, before anything is
    // allocated and long before anything is suspended. A symbol that is not
    // there is a refusal with nothing to undo.
    QueuedHook<0x90601> good(reinterpret_cast<void*>(&batch_target_a), "batch_a");
    (*good).onInvoke([](TargetFn orig, int x) {
        g_a_calls.fetch_add(1, std::memory_order_relaxed);
        return orig(x) + 1;
    });

    QueuedHook<0x90602> missing("splice_no_such_library", "splice_no_such_symbol");
    (*missing).onInvoke([](TargetFn orig, int x) { return orig(x); });

    const splice::Outcome refused = splice::install_all(splice::live);
    EXPECT_EQ(refused.state(), splice::State::refused);
    EXPECT_EQ(refused.installed_sites(), 0u);
    EXPECT_EQ(refused.failure().stage, splice::Stage::prepare)
        << "a symbol that does not resolve must fail before anything is written";

    EXPECT_EQ(batch_target_a(kArg), kPlainA)
        << "the resolvable site was installed although the set was refused";
    EXPECT_FALSE((*good).is_installed());
    EXPECT_FALSE((*missing).is_installed());
}

TEST_F(InstallAllTier, a_hook_whose_gates_are_unsatisfiable_refuses_the_whole_set) {
    SPLICE_REQUIRE_LIVE_TIER();
    // A re-entrancy scope that could not be reserved already makes the
    // single-site install refuse. In a group it has to refuse the group: the
    // entry has no usable callback, so installing it would put a patch in
    // front of a hook that cannot fire.
    QueuedHook<0x90701> a(reinterpret_cast<void*>(&batch_target_a), "batch_a");
    (*a).onInvoke([](TargetFn orig, int x) { return orig(x) + 1; });

    // Hold every scope there is, so the next request cannot be satisfied.
    // Counting to 64 is not the test for that -- the sixty-fourth request is
    // the last one that SUCCEEDS. The probe below is what says the supply is
    // gone, and it has to stay alive, because releasing it would hand the bit
    // straight back to the hook this case needs starved.
    std::vector<std::unique_ptr<splice::Reentrancy>> held;
    for (int i = 0; i < 64; ++i) {
        auto scope = std::make_unique<splice::Reentrancy>();
        if (scope->bit() == 0) break;
        held.push_back(std::move(scope));
    }
    const splice::Reentrancy probe;
    if (probe.bit() != 0) {
        GTEST_SKIP() << "a re-entrancy scope was still available after holding "
                     << held.size() << "; this case needs the supply exhausted";
    }

    QueuedHook<0x90702> starved(reinterpret_cast<void*>(&batch_target_b), "batch_b");
    (*starved).non_reentrant().onInvoke([](TargetFn orig, int x) { return orig(x); });

    const splice::Outcome refused = splice::install_all(splice::live);
    EXPECT_EQ(refused.state(), splice::State::refused);
    EXPECT_EQ(refused.installed_sites(), 0u);
    EXPECT_EQ(batch_target_a(kArg), kPlainA);
    EXPECT_EQ(batch_target_b(kArg), kPlainB);
}
