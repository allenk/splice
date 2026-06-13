// ─── Splice engine — cross-platform strategy dispatch ────────────────────
//
// Implements the C ABI declared in <splice/engine.h>. Compiled once per
// platform — this file serves both POSIX and Win32.
//
// Strategy, in order of preference:
//   1. If `lib_name` is provided — resolve the symbol via the platform's
//      loader (dlsym on POSIX, GetProcAddress on Win32), then fall through
//      to address-based hooking using the resolved pointer.
//   2. For address-based hooking on POSIX — probe loaded ELF objects for a
//      GOT/PLT entry pointing at `target`; if found, atomic-write the new
//      function. On Win32, skip directly to instruction patching until
//      Phase 4b adds IAT patching.
//   3. Otherwise — relocate the prologue into a trampoline and overwrite
//      the target with an indirect branch.
// ───────────────────────────────────────────────────────────────────────────

// NOMINMAX must precede any include that might reach <windows.h>.
#ifndef NOMINMAX
#   define NOMINMAX
#endif

#include <splice/engine.h>

#if defined(SPLICE_HAS_POSIX_HOOK)
#   include "got_patcher.h"
#endif
#if defined(SPLICE_HAS_WIN32_OS)
#   include "iat_patcher.h"
#endif
#if defined(SPLICE_HAS_ARM64_BACKEND)
#   include "../arch/arm64/disasm.h"
#   include "../arch/arm64/patcher.h"
#   include "../arch/arm64/atomic_patch.h"
#endif
#if defined(SPLICE_HAS_X86_64_BACKEND)
#   include "../arch/x86_64/disasm.h"
#   include "../arch/x86_64/patcher.h"
#   include "../arch/x86_64/atomic_patch.h"
#endif

#if defined(SPLICE_HAS_ARM64_BACKEND) || defined(SPLICE_HAS_X86_64_BACKEND)
#   include "../os/memory.h"
#endif

#include <splice/log.h>

#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#   include <windows.h>
#else
#   include <dlfcn.h>
#endif

