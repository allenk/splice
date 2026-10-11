// ─── Reading an environment variable, without a deprecation warning ───────
//
// Several tests are local gates that step aside on shared CI runners, and they
// all ask the same question. MSVC deprecates `getenv` in favour of
// `_dupenv_s`, so every caller that spelled it directly carried a C4996 -- one
// of them since 2026-06-13. One helper, warning-free on both compilers, is
// cheaper than three suppressions.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdlib>

namespace splice::test {

// True when `name` is present in the environment, whatever its value. The
// convention every caller here uses is presence, not content: `CI` is set to
// all sorts of things by all sorts of runners.
inline bool env_is_set(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t length = 0;
    if (::_dupenv_s(&value, &length, name) != 0) return false;
    const bool present = value != nullptr;
    std::free(value);
    return present;
#else
    return std::getenv(name) != nullptr;
#endif
}

} // namespace splice::test
