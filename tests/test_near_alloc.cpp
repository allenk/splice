// ─── Near-allocation tests ────────────────────────────────────────────────
//
// allocate_executable_memory's near_addr argument exists so the x86_64
// patcher can keep two things inside a 32-bit displacement of its target: the
// relocated prologue, which must still reach back into the original module,
// and the relay, which is what keeps the installed patch down to 5 bytes and
// therefore atomic. See src/os/memory.h for the full argument.
//
// What is asserted here is the allocator's own contract, because that is the
// part with a deterministic answer: a process has free address space near its
// own code, so the search must find it. The consequence for the patcher -- a
// 5-byte E9 rather than a 14-byte FF 25 -- is covered by
// installed_patch_is_the_short_form below, and end-to-end against a foreign
// module by the GTG overlay probe, where the module distance is genuinely
// gigabytes rather than whatever a single test binary happens to produce.
// ───────────────────────────────────────────────────────────────────────────
#include <gtest/gtest.h>

#include <splice/splice.h>

#include "arch/x86_64/atomic_patch.h"
#include "os/memory.h"
#include "test_targets.h"   // SPLICE_TEST_NOINLINE

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>
#include <cstring>

namespace {

std::int64_t distance(const void* a, const void* b) {
    return reinterpret_cast<std::int64_t>(b) - reinterpret_cast<std::int64_t>(a);
}

std::int64_t abs64(std::int64_t v) { return v < 0 ? -v : v; }

// A target to measure against. Its address is the thing being approached, so
// it needs to be real code rather than a data object -- executable pages are
// where the interesting neighbours are.
SPLICE_TEST_NOINLINE int near_alloc_anchor(int x) { return x + 7; }

} // namespace

TEST(NearAlloc, hint_lands_within_rel32_reach_of_its_anchor) {
    void* const anchor = reinterpret_cast<void*>(&near_alloc_anchor);

    void* mem = splice::os::allocate_executable_memory(144, anchor);
    ASSERT_NE(mem, nullptr);

    const std::int64_t delta = distance(anchor, mem);
    std::printf("[near-alloc] anchor=%p block=%p delta=%lld\n",
                anchor, mem, static_cast<long long>(delta));

    // The whole point: within a signed 32-bit displacement. Asserting the
    // signed bound rather than "it is close" is deliberate -- 2 GB minus one
    // byte is a pass and 2 GB plus one is a failure, and nothing in between
    // matters.
    EXPECT_LT(abs64(delta), static_cast<std::int64_t>(0x7FFFFFFF));

    splice::os::free_executable_memory(mem, 144);
}

TEST(NearAlloc, repeated_requests_each_get_their_own_block_still_in_range) {
    void* const anchor = reinterpret_cast<void*>(&near_alloc_anchor);

    // One hook per target is the common case, but a consumer hooking a dozen
    // methods on one vtable asks a dozen times from nearly the same address.
    // The search must not keep handing back the one block it already gave out.
    void* blocks[8]{};
    for (auto& block : blocks) {
        block = splice::os::allocate_executable_memory(144, anchor);
        ASSERT_NE(block, nullptr);
        EXPECT_LT(abs64(distance(anchor, block)),
                  static_cast<std::int64_t>(0x7FFFFFFF));
    }
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t j = i + 1; j < 8; ++j) {
            EXPECT_NE(blocks[i], blocks[j]);
        }
    }
    for (auto* block : blocks) {
        splice::os::free_executable_memory(block, 144);
    }
}

TEST(NearAlloc, a_null_hint_still_allocates) {
    // The hint is optional and every pre-existing caller omits it. A
    // regression here would break the ARM64 backend, which deliberately does
    // not pass one.
    void* mem = splice::os::allocate_executable_memory(128, nullptr);
    ASSERT_NE(mem, nullptr);
    splice::os::free_executable_memory(mem, 128);
}

// The byte check below is an x86_64 encoding claim, so it is gated on the
// architecture rather than on a splice build flag -- SPLICE_HAS_X86_64_BACKEND
// is PRIVATE to the library target and invisible here.
#if defined(_M_X64) || defined(__x86_64__)

namespace {

SPLICE_TEST_NOINLINE int relay_probe_target(int x) { return x * 2; }

} // namespace

TEST(NearAlloc, installed_patch_is_the_short_form) {
    // Reads the patched prologue back and insists on `E9 rel32`, not
    // `FF 25`. Within one test binary the hook body is close to the target
    // anyway, so this does not prove the relay is what achieved it -- it
    // proves the relay did not COST it, which is the regression worth pinning
    // here. The proof that it achieves it across modules is the overlay probe
    // against dxgi.dll.
    auto& hook = SPLICE_HOOK_ADDR(&relay_probe_target)
        .onInvoke([](auto original, int x) { return original(x) + 1; });
    splice::install_all();
    (void)hook;

    const auto* bytes =
        reinterpret_cast<const std::uint8_t*>(&relay_probe_target);
    std::printf("[near-alloc] prologue after install: %02X %02X\n",
                bytes[0], bytes[1]);
    EXPECT_EQ(bytes[0], 0xE9);

    // And it still behaves: a short patch that broke the call would satisfy
    // the byte check and nothing else.
    EXPECT_EQ(relay_probe_target(10), 21);
}

