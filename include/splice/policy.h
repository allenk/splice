// ─── Splice concurrency policy tags ────────────────────────────────────────
//
// FR-010 (Phase 3.5). The hot-path callback storage is a *policy* — three
// alternatives, each picked at compile time:
//
//   policy::rcu_writeonce  (default) — reader = single atomic acquire-load.
//                                      writer = atomic store of new pointer.
//                                      callback objects intentionally leak
//                                      (Splice hook callbacks live for
//                                      program lifetime; the leak is
//                                      bounded by the hook count).
//   policy::rcu_refcounted           — reader = atomic<shared_ptr> load
//                                      (refcount RMW).  writer = swap.
//                                      Zero leak; readers slightly slower.
//                                      Requires C++20 std::atomic<shared_ptr>.
//   policy::shared_mutex             — classic reader-writer lock.
//                                      No leak, immediate visibility.
//                                      Use for callbacks that get swapped
//                                      at runtime (rare).
//
// User-facing API:
//   SPLICE_HOOK(func)                          → uses SPLICE_DEFAULT_POLICY
//   SPLICE_HOOK_AS<splice::policy::shared_mutex>(func) → explicit override
//
// Switching at runtime is *deliberately* not supported — see
// docs/v2-design-rationale.md §"Why not runtime-
// switchable" for the reasoning. Build-flag and per-call-site overrides
// cover the legitimate use cases.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

namespace splice::policy {

struct rcu_writeonce {};
struct rcu_refcounted {};
struct shared_mutex {};

} // namespace splice::policy

// Process-wide default. Override at build time:
//   add_compile_definitions(SPLICE_DEFAULT_POLICY=::splice::policy::shared_mutex)
#ifndef SPLICE_DEFAULT_POLICY
#   define SPLICE_DEFAULT_POLICY ::splice::policy::rcu_writeonce
#endif
