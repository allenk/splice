// ─── src/os/threads.h — the OS thread surface ─────────────────────────────
//
// Real threads, no patching. Everything here is about whether the OS layer
// reports the truth: that the thread set is complete or admits it is not, that
// suspend counts come back the way they went in, and that the program counter
// can be read and written without disturbing anything else.
//
// One discipline runs through the suspended sections: **no assertion, no
// allocation and no logging happens while a thread is suspended.** Results are
// recorded into locals and checked after the resume. A suspended thread can
// hold the heap lock, and a gtest failure allocates — which would deadlock the
// very test that found the bug, and report nothing.
//
// The worker threads publish their own thread ids, so every suspend and every
// program-counter write in this file targets a thread this file created.
// Suspending an arbitrary thread of the test process and moving its program
// counter would be a different and much worse test.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include "os/threads.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(_WIN32)
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <windows.h>
#endif

namespace {

using splice::os::EnumerateStatus;
using splice::os::ThreadRef;

#if defined(_WIN32)

constexpr auto kPatience = std::chrono::seconds(10);

// A worker that parks in a spin loop, publishes its thread id, and finishes
// with a value the test can check. Parked on a spin rather than a condition
// variable on purpose: a thread suspended inside a futex wait is a less
// interesting place to read a program counter from than one in our own code.
class ParkedWorker {
public:
    void start() {
        m_thread = std::thread([this] {
#if defined(_WIN32)
            m_tid.store(::GetCurrentThreadId(), std::memory_order_release);
#endif
            m_parked.store(true, std::memory_order_release);
            while (!m_release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            m_result.store(42, std::memory_order_release);
        });
    }

    [[nodiscard]] bool wait_until_parked() const noexcept {
        const auto deadline = std::chrono::steady_clock::now() + kPatience;
        while (!m_parked.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::yield();
        }
        return true;
    }

    void release_and_join() {
        m_release.store(true, std::memory_order_release);
        if (m_thread.joinable()) m_thread.join();
    }

    [[nodiscard]] std::uint32_t tid() const noexcept {
        return m_tid.load(std::memory_order_acquire);
    }

    [[nodiscard]] int result() const noexcept {
        return m_result.load(std::memory_order_acquire);
    }

    ~ParkedWorker() { release_and_join(); }

private:
    std::thread       m_thread;
    std::atomic<bool> m_parked{false};
    std::atomic<bool> m_release{false};
    std::atomic<std::uint32_t> m_tid{0};
    std::atomic<int>  m_result{0};
};

// Releases whatever an enumeration wrote, however it ended. The surface
// requires this of every caller, success or not.
class ReleaseAll {
public:
    ReleaseAll(std::vector<ThreadRef>& refs, std::size_t count) noexcept
        : m_refs(refs), m_count(count) {}

    ~ReleaseAll() {
        for (std::size_t i = 0; i < m_count; ++i) splice::os::release_thread(m_refs[i]);
    }

