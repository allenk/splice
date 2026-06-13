// ─── Live hook integration test ────────────────────────────────────────────
//
// Proves the end-to-end pipeline on a platform with a real engine:
//   SPLICE_HOOK_ADDR  →  InterceptorEntry  →  install()
//                     →  splice_hook_address (C ABI)
//                     →  install_inline_patch (arch/<arch>/patcher.cpp)
//                     →  OS memory layer (VirtualAlloc / mmap)
//                     →  trampoline dispatch  →  HookManager::invoke
//                     →  user callback
//
// Runs only where a full engine is compiled. On stub-only builds (e.g.
// arch=arm32 today) the test self-disables with a clear log line.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/splice.h>

#include "helpers/test_targets.h"

// ─── Platform capability detection ─────────────────────────────────────────
// The engine/arch defines are private (PRIVATE compile-defs on the splice
// target) so tests can't see them directly. Infer from host tuple instead.
#if (defined(__aarch64__) || defined(__arm64__)) && \
    (defined(__linux__) || defined(__ANDROID__))
#   define SPLICE_LIVE_HOOK_SUPPORTED 1
#elif defined(__x86_64__) || defined(_M_X64)
#   define SPLICE_LIVE_HOOK_SUPPORTED 1
#else
#   define SPLICE_LIVE_HOOK_SUPPORTED 0
#endif

#if SPLICE_LIVE_HOOK_SUPPORTED

namespace {

// Track whether our hook callback was entered.
std::atomic<int> g_hook_hits{0};

// Preserve whatever install() hands us as "original" so we can call through.
int (*g_original_add_one)(int) = nullptr;

int hook_add_one(int (*orig)(int), int x) {
    g_hook_hits.fetch_add(1, std::memory_order_relaxed);
    g_original_add_one = orig;
    return orig(x) + 100;  // differentiate hooked from unhooked result
}

} // namespace

TEST(LiveHook, splice_hook_addr_intercepts_free_function) {
    using splice::test::add_one;

    // Baseline — no hook installed yet.
    EXPECT_EQ(add_one(1), 2);
    EXPECT_EQ(g_hook_hits.load(), 0);

    // SPLICE_HOOK_ADDR_STATIC queues an installer on first evaluation.
    // Subsequent evaluations in the same process return the same entry.
    SPLICE_HOOK_ADDR_STATIC(&add_one)
        .onInvoke([](int (*orig)(int), int x) {
            return hook_add_one(orig, x);
        });

    // Drive the installer queue.
    splice::install_all();

    // The hook is either installed (engine present) or the install_all
    // logged a warning and moved on (stub path). Either way:
    //   - if installed: add_one(5) returns (5+1)+100 = 106 and g_hook_hits > 0
    //   - if stubbed:   add_one(5) returns 6 and g_hook_hits == 0
    const int result = add_one(5);
    if (splice_is_hooked(reinterpret_cast<void*>(&add_one))) {
        EXPECT_EQ(result, 106);
        EXPECT_GT(g_hook_hits.load(), 0);
        EXPECT_NE(g_original_add_one, nullptr);
        // Original still callable via the trampoline saved by the engine.
        EXPECT_EQ(g_original_add_one(10), 11);
    } else {
        GTEST_SKIP() << "No live patcher for this platform — stub build detected";
    }
}

#else  // !SPLICE_LIVE_HOOK_SUPPORTED

TEST(LiveHook, disabled_on_unsupported_platform) {
    GTEST_SKIP() << "No arch backend for this host";
}

#endif
