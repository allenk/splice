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

// Reservations are granularity-aligned (64 KB on every Windows that matters),
// not page-aligned: VirtualAlloc rounds a requested base down to the
// granularity, so a near-search stepping by pages would keep retrying the
// same block.
std::size_t allocation_granularity() {
    static const std::size_t g = []() {
        SYSTEM_INFO info{};
        ::GetSystemInfo(&info);
        return static_cast<std::size_t>(info.dwAllocationGranularity);
    }();
    return g;
}

// Walk outward from `origin` in granularity steps and take the first free
// region that fits. Both directions, nearest first, so the result is as close
// as the address space allows rather than merely in range.
//
// VirtualQuery is what makes this cheap: it reports a whole region at a time,
// so occupied stretches are stepped over rather than probed one block at a
// time, and the loop below usually succeeds on its first or second candidate
// because the space just past a module image is typically free.
void* allocate_near(std::uintptr_t origin, std::size_t bytes) {
    const std::size_t gran = allocation_granularity();
    const auto gran_mask = static_cast<std::uintptr_t>(gran) - 1;
    const std::uintptr_t base = origin & ~gran_mask;
    const std::uintptr_t low = origin > kNearReach ? origin - kNearReach : gran;
    const std::uintptr_t high =
        origin + kNearReach < origin ? UINTPTR_MAX : origin + kNearReach;

    for (std::size_t step = 0; step <= kNearReach / gran; ++step) {
        const auto offset = static_cast<std::uintptr_t>(step) * gran;
        for (int direction = 0; direction < 2; ++direction) {
            // step 0 names the same address twice.
            if (step == 0 && direction == 1) continue;
            if (direction == 1 && base < offset) continue;
            const std::uintptr_t candidate =
                direction == 0 ? base + offset : base - offset;
            if (candidate < low || candidate > high) continue;

            MEMORY_BASIC_INFORMATION info{};
            if (::VirtualQuery(reinterpret_cast<void*>(candidate), &info,
                               sizeof(info)) == 0) {
                continue;
            }
            if (info.State != MEM_FREE || info.RegionSize < bytes) continue;

            void* mem = ::VirtualAlloc(reinterpret_cast<void*>(candidate), bytes,
                                       MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE);
            if (mem != nullptr) return mem;
            // Losing a race with another allocator is ordinary. Keep walking.
        }
    }
    return nullptr;
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

void* allocate_executable_memory(std::size_t size, const void* near_addr) {
    const std::size_t ps = page_size();
    const std::size_t aligned = (size + ps - 1) & ~(ps - 1);

    if (near_addr != nullptr) {
        void* near_mem =
            allocate_near(reinterpret_cast<std::uintptr_t>(near_addr), aligned);
        if (near_mem != nullptr) {
            SPLICE_LOGV("allocate_executable_memory: %zu bytes at %p "
                        "(near %p, delta=%lld)",
                        aligned, near_mem, near_addr,
                        static_cast<long long>(
                            reinterpret_cast<std::intptr_t>(near_mem) -
                            reinterpret_cast<std::intptr_t>(near_addr)));
            return near_mem;
        }
        // Worth a warning rather than a verbose line: on x86_64 this is what
        // decides whether the install can be atomic.
        SPLICE_LOGW("allocate_executable_memory: nothing free within %zu bytes "
                    "of %p; falling back to an OS-chosen address",
                    kNearReach, near_addr);
    }

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

namespace {

// Already writable? Then leave it alone. VirtualProtect would answer the
// question too -- it hands back the previous protection -- but only by having
// already changed it, and PAGE_READWRITE is not a faithful stand-in for
// PAGE_WRITECOPY or for a page that also carried execute rights.
bool writable_protection(DWORD protect) {
    switch (protect & 0xFFu) {
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

} // namespace

bool make_data_writable(void* addr, std::size_t size, DataProtection& saved) noexcept {
    if (addr == nullptr || size == 0) return false;

    MEMORY_BASIC_INFORMATION info{};
    if (::VirtualQuery(addr, &info, sizeof(info)) != sizeof(info)) {
        SPLICE_LOGE("make_data_writable(%p, %zu): VirtualQuery failed, err=%lu",
                    addr, size, ::GetLastError());
        return false;
    }
    if (info.State != MEM_COMMIT) {
        SPLICE_LOGE("make_data_writable(%p, %zu): not committed memory (state=0x%lx)",
                    addr, size, info.State);
        return false;
    }

    // One query describes one region. A range crossing into the next region
    // would need its protection restored piecewise, and a slot splice swaps
    // never does -- so refuse rather than restore the wrong half.
    const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    const auto first = reinterpret_cast<std::uintptr_t>(addr);
    if (first + size > base + info.RegionSize) {
        SPLICE_LOGE("make_data_writable(%p, %zu): range crosses a region boundary",
                    addr, size);
        return false;
    }

    if (writable_protection(info.Protect)) {
        saved = DataProtection{};          // nothing changed, nothing to put back
        return true;
    }

    const std::size_t ps = page_size();
    const auto start = reinterpret_cast<void*>(page_start(addr, ps));
    const auto span = pages_span(addr, size, ps);

    DWORD old_protect = 0;
    if (!::VirtualProtect(start, span, PAGE_READWRITE, &old_protect)) {
        SPLICE_LOGE("make_data_writable(%p, %zu): VirtualProtect RW failed, err=%lu",
                    addr, size, ::GetLastError());
        return false;
    }
    saved.previous = old_protect;
    saved.changed = true;
    return true;
}

void restore_data_protection(void* addr, std::size_t size,
                             const DataProtection& saved) noexcept {
    if (!saved.changed) return;

    const std::size_t ps = page_size();
    const auto start = reinterpret_cast<void*>(page_start(addr, ps));
    const auto span = pages_span(addr, size, ps);

    DWORD unused = 0;
    if (!::VirtualProtect(start, span, static_cast<DWORD>(saved.previous), &unused)) {
        SPLICE_LOGW("restore_data_protection(%p, %zu): VirtualProtect back to 0x%lx "
                    "failed, err=%lu",
                    addr, size, static_cast<DWORD>(saved.previous), ::GetLastError());
    }
}

bool flush_instruction_cache(void* addr, std::size_t size) {
    // x86 / x86_64 are cache-coherent for instruction streams, so this is a
    // no-op in practice. Windows' FlushInstructionCache is still the right
    // primitive to call — it will do the needful on ARM64/WoA if we ever
    // target that.
    if (::FlushInstructionCache(::GetCurrentProcess(), addr, size) != FALSE) return true;
    SPLICE_LOGW("flush_instruction_cache(%p, %zu): failed, err=%lu",
                addr, size, ::GetLastError());
    return false;
}

} // namespace splice::os
