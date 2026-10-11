// ─── Splice re-entrancy scopes ─────────────────────────────────────────────
//
// `.non_reentrant()` is a gate like `.when()` and `.once()`: it decides whether
// the callback runs. It answers one question — is this thread already inside a
// callback of the same scope? — and when the answer is yes, the call goes
// straight to the original.
//
// The re-entry that matters usually arrives through a DIFFERENT hook. An
// interface method's implementation calls a sibling method, both are hooked,
// and without a gate the work is done twice, or the hooks recurse until the
// stack runs out. A scope is therefore a set of hooks, not one hook: every hook
// that shares a scope is suppressed while any of them is running on the thread.
//
//     splice::Reentrancy present_family;
//     SPLICE_HOOK_ADDR(present_addr,  Present_t ).non_reentrant(present_family).onInvoke(...);
//     SPLICE_HOOK_ADDR(present1_addr, Present1_t).non_reentrant(present_family).onInvoke(...);
//
// `.non_reentrant()` with no argument gives one hook a scope of its own, which
// covers direct recursion.
//
// Cost on the hot path: one thread-local load and a mask test on every call,
// and a store on the way in and out of the outermost one. That is what a
// hand-written thread_local flag costs, which is what this replaces.
//
// Limit, stated rather than discovered: a process can hold 64 scopes at once,
// one bit each in a single thread-local word. Reserving a 65th yields an
// invalid scope, and a hook asked to use it refuses to install rather than
// installing without the protection it was asked for.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <atomic>
#include <cstdint>

namespace splice {

namespace detail {

// One bit per live scope. Function-local so every translation unit of a module
// shares one word without a definition in src/.
inline std::atomic<std::uint64_t>& reentrancy_bits_in_use() noexcept {
    static std::atomic<std::uint64_t> bits{0};
    return bits;
}

// Returns the lowest free bit, or 0 when all 64 are taken.
inline std::uint64_t reserve_reentrancy_bit() noexcept {
    auto& in_use = reentrancy_bits_in_use();
    std::uint64_t seen = in_use.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint64_t free_bits = ~seen;
        if (free_bits == 0) return 0;
        const std::uint64_t bit = free_bits & (~free_bits + 1);
        if (in_use.compare_exchange_weak(seen, seen | bit,
                                         std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
            return bit;
        }
    }
}

inline void release_reentrancy_bit(std::uint64_t bit) noexcept {
    if (bit == 0) return;
    reentrancy_bits_in_use().fetch_and(~bit, std::memory_order_acq_rel);
}

// The scopes this thread is currently inside. Constant-initialised, so access
// needs no guard.
inline thread_local std::uint64_t t_reentrancy_active = 0;

[[nodiscard]] inline bool reentered(std::uint64_t bit) noexcept {
    return (t_reentrancy_active & bit) != 0;
}

// Marks the scope entered for the lifetime of one outermost callback. Clearing
// in the destructor keeps the scope honest if the callback unwinds.
class ReentryHold {
public:
    explicit ReentryHold(std::uint64_t bit) noexcept : m_bit(bit) {
        t_reentrancy_active |= m_bit;
    }

    ~ReentryHold() { t_reentrancy_active &= ~m_bit; }

    ReentryHold(const ReentryHold&) = delete;
    ReentryHold& operator=(const ReentryHold&) = delete;

private:
    std::uint64_t m_bit;
};

} // namespace detail

// A set of hooks that may not re-enter one another on the same thread.
//
// Must outlive every hook that uses it; a namespace-scope or function-local
// static is the usual home. Neither copyable nor movable, because the identity
// of the scope is the bit it holds.
class Reentrancy {
public:
    Reentrancy() noexcept : m_bit(detail::reserve_reentrancy_bit()) {}

    ~Reentrancy() { detail::release_reentrancy_bit(m_bit); }

    Reentrancy(const Reentrancy&) = delete;
    Reentrancy& operator=(const Reentrancy&) = delete;

    // False when the process already holds 64 scopes. A hook told to use an
    // invalid scope refuses to install.
    [[nodiscard]] bool valid() const noexcept { return m_bit != 0; }

    [[nodiscard]] std::uint64_t bit() const noexcept { return m_bit; }

private:
    std::uint64_t m_bit;
};

} // namespace splice
