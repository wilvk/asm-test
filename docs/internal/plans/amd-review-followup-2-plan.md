# asm-test — AMD review follow-ups (round 2): close the 2026-07-20 findings

> **Context (2026-07-20).** This plan actions a fresh review of the AMD tracing
> surface performed after the [2026-07-17 adversarial review](../analysis/2026-07-17-amd-hardware-review.md)
> and its [follow-up plan](amd-review-followup-plan.md) had largely landed
> (`amd-ibs-backend-honesty` + `amd-branchsnap-lbr-docs` series, 2026-07-18..20).
> The round-1 findings P1–P8 are shipped; this round covers what a re-audit of
> the *current* HEAD surfaced — two genuinely new code defects the earlier passes
> missed, plus the doc drift that the fast landing wave left behind.
>
> **Every code:line and doc claim below was independently verified against HEAD**
> (`73c060a`) — the raw audit produced two framing errors that were corrected
> before they reached this plan; see [§ Provenance](#provenance) so they are not
> re-introduced. Sources: [src/ibs_backend.c](../../../src/ibs_backend.c),
> [src/trace_auto.c](../../../src/trace_auto.c), [src/msr_lbr.c](../../../src/msr_lbr.c),
> [src/debug.c](../../../src/debug.c), and the AMD plan/matrix/validation docs.

> Status legend: **planned** unless marked *(landed)*. The house rule holds:
> **no untested hardware code** — which is why T6 (the self-hosted Zen runner)
> ships nothing that cannot self-validate on registered silicon this project does
> not yet have.

> **The organising fact (unchanged from round 1, and reinforced).** The AMD
> *code* is in good shape and getting better; the drift now lives in the *docs
> and plans*, which the 2026-07-18..20 landing wave outran. The single new
> runtime defect (T1) is an honesty-contract violation of exactly the class the
> `amd-ibs-backend-honesty` series set out to close — it slipped through because
> it sits on the OOM path, which no test exercises. The largest real cluster is
> T2 (doc drift), because a stale plan/checklist is what let the round-1
> `call_auto` bug hide in the first place.

## Items

Severity/effort are this review's verified values. "Silicon" marks work that
cannot be validated on either dev box (Zen 5 9950X, Zen 2 4900HS) or the hosted
CI pool.

| # | Item | Sev | Effort | Silicon? |
|---|---|---|---|---|
| **T1** | IBS survey/window lanes discard the export return → an OOM'd export reports a **complete, empty** survey (`OK`, `n==0`) with no loss signal | med | S | no |
| **T2** | Doc drift: the round-1 follow-up plan, the parity matrix's recommendation rows, the orphan review page, the AMD plan's freeze residual, and the manual validation checklist all lag shipped code | high | M | no |
| **T3** | Minor cleanups: stale `g_open_errno`, an orphaned false-freeze comment, and a defensive short-SAMPLE guard asymmetry | low | S | no |
| **T4** | `trace_auto` MSR rung discards a nonempty-truncated partial that the fast rung would return `OK` — a completeness asymmetry (errs safe) | low | S | no |
| **T5** | Extend `ASMTEST_HWDBG` diagnostics into `ibs_backend.c` and `trace_auto.c` — the two AMD TUs the Phase-4 debug facility never reached | low | S | no |
| **T6** | Self-hosted Zen runner — register silicon under the already-built `hw.yml`; the only structural gap left | high | L | **infra** |

---

## T1 — IBS export OOM must not report a complete empty survey *(planned)*

**The defect.** [src/ibs_backend.c:614](../../../src/ibs_backend.c) `eh_export`
returns `-1` when its result `calloc` fails and leaves `out->edges`/`out->n`
untouched. Every branch-stack lane calls it and **discards the return**, then
returns success:

- `survey_pid` — [:1008](../../../src/ibs_backend.c) → `return ASMTEST_IBS_OK`
- `survey_process` — [:1128](../../../src/ibs_backend.c) → `return ASMTEST_IBS_OK`
- `window_end` — [:1463](../../../src/ibs_backend.c) → `return ASMTEST_IBS_OK`
- `survey_fetch_pid` — [:1775](../../../src/ibs_backend.c) (`fh_export`) → `return ASMTEST_IBS_OK`

The public entry points zero `out` on entry (`memset(out, 0, sizeof *out)`), so
on OOM the caller receives `ASMTEST_IBS_OK` with `edges == NULL`, `n == 0`, yet
`branch_samples > 0` — every aggregated edge dropped, presented as a genuinely
empty survey with **no** `lost`/`throttled`/error tell. `n == 0` is also the
legitimate no-branch outcome, so nothing distinguishes lost-to-OOM from
honestly-empty. The IBS-precover consumer in
[src/trace_auto.c:248](../../../src/trace_auto.c) (`nrc != OK || blk.n == 0`)
reads it as "IBS honestly covered no blocks."

**It is an oversight, not a design choice** — the sibling software-clock lane
does it right: [:1367](../../../src/ibs_backend.c) captures
`int xrc = sw_export(&h, out);` and [:1373](../../../src/ibs_backend.c) returns
`EUNAVAIL` on failure. The Op/Fetch lanes just forgot.

**Reachability is low** (the export array is `h->n` entries — strictly smaller
than the `h->cap`-slot hash that already allocated), so this is a
correctness-of-contract fix, not a hot-path one. But the whole lane's advertised
value is honest gaps, and this is a silent one.

**Acceptance.**
- All four lanes capture the export return and, on failure, free their scratch
  and return `ASMTEST_IBS_EUNAVAIL` — mirroring the SW lane at
  [:1367](../../../src/ibs_backend.c)/[:1373](../../../src/ibs_backend.c).
- A pure/fault-injected test proves the contract: with the export allocation
  forced to fail on a run that saw `branch_samples > 0`, the lane returns
  `EUNAVAIL`, never `OK`. (Add a test seam or a size-0-hint path if no allocator
  hook exists — the point is that no test reaches this path today, which is why
  it shipped.)
- The window lane ([:1463](../../../src/ibs_backend.c)) is covered too, not just
  the surveys.

## T2 — Un-stale the AMD docs the landing wave outran *(planned)*

Five separate documents assert things the current code contradicts. Each is a
pure doc edit; group them.

**(a) Round-1 follow-up plan is stale.**
[amd-review-followup-plan.md](amd-review-followup-plan.md) still tags **P1, P2,
P3, P4, P5, P6, P7, P8** `*(planned)*` though all shipped (verified at HEAD:
`b16f087`, `0d1dc42`, `0ded0d6`, `f58c096`, `3e5454a`, header install,
`0b60060`, `19237a2`). A reader is told the release-blocking P1 is unrepaired and
seven fixes are outstanding.
- *Acceptance:* mark P1–P8 `*(landed)*` with their commits; leave **P9** as the
  one open item, and cross-link its non-gated half to T2(e) and its gated half to
  T6.

**(b) Parity-matrix recommendation rows still put AMD LBR primary on Zen 3.**
The capability cell was corrected (`f58c096`,
[trace-parity-matrix.md:134](../analysis/trace-parity-matrix.md) — "BRS in
silicon, NOT openable by this tree"), but the recommendation tables were not:
- [:254](../analysis/trace-parity-matrix.md) (Matrix 7) recommends
  "**AMD LBR (Tier A; Tier-B stitches past 16 live)**" as *Primary* for "AMD Zen 3
  / Zen 4, small routine" — false for Zen 3, where the open is `-EINVAL` →
  `AMD_NOHW` ([hwtrace.c:275-278](../../../src/hwtrace.c)).
- [:324-325](../analysis/trace-parity-matrix.md) (Matrix 8) list
  "AMD LBR → DynamoRIO → …" and "*(LBR sets `truncated`)*" for "AMD Zen 3 / Zen 4";
  on Zen 3 AMD LBR never opens, so it never "sets truncated" — the real
  resolution is DynamoRIO → single-step → emulator.
- *Acceptance:* split the bundled "Zen 3 / Zen 4" cells so the Zen 3 column
  matches [:134](../analysis/trace-parity-matrix.md) — DynamoRIO/single-step
  primary, AMD LBR absent.

**(c) Parity-matrix Matrix 1 mislabels shipped tiers as forward-look.**
[:92](../analysis/trace-parity-matrix.md) ends the AMD LBR row
"…forward-look: MSR-direct, BRS (Zen 3), IBS (Zen 2)." **MSR-direct** is shipped
([src/msr_lbr.c](../../../src/msr_lbr.c) + `trace_auto` rung 1b) and **IBS** is
shipped ([src/ibs_backend.c](../../../src/ibs_backend.c), 1861 lines, installed
header). Only BRS (Zen 3) is genuinely forward-look.
- *Acceptance:* the row states MSR-direct and IBS as shipped; only BRS remains
  forward-look.

**(d) Orphan review page reads "zero IBS code."**
[docs/amd_tracing_review.md:68](../../amd_tracing_review.md) still states "There
is currently zero IBS code in the repo; IBS appears only in planning docs" — now
false. Its P0/P1 items (F5/F7 clamp, F22 mechanism, F24 empty-trace, F27
`struct_size`) have all shipped. It is `orphan: true` (low blast radius) but
marked superseded nowhere.
- *Acceptance:* add a "SUPERSEDED — see [2026-07-17 review](../analysis/2026-07-17-amd-hardware-review.md)
  + this plan" banner at the top; fix or strike the "zero IBS code" line.

**(e) AMD plan freeze residual + validation-checklist P9 shrink.**
- [amd-tracing-plan.md:1669](amd-tracing-plan.md)/[:1682](amd-tracing-plan.md)
  still list the **deleted** `asmtest_amd_freeze_available` among "unconditional
  prototypes to fold into `src/amd_backend.h`." The retirement is described
  correctly elsewhere ([:569](amd-tracing-plan.md), [:578](amd-tracing-plan.md),
  [:1760](amd-tracing-plan.md)); these two are the leftover. *Acceptance:* drop
  the symbol from the Phase-9 hygiene bullet.
- [amd-hardware-validation.md](../amd-hardware-validation.md) never got round-1
  **P9**'s non-gated half: now that `hw.yml` exists, the manual checklist should
  state **what a Zen runner covers vs. what stays manual** (Zen 2's IBS-only lane
  and Zen 3's BRS, neither of which a single Zen 4/5 runner reaches). Its
  historical "what green proves" counts ([:67-80](../amd-hardware-validation.md))
  also cite the 2026-07-12 capture (`1..410` / `1..23`) while the 2026-07-20 run
  is `1..658` / `1..84`. *Acceptance:* add the runner-vs-manual coverage split;
  refresh or explicitly date-stamp the historical counts.

**(f) asmspy-plan IBS residual is itself stale.**
[asmspy-plan.md:131](asmspy-plan.md)'s "REMAINING" clause says
`examples/ibs_probe.c` "still prints AVAILABLE … without attempting an open" —
but it now attempts a real `asmtest_ibs_survey_pid` open and prints
`asmtest_ibs_unavail_reason()` first ([examples/ibs_probe.c:71-86](../../../examples/ibs_probe.c)).
- *Acceptance:* mark that clause closed (the substrate-vs-open honesty fix
  landed for `ibs_probe` and `report_fetch`).

## T3 — Minor code + comment cleanups *(planned)*

Batch three low-severity items:

- **Stale IBS diagnostic errno.** `g_open_errno` is written only on failure
  ([src/ibs_backend.c:887](../../../src/ibs_backend.c)) and never reset to `0` on
  success (the only writes are the init at [:423](../../../src/ibs_backend.c) and
  the failure at [:887](../../../src/ibs_backend.c)). After a `survey_process`
  where one thread's channel open failed (`ESRCH`/`EMFILE`) but the survey
  returned `OK`, a later unrelated `EUNAVAIL` makes
  `asmtest_ibs_unavail_reason()` report the wrong (stale) errno. *Fix:* clear
  `g_open_errno = 0` at the top of each public capture entry point (or on the
  first successful `ibs_chan_open`). *Acceptance:* a test that opens
  successfully after a prior failure sees an empty reason.
- **Orphaned false-freeze comment.**
  [examples/test_hwtrace.c:295-298](../../../examples/test_hwtrace.c) still reads
  "prints this host's actual support so the freeze gate in `hwtrace_end_amd` is
  observable" — the freeze gate was retired (`3e5454a`) and never lived in
  `hwtrace_end_amd`. This is the round-1 P5 residual. *Acceptance:* rewrite the
  block to describe the substrate probe it now heads, or delete it.
- **Short-SAMPLE guard symmetry (defensive, optional).** The hardened test-seam
  parser guards `off + sizeof(header) + 8 <= span` before loading `nr`; the two
  *live* survey drains at [src/hwtrace.c:1539](../../../src/hwtrace.c) and
  [:1805](../../../src/hwtrace.c) do not. **Unreachable** on real kernel records
  (they consume only complete records from a disabled ring), so this closes an
  asymmetry, not a reachable defect. *Acceptance:* add the same guard at both
  sites, or add a one-line comment recording why the drains are safe without it.
  Zero runtime cost either way.

## T4 — `trace_auto` MSR rung: return a truncated partial, like the fast rung *(planned)*

**The asymmetry.** The tail return is
`return ran ? ASMTEST_HW_OK : ASMTEST_HW_EUNAVAIL`
([src/trace_auto.c:442](../../../src/trace_auto.c)). A **fast-tier** truncated
partial that no later rung resets survives to that tail as `HW_OK` +
`truncated`. The **MSR rung 1b**, by contrast,
`call_auto_reset`s ([:349](../../../src/trace_auto.c)) then commits only on
`HW_OK && !trace->truncated` ([:353-355](../../../src/trace_auto.c)); a nonempty
truncated MSR read (window overflow —
[src/msr_lbr.c:190-196](../../../src/msr_lbr.c) fills `trace` then flags
`truncated`) fails that guard, `ran` stays 0, and if block-step
([:380](../../../src/trace_auto.c)) and per-insn ([:414](../../../src/trace_auto.c))
are both unavailable-or-failing, the function returns `EUNAVAIL` +
`used=MECH_NONE` while a usable 16-deep partial sits in `trace`, unreturned.

**This errs safe** (`EUNAVAIL` is never a false-complete) and needs a narrow
config (MSR available, both ptrace rungs down, MSR result nonempty-truncated), so
it is a consistency nit, not a correctness bug. But the same truncated outcome
being `HW_OK` from one rung and `EUNAVAIL` from another contradicts the header
contract "Returns `HW_OK` when some tier ran"
([asmtest_trace_auto.h:201](../../../include/asmtest_trace_auto.h)).

**Acceptance (pick one, deliberately):**
- *Preferred:* when rung 1b produced a nonempty trace, commit it as
  `HW_OK` + `truncated` (set `ran = 1`, stamp `mechanism = MECH_MSR_LBR`) instead
  of discarding on the `!truncated` guard — matching how a fast-tier truncated
  partial is returned. Guard on "nonempty", so the genuinely empty
  `!any_in_region` case ([msr_lbr.c:197-204](../../../src/msr_lbr.c)) still falls
  through.
- *Or:* document at [:353](../../../src/trace_auto.c) that the discard is
  intentional (rung 1b re-runs the routine, so a truncated MSR partial is
  deliberately dropped in favour of a complete stepper result) — and accept that
  `EUNAVAIL`-beside-a-partial is the contract on the all-downstream-down path.
- Either way: a test drives the narrow config (MSR truncated + steppers absent)
  and pins the chosen behaviour.

## T5 — Extend the debug facility into IBS and the cascade *(planned)*

The Phase-4 env-gated logging (`ASMTEST_HWTRACE_DEBUG=1` /
`ASMTEST_AMD_DEBUG=1`, [src/debug.c:18](../../../src/debug.c);
`ASMTEST_HWDBG` at [src/debug.h:34](../../../src/debug.h)) already instruments
`amd_backend.c` (9 sites), `hwtrace.c` (17), `branchsnap.c` (7) and
`msr_lbr.c` (4) — this closes the old F32 "no AMD diagnostics" finding. The two
AMD TUs it never reached are **`ibs_backend.c` (0 sites)** and
**`trace_auto.c` (0 sites)**: on a Zen box an operator can trace an LBR self-skip
but not an IBS open failure or an escalation-rung decision.

**Acceptance.** `ASMTEST_HWDBG` (or a thin `ibs`-scoped alias sharing
`asmtest_hwtrace_debug_enabled`) logs, under the existing env gate:
- in `ibs_backend.c`: each capability-probe return, the `perf_event_open`
  errno on `ibs_chan_open` failure, near-full-ring loss/throttle, and the
  export/EUNAVAIL decision from T1;
- in `trace_auto.c`: which rung committed (or why each was skipped) and the
  final `mechanism`.
- The gate stays the single `getenv`-once cache; no new env var; no output when
  disabled. Keep it out of any async-signal-unsafe context (per
  [debug.h:16](../../../src/debug.h)).

## T6 — Self-hosted Zen runner *(forward-look — infra-gated)*

Round-1 **P9** built the lane and it is correct:
[.github/workflows/hw.yml](../../../.github/workflows/hw.yml) carries
`hwtrace-privileged-zen`, guarded off by `vars.HW_RUNNER_AMD_ZEN`, `dispatch` +
nightly only, and — crucially — it **fails if the AMD paths self-skip** (the
anti-vacuous-green assert). `CAP_PERFMON` alone suffices (measured on Zen 5), so
the lane needs only silicon under it. The interim hosted gate
([ci.yml:1210](../../../.github/workflows/ci.yml)) structurally cannot cover the
AMD-exact paths (they self-skip off-Zen) and has no assert step, so **today no CI
lane exercises AMD silicon** — the exact gap class that hid the round-1
`call_auto` bug.

**This is the one item this project cannot complete unaided** — it needs
registered Zen 4/5 hardware plus a repo admin flipping `HW_RUNNER_AMD_ZEN=1`
(the runner runs privileged capture, so registration is a credentialed,
maintainer-only step per [runners runbook](../ci/runners.md)). Per the house
rule, nothing ships here that the runner would validate; the doc half (name what
the runner covers vs. what stays manual) is folded into **T2(e)** and is the
actionable interim.

**Acceptance.** `hwtrace-privileged-zen` runs green on a registered Zen host
(no self-skip; `call_auto` escalates off the 16-deep window); the manual
checklist (T2(e)) then shrinks to Zen 2 IBS-only + Zen 3 BRS.

---

## Sequencing

1. **T1** — the one runtime defect; land with its OOM-path test.
2. **T2** — the largest real cluster; a single doc-sweep commit (a–f).
3. **T3** — batch the three cleanups.
4. **T4 / T5** — the consistency nit and the diagnostics extension; independent, either order.
5. **T6** — record-and-gate; blocks on hardware + repo-admin credentials.

T1–T5 all validate on the existing hosts (most are host-independent or pure
tests); only T6 is silicon-gated.

## Provenance

This plan rests on a fresh 2026-07-20 read of the AMD surface plus an independent
line-by-line re-verification against HEAD. Two raw-audit framing errors were
caught and corrected here so they are not re-introduced (the Matrix 3 dead-end
convention):

- **Retracted:** an initial claim that the AMD TUs have "zero diagnostic
  logging / no `ASMTEST_AMD_DEBUG`." **False** — the facility exists
  ([src/debug.c](../../../src/debug.c)) and instruments four AMD TUs; old F32
  landed. The accurate residual is the narrow IBS/cascade coverage gap, which is
  **T5** (not a sweeping absence).
- **Corrected mechanism:** the T4 asymmetry was first attributed to the
  `if (ran && !trace->truncated)` early-return at
  [trace_auto.c:316](../../../src/trace_auto.c). That line returns only the
  *non*-truncated case; the actual asymmetry is the tail return at
  [:442](../../../src/trace_auto.c) combined with rung 1b's reset + strict guard.
  Severity confirmed low (errs safe).

Two T2 sub-items (b Matrix 7/8, e freeze residual) and the T3 short-SAMPLE
symmetry note were confirmed against source during this pass; all file:line
references above were re-checked at `73c060a`.
