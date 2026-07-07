# asm-test — repository review (2026-07-04)

**Scope:** whole-repo improvement/expansion review across nine dimensions —
core API, runner & reporting, tracing tiers, emulator & tooling, language
bindings, build/CI, documentation, the framework's own self-tests, and
strategic product direction. Unlike the [2026-07-01](2026-07-01-repo-review.md)
and [2026-07-02](2026-07-02-repo-review.md) reviews (which tracked crashes and
release/packaging process), this pass looks for **small correctness defects on
real user paths** plus **high-leverage DX and expansion opportunities** that are
*not* already tracked.

**Method:** nine parallel dimension reviewers each read their subsystem's source
and docs and proposed candidate improvements with `file:line` evidence; every
candidate was then run past an adversarial verifier checking three things —
already implemented? already tracked in a plan/review/summary/DESIGN? real
value? — with the tracked P0–P5 set from
[open-defects-and-review-items-plan.md](../plans/open-defects-and-review-items-plan.md)
and the hardware/privilege-blocked set from
2026-07-02-roadmap-assessment.md
supplied as exclusions. ~30 recommendations survived.

**Caveat — partial verification:** the verifier agents for the **bindings** and
**self-test** dimensions were interrupted by an upstream spend limit and did not
run. Their findings were retained as **finder-only** *except* the three
highest-impact bindings items (N1, N2, N7), which were re-checked against the
code by hand for this review and are marked **[verified]**. Items marked
**[verified]** were confirmed against the source; **[finder-only]** items have
solid `file:line` evidence but no independent second pass — treat them as leads.

Paths are repo-relative; `file:line` points at the exact site. Nothing in this
review is blocked on hardware, privileges, or a credentialed action.

---

## Remediation status (updated 2026-07-04)

All six steps of the suggested fix order have been worked through, and every one
of the ~50 findings now has an explicit disposition in the `Status` column below.

**At a glance — 50 findings:**

| Disposition | Count | Items |
|---|---|---|
| ✅ Fixed & verified | 35 | A1 A2 D1 N1 N2 · A3 A4 R1 R2 R3 T1 E1 D3 A5 N7 E2 · K5 K2 K3 K4 · E3 D2 R4 R6 K6 K7 E4 E6 D4 D5 R7 · X1 X2 X3 X4 |
| 🟡 Fixed, CI-run validation pending | 1 | K1 (cache config landed 2026-07-07; the gha cache only exercises on a real Actions run) |
| ⏸ Confirmed, deferred | 1 | N4 (multi-binding expansion) |
| ⚪ Not actionable as written | 3 | N3 (scaffolding by design), N5 (docs defer it), N6 (heterogeneous by design) |
| ⬜ Open expansions (step 5) | 10 | P1 E7 A6 A7 R5 E5 P2 P3 P4 P5 |

Landed across: **59adb74** (step 1), **817cc72** (step 2), **356b0ff** + **2b8e8f9**
(step 3), **8fb7c14** (step 4), **a8f7054** (step 6). Each "fixed" row was
independently re-verified against the committed code (40/40 confirmed, 0
discrepancies) — not just self-reported.

Per-step detail:

- **Step 1 — the "green CI, broken artifact" cluster (D1, N1, N2, A1, A2)** —
  fixed in **59adb74**, each with a regression gate (a `make
  check-header-portability` target compiling a strict-c11 and a C++ consumer plus
  a docs lint; a go consumer-link step and a JDK-25/no-preview java smoke in
  `release.yml`). Verified in the cpp/go/java containers.
- **Step 2 — the cheap correctness wins (A3, A4, A5, R1, R2, R3, T1, E1, N7, D3,
  E2)** — fixed in the follow-up commit. Verified: `make test`/`check` green,
  TAP/YAML spec-conformance and the crashing-BENCH exit and empty-selection paths
  exercised by hand, and the cpp (20/20), emu (47/47), and go conformance
  containers all pass.
- **Step 3 — CI leverage (K3, K4, K5, K2)** — fixed. K5 makes the PIC and
  native-trace object trees rebuild on a build-knob flip; K4 sanitizes the
  emulator tier (emu-test 47/47 clean under ASan+UBSan; the single-step / ptrace /
  codeimage tiers are excluded because sanitizer instrumentation perturbs the
  exact instruction stream / emission addresses they measure); K3 widens
  clang-tidy and gcov across the C tree; K2 wires the cross-language hardware-trace
  wrappers and the eBPF codeimage detector into CI. Verified in the CI container
  (docker-analyze, docker-coverage, docker-sanitize, docker-hwtrace-codeimage
  19/19, docker-hwtrace-go all green). **K1** (cache the Keystone/Capstone builds)
  was **deferred** here — a CI-throughput optimization whose mechanisms (docker
  buildx `type=gha` cache; host `actions/cache` of a compiled toolchain) are
  GitHub-Actions-runtime-specific and can't be validated locally — and then
  **landed 2026-07-07 in its own commit**: ci.yml only (release.yml stays
  cache-free on purpose), non-fatal on any cache miss/outage; a CI run must
  still confirm the warm-cache path.
