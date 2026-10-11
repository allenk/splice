// ─── R1: the group transaction's fault matrix ─────────────────────────────
//
// `recovered` and `lost` are the two states the whole design rests on, and
// neither occurs on demand in a healthy process. This is how they are
// produced: fail operation o on thread j at site i, then assert the world.
// Every row of design section 8.3 is here, and so is every case the interim
// single-site primitive carried -- that matrix was paid for once and is not
// being re-derived.
//
// The seam is a compile-time template parameter and it is a TEST seam. The
// primitive this replaces documented its own seam as internal and was then
// used through it from outside anyway, which is the whole reason the
// replacement keeps the seam and moves the surface.
//
// The plans are real: a real PreparedStrictPatch over real executable memory,
// with a real instruction-boundary map. Only the threads are fake, because a
// real thread cannot be asked to be at a chosen program counter on demand --
// that is a different runner, and it is the one that closes gate 4.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include "engine/live_install.h"
#include "os/memory.h"

#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace {

using namespace splice::engine;
using splice::os::ThreadRef;

constexpr std::size_t kBlock = 4096;

// One fake thread. `count` models the OS suspend count, so a thread that was
// already suspended before the transaction started is representable -- and
// that case is the reason resume compares counts at all.
struct FakeThread {
    std::uint32_t  id = 0;
    std::uint32_t  count = 0;
    std::uintptr_t pc = 0;
    bool fail_suspend = false;
    bool fail_capture = false;
    bool fail_set = false;
    bool fail_restore = false;     // the second set_pc, during the undo
    bool fail_resume = false;
    unsigned sets = 0;
    unsigned resumes = 0;
};

struct FakeOps {
    std::vector<unsigned char>* watched = nullptr;   // expected bytes per site
    std::vector<unsigned char*>* targets = nullptr;  // site targets, same order
    bool mismatched_resume = false;
    bool fail_flush = false;
    mutable unsigned suspends = 0;
    mutable unsigned flushes = 0;
    mutable unsigned resume_calls = 0;
    mutable bool restored_before_resume = true;      // the ordering assertion
    mutable bool saw_resume = false;

    static FakeThread& of(ThreadRef ref) {
        return *reinterpret_cast<FakeThread*>(ref.value);
    }

    splice::os::Enumeration enumerate(std::span<ThreadRef>) const noexcept {
        return splice::os::Enumeration{splice::os::EnumerateStatus::unsupported, 0};
    }
    void release(ThreadRef) const noexcept {}
    std::uint32_t id(ThreadRef ref) const noexcept { return of(ref).id; }

    bool suspend(ThreadRef ref, std::uint32_t& prior) const noexcept {
        ++suspends;
        auto& thread = of(ref);
        if (thread.fail_suspend) return false;
        prior = thread.count++;
        return true;
    }

    bool resume(ThreadRef ref, std::uint32_t& prior) const noexcept {
        auto& thread = of(ref);
        ++thread.resumes;
        ++resume_calls;
        // Checked on the first resume of the transaction: by the time any
        // thread is let go, every site must already be back to the bytes it
        // started with. This is the assertion that makes "sites 0..4 restored
        // before resume" a test rather than a claim.
        if (!saw_resume) {
            saw_resume = true;
            if (targets != nullptr && watched != nullptr) {
                for (std::size_t i = 0; i < targets->size(); ++i) {
                    if (std::memcmp((*targets)[i], watched->data() + i * 16, 16) != 0) {
                        restored_before_resume = false;
                    }
                }
            }
        }
        if (thread.fail_resume) return false;
        const std::uint32_t before = thread.count;
        if (thread.count != 0) --thread.count;
        prior = mismatched_resume ? before + 1 : before;
        return true;
    }

    bool capture_pc(ThreadRef ref, std::uintptr_t& pc) const noexcept {
        auto& thread = of(ref);
        if (thread.fail_capture) return false;
        pc = thread.pc;
        return true;
    }

