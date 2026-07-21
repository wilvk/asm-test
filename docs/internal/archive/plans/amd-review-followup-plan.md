# asm-test — AMD review follow-ups: close the 2026-07-17 findings

> **Context (2026-07-17).** This plan actions the confirmed findings of the
> [AMD hardware review](../../analysis/2026-07-17-amd-hardware-review.md) — an
> adversarial pass over the AMD tiers in which **four of nine** candidate
> findings were refuted and are recorded there as do-not-re-raise. Only what
> survived two independent skeptics, with kernel claims checked against fetched
> primary source at pinned tags, appears below.
>
> Sources: [src/amd_backend.c](../../../../src/amd_backend.c),
> [src/ibs_backend.c](../../../../src/ibs_backend.c),
> [src/branchsnap.c](../../../../src/branchsnap.c),
> [src/hwtrace.c](../../../../src/hwtrace.c). Siblings: the
> [AMD tracing plan](../../plans/amd-tracing-plan.md) (the tiers themselves) and the
> [manual validation doc](../../amd-hardware-validation.md) (which P1 repairs).

> **Status (2026-07-21): reconciled and CLOSED — P1–P8 all landed; archived.**
> Every code/doc item here shipped in the 2026-07-18..20 landing wave (each
> heading and table row below now carries its commit). The sole open item is
> **P9** (self-hosted Zen runner) — credential/infra-gated, re-scoped by
> [amd-review-followup-2-plan.md](../../plans/amd-review-followup-2-plan.md) **T6** and
> tracked in [self-hosted-ci-runners.md](../../implementations/self-hosted-ci-runners.md)
> (currently ◐ 2/6) — so this plan is archived with P9 owned elsewhere.

> Status legend: **P1–P8 *(landed)***, with commits on each heading and table
> row; **P9 is the sole open item** (forward-look, credential/infra-gated). The
> house rule holds: **no untested hardware code** — which is why P4 shipped
> documentation only and its code half stays gated on silicon this project does
> not have.

> **The organising fact.** The AMD *code* is in better shape than the process
> that validates it. No CI lane exercises AMD silicon — the one AMD-targeted job
> is named `hwtrace-privileged (PERFMON; AMD-exact self-skips off Zen)` and runs
> on `ubuntu-latest`. The only real gate is a manual checklist that is stale and
> can pass vacuously. **P1 is therefore ranked above every code fix here**: a
> correct fix behind a gate that cannot see it is not verified.
> **Update 2026-07-21:** P1 landed first, as ranked (`b16f087`); with P1–P8
> shipped, the one remaining instance of the organising fact is P9's runner.

## Items

Severity/effort are the review's verified values. "Silicon" marks work that
cannot be validated on either dev box (Zen 5 9950X, Zen 2 4900HS).

| # | Item | Sev | Effort | Silicon? |
|---|---|---|---|---|
| **P1** | Un-stale [amd-hardware-validation.md](../../amd-hardware-validation.md) — the only AMD gate; 3 of its checks are now wrong, one inverts a regression signal *(landed — `b16f087`)* | **high** | S | no |
| **P2** | `branchsnap` depth check counts a synthetic edge → spurious `truncated` at `use == 15` → a real re-execution *(landed — `0d1dc42`)* | med | S | no |
| **P3** | `IBS_MAX_RECORD` is pre-callchain → the loss heuristic is ~10× short → **silent** ring loss with `lost==0 && throttled==0` *(landed — `0ded0d6`)* | med-high | S | no |
| **P4** | Zen 3 BRS: three docs assert a working path the kernel proves cannot open (one is a Phase marked *landed* specifying the exact missing arm) *(landed — `f58c096`)* | med | S | **docs no / code yes** |
| **P5** | `asmtest_amd_freeze_available()` is dead; one site prints a **flatly false** string to a human; 3 plan sections marked *landed* assert the opposite of shipped code *(landed — `3e5454a`)* | low | S | no |
| **P6** | `asmtest_ibs.h` is not installed, though the guide tells users to `#include` it *(landed — `aff4405`; asmtest_ibs.h installed via Makefile:568/580)* | med | XS | no |
| **P7** | Record the kernel append-order dependency the IBS bit-5 caps gate silently relies on *(landed — `0b60060`)* | low | S | no |
| **P8** | `sample_period` / `period_jitter` are set **nowhere in the tree, not even in tests** — the rounding, clamp and ABI guard have never executed *(landed — `19237a2`)* | low-med | S | no |
| **P9** | Self-hosted Zen runner — closes the class of gap that hid the `call_auto` bug — **the sole open item**; re-scoped by [amd-review-followup-2-plan.md](../../plans/amd-review-followup-2-plan.md) T6, tracked in [self-hosted-ci-runners.md](../../implementations/self-hosted-ci-runners.md) (◐ 2/6) | **high** | L | infra |

