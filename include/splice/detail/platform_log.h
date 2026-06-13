/**
 * @file        platform_log.h
 * @author      Allen Kuo
 * @brief       Portable/Platform-agnostic logging utility (vendored copy)
 * @version     1.3
 * @date        2025-01-16
 * @copyright   Copyright (c) 2025 Allen Kuo. All rights reserved.
 * @license     MIT License
 *
 * ============================================================================
 *  Vendored into Splice as the C-based logging substrate. This is Allen's
 *  common log mechanism, reused across multiple projects. Do NOT modify
 *  this file inline — raise changes upstream in the canonical copy and
 *  re-vendor. Splice-specific extensions live in <splice/log.h>.
 * ============================================================================
 *
 * Platform detection order (Priority):
 *   1. PLOG_DISABLE          → No logging
 *   2. PLOG_FORCE_CONSOLE    → Force printf (for NDK console apps)
 *   3. PLOG_USE_SPDLOG       → spdlog
 *   4. Android + HWC2        → HWC_LOG*
 *   5. Android               → ALOG*
 *   6. Fallback              → printf/fprintf
 *
 * Usage for NDK Console Apps with Timestamp and PID/TID:
 *   #define PLOG_FORCE_CONSOLE     // Force console output
 *   #define PLOG_ENABLE_TIMESTAMP  // Enable timestamp prefix
 *   #define PLOG_TIMESTAMP_MS      // Use milliseconds (optional)
 *   #define PLOG_ENABLE_PID_TID    // Enable PID/TID display (requires PLOG_ENABLE_TIMESTAMP)
 *   #define PLOG_TAG "MyModule"
 *   #include "platform_log.h"
 *
 *   PLOG_I("Started");          // Output: 01-16 14:30:25.123 12345 12345 I MyModule  : Started
 *   PLOG_D("Value: %d", x);
 *   PLOG_E("Error: %s", msg);
 *
 * Timestamp Options:
 *   - PLOG_ENABLE_TIMESTAMP      Enable timestamp (default: seconds)
 *   - PLOG_TIMESTAMP_MS          Milliseconds precision (MM-DD HH:MM:SS.mmm)
 *   - PLOG_TIMESTAMP_US          Microseconds precision (MM-DD HH:MM:SS.uuuuuu)
 *   - PLOG_ENABLE_PID_TID        Enable PID/TID after timestamp (logcat style)
 *
 * Standard Usage:
 *   #define PLOG_TAG "MyModule"
 *   #include "platform_log.h"
 *
 *   PLOG_I("Started");
 *   PLOG_D("Value: %d", x);
 *   PLOG_E("Error: %s", msg);
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

    // ============================================================================
    // Configuration
    // ============================================================================

#ifndef PLOG_TAG
#define PLOG_TAG "PLOG"
#endif

#ifndef PLOG_LEVEL
#if defined(DEBUG) || defined(_DEBUG)
#define PLOG_LEVEL 0  // VERBOSE
#else
#define PLOG_LEVEL 2  // INFO
#endif
#endif

#define PLOG_LEVEL_VERBOSE  0
#define PLOG_LEVEL_DEBUG    1
#define PLOG_LEVEL_INFO     2
#define PLOG_LEVEL_WARN     3
#define PLOG_LEVEL_ERROR    4

// Timestamp configuration
// #define PLOG_ENABLE_TIMESTAMP        // Enable timestamp prefix
// #define PLOG_TIMESTAMP_MS            // Use milliseconds (default: seconds)
// #define PLOG_TIMESTAMP_US            // Use microseconds (higher precision)
// #define PLOG_ENABLE_PID_TID          // Enable PID/TID (requires PLOG_ENABLE_TIMESTAMP)

// ============================================================================
// Backend Selection (Priority Order)
// ============================================================================

#if defined(PLOG_DISABLE)
    // Disabled: no-op (highest priority)
#define PLOG_BACKEND_NONE
#elif defined(PLOG_FORCE_CONSOLE) || defined(PLOG_FORCE_PRINTF)
    // Force console output (for NDK console apps - priority over Android log)
#define PLOG_BACKEND_PRINTF
#elif defined(PLOG_USE_SPDLOG)
    // Explicit spdlog
#define PLOG_BACKEND_SPDLOG
#elif defined(__ANDROID__)
    // Android platform
#if defined(USE_HWC2) || defined(PLOG_ANDROID_HWC2)
#define PLOG_BACKEND_HWC2
#else
#define PLOG_BACKEND_ANDROID
#endif
#else
    // Fallback
#define PLOG_BACKEND_PRINTF
#endif

// ============================================================================
// Backend Implementation
// ============================================================================

// ────────────────────────────────────────────────────────────────────────────
// Backend: None (Disabled)
// ────────────────────────────────────────────────────────────────────────────
#if defined(PLOG_BACKEND_NONE)
#define PLOG_V(...)
#define PLOG_D(...)
#define PLOG_I(...)
#define PLOG_W(...)
#define PLOG_E(...)

// ────────────────────────────────────────────────────────────────────────────
// Backend: spdlog
// ────────────────────────────────────────────────────────────────────────────
#elif defined(PLOG_BACKEND_SPDLOG)
#ifndef __cplusplus
#error "spdlog requires C++"
#endif
#include <spdlog/spdlog.h>

#if PLOG_LEVEL <= PLOG_LEVEL_VERBOSE
#define PLOG_V(...) spdlog::trace("[" PLOG_TAG "] " __VA_ARGS__)
#else
#define PLOG_V(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_DEBUG
#define PLOG_D(...) spdlog::debug("[" PLOG_TAG "] " __VA_ARGS__)
#else
#define PLOG_D(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_INFO
#define PLOG_I(...) spdlog::info("[" PLOG_TAG "] " __VA_ARGS__)
#else
#define PLOG_I(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_WARN
#define PLOG_W(...) spdlog::warn("[" PLOG_TAG "] " __VA_ARGS__)
#else
#define PLOG_W(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_ERROR
#define PLOG_E(...) spdlog::error("[" PLOG_TAG "] " __VA_ARGS__)
#else
#define PLOG_E(...)
#endif

// ────────────────────────────────────────────────────────────────────────────
// Backend: HWC2 (Android Hardware Composer)
// ────────────────────────────────────────────────────────────────────────────
#elif defined(PLOG_BACKEND_HWC2)
    // Map to HWC2 logging macros
#undef DEBUG_LOG_TAG
#define DEBUG_LOG_TAG PLOG_TAG
#include "utils/debug.h"

#if PLOG_LEVEL <= PLOG_LEVEL_VERBOSE
#define PLOG_V(...) HWC_LOGV(__VA_ARGS__)
#else
#define PLOG_V(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_DEBUG
#define PLOG_D(...) HWC_LOGD(__VA_ARGS__)
#else
#define PLOG_D(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_INFO
#define PLOG_I(...) HWC_LOGI(__VA_ARGS__)
#else
#define PLOG_I(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_WARN
#define PLOG_W(...) HWC_LOGW(__VA_ARGS__)
#else
#define PLOG_W(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_ERROR
#define PLOG_E(...) HWC_LOGE(__VA_ARGS__)
#else
#define PLOG_E(...)
#endif

// ────────────────────────────────────────────────────────────────────────────
// Backend: Android (Normal App)
// ────────────────────────────────────────────────────────────────────────────
#elif defined(PLOG_BACKEND_ANDROID)
#undef LOG_TAG
#define LOG_TAG PLOG_TAG
#include <android/log.h>

// Note: Android log already includes timestamps via logcat
// PLOG_ENABLE_TIMESTAMP has no effect for Android backend

#if PLOG_LEVEL <= PLOG_LEVEL_VERBOSE
#define PLOG_V(...) __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#else
#define PLOG_V(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_DEBUG
#define PLOG_D(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#else
#define PLOG_D(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_INFO
#define PLOG_I(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#else
#define PLOG_I(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_WARN
#define PLOG_W(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#else
#define PLOG_W(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_ERROR
#define PLOG_E(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define PLOG_E(...)
#endif

// ────────────────────────────────────────────────────────────────────────────
// Backend: printf (Fallback & Force Console)
// ────────────────────────────────────────────────────────────────────────────
#elif defined(PLOG_BACKEND_PRINTF)
#include <stdio.h>

// Timestamp support for printf backend
#if defined(PLOG_ENABLE_TIMESTAMP)
#include <time.h>

#ifdef _WIN32
#include <windows.h>

// Get current thread ID on Windows
static inline unsigned long plog_get_tid(void) {
    return (unsigned long)GetCurrentThreadId();
}

// Get current process ID on Windows
static inline unsigned long plog_get_pid(void) {
    return (unsigned long)GetCurrentProcessId();
}

static inline void plog_print_timestamp(FILE* stream) {
    char tmbuf[64];

#if defined(_MSC_VER) && _MSC_VER >= 1800  // Visual Studio 2013+ (Windows 8+)
    // Use high-precision API if available
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);

    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    const uint64_t EPOCH_DIFF = 11644473600000000ULL;  // microseconds
    uint64_t us_since_epoch = (uli.QuadPart / 10) - EPOCH_DIFF;

    time_t sec = us_since_epoch / 1000000;
    uint32_t us = us_since_epoch % 1000000;

    struct tm nowtm;
    localtime_s(&nowtm, &sec);
    strftime(tmbuf, sizeof(tmbuf), "%m-%d %H:%M:%S", &nowtm);

#if defined(PLOG_TIMESTAMP_US)
    fprintf(stream, "%s.%06u ", tmbuf, us);
#elif defined(PLOG_TIMESTAMP_MS)
    fprintf(stream, "%s.%03u ", tmbuf, us / 1000);
#else
    fprintf(stream, "%s ", tmbuf);
#endif

#else
    // Fallback for Windows 7 and older
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(tmbuf, sizeof(tmbuf), "%02d-%02d %02d:%02d:%02d",
        st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

#if defined(PLOG_TIMESTAMP_US)
    fprintf(stream, "%s.%06d ", tmbuf, st.wMilliseconds * 1000);  // Approximate
#elif defined(PLOG_TIMESTAMP_MS)
    fprintf(stream, "%s.%03d ", tmbuf, st.wMilliseconds);
#else
    fprintf(stream, "%s ", tmbuf);
#endif

#endif  // Win8+ vs Win7 fallback
}

#else  // POSIX

#include <sys/time.h>
#include <unistd.h>

// Get current thread ID on POSIX
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/syscall.h>
static inline unsigned long plog_get_tid(void) {
    return (unsigned long)syscall(SYS_gettid);
}
#elif defined(__APPLE__)
#include <pthread.h>
static inline unsigned long plog_get_tid(void) {
    uint64_t tid;
    pthread_threadid_np(NULL, &tid);
    return (unsigned long)tid;
}
#else
// Fallback: use pthread_self (not a real TID but unique per thread)
#include <pthread.h>
static inline unsigned long plog_get_tid(void) {
    return (unsigned long)pthread_self();
}
#endif

// Get current process ID on POSIX
static inline unsigned long plog_get_pid(void) {
    return (unsigned long)getpid();
}

static inline void plog_print_timestamp(FILE* stream) {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    time_t nowtime = tv.tv_sec;
    struct tm* nowtm = localtime(&nowtime);

    char tmbuf[64];
    strftime(tmbuf, sizeof(tmbuf), "%m-%d %H:%M:%S", nowtm);

#if defined(PLOG_TIMESTAMP_US)
    fprintf(stream, "%s.%06ld ", tmbuf, (long)tv.tv_usec);
#elif defined(PLOG_TIMESTAMP_MS)
    fprintf(stream, "%s.%03ld ", tmbuf, (long)(tv.tv_usec / 1000));
#else
    fprintf(stream, "%s ", tmbuf);
#endif
}

#endif  // _WIN32, POSIX

// Print PID/TID if enabled
#if defined(PLOG_ENABLE_PID_TID)
#define PLOG_PID_TID_PREFIX(stream) fprintf(stream, "%5lu %5lu ", plog_get_pid(), plog_get_tid())
#else
#define PLOG_PID_TID_PREFIX(stream)
#endif

#define PLOG_TIMESTAMP_PREFIX(stream) do { plog_print_timestamp(stream); PLOG_PID_TID_PREFIX(stream); } while(0)

#else  // !PLOG_ENABLE_TIMESTAMP
#define PLOG_TIMESTAMP_PREFIX(stream)
#endif  // PLOG_ENABLE_TIMESTAMP

#if defined(PLOG_ENABLE_TIMESTAMP) && defined(PLOG_ENABLE_PID_TID)
#define PLOG_FORMAT_LOGCAT 1
#else
#define PLOG_FORMAT_LOGCAT 0
#endif

#if PLOG_FORMAT_LOGCAT

#if PLOG_LEVEL <= PLOG_LEVEL_VERBOSE
#define PLOG_V(...) do { PLOG_TIMESTAMP_PREFIX(stdout); printf("V %-8s: ", PLOG_TAG); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while(0)
#else
#define PLOG_V(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_DEBUG
#define PLOG_D(...) do { PLOG_TIMESTAMP_PREFIX(stdout); printf("D %-8s: ", PLOG_TAG); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while(0)
#else
#define PLOG_D(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_INFO
#define PLOG_I(...) do { PLOG_TIMESTAMP_PREFIX(stdout); printf("I %-8s: ", PLOG_TAG); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while(0)
#else
#define PLOG_I(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_WARN
#define PLOG_W(...) do { PLOG_TIMESTAMP_PREFIX(stderr); fprintf(stderr, "W %-8s: ", PLOG_TAG); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while(0)
#else
#define PLOG_W(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_ERROR
#define PLOG_E(...) do { PLOG_TIMESTAMP_PREFIX(stderr); fprintf(stderr, "E %-8s: ", PLOG_TAG); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while(0)
#else
#define PLOG_E(...)
#endif

#else  // Standard format (no logcat style)

#if PLOG_LEVEL <= PLOG_LEVEL_VERBOSE
#define PLOG_V(...) do { PLOG_TIMESTAMP_PREFIX(stdout); printf("[V][" PLOG_TAG "] "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while(0)
#else
#define PLOG_V(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_DEBUG
#define PLOG_D(...) do { PLOG_TIMESTAMP_PREFIX(stdout); printf("[D][" PLOG_TAG "] "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while(0)
#else
#define PLOG_D(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_INFO
#define PLOG_I(...) do { PLOG_TIMESTAMP_PREFIX(stdout); printf("[I][" PLOG_TAG "] "); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while(0)
#else
#define PLOG_I(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_WARN
#define PLOG_W(...) do { PLOG_TIMESTAMP_PREFIX(stderr); fprintf(stderr, "[W][" PLOG_TAG "] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while(0)
#else
#define PLOG_W(...)
#endif

#if PLOG_LEVEL <= PLOG_LEVEL_ERROR
#define PLOG_E(...) do { PLOG_TIMESTAMP_PREFIX(stderr); fprintf(stderr, "[E][" PLOG_TAG "] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while(0)
#else
#define PLOG_E(...)
#endif

#endif  // PLOG_FORMAT_LOGCAT

#else
#error "Unknown PLOG backend"
#endif

// ============================================================================
// Conditional Logging
// ============================================================================

#define PLOG_V_IF(cond, ...) do { if (cond) PLOG_V(__VA_ARGS__); } while(0)
#define PLOG_D_IF(cond, ...) do { if (cond) PLOG_D(__VA_ARGS__); } while(0)
#define PLOG_I_IF(cond, ...) do { if (cond) PLOG_I(__VA_ARGS__); } while(0)
#define PLOG_W_IF(cond, ...) do { if (cond) PLOG_W(__VA_ARGS__); } while(0)
#define PLOG_E_IF(cond, ...) do { if (cond) PLOG_E(__VA_ARGS__); } while(0)

// ============================================================================
// Backend Info (for debugging)
// ============================================================================

#if defined(PLOG_BACKEND_NONE)
#define PLOG_BACKEND_NAME "None (Disabled)"
#elif defined(PLOG_BACKEND_SPDLOG)
#define PLOG_BACKEND_NAME "spdlog"
#elif defined(PLOG_BACKEND_HWC2)
#define PLOG_BACKEND_NAME "HWC2"
#elif defined(PLOG_BACKEND_ANDROID)
#define PLOG_BACKEND_NAME "Android Log"
#elif defined(PLOG_BACKEND_PRINTF)
#if defined(PLOG_FORCE_CONSOLE) || defined(PLOG_FORCE_PRINTF)
#define PLOG_BACKEND_NAME "Console (Forced)"
#else
#define PLOG_BACKEND_NAME "printf"
#endif
#else
#define PLOG_BACKEND_NAME "Unknown"
#endif

#ifdef __cplusplus
}
#endif
