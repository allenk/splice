# FR-010 Step 6 — RCU registry design and escape-hatch evaluation

**Status:** Design proposal / awaiting Allen's decision to implement
**FR:** FR-010 Step 6 (a true RCU registry — close the 8t/1t < 5× gate)
**Written:** 2026-05-19
**Author:** Allen Kuo
**Read with:**
- [`template-callback-evaluation.md`](./template-callback-evaluation.md) — the Step 5 policy framework design
- [`fr-010-step5-microbench-report.md`](./fr-010-step5-microbench-report.md) — the Step 5 measurements
- [`benchmark-baseline.md`](./benchmark-baseline.md) — the overall FR-010 baseline
- [`v2-design-rationale.md`](./v2-design-rationale.md) §"Why not runtime-switchable"

---

## TL;DR

1. **Problem:** reader contention at 8 threads takes a hooked call from 23.7 ns
   to 688 ns/call (**a 26.9× degradation**; gate < 5×). The culprit is
   cache-line ping-pong on the reader counter of `HookContext::m_mutex`.
2. **Solution:** replace `m_hooks`' `unordered_map + shared_mutex` with an RCU
   snapshot — readers take no lock; writers atomically publish a new snapshot.
3. **Trade-off principle** (set by Allen): **every RCU change must have an
   escape hatch**. RCU is by nature a functional trade-off (writers pay more,
   old data is reclaimed late, peak memory is higher) and should not be imposed
   on every user.
4. **Escape-hatch design:** a build-time macro, `SPLICE_REGISTRY_IMPL`,
   parallel to Step 3's `SPLICE_DEFAULT_POLICY`. **The default keeps the
   current behaviour** (`shared_mutex_map`); `rcu_atomic_array` is opt-in. No
   API change.
5. **Expected gain:** 8-thread hooked **688 → ~50–80 ns/call** (bringing the
   < 5× gate within reach).

---

## Why Step 6

Step 5's microbenchmark showed that `std::function → thunk` saves only ~1 ns
end to end. **Splice's real bottleneck is not callback dispatch but reader
contention on the registry lookup.**

### The evidence for 8t/1t = 26.9×

From `docs/benchmark-baseline.md` (after FR-010 Step 3.5):

| Measured (Ryzen 9 9950X3D) | Value |
|---|---|
| 1-thread hooked call | 23.7 ns/call |
| 8-thread hooked call | 688 ns/call |
| **8t/1t ratio** | **26.9×** |
| FR-010 gate | < 5× |

`shared_mutex` looks as though it allows "lock-free" reads, but a reader still
has to atomically increment and decrement `m_reader_count`. With 8 threads
doing that at once, the cache line ping-pongs between 8 cores' L1 caches, and
every RMW causes MOESI invalidation traffic. **That is the nature of reader
contention.**

### Why RCU solves it

RCU's (Read-Copy-Update's) core promise: **on the hot path a reader does no
atomic RMW at all; it only reads an immutable snapshot.**

```cpp
// shared_mutex version (now)
Hook& get_hook(int id) {
    std::shared_lock lock(m_mutex);          // ← atomic RMW (cache line bounce)
    return *m_hooks[id];
}

// RCU version (proposed)
Hook& get_hook(int id) {
    auto* snap = m_snapshot.load(std::memory_order_acquire);  // ← pure load
    return *snap->lookup(id);                                  //    no atomic RMW
}
```

Readers do only pure loads, with zero cache-line bouncing. Each of the 8 cores'
L1 caches can hold the same snapshot read-only; MOESI stays in the S state,
with no invalidation traffic.

---

## The trade-off principle — why an escape hatch is required

> "The RCU optimisation should count as a functional trade-off. So there has to
> be a switch to control it."
> — Allen, 2026-05-19

RCU's cost is not zero; it moves the cost from readers to writers and memory:

