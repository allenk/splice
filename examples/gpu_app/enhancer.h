// ─── enhancer — the Splice "loader" for the GPU app ────────────────────────
//
// This is the production game-enhancer pattern in miniature. It installs Splice hooks on the
// gpu:: driver to transparently upscale the game's render resolution — the
// game keeps asking for 160x120, the enhancer makes the GPU render 640x480
// and rescales the geometry to match. Plus a frame counter, gated so it can
// be toggled at runtime.
//
// The game source is never touched; all behaviour change happens through
// Splice hooks underneath it.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>

namespace enhancer {

// Upscale factor. game 160x120 * 4 = 640x480.
inline constexpr int kUpscale = 4;

// Install the Splice hooks and run install_all(). Call once. After this the
// hooks are live for the rest of the process (fire-and-forget, the natural
// fit for a game enhancer — see splice_game_Loader_minimal.cpp).
void install();

// Runtime toggle. When disabled, the gated hooks pass straight through to the
// original driver (zero behaviour change) — like the enhancer's setEnabled.
void set_enabled(bool on);
bool enabled();

// Frames observed by the present() hook since install.
std::uint64_t frame_count();

} // namespace enhancer
