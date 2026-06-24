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

**Follow-up landed:** FP/vector marshalling now covers the other guests too —
`emu_arm64_call_fp` / `emu_arm64_call_vec` (full NEON `v0..31`, captured into
`emu_arm64_regs_t.v[]`), `emu_riscv_call_fp` (D-extension `f0..31`, FP unit enabled
at open via `mstatus.FS`), and `emu_arm_call_fp` (VFP `d0..31`/`q0..15`, enabled at
open via CPACR + FPEXC). A generic `ASSERT_EMU_VEC128_EQ` and field-direct fault
macros work across every guest's result struct. Four more `emu` tests (now 37
passing).

**Leftovers landed (Track C closed).** The two remaining open C items are now
resolved:

- **ARM NEON vector-arg marshalling** — `emu_arm_call_vec` marshals 128-bit
  vectors into `q0..q3` (AAPCS-VFP) and captures the whole `q0..q15` file,
  bringing ARM32 to the x86-64/AArch64 vector parity. (`emu_arm.vector_arg_
  captures_q_file`.)
- **Source-line coverage mapping** — `emu_line_map_t` + `emu_line_lookup`,
  `emu_trace_source_report`, and `emu_trace_lcov_source` translate block
  byte-offsets into source-line coverage (hit **and** missed lines) via a
  caller-supplied, ascending `(offset, line)` map. The map is produced
  out-of-band (objdump/readelf/listing/DWARF), so the framework keeps its
  no-extra-dependency, no-DWARF-parsing stance — the offset-level `emu_trace_
  lcov` remains the zero-config baseline. (`emu.source_line_mapping`.)
- **RISC-V "V" (vector) extension** — *not feasible, documented decision.*
  Unicorn's RISC-V guest exposes no vector registers (`UC_RISCV_REG_V0..` do not
  exist) and no `vtype`/`vl` CSRs, so there is no register interface to marshal
  vector args into or capture them from; RISC-V stays scalar-FP
  (`emu_riscv_call_fp`). Recorded in `src/emu.c` and `docs/emulator.md`. Would
  become feasible only if a future Unicorn exposes the RVV register file.

Two more `emu` tests (now **39** passing). Track C has no open C items.

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

Lower priority; pick up individually as interest dictates. All five items have a
landed slice; the native Win64 **capture** tier shipped (the Win32 runner port is
the only deferred remainder).

