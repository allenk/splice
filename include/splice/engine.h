// ─── Splice engine — C ABI boundary ────────────────────────────────────────
//
// The ONLY way public template code talks to the compiled backend. Keeping
// the surface C-linkage lets us:
//   - compile the engine as a single TU (no header-only template bloat)
//   - swap backend implementations per platform without touching public API
//   - link against pre-built engine in tooling / language bindings
//
// Architecture boundary (architecture.md §Dependency Rules):
//   Public headers ──(C ABI only)──▶ Engine
// ───────────────────────────────────────────────────────────────────────────
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Hook a symbol resolved by name in a shared library.
//
// @param lib_name     e.g. "libGL.so"; may be NULL/empty to reinterpret
//                     `symbol_name` as a raw address (legacy compat path).
// @param symbol_name  dlsym/GetProcAddress name.
// @param new_func     replacement function pointer (trampoline address).
// @param original_func  out: populated with a pointer that calls the
//                     original function (the user's "call through" path).
// @return             the patched location on success, NULL on failure.
void* splice_hook_symbol(const char* lib_name,
                         const char* symbol_name,
                         void* new_func,
                         void** original_func);

// Hook at a known address. Tries GOT/PLT reverse first (safe); falls back
// to direct instruction patching if the address isn't in any import table.
//
// This is the modern, strictly-typed alternative to passing an address as
// `(const char*)addr` into splice_hook_symbol.
void* splice_hook_address(void* target_addr,
                          void* new_func,
                          void** original_func);

// Pre-patch callback signature. Fires after the original function pointer
// is identified (trampoline built / GOT slot read) but BEFORE the patch
// is committed. Use this to register the original with a downstream
// dispatcher table before any thread can observe the patch.
typedef void (*splice_pre_patch_fn)(void* original, void* user_data);

// Like `splice_hook_address`, but invokes `pre_cb` between trampoline
// preparation and the atomic patch. Closes a race window where, between
// the patch landing and the caller updating its dispatcher tables,
// another thread can observe the new patched prologue / GOT slot but
// read a stale `original` pointer.
//
// `splice_hook_address(...)` is equivalent to
// `splice_hook_address_pre(..., nullptr, nullptr)`.
void* splice_hook_address_pre(void* target_addr,
                              void* new_func,
                              void** original_func,
                              splice_pre_patch_fn pre_cb,
                              void* pre_user);

// Symbol-resolution variant of `splice_hook_address_pre`. Resolves
// `lib_name::symbol_name` via the platform loader, then performs the
// hook via the same pre-patch callback path as `splice_hook_address_pre`.
void* splice_hook_symbol_pre(const char* lib_name,
                             const char* symbol_name,
                             void* new_func,
                             void** original_func,
                             splice_pre_patch_fn pre_cb,
                             void* pre_user);

// ─── Disable / FR-013 Tiered Semantics ────────────────────────────────────
//
// Splice's threat model (in-process, non-privileged, no ptrace) makes
// true reversible uninstall architecturally impossible. Instead we offer
// **disable** in two tiers, named to make the limit visible:
//
//   Tier 1 — POINTER_SWAP    (GOT / IAT / vtable slot)
//     Atomic pointer-sized write of the original back into the slot.
//     Always safe; reversible without resource leak.
//
//   Tier 2 — INLINE          (target prologue overwritten with jmp)
//     Atomic restore of the original first instruction word into the
//     prologue. **Trampoline memory is permanently leaked** because we
//     can't prove no thread is currently executing inside it. Same
//     trade-off as ShadowHook's lazy cleanup. Documented limitation.
//
//   Tier 3 — true reversible uninstall with memory reclaim:
//     NOT IMPLEMENTED, by design. Requires ptrace / stop-the-world.
//     See docs/Splice Plan.md §FR-013 for the rationale.

// Strategy used by an installed hook. Filled in by `splice_hook_*_pre`
// when the caller passes a non-null `out_record`.
typedef enum {
    SPLICE_PATCH_STRATEGY_POINTER_SWAP = 0,   // GOT / IAT
    SPLICE_PATCH_STRATEGY_INLINE       = 1,   // direct prologue overwrite
} splice_patch_strategy;

// Patch record — opaque to user code, but plain-old-data so it can be
// stored and shipped by value. Populated by install, consumed by disable.
typedef struct {
    int   strategy;             // splice_patch_strategy
    void* hook_site;            // strategy=POINTER_SWAP: void** slot (cast to void*)
                                // strategy=INLINE      : target function address
    void* pre_hook_pointer;     // strategy=POINTER_SWAP: original function pointer
                                // strategy=INLINE      : unused (zero)
    void* trampoline;           // strategy=INLINE      : allocated trampoline (do not free)
                                // strategy=POINTER_SWAP: unused (zero)
    unsigned char pre_hook_bytes[16];  // strategy=INLINE: original first 16 bytes
    unsigned int  pre_hook_byte_len;   // valid bytes in pre_hook_bytes (4 = ARM64, 5..14 = x86_64)
} splice_patch_record;

// Extended install variants that fill `out_record` (pass null to skip).
// The shorter variants (`splice_hook_address_pre` / `splice_hook_symbol_pre`)
// remain available for callers that don't need disable capability.
void* splice_hook_address_pre_rec(void* target_addr,
                                  void* new_func,
                                  void** original_func,
                                  splice_pre_patch_fn pre_cb,
                                  void* pre_user,
                                  splice_patch_record* out_record);

void* splice_hook_symbol_pre_rec(const char* lib_name,
                                 const char* symbol_name,
                                 void* new_func,
                                 void** original_func,
                                 splice_pre_patch_fn pre_cb,
                                 void* pre_user,
                                 splice_patch_record* out_record);

// Disable a previously-installed hook described by `record`.
// Returns:
//    0 on success
//   -1 on failure (record invalid, slot value didn't match expected hook,
//                  arch operation rejected, etc.)
//   -2 on "tier not yet implemented" (currently INLINE / Tier 2)
//
// After successful return, the patch is reversed:
//   - POINTER_SWAP: the slot holds `pre_hook_pointer` again.
//   - INLINE     : (when implemented) the prologue holds the original
//                  bytes again; trampoline memory remains allocated.
int splice_disable(const splice_patch_record* record);

// Probe helper — returns non-zero if `addr` looks like it was patched by
// Splice (matches our trampoline signature). Not a security check; purely
// a debugging aid.
int splice_is_hooked(const void* addr);

// Diagnostic: log the first 8 instructions starting at `func_addr` via
// SPLICE_LOG*. Useful when a hook "silently fails" to attach.
void splice_debug_function_start(const void* func_addr, const char* label);

#ifdef __cplusplus
} // extern "C"
#endif
