// ─── Splice registries — backwards-compat shims over HookContext ──────────
//
// After Phase 1.5 (FR-008), all hook state lives in `splice::HookContext`
// and the process accesses it via `splice::default_context()`. This header
// keeps the pre-1.5 class names (`IdRegistry`, `HookManager`,
// `OriginalRegistry`) and free functions (`register_global_installer`,
// `install_all`) alive as thin delegating wrappers, so:
//
//   - InterceptorEntry<> in core.h keeps compiling unmodified
//   - TrampolineGenerator<> in trampoline.h keeps compiling unmodified
//   - Pre-1.5 user code ports cleanly with no `s/IDRegistry/.../g` churn
//
// For new code that wants test isolation or explicit context scoping,
// prefer the HookContext API directly.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <splice/context.h>

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace splice {

// ─── IdRegistry — routes to default_context() ──────────────────────────────
class IdRegistry {
public:
    static void register_mapping(const std::string& key, int id) {
        default_context().register_mapping(key, id);
    }
    static int get_id_by_key(const std::string& key) {
        return default_context().get_id_by_key(key);
    }
    static std::string get_key_by_id(int id) {
        return default_context().get_key_by_id(id);
    }
};

// ─── HookManager — routes to default_context() ────────────────────────────
// Hook<> is now defined at namespace scope in <splice/context.h>. The
// aliases below keep legacy `HookManager::Hook<...>` spellings working
// while threading the FR-010 Policy parameter through.
class HookManager {
public:
    // Legacy typedef kept for v1.x source compatibility. Splice itself
    // no longer uses it — FR-010 Step 3.5 moved the hot-path mutex to
    // shared_mutex in HookContext. Will be removed in v2.0.
    using HookMutex = std::shared_mutex;

    template <typename Ret, typename... Args>
    using Hook = ::splice::Hook<Ret, Args...>;

    template <typename Policy, typename Ret, typename... Args>
    using HookAs = ::splice::HookAs<Policy, Ret, Args...>;

    // Policy-explicit form (FR-010 Step 4). Trampoline calls this so the
    // downcast inside HookContext::get_hook_as is type-correct under
    // SPLICE_HOOK_AS overrides.
    template <typename Policy, typename Ret, typename... Args>
    static HookAs<Policy, Ret, Args...>& get_hook_as(int id) {
        return default_context().template get_hook_as<Policy, Ret, Args...>(id);
    }

    // Legacy spelling — defaults Policy to SPLICE_DEFAULT_POLICY.
    template <typename Ret, typename... Args>
    static Hook<Ret, Args...>& get_hook(int id) {
        return default_context().template get_hook<Ret, Args...>(id);
    }
};

// ─── OriginalRegistry — routes to default_context() ───────────────────────
class OriginalRegistry {
public:
    template <typename FuncType>
    static void set_original(int id, FuncType original) {
        default_context().template set_original<FuncType>(id, original);
    }
    template <typename FuncType>
    static FuncType get_original(int id) {
        return default_context().template get_original<FuncType>(id);
    }
    template <typename FuncType>
    static FuncType get_original_by_key(const std::string& key) {
        return default_context().template get_original_by_key<FuncType>(key);
    }
};

// ─── Global installer queue — free functions on default_context() ────────
// register_global_installer returns an InstallerToken (Task #57). Callers
// that don't want RAII semantics can ignore it (the token's destructor
// will fire at the temporary's end of full-expression and unregister).
// In practice every caller stores it — see InterceptorEntry::m_installer_token.

[[nodiscard]] inline InstallerToken register_global_installer(std::function<void()> fn) {
    return default_context().register_installer(std::move(fn));
}

inline void install_all() {
    default_context().install_all();
}

// Count of *live* installers currently queued in the default context.
// Slots whose token was released are skipped.
inline std::size_t global_installer_count() {
    return default_context().installer_count();
}

// ─── Batch registration helper ────────────────────────────────────────────
// Holds the tokens for the batch's lifetime so a process-static
// InterceptorBatch keeps its hooks installable until shutdown.
class InterceptorBatch {
public:
    using EntryInstaller = std::function<void()>;
    InterceptorBatch(std::initializer_list<EntryInstaller> installers) {
        m_tokens.reserve(installers.size());
        for (auto& fn : installers) {
            m_tokens.push_back(register_global_installer(fn));
        }
    }

private:
    std::vector<InstallerToken> m_tokens;
};

} // namespace splice
