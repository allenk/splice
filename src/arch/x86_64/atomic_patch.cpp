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

namespace {

// The hardware guarantee is about an aligned quadword, not about `target`.
//
// Intel SDM Vol 3A 8.1.1 promises single-copy atomicity for a quadword aligned
// on an 8-byte boundary. What has to live inside one such quadword is the
// PATCH, not the function entry -- so a 5-byte `E9 rel32` starting at
// `target & 7 == 3` is still one atomic store away, even though `target` itself
// is unaligned. The requirement is simply offset + length <= 8.
//
// This matters more than it sounds. Splice used to demand `target & 7 == 0` and
// fall back to a non-atomic memcpy otherwise. MSVC aligns function entries to
// 16 bytes so Windows never noticed; GCC at -O0 packs small functions tightly,
// and in Splice's own Linux suite only 4 of 13 distinct hook targets were
// 8-byte aligned -- 10 of 14 installs took the torn-state path for no reason
// other than an over-strict precondition.
struct QuadwordSlot {
    std::uint64_t* word;    // the aligned quadword containing the patch
    unsigned       offset;  // where the patch starts inside it, 0..7
    unsigned       room;    // bytes available from `offset` to the word's end
};

QuadwordSlot quadword_slot_for(void* target) noexcept {
    const auto addr = reinterpret_cast<std::uintptr_t>(target);
    const unsigned offset = static_cast<unsigned>(addr & 0x7u);
    return QuadwordSlot{
        reinterpret_cast<std::uint64_t*>(addr - offset),
        offset,
        8u - offset,
    };
}

// Overlay `len` bytes at `slot.offset` inside the quadword, atomically.
//
// A compare-exchange loop rather than a plain store, because the bytes of the
// quadword we are NOT changing do not all belong to us: bytes before
// `slot.offset` belong to whatever precedes the function -- padding, or the tail
// of a tightly packed neighbour that Splice might be patching at the same
// moment. Reading them and storing them back would silently undo a concurrent
// change to them. The loop costs nothing at install time and removes the
// question.
void atomic_overlay(const QuadwordSlot& slot, const std::uint8_t* bytes,
                    unsigned len) noexcept {
    std::atomic_ref<std::uint64_t> word_ref(*slot.word);
    std::uint64_t expected = word_ref.load(std::memory_order_relaxed);
    for (;;) {
        std::uint8_t staging[8];
        std::memcpy(staging, &expected, 8);
        std::memcpy(staging + slot.offset, bytes, len);
        std::uint64_t desired = 0;
        std::memcpy(&desired, staging, 8);
        if (word_ref.compare_exchange_weak(expected, desired,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
            return;
        }
        // `expected` now holds the current value; rebuild the overlay on it.
    }
}

}  // namespace

bool atomic_install_jmp_rel32(void* target, void* new_func) noexcept {
    // ── Precondition 1: the 5-byte patch fits in one aligned quadword ─────
    // Not "target is aligned" -- see quadword_slot_for above. offset <= 3 is
    // exactly the condition offset + 5 <= 8.
    const auto target_addr = reinterpret_cast<std::uintptr_t>(target);
    const QuadwordSlot slot = quadword_slot_for(target);
    if (slot.room < 5) {
        SPLICE_LOGW("x86_64 atomic_install: target=%p sits %u bytes into its "
                    "quadword, so a 5-byte patch would straddle two; atomic "
                    "path unavailable",
                    target, slot.offset);
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

    // ── Step 1+2: build the 5 patch bytes ────────────────────────────────
    //   byte 0     = 0xE9            (rel32 jmp opcode)
    //   bytes 1..4 = rel32           (sign-extended low-32)
    //
    // Everything else in the quadword is left exactly as it is found, which is
    // what atomic_overlay does. Nothing outside these five bytes is ours to
    // change: bytes after the patch are the function's own continuation (the
    // trampoline returns into them), and bytes before it, when target is
    // unaligned, belong to whatever precedes the function.
    std::uint8_t patch[5];
    patch[0] = 0xE9;
    const auto rel32 = static_cast<std::int32_t>(disp);
    std::memcpy(patch + 1, &rel32, 4);

    // ── Step 3: one atomic read-modify-write on the containing quadword ──
    // Per Intel SDM Vol 3A §8.1.1 an aligned quadword access is atomic with
    // respect to instruction fetch, so no CPU can observe the patch half
    // written. TSO gives release semantics for free.
    atomic_overlay(slot, patch, 5);

    SPLICE_LOGV("x86_64 atomic_install: done (word=%p offset=%u)",
                static_cast<void*>(slot.word), slot.offset);
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

    const QuadwordSlot slot = quadword_slot_for(target);
    if (slot.room < 5) {
        // The 5 live patch bytes straddle two quadwords, so no single atomic
        // access covers them — the same condition that made the install
        // non-atomic, and reached for the same targets. Mirror the installer's
        // fallback: restore by plain byte copy, with the same residual hazard
        // that path already documents. The engine has made the page writable
        // and flushes the i-cache after we return.
        SPLICE_LOGW("x86_64 atomic_disable: target=%p sits %u bytes into its "
                    "quadword; using non-atomic memcpy restore (%u bytes). "
                    "Caller threads may observe torn state.",
                    target, slot.offset, len);
        std::memcpy(target, pre_hook_bytes, len);
        return true;
    }

    SPLICE_LOGV("x86_64 atomic_disable: target=%p len=%u offset=%u",
                target, len, slot.offset);

    // ── Step 1: restore the bytes past the quadword, non-atomically ──────
    // Everything from the end of the containing quadword onwards is
    // unreachable while the live patch still sits at [0..5): flow leaves the
    // function before reaching it. TSO guarantees these stores are globally
    // visible before the atomic overlay in step 3 lands.
    //
    // This is the same trick the install side uses for the relay, in reverse:
    // put the non-atomic writes where nobody is executing, then flip with one
    // atomic access.
    const unsigned inside = len < slot.room ? len : slot.room;
    if (len > inside) {
        std::memcpy(static_cast<std::uint8_t*>(target) + inside,
                    pre_hook_bytes + inside,
                    len - inside);
    }

    // ── Step 2: the bytes that go back inside the quadword ───────────────
    // Only the saved prologue's first `inside` bytes. Everything else in the
    // quadword is left as found: bytes past the patch were either restored in
    // step 1 or were never disturbed, and bytes before it -- when target is
    // unaligned -- were never ours.

    // ── Step 3: one atomic read-modify-write, and the hook is gone ───────
    // Mirrors atomic_install. After this lands, flow follows the original
    // prologue again; no CPU can observe a half-restored instruction.
    atomic_overlay(slot, pre_hook_bytes, inside);

    SPLICE_LOGV("x86_64 atomic_disable: done (word=%p offset=%u inside=%u)",
                static_cast<void*>(slot.word), slot.offset, inside);
    return true;
}

} // namespace splice::arch::x86_64
