# Splice

> Cross-platform C++ function hooking library — **type-safe, fluent, and honest about its limits.**

[![CI](https://github.com/allenk/splice/actions/workflows/ci.yml/badge.svg)](https://github.com/allenk/splice/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![License: MIT](https://img.shields.io/badge/License-MIT-green)
![Platforms](https://img.shields.io/badge/platforms-Android%20%7C%20Linux%20%7C%20Windows-lightgrey)

**Status:** **v1.0.0** · productized from `the predecessor framework`, the framework behind a production Android game enhancer. Live-verified on Windows x86_64 and Snapdragon 8 Gen 3 ARM64.

---

## Why Splice — modern C++ that checks your hooks at compile time

Every C-based hooking library makes you **re-declare the target's signature by
hand** and cast through `void*`. Get it wrong and you get silent
calling-convention / stack corruption — the worst class of bug in this domain.

Splice makes the **compiler** enforce it:

```cpp
#include <splice/splice.h>

// decltype(&eglSwapBuffers) is deduced — your use of `orig` and the args
// must match, or it fails to COMPILE. No void*, no hand-written signatures.
SPLICE_HOOK_LIB("libEGL.so", eglSwapBuffers)
    .after([](EGLBoolean /*ret*/, EGLDisplay d, EGLSurface s) { ++g_frames; });

SPLICE_HOOK_ADDR(&add)
    .when([]{ return g_enabled.load(); })   // composable runtime gate
    .onInvoke([](auto orig, int a, int b) { return orig(a, b) + 100; });

// One-liner diagnostics
SPLICE_TRACE(glDrawArrays);   SPLICE_COUNT(malloc);   SPLICE_TIME(glTexImage2D);

splice::install_all();
```

That fluent, type-deduced surface is Splice's edge. No C library (ShadowHook,
Detours, SubHook) can offer it; the only other modern-C++ option (rcmp) is
x86-only and can't unhook. See [the comparison](#how-splice-compares).

## The fluent API, by example

**Pick a target — three ways:**

```cpp
SPLICE_HOOK(some_func);                       // linker-visible symbol (compile-time &)
SPLICE_HOOK_LIB("libEGL.so", eglSwapBuffers); // resolve by name at install time (dlsym/IAT)
SPLICE_HOOK_ADDR(func_ptr);                   // any address — vtable slot, RVA, JIT
```

**Choose what your hook does — three mutually-exclusive verbs:**

```cpp
// .onInvoke — full control. You get `orig`; call it (or don't), rewrite args/return.
SPLICE_HOOK(add).onInvoke([](auto orig, int a, int b) {
    return orig(a, b) + 100;                  // run original, then tweak the result
});

// .before — fire BEFORE the original. No `orig`, no return plumbing.
SPLICE_HOOK(set_user).before([](const char* name) {
    audit_log("set_user", name);             // observe inputs, original runs as normal
});

// .after — fire AFTER the original. Receives the return value + args.
SPLICE_HOOK(read_bytes).after([](int n_read, void* buf, int cap) {
    metrics.add(n_read);                      // observe the result
});
// (.after on a void function takes just the args: .after([](Args...){...}))
```

**Gate when it fires — compose freely with any verb:**

```cpp
SPLICE_HOOK(glDrawArrays)
    .when([]{ return g_capture.load(); })     // only while capture is on (else original, zero cost)
    .times(120)                               // ...for the first 120 calls
    .before([](GLenum, GLint, GLsizei n) { g_verts += n; });

SPLICE_HOOK(engine_init)
    .once()                                   // fire exactly once, then transparent
    .after([]{ SPLICE_LOGI("engine up"); });
// .when().once() == .once().when() — order doesn't matter.
```

**Hook a C++ member function — `this` becomes the first explicit arg:**

```cpp
SPLICE_HOOK_MEMBER(Widget::draw)              // non-virtual; deduces Ret(Widget*, Args...)
    .onInvoke([](auto orig, Widget* self, int layer) {
        return orig(self, layer + 1);
    });
```

**One-liner diagnostics — no lambda needed:**

```cpp
SPLICE_TRACE(eglSwapBuffers);  // log each call     SPLICE_COUNT(malloc);  // call counter
SPLICE_TIME(glTexImage2D);     // avg/min/max timing
```

**Scope-bound auto-disable (RAII) — for tests, plugins, temporary sessions:**

```cpp
{
    splice::ScopedHook h = SPLICE_HOOK(target).onInvoke(my_cb);
    run_under_hook();
}                                             // h leaves scope → hook disabled
```

**Commit:**

```cpp
splice::install_all();   // installs every hook queued above, in one shot
```

Every signature above is **deduced and checked by the compiler** — pass the
wrong argument type to `orig`, or mismatch the callback, and it fails to
build. The C-based libraries make you hand-write the signature and cast
through `void*`, where the same mistake is silent runtime corruption.

## Design values

1. **Type safety first** — signatures deduced via `decltype`, no `void*` in user code.
2. **Fluent chainable API** — reads top-to-bottom, verb-first, composable gates.
3. **Zero-friction for the 90% case** — one-arg macros; escape hatches for the rest.
4. **Platform-honest** — GOT/PLT on ELF, IAT on PE, atomic inline patching everywhere.
5. **Hot-path aware logging** — C macro substrate (not spdlog/fmt), `_ONCE` / `_EVERY_N` variants inside callbacks.
6. **Honest about limits** — true reversible uninstall is impossible for in-process non-privileged inline hooks. Splice offers tiered `disable()` (Tier 1 reversible; Tier 2 atomic restore, trampoline leaks) and says so plainly — see [`docs/splice-guide.md`](docs/splice-guide.md) §9.

## How Splice compares

Spec-level review vs the field (full doc: [`docs/splice-vs-hooking-libraries.md`](docs/splice-vs-hooking-libraries.md)):

| | **Splice** | ShadowHook | Detours | PolyHook2 | SubHook | rcmp |
|---|---|---|---|---|---|---|
| OS | Android/Linux/Windows | Android | Windows | Windows | Win/Linux/macOS | Win/Linux |
| Arch | ARM64 + x86_64 | arm32/arm64 | x86…ARM64 | x86/x64 | x86 | x86/x64 |
| Type-safe `decltype` API | ✅ | ✖ (C) | ✖ (C) | partial | ✖ (C) | ✅ |
| Fluent DSL / RAII | ✅ | ✖ | ✖ | ✖ | ✖ | ✖ |
| Import-table + inline | ✅ | inline | ✅ | ✅ | inline | inline |
| License | MIT | MIT | MIT | MIT | BSD-2 | MIT |

**The niche:** the only library here that is *both* modern-C++ (the compiler
checks your hook) *and* spans ARM64 + x86_64 across Android + Linux + Windows.
Each rival is a single-OS or single-arch specialist. Rivals lead where they
should: ShadowHook on Android battle-testing + unhook, Detours on Windows
true-uninstall + injection, PolyHook2 on hook-type variety.

## Platform matrix

| Platform | Status | Install strategies |
|---|---|---|
| Android ARM64 | ✅ Production | inline patch (live-verified, Snapdragon 8 Gen 3) |
| Linux ARM64 | ✅ Supported | GOT/PLT + inline |
| Linux x86_64 | ✅ Supported | GOT/PLT + inline |
| Windows x86_64 | ✅ Supported | IAT + inline |
| Android ARM32 | ⏸ Optional | v1.1 (FR-006) |

CI builds + tests Windows x64, Linux x64 (+ASan), Linux ARM64, and
cross-compiles Android ARM64 on every push. On-device ARM64 verification is a
manual step on real hardware. See [`docs/splice-guide.md`](docs/splice-guide.md) §5 / §9.

## Quick start

### Windows (VS2022 + Ninja + VCPKG)

```powershell
Import-Module "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "C:\Program Files\Microsoft Visual Studio\2022\Community" -DevCmdArguments "-arch=x64 -host_arch=x64"
$env:VCPKG_ROOT = "C:\vcpkg"
cmake --preset=windows-x64-dev
cmake --build --preset=windows-x64-dev
ctest --preset=windows-x64-dev --output-on-failure
```

### Linux (Ninja + VCPKG)

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset=linux-x64-dev
cmake --build --preset=linux-x64-dev
ctest --preset=linux-x64-dev --output-on-failure
```

### Use it from your project

```cmake
find_package(splice CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE splice::splice)
```

Or zero-install via FetchContent / vendoring — `add_subdirectory(splice)` and
the `splice::splice` alias work either way.

## Examples — and the two hooking paradigms

The [`examples/`](examples/) directory is the fastest way in:

| Example | Shows |
|---|---|
| [`hello_hook`](examples/hello_hook.cpp) | the whole API in 40 lines |
| [`malloc_tracker`](examples/malloc_tracker.cpp) | hook a libc function, allocation-free accounting |
| [`member_function`](examples/member_function.cpp) | `SPLICE_HOOK_MEMBER` on a C++ method |
| [`gpu_app/`](examples/gpu_app/) | **Paradigm A** — patch GLES-style commands directly to upscale a render (the production game-enhancer pattern) |
| [`vulkan_app/`](examples/vulkan_app/) | **Paradigm B** — hook `vkGetDeviceProcAddr` once and substitute wrapper pointers (the real Vulkan-layer / RenderDoc pattern) |

Both GPU demos run the same unmodified "game" at 160×120, and Splice
transparently upscales it to 640×480 — the GLES one by patching each command,
the Vulkan one by rewriting the dispatch getter's return value:

| Baseline (160×120) | Splice-enhanced (640×480) |
|---|---|
| ![baseline](docs/attachments/gpu_demo_baseline.png) | ![enhanced](docs/attachments/gpu_demo_enhanced.png) |

**Paradigm A — patch the command directly** (GLES / libc / game functions):

```cpp
// gpu_app: rewrite the viewport command's ARGUMENTS to upscale
SPLICE_HOOK_ADDR(&gpu::set_viewport)
    .when([]{ return g_enabled; })
    .onInvoke([](auto orig, int w, int h) { orig(w * 4, h * 4); });
```

**Paradigm B — hook the dispatcher, rewrite its RETURN VALUE** (Vulkan, COM, plugin `dlsym`):

```cpp
// vulkan_app: hook vkGetDeviceProcAddr ONCE; hand the app wrapper pointers.
// This is how RenderDoc / MangoHud / Vulkan layers actually intercept.
SPLICE_HOOK_ADDR(&vk_get_device_proc_addr)
    .onInvoke([](auto orig, VkDevice dev, const char* name) -> PFN_vkVoidFunction {
        auto real = orig(dev, name);
        if (std::strcmp(name, "vkCmdDraw") == 0) {
            g_real_draw = (PFN_CmdDraw)real;             // keep the real pointer
            return (PFN_vkVoidFunction)&wrapped_draw;    // return our wrapper instead
        }
        return real;                                     // pass everything else through
    });
```

One Splice hook on the getter influences every Vulkan command — and because
the commands themselves are never patched, even libc-heavy ones stay hookable.
The `.onInvoke` here rewrites a *function pointer* return; in Paradigm A it
rewrites *arguments*. Same fluent API, two interception strategies.

```bash
cmake --build --preset=windows-x64-dev --target splice_gpu_demo splice_vulkan_demo
./out/build/windows-x64-dev/examples/gpu_app/gpu_demo
./out/build/windows-x64-dev/examples/vulkan_app/vulkan_demo
```

The two paradigms are explained in [`docs/splice-guide.md`](docs/splice-guide.md) §11.

Full capability guide: [`docs/splice-guide.md`](docs/splice-guide.md) ([中文版](docs/splice-guide-zh.md)).

## Structure

```
include/splice/      Public API (header-only)
  splice.h           Umbrella header
  macros.h           SPLICE_HOOK* / _MEMBER / _AS family
  core.h             InterceptorEntry<> + fluent modifiers + ScopedHook
  context.h          HookContext, HookStorage, HookRegistry (policy/registry)
  policy.h           rcu_writeonce / shared_mutex concurrency policies
  registry_impl.h    SPLICE_REGISTRY_IMPL (shared_mutex_map / rcu_atomic_array)
  trampoline.h       per-call-site trampoline generator
  traits.h           function_traits + member_function_traits
  diagnostics.h      SPLICE_TRACE / _COUNT / _TIME sugar
  log.h              SPLICE_LOG* + hot-path variants
  engine.h           C ABI to the compiled engine
  detail/platform_log.h   Vendored C logging substrate
src/                 Compiled library
  engine/            engine dispatcher + GOT/IAT patchers
  arch/{arm64,x86_64}/  disasm + inline patcher + atomic install
  os/{posix,win32}/  memory + module enumeration
tests/               GTest (132 cases)
benchmark/           Google Benchmark microbenches (FR-010)
examples/            hello_hook, malloc_tracker, member_function, gpu_app, vulkan_app
docs/                splice-guide.md (canonical) + comparisons + FR-010 reports
cmake/               spliceConfig.cmake.in (find_package support)
```

## Roadmap

**v1.0.0 — shipped:** Phases 0–4.5 + Fluent API v2. Highlights:

- Cross-platform engine (ARM64 + x86_64; Android/Linux/Windows), GOT/PLT + IAT + atomic inline patching.
- Performance pass — 41 → 22 ns/call single-thread; opt-in RCU registry for many-thread readers.
- Fluent API v2 — `.before`/`.after`/`.when`/`.once`/`.times`, `SPLICE_HOOK_MEMBER`, diagnostics, `ScopedHook`.
- Tiered `disable()`, `find_package(splice)` install/export.

**Post-1.0 (optional):** Android ARM32 (FR-006); macOS is research-only (see [`docs/macos-port-notes.md`](docs/macos-port-notes.md)).

## License

MIT — see [LICENSE](LICENSE).
