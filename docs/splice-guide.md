# Splice — Complete Capability Guide

**Audience:** Anyone meeting Splice for the first time — "what is it / what
can it do / what can't it do / how do I use it".
**Position:** This is the **canonical user guide** for Splice. For deeper
dives, see:
- Design trade-offs: [`v2-design-rationale.md`](./v2-design-rationale.md)
- vs the predecessor framework (short): [`splice-vs-predecessor-summary.md`](./splice-vs-predecessor-summary.md)
- Performance journey: [`fr-010-performance-summary.md`](./fr-010-performance-summary.md)
- Engine internals: [`hooking-internals.md`](./hooking-internals.md)
- Implementation roadmap: [`Splice Plan.md`](./Splice%20Plan.md)
- Chinese version: [`splice-guide-zh.md`](./splice-guide-zh.md)

---

## 1. One-line positioning

> **Splice is a type-safe, fluent, cross-platform C++20 function hooking library.**
>
> Born for Android ARM64 game-enhancer work (productized from the predecessor framework),
> extended to Linux ARM64/x86_64 + Windows x64. Goals: reads like a DSL,
> runs at zero perceptible cost, and is **honest** about what it can and
> cannot do.

---

## 2. 30-second demo

```cpp
#include <splice/splice.h>

// Intercept eglSwapBuffers, do work after every frame swap
SPLICE_HOOK_LIB("libEGL.so", eglSwapBuffers)
    .after([](EGLBoolean /*ret*/, EGLDisplay d, EGLSurface s) {
        ++g_frame_count;
    });

// Intercept and rewrite the return value
SPLICE_HOOK_ADDR(&my_func)
    .onInvoke([](auto orig, int x) {
        int r = orig(x);
        return r * 2;
    });

// Intercept only when a condition holds — otherwise the trampoline calls
// the original directly (zero cost gate)
SPLICE_HOOK(glDrawArrays)
    .when([]{ return g_capture_enabled.load(); })
    .before([](GLenum, GLint, GLsizei n) { g_total_verts += n; });

splice::install_all();   // one-shot commit of all queued hooks
```

---

## 3. What it CAN do — capability list

### 3.1 Hook install strategies (engine picks the safest automatically)

| Strategy | When it kicks in | Safety |
|---|---|---|
| **POINTER_SWAP** (GOT/PLT on ELF, IAT on PE) | Functions imported via the dynamic linker | Tier 1: **atomic, fully reversible**, permanent disable |
| **INLINE patch** (jmp rel32 on x86_64, indirect branch on ARM64) | Direct addresses / local functions / vtable slots | Tier 2: **atomic install**, **atomic disable**, trampoline memory leaked forever |

The engine tries POINTER_SWAP first, falls back to INLINE on failure.

### 3.2 Five hook modifier styles (fluent API)

```cpp
// 1. Full replacement — you decide whether to call orig
.onInvoke([](auto orig, Args... args) -> Ret { ... })

// 2. Fire BEFORE original (no orig pointer needed)
.before([](Args... args) { ... })

// 3. Fire AFTER original (receives ret + args)
.after([](Ret ret, Args... args) { ... })

// 4. Predicate gate — when false, trampoline takes the original path
.when([]{ return condition; })

// 5. One-shot / N-shot
.once()
.times(5)
```

`.when` / `.once` / `.times` **compose freely** with `.onInvoke` /
`.before` / `.after`. The three action verbs (`onInvoke` / `before` /
`after`) are **mutually exclusive** — the last one set overwrites the
previous.

### 3.3 Diagnostic one-liners

```cpp
SPLICE_TRACE(eglSwapBuffers);          // log args + return on every call
SPLICE_COUNT(malloc);                  // counter — query via splice::stats<malloc>()
SPLICE_TIME(glTexImage2D);             // accumulated avg/min/max
```

### 3.4 Hook target expression

| Form | Use case |
|---|---|
| `SPLICE_HOOK(funcname)` | Linker-visible symbol, address known at compile time |
| `SPLICE_HOOK_LIB("libfoo.so", funcname)` | Resolved via dlopen / LoadLibrary at install time |
| `SPLICE_HOOK_ADDR(&funcname)` | Pass a function pointer directly (vtable slots, RVAs, JIT addrs) |