---

## P1 — Repair the AMD validation checklist *(landed — `b16f087`)*

**Why first.** [amd-hardware-validation.md](../../amd-hardware-validation.md) has
**exactly one commit** (`43066e0`), timestamped identically to `5d8e0d2` — the
commit that *fixed* the bug the doc describes as open. It has never been
revisited, and it is the only thing standing between AMD-specific code and a
release.

Three defects:

1. **`:91-112`** calls the `call_auto` completeness bug *"not yet fixed"*. It was
   fixed in the same minute the doc was written;
   [the findings doc](../../analysis/2026-07-12-zen5-privileged-lbr-findings.md)
   marks it `~~OPEN~~ RESOLVED`, verified deterministic across 16 privileged runs.
2. **`:124-126`** instructs the maintainer to treat `truncated=0` as *"the known
   open finding, not a clean pass."* Post-fix that result should never occur — so
   the checklist now tells a maintainer to **shrug off a genuine regression**. A
   stale doc has inverted a signal.
3. **`:121-122`** gates on `ibs_probe` printing **AVAILABLE** and on `test_ibs`
   passing. [asmspy-plan.md:131](../../plans/asmspy-plan.md), measured 2026-07-17, records
   that `ibs_probe` *"actively misreports this host … when no survey can open"*
   and that *"`make ibs-test` is vacuously green here"*. Reproduced off-AMD:
   `1..39`, 39 `ok`, 6 `# SKIP` — every passing assertion pure-decode or
   API-shape.

**Acceptance.**

- §"OPEN finding to watch" is retitled/struck to match the findings doc, or
  removed; the `truncated=0` checklist line becomes *"escalation MUST fire
  (`backend=3 insns=77`); a `truncated=0` here is a **regression**, not a known
  issue."*
- The `ibs_probe` AVAILABLE gate is replaced by one that cannot pass vacuously —
  assert a **positive live count** (e.g. `survey_process` covers 3/3 workers),
  not a substrate string. Pairs with the `asmspy-plan.md:131` REMAINING item
  (`ibs_probe` / `ibs-test` must consume `asmtest_ibs_unavail_reason` or attempt
  a real open before claiming availability) — **fix them together**; each is the
  other's evidence.
- A line is added recording that the checklist's own staleness is the failure
  mode it must guard against: every item states the *observation*, not the
  *expected verdict*.

## P2 — `branchsnap`: gate the depth check on `use`, not `n_dec` *(landed — `0d1dc42`)*

[branchsnap.c:344-354](../../../../src/branchsnap.c) builds
`arr[] = [synthetic boundary edge] + [use hardware entries]` and passes
`n_dec = use + 1` into a check counting **hardware slots only**
([amd_backend.c:525-526](../../../../src/amd_backend.c),
`nbr >= asmtest_amd_lbr_depth()` = 16). The synthetic edge holds no slot, so the
trip point is `use >= 15`. A provably complete `use == 15` window is flagged
`truncated`, contradicting the intent stated at
[:311-316](../../../../src/branchsnap.c) (*"armed EXACTLY when it should be"*).

Fails safe — never partial-as-complete — but **not free**: a spurious `truncated`
fails [trace_auto.c:225](../../../../src/trace_auto.c) and escalates to a **second
real in-process execution** (whose own comment warns non-idempotent side effects
run again) or a ~1000× step. The standalone `asmtest_amd_snapshot_trace()` path
has no backstop.

Conditional on a valid in-region exit cookie
([:347](../../../../src/branchsnap.c)); without it `n_dec == use` and the check is
already exact. The trim and the append landed in the same commit (`e9ca70e`); no
TODO, no note.

**Acceptance.**

- The depth check sees the hardware count. Either pass `use` alongside `n_dec`,
  or call `asmtest_amd_decode_reach` — whose header at
  [amd_backend.h:120-126](../../../../src/amd_backend.h) says it exists *"so the
  tail-completed window is not spuriously truncated"*, i.e. the tree already
  holds the fix shape and branchsnap deliberately calls the thin wrapper instead.
