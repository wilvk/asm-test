# macOS DynamoRIO from a pinned source fork — implementation

> **Provenance.** Authored 2026-07-22 after a live feasibility spike on the
> macOS-14.7.5 / Intel Core i7-8559U dev host. This document **supersedes the
> upstream-release gate** that held
> [macos-dynamorio-port.md](macos-dynamorio-port.md) (and its source plan
> [macos-drtrace-plan.md](../plans/macos-drtrace-plan.md)) at "BLOCKED UPSTREAM".
> It does not replace that doc — it *unblocks* it: FB1–FB4 below produce and
> validate a `libdynamorio.dylib`, then hand off to that doc's T3–T11 for the
> asm-test-side wiring (Makefile paths, bindings, cleanup, CI job body).

## Why this work now exists (the gate changed)

[macos-dynamorio-port.md](macos-dynamorio-port.md) is gated on one thing:
**"DynamoRIO has never published a macOS release asset, so there is nothing to
attach with."** That premise had a stacked corollary in the plan — *"a
from-source macOS build is out of scope"* — written when no macOS DynamoRIO
binary could be obtained by any means.

A **local source fork now exists**:
`/Users/willemvanketwich/source/dynamorio` (remote
`https://github.com/wilvk/dynamorio`), tracking upstream master. That dissolves
the release-asset gate: we build the runtime ourselves. And it re-scopes the
plan's "out of scope" line — the CLAUDE.md dependency rule (*"a missing
installable dependency is added where the work runs, and pinned"*) now applies,
because there is finally a pinnable source to build. The build produces a macOS
Mach-O `libdynamorio.dylib`, which no Docker/Linux lane can run — so the lane's
substrate is the macOS host itself, under the rule's hardware exemption, exactly
like the native single-step tier.

## What was verified on 2026-07-22 (host: macOS 14.7.5, Intel i7-8559U, Apple clang 16)

These are measured facts from the spike, not inferences — they are why FB1/FB2
are the right first tasks and why the port is judged feasible:

1. **DynamoRIO builds from the fork on macOS x86-64.** From fork HEAD
   `cca42665b` (upstream master, no local changes), with submodules initialised
   (`git submodule update --init --depth 1` → `third_party/elfutils`, `libipt`,
   `zlib`):
   ```sh
   cmake -DDEBUG=OFF -DBUILD_TESTS=ON <fork>     # CMake 4.3.4, no policy shim
   cmake --build . --target dynamorio -j$(sysctl -n hw.ncpu)
   ```
   links `lib64/release/libdynamorio.dylib`. Host deps: `nasm`, `lz4` (present),
   SDK `zlib`; Intel-PT third-party libs skip cleanly (`PT related libraries
   only supported on Linux x86_64`). The benign linker notes (`ignoring -e`, `no
   platform load command … assuming: macOS` on the hand-written `.asm` objects)
   do not affect the link.
2. **The install layout is exactly `lib64/release/libdynamorio.dylib`** — the
   path [macos-dynamorio-port.md](macos-dynamorio-port.md) T2/T3/T4 only
   *assumed* ("inferred, not observed"). **Now confirmed.** No `APPLE` override
   in DynamoRIO's CMake install rules; `DR_LIBSUBDIR` stays `lib64/release`.
3. **The tree has real, current Mach-O support** — `core/unix/loader_macos.c`,
   `module_macho.c`, `signal_macos.c`, `tls_macos_x86.c`, `native_macho.c`,
   `memquery_macos.c`, `ksynch_macos.c` all compile. This is not the abandoned
   2016 state the port doc's Research notes cite (#1997 "Bailing on Mac support
   for now").
4. **Upstream CI-gates the exact interface this tier needs.** The fork's
   `.github/workflows/ci-osx.yml` runs `./suite/runsuite_wrapper.pl automated_ci
   64_only` on `macos-15-intel` on **every push/PR**. In
   `suite/runsuite_wrapper.pl`, the `$is_macos` 64-bit ignore-failures list does
   **not** contain `api.startstop` or `api.detach` — the
   `dr_app_setup/start/stop_and_cleanup` cooperative-attach path — whereas the
   Windows list *does* ignore `api.startstop` (i#2246). So upstream **expects
   that path to pass on macOS x86-64** and blocks merges on it.
5. **One concrete startup crash on this host — the M0 go/no-go, and FB2's job.**
   `api.startstop`, linked against the freshly-built dylib, faults at
   `dr_app_setup` startup: `EXC_BAD_ACCESS (address=0x8)` in
   `get_application_name_helper` at `core/unix/os.c:1192`, on the
   `config_read → get_application_short_name` path. That macOS branch reads the
   executable path by walking `our_environ` to its NULL terminator and
   dereferencing the slot **above** `envp` (the XNU exec-path convention); it
   faults when that slot is not a readable string. It is **stack-layout
   dependent** (observed one clean 10-iteration attach/detach run, then
   consistent crashes), which is consistent with upstream's controlled macos-15
   CI passing it while a standalone `dr_app_*` embedding launch here does not.
   The functional path is sound (when it did run, all 10 attach/detach
   iterations fired the client's `event_post_attach`/`event_pre_detach`
   callbacks and matched `startstop.expect` line-for-line); the fault is in
   exec-path resolution, not attach.

## Relationship to macos-dynamorio-port.md

- **Replaces** that doc's **T1** (standing upstream release recheck — no longer
  the gate) and **T2** (inspect a release tarball — there is none; FB1 builds
  from source and FB2 records the layout, already confirmed above).
