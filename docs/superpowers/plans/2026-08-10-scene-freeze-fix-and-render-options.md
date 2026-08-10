# Scene Freeze Fix + Render Options Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Un-freeze the 3D scene (`project()` broke under `keep_order` — CONFIRMED by probe), keep playback smooth on large weaves, make panning zoom-proportional and view-relative, and add a Settings GPU toggle.

**Architecture:** `Projection` gains a base-sorted index vector so the *domain order* (append-only under `keep_order`) and the *address-lookup order* (binary search in `project()`) decouple — both stay O(log n). Playback smoothness is a pure hold-coarse predicate beside `should_degrade`: while playing, an over-budget scrub never lands the full slice; pausing lands it. Panning routes both mouse and keyboard through a new `Camera::pan_view` (yaw-rotated, identity at yaw=0) with the keyboard step scaled by radius exactly as the mouse already is. The GPU toggle rides the pane's existing no-GL degraded branch.

**Tech Stack:** C++17 desktop tree, hand-rolled `check()` harness (NO gtest), `mk/desktop.mk` link lines (no new TUs — all changes land in existing files/headers).

## Global Constraints

- Plane expansion: the existing `order` growth (clamped [6,12], byte-coarsening beyond) plus the reflow notice IS the sanctioned expansion path; do NOT raise the 12 ceiling (order 13 = 268MB/layer per-cell allocations in `build_terrain`).
- Shared tree: private `GIT_INDEX_FILE` + `write-tree`/`commit-tree` + push per task; path-scoped everything; verify each touched test binary INDIVIDUALLY.
- `test_camera:111` pins radius with EXACT `==` — pan changes must not touch radius math.
- Default-path behavior must stay byte-identical: sorted-mode `build_projection`, non-playing scrub degrade, yaw-0 pan, `use_gpu=true`.

---

### Task 1: `project()` correct under `keep_order` (the freeze)

**Files:**
- Modify: `desktop/src/space/types.h` (Projection struct ~78), `desktop/src/space/projection.cpp` (`build_projection` ~222, `Projection::project` ~560)
- Test: `desktop/test/test_projection.cpp`

**Interfaces:**
- Produces: `std::vector<uint32_t> Projection::by_base;` — indices into `regions`, sorted by `base`. Built by `build_projection` in BOTH modes. `project()` binary-searches through it; `domain_off`/atlas rects stay domain-ordered and untouched.

- [ ] **Step 1: Failing test** (this is the probe that confirmed the bug):

```cpp
{
    // keep_order + project(): the freeze regression. Domain order is the
    // caller's, but an ADDRESS must still resolve — project() searches a
    // base-sorted index, not the domain order.
    Region code; code.base = 0x2000; code.len = 64;  code.kind = Region::Code;
    Region data; data.base = 0x1000; data.len = 128; data.kind = Region::Unknown;
    const Projection p = build_projection({code, data}, /*keep_order=*/true);
    float u = 0, v = 0;
    check("keep_order: high-base region still places",
          p.project(0x2010, &u, &v),
          "the region out of sorted position must not vanish from lookup");
    uint64_t back = 0; const Region *r = nullptr;
    check("keep_order: placed cell round-trips to the SAME region",
          p.project(0x2010, &u, &v) &&
              p.unproject(u, v, &back, &r) && r != nullptr &&
              r->base == 0x2000,
          "u,v must land in the code region's own cells");
    check("keep_order: low-base region places too",
          p.project(0x1008, &u, &v), "both orders must resolve");
}
```

(Adapt `unproject` to its real signature in the file — read its declaration first; if it returns by struct, assert the region base through that.)

