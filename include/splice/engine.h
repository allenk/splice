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
//     NOT IMPLEMENTED, by design. Requires ptrace / stop-the-world,
//     which an in-process non-privileged hook does not have. See the
//     README's "What Splice does not do" and docs/splice-guide.md.

// Strategy used by an installed hook. Filled in by `splice_hook_*_pre`
// when the caller passes a non-null `out_record`.
typedef enum {
    SPLICE_PATCH_STRATEGY_POINTER_SWAP = 0,   // GOT / IAT
    SPLICE_PATCH_STRATEGY_INLINE       = 1,   // direct prologue overwrite
    SPLICE_PATCH_STRATEGY_DATA_SLOT    = 2,   // a pointer slot in data we do not own
} splice_patch_strategy;

// Why DATA_SLOT is not POINTER_SWAP, although both store one pointer:
// POINTER_SWAP's slot lives in a GOT or an import table, on a page the loader
// has already decided the protection of, and both patchers restore read-only
// afterwards because for those pages that is the right answer. A vtable can be
// in a module's read-only data, in ordinary writable data, or on the heap when
// the object was built at run time. DATA_SLOT asks what the protection is and
// puts that back, which is the only difference between them -- and the reason
// the comment above this enum used to list "vtable slot" under POINTER_SWAP
// and was wrong to.

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

// Opt-in x86_64 exact-site, strict-write install (not safe live installation).
// Requires exactly 16 readable bytes at target and expected_bytes, with a
// matching snapshot. No jump unwrapping or GOT/IAT substitution. Unsupported
// prologues, including branch/jump stubs, are refused; this is not a detector
// for all third-party hooks. Other architectures refuse without side effects.
// Caller must exclude invocation and all mutation throughout this operation.
// Target must be executable code on ordinary RX pages; the current OS layer
// restores RX, not arbitrary original page protection flags.
// pre_cb must be nonthrowing, nonmutating and must not reenter install/disable.
// Only a five-byte rel32 patch fitting one aligned quadword is allowed; no
// ordinary-copy fallback or padding writes. Success records INLINE at target.
// Refusal returns null, leaves original_func and out_record unchanged, and
// never calls pre_cb. Repeated installs are refused, including our own hooks.
void* splice_hook_address_strict_pre_rec(void* target_addr, void* new_func,
                                         void** original_func,
                                         splice_pre_patch_fn pre_cb, void* pre_user,
                                         const unsigned char* expected_bytes,
                                         unsigned int expected_size,
                                         splice_patch_record* out_record);

// ─── What a coordinated install would do, asked before doing it ───────────

// Facts about the prologue an install would replace, decoded and not written.
//
// Why facts and not a verdict: a consumer's admission rule is the consumer's.
// The shipping overlay refuses to patch its two hottest targets unless the
// patch window is a single instruction, which is stricter than this library
// requires, and it records these numbers as evidence when it refuses. A
// boolean would serve the rule and not the evidence, and moving the rule in
// here would move a product safety decision into the wrong repository.
typedef struct {
    // Bytes the patch would replace: whole instructions covering the patch
    // length. Five, unless a prologue's first instruction is longer.
    unsigned int patch_bytes;
    // Length of the instruction at the entry.
    unsigned int first_instruction;
    // Instruction starts lying strictly inside the patch window. Zero means
    // the window is exactly one instruction, so no thread can be *inside* the
    // bytes being replaced -- it is either at the entry or past all of them.
    unsigned int interior_boundaries;
    // Where they are: bit N is set when an instruction starts at offset N
    // inside the window. Bit 0 is never set, because the entry is not an
    // interior boundary. A count answers an admission rule; the positions are
    // what gets written into a refusal's evidence, and a consumer that had
    // both before should not lose one of them to a nicer API.
    unsigned int interior_mask;
} splice_prologue_facts;

// Fills `out` and returns 0 when the prologue at `target` decodes; returns -1
// and leaves `out` untouched when it does not, when the architecture has no
// decoder compiled, or when `target` does not name readable memory.
//
// Reads bytes and writes nothing. A jump thunk is followed first, as every
// install path does, so the facts describe the function and not the thunk in
// front of it.
//
// The patch length measured against is five bytes, which is what would
// actually be written: a relay is allocated beside every target, so the
// jump's destination is always within rel32 and the fourteen-byte absolute
// form does not arise.
int splice_prologue_profile(const void* target, splice_prologue_facts* out);

// Resolve a library symbol to its address, and nothing else.
//
// The install entry points above resolve and patch in one call, which is all a
// single hook ever needed. A coordinated group needs the two separated: every
// target has to be known before `splice_live_prepare` can allocate for it, and
// prepare is the phase that may not be re-entered once anything is frozen.
//
// `lib_name` must name a loaded or loadable module and `symbol_name` an
// exported symbol in it. Returns null when either cannot be found, having
// written nothing and loaded nothing it did not already need. The returned
// address is raw: a jump thunk is still a jump thunk here, because unwrapping
// one is the install path's decision and depends on the tier.
void* splice_resolve_symbol(const char* lib_name, const char* symbol_name);

