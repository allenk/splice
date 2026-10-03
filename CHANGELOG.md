# Changelog

All notable changes to Splice are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versioning follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Everything below landed after `1.0.0` and went unrecorded until 2026-09-28.
That gap is itself worth noting: `.observe()` is a **new public API**, and
Splice treats the `splice::` surface as ABI-stable after Phase 1, so an
unrecorded addition is exactly what this file is for.

All of it came out of Splice's first consumer outside its own tests — the
GpuThermalGuard game overlay, hooking `IDXGISwapChain::Present` in `dxgi.dll`.
Known gaps that work surfaced and did **not** close are listed at the end of
this section, under *Not fixed — recorded instead*.

### Added
- Internal x86_64 `PreparedStrictPatch`: prepare without target mutation,
  instruction-boundary IP mapping, and an explicit caller-coordinated write.
  This is not a public live-install API and does not enumerate/suspend threads.
  Existing strict/legacy entry points retain their contracts.
- **`.observe()` / `invocations()`** on `InterceptorEntry` — count how many times
  a patched function was actually entered, readable afterwards. Exists because
  `is_installed()` and `splice_is_hooked()` answer a different question: they
  report that an address is patched, not that any call site still reaches it.
  The two come apart when a compiler can see the target's body, proves it pure
  and folds the call away — which reads like a library defect. Counts on entry,
  ahead of `.when()` and `.times()`: a call a gate turns away still arrived.
  Off by default; one relaxed increment when asked for.

### Changed
- RIP-relative forms with trailing immediates now fail closed instead of
  corrupting their immediate with a misplaced displacement fixup. Supporting
  them requires exact displacement-field metadata, not a length-minus-four guess.
- **x86_64 installs are now atomic by arrangement rather than by luck.** The
  5-byte `E9 rel32` patch reaches ±2 GB, and `new_func` lives in the consumer's
  module wherever that loaded — routinely further. Splice now allocates the
  trampoline near the target and puts a 14-byte **relay** beside it, so the live
  prologue only ever takes 5 bytes inside one aligned quadword. Measured against
  `dxgi.dll` with the hook 28.7 GB away: still `atomic, via relay`, 240/240
  presents intercepted windowed and 239/239 at 3840×2160 on hardware.
- **`splice::os::allocate_executable_memory` takes an optional `near_addr`.**
  Default `nullptr` preserves the previous behaviour for every existing caller.
  Win32 walks free regions with `VirtualQuery`; POSIX runs a bounded
  `MAP_FIXED_NOREPLACE` probe and range-checks every result. Falls back to an
  OS-chosen address, so callers must still cope with a far result.
- **The prologue walk now stops at the patch length instead of always 16 bytes.**
  `calculate_copy_size` gained a `min_bytes` argument, defaulting to the old 16.
  Since the relay guarantees a 5-byte patch, the x86_64 patcher passes 5.
  Observed prologues dropped from 16–19 bytes to 5–9, and of 69 installs in one
  suite run, 59 now disturb **zero** bytes of the live target beyond the single
  atomic store.
- **Diagnostics report the first hit, not only every 65536th.** `SPLICE_COUNT`
  and `SPLICE_TIME` were silent until a hook had fired 65 536 times, which for
  anything but a hot loop meant silent.

### Fixed
- **`splice_disable` is available on ordinary functions again.** The 16-byte walk
  made an MSVC entry sequence round up to 17 or 19 bytes and overflow the
  16-byte record `splice_disable` restores from, so Tier 2 came back
  unavailable. 24 of the installs in Splice's own suite hit it, `Present` among
  them at 19 bytes. Now zero.
- **`unwrap_jump_stub` no longer follows Splice's own patches.** It exists to
  skip MSVC ILT stubs and import thunks and follows `E9` and `FF 25`, which is
  indistinguishable from an installed patch — so a second install walked
  `patched fn → relay → hook stub` and patched whatever it landed on, observed
  patching Splice's own relay. The engine now keeps a registry of live patch
  sites and stops at one; a byte test such as `splice_is_hooked()` cannot make
  this distinction and is not used for it.
- **A second, different hook on an already-patched address is refused instead of
  corrupting the first.** Stacking did not chain: the second trampoline
  relocated the installed `E9` back to the first hook's relay, so hook one's
  `original` re-entered hook one — infinite recursion, `0xC00000FD`. Chaining
  deliberately is still wanted; see *Not fixed* below.
- **A repeat install of the same hook is a silent no-op.** `install_all()` re-runs
  every live installer on each call, so this is routine; it previously either
  re-patched an already-patched prologue or, after the refusal above, logged a
  failure. Recognised by destination: same `new_func` returns the existing site,
  a different one is refused.

