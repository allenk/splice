// ─── R2: the controlled boundary window ───────────────────────────────────
//
// The fixture owed since 2026-10-02. S-14 asks what happens to a thread that
// is already at an instruction boundary inside `target+1..4` when a five-byte
// jump is written over it, and the answer has always been "nobody knows,
// because nothing can put a thread there on demand".
//
// It is deterministic once the question is stated correctly. We do not need a
// thread to ARRIVE at `target+k` by luck; we need to know that a thread WHOSE
// PROGRAM COUNTER IS `target+k` is mapped correctly and continues correctly.
// So the thread is parked somewhere harmless, suspended, its program counter
// set to `target+k`, the group installed, and the thread resumed -- and what it
// computes afterwards says where it resumed from.
//
//     for k in 0 .. past the displaced prefix:
//         expect: either a correct mapped continuation, or an unchanged refusal
//         never:  a resumed thread at an address that is no longer an instruction
//
// Both halves of that are asserted, and the two are not interchangeable: an
// interior offset is refused because our decoding says it is not a boundary,
// and a thread is never resumed into one. An interior program counter cannot
// occur in a healthy process at all -- a CPU is always at a boundary of the
// stream it decoded -- so the case it stands for is a prologue somebody else
// has already patched, where our boundaries and the real ones disagree.
//
// ─── why the code here is hand-written machine code ───────────────────────
//
// Two things have to be true of the target that no compiler will promise:
//
//   * **Known instruction boundaries.** The whole test is about offsets. A
//     compiled function's prologue is whatever the optimiser felt like.
//   * **Register tolerance.** Entering at offset k skips every instruction
//     before it, so the register state at k has to be one the remaining
//     instructions can work from. Here the accumulator starts at zero, set by
//     the parking stub, which is exactly what the skipped instructions would
//     have left.
//
//     target:  83 C0 04    add eax, 4      offset 0   \ relocated: copy_size 6
//              83 C0 01    add eax, 1      offset 3   /
//              83 C0 02    add eax, 2      offset 6
//              C3          ret             offset 9
//
// With eax starting at zero, where the thread resumes is legible in what it
// returns: 7 from offset 0, 3 from offset 3, 2 from offset 6. That is the
// measurement -- not "it did not crash", which a wrong mapping passes just as
// easily.
//
// The parking stub is frameless on purpose. Its stack pointer therefore points
// at its own return address for its whole duration, so when the hijacked
// program counter runs the target's `ret`, the thread returns to the C++ caller
// of the stub as if the stub had returned normally -- carrying the accumulator
// out as the stub's return value. A compiled spin loop would have pushed a
// frame and the `ret` would have returned into the middle of its own locals.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/live.h>

#include "os/memory.h"
#include "os/threads.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#if defined(_WIN32)
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#endif

namespace {

using splice::os::ThreadRef;

// ─── the two hand-written functions ───────────────────────────────────────

// add eax,4 / add eax,1 / add eax,2 / ret -- boundaries at 0, 3, 6 and 9.
const unsigned char kTargetCode[] = {
    0x83, 0xC0, 0x04,
    0x83, 0xC0, 0x01,
    0x83, 0xC0, 0x02,
    0xC3,
};
constexpr std::uintptr_t kFirstBoundary  = 0;
constexpr std::uintptr_t kSecondBoundary = 3;
constexpr std::uintptr_t kPastPrefix     = 6;   // outside the displaced prologue
constexpr int kFromFirst  = 7;                  // 4 + 1 + 2
constexpr int kFromSecond = 3;                  //     1 + 2
constexpr int kFromPast   = 2;                  //         2

// xor eax,eax / entered = 1 / pause / cmp release,0 / je spin / ret
//
// Frameless, so the stack pointer points at the return address throughout --
// see the note at the top of this file. The two flag pointers arrive in the
// first two integer argument registers, which differ by ABI.
#if defined(_WIN32)
const unsigned char kParkCode[] = {
    0x31, 0xC0,              // xor eax, eax
    0xC6, 0x01, 0x01,        // mov byte [rcx], 1      -- entered
    0xF3, 0x90,              // pause                  <- spin
    0x80, 0x3A, 0x00,        // cmp byte [rdx], 0      -- release
    0x74, 0xF9,              // je spin
    0xC3,                    // ret
};
#else
const unsigned char kParkCode[] = {
    0x31, 0xC0,              // xor eax, eax
    0xC6, 0x07, 0x01,        // mov byte [rdi], 1      -- entered
    0xF3, 0x90,              // pause                  <- spin
    0x80, 0x3E, 0x00,        // cmp byte [rsi], 0      -- release
    0x74, 0xF9,              // je spin
    0xC3,                    // ret
};
#endif
constexpr std::size_t kParkSpinOffset = 5;    // the pause
constexpr std::size_t kParkSpinEnd    = 12;   // the ret

using ParkFn   = int (*)(volatile char*, volatile char*);
using TargetFn = int (*)();

constexpr std::size_t kBlockSize = 4096;

std::atomic<unsigned long> g_worker_tid{0};
std::atomic<int>           g_witness{-1};
std::atomic<int>           g_callback_hits{0};

// The thread that will be parked and hijacked.
void worker_body(ParkFn park, volatile char* entered, volatile char* release) {
#if defined(_WIN32)
    g_worker_tid.store(::GetCurrentThreadId());
#endif
    g_witness.store(park(entered, release));
}

class BoundaryWindow : public ::testing::Test {
protected:
    void SetUp() override {
        if (!splice::live_tier_available()) {
            GTEST_SKIP() << "the controlled window needs an OS that can suspend "
                            "a thread and move its program counter";
        }
        block = static_cast<unsigned char*>(
            splice::os::allocate_executable_memory(kBlockSize));
        ASSERT_NE(block, nullptr);
        std::memset(block, 0x90, kBlockSize);

        // Eight-byte aligned, because a five-byte patch is installed with one
        // aligned store and the planner refuses a target whose low three bits
        // are above three.
        target = block + 64;
        std::memcpy(target, kTargetCode, sizeof(kTargetCode));
        park = block + 256;
        std::memcpy(park, kParkCode, sizeof(kParkCode));

        g_worker_tid.store(0);
        g_witness.store(-1);
        g_callback_hits.store(0);
        entered = 0;
        release = 0;
    }

