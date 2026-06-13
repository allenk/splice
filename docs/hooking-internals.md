# Function Hooking on ARM64 and x86_64

> A design doc for Splice — and an attempt to explain why redirecting a
> function call is a one-line edit on some hardware and a multi-step
> hardware ritual on others.
>
> Audience: someone comfortable with C++ and assembly who wants to know
> *why* `memcpy` over a function prologue can SIGILL on ARM64 but
> usually works on Intel.

---

## TL;DR

There are two fundamentally different ways to redirect a function call:

1. **Patch the pointer that the call goes through** (GOT / IAT / vtable slot)
2. **Patch the code that gets called** (inline prologue overwrite)

The first is a single aligned-pointer-sized atomic write. Done.

The second has to confront, in order:
- **Atomicity** — the multi-byte write must not be observable mid-stream
- **Memory order** — other CPUs must not see the writes out of program order
- **I-cache coherency** — other CPUs may have cached the old bytes; on weak-ordered ISAs, you must explicitly invalidate
- **Pipeline flush** — the patching CPU may have prefetched stale instructions; you must explicitly drain its pipeline

x86_64 hardware solves #2, #3, and #4 transparently. You only need
care about #1 — and that's just "use an aligned 8-byte store." All
modern x86 cores snoop the instruction cache, propagate stores in
Total Store Order, and flush their own pipelines on cross-modifying
code without you asking.

ARMv8 makes you do all four explicitly: `DMB ISH`, `DC CVAU`, `IC IVAU`,
`ISB`. The trade-off is power efficiency — most code doesn't self-modify,
so why pay coherency traffic for the rare case?

This article walks through both, then compares Splice's choices with
Microsoft Detours.

---

## Part 1 — The Two Strategies

### 1.1 Pointer redirect (GOT, IAT, vtable slot)

```
caller:          callsite:                                target_func:
 ...                │                                       ┌──────────┐
 call printf  ────► │ call qword ptr [printf@got]   ──────► │  push rbp│
 ...                │                                       │  mov  rbp│
                    │                                       │  ...     │
                    └──── reads pointer from ────┐          │          │
                                                 ▼          │          │
                                    ┌──────────────────┐    │          │
                                    │ printf@got:      │ ──►│          │
                                    │  → addr of printf│    │          │
                                    │  (8 bytes data)  │    │          │
                                    └──────────────────┘    └──────────┘
                                            ▲
                                            │
                                       This is what we patch
                                            │
                                       atomic 8-byte write
```

To redirect: **rewrite the 8-byte pointer**. That's it.

- **GOT** (Global Offset Table) on ELF: every imported function gets one
  pointer slot in the executable's `.got` section. PLT stubs read it
  before jumping.
- **IAT** (Import Address Table) on PE/COFF: same idea, different name.
  Windows binaries have one IAT per imported DLL.
- **vtable slot**: per-class virtual function table. Same shape — a
  pointer that gets read before the call.

All three are **data**, not code. The CPU treats them as ordinary memory
loads. There's no I-cache. The aligned 8-byte write is atomic by ISA
guarantee on every architecture I've ever shipped on. You don't need
barriers, you don't need pipeline flushes, you don't need to worry
about other threads racing through a half-written byte.

You do still need to flip the page from RO to RW (RELRO / `IMAGE_DIRECTORY_ENTRY_IMPORT`
sections are usually RO) and back, but that's a `mprotect` / `VirtualProtect`
syscall, not a hardware concern.

### 1.2 Code redirect (inline prologue overwrite)

```
target_func:                          ───────────────►   target_func:
 ┌──────────────────┐                                    ┌──────────────────────┐
 │ push rbp         │  (5 bytes)                         │ E9 rel32             │  (5 bytes — JMP to hook)
 │ mov  rbp, rsp    │  (3 bytes)         OVERWRITE       │ <preserve byte 5..7> │
 │ sub  rsp, 0x20   │  (4 bytes)         ─────────►      │ <existing bytes>     │
 │ ...              │                                    │ ...                  │
 └──────────────────┘                                    └──────────────────────┘

(original prologue → relocated into trampoline with PC-relative fixup)
```

