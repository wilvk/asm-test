# Self-hosted CI runner lanes for hardware-gated tiers (AMD Zen, Intel PT/CoreSight, macOS arm64, KVM Linux) — implementation

> **Sources.** Actioned from
> [amd-review-followup-plan.md](../plans/amd-review-followup-plan.md) (P9),
> [hardware-trace-plan.md](../plans/hardware-trace-plan.md) (the CI section of
> Phase 1), [macos-clean-test-plan.md](../plans/macos-clean-test-plan.md)
> (Track E's one open exception), and
> [amd-hardware-validation.md](../amd-hardware-validation.md) /
> [2026-07-12-zen5-privileged-lbr-findings.md](../analysis/2026-07-12-zen5-privileged-lbr-findings.md)
> (the standing "a self-hosted Zen runner would light these paths up for free"
> suggestion). Written 2026-07-17. If this doc and a source disagree, this doc
> wins (sources may be stale); if the CODE and this doc disagree, re-verify
> before implementing.

## Why this work exists

Every hardware-exact capture path in this repo — AMD LbrExtV2 branch stacks,
live IBS sampling, Intel PT AUX capture, ARM CoreSight, the macOS clean-room
VMs, the Docker-OSX KVM lane — self-skips on GitHub-hosted runners, because the
hosted pool has none of that hardware. That gap is exactly what hid the
`trace_call_auto` completeness bug for weeks (see
[2026-07-12-zen5-privileged-lbr-findings.md](../analysis/2026-07-12-zen5-privileged-lbr-findings.md)):
the code was fine at decode level and wrong live, and no CI host could ever run
it live. This doc stands up self-hosted runner lanes — registered, labeled,
security-hardened, and *allowed to be absent* — so the live paths get
continuous coverage the moment silicon is attached, and the manual pre-release
checklist shrinks to only what no runner can cover.

## What already exists (verified 2026-07-17)

The make targets and workflows the runners will execute are all landed and
green; **nothing in this doc writes new capture code** — it is registration,
workflow wiring, and documentation.

| Piece | Where | Role |
|---|---|---|
| `docker-hwtrace-privileged` | [mk/docker.mk:584-588](../../../mk/docker.mk) | Builds the hwtrace image and runs `make hwtrace-test ibs-test` with `--cap-add=PERFMON` under Docker's **default** seccomp — no `--privileged`, no `seccomp=unconfined`. On Zen silicon the AMD live paths run; elsewhere they self-skip. |
| `hwtrace-privileged` CI job | [.github/workflows/ci.yml:813-840](../../../.github/workflows/ci.yml) | Runs that target on `ubuntu-latest` as a bitrot gate; its own comment (`:811`) says "A future self-hosted AMD runner would light the live paths up here for free". |
| `hwtrace` CI job | [.github/workflows/ci.yml:531-549](../../../.github/workflows/ci.yml) | Hosted decode + self-skip-gating lane; the comment above it (`:525-530`) names the self-hosted bare-metal job as "a separate, allowed-to-be-absent lane" — which does not exist yet. |
| `hwtrace-test` | [mk/native-trace.mk:2128-2134](../../../mk/native-trace.mk) | Runs `test_hwtrace`, `ibs_probe`, `test_ibs`. |
| `osx-vm-test` | [mk/bindings.mk:618-620](../../../mk/bindings.mk) | Track C tart clean room (Apple-Silicon-only, written but unvalidated — see [macos-cleanroom-lanes.md](macos-cleanroom-lanes.md)). |
| `docker-osx-bindings` | [mk/docker.mk:678-684](../../../mk/docker.mk) | Track D Docker-OSX clean room; hard-errors without `/dev/kvm`. |
| Live-run assert pattern | [.github/workflows/ci.yml:506-523](../../../.github/workflows/ci.yml) | The `gccanon-attach` job greps its own log and **fails on a self-skip** — the pattern every self-hosted lane here mirrors. |
| Non-vacuity markers | [examples/test_hwtrace.c:742](../../../examples/test_hwtrace.c) (`# SKIP AMD LBR live capture:`), [examples/test_hwtrace.c:1039-1045](../../../examples/test_hwtrace.c) (`# call_auto escalate: … (LBR window sufficed|escalated off the LBR window)`), [examples/test_ibs.c:327](../../../examples/test_ibs.c) (`# SKIP IBS live capture:`) | The exact strings the assert steps key on. |
| CAP_PERFMON measurements | [mk/docker.mk:573-583](../../../mk/docker.mk), [mk/cli.mk:384-389](../../../mk/cli.mk) | Measured on Zen 5: `CAP_PERFMON` alone bypasses `kernel.perf_event_paranoid` (even `=4`); no host sysctl change is ever needed. |
| `workflow_dispatch` pattern | [.github/workflows/release.yml:9-12](../../../.github/workflows/release.yml) | The manual-dispatch trigger shape to copy. |

**No self-hosted runner exists today**: `grep -rn self-hosted .github/workflows/`
hits only two comments (`ci.yml:529`, `ci.yml:811`) — zero `runs-on` entries.
No workflow invokes `osx-vm-test` or `docker-osx-bindings`.

Prove the baseline before touching anything (any Linux/macOS host with Docker):

```sh
# osx-vm-test and docker-osx-bindings are surfaced in `make help`:
make help | grep -E 'osx-vm-test|docker-osx-bindings'
# docker-hwtrace-privileged is a docker-* lane NOT printed by `make help`;
# confirm it exists in the makefiles instead (the next line then runs it):
grep -n '^docker-hwtrace-privileged:' mk/docker.mk   # -> mk/docker.mk:584
make docker-hwtrace-privileged   # builds + runs; off-Zen the AMD live tests
                                 # print "# SKIP AMD LBR live capture: ..." and
                                 # the run still exits 0 — that is the hosted
                                 # baseline this doc upgrades
```

## Tasks

### T1 — Write the runner registration + security-posture runbook and apply the repo settings  (S, depends on: none; **gated: repo-admin credentials**)

**Goal.** A junior operator with a spare machine and this one runbook can
register a correctly-labeled, ephemeral, security-hardened runner for this
repo without asking anyone anything.

**Steps.**

1. Create `docs/internal/ci/runners.md` (new directory; internal docs are
   excluded from the Sphinx build, so no `docs/index` wiring is needed).
   Mirror the tone of [amd-hardware-validation.md](../amd-hardware-validation.md)
   (procedure + "what green proves"). Content, in order:
   - **Registration flow** (from the verified research, cite the URLs in the
     Research notes below): repo Settings → Actions → Runners → "New
     self-hosted runner"; download + extract the pinned runner tarball
     (v2.335.1, 2026-06-09 — record the version AND the tarball's SHA-256 in
     the runbook when downloading, mirroring the
     `scripts/third-party-digests.txt` pinning habit); then
     `./config.sh --url https://github.com/<owner>/asm-test --token <TOKEN> --ephemeral --labels <lane-label>`.
     Note the registration token **expires after one hour** and needs repo
     admin. Record the REST alternative
     (`POST /repos/{owner}/{repo}/actions/runners/registration-token`) and the
     JIT option (`POST …/actions/runners/generate-jitconfig` →
     `./run.sh --jitconfig <encoded_jit_config>`) for automated re-registration.
   - **Ephemeral-only policy**: `--ephemeral` de-registers after one job;
     GitHub's autoscaling guidance recommends ephemeral, never persistent,
     runners. A systemd unit (or launchd plist on the Mac) loops: fetch a
     fresh JIT config → `./run.sh --jitconfig …` → repeat. One job per
     registration means a compromised job cannot persist on the runner
     identity.
   - **Label scheme** (custom labels are set at `config.sh` time; defaults
     `self-hosted` + OS + arch are added automatically; matching is
     case-insensitive and `runs-on` requires ALL listed labels):

     | Lane | `runs-on` | Runs |
     |---|---|---|
     | AMD Zen live capture | `[self-hosted, linux, x64, amd-zen]` | `make docker-hwtrace-privileged` (T3) |
     | Bare-metal Intel PT | `[self-hosted, linux, x64, intel-pt]` | `make docker-hwtrace-privileged`, later `hwtrace-pt-live` (T5) |
     | AArch64 CoreSight board | `[self-hosted, linux, arm64, coresight]` | `make hwtrace-test` once [coresight-live-decode.md](coresight-live-decode.md#T3) lands (T5) |
     | Apple-Silicon tart host | `[self-hosted, macos, arm64, tart]` | `make osx-vm-test` (T6) |
     | Bare-metal KVM Linux | `[self-hosted, linux, x64, kvm]` | `make docker-osx-bindings` (T6) |

   - **Security posture** (this is the load-bearing half — quote the
     hardening guide's warning that self-hosted runners "should almost never
     be used for public repositories"): self-hosted jobs live in a separate
     workflow (`hw.yml`, T2) that has **no `pull_request` trigger and never
     `pull_request_target`**, so fork code can never reach a runner; every
     self-hosted job carries `environment: hw-runners` (required-reviewer
     approval gate) and a `vars.HW_RUNNER_*` guard; workflows get
     `permissions: contents: read` and no secrets; the runner OS user is a
     dedicated non-sudo `runner` account. Record honestly that on the Linux
     boxes that account must be in the `docker` group, and docker-group
     membership is root-equivalent on that host — the box must therefore be
     dedicated to CI, hold no credentials, and be rebuildable from notes.
     Runner groups do not apply to repo-level runners (they are an org
     feature); note that if the repo ever moves into an org, hw runners go in
     a non-default group with a "Selected repositories" access policy and the
     public-repo access override left OFF.
2. Apply the repo settings (needs admin; record each as a checked item in the
   runbook so the next operator can re-verify):
   - Settings → Actions → General → fork pull request workflows: set
     **"Require approval for all external contributors"** (the default only
     covers first-time contributors).
   - Settings → Environments → create **`hw-runners`** with the maintainer as
     a required reviewer (up to 6 reviewers allowed; one approval suffices)
     and a deployment-branch policy of `main`.
   - Settings → Secrets and variables → Actions → Variables: create
     `HW_RUNNER_AMD_ZEN`, `HW_RUNNER_INTEL_PT`, `HW_RUNNER_CORESIGHT`,
     `HW_RUNNER_MACOS_TART`, `HW_RUNNER_KVM` — all set to `0`. A job guarded
     by `if: vars.HW_RUNNER_AMD_ZEN == '1'` skips instantly instead of
     queuing forever against a runner that does not exist (the failure mode
     [macos-clean-test-plan.md](../plans/macos-clean-test-plan.md) Track E
     explicitly deferred on).

**Code.** None — one new internal markdown file plus GitHub settings.

**Tests.** No testable repo surface. Manual verification: a second person
follows the runbook's registration section against a scratch VM and reaches
"Listening for Jobs" without needing outside help; the five variables and the
`hw-runners` environment are visible in repo settings.

**Docs.** The runbook IS the doc (internal-only — it contains operational
security posture, not user-facing material). No changelog entry (nothing
user-visible yet).

**Done when.**

- `docs/internal/ci/runners.md` exists and covers registration, ephemeral
  loop, labels, and posture with the settings checklist all checked.
- The `hw-runners` environment exists with a required reviewer.
- All five `HW_RUNNER_*` variables exist and read `0`.

### T2 — Land the guarded `hw.yml` workflow that cannot queue forever  (S, depends on: T1)

**Goal.** A new workflow file carries every self-hosted job, variable-guarded
off, so it merges today, stays green with zero runners, and each lane later
lights up by flipping one repo variable.

**Steps.**

1. Create `.github/workflows/hw.yml`. Copy the trigger shape from
   [release.yml:9-12](../../../.github/workflows/release.yml)
   (`workflow_dispatch`) and add a nightly `schedule` at `0 5 * * *` (two
   hours before [ci.yml](../../../.github/workflows/ci.yml)'s `0 7 * * *`
   nightly, so the two never contend for a one-box runner). **Deliberately no
   `push` and no `pull_request` trigger** — the hardware-trace plan's CI
   section requires these lanes be gated to trusted, maintainer-approved runs,
   and dispatch + schedule (schedule always runs `main`) is exactly that.
2. Top of file:

   ```yaml
   name: hw (self-hosted, allowed-to-be-absent)

   on:
     workflow_dispatch:
     schedule:
       - cron: '0 5 * * *'

   permissions:
     contents: read

   concurrency:
     # Static group ⇒ two hw.yml runs (a manual dispatch overlapping the
     # nightly) serialize instead of running at once; cancel-in-progress:false
     # lets the first finish rather than killing it. Keying on
     # ${{ github.run_id }} would be UNIQUE per run and group nothing — a no-op.
     # A single self-hosted box also runs one job at a time, so this is the
     # belt to the runner's suspenders, not the sole guard against contention.
     group: hw-self-hosted
     cancel-in-progress: false
   ```

3. Add the first job — the Zen lane T3 will light up — mirroring the
   assert-the-lane-really-ran shape of
   [ci.yml:506-523](../../../.github/workflows/ci.yml):

   ```yaml
   jobs:
     hwtrace-privileged-zen:
       name: hwtrace-privileged (self-hosted Zen — LIVE AMD LBR + IBS)
       if: vars.HW_RUNNER_AMD_ZEN == '1'
       runs-on: [self-hosted, linux, x64, amd-zen]
       environment: hw-runners
       timeout-minutes: 45
       steps:
         - uses: actions/checkout@v7
         - name: Live AMD capture under CAP_PERFMON (default seccomp)
           run: |
             set -o pipefail
             make docker-hwtrace-privileged 2>&1 | tee /tmp/hwzen.log
         - name: Assert the AMD live paths RAN (a self-skip HERE is a failure)
           run: |
             log=/tmp/hwzen.log
             if grep -q '# SKIP AMD LBR live capture' "$log"; then
               echo "::error::AMD LBR live capture SELF-SKIPPED on the Zen runner — the lane tested nothing. Wrong host, missing amd_lbr_v2, or a broken cap add."
               exit 1
             fi
             if grep -q '# SKIP IBS live capture' "$log"; then
               echo "::error::IBS live capture SELF-SKIPPED on the Zen runner."
               exit 1
             fi
             grep '# call_auto escalate:' "$log"
             if grep '# call_auto escalate:' "$log" | grep -q 'LBR window sufficed'; then
               echo "::error::call_auto did NOT escalate off the 16-deep window on a 25-back-edge loop — a REGRESSION of the 5d8e0d2 fix, not a known issue."
               exit 1
             fi
             if grep -q '^not ok' "$log"; then
               echo "::error::failing assertions:"; grep '^not ok' "$log"; exit 1
             fi
   ```

   The escalate-line grep keys on the literal suffixes printed at
   [examples/test_hwtrace.c:1039-1045](../../../examples/test_hwtrace.c) —
   `(LBR window sufficed)` when `used.backend == ASMTEST_HWTRACE_AMD_LBR`,
   `(escalated off the LBR window)` otherwise. On a 25-back-edge loop the
   window **must** escalate; the `trace_call_auto` completeness bug this
   guards was fixed in `5d8e0d2`, so "window sufficed" here is a regression
   signal, never "the known open finding".
4. Do NOT touch the hosted jobs in `ci.yml`: `hwtrace-privileged`
   ([ci.yml:813-840](../../../.github/workflows/ci.yml)) stays on
   `ubuntu-latest` as the cannot-bitrot gate. Being in a separate file is a
   security decision, not taste — `ci.yml` runs on `pull_request`, and any
   job added there would execute fork-PR code on the hardware. (The plan
   pointer suggesting the new job "sit beside" ci.yml's `clean-room` job at
   `:891` predates this posture; this doc wins.)
5. Push, then verify:
   `gh workflow run hw.yml && gh run watch` — the run completes in seconds
   with `hwtrace-privileged-zen` **skipped** (its variable is `0`), proving
   the file can land ahead of all hardware.

**Code.** One new file, `.github/workflows/hw.yml`, as above.

**Tests.** The workflow run itself: dispatched with all variables `0`, every
job shows "skipped" and the run concludes green in under a minute. A YAML
error surfaces as GitHub refusing the workflow on push (check the Actions tab
banner).

**Docs.** Internal-only at this stage; `docs/internal/ci/runners.md` gains a
one-paragraph "the workflow" section pointing at `hw.yml`. Changelog waits for
the first live lane (T3).

**Done when.**

- `hw.yml` is on `main`; `gh workflow run hw.yml` completes green with the
  Zen job skipped.
- The scheduled trigger appears in the Actions UI (visible next 05:00 UTC).
- `grep -rn 'pull_request' .github/workflows/hw.yml` returns nothing.

### T3 — Register the AMD Zen runner and light the live lane  (L, depends on: T1, T2; **gated: a physical AMD Zen 4/5 Linux box**)

**Goal.** `hwtrace-privileged-zen` runs green on real Zen silicon in CI, with
the assert step proving the AMD-exact live paths executed rather than
self-skipped.

**Steps.**

1. Prepare the box: a dedicated bare-metal Linux install on an AMD **Zen 4 or
   Zen 5** part — the LbrExtV2 live floor is Zen 4, not Zen 3 (the Zen 3 BRS
   arm was never built in this tree; see
   [amd-branchsnap-lbr-docs.md](amd-branchsnap-lbr-docs.md#T5) and its Zen 4
   floor sweep in [#T6](amd-branchsnap-lbr-docs.md#T6)). Verify:
   `grep -m1 amd_lbr_v2 /proc/cpuinfo` hits. Install Docker (20.10+ so the
   `PERFMON` cap name is known); no sysctl changes — `CAP_PERFMON` bypasses
   `perf_event_paranoid` even at the Debian/Ubuntu `=4` default (measured;
   [mk/docker.mk:573-583](../../../mk/docker.mk),
   [mk/cli.mk:384-389](../../../mk/cli.mk)).
2. Prove the target locally BEFORE registering (separates hardware problems
   from CI problems): clone the repo on the box, run
   `make docker-hwtrace-privileged 2>&1 | tee /tmp/local.log`, and apply the
   T2 assert greps by hand. Expected: zero `# SKIP AMD LBR live capture`,
   zero `# SKIP IBS live capture`, the escalate line reading
   `used.backend=3 insns=77 … (escalated off the LBR window)`, zero `not ok`.
   (Do not pin the `1..N` plan counts — they drift; the greps are the
   contract.)
3. Register per the T1 runbook: dedicated `runner` user (non-sudo, in
   `docker`), pinned v2.335.1 tarball,
   `./config.sh --url https://github.com/<owner>/asm-test --token <TOKEN> --ephemeral --labels amd-zen`
   (defaults contribute `self-hosted`, `linux`, `x64`), then the ephemeral
   re-registration service. The runner appears under Settings → Actions →
   Runners as `Idle` with labels `self-hosted linux x64 amd-zen`.
4. Flip `HW_RUNNER_AMD_ZEN` to `1`, run `gh workflow run hw.yml`, approve the
   `hw-runners` environment prompt, watch the job pick up on the Zen runner
   and go green through the assert step.
5. Update the stale comment block at
   [ci.yml:808-812](../../../.github/workflows/ci.yml): the hosted job's
   comment still calls the `call_auto` finding "open" (it was fixed in
   `5d8e0d2` — [2026-07-12-zen5-privileged-lbr-findings.md](../analysis/2026-07-12-zen5-privileged-lbr-findings.md)
   marks it `~~OPEN~~ RESOLVED`) and calls the self-hosted runner "future".
   Rewrite it to: the live counterpart now exists in `hw.yml`
   (`hwtrace-privileged-zen`); this hosted copy remains the build/decode
   bitrot gate.

**Code.** No source changes; one comment rewrite in `ci.yml` (step 5).

**Tests.** The CI run is the test. Failure looks like: the assert step red
with the `::error::` line naming which live path skipped, or the job queued
(label mismatch — re-check `config.sh --labels`). Pass looks like: job green
on a runner named in the run's "runner" line, assert step echoing the
escalate line.

**Docs.** Append to `CHANGELOG.md` under `## [Unreleased]` / `### Added`:
"Self-hosted AMD Zen CI lane (`hw.yml` `hwtrace-privileged-zen`): the exact
LbrExtV2 + live-IBS paths now run in CI on real silicon, with a
self-skip-is-failure assert." Update
[docs/reference/ci.md](../../reference/ci.md) "What CI covers" with one
sentence on the allowed-to-be-absent `hw` workflow — and while editing that
page, correct its stale "`macos-13`/`rosetta`" phrasing to `macos-15-intel`
(the macOS 13 image was retired 2025-12-08;
[release.yml:28-35](../../../.github/workflows/release.yml)).

**Done when.**

- A green `hw.yml` run shows `hwtrace-privileged-zen` executed on the
  `amd-zen` runner with the assert step passing.
- The nightly schedule produces the same green unattended (check the next
  morning's run).
- With the box powered off, the next scheduled run shows the job **skipped**
  only if the variable was flipped back — otherwise it fails visibly at
  pickup timeout; the runbook records "power down ⇒ set `HW_RUNNER_AMD_ZEN=0`"
  as the operator rule.

### T4 — Shrink the manual AMD checklist to what the runner cannot cover  (S, depends on: T3; coordinate with [amd-ibs-backend-honesty.md](amd-ibs-backend-honesty.md#T2))

**Goal.** [amd-hardware-validation.md](../amd-hardware-validation.md) stops
being "the one validation step that cannot run in CI" and becomes a short
residue list of exactly what the Zen runner does not reach, each with its
reason and its owning doc.

**Steps.**

1. Coordination: [amd-ibs-backend-honesty.md#T2](amd-ibs-backend-honesty.md)
   owns *repairing* that checklist (retiring the stale "not yet fixed" §"OPEN
   finding", inverting the `truncated=0` guidance, un-vacuous-ing the
   `ibs_probe` gate). Land after it, or in the same PR series — do **not**
   restate or redo its edits here.
2. Rewrite the doc's framing sections ("What this is", "Why CI can't do
   this"): CI now *does* do this — the `hw.yml` Zen lane runs
   `make docker-hwtrace-privileged` on Zen silicon per T3. The manual
   procedure remains only for the residue.
3. Replace the pre-release checklist's scope with the explicit residue list
   (this is the "state which those are" half of the source item):
   - **Zen 2 IBS-only lane** — the runner is Zen 4/5; the Zen 2 dev box
     (Ryzen 9 4900HS) has `ibs_op` but no `amd_lbr_v2`, so the
     IBS-without-LBR degradation path runs only there:
     `make docker-hwtrace-ibs` ([mk/docker.mk:567-571](../../../mk/docker.mk)),
     manual, pre-release.
   - **Zen 3 BRS** — silicon this project does not own AND a capture arm the
     tree has not built; both live in
     [amd-branchsnap-lbr-docs.md#T8](amd-branchsnap-lbr-docs.md). Not a
     checklist item — a link.
   - **MSR-direct lane** — `make docker-hwtrace-msr`
     ([mk/docker.mk:554-558](../../../mk/docker.mk)) needs `--privileged` plus
     the host `msr` module; policy decision recorded here: it stays OFF the CI
     runner (a `--privileged` container on a CI box widens the blast radius
     past what T1's posture accepts) and remains manual on the same Zen box.
   - **status live-EPERM path** — unreachable as root-in-container; unchanged.
4. Keep the "what green proves" table but re-point the left column at the CI
   lane and the right at the residue. Use Zen 4+ phrasing for the LBR floor
   in any text this task writes (the tree-wide "Zen 3+" sweep belongs to
   [amd-branchsnap-lbr-docs.md#T6](amd-branchsnap-lbr-docs.md), not here).

**Code.** None — one internal doc rewritten.

**Tests.** No testable surface (documentation task). Manual verification: a
reader following only the rewritten doc performs exactly the residue steps
and nothing the CI lane already covers; every residue item names its command
and its gate.

**Docs.** The task IS a docs task; additionally one `CHANGELOG.md`
`### Changed` bullet: "AMD manual pre-release validation shrunk to the
runner-uncoverable residue (Zen 2 IBS, MSR-direct, live-EPERM); LbrExtV2 +
live IBS moved to the self-hosted CI lane."

**Done when.**

- The rewritten doc contains no instruction the `hwtrace-privileged-zen` lane
  already performs.
- Each residue item names: the command, the hardware/privilege gate, and (for
  Zen 3 BRS) the owning sibling doc.
- No text in the rewrite contradicts
  [amd-ibs-backend-honesty.md#T2](amd-ibs-backend-honesty.md)'s repaired
  wording (read its landed diff first).

### T5 — Extend `hw.yml` with the bare-metal Intel PT lane and the CoreSight placeholder  (M, depends on: T2; **gated: bare-metal Intel PT x86-64 box; the CoreSight arm lands only as a dark, variable-guarded placeholder, its live activation deferred to [coresight-live-decode.md#T4](coresight-live-decode.md)**)

**Goal.** The "explicit, separate, allowed-to-be-absent" bare-metal
hardware-trace job the hosted `hwtrace` lane's comment promises
([ci.yml:525-530](../../../.github/workflows/ci.yml)) exists for Intel PT,
with the CoreSight variant specified and guarded off until its decoder lands.

**Steps.**

1. Add `hwtrace-pt-baremetal` to `hw.yml`, cloned from the T2 Zen job:
   `runs-on: [self-hosted, linux, x64, intel-pt]`,
   `if: vars.HW_RUNNER_INTEL_PT == '1'`, `environment: hw-runners`. Body:
   `make docker-hwtrace-privileged 2>&1 | tee /tmp/hwpt.log` — on bare-metal
   Intel the container sees the host kernel's `intel_pt` PMU node and
   `CAP_PERFMON` authorizes the open, so the PT live capture paths in
   `test_hwtrace` run. Shakedown caveat to verify on first bring-up (record
   the answer in the runbook): confirm
   `/sys/bus/event_source/devices/intel_pt/type` is visible inside the
   container (`docker run --rm asmtest-hwtrace cat /sys/bus/event_source/devices/intel_pt/type`);
   if an engine/kernel combination hides it, fall back to a host-native
   `make hwtrace-test` step on the runner with the box's
   `perf_event_paranoid` lowered — and record that deviation from the
   Docker-first rule as a hardware-shaped exception.
2. Non-vacuity for PT: do **not** grep ad-hoc skip strings — the PT skip text
   has a known cosmetic misreport
   ([2026-07-12-zen5-privileged-lbr-findings.md §3](../analysis/2026-07-12-zen5-privileged-lbr-findings.md)).
   The honest mechanism is the require-mode target
   [intel-pt-whole-window-substrate.md#T5](intel-pt-whole-window-substrate.md)
   builds (`hwtrace-pt-live`, `ASMTEST_REQUIRE_PT=1` — skip becomes hard
   failure). Until that lands, this job is a build-and-run gate on PT
   silicon; once it lands, add a second step running `hwtrace-pt-live` inside
   the hwtrace image and the lane becomes self-skip-proof. Put a dated
   `# TODO(intel-pt-whole-window-substrate#T5)` comment on the job so the
   upgrade is not forgotten.
3. Register the Intel box per the T1 runbook (`--labels intel-pt`); flip
   `HW_RUNNER_INTEL_PT=1`; dispatch; approve; verify green.
4. Add `hwtrace-coresight-board` as a fully-written but variable-guarded job:
   `runs-on: [self-hosted, linux, arm64, coresight]`,
   `if: vars.HW_RUNNER_CORESIGHT == '1'`, body `make hwtrace-test`. Today the
   CoreSight tier self-skips everywhere (`asmtest_cs_decoder_present()`
   returns 0 — [hardware-trace-plan.md](../plans/hardware-trace-plan.md)
   Implementation status), so this job stays dark: leave
   `HW_RUNNER_CORESIGHT=0` with a comment that flipping it is
   [coresight-live-decode.md#T4](coresight-live-decode.md)'s acceptance step,
   on the board that doc's #T2 brings up. Do not write CoreSight assert-greps
   here — that doc owns its live markers.

**Code.** Two jobs appended to `.github/workflows/hw.yml`.

**Tests.** As T3: the dispatched run is the test. PT pass: job green on the
`intel-pt` runner; after the require-target upgrade, a PT-less impostor host
turns the run red instead of silently green. CoreSight: job shows "skipped"
while its variable is `0`.

**Docs.** [docs/reference/ci.md](../../reference/ci.md): extend the T3
sentence to name all three hardware lanes as allowed-to-be-absent.
`CHANGELOG.md` `### Added` bullet for the PT lane when it first runs green.

**Done when.**

- `hwtrace-pt-baremetal` green on real PT silicon via dispatch AND schedule.
- The in-container `intel_pt` visibility answer is recorded in
  `docs/internal/ci/runners.md`.
- `hwtrace-coresight-board` exists, is skipped, and names its unblock
  condition in a comment.
- Off all hardware (variables `0`), `gh workflow run hw.yml` still completes
  green in seconds.

### T6 — Wire the macOS-arm64 tart and KVM Docker-OSX dispatch lanes  (S, depends on: T1, T2, [macos-cleanroom-lanes.md#T2](macos-cleanroom-lanes.md) and [#T6](macos-cleanroom-lanes.md) green; **gated: Apple-Silicon host + bare-metal KVM Linux box**)

**Goal.** The Track C/D clean-room backstops become dispatchable CI jobs on
self-hosted runners — the one exception
[macos-clean-test-plan.md](../plans/macos-clean-test-plan.md) Track E left
open — without ever touching the hosted pool.

**Steps.**

1. **Hard precondition** (the plan's own wiring condition): both lanes must
   have run green at least once locally —
   [macos-cleanroom-lanes.md#T2](macos-cleanroom-lanes.md) (tart shakedown)
   and [#T6](macos-cleanroom-lanes.md) (Docker-OSX shakedown). Wiring first
   inverts the plan's ordering and buys a job that can only fail its
   shakedown in CI.
2. Add `cleanroom-tart` to `hw.yml`:
   `runs-on: [self-hosted, macos, arm64, tart]` (label matching is
   case-insensitive; the Mac's default labels are `macOS`/`ARM64`),
   `if: vars.HW_RUNNER_MACOS_TART == '1'`, `environment: hw-runners`, step:
   `make osx-vm-test` ([mk/bindings.mk:618-620](../../../mk/bindings.mk),
   which runs `scripts/osx-vm.sh`). The runner service on the Mac runs as the
   dedicated user with tart installed per the sibling doc; no Docker
   involved. The macOS EULA permits up to 2 macOS VMs on Apple hardware, so
   this lane is license-clean.
3. Add `cleanroom-kvm-osx`: `runs-on: [self-hosted, linux, x64, kvm]`,
   `if: vars.HW_RUNNER_KVM == '1'`, `environment: hw-runners`, step:
   `make docker-osx-bindings` ([mk/docker.mk:678-684](../../../mk/docker.mk)).
   The `if: vars.HW_RUNNER_KVM == '1'` guard is the *skip* mechanism — with no
   KVM box the variable stays `0` and the job never runs, which is how this
   lane honors the global position's "self-skip off KVM" (no false pass off
   KVM). The target's own `/dev/kvm` hard-error is NOT that skip path; it is a
   mislabel tripwire for the one case the guard cannot catch — variable on but
   the runner wrong — where the job goes red with the target's clear message,
   which is exactly right (per the global position: the Docker-OSX lane is
   hardware-gated on a bare-metal `/dev/kvm` box; the plan's old "launchable
   today" note referred to a host this environment does not have). Note the
   EULA-gray status of macOS-on-non-Apple in the job comment, mirroring the
   plan's honesty.
4. Optional PR-label path (the plan's "and/or a `clean-room` PR-label"): if
   wanted, add a `pull_request: types: [labeled]` trigger to `hw.yml` with a
   job-level guard that ALL THREE hold —
   `github.event.label.name == 'clean-room'`,
   `github.event.pull_request.head.repo.full_name == github.repository`
   (same-repo only — fork PRs can never qualify), and the `hw-runners`
   environment approval. If any of that feels heavy, skip it: dispatch-only
   is the recommended shape, and the runbook documents the label recipe as
   deferred.
5. Register the two runners per T1 (`--labels tart` on the Mac,
   `--labels kvm` on the Linux box), flip the variables, dispatch, approve,
   verify.

**Code.** Two (optionally three-trigger) jobs appended to `hw.yml`.

**Tests.** Dispatch run green on both runners; with variables `0` both jobs
skip. The KVM job on a non-KVM host fails with the target's
`/dev/kvm absent` message (negative test: run once with the label
deliberately on a KVM-less scratch VM before trusting the lane).

**Docs.** [docs/clean-room-testing.md](../../clean-room-testing.md) gains a
short "Running the C/D lanes from CI" subsection (dispatch instructions +
which variable/hardware each needs). Update the Track E exception block in
[macos-clean-test-plan.md](../plans/macos-clean-test-plan.md) from "not
wired" to done-with-date. One `CHANGELOG.md` `### Added` bullet.

**Done when.**

- `gh workflow run hw.yml` produces green `cleanroom-tart` and
  `cleanroom-kvm-osx` runs on their respective runners.
- Neither job ever appears in a `pull_request`-triggered run from a fork
  (inspect the triggers; if step 4 was taken, verify a fork PR with the label
  does NOT start the job).
- Track E's exception paragraph is updated.

## Task order & parallelism

```
T1 (runbook + settings)
 └─→ T2 (hw.yml scaffold)  ── lands with zero hardware
       ├─→ T3 (Zen runner live)      [gate: Zen 4/5 box]
       │     └─→ T4 (checklist shrink; coordinate w/ amd-ibs-backend-honesty#T2)
       ├─→ T5 (Intel PT lane; CS placeholder)  [gate: PT box; CS activation deferred → coresight-live-decode#T4]
       └─→ T6 (tart + KVM lanes)  [gates: Apple Silicon + KVM box; after macos-cleanroom-lanes#T2/#T6]
```

T1→T2 is the ungated critical path and one person can do both in a day.
T3, T5, and T6 are mutually independent — three people with three different
machines can run them concurrently; each is blocked only by its own hardware.
T4 additionally coordinates with an out-of-doc task
([amd-ibs-backend-honesty.md#T2](amd-ibs-backend-honesty.md)) — landing after it
or in the same PR series — on top of its in-doc dependency on T3.

## Constraints & gates

- **Hardware gates (real, per CLAUDE.md — record and guard, do not fake):**
  an AMD Zen 4/5 box (T3), a bare-metal Intel PT x86-64 box (T5), an AArch64
  CoreSight board (needed only to activate T5's dark CoreSight job, whose
  live decode is deferred to
  [coresight-live-decode.md#T4](coresight-live-decode.md)), an Apple-Silicon
  Mac (T6), a bare-metal `/dev/kvm` Linux box (T6). The recorded form of each
  gate is its `HW_RUNNER_*` variable at `0` plus the runbook's status table.
- **Credential gates:** repo admin for registration tokens, environments,
  variables, and fork-PR settings (T1). Registration tokens expire after one
  hour — generate at the keyboard of the target machine.
- **Security posture is a constraint, not advice:** no `pull_request` /
  `pull_request_target` triggers in `hw.yml`, ever; ephemeral runners only;
  `environment: hw-runners` + `permissions: contents: read` on every
  self-hosted job; no secrets exposed to hw jobs; dedicated non-sudo runner
  user; CI-only boxes. GitHub's hardening guide is blunt that self-hosted
  runners on public repos are dangerous — this posture is the mitigation set.
- **Pinning:** the runner tarball is pinned (v2.335.1) with its SHA-256
  recorded in the runbook, mirroring the repo's third-party pinning habit.
  In-repo dependencies need nothing new — every lane runs existing `docker-*`
  targets.
- **Positions honored:** Zen 4 (not Zen 3) is the LbrExtV2 live floor;
  `call_auto` non-escalation is a regression signal, never "the known open
  finding"; the Docker-OSX lane is `/dev/kvm`-hardware-gated; macOS Intel CI
  references use `macos-15-intel`, never `macos-13`.

## Research notes (verified 2026-07-17)

- **Registration flow**: repo Settings → Actions → Runners → "New self-hosted
  runner"; download/extract, `./config.sh --url <URL> --token <TOKEN>`,
  `./run.sh`. Registration token "expires after one hour"; repo-level
  registration needs repo admin.
  <https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/adding-self-hosted-runners>
- **REST + JIT**: `POST /repos/{owner}/{repo}/actions/runners/registration-token`
  (and `…/remove-token`); JIT runners via
  `POST /repos/{owner}/{repo}/actions/runners/generate-jitconfig` (required:
  `name`, `runner_group_id`, `labels` 1-100 items; optional `work_folder`,
  default `_work`) returning `encoded_jit_config`.
  <https://docs.github.com/en/rest/actions/self-hosted-runners?apiVersion=2022-11-28>
  The start flag is `--jitconfig` (confirmed in runner source,
  <https://github.com/actions/runner/blob/main/src/Runner.Common/Constants.cs>
  line 112 — the docs REST page omits the run command; caveat noted).
- **Ephemeral + autoscaling**: `./config.sh … --ephemeral` de-registers after
  one job; GitHub recommends autoscaling only with ephemeral, never
  persistent, runners.
  <https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/autoscaling-with-self-hosted-runners>
- **Current runner release**: v2.335.1 (2026-06-09).
  <https://github.com/actions/runner/releases/latest>
- **Public-repo hardening**: self-hosted runners "should almost never be used
  for public repositories"; JIT/ephemeral improves registration security but
  "Re-using hardware to host JIT runners can risk exposing information".
  <https://docs.github.com/en/actions/security-for-github-actions/security-guides/security-hardening-for-github-actions>
- **Runner groups**: org-level feature; default group auto-joins new runners;
  by default "only private repositories can access runners in a runner group"
  (public-repo access is an explicit override; the literal checkbox name
  could not be quoted verbatim from the docs — caveat).
  <https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/managing-access-to-self-hosted-runners-using-groups>
- **Labels**: defaults at registration are `self-hosted`, OS
  (`linux`/`windows`/`macOS`), arch (`x64`/`ARM`/`ARM64`); suppressible with
  `--no-default-labels`; custom labels at config time
  (`--labels a,b`, case-insensitive; unused custom labels auto-delete within
  24 h).
  <https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/using-labels-with-self-hosted-runners>
  `runs-on` labels are cumulative (runner must hold ALL);
  canonical `runs-on: [self-hosted, linux, x64]`.
  <https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/using-self-hosted-runners-in-a-workflow>
- **Fork-PR approval settings**: options are "Require approval for
  first-time contributors who are new to GitHub", "Require approval for
  first-time contributors" (default), "Require approval for all external
  contributors".
  <https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/enabling-features-for-your-repository/managing-github-actions-settings-for-a-repository>
  Pending fork runs auto-delete after 30 days without approval.
  <https://docs.github.com/en/actions/managing-workflow-runs-and-deployments/managing-workflow-runs/approving-workflow-runs-from-public-forks>
- **`pull_request_target`**: runs in the base repo's default-branch context
  with secrets and a write token; "Running untrusted code on the
  pull_request_target trigger may lead to security vulnerabilities" — the
  reason `hw.yml` never uses it.
  <https://docs.github.com/en/actions/writing-workflows/choosing-when-your-workflow-runs/events-that-trigger-workflows>
- **Environments**: up to 6 required reviewers, one approval suffices;
  optional wait timer and deployment-branch policies; admins bypass by
  default (can be disallowed); environments are available to public repos on
  all plans.
  <https://docs.github.com/en/actions/managing-workflow-runs-and-deployments/managing-deployments/managing-environments-for-deployment>
- **CAP_PERFMON**: Docker's current default seccomp profile carries a
  dedicated rule `{"names":["perf_event_open"],"caps":["CAP_PERFMON"]}`
  (<https://raw.githubusercontent.com/moby/profiles/main/seccomp/default.json>),
  so [mk/docker.mk:584-588](../../../mk/docker.mk)'s plain
  `--cap-add=PERFMON` under the default profile is sufficient. Kernel side:
  since v5.9, processes with `CAP_PERFMON` "bypass scope permissions checks"
  for perf_events (<https://www.kernel.org/doc/html/latest/admin-guide/perf-security.html>)
  — consistent with the repo's measured comments
  ([mk/docker.mk:573-583](../../../mk/docker.mk),
  [mk/cli.mk:384-389](../../../mk/cli.mk)). The minimum engine version
  shipping that seccomp rule (~20.10+) was not independently verified —
  caveat carried into the runbook.

## Out of scope

- **Running the tart / Docker-OSX shakedowns themselves** — that is
  [macos-cleanroom-lanes.md](macos-cleanroom-lanes.md) (#T2 tart, #T4/#T5/#T6
  Docker-OSX); T6 here only wires the CI jobs after those are green.
- **Repairing the AMD validation checklist's stale claims** (the "not yet
  fixed" finding, the vacuous `ibs_probe` gate) —
  [amd-ibs-backend-honesty.md#T2](amd-ibs-backend-honesty.md); T4 here only
  shrinks the repaired checklist's scope.
- **Zen 3 BRS capture arm and the Zen 4 floor sweep** —
  [amd-branchsnap-lbr-docs.md#T5/#T6/#T8](amd-branchsnap-lbr-docs.md).
- **The `hwtrace-pt-live` require-mode target and PT window substrate** —
  [intel-pt-whole-window-substrate.md#T5](intel-pt-whole-window-substrate.md);
  T5 here consumes it when it lands.
- **CoreSight live decode and board bring-up** —
  [coresight-live-decode.md#T2-#T4](coresight-live-decode.md); T5's CoreSight
  job stays dark until its #T4 flips the variable.
- **Foreign-PID PT attach** (a future consumer of the PT runner) —
  [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md).
- **Benchmark lanes on these runners** — any bench-on-hw follow-up belongs to
  [benchmarks-ci-followups.md](benchmarks-ci-followups.md).
