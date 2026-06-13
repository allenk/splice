// ─── Splice diagnostic one-liner sugar ─────────────────────────────────────
//
// FR-009 Step 9.4 — three macros that turn common observability patterns
// into zero-ceremony hook setups:
//
//   SPLICE_TRACE(func)      — log on every call (just a name marker)
//   SPLICE_COUNT(func)      — atomic counter, periodic log dump
//   SPLICE_TIME(func)       — accumulated ns timing, periodic avg log
//
// Each has three variants for the three SPLICE_HOOK target forms:
//   SPLICE_TRACE / SPLICE_TRACE_LIB / SPLICE_TRACE_ADDR
//   SPLICE_COUNT / SPLICE_COUNT_LIB / SPLICE_COUNT_ADDR
//   SPLICE_TIME  / SPLICE_TIME_LIB  / SPLICE_TIME_ADDR
//
// All macros expand to a `SPLICE_HOOK_*_STATIC(...).before/.after/.onInvoke`
// chain — i.e. a STATEMENT, not a declaration. Use inside a function (e.g.
// setupDebugHooks). For file-scope auto-install, wrap in a static
// initialiser yourself.
//
// Periodic logging: SPLICE_COUNT / SPLICE_TIME log a snapshot every 65536
// calls (mask 0xFFFF). The threshold is intentionally coarse — these are
// debug diagnostics, not production telemetry. For programmatic access to
// the running stats, use `.before/.after` directly with your own atomics
// (the `splice::stats<func>()` template proposed in FR-009 §AC-8 is left
// for a future ticket).
//
// Args inspection: SPLICE_TRACE deliberately does NOT print individual
// args — generic formatting across arbitrary types is a separate problem
// (string vs pointer vs int vs ...). If you want richer trace output,
// write your own `.before([](T1 a1, T2 a2){...})` — it's 3 lines.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <splice/log.h>
#include <splice/macros.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace splice::detail::diag {

// Periodic-log heuristic: dump every (1 << kReportShift) calls. Coarse on
// purpose — keeps the hot path quiet.
inline constexpr std::uint64_t kReportMask = 0xFFFF;   // every 65536 hits

inline void report_count(const char* label,
                         std::atomic<std::uint64_t>& counter) noexcept {
    const auto n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & kReportMask) == 0) {
        SPLICE_LOGI("[COUNT] %s: %llu calls",
                    label, static_cast<unsigned long long>(n));
    }
}

inline void report_time(const char* label,
                        std::atomic<std::uint64_t>& total_ns,
                        std::atomic<std::uint64_t>& counter,
                        std::uint64_t dt_ns) noexcept {
    const auto t = total_ns.fetch_add(dt_ns, std::memory_order_relaxed) + dt_ns;
    const auto n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & kReportMask) == 0) {
        SPLICE_LOGI("[TIME] %s: avg=%llu ns (n=%llu, total=%llu ns)",
                    label,
                    static_cast<unsigned long long>(t / n),
                    static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(t));
    }
}

} // namespace splice::detail::diag


