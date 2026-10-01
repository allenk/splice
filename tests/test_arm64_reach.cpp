// ─── ARM64: can a trampoline be reached from the prologue it copies? ──────
//
// Measure before changing anything, because the measurement decides how much
// the work is worth: a near-allocation hint only helps if the nearest free
// page is actually within a copied instruction's reach. This is that
// measurement, written as a test rather than as a loose C file so it is
// reproducible through the existing `scripts/adb-run-tests.sh` flow and so it
// keeps guarding the fix afterwards.
//
// What is at stake: ARM64's `calculate_copy_size` always copies the first four
// instructions, and `emit_prologue_copy` must then relocate each one. The budgets
// have no slack and two of them have no fallback:
//
//     B / BL    +/-128 MB   fallback: yes, an indirect branch
//     ADRP      +/-4 GB     fallback: NONE, install refused
//     ADR       +/-1 MB     fallback: NONE, install refused
//
// So the distance from the hook target to wherever the trampoline was allocated
// is not a detail; it decides whether a function can be hooked at all. Measured
// over real prologues, 18.8 % of the 2 685 functions in the NDK's
// libc++_shared.so have an ADRP or ADR in their first four instructions.
//
// These tests run only on ARM64, and the reach tests need a real device. On any
// other architecture they report what they would have measured and pass, because
// a test that fails for being on the wrong host teaches nobody anything.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>

#include <splice/splice.h>

#include <os/memory.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#   include <arch/arm64/disasm.h>
#endif

#include "test_targets.h"

#if defined(__aarch64__) || defined(_M_ARM64)
#   define SPLICE_TEST_ARM64 1
#else
#   define SPLICE_TEST_ARM64 0
#endif

#if !defined(_WIN32)
#   include <dlfcn.h>
#endif

namespace {

// The three budgets, named rather than repeated as literals.
constexpr std::int64_t kAdrReach  = 1LL << 20;    // +/-1 MB
constexpr std::int64_t kBReach    = 128LL << 20;  // +/-128 MB
constexpr std::int64_t kAdrpReach = 4LL << 30;    // +/-4 GB

std::int64_t distance(const void* from, const void* to) {
    const auto a = reinterpret_cast<std::uintptr_t>(from);
    const auto b = reinterpret_cast<std::uintptr_t>(to);
    return b >= a ? static_cast<std::int64_t>(b - a)
                  : -static_cast<std::int64_t>(a - b);
}

std::int64_t magnitude(std::int64_t d) { return d < 0 ? -d : d; }

void report(const char* what, std::int64_t d) {
    const double mb = static_cast<double>(magnitude(d)) / (1024.0 * 1024.0);
    std::printf("[reach] %-34s %+12.2f MB   ADR:%-3s B:%-3s ADRP:%s\n", what, mb,
                magnitude(d) <= kAdrReach ? "yes" : "NO",
                magnitude(d) <= kBReach ? "yes" : "NO",
                magnitude(d) <= kAdrpReach ? "yes" : "NO");
}

// A function in this binary, to stand for a hook target in the executable.
// Deliberately not inlined and deliberately taken by address: an inlined one has
// no address to measure a distance from. SPLICE_TEST_NOINLINE comes from
// helpers/test_targets.h rather than being defined a second time here.
volatile int g_sink = 0;
SPLICE_TEST_NOINLINE void TargetInThisBinary() { g_sink = g_sink + 1; }

}  // namespace

// ─── the measurement S-01 asks for ────────────────────────────────────────