- **Step 4 — DX & docs (R4, R6, R7, K6, K7, D2, D4, D5, E3, E4, E6)** — fixed.
  Runner: complete JUnit (`<error>`/`errors=`/`time=`/`<system-out>`), bench
  dispersion + `--bench-format=json`, and `--color`/`NO_COLOR`. Emulator: a
  register-preload API (`emu_set_reg`), a retrievable fuzz corpus (handle-owned,
  feeds `emu_mutation_test1`), and read watchpoints (`emu_watch_reads`) — each
  with a new self-test (emu tier 47 → 50, all green). Build: `make install` now
  ships the native-trace headers plus `install-shared-hwtrace`/`-drtrace`. Docs:
  fixed the releasing/arm64-CI/ci.md drift, back-filled the API reference
  (Win64 + AVX2/512), added a Troubleshooting & FAQ page, and gave the README a
  Documentation funnel into the docs site. Verified: `make test`/`check` green,
  the JUnit/bench/color paths exercised by hand, and the emulator container (50/50).
- **Step 6 — re-verify the finder-only leads (N3–N6, X1–X4)** — the review's own
  bindings/self-test verifiers were interrupted, so each was re-checked against
  the code first. **X1–X4 confirmed and fixed**: the self-test suites now assert
  TEARDOWN runs (a --no-fork lifecycle counter), fault a guard-page overrun/
  underrun and a differential-model mismatch, contain SIGILL/SIGFPE/SIGBUS (not
  just SIGSEGV/SIGABRT), and validate the JUnit escaper on a `< & > "` message
  (xmllint installed on the test lane). The re-check **downgraded three finder
  leads**: **N5** is not real (the zig README explicitly *defers* the module
  export), **N6** is impractical as proposed (corpus mirroring is heterogeneous
  by design, so a flat parity gate is all-noise — the same reason
  check-bindings-parity.sh skips the corpus surface), and **N3** is
  working-as-documented scaffolding. **N4** is confirmed but a genuine
  multi-binding expansion, deferred. Verified: `make check` green (self-tests
  36 → 43), the new cases exercised by hand + validated well-formed.

Everything else remains open (`verified` = confirmed real, not yet actioned).

---

## Overall

The core is complete and CI-exercised, and the existing open-defects plan
already tracks the big-ticket items (Go flaky crash, supply-chain pinning,
Intel-mac payload, first real tag). This review surfaces a layer below that: a
cluster of **small correctness defects that silently break real user paths** —
C++ consumers, `-std=c11` consumers, the copy-paste README example, and the
*published* Go and Java packages — alongside DX gaps versus peer test frameworks
and a set of low-risk expansions. The highest-value theme is that several
**"green CI, broken artifact"** paths exist: the published Go module can't link,
the published Java jar can't load on a modern JDK, and the README's first example
can't assemble — none of which any current gate catches.

### Severity summary