// A prologue that cannot be relocated to an arbitrary address.
//
// Touching a global compiles to a RIP-relative access -- `mov eax, [rip+disp32]`
// -- and relocate_instruction has to rewrite that disp32 to keep pointing at
// the same global from the trampoline. From beyond 2 GB the new displacement
// does not fit in int32, emit_prologue_copy fails, and install_inline_patch
// refuses outright. This is not the atomicity problem; it is the other half of
// what near-allocation buys, and it presents as "that function cannot be
// hooked" rather than as a warning.
//
// Measured in this very binary: an mmap with no address hint lands ~45 TB from
// the executable's code on Linux, and VirtualAlloc with no hint landed 2.3 GB
// away on Windows. So a hint-free trampoline is out of rel32 range essentially
// always on Linux and by the loader's whim on Windows.
//
// What this particular test DOES cover is narrower than that, and saying so
// matters: at -O0 the first instructions here are `push rbp; mov rbp, rsp`, so
// the RIP-relative access is past the 5 bytes actually copied and the install
// succeeds either way. Run with the hint removed, it still passes. The range
// dependency itself is pinned directly in test_x86_64_disasm.cpp
// (relocate_rip_relative_refuses_beyond_rel32); this one is a live-fire check
// that a function which touches a global hooks and the global still works.
//
// Which also records a second effect of the min_bytes change: copying 5 bytes
// instead of 16 shrinks the window in which a rel32 or RIP-relative operand can
// fall, so it reduces exposure to the relocation limit as well as fixing the
// disable record.
int g_riprel_witness = 0;

namespace {

SPLICE_TEST_NOINLINE int riprel_prologue(int x) {
    g_riprel_witness += x;
    return g_riprel_witness;
}

} // namespace

TEST(NearAlloc, a_rip_relative_prologue_is_hookable) {
    g_riprel_witness = 0;
    ASSERT_EQ(riprel_prologue(1), 1);

    auto& hook = SPLICE_HOOK_ADDR(&riprel_prologue)
        .observe()
        .onInvoke([](auto original, int x) { return original(x) * 10; });
    splice::install_all();

    // The install itself is the assertion. If the prologue could not be
    // relocated, splice_is_hooked is false and nothing below would have run
    // through the hook at all.
    EXPECT_TRUE(splice_is_hooked(reinterpret_cast<void*>(&riprel_prologue)));

    const int result = riprel_prologue(4);
    EXPECT_EQ(hook.invocations().value_or(0), 1u);
    EXPECT_EQ(result, 50);          // (1 + 4) * 10
    EXPECT_EQ(g_riprel_witness, 5); // the relocated access still reached the global
}

// A second inline patch on an address we already patched is refused.
//
// This pins a refusal that stands in for what used to be an accident, and the
// history is the point of the comment.
//
// Before the prologue walk was shortened to the patch length, a second install
// here failed inside emit_prologue_copy: walking 16 bytes over a patched short
// function ran off the end and found something unrelocatable. Nobody designed
// that, but the outcome -- "re-hooking quietly does not work" -- was safe.
//
// Shortening the walk to 5 bytes removed the barrier, and what got through was
// not a working chain. The second trampoline copies the installed `E9 rel32` and
// relocates it, which points it back at the FIRST hook's relay, so hook one's
// `original` re-enters hook one. Measured outcome: infinite recursion, exit code
// 0xC00000FD, stack overflow.
//
// Two separate defects surfaced on the way here, both older than the relay:
//
//   1. unwrap_jump_stub followed Splice's own patch -- `patched --E9--> relay
//      --FF25--> hook stub` -- and treated whatever the stub began with as "the
//      real function", then patched an unrelated address. Now guarded by a
//      registry of live patch sites, because a byte test cannot tell a compiler
//      thunk from our own jump.
//   2. install_all() re-runs installers that already ran, so a second
//      install_all() re-patches an address even with one hook entry.
//
// Chaining onto an existing patch is genuinely wanted -- a player's machine may
// already have Steam Overlay in the same slot -- but it has to follow the
// existing jump to find the true original, and that needs designing rather than
// inheriting. Until then, refusing is the honest answer.
TEST(NearAlloc, a_second_install_on_the_same_address_is_refused) {
    static int base_calls = 0;
    base_calls = 0;

    struct Target {
        SPLICE_TEST_NOINLINE static int call(int x) {
            ++base_calls;
            return x + 1;
        }
    };

    ASSERT_EQ(Target::call(10), 11);
    ASSERT_EQ(base_calls, 1);

    auto& first = SPLICE_HOOK_ADDR(&Target::call)
        .observe()
        .onInvoke([](auto original, int x) { return original(x) * 2; });
    splice::install_all();
    ASSERT_TRUE(splice_is_hooked(reinterpret_cast<void*>(&Target::call)));
    ASSERT_EQ(Target::call(10), 22);          // (10 + 1) * 2
    const auto first_calls_before = first.invocations().value_or(0);

    auto& second = SPLICE_HOOK_ADDR(&Target::call)
        .observe()
        .onInvoke([](auto original, int x) { return original(x) + 100; });
    splice::install_all();

    // The refusal is not an exception and not a crash; the second entry simply
    // never becomes live. The first hook keeps working exactly as before, which
    // is the property that matters: a rejected install must not damage the one
    // already there.
    const int result = Target::call(10);
    std::printf("[near-alloc] after a refused second install: result=%d "
                "first=%llu second=%llu base=%d\n",
                result,
                static_cast<unsigned long long>(first.invocations().value_or(0)),
                static_cast<unsigned long long>(second.invocations().value_or(0)),
                base_calls);

    EXPECT_EQ(result, 22) << "the first hook must still be the one in effect";
    EXPECT_EQ(second.invocations().value_or(0), 0u)
        << "the second hook must never have been entered";
    EXPECT_EQ(first.invocations().value_or(0), first_calls_before + 1);
    EXPECT_EQ(base_calls, 3);   // one unhooked, then one per hooked call
}