This is harder by every imaginable measure:

1. The **target memory is code**. CPUs cache code separately from data.
2. The **write is multi-byte**. On most ISAs that means it can be observed
   mid-write by another core unless you use a special atomic primitive.
3. **Instructions may be variable-length** (x86) or fixed but with
   PC-relative bits (ARM64 ADRP/ADR/B/BL) — overwriting them isn't a
   straight byte copy, you need to relocate.
4. The patching CPU's own **pipeline** may have already prefetched the
   old bytes; without an explicit flush, it might keep executing the
   pre-patch instruction stream.

Splice does this only when GOT/IAT isn't available (e.g. hooking a
function the binary calls directly without going through the import
table — most game-internal helpers, anything `static`-linked into the
same module). The Plan §FR-013 names this "Tier 2" and documents that
disabling such a hook leaks the trampoline forever, because there's
no safe time to free it.

---

## Part 2 — The Four Problems Inline Patching Has To Solve

Let me name them sharply, then walk through how each architecture handles them.

### 2.1 Problem #1: Atomicity of the write

If you `memcpy` 16 bytes over a function prologue, another thread's
instruction fetch can land in the middle. That thread sees, say, 8
bytes of the new code followed by 8 bytes of the old code. Whatever
that combination decodes to ── garbage ── gets executed. Best case:
SIGILL. Worst case: silent jump into nowhere.

The fix is one of:
- **Atomic single-store of the entire patch** (only works if the patch
  fits in a single architectural atomic store size — 8 bytes on x86_64,
  4 bytes on ARM64)
- **Stop-the-world thread suspension** during the write (Detours does
  this; needs ptrace / SuspendThread)
- **Two-stage write** where the intermediate state is also valid code
  (e.g. write a `0xCC` int3 first, install a trap handler, complete
  the patch under the trap)

ShadowHook tried the naive `memcpy` and shipped a SIGILL in production.
The retrospective is documented in their release notes.

### 2.2 Problem #2: Memory ordering across cores

Even if the write itself is atomic, when you do *two* writes in your
patch sequence (say, write the literal pool first, then write the
branch instruction), other cores may see them in *the opposite order*
on weakly-ordered ISAs.

Example, ARM64 16-byte indirect branch sequence:
```
[target+0]   ldr  x17, #8           ← branch instruction
[target+8]   .quad new_func         ← literal pool, 8 bytes
```

If you write the literal first, then write the `ldr` instruction, but
another core sees them in *reverse* order, it'll see the new `ldr`
pointing at... whatever `[target+8]` had before. Random old data.
Indirect branch into garbage. SIGILL.

The fix on weak-ordered hardware: **memory barriers** between dependent
writes. On TSO hardware (x86): the order is guaranteed by the ISA, no
explicit barriers needed.

### 2.3 Problem #3: Instruction-cache coherency

CPUs have separate caches for data (`D$`) and instructions (`I$`).
When you write to memory, you populate the D-cache. The I-cache may
still hold stale lines pointing at the *old* code.

x86 hardware **automatically snoops** writes in the inner-shareable
domain and invalidates corresponding I-cache lines. Self-modifying
code "just works" without software intervention.

ARMv8 does **not** snoop. The I-cache is a one-way street: it's
populated by instruction fetches and never invalidated by data writes.
You must explicitly tell it: "I just wrote new code at this address,
go invalidate the I-cache lines covering it."

This is what the `IC IVAU` instruction does. More on it below.

### 2.4 Problem #4: Pipeline prefetch on the patching CPU

Modern out-of-order CPUs have deep instruction pipelines. The CPU may
have already fetched the bytes at `target+N` for `N` in [0..50] *before*
your patch landed. Even if the I-cache is now invalidated and the new
bytes are in there, the in-flight pipeline stages still have the old
bytes.

x86 handles this transparently: cross-modifying-code rules in Intel
SDM Vol 3A §8.1.3 say that an aligned write to an instruction stream
forces affected pipeline stages to refetch. No software intervention.

