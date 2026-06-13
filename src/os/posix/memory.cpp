// ─── POSIX memory / cache primitives — implementation ────────────────────
#include "../memory.h"

#include <splice/log.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace splice::os {

namespace {

std::uintptr_t page_start(const void* addr) {
    const auto ps = static_cast<std::uintptr_t>(::getpagesize());
    return reinterpret_cast<std::uintptr_t>(addr) & ~(ps - 1);
}

std::size_t pages_span(const void* addr, std::size_t size) {
    const auto ps = static_cast<std::size_t>(::getpagesize());
    const auto start = page_start(addr);
    const auto end_addr = reinterpret_cast<std::uintptr_t>(addr) + size;
    const auto aligned_end = (end_addr + ps - 1) & ~(ps - 1);
    return aligned_end - start;
}

} // namespace

std::size_t page_size() {
    return static_cast<std::size_t>(::getpagesize());
}

void* allocate_executable_memory(std::size_t size) {
    const std::size_t ps = page_size();
    const std::size_t aligned = (size + ps - 1) & ~(ps - 1);

    void* mem = ::mmap(nullptr, aligned,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        SPLICE_LOGE("allocate_executable_memory: mmap failed errno=%d (%s)",
                    errno, std::strerror(errno));
        return nullptr;
    }
    SPLICE_LOGV("allocate_executable_memory: %zu bytes at %p", aligned, mem);
    return mem;
}

void free_executable_memory(void* mem, std::size_t size) {
    if (mem == nullptr) return;
    const std::size_t ps = page_size();
    const std::size_t aligned = (size + ps - 1) & ~(ps - 1);
    ::munmap(mem, aligned);
}

bool make_executable_writable(void* addr, std::size_t size) {
    const auto start = page_start(addr);
    const auto span = pages_span(addr, size);
    if (::mprotect(reinterpret_cast<void*>(start), span,
                   PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        SPLICE_LOGE("make_executable_writable(%p, %zu): mprotect failed errno=%d (%s)",
                    addr, size, errno, std::strerror(errno));
        return false;
    }
    return true;
}

void restore_executable(void* addr, std::size_t size) {
    const auto start = page_start(addr);
    const auto span = pages_span(addr, size);
    if (::mprotect(reinterpret_cast<void*>(start), span, PROT_READ | PROT_EXEC) != 0) {
        SPLICE_LOGW("restore_executable(%p, %zu): mprotect failed errno=%d",
                    addr, size, errno);
    }
}

void flush_instruction_cache(void* addr, std::size_t size) {
    auto* begin = static_cast<char*>(addr);
    __builtin___clear_cache(begin, begin + size);
}

} // namespace splice::os
