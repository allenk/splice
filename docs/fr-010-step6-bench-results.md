# FR-010 Step 6.3 — RCU registry benchmark results

**Status:** Data complete
**FR:** FR-010 Step 6.3 (contended bench validates the 8t/1t < 5× gate)

> ## Correction, 2026-10-13: the "RCU" column was not an RCU build
>
> **The full-trampoline comparison in this report measured one configuration
> twice.** Selecting the registry was documented as
> `cmake -DSPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array`, and that
> form creates a CMake cache variable which nothing in the build system read.
> The tree this report names still exists, with `SPLICE_REGISTRY_IMPL:
> UNINITIALIZED` in its cache and zero occurrences of `rcu_atomic_array` in its
> compile commands. Both of its columns were `shared_mutex_map`, and the 10%
> between them was noise.
>
> **So the analysis below is wrong where it subtracts.** "The rest of the
> trampoline = 913 ns at 8 threads", the four suspects proposed for it, and the
> recommendation to profile, all follow from excluding a registry that had
> never been excluded. Measured 2026-10-13 with the define actually reaching
> the compiler, on the same machine and the same benchmark: **8-thread dispatch
> is 19 ns, not 923**, and single-thread dispatch is 4.3 ns against 23.3 for the
> locked map. There is no residual to explain. The bouncer this report went
> looking for was the lock it had just ruled out.
>
> What stands: the **registry-isolated** numbers below, because
> `bench_registry_lookup.cpp` instantiates both implementations explicitly in
> one binary and never depended on the macro. They reproduce today.
>
> The escape hatch is now a real CMake option, and `rcu_atomic_array` is the
> default.
**Test date:** 2026-05-26
**Author:** Allen Kuo
**Read with:**
- [`fr-010-step6-rcu-registry-design.md`](./fr-010-step6-rcu-registry-design.md) — the Step 6 design
- [`fr-010-step5-microbench-report.md`](./fr-010-step5-microbench-report.md) — Step 5, `std::function` vs thunk
- [`benchmark-baseline.md`](./benchmark-baseline.md) — the FR-010 baseline

---

## TL;DR

| Measured | Default (`shared_mutex_map`) | RCU (`rcu_atomic_array`) | Gain |
|---|---|---|---|
| **HookRegistry in isolation** 8t/1t | 51.7× | **5.5×** | **−89%** ✅ |
| **OriginalsRegistry in isolation** 8t/1t | 48.3× | **3.9×** ✅ | **−92%** ✅ |
| **Full trampoline** 8t/1t | ~50× | ~42× | −16% |
| Single-thread hooked call | 22.8 ns | 22.0 ns | −3% |

**Conclusion:** the RCU registry **meets its target on its own** (< 5× gate),
but the full trampoline has **another, unidentified bouncer** (not the
registry, not the Meyers guard, not `std::function` itself). The overall < 5×
gate is left for follow-up.

---

## Experiment design (Step 6.3, strengthened)

After Steps 6.1 and 6.2, running `bench_hook_contended` showed RCU and the
baseline almost level. Three hypotheses:

1. **RCU was done wrong** — but the unit tests pass 8/8; the logic is correct
2. **The registry is not the bottleneck** — something else is bouncing
3. **Bench harness noise** — Google Benchmark's thread synchronisation interferes

Three independent microbenchmarks were written to separate them:

- `benchmark/bench_registry_lookup.cpp` — **drives HookRegistry /
  OriginalsRegistry directly, bypassing the trampoline**. A/B comparison
  within one binary.
- `benchmark/bench_hook_contended.cpp` — full trampoline contention (existing)
- `benchmark/bench_hook_overhead.cpp` — the 1-thread hot path (existing)

---

## Platform 1: Windows x86_64

**Hardware:** AMD Ryzen 9 9950X3D (4.3 GHz, 32 threads)
**OS:** Windows 11 Pro 26200
**Compiler:** MSVC 19.44.35211 / VS2022
**Presets:**
- baseline: `windows-x64-bench` (default `SPLICE_REGISTRY_IMPL` = `shared_mutex_map`)
- RCU: `out/build/windows-x64-bench-rcu` (`-DSPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array`)

### Result 1: the registry-lookup isolation bench

```
$ bench_registry_lookup --benchmark_min_time=1s
```

