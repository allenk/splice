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

extern "C" {

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
