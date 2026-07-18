# Benchmarks CI: windows/macOS-Intel legs, nightly auto-commit, and BM_MODEL_COST — implementation

> **Sources.** Actioned from the archived (completed) plan
> [cross-arch-benchmarking-plan.md](../archive/plans/cross-arch-benchmarking-plan.md)
> (its "remain follow-ups" note, lines 27–30), the analysis
> [2026-07-11-cross-arch-benchmarking.md](../analysis/2026-07-11-cross-arch-benchmarking.md)
> (§4.3 the weighted cost model, §7.6 the BM_MODEL_COST follow-on), and the
> follow-ups comment in
> [.github/workflows/ci.yml](../../../.github/workflows/ci.yml) lines 74–76.
> Written 2026-07-17. If this doc and a source disagree, this doc wins (sources
> may be stale); if the CODE and this doc disagree, re-verify before
> implementing. Note the archived plan's CI table says `macos-13` — that image
> was retired 2025-12-08 and every macOS-Intel leg uses **`macos-15-intel`**
> (see the in-file rationale at ci.yml lines 139–142).

## Why this work exists

The cross-system benchmarking feature landed with a three-leg CI matrix
(`ubuntu-latest`, `ubuntu-24.04-arm`, `macos-latest`), so two of the five
OS × arch systems the feature was designed to compare — real Windows x86-64 and
Intel macOS — produce no report at all, and per-box records only enter the repo
when a human runs `make bench-record` and commits. After this work, every OS
leg produces and uploads a per-system report, the deterministic golden gate
runs on real Windows for the first time, a nightly job auto-commits per-box
history into `benchmarks/`, and the reporter gains the Capstone-weighted
`BM_MODEL_COST` metric — the honest cross-ISA cost proxy the analysis
recommended and the plan deferred.

## What already exists (verified 2026-07-17)

The landed substrate, all verified against the working tree:

- [mk/bench.mk](../../../mk/bench.mk) — the whole bench target group:
  `emu-bench` (lines 15–25), `features`, `bench-report` / `bench-record`
  (lines 54–63), `bench-check` (lines 69–73), `bench-compare`, `docker-bench`
  (lines 83–85). `make help` lists them (Makefile lines 98–105).
- [tools/emu_bench.c](../../../tools/emu_bench.c) — the deterministic producer.
  Seven cases over [tools/asmbench_fixtures.h](../../../tools/asmbench_fixtures.h)
  (`math.add3` × 5 ISAs incl. win64, `loop.sum_to_10`/`sum_to_100` x86-64),
  each run count-only via `asmtest_trace_new(0, 256)` (line 96–97), emitted as
  `{"kind": "insns", "unit": "insn", ...}` JSON rows (lines 180–185).
- [tools/asmfeatures.c](../../../tools/asmfeatures.c) — the capability +
  trace-completeness probe. Its link line pulls `$(HWTRACE_OBJS)` and
  `drtrace_app.o` (mk/bench.mk lines 43–47) — POSIX-only objects, which is why
  it does not build on Windows (relevant to T2).
- [scripts/bench-report.sh](../../../scripts/bench-report.sh) — the runner.
  Already OS-aware: the descriptor probe has a `MINGW*|MSYS*|CYGWIN*` branch
  (lines 54–58), and the `box_id` falls back to
  `<vendor>-<os>-<arch>-<hash8 of the CPU model>` unless `ASMTEST_BOX_ID` is
  set (lines 76–77). Producer paths are currently **hardcoded** to
  `$BUILD/test_bench`, `$BUILD/emu-bench`, `$BUILD/asmfeatures` (lines
  102–105). `--record` persists golden + per-box files (lines 164–197).
- [scripts/bench-golden-check.py](../../../scripts/bench-golden-check.py) —
  the golden gate; `key_rows` (line 16) keys on `(name, arch, value, blocks)`
  with **no `kind` filter** (relevant to T6).
- [scripts/bench-compare](../../../scripts/bench-compare) — the aggregator;
  the deterministic matrix builder (line 102) also has **no `kind` filter**,
  so a second row kind for the same `(name, arch)` would silently overwrite
  the insn cell (relevant to T6).
- [benchmarks/](../../../benchmarks) — `golden/emu-insns.json` (7 rows) and
  three committed boxes under `benchmarks/boxes/` (two amd-linux, one
  intel-macos).
- [.github/workflows/ci.yml](../../../.github/workflows/ci.yml) — the
  `benchmarks` job (lines 77–104, matrix at line 84 =
  `[ubuntu-latest, ubuntu-24.04-arm, macos-latest]`), `benchmarks-compare`
  (lines 108–132), the nightly Intel-mac pattern `test-macos-x86` (lines
  139–147, `runs-on: macos-15-intel`, gated
  `if: github.event_name == 'schedule' || github.event_name == 'workflow_dispatch'`),
  the real-Windows `windows` job (lines 1004–1021, MSYS2/MINGW64 via
  `msys2/setup-msys2@v2`, runs `make win64-runner-test WIN64_CC=gcc WINE=`),
  and the `needs`-two/`if`-one collect pattern in `package-libs-collect`
  (lines 1236–1239). No `permissions:` block exists anywhere in the file yet.
- [mk/win64.mk](../../../mk/win64.mk) — the Windows/mingw lane: `WIN64_CC`,
  `WIN64_NASM`, `WINE` knobs (lines 17–21), `win64-runner-test` (lines
  134–146) which builds `suite_win64.exe` from `src/asmtest.c` +
  `src/platform_win32.c` (so `--bench --bench-format=json` — parsed in
  [src/asmtest.c](../../../src/asmtest.c) line 2075 — works on Windows; one
  `BENCH` case exists at
  [tests/win64/suite_win64.c](../../../tests/win64/suite_win64.c) line 80),
  and `docker-win64` (lines 220–224) building
  [Dockerfile.win64](../../../Dockerfile.win64) on the bindings base.
