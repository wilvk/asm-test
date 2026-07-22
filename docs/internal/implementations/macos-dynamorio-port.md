# macOS DynamoRIO native-trace port (M0-M2), gated on an upstream DR macOS release — implementation

> **Sources.** Actioned from
> [macos-drtrace-plan.md](../plans/macos-drtrace-plan.md) (phases M0/M1/M2 and
> the standing upstream gate) and
> [dynamorio-native-trace-plan.md](../archive/plans/dynamorio-native-trace-plan.md)
> (the landed Linux tier this ports). Written 2026-07-17. If this doc and a
> source disagree, this doc wins (sources may be stale); if the CODE and this
> doc disagree, re-verify before implementing.

> **UPDATE 2026-07-22 — the upstream-release gate is retired; this doc is
> unblocked via a source fork.** A local fork of DynamoRIO
> (`wilvk/dynamorio`, at `/Users/willemvanketwich/source/dynamorio`) builds
> `lib64/release/libdynamorio.dylib` on macOS x86-64 (verified live on the
> Intel-mac dev host). See
> [macos-dynamorio-fork-build.md](macos-dynamorio-fork-build.md), which
> **supersedes T1** (release recheck) and **T2** (inspect a release tarball —
> there is none; the fork confirms the `lib64/release/libdynamorio.dylib`
> layout) and provides the runtime that **T3/T4/T5** consume once its FB2 (fix
> the `dr_app_setup` startup crash at `core/unix/os.c:1192`) is green. T6–T10
> below still apply unchanged; T6/T8's Apple-Silicon gates remain. Read the
> "Constraints & gates" note at the bottom, now amended, before treating any T1
> "blocked upstream" language below as current.

## Why this work exists

The DynamoRIO in-process native-trace tier (trace real code on the real CPU,
in-process, via `dr_app_*` cooperative attach) is implemented and validated on
Linux x86-64 only. macOS users today get the Unicorn emulator tier and the
single-step tier, but no DynamoRIO tier. This doc is the complete, ordered
recipe for porting that tier to macOS — Intel x86-64 first (in-process attach +
Mach-O marker resolution), then generated-code W^X on Intel/Rosetta and Apple
Silicon, then bindings + CI — the moment the one thing it is blocked on exists:
**an upstream DynamoRIO macOS release asset, which has never been published**
(verified live 2026-07-17; see T1). Only T1, the standing recheck, is
executable today; every other task is held behind it by the plan's own
directive.

## What already exists (verified 2026-07-17)

The landed Linux substrate this port extends:

- [src/drtrace_app.c](../../../src/drtrace_app.c) — app-side lifecycle
  (`dr_app_setup/start/stop_and_cleanup` via `dlopen`/`dlsym`), the exported
  begin/end/register markers, and the W^X exec-memory helpers. Everything in
  it is POSIX and compiles on macOS today (the plan verified
  `cc -c src/drtrace_app.c` on macOS x86-64), but every libdynamorio path is
  hardcoded `.so`: `dr_bundled_lib` (lines 55-67, hardcode at 60),
  `dr_lib_path` (lines 76-98, hardcodes at 82, 93, 96), and the `dr_probe`
  availability cascade (lines 163-196, hardcode at 180, plus the
  human-readable reason strings at 186-194). `grep -n '__APPLE__\|dylib'
  src/drtrace_app.c` returns nothing.
- [src/drtrace_client.c](../../../src/drtrace_client.c) — the DR client.
  Marker resolution is `dr_get_proc_address` over every loaded module
  (`try_resolve` lines 338-352, `resolve_all_modules` lines 354-362); signal
  chaining is `DR_SIGNAL_DELIVER` (lines 391-395). Whether
  `dr_get_proc_address` resolves exported symbols in a **Mach-O** main
  executable is the load-bearing unknown M0 exists to answer.
- [mk/native-trace.mk](../../../mk/native-trace.mk) — the tier's Make targets.
  `DR_LIBDIR`/`DR_DLLIB` hardcode `$(DYNAMORIO_HOME)/lib64/release/libdynamorio.so`
  (lines 35-36); the client rule target is the literal
  `$(BUILD)/libasmtest_drclient.so` (lines 88-101, grouped-output stubs for the
  val/taint clients at 105-111); `test_drtrace` links `-rdynamic ... -ldl`
  (lines 132-134); the `drtrace-test` run rule sets
  `ASMTEST_DRCLIENT=...libasmtest_drclient.so` (lines 146-148). The
  "DynamoRIO not found" skip message names `DynamoRIO-Linux-<ver>` at **32**
  sites (`grep -c 'DynamoRIO-Linux-<ver>' mk/native-trace.mk` → 32). DR-tier
  link rules pass `-ldl` unconditionally (lines 118, 134, 196, 206, 272, 282,
  355, 365, 538; plus line 1823, the out-of-scope `dr_valtrace_bench` benchmark
  rule that T10 deliberately leaves alone). The per-binding `drtrace_env`
  (lines 2355-2358) hardcodes
  `libasmtest_drapp.so`, `libasmtest_drclient.so`, and `LD_LIBRARY_PATH`.
- [Makefile](../../../Makefile) — `UNAME_S := $(shell uname -s)` at line 221;
  the `shlib_*` macros (lines 219-235) already emit `.dylib` on Darwin, so
  `shared-drtrace`'s `libasmtest_drapp` artifact name is *already*
  platform-correct; only the DR-specific paths above are not.
- [examples/test_drtrace.c](../../../examples/test_drtrace.c) — the Linux
  smoke harness. TAP-style `CHECK` macro at lines 26-35; the hardcoded
  **x86-64** machine-code `ROUTINE[]` at lines 47-54 (cannot run on arm64);
  symbol mode (`asmtest_dr_register_symbol("asmtest_symbol_demo", ...)`) at
  lines 163-169 — the one case that traces a normally-compiled function.
  `asmtest_symbol_demo` itself lives in
  [src/drtrace_app.c](../../../src/drtrace_app.c) (line 369) and is declared
  in [include/asmtest_drtrace.h](../../../include/asmtest_drtrace.h) (line 137).
- [.github/workflows/ci.yml](../../../.github/workflows/ci.yml) — the Linux
  `drtrace` job (lines 336-359) pins `DR_VERSION: 11.91.20630` (line 341) and
  fetches `DynamoRIO-Linux-${DR_VERSION}.tar.gz`; the nightly Intel-macOS
  pattern to mirror is `test-macos-x86` (lines 143-147): schedule/dispatch
  gated, `runs-on: macos-15-intel`, with the comment block at lines 139-142
  recording that `macos-13` was retired 2025-12-08. The workflow cron is
  `'0 7 * * *'` (line 10).