- [ ] **Step 2: Run to verify it fails** — `make build/desktop_test_projection` → first check FAILS ("region out of sorted position must not vanish").
- [ ] **Step 3: Implement.** `types.h`: add `by_base` after `domain_off`, comment: lookup order vs domain order, why they diverge under keep_order. Update the `regions` field comment ("sorted by base" → "domain order; base-sorted lookup via by_base"). `build_projection`: after `p.regions = std::move(regions);` fill `by_base` with 0..n-1 sorted by `regions[i].base`. `project()`: replace the direct binary search over `regions` with the same search over `by_base` indirection (`regions[by_base[mid]].base <= addr`); resolve `idx = by_base[lo-1]` and use `idx` for `rects[idx]`/`domain_off[idx]` exactly where `lo-1` was used.
- [ ] **Step 4: Grep for other sorted-order assumptions** — `grep -n "regions\[" desktop/src/space/projection.cpp` and check the rel→abs anchor + `unproject` paths only ever index via domain search results (they do — verify, don't assume).
- [ ] **Step 5: Verify** — `desktop_test_projection`, `desktop_test_terrain`, `desktop_test_vmmap`, `desktop_test_shell` all green individually.
- [ ] **Step 6: Commit + push** — `desktop/space: project() searches a base-sorted index — fixes the keep_order scene freeze`.

---

### Task 2: playback holds the coarse plane (animation smoothness)

**Files:**
- Modify: `desktop/src/ui/progress.h` (~80, beside `should_degrade`), `desktop/src/ui/shell.cpp` (slice block ~1800)
- Test: `desktop/test/test_progress.cpp`

**Interfaces:**
- Produces: `inline bool scrub_hold_coarse(bool playing, bool pending);` in `progress.h` — true when the over-budget slice must stay coarse this frame: `playing || !pending`. (Not playing: coarse first frame, full lands next — today's behavior, unchanged. Playing: always coarse, so a running animation never pays the full re-slice per frame; the full slice lands on the first frame after pause/stop.)

- [ ] **Step 1: Failing test** in `test_progress.cpp` (match its harness):

```cpp
check("scrub: paused first frame degrades", scrub_hold_coarse(false, false),
      "an over-budget scrub still shows coarse for the in-flight frame");
check("scrub: paused second frame lands full", !scrub_hold_coarse(false, true),
      "the full slice must land one frame later when not playing");
check("scrub: playing always holds coarse",
      scrub_hold_coarse(true, false) && scrub_hold_coarse(true, true),
      "playback must never pay the full re-slice per animated frame");
```

- [ ] **Step 2: Run to verify failure** (no such symbol).
- [ ] **Step 3: Implement** the predicate with a doc comment naming the intent (playback speed is wall-clock via transport_tick; this keeps the per-frame COST bounded so the frame rate holds too). In `shell.cpp`, restructure the gate:

```cpp
const uint64_t cells = sv.terr.code.size() + sv.terr.data.size();
const bool over = should_degrade(cells, kSceneCellBudget);
if (over && scrub_hold_coarse(sv.play.playing, sv.scrub_pending)) {
    sv.slice = sv.terr.coarse_slice(); // cheap, labelled coarse
    sv.scrub_pending = true;           // full slice lands when eligible
} else { /* existing full-slice body, unchanged */ }
```

- [ ] **Step 4: Verify** — `desktop_test_progress` green; `desktop_test_shell` green (the degrade note text path unchanged).
- [ ] **Step 5: Commit + push** — `desktop/ui: playback holds the coarse plane — full slice lands on pause`.

---

### Task 3: pan — zoom-proportional keyboard step + view-relative panning

**Files:**
- Modify: `desktop/src/scene3d/camera.h` (`Camera::pan_view` + `camera_key` pan cases), `desktop/src/ui/shell.cpp` (mouse pan call ~2242)
- Test: `desktop/test/test_camera.cpp`

**Interfaces:**
- Produces: `void Camera::pan_view(float du, float dv);` — rotates the (du, dv) plane delta by `yaw` about Y then applies `pan()`: `du' = du*cos(yaw) + dv*sin(yaw); dv' = -du*sin(yaw) + dv*cos(yaw)` (identity at yaw=0, so today's default-view feel is preserved; pick the sign pair that makes yaw=0 EXACTLY equal `pan(du, dv)` and a positive yaw rotate the pan the same way it rotates the eye — derive from `eye()`'s own trig in the file, don't trust this formula blind).
- `camera_key` pan cases: step becomes `kPanStepPerRadius * c.radius` with `kPanStepPerRadius = 0.0182f` (≈ today's 0.04 at the default radius 2.2), routed through `pan_view`.
- Mouse pan (shell.cpp): `sv.cam.pan_view(-io.MouseDelta.x * scale, -io.MouseDelta.y * scale);` — same deltas, same radius scale, now view-relative.

- [ ] **Step 1: Failing tests** in `test_camera.cpp` (match harness; do NOT touch radius pins):

```cpp
{
    // pan_view: identity at yaw=0, view-rotated otherwise.
    Camera a; a.yaw = 0.0f; a.target[0] = 0.5f; a.target[2] = 0.5f;
    Camera b = a;
    a.pan(0.1f, 0.02f); b.pan_view(0.1f, 0.02f);
    check("pan_view at yaw=0 IS pan",
          a.target[0] == b.target[0] && a.target[2] == b.target[2],
          "the rotation must be the identity at yaw 0");
    Camera c; c.yaw = 1.5707963f; c.target[0] = 0.5f; c.target[2] = 0.5f;
    c.pan_view(0.1f, 0.0f);
    check("pan_view at yaw=pi/2 rotates onto the other axis",
          std::fabs(c.target[0] - 0.5f) < 1e-4f &&
              std::fabs(c.target[2] - 0.5f) > 0.05f,
          "a quarter-turn view must map screen-right onto the other plane axis");
    // keyboard pan scales with zoom: same key, double radius, double distance.
    Camera near_c; near_c.radius = 1.0f;
    Camera far_c;  far_c.radius = 2.0f;
    camera_key(near_c, CamKey::PanRight);
    camera_key(far_c, CamKey::PanRight);
    check("keyboard pan is zoom-proportional",
          std::fabs((far_c.target[0] - 0.5f) -
                    2.0f * (near_c.target[0] - 0.5f)) < 1e-5f,
          "a dollied-out camera must pan farther per press, like the mouse");
}
```

- [ ] **Step 2: Run to verify failure** (no `pan_view`).
- [ ] **Step 3: Implement** `pan_view` (verify the rotation sign against `eye()`'s trig so screen-right stays screen-right), rewire `camera_key` pan cases, rewire the mouse call.
- [ ] **Step 4: Verify** — `desktop_test_camera` green (including the pre-existing exact pins), `desktop_test_goto` + `desktop_test_focus` green (they consume Camera).
- [ ] **Step 5: Commit + push** — `desktop/scene3d: view-relative pan; keyboard pan scales with zoom`.

---

### Task 4: Settings GPU toggle

**Files:**
- Modify: `desktop/src/ui/settings.h`, `desktop/src/ui/settings.cpp`, `desktop/src/ui/shell.cpp` (settings pane "3D scene" section; degraded branch ~1960)
- Test: `desktop/test/test_settings.cpp`

**Interfaces:**
- Produces: `bool Settings::use_gpu = true;` serialized as `"use_gpu"`, absent key → true. Pane behavior: `!use_gpu` takes the SAME body as the no-GL branch (flat 2D surface for Plane, honest "no flat form" text for standalone kinds, keyboard focus target kept) with its own message naming the Settings toggle as the cause — a user choice, not a missing capability.

- [ ] **Step 1: Failing test** in `test_settings.cpp`:

```cpp
Settings gd;
check("gpu defaults ON", gd.use_gpu, "GPU rendering starts checked");
Settings gs; gs.use_gpu = false; Settings gb;
check("gpu toggle round-trips",
      settings_parse(settings_serialize(gs), gb) && !gb.use_gpu,
      "use_gpu must persist");
Settings gl2;
check("gpu absent key stays ON", settings_parse("{}", gl2) && gl2.use_gpu,
      "an old store must not disable the GPU path");
```

- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** — struct field + (de)serialization; pane checkbox under "3D scene" ("Use GPU for the 3D viewport") + `settings_dirty`; in `draw_scene_overview` change the degraded gate to also fire on `!s.settings.use_gpu`, with the message split by cause (`scene_host == nullptr` → existing text; setting off → "GPU rendering is OFF (Settings ▸ 3D scene) — flat 2D surface below").
- [ ] **Step 4: Verify** — `desktop_test_settings`, `desktop_test_shell` green.
- [ ] **Step 5: Commit + push** — `desktop/ui: Settings toggle for GPU 3D rendering (default ON)`.

## Explicitly out of scope

- Raising the plane-order ceiling past 12 (memory: 4^13 cells ≈ 268MB/layer). The plane already expands via `order` growth + byte-coarsening + reflow notice; Task 1 makes that path correct under keep_order.
- A playback speed slider (Transport::steps_per_sec stays 8.0) — natural follow-up, not asked for.
- Moving the weave itself onto the GPU — the toggle governs the existing GL render path honestly; a compute-shader weave is a separate design.

## Self-Review notes

- Coverage: freeze fix (T1), animation that does not slow down (T2 — cost-bounding; rate was already wall-clock), panning options (T3 — zoom-proportional + view-relative, both funnels), GPU option in settings (T4), "plane can be expanded if needed" (existing order growth kept correct, ceiling deliberately unraised).
- Types consistent: `by_base` (T1) is internal to Projection; `scrub_hold_coarse(bool, bool)` used identically in T2 test and wiring; `pan_view(float, float)` in T3 test, camera_key, and mouse call.
- Defaults: every task's OFF/default path is pinned by an explicit test asserting today's behavior.
