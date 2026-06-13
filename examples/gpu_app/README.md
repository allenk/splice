# Splice GPU Demo

A self-contained, cross-platform demonstration of the **a production game enhancer enhancement
pattern**: a "game" renders through a GPU driver at its native resolution, and
a Splice enhancer transparently **upscales the render resolution** underneath
it — the game source never changes.

This is `reference/splice_game_Loader.cpp` made real and runnable, with a
software GPU standing in for `libGLESv2.so`.

## What's in here

| File | Role | Real-world analogue |
|---|---|---|
| `mini_gpu.{h,cpp}` | Software GPU driver — framebuffer + barycentric rasteriser + PPM writer | `libGLESv2.so` / `libEGL.so` |
| `game.{h,cpp}` | The "game": a render loop that draws a moving-triangle scene. Knows nothing about Splice. | A real game binary |
| `enhancer.{h,cpp}` | The Splice loader: hooks the driver to upscale + count frames | `a production game enhancer_Loader.cpp` |
| `main.cpp` | Runs the game baseline (no hooks) then enhanced (Splice installed) | The injector / process attach |

## The hooks (mirroring a production game enhancer)

| Driver function | Hook | Effect | a production game enhancer analogue |
|---|---|---|---|
| `gpu::set_viewport` | `.when(enabled).onInvoke` | rewrite 160×120 → 640×480 | `ANativeWindow_setBuffersGeometry` upscale |
| `gpu::draw_triangle` | `.when(enabled).onInvoke` | scale geometry ×4 | `glViewport` / `glScissor` / draw rescale |
| `gpu::frame_mark` | `.after` | count frames | `eglSwapBuffers` frame counter |

## Build & run

```bash
cmake --preset=windows-x64-dev     # or linux-x64-dev, android-arm64-dev
cmake --build --preset=windows-x64-dev --target splice_gpu_demo
./out/build/windows-x64-dev/examples/gpu_app/gpu_demo
```

Output (the rewrite made visible):

```
[pass 1] baseline (no enhancer)      -> gpu_baseline.ppm  160x120
[pass 2] enhanced (Splice, 4x)       -> gpu_enhanced.ppm  640x480
  frames observed by frame_mark() hook: 8
```

Open `gpu_baseline.ppm` and `gpu_enhanced.ppm` in any image viewer (or
`magick gpu_enhanced.ppm out.png`) — the same scene, rendered at 16× the
pixel count, entirely through Splice hooks. The game asked for 160×120 both
times.

### Result (verified on Windows x86_64 and Snapdragon 8 Gen 3 ARM64)

| Baseline (160×120) | Enhanced via Splice (640×480) |
|---|---|
| ![baseline](../../docs/attachments/gpu_demo_baseline.png) | ![enhanced](../../docs/attachments/gpu_demo_enhanced.png) |

Same scene composition (green HUD top-left, red triangle bottom-right at its
final frame position). Pixel-count analysis confirms the **geometry** was
rescaled, not merely the framebuffer enlarged: background 18,648 → 298,998
(≈16×), red triangle 321 → 4,881, green HUD 231 → 3,321. The ARM64 device
produces a byte-identical 640×480 image via real inline patching.

## "What's hookable" — a real lesson this demo teaches

The frame counter hooks `frame_mark`, **not** `present`, on purpose. Inline
patching needs to relocate the first ~16 bytes of the target's prologue into
a trampoline. Two prologues that bite:

- **`present()`** — its `fopen`/`fprintf` body loads globals via RIP-relative
  addressing in the first bytes; those can't be relocated. Install fails
  cleanly (logged), it doesn't crash.
- **Release `/O2` tiny functions** — the optimiser front-loads RIP-relative
  global access and may ICF-fold identical bodies, so even `set_viewport`
  can become un-hookable in an optimised build.

So `frame_mark` is a deliberately tiny, param-only function: its prologue is
always relocatable. This mirrors a real integration choice — hook a clean,
stable seam rather than a libc-heavy one. See `docs/splice-guide.md` §9.6
(disassembler boundaries) for the full story.

**This demo builds and runs in Debug** (where the small driver functions have
relocatable prologues). It is illustrative of the *pattern*; a production
target would hook real GLES entry points, which have stable, hookable
prologues by design.