- **Feeds** that doc's **T3/T4/T5** (dylib-name resolution, Make wiring +
  `drtrace-test-macos`, the M0 compiled-function harness): FB3 provides the
  built `DYNAMORIO_HOME` those tasks consume, and FB2 must be green first (a DR
  runtime that crashes at `dr_app_setup` cannot trace anything).
- **Leaves intact** that doc's **T6–T10** (Rosetta verdict, `MAP_JIT`/arm64
  helpers + entitlement, bindings, `-ldl`/skip-message cleanup) — all still
  apply unchanged; the arm64/Rosetta legs keep their Apple-Silicon hardware
  gates.
- **Revises** that doc's **T11** (CI job): the nightly `drtrace-macos` lane
  builds the *pinned fork from source* on `macos-15-intel` (FB4) instead of
  fetching a release tarball that does not exist.

## Tasks

### FB1 — Pin the fork and add a reproducible macOS source-build  (M, depends on: none)

**Goal.** A pinned, one-command build produces
`$(DYNAMORIO_HOME)/lib64/release/libdynamorio.dylib` on a macOS x86-64 host from
a fixed fork commit — the pinned-source form of the CLAUDE.md dependency rule
(mirrors [scripts/build-capstone.sh](../../../scripts/build-capstone.sh)).

**Steps.**
1. Record the pin. Add a `dynamorio-fork` row to
   [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt)
   naming the fork remote (`https://github.com/wilvk/dynamorio`) and the exact
   commit (start from `cca42665b`; FB2 advances it to the fix commit). Git
   commit is the immutable pin here (as the Pin/libdft docs pin git commits, not
   tarball digests); add the emitter line to
   [scripts/refresh-thirdparty-digests.sh](../../../scripts/refresh-thirdparty-digests.sh)
   and vendor the DynamoRIO `License.txt` under
   [licenses/](../../../licenses/) (BSD-3 + subcomponents).
2. Add `scripts/build-dynamorio-macos.sh`, patterned on `build-capstone.sh`:
   clone-or-reuse the pinned fork commit into a cache dir, `git submodule update
   --init --depth 1`, `cmake -DDEBUG=OFF -DBUILD_TESTS=OFF`, `cmake --build .
   --target dynamorio`, then stage a minimal `DYNAMORIO_HOME` (`lib64/release/`,
   `cmake/`, `include/`, `ext/`) into a caller-named prefix. Echo the resolved
   `DYNAMORIO_HOME`. Arch-gate to x86-64 macOS; on any other host print the
   Darwin-only / arch skip and exit 0 (the tier self-skips, it never hard-fails
   a Linux build).
3. Wire a `make dynamorio-macos` convenience target (in
   [mk/native-trace.mk](../../../mk/native-trace.mk)) that runs the script and
   prints the `DYNAMORIO_HOME` to export, plus a `make help` line.

**Code.** One new script; one manifest row + emitter; one Make target; a
vendored license dir. No C.

**Tests.** On this Intel-mac host: `scripts/build-dynamorio-macos.sh <prefix>`
exits 0 and leaves `<prefix>/lib64/release/libdynamorio.dylib` (verify with
`lipo -info` → `x86_64`). On Linux: the arch gate prints the skip and exits 0.
`make docker-drtrace` unaffected (Linux lane untouched).

**Done when.**
- `scripts/build-dynamorio-macos.sh` builds the pinned fork to
  `lib64/release/libdynamorio.dylib` on macOS x86-64, reproducibly.
- The pin (remote + commit) is recorded in the manifest and re-emitted by the
  refresh script; the license is vendored.
- The script self-skips (exit 0) off macOS-x86-64.

### FB2 — Fix the dr_app_setup startup crash; prove api.startstop passes reliably  (M, depends on: FB1) — **the M0 go/no-go**

