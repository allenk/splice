// ─── FR-009 Step 9.1 — .before() / .after() modifier tests ────────────────
//
// Exercises the new `set_before` / `set_after` paths on HookAs without
// going through the live patcher. We drive the trampoline directly (same
// pattern as test_trampoline.cpp) so the tests run on every platform
// regardless of the arch backend.
//
// Both modifiers are pure sugar over set_invoke — they wrap the user
// lambda into a HookFn that brackets the original call. Validates:
//   - .before fires before original, sees args, original return is from orig
//   - .after fires after original, sees return value + args
//   - .after for void Ret takes (Args...), not (void, Args...)
//   - Mutual exclusion: a later .onInvoke replaces a prior .before/.after
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/context.h>
#include <splice/core.h>
#include <splice/registry.h>
#include <splice/trampoline.h>

#include <atomic>
#include <string>

namespace {

int double_it(int x) { return x * 2; }
void void_sink(int*) {}

template <typename Gen>
int slot_of() {
    return splice::default_context().slot_for(Gen::get_trampoline_ptr());
}

} // namespace

// ─── Test 1 — .before fires, original return unchanged ──────────────────
TEST(HookModifiers, before_fires_before_original_unchanged_return) {
    using Gen = splice::TrampolineGenerator<int (*)(int), 70001>;
    const int slot = slot_of<Gen>();

    splice::OriginalRegistry::set_original<int (*)(int)>(slot, &double_it);

    std::atomic<int> before_count{0};
    std::atomic<int> last_arg{0};
    splice::HookManager::get_hook<int, int>(slot).set_before(
        [&](int x) {
            before_count.fetch_add(1, std::memory_order_relaxed);
            last_arg.store(x, std::memory_order_relaxed);
        });

    auto* tramp = reinterpret_cast<int (*)(int)>(Gen::get_trampoline_ptr());

    EXPECT_EQ(tramp(7), 14);                    // original behaviour preserved
    EXPECT_EQ(before_count.load(), 1);          // before fired exactly once
    EXPECT_EQ(last_arg.load(), 7);              // saw the actual arg
}

// ─── Test 2 — .after fires with return value ────────────────────────────
TEST(HookModifiers, after_fires_after_original_with_return_value) {
    using Gen = splice::TrampolineGenerator<int (*)(int), 70002>;
    const int slot = slot_of<Gen>();

    splice::OriginalRegistry::set_original<int (*)(int)>(slot, &double_it);

    std::atomic<int> after_count{0};
    std::atomic<int> last_ret{0};
    std::atomic<int> last_arg{0};
    splice::HookManager::get_hook<int, int>(slot).set_after(
        [&](int ret, int x) {
            after_count.fetch_add(1, std::memory_order_relaxed);
            last_ret.store(ret, std::memory_order_relaxed);
            last_arg.store(x, std::memory_order_relaxed);
        });

    auto* tramp = reinterpret_cast<int (*)(int)>(Gen::get_trampoline_ptr());

    EXPECT_EQ(tramp(5), 10);                    // original return passes through
    EXPECT_EQ(after_count.load(), 1);
    EXPECT_EQ(last_ret.load(), 10);             // after saw the return
    EXPECT_EQ(last_arg.load(), 5);              // after saw the arg
}

// ─── Test 3 — .after for void return takes (Args...) only ───────────────
TEST(HookModifiers, after_on_void_return_takes_args_only) {
    using Gen = splice::TrampolineGenerator<void (*)(int*), 70003>;
    const int slot = slot_of<Gen>();

    splice::OriginalRegistry::set_original<void (*)(int*)>(slot, &void_sink);

    std::atomic<int> after_count{0};
    int* observed_arg = nullptr;
    splice::HookManager::get_hook<void, int*>(slot).set_after(
        [&](int* p) {
            after_count.fetch_add(1, std::memory_order_relaxed);
            observed_arg = p;
        });

    auto* tramp = reinterpret_cast<void (*)(int*)>(Gen::get_trampoline_ptr());
    int local = 42;
    tramp(&local);

    EXPECT_EQ(after_count.load(), 1);
    EXPECT_EQ(observed_arg, &local);
}