ARMv8 makes you ask: `ISB` (Instruction Synchronization Barrier) drains
the current CPU's pipeline. After `ISB`, all subsequent instruction
fetches go through the now-coherent I-cache.

---

## Part 3 — The ARMv8 Toolkit Explained

These are the instructions Splice's `arm64::atomic_install_indirect_branch`
uses, in the order it uses them. Each does one specific thing.

### 3.1 `DMB ISH` — Data Memory Barrier, Inner Shareable

**Purpose**: orders memory accesses (loads + stores) on this CPU with
respect to other CPUs in the inner-shareable domain.

After `DMB ISH`:
- Every load/store **before** the barrier is observed by other CPUs
  before any load/store **after** the barrier
- The barrier blocks the issuing CPU only minimally — it's a **fence**
  on observability, not a stall

**Why "Inner Shareable"?** ARMv8 has memory domain levels:
- **Non-Shareable** — only this CPU sees the memory (rare)
- **Inner Shareable** — all CPUs in the same SoC / cluster (the common case)
- **Outer Shareable** — across SoCs / system fabric (multi-socket, GPUs)
- **Full System** — everything

For self-modifying code on a multi-core ARM SoC, `ISH` is the right
scope. `OSH` would be overkill (slower, no benefit). `SY` (full system)
is overkillier still.

In Splice:
```cpp
asm volatile("dmb ish" ::: "memory");
```

The `"memory"` clobber tells the C++ compiler not to reorder loads/stores
across the barrier (compiler-level fence; the `dmb ish` is the hardware
fence).

### 3.2 `DSB ISH` — Data Synchronization Barrier

**Purpose**: stronger than DMB. The issuing CPU **stalls** until every
prior memory access has completed (not just become observable, but
actually finished — including cache-maintenance operations).

When you'd use DMB vs DSB:
- **DMB** = "make these stores observable in order"
- **DSB** = "make these stores observable AND complete; only then continue"

Splice uses `DSB ISH` after cache-maintenance ops (`DC CVAU`, `IC IVAU`)
because we need those to actually finish before we proceed:

```cpp
for (...) asm volatile("dc cvau, %0" :: "r"(addr) : "memory");
asm volatile("dsb ish" ::: "memory");   // wait for all DC CVAU to complete

for (...) asm volatile("ic ivau, %0" :: "r"(addr) : "memory");
asm volatile("dsb ish" ::: "memory");   // wait for all IC IVAU to broadcast
```

Without the DSB, the CPU could move on to the next instruction (e.g.
`ISB`) before the cache-maintenance had visible effect.

### 3.3 `DC CVAU` — Data Cache Clean by Virtual Address to Point of Unification

**Purpose**: write back any dirty D-cache line covering the given
virtual address, down to the **Point of Unification (PoU)**.

**PoU** is the level in the cache hierarchy where the I-cache and D-cache
become coherent — typically the L2 cache or unified L3.

```
            ┌──────────┐    ┌──────────┐
            │  CPU 0   │    │  CPU 1   │
            │┌────────┐│    │┌────────┐│
            ││  L1 D$ ││    ││  L1 D$ ││  ← dirty bytes here after write
            ││  L1 I$ ││    ││  L1 I$ ││  ← stale bytes here (old code)
            │└────────┘│    │└────────┘│
            └─────┬────┘    └────┬─────┘
                  │              │
                  ▼              ▼
            ┌────────────────────────┐
            │     L2 cache (PoU)     │  ← DC CVAU pushes dirty D$ down to here
            └────────────────────────┘     IC IVAU pulls fresh from here on next fetch
```

When you write new instructions:
1. Bytes land in the D-cache (and L2 eventually, via writeback)
2. The I-cache **doesn't know** the bytes changed
3. `DC CVAU <addr>` forces the dirty D-cache line containing `<addr>`
   to write back to PoU immediately