Each form has a `_STATIC` variant (entry persists for program lifetime —
the typical "installAll" pattern) and a `_AS` variant (per-call-site
policy override, see §6.2).

### 3.5 Disable and lifecycle

| Operation | Behaviour |
|---|---|
| `entry.disable()` | Tier 1: atomic pointer restore; Tier 2: atomic prologue-bytes restore |
| `entry.is_installed()` | Query whether the hook is still active |
| **`splice::ScopedHook`** | RAII — automatic disable when the object destructs |
| `splice::install_all()` | Run every queued installer (each `SPLICE_HOOK_STATIC` queues one) |

**Important: true uninstall + memory reclaim is NOT supported** — this is
**architecturally impossible** for in-process non-privileged hooks (would
require knowing no thread is currently executing inside the trampoline,
which needs ptrace or root). Splice offers disable: behaviour is restored,
the callback is preserved (re-installable), trampoline memory leaks. See
§9 for the limitations chapter.

### 3.6 Cross-signature, cross-arity

- Any `Ret(Args...)` function can be hooked, **arity is unbounded** (variadic templates)
- `void` returns, C linkage, calling conventions are auto-deduced
- Lambdas can capture (`[this]`, `[&state]`, by-value — all fine)
- `decltype(&func)` compile-time deduction — wrong signature fails at **compile time**, no runtime surprise

### 3.7 Concurrency model — policy framework

Each hook's callback storage is chosen by a policy tag:

| Policy | Reader cost | Writer behaviour | Use when |
|---|---|---|---|
| `splice::policy::rcu_writeonce` (**default**) | 1 atomic acquire-load | Write-once after install; old callback **intentionally leaked** (hook lives for program lifetime) | 99% of cases |
| `splice::policy::shared_mutex` | reader-counter RMW | Writer takes unique_lock; callbacks can be swapped at runtime | Callback genuinely changes at runtime |
| `splice::policy::rcu_refcounted` (v1.1) | atomic<shared_ptr> load | Swap with refcounted reclaim | Zero-leak dynamic swap (C++20) |

Switching:
```cpp
// Per-call-site override (FR-010 Step 4)
SPLICE_HOOK_AS(splice::policy::shared_mutex, my_func)
    .onInvoke(swappable_callback);
```

### 3.8 Registry concurrency model — registry framework

The entire HookContext lookup table is also pluggable:

| Impl | Reader cost | 8t/1t scaling | Writer |
|---|---|---|---|
| `splice::registry::shared_mutex_map` (**default**) | shared_lock + map find | ~50× | unique_lock |
| `splice::registry::rcu_atomic_array` | pure atomic load + array index | **~4×** ✅ | copy snapshot + atomic publish + 100ms deferred reclaim |

Switch at build time:
```bash
cmake -DSPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array
```

---

## 4. What it CAN'T do — honest non-goals

### 4.1 Things we don't do

| Feature | Why not |
|---|---|
| True uninstall + memory reclaim | Architecturally impossible in-process non-priv — ShadowHook and LSPlant can't either |
| `uninstallAll()` | Same reason; replaced by `disable_all()` |
| Windows VEH hooking | Not needed; SEH path is sufficient |
| Kernel-mode hooking | Out of scope |
| Stop-the-world thread suspend | Needs ptrace / root |
| Hook priority ordering | Install order = execution order; no separate priority API |
| Python / Rust bindings | Not for v1.0 |
| Unity / Unreal engine integration layer | Out of scope |

### 4.2 Design trade-offs you must know

| Trade-off | Meaning |
|---|---|
| **`disable()` ≠ uninstall** | Tier 2 inline disable restores the prologue but trampoline JIT memory leaks (forever, bounded by hook count) |
| **rcu_writeonce is write-once-leak** | Every `set_invoke` allocates a fresh `std::function`; old ones are not reclaimed. Splice assumes callbacks are set once and stay |
| **RCU registry is capped at SPLICE_MAX_HOOKS** | Default 512; hard limit. Rebuild to raise |
| **Recursive hook callbacks deadlock** | Calling the same hooked function inside its callback infinite-loops (use `.when(false)` to gate yourself out) |
| **`__COUNTER__` ID system** | Fixed to `__LINE__ << 16 \| __COUNTER__`; in theory two TUs on the same line with the same counter would still collide (probability << 10⁻⁶) |
| **No guarantee on callback ordering** | Multiple hooks on the same target (which you should not do) have undefined behaviour |