**Goal.** `api.startstop` (and `api.detach`), linked against the fork's
`libdynamorio.dylib`, pass reliably in the standalone `dr_app_*` embedding model
on macOS x86-64 — no `get_application_name_helper` fault. This is the gate: a DR
runtime that crashes at `dr_app_setup` cannot trace, so nothing in
[macos-dynamorio-port.md](macos-dynamorio-port.md) proceeds until it is green.

**Steps.**
1. Reproduce in the fork: build `--target api.startstop runstats`
   (`-DBUILD_TESTS=ON`), run `ctest -R 'api\.startstop'`. Confirm the fault at
   `core/unix/os.c:1192` (`get_application_name_helper`, address 0x8) and its
   intermittency (re-run several times; it is stack-layout dependent).
2. Root-cause the exec-path read. The macOS branch derefs the slot above `envp`
   via `our_environ`; it is fragile when `our_environ` is not the true
   kernel-provided stack environment in the embedding (non-`drrun`) launch. Two
   candidate fixes to evaluate in the fork:
   - Make the read defensive: bounds/NULL-check the post-`envp` slot and the
     `EXECUTABLE_KEY` compare before deref; fall back rather than fault.
   - Prefer a robust source for the exec path on macOS
     (`_NSGetExecutablePath`) when the above-`envp` slot is not present, and/or
     ensure `our_environ` is captured from the app's real `environ` during
     `dr_app_setup`.
   Keep the change minimal and macOS-scoped; it is an upstream-shaped fix — open
   it against `wilvk/dynamorio` (and consider an upstream PR).
3. Advance the FB1 pin to the fix commit.

**Code.** A small, macOS-guarded change in `core/unix/os.c` (and possibly the
`dr_app_setup` init path) **in the fork**, not in asm-test.

**Tests.** In the fork: `ctest -R 'api\.startstop'` and `ctest -R 'api\.detach'`
pass, **stable across ≥5 consecutive runs** (the intermittency is the bug — a
single green run is not sufficient). Record the before/after in this doc.

**Done when.**
- `api.startstop` + `api.detach` pass reliably (≥5/5) against the fork dylib on
  this Intel-mac host, in the standalone embedding model.
- The fix is committed to the fork and the FB1 pin points at it.
- If the crash proves to be a genuine upstream macOS defect, that is recorded
  (with the issue/PR link) as the substantive M0 finding.

### FB3 — Hand off to the M0 harness: DYNAMORIO_HOME → the asm-test drtrace tier  (M, depends on: FB2)

**Goal.** With a working fork dylib, drive
[macos-dynamorio-port.md](macos-dynamorio-port.md) T3→T5: a normally-compiled
function is traced end-to-end (attach, marker resolution, coverage, clean
shutdown) through asm-test's `dr_app_*` app/client, on macOS x86-64.

**Steps.**
1. Land port-doc **T3** (dylib-aware `libdynamorio` resolution in
   `src/drtrace_app.c`) and **T4** (Darwin `.dylib`/`-ldl` Make wiring +
   `drtrace-test-macos`) — both already fully specified there; nothing new here.
2. Point `DYNAMORIO_HOME` at FB1's output and run port-doc **T5**'s M0
   harness: `make drtrace-test-macos DYNAMORIO_HOME=<FB1 prefix>`. The
   load-bearing asm-test-specific unknown port-doc T5 names is whether
   `dr_get_proc_address` resolves exported markers in a **Mach-O** main
   executable — FB2 clears the DR-runtime crash so this can finally be observed.
3. Record the M0 verdict (GO / no-go + root cause) in
   [macos-drtrace-plan.md](../plans/macos-drtrace-plan.md).

**Code.** Per port-doc T3/T4 (asm-test side); no new files here beyond T5's
`examples/test_drtrace_macos.c`.

**Tests.** `make drtrace-test-macos DYNAMORIO_HOME=<FB1 prefix>` prints all-`ok`
TAP and exits 0 (twice in a row), covering both compiled-function mode and
symbol mode. `make docker-drtrace` still green (Linux unchanged).

**Done when.**
- The M0 compiled-function/symbol harness traces `add2`/`asmtest_symbol_demo`
  green on macOS x86-64 through the fork runtime.
- Port-doc T3/T4/T5 "Done when" are all met; the plan records M0 = GO (or the
  documented marker-resolution no-go).

### FB4 — Nightly drtrace-macos CI: build the pinned fork, run the lane  (S, depends on: FB3)

**Goal.** A separate nightly CI job builds the pinned fork on `macos-15-intel`
and runs the macOS DR lane, never blocking the Unicorn tier — the fork-build
form of [macos-dynamorio-port.md](macos-dynamorio-port.md) T11.

