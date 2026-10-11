// ─── R5: the graphics interfaces, in-process ───────────────────────────────
//
// The closest thing to the shipping consumer without leaving our own process.
// `IDXGISwapChain::Present` is the function the overlay actually patches, and
// everything about it that is not the overlay's own frame pacing can be proved
// here: the slot number, the table it lives in, the group transaction over it,
// the gates, the coexistence guard, and what the coordinated tier makes of a
// prologue inside `dxgi.dll`.
//
// Three things this adds that no earlier runner could:
//
//   1. **Slot numbers from a published ABI, with no device at all.** A
//      pointer-to-member is resolved from the headers, so the first four cases
//      run on any Windows and in CI. `ID3D12CommandQueue` is the useful one:
//      its `Wait` and `GetTimestampFrequency` sit on either side of the point
//      where the compiler changes how it encodes a slot displacement -- two
//      adjacent methods of a shipped interface, one on each side.
//
//   2. **A table with more than one live instance.** `IMalloc` from
//      `CoGetMalloc` is a process singleton, so it could not distinguish
//      per-object from per-table. Two swap chains can, and the answer belongs
//      in the record rather than in a reader's assumption.
//
//   3. **`.expect()` on the slot an overlay fights over.** Steam, Discord and
//      RTSS all want slot 8 of this interface. The guard that refuses when the
//      slot no longer holds what the caller believed is proved here against
//      the real address rather than a synthesised one.
//
// The device cases skip when a device cannot be created, which is the same
// call this project already makes for benchmarks: a gate that cannot run on
// the hardware at hand reports that, rather than passing by omission.
//
// D3D is reached through `LoadLibrary` and `GetProcAddress`, not an import
// library, for a reason worth stating: a hard import would make this binary
// fail to start on a machine with no D3D, taking every unrelated test in it
// down as well. It is also how an injector finds these vtables in the first
// place, so the lookup here is the one the consumer performs.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/live.h>

#if defined(_WIN32)

#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#   include <d3d11.h>
#   include <d3d12.h>
#   include <dxgi.h>

#   include "pmf_slot.h"
#   include "test_env.h"

#   include <atomic>
#   include <cstring>

namespace {

using splice::test::slot_of;

std::atomic<long long> g_present_calls{0};
std::atomic<long long> g_present_b_calls{0};
std::atomic<long long> g_desc_calls{0};
std::atomic<long long> g_signal_calls{0};

void reset_counters() {
    g_present_calls.store(0);
    g_present_b_calls.store(0);
    g_desc_calls.store(0);
    g_signal_calls.store(0);
}

// A hidden window, created once and destroyed with the suite. Never shown:
// windowed Present on the bitblt model does not need the window mapped, and a
// test that stole focus on the owner's desktop would not be run twice.
class HiddenWindow {
public:
    bool create(const wchar_t* title) {
        WNDCLASSEXW cls{};
        cls.cbSize        = sizeof(cls);
        cls.lpfnWndProc   = ::DefWindowProcW;
        cls.hInstance     = ::GetModuleHandleW(nullptr);
        cls.lpszClassName = L"SpliceGraphicsRunner";
        ::RegisterClassExW(&cls);               // idempotent across instances
        m_window = ::CreateWindowExW(0, cls.lpszClassName, title,
                                     WS_OVERLAPPEDWINDOW, 0, 0, 320, 240,
                                     nullptr, nullptr, cls.hInstance, nullptr);
        return m_window != nullptr;
    }
    void destroy() {
        if (m_window != nullptr) {
            ::DestroyWindow(m_window);
            m_window = nullptr;
        }
    }
    // DXGI does not need the queue serviced for a windowed present, but a
    // window with an unserviced queue is a window the shell will eventually
    // ask about, so it is drained between frames.
    static void pump() {
        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != 0) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }
    [[nodiscard]] HWND handle() const { return m_window; }

private:
    HWND m_window = nullptr;
};

DXGI_SWAP_CHAIN_DESC windowed_desc(HWND window) {
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount                        = 1;
    desc.BufferDesc.Width                   = 320;
    desc.BufferDesc.Height                  = 240;
    desc.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator   = 0;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow                       = window;
    desc.SampleDesc.Count                   = 1;
    desc.SampleDesc.Quality                 = 0;
    desc.Windowed                           = TRUE;
    desc.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;
    return desc;
}