- **Native Win64 trampoline** — *capture tier landed; runner port deferred.* The
  Microsoft x64 capture trampoline (all eight `asm_call_capture*` variants) runs on
  real x86-64 silicon with **no Windows host** — natively via `__attribute__((ms_abi))`
  and as a real PE under **Docker + Wine** — with a first-class `regs_t` layout,
  manifest, and a CI `win64` job. The POSIX runner is *not* ported to Win32 (the
  suite runs `--no-fork`); that remains scoped for on-demand pickup. See the
  [Native Win64 tier implementation plan](win64-native-tier-plan.md) for the full
  build, [docs/win64.md](../win64.md) for usage, and [Track E.1 — Native Win64
  trampoline (scoping)](#track-e1--native-win64-trampoline-scoping) below for what
  the remaining runner port takes.
- **Parallel execution** — *done.* `-jN` / `--jobs=N` runs up to N tests
  concurrently as a pool of forked children over the existing per-test fork model
  (`run_parallel` in `src/asmtest.c`, using `poll()` over the children's result
  pipes). Output is buffered and rendered in registration order, so TAP/JUnit stays
  byte-identical to a serial run regardless of finish order; per-test timeout and
  crash containment are unchanged, and `--no-fork` forces serial. Verified ~9× on
  8 sleepy tests at `-j8`, and four new `tests/expect.sh` checks pin ordering,
  failure reporting, crash containment, and bad-`--jobs` rejection.
- **libc-callback routines** — *done.* `examples/callback.s` / `.asm` +
  `examples/test_callback.c`: `sum_map(arr, n, fn)` and `count_if(arr, n, pred)`
  invoke a C function pointer per element, demonstrating an assembly routine that
  calls back into C with correct callee-saved/stack-alignment discipline (the
  bug class the example is built to catch). Builds under both GAS and NASM.
- **Valgrind / sanitizer story for the routine under test** — *done.* `make
  valgrind` runs the example suites under memcheck (`--no-fork`, `--error-exitcode`)
  to catch bugs in the routine under test, complementing the always-on guard-page
  allocator (`asmtest_guarded_alloc` / `_under`); `make docker-valgrind`, the
  `--valgrind` installer flag, and a README "Debugging a routine under test"
  section round it out. Distinct from Track D's `make sanitize`, which instruments
  the framework's own C.
- **AArch64 alternative assembler** — *done (documented decision).* GAS is the sole
  AArch64 backend — the `.s` sources assemble through the C compiler's built-in
  assembler, so no extra tool is needed; NASM stays x86-64-only by design. Recorded
  in the README "Two assembler backends" bullet.

---

## Track E.1 — Native Win64 trampoline (scoping)

> For the concrete phased *how* (substrate, trampoline, layout, runner port, CI),
> see the [Native Win64 tier implementation plan](win64-native-tier-plan.md). This
> section is the *scoping* it builds on.

The one Track E item left undone. This section records *what it takes* so the
decision to pick it up is informed, not so it is recommended — the short version
is that **the trampoline is the easy part; the hard part is that the whole runner
is POSIX**, and a native Win64 tier means porting that runner to Win32. The
toolchain and CI need not run *on Windows*, though: **Docker + Wine** (Option 2
below) executes real Win64 PEs on the project's existing Linux CI, so the runner
port can be developed and tested without ever provisioning a Windows host.

**Context — Win64 already works in the emulator.** `emu_call_win64` /
`emu_call_win64_traced` (`src/emu.c`) run a routine's bytes under the Microsoft
x64 ABI on a *System V host*: integer args in `rcx, rdx, r8, r9`, a 32-byte
shadow space, stack args at `[rsp+40+8*i]`, and `rsi`/`rdi` treated as
nonvolatile. So the ABI knowledge is already encoded and tested; a native tier
buys running the real routine on real silicon under the real OS (relevant for
SEH, TLS, and Windows syscalls) at the cost of a second platform port.

### What's required

**1. A Win64 toolchain and a host to run it *(Option 1: a real Windows runner)*.**
The project is POSIX-only across a 4-way `{x86-64, AArch64} × {Linux, macOS}`
matrix. The most faithful host is a `windows-latest` runner with a Windows
assembler: **MSVC** (`cl` + `ml64`/MASM) or **MinGW-w64** (gcc + GAS). NASM
already supports `-f win64`, so the existing NASM `.asm` path is the cheapest
assembler route. This is no longer a hard *gating* dependency, though — the same
assemblers run as Linux cross-tools under **Docker + Wine** (Option 2 below),
which removes the Windows host itself and keeps Win64 on the existing Linux CI.

**2. A Win64 capture trampoline.** Mirror the ~8 `asm_call_capture*` variants in
`src/capture.s` for the Microsoft x64 ABI. Deltas from the System V version
(all already modeled in `emu.c`):

| | System V (current) | Win64 (needed) |
|---|---|---|
| Integer args | rdi, rsi, rdx, rcx, r8, r9 | **rcx, rdx, r8, r9** (4), rest on stack |
| FP args | xmm0–7 | **xmm0–3**, *positional* with the int regs (slot _i_ is int **or** xmm, never both) |
| Shadow space | none | **32 bytes** reserved below the return address at every call site |
| Callee-saved int | rbx, rbp, r12–r15 | adds **rdi, rsi** (args on SysV, preserved here) |
| Callee-saved FP | none (all xmm volatile) | **xmm6–xmm15** must be saved/restored |
| Varargs | `al` = # vector regs | FP args **duplicated** into the matching int reg |

So the trampoline saves/seeds/verifies a *larger* register set than the SysV one.
This part is well understood and moderate effort.

**3. A Win64 `regs_t` + ABI-preservation extension.** `include/asmtest.h` has
`#if __x86_64__` / `#elif __aarch64__` branches whose field offsets must match
the trampoline's stores. Win64 needs a **third branch** (same arch, different
ABI) adding `rdi`/`rsi` and `xmm6–15` to the captured/checked set, new sentinels,
and matching `asmtest_assert_abi` logic.

**4. Porting the runner to Win32 *(the real cost)*.** `src/asmtest.c` is built on
POSIX primitives, several of which are *core features*, not conveniences:

- `fork()` / `pipe()` / `waitpid()` → per-test **crash isolation** and the `-jN`
  pool (`run_parallel`, `poll()`),
- `sigaction` + `siglongjmp` → **crash-to-failure** (SIGSEGV/SIGBUS/SIGABRT),
- `alarm()` → **per-test timeout**,
- `mmap`/`PROT_NONE` → the **guard-page allocator**,
- `fnmatch` → `--filter`.

On native Windows (MSVC) none of these exist. Either accept **MinGW/MSYS2** (a
POSIX-ish layer where `fork` is emulated and unreliable and signals are limited —
degrading the framework's headline isolation feature), or port to Win32 proper:
`CreateProcess` for isolation, a **vectored exception handler / SEH** for
crash-to-failure, `CreateTimerQueueTimer` or a watchdog thread for timeouts,
`VirtualAlloc` + `PAGE_NOACCESS` for guard pages, and `WaitForMultipleObjects`
for the parallel pool. This port is the bulk of the work.

### Option 2 — Docker + Wine (no Windows host)

A `windows-latest` runner is not the only way to *execute* real Win64 code.
**Wine runs a real Win64 PE on Linux** — real PE loader, real Win32 personality —
so the entire native tier (the trampoline **and** the §4 Win32 runner port) can be
built, run, and CI'd on the project's existing Linux Docker infrastructure with no
Windows machine in the loop. Wine doesn't remove the runner port (the runner still
has to *become* Win32); it removes the Windows **host**.

