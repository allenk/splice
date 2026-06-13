// ─── Splice core — InterceptorEntry and install machinery ──────────────────
//
// Home of the fluent hook builder. One `InterceptorEntry<FuncType>` per
// SPLICE_HOOK* call site — constructed at static-init time, pushed into
// the global installer queue, called by splice::install_all().
//
// Preserves the legacy `.onInvoke(lambda)` method name deliberately. This
// is Splice's signature fluent-API moment — snake_case'ing it to
// `.on_invoke()` would technically comply with the 3+1 rule but erode
// the DSL feel the user explicitly asked us to preserve. Treat this as
// the documented exception for fluent-chain methods; FR-009 may extend
// (`.before()`, `.after()`, `.when()`, `.once()`, `.times(n)`) under the
// same exception.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <splice/engine.h>
#include <splice/log.h>
#include <splice/policy.h>
#include <splice/registry.h>
#include <splice/traits.h>
#include <splice/trampoline.h>

#include <atomic>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace splice {

// Primary template — undefined on purpose; only the function-pointer
// specialization below is instantiable.
//
// Policy is the FR-010 concurrency tag (rcu_writeonce / shared_mutex /...).
// It defaults to SPLICE_DEFAULT_POLICY so existing SPLICE_HOOK call sites
// compile unchanged; SPLICE_HOOK_AS pins it to a specific value.
template <typename T, typename Policy = SPLICE_DEFAULT_POLICY>
class InterceptorEntry;

template <typename Ret, typename... Args, typename Policy>
class InterceptorEntry<Ret (*)(Args...), Policy> {
public:
    using FuncType = Ret (*)(Args...);

    // Constructor — symbol-based hooking (lib + func name).
    InterceptorEntry(const char* lib_name, const char* func_name, const char* sym_name,
                     int unique_id, void* trampoline_ptr)
        : m_lib(lib_name ? lib_name : ""),
          m_func(func_name),
          m_sym(sym_name),
          m_unique_id(unique_id),
          m_slot(default_context().slot_for(trampoline_ptr)),
          m_trampoline_ptr(trampoline_ptr),
          m_original(nullptr),
          m_direct_addr(nullptr) {
        const std::string meaningful_key = m_lib + "_" + m_func;
        IdRegistry::register_mapping(meaningful_key, m_slot);

        SPLICE_LOGV("InterceptorEntry create: %s::%s (sym=%s) uid=%d slot=%d tramp=%p",
                    m_lib.c_str(), m_func.c_str(), m_sym.c_str(),
                    m_unique_id, m_slot, m_trampoline_ptr);

        // RAII: token's destructor (run from ~InterceptorEntry) marks the
        // installer slot dead so install_all() skips it. Prevents the
        // pre-Task #57 hazard where stack/test-scope entries would leave
        // a dangling [this] lambda in a process-wide vector.
        m_installer_token = register_global_installer([this]() { install(); });
    }

    // Constructor — direct-address hooking (for vtable slots, RVAs, etc.).
    InterceptorEntry(std::nullptr_t, void* func_addr, const char* label,
                     int unique_id, void* trampoline_ptr)
        : m_lib(""),
          m_func(label),
          m_sym(label),
          m_unique_id(unique_id),
          m_slot(default_context().slot_for(trampoline_ptr)),
          m_trampoline_ptr(trampoline_ptr),
          m_original(nullptr),
          m_direct_addr(func_addr) {
        std::stringstream ss;
        ss << "ADDR_" << func_addr;
        IdRegistry::register_mapping(ss.str(), m_slot);

        SPLICE_LOGV("InterceptorEntry create (direct): addr=%p label=%s uid=%d slot=%d tramp=%p",
                    func_addr, label, m_unique_id, m_slot, m_trampoline_ptr);

        m_installer_token = register_global_installer([this]() { install_direct(); });
    }

