// ─── Splice HookContext ────────────────────────────────────────────────────
//
// FR-008 / Phase 1.5 — consolidation of the four loose globals the v1.4
// review flagged as architectural debt:
//
//   the predecessor framework::IDRegistry         (key ↔ id mapping)
//   the predecessor framework::HookManager        (per-signature hook callback map)
//   the predecessor framework::OriginalRegistry   (id → original function pointer)
//   getGlobalInstallers()               (install queue)
//
// Each used to be its own static-singleton class with its own mutex —
// impossible to reset between unit tests, brittle under
// multi-shared-library link, zero fork-safety. They are now four fields
// inside a single `splice::HookContext` that the process accesses via
// `splice::default_context()`. Advanced users (tests, sandboxes) can
// construct isolated HookContext instances on the stack.
//
// Backwards compat: `<splice/registry.h>` still exposes `IdRegistry`,
// `HookManager`, `OriginalRegistry`, `register_global_installer`,
// `install_all` — they are now thin shims that delegate here.
//
// Known residual debt (addressed in later phases):
//   - std::function still on the hot path (FR-010 Step 5 / template Callback)
//   - void* round-trip in `m_originals` (planned: typed handle via std::any)
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <splice/log.h>
#include <splice/policy.h>
#include <splice/registry_impl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace splice {

class HookContext;

// ─── InstallerToken — RAII handle for a queued installer ───────────────────
// Returned by `HookContext::register_installer`. Move-only. On destruction
// the token tells its HookContext to mark the slot dead so `install_all()`
// will skip it. Solves Task #57: prior to this, every InterceptorEntry
// constructor pushed a `[this]` lambda into a `std::vector` that was never
// drained, leaving a dangling capture once the entry went out of scope.
//
// The generation field lets `HookContext::reset()` (test fixture helper)
// invalidate every outstanding token in O(1) by bumping the context's
// generation counter — slot indices issued under an older generation
// silently no-op on release.
class InstallerToken {
public:
    InstallerToken() noexcept = default;
    InstallerToken(HookContext* ctx, int slot, std::uint32_t generation) noexcept
        : m_ctx(ctx), m_slot(slot), m_generation(generation) {}

    ~InstallerToken() { release(); }

    InstallerToken(const InstallerToken&) = delete;
    InstallerToken& operator=(const InstallerToken&) = delete;

    InstallerToken(InstallerToken&& other) noexcept
        : m_ctx(other.m_ctx), m_slot(other.m_slot), m_generation(other.m_generation) {
        other.m_ctx = nullptr;
        other.m_slot = -1;
        other.m_generation = 0;
    }

    InstallerToken& operator=(InstallerToken&& other) noexcept {
        if (this != &other) {
            release();
            m_ctx = other.m_ctx;
            m_slot = other.m_slot;
            m_generation = other.m_generation;
            other.m_ctx = nullptr;
            other.m_slot = -1;
            other.m_generation = 0;
        }
        return *this;
    }

    // Release early. Idempotent — safe to call before destruction.
    void release() noexcept;

    [[nodiscard]] bool empty() const noexcept { return m_ctx == nullptr; }
    [[nodiscard]] int slot() const noexcept { return m_slot; }

private:
    HookContext* m_ctx = nullptr;
    int m_slot = -1;
    std::uint32_t m_generation = 0;
};

// ─── HookBase ───────────────────────────────────────────────────────────────
// Empty virtual base — lets HookContext store `shared_ptr<HookBase>` as a
// heterogeneous container while each concrete `Hook<Ret,Args...>` carries
// its own type-safe invocation path.
class HookBase {
public:
    virtual ~HookBase() = default;
};

// ─── HookStorage<Policy, Ret, Args...> ──────────────────────────────────────
// FR-010 hot-path callback storage. One partial spec per policy tag from
// <splice/policy.h>. The signature must match across all specs:
//   - using HookFn  = std::function<Ret(Ret(*)(Args...), Args...)>;
//   - void store(HookFn fn);
//   - Ret  invoke(Ret(*)(Args...) orig, Args... args);
template <typename Policy, typename Ret, typename... Args>
struct HookStorage;   // primary intentionally undefined

// rcu_writeonce — default. Reader = 1 atomic acquire-load. Writer = atomic
// store of a fresh heap-allocated HookFn. **Old HookFn objects leak by
// design** (see policy.h doc — Splice hooks live for program lifetime).
template <typename Ret, typename... Args>
struct HookStorage<policy::rcu_writeonce, Ret, Args...> {
    using FuncType = Ret (*)(Args...);
    using HookFn   = std::function<Ret(FuncType, Args...)>;

