// ─── Splice FR-010 — multi-thread reader contention ──────────────────────
//
// Same hooked target as bench_hook_overhead, but driven by N threads
// (Google Benchmark's --benchmark_min_threads/--benchmark_max_threads, or
// the explicit Threads(N) attribute). Each thread independently runs the
// inner loop; Google Benchmark reports per-thread iteration time and the
// implied aggregate throughput.
//
// What we are *actually* measuring here is the cost of N threads
// simultaneously executing through the hook: Hook<>::invoke takes
// recursive_mutex (today), and HookContext::get_original takes another.
// Both turn into futex syscalls under contention. FR-010's < 5x gate
// requires this to come down to roughly 5x the single-thread cost from
// bench_hook_overhead. The current pre-policy build will be much worse —
// that's the point. We need the number to know how much better the
// rcu_writeonce policy needs to be.
// ───────────────────────────────────────────────────────────────────────────
#include <benchmark/benchmark.h>
#include <splice/splice.h>

#include "bench_targets.h"

namespace {

#if (defined(__aarch64__) || defined(__arm64__)) && \
    (defined(__linux__) || defined(__ANDROID__))
#   define SPLICE_BENCH_LIVE_HOOK 1
#elif defined(__x86_64__) || defined(_M_X64)
#   define SPLICE_BENCH_LIVE_HOOK 1
#else
#   define SPLICE_BENCH_LIVE_HOOK 0
#endif

struct ContendedHookFixture {
    ContendedHookFixture() {
#if SPLICE_BENCH_LIVE_HOOK
        SPLICE_HOOK_ADDR_STATIC(&splice::bench::hot_target)
            .onInvoke([](auto orig, int x) { return orig(x); });
        splice::install_all();
        installed = splice_is_hooked(reinterpret_cast<void*>(&splice::bench::hot_target));
#endif
    }
    bool installed = false;
};

ContendedHookFixture& fixture() {
    static ContendedHookFixture f;
    return f;
}

} // namespace

// ─── BM_RawCall_Contended ─────────────────────────────────────────────────
// Direct call across N threads, no hook. Establishes the contention floor
// (which should be ~zero — the noinline target has no shared state).
static void BM_RawCall_Contended(benchmark::State& state) {
    int x = state.thread_index();
    for (auto _ : state) {
        benchmark::DoNotOptimize(x = splice::bench::hot_target(x));
    }
}
BENCHMARK(BM_RawCall_Contended)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// ─── BM_HookedCall_Contended ──────────────────────────────────────────────
// The headline number: hooked dispatch under contention. This is what
// FR-010's < 5x gate is measured against.
static void BM_HookedCall_Contended(benchmark::State& state) {
    if (!fixture().installed) {
        state.SkipWithError("no live patcher on this platform — hook not installed");
        return;
    }
    int x = state.thread_index();
    for (auto _ : state) {
        benchmark::DoNotOptimize(x = splice::bench::hot_target(x));
    }
}
BENCHMARK(BM_HookedCall_Contended)->Threads(1)->Threads(2)->Threads(4)->Threads(8);
