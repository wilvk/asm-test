# Make `auto` reliably capture — one shot is not a policy — implementation

> **A per-gap brief cut from [38](38-live-feed-completion-roadmap.md)**, and the
> repair of the complaint that reads *"start and arm a process, it starts then
> stops, and the pane says `refused: no session is running`."* Two independent
> halves produce that one sentence: **`auto` gives up after a single unlucky
> candidate**, and **the session-lifecycle state machine mishandles a capture that
> ends on its own**. Neither is a hardware gate. Both are logic, and logic is
> testable on every lane.
>
> **The headline is host-shaped, and backwards.** The picker has two samplers: the
> AMD IBS-Op *entry-arrival* rule (the strong evidence) and a portable
> software-clock *residency* rule (the weak one). The weak one gets a documented
> retry walk over up to `AUTO_SW_TRIES` ranked candidates. The strong one gets
> **one candidate and no walk at all** — `auto_pick` ranks `nc` candidates
> internally, reports the count, and returns only `cands[0]`
> ([asmspy.c:2634-2649](../../../cli/asmspy.c#L2634)), so the walk's guard
> `attempt + 1 < ncand` ([asmspy.c:3746](../../../cli/asmspy.c#L3746)) sees
> `ncand == 0` and never fires. **On an AMD box where the IBS path actually opens,
> the better sampler produces the less resilient capture.**
>
> Authored 2026-07-29, verified against HEAD `0b52704`. **Originally measured on a
> Ryzen 9 4900HS** (family 0x17, Zen 2 / Renoir, `perf_event_paranoid=2`, no
> `amd_lbr_v2`) — the IBS-without-LBR box
> [amd-hardware-validation.md](../amd-hardware-validation.md) calls the only place
> that degradation path runs. **Re-verified 2026-07-29 against HEAD `54b004a` on a
> Ryzen 9 9950X** (family 26 / `0x1A`, Zen 5, model 68), `perf_event_paranoid=4`,
> `ibs_op` + `ibs_fetch` + `swfilt` present, **`amd_lbr_v2` present**. On the 9950X
> at `paranoid=4`, `asmtest_ibs_available()` still reports IBS present (it never
> reads `paranoid`), but `perf_event_open` is refused (`EACCES`) for **both** IBS-Op
> and `SW_TASK_CLOCK` unprivileged, so `--sample` and `--dataflow --auto` both
> self-skip (*"needs perf_event_paranoid<=2 or CAP_PERFMON"*) — auto takes the IBS
> branch and does **not** fall back to the sw sampler. **So the IBS-vs-sw inversion
> this brief headlines reproduces on this box only with CAP_PERFMON or a lowered
> `paranoid` (e.g. `make docker-hwtrace-privileged`);** the picker logic itself is
> host-independent and testable on every lane. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Status (2026-07-29) — ☐ 0/6.** Not started; re-verified still 0/6 against HEAD
> `54b004a` on the Zen 5 box (2026-07-29).

## Why this work exists

`auto` must clear three sequential gates, and each fails differently per host.

**Gate 1 — a sampler must exist.** `asmtest_ibs_available()` is a pure substrate
probe: CPUID vendor (`is_amd()`, [ibs_backend.c:404-410](../../../src/ibs_backend.c#L404)),
the `Fn8000_001B` capability bits, the `ibs_op` PMU node, and the kernel `swfilt`
bit ([ibs_backend.c:428-477](../../../src/ibs_backend.c#L428)). It deliberately
**never reads `perf_event_paranoid`** — *"if IBS exists but perf is locked down, the
sw open would be refused by the same lockdown, and one skip naming CAP_PERFMON
beats two"* ([asmspy.c:2801-2806](../../../cli/asmspy.c#L2801)). IBS is AMD-only
three times over (vendor check, the `#if __linux__ && __x86_64__` gate at
[ibs_backend.c:330](../../../src/ibs_backend.c#L330), and the header contract).

**Gate 2 — the 400 ms window must observe something.** `AUTO_WINDOW_MS 400`
([asmspy.c:2559](../../../cli/asmspy.c#L2559)) is a compile-time constant and is
**not reachable from the wire**: the serve grammar's `ms` is wired only to
`asmspy_engine_sample` ([asmspy.c:3665](../../../cli/asmspy.c#L3665)), i.e. `sample`
mode. An idle target yields *"no function was observed being ENTERED"* and the
session is over. There is no retry — one window, one verdict.

**Gate 3 — the picked region must be re-entered** within `DFP_ENTRY_WAIT_MS 10000`
([dataflow_ptrace.c:213](../../../src/dataflow_ptrace.c#L213)), overridable by
`ASMTEST_DF_ENTRY_WAIT_MS` — the only one of the three timings that is tunable at
all.

The walk exists precisely because gate 3 fails routinely, and the code says so:
*"Residency's winner can be a function that never re-enters (main-shaped), so one
try is a coin flip"* ([asmspy.c:2669-2673](../../../cli/asmspy.c#L2669)). The test
suite already pins the disagreement it protects against —
[test_autoregion.c:306-336](../../../cli/test_autoregion.c#L306) case 15: the entry
rule picks the hot *callee*, the residency rule picks the looping *caller*, *"and the
correct pick is next in the sw ranking — the contract the candidate walk exists
for."* The repo understands the failure mode completely. It simply never wired the
recovery to the path this host takes.

**The second half.** When a pass does end — cleanly, skipped, or one-shot by design
— the desktop never learns. `InspectState::active` is mutated in five places
([inspect_door.cpp](../../../desktop/src/ui/inspect_door.cpp) 68, 181, 281, 452,
594) and every one is a user action; nothing reconciles it against a self-ended
session, and :281 sits inside `if (session.growing())` (the guard at :263) so it is
unreachable once the footer lands. `active` keeps `[Auto]` forever, `budget_can_start` refuses every
frame, and the pane offers only Swap. Clicking Swap sends `stop` — and the host's
command loop runs `serve_reap()` at the **top**, before dispatch
([asmspy.c:4189](../../../cli/asmspy.c#L4189)), which clears `joinable`, so the stop
branch at [:4200](../../../cli/asmspy.c#L4200) then refuses the very command whose
precondition it just destroyed. **A first, non-redundant stop for a self-finished
session is always refused.** Reproduced with no victim process:

```
printf '{"cmd":"stop"}\n{"cmd":"quit"}\n' | ./build/asmspy --serve
→ {"k":"err","reason":"no session is running","cmd":"stop"}
```

The desktop renders that as `refused: %s`
([inspect_door.cpp:222](../../../desktop/src/ui/inspect_door.cpp#L222)) in the same
frame as a contradictory success toast, *"session ended: max"*.

## What already exists (verified 2026-07-29)

- **The ranking is already pure and tested on every host.**
  [asmspy_autoregion.h:5-9](../../../cli/asmspy_autoregion.h#L5) states the rule:
  *"Pure (no ptrace, no perf, no allocation, no I/O), so the decision is
  unit-testable on ANY host."* `cli/test_autoregion.c` is 37 checks built with **no
  backend link** ([mk/cli.mk:423-425](../../../mk/cli.mk#L423)), run from
  [cli_smoke.sh:40](../../../cli/cli_smoke.sh#L40) — which states the doctrine this
  brief extends: *"the sampler feeding them is AMD-IBS hardware that self-skips off
  an AMD host, so these checks carry the real burden on every host."*
- **Two walk implementations, both inline and neither testable.** The CLI's in
  `cmd_dataflow` ([asmspy.c:2874-2914](../../../cli/asmspy.c#L2874)) and serve's in
  `SM_AUTO` ([asmspy.c:3736-3764](../../../cli/asmspy.c#L3736)) are the same
  algorithm, duplicated, tangled with the engine call.
- **`serve_emit_pick`** already reports each attempt on the wire with a
  sampler/rule label (`"ibs-op"`/`"entry"` vs `"sw-clock"`/`"residency"`) and an
  `attempt/total` pair — the honest-substitution channel already exists; the IBS
  path just hardcodes `0, 0, 1, 1` ([asmspy.c:3733](../../../cli/asmspy.c#L3733)).
- **The lanes.** `cli-smoke` runs on `ubuntu-latest` **and** `ubuntu-24.04-arm`
  (real Neoverse-N2, not qemu). `docker-cli-ibs` exists
  ([mk/cli.mk:689](../../../mk/cli.mk#L689)) but is referenced by **no**
  workflow. **No CI lane has AMD silicon**: the self-hosted `amd-zen` lane in
  `hw.yml` is guarded by `vars.HW_RUNNER_AMD_ZEN`, currently `0` and de-registered
  ([ci/runners.md:276](../ci/runners.md)). So on CI this brief's logic must be
  proven **without** a sampler, or it is not proven at all.
- **The precedent for exactly that** is `test_hwtrace.c`'s synthetic branch stack
  and `test_ibs.c`'s synthetic raw records: *"validated WITHOUT capture hardware …
  Runs on any Linux x86-64 host with Capstone (incl. this Zen 2 box, where live AMD
  capture self-skips)."*

## Tasks

### T1 — extract the candidate walk into a pure decision  (S)

**Goal.** One host-independent function that answers *"this candidate did not run —
what next?"*, so the policy is unit-testable and stops existing twice.

**Steps.**
1. Add to [`cli/asmspy_autoregion.h`](../../../cli/asmspy_autoregion.h) — the
   established home for pure picker decisions — a walk state + step function taking
   `(rc, attempt, ncand)` and returning advance / stop / exhausted, with the reason
   string for the wire. No ptrace, no perf, no allocation, no I/O: the file's
   standing rule.
2. Replace **both** inline walks ([asmspy.c:2874-2914](../../../cli/asmspy.c#L2874)
   and [:3736-3764](../../../cli/asmspy.c#L3736)) with calls to it. The duplication
   is why the IBS gap survived: the sw path was fixed once and the other copy was
   never revisited.

**Tests.** New cases in `cli/test_autoregion.c` (no backend link, runs on every
lane): advance on `NEVER_RAN` while candidates remain; stop on any other rc; stop
when exhausted and report *exhausted*, not *never ran*; a single-candidate list
never advances; `ncand == 0` is a legal input meaning "no list" and must not
underflow — **the exact expression that hides today's bug**.

**Done when.** `make cli-smoke` green on both the x86-64 and arm64 lanes; reverting
either call site to its inline copy fails a named check.

### T2 — give the IBS path the ranked candidates it already computed  (M, depends on: T1)

**Goal.** Close the inversion: the strong sampler gets at least the recovery the
weak one has.

**Steps.**
1. Change `auto_pick` ([asmspy.c:2563](../../../cli/asmspy.c#L2563)) to the same
   shape `auto_pick_sw` already uses ([:2697](../../../cli/asmspy.c#L2697)) —
   `(auto_cand_copy *out, int max_out)` returning the count. The ranked array is
   **already there**: `asmspy_autoregion_rank` fills `nc` candidates and the
   function reports *"of %zu candidates"* before discarding all but `cands[0]`.
   Copy names rather than borrowing, exactly as the sw path does — the JIT map is
   freed on the way out and borrowed pointers would dangle.
2. Set `ncand` from it in both callers so the T1 walk engages. Keep the `"ibs-op"` /
   `"entry"` labels and now emit **real** per-candidate weight/sites in
   `serve_emit_pick` instead of the hardcoded `0, 0, 1, 1`.
3. Name the tries bound for what it now covers on both paths; `AUTO_SW_TRIES`
   ([:2673](../../../cli/asmspy.c#L2673)) is no longer sw-only.

**Tests.** `test_autoregion`: the rank function already returns an ordered list, so
assert the *adapter* preserves order, count and independent name storage. Then the
end-to-end bar in `cli_smoke.sh`: on a host where the IBS leg self-skips this
still must not silently pass — assert the skip reason names `perf` or the
substrate, mirroring the existing sw-leg guard against *"an empty/vague reason (the
--sample lesson)"* ([cli_smoke.sh:658-661](../../../cli/cli_smoke.sh#L658)).

**Done when.** On an AMD box with the IBS path live (CAP_PERFMON or `paranoid<=2`;
e.g. `make docker-hwtrace-privileged` — on this 9950X at `paranoid=4` it self-skips)
a first candidate that never re-enters walks to the second and says so on the wire;
reverting the signature fails a named check; the arm64 lane stays green (both
samplers self-skip there — see Constraints).

### T3 — an empty window is a retry, not a verdict; and put the window on the wire  (M)

**Goal.** Stop turning "the target was idle for 400 ms" into "this target has no
regions."

**Steps.**
1. When a pick returns *ran but nothing qualified* (`nc == 0` /
   `ncand == 0` — **not** the self-skip case, which stays a skip), re-sample up to a
   small bounded number of times before reporting `ASMSPY_REGION_NEVER_RAN`. Keep the
   two facts distinct exactly as the code already does: *"`<0` = the SAMPLER
   self-skipped (perf refused); `0` = it ran and nothing qualified. Two different
   facts about two different subsystems, so they must not share a code or a reason"*
   ([asmspy.c:3696-3698](../../../cli/asmspy.c#L3696)).
2. Thread the sample window as a start param (the serve grammar already validates
   `ms`; it is simply wired to the wrong engine) and as a CLI flag, defaulting to
   `AUTO_WINDOW_MS` so nothing changes unasked. Surface it in the capture pane
   beside the existing `steps` / `continuous` checkboxes.
3. Report each empty window on the wire so a retry is visible, not silent — reuse
   the `serve_emit_pick` attempt/total channel.

**Tests.** The retry *policy* is pure — put it in `test_autoregion` with the T1 walk
(empty window advances until the bound, then reports honestly). The param plumbing
is asserted in `cli_smoke.sh` and in the desktop's `test_shell` start-params case,
both of which run with no sampler.

**Done when.** A deliberately idle target reports an honest *"idle window, N
attempts"* rather than a bare `NEVER_RAN`; the window is settable from the GUI and
the CLI; the default is byte-identical to today.

### T4 — `continuous` re-arms through a quiet window  (M)

**Goal.** Make the checkbox mean what it says.

Today the re-arm loop clears `stop_loop` **only** on a successful pass
([asmspy_engine.c](../../../cli/asmspy_engine.c), the `DF_PTRACE_OK`/`FAULT` arm);
`DF_PTRACE_NEVER` leaves it at its initialised `1`, so the loop breaks. A continuous
session therefore ends the first time the region is quiet for one entry wait, while
the label promises *"re-arm and keep capturing until Stop"*.

**Steps.**
1. Under `continuous`, treat `DF_PTRACE_NEVER` as *quiet, still armed* and re-arm,
   bounded, instead of breaking. Preserve the existing distinction that a
   never-produced session still reports `NEVER_RAN` on the first pass — the change
   is only for a session that is meant to persist.
2. Emit a marker so a quiet stretch is visible in the recording rather than
   inferred from a gap, and so the desktop can say *"armed, region quiet"* rather
   than showing a frozen view.
3. Either re-pick when the pinned region goes permanently cold, **or** state the
   refusal. Do not silently re-pick: `SM_AUTO` pins its pick deliberately
   ([35](35-continuous-live-dataflow.md) T1 step 3) so the view does not jump between
   functions mid-session. If re-picking, it must be an explicit, wire-visible event.

**Tests.** `cli_smoke.sh` with a fixture whose region goes quiet then hot: assert
the session survives the quiet stretch and captures the later invocation. Use
`ASMTEST_DF_ENTRY_WAIT_MS` to keep it fast, as the existing sw leg already does.

**Done when.** A continuous capture survives a quiet region; the one-shot default is
unchanged; the checkbox label matches the behaviour, or the label changes.

### T5 — the session-lifecycle repairs  (M)

**Goal.** A capture that ends on its own is announced, the jack is freed, and no
command is refused for a precondition the host itself just removed.

**Steps.**
1. **Announce the self-end.** Emit the terminal `session` event from the tracer tail
   ([asmspy.c:3819-3831](../../../cli/asmspy.c#L3819)) rather than deferring it to
   `serve_reap` on the next command. Today an idle desktop is never told — the
   protocol has no status query and the desktop sends no heartbeat, so **nothing**
   arrives until the user acts.
2. **Free the desktop's jack.** Reconcile `InspectState::active` against
   `LiveStatus::sessions_ended`, immediately before the Queue poll
   ([inspect_door.cpp:409](../../../desktop/src/ui/inspect_door.cpp#L409)) so the
   queue sees the freed jack in the same frame. **Ships with step 1** — with no next
   command, `sessions_ended` never increments and the reconciliation never fires.
3. **Stop refusing the reap's own victim.** Capture `joinable` before
   `serve_reap` ([asmspy.c:4189](../../../cli/asmspy.c#L4189)) and, when the reap
   just fired, ack the `stop` instead of erroring. Same shape for `pause`
   ([:4215](../../../cli/asmspy.c#L4215)).
4. **Clear `last_err` on a new session.** The `started` branch
   ([session.cpp:358-366](../../../desktop/src/live/session.cpp#L358)) already clears
   four stale fields and pointedly not this one, so a red *"refused:"* banner haunts
   the next healthy capture.
5. **Pass the start params on restart.** `inspect_confirm_swap`
   ([inspect_door.cpp:183](../../../desktop/src/ui/inspect_door.cpp#L183)) and the
   Queue poll ([:410](../../../desktop/src/ui/inspect_door.cpp#L410)) call
   `send_start(mode, pid)` with no third argument, silently dropping `steps`,
   `continuous` and the region — so a restarted `auto` comes back ringless and
   one-shot, and a restarted `trace`/`dataflow` is refused outright for want of a
   region. **This is what makes `continuous` un-settable in practice**: the only
   path the stale jack leaves open is the one that discards the flag.

**Tests.** `desktop/test/fixtures/fake_serve.sh` handles `stop` statelessly and so
**cannot** reproduce the refusal — give it a `joinable` notion so the stale-state
path is representable, then assert in `test_inspect`/`test_shell` that after a
self-ended session the jack is free, a restart carries its params, and no refusal
banner survives it. Note `desktop_test_shell` is currently **red** on this checkout (one bar:
`attach/no-host reveals panes`) because `resolve_asmspy_path` finds a real
`./build/asmspy`; `attach/no-host does not start` no longer fails (`e6d827a` scoped
the perturb confirm to the arm64 blocking-syscall case only). Fix or quarantine that
first, or the new bars land on an already-failing suite.

**Done when.** Start + Arm a target that ends one-shot: the pane returns to idle, the
jack frees, no refusal appears, and a second Start works without a Swap.

### T6 — honesty chrome and doc reconciliation  (S, depends on: T2, T3, T4, T5)

**Steps.**
1. Surface the pick honestly in the capture pane: which sampler and rule was used,
   which attempt of how many, and — when it skipped — the substrate reason verbatim.
   The wire already carries all of it via `serve_emit_pick`; the pane discards it.
   A user on an Intel host and one on an AMD box are getting **different
   selection rules**, and the UI says nothing.
2. Fix two stale references in [38](38-live-feed-completion-roadmap.md): its text
   ([38:93](38-live-feed-completion-roadmap.md)) says *"the closable remainder is
   L1–L7"* while the table ends at L6 (adding T6.3's `auto` row as **L7** resolves
   this the other way — do one or the other, not neither), and its AMD bullet
   ([38:64](38-live-feed-completion-roadmap.md)) links `amd-live-validation` to
   `../../../CLAUDE.md`, which has no such section — the ledger is
   [amd-hardware-validation.md](../amd-hardware-validation.md).
3. `CHANGELOG.md`, this README's row, and doc 38's L-table (add the `auto`
   reliability row it currently lacks — the audit recorded the *survey stream* as a
   permanent AMD gate and never noticed that the region *pick* has a portable
   fallback and is therefore closable).

**Done when.** The pane names the sampler, rule and attempt; doc 38's two stale
claims are corrected; no doc still implies `auto` reliability is a hardware gate.

## Task order & parallelism

`T1` → `T2`; `T3`, `T4`, `T5` are independent of each other and of T2 (T3 and T4
touch the engine/picker, T5 the session plumbing); `T6` last. T5 is the one a user
feels first — it can ship alone.

## Constraints & gates

- **No CI lane has AMD silicon**, so T2's *live* leg self-skips everywhere in CI by
  design ([ci.yml](../../../.github/workflows/ci.yml) `hwtrace-privileged`: *"on a
  non-AMD runner the AMD-exact live tests self-skip … it goes RED only on a real
  regression, never for lack of AMD silicon"*). Therefore **every policy change in
  this brief must be proven by a pure test**, per
  [CLAUDE.md](../../../CLAUDE.md): *"A test that can only ever self-skip is not a
  test."* T1/T2/T3's decision logic all land in `test_autoregion`, which links no
  backend and runs on both `cli-smoke` lanes.
- **The hardware gate that IS legitimate** is narrow and must be recorded, not
  worked around: IBS-Op needs AMD Zen 2+ with `ibs_op` and kernel `swfilt`. That is
  a CPU-generation gate, exactly what
  [CLAUDE.md](../../../CLAUDE.md) exempts. It gates *which rule runs*, never
  *whether the walk logic is correct*.
- **aarch64 gets neither sampler.** Both live in the same
  `#if defined(__linux__) && defined(__x86_64__)` block
  ([ibs_backend.c:330](../../../src/ibs_backend.c#L330)), so on arm64 `--auto` is
  unusable and self-skips cleanly. The in-code comment concedes this is incidental
  for the portable one — *"nothing about SW_TASK_CLOCK is arch-specific; lifting
  that is part of the ARM64 rows."* **Lifting it is not this brief** (it belongs with
  [38](38-live-feed-completion-roadmap.md) L1), but no change here may deepen the
  coupling, and the arm64 `cli-smoke` lane must stay green.
- **Docker blocks `perf_event_open` under default seccomp**, so both samplers fail
  with EACCES in a plain `docker run` regardless of silicon;
  `docker-cli-ibs`/`docker-hwtrace-ibs` add `seccomp=unconfined`. A lane that goes
  green without ever opening the sampler is the failure mode
  [mk/cli.mk](../../../mk/cli.mk) already records as measured — do not add another.
- **`--sampler=ibs` on a non-AMD host is not rejected**; it falls to `auto_pick`,
  which self-skips ([asmspy.c:2807-2808](../../../cli/asmspy.c#L2807) tests only
  `SAMPLER_SW`). Forcing the strong sampler on hardware that cannot run it degrades
  to *no capture* rather than to the fallback. Decide deliberately: keep it (an
  explicit force should not silently do something else) but say so in the skip
  reason.

## Non-goals / honest limits

- **Not a new sampler, and not a hardware unlock.** Nothing here makes IBS work off
  AMD or lifts the arm64 gate. It makes the *policy* around whichever sampler exists
  correct and testable.
- **Not per-tid attribution.** [38](38-live-feed-completion-roadmap.md) records that
  as a stated design refusal (statistical absence proves nothing); this brief does
  not reopen it.
- **Not "capture everything."** Every pass stays scoped and bounded; the walk stays
  bounded; the retry stays bounded. Widening the window is a knob, not a default.
- **Not a fix for an idle target.** If a process genuinely executes nothing in any
  window, no policy recovers a region — the honest report is the deliverable. What
  changes is that *"idle for 400 ms once"* stops being spelled *"this target has no
  regions."*
- **Does not make the 3D terrain rich.** `auto`/`dataflow` still emit no
  `trace`/`coverage`, so the terrain keeps the `df_step` residency rung
  ([36](36-anchor-the-3d-plane.md) T3). Real relief needs `trace` mode, which needs
  a named region.

## Cross-references

Cut from [38](38-live-feed-completion-roadmap.md) (live-feed roadmap). The one-shot
default and the re-arm loop are [35](35-continuous-live-dataflow.md); T4 is its
follow-on and inherits its recorded serve-loop caveat (`max` bounds insns-per-pass,
not pass count). The desktop consumers whose emptiness prompted this are
[36](36-anchor-the-3d-plane.md)/[37](37-region-tag-on-df-step.md). The live host and
budget patch-bay are [07](07-serve-live-host.md); the capture pane is
[08](08-observer-views.md). Honesty chrome D7 / [23](23-graded-truth-layer.md);
wording D7 / [24](24-one-visual-language.md). Host ledgers:
[amd-hardware-validation.md](../amd-hardware-validation.md) (which already records
this 9950X — the 2026-07-20 and 2026-07-22 Zen 5 entries), the Zen 2 empirical
record in [analysis/](../analysis/2026-07-12-zen2-ibs-tracing-review.md), and the
Zen 5 privileged-LBR findings in
[analysis/](../analysis/2026-07-12-zen5-privileged-lbr-findings.md).