namespace {

// Resolve `lib_name::symbol_name` to a raw function pointer using the
// platform's dynamic loader. Returns nullptr on failure.
void* resolve_symbol(const char* lib_name, const char* symbol_name) {
#if defined(_WIN32)
    // Try an already-loaded module first — avoids the side effects of
    // LoadLibrary (which runs DllMain). Windows equivalent of the POSIX
    // RTLD_NOLOAD probe.
    HMODULE module = ::GetModuleHandleA(lib_name);
    if (module == nullptr) {
        module = ::LoadLibraryA(lib_name);
        if (module == nullptr) {
            SPLICE_LOGE("LoadLibrary(%s) failed, err=%lu", lib_name, ::GetLastError());
            return nullptr;
        }
    }
    auto addr = reinterpret_cast<void*>(::GetProcAddress(module, symbol_name));
    if (addr == nullptr) {
        SPLICE_LOGE("GetProcAddress(%s, %s) failed, err=%lu",
                    lib_name, symbol_name, ::GetLastError());
    }
    return addr;
#else
    void* handle = ::dlopen(lib_name, RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr) {
        SPLICE_LOGE("dlopen(%s) failed: %s", lib_name, ::dlerror());
        return nullptr;
    }
    void* target = ::dlsym(handle, symbol_name);
    if (target == nullptr) {
        SPLICE_LOGE("dlsym(%s) failed: %s", symbol_name, ::dlerror());
    }
    ::dlclose(handle);  // bionic dlclose is a no-op; glibc honours it
    return target;
#endif
}

// Pre-patch callback: fires after the trampoline (or pointer-swap target)
// is identified, but before the new value is committed. Lets the caller
// register the trampoline / original-pointer with downstream lookup
// tables (e.g. OriginalRegistry) so threads observing the new patch
// can immediately resolve a valid `original`.
using PrePatchFn = void (*)(void* trampoline, void* user_data);

// Invoke the architectural inline-patcher for the current target.
// The `pre_hook_bytes` / `pre_hook_byte_len` out-params (both nullable)
// receive a snapshot of the original prologue bytes for FR-013 Tier 2
// disable replay; they are populated only when the install actually
// happens (i.e. when we return non-null).
void* install_inline_patch(void* target, void* new_func, void** original_func,
                           PrePatchFn pre_cb, void* pre_user,
                           unsigned char* pre_hook_bytes,
                           unsigned int* pre_hook_byte_len) {
#if defined(SPLICE_HAS_ARM64_BACKEND)
    return splice::arch::arm64::install_inline_patch(target, new_func, original_func,
                                                     pre_cb, pre_user,
                                                     pre_hook_bytes, pre_hook_byte_len);
#elif defined(SPLICE_HAS_X86_64_BACKEND)
    return splice::arch::x86_64::install_inline_patch(target, new_func, original_func,
                                                      pre_cb, pre_user,
                                                      pre_hook_bytes, pre_hook_byte_len);
#else
    (void)target; (void)new_func; (void)original_func; (void)pre_cb; (void)pre_user;
    (void)pre_hook_bytes; (void)pre_hook_byte_len;
    SPLICE_LOGE("install_inline_patch: no arch backend compiled");
    return nullptr;
#endif
}

// Unwrap jump stubs so we hook the real function body, not an ILT entry.
// Common chains on MSVC debug Windows x86_64:
//   &func  --E9-->  ILT stub  --FF25-->  import thunk dereference  --> real
// Each pass follows exactly one level; we loop with a depth cap so the
// common two-level chain reaches the real function, but a malformed
// circular chain can't trap us forever. Idempotent on targets that
// aren't stubs.
void* unwrap_jump_stub(void* target) {
#if defined(SPLICE_HAS_X86_64_BACKEND)
    constexpr int kMaxDepth = 4;
    for (int depth = 0; depth < kMaxDepth && target != nullptr; ++depth) {
        const auto* bytes = static_cast<const std::uint8_t*>(target);

        // E9 rel32 — relative near jump (MSVC ILT stub)
        if (bytes[0] == 0xE9) {
            std::int32_t rel32 = 0;
            std::memcpy(&rel32, bytes + 1, 4);
            void* dest = static_cast<std::uint8_t*>(target) + 5 + rel32;
            SPLICE_LOGV("unwrap_jump_stub[%d]: %p  --E9-->  %p", depth, target, dest);
            target = dest;
            continue;
        }

        // FF 25 disp32 — indirect near jmp via [rip + disp32] (import thunk)
        if (bytes[0] == 0xFF && bytes[1] == 0x25) {
            std::int32_t disp32 = 0;
            std::memcpy(&disp32, bytes + 2, 4);
            auto* slot = reinterpret_cast<void**>(
                static_cast<std::uint8_t*>(target) + 6 + disp32);
            void* dest = *slot;
            SPLICE_LOGV("unwrap_jump_stub[%d]: %p  --FF25-->  %p", depth, target, dest);
            target = dest;
            continue;
        }

        break;  // not a jump stub — we've reached the real body
    }
#endif
    return target;
}

void* hook_by_address(void* target_addr, void* new_func, void** original_func,
                      PrePatchFn pre_cb = nullptr, void* pre_user = nullptr,
                      splice_patch_record* out_record = nullptr) {
    if (target_addr == nullptr) {
        SPLICE_LOGE("splice_hook_address: null target");
        return nullptr;
    }

    // Follow one level of jump-stub indirection (MSVC ILT, DLL imports).
    target_addr = unwrap_jump_stub(target_addr);

#if defined(SPLICE_HAS_POSIX_HOOK)
    // Strategy 1 (POSIX): ELF GOT/PLT reverse — Tier 1 safe.
    if (void** got_entry = splice::engine::find_got_entry_for_address(target_addr); got_entry) {
        SPLICE_LOGV("hook_by_address: GOT entry %p -> %p", got_entry, *got_entry);
        void* original_value = *got_entry;
        if (original_func != nullptr) *original_func = original_value;
        // Pre-patch callback: register the original BEFORE the atomic swap.
        if (pre_cb != nullptr) pre_cb(original_value, pre_user);
        if (splice::engine::patch_got_entry(got_entry, new_func)) {
            SPLICE_LOGI("Hooked via GOT at %p", got_entry);
            if (out_record != nullptr) {
                out_record->strategy = SPLICE_PATCH_STRATEGY_POINTER_SWAP;
                out_record->hook_site = got_entry;
                out_record->pre_hook_pointer = original_value;
                out_record->trampoline = nullptr;
                out_record->pre_hook_byte_len = 0;
            }
            return got_entry;
        }
        SPLICE_LOGW("GOT patch failed; falling back to direct instruction patching");
    }
#elif defined(SPLICE_HAS_WIN32_OS)
    // Strategy 1 (Win32): PE/COFF IAT reverse — Tier 1 safe.
    if (void** iat_entry = splice::engine::find_iat_entry_for_address(target_addr); iat_entry) {
        SPLICE_LOGV("hook_by_address: IAT entry %p -> %p", iat_entry, *iat_entry);
        void* original_value = *iat_entry;
        if (original_func != nullptr) *original_func = original_value;
        // Pre-patch callback: register the original BEFORE the atomic swap.
        if (pre_cb != nullptr) pre_cb(original_value, pre_user);
        if (splice::engine::patch_iat_entry(iat_entry, new_func)) {
            SPLICE_LOGI("Hooked via IAT at %p", iat_entry);
            if (out_record != nullptr) {
                out_record->strategy = SPLICE_PATCH_STRATEGY_POINTER_SWAP;
                out_record->hook_site = iat_entry;
                out_record->pre_hook_pointer = original_value;
                out_record->trampoline = nullptr;
                out_record->pre_hook_byte_len = 0;
            }
            return iat_entry;
        }
        SPLICE_LOGW("IAT patch failed; falling back to direct instruction patching");
    }
#endif

    // Strategy 2: direct instruction patching (Tier 2 — trampoline leak).
    // The arch patcher fires the pre-patch callback internally, after the
    // trampoline is built but before the atomic install lands. It also
    // snapshots the original prologue bytes for FR-013 Tier 2 disable
    // (Phase 4.5c-2) directly into the record's pre_hook_bytes buffer.
    unsigned char* bytes_out = (out_record != nullptr) ? out_record->pre_hook_bytes : nullptr;
    unsigned int*  len_out   = (out_record != nullptr) ? &out_record->pre_hook_byte_len : nullptr;
    void* result = install_inline_patch(target_addr, new_func, original_func,
                                        pre_cb, pre_user,
                                        bytes_out, len_out);
    if (result != nullptr && out_record != nullptr) {
        out_record->strategy = SPLICE_PATCH_STRATEGY_INLINE;
        out_record->hook_site = target_addr;
        out_record->pre_hook_pointer = nullptr;
        out_record->trampoline = (original_func != nullptr) ? *original_func : nullptr;
        // pre_hook_bytes / pre_hook_byte_len already filled by the arch
        // installer above. If install actually failed (result == nullptr)
        // we leave the record alone — caller's contract is "consumed only
        // if install succeeded".
    }
    return result;
}

} // namespace