    void TearDown() override {
        if (outcome.active()) EXPECT_TRUE(outcome.disable());
        release = 1;
        if (worker.joinable()) worker.join();
        if (block) splice::os::free_executable_memory(block, kBlockSize);
    }

    // Start the worker and wait until it is spinning inside the parking stub.
    void park_the_worker() {
        worker = std::thread(worker_body,
                             reinterpret_cast<ParkFn>(park),
                             &entered, &release);
        while (entered == 0) std::this_thread::yield();
        ASSERT_NE(g_worker_tid.load(), 0u);
    }

    // Open the parked worker, out of the OS's own enumeration -- the same
    // surface the transaction uses, so the test is not holding a handle the
    // transaction cannot see.
    ThreadRef open_worker() {
        refs.assign(512, ThreadRef{});
        const auto enumeration =
            splice::os::enumerate_other_threads(std::span<ThreadRef>(refs));
        EXPECT_EQ(enumeration.status, splice::os::EnumerateStatus::ok);
        ThreadRef found{};
        for (std::size_t i = 0; i < enumeration.count; ++i) {
            if (splice::os::thread_id(refs[i]) == g_worker_tid.load()) {
                found = refs[i];
            }
        }
        opened = enumeration.count;
        return found;
    }

    void release_refs() {
        for (std::size_t i = 0; i < opened; ++i) splice::os::release_thread(refs[i]);
        refs.clear();
        opened = 0;
    }

    // Install the group on the hand-written target.
    splice::Outcome install() {
        splice::Live<> group;
        return group
            .at(static_cast<void*>(target), TargetFn{},
                [](auto orig) {
                    g_callback_hits.fetch_add(1);
                    return orig() + 1000;
                })
            .install(splice::live);
    }

    unsigned char* block = nullptr;
    unsigned char* target = nullptr;
    unsigned char* park = nullptr;
    volatile char  entered = 0;
    volatile char  release = 0;
    std::thread    worker;
    std::vector<ThreadRef> refs;
    std::size_t    opened = 0;
    splice::Outcome outcome;
};

} // namespace

// ─── a thread at a boundary inside the bytes being replaced ───────────────