    // Fluent entry — the signature moment. Deliberate camelCase exception.
    // Uses m_slot (the globally-unique runtime slot keyed by trampoline_ptr)
    // rather than m_unique_id (per-TU __COUNTER__) — avoids cross-TU id
    // collisions that mis-route the registry downcast.
    //
    // FR-009 Step 9.2: any pending gates accumulated via .when() / .once() /
    // .times(n) are wrapped around the user lambda before storage. Gates
    // are consumed on every action verb call, so the chain composes
    // naturally: `.when(p).once().onInvoke(fn)` is equivalent to
    // `.once().when(p).onInvoke(fn)`. Gates set AFTER the action verb are
    // not retroactively applied — document this in the user guide.
    InterceptorEntry& onInvoke(std::function<Ret(FuncType, Args...)> fn) {
        commit_with_gates(std::move(fn));
        return *this;
    }

    // FR-009 Step 9.1 — `.before(lambda)` fires before the original. Lambda
    // signature: `void(Args...)` — no `orig` pointer is offered because
    // before-hooks can't suppress the call. Mutually exclusive with
    // .onInvoke() / .after() at the same call site (last writer wins).
    template <typename Before>
    InterceptorEntry& before(Before fn) {
        commit_with_gates(
            [f = std::move(fn)](FuncType orig, Args... args) -> Ret {
                f(args...);
                return orig(args...);
            });
        return *this;
    }

    // FR-009 Step 9.1 — `.after(lambda)` fires after the original. Lambda
    // signature: `void(Ret, Args...)` for non-void return, `void(Args...)`
    // for void return. Mutually exclusive with .onInvoke() / .before().
    template <typename After>
    InterceptorEntry& after(After fn) {
        if constexpr (std::is_void_v<Ret>) {
            commit_with_gates(
                [f = std::move(fn)](FuncType orig, Args... args) {
                    orig(args...);
                    f(args...);
                });
        } else {
            commit_with_gates(
                [f = std::move(fn)](FuncType orig, Args... args) -> Ret {
                    Ret ret = orig(args...);
                    f(ret, args...);
                    return ret;
                });
        }
        return *this;
    }

    // ─── FR-009 Step 9.2 — composable gates ───────────────────────────────
    // Order-independent: `.when(p).once()` and `.once().when(p)` compose to
    // identical behaviour. Gates accumulate in m_pending_gates and are
    // wrapped around the user lambda when an action verb commits.

    // .when(predicate) — gate hook firing on a runtime condition. Predicate
    // signature: `bool()` (nullary). When false, the trampoline takes the
    // original path with no callback invocation (zero overhead beyond the
    // single predicate call).
    template <typename Pred>
    InterceptorEntry& when(Pred pred) {
        m_pending_gates.when_pred = std::function<bool()>(std::move(pred));
        return *this;
    }

    // .once() — fire exactly once, then become transparent. Thread-safe via
    // atomic counter (shared between trampoline invocations).
    InterceptorEntry& once() {
        m_pending_gates.remaining = std::make_shared<std::atomic<long long>>(1);
        return *this;
    }

    // .times(n) — fire n times, then become transparent.
    InterceptorEntry& times(long long n) {
        m_pending_gates.remaining = std::make_shared<std::atomic<long long>>(n);
        return *this;
    }

    // Query — was install() actually successful?
    [[nodiscard]] bool is_installed() const noexcept { return m_installed; }

    // FR-013 Tier 1/2 disable. For POINTER_SWAP (GOT/IAT) installs, this
    // atomically restores the original pointer — always safe.
    // For INLINE installs (Phase 4.5c-2), restores the original first
    // instruction word atomically; trampoline memory is leaked per
    // FR-013 documented limitation.
    //
    // Returns true on success. After successful disable, calls to the
    // hooked function go to the original behaviour again. The
    // HookManager callback is preserved (re-installable via install()).
    bool disable() noexcept {
        if (!m_installed) return false;
        const int err = splice_disable(&m_record);
        if (err == 0) {
            m_installed = false;
            SPLICE_LOGV("InterceptorEntry::disable: ok id=%d", m_unique_id);
            return true;
        }
        if (err == -2) {
            SPLICE_LOGW("InterceptorEntry::disable: INLINE Tier 2 not yet implemented (id=%d)",
                        m_unique_id);
        } else {
            SPLICE_LOGE("InterceptorEntry::disable: failed (err=%d, id=%d)", err, m_unique_id);
        }
        return false;
    }

    ~InterceptorEntry() {
        SPLICE_LOGV("InterceptorEntry destroyed: %s::%s id=%d",
                    m_lib.c_str(), m_sym.c_str(), m_unique_id);
    }

