// ─── .replace_with() — when the caller already is the hook ─────────────────
//
// `.onInvoke()` asks splice to build the hook: a stub is claimed, the callback
// goes into the registry, and the original arrives as the callback's first
// argument. That is the right shape for almost everything.
//
// It is the wrong shape for the consumer this library was productised for. Its
// render-thread hook is already a `STDMETHODCALLTYPE` function, and it reads
// the function it displaced out of its own atomic with acquire, because that
// is the only load it is willing to do per frame. Wrapping it in a splice stub
// would add an indirection to the hottest path in the process and buy nothing.
//
// Until now the only way to express that was the three-call C ABI by hand --
// which works, and is exactly the crude packaging this redesign exists to
// replace. `.replace_with()` is the same transaction said in the library's own
// idiom: an action verb beside `.onInvoke()`, composing with
// `.expect_prologue()` and with the group.
//
// The interesting assertion is not that it installs. It is that a gate asked
// for alongside it is **refused rather than ignored**: splice is not in the
// call path with this verb, so `.when()` would never run, and a gate that
// silently did nothing is worse than a group that says why it will not
// install.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/live.h>

#include "test_targets.h"

#include <atomic>
#include <cstring>

namespace {

using splice::test::batch_target_a;
using splice::test::batch_target_b;

using TargetFn = int (*)(int);

constexpr int kArg    = 3;
constexpr int kPlainA = kArg * 7;      // 21
constexpr int kPlainB = kArg * 11;     // 33

// The consumer's shape: the replacement is a plain function and it loads the
// displaced original out of an atomic the caller owns.
std::atomic<void*> g_original_a{nullptr};
std::atomic<void*> g_original_b{nullptr};
std::atomic<int>   g_calls_a{0};
std::atomic<int>   g_calls_b{0};

int replacement_a(int x) {
    g_calls_a.fetch_add(1, std::memory_order_relaxed);
    auto* original = reinterpret_cast<TargetFn>(
        g_original_a.load(std::memory_order_acquire));
    return original ? original(x) + 1 : -1;
}

int replacement_b(int x) {
    g_calls_b.fetch_add(1, std::memory_order_relaxed);
    auto* original = reinterpret_cast<TargetFn>(
        g_original_b.load(std::memory_order_acquire));
    return original ? original(x) + 2 : -1;
}

class ReplaceWith : public ::testing::Test {
protected:
    void SetUp() override {
        if (!splice::live_tier_available()) {
            GTEST_SKIP() << "the coordinated tier needs an OS that can suspend "
                            "a thread and move its program counter";
        }
        g_original_a.store(nullptr);
        g_original_b.store(nullptr);
        g_calls_a.store(0);
        g_calls_b.store(0);
    }

    void TearDown() override {
        if (outcome.active()) {
            EXPECT_TRUE(outcome.disable());
        }
        EXPECT_EQ(batch_target_a(kArg), kPlainA) << "target A was left hooked";
        EXPECT_EQ(batch_target_b(kArg), kPlainB) << "target B was left hooked";
    }

    splice::Outcome outcome;
};

} // namespace

TEST_F(ReplaceWith, two_sites_with_the_callers_own_replacements) {
    splice::Live<> group;
    outcome = group
        .at(reinterpret_cast<void*>(&batch_target_a), TargetFn{})
            .replace_with(&replacement_a, g_original_a)
        .at(reinterpret_cast<void*>(&batch_target_b), TargetFn{})
            .replace_with(&replacement_b, g_original_b)
        .install(splice::live);

    ASSERT_EQ(outcome.state(), splice::State::installed)
        << "stage=" << static_cast<int>(outcome.failure().stage)
        << " op=" << static_cast<int>(outcome.failure().operation)
        << " site=" << outcome.failure().site.ordinal;
    EXPECT_EQ(outcome.installed_sites(), 2u);

    // Published before the commit, which is what makes the first call safe:
    // a thread that arrives the instant the patch lands must find the original
    // already there.
    EXPECT_NE(g_original_a.load(std::memory_order_acquire), nullptr);
    EXPECT_NE(g_original_b.load(std::memory_order_acquire), nullptr);

    EXPECT_EQ(batch_target_a(kArg), kPlainA + 1);
    EXPECT_EQ(batch_target_b(kArg), kPlainB + 2);
    EXPECT_EQ(g_calls_a.load(), 1);
    EXPECT_EQ(g_calls_b.load(), 1);

    ASSERT_TRUE(outcome.disable());
    const int settled_a = g_calls_a.load();
    EXPECT_EQ(batch_target_a(kArg), kPlainA);
    EXPECT_EQ(g_calls_a.load(), settled_a) << "the replacement still ran after disable";
}

