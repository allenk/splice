// ─── Splice HookContext — out-of-line singleton ───────────────────────────
//
// Parks the default-context Meyers singleton in a compiled TU so there is
// exactly one instance across all translation units that link against
// libsplice. A purely inline definition in context.h would give each TU
// its own copy on Windows when splice is linked statically into multiple
// DLLs — the hazard the v1.4 review called out.
// ───────────────────────────────────────────────────────────────────────────
#include <splice/context.h>

#include <atomic>
#include <new>

namespace splice {

HookContext& default_context() noexcept {
    static HookContext ctx;
    return ctx;
}

namespace detail {

// The size this translation unit -- the one that owns the shared instance --
// compiled HookContext to. Headers compare their own sizeof against it, which
// catches a consumer built with a different SPLICE_REGISTRY_IMPL before it
// reads the instance at the wrong offsets. SPLICE_MAX_HOOKS no longer affects
// this size: the slot arrays live behind pointers allocated here, so a build
// that disagrees about the capacity has nothing to read at the wrong offset.
std::size_t library_context_size() noexcept { return sizeof(HookContext); }

// ─── Slot storage ──────────────────────────────────────────────────────────
// The capacity exists here and in no header, and that is the whole mechanism:
// a consumer compiled with a different SPLICE_MAX_HOOKS cannot disagree with
// the library about a layout, because no layout depends on it. Detecting such a
// disagreement was the previous answer and it could not be made to work.
//
// `new (std::nothrow)` rather than `new`, and no exceptions anywhere in these
// four functions: the first caller is default_context()'s construction, which
// is noexcept, in a library that is a guest in someone else's process. A
// failure here is reported as a refused install by HookContext, not as a
// terminate.

int configured_capacity() noexcept { return static_cast<int>(SPLICE_MAX_HOOKS); }

std::atomic<void*>* allocate_original_slots(int count) noexcept {
    if (count <= 0) return nullptr;
    auto* slots = new (std::nothrow) std::atomic<void*>[static_cast<std::size_t>(count)];
    if (slots == nullptr) return nullptr;
    // std::atomic's default constructor leaves the value uninitialised before
    // C++20, and this header is C++17-compatible for AOSP consumers.
    for (int i = 0; i < count; ++i) slots[i].store(nullptr, std::memory_order_relaxed);
    return slots;
}

std::atomic<HookBase*>* allocate_hook_slots(int count) noexcept {
    if (count <= 0) return nullptr;
    auto* slots = new (std::nothrow) std::atomic<HookBase*>[static_cast<std::size_t>(count)];
    if (slots == nullptr) return nullptr;
    for (int i = 0; i < count; ++i) slots[i].store(nullptr, std::memory_order_relaxed);
    return slots;
}

void free_original_slots(std::atomic<void*>* slots) noexcept { delete[] slots; }

void free_hook_slots(std::atomic<HookBase*>* slots) noexcept {
    // The slots, not what they point at. Hook objects are program-lifetime by
    // design -- nothing here can establish that no thread holds a pointer to
    // one, which is the same missing protocol that keeps the trampolines
    // allocated forever.
    delete[] slots;
}

} // namespace detail

} // namespace splice
