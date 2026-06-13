// ─── FR-009 Step 9.3 — SPLICE_HOOK_MEMBER tests ───────────────────────────
//
// Two layers:
//   1. member_function_traits — pure compile-time type mapping. Runs on
//      every platform (no patching).
//   2. Live member hook — non-virtual method patched via the explicit-this
//      free-function signature. Runs only where a real engine exists.
//
// Virtual-member hooking is deliberately NOT tested here: the PMF for a
// virtual is a vtable offset, not a code address (see member_code_addr
// doc). The macro's static path resolves to garbage for virtuals — that's
// a documented limitation, not a bug, so there's nothing to assert.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/splice.h>
#include <splice/traits.h>

#include <atomic>
#include <type_traits>

namespace {

struct Widget {
    int base = 0;
    // Non-virtual — its PMF carries a real code address. Defined out-of-line
    // (below) with noinline so the hook has a real prologue to patch.
    int scale(int factor);
    int peek() const { return base; }   // only used for the const-trait test
};

} // namespace

// Defined out-of-line so the compiler can't inline it away — the hook needs
// a real prologue to patch.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
int Widget::scale(int factor) {
    return base * factor;
}

// ─── Layer 1 — compile-time trait mapping ─────────────────────────────────

TEST(MemberFunctionTraits, maps_pmf_to_explicit_this_free_fn) {
    using PMF = decltype(&Widget::scale);          // int(Widget::*)(int)
    using Free = splice::member_free_fn_t<PMF>;    // int(*)(Widget*, int)

    static_assert(std::is_same_v<Free, int (*)(Widget*, int)>,
                  "scale should map to int(*)(Widget*, int)");

    using Traits = splice::member_function_traits<PMF>;
    static_assert(std::is_same_v<Traits::return_type, int>);
    static_assert(std::is_same_v<Traits::class_type, Widget>);
    static_assert(Traits::arity == 2);             // this + factor
    SUCCEED();
}

TEST(MemberFunctionTraits, handles_const_qualified_member) {
    using PMF = decltype(&Widget::peek);           // int(Widget::*)() const
    using Free = splice::member_free_fn_t<PMF>;    // int(*)(Widget*)

    static_assert(std::is_same_v<Free, int (*)(Widget*)>,
                  "const member should still map to a plain Widget* free fn");
    static_assert(splice::member_function_traits<PMF>::arity == 1);  // this only
    SUCCEED();
}

// ─── Layer 2 — live member hook (engine-dependent) ────────────────────────
#if (defined(__aarch64__) || defined(__arm64__)) && \
    (defined(__linux__) || defined(__ANDROID__))
#   define SPLICE_MEMBER_HOOK_LIVE 1
#elif defined(__x86_64__) || defined(_M_X64)
#   define SPLICE_MEMBER_HOOK_LIVE 1
#else
#   define SPLICE_MEMBER_HOOK_LIVE 0
#endif

#if SPLICE_MEMBER_HOOK_LIVE

namespace {
std::atomic<int> g_member_hits{0};
}

TEST(HookMember, non_virtual_member_intercepted) {
    Widget w;
    w.base = 10;

    // Baseline.
    EXPECT_EQ(w.scale(3), 30);
    EXPECT_EQ(g_member_hits.load(), 0);

    // Hook scale(): callback receives explicit `Widget* self` then args.
    g_member_hits.store(0);
    SPLICE_HOOK_MEMBER_STATIC(Widget::scale)
        .onInvoke([](auto orig, Widget* self, int factor) {
            g_member_hits.fetch_add(1, std::memory_order_relaxed);
            // Call original, then add a sentinel so we can tell hooked apart.
            return orig(self, factor) + 1000;
        });

    splice::install_all();

    if (splice_is_hooked(reinterpret_cast<void*>(
            splice::detail::member_code_addr<decltype(&Widget::scale)>(
                &Widget::scale)))) {
        EXPECT_EQ(w.scale(3), 1030);            // (10*3) + 1000
        EXPECT_GT(g_member_hits.load(), 0);
    } else {
        GTEST_SKIP() << "member not patchable on this build (stub backend)";
    }
}

#else

TEST(HookMember, disabled_on_unsupported_platform) {
    GTEST_SKIP() << "No live patcher on this platform";
}

#endif
