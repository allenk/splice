# FR-010 Benchmark Baseline

> Numbers captured before any FR-010 work lands. Anchor for measuring
> the policy-framework improvements (Steps 3-5).
>
> **Date**: 2026-05-09
> **Build**: `windows-x64-bench` (Release · MSVC 14.44 · Ninja)
> **Hardware**: AMD Ryzen 9 9950X3D / 32 logical cores @ 4.3 GHz, L1d 48 KB / L1i 32 KB / L2 1 MB / L3 96 MB
> **Splice rev**: `master` after Task #57 (InstallerToken RAII shipped)
> **Target**: `splice::bench::hot_target(int)` — 5-instruction integer hash, noinline, separate TU
> **Bench tool**: Google Benchmark, `--benchmark_min_time=1.0s`, `_unset_` for repetitions

---

## Single-thread overhead (`bench_hook_overhead`)

```
---------------------------------------------------------------
Benchmark                     Time             CPU   Iterations
---------------------------------------------------------------
BM_RawCall                 2.53 ns         2.51 ns    560000000
BM_HookedCall_Active       41.1 ns         41.3 ns     34461538
```

| Curve | ns/call | Δ vs Raw |
|-------|---------|----------|
| Raw (no hook) | **2.53** | — |
| Hooked passthrough | **41.1** | **+38.6 ns** |

**FR-010 gate**: hooked call < 20 ns/call.
**Current shortfall**: 41.1 ns is **2.06×** over budget. Of the 38.6 ns hook overhead, the v1.4 review attributes:

- ~10-15 ns to `std::function` type erasure (heap-allocated invoker stub + 2 indirect calls)
- ~15-20 ns to `recursive_mutex::lock`/`unlock` × 3 (HookContext::get_hook + Hook::invoke + HookContext::get_original)
- ~5-10 ns to trampoline / atomic patch site / cache effects (immutable cost — survives FR-010)

Improvements #2 (policy framework, default `rcu_writeonce`) and #3 (template callback) target the first two.

---

## Multi-thread contention (`bench_hook_contended`)

```
----------------------------------------------------------------------------
Benchmark                                  Time             CPU   Iterations
----------------------------------------------------------------------------
BM_RawCall_Contended/threads:1          2.48 ns         2.48 ns    560000000
BM_RawCall_Contended/threads:2          2.56 ns         2.57 ns    560000000
BM_RawCall_Contended/threads:4          3.13 ns         3.17 ns    512000000
BM_RawCall_Contended/threads:8          8.04 ns         7.96 ns    182458184

BM_HookedCall_Contended/threads:1       40.1 ns         39.9 ns     34461538
BM_HookedCall_Contended/threads:2        108  ns         109  ns     12800000
BM_HookedCall_Contended/threads:4        319  ns         307  ns      4480000
BM_HookedCall_Contended/threads:8       1568  ns        1535  ns       896000
```

### Hooked scaling vs single-thread baseline

| Threads | Raw ns/call | Hooked ns/call | Hooked ratio |
|---------|-------------|-----------------|--------------|
| 1 | 2.48 | 40.1 | **1.00×** |
| 2 | 2.56 | 108  | 2.69× |
| 4 | 3.13 | 319  | 7.96× |
| 8 | 8.04 | 1568 | **39.1×** |

**FR-010 gate**: 8-thread ≤ 5× single-thread.
**Current shortfall**: **39.1× — almost 8× over budget**.

Per-call cost climbs **superlinearly** with thread count, the diagnostic
fingerprint of futex-backed mutex contention. Each contended `lock()` falls
into a kernel-mode futex wait once `recursive_mutex` detects another holder;
at 8 threads the syscall + reschedule cost dominates.

The Raw curve also degrades 1 → 8 (2.48 ns → 8.04 ns) — pure cache-line
ping-pong on the noinline target's body. That floor is unavoidable for
cross-CPU shared code, and it is what the post-FR-010 hook overhead should
asymptote to.

---

## Step 3 + 3.5 results (FR-010 policy framework + shared_mutex registry, 2026-05-09)

After landing `splice::policy::rcu_writeonce` as the default `HookStorage`
specialisation **and** converting `HookContext::m_mutex` from
`recursive_mutex` to `shared_mutex` (read paths take `shared_lock`,
`get_hook` uses a shared-lock fast path with unique-lock fallback on
miss).

### Single-thread

| Curve | Baseline | Step 3 (Hook<> only) | Step 3.5 (+ registry) | Δ vs baseline |
|-------|----------|-----------------------|------------------------|---------------|
| Raw (no hook) | 2.53 ns | 2.43 ns | **2.45 ns** | — |
| Hooked passthrough | 41.1 ns | 29.6 ns | **23.7 ns** | **−42%** |
| Hook overhead Δ | +38.6 ns | +27.2 ns | **+21.3 ns** | **−45%** |

**Gate**: < 20 ns. Current: 23.7 ns. Remaining 3.7 ns over budget is
`std::function`'s heap-erased invoker stub on the dispatch path —
removed in Step 5 (template Callback).

### 8-thread contended

| Threads | Raw | Hooked (baseline) | Hooked (Step 3.5) | 8t/1t ratio |
|---------|-----|-------------------|---------------------|-------------|
| 1 | 2.47 ns | 40.1 ns | **25.6 ns** | 1.00× |
| 2 | 2.62 ns | 108  ns | **51.5 ns** | 2.01× |
| 4 | 3.74 ns | 319  ns | **201  ns** | 7.85× |
| 8 | 11.3 ns | 1568 ns | **688  ns** | **26.9×** |

