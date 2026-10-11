// ─── R4: a real COM interface, in a module we did not build ───────────────
//
// Design section 8.1 asks for the live tier to be proved against real COM
// before the overlay is asked again, and `IMalloc` is the first interface worth
// pointing it at: a real system object with a real vtable in `ole32.dll`,
// present on every Windows, with methods cheap enough to call millions of
// times, and needing no device, no window and no message pump.
//
// It exercises the whole chain against something nobody here wrote -- pointer-
// to-member to slot index under the Microsoft ABI, the group transaction, the
// re-entrancy gate, disable -- and it is the honest stress case for hooking,
// because `Alloc` is the allocator. The transaction must not touch an allocator
// while threads are suspended, and here the allocator is also the target.
//
// What this adds over the portable runner: that interface's table is in a
// module the test built itself. This one's is in a Microsoft binary, laid out
// by a compiler we do not control, with slot indices that are a published ABI
// rather than something we chose. If the resolver disagreed with COM, this is
// where it would show.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/live.h>

#if defined(_WIN32)

#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#   include <objbase.h>
#   include <psapi.h>

#   include <atomic>
#   include <cstring>
#   include <thread>
#   include <vector>

namespace {

std::atomic<long long> g_alloc_calls{0};
std::atomic<long long> g_free_calls{0};
std::atomic<long long> g_getsize_calls{0};

template <typename PMF>
int slot_of(PMF pmf) {
    unsigned char raw[sizeof(PMF)];
    std::memcpy(raw, &pmf, sizeof(PMF));
    return splice_vtable_slot_of_pmf(raw, static_cast<unsigned int>(sizeof(PMF)));
}

// The COM apartment and the task allocator, for the whole suite. CoGetMalloc
// hands back the same singleton every time, so its vtable is shared with
// everything in the process that allocates task memory -- which is the point.
class ComMalloc : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        ASSERT_EQ(::CoInitializeEx(nullptr, COINIT_MULTITHREADED), S_OK);
        ASSERT_EQ(::CoGetMalloc(1, &s_malloc), S_OK);
        ASSERT_NE(s_malloc, nullptr);
    }

    static void TearDownTestSuite() {
        if (s_malloc != nullptr) {
            s_malloc->Release();
            s_malloc = nullptr;
        }
        ::CoUninitialize();
    }

    void SetUp() override {
        g_alloc_calls.store(0);
        g_free_calls.store(0);
        g_getsize_calls.store(0);
    }

    void TearDown() override {
        if (outcome.active()) EXPECT_TRUE(outcome.disable());
    }

    // The provider's own table entry, read through the object rather than
    // through splice, so a slot the resolver reports can be compared with what
    // COM actually has there.
    static void* table_entry(int index) {
        void** table = nullptr;
        std::memcpy(&table, static_cast<void*>(s_malloc), sizeof(table));
        return table[index];
    }

    static IMalloc*        s_malloc;
    splice::Outcome        outcome;
};

IMalloc* ComMalloc::s_malloc = nullptr;

} // namespace

// ─── what the resolver says about a published ABI ─────────────────────────

