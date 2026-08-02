# The 3D as an instrument — a functional-improvement roadmap

> **Sources.** The three GUI analysis docs that concern the 3D overview:
> [2026-07-29-3d-visualization-catalog.md](../analysis/2026-07-29-3d-visualization-catalog.md)
> (14 layers + 12 scenes, §9's open limits),
> [2026-07-30-computer-as-city-3d.md](../analysis/2026-07-30-computer-as-city-3d.md)
> (the city metaphor that absorbs the catalog, §7's "where the metaphor strains"),
> and [2026-07-29-gui-ux-dataviz-review.md](../analysis/2026-07-29-gui-ux-dataviz-review.md)
> §4.6 + the affordance-discovery theme (§4.4) — the only one of the three that
> reviewed the scene as a *thing a person operates* rather than a thing that is
> drawn. Read [_conventions.md](../implementations/_conventions.md) first; D1–D11 live
> in this directory's [README](README.md).
>
> Authored 2026-08-02 against HEAD `f110150`. Every file:line below was
> re-verified against that tree while writing. If a citation disagrees with the
> code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Status — roadmap only, no tasks of its own.** The six briefs it cuts are
> [47](47-scene-inspect-and-pickable-overlays.md), [48](48-scene-navigation-and-goto.md),
> [49](49-one-time-truth-in-the-scene.md), [50](50-two-way-brushing.md),
> [51](51-scene-focus-and-scale.md) and [52](52-flat-terrain-surface.md). All six
> are cut and unclaimed.

## 1. Why this family exists, and how it differs from 43

[43-faithful-city-roadmap.md](43-faithful-city-roadmap.md) is the **representation**
axis: what the scene *depicts* (zoning, weather, districts, towers, seasons). Its
Phase A landed; B–E are large and mostly gated on new geometry, new producers, or
a new `space/city.{h,cpp}` aggregation module.

This family is the orthogonal **instrument** axis: what a person can *do* with the
scene. The distinction is not academic — it explains a real asymmetry the three
source docs share. The catalog and the city doc between them propose 33 concepts
and 44 city elements, and *every one of them is something to draw*. Neither doc
proposes a single new thing to **do**. The UX review is the only source that
looked at operation, and its 3D findings (#14, #37, #38, #40, #50, #55, #56, #58)
are almost entirely unaddressed — they were not absorbed by the city doc, which
says outright that it absorbs "that catalog's 14 layers + 12 scenes", not the
review.

The consequence, verified in the tree today: the 3D overview is a **faithful
picture you can orbit and click once**. Its stated contract is *"3D to FIND a
place, then the flat 2D views to READ it"*
([shell.cpp:907](../../../desktop/src/ui/shell.cpp#L907)) — and the FIND half is
the half that is missing. You cannot ask what is under the cursor before
committing, you cannot go to a named place, you cannot recentre on what you found,
you cannot get back to the scene from the 2D view you were sent to, and roughly
half the geometry on screen is not pickable at all.

These six briefs are cheap relative to 43's Phases B–E — five of the six are pure
render/UI-side work over models that already exist, none needs a producer change,
none needs a schema change, and none needs the instanced-building system. They
also *compose* with 43: nothing here conflicts with a later city phase, and two of
them (48's landmark navigation, 51's entity budget) are prerequisites the city doc
itself names in §7 ("Navigation ergonomics… does the operator drown in controls?"
and the occlusion/LOD question).

## 2. The gap table — verified 2026-08-02 against `f110150`

| # | Functional gap | Evidence in the tree today | What it costs | Brief |
|---|---|---|---|---|
| G1 | **No inspection before commitment.** A click immediately navigates away; nothing says what is under the cursor first. | [shell.cpp:1154-1177](../../../desktop/src/ui/shell.cpp#L1154) — `vp_hover` gates only orbit/dolly/pick; on release, `resolve_pick` → `dt_nav_go` with no preview step. | Every pick is a blind bet; a miss yanks the user into an unrelated 2D view and loses the camera framing they were building. | [47](47-scene-inspect-and-pickable-overlays.md) |
| G2 | **`resolve_pick` computes a rich answer and throws it away.** | [pick.cpp:52-119](../../../desktop/src/scene3d/pick.cpp#L52) derives `addr`, the owning `Region*`, the `CodeCell`/`DataCell` and the churn state — then returns only a `dt_link`. | The data for a hover readout already exists and is discarded once per pick. | [47](47-scene-inspect-and-pickable-overlays.md) |
| G3 | **Half the drawn geometry is un-pickable.** The pick pass draws terrain cells and PC vertices only. | [scene.cpp:776-794](../../../desktop/src/scene3d/scene.cpp#L776) draws `prog_pick_terrain_` + `vao_pts_`; `access_spurs_` and `conv_arcs_` ([scene.h:187-189](../../../desktop/src/scene3d/scene.h#L187)) are never drawn into the pick FBO. | Convergence arcs are the scene's one cross-thread finding and are dead overview objects — the city doc calls making them pick "the real deliverable". | [47](47-scene-inspect-and-pickable-overlays.md) |
| G4 | **The camera cannot go anywhere.** `target` is fixed; only `orbit`/`dolly` exist. | [camera.h:32,51-68](../../../desktop/src/scene3d/camera.h#L32) — `target[3] = {0.5,0,0.5}`; `reset()`/`top_down()` restore it. | After dollying in, an off-centre district cannot be studied: you orbit a point you do not care about. Directly blocks "3D to FIND". | [48](48-scene-navigation-and-goto.md) |
| G5 | **No way to name a destination.** No address entry, no region list → camera, no landmark. | No caller of `Projection::project` ([types.h:44](../../../desktop/src/space/types.h#L44)) exists in the camera/HUD path; the HUD's region legend ([hud.h:76](../../../desktop/src/scene3d/hud.h#L76)) is a swatch key, not a target list. | Finding a known address means orbiting and guessing. The plane is a *reproducible* address layout — a lookup is exact and trivially available. | [48](48-scene-navigation-and-goto.md) |
| G6 | **Camera controls are undiscoverable** (UX #38). | HUD exposes reset/top-down buttons only; orbit/dolly/`CamKey` ([camera.h:115-158](../../../desktop/src/scene3d/camera.h#L115)) are advertised nowhere. | The project's known "keymap advertises keys the UI never shows" pattern, inverted: here the UI shows nothing at all. | [48](48-scene-navigation-and-goto.md) |
| G7 | **Two contradictory time truths on screen at once.** The terrain slices to `[0,t]`; the worldline draws all of `[0,nsteps]`. | [shell.cpp:1002-1012](../../../desktop/src/ui/shell.cpp#L1002) re-slices on `hud.t`; `kTrajFrag` ([embedded.h](../../../desktop/src/scene3d/shaders/embedded.h)) has no time cut — only 44-T6's comet tail around `uHeadY`. | The primer promises "press Play to watch the path form" ([shell.cpp:912](../../../desktop/src/ui/shell.cpp#L912)); the path does not form. Worse, a user reading the two surfaces together reads a false simultaneity. | [49](49-one-time-truth-in-the-scene.md) |
| G8 | **Height is not readable as a quantity.** No contours, no scale reference, no key for the terrain's own encodings (UX #37/#40/#56). | `kTerrainFrag` applies a ramp + flag tints with no banding; the HUD legend lists region swatches only. | Terrain-Y is density and trajectory-Y is trace-time — two quantities sharing one axis with nothing labelling either. | [49](49-one-time-truth-in-the-scene.md) |
| G9 | **Navigation is one-way.** 3D → 2D works; 2D → 3D does not exist. | `Selection` ([selection.h:33-60](../../../desktop/src/ui/selection.h#L33)) never reaches `SceneFrame` ([scene_host.h:37-65](../../../desktop/src/ui/scene_host.h#L37)); `follow_step` free-runs on its own transport ([shell.h:91-110](../../../desktop/src/ui/shell.h#L91)). | You can find a place and read it, but never ask "where is the thing I am reading?" — the round trip the whole find/read split implies is half-built. | [50](50-two-way-brushing.md) |
| G10 | **The one existing 2D↔3D step mapping is an unchecked ordinal.** `resolve_pick` sets `link.step = pv.t`, where `pv.t` is a per-tid vertex counter. | [pick.cpp:136-139](../../../desktop/src/scene3d/pick.cpp#L136) vs. `p.t = next_t[tid]++` ([trajectory.cpp:99](../../../desktop/src/space/trajectory.cpp#L99) and again at :144); `dt_link::step` is documented as "a dataflow step index" ([nav.h:62](../../../desktop/src/nav.h#L62)). | For a multi-tid recording these axes cannot coincide by construction — the same mismatch 44 documented and deferred, already shipped in the *other* direction with no guard. | [50](50-two-way-brushing.md) |
| G11 | **No subject-based focus.** Layer toggles are fidelity-class only; you cannot isolate one thread, one region, or one kind. | `SceneLayers` ([scene.h:36-47](../../../desktop/src/scene3d/scene.h#L36)) is 9 bools, all class-wide; `Scene::Line` ([scene.h:171-185](../../../desktop/src/scene3d/scene.h#L171)) carries no `tid`. | On a multi-thread capture every path is drawn at equal weight; the one you care about cannot be brought forward. | [51](51-scene-focus-and-scale.md) |
| G12 | **Degradation is time-budgeted, never distance-budgeted.** | `kScrubCellBudget` / `should_degrade` / `coarse_slice` ([shell.cpp:1001-1012](../../../desktop/src/ui/shell.cpp#L1001)) handle a slow *slice*; nothing handles a dense *frame*. | Both source docs flag occlusion at scale as the top perf risk (catalog §9, city §7) with no mechanism proposed below Phase C/D. | [51](51-scene-focus-and-scale.md) |
| G13 | **No GL, no scene** (UX #58). | `s.scene_host == nullptr` → a text placard ([shell.cpp:1081-1091](../../../desktop/src/ui/shell.cpp#L1081)); `top_down()` still projects through `mat4x4_perspective` ([camera.h:88-94](../../../desktop/src/scene3d/camera.h#L88)), so even the "2D-ish" preset has perspective. | The whole spatial channel is unavailable headless, in the null harness, over plain SSH, and on a driver where shader build fails — and there is no perspective-free surface for exact reading anywhere. | [52](52-flat-terrain-surface.md) |

Two gaps in the source docs are deliberately **not** picked up here, because they
are representation work owned by [43](43-faithful-city-roadmap.md):
the instanced building system (city Phase D) and the `maps` producer (city Phase
E). Two more are recorded as out of scope for a different reason: the catalog's
**observed-data-span projection** (§4, its own stated single largest unknown) and
the **fibre gate** (§8.4) are producer/capture-layer gates, not instrument gaps.

## 3. Sequencing

```
47 inspect ──┐
48 navigate ─┼── independent, land in any order, highest value first
49 one-time ─┘
                     50 two-way brushing   (reads 47's hint helper; otherwise free)
                     51 focus & scale      (reads 48's landmarks; otherwise free)
                     52 flat surface       (largest; independent of all five)
```

- **47 and 48 first.** They are the two halves of "3D to FIND": 47 makes a
  candidate inspectable, 48 makes a destination reachable. Either alone is a real
  improvement; together they change what the pane is for.
- **49 is independent and small.** It is also the only brief that fixes something
  currently *misleading* rather than merely absent (G7), so it should not sit
  behind the other two if capacity is short.
- **50 needs a fidelity decision made carefully, not a lot of code.** Its whole
  weight is in refusing to fabricate a correspondence — see §4.
- **51 depends on 48** only for the landmark anchors its LOD tiers frame; it can
  be built first with a fixed anchor.
- **52 is the largest and can proceed fully in parallel** — it touches a new TU
  (`views/scene2d_draw.cpp`) and the null-backend branch, colliding with nothing.

## 4. The one fidelity decision this family turns on

Every brief here obeys the same four invariants the catalog states (§3) and the
city doc restates (§6) — statistical never merges into exact, truncation survives
the drill-in, unknown is not zero, no fabricated structure. One decision is new to
*this* family and is stated once here so the briefs need not re-derive it:

**Cross-axis brushing goes through the ADDRESS, never through an ordinal.**

The scene's plane is an address plane. `Selection.off` is a code offset in the
recording's basis — the same key `resolve_pick` already produces
([pick.cpp:90](../../../desktop/src/scene3d/pick.cpp#L90)) — so
`off → base+off → Projection::project → cell` is exactly invertible and states
nothing the recording did not. Where only a step is in hand, the recording itself
carries the bridge: `DataflowStream::insn_off[step]` plus `insn_rbase[step]`
([streams.h:68-76](../../../desktop/src/doc/streams.h#L68)) give that step's
address directly.

What is forbidden is the ordinal shortcut: treating `TrajPoint.t` (a per-tid
vertex counter) and `Selection.step` (a dataflow step index) as the same number
because they are both called a step. That is the trap 44 identified and correctly
declined ([shell.h:91-110](../../../desktop/src/ui/shell.h#L91)); G10 shows it is
already present, unguarded, in the reverse direction. [50](50-two-way-brushing.md)
is the brief that resolves both directions the same, address-first way — and where
even the address route is ambiguous (one offset executed at many steps), it
surfaces the ambiguity rather than picking a representative silently.

## 5. Cross-references

Extends [10-spacetime-3d-overview.md](../archive/gui/10-spacetime-3d-overview.md)
(the scene, camera, HUD and pick router this family operates on),
[34-playhead-and-scene-reach.md](../archive/gui/34-playhead-and-scene-reach.md)
(the two-axes rule 49 and 50 both hold to) and
[44-faithful-city-phase-a-mvp-terrain-reskin.md](../archive/gui/44-faithful-city-phase-a-mvp-terrain-reskin.md)
(the zoning/weather/vehicle substrate 47–51 read). Sibling roadmaps:
[43](43-faithful-city-roadmap.md) (representation) and
[38](38-live-feed-completion-roadmap.md) (the live feed). Fidelity chrome D7 /
[23](../archive/gui/23-graded-truth-layer.md); wording D7 /
[24](../archive/gui/24-one-visual-language.md).
