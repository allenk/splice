// ─── Splice engine — stub for unsupported platforms ───────────────────────
//
// Compiled only when no backend matches the current platform (today: Windows
// x86_64, x86_64 POSIX without Phase 3, etc.). Lets the public headers
// compile and template machinery instantiate, so developers can still
// iterate on the C++ surface without an ARM64/Android toolchain.
//
// install() calls from InterceptorEntry will log-and-return. Real hooking
// lands when the matching backend is enabled.
// ───────────────────────────────────────────────────────────────────────────
#include <splice/engine.h>

#include <splice/log.h>

#include "vtable_slot.h"

extern "C" {

// Resolving a pointer-to-member needs no OS layer and no arch backend -- it is
// ABI arithmetic, or decoding a thunk the compiler emitted in this binary --
// so it answers for real even on a platform splice cannot hook.
int splice_vtable_slot_of_pmf(const void* pmf_bytes, unsigned int pmf_size) {
    return splice::engine::slot_of_member_pointer(pmf_bytes, pmf_size);
}

int splice_vtable_swap(void**, void*, void**, splice_patch_record*) {
    SPLICE_LOGW_ONCE("splice_vtable_swap: no backend compiled for this platform");
    return -1;
}

int splice_vtable_swap_expect(void**, void*, void*, void** out_found,
                              splice_patch_record*) {
    SPLICE_LOGW_ONCE("splice_vtable_swap_expect: no backend compiled for this "
                     "platform");
    // A refusal still answers the question the caller asked -- "what is in the
    // slot?" -- and null is not that answer, so say nothing rather than
    // something false. The caller is told it was refused by the return value.
    (void)out_found;
    return -1;
}

// Reading bytes needs no OS layer, but deciding how many of them a patch would
// displace needs the arch decoder, and a platform without one has no honest
// answer. `out` is left untouched, which is what the header promises for a
// failure, so a caller cannot mistake a zeroed struct for a measured one.
int splice_prologue_profile(const void*, splice_prologue_facts*) {
    SPLICE_LOGW_ONCE("splice_prologue_profile: no decoder compiled for this "
                     "platform");
    return -1;
}

// Resolution is the OS layer's job and this platform has none compiled, so
// there is nothing to ask. Returning null is the same answer the real one
// gives for a module or symbol it cannot find, and the caller already has to
// handle it.
void* splice_resolve_symbol(const char* lib_name, const char* symbol_name) {
    SPLICE_LOGW_ONCE("splice_resolve_symbol: no backend compiled for this "
                     "platform (ignored: %s::%s)",
                     lib_name ? lib_name : "(nil)",
                     symbol_name ? symbol_name : "(nil)");
    return nullptr;
}

int splice_live_supported(void) { return 0; }

void* splice_live_prepare(const splice_live_site*, unsigned int, void**,
                          splice_live_outcome* out) {
    SPLICE_LOGW_ONCE("splice_live_prepare: no backend compiled for this platform");
    if (out != nullptr) {
        *out = splice_live_outcome{};
        out->state = SPLICE_LIVE_REFUSED;
    }
    return nullptr;
}

void splice_live_commit(void*, splice_patch_record*, splice_live_outcome* out) {
    if (out != nullptr) {
        *out = splice_live_outcome{};
        out->state = SPLICE_LIVE_REFUSED;
    }
}

void splice_live_release(void*) {}

void* splice_hook_address_strict_pre_rec(void*, void*, void**,
                                        splice_pre_patch_fn, void*,
                                        const unsigned char*, unsigned int,
                                        splice_patch_record*) {
    return nullptr;
}

void* splice_hook_symbol(const char* lib_name,
                         const char* symbol_name,
                         void* /*new_func*/,
                         void** /*original_func*/) {
    SPLICE_LOGW_ONCE("splice_hook_symbol: no backend compiled for this platform "
                     "(ignored: %s::%s)",
                     lib_name ? lib_name : "(nil)",
                     symbol_name ? symbol_name : "(nil)");
    return nullptr;
}

void* splice_hook_symbol_pre(const char* lib_name,
                             const char* symbol_name,
                             void* /*new_func*/,
                             void** /*original_func*/,
                             splice_pre_patch_fn /*pre_cb*/,
                             void* /*pre_user*/) {
    SPLICE_LOGW_ONCE("splice_hook_symbol_pre: no backend compiled (ignored: %s::%s)",
                     lib_name ? lib_name : "(nil)",
                     symbol_name ? symbol_name : "(nil)");
    return nullptr;
}

void* splice_hook_address_pre_rec(void* target_addr,
                                  void* /*new_func*/,
                                  void** /*original_func*/,
                                  splice_pre_patch_fn /*pre_cb*/,
                                  void* /*pre_user*/,
                                  splice_patch_record* /*out_record*/) {
    SPLICE_LOGW_ONCE("splice_hook_address_pre_rec: no backend compiled (ignored target=%p)",
                     target_addr);
    return nullptr;
}

void* splice_hook_symbol_pre_rec(const char* lib_name,
                                 const char* symbol_name,
                                 void* /*new_func*/,
                                 void** /*original_func*/,
                                 splice_pre_patch_fn /*pre_cb*/,
                                 void* /*pre_user*/,
                                 splice_patch_record* /*out_record*/) {
    SPLICE_LOGW_ONCE("splice_hook_symbol_pre_rec: no backend compiled (ignored: %s::%s)",
                     lib_name ? lib_name : "(nil)",
                     symbol_name ? symbol_name : "(nil)");
    return nullptr;
}

int splice_disable(const splice_patch_record* /*record*/) {
    SPLICE_LOGW_ONCE("splice_disable: no backend compiled");
    return -1;
}

void* splice_hook_address(void* target_addr,
                          void* /*new_func*/,
                          void** /*original_func*/) {
    SPLICE_LOGW_ONCE("splice_hook_address: no backend compiled (ignored target=%p)",
                     target_addr);
    return nullptr;
}

void* splice_hook_address_pre(void* target_addr,
                              void* /*new_func*/,
                              void** /*original_func*/,
                              splice_pre_patch_fn /*pre_cb*/,
                              void* /*pre_user*/) {
    SPLICE_LOGW_ONCE("splice_hook_address_pre: no backend compiled (ignored target=%p)",
                     target_addr);
    return nullptr;
}

int splice_is_hooked(const void* /*addr*/) {
    return 0;
}

void splice_debug_function_start(const void* /*func_addr*/, const char* /*label*/) {
    SPLICE_LOGW_ONCE("splice_debug_function_start: no disassembler on this platform");
}

} // extern "C"
