// ─── POSIX memory / cache primitives — implementation ────────────────────
#include "../memory.h"

#include <splice/log.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include "proc_open.h"

#include <sys/mman.h>
#include <sys/types.h>
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

bool in_near_reach(std::uintptr_t origin, const void* mem, std::size_t size) {
    const auto begin = reinterpret_cast<std::uintptr_t>(mem);
    const std::uintptr_t end = begin + size;
    const std::uintptr_t lo = begin < origin ? begin : origin;
    const std::uintptr_t hi = end > origin ? end : origin;
    return hi - lo <= kNearReach;
}

// How far either side of the hint to search, and how finely.
//
// Two phases, because the documented reach is kNearReach -- two gigabytes --
// and walking that one page at a time would be half a million mmap calls. The
// first phase walks page by page through the two megabytes either side of the
// hint, which is where a hole next to a module image usually is and where the
// closest block is. Past that the stride opens to kNearProbeStride and the
// walk continues to the edge of the reach, testing one page per stride, which
// is enough because any hole in range will do.
//
// The page-by-page phase used to be the whole search, so this allocator
// covered 1/1024th of the range it documented, and the warning it logged on
// failure named kNearReach -- a window it had never looked at. It worked for
// as long as the caller's own image was small enough that the pages just past
// it were free, and it began failing when a sanitizer build of this project's
// test suite grew until all two megabytes either side of the anchor were
// mapped. What follows a failed search is not merely a slower path: an
// OS-chosen address lands tens of terabytes away on x86_64 Linux, which
// forfeits the atomic install and makes any rip-relative prologue
// unrelocatable.
//
// Still no /proc/self/maps gap search, which would find a hole in one read
// instead of by probing. This file parses that file for other reasons now, so
// the machinery objection has gone; what remains is that the probe succeeds on
// its first attempt in the ordinary case and needs nothing Linux-specific.
constexpr std::size_t kNearProbePages  = 512;
constexpr std::size_t kNearProbeStride = 2u * 1024u * 1024u;

} // namespace

std::size_t page_size() {
    return static_cast<std::size_t>(::getpagesize());
}

