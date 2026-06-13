# Template callback in place of `std::function` — evaluation

**Status:** Under evaluation / awaiting decision
**For:** FR-010 Step 5
**Audience:** Allen (decision maker)
**Written:** 2026-05-18

---

## Problem statement

Splice's hook hot path currently stores the callback in a `std::function`:

```cpp
// include/splice/context.h:125
using HookFn = std::function<Ret(FuncType, Args...)>;
```

That was the original the predecessor framework's design choice, and **it works
correctly**. But FR-010's performance target requires a single-threaded hook
under 20 ns; it is currently 23.7 ns, and most of the remaining ~4 ns is
assumed to be `std::function`'s type-erasure overhead:

1. **vtable hop:** every `invoke` goes through an indirect call to the manager
   function inside `std::function` (similar to a virtual call)
2. **SBO boundary:** a lambda capture larger than ~16 bytes is heap-allocated
   (only at `store`, but the chance of a cache miss rises)
3. **The compiler cannot inline:** `std::function::operator()` crosses the
   type-erasure boundary, cutting cross-TU inlining

Estimated gain: **23.7 ns → 16–18 ns (−25% to −30%)**, meeting the < 20 ns gate.

---

## The core question: does the user API change?

**Short answer: 99% unchanged. It is written exactly the same; only the time
of type checking moves from run time to compile time.**

Detailed answer, in three usage scenarios.

### Scenario A — a plain lambda (90% of uses)

#### Before (now)
```cpp
SPLICE_HOOK_ADDR(&eglSwapBuffers)
    .onInvoke([](auto orig, EGLDisplay d, EGLSurface s) {
        ++frame_count;
        return orig(d, s);
    });
```

#### After (template callback)
```cpp
SPLICE_HOOK_ADDR(&eglSwapBuffers)
    .onInvoke([](auto orig, EGLDisplay d, EGLSurface s) {
        ++frame_count;
        return orig(d, s);
    });
```

**Not one character changes.** The compiler deduces the lambda's type and the
template `onInvoke` stores it under its original type. No type erasure.

---

### Scenario B — a lambda with captures (5% of uses)

#### Before
```cpp
int counter = 0;
std::string label = "frame";
SPLICE_HOOK_ADDR(&eglSwapBuffers)
    .onInvoke([&counter, label](auto orig, EGLDisplay d, EGLSurface s) {
        ++counter;
        SPLICE_LOGD("%s=%d", label.c_str(), counter);
        return orig(d, s);
    });
```

#### After
```cpp
int counter = 0;
std::string label = "frame";
SPLICE_HOOK_ADDR(&eglSwapBuffers)
    .onInvoke([&counter, label](auto orig, EGLDisplay d, EGLSurface s) {
        ++counter;
        SPLICE_LOGD("%s=%d", label.c_str(), counter);
        return orig(d, s);
    });
```

**Not one character changes.** The captures are stored in `HookStorage`
together with the lambda; the internal layout changes from `std::function`'s
SBO buffer to direct in-place storage.

**The one thing to note:** the lambda object is **copied** inside `onInvoke`
(the same as `std::function` does now). The `&counter` reference still refers
to the variable in the caller's scope, so `++counter` still modifies the
original `counter`. The semantics are exactly equivalent.

---

### Scenario C — passing in a `std::function` variable (< 1% of uses; possibly none of yours)

This is the **only** scenario that differs.

#### Before
```cpp
std::function<int(int(*)(int), int)> stored_callback = make_my_callback();
SPLICE_HOOK_ADDR(&some_func)
    .onInvoke(stored_callback);   // OK — std::function copy-init from std::function
```

#### After (two options)

**Option 1: fully compatible**
```cpp
// This line still compiles — std::function is itself a callable, and is
// stored in HookStorage as just another callable type. But performance falls
// back to std::function's level (HookStorage calls std::function::operator()).
std::function<int(int(*)(int), int)> stored_callback = make_my_callback();
SPLICE_HOOK_ADDR(&some_func).onInvoke(stored_callback);
```

