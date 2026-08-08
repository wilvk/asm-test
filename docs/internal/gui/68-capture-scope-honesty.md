# The dataflow views say what population they are showing — implementation

> **Source.** The 2026-08-08 session question *"why does the timeline and scrubber
> only show a small set of instructions? should there be more here given the number
> of instructions captured should be high?"*, chased to its measurements.
>
> Read [\_conventions.md](../implementations/_conventions.md) first; D1–D11 live in
> this directory's [README](README.md). **Prerequisites: none.** Model + draw half
> only; no engine, wire or schema change.
>
> Authored 2026-08-08 against `f575aa3c`, every citation verified against the tree
> at that commit. If a cited `file:line` disagrees with the code, the code wins —
> re-verify, then fix this doc in the same change.
>
> **Status — ☐ 0/3.**

## What was measured

Against a synthetic target with one hot function (`work()`, a 200-iteration loop),
`asmspy --dataflow <pid> --auto --steps --blame --sampler=ptrace`:

| capture | `df_step` | `end` footer |
|---|---|---|
| no `max` (a hand-driven Start) | **2013** | `events: 8239, truncated: false, steps_total: 2013` |
| `--max=400` (what a **Sweep** leg sends) | **400** | `events: 1628, truncated: **true**, steps_total: 400` |

So `max` bounds **steps**, not events, and 2013 steps is *one invocation* of one
function. Both numbers are correct and neither is a bug. The problem is that
nothing in the app says which of the two you are looking at, or what the 2013 is
2013 *of*.

Three separate scopes narrow "the process ran billions of instructions" down to the
rows on screen, and the app states **none** of them:

1. **`dataflow` / `auto` single-step ONE REGION**, not the process
   ([budget.h](../../../desktop/src/live/budget.h)'s `mode_needs_region`; the serve
   host resolves a `func` or `base+len`, and `auto` samples for one). The timeline's
   population is that region's steps.
2. **One invocation pass at a time.** A `continuous` capture — the default for
   `auto` ([doors.h:359-366](../../../desktop/src/ui/doors.h#L359)) — re-arms the
   region and appends passes; the dataflow views show one
   ([shell.cpp:2334](../../../desktop/src/ui/shell.cpp#L2334)).
3. **A Sweep leg is hard-capped at 400 steps**
   ([doors.h:419](../../../desktop/src/ui/doors.h#L419) →
   [inspect_door.cpp:556](../../../desktop/src/ui/inspect_door.cpp#L556)).

## The three defects

### T1 — the timeline only states its population when it is *truncated*

`build_banner` ([timeline.cpp:94-121](../../../desktop/src/views/timeline.cpp#L94))
emits text only when `truncated || steps_missing || lost || throttled`. A clean
2013-step capture of one function in a browser therefore renders 2013 rows under
**no banner at all** — visually identical to a complete trace of a small program.

The recording knows every fact needed to say otherwise: the region base is on every
row (`dt_timeline_row::rbase`), the pass count is in `SegmentedDataflow`, and
`steps_total` is in the footer.

**Deliverable.** A `dt_timeline::scope` line, built by the same pure builder as the
banner and asserted in `test_timeline`, that is present on **every** dataflow view —
naming the region, the invocation, and the step count as a scope rather than a
total. Wording must not claim a denominator the recording did not state (the
existing `steps_total`-vs-`insns_total`-vs-unknown ladder in
[feed.cpp:94-103](../../../desktop/src/loom/feed.cpp#L94) is the precedent: it never
invents an M).

### T2 — the Scrubber is hard-pinned to the latest pass while its peers are pinnable

`build_step_index` returns `seg.passes[seg.latest()]` and nothing else
([stepindex.cpp:180-189](../../../desktop/src/analysis/stepindex.cpp#L180)), and
`shell_apply_df_pass` swaps only `s.streams[i].df`
([shell.cpp:2334-2350](../../../desktop/src/ui/shell.cpp#L2334)) — never
`s.stepidx[i]`. `body_timeline`
([shell.cpp:2463](../../../desktop/src/ui/shell.cpp#L2463)), `body_slice` and the
Loom pane all draw the pass pager; `body_scrubber`
([shell.cpp:2544](../../../desktop/src/ui/shell.cpp#L2544)) draws none.

This is not merely an asymmetry. The Scrubber **seeds its playhead from the shared
selection** ([shell.cpp:2563-2573](../../../desktop/src/ui/shell.cpp#L2563)), so
pinning the timeline to pass 2 and clicking
step 12 puts the Scrubber on step 12 **of pass 7** — a different instruction, shown
as though it were the one just clicked, with no chrome anywhere saying so. Two views
disagreeing silently about what "step 12" means is the cardinal sin this codebase
names in [D4](README.md).

**Deliverable.** Cache the `SegmentedStepIndex` beside `seg_df`, have
`shell_apply_df_pass` resolve **both**, and draw the pager in `body_scrubber` on the
same gate as its peers. `test_shell` asserts that a pinned pass moves the Scrubber's
index, not only the timeline's.

### T3 — a Sweep never says its legs are capped

`sweep_plan` ([budget.cpp:171-183](../../../desktop/src/live/budget.cpp#L171)) says
`"auto -> tree -> trace, 400 events each"`. The measurement above shows `max` bounds
**steps** for the dataflow legs, and that the resulting recording is flagged
`truncated: true` — the operator is told "events", gets steps, and the one leg that
fills the Timeline/Loom/Scrubber is the one the cap bites hardest.

**Deliverable.** `sweep_plan` names the unit each leg's cap is in, and says that the
`auto` leg's recording will come back truncated. Pure, so `test_budget` pins it.

## Non-goals

- **Raising or removing `sweep_max`.** 400 is a deliberate bound (an unbounded first
  leg means the sweep never reaches its second — [doors.h:413-415](../../../desktop/src/ui/doors.h#L413)).
  This work makes the bound legible, not larger.
- **Widening the capture scope.** Single-stepping one region is what `dataflow` *is*.
- **The empty-Loom case.** Already closed at `f575aa3c`: a capture the host refused
  now leads with its measured `end.skip` reason
  ([feed.cpp:107-124](../../../desktop/src/loom/feed.cpp#L107)) instead of the
  generic "the only L0 value producer is the x86-64 emulator producer", which sent a
  reader whose `ptrace_scope` refused the attach off to look for an emulator.

## Host note (not a code defect)

On the box this was measured on, **every** ptrace mode is refused against a browser:

```
$ asmspy --info <firefox-pid>
  attach   NO — yama ptrace_scope=1 refused this target (Permission denied)
  modes    log NO  stream NO  trace NO  dataflow NO  tree NO  graph NO  procs NO  sample NO  watch NO
```

`/etc/sysctl.d/10-ptrace.conf` sets `kernel.yama.ptrace_scope = 1` and the
`90-asmtest-tracing.conf` override that used to beat it is **absent**;
`kernel.perf_event_paranoid` is `4`. A capture of such a target skips and records
nothing, which is what T1/T2's chrome must not be mistaken for.