// A present that lands on a window nobody can see is allowed to report that
// it was occluded. Both answers are the interface working.
bool present_succeeded(HRESULT hr) {
    return hr == S_OK || hr == DXGI_STATUS_OCCLUDED;
}

void* table_entry(void* object, int index) {
    void** table = nullptr;
    std::memcpy(&table, object, sizeof(table));
    return table[index];
}

} // namespace

// ─── what the resolver says about the D3D ABI: no device needed ───────────

TEST(GraphicsSlots, the_resolver_agrees_with_the_dxgi_abi) {
    // IUnknown takes 0 to 2, IDXGIObject 3 to 6, IDXGIDeviceSubObject 7, and
    // IDXGISwapChain's own methods follow. Present at 8 is the number every
    // overlay in the world hard-codes, which is exactly why it is asserted as
    // a literal here: ours to resolve, not ours to choose.
    EXPECT_EQ(slot_of(&IDXGISwapChain::GetParent), 6);
    EXPECT_EQ(slot_of(&IDXGISwapChain::GetDevice), 7);
    EXPECT_EQ(slot_of(&IDXGISwapChain::Present), 8);
    EXPECT_EQ(slot_of(&IDXGISwapChain::GetBuffer), 9);
    EXPECT_EQ(slot_of(&IDXGISwapChain::GetDesc), 12);
    EXPECT_EQ(slot_of(&IDXGISwapChain::ResizeBuffers), 13);
    EXPECT_EQ(slot_of(&IDXGISwapChain::GetLastPresentCount), 17);
}

TEST(GraphicsSlots, the_resolver_agrees_with_the_d3d11_device_abi) {
    // Forty own methods, which puts the useful ones far past any displacement
    // a one-byte form could carry. GetCreationFlags at 38 is a 0x130 offset.
    EXPECT_EQ(slot_of(&ID3D11Device::CreateBuffer), 3);
    EXPECT_EQ(slot_of(&ID3D11Device::CreateTexture2D), 5);
    EXPECT_EQ(slot_of(&ID3D11Device::CreateRenderTargetView), 9);
    EXPECT_EQ(slot_of(&ID3D11Device::GetFeatureLevel), 37);
    EXPECT_EQ(slot_of(&ID3D11Device::GetCreationFlags), 38);
    EXPECT_EQ(slot_of(&ID3D11Device::GetImmediateContext), 40);
    EXPECT_EQ(slot_of(&ID3D11Device::GetExceptionMode), 42);
}

TEST(GraphicsSlots, the_resolver_agrees_with_the_d3d12_queue_abi) {
    EXPECT_EQ(slot_of(&ID3D12CommandQueue::SetName), 6);
    EXPECT_EQ(slot_of(&ID3D12CommandQueue::GetDevice), 7);
    EXPECT_EQ(slot_of(&ID3D12CommandQueue::ExecuteCommandLists), 10);
    EXPECT_EQ(slot_of(&ID3D12CommandQueue::Signal), 14);
    EXPECT_EQ(slot_of(&ID3D12CommandQueue::Wait), 15);
    EXPECT_EQ(slot_of(&ID3D12CommandQueue::GetTimestampFrequency), 16);
    EXPECT_EQ(slot_of(&ID3D12CommandQueue::GetDesc), 18);
}

TEST(GraphicsSlots, the_displacement_boundary_falls_between_two_adjacent_methods) {
    // Under the Microsoft ABI a pointer to a virtual member is a thunk that
    // tail-jumps through the slot, and the slot's displacement is encoded in
    // one byte up to 0x78 and in four from 0x80. Slot 15 is 0x78 and slot 16
    // is 0x80, so the compiler changes form between two methods declared one
    // line apart in a shipped Microsoft header.
    //
    // What this case proves is narrower than it first looks, and the narrower
    // claim is the true one: it proves the decoder handles BOTH forms, not
    // that it reads the one-byte one correctly. Mutating that byte from signed
    // to unsigned survives every test in this suite, because the compiler only
    // ever emits the one-byte form for 0x00 through 0x78 and those bytes are
    // the same number either way. Deleting the four-byte form, which is what
    // this case is actually sensitive to, is caught here five times over.
    //
    // Having it land on a real interface rather than a synthesised one is why
    // it is here and not only in the unit runner: these two slot numbers are
    // fixed by a shipped header, so the boundary cannot drift out from under
    // the assertion.
    EXPECT_EQ(slot_of(&ID3D12CommandQueue::Wait), 15);
    EXPECT_EQ(slot_of(&ID3D12CommandQueue::GetTimestampFrequency), 16);
    EXPECT_EQ(slot_of(&ID3D12CommandQueue::GetTimestampFrequency),
              slot_of(&ID3D12CommandQueue::Wait) + 1)
        << "the two forms do not meet; a slot was dropped or duplicated across "
           "the boundary";

    // And the same step on the interface the overlay uses, where it sits
    // between GetContainingOutput and GetFrameStatistics.
    EXPECT_EQ(slot_of(&IDXGISwapChain::GetContainingOutput), 15);
    EXPECT_EQ(slot_of(&IDXGISwapChain::GetFrameStatistics), 16);
}

