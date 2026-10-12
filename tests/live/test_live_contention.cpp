// ─── R3: contention, in-process ───────────────────────────────────────────
//
// N threads calling the targets in a tight loop while the group installs. The
// assertion that matters is not "it did not crash" -- a crash-free run is not
// evidence of correctness -- but that **every call returned a value consistent
// with either the hooked or the unhooked implementation**, and never anything
// else. A torn instruction stream shows up as a third value, or as a fault.
//
// This is `LiveHookStress`'s shape pointed at the coordinated tier, and the
// difference between the two is the whole reason the tier exists:
//
//   LiveHookStress patches a live function while sixteen threads execute it,
//   with nothing suspended. Safety there rests on the CPU's cross-modifying-
//   code behaviour, which is why that test is a local gate -- on an
//   oversubscribed runner the window can produce a fault that says nothing
//   about the library.
//
//   Here every thread the engine could enumerate is suspended for the
//   duration of the write, so for those threads there is no
//   cross-modifying-code window to lose: nothing in the enlisted set is
//   executing while the bytes change. If this test ever reports a third value
//   from an enlisted thread, it is not the machine being unlucky.
//
//   The qualification is not pedantry and it bounds what this test proves. The
//   enlisted set is one enumeration taken before the first suspension, and a
//   thread created after it is neither suspended nor detected. Every worker
//   here is started before the first install and joined after the last, which
//   is what puts them all in the set -- so this runner demonstrates the
//   property for a set it arranged to be complete, and says nothing about a
//   process where threads appear during an install. That case has no detector
//   anywhere in this project.
//
// So the install is repeated -- install, disable, install, disable, under
// continuous load -- because one install can get away with a race that a dozen
// cannot, and because disable has the same window as install and had never been
// driven under load at all.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/live.h>

#include "test_targets.h"
#include "test_env.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using splice::test::contention_target_a;
using splice::test::contention_target_b;

using TargetFn = int (*)(int);

constexpr int kArgument = 7;
constexpr int kPlainA   = kArgument * 3;          // 21
constexpr int kPlainB   = kArgument * 5;          // 35
constexpr int kHookedA  = kPlainA + 1000;         // 1021
constexpr int kHookedB  = kPlainB + 1000;         // 1035

std::atomic<long long> g_hook_calls{0};

} // namespace

