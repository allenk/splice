// ─── Splice public macros ──────────────────────────────────────────────────
//
// User-facing entry points. Each macro expands to an IIFE that returns a
// reference to a static/thread_local `splice::InterceptorEntry<>` instance,
// enabling chaining like:
//
//   SPLICE_HOOK(lib, func).onInvoke([](auto orig, auto... args){ ... });
//
// Arity dispatch trick (`SPLICE_GET_MACRO`) routes:
//   SPLICE_HOOK(func)         → single-arg, takes &func directly
//   SPLICE_HOOK(lib, func)    → two-arg, resolves via dlsym at install time
//
// The SPLICE_UNIQUE_ID macro generates unique compile-time ids per call site,
// letting the template system synthesise a dedicated trampoline function
// per hook without the user naming it.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <splice/core.h>

// ─── arity-dispatch plumbing ────────────────────────────────────────────────
#define SPLICE_GET_MACRO(_1, _2, NAME, ...) NAME

// ─── Per-call-site UniqueId ─────────────────────────────────────────────────
// SPLICE_UNIQUE_ID alone is per-TU (starts at 0 in every file), so the first
// SPLICE_HOOK in any two TUs would instantiate the SAME
// TrampolineGenerator<FuncType, 0, Policy> — ODR-deduplicated to one
// trampoline address. Two patched functions then jump to the SAME trampoline,
// and re-install of any one corrupts both via the shared OriginalsRegistry
// slot (last-writer-wins → recursive trampoline → infinite loop).
//
// Composing __LINE__ << 16 | SPLICE_UNIQUE_ID keeps the id 32-bit-safe (line
// numbers fit in 16 bits in any real file, counter likewise) and is
// effectively unique across translation units — two SPLICE_HOOK macros
// would need to land on the SAME source line AND have the same SPLICE_UNIQUE_ID
// progression in distinct TUs to collide.
//
// `(... & 0xFFFF)` clamps just in case SPLICE_UNIQUE_ID ever exceeds 16 bits
// (it won't in practice; default cap is per-TU at ~32K).
#define SPLICE_UNIQUE_ID ((__LINE__ << 16) | (__COUNTER__ & 0xFFFF))

// MSVC quirk: a local `using FuncType = SPLICE_DEDUCE_FN(funcname)` inside a lambda
// inside a TEST() body confuses the partial-specialisation matcher of
// `splice::TrampolineGenerator`. Pass `SPLICE_DEDUCE_FN(funcname)` directly to the
// templates and skip the alias — slightly more verbose macro, no surprises.
//
// `remove_function_noexcept_t` normalises away a trailing `noexcept` (part of
// the type since C++17) so libc/Bionic functions declared `__THROW` — e.g.
// glibc `malloc` is `void*(*)(size_t) noexcept` — match the non-noexcept
// template specialisations. No-op for ordinary signatures. See traits.h.
#define SPLICE_DEDUCE_FN(funcname)  splice::remove_function_noexcept_t<decltype(&funcname)>
#define SPLICE_DEDUCE_PTR(func_ptr) splice::remove_function_noexcept_t<decltype(func_ptr)>

// ─── 2-arg form: explicit lib + func name ───────────────────────────────────
#define SPLICE_HOOK_IMPL(libname, funcname, uniqueId)                                        \
    [&]() -> splice::InterceptorEntry<SPLICE_DEDUCE_FN(funcname)>& {                                \
        static thread_local splice::InterceptorEntry<SPLICE_DEDUCE_FN(funcname)> entry(             \
            libname, #funcname, #funcname, uniqueId,                                         \
            splice::TrampolineGenerator<SPLICE_DEDUCE_FN(funcname), uniqueId>::get_trampoline_ptr());\
        return entry;                                                                        \
    }()

#define SPLICE_HOOK_STATIC_IMPL(libname, funcname, uniqueId)                                 \
    [&]() -> splice::InterceptorEntry<SPLICE_DEDUCE_FN(funcname)>& {                                \
        static splice::InterceptorEntry<SPLICE_DEDUCE_FN(funcname)> entry(                          \
            libname, #funcname, #funcname, uniqueId,                                         \
            splice::TrampolineGenerator<SPLICE_DEDUCE_FN(funcname), uniqueId>::get_trampoline_ptr());\
        return entry;                                                                        \
    }()

// ─── 1-arg form: auto-resolve via &func (compile-time address) ──────────────
// Works when `funcname` is a linker-visible symbol. For dlopen'd libraries,
// use the 2-arg form instead.
#define SPLICE_HOOK_AUTO_IMPL(funcname, uniqueId)                                            \
    [&]() -> splice::InterceptorEntry<SPLICE_DEDUCE_FN(funcname)>& {                                \
        static thread_local splice::InterceptorEntry<SPLICE_DEDUCE_FN(funcname)> entry(             \
            nullptr, reinterpret_cast<void*>(&funcname), #funcname, uniqueId,                \
            splice::TrampolineGenerator<SPLICE_DEDUCE_FN(funcname), uniqueId>::get_trampoline_ptr());\
        return entry;                                                                        \
    }()

