// ─── Pointer-slot patching — implementation ───────────────────────────────
#include "slot_patcher.h"

#include "../os/memory.h"

#include <splice/log.h>

#include <atomic>
#include <cstddef>

namespace splice::engine {

namespace {

// Makes the slot writable, hands the slot to `write`, and puts the protection
// back whatever `write` decided. Both entry points need exactly this and
// differ only in what they do while the page is writable, so the protection
// lifetime lives in one place rather than two.
template <typename Write>
bool with_writable_slot(void** slot, const char* who, Write write) {
    if (slot == nullptr) {
        SPLICE_LOGE("%s: null slot", who);
        return false;
    }
    splice::os::DataProtection saved;
    if (!splice::os::make_data_writable(slot, sizeof(void*), saved)) {
        SPLICE_LOGE("%s: cannot make %p writable", who, static_cast<void*>(slot));
        return false;
    }
    const bool wrote = write();
    splice::os::restore_data_protection(slot, sizeof(void*), saved);
    return wrote;
}

}  // namespace

bool swap_pointer_slot(void** slot, void* new_func, void** out_previous) {
    return with_writable_slot(slot, "swap_pointer_slot", [&] {
        // Read before the store, and report it before restoring the
        // protection: the caller's undo path needs this value whatever
        // happens next.
        if (out_previous != nullptr) *out_previous = *slot;

        std::atomic_ref<void*> cell(*slot);
        cell.store(new_func, std::memory_order_release);
        return true;
    });
}

bool swap_pointer_slot_expect(void** slot, void* expected, void* new_func,
                              void** found) {
    return with_writable_slot(slot, "swap_pointer_slot_expect", [&] {
        std::atomic_ref<void*> cell(*slot);
        // `seen` carries the comparand in and, on failure, what the slot
        // actually held. The release ordering on success is the same promise
        // the unconditional store makes: a reader that observes the new
        // pointer observes everything published before this call.
        void* seen = expected;
        if (cell.compare_exchange_strong(seen, new_func,
                                         std::memory_order_release,
                                         std::memory_order_relaxed)) {
            if (found != nullptr) *found = expected;
            return true;
        }
        if (found != nullptr) *found = seen;
        SPLICE_LOGW("swap_pointer_slot_expect: %p holds %p, not the expected "
                    "%p -- somebody else owns this slot now; refusing rather "
                    "than overwriting them",
                    static_cast<void*>(slot), seen, expected);
        return false;
    });
}

} // namespace splice::engine
