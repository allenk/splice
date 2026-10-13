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

// ─── Test 5 — clear() detaches slots and destroys nothing ────────────────
// The RCU-only test that used to live here asserted the opposite: that retired
// snapshots are *reclaimed* once a 100 ms grace period has elapsed. It passed
// every run, and what it certified was a use-after-free waiting on a
// scheduler — a reader loads the snapshot pointer and then dereferences the
// array, and a thread preempted between those two steps exceeds any grace
// period. Elapsed time is not quiescence. So the snapshot and its retire queue
// are gone, and this is the contract that replaced them.
//
// Under the old implementation the second half of this test was undefined
// behaviour: `clear()` retired the snapshot that owned the hooks through
// shared_ptr, so a pointer held across it dangled once the period elapsed.
// Here it is the guarantee.
TEST(HookRegistry_RcuArray, clear_detaches_slots_without_destroying_hooks) {
    splice::HookRegistry<splice::registry::rcu_atomic_array> r;

    DummyHook* held[5] = {};
    for (int i = 0; i < 5; ++i) {
        auto& h = r.template get_or_create<DummyHook>(i);
        h.payload = i + 1000;
        held[i] = &h;
    }
    EXPECT_EQ(r.size(), 5u);

    r.clear();

    // Detached: the registry reports nothing, and a fresh lookup builds a new
    // object rather than handing back the old one.
    EXPECT_EQ(r.size(), 0u);
    auto& fresh = r.template get_or_create<DummyHook>(0);
    EXPECT_NE(&fresh, held[0]);
    EXPECT_EQ(fresh.payload, 0);          // a new object, not the old payload

    // Not destroyed: every pointer taken before the clear is still usable,
    // which is what lets a reader in flight finish against what it already
    // has. The objects are program-lifetime on purpose — see the registry's
    // destructor and .lsan-suppressions.txt.
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(held[i]->payload, i + 1000);
        held[i]->payload = i + 2000;
        EXPECT_EQ(held[i]->payload, i + 2000);
    }
}

// ─── Test 6 — RCU-only: the capacity is a runtime bound, and it refuses ──
// `capacity()` replaced a `static constexpr kCapacity` so that the configured
// value stops appearing in any header-visible layout. This asserts the bound
// is still enforced, from the other side of that change.
TEST(HookRegistry_RcuArray, capacity_is_runtime_and_out_of_range_is_refused) {
    splice::HookRegistry<splice::registry::rcu_atomic_array> r;

    ASSERT_TRUE(r.storage_ok());
    EXPECT_EQ(r.capacity(), splice::detail::configured_capacity());

    // At the bound and beyond it: logged, ignored, and not stored. The inert
    // object this returns is shared, which is why nothing installs through it.
    (void)r.template get_or_create<DummyHook>(r.capacity());
    (void)r.template get_or_create<DummyHook>(-1);
    EXPECT_EQ(r.size(), 0u);

    // One below the bound is a real slot.
    auto& last = r.template get_or_create<DummyHook>(r.capacity() - 1);
    last.payload = 7;
    EXPECT_EQ(r.size(), 1u);
    EXPECT_EQ(r.template get_or_create<DummyHook>(r.capacity() - 1).payload, 7);
}

} // namespace
