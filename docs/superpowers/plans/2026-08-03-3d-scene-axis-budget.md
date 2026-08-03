# 3D scene axis budget — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Re-encode the desktop 3D overview so its three spatial axes carry three decodable quantities — a 100 %-packed region atlas on the floor, access density on Y, and time moved out of the spatial budget into playhead animation.

**Architecture:** A new `Layout::Atlas` mode on `space::Projection` replaces the Hilbert curve with a squarified treemap of regions (serpentine within each), keeping `project()`/`unproject()` signatures so existing callers compile unchanged. The trajectory stops mapping trace step to world Y and becomes a playhead-driven PC comet. An optional `SceneLayers` motif layer adds opcode-class and syscall-family colour, reusing classifiers that already exist.

**Tech Stack:** C++17, `desktop/src/space/` (pure, engine-free — no GL, no ImGui), `desktop/src/scene3d/` (GL), `desktop/src/ui/` (ImGui), the null test harness under `desktop/test/`.

**Spec:** [2026-08-03-3d-scene-axis-budget-design.md](../specs/2026-08-03-3d-scene-axis-budget-design.md)

## Global Constraints

- **D4 — purity.** `space/` includes no GL, no ImGui, no engine header. Anything added there must compile into both desktop binaries *and* the null test harness. `scene3d/layers.h` may include `scene3d/scene.h` only.
- **D7 — no fabricated structure.** No inferred layout weighting, no guessed classification. `OpClass::Unknown` and `SyscallClass::Other` abstain visibly; they are never folded into a neighbouring class.
- **Treemap area is proportional to `Region::len`.** Any other weighting is a fabricated emphasis. An equal-area mode, if ever added, must be user-selected and labelled.
- **`SceneLayers` is exhaustive by test.** Every bool needs exactly one `LayerDesc` row in `scene_layers_all()` or `test_layers.cpp` fails by name.
- **Statistical never merges into exact.** A `TF_STAT` surface stays in its own `Terrain`, drawn in separate ink with a persistent `STATISTICAL — survey` label.
- **Build/verify via Docker.** `make docker-desktop-test`, `make docker-fmt-check`. Never `make X >/dev/null 2>&1` — it hides compile errors and leaves a stale binary "passing".
- **clang-format 18 is canonical.** Use `make docker-fmt`. `desktop/` addon includes in `shell.cpp` are order-sensitive — keep the existing fence comments; a bare `clang-format -i` re-sorts them into an `imgui_internal.h #error`.
- **Shared tree.** Many agents work this repo concurrently. Commit by explicit path, never `git add -A`.

---

### Task 1: Clamp the runaway worldline height (standalone defect fix)

Lands first and independently of the re-encoding, so the live defect is fixed regardless of when the substrate work completes.

**Files:**
- Modify: `desktop/src/scene3d/scene.cpp:797-802`
- Test: `desktop/test/test_scene_traj.cpp` (create if absent)

**Interfaces:**
- Consumes: `space::TrajectorySet`, `space::Projection` (unchanged).
- Produces: no signature change. `Scene::traj_scale_` becomes bounded such that every emitted vertex Y lies in `[0, 0.6]`.

- [ ] **Step 1: Write the failing test**

```cpp
// A `mem` stream can outlast the trace's own step count (sediment.cpp:33-37).
// The worldline must not escape its 0.6 world-unit envelope when it does.
TEST(scene_traj, mem_step_past_nsteps_stays_in_envelope) {
    space::TrajectorySet ts;
    space::Trajectory tr;
    tr.points.push_back({/*addr=*/0x1000, /*t=*/0, /*is_access=*/false, /*placed=*/true});
    // t far beyond the terrain's stated extent
    tr.points.push_back({/*addr=*/0x1000, /*t=*/100000, /*is_access=*/false, /*placed=*/true});
    ts.trajectories.push_back(tr);

    Scene s;
    s.nsteps = 10; // terrain under-reports
    const float scale = scene_traj_scale(s.nsteps, /*max_t=*/100000, s.time_scale);
    EXPECT_LE(100000.0f * scale, 0.6f + 1e-4f);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make docker-desktop-test TEST=test_scene_traj`
Expected: FAIL — `scene_traj_scale` not declared.

- [ ] **Step 3: Extract the scale rule and apply sediment's guard**

Add to `desktop/src/scene3d/scene.h`, beside the `time_scale` member:

