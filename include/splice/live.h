// ─── Splice live — hooking a whole interface as one thing ──────────────────
//
// One new noun and one new adverb:
//
//     splice::Vtable<IDXGISwapChain3>{chain}
//         .on(&IDXGISwapChain3::Present,       present_cb)
//         .on(&IDXGISwapChain3::ResizeBuffers, resize_cb)
//         .non_reentrant()                 // one gate for the whole group
//         .install(splice::slot_swap);      // all or nothing
//
// `on` deduces the signature from the pointer-to-member, so the callback is
// compile-checked against HRESULT(IDXGISwapChain3*, UINT, UINT). A wrong
// argument type is a build error, which is the reason this library exists.
//
// Two tiers, named honestly, because they are not interchangeable:
//
//   splice::slot_swap  store a pointer into the vtable slot. Needs no thread
//                      coordination -- it is one pointer-sized atomic write.
//                      Reaches only that vtable, and another overlay swapping
//                      the same slot displaces us or we displace it.
//   splice::live       inline-patch the function the slot points at. Survives
//                      other overlays' slot swaps, and needs the execution
//                      coordination that is not here yet -- install(live)
//                      refuses, in as many words, rather than quietly doing a
//                      slot swap instead. A tier that silently becomes a
//                      weaker tier is worse than one that says no.
//
// What a group buys over N separate hooks, even on this tier: the sites are
// written together and undone together, so a failure at site 5 cannot leave
// sites 0..4 installed with no record of them anywhere.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <splice/context.h>
#include <splice/engine.h>
#include <splice/gates.h>
#include <splice/log.h>
#include <splice/policy.h>
#include <splice/reentrancy.h>
#include <splice/registry.h>
#include <splice/traits.h>
#include <splice/trampoline.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

// How many live sites one signature can carry at a time. Each one is a
// distinct stub function, instantiated at compile time, and a group claims one
// per hooked method. Raise it if a refusal says you have run out; eight covers
// every interface this library has been pointed at, and the DXGI overlay that
// drove the design uses six methods across five signatures.
#ifndef SPLICE_SLOT_STUBS_PER_SIGNATURE
#   define SPLICE_SLOT_STUBS_PER_SIGNATURE 8
#endif

namespace splice {

// ─── tiers ────────────────────────────────────────────────────────────────
struct slot_swap_t { };
struct live_t      { };
inline constexpr slot_swap_t slot_swap{};
inline constexpr live_t      live{};

// ─── what came of the attempt ─────────────────────────────────────────────
enum class State {
    installed,   // every site hooked
    refused,     // nothing written; targets untouched -- safe to retry
    recovered,   // the attempt failed and splice undid it -- safe to retry
    lost,        // splice could not restore the world -- the caller's problem
};

// Where an attempt stopped. The slot_swap tier reaches only `prepare` and
// `write`; the rest belong to the coordinated tier and are named here because
// they are the vocabulary of the design, not because anything produces them
// yet.
enum class Stage { none, prepare, enlist, map, write, migrate, flush, resume };

// What was being attempted when it stopped.
enum class Operation {
    none, resolve_slot, read_slot, write_slot, restore_slot,
    enumerate_threads, suspend_thread, capture_pc, set_pc, resume_thread,
};

// Which member of the group. `ordinal` counts .on() calls from zero, so it
// identifies the site even when the slot could not be resolved.
struct Site {
    std::size_t ordinal    = 0;
    int         slot_index = -1;
};

// POD, no allocation, filled by splice. Rich enough that nobody has to wrap
// the seam to find out what happened.
struct Failure {
    Site          site{};
    Stage         stage     = Stage::none;
    Operation     operation = Operation::none;
    std::uint32_t thread_id = 0;      // 0 when not thread-specific
    std::uint32_t os_error  = 0;
};

namespace detail {

// ─── the stub pool ────────────────────────────────────────────────────────
//
// A hooked vtable slot has to hold the address of a function with the target's
// exact signature, and that function has to know WHICH site it is in order to
// find its callback and its original. splice already solves that: a
// TrampolineGenerator instantiation is a distinct function whose own address
// is its identity (HookContext::slot_for), which is how every SPLICE_HOOK call
// site works. A macro gets its instantiation from __LINE__ and __COUNTER__; a
// runtime chain has neither, so it claims one from a small pool instead.
//
// The pool's ids are 0..SPLICE_SLOT_STUBS_PER_SIGNATURE-1, and a macro call
// site can never collide with them: SPLICE_UNIQUE_ID is
// `(__LINE__ << 16) | (__COUNTER__ & 0xFFFF)`, and __LINE__ is at least 1, so
// every id a macro produces is at least 0x10000. That is the same assumption
// about line numbers macros.h already documents and relies on.
//
// Identical-code folding does not merge these, although their bodies look
// alike: each instantiation reads its own function-local statics, so the code
// bytes reference different symbols and the linker cannot fold them. If that
// ever stopped being true, two sites would share one registry slot -- the
// cross-aliasing FR-010 Step 6.4 fixed for __COUNTER__ collisions, arriving by
// a different road.

template <typename FuncType, typename Policy, std::size_t... I>
void* const* stub_table(std::index_sequence<I...>) {
    static void* const table[] = {
        TrampolineGenerator<FuncType, static_cast<int>(I), Policy>::get_trampoline_ptr()...
    };
    return table;
}

template <typename FuncType, typename Policy>
class StubPool {
public:
    static constexpr std::size_t kCount = SPLICE_SLOT_STUBS_PER_SIGNATURE;