// ─── a device, a hidden window and a real swap chain ──────────────────────

namespace {

class SwapChainFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        HMODULE d3d11 = ::LoadLibraryW(L"d3d11.dll");
        if (d3d11 == nullptr) {
            s_reason = "d3d11.dll is not present on this machine";
            return;
        }
        auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
            reinterpret_cast<void*>(
                ::GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")));
        if (create == nullptr) {
            s_reason = "d3d11.dll has no D3D11CreateDeviceAndSwapChain";
            return;
        }
        if (!s_window_a.create(L"splice graphics runner A") ||
            !s_window_b.create(L"splice graphics runner B")) {
            s_reason = "a window could not be created";
            return;
        }

        // Hardware first, then the software rasteriser, because a runner with
        // no GPU is the ordinary case and WARP is enough for every assertion
        // here -- none of them is about pixels.
        const D3D_DRIVER_TYPE drivers[] = { D3D_DRIVER_TYPE_HARDWARE,
                                            D3D_DRIVER_TYPE_WARP };
        HRESULT last = E_FAIL;
        for (const D3D_DRIVER_TYPE driver : drivers) {
            DXGI_SWAP_CHAIN_DESC desc = windowed_desc(s_window_a.handle());
            last = create(nullptr, driver, nullptr, 0, nullptr, 0,
                          D3D11_SDK_VERSION, &desc, &s_chain_a, &s_device,
                          nullptr, &s_context);
            if (SUCCEEDED(last)) break;
        }
        if (s_device == nullptr || s_chain_a == nullptr) {
            s_reason = "no D3D11 device could be created";
            SPLICE_LOGW("graphics runner: D3D11CreateDeviceAndSwapChain failed, hr=0x%08lx",
                        static_cast<unsigned long>(last));
            return;
        }

        // A second swap chain on the second window, from the same factory, so
        // the per-object question has something to be asked about.
        IDXGIDevice*  dxgi_device  = nullptr;
        IDXGIAdapter* adapter      = nullptr;
        IDXGIFactory* factory      = nullptr;
        if (SUCCEEDED(s_device->QueryInterface(__uuidof(IDXGIDevice),
                                               reinterpret_cast<void**>(&dxgi_device))) &&
            SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory),
                                         reinterpret_cast<void**>(&factory)))) {
            DXGI_SWAP_CHAIN_DESC desc = windowed_desc(s_window_b.handle());
            factory->CreateSwapChain(s_device, &desc, &s_chain_b);
        }
        if (factory != nullptr)      factory->Release();
        if (adapter != nullptr)      adapter->Release();
        if (dxgi_device != nullptr)  dxgi_device->Release();
    }

    static void TearDownTestSuite() {
        if (s_chain_b != nullptr) { s_chain_b->Release(); s_chain_b = nullptr; }
        if (s_chain_a != nullptr) { s_chain_a->Release(); s_chain_a = nullptr; }
        if (s_context != nullptr) { s_context->Release(); s_context = nullptr; }
        if (s_device  != nullptr) { s_device->Release();  s_device  = nullptr; }
        s_window_b.destroy();
        s_window_a.destroy();
    }

    void SetUp() override {
        if (s_chain_a == nullptr) GTEST_SKIP() << s_reason;
        reset_counters();
    }

    void TearDown() override {
        if (outcome.active()) EXPECT_TRUE(outcome.disable());
    }

    static HRESULT present_a() {
        HiddenWindow::pump();
        return s_chain_a->Present(0, 0);
    }

    static HiddenWindow        s_window_a;
    static HiddenWindow        s_window_b;
    static ID3D11Device*       s_device;
    static ID3D11DeviceContext* s_context;
    static IDXGISwapChain*     s_chain_a;
    static IDXGISwapChain*     s_chain_b;
    static const char*         s_reason;

    splice::Outcome outcome;
};