| # | Finding | Severity | Kind | Status |
|---|---------|----------|------|--------|
| A1 | `sigjmp_buf` in public header breaks `-std=c11` consumers | High | defect | ✅ fixed (59adb74) |
| A2 | Capture macros (`ASM_CALL*`) don't compile from C++ | High | defect | ✅ fixed (59adb74) |
| D1 | README "Writing a test" example doesn't assemble | High | defect | ✅ fixed (59adb74) |
| N1 | Published Go module links a test-only fixture lib → unconsumable | High | defect | ✅ fixed (59adb74) |
| N2 | Java binding compiled `--enable-preview` → jar dead on JDK 22+ | High | defect | ✅ fixed (59adb74) |
| A3 | `ASSERT_UEQ`/`ASSERT_REG_EQ` truncate to 32-bit on Win64 (LLP64) | Medium | defect | ✅ fixed (step 2) |
| A4 | `ASM_CALLN`/`ASM_SRET`/`ASM_CALL_WIN64_N` don't cast varargs | Medium | defect | ✅ fixed (step 2) |
| R1 | TAP stream not spec-conformant (version-line order, invalid YAML) | Medium | defect | ✅ fixed (step 2) |
| R2 | A crashing/hanging `BENCH` still exits 0 | Medium | defect | ✅ fixed (step 2) |
| R3 | Empty test selection exits 0 silently | Medium | improvement | ✅ fixed (step 2) |
| T1 | `asmtest_dr_available()` disagrees with `dr_lib_path()`; no DR skip-reason | Medium | defect | ✅ fixed (step 2) |
| E1 | Emulator run-off nondeterministic across reused handles | Medium | defect | ✅ fixed (step 2) |
| K5 | `.build-flags` sentinel absent from PIC/native-trace object trees | Medium | defect | ✅ fixed (step 3) |
| D3 | Stale `1.0.0` version strings (README, `conf.py`, SECURITY, …) | Medium | defect | ✅ fixed (step 2) |
| A5 | `asmtest_capture_vec_f32` exported but not declared in the header | Low | defect | ✅ fixed (step 2) |
| N7 | `vec_add8d` missing from the corpus name table | Low | defect | ✅ fixed (step 2) |
| K1 | Keystone/Capstone source builds re-compiled ~20× per push (no cache) | High | improvement | 🟡 fixed (2026-07-07 — actions/cache on the host builds + a gha-scoped buildx layer cache on the docker bindings-base; designed non-fatal on a cold/unavailable cache; CI-run validation pending) |
| K2 | `hwtrace-bindings-test` / `codeimage-test` exist but run in no CI job | High | expansion | ✅ fixed (step 3) |
| K3 | clang-tidy & gcov cover only 1 of 20 C translation units | High | improvement | ✅ fixed (step 3) |
| E3 | "preload arbitrary registers" advertised but no API exists | High | defect/expansion | ✅ fixed (step 4) |
| D2 | README doesn't funnel to the docs site; bindings/tracing invisible | High | docs | ✅ fixed (step 4) |
| P1 | C core has no system-package presence (brew/deb/AUR/vcpkg/conan) | High | expansion | verified |
| R4 | JUnit report incomplete (`<error>`, `time=`, `<system-out>`) | Medium | improvement | ✅ fixed (step 4) |
| R6 | Bench mode has no dispersion stat and no machine-readable output | Medium | improvement | ✅ fixed (step 4) |
| K4 | `make sanitize` never instruments the emu/asm/trace tiers | Medium | improvement | ✅ fixed (step 3, emu tier) |
| K6 | Native-trace headers/libs not installed → installed-prefix can't build | Medium | improvement | ✅ fixed (step 4) |
| K7 | Docs drift: `releasing.md` steps, arm64-CI claims, `ci.md` job list | Medium | docs | ✅ fixed (step 4) |
| E4 | Fuzz corpus is built then discarded — no replay/persistence | Medium | improvement | ✅ fixed (step 4) |
| E6 | Watchpoints cover writes only, not reads | Medium | expansion | ✅ fixed (step 4) |
| E7 | `ASSERT_MATCHES_REF` reports the raw first-failing input (no shrink) | Medium | expansion | verified |
| A6 | No mixed integer+FP capture macro (`ASM_MIXCALL`) | Medium | expansion | verified |
| A7 | Differential testing is integer-only (no FP reference models) | Medium | expansion | verified |
| R5 | No `--fail-fast`, `--repeat=N`, or shard selection | Medium | expansion | verified |
| E5 | No emulator snapshot/restore → order-dependent fuzz/mutation sweeps | Medium | expansion | verified |
| E2 | `emulator.md` still claims register state is retained across calls | Medium | docs | ✅ fixed (step 2) |
| D4 | API-reference index omits the post-1.0 surface (Win64, AVX2/512) | Medium | docs | ✅ fixed (step 4) |
| D5 | No troubleshooting / FAQ page for the environment-sensitive tiers | Medium | docs | ✅ fixed (step 4) |
| P2 | No consumer-facing CI integration (GitHub Action / GitLab template) | Medium | expansion | verified |
| P3 | RISC-V is emulator-guest only; no native RISC-V host tier | Medium | expansion | verified |
| P4 | No maintained "asm-test vs alternatives" comparison page | Medium | docs | verified |
| P5 | Teaching audience recognized but no classroom kit / autograder recipe | Medium | expansion | verified |
| R7 | Color is `isatty`-only; `NO_COLOR`/`--color` unsupported | Low | improvement | ✅ fixed (step 4) |
| N3 | Lua rock & Java jar not actually publishable as documented | Medium | defect | ⚪ not a defect — packaging is scaffolding by design (packaging.md); only nit is an unused `LUAROCKS` var |
| N4 | Wide-arity / struct-return / mixed-FP capture unreachable from bindings | Medium | improvement | ⏸ confirmed; deferred (genuine multi-binding expansion — C entry points exist + are FFI-friendly, just unwrapped) |
| N5 | Zig package exports no importable module | Medium | improvement | ⚪ not real — the zig README's "## Deferred" lists the module export as future work; finder missed it |
| N6 | No tripwire stops a new corpus case silently skipping 9/10 bindings | Medium | improvement | ⚪ impractical as proposed — corpus mirroring is heterogeneous by design (Python data-replays, Go names differently, tiers per-binding); a flat parity rule is all-noise |
| X1 | Nothing anywhere asserts `TEARDOWN` actually runs | Medium | improvement | ✅ fixed (step 6) |
| X2 | Guard-page & `ASSERT_MATCHES_REF` paths only run with exit ignored | High | improvement | ✅ fixed (step 6) |
| X3 | Crash containment self-tested for SIGSEGV/SIGABRT only | Medium | improvement | ✅ fixed (step 6) |
| X4 | JUnit XML escaping never validated (`xmllint` installed on no lane) | Medium | improvement | ✅ fixed (step 6) |

---

## Correctness defects (verified)

