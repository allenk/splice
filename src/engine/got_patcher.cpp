// ─── ELF GOT/PLT patcher — implementation ─────────────────────────────────
//
// Ported from the predecessor framework.h v3.0. The dl_iterate_phdr walk is
// preserved verbatim — it is battle-tested across Android ARM64 scenarios.
// ───────────────────────────────────────────────────────────────────────────
#include "got_patcher.h"

#include "../os/memory.h"

#include <splice/log.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

namespace splice::engine {

namespace {

struct FindGotContext {
    const void* target_addr;
    void** found_entry;
};

// dl_iterate_phdr callback. Must have C linkage compatible with dl_iterate_phdr.
int iterate_phdr_callback(struct ::dl_phdr_info* info, std::size_t, void* data) {
    auto* ctx = static_cast<FindGotContext*>(data);

    for (int i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type != PT_DYNAMIC) continue;

        auto* dyn = reinterpret_cast<Elf64_Dyn*>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);

        // Locate DT_PLTGOT.
        Elf64_Addr* got = nullptr;
        for (Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; ++d) {
            if (d->d_tag == DT_PLTGOT) {
                got = reinterpret_cast<Elf64_Addr*>(info->dlpi_addr + d->d_un.d_ptr);
                break;
            }
        }
        if (got == nullptr) continue;

        // Heuristic sweep — scan up to 1000 entries looking for our target.
        // Bounded by invalid-pointer sentinel (very low / very high).
        const std::size_t ps = splice::os::page_size();
        for (int j = 0; j < 1000; ++j) {
            auto* entry = reinterpret_cast<void**>(&got[j]);
            const auto page_start = reinterpret_cast<std::uintptr_t>(entry) & ~(ps - 1);

            // Flip to RW so we can deref safely — some GOT pages are RO.
            if (::mprotect(reinterpret_cast<void*>(page_start), ps,
                           PROT_READ | PROT_WRITE) != 0) {
                break;
            }

            if (*entry == ctx->target_addr) {
                ctx->found_entry = entry;
                SPLICE_LOGV("Found GOT entry %p -> %p", entry, *entry);
                // Leave the page RW — the caller (patch_got_entry) will restore it.
                return 1;  // stop iteration
            }

            ::mprotect(reinterpret_cast<void*>(page_start), ps, PROT_READ);

            // Stop the sweep if we hit garbage (well outside the canonical heap/stack range).
            const auto cur = reinterpret_cast<std::uintptr_t>(*entry);
            if (cur < 0x1000u || cur > 0x7FFFFFFFFFFFu) {
                break;
            }
        }
    }
    return 0;  // continue iteration
}

} // namespace

void** find_got_entry_for_address(const void* func_addr) {
    FindGotContext ctx{func_addr, nullptr};
    ::dl_iterate_phdr(&iterate_phdr_callback, &ctx);
    return ctx.found_entry;
}

bool patch_got_entry(void** got_entry, void* new_func) {
    if (got_entry == nullptr) return false;

    const std::size_t ps = splice::os::page_size();
    const auto page_start =
        reinterpret_cast<std::uintptr_t>(got_entry) & ~(ps - 1);

    if (::mprotect(reinterpret_cast<void*>(page_start), ps,
                   PROT_READ | PROT_WRITE) != 0) {
        SPLICE_LOGE("patch_got_entry: mprotect RW failed on %p: %s",
                    got_entry, std::strerror(errno));
        return false;
    }

    *got_entry = new_func;  // atomic pointer-sized write

    if (::mprotect(reinterpret_cast<void*>(page_start), ps, PROT_READ) != 0) {
        SPLICE_LOGW("patch_got_entry: mprotect restore failed on %p: %s",
                    got_entry, std::strerror(errno));
    }
    return true;
}

} // namespace splice::engine