extern "C" {

void* splice_hook_symbol(const char* lib_name,
                         const char* symbol_name,
                         void* new_func,
                         void** original_func) {
    return splice_hook_symbol_pre(lib_name, symbol_name, new_func, original_func,
                                  nullptr, nullptr);
}

void* splice_hook_symbol_pre(const char* lib_name,
                             const char* symbol_name,
                             void* new_func,
                             void** original_func,
                             splice_pre_patch_fn pre_cb,
                             void* pre_user) {
    return splice_hook_symbol_pre_rec(lib_name, symbol_name, new_func, original_func,
                                      pre_cb, pre_user, nullptr);
}

void* splice_hook_symbol_pre_rec(const char* lib_name,
                                 const char* symbol_name,
                                 void* new_func,
                                 void** original_func,
                                 splice_pre_patch_fn pre_cb,
                                 void* pre_user,
                                 splice_patch_record* out_record) {
    if (new_func == nullptr) {
        SPLICE_LOGE("splice_hook_symbol_pre_rec: null new_func");
        return nullptr;
    }

    if (lib_name != nullptr && lib_name[0] != '\0') {
        if (symbol_name == nullptr) {
            SPLICE_LOGE("splice_hook_symbol_pre_rec: symbol_name required when lib_name is set");
            return nullptr;
        }
        SPLICE_LOGV("splice_hook_symbol_pre_rec: %s::%s", lib_name, symbol_name);
        void* target = resolve_symbol(lib_name, symbol_name);
        if (target == nullptr) return nullptr;
        SPLICE_LOGV("splice_hook_symbol_pre_rec: resolved %s -> %p", symbol_name, target);
        return hook_by_address(target, new_func, original_func, pre_cb, pre_user, out_record);
    }

    // Legacy path: lib_name unset → `symbol_name` is actually a raw address.
    void* target = reinterpret_cast<void*>(const_cast<char*>(symbol_name));
    return hook_by_address(target, new_func, original_func, pre_cb, pre_user, out_record);
}

int splice_disable(const splice_patch_record* record) {
    if (record == nullptr) {
        SPLICE_LOGE("splice_disable: null record");
        return -1;
    }

    switch (record->strategy) {
        case SPLICE_PATCH_STRATEGY_POINTER_SWAP: {
            // Tier 1 — atomic pointer-sized write back to original. Always safe.
            // Works identically for ELF GOT/PLT and PE IAT (it's just a slot).
            auto* slot = static_cast<void**>(record->hook_site);
            if (slot == nullptr || record->pre_hook_pointer == nullptr) {
                SPLICE_LOGE("splice_disable: invalid POINTER_SWAP record (slot=%p, original=%p)",
                            slot, record->pre_hook_pointer);
                return -1;
            }
            SPLICE_LOGV("splice_disable: POINTER_SWAP slot=%p restoring %p (was %p)",
                        slot, record->pre_hook_pointer, *slot);

#if defined(SPLICE_HAS_POSIX_HOOK)
            const bool ok = splice::engine::patch_got_entry(slot, record->pre_hook_pointer);
#elif defined(SPLICE_HAS_WIN32_OS)
            const bool ok = splice::engine::patch_iat_entry(slot, record->pre_hook_pointer);
#else
            const bool ok = false;
#endif
            if (!ok) {
                SPLICE_LOGE("splice_disable: arch-level slot patch failed");
                return -1;
            }
            SPLICE_LOGI("Disabled (POINTER_SWAP) at %p", slot);
            return 0;
        }

        case SPLICE_PATCH_STRATEGY_INLINE: {
            // Tier 2 — inline disable (FR-013 Phase 4.5c-2). Atomically
            // restore the original first instruction word (with cache
            // maintenance on ARM64) and reverse the rest non-atomically
            // while it's still unreachable. Trampoline memory remains
            // allocated forever — same trade-off ShadowHook documents,
            // because we cannot prove no thread is currently executing
            // inside it.
            void* target = record->hook_site;
            const unsigned int len = record->pre_hook_byte_len;
            if (target == nullptr || len == 0) {
                SPLICE_LOGE("splice_disable: invalid INLINE record (target=%p len=%u)",
                            target, len);
                return -1;
            }

#if defined(SPLICE_HAS_ARM64_BACKEND) || defined(SPLICE_HAS_X86_64_BACKEND)
            // Make the patched page writable for the duration of the
            // restore. Same prologue size as install (16 bytes covers
            // both ARM64's fixed 16-byte branch and x86_64's worst-case
            // copy_size we accept for atomic install).
            if (!splice::os::make_executable_writable(target, 16)) {
                SPLICE_LOGE("splice_disable: cannot make target writable: %p", target);
                return -1;
            }

#   if defined(SPLICE_HAS_ARM64_BACKEND)
            splice::arch::arm64::atomic_disable_indirect_branch(target,
                                                                record->pre_hook_bytes);
#   elif defined(SPLICE_HAS_X86_64_BACKEND)
            if (!splice::arch::x86_64::atomic_disable_inline(target,
                                                             record->pre_hook_bytes,
                                                             len)) {
                splice::os::restore_executable(target, 16);
                SPLICE_LOGE("splice_disable: x86_64 atomic_disable_inline rejected request");
                return -1;
            }
#   endif

            splice::os::flush_instruction_cache(target, 16);
            splice::os::restore_executable(target, 16);

            SPLICE_LOGI("Disabled (INLINE) at %p — trampoline %p leaked per FR-013",
                        target, record->trampoline);
            return 0;
#else
            SPLICE_LOGW("splice_disable: INLINE (Tier 2) — no arch backend compiled");
            return -1;
#endif
        }

        default:
            SPLICE_LOGE("splice_disable: unknown strategy %d", record->strategy);
            return -1;
    }
}

void* splice_hook_address(void* target_addr, void* new_func, void** original_func) {
    if (new_func == nullptr) {
        SPLICE_LOGE("splice_hook_address: null new_func");
        return nullptr;
    }
    return hook_by_address(target_addr, new_func, original_func, nullptr, nullptr);
}

void* splice_hook_address_pre(void* target_addr, void* new_func, void** original_func,
                              splice_pre_patch_fn pre_cb, void* pre_user) {
    return splice_hook_address_pre_rec(target_addr, new_func, original_func,
                                       pre_cb, pre_user, nullptr);
}

void* splice_hook_address_pre_rec(void* target_addr, void* new_func, void** original_func,
                                  splice_pre_patch_fn pre_cb, void* pre_user,
                                  splice_patch_record* out_record) {
    if (new_func == nullptr) {
        SPLICE_LOGE("splice_hook_address_pre_rec: null new_func");
        return nullptr;
    }
    return hook_by_address(target_addr, new_func, original_func, pre_cb, pre_user, out_record);
}

int splice_is_hooked(const void* addr) {
    if (addr == nullptr) return 0;
#if defined(SPLICE_HAS_ARM64_BACKEND)
    const auto* insn = static_cast<const std::uint32_t*>(addr);
    return splice::arch::arm64::looks_hooked(insn) ? 1 : 0;
#elif defined(SPLICE_HAS_X86_64_BACKEND)
    const auto* bytes = static_cast<const std::uint8_t*>(addr);
    return splice::arch::x86_64::looks_hooked(bytes) ? 1 : 0;
#else
    return 0;
#endif
}

void splice_debug_function_start(const void* func_addr, const char* label) {
    if (func_addr == nullptr) return;
#if defined(SPLICE_HAS_ARM64_BACKEND)
    const auto* insn = static_cast<const std::uint32_t*>(func_addr);
    SPLICE_LOGI("=== %s @ %p ===", label ? label : "(func)", func_addr);
    for (int i = 0; i < 8; ++i) {
        const auto info = splice::arch::arm64::analyze_instruction(&insn[i]);
        SPLICE_LOGI("  [%d] %08x  type=%d", i, insn[i], static_cast<int>(info.type));
    }
#elif defined(SPLICE_HAS_X86_64_BACKEND)
    const auto* bytes = static_cast<const std::uint8_t*>(func_addr);
    SPLICE_LOGI("=== %s @ %p ===", label ? label : "(func)", func_addr);
    std::size_t off = 0;
    for (int i = 0; i < 8; ++i) {
        const auto info = splice::arch::x86_64::analyze_instruction(bytes + off, 15);
        if (info.length == 0) break;
        SPLICE_LOGI("  [%d] len=%u op=0x%02x type=%d", i,
                    info.length, info.opcode, static_cast<int>(info.type));
        off += info.length;
    }
#else
    (void)label;
    SPLICE_LOGW("splice_debug_function_start: no disassembler on this arch");
#endif
}

} // extern "C"
