// ─── Registry tests ────────────────────────────────────────────────────────
// Exercises IdRegistry, OriginalRegistry, HookManager, and the global
// installer queue without needing any actual hooking to take place.
// Platform-independent.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>
#include <splice/registry.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

TEST(IdRegistry, registers_and_looks_up_by_key) {
    // Use fresh keys to avoid collision with earlier tests in the same process.
    splice::IdRegistry::register_mapping("test_libX_funcA", 10001);
    EXPECT_EQ(splice::IdRegistry::get_id_by_key("test_libX_funcA"), 10001);
    EXPECT_EQ(splice::IdRegistry::get_key_by_id(10001), "test_libX_funcA");
}

TEST(IdRegistry, lookup_missing_key_returns_minus_one) {
    EXPECT_EQ(splice::IdRegistry::get_id_by_key("no_such_key_xyz"), -1);
    EXPECT_TRUE(splice::IdRegistry::get_key_by_id(-42).empty());
}

TEST(IdRegistry, thread_safe_register_and_lookup) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 100;

    std::vector<std::thread> pool;
    std::atomic<int> race_failures{0};
    for (int t = 0; t < kThreads; ++t) {
        pool.emplace_back([t, &race_failures]() {
            for (int i = 0; i < kPerThread; ++i) {
                const int id = 20000 + t * kPerThread + i;
                const std::string key = "racing_" + std::to_string(id);
                splice::IdRegistry::register_mapping(key, id);
                if (splice::IdRegistry::get_id_by_key(key) != id) {
                    race_failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    EXPECT_EQ(race_failures.load(), 0);
}

// ─── OriginalRegistry ───────────────────────────────────────────────────────

namespace {
int fake_original_unary(int x) { return x + 1; }
}

TEST(OriginalRegistry, set_and_get_round_trip) {
    using Fn = int (*)(int);
    splice::OriginalRegistry::set_original<Fn>(30001, &fake_original_unary);

    auto got = splice::OriginalRegistry::get_original<Fn>(30001);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got(41), 42);
}

TEST(OriginalRegistry, unknown_id_returns_nullptr) {
    using Fn = int (*)(int);
    EXPECT_EQ(splice::OriginalRegistry::get_original<Fn>(999999), nullptr);
}

TEST(OriginalRegistry, lookup_by_key_resolves_via_id_registry) {
    using Fn = int (*)(int);
    splice::IdRegistry::register_mapping("lookup_libX_funcB", 30002);
    splice::OriginalRegistry::set_original<Fn>(30002, &fake_original_unary);

    auto got = splice::OriginalRegistry::get_original_by_key<Fn>("lookup_libX_funcB");
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got(9), 10);
}

// ─── HookManager ────────────────────────────────────────────────────────────

namespace {
int passthrough_plus_two(int x) { return x + 2; }
}

TEST(HookManager, invoke_without_callback_calls_original) {
    auto& hook = splice::HookManager::get_hook<int, int>(40001);

    const int result = hook.invoke(&passthrough_plus_two, 5);
    EXPECT_EQ(result, 7);  // original: x + 2
}

TEST(HookManager, set_invoke_redirects_to_callback) {
    using Fn = int (*)(int);
    auto& hook = splice::HookManager::get_hook<int, int>(40002);

    hook.set_invoke([](Fn orig, int x) {
        return orig(x) * 10;  // original is x+2, callback multiplies
    });
    EXPECT_EQ(hook.invoke(&passthrough_plus_two, 3), 50);  // (3+2)*10
}

TEST(HookManager, different_signatures_at_different_ids_are_independent) {
    // Phase 1.5 note: before the HookContext consolidation, the old
    // HookManager stored hooks in a *per-signature* static map, so the
    // same unique_id could coexist under `<int,int>` and `<double,double>`.
    // That was an accidental quirk of the template-instantiation layout,
    // never reachable from the public SPLICE_HOOK macros (every call site
    // gets its own `__COUNTER__` id, and the signature is fixed by
    // `decltype(&func)`). The new contract is: one unique_id → one
    // implicit signature. This test now uses distinct ids, which mirrors
    // how real code obtains them.
    using IntFn = int (*)(int);
    using DblFn = double (*)(double);

    auto& hook_i = splice::HookManager::get_hook<int, int>(40003);
    auto& hook_d = splice::HookManager::get_hook<double, double>(40004);

    hook_i.set_invoke([](IntFn, int x) { return x * 2; });
    hook_d.set_invoke([](DblFn, double x) { return x + 0.5; });

    EXPECT_EQ(hook_i.invoke(nullptr, 5), 10);
    EXPECT_DOUBLE_EQ(hook_d.invoke(nullptr, 1.5), 2.0);
}

// ─── Global installer queue ─────────────────────────────────────────────────

TEST(GlobalInstallers, register_and_install_all_runs_each) {
    const std::size_t before = splice::global_installer_count();

    std::atomic<int> counter{0};
    auto t1 = splice::register_global_installer([&counter]() { counter.fetch_add(1); });
    auto t2 = splice::register_global_installer([&counter]() { counter.fetch_add(10); });
    auto t3 = splice::register_global_installer([&counter]() { counter.fetch_add(100); });

    const std::size_t after = splice::global_installer_count();
    EXPECT_EQ(after - before, 3u);

    // install_all iterates every registered installer, including the ones
    // from prior tests. We only assert our own contributions landed.
    splice::install_all();
    EXPECT_GE(counter.load(), 111);

    // Tokens t1/t2/t3 release here — the slots return to the free list and
    // installer_count() drops back to `before` (Task #57 RAII behaviour).
}

TEST(GlobalInstallers, token_release_marks_slot_dead) {
    const std::size_t before = splice::global_installer_count();
    std::atomic<int> hits{0};
    {
        auto t = splice::register_global_installer([&hits]() { hits.fetch_add(1); });
        EXPECT_EQ(splice::global_installer_count(), before + 1);
    } // token drops here
    EXPECT_EQ(splice::global_installer_count(), before);

    // After release, install_all must NOT invoke the dead lambda.
    splice::install_all();
    EXPECT_EQ(hits.load(), 0);
}

TEST(GlobalInstallers, freed_slot_is_recycled) {
    const std::size_t before = splice::global_installer_count();
    int slot_first = -1;
    int slot_second = -1;
    {
        auto t = splice::register_global_installer([]() {});
        slot_first = t.slot();
    }
    auto t2 = splice::register_global_installer([]() {});
    slot_second = t2.slot();
    EXPECT_EQ(slot_first, slot_second)
        << "free-list should hand the just-released slot back";
    EXPECT_EQ(splice::global_installer_count(), before + 1);
}
