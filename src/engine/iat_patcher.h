// ─── PE/COFF IAT patcher ──────────────────────────────────────────────────
//
// Windows equivalent of `got_patcher.h`. Walks every loaded module via
// `EnumProcessModules`, parses each PE's Import Descriptors, and locates
// the Import Address Table slot currently pointing at `func_addr`.
//
// Semantics match the POSIX GOT path: it's a Tier 1 safe hook —
//  - atomic pointer-sized write, no trampoline
//  - only affects calls that go through the IAT (inter-module imports),
//    not intra-module direct calls
//  - `disable()` (FR-013 Tier 1) can reverse the swap symmetrically
//
// Compiled only when SPLICE_HAS_WIN32_OS is ON.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

namespace splice::engine {

// Locate the IAT slot whose current value equals `func_addr`. Returns the
// address of the slot (a writable `void**`), or nullptr if no match is
// found across any loaded module.
void** find_iat_entry_for_address(const void* func_addr);

// Atomically overwrite an IAT slot. Returns true on success.
// The slot's page is flipped to RW for the write, then restored to RO.
bool patch_iat_entry(void** iat_entry, void* new_func);

} // namespace splice::engine