    InterceptorEntry(const InterceptorEntry&) = delete;
    InterceptorEntry& operator=(const InterceptorEntry&) = delete;

private:
    // Pre-patch callback context. Captures the unique_id so the static
    // C-compatible callback can update OriginalRegistry without needing
    // to template on FuncType (the C ABI is type-erased anyway — the
    // void* the registry stores is the same regardless of FuncType).
    struct PrePatchCtx {
        InterceptorEntry* self;
    };

    static void on_trampoline_ready(void* original, void* user_data) noexcept {
        auto* ctx = static_cast<PrePatchCtx*>(user_data);
        ctx->self->m_original = reinterpret_cast<FuncType>(original);
        // Use the runtime slot (keyed by trampoline_ptr) — avoids cross-TU
        // __COUNTER__ collisions that previously cross-aliased originals
        // between hooks installed from different translation units.
        OriginalRegistry::set_original(ctx->self->m_slot, ctx->self->m_original);
    }

    void install() {
        PrePatchCtx ctx{this};
        void* stub = splice_hook_symbol_pre_rec(m_lib.c_str(), m_sym.c_str(),
                                                m_trampoline_ptr,
                                                reinterpret_cast<void**>(&m_original),
                                                &on_trampoline_ready, &ctx,
                                                &m_record);
        if (stub) {
            m_installed = true;
            SPLICE_LOGV("Hook installed: %s::%s => %p (id=%d, strategy=%d)",
                        m_lib.c_str(), m_sym.c_str(), stub, m_unique_id, m_record.strategy);
        } else {
            SPLICE_LOGE("Hook install FAILED: %s::%s", m_lib.c_str(), m_sym.c_str());
        }
    }

    void install_direct() {
        if (m_direct_addr == nullptr) {
            SPLICE_LOGE("install_direct: no address provided");
            return;
        }
        PrePatchCtx ctx{this};
        void* stub = splice_hook_address_pre_rec(m_direct_addr, m_trampoline_ptr,
                                                 reinterpret_cast<void**>(&m_original),
                                                 &on_trampoline_ready, &ctx,
                                                 &m_record);
        if (stub) {
            m_installed = true;
            SPLICE_LOGV("Hook installed (direct): %p => %p (id=%d, strategy=%d)",
                        m_direct_addr, m_trampoline_ptr, m_unique_id, m_record.strategy);
        } else {
            SPLICE_LOGE("Hook install FAILED (direct): %p", m_direct_addr);
        }
    }

    // ─── FR-009 Step 9.2 — accumulated gates ──────────────────────────────
    // Cleared every time an action verb (onInvoke/before/after) commits, so
    // subsequent action verbs at the same site start fresh.
    struct PendingGates {
        std::function<bool()>                   when_pred;
        std::shared_ptr<std::atomic<long long>> remaining;   // shared with lambda
    };
    PendingGates m_pending_gates;

    // Wrap user-supplied invoke-shaped lambda with pending gates, push to
    // HookManager, and clear m_pending_gates.
    template <typename InvokeFn>
    void commit_with_gates(InvokeFn fn) {
        // Snapshot the gates by value (shared_ptr is cheap) and reset
        // m_pending_gates so the next action verb starts clean.
        PendingGates gates = std::move(m_pending_gates);
        m_pending_gates = {};

        if (!gates.when_pred && !gates.remaining) {
            // Fast path — no gates, store the lambda as-is.
            HookManager::get_hook_as<Policy, Ret, Args...>(m_slot)
                .set_invoke(std::move(fn));
            return;
        }

        // Wrap with whatever gates are present. Both gate checks compile
        // out as no-ops on unused branches because of the empty function /
        // null shared_ptr checks at call time.
        auto wrapped = [gates = std::move(gates), fn = std::move(fn)]
                       (FuncType orig, Args... args) -> Ret {
            if (gates.when_pred && !gates.when_pred()) {
                return orig(args...);
            }
            if (gates.remaining) {
                // fetch_sub returns the value BEFORE decrement. Fire only
                // while the budget is positive. The counter keeps ticking
                // down past zero (harmless — bounded by call count).
                if (gates.remaining->fetch_sub(1, std::memory_order_acq_rel) <= 0) {
                    return orig(args...);
                }
            }
            return fn(orig, args...);
        };

        HookManager::get_hook_as<Policy, Ret, Args...>(m_slot)
            .set_invoke(std::move(wrapped));
    }

