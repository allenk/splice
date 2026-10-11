// ─── Splice live — hooking a whole interface as one thing ──────────────────
//
// One new noun and one new adverb:
//
//     splice::Vtable<IDXGISwapChain3>{chain}
//         .on(&IDXGISwapChain3::Present,       present_cb)
//         .on(&IDXGISwapChain3::ResizeBuffers, resize_cb)
//         .non_reentrant()                 // one gate for the whole group
//         .install(splice::live);           // all or nothing
//
// `on` deduces the signature from the pointer-to-member, so the callback is
// compile-checked against HRESULT(IDXGISwapChain3*, UINT, UINT). A wrong
// argument type is a build error, which is the reason this library exists.
//
// Not every live target is a COM method. The group is the general noun and
// `Vtable` is one way to fill it:
//
//     splice::Live group;
//     group.at(present_addr, Present_t{}, present_cb)
//          .at(signal_addr,  Signal_t{},  signal_cb)
//          .install(splice::live);
//
// Two tiers, named honestly, because they are not interchangeable:
//
//   splice::slot_swap  store a pointer into the vtable slot. Needs no thread
//                      coordination -- it is one pointer-sized atomic write.
//                      Reaches that vtable, and another overlay swapping the
//                      same slot displaces us or we displace it. There is no
//                      slot to swap for a plain address, so a group named that
//                      way refuses this tier.
//   splice::live       inline-patch the function the slot points at. Survives
//                      other overlays' slot swaps, because every existing slot
//                      pointer stays valid and the call still arrives. Needs
//                      the thread coordination the engine half performs: every
//                      other thread suspended once, any program counter inside
//                      the bytes being replaced mapped through the trampoline,
//                      all sites written, resume.
//
// Neither tier is scoped to an object, and `Vtable` takes one, so: a class has
// one vtable shared by every instance, so a slot swap made through one object
// is seen by all of them -- measured on two swap chains from one device, which
// share a table pointer. The coordinated tier is wider again, patching the
// function itself, so every table pointing at it and every direct caller
// arrive at the callback. Compare identity in the callback when one object is
// what you meant.
//
// What a group buys over N separate hooks, on either tier: the sites are
// written together and undone together, so a failure at the fifth cannot leave
// four installed with no record of them anywhere. On the live tier it also buys
// one suspension instead of N, and one thread enumeration instead of N -- and a
// thread enumeration costs about 118 ms, because the OS walks every thread on
// the machine rather than this process's.
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

// Whether the coordinated tier can run here at all. It needs an architecture
// whose prologue can be planned and an OS that can suspend a thread and move
// its program counter; today that is x86_64 on Windows. Ask before offering
// the tier to a user, rather than learning it from a refusal.
[[nodiscard]] inline bool live_tier_available() noexcept {
    return splice_live_supported() != 0;
}

// ─── what came of the attempt ─────────────────────────────────────────────
enum class State {
    installed,   // every site hooked
    refused,     // nothing written; targets untouched -- safe to retry
    recovered,   // the attempt failed and splice undid it -- safe to retry
    lost,        // splice could not restore the world -- the caller's problem
};

// Where an attempt stopped. The slot_swap tier reaches only `prepare` and
// `write`; the rest belong to the coordinated tier.
enum class Stage { none, prepare, enlist, map, write, migrate, flush, resume };

// What was being attempted when it stopped.
enum class Operation {
    none, resolve_slot, read_slot, write_slot, restore_slot,
    enumerate_threads, suspend_thread, capture_pc, set_pc, resume_thread,
};

// Which member of the group. `ordinal` counts the sites in the order they were
// named, so it identifies one even when its slot could not be resolved.
struct Site {
    std::size_t ordinal    = 0;
    int         slot_index = -1;   // -1 for a site named by address
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
// A hooked site has to hold the address of a function with the target's exact
// signature, and that function has to know WHICH site it is in order to find
// its callback and its original. splice already solves that: a
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
    // BEFORE anything is written, every time: a thread that observes the new
    // code must find a complete hook behind it.
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

// One site, between the chain and .install().
//
// `slot` is the vtable entry, when the site was named through one, and
// `target` is the function address, when it was named directly. Exactly one of
// them is set: a group named through an interface reads its target out of the
// slot at install time, because that is also when `.expect` is checked.
struct PendingSite {
    Site   site{};
    void** slot          = nullptr;
    void*  target        = nullptr;
    void*  stub          = nullptr;
    int    registry_slot = -1;
    void*  expected      = nullptr;   // .expect(addr): refuse unless this is there
    bool   expect_set    = false;

