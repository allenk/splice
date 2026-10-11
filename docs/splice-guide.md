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
| **INLINE patch** (jmp rel32 on x86_64, indirect branch on ARM64) | Direct addresses / local functions | Tier 2: atomic paths with implementation-dependent fallbacks; no execution-draining guarantee; trampoline memory retained |
| **DATA_SLOT** (a vtable slot) | `splice::Vtable` on the `slot_swap` tier — see §3.4b | Tier 1: atomic, reversible; the page is restored to the protection it had, which a vtable needs and a GOT or IAT does not |

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

// 6. Re-entrancy gate — a nested call on the same thread goes to the original
.non_reentrant()                 // this hook alone: covers direct recursion
.non_reentrant(scope)            // shared with sibling hooks: splice::Reentrancy scope;
```

`.when` / `.once` / `.times` / `.non_reentrant` **compose freely** with
`.onInvoke` / `.before` / `.after`. The three action verbs (`onInvoke` /
`before` / `after`) are **mutually exclusive** — the last one set overwrites
the previous.

`.non_reentrant(scope)` is for methods that call each other. When one hooked
method's implementation calls another hooked method — a `Present1` that calls
`Present`, say — the nested call arrives through a *different* hook, so a gate
on one hook alone does not see it. Every hook that shares a `splice::Reentrancy`
is skipped while any of them is running on the thread. A nested call does not
consult `.when()` and does not spend a `.once()` / `.times(n)` budget, because it
is not a new call. The cost is one thread-local load per call, which is what a
hand-written `thread_local` flag costs. A process can hold 64 scopes at once; a
hook asked to use a scope that could not be reserved refuses to install.

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

### 3.4b Hooking a whole interface — `splice::Vtable`

A COM interface, or any C++ class with virtual functions, can be hooked by
method name rather than by address. The signature comes from the
pointer-to-member, so the callback is compile-checked:

```cpp
#include <splice/live.h>

splice::Outcome overlay = splice::Vtable<IDXGISwapChain3>{chain}
    .on(&IDXGISwapChain3::Present,       present_cb)
    .on(&IDXGISwapChain3::Present1,      present1_cb)
    .on(&IDXGISwapChain3::ResizeBuffers) .when(enabled).before(note_resize)
    .non_reentrant()                 // one scope for the whole group
    .install(splice::slot_swap);     // all or nothing

if (!overlay.ok()) { /* overlay.state(), overlay.failure() say what happened */ }
```

Every per-method gate from §3.2 applies — `.when()`, `.once()`, `.times(n)`,
`.observe()`, `.non_reentrant()`, plus two guards against arriving second:

- `.expect(address)` refuses to install unless the **slot** still holds the
  function you expected. For the `slot_swap` tier, where the slot is what
  you write.
- `.expect_prologue(bytes)` refuses unless the **target** still begins with
  the sixteen bytes you read. For the coordinated tier, which patches the
  function rather than the table — there the slot can be pristine while
  the code behind it has already been replaced, so this is the only check
  that notices. The parameter is a reference to `const unsigned char[16]`,
  not a pointer, because the engine reads that many bytes. A site that does
  not ask is not checked.

**Two tiers, and they are not interchangeable.**

| Tier | Mechanism | Coordination | Reaches |
|---|---|---|---|
| `splice::slot_swap` | one atomic pointer-sized store into the vtable slot | none needed | that vtable -- which is **every object sharing it**, not just the one you passed; another overlay swapping the same slot displaces you, or you displace it |
| `splice::live` | inline patch of the function the slot points at | every other thread suspended once | that function -- every vtable pointing at it, and direct callers too; survives other overlays' slot swaps, because every existing slot pointer stays valid |

**Neither tier is scoped to an object, and the constructor takes one, so this
is worth being plain about.** `splice::Vtable<T>{object}` reads the object's
function table and works on the table. A C++ or COM class has one vtable per
concrete class, shared by every instance, so a slot swap made through one
object is observed by all of them. Measured on two DXGI swap chains created
from the same device: one table pointer, and a hook installed through the first
chain sees frames presented on the second.

The coordinated tier is wider still, because it patches the function rather
than any table: every interface pointer that resolves to that function arrives
at your callback, as does any direct call to it from anywhere in the process.
That width is the point when the slot is contested -- it is why the tier
survives another overlay's swap -- but it is width, and a hook you believed was
scoped to your own object is not.

If you need one object only, compare identity in the callback and pass the rest
through:

```cpp
.on(&IDXGISwapChain::Present, [mine](auto orig, IDXGISwapChain* self,
                                     UINT sync, UINT flags) {
    if (self == mine) { /* ... */ }
    return orig(self, sync, flags);
})
```

**What `splice::live` does, and the one thing it does not.** It suspends every
other thread once, reads each one's program counter, and maps any that sits
inside the bytes being replaced through that site's instruction-boundary map --
so a thread already executing the prologue continues from the relocated copy of
the instruction it was at. A program counter that is not at a boundary our
decoding recognises is refused, and nothing is written. Then every site is
written, flushed with its own length, and the threads resume in reverse order.
A failure part-way puts back everything already written **before any thread
resumes**.

The limit, stated rather than implied: **`splice::live` means splice coordinated
the threads it could enumerate.** A thread created after the enumeration is not
in the set. This is not a stop-the-world -- that needs ptrace or root and is a
documented non-goal -- and the tier is named `live` rather than `safe` for
exactly that reason. It is still strictly better than the alternative, which is
requiring the caller to guarantee that nothing is executing the target.

Use `splice::slot_swap` when a slot swap is enough: it is one atomic store and
coordinates nothing, so there is nothing to go wrong. Reach for `splice::live`
when the slot is contested -- when something else may swap the same pointer and
you need to be in the call chain regardless.

### 3.4c The queue as one transaction — `install_all(splice::live)`

`install_all()` has always meant "install every hook queued above", one hook at
a time. Give it a tier and it means the same thing in one transaction:

```cpp
SPLICE_HOOK_ADDR(present_addr, decltype(&Present)).onInvoke(present_cb);
SPLICE_HOOK_ADDR(signal_addr,  decltype(&Signal)).before(signal_cb);

