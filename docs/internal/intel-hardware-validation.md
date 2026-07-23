# Intel PT hardware validation — the manual pre-release step

**What this is:** the one Intel-Processor-Trace validation step that cannot run
in CI. GitHub-hosted runners (and every VM/container without a bare-metal Intel
PMU) do not expose the `intel_pt` PMU with a usable AUX trace, so the live
whole-window capture, foreign-pid attach, per-tid hop, and F5 PT-replay paths
**self-skip** everywhere in the cloud. This page is the manual pre-release
procedure that runs those paths for real, on Intel silicon, and says what a green
run proves.

Like [amd-hardware-validation.md](amd-hardware-validation.md) this is a
**documented manual step**, not a CI gate: there is no bare-metal Intel PT box in
GitHub's hosted pool. A self-hosted Intel-PT runner would let a guarded `hw.yml`
job light these paths up (see [self-hosted-ci-runners.md](implementations/self-hosted-ci-runners.md));
until one exists a maintainer runs this by hand.

## Why CI can't do this

The `intel_pt` PMU is exposed to a container (the tier's `hw_classify` reports
`INTEL_PT` present), but `perf_event_open` for the AUX area is gated on
`perf_event_paranoid` / `CAP_PERFMON`, and — the load-bearing part — a real AUX
trace with control-flow (COFI) packets only comes from **bare-metal Intel PT
silicon**. On an AMD box, a VM, or a hypervisor that partially virtualizes PT you
either get no PMU or a heartbeat-only stream, so the live tests self-skip.

| Lane | Where | Proves |
|---|---|---|
| `docker-hwtrace` (plain) | any host | the tier's detect-and-skip stays green; PT capture self-skips with the permission/PMU reason; the **synthetic** PT-fixture decode (`test_wholewindow_decode`, `pt hop` synthetic) runs on every host. |
| **this doc** — `make hwtrace-pt-live` / `make dataflow-pt-live` on a PT box | a real bare-metal Intel PT host + `CAP_PERFMON` (or `perf_event_paranoid<0`) | the live paths CI can't reach: whole-window self-capture + libipt decode, capture-side address filtering, the anon-region decode-time fallback, AUX-ring truncation, foreign-pid attach capture, and F5 PT-replay matching the single-step oracle with zero single-steps. |

`make hwtrace-pt-live` sets `ASMTEST_REQUIRE_PT=1` so it **fails rather than
skips** where a runner claims PT but the PMU is hidden. It has no Docker target of
its own, but it runs cleanly inside the `asmtest-hwtrace` image with
`--cap-add=PERFMON` (the PMU is visible in-container; CAP_PERFMON bypasses
`perf_event_paranoid`). Likewise `make dataflow-pt-live` inside
`asmtest-dataflow-attach`.

## Checklist (leave blank — a template)

- [ ] Bare-metal Intel host with `intel_pt` in `/sys/bus/event_source/devices/`.
      Record `cat .../intel_pt/nr_addr_filters` (address-filter slots) and
      `perf_event_paranoid`.
- [ ] `docker run --rm --cap-add=PERFMON asmtest-hwtrace make hwtrace-pt-live` →
      `# N passed, 0 failed`, exit 0, **no** `# SKIP pt live …` lines. Record the
      accepted address-filter form (`# pt live: address-filter form accepted = …`)
      and `# pt nr_addr_filters: N`.
- [ ] `docker run --rm --cap-add=PERFMON --security-opt seccomp=unconfined
      asmtest-dataflow-attach make dataflow-pt-live` → `# all N checks passed`,
      exit 0, with the T4 `live(…)` cases running (not skipped) and the F5 replay
      value trace **byte-identical to the emulator L0 (zero single-steps)**.
- [ ] Sanity: the same host, plain `docker-hwtrace` (no caps) still self-skips PT
      cleanly and stays green (no regression to the non-PT path).
- [ ] `.NET` managed compose PT prongs (T5/T10/T11): the `asmtest-dotnet` image
      carries `libipt-dev` (since `4cf5d17`), so run
      `docker run --rm --cap-add=PERFMON -e DOTNET_CLI_TELEMETRY_OPTOUT=1 -e
      DOTNET_NOLOGO=1 asmtest-dotnet make hwtrace-dotnet-test` ≥5× → every run
      PASS with `AsmTrace(IntelPt): armed on Intel PT silicon` and
      `ambient: >=2 stitched slices captured` (a capture, not a self-skip), plus
      `make docker-hwtrace-dotnet-ambient-stress` → `1..N`, 0 `not ok`, the
      ambient set captured live on every iteration.

## Recorded runs

The checklist above is a reusable template — leave its boxes blank. Record each
observed run here (host, date, the values each item asks for), newest first.

### 2026-07-23 — Intel Core i7-8559U, first CI-lane run (`hw.yml` `hwtrace-pt-baremetal`)

