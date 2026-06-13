// ─── game — a tiny "GPU app" that renders a scene ──────────────────────────
//
// This is the application Splice will enhance. It is completely unaware of
// Splice: it just calls the gpu:: driver API in a render loop, exactly like
// a real game calls GLES. The Splice enhancer hooks the driver underneath
// it — the game source never changes.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

namespace game {

// The resolution the game asks for. The enhancer's job is to transparently
// upscale this — mirroring a production game enhancer enlarging a game's render target.
inline constexpr int kGameWidth  = 160;
inline constexpr int kGameHeight = 120;

// Render `frames` frames of a moving-triangle scene at the game's native
// resolution, writing the final frame to `out_ppm`. Returns the number of
// frames presented.
int run(int frames, const char* out_ppm);

} // namespace game