- Portability facts for T2: [src/emu.c](../../../src/emu.c),
  [src/trace.c](../../../src/trace.c), [src/disasm.c](../../../src/disasm.c)
  include only stdio/stdlib/string + `unicorn/unicorn.h` /
  `capstone/capstone.h` — no POSIX headers. `emu.c` line 113 records
  `trace_append_insn(c->t, address - c->base)`, so recorded insn offsets index
  directly into the fixture bytes (relevant to T6).
- `BM_MODEL_COST` exists nowhere in `include/ src/ tools/ scripts/ mk/`
  (grep verified) — only in the archived plan's struct sketch (line 110) and
  its "a follow-on, not required" note (line 181).

**Prove the baseline green before touching anything** (any Linux host with
Docker; the emulator deps live in the CI image):

```sh
make docker-bench          # builds the image, runs `make bench-report` inside
                           # expect: "bench-report: build/bench-report-linux-x86_64.json"
                           # + a features/native/emu summary, then the JSON on stdout
python3 scripts/bench-compare   # reads the committed benchmarks/ tree
                           # expect: Markdown with Systems + count + capture-depth tables
```

On a host with libunicorn installed (`make deps DEPS_ARGS=--emu`),
`make bench-check` must print `bench-golden-check: OK (7 deterministic count
rows match)`.

## Tasks

### T1 — Add the nightly `benchmarks-macos-x86` leg (macos-15-intel)  (S, depends on: none)

**Goal.** A scheduled/dispatch-gated CI job produces and uploads
`bench-report-macos-15-intel` from a real Intel macOS runner, and
`benchmarks-compare` merges it when present.

**Steps.**

1. In [.github/workflows/ci.yml](../../../.github/workflows/ci.yml), after the
   `benchmarks-compare` job, add a job `benchmarks-macos-x86`. Copy the gating
   fields of `test-macos-x86` (lines 143–147): `runs-on:
   macos-15-intel`, `timeout-minutes: 15`, `if: github.event_name == 'schedule'
   || github.event_name == 'workflow_dispatch'` — but set `name: benchmarks
   (macos-15-intel, nightly)`, do NOT copy the `name: test (macos-15-intel,
   nightly)` at line 144 (that display name is asserted in Done-when). Copy the
   step body of the
   existing `benchmarks` job (lines 85–104): checkout@v7, `make deps
   DEPS_ARGS=--emu`, `make bench-check`, `make bench-report`, upload-artifact@v7
   with `name: bench-report-macos-15-intel`, `path: build/bench-report-*.json`,
   `if-no-files-found: error`.
2. Add job-level `env: { ASMTEST_BOX_ID: gh-macos-15-intel }` — and while
   there, add `ASMTEST_BOX_ID: gh-${{ matrix.os }}` to the existing
   `benchmarks` job. Runner VMs draw from a fleet of CPU models, so the
   default `box_id` (hash of the CPU string, bench-report.sh lines 76–77)
   would mint a new box per model; the plan (§6.1) says CI passes the runner
   label. T4 depends on this being stable.
3. Wire the compare job: change `benchmarks-compare` to
   `needs: [benchmarks, benchmarks-macos-x86]` while keeping
   `if: ${{ !cancelled() && needs.benchmarks.result == 'success' }}` — the
   exact pattern `package-libs-collect` uses (lines 1236–1239) so per-push runs
   (where the Intel leg is skipped) still compare the three per-push reports.
   The download step already uses `pattern: bench-report-*`, so no change
   there.

**Code.** ci.yml only; ~30 added lines, no C or make changes.

**Tests.** No local surface (needs a GitHub-hosted Intel mac — a hardware
gate). Verification is a `workflow_dispatch` run of CI on the pushed branch:
the new job must go green with `bench-golden-check: OK (7 ...)` in its log and
the compare job's step summary must show a `gh-macos-15-intel` row in the
Systems table (real-time unit `cyc`, feature grid showing the single-step tier
available — macOS-Intel is a single-step host).

**Docs.** Extend the CI bullet in
[docs/reference/ci.md](../../../docs/reference/ci.md) (lines 25–30) with "plus
nightly `macos-15-intel` and per-push `windows-latest` legs" once T3 also
lands; note the leg in the guide's "In your CI" section together with T4 (the
guide edit is owned by T4 to avoid three sequential edits of one paragraph).

**Done when.**

- `gh workflow run CI` (or the Actions UI dispatch) shows
  `benchmarks (macos-15-intel, nightly)` green, artifact
  `bench-report-macos-15-intel` present.
- A plain push run does NOT run the job (event gate), and
  `benchmarks-compare` still succeeds with three reports.
- The uploaded report's `system.box_id` is `gh-macos-15-intel` and
  `system.os` is `macos`, `system.arch` `x86_64`.

### T2 — Build the Windows benchmark producers (mk/win64.mk + runner overrides)  (M, depends on: none)

**Goal.** `emu-bench` and the bench-report runner work on the Windows/mingw
toolchain: the golden gate and a full per-system report run on a real Windows
host, and the deterministic producer is regression-tested in-tree under Wine.

**Steps.**

