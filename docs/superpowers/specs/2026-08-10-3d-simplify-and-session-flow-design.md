# 3D simplify + the Session flow scene (design)

**Date:** 2026-08-10.
**Status:** approved design, ready for an implementation plan.
**Scope:** desktop/ only. Two deliverables: (1) a simplified reading posture
for the 3D pane, (2) a NEW scene kind — the **Session flow** — built from the
session-strip channels and deliberately shaped as smooth stacked ribbons,
because the existing per-event encodings read as "a bunch of spikes".

## Why this work exists

At Firefox scale the 3D pane drowns the same way the strip did: hundreds of
worldlines, and per-event vertical geometry everywhere — one access-mark spur
per `mem` event on the worldlines ([scene.cpp:915-919](../../../desktop/src/scene3d/scene.cpp#L915)),
one bar per cell in the relief/tide/pillars family (data_layers_gl.cpp states
"a spike per cell rather than a tessellated surface"), one segment per band
per column in sediment. The existing LOD ([lod.h](../../../desktop/src/scene3d/lod.h))
only drops layers past a 200k-entity budget or when dollying out; nothing caps
worldlines, and the standalone kinds have no LOD at all. And no scene in the
tree draws a **continuous aggregated surface over stream order** — the exact
form that reads well at scale (grep confirms: no streamgraph / stacked-area
geometry anywhere; the terrain heightfield and canopy quads are the only
triangle surfaces).

## Deliverable 1 — the 3D simplified posture

The strip's posture rule, transplanted (`strip.h:143-147` precedent): the
MODEL stays complete, the posture decides what is drawn, thresholds keep
small scenes byte-identical, and everything hidden is COUNTED on screen.

- **`HudState::detail`** (bool, default `false`) beside `kind`/`req_kind` —
  the 3D pane's reading-posture home. A `"detail"` / `"simplify"` SmallButton
  in the HUD next to the scene-kind selector (the strip's exact labels).
- **Worldline cap** — new pure `space::simplify_trajectories(const
  TrajectorySet &, size_t cap, SimplifyNote *)` keeping the
  `kSceneSimplifiedTrajs = 8` largest trajectories (by point count, ties
  ascending tid) IN MODEL ORDER; `SimplifyNote{hidden_threads,
  hidden_points}` carries the count. At or under the cap the returned set is
  the input, element for element. Applied in the weave when `!detail`; the
  eviction-safe substrate fingerprint gains `SceneView::woven_detail` so a
  posture flip drops `built` (camera survives — the growth-rebuild rule).
- **Spike-layer withholding** — new pure
  `scene3d::simplify_apply(SceneLayers, bool detail)` (lod_apply's shape:
  never turns a layer ON): when `!detail` it clears the per-event spike
  layers — `access_marks`, `data_relief`, `working_set`, `lifetime`,
  `sediment` — and leaves every aggregate (terrain, canopy, ridge,
  convergence, crossings) alone. Applied at frame build immediately before
  `lod_apply` (`shell.cpp:2115`), so LOD still degrades further when the
  camera and budget say so.
- **Honesty chrome** — `scene3d::simplify_note(...)` produces the placard
  line: `"simplified — top 8 of N worldlines (M points folded); access marks
  withheld — detail restores"`, shown in the HUD only when something was
  actually withheld (the strip's HUD rule). Pinned by test.

## Deliverable 2 — SceneKind::SessionFlow, "the session flow"

A new standalone scene kind over the SAME channels the session strip reads —
threads, syscalls, memory accesses, run seams — drawn as **smooth stacked
ribbon surfaces flowing along stream order**, not marks:

- **Axes** (unique Y per the scene_kind test): X = `"stream order (seq
  buckets)"`, Y = `"activity rate (events per bucket)"`, Z = `"session rows
  (threads · kernel · memory)"`, y_not = `"NOT time — stream order only; NOT
  a duration"`.
- **Rows** (depth-stacked ribbons, near → far): the top-8 thread lanes (the
  strip's `strip_selected_lanes`, tid palette hues), ONE aggregate lane row
  when anything hid (`"(+N lanes, M events)"` — counted, never vanished),
  ONE kernel row (height = syscalls per bucket, coloured by the bucket's
  DOMINANT `SyscallClass`, grey when tied/unknown), ONE memory row (height =
  accesses per bucket, split-shaded read/write).
- **Geometry** — per row, one triangle-strip ribbon: bucket heights
  (`kFlowBuckets = 192` across the stream) with midpoint smoothing for
  display; the HUD legend states, verbatim and pinned: `"heights are events
  per bucket of stream order, smoothed for display — not time, not
  duration"`. Zero-height stretches stay zero (a silent thread reads flat,
  never invented).
- **Run seams** — translucent full-height wall quads at each seam's bucket
  position, labelled in hover (`pass … / coverage close / capture N`).
- **Data path** — the crossing.h POD pattern (the clean precedent):
  `space/sessionflow.h` header-only POD (`SessionFlowScene{rows, seams,
  buckets, seq_end, notes…}`, each `FlowRow{kind, label, hue key, heights[],
  events}`), built by a new `views/strip_flow.cpp`:
  `space::SessionFlowScene build_session_flow(const StripModel &)` — reusing
  `strip_selected_lanes` and the classify enums, so the flow and the strip
  can never disagree about lanes or classes. scene3d consumes the space/ POD
  only (unlike standalone.h's existing views/ includes — new code follows
  the stated-clean pattern).
- **Registration** — the full `ed8ce647` checklist: enum + `all_scene_kinds`
  + name + axes; model member on SceneView; weave in
  `shell_weave_standalone` (builds the StripModel via the same
  `strip_build`/`shell_assemble_regions` call the strip tab uses);
  availability = the strip's presence rule (any of
  mem/syscall/trace/call/watch/df_step, verbatim reason); pick order = rows
  then seams (hover names the row + its counts; links: kernel/lane rows →
  the syscalls view (pid when the lane's tgid is known), memory row → the
  timeline, seams hover-only); per-kind chrome legend (row palette + the two
  strip-pinned notes + the bucket/smoothing note); default camera
  (`target[1] > 0`, framed like ModuleRibbon's); `StandaloneRenderer` gains
  a triangle batch (`tris_`) for the ribbon surfaces + seam walls beside its
  existing line/pick batches; screenshot name + `shots.json` entry
  (test_shot_manifest requires it); mk link lines.

## Approaches considered

- **Chosen** — posture + new smooth-ribbon kind, as above.
- **Rejected: geometry-LOD the existing plane** (merged meshes/instancing —
  lod.h explicitly defers this to 43 Phase D; enormous GL surface for a
  readability problem the posture + new scene solve directly).
- **Rejected: retro-fit smoothing onto relief/sediment** — those layers'
  claims are per-cell/per-band by design; smoothing them would fabricate
  continuity where the data is discrete. The NEW scene aggregates rates,
  which smoothing presents honestly (a stated bucket rate).

## Testing

- Pure: `simplify_trajectories` (cap, ties, under-threshold identity, note
  counts) in test_trajectory; `simplify_apply`/`simplify_note` (never-ON,
  exact layer set, pinned strings) in a new test_simplify (test_camera's
  harness style); `build_session_flow` (row set, aggregate row, dominant
  class, heights sum to event counts before smoothing, seam positions, dump
  determinism) in test_standalone's harness; scene_kind exhaustiveness
  (axes/name/camera/pick-band tests iterate all kinds automatically).
- shots.json entry (test_shot_manifest gate).
- GL: docker-desktop's FBO lane must stay green; the new kind's upload is
  exercised by the shot machinery and the round-trip invariants already
  pinned for standalone kinds.

## Out of scope

- No Settings field, no persistence of `detail` (either pane).
- No producer/wire change; no change to existing layers' geometry.
- No LOD for Divergence/Invocation/ModuleRibbon/LanePrism (unchanged).
