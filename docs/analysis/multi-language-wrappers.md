# Analysis: multi-language wrappers for asm-test

*Status: analysis / findings. This document records a feasibility investigation,
not shipped behaviour. The corresponding implementation roadmap is
[Multi-language bindings plan](../archive/plans/multi-language-bindings-plan.md).*

## Question

Can wrappers for languages other than C be generated to **run** and **test**
assembly through asm-test — and, going further:

1. Can those wrappers also **call** assembly for general (non-test) use?
2. Can a wrapped execution *and* a test be driven from another language and the
   result validated?
3. Can that be split into **two separate calls** (execute, then validate)?
4. Does it still work with **host-language code in between** the two calls, and
   with pre-existing **register and memory state** on the host side?

## Determination

**Yes to all of it.** The architecture is unusually well-suited to foreign-language
bindings because the framework's entire value-add is exposed as a *flat C-ABI
surface* — plain functions taking pointers, `long` arrays, and fixed-layout
structs. Anything with a C FFI can consume it, and almost every language has one.

The findings below establish *why* it works, the irreducible constraints, and the
correctness rules a binding must honour.

---

## Finding 1 — The exposed surface is FFI-shaped by construction

Three independent mechanisms let another language exercise an assembly routine,
in increasing order of capability:

| Mechanism | Entry point | What it gives |
|---|---|---|
| **Direct call** | the routine's own symbol | call through the real ABI, get the return value |
| **Capture** | `asm_call_capture(regs_t*, void* fn, const long* args)` and `_fp`/`_vec`/`_fp_n`/`_vec_n`/`_args`/`_sret`/`_bigstruct` variants | return value + GP registers + flags + callee-saved state + FP/vector file |
| **Emulator** | `emu_open` → `emu_call`/`_fp`/`_vec`/`_traced` → result struct → `emu_close` | full register file, precise faults, instruction trace, block coverage — across x86-64, AArch64, RISC-V, ARM32, and Win64 |

