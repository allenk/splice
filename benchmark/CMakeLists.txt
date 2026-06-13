# ─── Splice FR-010 microbenchmarks ───────────────────────────────────────────
# Built only when -DSPLICE_BUILD_BENCHMARKS=ON. Top-level CMakeLists has
# already done find_package(benchmark CONFIG REQUIRED).
#
# All bench binaries live under out/build/<preset>/benchmark/. They are NOT
# registered with CTest — microbenchmarks must run uncontested on a quiet
# machine to give meaningful numbers.
# ─────────────────────────────────────────────────────────────────────────────

# Shared noinline targets for the bench binaries — separate TU so the
# compiler can't inline through them. Uses the same noinline strategy as
# tests/helpers/test_targets.h.
add_library(splice_bench_targets STATIC bench_targets.cpp)
target_include_directories(splice_bench_targets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ─── Single-thread overhead ──────────────────────────────────────────────────
add_executable(bench_hook_overhead bench_hook_overhead.cpp)
target_link_libraries(bench_hook_overhead PRIVATE
    splice::splice
    splice_bench_targets
    benchmark::benchmark
    benchmark::benchmark_main
)

# ─── 8-thread reader contention ──────────────────────────────────────────────
add_executable(bench_hook_contended bench_hook_contended.cpp)
target_link_libraries(bench_hook_contended PRIVATE
    splice::splice
    splice_bench_targets
    benchmark::benchmark
    benchmark::benchmark_main
)

# ─── Step 5 validation: std::function vs thunk callback storage ──────────────
# Standalone microbench — does NOT use Splice's HookStorage. Compares three
# local mocks (direct call / std::function / thunk) to validate the Step 5
# refactor hypothesis before committing.
add_executable(bench_callback_storage bench_callback_storage.cpp)
target_link_libraries(bench_callback_storage PRIVATE
    splice_bench_targets
    benchmark::benchmark
)

# ─── Step 6 validation: registry-lookup isolation under contention ───────────
# Directly drives HookRegistry / OriginalsRegistry of BOTH impls in the same
# binary so we can A/B compare without rebuilding. Bypasses the trampoline
# entirely.
add_executable(bench_registry_lookup bench_registry_lookup.cpp)
target_link_libraries(bench_registry_lookup PRIVATE
    splice::splice
    benchmark::benchmark
)