splice::Outcome all = splice::install_all(splice::live);
if (!all.ok()) { /* all.state(), all.failure() */ }
```

One thread enumeration and one suspension for the whole set instead of one
each, and all-or-nothing across it: a failure at the fourth hook cannot leave
three installed. Everything else is unchanged — each hook keeps its own gates,
its own `original()`, and its own `is_installed()`, because the entries are
driven rather than bypassed.

It returns `refused`, having written nothing, in three cases:

- **a queued hook that cannot join.** A symbol that will not resolve, a
  re-entrancy scope that could not be reserved, or an installer registered
  through `register_global_installer(fn)` with no group path. Installing the
  rest would hand you a group you believe is complete.
- **nothing to install.** An empty queue, or every queued hook already
  installed. `install_all()` is still safe to call twice — an already-installed
  hook does not re-join, because re-patching a function splice already owns is
  refused and would take the rest of the set down with it — but a call where
  every hook is already in place has nothing to do, and `installed` would be a
  claim about work that did not happen.
- **`splice::slot_swap`.** A queued hook names a function, by symbol or by
  address; there is no vtable slot behind it to swap. Use
  `splice::Vtable<Interface>` for that tier.

Disable through the returned `Outcome`, which puts every site back and tells
each entry it is no longer installed. An entry's own `disable()` works too, for
a hook installed on its own; calling both is the one thing to avoid, and the
entries are told precisely so that the second call reports failure instead of
restoring bytes that are already original.

Two behaviours of the coordinated tier worth knowing before you hand it an
address:

- **It follows jump thunks.** `&function` for a function in another
  translation unit is often an ILT thunk under incremental linking, and an
  import is a thunk too. The tier patches the function behind them, which is
  what you meant.
- **A second install on the same function is refused.** An installed patch is a
  jump as well, so chaining onto one would walk into splice's own relay.
  Chaining is a real feature and a design question; until it is answered, the
  honest answer is no.

`splice::live_tier_available()` says whether the tier can run at all on this
build: it needs an architecture whose prologue can be planned and an OS that
can suspend a thread and move its program counter, which today means x86_64 on
Windows. Everywhere else it returns false and `install(splice::live)` refuses.
It never falls back to a slot swap -- a tier that silently becomes a weaker
tier is worse than one that says no.

**All or nothing.** If the fifth method fails to install, the four already
written are restored before `install` returns, and the result is
`State::recovered` — safe to retry, with nothing published. `State::refused`
means nothing was ever written. The four states are `installed`, `refused`,
`recovered` and `lost`; only `lost` is the caller's problem.

**Slot resolution, and when it refuses.** `&Interface::Method` is resolved to a
slot index per C++ ABI — arithmetic on the Itanium ABI, decoding the
compiler's dispatch thunk under MSVC. It refuses, rather than guessing, for a
non-virtual member, an inheritance shape that needs `this` adjusted, an
unrecognised thunk, or an architecture it has not been taught. The escape hatch
keeps the type safety:

```cpp
    .on_slot(8, &IDXGISwapChain3::Present, present_cb)   // index by hand, signature still checked
