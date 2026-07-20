# AMD hardware validation — the manual pre-release step

**What this is:** the one validation step that cannot run in CI. GitHub-hosted
runners are Intel/generic — they have no AMD `amd_lbr_v2` (LbrExtV2) and no
`ibs_op` PMU — so the exact AMD branch-stack capture and live IBS paths
**self-skip** everywhere in the cloud. This page is the manual pre-release
procedure that runs those paths for real, on Zen silicon, and says what a green
run proves.

This is deliberately a **documented manual step**, not a CI gate. There is no
AMD runner in GitHub's hosted pool; a self-hosted Zen runner would let the CI
`hwtrace-privileged` job light these paths up for free (see below), but until one
exists a maintainer runs this by hand before tagging a release.

## Why CI can't do this

Two lanes share the `hwtrace` image but prove different things:

| Lane | Where | Proves |
|---|---|---|
| `hwtrace-privileged` CI job (`.github/workflows/ci.yml`) | GitHub `ubuntu-latest` (Intel/generic) | target builds + runs; `--cap-add=PERFMON` accepted; IBS-Op **decoder** + substrate/PMU detection execute; the detect-and-skip chain stays green. The AMD-exact **live** tests self-skip (no Zen CPU). |
| **this doc** — `make docker-hwtrace-privileged` on Zen | a real AMD Zen 3+/4/5 box | the exact live paths the runner can't reach: LbrExtV2 live capture, Tier-B stitch, over-ring truncation, `#2A` period reach, `sample_window` survey, live IBS out-of-band + whole-process survey. |

The CI job exists so the target **cannot bitrot** and so IBS decode + substrate
detection get continuous coverage; it is honestly named
`hwtrace-privileged (PERFMON; AMD-exact self-skips off Zen)` and is green via
self-skip on the non-AMD runner. It is **not** evidence that the AMD-exact
capture works — only this manual run is. Do not read a green CI badge as
coverage of the LbrExtV2 or live-IBS tiers.

This gap is exactly what let the [Zen 5 privileged-capture
findings](analysis/2026-07-12-zen5-privileged-lbr-findings.md) `call_auto` bug
hide: every CI host either lacks AMD LBR (self-skips) or is an unprivileged dev
box at `perf_event_paranoid=4` (self-skips), so the rung had **never run live**
before the privileged lane existed.

## Prerequisites

- An **AMD Zen 3+/4/5** host — `grep -m1 amd_lbr_v2 /proc/cpuinfo` must hit
  (Zen 3/4/5 have LbrExtV2; the `ibs_op` PMU is Zen 2+). Verified on **AMD
  Ryzen 9 9950X** (Zen 5, `amd_lbr_v2` + `perfmon_v2` + `ibs`), Linux 6.17.
- Docker (`--cap-add=PERFMON` is a plain cap add; Docker 20.10+ / Linux 5.8+).
- **No host privilege needed.** `CAP_PERFMON` inside the container bypasses
  `kernel.perf_event_paranoid`, so this works even on the Debian/Ubuntu
  `paranoid=4` ("no unprivileged perf at all") default — no `--privileged`, no
  `seccomp=unconfined`, no lowering the host sysctl.

## Run it

```sh
make docker-hwtrace-privileged
```

That builds the `hwtrace` image on the shared bindings base and runs
`make hwtrace-test ibs-test` under Docker's **default** seccomp profile with
`--cap-add=PERFMON` (the target definition is in
[`../../mk/docker.mk`](../../mk/docker.mk)). To confirm the cap is *necessary*
as well as sufficient, run the plain unprivileged image alongside it — the same
AMD tests self-skip without the cap:

```sh
docker run --rm asmtest-hwtrace make hwtrace-test    # no cap: AMD tests self-skip
```

## What green proves

A passing run (captured 2026-07-12 on the Ryzen 9 9950X) exercises, live:

- **`test_hwtrace`** — `1..410`, **0 failures**. The AMD LbrExtV2 tier actually
  runs: live single-shot + snapshot-opt-in capture (honest, never
  empty-yet-complete); the Tier-B loop stitches **beyond a single 16-deep
  window** (`best_insns≈309`, `covered`, `truncated=1` on the over-ring run);
  `#2A` reconstructs at `period=1` and `period=4`; `sample_window` and the
  IBS-Op survey fallback + begin/end split collect real endpoints. LBR
  branch-stack depth reads **16** (16 on every shipping Zen).
- **`test_ibs`** — `1..23`, **0 failures**, run twice (once inside
  `hwtrace-test`, once as the standalone `ibs-test`). `ibs_probe` reports IBS-Op
  **AVAILABLE** (OpSam + BrnTrgt capable); `survey_pid` records retired
  taken-branch edges on the spin-loop back-edge and `survey_process` covers
  **3/3** worker functions.

Still expected-**skipped even on the AMD box** (these are correct skips, not
regressions):

- **AMD MSR-direct** — needs `/dev/cpu/N/msr` + `CAP_SYS_ADMIN`; that is the
  separate `make docker-hwtrace-msr` (`--privileged`) lane.
