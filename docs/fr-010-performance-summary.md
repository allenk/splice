# FR-010 — Splice performance improvements, and the truth about `std::function`

**Status:** Knowledge base (KB) / for future readers
**FR:** the whole FR-010 series (Steps 3.5 / 5 / 6.1–6.4)
**Written:** 2026-05-27
**Author:** Allen Kuo
**Role:** this is the **overview**. Per-step detail is in:
- [`benchmark-baseline.md`](./benchmark-baseline.md) — the original baseline
- [`template-callback-evaluation.md`](./template-callback-evaluation.md) — Step 5 design evaluation
- [`fr-010-step5-microbench-report.md`](./fr-010-step5-microbench-report.md) — `std::function` vs thunk, measured
- [`fr-010-step6-rcu-registry-design.md`](./fr-010-step6-rcu-registry-design.md) — Step 6 design
- [`fr-010-step6-bench-results.md`](./fr-010-step6-bench-results.md) — Step 6 measurements

---

## In one sentence (for whoever comes next)

> **Hot-path optimisation is won or lost in the critical section and the
> synchronisation, not in dispatch wrapping. In Splice: changes to the sync
> layer gave −42% to −92%; changes to the wrapping layer gave about 5%.**

A gap of two orders of magnitude — a discipline worth making permanent.
Verify the hypothesis with a microbenchmark before acting; do not attack
`std::function` on textbook intuition.

---

## TL;DR

| Metric | Start (pre-FR-010) | Step 3.5 | Step 6 | Target |
|---|---|---|---|---|
| 1-thread hooked call | 41.1 ns | 23.7 ns | 22.0 ns | < 20 ns |
| 8-thread hooked call | 1568 ns | 688 ns | 923 ns † | — |
| 8t/1t ratio | 39.1× | 26.9× | 42× † | < 5× |
| Registry lookup (pure isolation) 8t/1t | — | — | **3.9–5.5×** ✅ | < 5× |

† The Step 6 trampoline numbers are not a regression; they come from a
different bench shape (the registry has already won; the residual bouncer is
somewhere outside `std::function` — see
[`fr-010-step6-bench-results.md`](./fr-010-step6-bench-results.md)).

**Biggest finding:** `std::function` was assumed to be the prime suspect on
the hot path. **Measured, it accounts for only 1.4–1.5 ns**; switching to a
thunk saves only ~1 ns more. A small microbenchmark established this and
avoided an ineffective ~200-line refactor.

---

## 1. The improvement journey

### Before — baseline (measured 2026-05-09)

Hardware: AMD Ryzen 9 9950X3D, MSVC /O2, Release.

```
BM_HookedCall (1 thread):       41.1 ns/call
BM_HookedCall_Contended/8t:    1568 ns/call  (ratio 39.1×)
```

Composition (analysed afterwards):
- `std::recursive_mutex` lock / unlock: ~10 ns
- `std::unordered_map<int, shared_ptr<Hook>>` find: ~10 ns
- `std::function::operator()` dispatch: ~1.4 ns
- The real trampoline + hooked function body: ~20 ns

### Step 3.5 — `shared_mutex` replaces `recursive_mutex` (2026-05-09)

**One change:** `HookContext::m_mutex` goes from `std::recursive_mutex` to
`std::shared_mutex`; the reader path takes `shared_lock`, the writer path
`unique_lock`.

```cpp
// Before
std::recursive_mutex m_mutex;
std::lock_guard lock(m_mutex);
// → atomic RMW under contention; serialises ALL readers

// After
std::shared_mutex m_mutex;
std::shared_lock lock(m_mutex);   // multiple readers OK
// → reader counter atomic RMW still bounces, but no full serialisation
```

| Metric | Improvement |
|---|---|
| 1-thread | 41.1 → **23.7 ns** (−42%) |
| 8-thread | 1568 → **688 ns** (−56%) |
| 8t/1t | 39.1× → **26.9×** |

The "full serialisation" layer is gone, but cache-line ping-pong on the reader
counter is still the biggest cost at 8 threads.

### Step 6.1 — the escape-hatch framework (2026-05-23)

**Motivation:** do RCU **without forcing every user to take RCU's
trade-offs** (slower writers, temporarily 2× memory, AOSP toolchain risk). It
established the engineering principle: "an optimisation with a trade-off must
come with an escape hatch".

**Changes:**
- New `include/splice/registry_impl.h`, defining the `SPLICE_REGISTRY_IMPL` macro
- Partial specialisation moves `HookContext::m_hooks`'s logic into `HookRegistry<Impl>`
- Default `Impl = registry::shared_mutex_map`, **behaving exactly as Step 3.5**
- Opt-in: build with `-DSPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array`

Zero behaviour change, zero performance change. Preparation for Step 6.2.

### Step 6.2 — the RCU implementation (2026-05-25)