TEST(Arm64Reach, blind_allocation_cannot_reach_this_binary) {
#if !SPLICE_TEST_ARM64
    GTEST_SKIP() << "ARM64 only: this measures ARM64's relocation budgets";
#else
    // What splice does today for ARM64: allocate with no hint at all.
    void* blind = splice::os::allocate_executable_memory(4096);
    ASSERT_NE(blind, nullptr);

    const auto* target = reinterpret_cast<const void*>(&TargetInThisBinary);
    const std::int64_t d = distance(target, blind);
    report("unhinted -> a function in this binary", d);

    // Measured 2026-09-29: 102.46 GB from this test on Android 14 / API 34, and
    // 47.62-166.83 GB from a standalone probe over ten runs on the same device;
    // a flat 85.33 TB on Ubuntu 24.04 aarch64 (Jetson Orin Nano) over five.
    // Never once inside ADRP's +/-4 GB. The assertion is deliberately the weak
    // form -- "further than ADRP can reach" -- because the exact figure is the
    // kernel's business and only the consequence is splice's.
    EXPECT_GT(magnitude(d), kAdrpReach)
        << "An unhinted mapping landed within ADRP range of this binary. That is "
           "not a failure -- it means this platform places anonymous mappings "
           "near the executable, and S-01 shrinks here. Re-measure before "
           "relying on it.";

    splice::os::free_executable_memory(blind, 4096);
#endif
}

TEST(Arm64Reach, hinted_allocation_reaches_this_binary) {
#if !SPLICE_TEST_ARM64
    GTEST_SKIP() << "ARM64 only: this measures ARM64's relocation budgets";
#else
    // The S-01 fix: pass the target as the hint. This is the test that has to
    // keep passing once patcher.cpp does it.
    const auto* target = reinterpret_cast<const void*>(&TargetInThisBinary);
    void* near_block = splice::os::allocate_executable_memory(4096, target);
    ASSERT_NE(near_block, nullptr);

    const std::int64_t d = distance(target, near_block);
    report("hinted -> a function in this binary", d);

    // Measured 2026-09-29 on Android 14 / API 34: +1.14 MB. Inside ADRP and B,
    // OUTSIDE ADR.
    //
    // The 1.14 MB is not a weakness in the search -- it is this binary's own size.
    // allocate_near starts at the page after the target and walks outward, and
    // splice_unit_test is 21 MB, so every page for a long way either side of
    // TargetInThisBinary belongs to the binary and is already mapped. A standalone
    // 9.5 KB probe doing the identical walk landed 6.78 KB away in two probes,
    // because there the next page really was free.
    //
    // Which is the point worth keeping: the achievable distance scales with the
    // size of the module being hooked, so a test binary's result does not predict
    // a real library's, and ADR's +/-1 MB is the first budget to fall. Only ADRP
    // and B are asserted, because only they were measured to hold.
    EXPECT_LE(magnitude(d), kAdrpReach) << "the hint did not bring the block "
                                           "within ADRP range";
    EXPECT_LE(magnitude(d), kBReach) << "the hint did not bring the block within "
                                        "B range";

    splice::os::free_executable_memory(near_block, 4096);
#endif
}

// ─── the case S-01 did not anticipate ─────────────────────────────────────
//
// A shared library, which is the shape that actually gets hooked on Android, is
// a completely different measurement from the executable -- and the answer runs
// the other way. Recorded as its own test because averaging the two would hide
// both.

TEST(Arm64Reach, a_shared_library_is_already_within_adrp_unhinted) {
#if !SPLICE_TEST_ARM64
    GTEST_SKIP() << "ARM64 only: this measures ARM64's relocation budgets";
#elif defined(_WIN32)
    GTEST_SKIP() << "POSIX only: uses dlsym to find a libc symbol";
#else
    void* libc_symbol = dlsym(RTLD_DEFAULT, "memcpy");
    if (libc_symbol == nullptr) {
        GTEST_SKIP() << "no dlsym for memcpy on this platform";
    }

    void* blind = splice::os::allocate_executable_memory(4096);
    ASSERT_NE(blind, nullptr);
    const std::int64_t d = distance(libc_symbol, blind);
    report("unhinted -> libc memcpy", d);

    // Measured 2026-09-29: 41.50-86.10 MB on Android over ten runs, 1.46-1.52 MB
    // on the Jetson over five. ADRP and B reach in every single run WITHOUT any
    // hint, because anonymous mappings and the shared libraries come out of the
    // same region while the executable sits at a different base entirely.
    //
    // So for the case splice actually ships into, near-allocation is not what
    // ADRP needs -- it already reaches. See the ADR test below for what does
    // still fail.
    EXPECT_LE(magnitude(d), kAdrpReach)
        << "An unhinted mapping was more than 4 GB from libc. If this starts "
           "failing, shared-library targets need near-allocation too and S-01's "
           "scope grows.";

    splice::os::free_executable_memory(blind, 4096);
#endif
}