```

**Lifecycle.** `outcome.disable()` restores every slot and releases the group's
stubs; `outcome.active()` says whether the hooks are still in place, while
`outcome.state()` stays the verdict on the install. Discarding the `Outcome`
does **not** uninstall — the hooks stay, like a `SPLICE_HOOK_STATIC` call site.

**One limit worth knowing up front.** Each hooked method claims one stub from a
per-signature pool, `SPLICE_SLOT_STUBS_PER_SIGNATURE`, which defaults to 8. A
group that runs out refuses and says so; raise the macro if you need more live
sites of one signature at a time.

**A group does not have to be an interface.** `splice::Live` fills the same
group by address, for a target that is not a method:

```cpp
splice::Live<> group;
auto outcome = group
    .at(present_addr, Present_t{}, present_cb)     // the type carries the signature
    .at(signal_addr,  Signal_t{})  .before(note_signal)
    .install(splice::live);
```

The signature comes from a value of the function-pointer type, the way
`SPLICE_HOOK_ADDR` takes it as a macro argument; the value is never called.
Every gate applies as it does on a vtable site. A group named this way has no
slot to swap, so it refuses `splice::slot_swap` rather than writing something
that is not a vtable entry.

### 3.5 Disable and lifecycle

| Operation | Behaviour |
|---|---|
| `entry.disable()` | Tier 1: atomic pointer restore; Tier 2: atomic prologue-bytes restore |
| `entry.is_installed()` | Query whether the hook is still active |
| `outcome.disable()` | A `splice::Vtable` group: every slot restored, stubs released |
| **`splice::ScopedHook`** | RAII — automatic disable when the object destructs |
| `splice::install_all()` | Run every queued installer (each `SPLICE_HOOK_STATIC` queues one) |

**Important: true uninstall + memory reclaim is NOT supported.** Splice does
not implement the execution-draining and lifetime protocol needed to reclaim
trampolines or unload hook code. Disable restores the target route but does not
cancel calls already in flight; callback storage and inline trampolines remain
allocated. This is a limitation of the current implementation, not a universal
privilege-based impossibility claim. See §9.

### 3.6 Strict exact-site installation (low-level, x86_64)

`splice_hook_address_strict_pre_rec` is an opt-in C ABI entry point. Existing
install APIs keep their behavior. The strict entry point does not unwrap jumps
or substitute GOT/IAT slots: it either patches the supplied address or refuses.

The caller provides a matching snapshot of exactly 16 readable target bytes and
must exclude both invocation and mutation throughout installation. The callback
must not throw, mutate target code or reenter install/disable. Supported prologues
are deliberately restricted; this is not a universal foreign-hook detector.

The installed jump is five bytes fitting inside one aligned quadword, direct or
via a near relay. There is no ordinary-copy fallback and no tail padding write.
Refusal does not invoke the callback or change caller output/record. Success
reports the exact INLINE site and the five modified original bytes. Repeated
installation, including an own hook, is refused. Other architectures refuse.

**This is not a busy-host installation API.** It does not coordinate thread
contexts, provide successful-hook unloading, or restore arbitrary original page
protection flags; the current OS layer returns affected pages to executable/read.
The fluent interface does not select this policy yet.

### 3.7 Cross-signature, cross-arity

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
| **Recursive hook callbacks recurse** | Calling the same hooked function inside its callback re-enters the hook; `.non_reentrant()` sends the nested call to the original |
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
  | .times(n)                   // N-shot
  | .non_reentrant([scope]);    // nested calls go to the original

entry.is_installed();           // query
entry.disable();                // trigger disable
```

### 6.3 Global API

```cpp
splice::install_all();          // install everything in the queue, one at a time
splice::install_all(splice::live);   // ...or as one coordinated transaction
splice::install_count();        // queue size
splice_is_hooked(void* addr);   // detect whether addr is hooked (C ABI)

splice::default_context();      // the process-wide default context
splice::HookContext ctx;        // construct an isolated context (testing)
ctx.reset();                    // wipe the entire context (test fixtures)

splice::register_global_installer([]{...});  // manually queue an installer
                                              // returns InstallerToken (RAII)
```

