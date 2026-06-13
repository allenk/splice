// ─── vk_enhancer — the Splice "Vulkan layer" ───────────────────────────────
//
// Demonstrates the AUTHENTIC Vulkan interception strategy: hook the dispatch
// getter (vk_get_device_proc_addr) and return wrapper function pointers. Only
// ONE function is inline-hooked; every command enhancement happens by pointer
// substitution — exactly how RenderDoc / MangoHud / Vulkan layers work.
//
// Contrast with examples/gpu_app, which inline-patches each GLES command.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>

namespace vk_enhancer {

inline constexpr int kUpscale = 4;   // 160x120 -> 640x480

void install();                 // hook the getter, run install_all()
void set_enabled(bool on);
bool enabled();
std::uint64_t frame_count();    // counted by the present wrapper

} // namespace vk_enhancer