    std::atomic<HookFn*> m_fn{nullptr};

    void store(HookFn fn) noexcept(false) {
        // Intentional leak — see policy.h. New allocation on every set,
        // bounded by the (small) number of times user code calls onInvoke.
        m_fn.store(new HookFn(std::move(fn)), std::memory_order_release);
    }

    Ret invoke(FuncType original, Args... args) {
        if (auto* fn = m_fn.load(std::memory_order_acquire)) {
            return (*fn)(original, args...);
        }
        return original(args...);
    }
};

// shared_mutex — escape hatch for callbacks swapped at runtime. Reader
// pays an atomic counter inc/dec; writer takes unique_lock.
template <typename Ret, typename... Args>
struct HookStorage<policy::shared_mutex, Ret, Args...> {
    using FuncType = Ret (*)(Args...);
    using HookFn   = std::function<Ret(FuncType, Args...)>;

    HookFn                    m_fn;
    mutable std::shared_mutex m_mutex;

    void store(HookFn fn) {
        std::unique_lock lock(m_mutex);
        m_fn = std::move(fn);
    }

    Ret invoke(FuncType original, Args... args) {
        std::shared_lock lock(m_mutex);
        if (m_fn) return m_fn(original, args...);
        return original(args...);
    }
};

// rcu_refcounted left out for now — atomic<shared_ptr> is C++20 and the
// AOSP-side toolchain support is uneven. Add when a real consumer needs
// zero-leak runtime swap. Step 6 in the recommended ordering.

// ─── HookAs<Policy, Ret, Args...> + Hook<Ret, Args...> alias ───────────────
// Per-signature wrapper. Policy selects HookStorage<>; pinned per call
// site by SPLICE_HOOK_AS, otherwise defaults to SPLICE_DEFAULT_POLICY via
// the legacy `Hook<Ret, Args...>` alias.
//
// The HookContext registry keys on (id, Policy) — the downcast inside
// `get_hook` is type-correct only when the same id is always queried
// with the same Policy. __COUNTER__ guarantees that for macro-generated
// call sites; manual InterceptorEntry construction must uphold it.
template <typename Policy, typename Ret, typename... Args>
class HookAs : public HookBase {
public:
    using FuncType = Ret (*)(Args...);
    using HookFn   = std::function<Ret(FuncType, Args...)>;

    void set_invoke(HookFn fn) { m_storage.store(std::move(fn)); }

    // FR-009 / Step 9.1 — `before` and `after` are sugar over set_invoke,
    // wrapping the user lambda into a HookFn that brackets the original
    // call. Zero hot-path overhead vs raw set_invoke (no extra atomic
    // loads — same single-slot dispatch). Mutual exclusion with onInvoke
    // is enforced by last-writer-wins on the same storage slot: a later
    // .onInvoke() / .before() / .after() simply overwrites whatever was
    // set last. Tested in tests/test_hook_modifiers.cpp.
    template <typename Before>
    void set_before(Before fn) {
        auto wrapper = [f = std::move(fn)](FuncType orig, Args... args) -> Ret {
            f(args...);
            return orig(args...);
        };
        set_invoke(std::move(wrapper));
    }

    template <typename After>
    void set_after(After fn) {
        if constexpr (std::is_void_v<Ret>) {
            auto wrapper = [f = std::move(fn)](FuncType orig, Args... args) {
                orig(args...);
                f(args...);
            };
            set_invoke(std::move(wrapper));
        } else {
            auto wrapper = [f = std::move(fn)](FuncType orig, Args... args) -> Ret {
                Ret ret = orig(args...);
                f(ret, args...);
                return ret;
            };
            set_invoke(std::move(wrapper));
        }
    }

    Ret invoke(FuncType original, Args... args) {
        return m_storage.invoke(original, std::forward<Args>(args)...);
    }

private:
    HookStorage<Policy, Ret, Args...> m_storage;
};

// Legacy alias — `Hook<Ret, Args...>` resolves to the default-policy
// instantiation. Pre-FR-010 source compiles unchanged.
template <typename Ret, typename... Args>
using Hook = HookAs<SPLICE_DEFAULT_POLICY, Ret, Args...>;