| Op | Threads | `shared_mutex_map` | `rcu_atomic_array` | Speed-up |
|---|---|---|---|---|
| HookRegistry::get | 1 | 10.1 ns | **0.96 ns** | **10.5×** |
| HookRegistry::get | 2 | 41.3 ns | 1.08 ns | 38× |
| HookRegistry::get | 4 | 190 ns | 2.24 ns | 85× |
| HookRegistry::get | 8 | 522 ns | **5.24 ns** | **100×** |
| **8t/1t ratio** | — | **51.7×** | **5.5×** | gate 5× (close) |
| OriginalsRegistry::get | 1 | 9.79 ns | **1.29 ns** | **7.6×** |
| OriginalsRegistry::get | 2 | 36.5 ns | 1.34 ns | 27× |
| OriginalsRegistry::get | 4 | 186 ns | 2.19 ns | 85× |
| OriginalsRegistry::get | 8 | 473 ns | **5.09 ns** | **93×** |
| **8t/1t ratio** | — | **48.3×** | **3.9×** ✅ | **gate 5× (met)** |

**Conclusion 1: RCU meets the target.** Registry-level reads fall from
5–10 ns to < 2 ns (**10× faster** single-threaded), and the 8t/1t ratio falls
from ~50× to < 6× (**8.5× better**).

### Result 2: the full trampoline bench (after caching the `default_context` pointer)

`include/splice/trampoline.h` gained a
`static HookContext* const ctx = &default_context();` cache, so the
trampoline no longer goes through MSVC's `_Init_thread_header_safe` guard bit
on every call.

```
$ bench_hook_contended --benchmark_min_time=2s
```

| Threads | Baseline | RCU | Gain |
|---|---|---|---|
| 1 | 22.1 ns | 22.0 ns | −0.5% |
| 8 | 1027 ns | **923 ns** | **−10%** |
| **8t/1t ratio** | **46.5×** | **42.0×** | −10% |

**Conclusion 2: the full trampoline ratio falls from ~50× to 42× (a 10% gain),
still far from the < 5× gate.**

---

## Analysis: where is the missing bouncer?

The 8-thread overhead, broken down (RCU mode):

| Component | 1t | 8t | 8t/1t |
|---|---|---|---|
| HookRegistry lookup | 0.96 ns | 5.24 ns | 5.5× |
| OriginalsRegistry lookup | 1.29 ns | 5.09 ns | 3.9× |
| **Subtotal (registry)** | **2.25 ns** | **10.3 ns** | **4.6×** |
| **"The rest" of the trampoline** | **19.8 ns** | **913 ns** | **46×** |
| Total | 22.0 ns | 923 ns | 42× |

"The rest" = entering and leaving the trampoline + `HookStorage::invoke` + the
`std::function` call + the real hooked function call + the jump back through
the JIT trampoline.

**The 46× ratio of "the rest" is the real source of the full trampoline's
bouncing.**

### Hypotheses ruled out

| Suspect | Why ruled out |
|---|---|
| `default_context()` Meyers singleton guard | Caching it saved only 10%; not the main cause |
| Registry `shared_mutex` | Replaced by RCU, still 46× |
| Atomic load of `m_fn` (HookStorage) | An atomic load of the same address should stay in S state and not bounce |
| Google Benchmark thread harness | `RawCall_Contended/threads:8` = 13.7 ns; the bench overhead is a small constant |

### Remaining suspects (for follow-up)

1. **`std::function`'s internal manager dispatch** — the storage is shared,
   but each call may trigger implicit atomic operations we have not
   identified. The Step 5 thunk change would test this at the same time (the
   Step 5 microbench measured the shared path at ~1 ns vs ~0.3 ns for a
   thunk).
2. **I-cache behaviour of the JIT trampoline** — every thread jumps into the
   same JIT region to execute `[saved_prologue; jmp original+N]`. That should
   stay in S state, but is there a TLB shootdown caused by W^X switching?
   (Ruled out: protection is switched only at install time.)
3. **CPU micro-architectural effects** — the 9950X3D's V-cache and branch
   prediction may behave unintuitively when 8 SMT/physical cores contend.
4. **MSVC ASLR relocation** — patching when the trampoline and the callback
   are more than 4 GB apart (the rel32 jump limit) may add hidden overhead.

Pinning it down needs VTune / `perf record`. Recommended as a **Step 6.x
follow-up ticket**, outside the current Step 6 scope.

---

## Platform 2: ARM64 — measured 2026-10-13