// ─── Test 4 — .onInvoke after .before overwrites (mutual exclusion) ─────
TEST(HookModifiers, onInvoke_after_before_overwrites) {
    using Gen = splice::TrampolineGenerator<int (*)(int), 70004>;
    const int slot = slot_of<Gen>();

    splice::OriginalRegistry::set_original<int (*)(int)>(slot, &double_it);

    std::atomic<int> before_count{0};
    splice::HookManager::get_hook<int, int>(slot).set_before(
        [&](int) { before_count.fetch_add(1, std::memory_order_relaxed); });

    // Now set onInvoke — should completely replace the before-wrapper.
    std::atomic<int> invoke_count{0};
    splice::HookManager::get_hook<int, int>(slot).set_invoke(
        [&](int (*)(int), int x) {
            invoke_count.fetch_add(1, std::memory_order_relaxed);
            return x + 100;                     // replaces original entirely
        });

    auto* tramp = reinterpret_cast<int (*)(int)>(Gen::get_trampoline_ptr());

    EXPECT_EQ(tramp(3), 103);                   // onInvoke ran, not original
    EXPECT_EQ(invoke_count.load(), 1);
    EXPECT_EQ(before_count.load(), 0);          // before was overwritten
}

// ─── FR-009 Step 9.2 — composable gates (.when / .once / .times) ────────
//
// Drive the gates through InterceptorEntry directly (no live patching). The
// trampoline still goes through HookManager::get_hook<>, but the gates are
// applied at .onInvoke / .before / .after commit time inside InterceptorEntry.
// We construct entries manually with a fake trampoline_ptr — the slot is
// fresh per test and the trampoline never executes.

// InterceptorEntry is non-copyable + non-movable. Each test constructs
// one in-place — verbose but compiler-bullet-proof (MSVC's preprocessor
// chokes on a multi-line macro that expands to a templated declaration).

// ─── Test 5 — .when(false) gates the callback out ────────────────────────
TEST(HookModifiers, when_false_skips_callback) {
    using Gen = splice::TrampolineGenerator<int (*)(int), 70010>;
    splice::InterceptorEntry<int (*)(int)> entry(
        nullptr, reinterpret_cast<void*>(&double_it), "test",
        70010, Gen::get_trampoline_ptr());
    const int slot = slot_of<Gen>();
    splice::OriginalRegistry::set_original<int (*)(int)>(slot, &double_it);

    std::atomic<int> hits{0};
    std::atomic<bool> gate_open{false};
    entry.when([&]{ return gate_open.load(); })
         .before([&](int) { hits.fetch_add(1, std::memory_order_relaxed); });

    auto* tramp = reinterpret_cast<int (*)(int)>(Gen::get_trampoline_ptr());

    EXPECT_EQ(tramp(5), 10);                    // orig still runs
    EXPECT_EQ(hits.load(), 0);                  // callback skipped (gate false)

    gate_open.store(true);
    EXPECT_EQ(tramp(5), 10);                    // orig still runs
    EXPECT_EQ(hits.load(), 1);                  // callback now fired
}

// ─── Test 6 — .once() fires exactly once then transparent ───────────────
TEST(HookModifiers, once_fires_exactly_once) {
    using Gen = splice::TrampolineGenerator<int (*)(int), 70011>;
    splice::InterceptorEntry<int (*)(int)> entry(
        nullptr, reinterpret_cast<void*>(&double_it), "test",
        70011, Gen::get_trampoline_ptr());
    const int slot = slot_of<Gen>();
    splice::OriginalRegistry::set_original<int (*)(int)>(slot, &double_it);

    std::atomic<int> hits{0};
    entry.once().before([&](int) {
        hits.fetch_add(1, std::memory_order_relaxed);
    });

    auto* tramp = reinterpret_cast<int (*)(int)>(Gen::get_trampoline_ptr());

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(tramp(7), 14);                // orig always runs
    }
    EXPECT_EQ(hits.load(), 1);                  // callback fired exactly once
}

// ─── Test 7 — .times(n) fires n times then transparent ──────────────────
TEST(HookModifiers, times_n_fires_n_times) {
    using Gen = splice::TrampolineGenerator<int (*)(int), 70012>;
    splice::InterceptorEntry<int (*)(int)> entry(
        nullptr, reinterpret_cast<void*>(&double_it), "test",
        70012, Gen::get_trampoline_ptr());
    const int slot = slot_of<Gen>();
    splice::OriginalRegistry::set_original<int (*)(int)>(slot, &double_it);

    std::atomic<int> hits{0};
    entry.times(3).before([&](int) {
        hits.fetch_add(1, std::memory_order_relaxed);
    });

    auto* tramp = reinterpret_cast<int (*)(int)>(Gen::get_trampoline_ptr());

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(tramp(2), 4);
    }
    EXPECT_EQ(hits.load(), 3);                  // exactly 3 callback fires
}

