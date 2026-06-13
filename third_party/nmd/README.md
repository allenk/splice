# nmd — vendored

**Upstream:** https://github.com/Nomade040/nmd
**License:** The Unlicense (public domain) — see `LICENSE`
**Purpose:** x86 / x86_64 length-disassembler for Splice's Phase 3 x86_64 backend.
Variable-length x86 instructions need a length decoder so we know how many
bytes of a function prologue to relocate into the trampoline. nmd is a C89,
header-only, zero-allocation, thread-safe disassembler that fits the bill.

**Files vendored:**
- `nmd_assembly.h` — the full single-header library (~10k LOC)
- `LICENSE` — Unlicense / public domain grant

**Usage pattern:**
```cpp
#define NMD_ASSEMBLY_IMPLEMENTATION    // in exactly one TU
#include <nmd_assembly.h>

nmd_x86_instruction insn;
if (nmd_x86_decode(bytes, size, &insn, NMD_X86_MODE_64, 0)) {
    // insn.length       — bytes consumed
    // insn.opcode        — primary opcode byte
    // insn.imm_mask/disp — for PC-relative fixup
}
```

**Bump policy:** pinned by git commit. Upstream is low-churn; re-vendor when
a bugfix we need lands upstream. Any local modifications must be documented
at the top of `nmd_assembly.h`.

Currently vendored from `master` branch circa 2026-04-19.
