// ─── a translation unit that disagrees about the capacity ─────────────────
//
// Compiled with `SPLICE_MAX_HOOKS=64` while the library it links against was
// built with its own value. The point of this file is that the define has no
// effect at all: nothing in splice's headers reads it, so this translation
// unit's view of `HookContext` is byte-for-byte the library's view and there is
// nothing for a mismatch to misread.
//
// That is the property that replaced three refuted attempts to *detect* such a
// mismatch -- a runtime size comparison, a configuration-tagged symbol, and a
// fingerprint baked into a macro expansion. Each failed for its own reason;
// removing the dependency needs none of them to work.
//
// This file deliberately uses the capacity-dependent surfaces: it asks the
// library for its capacity at runtime, takes a slot above the value it was
// compiled with, and round-trips through **both** registries at that slot --
// which is where a leftover header-side bound would still refuse or misread.
#include "mixed_config.h"

#include <splice/context.h>
#include <splice/registry.h>

namespace splice_mixed_config {

static int small_target(int value) { return value + 1; }

Report from_small_capacity_tu() {
    Report report;
    report.sizeof_context = sizeof(splice::HookContext);
    report.library_context_size = splice::detail::library_context_size();
    report.configured_capacity = splice::detail::configured_capacity();
    report.compiled_with = SPLICE_MAX_HOOKS;

    // A slot above this translation unit's own macro value, and inside the
    // library's. With the capacity still in the ABI this is exactly the id that
    // went wrong: this file would have computed the array bound from 64 and the
    // library from its own value.
    const int slot = report.configured_capacity > report.compiled_with
                         ? report.compiled_with + 1
                         : report.configured_capacity - 1;
    report.slot_used = slot;

    using Fn = int (*)(int);
    report.set_original = splice::OriginalRegistry::try_set_original<Fn>(slot, &small_target);
    const auto got = splice::OriginalRegistry::try_get_original<Fn>(slot);
    report.get_original = got.reason;
    report.original_round_trips = got.value == &small_target;

    const auto hook = splice::HookManager::try_get_hook<int, int>(slot);
    report.get_hook = hook.reason;
    report.hook_is_usable = hook.value != nullptr;
    if (hook.value != nullptr) {
        hook.value->set_invoke([](Fn orig, int x) { return orig ? orig(x) * 2 : -1; });
        report.hook_dispatches = hook.value->invoke(&small_target, 10) == 22;
    }
    return report;
}

} // namespace splice_mixed_config