#define SPLICE_HOOK_AUTO_STATIC_IMPL(funcname, uniqueId)                                     \
    [&]() -> splice::InterceptorEntry<SPLICE_DEDUCE_FN(funcname)>& {                                \
        static splice::InterceptorEntry<SPLICE_DEDUCE_FN(funcname)> entry(                          \
            nullptr, reinterpret_cast<void*>(&funcname), #funcname, uniqueId,                \
            splice::TrampolineGenerator<SPLICE_DEDUCE_FN(funcname), uniqueId>::get_trampoline_ptr());\
        return entry;                                                                        \
    }()

// ─── Direct-address form: SPLICE_HOOK_ADDR(ptr) ─────────────────────────────
#define SPLICE_HOOK_ADDR_IMPL(func_ptr, uniqueId)                                            \
    [&]() -> splice::InterceptorEntry<SPLICE_DEDUCE_PTR(func_ptr)>& {                                 \
        static thread_local splice::InterceptorEntry<SPLICE_DEDUCE_PTR(func_ptr)> entry(              \
            nullptr, reinterpret_cast<void*>(func_ptr), "direct_addr", uniqueId,             \
            splice::TrampolineGenerator<SPLICE_DEDUCE_PTR(func_ptr), uniqueId>::get_trampoline_ptr());\
        return entry;                                                                        \
    }()

#define SPLICE_HOOK_ADDR_STATIC_IMPL(func_ptr, uniqueId)                                     \
    [&]() -> splice::InterceptorEntry<SPLICE_DEDUCE_PTR(func_ptr)>& {                                 \
        static splice::InterceptorEntry<SPLICE_DEDUCE_PTR(func_ptr)> entry(                           \
            nullptr, reinterpret_cast<void*>(func_ptr), "direct_addr", uniqueId,             \
            splice::TrampolineGenerator<SPLICE_DEDUCE_PTR(func_ptr), uniqueId>::get_trampoline_ptr());\
        return entry;                                                                        \
    }()

// ─── Member-function form: SPLICE_HOOK_MEMBER(Class::method) ────────────────
// FR-009 Step 9.3. Deduces the explicit-`this` free-function signature
//   Ret(Class::*)(Args...)  →  Ret(*)(Class*, Args...)
// via member_function_traits, extracts the code address from the PMF, and
// installs through the direct-address path.
//
//   SPLICE_HOOK_MEMBER(Widget::draw)
//       .onInvoke([](auto orig, Widget* self, int layer) {
//           return orig(self, layer);
//       });
//
// The callback receives `Class* self` as the first explicit argument — the
// implicit `this` made visible. **NON-VIRTUAL members only** (see
// member_code_addr doc): a virtual method's PMF is a vtable offset, not a
// code address. To hook a virtual, resolve the vtable slot on a live
// instance and use SPLICE_HOOK_ADDR(slot).
#define SPLICE_HOOK_MEMBER_IMPL(method, uniqueId)                                            \
    [&]() -> splice::InterceptorEntry<                                                       \
                 splice::member_free_fn_t<decltype(&method)>>& {                             \
        using FreeFn = splice::member_free_fn_t<decltype(&method)>;                          \
        static thread_local splice::InterceptorEntry<FreeFn> entry(                          \
            nullptr,                                                                         \
            splice::detail::member_code_addr<decltype(&method)>(&method),                    \
            #method, uniqueId,                                                               \
            splice::TrampolineGenerator<FreeFn, uniqueId>::get_trampoline_ptr());            \
        return entry;                                                                        \
    }()

#define SPLICE_HOOK_MEMBER_STATIC_IMPL(method, uniqueId)                                     \
    [&]() -> splice::InterceptorEntry<                                                       \
                 splice::member_free_fn_t<decltype(&method)>>& {                             \
        using FreeFn = splice::member_free_fn_t<decltype(&method)>;                          \
        static splice::InterceptorEntry<FreeFn> entry(                                       \
            nullptr,                                                                         \
            splice::detail::member_code_addr<decltype(&method)>(&method),                    \
            #method, uniqueId,                                                               \
            splice::TrampolineGenerator<FreeFn, uniqueId>::get_trampoline_ptr());            \
        return entry;                                                                        \
    }()