**Option 2: strict but faster**
```cpp
// Reject std::function — a compile-time error, forcing a lambda or function pointer
SPLICE_HOOK_ADDR(&some_func).onInvoke(stored_callback); // ❌ static_assert
```

**Recommendation: option 1 (silently compatible).** Splice should not break
users' existing `std::function` code on its own initiative.

---

## Internal design changes (invisible to users)

### Before — `HookStorage<rcu_writeonce>`

```cpp
template <typename Ret, typename... Args>
struct HookStorage<policy::rcu_writeonce, Ret, Args...> {
    using HookFn = std::function<Ret(FuncType, Args...)>;
    std::atomic<HookFn*> m_fn{nullptr};

    void store(HookFn fn) {
        m_fn.store(new HookFn(std::move(fn)), std::memory_order_release);
    }
    Ret invoke(FuncType orig, Args... args) {
        if (auto* fn = m_fn.load(std::memory_order_acquire))
            return (*fn)(orig, args...);       // ← std::function vtable hop
        return orig(args...);
    }
};
```

### After — type-erased thunk + raw state

```cpp
template <typename Ret, typename... Args>
struct HookStorage<policy::rcu_writeonce, Ret, Args...> {
    using FuncType = Ret(*)(Args...);
    using Thunk    = Ret(*)(void* state, FuncType, Args...);

    std::atomic<Thunk> m_thunk{nullptr};
    std::atomic<void*> m_state{nullptr};

    template <typename Lambda>
    void store(Lambda fn) {
        // Static-storage instance per (call-site, Lambda type). One write
        // per hook over program lifetime — same leak semantics as before.
        static Lambda* s_fn = new Lambda(std::move(fn));
        m_state.store(s_fn, std::memory_order_relaxed);
        m_thunk.store(
            +[](void* st, FuncType orig, Args... args) -> Ret {
                return (*static_cast<Lambda*>(st))(orig, args...);
            },
            std::memory_order_release);
    }

    Ret invoke(FuncType orig, Args... args) {
        if (auto th = m_thunk.load(std::memory_order_acquire)) {
            return th(m_state.load(std::memory_order_relaxed), orig, args...);
        }
        return orig(args...);
    }
};
```

Hot-path difference:
- **Old:** load `HookFn*` → vtable lookup of `operator()` → call the manager → call the real lambda
- **New:** load the `Thunk` function pointer → one indirect call → the lambda inlined into the thunk

One less vtable hop, one less lookup through the manager pointer. (And `Thunk`
is a plain function pointer, which branch prediction handles well.)

### `HookAs::set_invoke`'s signature change

#### Before
```cpp
template <typename Policy, typename Ret, typename... Args>
class HookAs : public HookBase {
public:
    using HookFn = std::function<Ret(FuncType, Args...)>;
    void set_invoke(HookFn fn) { m_storage.store(std::move(fn)); }
    // ...
};
```

#### After
```cpp
template <typename Policy, typename Ret, typename... Args>
class HookAs : public HookBase {
public:
    template <typename Lambda>
    void set_invoke(Lambda&& fn) {
        m_storage.store(std::forward<Lambda>(fn));
    }
    // ...
};
```

`InterceptorEntry::onInvoke` becomes a template as well:

#### Before
```cpp
InterceptorEntry& onInvoke(std::function<Ret(FuncType, Args...)> fn) {
    HookManager::get_hook_as<Policy, Ret, Args...>(m_unique_id)
        .set_invoke(std::move(fn));
    return *this;
}
```

#### After
```cpp
template <typename Lambda>
InterceptorEntry& onInvoke(Lambda&& fn) {
    HookManager::get_hook_as<Policy, Ret, Args...>(m_unique_id)
        .set_invoke(std::forward<Lambda>(fn));
    return *this;
}
```

---

## Trade-offs