- [Dockerfile.drtrace](../../../Dockerfile.drtrace) — the `ARG DR_VERSION`
  pin pattern (lines 35-36, `11.91.20630`).
- [docs/guides/tracing/native-tracing.md](../../guides/tracing/native-tracing.md)
  — the user-facing guide whose backend table currently says the DynamoRIO
  tier runs on "Linux x86-64".
- [docs/internal/plans/macos-drtrace-plan.md](../plans/macos-drtrace-plan.md)
  — the plan, with the STATUS block at lines 19-69 holding the 2026-06-30 and
  2026-07-16 gate-check results. T1 appends to it.

**Nothing macOS-specific exists yet**: no `examples/test_drtrace_macos.c`, no
`drtrace-test-macos` target, no `*.entitlements` file anywhere in the tree, no
`MAP_JIT`/`pthread_jit_write_protect_np`/`__aarch64__` in `src/drtrace_app.c`,
no `drtrace-macos` CI job.

**Prove the baseline green before touching anything:**

```sh
make help                 # lists drtrace-test / docker-drtrace among targets
make drtrace-test         # on a host without DynamoRIO: prints
                          #   "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=..." then "1..0 # skipped"
make docker-drtrace       # the full Linux lane in a container: C smoke + Python wrapper, all green
```

## Tasks

### T1 — Run and record the standing upstream go/no-go recheck  (S, depends on: none)

**Goal.** The plan's STATUS block carries a dated 2026-07-17 entry proving the
releases-API check was re-run and still finds zero macOS DynamoRIO assets.

**Steps.**
1. Run the gate check (this is the whole go/no-go; the plan says nothing else
   in it matters until this prints an asset):
   ```sh
   gh api --paginate '/repos/DynamoRIO/dynamorio/releases' --jq '.[].assets[].name' \
     | grep -iE 'mac|darwin|osx' || echo 'still no macOS asset — plan stays blocked'
   gh api '/repos/DynamoRIO/dynamorio/releases?per_page=1' --jq '.[0].tag_name'
   ```
   Re-run live 2026-07-17 while authoring this doc: the grep matched **0**
   assets; the latest release is `cronbuild-11.91.20644`.