Same box and kernel as the entry below, but this time **not a by-hand run**: the
[self-hosted-ci-runners.md](implementations/self-hosted-ci-runners.md) T5 lane
executed via a registered ephemeral `intel-pt` runner —
[run 29997961188](https://github.com/wilvk/asm-test/actions/runs/29997961188),
dispatched at `main` `ac3b521`. `make docker-hwtrace-privileged` →
`# 649 passed, 0 failed` (PT live `ok 157-167`, `# pt live: address-filter form
accepted = object-relative`, `# pt nr_addr_filters: 2`), then the fail-not-skip
`make docker-hwtrace-pt-live` (`ASMTEST_REQUIRE_PT=1`) → `1..644`,
`# 644 passed, 0 failed`, PMU node printed in-log. Registration/power-down
record: [ci/runners.md](ci/runners.md) status table. The "until one exists a
maintainer runs this by hand" caveat at the top of this page is now half-retired:
the lane exists and is proven; unattended nightly coverage still needs a
standing runner.

### 2026-07-23 — Intel Core i7-8559U, independent validating agent (post-concurrency-fix ✅ run)

Same physical box as the 2026-07-21 entry, now on kernel `7.1.4-1-t2-noble`,
`perf_event_paranoid=2`, `intel_pt` PMU `type=10`, `nr_addr_filters=2`, bare
metal (no `hypervisor` flag), Docker via `--cap-add=PERFMON`. Validated at clean
`main` `4cf5d17` (the code-image use-after-free fix,
[dotnet-managed-pt-concurrency-plan.md](archive/plans/dotnet-managed-pt-concurrency-plan.md))
by an agent independent of that fix's implementer — this run stamps
[intel-pt-whole-window-substrate.md](implementations/intel-pt-whole-window-substrate.md)
and [managed-wholewindow-compose.md](implementations/managed-wholewindow-compose.md)
`✅`, closing the 2026-07-21 entry's recorded blocker (the managed
multi-threaded live-PT race). All runs exit 0, 0 `not ok`:

- **`make docker-hwtrace-pt-live`** ×3 → `1..644`, **`# 644 passed, 0 failed`**
  every run, ZERO PT skips (the only `# SKIP`s are AMD/MSR/stealth-helper
  gates). In-container PMU visible (`type` reads 10). `# pt live: address-filter
  form accepted = object-relative`, `# pt nr_addr_filters: 2`; taken/not-taken
  TNT walks, the 4 KiB-ring truncation signal, capture-side file filter and the
  anon-region decode-time fallback all asserted live.