// Disable works on a prologue that used to be too long to record.
//
// This is the end-to-end proof for the second half of the near-allocation work,
// and it is worth stating what makes it a proof rather than a demonstration.
//
// splice_disable restores the original prologue from a 16-byte record captured
// at install time. atomic_disable_inline rejects len == 0 or len > 16 outright,
// so if the record did not fit there is nothing to restore from and disable is
// simply unavailable -- reported at install with
// "copy_size=N > 16; disable will be unavailable".
//
// riprel_prologue above is a real, ordinary function, and under the old rule
// -- walk until 16 bytes are covered, because a 14-byte patch needs 16 -- it
// measured **17**. Measured, not supposed: running this binary with the walk
// reverted logs `calculate_copy_size: 17 (asked for 16)` followed by
// `copy_size=17 > 16; disable will be unavailable`. So disable was unavailable
// on it, and the round trip below could not have passed.
//
// The relay is what licenses the shorter walk: the installed patch is 5 bytes,
// so 5 is all that has to be moved aside. The record then measures 5 to 8, and
// at <= 8 bytes atomic_disable_inline restores it with a single aligned 8-byte
// store and nothing else -- no non-atomic write touches the live function at
// all.
TEST(NearAlloc, disable_round_trip_on_a_prologue_that_used_to_overflow) {
    g_riprel_witness = 0;
    ASSERT_EQ(riprel_prologue(3), 3);

    auto& entry = SPLICE_HOOK_ADDR_STATIC(&riprel_prologue)
        .onInvoke([](auto original, int x) { return original(x) + 1000; });
    splice::install_all();
    ASSERT_TRUE(entry.is_installed());

    g_riprel_witness = 0;
    EXPECT_EQ(riprel_prologue(5), 1005);      // 5 + 1000

    // The part that was impossible before: there is a record to restore from.
    ASSERT_TRUE(entry.disable())
        << "disable was rejected -- the prologue record did not fit in 16 bytes";
    EXPECT_FALSE(entry.is_installed());

    // Back to the original body, and the relocated global access still works.
    g_riprel_witness = 0;
    EXPECT_EQ(riprel_prologue(7), 7);
    EXPECT_EQ(g_riprel_witness, 7);

    // And the prologue really is the function's own again, not a jump.
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&riprel_prologue);
    std::printf("[near-alloc] prologue after disable: %02X %02X %02X\n",
                bytes[0], bytes[1], bytes[2]);
    EXPECT_NE(bytes[0], 0xE9) << "still a relative jump after disable";
    EXPECT_FALSE(bytes[0] == 0xFF && bytes[1] == 0x25)
        << "still an absolute jump after disable";
}

// The atomic patch only needs the PATCH inside one aligned quadword.
//
// Splice used to require `target & 7 == 0` and fall back to a non-atomic memcpy
// otherwise. That is stricter than the hardware rule it was standing in for:
// Intel SDM Vol 3A 8.1.1 promises single-copy atomicity for an aligned
// quadword, and what has to fit inside one is the five patch bytes, not the
// function entry. So offsets 0 through 3 are fine and only 4 through 7 straddle.
//
// Not a micro-optimisation. MSVC aligns function entries to 16 bytes so Windows
// never noticed, but GCC at -O0 packs small functions tightly: in Splice's own
// Linux suite only 4 of 13 distinct hook targets were 8-byte aligned, so 10 of
// 14 installs took the torn-state path purely because of this precondition.
// After the change the same suite reports 10 atomic and 4 straddling, at
// offsets 0, 1, 2 and 3.
//
// Done on a data buffer rather than on real code because the point is the byte
// arithmetic at every offset, and a controlled buffer is the only way to visit
// all eight. Nothing here is executed.
TEST(NearAlloc, atomic_patch_needs_the_patch_aligned_not_the_function) {
    alignas(8) static std::uint8_t buf[64];
    void* const dest = reinterpret_cast<void*>(&riprel_prologue);

    for (unsigned k = 0; k < 8; ++k) {
        std::memset(buf, 0xCC, sizeof(buf));
        std::uint8_t* const target = buf + 16 + k;   // 16 is 8-aligned, so &7 == k
        ASSERT_EQ(reinterpret_cast<std::uintptr_t>(target) & 7u, k);

        const bool ok = splice::arch::x86_64::atomic_install_jmp_rel32(target, dest);
        std::printf("[near-alloc] offset=%u -> %s\n", k, ok ? "atomic" : "straddles");

        if (k <= 3) {
            EXPECT_TRUE(ok) << "offset " << k << " leaves " << (8 - k)
                            << " bytes in the quadword, enough for 5";
        } else {
            EXPECT_FALSE(ok) << "offset " << k << " leaves only " << (8 - k)
                             << " bytes; a 5-byte patch must be refused";
            // A refused install must not have written anything.
            for (std::size_t i = 0; i < sizeof(buf); ++i) {
                ASSERT_EQ(buf[i], 0xCC) << "byte " << i << " touched by a refusal";
            }
            continue;
        }

        // The patch is there ...
        EXPECT_EQ(target[0], 0xE9);
        std::int32_t rel32 = 0;
        std::memcpy(&rel32, target + 1, 4);
        EXPECT_EQ(reinterpret_cast<std::uint8_t*>(target) + 5 + rel32,
                  reinterpret_cast<std::uint8_t*>(dest))
            << "the relative displacement does not resolve to the destination";

        // ... and nothing else moved. This is the assertion that matters: the
        // read-modify-write shares its quadword with bytes that are not ours --
        // padding, or a tightly packed neighbour Splice may be patching at the
        // same moment -- and they have to come back unchanged.
        for (std::size_t i = 0; i < sizeof(buf); ++i) {
            const bool in_patch = (buf + i >= target) && (buf + i < target + 5);
            if (in_patch) continue;
            ASSERT_EQ(buf[i], 0xCC)
                << "byte " << i << " outside the patch was modified (offset "
                << k << ")";
        }

        // And the restore puts it back, at the same offset.
        const std::uint8_t saved[5] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
        EXPECT_TRUE(splice::arch::x86_64::atomic_disable_inline(target, saved, 5));
        for (std::size_t i = 0; i < sizeof(buf); ++i) {
            ASSERT_EQ(buf[i], 0xCC)
                << "byte " << i << " not restored (offset " << k << ")";
        }
    }
}