TEST_F(ComMalloc, the_resolver_agrees_with_the_com_abi) {
    // IUnknown occupies slots 0 to 2 and IMalloc's own methods follow in
    // declaration order. These numbers are not ours to choose -- they are the
    // layout every COM client in the world depends on -- so they are asserted
    // as literals. A resolver that drifted would be wrong here and nowhere
    // else in this suite.
    EXPECT_EQ(slot_of(&IMalloc::Alloc), 3);
    EXPECT_EQ(slot_of(&IMalloc::Realloc), 4);
    EXPECT_EQ(slot_of(&IMalloc::Free), 5);
    EXPECT_EQ(slot_of(&IMalloc::GetSize), 6);
    EXPECT_EQ(slot_of(&IMalloc::DidAlloc), 7);
    EXPECT_EQ(slot_of(&IMalloc::HeapMinimize), 8);

    // And the addresses behind them are in somebody else's module.
    //
    // Which module is asked rather than assumed, and the answer is worth
    // writing down: this test first asserted ole32.dll and failed, because the
    // entries sit about six megabytes BELOW ole32's base. COM's core moved to
    // combase.dll years ago and ole32 forwards to it, so the implementation
    // behind CoGetMalloc is not in the DLL whose name the API carries. Pinning
    // a module name would have been pinning a Windows implementation detail;
    // what matters is that it is not ours.
    HMODULE self = nullptr;
    ASSERT_TRUE(::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCWSTR>(&slot_of<decltype(&IMalloc::Alloc)>),
                                     &self));

    for (const int index : {3, 5, 6}) {
        void* const fn = table_entry(index);
        ASSERT_NE(fn, nullptr) << "slot " << index;
        HMODULE owner = nullptr;
        ASSERT_TRUE(::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         reinterpret_cast<LPCWSTR>(fn), &owner))
            << "slot " << index << " does not lie in any loaded module";
        EXPECT_NE(owner, self) << "slot " << index << " is in our own image, "
                                  "which would make this runner prove nothing";
        wchar_t name[MAX_PATH]{};
        if (::GetModuleFileNameW(owner, name, MAX_PATH) != 0) {
            SPLICE_LOGI("IMalloc slot %d is in %ls", index, name);
        }
    }
}

// ─── the slot_swap tier on a live system interface ────────────────────────

TEST_F(ComMalloc, three_methods_hooked_as_one_group) {
    outcome = splice::Vtable<IMalloc>{s_malloc}
        .on(&IMalloc::Alloc, [](auto orig, IMalloc* self, SIZE_T size) {
            g_alloc_calls.fetch_add(1, std::memory_order_relaxed);
            return orig(self, size);
        })
        .on(&IMalloc::Free, [](auto orig, IMalloc* self, void* block) {
            g_free_calls.fetch_add(1, std::memory_order_relaxed);
            orig(self, block);
        })
        .on(&IMalloc::GetSize, [](auto orig, IMalloc* self, void* block) {
            g_getsize_calls.fetch_add(1, std::memory_order_relaxed);
            return orig(self, block);
        })
        .non_reentrant()
        .install(splice::slot_swap);

    ASSERT_EQ(outcome.state(), splice::State::installed)
        << "stage=" << static_cast<int>(outcome.failure().stage)
        << " site=" << outcome.failure().site.ordinal;
    EXPECT_EQ(outcome.installed_sites(), 3u);

    void* block = s_malloc->Alloc(64);
    ASSERT_NE(block, nullptr);
    const SIZE_T size = s_malloc->GetSize(block);
    EXPECT_GE(size, 64u);
    std::memset(block, 0xAB, 64);          // the memory is real and writable
    s_malloc->Free(block);

    EXPECT_GE(g_alloc_calls.load(), 1);
    EXPECT_GE(g_getsize_calls.load(), 1);
    EXPECT_GE(g_free_calls.load(), 1);

    ASSERT_TRUE(outcome.disable());
    const long long after_disable = g_alloc_calls.load();
    void* second = s_malloc->Alloc(64);
    ASSERT_NE(second, nullptr);
    s_malloc->Free(second);
    EXPECT_EQ(g_alloc_calls.load(), after_disable)
        << "the callback still ran after disable";
}

// ─── the same interface under load ────────────────────────────────────────

