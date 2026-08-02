# The fidelity and module layers — trust, module heat, work-kind, mispredictions

> **Sources.** The [3D catalog](../../analysis/2026-07-29-3d-visualization-catalog.md)
> §5 layers 1–4 (confidence terrain, per-module residency skyline, opcode-class
> code terrain, misprediction survey layer) and its §7 Phase 1; cut by
> [53-3d-catalog-build-roadmap.md](53-3d-catalog-build-roadmap.md) §4.1 (L1–L4).
> Read [_conventions.md](../implementations/_conventions.md) first; D1–D11 live in
> this directory's [README](README.md).
>
> **Prerequisites.** T1–T3 have none. T4 needs
> [54](54-3d-catalog-phase0-plumbing.md) **T4** (`mnemonic_class`); T5 needs
> [54](54-3d-catalog-phase0-plumbing.md) **T7** (the `HotEdge` sourcing decision).
> Nothing here needs a producer change, a schema change or the memory family.
>
> Authored 2026-08-02 against HEAD `b657876`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Status — ✅ 5/5, landed 2026-08-02** (T1 `dc3f918`; T2 `028aea3`; T3
> `ce9475e`; T4 `d9cc9a1`; T5 `cb09c1e`). T1: `scene3d/layers.{h,cpp}` — a pure
> `LayerDesc` table (id/label/question/grade/flag) covering every SceneLayers
> bool, grouped by fidelity/structure/activity/survey; hud.cpp's hand-listed
> checkbox block now builds from it (pointer-to-member, keyed by id, never
> position); `test_layers.cpp` pins exhaustiveness. T2: `kTerrainFrag` gains a
> `uConfidence`-gated re-tint (default off) plus two new `TerrainFlag` bits
> (`TF_INWINDOW_EMPTY`/`TF_OUTWINDOW`) set by `views/hotedges.cpp`'s
> `apply_coverage_window` (a no-op unless a window was actually stated).
> Scoped deviation: height stays density-driven (no geometric re-lift, noted
> in the shader), and drill-in keeps the existing canvas routing for an
> unknown cell rather than `dt_view::region` — that view is the region-
> capture-mode invocation pager (`views/region.h`), not a generic per-address
> lookup, so routing there would open a mismatched pane. T3:
> `space/canopy.{h,cpp}`'s `build_module_canopies` sums each region's t-gated
> hit counts (raw, summed BEFORE log-scaling — the anti-regression bar) into
> one exact canopy per region plus a separate statistical one wherever the
> survey layer has residency; rendered as translucent bounding-box quads
> (plain alpha blend, a scoped simplification vs. 55's dithered-discard
> idiom). T4: `space/opcode_terrain.{h,cpp}` classifies each cell's DISTINCT
> offsets via `mnemonic_class` over the recording's own recorded disasm text
> (D10, no re-disassembly); tints via a new per-cell `tex_opclass_` R8UI
> through `kTerrainFrag`'s `uOpcode` toggle (purity/runner-up ticks not
> rendered, a scoped simplification). T5: `space/mispred.h` (split from
> `views/hotedges.h` so scene3d/ keeps depending on space/ only, D4) plus
> `build_mispred_layer` project each hot edge onto the plane — an unplaceable
> endpoint is counted (`off_plane`), never dropped; rendered by reusing
> `prog_traj_` (no new shader) as bezier arcs + sheath/core site columns.
> Deferred in both T3 and T5, stated rather than silently skipped:
> interactive pick/drill-in for the new canopy/arc/column geometry needs a
> new `PickBands` band, not implemented — clicking currently falls through
> to whatever is beneath. `docker-desktop`'s full chain (desktop +
> desktop-render + desktop-test, desktop-engine-boundary-check,
> desktop-ui-test 28/28, desktop-test-xvfb) reproduced green on this host;
> `test_scene_fbo`'s one pre-existing "contour bands" GL failure (doc 55 T3,
> a software-renderer fixture sensitivity) was confirmed unrelated by
> reproducing it on a clean checkout with none of this doc's commits
> applied.