// Disable, then install again. Nothing tested this, and it now has to work
// through a check that did not exist before.
//
// `InterceptorEntry::disable()` documents the hook as "re-installable via
// install()", and install() is private -- the route a consumer actually takes is
// another `splice::install_all()`, which re-runs the registered installer.
//
// That route goes straight through the refusal added for stacked patches. If
// `splice_disable` did not tell the engine to forget the site, the re-install
// would be rejected as a second patch on an already-patched address and a
// documented capability would have quietly stopped working. This test is here
// because that is a plausible way to break something while fixing something
// else.
TEST(NearAlloc, a_disabled_hook_can_be_installed_again) {
    static int calls = 0;
    calls = 0;

    struct Target {
        SPLICE_TEST_NOINLINE static int call(int x) {
            ++calls;
            return x + 1;
        }
    };

    void* const addr = reinterpret_cast<void*>(&Target::call);
    ASSERT_EQ(Target::call(1), 2);

    auto& entry = SPLICE_HOOK_ADDR_STATIC(&Target::call)
        .observe()
        .onInvoke([](auto original, int x) { return original(x) * 10; });

    splice::install_all();
    ASSERT_TRUE(entry.is_installed());
    ASSERT_TRUE(splice_is_hooked(addr));
    EXPECT_EQ(Target::call(1), 20);

    ASSERT_TRUE(entry.disable());
    EXPECT_FALSE(entry.is_installed());
    EXPECT_EQ(Target::call(1), 2) << "disable did not restore the original";
    const auto calls_at_disable = entry.invocations().value_or(0);

    // The re-install. Before the site-forgetting in splice_disable this is the
    // call that would have been refused.
    splice::install_all();
    EXPECT_TRUE(entry.is_installed()) << "re-install after disable was rejected";
    EXPECT_TRUE(splice_is_hooked(addr));
    EXPECT_EQ(Target::call(1), 20) << "the hook is installed but not in effect";
    EXPECT_EQ(entry.invocations().value_or(0), calls_at_disable + 1);

    // Leave the target clean for anything else in this binary.
    EXPECT_TRUE(entry.disable());
    EXPECT_EQ(Target::call(1), 2);
}

// V-1 — a target that is genuinely out of rel32 range, inside ctest.
//
// This is the test whose absence let three of the 2026-09-28 defects survive
// 142 passing runs. Every other test in this repository hooks a function in its
// own binary, where the target is always within +/-2 GB of the callback, always
// relocatable, and always aligned by the compiler. That single condition hides
// every distance problem there is.
//
// No second module is needed to fix that. Put a function somewhere far away and
// hook it there.
//
// What passing proves, and none of it is provable in-binary:
//
//   * the patch is still FIVE bytes with the callback out of range. Without a
//     relay it could not be: an unreachable destination forces the 14-byte
//     `FF 25` form, which is the non-atomic path. A 0xE9 here is the relay's
//     signature and nothing else can produce it.
//   * the prologue was relocated across that distance -- calling through the
//     hook reaches the original, so emit_prologue_copy did not refuse.
//   * the disable record still fits, so splice_disable round-trips at distance.
//
// What it deliberately does NOT do is assert the trampoline's address or the
// relay's offset. Those are internal, and a test that pins them would break on
// every layout change while proving nothing extra -- the five bytes already say
// the relay ran.

namespace {

// A tiny function as machine code, so it can be placed at an address of our
// choosing rather than wherever the linker felt like.
//
//   lea eax, [arg + 1]     3 bytes, register-relative, so nothing to relocate
//   nop nop nop            padding, so the 5-byte walk lands on a boundary
//   ret
//
// The argument register is the only ABI difference: RCX on Windows x64, RDI on
// System V. Three nops rather than none because calculate_copy_size(target, 5)
// rounds up to whole instructions, and walking off the end of a 4-byte function
// would decode whatever follows it.
#if defined(_WIN32)
constexpr std::uint8_t kFarStub[] = {0x8D, 0x41, 0x01,  // lea eax, [rcx+1]
                                     0x90, 0x90, 0x90,  // nop x3
                                     0xC3};             // ret
#else
constexpr std::uint8_t kFarStub[] = {0x8D, 0x47, 0x01,  // lea eax, [rdi+1]
                                     0x90, 0x90, 0x90,  // nop x3
                                     0xC3};             // ret
#endif

using FarFn = int (*)(int);

std::int64_t distance_from(const void* a, const void* b) {
    return reinterpret_cast<std::int64_t>(b) - reinterpret_cast<std::int64_t>(a);
}

// Ask the near-allocator for a block near an address that is deliberately far
// from us. Reuses the machinery under test rather than hand-rolling a second
// allocator, and the far hint is what makes the result far.
void* allocate_far_from(const void* here, std::size_t bytes) {
    constexpr std::int64_t kSixteenGB = 16LL * 1024 * 1024 * 1024;
    const auto origin = reinterpret_cast<std::int64_t>(here);

    for (const std::int64_t offset : {-kSixteenGB, kSixteenGB,
                                      -4 * kSixteenGB, 4 * kSixteenGB}) {
        const std::int64_t hint = origin + offset;
        if (hint < 0x100000) continue;   // below the lowest usable address
        void* mem = splice::os::allocate_executable_memory(
            bytes, reinterpret_cast<const void*>(hint));
        if (mem == nullptr) continue;
        if (abs64(distance_from(here, mem)) > 0x7FFFFFFF) return mem;
        // Landed in range after all -- no use for this test, hand it back.
        splice::os::free_executable_memory(mem, bytes);
    }
    return nullptr;
}

int g_far_hook_calls = 0;

}  // namespace