// ─── HookRegistry<Impl> ────────────────────────────────────────────────────
// FR-010 Step 6. Owns the per-id `HookBase` slot map. Selected at compile
// time via `SPLICE_REGISTRY_IMPL`. Step 6.1 ships only the shared_mutex_map
// specialisation (zero behaviour change); Step 6.2 adds rcu_atomic_array.
//
// Interface contract for every specialisation:
//   - template <typename HookT> HookT& get_or_create(int id);
//   - void clear() noexcept;
//   - std::size_t size() const noexcept;
//
// HookT is constructed via std::make_shared when the slot is empty. The
// caller (HookContext::get_hook_as) ensures the same id is always queried
// with the same HookT (enforced by __COUNTER__-derived ids inside the
// SPLICE_HOOK* macros).
template <typename Impl>
class HookRegistry;   // primary intentionally undefined

// shared_mutex_map — the pre-Step-6 implementation, lifted verbatim into
// a partial spec. Reader pays an atomic RMW on the shared_mutex counter
// (cache-line bouncing under contention — the bottleneck Step 6 addresses
// by adding rcu_atomic_array as an opt-in).
template <>
class HookRegistry<registry::shared_mutex_map> {
public:
    template <typename HookT>
    HookT& get_or_create(int id) {
        // Fast path — slot already exists, reader-lock is enough.
        {
            std::shared_lock lock(m_mutex);
            auto it = m_hooks.find(id);
            if (it != m_hooks.end()) {
                return *static_cast<HookT*>(it->second.get());
            }
        }
        // Slow path — first call for this id, install the slot.
        std::unique_lock lock(m_mutex);
        auto& slot = m_hooks[id];
        if (!slot) {
            slot = std::make_shared<HookT>();
        }
        return *static_cast<HookT*>(slot.get());
    }

    void clear() noexcept {
        std::unique_lock lock(m_mutex);
        m_hooks.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::shared_lock lock(m_mutex);
        return m_hooks.size();
    }

private:
    mutable std::shared_mutex                                m_mutex;
    std::unordered_map<int, std::shared_ptr<HookBase>>       m_hooks;
};

// rcu_atomic_array — Step 6.2 (publish-and-reclaim). Reader is a single
// std::memory_order_acquire load on the snapshot pointer; no atomic RMW
// on the hot path. Writer copies the snapshot, fills the new slot,
// publishes the new pointer with release semantics, and retires the old
// pointer with a wall-clock timestamp.
//
// ── Reclamation strategy (Step 6.4 — time-deferred, deviating from the
// classical EBR sketched in the Step 6 design doc) ───────────────────────
// Standard epoch-based reclamation marks reader critical sections with a
// thread-local atomic store on entry and exit. For Splice this would add
// ~3-9 ns to every trampoline call, eating the entire FR-010 6.2/6.3 gain.
//
// Instead, we trust the Splice reader model:
//   - Every reader (the trampoline) completes in ns-μs
//   - Writers (install_slot) are cold-path, lock-serialised
//   - After publish, ANY in-flight reader using the old snapshot will
//     have finished within a small wall-clock grace period
//
// Strategy:
//   - On write: push (snap*, now) onto m_retire
//   - On every subsequent write: drain anything older than kGracePeriodMs
//     while still holding m_write_mutex. Since the next writer arrives
//     orders of magnitude later than any reader's window, the grace
//     period is effectively infinite from the reader's perspective.
//
// Pathological case: a reader thread that is preempted mid-trampoline for
// > 100 ms could observe a freed snapshot. In practice the OS scheduler's
// preemption quantum is typically 1–10 ms; 100 ms is 10–100× headroom.
// Documented limitation; user can rebuild with a larger grace period.
//
// Snapshot bound: SPLICE_MAX_HOOKS (default 512). Out-of-range ids hard-
// abort with SPLICE_LOGE — a hook id from __COUNTER__ above the bound is
// a programming error, not a recoverable condition.
template <>
class HookRegistry<registry::rcu_atomic_array> {
public:
    HookRegistry() = default;

    ~HookRegistry() {
        // At registry teardown: nothing is reading anymore (otherwise the
        // user violated the lifetime contract). Free everything.
        delete m_snapshot.load(std::memory_order_acquire);
        for (auto& e : m_retire) delete e.snap;
    }

