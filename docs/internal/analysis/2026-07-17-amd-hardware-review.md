# AMD hardware review — adversarial verification of the AMD tiers

Review date: 2026-07-17. Scope: the AMD-specific tracing surface —
[src/amd_backend.c](../../../src/amd_backend.c),
[src/ibs_backend.c](../../../src/ibs_backend.c),
[src/msr_lbr.c](../../../src/msr_lbr.c),
[src/branchsnap.c](../../../src/branchsnap.c) and the AMD paths in
[src/hwtrace.c](../../../src/hwtrace.c) — plus the AMD plan/validation docs.

**Method.** Nine candidate findings were raised by a code read, then each was
attacked by two independent skeptics instructed to *refute* it and to default to
"refuted" absent positive evidence. Every claim resting on kernel or silicon
behaviour had to be checked against **fetched primary source** — not recall.
Kernel citations below are from **pinned tags** (v6.10 / v6.12 / v6.14), after one
verifier caught `master` shifting line numbers mid-review and re-pinned.

**Four of the nine did not survive.** They are recorded in §4 so they are not
re-raised — the same reason [amd-tracing-plan.md](../plans/amd-tracing-plan.md)
Matrix 3 records its dead ends.

Hosts referenced: Zen 5 (Ryzen 9 9950X) and Zen 2 (Ryzen 9 4900HS) — the two dev
boxes. **No Zen 3 silicon exists here**, which is load-bearing for §2.1.

## 1. The headline: AMD's verification, not AMD's code

The AMD code is in better shape than the process that validates it. Three facts
compose badly:

1. **No CI lane exercises AMD silicon.** The one AMD-targeted job is
   `hwtrace-privileged`, whose own name reads
   `(PERFMON; AMD-exact self-skips off Zen)` and which runs on `ubuntu-latest`.
   Its comment in [ci.yml](../../../.github/workflows/ci.yml) calls it *"the ONE
   place the exact AMD LbrExtV2 branch-stack + live IBS paths get exercised end
   to end."* On a non-AMD runner it never does.
2. **The only real gate is the manual checklist** in
   [amd-hardware-validation.md](../amd-hardware-validation.md) — which the doc
   itself frames as the deliberate substitute for a runner.
3. **That checklist is stale, and can pass vacuously.** See §2.5.

Everything AMD-specific — ~3,463 lines of backend plus the hwtrace binding
surface across ten languages — rests on a human remembering to run a document
whose own gates this project has since measured to be false.

## 2. Confirmed findings

### 2.1 Zen 3 BRS is non-functional, and the plan believes it works

**Severity: correctness (Zen 3 / Fam19h only) + false documentation.**

Both the probe ([src/hwtrace.c:253-255](../../../src/hwtrace.c)) and the capture
([:805-806, :820](../../../src/hwtrace.c)) open
`PERF_TYPE_HARDWARE / PERF_COUNT_HW_BRANCH_INSTRUCTIONS`. On Fam19h the kernel
routes `has_branch_stack` events to `amd_brs_hw_config`, which rejects both
properties:

- it requires the event be **exactly** `AMD_FAM19H_BRS_EVENT = 0xc4`
  (`arch/x86/events/amd/brs.c:96-98,135-136`; `perf_event.h:1495`), but every AMD
  event map — including the `amd_zen2_perfmon_event_map` that Fam19h actually
  selects — maps the generic event to `0x00c2` → `amd_is_brs_event()` false →
  `-EINVAL`;
- it requires `sample_period > x86_pmu.lbr_nr` (16, set at `brs.c:60-62` for
  `case 0x19`) — `brs.c:148-149`. The default period is 1; the opt-in
  `lbr_period` clamps to `depth-1` (≤15). Both fail.

`amd_branch_probe` classifies `EINVAL` as `AMD_NOHW`
([src/hwtrace.c:275-278](../../../src/hwtrace.c)), so `hw_classify` tells a real
Zen 3 owner *"no AMD branch records (needs Zen 3 BRS / Zen 4 LbrExtV2)"*
([:385-387](../../../src/hwtrace.c)). `PERF_TYPE_RAW` appears nowhere in `src/`
or `include/`. The two survey opens ([:1347-1348](../../../src/hwtrace.c),
[:1608-1609](../../../src/hwtrace.c)) break identically.

