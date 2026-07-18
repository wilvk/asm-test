# AArch64 single-step ptrace stream validation and binding fixtures on real silicon — implementation

> **Sources.** Actioned from the two remaining AArch64 items (SS-A64-STREAM,
> SS-A64-BIND) of
> [zen2-singlestep-trace-plan.md](../plans/zen2-singlestep-trace-plan.md)
> (Phase 5, "The one genuinely remaining Phase-5 front"). Written 2026-07-17.
> If this doc and a source disagree, this doc wins (sources may be stale); if
> the CODE and this doc disagree, re-verify before implementing.

## Why this work exists

The out-of-process AArch64 ptrace tracer is fully written and its fixtures are
decode- and execute-validated — but the live capture itself has **never run**,
because qemu-user cannot emulate the ptrace tracer/tracee relationship and no
dev box is AArch64. Externally-verified research (see Research notes) shows the
gate is now closable **without buying hardware**: GitHub's `ubuntu-24.04-arm`
hosted runners are real Neoverse silicon VMs on which full ptrace
tracer/tracee workloads (strace's and CRIU's own CI) demonstrably pass. This
doc wires that runner in as the live-validation lane, settles the one genuinely
undetermined question (do hardware breakpoints *fire* under Azure's
hypervisor?) with a decisive probe, records the first arm64
`benchmarks/boxes/` entry, and gives all ten language bindings live AArch64
fixtures so their ptrace tests stop being x86-64-fixture-only.

## What already exists (verified 2026-07-17)

The AArch64 tracer and everything around it is landed; only the live stream is
unproven. Key files:

- [src/ptrace_backend.c](../../../src/ptrace_backend.c) — the out-of-process
  stepper, compiled for `__linux__ && (__x86_64__ || __aarch64__)` (line 295).
  The AArch64 arm reads PC/x0 via `PTRACE_GETREGSET`/`NT_PRSTATUS` (lines
  445–458), arms hardware execution breakpoints through the `NT_ARM_HW_BREAK`
  regset (`set_hw_bp`/`clear_hw_bp`, lines 502–550; control word
  `ARM64_HWBP_CTRL 0x1e5`, slot count read from `dbg_info & 0xff` at line 528),
  and self-probes `PTRACE_SINGLESTEP` functionality with the hang-proof
  `probe_singlestep()` (lines 552–598) so `asmtest_ptrace_available()` returns
  0 under qemu-user with the skip reason "AArch64 PTRACE_SINGLESTEP is
  non-functional here (e.g. qemu-user emulation)…" (lines 600–622).
  `run_until()` (lines 678–783) is the shared breakpoint-and-continue used by
  `run_to` and the call-out step-over: software `brk` by default, hardware
  breakpoint on a `POKETEXT` refusal, `ASMTEST_PTRACE_HW_BP` forces hardware.
- [include/asmtest_ptrace.h](../../../include/asmtest_ptrace.h) — the public
  surface (`asmtest_ptrace_available/skip_reason/trace_call/trace_attached/
  run_to`, the `asmtest_proc_*` resolvers, `asmtest_jitdump_find`, …).
- [examples/test_hwtrace.c](../../../examples/test_hwtrace.c) — the C parity
  harness. It already arch-selects AArch64 fixtures: `ROUTINE_A64`/`LOOP_A64`
  (lines 125–138), expected stream `{0x0, 0x4, 0x8, 0x10}` and a 63-insn
  20-trip loop (lines 3845–3852). The attach / `/proc` resolve / `run_to` /
  versioned / call-out tests are all compiled for both arches; the ONE
  arch-asymmetry is that `test_ptrace_callout("hardware bp", 1)` is
  unconditionally skipped on AArch64 with "hardware breakpoints are x86-64
  only" (lines 6003–6009) — stale relative to the shipped `NT_ARM_HW_BREAK`
  seam, and fixed by T2 below.
- [mk/native-trace.mk](../../../mk/native-trace.mk) — `hwtrace-test` (line
  2127) runs `test_hwtrace` + the IBS probe/suite; `hwtrace-bindings-test`
  and the per-language `hwtrace-<lang>-test` targets (lines 2523–2723).
- [mk/docker.mk](../../../mk/docker.mk) — `DOCKER_PLATFORM=linux/arm64`
  emulates the aarch64 runner (lines 20–27); `HWTRACE_DOCKER_LANGS := cpp rust
  go node java dotnet ruby lua zig` (line 433, python runs on the host job);
  `docker-hwtrace` (line 442), the per-language `docker-hwtrace-<lang>` rule
  (lines 638–643), and `docker-hwtrace-bindings` (line 645).
- [.github/workflows/ci.yml](../../../.github/workflows/ci.yml) — the `test`,
  `benchmarks`, `emu`, and `asm` jobs already run on `ubuntu-24.04-arm` (lines
  37, 84, 275, 309), so the repo builds and its core suites pass on that
  runner today. `hwtrace-test` runs only on `ubuntu-latest` (the `hwtrace` job,
  line 531) and nightly on `macos-15-intel` — never on the arm runner. The
  `dataflow` job (lines 625–678) is the repo's anti-vacuity pattern: tee the
  log, fail on unexpected `# SKIP` lines, fail on `not ok`, enforce a
  pass-count floor.
