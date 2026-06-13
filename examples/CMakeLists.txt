# ─── Splice examples ───────────────────────────────────────────────────────
# Built when -DSPLICE_BUILD_EXAMPLES=ON (default when splice is the top-level
# project). Each example is a standalone executable linking splice::splice.
#
# Binaries land under out/build/<preset>/examples/.
# ─────────────────────────────────────────────────────────────────────────────

# Portable examples — compile and run on every supported platform.
# (On stub-only builds they run too; they self-detect via splice_is_hooked
# and print a "no live patcher" line instead of asserting.)
set(_splice_examples
    hello_hook
    malloc_tracker
    member_function
)

foreach(_ex ${_splice_examples})
    add_executable(splice_example_${_ex} ${_ex}.cpp)
    target_link_libraries(splice_example_${_ex} PRIVATE splice::splice)
    set_target_properties(splice_example_${_ex} PROPERTIES OUTPUT_NAME ${_ex})
endforeach()

# ─── GPU demo — self-contained software GPU + Splice enhancer ───────────────
# The runnable analogue of reference/splice_game_Loader.cpp: a mini software
# GPU app that Splice hooks to upscale render resolution. Its own subdirectory
# (multiple TUs). See examples/gpu_app/README.md.
add_subdirectory(gpu_app)

# ─── Vulkan demo — software Vulkan + dispatch-getter interception ───────────
# Hooks vk_get_device_proc_addr and substitutes wrapper pointers (the real
# Vulkan-layer pattern), vs gpu_app's inline patching. See vulkan_app/README.
add_subdirectory(vulkan_app)

# ─── EGL example — only where the EGL header AND library both exist ─────────
# (Android NDK sysroot / Mesa Linux). The example takes &eglSwapBuffers for a
# splice_is_hooked() diagnostic, so the symbol must resolve at link time —
# the header alone is not enough.
find_path(SPLICE_EGL_INCLUDE_DIR EGL/egl.h)
find_library(SPLICE_EGL_LIBRARY EGL)
if(SPLICE_EGL_INCLUDE_DIR AND SPLICE_EGL_LIBRARY)
    add_executable(splice_example_egl_frame_counter egl_frame_counter.cpp)
    target_link_libraries(splice_example_egl_frame_counter
        PRIVATE splice::splice ${SPLICE_EGL_LIBRARY})
    target_include_directories(splice_example_egl_frame_counter
        PRIVATE ${SPLICE_EGL_INCLUDE_DIR})
    set_target_properties(splice_example_egl_frame_counter
        PROPERTIES OUTPUT_NAME egl_frame_counter)
    message(STATUS "  Example egl_frame_counter : ON (EGL at ${SPLICE_EGL_LIBRARY})")
else()
    message(STATUS "  Example egl_frame_counter : OFF (no EGL header+library on this host)")
endif()
