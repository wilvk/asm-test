# Inspect before you leap — hover readout, pick preview, pickable overlays

> **Sources.** Gaps G1–G3 of [46-3d-functional-roadmap.md](46-3d-functional-roadmap.md),
> which cuts this brief. Upstream: [UX/dataviz review](../analysis/2026-07-29-gui-ux-dataviz-review.md)
> #14 ("Show hover feedback and a pick preview on the 3D viewport") and the
> [city doc](../analysis/2026-07-30-computer-as-city-3d.md) §4 "Interaction /
> drill-in" — *"today arcs are un-pickable dead overview objects, making them pick
> is the real deliverable"*. Read [_conventions.md](../implementations/_conventions.md)
> first; D1–D11 live in this directory's [README](README.md).
>
> **Prerequisites: none.** Every task reads models that already exist
> (`TerrainModel`, `TrajectorySet`, `ConvergenceSet`, `Projection`). No producer
> change, no schema change, no new engine link, no new third-party dep.
>
> Authored 2026-08-02 against HEAD `f110150`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Status — ☐ 0/5, not started.**

## Why this work exists

The 3D pane's contract is *"use 3D to FIND a place, then the flat 2D views to
READ it"* ([shell.cpp:907](../../../desktop/src/ui/shell.cpp#L907)). Today the
only way to learn anything about a place is to click it — which immediately
navigates to another view ([shell.cpp:1164-1175](../../../desktop/src/ui/shell.cpp#L1164)),
abandoning the camera framing that got you there. There is no cheaper question
than "what is this?", and the scene cannot answer it.

The information needed is not missing — it is computed and discarded.
`resolve_pick` already derives the cell's address, its owning `Region`, its
`CodeCell`/`DataCell` and its churn state
([pick.cpp:52-119](../../../desktop/src/scene3d/pick.cpp#L52)) purely to decide
*which* view to open, then returns a bare `dt_link`. Exposing that as a hover
readout is a refactor, not new analysis.

Separately, roughly half the drawn geometry cannot be picked at all: the pick pass
renders the terrain program and the PC-vertex points
([scene.cpp:776-794](../../../desktop/src/scene3d/scene.cpp#L776)) and never draws
`access_spurs_` or `conv_arcs_` ([scene.h:187-189](../../../desktop/src/scene3d/scene.h#L187)).
Convergence arcs are the scene's only cross-thread finding — a bright magenta
bezier the user cannot interrogate, hide from a legend, or follow to either side.

## What already exists (verified 2026-08-02 against `f110150`)

- **The pick round trip works and is tested.** `pick_id_cell` / `pick_id_vertex`
  ([pick.h:27-30](../../../desktop/src/scene3d/pick.h#L27)) define the shared id
  space; `decode_pick` inverts it; the GL half writes it in
  `render_pick_into_fbo`. The id space is `0` = background, `[1, n*n]` = cells,
  `[n*n+1, …)` = vertices — **contiguous and unbanded**, so T3 below must claim
  new ranges above the vertex band, not between existing ones.
- **`render_pick_buffer`** ([scene.h:130-131](../../../desktop/src/scene3d/scene.h#L130))
  already exposes the whole id buffer for a camera — the headless FBO smoke uses
  it. T1's throttled hover can reuse `SceneHost::pick`'s single-pixel path; do not
  add a second readback mechanism.
- **`resolve_pick` is GL-free and engine-free** (`pick.cpp`'s banner) and is
  covered by tests that run with no GL context at all. Anything T2 adds beside it
  inherits that testability — keep the new helper in the same TU.
- **The cell→content lookup is a linear scan.** `resolve_pick` walks `terr.code`
  then `terr.data` comparing `cc.cell == p.cell`
  ([pick.cpp:62-73](../../../desktop/src/scene3d/pick.cpp#L62)). Acceptable once
  per click; **not** acceptable once per frame on hover. T1 addresses this
  explicitly.
- **The viewport is one `InvisibleButton`** ([shell.cpp:1140-1143](../../../desktop/src/ui/shell.cpp#L1140))
  serving as focus target and mouse hit-target; `vp_hover` is already computed and
  currently gates only camera + pick.
- **`ConvergenceSet`** carries `{tid_a, tid_b, cell, t_a, t_b, gap}` per mark
  ([converge.h](../../../desktop/src/space/converge.h)) — everything a readout and
  a drill-in need, with `gap` stating its own looseness.

## Tasks

### T1 — A throttled hover pick and the cell→content index (M)

**Goal.** The pixel under the cursor resolves to a `Pick` at most once per actual
mouse move, cheaply enough to run every frame the pointer is over the viewport.

**Steps.**
1. `ui/shell.h`, `SceneView`: add `ImVec2 hover_px` (last pixel the hover pick ran
   for), `uint32_t hover_id`, and the resolved hint from T2. Persist per
   recording, like `nav_dragging`/`viewport_focus` already are.
2. `ui/shell.cpp`, the `if (vp_hover)` block
   ([shell.cpp:1154](../../../desktop/src/ui/shell.cpp#L1154)): before the click
   handling, run the hover pick **only when** the cursor pixel differs from
   `hover_px` **and** no drag is in progress (`!sv.nav_dragging`) — a GL FBO
   readback per frame during an orbit is exactly the cost to avoid. Reuse
   `s.scene_host->pick(...)`; do not add a new host method.
3. `space/terrain.h`/`.cpp`: add a built-once `cell → index` lookup so a cell's
   `CodeCell`/`DataCell` resolves in O(log n) or O(1) rather than the current
   linear scan. Two candidate shapes — a sorted `std::vector<uint32_t>` of cell
   ids parallel to `code`/`data` plus `std::lower_bound`, or a flat
   `std::vector<int32_t>` of size `w*h` — **pick by measured memory**: at Hilbert
   order 12 a flat table is 16.7M entries, so the sorted-vector form is the
   default unless the plane is small. State the choice and its reason in the
   header comment.
4. Route `resolve_pick`'s existing scans through the new index so click and hover
   share one lookup path (no second implementation to drift).

**Tests.** `desktop/test/test_drillin.cpp` (the existing GL-free pick/router test
TU; `test_scene_fbo.cpp` covers the GL half): assert the index returns the same
`CodeCell`/`DataCell` the linear scan did for a fixture with several touched
cells, including a cell with neither. `test_terrain.cpp`: assert the index is
built once and is byte-stable across `slice(t)` calls.

**Done when.** Hover picking runs at most once per mouse-move pixel; a drag
performs zero readbacks; `resolve_pick` returns byte-identical links to before for
every existing test fixture.

### T2 — `resolve_pick_hint`: the readout `resolve_pick` throws away (M)

**Goal.** A pure, golden-testable function returning everything the user needs to
decide whether to click — sharing one code path with the click resolution, so the
preview can never disagree with what the click does.

**Steps.**
1. `scene3d/pick.h`: add
   ```
   struct PickHint {
       bool empty = true;          // nothing pickable under the cursor
       std::string what;           // "code cell" | "data cell" | "survey cell" |
                                   // "PC vertex" | "convergence" | "padding"
       std::string where;          // region label + address/offset, verbatim
       std::string quantity;       // the cell's own number, with its unit named
       std::string fidelity;       // "" when exact; else the graded reason
       std::string target;         // dt_view_name of where a click would go, or
                                   // "" when a click would do nothing
   };
   PickHint resolve_pick_hint(const space::TerrainModel &, 
                              const space::TrajectorySet &,
                              const space::ConvergenceSet &, const Pick &);
   ```
   Keep it in `pick.cpp` beside `resolve_pick` (same GL-free, engine-free TU).
2. **Factor, do not duplicate.** Extract the cell classification currently inline
   in `resolve_pick` ([pick.cpp:56-119](../../../desktop/src/scene3d/pick.cpp#L56))
   into one private helper both functions call, so the hint's `target` is derived
   from the *same* branch that picks the view. A preview that can drift from the
   action is worse than no preview.
3. Fidelity text is graded, not invented: a `TF_TORN` cell says its height is a
   lower bound; a `TF_STAT`-only cell says "statistical — survey"; a `TF_UNKNOWN`
   cell says "in-domain, never described" (**not** a count of zero); a padding
   cell says "outside the compacted domain — nothing here". Reuse the existing
   wording from the HUD chips (`placement_chips`,
   [hud.h:29-34](../../../desktop/src/scene3d/hud.h#L29)) rather than writing new
   phrasing for the same facts (D7 / [24](../archive/gui/24-one-visual-language.md)).
4. `quantity` names its unit — "heat 1,284 hits" not "1284" — the review's
   highest-leverage finding generalised (bare-integer magnitude, §2 of the review).

**Tests.** A golden-text case per class in the pick test TU: exact code cell,
churned code cell, data cell, statistical-only cell, `TF_UNKNOWN` cell, padding
cell, exact PC vertex, statistical residency vertex. Assert `target` equals
`dt_view_name(resolve_pick(...)->view)` for every case where `resolve_pick`
returns a link, and that `target` is empty exactly when it returns `nullopt` —
this is the anti-drift assertion and it must be exhaustive.

**Done when.** Every branch of `resolve_pick` has a corresponding hint case, and
the two functions share one classification helper.

### T3 — Make the overlay geometry pickable (M)

**Goal.** Convergence arcs and access spurs stop being un-interrogable decoration.

**Steps.**
1. `scene3d/pick.h`: add two id-range constructors above the existing vertex band
   — `pick_id_conv(n, npts, i)` and `pick_id_spur(n, npts, nconv, i)`. The vertex
   band is open-ended today (`[n*n+1, …)`), so the new bands must start past the
   **actual uploaded** vertex count, which the decoder therefore needs. Extend
   `decode_pick` to take the band sizes (or a small `PickBands` struct built once
   per upload) rather than inferring them — an inferred boundary is the drift risk
   here.
2. `scene3d/scene.cpp`: give `conv_arcs_` and `access_spurs_` an id attribute
   (mirroring `vbo_pts_id_`) and draw them into the pick FBO in
   `render_pick_into_fbo` ([scene.cpp:761-795](../../../desktop/src/scene3d/scene.cpp#L761)),
   after the points, with a widened line width so a thin arc is clickable. Reuse
   `prog_pick_pt_`'s frag shader (`flat in uint fId`) — the vertex stage needs
   only the same `pos`+`vid` pair.
3. `scene3d/pick.cpp`: extend `resolve_pick` — a convergence arc has **two** valid
   destinations (tid A at `t_a`, tid B at `t_b`), so a single `dt_link` cannot
   express it. Resolve the arc to the tid whose `t` is nearer the current
   `follow_step`, **and say which side was chosen in the hint** rather than
   choosing silently; an access spur resolves to the same step as its owning PC
   vertex (per `pick.h`'s existing note, it drills to that step — now it can be
   *hit* as well as *implied*).
4. Fidelity: a convergence is a **co-locality hint, never a proven race or
   order** — its readout must carry `gap` verbatim and the existing wording from
   `converge.h`'s own contract. Do not let a pickable arc read as a stronger claim
   than a drawn one.

**Tests.** `test_drillin.cpp`: assert `decode_pick` round-trips each band's first,
middle and last id with no overlap between bands for several `(n, npts, nconv)`
combinations, including empty bands. Assert an arc pick resolves to a link naming
one of the two tids and that the hint states which. Extend the existing FBO smoke
to assert an arc id appears in `render_pick_buffer` for a fixture with a known
convergence.

**Done when.** Every drawn overlay primitive is pickable; the bands cannot alias;
an empty `conv`/`spur` set changes no existing id.

### T4 — The hover readout, and a non-destructive inspect (M)

**Goal.** The cursor answers "what is this and where would a click send me?"
before any navigation happens.

**Steps.**
1. `ui/shell.cpp`: in the `vp_hover` branch, render `sv.hover_hint` as an ImGui
   tooltip — `what` / `where` / `quantity` on their own lines, `fidelity` in
   `dt_warn_col()` when non-empty, and a final dimmed line `"click → <target>"`
   (or `"click → nothing here"` when `target` is empty). Text-only, so it survives
   2.0x DPI and the CVD palette unchanged.
2. Make the drill-in **deliberate rather than incidental**: keep single-click →
   navigate (no regression, no relearning), but suppress it when
   `hint.target` is empty so a padding click is a no-op instead of a silent
   nothing-happened. The existing `nav_dragging` guard already suppresses
   drag-release; leave it exactly as is.
3. Under the null backend (`s.scene_host == nullptr`) there is no pick, so no
   tooltip — the placard branch ([shell.cpp:1081](../../../desktop/src/ui/shell.cpp#L1081))
   returns before this code. Do not fabricate a hover there;
   [52](52-flat-terrain-surface.md) is where the no-GL path gains a real surface.

**Tests.** `desktop/test/test_shell.cpp` under the null backend cannot exercise
the tooltip (no GL, no pick) — so the assertion lives in T2's golden text plus a
`desktop-ui-test` interaction case if one can drive a synthetic hover; if it
cannot, say so in the test file rather than leaving an untested claim implied.

**Done when.** Hovering any pickable primitive shows its identity, its quantity
with a named unit, its fidelity grade when not exact, and its click destination;
hovering padding says so.

### T5 — HUD: legend the overlays and advertise what is pickable (S)

**Goal.** The magenta arcs and lavender spurs stop being unexplained marks, and
the user learns the pane is interrogable.

**Steps.**
1. `scene3d/hud.cpp`: add the convergence and access-spur swatches to the legend
   (UX #36 — the `layers.convergence` toggle plumbing already exists at
   [scene.cpp](../../../desktop/src/scene3d/scene.cpp) and
   [scene.h:41-42](../../../desktop/src/scene3d/scene.h#L41); only the HUD row and
   key are missing), plus a `convergence` checkbox bound to
   `SceneLayers::convergence`.
2. Add a one-line hint to the legend block: hover to inspect, click to open the
   flat reader. Keep it in the legend, not the primer — the primer is first-open
   only ([shell.cpp:899-914](../../../desktop/src/ui/shell.cpp#L899)) and this must
   stay visible.
3. The arc swatch's caption states the fidelity grade of the mark itself: a
   co-locality hint with a stated gap, never a proven race.

**Tests.** `test_shell.cpp` / the HUD test TU: assert the legend enumerates every
drawn overlay class and that toggling `convergence` off is reflected in the
`SceneLayers` the frame carries.

**Done when.** No drawn mark class lacks a legend entry; every legend entry names
a real encoding.

## Fidelity notes (D7)

- A hint never states a quantity the model does not carry. An absent `mem` stream
  means a data cell's `quantity` says "coarse — no per-access memory stream"
  (reusing `TerrainModel::mem_note`), not "0 bytes".
- `TF_UNKNOWN` and padding are **different** and must read differently: in-domain
  never-described vs. outside the compacted domain. This is the distinction 44's
  T2 built into the terrain; the readout is where a user finally sees it named.
- A statistical cell's hint never offers an exact reader as its `target` — the
  existing `resolve_pick` invariant ([pick.cpp:107-111](../../../desktop/src/scene3d/pick.cpp#L107))
  carries through unchanged because T2 shares its branch.
- A convergence hint carries `gap` verbatim. Making a hint pickable must not
  upgrade the claim it makes.

## Effort and risk

Five tasks, four medium and one small; entirely render/UI-side. The two risks
worth naming: the per-frame FBO readback if T1's throttle is wrong (measure it —
an orbit must perform zero readbacks), and pick-id band aliasing if T3 infers a
boundary instead of carrying it (the test asserts non-overlap explicitly for this
reason).
