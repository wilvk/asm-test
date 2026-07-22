# AMD hardware validation — the runner-uncoverable residue

**What this is:** the short list of AMD capture paths a maintainer still runs by
hand before a release, because the self-hosted CI runner cannot reach them. It is
**no longer** the whole AMD validation story.

The AMD-exact **LbrExtV2 branch-stack + live-IBS** paths — the ones that
self-skip everywhere in GitHub's hosted pool — now run for real in CI on the
self-hosted **`hwtrace-privileged-zen`** lane in
[`.github/workflows/hw.yml`](../../.github/workflows/hw.yml): it runs
`make docker-hwtrace-privileged` on a registered AMD **Zen 4/5** runner and its
assert step fails hard if any live path self-skips. That lane ran green
end-to-end on 2026-07-22 ([run
29897214772](https://github.com/wilvk/asm-test/actions/runs/29897214772) — see
[Recorded runs](#recorded-runs)). So the LBR + live-IBS tier is CI's job now, not
a manual checklist item.

This page is the **residue**: exactly the AMD paths that Zen 4/5 runner does not
reach, each with its command, its hardware/privilege gate, and (where the work
lives elsewhere) its owning doc.

> **Standing-runner caveat.** The Zen lane only produces coverage while a runner
> is actually registered and `HW_RUNNER_AMD_ZEN=1`. If no standing Zen runner is
> currently attached (e.g. the proof run used a one-shot ephemeral runner that
> de-registered), the LBR + live-IBS tier is covered by registering a runner and
> letting that lane run — **not** by copying its steps back into this checklist.
> The one command the lane runs, `make docker-hwtrace-privileged`, and what a
> green run proves, are documented at
> [self-hosted-ci-runners.md](implementations/self-hosted-ci-runners.md) and the
> runbook [ci/runners.md](ci/runners.md); do not restate them here.

## The three tiers (who covers what)

| Tier | Where | Covers |
|---|---|---|
| Hosted bitrot gate — `hwtrace-privileged` | `ci.yml`, GitHub `ubuntu-latest` (Intel/generic) | target builds + runs; `--cap-add=PERFMON` accepted; IBS-Op **decoder** + substrate/PMU detection execute; the detect-and-skip chain stays green. The AMD-exact **live** tests self-skip (no Zen CPU). A green badge here is **not** evidence the LbrExtV2/live-IBS capture works. |
| Self-hosted Zen lane — `hwtrace-privileged-zen` | `hw.yml`, a registered **Zen 4/5** runner | the exact live paths: LbrExtV2 live capture, Tier-B stitch beyond a single 16-deep window, over-ring truncation, `#2A` period reach, `sample_window` survey, live IBS out-of-band + whole-process survey. **This replaced the old manual full-run.** |
| **This doc** — the residue | manual, pre-release, on the box each item names | the four paths below, none of which the Zen 4/5 runner can reach. |

The **LbrExtV2 live floor is Zen 4** (Zen 3's BRS arm was never built in this
tree — see the Zen 3 BRS residue item); the `ibs_op` PMU is Zen 2+.

## The residue — what the Zen 4/5 runner does not reach

Each item states the **command**, the **gate**, and the **observation to
record** (never the expected verdict — a stale item is one an observation
contradicts; fix the doc in the same change that records the run).

### 1. Zen 2 IBS-without-LBR degradation

- **Command:** `make docker-hwtrace-ibs`
  ([mk/docker.mk](../../mk/docker.mk) `docker-hwtrace-ibs`).
- **Gate:** **Zen 2 silicon** — the CI runner is Zen 4/5, which *has*
  `amd_lbr_v2`, so it can never exercise the branch that fires when IBS is
  present but LbrExtV2 is **absent**. The Zen 2 dev box (**Ryzen 9 4900HS**:
  `ibs_op` present, no `amd_lbr_v2`) is the only place that degradation path
  runs.
- **Record:** IBS survey paths run while the AMD-LBR tier degrades honestly (no
  LbrExtV2), `0 not ok`, no unexpected skips.

### 2. MSR-direct capture

- **Command:** `make docker-hwtrace-msr`
  ([mk/docker.mk](../../mk/docker.mk) `docker-hwtrace-msr`).
- **Gate:** `--privileged` **plus** the host `msr` kernel module loaded.
- **Policy — stays OFF the CI runner (recorded decision):** a `--privileged`
  container on a CI box widens the blast radius past what the runner security
  posture ([ci/runners.md](ci/runners.md#security-posture)) accepts. This lane is
  therefore **not** added to `hw.yml`; it remains a manual pre-release step on the
  same Zen box, run only when the `msr` module is loaded.
- **Record:** the MSR-direct path opens and captures with `0 not ok`; if the
  `msr` module is not loaded, record it as gated, not observed.

### 3. `status` live-EPERM path

- **Gate:** an **unprivileged / non-root** context. The container runs as root,
  so the unprivileged-`EPERM` branch of the `status` probe is unreachable there —
  and it is equally unreachable on the Zen runner, which also runs the target as
  root-in-container.
- **Record:** noted, not run (unchanged — there is no container lane that reaches
  it).

### 4. Zen 3 BRS

Not a checklist item — a **link**. Zen 3 (Family 19h) Branch Sampling is silicon
this project does not own **and** a capture arm the tree has never built; both
live in [amd-branchsnap-lbr-docs.md#T8](implementations/amd-branchsnap-lbr-docs.md).
Nothing to run here.

## RESOLVED (5d8e0d2) — `call_auto` AMD-LBR rung

The [Zen 5 findings §2](analysis/2026-07-12-zen5-privileged-lbr-findings.md)
`trace_call_auto` AMD-LBR completeness bug is **FIXED** (commit `5d8e0d2`); the
findings doc marks it `~~OPEN~~ RESOLVED`, deterministic across 16 privileged
runs (every one escalates `backend=3 insns=77`).

The `call_auto` case (b) drives 25 taken back-edges; a 16-deep window cannot hold
them, so escalation **MUST** fire — the `# call_auto escalate:` line reads
`backend=3` with the full `insns` count (~77). A `truncated=0 (LBR window
sufficed)` on this line is a **REGRESSION**, not a known finding. This is no
longer a manual eyeball: the **`hwtrace-privileged-zen` assert step now enforces
it in CI** — it greps the escalate line and fails the run if it reads `LBR window
sufficed`. The regression signal is guarded by the lane, not by this checklist.

## Recorded runs

Record each observed run here (host, date, values), newest first.

### 2026-07-22 — Ryzen 9 4900HS (Zen 2, Family 17h): residue items 1 + 2 run

Box: `ibs_op` + `ibs_fetch` PMUs present, **no `amd_lbr_v2`** (the residue's Zen 2
degradation premise, confirmed live), host `perf_event_paranoid=2`, `msr` module
loaded, kernel 6.14. Three lanes, all **exit 0, 0 `not ok`**:

- **Residue item 1 — `make docker-hwtrace-ibs`** (seccomp=unconfined, no caps):
  `test_ibs` **`1..81`** with the live surveys RUNNING unprivileged (paranoid=2 +
  `swfilt`): `# IBS-Op on this host: AVAILABLE` + `# IBS-Fetch … AVAILABLE` via
  the real-open path, out-of-band pid survey (spin-loop back-edge captured, `3
  edges, 5818/24431 branch/total samples, throttled`), whole-process survey
  `3/3 worker functions covered, 39 edges`, fetch survey (`4 addrs, 21795/21795
  valid, 0 ic-miss, 0 itlb-miss`), phase5 callchain + record-bound checks all
  `ok`. ONE honest skip: system-wide capture (`needs CAP_PERFMON / paranoid<=0
  (substrate present) — EACCES`). This is exactly the IBS-present /
  LbrExtV2-absent path the Zen 4/5 runner can never reach.
- **`make docker-hwtrace-amd`** (CAP_PERFMON): `test_hwtrace` `1..645` +
  `test_ibs` `1..84` — 729 ok, 0 failed. The AMD-LBR tier degrades honestly on
  this box: every AMD-LBR live test self-skips with `no AMD branch records
  (needs Zen 3 BRS / Zen 4 LbrExtV2)`, never a false AVAILABLE; with CAP_PERFMON
  the system-wide IBS capture also ran (**zero** `# SKIP IBS` lines). The
  regression signal reads `# call_auto escalate: rc=0 result=25 used.backend=3
  insns=77 truncated=0 (escalated off the LBR window)` — escalation fires, not
  `LBR window sufficed`.
- **Residue item 2 — `make docker-hwtrace-msr`** (`--privileged`, host `msr`
  module loaded): `1..654` + `1..84` — 738 ok, 0 failed. The **MSR-direct
  LbrExtV2 path itself self-skips on this box** (`# SKIP AMD MSR-direct:
  substrate absent (needs amd_lbr_v2 + /dev/cpu/N/msr + CAP_SYS_ADMIN)`) — Zen 2
  has no LbrExtV2 MSRs, so item 2's primary observation (the MSR path opening
  and capturing) **still awaits the Zen 4/5 box with the `msr` module loaded**.
  Recorded here instead: the lane's `/dev/cpu/N/msr` mechanics work, and its
  second consumer — the W3 in-process BTF block-step — ran LIVE through the same
  access: `# in-process BTF: rc=0 result=42 insns=5 truncated=0` (stream
  `[0,3,6,c,11]`, block partition `{0, 0x11}` matching
  single-step/PT/AMD/DynamoRIO) and the 62-insn loop fixture complete with no
  depth ceiling (`insns=62 truncated=0`) — the independent re-validation now
  recorded on [inproc-btf-block-step](implementations/inproc-btf-block-step.md).
- Item 3's premise re-confirmed in both capability-bearing lanes: `# SKIP status
  live-EPERM lane: … (stage=4 paranoid=2 root=1)` — root-in-container keeps the
  unprivileged-EPERM branch unreachable, as recorded (noted, not run).

### 2026-07-22 — self-hosted CI Zen lane went live (Ryzen 9 9950X, Zen 5)

The `hwtrace-privileged-zen` lane in `hw.yml` ran green end-to-end for the first
time on real silicon — the LBR + live-IBS tier that this doc used to cover by
hand is now a CI lane. Registered a pinned `v2.335.1` `--ephemeral --labels
amd-zen` runner on the Ryzen 9 9950X (`amd_lbr_v2` present; tarball SHA-256
verified on box against GitHub's published digest), set `HW_RUNNER_AMD_ZEN=1`,
dispatched `hw.yml` → [run
29897214772](https://github.com/wilvk/asm-test/actions/runs/29897214772):
**`# 667 passed, 0 failed`**, zero `# SKIP AMD LBR live capture` / `# SKIP IBS
live capture`, `# call_auto escalate: rc=0 result=25 used.backend=3 insns=77
truncated=0 (escalated off the LBR window)`, assert step passing
(`hwtrace-privileged-zen: AMD LbrExtV2 + live IBS ran (no self-skip); call_auto
escalated off the LBR window`). One `# SKIP AMD MSR-direct: substrate absent`
line — expected residue (item 2 above), not a live-path skip. Runner was one-shot
ephemeral: it de-registered after the job and `HW_RUNNER_AMD_ZEN` was reset to
`0`; standing unattended-nightly coverage needs a persistent runner (JIT/ephemeral
loop).

### 2026-07-20 — Ryzen 9 9950X (Zen 5, Family 1Ah), validating agent

Box: `amd_lbr_v2` + `ibs_op`/`ibs_fetch` present, `perf_event_paranoid=4`,
kernel 6.17, Docker (no `--privileged`). Two lanes were run — the positive paths
and the honesty/blocked path — both **exit 0, 0 `not ok`**:

- **`make docker-hwtrace-privileged`** (CAP_PERFMON, default seccomp — bypasses
  paranoid=4): `test_hwtrace` **`1..658`**, the AMD LBR live tests ran (not
  skipped); `test_ibs` **`1..84`** with **no** `# SKIP IBS …` lines. `ibs_probe`
  printed **`IBS-Op … AVAILABLE`** and **`IBS-Fetch … AVAILABLE`** via the
  real-open path. `# whole-process: 3/3 worker functions covered` (56 edges) and
  `# system-wide: 3/3` (58 edges); `phase5` callchain survey + `record-bound`
  checks all `ok`. This is the exact target the `hwtrace-privileged-zen` CI lane
  now runs unattended.
- **`call_auto escalate` (the regression signal):** `# call_auto escalate:
  rc=0 result=25 used.backend=3 insns=77 truncated=0 (escalated off the LBR
  window)` — **escalation fired**, not a regression; the small-routine case is
  complete without escalation (`insns=5`, `ok`).
- **Honesty/blocked path — `make docker-hwtrace-ibs`** (seccomp=unconfined, NO
  CAP_PERFMON, so paranoid=4 refuses the open): `ibs_probe` printed `IBS-Op:
  substrate present but sampling is BLOCKED — perf_event_open refused (EACCES)`
  and the live IBS tests self-skipped with the real `unavail_reason` — a
  substrate-only host prints BLOCKED, never AVAILABLE. `1..60`, 0 `not ok`. (This
  is the *EACCES-honesty* use of the target; the **Zen 2 degradation** use in
  residue item 1 — IBS present, `amd_lbr_v2` absent — is a different path and
  needs a Zen 2 box.)
- Not run here: `docker-hwtrace-msr` (host `msr` module not loaded), the Zen 2
  degradation lane, and bare-metal Intel PT — recorded as gated, not observed.
