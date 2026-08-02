# The memory data-cell family — relief, tide, lifetime, ribbon, sediment

> **Sources.** The [3D catalog](../analysis/2026-07-29-3d-visualization-catalog.md)
> §5 layers 7 (read/write twin relief), 8 (working-set tide), 9 (observed-lifetime
> pillars), 12 (data-access worldline ribbon) and 13 (residency sediment columns),
> and its §7 Phase 2; cut by
> [53-3d-catalog-build-roadmap.md](53-3d-catalog-build-roadmap.md) §4.1 (L7, L8,
> L9, L12, L13). Read [_conventions.md](../implementations/_conventions.md) first;
> D1–D11 live in this directory's [README](README.md).
>
> **Prerequisites.** T2–T5 all need [54](54-3d-catalog-phase0-plumbing.md) **T1**
> (the observed-data-span projection); T2 and T3 additionally need **T2** (the
> read/write prefix-sum split). T6 has no prerequisite and can be built first.
> Layer registration assumes [56](56-fidelity-and-module-layers.md) T1.
> [55](55-scene-render-quality.md) should land before this brief — see *Effort and
> risk*.
>
> Authored 2026-08-02 against HEAD `b657876`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Status — ☐ 0/6, not started.**

## Why this work exists

The terrain has a data half. It has had one since 10-T2: `DataCell` with ordered
`steps`, a `cum_size` prefix sum and a `cum_rw` prefix OR
([terrain.h:121-126](../../../desktop/src/space/terrain.h#L121)); `TF_READ` and
`TF_WRITE` flags ([terrain.h:53-54](../../../desktop/src/space/terrain.h#L53)); a
whole rich-rung builder gated on the `mem` stream
([terrain.cpp:387-420](../../../desktop/src/space/terrain.cpp#L387)); and tests
covering all of it.

**None of it can fire in the shipped app.** The shell builds the plane from
`regions_from_codeimage(r)` alone ([shell.cpp:921](../../../desktop/src/ui/shell.cpp#L921)),
so no region maps a heap or stack address, so `cell_of` fails for every `mem`
access and the scan `continue`s past it
([terrain.cpp:397-399](../../../desktop/src/space/terrain.cpp#L397)). The data half
of the terrain is dead code reachable only from tests.

[54](54-3d-catalog-phase0-plumbing.md) T1 fixes that. This brief is what the fix is
*for*: five graphs that answer questions no existing view can, all riding one
projection extension and one prefix-sum split, which is why the catalog groups them
and why they are worth landing together.

They answer: *is this address read-mostly or a write accumulator?* (T2) · *what is
being touched right now versus drifting cold?* (T3) · *over what interval is each
address alive?* (T4) · *is the access pattern streaming, strided, or
pointer-chasing?* (T5) · *is each cell touched early, late, or throughout?* (T6)

## What already exists (verified 2026-08-02 against `b657876`)

- **`DataCell`** = `{cell, steps (ascending `mem` steps), cum_size (parallel prefix
  sum of size), cum_rw (parallel prefix OR of READ/WRITE bits)}`
  ([terrain.h:121-126](../../../desktop/src/space/terrain.h#L121)). `steps` is
  ascending and precomputed *for `slice()`*, so `front()`/`back()` are free —
  which is T4's entire data requirement.
- **`slice(t)` is a binary search per cell, not a rescan**
  ([terrain.h:130-133](../../../desktop/src/space/terrain.h#L130)) — so a windowed
  delta (T3) is two binary searches, and a per-band count (T6) is one per band.
- **`mem_present` / `mem_note`** already carry the coarse-rung provenance
  ([terrain.h:101-102](../../../desktop/src/space/terrain.h#L101)) — *"coarse: no
  per-access memory stream"* when absent. Every layer here reuses that chip rather
  than inventing an empty-state message.
- **`coarse_slice()`** is the existing degrade-under-budget path
  ([terrain.h:137-144](../../../desktop/src/space/terrain.h#L137)), driven by
  `should_degrade`/`kScrubCellBudget` in the shell
  ([shell.cpp:1001-1012](../../../desktop/src/ui/shell.cpp#L1001)). Any layer that
  makes scrubbing expensive must plug into it, not around it.
- **`trajectory.cpp`'s ribbon machinery** is what T5 substitutes into: the same
  per-vertex, per-tid line construction, with the effective address in place of the
  PC.
- **The access-mark spurs already exist and are a different thing** —
  `access_spurs_` ([scene.h:187](../../../desktop/src/scene3d/scene.h#L187)) draws
  PC→data-cell spurs. T5 is the *order* of data accesses, not the PC's association
  with them; the HUD must not let the two read as one layer.

## Tasks

### T1 — The data-cell HUD contract: say which rung is feeding this (S)

**Goal.** Before any of the four data layers exist, the pane can state whether it
has per-access memory data, observed data spans, both, or neither — so an empty
layer is never mistaken for an empty program.

**Steps.**
1. Extend `placement_chips` ([hud.h:29-34](../../../desktop/src/scene3d/hud.h#L29))
   with the data-rung facts: whether `mem_present`, how many observed-data spans
   the projection carries and at what threshold (the `data_span_note`
   [54](54-3d-catalog-phase0-plumbing.md) T1 adds), and how many `mem` accesses
   failed to place.
2. **Count the drops.** `terrain.cpp:397-399` currently `continue`s silently; make
   it increment a counter on the model. After [54](54-3d-catalog-phase0-plumbing.md)
   T1 this should be near zero, and if it is not, the span clustering is wrong and
   this is how anyone finds out.
3. Chip wording is graded, not new: reuse `mem_note`'s existing phrasing for the
   absent case (D7 / [24](../archive/gui/24-one-visual-language.md)).

**Tests.** `test_terrain.cpp`: the drop counter is zero for a fixture whose spans
cover every access, and exactly N for one where N addresses fall outside. The HUD
test TU: a `mem`-less recording shows the coarse chip; a `mem`-carrying recording
with spans shows the span count and threshold.

**Done when.** The pane always states which data rung it is on, and a dropped
access is never silent.

### T2 — Read/write twin relief (M) · *needs [54](54-3d-catalog-phase0-plumbing.md) T1 + T2*

**Goal.** Read-mostly constants, write accumulators and in-place RMW cells become
three visibly different shapes — an asymmetry the OR-merged terrain structurally
hides.

**Steps.**
1. Two mirrored bas-relief surfaces over the shared data cells: `+Y =
   log1p(cum_read_size ≤ t)` in a cool hue, `−Y = log1p(cum_write_size ≤ t)` in a
   warm one, both sliced by the terrain-time playhead. The two prefix sums are
   [54](54-3d-catalog-phase0-plumbing.md) T2's.
2. The three shapes fall out and should be named in the legend: a read-only const
   buffer is a cool peak with no pit; an accumulator is a warm pit with no peak; an
   in-place RMW cell is a balanced peak and pit pinched at the plane.
3. **A direction that was never captured is an ABSENT surface, not a zero-height
   one.** This is the layer's central rule: a cell with reads and no recorded
   writes must not render a flat write surface, because flat reads as *measured
   zero writes*. Absence is a hole.
4. **No RMW is inferred where only one direction was recorded.** The label says
   *observed* reads and *observed* writes.
5. Drill-in: peak or pit → slice/timeline at the cell's last access; a write pit
   may additionally offer a `views/watch` arm at that address.

**Fidelity.** Reads-up and writes-down are never merged (that merge is exactly what
`cum_rw`'s OR does, and why T2 of Phase 0 exists). A torn capture floors **both**
surfaces. Absent `mem` → flat plus the existing note, never a silent zero.

**Tests.** `test_terrain.cpp` + a builder test: a read-only fixture produces a peak
and **no** write surface (assert the surface is absent, not zero); a write-only
fixture the mirror; a mixed fixture both; the unknown-direction access from
[54](54-3d-catalog-phase0-plumbing.md) T2 contributes to neither surface;
`TF_TORN` floors both.

**Done when.** The three access shapes are distinguishable and an uncaptured
direction is a hole.

### T3 — Working-set tide (M) · *needs [54](54-3d-catalog-phase0-plumbing.md) T1 + T2*

**Goal.** What the program is touching *now* versus what has drifted cold — recency
the cumulative, monotonic terrain cannot show by construction.

**Steps.**
1. A second, windowed height surface over data cells only:
   `log1p(cum_size[upper_bound(t)] − cum_size[upper_bound(t − W)])` per `DataCell`
   — two binary searches over the existing prefix arrays, no rescan.
2. `W` is a HUD dwell knob **on the terrain-time axis**. The execution-step axis is
   untouched ([34](../archive/gui/34-playhead-and-scene-reach.md)'s two-axes rule).
   Name the unit on the control; a bare number is the review's top finding.
3. **Cold cells are a faded watermark at their last crest height**, at roughly 10%,
   excluded from the live mass — a faithful decay. A cell that has gone cold has not
   gone to zero, and drawing it at zero would say it was never touched.
4. Crest tint adopts T2's read/write split. If T2 is not taken, degrade to a binary
   "the window contains a write" flag from `cum_rw` **and label it as that** — an OR
   flag presented as a read/write ratio is a fabricated quantity.
5. Drill-in: crest cell → slice at the last hitting step, plus timeline.

**Fidelity.** The receding watermark is a decay, never a zero. Torn floors the
window and `TF_TORN` survives the drill-in. Survey residency (which is PC-space, not
data-space) is never fed here. Absent `mem` → flat plus `mem_note`.

**Tests.** A builder test: a cell touched only outside `[t−W, t]` has zero live
height and a non-zero watermark; a cell never touched at all has neither; the
windowed delta equals a brute-force sum over the same range for several `(t, W)`;
the degraded binary path is labelled differently from the split path.

**Done when.** Recency and drift are visible, and cold and never-touched look
different.

### T4 — Observed-lifetime pillars (M) · *needs [54](54-3d-catalog-phase0-plumbing.md) T1*

**Goal.** A Gantt-in-3D: over what interval each address is observed alive.

**Steps.**
1. Per touched `DataCell`, a translucent vertical pillar spanning
   `[steps.front(), steps.back()]` — both already precomputed and ascending
   ([terrain.h:123](../../../desktop/src/space/terrain.h#L123)), so this layer costs
   one pass and no rescan. `Y` is trace time.
2. Short stub = transient scratch; full-height pillar = persistent state; colour by
   read/write dominance (T2's split). A horizontal band of pillar starts or ends is
   a phase boundary and is the layer's real finding.
3. The terrain-time playhead reads as a horizontal plane through the pillars:
   below = born, intersected = live, above = untouched.
4. Drill-in: pillar → slice at its first or last step.

**Fidelity — the load-bearing one in this brief.** The pillar is an **observed-touch
lifetime, not an allocation lifetime**, and must be labelled in those words. No
producer emits allocation events (`recording.cpp`'s kind list is
trace/coverage/…/mem/blame/statediff — no malloc/free/mmap/brk), so the real object
may have existed long before the first touch and long after the last. A pillar whose
last touch sits at a torn tail is **open-topped** (floored, not capped). Uncovered
steps inside the interval are gaps, never zero-length pillars.

**Tests.** A builder test: a cell touched at steps 5 and 900 yields one pillar
spanning 5–900, not two; a cell touched once yields a stub with a stated
zero-length interval; a torn recording open-tops the pillars whose last touch is at
the tail; the label text contains "observed" and does not contain "allocat".

**Done when.** Address lifetimes are readable and no geometry or wording claims
allocation.

### T5 — Data-access worldline ribbon (M) · *needs [54](54-3d-catalog-phase0-plumbing.md) T1*

**Goal.** The *shape* of the access order — streaming, strided, or pointer-chasing —
which a flat address list cannot show and a locality-preserving plane can.

**Steps.**
1. One vertex per `mem` access at its effective address's cell; consecutive accesses
   joined in step order; `Y` = trace time; width = access size; colour = read/write.
   Reuse `trajectory.cpp`'s ribbon construction with `ea` substituted for the PC —
   this is a substitution, not a second ribbon implementation.
2. Fallback source when `mem` is absent but a dataflow capture is present:
   `DataflowStream::recs` `ValRec` with `space` in `{"abs","off"}` carrying
   `addr`/`size`/`write` ([streams.h:31-45](../../../desktop/src/doc/streams.h#L31)).
   **Label which source fed it** — the two are not the same population (the emulator's
   hardware hooks see implicit stack traffic a live `mem` enumeration does not).
3. The shapes are the point: a sequential scan hugs adjacent cells; a stride
   zig-zags; pointer-chasing leaps. **A leap across distinct observed-data spans is
   labelled genuine non-locality**, so a reader does not attribute it to the
   compaction.
4. **Explicitly not the existing PC→data access-mark spurs.** Give it its own legend
   entry saying so ([scene.h:187](../../../desktop/src/scene3d/scene.h#L187)).
5. Drill-in: ribbon segment → slice/timeline over that step range.

**Fidelity.** Exact only — never weave survey data into an ordered ribbon. An
uncovered step is a **gap**, never an interpolated segment; interpolation here would
draw an access that was never recorded. Torn caps the ribbon and the cap survives
the drill-in. Observed-data spans stay `Kind::Unknown`.

**Tests.** A builder test: a sequential fixture produces monotonically adjacent
cells; a strided one produces the documented alternation; a gap in the step coverage
produces a break rather than a joined segment; the fallback source is labelled
differently from `mem`.

**Done when.** Access locality is legible as a shape, and no segment exists where no
access was recorded.

### T6 — Residency sediment columns (M)

**Goal.** Whether each cell is touched early, late, or throughout — the phase the
moving terrain slice only reveals by scrubbing, stood up with the playhead paused.

**Steps.**
1. For each touched cell, subdivide a slender column along the terrain-time axis into
   bands `[τ, τ+δ]`; band opacity/height = the hit count in that window, by binary
   search over `CodeCell::steps` (or `DataCell::steps` for data cells). No rescan —
   the ascending step vectors are already there
   ([terrain.h:114-115](../../../desktop/src/space/terrain.h#L114),
   [:123](../../../desktop/src/space/terrain.h#L123)).
2. Exact columns are solid; `TF_STAT` columns are stippled **and live in the separate
   stat object**, never in the exact geometry buffer.
3. **A never-hit cell places no column.** Absence, not a zero nub — a nub reads as a
   measurement.
4. This rides the terrain trace-time axis only, never the exec-step playhead.
5. Drill-in: band → trace canvas (code) or slice explorer (data) at that offset and
   the band's step range; column → the cell's canvas.
6. **Band count is a budget.** N cells × B bands is the densest geometry in this
   family; wire it to the existing degrade path (`should_degrade`/`coarse_slice`,
   [shell.cpp:1001-1012](../../../desktop/src/ui/shell.cpp#L1001)) rather than a new
   throttle, and state the band count in the HUD.

**Fidelity.** `TF_STAT` never merged into exact. `TF_TORN` caps the column and floors
everything above it as a lower bound. A never-hit cell places nothing.

**Tests.** A builder test: a cell hit only in the first decile produces bands only
there; a uniformly hit cell produces even bands; the band counts sum to the cell's
total hit count (the conservation assertion); a never-hit cell produces no column; a
`TF_STAT` cell's bands land in the stat buffer.

**Done when.** Temporal phase is readable with the playhead stationary, and the band
counts conserve.

## Fidelity notes (D7)

- **The single most important sentence in this brief is T4's**: these are
  observed-touch intervals, not allocation lifetimes, because nothing in the capture
  layer emits allocation. Every layer here inherits it — a "working set" (T3) is a
  set of *touched* addresses, and a "span" ([54](54-3d-catalog-phase0-plumbing.md)
  T1) is a *touched extent*.
- **Absent is not zero, three times over**: T2's uncaptured direction is a missing
  surface; T3's cold cell is a watermark; T6's never-hit cell places no column. Each
  would be a plausible-looking zero if implemented carelessly, and a zero here reads
  as "the program did not do that".
- **The read/write split must never be back-derived from `cum_rw`.** It is an OR of
  bits, not a ratio; T3 step 4 states the only honest degraded form.
- T5 is exact-only. An ordered ribbon woven from sampled data would assert an access
  sequence that was never observed.

## Effort and risk

Six tasks: one small (T1), five medium. Risks:

- **This is the brief where translucency stops being decorative.** T2's two mirrored
  surfaces, T3's watermark, T4's translucent pillars and T6's banded columns all
  overlap in the same footprint. Land [55](55-scene-render-quality.md) — at minimum
  T1 (EDL) and T4 step 1 (the depth-write fix) — before or alongside, or the composed
  result will be a haze and the individual layers will get blamed for it.
- **T6 is the frame-budget risk** and the reason it is listed last despite having no
  prerequisite: it multiplies cell count by band count. Build it against the existing
  degrade path from the start.
- **All of T2–T5 are blocked on one task in another brief.** If
  [54](54-3d-catalog-phase0-plumbing.md) T1 is claimed by another agent, take T6 and
  T1 here first; they are genuinely independent.
