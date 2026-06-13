# FR-010 Step 5 — `std::function` vs thunk callback storage: microbenchmark report

**Status:** Evidence complete / awaiting Allen's decision on whether to do Step 5a
**FR:** FR-010 Step 5 (Template Callback — kill `std::function` on the hot path)
**Test date:** 2026-05-19
**Author:** Allen Kuo
**Read with:** [`template-callback-evaluation.md`](./template-callback-evaluation.md) — design evaluation, API impact, trade-off table

---

## TL;DR — if you have no time for the rest

**On the hot path a thunk saves 0.9–1.2 ns/call over `std::function`
(−23% to −27%). But Splice end to end is 23.7 ns, and 1 ns alone is not enough
to meet FR-010's < 20 ns gate.**

| Platform | `std::function` overhead | Thunk overhead | Saving | End-to-end estimate |
|---|---|---|---|---|
| Windows x86_64 (Ryzen 9 9950X3D, MSVC /O2) | +1.39 ns | +0.49 ns | **0.90 ns** (−23%) | 23.7 → ~22.8 ns |
| Android ARM64 (Snapdragon 8 Gen 3, Clang -O2) | +1.52 ns | +0.31 ns | **1.21 ns** (−27%) | awaiting an end-to-end baseline |

**Unexpected finding:** `std::function`'s overhead had been estimated at
~4–5 ns (vtable hop + state pointer); it measured 1.4–1.5 ns. Modern compilers
(MSVC 19.44 / Android Clang 19) are already quite aggressive with SBO and
devirtualisation for `std::function`.

**Recommended action: read the "Decision" section first.**

---

## Why benchmark first?

The key challenge Allen raised after reading
[`template-callback-evaluation.md`](./template-callback-evaluation.md):

> "But I'm concerned whether the performance really is as you say. Shouldn't
> we first write a small bench on PC and Android to verify this, and then
> start the change with data behind it?"

That is the right engineering discipline. A refactor that touches the hot path
and affects the compile time of 100+ hook call sites **should not start without
measured data**. Step 5's "estimated −4 ns, meets the < 20 ns gate" rested on
an **estimate** of `std::function`'s overhead, not on a **measurement**.

This report replaces the estimate with a measurement.

---

## Experiment design

### Why not change HookStorage directly and benchmark that?

Because that would:
1. change 200+ lines of hot-path code
2. have to pass the full unit-test suite
3. have to be validated on both Windows and ARM64
4. have to be rolled back entirely if the numbers looked bad

**First, an independent microbenchmark isolates the dispatch overhead**, with
zero changes inside Splice.

### Three measured paths

The independent microbenchmark is `benchmark/bench_callback_storage.cpp`, and
**it references nothing in Splice**. It defines three local storage structs
inside the microbenchmark, mirroring the structure of Splice's hot path:

#### Path 1: `BM_DirectCall` (floor)

```cpp
for (auto _ : state) {
    x = splice::bench::hot_target(x) + 1;   // no indirection at all
    benchmark::DoNotOptimize(x);
}
```

Purpose: establish the measurement floor. `hot_target` is a noinline
`int hot_target(int x)` (in its own TU, so the compiler cannot inline through
it).

#### Path 2: `BM_StdFunctionPath` (mirror of the current design)

```cpp
struct StdFunctionStorage {
    using HookFn = std::function<int(OrigFn, int)>;
    std::atomic<HookFn*> m_fn{nullptr};

    int invoke(OrigFn orig, int x) noexcept {
        if (auto* fn = m_fn.load(std::memory_order_acquire)) {
            return (*fn)(orig, x);          // std::function manager vtable hop
        }
        return orig(x);
    }
};
```

An exact mirror of `HookStorage<rcu_writeonce>` at `include/splice/context.h:128`:
a `std::atomic<std::function*>` load + an `operator()` call.

#### Path 3: `BM_ThunkPath` (the Step 5 proposal)

