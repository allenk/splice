// ─── What splice has already patched, and following a jump safely ──────────
//
// Two things engine.cpp kept to itself until the group transaction needed them,
// and both are needed together because one guards the other.
//
// `unwrap_jump_stub` exists because the address a caller hands us is often not
// the function. An incremental-linked build routes a cross-translation-unit
// `&function` through an ILT thunk -- a bare `E9 rel32` -- and an import lands
// on an `FF 25` thunk. Patching the thunk instead of the function installs a
// hook almost nothing calls.
//
// **It must never follow one of splice's own patches**, and that is what the
// site registry is for. An installed patch IS an `E9` or an `FF 25`, so a
// second install would walk `patched function -> relay -> hook stub` and treat
// whatever the stub begins with as "the real function" -- observed patching
// splice's own relay and then overflowing the stack. A byte test cannot make
// that distinction; only a record of what we wrote can.
//
// So any install path that unwraps must also register what it patched. A path
// that unwraps without registering is the 2026-09-28 defect with the guard
// removed.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

namespace splice::engine {

// Follow ILT and import thunks to the function behind them, stopping at
// anything splice itself wrote. Returns `target` unchanged when it is already
// the function, and on architectures with no thunk forms to follow.
void* unwrap_jump_stub(void* target);

// Record that `addr` now holds a patch of ours routed to `routed_to`, so
// unwrap_jump_stub stops there instead of following it.
void remember_patched_site(const void* addr, void* routed_to);

// Forget it, because the original bytes are back and a later install at this
// address is legitimate again.
void forget_patched_site(const void* addr);

// Is this address one splice has already patched? A second inline install on
// the same address is refused rather than chained -- chaining is wanted and is
// a design question, not something to inherit by accident.
bool is_own_patch_site(const void* addr);

} // namespace splice::engine