---

## 5. Platform support matrix

| Platform | Arch | Status | Install strategies | Notes |
|---|---|---|---|---|
| Android | ARM64 | ✅ Production | INLINE (no GOT/PLT for symbol-resolved path) | Verified on Snapdragon 8 Gen 3 |
| Linux | ARM64 | ✅ | GOT/PLT + INLINE | CI |
| Linux | x86_64 | ✅ | GOT/PLT + INLINE | CI |
| Windows | x64 | ✅ | IAT + INLINE | Live |
| Android | ARM32 | ⏸ Optional v1.1 | — | FR-006 |
| macOS | — | ❌ | — | Out of scope |
| iOS | — | ❌ | — | Out of scope |

**Sanitizer coverage:** ASan / UBSan (CI), TSan (Linux Phase 4.5).

---

## 6. Detailed API reference

### 6.1 Public macros (user-facing)

#### Hook installation
| Macro | Signature | Behaviour |
|---|---|---|
| `SPLICE_HOOK(func)` | linker-visible function | `thread_local static` entry, type auto-deduced |
| `SPLICE_HOOK(lib, func)` | dlopen/LoadLibrary symbol | resolved at install time |
| `SPLICE_HOOK_LIB(lib, func)` | same | explicit two-arg form |
| `SPLICE_HOOK_ADDR(func_ptr)` | arbitrary address | patches that address directly |
| `SPLICE_HOOK_MEMBER(Class::method)` | non-virtual member function | deduces explicit-`this` signature; callback gets `Class* self` first |
| `SPLICE_HOOK_STATIC(...)` | any of the above | `static` instead of `thread_local static` (entry persists for program lifetime) |

#### Policy override
| Macro | Use |
|---|---|
| `SPLICE_HOOK_AS(Policy, func)` | Per-call-site policy switch (e.g. shared_mutex) |
| `SPLICE_HOOK_LIB_AS(Policy, lib, func)` | Two-arg form |
| `SPLICE_HOOK_ADDR_AS(Policy, ptr)` | Direct-address form |
| `SPLICE_HOOK_*_AS_STATIC(...)` | Static variants of the above |

> Policy must be the **first** macro argument. C preprocessor macros can't
> accept template arguments, so `SPLICE_HOOK_AS<Policy>(func)` syntax is
> impossible — we put Policy in the macro arg list instead.

#### Diagnostics
| Macro | Behaviour |
|---|---|
| `SPLICE_TRACE(func)` | Log args + return on every call |
| `SPLICE_COUNT(func)` | Counter |
| `SPLICE_TIME(func)` | Accumulated avg/min/max timing |
| `SPLICE_GET_ORIGINAL(lib, func)` | Retrieve the original function pointer |
| `SPLICE_IS_INSTALLED(lib, func)` | Query whether the hook is installed |
| `SPLICE_CALL_ORIGINAL(lib, func, args...)` | Call the original if installed, otherwise no-op |

### 6.2 Fluent modifiers (`InterceptorEntry` methods)

```cpp
auto& entry = SPLICE_HOOK(func)
    .onInvoke(lambda)          // replace
  | .before(lambda)            // before
  | .after(lambda)             // after
  | .when(predicate)            // gate
  | .once()                     // one-shot
  | .times(n);                  // N-shot

entry.is_installed();           // query
entry.disable();                // trigger disable
```

### 6.3 Global API

```cpp
splice::install_all();          // install everything in the queue
splice::install_count();        // queue size
splice_is_hooked(void* addr);   // detect whether addr is hooked (C ABI)

splice::default_context();      // the process-wide default context
splice::HookContext ctx;        // construct an isolated context (testing)
ctx.reset();                    // wipe the entire context (test fixtures)

splice::register_global_installer([]{...});  // manually queue an installer
                                              // returns InstallerToken (RAII)
```

### 6.4 RAII helpers

```cpp
splice::ScopedHook h = SPLICE_HOOK(func).onInvoke(lambda);
// h's destructor disables the hook automatically

splice::InstallerToken t = splice::register_global_installer(fn);
// t's destructor removes the installer from the queue
```

---

## 7. Build-time configuration