HiddenWindow         SwapChainFixture::s_window_a;
HiddenWindow         SwapChainFixture::s_window_b;
ID3D11Device*        SwapChainFixture::s_device  = nullptr;
ID3D11DeviceContext* SwapChainFixture::s_context = nullptr;
IDXGISwapChain*      SwapChainFixture::s_chain_a = nullptr;
IDXGISwapChain*      SwapChainFixture::s_chain_b = nullptr;
const char*          SwapChainFixture::s_reason  = "the suite did not run";

} // namespace

TEST_F(SwapChainFixture, present_runs_through_the_hook_and_still_presents) {
    // The baseline the overlay needs: frames arrive at the callback, the
    // return value the application sees is still the one DXGI produced, and
    // after disable the callback is out of the path.
    ASSERT_TRUE(present_succeeded(present_a()))
        << "the swap chain did not present before anything was hooked";

    outcome = splice::Vtable<IDXGISwapChain>{s_chain_a}
        .on(&IDXGISwapChain::Present,
            [](auto orig, IDXGISwapChain* self, UINT sync, UINT flags) {
                g_present_calls.fetch_add(1, std::memory_order_relaxed);
                return orig(self, sync, flags);
            })
        .install(splice::slot_swap);

    ASSERT_EQ(outcome.state(), splice::State::installed)
        << "stage=" << static_cast<int>(outcome.failure().stage)
        << " site=" << outcome.failure().site.ordinal;

    constexpr int kFrames = 30;
    for (int frame = 0; frame < kFrames; ++frame) {
        EXPECT_TRUE(present_succeeded(present_a())) << "frame " << frame;
    }
    EXPECT_EQ(g_present_calls.load(), kFrames)
        << "a frame went past the hook";

    ASSERT_TRUE(outcome.disable());
    const long long settled = g_present_calls.load();
    EXPECT_TRUE(present_succeeded(present_a()));
    EXPECT_EQ(g_present_calls.load(), settled)
        << "the callback still ran after disable";
}

TEST_F(SwapChainFixture, the_unit_of_a_slot_swap_is_the_table_not_the_object) {
    // Asked rather than assumed, and the answer is the reason this case is
    // here: `Vtable<T>` is handed an object, but what it writes is the
    // object's function TABLE, and every instance of a concrete class shares
    // one. A reader who took "the object" literally would ship a hook they
    // believed was scoped to their own swap chain.
    //
    // IMalloc could not settle this -- CoGetMalloc returns a singleton. Two
    // swap chains can, so the measurement is made here and the test asserts
    // whichever way the tables actually fall.
    if (s_chain_b == nullptr) {
        GTEST_SKIP() << "the second swap chain was not created, so there is "
                        "nothing to compare against";
    }

    void* table_a = nullptr;
    void* table_b = nullptr;
    std::memcpy(&table_a, static_cast<void*>(s_chain_a), sizeof(table_a));
    std::memcpy(&table_b, static_cast<void*>(s_chain_b), sizeof(table_b));
    const bool shared_table = (table_a == table_b);
    SPLICE_LOGI("graphics runner: swap chain tables %p and %p are %s", table_a, table_b,
                shared_table ? "the same" : "different");

    outcome = splice::Vtable<IDXGISwapChain>{s_chain_a}
        .on(&IDXGISwapChain::Present,
            [](auto orig, IDXGISwapChain* self, UINT sync, UINT flags) {
                g_present_calls.fetch_add(1, std::memory_order_relaxed);
                return orig(self, sync, flags);
            })
        .install(splice::slot_swap);
    ASSERT_EQ(outcome.state(), splice::State::installed);

    HiddenWindow::pump();
    EXPECT_TRUE(present_succeeded(s_chain_b->Present(0, 0)));

    if (shared_table) {
        EXPECT_GE(g_present_calls.load(), 1)
            << "one table, two objects: a swap chain sharing the patched table "
               "must go through the hook";
    } else {
        EXPECT_EQ(g_present_calls.load(), 0)
            << "separate tables, so the second chain must be untouched";
    }

    // Either way the hooked object is hooked, and the other chain still works.
    const long long before = g_present_calls.load();
    EXPECT_TRUE(present_succeeded(present_a()));
    EXPECT_EQ(g_present_calls.load(), before + 1);
}

