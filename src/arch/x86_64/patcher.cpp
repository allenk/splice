// ─── x86_64 inline-patch installer — implementation ──────────────────────
//
// Allocates a trampoline, relocates `target`'s prologue into it with
// PC-relative fixup, appends a jump-back to (target + prologue_size), and
// overwrites the prologue with a jump to `new_func`.
//
// Atomicity note (FR-011): the prologue overwrite is a plain memcpy — not
// atomic under concurrent execution of `target`. Phase 4.5 replaces it
// with a proper int3 + xchg sequence per Intel's "cross-modifying code"
// guidance.
// ───────────────────────────────────────────────────────────────────────────
#include "patcher.h"

#include "atomic_patch.h"
#include "disasm.h"
#include "../../os/memory.h"

#include <splice/log.h>

#include <cstdint>
#include <cstring>
#include <limits>

namespace splice::arch::x86_64 {

namespace {

// Worst-case trampoline size: original prologue (up to 64 bytes from the
// walk limit in disasm.cpp) plus a 14-byte absolute jmp back plus headroom
// for promoted rel8 → rel32 branches. 128 bytes is ample.
constexpr std::size_t kTrampolineReserve = 128;

// Emit the prologue copy with PC-relative fixup. Returns the number of
// bytes written into `dst`, or SIZE_MAX on fatal failure.
std::size_t emit_prologue_copy(std::uint8_t* dst, const std::uint8_t* src,
                               std::size_t copy_size, const void* src_base) {
    std::size_t src_off = 0;
    std::size_t dst_off = 0;

    while (src_off < copy_size) {
        const auto info = analyze_instruction(src + src_off, copy_size - src_off);
        if (info.length == 0 || info.type == InstructionType::Unknown) {
            SPLICE_LOGE("x86_64 emit_prologue_copy: decode fail at src_off=%zu", src_off);
            return static_cast<std::size_t>(-1);
        }

        const void* old_pc = static_cast<const std::uint8_t*>(src_base) + src_off;
        const void* new_pc = dst + dst_off;

        const std::size_t written = relocate_instruction(info, src + src_off,
                                                         old_pc, new_pc,
                                                         dst + dst_off);
        if (written == 0) {
            // In-place relocate failed. For rel8 branches we could promote
            // to rel32 here, but that grows the instruction by 3–4 bytes
            // and requires shifting everything after it. Rare in modern
            // compiler output for prologues — mostly matters for code that
            // hand-rolls its own prologue. Phase 3 scope deliberately
            // keeps this simple; surface as an error and let the caller
            // fall back to GOT/IAT patching or a bigger trampoline.
            SPLICE_LOGE("x86_64 emit_prologue_copy: cannot relocate type=%d at src_off=%zu",
                        static_cast<int>(info.type), src_off);
            return static_cast<std::size_t>(-1);
        }

        src_off += info.length;
        dst_off += written;
    }
    return dst_off;
}

} // namespace

void* install_inline_patch(void* target, void* new_func, void** original_func,
                           PrePatchFn on_trampoline_ready,
                           void* user_data,
                           unsigned char* pre_hook_bytes_out,
                           unsigned int* pre_hook_byte_len_out) {
    if (target == nullptr || new_func == nullptr) {
        SPLICE_LOGE("install_inline_patch: null target or new_func");
        return nullptr;
    }
    SPLICE_LOGV("x86_64 install_inline_patch: target=%p new_func=%p", target, new_func);

    // Step 1 — measure prologue.
    const std::size_t copy_size = calculate_copy_size(target);
    if (copy_size == 0) {
        SPLICE_LOGE("install_inline_patch: prologue decode failed");
        return nullptr;
    }

    // Step 1b — snapshot original bytes for FR-013 Tier 2 disable. Must
    // happen before any write to `target`. Cap at 16 (record buffer size).
    if (pre_hook_bytes_out != nullptr && pre_hook_byte_len_out != nullptr) {
        if (copy_size <= 16) {
            std::memcpy(pre_hook_bytes_out, target, copy_size);
            *pre_hook_byte_len_out = static_cast<unsigned int>(copy_size);
        } else {
            // Prologue too large for the disable record; leave len=0 so
            // splice_disable returns -1 with a clear error.
            *pre_hook_byte_len_out = 0;
            SPLICE_LOGW("install_inline_patch: copy_size=%zu > 16; disable will be unavailable",
                        copy_size);
        }
    }

    // Step 2 — allocate trampoline.
    void* trampoline = splice::os::allocate_executable_memory(kTrampolineReserve);
    if (trampoline == nullptr) {
        SPLICE_LOGE("install_inline_patch: trampoline alloc failed");
        return nullptr;
    }

    // Step 3 — copy + fix prologue into trampoline.
    auto* dst = static_cast<std::uint8_t*>(trampoline);
    const std::size_t used = emit_prologue_copy(dst,
                                                static_cast<const std::uint8_t*>(target),
                                                copy_size, target);
    if (used == static_cast<std::size_t>(-1)) {
        splice::os::free_executable_memory(trampoline, kTrampolineReserve);
        return nullptr;
    }

    // Step 4 — append 14-byte absolute jmp back to (target + copy_size).
    auto* return_addr = static_cast<std::uint8_t*>(target) + copy_size;
    const std::size_t tail = emit_jmp_abs64(dst + used, return_addr);
    const std::size_t total = used + tail;

    // Step 5 — flush trampoline cache (no-op on x86, but keep the API
    // symmetric with ARM64 for portability).
    splice::os::flush_instruction_cache(trampoline, total);

    if (original_func != nullptr) {
        *original_func = trampoline;
        SPLICE_LOGV("x86_64 trampoline at %p (used=%zu bytes)", trampoline, total);
    }

    // Pre-patch callback — fire BEFORE the atomic install so OriginalRegistry
    // (or any other dispatcher table) is up to date by the time the patched
    // prologue is observable. Without this, threads can race through the
    // C++ trampoline, read a stale OriginalRegistry, and call a null
    // `original` pointer.
    if (on_trampoline_ready != nullptr) {
        on_trampoline_ready(trampoline, user_data);
    }

    // Step 6 — make target writable, overwrite prologue, restore perms.
    if (!splice::os::make_executable_writable(target, 16)) {
        SPLICE_LOGE("install_inline_patch: can't RW target page");
        splice::os::free_executable_memory(trampoline, kTrampolineReserve);
        return nullptr;
    }

    // FR-011 / Phase 4.5b: atomic install for 5-byte E9 rel32 case.
    // Try the atomic path first (aligned 8-byte write per Intel SDM §8.1.1);
    // fall back to non-atomic memcpy only when alignment / range constraints
    // can't be met.
    bool installed_atomic = false;
    std::size_t patch_len = 5;

    if (atomic_install_jmp_rel32(target, new_func)) {
        installed_atomic = true;
        // patch_len stays 5 — the atomic helper writes 8 bytes but only
        // bytes 0..4 are the new instruction; bytes 5..7 were preserved.
    } else {
        // Atomic path declined (alignment or rel32 out of range).
        // Use the legacy memcpy install. Build the patch bytes via the
        // existing emit helpers and write non-atomically. Documented in
        // the changelog as a residual hazard for that codepath.
        std::uint8_t hook_jump[14];
        patch_len = emit_jmp_rel32(hook_jump, target, new_func);
        if (patch_len == 0) {
            patch_len = emit_jmp_abs64(hook_jump, new_func);
        }
        SPLICE_LOGW("x86_64 install: atomic path unavailable, using non-atomic "
                    "memcpy (%zu bytes). Caller threads may observe torn state.",
                    patch_len);
        std::memcpy(target, hook_jump, patch_len);
    }

    // Pad any remainder of the first overwritten instruction(s) with 0x90
    // (nop) so the in-between bytes are safe to execute if a misaligned
    // thread lands there. Bytes 5..7 of an atomic-installed E9 rel32 are
    // unreachable (jmp redirects flow before reaching them), so padding
    // there is only needed for instructions that were 6+ bytes long.
    if (copy_size > patch_len) {
        std::memset(static_cast<std::uint8_t*>(target) + patch_len, 0x90,
                    copy_size - patch_len);
    }
    splice::os::flush_instruction_cache(target, copy_size);
    splice::os::restore_executable(target, copy_size);

    if (installed_atomic) {
        SPLICE_LOGV("x86_64 install_inline_patch: ok (atomic), %p -> %p", target, new_func);
    } else {
        SPLICE_LOGV("x86_64 install_inline_patch: ok (non-atomic), %p -> %p", target, new_func);
    }
    return target;
}

} // namespace splice::arch::x86_64