TEST_F(ReplaceWith, the_published_original_is_the_function_and_not_the_thunk) {
    // `&batch_target_a` is an ILT entry under incremental linking. The group
    // patches the function behind it, so what gets published has to be a way
    // back into that function -- and the proof is that calling it produces the
    // unhooked answer while the hook is installed.
    splice::Live<> group;
    outcome = group
        .at(reinterpret_cast<void*>(&batch_target_a), TargetFn{})
            .replace_with(&replacement_a, g_original_a)
        .install(splice::live);
    ASSERT_EQ(outcome.state(), splice::State::installed);

    auto* original = reinterpret_cast<TargetFn>(
        g_original_a.load(std::memory_order_acquire));
    ASSERT_NE(original, nullptr);
    EXPECT_EQ(original(kArg), kPlainA)
        << "the published original did not reach the unhooked function";
    EXPECT_EQ(g_calls_a.load(), 0)
        << "calling the original went back through the replacement";
}

TEST_F(ReplaceWith, a_gate_asked_for_alongside_it_is_refused_not_ignored) {
    // The assertion that matters. With this verb the caller's function is in
    // the call path and splice's wrapper is not, so a gate could never run.
    // Refusing says so; ignoring would hand back a group whose `.once()` the
    // caller believes is in force.
    splice::Live<> group;
    const splice::Outcome refused = group
        .at(reinterpret_cast<void*>(&batch_target_a), TargetFn{})
            .once()
            .replace_with(&replacement_a, g_original_a)
        .install(splice::live);

    EXPECT_EQ(refused.state(), splice::State::refused);
    EXPECT_EQ(refused.installed_sites(), 0u);
    EXPECT_EQ(batch_target_a(kArg), kPlainA) << "a refused group wrote anyway";
    EXPECT_EQ(g_original_a.load(std::memory_order_acquire), nullptr)
        << "a refused group published an original";
}

TEST_F(ReplaceWith, it_composes_with_the_prologue_expectation) {
    // Both guards on one site, which is the shape the consumer's product path
    // needs: its own replacement, and a refusal if the target no longer begins
    // the way it was snapshotted.
    const auto* entry = reinterpret_cast<const unsigned char*>(&batch_target_a);
    const unsigned char* behind = entry;
    if (entry[0] == 0xE9) {
        std::int32_t rel = 0;
        std::memcpy(&rel, entry + 1, sizeof(rel));
        behind = entry + 5 + rel;
    }
    unsigned char wrong[SPLICE_LIVE_EXPECTED_BYTES]{};
    std::memcpy(wrong, behind, sizeof(wrong));
    wrong[0] = static_cast<unsigned char>(wrong[0] ^ 0xFFu);

    splice::Live<> group;
    const splice::Outcome refused = group
        .at(reinterpret_cast<void*>(&batch_target_a), TargetFn{})
            .expect_prologue(wrong)
            .replace_with(&replacement_a, g_original_a)
        .install(splice::live);

    EXPECT_EQ(refused.state(), splice::State::refused);
    EXPECT_EQ(refused.failure().stage, splice::Stage::prepare);
    EXPECT_EQ(batch_target_a(kArg), kPlainA);
    EXPECT_EQ(g_original_a.load(std::memory_order_acquire), nullptr);

    unsigned char right[SPLICE_LIVE_EXPECTED_BYTES]{};
    std::memcpy(right, behind, sizeof(right));
    splice::Live<> second;
    outcome = second
        .at(reinterpret_cast<void*>(&batch_target_a), TargetFn{})
            .expect_prologue(right)
            .replace_with(&replacement_a, g_original_a)
        .install(splice::live);
    ASSERT_EQ(outcome.state(), splice::State::installed);
    EXPECT_EQ(batch_target_a(kArg), kPlainA + 1);
}
