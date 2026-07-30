# Segment the dataflow decode by `df_invocation` — the last consumer that still conflates passes — implementation

> **A per-gap brief cut from [38](38-live-feed-completion-roadmap.md)** (gap **L2**,
> "the cleanest self-contained desktop win — host-testable, no engine change; do it
> first"). A continuous `dataflow`/`auto` capture ([35](35-continuous-live-dataflow.md)
> T1) appends many invocation passes into one growing recording, each delimited by a
> `df_invocation` marker and **each restarting its `df_step` at step 0**. The
> Scrubber already segments them (`build_segmented_step_index`,
> [analysis/stepindex.cpp](../../../desktop/src/analysis/stepindex.cpp)). Every other
> dataflow view reads `Streams::df` from `decode_streams`
> ([doc/streams.cpp](../../../desktop/src/doc/streams.cpp)), which **indexes by
> `step` across every pass at once** — so passes alias: `insn_off[step]` /
> `insn_rbase[step]` / `disasm[step]` are last-write-wins across passes, while
> `recs` and `edges` **accumulate all passes** into one `step`-space, mixing operands
> and def-use endpoints from different invocations that merely share a step number.
> Slice, Timeline, the Loom fabric, the 3D `df_step` trajectory, and the terrain
> churn walk all render from that conflation on a continuous capture.
>
> `streams.h` already names the bug and its owner:
> [streams.h:68-75](../../../desktop/src/doc/streams.h#L68) — *"KNOWN ALIASING
> (pre-existing, not fixed here): the decoder indexes by `step`, but a continuous
> capture restarts `step` at 0 per pass (35 T1), so passes alias … The fix belongs
> with 35's segmentation, not here."* **This brief is that fix.**
>
> **No engine, no wire, no schema change.** The producer already emits the
> `df_invocation` delimiters (35 T1); the desktop side is a pure decode. The
> `df_invocation` body carries `pass`, `result`, `steps`, `truncated`
> ([asmtrace-schema.md](asmtrace-schema.md)), exactly what the segmenter needs, and
> `Event::seq` gives the stream position the bucketing keys on — the same mechanism
> `build_segmented_step_index` uses. Host-testable on both `docker-desktop` lanes.
>
> Authored 2026-07-29 against HEAD `a7c9161`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Status (2026-07-29) — ✅ 3/3.** T1 the segmented decode + latest resolution
> (`4b15f87`); T2 the per-pass invocation pager on Slice/Timeline/Loom
> (`shell_apply_df_pass` + `shell_df_pass_pager`, following the latest by default,
> pinnable, one-shot recordings unchanged); T3 this reconcile. Both `docker-desktop`
> lanes green.

## Why this work exists

`decode_streams` was written before continuous capture existed. Its `df_step`
loop ([streams.cpp:171-236](../../../desktop/src/doc/streams.cpp#L171)) sizes
`insn_off` / `insn_rbase` / `disasm` to `nsteps = max(step)+1` over **all** events
and writes each step's field by `step` index; a second pass's step 0 overwrites the
first's. The operand and edge streams are worse than overwritten — they are
**merged**: `recs` gets every pass's ops appended, `edges` gets every pass's def-use
endpoints, all sharing one 0-based `step` axis. A Slice over a two-pass capture then
draws edges that never existed (pass 1's `from` to pass 0's `to` at the same step
number).

The Scrubber does not have this problem because doc 35 T3 gave it
`build_segmented_step_index` — one `StepIndex` per pass, bucketed by the
`df_invocation` markers' `seq`, with `build_step_index` resolving to the **latest**
pass as the live default. This brief gives the dataflow stream the identical
treatment, so the two live-default consumers agree on which pass they show.

**The mirror is exact and deliberate.** `SegmentedDataflow` : `DataflowStream` ::
`SegmentedStepIndex` : `StepIndex`. `build_segmented_dataflow` buckets by the same
`df_invocation` `seq` windows `build_segmented_step_index` uses. `decode_streams`
resolves to `passes[latest()]` the way `build_step_index` does. A one-shot recording
(no marker) is one pass over the whole list — byte-identical to today's flat decode,
which is what keeps every existing golden and view test green.

## Tasks

### T1 — segment the dataflow decode; resolve `Streams::df` to the latest pass

The core correctness fix. Refactor the inline `df_step`/`df_edge` decode
([streams.cpp:170-253](../../../desktop/src/doc/streams.cpp#L170)) into a pure core
`DataflowStream decode_dataflow(const std::vector<const Event *> &steps, const
std::vector<const Event *> &edges)` — the existing logic verbatim (sort by step,
size arrays to `max(step)+1`, fill offsets/rbase/disasm/ops, count `steps_missing`,
decode edges + `edge_loc`), operating on a supplied slice of events rather than the
whole recording.

Add to [streams.h](../../../desktop/src/doc/streams.h), immediately mirroring
`SegmentedStepIndex`:

```cpp
// The dataflow stream segmented by df_invocation (35 T1): one DataflowStream per
// continuous-capture pass, oldest first. A one-shot recording (no marker) is
// exactly ONE pass over the whole df_step/df_edge list — byte-identical to the
// pre-40 flat decode. Each pass restarts step at 0, so passes never alias (the
// KNOWN-ALIASING hazard streams.h flagged is closed here). Mirrors
// analysis/stepindex.h's SegmentedStepIndex.
struct SegmentedDataflow {
    std::vector<DataflowStream> passes; // one per pass, oldest first
    size_t latest() const { return passes.empty() ? 0 : passes.size() - 1; }
    bool present() const;         // any pass carries df_step
    size_t present_passes() const; // how many do
};

SegmentedDataflow build_segmented_dataflow(const Recording &r);
```

`build_segmented_dataflow` gathers the `df_step` and `df_edge` event pointers in
stream order, and the `df_invocation` markers. **No marker → one pass** over the
whole `df_step`+`df_edge` list (the pre-40 path). **Markers present →** sort markers
by `seq`; each pass owns the `df_step`/`df_edge` events whose `seq` falls in
`[marks[p].seq, marks[p+1].seq)` (last pass open-ended), exactly as
[stepindex.cpp:156-176](../../../desktop/src/analysis/stepindex.cpp#L156) buckets
regstate. Call `decode_dataflow` per bucket.

`decode_streams` replaces its inline df block with:

```cpp
SegmentedDataflow seg = build_segmented_dataflow(r);
if (!seg.passes.empty())
    s.df = std::move(seg.passes[seg.latest()]);
```

Retire the KNOWN-ALIASING comment at
[streams.h:68-75](../../../desktop/src/doc/streams.h#L68) — replace it with the
one-line statement that `insn_rbase` is per step within **one** pass and passes are
now separated by `build_segmented_dataflow`, so the wrong-base aliasing it warned of
cannot occur.

**Definition of done.** A new `desktop_test_streams` binary (link
`test_streams.o + $(DESKTOP_TEST_DOC)`; add to `DESKTOP_TESTS`) asserts, loading
`low-fidelity/continuous-df.asmtrace`:
- `build_segmented_dataflow` returns **3** passes; `present_passes() == 3`.
- pass 0 and pass 1 each have `nsteps == 3` with `insn_off == {0,3,6}`; pass 2
  (`truncated`) has `nsteps == 2` (only two `df_step` events survived) with the last
  op's `value == 300`.
- each pass's step-0 op `value` is its own (`6`, `10`, `100`) — **not** conflated.
- `decode_streams(r).df` equals `passes[latest()]` — its step-0 op `value == 100`
  (pass 2), `nsteps == 2`; it is **not** `max(step)+1 == 3` over all passes and its
  `recs` hold only pass 2's operands.
- a one-shot df fixture (e.g. `mem-df-chain.asmtrace`, no marker) yields **one**
  pass, and `decode_streams(r).df` is unchanged from the pre-40 decode (assert
  `nsteps`, `insn_off`, `recs.size()`, `edges.size()` against the known flat values).

All existing view/golden tests stay green (one-shot fixtures carry no marker →
byte-identical). Both `docker-desktop` lanes green.

### T2 — surface the passes: a per-pass selector on the dataflow views, latest by default

Make "per-pass Slice/Loom/Timeline" (the L2 headline) genuinely reachable. Cache the
`SegmentedDataflow` per recording alongside `Streams` in the shell
([shell.cpp:231](../../../desktop/src/ui/shell.cpp#L231)) and add a per-recording
**selected-pass** index defaulting to `latest()`. When a recording has **> 1** df
pass, the dataflow views' shared chrome shows an invocation selector — reuse the
intentional-discrete stepper the Observer region view already uses
(`dt_timepos_step("invocation", …)`,
[observer_draw.cpp:629](../../../desktop/src/views/observer_draw.cpp#L629)) — labelled
**"pass N of M · latest"** when N is the latest and **"pass N of M"** otherwise, so a
continuous capture never silently presents one pass as the whole run. Selecting a
pass swaps the cached `Streams::df` to `seg.passes[sel]`; the views re-render from it
unchanged (they already read `Streams::df`). A one-shot recording (one pass) shows
**no** selector — byte-identical chrome to today.

**Definition of done.** `test_shell` (or a focused draw test) loads
`continuous-df.asmtrace`, asserts the selector reports **3** passes and defaults to
pass 2, steps it to pass 0, and asserts the active `Streams::df` step-0 op is now
pass 0's (`value == 6`). A one-shot recording asserts no selector is offered. Both
lanes green.

### T3 — reconcile the roadmap and changelog

Flip [38](38-live-feed-completion-roadmap.md)'s L2 row to **CLOSED (doc 40)** with a
one-line reality note (mirror the Scrubber; latest is the live default; segmented
model exposed), add the doc-40 row to [README.md](README.md)'s follow-on list, and
add a CHANGELOG line. Update this brief's status to ✅ 3/3.

## What this brief is NOT

- **Not** a per-pass *diff* or cross-pass convergence — `df_step` carries no `tid`,
  so a live dataflow capture is one trajectory per pass by scope
  ([38](38-live-feed-completion-roadmap.md)'s corrected not-a-gap). Passes are
  shown one at a time, never fused into a faked global clock.
- **Not** a producer/wire/schema change — the `df_invocation` delimiters already
  ship (35 T1); this is pure decode + a selector.
- **Not** the Scrubber — it already segments (35 T3); this brings the dataflow
  stream to parity so the two live-default consumers agree.

## Claim

| Task | Who · when | Status |
|---|---|---|
| T1 the segmented decode + latest resolution | will · 2026-07-29 | ✅ `4b15f87` |
| T2 the per-pass selector | will · 2026-07-29 | ✅ landed |
| T3 roadmap/changelog reconcile | will · 2026-07-29 | ✅ landed |
