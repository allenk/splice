// ─── Splice GPU demo — main driver ─────────────────────────────────────────
//
// Runs the same unmodified "game" twice in one process:
//
//   Pass 1 (baseline): no hooks. Game renders at its native 160x120.
//   Pass 2 (enhanced): Splice enhancer installed + enabled. The game STILL
//                      asks for 160x120, but the hooks transparently upscale
//                      it to 640x480 and rescale the geometry.
//
// Output: out/gpu_baseline.ppm (160x120) and out/gpu_enhanced.ppm (640x480),
// the same scene at two resolutions — the rewrite made visible. Open them in
// any image viewer (PPM is widely supported; or convert with ImageMagick).
//
// This is reference/splice_game_Loader.cpp made real and runnable, with a
// software GPU standing in for libGLESv2.so.
// ───────────────────────────────────────────────────────────────────────────
#include "enhancer.h"
#include "game.h"
#include "mini_gpu.h"

#include <splice/splice.h>   // splice_is_hooked

#include <cstdio>

namespace {
constexpr int kFrames = 8;
}

int main() {
    std::printf("=== Splice GPU demo ===\n\n");

    // ── Pass 1: baseline, no Splice hooks ─────────────────────────────────
    std::printf("[pass 1] baseline (no enhancer)\n");
    game::run(kFrames, "gpu_baseline.ppm");
    std::printf("  -> framebuffer %dx%d\n\n",
                gpu::current_width(), gpu::current_height());

    // ── Install the Splice enhancer ───────────────────────────────────────
    // After this the driver entry points are patched, but the upscale hooks
    // stay transparent until we enable them.
    enhancer::install();
    std::printf("\n");

    // ── Pass 2: enhanced — same game code, hooks upscale underneath ───────
    std::printf("[pass 2] enhanced (Splice enabled, %dx upscale)\n",
                enhancer::kUpscale);
    enhancer::set_enabled(true);
    game::run(kFrames, "gpu_enhanced.ppm");
    std::printf("  -> framebuffer %dx%d\n\n",
                gpu::current_width(), gpu::current_height());

    // ── Report ────────────────────────────────────────────────────────────
    const int base_w = game::kGameWidth, base_h = game::kGameHeight;
    std::printf("=== result ===\n");
    std::printf("  baseline : gpu_baseline.ppm  %dx%d\n", base_w, base_h);
    std::printf("  enhanced : gpu_enhanced.ppm  %dx%d  (%dx more pixels)\n",
                base_w * enhancer::kUpscale, base_h * enhancer::kUpscale,
                enhancer::kUpscale * enhancer::kUpscale);
    std::printf("  frames observed by frame_mark() hook: %llu\n",
                static_cast<unsigned long long>(enhancer::frame_count()));

    if (!splice_is_hooked(reinterpret_cast<void*>(&gpu::set_viewport))) {
        std::printf("\n  NOTE: no live patcher on this build (stub backend) —\n");
        std::printf("  both passes rendered at native resolution.\n");
    }
    return 0;
}
