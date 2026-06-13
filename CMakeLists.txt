# ─── Splice ──────────────────────────────────────────────────────────────────
# Cross-platform C++ function hooking library.
# Prerequisites: set VCPKG_ROOT env var; `vcpkg install` before cmake.
#   Windows example: set VCPKG_ROOT=C:\vcpkg ; vcpkg install
#
# Build:
#   cmake --preset=dev
#   cmake --build --preset=dev
#   ctest --preset=dev --output-on-failure
# ─────────────────────────────────────────────────────────────────────────────
cmake_minimum_required(VERSION 3.21)

# VCPKG toolchain must be wired before project() so language init sees it.
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

project(splice
    VERSION 1.0.0
    DESCRIPTION "Cross-platform C++ function hooking library"
    LANGUAGES CXX)

# ─── Standard ────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Out-of-source builds only.
if(CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
    message(FATAL_ERROR "In-source builds are forbidden. Use a build/ directory.")
endif()

# ─── Top-level vs consumed-as-dependency ─────────────────────────────────────
if(NOT DEFINED SPLICE_IS_TOP_LEVEL)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(SPLICE_IS_TOP_LEVEL ON)
    else()
        set(SPLICE_IS_TOP_LEVEL OFF)
    endif()
endif()

# ─── Options ─────────────────────────────────────────────────────────────────
option(SPLICE_BUILD_TESTS        "Build tests (GTest)"                       ${SPLICE_IS_TOP_LEVEL})
option(SPLICE_BUILD_EXAMPLES     "Build examples"                            ${SPLICE_IS_TOP_LEVEL})
option(SPLICE_BUILD_BENCHMARKS   "Build FR-010 microbenchmarks (Google Benchmark)" OFF)
option(SPLICE_ENABLE_ASAN        "Enable AddressSanitizer"                   OFF)
option(SPLICE_ENABLE_UBSAN       "Enable UndefinedBehaviorSanitizer"         OFF)
option(SPLICE_ENABLE_TSAN        "Enable ThreadSanitizer"                    OFF)
option(SPLICE_LOG_RUNTIME_LEVEL  "Compile in runtime log level switch"       ON)
option(SPLICE_WARNINGS_AS_ERRORS "Promote warnings to errors"                OFF)

# ─── Architecture detection ──────────────────────────────────────────────────
# ARM64 and x86_64 are the Phase 0 targets; ARM32/x86 are Phase 5 optional.
set(SPLICE_TARGET_ARCH "unknown")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|AARCH64|ARM64)$")
    set(SPLICE_TARGET_ARCH "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(SPLICE_TARGET_ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM)$")
    set(SPLICE_TARGET_ARCH "arm32")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86)$")
    set(SPLICE_TARGET_ARCH "x86")
endif()
message(STATUS "Splice target arch: ${SPLICE_TARGET_ARCH} (CPU: ${CMAKE_SYSTEM_PROCESSOR})")

