// ─── x86_64 atomic patch sequence — implementation ───────────────────────
//
// Compiled on any host where SPLICE_HAS_X86_64_BACKEND is set — uses
// std::atomic_ref<uint64_t> which lowers to a single aligned `mov qword`
// on x86 with TSO ordering.
// ───────────────────────────────────────────────────────────────────────────

// NOMINMAX must precede any include that might reach <windows.h>.
#ifndef NOMINMAX
#   define NOMINMAX
#endif

#include "atomic_patch.h"

#include <splice/log.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>

namespace splice::arch::x86_64 {

bool atomic_install_jmp_rel32(void* target, void* new_func) noexcept {
    // ── Precondition 1: 8-byte alignment ──────────────────────────────────
    const auto target_addr = reinterpret_cast<std::uintptr_t>(target);
    if ((target_addr & 0x7u) != 0) {
        SPLICE_LOGW("x86_64 atomic_install: target=%p not 8-byte aligned (addr & 7 = %u); "
                    "atomic path unavailable",
                    target, static_cast<unsigned>(target_addr & 0x7u));
        return false;
    }

    // ── Precondition 2: rel32 fits in int32 ──────────────────────────────
    // E9 rel32 lands at: source + 5 + rel32, where source == target.
    const auto src = static_cast<std::int64_t>(target_addr);
    const auto dst = reinterpret_cast<std::int64_t>(new_func);
    const std::int64_t disp = dst - (src + 5);
    if (disp < std::numeric_limits<std::int32_t>::min() ||
        disp > std::numeric_limits<std::int32_t>::max()) {
        SPLICE_LOGW("x86_64 atomic_install: rel32 out of range (disp=%lld); "
                    "caller must use 14-byte abs64 fallback",
                    static_cast<long long>(disp));
        return false;
    }

    SPLICE_LOGV("x86_64 atomic_install: target=%p new_func=%p rel32=0x%x",
                target, new_func, static_cast<unsigned>(disp));

    // ── Step 1: read original 8 bytes ────────────────────────────────────
    // Treated as data; no atomic ordering needed here. We only need the
    // upper 3 bytes (target+5..7) preserved in the new 8-byte word.
    std::uint64_t original = 0;
    std::memcpy(&original, target, 8);

    // ── Step 2: construct the new 8-byte word in registers ────────────────
    // Layout (little-endian, x86):
    //   byte 0       = 0xE9                     (rel32 jmp opcode)
    //   bytes 1..4   = rel32 (sign-extended low-32)
    //   bytes 5..7   = original bytes 5..7      (preserve)
    //
    // Building via memcpy of an 8-byte staging buffer keeps the byte order
    // explicit and platform-independent (won't break if anyone ever
    // cross-compiles to a big-endian x86 ABI, which would be weird but...).
    std::uint8_t new_bytes[8];
    new_bytes[0] = 0xE9;
    const auto rel32 = static_cast<std::int32_t>(disp);
    std::memcpy(new_bytes + 1, &rel32, 4);  // bytes 1..4
    // Preserve bytes 5..7 from the original word.
    new_bytes[5] = static_cast<std::uint8_t>((original >> 40) & 0xFFu);
    new_bytes[6] = static_cast<std::uint8_t>((original >> 48) & 0xFFu);
    new_bytes[7] = static_cast<std::uint8_t>((original >> 56) & 0xFFu);

    std::uint64_t new_word = 0;
    std::memcpy(&new_word, new_bytes, 8);

    // ── Step 3: atomic 8-byte write ──────────────────────────────────────
    // std::atomic_ref<uint64_t>::store(release) on x86 lowers to a single
    // aligned `mov qword ptr [target], reg` instruction. Per Intel SDM
    // Vol 3A §8.1.1, aligned 8-byte stores are atomic with respect to
    // instruction fetch. TSO gives us release semantics for free.
    std::atomic_ref<std::uint64_t> word_ref(*static_cast<std::uint64_t*>(target));
    word_ref.store(new_word, std::memory_order_release);

    SPLICE_LOGV("x86_64 atomic_install: done");
    return true;
}

bool atomic_disable_inline(void* target,
                           const unsigned char* pre_hook_bytes,
                           unsigned int len) noexcept {
    if (target == nullptr || pre_hook_bytes == nullptr) {
        SPLICE_LOGE("x86_64 atomic_disable: null target or pre_hook_bytes");
        return false;
    }
    if (len == 0 || len > 16) {
        SPLICE_LOGE("x86_64 atomic_disable: invalid len=%u (must be 1..16)", len);
        return false;
    }

    const auto target_addr = reinterpret_cast<std::uintptr_t>(target);
    if ((target_addr & 0x7u) != 0) {
        // Target not 8-byte aligned — the aligned quadword atomic store is
        // unavailable, exactly as in atomic_install_jmp_rel32. Mirror the
        // installer's non-atomic fallback (x86_64/patcher.cpp): restore the
        // saved prologue via a plain byte copy. The residual hazard — a
        // concurrent in-flight instruction fetch could observe torn state —
        // is the same one the non-atomic install path already documents.
        // The engine has made the page writable and flushes the i-cache
        // after we return. Functions are 16-byte aligned under default MSVC /
        // Clang / GCC codegen, so this path is rare (it shows up for tiny
        // funcs the linker packs tightly, e.g. some -O0 ELF builds).
        SPLICE_LOGW("x86_64 atomic_disable: target=%p not 8-byte aligned; using "
                    "non-atomic memcpy restore (%u bytes). Caller threads may "
                    "observe torn state.",
                    target, len);
        std::memcpy(target, pre_hook_bytes, len);
        return true;
    }

    SPLICE_LOGV("x86_64 atomic_disable: target=%p len=%u", target, len);

    // ── Step 1: restore bytes [8..len) non-atomically ────────────────────
    // These bytes are unreachable from execution flow while the active
    // patch at [0..4] (atomic install) or [0..13] (abs64 install) is in
    // place. TSO ordering guarantees these stores are globally visible
    // before the atomic store in step 3 lands.
    if (len > 8) {
        std::memcpy(static_cast<std::uint8_t*>(target) + 8,
                    pre_hook_bytes + 8,
                    len - 8);
    }

    // ── Step 2: build the new 8-byte word from saved bytes [0..min(len,8)) ──
    // For len < 8, the upper bytes of the word stay as they currently are
    // on disk (which is whatever the install left there — either preserved
    // original bytes for the atomic-install case, or the tail of an abs64
    // jmp for the non-atomic case). We always merge with saved bytes when
    // available: if len >= 8 use saved[0..7] verbatim; if len < 8 use
    // saved[0..len-1] for the low part, current[len..7] for the high.
    std::uint64_t new_word = 0;
    if (len >= 8) {
        std::memcpy(&new_word, pre_hook_bytes, 8);
    } else {
        std::uint8_t buf[8];
        std::memcpy(buf, target, 8);                  // current bytes
        std::memcpy(buf, pre_hook_bytes, len);         // overlay saved low bytes
        std::memcpy(&new_word, buf, 8);
    }

    // ── Step 3: aligned 8-byte atomic store ──────────────────────────────
    // Mirrors atomic_install — Intel SDM §8.1.1 single-copy atomicity for
    // aligned quadword stores. Release ordering pairs with subsequent
    // hardware instruction fetches from this address.
    std::atomic_ref<std::uint64_t> word_ref(*static_cast<std::uint64_t*>(target));
    word_ref.store(new_word, std::memory_order_release);

    SPLICE_LOGV("x86_64 atomic_disable: done");
    return true;
}

} // namespace splice::arch::x86_64