    bool set_pc(ThreadRef ref, std::uintptr_t pc) const noexcept {
        auto& thread = of(ref);
        ++thread.sets;
        // A failed set is not assumed side-effect-free: the program counter is
        // written either way, which is what makes the undo path necessary
        // rather than optional.
        thread.pc = pc;
        if (thread.sets == 1) return !thread.fail_set;
        return !thread.fail_restore;
    }

    bool flush(void*, std::size_t) const noexcept {
        ++flushes;
        return !fail_flush;
    }

    std::uint32_t last_error() const noexcept { return 0; }
};

// A small, relocatable prologue: mov rax,rcx / mov eax,42 / ret. Instruction
// boundaries at 0, 3 and 8, which is what makes an "interior, non-boundary"
// program counter expressible at offset 1.
const unsigned char kCode[] = {0x48, 0x8b, 0xc1, 0xb8, 42, 0, 0, 0, 0xc3};

class FaultMatrix : public ::testing::Test {
protected:
    void SetUp() override {
        for (std::size_t i = 0; i < kSites; ++i) {
            blocks[i] = static_cast<unsigned char*>(
                splice::os::allocate_executable_memory(kBlock));
            ASSERT_NE(blocks[i], nullptr);
            std::memset(blocks[i], 0x90, kBlock);
            std::memcpy(blocks[i], kCode, sizeof(kCode));
        }
        // Somewhere for the patches to point. Never executed here.
        replacement = static_cast<unsigned char*>(
            splice::os::allocate_executable_memory(kBlock));
        ASSERT_NE(replacement, nullptr);
        std::memset(replacement, 0xc3, kBlock);

        for (std::size_t i = 0; i < kThreads; ++i) {
            threads[i].id = static_cast<std::uint32_t>(i + 1);
            threads[i].count = static_cast<std::uint32_t>(i);  // pre-existing suspension
            threads[i].pc = reinterpret_cast<std::uintptr_t>(blocks[0]) + (i ? 3 : 0);
        }
    }

    void TearDown() override {
        for (auto* block : blocks) {
            if (block) splice::os::free_executable_memory(block, kBlock);
        }
        if (replacement) splice::os::free_executable_memory(replacement, kBlock);
    }

    // Build a group over `sites` targets and `thread_count` fake threads, with
    // every plan prepared and nothing written.
    LiveGroup make_group(std::size_t sites, std::size_t thread_count) {
        LiveGroup group;
        group.sites.resize(sites);
        expected.assign(sites * 16, 0);
        site_targets.clear();
        for (std::size_t i = 0; i < sites; ++i) {
            auto& site = group.sites[i];
            site.target = blocks[i];
            std::memcpy(site.snapshot, blocks[i], 16);
            std::memcpy(expected.data() + i * 16, blocks[i], 16);
            site_targets.push_back(blocks[i]);
            EXPECT_TRUE(splice::os::make_executable_writable(blocks[i], 16));
            site.made_writable = true;
            const bool planned = site.plan.prepare(blocks[i], replacement,
                                                   site.snapshot, 16);
            EXPECT_TRUE(planned) << "plan for site " << i;
            site.copy_size = site.plan.copy_size();
        }
        group.threads.resize(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            group.threads[i].ref = ThreadRef{reinterpret_cast<std::uintptr_t>(&threads[i])};
            group.threads[i].id = threads[i].id;
        }
        group.prepared = true;
        return group;
    }

    FakeOps make_ops() {
        FakeOps ops;
        ops.watched = &expected;
        ops.targets = &site_targets;
        return ops;
    }

    // Every target holds the bytes it started with.
    void ExpectNothingWritten() {
        for (std::size_t i = 0; i < site_targets.size(); ++i) {
            EXPECT_EQ(std::memcmp(site_targets[i], expected.data() + i * 16, 16), 0)
                << "site " << i << " was written";
        }
    }

