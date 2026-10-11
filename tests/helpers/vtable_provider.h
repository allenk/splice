// ─── An interface that lives in someone else's module ─────────────────────
//
// What COM is, with the Windows removed: a class with virtual functions, whose
// implementation and whose function table are both built into a separate
// shared library. The test binary never sees the concrete class, only this
// interface and a factory, which is what makes the vtable here equivalent to
// the one `CoGetMalloc` or `CreateDXGIFactory1` hands back:
//
//   * it is in a module the test did not compile, so slot resolution cannot be
//     accidentally satisfied by the compiler's own knowledge of the layout;
//   * it is in read-only data, so the protection path is exercised rather than
//     skipped -- which is also why this must not be a table the test builds on
//     its heap;
//   * `twice` calls `scale`, so a group that hooks both has the delegation
//     problem .non_reentrant() exists for, in the smallest possible form.
//
// The 2026-09-28 rule says a test that hooks a function in its own binary
// proves almost nothing about distance. A slot swap has no distance in it, so
// that rule is not what this helper is for; the point here is the page the
// table sits on and the ABI the pointer-to-member is decoded under.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#if defined(_WIN32)
#   if defined(SPLICE_VTABLE_PROVIDER_BUILD)
#       define SPLICE_VTABLE_API __declspec(dllexport)
#   else
#       define SPLICE_VTABLE_API __declspec(dllimport)
#   endif
#else
#   define SPLICE_VTABLE_API __attribute__((visibility("default")))
#endif

namespace splice::test {

// Declaration order matters to the tests, which assert the resolved slot
// indices as well as the behaviour. It is deliberately NOT the same as the
// vtable order on every ABI -- MSVC lays an overload set out in reverse -- and
// that disagreement is one of the things being tested.
class ICalculator {
public:
    virtual int  add(int a, int b) = 0;
    virtual int  scale(int value) = 0;         // value * factor
    virtual int  twice(int value) = 0;         // calls scale(value) twice over
    virtual int  factor() const = 0;
    virtual void set_factor(int factor) = 0;
    virtual int  add(int a) = 0;               // overload: see above

protected:
    ~ICalculator() = default;                  // not virtual: keeps the table flat
};

// A wide interface, for the slot numbers a small one never reaches.
//
// MSVC encodes the vtable displacement in a virtual-call thunk three ways, and
// the boundary is exactly where a decoder goes wrong quietly: `jmp [rax]` for
// slot 0, `jmp [rax+disp8]` up to slot 15 (displacement 0x78), and
// `jmp [rax+disp32]` from slot 16 (0x80) on. A disp8 read as unsigned gets
// slot 16 right by accident and a sign-extended one gets it as -16; neither
// shows up on a six-method interface. The shipping consumer hooks index 39.
//
// No overloads and no virtual destructor here, so both ABIs lay these out in
// declaration order and the tests can assert the index itself.
#define SPLICE_WIDE_METHOD(n) virtual int m##n(int value) = 0;
class IWide {
public:
    SPLICE_WIDE_METHOD(0)  SPLICE_WIDE_METHOD(1)  SPLICE_WIDE_METHOD(2)
    SPLICE_WIDE_METHOD(3)  SPLICE_WIDE_METHOD(4)  SPLICE_WIDE_METHOD(5)
    SPLICE_WIDE_METHOD(6)  SPLICE_WIDE_METHOD(7)  SPLICE_WIDE_METHOD(8)
    SPLICE_WIDE_METHOD(9)  SPLICE_WIDE_METHOD(10) SPLICE_WIDE_METHOD(11)
    SPLICE_WIDE_METHOD(12) SPLICE_WIDE_METHOD(13) SPLICE_WIDE_METHOD(14)
    SPLICE_WIDE_METHOD(15) SPLICE_WIDE_METHOD(16) SPLICE_WIDE_METHOD(17)
    SPLICE_WIDE_METHOD(18) SPLICE_WIDE_METHOD(19) SPLICE_WIDE_METHOD(20)
    SPLICE_WIDE_METHOD(21) SPLICE_WIDE_METHOD(22) SPLICE_WIDE_METHOD(23)

protected:
    ~IWide() = default;
};
#undef SPLICE_WIDE_METHOD

// A long-lived instance owned by the provider module. Never null.
SPLICE_VTABLE_API ICalculator* calculator();

// A second, independently allocated instance sharing the same function table,
// for the case where two objects of one class are hooked at once.
SPLICE_VTABLE_API ICalculator* other_calculator();

SPLICE_VTABLE_API IWide* wide();

// The address the provider's own table holds for a slot, read inside the
// provider so the test can compare it against what splice resolved without
// reaching into the table itself.
SPLICE_VTABLE_API void* table_entry(int slot_index);
SPLICE_VTABLE_API void* wide_table_entry(int slot_index);

} // namespace splice::test
