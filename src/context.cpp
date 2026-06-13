// ─── Splice HookContext — out-of-line singleton ───────────────────────────
//
// Parks the default-context Meyers singleton in a compiled TU so there is
// exactly one instance across all translation units that link against
// libsplice. A purely inline definition in context.h would give each TU
// its own copy on Windows when splice is linked statically into multiple
// DLLs — the hazard the v1.4 review called out.
// ───────────────────────────────────────────────────────────────────────────
#include <splice/context.h>

namespace splice {

HookContext& default_context() noexcept {
    static HookContext ctx;
    return ctx;
}

} // namespace splice