    // Keeps the site's signature and callback alive without the group having
    // to know either. `group_bit` is the re-entrancy scope the whole group
    // shares, applied unless this site named one of its own.
    std::function<void(void* original, std::uint64_t group_bit)> publish;
    std::function<void()> release_stub;
};

// A site that is installed, and what it takes to undo it.
struct InstalledSite {
    void*                 where = nullptr;    // the slot or the target, for logging
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
    [[nodiscard]] std::size_t installed_sites() const noexcept {
        return m_group ? m_group->sites.size() : 0;
    }

    // Whether the hooks are in place NOW, which is a different question from
    // what the attempt returned: state() is the verdict on the install and
    // never changes, active() goes false when disable() succeeds. The same
    // distinction InterceptorEntry::is_installed() draws against its own
    // install, and for the same reason -- a caller that conflated them would
    // either disable twice or never.
    [[nodiscard]] bool active() const noexcept {
        return m_group != nullptr && m_group->installed;
    }

    // Put every site in the group back, and release the stubs the group
    // claimed. Like every other disable in splice this is a switch and not a
    // drain: a call already inside a callback carries on to completion.
    //
    // Returns true when every site was restored. False means at least one is
    // still live, and no stub is released in that case -- a stub something may
    // still call is not free.
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
    template <typename Derived, typename Policy> friend class GroupBuilder;

    State                               m_state = State::refused;
    Failure                             m_failure{};
    std::shared_ptr<detail::GroupState> m_group;
};

// ─── one member of the group, while its modifiers are being set ───────────
//
// Returned when a site is named without a callback, so the per-site gates can
// be spelled out. Every action verb returns the group, so the chain carries
// on:
//
//     .on(&I::ResizeBuffers).when(enabled).before(note_resize)
//     .on(&I::Present, present_cb)
//
template <typename Owner, typename FuncType>
class GroupSite {
public:
    using Binder = detail::SiteBinder<FuncType, typename Owner::policy_type>;

    GroupSite(Owner& owner, void** slot, void* target, int slot_index)
        : m_owner(owner), m_slot(slot), m_target(target), m_slot_index(slot_index) {}

    GroupSite& when(std::function<bool()> pred) {
        m_gates.when_pred = std::move(pred);
        return *this;
    }
    GroupSite& once() { return times(1); }
    GroupSite& times(long long n) {
        m_gates.remaining = std::make_shared<std::atomic<long long>>(n);
        return *this;
    }
    GroupSite& observe() {
        m_invocations = std::make_shared<std::atomic<std::uint64_t>>(0);
        m_gates.invocations = m_invocations;
        return *this;
    }
    // This site only. The group-wide form is the group's own
    // non_reentrant(), which is what a set of methods that call one another
    // wants.
    GroupSite& non_reentrant() {
        auto scope = std::make_shared<Reentrancy>();
        m_gates.reentry_bit = scope->bit();
        m_gates.owned_scope = std::move(scope);
        m_gates.reentry_requested = true;
        return *this;
    }
    GroupSite& non_reentrant(const Reentrancy& scope) {
        m_gates.reentry_bit = scope.bit();
        m_gates.owned_scope.reset();
        m_gates.reentry_requested = true;
        return *this;
    }
    // Refuse to install unless the slot still holds this address. The same
    // check the shipping consumer writes by hand against literal indices,
    // which is the reason it exists. Meaningless for a site named by address,
    // where there is no slot to read.
    GroupSite& expect(void* address) {
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
        m_owner.template add_pending<FuncType>(
            m_slot_index, m_slot, m_target, std::move(m_gates),
            std::move(fn), m_expected, m_expect_set);
        return m_owner;
    }

