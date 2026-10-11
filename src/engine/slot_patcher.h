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
bool swap_pointer_slot(void** slot, void* new_func, void** out_previous);

} // namespace splice::engine