TEST_F(SwapChainFixture, expect_guards_the_slot_an_overlay_fights_over) {
    // Slot 8 is the one Steam, Discord and RTSS all want. `.expect()` is the
    // guard for arriving second: install only if the slot still holds what the
    // caller believed it held.
    void* const current = table_entry(static_cast<void*>(s_chain_a), 8);
    ASSERT_NE(current, nullptr);

    // The address is right, so it installs.
    outcome = splice::Vtable<IDXGISwapChain>{s_chain_a}
        .on(&IDXGISwapChain::Present).expect(current)
        .onInvoke([](auto orig, IDXGISwapChain* self, UINT sync, UINT flags) {
            g_present_calls.fetch_add(1, std::memory_order_relaxed);
            return orig(self, sync, flags);
        })
        .install(splice::slot_swap);
    ASSERT_EQ(outcome.state(), splice::State::installed);
    EXPECT_TRUE(present_succeeded(present_a()));
    EXPECT_EQ(g_present_calls.load(), 1);
    ASSERT_TRUE(outcome.disable());

    // The slot is back to what it was, which is what makes the next assertion
    // about `expect` and not about disable.
    ASSERT_EQ(table_entry(static_cast<void*>(s_chain_a), 8), current);

    // Now the same install with a stale belief about the slot. Somebody else
    // got there first; this must refuse, and must not write.
    g_present_calls.store(0);
    splice::Outcome stale = splice::Vtable<IDXGISwapChain>{s_chain_a}
        .on(&IDXGISwapChain::Present).expect(static_cast<void*>(&g_present_calls))
        .onInvoke([](auto orig, IDXGISwapChain* self, UINT sync, UINT flags) {
            g_present_calls.fetch_add(1000, std::memory_order_relaxed);
            return orig(self, sync, flags);
        })
        .install(splice::slot_swap);

    EXPECT_EQ(stale.state(), splice::State::refused);
    EXPECT_EQ(stale.installed_sites(), 0u);
    // Unreachable while `expect` works, and deliberately not left to chance:
    // this table is shared with every swap chain in the process, so a hook
    // that installed here when it should not have would otherwise outlive the
    // test and fail an unrelated one instead of this one. Mutation testing
    // produced exactly that, which is why the line is here.
    if (stale.active()) EXPECT_TRUE(stale.disable());
    EXPECT_EQ(table_entry(static_cast<void*>(s_chain_a), 8), current)
        << "a refused install wrote to the slot anyway";
    EXPECT_TRUE(present_succeeded(present_a()));
    EXPECT_EQ(g_present_calls.load(), 0)
        << "a refused install put a callback in the path";
}

TEST_F(SwapChainFixture, two_methods_of_one_interface_with_a_gate_on_each) {
    // The fluent chain on a real interface, with the budget gate doing the
    // work: GetDesc is hooked once and Present every time. The point is that
    // the gate is evaluated at call time on a function in dxgi.dll like any
    // other, and that a spent budget leaves the original in the path rather
    // than removing the hook.
    outcome = splice::Vtable<IDXGISwapChain>{s_chain_a}
        .on(&IDXGISwapChain::GetDesc).once()
        .onInvoke([](auto orig, IDXGISwapChain* self, DXGI_SWAP_CHAIN_DESC* desc) {
            g_desc_calls.fetch_add(1, std::memory_order_relaxed);
            return orig(self, desc);
        })
        .on(&IDXGISwapChain::Present,
            [](auto orig, IDXGISwapChain* self, UINT sync, UINT flags) {
                g_present_calls.fetch_add(1, std::memory_order_relaxed);
                return orig(self, sync, flags);
            })
        .non_reentrant()
        .install(splice::slot_swap);

    ASSERT_EQ(outcome.state(), splice::State::installed);
    EXPECT_EQ(outcome.installed_sites(), 2u);

    DXGI_SWAP_CHAIN_DESC desc{};
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(s_chain_a->GetDesc(&desc), S_OK) << "call " << i;
        EXPECT_EQ(desc.BufferDesc.Width, 320u)
            << "the gate changed what the interface returned";
    }
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(present_succeeded(present_a()));
    }

    EXPECT_EQ(g_desc_calls.load(), 1) << "the budget gate let a second call in";
    EXPECT_EQ(g_present_calls.load(), 4) << "the ungated site dropped a call";
}