- [benchmarks/boxes/](../../../benchmarks/boxes/) — per-box capability
  records; currently `amd-linux-x86_64-9e05f0f2`, `amd-linux-x86_64-f39fe67d`,
  `intel-macos-x86_64-de7ec54c` (no arm64 box). Produced by
  `make bench-record` → [scripts/bench-report.sh](../../../scripts/bench-report.sh)
  `--record` (`ASMTEST_BOX_ID` / `ASMTEST_VIRTUALIZED` env overrides, lines
  77, 92–93).
- [tools/asmfeatures.c](../../../tools/asmfeatures.c) — the capability sweep
  behind each box record's `features.json`. Its native-trace completeness
  probe is x86-64-host-only (rows hardcode `"x86_64"`, lines 327–339) and it
  has **no** out-of-process ptrace row at all — extended in T3.
  [tools/asmbench_fixtures.h](../../../tools/asmbench_fixtures.h) already
  carries `FIX_A64_ADD3` (line 30).
- The ten binding hwtrace suites all have ptrace-toolkit sections gated on
  `asmtest_ptrace_available()`, but their fixtures are **x86-64 machine code
  only** (e.g. [bindings/python/tests/test_hwtrace.py](../../../bindings/python/tests/test_hwtrace.py)
  lines 27–30 and 399–409;
  [bindings/go/hwtrace_test.go](../../../bindings/go/hwtrace_test.go) lines
  21–24 and 562–592). On real AArch64, `available()` returns 1 and those tests
  would execute x86 bytes → SIGILL, i.e. they **fail rather than skip**. Java
  already guards two of its tests with `hostIsX86_64()`
  ([bindings/java/HwTraceTest.java](../../../bindings/java/HwTraceTest.java)
  lines 1255–1269 — "the ptrace stepper is available on aarch64 too, but these
  bytes would fault there"), but its plain `ptraceTraceCall` is unguarded.
  This is what SS-A64-BIND (T4) fixes.

**Prove the baseline green before touching anything:**

```sh
make hwtrace-test            # on any x86-64 Linux/macOS host: "== hwtrace-test ==",
                             # a stream of "ok N - ..." lines, exit 0
make docker-hwtrace DOCKER_PLATFORM=linux/arm64
                             # arm64 build sanity under qemu: suite passes, and the
                             # ptrace tests print "# SKIP ... AArch64 PTRACE_SINGLESTEP
                             # is non-functional here (e.g. qemu-user emulation)"
```

The second command needs Docker with qemu binfmt (Docker Desktop ships it); it
proves the arm64 compile is clean and the emulation self-skip is intact — the
exact behavior that must NOT appear on the real runner.

## Tasks

### T1 — Add the `hwtrace-arm64` CI job: the live AArch64 single-step stream, asserted non-vacuous  (M, depends on: none)

**Goal.** `make hwtrace-test` runs on a `ubuntu-24.04-arm` GitHub-hosted
runner with the ptrace tier executing LIVE (not self-skipping), gating every
push.

**Steps.**

1. Re-verify locally that the arm64 build is clean:
   `make docker-hwtrace DOCKER_PLATFORM=linux/arm64` (see baseline above).
2. Add a job `hwtrace-arm64` to
   [.github/workflows/ci.yml](../../../.github/workflows/ci.yml), mirroring
   the existing `hwtrace` job (line 531) with three deltas:
   - `runs-on: ubuntu-24.04-arm` (same label the `test` job already uses,
     line 37).
   - Dependencies: **do not** install `libipt-dev` (x86-only PT decode; the PT
     backend honestly self-skips). Build the pinned Capstone 5.0.1 from source
     via the cache-then-build pair the `cli` job uses (ci.yml lines 577–584:
     `actions/cache` on `~/.cache/asmtest-thirdparty/capstone` keyed on the
     pinned inputs, then `sh scripts/thirdparty-cache.sh cached-build
     capstone`). [scripts/build-capstone.sh](../../../scripts/build-capstone.sh)
     is arch-neutral (cmake source build, digest-pinned). Optionally add
     `libopencsd-dev` (arm-relevant, available on arm64) — not required.
   - Run step: `set -o pipefail; make WERROR=1 hwtrace-test 2>&1 | tee
     /tmp/hwtrace-arm64.log`.
3. Add the anti-vacuity step, mirroring the `dataflow` job's shape (ci.yml
   lines 630–678):
   - **Hard fail** if the log contains `AArch64 PTRACE_SINGLESTEP is
     non-functional` — the qemu/self-skip string
     ([src/ptrace_backend.c](../../../src/ptrace_backend.c) line 616) must
     never print on real silicon; if it does, the job proved nothing.
   - **Hard fail** on any `^not ok` line.
   - **Fail on unexpected `# SKIP` lines**, with a curated BY-NAME allowlist
     (never blanket): the in-process single-step backend (x86-only), Intel
     PT / AMD LBR / CoreSight capture, IBS (non-AMD host), BTF block-step
     (`PTRACE_SINGLEBLOCK` has no AArch64 form — see
     [ptrace-blockstep-tracer-correctness.md](ptrace-blockstep-tracer-correctness.md)
     for that tier), and — until T2 lands — the `hardware bp` call-out skip.
   - **Pass-count floor**: after the first green run, set
     `grep -c '^ok '` ≥ (observed − a few), with a comment recording the
     observed count and date, exactly like the dataflow job's floor comment
     (ci.yml lines 662–677).
