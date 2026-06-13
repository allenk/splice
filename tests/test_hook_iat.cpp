// ─── IAT hook integration test (Phase 4b) ─────────────────────────────────
//
// Verifies the Tier-1 Win32 hook path:
//   SPLICE_HOOK_ADDR  →  unwrap_jump_stub  →  find_iat_entry_for_address
//                     →  patch_iat_entry (atomic pointer swap)
//                     →  subsequent imports through the IAT resolve to
//                        our hook.
//
// Uses GetTickCount from kernel32 — always imported when we actually
// reference it. Zero side effects, no UI, returns a plain DWORD.
// Windows-only; a skip stub runs elsewhere.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/splice.h>

#if defined(_WIN32)

#ifndef NOMINMAX
#   define NOMINMAX
#endif
#include <windows.h>

#include <atomic>

namespace {

// Tracks how many times our hook fires.
std::atomic<int> g_tick_hits{0};

// Sentinel value the hook returns — distinct from any real uptime tick.
constexpr DWORD kSentinelTick = 0xC0FFEEu;

// Save the original function pointer for the call-through test path.
DWORD (WINAPI * g_real_GetTickCount)() = nullptr;

// Replacement — same signature and calling convention as the real API.
DWORD WINAPI my_GetTickCount() {
    g_tick_hits.fetch_add(1, std::memory_order_relaxed);
    return kSentinelTick;
}

// Indirection so the compiler actually emits a `call qword ptr
// [__imp_GetTickCount]` through the IAT (a direct inline GetTickCount()
// at the call site can be recognised as a pure import by the optimiser,
// but routing through a function ensures the call goes through the IAT
// slot regardless of build config).
DWORD invoke_GetTickCount_via_iat() {
    return ::GetTickCount();
}

} // namespace

TEST(IatHook, installs_via_addr_and_redirects_import_call) {
    g_tick_hits.store(0);

    // Warm-up call — proves the API works and the IAT is populated.
    const DWORD baseline = invoke_GetTickCount_via_iat();
    EXPECT_NE(baseline, kSentinelTick);

    // Remember the real address so the smoke assertion below can compare.
    HMODULE k32 = ::GetModuleHandleA("kernel32.dll");
    ASSERT_NE(k32, nullptr);
    auto* real = reinterpret_cast<void*>(::GetProcAddress(k32, "GetTickCount"));
    ASSERT_NE(real, nullptr);
    g_real_GetTickCount = reinterpret_cast<DWORD (WINAPI *)()>(real);

    // Hook via the public fluent API — engine picks the IAT path
    // (Tier 1) when find_iat_entry_for_address succeeds. Capture the
    // entry reference so we can call .disable() at the end.
    auto& entry = SPLICE_HOOK_ADDR_STATIC(&::GetTickCount)
        .onInvoke([](auto /*orig*/) -> DWORD {
            return my_GetTickCount();
        });
    splice::install_all();
    EXPECT_TRUE(entry.is_installed());

    // The import call should now resolve to our hook.
    const DWORD hooked = invoke_GetTickCount_via_iat();
    EXPECT_EQ(hooked, kSentinelTick);
    EXPECT_GT(g_tick_hits.load(), 0);

    // A direct call through the saved real address still returns a real
    // tick — proving we didn't clobber kernel32 itself.
    if (g_real_GetTickCount != nullptr) {
        const DWORD raw = g_real_GetTickCount();
        EXPECT_NE(raw, kSentinelTick);
    }

    // ── FR-013 Tier 1 disable round-trip ─────────────────────────────────
    // After disable(), the IAT slot should hold the original kernel32
    // pointer again, so import calls no longer reach our hook.
    g_tick_hits.store(0);
    EXPECT_TRUE(entry.disable());
    EXPECT_FALSE(entry.is_installed());

    const DWORD post_disable = invoke_GetTickCount_via_iat();
    EXPECT_NE(post_disable, kSentinelTick)
        << "After disable(), import call still routed to hook";
    EXPECT_EQ(g_tick_hits.load(), 0)
        << "After disable(), hook callback was still invoked";
}

#else  // !_WIN32

TEST(IatHook, disabled_off_windows) {
    GTEST_SKIP() << "IAT hooking is Win32-only";
}

#endif