    void ExpectCountsRestored(std::size_t thread_count) {
        for (std::size_t i = 0; i < thread_count; ++i) {
            EXPECT_EQ(threads[i].count, static_cast<std::uint32_t>(i))
                << "thread " << i << " did not come back to its original count";
        }
    }

    void ExpectStillSuspended(std::size_t index) {
        EXPECT_EQ(threads[index].count, static_cast<std::uint32_t>(index) + 1)
            << "thread " << index << " should still carry our suspension";
    }

    static constexpr std::size_t kSites = 6;
    static constexpr std::size_t kThreads = 4;

    std::array<unsigned char*, kSites> blocks{};
    unsigned char* replacement = nullptr;
    std::array<FakeThread, kThreads> threads{};
    std::vector<unsigned char> expected;
    std::vector<unsigned char*> site_targets;
};

} // namespace

// ─── the healthy path ─────────────────────────────────────────────────────

TEST_F(FaultMatrix, every_site_is_written_and_every_thread_comes_back) {
    auto group = make_group(3, 2);
    auto ops = make_ops();

    const auto result = commit_group(group, ops);

    EXPECT_EQ(result.state, LiveState::installed);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_NE(std::memcmp(site_targets[i], expected.data() + i * 16, 16), 0)
            << "site " << i << " was not written";
    }
    ExpectCountsRestored(2);
    // The thread whose program counter sat at a boundary inside the displaced
    // prefix was moved into the trampoline; the one at offset 0 was too.
    EXPECT_EQ(threads[0].sets, 1u);
    EXPECT_EQ(threads[1].sets, 1u);
}

// ─── enlist ───────────────────────────────────────────────────────────────

TEST_F(FaultMatrix, a_suspend_failure_refuses_and_unwinds_only_our_own_increments) {
    threads[1].fail_suspend = true;
    auto group = make_group(2, 3);
    auto ops = make_ops();

    const auto result = commit_group(group, ops);

    EXPECT_EQ(result.state, LiveState::refused);
    EXPECT_EQ(result.failure.stage, LiveStage::enlist);
    EXPECT_EQ(result.failure.operation, LiveOperation::suspend);
    EXPECT_EQ(result.failure.thread_id, 2u);
    ExpectNothingWritten();
    ExpectCountsRestored(3);
    EXPECT_EQ(threads[1].resumes, 0u) << "a thread we failed to suspend is not resumed";
}

TEST_F(FaultMatrix, a_failed_unwind_during_enlist_is_lost_not_a_clean_refusal) {
    threads[2].fail_suspend = true;
    threads[0].fail_resume = true;
    auto group = make_group(2, 3);
    auto ops = make_ops();

    const auto result = commit_group(group, ops);

    EXPECT_EQ(result.state, LiveState::lost);
    EXPECT_EQ(result.failure.stage, LiveStage::resume);
    ExpectNothingWritten();
    ExpectStillSuspended(0);
}

// ─── map ──────────────────────────────────────────────────────────────────

TEST_F(FaultMatrix, a_context_that_cannot_be_read_refuses_before_any_write) {
    threads[1].fail_capture = true;
    auto group = make_group(2, 2);
    auto ops = make_ops();

    const auto result = commit_group(group, ops);

    EXPECT_EQ(result.state, LiveState::refused);
    EXPECT_EQ(result.failure.stage, LiveStage::map);
    EXPECT_EQ(result.failure.operation, LiveOperation::capture_pc);
    ExpectNothingWritten();
    ExpectCountsRestored(2);
    EXPECT_EQ(threads[0].sets, 0u) << "no program counter is moved before all are mapped";
}

