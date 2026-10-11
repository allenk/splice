#pragma once
// ─── pointer-to-member → vtable slot index, for tests that assert the number ──
//
// The engine's resolver reached directly, rather than through `splice::Vtable`.
// The difference is what a test can then claim: an install that worked has not
// shown which slot it chose, and against a published ABI the slot number is
// the thing worth pinning. `Vtable` resolves and installs in one step, so a
// test written only through it cannot tell a correct resolution from a
// coincidence.
//
// Shared by the COM runners, which both need it on interfaces whose layout is
// not ours to choose.
#include <splice/engine.h>

#include <cstring>

namespace splice::test {

template <typename PMF>
int slot_of(PMF pmf) {
    unsigned char raw[sizeof(PMF)];
    std::memcpy(raw, &pmf, sizeof(PMF));
    return splice_vtable_slot_of_pmf(raw, static_cast<unsigned int>(sizeof(PMF)));
}

} // namespace splice::test
