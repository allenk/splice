# macOS port notes (future research — FR-014 scoping)

**Status:** research only / not scheduled. Splice today targets Android,
Linux, Windows (ARM64 + x86_64). macOS is **out of scope for v1.x**.
**Hard blocker on our side:** we have **no macOS development environment**
(this machine has WSL2, Windows, and Android devices — no Mac, no Apple
Silicon). A macOS port cannot be built or tested here, so it stays a
paper study until that changes.
**Date:** 2026-06-05.

This doc captures the analysis so a future port doesn't start from zero.

---

## TL;DR

macOS hooking is **not fundamentally unsolvable** — mature open-source tools
(Dobby, Frida, fishhook) prove the techniques work, including on Apple
Silicon / arm64e. The difficulty splits cleanly:

- **Engineering-solved** (known playbooks exist to copy): W^X via MAP_JIT,
  arm64e PAC, Mach-O chained-fixups rebinding.
- **Apple policy wall** (not a missing technique — intended): you cannot
  modify signed/immutable code (system libs in the dyld shared cache, other
  notarized apps) on a stock machine without lowering protection (SIP off,
  debugger/JIT entitlements, root).

For Splice's origin use case (in-process game enhancement, third-party arm64
code) the work lands mostly on the *engineering-solved* side. The real costs
are: a new macOS OS layer, optional arm64e PAC handling, a new Mach-O Tier-1
backend — roughly a Windows-port-sized effort — plus the fact that **we'd
need a Mac to do any of it.**

---

## Why "it's POSIX, just reuse os/posix" is wrong

The difficulty is not the API surface (`mmap`/`mprotect` exist). It's the
**runtime security model**, especially on Apple Silicon.

### 1. W^X / code signing / hardened runtime (the #1 difficulty)

- Linux/Android: `mprotect` a code page to RW → patch → restore RX. That's
  what `src/os/posix/memory.cpp` does.
- macOS (arm64) forbids this: the kernel enforces **W^X** (a page can't be
  writable and executable at once), and a signed binary's executable pages
  are backed by code-signing and **may not be modified**.
- Writable-then-executable memory requires:
  - `mmap` with **`MAP_JIT`**,
  - the **`com.apple.security.cs.allow-jit`** entitlement,
  - **`pthread_jit_write_protect_np()`** to toggle W↔X per thread.
- The real blocker is **inline-patching the target's prologue** — that's a
  signed code page. On hardened runtime it's largely closed; Dobby/Frida do
  it but require SIP off, or a debugger/JIT entitlement, or get-task-allow.

→ Splice impact: a new `src/os/macos/memory.cpp` (MAP_JIT + jit-write-protect)
for the trampoline; inline patch of signed code needs relaxed protection.

### 2. Pointer Authentication (PAC) on arm64e

- Apple Silicon arm64e signs function pointers. Two places Splice would
  crash: the trampoline jumping back to `original+N`, and Tier-1 pointer
  swaps — both must strip/re-sign PAC correctly.
- Splice's current arm64 (non-e) backend does **not** handle PAC, so it
  can't be used as-is on arm64e.
- **De-risk:** most third-party apps/games ship **arm64, not arm64e**
  (arm64e is opt-in, mainly Apple's own platform code). The game-enhancement
  use case often never touches PAC. PAC mainly matters if you target Apple
  platform binaries.

### 3. Mach-O ≠ ELF/PE — Tier 1 needs a new backend

- Splice Tier 1 (POINTER_SWAP) uses ELF GOT/PLT or PE IAT. macOS is
  **Mach-O**: imports live in `__DATA,__got` / `__la_symbol_ptr`, bound by
  `dyld`. Modern macOS (12+) uses **chained fixups**, which changed the
  format again (fishhook had to be updated).
- → A new `src/engine/macho_rebinder.cpp` (fishhook-style + chained fixups),
  not reuse of the ELF GOT code. A whole new OS/format layer.

### 4. SIP + dyld shared cache

- System libraries aren't separate files — they're in the **dyld shared
  cache** (shared, immutable, signed). Inline-patching one is very hard.
- **SIP** blocks modifying system binaries and `task_for_pid` on system
  processes. Hooking your own process is least affected; injecting into
  another app needs SIP off / entitlements.

### 5. Distribution: entitlements + notarization

Shipping a macOS hooking tool needs the right entitlements
(`allow-jit`, `allow-unsigned-executable-memory`, or debugger) plus
notarization — process friction, not a technical wall.

---

## Mature solutions in the field (copy their playbooks)

| Tool | What it is | Maps to |
|---|---|---|
| **Dobby** (jmpews) | Lightweight inline hook lib, **macOS arm64/arm64e + iOS** | Closest analogue to "Splice with macOS" — the reference to copy |
| **Frida / Gum** | Most mature dynamic instrumentation; runs on Apple Silicon | Full-framework comparison; W^X + PAC already handled |
| **fishhook** (Facebook) | Mach-O symbol rebinding (`__got`/`__la_symbol_ptr`) + chained fixups | The macOS Tier-1 template |
| **objc swizzling** (`method_exchangeImplementations`) | **Apple-sanctioned** ObjC method replacement | First-class for ObjC/Swift-via-objc; not for C funcs |
| **dyld interposing** (`__DATA,__interpose`) | Apple-supported bind-time function replacement (own dylib) | Limited but official |

**Conclusion:** Dobby already demonstrates inline hooking under arm64e +
signing. A Splice macOS port is "follow Dobby/fishhook", not research risk.

---

## What ports cleanly vs needs new work

| Piece | Status |
|---|---|
| C++ upper layer (fluent API, policy, registry, traits) | ✅ zero change |
| Build (Clang + CMake) | ✅ fine |
| ARM64 disassembler + atomic patch sequence (DMB/IC IVAU/ISB) | ✅ reusable (must wrap in MAP_JIT + PAC) |
| x86_64 logic | ✅ reusable (Intel Macs — but EOL-ing) |
| `src/os/macos/memory.cpp` (MAP_JIT + pthread_jit_write_protect) | ❌ new |
| arm64e PAC handling | ❌ new (skippable if targeting arm64-only apps) |
| `src/engine/macho_rebinder.cpp` (Tier 1, chained fixups) | ❌ new |
| Entitlements / notarization story | ❌ new (deployment) |

Effort ≈ the Windows port (FR-005) or larger (PAC + W^X are nastier than
IAT). **Plus** the environment blocker: no Mac to build/test on.

---

## Verdict

- **Technically feasible** — proven by Dobby/Frida/fishhook; arm64e is solved.
- **The only "fundamental" wall is policy** (modifying signed system/other-app
  code without lowering protection) — same class as needing ptrace on Linux
  or injection on Windows, just stricter.
- **For Splice specifically**, the in-process / third-party-arm64 case lands
  on the solvable side.
- **Gating factor is non-technical:** no macOS hardware/environment here, and
  the macOS game-enhancement market is far smaller than Android — so ROI is
  the real question, not capability.

→ Park as future research. Revisit if (a) a macOS dev environment becomes
available, and (b) there's a concrete consumer on macOS.