    HookRegistry(const HookRegistry&) = delete;
    HookRegistry& operator=(const HookRegistry&) = delete;
    HookRegistry(HookRegistry&&) = delete;
    HookRegistry& operator=(HookRegistry&&) = delete;

    template <typename HookT>
    HookT& get_or_create(int id) {
        // ── Fast path — single acquire-load, pure read, no atomic RMW ──
        auto* snap = m_snapshot.load(std::memory_order_acquire);
        if (snap && id >= 0 && id < static_cast<int>(snap->slots.size())
            && snap->slots[static_cast<std::size_t>(id)]) {
            return *static_cast<HookT*>(
                snap->slots[static_cast<std::size_t>(id)].get());
        }
        return install_slot<HookT>(id);
    }

    void clear() noexcept {
        // Atomic publish of nullptr, retire old snapshot for time-deferred
        // reclamation. Readers still holding the old snapshot stay valid
        // until the grace period elapses.
        std::lock_guard lock(m_write_mutex);
        Snapshot* old = m_snapshot.exchange(nullptr, std::memory_order_acq_rel);
        if (old) {
            m_retire.push_back({old, std::chrono::steady_clock::now()});
        }
        drain_expired_locked();
    }

    // Best-effort: free anything past the grace period right now. Safe to
    // call from any thread (takes m_write_mutex). Typically not needed —
    // the next install_slot drains automatically.
    void reclaim_old_snapshots() {
        std::lock_guard lock(m_write_mutex);
        drain_expired_locked();
    }

    // Test helper: number of retired-but-not-yet-reclaimed snapshots.
    [[nodiscard]] std::size_t retired_count() const noexcept {
        std::lock_guard lock(m_write_mutex);
        return m_retire.size();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        auto* snap = m_snapshot.load(std::memory_order_acquire);
        if (!snap) return 0;
        std::size_t n = 0;
        for (auto& s : snap->slots) {
            if (s) ++n;
        }
        return n;
    }

private:
    struct Snapshot {
        std::array<std::shared_ptr<HookBase>, SPLICE_MAX_HOOKS> slots{};
    };

    template <typename HookT>
    HookT& install_slot(int id) {
        if (id < 0 || id >= static_cast<int>(SPLICE_MAX_HOOKS)) {
            SPLICE_LOGE("rcu_atomic_array: hook id=%d out of [0, %d). "
                        "Rebuild with -DSPLICE_MAX_HOOKS=<larger>.",
                        id, static_cast<int>(SPLICE_MAX_HOOKS));
            std::abort();
        }

        std::lock_guard lock(m_write_mutex);
        // Re-check inside the lock: another writer may have installed
        // this slot while we were waiting on m_write_mutex.
        auto* old = m_snapshot.load(std::memory_order_acquire);
        const std::size_t idx = static_cast<std::size_t>(id);
        if (old && old->slots[idx]) {
            return *static_cast<HookT*>(old->slots[idx].get());
        }

        auto next = std::make_unique<Snapshot>();
        if (old) next->slots = old->slots;   // copy 8 KiB (cold path, cheap)
        next->slots[idx] = std::make_shared<HookT>();
        HookT* result = static_cast<HookT*>(next->slots[idx].get());

        Snapshot* raw = next.release();
        m_snapshot.store(raw, std::memory_order_release);
        if (old) {
            m_retire.push_back({old, std::chrono::steady_clock::now()});
        }
        drain_expired_locked();
        return *result;
    }

    // Must be called with m_write_mutex held. Free retired snapshots whose
    // retirement timestamp is older than the grace period.
    void drain_expired_locked() noexcept {
        const auto now = std::chrono::steady_clock::now();
        const auto grace = std::chrono::milliseconds(kGracePeriodMs);
        auto it = m_retire.begin();
        while (it != m_retire.end() && (now - it->retired_at) >= grace) {
            delete it->snap;
            ++it;
        }
        m_retire.erase(m_retire.begin(), it);
    }

    struct RetireEntry {
        Snapshot* snap;
        std::chrono::steady_clock::time_point retired_at;
    };

    // 100 ms is 10–100× headroom over typical OS scheduler preemption
    // quanta. Override at build time if your reader windows are longer
    // (rare — would require a trampoline that itself blocks on I/O).
#ifndef SPLICE_RCU_GRACE_PERIOD_MS
#   define SPLICE_RCU_GRACE_PERIOD_MS 100
#endif
    static constexpr int kGracePeriodMs = SPLICE_RCU_GRACE_PERIOD_MS;