4. After the subsequent `DSB ISH`, the bytes are guaranteed to be at
   PoU — so any subsequent I-cache miss on this address will refetch
   the new bytes from PoU

`DC CVAU` operates one cache line at a time (typically 64 bytes on
ARMv8 cores; check `CTR_EL0` for the exact granule). To clean a
16-byte patch, you need at most one DC CVAU; for larger ranges, loop.

Splice's helper:
```cpp
for (auto* q = aligned_start(p); q < end; q += dline) {
    asm volatile("dc cvau, %0" :: "r"(q) : "memory");
}
asm volatile("dsb ish" ::: "memory");
```

### 3.4 `IC IVAU` — Instruction Cache Invalidate by Virtual Address to Point of Unification

**Purpose**: invalidate the I-cache line covering the given virtual
address, **broadcast across the inner-shareable domain**.

This is the counterpart to `DC CVAU`. Where DC CVAU pushes dirty D-cache
data **down to PoU**, IC IVAU invalidates I-cache data **above PoU**,
forcing a refetch from PoU on next instruction fetch.

The broadcast property is crucial: a single CPU executes IC IVAU, and
all CPUs in the inner-shareable domain invalidate their corresponding
I-cache lines. You don't have to inter-process-interrupt every other
CPU; the cache controller does it.

```cpp
for (auto* q = aligned_start(p); q < end; q += iline) {
    asm volatile("ic ivau, %0" :: "r"(q) : "memory");
}
asm volatile("dsb ish" ::: "memory");
```

### 3.5 `ISB` — Instruction Synchronization Barrier

**Purpose**: flush the issuing CPU's instruction pipeline.

After ISB, all subsequent instruction fetches go through the (now
coherent) I-cache. Any speculatively prefetched stale instructions
in the pipeline are discarded.

```cpp
asm volatile("isb" ::: "memory");
```

ISB is **single-CPU**: it only affects the CPU executing it. Other CPUs'
pipelines flush themselves on the next branch / interrupt / context
switch (which happens essentially continuously in practice; you don't
need to broadcast ISB).

### 3.6 The full ARMv8 self-modifying-code sequence

```cpp
// 1. Write new bytes (data store goes to D-cache)
*((uint32_t*)target) = new_instr;

// 2. Push dirty D-cache line down to PoU
dc_cvau(target);        // one DC CVAU per cache line
dsb_ish();              // wait for it

// 3. Invalidate I-cache line, broadcast to all CPUs
ic_ivau(target);        // one IC IVAU per cache line
dsb_ish();              // wait for broadcast

// 4. Flush this CPU's pipeline
isb();
```

In Splice this is wrapped in `invalidate_icache_range()` in
`src/arch/arm64/atomic_patch.cpp`. The atomic single-store of the
4-byte instruction word (problem #1) happens before this sequence,
guarded by a `DMB ISH` to enforce ordering with the prior literal-pool
write.

---

## Part 4 — Why x86_64 Doesn't Need Any of This

The dramatic answer: x86 has **TSO (Total Store Order)** + **coherent
I-cache** + **transparent cross-modifying-code handling**. Three
features that ARM deliberately doesn't have, all of which conspire to
make self-modifying code on x86 a one-liner.

### 4.1 TSO — Total Store Order

x86's memory model guarantees:
- **Stores** issued by one CPU are observed by all other CPUs in the
  same order
- **Loads** can be reordered ahead of earlier stores (the famous
  StoreLoad reordering case), but no other reorderings are visible
- Therefore: store-store ordering is automatic; you don't need DMB

In our 5-byte E9 rel32 install, there's only one store anyway (the
aligned 8-byte mov), so memory order isn't even an issue. But if you
wrote two stores — say, the literal then the instruction — TSO would
guarantee the literal lands first across all observers, no fence needed.

The intuition: **x86's caches form a consistent global history of
stores, automatically**.

### 4.2 Coherent I-cache (Intel SDM Vol 3A §11.6)

> "A write to a memory location in a code segment that is currently
>  cached in the processor causes the associated cache line (or lines)
>  to be invalidated."

