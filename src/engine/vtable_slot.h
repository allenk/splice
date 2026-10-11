// ─── Pointer-to-virtual-member → vtable slot index ─────────────────────────
//
// The user writes `&IDXGISwapChain3::Present`. What that value *is* depends
// entirely on the C++ ABI, so this is the one place that knows, and it refuses
// rather than guesses when the shape is not one it has been shown.
//
// All three forms below were measured on 2026-10-11, not read off a reference:
//
//   Itanium (gcc/clang, Linux x86_64)   { ptr = 8*slot + 1, adj = 0 }
//   AArch64 / ARM (Android, Linux)      { ptr = 8*slot,     adj = 1 }
//   MSVC x64                            ptr = a thunk, decoded below
//
// The AArch64 difference is the one worth naming: the generic Itanium rule
// puts the "this is virtual" bit in the low bit of the function pointer, and
// the ARM C++ ABI moves it into the low bit of the this-adjustment instead,
// because on 32-bit ARM the low bit of a code address already means Thumb.
// AArch64 inherited that choice. Applying the x86_64 rule there yields a slot
// index off by nothing visible on a small interface and wrong everywhere else
// -- on the architecture this library ships on.
//
// Nothing above this file needs to know any of it: the answer is an int.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

namespace splice::engine {

// The vtable slot index a pointer-to-member-function names, or -1 when it
// cannot be resolved: a non-virtual member, an inheritance shape that needs a
// this-adjustment, an unrecognised thunk, or an architecture whose ABI this
// file has not been taught. A refusal is not a defect to work around -- the
// caller's escape hatch is to name the index by hand, with the signature still
// checked by the compiler.
int slot_of_member_pointer(const void* pmf_bytes, unsigned int pmf_size);

} // namespace splice::engine
