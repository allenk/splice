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

#include <splice/engine.h>
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
#include <limits>
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

namespace detail {

// ─── what a queued installer contributes to a coordinated group ───────────
//
// `install_all()` has always meant "install every hook queued above", and it
// has always done it one hook at a time, because a `std::function<void()>` is
// all a loop needs. `install_all(splice::live)` means the same thing in one
// transaction, and a transaction needs the pieces separated: the target before
// anything is allocated, the publication between prepare and commit, and the
// record after the write lands.
//
// Nothing here is a template. That is the whole difficulty: the queue is type
// erased at registration, long before anyone asks for a group, so the entry
// has to hand over closures rather than a signature.
// A filled contribution with a null `target` is not an error: it means this
// installer has nothing for the group to do, because its hook is already in
// place. `install_all()` has always been safe to call more than once.
struct GroupContribution {
    void* target = nullptr;     // the function to patch, already resolved
    void* stub   = nullptr;     // the trampoline the entry already owns

    // Called with the original function, after the group has taken its
    // permissions and before it suspends anything. Same moment, and the same
    // reason, as the single-site path's pre-patch callback.
    std::function<void(void* original)> publish;

    // The record, once the write has landed -- or null to say the hook is not
    // in place: a failed commit, or the group being disabled later. Without
    // the null case the entry would still believe it was installed and a
    // second disable would restore bytes that are already original.
    std::function<void(const splice_patch_record*)> settle;
};

// Fills a contribution, or returns false when this installer cannot join a
// group at all -- a refused re-entrancy scope, an unresolvable symbol, or an
// installer registered by hand with no group path. A group is all or nothing,
// so false refuses the whole call rather than quietly installing the rest.
using GroupJoin = std::function<bool(GroupContribution&)>;

// ─── Build-configuration guard ────────────────────────────────────────────
// `library_context_size()` is defined in src/context.cpp, so it reports the
// layout the *compiled library* sees. SPLICE_REGISTRY_IMPL changes
// sizeof(HookContext), because it selects which registry type the context
// holds, and src/context.cpp owns the single HookContext every translation
// unit shares. SPLICE_MAX_HOOKS no longer does: since 2026-10-13 the slot
// arrays live behind pointers allocated in the library, so the capacity
// affects no layout a consumer can see -- which is why there is nothing left
// to detect about it.
// A consumer compiled with a different value would therefore read that
// instance's members at the wrong offsets: no diagnostic, no crash, until a
// function pointer read out of the originals table at the wrong offset is
// called. The CMake option exists so this cannot happen; the check exists
// because a hand-written -D can still try.
std::size_t library_context_size() noexcept;

// Templated so that `sizeof(ContextT)` is taken in the calling translation
// unit rather than wherever this header was first included.
//
// Returns false on disagreement rather than ending the process. Splice is
// loaded into someone else's program -- a game, an Android system service --
// and std::abort() there is a dead host, which on Windows with a Debug CRT is
// a dead host behind a modal dialog waiting for a human. A build mismatch is a
// build mistake, and the honest response to it is the same as for any other
// condition that makes installing impossible: log it and refuse. Callers must
// check this *before* touching any member of the shared context, which is what
// makes refusing safe -- on disagreement those members are not where this
// translation unit believes they are, so none of them is read.
template <typename ContextT>
[[nodiscard]] inline bool context_layout_agrees() noexcept {
    static const bool agrees = []() noexcept {
        const std::size_t library = library_context_size();
        if (library == sizeof(ContextT)) return true;
        SPLICE_LOGE("splice: build mismatch -- this translation unit sees "
                    "sizeof(HookContext)=%zu and libsplice was built with %zu. "
                    "SPLICE_REGISTRY_IMPL must be the same for the library and "
                    "every consumer; link splice::splice and it carries it. "
                    "Every hook from this translation unit is refused: reading "
                    "the shared context at these offsets would dispatch through "
                    "a pointer taken from the wrong place. (The capacity is no "
                    "longer part of this: it is a runtime value behind a "
                    "pointer, so a build that disagrees about SPLICE_MAX_HOOKS "
                    "has nothing here to misread.)",
                    sizeof(ContextT), library);
        return false;
    }();
    return agrees;
}

} // namespace detail

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

