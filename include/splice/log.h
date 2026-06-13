/**
 * @file        splice/log.h
 * @brief       Splice logging — thin wrapper over platform_log.h with
 *              hot-path extensions (rate-limit, once, hex dump, runtime level)
 *
 * Design rationale:
 *   - Hooks run on the hottest path imaginable. A C++ logger (spdlog, fmt)
 *     that allocates or formats eagerly would dominate the hook cost.
 *   - We use Allen's platform_log.h as the C-based substrate — it is
 *     macro-only, allocation-free, and compiles to direct __android_log_print
 *     / fprintf calls with zero type erasure.
 *   - This header adds the extras every hooking library needs:
 *       SPLICE_LOG_*_ONCE         — first-call only
 *       SPLICE_LOG_*_EVERY_N(n)   — throttle by count
 *       SPLICE_LOG_*_FIRST_N(n)   — emit the first N calls, then silent
 *       SPLICE_LOG_HEX(ptr, len)  — hex dump for patch/trampoline inspection
 *       SPLICE_LOG_HERE()         — drop a "I was here" breadcrumb
 *       splice::log::set_level()  — runtime level switch (atomic, optional)
 *
 * Usage:
 *   // Project-level tag override (optional — defaults to "splice"):
 *   #define SPLICE_LOG_TAG "MyHook"
 *   #include <splice/log.h>
 *
 *   SPLICE_LOGI("hook installed: %s", name);
 *   SPLICE_LOGD_EVERY_N(1000, "frame %llu", frame);   // every 1000th frame
 *   SPLICE_LOGV_ONCE("first call from thread %lu", tid);
 *   SPLICE_LOG_HEX(patched_bytes, 16);
 */

#pragma once

// ─── Tag ────────────────────────────────────────────────────────────────────
// Override at include site via: #define SPLICE_LOG_TAG "MyTag"
#ifndef SPLICE_LOG_TAG
#define SPLICE_LOG_TAG "splice"
#endif

// Feed the tag through to platform_log.h before including it.
#ifdef PLOG_TAG
#undef PLOG_TAG
#endif
#define PLOG_TAG SPLICE_LOG_TAG

// platform_log.h's Windows high-precision timestamp path uses uint32_t /
// uint64_t without including <stdint.h> — harmless on Clang/GCC/NDK where
// other system headers pull it in transitively, but MSVC leaves those names
// undeclared. Hoist the include here instead of patching the vendored copy.
// TODO(upstream): push this into platform_log.h v1.4 canonical copy.
#include <stdint.h>

#include "detail/platform_log.h"

// ─── Level aliases ──────────────────────────────────────────────────────────
// 1:1 map to PLOG_*. Zero overhead — pure macro aliases.
#define SPLICE_LOGV(...) PLOG_V(__VA_ARGS__)
#define SPLICE_LOGD(...) PLOG_D(__VA_ARGS__)
#define SPLICE_LOGI(...) PLOG_I(__VA_ARGS__)
#define SPLICE_LOGW(...) PLOG_W(__VA_ARGS__)
#define SPLICE_LOGE(...) PLOG_E(__VA_ARGS__)

#define SPLICE_LOGV_IF(cond, ...) PLOG_V_IF(cond, __VA_ARGS__)
#define SPLICE_LOGD_IF(cond, ...) PLOG_D_IF(cond, __VA_ARGS__)
#define SPLICE_LOGI_IF(cond, ...) PLOG_I_IF(cond, __VA_ARGS__)
#define SPLICE_LOGW_IF(cond, ...) PLOG_W_IF(cond, __VA_ARGS__)
#define SPLICE_LOGE_IF(cond, ...) PLOG_E_IF(cond, __VA_ARGS__)

