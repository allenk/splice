# Splice vs the predecessor framework — Quick Comparison

**Author:** Allen Kuo
**Date:** 2026-05-29
**In depth:** [`v2-design-rationale.md`](./v2-design-rationale.md)
**Performance evidence:** [`fr-010-performance-summary.md`](./fr-010-performance-summary.md)

---

## In one sentence

Splice is the **production-grade successor** to the predecessor framework: it keeps the
friendly fluent API, attacks the hot-path synchronisation, completes the
disable semantics, and adds cross-platform support.

---

## Comparison

| Axis | the predecessor framework (original) | Splice v2 (now) |
|---|---|---|
| **Target platforms** | Android ARM64 only | ARM64 + x86_64; Android / Linux / Windows |
| **API style** | `the legacy hook macro(func).onInvoke([](orig, ...){...})` | `SPLICE_HOOK_ADDR(func).onInvoke([](orig, ...){...})` — **kept verbatim** |
| **Registry map** | `std::map<int, ...>` (O(log n)) | `std::unordered_map<int, ...>` (O(1) average) |
| **Registry synchronisation** | `recursive_mutex` (fully serialised) | `shared_mutex` by default; opt-in **RCU snapshot** (pure-load reader) |
| **Callback storage** | `std::function` | Same (measured: `std::function` costs ~1.5 ns and is not the bottleneck, [Step 5 report](./fr-010-step5-microbench-report.md)) |
| **Installer queue lifetime** | `static vector`, never cleared → tests miss dangling lambdas | **`InstallerToken` RAII**, O(1) release (Task #57) |
| **Interruptible?** | No disable API | **Tier 1 / Tier 2 disable** (GOT/IAT pointer swap + inline atomic restore) |
| **Policy selection** | Hard-coded | **Build-time `SPLICE_DEFAULT_POLICY`** + **per-call-site `SPLICE_HOOK_AS`** |
| **Registry implementation selection** | Hard-coded | **Build-time `SPLICE_REGISTRY_IMPL`** (`shared_mutex_map` default / `rcu_atomic_array` opt-in) |
| **ID system** | Per-TU `__COUNTER__` (**cross-TU id collisions are a latent bug**) | `SPLICE_UNIQUE_ID = __LINE__ << 16 \| __COUNTER__` + runtime `slot_for(trampoline_ptr)` (globally unique) |
| **Trampoline generation** | 7 explicit specialisations (0/1/2/3/4/5/10 args) | 1 variadic template covering every arity |
| **Logging** | Direct `__android_log_print` calls | Cross-platform `platform_log.h` substrate + hot-path throttling (`LOGD_EVERY_N`, `LOGV_ONCE`) |
| **Testability** | Global singleton; tests cannot reset it | `HookContext` can be instantiated independently + `reset()` support |
| **Single-thread performance** | Never measured | **22.0 ns/call** (improved from 41.1 to 22.0, −46%) |
| **8 reader threads** | Never measured | Registry isolation ratio **3.9–5.5×** (improved from ~50×, −92%) |
| **CI / sanitizers** | None | ASan + UBSan + TSan, benchmark regression gate |

---

## What did not change (your design choices are kept)

1. **The "moment of signature" of the friendly fluent API** — the
   `.onInvoke(lambda)` form, the chainable builder and `auto orig` deduction
   are all untouched.
2. **Callbacks written as `std::function`** — measured not to be the
   bottleneck, so there is no reason to switch to thunks
   ([Step 5 report](./fr-010-step5-microbench-report.md)).
3. **Per-call-site trampolines** — one static function per hook site, no JIT
   thunks.
4. **C macro logging** — `platform_log.h`, no C++ logging library.

---

## Why these changes were worth making

1. **The cost in real multi-threaded scenarios** — when 4–8 game render
   threads call the same hooked function concurrently, the predecessor framework's
   `recursive_mutex` serialises every reader and per-call latency blows up by
   ~50×. Splice's `shared_mutex` / RCU paths bring that back to 3–9×.
2. **Safety in long-lived processes** — without a disable API the only
   assumption available is "the hook lives as long as the program". Splice's
   Tier 1/2 disable makes scenarios that need hooks switched on and off at run
   time, such as Android system services, feasible.
3. **Cross-platform** — the original ran only on ARM64 Android. With x86_64
   added, the toolchain itself can be benchmarked and debugged directly in a
   Windows development environment, without an `adb push` every time.
4. **Testable** — a full set of unit tests, integration tests and
   microbenchmarks gives regressions a safety net; changing the predecessor framework
   used to mean "pray, then run it on a device".
5. **Engineering discipline made permanent** — the escape-hatch principle and
   "verify with a microbenchmark before optimising" are written into the docs,
   so v1's guesswork optimisation is not repeated.

---

## What it does not replace

Splice still **relies on** the core ideas of the predecessor framework lineage:
- Compile-time trampoline generation (`__COUNTER__` → a unique static function)
- C-style hooking by symbol name + library
- The ARM64 disassembly logic (**ported verbatim**, not rewritten)

If your production code still uses the predecessor framework, **there is no need to
rush a switch** — v1's performance is entirely adequate in single-threaded
scenarios. Splice targets v1's specific pain points (multi-threaded
contention, disable, cross-platform); it is not a wholesale replacement.

---

## Further reading

- [`v2-design-rationale.md`](./v2-design-rationale.md) — design trade-offs in depth
- [`fr-010-performance-summary.md`](./fr-010-performance-summary.md) — the full performance journey
- [`fr-010-step6-rcu-registry-design.md`](./fr-010-step6-rcu-registry-design.md) — the RCU registry design
- [`CHANGELOG.md`](../CHANGELOG.md) — what shipped, and when