The I-cache **snoops** D-cache writes. When you `mov` to memory that
has been (or could be) instruction-fetched, the cache controller
automatically invalidates affected I-cache lines. No software
intervention.

The trade-off: this snooping is constant overhead — every D-cache
write checks against all I-cache lines. ARM removed this snooping in
ARMv7 onwards to save power; the cost is that software (compilers,
JITs, linkers, dynamic loaders, hooking libraries) has to manage
I-cache coherency explicitly.

### 4.3 Cross-modifying code and aligned-write atomicity (Intel SDM Vol 3A §8.1.3)

> "Atomicity of cross-modifying code execution is guaranteed if both
>  the modifying processor and the modified processor execute serializing
>  instructions ... However, in real-world applications, the size of
>  modification is often smaller than the size of an instruction
>  prefetched into the pipeline."

In practice, modern x86 (Pentium 4 and later) handles cross-modifying
code more leniently than the formal SDM rule suggests:
- An **aligned 8-byte instruction-stream write** is atomic from the
  perspective of instruction fetch
- The pipeline auto-flushes affected speculative fetches
- The I-cache is invalidated by snooping
- **Other CPUs** see the new bytes via cache coherence within a few
  cycles

The official "execute a serializing instruction" rule is for unaligned
writes or writes that span cache lines. For aligned 8-byte writes that
fit in a single cache line, the hardware handles everything.

This is why every major hooking library on x86 (Detours, MinHook,
PolyHook, EasyHook, our Splice 4.5b) just does the aligned 8-byte mov
and trusts the CPU.

### 4.4 The full x86_64 self-modifying-code sequence

```cpp
// That's it. One instruction. No barriers, no cache ops, no ISB.
*((std::atomic<uint64_t>*)target).store(new_word, std::memory_order_release);
```

`std::atomic_ref<uint64_t>` lowers to a single `mov qword ptr [target], reg`.
The CPU does the rest.

### 4.5 Why ARM made the opposite choice

It's not that ARM is "worse." It's a deliberate trade:

| | x86 | ARMv8 |
|---|---|---|
| Self-modifying code | Free | Software-managed |
| Constant power cost of I-cache snooping | Yes | No |
| Common case (no SMC) overhead | Some | None |
| JIT / hooking developer burden | Low | High |

ARM optimizes for **the common case**: most code doesn't self-modify.
Removing I-cache snooping saves silicon and power on every cycle of
every program forever. The cost is that the rare self-modifying code
(JITs like V8 / JVM / ART, hot-patching libraries like Splice / Frida /
ShadowHook) has to do explicit cache management.

For battery-powered devices (which ARMv8 dominates) this trade is
unambiguously correct. For desktop / server (which x86 dominates), the
wattage is irrelevant and developer ergonomics win.

---

## Part 5 — Comparison with Microsoft Detours

