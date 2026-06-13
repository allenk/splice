// ─── Splice Vulkan demo — main driver ──────────────────────────────────────
//
// Same before/after structure as the GLES demo, but the enhancement uses the
// AUTHENTIC Vulkan interception strategy: Splice hooks the dispatch getter
// (vk_get_device_proc_addr) and substitutes wrapper function pointers, rather
// than inline-patching each command.
//
//   Pass 1 (baseline): enhancer not installed → app resolves real commands →
//                       renders at native 160x120.
//   Pass 2 (enhanced): enhancer installed + enabled. The hooked getter hands
//                       the app wrapper pointers → upscaled to 640x480.
//
// Output: vk_baseline.ppm (160x120), vk_enhanced.ppm (640x480).
//
// Why this matters: it shows Splice altering a function's RETURN VALUE (a
// function pointer) to reroute behaviour — the dispatch-table interception
// that real Vulkan tools rely on. Compare examples/gpu_app, which inline-
// patches GLES commands directly.
// ───────────────────────────────────────────────────────────────────────────
#include "mini_vk.h"
#include "vk_app.h"
#include "vk_enhancer.h"

#include <splice/splice.h>   // splice_is_hooked

#include <cstdio>

namespace {
constexpr int kFrames = 8;
}

int main() {
    std::printf("=== Splice Vulkan demo (dispatch-getter interception) ===\n\n");

    std::printf("[pass 1] baseline (no enhancer)\n");
    vk_app::run(kFrames, "vk_baseline.ppm");
    std::printf("  -> framebuffer %dx%d\n\n",
                vk::current_width(), vk::current_height());

    vk_enhancer::install();
    std::printf("\n");

    std::printf("[pass 2] enhanced (Splice enabled, %dx upscale)\n",
                vk_enhancer::kUpscale);
    vk_enhancer::set_enabled(true);
    vk_app::run(kFrames, "vk_enhanced.ppm");
    std::printf("  -> framebuffer %dx%d\n\n",
                vk::current_width(), vk::current_height());

    const int bw = vk_app::kGameWidth, bh = vk_app::kGameHeight;
    std::printf("=== result ===\n");
    std::printf("  baseline : vk_baseline.ppm  %dx%d\n", bw, bh);
    std::printf("  enhanced : vk_enhanced.ppm  %dx%d  (%dx more pixels)\n",
                bw * vk_enhancer::kUpscale, bh * vk_enhancer::kUpscale,
                vk_enhancer::kUpscale * vk_enhancer::kUpscale);
    std::printf("  frames observed by vkQueuePresentKHR wrapper: %llu\n",
                static_cast<unsigned long long>(vk_enhancer::frame_count()));

    if (!splice_is_hooked(reinterpret_cast<void*>(&vk::vk_get_device_proc_addr))) {
        std::printf("\n  NOTE: no live patcher on this build (stub backend) —\n");
        std::printf("  the getter was not hooked; both passes ran native.\n");
    }
    return 0;
}