TEST(LiveContention, threads_call_both_targets_across_repeated_installs) {
    if (!splice::live_tier_available()) {
        GTEST_SKIP() << "the coordinated tier needs an OS that can suspend a "
                        "thread and move its program counter";
    }
    if (splice::test::env_is_set("CI")) {
        // Kept a local gate, and the reason is not the one LiveHookStress has.
        // There is no cross-modifying-code window here to be unlucky with. What
        // this does is suspend every thread in the process several times, with
        // a thread enumeration costing about 118 ms each time, so on a
        // two-vCPU shared runner it is slow rather than wrong -- and a slow
        // gate on shared hardware is a gate that gets switched off.
        GTEST_SKIP() << "coordinated-tier contention gate: suspends every thread "
                        "in the process repeatedly; run locally";
    }

    constexpr int kThreads = 16;
    std::atomic<int>       stop{0};
    std::atomic<int>       ready{0};
    std::atomic<int>       bad_a{0};
    std::atomic<int>       bad_b{0};
    std::atomic<long long> calls{0};

    auto worker = [&] {
        ready.fetch_add(1, std::memory_order_release);
        while (stop.load(std::memory_order_relaxed) == 0) {
            const int a = contention_target_a(kArgument);
            if (a != kPlainA && a != kHookedA) {
                bad_a.fetch_add(1, std::memory_order_relaxed);
            }
            const int b = contention_target_b(kArgument);
            if (b != kPlainB && b != kHookedB) {
                bad_b.fetch_add(1, std::memory_order_relaxed);
            }
            calls.fetch_add(2, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) threads.emplace_back(worker);
    while (ready.load(std::memory_order_acquire) < kThreads) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Four full cycles. Each one suspends sixteen busy worker threads plus
    // whatever else the process has, writes two sites, and resumes.
    constexpr int kCycles = 4;
    int installs = 0;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        splice::Live<> group;
        splice::Outcome outcome = group
            .at(reinterpret_cast<void*>(&contention_target_a), TargetFn{},
                [](auto orig, int x) {
                    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
                    return orig(x) + 1000;
                })
            .at(reinterpret_cast<void*>(&contention_target_b), TargetFn{},
                [](auto orig, int x) {
                    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
                    return orig(x) + 1000;
                })
            .non_reentrant()
            .install(splice::live);

        ASSERT_NE(outcome.state(), splice::State::lost)
            << "cycle " << cycle << ": a thread would not come back";
        if (outcome.state() != splice::State::installed) {
            // A refusal under load is a legitimate outcome and not a failure
            // of this test -- a worker may have been exactly at a program
            // counter the plan would not map. What is not legitimate is a
            // refusal that wrote something, and the value check below is what
            // would see it.
            SPLICE_LOGW("LiveContention cycle %d: %s at stage %d op %d tid %u err %u",
                        cycle,
                        outcome.state() == splice::State::recovered
                            ? "recovered" : "refused",
                        static_cast<int>(outcome.failure().stage),
                        static_cast<int>(outcome.failure().operation),
                        outcome.failure().thread_id,
                        outcome.failure().os_error);
            continue;
        }
        ++installs;

        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        EXPECT_TRUE(outcome.disable()) << "cycle " << cycle;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    stop.store(1, std::memory_order_release);
    for (auto& t : threads) t.join();

    SPLICE_LOGI("LiveContention: %lld calls across %d threads, %d/%d installs, "
                "%lld through the hook, %d+%d bad",
                calls.load(), kThreads, installs, kCycles,
                g_hook_calls.load(), bad_a.load(), bad_b.load());

    EXPECT_EQ(bad_a.load(), 0)
        << "a thread saw target A as neither hooked nor unhooked -- the "
           "instruction stream was observed mid-write";
    EXPECT_EQ(bad_b.load(), 0)
        << "a thread saw target B as neither hooked nor unhooked";
    EXPECT_GT(calls.load(), 10000LL) << "the workers did not iterate enough";
    EXPECT_GT(installs, 0) << "no cycle installed, so nothing was contended";
    EXPECT_GT(g_hook_calls.load(), 0LL)
        << "no call arrived through the hook, so the install was not observed";

    // Both targets are back to themselves.
    EXPECT_EQ(contention_target_a(kArgument), kPlainA);
    EXPECT_EQ(contention_target_b(kArgument), kPlainB);
}

TEST(LiveContention, a_group_installs_while_its_own_targets_are_being_called) {
    // The narrower claim, and the one with a hard assertion: during the write
    // nothing is executing, so a worker that is inside the target when the
    // install begins is suspended there and continued afterwards. It must
    // therefore never observe a half-written prologue -- not "rarely", never.
    //
    // Separate from the test above because that one tolerates a refusal under
    // load and this one does not: a single install, with the workers calling
    // the target but not necessarily inside it, has no reason to refuse.
    if (!splice::live_tier_available()) {
        GTEST_SKIP() << "no coordinated tier on this platform";
    }
    if (splice::test::env_is_set("CI")) {
        GTEST_SKIP() << "coordinated-tier contention gate; run locally";
    }

    std::atomic<int>       stop{0};
    std::atomic<int>       ready{0};
    std::atomic<int>       bad{0};
    std::atomic<long long> calls{0};
    constexpr int kThreads = 4;

    auto worker = [&] {
        ready.fetch_add(1, std::memory_order_release);
        while (stop.load(std::memory_order_relaxed) == 0) {
            const int a = contention_target_a(kArgument);
            if (a != kPlainA && a != kHookedA) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
            calls.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) threads.emplace_back(worker);
    while (ready.load(std::memory_order_acquire) < kThreads) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    splice::Live<> group;
    splice::Outcome outcome = group
        .at(reinterpret_cast<void*>(&contention_target_a), TargetFn{},
            [](auto orig, int x) { return orig(x) + 1000; })
        .install(splice::live);

    ASSERT_EQ(outcome.state(), splice::State::installed)
        << "stage=" << static_cast<int>(outcome.failure().stage)
        << " op=" << static_cast<int>(outcome.failure().operation)
        << " tid=" << outcome.failure().thread_id;

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    stop.store(1, std::memory_order_release);
    for (auto& t : threads) t.join();

    EXPECT_EQ(bad.load(), 0);
    EXPECT_GT(calls.load(), 1000LL);
    EXPECT_TRUE(outcome.disable());
    EXPECT_EQ(contention_target_a(kArgument), kPlainA);
}
