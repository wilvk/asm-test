# asm-test — Expansion Plan

A roadmap for expanding `asm-test` beyond the Phase 0–11 feature set documented in
[DESIGN.md](../../DESIGN.md). Phases 0–11 widened *what can be tested* and *how the
runner behaves*; this plan turns the polished single-repo project into a tool others
can **adopt**, **trust**, and **extend** — and brings the emulator tier up to the
ergonomics of the native tier.

Workstreams are ordered by value-to-effort. Each is independent; they can land in any
order, though Track A is recommended first (it guards every later change).

> Status legend: **planned** unless noted. Update this file as tracks land, the way
> DESIGN.md tracks its phases.

---

## Context: current state (as of the Phase 11 commit)

- ~5,000 lines of C + assembly, complete through Phase 11.
- Full System V ABI call model (int/FP/vector/struct/sret) on x86-64 + AArch64,
  Linux + macOS; GAS + NASM backends.
- Register/flag capture, ABI-preservation checks, guard pages, crash→failure, fork
  isolation, per-test timeout, differential/property testing, benchmark mode,
  TAP + JUnit output, a real CLI.
- Optional Unicorn emulator tier: x86-64, AArch64, RISC-V, ARM32 guests; Win64 ABI;
  instruction trace + basic-block coverage.
- 4-way CI matrix ({x86-64, AArch64} × {Linux, macOS}) plus NASM and emulator jobs.

### Gaps this plan closes

| # | Gap | Symptom |
|---|---|---|
| A | Framework has no self-tests | `tests/` promised in DESIGN.md §3 doesn't exist; `make demo-fail` is a manual demo, not an asserted check. A testing tool can't prove its own assertions fail when they should. |
| B | No way to *consume* the framework | No `make install`, `libasmtest.a`, single-header, pkg-config, versioning, or releases. Adoption means copying sources. |
| C | Emulator tier is second-class | Integer args only; no macro/assertion layer; coverage collected but never reported. |
| D | Thin quality infra | No sanitizer/static-analysis/coverage CI for the framework's own C; emulator CI is x86-64-only. |
| E | Breadth odds and ends | Native-side calling conventions, parallelism, libc-callback routines, Valgrind story. |

---

## Track A — Framework self-test suite *(recommended first) — done*

**Goal.** Make the framework prove its own behavior, closing the documented but missing
`tests/` directory. This is the highest-leverage work: it is a correctness guard for
every other track.

**Status: done.** `tests/positive.c` (success paths), `tests/negative.c` (failure /
crash / timeout / abort paths), and the `tests/expect.sh` black-box harness land the
suite; `make check` runs it (wired into the `test` and `nasm` CI jobs). 32 checks pass
on x86-64; the AArch64 `#if` branch is covered by the ARM CI runners. Verified to have
teeth: neutering a single `asmtest_fail` flips a check to `not ok`. The sub-sections
below describe what was built.

**Why first.** A unit-testing framework whose own assertions, exit codes, and
crash-handling aren't tested is the one tool that most needs meta-tests. Today the only
check is `make demo-fail`, whose failures are observed by a human, not asserted.

### Deliverables

1. **`tests/` directory** (the one DESIGN.md §3 already promises), separate from
   `examples/` (which stay as user-facing samples).
2. **Expect-fail harness** — a small driver (shell or C) that builds and runs a suite
   binary expected to fail, and asserts on:
   - exit code (nonzero on failure, zero on all-pass, `2` on bad CLI args),
   - that each intended failure appears in output with the right `file:line` and
     message substring,
   - that a passing run reports the right pass/fail/skip counts.
3. **Positive meta-tests** — assert each `ASSERT_*` *succeeds* on inputs it should
   (already covered indirectly by examples; consolidate).
4. **Negative meta-tests** — assert each `ASSERT_*` *fails* on inputs it should
   (`ASSERT_EQ(1,2)` must fail; `ASSERT_FLAG_SET` on a clear flag must fail; an
   unsigned compare must order large values correctly; `ASSERT_FP_NEAR` honors ULPs).
5. **Runner-behavior tests** — `--filter`, `--list`, `--shuffle --seed` determinism,
   `--format=junit` well-formedness (pipe through a validator), `--timeout` turns a
   hang into a reported timeout, fork isolation turns a `SIGABRT` into a contained
   failure while later tests still run.
6. **`make check`** target wiring it all into CI, added as a CI step on every matrix
   entry.

### Acceptance criteria

- `make check` is green on all four CI targets and red if any framework behavior
  regresses.
- Removing a single `asmtest_fail` call (a deliberate mutation) turns `make check`
  red — i.e. the negative tests have teeth.

### Notes / risk

- The expect-fail harness must parse output stably; pin on substrings and exit codes,
  not exact formatting, so cosmetic TAP changes don't break it.
- Keep it dependency-free (POSIX shell + the existing C runner) to preserve the
  "just `make` and a C compiler" requirement.