| Macro / Var | Default | Effect |
|---|---|---|
| `SPLICE_DEFAULT_POLICY` | `::splice::policy::rcu_writeonce` | Default callback storage policy |
| `SPLICE_REGISTRY_IMPL` | `::splice::registry::shared_mutex_map` | Registry implementation |
| `SPLICE_MAX_HOOKS` | 512 | `rcu_atomic_array` snapshot bound |
| `SPLICE_RCU_GRACE_PERIOD_MS` | 100 | RCU snapshot reclamation grace period |
| `SPLICE_LOG_TAG` | `"splice"` | Log tag string |
| `SPLICE_BUILD_TESTS` | ON | Build unit tests |
| `SPLICE_BUILD_EXAMPLES` | ON | Build examples |
| `SPLICE_BUILD_BENCHMARKS` | OFF | Build microbenches |
| `SPLICE_ENABLE_ASAN` | OFF | AddressSanitizer |
| `SPLICE_ENABLE_UBSAN` | OFF | UBSanitizer |

### Configuration recipes

```bash
# Default (safe & well-tested)
cmake --preset=windows-x64-dev

# Heavy 8-thread contention scenario — RCU registry
cmake --preset=windows-x64-release \
    -DSPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array

# Callbacks really do get swapped at runtime — flip default policy
cmake --preset=android-arm64-release \
    -DSPLICE_DEFAULT_POLICY=::splice::policy::shared_mutex

# AOSP system services (lots of hooks)
cmake --preset=android-arm64-release \
    -DSPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array \
    -DSPLICE_MAX_HOOKS=4096
```

---

## 8. Performance numbers (measured)

Hardware: AMD Ryzen 9 9950X3D (Windows) / Snapdragon 8 Gen 3 (ARM64)

| Measurement | Value |
|---|---|
| Single-thread hooked call | **22.0 ns/call** (from v1 baseline 41.1 ns, −46%) |
| 8-thread hooked call (end-to-end) | 923 ns/call (from v1 baseline 1568 ns, −41%) |
| 8t/1t ratio (end-to-end) | ~42× (registry-isolated meets < 5×) |
| Registry-isolated 8t/1t (RCU mode) | **3.9–5.5×** ✅ |
| Trampoline overhead (theoretical floor) | ~2 ns (no hook installed) |

Full bench: [`fr-010-performance-summary.md`](./fr-010-performance-summary.md)

### Hot-path cost breakdown (Windows 22.0 ns)

| Component | ns | Notes |
|---|---|---|
| Trampoline entry/exit | ~2 | jmp rel32 + saved prologue + return |
| `get_original(slot)` | 1.3 | atomic load on shared_mutex_map (0.3 in RCU mode) |
| `get_hook_as<...>(slot)` | 1.0 | same |
| `std::function::operator()` | 1.4 | dispatch — Step 5 microbench proves this is not the bottleneck |
| Callback body + `orig(x)` | ~16 | depends on the user lambda |

---

## 9. Limits and danger zones (HARD truths)

### 9.1 The physical limit of in-process non-priv hooking

**Can't do:** After a hook is installed, safely free trampoline memory and
restore the original prologue to 100% pristine state **while guaranteeing**
that no thread is currently executing inside the trampoline.

**Why:** It would require:
1. Enumerating every thread — `/proc/self/task` or Win32 `Thread32First`
2. Suspending each — `SIGSTOP` (needs ptrace) or `SuspendThread`
3. Inspecting each thread's instruction pointer to see if it's inside the
   trampoline — `GetThreadContext`

All of these need elevated privileges. Splice is an in-process non-priv
library — **it can't**.