4. Push, watch the run, and **triage whatever breaks** — this is the first
   time the capture stream executes. The assertions that prove the stream are
   already in the harness: "ptrace oop yields the exact in-process instruction
   stream" against `{0x0, 0x4, 0x8, 0x10}`, the 63-insn no-depth-ceiling loop
   (test_hwtrace.c lines 3845–3914), `test_ptrace_attach`,
   `test_proc_resolve_and_trace`, `test_run_to_and_trace` (software `brk`
   plant via `PTRACE_POKETEXT`), `test_ptrace_versioned`, and
   `test_ptrace_callout("software int3")`. Likely-benign environment notes:
   Ubuntu's default Yama `ptrace_scope=1` allows all of these (parent traces
   its forked child; the attach victims opt in via `PR_SET_PTRACER_ANY` — see
   the comment at [mk/native-trace.mk](../../../mk/native-trace.mk) lines
   2168–2174); the fixtures already call `__builtin___clear_cache` after
   writing code (test_hwtrace.c line 3875), which AArch64 requires.
5. Fix defects in [src/ptrace_backend.c](../../../src/ptrace_backend.c) /
   the harness as found, keeping x86-64 green (`make hwtrace-test` on Linux
   x86-64 or via `make docker-hwtrace`).

**Code.** ci.yml only (plus whatever the live run flushes out). No library
code changes are expected a priori; any that ARE needed follow the existing
`#if defined(__aarch64__)` seams in `src/ptrace_backend.c`.

**Tests.** The job IS the test: the C harness's existing AArch64 assertions,
executed for the first time, with the anti-vacuity step making a silent
self-skip a red build. Failure looks like `not ok N - ptrace oop yields the
exact in-process instruction stream` (or the ::error from the skip-grep); a
pass is the full `ok` stream plus the step's summary line (e.g. `hwtrace-arm64:
N assertions, ptrace tier ran live, no unexpected skips`).

**Docs.** Internal-only at this stage (the plan/guide flip happens in T6 once
the whole set is green).

**Done when.**

- The `hwtrace-arm64` job is green on `main`, its log shows the ptrace `ok`
  lines (not the qemu skip string), and the anti-vacuity step passes.
- `make docker-hwtrace DOCKER_PLATFORM=linux/arm64` still passes locally with
  the honest qemu self-skip (the emulation path keeps self-skipping cleanly on
  this Intel-Mac / any x86-64 host).
- The x86-64 `hwtrace` job and `make hwtrace-test` on x86-64 are unchanged
  and green.

### T2 — Arm-and-fire `NT_ARM_HW_BREAK` probe; un-skip the forced-hardware call-out on AArch64  (S, depends on: T1)

**Goal.** Replace the stale unconditional "hardware breakpoints are x86-64
only" skip with a definitive, hang-proof arm-and-fire probe, so the
`NT_ARM_HW_BREAK` `run_until` fallback is live-validated where the hypervisor
delivers debug exceptions and self-skips with a **measured, specific** reason
where it does not.

**Steps.**

1. In [examples/test_hwtrace.c](../../../examples/test_hwtrace.c), add a
   static `probe_arm64_hwbp(void)` (AArch64-only, next to the call-out tests):
   fork a child that does `PTRACE_TRACEME; raise(SIGSTOP);` then calls a tiny
   local function; the parent, from the SIGSTOP stop, reads the
   `NT_ARM_HW_BREAK` regset (`PTRACE_GETREGSET`, `struct user_hwdebug_state`)
   and classifies:
   - `dbg_info & 0xff == 0` → return "no slots" (qemu-user's case, and one of
     the two possible hypervisor outcomes).
   - otherwise arm slot 0 at the target function's address with control word
     `0x1e5` (the exact encoding shipping at
     [src/ptrace_backend.c](../../../src/ptrace_backend.c) lines 511–535 —
     re-derive it in the test rather than exporting the static, the way
     [examples/watchpoint_spike.c](../../../examples/watchpoint_spike.c)
     re-derived DR7 per
     [2026-07-15-hw-watchpoint-spike.md](../analysis/2026-07-15-hw-watchpoint-spike.md)),
     `PTRACE_CONT`, then a WNOHANG + deadline wait loop copied from
     `probe_singlestep()` (lines 560–597 — hang-proof is mandatory). A SIGTRAP
     whose stop-PC equals the target → "fires"; deadline expiry → "armed but
     never fired" (the accepted-but-silent hypervisor mode documented for WSL2
     x64 — see Research notes). Always SIGKILL + reap the child.
2. Replace the unconditional AArch64 skip in `test_ptrace_callout` (lines
   6003–6009) with the probe: run the `("hardware bp", 1)` case (which drives
   the real library path — `ASMTEST_PTRACE_HW_BP=1` → `run_until` →
   `set_hw_bp`) when the probe says "fires"; otherwise print ONE of two
   distinguishable skip lines: `# SKIP ptrace callout (hardware bp): no
   NT_ARM_HW_BREAK slots on this host` or `# SKIP ptrace callout (hardware
   bp): NT_ARM_HW_BREAK armed but never fired (hypervisor withholds debug
   exceptions)`.
3. Do NOT add a public `asmtest_ptrace_*` probe symbol: the binding
   function-parity gate counts the tier's exported surface across all ten
   bindings (51 symbols × 10 per the
   [plan](../plans/zen2-singlestep-trace-plan.md)), so a new export would
   ripple through every wrapper for no user value. Keep it test-static.
4. Update the `hwtrace-arm64` job's skip-allowlist (T1 step 3): the `hardware
   bp` skip is no longer blanket-allowed; instead the assert step emits a
   `::notice` carrying whichever of the three outcomes (ran live / no slots /
   armed-but-silent) the log shows. **The first green run of this step is the
   decisive public answer to the open research question** — record the outcome
   in the job's floor-comment and carry it into T6's docs pass.
5. `make fmt` (clang-format is CI-gated via `fmt-check`).

**Code.** `examples/test_hwtrace.c` only (~60 lines): the probe, the
reworked gate, the two skip strings. x86-64 behavior unchanged
(`test_ptrace_callout("hardware bp", 1)` already runs there, line 8305).

**Tests.** Self-testing by construction. On x86-64: unchanged pass. Under
qemu (`make docker-hwtrace DOCKER_PLATFORM=linux/arm64`): must print the
"no slots" skip (qemu emulates zero slots — same fact `set_hw_bp` already
encodes). On the arm64 runner: either the forced-hw call-out `ok` lines
(identical stream `{0x0, 0x4, 0x8}` to the software case, result 43) or the
"armed but never fired" skip — never a hang, never `not ok`.

**Docs.** Internal-only until the outcome is known; T6 writes the measured
result into
[docs/guides/tracing/native-tracing.md](../../../docs/guides/tracing/native-tracing.md)
(its lines 539–540 currently claim "AArch64's hardware-breakpoint ptrace
interface is a separate follow-on; there `run_to` is software-only for now" —
stale, the seam shipped).

**Done when.**

- On the arm64 runner the probe prints exactly one of the three outcomes and
  the job stays green either way, with the `::notice` surfacing it.
- Under qemu the "no slots" skip prints; on x86-64 nothing changes.
- The stale "x86-64 only" skip string is gone from the tree.

### T3 — Record the first arm64 `benchmarks/boxes/` entry (with an out-of-process ptrace feature row)  (S, depends on: T1)

**Goal.** `benchmarks/boxes/` gains a committed arm64 box record whose
`features.json` actually shows the out-of-process ptrace tier live — the
plan's "record a benchmarks/boxes entry" deliverable.

**Steps.**

1. Extend [tools/asmfeatures.c](../../../tools/asmfeatures.c) with an
   out-of-process ptrace row (today the sweep has no ptrace row at all, so an
   arm64 record would show nothing this doc validates): tier `"native-oop"`,
   backend `"ptrace_singlestep"`, arch = host, scope `"host"`. When
   `asmtest_ptrace_available()` is 1, run `asmtest_ptrace_trace_call` on the
   host-arch add3 fixture from
   [tools/asmbench_fixtures.h](../../../tools/asmbench_fixtures.h)
   (`FIX_X86_ADD3` on x86-64 — 4 insns; `FIX_A64_ADD3` on AArch64 — 3 insns),
   reporting `trace_insns`/`insns_truth`/`complete` like the `native-hw` rows
   (mirror `row()` usage at lines 327–339); when 0, emit
   `available: false` with `asmtest_ptrace_skip_reason()`. Guard the whole row
   `#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))`,
   emitting an honest unavailable row elsewhere (macOS).
