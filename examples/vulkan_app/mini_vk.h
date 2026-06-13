// ─── mini_vk — a tiny software "Vulkan" for the Splice demo ────────────────
//
// The companion to examples/gpu_app (which mirrored GLES). This one mirrors
// **Vulkan's dispatch model**, because Vulkan is hooked very differently from
// GLES:
//
//   GLES   : libGLESv2.so exports glViewport etc. → you inline/GOT-patch them
//            individually (what examples/gpu_app + a production game enhancer do).
//   Vulkan : commands are reached through a dispatch getter —
//            vkGetDeviceProcAddr(device, "vkCmdDraw") returns a function
//            pointer. Real interception (RenderDoc, MangoHud, layers) hooks
//            the GETTER and returns its own wrappers — it does NOT inline-
//            patch each command.
//
// So here the command implementations are file-static (NOT exported): the
// only way to obtain them is `vk_get_device_proc_addr`. That single function
// is the one Splice hooks — the authentic Vulkan-layer interception point.
//
// Software-backed (CPU framebuffer + rasteriser), so it builds and runs on
// Windows/Linux/Android with no Vulkan SDK, driver, or window — same offscreen
// FBO-readback model as the GLES demo.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>

#if defined(_MSC_VER)
#   define MINI_VK_NOINLINE __declspec(noinline)
#else
#   define MINI_VK_NOINLINE __attribute__((noinline))
#endif

namespace vk {

// Opaque device handle, à la VkDevice. The demo passes a dummy non-null value.
using VkDevice = void*;

// Generic function-pointer return type, à la PFN_vkVoidFunction. Callers cast
// the result to the specific PFN_* for the command they asked for — exactly
// how real Vulkan code uses vkGetDeviceProcAddr.
using PFN_vkVoidFunction = void (*)();

// Command signatures (simplified, software-backed). Names map to real Vulkan:
using PFN_CmdSetViewport = void (*)(int width, int height);                 // ~ vkCmdSetViewport
using PFN_CmdClear       = void (*)(std::uint8_t r, std::uint8_t g, std::uint8_t b); // ~ vkCmdClearAttachments
using PFN_CmdDraw        = void (*)(int x0, int y0, int x1, int y1, int x2, int y2,  // ~ vkCmdDraw
                                    std::uint8_t r, std::uint8_t g, std::uint8_t b);
using PFN_QueuePresent   = void (*)(const char* ppm_path);                  // ~ vkQueuePresentKHR

// THE interception point. Resolve a command by name; returns nullptr for an
// unknown name (like the real getter). This is the single function the Splice
// enhancer hooks — it returns wrappers instead of the real pointers.
MINI_VK_NOINLINE PFN_vkVoidFunction vk_get_device_proc_addr(VkDevice device,
                                                            const char* name);

// Introspection for the demo's report (not part of the dispatch surface).
int current_width() noexcept;
int current_height() noexcept;

} // namespace vk
