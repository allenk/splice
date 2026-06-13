// ─── HookStorage policy tests (FR-010 Step 3) ────────────────────────────
//
// Direct exercise of both HookStorage<Policy, Ret, Args...> specialisations
// without going through the live patcher. Validates:
//
//   - default-constructed storage falls through to original
//   - store() then invoke() runs the user lambda
//   - re-store() replaces the user lambda (rcu_writeonce will leak the
//     old one, shared_mutex will overwrite — both visible to next reader)
//   - thread-safety smoke under modest contention
//
// Lives in the unit test binary; both specialisations get compiled in
// every build regardless of SPLICE_DEFAULT_POLICY.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/context.h>
#include <splice/policy.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

int original_add(int a, int b) { return a + b; }

template <typename Policy>
void exercise_basic() {
    splice::HookStorage<Policy, int, int, int> storage;

    // 1) No store yet — invoke calls through to original.
    EXPECT_EQ(storage.invoke(&original_add, 2, 3), 5);

    // 2) Install a multiplier-style override.
    storage.store([](int (*orig)(int, int), int a, int b) {
        return orig(a, b) * 10;
    });
    EXPECT_EQ(storage.invoke(&original_add, 2, 3), 50);

    // 3) Replace with a different override.
    storage.store([](int (*)(int, int), int a, int b) {
        return a - b;
    });
    EXPECT_EQ(storage.invoke(&original_add, 10, 3), 7);
}

template <typename Policy>
void exercise_concurrent_readers() {
    splice::HookStorage<Policy, int, int, int> storage;
    storage.store([](int (*orig)(int, int), int a, int b) {
        return orig(a, b) + 1;
    });

    constexpr int kThreads = 8;
    constexpr int kIters   = 50000;
    std::atomic<int> bad{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t) {
        pool.emplace_back([&]() {
            for (int i = 0; i < kIters; ++i) {
                if (storage.invoke(&original_add, i, 1) != i + 2) {
                    bad.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    EXPECT_EQ(bad.load(), 0);
}

} // namespace

TEST(HookStorage_RcuWriteonce, basic_store_invoke_replace) {
    exercise_basic<splice::policy::rcu_writeonce>();
}

TEST(HookStorage_SharedMutex, basic_store_invoke_replace) {
    exercise_basic<splice::policy::shared_mutex>();
}

TEST(HookStorage_RcuWriteonce, concurrent_readers_no_tearing) {
    exercise_concurrent_readers<splice::policy::rcu_writeonce>();
}

TEST(HookStorage_SharedMutex, concurrent_readers_no_tearing) {
    exercise_concurrent_readers<splice::policy::shared_mutex>();
}
