// ─── Splice example: hello-hook ───────────────────────────────────────────
//
// The 5-minute "does it work" example. Hooks a plain free function, proves
// the callback fires and that calling `orig` still reaches the real body.
//
// Build (from a configured tree):
//   cmake --build --preset=windows-x64-dev --target splice_example_hello_hook
//   ./out/build/windows-x64-dev/examples/splice_example_hello_hook
//
// Expected output:
//   add(2, 3) before hook = 5
//   [hook] add(2, 3) intercepted
//   add(2, 3) after hook  = 105
// ───────────────────────────────────────────────────────────────────────────
#include <splice/splice.h>

#include <cstdio>

// A noinline target so the compiler leaves a real prologue to patch.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
int add(int a, int b) {
    return a + b;
}

int main() {
    std::printf("add(2, 3) before hook = %d\n", add(2, 3));

    // Install a hook: log the call, then return the original result + 100.
    SPLICE_HOOK_ADDR(&add)
        .onInvoke([](auto orig, int a, int b) {
            std::printf("[hook] add(%d, %d) intercepted\n", a, b);
            return orig(a, b) + 100;
        });

    splice::install_all();

    if (!splice_is_hooked(reinterpret_cast<void*>(&add))) {
        std::printf("(no live patcher on this platform — built as a stub)\n");
        return 0;
    }

    std::printf("add(2, 3) after hook  = %d\n", add(2, 3));
    return 0;
}