TEST_F(BoundaryWindow, a_thread_at_the_first_boundary_continues_correctly) {
    park_the_worker();
    ThreadRef ref = open_worker();
    ASSERT_TRUE(static_cast<bool>(ref));

    std::uint32_t prior = 0;
    ASSERT_TRUE(splice::os::suspend(ref, prior));
    std::uintptr_t parked_pc = 0;
    ASSERT_TRUE(splice::os::capture_pc(ref, parked_pc));
    const auto park_base = reinterpret_cast<std::uintptr_t>(park);
    EXPECT_GE(parked_pc, park_base + kParkSpinOffset);
    EXPECT_LT(parked_pc, park_base + kParkSpinEnd);

    const auto target_base = reinterpret_cast<std::uintptr_t>(target);
    ASSERT_TRUE(splice::os::set_pc(ref, target_base + kFirstBoundary));

    outcome = install();
    ASSERT_EQ(outcome.state(), splice::State::installed)
        << "stage=" << static_cast<int>(outcome.failure().stage)
        << " op=" << static_cast<int>(outcome.failure().operation);

    // The program counter moved out of the bytes that were replaced. This is
    // the mapping, observed rather than assumed: a thread left at
    // target+0 would now be standing on the first byte of a jump.
    std::uintptr_t mapped_pc = 0;
    ASSERT_TRUE(splice::os::capture_pc(ref, mapped_pc));
    EXPECT_NE(mapped_pc, target_base + kFirstBoundary);
    EXPECT_TRUE(mapped_pc < target_base || mapped_pc >= target_base + 16)
        << "the thread is still inside the patched prologue";

    std::uint32_t resumed = 0;
    ASSERT_TRUE(splice::os::resume(ref, resumed));
    release_refs();

    release = 1;
    worker.join();
    EXPECT_EQ(g_witness.load(), kFromFirst)
        << "the thread did not resume at the relocated first instruction";
    EXPECT_EQ(g_callback_hits.load(), 0) << "a thread already inside the "
                                            "function does not go through the hook";

    // And the hook itself works, for a call that arrives after the install.
    //
    // Only that it fired is asserted, not what it returned. The target's
    // accumulator starts from whatever eax holds, which is the whole reason
    // the parking stub zeroes it -- and a call that goes through the hook
    // passes through a C++ stub first, which is free to use eax for anything.
    // The arithmetic is a witness for the hijacked thread and for nothing else.
    auto fn = reinterpret_cast<TargetFn>(target);
    (void)fn();
    EXPECT_EQ(g_callback_hits.load(), 1);
}

TEST_F(BoundaryWindow, a_thread_at_the_second_boundary_continues_from_there) {
    park_the_worker();
    ThreadRef ref = open_worker();
    ASSERT_TRUE(static_cast<bool>(ref));

    std::uint32_t prior = 0;
    ASSERT_TRUE(splice::os::suspend(ref, prior));
    const auto target_base = reinterpret_cast<std::uintptr_t>(target);
    ASSERT_TRUE(splice::os::set_pc(ref, target_base + kSecondBoundary));

    outcome = install();
    ASSERT_EQ(outcome.state(), splice::State::installed);

    std::uintptr_t mapped_pc = 0;
    ASSERT_TRUE(splice::os::capture_pc(ref, mapped_pc));
    EXPECT_NE(mapped_pc, target_base + kSecondBoundary);

    std::uint32_t resumed = 0;
    ASSERT_TRUE(splice::os::resume(ref, resumed));
    release_refs();

    release = 1;
    worker.join();
    // 3, not 7: it resumed at the SECOND relocated instruction, so the first
    // one is not re-run. A mapping that pointed at the start of the trampoline
    // would give 7 and would look like a pass to a test that only asked
    // whether the thread survived.
    EXPECT_EQ(g_witness.load(), kFromSecond);
}

TEST_F(BoundaryWindow, a_thread_past_the_displaced_prologue_is_left_where_it_is) {
    park_the_worker();
    ThreadRef ref = open_worker();
    ASSERT_TRUE(static_cast<bool>(ref));

    std::uint32_t prior = 0;
    ASSERT_TRUE(splice::os::suspend(ref, prior));
    const auto target_base = reinterpret_cast<std::uintptr_t>(target);
    ASSERT_TRUE(splice::os::set_pc(ref, target_base + kPastPrefix));

    outcome = install();
    ASSERT_EQ(outcome.state(), splice::State::installed);

    // Nothing to map: those bytes were not touched, so the right answer is to
    // leave the program counter alone. Moving it would be the bug.
    std::uintptr_t mapped_pc = 0;
    ASSERT_TRUE(splice::os::capture_pc(ref, mapped_pc));
    EXPECT_EQ(mapped_pc, target_base + kPastPrefix);

    std::uint32_t resumed = 0;
    ASSERT_TRUE(splice::os::resume(ref, resumed));
    release_refs();

    release = 1;
    worker.join();
    EXPECT_EQ(g_witness.load(), kFromPast);
}

// ─── a program counter our decoding says is not a boundary ────────────────

