// ─── ELF GOT/PLT patcher ──────────────────────────────────────────────────
//
// For any address `target`, scan all loaded ELF objects via `dl_iterate_phdr`
// and patch the first GOT entry that happens to point at `target`. This is
// the "Tier 1" hook strategy: safe, atomic pointer write, no trampoline.
//
// Only compiled when SPLICE_HAS_POSIX_HOOK is set.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

namespace splice::engine {

// Locate the GOT slot whose current value equals `func_addr`. Returns the
// address of the slot (a writable `void**`), or nullptr if no match is found.
void** find_got_entry_for_address(const void* func_addr);

// Atomically overwrite a GOT slot. Returns true on success.
// The slot's page is flipped to RW for the write, then restored to RO.
bool patch_got_entry(void** got_entry, void* new_func);

} // namespace splice::engine