- **A fixture reaches `use == 15`** and asserts `truncated == false`. None does
  today — this is the whole reason the bug shipped. Frame it as a near-saturated
  window (a small routine with a ~7-8 iteration loop), **not** a "tiny routine";
  the review refuted that framing (fixtures run `use` ≈ 1-4 and the trim already
  rescues them).
- A negative test pins the boundary: `use == 16` still reports `truncated`.

## P3 — `IBS_MAX_RECORD`: size it for the callchain worst case *(landed — `0ded0d6`)*

[ibs_backend.c:282-283](../../../../src/ibs_backend.c) sizes the largest parsed
record at 112 bytes; its comment still reads *"header + IP + TID + RAW(size +
caps + 8 regs)"* — the **pre-callchain** layout. `git blame`: it landed in
`68b53850` (2026-07-12); `PERF_SAMPLE_CALLCHAIN` landed in `a266b91`
(2026-07-14) and did not touch it. With callchain the record reaches ~1032-1184
bytes (the tree never sets `sample_max_stack`; the kernel defaults to 127 —
`core.c:12454-55`), so the heuristic at
[:596-597](../../../../src/ibs_backend.c) is ~10× short.

`PERF_RECORD_LOST` does **not** cover the gap (`ring_buffer.c:203-206,262-264`:
the space check does `goto fail`; `fail:` only bumps counters — the LOST record
needs space in a *later successful* output). A ring that stays full until drain
yields `lost == 0` **and** `throttled == 0`: **silent loss**, in a lane whose
entire contract is honest gaps, exactly as the file's own comment at
[:593-595](../../../../src/ibs_backend.c) predicts.

The **window lane** is the exposed one:
[`asmtest_ibs_window_begin`:1333](../../../../src/ibs_backend.c) passes opts through
with no callchain gate and drains once. The fetch lane already clears
`cfg.callchain = 0` ([:1645](../../../../src/ibs_backend.c)) — the pattern exists,
applied one lane over.

**Acceptance.**

- `IBS_MAX_RECORD` bounds the **worst case** (make it callchain-aware, or derive
  it from `sample_max_stack`); the stale comment is corrected.
- The window lane either gates callchain out (mirroring `:1645`) or drains
  repeatedly. The `dsz` headroom test exercises a callchain-sized record.
- Separately: either wire the callchain consumer or stop advertising it — the
  deferral is recorded only in `a266b91`'s message while the **public header**
  ([asmtest_ibs.h:76-78](../../../../include/asmtest_ibs.h)) still promises
  *"statistical call-graph context"* for a knob that costs ring bandwidth (and a
  `get_callchain_buffers` allocation that can fail `perf_event_open` with
  `-ENOMEM`) and returns nothing. `zen2-ibs-tracing-plan.md:315-320` marks
  Phase 5 *(landed)* against a goal it did not meet.

## P4 — Zen 3 BRS: make the docs true; leave the code gated *(landed — `f58c096`)*

The review proved against pinned kernel source (v6.10 + v6.14) that the Zen 3
BRS path **cannot open**: the probe and capture use
`PERF_TYPE_HARDWARE / PERF_COUNT_HW_BRANCH_INSTRUCTIONS`, but `amd_brs_hw_config`
demands the event be exactly `AMD_FAM19H_BRS_EVENT = 0xc4`
(`brs.c:96-98,135-136`) while every AMD map renders the generic event as `0x00c2`
→ `-EINVAL`; and it demands `sample_period > lbr_nr` (16) — `brs.c:148-149` —
which the default 1 and the `lbr_period` clamp (≤15) both fail.
[hwtrace.c:275-278](../../../../src/hwtrace.c) then reports `EINVAL` as `AMD_NOHW`,
telling a real Zen 3 owner *"no AMD branch records"*.

**The code fix is hardware-blocked** — no Zen 3 box exists here, and the house
rule forbids shipping it unvalidated. Blast radius is bounded meanwhile:
[trace_auto.c](../../../../src/trace_auto.c) cascades past all three dead AMD rungs
to DynamoRIO/single-step, which trace correctly.

**The documentation is not blocked, and is actively false:**

- [trace-parity-matrix.md:133](../../analysis/trace-parity-matrix.md) asserts an
  unqualified **"✓ BRS"** for Zen 3.
- [amd-tracing-plan.md:173-176](../../plans/amd-tracing-plan.md) — **Phase 0, marked
  _landed_** — already specifies the missing arm: *"Distinguish Zen 3 BRS (needs
  the opt-in `branch-brs` event) from Zen 4 LbrExtV2 … by trying LbrExtV2 first
  and falling back to the BRS event."* It was never built. The plan knew.
