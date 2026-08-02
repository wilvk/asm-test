# One time truth — clip the worldline to the playhead, and make height readable

> **Sources.** Gaps G7–G8 of [46-3d-functional-roadmap.md](46-3d-functional-roadmap.md),
> which cuts this brief. Upstream: [UX/dataviz review](../../analysis/2026-07-29-gui-ux-dataviz-review.md)
> #50 ("Clip the 3D trajectory to the playhead and mark the execution front"),
> #56 ("Add a vertical scale reference and clarify the dual vertical meaning"),
> #37 ("Complete the 3D HUD legend: label the terrain's own encodings") and #40
> ("Add iso-density contour lines to the terrain"). Read
> [_conventions.md](../implementations/_conventions.md) first; D1–D11 live in this
> directory's [README](README.md).
>
> **Prerequisites: none.** Everything here reads state the frame already carries
> (`SceneFrame::slice_t`, `Scene::traj_scale_`, `Terrain::height`). No producer
> change, no schema change, no new dep.
>
> Authored 2026-08-02 against HEAD `f110150`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Status — ✅ 4/4, landed 2026-08-02.** T1 `slice_step` clips the worldline
> (dim, never discard — `uTimeCutY`/`uHasTimeCut` in `kTrajFrag`), applied to
> the trajectory lines and `access_spurs_`; `conv_arcs_` deliberately excluded
> (a mark carries two times, T1 step 5); `uHasTimeCut=0` once the playhead
> reaches the end, so the whole-recording view is byte-identical to the
> pre-brief render. T2 generalises `find_head` into `find_vertex` (exact vs.
> at-or-before) and adds a distinct beacon-amber execution-front glyph, gated
> on `layers.exact`, never snapped when nothing qualifies. T3 adds
> `uContourLevels` iso-density bands to `kTerrainFrag` (phase-shifted +0.5 so
> the always-1.0 brightest cell in any slice never lands exactly on a line —
> a real bug the GL smoke suite caught and this fix closed) plus a pure
> `terrain_encoding_swatches()` legend (exhaustive-by-test over
> TORN/STAT/CHURN/UNKNOWN) and `TerrainModel::max_full_heat` for the raw
> scale a band is worth. T4 adds `vertical_axes_note`, pure
> `trajectory_axis_ticks`, and `draw_trajectory_ruler` (HUD TU, projected via
> `Camera::mvp` into the 3D viewport, skipped under the null backend the same
> way the viewport image already is). `docker-desktop` green end to end
> (`test_shell`, `test_terrain`, `test_scene_fbo` GL smoke, `test_ui` 28/28,
> `desktop-engine-boundary-check` — D4 intact).

## Why this work exists

This is the only brief in the [46](46-3d-functional-roadmap.md) family that fixes
something currently **misleading** rather than merely absent, which is why it
should not queue behind the others.