**Steps.**
1. Add a `drtrace-macos` job to [.github/workflows/ci.yml](../../../.github/workflows/ci.yml),
   mirroring `test-macos-x86` (schedule/`workflow_dispatch` gated,
   `runs-on: macos-15-intel`, **never** `macos-13`). Instead of fetching a
   release tarball, it: `brew install nasm lz4`, then
   `scripts/build-dynamorio-macos.sh "$HOME/dr"` (checks out + builds the pinned
   fork commit), then `make drtrace-test-macos DYNAMORIO_HOME="$HOME/dr"`.
2. Keep it out of the `test` matrix so a DR failure never blocks the emulator
   tier (the plan's M2 requirement). The fork commit is the pin (no floating
   `master`); the build is reproducible from FB1.
3. Defer the arm64 (`macos-latest`) leg — it needs port-doc T7/T8 (MAP_JIT +
   entitlement) **and** an arm64 DR build (upstream arm64 macOS is open, i#5383);
   that follow-up belongs to
   [self-hosted-ci-runners.md](self-hosted-ci-runners.md).

**Code.** Workflow YAML only.

**Tests.** `workflow_dispatch` the workflow once: the new job builds the pinned
fork and runs the lane green. An unrelated push does not trigger it (nightly /
dispatch only). Local `actionlint` clean.

**Docs.** Add the macOS DR row to the backend table in
[docs/guides/tracing/native-tracing.md](../../guides/tracing/native-tracing.md)
and a `### Added` bullet under `## [Unreleased]` in
[CHANGELOG.md](../../../CHANGELOG.md) (the macOS DR tier is user-visible).

**Done when.**
- A dispatched run shows `drtrace (macOS x86-64, nightly)` green, building the
  pinned fork from source.
- The job pins the fork commit (no floating branch) and is nightly/dispatch only.

## Task order & parallelism

```
FB1 (pin + build script)
 └─ FB2 (fix os.c:1192 crash — the M0 GO/NO-GO gate)
     └─ FB3 (drive port-doc T3/T4/T5 — the M0 harness)
         └─ FB4 (nightly CI: build pinned fork on macos-15-intel)
```

- FB1→FB2→FB3→FB4 is a strict chain: a DR runtime that crashes at
  `dr_app_setup` (FB2) cannot trace, so FB3 cannot observe marker resolution
  before FB2 is green.
- Port-doc **T3/T4** (asm-test dylib-name + Make wiring) can be authored in
  parallel with FB1/FB2 — they touch only asm-test files and are validated by
  FB3. Port-doc **T10** (cleanup) needs only T4 and runs anytime after.
- Port-doc **T6/T8** (Rosetta, arm64 MAP_JIT + entitlement) keep their
  Apple-Silicon hardware gates and are out of this doc's Intel-first chain.

## Constraints & gates

- **The upstream-release gate is retired for this tier.** The old go/no-go
  (`gh api … releases … grep -i mac`) no longer governs: we build from the
  pinned fork. Record the fork commit + submodule pins instead. (The recheck may
  still be logged in the plan for the day upstream *does* ship a binary and a
  release-tarball path becomes an alternative — but it is no longer blocking.)
- **Hardware gates unchanged for the arm64/Rosetta legs.** M0 + native-Intel M1a
  need only a macOS x86-64 host (present — this dev box). The Rosetta verdict
  (port-doc T6) and all arm64 `MAP_JIT`/entitlement work (port-doc T7/T8) still
  need Apple Silicon, and arm64 additionally needs an arm64 DR build (upstream
  arm64 macOS is an open port, i#5383). Record each gated leg as "not run —
  <gate>".
- **The fix lives in the fork, not asm-test (FB2).** asm-test consumes a
  DynamoRIO runtime; it does not carry DR source patches. Keep the exec-path fix
  macOS-scoped and upstream-shaped.
- **No untested hardware code (CLAUDE.md).** FB2 must prove `api.startstop`
  reliably green (≥5/5) before FB3 trusts the runtime; a single intermittent
  pass is not acceptance.
- **CI runner**: every macOS Intel leg uses `macos-15-intel`, never `macos-13`
  (retired 2025-12-08) — binding doc-set position.

## Out of scope

- **The asm-test-side Make/binding wiring beyond the hand-off** — owned by
  [macos-dynamorio-port.md](macos-dynamorio-port.md) T3/T4/T6–T10; this doc only
  produces the runtime (FB1/FB2) and proves M0 (FB3) + CI (FB4).
- **arm64 / Apple Silicon and Rosetta** — port-doc T6/T7/T8 and
  [self-hosted-ci-runners.md](self-hosted-ci-runners.md).
- **A from-source DynamoRIO build inside Docker** — impossible for a macOS
  runtime (containers are Linux); the substrate is macOS hosts/runners.
- **Out-of-process Mach tracing** — [macos-oop-mach-stepper.md](macos-oop-mach-stepper.md);
  this tier is in-process `dr_app_*` only.