**Gate**: 8t ≤ 5× 1t. Current 26.9×. The remaining contention is the
`shared_mutex` reader counter — every reader still does an atomic
inc/dec on the same cache line, which ping-pongs between cores.
`policy::rcu_writeonce` removed mutex contention on `Hook<>::invoke`
itself, but the registry-side `shared_mutex` is now the dominant
contention source.

Per-call aggregate throughput at 8 threads:

| | Baseline | Step 3.5 |
|--|----------|----------|
| Aggregate ns/call (across 8 threads) | 196 ns | **86 ns** |
| Aggregate ops/sec | 5.1 M | **92.8 M** |
| Per-thread ops/sec | 0.64 M | **11.6 M** |

**~18× aggregate throughput improvement** — the futex-syscall path that
dominated `recursive_mutex` is gone, but cache-line contention on the
shared_mutex reader counter remains.

### Predicted Step 5 numbers (template Callback)

`std::function` adds two indirect calls + capture-pointer load on every
hook fire. Eliminating it should drop single-thread to ~12-15 ns range
and 8-thread to ~50-100 ns / thread. Gate clearance becomes plausible.

Beyond Step 5, the only way to push 8t/1t ratio below 5× is **true RCU
on the registry** (`m_originals` + `m_hooks` as immutable atomic-pointer
tables). That is Step 6 and **conditional** — only attempted if a real
consumer needs it.

---

## Predicted post-FR-010 numbers

Order-of-magnitude estimates based on the policy framework design in
`docs/v2-design-rationale.md` §Improvement #2:

| Metric | Current | Target | Predicted (`rcu_writeonce` + template Callback) |
|--------|---------|--------|-------------------------------------------------|
| Single-thread hooked | 41.1 ns | < 20 ns | **8-12 ns** |
| 8-thread hooked | 1568 ns | < 5× single | **30-50 ns (~4×)** |
| Throughput at 8 threads | 5.1 M calls/s | — | **160-260 M calls/s** |

The dominant gain comes from removing the futex path entirely:
`rcu_writeonce` replaces all three `recursive_mutex` acquisitions with a
single atomic-acquire load; cache-line ping is bounded by reads of an
*immutable* pointer (after install completes), so the line stays in
shared state in every reader's L1.

---

## How to reproduce

```powershell
# Configure (vcpkg pulls google-benchmark via the 'benchmarks' manifest feature)
cmake --preset=windows-x64-bench

# Build
cmake --build --preset=windows-x64-bench

# Run
./out/build/windows-x64-bench/benchmark/bench_hook_overhead.exe   --benchmark_min_time=1.0s
./out/build/windows-x64-bench/benchmark/bench_hook_contended.exe  --benchmark_min_time=1.0s
```

Notes:

- **Release build is non-negotiable.** Debug numbers are an order of magnitude slower and not comparable.
- **Run on a quiet machine.** The 8-thread bench is sensitive to anything else competing for cores. Disable Turbo Boost (`powercfg /SetActive SCHEME_MIN`) if you want the lowest variance.
- **Don't rely on a single run.** `--benchmark_repetitions=5 --benchmark_report_aggregates_only=true` gives mean/median/stddev. We capture a single run here for traceability; CI guard work in Step 6 will codify multi-run statistics.
- **Pre-FR-010 inline-disable warning is expected.** `install_inline_patch: copy_size=17 > 16; disable will be unavailable` — `hot_target`'s prologue is 17 bytes, one over the atomic-disable threshold. Hook is still installed and benched correctly; only `disable()` for *that specific function* would fall back to the non-atomic memcpy path.

---

## Why the bench target needs more than `return x + 1`

Initial attempt used `int hot_target(int x) { return x + 1; }`. MSVC /O2
ICF-folded that with other one-liners and the linker rewrote the prologue
as a RIP-relative branch. The Splice prologue relocator (`emit_prologue_copy`)
declines to copy `RipRelative` instructions into the trampoline because
the new location may exceed ±2 GB of the original target — so the install
failed with `cannot relocate type=6 at src_off=4`.

The current target body (xorshift-style integer hash, ~5 instructions) keeps
the prologue in plain `Regular` territory while still being only a few
cycles per call. **Future bench targets should mirror this discipline.**
A trivial `return x + 1` is a benchmark trap on Release builds; the cost
of the function body must be small **but not so small that the linker
can fold it**.

---

## Tracking

| Task | Status |
|------|--------|
| 2.1 — vcpkg dep + CMake wire-up | ✅ Done |
| 2.2 — `bench_hook_overhead` | ✅ Done |
| 2.3 — `bench_hook_contended` | ✅ Done |
| 2.4 — Capture baseline (this doc) | ✅ Done |
| Step 3 — Policy framework + `rcu_writeonce` default | Pending |
| Step 4 — `policy::shared_mutex` + `SPLICE_HOOK_AS` | Pending |
| Step 5 — Template Callback | Pending |
| Step 6 — `policy::rcu_refcounted` (optional) | Deferred |
| CI regression guard | Pending |

Re-run this bench at the end of Steps 3, 4, 5 and append the new column
to the result tables. Don't overwrite — the trail of numbers is the
audit story.
