# Splice v2 — Improvements over the Original the predecessor framework

> A focused design doc for the next two pieces of work after FR-013 closed
> the disable story: **Task #57 (installer-queue lifetime)** and
> **FR-010 / Phase 3.5 (performance pass)**.
>
> Both are *targeted upgrades over the in-house predecessor framework that
> Splice was productised from*. They are not rewrites — they replace
> three small but load-bearing pieces of the v1 hot path with patterns
> that scale to multi-thread game workloads (per-frame, per-syscall hook
> rates) without changing the friendly fluent API.
>
> Audience: someone who has read `reference/the predecessor framework.h` and is
> about to touch `include/splice/{core,context,registry}.h`.

---

## TL;DR

| # | Concern | Original (`reference/the predecessor framework.h`) | Splice v2 (planned) | Status |
|---|---------|------------------------------------------|---------------------|-----------|
| 0 | Map type | `std::map<int, ...>` (O(log n)) | `std::unordered_map<int, ...>` (O(1) amortised) | **Done** (FR-008) |
| 1 | Installer-queue lifetime | `static std::vector<std::function<void()>>`, never erased — entry destructor leaves a dangling `[this]` lambda behind | `register_installer` returns an RAII token; entry's destructor erases its slot | **Done** (Task #57, 2026-05-07) |
| 2 | Hot-path concurrency | One `recursive_mutex` per hook + one process-wide mutex per registry — readers serialise | Compile-time **policy framework**: default `rcu_writeonce` (atomic-pointer reader), opt-in `shared_mutex` for swap-heavy hooks | FR-010 |
| 3 | Callback storage | `std::function<Ret(FuncType, Args...)>` (heap allocation + indirect call) | Template parameter `Callback` on `Hook<>` so the lambda is fully inlined | FR-010 |

Targets after the pass: **< 20 ns / call** single-thread overhead on Zen 4 / Alder Lake; **< 5×** that under 8-thread contention; CI guard fails the build on regression.

---

## Why this list and not others

The v1.4 design review (`docs/Old API Hooker Review.md`) flagged six things on the hot path. Three of them have already shipped:

- ✅ `std::map` → `std::unordered_map` — folded into FR-008's HookContext consolidation.
- ✅ Atomic patching — closed by Phase 4.5a/b/d (FR-011).
- ✅ Tier 1 + Tier 2 disable — closed by Phase 4.5c-1/c-2 (FR-013).

The three remaining items below are the surface area of this document. The other reviewer suggestions (`std::any`-based type-safe internal storage, `dlopen` cleanliness, prologue-length heuristic) are still open but do not move the per-call overhead needle.

---

## Improvement #1 — Installer queue lifetime (Task #57)

### What's wrong

`the predecessor framework.h:245-254`:

```cpp
inline std::vector<std::function<void()>>& getGlobalInstallers() {
    static std::vector<std::function<void()>> installers;
    return installers;
}

inline void registerGlobalInstaller(std::function<void()> fn) {
    getGlobalInstallers().push_back(fn);
}
```

…and `InterceptorEntry` constructors push `[this]() { install(); }` lambdas in. There is **no symmetric remove**. Splice inherited this verbatim — `include/splice/context.h:164-168` is the same pattern wrapped in a class.

The lambda outlives the entry it captures whenever:

1. A test fixture creates a `static` or stack-scoped `InterceptorEntry` (the typical `TEST()` body pattern), and
2. A subsequent test calls `splice::install_all()`, which iterates **every** installer ever registered — including ones whose `this` is now stale or whose hook target is no longer a sane thing to overwrite.

On Windows the symptoms are usually benign (best-effort IAT walk fails quietly, hook unique_id collides). On Android ARM64 it surfaces as stack-corruption / SIGILL during full `splice_unit_test` runs, because the inline patcher actually writes `.text` and there is no SEH to absorb the mistake. The 4.5d work had to ship a *separate* `splice_stress_test` binary just to keep `default_context()` clean enough to install hooks (`tests/CMakeLists.txt:51-65`).

