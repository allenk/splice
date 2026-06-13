// ─── Splice test targets — implementation ─────────────────────────────────
//
// Separate TU so these functions have real, hookable prologues. The
// compiler cannot inline them from a different translation unit (especially
// with SPLICE_TEST_NOINLINE), so the function addresses that integration
// tests pass to SPLICE_HOOK_ADDR are guaranteed to be valid, patched-in-
// place targets.
// ───────────────────────────────────────────────────────────────────────────
#include "test_targets.h"

namespace splice::test {

SPLICE_TEST_NOINLINE int add_one(int x) { return x + 1; }
SPLICE_TEST_NOINLINE int multiply_two(int x) { return x * 2; }
SPLICE_TEST_NOINLINE double add_half(double x) { return x + 0.5; }
SPLICE_TEST_NOINLINE void side_effect(int* counter) { ++(*counter); }
SPLICE_TEST_NOINLINE int sum_five(int a, int b, int c, int d, int e) {
    return a + b + c + d + e;
}
SPLICE_TEST_NOINLINE SPLICE_TEST_ALIGN16 int stress_target(int x) { return x + 1; }
SPLICE_TEST_NOINLINE int policy_target_a(int x) { return x + 1; }
SPLICE_TEST_NOINLINE int policy_target_b(int x) { return x * 2; }

} // namespace splice::test
