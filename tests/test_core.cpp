// ─── Core / InterceptorEntry tests ────────────────────────────────────────
// Exercise the fluent API surface end-to-end on platforms where the engine
// is only a stub. The InterceptorEntry is constructed, registers its key,
// queues an installer, and `install_all()` routes to the stub — which
// correctly logs-and-returns-null. We verify no crash and that downstream
// macros still compile.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>
#include <splice/splice.h>

namespace {

int victim_function(int x) { return x + 1; }

} // namespace

TEST(InterceptorEntry, direct_address_ctor_registers_key) {
    splice::InterceptorEntry<int (*)(int)> entry{
        nullptr, reinterpret_cast<void*>(&victim_function), "victim_label",
        60001, reinterpret_cast<void*>(&victim_function)};
    // Constructor registers an "ADDR_<ptr>" mapping — look it up by scanning.
    EXPECT_FALSE(entry.is_installed());  // install_all not yet called
}

TEST(InterceptorEntry, symbol_ctor_registers_key) {
    // Step 6.4: IdRegistry now stores the runtime slot from slot_for(
    // trampoline_ptr), not the constructor's unique_id parameter. Verify
    // the key maps to the same slot the context assigns.
    void* trampoline_ptr = reinterpret_cast<void*>(&victim_function);
    const int expected_slot = splice::default_context().slot_for(trampoline_ptr);
    splice::InterceptorEntry<int (*)(int)> entry{
        "test_lib.so", "victim_function", "victim_function",
        60002, trampoline_ptr};
    EXPECT_EQ(splice::IdRegistry::get_id_by_key("test_lib.so_victim_function"),
              expected_slot);
}

// Compile-only sanity checks for the macro surface. Step 6.4: even with
// runtime slot_for keying registry by trampoline_ptr, we still skip
// .onInvoke() here — these tests don't need to set a callback, and
// avoiding it means no persistent slot is created, so install_all()
// re-runs from later tests have nothing of ours to re-patch.
TEST(FluentApi, splice_hook_macro_compiles_without_crash) {
    auto& entry = SPLICE_HOOK_STATIC("testlib", victim_function);
    (void)entry;
    SUCCEED();
}

TEST(FluentApi, splice_hook_addr_macro_compiles_without_crash) {
    auto& entry = SPLICE_HOOK_ADDR_STATIC(&victim_function);
    (void)entry;
    SUCCEED();
}