- **`.NET` live PT suite** — `docker run --rm --cap-add=PERFMON … asmtest-dotnet
  make hwtrace-dotnet-test` ×5 → **5/5 PASS**, `1..229`, 0 `not ok`, **zero
  crashes** (pre-fix this SIGSEGV'd 7/7). Every run: `ok AsmTrace(IntelPt):
  armed on Intel PT silicon — captured 867041 instructions` (T4 inline ctor,
  window EXACT) and `ok ambient: >=2 stitched slices captured (3)` — the T10
  per-tid hop capture + T11 ambient chain genuinely live, not self-skipping.
- **`make docker-hwtrace-dotnet-ambient-stress`** → `1..576`, 0 `not ok`,
  exit 0; the concurrent ambient/stitched set captured **live on 25/25
  iterations** (zero ambient skips).
- **`docker run --rm --cap-add=PERFMON --security-opt seccomp=unconfined
  asmtest-dataflow-attach make dataflow-pt-live`** ×3 → `1..29`, **`# all 29
  checks passed`** each; `live(20,22)`/`live(200,1)` foreign-pid captures ran,
  `PT-decoded path == single-step oracle path`, F5 replay byte-identical to the
  emulator L0 (zero single-steps).
- **Sanity (no caps):** `make docker-hwtrace` green (C `# 638 passed, 0
  failed`, python 26 passed/1 skipped) with PT self-skipping cleanly; plain
  `make docker-hwtrace-dotnet` `1..229`, 0 `not ok`, the PT prong self-skipping
  on **permission** (`perf_event capture not permitted (lower
  perf_event_paranoid or grant CAP_PERFMON)`), not a missing lib — the
  libipt-in-image contract holds unprivileged lanes harmless.
- **Hygiene:** `make docker-fmt-check` + `make docker-docs` clean; host-side
  `make check-bindings-parity` **OK — 142 tier symbols × 10 bindings** (run on
  the host, per the recorded dubious-ownership gotcha).
- **Honest residue (recorded, not hidden):** on this box `MethodsObserved` is 0
  consistently, so the `unwarmed/PT compose: >=1 method JIT'd inside the window`
  check self-skips every run (the capture itself is asserted live); forcing a
  guaranteed in-window JIT remains the concurrency plan's recorded follow-up.

### 2026-07-21 — Intel Core i7-8559U (Coffee Lake, Family 6), validating agent

The first time asmtest's Intel PT tier has run on real PT silicon. This is the
same physical box the macOS Mach-stepper doc validated
([macos-oop-mach-stepper.md](implementations/macos-oop-mach-stepper.md),
`intel-macos-x86_64-de7ec54c`), now running Linux (Ubuntu 26.04, kernel
`7.0.0-28-generic`) on Apple `MacBookPro15,2` hardware — bare metal (no
`hypervisor` CPU flag).

Box: `intel_pt` PMU present (`type=10`, `nr_addr_filters=2`),
`perf_event_paranoid=4`, Docker via `--cap-add=PERFMON` (bypasses paranoid=4; no
`--privileged`). All runs **exit 0, 0 `not ok`**:

- **`docker run --rm --cap-add=PERFMON asmtest-hwtrace make hwtrace-pt-live`** →
  `1..632`, **`# 631 passed, 0 failed`**, stable across 4 consecutive runs
  (`WERROR=1` clean). The `pt live` block ran live (no self-skip): the tier
  inits on real silicon; the self-JIT'd routine returns 42 under PT capture; the
  decoded stream covers the TAKEN walk `{0x0..0x11, 0xe skipped}` and the
  not-taken control covers the `0xe dec` (real-silicon TNT follow, not a baked
  answer); a 4 KiB AUX ring around a 2M-iteration loop reports `truncated`;
  `SET_FILTER` on a file-backed function is accepted (`# pt live: address-filter
  form accepted = object-relative`), the filtered function appears and its
  adjacent sibling is excluded; a `@exe` filter naming the anonymous JIT region
  is accepted-but-unmatched (`# pt live: anon @exe SET_FILTER rc=0`) so it captures
  nothing, and the decode-time fallback (unfiltered capture + code image) recovers
  the anon region. `# pt nr_addr_filters: 2`.
- **`docker run --rm --cap-add=PERFMON --security-opt seccomp=unconfined
  asmtest-dataflow-attach make dataflow-pt-live`** → `1..29`, **`# all 29 checks
  passed`**, stable across 3 runs. T4 `live(20,22)` and `live(200,1)` ran live:
  PT capture of one foreign-pid region invocation, `decoded {5,6} in-region
  offsets no truncation`, `PT-decoded path == single-step oracle path`, and
  **`F5 replay value trace is byte-identical to the emulator L0 (zero
  single-steps of the target)`** on both walks.
- **This run surfaced + fixed a real, load-bearing defect** the synthetic-fixture
  CI could never catch: the PT decode's code image covered only the target region,
  but a real unfiltered capture enables tracing (`TIP.PGE`) in the **caller**, so
  the decoder hit `-pte_nomap` at the first caller IP and decoded **zero**
  instructions. Fixed with a live-memory caller fallback
  (`asmtest_codeimage_read_live` in `read_recorder`; a self-memory fallback in the
  region-keyed `read_region`); plus explicit `pt`+`branch` COFI config bits in
  `pt_aux_open`; the foreign victim kept alive through `attach_end`; and the
  address-filter checks corrected to verified silicon behavior. See CHANGELOG
  `[Unreleased]` Fixed.
- **Reference-oracle cross-check:** `perf record -e intel_pt//u` on this box
  decodes 2.4M instructions / 104 k TNT+TIP from a ~114 KB AUX record — confirming
  the silicon does full PT flow and the earlier all-zero decode was the code-image
  gap above, not a hardware limit.
- **.NET inline `new AsmTrace(HwBackend.IntelPt)` — arms + captures live, but the
  managed multi-threaded PT suite is non-deterministically racy (a recorded
  blocker, NOT in the decode fix).** With `libipt-dev` temporarily added to the
  dotnet image and `make hwtrace-dotnet-test` run under `--cap-add=PERFMON`, the
  inline IntelPt ctor DID arm on this box: **`ok AsmTrace(IntelPt): armed on Intel
  PT silicon — captured 863180 instructions`**, the PT window is EXACT (not
  statistical). But the suite is unstable across runs: one run **SIGSEGVs**
  (createdump: **`signal 11 … code 0080 (SI_KERNEL) addr (nil)`** — a NULL-pointer
  deref) at a run-varying point (once after `ok 23`, once after `ok 81`), and
  another run completed all 227 checks but flaked one timing-sensitive compose
  check (`not ok unwarmed/PT compose: >=1 method JIT'd inside the window (got 0)`).
  The crash is a **race in the managed MULTI-THREADED live PT path** — the ambient
  hop / stitched-trace tests (T10/T11) spawn PT hops across async/pool threads and
  only run at all once libipt is present — NOT the single-threaded decode path this
  change fixed (a first gdb pass caught the abort with the main thread mid
  `ss_arm_tf`, i.e. a single-step TF window colliding with concurrent thread work,
  the managed-compose doc's documented per-thread hazard territory). Because it is
  non-deterministic and cross-thread, it needs dedicated concurrency work on the
  T10/T11 hop/stitch code; the `libipt-dev` dotnet-image addition was **reverted**
  so no privileged lane ships a race. Whole-window T4's .NET leg stays `◐` on that
  work, not on a missing install.
- Not run here to green: the T5/T11 managed-compose PT prongs
  ([managed-wholewindow-compose.md](implementations/managed-wholewindow-compose.md))
  — same dotnet-image + managed-concurrency gate as above (this box's PT hardware
  qualifies once the race + JIT-timing flake are resolved).