// ─── Refusal — why a registry access did not happen ────────────────────────
// Returned, never only logged. A caller that gets nothing back needs to know
// which of these it was: raise the capacity, look at memory pressure, or fix an
// id it computed itself are three different actions, and a log line is not an
// answer to a question the code has to act on.
enum class Refusal {
    ok,
    not_initialised,   // the registry storage could not be allocated
    out_of_range,      // the id is outside the configured capacity
    type_mismatch,     // the slot holds a hook of a different type
};

// `type_mismatch` is declared here and not produced yet: the slot has to carry
// the type identity it was created with before anything can compare it, which
// arrives with the checked dispatch path. Declaring it now keeps the one
// vocabulary rather than widening an enum later and making every caller's
// switch stale.

[[nodiscard]] inline const char* refusal_text(Refusal why) noexcept {
    switch (why) {
        case Refusal::ok:              return "ok";
        case Refusal::not_initialised: return "the registry storage was never allocated";
        case Refusal::out_of_range:    return "the id is outside the configured capacity";
        case Refusal::type_mismatch:   return "the slot holds a hook of a different type";
    }
    return "unknown";
}

// A value and why it is absent. `operator bool` tests the reason rather than
// the value, because "nothing is stored at this id" is a successful lookup and
// a null original is a legitimate answer to it -- a distinction the old
// pointer-returning accessors could not express at all.
template <typename T>
struct Checked {
    T       value{};
    Refusal reason = Refusal::ok;

    [[nodiscard]] explicit operator bool() const noexcept { return reason == Refusal::ok; }
};

// ─── HookBase ───────────────────────────────────────────────────────────────
// Empty virtual base — lets HookContext store `shared_ptr<HookBase>` as a
// heterogeneous container while each concrete `Hook<Ret,Args...>` carries
// its own type-safe invocation path.
class HookBase {
public:
    virtual ~HookBase() = default;
};

// ─── Slot storage, owned by the library ─────────────────────────────────────
// The configured capacity and the two slot arrays live behind these functions
// and nowhere else, which is what takes the capacity out of this header's ABI:
// no member layout, no inline expression and no template argument below
// depends on it, so a translation unit built with a different capacity has
// nothing it can misread.
//
// That is a deliberate retreat from detecting the disagreement. A runtime
// comparison cannot be made reliably private to one translation unit, a
// configuration-tagged symbol cannot reach code a consumer wrote inline in its
// own headers, and a value baked into a macro expansion is discarded with the
// body that carried it. Removing the dependency needs none of them to work.
//
// Allocation never throws and returns nullptr on failure, because its first
// caller is `default_context()`'s own construction -- which is noexcept, and
// which runs inside someone else's process.
namespace detail {

[[nodiscard]] int configured_capacity() noexcept;

[[nodiscard]] std::atomic<void*>* allocate_original_slots(int count) noexcept;
[[nodiscard]] std::atomic<HookBase*>* allocate_hook_slots(int count) noexcept;
void free_original_slots(std::atomic<void*>* slots) noexcept;
void free_hook_slots(std::atomic<HookBase*>* slots) noexcept;

} // namespace detail

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

    // A map takes any id, so this implementation imposes no capacity. The
    // runtime spelling exists so `HookContext` can ask both registries the
    // same question without knowing which one it has.
    [[nodiscard]] int capacity() const noexcept {
        return (std::numeric_limits<int>::max)();
    }
    [[nodiscard]] bool storage_ok() const noexcept { return true; }
    void release_storage() noexcept {}

private:
    mutable std::shared_mutex                                m_mutex;
    std::unordered_map<int, std::shared_ptr<HookBase>>       m_hooks;
};

// rcu_atomic_array — a flat array of atomic slot pointers, one per id, and
// nothing else. Reader is a single acquire load on a line nobody writes, which
// is why it stays Shared on every core and costs no coherence traffic.
//
// ── Why there is no snapshot any more (2026-10-13) ────────────────────────
// This specialisation used to publish an immutable snapshot and retire the
// previous one with a wall-clock timestamp, freeing it 100 ms later on the
// assumption that no reader could still be inside it. An external review
// refuted that: a reader loads the snapshot pointer and *then* dereferences
// the array, and a thread preempted between those two steps -- or stopped in a
// debugger, or descheduled on a loaded phone -- exceeds any grace period.
// `size()` was worse, traversing every slot of an unpinned snapshot. Elapsed
// time is not quiescence, and the hook objects surviving in the next
// snapshot's shared_ptr copies did not help: what the reader dereferences
// first is the array, and nothing kept that alive.
//
// Reclaiming it correctly needs reader registration or hazard pointers, which
// costs the hot path the whole reason this implementation exists. So nothing is
// reclaimed: one array for the life of the context, and hook objects that live
// for the life of the process. That is the same bargain the trampolines and the
// published callbacks already make here, and it is recorded in
// `.lsan-suppressions.txt` rather than hidden.
//
// Capacity is a runtime value from `detail::configured_capacity()`, not a
// template argument or an array bound, so nothing in this header's layout
// depends on it.
template <>
class HookRegistry<registry::rcu_atomic_array> {
public:
    HookRegistry() noexcept
        : m_slots(detail::allocate_hook_slots(detail::configured_capacity())),
          m_capacity(m_slots != nullptr ? detail::configured_capacity() : 0) {}

