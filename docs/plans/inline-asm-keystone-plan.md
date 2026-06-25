# asm-test — In-line assembly strings via Keystone: implementation plan

A phased roadmap for letting callers pass **assembly source as a string** and run
it, rather than only handing the framework pre-assembled object code. Keystone is
the assembler counterpart to the **Unicorn** disassembler the emulator tier
already uses, and it covers every guest the emulator already runs — x86-64,
AArch64, RISC-V (RV64), and ARM32.

This plan is the *how + in what order*. The headline it establishes: **the
emulator tier already consumes raw `(code, code_len)` byte buffers, so in-line
assembly is a text→bytes front end bolted onto paths that already exist** — no new
execution engine, and no native/capture-tier work (that would need an executable
`mmap`; in-line source belongs in the emulator tier where code is already bytes).

> Status legend: **planned** unless noted. Update this file as phases land, the way
> [expansion-plan.md](expansion-plan.md) and
> [multi-language-bindings-plan.md](multi-language-bindings-plan.md) track theirs.

---

## Goals & non-goals

**Goal.** Let a test supply a routine as assembly *text* and run/inspect it through
the existing emulator tier:

1. **assemble** an assembly string to machine code for any of the four guests
   (x86-64, AArch64, RV64, ARM32), at the emulator's load base so PC-relative
   branches resolve correctly, and
2. **run + capture** it through the existing `emu_*_call(..., code, code_len, ...)`
   paths — full register file, faults-as-data, traces/coverage — with one bridge
   call (`emu_call_asm`), and
3. surface assembler **errors as data** (bad mnemonic → `ok == false` plus the
   Keystone diagnostic), never a crash, and
4. expose the same in-line entry point through the **language bindings**.

**Non-goals.**

- The native / capture tier. Running assembled bytes on real silicon needs an
  executable `mmap` + ABI trampoline; in-line source stays emulator-only, exactly
  as the cross-arch guests already are.
- Replacing the corpus / symbol model. `asmtest_corpus_routine(name)` and the
  linked `.s` sources stay the primary path; in-line assembly is a parallel,
  additive entry point.
- A new dependency on the *core* framework. Keystone is optional and pkg-config
  gated, mirroring how `libunicorn` gates the emulator tier today.

---

## Design overview

```
   assembly text ──ks_asm()──▶ machine-code bytes ──emu_*_call()──▶ result
   (per guest)    Keystone     (at EMU_CODE_BASE)   Unicorn          (regs/faults)
```

Two layers:

- **Assembler core** (`src/assemble.c`, `include/asmtest_assemble.h`) — depends on
  **Keystone only**. Pure `text → bytes`; independently useful and unit-testable
  without Unicorn.
- **Emu bridge** (`emu_call_asm` and per-guest variants) — assembles, then
  delegates to the existing `_call`. Lives in `assemble.c` so its `.o` needs only
  Keystone headers (it *calls* `emu_call`, declared in `asmtest_emu.h`, but does
  not pull Unicorn itself); the final binary links both libs.

The emulator loads code at `EMU_CODE_BASE` (`0x00100000`, currently private to
[src/emu.c](../../src/emu.c)). Keystone must assemble at that same base so branch
and PC-relative targets match where the bytes actually land — so the base is
exposed in the public emu header and shared by both sides.

---

## Phase 1 — Assembler core (Keystone only) — **done**

New `include/asmtest_assemble.h`:

```c
typedef enum { ASM_X86_64, ASM_ARM64, ASM_RISCV64, ASM_ARM32 } asm_arch_t;
typedef enum { ASM_SYNTAX_INTEL, ASM_SYNTAX_ATT } asm_syntax_t; /* x86 only */

typedef struct {
    uint8_t *bytes;       /* malloc'd machine code (NULL on failure)        */
    size_t   len;         /* byte length                                    */
    size_t   stat_count;  /* statements assembled                           */
    bool     ok;
    char     err[160];    /* ks_strerror + offending statement on failure   */
} asm_result_t;

bool asmtest_assemble(asm_arch_t arch, asm_syntax_t syntax,
                      const char *source, uint64_t addr, asm_result_t *out);
void asmtest_asm_free(asm_result_t *r);
```

`src/assemble.c` wraps Keystone: `ks_open` with the arch/mode map
(`KS_ARCH_X86`/`KS_MODE_64`, `KS_ARCH_ARM64`, `KS_ARCH_RISCV`/`KS_MODE_RISCV64`,
`KS_ARCH_ARM`/`KS_MODE_ARM`), optional `ks_option(KS_OPT_SYNTAX, …)` for x86,
`ks_asm(addr)`, copy the encoding into a `malloc`'d buffer, then `ks_free` /
`ks_close`. On failure capture `ks_strerror(ks_errno(ks))` into `out->err` and set
`ok = false`. `asmtest_asm_free` frees `bytes` and zeroes the struct.

**Decision (settled): default x86 in-line syntax is Intel**, with `ASM_SYNTAX_ATT`
selectable. (The repo's `.s` corpus is GAS/AT&T, but in-line strings follow the
Keystone/kstool convention; AT&T stays one enum away.)

## Phase 2 — Emu bridge wrappers — **done**

Move `EMU_CODE_BASE` into [include/asmtest_emu.h](../../include/asmtest_emu.h) as a
public `#define` (out of emu.c). Add to `src/assemble.c`:

```c
bool emu_call_asm      (emu_t *e,        const char *src, const long *args, int n,
                        uint64_t max_insns, emu_result_t *out);
bool emu_arm64_call_asm(emu_arm64_t *e,  const char *src, const long *args, int n,
                        uint64_t max_insns, emu_arm64_result_t *out);
bool emu_riscv_call_asm(emu_riscv_t *e,  const char *src, const long *args, int n,
                        uint64_t max_insns, emu_riscv_result_t *out);
bool emu_arm_call_asm  (emu_arm_t *e,    const char *src, const long *args, int n,
                        uint64_t max_insns, emu_arm_result_t *out);
```

Each assembles `src` at `EMU_CODE_BASE` for its guest, then delegates to the
matching existing `_call`. On assemble failure: return `false` with the Keystone
message surfaced (via the result's `uc_err`/an error field, matching how the emu
side already reports setup failure).

## Phase 3 — Build wiring (mirror Unicorn exactly) — **done**

In [Makefile](../../Makefile), beside the `UNICORN_*` block:

- `KEYSTONE_CFLAGS ?= $(shell pkg-config --cflags keystone 2>/dev/null)` and
  `KEYSTONE_LIBS ?= $(shell pkg-config --libs keystone 2>/dev/null || echo -lkeystone)`.
- `$(BUILD)/assemble.o` and `$(BUILD)/pic/assemble.o` rules (Keystone cflags).
- Add `assemble.o` to the emu shared-lib link line so `libasmtest_emu` exports the
  in-line API, appending `$(KEYSTONE_LIBS)`.
- New `make asm-test` target building `test_asm` from the framework objects +
  `emu.o` + `assemble.o`, linking `$(UNICORN_LIBS) $(KEYSTONE_LIBS)`.
- New `docker-asm` target alongside `docker-emu`.

## Phase 4 — Dependency provisioning — **done**

In [scripts/install-deps.sh](../../scripts/install-deps.sh): add `want_keystone`, a
`--asm` flag (and include it in `--all`), a `have_keystone()` pkg-config probe, and
per-manager package names (`apt-get: libkeystone-dev`, `pacman: keystone`,
`brew: keystone`, …). Where a distro ships no package, print a source-build note
rather than failing. Update the `--asm` line in the usage banner and the
`make deps` comments in the Makefile.

## Phase 5 — Tests — **done**

New `examples/test_asm.c` (TAP, modeled on
[examples/test_emu.c](../../examples/test_emu.c)):

- **x86-64**: assemble `"mov rax, rdi; add rax, rsi; ret"`, run via
  `emu_call_asm(40, 2)`, assert `rax == 42` (matches the `add_signed` corpus
  expectation).
- **One routine per other guest** (AArch64 / RV64 / ARM32) asserting a known
  result through `emu_arm64_call_asm` / `emu_riscv_call_asm` / `emu_arm_call_asm`.
- **Error path**: a bad mnemonic → `ok == false` and a non-empty `err`.
- **Syntax**: the same x86 routine in AT&T (`ASM_SYNTAX_ATT`) to prove the option.
- **Round-trip vs corpus**: assemble `add_signed` and check the bytes run
  identically to the linked symbol.

Wire `test_asm` into `make asm-test`, the Docker matrix, and CI
([.github/workflows](../../.github/workflows)) gated like the emu jobs (skip where
Keystone is absent, exactly as emu skips without Unicorn).

## Phase 6 — Bindings — **done**

Expose the in-line entry point through each binding, following the existing
DllImport/FFI pattern in [bindings/](../../bindings) (see
[multi-language-bindings-plan.md](multi-language-bindings-plan.md) for the shared
library module structure):

- **C ABI surface**: thin `asmtest_emu_call_asm(emu, src, a0, a1, out)`-style
  shims (like the existing `asmtest_emu_call2`) so bindings never deal with Keystone
  structs directly — they pass a string and read back the same `emu_result_t`.
- **.NET** ([bindings/dotnet/Asmtest.cs](../../bindings/dotnet/Asmtest.cs)): a
  `[DllImport]` for the shim plus an `Emu.CallAsm("mov rax,42; ret", …)` wrapper
  on the typed `Emu` class; a conformance case in
  [bindings/dotnet/Program.cs](../../bindings/dotnet/Program.cs).
- **Node / Ruby / Lua / Java** (the reusable library module from the bindings
  plan): the same `CallAsm` method and one conformance assertion each, replaying
  the x86-64 `add_signed` string and checking `rax == 42`.
- Keep it additive: the corpus replay stays the primary conformance path; the
  in-line case is one extra check per binding, skipped where Keystone is absent.

## Phase 7 — Docs — **done**

- [README.md](../../README.md): a "Pass assembly as a string (in-line)" subsection
  under the emulator tier, with an x86-64 example.
- [DESIGN.md](../../DESIGN.md) / [docs/design.md](../design.md): a phase entry for
  the in-line assembler tier.
- [CHANGELOG.md](../../CHANGELOG.md): the new API, `make asm-test`, the `--asm`
  deps flag, and the binding `CallAsm` method.
- This plan: flip phase statuses to **done** as they land.

---

## Risks & open points

- **Keystone RISC-V maturity** — RV64 support is newer/limited in some Keystone
  builds; may need a version guard or to ship RISC-V in-line as best-effort with a
  documented caveat.
- **Packaging gaps** — Keystone isn't in every distro's repos; the deps script
  needs a source-build fallback note for those (Phase 4).
- **Two LLVM-derived libs in one binary** — Unicorn and Keystone both vendor MC
  layers; symbol clashes are unlikely (distinct prefixes) but worth a smoke link
  test early in Phase 3.
- **Branch/relocation base** — assembling at `EMU_CODE_BASE` is mandatory for
  correct PC-relative targets; this is the one piece of shared state between the
  assembler and the emulator and must stay in sync (Phase 2 exposes it).