| Axis | `shared_mutex` (now) | RCU registry |
|---|---|---|
| Reader latency | Moderate (`shared_lock` RMW) | **Very low (pure load)** |
| Writer latency | Moderate (`unique_lock`) | **High (copy snapshot + atomic publish + wait for a grace period)** |
| Peak memory | 1× registry size | **2×, briefly** (an old snapshot is reclaimed only after a grace period) |
| Write visibility | Immediate (visible when the lock is released) | **Delayed** (a reader may see the old snapshot for hundreds of ns) |
| Failure mode | Deadlock (known safeguards) | **Memory-reclamation bugs, very hard to debug** (use-after-free) |
| AOSP toolchain | Fully supported | **`atomic<shared_ptr>` is C++20; older NDKs support it partly** |

For desktop tools with only 1–2 threads or single-threaded games,
`shared_mutex` is already enough — the 26.9× degradation above occurs only in
heavily multi-threaded scenarios. **Forcing RCU's memory and writer-latency
costs on everyone is wrong.**

This is fully consistent with the spirit of the "policy framework" established
in Step 3:
- Step 3 offers a choice of `rcu_writeonce` / `shared_mutex` for **callback storage**
- Step 6 offers a choice of `rcu_atomic_array` / `shared_mutex_map` for **the registry as a whole**

The two escape hatches are independent and compose.

---

## Escape-hatch design

### Build-time switch

A new macro, **modelled on `SPLICE_DEFAULT_POLICY`**:

```cpp
// include/splice/registry_impl.h (new)
namespace splice::registry {

// Registry implementation tag — selects how HookContext stores m_hooks.
//
// shared_mutex_map (default)  : unordered_map<int, shared_ptr<HookBase>>
//                               guarded by shared_mutex. Predictable, well-
//                               tested, AOSP-toolchain safe. Reader pays
//                               atomic RMW on the shared_mutex counter
//                               (cache-line bouncing under contention).
//
// rcu_atomic_array            : atomic<Snapshot*> publish-and-forget. Reader
//                               does a single std::memory_order_acquire load;
//                               no atomic RMW on the hot path. Writer copies
//                               the snapshot, publishes new pointer, and
//                               defers old-snapshot reclamation by one
//                               grace period.
//                               Trade-offs: higher writer latency, ~2×
//                               memory peak during grace period, requires
//                               C++20 std::atomic<shared_ptr> or hand-rolled
//                               hazard-pointer scheme on older toolchains.
struct shared_mutex_map {};
struct rcu_atomic_array {};

} // namespace splice::registry

// Build-time selection. Override per project:
//   add_compile_definitions(SPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array)
//
// Default stays shared_mutex_map — opt-in only, **no surprises for existing users**.
#ifndef SPLICE_REGISTRY_IMPL
#   define SPLICE_REGISTRY_IMPL ::splice::registry::shared_mutex_map
#endif
```

### Why build time and not run time?

The same discipline as Step 3:

| When it switches | Cost | Suitable? |
|---|---|---|
| Build time | 0 ns at run time (template specialisation) | ✅ |
| Run time | +1 branch per lookup + two stores | ❌ violates the hot-path discipline |

The two registries have **completely different data layouts**
(`unordered_map` vs `atomic<Snapshot*>`); switching at run time would mean
keeping both and branching. Splice's hot-path discipline does not allow that
cost.

### The two registries as partial specialisations

`HookContext` moves `m_hooks` into a template helper:

