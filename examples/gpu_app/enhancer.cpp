// ─── enhancer — Splice hook setup ──────────────────────────────────────────
//
// Three hooks, mirroring the real a production game enhancer loader:
//
//   gpu::set_viewport  .when(enabled).onInvoke   → upscale the render target
//   gpu::draw_triangle .when(enabled).onInvoke   → scale geometry to match
//   gpu::present       .after                     → count frames (always on)
//
// The two upscale hooks are gated by .when(enabled) so toggling the enhancer
// off makes them transparent. The frame counter is ungated — it observes
// every frame regardless, like a telemetry probe.
// ───────────────────────────────────────────────────────────────────────────
#include "enhancer.h"

#include "mini_gpu.h"
#include "game.h"

#include <splice/splice.h>

#include <atomic>

namespace enhancer {
namespace {

std::atomic<bool>          g_enabled{false};
std::atomic<std::uint64_t> g_frames{0};

} // namespace

void set_enabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
bool enabled()            { return g_enabled.load(std::memory_order_relaxed); }
std::uint64_t frame_count() { return g_frames.load(std::memory_order_relaxed); }

void install() {
    // ── Hook 1: set_viewport — upscale the render resolution ──────────────
    // When the game asks for its native size, transparently enlarge it.
    // Identical intent to a production game enhancer rewriting ANativeWindow_setBuffersGeometry.
    SPLICE_HOOK_ADDR(&gpu::set_viewport)
        .when([] { return g_enabled.load(std::memory_order_relaxed); })
        .onInvoke([](auto orig, int w, int h) {
            if (w == game::kGameWidth && h == game::kGameHeight) {
                SPLICE_LOGI("[enhancer] upscaling viewport %dx%d -> %dx%d",
                            w, h, w * kUpscale, h * kUpscale);
                orig(w * kUpscale, h * kUpscale);
            } else {
                orig(w, h);
            }
        });

    // ── Hook 2: draw_triangle — scale geometry to the upscaled target ─────
    // The game emits coordinates in its native 160x120 space; multiply them
    // so the triangle lands correctly in the enlarged framebuffer. Mirrors
    // a production game enhancer scaling glViewport / glScissor / draw coordinates.
    SPLICE_HOOK_ADDR(&gpu::draw_triangle)
        .when([] { return g_enabled.load(std::memory_order_relaxed); })
        .onInvoke([](auto orig, int x0, int y0, int x1, int y1, int x2, int y2,
                     std::uint8_t r, std::uint8_t g, std::uint8_t b) {
            orig(x0 * kUpscale, y0 * kUpscale,
                 x1 * kUpscale, y1 * kUpscale,
                 x2 * kUpscale, y2 * kUpscale,
                 r, g, b);
        });

    // ── Hook 3: frame_mark — frame counter (ungated telemetry) ────────────
    // .after() — pure observation, no orig plumbing, void-return aware.
    // We hook the lightweight frame_mark seam rather than present(), because
    // present()'s libc-heavy prologue isn't relocatable for inline patching.
    SPLICE_HOOK_ADDR(&gpu::frame_mark)
        .after([](int /*frame_index*/) {
            g_frames.fetch_add(1, std::memory_order_relaxed);
        });

    splice::install_all();

    SPLICE_LOGI("[enhancer] installed (set_viewport hooked=%d, "
                "draw_triangle hooked=%d, frame_mark hooked=%d)",
                splice_is_hooked(reinterpret_cast<void*>(&gpu::set_viewport)),
                splice_is_hooked(reinterpret_cast<void*>(&gpu::draw_triangle)),
                splice_is_hooked(reinterpret_cast<void*>(&gpu::frame_mark)));
}

} // namespace enhancer