2. Verify on the dev host: `make bench-report` — the report's `features` array
   gains the row (`available: true` on x86-64 Linux / in `make docker-bench`;
   `"skip_reason": "AArch64 PTRACE_SINGLESTEP is non-functional here…"` under
   `DOCKER_PLATFORM=linux/arm64`). `make bench-check` must stay green (it
   gates only the deterministic emu counts, which this does not touch).
3. In the `benchmarks` job
   ([.github/workflows/ci.yml](../../../.github/workflows/ci.yml) lines
   77–104), add two arm64-conditional steps
   (`if: matrix.os == 'ubuntu-24.04-arm'`):
   - `ASMTEST_VIRTUALIZED=1 ASMTEST_BOX_ID=arm-linux-arm64-gha make
     bench-record` — `ASMTEST_VIRTUALIZED=1` because the runner is a VM
     (perf-history is flagged not-comparable, exactly what
     [scripts/bench-report.sh](../../../scripts/bench-report.sh) line 156
     prints); the explicit `ASMTEST_BOX_ID` because arm64 `/proc/cpuinfo` has
     no `vendor_id`/`model name`, so the auto-derived id degrades to
     `unknown-linux-arm64-<hash8>`.
   - upload `benchmarks/boxes/arm-linux-arm64-gha/` as artifact
     `arm64-box-record` (`if-no-files-found: error`).
4. Download the artifact from a green run, commit the
   `benchmarks/boxes/arm-linux-arm64-gha/{features.json,perf-history.jsonl}`
   directory to `main`, and push (this repo commits straight to `main`).
   Automated nightly re-recording is
   [benchmarks-ci-followups.md](benchmarks-ci-followups.md)'s territory — do
   not build it here.

**Code.** `tools/asmfeatures.c` (~40 lines), ci.yml (two steps), one committed
record directory.