    // The next unclaimed stub for this signature, or nullptr when they are all
    // in use. A claim lasts until the group that made it is disabled.
    static void* claim() {
        void* const* table =
            stub_table<FuncType, Policy>(std::make_index_sequence<kCount>{});
        for (std::size_t i = 0; i < kCount; ++i) {
            bool expected = false;
            if (claimed()[i].compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
                return table[i];
            }
        }
        return nullptr;
    }

    static void release(void* stub) {
        if (stub == nullptr) return;
        void* const* table =
            stub_table<FuncType, Policy>(std::make_index_sequence<kCount>{});
        for (std::size_t i = 0; i < kCount; ++i) {
            if (table[i] == stub) {
                claimed()[i].store(false, std::memory_order_release);
                return;
            }
        }
    }

private:
    // Zero-initialised before any dynamic initialisation runs, so there is no
    // first-claim race to lose.
    static std::atomic<bool>* claimed() {
        static std::atomic<bool> flags[kCount]{};
        return flags;
    }
};

// ─── binding one site, with its signature in hand ─────────────────────────
template <typename FuncType, typename Policy>
struct SiteBinder;      // undefined: only the function-pointer form binds

template <typename Ret, typename... Args, typename Policy>
struct SiteBinder<Ret (*)(Args...), Policy> {
    using FuncType = Ret (*)(Args...);
    using Invoke   = std::function<Ret(FuncType, Args...)>;

    // .before(f) -- f cannot suppress the call, so it is handed no original.
    template <typename Fn>
    static Invoke from_before(Fn f) {
        return Invoke([f = std::move(f)](FuncType orig, Args... args) -> Ret {
            f(args...);
            return orig(args...);
        });
    }

    // .after(f) -- f sees the return value, except when there is not one.
    template <typename Fn>
    static Invoke from_after(Fn f) {
        if constexpr (std::is_void_v<Ret>) {
            return Invoke([f = std::move(f)](FuncType orig, Args... args) {
                orig(args...);
                f(args...);
            });
        } else {
            return Invoke([f = std::move(f)](FuncType orig, Args... args) -> Ret {
                Ret result = orig(args...);
                f(result, args...);
                return result;
            });
        }
    }

    // Publish the original and the callback for a stub's registry slot. Called
    // BEFORE the slot is swapped, every time: a thread that observes the new
    // slot value must find a complete hook behind it.
    static void publish(int registry_slot, void* original, Gates gates, Invoke fn) {
        default_context().template set_original<FuncType>(
            registry_slot, reinterpret_cast<FuncType>(original));
        auto& hook =
            default_context().template get_hook_as<Policy, Ret, Args...>(registry_slot);
        if (gates.transparent()) {
            hook.set_invoke(std::move(fn));
        } else {
            hook.set_invoke(GateWrap<Ret, FuncType, Args...>::apply(
                std::move(gates), std::move(fn)));
        }
    }
};

// One site, between .on() and .install().
struct PendingSite {
    Site  site{};
    void* stub          = nullptr;
    int   registry_slot = -1;
    void* expected      = nullptr;    // .expect(addr): refuse unless the slot holds this
    bool  expect_set    = false;

