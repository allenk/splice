// ─── Splice trampoline generator ───────────────────────────────────────────
//
// Per-hook-site static trampoline function, instantiated at compile time via
// a `__COUNTER__`-derived UniqueId. Each call site gets its own address,
// sidestepping the need for JIT-generated thunks.
//
// Port note: the v1.4 explicit-specialization-per-arity path (0, 1, 2, 3, 4,
// 5, 10 args) has been deleted — modern C++17 variadic templates cover all
// arities in a single definition. Review §2.2 #4: "Two code paths, twice
// the maintenance burden."
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <splice/context.h>

#include <type_traits>
#include <splice/policy.h>
#include <splice/registry.h>

namespace splice {

template <typename FuncType, int UniqueId, typename Policy = SPLICE_DEFAULT_POLICY>
struct TrampolineGenerator;

template <typename Ret, typename... Args, int UniqueId, typename Policy>
struct TrampolineGenerator<Ret (*)(Args...), UniqueId, Policy> {
    using function_type = Ret (*)(Args...);

    // The trampoline itself — one static function per (FuncType, UniqueId,
    // Policy) tuple. Its address is what the macros install as the hook
    // target. Policy threads through to HookManager::get_hook so the
    // downcast inside HookContext::get_hook is type-correct even when
    // SPLICE_HOOK_AS pins a non-default policy at this call site.
    //
    // Step 6.3 perf note: cache the default-context address in a function-
    // local pointer initialised on the first call. This avoids calling
    // default_context() twice per trampoline invocation.
    //
    // Step 6.4 correctness fix: __COUNTER__-derived UniqueId is per-TU and
    // collides across TUs (first SPLICE_HOOK in every test file got id=0).
    // The registry slot is now allocated at runtime via slot_for(trampoline
    // address) — each linker-unique trampoline gets a globally-unique slot.
    // UniqueId stays as the template parameter (it differentiates trampoline
    // function instantiations per call site) but is no longer the registry
    // key. The lookup happens once per trampoline lifetime (function-local
    // static initialiser).
    static Ret trampoline(Args... args) {
        static HookContext* const ctx = &default_context();
        static const int slot = ctx->slot_for(reinterpret_cast<void*>(&trampoline));

        // Checked, so that "the library's own paths use the checked forms" is
        // true of the hottest one too. A refused lookup used to come back as a
        // reference to an inert slot shared by every refused id, and dispatch
        // went ahead through it with whatever `get_original` had returned --
        // which for a refused slot is nullptr.
        auto hook = ctx->template try_get_hook_as<Policy, Ret, Args...>(slot);
        if (hook.value == nullptr) {
            auto fallback = ctx->template try_get_original<function_type>(slot);
            if (fallback.value != nullptr) {
                // The site is patched and its hook is not resolvable: the
                // honest behaviour is the unhooked one, not a crash.
                return fallback.value(args...);
            }
            SPLICE_LOGE_ONCE("splice: dispatch refused for slot %d -- %s, and "
                             "there is no original to fall back to. The call "
                             "returns without running anything.",
                             slot, refusal_text(hook.reason));
            if constexpr (std::is_void_v<Ret>) {
                return;
            } else {
                return Ret{};
            }
        }
        auto original = ctx->template try_get_original<function_type>(slot).value;
        return hook.value->invoke(original, args...);
    }

    static void* get_trampoline_ptr() { return reinterpret_cast<void*>(&trampoline); }
};

// MSVC quirk: in some template-deduction contexts `decltype(&f)` is delivered
// as a function type `Ret(Args...)` rather than the expected pointer
// `Ret(*)(Args...)`. Silently accept both by delegating the function-type
// specialisation to the pointer form.
template <typename Ret, typename... Args, int UniqueId, typename Policy>
struct TrampolineGenerator<Ret(Args...), UniqueId, Policy>
    : TrampolineGenerator<Ret (*)(Args...), UniqueId, Policy> {};

} // namespace splice