// ─── Vtable hooking (Tier 1 on an interface) ──────────────────────────────

// The vtable slot index a pointer-to-virtual-member-function names, or -1 when
// it cannot be resolved.
//
// `pmf_bytes` is the raw object representation of the pointer-to-member and
// `pmf_size` its sizeof. Passing the bytes rather than the pointer is what
// keeps this a C ABI: the shape of a pointer-to-member is a C++ ABI detail,
// and which detail depends on the compiler, the architecture and the class's
// inheritance -- so the decision lives on one side of this boundary and the
// answer, an index, crosses it.
//
// Refuses (-1) for a non-virtual member, an inheritance shape needing a
// this-adjustment, an unrecognised thunk, or an architecture whose ABI the
// engine has not been taught. A refusal is not an error to retry; the caller
// names the index by hand instead, with the signature still compile-checked.
//
// Pass only the bytes of a real pointer-to-member. Under the Microsoft ABI
// those bytes ARE a code address and this function reads the instructions
// there, because that is the only place the slot number exists; handing it an
// arbitrary value reads arbitrary memory. The Itanium ABI side reads nothing
// but the bytes given.
int splice_vtable_slot_of_pmf(const void* pmf_bytes, unsigned int pmf_size);

// Store `new_func` into `*slot`, reporting the previous value through
// `out_previous` and filling `out_record` (either may be null) so the swap can
// be reversed with splice_disable.
//
// The store is a single release store. The caller must publish everything a
// hook needs -- the original function pointer and the callback -- BEFORE
// calling, because a reader on another core can observe the new slot value as
// soon as it lands. Publishing in the other order is a live call through a
// null original.
//
// Returns 0 on success, -1 when the slot could not be made writable or the
// arguments are unusable. On -1 nothing was written.
int splice_vtable_swap(void** slot, void* new_func, void** out_previous,
                       splice_patch_record* out_record);

// ─── The coordinated tier: a group of inline patches, one suspension ──────
//
// Two calls, and the split is the contract. `prepare` does every allocation,
// opens a reference to every thread the OS names and makes every target
// writable; `commit`
// suspends, writes and resumes. In between, the caller publishes whatever a
// reader will need behind the new code -- the original function pointer and
// the callback -- because doing that inside the suspension would mean taking a
// lock while another thread is suspended, which is how a process stops.
//
// Why a group at all: installing N sites one at a time means N suspensions of
// every enlisted thread, N thread enumerations at about 118 ms each, and no way
// back if
// the fifth of seven fails. One transaction writes all of them or none.

// How many bytes `splice_live_site::expected` must point at. Fixed by the
// engine's patch window; a shorter buffer is read past.
#define SPLICE_LIVE_EXPECTED_BYTES 16u

typedef struct {
    void* target;        // the function whose prologue is replaced
    void* replacement;   // where it should jump: the caller's stub
    // 16 bytes the caller believes are at `target`, or NULL. When given and
    // the target does not match, prepare refuses. Supply it when you have an
    // independent expectation -- it is the only check that can notice someone
    // else has already patched the function.
    const unsigned char* expected;
} splice_live_site;

typedef enum {
    SPLICE_LIVE_INSTALLED = 0,   // every site written, every thread resumed
    SPLICE_LIVE_REFUSED   = 1,   // nothing written; threads resumed
    SPLICE_LIVE_RECOVERED = 2,   // the attempt was undone before resuming
    SPLICE_LIVE_LOST      = 3,   // the world could not be restored
} splice_live_state;

// Where it stopped. `complete` never appears in a failure.
typedef enum {
    SPLICE_LIVE_STAGE_PREPARE  = 0,
    SPLICE_LIVE_STAGE_ENLIST   = 1,
    SPLICE_LIVE_STAGE_MAP      = 2,
    SPLICE_LIVE_STAGE_MIGRATE  = 3,
    SPLICE_LIVE_STAGE_WRITE    = 4,
    SPLICE_LIVE_STAGE_FLUSH    = 5,
    SPLICE_LIVE_STAGE_RESUME   = 6,
    SPLICE_LIVE_STAGE_COMPLETE = 7,
} splice_live_stage;

// What was being attempted when it stopped.
typedef enum {
    SPLICE_LIVE_OP_NONE          = 0,
    SPLICE_LIVE_OP_PREPARE_SITE  = 1,
    SPLICE_LIVE_OP_ENUMERATE     = 2,
    SPLICE_LIVE_OP_SUSPEND       = 3,
    SPLICE_LIVE_OP_CAPTURE_PC    = 4,
    SPLICE_LIVE_OP_SET_PC        = 5,
    SPLICE_LIVE_OP_WRITE_SITE    = 6,
    SPLICE_LIVE_OP_RESTORE_SITE  = 7,
    SPLICE_LIVE_OP_FLUSH_SITE    = 8,
    SPLICE_LIVE_OP_RESUME_THREAD = 9,
} splice_live_operation;

