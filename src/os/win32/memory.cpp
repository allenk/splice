// ─── Win32 memory / cache primitives — implementation ────────────────────
//
// Mirrors src/os/posix/memory.cpp using the Windows analogues:
//   - VirtualAlloc/VirtualFree  replace mmap/munmap
//   - VirtualProtect            replaces mprotect
//   - FlushInstructionCache     replaces __builtin___clear_cache
//   - GetSystemInfo             for page granularity
//
// Header is the shared src/os/memory.h.
// ───────────────────────────────────────────────────────────────────────────

// NOMINMAX must precede any include that might pull <windows.h>.
#ifndef NOMINMAX
#   define NOMINMAX
#endif

#include "../memory.h"

#include <splice/log.h>

#include <cstdint>

#include <windows.h>

namespace splice::os {

namespace {

std::uintptr_t page_start(const void* addr, std::size_t ps) {
    return reinterpret_cast<std::uintptr_t>(addr) & ~(static_cast<std::uintptr_t>(ps) - 1);
}

std::size_t pages_span(const void* addr, std::size_t size, std::size_t ps) {
    const auto start = page_start(addr, ps);
    const auto end_addr = reinterpret_cast<std::uintptr_t>(addr) + size;
    const auto aligned_end = (end_addr + ps - 1) & ~(static_cast<std::uintptr_t>(ps) - 1);
    return aligned_end - start;
}

} // namespace

std::size_t page_size() {
    static const std::size_t ps = []() {
        SYSTEM_INFO info{};
        ::GetSystemInfo(&info);
        return static_cast<std::size_t>(info.dwPageSize);
    }();
    return ps;
}

void* allocate_executable_memory(std::size_t size) {
    const std::size_t ps = page_size();
    const std::size_t aligned = (size + ps - 1) & ~(ps - 1);

    void* mem = ::VirtualAlloc(nullptr, aligned,
                               MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
    if (mem == nullptr) {
        SPLICE_LOGE("allocate_executable_memory: VirtualAlloc failed, err=%lu",
                    ::GetLastError());
        return nullptr;
    }
    SPLICE_LOGV("allocate_executable_memory: %zu bytes at %p", aligned, mem);
    return mem;
}

void free_executable_memory(void* mem, std::size_t /*size*/) {
    if (mem == nullptr) return;
    // VirtualFree with MEM_RELEASE ignores the `size` parameter (must be 0)
    // — the OS knows the original reservation size.
    ::VirtualFree(mem, 0, MEM_RELEASE);
}

bool make_executable_writable(void* addr, std::size_t size) {
    const std::size_t ps = page_size();
    const auto start = reinterpret_cast<void*>(page_start(addr, ps));
    const auto span = pages_span(addr, size, ps);

    DWORD old_protect = 0;
    if (!::VirtualProtect(start, span, PAGE_EXECUTE_READWRITE, &old_protect)) {
        SPLICE_LOGE("make_executable_writable(%p, %zu): VirtualProtect failed, err=%lu",
                    addr, size, ::GetLastError());
        return false;
    }
    return true;
}

void restore_executable(void* addr, std::size_t size) {
    const std::size_t ps = page_size();
    const auto start = reinterpret_cast<void*>(page_start(addr, ps));
    const auto span = pages_span(addr, size, ps);

    DWORD old_protect = 0;
    if (!::VirtualProtect(start, span, PAGE_EXECUTE_READ, &old_protect)) {
        SPLICE_LOGW("restore_executable(%p, %zu): VirtualProtect failed, err=%lu",
                    addr, size, ::GetLastError());
    }
}

void flush_instruction_cache(void* addr, std::size_t size) {
    // x86 / x86_64 are cache-coherent for instruction streams, so this is a
    // no-op in practice. Windows' FlushInstructionCache is still the right
    // primitive to call — it will do the needful on ARM64/WoA if we ever
    // target that.
    ::FlushInstructionCache(::GetCurrentProcess(), addr, size);
}

} // namespace splice::os