Every routine under test is already a globally-visible, properly-typed C symbol:
`ASM_FUNC` emits `.globl` + `.type @function` (`%function` on ARM, `_`-prefixed on
Mach-O) — see [`include/asm.h`](https://github.com/wilvk/asm-test/blob/main/include/asm.h).
The capture trampolines take only pointers and `long` arrays
([`include/asmtest.h`](https://github.com/wilvk/asm-test/blob/main/include/asmtest.h)),
and `regs_t` has hand-annotated, fixed field offsets per architecture. The
emulator uses the classic opaque-handle pattern (`emu_t*`) with value result
structs ([`include/asmtest_emu.h`](https://github.com/wilvk/asm-test/blob/main/include/asmtest_emu.h)).

### The irreducible native core

The capture trampoline **must** be compiled assembly — it cannot live in a C
header (the single-header amalgamation already documents this). So no binding can
be "pure" target-language: every wrapper depends on one compiled native artifact
(`capture.o` / `libasmtest`). That artifact is linked once, not per call. This is
the only hard, non-removable dependency a binding inherits.

---

## Finding 2 — Testing and general-purpose calling are the same FFI work

Calling assembly from another language for **general (non-test)** use is *strictly
simpler* than testing it — it is just plain FFI to a C-ABI symbol and needs none of
the framework's scaffolding:

```python
import ctypes
lib = ctypes.CDLL("./libmyasm.dylib")
lib.add_signed.restype  = ctypes.c_long
lib.add_signed.argtypes = [ctypes.c_long, ctypes.c_long]
lib.add_signed(2, 3)        # → 5, through the real ABI, no asm-test involved
```

The host FFI marshals arguments per the platform ABI automatically. The difference
between "test" and "production" calling:

| | Testing (via asm-test) | General calling (plain FFI) |
|---|---|---|
| Trampoline / `regs_t` | used (register/flag capture) | not used — call directly |
| Arg marshalling | asm-test helpers (`ASM_FCALLN`, sret…) | host FFI does it |
| Crash containment | fork isolation, guard pages, timeout, crash→failure | **none — a buggy routine faults the host process** |
| Discovery / reporting | asm-test runner | the host's own runner |

The shared object therefore serves both audiences from one artifact. Calling is
also **bidirectional**: a routine that takes a function pointer (e.g.
`sum_map(arr, n, fn)`) can call back into a host-language callback (a Python
`CFUNCTYPE`, a Rust `extern "C"` thunk, a Go `//export`).

---

## Finding 3 — Host register state is preserved automatically; the captured state is frozen data

The trampoline saves the caller's (host's) callee-saved registers on entry and
restores them on exit — `pushq %rbx … %r15` then `popq` in reverse on x86-64, and
`stp/ldp` of `x19…x30` on AArch64 (`src/capture.s`). The sentinel-seeding that
tests ABI preservation happens *between* save and restore, inside the trampoline's
own frame.

Consequences:

- **Whatever register state the host had before the call, it has after.** The asm
  cannot perturb the host's live registers, and the test cannot see them.
- **What the test inspects is a *snapshot* copied into `regs_t`** (in host memory),
  not live CPU registers. By the time the call returns, there is no live register
  state to carry — it is all data in a host buffer.

Setting register *preconditions* differs by tier:

- **Native trampoline path: you cannot set arbitrary registers.** A high-level
  language has variables, not named CPU registers. Those variables become
  *arguments*, marshalled into the ABI argument registers; callee-saved come up as
  fixed sentinels.
- **Emulator path: you can set anything.** The guest CPU is independent of the
  host's real registers, so any register (and `rflags`/`nzcv`, the FP/vector file)
  can be preloaded before the run. This is the path to use when "register state
  before execution" means arbitrary preconditions, not just arguments.

---

## Finding 4 — Memory is shared by pointer; the host owns lifetime

Memory crosses the boundary as a pointer passed in the `long` argument array; the
asm reads/writes through it, and the host observes results in *its own* buffer
after the call. The gotchas, ordered by how often they bite:

- **Moving GC.** Go / JVM / .NET / some JS runtimes can relocate an object
  mid-call. The buffer must be **pinned** (`runtime.Pinner`,
  `GCHandle.Alloc(Pinned)`, FFM off-heap `MemorySegment`). CPython does not move
  objects (refcounted), so `ctypes`/`cffi` buffers are stable; Rust and Zig have no
  GC, so this is a non-issue.
- **Lifetime.** The buffer must outlive the call (and any later validation that
  re-reads it).
- **Alignment.** Aligned SIMD loads (`movaps`) need a 16-aligned buffer, which host
  allocators don't always guarantee. `asmtest_guarded_alloc` / `_under` are
  themselves C-ABI functions a binding can call to get a controlled, guard-paged
  buffer — bringing the overrun-faults-exactly behaviour along for free.
- **Emulator memory is disjoint.** The guest has its own address space: `emu_map`
  a region, `emu_write` the bytes (full memory preconditions), run, `emu_read`
  back. No pinning, no host-allocator alignment problem — nothing is shared by
  pointer; bytes are staged in and copied out. This is the cleanest path for
  moving-GC languages.

---

## Finding 5 — Execute and validate decompose into two calls, with arbitrary host code between them

asm-test already separates execution from validation: `asm_call_capture`
(execute + snapshot) and the `ASSERT_*` macros / `asmtest_assert_abi` (validate)
are distinct entry points. A binding inherits that seam:

```
call 1 (execute):  host buffers (in) ─▶ asm runs ─▶ host buffers (out) + regs_t snapshot
                   host's own registers: saved & restored, untouched
   ... arbitrary host-language code may run here ...
call 2 (validate): reads host buffers (out) + regs_t   ← stable host memory
```

This is safe **by construction**, because everything that must persist between the
calls lives in host memory, not in volatile CPU state:

- **`regs_t` is a copy, not a live view.** Host code between the calls runs on the
  real registers freely; the snapshot does not track them. The capture is immutable
  the instant the call returns.
- **The trampoline is stateless / reentrant.** It reads `out`/`fn`/`args` and writes
  only `*out` — no framework globals. Intervening code can even call the trampoline
  again (or other framework functions); each execution lands in its own buffer.

What the intervening code must **not** do — all ordinary aliasing/lifetime
discipline, not FFI problems:

1. Don't mutate an output buffer you intend to validate.
2. Don't free it or let a moving GC relocate it before validation (pin across the
   whole gap).
3. Don't reuse the snapshot buffer if you still need the earlier result.

The emulator analog holds: `emu_call` copies the run into a caller-owned
`emu_result_t` value struct and guest memory is disjoint, so intervening host code
cannot touch guest state at all — just don't `emu_close` a handle you still need or
overwrite an unvalidated result.

---

## Finding 6 — Crash safety is the one real design decision

A native, direct FFI call has **no fork isolation** — that lives in asm-test's
runner, which a thin binding bypasses. So a faulting routine takes down the host
process. To validate even pathological assembly, choose one:

- **Route execution through the emulator** (recommended). A bad access returns
  `faulted = true, fault_addr = …` as *data* (`emu_result_t`); the host validates
  the fault instead of crashing. This is the strongest cross-language story: faults
  become return values.
- **Have the host fork/subprocess the call** (Python `multiprocessing`, a child
  process), reproducing the runner's isolation in the host.
- **Accept that a native crash fails the test loudly** — fine for trusted routines,
  messy for fuzzing.

---

## Finding 7 — Validation patterns, in increasing power

1. **Capture + validate.** Execute via the trampoline; the host reads `regs_t` and
   asserts return value / flags / ABI preservation (comparing callee-saved fields to
   the documented sentinel constants). The host's own test runner owns the verdict.
2. **Differential (asm vs host reference).** Generate inputs in the host, run the
   asm (captured) and a host-language reference model, assert equivalence over N
   inputs — `ASSERT_MATCHES_REF` re-expressed with the spec written in the host
   language.
3. **Cross-tier (native vs emulator).** Run the same routine natively (real silicon)
   and in the emulator, then assert the register files agree — a validation only
   this framework's dual tiers make possible, fully orchestratable from the host.

The trust boundary: validation is only as sound as (a) the host's `regs_t` mirror
matching the correct per-arch layout and (b) the host's expected values being right.
A wrong struct layout silently validates garbage.

---

## Finding 8 — Language survey: effort tracks GC behaviour

The dominant cost driver for the native path is the host's memory model
(pinning/alignment); the emulator path neutralises it.

| Priority | Language | Binding tool | Audience fit | Native-path memory friction |
|---|---|---|---|---|
| **1 — first** | **Python** | `cffi` (parses the header) / `ctypes` | CTF/security (pwntools), reversing, crypto research, teaching; `pytest` for free | **Lowest** — non-moving GC, stable buffers |
| **2 — high** | **Rust** | `bindgen` (per-arch via `cfg`) + `cc` | systems/crypto/perf; `cargo test` built-in | **Lowest** — no GC |
| **3 — situational** | **Zig** | `@cImport` the header directly | low-level/systems; `zig test`; superb cross-compile | Low — no GC, manual allocators |
| **3 — situational** | **Go** | `cgo` | real asm community; `go test` ubiquitous | **Highest** — moving stacks + cgo pointer rules; prefer the emulator path |
| **free** | **C++** | `#include "asmtest.h"` directly | already works with zero FFI | n/a (native) |
| **defer** | JS/TS (koffi), Java (FFM/jextract), C# (P/Invoke) | auto-gen available | clean FFI, thinner overlap with asm authors | moving GC → pinning |
| **community** | Ruby (Fiddle), Lua/LuaJIT (FFI) | manual | niche low-level use | varies |

**Sequencing rationale.** Python first, because lowest friction makes it the slice
that *proves the substrate* (shared-lib build, per-arch `regs_t` mirror, all three
call paths). Rust second, proving the no-GC / auto-`bindgen` path and reaching the
systems audience. Everything else follows mechanically, ordered by demand. The
through-line: languages with non-moving or no GC make native pass-by-pointer easy;
moving-GC languages either pin carefully or lean on the emulator's disjoint memory.

---

## Tiers of effort (per language)

- **Tier 1 — Thin FFI bindings (low).** Bind the trampoline + emulator; let the
  host language's own test runner do discovery/reporting/assertions. The sweet spot.
- **Tier 2 — Idiomatic assertion layer (medium).** Reimplement `ASSERT_ABI_PRESERVED`,
  flag/ULP/hexdump-diff and property testing on top of Tier 1 — pure logic over the
  captured struct; the hard parts (trampoline, emulator) stay native.
- **Tier 3 — Full runner port (high, not recommended).** The expansion plan already
  rules out *rewriting* the framework; Tier 1 makes it unnecessary, since each target
  language already has a runner.

## Conclusion

Multi-language wrappers are feasible and low-risk, for both running and testing
assembly, in one call or two, with intervening host code, against pre-existing host
state. The C core stays untouched; the work is a shared-library build target plus
per-language bindings. See the [Multi-language bindings plan](../archive/plans/multi-language-bindings-plan.md)
for the full roadmap covering both the trampoline and the emulator across every
language above.