TEST_F(FaultMatrix, an_interior_program_counter_is_refused_before_any_set) {
    // Offset 1 is inside the first instruction, not at a boundary. There is no
    // address in the trampoline that means "one byte into mov rax,rcx", so the
    // only honest answer is to refuse -- and to refuse before moving anything.
    threads[0].pc = reinterpret_cast<std::uintptr_t>(blocks[0]) + 1;
    auto group = make_group(1, 2);
    auto ops = make_ops();

    const auto result = commit_group(group, ops);

    EXPECT_EQ(result.state, LiveState::refused);
    EXPECT_EQ(result.failure.stage, LiveStage::map);
    ExpectNothingWritten();
    ExpectCountsRestored(2);
    EXPECT_EQ(threads[0].sets, 0u);
    EXPECT_EQ(threads[1].sets, 0u);
}

TEST_F(FaultMatrix, a_program_counter_outside_every_site_is_left_where_it_is) {
    threads[0].pc = 0xdead0000;
    threads[1].pc = 0xdead1000;
    auto group = make_group(2, 2);
    auto ops = make_ops();

    const auto result = commit_group(group, ops);

    EXPECT_EQ(result.state, LiveState::installed);
    EXPECT_EQ(threads[0].sets, 0u);
    EXPECT_EQ(threads[1].sets, 0u);
    EXPECT_EQ(threads[0].pc, 0xdead0000u);
    ExpectCountsRestored(2);
}

// ─── migrate ──────────────────────────────────────────────────────────────

TEST_F(FaultMatrix, a_migration_failure_restores_even_the_thread_that_failed) {
    threads[1].fail_set = true;
    auto group = make_group(2, 2);
    auto ops = make_ops();
    const auto original_pc = threads[1].pc;

    const auto result = commit_group(group, ops);

    EXPECT_EQ(result.state, LiveState::recovered);
    EXPECT_EQ(result.failure.stage, LiveStage::migrate);
    EXPECT_EQ(result.failure.operation, LiveOperation::set_pc);
    ExpectNothingWritten();
    ExpectCountsRestored(2);
    EXPECT_EQ(threads[1].pc, original_pc) << "the failed set was still undone";
    EXPECT_EQ(threads[0].pc, reinterpret_cast<std::uintptr_t>(blocks[0]));
}

TEST_F(FaultMatrix, a_restore_that_also_fails_leaves_the_threads_suspended) {
    threads[1].fail_set = true;
    threads[1].fail_restore = true;
    auto group = make_group(2, 2);
    auto ops = make_ops();

    const auto result = commit_group(group, ops);

    // There is no answer that resumes a thread at a program counter nobody can
    // put back, so it stays suspended and the caller is told so.
    EXPECT_EQ(result.state, LiveState::lost);
    ExpectNothingWritten();
    ExpectStillSuspended(0);
    ExpectStillSuspended(1);
    EXPECT_EQ(ops.resume_calls, 0u);
}

// ─── write: the group's own row ───────────────────────────────────────────

TEST_F(FaultMatrix, a_write_failure_at_site_five_restores_the_four_before_it) {
    auto group = make_group(6, 2);
    auto ops = make_ops();

    // Site 5 refuses to commit because its target no longer holds what the
    // plan snapshotted. That is the realistic shape of a mid-group failure:
    // something else changed the prologue after prepare.
    blocks[5][0] = 0xcc;
    expected[5 * 16] = 0xcc;   // what site 5 should still hold at resume time

    const auto result = commit_group(group, ops);

    EXPECT_EQ(result.state, LiveState::recovered);
    EXPECT_EQ(result.failure.stage, LiveStage::write);
    EXPECT_EQ(result.failure.operation, LiveOperation::write_site);
    EXPECT_EQ(result.failure.site, 5u);

    // Sites 0..4 hold their original bytes again...
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(std::memcmp(site_targets[i], expected.data() + i * 16, 16), 0)
            << "site " << i << " was left installed";
    }
    // ...and they were already restored by the time any thread was let go.
    // Without that ordering, a thread could run through a hook the group has
    // decided not to install.
    EXPECT_TRUE(ops.restored_before_resume);
    ExpectCountsRestored(2);
}