    std::atomic<Snapshot*>    m_snapshot{nullptr};
    mutable std::mutex        m_write_mutex;   // serialises writers + retire queue
    std::vector<RetireEntry>  m_retire;        // pending reclamation
};

// ─── OriginalsRegistry<Impl> ───────────────────────────────────────────────
// Companion to HookRegistry — owns the per-id `void*` original-function-
// pointer table that the trampoline reads on every fire.
//
// Same trade-off framing as HookRegistry. shared_mutex_map keeps the
// original unordered_map<int, void*> + shared_mutex pattern. rcu_atomic_
// array uses a fixed-size array of std::atomic<void*>, indexed by id —
// reader is a single acquire-load, no atomic RMW, no map find.
//
// Bound: SPLICE_MAX_HOOKS, same as HookRegistry. Out-of-range ids fall
// through to the slow path (logged) so we never crash on programs that
// momentarily exceed the bound during testing — but installs at such ids
// will not propagate to the trampoline.
template <typename Impl>
class OriginalsRegistry;   // primary undefined

template <>
class OriginalsRegistry<registry::shared_mutex_map> {
public:
    void set(int id, void* p) {
        std::unique_lock lock(m_mutex);
        m_map[id] = p;
    }

    [[nodiscard]] void* get(int id) const {
        std::shared_lock lock(m_mutex);
        auto it = m_map.find(id);
        return (it != m_map.end()) ? it->second : nullptr;
    }

    void clear() noexcept {
        std::unique_lock lock(m_mutex);
        m_map.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::shared_lock lock(m_mutex);
        return m_map.size();
    }

private:
    mutable std::shared_mutex          m_mutex;
    std::unordered_map<int, void*>     m_map;
};

template <>
class OriginalsRegistry<registry::rcu_atomic_array> {
public:
    void set(int id, void* p) noexcept {
        if (id < 0 || id >= static_cast<int>(SPLICE_MAX_HOOKS)) {
            SPLICE_LOGE("rcu_atomic_array originals: id=%d out of [0, %d). "
                        "Rebuild with -DSPLICE_MAX_HOOKS=<larger>.",
                        id, static_cast<int>(SPLICE_MAX_HOOKS));
            return;   // best-effort; trampoline will see nullptr
        }
        m_slots[static_cast<std::size_t>(id)].store(p, std::memory_order_release);
    }

    [[nodiscard]] void* get(int id) const noexcept {
        if (id < 0 || id >= static_cast<int>(SPLICE_MAX_HOOKS)) return nullptr;
        return m_slots[static_cast<std::size_t>(id)].load(std::memory_order_acquire);
    }

    void clear() noexcept {
        for (auto& a : m_slots) a.store(nullptr, std::memory_order_release);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::size_t n = 0;
        for (auto& a : m_slots) {
            if (a.load(std::memory_order_relaxed) != nullptr) ++n;
        }
        return n;
    }

private:
    std::array<std::atomic<void*>, SPLICE_MAX_HOOKS> m_slots{};
};

// ─── HookContext ────────────────────────────────────────────────────────────
// Owns all mutable hook state. Every public Splice API that used to touch
// a file-scope static now routes through an instance of this class.
//
// Thread safety (FR-010 Step 3.5): the registries that the trampoline
// reads on every fire (m_originals, m_hooks, m_key_to_id, m_id_to_key)
// are guarded by a `shared_mutex`. Read paths take `shared_lock`; write
// paths take `unique_lock`. The pre-FR-010 `recursive_mutex` was needed
// for an install-time re-entry pattern that turned out to be defensive —
// install_all snapshots the queue and runs callbacks *outside* the lock,
// so writes from inside a callback (`set_original`) acquire the unique
// lock without recursion.
//
// `get_hook` is the trampoline's hot read path. It uses a shared-lock
// fast path for already-existing slots, falling back to a unique-lock
// slow path only on the first call for a given id (cold install-time).
//
// The installer queue (m_installers + m_free_slots + m_generation) lives
// behind the same mutex but is touched only on cold paths (register /
// reset / install_all / token release).
class HookContext {
public:
    HookContext() = default;
    ~HookContext() = default;

    HookContext(const HookContext&) = delete;
    HookContext& operator=(const HookContext&) = delete;
    HookContext(HookContext&&) = delete;
    HookContext& operator=(HookContext&&) = delete;

    // ─── Id / key registry ───────────────────────────────────────────────