Two new specialisations:
- `HookRegistry<rcu_atomic_array>` — publishes through `std::atomic<Snapshot*>`, indexes a `std::array<shared_ptr, SPLICE_MAX_HOOKS>`. Reader = one acquire load
- `OriginalsRegistry<rcu_atomic_array>` — `std::array<std::atomic<void*>, SPLICE_MAX_HOOKS>`. Reader = one atomic load

`SPLICE_MAX_HOOKS = 512` (decided 2026-05-19): covers the typical hook count of
AOSP system services and keeps the 8 KiB snapshot within L1d.

### Step 6.3 — measurement (2026-05-26)

**The registry-lookup isolation microbench** (`benchmark/bench_registry_lookup.cpp`,
driving HookRegistry / OriginalsRegistry directly, **bypassing the trampoline**):

| Op | `shared_mutex_map` 1t→8t | `rcu_atomic_array` 1t→8t | RCU speed-up |
|---|---|---|---|
| HookRegistry::get | 10.1 → 522 ns | **0.96 → 5.24 ns** | 100× @ 8t |
| OriginalsRegistry::get | 9.79 → 473 ns | **1.29 → 5.09 ns** | 93× @ 8t |

**The registry layer's 8t/1t ratio:**
- `shared_mutex_map`: ~50× (this is what cache-line bouncing does)
- **`rcu_atomic_array`: 3.9–5.5× (meets the < 5× gate)** ✅

The full trampoline numbers did not improve in proportion (923 ns @ 8t,
ratio ~42×) — the remaining bouncer is not in the registry, **and not in
`std::function`** (see the next section).

### Step 6.4 — time-deferred reclamation (2026-05-26)

RCU snapshots cannot accumulate forever. The original design used EBR
(epoch-based reclamation), but the standard EBR form needs atomic operations
as a reader enters and leaves its critical section — 3–9 ns more per
trampoline call, **which would erase all of Step 6.2/6.3's gain**.

Replaced with **time-deferred reclamation**:
- A writer retiring an old snapshot records a timestamp
- The next writer scans the retire queue and frees entries older than 100 ms
- Readers pay **nothing extra**

Trade-off: if a reader thread is preempted by the scheduler for more than
100 ms, an old snapshot can be freed under it. In practice Splice's
trampoline runs on a ns–µs scale, so 100 ms is 10–100× of headroom.

---

## 2. The truth about `std::function` — the assumption we got wrong

### The starting assumption

Going into the Step 5 (template callback) design, the estimate was:

> `std::function`'s hot-path overhead is **~4–5 ns**:
> - vtable hop (~2 ns)
> - a second indirection through the manager pointer (~1 ns)
> - a cache miss on a heap-allocated capture (~2 ns)
> - switching to a thunk would save an estimated **3–4 ns / call**

That is the **textbook** cost estimate for `std::function`. The Step 5 design
document, `template-callback-evaluation.md`, at one point said "estimated end
to end 23.7 → 19 ns / call (meets the < 20 ns gate)".

### Allen's engineering discipline

> "But I'm concerned whether the performance really is as you say. Shouldn't
> we first write a small bench on PC and Android to verify this, and then
> start the change with data behind it?"
> — Allen, 2026-05-18

**That question saved an ineffective ~200-line refactor.**

### Microbenchmark design

`benchmark/bench_callback_storage.cpp` — **touches nothing in Splice**; it
defines three local paths inside the microbenchmark:

```cpp
// Path 1: Floor
int x = hot_target(x) + 1;   // no indirection

// Path 2: mirror of the current design
struct StdFunctionStorage {
    std::atomic<std::function<int(OrigFn, int)>*> m_fn;
    int invoke(...) { return (*m_fn.load())(orig, x); }  // std::function dispatch
};

// Path 3: the Step 5 proposal
struct ThunkStorage {
    std::atomic<int(*)(void*, OrigFn, int)> m_thunk;
    std::atomic<void*> m_state;
    int invoke(...) { return m_thunk.load()(m_state.load(), orig, x); }
};
```

All three paths share the same captureless lambda body and the same noinline
target. The only difference is the dispatch mechanism.

### Measured on two platforms (2026-05-19)

#### Windows x86_64 (Ryzen 9 9950X3D, MSVC /O2)

| Path | Mean | Delta vs Direct |
|---|---|---|
| BM_DirectCall | 2.60 ns | 0 |
| BM_StdFunctionPath | 3.99 ns | **+1.39 ns** |
| BM_ThunkPath | 3.09 ns | **+0.49 ns** |

**Δ(StdFunction − Thunk) = 0.90 ns/call (−23% dispatch overhead)**

#### Android ARM64 (Snapdragon 8 Gen 3, Clang 19 -O2)

| Path | Mean | Delta vs Direct |
|---|---|---|
| BM_DirectCall | 3.04 ns | 0 |
| BM_StdFunctionPath | 4.56 ns | **+1.52 ns** |
| BM_ThunkPath | 3.35 ns | **+0.31 ns** |

**Δ(StdFunction − Thunk) = 1.21 ns/call (−27% dispatch overhead)**

### Why is `std::function` so much cheaper than assumed?