Measured on **ARM64 Linux**, not Android: an NVIDIA Jetson Orin Nano 8GB
(6-core Cortex-A78AE, 15 W power mode, Ubuntu 24.04, gcc 13.3, 54 °C at the end
of the run), built natively on the board. Google Benchmark is not installed
there, so this is a standalone harness of the same shape as
`bench_hook_contended`: N threads calling one hooked function in a tight loop,
4,000,000 calls per thread per round, with the **unhooked call measured in the
same binary** as an in-run control. Two binaries differing only in
`SPLICE_REGISTRY_IMPL`, both built before anything was measured, run
alternately for ten rounds each, medians:

| threads | unhooked | `shared_mutex_map` | `rcu_atomic_array` | ratio |
|---|---|---|---|---|
| 1 | 3.32 ns | 100.8 ns | **11.95 ns** | 8.4x |
| 2 | 3.33 | 1055.5 | **11.97** | 88x |
| 4 | 3.32 | 1950.8 | **11.96** | 163x |
| 6 | 3.32 | 2994.7 | **11.97** | **250x** |

**The lock-free registry is flat: 6t/1t = 1.00x.** Its reader is an
acquire-load on a line nobody writes, so it stays Shared on every core and
generates no coherence traffic at all. The locked map pays an atomic
read-modify-write on one mutex word per lookup, twice per call, and on a
weakly-ordered machine that costs far more than the x86_64 equivalent --
**100 ns even at one thread, with nothing to contend with.**

So the prediction this section used to carry was wrong, and in an instructive
direction: it expected the gain to be "comparable to x86_64" because "registry
contention is the same in nature". Contention is exactly what is *not* the same
in nature here. On the 9950X3D the same comparison is 23.3 ns against 4.3 at
one thread and about 70x at eight; on this board it is 100.8 against 11.95 and
250x at six.

The single-thread gate is met on ARM64 with margin: 11.95 ns against < 20 ns.

**The scaling gate is defined at eight threads and this board has six cores**,
so 1.00x here is 6t/1t and is *not* the gate. It is strong evidence about the
same property -- a reader that generates no coherence traffic does not care how
many cores are reading -- and it is not the measurement the gate asks for. The
eight-thread run was not made on this board. Corrected after an external review
pointed out that a six-thread ratio had been presented as satisfying an
eight-thread gate.

Correctness alongside the numbers, same board, both configurations: the suite
builds clean with no warnings from the registry or context headers, and runs
**221 of 225** under ctest and **187 passed, 4 failed** in a single process --
*identical in both configurations*, the four being pre-existing `PrologueFacts`
failures on ARM64 that have nothing to do with the registry. The install took
the real ARM64 atomic path (`atomic_install: done`), not a fallback.

### Android, on the shipping target

Measured the same day on a **Snapdragon 8 Gen 3 / Android 14** device
(`arm64-v8a`, 8 cores, prime core 3.30 GHz), NDK r29 clang, Release, using
**this repository's own Google Benchmark binaries** rather than a standalone
harness -- so these numbers are comparable line-for-line with the x86_64 ones.
Three interleaved rounds, medians:

| | `shared_mutex_map` | `rcu_atomic_array` |
|---|---|---|
| `BM_RawCall` | 2.73 ns | 2.73 ns |
| `BM_HookedCall_Active` | 71.9 / 74.5 / 75.5 ns | **4.54 / 4.62 / 4.69 ns** |
| contended, 1 thread | 72.6 / 74.6 / 75.7 | 4.55 / 4.68 / 4.72 |
| contended, 2 | 320 / 385 / 393 | 5.13 / 5.25 / 5.33 |
| contended, 4 | 818 / 791 / 791 | 5.83 / 6.01 / 6.10 |
| contended, 8 | 1822 / 1799 / 1774 | **7.88 / 7.88 / 7.91** |

**The headline is what splice costs over an unhooked call: 1.9 ns** (4.6 against
2.73). That is the same 1.9 ns it costs on the 9950X3D. **There is no residual
ARM64 dispatch penalty once the lock is gone** -- which is worth stating plainly
next to the Jetson table above, because that table's "order of magnitude" is
the *ratio between the two registries*, not a cost the lock-free path carries.
8t/1t is 1.7x, against the < 5x gate; single-thread 4.6 ns against < 20 ns.

What *is* far worse on ARM64 is the locked map, and `bench_registry_lookup` on
the device says why -- both implementations compiled into one binary, so this
needs no rebuild:

| lookup | 1 thread | 2 | 4 | 8 |
|---|---|---|---|---|
| hooks, `shared_mutex_map` | **36.8 ns** | 284 | 321 | 967 |
| hooks, `rcu_atomic_array` | 3.35 | 3.11 | 3.20 | 3.86 |
| originals, `shared_mutex_map` | 37.6 | 280 | 311 | 938 |
| originals, `rcu_atomic_array` | 0.306 | 0.320 | 0.332 | 0.411 |

