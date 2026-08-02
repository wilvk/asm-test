# The memory data-cell family — relief, tide, lifetime, ribbon, sediment

> **Sources.** The [3D catalog](../analysis/2026-07-29-3d-visualization-catalog.md)
> §5 layers 7 (read/write twin relief), 8 (working-set tide), 9 (observed-lifetime
> pillars), 12 (data-access worldline ribbon) and 13 (residency sediment columns),
> and its §7 Phase 2; cut by
> [53-3d-catalog-build-roadmap.md](53-3d-catalog-build-roadmap.md) §4.1 (L7, L8,
> L9, L12, L13). Read [_conventions.md](../implementations/_conventions.md) first;
> D1–D11 live in this directory's [README](README.md).
>
> **Prerequisites.** T2–T5 all need [54](../archive/gui/54-3d-catalog-phase0-plumbing.md) **T1**
> (the observed-data-span projection); T2 and T3 additionally need **T2** (the
> read/write prefix-sum split). T6 has no prerequisite and can be built first.
> Layer registration assumes [56](../archive/gui/56-fidelity-and-module-layers.md) T1.
> [55](55-scene-render-quality.md) should land before this brief — see *Effort and
> risk*.
>
> Authored 2026-08-02 against HEAD `b657876`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change. Line numbers below were re-verified 2026-08-03 while implementing.
>
> **Status — ✅ 6/6, landed 2026-08-03.**

## Why this work exists