```cpp
// The worldline's world-Y per trace step. `max_t` is the largest TrajPoint.t
// actually present. nsteps is the TERRAIN's extent and can UNDER-REPORT it —
// a `mem` stream can outlast the trace's own step count (the same mismatch
// space/sediment.cpp:33-37 already guards). Extending the denominator rather
// than clamping the data keeps every real step on the axis.
float scene_traj_scale(uint32_t nsteps, uint64_t max_t, float time_scale);
```

In `desktop/src/scene3d/scene.cpp`:

```cpp
float scene_traj_scale(uint32_t nsteps, uint64_t max_t, float time_scale) {
    if (time_scale > 0.0f)
        return time_scale;
    const uint64_t top = std::max<uint64_t>(nsteps, max_t + 1);
    return top > 0 ? 0.6f / static_cast<float>(top) : 0.02f;
}
```

Then in `Scene::set_trajectories`, replace lines 800-801 with a pre-pass for `max_t` followed by:

```cpp
uint64_t max_t = 0;
for (const space::Trajectory &tr : ts.trajectories)
    for (const space::TrajPoint &pt : tr.points)
        max_t = std::max(max_t, pt.t);
const float scale = scene_traj_scale(nsteps, max_t, time_scale);
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make docker-desktop-test TEST=test_scene_traj`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add desktop/src/scene3d/scene.cpp desktop/src/scene3d/scene.h desktop/test/test_scene_traj.cpp
git commit -m "scene3d: bound the worldline envelope when a mem step outlasts nsteps"
```

---

### Task 2: The atlas layout in `space::Projection`

**Files:**
- Modify: `desktop/src/space/projection.h`, `desktop/src/space/projection.cpp:95-110` (order selection), `:335-370` (project/unproject)
- Test: `desktop/test/test_projection.cpp`

**Interfaces:**
- Consumes: `Projection::regions`, `Projection::domain_off` (both unchanged).
- Produces: `enum class Projection::Layout { Hilbert, Atlas }`, a `Layout layout` member defaulting to `Hilbert`, and `std::vector<AtlasRect> rects` populated only for `Atlas`. `project(addr,&u,&v)` and `unproject(u,v,&addr)` keep their current signatures and round-trip contract under both layouts.

```cpp
// One region's rectangle on the unit floor, in the SAME index order as
// Projection::regions, so a caller joins them by index with no second key.
struct AtlasRect { float u0, v0, u1, v1; };
```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(projection_atlas, packs_the_whole_floor) {
    Projection p = build_test_projection({{0x1000, 4096, Region::Code},
                                          {0x8000, 61440, Region::Mmap}});
    p.layout = Projection::Layout::Atlas;
    rebuild_layout(p);
    float area = 0.0f;
    for (const AtlasRect &r : p.rects)
        area += (r.u1 - r.u0) * (r.v1 - r.v0);
    // 100% packed by construction — this is the "only 1/4 of the floor" fix.
    EXPECT_NEAR(area, 1.0f, 1e-4f);
}

TEST(projection_atlas, area_is_proportional_to_len) {
    // 4096 : 61440 == 1 : 15, so the code rect must be 1/16 of the floor.
    Projection p = build_test_projection({{0x1000, 4096, Region::Code},
                                          {0x8000, 61440, Region::Mmap}});
    p.layout = Projection::Layout::Atlas;
    rebuild_layout(p);
    const AtlasRect &code = p.rects[0];
    EXPECT_NEAR((code.u1 - code.u0) * (code.v1 - code.v0), 1.0f / 16.0f, 1e-4f);
}

TEST(projection_atlas, round_trips_every_region_base) {
    Projection p = build_test_projection({{0x1000, 4096, Region::Code},
                                          {0x8000, 61440, Region::Mmap}});
    p.layout = Projection::Layout::Atlas;
    rebuild_layout(p);
    for (const Region &rg : p.regions) {
        float u = 0, v = 0;
        ASSERT_TRUE(p.project(rg.base, &u, &v));
        uint64_t back = 0;
        ASSERT_TRUE(p.unproject(u, v, &back));
        // Same cell, so the same region — cell quantisation means the exact
        // address need not survive, but the region must.
        EXPECT_EQ(region_index_of(p, back), region_index_of(p, rg.base));
    }
}

TEST(projection_atlas, adjacent_offsets_stay_adjacent_within_a_region) {
    Projection p = build_test_projection({{0x1000, 65536, Region::Code}});
    p.layout = Projection::Layout::Atlas;
    rebuild_layout(p);
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    ASSERT_TRUE(p.project(0x1000, &u0, &v0));
    ASSERT_TRUE(p.project(0x1000 + 64, &u1, &v1));
    // Serpentine order: neighbouring offsets are within one cell step.
    const float cell = 1.0f / static_cast<float>(atlas_cells_per_side(p));
    EXPECT_LE(std::hypot(u1 - u0, v1 - v0), cell * 1.5f);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make docker-desktop-test TEST=test_projection`