    ReleaseAll(const ReleaseAll&) = delete;
    ReleaseAll& operator=(const ReleaseAll&) = delete;

private:
    std::vector<ThreadRef>& m_refs;
    std::size_t             m_count;
};

[[nodiscard]] std::size_t index_of_tid(const std::vector<ThreadRef>& refs,
                                       std::size_t count, std::uint32_t tid) {
    for (std::size_t i = 0; i < count; ++i) {
        if (splice::os::thread_id(refs[i]) == tid) return i;
    }
    return count;
}

#endif // _WIN32

} // namespace

// ─── True on every platform ───────────────────────────────────────────────

// A reference that was never produced by an enumeration is rejected by every
// call, so a caller that ignores a failed enumeration still cannot act on
// nothing.
TEST(OsThreads, a_zero_reference_is_refused_by_every_call) {
    const ThreadRef none{};
    EXPECT_FALSE(static_cast<bool>(none));
    EXPECT_EQ(splice::os::thread_id(none), 0u);

    std::uint32_t count = 7;
    std::uintptr_t pc = 0x1234;
    EXPECT_FALSE(splice::os::suspend(none, count));
    EXPECT_FALSE(splice::os::resume(none, count));
    EXPECT_FALSE(splice::os::capture_pc(none, pc));
    EXPECT_FALSE(splice::os::set_pc(none, 0x5678));

    EXPECT_EQ(count, 7u) << "a failed call must not write its output";
    EXPECT_EQ(pc, 0x1234u);

    splice::os::release_thread(none);   // harmless, so a partial buffer releases in one loop
}

#if defined(_WIN32)

TEST(OsThreads, this_os_reports_coordination_as_supported) {
    EXPECT_TRUE(splice::os::kCoordinationSupported);
}

// The caller is excluded because every caller needs it excluded, and no thread
// is listed twice -- the two properties the transaction's bookkeeping rests on.
TEST(OsThreads, enumeration_excludes_the_caller_and_lists_each_thread_once) {
    ParkedWorker workers[3];
    for (auto& worker : workers) {
        worker.start();
        ASSERT_TRUE(worker.wait_until_parked());
    }

    std::vector<ThreadRef> refs(512);
    const auto found = splice::os::enumerate_other_threads(refs);
    const ReleaseAll release(refs, found.count);

    ASSERT_EQ(found.status, EnumerateStatus::ok);
    ASSERT_GE(found.count, 3u);

    const auto self = static_cast<std::uint32_t>(::GetCurrentThreadId());
    std::vector<std::uint32_t> ids;
    ids.reserve(found.count);
    for (std::size_t i = 0; i < found.count; ++i) {
        const auto id = splice::os::thread_id(refs[i]);
        EXPECT_NE(id, 0u);
        EXPECT_NE(id, self) << "the calling thread must never be enlisted";
        ids.push_back(id);
    }

    // A premise, not a guarantee this layer makes: nothing here deduplicates,
    // so this checks that Toolhelp does not list a thread twice. It is here
    // because the transaction's bookkeeping depends on it -- a thread listed
    // twice would be suspended twice and resumed once -- and an assumption
    // nobody checks is how that becomes someone else's crash.
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(std::adjacent_find(ids.begin(), ids.end()), ids.end())
        << "the OS listed a thread of this process more than once";

    for (const auto& worker : workers) {
        EXPECT_NE(index_of_tid(refs, found.count, worker.tid()), found.count)
            << "a thread this test created was not enumerated";
    }
}

// An incomplete thread set is the one thing coordination cannot be built on,
// so running out of room is a status of its own and not a short count.
TEST(OsThreads, a_buffer_too_small_reports_truncated_rather_than_success) {
    ParkedWorker workers[2];
    for (auto& worker : workers) {
        worker.start();
        ASSERT_TRUE(worker.wait_until_parked());
    }

    std::vector<ThreadRef> one(1);
    const auto found = splice::os::enumerate_other_threads(one);
    const ReleaseAll release(one, found.count);

    EXPECT_EQ(found.status, EnumerateStatus::truncated);
    EXPECT_EQ(found.count, 1u) << "what did fit is still owned by the caller";
}

TEST(OsThreads, an_empty_buffer_is_truncated_and_writes_nothing) {
    ParkedWorker worker;
    worker.start();
    ASSERT_TRUE(worker.wait_until_parked());

    const auto found = splice::os::enumerate_other_threads(std::span<ThreadRef>{});
    EXPECT_EQ(found.status, EnumerateStatus::truncated);
    EXPECT_EQ(found.count, 0u);
}

// The counts are the transaction's only evidence that nobody else is
// suspending the same threads, so they have to be exactly what the OS saw.
TEST(OsThreads, suspend_and_resume_report_the_count_from_before_each_call) {
    ParkedWorker worker;
    worker.start();
    ASSERT_TRUE(worker.wait_until_parked());

    std::vector<ThreadRef> refs(512);
    const auto found = splice::os::enumerate_other_threads(refs);
    const ReleaseAll release(refs, found.count);
    ASSERT_EQ(found.status, EnumerateStatus::ok);

    const std::size_t index = index_of_tid(refs, found.count, worker.tid());
    ASSERT_NE(index, found.count);
    const ThreadRef target = refs[index];

    // Nothing but OS calls between the first suspend and the last resume.
    std::uint32_t first = 99, second = 99, third = 99, fourth = 99;
    const bool ok_first  = splice::os::suspend(target, first);
    const bool ok_second = ok_first && splice::os::suspend(target, second);
    const bool ok_third  = ok_second && splice::os::resume(target, third);
    const bool ok_fourth = ok_third && splice::os::resume(target, fourth);

    EXPECT_TRUE(ok_first);
    EXPECT_TRUE(ok_second);
    EXPECT_TRUE(ok_third);
    EXPECT_TRUE(ok_fourth);
    EXPECT_EQ(first, 0u)  << "a running thread was suspended zero times before";
    EXPECT_EQ(second, 1u) << "the second suspend saw the first one";
    EXPECT_EQ(third, 2u)  << "the first resume saw both suspends";
    EXPECT_EQ(fourth, 1u) << "the last resume saw the one remaining";

    worker.release_and_join();
    EXPECT_EQ(worker.result(), 42) << "the thread ran to completion after resuming";
}

// capture_pc reads and set_pc writes. Proved by reading back the written value
// while the thread is still suspended, and restoring it before resuming -- so
// the write is demonstrated without ever resuming a thread at an address that
// is not an instruction boundary. Resuming elsewhere is a guarantee of its own
// and needs the boundary fixture, not this test.
TEST(OsThreads, the_program_counter_can_be_read_and_written_while_suspended) {
    ParkedWorker worker;
    worker.start();
    ASSERT_TRUE(worker.wait_until_parked());

    std::vector<ThreadRef> refs(512);
    const auto found = splice::os::enumerate_other_threads(refs);
    const ReleaseAll release(refs, found.count);
    ASSERT_EQ(found.status, EnumerateStatus::ok);

    const std::size_t index = index_of_tid(refs, found.count, worker.tid());
    ASSERT_NE(index, found.count);
    const ThreadRef target = refs[index];

    std::uint32_t ignored = 0;
    std::uintptr_t captured = 0, read_back = 0, restored = 0;
    bool ok_capture = false, ok_set = false, ok_read_back = false;
    bool ok_restore = false, ok_restored_read = false, ok_resume = false;

    if (splice::os::suspend(target, ignored)) {
        ok_capture = splice::os::capture_pc(target, captured);
        // A deliberately absurd destination: it is never resumed at.
        ok_set = ok_capture && splice::os::set_pc(target, captured + 1);
        ok_read_back = ok_set && splice::os::capture_pc(target, read_back);
        ok_restore = ok_read_back && splice::os::set_pc(target, captured);
        ok_restored_read = ok_restore && splice::os::capture_pc(target, restored);
        ok_resume = splice::os::resume(target, ignored);
    }

    EXPECT_TRUE(ok_capture);
    EXPECT_TRUE(ok_set);
    EXPECT_TRUE(ok_read_back);
    EXPECT_TRUE(ok_restore);
    EXPECT_TRUE(ok_restored_read);
    EXPECT_TRUE(ok_resume);

    EXPECT_NE(captured, 0u) << "a parked thread has a program counter";
    EXPECT_EQ(read_back, captured + 1) << "set_pc did not take effect";
    EXPECT_EQ(restored, captured) << "the inverse of a set_pc is a set_pc back";

    worker.release_and_join();
    EXPECT_EQ(worker.result(), 42) << "the thread continued correctly from its own pc";
}

#else

// Platform-honest rather than silently absent: the surface exists, compiles,
// and refuses, so the engine can refuse the tier at compile time and a caller
// that ignores that still cannot do damage. The shape of the eventual
// implementation is recorded in src/os/posix/threads.cpp (S-17).
TEST(OsThreads, this_os_reports_no_coordination_and_refuses_every_call) {
    EXPECT_FALSE(splice::os::kCoordinationSupported);

    std::vector<ThreadRef> refs(8);
    const auto found = splice::os::enumerate_other_threads(refs);
    EXPECT_EQ(found.status, EnumerateStatus::unsupported);
    EXPECT_EQ(found.count, 0u);

    const ThreadRef any{0x1234};
    std::uint32_t count = 0;
    std::uintptr_t pc = 0;
    EXPECT_FALSE(splice::os::suspend(any, count));
    EXPECT_FALSE(splice::os::resume(any, count));
    EXPECT_FALSE(splice::os::capture_pc(any, pc));
    EXPECT_FALSE(splice::os::set_pc(any, 0x5678));
}

#endif
