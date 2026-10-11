// ─── The provider module's side of the interface ──────────────────────────
#define SPLICE_VTABLE_PROVIDER_BUILD 1
#include "vtable_provider.h"

#include <cstring>

namespace splice::test {

namespace {

// Not `final`, and the delegation below launders `this` through a volatile
// pointer, for one reason: a virtual call the compiler can resolve statically
// is not a virtual call at run time. Inside a final class's own member
// function the dynamic type is known, the call to scale() becomes a direct
// call, and a hook on scale's vtable slot would never see it -- the test
// would then be asserting the absence of something that could not have
// happened. The control case in test_vtable_portable.cpp is what actually
// proves the nested call still goes through the table.
class Calculator : public ICalculator {
public:
    int add(int a, int b) override { return a + b; }
    int scale(int value) override { return value * m_factor; }

    // Delegates to another method of the same interface, through the vtable,
    // so a hook on `scale` sees the call that `twice` makes. This is the DXGI
    // shape in miniature: Present1 calls Present, both are hooked, and nothing
    // about that is specific to graphics.
    int twice(int value) override {
        ICalculator* volatile self = this;
        ICalculator* target = self;
        return target->scale(value) + target->scale(value);
    }

    int  factor() const override { return m_factor; }
    void set_factor(int factor) override { m_factor = factor; }
    int  add(int a) override { return a + 1000; }

private:
    int m_factor = 3;
};

Calculator& first() {
    static Calculator instance;
    return instance;
}

Calculator& second() {
    static Calculator instance;
    return instance;
}

// Each method returns a number that names it, so a test that resolves slot 16
// and calls it can tell m16 from m15 without trusting the index it just
// resolved.
#define SPLICE_WIDE_BODY(n) int m##n(int value) override { return value * 100 + n; }
class Wide : public IWide {
public:
    SPLICE_WIDE_BODY(0)  SPLICE_WIDE_BODY(1)  SPLICE_WIDE_BODY(2)
    SPLICE_WIDE_BODY(3)  SPLICE_WIDE_BODY(4)  SPLICE_WIDE_BODY(5)
    SPLICE_WIDE_BODY(6)  SPLICE_WIDE_BODY(7)  SPLICE_WIDE_BODY(8)
    SPLICE_WIDE_BODY(9)  SPLICE_WIDE_BODY(10) SPLICE_WIDE_BODY(11)
    SPLICE_WIDE_BODY(12) SPLICE_WIDE_BODY(13) SPLICE_WIDE_BODY(14)
    SPLICE_WIDE_BODY(15) SPLICE_WIDE_BODY(16) SPLICE_WIDE_BODY(17)
    SPLICE_WIDE_BODY(18) SPLICE_WIDE_BODY(19) SPLICE_WIDE_BODY(20)
    SPLICE_WIDE_BODY(21) SPLICE_WIDE_BODY(22) SPLICE_WIDE_BODY(23)
};
#undef SPLICE_WIDE_BODY

Wide& wide_instance() {
    static Wide instance;
    return instance;
}

} // namespace

ICalculator* calculator() { return &first(); }

IWide* wide() { return &wide_instance(); }

void* wide_table_entry(int slot_index) {
    if (slot_index < 0) return nullptr;
    void** table = nullptr;
    void* object = static_cast<void*>(&wide_instance());
    std::memcpy(&table, object, sizeof(table));
    return table[slot_index];
}

ICalculator* other_calculator() { return &second(); }

void* table_entry(int slot_index) {
    if (slot_index < 0) return nullptr;
    void** table = nullptr;
    void* object = static_cast<void*>(&first());
    std::memcpy(&table, object, sizeof(table));
    return table[slot_index];
}

} // namespace splice::test