The terrain re-slices to `[0, t]` whenever the playhead moves
([shell.cpp:1002-1012](../../../desktop/src/ui/shell.cpp#L1002)). The worldline
does not: each trajectory is uploaded once and drawn as a full `GL_LINE_STRIP`
over every vertex ([scene.cpp:680](../../../desktop/src/scene3d/scene.cpp#L680)),
and `kTrajFrag` has no time cut — its only time-aware branch is 44-T6's comet tail
around `uHeadY`, which brightens a band on a *different* axis
(`follow_step`, a per-tid vertex counter) and clips nothing.

So at playhead `t` the screen shows residency for `[0, t]` under a path for
`[0, nsteps]`, with nothing saying the two surfaces disagree. A user reading them
together reads a false simultaneity: a path apparently crossing terrain that,
according to the terrain, has not been touched yet. The primer's promise —
*"press Play to watch the path form"*
([shell.cpp:912](../../../desktop/src/ui/shell.cpp#L912)) — is not kept, because
the path does not form.

The second half of the brief is the quantity problem. Terrain-Y is normalised
log-density; trajectory-Y is `t * traj_scale_` (trace-time). Two different
quantities share one vertical axis with no ticks, no labels, no key, and no
banding anywhere on screen. `kTerrainFrag` applies a colour ramp and the `TF_*`
tints and stops; the HUD legend lists region swatches only. Height is a shape, not
a number — and the review's single highest-leverage finding across the whole GUI
was exactly this class of gap (magnitude rendered with no scale).

## What already exists (verified 2026-08-02 against `f110150`)

- **The frame already carries the playhead.** `SceneFrame::slice_t`
  ([scene_host.h:44](../../../desktop/src/ui/scene_host.h#L44)) is the `t` the
  slice was cut at — no new field is needed for T1, which is what makes it cheap.
- **The trajectory's Y is already a pure function of `t`.** `Scene::traj_scale_`
  ([scene.h:190](../../../desktop/src/scene3d/scene.h#L190)) is the single
  world-Y-per-step scale every vertex of an upload shares, and `kTrajVert` already
  passes `pos.y` through as `vY` for exactly this reason (44-T6's own comment).
  So the cut is `uTimeCutY = slice_t * traj_scale_` — one uniform, no new
  attribute, no re-upload.
- **The comet tail is on a different axis and must stay that way.**
  `follow_step` walks `TrajPoint.t`; `slice_t` walks terrain-residency time. 44's
  `SceneView` comment ([shell.h:91-110](../../../desktop/src/ui/shell.h#L91)) and
  34's two-axes rule both forbid fusing them. T1 adds a *second, separately named*
  uniform; it does not reuse `uHeadY`.
- **`Line::pt_t` / `pt_placed` / `pt_pos` are already CPU-retained per vertex**
  ([scene.h:181-184](../../../desktop/src/scene3d/scene.h#L181)) — 44-T6's
  `find_head` walks them. T2's execution-front marker reuses that retained data
  rather than adding a readback.
- **`Terrain::height` is normalised to `[0,1]` at upload**
  ([scene.h:70-72](../../../desktop/src/scene3d/scene.h#L70)), so a contour on
  `fract(vHeight * kLevels)` bands a *normalised* quantity — T3 must therefore
  carry the raw scale into the legend, or the bands are unlabelled decoration.
- **`CodeCell::full_heat`** ([terrain.h:116-118](../../../desktop/src/space/terrain.h#L116))
  is the raw per-cell count the height is derived from — the number a legend needs
  to state what a band is worth.

## Tasks

### T1 — Clip the worldline to the terrain playhead (M)

**Goal.** At playhead `t` the path and the terrain describe the same interval.

**Steps.**
1. `scene3d/scene.h`: add a public `uint64_t slice_step = 0;` set per frame like
   `follow_step` already is (a draw-time uniform, no upload). Name it for the axis
   it walks — `slice_step`, never `head`/`step`/`t` — so the two clocks stay
   distinguishable at every call site.
2. `ui/gl_scene_host.cpp`: pass `f.slice_t` into it each frame. `SceneFrame`
   already carries the value; no new field.
3. `scene3d/shaders/embedded.h`, `kTrajFrag`: add `uniform float uTimeCutY;` and
   `uniform int uHasTimeCut;`. Past the cut, **dim — never discard**: the vertices
   beyond the playhead are real recorded data, and hiding them would claim the
   recording ends there. A dim tail says "recorded, not yet reached"; a discard
   says "nothing here", which is false. Keep the existing stipple and comet-tail
   branches untouched and ordered after the dim so a statistical path stays
   stippled either side of the cut.
4. `scene3d/scene.cpp`: set `uTimeCutY = float(slice_step) * traj_scale_` in the
   trajectory draw, and `uHasTimeCut = 0` when the playhead is at the end
   (`slice_step >= nsteps`) so the whole-recording view is byte-identical to
   today's render.
5. Apply the same cut to `access_spurs_` — a spur is an access at its owning
   vertex's time, so an unclipped spur past the playhead reproduces the same false
   simultaneity at smaller scale. `conv_arcs_` are **excluded**: a convergence mark
   carries two times (`t_a`, `t_b`, [converge.h:36-45](../../../desktop/src/space/converge.h#L36))
   and dimming it against one of them would misstate which side is in the past —
   leave arcs unclipped and say so in the code comment.

**Tests.** `test_scene_fbo.cpp` (the existing GL smoke): with the playhead at the
end, the rendered frame is unchanged from the pre-change baseline; with the
playhead at half, a pixel sampled on the path beyond the cut is dimmer than the
same pixel at full playhead — and still non-background, which is the "dim, not
discard" assertion. If the smoke self-skips for want of GL, state that in the test
file and add the pure half: `test_camera`/`test_scene`-side assertion that
`slice_step` reaches the scene unchanged.

**Done when.** Scrubbing the playhead visibly forms the path; the end-of-recording
view is a byte-identical no-op; the comet tail and the time cut remain separate
uniforms with separate names.

### T2 — Mark the execution front (S)

**Goal.** The path's leading edge at the playhead is a visible point, not an
inference from where the dimming starts.

**Steps.**
1. `scene3d/scene.cpp`: generalise 44-T6's `find_head`
   ([scene.h:217](../../../desktop/src/scene3d/scene.h#L217)) into a helper that
   locates a vertex by *either* axis, and use it to place a front glyph at the
   last placed vertex with `pt_t <= slice_step`. Reuse the existing single-point
   `vao_head_`/`vbo_head_` upload path
   ([scene.h:169](../../../desktop/src/scene3d/scene.h#L169)) — a second per-frame
   one-vertex buffer is the same negligible cost, and matching the established
   shape keeps the two glyphs' lifecycle identical.
2. The front glyph and the followed-citizen vehicle are **different marks on
   different clocks** and must be visually distinct (different shape or colour,
   not merely different position) — two identical dots on two axes is the fusion
   trap rendered as art.
3. No placed vertex at or before the playhead → **no glyph** (never a snapped one),
   matching 44-T6's own rule for the vehicle.

**Tests.** Extend whatever covers `find_head` today with the by-`slice_step`
lookup: a fixture where the nearest vertex at-or-before the playhead is unplaced
must yield no glyph; a fixture with a placed vertex must yield its exact position.

**Done when.** The front is marked, is distinguishable from the vehicle, and is
absent rather than approximate when nothing qualifies.

### T3 — Make the height readable: contours + a legend of the scene's own encodings (M)

**Goal.** Height stops being an unlabelled shape.

**Steps.**
1. `scene3d/shaders/embedded.h`, `kTerrainFrag`: darken the base where
   `fract(vHeight * uContourLevels)` falls under a small threshold, guarded so a
   flat/zero cell draws no line (a contour on an empty cell would imply a measured
   level). Expose `uContourLevels` as a uniform with a HUD toggle so the banding
   can be turned off; default on.
2. `scene3d/hud.cpp`: add an "encodings" block to the legend — a height→density
   gradient swatch, plus swatches for churn/scaffold (cyan), torn/rubble (red
   gash), statistical (dim), and fog-of-war (dark pit). Define the swatch colours
   as C++ constants **mirroring the GLSL literals with a keep-in-sync comment**,
   exactly the convention `kTerrainFrag`'s `TORN`/`STAT`/`CHURN` constants already
   follow against `TerrainFlag` ([embedded.h](../../../desktop/src/scene3d/shaders/embedded.h),
   [terrain.h:49-61](../../../desktop/src/space/terrain.h#L49)). The GLSL string
   cannot include a C++ header; do not invent a generator for four colours.
3. **State what a contour band is worth.** The shader bands a normalised height,
   so the legend must carry the raw scale: the slice's maximum `full_heat` and the
   fact that the ramp is log. Without that the contours are decoration. Compute it
   in the pure model (a `max_full_heat` on the slice or read off `TerrainModel`),
   not in the draw half.
4. Fidelity: contours are a re-encoding of a value already shown, so they add no
   claim — but they must not be drawn on `TF_UNKNOWN` or padding cells, where
   there is no value to band.

**Tests.** `test_terrain.cpp`: the raw-scale value the legend reports equals the
maximum `full_heat` over the slice, and is 0 for an empty slice. The HUD test
asserts the legend enumerates every `TerrainFlag` the shader branches on — an
exhaustive check, so adding a flag without a key fails.

**Done when.** Every colour the terrain shader can produce has a legend entry, and
the height ramp states its scale and its log-ness.

### T4 — Name the two vertical axes (S)

**Goal.** A user can tell which quantity a given piece of geometry's height means.

**Steps.**
1. `scene3d/hud.cpp`: one line stating plainly that terrain height is access
   density (log) while path height is trace-time (steps) — the two are different
   quantities on one screen axis. Place it with the encodings block from T3, not
   in the primer (which is first-open only).
2. Draw a thin vertical ruler at a plane corner, ticked `0..nsteps`, labelled
   **for the trajectory time axis only** and captioned as such. Project world
   points through `Camera::mvp` with `ImDrawList` in the HUD TU — `hud.cpp` is the
   ImGui + pure-model TU by design, so the scene TU stays text-free and ImGui-free
   (D4). Skip the ruler when `nsteps == 0`.
3. Under the null backend the ruler cannot draw (no camera-projected viewport),
   but the HUD note still reads — that is the graceful degradation the pane
   already practises, and it should be stated in the code comment so a later
   reader does not "fix" it.

**Tests.** The HUD test asserts the note text is present whenever a trajectory
exists and that the tick labels span `0..nsteps` for a fixture with a known step
count. Pure-model tick generation (a `std::vector<uint64_t>` of tick values) is
golden-testable; do it that way rather than testing draw calls.

**Done when.** Both vertical meanings are named on screen, and the ruler is
unambiguously scoped to the trajectory axis.

## Fidelity notes (D7)

- **Dim, never discard.** Data past the playhead is recorded and real; hiding it
  would claim the recording stops there. The dim is a "not yet reached in this
  view" signal, not an absence signal.
- **The two clocks stay separate.** `slice_step` (terrain-residency time) and
  `follow_step` (`TrajPoint.t`) get separate uniforms, separate names and visually
  distinct marks. Reusing one for the other is the documented 34/44 fusion trap.
- **Convergence arcs are not clipped**, because a mark carries two times and
  clipping against one would misstate the pair. State this in the code, not only
  here.
- **Contours re-encode; they do not add.** No band on `TF_UNKNOWN`, no band on
  padding, no band on a flat cell.
- **The legend is exhaustive by test.** A shader branch with no legend entry is a
  colour the user cannot decode — the test makes that a build-time concern.

## Effort and risk

Four tasks, two medium and two small; shader + HUD + one draw-time uniform. The
one real risk is regressing the whole-recording view, which is the state the
existing golden/smoke coverage exercises — T1 step 4 makes that path an explicit
no-op (`uHasTimeCut = 0`) precisely so the baseline stays byte-identical.
