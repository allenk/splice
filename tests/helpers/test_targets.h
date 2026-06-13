// ─── Splice test targets ──────────────────────────────────────────────────
//
// Functions that integration tests will hook. Defined in a **separate TU**
// (test_targets.cpp) compiled with -fno-inline / __declspec(noinline) to
// prevent LTO from eliminating the symbols we want to intercept.
//
// This header is included by both the test TU and the target TU.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

// ─── noinline portability macro ─────────────────────────────────────────────
#if defined(__GNUC__) || defined(__clang__)
#   define SPLICE_TEST_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#   define SPLICE_TEST_NOINLINE __declspec(noinline)
#else
#   define SPLICE_TEST_NOINLINE
#endif

// Force 16-byte function-entry alignment so the x86_64 atomic install path
// (aligned 8-byte qword store) is exercised. MSVC already 16-byte-aligns
// functions; gcc/clang pack tightly at -O0, which can leave a target at an
// unaligned address and silently demote installs to the non-atomic memcpy
// fallback — fatal for the concurrent stress test, which must hammer the
// ATOMIC path it is named for.
#if defined(__GNUC__) || defined(__clang__)
#   define SPLICE_TEST_ALIGN16 __attribute__((aligned(16)))
#else
#   define SPLICE_TEST_ALIGN16
#endif

namespace splice::test {

// Simple arithmetic targets — easy to verify return values.
SPLICE_TEST_NOINLINE int add_one(int x);
SPLICE_TEST_NOINLINE int multiply_two(int x);
SPLICE_TEST_NOINLINE double add_half(double x);
SPLICE_TEST_NOINLINE void side_effect(int* counter);

// Multi-arg target (tests variadic trampoline path).
SPLICE_TEST_NOINLINE int sum_five(int a, int b, int c, int d, int e);

// Dedicated stress-test target (Phase 4.5d) — used by LiveHookStress only,
// so cross-test installer-queue replay can't double-patch a function that
// is the simultaneous target of LiveHook. Distinct symbol → distinct
// IdRegistry slot → distinct InterceptorEntry.
SPLICE_TEST_NOINLINE SPLICE_TEST_ALIGN16 int stress_target(int x);

// Dedicated PolicyOverride targets (FR-010 Step 4). Same reasoning as
// stress_target: _STATIC entries persist for program lifetime, so reusing
// add_one / multiply_two collides with LiveHook / InlineDisable when those
// tests run before PolicyOverride. Two symbols → two independent slots.
SPLICE_TEST_NOINLINE int policy_target_a(int x);
SPLICE_TEST_NOINLINE int policy_target_b(int x);

} // namespace splice::test