// Deliberately absent: a case where restoring a WRITTEN site fails.
//
// With the x86_64 plan it cannot happen. restore_write refuses only on a
// contract violation -- no storage, never committed, a displaced prefix
// outside 1..16 bytes -- and none of those is reachable for a site the group
// has just successfully written. Writing a test that pretended to drive it
// would be asserting the absence of something that could not occur, which is
// the shape of green-by-omission this project has paid for before.
//
// The branch stays, because it is the only correct answer when a restore does
// fail, and ARM64's restore is a four-instruction sequence with cache
// maintenance that genuinely can. The matrix row arrives with that backend.

// ─── flush and resume ─────────────────────────────────────────────────────

TEST_F(FaultMatrix, a_resume_failure_after_a_successful_write_does_not_hide_the_patch) {
    threads[0].fail_resume = true;
    auto group = make_group(2, 2);
    auto ops = make_ops();

    const auto result = commit_group(group, ops);

    EXPECT_EQ(result.state, LiveState::lost);
    EXPECT_EQ(result.failure.stage, LiveStage::resume);
    EXPECT_EQ(result.failure.operation, LiveOperation::resume_thread);
    EXPECT_EQ(result.failure.thread_id, 1u);
    // The writes stay. Undoing them would be a second decision made after the
    // first one failed, and the caller is the one who has to make it.
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_NE(std::memcmp(site_targets[i], expected.data() + i * 16, 16), 0);
    }
    ExpectStillSuspended(0);

    // The group still says it owns that suspension, which is the only thing a
    // recovery path has to go on: `lost` means some thread is still held, and
    // which one is not guessable from outside. A failed resume that cleared
    // the flag anyway would hand a recovery a thread it does not own and take
    // the count below where it started.
    EXPECT_TRUE(group.threads[0].suspended);
    EXPECT_FALSE(group.threads[1].suspended);
}

TEST_F(FaultMatrix, an_unexpected_resume_count_is_reported_and_not_resumed_twice) {
    auto group = make_group(1, 2);
    auto ops = make_ops();
    ops.mismatched_resume = true;

    const auto result = commit_group(group, ops);

    // The count coming back is not prior + 1, which means something outside
    // this transaction is suspending the same threads. The transaction says so
    // rather than compensating, and it does not resume again to "fix" it.
    EXPECT_EQ(result.state, LiveState::lost);
    EXPECT_EQ(result.failure.stage, LiveStage::resume);
    EXPECT_EQ(threads[0].resumes, 1u);
    EXPECT_EQ(threads[1].resumes, 1u);
}

TEST_F(FaultMatrix, a_flush_failure_keeps_the_threads_suspended) {
    auto group = make_group(2, 2);
    auto ops = make_ops();
    ops.fail_flush = true;

    const auto result = commit_group(group, ops);

    // The writes are in memory but the instruction stream may not have seen
    // them, so resuming a thread would resume it into an unknown mixture of
    // old and new instructions. There is no undo for that -- the sites are
    // already written -- so the threads stay suspended and the caller is told.
    EXPECT_EQ(result.state, LiveState::lost);
    EXPECT_EQ(result.failure.stage, LiveStage::flush);
    EXPECT_EQ(result.failure.operation, LiveOperation::flush_site);
    EXPECT_EQ(result.failure.site, 0u);
    EXPECT_EQ(ops.resume_calls, 0u);
    ExpectStillSuspended(0);
    ExpectStillSuspended(1);
}

// ─── at scale ─────────────────────────────────────────────────────────────

