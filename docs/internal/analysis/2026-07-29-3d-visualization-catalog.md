# 3D visualization catalog — new spacetime graphs for the desktop GUI

Review date: 2026-07-29. Scope: ideation of a NEW SET of 3D visualizations for
the [`desktop/`](../../../desktop/) spacetime overview — extending the existing
scene ([scene3d/scene.h](../../../desktop/src/scene3d/scene.h)'s `SceneLayers`,
the Hilbert [projection](../../../desktop/src/space/projection.cpp) /
[terrain](../../../desktop/src/space/terrain.cpp) /
[trajectory](../../../desktop/src/space/trajectory.cpp) substrate) across the
concepts requested: **memory, libraries interacted with, data manipulations,
common operations & representations, hot paths, syscalls, hardware, and additive
composition**. Every concept is grounded in what the `.asmtrace` recording
actually carries ([doc/streams.h](../../../desktop/src/doc/streams.h)); nothing is
proposed that no producer can honestly emit.

**Method.** 17-agent grounded ideation: eight domain ideators each read the real
view-models and data streams and proposed 3D forms, an adversarial pass re-opened
the code to reject anything unbuildable, structure-fabricating, or duplicative of
an existing view, and a synthesis pass deduped and ranked the survivors. **33
concepts survived, 1 dropped**, consolidated into **14 additive LAYERS** (they
compose on the one shared plane) and **12 standalone SCENES** (they need their own
coordinate system). Held throughout to the app's tested rules: *3D to find, 2D to
read*; the two time axes stay unfused; the render-only viewer stays engine-free;
and the honesty invariants below.

> **Verification.** The load-bearing buildability claims were spot-checked against
> the tree. Confirmed: the terrain fidelity flags `TF_STAT/TF_TORN/TF_CHURN`
> (`space/terrain.h:50-52`, set in `terrain.cpp`) exist; exact per-offset `heat`
> exists (`views/canvas.h:31`); `ValRec.wide/bytes` is decoded and used in the 2D
> views (so it is free for a 3D layer); and the syscall view genuinely drops
> `Event::seq` today (a real Phase-0 prerequisite). One caveat: a few cited
> accessor *names* are aspirational (e.g. a per-Hilbert-cell `full_heat_by_cell`
> roll-up does not exist yet — the raw per-offset heat and the projection do, so
> the aggregation is real work, not a missing capability). Line numbers are as of
> HEAD `4c75edb`; re-verify before building.

This is an ideation catalog, not a schedule — it is a menu the GUI briefs can
draw from. A companion to the [UX/dataviz review](2026-07-29-gui-ux-dataviz-review.md).

---

## 1. Thesis

The organizing idea is that the existing spacetime scene is not one finished visualization but a SUBSTRATE — a Hilbert-projected, compacted address plane with a vertical time axis and per-cell height — and almost every new graph is an additive, HUD-toggled LAYER that re-lifts or annotates those same cells to answer a different overview question, while a smaller set are standalone SCENES that need their own coordinate system (a value-lane loom, an invocation-index stack, a def-use generation cone, an execution-step A/B fork, a time×tid kernel floor). Composed on one plane, the layers turn the scene from 'where did the PC go and how hot is memory' into a stack of orthogonal overview questions answerable at a glance and drilled for exact reading: how much do I TRUST each region (exact/sampled/torn/unknown), what KIND of work does each code region do, which LIBRARY is hot as a whole, which addresses are read-mostly vs write-accumulators, what is the working set touching RIGHT NOW vs drifting cold, where does a tainted value SPREAD and escape, where does control cross into the KERNEL, and where does the branch predictor STRUGGLE. The scenes add the questions the plane structurally cannot host: how two recordings DIVERGE in architectural state, how one routine's control flow VARIES across its calls, which thread lives in which library WHEN, what happens INSIDE a wide vector register, and where a causal cone has a single BOTTLENECK. The whole set is unified by a strict fidelity discipline so that adding layers never launders statistical or truncated data into confident exact structure. The result is a composable overview instrument whose every pick routes into the flat 2D view that does the precise reading — 3D to find, 2D to read.

---

## 2. The substrate, and what "additive" means

The backbone is space/projection + space/terrain + space/types: a locality-preserving Hilbert curve over a compacted address domain (sparse gaps packed out), a vertical trace-time axis, and per-cell TerrainModel state built ONCE and re-sliceable. 'Additive' means concrete, mechanical things. (a) SceneLayers toggle model: each layer is a bool (plus, for the exploded stack, an explode toggle) on the existing HUD, co-existing on the identical plane cells so their X/Y stay registered — a Confidence-terrain lift, an Opcode-class tint, a Residency-skyline canopy, a Misprediction arc, and a Crossing spur can all be on at once, each reading a different channel of the same cell. (b) Layers that re-lift the SAME cells by a NEW quantity: the base terrain uses density as height; Confidence terrain uses fidelity class as height; Read/write twin relief splits DataCell into mirrored read/write bas-relief; Residency-skyline sums raw per-cell heat per region into a canopy. (c) ACCUMULATION over time on the terrain-time axis without fusing it into the exec-step playhead: Residency sediment columns stand the whole per-cell time-integral up as banded columns; Working-set tide takes a windowed cum_size delta; Observed-lifetime pillars span [steps.front(), steps.back()]. (d) SUPERIMPOSITION across recordings: Ensemble stability terrain re-runs build_terrain for N runs onto one canonical code-only projection; Divergence worldline and the pairwise diff view align two runs on execution step. The scenes deliberately leave the plane when their load-bearing axis is not an address (def-use generation, invocation index, value-lane, tid, kernel boundary), and say so. Two prerequisites are shared plumbing, not per-graph work: an observed-data-span projection extension (clustered from mem/ValRec addresses, kind=Unknown, so data cells place at all) unblocks the entire memory data-cell family, and a DataCell cum_read_size/cum_write_size split (one re-aggregation of data already scanned) unblocks both twin relief and the tide's crest hue.

---

## 3. Honesty frame — applies to every graph