TEST(Arm64Reach, adr_range_to_a_shared_library_is_not_obtainable) {
#if !SPLICE_TEST_ARM64
    GTEST_SKIP() << "ARM64 only: this measures ARM64's relocation budgets";
#elif defined(_WIN32)
    GTEST_SKIP() << "POSIX only: uses dlsym to find a libc symbol";
#else
    void* libc_symbol = dlsym(RTLD_DEFAULT, "memcpy");
    if (libc_symbol == nullptr) {
        GTEST_SKIP() << "no dlsym for memcpy on this platform";
    }

    // Ask for a block near libc, the same way the S-01 fix would.
    void* near_block = splice::os::allocate_executable_memory(4096, libc_symbol);
    ASSERT_NE(near_block, nullptr);
    const std::int64_t d = distance(libc_symbol, near_block);
    report("hinted -> libc memcpy", d);

    // This is the finding, and it is why S-02 is not belt-and-braces.
    //
    // The finding, stated as what was measured: ADR range beside a shared library
    // is obtainable SOMETIMES, and that is worse than never.
    //
    // Android 14 / API 34, four runs of this test: +0.46 MB once (ADR reached),
    // and three times the result was EXACTLY the unhinted distance -- 56.09,
    // 59.83, 36.08 MB -- meaning all 512 probes failed and
    // allocate_executable_memory fell back to an OS-chosen address, as its own
    // warning says it will. A standalone C probe walking the same window agreed
    // independently: 2 successes in 10 runs, needing 170-234 probes each.
    //
    // So 3 of 13 measured attempts landed inside ADR's +/-1 MB. bionic maps
    // libraries contiguously with guard regions, so whether a free page exists
    // within the +/-2 MB kNearProbePages covers is a property of that particular
    // process's layout on that particular boot.
    //
    // An install that works one time in four is worse than one that never works,
    // because it passes testing and fails in the field. That, rather than
    // impossibility, is why S-02 is required.
    //
    // On Ubuntu 24.04 aarch64 (Jetson Orin Nano) the probe succeeded at -650 KB,
    // but needed 324 of its 512 probes -- close to the edge there too.
    //
    // What a better search cannot do: if the region is genuinely full, no amount
    // of searching creates a gap. Reading /proc/self/maps (S-03) would answer
    // faster and would find gaps a linear probe steps over, but it cannot put a
    // page within +/-1 MB of libc when there is no room. And for the executable
    // case above, 1.14 MB was not a search failure at all -- it is simply where
    // the module's own pages end.
    //
    // Deliberately not asserted either way, because both outcomes were observed
    // on the same device within minutes. Asserting either would encode one
    // process layout as a requirement.
    if (magnitude(d) > kAdrReach) {
        std::printf("[reach] ADR range NOT obtainable beside libc -- S-02's "
                    "absolute materialisation is the only fix for an ADR in a "
                    "shared library's prologue\n");
    } else {
        std::printf("[reach] ADR range obtainable beside libc on this platform "
                    "(%.2f KB) -- does not generalise, see S-02\n",
                    static_cast<double>(magnitude(d)) / 1024.0);
    }

    splice::os::free_executable_memory(near_block, 4096);
#endif
}

