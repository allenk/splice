// ─── Splice example: member-function hook ─────────────────────────────────
//
// Hooks a non-virtual C++ member function with SPLICE_HOOK_MEMBER. The
// callback receives the implicit `this` as its first explicit argument.
//
// Build:
//   cmake --build ... --target splice_example_member_function
//
// Expected output:
//   counter.add(5)  before hook = 5
//   [hook] Counter::add(self=0x.., delta=5)
//   counter.add(5)  after hook  = 1005   (hook added +1000)
// ───────────────────────────────────────────────────────────────────────────
#include <splice/splice.h>

#include <cstdio>

class Counter {
public:
    // Non-virtual member — its pointer-to-member carries a real code
    // address that SPLICE_HOOK_MEMBER can patch. (A virtual would be a
    // vtable offset; hook the resolved slot with SPLICE_HOOK_ADDR instead.)
    int add(int delta);   // defined out-of-line below (noinline)

    int value() const { return m_value; }

private:
    int m_value = 0;
};

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
int Counter::add(int delta) {
    m_value += delta;
    return m_value;
}

int main() {
    Counter c;
    std::printf("counter.add(5)  before hook = %d\n", c.add(5));
    c = Counter{};   // reset

    SPLICE_HOOK_MEMBER(Counter::add)
        .onInvoke([](auto orig, Counter* self, int delta) {
            std::printf("[hook] Counter::add(self=%p, delta=%d)\n",
                        static_cast<void*>(self), delta);
            return orig(self, delta) + 1000;
        });

    splice::install_all();

    if (!splice_is_hooked(reinterpret_cast<void*>(
            splice::detail::member_code_addr<decltype(&Counter::add)>(
                &Counter::add)))) {
        std::printf("(member not patchable on this build — stub backend)\n");
        return 0;
    }

    std::printf("counter.add(5)  after hook  = %d\n", c.add(5));
    return 0;
}