TEST(NearAlloc, a_target_out_of_rel32_range_installs_atomically_and_works) {
    void* const here = reinterpret_cast<void*>(&riprel_prologue);

    void* const page = allocate_far_from(here, 4096);
    if (page == nullptr) {
        GTEST_SKIP() << "could not obtain an executable page more than 2 GB "
                        "from this module; the address space did not allow it";
    }

    std::memcpy(page, kFarStub, sizeof(kFarStub));
    splice::os::flush_instruction_cache(page, sizeof(kFarStub));

    auto* const far_fn = reinterpret_cast<FarFn>(page);
    const std::int64_t delta = distance_from(here, page);
    std::printf("[near-alloc] far target at %p, %+.2f GB from this module\n",
                page, static_cast<double>(delta) / (1024.0 * 1024.0 * 1024.0));

    ASSERT_GT(abs64(delta), static_cast<std::int64_t>(0x7FFFFFFF))
        << "the point of this test is that the target is out of range";
    ASSERT_EQ(far_fn(41), 42) << "the hand-assembled stub does not behave";

    g_far_hook_calls = 0;
    auto& entry = SPLICE_HOOK_ADDR_STATIC(far_fn)
        .observe()
        .onInvoke([](auto original, int x) {
            ++g_far_hook_calls;
            return original(x) * 100;
        });
    splice::install_all();

    ASSERT_TRUE(entry.is_installed())
        << "install refused at distance -- prologue relocation or placement";

    // The whole point. Five bytes, with the callback out of rel32 range, is
    // only reachable through the relay; a direct jump would have had to be the
    // 14-byte FF 25 form.
    const auto* bytes = static_cast<const std::uint8_t*>(page);
    std::printf("[near-alloc] patched prologue: %02X %02X %02X\n",
                bytes[0], bytes[1], bytes[2]);
    EXPECT_EQ(bytes[0], 0xE9)
        << "a far callback produced a long patch -- the relay did not run";

    // And the original is still reachable through it, which is what says the
    // prologue was relocated across the distance rather than refused.
    EXPECT_EQ(far_fn(41), 4200);          // (41 + 1) * 100
    EXPECT_EQ(g_far_hook_calls, 1);
    EXPECT_EQ(entry.invocations().value_or(0), 1u);

    // The disable record fits at distance too.
    ASSERT_TRUE(entry.disable()) << "disable unavailable for a far target";
    EXPECT_EQ(far_fn(41), 42) << "disable did not restore the far prologue";
    EXPECT_EQ(g_far_hook_calls, 1);

    // The page stays mapped: splice keeps a trampoline pointing into it and
    // FR-013 is explicit that trampolines are never reclaimed.
}

// A const function pointer is still a function pointer.
//
// `auto* const p = reinterpret_cast<Fn>(addr);` is the natural way to write a
// hook target resolved at run time, and it used to fail to compile: decltype
// keeps the const, and the InterceptorEntry specialisations were written
// without it, so the error named an undefined template rather than the cause.
//
// Found while writing the far-target test above, which is a real instance of
// exactly that pattern. Second time this repository has hit the shape -- the
// 2026-06-13 round fixed the same thing for `noexcept`, which C++17 also made
// part of the type.
TEST(NearAlloc, a_cv_qualified_function_pointer_can_be_hooked) {
    static int calls = 0;
    calls = 0;

    struct Target {
        SPLICE_TEST_NOINLINE static int call(int x) {
            ++calls;
            return x + 5;
        }
    };

    auto* const fn = &Target::call;          // int (*const)(int)
    auto& const_ref = fn;                    // int (*const&)(int)

    ASSERT_EQ(fn(1), 6);

    auto& entry = SPLICE_HOOK_ADDR_STATIC(const_ref)
        .observe()
        .onInvoke([](auto original, int x) { return original(x) * 3; });
    splice::install_all();

    ASSERT_TRUE(entry.is_installed());
    EXPECT_EQ(fn(1), 18);                    // (1 + 5) * 3
    EXPECT_EQ(entry.invocations().value_or(0), 1u);
    EXPECT_TRUE(entry.disable());
    EXPECT_EQ(fn(1), 6);
}

// V-3 — unwrap_jump_stub must still follow a real jump thunk.
//
// The patch-site registry added for defect 4 stops unwrap_jump_stub at an
// address Splice patched. It must NOT stop it at a compiler-generated thunk,
// which is the entire reason that function exists -- and nothing checked that.
//
// The failure mode of over-blocking is worth stating because it is not obvious:
// Splice would hook the THUNK instead of the function. Calls that go through
// the thunk would be intercepted and calls that reach the function by any other
// route would not, silently. That is the same defect the registry was written
// to prevent, arrived at from the opposite side.
//
// The thunks here are built by hand rather than coaxed out of the linker.
// MSVC's incremental linking produces ILT stubs, but whether it does depends on
// the build configuration, so a test that relies on it passes or vanishes
// depending on how it was compiled. These bytes are what the linker emits, and
// they are the same bytes on both platforms.

