#include "bench_targets.h"

#include <cstdint>

namespace splice::bench {

// Cheap but not *too* trivial: at MSVC /O2 the one-liner `return x + 1;`
// gets ICF-folded with other identical-shape functions and the linker
// rewrites the prologue with a RIP-relative branch, which the prologue
// relocator declines to copy. A handful of independent integer ops keeps
// the prologue plain `Regular` (push rbp / mov / arithmetic) while still
// being only a few cycles per call.
SPLICE_BENCH_NOINLINE int hot_target(int x) {
    std::uint32_t r = static_cast<std::uint32_t>(x);
    r ^= 0x9E3779B9u;
    r *= 0x85EBCA6Bu;
    r ^= r >> 13;
    return static_cast<int>(r);
}

} // namespace splice::bench
