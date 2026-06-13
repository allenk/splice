// ─── vk_app — a tiny "Vulkan game" ─────────────────────────────────────────
//
// Resolves each command through vk_get_device_proc_addr (exactly how a real
// Vulkan app caches PFN_* pointers), then renders a scene. It is unaware of
// Splice. Because it re-resolves the procs at the start of each run, the
// enhanced pass transparently picks up the enhancer's wrapped pointers.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

namespace vk_app {

inline constexpr int kGameWidth  = 160;
inline constexpr int kGameHeight = 120;

// Render `frames` frames at native resolution, writing the final frame to
// out_ppm. Returns frames presented.
int run(int frames, const char* out_ppm);

} // namespace vk_app
