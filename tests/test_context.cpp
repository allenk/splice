// ─── HookContext tests (FR-008 / Phase 1.5) ───────────────────────────────
// Proves that HookContext instances are genuinely independent, that
// default_context() is a true process-wide singleton, and that reset()
// wipes everything.
// Platform-independent.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>
#include <splice/context.h>
#include <splice/registry.h>  // for the backwards-compat shim tests

#include <atomic>
#include <thread>
#include <vector>

namespace {

int sample_add1(int x) { return x + 1; }
int sample_add2(int x) { return x + 2; }

} // namespace

// ─── default_context singleton identity ─────────────────────────────────────

TEST(DefaultContext, returns_same_instance_across_calls) {
    auto* p1 = &splice::default_context();
    auto* p2 = &splice::default_context();
    EXPECT_EQ(p1, p2);
}

// ─── isolation between HookContext instances ───────────────────────────────

TEST(HookContext, two_instances_have_independent_id_registries) {
    splice::HookContext a;
    splice::HookContext b;

    a.register_mapping("same_key", 111);
    b.register_mapping("same_key", 222);

    EXPECT_EQ(a.get_id_by_key("same_key"), 111);
    EXPECT_EQ(b.get_id_by_key("same_key"), 222);
}

TEST(HookContext, original_registries_are_independent) {
    splice::HookContext a;
    splice::HookContext b;

    a.set_original<int (*)(int)>(500, &sample_add1);
    b.set_original<int (*)(int)>(500, &sample_add2);

    auto fa = a.get_original<int (*)(int)>(500);
    auto fb = b.get_original<int (*)(int)>(500);
    ASSERT_NE(fa, nullptr);
    ASSERT_NE(fb, nullptr);
    EXPECT_EQ(fa(10), 11);
    EXPECT_EQ(fb(10), 12);
}

TEST(HookContext, hook_registries_are_independent) {
    splice::HookContext a;
    splice::HookContext b;

    a.get_hook<int, int>(600).set_invoke([](int (*)(int), int x) {
        return x * 10;
    });
    b.get_hook<int, int>(600).set_invoke([](int (*)(int), int x) {
        return x * 100;
    });

    // EXPECT_EQ is a variadic macro; extra parens protect the inner
    // `<int, int>` commas from being eaten as macro-argument separators.
    EXPECT_EQ((a.get_hook<int, int>(600).invoke(nullptr, 3)), 30);
    EXPECT_EQ((b.get_hook<int, int>(600).invoke(nullptr, 3)), 300);
}

TEST(HookContext, installer_queues_are_independent) {
    splice::HookContext a;
    splice::HookContext b;

    std::atomic<int> a_hits{0};
    std::atomic<int> b_hits{0};

    auto ta1 = a.register_installer([&a_hits]() { a_hits.fetch_add(1); });
    auto ta2 = a.register_installer([&a_hits]() { a_hits.fetch_add(10); });
    auto tb1 = b.register_installer([&b_hits]() { b_hits.fetch_add(100); });

    EXPECT_EQ(a.installer_count(), 2u);
    EXPECT_EQ(b.installer_count(), 1u);

    a.install_all();
    EXPECT_EQ(a_hits.load(), 11);
    EXPECT_EQ(b_hits.load(), 0);

    b.install_all();
    EXPECT_EQ(a_hits.load(), 11);
    EXPECT_EQ(b_hits.load(), 100);
}

// ─── reset() ────────────────────────────────────────────────────────────────

TEST(HookContext, reset_clears_id_registry) {
    splice::HookContext ctx;
    ctx.register_mapping("ephemeral_key", 777);
    ASSERT_EQ(ctx.get_id_by_key("ephemeral_key"), 777);

    ctx.reset();
    EXPECT_EQ(ctx.get_id_by_key("ephemeral_key"), -1);
    EXPECT_TRUE(ctx.get_key_by_id(777).empty());
}

TEST(HookContext, reset_clears_originals) {
    splice::HookContext ctx;
    ctx.set_original<int (*)(int)>(800, &sample_add1);
    ASSERT_NE(ctx.get_original<int (*)(int)>(800), nullptr);

    ctx.reset();
    EXPECT_EQ(ctx.get_original<int (*)(int)>(800), nullptr);
}

TEST(HookContext, reset_clears_installers) {
    splice::HookContext ctx;
    auto t1 = ctx.register_installer([]() {});
    auto t2 = ctx.register_installer([]() {});
    ASSERT_EQ(ctx.installer_count(), 2u);

    ctx.reset();
    EXPECT_EQ(ctx.installer_count(), 0u);

    // Outstanding tokens must no-op on release after reset (generation
    // bump). If reset() failed to invalidate them, the token destructors
    // running after this scope ends would touch a wiped m_installers
    // vector and crash. Just letting them go out of scope is the test.
}

TEST(HookContext, reset_invalidates_outstanding_tokens) {
    splice::HookContext ctx;
    splice::InstallerToken stale_a;
    splice::InstallerToken stale_b;
    {
        stale_a = ctx.register_installer([]() {});
        stale_b = ctx.register_installer([]() {});
        ASSERT_EQ(ctx.installer_count(), 2u);
    }
    ctx.reset();
    EXPECT_EQ(ctx.installer_count(), 0u);

    // New registrations under the bumped generation must not be wiped
    // when the stale tokens release at end of test.
    auto live = ctx.register_installer([]() {});
    EXPECT_EQ(ctx.installer_count(), 1u);
    // stale_a and stale_b release here — must NOT touch the live slot.
}

TEST(HookContext, reset_clears_hooks) {
    splice::HookContext ctx;
    ctx.get_hook<int, int>(900).set_invoke([](int (*)(int), int x) { return x * 5; });
    EXPECT_EQ((ctx.get_hook<int, int>(900).invoke(nullptr, 2)), 10);

    ctx.reset();
    // After reset, a fresh Hook<> is created — its default invoke calls
    // the original, not the previous callback.
    ctx.set_original<int (*)(int)>(900, &sample_add1);
    auto orig = ctx.get_original<int (*)(int)>(900);
    EXPECT_EQ((ctx.get_hook<int, int>(900).invoke(orig, 7)), 8);
}

// ─── Thread safety ──────────────────────────────────────────────────────────

TEST(HookContext, concurrent_register_mapping_is_thread_safe) {
    splice::HookContext ctx;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 100;

    std::atomic<int> failures{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t) {
        pool.emplace_back([t, &ctx, &failures]() {
            for (int i = 0; i < kPerThread; ++i) {
                const int id = t * kPerThread + i + 1;
                const std::string key = "t" + std::to_string(t) + "_i" + std::to_string(i);
                ctx.register_mapping(key, id);
                if (ctx.get_id_by_key(key) != id) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    EXPECT_EQ(failures.load(), 0);
}

// ─── Backwards-compat: the shim classes still talk to default_context() ────

TEST(ShimCompat, IdRegistry_reaches_default_context) {
    // Fresh, unique key so we don't collide with earlier tests.
    splice::IdRegistry::register_mapping("shim_compat_probe_2026", 12345);
    EXPECT_EQ(splice::default_context().get_id_by_key("shim_compat_probe_2026"), 12345);
}

TEST(ShimCompat, install_all_runs_installers_queued_via_shim) {
    std::atomic<int> hits{0};
    const auto before = splice::global_installer_count();
    auto token = splice::register_global_installer([&hits]() { hits.fetch_add(1); });
    EXPECT_EQ(splice::global_installer_count(), before + 1);

    splice::install_all();
    EXPECT_GE(hits.load(), 1);
}
