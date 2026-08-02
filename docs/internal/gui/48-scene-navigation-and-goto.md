# Go there — camera pan, recentre, address goto, and discoverable controls

> **Sources.** Gaps G4–G6 of [46-3d-functional-roadmap.md](46-3d-functional-roadmap.md),
> which cuts this brief. Upstream: [UX/dataviz review](../analysis/2026-07-29-gui-ux-dataviz-review.md)
> #55 ("Let the 3D camera pan / recenter on a region of interest") and #38
> ("Surface the 3D camera controls — wired but undiscoverable"); the
> [city doc](../analysis/2026-07-30-computer-as-city-3d.md) §4 "Camera" (a
> persistent compass home + a top-down minimap inset) and §7's own open question
> — *"does the 'city' preset + a guided default keep it legible, or does the
> operator drown in controls?"*. Read [_conventions.md](../implementations/_conventions.md)
> first; D1–D11 live in this directory's [README](README.md).
>
> **Prerequisites: none.** `Camera` is pure math over `linmath.h` and is already
> exercised headlessly by `test_camera.cpp`; `Projection::project` already exists
> and is tested. No producer change, no schema change, no new dep.
>
> Authored 2026-08-02 against HEAD `f110150`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Status — ◐ pure infra landed 2026-08-02, wiring open.** `Camera::pan`/
> `frame` + the four `CamKey` Pan* values (T1's model half), the
> `scene3d/goto.h`/`.cpp` resolvers (`scene_recentre_target` T2,
> `scene_goto_addr`/`scene_goto_region` T3, `scene_home_target` T4) and the HUD
> additions (T4's "you are here" + reset/default-view split, T3's "go to" row,
> T5's generated controls block) are landed, tested headlessly (new standalone
> `test_goto.cpp`, extended `test_camera.cpp`/`test_shell.cpp`), and
> `docker-desktop` is green. **What is not yet landed: the `ui/shell.cpp`
> wiring** that wires user input to these — T1 step 4 (middle-drag/shift-drag
> mouse pan), T2 (the double-click handler), T3/T4 (applying `HudState`'s
> `req_goto`/`req_reset_view`/`req_default_view` to the `Camera`, and calling
> `scene_home_target` once per weave to populate `SceneView::home_u/v`), and
> the primer-text / find-bar "show in 3D" touches. The HUD draws and computes
> correctly today; a click on "go to" or "default view" sets the intent but
> nothing yet applies it to the camera (harmless — no crash, no wrong
> placement, just inert until the next task lands). Deferred rather than
> forced through: [47-scene-inspect-and-pickable-overlays.md](47-scene-inspect-and-pickable-overlays.md)
> is *concurrently* landing its own `shell.cpp`/`pick.h`/`pick.cpp` changes
> (the throttled hover pick) in this same tree, and `shell.cpp` is the one
> file both briefs touch — the shell.cpp wiring above is the remaining,
> mechanical work for whoever picks this back up once that settles.

## Why this work exists

The plane is a *reproducible address layout*: `Projection::project` maps any
address to a plane cell, deterministically, from the recording's own region set
([types.h:42-47](../../../desktop/src/space/types.h#L42)). That is a
navigable coordinate system — and nothing in the app navigates it.

The camera orbits a hard-coded point. `Camera::target` is `{0.5, 0, 0.5}`
([camera.h:32](../../../desktop/src/scene3d/camera.h#L32)) and only `orbit()` and
`dolly()` mutate the camera at all ([camera.h:51-59](../../../desktop/src/scene3d/camera.h#L51));
`reset()` and `top_down()` restore it. So dollying toward a corner district leaves
you orbiting a point you do not care about, with the thing you were studying
swinging in and out of frame. For a pane whose stated job is *finding a place*,
this is the load-bearing gap.

There is also no way to *name* a destination. A user who knows the address they
want — from a canvas offset, a find hit, a hot-edge row, a region banner — has no
means to say "show me that". They orbit and guess. The `Projection` makes the
answer exact and cheap.

And the controls that do exist are invisible. `CamKey` defines a full keyboard
camera — orbit, dolly, reset, top-down
([camera.h:115-158](../../../desktop/src/scene3d/camera.h#L115)) — advertised
nowhere; left-drag orbit and wheel dolly are advertised nowhere; the HUD offers
two buttons. This is the project's known "keymap advertises keys the UI never
shows" pattern in its worst form, because here the UI does not advertise them
either.

## What already exists (verified 2026-08-02 against `f110150`)

- **`Camera` is pure and headlessly tested.** No GL, no ImGui — `test_camera.cpp`
  drives it under the null harness ([camera.h:1-14](../../../desktop/src/scene3d/camera.h#L1)).
  Every addition here stays in that closure, so every addition is unit-testable.
- **Dolly and pitch are already clamped** (`kMinRadius`/`kMaxRadius`,
  `kPitchLimit`, [camera.h:40-44](../../../desktop/src/scene3d/camera.h#L40)) — T1
  follows the same discipline for pan rather than inventing a new one.
- **`camera_key` funnels keyboard through the same `orbit`/`dolly`/`reset`/
  `top_down` the mouse uses** — one code path, stated as the design intent
  ([camera.h:107-114](../../../desktop/src/scene3d/camera.h#L107)). T1's pan must
  join that funnel, not sit beside it.
- **The keyboard camera is already wired to focus.** `s.cam_focus =
  sv.hud.kbd_focus || sv.viewport_focus` then `scene_apply_camera_keys(sv.cam)`
  ([shell.cpp:989-991](../../../desktop/src/ui/shell.cpp#L989)) — new `CamKey`
  values need no new plumbing.
- **`Projection::project(addr, &u, &v)`** returns the unit-plane coordinates
  directly ([types.h:44](../../../desktop/src/space/types.h#L44)) — exactly the
  `target[0]`/`target[2]` a recentre needs, no conversion.
- **The global find already produces offsets.** `FindHit::off`
  ([find.h:44-51](../../../desktop/src/ui/find.h#L44)) and the whole
  search-as-measurement model are pure and tested — T3 reuses that intent seam
  rather than adding a second search.
- **`Projection.regions` is sorted, non-overlapping, and carries `label`/`kind`**
  ([types.h:30-40](../../../desktop/src/space/types.h#L30)) — a ready-made
  destination list, already surfaced as swatches in the HUD legend but not as
  targets.

## Tasks

### T1 — `Camera::pan` and `Camera::frame` (S)

**Goal.** The camera can look at something other than the plane's centre, and can
be pointed at a cell programmatically.

**Steps.**
1. `scene3d/camera.h`: add
   `void pan(float du, float dv)` translating `target[0]`/`target[2]`, clamped to
   `[0,1]` on both axes so the target never leaves the model (the same
   clamp-don't-break discipline `dolly` already uses). `target[1]` stays 0 — the
   plane's height is not a pan axis, and letting it drift produces a camera whose
   horizon lies about where the ground is.
2. Add `void frame(float u, float v, float radius)` setting `target` to a plane
   coordinate and the radius, leaving `yaw`/`pitch` untouched — recentring must
   not also reorient, or the user loses the mental map they just built. Clamp
   `radius` through the existing `kMinRadius`/`kMaxRadius`.
3. Extend `CamKey` with `PanLeft`/`PanRight`/`PanForward`/`PanBack` and handle
   them in `camera_key` with a step scaled like `kOrbitStep` — keyboard and mouse
   stay one code path.
4. `ui/shell.cpp`: bind pan to **middle-drag and shift+left-drag** in the
   `vp_hover` block ([shell.cpp:1154-1163](../../../desktop/src/ui/shell.cpp#L1154)),
   leaving plain left-drag as orbit (no conflict, no relearning). Scale the delta
   by `radius` so panning feels the same when dollied in as when dollied out.

**Tests.** `desktop/test/test_camera.cpp`: pan clamps at all four edges; pan is
commutative with orbit (panning then orbiting equals orbiting then panning, for
the target); `frame()` leaves `yaw`/`pitch` bit-identical; `reset()` still returns
a panned camera to the documented default. Assert the new `CamKey` values apply
the same deltas the mouse path does.

**Done when.** `Camera` gains pan/frame with no GL and no ImGui, every new path is
covered by `test_camera`, and `reset()` remains a total restore.

### T2 — Recentre on what you found (S)

**Goal.** The place you just inspected becomes the place you are orbiting.

**Steps.**
1. `ui/shell.cpp`: on **double-click** in the viewport, run the existing pick, and
   on a `Pick::Cell` convert the cell to `(u,v)` — the cell-centre form
   `(x+0.5)/n, (y+0.5)/n` that `resolve_pick` already uses
   ([pick.cpp:52-53](../../../desktop/src/scene3d/pick.cpp#L52)) — and call
   `sv.cam.frame(u, v, sv.cam.radius)`. Reuse that rounding rather than
   re-deriving it; a second rounding rule is a second source of truth.
2. A double-click on a `Pick::Vertex` recentres on that vertex's cell (project its
   `addr`), not on the vertex's world-Y — the target rides the plane.
3. A double-click on background/padding does nothing and says so through the T4
   status line. Silence here reads as a broken control.
4. Guard against the double-click also firing the single-click navigate: the
   existing release-without-drag branch
   ([shell.cpp:1164](../../../desktop/src/ui/shell.cpp#L1164)) must not run for the
   second click of a double. Use ImGui's own double-click detection and suppress
   the pending single-click action for that frame.

**Tests.** `test_camera.cpp` covers `frame()` directly. The shell wiring has no
GL under the null backend, so assert the *pure* half: a helper
`scene_recentre_target(const TerrainModel&, const Pick&, float *u, float *v)` in
`pick.cpp` (GL-free) returning false for padding, tested for cell and vertex
picks.

**Done when.** Double-click recentres without reorienting; padding double-click is
an explicit no-op with a stated reason.

### T3 — Go to an address, a region, or a find hit (M)

**Goal.** A destination can be *named*, not only hunted.

**Steps.**
1. `scene3d/hud.cpp`: add a "go to" row — a small text input accepting a hex
   address or `0x…`, and a combo listing `terr.proj.regions` by `label` (falling
   back to `code@0x<base>` verbatim, never an invented name).
2. Resolution is pure and lives in an engine-free helper (`scene3d/hud.cpp` is
   ImGui-only; put the resolver in `pick.cpp` or a new `scene3d/goto.h` beside it
   — decide by where the tests are easiest to run, and say which in the header):
   `bool scene_goto_addr(const Projection&, uint64_t addr, float *u, float *v)`
   wrapping `Projection::project`. A region target frames the region's **actual
   cell footprint**, not a bounding box — walk `domain_off[i] .. domain_off[i+1]`
   through `project` and frame the extent that yields (the city doc's own rule:
   a plinth is the region's real Hilbert cell set, never a bbox, because a bbox
   paints over a neighbour's cells).
3. An address mapped by no region **refuses with the reason** — "not mapped by any
   region in this recording" — and moves the camera nowhere. Do not clamp to the
   nearest cell: that would silently answer a different question.
4. Wire the existing find: when a `FindHit` carries an `off`
   ([find.h:47](../../../desktop/src/ui/find.h#L47)) and the 3D pane is open, offer
   "show in 3D" through the same resolver. Reuse the find's intent seam; do not
   add a second search over the same streams.

**Tests.** New cases in the projection/pick test TU: a known address resolves to
the cell `project` gives; an unmapped address refuses with a non-empty reason and
does not write `u`/`v`; a region target frames an extent containing every cell of
that region's footprint and no cell of an adjacent region's.

**Done when.** Any address the recording maps can be reached in one action, and
any address it does not map refuses with a stated reason.

### T4 — Landmarks and a stable home (M)

**Goal.** The camera has somewhere meaningful to return to, and the scene tells
you where you are.

**Steps.**
1. `ui/shell.h`, `SceneView`: store a `home_u`/`home_v` computed once per weave —
   the **code-district centroid derived from the region footprint**, not the
   plane centre. Because a Hilbert cell is a real address, this anchor is stable
   across live growth (new events add cells; the founding region's placement does
   not move), which is exactly what makes "reset view" mean something on a growing
   capture.
2. Change the HUD's "reset view" to frame `home_*` rather than
   `Camera::reset()`'s fixed centre — and keep a separate "default view" that
   restores the literal `Camera{}` so the documented reset is not lost. Two
   buttons, two honest meanings; do not overload one.
3. Add a compact "you are here" readout to the HUD: the region under the camera
   target (via `Projection::unproject` on `target[0]`/`target[2]`) and the target
   address, or "outside the compacted domain" when unproject fails. This is the
   orientation channel the city doc's §4 asks for, in text, before any minimap
   geometry exists.
4. Status line for T2/T3 outcomes: a one-line `dt_warn_col()` note when a goto or
   recentre refused, naming the reason verbatim.

**Tests.** A pure helper `scene_home_target(const TerrainModel&, float*, float*)`
tested for: a code-only recording (centroid inside the code region's footprint), a
recording with no regions (returns false; caller keeps the default camera), and
byte-stability across two `build_terrain` calls on the same recording. The
"you are here" text is a pure function of `(Projection, target)` — golden-test it.

**Done when.** Reset frames a real landmark that does not move as a capture grows;
the HUD always states where the camera is pointed or why it cannot say.

### T5 — Advertise the controls (S)

**Goal.** The controls that exist stop being secret.

**Steps.**
1. `scene3d/hud.cpp`: a collapsible "controls" block listing, verbatim and
   complete: left-drag orbit, middle/shift-drag pan, wheel dolly, double-click
   recentre, click open-in-2D, hover inspect (once [47](47-scene-inspect-and-pickable-overlays.md)
   lands), and the arrow/`+`/`-`/`R`/`T` keys with their `CamKey` meanings.
   **Generate the key list from the `CamKey` enum**, not from a hand-written
   string — the review's finding is precisely that hand-written key lists drift
   from the bindings.
2. Add the pan and recentre lines to the scene primer's body
   ([shell.cpp:899-914](../../../desktop/src/ui/shell.cpp#L899)) — the primer
   already promises "3D to FIND", and this is the sentence that makes the promise
   operable.
3. If a control is disabled in the current state (no regions placed → goto has no
   targets), the row says why rather than vanishing (D7: graded, not hidden).

**Tests.** `test_shell.cpp` / the HUD test TU: assert the advertised key list
covers every `CamKey` value — an exhaustive switch or a static assert on the enum
count, so adding a `CamKey` without advertising it fails the build or the test.

**Done when.** Every wired control is named in the UI; adding a new one without
advertising it fails a test.

## Fidelity notes (D7)

- A goto never invents a destination. An unmapped address refuses; it is never
  clamped, snapped, or approximated to the nearest mapped cell.
- A region target frames the region's **real cell footprint**. A bounding box over
  a Hilbert layout necessarily includes cells belonging to other regions — framing
  one would show the user a rectangle the address space does not have.
- The "home" anchor is derived from recorded region placement, so it is stable
  across live growth by construction. Do not re-derive it per frame from the
  current slice; a home that drifts as events arrive is worse than a fixed centre.
- Region labels are used verbatim, including `code@0x<base>` for an unnamed span.
  No invented module names ([43](43-faithful-city-roadmap.md) Phase E is where real
  names come from, via a `maps` producer).

## Effort and risk

Five tasks, three small and two medium; all render/UI-side over pure models. The
main risk is control conflict — pan, orbit, pick, recentre and the existing
drag-suppression all share one `InvisibleButton`. T1/T2 keep plain left-drag and
single-click exactly as they are today and add only on unused gestures
(middle-drag, shift+drag, double-click), so no existing muscle memory or test
breaks.
