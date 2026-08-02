# The flat surface — a GL-free 2D terrain the whole app can read

> **Sources.** Gap G13 of [46-3d-functional-roadmap.md](46-3d-functional-roadmap.md),
> which cuts this brief. Upstream: [UX/dataviz review](../analysis/2026-07-29-gui-ux-dataviz-review.md)
> #58 ("Provide a faithful GL-free 2D terrain fallback and a flat reading
> surface"), and the [3D catalog](../analysis/2026-07-29-3d-visualization-catalog.md)
> §9's "WHERE 2D STILL WINS" — the standing rule that the third dimension must earn
> itself, and that precise reading is a 2D job. Read
> [_conventions.md](../implementations/_conventions.md) first; D1–D11 live in this
> directory's [README](README.md).
>
> **Prerequisites: none.** The `Terrain` slice, the `Projection` and the
> `TrajectorySet` are all pure and already built by the pane before any GL is
> touched. No producer change, no schema change, no new dep.
>
> Authored 2026-08-02 against HEAD `f110150`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Status — ☐ 0/4, not started.**

## Why this work exists

Two problems, one surface.

**No GL means no spatial channel at all.** When `s.scene_host == nullptr` the pane
draws a text placard where the viewport would be
([shell.cpp:1081-1091](../../../desktop/src/ui/shell.cpp#L1081)), and the same
happens when the host is not `ready()` (a driver where a shader will not build,
[shell.cpp:1093-1099](../../../desktop/src/ui/shell.cpp#L1093)) or when `render()`
returns no texture. That placard is a *correct* degradation — it names the reason
and the models, provenance and legend above it still read, so nothing is hidden —
but it means the entire spatial view is unavailable headless, in the null test
harness, over plain SSH, in `desktop-ui-test`, and on any driver that fails shader
build. The models are fully woven in every one of those cases. Only the drawing is
missing, and the drawing is a rectangle per cell.

**Even with GL, there is no perspective-free reading surface.** `top_down()` is
described as "the plain 2D-ish fallback … when depth confuses"
([camera.h:9-11](../../../desktop/src/scene3d/camera.h#L9)), but it still projects
through `mat4x4_perspective` ([camera.h:88-94](../../../desktop/src/scene3d/camera.h#L88)),
so cells near the frame edge are smaller than cells at the centre. For *finding* a
place that is fine. For *reading* a density off the plane — comparing two cells,
tracing a path's locality shape — a perspective plane is the wrong instrument, and
the app's own rule says so: 3D to find, **2D to read**.

A flat surface fixes both, and it makes the spatial channel testable under the
null backend for the first time — which is the quiet reason it is worth its size.

## What already exists (verified 2026-08-02 against `f110150`)

- **Everything needed is already computed before GL is reached.** By the time the
  `scene_host == nullptr` branch runs, `sv.terr`, `sv.traj`, `sv.conv` and
  `sv.slice` are all built ([shell.cpp:920-933](../../../desktop/src/ui/shell.cpp#L920))
  and the slice is kept current by the same re-slice path the GL view uses. This
  brief adds a *renderer*, not a model.
- **`Terrain` is a flat array pair.** `w`, `h`, `height[w*h]`, `flags[w*h]`
  ([types.h:70-75](../../../desktop/src/space/types.h#L70)) — one filled rect per
  cell, no geometry, no projection maths.
- **The colour rules are already written down twice** — in `kTerrainFrag` and in
  the C++ swatch constants [49](49-one-time-truth-in-the-scene.md)-T3 adds. This
  brief must not add a *third*: T1 puts the mapping in one pure helper and has both
  the flat surface and (by comment) the shader defer to it.
- **`Projection::project`** gives `(u,v)` for the trajectory polyline directly
  ([types.h:44](../../../desktop/src/space/types.h#L44)) — no camera needed.
- **The null-backend path is already a first-class, tested branch.** `test_shell`
  and `desktop-ui-test` drive it; the HUD, provenance chips and legend are all
  exercised there today. A drawn surface joins that coverage rather than needing
  new harness work.
- **`views/` is the established home for pure-model + ImGui draw halves**
  (`canvas`, `timeline`, `slice_view`, each with a `_draw` sibling) — the pattern
  T2 follows, so this TU does not become a new architectural shape.

## Tasks

### T1 — The cell→colour model, in one place (M)

**Goal.** One pure, tested function decides what a cell looks like — shared by the
flat surface and referenced by the shader, so the two cannot drift into different
readings of the same recording.

**Steps.**
1. New pure helper (in `space/terrain.h`'s TU or a small `views/scene2d.h`
   beside the draw half — choose by where the tests are simplest and say which,
   with the constraint that it must be engine-free and ImGui-free so both binaries
   and the null harness link it):
   ```
   struct CellPaint {
       float r, g, b, a;
       bool hatched = false;   // torn / unknown patterning the draw half applies
       const char *why = "";   // the fidelity state this paint encodes, verbatim
   };
   CellPaint cell_paint(float height, uint32_t flags, uint8_t kind);
   ```
2. It must reproduce `kTerrainFrag`'s decisions exactly, **in the same order**:
   kind hue → height mix → churn/scaffold → statistical dim → fog-of-war pit →
   torn/rubble last. Getting the order wrong produces a surface that disagrees
   with the 3D view about which fidelity state a cell is in — the one failure mode
   that would make this feature worse than the placard.
3. `why` carries the state's name so a hover readout and a golden text can both use
   it — and so a reader of the golden can see *which branch* fired, not only the
   resulting colour.
4. Add a comment at `kTerrainFrag` naming `cell_paint` as the C++ mirror, matching
   the existing keep-in-sync convention the `TORN`/`STAT`/`CHURN` constants already
   use against `TerrainFlag` ([terrain.h:49-61](../../../desktop/src/space/terrain.h#L49)).
   The GLSL string cannot include a header; the convention is the mechanism.

**Tests.** New golden in `test_terrain.cpp` (or a `test_scene2d.cpp`): a table of
`(height, flags, kind)` covering every flag combination and both sentinel kinds,
dumped as text. The golden makes any future divergence between the two renderers a
diff rather than a discovery.

**Done when.** One function decides cell appearance; its branch order matches the
shader's; every flag combination is pinned by a golden.

### T2 — The flat surface (M)

**Goal.** A faithful, perspective-free rendering of the current slice, drawn with
`ImDrawList` alone.

**Steps.**
1. New `desktop/src/views/scene2d_draw.cpp` (+ header), following the existing
   `*_draw` convention: takes the pure models and draws one filled rect per cell
   at `cell_paint`'s colour, sized to fit the available region with an integer
   cell size so cells stay square and do not shimmer.
2. Overlay each trajectory as a polyline through `Projection::project`,
   distinguishing exact from statistical by the **same** channel the 3D view uses
   (solid vs. dashed), not a new one. Access marks and convergence marks likewise
   reuse their established colours.
3. **Break the polyline at unplaced vertices.** `TrajPoint::placed`
   ([types.h:61-67](../../../desktop/src/space/types.h#L61)) is false for a rel
   offset the anchor could not place; bridging across one would draw a path segment
   the recording does not support. Break the strip; do not interpolate.
4. Honour the terrain playhead: the surface draws `sv.slice`, which is already cut
   at `sv.slice_t`, and the trajectory is clipped to the same `t` —
   [49](49-one-time-truth-in-the-scene.md)'s rule applies identically here, and if
   49 has not landed, this brief is where the flat surface gets it right from the
   start (say so in the code rather than reproducing the 3D view's inconsistency).
5. At very large `w`/`h` a rect-per-cell is too many draw commands: down-sample by
   an integer factor and **state the factor on screen** ("1 px = 4×4 cells"),
   taking the maximum height and the OR of the flags over each block so a torn or
   statistical cell can never be down-sampled away. Losing a fidelity flag to
   binning would be the worst possible bug in this feature.

**Tests.** `test_shell.cpp` under the null backend: the surface draws without a GL
context (the assertion is that the branch runs and produces a non-empty draw list
or a pure plan structure — prefer a plan structure, following the `loom_plan` /
`slice_view` precedent, so the *layout* is golden-testable rather than only the
draw call count). Golden the down-sample block reduction: a block containing one
torn cell is torn.

**Done when.** The slice renders with no GL; unplaced vertices break the path;
down-sampling never drops a fidelity flag and always states its factor.

### T3 — Replace the placard, and offer the surface with GL too (S)

**Goal.** The no-GL path gains a real view, and the GL path gains a reading mode.

**Steps.**
1. `ui/shell.cpp`: in each of the three degraded branches (`scene_host == nullptr`,
   `!ready()`, `render()` returned no texture) draw the flat surface **in addition
   to** the existing placard, not instead of it. The placard states *why* there is
   no 3D; that reason is still true and must still be visible. This is the
   restructure-never-remove rule (D7 / F5).
2. Add a "flat" toggle to the HUD for the GL path, drawing the surface in place of
   the viewport. It is a *reading* mode: the pane's own rule is 3D to find, 2D to
   read, and until now the second half had no surface to happen on.
3. Picking works on the flat surface by inverting the cell layout — a click maps to
   a cell arithmetically, with no FBO and no readback. Route it through the same
   `resolve_pick` (and [47](47-scene-inspect-and-pickable-overlays.md)'s
   `resolve_pick_hint` when present) so both surfaces produce identical links from
   identical cells.

**Tests.** `test_shell.cpp`: under the null backend a click at a known pixel
resolves to the expected cell and the expected `dt_link` — **this is the first
time the pane's pick router is exercisable end-to-end with no GL**, and it is
worth an explicit named test for that reason.

**Done when.** Every no-GL branch shows both the reason and the surface; a flat
pick and a 3D pick on the same cell produce identical links.

### T4 — Read exactly: axes, ticks and a value readout (S)

**Goal.** The flat surface is a *reading* instrument, so it states its numbers.

**Steps.**
1. Draw region boundaries from `proj.regions` with verbatim labels (including
   `code@0x<base>` for an unnamed span), and mark the compacted-domain edge so
   padding is visibly not-land rather than a dark cell that could read as
   never-touched.
2. Hover states the cell's address, its owning region and its raw quantity with a
   named unit — the same wording [47](47-scene-inspect-and-pickable-overlays.md)'s
   `PickHint` produces. If 47 has landed, call it; if not, produce the same text
   and note the duplication so 47 can collapse it rather than adding a second
   phrasing for the same fact.
3. A short scale strip: the height ramp with its raw maximum, matching
   [49](49-one-time-truth-in-the-scene.md)-T3's legend so the two surfaces state
   the same scale.

**Tests.** Golden the readout text for a fixture cell (address, region label,
quantity, unit) and assert the region boundary set matches
`Projection.domain_off`'s partition exactly.

**Done when.** The flat surface names every quantity it draws and every region it
bounds.

## Fidelity notes (D7)

- **The placard survives.** A degraded branch keeps its stated reason; the surface
  is added beside it. Replacing an explanation with a picture would hide *why* the
  3D is absent.
- **One colour rule, two renderers.** `cell_paint` is the single source of truth
  and its branch order is pinned by a golden — a flat surface that disagreed with
  the 3D view about a torn cell would be worse than no surface.
- **Down-sampling ORs the flags and maxes the height.** A binned block containing
  a torn or statistical cell is torn or statistical. The block factor is always
  stated on screen.
- **Unplaced vertices break the path.** Never interpolate across a vertex the
  anchor could not place — that would draw an address the recording never carried.
- **Padding is not a dark cell.** The compacted-domain edge is marked, so "outside
  the domain" and "in-domain, never described" stay distinguishable — the same
  distinction 44's `TF_UNKNOWN` established for the 3D view.

## Effort and risk

Four tasks, two medium and two small — the largest brief in the
[46](46-3d-functional-roadmap.md) family, but the most isolated: one new `views/`
TU plus three degraded-branch call sites. The main risk is the second-source-of-
truth problem, which T1 exists to prevent — build `cell_paint` and its golden
first, and the rest is layout.