### Fixed (continued — the same integration, later the same day)
- **The atomic install precondition was one level too strict.** It required the
  hook target itself to be 8-byte aligned; the hardware rule is that an *aligned
  quadword* access is atomic, so what must fit in one is the 5 patch bytes:
  `target & 7 <= 3`. MSVC aligns entries to 16 bytes so Windows never saw it;
  under GCC only 4 of 13 targets in Splice's own Linux suite were aligned, and
  10 of 14 installs took the torn-state path for no reason. Now 10 of 14 are
  atomic. Install and disable both write through the containing quadword with a
  compare-exchange, because the untouched bytes of that word may belong to a
  tightly packed neighbour.
- **Tier 1 (GOT/IAT) was not actually atomic.** `*entry = new_func;` with a
  comment asserting atomicity, and `core.h` promising it. No ordering either, so
  a reader could observe the new pointer before the `OriginalRegistry` write
  that makes it usable. Invisible on TSO x86_64; a real hole on weakly-ordered
  ARM64. Now `std::atomic_ref` with `memory_order_release`; identical codegen on
  x86_64.
- **`unwrap_jump_stub` no longer follows Splice's own patches**, and a second
  distinct hook on an already-patched address is refused rather than producing
  infinite recursion. A repeat install of the *same* hook is a silent no-op.
 
- **Two public headers corrected.** `engine.h` documented `-2` as "tier not yet
  implemented (currently INLINE / Tier 2)" and the INLINE restore as "(when
  implemented)"; Tier 2 has shipped since Phase 4.5c-2 and `-2` is never
  returned. `core.h`'s `disable()` promised Tier 1 atomicity the code did not
  provide. Both now describe the code, and both state what disable does *not*
  do: calls already in flight are not cancelled.

### Fixed — ARM64 (2026-09-29)
- **ARM64 now allocates the trampoline near the target.** The copied prologue is
  relocated, and AArch64's relocation budgets are narrow: `B`/`BL` ±128 MB (with
  a fallback), `ADRP` ±4 GB and `ADR` ±1 MB (no fallback — the install is
  refused). Without a hint the trampoline was measured 101.8–166.8 GB from a
  function in the executable on Android 14, and 85.33 TB on aarch64 Ubuntu —
  never once inside `ADRP` range — so any such function with an `ADRP` in its
  first four instructions could not be hooked inline. With the hint the
  trampoline lands about 1 MB away and `ADRP` and `B` are both satisfied.
  Verified on device by A/B: with the hint removed, installing over a real
  `ADRP` prologue fails; with it, the hook is entered and `disable()` restores
  the prologue.
- Measured alongside: a function in a **shared library** was already within
  `ADRP` and `B` range without any hint (29–86 MB on Android), because anonymous
  mappings and shared libraries come from the same region while the executable
  sits at a different base.

### Not fixed — recorded instead
- **`ADR` in a copied ARM64 prologue.** Its ±1 MB reach is not delivered by
  near-allocation and cannot be: for a target inside a large module the nearest
  free page lies past the module's own pages, and beside a shared library it was
  obtainable in only 3 of 13 measured attempts. The fix is to materialise the
  address absolutely (`movz` + three `movk`) instead of relocating the `ADR`,
  which is not yet implemented, so such prologues are still refused.
- **Chaining onto a patch somebody *else* installed** — Steam Overlay in the same
  slot — is still not supported. Splice refuses cleanly rather than corrupting
  the existing patch.

## [1.0.0] - 2026-06-03

First production release. Splice is a type-safe, fluent, cross-platform C++20
function hooking library productized from the `the predecessor framework` framework
(the engine behind an Android game enhancer).

**Highlights**
- Cross-platform: Android ARM64 (production, Snapdragon 8 Gen 3 verified),
  Linux ARM64/x86_64, Windows x64. GOT/PLT (ELF) + IAT (PE) + inline patching.
- Fluent API: `SPLICE_HOOK(fn).onInvoke(...)` plus `.before`/`.after`/
  `.when`/`.once`/`.times`, `SPLICE_HOOK_MEMBER`, and `SPLICE_TRACE`/`_COUNT`/
  `_TIME` diagnostics.
- Atomic install + tiered `disable()` (Tier 1 pointer-swap, Tier 2 inline
  atomic restore). No false promise of in-process uninstall.
- Performance: 22 ns/call single-thread (down from 41), opt-in RCU registry
  drops 8-thread reader contention from ~50× to ~4×.
