// ─── vk_enhancer — Splice hooks the Vulkan dispatch getter ─────────────────
//
// The whole enhancement is ONE Splice hook on vk_get_device_proc_addr. When
// the app resolves a command, the hook:
//   - calls orig() to get the REAL command pointer,
//   - stashes it, and
//   - returns OUR wrapper pointer instead (when enabled).
// The wrappers call the stashed real pointer with modified arguments. This is
// the Vulkan-layer pattern — no command is inline-patched; we substitute the
// pointers the app caches.
// ───────────────────────────────────────────────────────────────────────────
#include "vk_enhancer.h"

#include "mini_vk.h"
#include "vk_app.h"

#include <splice/splice.h>

#include <atomic>
#include <cstring>

namespace vk_enhancer {
namespace {

std::atomic<bool>          g_enabled{false};
std::atomic<std::uint64_t> g_frames{0};

// Real command pointers, captured the first time the app resolves each one
// through the hooked getter.
vk::PFN_CmdSetViewport g_real_set_viewport = nullptr;
vk::PFN_CmdDraw        g_real_draw         = nullptr;
vk::PFN_QueuePresent   g_real_present      = nullptr;

// ─── Wrapper commands (what the app unknowingly calls instead) ────────────

void wrapped_set_viewport(int w, int h) {
    if (g_enabled.load(std::memory_order_relaxed) &&
        w == vk_app::kGameWidth && h == vk_app::kGameHeight) {
        SPLICE_LOGI("[vk_enhancer] upscaling viewport %dx%d -> %dx%d",
                    w, h, w * kUpscale, h * kUpscale);
        g_real_set_viewport(w * kUpscale, h * kUpscale);
    } else {
        g_real_set_viewport(w, h);
    }
}

void wrapped_draw(int x0, int y0, int x1, int y1, int x2, int y2,
                  std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (g_enabled.load(std::memory_order_relaxed)) {
        g_real_draw(x0 * kUpscale, y0 * kUpscale,
                    x1 * kUpscale, y1 * kUpscale,
                    x2 * kUpscale, y2 * kUpscale, r, g, b);
    } else {
        g_real_draw(x0, y0, x1, y1, x2, y2, r, g, b);
    }
}

void wrapped_present(const char* ppm_path) {
    g_frames.fetch_add(1, std::memory_order_relaxed);   // count every frame
    g_real_present(ppm_path);
}

} // namespace

void set_enabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
bool enabled()            { return g_enabled.load(std::memory_order_relaxed); }
std::uint64_t frame_count() { return g_frames.load(std::memory_order_relaxed); }

void install() {
    // The single hook: intercept the dispatch getter. Its return value is a
    // function pointer, which the .onInvoke lambda rewrites.
    SPLICE_HOOK_ADDR(&vk::vk_get_device_proc_addr)
        .onInvoke([](auto orig, vk::VkDevice dev, const char* name)
                      -> vk::PFN_vkVoidFunction {
            vk::PFN_vkVoidFunction real = orig(dev, name);
            if (real == nullptr || name == nullptr) return real;

            // Capture the real pointer + hand back our wrapper. We always
            // substitute (the wrappers themselves honour the enabled flag),
            // so toggling enabled needs no re-resolve.
            if (std::strcmp(name, "vkCmdSetViewport") == 0) {
                g_real_set_viewport = reinterpret_cast<vk::PFN_CmdSetViewport>(real);
                return reinterpret_cast<vk::PFN_vkVoidFunction>(&wrapped_set_viewport);
            }
            if (std::strcmp(name, "vkCmdDraw") == 0) {
                g_real_draw = reinterpret_cast<vk::PFN_CmdDraw>(real);
                return reinterpret_cast<vk::PFN_vkVoidFunction>(&wrapped_draw);
            }
            if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
                g_real_present = reinterpret_cast<vk::PFN_QueuePresent>(real);
                return reinterpret_cast<vk::PFN_vkVoidFunction>(&wrapped_present);
            }
            return real;   // pass through commands we don't enhance
        });

    splice::install_all();

    SPLICE_LOGI("[vk_enhancer] installed (vkGetDeviceProcAddr hooked=%d)",
                splice_is_hooked(
                    reinterpret_cast<void*>(&vk::vk_get_device_proc_addr)));
}

} // namespace vk_enhancer
