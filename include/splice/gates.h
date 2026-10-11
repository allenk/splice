// ─── Splice gates — what .when() / .once() / .non_reentrant() mean ─────────
//
// The fluent modifiers accumulate here and are consumed at the action verb,
// which is what makes `.when(p).once()` and `.once().when(p)` the same hook.
//
// This lives in its own header because there is now more than one builder.
// A single hook site is built by InterceptorEntry; a group of vtable slots is
// built by splice::Vtable. Both offer the same gates, and the gates have to
// mean exactly the same thing in both -- in particular the ORDER in which they
// are consulted, which is not an implementation detail:
//
//   1. .observe()        counts arrivals, before any gate can turn one away
//   2. .non_reentrant()  a nested call is not a new call to decide about
//   3. .when(pred)       ask
//   4. .once()/.times(n) spend
//
// Putting the budget ahead of the re-entrancy gate would let one logical call
// spend several of a .times(n) allowance, and putting .when() ahead of it
// would ask the predicate about a call that is not going to the callback at
// all. One definition, so there is one answer.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <splice/reentrancy.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace splice::detail {

// The gates a builder has accumulated but not yet committed. Cleared by the
// builder every time an action verb commits, so the next verb starts clean.
struct Gates {
    std::function<bool()>                       when_pred;      // .when(pred)
    std::shared_ptr<std::atomic<long long>>     remaining;      // .once() / .times(n)
    std::shared_ptr<std::atomic<std::uint64_t>> invocations;    // .observe()
    std::uint64_t                               reentry_bit = 0;        // .non_reentrant()
    std::shared_ptr<Reentrancy>                 owned_scope;            // its no-argument form
    bool                                        reentry_requested = false;

    // Nothing to wrap: the callback can be stored exactly as written.
    [[nodiscard]] bool transparent() const noexcept {
        return !when_pred && !remaining && !invocations && !reentry_requested;
    }

    // A scope was asked for and none was available. The caller must refuse to
    // install rather than install without it: installing without the
    // protection that was asked for IS the double run, or the recursion, the
    // caller was preventing.
    [[nodiscard]] bool unsatisfiable() const noexcept {
        return reentry_requested && reentry_bit == 0;
    }
};

// Wraps a callback in the gates above. Parameterised on the hook's signature
// because the wrapper has to call through it; `FuncType` is the original
// function's pointer type, which the callback receives as its first argument.
template <typename Ret, typename FuncType, typename... Args>
struct GateWrap {
    using Invoke = std::function<Ret(FuncType, Args...)>;

    template <typename Fn>
    static Ret run(const Gates& gates, const Fn& fn, FuncType orig, Args... args) {
        if (gates.when_pred && !gates.when_pred()) {
            return orig(args...);
        }
        if (gates.remaining) {
            // fetch_sub returns the value BEFORE the decrement. Fire only
            // while the budget is positive. The counter keeps ticking down
            // past zero, which is harmless -- it is bounded by the call count.
            if (gates.remaining->fetch_sub(1, std::memory_order_acq_rel) <= 0) {
                return orig(args...);
            }
        }
        return fn(orig, args...);
    }

    template <typename Fn>
    static Invoke apply(Gates gates, Fn fn) {
        return Invoke([gates = std::move(gates), fn = std::move(fn)]
                      (FuncType orig, Args... args) -> Ret {
            // Before the gates: a call turned away by .when() still arrived,
            // and "nothing arrived" is the finding this counter is for.
            if (gates.invocations) {
                gates.invocations->fetch_add(1, std::memory_order_relaxed);
            }
            if (gates.reentry_bit != 0) {
                if (reentered(gates.reentry_bit)) {
                    return orig(args...);
                }
                ReentryHold hold(gates.reentry_bit);
                return run(gates, fn, orig, args...);
            }
            return run(gates, fn, orig, args...);
        });
    }
};

} // namespace splice::detail