- Improvement Phase 6 ([:1314+](../../plans/amd-tracing-plan.md)) is scoped as a *throughput*
  win presuming a working `sample_period=1` baseline the kernel disallows — the
  plan believes Zen 3 works and is merely slow.

**Acceptance.**

- The parity matrix's Zen 3 cell states the truth: the facility exists in
  silicon; **this tree cannot open it** (generic event + period, vs the required
  raw `0xc4` + period ≥ 17).
- Phase 0's BRS-fallback bullet is unmarked as landed, or the phase is annotated
  that this arm alone is outstanding.
- Phase 6 is rescoped: it is not a throughput optimisation but the **only way BRS
  opens at all**. Note that `sample_period=1` is load-bearing for Tier-B overlap
  ([:1332-1336](../../plans/amd-tracing-plan.md)), so BRS must be a distinct Tier-A mode —
  the plan already says so; the framing above it is what is wrong.
- Record that the discriminator a fix needs is **already in-tree**: CPUID
  `0x80000022` is read at [amd_backend.c:166](../../../../src/amd_backend.c) for
  stack depth and distinguishes Zen 3 from Zen 4.
- *(Silicon-gated, out of scope here:)* the probe retry
  (`PERF_TYPE_RAW`, `config = 0xc4`, `sample_period >= 17`) and an
  `AMD_NOHW`-vs-"BRS present, capture mode unsupported" split in
  `asmtest_hwtrace_status`.

## P5 — Retire the freeze probe or wire it; kill the false printf *(landed — `3e5454a`)*

An `nm` scan over `build/` finds one hit for `asmtest_amd_freeze_available`: a
**definition with zero undefined references**. Its gate was deliberately and
correctly removed in `5d8e0d21` (Zen 5 disproved the theory) and replaced with a
stronger unconditional exit-presence check — so this is hygiene, not correctness.
But the stale surface is broad, and one site lies:

- [examples/test_hwtrace.c:235-239](../../../../examples/test_hwtrace.c) prints
  `"PRESENT (single-window Tier-A trusted)"` / `"ABSENT (Tier-A window trusted
  only if it captured the region exit)"` — **both branches false**; Tier-A is
  exit-anchored on every part now. This is printed to a human.
- [amd_backend.h:32-34](../../../../src/amd_backend.h) is stale *by implication*
  (its necessary condition holds; the converse it invites is what
  [hwtrace.c:1051-1061](../../../../src/hwtrace.c) denies). `git blame` shows it
  predates the removal (`37118ec5`) and survived two later commits touching the
  same files.
- [amd-tracing-plan.md:560-570](../../plans/amd-tracing-plan.md) (marked **landed**), `:996`,
  `:1727` assert the opposite of the shipped code, and point at `hwtrace_end_amd`
  while the logic lives in `asmtest_amd_ring_parse_decode`.

**Acceptance.** Either delete the function and its probe test, or give it a
consumer. Either way the printf states what is true, `amd_backend.h`'s comment
stops implying a live gate, and the three plan sections are corrected. The
"deliberate diagnostic" defence does not hold while its only output site prints a
false string.

## P6 — Install `asmtest_ibs.h` *(landed — `aff4405`; asmtest_ibs.h installed via Makefile:568/580)*

[Makefile:528-534](../../../../Makefile) copies ten headers; `asmtest_ibs.h` is not
among them, and `install-shared-hwtrace`
([mk/native-trace.mk:2829](../../../../mk/native-trace.mk)) omits it too — while
[hardware-tracing.md:489](../../../guides/tracing/hardware-tracing.md) shows users
`#include <asmtest_ibs.h>`. The documented usage cannot compile against an
installed package.

**Acceptance.** The header installs and uninstalls with its siblings; the
`clean-room` lane compiles the guide's snippet. Check the same omission for any
other header a guide tells users to include.

## P7 — Record the caps append-order dependency *(landed — `0b60060`)*

`asmtest_ibs_decode_op` never reads the record's caps word
([:49](../../../../src/ibs_backend.c) treats it as padding), and the length check
provably cannot substitute: the kernel appends `IbsBrTarget` (bit 5) then
`IbsOpData4` (bit 10) positionally and cap-conditionally (`ibs.c:1084-1099`), so
`BRNTRGT=0 / OPDATA4=1` is **byte-identical in length** (68) to
`BRNTRGT=1 / OPDATA4=0` with a different `reg[7]`.

