# Native RISC-V (rv64) host tier — implementation

> **Sources.** Actioned from the
> [2026-07-04 repo review](../archive/reviews/2026-07-04-repo-review.md)
> (item **P3**, "Add a native RISC-V host tier" — status row line 165,
> strategic write-up lines 483–489) and
> [2026-07-04-plans-remaining-items.md](../analysis/2026-07-04-plans-remaining-items.md)
> (the 07-07 update names P3 as one of the two still-open review expansions).
> Written 2026-07-17. If this doc and a source disagree, this doc wins (sources
> may be stale); if the CODE and this doc disagree, re-verify before
> implementing.

## Why this work exists

asm-test's differentiating *native* capture tier — call an assembly routine
through the real ABI, capture return/callee-saved/flag state, assert on it —
exists only for x86-64 and AArch64. RISC-V is an **emulator guest only**
(Unicorn/Keystone, `emu_riscv_*`): you can emulate rv64 code on an x86 host,
but you cannot run asm-test *on* a RISC-V machine at all — the build stops at
`#error "asm-test supports x86-64 and AArch64 only"`. This work adds the rv64
(RV64GC, LP64D) host tier: a `regs_t` branch and callee-saved ABI check for the
RISC-V calling convention, `capture.s` trampolines, runner support (cycle
counter, big-struct dispatch), rv64 bodies for the example suites, and a
`linux/riscv64` Docker/binfmt CI lane mirroring the existing `linux/arm64`
lane. Per the CLAUDE.md dependency rule, the binfmt/QEMU lane is an
**installable dependency, not a gate** — no part of this tier may ship
validation-less.

## What already exists (verified 2026-07-17)

Every claim below was checked against the working tree on 2026-07-17.