2. Append a dated result block to the STATUS section of
   [macos-drtrace-plan.md](../plans/macos-drtrace-plan.md) (after the
   "Re-confirmed 2026-07-16" block, i.e. after line 69): date, latest
   cronbuild tag, match count, and "still NO-GO" (or, if an asset ever
   appears: the asset name, and a pointer to this doc's T2).
3. Record the recheck cadence in the same block: re-run **before any T2+ work
   is ever scheduled**, and whenever this plan or this implementation doc is
   next touched. Keep it manual — a one-command check does not warrant CI
   automation, and a scheduled job that fails "successfully" for years is
   noise.

**Code.** None (doc edit only).

**Tests.** No testable surface — the "test" is the command itself and its
recorded output. Manual verification: the grep prints nothing and the
`|| echo` fallback fires.

**Docs.** Internal-only (the plan's STATUS block). No changelog entry: not a
user-visible change.

**Done when.**
- The command above has been run and its output recorded verbatim (date,
  latest tag, 0 matches) in the plan's STATUS block.
- The block still states, correctly, that M0-M2 remain held and that buying
  Apple hardware does not unblock the plan.

### T2 — Obtain and inspect the macOS DynamoRIO release (plan Step 0)  (S, depends on: T1 printing an asset)

**Goal.** The real macOS tarball's layout, lipo arches, and CMake client
support are recorded in the plan before any path is wired.

**Steps.**
1. Download the asset T1 found from
   `https://github.com/DynamoRIO/dynamorio/releases/download/<tag>/<asset>`
   and unpack it.
2. Record in the plan's Step-0 section, replacing its "does not exist" result:
   - the actual relative path of `libdynamorio.dylib`. Expect
   `lib64/release/` — DynamoRIO's CMake install rules are platform-independent
   (`INSTALL_LIB_X64=lib64`, `.../release|debug`, no APPLE override; see
   Research notes) — but confirm against the unpacked tree.
   - presence of `cmake/DynamoRIOConfig.cmake` and a working
     `configure_DynamoRIO_client` (the client build in
     [drclient/CMakeLists.txt](../../../drclient/CMakeLists.txt) needs it).
   - `lipo -info <path>/libdynamorio.dylib` — x86-64-only or universal. All
     current evidence says x86-64-only (upstream's only macOS CI is
     `osx-x86-64` on `macos-15-intel`; the arm64 port is a separate open
     issue, i#5383). If x86-64-only, T8's arm64 leg stays gated on a second
     upstream event.
3. Compute and record `shasum -a 256` of the tarball — T11 pins it.

**Code.** None.

**Tests.** No testable surface; the deliverable is the recorded findings.

**Docs.** Internal-only (plan Step-0 result block).

**Done when.**
- The plan records: dylib path, lipo output, cmake-dir presence, tarball
  SHA-256, and the exact release tag.
- A go/no-go line: if the tarball lacks the Application Interface or client
  CMake support, the port stops here again (record why), per the plan's own
  Step-0 instruction.

### T3 — dylib-aware libdynamorio resolution in src/drtrace_app.c  (S, depends on: T2)

**Goal.** All five hardcoded `libdynamorio.so` sites in
[src/drtrace_app.c](../../../src/drtrace_app.c) resolve
`libdynamorio.dylib` on Darwin, byte-for-byte unchanged elsewhere.

**Steps.**
1. Near the top of `src/drtrace_app.c` (after the includes), add:
   ```c
   #if defined(__APPLE__)
   #define DR_LIBNAME "libdynamorio.dylib"
   #else
   #define DR_LIBNAME "libdynamorio.so"
   #endif
   ```
2. Replace the literals using C string-literal concatenation:
   - `dr_bundled_lib` line 60: `"%.*s/" DR_LIBNAME`
   - `dr_lib_path` lines 82 and 93: `"%s/lib64/release/" DR_LIBNAME`
     (keep `lib64/release/` unless T2 recorded a different subdir; if it did,
     add a `DR_LIBSUBDIR` define next to `DR_LIBNAME`)
   - `dr_lib_path` line 96 (bare-soname fallback): `DR_LIBNAME`
   - `dr_probe` line 180: same `"%s/lib64/release/" DR_LIBNAME`
3. Update the two reason strings in `dr_probe` (lines 186-194) so the message
   no longer hardcodes "libdynamorio.so" — either interpolate `DR_LIBNAME` or
   reword to "libdynamorio". Do the same for the `dr_bundled_lib` header
   comment (line 53), which also spells the bare `.so` soname; leaving it makes
   the "Done when" grep below report 2 instead of 1.
4. `make fmt` (clang-format is CI-gated via `fmt-check`).

**Code.** As above. Behavior contract: off Darwin the preprocessed output is
identical to today; the availability cascade
(`ASMTEST_DR_LIB` → bundled sibling → `DYNAMORIO_HOME` → bare soname) is
untouched in structure.

**Tests.** No new test file — this is exercised by every existing DR lane.
Prove no Linux regression with `make docker-drtrace` (all green, as baseline).
On the macOS dev host, prove it compiles: `cc -Iinclude -c src/drtrace_app.c
-o /tmp/drapp.o` exits 0. The runtime proof is T5.

**Docs.** Internal-only at this point (behavioral surface unchanged until T5);
user docs and changelog land with T5.

**Done when.**
- `grep -c 'libdynamorio\.so' src/drtrace_app.c` counts only the `DR_LIBNAME`
  definition (one site), zero raw literals elsewhere.
- `make docker-drtrace` is green (Linux behavior identical).
- `make fmt-check` passes.

### T4 — Darwin-conditional DR paths + the `drtrace-test-macos` target  (S, depends on: T2)

**Goal.** `mk/native-trace.mk` resolves `.dylib` names on Darwin, the DR-tier
link lines drop `-ldl` on Darwin, and a new `drtrace-test-macos` target builds
and runs the M0 harness.

**Steps.** All edits in [mk/native-trace.mk](../../../mk/native-trace.mk)
(target groups live in `mk/*.mk`, not the root Makefile), plus one `help` line.
1. Replace lines 35-36 with a platform conditional, mirroring the
   `ifeq ($(UNAME_S),Darwin)` pattern already at lines 14-18 of the same file
   (and [mk/bindings.mk](../../../mk/bindings.mk) lines 83-88):
   ```makefile
   ifeq ($(UNAME_S),Darwin)
   DRCLIENT_EXT := .dylib
   DRTRACE_LDFLAGS := -rdynamic -lpthread   # dlopen lives in libSystem; clang maps -rdynamic to -export_dynamic
   else
   DRCLIENT_EXT := .so
   DRTRACE_LDFLAGS := -rdynamic -ldl -lpthread
   endif
   DR_LIBDIR := $(DYNAMORIO_HOME)/lib64/release   # Darwin subdir confirmed by T2
   DR_DLLIB  := $(DR_LIBDIR)/libdynamorio$(if $(filter Darwin,$(UNAME_S)),.dylib,.so)
   DR_CLIENT := $(BUILD)/libasmtest_drclient$(DRCLIENT_EXT)
   ```
2. Re-target the client rule on the variable: `drtrace-client: $(DR_CLIENT)`,
   rule header `$(DR_CLIENT): src/drtrace_client.c ...` (lines 88-101), and
   the three grouped-output stubs (lines 105-111) become
   `$(BUILD)/libasmtest_drval_client$(DRCLIENT_EXT): $(DR_CLIENT) ;` etc.
   CMake's five unconditional `add_library(... SHARED)` client targets in
   [drclient/CMakeLists.txt](../../../drclient/CMakeLists.txt)
   (`asmtest_drclient`, `asmtest_drval_client`, `asmtest_drval_client_inlined`,
   `asmtest_drtaint_client`, `asmtest_drtaint_gcremap_client`) already emit
   `.dylib` on macOS with no CMake edit.
3. In `drtrace-test` (lines 146-148), set
   `ASMTEST_DRCLIENT=$(abspath $(DR_CLIENT))`.
4. Swap the hardcoded `-rdynamic ... -ldl -lpthread` on the **standalone
   harness link rules only** for `$(DRTRACE_LDFLAGS)` — start with
   `test_drtrace` (line 134); T10 finishes the remaining sites.
5. Add the new target after `drtrace-test`, mirroring its shape (lines
   136-161) minus the Linux-only val/taint sub-lanes:
   ```makefile
   .PHONY: drtrace-test-macos
   drtrace-test-macos:
   ifneq ($(UNAME_S),Darwin)
   	@echo "== drtrace-test-macos =="
   	@echo "# SKIP: Darwin-only target (host is $(UNAME_S))"
   	@echo "1..0 # skipped"
   else ifndef DR_AVAILABLE
   	@echo "== drtrace-test-macos =="
   	@echo "# SKIP: DynamoRIO not found. Set DYNAMORIO_HOME=/path/to/DynamoRIO-$(UNAME_S)-<ver>"
   	@echo "1..0 # skipped"
   else
   	@$(MAKE) drtrace-client
   	@$(MAKE) $(BUILD)/test_drtrace_macos
   	@echo "== drtrace-test-macos =="
   	ASMTEST_DRCLIENT=$(abspath $(DR_CLIENT)) \
   	ASMTEST_DR_LIB=$(abspath $(DR_DLLIB)) \
   	    $(BUILD)/test_drtrace_macos
   endif
   ```
   with a `$(BUILD)/test_drtrace_macos` build rule copied from the
   `test_drtrace` rule (lines 132-134), using `$(DRTRACE_LDFLAGS)`.
6. Leave every `docker-drtrace*` lane on `.so`: they run Linux containers
   regardless of host. Add one comment above the conditional in step 1 saying
   exactly that.
7. Add a `drtrace-test-macos` line to the `help` target
   ([Makefile](../../../Makefile) around lines 115-116, next to
   `drtrace-test`).

**Code.** As above. Do not touch `mk/docker.mk`.

**Tests.** On Linux: `make drtrace-test` (docker: `make docker-drtrace`)
unchanged-green; `make drtrace-test-macos` prints the Darwin-only SKIP and
`1..0 # skipped`. On macOS without DR: `make drtrace-test-macos` prints the
DynamoRIO-not-found SKIP. The positive path is T5.

**Docs.** Internal-only until T5.

**Done when.**
- `make drtrace-test-macos` self-skips cleanly on Linux (Darwin-only message)
  and on a DR-less Mac (not-found message).
- `make docker-drtrace` is green.
- `make -n drtrace-test DYNAMORIO_HOME=/tmp/x` on macOS shows `.dylib` paths
  throughout; on Linux `.so`.

### T5 — M0 compiled-function harness + the attach/marker go-no-go run  (M, depends on: T3, T4)

**Goal.** On macOS x86-64 with the T2 release, a normally-compiled function is
traced end-to-end (attach, marker resolution, coverage, clean shutdown) — the
go/no-go for the whole port.

**Steps.**
1. Write `examples/test_drtrace_macos.c`. Do **not** start from
   `test_drtrace.c` (it leans on the generated-bytes path); copy only its
   TAP `CHECK` macro (lines 26-35) and skip-preamble (lines 62-84). Body, per
   the plan's sketch:
   ```c
   /* compiled into __TEXT — no exec_alloc, no W^X, no entitlement */
   __attribute__((noinline)) static long add2(long a, long b) { return a + b; }
   ...
   asmtest_trace_t *tr = asmtest_trace_new(64, 64);
   CHECK(asmtest_dr_register_region("add2", (void *)add2, 64, tr) == ASMTEST_DR_OK, ...);
   asmtest_trace_begin("add2");
   long r = add2(20, 22);
   asmtest_trace_end("add2");
   CHECK(r == 42, ...);
   CHECK(asmtest_trace_covered(tr, 0), "block offset 0 covered");
   CHECK(asmtest_dr_marker_error() == 0, "markers balanced");
   ```
   Also exercise symbol mode — `asmtest_dr_register_symbol("asmtest_symbol_demo",
   256, str)` then `asmtest_symbol_demo(3, 4) == 10`, mirroring
   `test_drtrace.c` lines 163-169 — it walks the same `dr_get_proc_address`
   path, and whichever of the two resolves more cleanly on Mach-O is the
   acceptance anchor.
2. Run: `DYNAMORIO_HOME=/path/to/<T2-unpack> make drtrace-client
   drtrace-test-macos`. Watch, in the plan's order of likelihood-to-fail:
   (1) `dlopen(libdynamorio.dylib)` + `dr_app_setup` succeed; (2)
   `dr_app_start` returns (the Mach all-thread-takeover risk); (3) markers
   resolve — **if they do not, begin/end are silent no-ops and coverage is
   empty with nothing crashing**, so the `asmtest_trace_covered(tr, 0)` CHECK
   is the detector; (4) the block callback writes through the app-owned
   trace; (5) the `DR_SIGNAL_DELIVER` path doesn't crash; (6)
   `dr_app_stop_and_cleanup` returns the process to native.
3. Record the result in the plan (green, or the failing step + root cause).
   On a no-go at attach or marker resolution: file the root cause upstream
   (the tracker's load-bearing open issues are in Research notes), hold
   T6-T11, and record that in the plan's STATUS block.

**Code.** One new example file; no library changes beyond T3/T4.

**Tests.** The harness *is* the test: TAP output, `ok N - ...` per CHECK, exit
0. Failure looks like `not ok N - block offset 0 covered` (marker-resolution
failure) or a crash in `dr_app_start` (attach failure).

**Docs.** On green: add a macOS column/row note to the backend table in
[native-tracing.md](../../guides/tracing/native-tracing.md) ("macOS x86-64:
compiled-function/symbol mode") and a `### Added` bullet under
`## [Unreleased]` in [CHANGELOG.md](../../../CHANGELOG.md).

**Done when.**
- `make drtrace-test-macos DYNAMORIO_HOME=...` on macOS x86-64 prints all-`ok`
  TAP and exits 0, twice in a row (re-attach is out of scope; each run is one
  process lifecycle).
- The plan's M0 section records GO (or the documented no-go).
- `make docker-drtrace` still green.

### T6 — M1a: generated-bytes path on native Intel macOS + Rosetta verdict  (S, depends on: T5)

**Goal.** The existing `test_drtrace` generated-code assertions pass on native
Intel macOS, and the Rosetta 2 result is recorded as pass or
documented-unsupported.

**Steps.**
1. Extend `drtrace-test-macos` (T4) to also build and run
   `$(BUILD)/test_drtrace` after `test_drtrace_macos` — the existing
   `PROT_NONE → RW → RX` mprotect dance in `asmtest_exec_alloc`
   ([src/drtrace_app.c](../../../src/drtrace_app.c) lines 492-513) is expected
   to work unchanged on Intel silicon (no hardware W^X). Do **not** run the
   full `drtrace-test` target on Darwin: its val/taint sub-lanes are
   Linux-only (`-lrt`, `/dev/shm`, `drrun` launch lanes).
2. Rosetta leg (Apple Silicon hardware required): build the x86-64 slice and
   run it under Rosetta, mirroring the CI Rosetta job's approach
   ([ci.yml](../../../.github/workflows/ci.yml) lines 214-234):
   `make CC="cc -arch x86_64" drtrace-test-macos DYNAMORIO_HOME=<x86-64 DR>`.
   Expectation from the research: Rosetta emulates legacy mprotect-style JIT
   by fault-driven RW/RX page-state toggling, and our dance never requests
   simultaneous RWX — but this is exactly the plan's must-verify case, so
   record the observed result either way.
3. Record both verdicts in the plan's M1a section.

**Code.** Makefile target extension only.

**Tests.** `test_drtrace`'s existing 15+ CHECKs (coverage accumulation,
instruction-mode exact offset sequence, truncation bit) — unchanged. Pass =
all `ok`, exit 0. A Rosetta failure mode to watch: a hang or SIGBUS on first
execution of freshly-mprotected code.

**Docs.** Update the [native-tracing.md](../../guides/tracing/native-tracing.md)
macOS note with the generated-code verdict ("native Intel: supported; Rosetta:
<recorded verdict>"); changelog bullet amended in the same `### Added` entry
as T5.

**Done when.**
- `make drtrace-test-macos DYNAMORIO_HOME=...` on native Intel macOS runs both
  harnesses green.
- The plan's M1a section records the native-Intel pass and the Rosetta verdict
  (pass or documented-unsupported with the observed failure).
- The Rosetta leg self-skips honestly when no Apple Silicon host is available:
  record "not run — no Apple Silicon hardware" rather than guessing.

### T7 — M1b: platform exec-memory helpers + arm64 ENOSYS in asm_exec_native  (M, depends on: T5)

**Goal.** Executable-memory allocation is isolated behind
`exec_alloc_platform()`/`exec_copy()` helpers whose arm64-macOS arm uses
`MAP_JIT` + `pthread_jit_write_protect_np`, with every other platform
byte-for-byte unchanged, and `asmtest_asm_exec_native` refuses (ENOSYS) on
non-x86-64 hosts instead of assembling x86-64 bytes for them.

**Steps.**
1. In [src/drtrace_app.c](../../../src/drtrace_app.c), add the two static
   helpers exactly as the plan gives them (M1b "Problem 2" code block):
   `#if defined(__APPLE__) && defined(__aarch64__)` →
   `mmap(PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS|MAP_JIT)`
   in `exec_alloc_platform`, and
   `pthread_jit_write_protect_np(0); memcpy; pthread_jit_write_protect_np(1);
   __builtin___clear_cache` in `exec_copy`; `#else` → today's
   `PROT_NONE → mprotect(RW)` alloc and `memcpy → mprotect(RX) → clear_cache`
   copy. (On macOS, `MAP_JIT` mappings are never writable and executable
   simultaneously; the write-protect toggle is per-thread — see Research
   notes. `mprotect` must not be used on the arm64 arm.)
2. Refactor `asmtest_exec_alloc` (lines 492-513) and
   `asmtest_asm_exec_native` (lines 515-564) onto the helpers. The two-pass
   Keystone assemble in the latter fits: `exec_alloc_platform` yields the
   address for pass 2, then `exec_copy` copies + seals.
3. Arch-gate `asmtest_asm_exec_native`: it hardcodes `ASM_X86_64` at lines
   527 and 541 (the existing ENOSYS at line 521 is Keystone-absence only, not
   an arch gate). Add, at the top of the `#else` (Keystone-present) branch:
   ```c
   #if !defined(__x86_64__)
       return ASMTEST_DR_ENOSYS; /* host-native assembly is x86-64-only */
   #endif
   ```
   arm64 Keystone host-native assembly is explicitly out of scope.
4. `make fmt`; prove Linux is byte-identical in behavior:
   `make docker-drtrace` green.

**Code.** As above; `ASMTEST_DR_ENOSYS` is already defined in
[include/asmtest_drtrace.h](../../../include/asmtest_drtrace.h) (line 42).

**Tests.** Linux: the full existing DR suite via `make docker-drtrace` (the
helpers must be a pure refactor there). arm64 macOS compile check:
`cc -Iinclude -c src/drtrace_app.c` on Apple Silicon (or
`cc -arch arm64 ...` from any Mac) exits 0. Runtime proof is T8.

**Docs.** Internal-only until T8.

**Done when.**
- `make docker-drtrace` green (Linux unchanged).
- `grep -n 'exec_alloc_platform\|exec_copy' src/drtrace_app.c` shows both
  helpers and both callers refactored.
- On an arm64 build, `asmtest_asm_exec_native` returns `ASMTEST_DR_ENOSYS`
  (unit-observable from any harness once T8 runs).

### T8 — M1b: entitlement file, codesign step, arm64 acceptance + limitation docs  (M, depends on: T7; gated on Apple Silicon hardware AND an arm64/universal macOS DR binary)

**Goal.** `test_drtrace` is ad-hoc signed with the JIT entitlement on Darwin,
arm64 acceptance runs via the compiled-function harness, and the
hardened-interpreter limitation is documented as a limitation, not a TODO.

**Steps.**
1. New file `drtrace.entitlements` at the repo root:
   ```xml
   <?xml version="1.0" encoding="UTF-8"?>
   <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
   <plist version="1.0"><dict>
     <key>com.apple.security.cs.allow-jit</key><true/>
   </dict></plist>
   ```
2. In [mk/native-trace.mk](../../../mk/native-trace.mk), append a Darwin-only
   codesign line to the `$(BUILD)/test_drtrace` link rule (and only that
   rule — signing `libasmtest_drapp.dylib` does nothing: entitlements attach
   to the process's **main executable**):
   ```makefile
   ifeq ($(UNAME_S),Darwin)
   	codesign --entitlements drtrace.entitlements -s - $@
   endif
   ```
   **Never add `--options runtime`**: (a) without the hardened runtime the
   entitlement is not even required for `MAP_JIT`, and (b) *with* it a process
   may create only **one** `MAP_JIT` region — `test_drtrace` calls
   `asmtest_exec_alloc` three times (code/code2/code3), so a hardened-runtime
   signature would break the suite by design. Ad-hoc, non-hardened signing is
   the correct shape (see Research notes).
3. arm64 acceptance routes through the **M0 compiled-function harness**
   (plan option (b)): `make drtrace-test-macos DYNAMORIO_HOME=<arm64 DR>` on
   Apple Silicon. Do not add arm64 byte fixtures — `ROUTINE[]`
   ([test_drtrace.c](../../../examples/test_drtrace.c) lines 47-54) stays
   x86-64-only. Record honestly in the plan that on arm64 the `MAP_JIT`
   helpers are therefore exercised to compile + allocation smoke, not through
   a generated-code trace; a future arm64 fixture is a separate decision.
4. Document the binding limitation (T9 ships the text): generated code is
   standalone-executable-only on hardened arm64 — a hardened, Apple-signed
   interpreter (system `python3`/`node`/`ruby`) cannot be re-signed with
   `allow-jit`, so bindings on arm64 use compiled-function/symbol mode;
   ad-hoc-signed Homebrew interpreters do not need the entitlement at all
   (non-hardened processes are exempt), but that is their packaging accident,
   not our contract.

**Code.** One new plist; one Makefile stanza.

**Tests.** On Apple Silicon with an arm64 DR: `make drtrace-test-macos` green
(compiled-function harness — no entitlement needed for it);
`codesign -d --entitlements :- build/test_drtrace` prints the allow-jit key.
Without the hardware/upstream binary: the lane self-skips and the gate is
recorded (see Constraints).

**Docs.** [native-tracing.md](../../guides/tracing/native-tracing.md): a
"macOS arm64" subsection stating compiled-function/symbol mode is the
supported path, generated code is standalone-executable-only, and hardened
interpreters are excluded by the OS security model. Changelog: extend the
T5 `### Added` entry.

**Done when.**
- `drtrace.entitlements` exists; the signed harness verifies with `codesign -d`.
- On Apple Silicon + arm64 DR: `make drtrace-test-macos` green.
- The plan's M1b section records what ran, what was gated, and the recorded
  extent of MAP_JIT exercise.

### T9 — M2: bindings on macOS — compiled-function mode, $(DR_CLIENT) everywhere  (M, depends on: T5)

**Goal.** The per-language drtrace lanes resolve platform-correct library
names on Darwin and the documented macOS binding path is
compiled-function/symbol mode.

**Steps.**
1. In [mk/native-trace.mk](../../../mk/native-trace.mk), fix `drtrace_env`
   (lines 2355-2358): `ASMTEST_DRAPP_LIB=$(abspath $(call shlib_dev,libasmtest_drapp))`
   (the `shlib_dev` macro at [Makefile](../../../Makefile) line 226 already
   yields `.dylib` on Darwin), `ASMTEST_DRCLIENT=$(abspath $(DR_CLIENT))`,
   and a platform loader-path var
   (`DYLD_LIBRARY_PATH` on Darwin, `LD_LIBRARY_PATH` otherwise — mirror how
   the bindings lanes already handle this elsewhere in the file, or gate on
   `$(UNAME_S)`).
2. Same fix inline in `drtrace-python-test` (lines 2335-2338), which repeats
   the hardcoded `.so` names.
3. Verify each binding wrapper reads paths only from those env vars (they do —
   that is the documented contract of `drtrace_env`); no per-binding source
   changes are expected. Where a wrapper hardcodes `.so`, fix it to read the
   env var.
4. Run the dlopen-only lanes that need no extra toolchain on the Mac:
   `make drtrace-cpp-test drtrace-ruby-test DYNAMORIO_HOME=...` (mirroring the
   toolchain choice of ci.yml's macOS hwtrace step, line 204).

**Code.** Makefile edits; possibly small wrapper fixes.

**Tests.** On macOS x86-64 + DR: the cpp and ruby drtrace lanes print their
TAP/skip output green. On Linux: `make docker-drtrace-bindings` unchanged.
Note the existing `drtrace_run` downgrade (lines 2372-2377) already converts
DR's "Failed to take over all threads" on JIT/GC runtimes into a SKIP — keep
it; multi-threaded managed runtimes are best-effort by design.

**Docs.** [native-tracing.md](../../guides/tracing/native-tracing.md): the
macOS bindings paragraph (compiled-function/symbol mode is the supported
path on both Mac arches; generated code from bindings only where the
interpreter is non-hardened, and never promised). Changelog: same entry.

**Done when.**
- `make drtrace-cpp-test drtrace-ruby-test DYNAMORIO_HOME=...` green on macOS
  x86-64.
- `grep -n 'libasmtest_drclient\.so' mk/native-trace.mk` hits only the
  Linux-container (`docker-*`) context and comments.
- Linux bindings lanes unchanged-green (`make docker-drtrace-bindings`).

### T10 — M2: finish -ldl removal and the skip-message arch fix  (S, depends on: T4)

**Goal.** Every DR-tier link line honors `$(DRTRACE_LDFLAGS)`, every DR skip
message names `DynamoRIO-$(UNAME_S)-<ver>`, and the `docker-drtrace*` Linux
scoping is stated in a comment.

**Steps.** All in [mk/native-trace.mk](../../../mk/native-trace.mk):
1. Replace the literal `-rdynamic ... -ldl -lpthread` / `-ldl -lpthread` on
   the remaining DR-tier link rules — lines 118 (`shared-drtrace`), 196, 206
   (`dr_valtrace`), 272, 282 (`dr_taint`), 355, 365 (`dr_taint_simd`), 538
   (`taint_attach_coop`; keep its `-lrt`, it is a Linux-only lane) — with
   `$(DRTRACE_LDFLAGS)` (plus `-lrt` where present today). Note the shared-lib
   rule at 118 has no `-rdynamic` today; give it a second variable
   (`DRTRACE_SOLDFLAGS := -lpthread` / `-ldl -lpthread`) rather than adding
   `-rdynamic` to a dylib link. Leave the hwtrace-tier `-ldl` sites (lines
   2059+) alone — they belong to a different tier and sibling docs. Leave the
   `dr_valtrace_bench` bench link rule (line 1823) alone too: macOS benchmarks
   are out of scope (see Out of scope), and its unconditional `-ldl` links
   cleanly on macOS anyway (Apple clang provides a libdl stub).
2. `sed -i '' 's/DynamoRIO-Linux-<ver>/DynamoRIO-$(UNAME_S)-<ver>/g' mk/native-trace.mk`
   — 32 sites today; also the two copies in the `drtrace_skip` define (line
   2363) and `drtrace-python-test` (line 2330), which that same sed catches.
   Do NOT touch the `DynamoRIO-Linux-${DR_VERSION}` **download URLs** in
   [Dockerfile.drtrace](../../../Dockerfile.drtrace) and
   [ci.yml](../../../.github/workflows/ci.yml) — those name real Linux
   artifacts.
3. Comment above the `docker-drtrace` references and the `DRCLIENT_EXT`
   conditional: "docker-drtrace* lanes run Linux containers regardless of
   host; their paths are intentionally `.so`."

**Code.** Makefile only.

**Tests.** `make drtrace-test` on a DR-less Linux box prints
`Set DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>` (UNAME_S expands to
Linux); the same command on a Mac prints `DynamoRIO-Darwin-<ver>`.
`make docker-drtrace` green (link lines unchanged in effect on Linux).

**Docs.** Internal-only; cosmetic/cleanup (no changelog — not user-visible
behavior).

**Done when.**
- `grep -c 'DynamoRIO-Linux-<ver>' mk/native-trace.mk` → 0.
- `grep -n '\-ldl' mk/native-trace.mk` shows no unconditional `-ldl` on the
  enumerated DR-tier link rules (the hwtrace sites and the out-of-scope
  `dr_valtrace_bench` bench rule at line 1823 remain by design).
- `make docker-drtrace` and `make drtrace-test` (skip path) both behave as
  today on Linux.

### T11 — M2: the `drtrace-macos` CI job on macos-15-intel  (S, depends on: T5, T6)

**Goal.** A separate nightly CI job runs the macOS DR lane on an Intel runner
with a pinned DR release, without ever blocking the Unicorn tier.

**Steps.**
1. Add to [.github/workflows/ci.yml](../../../.github/workflows/ci.yml),
   mirroring the Linux `drtrace` job (lines 336-359) for the pin/fetch shape
   and `test-macos-x86` (lines 143-147) for the nightly gating — Intel macOS
   runners are scarce/slow, which is why that job is schedule/dispatch-gated;
   follow it:
   ```yaml
   drtrace-macos:
     name: drtrace (macOS x86-64, nightly)
     if: github.event_name == 'schedule' || github.event_name == 'workflow_dispatch'
     runs-on: macos-15-intel        # NOT macos-13: image retired 2025-12-08 (see the comment at ci.yml:139-142)
     timeout-minutes: 20
     env:
       DR_MACOS_VERSION: <T2 tag>   # pin like the Linux job's DR_VERSION
     steps:
       - uses: actions/checkout@v7
       - name: Fetch the pinned DynamoRIO macOS release
         run: |
           url="https://github.com/DynamoRIO/dynamorio/releases/download/<T2 tag>/<T2 asset name>"
           curl -fsSL "$url" -o /tmp/dr.tar.gz
           echo "<T2 sha256>  /tmp/dr.tar.gz" | shasum -a 256 -c -
           mkdir -p "$HOME/dynamorio"
           tar -xzf /tmp/dr.tar.gz -C "$HOME/dynamorio" --strip-components=1
       - name: Build + run the macOS DR lane
         run: make drtrace-test-macos DYNAMORIO_HOME="$HOME/dynamorio"
   ```
   `cmake` (needed for the client build) is preinstalled on the
   `macos-15-intel` image; no brew install step is required.
2. Do NOT fold this into the `test` matrix: a DR failure must never block the
   Unicorn tier (the plan's M2 requirement; also why Linux `drtrace` is its
   own job).
3. An arm64 (`macos-latest`) leg is deferred: it needs T8 plus an arm64 DR
   binary, and the hosted-runner signing constraint likely bites — that
   follow-up belongs to [self-hosted-ci-runners.md](self-hosted-ci-runners.md).

**Code.** Workflow YAML only.

**Tests.** `workflow_dispatch` the workflow once: the new job appears, fetches
the pinned tarball (digest-checked), and runs the lane green. The nightly
cron (`0 7 * * *`, ci.yml line 10) then covers it daily.

**Docs.** Changelog `### Added` (the macOS DR CI lane is user-visible
project surface); note in [native-tracing.md](../../guides/tracing/native-tracing.md)
that macOS x86-64 DR is CI-covered nightly.

**Done when.**
- A dispatched run shows `drtrace (macOS x86-64, nightly)` green.
- The job pins version + SHA-256 (no floating `latest`).
- Pushing an unrelated commit does not run the job (nightly/dispatch only).

## Task order & parallelism

```
T1 (recheck; the ONLY task runnable today)
 └─ gate opens ─ T2 (inspect tarball)
                  ├─ T3 (C dylib paths)   ─┐
                  ├─ T4 (make wiring)     ─┼─ T5 (M0 go/no-go) ── T6 (M1a Intel + Rosetta) ── T11 (CI)
                  │                        │        ├─ T7 (M1b helpers) ── T8 (M1b arm64; extra gates)
                  └────────────────────────┘        └─ T9 (bindings)
                          T4 ── T10 (cleanup; independent of T5)
```

- T3 and T4 are independent of each other (two people can take them
  concurrently after T2).
- T5 is the critical path: T6, T7, T9 all hang off it; nothing past T5
  proceeds on a no-go.
- T10 needs only T4 and can run in parallel with T5-T9.
- T8 carries two extra gates (Apple Silicon hardware; an arm64/universal DR
  binary) and must not block T9/T11 — the plan ships Intel-only value
  without it.

## Constraints & gates

- **The upstream *release* gate was real — now retired (2026-07-22).** As
  originally written, CLAUDE.md's rule (a missing installable dependency gets
  pinned, never self-skipped) could not apply for two stacked reasons: (1) the
  dependency **did not exist anywhere** — DynamoRIO has never published a macOS
  *release asset* (~430 releases, 2015-2026; re-verified live through
  2026-07-20), so there was nothing to pin; (2) even when it exists, a Darwin DR
  runtime cannot run in any Docker lane — containers are Linux — so the lane's
  substrate is macOS hosts/runners (the rule's hardware exemption).
  **Reason (1) no longer holds:** a *source fork* now provides a pinnable
  build (`wilvk/dynamorio` → `scripts/build-dynamorio-macos.sh`, per
  [macos-dynamorio-fork-build.md](macos-dynamorio-fork-build.md)), so CLAUDE.md's
  pin-the-dependency rule **does** apply — the pin is the fork commit + submodule
  SHAs. Reason (2) still holds (macOS host substrate, not Docker). The recorded
  gate is now the fork pin (not a release recheck); the self-skips
  (`drtrace-test-macos` off Darwin / off DR) still print their reason.
- ~~**A from-source macOS DynamoRIO build is explicitly out of scope**~~ **—
  AMENDED 2026-07-22.** This was written when no macOS DynamoRIO binary could be
  obtained by any means. A source fork now exists and builds
  `libdynamorio.dylib` on macOS x86-64, so the from-source path is the
  unblocking avenue after all — it is now scoped in
  [macos-dynamorio-fork-build.md](macos-dynamorio-fork-build.md) (FB1 pins the
  fork + adds `scripts/build-dynamorio-macos.sh`; FB2 fixes the `dr_app_setup`
  startup crash). The original caution still holds in spirit: upstream macOS is
  "in progress" and x86-64-CI-only, and the `dr_app_*` embedding path is the one
  to prove first (which is exactly FB2's job — a real startup crash was found on
  the dev host). It is no longer *out of scope*; it is the plan.
- **Hardware gates**: M0/M1a-native need a macOS x86-64 host (the current dev
  host is Intel-Mac x86_64 — present, not gating). The Rosetta leg of T6 and
  all of T8 need Apple Silicon. T8 additionally needs an arm64/universal
  macOS DR binary — a second upstream event on top of the main gate. Record
  each as "not run — <gate>" rather than inferring results.
- **Pinning**: the (future) macOS DR release is pinned by tag + SHA-256 in CI
  (T11), exactly like `DR_VERSION=11.91.20630` in
  [Dockerfile.drtrace](../../../Dockerfile.drtrace) and ci.yml's Linux job.
- **Signing**: only ad-hoc (`codesign -s -`), only on the standalone test
  executable, never `--options runtime` (hardened runtime imposes the
  one-MAP_JIT-region limit that our multi-allocation tests violate), never on
  dylibs (entitlements bind to the main executable).
- **CI runner**: every macOS Intel leg uses `macos-15-intel`, never
  `macos-13` (retired 2025-12-08) — binding doc-set position; the plan's M2
  YAML sketch predates this and is superseded here.
- **Attach model**: this tier's macOS port is the cooperative in-process
  `dr_app_*` path. On Linux, DR *external* attach is already wired for the
  taint/dataflow tier (`Dockerfile.taint-attach`, `docker-dataflow-attach`,
  landed 2026-07-14); only the drtrace control-flow tier is
  `dr_app_*`-cooperative, and that is the surface being ported.

## Research notes (verified 2026-07-17)

**DynamoRIO macOS release status.**
- Zero macOS/Darwin/OSX assets across the entire GitHub releases history
  (~430 releases, release_4_1_0 2015 → cronbuild-11.91.20644 2026-07-11);
  weekly cronbuilds ship exactly AArch64-Linux, ARM-Linux-EABIHF, Linux,
  Windows. Re-verified live while authoring: `gh api --paginate ... | grep -icE
  'mac|darwin|osx'` → 0. Source: https://api.github.com/repos/DynamoRIO/dynamorio/releases ,
  https://dynamorio.org/page_releases.html
- Upstream README still says "Mac OSX support is in progress"; the build docs
  carry no macOS instructions at all.
  https://raw.githubusercontent.com/DynamoRIO/dynamorio/master/README.md ,
  https://dynamorio.org/page_building.html
- Expected tarball layout `lib64/release/libdynamorio.dylib` is **inferred,
  not observed** (no macOS artifact has ever existed to inspect): DynamoRIO's
  CMake install layout is platform-independent (`INSTALL_LIB_X64=lib64`,
  `${INSTALL_LIB_BASE}/debug|/release`, no APPLE override) — T2 must confirm
  against the real tarball. https://raw.githubusercontent.com/DynamoRIO/dynamorio/master/CMakeLists.txt
- Single-arch (x86-64) expectation: the only macOS CI job is `osx-x86-64` on
  `macos-15-intel` (updated by PR #7744); macOS arm64 is a separate open port
  (i#5383, open since 2022, with blocker #7296 on client reachability); no
  universal-binary support exists anywhere in code or tracker.
  https://raw.githubusercontent.com/DynamoRIO/dynamorio/master/.github/workflows/ci-osx.yml ,
  https://github.com/DynamoRIO/dynamorio/issues/5383 ,
  https://github.com/DynamoRIO/dynamorio/issues/7296 ,
  https://github.com/DynamoRIO/dynamorio/pull/7744
- Load-bearing open issues on the `dr_app_*`/embedding path for macOS:
  #1997 (dynamorio_static on Mac, "Bailing on Mac support for now"),
  #1979 (64-bit Mac OSX support), #1285 (private loader), #1290 (ptrace
  injection), #1568 (TLS 64-bit). `dr_app.h` itself carries no macOS caveat.
  https://github.com/DynamoRIO/dynamorio/issues/1997 ,
  https://github.com/DynamoRIO/dynamorio/issues/1979 ,
  https://github.com/DynamoRIO/dynamorio/issues/1285 ,
  https://github.com/DynamoRIO/dynamorio/issues/1290 ,
  https://github.com/DynamoRIO/dynamorio/issues/1568 ,
  https://raw.githubusercontent.com/DynamoRIO/dynamorio/master/core/lib/dr_app.h

**macOS W^X / MAP_JIT / entitlements (T6-T8).**
- Apple silicon forbids simultaneous W+X pages for **all** processes,
  hardened or not; `MAP_JIT` regions are never writable and executable at
  once, and the RW/RX view is toggled **per-thread** via
  `pthread_jit_write_protect_np` — never `mprotect` on the JIT region.
  https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon ,
  https://keith.github.io/xcode-man-pages/pthread_jit_write_protect_np.3.html ,
  https://github.com/zherczeg/sljit/issues/99
- `com.apple.security.cs.allow-jit` is required **only under the hardened
  runtime**; a non-hardened (e.g. ad-hoc-signed) process may use `MAP_JIT`
  without it. Under hardened runtime + allow-jit, only ONE MAP_JIT region may
  be created — the reason T8 forbids `--options runtime`.
  https://developer.apple.com/documentation/BundleResources/Entitlements/com.apple.security.cs.allow-jit ,
  https://developer.apple.com/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon
- Rosetta 2 supports legacy x86-64 RWX/mprotect-style JIT by transparently
  toggling pages RW/RX and catching the Mach fault when the page is in the
  wrong state (Apple DTS description; forum/issue sources, not formal docs —
  hence T6's must-verify stance). Translated x86-64 code may run unsigned,
  "with precisely the same limitations that native unsigned code executing on
  an Intel-based Mac". https://developer.apple.com/forums/thread/672804 ,
  https://support.apple.com/guide/security/rosetta-2-on-a-mac-with-apple-silicon-secebb113be1/web
- Homebrew re-signs poured bottles **ad-hoc** (`codesign -s -`), not
  hardened — so Homebrew interpreters can MAP_JIT without allow-jit, while
  Apple-signed system interpreters are hardened and cannot be re-signed
  (caveat: maintainer statement, not a per-binary codesign audit).
  https://github.com/Homebrew/brew/issues/9082

**GitHub runners (T11).**
- `macos-15-intel` = macOS 15 x64, 4 vCPU/14 GB, introduced 2025-09-19,
  supported through Aug 2027, actively maintained (image release
  macos-15/20260715.0340 of 2026-07-15: macOS 15.7.7, CMake 3.31.5/4.1.2
  preinstalled — the DR client build needs no install step). `macos-latest`
  is arm64 (macos-15, M1); `macos-26-intel` also exists as an x64 successor.
  https://github.com/actions/runner-images ,
  https://github.com/actions/runner-images/issues/13045 ,
  https://github.com/actions/runner-images/issues/12520 ,
  https://raw.githubusercontent.com/actions/runner-images/main/images/macos/macos-15-Readme.md

## Out of scope

- **Docker-OSX / KVM macOS cleanroom lanes and any containerized-macOS
  validation** — [macos-cleanroom-lanes.md](macos-cleanroom-lanes.md) (note
  its KVM lane is hardware-gated on a bare-metal Linux `/dev/kvm` host).
- **Out-of-process Mach tracing on macOS** (task_for_pid, the
  `com.apple.security.cs.debugger` entitlement, thread_get_state stepping) —
  [macos-oop-mach-stepper.md](macos-oop-mach-stepper.md); this doc's tier is
  in-process only.
- **A self-hosted arm64 macOS runner with a real signing identity** for a
  future arm64 CI leg — [self-hosted-ci-runners.md](self-hosted-ci-runners.md).
- **Benchmark CI additions on macOS runners** —
  [benchmarks-ci-followups.md](benchmarks-ci-followups.md).
- **Shipping/packaging the macOS artifacts** (wheels/gems bundling a DR
  payload) — [distribution-packaging.md](distribution-packaging.md).
- **The Linux DR taint/dataflow external-attach tier** — landed on Linux and
  not part of this port; see the data-flow implementation docs (e.g.
  [dataflow-producer-correctness.md](dataflow-producer-correctness.md)).
- **A from-source macOS DynamoRIO build** — no sibling doc; explicitly
  unscoped by the plan (see Constraints & gates).
