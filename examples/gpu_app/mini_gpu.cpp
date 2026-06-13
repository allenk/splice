// ─── mini_gpu — software GPU implementation ────────────────────────────────
//
// A singleton CPU framebuffer + a barycentric triangle rasteriser. This is
// the "driver" — the game calls into it, and Splice can patch its entry
// points just like a real libGLESv2.so.
// ───────────────────────────────────────────────────────────────────────────
#include "mini_gpu.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace gpu {
namespace {

// The framebuffer is process-global driver state — like a real GL context.
struct Framebuffer {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;   // width*height*3, row-major
};

Framebuffer& fb() {
    static Framebuffer instance;
    return instance;
}

// Edge function for barycentric coverage.
inline long long edge(int ax, int ay, int bx, int by, int cx, int cy) {
    return static_cast<long long>(bx - ax) * (cy - ay) -
           static_cast<long long>(by - ay) * (cx - ax);
}

} // namespace

void set_viewport(int width, int height) {
    if (width <= 0 || height <= 0) return;
    auto& f = fb();
    f.width = width;
    f.height = height;
    f.rgb.assign(static_cast<std::size_t>(width) * height * 3, 0);
    std::printf("    [gpu] set_viewport(%d, %d)\n", width, height);
}

void clear(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    auto& f = fb();
    for (std::size_t i = 0; i + 2 < f.rgb.size(); i += 3) {
        f.rgb[i + 0] = r;
        f.rgb[i + 1] = g;
        f.rgb[i + 2] = b;
    }
}

void draw_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                   std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    auto& f = fb();
    if (f.width == 0 || f.height == 0) return;

    const int min_x = std::max(0, std::min({x0, x1, x2}));
    const int max_x = std::min(f.width - 1, std::max({x0, x1, x2}));
    const int min_y = std::max(0, std::min({y0, y1, y2}));
    const int max_y = std::min(f.height - 1, std::max({y0, y1, y2}));

    const long long area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) return;   // degenerate

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const long long w0 = edge(x1, y1, x2, y2, x, y);
            const long long w1 = edge(x2, y2, x0, y0, x, y);
            const long long w2 = edge(x0, y0, x1, y1, x, y);
            // Inside if all edge functions share the sign of the area.
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

namespace {
// The heavy lifting lives here, NOT in present(). Keeping present() a thin
// forwarder gives it a small, relocatable prologue so the inline patcher can
// hook it — a real-world tactic: the libc-heavy body (fopen/fprintf) emits
// RIP-relative instructions that can't be relocated into a trampoline, so we
// keep them out of the hooked function's first bytes.
void present_impl(const char* ppm_path) {
    auto& f = fb();
    if (ppm_path == nullptr || f.width == 0 || f.height == 0) return;

    std::FILE* fp = std::fopen(ppm_path, "wb");
    if (!fp) {
        std::printf("    [gpu] present: cannot open %s\n", ppm_path);
        return;
    }
    std::fprintf(fp, "P6\n%d %d\n255\n", f.width, f.height);
    std::fwrite(f.rgb.data(), 1, f.rgb.size(), fp);
    std::fclose(fp);
    std::printf("    [gpu] present -> %s (%dx%d)\n", ppm_path, f.width, f.height);
}
} // namespace

void frame_mark(int frame_index) {
    // Param-only body: no globals, no libc — the prologue stays relocatable
    // so the inline patcher can always hook this. The work is trivial on
    // purpose; the point is to give the enhancer a clean per-frame seam.
    const int next = frame_index + 1;
    (void)next;
}

void present(const char* ppm_path) {
    present_impl(ppm_path);   // thin forwarder, but still libc-heavy: not hooked
}

int current_width() noexcept { return fb().width; }
int current_height() noexcept { return fb().height; }

} // namespace gpu
