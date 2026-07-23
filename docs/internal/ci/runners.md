# Self-hosted CI runners — registration & security-posture runbook

**What this is:** the one runbook an operator with a spare machine follows to
attach a hardware-trace runner to this repo — registered, correctly labeled,
ephemeral, and security-hardened — so the hardware-exact capture lanes that
self-skip on GitHub's hosted pool (AMD LbrExtV2 + live IBS, bare-metal Intel PT,
AArch64 CoreSight, the macOS/KVM clean rooms) get continuous coverage the moment
silicon is attached. It is the operator half of
[self-hosted-ci-runners.md](../implementations/self-hosted-ci-runners.md); the CI
half is [`.github/workflows/hw.yml`](../../../.github/workflows/hw.yml).

This is deliberately an **internal, operational** document (security posture, not
user-facing material) — it lives under `docs/internal/`, which the Sphinx site
excludes, so nothing links it from the published guides.

> **Why the posture matters.** GitHub's own hardening guide is blunt that
> self-hosted runners "should almost never be used for public repositories": a
> fork PR that reaches a self-hosted runner can run arbitrary code on your
> machine. Every rule below exists to make that impossible. Read the
> [Security posture](#security-posture) section as a constraint, not advice.

## What green proves (and what it does not)

A green `hw.yml` lane proves the **hardware-exact live capture paths actually
ran** on real silicon — not just that the target builds and the detect-and-skip
gating chain is intact (that is what the hosted
`hwtrace-privileged` bitrot gate in
[`ci.yml`](../../../.github/workflows/ci.yml) already proves). Each self-hosted
job asserts on its own log and **fails on a self-skip** (mirroring the
`gccanon-attach` pattern in `ci.yml`): a lane that silently self-skipped tested
nothing, which is a failure here, not a pass. See
[amd-hardware-validation.md](../amd-hardware-validation.md) for the manual
counterpart these lanes replace.

## Registration flow

Runner software is **pinned** (`v2.335.1`, released 2026-06-09) and its tarball
verified before use, mirroring the repo's
[`scripts/third-party-digests.txt`](../../../scripts/third-party-digests.txt)
habit. Repo-level registration needs **repo admin** and a registration token that
**expires one hour** after it is minted — generate it at the keyboard of the
target machine.

1. **Get a registration token** (repo admin). Either:
   - UI: repo → Settings → Actions → Runners → **New self-hosted runner** (this
     page prints the exact `config.sh` line with a fresh token baked in), or
   - REST: `POST /repos/{owner}/asm-test/actions/runners/registration-token`
     (`gh api -X POST /repos/<owner>/asm-test/actions/runners/registration-token
     -q .token`). The paired `.../remove-token` de-registers.

2. **Download + verify the pinned runner** on the target box (Linux x64 shown;
   swap `linux-arm64` / `osx-arm64` per lane):

   ```sh
   RUNNER_VERSION=2.335.1
   base=https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}
   tarball=actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz
   curl -fSLO "${base}/${tarball}"
   # Verify against the SHA-256 GitHub publishes on the release page /
   # `gh api repos/actions/runner/releases/latest`. RECORD the value you
   # verified in the status table below — do not skip this step.
   shasum -a 256 "${tarball}"          # must equal the recorded digest
   mkdir -p actions-runner && tar -xzf "${tarball}" -C actions-runner
   ```

   > The per-platform SHA-256 is release-specific and published by GitHub with
   > each release; this runbook does not hard-code it (recording a digest we did
   > not verify on the box would defeat the point). Fill the
   > [status table](#operator-status-table) with the value the box computes,
   > confirmed equal to GitHub's published digest for `v2.335.1`.

3. **Configure** (ephemeral, labeled — see [Label scheme](#label-scheme)):

   ```sh
   ./config.sh \
     --url https://github.com/<owner>/asm-test \
     --token <REGISTRATION_TOKEN> \
     --ephemeral \
     --labels <lane-label>            # e.g. amd-zen
   ```

   `config.sh` adds the defaults `self-hosted`, the OS (`linux`/`macOS`), and the
   arch (`x64`/`ARM64`) automatically; `--labels` adds the custom lane label on
   top. Label matching is case-insensitive and `runs-on` requires **all** listed
   labels, so a job keyed `[self-hosted, linux, x64, amd-zen]` picks only a
   runner that carries every one.

4. **Run.** For a persistent-identity shakedown, `./run.sh` once and confirm
   "Listening for Jobs". For production, use the ephemeral loop below — never
   `./svc.sh install` a persistent service.

### JIT / automated re-registration (recommended for production)

An **ephemeral** runner de-registers after exactly one job, so a compromised job
cannot persist on the runner identity. To keep a box available, loop: mint a
fresh JIT config, run once, repeat.

```sh
# Mint a JIT config (repo admin token). runner_group_id=1 is the repo default group.
gh api -X POST /repos/<owner>/asm-test/actions/runners/generate-jitconfig \
  -f name="amd-zen-$(hostname)-$$" \
  -F runner_group_id=1 \
  -f 'labels[]=self-hosted' -f 'labels[]=linux' -f 'labels[]=x64' -f 'labels[]=amd-zen' \
  -q .encoded_jit_config > /tmp/jit
./run.sh --jitconfig "$(cat /tmp/jit)"    # runs one job, then exits
```

`generate-jitconfig` requires `name`, `runner_group_id`, and `labels` (1-100
items); `work_folder` defaults to `_work`. Wrap the mint→run pair in a systemd
unit (Linux) or launchd plist (macOS) that restarts on exit — that IS the
ephemeral autoscaling loop GitHub recommends (autoscale only with ephemeral,
never persistent, runners).

The loop is scripted:
[`scripts/runner-jit-loop.sh`](../../../scripts/runner-jit-loop.sh)
`<owner/repo> <lane-label> <runner-dir>` mints a JIT config per iteration
(unique runner name each cycle), runs exactly one job, and re-registers — with
a 60 s backoff on mint failure. Deploy it as a systemd **user** unit plus
`loginctl enable-linger` so it survives logout (template below; adjust paths);
`systemctl --user stop` + resetting the lane's `HW_RUNNER_*` variable is the
power-down flow. Stopping while a runner is idle can leave a stale runner
record; GitHub reaps offline ephemeral runners, so no manual cleanup is needed.

```ini
# ~/.config/systemd/user/gha-runner-<lane>.service
[Unit]
Description=GitHub Actions standing ephemeral runner loop (<lane> lane)
After=network-online.target

[Service]
ExecStart=<repo>/scripts/runner-jit-loop.sh <owner>/asm-test <lane> <runner-dir>
Restart=always
RestartSec=30
Environment=PATH=/usr/local/bin:/usr/bin:/bin

[Install]
WantedBy=default.target
```

> **Recorded posture deviations of the standing form** (vs. the one-shot
> registration the table's earlier rows used): the mint step needs `gh`
> authenticated with repo-admin **standing on the box** — bounded by `hw.yml`
> having no `pull_request` trigger and the owner-actor guard, so only
> owner-initiated `main` code ever runs here, but it is a credential the
> "dedicated, no-credentials box" rule says a runner box should not hold. And
> on a box with no passwordless sudo the unit runs as the **primary user**,
> not a dedicated `runner` account (the docker-group root-equivalence caveat
> above applies either way). Both are accepted, recorded tradeoffs for
> unattended nightly coverage on a dev box; a purpose-built CI box should
> still follow the dedicated-user flow.

> Caveat carried from the research: the REST docs page omits the run command; the
> `--jitconfig` flag name is confirmed in the runner source
> (`actions/runner` `src/Runner.Common/Constants.cs`), not the REST page.

## Label scheme

Custom labels are set at `config.sh`/JIT time; the defaults (`self-hosted`, OS,
arch) are added automatically. `runs-on` is cumulative — the runner must hold
every listed label.

| Lane | `runs-on` | Custom `--labels` | Runs | Doc/task |
|---|---|---|---|---|
| AMD Zen live capture | `[self-hosted, linux, x64, amd-zen]` | `amd-zen` | `make docker-hwtrace-privileged` | T3 |
| Bare-metal Intel PT | `[self-hosted, linux, x64, intel-pt]` | `intel-pt` | `make docker-hwtrace-privileged` **and** `make docker-hwtrace-pt-live` (`ASMTEST_REQUIRE_PT=1` — fail-not-skip) | T5 |
| AArch64 CoreSight board | `[self-hosted, linux, arm64, coresight]` | `coresight` | `make docker-hwtrace` (the image is where `libopencsd` lives) once [coresight-live-decode.md](../implementations/coresight-live-decode.md) lands | T5 (dark until [coresight #T4](../implementations/coresight-live-decode.md)) |
| Apple-Silicon tart host | `[self-hosted, macos, arm64, tart]` | `tart` | `make osx-vm-test` | T6 |
| Bare-metal KVM Linux | `[self-hosted, linux, x64, kvm]` | `kvm` | `make docker-osx-bindings` | T6 |

Unused custom labels auto-delete within 24 h; `--no-default-labels` suppresses
the OS/arch defaults if a lane ever needs to (none here do).

## Security posture

This is the load-bearing half of the runbook. Each rule maps to a repo setting or
a `hw.yml` invariant.

- **No fork code ever reaches a runner.** The self-hosted jobs live in a
  **separate** workflow, [`hw.yml`](../../../.github/workflows/hw.yml), which has
  **no `pull_request` trigger and never `pull_request_target`** — only
  `workflow_dispatch` + a nightly `schedule` (which always runs `main`). The
  hosted `pull_request`-triggered jobs stay in `ci.yml`; putting a self-hosted
  `runs-on` there would execute fork-PR code on your hardware. Keeping them in
  different files is a **security decision**, not organization.
- **Maintainer approval gate.** Every self-hosted job carries
  `environment: hw-runners`, an environment with a required reviewer — the run
  pauses for maintainer approval before it touches a runner.
- **Least privilege.** `hw.yml` sets `permissions: contents: read` and passes
  **no secrets** to any hw job. A leaked hw job cannot write the repo or exfil a
  token it never received.
- **Variable guard, so a job never queues forever.** Each job is
  `if: vars.HW_RUNNER_<LANE> == '1'`. With the variable absent or `0` the job
  **skips instantly** instead of queuing against a runner that does not exist —
  the failure mode the macOS Track E deferral explicitly avoided. Flipping one
  variable to `1` lights exactly one lane.
- **Dedicated, non-sudo runner user.** The runner service runs as a dedicated
  `runner` account with no sudo. **Honest caveat:** on the Linux boxes that
  account must be in the `docker` group to run the `docker-*` lanes, and
  **docker-group membership is root-equivalent** on that host. The box must
  therefore be **dedicated to CI, hold no credentials, and be rebuildable from
  notes** — treat a compromise of any hw job as a compromise of the whole box.
- **Fork-PR approval tightened repo-wide.** Set fork pull-request workflows to
  **"Require approval for all external contributors"** (the default only covers
  first-time contributors). Belt to the no-`pull_request`-trigger suspenders.
- **Runner groups (only if this repo joins an org).** Runner groups are an org
  feature and do not apply to repo-level runners. If the repo ever moves into an
  org, put the hw runners in a **non-default** group with a "Selected
  repositories" access policy and leave the public-repo access override **OFF**.

## Repo settings checklist (needs admin — apply once, then check here)

> **Credential gate.** Applying these needs **repo-admin** access this host does
> not have. They are recorded here as an operator checklist; a credentialed
> maintainer applies them and ticks each box. `hw.yml` is written to be **safe
> before any of these exist** — an absent `HW_RUNNER_*` variable reads as "not
> `1`", so every job skips and the workflow is green with zero settings applied.

- [ ] Settings → Actions → General → **Fork pull request workflows**: select
      **"Require approval for all external contributors."**
- [ ] Settings → Environments → create **`hw-runners`** with the maintainer as a
      **required reviewer** (up to 6 allowed; one approval suffices) and a
      **deployment-branch policy of `main`**.
- [x] Settings → Secrets and variables → Actions → **Variables** → create all
      five, each set to **`0`**:
      `HW_RUNNER_AMD_ZEN`, `HW_RUNNER_INTEL_PT`, `HW_RUNNER_CORESIGHT`,
      `HW_RUNNER_MACOS_TART`, `HW_RUNNER_KVM`. *(Done 2026-07-23 — all five
      exist; `HW_RUNNER_INTEL_PT` deliberately sits at `1` while its standing
      runner is live, per the operator status table.)*

Operator rule (records the power-cycle contract): **power a box down ⇒ set its
`HW_RUNNER_*` variable back to `0`**, so the next scheduled run skips the lane
instead of failing at a pickup timeout.

## The workflow

[`.github/workflows/hw.yml`](../../../.github/workflows/hw.yml) carries every
self-hosted job. It merges and stays green with **zero** runners (all jobs
variable-guarded off), then each lane lights up by flipping one repo variable and
registering the matching runner:

- **Triggers:** `workflow_dispatch` + nightly `schedule: '0 5 * * *'` (two hours
  before `ci.yml`'s `0 7 * * *` nightly, so a single one-box runner never sees
  the two contend). **No `push`, no `pull_request`.**
- **Concurrency:** a static `group: hw-self-hosted`, `cancel-in-progress: false`
  — a manual dispatch overlapping the nightly serializes rather than racing.
- **Jobs** (each `if: vars.HW_RUNNER_<LANE> == '1' && github.actor ==
  github.repository_owner`, `environment: hw-runners`):
  `hwtrace-privileged-zen` (T3), `hwtrace-pt-baremetal` +
  `hwtrace-coresight-board` (T5). T6's `cleanroom-tart` / `cleanroom-kvm-osx` are
  **not written yet, on purpose**: their hard precondition is
  [macos-cleanroom-lanes](../implementations/macos-cleanroom-lanes.md) #T2/#T6
  green *locally first*, and wiring ahead of that buys a job that can only fail
  its own shakedown in CI.

Verify the scaffold with no hardware (repo admin):

```sh
gh workflow run hw.yml && gh run watch   # all jobs "skipped"; run green in seconds
```

Verified after each job was added — 2026-07-22, with all three lanes present and
every `HW_RUNNER_*` absent:
[run 29920565410](https://github.com/wilvk/asm-test/actions/runs/29920565410)
completed in ~12 s with `hwtrace-privileged-zen`, `hwtrace-pt-baremetal` and
`hwtrace-coresight-board` all **skipped** (an all-skipped run is a non-failing
conclusion). That is the property that lets these jobs land ahead of the
hardware.

## Operator status table

Fill this in as boxes come online (one row per registered runner). It is the
recorded form of each hardware gate.

| Lane | Runner tarball SHA-256 (verified on box) | Box (SoC / kernel) | Registered | `HW_RUNNER_*` | Live-lane last green |
|---|---|---|---|---|---|
| amd-zen | `4ef2f25285f0ae4477f1fe1e346db76d2f3ebf03824e2ddd1973a2819bf6c8cf` (v2.335.1, verified on box 2026-07-22 == GitHub's published digest) | Ryzen 9 9950X (Zen 5, `amd_lbr_v2` present) | ☐ (ephemeral one-shot; de-registered after the proof run) | `0` | ✅ [run 29897214772](https://github.com/wilvk/asm-test/actions/runs/29897214772) — 2026-07-22 |
| intel-pt | `4ef2f25285f0ae4477f1fe1e346db76d2f3ebf03824e2ddd1973a2819bf6c8cf` (v2.335.1, verified on box 2026-07-23 == GitHub's published digest) | MacBookPro15,2 / Core i7-8559U (Coffee Lake), kernel `7.1.4-1-t2-noble`, **bare metal** (no `hypervisor` flag), `intel_pt` type=10 `nr_addr_filters=2`, `perf_event_paranoid=2` | ☑ **STANDING** (JIT/ephemeral loop: `gha-runner-intel-pt` systemd user unit + linger on the box, deployed 2026-07-23) | `1` (left on for the nightly) | ✅ dispatches [29999081537](https://github.com/wilvk/asm-test/actions/runs/29999081537) + [29999251602](https://github.com/wilvk/asm-test/actions/runs/29999251602) — 2026-07-23, consecutive runs on freshly-minted ephemeral runners (re-registration loop proven); first `0 5 * * *` nightly pending |
| coresight | _(record)_ | _(AArch64 ETM/ETE + sink)_ | ☐ | `0` | — |
| tart | _(record for osx-arm64)_ | _(Apple Silicon)_ | ☐ | `0` | — |
| kvm | _(record)_ | _(bare-metal `/dev/kvm`)_ | ☐ | `0` | — |

> **amd-zen local pre-registration proof (T3 step 2, 2026-07-22).** Before any
> runner is registered, the target itself was proven green on the Ryzen 9 9950X
> (Zen 5) dev box (`amd_lbr_v2` present): `make docker-hwtrace-privileged` →
> **666 `ok` / 0 failed**, zero `# SKIP AMD LBR live capture`, zero
> `# SKIP IBS live capture`, and `# call_auto escalate: rc=0 result=25
> used.backend=3 insns=77 truncated=0 (escalated off the LBR window)`. This
> separates a hardware problem from a CI problem — the AMD-exact live paths run
> on this silicon. **The CI lane then ran green live** ([run
> 29897214772](https://github.com/wilvk/asm-test/actions/runs/29897214772),
> 2026-07-22): a `v2.335.1` runner was registered `--ephemeral --labels amd-zen`
> on this box, `HW_RUNNER_AMD_ZEN` flipped to `1`, `gh workflow run hw.yml`
> dispatched, and `hwtrace-privileged-zen` executed on the `amd-zen` runner with
> the assert step passing (`AMD LbrExtV2 + live IBS ran (no self-skip);
> call_auto escalated off the LBR window` — `# 667 passed, 0 failed`). Because
> this was a **one-shot ephemeral** proof, the runner de-registered after the job
> and `HW_RUNNER_AMD_ZEN` was reset to `0` per the power-down rule — so the
> `amd-zen` row above shows `☐ (ephemeral one-shot)`. For an **unattended
> nightly**, register a standing runner via the JIT/ephemeral loop and leave
> `HW_RUNNER_AMD_ZEN=1`.

> **intel-pt local pre-registration proof (T5 step 1-2, recorded 2026-07-21).**
> Same shape as the amd-zen proof above, and it answers the shakedown question T5
> raises before anyone touches the runner: **is the `intel_pt` PMU visible INSIDE
> the container?** Yes — measured on the box above, where
> `docker run --rm --cap-add=PERFMON asmtest-hwtrace make hwtrace-pt-live` ran
> `1..632`, **631 passed / 0 failed**, stable across 4 consecutive runs, with the
> PT tier live (self-JIT'd routine traced under PT capture, TNT follow on both the
> taken and not-taken walks, `SET_FILTER` accepted, AUX truncation reported). So
> the Docker-first rule holds here — no host-native fallback and no
> `perf_event_paranoid` lowering is needed, because `CAP_PERFMON` bypasses
> `paranoid=4`. `make docker-hwtrace-pt-live` prints the PMU node
> (`/sys/bus/event_source/devices/intel_pt/type`) at the top of every run, so the
> answer stays visible in the log rather than only here. Full record:
> [intel-hardware-validation.md](../intel-hardware-validation.md) "Recorded runs".
>
> ~~**What remains for this lane is registration, not engineering:** register a
> `v2.335.1` runner on that box with `--labels intel-pt` (verify the tarball
> SHA-256 on the box per the flow above), set `HW_RUNNER_INTEL_PT=1`, dispatch
> `hw.yml`, approve the `hw-runners` environment, confirm green, then power down
> per the same rule as amd-zen.~~ **DONE 2026-07-23 — the CI lane ran green live**
> ([run 29997961188](https://github.com/wilvk/asm-test/actions/runs/29997961188)):
> a `v2.335.1` runner (tarball SHA-256 re-verified on this box == GitHub's
> published digest, same linux-x64 value the amd-zen row records) was registered
> `--ephemeral --labels intel-pt` on the i7-8559U box, `HW_RUNNER_INTEL_PT`
> flipped to `1`, `gh workflow run hw.yml` dispatched, and `hwtrace-pt-baremetal`
> executed on the `intel-pt` runner: `make docker-hwtrace-privileged` →
> **`# 649 passed, 0 failed`** with the PT tier live (`ok 157-167` — self-JIT'd
> routine under PT capture, TNT follow both walks, 4 KiB AUX truncation,
> SET_FILTER accepted `object-relative`, anon-region decode-time fallback;
> `# pt nr_addr_filters: 2`), then the fail-not-skip step
> `make docker-hwtrace-pt-live` (`ASMTEST_REQUIRE_PT=1`) → **`1..644`,
> `# 644 passed, 0 failed`**, with the in-container PMU node printed at the top
> of the log. The Zen and CoreSight jobs skipped on their `0` variables, as
> designed. One-shot ephemeral: the runner de-registered itself after the job and
> `HW_RUNNER_INTEL_PT` was reset to `0` per the power-down rule — so the row above
> shows `☐ (ephemeral one-shot)`. **Observed settings note:** the `hw-runners`
> environment currently has **no protection rules**, so the dispatch proceeded
> without an approval pause — the required-reviewer checklist item above remains
> unticked (an admin choice: adding it would also pause every unattended nightly
> run for approval). ~~The **nightly** half of T5's Done-when still needs a STANDING
> runner (JIT/ephemeral loop) — the same deferred deployment choice the amd-zen
> row records.~~ **STANDING runner deployed 2026-07-23, from the box itself:**
> `scripts/runner-jit-loop.sh` under the `gha-runner-intel-pt` systemd user unit
> (linger on), `HW_RUNNER_INTEL_PT` left at `1`. The loop is proven end to end by
> two **consecutive** dispatches, each picked up by a freshly JIT-minted ephemeral
> runner that de-registered after its one job:
> [run 29999081537](https://github.com/wilvk/asm-test/actions/runs/29999081537)
> (`hwtrace-pt-baremetal` green — `# 649 passed, 0 failed` privileged tier,
> `1..644` / `# 644 passed, 0 failed` require-mode PT, Zen + CoreSight skipped on
> their `0` variables) then
> [run 29999251602](https://github.com/wilvk/asm-test/actions/runs/29999251602)
> (green again on the loop's next runner identity). Also applied while there: the
> three missing lane variables (`HW_RUNNER_CORESIGHT`/`_MACOS_TART`/`_KVM`)
> created at `0`, completing that checklist bullet. Remaining observation, not
> engineering: the first `0 5 * * *` **scheduled** run landing green closes T5's
> "dispatch AND schedule" Done-when bullet — confirm with
> `gh run list --workflow=hw.yml` after 05:00 UTC and record it here.

## Hardware & credential gates (recorded, per CLAUDE.md)

Per [CLAUDE.md](../../../CLAUDE.md), only **hardware** and **credentials** are
legitimate self-skip gates; everything installable is added, not skipped. This
whole workstream is gated:

- **Credentials:** repo admin for registration tokens (one-hour expiry),
  environments, variables, and fork-PR settings.
- **Hardware:** an AMD **Zen 4/5** box (LbrExtV2 live floor is **Zen 4**, not
  Zen 3 — the Zen 3 BRS arm was never built in this tree; the `ibs_op` PMU is
  Zen 2+), a bare-metal **Intel PT** x86-64 box, an **AArch64 CoreSight** board
  (only to activate T5's dark CoreSight job, whose decode is
  [coresight-live-decode.md](../implementations/coresight-live-decode.md)'s
  scope), an **Apple-Silicon** Mac, and a bare-metal **`/dev/kvm`** Linux box
  (the Docker-OSX lane is `/dev/kvm`-hardware-gated — the variable guard, not the
  target's `/dev/kvm` hard-error, is its skip mechanism). Each gate's recorded
  form is its `HW_RUNNER_*` variable at `0` plus its row above.

## References

- Adding self-hosted runners, registration token (one-hour expiry) —
  <https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/adding-self-hosted-runners>
- REST + JIT (`registration-token`, `generate-jitconfig`) —
  <https://docs.github.com/en/rest/actions/self-hosted-runners?apiVersion=2022-11-28>
- Ephemeral + autoscaling —
  <https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/autoscaling-with-self-hosted-runners>
- Runner release `v2.335.1` (2026-06-09) —
  <https://github.com/actions/runner/releases/latest>
- Public-repo hardening ("should almost never be used for public repositories") —
  <https://docs.github.com/en/actions/security-for-github-actions/security-guides/security-hardening-for-github-actions>
- Labels (defaults, case-insensitive matching, cumulative `runs-on`) —
  <https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/using-labels-with-self-hosted-runners>
- Fork-PR approval settings + `pull_request_target` risk —
  <https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/enabling-features-for-your-repository/managing-github-actions-settings-for-a-repository>
- Environments (required reviewers, deployment-branch policy) —
  <https://docs.github.com/en/actions/managing-workflow-runs-and-deployments/managing-deployments/managing-environments-for-deployment>
