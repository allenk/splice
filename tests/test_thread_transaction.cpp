#include <gtest/gtest.h>
#include "os/win32/thread_transaction.h"
#include "os/memory.h"
#include <array>
#include <cstring>
#include <vector>

namespace {
using namespace splice::os::win32;
struct FakeThread {
    DWORD id{};
    DWORD pid{10};
    DWORD count{};
    CONTEXT context{};
    bool fail_suspend{}, fail_get{}, fail_set{}, fail_rollback{}, fail_resume{};
    unsigned sets{}, resumes{};
};
struct FakeOps {
    bool fail_flush{};
    unsigned suspends{}, flushes{};
    unsigned char* mutate_on_set{};
    bool mismatched_resume{};
    static FakeThread& thread(HANDLE h) { return *static_cast<FakeThread*>(h); }
    DWORD thread_id(HANDLE h) { return thread(h).id; }
    DWORD process_id(HANDLE h) { return thread(h).pid; }
    DWORD current_thread() { return 999; }
    DWORD current_process() { return 10; }
    DWORD suspend(HANDLE h) {
        ++suspends;
        auto& t = thread(h);
        return t.fail_suspend ? MAXDWORD : t.count++;
    }
    DWORD resume(HANDLE h) {
        auto& t = thread(h);
        ++t.resumes;
        if (t.fail_resume) return MAXDWORD;
        const auto before = t.count;
        if (t.count) --t.count;
        return mismatched_resume ? before + 1 : before;
    }
    bool get(HANDLE h, CONTEXT& c) {
        auto& t = thread(h);
        if (t.fail_get) return false;
        c = t.context;
        return true;
    }
    bool set(HANDLE h, const CONTEXT& c) {
        auto& t = thread(h);
        ++t.sets;
        if (t.sets > 1 && t.fail_rollback) return false;
        t.context = c;  // Model even a failed call as potentially modifying IP.
        if (mutate_on_set) { *mutate_on_set ^= 1; mutate_on_set = nullptr; }
        return t.sets == 1 ? !t.fail_set : !t.fail_rollback;
    }
    bool flush(void*) { ++flushes; return !fail_flush; }
};
class ThreadTransaction : public ::testing::Test {
protected:
    unsigned char* block{};
    std::array<unsigned char, 16> expected{};
    std::array<FakeThread, 2> threads{};
    std::array<ThreadSlot, 2> slots{};
    std::atomic<void*> publication{};
    FakeOps ops;
    void SetUp() override {
        block = static_cast<unsigned char*>(splice::os::allocate_executable_memory(4096));
        ASSERT_NE(block, nullptr);
        std::memset(block, 0x90, 4096);
        const unsigned char code[] = {0x48, 0x8b, 0xc1, 0xb8, 42, 0, 0, 0, 0xc3};
        std::memcpy(block, code, sizeof(code));
        std::memcpy(expected.data(), block, 16);
        for (std::size_t i = 0; i < threads.size(); ++i) {
            threads[i].id = static_cast<DWORD>(i + 1);
            threads[i].count = static_cast<DWORD>(i); // Preserve pre-existing suspension.
            threads[i].context.ContextFlags = CONTEXT_CONTROL;
            threads[i].context.Rip = reinterpret_cast<DWORD64>(block + (i ? 3 : 0));
            threads[i].context.Rsp = 0x12345678;
            slots[i].handle = &threads[i];
        }
    }
    void TearDown() override { if (block) splice::os::free_executable_memory(block, 4096); }
    void ExpectRestored() {
        EXPECT_EQ(threads[0].count, 0u);
        EXPECT_EQ(threads[1].count, 1u);
        EXPECT_EQ(threads[0].context.Rip, reinterpret_cast<DWORD64>(block));
        EXPECT_EQ(threads[1].context.Rip, reinterpret_cast<DWORD64>(block + 3));
        EXPECT_EQ(publication.load(), nullptr);
        EXPECT_EQ(std::memcmp(block, expected.data(), 16), 0);
    }
};

TEST_F(ThreadTransaction, success_maps_before_resume_and_preserves_counts) {
    splice::arch::x86_64::PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    const auto result = commit_enlisted(plan, slots, publication, ops);
    EXPECT_EQ(result.outcome, TransactionOutcome::installed);
    EXPECT_EQ(result.stage, TransactionStage::complete);
    EXPECT_EQ(publication.load(), plan.original());
    EXPECT_EQ(threads[0].context.Rip, reinterpret_cast<DWORD64>(plan.original()));
    EXPECT_EQ(threads[1].context.Rip, reinterpret_cast<DWORD64>(plan.original()) + 3);
    EXPECT_EQ(threads[1].context.Rsp, 0x12345678u);
    EXPECT_EQ(threads[0].count, 0u);
    EXPECT_EQ(threads[1].count, 1u);
    EXPECT_EQ(ops.flushes, 1u);
    EXPECT_FALSE(slots[0].suspended);
    EXPECT_FALSE(slots[1].suspended);
}

TEST_F(ThreadTransaction, suspend_failure_unwinds_only_owned_increment) {
    splice::arch::x86_64::PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    threads[1].fail_suspend = true;
    const auto result = commit_enlisted(plan, slots, publication, ops);
    EXPECT_EQ(result.outcome, TransactionOutcome::refused);
    EXPECT_EQ(result.stage, TransactionStage::suspend);
    EXPECT_EQ(threads[0].resumes, 1u);
    EXPECT_EQ(threads[1].resumes, 0u);
    ExpectRestored();
}

TEST_F(ThreadTransaction, context_failure_restores_all_counts_without_writes) {
    splice::arch::x86_64::PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    threads[1].fail_get = true;
    const auto result = commit_enlisted(plan, slots, publication, ops);
    EXPECT_EQ(result.outcome, TransactionOutcome::refused);
    EXPECT_EQ(result.stage, TransactionStage::context);
    EXPECT_EQ(threads[0].sets, 0u);
    ExpectRestored();
}

TEST_F(ThreadTransaction, migration_failure_restores_even_the_failed_set) {
    splice::arch::x86_64::PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    threads[1].fail_set = true;
    const auto result = commit_enlisted(plan, slots, publication, ops);
    EXPECT_EQ(result.outcome, TransactionOutcome::refused);
    EXPECT_EQ(result.stage, TransactionStage::migrate);
    EXPECT_EQ(threads[0].sets, 2u);
    EXPECT_EQ(threads[1].sets, 2u);
    ExpectRestored();
}

TEST_F(ThreadTransaction, rollback_failure_retains_storage_and_suspensions) {
    splice::arch::x86_64::PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    threads[1].fail_set = true;
    threads[0].fail_rollback = true;
    const auto result = commit_enlisted(plan, slots, publication, ops);
    EXPECT_EQ(result.outcome, TransactionOutcome::recovery_required);
    EXPECT_EQ(result.stage, TransactionStage::rollback);
    EXPECT_FALSE(plan.prepared()); // Retained and no longer commit-capable.
    EXPECT_TRUE(slots[0].suspended);
    EXPECT_TRUE(slots[1].suspended);
    EXPECT_EQ(threads[0].resumes + threads[1].resumes, 0u);
    EXPECT_EQ(publication.load(), nullptr);
    EXPECT_EQ(std::memcmp(block, expected.data(), 16), 0);
    EXPECT_EQ(threads[0].context.Rip, reinterpret_cast<DWORD64>(plan.original()));
}

TEST_F(ThreadTransaction, resume_failure_does_not_hide_successful_patch) {
    splice::arch::x86_64::PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    threads[1].fail_resume = true;
    const auto result = commit_enlisted(plan, slots, publication, ops);
    EXPECT_EQ(result.outcome, TransactionOutcome::recovery_required);
    EXPECT_EQ(result.stage, TransactionStage::resume);
    EXPECT_EQ(publication.load(), plan.original());
    EXPECT_EQ(block[0], 0xe9);
    EXPECT_FALSE(slots[0].suspended);
    EXPECT_TRUE(slots[1].suspended);
}

TEST_F(ThreadTransaction, flush_failure_keeps_threads_suspended) {
    splice::arch::x86_64::PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    ops.fail_flush = true;
    const auto result = commit_enlisted(plan, slots, publication, ops);
    EXPECT_EQ(result.outcome, TransactionOutcome::recovery_required);
    EXPECT_EQ(result.stage, TransactionStage::flush);
    EXPECT_TRUE(slots[0].suspended);
    EXPECT_TRUE(slots[1].suspended);
    EXPECT_EQ(threads[0].resumes + threads[1].resumes, 0u);
}

TEST_F(ThreadTransaction, stale_commit_restores_contexts_and_publication) {
    splice::arch::x86_64::PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    ops.mutate_on_set = block + 15;
    const auto result = commit_enlisted(plan, slots, publication, ops);
    EXPECT_EQ(result.outcome, TransactionOutcome::refused);
    EXPECT_EQ(result.stage, TransactionStage::commit);
    EXPECT_EQ(threads[0].count, 0u);
    EXPECT_EQ(threads[1].count, 1u);
    EXPECT_EQ(threads[0].context.Rip, reinterpret_cast<DWORD64>(block));
    EXPECT_EQ(threads[1].context.Rip, reinterpret_cast<DWORD64>(block + 3));
    EXPECT_EQ(publication.load(), nullptr);
    EXPECT_EQ(std::memcmp(block, expected.data(), 15), 0);
    EXPECT_EQ(block[15], expected[15] ^ 1); // Do not overwrite the external change.
}

TEST_F(ThreadTransaction, interior_ip_is_rejected_before_any_set) {
    splice::arch::x86_64::PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    threads[1].context.Rip = reinterpret_cast<DWORD64>(block + 4);
    const auto result = commit_enlisted(plan, slots, publication, ops);
    EXPECT_EQ(result.outcome, TransactionOutcome::refused);
    EXPECT_EQ(result.stage, TransactionStage::boundary);
    EXPECT_EQ(threads[0].sets + threads[1].sets, 0u);
    EXPECT_EQ(threads[0].count, 0u);
    EXPECT_EQ(threads[1].count, 1u);
    EXPECT_EQ(std::memcmp(block, expected.data(), 16), 0);
}

TEST_F(ThreadTransaction, invalid_identity_refuses_before_suspension) {
    splice::arch::x86_64::PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    for (int scenario = 0; scenario != 4; ++scenario) {
        threads[1].id = scenario == 0 ? 999 : scenario == 1 ? 1 : scenario == 2 ? 0 : 2;
        threads[1].pid = scenario == 3 ? 11 : 10;
        const auto result = commit_enlisted(plan, slots, publication, ops);
        EXPECT_EQ(result.outcome, TransactionOutcome::refused);
        EXPECT_EQ(result.stage, TransactionStage::identity);
        EXPECT_EQ(ops.suspends, 0u);
    }
    ExpectRestored();
}

TEST_F(ThreadTransaction, abort_resume_failure_is_not_clean_refusal) {
    splice::arch::x86_64::PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    threads[1].fail_get = true;
    threads[0].fail_resume = true;
    const auto result = commit_enlisted(plan, slots, publication, ops);
    EXPECT_EQ(result.outcome, TransactionOutcome::recovery_required);
    EXPECT_EQ(result.stage, TransactionStage::resume);
    EXPECT_TRUE(slots[0].suspended);
    EXPECT_FALSE(slots[1].suspended);
    EXPECT_EQ(publication.load(), nullptr);
    EXPECT_EQ(std::memcmp(block, expected.data(), 16), 0);
}

TEST_F(ThreadTransaction, unexpected_resume_count_does_not_double_resume) {
    splice::arch::x86_64::PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    ops.mismatched_resume = true;
    const auto result = commit_enlisted(plan, slots, publication, ops);
    EXPECT_EQ(result.outcome, TransactionOutcome::recovery_required);
    EXPECT_EQ(result.stage, TransactionStage::resume);
    EXPECT_FALSE(slots[0].suspended);
    EXPECT_FALSE(slots[1].suspended);
    EXPECT_EQ(threads[0].resumes, 1u);
    EXPECT_EQ(threads[1].resumes, 1u);
}

TEST_F(ThreadTransaction, invalid_inputs_refuse_without_suspension) {
    splice::arch::x86_64::PreparedStrictPatch plan;
    EXPECT_EQ(commit_enlisted(plan, slots, publication, ops).stage, TransactionStage::input);
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    EXPECT_EQ(commit_enlisted(plan, std::span<ThreadSlot>{}, publication, ops).stage,
              TransactionStage::input);
    std::vector<ThreadSlot> too_many(4097);
    EXPECT_EQ(commit_enlisted(plan, too_many, publication, ops).stage, TransactionStage::input);
    publication.store(block);
    EXPECT_EQ(commit_enlisted(plan, slots, publication, ops).stage, TransactionStage::input);
    EXPECT_EQ(publication.load(), block);
    publication.store(nullptr);
    slots[0].suspended = true;
    EXPECT_EQ(commit_enlisted(plan, slots, publication, ops).stage, TransactionStage::input);
    slots[0].suspended = false;
    slots[0].context_attempted = true;
    EXPECT_EQ(commit_enlisted(plan, slots, publication, ops).stage, TransactionStage::input);
    EXPECT_EQ(ops.suspends, 0u);
}

TEST_F(ThreadTransaction, large_sets_preserve_order_and_suspend_counts) {
    for (const std::size_t count : {129u, 258u, 1024u, 4096u}) {
        SCOPED_TRACE(count);
        std::memcpy(block, expected.data(), 16);
        publication.store(nullptr);
        splice::arch::x86_64::PreparedStrictPatch plan;
        ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
        std::vector<FakeThread> many(count);
        std::vector<ThreadSlot> enlisted(count);
        for (std::size_t i = 0; i < count; ++i) {
            many[i].id = static_cast<DWORD>(10000 + count - i);
            many[i].count = static_cast<DWORD>(i % 3);
            many[i].context.Rip = reinterpret_cast<DWORD64>(block);
            enlisted[i].handle = &many[i];
        }
        FakeOps local_ops;
        ASSERT_EQ(commit_enlisted(plan, enlisted, publication, local_ops).outcome,
                  TransactionOutcome::installed);
        EXPECT_EQ(local_ops.suspends, count);
        for (std::size_t i = 0; i < count; ++i) {
            EXPECT_EQ(enlisted[i].handle, &many[i]);
            EXPECT_EQ(many[i].count, i % 3);
            EXPECT_EQ(many[i].resumes, 1u);
            EXPECT_FALSE(enlisted[i].suspended);
        }
    }
}

TEST_F(ThreadTransaction, large_duplicate_set_refuses_before_suspension) {
    splice::arch::x86_64::PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    std::vector<FakeThread> many(258);
    std::vector<ThreadSlot> enlisted(many.size());
    for (std::size_t i = 0; i < many.size(); ++i) {
        many[i].id = static_cast<DWORD>(10000 + i);
        enlisted[i].handle = &many[i];
    }
    many.back().id = many.front().id;
    const auto result = commit_enlisted(plan, enlisted, publication, ops);
    EXPECT_EQ(result.outcome, TransactionOutcome::refused);
    EXPECT_EQ(result.stage, TransactionStage::identity);
    EXPECT_EQ(ops.suspends, 0u);
    EXPECT_EQ(publication.load(), nullptr);
    EXPECT_EQ(std::memcmp(block, expected.data(), 16), 0);
}
}