TEST_F(FaultMatrix, a_large_thread_set_keeps_its_order_and_its_counts) {
    // The duplicate scan is pairwise, so this is also the case that says what
    // that costs: 512 threads is 130,816 comparisons before the first
    // suspension, against an enumeration that costs about 118 ms.
    constexpr std::size_t kMany = 512;
    std::vector<FakeThread> many(kMany);
    for (std::size_t i = 0; i < kMany; ++i) {
        many[i].id = static_cast<std::uint32_t>(i + 1);
        many[i].count = static_cast<std::uint32_t>(i % 3);
        many[i].pc = 0xfeed0000 + i;          // outside every site
    }

    auto group = make_group(1, 0);
    group.threads.resize(kMany);
    for (std::size_t i = 0; i < kMany; ++i) {
        group.threads[i].ref = ThreadRef{reinterpret_cast<std::uintptr_t>(&many[i])};
        group.threads[i].id = many[i].id;
    }
    auto ops = make_ops();

    const auto result = commit_group(group, ops);

    EXPECT_EQ(result.state, LiveState::installed);
    EXPECT_EQ(ops.suspends, kMany);
    for (std::size_t i = 0; i < kMany; ++i) {
        EXPECT_EQ(many[i].count, static_cast<std::uint32_t>(i % 3))
            << "thread " << i;
        EXPECT_EQ(many[i].resumes, 1u) << "thread " << i;
    }
}

TEST_F(FaultMatrix, a_duplicate_anywhere_in_a_large_set_refuses_before_suspending) {
    constexpr std::size_t kMany = 512;
    std::vector<FakeThread> many(kMany);
    for (std::size_t i = 0; i < kMany; ++i) {
        many[i].id = static_cast<std::uint32_t>(i + 1);
    }
    many[kMany - 1].id = many[0].id;          // the last repeats the first

    auto group = make_group(1, 0);
    group.threads.resize(kMany);
    for (std::size_t i = 0; i < kMany; ++i) {
        group.threads[i].ref = ThreadRef{reinterpret_cast<std::uintptr_t>(&many[i])};
        group.threads[i].id = many[i].id;
    }
    auto ops = make_ops();

    const auto result = commit_group(group, ops);

    EXPECT_EQ(result.state, LiveState::refused);
    EXPECT_EQ(result.failure.operation, LiveOperation::enumerate);
    EXPECT_EQ(ops.suspends, 0u);
    ExpectNothingWritten();
}

// ─── the plan, moved ──────────────────────────────────────────────────────

TEST_F(FaultMatrix, a_moved_from_plan_owns_nothing) {
    // The plan is movable only because a group of them lives in a vector, and
    // today nothing in the library actually moves one -- resize constructs in
    // place. So the move is exercised here or nowhere, and "nowhere" means a
    // double free waiting for the first caller who grows a group.
    //
    // prepared() is the destructor's own condition read from outside: storage
    // held and not yet committed. A source that still reports true after being
    // moved from is a second owner of one trampoline.
    unsigned char snapshot[16]{};
    std::memcpy(snapshot, blocks[0], sizeof(snapshot));
    ASSERT_TRUE(splice::os::make_executable_writable(blocks[0], 16));

    splice::arch::x86_64::PreparedStrictPatch source;
    ASSERT_TRUE(source.prepare(blocks[0], replacement, snapshot, sizeof(snapshot)));
    ASSERT_TRUE(source.prepared());
    void* const trampoline = source.original();

    splice::arch::x86_64::PreparedStrictPatch moved(std::move(source));
    EXPECT_FALSE(source.prepared()) << "the source still owns the trampoline";
    EXPECT_TRUE(moved.prepared());
    EXPECT_EQ(moved.original(), trampoline) << "the trampoline moved, not copied";
    EXPECT_EQ(moved.target(), blocks[0]);

    // And the moved-to plan is the one that works.
    EXPECT_TRUE(moved.commit_write());
    EXPECT_NE(std::memcmp(blocks[0], snapshot, sizeof(snapshot)), 0);
    EXPECT_TRUE(moved.restore_write());
}

// ─── input ────────────────────────────────────────────────────────────────

TEST_F(FaultMatrix, an_unprepared_group_refuses_without_suspending_anything) {
    LiveGroup empty;
    auto ops = make_ops();
    ops.watched = nullptr;
    ops.targets = nullptr;

    const auto result = commit_group(empty, ops);
    EXPECT_EQ(result.state, LiveState::refused);
    EXPECT_EQ(result.failure.stage, LiveStage::prepare);
    EXPECT_EQ(ops.suspends, 0u);
}

