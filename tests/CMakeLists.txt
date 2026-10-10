# ─── Splice tests ────────────────────────────────────────────────────────────
# Phase 0: smoke tests
# Phase 1: unit tests (traits / registry / trampoline / core)
# Phase 1.5: context isolation tests
# Phase 2: ARM64 disasm golden tests + test helpers
# ─────────────────────────────────────────────────────────────────────────────

# Smoke tests (Phase 0)
add_executable(splice_smoke_test
    smoke_test.cpp
)
target_link_libraries(splice_smoke_test PRIVATE
    splice::splice
    splice_warnings
    splice_sanitizers
    GTest::gtest
    GTest::gtest_main
)

# The version the build system derived from include/splice/splice.h, handed to
# the test so it can assert the compiled constants equal it. Passing the
# numbers rather than letting the test read the header twice is the point: the
# two values arrive by different routes and are compared.
target_compile_definitions(splice_smoke_test PRIVATE
    SPLICE_TEST_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    SPLICE_TEST_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    SPLICE_TEST_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# Unit tests (Phase 1 + 1.5 + 2 + 4.5d stress)
# Task #57: stress_test.cpp used to live in its own binary because pre-RAII
# installer queues left dangling lambdas across tests. With InstallerToken
# in place, the unit test binary is clean and absorbs the stress workload.
add_executable(splice_unit_test
    test_traits.cpp
    test_registry.cpp
    test_trampoline.cpp
    test_core.cpp
    test_context.cpp
    test_arm64_disasm.cpp
    test_arm64_reach.cpp
    test_x86_64_disasm.cpp
    test_hook_global.cpp
    test_hook_iat.cpp
    test_hook_inline_disable.cpp
    test_hook_storage.cpp
    test_hook_policy_override.cpp
    test_hook_registry.cpp
    test_hook_modifiers.cpp
    test_scoped_hook.cpp
    test_diagnostics.cpp
    test_near_alloc.cpp
    test_prologue_census.cpp
    test_hook_member.cpp
    stress_test.cpp
    helpers/test_targets.cpp
)
target_link_libraries(splice_unit_test PRIVATE
    splice::splice
    splice_warnings
    splice_sanitizers
    GTest::gtest
    GTest::gtest_main
)
# Let ARM64 golden tests reach private src/arch/arm64/disasm.h
target_include_directories(splice_unit_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/helpers
)

# gtest_discover_tests runs the test binary on the host to enumerate cases.
# Pointless (and broken) when cross-compiling for Android: the host would try
# to launch an aarch64 ELF. Just register a single ctest per binary on the
# host's PATH, and let the user run the binaries manually on-device via adb.
include(GoogleTest)
if(NOT CMAKE_CROSSCOMPILING)
    # Host-only failure path: real ARM64 patcher/decoder, no ARM instructions run.
    add_executable(splice_arm64_patcher_failure_test
        test_arm64_patcher_failure.cpp
        ../src/arch/arm64/patcher.cpp
        ../src/arch/arm64/disasm.cpp
    )
    target_include_directories(splice_arm64_patcher_failure_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(splice_arm64_patcher_failure_test PRIVATE
        splice_warnings splice_sanitizers GTest::gtest_main)
    gtest_discover_tests(splice_arm64_patcher_failure_test DISCOVERY_TIMEOUT 30)
endif()
if(SPLICE_HAS_X86_64_BACKEND AND NOT CMAKE_CROSSCOMPILING)
    add_executable(splice_strict_install_test test_strict_install.cpp test_prepared_patch.cpp)
    target_include_directories(splice_strict_install_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(splice_strict_install_test PRIVATE
        splice::splice splice_warnings splice_sanitizers GTest::gtest_main)
    if(WIN32)
        target_sources(splice_strict_install_test PRIVATE test_thread_transaction.cpp)
        add_library(splice_strict_decoy SHARED helpers/strict_decoy.cpp)
        target_include_directories(splice_strict_decoy PRIVATE ${CMAKE_SOURCE_DIR}/src)
        target_link_libraries(splice_strict_decoy PRIVATE splice::splice splice_warnings)
        target_link_libraries(splice_strict_install_test PRIVATE splice_strict_decoy)
    endif()
    gtest_discover_tests(splice_strict_install_test DISCOVERY_TIMEOUT 30)
    # Real patcher with a deterministic OS fault seam; no production fault flags.
    add_executable(splice_patcher_failure_test
        test_patcher_failure.cpp
        ../src/arch/x86_64/patcher.cpp
        ../src/arch/x86_64/disasm.cpp
        ../src/arch/x86_64/atomic_patch.cpp
    )
    target_include_directories(splice_patcher_failure_test PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
    target_include_directories(splice_patcher_failure_test SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/nmd)
    target_link_libraries(splice_patcher_failure_test PRIVATE
        splice_warnings splice_sanitizers GTest::gtest_main)
    gtest_discover_tests(splice_patcher_failure_test DISCOVERY_TIMEOUT 30)
endif()
if(CMAKE_CROSSCOMPILING)
    add_test(NAME splice_smoke_test
             COMMAND ${CMAKE_COMMAND} -E echo
             "splice_smoke_test built — adb push $<TARGET_FILE:splice_smoke_test> /data/local/tmp/ to run")
    add_test(NAME splice_unit_test
             COMMAND ${CMAKE_COMMAND} -E echo
             "splice_unit_test built — adb push $<TARGET_FILE:splice_unit_test> /data/local/tmp/ to run")
else()
    gtest_discover_tests(splice_smoke_test  DISCOVERY_TIMEOUT 30)
    gtest_discover_tests(splice_unit_test   DISCOVERY_TIMEOUT 60)
endif()