There is also a latent UAF in `HookContext::reset()` (already documented in `project_hookcontext_debt`): clearing `m_installers` does not stop the entry destructors from firing later — and an entry that re-registered itself after `reset()` would have its lambda in the queue while the entry itself was already destructor-deep.

### After

`register_installer` returns a token — an RAII handle keyed by an integer slot id. The entry stores the token; its destructor releases it.

**Before** — `include/splice/core.h:55,75`:

```cpp
register_global_installer([this]() { install(); });
```

**After**:

```cpp
m_installer_token = register_global_installer([this]() { install(); });
//                                                ^^^^^^^^^^^^^^^^^^^^
// returns: splice::InstallerToken (RAII; dtor unregisters)
```

**Before** — `include/splice/context.h:208-213`:

```cpp
mutable std::recursive_mutex m_mutex;
std::unordered_map<std::string, int> m_key_to_id;
std::unordered_map<int, std::string> m_id_to_key;
std::unordered_map<int, void*> m_originals;
std::unordered_map<int, std::shared_ptr<HookBase>> m_hooks;
std::vector<std::function<void()>> m_installers;
```

**After** — `m_installers` becomes a stable-iterator container with a free-list for slot reuse:

```cpp
struct InstallerSlot {
    std::function<void()> fn;
    bool live;            // false → free list, skip during install_all
};
std::vector<InstallerSlot> m_installers;   // stable indices, live flag controls iteration
std::vector<int> m_free_slots;             // indices of slots with live==false
```

`InstallerToken` holds `{HookContext*, int slot_index}`; its destructor flips `live=false` and pushes the index onto `m_free_slots`. `register_installer` reuses a free slot if one is available, else appends.

Why `vector + free-list` and not `std::list`:
- Stable iterators (the install_all loop snapshots indices, not pointers).
- O(1) register, O(1) unregister, no node allocation per hook.
- The hot path is still `for (auto& s : m_installers) if (s.live) s.fn();` which a branch predictor handles fine — install_all runs once per process, not per call.

### Side benefit: the test pollution drops out

Every `static`/`thread_local`/scope-local `InterceptorEntry` now removes itself from the queue when it goes out of scope. The two test binaries (`splice_unit_test` + `splice_stress_test`) collapse back to one. The note in `tests/CMakeLists.txt:51-54` becomes deletable.

The current `tests/test_hook_inline_disable.cpp:62` workaround — `SPLICE_HOOK_ADDR_STATIC(&multiply_two)` plus a capture-less lambda — stops being needed; the test can use the ergonomic forms again.

### Cost

- One pointer + one int per `InterceptorEntry` (the token).
- One conditional in the install_all loop (predictable, mispredict-free after warm-up).
- Public ABI of `InterceptorEntry` widens by one member; that's pre-1.0 so it's free.

---

## Improvement #2 — Policy-driven hot-path concurrency (FR-010)

### What's wrong

The original takes the same `recursive_mutex` on every single hook invocation — twice (`HookManager::getHook` lookup + `Hook::invoke` itself), and once more in `OriginalRegistry::getOriginal`. Concretely:

`the predecessor framework.h:185-189`:

```cpp
Ret invoke(FuncType original, Args... args) {
    std::lock_guard<HookMutex> lock(mutex_);
    if (invokeFn_) return invokeFn_(original, args...);
    return original(args...);
}
```

Three `recursive_mutex::lock()` per hook fire. On Linux glibc each one is a CAS + a TLS read; if any other thread holds it (e.g. someone calling `setInvoke` from a config reload), it's a futex syscall. For a hook called per-frame at 144 Hz across eight threads this dominates the per-frame cost.

It was always wrong. The review's framing nailed it:

> Hook callbacks are almost always reads; writes only happen in `setInvoke`.

### Why a *policy*, not a single answer