void* allocate_executable_memory(std::size_t size, const void* near_addr) {
    const std::size_t ps = page_size();
    const std::size_t aligned = (size + ps - 1) & ~(ps - 1);
    const int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    const int flags = MAP_PRIVATE | MAP_ANONYMOUS;

    if (near_addr != nullptr) {
        const auto origin = reinterpret_cast<std::uintptr_t>(near_addr);
        const std::uintptr_t page_base =
            origin & ~(static_cast<std::uintptr_t>(ps) - 1);

        // MAP_FIXED_NOREPLACE (Linux 4.17+) makes the address a requirement
        // that fails instead of relocating, which is exactly what a search
        // wants. Where it is absent the address is only a hint, so every
        // result is range-checked below. Never MAP_FIXED: that would silently
        // evict whatever already lives there.
#ifdef MAP_FIXED_NOREPLACE
        const int near_flags = flags | MAP_FIXED_NOREPLACE;
#else
        const int near_flags = flags;
#endif
        // Stop where the block itself would no longer fit inside the reach,
        // rather than where its first byte would: in_near_reach measures the
        // whole span, so a candidate past this point could never be accepted.
        const std::uintptr_t furthest =
            kNearReach > aligned ? kNearReach - aligned : 0;
        std::uintptr_t offset = aligned;
        std::size_t probes = 0;
        while (offset <= furthest) {
            for (int direction = 0; direction < 2; ++direction) {
                if (direction == 1 && page_base < offset) continue;
                const std::uintptr_t candidate = direction == 0
                                                     ? page_base + offset
                                                     : page_base - offset;

                void* mem = ::mmap(reinterpret_cast<void*>(candidate), aligned,
                                   prot, near_flags, -1, 0);
                if (mem == MAP_FAILED) continue;
                if (in_near_reach(origin, mem, aligned)) {
                    SPLICE_LOGV("allocate_executable_memory: %zu bytes at %p "
                                "(near %p, delta=%lld)",
                                aligned, mem, near_addr,
                                static_cast<long long>(
                                    reinterpret_cast<std::intptr_t>(mem) -
                                    reinterpret_cast<std::intptr_t>(near_addr)));
                    return mem;
                }
                // The kernel ignored the hint. Hand the mapping straight back
                // rather than leak one we cannot use.
                ::munmap(mem, aligned);
            }
            offset += probes < kNearProbePages
                          ? static_cast<std::uintptr_t>(ps)
                          : static_cast<std::uintptr_t>(kNearProbeStride);
            ++probes;
        }
        SPLICE_LOGW("allocate_executable_memory: no free page within %zu bytes "
                    "of %p after %zu probes; falling back to an OS-chosen "
                    "address, which forfeits the atomic install path",
                    kNearReach, near_addr, probes);
    }

    void* mem = ::mmap(nullptr, aligned, prot, flags, -1, 0);
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

namespace {

// What /proc/self/maps says about the mapping that contains `addr`.
//
// There is no mprotect that reports the protection it replaced, and no POSIX
// call that asks. Linux and Android answer through /proc/self/maps, so that is
// where the answer comes from -- parsed in place, with no allocation and no
// stdio, because the only caller that matters will eventually run inside an
// install that must not touch the heap.
//
// Returns false when the mapping cannot be found or /proc is unreadable. A
// caller that cannot learn the current protection must refuse rather than
// guess: restoring PROT_READ onto a mapping that was writable is a way to
// break a program that has nothing to do with hooking.
struct Mapping {
    std::uintptr_t begin = 0;
    std::uintptr_t end   = 0;
    int            prot  = 0;
};

bool parse_hex(const char*& p, const char* limit, std::uintptr_t& out) {
    std::uintptr_t value = 0;
    const char* start = p;
    while (p < limit) {
        const char c = *p;
        int digit;
        if (c >= '0' && c <= '9')      digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        value = (value << 4) | static_cast<std::uintptr_t>(digit);
        ++p;
    }
    if (p == start) return false;
    out = value;
    return true;
}

// One line of /proc/self/maps: "7f3c1a000-7f3c1b000 r--p 00000000 ..."
bool parse_maps_line(const char* line, const char* limit, Mapping& out) {
    const char* p = line;
    if (!parse_hex(p, limit, out.begin)) return false;
    if (p >= limit || *p != '-') return false;
    ++p;
    if (!parse_hex(p, limit, out.end)) return false;
    if (p >= limit || *p != ' ') return false;
    ++p;
    if (limit - p < 4) return false;
    out.prot = 0;
    if (p[0] == 'r') out.prot |= PROT_READ;
    if (p[1] == 'w') out.prot |= PROT_WRITE;
    if (p[2] == 'x') out.prot |= PROT_EXEC;
    return true;
}

bool find_mapping(const void* addr, Mapping& out) {
    const int fd = splice_os_open_readonly("/proc/self/maps");
    if (fd < 0) {
        SPLICE_LOGE("find_mapping(%p): cannot open /proc/self/maps, errno=%d", addr, errno);
        return false;
    }
    const auto wanted = reinterpret_cast<std::uintptr_t>(addr);

    // One line of /proc/self/maps is well under 512 bytes before the path; a
    // long path is irrelevant here and is skipped with the rest of the line.
    char buffer[4096];
    std::size_t held = 0;          // bytes of a partial line kept at buffer[0]
    bool found = false;
    for (;;) {
        const ssize_t got = ::read(fd, buffer + held, sizeof(buffer) - held);
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        const std::size_t available = held + static_cast<std::size_t>(got);
        std::size_t consumed = 0;
        for (std::size_t i = 0; i < available && !found; ++i) {
            if (buffer[i] != '\n') continue;
            Mapping m;
            if (parse_maps_line(buffer + consumed, buffer + i, m) &&
                wanted >= m.begin && wanted < m.end) {
                out = m;
                found = true;
            }
            consumed = i + 1;
        }
        if (found || got == 0) break;
        held = available - consumed;
        if (held >= sizeof(buffer)) {
            // A single line longer than the whole buffer: a path we do not
            // need. Drop it and resynchronise on the next newline.
            held = 0;
        } else if (consumed != 0) {
            std::memmove(buffer, buffer + consumed, held);
        }
    }
    ::close(fd);
    if (!found) {
        SPLICE_LOGE("find_mapping(%p): no mapping contains this address", addr);
    }
    return found;
}

} // namespace

bool is_readable(const void* addr, std::size_t size) noexcept {
    if (addr == nullptr || size == 0) return false;
    Mapping mapping;
    if (!find_mapping(addr, mapping)) return false;
    if ((mapping.prot & PROT_READ) == 0) return false;
    return reinterpret_cast<std::uintptr_t>(addr) + size <= mapping.end;
}

bool make_data_writable(void* addr, std::size_t size, DataProtection& saved) noexcept {
    if (addr == nullptr || size == 0) return false;

    Mapping mapping;
    if (!find_mapping(addr, mapping)) return false;

    const auto first = reinterpret_cast<std::uintptr_t>(addr);
    if (first + size > mapping.end) {
        SPLICE_LOGE("make_data_writable(%p, %zu): range crosses a mapping boundary",
                    addr, size);
        return false;
    }
    if ((mapping.prot & PROT_WRITE) != 0) {
        saved = DataProtection{};          // nothing changed, nothing to put back
        return true;
    }

    const auto start = page_start(addr);
    const auto span = pages_span(addr, size);
    if (::mprotect(reinterpret_cast<void*>(start), span,
                   mapping.prot | PROT_WRITE) != 0) {
        SPLICE_LOGE("make_data_writable(%p, %zu): mprotect +w failed errno=%d (%s)",
                    addr, size, errno, std::strerror(errno));
        return false;
    }
    saved.previous = static_cast<std::uintptr_t>(mapping.prot);
    saved.changed = true;
    return true;
}

void restore_data_protection(void* addr, std::size_t size,
                             const DataProtection& saved) noexcept {
    if (!saved.changed) return;

    const auto start = page_start(addr);
    const auto span = pages_span(addr, size);
    if (::mprotect(reinterpret_cast<void*>(start), span,
                   static_cast<int>(saved.previous)) != 0) {
        SPLICE_LOGW("restore_data_protection(%p, %zu): mprotect back to 0x%x failed errno=%d",
                    addr, size, static_cast<unsigned>(saved.previous), errno);
    }
}

bool flush_instruction_cache(void* addr, std::size_t size) {
    auto* begin = static_cast<char*>(addr);
    __builtin___clear_cache(begin, begin + size);
    return true;   // the builtin has no failure to report
}

} // namespace splice::os