**Effort:** ~1–2 days. **Touches:** new `tests/`, `Makefile`, `.github/workflows/ci.yml`.

---

## Track B — Packaging & distribution *(done)*

**Goal.** Make `asm-test` consumable by another project without copying sources.

**Why.** A framework's value is reuse; today the friction is high. This is the change
most likely to grow real-world use.

**Status: done.** `make lib` builds `libasmtest.a`; `make install`/`uninstall` honor
`PREFIX`/`DESTDIR` and install headers under `include/asmtest/`, the lib, and a
generated `asmtest.pc` (from `asmtest.pc.in`); `make amalgamate` produces
`asmtest_single.h` via `scripts/amalgamate.sh`; `ASMTEST_VERSION`/`_NUM` macros land
in the public header; `CHANGELOG.md` and a README "Using asm-test in your project"
section document both consumption modes. Verified end to end: an external suite
(including an asm routine + `ASM_CALL2` capture) builds and passes using only
`pkg-config --cflags/--libs asmtest`, and both amalgamation TUs (API-only and
`ASMTEST_IMPLEMENTATION`) compile clean under `-Wall -Wextra`. Remaining: tag the
`v1.0.0` release.

### Deliverables

1. **`libasmtest.a`** build target (framework runtime + capture trampoline), plus the
   public headers, with a `make install` honoring `PREFIX`/`DESTDIR`.
2. **Single-header amalgamation** (`asmtest_single.h`) generated from the existing
   header + sources for drop-in use, behind a `make amalgamate` target. (The asm
   trampoline still needs assembling; document the two consumption modes — link the
   static lib, or vendor `capture.s` + the header.)
3. **pkg-config file** (`asmtest.pc`) so consumers can `pkg-config --cflags --libs
   asmtest`.
4. **Versioning**: a `VERSION` constant in the public header, a `CHANGELOG.md`, and a
   first tagged release (`v1.0.0`) once Track A is green.
5. **README "Using asm-test in your project"** section covering both modes.

### Acceptance criteria

- A throwaway external project can build a passing suite against the installed lib +
  headers using only `pkg-config` flags.
- The amalgamated header compiles clean under `-Wall -Wextra`.

### Notes / risk

- The asm trampoline (`capture.s`) cannot be amalgamated into a C header; be explicit
  that the static-lib path is the primary one and the single-header is a convenience
  for the C-only surface.
- Decide install layout (`include/asmtest/…`?) before tagging, since it's part of the
  public contract.

**Effort:** ~1–2 days. **Touches:** `Makefile`, new `asmtest.pc`, `CHANGELOG.md`,
`README.md`, a generator script for the amalgamation.

---

## Track C — Emulator tier parity & coverage reporting *(done)*

**Goal.** Raise the Unicorn emulator tier to the native tier's ergonomics, and make the
coverage data it already collects *useful*. This plays to the project's unique
strength: cross-arch, mid-routine introspection no ABI-boundary tool can match.

**Status: done.** C1: the x86-64 guest now marshals double args (`emu_call_fp`) and
128-bit vector args (`emu_call_vec`) into xmm0..7 and captures the whole XMM file
(`emu_x86_regs_t.xmm[16]`, via `emu_vec128_t`). C2: an assertion layer in
`asmtest_emu.h` — `ASSERT_NO_FAULT` / `ASSERT_FAULT` / `ASSERT_FAULT_AT`,
`ASSERT_EMU_REG_EQ`, `ASSERT_EMU_FP_EQ`, `ASSERT_EMU_VEC_EQ`, and coverage
`ASSERT_BLOCK_COVERED` / `ASSERT_BLOCKS_AT_LEAST`. C3: `emu_trace_report`,
`emu_coverage_uncovered` (prints the blocks a run missed against a universe trace),
and an offset-level `emu_trace_lcov` export, plus the `emu_trace_covered` predicate.
Five new `emu` tests cover FP, SIMD, the macros, the uncovered-block report, and lcov.
Fixed a latent bug surfaced by reusing an emulator handle across routines: Unicorn's
translation-block cache wasn't flushed after writing new code, so a second routine
re-ran the first's stale translation — now `load_code` flushes on every load, for all
guests. **Scope note:** FP/vector marshalling landed for the x86-64 guest (the primary
engine, which also carries Win64); the same for the AArch64/RISC-V/ARM32 guests
(NEON `v0..31`, etc.) is a natural follow-up. Source-line mapping (the stretch goal)
was left out; reporting is offset-level as planned. Covered by `make emu-test`.

### C1 — Wider argument marshalling

- `emu_call` currently takes integer args only (`const long *args`). Add FP/vector
  argument marshalling (and, where it makes sense, struct-by-value) for each guest, so
  a SIMD or FP routine can be single-stepped and inspected — matching what the native
  tier already does at the ABI boundary.

### C2 — Macro & assertion layer

