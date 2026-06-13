// ─── Inline-disable round-trip test (Phase 4.5c-2) ────────────────────────
//
// FR-013 Tier 2 disable validation. The IAT round-trip in
// test_hook_iat.cpp covers Tier 1 (POINTER_SWAP). This file exercises
// the harder Tier 2 (INLINE) path:
//
//   install hook  →  prologue overwritten with E9 rel32 (x86_64) /
//                    16-byte indirect branch (ARM64), original 16 bytes
//                    snapshot into splice_patch_record
//   call          →  hook fires, returns sentinel
//   disable()     →  arch atomic_disable_* replays saved bytes, restoring
//                    the original prologue under single-copy atomicity
//   call          →  original behaviour again, hook callback no longer fires
//
// Trampoline memory remains allocated forever per the documented FR-013
// limitation — we cannot prove no thread is currently inside it.
//
// Target choice: `splice::test::multiply_two` is defined in test_targets.cpp
// (separate TU, noinline). On Windows it has no IAT slot (it's defined IN
// the test binary, not imported), so engine.cpp's IAT probe falls through
// to the inline patcher — exactly the path we want to exercise here.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/splice.h>

#include "helpers/test_targets.h"

#include <atomic>

#if (defined(__aarch64__) || defined(__arm64__)) && \
    (defined(__linux__) || defined(__ANDROID__))
#   define SPLICE_INLINE_DISABLE_SUPPORTED 1
#elif defined(__x86_64__) || defined(_M_X64)
#   define SPLICE_INLINE_DISABLE_SUPPORTED 1
#else
#   define SPLICE_INLINE_DISABLE_SUPPORTED 0
#endif

#if SPLICE_INLINE_DISABLE_SUPPORTED

namespace {

std::atomic<int> g_inline_hits{0};

constexpr int kPreHookExpected  = 84;     // multiply_two(42)             unhooked
constexpr int kPostHookExpected = 1042;   // hooked: orig(42) + 1000 - 84

} // namespace

TEST(InlineDisable, install_invoke_disable_round_trip) {
    using splice::test::multiply_two;

    // ── Baseline: original behaviour before any hook ──────────────────────
    EXPECT_EQ(multiply_two(42), kPreHookExpected);

    g_inline_hits.store(0, std::memory_order_relaxed);

    // ── Install (forces inline path: no IAT slot for in-binary symbol) ────
    // STATIC storage + capture-less lambda — keeps the global installer
    // queue clean of dangling references (cf. test pollution / task #57).
    auto& entry = SPLICE_HOOK_ADDR_STATIC(&multiply_two)
        .onInvoke([](auto orig, int x) -> int {
            g_inline_hits.fetch_add(1, std::memory_order_relaxed);
            // Distinct sentinel: orig(x) + 1000 - x*2 = 1000 for x=42.
            // Using orig() proves the trampoline still works post-install.
            return orig(x) + (kPostHookExpected - kPreHookExpected);
        });
    splice::install_all();
    ASSERT_TRUE(entry.is_installed())
        << "inline install failed — engine returned null stub";

    // ── Hook is live: returns sentinel, hits counter increments ───────────
    EXPECT_EQ(multiply_two(42), kPostHookExpected);
    EXPECT_GT(g_inline_hits.load(), 0);

    // ── Disable (Tier 2): atomic restore of the patched prologue ──────────
    g_inline_hits.store(0, std::memory_order_relaxed);
    EXPECT_TRUE(entry.disable());
    EXPECT_FALSE(entry.is_installed());

    // ── Post-disable: original behaviour, hook no longer fires ────────────
    const int post_disable = multiply_two(42);
    EXPECT_EQ(post_disable, kPreHookExpected)
        << "After Tier 2 disable, multiply_two no longer returns the original";
    EXPECT_EQ(g_inline_hits.load(), 0)
        << "After Tier 2 disable, the hook callback was still invoked";
}

#else  // !SPLICE_INLINE_DISABLE_SUPPORTED

TEST(InlineDisable, disabled_on_unsupported_platform) {
    GTEST_SKIP() << "No live patcher on this platform — Tier 2 disable n/a";
}

#endif