namespace {

int v3_real_target(int x) { return x + 11; }

// `E9 rel32` -- an MSVC ILT stub, or any linker-generated jump thunk.
void* make_rel32_thunk(void* page, const void* dest) {
    auto* p = static_cast<std::uint8_t*>(page);
    const std::int64_t disp = reinterpret_cast<std::int64_t>(dest) -
                              (reinterpret_cast<std::int64_t>(p) + 5);
    if (disp < -0x80000000LL || disp > 0x7FFFFFFFLL) return nullptr;
    p[0] = 0xE9;
    const auto d32 = static_cast<std::int32_t>(disp);
    std::memcpy(p + 1, &d32, 4);
    return p;
}

// `FF 25 disp32` + absolute address -- an import thunk dereferencing its IAT
// slot. disp32 is 0, so the slot is the eight bytes that follow.
void* make_abs64_thunk(void* page, const void* dest) {
    auto* p = static_cast<std::uint8_t*>(page);
    p[0] = 0xFF;
    p[1] = 0x25;
    std::memset(p + 2, 0, 4);
    const auto addr = reinterpret_cast<std::uint64_t>(dest);
    std::memcpy(p + 6, &addr, 8);
    return p;
}

std::int32_t rel32_destination_offset(const void* thunk) {
    std::int32_t d32 = 0;
    std::memcpy(&d32, static_cast<const std::uint8_t*>(thunk) + 1, 4);
    return d32;
}

int g_v3_hook_calls = 0;

}  // namespace

TEST(NearAlloc, hooking_through_a_rel32_thunk_patches_the_function_behind_it) {
    void* const real = reinterpret_cast<void*>(&v3_real_target);
    void* const page = splice::os::allocate_executable_memory(4096, real);
    ASSERT_NE(page, nullptr);

    void* const thunk = make_rel32_thunk(page, real);
    ASSERT_NE(thunk, nullptr) << "the page landed out of rel32 range of the "
                                 "function, so no thunk could reach it";

    using Fn = int (*)(int);
    auto* const via_thunk = reinterpret_cast<Fn>(thunk);
    ASSERT_EQ(via_thunk(1), 12) << "the hand-built thunk does not reach";

    g_v3_hook_calls = 0;
    auto& entry = SPLICE_HOOK_ADDR_STATIC(via_thunk)
        .onInvoke([](auto original, int x) {
            ++g_v3_hook_calls;
            return original(x) * 2;
        });
    splice::install_all();
    ASSERT_TRUE(entry.is_installed());

    // The assertion that matters. Splice must have unwrapped the thunk and
    // patched the function behind it -- so a call that never touches the thunk
    // is intercepted too.
    EXPECT_TRUE(splice_is_hooked(real))
        << "the real function was not patched -- unwrap_jump_stub stopped at "
           "the thunk, which is the over-blocking failure V-3 exists to catch";
    EXPECT_EQ(v3_real_target(1), 24) << "(1 + 11) * 2";   // direct, no thunk
    EXPECT_EQ(g_v3_hook_calls, 1);

    // ... and the thunk itself was left alone: still one jump, still aimed at
    // the same place.
    const auto* thunk_bytes = static_cast<const std::uint8_t*>(thunk);
    EXPECT_EQ(thunk_bytes[0], 0xE9) << "the thunk was overwritten";
    EXPECT_EQ(static_cast<const std::uint8_t*>(thunk) + 5 +
                  rel32_destination_offset(thunk),
              static_cast<const std::uint8_t*>(real))
        << "the thunk was re-aimed somewhere else";

    // Going through it still works, because it lands on the patched function.
    EXPECT_EQ(via_thunk(1), 24);
    EXPECT_EQ(g_v3_hook_calls, 2);

    ASSERT_TRUE(entry.disable());
    EXPECT_EQ(v3_real_target(1), 12);
    EXPECT_EQ(via_thunk(1), 12);
    // Deliberately NOT freed. A SPLICE_HOOK_ADDR_STATIC entry is static: it
    // outlives this test and stays registered against this address, and
    // install_all() re-runs every live installer on every later call. Freeing
    // the page lets a later test be handed the same address, at which point the
    // stale entry unwraps through whatever now lives there and patches it --
    // which is exactly what happened when these three tests were first run
    // together, and it is a use-after-free in all but name. 4 KB per test, in a
    // process that exits in seconds, is the right trade. See S-12.
}