// ─── SPLICE_TRACE — log marker on every call ──────────────────────────────
// Uses .after(...) so it sees both void and non-void return types via a
// variadic capture. The lambda body intentionally ignores its args (see
// header doc for the rationale).
#define SPLICE_TRACE(funcname)                                                 \
    SPLICE_HOOK_STATIC(funcname).after([](auto&&...) {                         \
        SPLICE_LOGI("[TRACE] %s called", #funcname);                           \
    })

#define SPLICE_TRACE_LIB(libname, funcname)                                    \
    SPLICE_HOOK_STATIC(libname, funcname).after([](auto&&...) {                \
        SPLICE_LOGI("[TRACE] %s::%s called", libname, #funcname);              \
    })

#define SPLICE_TRACE_ADDR(funcptr)                                             \
    SPLICE_HOOK_ADDR_STATIC(funcptr).after([](auto&&...) {                     \
        SPLICE_LOGI("[TRACE] addr %p called",                                  \
                    reinterpret_cast<const void*>(funcptr));                   \
    })


// ─── SPLICE_COUNT — atomic per-call counter ───────────────────────────────
// Each macro use creates its own function-local static atomic (the lambda
// is uniquely typed per call site). Periodic dump via report_count.
#define SPLICE_COUNT(funcname)                                                 \
    SPLICE_HOOK_STATIC(funcname).before([](auto&&...) {                        \
        static std::atomic<std::uint64_t> s_count{0};                          \
        ::splice::detail::diag::report_count(#funcname, s_count);              \
    })

#define SPLICE_COUNT_LIB(libname, funcname)                                    \
    SPLICE_HOOK_STATIC(libname, funcname).before([](auto&&...) {               \
        static std::atomic<std::uint64_t> s_count{0};                          \
        ::splice::detail::diag::report_count(libname "::" #funcname, s_count); \
    })

#define SPLICE_COUNT_ADDR(funcptr)                                             \
    SPLICE_HOOK_ADDR_STATIC(funcptr).before([](auto&&...) {                    \
        static std::atomic<std::uint64_t> s_count{0};                          \
        ::splice::detail::diag::report_count(#funcptr, s_count);               \
    })


// ─── SPLICE_TIME — accumulated avg/total ns timing ────────────────────────
// Uses .onInvoke so we can bracket orig() with steady_clock samples. Branches
// on void/non-void return via `if constexpr`. Each call site has its own
// per-lambda statics for total + counter.
#define SPLICE_TIME(funcname)                                                  \
    SPLICE_HOOK_STATIC(funcname).onInvoke(                                     \
        [](auto orig, auto&&... args) {                                        \
            using steady = std::chrono::steady_clock;                         \
            static std::atomic<std::uint64_t> s_total{0};                      \
            static std::atomic<std::uint64_t> s_count{0};                      \
            const auto t0 = steady::now();                                    \
            if constexpr (std::is_void_v<decltype(                             \
                    orig(std::forward<decltype(args)>(args)...))>) {           \
                orig(std::forward<decltype(args)>(args)...);                   \
                const auto dt = std::chrono::duration_cast<                    \
                    std::chrono::nanoseconds>(steady::now() - t0).count();    \
                ::splice::detail::diag::report_time(                           \
                    #funcname, s_total, s_count,                               \
                    static_cast<std::uint64_t>(dt));                           \
            } else {                                                           \
                auto ret = orig(std::forward<decltype(args)>(args)...);        \
                const auto dt = std::chrono::duration_cast<                    \
                    std::chrono::nanoseconds>(steady::now() - t0).count();    \
                ::splice::detail::diag::report_time(                           \
                    #funcname, s_total, s_count,                               \
                    static_cast<std::uint64_t>(dt));                           \
                return ret;                                                    \
            }                                                                  \
        })

#define SPLICE_TIME_LIB(libname, funcname)                                     \
    SPLICE_HOOK_STATIC(libname, funcname).onInvoke(                            \
        [](auto orig, auto&&... args) {                                        \
            using steady = std::chrono::steady_clock;                         \
            static std::atomic<std::uint64_t> s_total{0};                      \
            static std::atomic<std::uint64_t> s_count{0};                      \
            const auto t0 = steady::now();                                    \
            if constexpr (std::is_void_v<decltype(                             \
                    orig(std::forward<decltype(args)>(args)...))>) {           \
                orig(std::forward<decltype(args)>(args)...);                   \
                const auto dt = std::chrono::duration_cast<                    \
                    std::chrono::nanoseconds>(steady::now() - t0).count();    \
                ::splice::detail::diag::report_time(                           \
                    libname "::" #funcname, s_total, s_count,                  \
                    static_cast<std::uint64_t>(dt));                           \
            } else {                                                           \
                auto ret = orig(std::forward<decltype(args)>(args)...);        \
                const auto dt = std::chrono::duration_cast<                    \
                    std::chrono::nanoseconds>(steady::now() - t0).count();    \
                ::splice::detail::diag::report_time(                           \
                    libname "::" #funcname, s_total, s_count,                  \
                    static_cast<std::uint64_t>(dt));                           \
                return ret;                                                    \
            }                                                                  \
        })

#define SPLICE_TIME_ADDR(funcptr)                                              \
    SPLICE_HOOK_ADDR_STATIC(funcptr).onInvoke(                                 \
        [](auto orig, auto&&... args) {                                        \
            using steady = std::chrono::steady_clock;                         \
            static std::atomic<std::uint64_t> s_total{0};                      \
            static std::atomic<std::uint64_t> s_count{0};                      \
            const auto t0 = steady::now();                                    \
            if constexpr (std::is_void_v<decltype(                             \
                    orig(std::forward<decltype(args)>(args)...))>) {           \
                orig(std::forward<decltype(args)>(args)...);                   \
                const auto dt = std::chrono::duration_cast<                    \
                    std::chrono::nanoseconds>(steady::now() - t0).count();    \
                ::splice::detail::diag::report_time(                           \
                    #funcptr, s_total, s_count,                                \
                    static_cast<std::uint64_t>(dt));                           \
            } else {                                                           \
                auto ret = orig(std::forward<decltype(args)>(args)...);        \
                const auto dt = std::chrono::duration_cast<                    \
                    std::chrono::nanoseconds>(steady::now() - t0).count();    \
                ::splice::detail::diag::report_time(                           \
                    #funcptr, s_total, s_count,                                \
                    static_cast<std::uint64_t>(dt));                           \
                return ret;                                                    \
            }                                                                  \
        })
