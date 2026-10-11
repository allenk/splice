// ─── Pointer-to-virtual-member → vtable slot ──────────────────────────────
//
// Every case here resolves a slot and then CALLS THROUGH IT, asserting on
// which method ran rather than on an index. That is deliberate: the index is
// not the same on every ABI, and a test that pinned one would be asserting a
// property of the compiler rather than of splice.
//
// What the indices actually are, measured 2026-10-11 on this interface's
// declaration order -- add(int,int), scale, twice, factor, set_factor,
// add(int):
//
//   gcc / clang (Itanium)   declaration order, so add(int) is last, slot 5
//   MSVC x64                an overload set is laid out in REVERSE, so
//                           add(int) is slot 0 and add(int,int) is slot 1
//
// A resolver that counted declarations would be right on one of those two and
// silently wrong on the other, which is why the decoder reads what the
// compiler emitted instead.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/engine.h>

#include "vtable_provider.h"

#include <cstdint>
#include <cstring>

namespace {

using splice::test::ICalculator;

template <typename PMF>
int slot_of(PMF pmf) {
    unsigned char raw[sizeof(PMF)];
    std::memcpy(raw, &pmf, sizeof(PMF));
    return splice_vtable_slot_of_pmf(raw, static_cast<unsigned int>(sizeof(PMF)));
}

// Call the provider's own table entry for `slot` as a free function taking the
// object first. That this works at all is the other half of what the resolver
// promises: member_free_fn_t maps Ret(Class::*)(Args...) onto
// Ret(*)(Class*, Args...), and both ABIs splice targets pass `this` as the
// first argument.
int call_int_int(int slot, int a, int b) {
    auto fn = reinterpret_cast<int (*)(ICalculator*, int, int)>(
        splice::test::table_entry(slot));
    return fn(splice::test::calculator(), a, b);
}

int call_int(int slot, int a) {
    auto fn = reinterpret_cast<int (*)(ICalculator*, int)>(
        splice::test::table_entry(slot));
    return fn(splice::test::calculator(), a);
}

} // namespace

TEST(VtableSlot, each_virtual_method_resolves_to_the_slot_that_calls_it) {
    const int add2 = slot_of(static_cast<int (ICalculator::*)(int, int)>(&ICalculator::add));
    const int scale = slot_of(&ICalculator::scale);

    ASSERT_GE(add2, 0);
    ASSERT_GE(scale, 0);

    splice::test::calculator()->set_factor(3);
    EXPECT_EQ(call_int_int(add2, 3, 4), 7);     // add
    EXPECT_EQ(call_int(scale, 5), 15);          // scale, factor 3
}

TEST(VtableSlot, two_overloads_resolve_to_two_different_slots) {
    const int add2 = slot_of(static_cast<int (ICalculator::*)(int, int)>(&ICalculator::add));
    const int add1 = slot_of(static_cast<int (ICalculator::*)(int)>(&ICalculator::add));

    ASSERT_GE(add2, 0);
    ASSERT_GE(add1, 0);
    EXPECT_NE(add2, add1);

    // Which is which is settled by calling them, not by the numbers.
    EXPECT_EQ(call_int_int(add2, 3, 4), 7);
    EXPECT_EQ(call_int(add1, 3), 1003);
}

TEST(VtableSlot, a_const_virtual_method_resolves_like_any_other) {
    const int factor = slot_of(&ICalculator::factor);
    ASSERT_GE(factor, 0);

    splice::test::calculator()->set_factor(7);
    auto fn = reinterpret_cast<int (*)(ICalculator*)>(splice::test::table_entry(factor));
    EXPECT_EQ(fn(splice::test::calculator()), 7);
    splice::test::calculator()->set_factor(3);
}

// ─── the slot range a six-method interface never reaches ──────────────────

TEST(VtableSlot, the_displacement_boundary_resolves_on_both_sides_of_it) {
    using splice::test::IWide;
    // 15 is the last slot MSVC reaches with a one-byte displacement (0x78) and
    // 16 is the first that needs four (0x80). Read as unsigned, a one-byte 0x80
    // would give slot 16 by accident; sign-extended, slot -16. Both answers are
    // wrong for the right reason, so both sides of the boundary are checked,
    // and each is confirmed by calling it: m<n> returns value*100 + n.
    const struct { int expected; int (IWide::*pmf)(int); } rows[] = {
        {0,  &IWide::m0},  {1,  &IWide::m1},  {14, &IWide::m14},
        {15, &IWide::m15}, {16, &IWide::m16}, {17, &IWide::m17},
        {23, &IWide::m23},
    };

    for (const auto& row : rows) {
        const int slot = slot_of(row.pmf);
        ASSERT_EQ(slot, row.expected) << "slot for m" << row.expected;
        auto fn = reinterpret_cast<int (*)(IWide*, int)>(
            splice::test::wide_table_entry(slot));
        EXPECT_EQ(fn(splice::test::wide(), 2), 200 + row.expected)
            << "slot " << slot << " does not run m" << row.expected;
    }
}