// ─── Public macros ──────────────────────────────────────────────────────────
#define SPLICE_HOOK_1(funcname)          SPLICE_HOOK_AUTO_IMPL(funcname, SPLICE_UNIQUE_ID)
#define SPLICE_HOOK_2(libname, funcname) SPLICE_HOOK_IMPL(libname, funcname, SPLICE_UNIQUE_ID)
#define SPLICE_HOOK(...) SPLICE_GET_MACRO(__VA_ARGS__, SPLICE_HOOK_2, SPLICE_HOOK_1)(__VA_ARGS__)

#define SPLICE_HOOK_STATIC_1(funcname)          SPLICE_HOOK_AUTO_STATIC_IMPL(funcname, SPLICE_UNIQUE_ID)
#define SPLICE_HOOK_STATIC_2(libname, funcname) SPLICE_HOOK_STATIC_IMPL(libname, funcname, SPLICE_UNIQUE_ID)
#define SPLICE_HOOK_STATIC(...) \
    SPLICE_GET_MACRO(__VA_ARGS__, SPLICE_HOOK_STATIC_2, SPLICE_HOOK_STATIC_1)(__VA_ARGS__)

// Explicit "resolve by library + symbol name" spelling. Identical to the
// two-arg SPLICE_HOOK(lib, func) form, but named for clarity at the call site
// and for symmetry with the SPLICE_HOOK_LIB_AS policy-override variant.
#define SPLICE_HOOK_LIB(libname, funcname) \
    SPLICE_HOOK_IMPL(libname, funcname, SPLICE_UNIQUE_ID)
#define SPLICE_HOOK_LIB_STATIC(libname, funcname) \
    SPLICE_HOOK_STATIC_IMPL(libname, funcname, SPLICE_UNIQUE_ID)

#define SPLICE_HOOK_ADDR(func_ptr)        SPLICE_HOOK_ADDR_IMPL(func_ptr, SPLICE_UNIQUE_ID)
#define SPLICE_HOOK_ADDR_STATIC(func_ptr) SPLICE_HOOK_ADDR_STATIC_IMPL(func_ptr, SPLICE_UNIQUE_ID)

#define SPLICE_HOOK_MEMBER(method)        SPLICE_HOOK_MEMBER_IMPL(method, SPLICE_UNIQUE_ID)
#define SPLICE_HOOK_MEMBER_STATIC(method) SPLICE_HOOK_MEMBER_STATIC_IMPL(method, SPLICE_UNIQUE_ID)

// ─── FR-010 Step 4: per-call-site policy override (`*_AS` family) ──────────
// Pin a specific concurrency policy at this call site. Pass the policy as
// the *first* macro argument (e.g. `splice::policy::shared_mutex`) — the
// usual SPLICE_HOOK_AS<Policy>(func) C++ syntax can't be expressed in a
// preprocessor macro because templates and macros parse independently.
//
//   SPLICE_HOOK_AS(splice::policy::shared_mutex, my_func).onInvoke(...);
//   SPLICE_HOOK_LIB_AS(splice::policy::shared_mutex, "libfoo.so", my_func)
//   SPLICE_HOOK_ADDR_AS(splice::policy::shared_mutex, &my_func).onInvoke(...);
//
// 99% of users want SPLICE_HOOK / SPLICE_HOOK_ADDR (default policy via
// SPLICE_DEFAULT_POLICY). Reach for `*_AS` only when a callback truly
// needs to be swapped at runtime — see policy.h doc for the trade-offs.

#define SPLICE_HOOK_AS_IMPL(Policy, funcname, uniqueId)                                        \
    [&]() -> splice::InterceptorEntry<SPLICE_DEDUCE_FN(funcname), Policy>& {                          \
        static thread_local splice::InterceptorEntry<SPLICE_DEDUCE_FN(funcname), Policy> entry(       \
            nullptr, reinterpret_cast<void*>(&funcname), #funcname, uniqueId,                  \
            splice::TrampolineGenerator<SPLICE_DEDUCE_FN(funcname), uniqueId, Policy>                 \
                ::get_trampoline_ptr());                                                       \
        return entry;                                                                          \
    }()

#define SPLICE_HOOK_AS_STATIC_IMPL(Policy, funcname, uniqueId)                                 \
    [&]() -> splice::InterceptorEntry<SPLICE_DEDUCE_FN(funcname), Policy>& {                          \
        static splice::InterceptorEntry<SPLICE_DEDUCE_FN(funcname), Policy> entry(                    \
            nullptr, reinterpret_cast<void*>(&funcname), #funcname, uniqueId,                  \
            splice::TrampolineGenerator<SPLICE_DEDUCE_FN(funcname), uniqueId, Policy>                 \
                ::get_trampoline_ptr());                                                       \
        return entry;                                                                          \
    }()