**Splice's compromise:** Tier 1/2 disable — behaviour is restored (the
callback no longer runs), but memory isn't. Any hook library that claims
"safe in-process uninstall" is either lying or playing Russian roulette
(ShadowHook's occasional SIGILL is exactly this class of bug).

### 9.2 Don't call a hooked function from inside its own callback

```cpp
SPLICE_HOOK(printf).onInvoke([](auto orig, const char* fmt, ...) {
    printf("called %s\n", fmt);   // ← re-enters the hook, infinite loop
    // Correct: orig("called %s\n", fmt);
});
```

If you really must: use a `thread_local` flag + `.when()` gate to skip
yourself.

### 9.3 Locking hazards

Don't hold other locks inside a hook callback and then wait on them
(easy way to deadlock). Splice itself takes a reader-lock on the hot path
(under shared_mutex_map mode) — acquiring a writer-lock inside the callback
and expecting other readers to exit will hang.

### 9.4 Tier 2 disable's memory leak is **by design**

Not a bug. Documented in FR-013, CLAUDE.md, and this guide. Each hook
leaks ~4 KB (a trampoline page) for the lifetime of the process. 100 hooks
= 400 KB. Negligible for any Android process.

### 9.5 Residual ID-system risk

`SPLICE_UNIQUE_ID = __LINE__ << 16 | __COUNTER__` still collides if two
TUs have a `SPLICE_HOOK` on the same source line with the same `__COUNTER__`
progression (probability < 10⁻⁶). On collision, two hooks share one
trampoline function (C++ ODR) and one registry slot — last writer wins,
potential infinite recursion. **Mitigation:** hooking the same signature
in multiple TUs naturally spreads the call sites to different lines.

### 9.6 Disassembler boundaries

- ARM64 supports: B, BL, B.cond, ADR, ADRP, LDR literal, CBZ/CBNZ, TBZ/TBNZ, generic instructions
- x86_64 supports: call rel32, jmp rel32/rel8, Jcc rel32/rel8, RIP-relative MOV/LEA, generic 1-byte+

If the prologue contains a PC-relative instruction outside this set,
install returns `cannot relocate` and fails cleanly (warns in log, does
not crash).

---

## 10. Cookbook — common recipes

### 10.1 Frame counter (most common pattern)

```cpp
std::atomic<uint64_t> g_frames{0};

SPLICE_HOOK_LIB("libEGL.so", eglSwapBuffers)
    .after([](EGLBoolean, EGLDisplay, EGLSurface) {
        g_frames.fetch_add(1, std::memory_order_relaxed);
    });
splice::install_all();
```

### 10.2 Conditional trace (debug-only, release auto-disables)

```cpp
constexpr bool kTraceEnabled = SPLICE_DEBUG_BUILD;

if constexpr (kTraceEnabled) {
    SPLICE_TRACE_LIB("libGLESv2.so", glDrawArrays);
}
```

### 10.3 First-call diagnostic

```cpp
SPLICE_HOOK(my_init)
    .once()
    .before([](Args... args) {
        SPLICE_LOGI("my_init called for the first time");
    });
```

### 10.4 Timer with predicate gate

```cpp
SPLICE_HOOK(expensive_func)
    .when([]{ return g_profile_mode.load(); })
    .onInvoke([](auto orig, auto... args) {
        auto start = std::chrono::steady_clock::now();
        auto ret = orig(args...);
        auto dt = std::chrono::steady_clock::now() - start;
        g_total_us.fetch_add(
            std::chrono::duration_cast<std::chrono::microseconds>(dt).count());
        return ret;
    });
```

### 10.5 Runtime enable/disable (shared_mutex policy)

```cpp
SPLICE_HOOK_LIB_AS(splice::policy::shared_mutex,
                   "libfoo.so", swap_target)
    .onInvoke(initial_callback);

// Later, from another thread, swap the callback
splice::HookManager::get_hook_as<splice::policy::shared_mutex, Ret, Args...>(
    splice::default_context().slot_for(trampoline_ptr))
        .set_invoke(new_callback);
```

### 10.6 ScopedHook RAII

```cpp
{
    splice::ScopedHook h = SPLICE_HOOK(target).onInvoke(lambda);
    do_something_under_hook();
} // h leaves scope, hook auto-disables
```

### 10.7 Isolated context (unit testing)

```cpp
TEST(MyFeature, isolated_hook) {
    splice::HookContext ctx;   // local context, doesn't pollute default_context
    // ... operate on ctx ...
    ctx.reset();              // wipe at test end
}
```

---

## 11. Two hooking paradigms (worked examples)

Splice supports two fundamentally different ways to intercept behaviour. Most
hooking tutorials only show the first; knowing both is what lets you hook
things like Vulkan correctly. Each has a runnable, dual-platform-verified
example under `examples/`.

### Paradigm A — patch the target function directly

Overwrite the target's prologue (inline patch) or swap its import-table slot
(GOT/PLT/IAT). The hook fires whenever that function is called. This is the
classic approach — it's what `a production game enhancer` does to GLES, and what
[`examples/gpu_app`](../examples/gpu_app/) demonstrates: hook `glViewport`-style
commands individually and rewrite their **arguments** to upscale a render.

```cpp
SPLICE_HOOK_ADDR(&gpu::set_viewport)            // patch THIS function
    .when([]{ return enabled; })
    .onInvoke([](auto orig, int w, int h) {
        orig(w * 4, h * 4);                      // rewrite the arguments
    });
```

Use when: the target is a directly-callable exported/known function (libc,
GLES, a game's own functions, vtable slots).

### Paradigm B — hook a dispatcher, rewrite what it returns

Some APIs don't expose their functions as patchable symbols — you obtain them
through a *resolver* that returns function pointers. **Vulkan** is the prime
example: `vkGetDeviceProcAddr(device, "vkCmdDraw")` hands back a pointer. Real
Vulkan tools (RenderDoc, MangoHud, validation layers) don't patch `vkCmdDraw`
— they hook the **getter** and return their own wrappers.
[`examples/vulkan_app`](../examples/vulkan_app/) shows this with a single
Splice hook that rewrites the getter's **return value**:

```cpp
SPLICE_HOOK_ADDR(&vk::vk_get_device_proc_addr)   // patch the RESOLVER, once
    .onInvoke([](auto orig, VkDevice dev, const char* name) -> PFN_vkVoidFunction {
        auto real = orig(dev, name);
        if (strcmp(name, "vkCmdDraw") == 0) {
            g_real_draw = (PFN_CmdDraw)real;     // capture the real pointer
            return (PFN_vkVoidFunction)&my_wrapped_draw;   // hand back a wrapper
        }
        return real;                             // pass everything else through
    });
```

Use when: the target is reached via a dispatch/factory function — Vulkan
proc-addr getters, COM `QueryInterface`, plugin `dlsym` shims, any
"give me a function pointer for X" API.

### Why the distinction matters

- **One hook vs many.** Paradigm B hooks a *single* function (the resolver)
  to influence *all* commands; Paradigm A hooks each command.
- **Different prologue constraints.** Paradigm A inline-patches the target,
  so the target's prologue must be relocatable (a libc-heavy function may not
  be — see §9.6). Paradigm B never patches the commands, only the resolver,
  so command bodies can be arbitrarily complex. (In the demos,
  `vkQueuePresentKHR` is frame-counted via B even though the equivalent
  `present()` couldn't be inline-hooked via A.)
- **Capability shown.** A rewrites *arguments*; B rewrites the *return value*
  (a function pointer). Splice's `.onInvoke` does both.

| | Paradigm A (`gpu_app`) | Paradigm B (`vulkan_app`) |
|---|---|---|
| Hook target | each command | the resolver, once |
| Real-world fit | GLES, libc, game funcs | Vulkan, COM, plugin dlsym |
| Rewrites | arguments | returned function pointer |
| Target prologue must relocate | yes | no (commands untouched) |

---

## 12. Toolchain & integration

### 12.1 CMake integration

```cmake
find_package(splice REQUIRED)
target_link_libraries(my_target PRIVATE splice::splice)
```

### 12.2 VCPKG manifest

```json
{
  "dependencies": ["splice"],
  "features": {
    "benchmarks": { "description": "FR-010 microbench", "dependencies": ["benchmark"] }
  }
}
```

### 12.3 Compiler requirements

- C++20 (API surface is C++17-compatible; internals use C++20 features
  like `if constexpr` + opt-in `std::atomic<shared_ptr>`)
- CMake 3.21+
- GTest (tests) / Google Benchmark (bench)
- VS2022 + Ninja (Windows) / NDK r29+ (Android) / Clang 16+ or GCC 12+ (Linux)

### 12.4 Vendored / bundled third-party

- `platform_log.h` — Allen's cross-platform C log substrate (vendored, do not edit inline)
- `nmd_assembly.h` — x86_64 disassembler

---

## 13. FAQ

### Q1. Splice vs Frida / Substrate / ShadowHook / LSPlant?

| | Splice | Frida | Substrate | ShadowHook | LSPlant |
|---|---|---|---|---|---|
| Language | C++20 | JS/Py | C/Obj-C | C/C++ | C++ |
| Embedded-friendly | ✅ | ❌ (1+ MB runtime) | ✅ | ✅ | ✅ |
| Cross ARM64/x86_64 | ✅ | ✅ | iOS only | ARM only | ARM64 only |
| Type-safe API | ✅ | ❌ | ❌ | ❌ | ❌ |
| **True uninstall** | ❌ (honest) | ⚠ Russian roulette | ❌ | ⚠ SIGILL prone | ⚠ UB |
| Fluent DSL | ✅ | ❌ | ❌ | ❌ | ❌ |

Splice's core edge: **type-safe + fluent + honest about limits**. Doesn't
try to out-feature the others in raw breadth.

### Q2. Why no hook priority ordering?

Lesson from the predecessor framework: **hooks should be independent modules, not
have cross-module "run before this one" dependencies**. If you need order,
use a single hook with internal dispatch — don't make the framework deal
with it.

### Q3. Why isn't the fastest RCU registry the default?

RCU has trade-offs (writer 10× slower, transient ~2× memory, AOSP toolchain
risk). 99% of users run with 1–2 threads where the default `shared_mutex_map`
is plenty. Heavy-multi-thread scenarios opt in to `rcu_atomic_array`.
Principle: **safe default + powerful opt-in**.

### Q4. Why is the callback still `std::function` rather than a template?

Decided after measurement ([`fr-010-step5-microbench-report.md`](./fr-010-step5-microbench-report.md)):
- Estimate: `std::function` overhead ~5 ns
- Actual measurement: 1.4–1.5 ns (MSVC/Clang already do SBO + devirt)
- Switching to a thunk would save only ~1 ns — not worth a ~200-line
  hot-path refactor

Engineering discipline: **microbench before refactoring the hot path**,
don't optimise from textbook intuition.

### Q5. Can I hook C++ member functions?

Yes. **Non-virtual** members have a one-liner:

```cpp
SPLICE_HOOK_MEMBER(Widget::scale)
    .onInvoke([](auto orig, Widget* self, int factor) {
        return orig(self, factor);   // `this` is the first explicit arg
    });
```

`SPLICE_HOOK_MEMBER` deduces `Ret(Class::*)(Args...)` → the explicit-`this`
free-function signature `Ret(*)(Class*, Args...)` via `member_function_traits`,
and extracts the code address from the pointer-to-member.

**Virtual** members can't use this macro — a virtual's pointer-to-member is
a vtable offset, not a code address. Resolve the vtable slot on a live
instance and use `SPLICE_HOOK_ADDR(slot_addr)` instead.

### Q6. Cross-process / cross-binary hooking?

Splice is an in-process library. For cross-process you handle the injection
and bootstrap Splice yourself.

### Q7. How does Splice compare to LD_PRELOAD / SetWindowsHookEx?

Those are OS-level mechanisms. Splice is process-internal dynamic patching.
They can co-exist.

---

## 14. Advanced topics (cross-links)

- **Engine internals (patch flow / atomic install / disasm):** [`hooking-internals.md`](./hooking-internals.md)
- **Design trade-offs (deep):** [`v2-design-rationale.md`](./v2-design-rationale.md)
- **Performance journey + engineering discipline:** [`fr-010-performance-summary.md`](./fr-010-performance-summary.md)
- **Step 6 RCU design:** [`fr-010-step6-rcu-registry-design.md`](./fr-010-step6-rcu-registry-design.md)
- **Step 5 `std::function` truth:** [`fr-010-step5-microbench-report.md`](./fr-010-step5-microbench-report.md)
- **vs the predecessor framework (short):** [`splice-vs-predecessor-summary.md`](./splice-vs-predecessor-summary.md)
- **vs Detours / PolyHook2 / SubHook / rcmp (spec review):** [`splice-vs-hooking-libraries.md`](./splice-vs-hooking-libraries.md)
- **Demo loader comparison:** `reference/a production game enhancer_Loader.cpp` (v1) vs `reference/splice_game_Loader.cpp` (v2)

---

## 15. Closing line

> Reads like a DSL, runs at zero perceptible cost, honest about what it
> can and cannot do.
>
> 99% of users only need `SPLICE_HOOK(func).onInvoke([](auto orig, ...){...})` +
> `splice::install_all()`. The 1% heavy-contention crowd flips a build flag
> to swap in the RCU registry and the shared_mutex policy. Splice lets you
> do both.

---

## Changelog

- 2026-05-31: Initial English version (paired with `splice-guide-zh.md`).
  Covers Phase 0–4.5 + FR-008/010/013 + FR-009 (in progress).
