// ─── Splice FR-010 Step 5 — callback storage microbench ──────────────────
//
// Standalone validation of the "std::function vs thunk" hypothesis before
// committing to the Step 5 refactor. Does NOT use Splice's HookStorage at
// all — three local mock implementations live in this TU, mirroring the
// rcu_writeonce hot path:
//
//   1. BM_DirectCall      — `orig(x) + 1`, no indirection. Floor.
//   2. BM_StdFunctionPath — std::atomic<std::function*> load + invoke.
//                           Mirrors current HookStorage<rcu_writeonce>.
//   3. BM_ThunkPath       — std::atomic<thunk> + std::atomic<void*>
//                           load + invoke. Mirrors Step 5 proposal.
//
// Same lambda body in all three. Same target. Same volatile barriers via
// benchmark::DoNotOptimize. The delta is purely callback-dispatch cost.
//
// Build:
//   cmake --preset=windows-x64-bench
//   cmake --build --preset=windows-x64-bench --target bench_callback_storage
//   out/build/windows-x64-bench/benchmark/bench_callback_storage.exe
//
// Cross-compile for ARM64:
//   cmake --preset=android-arm64-bench   (add preset if missing)
//   adb push ... /data/local/tmp/ && adb shell .../bench_callback_storage
// ───────────────────────────────────────────────────────────────────────────
#include <benchmark/benchmark.h>

#include "bench_targets.h"

#include <atomic>
#include <functional>
#include <utility>

namespace {

// ─── Shared callback body ────────────────────────────────────────────────
// Use the same lambda everywhere so the only difference between curves is
// how the lambda is stored & dispatched.
using OrigFn = int (*)(int);

// Captureless body — small, predictable, doesn't dominate the measurement.
constexpr auto callback_body = [](OrigFn orig, int x) noexcept {
    return orig(x) + 1;
};

// ─── Mock 1: std::function path (mirrors current HookStorage) ─────────────
struct StdFunctionStorage {
    using HookFn = std::function<int(OrigFn, int)>;
    std::atomic<HookFn*> m_fn{nullptr};

    template <typename Lambda>
    void store(Lambda fn) {
        m_fn.store(new HookFn(std::move(fn)), std::memory_order_release);
    }

    int invoke(OrigFn orig, int x) noexcept {
        if (auto* fn = m_fn.load(std::memory_order_acquire)) {
            return (*fn)(orig, x);
        }
        return orig(x);
    }
};

// ─── Mock 2: thunk path (mirrors Step 5 proposal) ─────────────────────────
struct ThunkStorage {
    using Thunk = int (*)(void* state, OrigFn orig, int x);

    std::atomic<Thunk> m_thunk{nullptr};
    std::atomic<void*> m_state{nullptr};

    template <typename Lambda>
    void store(Lambda fn) {
        // Per (Lambda type) storage — write-once leak, matches
        // rcu_writeonce semantics.
        static Lambda* s_fn = new Lambda(std::move(fn));
        m_state.store(s_fn, std::memory_order_relaxed);
        m_thunk.store(
            +[](void* st, OrigFn orig, int x) noexcept -> int {
                return (*static_cast<Lambda*>(st))(orig, x);
            },
            std::memory_order_release);
    }

    int invoke(OrigFn orig, int x) noexcept {
        if (auto th = m_thunk.load(std::memory_order_acquire)) {
            return th(m_state.load(std::memory_order_relaxed), orig, x);
        }
        return orig(x);
    }
};

// ─── Fixtures — install callbacks exactly once at process startup ─────────
StdFunctionStorage& std_storage() {
    static StdFunctionStorage s;
    static bool inited = []() {
        s.store(callback_body);
        return true;
    }();
    (void)inited;
    return s;
}

ThunkStorage& thunk_storage() {
    static ThunkStorage s;
    static bool inited = []() {
        s.store(callback_body);
        return true;
    }();
    (void)inited;
    return s;
}

} // namespace

// ─── BM_DirectCall ──────────────────────────────────────────────────────
// Floor: no storage indirection, just `orig(x) + 1`.
static void BM_DirectCall(benchmark::State& state) {
    int x = 0;
    for (auto _ : state) {
        x = splice::bench::hot_target(x) + 1;
        benchmark::DoNotOptimize(x);
    }
}
BENCHMARK(BM_DirectCall);

// ─── BM_StdFunctionPath ─────────────────────────────────────────────────
// Mirrors current HookStorage<rcu_writeonce>::invoke().
static void BM_StdFunctionPath(benchmark::State& state) {
    auto& s = std_storage();
    int x = 0;
    for (auto _ : state) {
        x = s.invoke(&splice::bench::hot_target, x);
        benchmark::DoNotOptimize(x);
    }
}
BENCHMARK(BM_StdFunctionPath);

// ─── BM_ThunkPath ───────────────────────────────────────────────────────
// Mirrors proposed Step 5 HookStorage<rcu_writeonce>::invoke().
static void BM_ThunkPath(benchmark::State& state) {
    auto& s = thunk_storage();
    int x = 0;
    for (auto _ : state) {
        x = s.invoke(&splice::bench::hot_target, x);
        benchmark::DoNotOptimize(x);
    }
}
BENCHMARK(BM_ThunkPath);

BENCHMARK_MAIN();