```cpp
// include/splice/context.h (changed)
template <typename Impl>
class HookRegistry;   // primary undefined

// shared_mutex_map — the current implementation (unchanged)
template <>
class HookRegistry<registry::shared_mutex_map> {
    std::shared_mutex                            m_mutex;
    std::unordered_map<int, std::shared_ptr<HookBase>> m_hooks;
public:
    template <typename HookT>
    HookT& get_or_create(int id) {
        { std::shared_lock l(m_mutex);
          if (auto it = m_hooks.find(id); it != m_hooks.end())
              return *static_cast<HookT*>(it->second.get());
        }
        std::unique_lock l(m_mutex);
        auto& slot = m_hooks[id];
        if (!slot) slot = std::make_shared<HookT>();
        return *static_cast<HookT*>(slot.get());
    }
    void clear() { std::unique_lock l(m_mutex); m_hooks.clear(); }
};

// rcu_atomic_array — the new Step 6 implementation (opt-in)
template <>
class HookRegistry<registry::rcu_atomic_array> {
    struct Snapshot {
        // Sparse array, indexed by id directly. Splice ids are __COUNTER__-
        // derived so they're dense from 0. Cap matches HookContext's
        // installer-slot bound.
        std::array<std::shared_ptr<HookBase>, SPLICE_MAX_HOOKS> slots;
    };
    std::atomic<Snapshot*> m_snapshot{nullptr};
    std::mutex             m_write_mutex;   // serialises writers only
public:
    template <typename HookT>
    HookT& get_or_create(int id) {
        // Hot path: pure load, no RMW.
        auto* snap = m_snapshot.load(std::memory_order_acquire);
        if (snap && id < (int)snap->slots.size() && snap->slots[id]) {
            return *static_cast<HookT*>(snap->slots[id].get());
        }
        return install_slot<HookT>(id);   // cold path
    }
private:
    template <typename HookT>
    HookT& install_slot(int id) {
        std::lock_guard l(m_write_mutex);
        auto* old = m_snapshot.load(std::memory_order_acquire);
        auto  next = std::make_unique<Snapshot>();
        if (old) next->slots = old->slots;
        if (!next->slots[id]) next->slots[id] = std::make_shared<HookT>();
        auto* raw = next.release();
        m_snapshot.store(raw, std::memory_order_release);
        defer_reclaim(old);                // see "memory reclamation"
        return *static_cast<HookT*>(raw->slots[id].get());
    }
};
```

`HookContext` holds a `HookRegistry<SPLICE_REGISTRY_IMPL>`. The choice between
the two templates is made at compile time; **the run-time cost is zero**.

---

## The user API is completely unchanged

### From the user's point of view

```cpp
// Whichever registry implementation is used, the user's code is identical
SPLICE_HOOK_ADDR(&eglSwapBuffers)
    .onInvoke([](auto orig, EGLDisplay d, EGLSurface s) {
        return orig(d, s);
    });
splice::install_all();
```

The switch happens only at build time:

```bash
# Default (now)
cmake --preset=windows-x64-release

# Opt-in RCU registry (highly concurrent hook scenarios, e.g. 60 fps × 8 threads)
cmake --preset=windows-x64-release \
      -DSPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array
```

Or in the consumer's CMakeLists.txt:

```cmake
add_compile_definitions(SPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array)
```

---

## RCU's hard problem — memory reclamation

The most dangerous part of RCU is **not reading or writing** but **when to
reclaim an old snapshot**.

### The essence of the problem

```
Thread A: snap = m_snapshot.load();        // gets snap = 0x1000
Thread B: m_snapshot.store(0x2000);        // publishes a new snapshot
Thread B: delete (Snapshot*)0x1000;        // ❌ Thread A is still using it! UAF
```

After a reader has loaded the snapshot pointer and before it dereferences it,
the writer must not free it. **How do you know when every reader has left?**

### Trade-offs between the solutions

