// ─── POSIX memory / cache primitives ──────────────────────────────────────
//
// Thin wrappers around mmap / mprotect / __builtin___clear_cache that the
// rest of Splice uses. Keeps OS calls isolated from the arch backends.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstddef>

namespace splice::os {

// Allocate RWX-mapped anonymous pages. Returns nullptr on failure.
void* allocate_executable_memory(std::size_t size);

// Free memory previously returned by allocate_executable_memory().
void free_executable_memory(void* mem, std::size_t size);

// Flip a page containing `addr..addr+size` to PROT_READ|PROT_WRITE|PROT_EXEC
// so we can overwrite it. Best-effort; returns false if mprotect fails.
bool make_executable_writable(void* addr, std::size_t size);

// Restore PROT_READ|PROT_EXEC on the page(s) touched by a prior
// make_executable_writable(). Non-fatal if it fails.
void restore_executable(void* addr, std::size_t size);

// Invalidate the instruction cache over [addr, addr+size). Wraps GCC's
// __builtin___clear_cache on POSIX.
void flush_instruction_cache(void* addr, std::size_t size);

// Page granularity on the running system.
std::size_t page_size();

} // namespace splice::os