// ─── does the fix actually enable anything? ───────────────────────────────
//
// The reach tests above measure distances. They do not show that any hook which
// used to be refused now succeeds, and a green suite does not either -- no
// existing target has a PC-relative address computation in its prologue, so none
// of them ever exercised the budget that was failing.
//
// splice::test::pcrel_prologue_target does: it reads a 4096-entry file-scope
// array, which on AArch64 forces ADRP (+ADD or +LDR) into the first four
// instructions, which is exactly the window calculate_copy_size copies.
//
// Before the S-01 fix this install is REFUSED on ARM64 with
// "Complex PC-relative fixup not implemented" -- verified by reverting the one
// line and re-running, which is the only way to know the test is testing
// something. Afterwards it succeeds and the hook is entered.

namespace {
int g_pcrel_hits = 0;
}

// The premise, checked rather than assumed.
//
// This test is worthless unless the target's first four instructions actually
// contain a PC-relative address computation, and whether they do is the
// compiler's decision, not ours. The first version of the target did not, and the
// hook test passed with S-01's fix reverted -- so this guard exists because its
// absence already cost a false pass once.
TEST(Arm64Reach, the_pcrel_target_really_has_a_pcrel_prologue) {
#if !SPLICE_TEST_ARM64
    GTEST_SKIP() << "ARM64 only: uses the ARM64 disassembler";
#else
    using namespace splice::arch::arm64;
    const auto* insns = reinterpret_cast<const std::uint32_t*>(
        &splice::test::pcrel_prologue_target);

    int pcrel = 0;
    for (int i = 0; i < 4; ++i) {
        const InstructionInfo info = analyze_instruction(&insns[i]);
        const char* kind = info.type == InstructionType::Adrp ? "ADRP"
                         : info.type == InstructionType::Adr  ? "ADR"
                         : info.type == InstructionType::B    ? "B"
                         : info.type == InstructionType::Bl   ? "BL"
                         : "regular";
        std::printf("[reach] pcrel target insn %d: 0x%08X  %s\n", i, insns[i],
                    kind);
        if (info.type == InstructionType::Adrp ||
            info.type == InstructionType::Adr) {
            ++pcrel;
        }
    }

    EXPECT_GT(pcrel, 0)
        << "The target's first four instructions contain no ADRP or ADR, so the "
           "hook test below proves nothing about S-01 -- calculate_copy_size "
           "copies 16 bytes and the relocation that was failing is not in them. "
           "Change the target, do not relax this.";
#endif
}

TEST(Arm64Reach, a_pcrel_prologue_can_be_hooked_inline) {
    g_pcrel_hits = 0;

    auto& entry = SPLICE_HOOK_ADDR_STATIC(&splice::test::pcrel_prologue_target)
        .onInvoke([](auto original) -> long {
            ++g_pcrel_hits;
            return original() * 10;
        });
    // install_all() is the only public installer -- install() and
    // install_direct() are both private on InterceptorEntry -- so this test is
    // in the same global installer queue as every other, with the ordering
    // sensitivity test_near_alloc.cpp:213 already documents. A dedicated target
    // symbol is what keeps it from colliding: nothing else hooks
    // pcrel_prologue_target.
    splice::install_all();
    ASSERT_TRUE(entry.is_installed())
        << "installing over a PC-relative prologue failed. On ARM64 this is "
           "S-01: the trampoline is too far for the copied ADRP to be "
           "relocated, and the engine returned a null stub.";

    EXPECT_EQ(splice::test::pcrel_prologue_target(), 10);
    EXPECT_EQ(g_pcrel_hits, 1);

    // Tier 2 disable restores the patched prologue atomically. Checked here
    // because a prologue containing a relocated ADRP is exactly the case where
    // restoring the wrong bytes would go unnoticed until the next call.
    EXPECT_TRUE(entry.disable());
    EXPECT_FALSE(entry.is_installed());
    EXPECT_EQ(splice::test::pcrel_prologue_target(), 1);
    EXPECT_EQ(g_pcrel_hits, 1) << "the hook was entered after disable()";
}
