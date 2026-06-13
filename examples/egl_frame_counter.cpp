// ─── Splice example: egl-frame-counter ────────────────────────────────────
//
// The canonical game-enhancer pattern: count frames by hooking
// eglSwapBuffers with a .after() observer. This is the real workload Splice
// was productized for (a production game enhancer).
//
// Only builds where EGL headers are present (Android / Linux with Mesa).
// The examples/CMakeLists.txt guards this target behind find_path(EGL/egl.h).
//
// On Android, load into a game process and watch:
//   adb logcat -s splice
// ───────────────────────────────────────────────────────────────────────────
#include <splice/splice.h>

#include <EGL/egl.h>

#include <atomic>

namespace {
std::atomic<std::uint64_t> g_frames{0};
}

// Call this from your loader's initialize() before the first frame.
void install_frame_counter() {
    // .after() — pure observation, no need to touch the return or call orig
    // ourselves. The hook lives for the program lifetime (fire-and-forget),
    // gated to log once every 60 frames to keep the hot path quiet.
    SPLICE_HOOK_LIB("libEGL.so", eglSwapBuffers)
        .after([](EGLBoolean /*result*/, EGLDisplay /*dpy*/, EGLSurface /*surf*/) {
            const auto n = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
            SPLICE_LOGI_EVERY_N(60, "[frame-counter] %llu frames",
                                static_cast<unsigned long long>(n));
        });

    splice::install_all();
    SPLICE_LOGI("[frame-counter] installed (hooked=%d)",
                splice_is_hooked(reinterpret_cast<void*>(&eglSwapBuffers)));
}

// Standalone build needs a main(); in a real loader you'd call
// install_frame_counter() from your library entry point instead.
int main() {
    install_frame_counter();
    return 0;
}