TEST(NearAlloc, hooking_through_an_abs64_thunk_patches_the_function_behind_it) {
    // Same property for the import-thunk shape, which unwrap_jump_stub also
    // follows and which the registry guard therefore also has to leave alone.
    static int calls = 0;
    calls = 0;

    struct Target {
        SPLICE_TEST_NOINLINE static int call(int x) { return x + 23; }
    };
    void* const real = reinterpret_cast<void*>(&Target::call);
    void* const page = splice::os::allocate_executable_memory(4096, real);
    ASSERT_NE(page, nullptr);

    using Fn = int (*)(int);
    auto* const via_thunk = reinterpret_cast<Fn>(make_abs64_thunk(page, real));
    ASSERT_EQ(via_thunk(1), 24);

    auto& entry = SPLICE_HOOK_ADDR_STATIC(via_thunk)
        .onInvoke([](auto original, int x) {
            ++calls;
            return original(x) * 10;
        });
    splice::install_all();
    ASSERT_TRUE(entry.is_installed());

    EXPECT_TRUE(splice_is_hooked(real))
        << "unwrap_jump_stub did not follow the FF 25 form to the function";
    EXPECT_EQ(Target::call(1), 240) << "(1 + 23) * 10";
    EXPECT_EQ(calls, 1);

    // The thunk is untouched: still FF 25, still pointing at the same address.
    const auto* b = static_cast<const std::uint8_t*>(page);
    EXPECT_EQ(b[0], 0xFF);
    EXPECT_EQ(b[1], 0x25);
    std::uint64_t slot = 0;
    std::memcpy(&slot, b + 6, 8);
    EXPECT_EQ(slot, reinterpret_cast<std::uint64_t>(real));

    ASSERT_TRUE(entry.disable());
    EXPECT_EQ(Target::call(1), 24);
    // Deliberately NOT freed. A SPLICE_HOOK_ADDR_STATIC entry is static: it
    // outlives this test and stays registered against this address, and
    // install_all() re-runs every live installer on every later call. Freeing
    // the page lets a later test be handed the same address, at which point the
    // stale entry unwraps through whatever now lives there and patches it --
    // which is exactly what happened when these three tests were first run
    // together, and it is a use-after-free in all but name. 4 KB per test, in a
    // process that exits in seconds, is the right trade. See S-12.
}

TEST(NearAlloc, unwrapping_stops_at_our_own_patch_and_the_second_hook_is_refused) {
    // The two halves together, which is where over-blocking and under-blocking
    // meet: the thunk must be followed, and the patch at the end of it must not.
    //
    // Once the function behind a thunk is patched, its first byte is 0xE9 --
    // indistinguishable from the thunk that led here. Following it would walk
    // into Splice's own relay and hook stub and then patch whatever they begin
    // with, which is the crash recorded as defect 4. The registry stops it, and
    // the second install is refused rather than mis-aimed.
    static int calls = 0;
    calls = 0;

    struct Target {
        SPLICE_TEST_NOINLINE static int call(int x) { return x + 7; }
    };
    void* const real = reinterpret_cast<void*>(&Target::call);
    void* const page = splice::os::allocate_executable_memory(4096, real);
    ASSERT_NE(page, nullptr);

    using Fn = int (*)(int);
    auto* const via_thunk = reinterpret_cast<Fn>(make_rel32_thunk(page, real));
    ASSERT_NE(via_thunk, nullptr);

    auto& first = SPLICE_HOOK_ADDR_STATIC(via_thunk)
        .onInvoke([](auto original, int x) { ++calls; return original(x) + 100; });
    splice::install_all();
    ASSERT_TRUE(first.is_installed());
    ASSERT_EQ(Target::call(1), 108);              // (1 + 7) + 100

    // Now the function itself starts with 0xE9. A second hook, again through
    // the thunk, must unwrap to the function and stop there.
    auto& second = SPLICE_HOOK_ADDR_STATIC(via_thunk)
        .observe()
        .onInvoke([](auto original, int x) { return original(x) + 1000; });
    splice::install_all();

    EXPECT_FALSE(second.is_installed())
        << "a second hook on an already-patched function was not refused";
    EXPECT_EQ(second.invocations().value_or(0), 0u);
    EXPECT_EQ(Target::call(1), 108) << "the first hook stopped working";

    ASSERT_TRUE(first.disable());
    EXPECT_EQ(Target::call(1), 8);
    // Deliberately NOT freed. A SPLICE_HOOK_ADDR_STATIC entry is static: it
    // outlives this test and stays registered against this address, and
    // install_all() re-runs every live installer on every later call. Freeing
    // the page lets a later test be handed the same address, at which point the
    // stale entry unwraps through whatever now lives there and patches it --
    // which is exactly what happened when these three tests were first run
    // together, and it is a use-after-free in all but name. 4 KB per test, in a
    // process that exits in seconds, is the right trade. See S-12.
}

// ─── V-4: the paths only concurrency reaches ───────────────────────────────