- `find_package(splice CONFIG)` + `splice::splice` install/export.
- 132 unit tests (Windows) / 131 + 1 skip (ARM64), ASan/UBSan/TSan clean.

### Added (Phase 6 — Friendly Fluent API v2, FR-009)
- **Modifiers** on `InterceptorEntry`: `.before(lambda)` (pre-call, no
  `orig`), `.after(lambda)` (post-call, sees return + args; void-return
  aware via `if constexpr`).
- **Composable gates**: `.when(pred)` (runtime condition — false routes
  straight to the original), `.once()`, `.times(n)` (atomic budget shared
  with the wrapper). Order-independent — `.when().once()` ≡ `.once().when()`.
- **`SPLICE_HOOK_MEMBER(Class::method)`** — `member_function_traits` maps
  `Ret(Class::*)(Args...) [cv][noexcept]` → explicit-`this`
  `Ret(*)(Class*, Args...)`; `detail::member_code_addr` extracts the PMF
  code address. Non-virtual only (a virtual PMF is a vtable offset).
- **Diagnostic sugar** in `<splice/diagnostics.h>`: `SPLICE_TRACE` /
  `SPLICE_COUNT` / `SPLICE_TIME` (+`_LIB`/`_ADDR` variants), coarse periodic
  dump.
- **`splice::ScopedHook`** — type-erased move-only RAII auto-disable;
  supports heterogeneous `std::vector<ScopedHook>`.
- `splice::Group` deferred to v1.1.

### Added (Phase 3.5 / Step 6 — Performance pass, FR-010)
- **Policy framework**: `SPLICE_DEFAULT_POLICY` selects callback storage —
  `policy::rcu_writeonce` (default, single atomic-load reader) or
  `policy::shared_mutex` (runtime-swappable). Per-call-site override via
  `SPLICE_HOOK_AS(Policy, ...)`.
- **Registry framework**: `SPLICE_REGISTRY_IMPL` selects the hook/originals
  lookup table — `registry::shared_mutex_map` (default) or
  `registry::rcu_atomic_array` (lock-free reader, time-deferred reclamation,
  `SPLICE_MAX_HOOKS`=512). Registry-isolated 8t/1t: ~50× → 3.9–5.5×.
- `recursive_mutex` → `shared_mutex` on the hot path; `std::map` →
  `unordered_map`. Single-thread hooked call 41.1 → 22.0 ns.
- **Cross-TU `__COUNTER__` collision fix**: `SPLICE_UNIQUE_ID =
  __LINE__ << 16 | __COUNTER__` + runtime `HookContext::slot_for(trampoline)`.
- Google Benchmark microbench harness (`bench_hook_overhead`,
  `bench_hook_contended`, `bench_registry_lookup`, `bench_callback_storage`).
  Step 5 (`std::function` → thunk) measured at only ~1 ns gain and
  intentionally skipped — see `docs/fr-010-step5-microbench-report.md`.

### Added (Task #57 — installer-queue lifetime)
- `splice::InstallerToken` — move-only RAII handle into the installer queue
  (slot + free-list + generation counter). `~InterceptorEntry` deregisters
  its installer; `HookContext::reset()` invalidates outstanding tokens in
  O(1). Kills the pre-1.0 dangling-`[this]`-lambda hazard.

### Added (Phase 4.5c-2 — Tier 2 inline disable, FR-013)
- Install snapshots the original prologue into
  `splice_patch_record::pre_hook_bytes`; `disable()` replays it atomically
  via `x86_64::atomic_disable_inline` (aligned 8-byte qword store) or
  `arm64::atomic_disable_indirect_branch` (reverse install order). Trampoline
  memory stays allocated forever per the documented FR-013 limitation.
  Verified round-trip on Windows x86_64 and Snapdragon 8 Gen 3.

### Added (Phase 4.5c-1 — Tier 1 disable, FR-013)

First half of the tiered `disable()` semantics. Phase 4.5c-2 (Tier 2
inline disable) deferred.

- **New C ABI in `<splice/engine.h>`**:
  - `splice_patch_strategy` enum (POINTER_SWAP / INLINE)
  - `splice_patch_record` opaque-ish POD struct (strategy, hook_site,
    pre_hook_pointer, trampoline, pre_hook_bytes[16])
  - `splice_hook_address_pre_rec()` / `splice_hook_symbol_pre_rec()` —
    install variants that fill the record
  - `splice_disable(record)` — return-coded reverse of an install
- **Engine populates the record**:
  - GOT/IAT path → `strategy = POINTER_SWAP`, slot + pre-hook pointer
  - Inline path → `strategy = INLINE` (record filled but disable returns
    `-2` "not yet implemented" until 4.5c-2)