    // Keeps the site's signature and callback alive without the group having
    // to know either. `group_bit` is the re-entrancy scope the whole group
    // shares, applied unless this site named one of its own.
    std::function<void(void* original, std::uint64_t group_bit)> publish;
    std::function<void()> release_stub;
};

// A site that is installed, and what it takes to undo it.
struct InstalledSite {
    void**                slot = nullptr;
    void*                 original = nullptr;
    splice_patch_record   record{};
    std::function<void()> release_stub;
};

// Survives the builder. Destroying it does NOT uninstall: a chain whose
// Outcome is discarded leaves its hooks in place, exactly as a
// SPLICE_HOOK_STATIC call site does. disable() is the way back.
struct GroupState {
    std::vector<InstalledSite> sites;
    bool installed = false;
};

} // namespace detail

// ─── the result the caller can act on ─────────────────────────────────────
class Outcome {
public:
    Outcome() = default;
    Outcome(State state, Failure failure) : m_state(state), m_failure(failure) {}

    [[nodiscard]] State   state()   const noexcept { return m_state; }
    [[nodiscard]] Failure failure() const noexcept { return m_failure; }
    [[nodiscard]] bool    ok()      const noexcept { return m_state == State::installed; }

    // Whether the hooks are in place NOW, which is a different question from
    // what the attempt returned: state() is the verdict on the install and
    // never changes, active() goes false when disable() succeeds. The same
    // distinction InterceptorEntry::is_installed() draws against its own
    // install, and for the same reason -- a caller that conflated them would
    // either disable twice or never.
    [[nodiscard]] bool active() const noexcept {
        return m_group != nullptr && m_group->installed;
    }
    [[nodiscard]] std::size_t installed_sites() const noexcept {
        return m_group ? m_group->sites.size() : 0;
    }

    // Put every slot in the group back, and release the stubs the group
    // claimed. Like every other disable in splice this is a switch and not a
    // drain: a call already inside a callback carries on to completion.
    //
    // Returns true when every site was restored. False means at least one slot
    // still holds our stub, and no stub is released in that case -- a stub
    // something may still call is not free.
    bool disable() noexcept {
        if (!m_group || !m_group->installed) return false;
        bool all = true;
        for (auto& site : m_group->sites) {
            if (splice_disable(&site.record) != 0) {
                all = false;
            }
        }
        if (!all) return false;
        for (auto& site : m_group->sites) {
            if (site.release_stub) site.release_stub();
        }
        m_group->installed = false;
        m_group->sites.clear();
        return true;
    }

private:
    template <typename T, typename Policy> friend class Vtable;

    State                               m_state = State::refused;
    Failure                             m_failure{};
    std::shared_ptr<detail::GroupState> m_group;
};

template <typename T, typename Policy> class Vtable;

// ─── one member of the group, while its modifiers are being set ───────────
//
// Returned by `.on(pmf)` without a callback, so the per-method gates can be
// spelled out. Every action verb returns the group, so the chain carries on:
//
//     .on(&I::ResizeBuffers).when(enabled).before(note_resize)
//     .on(&I::Present, present_cb)
//
template <typename Owner, typename PMF>
class VtableSite {
public:
    using FuncType = member_free_fn_t<PMF>;
    using Binder   = detail::SiteBinder<FuncType, typename Owner::policy_type>;

    // The pointer-to-member is taken by value and not kept: its only job is to
    // deduce PMF, which is what the signature and the callback check come
    // from. The slot it names was resolved before this site existed.
    VtableSite(Owner& owner, PMF, int slot_index)
        : m_owner(owner), m_slot_index(slot_index) {}

