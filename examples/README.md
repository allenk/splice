# Splice Examples

Standalone executables, each linking `splice::splice`. Built when
`SPLICE_BUILD_EXAMPLES=ON` (the default when Splice is the top-level project).

| Example | What it shows | Platforms |
|---|---|---|
| `hello_hook` | The 5-minute "does it work" — hook a free function, call `orig` through | all |
| `malloc_tracker` | Hook `malloc` by address, allocation-free `.before()` accounting | all (live where a patcher exists) |
| `member_function` | `SPLICE_HOOK_MEMBER` on a non-virtual method, explicit-`this` callback | all |
| `egl_frame_counter` | The game-enhancer pattern: `.after()` frame counter on `eglSwapBuffers` | Android / Mesa Linux (needs `EGL/egl.h`) |
| [`gpu_app/`](gpu_app/) | Full demo: software GPU + Splice enhancer that upscales render resolution 160×120 → 640×480 (the runnable production-enhancer pattern, **GLES-style inline patching**) | all |
| [`vulkan_app/`](vulkan_app/) | Same upscale, but the **Vulkan way**: Splice hooks `vkGetDeviceProcAddr` once and substitutes wrapper pointers (the RenderDoc/layer pattern) | all |

## Build & run

```bash
# Configure (any preset)
cmake --preset=windows-x64-dev          # or linux-x64-dev, android-arm64-dev

# Build one example
cmake --build --preset=windows-x64-dev --target splice_example_hello_hook

# Run it
./out/build/windows-x64-dev/examples/hello_hook
```

On a platform without a live patcher (e.g. a stub-only target), the
examples detect this via `splice_is_hooked()` and print a "no live patcher"
line instead of failing — so they always build and run cleanly.

## Quickstart (the < 10-minute path)

1. Read `hello_hook.cpp` — it's the whole API in 40 lines.
2. Copy the `SPLICE_HOOK_ADDR(&fn).onInvoke([](auto orig, ...){...})` +
   `splice::install_all()` shape into your code.
3. For richer patterns see [`../docs/splice-guide.md`](../docs/splice-guide.md)
   §10 (cookbook) — frame counters, timers, conditional gates, RAII scopes.