TEST_F(BoundaryWindow, an_interior_program_counter_refuses_and_writes_nothing) {
    // Offsets 1, 2, 4 and 5 are inside an instruction according to the
    // snapshot the plan decoded. A real thread cannot be there -- a CPU is
    // always at a boundary of the stream it decoded -- so what this stands for
    // is a prologue somebody else has already patched, where their boundaries
    // and ours disagree. The only safe answer is to write nothing.
    //
    // The thread is NOT resumed into such an address afterwards. It is put
    // back where it was parked, because an interior program counter is an
    // impossible state this test manufactured, and running it would be
    // executing the middle of an instruction on purpose.
    for (const std::uintptr_t k : {std::uintptr_t{1}, std::uintptr_t{2},
                                   std::uintptr_t{4}, std::uintptr_t{5}}) {
        SCOPED_TRACE("offset " + std::to_string(k));

        g_worker_tid.store(0);
        g_witness.store(-1);
        entered = 0;
        release = 0;
        park_the_worker();

        ThreadRef ref = open_worker();
        ASSERT_TRUE(static_cast<bool>(ref));
        std::uint32_t prior = 0;
        ASSERT_TRUE(splice::os::suspend(ref, prior));
        std::uintptr_t parked_pc = 0;
        ASSERT_TRUE(splice::os::capture_pc(ref, parked_pc));

        const auto target_base = reinterpret_cast<std::uintptr_t>(target);
        ASSERT_TRUE(splice::os::set_pc(ref, target_base + k));

        unsigned char before[16];
        std::memcpy(before, target, sizeof(before));

        splice::Outcome refused = install();

        EXPECT_EQ(refused.state(), splice::State::refused);
        EXPECT_EQ(refused.failure().stage, splice::Stage::map);
        EXPECT_EQ(refused.failure().thread_id, g_worker_tid.load());
        EXPECT_EQ(std::memcmp(before, target, sizeof(before)), 0)
            << "the target was written despite the refusal";

        // The refusal resumed every thread it suspended, so our own suspension
        // is the only one left -- the count it reports proves the transaction
        // balanced its own.
        std::uintptr_t after_pc = 0;
        ASSERT_TRUE(splice::os::capture_pc(ref, after_pc));
        EXPECT_EQ(after_pc, target_base + k) << "a refusal moved the thread";

        ASSERT_TRUE(splice::os::set_pc(ref, parked_pc));
        std::uint32_t resumed = 0;
        ASSERT_TRUE(splice::os::resume(ref, resumed));
        EXPECT_EQ(resumed, prior + 1) << "the transaction left a suspension behind";
        release_refs();

        release = 1;
        worker.join();
        EXPECT_EQ(g_witness.load(), 0) << "the parking stub did not return "
                                          "normally after the refusal";
    }
}

// ─── the check/write window, closed by construction ───────────────────────

TEST_F(BoundaryWindow, a_prologue_changed_between_prepare_and_commit_is_refused) {
    // S-18's shape: the bytes are compared, and then written. The coordinated
    // tier closes that window by suspending everything that could write inside
    // it, so the only writer left is the installing thread -- which is what
    // this drives, through the C ABI, in the one place where "between prepare
    // and commit" is expressible.
    //
    // What it proves is that the second comparison exists and is acted on: the
    // plan refuses to commit over bytes that are no longer the ones it
    // snapshotted and relocated. What it cannot prove is the absence of a
    // foreign writer, because inside this transaction there is nowhere for one
    // to run -- and that is the argument, not the test.
    splice_live_site site{};
    site.target = static_cast<void*>(target);
    site.replacement = static_cast<void*>(park);   // never called on this path
    site.expected = nullptr;

    void* originals[1] = {nullptr};
    splice_live_outcome engine_outcome{};
    void* handle = splice_live_prepare(&site, 1, originals, &engine_outcome);
    ASSERT_NE(handle, nullptr);
    ASSERT_NE(originals[0], nullptr);

    // The disagreement. In the field it is another hooking library; here it is
    // one byte, which is all it takes.
    target[0] = 0x90;

    splice_patch_record records[1]{};
    splice_live_commit(handle, records, &engine_outcome);
    splice_live_release(handle);

    EXPECT_EQ(engine_outcome.state, SPLICE_LIVE_RECOVERED);
    EXPECT_EQ(engine_outcome.stage, SPLICE_LIVE_STAGE_WRITE);
    EXPECT_EQ(engine_outcome.operation, SPLICE_LIVE_OP_WRITE_SITE);
    EXPECT_EQ(engine_outcome.site, 0u);

    // The mutation is still there and nothing was installed over it. A patch
    // written anyway would have replaced these five bytes with a jump.
    EXPECT_EQ(target[0], 0x90);
    EXPECT_EQ(std::memcmp(target + 1, kTargetCode + 1, sizeof(kTargetCode) - 1), 0);
}
