# Splice

> Cross-platform C++ function hooking library — **type-safe, fluent, and honest about its limits.**

[![CI](https://github.com/allenk/splice/actions/workflows/ci.yml/badge.svg)](https://github.com/allenk/splice/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![License: MIT](https://img.shields.io/badge/License-MIT-green)
![Platforms](https://img.shields.io/badge/platforms-Android%20%7C%20Linux%20%7C%20Windows-lightgrey)

**Status:** **v1.2.0** · productized from the framework behind an Android game-enhancement system in production. Live-verified on Windows x86_64, Snapdragon 8 Gen 3 and Jetson Orin Nano ARM64, and shipping inside running games on Windows — Direct3D 11 and Direct3D 12 — in [GpuThermalGuard](https://github.com/allenk/GpuThermalGuard)'s in-game overlay.

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
    audit_log("set_user", name);              // observe inputs, original runs as normal
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
splice::install_all();              // every hook queued above, in one shot
splice::install_all(splice::live);  // ...as ONE coordinated transaction (below)
```

Every signature above is **deduced and checked by the compiler** — pass the
wrong argument type to `orig`, or mismatch the callback, and it fails to
build. The C-based libraries make you hand-write the signature and cast
through `void*`, where the same mistake is silent runtime corruption.

## Direct3D 11 and Direct3D 12 — hooking a COM interface

This is the case Splice 1.2.0 was built around, because it is where a hook is
hardest to install: the render thread of a game that is **already running** may
be executing the very bytes being replaced.

A COM interface is a pointer to a **function table** — a vtable. Every
`IDXGISwapChain*` begins with a pointer to the table shared by its concrete
class, and `Present` is one slot in it. So there are two different things you
can patch, and `<splice/live.h>` names both rather than choosing for you.

### Name the methods, not the offsets

```cpp
#include <splice/live.h>

// Direct3D 11: the present path is one method.
splice::Outcome overlay = splice::Vtable<IDXGISwapChain>{swap_chain}
    .on(&IDXGISwapChain::Present,
        [](auto orig, IDXGISwapChain* self, UINT sync, UINT flags) {
            draw_overlay(self);
            return orig(self, sync, flags);     // `orig` is typed, not a void*
        })
    .install(splice::live);

if (!overlay.ok()) { /* overlay.state() and overlay.failure() say what happened */ }
```

The pointer-to-member deduces the target as
`HRESULT(IDXGISwapChain*, UINT, UINT)`, and the callback is checked against it:
two-argument `.on()` is invoke-shaped, so it receives the original first and
then the method's own arguments, `this` among them explicitly. Get one wrong and
it is a build error — on a COM method, where the alternative is hand-writing
`STDMETHODCALLTYPE` signatures and casting slot pointers through `void*`.

`.before`, `.after` and the gates are all available per method; `.on(pmf)` with
no callback starts the chain for them.

Slot resolution is arithmetic on the Itanium ABI, and decoding the dispatch
thunk under MSVC. It **refuses rather than guesses** for a non-virtual member,
an inheritance shape needing `this` adjusted, an unrecognised thunk, or an
architecture it has not been taught. When it refuses,
`.on_slot(8, &I::Present, cb)` names the index by hand with the signature still
checked.

### Direct3D 12 needs more than the present call

An overlay on D3D12 has to follow the swap chain through mode changes, so it is
a group of sites rather than one — and they must go in together or not at all:

```cpp
// Each callback is invoke-shaped: (auto orig, Self*, <the method's args>...).
splice::Outcome overlay = splice::Vtable<IDXGISwapChain3>{chain}
    .on(&IDXGISwapChain3::Present,            on_present)
    .on(&IDXGISwapChain3::Present1,           on_present1)
    .on(&IDXGISwapChain3::ResizeBuffers,      on_resize)
    .on(&IDXGISwapChain3::ResizeBuffers1,     on_resize1)
    .on(&IDXGISwapChain3::SetFullscreenState, on_fullscreen)
    .on(&IDXGISwapChain3::SetColorSpace1,     on_color_space)
    .non_reentrant()                     // one scope for the whole group
    .install(splice::live);              // every site, or none of them
```

A failure part-way restores the slots already written **before any thread
resumes** and reports `State::recovered` with nothing published;
`State::refused` means nothing was written at all. That is what a group buys
over six separate hooks: a failure at the fifth cannot leave four installed
with no record of them anywhere.

`.non_reentrant()` is there because these methods call each other — `Present1`
may reach `Present` internally. Within one scope, a nested call that arrives at
a hook of that scope goes **straight to the original** on that thread, so the
overlay does not draw twice and does not recurse. Nested calls do not consult
`.when()` and do not spend a `.once()` budget.

### When you have addresses instead of an object

A D3D12 overlay usually also wants `ID3D12CommandQueue::Signal`, which is a
different interface — and one `Vtable` object names one table. A group is the
general noun; `Vtable` is one way to fill it:

```cpp
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using SignalFn  = HRESULT(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);

splice::Live<> group;
group.at(present_addr, PresentFn{})
     .expect_prologue(present_bytes)        // refuse unless these are still the bytes
     .replace_with(&OnPresent, g_original_present);
group.at(signal_addr, SignalFn{})
     .expect_prologue(signal_bytes)
     .replace_with(&OnSignal, g_original_signal);

splice::Outcome overlay = group.install(splice::live);
```

`.at()` takes the signature as a tag type, so the callback is still
compile-checked. This is also the shape to use when the addresses were resolved
somewhere else — out of process, or from a runtime the target loaded rather than
the one you did.

`.replace_with(replacement, original)` is the action verb for a caller that
already **has** its own hook function. `.onInvoke()` asks Splice to build one: a
stub is claimed, the callback goes into the registry, and the original arrives
as the first argument. A render-thread hook that is already a plain function,
and that reads what it displaced out of its own atomic with acquire because that
is the only load it will do per frame, wants neither — wrapping it adds an
indirection to the hottest path in the process and buys nothing. Gates are
**refused** with this verb rather than ignored: they are evaluated by Splice's
wrapper, and here your function is in the call path instead, so a `.once()`
would never run.

### The two tiers, and how to choose

```cpp
.install(splice::slot_swap)   // store a pointer into the vtable slot
.install(splice::live)        // inline-patch the function the slot points at
```

| | `splice::slot_swap` | `splice::live` |
|---|---|---|
| What it writes | one pointer-sized atomic store into the slot | the function's prologue |
| Thread coordination | none needed | every thread Splice could enumerate, suspended once |
| Reaches | callers going through **that table** | every table pointing at the function, and every direct caller |
| Another overlay swaps the same slot | it displaces you, or you displace it | you survive — existing slot pointers stay valid and the call still arrives |
| Named by a plain address | refused — there is no slot to swap | supported |

**Neither tier is scoped to an object**, although `Vtable` takes one. A concrete
class has one vtable shared by every instance, so a slot swap made through one
object is seen by all of them — measured on two swap chains created from one
device, which share a table pointer. The coordinated tier is wider again. If one
object is what you meant, compare identity in the callback.

### What `live` does to a thread already inside the function

It is continued, not stepped over. Its program counter is mapped through that
site's instruction-boundary map to the relocated copy of the instruction it was
at. A program counter that is **not** at a boundary the decoder recognises is
refused and nothing is written. Both halves are driven deterministically in the
test suite — park a thread, set its program counter to each offset in turn,
install, and read back what the thread computes, which says *where* it resumed
rather than merely that it survived.

Two refusals worth knowing about, both found by a real consumer:

- **A target somebody else already patched.** `.expect_prologue(bytes)` refuses
  unless the function still begins with the bytes you read. It is the only check
  that notices this, because the coordinated tier patches the *function* and not
  the table — so a vtable slot can be pristine while the code behind it is not.
  A site that does not ask is not checked.
- **A second install on the same function is refused, not chained.** The group
  registers what it patched, so the next unwrap stops at Splice's own jump
  instead of walking into the relay behind it.

To decide *before* installing, `splice_prologue_profile` on the C ABI reports
what a coordinated install would replace at a target: the patch window, the
length of the instruction at the entry, and how many instruction starts lie
strictly inside the window and where they are. Zero interior boundaries means no
thread can be *inside* the bytes being replaced — it is either at the entry or
past all of them. It decodes and writes nothing. These are facts rather than a
verdict, deliberately: an overlay's admission rule is the overlay's, and the
shipping consumer's is stricter than this library requires.

### The limit, stated rather than implied

`splice::live` means Splice coordinated the threads it **could enumerate**. A
thread created after that enumeration is neither covered nor detected, and that
gap has no detector. It is not a stop-the-world — that remains a non-goal, and
reaching one needs a thread entry-exclusion protocol or privileges this library
does not assume. The tier is named `live` and not `safe` for exactly that
reason.

`splice::live_tier_available()` reports whether it can run on a build at all: it
needs an architecture whose prologue can be planned and an OS that can suspend a
thread, which today means x86_64 on Windows. `slot_swap` has no such
requirement.

## New in 1.2.0

**Grouped, coordinated installation** — `splice::Vtable`, `splice::Live`,
`splice::install_all(splice::live)`. One transaction: one thread enumeration and
one suspension for the whole set, all-or-nothing across it, with `State` and
`Stage` saying where an attempt stopped. `lost` is the one outcome a caller has
to decide something about — the threads are still suspended.

Why one transaction and not N: a thread enumeration costs about **118 ms**,
because the OS walks every thread on the machine rather than this process's.
Seven sites installed separately pay that seven times. Measured end-to-end in
the consuming overlay's own startup log, on the stage that contains the install:

| | samples | install stage (ms) |
|---|---|---|
| per-site installs (previous release) | 16 | 218 … 922, 1047, 1250, 1516, 1984, 2406 |
| one coordinated transaction (1.2.0) | 3 | **125, 188, 219** |

Three samples against sixteen, different games and targets, and the older
samples span several branch states — so this is a mechanism confirmed, not a
controlled A/B.

**The lock-free registry is now the default.** Measured on one commit with the
registry as the only difference, Release, callback dispatch in ns/call, with an
unhooked call as an in-run control:

| | unhooked | hooked, lock-free | hooked, locked |
|---|---|---|---|
| Ryzen 9 9950X3D | 2.37 | **4.3** | 23.3 |
| Snapdragon 8 Gen 3 / Android 14 | 2.73 | **4.6** | 71.9–75.5 |
| Jetson Orin Nano (Cortex-A78AE, 15 W) | 3.32 | **11.95** | 100.8 |

Contended, per thread: 9950X3D 8 threads **19** against about 1400; Snapdragon
8 threads **7.9** against 1800; Jetson 6 threads **11.97** against 2995 — where
the lock-free path's 6-thread cost equals its 1-thread cost, because an
acquire-load on a line nobody writes generates no coherence traffic. Both
long-standing performance gates (< 20 ns single-thread, < 5× 8t/1t) are met for
the first time. `shared_mutex_map` remains supported for an unbounded registry
or immediate writer visibility.

**Also in this release:**

- `.non_reentrant()` / `.non_reentrant(scope)` as a gate beside `.when()`,
  `.once()` and `.times(n)`, with a `splice::Reentrancy` scope shareable by
  sibling hooks.
- `.replace_with(replacement, original)`, `.expect(address)`,
  `.expect_prologue(bytes)`, `.observe()` / `invocations()`.
- On the C ABI: `splice_live_prepare` / `_commit` / `_release`,
  `splice_resolve_symbol`, `splice_prologue_profile`, `splice_vtable_swap`,
  `splice_vtable_slot_of_pmf`, and `SPLICE_PATCH_STRATEGY_DATA_SLOT` — a vtable
  slot's page may be read-only, writable or on the heap, so that path records
  the protection it found and puts that one back.
- `SPLICE_REGISTRY_IMPL` and `SPLICE_MAX_HOOKS` are real CMake cache options,
  validated at configure time and applied PUBLIC so they travel with
  `splice::splice` and through `install(EXPORT)`. A build whose translation
  units disagree about either is detected and refused instead of reading the
  shared context at the wrong offsets.
- Exceeding the registry capacity is a **refused install** with the reason
  logged. It used to end the process with `abort()` on one side and return
  silently on the other. There is no `abort()` left anywhere in the library: it
  is loaded into somebody else's program, where that is a dead host — and on
  Windows a modal runtime dialog that freezes the host and waits for a human.
- An address that names nothing is refused instead of faulting;
  `splice::os::is_readable` is consulted at every hop of a thunk walk.

Full detail, including what was corrected rather than added:
[CHANGELOG](CHANGELOG.md#120---2026-10-13).

## Used by

**[GpuThermalGuard](https://github.com/allenk/GpuThermalGuard)** — a power-limit
guard for NVIDIA GPUs with an in-game overlay. Its overlay DLL uses Splice to
hook the Direct3D 11 and Direct3D 12 present paths inside games that are already
running: **one site** on D3D11, and **seven** on D3D12 — the swap chain's
present, mode-change and colour-space methods plus one on the command queue —
installed as a single coordinated transaction with a sixteen-byte prologue
expectation per site.

Using Splice in a project? Open an issue or a PR to be listed here.

## Design values

1. **Type safety first** — signatures deduced via `decltype`, no `void*` in user code.
2. **Fluent chainable API** — reads top-to-bottom, verb-first, composable gates.
3. **Zero-friction for the 90% case** — one-arg macros; escape hatches for the rest.
4. **Platform-honest** — GOT/PLT on ELF, IAT on PE, vtable slots on COM, atomic inline patching everywhere.
5. **Hot-path aware logging** — C macro substrate (not spdlog/fmt), `_ONCE` / `_EVERY_N` variants inside callbacks.
6. **Refuse rather than guess** — an unresolvable slot, an unrecognised prologue, a program counter at no boundary, a capacity exceeded: each is a named refusal that writes nothing, not a best effort.
7. **Honest about limits** — true reversible uninstall is out of scope, by a missing protocol rather than by a law: reclaiming the trampoline needs proof that nothing can reach it again, and nothing here establishes it. Tiered `disable()` instead (Tier 1 reversible; Tier 2 atomic restore, trampoline leaks), said plainly — see [`docs/splice-guide.md`](docs/splice-guide.md) §9.

## How Splice compares

Spec-level review vs the field (full doc: [`docs/splice-vs-hooking-libraries.md`](docs/splice-vs-hooking-libraries.md)):

| | **Splice** | ShadowHook | Detours | PolyHook2 | SubHook | rcmp |
|---|---|---|---|---|---|---|
| OS | Android/Linux/Windows | Android | Windows | Windows | Win/Linux/macOS | Win/Linux |
| Arch | ARM64 + x86_64 | arm32/arm64 | x86…ARM64 | x86/x64 | x86 | x86/x64 |
| Type-safe `decltype` API | ✅ | ✖ (C) | ✖ (C) | partial | ✖ (C) | ✅ |
| Fluent DSL / RAII | ✅ | ✖ | ✖ | ✖ | ✖ | ✖ |
| Import-table + inline | ✅ | inline | ✅ | ✅ | inline | inline |
| Grouped all-or-nothing install | ✅ | ✖ | ✖ | ✖ | ✖ | ✖ |
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
| Linux ARM64 | ✅ Supported | GOT/PLT + inline (verified on Jetson Orin Nano) |
| Linux x86_64 | ✅ Supported | GOT/PLT + inline |
| Windows x86_64 | ✅ Production | IAT + inline + vtable slot; coordinated `splice::live` tier |
| Android ARM32 | ⏸ Optional | later (FR-006) |

The coordinated `live` tier needs an OS that can suspend a thread and an
architecture whose prologue can be planned — today x86_64 on Windows. Ask
`splice::live_tier_available()` rather than assuming. `slot_swap` and every
single-site path run everywhere above.

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

## Examples — and the hooking paradigms

The [`examples/`](examples/) directory is the fastest way in:

| Example | Shows |
|---|---|
| [`hello_hook`](examples/hello_hook.cpp) | the whole API in 40 lines |
| [`malloc_tracker`](examples/malloc_tracker.cpp) | hook a libc function, allocation-free accounting |
| [`member_function`](examples/member_function.cpp) | `SPLICE_HOOK_MEMBER` on a C++ method |
| [`gpu_app/`](examples/gpu_app/) | **Paradigm A** — patch GLES-style commands directly to upscale a render |
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

**Paradigm B — hook the dispatcher, rewrite its RETURN VALUE** (Vulkan, plugin `dlsym`):

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

**Paradigm C — the interface is already a table of pointers** (COM, Direct3D):
swap the slot, or patch the function behind it. That is what `<splice/live.h>`
exists for — see [the Direct3D section](#direct3d-11-and-direct3d-12--hooking-a-com-interface).

```bash
cmake --build --preset=windows-x64-dev --target splice_gpu_demo splice_vulkan_demo
./out/build/windows-x64-dev/examples/gpu_app/gpu_demo
./out/build/windows-x64-dev/examples/vulkan_app/vulkan_demo
```

Paradigms A and B are explained in [`docs/splice-guide.md`](docs/splice-guide.md) §11.

Full capability guide: [`docs/splice-guide.md`](docs/splice-guide.md) (Traditional Chinese translation: [`docs/splice-guide-zh.md`](docs/splice-guide-zh.md)).

## Structure

```
include/splice/      Public API (header-only)
  splice.h           Umbrella header + version constants
  macros.h           SPLICE_HOOK* / _MEMBER / _AS family
  core.h             InterceptorEntry<> + fluent modifiers + ScopedHook
  live.h             splice::Vtable / Live / Outcome, slot_swap + live tiers
  gates.h            .when / .once / .times / .non_reentrant, shared by site and group
  reentrancy.h       splice::Reentrancy scopes
  context.h          HookContext, HookStorage, HookRegistry (policy/registry)
  policy.h           rcu_writeonce / shared_mutex concurrency policies
  registry.h         hook + originals registry surface
  registry_impl.h    SPLICE_REGISTRY_IMPL (rcu_atomic_array default / shared_mutex_map)
  trampoline.h       per-call-site trampoline generator
  traits.h           function_traits + member_function_traits
  diagnostics.h      SPLICE_TRACE / _COUNT / _TIME sugar
  log.h              SPLICE_LOG* + hot-path variants
  engine.h           C ABI to the compiled engine
  detail/platform_log.h   Vendored C logging substrate
src/                 Compiled library
  engine/            engine dispatcher + GOT/IAT patchers + live transaction
  arch/{arm64,x86_64}/  disasm + inline patcher + atomic install
  os/{posix,win32}/  memory + module enumeration + thread suspension
tests/               GTest (326 cases)
benchmark/           Google Benchmark microbenches
examples/            hello_hook, malloc_tracker, member_function, gpu_app, vulkan_app
docs/                splice-guide.md (canonical) + comparisons + performance reports
cmake/               spliceConfig.cmake.in (find_package support)
```

## Roadmap

**v1.0.0 — shipped:** Phases 0–4.5 + Fluent API v2. Highlights:

- Cross-platform engine (ARM64 + x86_64; Android/Linux/Windows), GOT/PLT + IAT + atomic inline patching.
- Performance pass — 41 → 22 ns/call single-thread; opt-in RCU registry for many-thread readers.
- Fluent API v2 — `.before`/`.after`/`.when`/`.once`/`.times`, `SPLICE_HOOK_MEMBER`, diagnostics, `ScopedHook`.
- Tiered `disable()`, `find_package(splice)` install/export.

**v1.1.0 — shipped (2026-10-06):** live installation into running processes. Highlights:

- Near trampolines + relay: atomic 5-byte x86_64 installs regardless of where the hook lives.
- Strict exact-site installation; internal prepared patches and an enlisted-thread transaction (Windows x64).
- ARM64 near allocation so `ADRP`/`B` prologues relocate; `.observe()` / `invocations()`.
- Fail-closed fixes: refused stacking instead of recursion, Tier 1 made truly atomic, `splice_disable` restored on MSVC prologues.

**v1.2.0 — shipped (2026-10-13):** the grouped, coordinated install, and the
registry that pays for it. Highlights:

- `splice::Vtable` / `splice::Live` / `install_all(splice::live)` — one
  transaction, one thread enumeration, all-or-nothing, with named outcomes.
- COM / vtable hooking as a first-class path; Direct3D 11 and Direct3D 12 in
  production.
- `.non_reentrant()`, `.replace_with()`, `.expect_prologue()`,
  `splice_prologue_profile`.
- Lock-free registry by default: 4.3 ns/call on x86_64, 4.6 on Snapdragon 8 Gen 3.
- No `abort()` anywhere in the library; capacity and configuration mismatches
  are refusals.

**Next (optional):** chaining onto patches installed by others; the `live` tier
beyond Windows x86_64; `ADR` materialisation on ARM64; Android ARM32 (FR-006);
macOS is research-only (see [`docs/macos-port-notes.md`](docs/macos-port-notes.md)).

## License

MIT — see [LICENSE](LICENSE).