- **RISC-V is guest-only today.** `grep -rn riscv src/ include/ mk/ Makefile
  Dockerfile*` hits only the emulator/assembler/disassembler tier:
  `emu_riscv_*` in [include/asmtest_emu.h](../../../include/asmtest_emu.h)
  (lines 268–305, including the comment "There is no flags register — RISC-V
  folds comparisons into its branches"), `ASM_RISCV64` in
  [src/assemble.c](../../../src/assemble.c), `ASMTEST_ARCH_RISCV64` in
  [src/disasm.c](../../../src/disasm.c). Nothing in the native tier: no
  `__riscv` anywhere in `src/capture.s`, `src/asmtest.c`, `include/asmtest.h`,
  or `examples/*.s`.
- **The header to extend**: [include/asmtest.h](../../../include/asmtest.h)
  has per-arch `regs_t` branches — Win64 (129–181), x86-64 SysV (183–223,
  offsets pinned by `ASMTEST_STATIC_ASSERT`), AArch64 (225–277) — then
  `#error "asm-test supports x86-64 and AArch64 only"` at lines 279–281. The
  per-arch sentinel macros (`ASMTEST_SENTINEL_*`) and flag masks live in the
  same branches. `asmtest_cycle_counter()` (507–520) has only
  x86-64/`rdtsc` and AArch64/`cntvct_el0` arms — on any other arch control
  falls off the end of a non-void function (a `-Wall` warning, an error under
  `WERROR=1`, and garbage at runtime — it needs a real rv64 arm, not luck).
- **The ABI checker**: `asmtest_check_abi` in
  [src/asmtest.c](../../../src/asmtest.c) lines 198–248 compares each arch's
  callee-saved registers against its sentinels via a per-arch `chk[]` table
  (`#if` Win64 / x86-64 / AArch64); on an unknown arch the table would be
  empty (a zero-size-array compile problem). `asmtest_check_abi_vec`
  (270–307, gated `ASMTEST_ABI_WIN64 || __aarch64__`) checks the FP/vector
  callee-saved set (AArch64: d8–d15 low halves seeded `8..15`, read back from
  `vec[i].u64[0]`); `asmtest_assert_abi_vec` shares the gate (309–315).
- **The trampolines to mirror**: [src/capture.s](../../../src/capture.s)
  (1820 lines, GAS + cpp) implements, per arch:
  `asm_call_capture` (x86-64 at 17–76, AArch64 at 78–140), `_fp`, `_vec`,
  `_fp_n`, `_vec_n`, `_vec256`/`_vec512` (x86-only, with `ret`-only `#else`
  stubs "so the symbol resolves on other arches" — the self-skip precedent,
  lines 1182–1186), `_args`, `_sret`, and x86-only `asm_bigstruct_x86`
  (1730–1819). Every non-stub arch switch ends
  `#error "capture.s supports x86-64 and AArch64 only"`.
  [include/asm.h](../../../include/asm.h)'s `ASM_FUNC`/`ASM_ENDFUNC` `#else`
  branch (ELF, `@function`, `@progbits`) is **already valid RISC-V GAS** — no
  new branch needed, only its "x86-64" comment updates.
- **Runner + helpers are otherwise portable C**: fork isolation, guard pages,
  TAP/JUnit, RNG — no other arch gates in the `make test` object set
  (`FRAMEWORK_OBJS := build/asmtest.o build/capture.o`,
  [Makefile](../../../Makefile) line 52). Exceptions found by grep:
  `asm_call_capture_bigstruct` dispatch in src/asmtest.c 573–598
  (`__aarch64__` pointer path vs x86 inline-copy path) and
  `ASMTEST_BENCH_UNIT` at 1826–1830 (`#else` already says "ticks", comment
  mentions cntvct only). [src/ffi.c](../../../src/ffi.c) compiles on any arch
  (its flag-name `#if` chain falls through to "unknown → 0", lines 39–65) but
  reads `r->flags` and `sizeof r->vec` (lines 26, 31), so the rv64 `regs_t`
  must keep both fields.
- **Suites & self-tests**: suites are auto-discovered `examples/test_foo.c` +
  `examples/foo.s` pairs (Makefile pattern rule; explicit rules 307–314 for
  `test_arith`→`add.o`, `test_capture`→`flags.o`, `test_struct`→`structs.o`).
  With `SUITE_EXCLUDES` (Makefile 60–71) removed, `make test` runs 14 suites:
  args, arith, callback, capture, checked, fp, fpover, mem, qadd, qmul,
  refmatch, simd, struct, structparam. Every one of their `.s` files is
  `#if __x86_64__ / #elif __aarch64__` only. `make check` runs
  [tests/expect.sh](../../../tests/expect.sh) over `tests_positive`/
  `tests_negative`, whose `preserved_regs()`/`clobbered_regs()` builders are
  arch-gated ([tests/positive.c](../../../tests/positive.c) 21–45,
  [tests/negative.c](../../../tests/negative.c) 24–48) and whose flag cases
  (`posit.flag_set`/`flag_clear`, `neg.flag_set`/`neg.flag_clear`; expect.sh
  pins the latter two at lines 115–116) hand-set `r.flags` with the
  `ASMTEST_CF`/`ZF` macros.
- **The Docker lane to mirror**: [Dockerfile](../../../Dockerfile)
  (`ARG BASE=ubuntu:24.04` line 18, `ARG DEPS_ARGS=--all` line 40 — Keystone
  is source-built only when `--asm`/`--all`) and
  [mk/docker.mk](../../../mk/docker.mk): `DOCKER_PLATFORM ?=` → `_docker_plat`
  (lines 23–28), `docker-build` (34–35), `docker-test` = `make test && make
  check` in the container (37–38). The header comment documents the arm64
  flow: "Emulate the aarch64 runner with DOCKER_PLATFORM=linux/arm64". The
  pinned-digest pattern for third-party fetches is the Zig block (docker.mk
  154–172) anchored in
  [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt).
- **CI**: [.github/workflows/ci.yml](../../../.github/workflows/ci.yml) `test`
  job matrix `os: [ubuntu-latest, ubuntu-24.04-arm, macos-latest]` (line 37) —
  the arm64 leg is a **native** runner; no workflow uses QEMU/binfmt today
  (grep for `tonistiigi`/`setup-qemu`: zero hits). There is no hosted riscv64
  runner, so the new leg must be the binfmt lane. CI installs `libxml2-utils`
  on Linux runners for `make check`'s JUnit validation (ci.yml lines 53–54);
  the Docker image does not have it, so in-container `make check` currently
  prints expect.sh's "SKIP junit XML validation" line.
- **Manifest**: [scripts/gen-manifest.c](../../../scripts/gen-manifest.c)
  prints the host arch from an `#if` chain (lines 48–54, `"unknown"`
  fallback); `make manifest` regenerates `asmtest_abi.json` for the *build
  host*. The committed root copy is x86_64-generated — do not commit an
  rv64-generated one.
- **Baseline proof before touching anything**: `make test && make check` exits
  0 on the host (per-suite `ok` lines; expect.sh ends `# N passed, 0 failed`),
  and `make docker-test` (add `DOCKER_PLATFORM=linux/arm64` on an x86 host to
  prove the emulated-lane plumbing) does the same in a container. If either is
  red, stop and fix that first.

## Tasks

### T1 — Add the rv64 branch to `regs_t`, the ABI checker, and the runner's arch gates  (M, depends on: none)

**Goal.** `include/asmtest.h`, `src/asmtest.c`, `src/ffi.c`, and
`scripts/gen-manifest.c` compile for `riscv64-linux-gnu` with a complete LP64D
integer callee-saved model, an honest no-flags policy, and a working cycle
counter — before any trampoline exists.

**Steps.**
1. In [include/asmtest.h](../../../include/asmtest.h), replace the `#else` /
   `#error` at lines 279–281 with a new branch
   `#elif defined(__riscv) && __riscv_xlen == 64` (keep the `#error` as the
   final `#else`). Mirror the AArch64 branch (225–277).
2. Define the rv64 `regs_t` (LP64/LP64D, psABI — see Research notes):

   ```c
   typedef struct {
       unsigned long ret;   /* 0   = a0 (return value)      */
       unsigned long a1;    /* 8   = second return register */
       unsigned long s0;    /* 16  callee-saved (frame ptr) */
       unsigned long s1;    /* 24  */
       /* s2..s10 at 32..96 */
       unsigned long s11;   /* 104 */
       unsigned long flags; /* 112 = always 0: RISC-V has NO flags register */
       double fret;         /* 120 = fa0; valid after asm_call_capture_fp  */
       vec128_t vec[32];    /* 128 = fs-slots after _fp (see T3); no vector
                               capture on rv64 (rv64gc has no V registers) —
                               see T4 for the ASM_VCALL gate */
   } regs_t;
   ```

   Pin with `ASMTEST_STATIC_ASSERT`: `ret@0`, `a1@8`, `s0@16`, `s11@104`,
   `flags@112`, `fret@120`, `vec@128`, `sizeof == 640` (mirror lines
   271–277).
3. Define 12 sentinels `ASMTEST_SENTINEL_S0 0x1111111111111111UL` …
   `ASMTEST_SENTINEL_S11 0xCCCCCCCCCCCCCCCCUL` (extend the AArch64
   1..B nibble series to C).
4. **Flags policy (the review's honesty point): define NO flag-mask macros on
   rv64.** RISC-V has no condition-flags register; `r.flags` stays in the
   struct (always written 0 by the trampolines — src/ffi.c and shared runner
   code read it) but `ASMTEST_CF/ZF/SF/OF/...` are deliberately absent, so
   `ASSERT_FLAG_SET(&r, CF)` is a **compile error** on rv64 (the macro
   expands `ASMTEST_##FLG`, asmtest.h 1044–1047) rather than a silent
   always-false check. Add `#define ASMTEST_NO_FLAGS 1` in the rv64 branch
   and a comment block explaining it (cite the emulator header's existing
   wording: comparisons fold into branches).
5. Add the rv64 arm to `asmtest_cycle_counter()` (asmtest.h 507–520):
   `__asm__ __volatile__("rdtime %0" : "=r"(v));` — `rdtime` (the
   constant-frequency time CSR), **not** `rdcycle`, which recent kernels
   restrict in user mode. Update the doc comment; `ASMTEST_BENCH_UNIT`'s
   `#else "ticks"` branch (src/asmtest.c 1826–1830) already covers rv64 —
   just extend its comment.
6. In [src/asmtest.c](../../../src/asmtest.c) `asmtest_check_abi` (198–248),
   add the `#elif defined(__riscv) && __riscv_xlen == 64` rows:
   `{"s0", r->s0, ASMTEST_SENTINEL_S0}` … `{"s11", …}` (all 12; the base
   trampolines seed and check s0 too — only the variable-arity paths use it
   as a frame pointer, mirroring rbp/x29, see T2).
7. Extend `asm_call_capture_bigstruct` (573–598): change the pointer-passing
   branch to `#if defined(__aarch64__) || (defined(__riscv) && __riscv_xlen == 64)`
   — psABI passes >2×XLEN (>16-byte) aggregates by reference, same as
   AAPCS64. Update the AArch64-specific wording in the asmtest.h comment above
   `asm_call_capture_bigstruct` (361–372) with an rv64 note: a small
   `{long;double}` struct uses the **hardware FP calling convention** on
   LP64D (long → a0, double → fa0 — the x86-ish `ASM_MIXCALL` shape, *not*
   AArch64's two-GP-registers shape).
8. In [scripts/gen-manifest.c](../../../scripts/gen-manifest.c) (48–54) add
   `#elif defined(__riscv) && __riscv_xlen == 64` → `"riscv64"`. In
   [src/ffi.c](../../../src/ffi.c) `asmtest_regs_flag_set` (39–65) add an
   explanatory comment that rv64 intentionally resolves every name to 0.
   Update [include/asm.h](../../../include/asm.h)'s "x86-64" `#else` comments
   to "x86-64, RISC-V, …".
9. Compile gate (no runnable trampoline yet): on any x86-64 Ubuntu box or in
   a container, `apt-get install gcc-riscv64-linux-gnu` (4:13.2.0-7ubuntu1 in
   noble) and run
   `riscv64-linux-gnu-gcc -std=gnu11 -D_DEFAULT_SOURCE -Wall -Wextra -Iinclude -fsyntax-only src/asmtest.c src/ffi.c`
   — expect a **clean, silent exit 0** (the `-fsyntax-only` idiom the
   Makefile's `check-header-portability` target already uses). The full lane
   arrives in T5; per CLAUDE.md prefer running this inside the T5 container
   once it exists.

**Code.** As above. No behavior change on x86-64/AArch64/Win64: every edit is
a new `#elif` branch or a comment.

**Tests.** Compile-only at this stage (step 9). The real assertions land with
T2 (`make check` exercises `asmtest_check_abi` through
`posit.abi_preserved` / `neg.abi` once `preserved_regs`/`clobbered_regs` gain
rv64 branches in T4). Regression guard for the existing arches: `make test &&
make check` on the host stays green after this task.

**Docs.** Internal-only for now; user-facing docs land in T7 (one pass, after
the tier works).

**Done when.**
- `riscv64-linux-gnu-gcc … -fsyntax-only src/asmtest.c src/ffi.c` exits 0
  with no warnings.
- `make test && make check` still green on the x86-64/AArch64 host.
- `grep -n ASMTEST_NO_FLAGS include/asmtest.h` shows the rv64 branch defines
  it and no other branch does.

### T2 — Write the rv64 trampolines in capture.s  (L, depends on: T1)

**Goal.** Every `asm_call_capture*` entry point assembles and behaves
correctly on rv64: full psABI marshalling, sentinel seeding, capture of
a0/a1/s0–s11, `flags` stored as 0, `fret` = fa0.

**Steps.**
1. In [src/capture.s](../../../src/capture.s), add an
   `#elif defined(__riscv) && __riscv_xlen == 64` branch to **each** function's
   arch switch (keep every `#error` as the final `#else`, now reading
   "…x86-64, AArch64 and RV64 only" — also update asmtest.h's top-of-branch
   comments). Functions and their rv64 shapes:
   - `asm_call_capture(out=a0, fn=a1, args=a2)`: 128-byte frame
     (`addi sp, sp, -128`; sp stays 16-aligned per psABI); save
     `ra` at 0, `s0..s11` at 8..96, stash out at 104, fn at 112. Marshal
     `mv t1, a2` then `ld a0..a5, 0..40(t1)` (6 slots — the portable API
     width, mirroring AArch64 which also loads only x0..x5). Seed
     `li s0, 0x1111111111111111` … `li s11, 0xCCC…C` (the assembler's `li`
     pseudo materializes 64-bit constants). `ld t0, 112(sp); jalr ra, 0(t0)`.
     Capture: `ld t1, 104(sp)`; `sd a0, 0(t1)`, `sd a1, 8(t1)`,
     `sd s0..s11, 16..104(t1)`, `sd zero, 112(t1)` (flags ≡ 0). Restore
     ra/s0–s11, `addi sp, sp, 128; ret`.
   - `asm_call_capture_fp(out, fn, iargs=a2, fargs=a3)`: additionally
     `fld fa0..fa7, 0..56(t2)` from fargs, and after the call
     `fsd fa0, 120(t1)` (fret). (T3 adds fs seeding/capture here.)
   - `asm_call_capture_args(out, fn, args=a2, nargs=a3)`: **8** register args
     (`a0..a7` — unlike the 6-slot base path), overflow `args[8+]` copied to
     `0(sp), 8(sp)…` (first stack arg at offset 0, psABI). Use `s0` as the
     frame pointer to unwind the variable area (mirror x29 in the AArch64
     branch at 1396–1501); report `s0` preserved-but-not-checked by storing
     the `ASMTEST_SENTINEL_S0` constant into `out` (the rbp/x29 idiom —
     capture.s line 1488).
   - `asm_call_capture_fp_n(out, fn, iargs, fargs, nfargs)`: first 8 doubles
     → `fa0..fa7`; **doubles 9 and 10 → `a6`, `a7` as raw bit patterns
     (`ld` from fargs), 11+ → stack**. This is the psABI rule "FP args after
     the FP registers are exhausted use the integer convention": since this
     API always marshals `iargs[0..5]` into `a0..a5`, the next free integer
     registers are a6/a7. Document in asmtest.h (near lines 323–330): the
     placement is psABI-exact for a callee whose prototype has ≥6 leading
     integer parameters, and is the *framework's documented convention* for
     hand-written `.s` routines otherwise; a **C-compiled** callee with >8 FP
     args and <6 integer parameters expects different registers — a recorded
     limitation (see T7).
   - `asm_call_capture_sret(out, fn, result, args, nargs)`: rv64 passes the
     >16-byte-return hidden pointer as the **implicit first argument in a0**
     (the x86-64/rdi shape, *not* AArch64's x8): `result → a0`, visible args
     → `a1..a7` (7 register slots), overflow `args[7+]` → stack.
   - `asm_call_capture_vec`, `_vec_n`: rv64gc has **no vector registers** —
     add `ret`-only stubs with the exact comment idiom of the AVX2 stub
     (capture.s 1182–1186): "Never called: asmtest_cpu_has_vec128() is false
     on rv64, so the ASM_VCALL macros self-skip" (probe lands in T4).
     `_vec256`/`_vec512` already fall into their existing non-x86 stubs —
     verify, don't duplicate.
   - `asm_bigstruct_x86` stays x86-only (T1's dispatch routes rv64 through
     the pointer path).
2. After each function, rebuild in the T5 container (or with the cross
   toolchain: `riscv64-linux-gnu-gcc -x assembler-with-cpp -Iinclude -c
   src/capture.s -o /dev/null`) — zero errors, zero warnings.

**Code.** As above. Register-use discipline: t-registers (t0–t2, t3–t6) and
a-registers are caller-saved — never live across the `jalr`; reload out/fn
from the frame. `gp`/`tp` are unallocatable — never touch them. All stores to
`out` after the call are plain `sd`/`fsd` (nothing to preserve — there are no
flags to avoid disturbing, unlike the x86/AArch64 branches' careful ordering).

**Tests.** No new test files: the existing suites/self-tests are the test
surface once T4 lands. Interim smoke inside the T5 container:
`make build/test_arith && ./build/test_arith` (needs add.s's rv64 body from
T4 — coordinate; T2+T4 can be developed in lock-step per-suite). A failure
looks like `not ok … ASSERT_ABI_PRESERVED: s3 not restored (got 0x…, expected
0x4444444444444444)`; a pass is the plain `ok` TAP line.

**Docs.** Internal-only (T7 does the user-facing pass).

**Done when.**
- `src/capture.s` assembles for rv64 with zero warnings.
- In the T5 container, every `make test` suite binary runs green (with T4).
- On x86-64 and AArch64 hosts `make test && make check` is byte-identical
  green (no shared-branch edits).

### T3 — FP callee-saved (fs0–fs11) preservation check  (M, depends on: T2)

**Goal.** `ASSERT_ABI_PRESERVED_VEC` works on rv64: a routine that clobbers a
callee-saved FP register (`fs0–fs11`, preserved to FLEN=64 bits under LP64D)
is caught.

**Steps.**
1. rv64 has no `_vec` path, so the FP-preservation seed/capture lives in the
   `_fp` and `_fp_n` trampolines (divergence from AArch64, where only the
   `_vec` paths seed d8–d15 — document it in the asmtest.h comment at
   546–556). In `asm_call_capture_fp`: save the caller's fs0–fs11 to the
   frame (`fsd`, 96 more bytes), seed each with its **f-register number** as
   an integer bit pattern (`li t0, 8; fmv.d.x fs0, t0` … fs1←9, fs2←18 …
   fs11←27 — fs0/fs1 are f8/f9, fs2–fs11 are f18–f27), and after the call
   store each into `out->vec[reg#].u64[0]` (`vec[8]`, `vec[9]`,
   `vec[18..27]` — the AArch64 "index = register number" idiom), then restore
   the caller's values. Zero `vec[i].u64[1]` for those slots.
2. In [src/asmtest.c](../../../src/asmtest.c) extend the
   `asmtest_check_abi_vec` gate (270–307) with an rv64 branch that loops over
   the register-number list `{8, 9, 18, 19, …, 27}` checking
   `r->vec[i].u64[0] == i`, message `"fs%u not restored …"` (map index→fs
   name). Extend the `asmtest_assert_abi_vec` gate (309–315) and the
   asmtest.h declaration gate (545–558) to
   `|| (defined(__riscv) && __riscv_xlen == 64)`.
3. Note in the header: on rv64 `ASSERT_ABI_PRESERVED_VEC` is valid after an
   `_fp`/`_fp_n` capture (not `_vec`, which self-skips).

**Code.** As above; ~60 lines of asm, ~20 of C.

**Tests.** Extend [tests/positive.c](../../../tests/positive.c) /
[tests/negative.c](../../../tests/negative.c) in T4's arch branches: rv64
`preserved_regs()` also sets `r.vec[8].u64[0]=8` … `r.vec[27].u64[0]=27`; add
an rv64-gated negative case (an fs slot holding `0xDEAD`) asserting
`ASSERT_ABI_PRESERVED_VEC` fails with `fs… not restored`. Real-capture proof:
an rv64-gated routine in [examples/flags.s](../../../examples/flags.s) that
clobbers `fs2` pairs with an ABI-violation check in the failure-demo style
(mirror `clobbers_rbx`).

**Docs.** Covered by T7's ABI-capture page update.

**Done when.**
- In the riscv64 container, a routine clobbering `fs2` fails
  `ASSERT_ABI_PRESERVED_VEC` with `fs2 not restored`; the compliant path
  passes.
- x86-64/AArch64 `make check` unchanged.

### T4 — rv64 bodies for the examples and self-tests; gate the flag/vector surface  (M, depends on: T2)

**Goal.** `make test && make check` is green on rv64 with **no vacuous
passes**: every routine that can exist on rv64 does; flag-only and
vector-only surfaces are explicitly gated with printed reasons.

**Steps.**
1. Add `#elif defined(__riscv) && __riscv_xlen == 64` bodies to the 14
   suites' `.s` files (all currently x86-64/AArch64-only). The mechanical
   ones: [add.s](../../../examples/add.s) (`add a0, a0, a1; ret`),
   [args.s](../../../examples/args.s) (`sum3`/`sum8`/`sum10` — 8 register
   args, sum10 reads 2 stack args at `0(sp)`/`8(sp)`),
   [mem.s](../../../examples/mem.s), [qadd.s](../../../examples/qadd.s),
   [qmul.s](../../../examples/qmul.s),
   [refmatch.s](../../../examples/refmatch.s) (branchless via `slt`/masks),
   [callback.s](../../../examples/callback.s) (callee-saved loop regs,
   `jalr` the function pointer, 16-aligned frame),
   [fp.s](../../../examples/fp.s) (`fadd.d fa0, fa0, fa1`;
   `fcvt.d.l fa0, a0`; mix_scale reads a0 + fa0 — LP64D),
   [bench.s](../../../examples/bench.s)/[robust.s](../../../examples/robust.s)/
   [fault.s](../../../examples/fault.s) (used by `bench`/`demo-robust`/
   `demo-fail` and the bindings corpus, cheap to include now).
2. The convention-sensitive ones (get these from T2's documented shapes):
   - [fpover.s](../../../examples/fpover.s): `fp_sum10`/`fp_stack2` take 10
     doubles via `ASM_FCALLN` → on rv64 doubles 9–10 arrive in **a6/a7**
     (T2's `_fp_n` convention). Write the bodies against that placement and
     say so in the file comment — this test is what *proves* the documented
     convention.
   - [structparam.s](../../../examples/structparam.s): `pst2`
     ({long,long} = 2×XLEN → a0/a1 pair, same as both arches); `pst_mixed`
     ({long;double} → a0 + fa0 per the psABI hardware-FP convention — the
     test's existing `#else`/`ASM_MIXCALL` arm in
     [test_structparam.c](../../../examples/test_structparam.c) (lines 37–43)
     is **already correct for rv64**; only extend its comment, do not add a
     new arch arm); `bigsum` (24 bytes → by reference; T1's dispatch covers
     it, test unchanged).
   - [structs.s](../../../examples/structs.s): sret routines write through
     the hidden pointer arriving in **a0** (visible args from a1). In
     [test_struct.c](../../../examples/test_struct.c)
     `structret.small_struct_in_registers` (41–52), add an rv64 arm beside
     the `#if defined(__x86_64__)` one: `ASSERT_EQ(r.a1, 9)` — rv64 captures
     the second return register (a1), which AArch64 does not (x1).
3. Flag-only surface — compile out on rv64 with an in-suite skip marker:
   - [flags.s](../../../examples/flags.s): `set_carry`/`clear_carry` have no
     rv64 analog — guard them (and their `extern`s/tests) with
     `#if !defined(ASMTEST_NO_FLAGS)`; give `sum_via_rbx`/`clobbers_rbx` rv64
     bodies using `s1` (mirror the AArch64 x19 bodies).
   - [test_capture.c](../../../examples/test_capture.c): keep
     `capture.return_value_captured`, `capture.callee_saved_preserved`, and
     both guard tests; gate `carry_flag_set`/`carry_flag_clear`; in the
     `#else` add `TEST(capture, flags_unavailable_rv64) {
     SKIP("RISC-V has no condition-flags register (ASMTEST_NO_FLAGS)"); }`.
   - [test_checked.c](../../../examples/test_checked.c) /
     [checked.s](../../../examples/checked.s): the whole suite is the
     overflow *flag* — gate all four tests, add one SKIP-marker test with the
     same reason, and give `checked_add` an rv64 body returning the wrapping
     sum (so the symbol links) with a comment pointing at
     `__builtin_add_overflow`-style value-returning idioms as the rv64
     equivalent.
4. Vector surface: add `int asmtest_cpu_has_vec128(void)` beside the AVX
   probes (src/asmtest.c 461–498; declaration next to asmtest.h 299–303):
   returns 1 on x86-64/AArch64, 0 on rv64 (until an RVV arm exists). Add
   `if (!asmtest_cpu_has_vec128()) SKIP("128-bit vector capture not available
   on this target");` to `ASM_VCALL1/2/3/N` (asmtest.h 882–919), mirroring
   `ASM_VCALL256_*` (924–941). Give
   [simd.s](../../../examples/simd.s)'s `vec_add4f` a `ret`-only rv64 stub
   (never called — the macros skip first). `test_simd` then reports `skip`
   lines, not failures.
5. Self-tests: add rv64 branches to `preserved_regs()`/`clobbered_regs()`
   (s0–s11 sentinels; clobber `s11 = 0xDEAD`), gate the **four** flag TESTs
   (`posit.flag_set`/`flag_clear`, `neg.flag_set`/`flag_clear`) with
   `#if !defined(ASMTEST_NO_FLAGS)`, and in
   [tests/expect.sh](../../../tests/expect.sh) wrap the two flag checks
   (lines 115–116) in an arch guard:
   `if [ "$(uname -m)" != "riscv64" ]; then … else printf '# skip: no flags register on riscv64\n'; fi`.
6. Run the whole thing in the T5 container after each suite conversion.

**Code.** As above. Most edits are inside a new arch branch or an
`ASMTEST_NO_FLAGS` guard. The exception is step 4, which touches shared code:
it adds the new `asmtest_cpu_has_vec128()` probe and an
`if (!asmtest_cpu_has_vec128()) SKIP(...)` runtime guard to the shared
`ASM_VCALL1/2/3/N` macros (asmtest.h 882–919). Those edits are still
behavior-preserving on x86-64/AArch64 because the probe returns 1 there, so
those arches never take the skip branch — the guard only bites on rv64.
Existing arches see no diff in behavior.

**Tests.** This task IS tests. Pass: in the riscv64 container `make test`
prints all suites green (with `skip` lines only for simd/flag markers whose
reasons name the ISA fact) and `make check` ends `# N passed, 0 failed`
(N smaller than on x86 by exactly the gated flag checks). Fail example:
`not ok … ASSERT_EQ(r.ret, 5)` from a mis-marshalled argument, or an expect.sh
`not ok - ASSERT_FLAG_SET fails` if the expect.sh guard was forgotten.

**Docs.** Covered by T7.

**Done when.**
- Riscv64 container: `make test && make check` exits 0; `make test 2>&1 |
  grep -ci skip` matches exactly the documented skip set.
- x86-64 + AArch64 (`make docker-test` and
  `make docker-test DOCKER_PLATFORM=linux/arm64`): identical results to the
  pre-task baseline, including the two expect.sh flag checks still running.

### T5 — The riscv64 Docker/binfmt lane  (S, depends on: T1 (compile targets); enables T2–T4 validation)

**Goal.** `make docker-riscv64` builds the ubuntu:24.04 riscv64 image and
runs `make test && make check` in it on any Docker host, with the QEMU binfmt
dependency pinned and installable via a make target.

**Steps.**
1. In [mk/docker.mk](../../../mk/docker.mk), add a `DOCKER_DEPS_ARGS ?= --all`
   knob and pass `--build-arg DEPS_ARGS=$(DOCKER_DEPS_ARGS)` in `docker-build`
   (lines 34–35). Default unchanged; the riscv lane overrides it so the
   Keystone LLVM source build never runs under qemu-user emulation (hours).
2. Add, near the arm64 comment block (lines 20–22):

   ```make
   # riscv64 lane (riscv-native-tier). ubuntu:24.04 is multi-arch incl.
   # linux/riscv64; Docker Desktop ships emulators, a Linux host runs
   # `make binfmt-riscv64` once. DEPS_ARGS=--pkgconfig: the core suites need
   # no optional engine, and Keystone-under-qemu would take hours.
   # <digest>: fill from step 3 before the first run (placeholder, not runnable as-is)
   BINFMT_IMAGE ?= tonistiigi/binfmt:qemu-v10.2.3@sha256:<digest>
   .PHONY: binfmt-riscv64 docker-riscv64
   binfmt-riscv64:
   	$(DOCKER) run --privileged --rm $(BINFMT_IMAGE) --install riscv64
   docker-riscv64:
   	$(MAKE) docker-build DOCKER_PLATFORM=linux/riscv64 \
   	  DOCKER_IMAGE=asmtest-ci-riscv64 DOCKER_DEPS_ARGS=--pkgconfig
   	$(DOCKER) run --rm --platform linux/riscv64 asmtest-ci-riscv64 \
   	  sh -c 'uname -m && make test && make check'
   ```

   The distinct `asmtest-ci-riscv64` tag keeps the host-arch `asmtest-ci`
   image intact (the arm64 flow reuses the tag; don't copy that wart).
3. Resolve `<digest>` at adoption time:
   `docker buildx imagetools inspect tonistiigi/binfmt:qemu-v10.2.3` — pin
   `tag@sha256:…` in `BINFMT_IMAGE` and append an anchor line to
   [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt)
   following the Zig entries' style (new kind `image-digest`, name
   `tonistiigi-binfmt`, version `qemu-v10.2.3`; the file is a record — no
   fetch script consumes this entry, say so in the line's comment).
4. Add `libxml2-utils` to the [Dockerfile](../../../Dockerfile)'s base
   `apt-get install` line (lines 25–28) — CI runners install it for `make
   check`'s JUnit XML validation (ci.yml 53–54) but the image never had it,
   so every docker lane (this one included) silently printed expect.sh's
   "SKIP junit XML validation". Per the CLAUDE.md dependency rule that skip
   is an installable, not a gate. Also update the Dockerfile header comment
   (lines 3–5) to mention riscv64 beside arm64. No other Dockerfile change:
   `build-essential` in the riscv64 noble image is the same gcc-13 toolchain
   (see Research notes).
5. `make help`: add `docker-riscv64` under the Docker section
   ([Makefile](../../../Makefile) help text, "Docker (Linux CI lanes)",
   lines 159–165).
6. Validate end to end on the dev host: `make binfmt-riscv64` (skip on Docker
   Desktop, note the message), then `make docker-riscv64`. First run is slow
   (qemu-user TCG); expect green with the T4 skip set.

**Code.** As above.

**Tests.** The lane run itself: `make docker-riscv64` exits 0 and the run's
first output line is `riscv64` from the in-container `uname -m` (mirrors the
CI "Toolchain versions" step) — this catches a mis-installed binfmt silently
running the host arch. Failure mode without binfmt on a Linux host:
`exec format error` from `docker run` — that plus the `binfmt-riscv64` hint
is the expected diagnostic. In-container `make check` output must now show
the two "junit is well-formed XML" `ok` lines, not the xmllint SKIP.

**Docs.** Covered by T7 (portability + CI pages). Changelog entry lands there
too.

**Done when.**
- `make docker-riscv64` on an x86-64 Linux or macOS/Docker-Desktop host exits
  0 and its log contains `riscv64` from `uname -m`.
- `make docker-test` (host arch) unaffected; `docker-build` default deps
  unchanged (`--all` still in the image history); its `make check` now runs
  the xmllint validation (no SKIP line).

### T6 — CI leg for the riscv64 lane  (S, depends on: T4, T5)

**Goal.** Every push runs the rv64 core suites+self-tests in CI via the
binfmt lane.

**Steps.**
1. In [.github/workflows/ci.yml](../../../.github/workflows/ci.yml), add a
   job after the `test` job (which cannot grow a matrix row — there is no
   hosted riscv64 runner; this leg is Docker-under-QEMU, unlike the native
   `ubuntu-24.04-arm` row at line 37):

   ```yaml
   # Native rv64 host tier (riscv-native-tier). No hosted riscv64 runner
   # exists, so this leg runs the core suites in a linux/riscv64 container
   # under QEMU binfmt (tonistiigi/binfmt, pinned in mk/docker.mk). Slower
   # than native legs by ~10-20x — hence the fat timeout and core-only scope.
   test-riscv64:
     name: test (riscv64 container, qemu binfmt)
     runs-on: ubuntu-latest
     timeout-minutes: 45
     steps:
       - uses: actions/checkout@v7
       - name: Enable riscv64 binfmt (pinned QEMU)
         run: make binfmt-riscv64
       - name: Core suites + self-tests in the riscv64 container
         run: make docker-riscv64
   ```

2. Do NOT touch the macOS legs (global position: any Intel-mac leg is
   `macos-15-intel`, never `macos-13` — not relevant here but binding).
3. Update [docs/reference/ci.md](../../../docs/reference/ci.md)'s job list
   (part of T7's pass, but the ci.md edit ships in this task's PR so the job
   list never drifts — the K7 lesson from the review).
4. Validate on a real Actions run (push to a branch, or `workflow_dispatch`);
   confirm the leg's log shows `riscv64` and green TAP.

**Code.** As above.

**Tests.** The Actions run is the test. Failure modes to recognize: timeout
(raise to 60 only with evidence; investigate a qemu-hang first) and
`exec format error` (binfmt step didn't run/failed — its log must show
`installing: riscv64 OK`).

**Docs.** ci.md job list (step 3); the rest in T7.

**Done when.**
- A green `test-riscv64` leg on an actual Actions run, linked in the PR.
- Total leg wall time recorded in the PR description (baseline for future
  timeout tuning).

### T7 — User-facing docs, changelog, and the honesty notes  (S, depends on: T4–T6)

**Goal.** A user reading the docs knows rv64 is a first-class native host,
what the two documented divergences are (no flags; `_fp_n` >8-double
placement), and how to run the lane.

**Steps.**
1. [README.md](../../../README.md): update the portability bullets (lines 51
   and 171 say "x86-64 and AArch64" / "x86-64 or AArch64") to add "RISC-V
   (rv64)"; keep the emulator bullet (line 36) as-is (guests unchanged).
2. [docs/index.md](../../../docs/index.md) line 69 same fix.
3. [docs/reference/portability.md](../../../docs/reference/portability.md):
   add the rv64 host column/section — LP64D baseline (rv64gc), callee-saved
   set s0–s11 + fs0–fs11, **no condition flags** (`ASMTEST_NO_FLAGS`;
   `ASSERT_FLAG_*` is a compile error by design), no 128-bit vector capture
   (`ASM_VCALL*` self-skips; RVV is a possible future arm), tracing tiers not
   ported (link the out-of-scope list below).
4. [docs/guides/abi-capture.md](../../../docs/guides/abi-capture.md): document
   the rv64 `regs_t` fields, the a0/a1 return pair (note: rv64 captures the
   second return register — AArch64 does not, per the existing `_sret` note in
   asmtest.h 348–359), `ASSERT_ABI_PRESERVED_VEC`-after-`_fp` (T3), and the
   `_fp_n` >8-doubles placement rule with its exact limitation wording from
   T2.
5. [docs/reference/ci.md](../../../docs/reference/ci.md): the `test-riscv64`
   leg (if not already landed via T6 step 3) and the
   `make binfmt-riscv64`/`make docker-riscv64` local flow.
6. [CHANGELOG.md](../../../CHANGELOG.md) under `## [Unreleased]` / `### Added`:
   one entry — native RISC-V (rv64) host tier: LP64D capture trampolines +
   ABI checks, riscv64 Docker/binfmt lane (`make docker-riscv64`), CI leg;
   flags/vector surfaces explicitly gated on rv64.
7. `make docs` (or `make docker-docs`) — Sphinx `-W` build must pass (a bad
   relative link fails the build; `docs/internal/**` is excluded from the
   published build, so any published-page link to this doc must be a GitHub
   blob URL).

**Code.** Docs only.

**Tests.** `make docs` exits 0; `make docs-linkcheck` clean for the touched
pages.

**Docs.** This task.

**Done when.**
- `grep -rn "RISC-V" README.md docs/index.md docs/reference/portability.md`
  shows the native-tier mention in all three.
- `make docs` green; changelog entry present.

## Task order & parallelism

```
T1 (header/checker/runner)
 ├─→ T2 (trampolines) ──→ T3 (fs check)
 │        └──────────────→ T4 (examples/self-tests)
 └─→ T5 (docker lane)  ← validates T2/T3/T4 as they land
T4 + T5 ──→ T6 (CI leg) ──→ T7 (docs)
```

- **Critical path**: T1 → T2 → T4 → T6 → T7.
- T5 needs only T1 conceptually (the image builds off an unmodified
  Dockerfile) — build it **early** and use it as the validation loop for
  T2–T4; a second person can do T5 while the first does T2.
- T3 is independent of T4 (different files) — parallelizable after T2.
- Do T2 and T4 suite-by-suite in lock-step (a trampoline without a routine
  body has no runnable proof, and vice versa).

## Constraints & gates

- **No hardware gate.** Per CLAUDE.md, the binfmt/QEMU lane is an installable
  dependency: it is added (T5/T6), pinned, and the tier is CI-validated under
  qemu-user. A physical RISC-V board is *nice-to-have* corroboration, not a
  gate — do not write any self-skip predicated on "no RISC-V hardware".
- **Real gates that stay**: the tracing tiers (single-step/ptrace, DynamoRIO,
  hardware trace) are NOT ported by this work (see Out of scope); qemu-user
  additionally cannot emulate a ptrace tracer/tracee pair (recorded in
  [2026-07-04-plans-remaining-items.md](../analysis/2026-07-04-plans-remaining-items.md)
  §2), so a future rv64 tracing port would need real hardware or system-mode
  emulation — record that then, not now.
- **Honesty gates in-tier**: no flag macros on rv64 (compile error beats
  silent always-false); `ASM_VCALL*` skip with a printed ISA reason; the
  `_fp_n` >8-doubles placement limitation is documented where users will read
  it (T2/T7). A green rv64 lane whose skip count differs from the documented
  set is a regression, not a pass.
- **Pinning**: `tonistiigi/binfmt` by `tag@sha256` digest in docker.mk +
  anchor line in scripts/third-party-digests.txt (T5). The base image follows
  the repo-wide `ubuntu:24.04` convention (not digest-pinned anywhere — do
  not diverge unilaterally; the riscv64 manifest digest is recorded below for
  the day the repo adopts digest pins).
- **Keystone/Unicorn on rv64**: deliberately excluded from the lane
  (`DEPS_ARGS=--pkgconfig`). Whether noble's riscv64 port ships
  `libunicorn-dev`, and whether the repo-pinned Keystone 0.9.2 / Capstone
  5.0.1 build on an rv64 host, is **unverified** — the emulator tier on an
  rv64 host is follow-on work; adding it later means extending the image per
  the CLAUDE.md pattern, not self-skipping.
- **Do not commit** an rv64-regenerated `asmtest_abi.json` (the root copy is
  x86_64-generated; the manifest is a per-host build artifact).

## Research notes (verified 2026-07-17)

- **psABI (LP64/LP64D), integer convention** — callee-saved: `s0`–`s1`
  (x8–x9), `s2`–`s11` (x18–x27), plus `sp` (x2); `s0` doubles as the frame
  pointer when one is used. Caller-saved: `ra` (x1), `t0`–`t6`, `a0`–`a7`.
  `gp` (x3)/`tp` (x4) unallocatable; `x0` hardwired zero. Args in `a0`–`a7`,
  returns in `a0`/`a1`; 2×XLEN scalars use an (aligned) register pair,
  >2×XLEN by reference; variadic args pass like named args; FP args use
  `fa0`–`fa7` then the integer convention once FP registers are exhausted.
  Stack grows down, `sp` 16-byte (128-bit) aligned at procedure entry, first
  stack arg at offset 0. LP64D callee-saved FP: `fs0`–`fs1` (f8–f9),
  `fs2`–`fs11` (f18–f27), preserved up to ABI_FLEN bits.
  <https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc>
  (ratified v1.0:
  <https://docs.riscv.org/reference/abi/v1.0/riscv-cc-procedure-calling-convention.html>;
  master is a living draft with auto-generated pre-releases:
  <https://github.com/riscv-non-isa/riscv-elf-psabi-doc/releases>).
- **Baseline ISA/ABI**: rv64gc + lp64d is the Linux-distro baseline
  (Debian/Fedora agreed) — hence "no V registers" is a baseline fact, not a
  qemu artifact. <https://wiki.debian.org/RISC-V>
- **Toolchains**: Ubuntu 24.04 cross package `gcc-riscv64-linux-gnu`
  4:13.2.0-7ubuntu1 (gcc-13-based)
  (<https://packages.ubuntu.com/gcc-riscv64-linux-gnu>,
  <https://answers.launchpad.net/ubuntu/noble/amd64/cpp-13-riscv64-linux-gnu/13.2.0-23ubuntu4cross1>);
  native gcc inside the riscv64 noble image comes from the same gcc-defaults
  13.x (<https://launchpad.net/ubuntu/noble/+source/gcc-defaults>), so
  `build-essential` mirrors the arm64 lane exactly. Use explicit
  `-march=rv64gc -mabi=lp64d` if ever needed — GCC 13/14 do **not** accept
  profile strings (`rva22u64`) in `-march`
  (<https://gcc.gnu.org/gcc-14/changes.html>; profile-macro support still in
  patch review Oct 2025:
  <https://gcc.gnu.org/pipermail/gcc-patches/2025-October/696698.html>).
  Clang fully supports rv64gc codegen and does accept profile strings
  (<https://llvm.org/docs/RISCVUsage.html>). Source-built alternative:
  <https://github.com/riscv-collab/riscv-gnu-toolchain>.
- **Base image**: `ubuntu:24.04` is multi-arch including linux/riscv64 —
  riscv64 manifest digest
  `sha256:4edded5722eb644868b7b976033d241d2ab3fff0a170924df69b200a59a2b994`
  (list digest `sha256:4fbb8e…7092d90`, updated 2026-07-02)
  (<https://hub.docker.com/v2/repositories/library/ubuntu/tags/24.04>,
  <https://hub.docker.com/_/ubuntu>, Canonical per-arch repo
  <https://hub.docker.com/r/riscv64/ubuntu>). Fallback base if ever needed:
  Debian 13 "trixie", the first Debian with official riscv64 (released
  2025-08-09, <https://www.debian.org/News/2025/20250809>;
  <https://hub.docker.com/r/riscv64/debian/>).
- **binfmt**: `tonistiigi/binfmt` supports riscv64 and bundles qemu-riscv64;
  enable with `docker run --privileged --rm tonistiigi/binfmt --install
  riscv64` (<https://github.com/tonistiigi/binfmt/blob/master/README.md>).
  Pinnable tags follow `qemu-vX.Y.Z`; newest `qemu-v10.2.3` (pushed
  2026-06-08; prior pins `qemu-v10.2.1`, `qemu-v9.2.2`)
  (<https://hub.docker.com/v2/repositories/tonistiigi/binfmt/tags?page_size=30&name=qemu>).
  **Per-tag digest not captured at research time — resolve during T5 step 3.**
  Docker Desktop ships emulators already (matches the Dockerfile's arm64
  comment). Host-side apt alternative: noble's `qemu-user-static`
  1:8.2.2+ds-0ubuntu1.x covers riscv64
  (<https://launchpad.net/ubuntu/noble/+package/qemu-user-static>).
- **Not verified** (carry as open questions, do not state as fact): the exact
  native gcc patch version inside the riscv64 noble image (13.2 vs 13.3 —
  inferred from gcc-defaults, not executed); whether repo-pinned Keystone
  0.9.2 / Capstone 5.0.1 / Unicorn build or ship for an rv64 *host* (guest
  support ≠ host support; see Constraints).

## Out of scope

- **Tracing tiers on rv64** — the single-step/ptrace tier, DynamoRIO tier
  (upstream DR has no riscv64 release), hardware trace, and dataflow tiers
  stay x86-64(/AArch64) as documented in their own docs; nothing here touches
  [src/ss_backend.c](../../../src/ss_backend.c) /
  [src/ptrace_backend.c](../../../src/ptrace_backend.c). Related sibling
  work: [aarch64-ptrace-single-step-validation.md](aarch64-ptrace-single-step-validation.md),
  [inproc-btf-block-step.md](inproc-btf-block-step.md).
- **Language bindings on an rv64 host** (per-language toolchains under
  emulation, conformance corpus): follow-on once the C tier is green; the
  manifest generator is rv64-aware after T1, which is all this doc ships.
- **The emulator/assembler tier on an rv64 host** (Unicorn/Keystone host
  builds): see Constraints — verify-and-extend later, not here. RISC-V as an
  emulator *guest* is untouched and keeps working from x86/arm hosts.
- **An RVV (vector) capture arm** — needs the psABI vector calling convention
  and V-extension QEMU/toolchain plumbing; the `asmtest_cpu_has_vec128()`
  probe added in T4 is the designed hook, and
  [aarch64-sve-capture.md](aarch64-sve-capture.md) is the sibling precedent
  for a variable-length vector capture model.
- **NASM backend** — x86-only by definition
  ([src/capture.asm](../../../src/capture.asm) unchanged).
- **Native benchmarking comparisons** — `make bench` runs on rv64 after T1
  (rdtime ticks) but cross-system benchmark analysis belongs to
  [benchmarks-ci-followups.md](benchmarks-ci-followups.md).
- **Self-hosted RISC-V CI hardware** — if real rv64 boards ever join CI,
  that's [self-hosted-ci-runners.md](self-hosted-ci-runners.md) territory.