#define SPLICE_HOOK_LIB_AS_IMPL(Policy, libname, funcname, uniqueId)                           \
    [&]() -> splice::InterceptorEntry<SPLICE_DEDUCE_FN(funcname), Policy>& {                          \
        static thread_local splice::InterceptorEntry<SPLICE_DEDUCE_FN(funcname), Policy> entry(       \
            libname, #funcname, #funcname, uniqueId,                                           \
            splice::TrampolineGenerator<SPLICE_DEDUCE_FN(funcname), uniqueId, Policy>                 \
                ::get_trampoline_ptr());                                                       \
        return entry;                                                                          \
    }()

#define SPLICE_HOOK_ADDR_AS_IMPL(Policy, func_ptr, uniqueId)                                   \
    [&]() -> splice::InterceptorEntry<SPLICE_DEDUCE_PTR(func_ptr), Policy>& {                           \
        static thread_local splice::InterceptorEntry<SPLICE_DEDUCE_PTR(func_ptr), Policy> entry(        \
            nullptr, reinterpret_cast<void*>(func_ptr), "direct_addr", uniqueId,               \
            splice::TrampolineGenerator<SPLICE_DEDUCE_PTR(func_ptr), uniqueId, Policy>                  \
                ::get_trampoline_ptr());                                                       \
        return entry;                                                                          \
    }()

#define SPLICE_HOOK_ADDR_AS_STATIC_IMPL(Policy, func_ptr, uniqueId)                            \
    [&]() -> splice::InterceptorEntry<SPLICE_DEDUCE_PTR(func_ptr), Policy>& {                           \
        static splice::InterceptorEntry<SPLICE_DEDUCE_PTR(func_ptr), Policy> entry(                     \
            nullptr, reinterpret_cast<void*>(func_ptr), "direct_addr", uniqueId,               \
            splice::TrampolineGenerator<SPLICE_DEDUCE_PTR(func_ptr), uniqueId, Policy>                  \
                ::get_trampoline_ptr());                                                       \
        return entry;                                                                          \
    }()

#define SPLICE_HOOK_AS(Policy, funcname)                                                       \
    SPLICE_HOOK_AS_IMPL(Policy, funcname, SPLICE_UNIQUE_ID)
#define SPLICE_HOOK_AS_STATIC(Policy, funcname)                                                \
    SPLICE_HOOK_AS_STATIC_IMPL(Policy, funcname, SPLICE_UNIQUE_ID)
#define SPLICE_HOOK_LIB_AS(Policy, libname, funcname)                                          \
    SPLICE_HOOK_LIB_AS_IMPL(Policy, libname, funcname, SPLICE_UNIQUE_ID)
#define SPLICE_HOOK_ADDR_AS(Policy, func_ptr)                                                  \
    SPLICE_HOOK_ADDR_AS_IMPL(Policy, func_ptr, SPLICE_UNIQUE_ID)
#define SPLICE_HOOK_ADDR_AS_STATIC(Policy, func_ptr)                                           \
    SPLICE_HOOK_ADDR_AS_STATIC_IMPL(Policy, func_ptr, SPLICE_UNIQUE_ID)

// ─── Query helpers ──────────────────────────────────────────────────────────
#define SPLICE_GET_ORIGINAL(libname, funcname)                                               \
    splice::OriginalRegistry::get_original_by_key<SPLICE_DEDUCE_FN(funcname)>(libname "_" #funcname)

#define SPLICE_IS_INSTALLED(libname, funcname) \
    (SPLICE_GET_ORIGINAL(libname, funcname) != nullptr)

// Call the original if installed; otherwise silently no-op (void returns)
// or return a zero-initialised default.
#define SPLICE_CALL_ORIGINAL(libname, funcname, ...)                                         \
    [&]() {                                                                                  \
        auto _splice_orig = SPLICE_GET_ORIGINAL(libname, funcname);                          \
        if (_splice_orig) return _splice_orig(__VA_ARGS__);                                  \
    }()

// Strict variant — warns when the original isn't installed.
#define SPLICE_CALL_ORIGINAL_STRICT(libname, funcname, ...)                                  \
    [&]() -> decltype(funcname(__VA_ARGS__)) {                                               \
        auto _splice_orig = SPLICE_GET_ORIGINAL(libname, funcname);                          \
        if (_splice_orig) return _splice_orig(__VA_ARGS__);                                  \
        SPLICE_LOGE("Original function not installed: %s::%s", libname, #funcname);          \
        if constexpr (std::is_void_v<decltype(funcname(__VA_ARGS__))>) { return; }           \
        else { return {}; }                                                                  \
    }()

// ─── Batch registration convenience ─────────────────────────────────────────
#define SPLICE_HOOKS(list) static splice::InterceptorBatch _splice_hook_batch{list};