1. **Producer overrides in the runner.** Edit
   [scripts/bench-report.sh](../../../scripts/bench-report.sh) lines 102–105:
   introduce `TEST_BENCH="${TEST_BENCH:-$BUILD/test_bench}"`,
   `EMU_BENCH="${EMU_BENCH:-$BUILD/emu-bench}"`,
   `ASMFEATURES="${ASMFEATURES:-$BUILD/asmfeatures}"` and invoke those.
2. **Absent-probe row, not a crash.** When `$ASMFEATURES` is not an executable
   file, do not fail — write a one-row features fragment instead:

   ```json
   {"features": [{"tier": "probe", "backend": "asmfeatures", "arch": "x86_64",
     "scope": "host", "available": false,
     "skip_reason": "asmfeatures links the POSIX native-trace objects; not built on this OS",
     "fidelity": ""}]}
   ```

   This is the house rule — absent capability is data with a reason, and the
   report schema keeps its `features` array on every OS. (The full
   asmfeatures/VEH port is out of scope — see the last section.)
3. **mk/win64.mk rules.** Following the variable style at lines 17–21, add:

   ```make
   WIN64_UNICORN_CFLAGS  ?= $(shell pkg-config --cflags unicorn 2>/dev/null)
   WIN64_UNICORN_LIBS    ?= $(shell pkg-config --libs unicorn 2>/dev/null || echo -lunicorn)
   WIN64_CAPSTONE_CFLAGS ?= $(shell pkg-config --cflags capstone 2>/dev/null)
   WIN64_CAPSTONE_LIBS   ?= $(shell pkg-config --libs capstone 2>/dev/null)
   WIN64_CAPSTONE_DEF    ?= $(if $(WIN64_CAPSTONE_LIBS),-DASMTEST_HAVE_CAPSTONE,)
   ```

   then three phony targets mirroring `win64-runner-test` (lines 134–146):
   - `win64-emu-bench`: `$(WIN64_CC) -O2 -Wall -Iinclude -Itools` over
     `tools/emu_bench.c src/emu.c src/trace.c src/disasm.c` with the unicorn +
     capstone flags/defs above, to `$(WIN64_BUILD)/emu-bench.exe`.
     `CAPSTONE_DEF` matters: the main Makefile passes
     `-DASMTEST_HAVE_CAPSTONE` (Makefile line 789) and disasm.c degrades to
     stubs without it.
   - `win64-bench-check`: run `$(WINE) $(WIN64_BUILD)/emu-bench.exe
     --format=json > $(WIN64_BUILD)/emu-insns.fresh.json` then
     `python3 scripts/bench-golden-check.py benchmarks/golden/emu-insns.json
     $(WIN64_BUILD)/emu-insns.fresh.json` — the `WINE=` convention (empty on
     real Windows) is exactly `win64-runner-test`'s.
   - `win64-bench-report`: build `suite_win64.exe` (reuse the
     `win64-runner-test` compile line as a `$(WIN64_BUILD)/suite_win64.exe`
     file target so both share it) and `emu-bench.exe`, then run
     `BUILD=$(BUILD) TEST_BENCH=$(WIN64_BUILD)/suite_win64.exe
     EMU_BENCH=$(WIN64_BUILD)/emu-bench.exe
     ASMFEATURES=$(WIN64_BUILD)/asmfeatures.exe scripts/bench-report.sh` (the
     asmfeatures path intentionally names a file that does not exist, taking
     the step-2 branch).
4. **Wine test lane (the in-tree regression test).** Extend
   [Dockerfile.win64](../../../Dockerfile.win64) with mingw **cross builds of
   the pinned engines** so the producer is testable with no Windows host,
   per the repo's missing-dependency rule:
   - `ARG UNICORN_VERSION=2.1.4`; curl the GitHub release tarball, verify its
     SHA-256 against a new `tarball-sha256 unicorn 2.1.4 sha256:...` line in
     [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt)
     (generate with
     [scripts/refresh-thirdparty-digests.sh](../../../scripts/refresh-thirdparty-digests.sh));
     build static with
     `cmake -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc -DBUILD_SHARED_LIBS=OFF`,
     install to `/opt/win64-deps`.
   - Capstone from the already-pinned 5.0.1 commit
     (`097c04d9413c59a58b00d4d1c8d5dc0ac158ffaa`, same digests file), same
     cross-cmake shape, static, same prefix. Static linking avoids DLL-path
     issues under Wine.
   - Add `make docker-win64-bench` in mk/win64.mk (copy the `docker-win64`
     rule shape, lines 220–224) running
     `make win64-bench-check WIN64_UNICORN_CFLAGS=-I/opt/win64-deps/include
     WIN64_UNICORN_LIBS='-L/opt/win64-deps/lib -lunicorn' ...` (plus the
     capstone pair and `WIN64_CAPSTONE_DEF=-DASMTEST_HAVE_CAPSTONE`).
5. `make fmt` (no C sources change here beyond none — shell/make only, but run
   it anyway; it is the CI-gated habit), then `make docker-win64-bench`.

**Code.** `scripts/bench-report.sh` (~15 lines), `mk/win64.mk` (~45 lines),
`Dockerfile.win64` (~30 lines), one digest line.

**Tests.** `make docker-win64-bench` is the test: it fails red if the mingw
compile of emu.c/trace.c/disasm.c breaks, and its golden check proves the
Windows binary reproduces the committed counts — the first executable proof of
the "host- and OS-independent" claim on a PE. Pass looks like
`bench-golden-check: OK (7 deterministic count rows match)`; a drift failure
prints the per-row `golden=… fresh=…` diff. Also run `make win64-bench-report`
inside the image is NOT meaningful (the container is Linux, `uname` says
`Linux`, so the descriptor would be a Linux one) — the full-report path is
exercised on the real runner in T3; say so in the target's comment.