**Blast radius is bounded.** [src/trace_auto.c](../../../src/trace_auto.c)
cascades past all three dead AMD rungs (the sampled open is broken; branchsnap
and [msr_lbr.c:56](../../../src/msr_lbr.c) both gate on `amd_lbr_v2`, absent on
Zen 3) to DynamoRIO/single-step, which trace correctly. No wrong traces, no
partial-as-complete. The cost is a lost fast tier plus a materially misleading
diagnostic.

**Scoping caveat:** the misreport requires a kernel with BRS support (≥5.19). On
an older kernel `lbr_nr = 0` and `core.c:408` returns `EOPNOTSUPP`, making the
"no hardware" string arguably accurate. BRS is non-functional either way.

**The code fix is legitimately hardware-blocked** by the house "no untested
hardware code" rule — there is no Zen 3 box. **The documentation defects are
not:**

- [trace-parity-matrix.md:133](trace-parity-matrix.md) asserts an unqualified
  **"✓ BRS"** in the Zen 3 column. Factually false.
- [amd-tracing-plan.md:173-176](../plans/amd-tracing-plan.md) — **Phase 0, marked
  _landed_** — specifies the exact missing arm: *"Distinguish Zen 3 BRS (needs
  the opt-in `branch-brs` event) from Zen 4 LbrExtV2 … by trying LbrExtV2 first
  and falling back to the BRS event."* Never implemented. The plan already knew
  the answer.
- Improvement Phase 6 ([:1314+](../plans/amd-tracing-plan.md)) is scoped as a
  *throughput* win whose acceptance criterion presumes a working
  `sample_period=1` baseline — which the kernel proves impossible. The plan
  believes Zen 3 works and is merely slow.

The signal a fix would need is **already in-tree**: CPUID `0x80000022` is read at
[amd_backend.c:166](../../../src/amd_backend.c) for stack depth and is itself a
Zen3/Zen4 discriminator.

**Sub-claim that died:** "zero CPU-family detection anywhere" — false as stated.
Vendor detection exists ([hwtrace.c:214](../../../src/hwtrace.c)) and CPUID
feature detection exists in four places. What is genuinely absent is **family**
detection and any raw-`0xc4` path.

### 2.2 branchsnap's synthetic edge inflates the depth check by one

**Severity: correctness (Zen 4/5), fails safe, but costs a real re-execution.**

