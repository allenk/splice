// ─── Per-call-site policy override (FR-010 Step 4) ────────────────────────
//
// Verifies that SPLICE_HOOK_ADDR_AS(Policy, ...) pins the concurrency
// policy for that one hook without affecting the process-wide default.
// Two hooks live side-by-side in the same binary — one with the default
// rcu_writeonce, one with shared_mutex — and both must dispatch through
// their own HookStorage<> spec without cross-pollination.
//
// Cross-pollination would surface as a static_cast UB inside
// HookContext::get_hook_as: each id is downcast to its declared
// (Ret, Policy, Args...) tuple, so a mismatch would either segfault or
// invoke the wrong storage spec. We assert call-site behaviour, not
// pointer identity, since UB by design hides itself.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/splice.h>

#include "helpers/test_targets.h"

#include <atomic>

#if (defined(__aarch64__) || defined(__arm64__)) && \
    (defined(__linux__) || defined(__ANDROID__))
#   define SPLICE_POLICY_TEST_LIVE 1
#elif defined(__x86_64__) || defined(_M_X64)
#   define SPLICE_POLICY_TEST_LIVE 1
#else
#   define SPLICE_POLICY_TEST_LIVE 0
#endif

#if SPLICE_POLICY_TEST_LIVE

namespace {
std::atomic<int> g_default_hits{0};
std::atomic<int> g_swap_hits{0};
} // namespace

TEST(PolicyOverride, default_and_shared_mutex_coexist) {
    // Hook #1 — default policy (rcu_writeonce). Targets policy_target_a.
    // Dedicated targets (not add_one / multiply_two) so cross-test installer
    // replay can't double-patch over LiveHook / InlineDisable's _STATIC entries.
    SPLICE_HOOK_ADDR_STATIC(&splice::test::policy_target_a)
        .onInvoke([](auto orig, int x) {
            g_default_hits.fetch_add(1, std::memory_order_relaxed);
            return orig(x) + 100;
        });

    // Hook #2 — shared_mutex policy via _AS. Targets policy_target_b.
    SPLICE_HOOK_ADDR_AS_STATIC(splice::policy::shared_mutex, &splice::test::policy_target_b)
        .onInvoke([](auto orig, int x) {
            g_swap_hits.fetch_add(1, std::memory_order_relaxed);
            return orig(x) + 1000;
        });

    g_default_hits.store(0, std::memory_order_relaxed);
    g_swap_hits.store(0, std::memory_order_relaxed);

    splice::install_all();

    // ── Both should dispatch through their own storage spec ──────────────
    // policy_target_a(1)   baseline = 2  → hooked = 102
    // policy_target_b(7)   baseline = 14 → hooked = 1014
    if (splice_is_hooked(reinterpret_cast<void*>(&splice::test::policy_target_a))) {
        EXPECT_EQ(splice::test::policy_target_a(1), 102);
        EXPECT_GT(g_default_hits.load(), 0);
    } else {
        GTEST_SKIP() << "policy_target_a not patchable on this build (stub backend)";
    }

    if (splice_is_hooked(reinterpret_cast<void*>(&splice::test::policy_target_b))) {
        EXPECT_EQ(splice::test::policy_target_b(7), 1014);
        EXPECT_GT(g_swap_hits.load(), 0);
    }

    // ── Cross-pollination check: each hook only ran its own callback ──────
    // (a bad downcast would have routed one through the other's storage)
    EXPECT_EQ(g_default_hits.load(), 1);
    EXPECT_EQ(g_swap_hits.load(), 1);
}

#else  // !SPLICE_POLICY_TEST_LIVE

TEST(PolicyOverride, disabled_on_unsupported_platform) {
    GTEST_SKIP() << "No live patcher on this platform";
}

#endif
