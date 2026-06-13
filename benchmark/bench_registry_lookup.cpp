// ─── Splice FR-010 Step 6.3 — registry-lookup isolation microbench ────────
//
// Surgically isolates the registry hot-path:
//
//   BM_HookRegistry_Get / threads:1..8
//   BM_OriginalsRegistry_Get / threads:1..8
//
// Each thread does pure get_or_create<DummyHook>(id=0) (or get(0) for
// originals), no trampoline, no install, no live patching. Lets us see
// whether the registry-impl change actually removes the cache-line
// bouncing under contention, independent of all other Splice machinery.
//
// Build BOTH variants:
//   cmake --preset=windows-x64-bench && build target bench_registry_lookup
//   cmake -B out/build/windows-x64-bench-rcu -DSPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array
//   build same target
//
// Compare same threads:N rows between the two builds. The default
// shared_mutex_map should bounce under contention; rcu_atomic_array
// should stay flat.
// ───────────────────────────────────────────────────────────────────────────
#include <benchmark/benchmark.h>

#include <splice/context.h>
#include <splice/registry_impl.h>

namespace {

struct DummyHook : splice::HookBase {
    int payload = 0;
};

// One process-wide registry of each kind so all threads share it.
splice::HookRegistry<splice::registry::shared_mutex_map>& hooks_sm() {
    static splice::HookRegistry<splice::registry::shared_mutex_map> r;
    static bool init = [&]() { r.template get_or_create<DummyHook>(0); return true; }();
    (void)init;
    return r;
}
splice::HookRegistry<splice::registry::rcu_atomic_array>& hooks_rcu() {
    static splice::HookRegistry<splice::registry::rcu_atomic_array> r;
    static bool init = [&]() { r.template get_or_create<DummyHook>(0); return true; }();
    (void)init;
    return r;
}
splice::OriginalsRegistry<splice::registry::shared_mutex_map>& orig_sm() {
    static splice::OriginalsRegistry<splice::registry::shared_mutex_map> r;
    static bool init = [&]() { r.set(0, reinterpret_cast<void*>(0xCAFEBABEull)); return true; }();
    (void)init;
    return r;
}
splice::OriginalsRegistry<splice::registry::rcu_atomic_array>& orig_rcu() {
    static splice::OriginalsRegistry<splice::registry::rcu_atomic_array> r;
    static bool init = [&]() { r.set(0, reinterpret_cast<void*>(0xCAFEBABEull)); return true; }();
    (void)init;
    return r;
}

} // namespace

// ─── Hook registry lookup (the FR-010 Step 6 critical path) ──────────────
static void BM_HookRegistry_Get_SharedMutex(benchmark::State& state) {
    auto& r = hooks_sm();
    DummyHook* h = nullptr;
    for (auto _ : state) {
        h = &r.template get_or_create<DummyHook>(0);
        benchmark::DoNotOptimize(h);
    }
}
BENCHMARK(BM_HookRegistry_Get_SharedMutex)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

static void BM_HookRegistry_Get_RcuArray(benchmark::State& state) {
    auto& r = hooks_rcu();
    DummyHook* h = nullptr;
    for (auto _ : state) {
        h = &r.template get_or_create<DummyHook>(0);
        benchmark::DoNotOptimize(h);
    }
}
BENCHMARK(BM_HookRegistry_Get_RcuArray)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// ─── Originals registry lookup ───────────────────────────────────────────
static void BM_OriginalsRegistry_Get_SharedMutex(benchmark::State& state) {
    auto& r = orig_sm();
    void* p = nullptr;
    for (auto _ : state) {
        p = r.get(0);
        benchmark::DoNotOptimize(p);
    }
}
BENCHMARK(BM_OriginalsRegistry_Get_SharedMutex)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

static void BM_OriginalsRegistry_Get_RcuArray(benchmark::State& state) {
    auto& r = orig_rcu();
    void* p = nullptr;
    for (auto _ : state) {
        p = r.get(0);
        benchmark::DoNotOptimize(p);
    }
}
BENCHMARK(BM_OriginalsRegistry_Get_RcuArray)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

BENCHMARK_MAIN();
