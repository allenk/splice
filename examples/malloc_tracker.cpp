// ─── Splice example: malloc-tracker ───────────────────────────────────────
//
// Counts and sizes every malloc() call using SPLICE_COUNT-style diagnostics
// plus a custom .before() that accumulates bytes. Demonstrates hooking a
// libc function by symbol and the diagnostic sugar.
//
// Caveat: hooking the process-wide malloc is genuinely invasive — the hook
// callback itself must not allocate (it would re-enter). We keep the
// callback allocation-free (atomic adds only).
//
// Build:
//   cmake --build ... --target splice_example_malloc_tracker
// ───────────────────────────────────────────────────────────────────────────
#include <splice/splice.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {
std::atomic<std::uint64_t> g_calls{0};
std::atomic<std::uint64_t> g_bytes{0};
}

int main() {
    // Hook malloc by symbol. On ELF this takes the GOT/PLT (Tier 1) path;
    // on PE the IAT path. Either way the callback is allocation-free.
    //
    // Note: we hook the address of malloc directly here for portability —
    // SPLICE_HOOK_LIB("libc.so", malloc) also works on ELF where the symbol
    // is import-resolved.
    SPLICE_HOOK_ADDR(&malloc)
        .before([](size_t n) {
            g_calls.fetch_add(1, std::memory_order_relaxed);
            g_bytes.fetch_add(n, std::memory_order_relaxed);
        });

    splice::install_all();

    if (!splice_is_hooked(reinterpret_cast<void*>(&malloc))) {
        std::printf("(malloc not patchable on this build — stub backend)\n");
        return 0;
    }

    // Drive some allocations.
    for (int i = 0; i < 1000; ++i) {
        void* p = std::malloc(64 + i);
        std::free(p);
    }

    std::printf("malloc calls observed: %llu\n",
                static_cast<unsigned long long>(g_calls.load()));
    std::printf("total bytes requested: %llu\n",
                static_cast<unsigned long long>(g_bytes.load()));
    return 0;
}
