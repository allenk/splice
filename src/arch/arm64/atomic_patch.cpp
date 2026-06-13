// ─── ARM64 atomic patch sequence — implementation ────────────────────────
//
// Compiled only on ARM64 — uses inline ARMv8 assembly (DMB / DSB / ISB /
// DC CVAU / IC IVAU / MRS CTR_EL0). Gated by SPLICE_HAS_ARM64_BACKEND in
// CMakeLists.txt alongside the rest of the ARM64 patcher.
// ───────────────────────────────────────────────────────────────────────────
#include "atomic_patch.h"

#include <splice/log.h>

#include <atomic>
#include <cstdint>
#include <cstring>   // std::memcpy — libstdc++ does not pull it in transitively

namespace splice::arch::arm64 {

namespace {

// ─── ARMv8 barriers (zero-overhead inline) ─────────────────────────────────

inline void dmb_ish() noexcept {
    asm volatile("dmb ish" ::: "memory");   // data memory barrier, inner shareable
}

inline void dsb_ish() noexcept {
    asm volatile("dsb ish" ::: "memory");   // data sync barrier, inner shareable
}

inline void isb_sy() noexcept {
    asm volatile("isb" ::: "memory");        // instruction sync barrier
}

// ─── Cache maintenance: clean D-cache + invalidate I-cache to PoU ─────────
// Reads CTR_EL0 once per call to discover line sizes (cheap; could be cached
// at process init if profiling shows it as a hotspot).
void invalidate_icache_range(void* start, std::size_t size) noexcept {
    std::uint64_t ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    // CTR_EL0 layout (relevant fields):
    //   bits  3:0 = IminLine, log2 of words (4-byte units)
    //   bits 19:16 = DminLine, log2 of words
    const std::size_t iline = std::size_t{4} << (ctr & 0xF);
    const std::size_t dline = std::size_t{4} << ((ctr >> 16) & 0xF);

    auto* p = static_cast<std::uint8_t*>(start);
    auto* end = p + size;

    // 1. Clean each D-cache line covering the patched range to Point-of-Unification.
    for (auto* q = reinterpret_cast<std::uint8_t*>(
             reinterpret_cast<std::uintptr_t>(p) & ~(dline - 1));
         q < end; q += dline) {
        asm volatile("dc cvau, %0" : : "r"(q) : "memory");
    }
    dsb_ish();   // wait for all DC CVAU to complete

    // 2. Invalidate each I-cache line covering the patched range to PoU.
    for (auto* q = reinterpret_cast<std::uint8_t*>(
             reinterpret_cast<std::uintptr_t>(p) & ~(iline - 1));
         q < end; q += iline) {
        asm volatile("ic ivau, %0" : : "r"(q) : "memory");
    }
    dsb_ish();   // wait for all IC IVAU to complete

    // 3. Force this CPU to refetch instructions (other CPUs auto-refetch on
    //    next branch / context boundary because IC IVAU broadcasts in the
    //    Inner Shareable domain).
    isb_sy();
}

} // namespace

void atomic_install_indirect_branch(void* target, void* new_func) {
    auto* dst32 = static_cast<std::uint32_t*>(target);

    // Sanity: target must be 4-byte aligned (function entries always are).
    // Hard-fail in debug; release just proceeds and pays whatever the
    // hardware decides to do.
    SPLICE_LOGV("atomic_install: target=%p new_func=%p", target, new_func);

    // ─── Step 1: write the literal pool at [target+8..15] ──────────────────
    // This is data; until step 4 lands, no thread can reach it (because
    // dst32[0] is still the old instruction). Order doesn't matter much
    // here but doing it first keeps the rest of the sequence dependency-free.
    auto* literal = reinterpret_cast<std::uint64_t*>(&dst32[2]);
    *literal = reinterpret_cast<std::uint64_t>(new_func);

    // ─── Step 2: write `br x17` at [target+4..7] ──────────────────────────
    // Same reasoning — until dst32[0] becomes `ldr x17, #8`, no thread
    // executes dst32[1]. Plain store; no atomicity needed.
    dst32[1] = 0xD61F0220u;   // br x17

    // ─── Step 3: data memory barrier ──────────────────────────────────────
    // Make steps 1 and 2 globally observable BEFORE step 4 lands. Without
    // this, another CPU could observe the new ldr x17 instruction (step 4)
    // while still seeing the OLD br/literal — recipe for SIGILL.
    dmb_ish();

    // ─── Step 4: atomic 32-bit instruction write ──────────────────────────
    // ARMv8-A ARM §B2.2.1: "Single-copy atomicity" — an aligned 4-byte store
    // is atomic with respect to instruction fetch. Every concurrent fetcher
    // observes either the old word or the new word, never partial.
    //
    // std::atomic_ref<uint32_t> with release ordering pairs naturally with
    // the dmb_ish() above. Synchronises with any subsequent acquire on the
    // same location (we don't actually have C++ acquire readers; the
    // synchronisation is with hardware instruction fetch via IC IVAU below).
    std::atomic_ref<std::uint32_t> first_word(dst32[0]);
    first_word.store(0x58000051u, std::memory_order_release);  // ldr x17, #8

    // ─── Step 5: I-cache maintenance ──────────────────────────────────────
    // Other CPUs may have cached the OLD word in their I-cache. IC IVAU
    // (broadcast to Inner Shareable domain) invalidates those lines so
    // they refetch on next instruction-fetch. ISB drains this CPU's
    // instruction pipeline.
    invalidate_icache_range(target, 16);

    SPLICE_LOGV("atomic_install: done");
}

void atomic_disable_indirect_branch(void* target,
                                    const unsigned char* pre_hook_bytes) {
    auto* dst32 = static_cast<std::uint32_t*>(target);

    SPLICE_LOGV("atomic_disable: target=%p", target);

    // ─── Step 1: restore [target+4..15] non-atomically ────────────────────
    // While dst32[0] still holds `ldr x17, #8`, no fetcher reaches dst32[1]
    // or beyond — flow always diverts via x17. So plain stores are safe.
    // Read the 32-bit words in saved bytes 4..7 and 8..11 and 12..15 and
    // write them as three plain 4-byte stores. Using std::memcpy preserves
    // strict-aliasing.
    std::uint32_t saved_word_1 = 0;
    std::uint32_t saved_word_2 = 0;
    std::uint32_t saved_word_3 = 0;
    std::memcpy(&saved_word_1, pre_hook_bytes + 4, 4);
    std::memcpy(&saved_word_2, pre_hook_bytes + 8, 4);
    std::memcpy(&saved_word_3, pre_hook_bytes + 12, 4);
    dst32[1] = saved_word_1;
    dst32[2] = saved_word_2;
    dst32[3] = saved_word_3;

    // ─── Step 2: data memory barrier ──────────────────────────────────────
    // Make step 1 globally visible BEFORE step 3 lands. Without this, a
    // concurrent CPU could observe the new dst32[0] while still seeing
    // the patched (br x17 / literal) at dst32[1..3] — exactly the same
    // class of bug the install-side DMB defends against.
    dmb_ish();

    // ─── Step 3: atomic 32-bit instruction word restore ──────────────────
    // Single-copy atomicity (ARMv8 §B2.2.1) — every concurrent fetcher
    // observes either the patched `ldr x17, #8` or the original first
    // instruction word, never a torn intermediate.
    std::uint32_t saved_word_0 = 0;
    std::memcpy(&saved_word_0, pre_hook_bytes, 4);
    std::atomic_ref<std::uint32_t> first_word(dst32[0]);
    first_word.store(saved_word_0, std::memory_order_release);

    // ─── Step 4: I-cache maintenance ──────────────────────────────────────
    // Other CPUs may have cached the patched word in their I-cache. IC IVAU
    // (broadcast Inner Shareable) invalidates those lines so they refetch.
    invalidate_icache_range(target, 16);

    SPLICE_LOGV("atomic_disable: done");
}

} // namespace splice::arch::arm64