namespace {

struct Plain {
    int value = 0;
    int get() const { return value; }       // not virtual
};

struct BaseA {
    virtual int a() { return 1; }
    virtual ~BaseA() = default;
};
struct BaseB {
    virtual int b() { return 2; }
    virtual ~BaseB() = default;
};
// A method reached through the second base needs `this` adjusted before the
// call, which a slot index alone cannot express.
struct Both : BaseA, BaseB {
    int a() override { return 10; }
    int b() override { return 20; }
};

} // namespace

TEST(VtableSlot, a_non_virtual_member_is_refused) {
    EXPECT_EQ(slot_of(&Plain::get), -1);
}

TEST(VtableSlot, a_member_needing_a_this_adjustment_is_refused) {
    // What actually carries an adjustment is a pointer-to-member CONVERTED
    // from a base class: `&BaseB::b` as an `int (Both::*)()` has to move
    // `this` to the BaseB subobject before the call, and a slot index alone
    // cannot say that. Measured 2026-10-11, gcc 13 x86_64: {ptr=1, adj=8}.
    //
    // Refused on both ABIs, for two reasons that are the same fact arriving at
    // different times: on Itanium the adjustment field is non-zero, and under
    // MSVC a pointer-to-member of a multiply-inherited class is wider than a
    // pointer at all, so the refusal lands before any decoding.
    int (BaseB::*from_base)() = &BaseB::b;
    int (Both::*converted)() = from_base;
    EXPECT_EQ(slot_of(converted), -1);
}

#if !defined(_MSC_VER)
TEST(VtableSlot, an_override_of_a_secondary_base_resolves_through_the_primary_table) {
    // Itanium only, and not what I expected before measuring it: `&Both::b`
    // does NOT need an adjustment, even though `b` is introduced by the second
    // base. gcc gives {ptr=25, adj=0} -- slot 3 of Both's PRIMARY vtable --
    // and calling that slot with an unadjusted `Both*` runs Both::b.
    //
    // So the test asserts what the ABI does rather than what a this-adjustment
    // story would predict, and it proves it by calling through the slot.
    Both object;
    void** table = nullptr;
    std::memcpy(&table, static_cast<void*>(&object), sizeof(table));

    const int slot = slot_of(static_cast<int (Both::*)()>(&Both::b));
    ASSERT_GE(slot, 0);
    auto fn = reinterpret_cast<int (*)(void*)>(table[slot]);
    EXPECT_EQ(fn(&object), 20);

    // And the first base, which never needed one either.
    EXPECT_GE(slot_of(static_cast<int (Both::*)()>(&Both::a)), 0);
}
#endif

// ─── the guards no compiler exercises ─────────────────────────────────────
//
// The checks below are reached only by input a compiler does not emit, so a
// real pointer-to-member cannot drive them. Feeding the C entry point
// fabricated bytes can. This is the difference between a guard that is known
// to work and one that merely looks right: mutating either of the two Itanium
// checks away, or three of the Microsoft ones, is invisible to every test that
// uses a pointer-to-member the compiler made.

namespace {

// The object representation of a pointer-to-member, as the C entry point sees
// it: a word, and on the Itanium ABI a second word for the this-adjustment.
int resolve_words(std::uintptr_t first, std::uintptr_t second) {
    std::uintptr_t raw[2] = {first, second};
    return splice_vtable_slot_of_pmf(raw, sizeof(raw));
}

} // namespace

#if !defined(_MSC_VER)
TEST(VtableSlot, fabricated_itanium_pointers_meet_their_guards) {
#   if defined(__aarch64__) || defined(__arm__)
    // ARM puts the virtual marker in the adjustment word and the plain byte
    // offset in the first. Measured on the Android NDK, aarch64: the second
    // virtual method of an interface is {ptr = 8, adj = 1}.
    EXPECT_EQ(resolve_words(16, 1), 2);     // well formed, so the rest are the guards
    EXPECT_EQ(resolve_words(16, 0), -1);    // marker clear: not a virtual member
    EXPECT_EQ(resolve_words(18, 1), -1);    // 18 is not a whole number of slots
    EXPECT_EQ(resolve_words(16, 3), -1);    // adj >> 1 is a this-adjustment
#   else
    // Generic Itanium: the marker is the low bit of the first word, and the
    // offset is what remains.
    EXPECT_EQ(resolve_words(17, 0), 2);     // well formed
    EXPECT_EQ(resolve_words(16, 0), -1);    // marker clear: a plain code address
    EXPECT_EQ(resolve_words(3, 0), -1);     // offset 2 is not a whole slot
    EXPECT_EQ(resolve_words(17, 8), -1);    // a non-zero this-adjustment
#   endif
}
#endif