**Docs.** Add `win64-bench-check` / `docker-win64-bench` to the win64 target
list comment at the top of mk/win64.mk and to `make help`'s optional-tier
section (Makefile, near line 110). CHANGELOG under T3 (one entry for the whole
Windows leg).

**Done when.**

- `make docker-win64-bench` exits 0 with the golden-OK line on a Linux x86-64
  host (the lane is x86-64-only like `docker-win64`; under linux/arm64 the PE
  cannot run under Wine — keep that comment).
- `sh scripts/check-thirdparty-versions.sh` still passes (it cross-checks
  pinned versions; the new unicorn pin must not break it — extend it if it
  flags the new line).
- `scripts/bench-report.sh` still behaves identically on Linux with no env
  overrides (`make docker-bench` unchanged output shape).

### T3 — Add the per-push `benchmarks-windows` leg (windows-latest)  (S, depends on: T2)

**Goal.** Every push runs the golden gate and produces
`bench-report-windows-latest` on real Windows.

**Steps.**

1. In ci.yml, after `benchmarks-macos-x86`, add `benchmarks-windows`:
   `runs-on: windows-latest`, `timeout-minutes: 20`, per-push (no event gate —
   Windows runners are plentiful, unlike Intel macs), job env
   `ASMTEST_BOX_ID: gh-windows-latest`, and `defaults: run: shell: msys2 {0}`
   copied from the `windows` job (lines 1008–1010).
2. Steps: checkout@v7; `msys2/setup-msys2@v2` with `msystem: MINGW64`,
   `update: true`, `install: make git coreutils mingw-w64-x86_64-gcc
   mingw-w64-x86_64-unicorn mingw-w64-x86_64-capstone mingw-w64-x86_64-python
   mingw-w64-x86_64-pkgconf` (the existing `windows` job installs `make nasm
   coreutils mingw-w64-x86_64-gcc`; nasm is not needed here — emu-bench has no
   .asm objects, and `suite_win64.exe` does need nasm, so ADD `nasm` if you
   reuse the shared `suite_win64.exe` target — check the target's prerequisite
   list you wrote in T2 and match it). `git` and `python` are needed by
   bench-report.sh (`git rev-parse`, the python3 merge); MSYS2's minimal PATH
   does not see the Windows-native Python.
3. A "Toolchain + engine versions" step: `pacman -Q mingw-w64-x86_64-unicorn
   mingw-w64-x86_64-capstone` — MSYS2 is a rolling repo that cannot pin, so
   the run log must record what was actually poured (see Constraints).
4. `make win64-bench-check WIN64_CC=gcc WINE=` then
   `make win64-bench-report WIN64_CC=gcc WINE=` then upload
   `build/bench-report-*.json` as `bench-report-windows-latest`
   (`if-no-files-found: error`).
5. Add `benchmarks-windows` to `benchmarks-compare`'s `needs` list (same
   pattern as T1 step 3 — the `if` still keys only off `needs.benchmarks`).
6. Rewrite the stale follow-ups comment at ci.yml lines 74–76: the two legs
   are no longer follow-ups; leave only the nightly-auto-commit sentence for
   T4 to delete.

**Code.** ci.yml only.

**Tests.** CI-side: push the branch, the job must go green with the golden-OK
line (7 rows — proving the committed golden on a real Windows kernel), and the
report artifact must show `system.os: "windows"` (the `MINGW*` descriptor
branch, bench-report.sh lines 54–58), unit `cyc`, one native bench row
(`win64.ret_arg0`), 7 emulated rows, and the single `available:false`
asmfeatures probe row from T2. Local pre-flight: `make docker-win64-bench`
(T2) is the closest proxy.

**Docs.** [docs/reference/ci.md](../../../docs/reference/ci.md) benchmarks
bullet (with T1). CHANGELOG `### Added`: "Cross-system benchmark legs on real
Windows (`windows-latest`, per push) and Intel macOS (`macos-15-intel`,
nightly); the deterministic golden gate now runs on every OS the framework
targets."

**Done when.**

- Per-push CI shows `benchmarks-windows` green; `benchmarks-compare`'s summary
  table gains a `gh-windows-latest` row with `cyc` real-time cells and the
  feature grid shows the probe-absent row (`✗ asmfeatures links the POSIX…`).
- `make bench-check` semantics are untouched on POSIX legs.

### T4 — Nightly auto-commit of per-box records into benchmarks/  (M, depends on: T1, T3)

**Goal.** On the nightly schedule (and manual dispatch), every benchmarks leg
persists its box record and one job commits `benchmarks/boxes/**` back to
`main` as `github-actions[bot]`, with no risk of recursive CI storms.

**Steps.**

1. **Record on schedule, per leg.** In each bench job (`benchmarks` matrix,
   `benchmarks-macos-x86`, `benchmarks-windows`), append two steps gated
   `if: github.event_name == 'schedule' || github.event_name ==
   'workflow_dispatch'`:
   - `make bench-record` (POSIX legs) / `make win64-bench-record WIN64_CC=gcc
     WINE=` (add this thin alias in mk/win64.mk: `win64-bench-report` with
     `--record` — mirror how mk/bench.mk lines 60–63 pass `--record`). With
     `ASMTEST_BOX_ID` set (T1/T3) the records land under
     `benchmarks/boxes/gh-<runner-label>/`.
   - upload-artifact `bench-boxes-<label>` with `path: benchmarks/boxes/`,
     `if-no-files-found: error`.