TEST_F(ComMalloc, threads_hammer_the_allocator_across_the_install) {
    // The install happens while workers are inside Alloc and Free. On the
    // slot_swap tier the write is one atomic pointer store, so a worker either
    // loads the old slot value or the new one -- and every allocation it gets
    // back has to be usable either way. That is the assertion: not "it did not
    // crash", but "every block came back writable and the right size".
    constexpr int kThreads = 8;
    std::atomic<int>       stop{0};
    std::atomic<int>       ready{0};
    std::atomic<long long> loops{0};
    std::atomic<int>       bad{0};

    auto worker = [&] {
        ready.fetch_add(1, std::memory_order_release);
        while (stop.load(std::memory_order_relaxed) == 0) {
            void* p = s_malloc->Alloc(48);
            if (p == nullptr) {
                bad.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (s_malloc->GetSize(p) < 48u) {
                bad.fetch_add(1, std::memory_order_relaxed);
            }
            std::memset(p, 0x5A, 48);
            s_malloc->Free(p);
            loops.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) threads.emplace_back(worker);
    while (ready.load(std::memory_order_acquire) < kThreads) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    outcome = splice::Vtable<IMalloc>{s_malloc}
        .on(&IMalloc::Alloc, [](auto orig, IMalloc* self, SIZE_T size) {
            g_alloc_calls.fetch_add(1, std::memory_order_relaxed);
            return orig(self, size);
        })
        .on(&IMalloc::Free, [](auto orig, IMalloc* self, void* block) {
            g_free_calls.fetch_add(1, std::memory_order_relaxed);
            orig(self, block);
        })
        .non_reentrant()
        .install(splice::slot_swap);
    ASSERT_EQ(outcome.state(), splice::State::installed);

    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    stop.store(1, std::memory_order_release);
    for (auto& t : threads) t.join();

    EXPECT_EQ(bad.load(), 0) << "an allocation came back unusable";
    EXPECT_GT(loops.load(), 1000LL) << "the workers did not iterate enough";
    EXPECT_GT(g_alloc_calls.load(), 0);
    EXPECT_GT(g_free_calls.load(), 0);
    SPLICE_LOGI("ComMalloc: %lld loops across %d threads, %lld hooked allocs",
                loops.load(), kThreads, g_alloc_calls.load());
}

// ─── what the coordinated tier makes of a Microsoft prologue ──────────────

TEST_F(ComMalloc, the_live_tier_on_a_system_function_is_measured_not_assumed) {
    // Whether ole32's allocator has a prologue this planner can relocate is a
    // property of a Microsoft binary, not of splice, and it is not something to
    // assert blind. So this case accepts both answers and pins the thing that
    // must hold either way: the interface still works afterwards, and a refusal
    // writes nothing.
    //
    // It is also the case the design called the honest stress test for hooking.
    // The target IS the allocator; the transaction may not touch one while
    // threads are suspended, which is why every allocation it needs happens in
    // prepare, before anything is frozen.
    void* const before = table_entry(3);

    splice::Outcome live = splice::Vtable<IMalloc>{s_malloc}
        .on(&IMalloc::Alloc, [](auto orig, IMalloc* self, SIZE_T size) {
            g_alloc_calls.fetch_add(1, std::memory_order_relaxed);
            return orig(self, size);
        })
        .non_reentrant()
        .install(splice::live);

    SPLICE_LOGI("ComMalloc live tier: state=%d stage=%d op=%d err=%u",
                static_cast<int>(live.state()),
                static_cast<int>(live.failure().stage),
                static_cast<int>(live.failure().operation),
                live.failure().os_error);

    // The vtable slot is untouched on this tier whatever the outcome: it
    // patches the function, never the table.
    EXPECT_EQ(table_entry(3), before);

    if (live.state() == splice::State::installed) {
        void* block = s_malloc->Alloc(32);
        ASSERT_NE(block, nullptr);
        std::memset(block, 0x11, 32);
        s_malloc->Free(block);
        EXPECT_GE(g_alloc_calls.load(), 1)
            << "the inline patch installed but the callback never ran";
        ASSERT_TRUE(live.disable());
    } else {
        // A refusal has to leave the allocator working. This is the assertion
        // that matters for a refusal on a system function: nothing was half
        // written into ole32.
        EXPECT_NE(live.state(), splice::State::lost);
        EXPECT_EQ(live.installed_sites(), 0u);
    }

    void* after = s_malloc->Alloc(32);
    ASSERT_NE(after, nullptr) << "the allocator stopped working";
    s_malloc->Free(after);
}

#else   // !_WIN32

TEST(ComMalloc, needs_com) {
    GTEST_SKIP() << "IMalloc is a Windows interface; the portable runner in "
                    "test_vtable_portable.cpp covers the same chain elsewhere";
}

#endif