// ─── Test 8 — order independence: .when().once() == .once().when() ──────
TEST(HookModifiers, gate_order_is_independent) {
    using GenA = splice::TrampolineGenerator<int (*)(int), 70013>;
    using GenB = splice::TrampolineGenerator<int (*)(int), 70014>;
    splice::InterceptorEntry<int (*)(int)> entry_a(
        nullptr, reinterpret_cast<void*>(&double_it), "test_a",
        70013, GenA::get_trampoline_ptr());
    splice::InterceptorEntry<int (*)(int)> entry_b(
        nullptr, reinterpret_cast<void*>(&double_it), "test_b",
        70014, GenB::get_trampoline_ptr());
    const int slot_a = slot_of<GenA>();
    const int slot_b = slot_of<GenB>();
    splice::OriginalRegistry::set_original<int (*)(int)>(slot_a, &double_it);
    splice::OriginalRegistry::set_original<int (*)(int)>(slot_b, &double_it);

    std::atomic<bool> gate{true};
    std::atomic<int> hits_a{0}, hits_b{0};

    // Permutation A: .when().once()
    entry_a.when([&]{ return gate.load(); }).once()
           .before([&](int) { hits_a.fetch_add(1, std::memory_order_relaxed); });

    // Permutation B: .once().when() — should produce identical behaviour
    entry_b.once().when([&]{ return gate.load(); })
           .before([&](int) { hits_b.fetch_add(1, std::memory_order_relaxed); });

    auto* ta = reinterpret_cast<int (*)(int)>(GenA::get_trampoline_ptr());
    auto* tb = reinterpret_cast<int (*)(int)>(GenB::get_trampoline_ptr());

    // Both call 5 times with gate open — both should fire exactly once.
    for (int i = 0; i < 5; ++i) {
        ta(1);
        tb(1);
    }
    EXPECT_EQ(hits_a.load(), 1);
    EXPECT_EQ(hits_b.load(), 1);

    // Both call more times — neither should fire (once budget exhausted).
    for (int i = 0; i < 5; ++i) {
        ta(1);
        tb(1);
    }
    EXPECT_EQ(hits_a.load(), 1);
    EXPECT_EQ(hits_b.load(), 1);
}

// ─── Test 9 — .when() composes with .onInvoke (callback can replace orig) ─
TEST(HookModifiers, when_with_onInvoke_full_replacement) {
    using Gen = splice::TrampolineGenerator<int (*)(int), 70015>;
    splice::InterceptorEntry<int (*)(int)> entry(
        nullptr, reinterpret_cast<void*>(&double_it), "test",
        70015, Gen::get_trampoline_ptr());
    const int slot = slot_of<Gen>();
    splice::OriginalRegistry::set_original<int (*)(int)>(slot, &double_it);

    std::atomic<bool> capture{false};
    entry.when([&]{ return capture.load(); })
         .onInvoke([](int (*)(int), int x) { return x + 1000; });

    auto* tramp = reinterpret_cast<int (*)(int)>(Gen::get_trampoline_ptr());

    EXPECT_EQ(tramp(5), 10);                    // gate false → original (5*2)
    capture.store(true);
    EXPECT_EQ(tramp(5), 1005);                  // gate true  → onInvoke (5+1000)
}

// ─── Test 10 — gates reset between action verb commits ──────────────────
// .once().before(fn1) should consume the once gate; a SECOND .before(fn2)
// without re-setting .once() should fire unbounded.
TEST(HookModifiers, gates_consumed_per_action_verb) {
    using Gen = splice::TrampolineGenerator<int (*)(int), 70016>;
    splice::InterceptorEntry<int (*)(int)> entry(
        nullptr, reinterpret_cast<void*>(&double_it), "test",
        70016, Gen::get_trampoline_ptr());
    const int slot = slot_of<Gen>();
    splice::OriginalRegistry::set_original<int (*)(int)>(slot, &double_it);

    std::atomic<int> hits{0};
    entry.once().before([&](int) {
        hits.fetch_add(1, std::memory_order_relaxed);
    });

    // Overwrite with a fresh .before — no .once() this time. Should fire
    // every call.
    entry.before([&](int) {
        hits.fetch_add(1, std::memory_order_relaxed);
    });

    auto* tramp = reinterpret_cast<int (*)(int)>(Gen::get_trampoline_ptr());

    for (int i = 0; i < 5; ++i) tramp(1);
    EXPECT_EQ(hits.load(), 5);                  // second .before fires every call
}