Every graph in this set obeys four invariants uniformly. (1) STATISTICALLY DISTINCT: any survey-derived surface (IBS SurveyEdge/HotEdge counts, TF_STAT residency, mispred fill) is drawn in physically separate stippled/translucent/offset ink, carries a persistent 'STATISTICAL — survey' label, and is NEVER summed or blended into an exact terrain, trajectory, or convergence arc; a recording that wrongly claims exact:true gets the provenance_conflict banner ride-along. (2) TRUNCATION SURVIVES DRILL-IN: a TF_TORN cell frays/hatches/floors in 3D AND carries its truncation banner into whichever 2D reader the pick opens; a bounded def-use walk, an open RegionInvocation, insns_total>insns.size, and dt_divergence.bounded all render as explicit lower-bound geometry (frayed cap, '≥N'), never a clean proven-complete terminus. (3) UNKNOWN-NOT-ZERO: an undescribed step (has_step()==false), a value_valid==false operand, a region-footprint cell with no exact or statistical content, an unparsed syscall class/return, an uncaptured wide bytes[] buffer, or a k<N ensemble support all render as a distinct 'unknown' mark (sunken hatched pit, hollow rib/route, wireframe, dark mask, opacity gap) — deliberately structurally different from an observed zero. (4) NO FABRICATED STRUCTURE (D7): no force-directed layout, no dominator/CFG-cycle reconstruction of loop nesting, no element→lane def-use threads the edges never carry, no allocation lifetime where only observed-touch exists, no path-chaining presented as an observed run, no invented module name for a codeimage-only region. Redacted payloads stay redacted through the drill-in; the address plane's compacted-domain gaps are never miscounted as 'never described'.

---

## 4. Phase 0 — shared plumbing (unblocks much of the set)

These are prerequisites, not user-facing graphs. Several graphs are cheap once
these land and blocked until they do.

*These are small, mostly one-shot enabling changes that each unblock several graphs; doing them first turns most Phase 1-3 items into pure render work with no producer risk.*

