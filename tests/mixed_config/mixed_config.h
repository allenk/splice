// ─── what each translation unit reports back ──────────────────────────────
//
// Two translation units in one program, compiled with different values of
// `SPLICE_MAX_HOOKS`, and a shared inline function below that uses splice from
// a header -- which is the shape an external review named as the one a
// configuration tag cannot reach, because the enclosing inline function is the
// consumer's and no namespace the library declares can enclose it.
//
// Since 2026-10-13 that shape is harmless rather than undetected: the capacity
// lives behind a pointer and a runtime count inside the library, so no
// header-visible layout or inline expression depends on it.
#pragma once

#include <splice/context.h>
#include <splice/registry.h>

#include <cstddef>

namespace splice_mixed_config {

struct Report {
    std::size_t     sizeof_context = 0;        // as THIS translation unit sees it
    std::size_t     library_context_size = 0;  // as the library reports it
    int             configured_capacity = 0;   // the library's, at runtime
    int             compiled_with = 0;         // this TU's macro value
    int             slot_used = -1;
    splice::Refusal set_original = splice::Refusal::ok;
    splice::Refusal get_original = splice::Refusal::ok;
    splice::Refusal get_hook = splice::Refusal::ok;
    bool            original_round_trips = false;
    bool            hook_is_usable = false;
    bool            hook_dispatches = false;
};

Report from_small_capacity_tu();
Report from_default_capacity_tu();

// The case the review named: a `SPLICE_HOOK`-shaped use of the library inside
// an externally-linked inline function in a shared header. Both translation
// units below include this, so the enclosing function has one identity across
// both configurations and whichever body the linker keeps is what both call.
//
// It does not install anything -- that would need a hookable target per
// configuration and this is about layout, not patching. It reads the shared
// context through an inline body, which is the operation that would have
// misread a member had the capacity still been part of the layout.
inline std::size_t shared_inline_view_of_the_context() {
    return sizeof(splice::HookContext) + (splice::default_context().slot_capacity() > 0 ? 0u : 1u);
}

} // namespace splice_mixed_config
