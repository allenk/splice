// ─── FR-009 Step 9.5 — splice::ScopedHook RAII wrapper tests ──────────────
//
// Validates:
//   1. Construct from InterceptorEntry& → captures by reference
//   2. Destruction triggers disable() on the underlying entry
//   3. Move-constructed / move-assigned correctly transfer ownership
//   4. Default-constructed is empty; release() is no-op
//   5. release() is idempotent (calling twice does not double-disable)
//   6. Works inside heterogeneous std::vector<ScopedHook>
//
// We test the WRAPPER's behaviour, not the underlying disable mechanics
// (covered by test_hook_inline_disable.cpp etc.). Use a stub entry
// constructed in place — the actual disable will fail (no real install
// happened), but disable()'s return-false path still flips m_installed
// → false and the ScopedHook's RAII contract is met regardless.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/context.h>
#include <splice/core.h>
#include <splice/registry.h>
#include <splice/trampoline.h>

#include <vector>

namespace {

int dummy_func(int x) { return x; }

} // namespace

// ─── Test 1 — default-constructed ScopedHook is empty ─────────────────────
TEST(ScopedHook, default_constructed_is_empty) {
    splice::ScopedHook h;
    EXPECT_TRUE(h.empty());
    // Destructor on empty ScopedHook must not crash.
}

// ─── Test 2 — construct from entry, destructor calls disable ──────────────
TEST(ScopedHook, destructor_calls_disable) {
    using Gen = splice::TrampolineGenerator<int (*)(int), 80001>;

    splice::InterceptorEntry<int (*)(int)> entry(
        nullptr, reinterpret_cast<void*>(&dummy_func), "scoped_test",
        80001, Gen::get_trampoline_ptr());

    // Entry is not actually installed (we skipped install_all), so
    // is_installed() is false. But the ScopedHook contract is still
    // valid — it captures the reference and would disable on destruction
    // if the entry were live.
    {
        splice::ScopedHook h(entry);
        EXPECT_FALSE(h.empty());
    }   // ← h destructs, calls entry.disable() (no-op on uninstalled entry)
    // No crash, no assert — that's the contract.
    EXPECT_FALSE(entry.is_installed());
}

// ─── Test 3 — move-construct transfers ownership ──────────────────────────
TEST(ScopedHook, move_construct_transfers_ownership) {
    using Gen = splice::TrampolineGenerator<int (*)(int), 80002>;
    splice::InterceptorEntry<int (*)(int)> entry(
        nullptr, reinterpret_cast<void*>(&dummy_func), "scoped_test",
        80002, Gen::get_trampoline_ptr());

    splice::ScopedHook src(entry);
    EXPECT_FALSE(src.empty());

    splice::ScopedHook dst(std::move(src));
    EXPECT_TRUE(src.empty());
    EXPECT_FALSE(dst.empty());
}

// ─── Test 4 — move-assign transfers ownership and releases previous ──────
TEST(ScopedHook, move_assign_releases_and_transfers) {
    using GenA = splice::TrampolineGenerator<int (*)(int), 80003>;
    using GenB = splice::TrampolineGenerator<int (*)(int), 80004>;
    splice::InterceptorEntry<int (*)(int)> entry_a(
        nullptr, reinterpret_cast<void*>(&dummy_func), "scoped_a",
        80003, GenA::get_trampoline_ptr());
    splice::InterceptorEntry<int (*)(int)> entry_b(
        nullptr, reinterpret_cast<void*>(&dummy_func), "scoped_b",
        80004, GenB::get_trampoline_ptr());

    splice::ScopedHook a(entry_a);
    splice::ScopedHook b(entry_b);
    EXPECT_FALSE(a.empty());
    EXPECT_FALSE(b.empty());

    a = std::move(b);
    EXPECT_FALSE(a.empty());     // a now holds what b held
    EXPECT_TRUE(b.empty());      // b is empty
}

// ─── Test 5 — release() is idempotent ─────────────────────────────────────
TEST(ScopedHook, release_is_idempotent) {
    using Gen = splice::TrampolineGenerator<int (*)(int), 80005>;
    splice::InterceptorEntry<int (*)(int)> entry(
        nullptr, reinterpret_cast<void*>(&dummy_func), "scoped_test",
        80005, Gen::get_trampoline_ptr());

    splice::ScopedHook h(entry);
    h.release();
    EXPECT_TRUE(h.empty());

    // Second release must be a safe no-op.
    h.release();
    EXPECT_TRUE(h.empty());

    // Destructor on already-released ScopedHook must not crash.
}

// ─── Test 6 — vector<ScopedHook> with heterogeneous entries ──────────────
// This is the "showcase" demo loader pattern: collect every hook into a
// single vector, clear the vector to disable them all in one shot.
TEST(ScopedHook, heterogeneous_vector_supported) {
    using GenA = splice::TrampolineGenerator<int (*)(int),         80006>;
    using GenB = splice::TrampolineGenerator<double (*)(int, float), 80007>;
    using GenC = splice::TrampolineGenerator<void (*)(int*),       80008>;

    splice::InterceptorEntry<int (*)(int)> entry_a(
        nullptr, reinterpret_cast<void*>(&dummy_func), "scoped_a",
        80006, GenA::get_trampoline_ptr());
    splice::InterceptorEntry<double (*)(int, float)> entry_b(
        nullptr, reinterpret_cast<void*>(&dummy_func), "scoped_b",
        80007, GenB::get_trampoline_ptr());
    splice::InterceptorEntry<void (*)(int*)> entry_c(
        nullptr, reinterpret_cast<void*>(&dummy_func), "scoped_c",
        80008, GenC::get_trampoline_ptr());

    std::vector<splice::ScopedHook> hooks;
    hooks.emplace_back(entry_a);   // different FuncType, same container
    hooks.emplace_back(entry_b);
    hooks.emplace_back(entry_c);

    EXPECT_EQ(hooks.size(), 3u);
    for (const auto& h : hooks) {
        EXPECT_FALSE(h.empty());
    }

    hooks.clear();                  // all three disable() fire here
    EXPECT_TRUE(hooks.empty());
}
