// ─── Splice benchmark targets ─────────────────────────────────────────────
//
// Compiled as a separate TU so the compiler cannot inline these through to
// the benchmark loop. Equivalent to tests/helpers/test_targets.h but kept
// distinct because the bench binaries don't link against the test helpers
// (different CMake target, different include scope).
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#if defined(__GNUC__) || defined(__clang__)
#   define SPLICE_BENCH_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#   define SPLICE_BENCH_NOINLINE __declspec(noinline)
#else
#   define SPLICE_BENCH_NOINLINE
#endif

namespace splice::bench {

// Trivial integer target — minimum work per call so the hook overhead
// dominates the measurement.
SPLICE_BENCH_NOINLINE int hot_target(int x);

} // namespace splice::bench
