# ─── Splice GPU demo ───────────────────────────────────────────────────────
# A self-contained software-GPU app + a Splice enhancer that hooks it to
# upscale the render resolution. The runnable, cross-platform analogue of
# reference/splice_game_Loader.cpp.
#
# Driver (mini_gpu) and game live in their own TUs, compiled noinline, so the
# enhancer can patch real prologues — exactly like hooking libGLESv2.so.
# ─────────────────────────────────────────────────────────────────────────────
add_executable(splice_gpu_demo
    main.cpp
    enhancer.cpp
    game.cpp
    mini_gpu.cpp
)
target_link_libraries(splice_gpu_demo PRIVATE splice::splice)
set_target_properties(splice_gpu_demo PROPERTIES OUTPUT_NAME gpu_demo)