| Approach | Mechanism | Pros | Cons | Suits Splice? |
|---|---|---|---|---|
| **`atomic<shared_ptr>`** | Reference counting reclaims automatically | Simple, no UAF | Every reader load does a refcount RMW (**back to `shared_mutex`-level cache bouncing**) | ❌ loses RCU's advantage |
| **Hazard pointers** | Readers register "in use"; writers reclaim once every hazard is clear | Reader does pure loads + one registration (thread-local) | Complex to implement; needs thread-local registration | ⚠️ feasible but a lot of engineering |
| **Epoch-based reclamation (EBR)** | A global epoch counter; readers mark the epoch on entry; writers reclaim after epoch+2 | Reader does pure loads + an epoch mark; moderate to implement | Needs epoch broadcast; long-lived readers extend the grace period | ✅ **recommended** |
| **Quiescent-state reclamation (QSBR)** | Assumes readers have points where they are outside any critical section | Fastest | Needs cooperating code structure (Linux kernel style) | ❌ unsuitable for a hook library |

### Splice's choice: EBR with a 2-epoch retire queue

```cpp
// Pseudocode — the full implementation would go in src/context_rcu.cpp
struct EBR {
    static thread_local std::atomic<int> tl_epoch;   // 0 = quiescent, 1/2 = in CS
    static std::atomic<int> global_epoch;            // alternates 1 ↔ 2
    static std::vector<Snapshot*> retire_q[3];       // indexed by epoch

    static void reader_enter() {
        tl_epoch.store(global_epoch.load(std::memory_order_acquire),
                       std::memory_order_release);
    }
    static void reader_exit() {
        tl_epoch.store(0, std::memory_order_release);
    }
    static void defer_reclaim(Snapshot* old) {
        int e = global_epoch.load();
        retire_q[e].push_back(old);
        try_advance_epoch();
    }
    // ... try_advance_epoch scans all tl_epoch, advances + drains old queue
};
```

The implementation is roughly 200 lines of C++, but this is RCU's central
difficulty and there is no shortcut. **EBR plus a dev-build sanitizer hook that
catches readers missing an enter/exit** keeps the risk under control.

(Implementation later replaced EBR with time-deferred reclamation, because
standard EBR's reader-side atomics would cost 3–9 ns per call; see
[`fr-010-performance-summary.md`](./fr-010-performance-summary.md), Step 6.4.)

---

## Why an array and not a hash map?

Under RCU an `unordered_map` would have to copy its whole bucket structure, an
O(n) writer cost. **Splice's hook ids are derived from `__COUNTER__` and are
already dense integers from 0** — index a `std::array<shared_ptr, MAX>`
directly: O(1) lookup, O(MAX) writer copy (MAX around 256, adjustable).

```cpp
// SPLICE_MAX_HOOKS — build-time bound (decided 2026-05-19)
// Default 512 covers AOSP system-service hooking (300-800 typical) while
// keeping the snapshot at 8 KiB — well under L1d (32-48 KiB) so reader
// lookup stays at L1-hit speed (~1 ns). Bump to 4096 for extreme cases
// (full syscall tracing) but accept the L2-hit penalty.
#ifndef SPLICE_MAX_HOOKS
#   define SPLICE_MAX_HOOKS 512
#endif
```

**512 hook slots means a 512 × 16 bytes = 8 KiB snapshot**, which fits entirely
in L1d (32–48 KiB), so a reader's indexed lookup is a single L1 hit (~1 ns).
The reasons for the default of 512 (decided by Allen, 2026-05-19):

| MAX | Snapshot size | Fits L1d | Reader cost | Covers |
|---|---|---|---|---|
| 256 | 4 KiB | ✅ | ~1 ns | Game enhancement (10–50 hooks) |
| **512 (default)** | **8 KiB** | ✅ | **~1 ns** | **AOSP system services (300–800 hooks)** |
| 1024 | 16 KiB | ✅ tight | ~1 ns | Full API tracing |
| 4096 | 64 KiB | ❌ → L2 | **~5 ns (5× worse)** | Hooking every syscall |

> Beyond 1024 the snapshot no longer fits in L1d and readers fall back to L2
> speed. At that scale consider a hash-based fallback registry (future work,
> not in v1.0 scope).

---

## Expected performance

### 1 thread