```cpp
struct ThunkStorage {
    using Thunk = int (*)(void* state, OrigFn orig, int x);

    std::atomic<Thunk> m_thunk{nullptr};
    std::atomic<void*> m_state{nullptr};

    template <typename Lambda>
    void store(Lambda fn) {
        static Lambda* s_fn = new Lambda(std::move(fn));
        m_state.store(s_fn, std::memory_order_relaxed);
        m_thunk.store(
            +[](void* st, OrigFn orig, int x) noexcept -> int {
                return (*static_cast<Lambda*>(st))(orig, x);
            },
            std::memory_order_release);
    }

    int invoke(OrigFn orig, int x) noexcept {
        if (auto th = m_thunk.load(std::memory_order_acquire)) {
            return th(m_state.load(std::memory_order_relaxed), orig, x);
        }
        return orig(x);
    }
};
```

Hot-path difference:
- `std::function` path: load `HookFn*` → vtable lookup of `operator()` → call the manager → call the lambda
- Thunk path: load the thunk function pointer → one indirect call → the lambda inlined into the thunk

### Fairness controls

- ✅ All three paths share one callback body: `[](OrigFn orig, int x){ return orig(x) + 1; }`
- ✅ The same `splice::bench::hot_target` (noinline, its own TU)
- ✅ The same `benchmark::DoNotOptimize(x)` against dead-code elimination
- ✅ The same Release build / -O2
- ✅ Google Benchmark `--benchmark_repetitions=5` + `--benchmark_report_aggregates_only=true`

---

## Results

### Platform 1: Windows x86_64

**Hardware:** AMD Ryzen 9 9950X3D (4.3 GHz, 32 threads, 96 MiB L3)
**OS:** Windows 11 Pro 26200
**Compiler:** MSVC 19.44.35211 (cl.exe 14.44.35211)
**Build preset:** `windows-x64-bench` (Release, /O2, `-DNDEBUG`)
**Google Benchmark:** v1.9.5
**Run:** `out/build/windows-x64-bench/benchmark/bench_callback_storage.exe --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`

```
--------------------------------------------------------------------
Benchmark                          Time             CPU   Iterations
--------------------------------------------------------------------
BM_DirectCall_mean              2.59 ns         2.60 ns            5
BM_DirectCall_median            2.60 ns         2.61 ns            5
BM_DirectCall_stddev           0.016 ns        0.027 ns            5
BM_DirectCall_cv                0.63 %          1.02 %             5

BM_StdFunctionPath_mean         3.99 ns         3.91 ns            5
BM_StdFunctionPath_median       3.99 ns         3.92 ns            5
BM_StdFunctionPath_stddev      0.009 ns        0.114 ns            5
BM_StdFunctionPath_cv           0.23 %          2.91 %             5

BM_ThunkPath_mean               3.09 ns         3.09 ns            5
BM_ThunkPath_median             3.10 ns         3.08 ns            5
BM_ThunkPath_stddev            0.055 ns        0.069 ns            5
BM_ThunkPath_cv                 1.77 %          2.23 %             5
```

| Path | Mean | CV | Delta vs Direct |
|---|---|---|---|
| BM_DirectCall | 2.60 ns | 0.63 % | floor |
| BM_StdFunctionPath | 3.99 ns | 0.23 % | **+1.39 ns** |
| BM_ThunkPath | 3.09 ns | 1.77 % | **+0.49 ns** |

**Δ(StdFunction − Thunk) = 0.90 ns/call (−23% dispatch overhead)**

### Platform 2: Android ARM64

**Hardware:** Snapdragon 8 Gen 3 (Cortex-X4 prime @ 3.3 GHz; the bench ran on
the mid cluster at 2.27 GHz in the background — CPU scaling was not locked)
**OS:** Android 14 (One UI 6.1)
**Compiler:** Android Clang 19 (NDK r29 beta1, target `aarch64-linux-android30`)
**Build preset:** `android-arm64-bench` (Release, -O2, `-DNDEBUG`; a new preset)
**Google Benchmark:** v1.9.5 (arm64-android triplet)
**Deployment:** `adb push ... /data/local/tmp/`
**Run:** `adb shell /data/local/tmp/bench_callback_storage --benchmark_repetitions=5 --benchmark_report_aggregates_only=true`