// ─── Hot-path extensions (C-friendly, header-only, macro-based) ─────────────
//
// These are critical for hooks on functions called every frame (eglSwapBuffers,
// glDrawArrays, malloc, etc). A single unconditional log call there will flood
// logcat and dominate the hook cost.
//
// Implementation notes:
//   - Uses a static local counter per call site (safe under C11/C++11
//     memory model for relaxed atomic increment; exact precision not required).
//   - On platforms without <stdatomic.h> (pre-C11 MSVC), falls back to a
//     plain static counter — slightly inaccurate under heavy contention,
//     but never crashes and the log rate is still bounded.
//   - The *_FIRST_N variant stops incrementing once the threshold is hit
//     to avoid integer overflow on eternally-running hooks.

#if defined(__cplusplus)
#include <atomic>
#define SPLICE_LOG_ATOMIC_COUNTER_T     ::std::atomic<unsigned long long>
#define SPLICE_LOG_ATOMIC_INC(c)        ((c).fetch_add(1, ::std::memory_order_relaxed))
#define SPLICE_LOG_ATOMIC_LOAD(c)       ((c).load(::std::memory_order_relaxed))
#elif (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L) && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
#define SPLICE_LOG_ATOMIC_COUNTER_T     _Atomic unsigned long long
#define SPLICE_LOG_ATOMIC_INC(c)        atomic_fetch_add_explicit(&(c), 1, memory_order_relaxed)
#define SPLICE_LOG_ATOMIC_LOAD(c)       atomic_load_explicit(&(c), memory_order_relaxed)
#else
// Fallback: plain static. Not race-free, but the failure mode is
// "log a few extra times under contention" — acceptable.
#define SPLICE_LOG_ATOMIC_COUNTER_T     unsigned long long
#define SPLICE_LOG_ATOMIC_INC(c)        ((c)++)
#define SPLICE_LOG_ATOMIC_LOAD(c)       (c)
#endif