TEST_F(FaultMatrix, a_second_commit_of_the_same_group_refuses_without_suspending) {
    // One attempt per group, successful or not. The interesting case is the
    // one that failed early: its plans are untouched, so nothing downstream
    // would stop a second attempt -- only the group saying it has had its turn.
    threads[1].fail_suspend = true;
    auto group = make_group(1, 2);
    auto ops = make_ops();

    const auto first = commit_group(group, ops);
    ASSERT_EQ(first.state, LiveState::refused);
    ASSERT_EQ(first.failure.stage, LiveStage::enlist);
    const unsigned suspends_after_first = ops.suspends;
    ASSERT_GT(suspends_after_first, 0u);

    const auto again = commit_group(group, ops);
    EXPECT_EQ(again.state, LiveState::refused);
    EXPECT_EQ(again.failure.stage, LiveStage::prepare);
    EXPECT_EQ(ops.suspends, suspends_after_first)
        << "a second attempt must not suspend anything at all";
}

TEST_F(FaultMatrix, a_second_commit_after_a_successful_one_refuses) {
    auto group = make_group(1, 1);
    auto ops = make_ops();
    ASSERT_EQ(commit_group(group, ops).state, LiveState::installed);
    const unsigned suspends_after_first = ops.suspends;

    const auto again = commit_group(group, ops);
    EXPECT_EQ(again.state, LiveState::refused);
    EXPECT_EQ(again.failure.stage, LiveStage::prepare);
    EXPECT_EQ(ops.suspends, suspends_after_first);
}

TEST_F(FaultMatrix, a_duplicated_thread_refuses_before_the_first_suspension) {
    auto group = make_group(1, 2);
    auto ops = make_ops();
    group.threads[1].id = group.threads[0].id;      // the same thread twice

    const auto result = commit_group(group, ops);

    // Suspended twice and resumed once stays suspended forever, and neither
    // entry's bookkeeping would show it. So it is refused before anything is
    // suspended at all.
    EXPECT_EQ(result.state, LiveState::refused);
    EXPECT_EQ(result.failure.stage, LiveStage::prepare);
    EXPECT_EQ(result.failure.operation, LiveOperation::enumerate);
    EXPECT_EQ(ops.suspends, 0u);
    ExpectNothingWritten();
}

TEST_F(FaultMatrix, a_thread_with_no_id_refuses_before_the_first_suspension) {
    auto group = make_group(1, 2);
    auto ops = make_ops();
    group.threads[1].id = 0;

    const auto result = commit_group(group, ops);
    EXPECT_EQ(result.state, LiveState::refused);
    EXPECT_EQ(result.failure.operation, LiveOperation::enumerate);
    EXPECT_EQ(ops.suspends, 0u);
}

TEST_F(FaultMatrix, a_group_with_no_threads_still_installs) {
    // A single-threaded process has nothing to coordinate, and that is not a
    // failure: there is no thread that can be inside the bytes being replaced.
    auto group = make_group(2, 0);
    auto ops = make_ops();

    const auto result = commit_group(group, ops);

    EXPECT_EQ(result.state, LiveState::installed);
    EXPECT_EQ(ops.suspends, 0u);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_NE(std::memcmp(site_targets[i], expected.data() + i * 16, 16), 0);
    }
}

TEST_F(FaultMatrix, more_sites_than_a_group_may_hold_refuses) {
    LiveGroup group;
    group.sites.resize(kMaxGroupSites + 1);
    group.prepared = true;
    auto ops = make_ops();
    ops.watched = nullptr;
    ops.targets = nullptr;

    const auto result = commit_group(group, ops);
    EXPECT_EQ(result.state, LiveState::refused);
    EXPECT_EQ(result.failure.stage, LiveStage::prepare);
    EXPECT_EQ(ops.suspends, 0u);
}
