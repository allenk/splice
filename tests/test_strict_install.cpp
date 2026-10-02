#include <gtest/gtest.h>
#include <splice/engine.h>
#include "os/memory.h"
#include <array>
#include <cstring>

#if defined(_WIN32)
extern "C" __declspec(dllimport) void* splice_test_decoy_site();
#endif

namespace {
class StrictInstall : public ::testing::Test {
protected:
    unsigned char* block = nullptr;
    splice_patch_record installed{};
    void SetUp() override {
        block = static_cast<unsigned char*>(splice::os::allocate_executable_memory(4096));
        ASSERT_NE(block, nullptr);
        std::memset(block, 0x90, 4096);
    }
    void TearDown() override {
        if (installed.hook_site != nullptr) EXPECT_EQ(splice_disable(&installed), 0);
        if (block != nullptr) splice::os::free_executable_memory(block, 4096);
    }
};

// T-011-14: all alignments, with the real allocator and actual patch bytes.
TEST_F(StrictInstall, alignment_matrix_refuses_before_publication) {
    splice::os::restore_executable(block, 4096);
    for (unsigned offset = 0; offset != 8; ++offset) {
        auto* target = block + 64 * offset + offset;
        std::array<unsigned char, 16> expected{};
        std::memcpy(expected.data(), target, expected.size());
        int sentinel = 0;
        void* original = &sentinel;
        splice_patch_record record;
        std::memset(&record, 0x5a, sizeof(record));
        const auto record_before = record;
        unsigned calls = 0;
        const auto result = splice_hook_address_strict_pre_rec(
            target, block + 1024, &original,
            [](void*, void* user) { ++*static_cast<unsigned*>(user); }, &calls,
            expected.data(), 16, &record);
        if (offset < 4) {
            ASSERT_EQ(result, target);
            EXPECT_EQ(calls, 1u);
            EXPECT_EQ(record.hook_site, target);
            EXPECT_EQ(record.strategy, SPLICE_PATCH_STRATEGY_INLINE);
            EXPECT_EQ(record.pre_hook_byte_len, 5u);
            EXPECT_EQ(std::memcmp(target + 5, expected.data() + 5, 11), 0);
            EXPECT_EQ(splice_disable(&record), 0);
        } else {
            EXPECT_EQ(result, nullptr);
            EXPECT_EQ(calls, 0u);
            EXPECT_EQ(original, &sentinel);
            EXPECT_EQ(std::memcmp(&record, &record_before, sizeof(record)), 0);
            EXPECT_EQ(std::memcmp(target, expected.data(), 16), 0);
            // Keep the pre-fix RED test from leaking a patch into later cases.
            if (result != nullptr) EXPECT_EQ(splice_disable(&record), 0);
        }
    }
}

TEST_F(StrictInstall, stale_snapshot_is_refused_without_outputs) {
    std::array<unsigned char, 16> expected{};
    expected.fill(0x90);
    expected[15] = 0x91;
    splice::os::restore_executable(block, 4096);
    void* original = nullptr;
    const auto result = splice_hook_address_strict_pre_rec(
        block, block + 1024, &original, nullptr, nullptr, expected.data(), 16, &installed);
    EXPECT_EQ(result, nullptr);
    EXPECT_EQ(original, nullptr);
    EXPECT_EQ(block[0], 0x90);
}

TEST_F(StrictInstall, crossing_instruction_tail_is_preserved_and_repeat_refused) {
    // mov eax,42 (5 bytes) preceded by a NOP: copy boundary is six, patch is five.
    const unsigned char code[] = {0x90, 0xb8, 42, 0, 0, 0, 0xc3};
    std::memcpy(block, code, sizeof(code));
    const unsigned char replacement[] = {0xb8, 99, 0, 0, 0, 0xc3};
    std::memcpy(block + 1024, replacement, sizeof(replacement));
    splice::os::flush_instruction_cache(block, 4096);
    std::array<unsigned char, 16> expected{};
    std::memcpy(expected.data(), block, 16);
    splice::os::restore_executable(block, 4096);
    void* original = nullptr;
    ASSERT_EQ(splice_hook_address_strict_pre_rec(block, block + 1024, &original,
        nullptr, nullptr, expected.data(), 16, &installed), block);
    EXPECT_EQ(std::memcmp(block + 5, expected.data() + 5, 11), 0);
    EXPECT_EQ(installed.pre_hook_byte_len, 5u);
    EXPECT_EQ(installed.trampoline, original);
    EXPECT_EQ(reinterpret_cast<int (*)()>(original)(), 42);
    EXPECT_EQ(reinterpret_cast<int (*)()>(block)(), 99);
    EXPECT_EQ(splice_hook_address_strict_pre_rec(block, block + 1024, &original,
        nullptr, nullptr, expected.data(), 16, nullptr), nullptr);
}

TEST_F(StrictInstall, invalid_snapshot_size_and_null_inputs_are_refused) {
    std::array<unsigned char, 16> expected{};
    expected.fill(0x90);
    splice::os::restore_executable(block, 4096);
    for (unsigned scenario = 0; scenario != 5; ++scenario) {
        int sentinel = 0;
        void* original = &sentinel;
        splice_patch_record record;
        std::memset(&record, 0x5a, sizeof(record));
        const auto before = record;
        unsigned calls = 0;
        EXPECT_EQ(splice_hook_address_strict_pre_rec(
            scenario == 0 ? nullptr : block,
            scenario == 1 ? nullptr : block + 1024, &original,
            [](void*, void* user) { ++*static_cast<unsigned*>(user); }, &calls,
            scenario == 2 ? nullptr : expected.data(), scenario == 3 ? 15 :
                (scenario == 4 ? 17 : 16), &record), nullptr);
        EXPECT_EQ(calls, 0u);
        EXPECT_EQ(original, &sentinel);
        EXPECT_EQ(std::memcmp(&record, &before, sizeof(record)), 0);
        EXPECT_EQ(std::memcmp(block, expected.data(), 16), 0);
    }
}

// T-011-15: unsupported short and indirect jump stubs are not followed.
TEST_F(StrictInstall, short_and_indirect_jumps_are_refused_unchanged) {
    block[0] = 0xeb;
    block[1] = 62;  // Jump to block + 64.
    block[128] = 0xff;
    block[129] = 0x25;
    std::memset(block + 130, 0, 4);
    void* destination = block + 256;
    std::memcpy(block + 134, &destination, sizeof(destination));
    std::array<unsigned char, 4096> before{};
    std::memcpy(before.data(), block, before.size());
    splice::os::restore_executable(block, 4096);
    for (const auto offset : {0u, 128u}) {
        int sentinel = 0;
        void* original = &sentinel;
        splice_patch_record record;
        std::memset(&record, 0x5a, sizeof(record));
        const auto record_before = record;
        unsigned calls = 0;
        EXPECT_EQ(splice_hook_address_strict_pre_rec(block + offset, block + 1024,
            &original, [](void*, void* user) { ++*static_cast<unsigned*>(user); },
            &calls, before.data() + offset, 16, &record), nullptr);
        EXPECT_EQ(calls, 0u);
        EXPECT_EQ(original, &sentinel);
        EXPECT_EQ(std::memcmp(&record, &record_before, sizeof(record)), 0);
        EXPECT_EQ(std::memcmp(block, before.data(), before.size()), 0);
    }
}

#if defined(_WIN32)
// T-011-15: the other DLL owns a different Splice registry.
TEST(StrictDecoy, independent_dll_jump_and_destination_remain_unchanged) {
    auto* site = static_cast<unsigned char*>(splice_test_decoy_site());
    ASSERT_NE(site, nullptr);
    std::array<unsigned char, 16> expected{};
    std::array<unsigned char, 16> destination{};
    std::memcpy(expected.data(), site, 16);
    std::memcpy(destination.data(), site + 64, 16);
    ASSERT_EQ(site[0], 0xe9);
    void* original = nullptr;
    splice_patch_record record{};
    unsigned calls = 0;
    EXPECT_EQ(splice_hook_address_strict_pre_rec(site, site + 128, &original,
        [](void*, void* user) { ++*static_cast<unsigned*>(user); }, &calls,
        expected.data(), 16, &record), nullptr);
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(original, nullptr);
    EXPECT_EQ(record.hook_site, nullptr);
    EXPECT_EQ(std::memcmp(site, expected.data(), 16), 0);
    EXPECT_EQ(std::memcmp(site + 64, destination.data(), 16), 0);
}
#endif
}
