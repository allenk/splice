// ─── Pointer-slot patching, with the page left as it was found ─────────────
//
// One operation: store a function pointer into a slot that splice does not
// own, atomically, and hand back what was there before.
//
// This is deliberately separate from got_patcher.h and iat_patcher.h, which
// perform the same store on their own slot kinds. Those two know where their
// slot lives -- a GOT or an import table, on a page the loader has already
// decided the protection of -- and both restore read-only afterwards because
// for those pages that is the right answer.
//
// A vtable is the case where it is not. The same interface can have its
// function table in a module's read-only data, in ordinary writable data, or
// on the heap when the object was built at run time, and restoring the wrong
// one either refuses a legitimate hook or hands the program a read-only page
// it expects to write. So this one asks what the protection is and puts that
// back. See splice::os::make_data_writable.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

namespace splice::engine {

// Store `new_func` into `*slot` and report the previous value through
// `out_previous` (may be null). Returns false without writing anything when
// the slot cannot be made writable.
//
// The store itself is a single release store through std::atomic_ref: a reader
// that observes the new pointer also observes everything the caller published
// before calling, which for a hook means the original-function registry and
// the callback. Publishing in the other order is a live call through a null
// original, and that is a bug this project has already paid for once.
//
// Last writer wins, and the previous value is whatever was there at the
// instant of the store. Use this only when the caller has no earlier reading
// of the slot to honour; a caller that read the slot first wants the function
// below instead.
bool swap_pointer_slot(void** slot, void* new_func, void** out_previous);

// Store `new_func` into `*slot` only while it still holds `expected`, and
// report what was actually there through `found` (may be null) whether or not
// it matched. Returns false, having written nothing, on a mismatch or when the
// slot cannot be made writable.
//
// Why this exists, and why the unconditional form is not enough for an
// install: a group install reads the slot in one phase to learn the original
// function, publishes that original where the hook will look for it, and
// stores the new pointer in a later phase. Between the reading and the store,
// another overlay can take the same slot. An unconditional store then
// overwrites it without a word -- and worse, the original this hook publishes
// is the value from before that overlay arrived, so calling it skips their
// hook entirely. Neither outcome is detectable afterwards from the bytes.
//
// The comparison closes that window to the width of one atomic operation, and
// it is the pointer-slot equivalent of the second prologue comparison the
// inline tier performs immediately before it writes. A mismatch is somebody
// else's hook, which is a refusal and not an error to retry.
bool swap_pointer_slot_expect(void** slot, void* expected, void* new_func,
                              void** found);

} // namespace splice::engine