| Axis | `std::function` (now) | Template callback (proposed) |
|---|---|---|
| **Single-thread latency** | 23.7 ns | **Estimated 16–18 ns** (meets the < 20 ns gate) |
| **User API change** | — | **None** (scenarios A/B unchanged to the character) |
| **Type erasure** | Run time (vtable) | Compile time (template) |
| **Compile time** | Baseline | **A template instantiation per call site** (~50–100 ms each) |
| **Binary size** | Baseline | **One thunk function per hook site** (~50 bytes/site) |
| **Lambda capture limits** | Any size | Any size (but one static per type) |
| **Swapping the callback at run time** | Free | **Cannot switch to a different type under `rcu_writeonce`**; `shared_mutex` still can (it keeps `std::function`) |
| **Debugger experience** | Crosses the type-erasure boundary; the stack trace jumps twice | The lambda appears directly in the trace |
| **Error messages** | Run time (`bad_function_call`) | **Compile time** (a wrong lambda signature fails to compile) |

---

## Main risks

### 1. Compile-time blow-up (medium risk)

Each `SPLICE_HOOK*` call site instantiates its own
`HookStorage::store<Lambda>` + thunk function. A binary with 200+ hook sites
might compile 20% slower.

**Mitigation:** Splice's hook sites usually number < 50 (OpenGL/EGL/syscall).
Measure, then decide whether explicit instantiation is needed.

### 2. Rebinding the lambda type (low risk)

```cpp
SPLICE_HOOK_ADDR(&foo).onInvoke(lambda_a);  // store as Lambda_A
// ...later, the same hook id...
SPLICE_HOOK_ADDR(&foo).onInvoke(lambda_b);  // tries to store Lambda_B
```

Under `rcu_writeonce` the second call **silently overwrites** the first
lambda, but because the two lambdas have different types, each instantiates
its own `static Lambda* s_fn`, and the second call's thunk points to the new
type's storage. No UB, but surprising behaviour (both lambdas stay alive).

**Mitigation:** `SPLICE_HOOK_ADDR(&foo)` is already a distinct call site
through `__COUNTER__`, so types do not collide in practice. Document that
`onInvoke` should be called only once (`rcu_writeonce` semantics). The
`shared_mutex` policy keeps `std::function` to support dynamic swapping.

### 3. Compatibility with existing `std::function` arguments (resolved by option 1)

See scenario C above.

---

## Two-phase implementation (recommended)

### Phase 5a — change only the `rcu_writeonce` policy (small change, large gain)

- `HookStorage<rcu_writeonce>` switches to the thunk + state design
- `HookStorage<shared_mutex>` is **unchanged** and keeps `std::function`
- `HookAs::set_invoke` becomes a template
- Expected: 23.7 → 17–18 ns

Reason: 99% of users run `rcu_writeonce` (the default). `shared_mutex` is the
escape hatch and a slow path to begin with; there is no need to touch it.

### Phase 5b — advanced: inline the whole thunk path (optional)

Turn the thunk from an external function into a `[[gnu::always_inline]]`
member template. Saves another ~2 ns but touches the ABI (the trampoline path
would hold the thunk directly, without `HookContext::get_hook`).

**This phase is beyond Step 5's scope**, left for the future.

---

## Options not taken (ruled out)

### Option X: force captureless lambdas → plain function pointers
```cpp
void onInvoke(Ret(*)(FuncType, Args...));  // rejects lambda captures
```

- ✅ Fastest (theoretically 10–12 ns)
- ❌ **Breaks the existing API:** every `[&state]` capture you have now would need rewriting
- ❌ Violates the "keep the friendly fluent API feel" principle

**Not adopted.**

---

## Points for decision

Please decide:

1. **Do Step 5a?** (`rcu_writeonce` moves to templates, `shared_mutex` untouched)
   - Gain: meets the < 20 ns gate, the next benchmark milestone
   - Cost: ~200 lines of hot-path change + benchmark validation + documentation

2. **If so, accept scenario C's "option 1" compatibility path?** (`std::function`
   variables can still be passed, without the speed-up)
   - Recommended: yes — it breaks no existing use

3. **Is compile time a hard limit?** (if the < 5 s ceiling cannot be exceeded,
   measure the baseline first)

