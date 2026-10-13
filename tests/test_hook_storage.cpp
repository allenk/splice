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
//   - a null original is never called, whatever is stored (2026-10-13)
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
    }, /*needs_original=*/true);
    EXPECT_EQ(storage.invoke(&original_add, 2, 3), 50);

    // 3) Replace with a different override.
    storage.store([](int (*)(int, int), int a, int b) {
        return a - b;
    }, /*needs_original=*/false);
    EXPECT_EQ(storage.invoke(&original_add, 10, 3), 7);
}

template <typename Policy>
void exercise_concurrent_readers() {
    splice::HookStorage<Policy, int, int, int> storage;
    storage.store([](int (*orig)(int, int), int a, int b) {
        return orig(a, b) + 1;
    }, /*needs_original=*/true);

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

// ─── A null original is never called ──────────────────────────────────────
// Three ways to reach a hook with nothing safe to call, and before 2026-10-13
// all three called a null pointer. None of them involves a refusal: the slot is
// valid, the type is right, and the context is healthy.
template <typename Policy>
void exercise_null_original_is_refused() {
    splice::HookStorage<Policy, int, int, int> storage;

    // 1) Nothing stored and no original. This is a trampoline reached before
    //    anything was installed behind it, which the public API allows: the
    //    trampoline and its address are reachable. It used to be
    //    `return original(args...)` with original null.
    EXPECT_EQ(storage.invoke(nullptr, 2, 3), 0)
        << "a refused dispatch returns a value-initialised result, not a crash";

    // 2) A callback that needs the original, with no original. Every wrapper
    //    this library builds is of this kind.
    storage.store([](int (*orig)(int, int), int a, int b) {
        return orig(a, b) * 10;
    }, /*needs_original=*/true);
    EXPECT_EQ(storage.invoke(nullptr, 2, 3), 0);
    EXPECT_EQ(storage.invoke(&original_add, 2, 3), 50) << "and it still works with one";

    // 3) A callback explicitly allowed to work without one. This must still
    //    run: refusing it would refuse the case the review asked to preserve.
    storage.store([](int (*)(int, int), int a, int b) {
        return a * b;
    }, /*needs_original=*/false);
    EXPECT_EQ(storage.invoke(nullptr, 6, 7), 42)
        << "a callback that does not need the original runs without one";
}

// An empty std::function behind a non-null record is not a callback. Under
// rcu_writeonce the presence test used to be on the pointer, so this called an
// empty function -- which throws, inside a hook, in a library documented as
// usable with exceptions disabled.
template <typename Policy>
void exercise_empty_callable_is_not_a_callback() {
    splice::HookStorage<Policy, int, int, int> storage;
    using HookFn = typename splice::HookStorage<Policy, int, int, int>::HookFn;

    storage.store(HookFn{}, /*needs_original=*/false);

    // With an original: falls through to it rather than calling the empty one.
    EXPECT_EQ(storage.invoke(&original_add, 2, 3), 5);

    // Without one: refused, because an empty callable leaves the original as
    // the only thing to call and there is none.
    EXPECT_EQ(storage.invoke(nullptr, 2, 3), 0);
}

} // namespace

TEST(HookStorage_RcuWriteonce, a_null_original_is_never_called) {
    exercise_null_original_is_refused<splice::policy::rcu_writeonce>();
}

TEST(HookStorage_SharedMutex, a_null_original_is_never_called) {
    exercise_null_original_is_refused<splice::policy::shared_mutex>();
}

TEST(HookStorage_RcuWriteonce, an_empty_callable_is_not_a_callback) {
    exercise_empty_callable_is_not_a_callback<splice::policy::rcu_writeonce>();
}

TEST(HookStorage_SharedMutex, an_empty_callable_is_not_a_callback) {
    exercise_empty_callable_is_not_a_callback<splice::policy::shared_mutex>();
}

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