### A1. `sigjmp_buf` in the public header breaks `-std=c11` consumers — High
[include/asmtest.h:578](../../include/asmtest.h#L578) unconditionally declares
`extern sigjmp_buf asmtest_jmp;`, but `sigjmp_buf` is POSIX, not ISO C, so glibc
hides it under `-std=c11`. Any consumer compiling strict (the README pkg-config
flow adds no feature-test macros) cannot `#include "asmtest.h"` at all, and the
advertised `make CSTD=c11` ([Makefile:74](../../Makefile#L74)) fails on the first
compile. `asmtest_jmp` is referenced only inside `src/asmtest.c` (grep-verified),
so the extern can move out of the public header. *(Note: a header-only fix
unblocks consumers but not `make CSTD=c11` itself, since `src/asmtest.c` also
uses `stack_t`/`CLOCK_MONOTONIC`/`siglongjmp` without feature-test macros — that
TU needs `_POSIX_C_SOURCE`.)*

### A2. Capture macros don't compile from C++ — High
Every `ASM_CALL*`/`ASM_FCALL*`/`ASM_VCALL*` macro builds its argument array with
a C compound literal (`(long[6]){…}`). g++ rejects this with *"taking address of
temporary array"* even without `-pedantic`, so the entire ergonomic surface
fails in a C++ TU — despite `extern "C"` guards, a C++ `static_assert` path, and
the C++ binding's claim you can `#include "asmtest.h"` and use it directly
([bindings/cpp/asmtest.hpp:5](../../bindings/cpp/asmtest.hpp#L5)); the binding
hand-rolls arrays to dodge exactly this. Fix with `#ifdef __cplusplus`
inline-function overloads (or variadic-template shims) that build a local array.
Anchors: [include/asmtest.h:638-751](../../include/asmtest.h#L638). *(g++-specific;
clang++ accepts compound literals as an extension.)*

### A3. `ASSERT_UEQ`/`ASSERT_REG_EQ` truncate to 32-bit on the Win64 tier — Medium
`ASMTEST_CMP_` casts to `long` and `ASMTEST_UCMP_` to `unsigned long`
([include/asmtest.h:807-840](../../include/asmtest.h#L807)); under mingw LLP64
those are 32-bit, so `ASSERT_UEQ` — documented as the right choice for register
values that may exceed `LONG_MAX` — silently compares only the low 32 bits, and
`ASSERT_REG_EQ` (which expands to `ASSERT_UEQ`) can false-pass when two register
values differ only in the high half. `regs_t` fields are `unsigned long long`
precisely because Win64 is LLP64
([include/asmtest.h:136](../../include/asmtest.h#L136)), and
[tests/win64/suite_win64.c](../../tests/win64/suite_win64.c) already sidesteps the
macros with a local `unsigned long long` check. Switch the comparison macros
(and the `asmtest_regs_ret/_flags` FFI accessors,
[src/ffi.c:25](../../src/ffi.c#L25)) to `uint64_t`/`%llx`; no behavior change on
LP64.

### A4. Variadic capture macros don't cast their arguments — Medium
`ASM_CALL0..6` cast every argument to `(long)`, so a buffer pointer just works;
`ASM_CALLN` ([include/asmtest.h:663](../../include/asmtest.h#L663)), `ASM_SRET`,
and `ASM_CALL_WIN64_N` build `(long[]){__VA_ARGS__}` with **no** per-argument
cast, so a pointer argument — the common case at exactly the >6-argument boundary
these macros exist for — is `-Wint-conversion` on gcc 13 and a hard error on
gcc 14+/clang. [docs/guides/abi-capture.md](../../docs/guides/abi-capture.md)
recommends `ASM_CALLN` as "the general form" without noting the requirement. A
`FOREACH`-style cast-each macro (to a documented cap) removes the inconsistency.

### A5. `asmtest_capture_vec_f32` is exported but undeclared — Low
[src/ffi.c:87](../../src/ffi.c#L87) defines and exports it, and
[docs/reference/api-reference.md:181](../../docs/reference/api-reference.md#L181)
lists it beside `asmtest_capture6`/`asmtest_capture_fp2` as part of the
opaque-handle surface — but it has no prototype in `include/asmtest.h` (its peer
`asmtest_regs_vec_f32` *is* declared, so this is an oversight). One line brings
the header in line with the documented API and removes a drift risk.

### R1. The TAP stream is not spec-conformant — Medium
Two violations of the "consumable by any TAP harness" claim
([docs/guides/runner.md:90](../../docs/guides/runner.md#L90)): (1) with
`--shuffle`, `# shuffle seed=…` prints *before* `TAP version 13`
([src/asmtest.c:1907](../../src/asmtest.c#L1907)), but the version line must be
first; (2) the failure YAML emits `msg: <raw>`
([src/asmtest.c:1400](../../src/asmtest.c#L1400)) and virtually every assertion
message contains `): `, which is illegal in a YAML plain scalar — PyYAML rejects
even the single-line case. Fix: move the seed comment after the plan, and emit
the message as a block scalar (`msg: |` with indented lines).

### R2. A crashing or hanging `BENCH` still exits 0 — Medium
`run_benchmarks` prints `ERROR:` on a caught fatal signal but records nothing,
and `main` returns 0 unconditionally after bench mode
([src/asmtest.c:1690,1853](../../src/asmtest.c#L1690)) — so `make bench`, which
gating CI jobs run directly ([.github/workflows/ci.yml](../../.github/workflows/ci.yml)),
can never fail even if every benchmark SIGSEGVs. Track an error count and exit 1.

### R3. An empty test selection exits 0 silently — Medium
A typo'd `--filter` prints `1..0` and returns 0
([src/asmtest.c:1856](../../src/asmtest.c#L1856)), so a CI job that filters can
green-light while running nothing (pytest exits 5; gtest warns). At minimum warn
to stderr; better, add an opt-in `--fail-if-no-tests`. *(Keep `1..0` valid-TAP by
default — make any exit-code change opt-in.)*

### T1. `asmtest_dr_available()` disagrees with `dr_lib_path()` — Medium
`available()` returns 0 when none of `ASMTEST_DR_LIB`/bundled/`DYNAMORIO_HOME`
resolve, but `dr_lib_path()` falls through to `dlopen("libdynamorio.so")`
([src/drtrace_app.c:144-161](../../src/drtrace_app.c#L144)) — so a
system-installed `libdynamorio` reachable via ldconfig/`LD_LIBRARY_PATH`
self-skips every DR-gated test and the `trace_auto` cascade even though init
would succeed, contradicting the "kept in lock-step" comment. Separately, DR is
the only trace tier with no `*_skip_reason` counterpart
([include/asmtest_drtrace.h](../../include/asmtest_drtrace.h)), so a `0` gives no
diagnostic. *(The ldconfig case is uncommon; the comment/code contradiction and
skip-reason gap are the real issues.)*

### E1. Emulator run-off is nondeterministic across reused handles — Medium
`load_code` writes `code_len` bytes into the 64 KiB code region and never clears
the rest ([src/emu.c:167](../../src/emu.c#L167)), so on a reused handle a routine
that runs past its end (bad branch, truncated `code_len`, or the hardcoded 64-byte
FFI windows) executes the *previous* routine's leftover bytes — first-call and
reused-handle behavior differ, contradicting the tier's own `zero_regs`
determinism rationale ([src/emu.c:248](../../src/emu.c#L248)). Trap-fill (x86 `0xCC`
/ arch-appropriate) or zero-fill the window tail after `uc_mem_write`.

### K5. The build-flags sentinel is absent from the PIC & native-trace objects — Medium
`.build-flags` ([Makefile:217](../../Makefile#L217)) makes
`make test && make SAN=1 test` rebuild — but no `$(BUILD)/pic/%.o`
([Makefile:365-404](../../Makefile#L365)) nor any hwtrace-tier object
([mk/native-trace.mk:185](../../mk/native-trace.mk#L185)) depends on it, so
`make shared-emu && make SAN=1 shared-emu` silently reuses uninstrumented objects
and doesn't relink the `.so`. `native-trace.mk` already fixed this for
`drtrace_app.o` only. Lands harder if K4 is done. *(Partial for non-PIC hwtrace
binaries whose core objects do rebuild; exact for the shared-lib/bindings path.)*

### D1. The README's first authoring example doesn't assemble — High
[README.md:125](../../README.md#L125) uses `ASM_FUNC(add_signed)`, but the macro
is GAS `.macro ASM_FUNC name`, invoked paren-free
([examples/add.s:11](../../examples/add.s#L11)). Copy-pasting the README snippet
fails to assemble; it is the *only* parenthesized usage in the repo, and it's the
exact 5-minute onboarding path. [docs/getting-started/quickstart.md:26](../../docs/getting-started/quickstart.md#L26)
already shows the correct form.

### D3. Stale `1.0.0` version strings — Medium
`VERSION` and [include/asmtest.h:44](../../include/asmtest.h#L44) are `1.1.0`, but
the README, [docs/conf.py:16](../../docs/conf.py#L16) (so the published docs
banner reads "asm-test 1.0"), [SECURITY.md](../../SECURITY.md), and
integration/api-reference examples still say `1.0.0`.
[scripts/sync-version.sh](../../scripts/sync-version.sh) rewrites only binding
manifests, so `check-version` stays green over the drift. Fix the strings and
bring `conf.py` (and README/SECURITY) under the sync script — or have `conf.py`
read `VERSION`.

### N1. The published Go module links a test-only fixture lib — High **[hand-verified]**
[bindings/go/asmtest.go:25](../../bindings/go/asmtest.go#L25) hard-codes
`#cgo LDFLAGS: … -lasmtest_corpus`, but `libasmtest_corpus` is a conformance
fixture built only by the binding-test targets: nothing in `make install-shared`
/ `install-shared-emu` / `package-libs` stages it (confirmed: `libasmtest_emu.so`
exports 0 `corpus_routine` symbols; only `libasmtest_corpus.so` does). An
out-of-repo consumer who `go get`s the module and links a binary gets
`cannot find -lasmtest_corpus`, so the published module is unconsumable as
documented — and release CI runs only `go vet + build` (no link), so it can't
catch it. Move `CorpusRoutine`/the `-lasmtest_corpus` directive into a
build-tagged file or an `asmtest/corpus` subpackage so only the tests pull the
fixture.

### N2. The Java binding is compiled `--enable-preview` — High **[hand-verified]**
`java-test`, `java-package`, and the release smoke all compile with
`javac --release 21 --enable-preview`
([mk/bindings.mk:192,365](../../mk/bindings.mk#L192)). Preview classfiles
(minor version `0xFFFF`) refuse to load on any JDK ≠ 21 and require
`--enable-preview` at runtime even there — so the jar `make java-package`
produces is dead on arrival for Maven Central consumers on JDK 22/25, where
`java.lang.foreign` is *final* and the flag is rejected. The pom itself notes FFM
is stable in 22+. Target `--release 22` and drop the preview flags everywhere.

### N7. `vec_add8d` is missing from the corpus name table — Low **[hand-verified]**
[bindings/conformance/corpus_routines.c:22](../../bindings/conformance/corpus_routines.c#L22)
declares `vec_add8d` (AVX-512) but the `asmtest_corpus_routine` lookup registers
only `vec_add4d` (lines 43–44), so Go/node/ruby each grew per-binding dlsym
workarounds. A two-line `strcmp` entry makes the name table the single source of
truth it claims to be and stops the 12th binding tripping over it.

---

## Improvements & expansions (verified)

### Core API
- **A6 — `ASM_MIXCALL` for mixed integer+FP args (expansion, small).** The
  ptr+len+scalar signature is canonical, but the only path is calling
  `asm_call_capture_fp` with hand-built compound literals; the header itself
  punts ([include/asmtest.h:713](../../include/asmtest.h#L713)) and the repo's own
  example hand-rolls it. A macro over the existing entry point closes the gap.
- **A7 — FP reference models for differential testing (expansion, medium).**
  `ASSERT_MATCHES_REF{1,2,3}` are `long`-only
  ([include/asmtest.h:443](../../include/asmtest.h#L443)), so the entire FP/SIMD
  surface — where rounding/NaN/lane bugs live — can't be property-tested against a
  C model. The call side (`asm_call_capture_fp`, ULP distance) already exists.

### Runner & reporting
- **R4 — Complete the JUnit report (improvement, medium).** Every non-pass maps
  to `<failure>` ([src/asmtest.c:1508-1570](../../src/asmtest.c#L1508)), so a
  SIGSEGV is indistinguishable from an assertion failure; no `errors=`/`time=`
  attributes; stdout is `dup2`'d to `/dev/null` instead of `<system-out>`.
  `wire_result_t` already tags crash paths, so classification needs no new
  plumbing (though `<system-out>` capture does).
- **R5 — `--fail-fast`, `--repeat=N`, shard selection (expansion, small).** The
  three flow-control features every peer runner ships; `--repeat` pairs with the
  existing `--shuffle` for flake hunting. All localized changes to the
  selection/run loops.
- **R6 — Bench dispersion + machine-readable output (improvement, medium).** Only
  min/median/mean, no stddev/CV, and no JSON/CSV baseline diff
  ([src/asmtest.c:1606](../../src/asmtest.c#L1606)). The per-round samples are
  already sorted; adding a dispersion column and `--bench-format=json` is mostly
  formatting.
- **R7 — Honor `NO_COLOR` and add `--color=auto|always|never` (improvement,
  small).** Color is `isatty`-only ([src/asmtest.c:1911](../../src/asmtest.c#L1911)),
  so CI viewers that render ANSI over a pipe get monochrome and there's no
  force-off.

### Emulator & tooling
- **E3 — Register-preload API, or retract the claim (defect/expansion, medium).**
  The header/guide/README pitch "preload arbitrary registers," but setup zeros the
  file and writes only argument registers
  ([src/emu.c:269](../../src/emu.c#L269)); no API sets a non-arg register. The
  name→UC-register map already exists for `emu_guard_reg`. Either add the API or
  scale the three claims back to "preload memory and arguments."
- **E2 — Fix the stale `emulator.md` retained-state note (docs, small).**
  [docs/guides/emulator.md:289](../../docs/guides/emulator.md#L289) says register
  state is *retained* across calls and prescribes a fresh-handle workaround, but
  the code now zeros GP+XMM+EFLAGS every call
  ([src/emu.c:253](../../src/emu.c#L253)) — the note sends guard users chasing
  state that no longer exists.
- **E4 — Expose the fuzz corpus (improvement, small).** `emu_fuzz_cover1` keeps
  every coverage-growing input then frees it, returning only counts
  ([src/fuzz.c:39-75](../../src/fuzz.c#L39)); the caller can't replay, persist, or
  feed them into `emu_mutation_test1`, whose input shape is exactly that set.
- **E6 — Extend watchpoints to memory reads (expansion, small).**
  `emu_watch_writes` hooks only `UC_HOOK_MEM_WRITE`
  ([src/emu.c:125](../../src/emu.c#L125)); a routine *reading* a secret/uninit
  region or past a declared length can't be flagged. The plumbing is
  direction-agnostic.
- **E7 — Shrink failing inputs in `ASSERT_MATCHES_REF` (expansion, medium).** It
  reports the raw first-failing 64-bit draw
  ([src/asmtest.c:645](../../src/asmtest.c#L645)); a deterministic shrink toward
  `0`/`-1`/`0x8000…` lands on the boundary values where asm bugs actually are,
  cheaply, since the seed already makes it reproducible.
- **E5 — Emulator snapshot/restore (expansion, medium).** Mapped memory and stack
  persist across calls by design ([src/emu.c:250](../../src/emu.c#L250)), so
  fuzz/mutation sweeps run each candidate against memory dirtied by earlier runs,
  making killed/survived classification handle-history-dependent. A
  `uc_context_save/restore` + region-copy pair fixes it.

### Build / CI
- **K1 — Cache the Keystone/Capstone source builds (improvement, medium).**
  `Dockerfile.bindings-base` compiles Keystone (a trimmed LLVM) + Capstone from
  source with no cache anywhere (`grep cache .github/workflows` → nothing), so
  ~20 identical multi-minute compiles run per push across the bindings,
  clean-room, asm, package-libs, and release jobs. Both are version-pinned →
  `cache-from/to: type=gha` + an `actions/cache` on the host installs.
- **K2 — Wire `hwtrace-bindings-test` / `codeimage-test` into CI (expansion,
  small).** Both run live on any x86-64 Linux with no privilege and have ready
  docker lanes, but no CI job runs either
  ([mk/native-trace.mk:551](../../mk/native-trace.mk#L551)); the hwtrace job runs
  only the C harness. This is the cross-language regression the makefile calls
  "the tier's first that actually executes," and it directly covers the
  codeimage/ptrace code the tracked P0 flake is suspected to live in. *(Needs a
  python matrix exclusion — `HWTRACE_DOCKER_LANGS` omits python.)*
- **K3 — Widen clang-tidy & gcov past the single runner TU (improvement,
  small).** `make tidy` and `make coverage` cover only `src/asmtest.c` of 20 C
  files ([Makefile:571,550](../../Makefile#L571)); the emu/ffi/trace/ptrace/
  codeimage/hwtrace TUs — including the code memory suspects a latent bug in — are
  never analyzed. *(Exclude `platform_win32.c`/`drtrace_client.c`, which need
  windows.h / dr_api.h.)*
- **K4 — Run the optional tiers under ASan/UBSan (improvement, small).**
  `make sanitize` rebuilds only `test`+`check`
  ([Makefile:539](../../Makefile#L539)); the `SAN=1` plumbing already flows into
  every tier rule, so instrumenting `emu-test`/`hwtrace-test`/`codeimage-test`
  (the C the bindings dlopen) is mostly wiring — the cheapest systematic hunt for
  the memory-bug class.
- **K6 — Install the native-trace headers + add install targets (improvement,
  small).** `make install` omits `asmtest_hwtrace.h`/`asmtest_drtrace.h`/etc.
  ([Makefile:467](../../Makefile#L467)), and no `install-shared-hwtrace/-drtrace`
  target exists — so an installed-prefix consumer can't compile against either
  trace tier, though the guide shows `#include "asmtest_drtrace.h"` and
  packaging.md promises "(or installs the libs)."
- **K7 — Fix docs/workflow drift (docs, small).** `releasing.md` step 1 says bump
  the version "in the Makefile" and hand-edit manifests, but it moved to `VERSION`
  + `make sync-version` (CI-gated) — following the written steps trips
  `check-version`. README and `mk/docker.mk` still say arm64 CI runs "only test +
  emu" though it also runs `asm` and `package-libs`. `ci.md` describes ~6 of ~20
  jobs.

### Documentation
- **D2 — Restructure the README as a funnel into the docs site (docs, medium).**
  Its only `docs/` links are two plan files; it never links `docs/index.md` or the
  RTD site (no file states the URL), the 11 bindings / tracing guides / Win64 tier
  never appear, and a 73-line feature dump duplicates
  [docs/reference/features.md](../../docs/reference/features.md). Replace with a
  short pitch + quick start + a "Documentation" section mirroring
  [docs/index.md](../../docs/index.md)'s "Where to start."
- **D4 — Back-fill the API-reference index with the post-1.0 surface (docs,
  small).** [docs/reference/api-reference.md](../../docs/reference/api-reference.md)
  bills itself "a consolidated index" but omits the entire Win64 call family,
  `ASM_VCALL256/512`, `ASSERT_VEC256/512_EQ`, `asm_call_capture_vec256/512`, and
  the AVX gates — all documented in the guides.
- **D5 — Add a Troubleshooting / FAQ page (docs, medium).** No such content
  exists, yet the framework is full of environment-sensitive tiers
  (`perf_event_paranoid`, `LD_LIBRARY_PATH`/`ASMTEST_LIB`, Keystone source build,
  arm64 binfmt) whose guidance is scattered; a symptom→fix page also gives the
  self-skip design a landing spot.

### Strategic / product
- **P1 — Distribute the C core through system package managers (expansion,
  medium).** The static lib is the documented *primary* path, yet the only way to
  get it is `git clone && make install` — every packaging plan covers the 10
  language registries but never the C library itself. A brew tap, `.deb`, AUR
  PKGBUILD, and vcpkg/conan ports are incremental on the artifacts release.yml
  already builds. Sequenced after the first real tag (P4 in the open-defects plan).
- **P2 — Ship a consumer-facing CI integration (expansion, medium).** The runner
  emits JUnit "for CI ingestion," but a consumer wiring it up gets zero help
  (`ci.md` documents only this repo's matrix). A marketplace composite Action +
  a documented `.gitlab-ci.yml` snippet turns that into copy-paste onboarding.
- **P3 — Add a native RISC-V host tier (expansion, large).** RISC-V exists only
  as an emulator guest; the differentiating *native* capture tier is x86-64/
  AArch64 only ([src/capture.s](../../src/capture.s)). The roadmap excludes new
  *guest* archs, not a host port, and it's actionable via the existing
  `linux/riscv64` binfmt lane. *(Genuinely large: RISC-V has no flags register
  while the header promises flags on every target, and every `examples/*.s` needs
  a `__riscv` body.)*
- **P4 — Write a maintained "asm-test vs alternatives" comparison page (docs,
  small).** The only positioning is DESIGN.md's 2015-era prior-art table inside a
  self-declared-historical file; nothing addresses cmocka/Criterion + `.s`, raw
  Unicorn scripting, or qemu+gdb.
- **P5 — Package the teaching story (expansion, medium).** Education is a
  recognized audience but there's no assignment-template repo, GitHub Classroom
  autograding recipe, or instructor guide. All primitives exist (JUnit for
  autograders, fork isolation, RISC-V/ARM32 guests that run student code on any
  laptop).

---

## Finder-only leads (not independently verified)

The bindings and self-test verifiers were interrupted; these have solid
`file:line` evidence but no second pass. Re-check before acting.

- **N3 — Lua rock & Java jar aren't actually publishable as documented.**
  `make lua-package` never invokes `luarocks` (`LUAROCKS` is defined but unused)
  and the rockspec's `build.install.lib` is empty; `java-package` is a bare
  `jar cf` (the pom is never used), missing the sources/javadoc/signing Maven
  Central needs. Anchors: [mk/bindings.mk:390](../../mk/bindings.mk#L390),
  [bindings/lua/](../../bindings/lua/), [bindings/java/pom.xml](../../bindings/java/pom.xml).
- **N4 — Wide-arity / struct-return / mixed-FP capture unreachable from every
  binding.** No binding references `capture_args`/`sret`/`bigstruct`; the FFI layer
  offers only `capture6`/`fp2`/`vec_f32`. The array-form C entry points already
  exist and are FFI-friendly. Anchors: [src/ffi.c:68](../../src/ffi.c#L68).
- **N5 — The Zig package exports no importable module.**
  [bindings/zig/build.zig](../../bindings/zig/build.zig) never calls `addModule`,
  so a consumer can't `@import("asmtest")` — contradicting the docs' "reusable
  library module" claim.
- **N6 — No tripwire stops a new corpus case silently skipping 9/10 bindings.**
  Only Python replays `corpus.json` as data; the rest hand-mirror cases. A sibling
  to [scripts/check-bindings-parity.sh](../../scripts/check-bindings-parity.sh)
  requiring each case name to appear per binding closes the blind spot.
- **X2 — Headline negative-path features run only with exit codes ignored.**
  Guard-page overrun/underrun and `ASSERT_MATCHES_REF` mismatch are exercised only
  in `test_failure_demo`/`test_robust`, whose recipes prefix `-` and assert
  nothing; a regression (e.g. `guarded_alloc` degrading to `malloc`) keeps CI
  green. Add pure-C cases to [tests/negative.c](../../tests/negative.c) with
  `expect_fail_msg` lines.
- **X1 — Nothing asserts `TEARDOWN` runs.** [tests/positive.c](../../tests/positive.c)
  defines a teardown whose effect is never observed (single test; fork resets
  state anyway). Make SETUP/TEARDOWN inc/dec a counter across two tests under
  `--no-fork`.
- **X3 — Crash containment self-tested for SIGSEGV/SIGABRT only.** The runner
  handles SIGILL/SIGFPE/SIGBUS + a sigaltstack overflow path, but
  [tests/negative.c](../../tests/negative.c) never raises them — and SIGILL (bad
  opcode) / SIGFPE (div-by-zero) are the most common real asm-routine crashes.
- **X4 — JUnit escaping never validated.** `expect.sh`'s well-formed-XML check is
  guarded by `command -v xmllint`, which no lane installs, and it only checks the
  all-passing suite (no `<failure>` element). Add `libxml2-utils` to deps and a
  negative case whose message contains `<&">`.

---

## Suggested fix order

1. **The "green CI, broken artifact" defects first — D1, N1, N2, A1, A2.** Each
   breaks a real first-contact path (assemble the README example, `go get` the
   module, load the jar, compile as C11/C++) and none is caught by a current gate.
   All small except A2.
2. **The cheap correctness wins — A3, A4, A5, R1, R2, R3, T1, E1, N7, D3, E2.**
   Silent false-passes, spec violations, and stale claims; each is small.
3. **CI leverage — K3, K4, K5, K1, K2.** Widen the analyzers/sanitizers over the
   whole tree (K3/K4/K5 target exactly the code the tracked P0 flake hides in),
   then cut the ~20×-per-push Keystone rebuild (K1) and cover the live trace
   bindings (K2).
4. **DX & docs — R4, R6, R7, K6, K7, D2, D4, D5, E3, E4, E6.** Peer-parity and
   discoverability; mostly small.
5. **Expansions on demand — A6, A7, R5, E5, E7, P1–P5.** Genuine new capability;
   pick against a concrete user need. P1/P2 are sequenced behind the first real
   tag.
6. **Re-verify then action the finder-only leads — N3–N6, X1–X4.**

*Method note: nine dimension reviewers with adversarial verification; the tracked
P0–P5 and hardware/privilege-blocked sets were supplied as exclusions, so no item
here duplicates existing plans. The bindings/self-test verifiers were interrupted
by a spend limit — see the Caveat.*