- Observed-data-span projection extension: cluster mem/ValRec addresses into compacted spans, kind=Unknown, label 'observed data' (the SINGLE blocker for the whole memory data-cell family: tide, twin relief, pillars, data-access ribbon)
- DataCell cum_read_size/cum_write_size split (one re-aggregation of the existing cum_rw scan; unblocks twin relief and the tide's crest hue)
- Carry Event::seq through obs_syscalls_build (currently dropped; needed by crossing spurs, boundary curtain, module excursion, module braid)
- Factor asm_language.cpp word-lists into an engine-free, ImGui-free mnemonic_class(token, guest) header (needed by opcode terrain + computation-character)
- Client-side BFS-depth walk helper over DataflowStream.edges (lineage.cpp::walk semantics; needed by taint isochrone + cone frustum)
- Add an optional invocation-index field appended LAST to dt_link (byte-stable like pid; needed by invocation stack)
- Extend SurveyEdge with is_return+from_addr OR standardize on HotEdge for survey marks (needed by misprediction layer + module braid)

---

## 5. Additive layers — compose on the shared address×time×height plane

Each is a `SceneLayers` toggle; several can be lit at once, all reading a
different channel of the same registered cells. Ranked value-then-effort.

### 1. Confidence terrain (+ coverage-window mask)

`high value` · `medium effort` · Hardware & microarchitecture / framework

- **Answers.** Across the whole plane right now, how much do I trust each region — exact, sampled, torn, or never-described — and where the survey stated a window, is an empty cell 'looked-and-saw-nothing' or 'never-looked'?
- **Encoding.** Re-lift the SAME plane cells by FIDELITY CLASS instead of heat: exact code/data = solid opaque hue; TF_STAT survey = stippled translucent; TF_TORN = frayed hatched upper edge; a region-footprint cell with neither exact nor statistical content, or a df step with has_step()==false, = a SUNKEN hatched 'unknown' floor; TF_CHURN still tick-marks. Optional overlay when have_window: in-window credited mounds, in-window-empty 'below-rate' cross-hatch, out-of-window dark 'never-looked' mask. Rides the terrain trace-time axis so cells reclassify as the playhead advances.
- **Built from.** `TerrainModel TF_STAT/TF_TORN/TF_CHURN flags` · `TerrainModel.stat (separate survey terrain)` · `DataflowStream.has_step()/step_present` · `Streams.truncated/torn/statistical` · `Projection region extents (footprints)` · `HotEdgeView.have_window/window_base/window_len/sampler/lost/throttled` · `SurveyEdge.count`
- **Drill-in.** Pick by class via resolve_pick: exact code -> canvas (disasm if churned), exact data -> slice, statistical-only -> hotedges (full sampler/lost/throttled chrome + evidence label), unknown region cell -> region view 'never described here'; a torn cell carries its truncation banner into whichever 2D view opens.
- **Honesty.** Unknown is a sunken pit, never zero height; statistical class never merged into exact; torn frayed and loud through the drill-in; window absent -> NO mask fabricated (defer to the statistical layer, 'whole-process assumed'); in-window-empty labelled 'below-rate — unknown, not cold', never per-address scanning.

### 2. Per-module residency skyline

`high value` · `medium effort` · Libraries & module boundaries

- **Answers.** Which library/module is actually hot AS A WHOLE — the per-cell terrain cannot be summed by region by eye?
- **Encoding.** Additive HUD-toggled canopy: one translucent canopy per Projection region at height = LOG(sum of RAW per-cell heat over that region's footprint cells) — aggregate the canvas full_heat_by_cell THEN log, NOT the already-log Terrain.height; region-kind hue + label; no smoothing across region boundaries. A region with cells mapped but zero heat = flat wire-outline footprint. TORN cells within a region hatch its canopy. Survey residency (TF_STAT cells) raises a physically separate, stippled, offset, labelled canopy. Fine per-cell terrain still shows underneath.
- **Built from.** `Projection.regions footprints` · `dt_canvas full_heat_by_cell (raw per-cell heat)` · `TerrainModel TF_TORN/TF_STAT flags` · `Region.label/kind (space/types.h)`
- **Drill-in.** Exact canopy -> region view or tree filtered to module (TORN carries its truncation banner); statistical canopy -> hotedges.
- **Honesty.** Aggregate raw heat then log (never sum already-log heights); mapped-but-cold region = outline, never a solid slab; survey canopy is a separate object, never summed into the exact one; torn module reads as a hatched floor that survives drill-in.

### 3. Opcode-class code terrain

`high value` · `medium effort` · Common operations & value representations

- **Answers.** What KIND of work does each code region do — move / int-arith / logic / compare-branch / scalar-float / vector-SIMD / system — as a spatial map, not a linear disasm scroll?
- **Encoding.** Per Code-kind cell, bucket the offsets landing in it -> first disasm token -> a purpose-built OpClass; tint by the DOMINANT class, saturation = purity, with a 2-3 stack of runner-up ticks so 'mixed' is legible. Memory-touch class is derived ONLY from ValRec.space (abs/off), never mnemonics. Only Code regions are painted; reuses the substrate unchanged.
- **Built from.** `TraceStream.disasm (per-offset recorded text)` · `DataflowStream.disasm (per-step)` · `ValRec.space (abs/off)` · `NEW engine-free mnemonic_class(first_token, guest) factored from asm_language.cpp word-lists (no TextEditor.h/ImGui, no engine)`
- **Drill-in.** Pick a cell -> Selection.set(off = the cell's hottest offset) -> disasm view (a cell spans a range; land on the representative offset and let the reader scroll).
- **Honesty.** Empty disasm or any token outside the known sets = UNKNOWN, hatched/neutral, never coerced to 'move' or zero; survey-only cells carry NO class (stay TF_STAT); torn keeps TF_TORN, churn keeps TF_CHURN; classifier is guest-gated (x86 vs arm64) and ambiguous edges (movd/movq, cvtsi2sd) flagged, not silently bucketed.

### 4. Misprediction survey layer (bias arcs + brittle-branch site columns)

`high value` · `medium effort` · Hot paths & hardware / microarchitecture

- **Answers.** Where does the branch predictor struggle — which CFG EDGES and which branch SITES carry the mispredictions across the whole survey?
- **Encoding.** Two statistical idioms on one 'branch (statistical)' toggle, never summed with exact. (a) Arcs: one quadratic-bezier tube per HotEdge from project(from_addr) to project(to_addr), apex height/radius = log10(count), colour = mispred/count on a cool->amber bias ramp, is_return dashed, a derived latch curvature (to<from within one region) labelled derived. (b) Site columns: per from_addr cell, an outer glassy sheath = log(count) with an inner solid core = log(mispred) so fill reads mispred rate; is_return chevron cap.
- **Built from.** `HotEdge{from_addr,to_addr,count,mispred,is_return}` · `HotEdgeView{sampler,samples,lost,throttled,provenance_conflict,have_window}` · `Projection.project` · `(extend SurveyEdge with is_return+from_addr, OR source from HotEdge — SurveyEdge decode currently drops is_return)`
- **Drill-in.** Pick an arc or column -> hotedges ranked table + src x dst matrix scrolled to that from address.
- **Honesty.** Whole layer wears 'STATISTICAL — survey' + provenance_conflict stamp, never merged into exact terrain or the cross-thread convergence arcs; mispred=0 renders a genuinely cool zero-height core in a full sheath (a real low value, distinct from a dropped weak-evidence layer); an endpoint Projection.project cannot place -> 'N off-plane' HUD chip, never dropped; opacity graded by sampler (ibs-op crisp vs sw-clock washed); latch curvature labelled derived, is_return is recorded.

### 5. Crossing spurs on the worldline

`high value` · `medium effort` · Syscalls & the kernel boundary

- **Answers.** Where along the execution path being read did control actually LEAVE userspace into the kernel — in situ on the worldline the flat list can't give?
- **Encoding.** Additive layer on space/trajectory, no axes of its own. For each syscall event, anchor to the last trace insn with a smaller Event::seq; from that worldline vertex a spur shoots OFF the address plane to a thin floating kernel rail, a return spur comes back to the resume vertex, and out+return meet at the rail with NO span. Hue = derived class; thickness = row.payload.size() when has_payload; return spur tinted by parsed outcome (green/red/grey). Reuses per-tid trajectory colouring.
- **Built from.** `Event.seq (carried through obs_syscalls_build, currently dropped)` · `TraceStream::insns (interleaved against seq)` · `SyscallRow{tid,line,payload,record_redacted}` · `space/trajectory worldline`
- **Drill-in.** Pick -> deep-link to the syscalls row (payload still redacted).
- **Honesty.** SELF-GATES: no TraceStream::present() worldline -> layer disabled, HUD says why (never invents a path to hang spurs on); anchor labelled 'approx (last insn)' and drawn hollow where the trace is sparse/truncated; kernel dwell unmeasured (out+return meet with no length claim); record_redacted spurs hatched 'withheld at record time', content never shown.

### 6. Taint isochrone (forward-spread front)

`high value` · `medium effort` · Data manipulations & dataflow

- **Answers.** From a chosen definition, how FAR and WHERE does the value spread across the address plane, and does it ESCAPE into a different region kind?
- **Encoding.** Plane layer. Compute a generation number with a pure client-side BFS-depth walk over DataflowStream.edges (lineage.cpp::walk semantics: adjacency + first-reach depth). Tint each reached step's memory-WRITE ValRec (write && space in {abs,off}) at addr -> Hilbert cell by BFS depth on a near=hot ramp; an escape glyph where the reached cell's region-kind differs from the origin's. The front advances on the def-use GENERATION (execution-step playhead), never the terrain residency slice.
- **Built from.** `DataflowStream.edges (dt_edge)` · `DataflowStream.recs (ValRec write/space/addr/value_valid)` · `Projection cell` · `Streams.blame (origin seed)` · `space::Region kind`
- **Drill-in.** Reached cell -> slice at the writing step; origin -> blame; escape cell -> slice at the store.
- **Honesty.** Reg-only writes have no cell and never tint (never colour cell 0); slice_view LOWER-BOUND banner under truncation; steps_missing/!has_step() -> UNKNOWN gaps in the front (never 'not reached'); value_valid==false spreads as a hollow route; exact-only, SurveyEdge never fed. Generation via BFS-depth, NOT dt_slice_forward (which returns only a de-duped step set with no depth).

### 7. Read/write twin relief

`high value` · `medium effort` · Memory & working set

- **Answers.** Is each data address a read-mostly constant, a write accumulator, or an in-place RMW cell — asymmetry the OR-merged terrain structurally hides?
- **Encoding.** Two mirrored bas-relief surfaces over the shared data cells: +Y = log1p(cum_read_size <= t) in a cool hue, -Y = log1p(cum_write_size <= t) in a warm hue, both sliced by the terrain-time playhead. Read-only const buffer = cool peak/no pit; accumulator = warm pit/no peak; in-place RMW = balanced peak+pit pinched at the plane.
- **Built from.** `mem.rw + size (already scanned at terrain.cpp:373-401)` · `NEW DataCell.cum_read_size / cum_write_size parallel prefix sums (split of the single cum_rw OR-flag — one re-aggregation, unblocks the tide's crest hue too)` · `observed-data-span projection extension (kind=Unknown)`
- **Drill-in.** Pick a peak/pit -> slice/timeline at the cell's last access; a write pit may additionally offer a views/watch arm at that address.
- **Honesty.** Reads-up and writes-down are never merged; a direction never captured is an ABSENT surface, not a zero-height peak; no RMW inferred where only one direction was recorded; torn floors both surfaces; absent mem = flat + note. PREREQ (shared with ranks 8/9/12): observed-data-span projection extension.

### 8. Working-set tide

`high value` · `medium effort` · Memory & working set

- **Answers.** What is the program ACTIVELY touching right now versus drifting cold — recency and drift the cumulative, monotonic terrain structurally cannot show?
- **Encoding.** A second, windowed height surface over DATA cells only, height = log1p(cum_size[upper_bound(t)] - cum_size[upper_bound(t-W)]) per DataCell, W a HUD dwell knob on the terrain-time axis (exec-step axis untouched). Cold cells (last touch < t-W) drawn as a faded ~10% watermark at last crest height, excluded from live mass. Crest tint adopts the twin-relief read/write split; if that split is not taken, degrade to a binary 'window contains a write' flag from cum_rw and label it as such.
- **Built from.** `DataCell.steps + cum_size delta (terrain.h:99-101, two binary searches)` · `DataCell.cum_read_size/cum_write_size (from rank 7)` · `observed-data-span projection extension`
- **Drill-in.** Pick a crest cell -> slice at the last hitting step (+ timeline).
- **Honesty.** The receding watermark is an honest decay, never a zero; an OR flag is never presented as a read/write ratio; torn floors the window and TF_TORN survives drill-in; survey (statistical PC-space) never fed here; absent mem -> flat + mem_note, never a silent zero. PREREQ: observed-data-span projection + the rank-7 split.

### 9. Observed-lifetime pillars

`high value` · `medium effort` · Memory & working set

- **Answers.** For each address, over what INTERVAL is it observed alive — a Gantt-in-3D over the plane that the density-at-a-slice terrain never shows?
- **Encoding.** Per touched DataCell, a translucent vertical pillar spanning [steps.front(), steps.back()] (both already in DataCell.steps — no rescan), Y = trace time. Short stub = transient scratch, full-height pillar = persistent state; colour by read/write dominance. A horizontal band of pillar starts/ends = a phase boundary. The terrain-time playhead reads as a horizontal plane: below=born, intersected=live, above=untouched.
- **Built from.** `DataCell.steps (front/back, ascending, precomputed)` · `DataCell read/write split (rank 7)` · `observed-data-span projection extension`
- **Drill-in.** Pick a pillar -> slice at its first or last step.
- **Honesty.** Labelled verbatim OBSERVED-TOUCH lifetime, NOT allocation lifetime — there is no malloc/free/mmap/brk producer anywhere (recording.cpp kind list is trace/coverage/.../mem/blame/statediff), so the real allocation may predate first touch and outlive last touch; a pillar whose last touch sits at the torn tail is OPEN-TOPPED (floor); uncovered steps are gaps, never zero-length pillars. PREREQ: observed-data-span projection.

### 10. JIT churn strata

`medium value` · `medium effort` · Libraries & module boundaries

- **Answers.** Where and how much does self-modifying / JIT code rewrite itself, and in what version order?
- **Encoding.** Additive layer. Over a code region footprint, stack translucent horizontal sheets — one per DISTINCT codeimage version at that base, read from the RAW codeimage events (base/len/version/bytes), NOT regions_from_codeimage which collapses to latest/widest. Z order = recorded appearance order (Event.seq), earliest at base, live version opaque on top, older sheets fade; version number labelled on the exposed edge; churn hue + version-count badge on multi-sheet stacks.
- **Built from.** `raw codeimage Events (base/len/version/bytes)` · `Event.seq (appearance order)` · `disasm.cpp CodeVersion list (version, bytes, when)`
- **Drill-in.** Sheet -> disasm view's CodeVersion list SELECTING that specific version (the view enumerates all versions though it defaults to the greatest); stack base -> region view.
- **Honesty.** Only recorded version bumps produce sheets; a base seen at one version = a single ground sheet; a static/non-JIT region carries no stack; a bytes change with no recorded version bump stays one sheet (unknown != churn); truncated codeimage history flagged, never back-filled.

### 11. Blame convergence forest

`medium value` · `medium effort` · Data manipulations & dataflow

- **Answers.** Across ALL attribution cones, which producing step is the shared root cause many sinks trace back to — a triage overview no per-cone 2D list surfaces?
- **Encoding.** Plane layer over Streams.blame. Sink at insn_off[sink] -> Hilbert cell; spurs from each cone[] producer step (-> insn_off -> cell); a producer cell's convergence weight = count of DISTINCT cones whose cone[] contains that step. Rendered as a distinct beacon/brightness on its own toggle, NOT reusing terrain height (which already encodes access density).
- **Built from.** `Streams.blame (BlameAttr.cone[] ascending producing steps, born_untraced, off/loc)` · `insn_off -> Projection cell`
- **Drill-in.** Convergence spike -> slice at the shared step; sink -> blame; producer cell -> disasm.
- **Honesty.** Convergence weight is an honest count of set overlap, never a synthesized cross-cone link; born_untraced cones = sink alone (never a false shared spike); truncation -> every cone a lower bound, a missing convergence is 'not seen' (reuse slice_view banner); exact blame only. Rich only when the recording blames multiple sinks — degrades honestly to a faint single bundle.

### 12. Data-access worldline ribbon

`medium value` · `medium effort` · Memory & working set

- **Answers.** Is the data-access ORDER streaming, strided, or pointer-chasing — a locality SHAPE a flat address list cannot show but a Hilbert plane can?
- **Encoding.** A ribbon over the DATA-address plane: one vertex per mem access at its ea's plane cell, consecutive accesses joined in step order, Y = trace time; width = access size, colour = read/write. Reuse trajectory.cpp's ribbon with ea substituted for PC. A sequential scan hugs adjacent cells, a stride zig-zags, pointer-chasing leaps; leaps across distinct observed-data spans are honest non-locality, labelled. Explicitly NOT the existing PC->data access-mark spurs.
- **Built from.** `mem (ea ordered by step, size, rw)` · `DataflowStream.recs ValRec abs/off addr/size/write (fallback when mem absent but --dataflow present)` · `observed-data-span projection extension`
- **Drill-in.** Pick a ribbon segment -> slice/timeline over that step range.
- **Honesty.** Exact-only (never weave survey); an uncovered step = a GAP, never an interpolated segment; torn -> torn cap surviving drill-in; observed-data spans stay kind=Unknown; distant-span leaps are labelled honest non-locality, not a layout artifact. PREREQ: observed-data-span projection.

### 13. Residency sediment columns

`medium value` · `medium effort` · Additive composition & framework

- **Answers.** Is each cell touched early, late, or throughout — the phase the moving terrain slice reveals only by scrubbing, stood up with the playhead paused?
- **Encoding.** Additive SceneLayers bool. For each touched cell, subdivide a slender column along the terrain-time Z into bands [tau, tau+delta]; band opacity/height = hit count from CodeCell.steps (data cells from DataCell.steps) via binary search in that window — no re-scan. Exact columns solid; TF_STAT columns stippled and in the separate stat object.
- **Built from.** `CodeCell.steps` · `DataCell.steps (ascending, precomputed for slice())` · `TerrainModel TF_STAT/TF_TORN`
- **Drill-in.** Pick a band -> trace canvas (code) or slice explorer (data) at that offset and the band's step range; pick the column -> the cell's canvas.
- **Honesty.** TF_STAT columns never merged into exact; TF_TORN caps the column and floors everything above as a lower bound; a never-hit cell places NO column (absence, not a zero nub); rides the terrain trace-time axis only, never the exec-step playhead.

### 14. Dominant-path ridge

`medium value` · `medium effort` · Hot paths & control flow

- **Answers.** At each fork, which successor does control usually take — the aggregate modal backbone and the fork mass?
- **Encoding.** A ridge layer over the substrate. At each block, the modal (max-count) observed successor from aggregated consecutive-block transitions in TraceStream.insns/blocks; a raised tube threads project()'d block cells in program-visit order, height = transition count (log). MANDATORY tie-dimming: fraction->1 solid/bright, fraction->0.5 dim/visibly split. Fork glyph on the plane sized by leaving mass (1 - modal_fraction). Survey fallback (max-count outgoing HotEdge per block) in statistical ink, never blended.
- **Built from.** `TraceStream.insns/blocks (ordered, dedup ascending)` · `SurveyEdge/HotEdge count (fallback)`
- **Drill-in.** Pick a segment -> canvas/disasm at the offset; pick a fork -> hotedges (off=block_addr) for the exact successor split.
- **Honesty.** REFRAMED as a per-fork modal aggregate labelled 'modal path (aggregate)', NOT a claim any single run followed the whole chain (avoids the documented greedy-reconstructor trap); a block whose successor was never recorded caps the ridge 'unknown continuation', never wraps to offset 0; a truncated trace's counts are a stated lower bound; survey fallback in separate ink.

---

## 6. Standalone scenes — their own coordinate system

These leave the address plane because their load-bearing axis is not an address
(a def-use generation, an invocation index, a value lane, a thread id, the kernel
boundary) — and each says so. Ranked value-then-effort.

### 1. Divergence worldline

`high value` · `medium effort` · Additive composition & cross-recording

- **Answers.** Two recordings of the same routine — where do their architectural states FIRST diverge and how does the disagreement widen?
- **Encoding.** Standalone scene, vertical = execution STEP. A fused tube climbs the shared prefix; at dt_divergence.step it forks into an A and a B ribbon (tid/region-kind coloured). For each dt_state_step, a horizontal rib between the ribbons: thickness = regs.size(), coloured by register class (derived from reg name). A bright pillar marks patient zero.
- **Built from.** `dt_divergence (step, off_a/off_b, bounded)` · `dt_statediff (per-step diverging register names, bounded, merged)` · `code_sha/basis/arch admission gate` · `TrajectorySet (tid colouring only, not geometry)`
- **Drill-in.** Fork pillar -> diff view patient-zero row (v=diff, rec/rec_b, step); rib -> slice explorer / operand timeline (or v=blame) at that step.
- **Honesty.** Refusal card on a failed code_sha/basis/arch gate; dt_divergence.bounded -> the shared tube ends in a TORN cap (not a clean proven-identical terminus); a bounded/uncomputed step -> a HOLLOW rib (unknown), never zero-width 'agree'; post-fork ribs are step-indexed architectural-STATE disagreement per dt_statediff, NOT proof of instruction correspondence after the streams parted; no statediff -> ribs absent + note.

### 2. Invocation stack

`high value` · `medium effort` · Hot paths & control flow

- **Answers.** How does one routine's control flow VARY across its many calls — the pager shows one invocation at a time?
- **Encoding.** Keep the Hilbert plane; swap the vertical axis from trace-time to INVOCATION INDEX (discrete, labelled 'invocation #, not time', never a scrub). One horizontal block-heat slab per RegionInvocation (or per SegmentedDataflow pass), stacked in call order; per-slab column height/opacity = that block's execution count within that one invocation. A block present in some slabs and absent in others pops as a mismatched stack.
- **Built from.** `RegionInvocation (insns ordered, blocks dedup, blocks_total/insns_total, truncated, closed, number)` · `SegmentedDataflow.passes` · `codeimage version tint` · `NEW optional dt_link invocation-index field, appended LAST for byte-stable forward-compat like pid`
- **Drill-in.** Pick a slab-cell -> region view at that invocation number and offset (the exact per-invocation reader), via the new invocation field.
- **Honesty.** Invocation index is discrete and never a time scrub (honors the two-axes rule); an OPEN (closed=false) or truncated invocation renders frayed/capped and labelled a prefix; an undescribed step is an unknown cell, never zero; exact-only; the drill-in MUST NOT overload dt_link.step (which means a dataflow step index) — add a dedicated invocation field. Distinct from the loop helix (that counts iterations WITHIN one run's loop; this compares control flow ACROSS calls).

### 3. Module excursion ribbon

`high value` · `medium effort` · Libraries & module boundaries

- **Answers.** Which THREAD lives in which library WHEN — cross-thread library-excursion patterns a single 2D icicle cannot show?
- **Encoding.** 3D earns itself across threads: put TIDS on the third spatial (lane/Z) axis — one parallel depth-vs-callorder sheet per thread. Per lane: X = call-order (Event.seq, a THIRD axis never fused with either playhead), Y = call depth (the engine's effective focus-rebased depth), band colour = current frame's module. A boundary crossing = the seam where the band colour changes, brightened. A single-thread recording degrades to a 2D icicle timeline, not a tilted flat chart.
- **Built from.** `TreeRow (tid, depth, addr, name, module)` · `Event.seq (call order)` · `started-params depth cap`
- **Drill-in.** Pick a segment -> tree at that call carrying its tid + module filter + target symbol.
- **Honesty.** A depth-capped capture marks the lane floor CAPPED (clipped edge, not a real bottom) so truncation survives drill-in; returns inferred only from recorded depth decreases (state it); an unresolved module = 'unknown' hue, never blank; no survey data touches this (it is the EXACT recorded call tree); single-thread -> honest 2D icicle.

### 4. SIMD lane prism

`high value` · `medium effort` · Data manipulations & dataflow

- **Answers.** What happens INSIDE a wide vector register over time — byte/element manipulation no current view looks into (the loom folds sub-regs to a 64-bit container)?
- **Encoding.** A prism: X = byte/element index, Z = time/stacked writes, Y = byte magnitude, colour = a stable value->hue hash. A recorded byte keeps its hue as it moves lanes under a shuffle, so a permutation is revealed HONESTLY through colour continuity — built entirely from ValRec.wide + bytes[] + size, decoded today but unused.
- **Built from.** `ValRec.wide + bytes[] (decoded in memory order) + size` · `disasm mnemonic (only for unambiguous element width)`
- **Drill-in.** Pick -> timeline/slice at the step (exact hex) + disasm at insn_off.
- **Honesty.** DROP the element->element def-use thread: dt_edge/edge_loc resolve only to a whole location (a single reg/addr), never a byte or lane, so a lane->lane permutation thread would fabricate a mapping the recording never carries — any hop is drawn only at register granularity (step->step); element width is NOT recorded (ValRec.size is TOTAL operand width) -> default 16 byte-lanes labelled 'element width not recorded', split into 32/64-bit only when the mnemonic is unambiguous; bytes[] absent/odd -> '[wide]' wireframe (never zero bytes); value_valid==false -> hollow.

### 5. Exploded evidence stack (SceneLayers framework capstone)

`high value` · `large effort` · Additive composition & framework

- **Answers.** For one recording, how do its fidelity layers relate cell-by-cell WITHOUT taller marks occluding shorter — physically isolating exact / statistical / access / churn?
- **Encoding.** Explode the SceneLayers of one recording into parallel translucent sheets stacked on a labelled layer-index Z (NEVER the trajectory time-Z): an exact-heat CodeCell sheet (solid), the TF_STAT survey residency sheet (stippled, physically separate), a DataCell access sheet (TF_READ/TF_WRITE tint), a TF_CHURN sheet. Thin vertical lines join the SAME plane cell across sheets where it is populated on more than one. Extend SceneLayers with one bool per sheet + an 'explode' toggle.
- **Built from.** `TerrainModel CodeCell/DataCell` · `TF_STAT/TF_TORN/TF_CHURN/TF_READ/TF_WRITE flags` · `SceneLayers (+ per-sheet bool + explode toggle)` · `resolve_pick per-kind router`
- **Drill-in.** Pick on a sheet routes through resolve_pick's existing per-kind logic to that sheet's 2D reader: exact -> canvas, churned -> versioned disasm, stat -> hotedges, access -> slice explorer.
- **Honesty.** Layer-index Z is labelled NOT time (two-axes rule); statistical is a physically separate object (the strongest form of the never-merge invariant); each sheet keeps its own TF_TORN floor; a cell absent from a sheet is a literal hole (unknown-not-zero); explode off = not-lifted, never faked-flat; HUD legend annotates each sheet with its fidelity grade + a cells-placed count.

### 6. The boundary curtain

`high value` · `large effort` · Syscalls & the kernel boundary

- **Answers.** What is the whole-run RHYTHM of kernel crossings across threads — bursts, errno storms, per-tid syscall cadence the flat ordered list cannot gestalt?
- **Encoding.** A time x tid FLOOR (X = Event::seq, Z = tid lane grouped into process bands via a topo tgid join) with a constant-height KERNEL WALL that encodes no magnitude. One tube per syscall crossing, floor -> wall; hue = derived class; thickness = row.payload.size() when has_payload, hairline otherwise; wall-strike rim = parsed return (green success / red -1 ERRNO / grey undecodable). A dense red wall patch = an errno storm.
- **Built from.** `Event.seq (carried through obs_syscalls_build)` · `SyscallRow{tid,line,payload,record_redacted}` · `TopoTask.tgid (process grouping)` · `ObsChrome (torn/exact/truncated)` · `NEW name->class classifier + return-value parser (both bucket-to-unknown on any miss)`
- **Drill-in.** Pick -> deep-link to the syscalls row (payload still redacted).
- **Honesty.** HUD states 'kernel dwell not measured' (the wall carries no length/height claim); syscalls carry no pc/addr, so the honest substrate is time x tid, NOT the address plane; parsers bucket to a visible 'other' / grey on any parse miss (never folded into io/net, never green-on-unknown); record_redacted strikes hatched 'withheld at record time', content never rendered; the floor+wall fade past the last recorded seq under torn (a prefix, not the whole).

### 7. Module interaction braid

`high value` · `large effort` · Libraries & module boundaries

- **Answers.** Which modules are the coupling HUBS, and how do exact calls and sampled control flow into/out of each library — a module abstraction the scene lacks?
- **Encoding.** Pillars on a region-base-ordered ring; height (log) = distinct (name+module) symbols entered per module from TreeRow; region-kind hue + label. TWO edge idioms, never summed: (1) EXACT call arcs — solid tube, thickness = log call count, source module reconstructed from the enclosing shallower same-tid frame via TreeRow.depth, tid-tinted; (2) SURVEY chords — stippled/translucent/offset, from HotEdge with '[module]' parsed. Red rim = mispred fraction (survey only); self-loop halo = intra-module count.
- **Built from.** `TreeRow (name/module/depth/tid)` · `Event.seq` · `HotEdgeView/HotEdge (from/to '[module]', mispred, is_return)` · `Region.label/kind` · `deterministic region-base order`
- **Drill-in.** Pillar -> tree filtered to module; exact arc -> tree focused on target symbol; survey chord -> hotedges matrix (statistical routes only to the survey reader).
- **Honesty.** Three label spaces (call.module / survey [module] / Region.label) kept as separate provenance — a mismatch = two pillars, never a silent merge; exact-arc source labelled 'inferred from depth; returns from depth-decreases'; a survey endpoint whose module does not resolve = an 'unknown' pillar/chord, never dropped and never merged into a resolved module; a mapped-but-unentered module = wire-outline pillar (breadth 0 != a zero solid); a codeimage-only region shows 'code@0x…', never an invented name; deterministic order, no force-directed layout.

### 8. Cone frustum (causal cone with bottleneck detection)

`high value` · `large effort` · Data manipulations & dataflow

- **Answers.** For one value, is its causal cone a wide FAN or a deep CHAIN, and is there a single BOTTLENECK every path funnels through?
- **Encoding.** Scene. Y = signed def-use generation via BFS-depth over DataflowStream.edges (lineage.cpp::walk semantics; back=-, fwd=+ — a DIRECTION, not a second wall-clock). A ring per generation; radius from node count at that depth; angular position by insn_off; node colour by region-kind of insn_off; edges = dt_edge between adjacent rings. The BOTTLENECK is TRUE articulation detection (a cut vertex every deeper path traverses) over the cone's real edge subgraph.
- **Built from.** `DataflowStream.edges (dt_edge)` · `insn_off` · `space::Region kind` · `Streams.blame (BlameAttr.cone seeds the apex, born_untraced)`
- **Drill-in.** Node -> slice with cone pre-selected; apex -> blame; bottleneck -> slice/disasm at the choke step.
- **Honesty.** BFS-depth, NOT dt_slice_forward's flat set; the bottleneck is a real graph property, not merely a size-1 ring (edges can skip generations, a bypassed lone node is no choke) — do the graph cut; born_untraced -> apex ALONE, never an empty cone; truncation/steps_missing -> frayed/unknown rim carrying the slice_view lower-bound banner; exact-only, SurveyEdge contributes no edge.

### 9. Value-ribbon braid (the value river)

`high value` · `large effort` · Data manipulations & dataflow

- **Answers.** How does a value flow between register and memory LOCATIONS over time — the hops the overplotted flat loom cannot separate?
- **Encoding.** Scene on the loom substrate. X = step, Z = lane (loom_reg_deck order then coalesced mem bands), ribbons = spans (t_write..t_end), threads = hops (from_span -> to_span). PIN the Y axis to a real quantity (value magnitude / crossing activity) or keep ribbons flat with separation entirely in Z — never an arbitrary offset dressed as data. Kept an OVERVIEW; all reading routes to 2D.
- **Built from.** `loom_fabric_build (lanes = reg deck + coalesced mem bands, spans kLoomAlive, hops from_span/to_span/edge, knots is_knot)` · `ValRec.value_valid` · `born_untraced` · `insn_off` · `StateDelta.changed (corroboration only)`
- **Drill-in.** Span -> loom biography/slice at t_write; hop -> slice at edge to_step; lane -> register scrubber.
- **Honesty.** loom_fabric_build HARD-REFUSES non-exact provenance (a statistical feed yields the refusal placard, never a woven braid — honesty is structural); to_span==kLoomNoSpan ('read defines nothing') MUST terminate, never invent a landing; value_valid==false -> hollow; born_untraced fades from t=0 with the instrumentation glyph; kLoomAlive spans fray off a truncated end; insn_off==UINT64_MAX -> unknown (never offset 0); knots -> twist glyph; StateDelta ticks are corroboration only; the Y axis is never an arbitrary offset masquerading as data.

### 10. Ensemble stability terrain

`high value` · `large effort` · Additive composition & cross-recording

- **Answers.** Across N runs of the same routine, where is execution STABLE versus VARIABLE — mean and spread a single-run terrain and the pairwise diff cannot express?
- **Encoding.** Ensemble mean+spread terrain over N workspace recordings that pass the code_sha/basis/arch gate. Build ONE canonical code-only ensemble Projection (code regions key/compact by len, not absolute base, so they align across ASLR); re-run build_terrain for each admitted recording ONTO that shared projection. Per code cell: height = mean full_heat (log), dispersion tint = CoV on the warn/hot axis, opacity = support k-of-N. Statistical (TF_STAT) runs form a physically separate stippled ensemble surface.
- **Built from.** `per-recording TerrainModel.CodeCell.full_heat` · `code_sha/code_present admission gate (dt_diff_build)` · `TF_STAT stat Terrain` · `build_projection / regions_from_codeimage` · `dt_heat_delta (drill-in target)`
- **Drill-in.** Pick a high-variance cell -> the pairwise diff view (v=diff) of the pair with the largest dt_heat_delta at that offset.
- **Honesty.** code_sha proves the routine BYTES match, NOT that the Projection is identical (each run compacts its own data/stack regions) — hence a canonical code-only projection keyed by len; data/stack rendered as an explicit non-co-registrable band, never averaged; a cell hit by k<N runs is opacity k (absence = a hole, never a zero valley); a TF_TORN contributor floors that cell's mean as a lower bound; a recording failing the gate is a refusal card with its reason; statistical never averaged into exact.

### 11. Loop helix nest

`high value` · `large effort` · Hot paths & control flow

- **Answers.** How many ITERATIONS does each loop run and how do loops NEST — iteration structure the terrain and trajectory do not extract?
- **Encoding.** Standalone scene. Per loop detected AS OBSERVED RECURRENCE of a header offset in TraceStream.insns / RegionInvocation.insns, a vertical helix: turns-up = iteration index (one turn per observed recurrence, an exact count), angle = position of the offset within the observed body span, turn colour = per-iteration block heat over region-kind hue.
- **Built from.** `TraceStream.insns (ordered)` · `RegionInvocation.insns` · `blocks/blocks_total/insns_total (truncation)` · `region-kind`
- **Drill-in.** Pick a turn -> region (or canvas) at the header offset for the exact per-invocation reader — route to timeline/slice ONLY when a DataflowStream pass actually exists for that step; pick the helix -> region view.
- **Honesty.** CONSTRAIN nesting to what the linear stream states unambiguously: draw an inner coil only where an inner header's revisits are strictly contained within a single outer iteration span EVERY time; where ambiguous (irreducible flow, shared/overlapping headers) draw SIBLING coils labelled 'nesting not determinable from the trace' — no dominator/CFG-cycle reconstruction (D7, the documented block-step trap); exact-only (a survey-only recording states 'loop iteration structure needs an exact capture'); truncation/open invocation frays the top '>= N iterations (truncated)' as a lower bound; no repeated offset yields NO helix (never a fake single-turn coil).

### 12. Computation character over invocation passes

`medium value` · `medium effort` · Common operations & value representations

- **Answers.** Across a routine's MANY invocations, how does its instruction-class and operand-width character EVOLVE from warm-up to steady-state — cross-pass evolution no single 2D view shows at once?
- **Encoding.** Promote to 3D ONLY when a recording has many invocations and the question is cross-pass evolution: X = step-window, Z = invocation PASS (SegmentedDataflow.passes), Y = count, ONE surface per FACET as small-multiples (op-class facet AND operand-width facet — the class/width becomes the facet, not the depth axis). A one-shot / few-pass recording degrades to a 2D ImPlot ridgeline / streamgraph. (Consolidates the reshaped Operation-Mix Ridge Stack and Value-Representation Terrain, whose only load-bearing 3D form is the pass axis.)
- **Built from.** `TraceStream.disasm / DataflowStream.disasm + shared mnemonic_class map (from Opcode-class terrain)` · `ValRec.size/space/wide/bytes/value_valid (operand-width facet)` · `SegmentedDataflow.passes` · `NEW class/width filter facet on Selection/timeline_build`
- **Drill-in.** Pick (facet, window, pass) -> class/width-filtered timeline at that step range (needs the new filter facet; degrade to brushing one representative step until it exists).
- **Honesty.** A single-pass recording is ONE flat 2D chart honestly (3D buys only occlusion — this is fundamentally a reading task, promoted to 3D only for genuine cross-pass evolution); empty / no-df_step windows feed a dedicated UNKNOWN lane (never redistributed, never zero); a survey-only recording renders NOTHING + 'no exact operation stream (statistical capture)' banner (SurveyEdge carries only from/to/count/mispred — never synthesize a mix from CFG edges); torn windows hatch every surface with insns_total-vs-shown; wide-with-empty-bytes = present-but-value-unknown, never zero.

---

## 7. Build roadmap

Cheapest-highest-value first. Phase 0 is §4.

### Phase 1 — no-data-prereq high-value layers (cheapest, highest value)

*Every item here rides the existing plane and pre-computed terrain, so they compose immediately and stress-test the SceneLayers toggle model with the highest-value overview questions (trust, module heat, mispredictions, work-kind, taint spread, kernel crossings).*

- Confidence terrain (+ coverage-window mask) — reuses existing TF_STAT/TORN/CHURN, defines the honesty axis literally, zero data prereq
- Per-module residency skyline — reuses full_heat_by_cell, zero prereq
- Misprediction survey layer (arcs + site columns) — reuses HotEdge, only the SurveyEdge/HotEdge sourcing decision
- Opcode-class code terrain — after the classifier factor from Phase 0
- Taint isochrone — after the BFS-depth helper
- Crossing spurs on the worldline — after Event::seq carry-through

### Phase 2 — memory data-cell family (after the observed-data-span projection)

*All gated on the Phase-0 observed-data-span projection + cum_read/write split; landing them together amortizes that one extension across four high/medium-value memory graphs.*

- Read/write twin relief (foundational for the family)
- Working-set tide
- Observed-lifetime pillars
- Data-access worldline ribbon
- Residency sediment columns (no prereq, can slot earlier if capacity allows)

### Phase 3 — high-value medium scenes

*Standalone scenes whose data already exists and whose effort is medium; each answers a question the plane structurally cannot host (divergence, cross-call variation, per-thread library residency, intra-register manipulation).*

- Divergence worldline — reuses dt_divergence/dt_statediff, huge A/B-comparison payoff
- Invocation stack — after the dt_link invocation field
- Module excursion ribbon — after Event::seq
- SIMD lane prism — reuses decoded-but-unused ValRec.wide/bytes

### Phase 4 — large scenes and the framework capstone

*These carry real geometry/analysis cost (articulation cuts, ensemble projections, kernel-wall meshes, honesty-constrained loop nesting) and are best built once the substrate and honesty primitives are proven by earlier phases.*

- Exploded evidence stack (SceneLayers explode — demonstrates the additive framework)
- The boundary curtain (new time x tid kernel domain)
- Module interaction braid (coupling hubs)
- Cone frustum (causal cone + articulation bottleneck)
- Value-ribbon braid (value river on the loom)
- Ensemble stability terrain (cross-recording mean/variance)
- Loop helix nest (loop iteration structure)

### Phase 5 — remaining medium layers and the pass-axis scene

*Solid medium-value additions that depend on either niche data (multi-sink blame, multi-version codeimage) or a new selection facet; fold in as capacity allows without blocking the high-value spine.*

- JIT churn strata
- Blame convergence forest
- Dominant-path ridge
- Computation character over invocation passes (needs the Selection/timeline class/width filter facet)

---

## 8. Open questions & known limits

The genuine unknowns — missing producer fields, occlusion/perf risks, and the
places 2D still wins (recorded here so they are not re-litigated).

- OBSERVED-DATA-SPAN PROJECTION is the single largest unknown: no producer emits data-region kinds (no malloc/free/mmap/brk in recording.cpp's kind list), so data cells must be clustered from mem/ValRec addresses as kind=Unknown spans. This is buildable but is a hard blocker for the entire memory data-cell family (tide, twin relief, pillars, data-access ribbon) — its clustering quality (how to bin sparse addresses into compacted spans without fabricating structure) needs validation.
- SIMD element width is genuinely NOT recorded — ValRec.size is total operand width, so pshufb (byte) vs paddd (dword) is indistinguishable from data; the prism must default to 16 byte-lanes and only sub-divide when the disasm mnemonic is unambiguous. Confirm the disasm text is reliably present for the vector mnemonics of interest.
- def-use edges (dt_edge/edge_loc) resolve only to a whole location, never a byte or vector lane — so no graph can draw element->element permutation threads; the prism reveals shuffles only through value-hash colour continuity. Worth confirming no finer-grained edge is ever emitted.
- the survey WINDOW provenance field (have_window/window_base/window_len) is optional and appears rarely emitted (no clear emitter in asmspy.c) — the coverage-window mask only earns its distinctive 'never-looked vs below-rate' axis when a window is stated; otherwise it correctly degrades to the existing statistical layer. Does any current backend actually emit it?
- SurveyEdge (streams.h) carries only {from,to,count,mispred} and drops is_return/from_addr — the misprediction layer and module braid must source from HotEdge or the field must be added to SurveyEdge; pick one before building both.
- OCCLUSION / PERF: several graphs stack many translucent primitives on the same footprint (exploded stack sheets, residency sediment bands, JIT strata, ensemble support layers) — needs depth-sorting, per-layer toggles, and possibly a cell-count budget so a dense recording does not become an unreadable haze or a frame-rate cliff. The ensemble terrain over large N is the worst case.
- the greedy-reconstructor trap (documented block-step ambiguity) constrains two graphs: loop helix nesting must fall back to sibling coils under irreducible/shared-header flow, and dominant-path ridge must be framed as a per-fork aggregate, never an observed path. These honesty constraints reduce their gestalt payoff — verify the constrained forms still answer their overview question.
- code_sha proves routine BYTES match but NOT that per-run Projections are identical (each run compacts its own data/stack) — the ensemble terrain must build a canonical code-only projection keyed by len; confirm code regions genuinely align across ASLR under that keying.
- WHERE 2D STILL WINS (reshaped out of the 3D catalog): SP-depth stack canyon, per-register pressure/liveness, syscall class-mix streamgraph, and FD lifelines were all demoted to 2D ImPlot views — their real job is precise per-item reading and the third dimension bought only occlusion. Only the invocation-PASS axis salvaged a genuine 3D form (the computation-character scene). These 2D views are still worth building, just not here.
- regsynth register-file enrichment (used to corroborate liveness/value) is full-app-only (regsynth.h links the engine), NOT engine-free, so it cannot enter the render-only scene TU — any graph relying on it must degrade to a labelled re-derivation from ValRec in the viewer build.
- the drill-in facet gap: Selection carries one {step,off} and timeline_build takes only an optional cone, so class/width-filtered drill-ins (computation character) and any 'this class's steps in this window' route need a NEW filter facet; until it lands those picks degrade to one representative step.

---

## 9. Provenance

Generated 2026-07-29 by a 17-agent grounded-ideation workflow (8 domain ideators
→ adversarial buildability+honesty critique → synthesis) under
[CLAUDE.md](../../../CLAUDE.md)'s ultracode mode. Extends the existing 3D design
([docs/internal/gui/10-spacetime-3d-overview.md](../gui/10-spacetime-3d-overview.md),
[34-playhead-and-scene-reach.md](../gui/34-playhead-and-scene-reach.md)) and the
Wave-1 `mem[]` address stream ([29-mem-address-stream.md](../gui/29-mem-address-stream.md)).
Sibling analysis docs live in [`docs/internal/analysis/`](.).