    Owner&        m_owner;
    void**        m_slot = nullptr;
    void*         m_target = nullptr;
    int           m_slot_index = -1;
    detail::Gates m_gates;
    void*         m_expected = nullptr;
    bool          m_expect_set = false;
    std::shared_ptr<std::atomic<std::uint64_t>> m_invocations;
};

// ─── the group, with the front door bolted on by the derived class ───────
//
// Everything a group does regardless of how its sites were named: the pending
// list, the group-wide gate, the two install paths, the undo, and the refusal
// bookkeeping. The two front doors differ only in how a site gets named, so
// that is all they contain.
//
// Curiously recurring, so that `.non_reentrant()` returns the derived type and
// the chain can carry on with another site. Gates compose in any order
// everywhere else in splice, and a base class that quietly ended the chain
// would be the one place they did not.
template <typename Derived, typename Policy>
class GroupBuilder {
public:
    using policy_type = Policy;

    GroupBuilder(const GroupBuilder&) = delete;
    GroupBuilder& operator=(const GroupBuilder&) = delete;

    // One re-entrancy scope for the whole group, which is what the problem
    // actually is: the re-entry arrives through a DIFFERENT method on the same
    // object. Present1's implementation calls Present, both are hooked, and
    // without this the overlay draws twice or recurses.
    //
    // Allowed after the sites are named, like every other gate: it is applied
    // when the group installs, not where it is spelled.
    Derived& non_reentrant() {
        m_group_scope = std::make_shared<Reentrancy>();
        m_group_reentry_requested = true;
        return self();
    }
    Derived& non_reentrant(const Reentrancy& scope) {
        m_group_scope.reset();
        m_group_bit = scope.bit();
        m_group_reentry_requested = true;
        return self();
    }

    // ─── install: the slot_swap tier ──────────────────────────────────────
    //
    // One pointer-sized atomic store per site. No thread is suspended and no
    // code is written, so the only things that can go wrong are a slot that is
    // not what the caller said it would be, and a page that will not take the
    // write.
    Outcome install(slot_swap_t) {
        std::uint64_t group_bit = 0;
        if (!ready(group_bit)) return refusal();

        auto group = std::make_shared<detail::GroupState>();
        group->sites.reserve(m_sites.size());

        for (auto& pending : m_sites) {
            if (pending.slot == nullptr) {
                SPLICE_LOGE("splice: the slot_swap tier needs a vtable slot, and "
                            "site %zu was named by address -- use splice::live",
                            pending.site.ordinal);
                return undo(group, pending.site, Operation::write_slot);
            }
            void* original = nullptr;
            if (!read_slot(pending, original)) {
                return undo(group, pending.site, Operation::read_slot);
            }

            // Publish first, write second. A reader that sees the new pointer
            // must find the original and the callback already there; the other
            // order is a live call through a null original, which this project
            // has paid for once already (FR-011 Phase 4.5d).
            pending.publish(original, group_bit);

            splice_patch_record record{};
            if (splice_vtable_swap(pending.slot, pending.stub, nullptr, &record) != 0) {
                return undo(group, pending.site, Operation::write_slot);
            }
            group->sites.push_back(detail::InstalledSite{
                static_cast<void*>(pending.slot), record, pending.release_stub});
        }
        return installed(group);
    }