    void register_mapping(const std::string& key, int id) {
        std::unique_lock lock(m_mutex);
        m_key_to_id[key] = id;
        m_id_to_key[id] = key;
        SPLICE_LOGV("HookContext::register_mapping: %s => %d", key.c_str(), id);
    }

    [[nodiscard]] int get_id_by_key(const std::string& key) const {
        std::shared_lock lock(m_mutex);
        auto it = m_key_to_id.find(key);
        return (it != m_key_to_id.end()) ? it->second : -1;
    }

    [[nodiscard]] std::string get_key_by_id(int id) const {
        std::shared_lock lock(m_mutex);
        auto it = m_id_to_key.find(id);
        return (it != m_id_to_key.end()) ? it->second : std::string{};
    }

    // ─── Original function pointer registry ──────────────────────────────
    // Owned by OriginalsRegistry<SPLICE_REGISTRY_IMPL>. Under
    // rcu_atomic_array, both set and get are lock-free (atomic store /
    // load on a fixed-size array indexed by id). Under shared_mutex_map,
    // behaves as the pre-Step-6 unordered_map + shared_mutex.

    template <typename FuncType>
    void set_original(int id, FuncType original) {
        m_originals_registry.set(id, reinterpret_cast<void*>(original));
    }

    template <typename FuncType>
    [[nodiscard]] FuncType get_original(int id) const {
        return reinterpret_cast<FuncType>(m_originals_registry.get(id));
    }

    template <typename FuncType>
    [[nodiscard]] FuncType get_original_by_key(const std::string& key) const {
        const int id = get_id_by_key(key);
        return (id >= 0) ? get_original<FuncType>(id) : nullptr;
    }

    // ─── Hook callback registry ──────────────────────────────────────────
    // Trampoline hot path. Encapsulated in HookRegistry<SPLICE_REGISTRY_IMPL>
    // (FR-010 Step 6) — the default `shared_mutex_map` impl keeps the
    // pre-Step-6 behaviour exactly. Switch to `rcu_atomic_array` at build
    // time for 8-thread reader scenarios.

    // Per-call-site lookup with explicit Policy. Same id must always be
    // queried with the same (Ret, Policy, Args...) tuple — macros
    // enforce this via __COUNTER__.
    template <typename Policy, typename Ret, typename... Args>
    HookAs<Policy, Ret, Args...>& get_hook_as(int id) {
        using HookT = HookAs<Policy, Ret, Args...>;
        return m_hook_registry.template get_or_create<HookT>(id);
    }

    // Legacy spelling — defaults to SPLICE_DEFAULT_POLICY.
    template <typename Ret, typename... Args>
    Hook<Ret, Args...>& get_hook(int id) {
        return get_hook_as<SPLICE_DEFAULT_POLICY, Ret, Args...>(id);
    }

    // ─── Trampoline-pointer → runtime slot ────────────────────────────────
    // Fixes the cross-TU `__COUNTER__` collision. Pre-Step-6.4 the macros
    // used `__COUNTER__` directly as the registry slot id, but __COUNTER__
    // is per-TU and starts at 0 every file — so the first SPLICE_HOOK in
    // any two TUs both got id=0, and the registry's get_original /
    // get_hook_as slots got cross-aliased (last writer wins → infinite
    // recursion when the trampolines called each other's "original").
    //
    // The fix: each trampoline function has a globally unique address
    // (the linker guarantees this — one symbol per call-site instantiation).
    // We map (trampoline_ptr → runtime int slot) here and use the slot
    // for all registry lookups. Same trampoline_ptr always resolves to
    // the same slot, no matter which TU asks.
    //
    // m_unique_id from __COUNTER__ stays as a debug-log identifier
    // (IdRegistry::register_mapping) but no longer drives registry access.
    [[nodiscard]] int slot_for(void* trampoline_ptr) {
        std::unique_lock lock(m_mutex);
        auto it = m_slot_by_trampoline.find(trampoline_ptr);
        if (it != m_slot_by_trampoline.end()) return it->second;
        const int slot = m_next_slot++;
        m_slot_by_trampoline[trampoline_ptr] = slot;
        return slot;
    }

    // ─── Installer queue ─────────────────────────────────────────────────
    // Slot-based: each registered lambda gets a stable index, returned to
    // the caller as an InstallerToken. Token destruction marks the slot
    // dead — see Task #57. m_free_slots recycles indices so long-running
    // processes don't leak vector growth.