# ─── Warning flags (shared across targets) ───────────────────────────────────
add_library(splice_warnings INTERFACE)
if(MSVC)
    target_compile_options(splice_warnings INTERFACE /W4 /permissive-)
    if(SPLICE_WARNINGS_AS_ERRORS)
        target_compile_options(splice_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(splice_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast -Wconversion -Wsign-conversion)
    if(SPLICE_WARNINGS_AS_ERRORS)
        target_compile_options(splice_warnings INTERFACE -Werror)
    endif()
endif()

# ─── Sanitizers (shared across targets) ──────────────────────────────────────
add_library(splice_sanitizers INTERFACE)
if(SPLICE_ENABLE_ASAN)
    if(MSVC)
        target_compile_options(splice_sanitizers INTERFACE /fsanitize=address)
    else()
        target_compile_options(splice_sanitizers INTERFACE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(splice_sanitizers INTERFACE -fsanitize=address)
    endif()
endif()
if(SPLICE_ENABLE_UBSAN AND NOT MSVC)
    target_compile_options(splice_sanitizers INTERFACE
        -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(splice_sanitizers INTERFACE -fsanitize=undefined)
endif()
if(SPLICE_ENABLE_TSAN AND NOT MSVC)
    target_compile_options(splice_sanitizers INTERFACE
        -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(splice_sanitizers INTERFACE -fsanitize=thread)
endif()

# ─── Backend capability detection ────────────────────────────────────────────
# Splice needs (1) an OS abstraction (memory / modules) and (2) an arch patcher
# to install real hooks. The two are orthogonal:
#   - POSIX OS layer  (mmap / mprotect / dl_iterate_phdr) — Linux, Android
#   - Win32 OS layer  (VirtualAlloc / VirtualProtect)     — Windows
#   - ARM64 patcher   — Android ARM64 + Linux ARM64
#   - x86_64 patcher  — Linux x86_64 + Windows x86_64
# When both an OS layer and a matching arch patcher are present, the full
# engine is compiled; otherwise we compile the stub so the public API still
# exists for dev iteration.
if(WIN32)
    set(SPLICE_HAS_WIN32_OS ON)
    set(SPLICE_HAS_POSIX_HOOK OFF)
elseif(UNIX OR ANDROID)
    set(SPLICE_HAS_WIN32_OS OFF)
    set(SPLICE_HAS_POSIX_HOOK ON)
else()
    set(SPLICE_HAS_WIN32_OS OFF)
    set(SPLICE_HAS_POSIX_HOOK OFF)
endif()

if(SPLICE_TARGET_ARCH STREQUAL "arm64")
    set(SPLICE_HAS_ARM64_BACKEND ON)
else()
    set(SPLICE_HAS_ARM64_BACKEND OFF)
endif()

if(SPLICE_TARGET_ARCH STREQUAL "x86_64")
    set(SPLICE_HAS_X86_64_BACKEND ON)
else()
    set(SPLICE_HAS_X86_64_BACKEND OFF)
endif()

if((SPLICE_HAS_POSIX_HOOK OR SPLICE_HAS_WIN32_OS) AND
   (SPLICE_HAS_ARM64_BACKEND OR SPLICE_HAS_X86_64_BACKEND))
    set(SPLICE_HAS_ENGINE ON)
else()
    set(SPLICE_HAS_ENGINE OFF)
endif()

message(STATUS "  POSIX hook backend: ${SPLICE_HAS_POSIX_HOOK}")
message(STATUS "  Win32 OS layer    : ${SPLICE_HAS_WIN32_OS}")
message(STATUS "  ARM64 backend     : ${SPLICE_HAS_ARM64_BACKEND}")
message(STATUS "  x86_64 backend    : ${SPLICE_HAS_X86_64_BACKEND}")
message(STATUS "  Full engine       : ${SPLICE_HAS_ENGINE}")

# ─── Core library ────────────────────────────────────────────────────────────
# Phase 1: public headers split into traits/registry/trampoline/core/macros/
# engine; compiled sources include the engine dispatcher plus arch/os layers
# when the current platform supports them.
set(_splice_sources
    src/log.cpp
    src/context.cpp
)

# Disassemblers are pure bit-manipulation with no OS dependencies — always
# compile them so golden tests can run on any host (including Windows dev
# machines targeting non-native arches).
list(APPEND _splice_sources
    src/arch/arm64/disasm.cpp
    src/arch/x86_64/disasm.cpp
)

if(SPLICE_HAS_ENGINE)
    list(APPEND _splice_sources src/engine/engine.cpp)

    # OS layer — one of POSIX or Win32, never both.
    if(SPLICE_HAS_POSIX_HOOK)
        list(APPEND _splice_sources
            src/os/posix/memory.cpp
            src/engine/got_patcher.cpp  # ELF-only
        )
    elseif(SPLICE_HAS_WIN32_OS)
        list(APPEND _splice_sources
            src/os/win32/memory.cpp
            src/engine/iat_patcher.cpp  # PE/COFF Tier-1 hook strategy
        )
    endif()

    # Arch patcher(s).
    if(SPLICE_HAS_ARM64_BACKEND)
        list(APPEND _splice_sources
            src/arch/arm64/patcher.cpp
            src/arch/arm64/atomic_patch.cpp   # FR-011 ARMv8 atomic install
        )
    endif()
    if(SPLICE_HAS_X86_64_BACKEND)
        list(APPEND _splice_sources
            src/arch/x86_64/patcher.cpp
            src/arch/x86_64/atomic_patch.cpp  # FR-011 x86_64 atomic install
        )
    endif()
else()
    list(APPEND _splice_sources src/engine/engine_stub.cpp)
endif()

add_library(splice STATIC ${_splice_sources})
add_library(splice::splice ALIAS splice)

target_include_directories(splice PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
# Private include so backend TUs can reach their sibling headers
# (src/arch/**, src/os/**, src/engine/**) via relative paths.
target_include_directories(splice PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(splice PUBLIC cxx_std_20)
# Warnings + sanitizers are build-time-only concerns. Guard with
# BUILD_INTERFACE so they neither propagate to consumers nor block
# install(EXPORT) (an exported target may not reference non-exported ones).
target_link_libraries(splice PRIVATE
    $<BUILD_INTERFACE:splice_warnings>
    $<BUILD_INTERFACE:splice_sanitizers>)

if(SPLICE_HAS_ENGINE)
    if(SPLICE_HAS_POSIX_HOOK)
        target_compile_definitions(splice PRIVATE SPLICE_HAS_POSIX_HOOK=1)
        target_link_libraries(splice PRIVATE ${CMAKE_DL_LIBS})
        if(ANDROID)
            # platform_log.h routes to __android_log_print on Android — that
            # symbol lives in liblog.so, not libdl/libpthread.
            target_link_libraries(splice PUBLIC log)
        endif()
    endif()
    if(SPLICE_HAS_WIN32_OS)
        target_compile_definitions(splice PRIVATE SPLICE_HAS_WIN32_OS=1)
        # EnumProcessModules lives in psapi.lib
        target_link_libraries(splice PRIVATE psapi)
    endif()
    if(SPLICE_HAS_ARM64_BACKEND)
        target_compile_definitions(splice PRIVATE SPLICE_HAS_ARM64_BACKEND=1)
    endif()
    if(SPLICE_HAS_X86_64_BACKEND)
        target_compile_definitions(splice PRIVATE SPLICE_HAS_X86_64_BACKEND=1)
    endif()
endif()

# Make the vendored nmd header discoverable to src/arch/x86_64/disasm.cpp.
# SYSTEM marks it as external so /W4 warnings from nmd's C89 idioms don't
# pollute our build output — our own code still builds under full /W4.
target_include_directories(splice SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nmd)

if(NOT SPLICE_LOG_RUNTIME_LEVEL)
    target_compile_definitions(splice PUBLIC SPLICE_LOG_RUNTIME_LEVEL_DISABLED=1)
endif()

# Platform-specific log backend hints for the printf fallback (visible to all
# consumers of splice/log.h). Android detection is automatic via __ANDROID__.
if(NOT ANDROID AND SPLICE_IS_TOP_LEVEL)
    # Local dev: use printf backend with timestamp + PID/TID (logcat style)
    target_compile_definitions(splice PUBLIC
        PLOG_FORCE_CONSOLE
        PLOG_ENABLE_TIMESTAMP
        PLOG_TIMESTAMP_MS
        PLOG_ENABLE_PID_TID
    )
endif()

# PLOG auto-detects PLOG_LEVEL from DEBUG/_DEBUG, but NDK Clang defines
# neither for Debug builds (only MSVC sets _DEBUG). Force VERBOSE for any
# CMake Debug config so install paths log identically across platforms.
target_compile_definitions(splice PUBLIC
    $<$<CONFIG:Debug>:PLOG_LEVEL=0>
)

# ─── Tests ───────────────────────────────────────────────────────────────────
if(SPLICE_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif()

# ─── Examples ────────────────────────────────────────────────────────────────
# hello_hook / malloc_tracker / member_function (+ egl_frame_counter where
# EGL headers exist). See examples/README.md.
if(SPLICE_BUILD_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt)
    add_subdirectory(examples)
endif()

# ─── Benchmarks (FR-010) ─────────────────────────────────────────────────────
if(SPLICE_BUILD_BENCHMARKS)
    find_package(benchmark CONFIG REQUIRED)
    add_subdirectory(benchmark)
endif()

# ─── Install / export (find_package(splice)) ────────────────────────────────
# Lets downstream projects do:
#     find_package(splice CONFIG REQUIRED)
#     target_link_libraries(app PRIVATE splice::splice)
# Skipped for sub-builds (add_subdirectory consumers use the alias directly).
if(SPLICE_IS_TOP_LEVEL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS splice
        EXPORT spliceTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # Public headers — the whole include/splice tree (header-only API surface
    # plus the vendored detail/platform_log.h).
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/splice
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h")

    set(_splice_cmakedir ${CMAKE_INSTALL_LIBDIR}/cmake/splice)

    install(EXPORT spliceTargets
        FILE spliceTargets.cmake
        NAMESPACE splice::
        DESTINATION ${_splice_cmakedir})

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/spliceConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/spliceConfig.cmake
        INSTALL_DESTINATION ${_splice_cmakedir})

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/spliceConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/spliceConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/spliceConfigVersion.cmake
        DESTINATION ${_splice_cmakedir})
endif()

# ─── Summary ─────────────────────────────────────────────────────────────────
message(STATUS "─── Splice ${PROJECT_VERSION} ───────────────────────────────")
message(STATUS "  Build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Target arch       : ${SPLICE_TARGET_ARCH}")
message(STATUS "  Tests             : ${SPLICE_BUILD_TESTS}")
message(STATUS "  Examples          : ${SPLICE_BUILD_EXAMPLES}")
message(STATUS "  Benchmarks        : ${SPLICE_BUILD_BENCHMARKS}")
message(STATUS "  ASan              : ${SPLICE_ENABLE_ASAN}")
message(STATUS "  UBSan             : ${SPLICE_ENABLE_UBSAN}")
message(STATUS "  TSan              : ${SPLICE_ENABLE_TSAN}")
message(STATUS "  Runtime log level : ${SPLICE_LOG_RUNTIME_LEVEL}")
message(STATUS "─────────────────────────────────────────────────────────────")
