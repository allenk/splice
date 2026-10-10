// ─── Splice Phase 0 smoke test ─────────────────────────────────────────────
//
// Minimum bar: the umbrella header includes, the logging substrate compiles,
// and the runtime level switch actually toggles. Nothing here hooks anything
// yet — real hooking lands in Phase 1.
// ───────────────────────────────────────────────────────────────────────────

#define SPLICE_LOG_TAG "splice_test"

#include <gtest/gtest.h>
#include <splice/splice.h>

#include <array>
#include <cstdint>

TEST(SmokeTest, umbrella_header_compiles) {
    // If this translation unit linked, <splice/splice.h> is self-contained.
    //
    // The three constants are the library's own answer to "which splice is
    // this?". The build system's answer reaches here by a different route: the
    // top-level CMakeLists.txt parses the same header before project() and
    // derives PROJECT_VERSION, which tests/CMakeLists.txt passes in as these
    // macros. So the comparison is not a tautology -- it fails when the
    // compiler and the parse read different headers, which is what a stale
    // installed copy or a mixed include path produces.
    //
    // This assertion used to be EXPECT_GE(kVersionMinor, 0), relaxed so that a
    // version bump could not break it. It could not fail for any version
    // either, and in the four months it stood that way a release shipped
    // headers reporting the previous version. An assertion no input can fail
    // is not a detector.
    EXPECT_EQ(splice::kVersionMajor, SPLICE_TEST_VERSION_MAJOR);
    EXPECT_EQ(splice::kVersionMinor, SPLICE_TEST_VERSION_MINOR);
    EXPECT_EQ(splice::kVersionPatch, SPLICE_TEST_VERSION_PATCH);
}

TEST(SmokeTest, log_macros_emit_without_crashing) {
    // Exercise each level. Output goes to console backend with timestamp + PID/TID.
    SPLICE_LOGV("verbose — should not crash");
    SPLICE_LOGD("debug — v=%d", 42);
    SPLICE_LOGI("info — hello from %s", "splice");
    SPLICE_LOGW("warn — code=0x%x", 0xCAFEu);
    SPLICE_LOGE("error — path should still be reached");
    SUCCEED();
}

TEST(SmokeTest, hot_path_variants_throttle) {
    // 1000 calls × EVERY_N(100) should emit roughly 10 lines. The test only
    // verifies it does not crash or throw — exact count is backend-dependent.
    for (int i = 0; i < 1000; ++i) {
        SPLICE_LOGD_EVERY_N(100, "throttled i=%d", i);
    }
    // ONCE fires exactly once regardless of invocation count.
    for (int i = 0; i < 50; ++i) {
        SPLICE_LOGI_ONCE("first call only, i=%d", i);
    }
    // FIRST_N fires only the first N times.
    for (int i = 0; i < 50; ++i) {
        SPLICE_LOGW_FIRST_N(3, "burst-limited i=%d", i);
    }
    SUCCEED();
}

TEST(SmokeTest, hex_dump_safe_on_small_buffer) {
    constexpr std::array<std::uint8_t, 16> bytes{
        0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
        0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28,
    };
    SPLICE_LOG_HEX(bytes.data(), bytes.size());
    SUCCEED();
}

TEST(SmokeTest, hex_dump_safe_on_null_or_empty) {
    SPLICE_LOG_HEX(nullptr, 0);
    SPLICE_LOG_HEX(nullptr, 16);
    std::uint8_t dummy = 0;
    SPLICE_LOG_HEX(&dummy, 0);
    SUCCEED();
}

TEST(SmokeTest, runtime_level_toggles) {
    using splice::log::Level;
    const auto original = splice::log::get_level();

    splice::log::set_level(Level::Warn);
    EXPECT_EQ(splice::log::get_level(), Level::Warn);
    EXPECT_FALSE(splice::log::is_enabled(Level::Debug));
    EXPECT_TRUE (splice::log::is_enabled(Level::Warn));
    EXPECT_TRUE (splice::log::is_enabled(Level::Error));

    splice::log::set_level(Level::Verbose);
    EXPECT_TRUE(splice::log::is_enabled(Level::Debug));

    splice::log::set_level(Level::Silent);
    EXPECT_FALSE(splice::log::is_enabled(Level::Error));

    splice::log::set_level(original);
}

TEST(SmokeTest, breadcrumb_compiles) {
    SPLICE_LOG_HERE();
    SUCCEED();
}
