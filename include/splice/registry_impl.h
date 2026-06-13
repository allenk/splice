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
//   registry::rcu_atomic_array            — atomic<Snapshot*> publish-
//       and-forget. Reader does a single std::memory_order_acquire load;
//       no atomic RMW on the hot path. Writer copies the snapshot,
//       publishes the new pointer, and defers reclamation via EBR.
//       Trade-offs: higher writer latency, ~2× transient memory during
//       grace period, snapshot capped at SPLICE_MAX_HOOKS.
//
// The choice is build-time. Switching at runtime is deliberately not
// supported — see docs/fr-010-step6-rcu-registry-design.md §"Why build-
// time, not runtime".
//
// User-facing escape hatch:
//   cmake -DSPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array
//   add_compile_definitions(SPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array)
//
// Parallels SPLICE_DEFAULT_POLICY (Step 3) — both follow the engineering
// rule: any optimisation that has functional trade-offs ships behind an
// escape hatch with the surprise-free default.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

namespace splice::registry {

// Tag types — empty struct, picked at compile time via partial spec.
struct shared_mutex_map {};
struct rcu_atomic_array {};

} // namespace splice::registry

// Build-time default. Override at the consumer level:
//   add_compile_definitions(SPLICE_REGISTRY_IMPL=::splice::registry::rcu_atomic_array)
#ifndef SPLICE_REGISTRY_IMPL
#   define SPLICE_REGISTRY_IMPL ::splice::registry::shared_mutex_map
#endif

// Snapshot bound for the rcu_atomic_array impl. Decided 2026-05-19:
//   - 512 × 16 bytes (shared_ptr on x64) = 8 KiB snapshot
//   - Fits comfortably in L1d (32-48 KiB) → reader lookup stays at ~1 ns
//   - Covers AOSP system-service hooking (typical 300-800 hooks)
//   - Bump to 4096 for full-syscall tracing, but accept L2 reader penalty
// See docs/fr-010-step6-rcu-registry-design.md §"Why selecting 512".
#ifndef SPLICE_MAX_HOOKS
#   define SPLICE_MAX_HOOKS 512
#endif