- **`InterceptorEntry` exposes `disable()`**:
  - Stores the record on successful install
  - `entry.disable()` calls `splice_disable(&record)` and updates
    `m_installed`. `is_installed()` now reflects the live state.
- **Tier 1 disable is atomic**: pointer-sized write back to the slot
  via `patch_got_entry` / `patch_iat_entry` (same code paths used for
  install). x86 / ARMv8 both guarantee aligned pointer-sized writes are
  single-copy-atomic, so no DMB / IC IVAU dance needed for this tier.
- **Verified on `windows-x64-dev`** — IAT disable round-trip:
  ```
  Hooked via IAT at 00007FF6B225F130
  Disabled (POINTER_SWAP) at 00007FF6B225F130
  ```
  After `disable()`: import calls return real ticks, hook callback no
  longer invoked. 96/96 unit tests green.

### Added (Phase 4.5d — multi-thread stress validation + install-order fix, FR-011)

The stress test caught a real correctness bug in the install pipeline: the
`OriginalRegistry::set_original` call lived **after** `splice_hook_address`
returned, while the atomic patch had already landed. A worker thread
observing the new patched prologue could enter the C++ trampoline,
read a still-null `original` pointer from `OriginalRegistry`, and call
`null(...)` from the user callback → SIGSEGV. Reproducible at 16 threads
within milliseconds of install on Windows x86_64.

**Fix** — pre-patch callback wired through the entire install path:
- New C ABI: `splice_hook_address_pre()` and `splice_hook_symbol_pre()`
  in `<splice/engine.h>`, accepting `splice_pre_patch_fn pre_cb` +
  `void* user_data`. Legacy `splice_hook_address()` /
  `splice_hook_symbol()` delegate with null callback.
- `arch::arm64::install_inline_patch` and `arch::x86_64::install_inline_patch`
  take the callback and invoke it AFTER trampoline setup but BEFORE the
  atomic patch lands.
- `engine::hook_by_address` fires the callback in the GOT/IAT path too,
  passing the original slot value so the registry is set before the
  pointer swap commits.
- `InterceptorEntry::install` and `InterceptorEntry::install_direct`
  use the new variants with a static C-compatible callback that updates
  `OriginalRegistry` from the trampoline pointer the patcher hands over.
  No more post-patch update window.

**Validation** — multi-thread stress test (`tests/stress_test.cpp`,
new isolated `splice_stress_test` binary):
- Windows x86_64: 4,719,185 invocations across 16 threads during
  install, 0 bad results, 0 crashes.
- Android ARM64 (Snapdragon 8 Gen 3, Pineapple): 5,875,368 invocations
  across 16 threads during install, 0 bad results, 0 SIGILL.
- Test isolated in its own binary so the global default_context() starts
  clean (avoids the `test_core` / `test_registry` installer-queue
  pollution still tracked as task #57).

This closes the FR-011 atomicity story end-to-end. The 5-byte / 16-byte
atomic install paths (Phases 4.5a + 4.5b) are now validated under real
contention, and the install-ordering race that was hiding underneath
those phases is fixed.

### Added (Phase 4.5b — x86_64 atomic patching, FR-011)
- `src/arch/x86_64/atomic_patch.{h,cpp}` — aligned 8-byte atomic store
  for the 5-byte `E9 rel32` install path. Per Intel SDM Vol 3A §8.1.1
  ("Guaranteed Atomic Operations"), aligned 8-byte stores are atomic
  with respect to instruction fetch on all modern x86_64 CPUs. Builds
  the new 8-byte word as `[E9, rel32(4 bytes), original-byte-5,
  original-byte-6, original-byte-7]` so bytes 5..7 are preserved (they
  become unreachable after the jump redirect, but preserving them avoids
  ever exposing torn middle state).
- `x86_64::install_inline_patch` updated to try `atomic_install_jmp_rel32`
  first; falls back to non-atomic memcpy with `SPLICE_LOGW` only when:
    - target is not 8-byte aligned (function entries usually are
      16-byte aligned by default, so this rarely triggers), or
    - displacement(new_func − (target + 5)) doesn't fit in int32 (the
      14-byte abs64 fallback case)
- **Verified on `windows-x64-dev`** — LiveHook test passes via the atomic
  path (`ok (atomic)` log line); 95/95 unit tests still green; clean
  `/W4 /permissive-` build.
- Note: the 14-byte abs64 fallback case remains non-atomic. A proper fix
  for that path requires either thread-suspension (`SuspendThread` +
  IP inspection) or a thunk-based 5-byte-jump-to-abs64-stub design.
  Deferred to a later iteration; in practice trampolines stay within
  ±2 GB so the 14-byte path is rarely triggered.

