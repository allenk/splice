// ─── the other side of the mixed build ────────────────────────────────────
//
// Compiled with no `SPLICE_MAX_HOOKS` of its own, so it sees the header's
// fallback while its sibling was told 64 and the library was built with its
// own value. Three views of a number that no longer affects anything either of
// them can see.
#include "mixed_config.h"

#include <splice/context.h>
#include <splice/registry.h>

namespace splice_mixed_config {

static int default_target(int value) { return value + 5; }

Report from_default_capacity_tu() {
    Report report;
    report.sizeof_context = sizeof(splice::HookContext);
    report.library_context_size = splice::detail::library_context_size();
    report.configured_capacity = splice::detail::configured_capacity();
    report.compiled_with = SPLICE_MAX_HOOKS;

    // A different slot from the sibling's, so the two cannot pass by sharing
    // one and masking a disagreement about where it lives.
    const int slot = report.configured_capacity > 2 ? report.configured_capacity - 2 : 0;
    report.slot_used = slot;

    using Fn = int (*)(int);
    report.set_original = splice::OriginalRegistry::try_set_original<Fn>(slot, &default_target);
    const auto got = splice::OriginalRegistry::try_get_original<Fn>(slot);
    report.get_original = got.reason;
    report.original_round_trips = got.value == &default_target;

    const auto hook = splice::HookManager::try_get_hook<int, int>(slot);
    report.get_hook = hook.reason;
    report.hook_is_usable = hook.value != nullptr;
    if (hook.value != nullptr) {
        hook.value->set_invoke([](Fn orig, int x) { return orig ? orig(x) * 3 : -1; });
        report.hook_dispatches = hook.value->invoke(&default_target, 10) == 45;
    }
    return report;
}

} // namespace splice_mixed_config