## Why this work exists

These four layers are the catalog's highest-value items *and* the ones with the
least new data behind them: every quantity they draw is already computed, decoded
and tested. The confidence terrain re-lifts cells by flags that
`TerrainModel::slice()` already sets. The module skyline sums a `full_heat` field
that already exists. The misprediction layer reads a `HotEdge` set the hotedges
view already ranks. Only the opcode terrain needs anything new, and that is a word
list moved into a different translation unit.

They also do something the rest of the family depends on: **they are the first
four layers to coexist**, which is what turns `SceneLayers` from a list of bools
into a system. Doing that once, here, is T1.

The four answer four different overview questions on the same registered cells:
*how much do I trust this?* (T2) · *which library is hot as a whole?* (T3) ·
*what kind of work happens here?* (T4) · *where does the predictor struggle?* (T5).

## What already exists (verified 2026-08-02 against `b657876`)

- **`SceneLayers` is nine bools**, all class-wide, with the four Phase-A additions
  documented as defaulting true ([scene.h:36-47](../../../desktop/src/scene3d/scene.h#L36)).
  It is passed by value into `Scene::render`
  ([scene.h:121](../../../desktop/src/scene3d/scene.h#L121)) and lives on
  `HudState` ([hud.h:56](../../../desktop/src/scene3d/hud.h#L56)).
- **Every flag T2 needs is set and tested.** `TF_TORN`, `TF_STAT`, `TF_CHURN`,
  `TF_READ`, `TF_WRITE`, `TF_UNKNOWN`
  ([terrain.h:49-62](../../../desktop/src/space/terrain.h#L49)); `TF_UNKNOWN` is
  set **per-slice** in `slice()`, deliberately not in `build_terrain`
  ([terrain.h:55-61](../../../desktop/src/space/terrain.h#L55)), so it already
  reclassifies as the playhead moves — which is exactly T2's requirement.
- **The separate statistical terrain exists as its own object.**
  `TerrainModel::has_stat` + `TerrainModel::stat`
  ([terrain.h:107-108](../../../desktop/src/space/terrain.h#L107)), uploaded to its
  own texture pair by `set_stat_terrain` and drawn by a separate program
  ([scene.h:87-90](../../../desktop/src/scene3d/scene.h#L87),
  [scene.cpp:633-640](../../../desktop/src/scene3d/scene.cpp#L633)). The
  never-merge invariant is already geometry, not a convention.
- **`CodeCell::full_heat`** is *"the canvas per-offset heat summed over the offsets
  in this cell"* ([terrain.h:117-119](../../../desktop/src/space/terrain.h#L117)) —
  raw, not log-scaled. `Terrain::height` is the log-scaled one
  ([types.h:71-75](../../../desktop/src/space/types.h#L71)). T3 turns on this
  distinction.
- **`Projection::regions`** are sorted, non-overlapping and carry
  `{base, len, kind, label, version}` ([types.h:19-24](../../../desktop/src/space/types.h#L19)),
  with `domain_off` giving each region's compacted extent
  ([types.h:40](../../../desktop/src/space/types.h#L40)) — so a region's footprint
  cells are derivable without a scan.
- **`region_style()`** is the single source of region hue + legend name
  ([projection.h:65-69](../../../desktop/src/space/projection.h#L65)), duplicated
  deliberately into the shader as `kindHue`
  ([embedded.h:54-61](../../../desktop/src/scene3d/shaders/embedded.h#L54)).
- **`HotEdgeView`** carries the whole fidelity channel a statistical layer must
  render: `sampler`, `samples`, `lost`, `throttled`, `have_window`,
  `window_base/window_len` ([hotedges.h:50-64](../../../desktop/src/views/hotedges.h#L50)).
- **The HUD is pure ImGui and drawn even under the null backend**
  ([shell.cpp:935-941](../../../desktop/src/ui/shell.cpp#L935)) — so every legend
  and chip these layers add is headlessly testable, with no GL.

## Tasks

### T1 — A layer registry: from nine bools to a system (M)

**Goal.** Adding a layer stops meaning "edit five files and hope the HUD, the
renderer and the legend agree".

**Steps.**
1. `scene3d/scene.h`: keep `SceneLayers` as the wire between shell and scene
   (it is a POD carried on `SceneFrame`,
   [scene_host.h:46](../../../desktop/src/ui/scene_host.h#L46), and widening it is
   the smallest change), but add a **pure descriptor table** beside it in a new
   engine-free TU:
   ```
   struct LayerDesc {
       const char *id;        // stable key, used by persistence and tests
       const char *label;     // HUD text
       const char *question;  // the overview question it answers, one line
       enum Grade { Exact, Statistical, Derived } grade;
       bool SceneLayers::*flag;   // the bool it drives
   };
   const std::vector<LayerDesc> &scene_layers_all();
   ```
2. `scene3d/hud.cpp`: build the toggle list and the legend **from the table**, so a
   layer added without a legend entry is impossible rather than merely discouraged.
   This is the fix for the review's #36 (unexplained marks) generalised, and it
   subsumes [47](47-scene-inspect-and-pickable-overlays.md) T5's HUD row — whichever
   lands second uses the table.
3. `grade` drives the shared chrome: a `Statistical` layer's toggle carries the
   `STATISTICAL — survey` label in the warn colour automatically, from
   `ui/theme.h`'s existing palette, so no layer re-invents the phrasing (D7 /
   [24](../archive/gui/24-one-visual-language.md)).
4. **Group the toggles.** Nine is a list; fifteen is a wall. Group by the question
   asked — *fidelity* / *structure* / *activity* / *survey* — and collapse the
   groups. The HUD is already a separate window, so it has the room.
5. Persist the set per recording, alongside the existing `SceneView` state
   ([shell.h](../../../desktop/src/ui/shell.h)), keyed by the stable `id` string so
   a reordered table does not shuffle a user's saved toggles.

**Tests.** `test_shell.cpp` / the HUD test TU: every `LayerDesc` has a non-empty
label, question and legend entry; every `SceneLayers` member appears in exactly one
descriptor (the exhaustiveness assertion — this is what makes the table load-bearing);
a `Statistical`-graded layer's toggle text contains the shared statistical wording;
toggles round-trip through persistence by id.

**Done when.** No layer can be added without a legend entry and a stated question,
and the assertion that enforces it is a test rather than a review comment.

### T2 — Confidence terrain and the coverage-window mask (M)

**Goal.** The plane answers "how much do I trust each region right now" directly,
instead of leaving it to be inferred from chips.

**Steps.**
1. **Re-lift by fidelity class, not density.** A second terrain program (or a
   uniform-switched branch in `kTerrainFrag`) whose height and ink come from
   `Terrain::flags` rather than `height`:
   - exact code/data → solid, opaque, at a class height;
   - `TF_STAT` → stippled + translucent, drawn from the **separate `stat`
     terrain** that already exists, never from the exact one;
   - `TF_TORN` → frayed/hatched upper edge, the *rubble* idiom Phase A already
     established ([embedded.h:76-77](../../../desktop/src/scene3d/shaders/embedded.h#L76));
   - `TF_UNKNOWN` → a **sunken** hatched floor. Phase A already renders this as a
     dark mix ([embedded.h:74-75](../../../desktop/src/scene3d/shaders/embedded.h#L74));
     this layer is where it becomes a *depth*, so unknown is structurally below
     zero rather than merely dark.
   - `TF_CHURN` keeps its tick marks.
2. **The coverage-window mask, only when the window is stated.** `HotEdgeView`
   carries `have_window`/`window_base`/`window_len`
   ([hotedges.h:59-60](../../../desktop/src/views/hotedges.h#L59)). When present:
   in-window-and-credited = mound; in-window-and-empty = a cross-hatch labelled
   **"below-rate — unknown, not cold"**; out-of-window = a dark *never-looked*
   mask. When `have_window` is false, **draw no mask at all** and say
   "whole-process assumed" — the catalog's §9 notes the field appears rarely
   emitted, so the no-window path is the common one and must be the graceful one.
3. It rides the existing terrain-time axis: cells reclassify as `hud.t` moves,
   because `TF_UNKNOWN` is already per-slice.
4. **Drill-in by class**, through `resolve_pick`'s existing per-kind router
   ([pick.cpp](../../../desktop/src/scene3d/pick.cpp)): exact code → canvas (disasm
   when churned), exact data → slice, statistical-only → hotedges with the full
   sampler/lost/throttled chrome, unknown → the region view saying *never described
   here*. A torn cell carries its truncation banner into whichever view opens.

**Fidelity.** This layer *is* the fidelity axis, so it has the least room to be
loose: unknown is a pit and never a zero height; statistical is never merged into
exact (it is literally a different GL object); torn is loud and survives the
drill-in; and an absent window fabricates no mask.

**Tests.** `test_terrain.cpp`: a fixture with one cell of each class produces the
expected flag set at several `t`, including a cell that is unknown at `t=0` and
exact later. `test_scene_fbo.cpp`: the unknown cell renders below the plane, the
statistical cell renders from the `stat` textures, and the exact cell's pixels
never mix the statistical hue. `test_drillin.cpp`: one case per class asserting
the routed view.

**Done when.** Every fidelity class is visually and structurally distinct on the
plane, and the window mask appears only when a window was stated.

### T3 — Per-module residency skyline (M)

**Goal.** "Which library is hot as a whole" — a question the per-cell terrain
structurally cannot answer, because a viewer cannot sum a height field by eye.

**Steps.**
1. New pure builder in `space/` (engine-free, D4):
   ```
   struct ModuleCanopy {
       size_t region;        // index into Projection::regions
       double raw_heat;      // SUM of CodeCell::full_heat over the footprint
       double height;        // log-scaled, AFTER the sum
       uint32_t cells_mapped, cells_hit;
       bool torn, statistical;
   };
   std::vector<ModuleCanopy> build_module_canopies(const space::TerrainModel &,
                                                   uint64_t t);
   ```
2. **Aggregate raw, then log — in that order.** `CodeCell::full_heat` is the raw
   count ([terrain.h:117-119](../../../desktop/src/space/terrain.h#L117));
   `Terrain::height` is already log-scaled ([types.h:73](../../../desktop/src/space/types.h#L73)).
   Summing heights would sum logarithms, which is a product, which is not a heat.
   This is the single arithmetic mistake this layer can make and the test must pin
   it.
3. Render one translucent canopy per region at that height, hued by
   `region_style(kind)` and labelled. **No smoothing across region boundaries** —
   a canopy is a step function because modules are.
4. **A region with cells mapped and zero heat is a wire outline**, not a
   zero-height slab: mapped-but-cold and never-mapped are different facts. Track
   both counts so the readout can say which.
5. Torn cells within a region hatch its canopy; survey (`TF_STAT`) residency
   raises a **physically separate, stippled, offset** canopy that is never summed
   into the exact one. The fine per-cell terrain still shows underneath — the
   canopy is translucent and additive, not a replacement.
6. Drill-in: exact canopy → the region view or the tree filtered to that module;
   statistical canopy → hotedges.

**Fidelity.** Aggregate raw then log; a mapped-but-cold region is an outline;
the survey canopy is a separate object; a torn module reads as hatched and stays
hatched through the drill-in.

**Tests.** `test_terrain.cpp` (or a new `test_canopy.cpp`): for a fixture with two
regions of known per-cell heats, `raw_heat` equals the arithmetic sum and `height`
equals `log(sum)` — assert explicitly that it does **not** equal the sum of the
per-cell `Terrain::height` values, which is the anti-regression test for step 2. A
region with mapped cells and no hits has `cells_mapped > 0, cells_hit == 0` and is
flagged for outline rendering. `test_scene_fbo.cpp`: the statistical canopy is a
separate draw and its pixels never blend into the exact canopy's.

**Done when.** Module-level heat is legible at a glance and the log/sum order is
pinned by a test.

### T4 — Opcode-class code terrain (M) · *needs [54](54-3d-catalog-phase0-plumbing.md) T4*

**Goal.** A spatial map of *what kind of work* each code region does — move,
int-arith, logic, compare-branch, scalar-float, vector-SIMD, system — rather than a
linear disassembly scroll.

**Steps.**
1. Per `Code`-kind cell, bucket the offsets that land in it, take each offset's
   first disassembly token, and classify with `mnemonic_class(token, guest)` from
   [54](54-3d-catalog-phase0-plumbing.md) T4. The text is already there:
   `TraceStream::disasm` is a per-offset map
   ([streams.h:51](../../../desktop/src/doc/streams.h#L51)) and
   `DataflowStream::disasm` is per-step ([streams.h:87](../../../desktop/src/doc/streams.h#L87)),
   both recorded at capture time under D10.
2. Tint by the **dominant** class; saturation = purity (dominant count / total);
   plus a small stack of two or three runner-up ticks, so a genuinely mixed cell
   reads as mixed rather than as a confident wrong answer.
3. **Only `Code` regions are painted.** Data cells carry no opcode class and must
   not be tinted by proximity.
4. **Memory-touch is never derived from the mnemonic.** If the layer wants a
   "touches memory" channel it comes from `ValRec::space` being `"abs"`/`"off"`
   ([streams.h:33](../../../desktop/src/doc/streams.h#L33)) — the header of
   [54](54-3d-catalog-phase0-plumbing.md) T4 says this and this is the consumer it
   was written for.
5. Drill-in: pick → `Selection.set(off = the cell's hottest offset)` → the disasm
   view. A cell spans an address range; land on the representative offset and let
   the reader scroll rather than inventing a range selection.

**Fidelity.** An empty `disasm` string or a token in no list is **Unknown** —
hatched/neutral, never coerced to "move" and never a zero. Survey-only cells carry
no class at all (they stay `TF_STAT`); torn stays torn; churned stays churned. The
guest gate is real: x86 and arm64 vocabularies overlap and a classifier run against
the wrong guest is confidently wrong. Ambiguous mnemonics are flagged, not
bucketed.

**Tests.** A pure builder test: a cell of all-`mov` offsets is pure Move at
saturation 1.0; a 50/50 cell is dominant-with-low-purity and carries a runner-up
tick; a cell whose offsets have no recorded disasm is entirely Unknown; a cell
mixing an ambiguous mnemonic carries the ambiguity flag. `test_drillin.cpp`: the
pick lands on the hottest offset, not the first.

**Done when.** A code region's character is readable spatially and every
un-classifiable offset is visibly un-classified.

### T5 — Misprediction survey layer (M) · *needs [54](54-3d-catalog-phase0-plumbing.md) T7*

**Goal.** Where the branch predictor struggles — which edges and which sites —
drawn as a first-class **statistical** object.

**Steps.**
1. One toggle, `branch (statistical)`, carrying two idioms that are never summed
   with anything exact:
   - **Bias arcs.** One quadratic-bezier tube per `HotEdge` from
     `project(from_addr)` to `project(to_addr)`, apex height and radius from
     `log10(count)`, colour from `mispred/count` on a cool→amber ramp, `is_return`
     dashed. The `conv_arcs_` tessellation
     ([scene.h:188-189](../../../desktop/src/scene3d/scene.h#L188)) is the existing
     bezier-into-`GL_LINES` machinery — reuse it rather than writing a second.
   - **Site columns.** Per `from_addr` cell, an outer glassy sheath at
     `log(count)` with an inner solid core at `log(mispred)`, so the fill fraction
     *is* the misprediction rate; `is_return` gets a chevron cap.
2. **Provenance rides the whole layer**, from `HotEdgeView`: the
   `STATISTICAL — survey` label always; `sampler` graduating the opacity (`ibs-op`
   crisp, `sw-clock` washed); `lost`/`throttled` surfaced; `provenance_conflict`
   raising the existing banner.
3. **An endpoint the projection cannot place is counted, not dropped.**
   `Projection::project` returns false for an unmapped address
   ([types.h:44](../../../desktop/src/space/types.h#L44)) — show an "N off-plane"
   HUD chip. A silently dropped edge is a misprediction the user never learns
   about.
4. `mispred == 0` renders a genuinely cool, zero-height core inside a full sheath —
   a real measured low, deliberately distinct from a layer that is absent.
5. Drill-in: arc or column → the hotedges ranked table and src×dst matrix, scrolled
   to that from-address. **Only** to hotedges: a statistical mark must never open an
   exact reader, which is `resolve_pick`'s existing invariant
   ([pick.cpp:107-111](../../../desktop/src/scene3d/pick.cpp#L107)).
6. A derived latch curvature (a `to < from` edge within one region) is **labelled
   derived** — it is a rendering choice, not a recorded fact.

**Fidelity.** Never merged into the exact terrain or the cross-thread convergence
arcs; opacity graded by sampler; off-plane endpoints counted; `is_return` is
recorded data and used as such (which is why
[54](54-3d-catalog-phase0-plumbing.md) T7 chose `HotEdge`).

**Tests.** `test_obs_hotedges.cpp` / a new layer builder test: arc geometry is
deterministic for a fixed edge set; `mispred == 0` and "layer absent" produce
different geometry; an unplaceable endpoint increments the off-plane count and
emits no arc; the `sampler` string maps to the documented opacity.
`test_drillin.cpp`: an arc pick resolves to `hotedges`, never to canvas or slice.

**Done when.** The survey's branch behaviour is on the plane, is unmistakably
statistical, and loses no edge silently.

## Fidelity notes (D7)

- The **statistical / exact separation is already geometry** in this tree (a
  separate `Terrain`, a separate texture pair, a separate program). Every layer
  here extends that pattern rather than re-deciding it: T3's survey canopy and
  T5's whole layer are separate objects, not tinted variants.
- **Log-vs-raw is a fidelity question, not a styling one** (T3 step 2). Summing
  log heights understates a busy module by an amount that depends on how its heat
  is distributed — a plausible-looking wrong answer, which is the worst kind.
- **Unknown appears in three different guises here** and all three must stay
  distinguishable: T2's `TF_UNKNOWN` pit (in-domain, never described), T4's
  unclassifiable mnemonic (described, but not understood), and T3's mapped-but-cold
  region (understood, and genuinely zero). Only the last is a measured zero.
- T5's `HotEdge` source keeps the survey physically separate at the *model* layer,
  which is why [54](54-3d-catalog-phase0-plumbing.md) T7 exists.

## Effort and risk

Five medium tasks. Risks:

- **T1 is the one with a real design decision in it** and it is worth spending the
  time: every later brief in this family registers into it, and a registry that
  does not carry `grade` will be worked around rather than extended.
- **Four new layers on one plane is where occlusion starts.** T3's canopies and
  T5's arcs both sit *above* the terrain. Land [55](55-scene-render-quality.md) T1
  (EDL) and T4 step 1 (the depth-write fix) alongside this brief or the composed
  result will be less readable than the parts.
- **T4's classifier is guest-gated** and the guest string must come from the
  recording, not a default. A recording whose arch is unstated classifies as
  `Unknown` throughout — correct, and it must look deliberate rather than broken.