[Microsoft Detours](https://github.com/microsoft/Detours) is the
reference x86/x64 hooking library. Battle-tested, widely deployed,
well-engineered. Splice's design overlaps with Detours in places and
diverges deliberately in others.

### 5.1 Where Detours is unambiguously stronger

- **Maturity**: shipped in Microsoft products since the late 90s.
  Has handled essentially every weird x86 instruction encoding,
  exception-handler interaction, and Windows-specific corner case.
- **Transactional commit**: `DetourTransactionBegin` /
  `DetourAttach` (queue) / `DetourTransactionCommit` (apply all under
  thread suspension). This gives **multi-thread safety guarantees**
  Splice doesn't currently match.
- **Thread suspension fallback**: when atomic install isn't viable
  (instruction crosses cache line, target is misaligned, multi-byte
  patch wider than 8 bytes), Detours suspends every other thread,
  walks each thread's RIP to verify it's not in the patch region,
  installs, resumes. Strong correctness guarantee.
- **Hot-patchable function support**: Detours can use the Windows
  hot-patch convention (the `MOV EDI, EDI` 2-byte NOP at function
  entry, plus 5 bytes of NOPs immediately before) to install hooks
  without any of these atomicity concerns.

### 5.2 Where Splice is different (not necessarily better; just different)

| Dimension | Detours | Splice |
|---|---|---|
| Architectures | x86, x64 only | x86_64 + ARM64 (Android first) |
| Strategy 1 | None | **GOT/IAT pointer redirect (Tier 1, always safe)** |
| Strategy 2 | Inline patch (their main path) | Inline patch (Tier 2, rare) |
| Atomicity (5-byte case) | Aligned 8-byte mov + serializing | Aligned 8-byte mov |
| Atomicity (>8-byte case) | Thread suspension | Non-atomic with `SPLICE_LOGW` (best-effort) |
| Disable / uninstall | Full transactional reversal | Tiered: GOT-reverse always safe; inline disable leaks trampoline; full reversal NOT supported (FR-013) |
| Multi-thread guarantees | Strong (transactional API) | Best-effort + documented gaps |
| API style | C, transactional | C++ fluent (`SPLICE_HOOK(...).onInvoke(...)`) |
| Codebase size | ~10k LOC Win32-specific | ~3k LOC cross-platform |
| Thread coordination | `SuspendThread` + IP walk | None today (4.5d roadmap) |
| Embeddability | Mature, well-known DLL | Static lib, header-only API |

### 5.3 The 90% case: GOT/IAT redirect

Splice's biggest design difference from Detours is that **Splice tries
GOT/IAT first**, and only falls through to inline patching when there's
no import-table entry pointing at the target. Detours only does inline.

For the canonical use case Splice was built for ── hooking
`eglCreateContext`, `glDrawArrays`, `ANativeWindow_setBuffersGeometry`
in an Android game ── GOT/IAT covers all of it. The game is calling
these functions through its own GOT (built by the linker). Patch the
GOT slot, all subsequent calls from the game to that function go to
your hook. Atomic pointer write. No instruction patching, no cache
maintenance, no thread coordination.

The whole `arm64/atomic_patch.cpp` machinery is the **fallback** for
when the user wants to hook a function the game is calling directly
(static-linked helper, intra-module call). In production, this is
rarely necessary.

Detours doesn't have this pre-stage — every hook goes through inline
patching. On Windows with PE-format binaries this still works, but
it's working harder than it needs to.

### 5.4 What Splice should borrow from Detours

Two things, both currently in Splice's roadmap:

1. **Transactional commit** — queue multiple hook installs, apply
   them all under a single `SuspendThread` window. Currently Splice
   installs one at a time with no synchronisation. Phase 4.5d's
   stress test infrastructure naturally leads here.

2. **Thread suspension for the >8-byte cases** — the 14-byte abs64
   fallback in Splice's x86_64 patcher is currently non-atomic with
   `SPLICE_LOGW`. Detours' approach (SuspendThread + IP walk + write +
   ResumeThread) is the right answer. Adding it would close Splice's
   only documented atomicity gap.

### 5.5 What Splice does that Detours doesn't

1. **ARM64**. Detours has never supported ARM. For Splice's primary
   deployment target (Android phones), this is the entire game.

2. **GOT/IAT first**. Saves the user from the inline-patch hazards
   in the 90% case.

3. **Honest about uninstall limits** (FR-013). Detours can rely on
   thread suspension to reverse a hook safely. Splice's threat model
   (in-process, non-privileged, no ptrace) makes that impossible, and
   the design doc says so out loud rather than offering a leaky API.

4. **Modern C++ fluent API**. Subjective but Splice's
   `SPLICE_HOOK(eglSwapBuffers).onInvoke([](auto orig, auto... args){...})`
   reads better than Detours'
   `DetourAttach(&(PVOID&)RealEglSwapBuffers, MyEglSwapBuffers)`.

---

## Part 6 — Splice's Concrete Implementation

For reference, here's where each concept lives in the codebase:

### 6.1 Tier 1: GOT/IAT redirect

