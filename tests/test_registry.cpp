// ─── Registry tests ────────────────────────────────────────────────────────
// Exercises IdRegistry, OriginalRegistry, HookManager, and the global
// installer queue without needing any actual hooking to take place.
// Platform-independent.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>
#include <splice/registry.h>

#include <atomic>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// Synthetic registry ids, expressed against the capacity rather than written
// as round numbers. The hot-path registries are selected at build time and the
// default indexes an array by id, so an id is an index, not a label: 30001 was
// a perfectly good name under the old map and is out of range under the array.
// Counting down from the top also keeps these clear of the low slots
// `HookContext::slot_for` issues to real hooks in this process.
//
// The capacity comes from the library at runtime, not from the preprocessor.
// Since 2026-10-13 `SPLICE_MAX_HOOKS` is private to the library's own build --
// the slot arrays live behind pointers, so no consumer's layout depends on it
// and no consumer is told the value. A test is a consumer like any other.
const int kCapacity              = splice::detail::configured_capacity();
const int kIdOriginalRoundTrip   = kCapacity - 1;
const int kIdOriginalByKey       = kCapacity - 2;
const int kIdNothingStored       = kCapacity - 3;
const int kIdHookNoCallback      = kCapacity - 4;
const int kIdHookWithCallback    = kCapacity - 5;
const int kIdHookIntSignature    = kCapacity - 6;
const int kIdHookDoubleSignature = kCapacity - 7;

} // namespace

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
    splice::OriginalRegistry::set_original<Fn>(kIdOriginalRoundTrip, &fake_original_unary);

    auto got = splice::OriginalRegistry::get_original<Fn>(kIdOriginalRoundTrip);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got(41), 42);
}

TEST(OriginalRegistry, unknown_id_returns_nullptr) {
    using Fn = int (*)(int);
    // In range, and nothing was ever stored at it -- which is the case this
    // test is named for. It used to pass 999999, which under the array
    // implementation exercises the out-of-range guard instead.
    EXPECT_EQ(splice::OriginalRegistry::get_original<Fn>(kIdNothingStored), nullptr);
}

TEST(OriginalRegistry, lookup_by_key_resolves_via_id_registry) {
    using Fn = int (*)(int);
    splice::IdRegistry::register_mapping("lookup_libX_funcB", kIdOriginalByKey);
    splice::OriginalRegistry::set_original<Fn>(kIdOriginalByKey, &fake_original_unary);

    auto got = splice::OriginalRegistry::get_original_by_key<Fn>("lookup_libX_funcB");
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got(9), 10);
}

// ─── HookManager ────────────────────────────────────────────────────────────

namespace {
int passthrough_plus_two(int x) { return x + 2; }
}

TEST(HookManager, invoke_without_callback_calls_original) {
    auto& hook = splice::HookManager::get_hook<int, int>(kIdHookNoCallback);

    const int result = hook.invoke(&passthrough_plus_two, 5);
    EXPECT_EQ(result, 7);  // original: x + 2
}

TEST(HookManager, set_invoke_redirects_to_callback) {
    using Fn = int (*)(int);
    auto& hook = splice::HookManager::get_hook<int, int>(kIdHookWithCallback);

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

    auto& hook_i = splice::HookManager::get_hook<int, int>(kIdHookIntSignature);
    auto& hook_d = splice::HookManager::get_hook<double, double>(kIdHookDoubleSignature);

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

// ─── Checked accessors ─────────────────────────────────────────────────────
// Added 2026-10-13. The pointer-returning spellings cannot distinguish a
// refusal from an empty slot, and the `void` setter cannot report one at all,
// so a caller passing an id it computed itself could silently lose an original
// and only find out from a log. These assert the distinction the checked forms
// exist to make.

TEST(CheckedAccessors, an_empty_slot_is_a_successful_lookup_not_a_refusal) {
    using Fn = int (*)(int);
    const auto got = splice::OriginalRegistry::try_get_original<Fn>(kIdNothingStored);

    // Nothing is stored here, which is an answer and not a failure.
    EXPECT_TRUE(static_cast<bool>(got));
    EXPECT_EQ(got.reason, splice::Refusal::ok);
    EXPECT_EQ(got.value, nullptr);
}

TEST(CheckedAccessors, an_out_of_range_id_is_refused_and_says_why) {
    using Fn = int (*)(int);

    // -1 is outside every configuration, map or array.
    const auto got = splice::OriginalRegistry::try_get_original<Fn>(-1);
    EXPECT_FALSE(static_cast<bool>(got));
    EXPECT_EQ(got.reason, splice::Refusal::out_of_range);
    EXPECT_EQ(got.value, nullptr);

    EXPECT_EQ(splice::OriginalRegistry::try_set_original<Fn>(-1, &fake_original_unary),
              splice::Refusal::out_of_range);

    const auto hook = splice::HookManager::try_get_hook<int, int>(-1);
    EXPECT_FALSE(static_cast<bool>(hook));
    EXPECT_EQ(hook.reason, splice::Refusal::out_of_range);
    EXPECT_EQ(hook.value, nullptr);
}

TEST(CheckedAccessors, the_capacity_bound_is_refused_where_there_is_one) {
    using Fn = int (*)(int);
    const int cap = splice::default_context().slot_capacity();
    if (cap == (std::numeric_limits<int>::max)()) {
        GTEST_SKIP() << "this configuration's registry is unbounded (a map), "
                        "so there is no bound to refuse at";
    }

    EXPECT_EQ(splice::OriginalRegistry::try_set_original<Fn>(cap, &fake_original_unary),
              splice::Refusal::out_of_range);
    EXPECT_EQ(splice::OriginalRegistry::try_get_original<Fn>(cap).reason,
              splice::Refusal::out_of_range);

    // The template argument list's comma has to be kept out of the macro, so
    // the result lands in a local first.
    const auto at_bound = splice::HookManager::try_get_hook<int, int>(cap);
    EXPECT_EQ(at_bound.reason, splice::Refusal::out_of_range);

    // One below it is a real slot, so the bound is the bound and not an
    // off-by-one.
    const auto below_bound = splice::HookManager::try_get_hook<int, int>(cap - 1);
    EXPECT_EQ(below_bound.reason, splice::Refusal::ok);
}

TEST(CheckedAccessors, a_checked_hook_is_the_same_object_as_the_legacy_reference) {
    const auto checked = splice::HookManager::try_get_hook<int, int>(kIdHookNoCallback);
    ASSERT_TRUE(static_cast<bool>(checked));
    auto& legacy = splice::HookManager::get_hook<int, int>(kIdHookNoCallback);
    EXPECT_EQ(checked.value, &legacy)
        << "the checked form must not be a second slot for the same id";
}

TEST(CheckedAccessors, the_legacy_setter_refuses_without_storing_and_without_crashing) {
    using Fn = int (*)(int);

    // The accepted limitation, asserted rather than described: this signature
    // cannot report, so it logs and discards. What must not happen is a store
    // at an out-of-range id or a dead process.
    splice::OriginalRegistry::set_original<Fn>(-1, &fake_original_unary);
    EXPECT_EQ(splice::OriginalRegistry::get_original<Fn>(-1), nullptr);

    // And the reference-returning spelling hands back something usable rather
    // than dereferencing a refusal.
    auto& inert = splice::HookManager::get_hook<int, int>(-1);
    inert.set_invoke([](int (*orig)(int), int x) { return orig ? orig(x) : x; });
    SUCCEED() << "a refused reference lookup returned an object, not a crash";
}