- **status live-EPERM lane** — can't be exercised as root (the run is root in
  the container, so the unprivileged-EPERM path is unreachable here).
- **Intel PT** — wrong vendor; PT is bare-metal-Intel only.

## RESOLVED (5d8e0d2) — `call_auto` AMD-LBR rung

The [Zen 5 findings §2](analysis/2026-07-12-zen5-privileged-lbr-findings.md)
`trace_call_auto` AMD-LBR completeness bug is **FIXED** (commit `5d8e0d2`); the
findings doc marks it `~~OPEN~~ RESOLVED`, deterministic across 16 privileged
runs (every one escalates `backend=3 insns=77`).

In `test_hwtrace` the `call_auto` case (b) drives 25 taken back-edges; a 16-deep
window cannot hold them, so escalation **MUST** fire — the `# call_auto
escalate:` line must read `backend=3` with the full `insns` count (~77) matching
the single-step baseline. Post-fix, a `truncated=0 (LBR window sufficed)` on this
line can no longer legitimately occur: it is a **REGRESSION**, not a known
finding (see the checklist below).

## Pre-release checklist

This checklist's own staleness is the failure mode it guards against: every item
states the **observation to record**, never the expected verdict. If an
observation contradicts an item, the item is stale — fix the doc in the same
change that records the run.

- [ ] On an AMD Zen 3+/4/5 host: `grep amd_lbr_v2 /proc/cpuinfo` hits.
- [ ] `make docker-hwtrace-privileged` exits **0**.
- [ ] `test_hwtrace` `1..410` (or current plan) with **0** `not ok`; the AMD LBR
      live tests (`ok` on "AMD LBR live …", Tier-B "stitched BEYOND a single
      16-deep window", `#2A`) actually **ran**, not skipped.
- [ ] `test_ibs` reports its full plan (current count) with **0** `not ok`
      **and the live IBS tests actually ran** (no `# SKIP IBS live capture` /
      `# SKIP IBS whole-process capture` lines); `ibs_probe` reports IBS-Op
      **AVAILABLE** via its real-open path (T1 — a substrate-only probe now prints
      `BLOCKED` instead); the `# whole-process:` line reads `3/3 worker functions
      covered` — record the observed count.
- [ ] Only the three expected skips above (MSR-direct, status live-EPERM, PT).
- [ ] Inspect the `# call_auto escalate:` line: escalation **MUST** fire
      (`backend=3 insns=77`). A `truncated=0` here is a **REGRESSION** — file it
      and do not tag; it is not a known issue.
- [ ] For MSR-direct coverage additionally run `make docker-hwtrace-msr` (needs
      the host `msr` module loaded).

## Recorded runs

The checklist above is a reusable template — leave its boxes blank. Record each
observed run here (host, date, the values each item asks for), newest first.

### 2026-07-20 — Ryzen 9 9950X (Zen 5, Family 1Ah), validating agent

Box: `amd_lbr_v2` + `ibs_op`/`ibs_fetch` present, `perf_event_paranoid=4`,
kernel 6.17, Docker (no `--privileged`). Two lanes were run — one for the
positive paths, one for the honesty/blocked path — both **exit 0, 0 `not ok`**:

- **`make docker-hwtrace-privileged`** (CAP_PERFMON, default seccomp — bypasses
  paranoid=4): `test_hwtrace` **`1..658`**, the AMD LBR live tests ran (not
  skipped); `test_ibs` **`1..84`** with **no** `# SKIP IBS …` lines — the live
  IBS lanes genuinely executed. `ibs_probe` printed **`IBS-Op … AVAILABLE`** and
  **`IBS-Fetch … AVAILABLE`** via the real-open path (T1). `# whole-process:
  3/3 worker functions covered` (56 edges) and `# system-wide: 3/3 worker
  functions covered` (58 edges); `phase5: callchain survey succeeds` +
  `callchain-on survey still recovers the spin_loop edge`; the `record-bound`
  checks (base 112, callchain ≥1192, ring still usable) all `ok`.
- **`call_auto escalate` (the regression signal):** `# call_auto escalate:
  rc=0 result=25 used.backend=3 insns=77 truncated=0 (escalated off the LBR
  window)` → `ok - call_auto: escalates past the 16-branch window to a COMPLETE
  trace`. **Escalation fired (`backend=3 insns=77`) — not a regression**; the
  small-routine case is complete without escalation (`insns=5`, `ok`).
- **Honesty/blocked path — `make docker-hwtrace-ibs`** (seccomp=unconfined, NO
  CAP_PERFMON, so paranoid=4 still refuses the open): `ibs_probe` printed
  `IBS-Op: substrate present but sampling is BLOCKED — perf_event_open refused
  (EACCES) …` and `# SKIP ibs_probe: … not openable`, and the 5 live IBS tests
  self-skipped with the real `unavail_reason` (EACCES) — **T1's fix confirmed:
  a substrate-only host prints BLOCKED, never AVAILABLE.** `1..60`, 0 `not ok`.
- Not run here: `docker-hwtrace-msr` (host `msr` module not loaded) and bare-metal
  Intel PT — recorded as gated, not observed.