Expected: FAIL — `Projection::Layout` not declared.

- [ ] **Step 3: Implement the squarified treemap + serpentine**

In `projection.h`, add the enum, the `layout` member, `rects`, and declare:

```cpp
// Recompute the layout for p.layout. For Atlas this fills p.rects with a
// squarified treemap whose areas are proportional to Region::len (any other
// weighting would be a fabricated emphasis — D7). Idempotent.
void rebuild_layout(Projection &p);
// Cells per side of the atlas grid — the quantisation project() rounds to.
uint32_t atlas_cells_per_side(const Projection &p);
```

In `projection.cpp`, implement standard squarified treemap over
`regions[i].len / total`, then within a rect map region-relative offset to a
serpentine row walk: row `r = off / cols`, column `c = off % cols`, with `c`
reversed on odd rows so consecutive offsets stay adjacent across the row break.
`project()` branches on `layout`; the Hilbert path is unchanged.

- [ ] **Step 4: Run tests to verify they pass**

Run: `make docker-desktop-test TEST=test_projection`
Expected: PASS (4 tests).

- [ ] **Step 5: Commit**

```bash
git add desktop/src/space/projection.h desktop/src/space/projection.cpp desktop/test/test_projection.cpp
git commit -m "space: add the region-atlas projection layout, 100% packed and decodable"
```

---

### Task 3: Route the atlas through the shared address funnel

**Files:**
- Modify: `desktop/src/space/locate.cpp`, `desktop/src/space/stepplace.cpp`
- Test: `desktop/test/test_locate.cpp`, `desktop/test/test_stepplace.cpp`

**Interfaces:**
- Consumes: `Projection::Layout` and `rebuild_layout()` from Task 2.
- Produces: no signature change to `scene_locate_off` or `place_address`. Both become layout-agnostic.

