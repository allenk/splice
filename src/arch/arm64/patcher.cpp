// ─── ARM64 inline-patch installer — implementation ────────────────────────
//
// Port of `interceptor_hook_function_internal()` from the predecessor framework.h
// v3.0. Relies on the OS abstraction for memory allocation / protection /
// cache flush — no mprotect / mmap / __builtin___clear_cache calls here.
// ───────────────────────────────────────────────────────────────────────────
#include "patcher.h"

#include "atomic_patch.h"
#include "disasm.h"
#include "../../os/memory.h"

#include <splice/log.h>

#include <cstdint>
#include <cstring>

namespace splice::arch::arm64 {

namespace {

// Emit the prologue copy (with PC-relative fixup) into the trampoline buffer.
// Returns the number of bytes written, or SIZE_MAX on fatal failure.
std::size_t emit_prologue_copy(std::uint32_t* dst, const std::uint32_t* src,
                               std::size_t copy_size) {
    std::size_t trampoline_used = 0;
    const std::size_t insn_count = copy_size / 4;

    for (std::size_t i = 0; i < insn_count; ++i) {
        const InstructionInfo info = analyze_instruction(&src[i]);

        if (info.type == InstructionType::Regular) {
            dst[trampoline_used / 4] = src[i];
            trampoline_used += 4;
            continue;
        }

        const void* old_pc = &src[i];
        const void* new_pc = &dst[trampoline_used / 4];
        const std::uint32_t fixed = fix_pc_relative_instruction(src[i], info.type, old_pc, new_pc);

        if (fixed != 0) {
            dst[trampoline_used / 4] = fixed;
            trampoline_used += 4;
            continue;
        }

        // Fixup failed — emit an indirect branch instead.
        SPLICE_LOGV("Fixup overflow at idx %zu; emitting indirect branch", i);

        if (info.type == InstructionType::B || info.type == InstructionType::Bl) {
            const auto* branch_target = reinterpret_cast<const void*>(
                reinterpret_cast<std::int64_t>(old_pc) + info.offset);
            const bool link = (info.type == InstructionType::Bl);
            trampoline_used += generate_indirect_branch(&dst[trampoline_used / 4],
                                                        branch_target, link);
        } else {
            SPLICE_LOGE("Complex PC-relative fixup not implemented (type=%d)",
                        static_cast<int>(info.type));
            return static_cast<std::size_t>(-1);
        }
    }

    return trampoline_used;
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

    SPLICE_LOGV("install_inline_patch: target=%p new_func=%p", target, new_func);

    // Snapshot the 16 bytes we are about to overwrite — FR-013 Tier 2
    // disable replays these to atomically restore the prologue.
    if (pre_hook_bytes_out != nullptr && pre_hook_byte_len_out != nullptr) {
        std::memcpy(pre_hook_bytes_out, target, 16);
        *pre_hook_byte_len_out = 16;
    }

    // Step 1 — measure the prologue we need to preserve.
    const std::size_t copy_size = calculate_copy_size(target);

    // Step 2 — allocate a trampoline with headroom for fixup indirect branches
    // plus the trailing jump-back.
    const std::size_t trampoline_size = copy_size + 32;
    void* trampoline = splice::os::allocate_executable_memory(trampoline_size);
    if (trampoline == nullptr) {
        SPLICE_LOGE("install_inline_patch: trampoline allocation failed");
        return nullptr;
    }

    // Step 3 — copy + fix instructions into the trampoline.
    const auto* src = static_cast<const std::uint32_t*>(target);
    auto* dst = static_cast<std::uint32_t*>(trampoline);
    const std::size_t used = emit_prologue_copy(dst, src, copy_size);
    if (used == static_cast<std::size_t>(-1)) {
        splice::os::free_executable_memory(trampoline, trampoline_size);
        return nullptr;
    }

    // Step 4 — append jump-back to the instruction after the patched prologue.
    const auto* return_addr = reinterpret_cast<const void*>(
        reinterpret_cast<std::uintptr_t>(target) + copy_size);
    const std::size_t jump_size = generate_indirect_branch(&dst[used / 4], return_addr, false);
    const std::size_t total_used = used + jump_size;

    // Step 5 — flush trampoline cache.
    splice::os::flush_instruction_cache(trampoline, total_used);

    if (original_func != nullptr) {
        *original_func = trampoline;
        SPLICE_LOGV("Trampoline at %p size=%zu", trampoline, total_used);
    }

    // Step 5b — pre-patch callback. Invoke BEFORE the atomic install so
    // any global state (OriginalRegistry, dispatcher tables) is updated
    // first. Otherwise the C++ trampoline could fire on a thread that
    // observes the new patched prologue but reads a stale registry and
    // calls a null `original` pointer.
    if (on_trampoline_ready != nullptr) {
        on_trampoline_ready(trampoline, user_data);
    }

    // Step 6 — make the target page writable, overwrite prologue, restore perms.
    if (!splice::os::make_executable_writable(target, 16)) {
        SPLICE_LOGE("install_inline_patch: failed to make target writable");
        splice::os::free_executable_memory(trampoline, trampoline_size);
        return nullptr;
    }

    // FR-011 / Phase 4.5a: atomic install — replaces the unsafe `memcpy`
    // from Phase 1 with the architecturally-correct ARMv8 sequence
    // (literal first → DMB ISH → atomic 32-bit instr write → IC IVAU + ISB).
    // See src/arch/arm64/atomic_patch.cpp for the full rationale.
    atomic_install_indirect_branch(target, new_func);

    // The atomic sequence handles its own I-cache invalidate; this call
    // is now redundant but kept as a safety net for any non-mainstream
    // ARMv8 implementation that needs the extra flush. Cheap on real CPUs.
    splice::os::flush_instruction_cache(target, 16);

    // Restore protection to R+X (best-effort; not fatal on failure).
    splice::os::restore_executable(target, 16);

    SPLICE_LOGV("install_inline_patch: ok, %p -> %p", target, new_func);
    return target;
}

} // namespace splice::arch::arm64