**Tests.** `make bench-report` locally (row present, well-formed JSON —
the script pipes through python3's `json` so malformed output fails loudly);
`make bench-check` green; the committed `features.json` shows
`"tier": "native-oop", "available": true, "complete": true, "trace_insns": 3,
"insns_truth": 3` on the arm64 record.

**Docs.** Add the new box + the `native-oop` row meaning to
[docs/guides/cross-system-benchmarking.md](../../../docs/guides/cross-system-benchmarking.md)
(it already documents the box-record layout and the qemu-arm64 caveats at
lines 150 and 177). Changelog in T6.

**Done when.**

- `benchmarks/boxes/arm-linux-arm64-gha/` is committed with the live
  `native-oop` row.
- The three existing x86 box records are untouched (re-recording them to pick
  up the new row is optional and happens whenever those boxes next run
  `make bench-record`).
- `make bench-report` on any host emits the row (available or honestly not).

### T4 — SS-A64-BIND: arch-selected AArch64 fixtures in all ten binding hwtrace suites  (M, depends on: T1)

**Goal.** On real AArch64 every binding's ptrace-gated tests run LIVE against
AArch64 machine-code fixtures (instead of faulting on x86 bytes), and still
pass/skip exactly as today on x86-64 and under qemu.

**Steps.**

1. For each of the ten suites, add the AArch64 twin fixtures, copying the
   bytes verbatim from [examples/test_hwtrace.c](../../../examples/test_hwtrace.c)
   lines 130–137:
   - `ROUTINE_A64` (`add x0,x0,x1; cmp x0,#100; b.le +4; sub x0,x0,#1; ret`)
     — `trace_call(20,22)` returns 42, expected stream
     `[0x0, 0x4, 0x8, 0x10]`, 2 blocks, not truncated.
   - `LOOP_A64` where the suite exercises the loop — 63 executed insns for
     `loop(1,20)`.
2. Arch-select per language idiom (the fixture picker is the only new logic;
   the assertions parameterize on `(bytes, want_insns, want_result)`):
   - [bindings/python/tests/test_hwtrace.py](../../../bindings/python/tests/test_hwtrace.py)
     — `platform.machine() in ("aarch64", "arm64")`; parameterize
     `test_ptrace_trace_call` (lines 399–409).
   - [bindings/go/hwtrace_test.go](../../../bindings/go/hwtrace_test.go) —
     `runtime.GOARCH == "arm64"`; `TestPtraceTraceCall` (line 562).
   - [bindings/rust/tests/hwtrace.rs](../../../bindings/rust/tests/hwtrace.rs)
     — `cfg!(target_arch = "aarch64")` (ptrace section from line 456).
   - [bindings/java/HwTraceTest.java](../../../bindings/java/HwTraceTest.java)
     — reuse the existing `hostIsX86_64()` (line 1256) to select bytes in
     `ptraceTraceCall` (line ~813) instead of only guarding descent.
   - [bindings/node/test_hwtrace.js](../../../bindings/node/test_hwtrace.js)
     — `process.arch === 'arm64'` (ptrace section from line 620).
   - [bindings/cpp/test_hwtrace.cpp](../../../bindings/cpp/test_hwtrace.cpp)
     — `#if defined(__aarch64__)` (section from line 351).
   - [bindings/ruby/test_hwtrace.rb](../../../bindings/ruby/test_hwtrace.rb)
     — `RUBY_PLATFORM =~ /aarch64|arm64/` (section from line 279).
   - [bindings/lua/test_hwtrace.lua](../../../bindings/lua/test_hwtrace.lua)
     — probe `io.popen("uname -m")` (section from line 412).
   - [bindings/zig/src/hwtrace_test.zig](../../../bindings/zig/src/hwtrace_test.zig)
     — `builtin.cpu.arch == .aarch64` (section from line 155).
   - [bindings/dotnet/hwtrace/HwTraceProgram.cs](../../../bindings/dotnet/hwtrace/HwTraceProgram.cs)
     — `RuntimeInformation.ProcessArchitecture == Architecture.Arm64`
     (section from line 732).
3. Audit each suite's ptrace-gated tests that CANNOT be arch-parameterized and
   give them explicit x86-64 guards with a printed skip (mirror Java's
   pattern and message at lines 1264–1268), never a silent pass:
   - **stealth** tests (e.g. python `test_stealth_trace_out_of_process`, node's
     `stealthTrace` block): the stealth stepper compiles on AArch64
     ([src/hwtrace.c](../../../src/hwtrace.c) line 2999) but its C oracle test
     is x86-64-gated (test_hwtrace.c line 6080), so the binding tests must
     skip on arm64 until the C oracle validates that path (a follow-on, not
     this doc).
   - **descent** fixtures (x86 call-blob) — python lines 483–514, java already
     guarded.
   - node's x86 `movabs` BigInt leaf (test_hwtrace.js lines 640–652).
   - **blockstep** tests need no change: `blockstep_available()` is already 0
     on AArch64 and every suite gates on it.
4. Verify without hardware: `make hwtrace-bindings-test` on the x86-64 path
   (unchanged), and at least one qemu lane —
   `make docker-hwtrace DOCKER_PLATFORM=linux/arm64` (covers the python
   wrapper via the hwtrace image) plus one compiled binding, e.g.
   `make docker-hwtrace-go DOCKER_PLATFORM=linux/arm64` — where every ptrace
   test must SKIP with the qemu reason (fixtures compile, gate holds). The
   live arm64 proof comes from T5.

**Code.** Ten test files; no wrapper/library code changes (the wrappers
already pass raw bytes through).

**Tests.** These ARE tests. Failure mode being eliminated: on real arm64,
today's suites die with SIGILL inside the tracee (surfacing as a non-zero
`trace_call` rc or a crashed harness); after this task they print live `ok`
assertions on `[0x0, 0x4, 0x8, 0x10]`. On x86-64: byte-identical behavior to
today.