| Path | `shared_mutex_map` | `rcu_atomic_array` |
|---|---|---|
| `HookContext::get_hook` | ~10 ns (`shared_lock` RMW + map find) | **~2 ns (atomic load + array index)** |
| Rest of the hot path | ~14 ns | ~14 ns |
| **End to end** | **23.7 ns** | **~16 ns (meets the < 20 ns gate)** |

### 8 threads

| Path | `shared_mutex_map` | `rcu_atomic_array` |
|---|---|---|
| `HookContext::get_hook` | ~80 ns × a contention factor of 8 = ~640 ns | **~2 ns (readers do not contend at all)** |
| Rest of the hot path | ~50 ns | ~50 ns |
| **End to end** | **688 ns** | **~50–80 ns** |
| **8t/1t ratio** | **26.9×** | **~3–5× (meets the < 5× gate)** |

### Writer cost

| Operation | `shared_mutex_map` | `rcu_atomic_array` |
|---|---|---|
| `onInvoke` (first install) | ~50 ns (`unique_lock` + map insert) | **~500 ns** (copy a 4 KiB snapshot + atomic publish) |
| Swapping a callback at run time (rare) | ~50 ns | ~500 ns + grace period (~100 µs) |

Writers are 10× slower — but **installing a callback is a cold path (once per
hook per lifetime)**, so being somewhat slower is entirely acceptable.

### Memory

| State | `shared_mutex_map` | `rcu_atomic_array` |
|---|---|---|
| Steady state | 1× snapshot ~ 4 KiB | 1× snapshot ~ 4 KiB |
| During a write (within the grace period) | 1× | **~2× ~ 8 KiB** (briefly) |

For embedded or extremely memory-sensitive scenarios this doubled peak needs
evaluating. Splice's main deployment target is Android phones (GB of RAM),
where a 4 KiB fluctuation is negligible.

---

## Risks and mitigations

### Risk 1: uneven `atomic<shared_ptr>` support across AOSP toolchains

`std::atomic<std::shared_ptr<T>>` is a C++20 feature, **supported by libc++
only from LLVM 17**. Android NDK r29 uses LLVM 19 (our build environment), but
**toolchains used for third-party AOSP integration may be older**.

**Mitigation:** EBR + raw pointers (the design above), **avoiding
`atomic<shared_ptr>` entirely**. Only `std::atomic<Snapshot*>` + manual EBR
reclamation; compiles as C++17.

### Risk 2: an EBR implementation bug could cause use-after-free

EBR's core rule is "reclaim only after every reader has left". If a reader
forgets to exit (early return, an exception thrown without RAII), the grace
period is extended but **there is no UAF**. If the reclamation logic has a bug
(advancing the epoch too early), there is a UAF.

**Mitigation:**
1. Wrap the EBR guard in RAII (reader enter / exit always paired)
2. A debug-build `SPLICE_RCU_VERIFY` flag: before every reclaim, scan every
   thread-local epoch and free only once all have expired (even if production
   builds skip this for performance)
3. CI runs under both ASan and TSan (the infrastructure already exists)

### Risk 3: copying the snapshot becomes expensive with many hooks

If `SPLICE_MAX_HOOKS` is raised to 4096, every install copies a 64 KiB
snapshot and writers become very slow. But that is a **cold path** and does
not affect the hot path.

**Mitigation:** `SPLICE_MAX_HOOKS` defaults to 256; users who really need more
can change it at build time.

### Risk 4: both registries need maintaining

The code base gains another implementation branch.

**Mitigation:**
- Both registries run the same unit tests (a template helper), so they cannot drift
- The benchmarks measure both, and the CI gate allows neither to regress
- The documentation states plainly: **the RCU registry is an opt-in speed-up; the default is unchanged**

### Risk 5: users who enable RCU hit problems without knowing why

**Mitigation:**
- Log `SPLICE_LOGI("registry=rcu_atomic_array")` at start-up
- A `splice::diagnostics()` API returns the current registry implementation's name
- The documentation shows the trade-off prominently

