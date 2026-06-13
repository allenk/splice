// ─── PE/COFF IAT patcher — implementation ─────────────────────────────────
//
// For each loaded module we walk:
//
//   IMAGE_DOS_HEADER
//     → e_lfanew →  IMAGE_NT_HEADERS64
//         → OptionalHeader.DataDirectory[IMPORT]
//             → IMAGE_IMPORT_DESCRIPTOR[]  (one per imported DLL)
//                 → FirstThunk → IMAGE_THUNK_DATA64[]  ← **this is the IAT**
//
// The FirstThunk pointer(s) are what the module actually reads when it
// calls through `call qword ptr [__imp_Foo]`. Swapping the thunk's value
// makes all subsequent inter-module calls to `Foo` resolve to our hook.
//
// Only compiled with SPLICE_HAS_WIN32_OS.
// ───────────────────────────────────────────────────────────────────────────

#ifndef NOMINMAX
#   define NOMINMAX
#endif

#include "iat_patcher.h"

#include "../os/memory.h"

#include <splice/log.h>

#include <cstdint>
#include <vector>

#include <windows.h>
#include <psapi.h>

namespace splice::engine {

namespace {

// Iterate every IMAGE_THUNK_DATA64 in `module`'s IAT, invoking `visit(slot)`.
// `visit` returns true to continue iteration, false to stop early.
//
// Returns true if iteration completed normally, false if `visit` stopped it
// (the caller has found what it wanted).
template <typename Visitor>
bool walk_module_iat(HMODULE module, Visitor&& visit) {
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return true;  // skip odd module

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return true;

    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return true;  // no imports

    const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);

    for (; desc->Name != 0; ++desc) {
        if (desc->FirstThunk == 0) continue;

        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            const_cast<std::uint8_t*>(base + desc->FirstThunk));

        for (; iat->u1.Function != 0; ++iat) {
            auto* slot = reinterpret_cast<void**>(&iat->u1.Function);
            if (!visit(slot)) {
                return false;  // caller signalled "stop"
            }
        }
    }
    return true;
}

// Snapshot all currently-loaded modules. EnumProcessModules may need two
// passes if modules are loaded/unloaded between the size query and the
// fill call; one retry is the pragmatic handling.
std::vector<HMODULE> enumerate_modules() {
    std::vector<HMODULE> modules;
    HANDLE process = ::GetCurrentProcess();

    DWORD needed = 0;
    if (!::EnumProcessModules(process, nullptr, 0, &needed) || needed == 0) {
        SPLICE_LOGE("EnumProcessModules size-query failed, err=%lu", ::GetLastError());
        return modules;
    }

    modules.resize(needed / sizeof(HMODULE));
    if (!::EnumProcessModules(process, modules.data(),
                              static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                              &needed)) {
        SPLICE_LOGE("EnumProcessModules fill failed, err=%lu", ::GetLastError());
        modules.clear();
        return modules;
    }

    modules.resize(needed / sizeof(HMODULE));
    return modules;
}

} // namespace

void** find_iat_entry_for_address(const void* func_addr) {
    void** found = nullptr;

    for (HMODULE module : enumerate_modules()) {
        const bool completed = walk_module_iat(module, [&](void** slot) -> bool {
            if (*slot == func_addr) {
                found = slot;
                SPLICE_LOGV("Found IAT entry %p -> %p in module %p",
                            slot, *slot, module);
                return false;  // stop
            }
            return true;  // continue
        });
        if (!completed) break;
    }
    return found;
}

bool patch_iat_entry(void** iat_entry, void* new_func) {
    if (iat_entry == nullptr) return false;

    DWORD old_protect = 0;
    if (!::VirtualProtect(iat_entry, sizeof(void*),
                          PAGE_READWRITE, &old_protect)) {
        SPLICE_LOGE("patch_iat_entry: VirtualProtect RW failed on %p, err=%lu",
                    iat_entry, ::GetLastError());
        return false;
    }

    *iat_entry = new_func;  // pointer-sized atomic write on x86_64

    DWORD restore_unused = 0;
    if (!::VirtualProtect(iat_entry, sizeof(void*), old_protect, &restore_unused)) {
        SPLICE_LOGW("patch_iat_entry: VirtualProtect restore failed on %p, err=%lu",
                    iat_entry, ::GetLastError());
    }
    return true;
}

} // namespace splice::engine