2. **The commit job.** New job `benchmarks-record`:

   ```yaml
   benchmarks-record:
     name: benchmarks (nightly record commit)
     if: github.event_name == 'schedule' || github.event_name == 'workflow_dispatch'
     needs: [benchmarks, benchmarks-macos-x86, benchmarks-windows]
     runs-on: ubuntu-latest
     timeout-minutes: 10
     permissions:
       contents: write
   ```

   Steps: checkout@v7 (default `persist-credentials` keeps the token in git
   config, so plain `git push` works); download-artifact@v8 `pattern:
   bench-boxes-*`, `path: benchmarks/boxes/`, `merge-multiple: true`; a
   retention step trimming every `benchmarks/boxes/*/perf-history.jsonl` to
   its last 200 lines (`tail -n 200` into a temp file and move back — the plan
   §6.3's bounded-growth rule); then:

   ```sh
   git config user.name  "github-actions[bot]"
   git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
   git add benchmarks/boxes
   git diff --cached --quiet && { echo "no record changes"; exit 0; }
   git commit -m "chore(benchmarks): nightly per-box records [skip ci]"
   git push || { git pull --rebase origin main && git push; }
   ```

3. **What is deliberately NOT committed:** `benchmarks/golden/emu-insns.json`.
   Golden drift is a reviewed, intended code change (every leg's `bench-check`
   would already have failed the run before this job); auto-committing it
   would launder a regression into history. State this in the job comment.
4. **Permissions scoping.** Setting `permissions:` on this job sets all
   unspecified scopes to `none` **for this job only** — other jobs keep the
   repo default. Do not add a workflow-level block (it would silently drop
   scopes for every job).
5. Delete the now-fully-stale follow-ups sentence from the ci.yml comment
   (T3 step 6 left it) and refresh the job-group comment (lines 68–76) to
   describe the five legs + nightly record.

**Code.** ci.yml (~55 lines), mk/win64.mk (~4 lines).

**Tests.** No local surface (credentials + hosted runners are the gate).
Verification sequence, in order:
1. `workflow_dispatch` the branch: the record job must run, and on a branch
   push the `git push` goes to that branch's ref via the checkout default —
   confirm the commit lands on the PR branch with author
   `github-actions[bot]` and touches only `benchmarks/boxes/**`.
2. After merge, the first real nightly must produce one commit on `main` with
   five box dirs (`gh-ubuntu-latest`, `gh-ubuntu-24.04-arm`,
   `gh-macos-latest`, `gh-macos-15-intel`, `gh-windows-latest`), each
   `perf-history.jsonl` one line longer.
3. Confirm no CI run was triggered by that commit (GITHUB_TOKEN pushes do not
   create workflow runs; the `[skip ci]` trailer is belt-and-braces).

**Docs.** Update the guide's "In your CI" closing paragraph
([docs/guides/cross-system-benchmarking.md](../../../docs/guides/cross-system-benchmarking.md)
lines 220–227): the Windows/Intel-macOS legs and the nightly auto-commit are
no longer "tracked follow-ups" — describe the five legs, the nightly record
commit, and that golden regeneration remains a human PR. Update
[docs/reference/ci.md](../../../docs/reference/ci.md) accordingly. CHANGELOG
`### Added`: "Nightly auto-commit of per-box benchmark records
(`benchmarks/boxes/**`) by github-actions[bot]; golden files stay
human-reviewed." `make docs` must build clean (`-W`).

**Done when.**

- Nightly commit appears on `main` as above; re-running the job with no new
  data prints `no record changes` and exits 0.
- `python3 scripts/bench-compare` (no args) over the committed tree now shows
  the five `gh-*` boxes.
- If branch protection ever blocks the bot, the job fails loudly at `git push`
  (recorded constraint below), not silently.

### T5 — Instruction-class helper `asmtest_disas_class`  (S, depends on: none)

**Goal.** One Capstone-backed classifier maps any executed instruction of any
guest ISA to {OTHER, MEM, BRANCH, MULDIV} so the model-cost producer (T6) can
weight it.

**Steps.**

1. In [include/asmtest_trace.h](../../../include/asmtest_trace.h), after the
   `asmtest_disas_is_*` block (lines 176–249), declare:

   ```c
   typedef enum {
       ASMTEST_INSN_OTHER = 0,  /* default: 1-weight ALU/move/etc.        */
       ASMTEST_INSN_MEM = 1,    /* any explicit memory operand            */
       ASMTEST_INSN_BRANCH = 2, /* JUMP/CALL/RET/IRET groups              */
       ASMTEST_INSN_MULDIV = 3, /* multiply/divide (mnemonic-matched)     */
   } asmtest_insn_class_t;

   asmtest_insn_class_t asmtest_disas_class(asmtest_arch_t arch,
                                            const uint8_t *code,
                                            size_t code_len, uint64_t off);
   ```

2. Implement in [src/disasm.c](../../../src/disasm.c), mirroring
   `asmtest_disas_is_call` (lines 142–172): `cs_target()` → `cs_open` →
   `cs_option(h, CS_OPT_DETAIL, CS_OPT_ON)` → `cs_disasm(..., 1, &insn)` →
   classify → `cs_free`/`cs_close`. Without Capstone (`#else` arm) return
   `ASMTEST_INSN_OTHER`. Classification precedence (document it in the header
   comment — an instruction can match several):
   - BRANCH if `cs_insn_group` matches `CS_GRP_JUMP`/`CS_GRP_CALL`/
     `CS_GRP_RET`/`CS_GRP_IRET` (note the existing caveat at disasm.c line 67:
     some opcodes file under `CS_GRP_JUMP` on older Capstones — grouping only,
     no id-splitting needed here);
   - else MULDIV if the mnemonic contains `mul` or `div`
     (`strstr(insn->mnemonic, ...)`) — deliberately a *model-grade* string
     match that covers `mul/imul/umull/smull/div/sdiv/udiv/divss...` across
     all four ISAs without four id tables; say so in the comment;
   - else MEM if any operand is a memory operand — per-arch detail walk
     exactly like [src/dataflow_operands.c](../../../src/dataflow_operands.c)
     (`X86_OP_MEM` at line 145, `ARM64_OP_MEM` at line 171; add the RISCV/ARM
     equivalents the same way);
   - else OTHER.
3. `make fmt`, then rebuild: `make WERROR=1 test` (disasm.o is in the core
   link) must stay green.

**Code.** ~70 lines C.

**Tests.** Extend the existing emulator/disassembler suite:
[examples/test_emu.c](../../../examples/test_emu.c) already exercises
`asmtest_disas_*` helpers; add a `TEST(disas, class_x86)` (and an arm64
sibling) asserting over `FIX_X86_SUMTON`
([tools/asmbench_fixtures.h](../../../tools/asmbench_fixtures.h) line 42:
`xor eax,eax; add rax,rdi; dec rdi; jnz L; ret`): offset of `jnz` and `ret` →
BRANCH, `xor/add/dec` → OTHER; and a hand-written `mul` byte pair → MULDIV;
and a `mov rax,[rdi]` → MEM. Run via `make emu-test` (Capstone present in
that lane); without Capstone the assertions must flip to expecting OTHER —
gate them on `asmtest_disas_available()` the way neighbouring disas tests do.
Failure looks like a normal `not ok`/assert diff from the suite; pass is the
suite's `ok` lines.

**Docs.** Internal-only at this step (the user-facing metric doc lands with
T6). The header comment IS the contract — write it fully.

**Done when.**

- `make emu-test` green with the new cases on a Capstone host;
  `make docker-test` (no Capstone) still green (degrade path).
- `make check-bindings-parity` still passes — if it flags the new public
  symbol as unwrapped, add it to
  [scripts/bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt)
  with a comment (it is a reporter-internal helper, not binding surface), or
  wrap it; do not skip the gate.

### T6 — Emit BM_MODEL_COST and teach the gate + reporter about row kinds  (M, depends on: T5)

**Goal.** `emu-bench` emits a deterministic Capstone-weighted `model_cost` row
per case, the golden gate ignores non-`insns` kinds, and `bench-compare`
renders a separate model-cost matrix — everything explicitly labelled a model.

**Steps.**

1. **Producer.** In [tools/emu_bench.c](../../../tools/emu_bench.c):
   - Change `asmtest_trace_new(0, 256)` (line 96) to
     `asmtest_trace_new(4096, 256)` so per-instruction offsets are recorded
     (`sum_to_100` executes 302; 4096 is headroom — add a `_Static_assert`-style
     comment tying the cap to the largest fixture).
   - After a successful run, when `asmtest_disas_available()`, walk
     `asmtest_emu_trace_insn_at(t, i)` for `i < insns_len`, classify each
     offset with `asmtest_disas_class` (memoize per offset in a small array —
     fixtures are ≤ 32 bytes, loop offsets repeat), and sum weights from one
     static table:

     ```c
     /* THE MODEL — comparable across ISAs by construction, not silicon.
      * Keep in sync with docs/guides/cross-system-benchmarking.md. */
     static const double MODEL_W[] = {
         [ASMTEST_INSN_OTHER] = 1.0, [ASMTEST_INSN_MEM] = 3.0,
         [ASMTEST_INSN_BRANCH] = 2.0, [ASMTEST_INSN_MULDIV] = 8.0};
     ```

   - Map the guest to the disas arch: `G_X86`/`G_WIN64` →
     `ASMTEST_ARCH_X86_64`, `G_ARM64` → `ASMTEST_ARCH_ARM64`, `G_RISCV` →
     `ASMTEST_ARCH_RISCV64`, `G_ARM32` → `ASMTEST_ARCH_ARM32`.
   - JSON: after each `insns` row, emit a sibling
     `{"name": ..., "arch": ..., "kind": "model_cost", "value": <sum>,
     "unit": "model-cyc", "deterministic": true, "complete": <same>}` —
     `BM_MODEL_COST` in the plan's naming, kind string `model_cost` on the
     wire. Text mode: add a `model-cyc` column, `-` when Capstone is absent.
   - When the trace truncated (`insns_len < insns_total`), mark the model row
     `"complete": false` and compute over the recorded prefix — mirrors the
     `complete` discipline everywhere else.
2. **Golden gate.** In
   [scripts/bench-golden-check.py](../../../scripts/bench-golden-check.py)
   `key_rows` (line 16), filter to `b.get("kind", "insns") == "insns"` in both
   golden and fresh. Rationale (put it in the docstring): model rows exist
   only where Capstone is linked and their values depend on the Capstone
   version's decode (repo pin 5.0.1; MSYS2 pours 5.0.9), so they are
   deterministic *per build* but not "one file identical on every leg"
   material. Also add the anti-vacuity check: when the fresh doc contains ANY
   `model_cost` row, every `insns` row must have a model sibling — a missing
   sibling means the producer half-ran; exit 1 with a named error.
3. **Reporter.** In [scripts/bench-compare](../../../scripts/bench-compare)
   line 102's loop, take only `kind in (None, "insns")` for the insn matrix
   (None keeps the committed-golden pseudo-report path working), and collect
   `kind == "model_cost"` into a second matrix emitted as
   `## Model cost (model-cyc/call — a MODEL, not silicon)` with a caption
   naming the weights and pointing at the guide. Never mix the two units in
   one table (the house rule).
4. **Make the rows real in CI.** The Linux/macOS `benchmarks` legs currently
   install unicorn but not Capstone
   ([scripts/install-deps.sh](../../../scripts/install-deps.sh) `--emu` leaves
   `capstone_pkg` empty and points at the pinned source build, lines 147–153).
   Add to the `benchmarks` job and `benchmarks-macos-x86` the K1 cached pinned
   Capstone build — copy the two steps verbatim from the `emu` job (ci.yml
   lines 285–292: `actions/cache@v6` on `~/.cache/asmtest-thirdparty/capstone`
   + `sh scripts/thirdparty-cache.sh cached-build capstone`). The Windows leg
   already gets capstone from pacman (T3).
5. Regenerate nothing: `benchmarks/golden/emu-insns.json` is untouched by
   design (step 2 filters). Run `make bench-check` to prove it.

**Code.** `tools/emu_bench.c` (~60 lines), two scripts (~25 lines), ci.yml
(2 × 2 steps).

**Tests.**
- `make bench-check` on a Capstone host: still `OK (7 ...)` (filter works),
  and force-failure check: temporarily delete one model row from the fresh
  file and re-run the script by hand — must exit 1 with the missing-sibling
  error.
- `make emu-bench` text output shows the model column; sanity-check one value
  by hand: `math.add3` x86-64 = `mov+add+add+ret` = OTHER+OTHER+OTHER+BRANCH
  = 1+1+1+2 = **5.0**; `arm64` = add+add+ret = 1+1+2 = **4.0** (the RISC forms
  drop the move — the cross-ISA story the fixture header documents).
- `python3 scripts/bench-compare build/bench-report-linux-x86_64.json` after a
  fresh `make bench-report` shows both matrices, values matching the hand
  calculation.
- CI: the `benchmarks` legs' reports now carry model rows and the compare
  summary shows the model matrix.

**Docs.** New section in
[docs/guides/cross-system-benchmarking.md](../../../docs/guides/cross-system-benchmarking.md)
"The weighted model-cost metric (`BM_MODEL_COST`)": the weight table, the
classification precedence, the explicit "comparable by construction; a model,
not silicon" caveat from the analysis §4.3, the Capstone requirement and what
happens without it, and one schema example row (`kind: "model_cost"`, unit
`model-cyc`). Add a cross-reference sentence to
[docs/guides/benchmarks.md](../../../docs/guides/benchmarks.md)'s comparing
section. CHANGELOG `### Added`. `make docs` clean.

**Done when.**

- `make emu-bench`, `make bench-check`, `make bench-report`,
  `python3 scripts/bench-compare <report>` all green locally with the
  hand-verified values above.
- `make docker-bench` output carries model rows (the CI image has Capstone).
- A no-Capstone build (`make docker-win64-bench` before its capstone flags, or
  a bare host) emits NO model rows and every gate still passes — absence is
  data, not failure.

## Task order & parallelism

- **Independent starts:** T1, T2, and T5 touch disjoint files — three people
  can start concurrently.
- **Ordered:** T2 → T3 → T4 (the Windows producers, then the leg, then the
  recorder that needs all legs); T5 → T6.
- **Cross-chain:** T4 also wants T1 (the Intel leg's record); T6 step 4 edits
  the same ci.yml jobs as T1/T3 — land T6's CI step after T1/T3 or rebase.
- **Critical path:** T2 → T3 → T4 (the only chain with two M tasks).

```
T1 ────────────┐
T2 → T3 ───────┼→ T4
T5 → T6 (ci.yml edit after T1/T3)
```

## Constraints & gates

- **Real gates (record + self-skip / CI-side validation):** no local Intel
  macOS or Windows hardware exists in this environment — T1/T3/T4's runtime
  behavior is validated only via `workflow_dispatch` runs on the branch
  (schedule-gated jobs also honor dispatch, ci.yml line 20's concurrency
  grouping already isolates them). Record the dispatch run URL in the PR.
- **Branch protection:** it was NOT verified whether `GITHUB_TOKEN` can push
  to a protected `main`. If protection is on, the T4 push fails — exempt the
  bot or relax protection deliberately; do not swap in a PAT silently.
- **Pinning:** unicorn 2.1.4 and capstone 5.0.1 cross-builds must verify
  against [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt)
  (T2). Two ecosystems structurally cannot honor the repo pins: Homebrew has
  no versioned unicorn/capstone formulae, and MSYS2 is a rolling repo (exact
  versions need archived package URLs). Mitigation: the pinned-source builds
  stay canonical wherever they run (Linux/macOS/Docker-Wine); the
  brew/pacman-fed legs log their poured versions (T3 step 3), and no
  golden-gated value depends on Capstone (T6 step 2 keeps model rows out of
  the golden file).
- **Homebrew Intel timeline:** macOS Intel drops to Tier 3 (no new bottles,
  no CI) possibly ~Sept 2026 — the T1 leg's `brew install unicorn` may then
  need the pinned-source route or a tap; the leg must fail loudly, not
  self-skip, when that day comes (deps failure fails the step).
- **Schedule caveats:** public-repo schedules auto-disable after 60 days of
  repo inactivity, runs can be delayed under load, and they run on the default
  branch's latest commit — the T4 rebase-retry handles the resulting races.
- **License:** no new runtime dependency beyond the already-shipped engines;
  unicorn is GPLv2 — it is cross-built into a test image only (Dockerfile.win64),
  never redistributed, matching how every other lane consumes it.

## Research notes (verified 2026-07-17)

- **Runner labels.** `windows-latest` = windows-2025 (x64, 4 vCPU);
  `macos-latest` = macos-15 on arm64 M1 (since Aug 2025,
  <https://github.com/actions/runner-images/issues/12520>);
  `macos-15-intel` = macOS 15 x64, introduced 2025-09-19, supported through
  Aug 2027 (<https://github.com/actions/runner-images/issues/13045>), actively
  maintained (release `macos-15/20260715.0340`, 2026-07-15,
  <https://api.github.com/repos/actions/runner-images/releases>) — macOS 15.7.7,
  Homebrew 6.0.11, **no nasm preinstalled**
  (<https://raw.githubusercontent.com/actions/runner-images/main/images/macos/macos-15-Readme.md>).
  The README also lists `macos-26-intel` (x64, GA) — Intel was extended to
  macOS 26. `macos-13`/`macos-14` are retired/deprecated
  (<https://docs.github.com/en/actions/reference/runners/github-hosted-runners>,
  <https://github.com/actions/runner-images>).
- **brew on macos-15-intel.** unicorn 2.1.4, capstone 5.0.9, keystone 0.9.2,
  nasm 3.02 ship NO Intel-Sequoia bottles (bottle tags checked at
  <https://formulae.brew.sh/api/formula/unicorn.json>, `.../capstone.json`,
  `.../keystone.json`, `.../nasm.json>`); Intel Sequoia pours the Sonoma
  bottle as fallback (maintainer position,
  <https://github.com/orgs/Homebrew/discussions/6624>, fix
  <https://github.com/Homebrew/brew/pull/20802>), so
  `make deps DEPS_ARGS=--emu` works there today. Intel tier drops to Tier 3
  "September (or later) 2026", unsupported Sept 2027
  (<https://docs.brew.sh/Support-Tiers>,
  <https://brew.sh/2025/11/12/homebrew-5.0.0/>). Brew cannot pin versions (no
  versioned formulae).
- **Windows dep routes.** Chocolatey's community feed has NO unicorn/capstone
  library packages (OData search,
  <https://community.chocolatey.org/api/v2/>). Two workable, preinstalled
  routes on windows-2025
  (<https://raw.githubusercontent.com/actions/runner-images/main/images/windows/Windows2025-Readme.md>):
  vcpkg at `C:\vcpkg` (ports unicorn 2.1.4 — `"supports": "!(arm & windows)"`,
  so not on windows-11-arm — and capstone 5.0.9,
  <https://raw.githubusercontent.com/microsoft/vcpkg/master/ports/unicorn/vcpkg.json>,
  `.../capstone/vcpkg.json>`), and MSYS2 at `C:\msys64` with pacman packages
  `mingw-w64-unicorn 2.1.4-5`, `mingw-w64-capstone 5.0.9-1`,
  `mingw-w64-keystone 0.9.2-9`
  (<https://packages.msys2.org/api/search?query=unicorn>). **This doc chooses
  MSYS2** because the repo's `windows` job already lives in an MSYS2/MINGW64
  shell (ci.yml lines 1004–1021) and the mk/win64.mk toolchain is mingw.
  Version skew: ecosystem capstone is 5.0.9 vs repo pin 5.0.1; unicorn 2.1.4
  and keystone 0.9.2 match the repo's expectations.
- **Auto-commit pattern.** `permissions: contents: write` at job level;
  specifying any permission zeroes the unspecified scopes for that job
  (<https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax>,
  <https://docs.github.com/en/actions/tutorials/authenticate-with-github_token>).
  actions/checkout (current major v7, 2026-06-18) persists the token in git
  config by default, so plain `git push` works
  (<https://github.com/actions/checkout>). Bot identity:
  `github-actions[bot]`, verified uid 41898282
  (<https://api.github.com/users/github-actions%5Bbot%5D>) → email
  `41898282+github-actions[bot]@users.noreply.github.com`. Pushes made with
  `GITHUB_TOKEN` do not create new workflow runs (exceptions:
  `workflow_dispatch`/`repository_dispatch`) — the desired anti-recursion
  property here
  (<https://docs.github.com/en/actions/using-workflows/triggering-a-workflow>).
  Schedule caveats (60-day auto-disable, load delays, default-branch commit)
  per
  <https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows>.
  Unverified: whether new repos default `GITHUB_TOKEN` to read-only (docs only
  say the default follows the enterprise/org/repo setting) — hence the
  explicit `permissions` block is REQUIRED, and the protected-branch behavior
  noted under Constraints.

## Out of scope

- **Bare-metal / self-hosted legs** (AMD Zen, Intel PT hosts) —
  [self-hosted-ci-runners.md](self-hosted-ci-runners.md).
- **A Windows port of the native-trace feature probe** (asmfeatures with a
  VEH single-step backend, so the Windows report's feature grid shows real
  rows instead of the T2 probe-absent row). No sibling doc owns it; T2
  deliberately records the absence as data. Revisit only after a Windows
  hwtrace tier exists.
- **macOS clean-room / packaging lanes** touching the same runners —
  [macos-cleanroom-lanes.md](macos-cleanroom-lanes.md),
  [distribution-packaging.md](distribution-packaging.md).
- **The DynamoRIO macOS port's CI** (also `macos-15-intel`, same conflict
  resolution) — [macos-dynamorio-port.md](macos-dynamorio-port.md).
- **New guest ISAs in the fixture set** (e.g. RISC-V native tier work) —
  [riscv-native-tier.md](riscv-native-tier.md); this doc only weights and
  reports the fixtures that exist.
