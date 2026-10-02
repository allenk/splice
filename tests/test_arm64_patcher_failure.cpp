#include <gtest/gtest.h>
#include "arch/arm64/patcher.h"
#include "arch/arm64/atomic_patch.h"
#include "os/memory.h"
#include <array>
#include <cstdint>

namespace {
alignas(16) std::array<std::uint32_t, 64> trampoline_storage{};
unsigned allocations = 0;
unsigned frees = 0;
unsigned writable_requests = 0;
unsigned restore_requests = 0;
unsigned atomic_calls = 0;
}

namespace splice::os {
void* allocate_executable_memory(std::size_t size, const void*) {
    EXPECT_LE(size, sizeof(trampoline_storage));
    ++allocations;
    return trampoline_storage.data();
}
void free_executable_memory(void* memory, std::size_t) {
    EXPECT_EQ(memory, trampoline_storage.data());
    ++frees;
}
bool make_executable_writable(void*, std::size_t size) {
    EXPECT_EQ(size, 16u);
    ++writable_requests;
    return false;
}
void restore_executable(void*, std::size_t) { ++restore_requests; }
void flush_instruction_cache(void*, std::size_t) {}
std::size_t page_size() { return 4096; }
}

namespace splice::arch::arm64 {
// Deliberately do not link the native ARM64 atomic implementation on x64.
// Any reach into installation means the permission refusal was ignored.
void atomic_install_indirect_branch(void*, void*) {
    ++atomic_calls;
    ADD_FAILURE() << "Atomic installation reached after denied write permission";
}
}

// T-011-17: host execution tests lifecycle, not ARM instruction execution.
TEST(Arm64PatcherFailure, denied_write_does_not_publish_or_modify_target) {
    alignas(16) std::array<std::uint32_t, 16> target{};
    target.fill(0xd503201f);  // ARM64 NOP: relocatable, no earlier failure.
    const auto before = target;
    int sentinel = 0;
    void* original = &sentinel;
    unsigned callback_calls = 0;
    allocations = frees = writable_requests = restore_requests = atomic_calls = 0;

    const auto result = splice::arch::arm64::install_inline_patch(
        target.data(), target.data() + 8, &original,
        [](void*, void* user) { ++*static_cast<unsigned*>(user); }, &callback_calls);

    EXPECT_EQ(result, nullptr);
    EXPECT_EQ(writable_requests, 1u);
    EXPECT_EQ(allocations, 1u);
    EXPECT_EQ(frees, 1u);
    EXPECT_EQ(restore_requests, 0u);
    EXPECT_EQ(atomic_calls, 0u);
    EXPECT_EQ(callback_calls, 0u);
    EXPECT_EQ(original, &sentinel);
    EXPECT_EQ(target, before);
}