    VtableSite& when(std::function<bool()> pred) {
        m_gates.when_pred = std::move(pred);
        return *this;
    }
    VtableSite& once() { return times(1); }
    VtableSite& times(long long n) {
        m_gates.remaining = std::make_shared<std::atomic<long long>>(n);
        return *this;
    }
    VtableSite& observe() {
        m_invocations = std::make_shared<std::atomic<std::uint64_t>>(0);
        m_gates.invocations = m_invocations;
        return *this;
    }
    // This method only. The group-wide form is Vtable::non_reentrant(), which
    // is what a set of methods that call one another wants.
    VtableSite& non_reentrant() {
        auto scope = std::make_shared<Reentrancy>();
        m_gates.reentry_bit = scope->bit();
        m_gates.owned_scope = std::move(scope);
        m_gates.reentry_requested = true;
        return *this;
    }
    VtableSite& non_reentrant(const Reentrancy& scope) {
        m_gates.reentry_bit = scope.bit();
        m_gates.owned_scope.reset();
        m_gates.reentry_requested = true;
        return *this;
    }
    // Refuse to install unless the slot still holds this address. The same
    // check the shipping consumer writes by hand against literal indices,
    // which is the reason it exists.
    VtableSite& expect(void* address) {
        m_expected = address;
        m_expect_set = true;
        return *this;
    }

    // The counter .observe() installed, so the caller can read it after the
    // chain has moved on. Null when this site was never told to observe.
    [[nodiscard]] std::shared_ptr<std::atomic<std::uint64_t>> counter() const {
        return m_invocations;
    }

    template <typename Fn>
    Owner& onInvoke(Fn fn) {
        return commit(typename Binder::Invoke(std::move(fn)));
    }
    template <typename Fn>
    Owner& before(Fn fn) { return commit(Binder::from_before(std::move(fn))); }
    template <typename Fn>
    Owner& after(Fn fn) { return commit(Binder::from_after(std::move(fn))); }

private:
    Owner& commit(typename Binder::Invoke fn) {
        m_owner.template add_site<FuncType>(m_slot_index, std::move(m_gates),
                                            std::move(fn), m_expected, m_expect_set);
        return m_owner;
    }

    Owner&        m_owner;
    int           m_slot_index;
    detail::Gates m_gates;
    void*         m_expected = nullptr;
    bool          m_expect_set = false;
    std::shared_ptr<std::atomic<std::uint64_t>> m_invocations;
};

// ─── the group ────────────────────────────────────────────────────────────
template <typename T, typename Policy = SPLICE_DEFAULT_POLICY>
class Vtable {
public:
    using policy_type = Policy;

    // Reads the object's function table. The object must be polymorphic and
    // must keep its table pointer first, which is what a COM interface and a
    // single-inheritance C++ interface both guarantee.
    explicit Vtable(T* object) {
        static_assert(std::is_polymorphic_v<T>,
                      "splice::Vtable needs a class with virtual functions");
        if (object == nullptr) {
            SPLICE_LOGE("splice::Vtable: null object");
            fail(Stage::prepare, Operation::read_slot, 0);
            return;
        }
        std::memcpy(&m_table, static_cast<void*>(object), sizeof(m_table));
        if (m_table == nullptr) {
            SPLICE_LOGE("splice::Vtable: object at %p has no function table",
                        static_cast<void*>(object));
            fail(Stage::prepare, Operation::read_slot, 0);
        }
    }

    Vtable(const Vtable&) = delete;
    Vtable& operator=(const Vtable&) = delete;

    // .on(pmf) -- gates and action verb follow.
    template <typename PMF>
    VtableSite<Vtable, PMF> on(PMF pmf) {
        return VtableSite<Vtable, PMF>(*this, pmf, resolve(pmf));
    }

    // .on(pmf, callback) -- the common case, invoke-shaped.
    template <typename PMF, typename Fn>
    Vtable& on(PMF pmf, Fn fn) {
        return on(pmf).onInvoke(std::move(fn));
    }

    // The escape hatch for a slot splice cannot resolve: name the index, keep
    // the type safety. The pointer-to-member is still required, and still
    // deduces the signature the callback is checked against.
    template <typename PMF>
    VtableSite<Vtable, PMF> on_slot(int slot_index, PMF pmf) {
        return VtableSite<Vtable, PMF>(*this, pmf, slot_index);
    }
    template <typename PMF, typename Fn>
    Vtable& on_slot(int slot_index, PMF pmf, Fn fn) {
        return on_slot(slot_index, pmf).onInvoke(std::move(fn));
    }

