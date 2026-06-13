// ─── Trampoline generator tests ────────────────────────────────────────────
// Validates that TrampolineGenerator<FuncType, UniqueId> instantiates for
// common function-pointer shapes and that its trampoline delegates to the
// HookManager entry of the matching (unique_id, signature). No actual
// function patching takes place — pure template + dispatch behaviour.
// Platform-independent.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>
#include <splice/context.h>
#include <splice/registry.h>
#include <splice/trampoline.h>

namespace {

int sample_unary(int x) { return x * 3; }
double sample_binary(int a, float b) { return a + b; }
void sample_void_nullary() {}

// Step 6.4: trampoline now keys registry lookups by a runtime slot allocated
// via HookContext::slot_for(trampoline_ptr), not by the compile-time UniqueId
// template arg. Tests that directly seed registries (bypassing the
// InterceptorEntry install path) must seed against the slot, not UniqueId.
template <typename Gen>
int slot_of() {
    return splice::default_context().slot_for(Gen::get_trampoline_ptr());
}

} // namespace

TEST(TrampolineGenerator, pointer_is_non_null_for_basic_signatures) {
    using G0 = splice::TrampolineGenerator<void (*)(), 50001>;
    using G1 = splice::TrampolineGenerator<int (*)(int), 50002>;
    using G2 = splice::TrampolineGenerator<double (*)(int, float), 50003>;

    EXPECT_NE(G0::get_trampoline_ptr(), nullptr);
    EXPECT_NE(G1::get_trampoline_ptr(), nullptr);
    EXPECT_NE(G2::get_trampoline_ptr(), nullptr);
}

TEST(TrampolineGenerator, each_unique_id_gets_distinct_address) {
    // Same signature, different UniqueId → distinct static functions →
    // distinct addresses. This is the core __COUNTER__-driven guarantee.
    using A = splice::TrampolineGenerator<int (*)(int), 50010>;
    using B = splice::TrampolineGenerator<int (*)(int), 50011>;
    EXPECT_NE(A::get_trampoline_ptr(), B::get_trampoline_ptr());
}

TEST(TrampolineGenerator, trampoline_dispatches_through_hook_manager) {
    using Gen = splice::TrampolineGenerator<int (*)(int), 50020>;
    const int slot = slot_of<Gen>();

    // Seed the OriginalRegistry against the runtime slot (see slot_of doc).
    splice::OriginalRegistry::set_original<int (*)(int)>(slot, &sample_unary);

    // Install a hook callback that multiplies the original result by 100.
    auto& hook = splice::HookManager::get_hook<int, int>(slot);
    hook.set_invoke([](int (*orig)(int), int x) { return orig(x) * 100; });

    // Directly invoke the trampoline — simulates what an overwritten target
    // would do at runtime on a supported platform.
    auto* tramp = reinterpret_cast<int (*)(int)>(Gen::get_trampoline_ptr());
    EXPECT_EQ(tramp(7), 7 * 3 * 100);
}

TEST(TrampolineGenerator, trampoline_passes_through_when_no_hook_set) {
    using Gen = splice::TrampolineGenerator<double (*)(int, float), 50030>;
    const int slot = slot_of<Gen>();

    splice::OriginalRegistry::set_original<double (*)(int, float)>(slot, &sample_binary);

    auto* tramp = reinterpret_cast<double (*)(int, float)>(Gen::get_trampoline_ptr());
    EXPECT_DOUBLE_EQ(tramp(2, 0.25f), 2.25);
}

TEST(TrampolineGenerator, void_nullary_signature_compiles) {
    using Gen = splice::TrampolineGenerator<void (*)(), 50040>;
    const int slot = slot_of<Gen>();

    splice::OriginalRegistry::set_original<void (*)()>(slot, &sample_void_nullary);

    auto* tramp = reinterpret_cast<void (*)()>(Gen::get_trampoline_ptr());
    tramp();  // Should not crash.
    SUCCEED();
}
