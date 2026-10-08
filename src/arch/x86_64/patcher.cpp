// ─── x86_64 inline-patch installer — implementation ──────────────────────
//
// Allocates a trampoline, relocates `target`'s prologue into it with
// PC-relative fixup, appends a jump-back to (target + prologue_size), and
// overwrites the prologue with a jump to `new_func`.
//
// Both the trampoline and a 14-byte relay are placed within rel32 range of
// the target where the address space allows, which is what lets the prologue
// patch stay 5 bytes long -- and therefore atomic. See step 2 and step 4b.
//
// Atomicity note (FR-011): the 5-byte case installs through an aligned
// 8-byte atomic store (atomic_patch.cpp). The 14-byte fallback remains a
// plain memcpy and is not atomic under concurrent execution of `target`; it
// is now reached only when no memory was free within 2 GB of the target.
// ───────────────────────────────────────────────────────────────────────────
// NOMINMAX must precede any include that might reach <windows.h> -- plog does,
// by way of splice/log.h, and its min/max macros would otherwise eat
// numeric_limits<>::min(). Same guard as atomic_patch.cpp.
#ifndef NOMINMAX
#   define NOMINMAX
#endif

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

// The relay is a single 14-byte `FF 25` absolute jump; see step 4b. It sits
// after the trampoline in the same allocation, so one near-search serves both
// and releasing the trampoline releases it too -- MEM_RELEASE and munmap both
// work from the base of the original reservation.
constexpr std::size_t kRelayOffset   = kTrampolineReserve;
constexpr std::size_t kRelayReserve  = 16;
constexpr std::size_t kNearBlockSize = kTrampolineReserve + kRelayReserve;

// Can a 5-byte `E9 rel32` placed at `from` reach `to`?
//
// atomic_install_jmp_rel32 and emit_jmp_rel32 each check this for themselves,
// so nothing depends on this being right. It is here to CHOOSE between two
// destinations before either is asked -- so that a relay which landed far is
// not preferred over a new_func that happens to be near.
bool within_rel32(const void* from, const void* to) noexcept {
    const auto src = reinterpret_cast<std::int64_t>(from);
    const auto dst = reinterpret_cast<std::int64_t>(to);
    const std::int64_t disp = dst - (src + 5);
    return disp >= std::numeric_limits<std::int32_t>::min() &&
           disp <= std::numeric_limits<std::int32_t>::max();
}

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

PreparedStrictPatch::~PreparedStrictPatch() {
    if (storage_ && !committed_) splice::os::free_executable_memory(storage_, kNearBlockSize);
}

bool PreparedStrictPatch::prepare(void* target, void* replacement,
                                   const unsigned char* expected, std::size_t expected_size) {
    if (storage_ || !target || !replacement || !expected || expected_size != 16 ||
        (reinterpret_cast<std::uintptr_t>(target) & 7u) > 3u ||
        std::memcmp(target, expected, 16) != 0) return false;
    InstructionInfo instructions[5]{};
    std::size_t source_size = 0;
    std::size_t count = 0;
    while (source_size < 5) {
        const auto info = analyze_instruction(expected + source_size, 16 - source_size);
        if (!info.length || (info.type != InstructionType::Regular &&
                            info.type != InstructionType::RipRelative) ||
            info.opcode == 0xff || info.opcode == 0xc3 || info.opcode == 0xc2 ||
            info.opcode == 0xcb || info.opcode == 0xca ||
            (info.opcode >= 0xe0 && info.opcode <= 0xe3)) return false;
        instructions[count++] = info;
        source_size += info.length;
    }
    auto* storage = static_cast<unsigned char*>(
        splice::os::allocate_executable_memory(kNearBlockSize, target));
    if (!storage) return false;
    const auto fail = [&]() {
        splice::os::free_executable_memory(storage, kNearBlockSize);
        return false;
    };
    auto* relay = storage + kRelayOffset;
    emit_jmp_abs64(relay, replacement);
    void* destination = within_rel32(target, replacement) ? replacement : relay;
    PreparedRel32Patch write{};
    if (!prepare_atomic_jmp_rel32(target, destination, write)) return fail();
    std::size_t source_offset = 0;
    std::size_t relocated_offset = 0;
    unsigned char sources[5]{}, relocated[5]{};
    for (std::size_t i = 0; i < count; ++i) {
        sources[i] = static_cast<unsigned char>(source_offset);
        relocated[i] = static_cast<unsigned char>(relocated_offset);
        const auto size = relocate_instruction(instructions[i], expected + source_offset,
            static_cast<unsigned char*>(target) + source_offset,
            storage + relocated_offset, storage + relocated_offset);
        if (!size) return fail();
        source_offset += instructions[i].length;
        relocated_offset += size;
    }
    const auto tail = emit_jmp_abs64(storage + relocated_offset,
        static_cast<unsigned char*>(target) + source_size);
    splice::os::flush_instruction_cache(storage, relocated_offset + tail);
    splice::os::flush_instruction_cache(relay, 14);
    // Publish only a completely prepared object; cancellation owns the block.
    target_ = target;
    storage_ = storage;
    write_ = write;
    std::memcpy(expected_, expected, 16);
    std::memcpy(source_offsets_, sources, 5);
    std::memcpy(relocated_offsets_, relocated, 5);
    boundary_count_ = count;
    copy_size_ = source_size;
    return true;
}

