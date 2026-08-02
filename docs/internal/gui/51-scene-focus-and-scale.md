# Focus and scale — subject filtering, thread ghosting, and an entity budget

> **Sources.** Gaps G11–G12 of [46-3d-functional-roadmap.md](46-3d-functional-roadmap.md),
> which cuts this brief. Upstream: the
> [3D catalog](../analysis/2026-07-29-3d-visualization-catalog.md) §9 ("OCCLUSION /
> PERF: several graphs stack many translucent primitives on the same footprint …
> needs depth-sorting, per-layer toggles, and possibly a cell-count budget so a
> dense recording does not become an unreadable haze or a frame-rate cliff") and
> the [city doc](../analysis/2026-07-30-computer-as-city-3d.md) §4 "LOD / scale"
> (the `kCityEntityBudget` with three labelled camera-distance tiers) and §3
> "Multi-City, Scale & Liveness" (`Line.tid` so `render()` can ghost non-selected
> paths). Read [_conventions.md](../implementations/_conventions.md) first; D1–D11
> live in this directory's [README](README.md).
>
> **Prerequisites: none hard.** T4 frames its LOD tiers on
> [48](48-scene-navigation-and-goto.md)'s landmark anchor when present and on the
> plane centre otherwise. No producer change, no schema change, no new dep.
>
> Authored 2026-08-02 against HEAD `f110150`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Status — ☐ 0/4, not started.**

## Why this work exists

Two different scale problems share one root: the scene has no notion of a
*subject*, so it can only draw everything at equal weight.

**Focus.** `SceneLayers` is nine bools ([scene.h:36-47](../../../desktop/src/scene3d/scene.h#L36))
and every one of them is a *class* switch — terrain, exact, statistical, access
marks, convergence, zoning, weather, ghost fog, vehicle. There is no way to say
"this thread", "this region", "this kind". On a multi-thread capture every path is
drawn identically and the one you care about is somewhere in the bundle. The
renderer cannot even express the distinction: `Scene::Line`
([scene.h:171-185](../../../desktop/src/scene3d/scene.h#L171)) retains a colour, a
statistical flag and per-vertex facts, but **no `tid`** — so `render()` could not
ghost the other threads if it wanted to.

**Scale.** The existing degrade path is time-budgeted, not density-budgeted:
`kScrubCellBudget` / `should_degrade` / `coarse_slice`
([shell.cpp:1001-1012](../../../desktop/src/ui/shell.cpp#L1001)) exist to keep a
slow *slice* off the UI thread and are a good, proven idiom. Nothing handles a
dense *frame*. A Hilbert order-12 plane is 4096×4096 ≈ 16.7M cells; both source
docs name occlusion at scale as the top rendering risk and neither proposes a
mechanism below city Phase C/D. The cheap half of that mechanism does not need the
building system at all.

The two are one brief because they are one lever: knowing what the user is
interested in is what lets you drop the rest without lying about it.

## What already exists (verified 2026-08-02 against `f110150`)

- **The degrade idiom is proven and labelled.** `coarse_slice()`
  ([terrain.h:137-144](../../../desktop/src/space/terrain.h#L137)) is a flat plane
  that *carries the torn flag* and is announced by `scrub_degrade_note()`
  ([shell.cpp:1013-1014](../../../desktop/src/ui/shell.cpp#L1013)) — degrading
  hides nothing because the coarse rung is the same labelled rung the terrain
  shows normally. T4 generalises this shape; it does not invent a new one.
- **One `Line` per trajectory already exists**, one per tid, built in
  `set_trajectories` ([scene.cpp](../../../desktop/src/scene3d/scene.cpp)) — the
  per-thread split is already in the geometry, only the identity is missing.
- **`Trajectory::tid` and `TrajPoint::tid`** are carried end to end
  ([trajectory.h:48-52](../../../desktop/src/space/trajectory.h#L48),
  [types.h:60](../../../desktop/src/space/types.h#L60)), so T1 is plumbing an
  existing fact into the renderer, not deriving a new one.
- **`Projection.regions` is the region list**, sorted and non-overlapping, with
  `label` and `kind` ([types.h:30-40](../../../desktop/src/space/types.h#L30)) —
  the subject list a region filter needs.
- **`kind_by_cell`** ([terrain.h:87](../../../desktop/src/space/terrain.h#L87)) is
  already uploaded as an R8UI texture by 44's `set_zoning`
  ([scene.h:78-79](../../../desktop/src/scene3d/scene.h#L78)) — a kind filter is a
  shader comparison against a texture that is already bound, costing nothing.
- **`Camera::radius`** with its `kMinRadius`/`kMaxRadius` clamps
  ([camera.h:40-41](../../../desktop/src/scene3d/camera.h#L40)) is the natural LOD
  key and is already pure and tested.

## Tasks

### T1 — `Scene::Line` learns its tid; ghost the rest (M)

**Goal.** One thread can be brought forward without the others being hidden.

**Steps.**
1. `scene3d/scene.h`: add `int32_t tid = -1;` to `Scene::Line`
   ([scene.h:171](../../../desktop/src/scene3d/scene.h#L171)) and populate it in
   `set_trajectories` from `Trajectory::tid`.
2. Add a public `int32_t focus_tid = -1;` (draw-time, no upload, matching the
   `follow_step` convention) and, in the trajectory draw loop
   ([scene.cpp:670-690](../../../desktop/src/scene3d/scene.cpp#L670)), scale
   `uColor.a` down for lines whose tid differs — **ghost, never hide**. A hidden
   path claims the thread did not run; a ghosted one says "still there, not the
   subject". Same reasoning as [49](49-one-time-truth-in-the-scene.md)'s
   dim-not-discard.
3. The statistical residency trajectory is **never** promoted to a focused thread:
   a survey is an aggregate, not a per-thread path
   ([converge.h](../../../desktop/src/space/converge.h)'s S3 admission rule states
   this for convergence and the same reasoning applies here). Focusing a tid
   leaves the statistical line at its normal weight and says so in the HUD.
4. `scene3d/hud.cpp`: a thread roster — one row per exact trajectory, showing tid,
   its swatch, and its placed-vertex count — selecting one sets `focus_tid`. A tid
   present in the recording's topology but with no placed path gets a row reading
   "present, no placed path", never omission.

**Tests.** `test_scene`-side or the FBO smoke: with `focus_tid` set, the focused
line's alpha is unchanged and others are reduced; with `focus_tid = -1` every
alpha is byte-identical to today. Pure half: the roster's row list is a function
of `TrajectorySet` — golden-test it, including the no-placed-path row.

**Done when.** A thread can be focused; no thread can be made invisible; the
statistical layer is never focusable.

### T2 — Region and kind filters on the plane (M)

**Goal.** "Show me only the heap", "only this library", without pretending the
rest is empty.

**Steps.**
1. `scene3d/scene.h`: `uint32_t kind_mask = 0xFFu;` and `int32_t focus_region =
   -1;` (draw-time values).
2. `kTerrainFrag`: with a kind excluded by the mask, desaturate and flatten
   *brightness* — **never the height**. Height is the encoded quantity; changing it
   changes the data. Desaturation is a presentation channel and is safe. The
   `uKind` texture is already bound (44-T1), so this is a comparison, not an
   upload.
3. `focus_region` desaturates every cell outside that region's footprint. Resolve
   membership from the same `domain_off[i]..domain_off[i+1]` walk
   [48](48-scene-navigation-and-goto.md)'s region framing uses — one rule for
   "which cells are this region's", not two.
4. `scene3d/hud.cpp`: kind checkboxes reusing `region_style()`'s labels and
   colours ([projection.h](../../../desktop/src/space/projection.h)) so the filter
   UI and the plane share one palette; a region combo listing `proj.regions` by
   verbatim label.
5. **The HUD states the filter is on and what it hides.** A filtered plane that
   looks like an unfiltered one is a false reading of the recording — the chip
   says "showing 2 of 6 kinds" in `dt_warn_col()`, in the same spirit as
   `scrub_degrade_note()`.

**Tests.** The pure half is the membership rule: a helper returning a region's
cell set, asserted to contain every cell `project` places in that region and no
cell of a neighbour. The shader half rides the existing FBO smoke if it runs; say
so in the test file if it self-skips.

**Done when.** Filters change only presentation channels, never heights or flags,
and an active filter is always announced.

### T3 — Filters compose with, and never launder, fidelity (S)

**Goal.** Subject filtering cannot be mistaken for fidelity filtering.

**Steps.**
1. Audit every new filter against the four invariants: a desaturated statistical
   cell is still stippled and still statistical; a filtered-out torn cell keeps
   `TF_TORN` in the model and reappears with its rubble intact when unfiltered; a
   `TF_UNKNOWN` cell is never made to look like a filtered-out described cell —
   those are different states and must stay visually distinct at every filter
   setting.
2. Make the ordering explicit in `kTerrainFrag`: subject desaturation applies
   **before** the `TF_*` fidelity branches, so fidelity always wins the last word
   on a pixel. Comment it as load-bearing, because a later edit that reorders the
   branches would silently launder a torn cell into a filtered one.
3. HUD wording distinguishes the two axes in one line: layers grade *evidence*,
   filters choose *subject*.

**Tests.** For each `TerrainFlag`, assert the flag survives a filter round trip in
the model (filters are draw-time only and must not touch `Terrain::flags` at all
— the strongest form of this assertion is that no filter code path writes to the
model, which a review plus a test on `flags` byte-equality can both cover).

**Done when.** No filter setting can make a fidelity state unreadable or make two
different fidelity states look alike.

### T4 — A camera-distance entity budget (M)

**Goal.** A dense recording degrades legibly and audibly rather than becoming a
haze or a frame cliff.

**Steps.**
1. Generalise the proven `kScrubCellBudget` / `should_degrade` / `coarse_slice`
   idiom into a **distance-keyed** tier selection over `Camera::radius`, with
   three labelled tiers:
   - **NEAR** — the current full per-cell terrain plus all overlays;
   - **MID** — terrain plus focused/high-weight overlays; low-weight overlay
     classes (access spurs first, then non-focused trajectories) drop out;
   - **FAR** — the existing `top_down()` flat reading of the plane.
   The tier is a **pure function of `(radius, entity count)`** — put it in
   `camera.h` or a small pure helper beside it so `test_camera` covers the
   thresholds directly, and so the tier can be asserted with no GL.
2. **Every drop is announced.** A tier that omits a class shows the same kind of
   placard `scrub_degrade_note()` already provides, naming what is not being
   drawn. Silent truncation reads as "there was nothing there" — the exact failure
   this repo's degrade idiom was built to avoid.
3. **Provenance survives LOD.** A decimated *exact* overlay is still exact and
   must be labelled "exact (LOD)", never allowed to look like the statistical
   stipple. This is the city doc's own explicit warning about the LOD crowd band
   and it is easy to get wrong: two ways of drawing less must not converge on one
   appearance.
4. Do **not** build a geometry LOD (merged meshes, aggregate towers) here — that
   is [43](43-faithful-city-roadmap.md) Phase D's instanced building system. This
   task drops and dims existing primitives only.

**Tests.** `test_camera.cpp`: tier boundaries are exact and monotonic in radius
(dollying out never re-adds a class); an entity count under budget always yields
NEAR regardless of radius. A golden on the placard text for each tier, so a silent
drop fails a test.

**Done when.** The tier is a pure, tested function; every omission is named on
screen; no exact data is ever presented in the statistical channel.

## Fidelity notes (D7)

- **Ghost, never hide.** Both filtering mechanisms reduce salience; neither
  removes a mark. Absence in this app means "not recorded", and the filters must
  not be able to say that on the recording's behalf.
- **Filters are draw-time only.** No filter writes to `TerrainModel`,
  `Terrain::flags` or `TrajectorySet`. The model stays the source of truth so a
  golden text and a filtered render can never disagree.
- **Presentation channels only.** Desaturation and alpha are safe; height, flags
  and geometry encode data and are untouchable.
- **Fidelity wins the last branch.** Subject filtering applies before the `TF_*`
  branches so a torn cell reads as torn at every filter setting.
- **Degradation is announced.** Inherited verbatim from the existing scrub
  degrade: the labelled coarse rung hides nothing precisely *because* it says it
  is coarse.
- **The statistical layer is never a subject.** A survey is an aggregate; focusing
  it as though it were a thread would promote sampled residency to a named
  citizen.

## Effort and risk

Four tasks, three medium and one small; draw-side only. The real risk is scope
creep toward Phase D — T4 step 4 draws that line explicitly. The second risk is
the appearance collision in T4 step 3 (decimated-exact vs. statistical), which is
why it is called out as a test, not a guideline.
