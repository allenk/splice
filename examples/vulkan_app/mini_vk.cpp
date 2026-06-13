// ─── mini_vk — software Vulkan implementation ──────────────────────────────
//
// CPU framebuffer + barycentric rasteriser (same engine as the GLES demo).
// The command functions are file-static: unreachable except through
// vk_get_device_proc_addr — the authentic Vulkan dispatch model.
// ───────────────────────────────────────────────────────────────────────────
#include "mini_vk.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vk {
namespace {

struct Framebuffer {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;
};

Framebuffer& fb() {
    static Framebuffer instance;
    return instance;
}

inline long long edge(int ax, int ay, int bx, int by, int cx, int cy) {
    return static_cast<long long>(bx - ax) * (cy - ay) -
           static_cast<long long>(by - ay) * (cx - ax);
}

// ─── Command implementations (file-static — only reachable via the getter) ──

void cmd_set_viewport(int width, int height) {
    if (width <= 0 || height <= 0) return;
    auto& f = fb();
    f.width = width;
    f.height = height;
    f.rgb.assign(static_cast<std::size_t>(width) * height * 3, 0);
    std::printf("    [vk] vkCmdSetViewport(%d, %d)\n", width, height);
}

void cmd_clear(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    auto& f = fb();
    for (std::size_t i = 0; i + 2 < f.rgb.size(); i += 3) {
        f.rgb[i + 0] = r;
        f.rgb[i + 1] = g;
        f.rgb[i + 2] = b;
    }
}

void cmd_draw(int x0, int y0, int x1, int y1, int x2, int y2,
              std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    auto& f = fb();
    if (f.width == 0 || f.height == 0) return;

    const int min_x = std::max(0, std::min({x0, x1, x2}));
    const int max_x = std::min(f.width - 1, std::max({x0, x1, x2}));
    const int min_y = std::max(0, std::min({y0, y1, y2}));
    const int max_y = std::min(f.height - 1, std::max({y0, y1, y2}));

    const long long area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) return;

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const long long w0 = edge(x1, y1, x2, y2, x, y);
            const long long w1 = edge(x2, y2, x0, y0, x, y);
            const long long w2 = edge(x0, y0, x1, y1, x, y);
            const bool inside = (area > 0) ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
                                           : (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (inside) {
                const std::size_t idx =
                    (static_cast<std::size_t>(y) * f.width + x) * 3;
                f.rgb[idx + 0] = r;
                f.rgb[idx + 1] = g;
                f.rgb[idx + 2] = b;
            }
        }
    }
}

void queue_present(const char* ppm_path) {
    auto& f = fb();
    if (ppm_path == nullptr || f.width == 0 || f.height == 0) return;
    std::FILE* fp = std::fopen(ppm_path, "wb");
    if (!fp) return;
    std::fprintf(fp, "P6\n%d %d\n255\n", f.width, f.height);
    std::fwrite(f.rgb.data(), 1, f.rgb.size(), fp);
    std::fclose(fp);
    std::printf("    [vk] vkQueuePresentKHR -> %s (%dx%d)\n",
                ppm_path, f.width, f.height);
}

} // namespace

PFN_vkVoidFunction vk_get_device_proc_addr(VkDevice /*device*/, const char* name) {
    if (name == nullptr) return nullptr;
    if (std::strcmp(name, "vkCmdSetViewport") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&cmd_set_viewport);
    if (std::strcmp(name, "vkCmdClear") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&cmd_clear);
    if (std::strcmp(name, "vkCmdDraw") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&cmd_draw);
    if (std::strcmp(name, "vkQueuePresentKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&queue_present);
    return nullptr;   // unknown command
}

int current_width() noexcept { return fb().width; }
int current_height() noexcept { return fb().height; }

} // namespace vk
