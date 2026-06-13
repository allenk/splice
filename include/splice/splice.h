// ─── Splice umbrella header ────────────────────────────────────────────────
//
// Pulls in the full public API. Performance-sensitive consumers should
// include only what they need:
//
//   <splice/log.h>        — logging substrate + hot-path variants
//   <splice/macros.h>     — SPLICE_HOOK*, SPLICE_HOOK_STATIC, SPLICE_HOOK_ADDR
//   <splice/core.h>       — InterceptorEntry<>
//   <splice/registry.h>   — IdRegistry, HookManager, OriginalRegistry
//   <splice/traits.h>     — function_traits<>
//   <splice/trampoline.h> — TrampolineGenerator<>
//   <splice/engine.h>     — C ABI: splice_hook_symbol(), splice_hook_address()
//
// Per project rule (CLAUDE.md §Rules), this header stays under 50 lines.
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#include <splice/context.h>
#include <splice/core.h>
#include <splice/diagnostics.h>
#include <splice/engine.h>
#include <splice/log.h>
#include <splice/macros.h>
#include <splice/registry.h>
#include <splice/traits.h>
#include <splice/trampoline.h>

namespace splice {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

} // namespace splice
