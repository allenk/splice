// ─── vk_app — render loop ──────────────────────────────────────────────────
//
// The canonical Vulkan startup shape: resolve every command via the dispatch
// getter, cache the PFN_* pointers, then call through them. The app casts the
// generic PFN_vkVoidFunction to the specific type — just like real Vulkan.
// ───────────────────────────────────────────────────────────────────────────
#include "vk_app.h"

#include "mini_vk.h"

#include <cstdio>

namespace vk_app {

int run(int frames, const char* out_ppm) {
    vk::VkDevice dev = reinterpret_cast<vk::VkDevice>(0x1);  // dummy handle

    // Resolve commands through the getter. If the Splice enhancer is
    // installed + enabled, this getter is hooked and hands back wrappers.
    auto set_viewport = reinterpret_cast<vk::PFN_CmdSetViewport>(
        vk::vk_get_device_proc_addr(dev, "vkCmdSetViewport"));
    auto clear = reinterpret_cast<vk::PFN_CmdClear>(
        vk::vk_get_device_proc_addr(dev, "vkCmdClear"));
    auto draw = reinterpret_cast<vk::PFN_CmdDraw>(
        vk::vk_get_device_proc_addr(dev, "vkCmdDraw"));
    auto present = reinterpret_cast<vk::PFN_QueuePresent>(
        vk::vk_get_device_proc_addr(dev, "vkQueuePresentKHR"));

    if (!set_viewport || !clear || !draw || !present) {
        std::printf("  [vk_app] failed to resolve commands\n");
        return 0;
    }

    set_viewport(kGameWidth, kGameHeight);

    for (int frame = 0; frame < frames; ++frame) {
        clear(20, 20, 40);

        const int t = (kGameWidth - 40) * frame / (frames > 1 ? frames - 1 : 1);
        draw(t, kGameHeight - 20, t + 20, kGameHeight - 20, t + 10, kGameHeight - 50,
             240, 80, 80);
        draw(5, 5, 25, 5, 5, 25, 80, 200, 120);

        present(frame == frames - 1 ? out_ppm : nullptr);  // present every frame
    }

    std::printf("  [vk_app] rendered %d frames at native %dx%d\n",
                frames, kGameWidth, kGameHeight);
    return frames;
}

} // namespace vk_app