    ~HookRegistry() {
        // The slot array goes; the hooks in it do not. Nothing here can
        // establish that no thread holds a pointer to one -- that is the same
        // missing protocol the trampolines are never reclaimed for -- and at
        // teardown the process is ending anyway.
        detail::free_hook_slots(m_slots);
    }

    HookRegistry(const HookRegistry&) = delete;
    HookRegistry& operator=(const HookRegistry&) = delete;
    HookRegistry(HookRegistry&&) = delete;
    HookRegistry& operator=(HookRegistry&&) = delete;

    template <typename HookT>
    HookT& get_or_create(int id) {
        // ── Fast path — one acquire load, no atomic RMW, nothing to reclaim ──
        if (m_slots != nullptr && id >= 0 && id < m_capacity) {
            if (auto* hook = m_slots[id].load(std::memory_order_acquire)) {
                return *static_cast<HookT*>(hook);
            }
        }
        return install_slot<HookT>(id);
    }

    // Detach every slot without destroying anything already published: a
    // reader holding one of these pointers finishes against the object it
    // already has, and the next lookup creates a fresh one. Destroying them
    // here would reintroduce exactly the window this implementation no longer
    // has.
    void clear() noexcept {
        std::lock_guard lock(m_write_mutex);
        for (int i = 0; i < m_capacity; ++i) {
            m_slots[i].store(nullptr, std::memory_order_release);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::size_t n = 0;
        for (int i = 0; i < m_capacity; ++i) {
            if (m_slots[i].load(std::memory_order_acquire) != nullptr) ++n;
        }
        return n;
    }

    // The id is the index, so this is a hard bound -- but a runtime one.
    // `HookContext::slot_for` will not issue a slot at or beyond it.
    [[nodiscard]] int capacity() const noexcept { return m_capacity; }

    // False when the array could not be allocated. The context asks both
    // registries and refuses every registry-backed install if either says no,
    // so a half-built bound is never exposed.
    [[nodiscard]] bool storage_ok() const noexcept { return m_slots != nullptr; }

    void release_storage() noexcept {
        detail::free_hook_slots(m_slots);
        m_slots = nullptr;
        m_capacity = 0;
    }

private:
    template <typename HookT>
    HookT& install_slot(int id) {
        if (m_slots == nullptr || id < 0 || id >= m_capacity) {
            // Defensive, and deliberately not fatal. `slot_for` refuses to
            // issue a slot outside the bound, so a well-formed install cannot
            // arrive here; a caller passing a raw id can. It used to abort the
            // process, which turned a capacity limit into a dead consumer --
            // inside a game, or inside a system service.
            SPLICE_LOGE("rcu_atomic_array: hook id=%d outside [0, %d) -- "
                        "ignored. Raise SPLICE_MAX_HOOKS for a build that needs "
                        "more than %d hook sites.",
                        id, m_capacity, m_capacity);
            static HookT inert;
            return inert;
        }

        std::lock_guard lock(m_write_mutex);
        // Re-check inside the lock: another writer may have installed this
        // slot while we were waiting on m_write_mutex.
        if (auto* existing = m_slots[id].load(std::memory_order_acquire)) {
            return *static_cast<HookT*>(existing);
        }

        // Nothrow, because the whole point of the refusal path below is that a
        // hook library does not end a host it is only a guest in.
        auto* hook = new (std::nothrow) HookT();
        if (hook == nullptr) {
            SPLICE_LOGE("rcu_atomic_array: could not allocate the hook for "
                        "id=%d; this hook is refused and the slot stays empty.",
                        id);
            static HookT inert;
            return inert;
        }
        // Release, so a reader that acquires this pointer sees a constructed
        // object. That ordering is what the shared_ptr was providing by
        // accident of its own store, and it is now explicit.
        m_slots[id].store(hook, std::memory_order_release);
        return *hook;
    }

    std::atomic<HookBase*>* m_slots = nullptr;
    int                     m_capacity = 0;
    mutable std::mutex      m_write_mutex;   // serialises writers only
};

// ─── OriginalsRegistry<Impl> ───────────────────────────────────────────────
// Companion to HookRegistry — owns the per-id `void*` original-function-
// pointer table that the trampoline reads on every fire.
//
// Same trade-off framing as HookRegistry. shared_mutex_map keeps the
// original unordered_map<int, void*> + shared_mutex pattern. rcu_atomic_
// array uses an array of std::atomic<void*> indexed by id — reader is a
// single acquire-load, no atomic RMW, no map find.
//
// Bound: a runtime capacity, the same one the hook registry gets, read from
// `detail::configured_capacity()` rather than baked into this class's layout.
// Out-of-range ids are logged and ignored rather than crashing, and an install
// at such an id does not reach the trampoline.
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