There is no universal best concurrency model for `Hook<>`. RCU-style designs win when callbacks are *write-once* (Splice's typical case), but they impose lock-in:

| Lock-in axis | RCU write-once-leak | RCU refcounted (`atomic<shared_ptr>`) | shared_mutex |
|--------------|---------------------|----------------------------------------|--------------|
| **Memory** | every `set_invoke` permanently leaks one callback | old version lives during grace period | 0 |
| **Write latency** | atomic store (~50 ns) | atomic store + refcount + reclaim wait | bounded by slowest reader holding the shared_lock |
| **Visibility staleness** | reader can run old callback for a window after store | bounded by grace period | unique_lock writer → next reader sees new value immediately |
| **Reader cost** | 1 acquire load | acquire load + atomic refcount RMW | 2 atomic RMW + cache-line ping |

For a hook called per-frame the RCU reader cost wins by 5-10×. For a hook whose callback is genuinely swapped at runtime (A/B testing, dynamic instrumentation, tracing toggle), `shared_mutex`'s zero-leak / immediate-visibility semantics matter more.

Splice's answer: **expose the choice as a compile-time policy with a sensible default**. 99% of users get the fast default; the 1% who need swap semantics opt in with one extra macro arg. Runtime-switchable is *deliberately not offered* — see "Why not runtime-switchable" below.

### After — the policy framework

```cpp
namespace splice::policy {

// reader = 1 acquire load; writer = new + atomic store; callback leaks
// on every set_invoke (intentional — Splice hooks live for program life)
struct rcu_writeonce {};

// reader = atomic<shared_ptr>::load (refcount RMW); writer = atomic
// shared_ptr swap; old version reclaimed by refcount. Zero leak,
// readers slightly slower than rcu_writeonce.
struct rcu_refcounted {};

// classic reader-writer lock; no leak, no staleness, but readers
// pay an atomic counter inc/dec each call.
struct shared_mutex {};

}  // splice::policy

#ifndef SPLICE_DEFAULT_POLICY
#  define SPLICE_DEFAULT_POLICY ::splice::policy::rcu_writeonce
#endif
```

`Hook<>` grows a `Policy` template parameter; storage + dispatch live in three partial specialisations of an internal `HookStorage<Policy, Ret, Args...>`:

```cpp
template <typename Ret, typename... Args, typename Policy = SPLICE_DEFAULT_POLICY>
class Hook : public HookBase {
    HookStorage<Policy, Ret, Args...> m_storage;
public:
    void set_invoke(HookFn fn) { m_storage.store(std::move(fn)); }
    Ret  invoke(FuncType orig, Args... args) {
        return m_storage.invoke(orig, std::forward<Args>(args)...);
    }
};

// rcu_writeonce — the default. Reader is lock-free.
template <typename Ret, typename... Args>
struct HookStorage<policy::rcu_writeonce, Ret, Args...> {
    std::atomic<HookFn*> m_fn{nullptr};

    void store(HookFn fn) {
        m_fn.store(new HookFn(std::move(fn)), std::memory_order_release);
        // intentional leak — hook callbacks live for program lifetime
    }
    Ret invoke(FuncType orig, Args... args) {
        if (auto* p = m_fn.load(std::memory_order_acquire)) return (*p)(orig, args...);
        return orig(args...);
    }
};

// shared_mutex — escape hatch for runtime callback swap.
template <typename Ret, typename... Args>
struct HookStorage<policy::shared_mutex, Ret, Args...> {
    HookFn m_fn;
    mutable std::shared_mutex m_mutex;

    void store(HookFn fn) {
        std::unique_lock lock(m_mutex);
        m_fn = std::move(fn);
    }
    Ret invoke(FuncType orig, Args... args) {
        std::shared_lock lock(m_mutex);
        return m_fn ? m_fn(orig, args...) : orig(args...);
    }
};
```

### User-facing API: two macro variants

```cpp
// 99% case — uses SPLICE_DEFAULT_POLICY (= rcu_writeonce)
SPLICE_HOOK(eglSwapBuffers)
    .onInvoke([](auto orig, EGLDisplay d, EGLSurface s) { ++frames; return orig(d, s); });

// 1% case — explicit policy for callbacks that get swapped at runtime
SPLICE_HOOK_AS<splice::policy::shared_mutex>(my_swappable_func)
    .onInvoke(initial_callback);

// later, from any thread, with immediate visibility:
my_swappable_entry.onInvoke(updated_callback);
```

`SPLICE_HOOK_AS` exists alongside `SPLICE_HOOK` rather than as a chained `.with_policy<...>()` because the policy must be known *before* the `Hook<>` template is instantiated, not after.

### Why not runtime-switchable

`splice::default_context().use_policy(policy::shared_mutex)` is rejected on purpose. The three policies have *different memory layouts* for `Hook<>` (atomic pointer vs `HookFn + shared_mutex`). Switching at runtime would require either:

- Migrating every existing `Hook<>` instance — exposes a mid-state where readers see neither the old nor the new layout, or
- Adding a runtime branch on `current_policy` to every `invoke()` — permanent ~3-5 ns hot-path cost for a feature 99% of users never touch.

Both violate the FR-010 < 20 ns target. If you genuinely need to switch policies, it's a build-flag decision, not a runtime one.

### What about the per-Hook storage *and* the registry lock

The current code grabs both:

```
hot path = lock(context.m_mutex) → lookup hook map → unlock → lock(hook.m_mutex) → invoke → unlock
```

`HookContext::get_original` and `get_hook` are *also* hot reader paths. They use the same policy framework: `m_originals` and `m_hooks` go behind a `HookContext::registry_storage<Policy>` that mirrors the per-hook design. Default policy → registry lookup is a single atomic acquire load.

The *write* path (install / set_invoke) takes the unique-lock variant in every policy and is correspondingly slower — which is what we want, because writes are off the critical path.

### Implementation limits & impacts

1. **`HookContext::get_hook<Ret, Args...>(id)` downcast gains one axis** — already requires "same id ⇒ same `<Ret, Args...>`"; adding `Policy` keeps the same invariant. `__COUNTER__` already guarantees uniqueness; add a debug assertion that logs `SPLICE_LOGE` on policy mismatch instead of UB.
2. **Compile time / binary size** — three `HookStorage<>` specialisations per call site that uses non-default policy. In practice each binary uses one or two; not a concern.
3. **CI matrix** — three default-policy builds × two arches × two OSes = 12 combinations. Realistic CI: full coverage on `rcu_writeonce`, sanity build on `shared_mutex` × Linux x64; `rcu_refcounted` covered by unit tests only.
4. **Benchmark obligations** — FR-010 acceptance requires three curves (one per policy) on `bench_hook_overhead` and `bench_hook_contended`, plus a `bench_swap_visibility` that only `rcu_*` policies legitimately fail.
5. **ABI surface** — `splice::policy::*` types live in `include/splice/policy.h` and are part of the public API. Adding fields to a policy is a v2.0 break.
6. **C++17 consumers** — `rcu_refcounted` needs `std::atomic<std::shared_ptr<T>>` (C++20). For C++17 builds, mark `rcu_refcounted` `[[deprecated]]` and silently fall back to `rcu_writeonce`. `rcu_writeonce` itself is C++17-clean.
7. **Re-entry** — the v1 `recursive_mutex` masked the fact that install-time `set_original` re-enters the context. Under the policy framework the re-entry must be moved outside the writer's unique_lock (already needed for `shared_mutex` correctness; free under `rcu_*`).

### Why "intentional leak" is fine

`rcu_writeonce` leaks one `HookFn` per `set_invoke`. Splice's typical workload calls `set_invoke` exactly once per hook at process startup, so the leak is bounded by the number of hooks (typically ≤ 100, ≤ 8 KB total). Programs that do swap callbacks repeatedly would, by definition, choose `policy::shared_mutex` instead.

This is the design's deliberate trade: **make the common case (write-once) reader-lock-free, make the uncommon case (swap-heavy) explicit**.

---

## Improvement #3 — `std::function` → template callback (FR-010)

### What's wrong

`the predecessor framework.h:178` and the inherited `include/splice/core.h:79`:

```cpp
using HookFn = std::function<Ret(FuncType, Args...)>;

InterceptorEntry& onInvoke(std::function<Ret(FuncType, Args...)> fn) {
    HookManager::get_hook<Ret, Args...>(m_unique_id).set_invoke(std::move(fn));
    return *this;
}
```

`std::function` always type-erases. For non-empty captures it heap-allocates (libstdc++ has a small-buffer of 16 bytes; libc++ 24; MSVC ~64). Every call goes through:

1. Read the type-erased `_M_invoker` function pointer.
2. Indirect-call into a stub that downcasts the storage and forwards args.
3. The stub then indirect-calls the actual lambda body.

Two indirect calls minimum, no inlining ever, optimiser fence on every parameter. For a hook whose body is `++frame_count;` the wrapper is *literally an order of magnitude* more expensive than the work it wraps.

### After

Move the callback type into the template parameters of `Hook` and `InterceptorEntry`:

```cpp
template <typename FuncType, int UniqueId, typename Callback = void>
class HookEntry { /* the new InterceptorEntry */ };

template <typename FuncType, int UniqueId, typename Callback>
class Hook {
    Callback m_cb;                           // the lambda's *actual* type
    // …
    auto invoke(FuncType orig, auto... args) -> decltype(orig(args...)) {
        return m_cb(orig, args...);          // fully inlinable
    }
};
```

The `__COUNTER__`-derived `UniqueId` already gives every macro call site a fresh template instantiation, so there is no `Callback` collision across hooks. The fluent chain becomes:

```cpp
SPLICE_HOOK(eglSwapBuffers).onInvoke([](auto orig, EGLDisplay d, EGLSurface s) {
    ++frame_count;
    return orig(d, s);
});
```

…with `Callback = decltype(the lambda)` deduced at the call site. `onInvoke` returns a *different* `HookEntry<…, NewCallback>` because the type changed — so `.onInvoke()` is a one-shot terminator, not a builder you can re-set. (Builders that flip callbacks at runtime — see `.times(n)`, `.once()` in FR-009 — keep using a typed wrapper that stores the original `Callback` plus its runtime state.)

### What this breaks (and what it doesn't)

Doesn't break:
- The macros (`SPLICE_HOOK`, `SPLICE_HOOK_ADDR`, `SPLICE_HOOK_STATIC`) all expand to type-deduced expressions; users never spell the type.
- Existing tests that go `auto& entry = SPLICE_HOOK_ADDR(...)` — `auto&` doesn't care about the new template parameter.
- Disable / install — they live in the C ABI and are type-erased anyway.

Does break:
- Any code that holds `InterceptorEntry<FuncType>&` by name — must become `auto&`.
- `InterceptorBatch` (registry.h:94) currently takes `std::function<void()>` — that's an installer, not an invoker, and it's *cold-path*. Leave it on `std::function`.

The compromise: `onInvoke` keeps a `std::function` overload alongside the templated one. User code that prefers ABI stability can opt back into type erasure with one extra wrapping layer. Best-of-both-worlds defaults to the inlined path.

### Expected speedup

The review cited `std::function` as the dominant cost on hot hooks. Going from "two indirect calls + heap" to "one inlined lambda body" typically takes a no-op hook from ~40-80 ns to <10 ns — comfortable headroom under the < 20 ns target.

---

## Improvement #0 (already shipped) — `std::map` → `std::unordered_map`

For completeness, what's already in the tree on `master`:

```diff
- static std::map<std::string, int> map;
- static std::map<int, std::string> map;
- static std::map<int, void*>      registry;
- static std::map<int, Hook<…>>    hooks;
+ std::unordered_map<std::string, int>                 m_key_to_id;
+ std::unordered_map<int, std::string>                 m_id_to_key;
+ std::unordered_map<int, void*>                       m_originals;
+ std::unordered_map<int, std::shared_ptr<HookBase>>   m_hooks;
```

Verified at `include/splice/context.h:209-212`. No further work — the doc notes it because the v1.4 review listed it as a TODO and a casual reader of this file would otherwise wonder why we skipped it.

---

## Benchmark harness (prerequisite for the whole pass)

There is no benchmark code in the tree. Step zero of FR-010 is to add one.

### Layout

```
benchmark/
├── CMakeLists.txt
├── bench_hook_overhead.cpp   — single hook, single thread, no-op callback
├── bench_hook_contended.cpp  — same hook, 8 threads spinning
├── bench_install_all.cpp     — 1M install_all() with sparse hooks
└── helpers/
    └── bench_targets.h       — noinline functions matched to test_targets
```

GoogleBench is already in vcpkg; pin to whatever Splice's `vcpkg.json` settles on. The harness lives outside `tests/` so CTest doesn't run it.

### Acceptance gates (from `specs/requirements.md:406-410`)

| Gate | Threshold | Where measured |
|------|-----------|----------------|
| Single-thread no-op overhead | **< 20 ns / call** | bench_hook_overhead, mean of 10⁷ calls |
| 8-thread contended | **< 5× single-thread** | bench_hook_contended |
| Dispatch with 1M installed | Amortised O(1) | bench_install_all, slope check |
| CI regression | Fail build if ≥ +10% vs `master` | `.github/workflows/bench.yml` |

The CI guard runs on a self-hosted Zen 4 runner (the same Snapdragon-ARM64 + Win-x86_64 pair we use for stress validation). Numbers on shared GitHub runners are too noisy.

---

## Recommended ordering

The three improvements are not independent; doing them in the wrong order forces rework.

```
┌─ Step 1 ─ Task #57: installer-queue lifetime (RAII token)
│           ✅ shipped 2026-05-07. Single unified test binary,
│              92/93 on Snapdragon ARM64, 100/100 on Windows x64.
│
├─ Step 2 ─ Benchmark harness
│           Why now: locks in the baseline numbers before any hot-path
│           change, so we can attribute each subsequent delta correctly.
│           Three target curves baked in from day 1: rcu_writeonce,
│           shared_mutex, recursive_mutex (the v1 baseline).
│
├─ Step 3 ─ Policy framework + rcu_writeonce default (Improvement #2)
│           Lands `splice::policy::*` and `HookStorage<Policy, ...>`
│           specialisations. Default = rcu_writeonce. Existing recursive
│           paths stay alive as a fallback for the C++17-only
│           configurations.
│
├─ Step 4 ─ shared_mutex policy + SPLICE_HOOK_AS escape hatch
│           Adds the runtime-swap-friendly variant. One unit test that
│           exercises a single binary running both policies side-by-side
│           on different hooks (no cross-pollination).
│
├─ Step 5 ─ Template Callback (Improvement #3)
│           Why after the policy work: the policy split already moved
│           Hook<>'s storage into a partial spec; templating Callback is
│           the same kind of refactor at a different axis. Doing them
│           together would entangle two API breaks.
│
└─ Step 6 ─ rcu_refcounted policy (optional, only if a real consumer
            asks for it — atomic<shared_ptr> drags in C++20-only code,
            and shared_mutex covers most "swappable callback" needs).
```

### Why not do #57 and FR-010 in parallel

The template-callback work (Step 5) lands new template parameters on `InterceptorEntry`, which means the constructor signature changes — exactly the constructor that registers the installer. Doing #57 first means the install token API was settled before the constructor's signature changed; this is no longer hypothetical — Step 1 shipped 2026-05-07 ahead of any of the FR-010 work, so Steps 2-5 inherit a clean base.

---

## Out of scope for v2

Listed here so they're not invented mid-PR:

- **Hook priority / ordering beyond registration order.** Stays on the FR-009 wishlist.
- **Cross-process or kernel-mode hooks.** Hard non-goal.
- **A C-only public API.** Splice's selling point is the C++ fluent chain; a C façade is a v1.1 conversation.
- **Auto-detecting which thread is currently inside a trampoline (for true uninstall with memory reclaim).** FR-013 closed the door on this in-process; it stays closed.

---

## References

- Original v1 source — `reference/the predecessor framework.h:155-264` (the four registries + installer queue)
- v1.4 review — `docs/Old API Hooker Review.md` §3.2, §5.4
- Acceptance criteria — `specs/requirements.md:388-419`
- Hooking-internals overview (atomic patching, ICache, memory model) — `docs/hooking-internals.md`
- The library's own architecture summary — `specs/architecture.md`