**How it maps onto the existing Docker setup.** The bindings already test each
toolchain in its own image — `Dockerfile.bindings-base` → `bindings/<lang>/
Dockerfile` → `make docker-<lang>` (build the base once, cached, then a small
image on top). Win64 gets the same treatment: a `Dockerfile.win64` on the same
`ubuntu:24.04` base installing

- **`mingw-w64`** (`x86_64-w64-mingw32-gcc`, GAS) and/or **`nasm`** (`-f win64`) —
  the assemblers/linkers from requirement #1, as Linux *cross*-tools;
- **`wine64`** (WineHQ stable; modern WoW64) — the execution environment;

plus a `make docker-win64` target that cross-compiles the framework + a Win64
suite to a PE and runs it under `wine64`, mirroring `make docker-<lang>` (and
joining `make docker-bindings` / the `bindings` CI matrix).

**What it buys, between the two endpoints.**

| | `ms_abi` native (lightest) | **Docker + Wine** | `windows-latest` (heaviest) |
|---|---|---|---|
| Host needed | none (Linux/macOS x86-64) | none (Linux Docker) | a Windows runner |
| PE loader / Win32 APIs | ✗ (ABI only) | ✓ (Wine implements them) | ✓ (real OS) |
| §4 runner port exercised | ✗ | ✓ (CreateProcess, SEH, VirtualAlloc, timers all work under Wine) | ✓ |
| OS fidelity | ABI only | high, not 100% | authoritative |

So the full §4 port — `CreateProcess` isolation, a vectored-exception/SEH
crash-to-failure path, `CreateTimerQueueTimer` timeouts, `VirtualAlloc` +
`PAGE_NOACCESS` guard pages, `WaitForMultipleObjects` for the pool — can be
developed and regression-tested entirely under Wine in Docker, because Wine
implements all of them. (The MinGW emulated-`fork` shortcut stays just as
unreliable under Wine as on Windows, so the proper Win32 port is still the path to
real isolation.)

**Caveats.** Wine ≠ Windows at the edges (some SEH corner cases and syscalls
differ), so green-under-Wine is strong but not final — keep one occasional
`windows-latest` job for authoritative sign-off, or accept Wine's fidelity and
drop it. Needs 64-bit Wine, and it is x86-64 only: on an AArch64 host, Win64-x64
stays emulator-only (Unicorn), exactly as today.

> Even lighter, if only the **ABI** needs validating (not the PE loader or OS):
> compile the call site with GCC/Clang `__attribute__((ms_abi))` and capture the
> Win64 register/flag state natively on the existing x86-64 Linux/macOS rows — no
> Wine, no PE, no Windows. Reach for Wine when you also want the real PE loader and
> the Win32 runner port under test.

### Effort

- Trampoline + `regs_t` + header branch: **~2–3 days** (ABI is known/encoded).
- Win32 runner port (isolation, signals→SEH, timeout, guard pages, parallel):
  **~1–2 weeks** done properly; the MinGW shortcut is cheaper but compromises
  isolation.
- CI + toolchain wiring: **~1 day** for a `windows-latest` runner, or a comparable
  day for a `Dockerfile.win64` + `make docker-win64` (Option 2) on the existing
  Linux CI — no Windows host to provision.

### Recommended first slice (if pursued)

Lowest-risk wedge that validates the trampoline without the full runner port:
a **NASM `-f win64` trampoline + a single suite run with `--no-fork`**. Run it
the cheapest way first — cross-assembled and executed under `wine64` in a `make
docker-win64` image (Option 2), needing no Windows runner — and add a
`windows-latest` job later only if real-OS sign-off is wanted. Either exercises
the Win64 ABI and capture path on real x86-64 hardware; whether the Win32
isolation port is worth doing can be decided from there.

### Acceptance criteria (full tier)

- A Win64 routine builds and is callable via `ASM_CALL*` on a Windows runner,
  with `ASSERT_ABI_PRESERVED` covering the Win64 callee-saved set (incl.
  `rdi`/`rsi`, `xmm6–15`).
- `make test` and `make check` are green on `windows-latest` with isolation
  (fork-equivalent), timeout, and crash containment all working.

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
- Rewriting in another language; the C + asm core is the point. (Note: *wrapping*
  the C core with bindings for other languages — distinct from rewriting it — is a
  separate, planned effort; see the
  [Multi-language bindings plan](multi-language-bindings-plan.md) and its
  [feasibility analysis](../analysis/multi-language-wrappers.md).)
