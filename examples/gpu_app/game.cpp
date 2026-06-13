// ─── game — render loop implementation ─────────────────────────────────────
//
// Draws a background plus a triangle that slides across the screen over the
// course of the run. Pure driver calls — no Splice, no awareness of hooks.
// ───────────────────────────────────────────────────────────────────────────
#include "game.h"

#include "mini_gpu.h"

#include <cstdio>

namespace game {

int run(int frames, const char* out_ppm) {
    // The game defines its render resolution. (The enhancer may rewrite this
    // call underneath us — we neither know nor care.)
    gpu::set_viewport(kGameWidth, kGameHeight);

    for (int frame = 0; frame < frames; ++frame) {
        // Background.
        gpu::clear(20, 20, 40);

        // A triangle that marches left-to-right across the native game space.
        // Coordinates are in the GAME's coordinate system (0..kGameWidth).
        const int t = (kGameWidth - 40) * frame /
                      (frames > 1 ? frames - 1 : 1);
        gpu::draw_triangle(t,        kGameHeight - 20,
                           t + 20,   kGameHeight - 20,
                           t + 10,   kGameHeight - 50,
                           240, 80, 80);

        // A static "HUD" marker in the corner.
        gpu::draw_triangle(5, 5, 25, 5, 5, 25, 80, 200, 120);

        // End-of-frame heartbeat (the eglSwapBuffers frame-boundary signal).
        gpu::frame_mark(frame);

        // Dump the final frame to an image so the result is visible.
        if (frame == frames - 1) {
            gpu::present(out_ppm);
        }
    }

    std::printf("  [game] rendered %d frames at native %dx%d\n",
                frames, kGameWidth, kGameHeight);
    return frames;
}

} // namespace game