**The live path is safe**, and more safely than first claimed: BrTarget is
appended *before* OpData4, so `BRNTRGT=1` forces `reg[7] = IbsBrTarget`, and
`ibs_probe()` hard-gates on CPUID bit 5
([:351-355](../../../../src/ibs_backend.c)). **But nothing in this tree records that
the gate's sufficiency depends on kernel append order.** That is the finding.

Exposure is confined to the exported **pure** decoder
([asmtest_ibs.h:133-142](../../../../include/asmtest_ibs.h)), documented
*"host-independent"* and *"unit-tested on ANY host"*, which sits on the opposite
side of the `__linux__ && __x86_64__` split from the CPUID gate protecting it.

**Acceptance.**

- The dependency is written down at [:105-106](../../../../src/ibs_backend.c),
  whose current comment asserts the one inference the kernel disproves.
- **Preferred:** the pure decoder validates the record's own caps word (bit 5
  before trusting `reg[7]`). Existing fixtures already carry self-consistent caps
  (`0x3ff`, `0x81bff`), so the suite passes unchanged. The obstacle is that
  `ld_u32` is `__linux__ && __x86_64__`-gated ([:89-95](../../../../src/ibs_backend.c))
  — un-gating it is the work, and that gate is the accidental reason this was
  never done.
- Calibration to keep in the comment: perf's own decoder is **weaker**
  (`tools/perf/util/amd-sample-raw.c:192-216` reads `*(rip+6)` with no caps check
  and no length floor). This is hardening, not a defect.
- **Do not claim unreachability.** "No shipping part sets OpData4 with BrnTrgt
  clear" is *reasoned, not proven* — AMD primary source was unobtainable. Say
  *likely*.
- Cheap sibling: `IBS_CAPS_AVAIL` (bit 0) is never checked. Fails safe
  (`EDECODE`, no misread), but `available()==1` does not imply the kernel agrees.

## P8 — Exercise `sample_period` / `period_jitter` at all *(landed — `19237a2`)*

Both are set **nowhere in the tree — not even in tests**;
[examples/test_ibs.c:230](../../../../examples/test_ibs.c) *asserts they are zero*.
So the `/16` rounding, the `< 16 → IBS_DEFAULT_PERIOD` clamp
([:658-660](../../../../src/ibs_backend.c)) and the `struct_size` additive-ABI guard
([:701-704](../../../../src/ibs_backend.c)) have never executed against a non-default
value.

Note this is **not** the refuted "opts are unreachable" claim — NULL is the
designed contract and NULL callers correctly get the Phase-5 defaults. This is
narrower: the non-default path is untested.

**Acceptance.** A pure test drives `ibs_period` across the boundaries (0, 1, 15,
16, 17, a non-multiple of 16) and asserts the rounding and clamp. A `struct_size`
test pins the additive-ABI guard. No live capture needed — this is all pure.

Also fix [asmtest_ibs.h:166-168](../../../../include/asmtest_ibs.h), which calls the
residual-race fix *"a later **opts.system_wide** phase"* — landed in `a266b91`
under a name that never shipped (`ASMTEST_IBS_OPT_SYSTEM_WIDE`, in `flags`).

## P9 — Self-hosted Zen runner *(forward-look — the sole open item; re-scoped by [amd-review-followup-2-plan.md](../../plans/amd-review-followup-2-plan.md) T6, tracked in [self-hosted-ci-runners.md](../../implementations/self-hosted-ci-runners.md), ◐ 2/6)*

The author's own top suggestion, twice recorded:
[amd-hardware-validation.md:10-13](../../amd-hardware-validation.md) (*"a
self-hosted Zen runner would let the CI `hwtrace-privileged` job light these paths
up for free"*) and
[the Zen 5 findings](../../analysis/2026-07-12-zen5-privileged-lbr-findings.md)
§"Suggested next step". It closes the class of gap that hid the `call_auto` bug:
*"every host in CI either lacks AMD LBR (self-skips) or is the unprivileged dev
box (self-skips at `paranoid=4`)"*.

`CAP_PERFMON` alone suffices (no `--privileged`, no `SYS_ADMIN`, no
`seccomp=unconfined`) — measured on Zen 5 — so the lane already exists and works;
it needs only silicon under it. **P1 is the interim substitute and stays valuable
regardless:** a runner covers Zen 5, not Zen 2's IBS-only lane or Zen 3's BRS.

**Acceptance.** `hwtrace-privileged` runs on a Zen host in CI; the manual
checklist shrinks to what the runner cannot cover, and says which those are.