#if defined(_MSC_VER) && defined(_M_X64)
namespace {

// A fabricated virtual-call thunk. Only read, never executed, which is why a
// plain array is enough.
struct FakeThunk {
    unsigned char code[16] = {};
    [[nodiscard]] int resolve() const {
        const void* address = code;
        return splice_vtable_slot_of_pmf(&address, sizeof(address));
    }
};

} // namespace

TEST(VtableSlot, fabricated_microsoft_thunks_meet_their_guards) {
    // The three forms a compiler does emit, for comparison.
    FakeThunk slot0;      // mov rax,[rcx]; jmp [rax]
    const unsigned char s0[] = {0x48, 0x8B, 0x01, 0xFF, 0x20};
    std::memcpy(slot0.code, s0, sizeof(s0));
    EXPECT_EQ(slot0.resolve(), 0);

    FakeThunk slot15;     // jmp [rax+0x78], the last one-byte displacement
    const unsigned char s15[] = {0x48, 0x8B, 0x01, 0xFF, 0x60, 0x78};
    std::memcpy(slot15.code, s15, sizeof(s15));
    EXPECT_EQ(slot15.resolve(), 15);

    FakeThunk slot16;     // jmp [rax+0x80], the first four-byte one
    const unsigned char s16[] = {0x48, 0x8B, 0x01, 0xFF, 0xA0, 0x80, 0x00, 0x00, 0x00};
    std::memcpy(slot16.code, s16, sizeof(s16));
    EXPECT_EQ(slot16.resolve(), 16);

    // And the ones it does not. A one-byte displacement with the high bit set
    // is negative, which is how slot 16 would be read as -16 by a decoder that
    // took the byte as unsigned and never looked at the boundary.
    FakeThunk negative;
    const unsigned char neg[] = {0x48, 0x8B, 0x01, 0xFF, 0x60, 0x80};
    std::memcpy(negative.code, neg, sizeof(neg));
    EXPECT_EQ(negative.resolve(), -1);

    FakeThunk misaligned;  // not a whole number of slots
    const unsigned char mis[] = {0x48, 0x8B, 0x01, 0xFF, 0x60, 0x04};
    std::memcpy(misaligned.code, mis, sizeof(mis));
    EXPECT_EQ(misaligned.resolve(), -1);

    FakeThunk adjustor;    // add rcx, 8 first: a secondary base, refused
    const unsigned char adj[] = {0x48, 0x83, 0xC1, 0x08, 0x48, 0x8B, 0x01, 0xFF, 0x20};
    std::memcpy(adjustor.code, adj, sizeof(adj));
    EXPECT_EQ(adjustor.resolve(), -1);

    // Three bytes that are not the vtable load, followed by a jump form that
    // is. Without the prefix check the decoder would step over them and report
    // slot 2, which is the whole reason it checks: the displacement only means
    // a vtable slot if what came before it loaded the vtable.
    FakeThunk wrong_prefix;
    const unsigned char wrong[] = {0x48, 0x83, 0xC1, 0xFF, 0x60, 0x10};
    std::memcpy(wrong_prefix.code, wrong, sizeof(wrong));
    EXPECT_EQ(wrong_prefix.resolve(), -1);

    FakeThunk unknown_jump;
    const unsigned char unk[] = {0x48, 0x8B, 0x01, 0xEB, 0x10};
    std::memcpy(unknown_jump.code, unk, sizeof(unk));
    EXPECT_EQ(unknown_jump.resolve(), -1);
}

TEST(VtableSlot, a_fabricated_jump_stub_is_followed_to_the_thunk_behind_it) {
    // An incremental-linked build puts an `E9 rel32` in front of the thunk.
    // Built by hand here so the case does not depend on how this binary was
    // linked -- which is exactly how it would stop being covered.
    static unsigned char block[64] = {};
    unsigned char* stub = block;
    unsigned char* thunk = block + 32;

    const unsigned char body[] = {0x48, 0x8B, 0x01, 0xFF, 0x60, 0x10};   // slot 2
    std::memcpy(thunk, body, sizeof(body));

    stub[0] = 0xE9;
    const std::int32_t rel32 = static_cast<std::int32_t>(thunk - (stub + 5));
    std::memcpy(stub + 1, &rel32, sizeof(rel32));

    const void* address = stub;
    EXPECT_EQ(splice_vtable_slot_of_pmf(&address, sizeof(address)), 2);
}
#endif

TEST(VtableSlot, a_pointer_to_member_shorter_than_a_pointer_is_refused) {
    unsigned char raw[2] = {0xFF, 0xFF};
    EXPECT_EQ(splice_vtable_slot_of_pmf(raw, sizeof(raw)), -1);
    EXPECT_EQ(splice_vtable_slot_of_pmf(nullptr, 8), -1);
}