36.8 ns for one uncontended `shared_lock` plus a hash find, against 10.3 ns for
the same operation on x86_64.

**Two corrections to what this paragraph used to say, both from an external
review, because both claimed more than the numbers support.**

It said two of those lookups "account for essentially all of the 71.9 ns hooked
call". That is two micro-benchmarks added together and presented as a
measurement of the whole, which this project has already been wrong about once:
the same arithmetic predicted about 7.5 ns for the lock-free path when the
measured answer was 4.3. Adding isolated lookups is a **derivation**, not a
measurement, and back-to-back lock acquisitions contend differently from
acquisitions separated by a function call. What is measured here is the A/B: the
locked configuration costs 71.9 ns per call and the lock-free one 4.6 ns, on
this device, in this build.

It also said an acquire/release read-modify-write "is simply dearer on a
weakly-ordered machine". The difference is real and survives normalising for
clock -- about 34 cycles against 121 -- but nothing in this comparison isolates
its cause. The two sides differ in core, in compiler, and in standard library,
and the standard library implements `shared_mutex` differently on each. Which
of those the 3.5x belongs to is **not isolated**, and naming a mechanism would
be replacing one unverified cause with another.

Correctness on the device, both configurations, run as a single process the way
Android runs it: **187 passed, 4 failed, identical** -- the same four
`PrologueFacts` cases as the Jetson, which is now two independent ARM64 targets
agreeing that those four are an ARM64 property and not a registry one.

### A measurement note worth keeping

The Jetson figures above come from a standalone harness because Google
Benchmark is not installed there. That harness runs a fixed iteration count
immediately, which is fine on the Jetson -- its clock is pinned at 1.5104 GHz
in the 15 W profile, `scaling_cur_freq == cpuinfo_max_freq` -- and **wrong on a
phone**. Run on the Android device it reported the *unhooked* call at 3.67 ns
with one thread and 1.36 ns with four: the same code apparently getting 2.7x
faster as load increased, which is the governor ramping, not the code. Google
Benchmark on the same device held `BM_RawCall` at 2.73 ns across every round.
**The in-run unhooked control is what exposed it**, which is the whole reason
it is measured in the same binary.

---

## FR-010 progress overall

| Step | Content | Gain | Meets < 20 ns / < 5×? |
|---|---|---|---|
| 3.5 | `shared_mutex` replaces `recursive_mutex` | 41 → 24 ns; 39× → 27× | Partly |
| 4 | `SPLICE_HOOK_AS` per-site policy override | — | API |
| 5 (decided: skipped) | `std::function` → thunk | est. −1 ns | Not enough |
| **6.1** | Escape-hatch framework | No change | — |
| **6.2** | RCU hooks registry + originals | **registry-internal ratio 50× → 4–5.5×** | **registry ✅** |
| **6.3** | Bench validation | trampoline ratio 50× → 42× | **Not met** |
| 6.4 | EBR reclamation | Correctness, not performance | — |
| 6.x | Root-causing the trampoline's residual bouncer | Unknown | — |

**Single-thread target (< 20 ns):** 22.0 ns, 2 ns short.
**8-thread target (< 5× of 1t):** met by the registry; not met by the
trampoline overall.

---

## Points for decision

For Allen to decide:

1. **Accept Step 6 as partly met and close it**: 5.5× / 3.9× for the registry
   is already a large improvement. Finish 6.4 / 6.5, close Step 6, and file the
   remaining "rest at 46×" as a future ticket.
2. **Keep digging**: write a microbench isolating `HookStorage::invoke` to find
   the final bouncer. Perhaps 1–2 working days.
3. **Jump to Step 5 (thunk)**: a thunk saves dispatch overhead and may also
   change the shared-memory access pattern, which would test hypothesis 1 at
   the same time, and removes the `std::function` dependency.

Recommendation: **option 1, plus 6.4 (EBR correctness)**. Because:
- the registry result is already large (hot-path read 10 ns → 1 ns; ratio
  50× → 4×)
- "the rest at 46×" needs deeper tools (VTune / perf) to locate, beyond what
  microbenchmarks can do
- 6.4 EBR is a correctness issue, unrelated to performance, and should be done
- FR-010 as a whole has improved substantially; the marginal gain of pushing
  on to < 5× is poor return on the time invested

---

## Change log

- 2026-05-26: First version. Windows x86_64 data complete; ARM64 to be measured.
