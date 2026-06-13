// ─── Splice type traits ────────────────────────────────────────────────────
//
// Compile-time reflection helpers for the fluent API. Ported from
// the predecessor framework v1.4 with minor rename (`function_traits` kept — it is
// already snake_case, matching the 3+1 rule).
//
// FR-009 Step 9.3 adds `member_function_traits` — maps a pointer-to-member
// `Ret(Class::*)(Args...) [cv] [noexcept]` onto the equivalent free-function
// type with an explicit leading `Class*` (the "thiscall → cdecl" view that
// SPLICE_HOOK_MEMBER installs against).
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstddef>
#include <cstring>

namespace splice {

// Primary template — intentionally undefined; only the specializations below
// satisfy the concept of "a function-pointer-like thing".
template <typename T>
struct function_traits;

// Free function pointer: Ret(*)(Args...)
template <typename Ret, typename... Args>
struct function_traits<Ret (*)(Args...)> {
    using return_type = Ret;
    using function_type = Ret (*)(Args...);
    static constexpr std::size_t arity = sizeof...(Args);
};

// Convenience aliases for users who want the pieces individually.
template <typename F>
using return_type_t = typename function_traits<F>::return_type;

template <typename F>
inline constexpr std::size_t arity_v = function_traits<F>::arity;

// ─── remove_function_noexcept ───────────────────────────────────────────────
//
// Strip a trailing `noexcept` from a function-pointer type. Since C++17,
// `noexcept` is part of a function's type, so `decltype(&malloc)` on glibc /
// Bionic (where libc is declared `__THROW`) is `void*(*)(size_t) noexcept` —
// a DIFFERENT type from `void*(*)(size_t)`, which does NOT match the
// non-noexcept partial specialisations of `InterceptorEntry` /
// `TrampolineGenerator` / `function_traits`. The result is an "incomplete
// type" error the moment you hook any libc function on a conforming compiler
// (MSVC's libc headers omit `noexcept`, which is why Windows never tripped).
//
// Normalising the deduced type at the hook-macro boundary lets every such
// function flow through the existing code paths. Calling a `noexcept` function
// through a non-`noexcept` pointer is well-defined (the qualifier only
// constrains the callee, not the call site).
template <typename F>
struct remove_function_noexcept {
    using type = F;
};

template <typename Ret, typename... Args>
struct remove_function_noexcept<Ret (*)(Args...) noexcept> {
    using type = Ret (*)(Args...);
};

template <typename F>
using remove_function_noexcept_t = typename remove_function_noexcept<F>::type;

// ─── member_function_traits ─────────────────────────────────────────────────
//
// Decompose a pointer-to-member-function type and synthesise the
// explicit-`this` free-function pointer Splice's trampoline installs.
//
//   Ret(Class::*)(Args...)   →  free_function_type = Ret(*)(Class*, Args...)
//
// Covers the cv / noexcept qualifier matrix that shows up in practice. The
// `class_type` member exposes the owning class so the macro can spell the
// explicit-this signature and so SPLICE_HOOK_MEMBER can static_assert on a
// real member. The synthesised free-function signature DROPS cv/noexcept —
// the trampoline is a plain `extern "C"`-style free function; the const-ness
// of the original `this` is the caller's concern, not the patch's.
template <typename T>
struct member_function_traits;

// Unqualified specialisation — written by hand because calling a
// function-like macro with an empty argument trips MSVC's /W4 C4003.
template <typename Ret, typename Class, typename... Args>
struct member_function_traits<Ret (Class::*)(Args...)> {
    using return_type        = Ret;
    using class_type         = Class;
    using free_function_type = Ret (*)(Class*, Args...);
    static constexpr std::size_t arity = sizeof...(Args) + 1;   // +this
};

// Macro stamps out the qualified variants (const / noexcept / both) so we
// don't hand-copy three more near-identical bodies. The qualifier is never
// empty here, so no C4003.
#define SPLICE_MEMFN_TRAITS_SPEC(CV_NOEXCEPT)                                  \
    template <typename Ret, typename Class, typename... Args>                  \
    struct member_function_traits<Ret (Class::*)(Args...) CV_NOEXCEPT> {       \
        using return_type        = Ret;                                        \
        using class_type         = Class;                                      \
        using free_function_type = Ret (*)(Class*, Args...);                   \
        static constexpr std::size_t arity = sizeof...(Args) + 1; /* +this */  \
    }

SPLICE_MEMFN_TRAITS_SPEC(const);
SPLICE_MEMFN_TRAITS_SPEC(noexcept);
SPLICE_MEMFN_TRAITS_SPEC(const noexcept);

#undef SPLICE_MEMFN_TRAITS_SPEC

// The explicit-this free-function pointer type for a given Class::method.
template <typename PMF>
using member_free_fn_t = typename member_function_traits<PMF>::free_function_type;

namespace detail {

// Extract the raw code address from a pointer-to-member-function.
//
// LIMITATION (documented in splice-guide §9 and FR-009): this is reliable
// only for NON-VIRTUAL members. On both the Itanium ABI (single
// inheritance) and MSVC, a non-virtual PMF's first pointer-sized field is
// the function's code address — which is exactly what the inline patcher
// needs. A VIRTUAL member's PMF instead stores a vtable offset / thunk, so
// this returns garbage for virtuals. To hook a virtual, resolve the vtable
// slot against a live instance and use SPLICE_HOOK_ADDR(slot_addr).
//
// We memcpy the leading pointer rather than union-cast because MSVC PMFs
// can be wider than void* (multiple/virtual inheritance carries adjustor
// thunks in trailing fields we don't touch).
template <typename PMF>
inline void* member_code_addr(PMF pmf) noexcept {
    static_assert(sizeof(PMF) >= sizeof(void*),
                  "unexpected member-function-pointer layout");
    void* addr = nullptr;
    std::memcpy(&addr, &pmf, sizeof(void*));
    return addr;
}

} // namespace detail

} // namespace splice