TEST_F(SwapChainFixture, the_live_tier_on_present_is_measured_not_assumed) {
    // Whether DXGI's Present has a prologue this planner can relocate is a
    // property of a Microsoft binary, so this case accepts both answers and
    // pins what must hold either way: the swap chain still presents, and a
    // refusal wrote nothing.
    //
    // Local gate, and for the same reason as the contention runner next door
    // rather than LiveHookStress's: this suspends every thread in the process,
    // and with a D3D device alive that set includes the runtime's own worker
    // threads. There is no cross-modifying-code window to lose -- the risk is
    // cost and the breadth of what gets frozen, not correctness.
    if (splice::test::env_is_set("CI")) {
        GTEST_SKIP() << "coordinated-tier graphics gate: suspends every thread "
                        "in the process with a live D3D device; run locally";
    }

    void* const before = table_entry(static_cast<void*>(s_chain_a), 8);

    splice::Outcome live = splice::Vtable<IDXGISwapChain>{s_chain_a}
        .on(&IDXGISwapChain::Present,
            [](auto orig, IDXGISwapChain* self, UINT sync, UINT flags) {
                g_present_calls.fetch_add(1, std::memory_order_relaxed);
                return orig(self, sync, flags);
            })
        .non_reentrant()
        .install(splice::live);

    SPLICE_LOGI("graphics runner, coordinated tier on Present: state=%d stage=%d op=%d err=%u",
                static_cast<int>(live.state()),
                static_cast<int>(live.failure().stage),
                static_cast<int>(live.failure().operation),
                live.failure().os_error);

    // This tier patches the function, never the table, whatever the outcome.
    EXPECT_EQ(table_entry(static_cast<void*>(s_chain_a), 8), before);

    if (live.state() == splice::State::installed) {
        EXPECT_TRUE(present_succeeded(present_a()));
        EXPECT_GE(g_present_calls.load(), 1)
            << "the inline patch installed but no frame reached the callback";
        ASSERT_TRUE(live.disable());
    } else {
        EXPECT_NE(live.state(), splice::State::lost);
        EXPECT_EQ(live.installed_sites(), 0u);
    }

    EXPECT_TRUE(present_succeeded(present_a()))
        << "the swap chain stopped presenting";
}

// ─── D3D12: a command queue and a fence that has to keep advancing ────────

namespace {

class CommandQueueFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        HMODULE d3d12 = ::LoadLibraryW(L"d3d12.dll");
        if (d3d12 == nullptr) {
            s_reason = "d3d12.dll is not present on this machine";
            return;
        }
        auto create = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
            reinterpret_cast<void*>(::GetProcAddress(d3d12, "D3D12CreateDevice")));
        if (create == nullptr) {
            s_reason = "d3d12.dll has no D3D12CreateDevice";
            return;
        }
        const HRESULT hr = create(nullptr, D3D_FEATURE_LEVEL_11_0,
                                  __uuidof(ID3D12Device),
                                  reinterpret_cast<void**>(&s_device));
        if (FAILED(hr) || s_device == nullptr) {
            s_reason = "no D3D12 device could be created";
            SPLICE_LOGW("graphics runner: D3D12CreateDevice failed, hr=0x%08lx",
                        static_cast<unsigned long>(hr));
            return;
        }
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (FAILED(s_device->CreateCommandQueue(
                &desc, __uuidof(ID3D12CommandQueue),
                reinterpret_cast<void**>(&s_queue)))) {
            s_reason = "the device would not create a command queue";
            return;
        }
        if (FAILED(s_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                         __uuidof(ID3D12Fence),
                                         reinterpret_cast<void**>(&s_fence)))) {
            s_reason = "the device would not create a fence";
        }
    }

    static void TearDownTestSuite() {
        if (s_fence  != nullptr) { s_fence->Release();  s_fence  = nullptr; }
        if (s_queue  != nullptr) { s_queue->Release();  s_queue  = nullptr; }
        if (s_device != nullptr) { s_device->Release(); s_device = nullptr; }
    }

    void SetUp() override {
        if (s_queue == nullptr || s_fence == nullptr) GTEST_SKIP() << s_reason;
        reset_counters();
    }

    void TearDown() override {
        if (outcome.active()) EXPECT_TRUE(outcome.disable());
    }

    // An empty queue executes a Signal almost immediately, but "almost" is not
    // a thing to assert on, so this waits.
    static bool wait_for(UINT64 value) {
        for (int spin = 0; spin < 2000; ++spin) {
            if (s_fence->GetCompletedValue() >= value) return true;
            ::Sleep(1);
        }
        return false;
    }

    static ID3D12Device*       s_device;
    static ID3D12CommandQueue* s_queue;
    static ID3D12Fence*        s_fence;
    static const char*         s_reason;

    splice::Outcome outcome;
};

