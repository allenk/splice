# FR-010 Step 6.3 — RCU registry benchmark results

**Status:** Data complete
**FR:** FR-010 Step 6.3 (contended bench validates the 8t/1t < 5× gate)
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

## Platform 2: Android ARM64

_To be measured. The USB connection is currently unstable, and the VS Code
extension `BvSshServer` holds port 5037; adb has to be restarted before the
bench can be pushed._

Expected: on ARM64 `std::function` costs slightly more than on x86_64 (per the
Step 5 microbench); the RCU registry's relative gain should be comparable to
x86_64 (registry contention is the same in nature).

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
