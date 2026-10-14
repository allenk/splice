// ─── Splice registry implementation selector ──────────────────────────────
//
// FR-010 Step 6. Selects how `HookContext` stores its per-id hook slots
// (the m_hooks map under shared_mutex prior to this).
//
//   registry::shared_mutex_map  (default) — unordered_map<int,
//       shared_ptr<HookBase>> guarded by shared_mutex. Predictable, well-
//       tested, AOSP-toolchain safe. Reader pays an atomic RMW on the
//       shared_mutex counter — cache-line bouncing under contention is
//       the bottleneck FR-010 Step 6 attacks.
//
//   registry::rcu_atomic_array            — an array of atomic slot
//       pointers, one per id. Reader does a single
//       std::memory_order_acquire load; no atomic RMW on the hot path.
//       Writer does one release store into a slot.
//       Trade-offs: the capacity is a hard bound, and a hook object lives
//       for the life of the process.
//
//       It published an immutable snapshot and reclaimed retired ones
//       after a 100 ms grace period until 2026-10-13. That cannot be
//       sound: a reader loads the snapshot pointer and *then* dereferences
//       the array, and a thread preempted between those two steps outlives
//       any period. Reclaiming correctly needs reader registration or
//       hazard pointers, which costs the hot path the whole reason this
//       implementation exists -- so nothing is reclaimed. Elapsed time was
//       never a proof that no thread was still reading.
//
// The choice is build-time. Switching at runtime is deliberately not
// supported — see docs/fr-010-step6-rcu-registry-design.md §"Why build-
// time, not runtime".
//
// Choosing it, and why there is only one supported way:
//
//   cmake -DSPLICE_REGISTRY_IMPL=::splice::registry::shared_mutex_map
//
// That is a real CMake cache option. It is applied to the splice target as a
// PUBLIC compile definition and carried into the installed interface, so the
// library and every consumer of `splice::splice` compile against one value.
// This matters more than it looks: the choice changes `sizeof(HookContext)`
// -- 384 bytes against 416, measured with MSVC x64 and the release CRT, and a
// different pair under another standard library but never the same number --
// and `src/context.cpp` owns the single instance of that class which every
// translation unit shares. A consumer that set the macro by hand while the
// library was built the other way would read that instance's members at the
// wrong offsets -- no diagnostic, no crash until a function pointer read out
// of the originals table is called. Splice checks for that at first use and
// refuses every hook from the translation unit that disagrees
// (`detail::context_layout_agrees`), but the reason the check exists is that
// the build should never be able to disagree in the first place.
//
// The capacity is no longer part of that size. Until 2026-10-13 both slot
// arrays were members and this figure was 4472 bytes; they are a pointer and a
// runtime count now, which is why the lock-free context is the smaller of the
// two and why SPLICE_MAX_HOOKS no longer appears here at all.
//
// Until 2026-10-13 this header documented `cmake -D...` for a variable nothing
// in the build system read, so the flag silently did nothing and the lock-free
// registry had never once been compiled -- including by the FR-010 Step 6
// measurements that reported numbers for it.
//
// Parallels SPLICE_DEFAULT_POLICY (Step 3), with one difference worth stating
// because it misled this project for five months: the *policy* governs how the
// stored callable is read, and the *registry* governs finding it. Selecting a
// lock-free policy never selected a lock-free registry. Both are lock-free by
// default now.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

namespace splice::registry {

// Tag types — empty struct, picked at compile time via partial spec.
struct shared_mutex_map {};
struct rcu_atomic_array {};

} // namespace splice::registry

// Build-time default, as of 2026-10-13: the lock-free array. Measured on one
// commit with the registry as the only difference, single-thread callback
// dispatch is 4.3 ns against 23.5, and 8-thread dispatch 20 ns against ~1348 --
// the locked map was about 91% of all single-thread dispatch cost. It is also
// what makes FR-010's two gates reachable (< 20 ns, and 8t/1t < 5x).
//
// `shared_mutex_map` remains supported and is the escape hatch now, for the
// two things it still does better at the cost above: it takes any id, so there
// is no capacity to exceed, and it holds each hook in a shared_ptr, so
// replacing or clearing an entry frees it. Writer visibility is not one of
// them any more -- with the snapshot gone, a release store is as prompt as the
// lock was. The CMake option sets this; a build with no CMake in sight gets
// this default.
#ifndef SPLICE_REGISTRY_IMPL
#   define SPLICE_REGISTRY_IMPL ::splice::registry::rcu_atomic_array
#endif

// Capacity for the rcu_atomic_array impl. Decided 2026-05-19:
//   - 512 slots, 8 bytes each, stays comfortably in L1d (32-48 KiB)
//   - Covers AOSP system-service hooking (typical 300-800 hooks)
//   - Bump to 4096 for full-syscall tracing, but accept the L2 reader penalty
// See docs/fr-010-step6-rcu-registry-design.md §"Why selecting 512".
//
// This fallback is for a build that compiles the library without CMake. The
// CMake option is PRIVATE to the library's own target on purpose: since
// 2026-10-13 the slot arrays live behind a pointer and a runtime count, so no
// header-visible layout, inline expression or template argument depends on
// this value -- which means a translation unit compiled with a different one
// has nothing it can misread, and does not need to be told. Three ways of
// *detecting* such a disagreement were designed and refuted before that;
// removing the dependency needs none of them to work. Ask
// splice::detail::configured_capacity() for the value at runtime.
#ifndef SPLICE_MAX_HOOKS
#   define SPLICE_MAX_HOOKS 512
#endif