    // One re-entrancy scope for the whole group, which is what the problem
    // actually is: the re-entry arrives through a DIFFERENT method on the same
    // object. Present1's implementation calls Present, both are hooked, and
    // without this the overlay draws twice or recurses.
    //
    // Allowed after the .on() calls, like every other gate: it is applied when
    // the group installs, not where it is spelled.
    Vtable& non_reentrant() {
        m_group_scope = std::make_shared<Reentrancy>();
        m_group_reentry_requested = true;
        return *this;
    }
    Vtable& non_reentrant(const Reentrancy& scope) {
        m_group_scope.reset();
        m_group_bit = scope.bit();
        m_group_reentry_requested = true;
        return *this;
    }

    // ─── install ──────────────────────────────────────────────────────────
    Outcome install(slot_swap_t) {
        if (m_failed) return refusal();
        if (m_sites.empty()) {
            SPLICE_LOGE("splice::Vtable::install: no methods were named");
            fail(Stage::prepare, Operation::none, 0);
            return refusal();
        }
        const std::uint64_t group_bit =
            m_group_scope ? m_group_scope->bit() : m_group_bit;
        if (m_group_reentry_requested && group_bit == 0) {
            SPLICE_LOGE("splice::Vtable::non_reentrant: no re-entrancy scope available "
                        "(64 in use); the group will not install");
            fail(Stage::prepare, Operation::none, 0);
            return refusal();
        }

        auto group = std::make_shared<detail::GroupState>();
        group->sites.reserve(m_sites.size());

        for (auto& pending : m_sites) {
            void** slot = m_table + pending.site.slot_index;
            void* original = *slot;
            if (original == nullptr) {
                SPLICE_LOGE("splice::Vtable::install: slot %d of table %p is empty",
                            pending.site.slot_index, static_cast<void*>(m_table));
                return undo(group, pending.site, Operation::read_slot);
            }
            if (pending.expect_set && original != pending.expected) {
                SPLICE_LOGE("splice::Vtable::install: slot %d holds %p, not the "
                            "expected %p", pending.site.slot_index, original,
                            pending.expected);
                return undo(group, pending.site, Operation::read_slot);
            }

            // Publish first, swap second. A reader that sees the new pointer
            // must find the original and the callback already there; the other
            // order is a live call through a null original, which this project
            // has paid for once already (FR-011 Phase 4.5d).
            pending.publish(original, group_bit);

            splice_patch_record record{};
            if (splice_vtable_swap(slot, pending.stub, nullptr, &record) != 0) {
                return undo(group, pending.site, Operation::write_slot);
            }
            group->sites.push_back(detail::InstalledSite{
                slot, original, record, pending.release_stub});
        }

        group->installed = true;
        Outcome outcome(State::installed, Failure{});
        outcome.m_group = std::move(group);
        SPLICE_LOGI("splice::Vtable: %zu slot(s) installed in table %p",
                    m_sites.size(), static_cast<void*>(m_table));
        m_sites.clear();
        return outcome;
    }

    // The coordinated tier. It refuses, and says so, rather than installing
    // the weaker tier behind the caller's back.
    Outcome install(live_t) {
        SPLICE_LOGE("splice::Vtable::install(live): the coordinated tier is not "
                    "implemented yet -- it needs the thread transaction. Use "
                    "install(splice::slot_swap), which hooks the vtable slot "
                    "without suspending anything, and read what that does not "
                    "cover before relying on it.");
        fail(Stage::prepare, Operation::none, 0);
        return refusal();
    }