    // ─── install: the coordinated tier ────────────────────────────────────
    //
    // Inline-patches the function behind each site, under one suspension of
    // every other thread. Three calls into the engine, and their order is the
    // contract: prepare allocates and opens everything, the publication
    // happens here where taking a lock is still safe, and commit suspends,
    // writes and resumes without allocating anything at all.
    Outcome install(live_t) {
        std::uint64_t group_bit = 0;
        if (!ready(group_bit)) return refusal();

        std::vector<splice_live_site> sites(m_sites.size());
        for (std::size_t i = 0; i < m_sites.size(); ++i) {
            auto& pending = m_sites[i];
            void* target = pending.target;
            if (pending.slot != nullptr && !read_slot(pending, target)) {
                fail(Stage::prepare, Operation::read_slot, 0, pending.site);
                return refusal();
            }
            if (target == nullptr) {
                fail(Stage::prepare, Operation::read_slot, 0, pending.site);
                return refusal();
            }
            pending.target = target;
            sites[i].target = target;
            sites[i].replacement = pending.stub;
            sites[i].expected = nullptr;
        }

        std::vector<void*> originals(m_sites.size(), nullptr);
        splice_live_outcome engine_outcome{};
        void* handle = splice_live_prepare(sites.data(),
                                           static_cast<unsigned int>(sites.size()),
                                           originals.data(), &engine_outcome);
        if (handle == nullptr) {
            record_engine_failure(engine_outcome);
            return refusal();
        }

        // Published while nothing is suspended, on purpose: this takes the
        // registry's lock, and a lock taken while another thread is suspended
        // is how a process stops.
        for (std::size_t i = 0; i < m_sites.size(); ++i) {
            m_sites[i].publish(originals[i], group_bit);
        }

        std::vector<splice_patch_record> records(m_sites.size());
        splice_live_commit(handle, records.data(), &engine_outcome);
        splice_live_release(handle);

        if (engine_outcome.state != SPLICE_LIVE_INSTALLED) {
            record_engine_failure(engine_outcome);
            if (engine_outcome.state == SPLICE_LIVE_LOST) {
                // The stubs stay claimed: a thread may still be inside one,
                // and on this path the threads may still be suspended.
                m_sites.clear();
                return Outcome(State::lost, m_failure);
            }
            release_pending();
            return Outcome(engine_outcome.state == SPLICE_LIVE_RECOVERED
                               ? State::recovered : State::refused,
                           m_failure);
        }

        auto group = std::make_shared<detail::GroupState>();
        group->sites.reserve(m_sites.size());
        for (std::size_t i = 0; i < m_sites.size(); ++i) {
            group->sites.push_back(detail::InstalledSite{
                m_sites[i].target, records[i], m_sites[i].release_stub});
        }
        return installed(group);
    }

    // Used by GroupSite. Public only because the site is a separate class;
    // nothing else should call it.
    template <typename FuncType, typename Fn>
    void add_pending(int slot_index, void** slot, void* target,
                     detail::Gates gates, Fn fn, void* expected, bool expect_set) {
        using Binder = detail::SiteBinder<FuncType, Policy>;
        const Site site{m_sites.size(), slot_index};

        if (m_failed) return;
        if (slot == nullptr && target == nullptr) {
            SPLICE_LOGE("splice: site %zu names neither a resolvable vtable slot "
                        "nor an address; name the index with "
                        ".on_slot(index, pmf, callback) when a pointer-to-member "
                        "cannot be resolved", site.ordinal);
            fail(Stage::prepare, Operation::resolve_slot, 0, site);
            return;
        }
        if (gates.unsatisfiable()) {
            SPLICE_LOGE("splice: no re-entrancy scope available for site %zu "
                        "(64 in use)", site.ordinal);
            fail(Stage::prepare, Operation::none, 0, site);
            return;
        }

        void* stub = detail::StubPool<FuncType, Policy>::claim();
        if (stub == nullptr) {
            SPLICE_LOGE("splice: no stub left for this signature (%d in use); "
                        "raise SPLICE_SLOT_STUBS_PER_SIGNATURE",
                        static_cast<int>(detail::StubPool<FuncType, Policy>::kCount));
            fail(Stage::prepare, Operation::none, 0, site);
            return;
        }

        detail::PendingSite pending;
        pending.site = site;
        pending.slot = slot;
        pending.target = target;
        pending.stub = stub;
        pending.registry_slot = default_context().slot_for(stub);
        pending.expected = expected;
        pending.expect_set = expect_set;
        pending.release_stub = [stub]() {
            detail::StubPool<FuncType, Policy>::release(stub);
        };
        pending.publish = [slot_id = pending.registry_slot, gates = std::move(gates),
                           fn = std::move(fn)](void* original,
                                               std::uint64_t group_bit) mutable {
            detail::Gates site_gates = gates;
            // A scope named on the site itself wins; otherwise the group's
            // applies. Nothing merges two scopes: a hook is inside one scope
            // or inside none.
            if (!site_gates.reentry_requested && group_bit != 0) {
                site_gates.reentry_bit = group_bit;
                site_gates.reentry_requested = true;
            }
            Binder::publish(slot_id, original, std::move(site_gates), fn);
        };
        m_sites.push_back(std::move(pending));
    }

protected:
    GroupBuilder() = default;

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

private:
    Derived& self() noexcept { return static_cast<Derived&>(*this); }