---

## Appendix: relation to benchmark-baseline.md

Current benchmark data:
- 1-thread hooked: 23.7 ns (target < 20 ns, **not met**)
- 8-thread hooked: 688 ns, 8t/1t = 26.9× (target < 5×, **not met**)

Step 5 attacks the 1-thread number. The 8-thread ratio is Step 6's job (a real
RCU registry), outside this evaluation.

---

## Appendix B: measured microbenchmark (2026-05-19)

Before deciding on Step 5a, an independent microbenchmark,
`benchmark/bench_callback_storage.cpp`, was written — **without touching Splice
internals** — comparing three paths directly:
- `BM_DirectCall` — `orig(x) + 1`, no storage indirection (floor)
- `BM_StdFunctionPath` — `std::atomic<std::function*>` load + invoke (mirrors the current design)
- `BM_ThunkPath` — `std::atomic<thunk_fn>` + `std::atomic<void*>` load + invoke (mirrors Step 5)

All three paths share the same captureless lambda body (`orig(x) + 1`) and the
same noinline target.

### Platform 1: Windows x86_64 (Ryzen 9 9950X3D, MSVC /O2, Release)

| Path | Mean | Median | CV | Delta vs Direct |
|---|---|---|---|---|
| BM_DirectCall | 2.60 ns | 2.60 ns | 0.63 % | 0 |
| BM_StdFunctionPath | 3.99 ns | 3.99 ns | 0.23 % | **+1.39 ns** |
| BM_ThunkPath | 3.09 ns | 3.10 ns | 1.77 % | **+0.49 ns** |

**A thunk saves 0.90 ns/call over `std::function` (−23% of the dispatch
overhead).**

⚠️ **Important correction: the original "~4 ns of `std::function` overhead"
estimate was too optimistic about the gain.** x64 compilers already handle
`std::function`'s SBO + devirtualisation very well. The actual dispatch
overhead is ~1.4 ns, ~0.5 ns with a thunk.

### Platform 2: Android ARM64 (Snapdragon 8 Gen 3, Clang -O2, Release)

_To be measured — run once the device is connected over USB:_
```bash
adb push out/build/android-arm64-bench/benchmark/bench_callback_storage /data/local/tmp/
adb shell /data/local/tmp/bench_callback_storage --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

Past experience is that ARM64 Clang optimises `std::function` less well than
x64 MSVC, so the delta is expected to exceed x86_64's 0.90 ns. **This is the
deciding point for Step 5a.**

(The ARM64 result was later measured at +1.52 ns; see
[`fr-010-step5-microbench-report.md`](./fr-010-step5-microbench-report.md).)

### Step 5a's expected gain, recalibrated

The Windows microbenchmark result applied back to Splice end to end:

| | Now | After Step 5a (estimated) | Gate (< 20 ns) |
|---|---|---|---|
| Windows x86_64 1-thread | 23.7 ns | **~22.8 ns** (−0.9) | ❌ still not met |
| Android ARM64 1-thread | (no baseline yet, to be measured) | TBD | TBD |

**Conclusion: on x86_64, Step 5a alone is not enough to meet the < 20 ns
gate.** Other optimisations are needed (fewer atomic loads on the trampoline
path, a faster thread-local cache for `get_hook`, and so on).

If the ARM64 microbenchmark showed `std::function` overhead above 5 ns
(considered likely at the time), Step 5a would be very effective on ARM64 —
and ARM64 is Splice's main battleground (Android game enhancement).

**Decision path:**
1. Run the ARM64 microbenchmark
2. If the ARM64 delta > 3 ns → strongly recommend Step 5a
3. If the ARM64 delta < 1 ns → consider skipping Step 5 and doing Step 6 (RCU registry, attacking the 8t ratio)

### Where is the microbenchmark?

- Source: `benchmark/bench_callback_storage.cpp`
- Windows preset: `windows-x64-bench`
- Android preset: `android-arm64-bench` (added for this work)
- Re-runnable, and does not touch Splice's main path.