- The native tier has `ASM_CALLn` + `ASSERT_*`; the emulator makes callers hand-roll
  `emu_open`/`emu_call`/`emu_close` and read raw structs. Add convenience macros and
  assertions: `ASSERT_EMU_REG_EQ(&res, rax, …)`, `ASSERT_NO_FAULT(&res)`,
  `ASSERT_FAULT_AT(&res, kind, addr)`, and coverage assertions
  (`ASSERT_BLOCK_COVERED`, `ASSERT_ALL_BLOCKS_COVERED` over an accumulated trace).

### C3 — Coverage reporting

- Coverage is collected (`emu_trace_t`) but never *reported* — only raw byte offsets.
  Add output: a text summary (blocks hit / total, list of uncovered block offsets) and
  an lcov-style export so coverage can feed standard tooling. Optionally map offsets
  back to source lines when debug info is available.

### Acceptance criteria

- An FP/SIMD routine can be run in the emulator and its result asserted with the new
  macros, no manual struct reads.
- A coverage report prints uncovered block offsets for the `classify` example and the
  accumulated-union assertion is expressible as a single macro.

### Notes / risk

- Keep the emulator strictly optional (still gated behind `-lunicorn` and `make
  emu-test`); the core framework must not gain a hard dependency.
- Source-line mapping is the riskiest sub-item (debug-info parsing); scope it as a
  stretch goal behind offset-level reporting, which is self-contained.

**Effort:** ~3–5 days (C1+C2 ~2–3, C3 ~1–2; line mapping extra). **Touches:**
`src/emu.c`, `include/asmtest_emu.h`, `examples/test_emu.c`, `Makefile`.

---

## Track D — Quality infrastructure *(done)*

**Goal.** Harden the framework's own C, since its job is rigor.

**Status: done.** Makefile knobs `SAN=1` (ASan + UBSan) and `COV=1` (gcov) flow
through `CFLAGS`; convenience targets `make sanitize`, `make coverage`, and `make
tidy` (clang-tidy, curated in `.clang-tidy`) drive them. CI gains four things: a
`sanitize` job (full ASan + UBSan + LeakSanitizer on Linux), an `analyze` job
(clang-tidy, informational baseline — not gating yet), a `coverage` job that uploads
`asmtest.c.gcov` as an artifact, and the `emu` job extended to a `{x86-64, arm64}`
matrix. Verified locally on x86-64 macOS: `make sanitize` is clean (no ASan/UBSan
reports, 32/32 self-tests, zero failures across suites), `make coverage` reports
69% of `src/asmtest.c`, and `clang --analyze` (the engine behind the `clang-analyzer-*`
checks) finds nothing. LeakSanitizer and clang-tidy run in CI on Linux, where those
tools are available.

### Deliverables

1. **Sanitizer CI job** — build + run the suites (and Track A's `make check`) under
   ASan + UBSan on the framework's own C code.
2. **Static analysis** — a `clang-tidy` (or `scan-build`/`cppcheck`) CI job over
   `src/`, with a curated check set.
3. **Coverage of the C runner** — gcov/llvm-cov over `src/asmtest.c`, surfaced as a CI
   artifact (not a hard gate initially).
4. **Emulator CI on arm64** — extend the `emu` job to `ubuntu-24.04-arm` (the guests
   are host-independent, so this verifies the wrapper on a second host arch).

### Acceptance criteria

- Sanitizer job is green (or its findings are triaged and fixed).
- Static-analysis job runs on every PR with an agreed baseline.

**Effort:** ~1 day. **Touches:** `.github/workflows/ci.yml`, `Makefile` (sanitizer
flags), possibly a `.clang-tidy`.

---

## Track E — Breadth (opportunistic)

Lower priority; pick up individually as interest dictates.

- **Native Win64 trampoline** — Win64 is emulator-only today; a native trampoline would
  need a Windows host/CI runner. Scope only if Windows support becomes a goal.
- **Parallel execution** — each test already forks; a `-jN` to run N children
  concurrently is a natural extension of the existing fork model.
- **libc-callback routines** — helpers/examples for routines that call back into C or
  take pointers to complex data, with marshalling helpers.
- **Valgrind / sanitizer story for the routine under test** — document and example how
  to run a routine under Valgrind or with the guard-page allocator to catch its bugs.
- **AArch64 alternative assembler** — NASM is x86-64-only by nature; either document
  GAS as the sole AArch64 path (current reality) or evaluate another assembler.

---

## Suggested sequencing

1. **Track A** (self-tests) — guards everything that follows.
2. **Track B** (packaging) + **v1.0.0** tag — turns the project into something
   adoptable, now that A backs the release.
3. **Track D** (quality infra) — cheap, compounds with A.
4. **Track C** (emulator parity) — the deepest engineering; do it once the foundation
   is locked.
5. **Track E** — opportunistic, any time.

## Out of scope (for now)

- New guest architectures beyond the existing four (diminishing returns vs. effort).
- A GUI/TUI front-end (TAP + JUnit already integrate with standard tooling).
- Rewriting in another language; the C + asm core is the point.
