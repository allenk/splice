// ─── Pointer-slot patching — implementation ───────────────────────────────
#include "slot_patcher.h"

#include "../os/memory.h"

#include <splice/log.h>

#include <atomic>
#include <cstddef>

namespace splice::engine {

bool swap_pointer_slot(void** slot, void* new_func, void** out_previous) {
    if (slot == nullptr) {
        SPLICE_LOGE("swap_pointer_slot: null slot");
        return false;
    }

    splice::os::DataProtection saved;
    if (!splice::os::make_data_writable(slot, sizeof(void*), saved)) {
        SPLICE_LOGE("swap_pointer_slot: cannot make %p writable", static_cast<void*>(slot));
        return false;
    }

    // Read before the store, and report it before restoring the protection:
    // the caller's undo path needs this value whatever happens next.
    if (out_previous != nullptr) *out_previous = *slot;

    std::atomic_ref<void*> cell(*slot);
    cell.store(new_func, std::memory_order_release);

    splice::os::restore_data_protection(slot, sizeof(void*), saved);
    return true;
}

} // namespace splice::engine
