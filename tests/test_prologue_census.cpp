// ─── V-2: what calculate_copy_size does to real prologues ──────────────────
//
// Every other test of calculate_copy_size feeds it one hand-built byte
// sequence. That proves the walk arithmetic and nothing about the code it will
// actually meet. This walks a real, shipped, optimised library instead.
//
// The property under test is the one splice_disable depends on: with
// min_bytes = 5 -- what an install through a relay actually writes -- the result
// must never exceed the 16-byte record that atomic_disable_inline restores
// from. `> 16` is not a degradation, it removes the feature:
//
//     if (len == 0 || len > 16) { ... return false; }
//
// Before the walk was made patch-length aware it asked for 16 and routinely got
// 17 or 19 from ordinary MSVC entry sequences -- 27 warnings in one suite run,
// and 19 bytes on IDXGISwapChain::Present, which is what made disable
// unavailable on the function this whole line of work exists to hook.
//
// The census also prints the distribution, because a number nobody looks at is
// worth less than a number in the log. Prior art in this project: the ADRP
// census under splice-todo S-01 turned "common" into 18.8 %.
#include <gtest/gtest.h>

#include <arch/x86_64/disasm.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#else
#   include <dlfcn.h>
#endif

#if defined(_M_X64) || defined(__x86_64__)

using namespace splice::arch::x86_64;

namespace {

struct Sample {
    const void* address;
    std::string name;
};

#if defined(_WIN32)

// Walk a loaded module's PE export directory.
//
// Two things must be skipped or the census measures the wrong thing. A
// forwarder's RVA points back inside the export directory at a string like
// "NTDLL.RtlAllocHeap" rather than at code, and data exports are not functions
// at all. Both decode as garbage and would be counted as prologues.
std::vector<Sample> exports_of(const char* module_name) {
    std::vector<Sample> out;
    HMODULE mod = ::GetModuleHandleA(module_name);
    if (mod == nullptr) return out;

    auto* const base = reinterpret_cast<const std::uint8_t*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return out;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return out;

    const auto& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return out;

    const auto* exp =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const auto* functions =
        reinterpret_cast<const DWORD*>(base + exp->AddressOfFunctions);
    const auto* names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
    const auto* ordinals =
        reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);

    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const DWORD rva = functions[ordinals[i]];
        // Inside the export directory means a forwarder string, not code.
        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) {
            continue;
        }
        out.push_back(Sample{base + rva,
                             reinterpret_cast<const char*>(base + names[i])});
    }
    return out;
}

std::vector<Sample> collect_samples() {
    std::vector<Sample> all;
    for (const char* mod : {"kernel32.dll", "ntdll.dll"}) {
        auto part = exports_of(mod);
        all.insert(all.end(), part.begin(), part.end());
    }
    return all;
}

#else

// No portable export walk on ELF, and parsing .dynsym here would be a second
// implementation of something got_patcher.cpp already does privately. A named
// list is smaller but it is still real, optimised, shipped libc code -- which is
// the property this census is after.
std::vector<Sample> collect_samples() {
    static const char* const kNames[] = {
        "malloc", "calloc", "realloc", "free", "memcpy", "memmove", "memset",
        "memcmp", "strlen", "strcmp", "strncmp", "strcpy", "strncpy", "strchr",
        "strrchr", "strstr", "strdup", "printf", "fprintf", "snprintf",
        "sprintf", "puts", "fputs", "fopen", "fclose", "fread", "fwrite",
        "fseek", "ftell", "read", "write", "open", "close", "lseek", "mmap",
        "munmap", "mprotect", "abort", "exit", "atoi", "atol", "strtol",
        "qsort", "bsearch", "getenv", "time", "clock_gettime", "pthread_self",
        "pthread_mutex_lock", "pthread_mutex_unlock",
    };
    std::vector<Sample> out;
    for (const char* n : kNames) {
        if (void* p = ::dlsym(RTLD_DEFAULT, n); p != nullptr) {
            out.push_back(Sample{p, n});
        }
    }
    return out;
}

#endif

}  // namespace

TEST(PrologueCensus, a_five_byte_patch_always_fits_the_sixteen_byte_record) {
    const auto samples = collect_samples();
    if (samples.size() < 20) {
        GTEST_SKIP() << "only " << samples.size()
                     << " function addresses available; too small to be a census";
    }

    std::map<std::size_t, int> at_five;
    std::map<std::size_t, int> at_sixteen;
    std::vector<std::string> oversized_at_five;
    std::vector<std::string> oversized_at_sixteen;
    int undecodable = 0;

    for (const auto& s : samples) {
        const std::size_t five = calculate_copy_size(s.address, 5);
        const std::size_t sixteen = calculate_copy_size(s.address, 16);

        // 0 means the decoder gave up. Counted rather than asserted away: an
        // export can legitimately be data, and this census does not get to
        // decide what a library exports.
        if (five == 0 || sixteen == 0) {
            ++undecodable;
            continue;
        }

        ++at_five[five];
        ++at_sixteen[sixteen];
        if (five > 16 && oversized_at_five.size() < 10) {
            oversized_at_five.push_back(s.name + " = " + std::to_string(five));
        }
        if (sixteen > 16 && oversized_at_sixteen.size() < 10) {
            oversized_at_sixteen.push_back(s.name + " = " + std::to_string(sixteen));
        }
    }

    const auto total = static_cast<int>(samples.size()) - undecodable;
    ASSERT_GT(total, 0);

    auto over = [](const std::map<std::size_t, int>& d) {
        int n = 0;
        for (const auto& [size, count] : d) {
            if (size > 16) n += count;
        }
        return n;
    };
    const int over_five = over(at_five);
    const int over_sixteen = over(at_sixteen);

    std::printf("[census] %d functions decoded (%d undecodable, skipped)\n",
                total, undecodable);
    std::printf("[census] asked for 5  : %d over 16 bytes (%.2f%%)\n",
                over_five, 100.0 * over_five / total);
    std::printf("[census] asked for 16 : %d over 16 bytes (%.2f%%)  <- the old "
                "behaviour, and the reason disable was unavailable\n",
                over_sixteen, 100.0 * over_sixteen / total);

    std::printf("[census] distribution asked-for-5:");
    for (const auto& [size, count] : at_five) {
        std::printf(" %zu:%d", size, count);
    }
    std::printf("\n");

    for (const auto& n : oversized_at_sixteen) {
        std::printf("[census]   over 16 when asking 16: %s\n", n.c_str());
    }
    for (const auto& n : oversized_at_five) {
        std::printf("[census]   OVER 16 WHEN ASKING 5: %s\n", n.c_str());
    }

    // The property. A 5-byte patch needs at most 5 bytes rounded up to an
    // instruction boundary, and no x86_64 instruction is longer than 15, so
    // 4 + 15 = 19 is the arithmetic worst case -- but it takes a 15-byte
    // instruction straddling byte 5 to get there, and this says how often that
    // actually happens in shipped code.
    EXPECT_EQ(over_five, 0)
        << "a 5-byte patch produced a record too large for splice_disable";

    // Not asserted, only reported: how bad the old behaviour was on this
    // corpus. Asserting it would pin someone else's compiler output.
    EXPECT_GE(over_sixteen, 0);
}

#endif  // x86_64