// Why atomic_overlay is a compare-exchange and not a store.
//
// It read-modify-writes the whole quadword containing the patch, and the bytes
// it is NOT changing do not all belong to it: below `offset` they are whatever
// precedes the function, above `offset + len` they are the function's own
// continuation. A plain read-then-store would silently revert a concurrent
// change to any of them.
//
// That is not hypothetical. A 5-byte patch at offset 0 covers bytes 0-4, and a
// neighbour packed into the same quadword starts at 5, 6 or 7 -- exactly the
// offsets where the atomic path refuses and the non-atomic memcpy is used
// instead. So one thread can be doing an atomic RMW of the quadword while
// another memcpys into its upper bytes, which is what this hammers.
//
// With a plain store this fails: the writer loses whatever the other thread put
// in bytes 5-7 between the read and the store. With the CAS it retries and
// re-overlays onto the newer value, so both survive. Run for enough iterations
// that the window is actually hit.
TEST(NearAlloc, the_overlay_cas_does_not_revert_a_neighbour) {
    alignas(8) static std::uint8_t arena[64];
    std::memset(arena, 0xCC, sizeof(arena));

    std::uint8_t* const target = arena + 16;          // offset 0 in its quadword
    std::uint8_t* const neighbour = target + 5;       // bytes 5..7 of the same word
    void* const dest = reinterpret_cast<void*>(&riprel_prologue);

    constexpr int kRounds = 20000;
    std::atomic<bool> go{false};
    std::atomic<int> neighbour_lost{0};
    std::atomic<int> patch_lost{0};

    std::thread patcher([&] {
        while (!go.load(std::memory_order_acquire)) { /* spin */ }
        for (int i = 0; i < kRounds; ++i) {
            splice::arch::x86_64::atomic_install_jmp_rel32(target, dest);
            if (target[0] != 0xE9) patch_lost.fetch_add(1);
        }
    });

    std::thread writer([&] {
        while (!go.load(std::memory_order_acquire)) { /* spin */ }
        for (int i = 0; i < kRounds; ++i) {
            // Stands in for a neighbour being patched through the non-atomic
            // path: three bytes inside the same quadword, written plainly.
            neighbour[0] = 0xA1;
            neighbour[1] = 0xA2;
            neighbour[2] = 0xA3;
            if (neighbour[0] != 0xA1 || neighbour[1] != 0xA2 ||
                neighbour[2] != 0xA3) {
                neighbour_lost.fetch_add(1);
            }
        }
    });

    go.store(true, std::memory_order_release);
    patcher.join();
    writer.join();

    std::printf("[near-alloc] %d rounds: neighbour reverted %d time(s), "
                "patch reverted %d time(s)\n",
                kRounds, neighbour_lost.load(), patch_lost.load());

    EXPECT_EQ(patch_lost.load(), 0) << "the patch was lost under contention";
    EXPECT_EQ(neighbour_lost.load(), 0)
        << "the overlay reverted a neighbour's bytes -- a plain store would do "
           "exactly this, which is why it is a compare-exchange";

    // Both survive together at the end, not just individually during the run.
    EXPECT_EQ(target[0], 0xE9);
    EXPECT_EQ(neighbour[0], 0xA1);
    EXPECT_EQ(neighbour[1], 0xA2);
    EXPECT_EQ(neighbour[2], 0xA3);

    // Nothing outside the quadword moved.
    for (std::size_t i = 0; i < 16; ++i) EXPECT_EQ(arena[i], 0xCC) << "at " << i;
    for (std::size_t i = 24; i < sizeof(arena); ++i) {
        EXPECT_EQ(arena[i], 0xCC) << "at " << i;
    }
}

namespace {

int v4_cycle_target(int x) { return x * 2; }
std::atomic<int> g_v4_cycle_calls{0};

}  // namespace

// Install and disable repeatedly, checking behaviour every round.
//
// FR-011 T-011-03 asks for 10 000 cycles; this runs 300. The reason is S-10 --
// each install allocates a trampoline that is never reclaimed, so 10 000 rounds
// retains roughly 40 MB and would look like a leak to any tool watching, because
// it is one. 300 rounds costs about 1.2 MB, exercises the same state machine,
// and keeps the number honest rather than aspirational.
//
// What it actually pins: that install -> disable leaves the target byte-identical
// to how it started, every time, so the record and the restore agree over and
// over rather than once.
TEST(NearAlloc, install_and_disable_cycle_without_drifting) {
    std::uint8_t original[16];
    std::memcpy(original, reinterpret_cast<const void*>(&v4_cycle_target), 16);

    constexpr int kCycles = 300;
    int mismatches = 0;
    int failures = 0;

    for (int i = 0; i < kCycles; ++i) {
        g_v4_cycle_calls.store(0);
        auto& entry = SPLICE_HOOK_ADDR(&v4_cycle_target)
            .onInvoke([](auto original_fn, int x) {
                g_v4_cycle_calls.fetch_add(1);
                return original_fn(x) + 1;
            });
        splice::install_all();

        if (!entry.is_installed()) { ++failures; break; }
        if (v4_cycle_target(3) != 7) ++mismatches;        // 3*2 + 1
        if (g_v4_cycle_calls.load() != 1) ++mismatches;

        if (!entry.disable()) { ++failures; break; }
        if (v4_cycle_target(3) != 6) ++mismatches;        // back to 3*2

        std::uint8_t now[16];
        std::memcpy(now, reinterpret_cast<const void*>(&v4_cycle_target), 16);
        if (std::memcmp(original, now, sizeof(now)) != 0) ++mismatches;
    }

    std::printf("[near-alloc] %d install/disable cycles: %d failure(s), "
                "%d mismatch(es)\n",
                kCycles, failures, mismatches);
    EXPECT_EQ(failures, 0);
    EXPECT_EQ(mismatches, 0)
        << "the target drifted across cycles -- the restore and the record "
           "disagree somewhere";
}

namespace {

int v4_contended_target(int x) { return x + 31; }

}  // namespace

// Many threads calling install_all() at once, against one entry.
//
// install_all() copies the live installer list under a shared lock and then runs
// every installer, so N threads means N attempts at the same address. Exactly
// one may patch; the rest must take the "already routed here" path and return
// the existing site rather than patching on top of it. Anything else is either a
// second patch (the recursion of defect 5) or a spurious failure.
TEST(NearAlloc, concurrent_install_all_patches_once_and_never_twice) {
    auto& entry = SPLICE_HOOK_ADDR_STATIC(&v4_contended_target)
        .observe()
        .onInvoke([](auto original, int x) { return original(x) * 2; });

    constexpr int kThreads = 8;
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) { /* spin */ }
            for (int n = 0; n < 200; ++n) splice::install_all();
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();

    EXPECT_TRUE(entry.is_installed());
    EXPECT_TRUE(splice_is_hooked(reinterpret_cast<void*>(&v4_contended_target)));

    // One patch, not a stack of them. If a second had landed, the first hook's
    // original would re-enter the first hook and this would not return.
    const auto before = entry.invocations().value_or(0);
    EXPECT_EQ(v4_contended_target(4), 70);          // (4 + 31) * 2
    EXPECT_EQ(entry.invocations().value_or(0), before + 1)
        << "the target was entered more than once per call -- patches stacked";

    ASSERT_TRUE(entry.disable());
    EXPECT_EQ(v4_contended_target(4), 35);
}

#endif  // x86_64