### 6.3b Interface groups (`<splice/live.h>`)

```cpp
splice::Vtable<Interface> group{object};   // reads the object's function table
group.on(pmf, callback)                    // invoke-shaped; returns the group
group.on(pmf)                              // returns the site, for gates
     .when(p).once().times(n).observe()
     .non_reentrant([scope]).expect(addr)
     .onInvoke(cb) | .before(cb) | .after(cb)   // returns the group
group.on_slot(index, pmf [, callback])     // name the slot by hand
group.non_reentrant([scope])               // one scope for every method
splice::Outcome o = group.install(splice::slot_swap);   // or splice::live

splice::Live<> addressed;                  // the same group, named by address
addressed.at(target, FuncType{} [, callback])
addressed.at(target, FuncType{}).expect_prologue(sixteen).onInvoke(cb)
splice::Outcome o = splice::install_all(splice::live);  // the queue, as one group
splice::live_tier_available()              // can the coordinated tier run here

o.ok(); o.state(); o.failure(); o.installed_sites();
o.active();                     // are the hooks in place NOW
o.disable();                    // restore every slot, release the stubs

splice_vtable_slot_of_pmf(bytes, size);    // C ABI: slot index, or -1
splice_vtable_swap(slot, fn, &prev, &rec); // C ABI: one atomic pointer store
```

`splice::State` is `installed` / `refused` / `recovered` / `lost`;
`splice::Failure` carries the site, stage, operation, thread id and OS error.
`splice::Stage` is `none` / `prepare` / `enlist` / `map` / `write` / `migrate` /
`flush` / `resume`; the slot_swap tier only ever reports the first three.
See §3.4b.

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
| `SPLICE_SLOT_STUBS_PER_SIGNATURE` | 8 | Live `splice::Vtable` sites per signature |
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

### 9.1 Installation, disable, and reclamation are different contracts

Splice does not provide safe trampoline reclamation or DLL unloading after
publishing a hook. Restoring the target route does not drain calls already in
flight, including a thread in a callee with a saved return address into hook
code or a trampoline. An instruction-pointer snapshot alone misses that case.

On Windows, thread enumeration, suspension and context inspection depend on
the relevant access rights; elevation is not a universal requirement for a
process's own threads. That does not make a complete coordination protocol
trivial: lock ownership, new threads, error recovery and saved execution state
must all be addressed. Other platforms have different APIs and constraints;
this guide makes no platform-wide impossibility claim.

Likewise, an atomic target write is not a proof of safe installation while
another thread is inside the overwritten instruction sequence. Current x86_64
installation can also fall back to ordinary writes and performs separate tail
padding. Do not infer busy-host installation safety from an atomic-path log.
Require caller-controlled quiescence until a supported coordination contract
exists.

**Current lifecycle policy:** disable restores routing, not execution lifetime.
Keep callback code and inline trampolines resident while in-flight execution
may reference them. A drawing gate is not an uninstall operation.

References: [Windows thread access rights](https://learn.microsoft.com/en-us/windows/win32/procthread/thread-security-and-access-rights)
and [Detours thread enlistment](https://github.com/microsoft/Detours/wiki/DetourUpdateThread).

### 9.2 Don't call a hooked function from inside its own callback

```cpp
SPLICE_HOOK(printf).onInvoke([](auto orig, const char* fmt, ...) {
    printf("called %s\n", fmt);   // ← re-enters the hook, infinite loop
    // Correct: orig("called %s\n", fmt);
});
```

If you really must, add `.non_reentrant()`: the nested call then goes to the
original instead of back into the callback. When the nested call reaches a
*sibling* hook rather than the same one, give both the same
`splice::Reentrancy` scope.

### 9.3 Locking hazards

Don't hold other locks inside a hook callback and then wait on them
(easy way to deadlock). Splice itself takes a reader-lock on the hot path
(under shared_mutex_map mode) — acquiring a writer-lock inside the callback
and expecting other readers to exit will hang.

### 9.4 Tier 2 disable's memory leak is **by design**

Not a bug. Documented in FR-013, the CHANGELOG and this guide. Each hook
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
classic approach — it is what a game enhancer does to GLES, and what
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
