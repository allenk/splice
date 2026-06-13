# Splice vs other C/C++ hooking libraries — spec review

**Audience:** anyone choosing a function-hooking library, or evaluating where
Splice sits in the landscape.
**Method:** **spec review only** — feature/capability comparison from each
project's documentation and source. **No benchmarks were run**; latency
claims are not compared here (see `docs/fr-010-performance-summary.md` for
Splice's own measured numbers).
**Compared:** Splice ·
[ShadowHook](https://github.com/bytedance/android-inline-hook) ·
[Microsoft Detours](https://github.com/microsoft/Detours) ·
[PolyHook 2.0](https://github.com/stevemk14ebr/PolyHook_2_0) ·
[SubHook](https://github.com/Zeex/subhook) ·
[rcmp](https://github.com/Smertig/rcmp)
**Date:** 2026-06-05. Verify against upstream before relying on a specific row.

---

## 1. TL;DR — where each one wins

| Library | One-line | Pick it when |
|---|---|---|
| **Splice** | Type-safe fluent C++20, **one API across Android+Linux+Windows and ARM64+x86_64** | You want a typed DSL-grade API spanning platforms/arches, and accept disable-not-uninstall |
| **ShadowHook** | The **Android ARM** inline-hook incumbent (ByteDance, TikTok/Douyin) | You ship on Android ARM (incl. arm32/thumb) and want production-proven inline hooks + unhook |
| **Microsoft Detours** | The Windows gold standard — transactional, thread-safe, **true uninstall** | You are Windows-only and need battle-tested attach/detach + DLL injection |
| **PolyHook 2.0** | Widest hook-type variety on Windows (vtable, EAT, HW-breakpoint…) | You need vtable/EAT/breakpoint hooks on Windows x86/x64 |
| **SubHook** | Tiny, dependency-free C inline hooker | You want a minimal x86/x64 inline hook in C with zero deps |
| **rcmp** | Clean C++17 inline hooker, typed call-conv aware | You want a tidy modern API for x86/x64 inline hooks and never need to unhook |

**The honest headline:** Splice is **not** alone on ARM64 Android — that turf
belongs to **ShadowHook** (and the wider Android inline-hook field: Dobby,
And64InlineHook, ByteHook for PLT, etc.). What Splice uniquely offers among
this set is **one type-safe API that spans both ARM64 *and* x86_64, and
Android *and* Linux *and* Windows**. Each rival is a *specialist*:
ShadowHook = Android/ARM, Detours = Windows, PolyHook2/SubHook/rcmp = x86/x64.
Splice is the generalist.

---

## 2. At-a-glance matrix

Columns ordered by closeness to Splice's use case.

| Axis | **Splice** | ShadowHook | Detours | PolyHook2 | SubHook | rcmp |
|---|---|---|---|---|---|---|
| **OS** | Android, Linux, Windows | **Android only** | Windows only | Windows (Linux partial) | Win/Linux/macOS | Win/Linux |
| **Arch** | ARM64, x86_64 | **arm32/thumb, arm64** | x86/x64/ARM/ARM64 | x86/x64 | x86 (32/64) | x86/x64 |
| **Inline patch** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Import table (GOT/PLT, IAT)** | ✅ GOT/PLT + IAT | ✖ (sibling ByteHook does PLT) | ✅ import edits | ✅ IAT + EAT | ❌ | ❌ |
| **vtable / VFunc swap** | ✖ (ADDR on a slot) | ✖ | ✖ | ✅ | ❌ | ❌ |
| **HW/SW breakpoint** | ❌ | ❌ | ❌ | ✅ | ❌ | ❌ |
| **Register intercept** (read/modify CPU regs mid-func) | ❌ | ✅ | ❌ | ❌ | ❌ | ❌ |
| **Atomic install** | ✅ ARMv8 / x86 aligned | ✅ arm64 (32-bit caveat) | via thread suspend | standard | standard | standard |
| **Thread coordination** | pre-patch callback (no suspend) | island trampolines + modes | **suspends + rewrites IPs** | none | none | none |
| **Uninstall / disable** | tiered **disable** (Tier1 reversible; Tier2 atomic restore, trampoline leaks) | **unhook** (best-effort; 32-bit crash risk) | **true detach** (restore + free) | unhook | remove | **none** |
| **Multi-hook coordination** | registration order | **shared / multi / unique modes** | transaction | per-type | ✖ | ✖ |
| **API style** | fluent, **type-safe** C++20 | C | C, transactional | C++20 OO | C (+C++ wrap) | C++17 typed |
| **Member-function sugar** | ✅ (non-virtual) | ✖ | ✖ | ✅ (vfunc) | ✖ | ✅ |
| **Diagnostics sugar** | ✅ TRACE/COUNT/TIME | ✖ | ✖ | ✖ | ✖ | ✖ |
| **RAII auto-disable** | ✅ ScopedHook | ✖ | ✖ | partial | ✖ | ✖ |
| **Pending hooks** (not-yet-loaded ELF) | ✖ (resolves at install) | ✅ | ✖ | ✖ | ✖ | ✖ |
| **DLL/SO injection** | ✖ (in-process only) | ✖ (in-process) | ✅ | ✖ | ✖ | ✖ |
| **License** | MIT | MIT | MIT (4.0.1+) | MIT | BSD-2-Clause | MIT |
| **Maturity** | v1.0.0 (new) | **production at scale** | very mature (MS) | mature | mature, small | small/young |

> ✅ first-class · ✖ not offered but achievable another way · ❌ not supported

---

## 3. Per-library spec notes

### ShadowHook — the Android ARM incumbent (the closest rival)
- **OS/Arch:** Android only (API 16–36); armeabi-v7a (arm32 + thumb) and
  arm64-v8a. **No x86/x86_64**, no desktop OS.
- **Strategy:** inline only. Hook by absolute address, by instruction
  address, or by library basename + symbol (with **pending hooks** for ELFs
  not yet loaded). Relocates the displaced prologue; uses ±128 MB relative
  jumps into allocated "island" trampolines (which bounds simultaneous hooks
  per ELF).
- **Multi-hook modes** (a real strength): `shared` (default; auto recursion
  guard, needs `SHADOWHOOK_STACK_SCOPE()` in proxies), `multi` (chaining),
  `unique` (one hook per address).
- **Intercept/register API:** read/modify CPU registers (incl. FPSIMD) at a
  given instruction — something none of the others here offer, Splice
  included.
- **Unhook:** supported (`shadowhook_unhook`), best-effort. Documented
  caveats: in 32-bit, hooking hot functions has a "certain probability of
  crashes" (atomic-covering limit); unhook can return error/unfinished.
- **Maturity:** ByteDance production — TikTok/Douyin/Toutiao/Lark. The
  battle-tested choice for Android inline hooking. **C API**, MIT.

### Microsoft Detours — the Windows gold standard
- **Arch:** x86, x64, ARM, ARM64 (+ ARM64EC).
- **Model:** transactional — `DetourTransactionBegin` → `DetourAttach`/
  `DetourDetach` → `DetourUpdateThread` → `DetourTransactionCommit`.
- **The differentiator:** `DetourUpdateThread` enlists threads so their
  **instruction pointers are rewritten** on commit — real thread
  coordination (Windows allows in-process `SuspendThread` without elevation).
  This is *why* Detours can **truly detach** (restore prologue **and** free
  the trampoline) safely.
- **Extras:** DLL injection, import editing, payloads.
- **Cost:** Windows-only; C API, manual function-pointer casts; no fluent/
  type-safe API; no Android.

### PolyHook 2.0 — widest hook-type variety
- x86/x64; Windows primary, "partial unix … not well tested". No ARM.
- Hook types: inline, runtime-inline (JIT stubs), VFuncSwap, VTableSwap, SW
  breakpoint, **HW breakpoint**, IAT, **EAT** — the richest menu here.
- Capstone (default) or Zydis. C++20, MIT.

### SubHook — minimal, dependency-free
- x86 only (32 & 64-bit); Windows/Linux/macOS. Inline + trampoline.
- Own disassembler — only a small prologue-instruction subset; trampoline can
  be NULL on unsupported prologues. C with C++ wrapper. BSD-2-Clause.

### rcmp — clean modern C++17 inline hooker
- x86/x86-64 ("more soon"); Windows/Linux. Inline via length disasm.
- Uses **nmd by Nomade040** — the *same* x86_64 length disassembler Splice
  vendors. Typed, x86 calling-convention aware. MIT.
- **Explicitly "No way to disable hook."**

### Splice — type-safe, fluent, cross-platform/arch
- ARM64 + x86_64; Android (production-targeted), Linux, Windows.
- GOT/PLT (ELF) + IAT (PE) pointer-swap (Tier 1) and inline atomic patch
  (Tier 2). Real single-copy-atomic install (ARMv8 §B2.2.1 / Intel aligned
  8-byte) **without** thread suspension — a pre-patch callback updates the
  registry before the patch lands.
- **Disable, not uninstall:** Tier 1 reversible; Tier 2 atomic prologue
  restore but trampoline leaks (documented). No stop-the-world suspension by
  design (would need ptrace/root on POSIX).
- Fluent type-deduced API, `-fno-exceptions` friendly; `.before/.after/.when/
  .once/.times`, `SPLICE_HOOK_MEMBER`, `TRACE/COUNT/TIME`, `ScopedHook`;
  opt-in RCU registry.

---

## 4. The axes that actually decide it

### Platform & architecture (the first filter)
- **Android ARM (incl. arm32/thumb):** ShadowHook is the incumbent; Splice
  covers arm64 (arm32 is post-1.0, FR-006) and adds x86_64 + desktop OSes.
- **Windows:** Detours (any arch incl. ARM64) and PolyHook2 (x86/x64);
  Splice as a cross-platform alternative.
- **One codebase across Android + Linux + Windows AND ARM64 + x86_64:**
  **Splice only.** Every rival is single-OS-family or single-arch-family.

### Uninstall safety — a three-way spectrum
- **Detours — safe uninstall.** Suspends threads + rewrites IPs, then
  restores and frees. Zero residue. Windows-only luxury.
- **ShadowHook — pragmatic unhook.** Real `unhook`, used in production, but
  best-effort: a documented small crash window on 32-bit hot functions and
  occasional unhook-incomplete states. No full thread coordination.
- **Splice — disable, not uninstall.** Tier 1 fully reversible; Tier 2
  restores behaviour atomically but **leaks the trampoline** — deliberately
  no thread suspension, so there is *no* unhook crash window, at the cost of
  not reclaiming memory. (PolyHook2/SubHook also unhook; rcmp can't at all.)

These are principled, different bets: Detours pays thread-suspension for
clean removal; ShadowHook accepts a tiny risk window for in-place removal;
Splice refuses both risk and suspension and is upfront that inline hooks then
can't reclaim memory. See `docs/splice-guide.md` §9.1.

### API ergonomics & type safety
Splice and rcmp are the type-safe modern-C++ options; PolyHook2 is OO C++;
Detours, SubHook, and ShadowHook are C-style with manual pointer casts. Only
Splice has the full fluent DSL + member-hook sugar + one-liner diagnostics +
RAII scopes. ShadowHook counters with capabilities Splice lacks: a
register-intercept API, shared/multi/unique multi-hook modes, and pending
hooks for unloaded ELFs.

### Hook-type breadth
PolyHook2 wins variety (vtable/EAT/HW-breakpoint). ShadowHook adds register
intercept. Splice covers inline + GOT/PLT/IAT + non-virtual member. SubHook
and rcmp are inline-only.

---

## 5. The modern-C++ advantage (Splice's biggest differentiator)

Cross-platform reach is the *first filter*; **modern C++ is the reason to
pick Splice once you're in C++**. This isn't aesthetics — it removes a whole
class of bug that the C-based libraries (ShadowHook, Detours, SubHook)
structurally cannot.

### The killer point: the compiler checks your hook signature

Hooking's most dangerous bug is **re-declaring the target's signature by hand**
— the compiler can't see the real function, so a mismatch becomes silent
calling-convention / register / stack corruption (the worst kind of UB).

**ShadowHook (C):**
```c
int (*orig)(int);
int proxy(int x) { SHADOWHOOK_STACK_SCOPE(); return orig(x) + 1; }
shadowhook_hook_sym_name("libfoo.so", "bar", (void*)proxy, (void**)&orig);
```
`proxy`/`orig` are hand-written; the `(void*)` casts erase all type info.
If `bar` is really `int bar(int,int)`, you get runtime UB — nothing warns you.

**Splice (C++):**
```cpp
SPLICE_HOOK_LIB("libfoo.so", bar).onInvoke([](auto orig, int x){ return orig(x)+1; });
```
`decltype(&bar)` deduces the type; your use of `orig`/args must be consistent
or it **fails at compile time**. Splice can't verify your declaration matches
the binary (nobody can without symbols) — but it eliminates the *within-your-
code* mismatch that's trivially easy to get wrong in C. **No C library can do
this.**

### What modern C++ buys, concretely (and which rivals have it)

| Capability | Why it matters for hooking | Splice | ShadowHook | Detours | PolyHook2 | SubHook | rcmp |
|---|---|---|---|---|---|---|---|
| `decltype` signature deduction | kills the hand-written-signature UB class | ✅ | ✖ (C) | ✖ (C) | partial | ✖ (C) | ✅ |
| Fluent DSL (`.when().once().before()`) | composable, reads top-to-bottom | ✅ | ✖ | ✖ | ✖ | ✖ | ✖ |
| Lambda capture for hook state | no `void* user_data` plumbing | ✅ | ✖ | ✖ | partial | ✖ | ✅ |
| `if constexpr` void/return handling | one code path, no macros per arity | ✅ | n/a | n/a | partial | n/a | partial |
| Variadic-template trampolines | any arity, compile-time, no JIT thunk | ✅ | n/a | n/a | runtime JIT | n/a | ✅ |
| RAII (`ScopedHook`) | scope-bound auto-disable | ✅ | ✖ | ✖ | partial | ✖ | ✖ |
| Member-function sugar | `SPLICE_HOOK_MEMBER(Class::m)` | ✅ | ✖ | ✖ | ✅(vfunc) | ✖ | ✅ |
| Exception-neutral | works with **and** without `-fno-exceptions` | ✅ | n/a (C) | n/a (C) | ? | n/a (C) | ? |

The only other genuinely modern-C++ option here is **rcmp** (C++17, typed) —
but it's **x86/x64-only and can't unhook**. So among libraries that are *both*
modern-C++ *and* cross-arch (ARM64 + x86_64) *and* cross-OS, **Splice stands
alone**. PolyHook2 is OO C++ but C++ in the "classes + manual casts" sense,
not the "the compiler deduces and checks your hook" sense.

### Exception-neutral (a modern-C++ nicety the C libs can't frame either way)

Splice itself never throws (return codes + `noexcept`), yet it doesn't force
`noexcept` on your callback. So it compiles and runs **both** with exceptions
enabled (the default we test under) **and** under `-fno-exceptions` (the AOSP
norm) — your callback may throw in the former, must not in the latter, and
the library is correct in both. C libraries simply have no exceptions to
reason about; Splice gives C++ users the choice without imposing a policy.

### The honest counterpoint (why the rivals stay C)

Modern C++ is a trade, not a free win. ShadowHook/Detours are C **on purpose**:
- **Stable C ABI** — callable from any language; clean injected-`.so` boundary.
- **Hostile/early-init contexts** — injected hooks may run before the C++
  runtime is up; global constructors, `std::function`, exceptions are
  liabilities there. Splice's `static InterceptorEntry` registration (global
  ctors) and hot-path `std::function` are the real costs of its C++ comfort.
- **Smaller footprint.**

So the precise framing:

> **Splice trades deployment-context robustness (hostile injection, language-
> agnostic ABI, early init) for compile-time safety and developer ergonomics.**

If you control the build and target normal C++ apps / your own software, that
trade is hugely in your favour. If you inject into arbitrary third-party
processes at the earliest moments, a C library is the safer tool.

### Where this could go further (the modern-C++ roadmap)

Splice already leans on `decltype`, variadics, `if constexpr`, RAII. The next
steps would widen the gap none of the others are chasing: **concepts** to
constrain hook signatures with friendly errors, `std::expected<Hook,
InstallError>` typed install results, fewer global constructors via a
compile-time registry, and (C++26) **reflection** for automatic argument
tracing (today `SPLICE_TRACE` skips per-arg formatting precisely because
generic formatting is hard without reflection).

---

## 6. Where Splice honestly stands

**Splice's unique ground:**
- The only one here with **one type-safe API across both ARM64 and x86_64 and
  across Android/Linux/Windows**. Everyone else is a specialist.
- The most ergonomic API (fluent chain, member hooks, diagnostics, RAII).
- Honest, documented disable tiers + opt-in RCU concurrency.

**Where rivals are ahead:**
- **ShadowHook** is far more battle-tested on Android, supports arm32/thumb
  (Splice: post-1.0), and offers register-intercept, multi-hook modes,
  pending hooks, and unhook — none of which Splice has.
- **Detours** offers *true* uninstall + DLL injection and years of Windows
  hardening.
- **PolyHook2** offers more hook *types*.
- **SubHook/rcmp** are smaller/simpler for plain x86/x64 inline hooks.
- Splice is **v1.0.0 (new)** vs years of field use elsewhere.

**Rule of thumb:**
- Android ARM, production, need unhook / register intercept → **ShadowHook**.
- Windows-only, need uninstall or injection → **Detours**.
- Windows, exotic hook types → **PolyHook2**.
- Minimal x86/x64 inline hook → **SubHook** (C) / **rcmp** (C++17).
- **One typed API across ARM64 + x86_64 and Android + Linux + Windows →
  Splice.**

---

## 7. Sources

- ShadowHook — <https://github.com/bytedance/android-inline-hook>,
  `doc/manual.md` (Android API 16–36; armeabi-v7a/arm64-v8a, no x86; inline;
  shared/multi/unique modes; register intercept; pending hooks; `shadowhook_
  unhook` with 32-bit crash-probability caveat; MIT; TikTok/Douyin/Lark).
- Microsoft Detours — <https://github.com/microsoft/Detours>, wiki +
  Microsoft Research docs (x86/x64/ARM/ARM64/ARM64EC; transactional
  attach/detach; `DetourUpdateThread`; MIT 4.0.1+).
- PolyHook 2.0 — <https://github.com/stevemk14ebr/PolyHook_2_0> README
  (C++20 x86/x64; inline/runtime/VFuncSwap/VTableSwap/SW+HW breakpoint/IAT/
  EAT; Capstone/Zydis; MIT; Windows primary, partial unix).
- SubHook — <https://github.com/Zeex/subhook> (C + C++ wrapper; x86 32/64;
  inline + trampoline; own prologue disassembler; BSD-2-Clause; install/
  remove).
- rcmp — <https://github.com/Smertig/rcmp> README (C++17; Windows/Linux;
  x86/x86-64; inline via nmd; MIT; "No way to disable hook").
- Splice — this repository (`docs/splice-guide.md`, `CHANGELOG.md`).

> Spec review only — no comparative benchmarks were run, per request. For
> Splice's own measured latency see `docs/fr-010-performance-summary.md`.