---

## Implementation plan

In order, each step with a verifiable gate:

| Step | Work | Gate |
|---|---|---|
| 6.1 | Add `include/splice/registry_impl.h` + the `SPLICE_REGISTRY_IMPL` macro. `HookRegistry<shared_mutex_map>` encapsulates the existing `m_hooks` logic. **No behaviour change in this step.** | Existing 105/105 + 94/94 all green |
| 6.2 | Implement `HookRegistry<rcu_atomic_array>` without real EBR (simple leak first — old snapshots are never reclaimed, consistent with the `rcu_writeonce` strategy). | 4 new RCU registry unit tests (basic lookup + concurrent readers) |
| 6.3 | Run the contended bench and confirm 8t/1t falls below 5×. | The 8-thread bench shows < 100 ns/call |
| 6.4 | Implement real EBR reclamation. | TSan + a 1M-iteration stress run with 0 races |
| 6.5 | CI integration: both registry implementations run unit tests + benches; neither may regress. | CI green |
| 6.6 | Complete the documentation (including the README and a CHANGELOG entry). | — |

---

## Points for decision

Please decide:

1. **Overall direction:** do Step 6 (RCU registry + escape hatch)?
2. **Escape-hatch mechanism:** the build-time macro `SPLICE_REGISTRY_IMPL`
   (**strongly recommended**)?
3. **Memory reclamation:** EBR (recommended, no toolchain dependency)? Or a
   simple leak (ship first, add reclamation later)?
4. ~~**Default `SPLICE_MAX_HOOKS`**~~ — **decided 2026-05-19: 512**
   (covers AOSP system services, L1d-friendly, readers do not degrade)

---

## Appendix A: relation to Step 5

Step 5 (callback storage thunk) and Step 6 (registry RCU) are **orthogonal**:

| | Callback storage | Registry |
|---|---|---|
| **Scope** | Each hook's invoke path | Every hook's lookup path |
| **Where the time goes** | Dispatch overhead | Reader contention |
| **Typical saving** | ~1 ns/call | ~10 ns/call (1t), ~600 ns/call (8t) |
| **Escape hatch** | `SPLICE_DEFAULT_POLICY` + `SPLICE_HOOK_AS` | `SPLICE_REGISTRY_IMPL` |
| **Interaction** | Fully independent | — |

The gains compose. But **Step 6's gain is far larger than Step 5's** (10× to
100×), so it takes priority.

## Appendix B: why this document is worth keeping

This document records not only "what we are going to do" but the complete
account of "**why it is off by default, who can turn it on, and what they pay
when they do**".

Every future question can be answered from here:
- "Why not use RCU by default?" → a functional trade-off: writers 10× slower, 2× memory, AOSP risk
- "How do I turn RCU on?" → `-DSPLICE_REGISTRY_IMPL=...`
- "I turned RCU on — why is it still slow?" → probably a reader not going through the EBR guard, or the hotspot is not in the registry
- "Why not a run-time switch?" → it violates the hot-path discipline, consistent with the Step 3 policy framework

It also makes a **Splice engineering principle** permanent:
> Any optimisation with the character of a "functional trade-off" (not simply
> "faster and better") **must provide an escape hatch**. The default leans
> towards "fewest surprises"; opt-in is for informed users.

The principle already appears in three places:
1. Step 3: `SPLICE_DEFAULT_POLICY` + `SPLICE_HOOK_AS` (callback storage)
2. Step 6: `SPLICE_REGISTRY_IMPL` (registry)
3. ScopedHook listing Tier 2 disable as "best-effort, the trampoline leaks
   permanently" is another embodiment of a trade-off (FR-013).

---

## Change log

- 2026-05-19: First version. Full design + escape hatch + EBR strategy + implementation plan, awaiting Allen's decision.
