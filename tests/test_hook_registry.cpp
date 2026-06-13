// ─── HookRegistry<Impl> tests (FR-010 Step 6.2) ──────────────────────────
//
// Direct exercise of both HookRegistry partial specialisations without
// going through HookContext or the live patcher. Validates:
//
//   - default-empty get_or_create installs a fresh slot
//   - same id returns the SAME object across calls (identity invariant)
//   - concurrent readers see a stable slot (no UAF, no tearing)
//   - concurrent writers cooperatively install distinct ids
//
// Both impls share the same interface contract, so each test runs
// twice via a typed-test fixture — one parametrisation per registry impl.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/context.h>
#include <splice/registry_impl.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace {

// A minimal HookBase-derived type just for identity tracking — these tests
// don't care about hook semantics, only the registry's slot management.
struct DummyHook : splice::HookBase {
    int payload = 0;
};

template <typename Impl>
class HookRegistryTest : public ::testing::Test {
protected:
    splice::HookRegistry<Impl> registry;
};

using RegistryImpls = ::testing::Types<
    splice::registry::shared_mutex_map,
    splice::registry::rcu_atomic_array>;

class RegistryImplNames {
public:
    template <typename T>
    static std::string GetName(int) {
        if constexpr (std::is_same_v<T, splice::registry::shared_mutex_map>) {
            return "shared_mutex_map";
        } else if constexpr (std::is_same_v<T, splice::registry::rcu_atomic_array>) {
            return "rcu_atomic_array";
        } else {
            return "unknown";
        }
    }
};

TYPED_TEST_SUITE(HookRegistryTest, RegistryImpls, RegistryImplNames);

// ─── Test 1 — basic install + lookup ─────────────────────────────────────
TYPED_TEST(HookRegistryTest, get_or_create_installs_fresh_slot) {
    EXPECT_EQ(this->registry.size(), 0u);

    auto& h = this->registry.template get_or_create<DummyHook>(7);
    h.payload = 42;
    EXPECT_EQ(this->registry.size(), 1u);
    EXPECT_EQ(h.payload, 42);
}

// ─── Test 2 — identity invariant ─────────────────────────────────────────
TYPED_TEST(HookRegistryTest, same_id_returns_same_object) {
    auto& h1 = this->registry.template get_or_create<DummyHook>(3);
    h1.payload = 100;
    auto& h2 = this->registry.template get_or_create<DummyHook>(3);
    EXPECT_EQ(&h1, &h2);
    EXPECT_EQ(h2.payload, 100);

    // Different id → different object.
    auto& h3 = this->registry.template get_or_create<DummyHook>(4);
    EXPECT_NE(&h1, &h3);
    EXPECT_EQ(this->registry.size(), 2u);
}

// ─── Test 3 — concurrent readers don't tear ──────────────────────────────
TYPED_TEST(HookRegistryTest, concurrent_readers_see_stable_slot) {
    // Pre-install a single slot. Then 8 threads read it concurrently.
    auto& origin = this->registry.template get_or_create<DummyHook>(11);
    origin.payload = 0xC0FFEE;

    constexpr int kThreads = 8;
    constexpr int kIters   = 50000;
    std::atomic<int> bad{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t) {
        pool.emplace_back([&]() {
            for (int i = 0; i < kIters; ++i) {
                auto& h = this->registry.template get_or_create<DummyHook>(11);
                if (h.payload != 0xC0FFEE || &h != &origin) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    EXPECT_EQ(bad.load(), 0);
}

// ─── Test 4 — concurrent writers install distinct ids cooperatively ──────
TYPED_TEST(HookRegistryTest, concurrent_writers_install_distinct_ids) {
    constexpr int kThreads = 8;
    constexpr int kIdsPerThread = 16;
    std::atomic<int> bad{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t) {
        pool.emplace_back([&, t]() {
            for (int i = 0; i < kIdsPerThread; ++i) {
                const int id = t * kIdsPerThread + i;   // unique per thread
                auto& h = this->registry.template get_or_create<DummyHook>(id);
                h.payload = id;
                // Read back — same writer thread, must see own write.
                auto& h2 = this->registry.template get_or_create<DummyHook>(id);
                if (&h != &h2 || h2.payload != id) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    EXPECT_EQ(bad.load(), 0);
    EXPECT_EQ(this->registry.size(),
              static_cast<std::size_t>(kThreads * kIdsPerThread));
}

// ─── Test 5 — RCU-only: retired snapshots reclaimed after grace period ──
// Lives outside the typed-test fixture because retired_count() only exists
// on the rcu_atomic_array specialisation (the shared_mutex_map impl has
// no retire queue — it free()s in place under unique_lock).
TEST(HookRegistry_RcuArray, reclaims_after_grace_period) {
    splice::HookRegistry<splice::registry::rcu_atomic_array> r;

    // Force several snapshot replacements by installing distinct ids.
    // Each install retires the previous snapshot.
    for (int i = 0; i < 5; ++i) {
        (void)r.template get_or_create<DummyHook>(i);
    }
    // 5 installs → 4 retires (first install has no predecessor).
    EXPECT_EQ(r.retired_count(), 4u);

    // Sleep just past grace period (SPLICE_RCU_GRACE_PERIOD_MS = 100 ms).
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    // Trigger a drain. Could be done by installing a new slot OR by
    // explicit reclaim_old_snapshots(). Use the explicit API so the test
    // doesn't have to know how many installs to fire.
    r.reclaim_old_snapshots();
    EXPECT_EQ(r.retired_count(), 0u);

    // Live snapshot still valid — those 5 slots are reachable.
    EXPECT_EQ(r.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        auto& h = r.template get_or_create<DummyHook>(i);
        h.payload = i + 1000;
        EXPECT_EQ(h.payload, i + 1000);
    }
}

} // namespace
