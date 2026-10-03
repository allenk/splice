#include <gtest/gtest.h>
#include "arch/x86_64/patcher.h"
#include "os/memory.h"
#include <array>
#include <cstring>

namespace {
using splice::arch::x86_64::PreparedStrictPatch;
class PreparedPatch : public ::testing::Test {
protected:
    unsigned char* block{};
    std::array<unsigned char, 16> expected{};
    void SetUp() override {
        block = static_cast<unsigned char*>(splice::os::allocate_executable_memory(4096));
        ASSERT_NE(block, nullptr);
        std::memset(block, 0x90, 4096);
        const unsigned char code[] = {0x48, 0x8b, 0xc1, 0xb8, 42, 0, 0, 0, 0xc3};
        const unsigned char replacement[] = {0xb8, 99, 0, 0, 0, 0xc3};
        std::memcpy(block, code, sizeof(code));
        std::memcpy(block + 1024, replacement, sizeof(replacement));
        std::memcpy(expected.data(), block, 16);
        splice::os::flush_instruction_cache(block, 4096);
        splice::os::restore_executable(block, 4096);
    }
    void TearDown() override { if (block) splice::os::free_executable_memory(block, 4096); }
};

TEST_F(PreparedPatch, preparation_preserves_target_and_maps_exact_boundaries) {
    PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    EXPECT_EQ(std::memcmp(block, expected.data(), 16), 0);
    EXPECT_EQ(plan.copy_size(), 8u);
    const auto base = reinterpret_cast<std::uintptr_t>(block);
    const auto trampoline = reinterpret_cast<std::uintptr_t>(plan.original());
    for (unsigned offset = 0; offset != 10; ++offset) {
        std::uintptr_t mapped = 123;
        const bool boundary = offset == 0 || offset == 3 || offset >= 8;
        EXPECT_EQ(plan.map_ip(base + offset, mapped), boundary);
        EXPECT_EQ(mapped, !boundary ? 123 : offset >= 8 ? base + offset : trampoline + offset);
    }
    std::uintptr_t outside{};
    EXPECT_TRUE(plan.map_ip(base - 1, outside));
    EXPECT_EQ(outside, base - 1);
    EXPECT_EQ(reinterpret_cast<int (*)()>(plan.original())(), 42);
    // This fixture's first instruction changes no state needed by its tail.
    EXPECT_EQ(reinterpret_cast<int (*)()>(trampoline + 3)(), 42);
}

TEST_F(PreparedPatch, controlled_commit_is_single_use_and_preserves_tail) {
    PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    ASSERT_TRUE(splice::os::make_executable_writable(block, 5));
    ASSERT_TRUE(plan.commit_write());
    splice::os::flush_instruction_cache(block, 5);
    splice::os::restore_executable(block, 5);
    EXPECT_FALSE(plan.commit_write());
    EXPECT_EQ(std::memcmp(block + 5, expected.data() + 5, 11), 0);
    EXPECT_EQ(reinterpret_cast<int (*)()>(block)(), 99);
    EXPECT_EQ(reinterpret_cast<int (*)()>(plan.original())(), 42);
}

TEST_F(PreparedPatch, stale_snapshot_refuses_without_target_write) {
    PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    ASSERT_TRUE(splice::os::make_executable_writable(block, 16));
    block[15] ^= 1;
    const unsigned char changed = block[15];
    EXPECT_FALSE(plan.commit_write());
    EXPECT_EQ(std::memcmp(block, expected.data(), 15), 0);
    EXPECT_EQ(block[15], changed);
    splice::os::restore_executable(block, 16);
}

TEST_F(PreparedPatch, invalid_input_and_empty_plan_refuse) {
    PreparedStrictPatch plan;
    std::uintptr_t mapped = 123;
    EXPECT_FALSE(plan.map_ip(0, mapped));
    EXPECT_EQ(mapped, 123u);
    EXPECT_FALSE(plan.commit_write());
    EXPECT_FALSE(plan.prepare(nullptr, block, expected.data(), 16));
    EXPECT_FALSE(plan.prepare(block, nullptr, expected.data(), 16));
    EXPECT_FALSE(plan.prepare(block, block + 1024, nullptr, 16));
    EXPECT_FALSE(plan.prepare(block, block + 1024, expected.data(), 15));
    auto mismatch = expected;
    mismatch[15] ^= 1;
    EXPECT_FALSE(plan.prepare(block, block + 1024, mismatch.data(), 16));
    EXPECT_FALSE(plan.prepare(block + 4, block + 1024, expected.data(), 16));
    EXPECT_EQ(plan.original(), nullptr);
    EXPECT_EQ(std::memcmp(block, expected.data(), 16), 0);
}

TEST_F(PreparedPatch, rip_relative_original_preserves_loaded_value) {
    ASSERT_TRUE(splice::os::make_executable_writable(block, 4096));
    const unsigned char code[] = {0x8b, 0x05, 58, 0, 0, 0, 0xc3};
    std::memcpy(block, code, sizeof(code));
    const int value = 57;
    std::memcpy(block + 64, &value, sizeof(value));
    std::memcpy(expected.data(), block, 16);
    splice::os::flush_instruction_cache(block, 4096);
    splice::os::restore_executable(block, 4096);
    PreparedStrictPatch plan;
    ASSERT_TRUE(plan.prepare(block, block + 1024, expected.data(), 16));
    EXPECT_EQ(plan.copy_size(), 6u);
    EXPECT_EQ(reinterpret_cast<int (*)()>(plan.original())(), value);
    EXPECT_FALSE(plan.prepare(block, block + 1024, expected.data(), 16));
}

TEST_F(PreparedPatch, branch_prologue_refuses_before_preparation) {
    ASSERT_TRUE(splice::os::make_executable_writable(block, 16));
    block[0] = 0xe9;
    std::memset(block + 1, 0, 4);
    std::memcpy(expected.data(), block, 16);
    splice::os::flush_instruction_cache(block, 16);
    splice::os::restore_executable(block, 16);
    PreparedStrictPatch plan;
    EXPECT_FALSE(plan.prepare(block, block + 1024, expected.data(), 16));
    EXPECT_EQ(plan.original(), nullptr);
    EXPECT_EQ(std::memcmp(block, expected.data(), 16), 0);
}

TEST_F(PreparedPatch, rip_relative_trailing_immediate_refuses_unchanged) {
    ASSERT_TRUE(splice::os::make_executable_writable(block, 16));
    const unsigned char code[] = {0xc7, 0x05, 0x10, 0, 0, 0, 0x78, 0x56, 0x34, 0x12};
    std::memcpy(block, code, sizeof(code));
    std::memcpy(expected.data(), block, 16);
    splice::os::flush_instruction_cache(block, 16);
    splice::os::restore_executable(block, 16);
    PreparedStrictPatch plan;
    EXPECT_FALSE(plan.prepare(block, block + 1024, expected.data(), 16));
    EXPECT_EQ(plan.original(), nullptr);
    EXPECT_EQ(std::memcmp(block, expected.data(), 16), 0);
}
}
