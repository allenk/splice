// ─── POSIX memory / cache primitives ──────────────────────────────────────
//
// Thin wrappers around mmap / mprotect / __builtin___clear_cache that the
// rest of Splice uses. Keeps OS calls isolated from the arch backends.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstddef>

namespace splice::os {

// How far "near" reaches, for the near_addr argument below. A 32-bit signed
// displacement spans +/-2 GB; the slack keeps the whole allocation in range
// rather than only its first byte.
inline constexpr std::size_t kNearReach = 0x7FFF0000U;

// Allocate RWX-mapped anonymous pages. Returns nullptr on failure.
//
// `near_addr`, when non-null, asks for the pages within kNearReach of it.
// This is a request, not a promise: when no free region is available in range
// the allocator falls back to letting the OS choose, so the caller must still
// cope with a far result -- and, on x86_64, that is a behaviour difference
// rather than merely a slower path (see below).
//
// Why an inline patcher cares, twice over:
//
//   1. A relocated prologue has to keep reaching its own module. Copying an
//      instruction into a trampoline rewrites its rel32 and rip-relative
//      operands to point back at the original target; from beyond 2 GB that
//      rewrite does not fit in int32 and the install fails outright. MSVC
//      prologues reach for rip-relative globals routinely, /GS cookies among
//      them, so a far trampoline makes ordinary functions unhookable.
//
//   2. It decides whether the patch can be installed atomically. Reaching a
//      far destination needs the 14-byte `FF 25` jump, and 14 bytes cannot be
//      written as one store; a thread already executing the target can
//      observe a torn instruction. Five bytes fit inside one aligned
//      quadword. See the relay in src/arch/x86_64/patcher.cpp.
//
// Not used by the ARM64 backend, and not because a hint would be too weak
// there -- because it needs none. arch/arm64 installs a 16-byte literal-pool
// indirect branch (ldr x17, #8 / br x17 / .quad target) whose reach is the
// whole address space, and atomicity comes from writing the literal first and
// then the single 32-bit instruction. Neither problem above exists, so the
// ARM64 patcher asks for no hint and this argument stays at its default.
void* allocate_executable_memory(std::size_t size,
                                 const void* near_addr = nullptr);

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
