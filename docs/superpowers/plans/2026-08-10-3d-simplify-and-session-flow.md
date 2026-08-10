# 3D Simplify + Session Flow Scene Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A simplified reading posture for the 3D pane (top-8 worldlines + spike-layer withholding, counted and reversible), and a new SceneKind::SessionFlow — smooth depth-stacked ribbon surfaces of thread/kernel/memory activity rates over stream order with run-seam walls — replacing "a bunch of spikes" with flowing aggregate forms.

**Architecture:** Posture: `HudState::detail` + pure `space::simplify_trajectories` (weave-side cap) + pure `scene3d::simplify_apply` (frame-side layer withholding, lod_apply's shape) + `SceneView::woven_detail` fingerprint. New kind: the crossing.h POD pattern — `space/sessionflow.h` built by `views/strip_flow.cpp` from `StripModel` (reusing `strip_selected_lanes` + classify enums) — registered through the full `ed8ce647` standalone-kind checklist, with a new triangle batch in `StandaloneRenderer` for ribbon surfaces.

**Tech Stack:** C++17, OpenGL (standalone_gl), null-backend-testable pure halves, `mk/desktop.mk`, `make desktop-test` + authoritative `make docker-desktop`.

**Spec:** `docs/superpowers/specs/2026-08-10-3d-simplify-and-session-flow-design.md`.

## Global Constraints

- Model stays complete everywhere; postures/scenes draw aggregates. Hidden = COUNTED on screen, verbatim pinned strings; at/below `kSceneSimplifiedTrajs = 8` the posture is a no-op (identity).
- `simplify_apply` NEVER turns a layer on; clears exactly `{access_marks, data_relief, working_set, lifetime, sediment}` when `!detail`.
- SessionFlow's pinned notes: axis `"stream order — not time"`; mem `"mem carries no tid — access marks are r/w-hued, never thread-hued"` (both from StripModel statics); new `"heights are events per bucket of stream order, smoothed for display — not time, not duration"`.
- test_scene_kind's automatic gates for a new kind: unique name, unique Y-axis string, `y_not` containing "not", camera `target[1] > 0`, pick bands non-colliding; `shots.json` entry required by test_shot_manifest.
- scene3d consumes `space/` PODs only for NEW code (crossing.h pattern; do not add more views/ includes to scene3d).
- Shared-tree discipline: detached worktree off origin/main; path-scoped adds; push per task (`git push origin HEAD:main`, rebase on reject); guard every test run's exit code before committing.

---

### Task 1: the flow model — `space/sessionflow.h` + `views/strip_flow.cpp`

**Interfaces produced:**
```cpp
// space/sessionflow.h (header-only POD)
namespace asmdesk::space {
inline constexpr uint32_t kFlowBuckets = 192;
enum class FlowRowKind : uint8_t { Lane, AggregateLanes, Kernel, Memory };
struct FlowRow {
    FlowRowKind kind; std::string label;
    int64_t tid; long tgid; uint32_t lane_ord;   // palette hue for Lane rows
    std::vector<float> heights;                  // kFlowBuckets smoothed rates
    std::vector<uint64_t> counts;                // RAW per-bucket counts (pre-smoothing)
    std::vector<uint8_t> bucket_class;           // Kernel row: dominant SyscallClass+1 per bucket, 0 = none/tie
    uint64_t events;                             // the row's total
};
struct FlowSeam { uint32_t bucket; std::string label; uint8_t kind; };
struct SessionFlowScene {
    std::vector<FlowRow> rows;   // near→far: lanes, aggregate?, kernel, memory
    std::vector<FlowSeam> seams;
    uint64_t seq_end; uint32_t buckets = kFlowBuckets;
    bool enabled; std::string disabled_reason;   // never empty when disabled
    static const char *smoothing_note();         // the pinned display claim
};
}
// views/strip_flow.h: space::SessionFlowScene build_session_flow(const StripModel &m);
// + std::string session_flow_dump(const space::SessionFlowScene &);
```
**Build rules:** rows from `strip_selected_lanes(m, /*detail=*/false)` (top-8 + aggregate); per-row raw counts by bucketing that lane's activity seqs into `kFlowBuckets` over `[0, seq_end)`; kernel row from `m.sys` (dominant class per bucket: strict majority of the bucket's classes, else 0); memory row from `m.mem`; heights = `log1p(count)` then ONE midpoint-smoothing pass `h'[i] = 0.25h[i-1] + 0.5h[i] + 0.25h[i+1]` (ends clamped), normalized to max 1.0 per SCENE (not per row — relative rates must compare across rows); rows with zero events are dropped EXCEPT lanes (a kept silent lane draws flat); seams from `m.seams` mapped to buckets; `enabled=false` + reason when `m.seq_end == 0` or every channel empty.

**Steps:** failing pure tests in `desktop/test/test_standalone.cpp`'s harness style but in a NEW `desktop/test/test_session_flow.cpp` (mk: DESKTOP_TESTS + link `vw/strip_flow.o vw/strip.o src/nav.o $(DESKTOP_TEST_DOC)`): row set/order over a synthetic StripModel (12 lanes → 8 + aggregate + kernel + memory), raw `counts` sum == the channel totals (smoothing never changes `counts`), dominant-class rule incl. tie→0, seam bucket mapping, under-threshold identity (3 lanes → 3 lane rows, no aggregate), disabled reason, dump determinism. Implement. Run. Commit + push (`desktop/space: session flow POD + builder — smooth bucketed rates over stream order`).