```
src/engine/got_patcher.cpp     ← ELF GOT scan via dl_iterate_phdr (POSIX)
src/engine/iat_patcher.cpp     ← PE IAT scan via EnumProcessModules (Win32)
src/engine/engine.cpp          ← strategy dispatch: GOT/IAT first, fall through to inline
```

The atomic pointer write itself is a single `*entry = new_func;` after
flipping the page to RW. No barriers, no cache ops. Works identically
on x86_64 and ARM64.

### 6.2 Tier 2: Inline patch (atomic where possible)

```
src/arch/arm64/atomic_patch.cpp   ← DMB ISH + atomic 32-bit instr write + DC CVAU + IC IVAU + ISB
src/arch/x86_64/atomic_patch.cpp  ← single aligned 8-byte mov; no barriers needed
```

ARM64 install (`atomic_install_indirect_branch`):
```
1. Write 8-byte literal pool at [target+8..15]
2. Write `br x17` instruction at [target+4..7]
3. dmb ish  ← make 1+2 globally visible
4. Atomic 32-bit store of `ldr x17, #8` at [target+0]
5. dc cvau + dsb ish + ic ivau + dsb ish + isb
```

x86_64 install (`atomic_install_jmp_rel32`):
```
1. Read original 8 bytes at target
2. Construct new word: byte 0 = E9, bytes 1..4 = rel32, bytes 5..7 preserved
3. Atomic 8-byte store at target
```

### 6.3 Tier 3: NOT supported, by design

True reversible uninstall with memory reclaim. Documented in FR-013.
Industry precedent (Frida, ShadowHook, LSPlant) confirms this is a
fundamental limit of in-process non-privileged inline hooking, not an
engineering shortcoming. Splice exposes a tiered `disable()` API
instead — Tier 1 GOT reverse always safe; Tier 2 inline disable with
permanent trampoline leak.

---

## Part 7 — Open Issues

### 7.1 14-byte abs64 fallback is non-atomic

When the trampoline is allocated more than ±2 GB from the target, the
5-byte E9 rel32 won't reach. The fallback is a 14-byte
`FF 25 [RIP+0] [8-byte abs addr]`. 14 bytes can't be written atomically
in a single store on x86_64.

Phase 4.5b currently uses non-atomic memcpy with `SPLICE_LOGW` for this
case. Documented in CHANGELOG. Two paths to a real fix:
- **Thunk-based**: 5-byte E9 to a stub page allocated within ±2 GB of
  target; the stub does the abs64 jump. Atomic 5-byte install at the
  hot site. Detours uses this approach.
- **Thread suspension**: SuspendThread (Windows) + RIP walk + write +
  ResumeThread. Stronger correctness, more invasive.

### 7.2 No multi-thread stress validation yet

Phase 4.5a (ARM64) and 4.5b (x86_64) introduced atomic install paths
but verified only end-to-end correctness on single-threaded LiveHook.
The atomic semantics matter under multi-threaded racing — install
running while 100 threads invoke the target. That's Phase 4.5d's
TSan-instrumented stress test (see `docs/Splice Plan.md`).

### 7.3 IAT patch covers one module at a time

`find_iat_entry_for_address` returns the first matching IAT slot
across all loaded modules. To hook *every* caller of a Windows API,
you'd need to walk every module and patch each one. Most use cases
don't need this; some advanced ones do. Currently a known limitation.

---

## References

- ARM Architecture Reference Manual ARMv8 §B2.2.1 (Single-copy atomicity),
  §D7.5.6 (Cache maintenance instructions)
- Intel® 64 and IA-32 Architectures Software Developer's Manual Vol 3A
  §8.1 (Locked Atomic Operations), §11.6 (Self-Modifying Code)
- Microsoft Detours: <https://github.com/microsoft/Detours>
- Frida Gum: <https://frida.re/docs/gum/>
- ShadowHook: <https://github.com/bytedance/android-inline-hook>
- LSPlant: <https://github.com/LSPosed/LSPlant>
- Allen's `docs/Old API Hooker Review.md` — the predecessor framework v1.4 review
  that triggered Splice's design re-thinking

---

## License

Public domain (within CodeForge convention).