// Plain old data, filled by splice, rich enough that nobody has to wrap the
// seam to find out what happened.
typedef struct {
    int          state;       // splice_live_state
    unsigned int site;        // which member of the group, counted from zero
    int          stage;       // splice_live_stage
    int          operation;   // splice_live_operation
    unsigned int thread_id;   // on whom; 0 when not thread-specific
    unsigned int os_error;    // GetLastError()/errno at the failure point
    // How many threads the group enlisted, once it got as far as enumerating
    // them; zero before that. Reported because it is evidence a caller cannot
    // obtain any other way and may need afterwards: "the attach refused and
    // there were a hundred and forty-three threads" is a different situation
    // from the same refusal with four, and the engine is the only thing that
    // counted.
    unsigned int threads;
} splice_live_outcome;

// Plan every site. Allocates trampolines, snapshots prologues, builds the
// instruction-boundary maps, makes the targets writable and opens a reference
// to every other thread the OS names at this instant. **Nothing is written to
// any target.** The enumeration happens once, here; a thread created after it
// is not in the set and is not detected, which is the limit the coordinated
// tier is named `live` rather than `safe` for.
//
// `originals_out`, when given, receives `count` pointers: each one calls the
// original behaviour of the matching site, and is what the caller publishes
// before committing.
//
// Returns an opaque group handle, or NULL with `out` saying why. A NULL return
// means every target is untouched. The handle must be released exactly once
// with splice_live_release, whether or not commit is called or succeeds.
// Non-zero when this build can run the coordinated tier at all: it needs an
// architecture whose prologue can be planned and an OS that can suspend a
// thread and move its program counter. Ask before offering the tier to a user,
// rather than discovering it from a refusal.
int splice_live_supported(void);

void* splice_live_prepare(const splice_live_site* sites, unsigned int count,
                          void** originals_out, splice_live_outcome* out);

// Suspend every thread prepare enlisted, map any program counter that sits
// inside the bytes being replaced, write every site, flush, resume. One attempt
// per group.
//
// The set is the one prepare captured, not whatever the process holds now. A
// thread that appeared since is neither suspended nor reported.
//
// `records_out`, when given and the outcome is installed, receives `count`
// patch records so each site can later be reversed with splice_disable.
//
// On `refused` nothing was written. On `recovered` the writes were undone
// before any thread resumed. On `lost` the threads are **still suspended** and
// the module must not be unloaded: that is what the word means, and it is the
// one outcome the caller genuinely has to decide something about.
void splice_live_commit(void* group, splice_patch_record* records_out,
                        splice_live_outcome* out);

// Release the group's bookkeeping: the thread references, and the page
// protection prepare changed. Does **not** uninstall anything, and never frees
// a trampoline -- a group that failed has retained its trampolines because a
// thread may have been migrated into one, and a group that succeeded is using
// them.
void splice_live_release(void* group);

// Disable a previously-installed hook described by `record`.
// Returns:
//    0 on success
//   -1 on failure (record invalid, slot value didn't match expected hook,
//                  arch operation rejected, etc.)
//   -2 is reserved and no longer returned. It once meant "tier not yet
//      implemented", naming INLINE / Tier 2 -- which has been implemented since
//      Phase 4.5c-2. Kept documented rather than deleted because it is part of a
//      published C ABI and a caller may still test for it.
//
// After successful return, the patch is reversed:
//   - POINTER_SWAP: the slot holds `pre_hook_pointer` again, written with a
//                   release store (see iat_patcher.cpp / got_patcher.cpp).
//   - DATA_SLOT   : the same, through the protection-preserving path the
//                   install used (see slot_patcher.cpp).
//   - INLINE      : the prologue holds the original bytes again, restored with
//                   one atomic access to the word containing the patch.
//                   Trampoline memory remains allocated -- permanently, and by
//                   design: nothing in-process can prove no thread is still
//                   executing inside it. See FR-013 Tier 3.
//
// What "reversed" does NOT mean, on either strategy: calls already in flight are
// not cancelled. A thread inside the hook, or inside the trampoline, or holding
// the old pointer it already loaded, carries on. Disable is a switch, not a
// drain.
int splice_disable(const splice_patch_record* record);

// Probe helper — returns non-zero if `addr` looks like it was patched by
// Splice (matches our trampoline signature). Not a security check; purely
// a debugging aid.
//
// It answers "is this address patched", NOT "has the hook run". The two come
// apart, and the gap is a trap worth naming: a compiler that can see the
// target's body may prove it pure and replace the call with its result, so the
// patch is installed and correct while no call site reaches it any more. The
// symptom is this function returning non-zero beside a callback that never
// fires, which reads like a Splice defect and is not one.
//
// To answer the other question, use SPLICE_COUNT / SPLICE_COUNT_ADDR — they
// report on the first hit as well as periodically, so a hook that fires once
// says so.
int splice_is_hooked(const void* addr);

// Diagnostic: log the first 8 instructions starting at `func_addr` via
// SPLICE_LOG*. Useful when a hook "silently fails" to attach.
void splice_debug_function_start(const void* func_addr, const char* label);

#ifdef __cplusplus
} // extern "C"
#endif