[src/branchsnap.c:344-354](../../../src/branchsnap.c) builds
`arr[] = [synthetic boundary edge] + [use trimmed hardware entries]` and passes
`n_dec = use + 1` into a depth-ceiling check that counts **only hardware slots**:
[amd_backend.c:525-526](../../../src/amd_backend.c),
`if ((int)nbr >= asmtest_amd_lbr_depth())` with depth 16. The synthetic edge
occupies no slot, so the effective trip point is `use >= 15`, not `use >= 16`. A
**provably complete, trimmed `use == 15` window is flagged `truncated`** —
contradicting the design intent stated two paragraphs up in the same comment
block ([:311-316](../../../src/branchsnap.c): *"armed EXACTLY when it should
be"*).

Nothing compensates: no cap on `use`, no `-1`, `truncated` is write-only-true,
and the caller ([hwtrace.c:1115-1132](../../../src/hwtrace.c)) only ORs more
truncation in. The sampled sibling gates on the **raw** hardware count
([hwtrace.c:1017](../../../src/hwtrace.c)) with no synthetic append —
branchsnap is the only caller that inflates `nbr`.

**The direction is safe** (a false positive on an honesty flag, never
partial-as-complete) **but not free:** a spurious `truncated` fails
[trace_auto.c:225](../../../src/trace_auto.c) and escalates to a second real
in-process execution — whose own comment warns non-idempotent side effects run
again — or to a ~1000× step. On the standalone `asmtest_amd_snapshot_trace()`
path there is no backstop at all.

Conditional on a valid in-region exit cookie
([branchsnap.c:347](../../../src/branchsnap.c)); without it `n_dec == use` and
the check is exact. The trim and the append landed in the **same commit**
(`e9ca70e`); `git log -S"n_dec"` returns only it — no TODO, no note. **No fixture
reaches `use == 15`.**

**Sub-claim that died:** "costs the tiny-routine case the snapshot path exists to
serve" — tiny routines are low-`use` by construction (fixtures run `use` ≈ 1-4)
and the trim already rescues them. The bug fires only at the boundary value
`use == 15` — a near-saturated window, reachable by a small routine with a ~7-8
iteration loop. Report the mechanism, not the "tiny routine" framing.

**Fix: gate on `use`, not `n_dec`.** One line.

### 2.3 `IBS_MAX_RECORD` is a stale constant → silent ring loss

**Severity: honesty (the lane's core contract), unknown to the author.**

[src/ibs_backend.c:282-283](../../../src/ibs_backend.c) sizes the largest parsed
record at 112 bytes, and its comment still reads *"header + IP + TID + RAW(size +
caps + 8 regs)"* — the **pre-callchain** layout. `git blame`: it landed in
`68b53850` (2026-07-12); `PERF_SAMPLE_CALLCHAIN` landed in `a266b91`
(2026-07-14) and **did not touch it**. With callchain the record reaches
~1032-1184 bytes (the repo never sets `sample_max_stack`; the kernel defaults it
to 127 — `core.c:12454-55`). The near-full-ring loss heuristic at
[:596-597](../../../src/ibs_backend.c) is therefore ~10× short.

This matters because **`PERF_RECORD_LOST` does not cover the gap** — verified
against `ring_buffer.c:203-206,262-264`: the non-overwrite space check does
`goto fail`, and `fail:` only bumps counters; the LOST record needs its own space
in a *later successful* `__perf_output_begin`. A ring that stays full until drain
yields `lost == 0` **and** `throttled == 0` — silent loss with no honesty signal,
exactly as the file's own comment at [:593-595](../../../src/ibs_backend.c)
predicts.

**The window lane is the exposed one:** `asmtest_ibs_window_begin`
([:1333](../../../src/ibs_backend.c)) passes opts straight through with **no
callchain gate** and drains exactly once. Contrast `asmtest_ibs_survey_fetch_pid`,
which explicitly clears `cfg.callchain = 0`
([:1645](../../../src/ibs_backend.c)) — the author knows the pattern and applied
it in the other lane.

**Related, smaller:** the callchain consumer was **knowingly deferred**
(`a266b91`: *"the optional 5b asmspy `--graph` consumer is deferred"*), but the
deferral lives only in a commit message while the **public header**
([include/asmtest_ibs.h:76-78](../../../include/asmtest_ibs.h)) still advertises
*"statistical call-graph context"* for a knob that costs ring bandwidth (plus a
`get_callchain_buffers` allocation that can fail `perf_event_open` with
`-ENOMEM`) and returns nothing.

**Sub-claim that died:** "every sample carries a full user stack" — overstated.
`exclude_callchain_kernel=1` selects a frame-pointer walk, typically ~1-2 entries
on `-fomit-frame-pointer` binaries, and records are sized to actual `nr`. The
*average* cost is small — irrelevant, since a max-record bound must bound the
worst case.

### 2.4 `asmtest_amd_freeze_available()` is dead code with live, false docs

**Severity: hygiene (no wrong runtime behaviour) — but one site lies to a human.**

An `nm` scan over every object in `build/` finds exactly one hit:
`build/amd_backend.o: T _asmtest_amd_freeze_available` — a **definition with zero
undefined references anywhere**. Its only callers are a self-consistency probe
([examples/test_hwtrace.c:231-232](../../../examples/test_hwtrace.c)) asserting
only `a ∈ {0,1}` and `a == b`. The
[src/amd_backend.h:34](../../../src/amd_backend.h) declaration has **zero**
consumers — the test carries its own hand-copied forward decl, exactly the
pattern [amd_backend.h:8-10](../../../src/amd_backend.h) exists to eliminate.

The gate was deliberately and **correctly** removed in `5d8e0d21` (Zen 5 silicon
disproved the theory; see the
[Zen 5 findings](2026-07-12-zen5-privileged-lbr-findings.md) §2) and replaced
with a strictly stronger unconditional exit-presence check. So this is hygiene,
not correctness. `git blame` proves
[amd_backend.h:32-34](../../../src/amd_backend.h) predates the removal
(`37118ec5`, 2026-07-10) and survived two later commits that edited the same
files.

Sites still asserting the removed gate is live:

1. [examples/test_hwtrace.c:235-239](../../../examples/test_hwtrace.c) —
   **flatly false, and printed to a human**:
   `"PRESENT (single-window Tier-A trusted)"` /
   `"ABSENT (Tier-A window trusted only if it captured the region exit)"`. Both
   branches are wrong; Tier-A is exit-anchored on **every** part now.
2. [src/amd_backend.h:32-34](../../../src/amd_backend.h) and
   [src/amd_backend.c:92-99](../../../src/amd_backend.c) — stale *by
   implication*: the "without freeze you cannot trust it" necessary condition
   remains true; the converse it invites is what
   [hwtrace.c:1051-1061](../../../src/hwtrace.c) denies.
3. [amd-tracing-plan.md:560-570](../plans/amd-tracing-plan.md) (marked
   **landed**), `:996`, `:1727` — the written record currently believes the
   opposite of the shipped code. (They also point at `hwtrace_end_amd` while the
   logic lives in `asmtest_amd_ring_parse_decode`.)

The "kept as a deliberate diagnostic" defence fails: its only output site prints
a false string.

### 2.5 The AMD validation checklist is stale and can pass vacuously

**Severity: process — this is the only AMD gate that exists.**

[amd-hardware-validation.md](../amd-hardware-validation.md) has **exactly one
commit** (`43066e0`), timestamped `2026-07-12 22:44:27` — **identical** to
`5d8e0d2`, the commit that *fixed* the bug the doc describes as open. It was
written describing a live bug in the same batch that closed it, and has never
been revisited. Three of its gates are now wrong:

- **`:91-112`** — *"the `trace_call_auto` AMD-LBR completeness bug is **not yet
  fixed**"*. It was, that same minute;
  [the findings doc](2026-07-12-zen5-privileged-lbr-findings.md) marks it
  `~~OPEN~~ RESOLVED`, verified deterministic across 16 privileged runs.
- **`:124-126`** instructs the maintainer to *"treat a `truncated=0` on this line
  as a **known false-green**, not a pass."* Post-fix that result should never
  occur — so a genuine **regression** now reads as an expected known-issue. The
  stale doc has inverted a regression signal.
- **`:121-122`** gates on `ibs_probe` printing **AVAILABLE** and on `test_ibs`
  passing. [asmspy-plan.md:131](../plans/asmspy-plan.md), **measured 2026-07-17**
  — five days later, in a doc maintained through today — records that
  `examples/ibs_probe.c` *"actively misreports this host … when no survey can
  open"* and that *"`make ibs-test` is vacuously green here"*, with all six live
  paths self-skipping. Reproduced off-AMD while preparing this review:
  `make ibs-test` reports `1..39` with 39 `ok` and 6 `# SKIP` — every passing
  assertion is pure-decode or API-shape.

So the release checklist gates on two signals this project has since measured to
be false, and a third that inverts.

### 2.6 The IBS caps gate is sufficient — by an unrecorded accident

**Severity: latent (unreachable on known silicon), but the dependency is
undocumented.**

`asmtest_ibs_decode_op` never reads the record's own caps word:
`IBS_RAW_REG_OFF(k) = 4 + 8*k` ([:49](../../../src/ibs_backend.c)) treats the
leading `[u32 caps]` as pure padding, and `ld_u32` is called only at `:554`,
`:567`, `:1598` — never at payload offset 0. Its length check provably cannot
substitute: the kernel appends `IbsBrTarget` (cap bit 5) then `IbsOpData4` (cap
bit 10) **positionally and cap-conditionally** (`ibs.c:1084-1099`), so
`BRNTRGT=0 / OPDATA4=1` yields 68 bytes with `reg[7] = IbsOpData4` —
**byte-identical in length** to `BRNTRGT=1 / OPDATA4=0` with
`reg[7] = IbsBrTarget`. `IBS_RAW_MIN_BYTES` is exactly 68. The comment at
[:105-106](../../../src/ibs_backend.c) asserts the one inference the kernel
disproves.

**The live path is safe — and more safely than first thought.** Because BrTarget
is appended *before* OpData4, `BRNTRGT=1` forces `reg[7] = IbsBrTarget`
regardless, and `ibs_probe()` hard-gates on CPUID `Fn8000_001B` EAX bit 5
([:351-355](../../../src/ibs_backend.c)) with every live entry calling
`asmtest_ibs_available()` first. **But that sufficiency rests on a kernel
append-order guarantee recorded nowhere in this tree.** That is the finding —
not the decode.

Exposure is confined to the **exported pure decoder**
([include/asmtest_ibs.h:133-142](../../../include/asmtest_ibs.h)) fed a
synthetic, foreign, or VM record — which is precisely what a decoder documented
*"host-independent"* and *"unit-tested on ANY host"* invites, and which sits on
the **opposite side** of the `#if defined(__linux__) && defined(__x86_64__)`
split from the CPUID gate that protects it (`:89-97` vs `:100`).

Worth recording for calibration: **perf's own userspace decoder is weaker** —
`tools/perf/util/amd-sample-raw.c:192-216` reads `*(rip+6)` positionally with
zero caps checks and no length floor at all. asm-test is already stricter than
upstream convention.

**Sub-claim that died:** "`IBS_CAPS_BRNTRGT` appears exactly once in the tree" —
false; 11 occurrences across 4 files. Accurate: *never `#define`d, and the
**record's** caps word is never read*. The CPUID bit **is** checked, spelled
`(1u << 5) /* BrnTrgt */`.

**Not established:** the reachability argument. "No shipping AMD part sets
OpData4 with BrnTrgt clear" is **reasoned, not proven** — both verifiers tried
AMD primary source (IBS spec 69205, CPUID spec 25481) and got navigation chrome
only. It rests on an inference chain (OpData4 ⟹ Zen ⟹ BrnTrgt) supported by this
repo's own dev-box measurements (Zen 2 `EAX=0x3FF`, Zen 5). Treat as *likely
unreachable*, never *"cannot happen"*.

### 2.7 Smaller confirmed items

- **`asmtest_ibs.h` is not installed.** [Makefile:528-534](../../../Makefile)
  copies ten headers; `asmtest_ibs.h` is not among them, and
  `install-shared-hwtrace` ([mk/native-trace.mk:2829](../../../mk/native-trace.mk))
  does not either — while
  [hardware-tracing.md:489](../../guides/tracing/hardware-tracing.md) tells users
  to `#include <asmtest_ibs.h>`. The documented usage cannot compile against an
  installed package.
- **[include/asmtest_ibs.h:166-168](../../../include/asmtest_ibs.h) is stale.**
  It calls the residual-race fix *"a later **opts.system_wide** phase"* — a phase
  that landed in `a266b91`, under a member name that never shipped (it is
  `ASMTEST_IBS_OPT_SYSTEM_WIDE` in `flags`).
- **`sample_period` / `period_jitter` are set nowhere in the tree — not even in
  tests.** [examples/test_ibs.c:230](../../../examples/test_ibs.c) *asserts they
  are zero*. So the `/16` rounding, the `< 16 → IBS_DEFAULT_PERIOD` clamp
  ([:658-660](../../../src/ibs_backend.c)) and the `struct_size` additive-ABI
  guard ([:701-704](../../../src/ibs_backend.c)) have never executed against a
  non-default value. A narrow but real test gap.
- **`IBS_CAPS_AVAIL` (bit 0) is never checked.** The kernel's `__get_ibs_caps`
  returns `IBS_CAPS_DEFAULT` (AVAIL|FETCHSAM|OPSAM, **BrnTrgt clear**) when bit 0
  is clear, so a host with bit 0 clear + bit 5 set (plausible VM CPUID synthesis)
  makes `asmtest_ibs_available()` return 1 while the kernel emits 60-byte
  payloads → `EDECODE`, zero edges, no misread. **Fails safe**, but
  `available()==1` does not imply the kernel agrees.

## 3. Forward-look: the IBS-Op data channel (already a recorded non-goal)

The IBS-Op decoder reads `reg[1]` (RIP), `reg[2]` (OpData) and `reg[7]`
(BrTarget) only ([:51-53, :111-113](../../../src/ibs_backend.c)). Regs 0/3/4/5/6
are unread, including `reg[5] = IbsDcLinAd` (sampled data linear address),
`reg[4] = IbsOpData3` (DcMiss + miss latency), `reg[3] = IbsOpData2` (NUMA /
coherence source), and the low 32 bits of `reg[2]` (completion-to-retire and
tag-to-retire latency) — verified against kernel `amd-ibs.h` at v6.14. They are
**already in the buffer**: `:755` requests `PERF_SAMPLE_RAW`, which makes the
kernel populate regs 0..6, and `:107` already validates the length covers all 8.
Decoding them needs no new privilege and no new attr bit. Since the decoder
returns `NOEDGE` for every non-branch op, roughly half of all samples are
discarded — precisely the population carrying data addresses.

**This is an additive opportunity, not a defect, and it is already decided.**
[data-flow-tracing-plan.md:94](../archive/plans/data-flow-tracing-plan.md), under
**Non-goals**: *"Hardware-only data flow — the silicon has no operand-value
channel; only PTWRITE instrumentation, **PEBS/IBS statistical address sampling**,
or PT-path + replay **(deferred; see the analysis note)**."*
[data-flow-capture.md:257](data-flow-capture.md) identified it and `:339` gives
it a verdict; `git log -S "PEBS/IBS"` shows the identify→defer chain 27 minutes
apart the same night. **Textbook considered deferral — do not re-raise it as an
oversight.**

The category matters: IBS yields an **address, never a value**, so it cannot
underpin def-use or taint, and
[include/asmtest_ibs.h:21-23](../../../include/asmtest_ibs.h) carries the
invariant that IBS *"must NEVER feed the exact insns[]/blocks[] parity
contract."* It is not competing with the data-flow tier for the same job.

## 4. Refuted — do not re-raise

Recorded with the reason, per the Matrix 3 convention.

| Claim | Verdict |
|---|---|
| **MSR path ignores an LbrExtV2 top-of-stack rotation** | **Fully refuted.** Transplants *Intel* architecture onto AMD. LbrExtV2 is not a circular buffer: internal register renaming pins `From[0]/To[0]` to the TOS — `arch/x86/events/amd/lbr.c:213-217` says so verbatim, then hard-codes `cpuc->lbr_stack.hw_idx = 0` *because* no rotation is possible. `amd_pmu_lbr_read` is a plain `for (i = 0; i < lbr_nr; i++)` over `MSR_AMD_SAMP_BR_FROM + idx*2` — byte-for-byte the indexing [msr_lbr.c:174-177](../../../src/msr_lbr.c) already uses. `msr-index.h:901-904` defines exactly one field for `MSR_AMD_DBG_EXTN_CFG` (`LBRV2EN`); the claimed TOS pointer **does not exist**. Rotation is Intel-only (`intel/lbr.c:737`). The repo already documents the property that makes the linear read correct ([amd-msr-direct-lbr-plan.md:37](../archive/plans/amd-msr-direct-lbr-plan.md)). **The code is correct as written.** |
| **The IBS opts struct is unreachable dead code** | **Framing refuted.** NULL is the *designed contract*: [asmtest_ibs.h:86](../../../include/asmtest_ibs.h) — *"Pass NULL for defaults"*; [ibs_backend.c:685-687](../../../src/ibs_backend.c) applies the Phase-5 defaults *"even to NULL / legacy callers"*; `a266b91` records the NULL-everywhere state as intent verbatim. `COUNT_CYCLES`/`NO_JITTER` are opt-**outs** from live defaults — a CLI flag could only degrade sampling. `SYSTEM_WIDE` is a spec'd privileged library opt-in, correctly outside the lane's advertised unprivileged envelope, and **is tested live**: `docker-hwtrace-privileged` ([mk/docker.mk:587-588](../../../mk/docker.mk)) runs `ibs-test` under `--cap-add=PERFMON`, and `test_live_system_wide` ([examples/test_ibs.c:640-694](../../../examples/test_ibs.c)) asserts it. `sample_period`'s CLI absence is a documented position ([hardware-tracing.md:501-505](../../guides/tracing/hardware-tracing.md)). Only the two stale/test items in §2.7 survive. |
| **The `RipInvalidChk` gap misdiagnoses real hardware** | **Impact refuted.** The premise is right — [:117/:121](../../../src/ibs_backend.c) reads `IbsOpData` bit 38 though AMD defines it only under CPUID `Fn8000_001B_EAX[7]`, which `ibs_probe()` never checks (Linux does, at `ibs.c:1149/:1185`). But the affected silicon population is **empty**: Family 10h is the only IBS family lacking RipInvalidChk, and its BKDG (#31116 rev 3.48) hardwires *"EAX 5 BrnTrgt … = 0"* — so the existing bit-5 gate rejects it first. Every family setting BrnTrgt=1 also sets RipInvalidChk=1 (F12h/F14h/F16h BKDGs, all *"Value: 1"*). Even bypassing the probe, the kernel omits `reg[7]` on such a part (`ibs.c:1167`) → 60 bytes → `EDECODE` before bit 38 is read. Worth at most an explicit bit-7 check or a comment recording why bit 5 suffices — **not a bug fix**. |
| **The unread IBS regs are an unnoticed gap** | **Refuted as a defect;** survives only as the forward-look in §3. The "never became a plan row" claim was a **false negative from grep term selection** — the plan text reads *"address sampling"*, which `data address` / `DcLinAd` cannot match. It is a recorded, dated non-goal. |

## 5. What was and was not checked against primary source

**Checked against fetched primary source:** the Linux kernel at pinned tags for
§2.1 (v6.10 + v6.14 `amd/brs.c`, `amd/core.c`, `amd/lbr.c`, `perf_event.h`),
§2.3 (v6.12 `core.c`, `callchain.c`, `ring_buffer.c`, UAPI), §2.6 (v6.12 + v6.14
`amd/ibs.c`, plus `tools/perf/util/amd-sample-raw.c`), §3 (v6.14 `amd-ibs.h`,
`msr-index.h`), §2.2 (v6.10 `amd/lbr.c`), and both refutations in §4 that rest on
kernel behaviour. **AMD primary docs** (BKDGs #31116, #41131, #43170, #42300,
#48751; CPUID spec #25481) were fetched for the `RipInvalidChk` refutation only.

**Not established — flag if cited:**

- §2.6's reachability (see there). AMD primary source was unobtainable.
- §2.6's dependency on kernel append order is verified for v6.12/v6.14 but is
  **unpinned in this tree** — nothing records that the bit-5 gate's sufficiency
  depends on it. That *is* the finding.
- §2.1's misreport scoping is kernel-version-dependent (≥5.19).
- **Nothing here was validated on Zen 3 silicon.** §2.1 and §2.2 are proven by
  source reading against hardware this project does not have. The kernel chains
  are strong; the runtime behaviour is inferred. Say so when acting on them.