These two are documented as *"the ONE address route"* and *"the shared plane arithmetic"*. If both are layout-agnostic, picking, goto, zoning and every `(u,v)`-keyed layer follow for free — that is what contains the blast radius.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(locate_atlas, scene_locate_off_agrees_with_project_under_atlas) {
    Projection p = build_test_projection({{0x1000, 4096, Region::Code}});
    p.layout = Projection::Layout::Atlas;
    rebuild_layout(p);
    float lu = 0, lv = 0;
    ASSERT_TRUE(scene_locate_off(p, /*region=*/0, /*off=*/0x40, &lu, &lv));
    float pu = 0, pv = 0;
    ASSERT_TRUE(p.project(0x1040, &pu, &pv));
    EXPECT_NEAR(lu, pu, 1e-5f);
    EXPECT_NEAR(lv, pv, 1e-5f);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make docker-desktop-test TEST=test_locate`
Expected: FAIL — mismatch, because the helper re-derives Hilbert arithmetic.

- [ ] **Step 3: Make both helpers delegate to `Projection::project`**

Replace any open-coded Hilbert index maths in `locate.cpp` and `stepplace.cpp` with a call to `p.project()` / `p.unproject()`, so the layout branch lives in exactly one place.

- [ ] **Step 4: Run tests to verify they pass**

Run: `make docker-desktop-test TEST=test_locate && make docker-desktop-test TEST=test_stepplace`
Expected: PASS. Existing Hilbert-layout tests must also still pass — run the full file, not just the new case.

- [ ] **Step 5: Commit**

```bash
git add desktop/src/space/locate.cpp desktop/src/space/stepplace.cpp desktop/test/test_locate.cpp desktop/test/test_stepplace.cpp
git commit -m "space: make the shared address funnel layout-agnostic"
```

---

### Task 4: Camera fit-to-content

**Files:**
- Modify: `desktop/src/scene3d/camera.h:81-88`
- Test: `desktop/test/test_camera.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks — takes a plain bounds struct so `camera.h` stays linmath-only.
- Produces: `void Camera::fit(float u0, float v0, float u1, float v1);` and `reset()`/`top_down()` routed through it.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(camera, fit_frames_a_quarter_plane_closer_than_the_whole) {
    Camera whole; whole.fit(0.0f, 0.0f, 1.0f, 1.0f);
    Camera quarter; quarter.fit(0.0f, 0.0f, 0.5f, 0.5f);
    EXPECT_LT(quarter.radius, whole.radius);
    EXPECT_NEAR(quarter.target[0], 0.25f, 1e-5f);
    EXPECT_NEAR(quarter.target[2], 0.25f, 1e-5f);
}

TEST(camera, fit_respects_the_dolly_clamps) {
    Camera c; c.fit(0.0f, 0.0f, 0.001f, 0.001f);
    EXPECT_GE(c.radius, Camera::kMinRadius);
    EXPECT_LE(c.radius, Camera::kMaxRadius);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make docker-desktop-test TEST=test_camera`
Expected: FAIL — no member `fit`.

- [ ] **Step 3: Implement `fit`**

```cpp
// Frame an axis-aligned region of the floor: centre the target on it and pull
// the radius back far enough that its larger extent fits the vertical fov.
// Clamped through the SAME kMinRadius/kMaxRadius dolly uses, so a degenerate
// region cannot produce a camera inside the near plane.
void fit(float u0, float v0, float u1, float v1) {
    target[0] = 0.5f * (u0 + u1);
    target[1] = 0.0f;
    target[2] = 0.5f * (v0 + v1);
    const float extent = std::fmax(std::fabs(u1 - u0), std::fabs(v1 - v0));
    radius = clampf(extent / std::tan(0.5f * fovy) * 1.1f, kMinRadius, kMaxRadius);
}
```

Then `reset()` sets the default angles and calls `fit(0,0,1,1)`; `top_down()` keeps `pitch = kPitchLimit` and calls `fit` too.

- [ ] **Step 4: Run test to verify it passes**

Run: `make docker-desktop-test TEST=test_camera`
Expected: PASS (2 new tests plus the existing file).

- [ ] **Step 5: Commit**

```bash
git add desktop/src/scene3d/camera.h desktop/test/test_camera.cpp
git commit -m "scene3d: give the camera a fit-to-bounds preset"
```

---

### Task 5: The PC comet — time out of the spatial budget

**Files:**
- Modify: `desktop/src/scene3d/scene.h`, `desktop/src/scene3d/scene.cpp` (`set_trajectories`, `render`)
- Modify: `desktop/src/scene3d/hud.cpp` — remove the `draw_trajectory_ruler` call from the Plane scene
- Test: `desktop/test/test_scene_traj.cpp`

**Interfaces:**
- Consumes: `scene_traj_scale` (Task 1) — retained for the non-Plane scenes that still spatialise time.
- Produces: `uint32_t Scene::comet_tail = 256;` (trail length in steps) and `Scene::comet_mode` (`bool`, default `true` for the Plane scene). With `comet_mode`, every worldline vertex is emitted at `y = 0`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(scene_traj, comet_mode_flattens_the_worldline) {
    space::TrajectorySet ts;
    space::Trajectory tr;
    for (uint64_t t = 0; t < 1000; ++t)
        tr.points.push_back({0x1000 + t * 8, t, false, true});
    ts.trajectories.push_back(tr);

    Scene s;
    s.nsteps = 1000;
    s.comet_mode = true;
    s.set_trajectories(ts, atlas_projection());
    for (float y : s.debug_vertex_heights())
        EXPECT_FLOAT_EQ(y, 0.0f); // time is no longer a spatial axis
}

TEST(scene_traj, comet_tail_selects_a_window_ending_at_the_playhead) {
    Scene s;
    s.comet_mode = true;
    s.comet_tail = 100;
    s.slice_step = 500;
    const auto w = s.comet_window();
    EXPECT_EQ(w.first, 400u);
    EXPECT_EQ(w.second, 500u);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make docker-desktop-test TEST=test_scene_traj`
Expected: FAIL — no member `comet_mode`.

- [ ] **Step 3: Implement comet mode**

In `set_trajectories`, when `comet_mode` emit `y = 0.0f` for every vertex instead of `pt.t * scale`. Add:

```cpp
// The trail window [lo, hi] ending at the playhead — hi is slice_step, lo is
// comet_tail steps behind it (saturating at 0). render() draws vertices in
// this window at full brightness and fades the rest; nothing is discarded,
// because the path outside the window is real, just not recent.
std::pair<uint64_t, uint64_t> comet_window() const;
```

In `render()`, pass the window as a uniform and fade by distance from `hi`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `make docker-desktop-test TEST=test_scene_traj`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add desktop/src/scene3d/scene.h desktop/src/scene3d/scene.cpp desktop/src/scene3d/hud.cpp desktop/test/test_scene_traj.cpp
git commit -m "scene3d: the PC comet — trace time animates rather than occupying Y"
```

---

### Task 6: The optional motif layer

**Files:**
- Modify: `desktop/src/scene3d/scene.h` (the `SceneLayers` bool), `desktop/src/scene3d/layers.cpp` (the registry row), `desktop/src/scene3d/scene.cpp` (draw)
- Test: `desktop/test/test_layers.cpp`

**Interfaces:**
- Consumes: `space::CellOpcode` / `OpClass` (`space/opcode_terrain.h`), `space::SyscallClass` / `syscall_class_name()` (`space/crossing.h`). No new classifier.
- Produces: `bool SceneLayers::motifs` and its `LayerDesc` row, id `"motifs"`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(layers, motifs_has_a_registry_row_and_is_toggleable) {
    const auto &all = scene_layers_all();
    auto it = std::find_if(all.begin(), all.end(),
                           [](const LayerDesc &d) { return std::string(d.id) == "motifs"; });
    ASSERT_NE(it, all.end()) << "every SceneLayers bool needs exactly one row";
    EXPECT_EQ(it->group, LayerDesc::Group::Activity);
    // A re-encoding of exact data — no new claim, but not a raw field either.
    EXPECT_EQ(it->grade, LayerGrade::Derived);

    SceneLayers l;
    EXPECT_TRUE(l.*(it->flag)) << "defaults ON, matching the registry convention";
    l.*(it->flag) = false; // and must be switchable off — that is what optional means
    EXPECT_FALSE(l.*(it->flag));
}

TEST(layers, motifs_abstain_rather_than_guess) {
    // OpClass::Unknown and SyscallClass::Other must map to the neutral tint,
    // never to a neighbouring family's colour (D7).
    EXPECT_EQ(motif_colour_for(space::OpClass::Unknown), kMotifNeutral);
    EXPECT_EQ(motif_colour_for(space::SyscallClass::Other), kMotifNeutral);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make docker-desktop-test TEST=test_layers`
Expected: FAIL — no `motifs` member; the exhaustiveness test also flags it.

- [ ] **Step 3: Implement the layer**

Add `bool motifs = true;` to `SceneLayers`, then the registry row in `layers.cpp`:

```cpp
{"motifs", "motifs (opcode + crossings)",
 "what kind of work is this, and where does it cross into the kernel?",
 LayerDesc::Group::Activity, LayerGrade::Derived, &SceneLayers::motifs},
```

Draw opcode-class emission from `CellOpcode::dominant` (neutral tint when
`dominant == OpClass::Unknown` or `purity` is below the existing threshold), and
a crossing mark coloured by `SyscallClass` with the legend built from
`syscall_class_name()`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `make docker-desktop-test TEST=test_layers`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add desktop/src/scene3d/scene.h desktop/src/scene3d/layers.cpp desktop/src/scene3d/scene.cpp desktop/test/test_layers.cpp
git commit -m "scene3d: optional motif layer — opcode class and syscall-crossing family"
```

---

### Task 7: The motif-distinctness acceptance gate

The gate that decides whether the encoding actually builds intuition. A principled encoding that renders every program identically has failed.

**Files:**
- Create: `desktop/test/test_motif_distinctness.cpp`
- Modify: `mk/desktop.mk` — register the new test binary

**Interfaces:**
- Consumes: everything above, plus the existing image-distinctness helper the doc-screenshot gate uses.
- Produces: nothing consumed downstream.

- [ ] **Step 1: Write the failing test**

```cpp
// Three recordings whose behaviour we already know must be pairwise DISTINCT
// with motifs on. Non-blankness is not enough: an encoding that renders every
// program the same way has failed regardless of how principled it is.
TEST(motif_distinctness, known_recordings_are_pairwise_distinct) {
    const char *fixtures[] = {"memcpy.asmtrace", "simd.asmtrace", "syscalls.asmtrace"};
    std::vector<Image> shots;
    for (const char *f : fixtures)
        shots.push_back(render_plane_scene(f, /*motifs=*/true));
    for (size_t i = 0; i < shots.size(); ++i)
        for (size_t j = i + 1; j < shots.size(); ++j)
            EXPECT_TRUE(images_distinct(shots[i], shots[j]))
                << fixtures[i] << " vs " << fixtures[j];
}

TEST(motif_distinctness, geometry_is_still_correct_with_motifs_off) {
    // "Optional" has to mean the scene stands without it.
    const Image shot = render_plane_scene("memcpy.asmtrace", /*motifs=*/false);
    EXPECT_FALSE(image_blank(shot));
    EXPECT_TRUE(image_has_floor_coverage(shot, /*min_fraction=*/0.5f));
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make docker-desktop-test TEST=test_motif_distinctness`
Expected: FAIL — fixtures absent.

- [ ] **Step 3: Generate the three fixtures**

Record them with the CLI against the bundled examples, and commit them under `desktop/test/fixtures/`. Each must be small enough to keep the test under a second. Note in the file header which example produced each, so a later reader can regenerate them.

- [ ] **Step 4: Run tests to verify they pass**

Run: `make docker-desktop-test TEST=test_motif_distinctness`
Expected: PASS. **If the distinctness case fails, stop and report** — that is the design being refuted, not a test to loosen.

- [ ] **Step 5: Commit**

```bash
git add desktop/test/test_motif_distinctness.cpp desktop/test/fixtures mk/desktop.mk
git commit -m "desktop(test): gate the motif encoding on pairwise distinctness"
```

---

### Task 8: Flip the default, regenerate goldens, document

**Files:**
- Modify: `desktop/src/space/projection.cpp` — default `layout` to `Atlas`
- Modify: `docs/_static/gui/*.png` (regenerate), `docs/internal/gui/` (a short brief recording the change)

**Interfaces:**
- Consumes: all prior tasks.
- Produces: the user-visible change.

- [ ] **Step 1: Flip the default and run the whole suite**

Run: `make docker-desktop-test`
Expected: golden-image failures across the suite — that is the point of this step. Read them; any *geometry* failure is a real bug, only *image* churn is expected.

- [ ] **Step 2: Regenerate the doc screenshots — ONCE, from the merged tree**

Run the `--serve` screenshot flow (headless `--record` never emits `codeimage`, and `codeimage` gates every 3D scene). Regenerate **after** the last recorder edit, never per-agent; two agents regenerating one golden must regenerate from the merged tree rather than picking a side.

- [ ] **Step 3: Verify the screenshots are pairwise distinct**

Gate on distinctness, not just non-blankness — the same rule Task 7 encodes.

- [ ] **Step 4: Write the brief**

Add `docs/internal/gui/61-scene-axis-budget.md` following `_conventions.md`, cross-referencing [53](../../internal/gui/53-3d-catalog-build-roadmap.md) to record that the depiction catalog now composes onto the atlas substrate.

- [ ] **Step 5: Commit**

```bash
git add desktop/src/space/projection.cpp docs/_static/gui docs/internal/gui/61-scene-axis-budget.md
git commit -m "scene3d: make the region atlas the default floor layout"
```

---

## Self-review

**Spec coverage.** Component 1 (atlas) → Tasks 2, 3, 8. Component 2 (time as animation) → Tasks 1, 5. Component 3 (optional motifs) → Tasks 6, 7. Component 4 (camera fit) → Task 4. Acceptance gate → Task 7. The spec's interim-guard requirement — that the clamp land first, on its own — is Task 1, ordered first deliberately.

**Type consistency.** `scene_traj_scale(uint32_t, uint64_t, float)` is defined in Task 1 and reused in Task 5. `Projection::Layout` / `rebuild_layout` / `atlas_cells_per_side` are defined in Task 2 and consumed in Tasks 3 and 8. `AtlasRect` is defined once, in Task 2. `SceneLayers::motifs` is defined in Task 6 and consumed in Task 7.

**Known gap, deliberately left.** Task 7's `render_plane_scene`, `images_distinct`, `image_blank` and `image_has_floor_coverage` are named but not defined here — they are the existing doc-screenshot helpers, and the implementer must locate them rather than write new ones. If they turn out not to exist in a reusable form, Task 7 grows a step to extract them, and that is a legitimate reason to reject the task rather than to hand-roll a fourth image comparator.
