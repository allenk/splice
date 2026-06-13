// ─── mini_gpu — a tiny software "GPU" for the Splice demo ──────────────────
//
// A self-contained stand-in for a real GLES driver. The functions below are
// the analogues of the GL calls a production game enhancer hooks in a real game:
//
//   gpu_set_viewport(w, h)   ~  ANativeWindow_setBuffersGeometry / glViewport
//   gpu_clear(r, g, b)       ~  glClear
//   gpu_draw_triangle(...)   ~  glDrawArrays / glDrawElements
//   gpu_present(path)        ~  eglSwapBuffers
//
// They are deliberately plain free functions in a separate TU, compiled
// noinline, so each has a real prologue that Splice can patch — exactly like
// hooking libGLESv2.so in the real world. The "GPU" keeps a CPU framebuffer
// and a barycentric rasterizer so the demo produces an actual image: the
// Splice enhancer can upscale the render resolution and you can SEE the
// before/after in the output PPMs.
//
// Cross-platform: pure C++17, no GL / EGL / windowing dependency. Runs on
// Windows, Linux, and Android identically.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>

#if defined(_MSC_VER)
#   define MINI_GPU_NOINLINE __declspec(noinline)
#else
#   define MINI_GPU_NOINLINE __attribute__((noinline))
#endif

namespace gpu {

// ─── Driver-facing API (the "GL" surface the game calls) ──────────────────
// All noinline + extern so the demo's Splice enhancer can hook them by
// address (SPLICE_HOOK_ADDR(&gpu::set_viewport), etc.).

// Resize the render target and (re)allocate the framebuffer. Analogue of
// the resolution-defining call a game makes once at startup.
MINI_GPU_NOINLINE void set_viewport(int width, int height);

// Fill the whole framebuffer with a solid colour.
MINI_GPU_NOINLINE void clear(std::uint8_t r, std::uint8_t g, std::uint8_t b);

// Rasterise a filled triangle in framebuffer pixel coordinates. Nine args
// on purpose — shows Splice handling a high-arity signature, and gives the
// enhancer real coordinates to rescale.
MINI_GPU_NOINLINE void draw_triangle(int x0, int y0,
                                     int x1, int y1,
                                     int x2, int y2,
                                     std::uint8_t r, std::uint8_t g, std::uint8_t b);

// Per-frame heartbeat. The game calls this once at the end of every frame.
// Analogue of eglSwapBuffers' frame-boundary role — the lightweight signal
// the enhancer hooks to count frames. Kept deliberately tiny with a
// param-only body so its prologue is always relocatable (a libc-heavy
// function like present() loads globals via RIP-relative addressing in its
// first bytes, which the inline patcher can't relocate — see the demo
// README's "what's hookable" note).
MINI_GPU_NOINLINE void frame_mark(int frame_index);

// "Swap buffers": write the current framebuffer to a PPM file. Analogue of
// eglSwapBuffers' present role. NOT hooked in this demo — its fopen/fprintf
// prologue isn't relocatable; frame counting goes through frame_mark instead.
MINI_GPU_NOINLINE void present(const char* ppm_path);

// ─── Introspection (for the demo's reporting; not hooked) ─────────────────
int current_width() noexcept;
int current_height() noexcept;

} // namespace gpu
