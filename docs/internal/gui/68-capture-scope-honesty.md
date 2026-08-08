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
> **Status — ☑ 3/3** (implemented 2026-08-08; `make desktop-test` green, 100 binaries).

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

**Deliverable — done.** `dt_dataflow_scope(const Streams &)`
([timeline.cpp](../../../desktop/src/views/timeline.cpp)) builds the line; it is
pure, so `test_timeline` pins every branch with no frame. It names the recorded
step count, the region base the offsets are relative to (or says the wire stated
none — never `0x0`), and closes with *"never the whole process"*. It invents no
denominator: `of M` appears only when the footer stated a total **larger** than
what arrived, matching the `steps_total` ladder in
[feed.cpp:94-103](../../../desktop/src/loom/feed.cpp#L94).

Drawn once, in `shell_df_pass_pager` — already the one strip all four dataflow
views put above themselves — and **before** that function's `npasses <= 1` early
return, since a one-shot recording draws no pager and is exactly the shape whose
row count reads as the whole story. No `dt_timeline` field: one builder, one draw
site, no state that could go stale.

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

**Deliverable — done.** `ShellState::seg_stepidx` caches the ring per pass beside
`seg_df` (same `df_invocation` markers, so the ordinal means the same thing in
both), `shell_apply_df_pass` resolves both, and `body_scrubber` draws the pager.

Two guards worth keeping: the swap is **bounds-checked** rather than assuming the
two marker sets agree, and it is gated on the pass actually **changing** — the
Scrubber's synthesise action (30 R3 T4) replaces `stepidx[i]` in place, and an
unconditional per-frame copy would discard it. `test_shell` pins all of it against
`continuous-df.asmtrace`, whose three passes hold `rax = 6 / 10 / 100` at step 0.

### T3 — a Sweep never says its legs are capped

`sweep_plan` ([budget.cpp:171-183](../../../desktop/src/live/budget.cpp#L171)) says
`"auto -> tree -> trace, 400 events each"`. The measurement above shows `max` bounds
**steps** for the dataflow legs, and that the resulting recording is flagged
`truncated: true` — the operator is told "events", gets steps, and the one leg that
fills the Timeline/Loom/Scrubber is the one the cap bites hardest.

**Deliverable — done.** The line now reads *"… 400 steps each (the single-step
legs) — a leg that reaches its cap stops there and its recording is flagged
TRUNCATED"*, for both sweep shapes. Pure, pinned in `test_budget`.

## Non-goals

- **Raising or removing `sweep_max`.** 400 is a deliberate bound (an unbounded first
  leg means the sweep never reaches its second — [doors.h:413-415](../../../desktop/src/ui/doors.h#L413)).
  This work makes the bound legible, not larger.
- **Widening the capture scope.** Single-stepping one region is what `dataflow` *is*.
- **The empty-Loom case.** Already closed at `f575aa3c`: a capture that skipped now
  leads with its measured `end.skip` reason
  ([feed.cpp:107-124](../../../desktop/src/loom/feed.cpp#L107)) instead of the
  generic "the only L0 value producer is the x86-64 emulator producer", which sent
  the reader off to look for an emulator. See the measurement below for what that
  actually says on a browser.

## What an `auto` capture of a browser actually does (measured)

Worth recording, because it is a **fourth** way to get few or no rows and it is not
one of the three scopes above. Driving the serve protocol with exactly the params
the capture pane sends (`mode: auto`, `steps`, `insns`, `blame`, `ms: 2000`,
`continuous`, `max: 400`) against a Firefox `Web Content` process, on a host where
`ptrace_scope` is `0` and the target is fully attachable:

```json
{"k":"session","state":"pick","pick":{"sampler":"ptrace-pc","evidence":"idle","func":"(idle window)","attempt":1,"of":3}}
{"k":"session","state":"pick","pick":{"sampler":"ptrace-pc","evidence":"idle","func":"(idle window)","attempt":2,"of":3}}
{"k":"session","state":"skip","skip":{"code":1,"reason":"3 idle sample windows (2000 ms each) qualified no region — the target may be idle, or its work may be in a module the filter excludes"}}
```

The whole recording is three events: a `note`, an `end` with `skip`, and nothing
else. `auto` picks its region by watching the target **enter** a function; a browser
on a rendered page enters almost nothing, so no candidate qualifies, no single-step
ever runs, and every dataflow view is correctly empty.

Two things follow. First, the perf-free chain works as
[budget.cpp:285-315](../../../desktop/src/live/budget.cpp#L285) claims — the sampler
that ran is `ptrace-pc`, at `perf_event_paranoid = 4`, with the GUI sending no
`sampler` key. Second, the `f575aa3c` Loom placard is what makes this legible:
verified end to end on that exact recording, the pane renders

> this capture SKIPPED and recorded nothing — 3 idle sample windows (2000 ms each)
> qualified no region — the target may be idle, or its work may be in a module the
> filter excludes. Nothing below is a fact about the target. (…)

An **idle** target and a **refused** one are different problems with different
remedies, and the placard now names which one happened. Run `asmspy --info <pid>`
to tell them apart before spending a capture: it never attaches, and it prints
whether each of the nine modes will work.