// One-shot: fires on the first invocation only.
#define SPLICE_LOG_ONCE_IMPL(LEVEL_MACRO, ...) \
    do { \
        static SPLICE_LOG_ATOMIC_COUNTER_T _splice_once_##__LINE__ = 0; \
        if (SPLICE_LOG_ATOMIC_INC(_splice_once_##__LINE__) == 0) { \
            LEVEL_MACRO(__VA_ARGS__); \
        } \
    } while (0)

#define SPLICE_LOGV_ONCE(...) SPLICE_LOG_ONCE_IMPL(PLOG_V, __VA_ARGS__)
#define SPLICE_LOGD_ONCE(...) SPLICE_LOG_ONCE_IMPL(PLOG_D, __VA_ARGS__)
#define SPLICE_LOGI_ONCE(...) SPLICE_LOG_ONCE_IMPL(PLOG_I, __VA_ARGS__)
#define SPLICE_LOGW_ONCE(...) SPLICE_LOG_ONCE_IMPL(PLOG_W, __VA_ARGS__)
#define SPLICE_LOGE_ONCE(...) SPLICE_LOG_ONCE_IMPL(PLOG_E, __VA_ARGS__)

// Throttled: fires every Nth invocation (1st, (N+1)th, (2N+1)th, ...).
#define SPLICE_LOG_EVERY_N_IMPL(LEVEL_MACRO, N, ...) \
    do { \
        static SPLICE_LOG_ATOMIC_COUNTER_T _splice_ev_##__LINE__ = 0; \
        unsigned long long _n = SPLICE_LOG_ATOMIC_INC(_splice_ev_##__LINE__); \
        if ((_n % (unsigned long long)(N)) == 0) { \
            LEVEL_MACRO(__VA_ARGS__); \
        } \
    } while (0)

#define SPLICE_LOGV_EVERY_N(n, ...) SPLICE_LOG_EVERY_N_IMPL(PLOG_V, n, __VA_ARGS__)
#define SPLICE_LOGD_EVERY_N(n, ...) SPLICE_LOG_EVERY_N_IMPL(PLOG_D, n, __VA_ARGS__)
#define SPLICE_LOGI_EVERY_N(n, ...) SPLICE_LOG_EVERY_N_IMPL(PLOG_I, n, __VA_ARGS__)
#define SPLICE_LOGW_EVERY_N(n, ...) SPLICE_LOG_EVERY_N_IMPL(PLOG_W, n, __VA_ARGS__)
#define SPLICE_LOGE_EVERY_N(n, ...) SPLICE_LOG_EVERY_N_IMPL(PLOG_E, n, __VA_ARGS__)

// Burst-limited: fires only the first N invocations, then goes quiet.
#define SPLICE_LOG_FIRST_N_IMPL(LEVEL_MACRO, N, ...) \
    do { \
        static SPLICE_LOG_ATOMIC_COUNTER_T _splice_fn_##__LINE__ = 0; \
        if (SPLICE_LOG_ATOMIC_LOAD(_splice_fn_##__LINE__) < (unsigned long long)(N)) { \
            SPLICE_LOG_ATOMIC_INC(_splice_fn_##__LINE__); \
            LEVEL_MACRO(__VA_ARGS__); \
        } \
    } while (0)

#define SPLICE_LOGV_FIRST_N(n, ...) SPLICE_LOG_FIRST_N_IMPL(PLOG_V, n, __VA_ARGS__)
#define SPLICE_LOGD_FIRST_N(n, ...) SPLICE_LOG_FIRST_N_IMPL(PLOG_D, n, __VA_ARGS__)
#define SPLICE_LOGI_FIRST_N(n, ...) SPLICE_LOG_FIRST_N_IMPL(PLOG_I, n, __VA_ARGS__)
#define SPLICE_LOGW_FIRST_N(n, ...) SPLICE_LOG_FIRST_N_IMPL(PLOG_W, n, __VA_ARGS__)
#define SPLICE_LOGE_FIRST_N(n, ...) SPLICE_LOG_FIRST_N_IMPL(PLOG_E, n, __VA_ARGS__)

// Breadcrumb: "I was here" — logs file:line:function at DEBUG level.
#define SPLICE_LOG_HERE() SPLICE_LOGD("@ %s:%d (%s)", __FILE__, __LINE__, __func__)

// ─── Hex dump helper (C ABI, linked once) ───────────────────────────────────
//
// Emits a single log line per 16-byte row:
//   D splice  : 0x7fff12340000: 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28
//
// Safe to call on any pointer + length. Truncates output if len > 4096.

#ifdef __cplusplus
extern "C" {
#endif

void splice_log_hex_impl(const char* level_tag, const void* ptr, unsigned long len);

#ifdef __cplusplus
}
#endif

#define SPLICE_LOG_HEX(ptr, len)  splice_log_hex_impl("D", (ptr), (unsigned long)(len))
#define SPLICE_LOG_HEX_W(ptr, len) splice_log_hex_impl("W", (ptr), (unsigned long)(len))
#define SPLICE_LOG_HEX_E(ptr, len) splice_log_hex_impl("E", (ptr), (unsigned long)(len))

// ─── Runtime level switch (optional, C++ only) ──────────────────────────────
//
// Compile-time PLOG_LEVEL still gates whether the macros emit *any* code.
// At runtime, callers can further suppress output without rebuild:
//
//     splice::log::set_level(splice::log::Level::Warn);
//
// Implementation provided out-of-line (src/log.cpp); when the project is
// built with SPLICE_LOG_RUNTIME_LEVEL=OFF, these become no-ops.

#ifdef __cplusplus
namespace splice::log {

enum class Level : int {
    Verbose = PLOG_LEVEL_VERBOSE,
    Debug   = PLOG_LEVEL_DEBUG,
    Info    = PLOG_LEVEL_INFO,
    Warn    = PLOG_LEVEL_WARN,
    Error   = PLOG_LEVEL_ERROR,
    Silent  = PLOG_LEVEL_ERROR + 1,
};

// Thread-safe. Lock-free relaxed atomic internally.
void set_level(Level level) noexcept;
Level get_level() noexcept;

// Query: is the given level currently enabled at runtime?
// Used by the *_EVERY_N etc. wrappers to short-circuit before formatting.
bool is_enabled(Level level) noexcept;

} // namespace splice::log
#endif
