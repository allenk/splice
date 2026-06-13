// ─── Splice FR-010 — single-thread hook overhead ─────────────────────────
//
// Three curves on a noinline target:
//
//   BM_RawCall              — direct call, no hook installed (baseline)
//   BM_HookedCall_Active    — hook installed with a no-op trampoline-through callback
//   BM_HookedCall_Disabled  — hook installed then disable()'d (Tier 2 for inline)
//
// The first establishes the floor; the second is what FR-010's < 20 ns/call
// gate measures. The third sanity-checks that disable() really does
// restore native cost and isn't paying any residual indirection.
//
// Caveat: this build measures the *current* (pre-FR-010) implementation —
// recursive_mutex × std::function. The numbers it produces are the baseline
// against which the policy framework will be compared in FR-010 Step 3.
// ───────────────────────────────────────────────────────────────────────────
#include <benchmark/benchmark.h>
#include <splice/splice.h>

#include "bench_targets.h"

namespace {

// Capability detection lifted from the live-hook tests.
#if (defined(__aarch64__) || defined(__arm64__)) && \
    (defined(__linux__) || defined(__ANDROID__))
#   define SPLICE_BENCH_LIVE_HOOK 1
#elif defined(__x86_64__) || defined(_M_X64)
#   define SPLICE_BENCH_LIVE_HOOK 1
#else
#   define SPLICE_BENCH_LIVE_HOOK 0
#endif

// Process-static so the hook + trampoline are installed exactly once for
// the lifetime of the bench binary. Re-running individual benchmarks
// (Google Benchmark's --benchmark_repetitions) does not re-install.
struct HookFixture {
    HookFixture() {
#if SPLICE_BENCH_LIVE_HOOK
        SPLICE_HOOK_ADDR_STATIC(&splice::bench::hot_target)
            .onInvoke([](auto orig, int x) { return orig(x); });   // no-op passthrough
        splice::install_all();
        installed = splice_is_hooked(reinterpret_cast<void*>(&splice::bench::hot_target));
#endif
    }
    bool installed = false;
};

HookFixture& fixture() {
    static HookFixture f;
    return f;
}

} // namespace

// ─── BM_RawCall ──────────────────────────────────────────────────────────
// Direct call to the noinline target. Measures the floor.
static void BM_RawCall(benchmark::State& state) {
    int x = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(x = splice::bench::hot_target(x));
    }
}
BENCHMARK(BM_RawCall);

// ─── BM_HookedCall_Active ────────────────────────────────────────────────
// Same target, but with a passthrough hook installed.
// On platforms without a live patcher, this benchmark falls back to the
// raw call (the SPLICE_HOOK macro silently no-ops).
static void BM_HookedCall_Active(benchmark::State& state) {
    if (!fixture().installed) {
        state.SkipWithError("no live patcher on this platform — hook not installed");
        return;
    }
    int x = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(x = splice::bench::hot_target(x));
    }
}
BENCHMARK(BM_HookedCall_Active);