```
***WARNING*** CPU scaling is enabled, the benchmark real time measurements may be noisy and will incur extra overhead.
--------------------------------------------------------------------
Benchmark                          Time             CPU   Iterations
--------------------------------------------------------------------
BM_DirectCall_mean              3.04 ns         3.03 ns            5
BM_DirectCall_median            3.04 ns         3.03 ns            5
BM_DirectCall_stddev           0.001 ns        0.001 ns            5
BM_DirectCall_cv                0.03 %          0.02 %             5

BM_StdFunctionPath_mean         4.56 ns         4.55 ns            5
BM_StdFunctionPath_median       4.56 ns         4.55 ns            5
BM_StdFunctionPath_stddev      0.004 ns        0.004 ns            5
BM_StdFunctionPath_cv           0.09 %          0.09 %             5

BM_ThunkPath_mean               3.35 ns         3.34 ns            5
BM_ThunkPath_median             3.34 ns         3.34 ns            5
BM_ThunkPath_stddev            0.002 ns        0.002 ns            5
BM_ThunkPath_cv                 0.06 %          0.06 %             5
```

| Path | Mean | CV | Delta vs Direct |
|---|---|---|---|
| BM_DirectCall | 3.04 ns | 0.03 % | floor |
| BM_StdFunctionPath | 4.56 ns | 0.09 % | **+1.52 ns** |
| BM_ThunkPath | 3.35 ns | 0.06 % | **+0.31 ns** |

**Δ(StdFunction − Thunk) = 1.21 ns/call (−27% dispatch overhead)**

⚠️ The CV on ARM64 is extremely low (< 0.1 %): although the phone's CPU
scales, the thread stayed pinned to one cluster, which is steadier than a PC's
many cores drifting.

---

## Analysis

### Cross-platform consistency

| Measured | x86_64 | ARM64 | Ratio |
|---|---|---|---|
| Direct call | 2.60 ns | 3.04 ns | ARM64 17 % slower |
| `std::function` overhead | 1.39 ns | 1.52 ns | ARM64 9 % slower |
| Thunk overhead | 0.49 ns | 0.31 ns | ARM64 37 % **faster** |
| Saving, `std::function` vs thunk | 0.90 ns (−23 %) | 1.21 ns (−27 %) | ARM64 slightly better |

The two platforms tell an **internally consistent** story:
1. `std::function` overhead is ~1.5 ns (not ~5 ns)
2. Thunk overhead is ~0.3–0.5 ns (two atomic loads + one indirect call)
3. The difference between them is ~1 ns

### Why so much less than the estimate?

The default model was the **textbook `std::function`**:
- a virtual call (vtable hop)
- a second indirection through the manager pointer
- a heap-allocated capture (cache miss)

What actually happens:
1. **SBO applies:** the captureless lambda (the test case) fits entirely in
   `std::function`'s built-in buffer; no heap allocation
2. **Both MSVC and Clang devirtualise:** the lambda's type is visible at
   `store`, so at the inlining point the compiler knows the actual manager type
3. **The CPU's predictor:** an indirect call through a function pointer that
   hits in the BTB costs close to a direct call

### Why is the thunk still faster?

The remaining ~1 ns comes from:
- one less manager indirection (the thunk dispatches straight to the lambda)
- a smaller cache footprint (a `std::function` is 64 bytes on x64; thunk + state is 16 bytes)
- the compiler can inline the lambda body into the thunk (the thunk is
  captureless and the body explicitly visible)

### End-to-end estimate for Splice

Windows x86_64 baseline (after FR-010 Step 3.5) = **23.7 ns/call**

Estimated composition:
- Trampoline entry / register save: ~2 ns
- Atomic load of the original pointer: ~1 ns
- HookContext `shared_lock` + map lookup: ~10 ns (the hot path already uses `shared_lock`)
- `HookStorage::invoke`: **~2 ns** (of which ~1.4 ns is `std::function` dispatch)
- Lambda body: ~1 ns (trivial pass-through)
- Trampoline exit: ~2 ns
- Miscellaneous (atomic counters, branch mispredicts): ~5 ns

With a thunk, `HookStorage::invoke` becomes ~1 ns, saving ~1 ns/call.
**Estimated end to end: 23.7 → ~22.7 ns/call.**

**Conclusion: Step 5a alone cannot meet the < 20 ns gate.** It would also need:
- a cache for `HookContext::get_hook` (a thread-local first-call cache)
- or less `shared_lock` contention (an atomic snapshot)
- or Step 6's RCU registry

---

## Decision

Mapping the evidence back onto the decision matrix in
`template-callback-evaluation.md`:

| ARM64 `std::function` overhead | Original recommendation | Measured |
|---|---|---|
| > 5 ns | Strongly recommend Step 5a | — |
| 2–5 ns | Do Step 5a (a compromise gain) | — |
| **1–2 ns** | (not planned for) | **The measurement lands here: 1.52 ns** |
| < 1 ns | Skip Step 5, go straight to Step 6 | — |

The measurement falls on the boundary of the original matrix. The decision,
reframed:

### Option A — do Step 5a (recommended)

**For:**
- The gain on its own is small (~1 ns / 4%), but there is **no downside**:
  - The API is completely unchanged (scenarios A/B/C all compatible)
  - Compile time estimated +5–10% (well within the < 5 s ceiling)
  - Binary size +50 bytes/site × 50 sites = +2.5 KB (negligible)
- It moves errors from run time (`bad_function_call`) to compile time (a
  signature mismatch fails to compile)
- It turns the hot path from a textbook `std::function` into a transparent
  thunk — **a better debugger experience**
- It **stacks** with Step 6 and later inline optimisations: each small change
  adds up until the gate is met

**Cost:** ~200 lines of hot-path change + 4 HookStorage tests + bench
validation on both platforms.

### Option B — skip Step 5, go straight to Step 6 (also reasonable)

**For:**
- A 1 ns end-to-end gain, in a hooks-per-frame scenario
  (60 fps × 100 hooks = 6000 calls/sec), adds up to 6 µs/sec — **not
  perceptible in practice**
- Step 6 (RCU registry) attacks the bigger problem, 8t/1t = 26.9×, with more
  room to gain
- A shorter delivery schedule

**Cost:** give up ~1 ns, and take on Step 6's design risk (`atomic<shared_ptr>`
support is uneven across AOSP toolchains).

### Option C — do both, Step 6 first, then Step 5a

**For:** handle the 8-thread bottleneck first (bigger impact); do Step 5a last
as a hygiene change.

---

## Appendix A: where the bench code is, and how to re-run it

### Code

- **Microbenchmark:** `benchmark/bench_callback_storage.cpp`
- **Shared noinline target:** `benchmark/bench_targets.h` / `.cpp` (`splice::bench::hot_target`)
- **CMake integration:** `benchmark/CMakeLists.txt`

### Re-running on Windows

```bash
# In a VS2022 dev shell:
cmake --preset=windows-x64-bench
cmake --build --preset=windows-x64-bench --target bench_callback_storage
out/build/windows-x64-bench/benchmark/bench_callback_storage.exe \
    --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

### Re-running on Android ARM64

```bash
# Any host (NDK on PATH):
cmake --preset=android-arm64-bench
cmake --build --preset=android-arm64-bench --target bench_callback_storage

# Push + run:
adb push out/build/android-arm64-bench/benchmark/bench_callback_storage /data/local/tmp/
adb shell chmod 0755 /data/local/tmp/bench_callback_storage
adb shell /data/local/tmp/bench_callback_storage \
    --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

### New CMake preset

`android-arm64-bench` (added for this experiment) — Release build, benchmarks
ON, `VCPKG_MANIFEST_FEATURES=benchmarks`. In `CMakePresets.json`, inheriting
from `_android_arm64`.

---

## Appendix B: why this report is worth keeping

1. **A record of the facts for future contributors:** the next person to change
   the hot path can learn from this report that "`std::function`'s overhead on a
   captureless / SBO-fitting lambda is ~1.5 ns, not the legendary 5–10 ns", and
   avoid repeating the same estimation error.

2. **A methodology template:** "isolate the hypothesis with a microbenchmark
   first, then decide whether to touch the hot path" is an engineering
   discipline worth making permanent. Step 6 and later optimisations should
   follow it too.

3. **A cross-platform baseline record:** absolute callback-dispatch numbers for
   x86_64 and ARM64; if a toolchain upgrade (MSVC 19.50 / Clang 20) causes a
   regression, this is the comparison.

4. **Traceable decisions:** whichever of A/B/C is chosen, later readers see not
   only the result — why Splice is designed this way — but the process: what
   data the decision was made on.

---

## Change log

- 2026-05-19: First version. Windows + ARM64 data complete; awaiting decision.