    std::string m_lib;
    std::string m_func;
    std::string m_sym;
    int m_unique_id;     // per-TU __COUNTER__ — kept only as a debug-log identity
    int m_slot;          // globally-unique runtime slot from HookContext::slot_for
    void* m_trampoline_ptr;
    FuncType m_original;
    void* m_direct_addr;
    splice_patch_record m_record{};   // populated on install, consumed by disable
    bool m_installed = false;          // true between successful install() and disable()
    InstallerToken m_installer_token;  // RAII handle into HookContext's installer queue
};

// MSVC quirk: same workaround as in trampoline.h. Some macro call sites
// deliver `decltype(&f)` as a function type instead of function pointer;
// provide a delegating specialisation so both forms resolve identically.
template <typename Ret, typename... Args, typename Policy>
class InterceptorEntry<Ret(Args...), Policy>
    : public InterceptorEntry<Ret (*)(Args...), Policy> {
    using Base = InterceptorEntry<Ret (*)(Args...), Policy>;
public:
    using Base::Base;  // inherit all constructors
};

// ─── splice::ScopedHook ────────────────────────────────────────────────────
// FR-009 Step 9.5 — RAII auto-disable wrapper around an InterceptorEntry.
//
// Type-erased on purpose: holds a `std::function<void()>` that calls
// `entry.disable()`. Two consequences:
//   1. Heterogeneous container support — `std::vector<ScopedHook>` works
//      across different (FuncType, Policy) entries.
//   2. The underlying entry's fluent API (.onInvoke/.before/.after/...)
//      is *not* re-exposed. Usage pattern: build the chain first, then
//      capture the resulting reference into a ScopedHook:
//
//          splice::ScopedHook h = SPLICE_HOOK(foo).onInvoke([](...){...});
//          // h destructs → entry.disable() fires
//
// Move-only. Default-constructible to an empty state (no-op on destruction).
// release() is idempotent — explicit early disable + detach.
//
// When to use:
//   - Unit tests that need clean teardown between cases
//   - Plugin systems with load/unload lifecycle
//   - Short-lived hook sessions ("debug for 30 seconds then stop")
//
// When NOT to use:
//   - Hooks that live for the program's lifetime (use SPLICE_HOOK_STATIC
//     fire-and-forget — no ScopedHook needed)
//   - When `.when(predicate)` already covers the runtime gate
//
// The underlying entry is *not owned* — it typically lives in a function-
// local static created by the SPLICE_HOOK_* macro. Multiple ScopedHooks
// referencing the same entry will each try to disable on destruction;
// `disable()` is idempotent on already-disabled entries (returns false,
// no-op), so this is safe but wasteful — don't do it.
class ScopedHook {
public:
    ScopedHook() noexcept = default;

    // Construct from any InterceptorEntry<FuncType, Policy>. The lambda
    // captures `entry` by reference — the entry must outlive the
    // ScopedHook. SPLICE_HOOK_*_STATIC macros produce process-lifetime
    // statics, so this is the safe default.
    template <typename FuncType, typename Policy>
    ScopedHook(InterceptorEntry<FuncType, Policy>& entry) noexcept
        : m_disable_fn([&entry]() noexcept { (void)entry.disable(); }) {}

    ~ScopedHook() { release(); }

    ScopedHook(const ScopedHook&) = delete;
    ScopedHook& operator=(const ScopedHook&) = delete;

    ScopedHook(ScopedHook&& other) noexcept
        : m_disable_fn(std::move(other.m_disable_fn)) {
        other.m_disable_fn = nullptr;
    }

    ScopedHook& operator=(ScopedHook&& other) noexcept {
        if (this != &other) {
            release();
            m_disable_fn = std::move(other.m_disable_fn);
            other.m_disable_fn = nullptr;
        }
        return *this;
    }

    // Explicit disable + detach. Idempotent.
    void release() noexcept {
        if (m_disable_fn) {
            m_disable_fn();
            m_disable_fn = nullptr;
        }
    }

    [[nodiscard]] bool empty() const noexcept {
        return m_disable_fn == nullptr;
    }

private:
    std::function<void()> m_disable_fn;
};

} // namespace splice
