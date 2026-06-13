# Splice Vulkan Demo — dispatch-getter interception

The Vulkan companion to [`../gpu_app`](../gpu_app). Same visible result
(transparently upscale a "game" 160×120 → 640×480), but it teaches the
**authentic Vulkan interception strategy**, which is fundamentally different
from GLES.

## GLES vs Vulkan hooking — the point of this demo

| | GLES (`gpu_app`) | Vulkan (this demo) |
|---|---|---|
| How commands are reached | `libGLESv2.so` **exports** `glViewport` etc. | via a **dispatch getter**: `vkGetDeviceProcAddr(dev, "vkCmdDraw")` returns a function pointer |
| How you intercept | inline/GOT-patch **each command** (a production game enhancer, `gpu_app`) | hook the **getter once**, return your own **wrapper pointers** (RenderDoc, MangoHud, Vulkan layers) |
| Splice hooks… | N functions (one per command) | **1 function** (`vk_get_device_proc_addr`) |
| Splice feature shown | `.onInvoke` rewriting **arguments** | `.onInvoke` rewriting the **return value** (a function pointer) |

Real Vulkan tools never inline-patch `vkCmdDraw`; they sit in the dispatch
chain and hand the app substitute pointers. This demo reproduces exactly that
with one Splice hook.

## How it works

1. `mini_vk` — software Vulkan. The command implementations (`cmd_set_viewport`,
   `cmd_draw`, `queue_present`…) are **file-static**: the *only* way to get
   them is `vk_get_device_proc_addr(device, name)`. That getter is the single
   hookable export.
2. `vk_app` — resolves each command through the getter (caching `PFN_*`
   pointers, like a real Vulkan app), then renders.
3. `vk_enhancer` — **one** Splice hook on `vk_get_device_proc_addr`. When the
   app resolves a command, the hook captures the real pointer and returns a
   wrapper instead: `wrapped_set_viewport`/`wrapped_draw` upscale ×4,
   `wrapped_present` counts frames. Toggle via `set_enabled`.

A neat consequence: **`vkQueuePresentKHR` is frame-counted even though its
real body is libc-heavy** (fopen/fprintf). In `gpu_app` that same heavy
function *couldn't* be inline-hooked (its prologue isn't relocatable) — but
the dispatch-substitution approach never patches it, so there's no such
limit. Different strategy, different constraints.

## Build & run

```bash
cmake --preset=windows-x64-dev          # or linux-x64-dev, android-arm64-dev
cmake --build --preset=windows-x64-dev --target splice_vulkan_demo
./out/build/windows-x64-dev/examples/vulkan_app/vulkan_demo
```

Output:

```
[pass 1] baseline (no enhancer)      -> vk_baseline.ppm  160x120
[pass 2] enhanced (Splice enabled)   -> vk_enhanced.ppm  640x480
  frames observed by vkQueuePresentKHR wrapper: 8
```

### Result (verified Windows x86_64 + Snapdragon 8 Gen 3 ARM64)

| Baseline (160×120) | Enhanced via dispatch hook (640×480) |
|---|---|
| ![baseline](../../docs/attachments/vk_demo_baseline.png) | ![enhanced](../../docs/attachments/vk_demo_enhanced.png) |

Pixel-count analysis matches the GLES demo (bg 18,648→298,998, red
321→4,881, green 231→3,321) — same scene, upscaled. ARM64 device produces a
byte-identical 640×480 image. Only `vk_get_device_proc_addr` was inline-
patched; every command change is pointer substitution.

## Note

Software-backed (no Vulkan SDK / driver / window), so it runs on any
platform — same offscreen-FBO-readback model as `gpu_app`. The *names* and
*dispatch flow* mirror real Vulkan; the rendering is a CPU rasteriser. For a
real-Vulkan build (SwiftShader/lavapipe headless) the same Splice hook on the
real `vkGetDeviceProcAddr` would apply — that's option C in the design notes,
deliberately not taken here to keep the demo dependency-free.