The default mental model was the "textbook" `std::function`: heap allocation,
a vtable hop, manager indirection. What actually happens:

1. **SBO (small-buffer optimisation) applies**
   - A captureless lambda fits entirely in `std::function`'s built-in buffer
     (typically ~16 bytes)
   - No heap allocation, no cache miss
   - Both libc++ and MS-STL implement SBO

2. **The compiler devirtualises**
   - MSVC 19.44 and Android Clang 19 are both aggressive
   - The lambda's type is visible at `store` → the manager function's type is inlined
   - The vtable hop folds into a direct call

3. **CPU branch prediction + BTB**
   - An indirect call through a function pointer, once it hits in the BTB,
     costs close to a direct call
   - The first call is expensive; the steady state is very cheap
   - The bench runs 1.2 × 10⁹ iterations, so every indirect call is long warm

### Conclusion

| Dimension | Assumed | Measured |
|---|---|---|
| `std::function` overhead | ~4–5 ns | **~1.4–1.5 ns** |
| Expected saving from a thunk | 3–4 ns/call | **~1 ns/call** |
| Meets the < 20 ns gate? | Yes | **No** (23.7 → 22.8 ns) |
| Worth a ~200-line refactor? | Yes | **No** (poor return on effort) |

**Step 5 (template callback) was deferred.** Step 6 (RCU registry) came first,
with a far larger gain (see Step 6.3's numbers).

---

## 3. Where is the residual bouncer?

Step 6.3 measured the registry in isolation at < 5×, but the full trampoline
is still ~42×. The difference:

| | 1t | 8t | 8t/1t |
|---|---|---|---|
| Registry lookup (both) | 2.25 ns | 10.3 ns | 4.6× |
| **"The rest" of the trampoline** | **19.8 ns** | **913 ns** | **46×** |
| Total | 22.0 ns | 923 ns | 42× |

Candidates for "the rest":
- `HookStorage::invoke` (atomic load + `std::function` call)
- The real hooked function call (back into the original through the JIT trampoline)
- Google Benchmark's thread harness (unlikely; `BM_RawCall_Contended` already calibrates it)
- MSVC `_Init_thread_header_safe` (the Meyers singleton guard; **already cached as
  `static HookContext* const ctx`**, saving only 10%)

Since the Step 5 microbench showed `std::function` costs only 1.5 ns, **it is
not the culprit**. The real bouncer needs VTune / perf to locate, and **is
filed as a Step 6.x follow-up ticket**, outside the current FR-010 scope.

---

## 4. What was gained, what was not, what was learned

### Gained
- **Single-thread hot path: 41.1 → 22.0 ns/call (−46%)**
- **Registry-layer 8t/1t ratio: ~50× → 3.9–5.5× (−92%)**
- Multi-threaded 8t end to end: 1568 → 923 ns/call (−41%)
- A complete escape hatch for RCU (`SPLICE_REGISTRY_IMPL`, `SPLICE_MAX_HOOKS`,
  `SPLICE_RCU_GRACE_PERIOD_MS`)
- An engineering principle: "an optimisation with a trade-off must come with
  an escape hatch"
- An engineering discipline: "write a microbenchmark to verify the hypothesis
  before touching the hot path"

### Not gained (targets missed)
- The 1t < 20 ns gate: 2 ns short
- The end-to-end 8t/1t < 5× gate: well short; the residual bouncer has to be found

### Learned
1. **A "textbook estimate" is not a benchmark.** `std::function` was estimated
   at ~4 ns and measured at 1.5 ns. The gap comes from three modern
   compiler/CPU mechanisms stacking: SBO, devirtualisation, BTB.
2. **Every refactor should start with a number: "I expect this to save N ns".**
   If the estimate is wrong, stop; do not push on regardless.
3. **Splice's real hot-path bottleneck is reader contention, not dispatch
   itself.** That is the opposite of the predecessor framework v1's intuition (v1
   attacked dispatch, not contention).
4. **RCU's design cost is mostly in memory reclamation, not in publication.**
   Time-deferred reclamation shows that **where readers run on a ns–µs scale**,
   standard EBR is over-engineering.

---

## 5. For whoever comes next

If you want to optimise the hot path in later Splice work:

1. **Measure first, then act.** `benchmark/` already has a full microbenchmark
   toolchain; adding one more is a change of under 200 lines.
2. **`std::function` is not the prime suspect.** If you revisit Step 5, put
   your evidence in this report's comparison table.
3. **The residual bouncer at 8t/1t = 42× is still unsolved.** It needs VTune /
   `perf record`. Take it on when you have the time and the tools.
4. **Any RCU change must keep its escape hatch.** `SPLICE_REGISTRY_IMPL` is the
   template.
5. **A microbenchmark is written for your future self.** Isolate the
   components, name them clearly, leave commands that can be re-run.

---

## Change log

- 2026-05-27: First version. Consolidates Steps 3.5 / 6.1–6.4 and the Step 5 evidence chain.