The terrain has a data half. It has had one since 10-T2: `DataCell` with ordered
`steps`, a `cum_size` prefix sum and a `cum_rw` prefix OR
([terrain.h:142-160](../../../desktop/src/space/terrain.h#L142)); `TF_READ` and
`TF_WRITE` flags ([terrain.h:53-54](../../../desktop/src/space/terrain.h#L53)); a
whole rich-rung builder gated on the `mem` stream
([terrain.cpp:388-440](../../../desktop/src/space/terrain.cpp#L388)); and tests
covering all of it.

**Until 54 T1 landed, none of it could fire in the shipped app.** The shell built
the plane from `regions_from_codeimage(r)` alone, so no region mapped a heap or
stack address, so `cell_of` failed for every `mem` access and the scan `continue`d
past it. The data half of the terrain was dead code reachable only from tests.

[54](../archive/gui/54-3d-catalog-phase0-plumbing.md) T1 fixed that, and the fix is
live in the shipping path: `shell.cpp` now appends `observed_data_spans(r, regs,
&span_note)` to the code regions before `build_projection`
([shell.cpp:936-948](../../../desktop/src/ui/shell.cpp#L936)). **Verified end to
end while implementing this brief** (`test_datalayers.cpp`'s `weave()` performs the
identical composition and asserts `TerrainModel::data` is non-empty for a `mem`
fixture whose addresses lie outside every `codeimage` region). This brief is what
the fix is *for*: five graphs that answer questions no existing view can, all
riding one projection extension and one prefix-sum split, which is why the catalog
groups them and why they are worth landing together.

They answer: *is this address read-mostly or a write accumulator?* (T2) · *what is
being touched right now versus drifting cold?* (T3) · *over what interval is each
address alive?* (T4) · *is the access pattern streaming, strided, or
pointer-chasing?* (T5) · *is each cell touched early, late, or throughout?* (T6)

## What already exists (verified 2026-08-02 against `b657876`; lines re-verified 2026-08-03)

- **`DataCell`** = `{cell, steps (ascending `mem` steps), cum_size (parallel prefix
  sum of size), cum_rw (parallel prefix OR of READ/WRITE bits), cum_read_size /
  cum_write_size (54 T2's split)}`
  ([terrain.h:142-160](../../../desktop/src/space/terrain.h#L142)). `steps` is
  ascending and precomputed *for `slice()`*, so `front()`/`back()` are free —
  which is T4's entire data requirement.
- **`slice(t)` is a binary search per cell, not a rescan**
  ([terrain.h:191](../../../desktop/src/space/terrain.h#L191)) — so a windowed
  delta (T3) is two binary searches, and a per-band count (T6) is one per band.
- **`mem_present` / `mem_note`** already carry the coarse-rung provenance
  ([terrain.h:110-111](../../../desktop/src/space/terrain.h#L110)) — *"coarse: no
  per-access memory stream"* when absent. Every layer here reuses that chip rather
  than inventing an empty-state message.
- **`coarse_slice()`** is the existing degrade-under-budget path
  ([terrain.h:211](../../../desktop/src/space/terrain.h#L211)), driven by
  `should_degrade`/`kScrubCellBudget` in the shell
  ([shell.cpp:1095-1112](../../../desktop/src/ui/shell.cpp#L1095)). Any layer that
  makes scrubbing expensive must plug into it, not around it.
- **`trajectory.cpp`'s ribbon machinery** is what T5 substitutes into: the same
  per-vertex, per-tid line construction, with the effective address in place of the
  PC.
- **The access-mark spurs already exist and are a different thing** —
  `access_spurs_` ([scene.h:371](../../../desktop/src/scene3d/scene.h#L371)) draws
  PC→data-cell spurs. T5 is the *order* of data accesses, not the PC's association
  with them; the HUD must not let the two read as one layer.

## Tasks

### ☑ T1 — The data-cell HUD contract: say which rung is feeding this (S)

**Goal.** Before any of the four data layers exist, the pane can state whether it
has per-access memory data, observed data spans, both, or neither — so an empty
layer is never mistaken for an empty program.

**Steps.**
1. Extend `placement_chips` ([hud.h:29-34](../../../desktop/src/scene3d/hud.h#L29))
   with the data-rung facts: whether `mem_present`, how many observed-data spans
   the projection carries and at what threshold (the `data_span_note`
   [54](../archive/gui/54-3d-catalog-phase0-plumbing.md) T1 adds), and how many `mem` accesses
   failed to place.
2. **Count the drops.** `terrain.cpp`'s mem scan `continue`d silently; make it
   increment a counter on the model. After
   [54](../archive/gui/54-3d-catalog-phase0-plumbing.md)
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

**Landed.** `TerrainModel::mem_accesses` / `mem_dropped`
([terrain.h:113-124](../../../desktop/src/space/terrain.h#L113)) counted in the mem
scan; `placement_chips` now owns the whole data-rung contract — the coarse/rich
chip (previously an untestable inline `chip()` call in `draw_scene_hud`, now one
source of truth), the observed-span count + gap threshold, and the placement census
([hud.cpp](../../../desktop/src/scene3d/hud.cpp)). The span label got a named
constant (`space::kObservedDataLabel`) so the HUD's count and the projection's
label cannot drift. Tests: `desktop/test/test_datalayers.cpp` fixtures A–D (zero
drops through the SHIPPING composition, all-dropped graded `Bad`, exactly 1-of-3
dropped, and the `mem`-less coarse chip in `mem_note`'s verbatim words).

### ☑ T2 — Read/write twin relief (M) · *needs [54](../archive/gui/54-3d-catalog-phase0-plumbing.md) T1 + T2*

**Goal.** Read-mostly constants, write accumulators and in-place RMW cells become
three visibly different shapes — an asymmetry the OR-merged terrain structurally
hides.

**Steps.**
1. Two mirrored bas-relief surfaces over the shared data cells: `+Y =
   log1p(cum_read_size ≤ t)` in a cool hue, `−Y = log1p(cum_write_size ≤ t)` in a
   warm one, both sliced by the terrain-time playhead. The two prefix sums are
   [54](../archive/gui/54-3d-catalog-phase0-plumbing.md) T2's.
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
[54](../archive/gui/54-3d-catalog-phase0-plumbing.md) T2 contributes to neither surface;
`TF_TORN` floors both.

**Done when.** The three access shapes are distinguishable and an uncaptured
direction is a hole.

**Landed.** `space/datacell.{h,cpp}` — `build_data_relief(model, t)`, one binary
search per data cell, no rescan. **One model change was needed and is the honest
core of this task:** presence could not be derived from anything that existed.
`cum_rw` folds an unrecognised `rw` token into `TF_READ` (the pre-existing tint
approximation), and `cum_read_size > 0` misreports a zero-BYTE access as an absent
direction — magnitude and presence are different questions. So `DataCell::cum_dir`
(a prefix OR of `DD_READ`/`DD_WRITE`, 0 for an unrecorded direction) was added
beside the 54 T2 sums, one byte per access. `ReliefCell::has_read`/`has_write` come
from it; a false one means the renderer emits **no vertex** on that side, so the
absence is geometric and no shader branch can draw it flat. Draw half:
`scene3d/data_layers_gl.cpp` (a new TU — five layers of GL in `scene.cpp` would be
a large footprint in the tree's hottest merge file; `scene.cpp` keeps one call site
and one teardown line). Legend: `relief_shape_swatches()` takes its text from
`space::relief_shape_label()` rather than copying it, so the legend and the model
cannot state two different rules. Tests: `test_datalayers.cpp` `t2_twin_relief` —
read-only asserts the write surface is **absent, not zero**; the mirror; both; the
unknown-direction access feeding neither surface and being **counted**
(`unknown_direction_bytes`); `TF_TORN` flooring **both**; the playhead cutting the
write surface off a mixed cell at `t=4`; the legend's exhaustiveness over
`ReliefShape`.

### ☑ T3 — Working-set tide (M) · *needs [54](../archive/gui/54-3d-catalog-phase0-plumbing.md) T1 + T2*

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

**Landed.** `build_working_set_tide(model, t, W)` in `space/datacell.{h,cpp}`:
`live_bytes` is the `(t−W, t]` delta over the existing prefix sums (two binary
searches, no rescan), pinned by a test that compares it to a brute-force sum over
the same range for **every (t, W) in a 4×4 probe grid** — so the fast path is
proved equal to the thing it replaces, not just plausible. A cold cell's watermark
is the same window sum recomputed **as of its own last hitting step**, so it
recedes to its last crest rather than to zero; a cell not yet touched at this slice
produces **no entry at all**, which is what keeps "went cold" and "never touched"
from looking the same. `W` is a HUD `SliderInt` labelled *"dwell window (trace-time
steps)"* — the unit is on the control, and it moves on the **terrain-time** axis
only (doc 34's two-axes rule); a window move rebuilds the tide on its own
condition, never by re-slicing the terrain and never through the degrade path.

**One honest correction to step 4.** The brief's degraded form — *"a binary 'the
window contains a write' flag from `cum_rw`"* — is not derivable: `cum_rw` is a
prefix **OR**, so it is monotone and cannot report that a bit is present *within* a
window, only that a bit not seen before the window **first appeared** inside it.
Shipping the stronger sentence would have been the exact fabrication step 4 exists
to forbid, so `TideCell::window_first_direction_bit` is named and labelled for what
it actually is, and `tide_tint_label(RwFlagOnly)` says *"no ratio... cum_rw is a
prefix OR, not a ratio"*. A test asserts the two tint labels differ and that the
degraded one refuses to call itself a ratio. In practice the split path is what
runs: `RwFlagOnly` is reached only when **no** access in the recording carried a
recognisable `rw` token.

The live crest and the cold watermark are separate GL batches, so no draw-order
accident can composite a decayed cell into the live mass; the watermark draws at
10% alpha in the live hue (it is the same quantity at an earlier time).

### ☑ T4 — Observed-lifetime pillars (M) · *needs [54](../archive/gui/54-3d-catalog-phase0-plumbing.md) T1*

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

**Landed.** `build_lifetime_pillars(model)` in `space/datacell.{h,cpp}` — one pass,
no rescan: both ends are `DataCell::steps.front()/back()`, already precomputed and
ascending. A WHOLE-RECORDING aggregate, so it uploads on the weave gate, never on a
scrub (a Gantt that moved with the playhead would not be a Gantt); the playhead
reads as a plane through the pillars instead.

`lifetime_pillar_label()` carries this brief's load-bearing sentence, and the test
does not merely grep for "observed" — it walks **every** occurrence of `allocat` in
the label and fails unless each one sits inside an explicit denial. Open-topped
pillars (torn capture, last touch at the observed tail) live in their **own** GL
batch with their own colour and are counted in the HUD in the refuse colour: an
interval and a lower bound on an interval are different claims and must not share a
buffer. Torn-ness is not smeared over every pillar — a test asserts a pillar that
ended *before* the tail stays closed.

A zero-length pillar (one single touch) states `zero_length` and draws a deliberate
one-step stub rather than vanishing: the touch was really observed, and a vanished
pillar would hide it. Tests: 5-and-900 yields **one** pillar spanning 5..900 (never
two); the single-touch stub keeps its stated zero-length interval; the torn tail
open-tops exactly the right pillar; no `mem` yields no pillars at all.

### ☑ T5 — Data-access worldline ribbon (M) · *needs [54](../archive/gui/54-3d-catalog-phase0-plumbing.md) T1*

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

**Landed.** `space/dataribbon.{h,cpp}`. `build_data_ribbon(rec, proj)` reuses
`trajectory.cpp`'s construction shape — same `mem` events, same `step` axis, same
ordering rule — with `ea` substituted for the PC. Fallback to
`DataflowStream::recs`' **abs**-space `ValRec`s when `mem` is absent (an `"off"`
record is region-relative and is never placed raw, the same rule 54 T1 holds), and
`RibbonSource` is stated in the legend with the sentence *"NOT the same population:
the emulator's hardware hooks see implicit stack traffic a live `mem` enumeration
does not"*.

**What "a gap in the step coverage" actually means here, made checkable.** The
brief asks for a break rather than a joined segment; the concrete rule shipped is:
a segment is emitted only when **both** endpoints place **and** both steps lie
inside the covered step domain (`TraceStream::insns.size()`, or
`DataflowStream::insn_off.size()`), the number of steps the capture actually holds.
An unplaceable access is kept as a vertex and **counted** (`off_plane`) but breaks
the ribbon: joining across it would assert a locality step from A straight to C
that the program never made. Gaps are counted, never silent. The GL half walks
`DataRibbon::segs` only, so it structurally cannot invent a segment.

`rec.dropped()` is a whole-recording fact that cannot be localised to a pair, so it
is stated on the layer (`drops_present`, surfaced in the note) rather than silently
ignored or used to break every segment.

Shape tests are stated as **comparisons**, not as absolute plane distances: how far
two consecutive cells land apart is the Hilbert projection's business (it depends
on the compacted domain size and the clamped order), while *"a stride travels
further per step than a scan"* is this layer's own claim — so the test asserts
`max_hop(strided) > max_hop(sequential)`. Cross-span leaps get their own GL batch
and colour and the note calls them **GENUINE** non-locality, so a reader never
blames the compaction. The HUD adds one more line when `access order` and `access`
are both on, naming them as two different layers.

### ☑ T6 — Residency sediment columns (M)

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

**Landed.** `space/sediment.{h,cpp}` — one binary search per band over the ascending
`CodeCell::steps` / `DataCell::steps`, no rescan. Whole-recording (the layer's point
is to be readable with the playhead **stationary**), covering **code and data**
cells: T6 has no `mem` prerequisite and a test asserts a `mem`-less recording still
produces code columns.

**Built against the degrade path from the start**, as the brief instructed. The
shell's scrub budget was a function-local `kScrubCellBudget` inside the re-slice
block; it is now file-scope `kSceneCellBudget` and the sediment builder takes **that
same number**. Two throttles that could drift apart would not be "wired to the
existing degrade path". When `touched_cells × bands` would exceed it, the **band
count** halves until it fits (floor 1) — **never the cell set**: dropping cells
would silently delete measurements, while merging adjacent time windows costs
temporal resolution and nothing else. `degraded` + `bands_requested` state it, and
`sediment_note()` puts the band count in the HUD (T6 step 6), because a coarsened
column looks exactly like a sparsely-hit one otherwise. A test asserts the degraded
build has the **same number of columns** as the ungraded one.

**Conservation** is asserted over every column at seven different band counts
(1, 2, 3, 7, 10, 16, 64), including after coarsening. The band edges tile the axis
by integer proportion with the top band closed, and the axis is extended to
`max(nsteps, last_step+1)` — a `mem` stream can outrun the trace's step count, and
clamping the data instead of the axis would have broken conservation silently.

**One thing the brief could not have known.** `TF_STAT` cells have no per-step
attribution to band: `TerrainModel::stat` is a height field with no step list,
because a survey states *where* residency landed and never *when*. Fabricating
per-band counts for it would be precisely the invented temporal claim this layer
must not make. So a survey column carries its magnitude as **one unattributed band**
over the whole axis, `stat_has_no_phase` says so, and the note says so. It lives in
its own `stat` vector, never in `exact`. The isolation test is sharper than
"different cells": the fixture deliberately puts a survey edge on a **traced** cell,
and asserts the exact column's hit total still equals that cell's real step count —
i.e. the survey magnitude was never summed in.

## Fidelity notes (D7)

- **The single most important sentence in this brief is T4's**: these are
  observed-touch intervals, not allocation lifetimes, because nothing in the capture
  layer emits allocation. Every layer here inherits it — a "working set" (T3) is a
  set of *touched* addresses, and a "span" ([54](../archive/gui/54-3d-catalog-phase0-plumbing.md)
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
  [54](../archive/gui/54-3d-catalog-phase0-plumbing.md) T1 is claimed by another agent, take T6 and
  T1 here first; they are genuinely independent.

## How it landed (2026-08-03)

All six tasks, in order, one commit each. The two risks this section named both
held up in practice, and one of them changed the shape of the work:

- **Translucency did stop being decorative**, and the mitigation was structural
  rather than a render trick: every layer here is a **batched `GL_LINES` buffer**,
  and the halves that must never be confused get **separate batches** — read vs
  write, live vs watermark, closed vs open-topped, exact vs statistical, joined vs
  cross-span leap. That is what keeps a fidelity distinction from depending on draw
  order or on a shader branch. [55](55-scene-render-quality.md) T1 (EDL) and T4
  (dithered translucency) had landed, and the composed scene reads.
- **T6 was the frame-budget risk** and it is wired to the *existing* degrade path,
  not around it: `shell.cpp`'s scrub budget was hoisted from a function-local
  `kScrubCellBudget` to file-scope `kSceneCellBudget`, and the sediment builder
  takes that same number. Over budget it halves the **band count**, never the cell
  set.

Three places the brief's own prescription had to be corrected, each verified
against the code rather than argued from the doc (all three are written up in the
relevant task above):

1. **T2 needed a new prefix array.** Direction *presence* is derivable from
   neither `cum_rw` (which folds an unrecognised token into `TF_READ`) nor
   `cum_read_size > 0` (which loses a zero-byte access). `DataCell::cum_dir` is the
   honest three-state signal, and the whole absent-surface rule rests on it.
2. **T3 step 4's degraded flag is not derivable as written.** `cum_rw` is a prefix
   OR — monotone — so it can report that a bit *first appeared* in a window, never
   that a bit is *present* in one. The field is named and labelled for what it is.
3. **T6's `TF_STAT` columns cannot be banded at all.** A survey has no per-step
   attribution, so a statistical column is one *unattributed* band over the whole
   axis with `stat_has_no_phase` set, not a fabricated distribution.

New pure models: `space/datacell.{h,cpp}` (T2/T3/T4), `space/dataribbon.{h,cpp}`
(T5), `space/sediment.{h,cpp}` (T6). New draw half:
`scene3d/data_layers_gl.cpp` — `scene.cpp` gained exactly two lines (one
`draw_data_layers()` call in `render()`, one `free_data_layers()` in `shutdown()`).
New test TU: `desktop/test/test_datalayers.cpp`, whose `weave()` performs
`ui/shell.cpp`'s **exact** projection composition, so a layer that only worked
against a hand-built data region would fail these tests rather than pass them.

Five registry rows joined [56](../archive/gui/56-fidelity-and-module-layers.md)
T1's `LayerDesc` table (`relief`, `working set`, `lifetime`, `access order`,
`sediment`), all defaulting **off** — each adds a surface in the same footprint, and
a session should not open into a composited haze it did not ask for.

**Not done.** These layers are drawn, not **picked**: no pick band was allocated, so
the terrain cell under a bar stays the click target and the drill-ins each task
sketches (step 5 of T2/T3, step 4 of T4, step 5 of T5, step 5 of T6) open the flat
reader for the *cell*, not for the bar standing on it. Adding a band is
[47](47-scene-inspect-and-pickable-overlays.md) T3's allocator, and doing it per
layer here would have meant five new bands negotiated against a brief being
implemented concurrently.