### Added (Phase 4.5a — ARM64 atomic patching, FR-011)
- `src/arch/arm64/atomic_patch.{h,cpp}` — architecturally-correct ARMv8
  install sequence per ARMv8-A ARM §B2.2.1 ("Single-copy atomicity") and
  the Android ART runtime's concurrent-modify pattern. Replaces the
  unsafe Phase 1 `memcpy(target, hook_jump, 16)` with:
  ```
  1. write literal at [target+8..15]            (data)
  2. write `br x17` at [target+4..7]            (instruction, unreachable)
  3. dmb ish                                    (data memory barrier)
  4. atomic 32-bit write of `ldr x17, #8`        (THE moment)
  5. dc cvau + dsb ish + ic ivau + dsb ish + isb (I-cache maintenance)
  ```
  Step 4 is the only observable transition; ARMv8 guarantees aligned
  4-byte instruction-word writes are atomic with respect to instruction
  fetch. The DMB before step 4 makes steps 1+2 globally visible; the IC
  IVAU after step 4 invalidates other CPUs' I-cache lines so they refetch.
- `arm64/patcher.cpp::install_inline_patch` updated to call
  `atomic_install_indirect_branch()` instead of `memcpy`.
- **Verified on Snapdragon 8 Gen 3 / Android 14** — LiveHook test passes
  end-to-end via the atomic sequence; logcat confirms `atomic_install`
  trace fires correctly; no SIGILL, no silent corruption.
- Multi-thread stress validation (TSan + 100-thread invoke during
  install) deferred to Phase 4.5d.
- x86_64 atomic patching deferred to Phase 4.5b — Intel's instruction
  fetch is more forgiving for our 5-byte E9 patch in practice.

### Validated on real ARM64 hardware (2026-04-26)
First-ever execution of the entire Splice ARM64 path on real hardware:
**Snapdragon 8 Gen 3 (Pineapple) running Android 14 / arm64-v8a / kernel 6.1.25**.
Pushed via `adb push` to `/data/local/tmp/splice/`, ran with adb root.

Results:
- **smoke_test: 7/7 pass** — logging substrate, hot-path variants, hex dump,
  runtime level toggle, breadcrumb all work via `__android_log_print`.
- **unit_test: 84/88 pass on ARM64** — every Splice library path verified:
  function_traits, registry, trampoline, HookContext, ARM64 disasm (26 vectors),
  x86_64 disasm (23 vectors), context isolation, shim compat, fluent API
  compile-paths.
- **`LiveHook.splice_hook_addr_intercepts_free_function` PASSED** — the
  end-to-end ARM64 inline-patch pipeline executes correctly:
  ```
  install_inline_patch: target=0x591ef92e20 new_func=0x591ef69900
  calculate_copy_size: 16
  allocate_executable_memory: 4096 bytes at 0x704ba37000
  Trampoline at 0x704ba37000 size=32
  install_inline_patch: ok
  Hook installed (direct): 0x591ef92e20 => 0x591ef69900 (id=0)
  ```
  Confirms: mmap RWX is allowed on Android 14 /data/local/tmp,
  `mprotect` flips RX↔RW, ARM64 prologue analysis (verbatim from
  the predecessor framework.h v3.0) is correct, 16-byte indirect branch
  patcher works, `__builtin___clear_cache` succeeds, HookManager
  dispatch routes through trampoline correctly.
- **CMake**: PLOG_LEVEL forced to 0 (Verbose) for any Debug config —
  NDK Clang doesn't auto-define `_DEBUG` like MSVC does, so without
  this Android Debug builds silently drop SPLICE_LOGV.

Known test-design issue (NOT a Splice library bug — filed as task):
- `test_core::InterceptorEntry::*` and `test_registry::GlobalInstallers`
  capture stack-local objects (`[this]`, `[&counter]`) into installer
  lambdas pushed onto the global queue. When tests end, references dangle.
  Subsequent tests calling `splice::install_all()` dereference freed
  stack → stack-protector trips. Latent on Windows (different memory
  reuse). Fix scheduled: per-test HookContext fixture with `reset()`.

### Added (Android cross-compile via NDK)
- `CMakePresets.json` — `android-arm64-dev` / `android-arm64-release`
  presets chained through the NDK toolchain via vcpkg
  (`VCPKG_CHAINLOAD_TOOLCHAIN_FILE`). API 29, ABI `arm64-v8a`, STL
  `c++_static`, generator Ninja. Adapted from GeminiWatermarkTool's
  preset convention.
- `tests/CMakeLists.txt` — `gtest_discover_tests` gated by
  `NOT CMAKE_CROSSCOMPILING` so the host doesn't try to launch an
  aarch64 ELF for case enumeration. Cross-compiled binaries register
  a single info-print ctest that points at the deploy command.
- `scripts/adb-run-tests.sh` + `.ps1` — push the test binaries to
  `/data/local/tmp/splice/` and run them on a connected device.
- `target_link_libraries(splice PUBLIC log)` on Android — `platform_log.h`
  routes through `__android_log_print`, which lives in `liblog.so` and
  has to be linked explicitly.
- The README's Android section — the full cross-compile and on-device
  workflow, including `adb logcat -s splice` for live log capture.

### Added (Phase 4b — PE IAT patching, FR-005)
- `src/engine/iat_patcher.{h,cpp}` — Windows Tier-1 hook strategy.
  `find_iat_entry_for_address()` walks every loaded module via
  `EnumProcessModules`, parses `IMAGE_IMPORT_DESCRIPTOR` → `FirstThunk`
  → `IMAGE_THUNK_DATA64[]`, and returns the first IAT slot whose value
  matches the target. `patch_iat_entry()` performs an atomic
  pointer-sized write under a transient `VirtualProtect(PAGE_READWRITE)`
  flip. Links against `psapi.lib` for module enumeration.
- `src/engine/engine.cpp` — IAT lookup now wired into `hook_by_address`
  as the Win32 equivalent of the POSIX GOT path. Strategy order:
  unwrap ILT stubs → IAT lookup (Tier 1, safe) → fall through to
  inline instruction patch (Tier 2, trampoline leak).
- `unwrap_jump_stub()` extended to loop (depth-capped at 4) so the
  MSVC debug chain `ILT E9 → import thunk FF 25 → real function` is
  fully resolved before IAT lookup.
- `tests/test_hook_iat.cpp` — hooks kernel32's `GetTickCount` via
  `SPLICE_HOOK_ADDR_STATIC`, verifies the engine picks the IAT path,
  and confirms that imported calls from our module now resolve to the
  hook while direct kernel32 invocations remain untouched.
- **95/95** tests pass on `windows-x64-dev`. Splice now offers both
  tiers of Windows hook semantics documented in FR-013.

### Added (Phase 4a — Windows OS layer + live x86_64 hooks, FR-005)
- `src/os/memory.h` promoted out of `posix/` — shared interface for both
  POSIX and Win32 OS implementations. Arch patchers use the unified
  `splice::os::*` namespace regardless of platform.
- `src/os/win32/memory.cpp` — `VirtualAlloc`/`VirtualFree`,
  `VirtualProtect`, `FlushInstructionCache`, `GetSystemInfo` for page
  granularity. Mirrors the POSIX layer's API surface 1:1.
- `src/engine/engine.cpp` refactored cross-platform. `resolve_symbol()`
  dispatches between POSIX `dlopen`+`dlsym` and Win32
  `GetModuleHandle`+`LoadLibrary`+`GetProcAddress`. GOT patching stays
  POSIX-only (Phase 4b adds IAT).
- **`unwrap_jump_stub()`** — follows one level of ILT indirection
  (MSVC `E9 rel32` debug stubs, `FF 25 + disp32` DLL-import stubs)
  before measuring the prologue. Without it, the patcher would try
  to relocate the stub's rel32 into a faraway trampoline and fail.
- CMake: `SPLICE_HAS_ENGINE` now gates the full engine on
  `(POSIX OR Win32) AND (ARM64 OR x86_64)`; `SPLICE_HAS_WIN32_OS=1`
  propagated to the compilation. Stub TU still covers unsupported tuples.
- `tests/test_hook_global.cpp` — live integration test that installs a
  hook on `splice::test::add_one` via `SPLICE_HOOK_ADDR_STATIC`, calls
  through on Windows x86_64, verifies the callback ran and the
  original is still invocable. Self-skips on platforms without a live
  patcher.
- **94/94** tests green on `windows-x64-dev` — Splice now produces real
  x86_64 hooks on Windows, not just a compile-scaffolded stub.

### Added (Phase 3 — x86_64 Backend, FR-004)
- Vendored **nmd** length-disassembler (Unlicense, `third_party/nmd/`)
  as `nmd_assembly.h` — single-header C89 x86 decoder. `SYSTEM` include
  suppresses its C4244 conversion warnings so our `/W4 /permissive-`
  build output stays clean.
- `src/arch/x86_64/disasm.{h,cpp}` — wraps nmd with the same interface
  as the ARM64 disasm: `analyze_instruction`, `calculate_copy_size`,
  `relocate_instruction`, `emit_jmp_rel32`, `emit_jmp_abs64`,
  `looks_hooked`. Classifies: `CallRel32`, `JmpRel32`, `JccRel32`,
  `JmpRel8`, `JccRel8`, `RipRelative`, `Regular`, `Unknown`.
- `src/arch/x86_64/patcher.{h,cpp}` — inline-patch installer with
  prologue relocation + PC-relative fixup. Prefers 5-byte `E9 rel32`
  patch, falls back to 14-byte `FF 25 + abs64` when target is beyond
  ±2GB. POSIX-gated like the ARM64 patcher (Phase 4 wires Windows).
- Engine dispatch in `src/engine/engine.cpp` picks arm64 or x86_64
  backend at compile time via `SPLICE_HAS_{ARM64,X86_64}_BACKEND`.
  `splice_is_hooked()` probe handled for both arches.
- CMake arch detection flags `SPLICE_HAS_X86_64_BACKEND`; both disasms
  compile on every host (pure computation), patchers only where the
  OS abstraction is present.
- `tests/test_x86_64_disasm.cpp` — 23 new golden vectors covering every
  InstructionType × {forward, backward, boundary}, plus `emit_jmp_rel32`
  /`emit_jmp_abs64` byte-level verification and `calculate_copy_size`
  walking a realistic prologue.

### Known MSVC quirks (workarounds applied)
- `NOMINMAX` must be defined **before** any include that transitively
  pulls `<windows.h>` (for us that means before `<splice/log.h>`,
  because `platform_log.h` includes `<windows.h>` on MSVC for the
  high-precision timestamp path). Otherwise `min`/`max` macros collide
  with `std::numeric_limits<T>::max()` further down.

### Added (Phase 1.5 — HookContext consolidation, FR-008)
- `include/splice/context.h` — new home for:
  - `splice::HookBase` (empty virtual base for heterogeneous storage)
  - `splice::Hook<Ret, Args...>` (formerly `HookManager::Hook<>`, now
    namespace-scope and derives from `HookBase`)
  - `splice::HookContext` — consolidates the four pre-1.5 singletons
    (id/key map, original registry, per-signature hook map, installer
    queue) into a single class guarded by one `recursive_mutex`
  - `splice::default_context()` — process-wide Meyers singleton, lives
    in `src/context.cpp` so shared-library consumers see a single instance
- `HookContext::reset()` wipes all four registries for test-fixture
  teardown; `installer_count()` exposes queue size without leaking the
  vector by reference
- `<splice/registry.h>` refactored into delegating shims — `IdRegistry`,
  `HookManager`, `OriginalRegistry`, `register_global_installer`,
  `install_all`, `InterceptorBatch` all keep their pre-1.5 signatures and
  route through `default_context()`
- Container upgrade: `std::map` → `std::unordered_map` on all four
  registries (zero-cost change during the rewrite; the explicit perf pass
  FR-010 Phase 3.5 still plans to swap the hot-path mutex)
- New `tests/test_context.cpp` — 12 tests covering:
  - `default_context()` singleton identity
  - Independent id / original / hook / installer registries across
    HookContext instances
  - `reset()` wiping each registry
  - Concurrent `register_mapping` thread safety (8 threads × 100 ops)
  - Backwards-compat: `IdRegistry` + `install_all` shims still reach
    `default_context()`

### Changed
- **Behavioural change**: the same `unique_id` may no longer be reused
  across different template signatures. Previously worked by accident due
  to the per-signature static map in `HookManager::getHook<Ret,Args...>`;
  the new `HookContext` stores hooks in a single map keyed by id, so the
  signature is now effectively determined by the first call. This is not
  reachable from the public `SPLICE_HOOK*` macros (`__COUNTER__` gives
  each call site a unique id, `decltype(&func)` fixes the signature);
  only the pre-1.5 internal test case hit it. Test updated.
- `splice::global_installers()` (returned a vector reference) removed in
  favour of `splice::global_installer_count()` — encapsulated inside
  HookContext now.

### Added (Phase 1 — the predecessor framework port)
- Public header split (FR-002):
  - `include/splice/traits.h` — `function_traits`, `arity_v`, `return_type_t`
  - `include/splice/registry.h` — `IdRegistry`, `HookManager`, `OriginalRegistry`,
    global installer queue, `InterceptorBatch`
  - `include/splice/trampoline.h` — `TrampolineGenerator<>` (variadic only;
    explicit specialisation path deleted — review §2.2 #4)
  - `include/splice/core.h` — `InterceptorEntry<>` with fluent `.onInvoke(...)`
  - `include/splice/macros.h` — `SPLICE_HOOK(func)` / `SPLICE_HOOK(lib,func)`
    via arity-dispatched `SPLICE_GET_MACRO`, plus `_STATIC` / `_ADDR` /
    `_ADDR_STATIC` forms, `SPLICE_GET_ORIGINAL`, `SPLICE_IS_INSTALLED`,
    `SPLICE_CALL_ORIGINAL`, `SPLICE_CALL_ORIGINAL_STRICT`, `SPLICE_HOOKS`
  - `include/splice/engine.h` — C ABI: `splice_hook_symbol()`,
    `splice_hook_address()`, `splice_is_hooked()`, `splice_debug_function_start()`
- ARM64 backend (ported verbatim from the predecessor framework.h v3.0):
  - `src/arch/arm64/disasm.{h,cpp}` — B / BL / B.cond / ADR / ADRP / LDR literal /
    CBZ / CBNZ / TBZ / TBNZ decode with correct 19/21/26-bit sign extension
  - `src/arch/arm64/patcher.{h,cpp}` — inline-patch installer with PC-relative
    fixup and indirect-branch trampoline
- POSIX OS abstraction:
  - `src/os/posix/memory.{h,cpp}` — mmap/mprotect/__builtin___clear_cache wrapper
- ELF GOT/PLT patcher:
  - `src/engine/got_patcher.{h,cpp}` — `dl_iterate_phdr` walk + atomic pointer write
- Engine dispatch:
  - `src/engine/engine.cpp` — strategy selection (symbol resolve → GOT → inline patch)
  - `src/engine/engine_stub.cpp` — no-op stub for Windows/unsupported platforms
    so the public API still compiles everywhere
- CMake conditional compilation: POSIX+ARM64 builds the full backend;
  other platforms compile the stub. `${CMAKE_DL_LIBS}` linked for dlopen/dlsym.
- 24 new unit tests across `test_traits.cpp`, `test_registry.cpp`,
  `test_trampoline.cpp`, `test_core.cpp` — 32/32 pass on `windows-x64-dev`.

### Known MSVC quirks (workarounds applied)
- `decltype(&f)` sometimes decays to function type `Ret(Args...)` instead
  of pointer `Ret(*)(Args...)` in partial-specialisation matching contexts.
  `TrampolineGenerator` and `InterceptorEntry` now both accept function-type
  template arguments and delegate to their pointer specialisations.
- `platform_log.h` Windows high-precision timestamp path needed `<stdint.h>`
  hoisted into `<splice/log.h>` (TODO: upstream to platform_log.h v1.4).

### Added (Phase 0 bootstrap)
- Specification-driven scaffolding with FR-001 through FR-013
- Top-level `CMakeLists.txt`:
  - `splice` STATIC library target (`splice::splice` alias)
  - Build options: `SPLICE_BUILD_TESTS`, `SPLICE_BUILD_EXAMPLES`,
    `SPLICE_ENABLE_ASAN/UBSAN/TSAN`, `SPLICE_LOG_RUNTIME_LEVEL`,
    `SPLICE_WARNINGS_AS_ERRORS`
  - Arch detection (arm64, x86_64, arm32, x86)
  - Shared `splice_warnings` and `splice_sanitizers` INTERFACE targets
- Logging substrate (FR-012):
  - `include/splice/detail/platform_log.h` vendored from Allen's canonical v1.3
  - `include/splice/log.h` with hot-path variants
    (`*_ONCE`, `*_EVERY_N`, `*_FIRST_N`), hex dump, breadcrumb,
    C++ runtime level switch
  - `src/log.cpp` — `splice_log_hex_impl` + `splice::log::{set,get}_level`
- Umbrella `include/splice/splice.h` (< 50 lines, re-exports log.h)
- Phase 0 smoke test exercising every logging path
- Project support files: `.clang-format` (Google 4-space 100-col),
  `LICENSE` (MIT), `README.md`, this `CHANGELOG.md`

### Design decisions
- No C++ logging library (spdlog/fmt/glog) — too expensive for hook hot-path
- `disable()`/`disable_all()` (tiered), NOT `uninstall()` — true reversible
  uninstall is architecturally impossible for in-process non-privileged
  inline hooks (see FR-013)
- ARM64 disassembly will be ported **verbatim** from the predecessor framework.h v3.0
- Callback type is a template parameter, not `std::function`, so the
  fluent chain fully inlines (FR-010)

[1.0.0]: https://github.com/allenk/splice/releases/tag/v1.0.0