---

### Task 2: the 3D simplified posture

**Files:** `space/trajectory.h/.cpp` (+`SimplifyNote`, `simplify_trajectories`), new `scene3d/simplify.h` (header-only: `simplify_apply`, `simplify_note`, `kSceneSimplifiedTrajs = 8`), `scene3d/hud.h` (`HudState::detail`), `scene3d/hud.cpp` (button beside the kind selector), `ui/shell.h` (`SceneView::woven_detail`), `ui/shell.cpp` (weave applies the cap when `!detail` + fingerprint drop + `simplify_apply` before `lod_apply` at the frame build + carry `detail` across growth rebuilds with the camera), tests: `test_trajectory.cpp` (cap/ties/identity/note), new `test_simplify.cpp` (never-ON, exact clear set, pinned note strings; mk wiring).

```cpp
// space/trajectory.h
struct SimplifyNote { size_t hidden_threads = 0; uint64_t hidden_points = 0; };
TrajectorySet simplify_trajectories(const TrajectorySet &t, size_t cap, SimplifyNote *note);
// scene3d/simplify.h
inline constexpr size_t kSceneSimplifiedTrajs = 8;
SceneLayers simplify_apply(SceneLayers in, bool detail);   // clears the 5 spike layers when !detail
std::string simplify_note(bool detail, const space::SimplifyNote &n, bool access_marks_were_on);
```
`simplify_note` returns "" when detail OR nothing withheld; else the spec's placard line with real counts. Weave: `sv.woven_detail != sv.hud.detail → built=false` (beside the woven_union check); when weaving with `!detail`, `traj = simplify_trajectories(traj, kSceneSimplifiedTrajs, &sv.simplify)` (store the note on SceneView). Frame: `f.layers = lod_apply(simplify_apply(sv.hud.layers, sv.hud.detail), sv.hud.lod)`. HUD: SmallButton `detail`/`simplify` sets `s.detail` directly (the shell's fingerprint reweaves); TextDisabled the note line when non-empty. Commit + push (`desktop/scene3d: simplified posture — top-8 worldlines, spike layers withheld, counted`).

---

### Task 3: SceneKind::SessionFlow registration (pure half)

Follow `ed8ce647`'s checklist verbatim, all in one task since the pieces only compile together: enum (append after LanePrism) + `all_scene_kinds` + `scene_kind_name("Session flow")` + `scene_axes` (spec's strings; `-Wswitch` forces the case) + `standalone_default_camera` case (frame like ModuleRibbon, `pitch 2.5/0.5/0.4`-ish, `target[1] = 0.4f`) + `SceneView::flow` member (`space::SessionFlowScene`) + weave in `shell_weave_standalone` (build the StripModel exactly as `shell_strip_body` does — `strip_build(*src.rec, shell_assemble_regions(...), seams)` — then `sv.flow = build_session_flow(sm)`) + availability row (the strip's kind-set rule + verbatim reason) + `flow_pick_order(const SessionFlowScene&)` (rows then seams) and `flow_pick_link` (Lane/AggregateLanes/Kernel → `dt_view::syscalls` (+pid when the lane's tgid known), Memory → `dt_view::timeline`, seams → no link) in `views/strip_flow.{h,cpp}` + `shell_standalone_pick` case (hover: row label + `N events` / seam label) + `shell_standalone_chrome` case (legend: tid palette note, kernel class hues, the three pinned notes, `buckets: 192`) + `SceneFrame::flow` + fill. Tests: test_scene_kind passes automatically (unique name/Y/y_not/camera/pick bands — pick band count from `flow_pick_order`); extend test_session_flow with pick order/link checks. Commit + push (`desktop/scene3d: SceneKind::SessionFlow — registration, pick, chrome (pure half)`).

---

### Task 4: the GL half + screenshot manifest

`standalone_gl.h/.cpp`: add `std::vector<Vtx> tris_;` batch + `build_session_flow_geometry()` — per row, a triangle strip over bucket midpoints (two verts per bucket edge: baseline `y=row_z_base`… actually X = bucket→u, Y = height, Z = row index → depth; emit as triangles into `tris_`), per-row colour (tid palette / class hue / rw duotone / grey aggregate), seam walls as translucent quads, pick ids per row/seam matching `flow_pick_order`; `upload` switch case; draw the `tris_` batch with depth test as the existing renderer does; `StandaloneFrame::flow` pointer + `gl_scene_host.cpp` fill. `ui/shot.cpp` name map `"session-flow"` + `desktop/shots.json` entry (copy an existing standalone entry's shape). mk: app/viewer/test link lines for `vw/strip_flow.o`. Verify: `test_shot_manifest` green; app + viewer link; `test_scene_fbo` untouched-green. Commit + push (`desktop/scene3d: SessionFlow GL ribbons + seam walls + shot manifest`).

---

### Task 5: full verification

`make desktop-test` (exit-guarded) → 0 new failures; `make docker-desktop` → exit 0 (GL/FBO + ui-test + Xvfb); confirm every commit is an ancestor of origin/main; fast-forward the main checkout; remove the worktree.