    // Everything both tiers check before they touch anything.
    bool ready(std::uint64_t& group_bit) {
        if (m_failed) return false;
        if (m_sites.empty()) {
            SPLICE_LOGE("splice: install was asked for, but no site was named");
            fail(Stage::prepare, Operation::none, 0);
            return false;
        }
        group_bit = m_group_scope ? m_group_scope->bit() : m_group_bit;
        if (m_group_reentry_requested && group_bit == 0) {
            SPLICE_LOGE("splice: non_reentrant() for the group could not reserve "
                        "a scope (64 in use); the group will not install");
            fail(Stage::prepare, Operation::none, 0);
            return false;
        }
        return true;
    }

    // Read what the site's slot holds, and check it against `.expect`.
    bool read_slot(const detail::PendingSite& pending, void*& value) {
        value = *pending.slot;
        if (value == nullptr) {
            SPLICE_LOGE("splice: slot %d is empty", pending.site.slot_index);
            return false;
        }
        if (pending.expect_set && value != pending.expected) {
            SPLICE_LOGE("splice: slot %d holds %p, not the expected %p",
                        pending.site.slot_index, value, pending.expected);
            return false;
        }
        return true;
    }

    Outcome installed(std::shared_ptr<detail::GroupState> group) {
        group->installed = true;
        Outcome outcome(State::installed, Failure{});
        outcome.m_group = std::move(group);
        SPLICE_LOGI("splice: %zu site(s) installed", m_sites.size());
        m_sites.clear();
        return outcome;
    }

    // One site failed: put every site already written back before anything
    // else happens, and report `recovered` -- the attempt is undone and
    // retrying is safe. This is the whole point of a group. Installing one
    // site at a time can leave five of seven hooks live with nothing holding a
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
        for (auto& written : group->sites) {
            if (splice_disable(&written.record) != 0) {
                all = false;
                SPLICE_LOGE("splice: could not restore %p after failing at site %zu",
                            written.where, site.ordinal);
                continue;
            }
            if (written.release_stub) written.release_stub();
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

    void record_engine_failure(const splice_live_outcome& outcome) {
        fail(stage_of(outcome), operation_of(outcome), outcome.os_error,
             site_of(outcome));
        m_failure.thread_id = outcome.thread_id;
    }

    static Stage stage_of(const splice_live_outcome& outcome) noexcept {
        switch (outcome.stage) {
            case SPLICE_LIVE_STAGE_ENLIST:  return Stage::enlist;
            case SPLICE_LIVE_STAGE_MAP:     return Stage::map;
            case SPLICE_LIVE_STAGE_MIGRATE: return Stage::migrate;
            case SPLICE_LIVE_STAGE_WRITE:   return Stage::write;
            case SPLICE_LIVE_STAGE_FLUSH:   return Stage::flush;
            case SPLICE_LIVE_STAGE_RESUME:  return Stage::resume;
            default:                        return Stage::prepare;
        }
    }

    static Operation operation_of(const splice_live_outcome& outcome) noexcept {
        switch (outcome.operation) {
            case SPLICE_LIVE_OP_ENUMERATE:     return Operation::enumerate_threads;
            case SPLICE_LIVE_OP_SUSPEND:       return Operation::suspend_thread;
            case SPLICE_LIVE_OP_CAPTURE_PC:    return Operation::capture_pc;
            case SPLICE_LIVE_OP_SET_PC:        return Operation::set_pc;
            case SPLICE_LIVE_OP_RESTORE_SITE:  return Operation::restore_slot;
            case SPLICE_LIVE_OP_RESUME_THREAD: return Operation::resume_thread;
            case SPLICE_LIVE_OP_WRITE_SITE:
            case SPLICE_LIVE_OP_FLUSH_SITE:    return Operation::write_slot;
            case SPLICE_LIVE_OP_PREPARE_SITE:  return Operation::resolve_slot;
            default:                           return Operation::none;
        }
    }

    Site site_of(const splice_live_outcome& outcome) const noexcept {
        const std::size_t ordinal = outcome.site;
        if (ordinal < m_sites.size()) return m_sites[ordinal].site;
        return Site{ordinal, -1};
    }

    std::vector<detail::PendingSite> m_sites;
    std::shared_ptr<Reentrancy>      m_group_scope;
    std::uint64_t                    m_group_bit = 0;
    bool                             m_group_reentry_requested = false;
    bool                             m_failed = false;

protected:
    Failure m_failure{};
};

// ─── a group named through an interface ───────────────────────────────────
template <typename T, typename Policy = SPLICE_DEFAULT_POLICY>
class Vtable : public GroupBuilder<Vtable<T, Policy>, Policy> {
public:
    // Reads the object's function table. The object must be polymorphic and
    // must keep its table pointer first, which is what a COM interface and a
    // single-inheritance C++ interface both guarantee.
    //
    // The object names the table; it does not scope the hook to itself. That
    // table belongs to the concrete class and is shared by every instance of
    // it. See the tier note at the top of this file.
    explicit Vtable(T* object) {
        static_assert(std::is_polymorphic_v<T>,
                      "splice::Vtable needs a class with virtual functions");
        if (object == nullptr) {
            SPLICE_LOGE("splice::Vtable: null object");
            this->fail(Stage::prepare, Operation::read_slot, 0);
            return;
        }
        std::memcpy(&m_table, static_cast<void*>(object), sizeof(m_table));
        if (m_table == nullptr) {
            SPLICE_LOGE("splice::Vtable: object at %p has no function table",
                        static_cast<void*>(object));
            this->fail(Stage::prepare, Operation::read_slot, 0);
        }
    }