bool PreparedStrictPatch::map_ip(std::uintptr_t ip, std::uintptr_t& mapped) const noexcept {
    if (!storage_) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(target_);
    if (ip < base || ip - base >= copy_size_) {
        mapped = ip;
        return true;
    }
    for (std::size_t i = 0; i < boundary_count_; ++i) {
        if (ip - base == source_offsets_[i]) {
            mapped = reinterpret_cast<std::uintptr_t>(storage_) + relocated_offsets_[i];
            return true;
        }
    }
    return false;
}

bool PreparedStrictPatch::commit_write() noexcept {
    if (!storage_ || committed_ || std::memcmp(target_, expected_, 16) != 0) return false;
    commit_atomic_jmp_rel32(write_);
    committed_ = true;
    return true;
}

void* install_inline_patch(void* target, void* new_func, void** original_func,
                           PrePatchFn on_trampoline_ready,
                           void* user_data,
                           unsigned char* pre_hook_bytes_out,
                           unsigned int* pre_hook_byte_len_out,
                           const unsigned char* strict_expected_bytes) {
    if (target == nullptr || new_func == nullptr) {
        SPLICE_LOGE("install_inline_patch: null target or new_func");
        return nullptr;
    }
    const bool strict = strict_expected_bytes != nullptr;
    if (strict && ((reinterpret_cast<std::uintptr_t>(target) & 7u) > 3u ||
                   std::memcmp(target, strict_expected_bytes, 16) != 0)) {
        return nullptr;
    }
    SPLICE_LOGV("x86_64 install_inline_patch: target=%p new_func=%p", target, new_func);

    // Step 1 — place the trampoline and its relay, near the target.
    //
    // This comes before measuring anything because the order is load-bearing:
    // how far away this block lands decides how long the prologue patch will
    // be, and how long the patch will be decides how much prologue has to be
    // moved out of its way. Measuring first would mean measuring against a
    // worst case that is now usually avoidable.
    //
    // Proximity is not a micro-optimisation; it buys two separate things, both
    // about whether the install works at all. See the contract on
    // allocate_executable_memory in src/os/memory.h.
    void* trampoline =
        splice::os::allocate_executable_memory(kNearBlockSize, target);
    if (trampoline == nullptr) {
        SPLICE_LOGE("install_inline_patch: trampoline alloc failed");
        return nullptr;
    }

    // Step 2 — the relay, and why the install needs one.
    //
    // The prologue patch we want is a 5-byte `E9 rel32`, which reaches
    // +/-2 GB. `new_func` is compiled code in the consumer's module, and where
    // that module loaded is not ours to choose: across a 64-bit address space
    // it is routinely further away than that. Jumping straight to it then
    // needs the 14-byte `FF 25` form -- and 14 bytes cannot be written as one
    // store, so a thread already executing inside `target` (a game's render
    // thread in Present, to name the case this was built for) can observe half
    // the old instruction and half the new one.
    //
    // So we do not jump to new_func. We jump to a relay sitting beside the
    // trampoline, inside rel32 range, whose entire body is a 14-byte absolute
    // jump to new_func. Those 14 non-atomic bytes are still written -- but
    // into memory nothing is executing yet, because the relay is finished and
    // cache-flushed before the live function's prologue is touched. What lands
    // on the live function is 5 bytes inside a single aligned quadword.
    //
    // One indirection is added to every intercepted call: a jmp to a hot cache
    // line, then a jmp through an adjacent pointer. That is what Detours and
    // MinHook both pay for the same guarantee -- MinHook calls the same object
    // a "relay function".
    // Always emitted, not always used -- see the destination choice below. It
    // costs 14 bytes of an allocation we already made and removes a branch from
    // the failure path: if the direct jump turns out to be unavailable, the
    // relay is already there and already cache-flushed.
    auto* const relay = static_cast<std::uint8_t*>(trampoline) + kRelayOffset;
    const std::size_t relay_len = emit_jmp_abs64(relay, new_func);

    // Use the relay only when it buys something.
    //
    // The relay exists to shorten the patch when `new_func` is out of rel32
    // range. When it is already in range -- which is every hook whose callback
    // lives in the same module as its target, so most of them -- routing
    // through the relay adds a jump to every intercepted call and shortens
    // nothing, because the patch would have been 5 bytes either way.
    //
    // Measured, same machine, same session, bench_hook_overhead
    // BM_HookedCall_Active: 23.6 ns/call direct against 24.8 ns/call through
    // the relay. +1.2 ns, +5%, for nothing when the direct jump is available.
    //
    // So: prefer new_func, fall back to the relay, and only then to the
    // 14-byte form. The overlay case -- a hook 28.7 GB from dxgi.dll -- still
    // gets the relay and still gets an atomic install; a same-module hook now
    // pays exactly what it paid before any of this existed.
    void* jump_dest = new_func;
    if (!within_rel32(target, new_func) && within_rel32(target, relay)) {
        jump_dest = relay;
    }
    [[maybe_unused]] const bool via_relay = jump_dest != new_func;

    // How long the patch will be: 5 bytes when its destination is reachable
    // by rel32, 14 when nothing is. Everything below measures against this
    // rather than against the 14-byte worst case.
    const std::size_t patch_len_planned = within_rel32(target, jump_dest) ? 5 : 14;
    PreparedRel32Patch prepared{};
    if (strict && !prepare_atomic_jmp_rel32(target, jump_dest, prepared)) {
        splice::os::free_executable_memory(trampoline, kNearBlockSize);
        return nullptr;
    }

    // Step 3 — measure the prologue, to the length of the patch that will
    // actually be written.
    //
    // Walking further than necessary is not conservatism, it is three separate
    // costs: more instructions to relocate (each of which may turn out to be
    // unrelocatable and fail the install outright), more bytes of the caller's
    // function disturbed, and an original-bytes record that may no longer fit
    // in the 16 bytes splice_disable has to restore from.
    std::size_t copy_size = 0;
    if (strict) {
        // Bound every decode by the supplied snapshot. Reject control flow in
        // the relocated prefix rather than interpreting a thunk as provenance.
        // FF conservatively rejects the whole group (including indirect jumps).
        while (copy_size < 5) {
            const auto info = analyze_instruction(strict_expected_bytes + copy_size,
                                                   16 - copy_size);
            if (info.length == 0 || info.type == InstructionType::Unknown ||
                (info.type != InstructionType::Regular &&
                 info.type != InstructionType::RipRelative) ||
                info.opcode == 0xff || info.opcode == 0xc3 || info.opcode == 0xc2 ||
                info.opcode == 0xcb || info.opcode == 0xca ||
                (info.opcode >= 0xe0 && info.opcode <= 0xe3)) {
                splice::os::free_executable_memory(trampoline, kNearBlockSize);
                return nullptr;
            }
            copy_size += info.length;
        }
    } else {
        copy_size = calculate_copy_size(target, patch_len_planned);
    }
    if (copy_size == 0) {
        SPLICE_LOGE("install_inline_patch: prologue decode failed");
        splice::os::free_executable_memory(trampoline, kNearBlockSize);
        return nullptr;
    }
    if (copy_size < patch_len_planned) {
        // Unreachable: the walk above does not stop short of min_bytes for any
        // length this code asks for. Checked rather than assumed because being
        // wrong writes the patch past the prologue that was moved aside, into
        // an instruction still expected to execute -- in someone else's
        // process.
        SPLICE_LOGE("install_inline_patch: copy_size=%zu below planned patch "
                    "length %zu; refusing to patch",
                    copy_size, patch_len_planned);
        splice::os::free_executable_memory(trampoline, kNearBlockSize);
        return nullptr;
    }

    // Step 3b — snapshot original bytes for FR-013 Tier 2 disable. Must
    // happen before any write to `target`. Cap at 16 (record buffer size).
    if (pre_hook_bytes_out != nullptr && pre_hook_byte_len_out != nullptr) {
        if (copy_size <= 16) {
            const auto recorded_size = strict ? 5 : copy_size;
            std::memcpy(pre_hook_bytes_out, target, recorded_size);
            *pre_hook_byte_len_out = static_cast<unsigned int>(recorded_size);
        } else {
            // Prologue too large for the disable record; leave len=0 so
            // splice_disable returns -1 with a clear error. Much rarer now
            // that the walk stops at 5 rather than 16 -- a 17- or 19-byte MSVC
            // entry sequence used to land here routinely.
            *pre_hook_byte_len_out = 0;
            SPLICE_LOGW("install_inline_patch: copy_size=%zu > 16; disable will be unavailable",
                        copy_size);
        }
    }

    // Step 4 — copy + fix prologue into trampoline.
    auto* dst = static_cast<std::uint8_t*>(trampoline);
    const std::size_t used = emit_prologue_copy(dst,
                                                static_cast<const std::uint8_t*>(target),
                                                copy_size, target);
    if (used == static_cast<std::size_t>(-1)) {
        splice::os::free_executable_memory(trampoline, kNearBlockSize);
        return nullptr;
    }

    // Step 5 — append 14-byte absolute jmp back to (target + copy_size).
    auto* return_addr = static_cast<std::uint8_t*>(target) + copy_size;
    const std::size_t tail = emit_jmp_abs64(dst + used, return_addr);
    const std::size_t total = used + tail;

    // Step 6 — flush trampoline cache (no-op on x86, but keep the API
    // symmetric with ARM64 for portability). Covers the relay as well, which
    // must be complete and visible before step 6 patches the target.
    splice::os::flush_instruction_cache(trampoline, total);
    splice::os::flush_instruction_cache(relay, relay_len);

    // Complete the fallible permission change before publishing the trampoline.
    // A failed change frees it; neither output nor callback may retain it then.
    const std::size_t writable_size = strict ? 5 : 16;
    if (!splice::os::make_executable_writable(target, writable_size)) {
        SPLICE_LOGE("install_inline_patch: can't RW target page");
        splice::os::free_executable_memory(trampoline, kNearBlockSize);
        return nullptr;
    }
    if (strict && std::memcmp(target, strict_expected_bytes, 16) != 0) {
        splice::os::restore_executable(target, writable_size);
        splice::os::free_executable_memory(trampoline, kNearBlockSize);
        return nullptr;
    }

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

    // Step 7 — overwrite prologue, restore perms.

    // FR-011 / Phase 4.5b: atomic install for 5-byte E9 rel32 case.
    // Try the atomic path first (aligned 8-byte write per Intel SDM §8.1.1);
    // fall back to non-atomic memcpy only when alignment / range constraints
    // can't be met.
    bool installed_atomic = false;
    std::size_t patch_len = 5;

    if (strict) {
        // All rejection decisions preceded publication. Commit only the
        // prepared write; there is no fallible admission path after callback.
        commit_atomic_jmp_rel32(prepared);
        installed_atomic = true;
    } else if (atomic_install_jmp_rel32(target, jump_dest)) {
        installed_atomic = true;
        // patch_len stays 5 — the atomic helper writes 8 bytes but only
        // bytes 0..4 are the new instruction; bytes 5..7 were preserved.
    } else {
        // Atomic path declined (alignment or rel32 out of range).
        // Use the legacy memcpy install. Build the patch bytes via the
        // existing emit helpers and write non-atomically. Documented in
        // the changelog as a residual hazard for that codepath.
        std::uint8_t hook_jump[14];
        patch_len = emit_jmp_rel32(hook_jump, target, jump_dest);
        if (patch_len == 0) {
            patch_len = emit_jmp_abs64(hook_jump, jump_dest);
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
    if (!strict && copy_size > patch_len) {
        std::memset(static_cast<std::uint8_t*>(target) + patch_len, 0x90,
                    copy_size - patch_len);
    }
    splice::os::flush_instruction_cache(target, strict ? 5 : copy_size);
    splice::os::restore_executable(target, strict ? writable_size : copy_size);

    // Report the route, not just the outcome. "atomic" and "via relay" answer
    // different questions, and when an install turns out non-atomic the next
    // thing anyone wants to know is whether the relay was unavailable.
    SPLICE_LOGV("x86_64 install_inline_patch: ok (%s, %s), %p -> %p "
                "(patch=%zu prologue=%zu)",
                installed_atomic ? "atomic" : "non-atomic",
                via_relay ? "via relay" : "direct",
                target, new_func, patch_len, copy_size);
    return target;
}

} // namespace splice::arch::x86_64