    [[nodiscard]] InstallerToken register_installer(std::function<void()> fn) {
        std::unique_lock lock(m_mutex);
        int slot;
        if (!m_free_slots.empty()) {
            slot = m_free_slots.back();
            m_free_slots.pop_back();
            auto& s = m_installers[static_cast<std::size_t>(slot)];
            s.fn = std::move(fn);
            s.live = true;
        } else {
            slot = static_cast<int>(m_installers.size());
            m_installers.push_back({std::move(fn), true});
        }
        SPLICE_LOGV("HookContext::register_installer: slot=%d gen=%u live=%zu",
                    slot, m_generation, live_installer_count_locked());
        return InstallerToken{this, slot, m_generation};
    }

    // Called by InstallerToken::release(). Idempotent under generation
    // mismatch (post-reset tokens no-op) and double-release.
    void unregister_installer(int slot, std::uint32_t gen) noexcept {
        std::unique_lock lock(m_mutex);
        if (gen != m_generation) return;
        if (slot < 0 || static_cast<std::size_t>(slot) >= m_installers.size()) return;
        auto& s = m_installers[static_cast<std::size_t>(slot)];
        if (!s.live) return;
        s.live = false;
        s.fn = nullptr;                      // drop the capture immediately
        m_free_slots.push_back(slot);
    }

    // Runs every live queued installer. Snapshots the live lambdas under
    // lock first so installers can call back into the context (e.g.
    // `set_original`) without deadlocking — installer callbacks acquire
    // the unique lock independently when they write.
    void install_all() {
        std::vector<std::function<void()>> local;
        {
            std::shared_lock lock(m_mutex);
            local.reserve(m_installers.size());
            for (auto& s : m_installers) {
                if (s.live) local.push_back(s.fn);
            }
        }
        SPLICE_LOGV("HookContext::install_all: running %zu installer(s)", local.size());
        for (auto& fn : local) {
            fn();
        }
    }

    [[nodiscard]] std::size_t installer_count() const {
        std::shared_lock lock(m_mutex);
        return live_installer_count_locked();
    }

    // ─── Test-fixture helper ────────────────────────────────────────────
    // Wipe all four registries + the installer queue. Bumps the generation
    // counter so any outstanding InstallerToken is silently invalidated
    // on its release path.
    void reset() {
        // Three locks (m_mutex for ids/installers, hook registry's own
        // internal mutex, originals registry's own internal mutex —
        // rcu_atomic_array version of originals is lock-free). Order:
        // m_mutex first to match all other code paths that touch the non-
        // hook state. The two registry clears take their own locks and
        // never reach back into m_mutex, so no deadlock risk.
        {
            std::unique_lock lock(m_mutex);
            m_key_to_id.clear();
            m_id_to_key.clear();
            m_installers.clear();
            m_free_slots.clear();
            m_slot_by_trampoline.clear();
            m_next_slot = 0;
            ++m_generation;
        }
        m_hook_registry.clear();
        m_originals_registry.clear();
    }

private:
    struct InstallerSlot {
        std::function<void()> fn;
        bool live = false;
    };

    [[nodiscard]] std::size_t live_installer_count_locked() const noexcept {
        std::size_t n = 0;
        for (auto& s : m_installers) {
            if (s.live) ++n;
        }
        return n;
    }

    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, int> m_key_to_id;
    std::unordered_map<int, std::string> m_id_to_key;
    OriginalsRegistry<SPLICE_REGISTRY_IMPL> m_originals_registry;
    HookRegistry<SPLICE_REGISTRY_IMPL> m_hook_registry;
    std::unordered_map<void*, int> m_slot_by_trampoline;
    int m_next_slot = 0;
    std::vector<InstallerSlot> m_installers;
    std::vector<int> m_free_slots;
    std::uint32_t m_generation = 0;
};

// Out-of-line: needs the full HookContext definition.
inline void InstallerToken::release() noexcept {
    if (m_ctx == nullptr) return;
    m_ctx->unregister_installer(m_slot, m_generation);
    m_ctx = nullptr;
    m_slot = -1;
    m_generation = 0;
}

// Process-wide default context. All SPLICE_HOOK* macros route here by
// default. Implementation: Meyers singleton in src/context.cpp to survive
// static-init-order issues across translation units.
HookContext& default_context() noexcept;

} // namespace splice