    // .on(pmf) -- gates and action verb follow.
    template <typename PMF>
    GroupSite<Vtable, member_free_fn_t<PMF>> on(PMF pmf) {
        return site<member_free_fn_t<PMF>>(resolve(pmf));
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
    GroupSite<Vtable, member_free_fn_t<PMF>> on_slot(int slot_index, PMF) {
        return site<member_free_fn_t<PMF>>(slot_index);
    }
    template <typename PMF, typename Fn>
    Vtable& on_slot(int slot_index, PMF pmf, Fn fn) {
        return on_slot(slot_index, pmf).onInvoke(std::move(fn));
    }

private:
    template <typename FuncType>
    GroupSite<Vtable, FuncType> site(int slot_index) {
        void** slot = (m_table != nullptr && slot_index >= 0)
                          ? m_table + slot_index
                          : nullptr;
        return GroupSite<Vtable, FuncType>(*this, slot, nullptr, slot_index);
    }

    template <typename PMF>
    static int resolve(PMF pmf) {
        unsigned char raw[sizeof(PMF)];
        std::memcpy(raw, &pmf, sizeof(PMF));
        return splice_vtable_slot_of_pmf(raw, static_cast<unsigned int>(sizeof(PMF)));
    }

    void** m_table = nullptr;
};

// ─── a group named by address ─────────────────────────────────────────────
//
// The general noun; `Vtable` is one way to fill a group and this is the other,
// for a target that is not a method of an interface. The signature comes from
// a value of the function-pointer type, the same way SPLICE_HOOK_ADDR takes it
// as a macro argument: a default-constructed `Present_t{}` is a type carried
// in an argument, and it is never called.
template <typename Policy = SPLICE_DEFAULT_POLICY>
class Live : public GroupBuilder<Live<Policy>, Policy> {
public:
    Live() = default;

    template <typename FuncType>
    GroupSite<Live, FuncType> at(void* target, FuncType) {
        return GroupSite<Live, FuncType>(*this, nullptr, target, -1);
    }
    template <typename FuncType, typename Fn>
    Live& at(void* target, FuncType tag, Fn fn) {
        return at(target, tag).onInvoke(std::move(fn));
    }
};

} // namespace splice