ID3D12Device*       CommandQueueFixture::s_device = nullptr;
ID3D12CommandQueue* CommandQueueFixture::s_queue  = nullptr;
ID3D12Fence*        CommandQueueFixture::s_fence  = nullptr;
const char*         CommandQueueFixture::s_reason = "the suite did not run";

} // namespace

TEST_F(CommandQueueFixture, signal_runs_through_the_hook_and_the_fence_advances) {
    // Signal is the D3D12 equivalent of the frame boundary, and the assertion
    // that matters is not that the callback ran but that the GPU still saw the
    // signal: a hook on a queue method that swallowed the call would leave a
    // consumer waiting on a fence that never advances.
    const UINT64 base = s_fence->GetCompletedValue();

    outcome = splice::Vtable<ID3D12CommandQueue>{s_queue}
        .on(&ID3D12CommandQueue::Signal,
            [](auto orig, ID3D12CommandQueue* self, ID3D12Fence* fence, UINT64 value) {
                g_signal_calls.fetch_add(1, std::memory_order_relaxed);
                return orig(self, fence, value);
            })
        .non_reentrant()
        .install(splice::slot_swap);

    ASSERT_EQ(outcome.state(), splice::State::installed)
        << "stage=" << static_cast<int>(outcome.failure().stage)
        << " site=" << outcome.failure().site.ordinal;

    for (UINT64 step = 1; step <= 4; ++step) {
        ASSERT_EQ(s_queue->Signal(s_fence, base + step), S_OK) << "step " << step;
        EXPECT_TRUE(wait_for(base + step))
            << "the fence stopped at " << s_fence->GetCompletedValue()
            << " waiting for " << (base + step);
    }

    EXPECT_EQ(g_signal_calls.load(), 4);
    SPLICE_LOGI("graphics runner: ID3D12CommandQueue::Signal, %lld through the hook, fence at %llu",
                g_signal_calls.load(),
                static_cast<unsigned long long>(s_fence->GetCompletedValue()));

    ASSERT_TRUE(outcome.disable());
    const long long settled = g_signal_calls.load();
    ASSERT_EQ(s_queue->Signal(s_fence, base + 5), S_OK);
    EXPECT_TRUE(wait_for(base + 5));
    EXPECT_EQ(g_signal_calls.load(), settled)
        << "the callback still ran after disable";
}

TEST_F(CommandQueueFixture, a_queue_method_past_the_displacement_boundary) {
    // GetTimestampFrequency is slot 16, the first slot whose thunk carries a
    // four-byte displacement. Resolving it is one thing; installing on it and
    // having the interface still answer correctly is the other half, and this
    // is the only place in the suite where that half runs against a real
    // Microsoft table.
    UINT64 reference = 0;
    const HRESULT plain = s_queue->GetTimestampFrequency(&reference);
    if (FAILED(plain)) {
        GTEST_SKIP() << "this queue does not report a timestamp frequency";
    }

    outcome = splice::Vtable<ID3D12CommandQueue>{s_queue}
        .on(&ID3D12CommandQueue::GetTimestampFrequency,
            [](auto orig, ID3D12CommandQueue* self, UINT64* frequency) {
                g_signal_calls.fetch_add(1, std::memory_order_relaxed);
                return orig(self, frequency);
            })
        .install(splice::slot_swap);
    ASSERT_EQ(outcome.state(), splice::State::installed);

    UINT64 hooked = 0;
    ASSERT_EQ(s_queue->GetTimestampFrequency(&hooked), S_OK);
    EXPECT_EQ(hooked, reference) << "the hook changed the answer";
    EXPECT_EQ(g_signal_calls.load(), 1);
}

#else   // !_WIN32

TEST(GraphicsSlots, needs_d3d) {
    GTEST_SKIP() << "D3D11, D3D12 and DXGI are Windows interfaces; the "
                    "portable runner covers the same chain elsewhere";
}

#endif
