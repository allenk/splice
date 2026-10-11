// ─── .expect_prologue() — the only check that notices a foreign patch ──────
//
// `.expect(address)` asks whether a vtable slot still holds the pointer the
// caller read. On the coordinated tier that question is not available: the tier
// patches the function, not the table, and a slot can be pristine while the
// code behind it has already been replaced by somebody else.
//
// The engine has always been able to answer the other question -- refuse unless
// the target's first bytes are the ones the caller believes -- and said so in
// the C ABI. The fluent layer could not reach it: `install(live_t)` passed a
// null expectation for every site. That gap was found by the consumer, whose
// product install path carries a sixteen-byte snapshot per site and relies on
// exactly this refusal.
//
// Two assertions, and the second is the one that makes the first mean
// something: a wrong expectation refuses and writes nothing, and a RIGHT
// expectation installs. A check that always refused would pass the first on
// its own.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/live.h>

#include "test_targets.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

using splice::test::batch_target_a;

using TargetFn = int (*)(int);

constexpr int kArg   = 4;
constexpr int kPlain = kArg * 7;      // 28

std::atomic<int> g_calls{0};

// The test follows the thunk itself rather than asking splice to, and that is
// deliberate: the thing under test is whether the group checks the bytes the
// caller supplies, so the caller has to arrive at those bytes independently.
// Under incremental linking `&batch_target_a` is an ILT entry -- a bare
// `E9 rel32` -- and its bytes are not the function's, which is the same trap
// the contention runner hit at a larger scale.
void* follow_thunk(void* address) {
    auto* code = static_cast<unsigned char*>(address);
    for (int depth = 0; depth < 4 && code[0] == 0xE9; ++depth) {
        std::int32_t rel = 0;
        std::memcpy(&rel, code + 1, sizeof(rel));
        code = code + 5 + rel;
    }
    return code;
}

void snapshot_prologue(void* target, unsigned char (&out)[SPLICE_LIVE_EXPECTED_BYTES]) {
    std::memcpy(out, follow_thunk(target), sizeof(out));
}

class ExpectPrologue : public ::testing::Test {
protected:
    void SetUp() override {
        if (!splice::live_tier_available()) {
            GTEST_SKIP() << "the coordinated tier needs an OS that can suspend "
                            "a thread and move its program counter";
        }
        g_calls.store(0);
    }

    void TearDown() override {
        if (outcome.active()) {
            EXPECT_TRUE(outcome.disable());
        }
        EXPECT_EQ(batch_target_a(kArg), kPlain) << "the target was left hooked";
    }

    splice::Outcome outcome;
};

} // namespace

TEST_F(ExpectPrologue, the_right_expectation_installs) {
    // The target's own bytes, read through the same unwrap the group performs,
    // which is why they are taken from the address the group will patch rather
    // than from `&batch_target_a` -- under incremental linking that is a jump
    // thunk, and its bytes are not the function's.
    void* const entry = reinterpret_cast<void*>(&batch_target_a);
    unsigned char expected[SPLICE_LIVE_EXPECTED_BYTES]{};
    snapshot_prologue(entry, expected);

    splice::Live<> group;
    outcome = group
        .at(entry, TargetFn{}).expect_prologue(expected)
        .onInvoke([](auto orig, int x) {
            g_calls.fetch_add(1, std::memory_order_relaxed);
            return orig(x) + 1;
        })
        .install(splice::live);

    ASSERT_EQ(outcome.state(), splice::State::installed)
        << "stage=" << static_cast<int>(outcome.failure().stage)
        << " op=" << static_cast<int>(outcome.failure().operation);
    EXPECT_EQ(batch_target_a(kArg), kPlain + 1);
    EXPECT_EQ(g_calls.load(), 1);
}

TEST_F(ExpectPrologue, a_wrong_expectation_is_refused_and_writes_nothing) {
    void* const entry = reinterpret_cast<void*>(&batch_target_a);
    unsigned char wrong[SPLICE_LIVE_EXPECTED_BYTES]{};
    snapshot_prologue(entry, wrong);
    wrong[0] = static_cast<unsigned char>(wrong[0] ^ 0xFFu);   // one byte off

    splice::Live<> group;
    const splice::Outcome refused = group
        .at(entry, TargetFn{}).expect_prologue(wrong)
        .onInvoke([](auto orig, int x) {
            g_calls.fetch_add(1000, std::memory_order_relaxed);
            return orig(x) + 1;
        })
        .install(splice::live);

    EXPECT_EQ(refused.state(), splice::State::refused);
    EXPECT_EQ(refused.installed_sites(), 0u);
    EXPECT_EQ(refused.failure().stage, splice::Stage::prepare)
        << "a prologue mismatch must be caught before anything is written";
    EXPECT_EQ(batch_target_a(kArg), kPlain) << "a refused install wrote anyway";
    EXPECT_EQ(g_calls.load(), 0);
}

TEST_F(ExpectPrologue, a_site_that_did_not_ask_is_not_checked) {
    // Null means "do not check", which is every site that existed before this
    // and must stay the default. Otherwise adding the field would have made
    // every address-named group refuse.
    void* const entry = reinterpret_cast<void*>(&batch_target_a);

    splice::Live<> group;
    outcome = group
        .at(entry, TargetFn{}, [](auto orig, int x) {
            g_calls.fetch_add(1, std::memory_order_relaxed);
            return orig(x) + 1;
        })
        .install(splice::live);

    ASSERT_EQ(outcome.state(), splice::State::installed);
    EXPECT_EQ(batch_target_a(kArg), kPlain + 1);
}