    // As above: an unbounded map imposes no capacity.
    [[nodiscard]] int capacity() const noexcept {
        return (std::numeric_limits<int>::max)();
    }
    [[nodiscard]] bool storage_ok() const noexcept { return true; }
    void release_storage() noexcept {}

private:
    mutable std::shared_mutex          m_mutex;
    std::unordered_map<int, void*>     m_map;
};

template <>
class OriginalsRegistry<registry::rcu_atomic_array> {
public:
    // Same shape as the hook registry, for the same reason: a runtime capacity
    // behind a pointer, so this class's layout does not depend on the
    // configured value. It was a `std::array<std::atomic<void*>,
    // SPLICE_MAX_HOOKS>` until 2026-10-13, which made `sizeof(HookContext)`
    // depend on the capacity -- and made every consumer that disagreed about
    // it read the shared instance at the wrong offsets.
    //
    // Out of range is logged and ignored on both sides, as it has been since
    // the same day. Before that the two disagreed: the hooks side aborted the
    // process and this side returned, so one programming error killed a
    // program in two different ways depending on which registry reached it
    // first.
    OriginalsRegistry() noexcept
        : m_slots(detail::allocate_original_slots(detail::configured_capacity())),
          m_capacity(m_slots != nullptr ? detail::configured_capacity() : 0) {}

    ~OriginalsRegistry() { detail::free_original_slots(m_slots); }

    OriginalsRegistry(const OriginalsRegistry&) = delete;
    OriginalsRegistry& operator=(const OriginalsRegistry&) = delete;
    OriginalsRegistry(OriginalsRegistry&&) = delete;
    OriginalsRegistry& operator=(OriginalsRegistry&&) = delete;

    void set(int id, void* p) noexcept {
        if (m_slots == nullptr || id < 0 || id >= m_capacity) {
            SPLICE_LOGE("rcu_atomic_array originals: id=%d outside [0, %d) -- "
                        "ignored. Raise SPLICE_MAX_HOOKS for a build that needs "
                        "more than %d hook sites.",
                        id, m_capacity, m_capacity);
            return;   // the trampoline will see nullptr; the install is refused
        }
        m_slots[id].store(p, std::memory_order_release);
    }

    [[nodiscard]] void* get(int id) const noexcept {
        if (m_slots == nullptr || id < 0 || id >= m_capacity) return nullptr;
        return m_slots[id].load(std::memory_order_acquire);
    }