**Docs.** Internal-only, no user-facing docs — the binding READMEs don't
enumerate per-arch fixtures, and the guide flip is T6.

**Done when.**

- `make hwtrace-bindings-test` green on x86-64 Linux (and the macOS-host
  subset unchanged).
- Under `DOCKER_PLATFORM=linux/arm64`, sampled lanes show ptrace SKIPs (not
  failures).
- Each of the ten suites contains the A64 fixture and an explicit x86-only
  skip for stealth/descent-style tests (grep proof:
  `grep -rl "0x1f, 0x90, 0x01, 0xf1" bindings/ | wc -l` → 10, allowing for
  per-language byte formatting).

### T5 — Run the binding lanes live on the arm64 runner (`hwtrace-bindings-arm64` + host python)  (S, depends on: T4)

**Goal.** The ten bindings' live AArch64 fixtures actually execute on real
silicon in CI: the nine docker lanes natively on the arm64 runner, python on
the runner host.

**Steps.**

1. Add `make WERROR=1 hwtrace-python-test` as a step on the T1 `hwtrace-arm64`
   job (mirroring the x86 `hwtrace` job's python step, ci.yml lines 548–549) —
   python is excluded from `HWTRACE_DOCKER_LANGS`, so the host job is its only
   lane.
2. Add a `hwtrace-bindings-arm64` job cloned from `hwtrace-bindings` (ci.yml
   lines 734–767) with:
   - `runs-on: ubuntu-24.04-arm`.
   - First step: probe `docker version`; if absent, `sudo apt-get install -y
     docker.io` and start the daemon (the runner is a full VM with
     passwordless sudo — see Research notes; whether Docker is preinstalled on
     the arm64 image is not verified anywhere public, so probe-then-install
     rather than assume).
   - A **distinct** buildx cache scope (e.g.
     `scope=asmtest-bindings-base-arm64`) — reusing the x86 scope would
     poison/thrash both caches with foreign-arch layers.
   - Run `make docker-hwtrace-bindings 2>&1 | tee /tmp/hwb-arm64.log`. On an
     arm64 host `DOCKER_PLATFORM` stays empty, so the images build and run
     natively arm64 — the design already anticipated in
     [mk/docker.mk](../../../mk/docker.mk) lines 20–22.
   - Anti-vacuity step: hard-fail if the log contains any
     ptrace-unavailability skip (`ptrace backend unavailable` /
     `ptrace toolkit unavailable` / `AArch64 PTRACE_SINGLESTEP is
     non-functional` — collect the exact per-binding strings from the T4 diff
     while implementing); hard-fail on failed-assertion markers. The x86-only
     skips added in T4 (stealth/descent/blockstep) are the allowlist.
   - Drop the two dotnet-extra steps (`docker-hwtrace-dotnet9`,
     `docker-hwtrace-dotnet-stress`) from the clone: they are x86-lane
     hardening with their own history; add them later only if they prove
     arm64-clean.
3. Expect per-language toolchain fallout (e.g. the Zig tarball fetch and any
   image whose setup is arch-conditional — see `DOCKER_SETUP_<lang>` in
   [mk/docker.mk](../../../mk/docker.mk) lines 136–140): per CLAUDE.md, a
   missing installable dependency is added to the relevant Dockerfile/setup
   var with a pinned version, never a self-skip. If a language's toolchain
   genuinely does not ship for linux/arm64, record it in the job as a
   named, printed skip and note it in T6's docs pass.

**Code.** ci.yml only (plus targeted Dockerfile/`DOCKER_SETUP_*` fixes if a
lane's image is not arch-clean).

**Tests.** The job. Pass: nine lanes green natively arm64, each log showing
the live A64 ptrace assertions; python's live assertions on the host step.
Failure: the anti-vacuity grep names the binding whose gate self-skipped.

**Docs.** Internal-only (T6 flips the guides).

**Done when.**

- `hwtrace-bindings-arm64` and the python step are green on `main`.
- The x86 `hwtrace-bindings` job is untouched and green.
- Any lane that could not run on arm64 is a NAMED skip with a recorded reason,
  not a silent absence.

### T6 — Flip the plan, guides, and changelog to "validated on real AArch64"  (S, depends on: T1, T2, T3, T4, T5)

**Goal.** Every doc that says the AArch64 stream is "pending real hardware"
tells the new truth, including the measured hardware-breakpoint outcome.

**Steps.**

1. [docs/internal/plans/zen2-singlestep-trace-plan.md](../plans/zen2-singlestep-trace-plan.md):
   rewrite the "one genuinely remaining Phase-5 front" paragraph (lines
   146–155) and the Phase-5 status bullets (lines 307–311, the AArch64-tracer
   *Done* note's "awaits a real AArch64 host" sentences at lines 498–508, and
   the hardware-breakpoint note's "pending real AArch64 hardware" at line
   441) to: validated live on GitHub `ubuntu-24.04-arm` (Cobalt 100 /
   Neoverse-N2-class VM), date, job names, box id `arm-linux-arm64-gha`;
   record the T2 hardware-breakpoint outcome verbatim (fires / no slots /
   armed-but-silent). Keep the plan's own status-legend convention (amend,
   don't erase).
2. [docs/guides/tracing/native-tracing.md](../../../docs/guides/tracing/native-tracing.md):
   - lines 446–450: replace "code-implemented and build/self-skip-validated,
     with the live stream **pending real hardware**" with the validated
     statement (still self-skips under qemu-user — that behavior is
     unchanged and worth keeping documented).
   - lines 539–540: replace "AArch64's hardware-breakpoint ptrace interface
     is a separate follow-on; there `run_to` is software-only for now" with
     the `NT_ARM_HW_BREAK` fallback description + the measured CI outcome.
3. [docs/guides/tracing/hardware-tracing.md](../../../docs/guides/tracing/hardware-tracing.md)
   line 546–547 ("the out-of-process ptrace form adds AArch64"): append the
   validated-on note.
4. [docs/guides/cross-system-benchmarking.md](../../../docs/guides/cross-system-benchmarking.md):
   the arm64 box row (if not already done in T3).
5. [CHANGELOG.md](../../../CHANGELOG.md) under `## [Unreleased]` → `### Added`:
   one entry covering the arm64 CI lanes, the live AArch64 stream validation,
   the `native-oop` features row + arm64 box record, and the per-binding
   AArch64 fixtures.
6. `make docs` (or `make docker-docs`) — the Sphinx build is `-W`
   fail-on-warning; fix anything it flags.

**Code.** Docs + changelog only.

**Tests.** `make docs` exit 0; `git grep -n "pending real" docs/guides` returns
nothing about the AArch64 stream.

**Docs.** This task IS the docs.

**Done when.**

- No published page or plan still claims the AArch64 live stream is
  unvalidated.
- The hardware-breakpoint answer appears with its measured value and date, not
  as speculation.
- Changelog entry present; docs build green.

## Task order & parallelism

```
T1 ──┬── T2 (hw-bp probe; tightens T1's allowlist)
     ├── T3 (box record)                 ─┐
     └── T4 (binding fixtures) ── T5      ├── T6 (docs flip)
                                          ─┘
```

- **T1 is the critical path** — everything else consumes its runner lane.
- T2, T3, T4 are mutually independent once T1 is green (three people can work
  in parallel). T5 needs T4. T6 needs everything.
- T4 can be *authored* before T1 is green (its qemu verification needs no
  hardware), but must not be declared done until T5 executes it live.

## Constraints & gates

- **No new hardware gate remains for the ptrace half.** The `ubuntu-24.04-arm`
  runner is real silicon under a hypervisor and demonstrably runs ptrace
  tracer/tracee suites (see Research notes); per CLAUDE.md, that makes this
  installable-in-CI, not a legitimate self-skip. The qemu self-skip stays
  correct **for emulated environments only**.
- **The hardware-breakpoint half may hit a REAL gate**: if T2 measures
  "armed but never fired", debug-exception delivery is withheld by Azure's
  hypervisor — that is a hypervisor/hardware fact, recorded as a named,
  classified self-skip (the CLAUDE.md hardware exception), and bare-metal
  validation moves to [self-hosted-ci-runners.md](self-hosted-ci-runners.md)
  territory. Never let it silently degrade: the probe's two skip strings keep
  the two causes distinguishable forever.
- **Pinning**: Capstone stays the pinned 5.0.1 source build
  (`scripts/build-capstone.sh` + `scripts/third-party-digests.txt`); no distro
  Capstone on the new jobs. Any Dockerfile additions T5 forces follow the
  pinned-version patterns already in the tree.
- **Virtualized perf numbers are not comparable**: the arm64 box record MUST
  carry `ASMTEST_VIRTUALIZED=1`; also note the runner's PMU is withheld
  (perf-event sampling returns nothing there — Research notes), which is why
  no perf-based lane is proposed for this runner.
- **Binding parity gate**: do not add public `asmtest_ptrace_*` symbols in
  this work (T2 note) — the 51-symbol × 10-binding parity gate would demand
  ten wrapper updates for a test-only need.
- The runner's default Yama `ptrace_scope=1` is sufficient for every test here
  (descendant tracing + `PR_SET_PTRACER_ANY` opt-ins); do not add `sudo sysctl
  kernel.yama.ptrace_scope=0` unless a live failure proves it necessary, and
  if so, comment why in the workflow.

## Research notes (verified 2026-07-17)

- **Runner availability**: GitHub `ubuntu-24.04-arm` hosted runners — public
  preview free for public repos 2025-01-16
  (<https://github.blog/changelog/2025-01-16-linux-arm64-hosted-runners-now-available-for-free-in-public-repositories-public-preview/>),
  GA for public repos 2025-08-07
  (<https://github.blog/changelog/2025-08-07-arm64-hosted-runners-for-public-repositories-are-now-generally-available/>),
  available in private repos since 2026-01-29
  (<https://github.blog/changelog/2026-01-29-arm64-standard-runners-are-now-available-in-private-repositories/>).
- **Runner shape**: each standard runner is a full VM (not a container) with
  passwordless sudo
  (<https://docs.github.com/en/actions/reference/runners/github-hosted-runners>);
  hardware is 4-vCPU Azure **Cobalt 100** (Arm Neoverse-N2-based,
  <https://learn.microsoft.com/en-us/azure/virtual-machines/sizes/cobalt-overview>),
  Dpdsv6 SKU per GitHub staff in the launch discussion, which also confirms
  **no `/dev/kvm`** ("kvm: HYP mode not available" — guest kernel at EL1 under
  Azure's hypervisor) (<https://github.com/orgs/community/discussions/148648>).
- **ptrace works there, by proxy**: strace's aarch64 build+test matrix (a
  suite that is entirely PTRACE_TRACEME/PTRACE_SYSCALL tracer-tracee pairs)
  runs green on these exact runners
  (workflow <https://github.com/strace/strace/blob/master/.github/workflows/ci.yml>,
  green run <https://github.com/strace/strace/actions/runs/29562526806>);
  CRIU's aarch64 jobs (compel parasite injection, PTRACE_SEIZE/ATTACH on
  arbitrary processes — beyond parent-child) pass
  (<https://github.com/checkpoint-restore/criu/actions/runs/23529675698>);
  GEF runs its gdb-driven suite on `ubuntu-24.04-arm` in containers
  (<https://github.com/hugsy/gef/blob/main/.github/workflows/tests.yml>).
  asm-test's parent-traces-forked-child model is additionally safe under
  Ubuntu's default Yama `ptrace_scope=1`.
- **`NT_ARM_HW_BREAK`/`NT_ARM_HW_WATCH` firing is NOT publicly determined**
  on these runners — exhaustive search found no report either way. Adjacent
  facts cut both ways: the hypervisor withholds the PMU (perf collects zero
  samples: <https://github.com/actions/runner-images/issues/11689>,
  <https://github.com/actions/runner-images/issues/11789>) but PMU counters
  are separate silicon from the DBGB/DBGW debug registers; and there is
  documented precedent for accepted-but-never-firing hardware watchpoints
  under a Microsoft hypervisor (WSL2 x64,
  <https://github.com/microsoft/WSL/issues/5741>, cited as the failure-mode
  precedent, not as current WSL behavior). Slot counts come from
  `ID_AA64DFR0_EL1` read by the kernel's hw_breakpoint driver
  (<https://github.com/torvalds/linux/blob/master/arch/arm64/kernel/hw_breakpoint.c>)
  and are reported via ptrace `dbg_info`
  (<https://github.com/torvalds/linux/blob/master/arch/arm64/kernel/ptrace.c>),
  so the guest will very likely REPORT nonzero slots; only a delivered-SIGTRAP
  probe (T2) is decisive. Do NOT cite LLVM premerge as evidence — its AArch64
  lldb lane runs on Depot's `depot-ubuntu-24.04-arm-16`, not GitHub-hosted
  (<https://github.com/llvm/llvm-project/blob/main/.github/workflows/premerge.yaml>).
- **Repo-internal precedent** for the hypervisor-masking pattern: the x86
  dataflow CI job already documents "GitHub's runners are VMs whose hypervisor
  MASKS BTF/DEBUGCTL" and allows that one skip by name
  ([.github/workflows/ci.yml](../../../.github/workflows/ci.yml) lines
  636–648) — T1/T2's assert steps copy that posture.
- Capstone pin: 5.0.1 source build, digest-pinned
  ([scripts/build-capstone.sh](../../../scripts/build-capstone.sh),
  `scripts/third-party-digests.txt`).

## Out of scope

- **asmspy / `cli/` on AArch64** — the CLI's arm64 port and its lanes:
  [asmspy-aarch64-support.md](asmspy-aarch64-support.md). This doc validates
  the *library* tracer (`src/ptrace_backend.c`) only.
- **Apple Silicon out-of-process stepping** (Mach exception ports — the
  ptrace backend is Linux-only): [macos-oop-mach-stepper.md](macos-oop-mach-stepper.md).
- **BTF block-step**: no AArch64 form exists (`PTRACE_SINGLEBLOCK` is
  x86/ppc/s390); the x86 tier's correctness work is
  [ptrace-blockstep-tracer-correctness.md](ptrace-blockstep-tracer-correctness.md)
  and the in-process variant is [inproc-btf-block-step.md](inproc-btf-block-step.md).
- **Bare-metal / self-hosted runners** (the fallback if T2 measures
  armed-but-silent, and all perf-dependent arm64 lanes):
  [self-hosted-ci-runners.md](self-hosted-ci-runners.md).
- **ARM CoreSight live decode** (a hardware-trace facility, unrelated to the
  single-step stream): [coresight-live-decode.md](coresight-live-decode.md).
- **SVE state capture on AArch64**: [aarch64-sve-capture.md](aarch64-sve-capture.md).
- **Data watchpoints** (`NT_ARM_HW_WATCH`, write/read-write mode) belong to
  the data-flow tier; this doc only exercises execute breakpoints
  (`NT_ARM_HW_BREAK`). The design groundwork is
  [2026-07-15-hw-watchpoint-spike.md](../analysis/2026-07-15-hw-watchpoint-spike.md).
- **Nightly auto-commit of box records**:
  [benchmarks-ci-followups.md](benchmarks-ci-followups.md).
- **Extending the C §D3 stealth-stepper oracle test to AArch64**
  (examples/test_hwtrace.c line 6080 is x86-gated): a follow-on the T4
  binding-side skips explicitly point at; file it against the plan when the
  arm64 lanes are stable.