    // Used by VtableSite. Public only because the site is a separate class;
    // nothing else should call it.
    template <typename FuncType, typename Fn>
    void add_site(int slot_index, detail::Gates gates, Fn fn,
                  void* expected, bool expect_set) {
        using Binder = detail::SiteBinder<FuncType, Policy>;
        const Site site{m_sites.size(), slot_index};

        if (m_failed) return;
        if (slot_index < 0) {
            SPLICE_LOGE("splice::Vtable::on: this pointer-to-member could not be "
                        "resolved to a vtable slot; name the index with "
                        ".on_slot(index, pmf, callback)");
            fail(Stage::prepare, Operation::resolve_slot, 0, site);
            return;
        }
        if (gates.unsatisfiable()) {
            SPLICE_LOGE("splice::Vtable::non_reentrant: no re-entrancy scope available "
                        "for slot %d (64 in use)", slot_index);
            fail(Stage::prepare, Operation::none, 0, site);
            return;
        }

        void* stub = detail::StubPool<FuncType, Policy>::claim();
        if (stub == nullptr) {
            SPLICE_LOGE("splice::Vtable::on: no stub left for this signature "
                        "(%d in use); raise SPLICE_SLOT_STUBS_PER_SIGNATURE",
                        static_cast<int>(detail::StubPool<FuncType, Policy>::kCount));
            fail(Stage::prepare, Operation::none, 0, site);
            return;
        }

        detail::PendingSite pending;
        pending.site = site;
        pending.stub = stub;
        pending.registry_slot = default_context().slot_for(stub);
        pending.expected = expected;
        pending.expect_set = expect_set;
        pending.release_stub = [stub]() {
            detail::StubPool<FuncType, Policy>::release(stub);
        };
        pending.publish = [slot = pending.registry_slot, gates = std::move(gates),
                           fn = std::move(fn)](void* original,
                                               std::uint64_t group_bit) mutable {
            detail::Gates site_gates = gates;
            // A scope named on the method itself wins; otherwise the group's
            // applies. Nothing merges two scopes: a hook is inside one scope
            // or inside none.
            if (!site_gates.reentry_requested && group_bit != 0) {
                site_gates.reentry_bit = group_bit;
                site_gates.reentry_requested = true;
            }
            Binder::publish(slot, original, std::move(site_gates), fn);
        };
        m_sites.push_back(std::move(pending));
    }

private:
    template <typename PMF>
    static int resolve(PMF pmf) {
        unsigned char raw[sizeof(PMF)];
        std::memcpy(raw, &pmf, sizeof(PMF));
        return splice_vtable_slot_of_pmf(raw, static_cast<unsigned int>(sizeof(PMF)));
    }

    void fail(Stage stage, Operation operation, std::uint32_t os_error,
              Site site = Site{}) {
        if (m_failed) return;          // keep the first failure, not the last
        m_failed = true;
        m_failure.site = site;
        m_failure.stage = stage;
        m_failure.operation = operation;
        m_failure.os_error = os_error;
    }

    [[nodiscard]] Outcome refusal() {
        release_pending();
        return Outcome(State::refused, m_failure);
    }

    // One site failed: put every site already written back before anything
    // else happens, and report `recovered` -- the attempt is undone and
    // retrying is safe. This is the whole point of a group. Installing one site
    // at a time can leave five of seven hooks live with nothing holding a
    // record of them.
    Outcome undo(const std::shared_ptr<detail::GroupState>& group, Site site,
                 Operation operation) {
        fail(Stage::write, operation, 0, site);
        if (group->sites.empty()) {
            // The first site failed, so there is nothing to undo and nothing
            // was published. `refused` and `recovered` are different answers
            // and collapsing them would cost the caller the one fact it gets
            // for free here: the targets were never touched at all.
            release_pending();
            return Outcome(State::refused, m_failure);
        }
        bool all = true;
        for (auto& installed : group->sites) {
            if (splice_disable(&installed.record) != 0) {
                all = false;
                SPLICE_LOGE("splice::Vtable::install: could not restore slot %p "
                            "after failing at ordinal %zu",
                            static_cast<void*>(installed.slot), site.ordinal);
                continue;
            }
            if (installed.release_stub) installed.release_stub();
        }
        group->sites.clear();
        release_pending();
        return Outcome(all ? State::recovered : State::lost, m_failure);
    }

    // Stubs claimed by sites that never got installed go back to the pool.
    void release_pending() {
        for (auto& pending : m_sites) {
            if (pending.release_stub) pending.release_stub();
        }
        m_sites.clear();
    }

    void**                           m_table = nullptr;
    std::vector<detail::PendingSite> m_sites;
    std::shared_ptr<Reentrancy>      m_group_scope;
    std::uint64_t                    m_group_bit = 0;
    bool                             m_group_reentry_requested = false;
    bool                             m_failed = false;
    Failure                          m_failure{};
};

} // namespace splice