    void clear() noexcept {
        for (int i = 0; i < m_capacity; ++i) {
            m_slots[i].store(nullptr, std::memory_order_release);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::size_t n = 0;
        for (int i = 0; i < m_capacity; ++i) {
            if (m_slots[i].load(std::memory_order_relaxed) != nullptr) ++n;
        }
        return n;
    }

    [[nodiscard]] int capacity() const noexcept { return m_capacity; }
    [[nodiscard]] bool storage_ok() const noexcept { return m_slots != nullptr; }

    void release_storage() noexcept {
        detail::free_original_slots(m_slots);
        m_slots = nullptr;
        m_capacity = 0;
    }

private:
    std::atomic<void*>* m_slots = nullptr;
    int                 m_capacity = 0;
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
    // What went wrong at construction, if anything. Immutable afterwards.
    // `allocation_failed` is not a crash: the context is built, usable and
    // empty, and every registry-backed install is refused through the path
    // that already reports an exhausted capacity. Failing here instead would
    // mean failing inside `default_context()`, which is noexcept and runs in
    // someone else's process.
    enum class InitStatus { ok, allocation_failed };

    // Both slot arrays or neither. If either allocation failed, the other is
    // released and the capacity is zero for both, so nothing is ever exposed
    // with a half-established bound.
    HookContext() noexcept {
        if (!m_hook_registry.storage_ok() || !m_originals_registry.storage_ok()) {
            m_hook_registry.release_storage();
            m_originals_registry.release_storage();
            m_init_status = InitStatus::allocation_failed;
            m_slot_capacity = 0;
            SPLICE_LOGE("splice: could not allocate the registry slots for %d "
                        "hook sites. This context holds no hooks: every install "
                        "that needs a registry slot is refused, and the host is "
                        "left running.",
                        detail::configured_capacity());
            return;
        }
        const int hooks = m_hook_registry.capacity();
        const int originals = m_originals_registry.capacity();
        m_slot_capacity = hooks < originals ? hooks : originals;
    }

    ~HookContext() = default;

    // The bound a slot has to be valid in: the smaller of the two registries'
    // capacities, because the trampoline reads one of each per call. Zero when
    // the storage could not be allocated.
    [[nodiscard]] int slot_capacity() const noexcept { return m_slot_capacity; }
    [[nodiscard]] InitStatus init_status() const noexcept { return m_init_status; }

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

    // Checked. The order of these two tests is the contract and not an
    // accident: an unallocated registry is reported as such and never as an
    // invalid id, because a caller acts differently on the two.
    template <typename FuncType>
    [[nodiscard]] Refusal try_set_original(int id, FuncType original) {
        if (m_init_status != InitStatus::ok) return Refusal::not_initialised;
        if (id < 0 || id >= m_slot_capacity) return Refusal::out_of_range;
        m_originals_registry.set(id, reinterpret_cast<void*>(original));
        return Refusal::ok;
    }

    // A null value with `Refusal::ok` means the lookup worked and nothing is
    // stored there. That is a different fact from a refusal, and the
    // pointer-returning spelling below cannot tell them apart.
    template <typename FuncType>
    [[nodiscard]] Checked<FuncType> try_get_original(int id) const {
        if (m_init_status != InitStatus::ok) return {FuncType{}, Refusal::not_initialised};
        if (id < 0 || id >= m_slot_capacity) return {FuncType{}, Refusal::out_of_range};
        return {reinterpret_cast<FuncType>(m_originals_registry.get(id)), Refusal::ok};
    }

    // Kept for source and ABI continuity, and **it cannot report a refusal** --
    // there is nowhere in a `void` return for a reason to go. It logs one and
    // discards the value, which means a caller that passes an id it computed
    // itself can lose an original and only find out from a log. Prefer
    // `try_set_original`; the library's own paths use it. This is an accepted
    // limitation of a signature that is treated as stable, stated here rather
    // than left for a reader to discover.
    template <typename FuncType>
    void set_original(int id, FuncType original) {
        const Refusal why = try_set_original<FuncType>(id, original);
        if (why != Refusal::ok) {
            SPLICE_LOGE("splice: set_original(id=%d) refused -- %s. This "
                        "signature cannot report that, so the original is not "
                        "stored and the trampoline will see nullptr; use "
                        "try_set_original to get the reason back.",
                        id, refusal_text(why));
        }
    }

    // As above: a null return means either "nothing stored" or "refused", and
    // this spelling cannot say which. `try_get_original` can.
    template <typename FuncType>
    [[nodiscard]] FuncType get_original(int id) const {
        return try_get_original<FuncType>(id).value;
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
    // Checked, and the form the library's own paths use. Same order of tests
    // as the originals side, for the same reason.
    template <typename Policy, typename Ret, typename... Args>
    [[nodiscard]] Checked<HookAs<Policy, Ret, Args...>*> try_get_hook_as(int id) {
        using HookT = HookAs<Policy, Ret, Args...>;
        if (m_init_status != InitStatus::ok) return {nullptr, Refusal::not_initialised};
        if (id < 0 || id >= m_slot_capacity) return {nullptr, Refusal::out_of_range};
        return {&m_hook_registry.template get_or_create<HookT>(id), Refusal::ok};
    }

    // Kept for source and ABI continuity. A reference cannot carry a refusal,
    // so on one it logs and hands back the registry's shared inert slot --
    // which is why nothing should dispatch through this spelling. Accepted
    // limitation, same as `set_original` above; `try_get_hook_as` reports.
    template <typename Policy, typename Ret, typename... Args>
    HookAs<Policy, Ret, Args...>& get_hook_as(int id) {
        using HookT = HookAs<Policy, Ret, Args...>;
        auto checked = try_get_hook_as<Policy, Ret, Args...>(id);
        if (checked.value != nullptr) return *checked.value;
        SPLICE_LOGE("splice: get_hook_as(id=%d) refused -- %s. This signature "
                    "cannot report that; use try_get_hook_as. What comes back "
                    "is an inert slot shared by every refused id.",
                    id, refusal_text(checked.reason));
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
    // Returns -1 when no slot can be issued: the configured capacity is
    // exhausted, or this translation unit disagrees with the library about the
    // context's layout. Callers treat that as a refused install -- see
    // InterceptorEntry::refusal_reason() and the group's prepare stage.
    // Handing out a slot the registries cannot hold would publish a hook the
    // trampoline then fails to resolve, which is the one outcome worse than
    // not installing.
    //
    // The layout check comes first, before any member is touched, because on
    // disagreement the members are not at the offsets this translation unit
    // computed -- including the mutex the next line would lock.
    [[nodiscard]] int slot_for(void* trampoline_ptr) {
        if (!detail::context_layout_agrees<HookContext>()) return -1;
        std::unique_lock lock(m_mutex);
        auto it = m_slot_by_trampoline.find(trampoline_ptr);
        if (it != m_slot_by_trampoline.end()) return it->second;
        if (m_next_slot >= m_slot_capacity) {
            // Two conditions, one refusal, and they are not the same fact: a
            // consumer that sees this needs to know whether to raise the
            // capacity or to look at memory pressure.
            if (m_init_status == InitStatus::allocation_failed) {
                SPLICE_LOGE("splice: no registry slot can be issued -- the "
                            "registry storage was never allocated. This install "
                            "is refused; the host is unaffected.");
            } else {
                SPLICE_LOGE("splice: no registry slot left -- %d hook sites are "
                            "in use and that is the configured capacity. Rebuild "
                            "with a larger SPLICE_MAX_HOOKS; this install is "
                            "refused rather than guessed at.", m_slot_capacity);
            }
            return -1;
        }
        const int slot = m_next_slot++;
        m_slot_by_trampoline[trampoline_ptr] = slot;
        return slot;
    }

    // ─── Installer queue ─────────────────────────────────────────────────
    // Slot-based: each registered lambda gets a stable index, returned to
    // the caller as an InstallerToken. Token destruction marks the slot
    // dead — see Task #57. m_free_slots recycles indices so long-running
    // processes don't leak vector growth.

    [[nodiscard]] InstallerToken register_installer(std::function<void()> fn,
                                                    detail::GroupJoin join = nullptr) {
        std::unique_lock lock(m_mutex);
        int slot;
        if (!m_free_slots.empty()) {
            slot = m_free_slots.back();
            m_free_slots.pop_back();
            auto& s = m_installers[static_cast<std::size_t>(slot)];
            s.fn = std::move(fn);
            s.join = std::move(join);
            s.live = true;
        } else {
            slot = static_cast<int>(m_installers.size());
            m_installers.push_back({std::move(fn), std::move(join), true});
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
        s.join = nullptr;                    // and the group path's, same reason
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

    // The group paths of every live installer, snapshotted the same way and
    // for the same reason: a join resolves a symbol and reads the registry, so
    // it must not run under this lock.
    //
    // A live installer with no group path contributes a null here rather than
    // being dropped, because the caller has to be able to tell "nothing was
    // queued" from "something was queued that cannot join a group". The first
    // is an empty call; the second is a refusal.
    [[nodiscard]] std::vector<detail::GroupJoin> group_joins() const {
        std::vector<detail::GroupJoin> local;
        std::shared_lock lock(m_mutex);
        local.reserve(m_installers.size());
        for (auto& s : m_installers) {
            if (s.live) local.push_back(s.join);
        }
        return local;
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
        detail::GroupJoin     join;     // null: cannot join a coordinated group
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
    int m_slot_capacity = 0;
    InitStatus m_init_status = InitStatus::ok;
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
