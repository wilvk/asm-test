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
- **How to run tests.** There is **no** `docker-desktop-test` target and no `TEST=` parameter. One test: `make build/desktop_test_<name> && ./build/desktop_test_<name>`. Whole suite: `make desktop-test`. Containerised lane (what CI runs): `make docker-desktop`. Format gate: `make docker-fmt-check`. Never `make X >/dev/null 2>&1` — it hides compile errors and leaves a stale binary "passing".
- **Registering a new test.** `desktop/test/*.cpp` compiles via the pattern rule at [mk/desktop.mk:421](../../../mk/desktop.mk#L421), so a new file needs only two additions: a link rule (`$(BUILD)/desktop_test_<name>: $(BUILD)/desktop/test/t/test_<name>.o` + the two-line `$(CXX)` recipe, pattern at [mk/desktop.mk:1686](../../../mk/desktop.mk#L1686)) and an entry in `DESKTOP_TESTS` ([mk/desktop.mk:1199](../../../mk/desktop.mk#L1199)). A test that includes `linmath.h` also needs the order-only prereq at [mk/desktop.mk:1685](../../../mk/desktop.mk#L1685). **A test not in `DESKTOP_TESTS` never runs in CI** — it is not a test.
- **GL is available in the container.** [Dockerfile.desktop:17-19,43,48](../../../Dockerfile.desktop#L17) pins software Mesa (llvmpipe) + EGL with `LIBGL_ALWAYS_SOFTWARE=1` specifically so the scene FBO smoke renders offscreen. Pixel-level assertions therefore genuinely run under `make docker-desktop` and must **not** be written as self-skipping (CLAUDE.md).
- **clang-format 18 is canonical.** Use `make docker-fmt`. `desktop/` addon includes in `shell.cpp` are order-sensitive — keep the existing fence comments; a bare `clang-format -i` re-sorts them into an `imgui_internal.h #error`.
- **Shared tree.** Many agents work this repo concurrently. Commit by explicit path, never `git add -A`.
- **There is NO gtest — do not write `TEST(...)` / `EXPECT_*` / `ASSERT_*`.** No `gtest`, `googletest`, `catch2` or `doctest` appears in any makefile. Every test is a standalone binary with its own `main()`, using this exact house idiom:

```cpp
static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL: %s (%s)\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

int main() {
    // ... checks ...
    if (failures) {
        std::fprintf(stderr, "%d <name> check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_<name>: all checks passed\n");
    return 0;
}
```

  The third argument to `check` is the **failure explanation**, printed only on failure — write what actually went wrong (`"got order " + std::to_string(p.order)`), never a restatement of the assertion.
- **Exact API signatures** (copy these; earlier drafts got them wrong):
  - `bool Projection::project(uint64_t addr, float *u, float *v) const`
  - `bool Projection::unproject(float u, float v, uint64_t *addr, const Region **r) const` — **four** args, and the last is a `const Region **`, not an index
  - `Projection build_projection(std::vector<Region> regions)` — takes a `std::vector<Region>` by value; build one by filling `Region{base, len, kind}` and moving it in
  - `space::TrajPoint` field order is `{t, addr, fidelity, is_access, tid, placed}` — `t` comes **first**. Prefer named field assignment over brace-init so a field reorder cannot silently transpose values.

---

### Task 1: Clamp the runaway worldline height (standalone defect fix)

Lands first and independently of the re-encoding, so the live defect is fixed regardless of when the substrate work completes.

**Files:**
- Create: `desktop/src/scene3d/trajscale.h` — the scale rule, **header-only**
- Modify: `desktop/src/scene3d/scene.cpp:797-802` — call it
- Create: `desktop/test/test_scene_traj.cpp` — **does not exist**; this task creates it
- Modify: `mk/desktop.mk` — register the new test binary (see Global Constraints)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `float scene_traj_scale(uint32_t nsteps, uint64_t max_t, float time_scale)` in `asmdesk::scene3d`, header-only. `Scene::set_trajectories` routes through it so every emitted vertex Y lies in `[0, 0.6]`.

**Why a new header rather than a member of `scene.cpp`:** `scene.o` links GL, so a test touching it needs a GL context for a piece of pure arithmetic. [camera.h](../../../desktop/src/scene3d/camera.h) and `linequad.h` are already header-only for exactly this reason — `test_camera.cpp`'s own comment calls them "header-only citizens" whose binary "links NOTHING but the header". This test joins them: it stays in the null harness and links only its own object.

- [ ] **Step 1: Write the failing test**

```cpp
// test_scene_traj.cpp — the worldline's vertical scale rule. Null harness, no
// GL: this binary links nothing but the header-only scene3d/trajscale.h, on
// the same terms as test_camera.cpp's citizens.
#include <cstdio>
#include <string>

#include "scene3d/trajscale.h"

using asmdesk::scene3d::scene_traj_scale;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL: %s (%s)\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

int main() {
    // A `mem` stream can outlast the trace's own step count — the same mismatch
    // space/sediment.cpp:33-37 already guards. The worldline must not escape
    // its 0.6 world-unit envelope when it does.
    {
        const float scale = scene_traj_scale(/*nsteps=*/10, /*max_t=*/100000,
                                             /*time_scale=*/0.0f);
        const float top = 100000.0f * scale;
        check("a mem step past nsteps stays inside the 0.6 envelope",
              top <= 0.6f + 1e-4f, "top of the worldline reached " +
                                       std::to_string(top));
    }
    // The ordinary case is unchanged: nsteps covers every step, so the path
    // still tops out AT 0.6 rather than being needlessly compressed.
    {
        const float scale = scene_traj_scale(1000, 999, 0.0f);
        check("a well-formed trace still spans the full envelope",
              std::fabs(999.0f * scale - 0.6f) < 0.01f,
              "top was " + std::to_string(999.0f * scale) + ", wanted ~0.6");
    }
    // nsteps == 0 must NOT fall back to a fixed per-step constant: at the
    // golden's 35560 steps the old 0.02f produced 711 world units, ~300x the
    // camera radius and past zfar.
    {
        const float scale = scene_traj_scale(0, 35560, 0.0f);
        const float top = 35560.0f * scale;
        check("nsteps == 0 does not blow the envelope", top <= 0.6f + 1e-4f,
              "top of the worldline reached " + std::to_string(top));
    }
    // An explicit time_scale is still honoured verbatim — callers that set it
    // are stating a scale, not asking for one.
    check("an explicit time_scale is passed through",
          scene_traj_scale(1000, 999, 0.25f) == 0.25f, "explicit scale ignored");

    if (failures) {
        std::fprintf(stderr, "%d scene_traj check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_scene_traj: all checks passed\n");
    return 0;
}
```

- [ ] **Step 2: Register the new test binary in `mk/desktop.mk`**

The file is new, so without this it never builds and never runs in CI. It links only its own object, exactly like `test_camera`:

```make
$(BUILD)/desktop_test_scene_traj: $(BUILD)/desktop/test/t/test_scene_traj.o
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@
```

and add `$(BUILD)/desktop_test_scene_traj \` to the `DESKTOP_TESTS` list at [mk/desktop.mk:1199](../../../mk/desktop.mk#L1199). It does **not** include `linmath.h`, so it needs no entry in the order-only prereq group.

- [ ] **Step 3: Run test to verify it fails**

Run: `make build/desktop_test_scene_traj && ./build/desktop_test_scene_traj`
Expected: FAIL — `scene_traj_scale` not declared.

- [ ] **Step 4: Write the header and apply sediment's guard**

Create `desktop/src/scene3d/trajscale.h`:

```cpp
// trajscale.h — the worldline's world-Y per trace step, as PURE ARITHMETIC.
// Header-only and dependency-free (no GL, no ImGui, no linmath), on the same
// terms as camera.h and linequad.h, so test_scene_traj.cpp links nothing but
// its own object and the rule is checkable with no context at all.
#ifndef ASMDESK_SCENE3D_TRAJSCALE_H
#define ASMDESK_SCENE3D_TRAJSCALE_H

#include <algorithm>
#include <cstdint>

namespace asmdesk::scene3d {

// `max_t` is the largest TrajPoint.t actually present. `nsteps` is the
// TERRAIN's extent and can UNDER-REPORT it — a `mem` stream can outlast the
// trace's own step count (the same mismatch space/sediment.cpp:33-37 already
// guards, with the same max()). EXTENDING the denominator rather than clamping
// the data is what keeps every real step on the axis: a step past the stated
// extent is still a real step. A positive `time_scale` is a caller STATING a
// scale and is returned verbatim.
inline float scene_traj_scale(uint32_t nsteps, uint64_t max_t,
                              float time_scale) {
    if (time_scale > 0.0f)
        return time_scale;
    const uint64_t top = std::max<uint64_t>(nsteps, max_t + 1);
    return top > 0 ? 0.6f / static_cast<float>(top) : 0.6f;
}

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_TRAJSCALE_H
```

Note the final fallback is `0.6f`, not the old `0.02f`: with `max_t + 1` in the `max()`, `top` is zero only when there are no points at all, so any surviving constant must still be envelope-safe rather than a per-step rate.

Then in `Scene::set_trajectories`, `#include "scene3d/trajscale.h"` and replace lines 800-801 with a pre-pass for `max_t` followed by:

```cpp
uint64_t max_t = 0;
for (const space::Trajectory &tr : ts.trajectories)
    for (const space::TrajPoint &pt : tr.points)
        max_t = std::max(max_t, pt.t);
const float scale = scene_traj_scale(nsteps, max_t, time_scale);
```

- [ ] **Step 5: Run test to verify it passes**

Run: `make build/desktop_test_scene_traj && ./build/desktop_test_scene_traj`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add desktop/src/scene3d/trajscale.h desktop/src/scene3d/scene.cpp desktop/test/test_scene_traj.cpp mk/desktop.mk
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

Add to the existing `desktop/test/test_projection.cpp`, reusing its `check`/`fail` helpers and its `Ref` struct. Add a local builder beside the existing setup:

```cpp
// An atlas-layout projection over the given regions. Mirrors main()'s existing
// build, then switches layout — build_projection() itself stays layout-neutral.
static Projection atlas_of(const std::vector<Ref> &refs) {
    std::vector<Region> in;
    for (const Ref &r : refs) {
        Region reg;
        reg.base = r.base;
        reg.len = r.len;
        reg.kind = r.kind;
        in.push_back(reg);
    }
    Projection p = build_projection(std::move(in));
    p.layout = Projection::Layout::Atlas;
    rebuild_layout(p);
    return p;
}
```

Then, inside `main()`:

```cpp
// --- the atlas layout: 100% packed, decodable, locally contiguous ----------
{
    // 4096 : 61440 == 1 : 15, so the code rect is exactly 1/16 of the floor.
    static const Ref kSmall = {0x0000000000400000ull, 4096, Region::Code};
    static const Ref kBig = {0x0000000001000000ull, 61440, Region::Mmap};
    const Projection p = atlas_of({kSmall, kBig});

    float area = 0.0f;
    for (const AtlasRect &r : p.rects)
        area += (r.u1 - r.u0) * (r.v1 - r.v0);
    // The whole point: no power-of-4 padding, so no empty three-quarters.
    check("the atlas packs the whole floor", std::fabs(area - 1.0f) < 1e-4f,
          "covered " + std::to_string(area) + " of the unit plane");

    // regions come out sorted by base, so index 0 is the code region.
    const AtlasRect &code = p.rects[0];
    const float code_area = (code.u1 - code.u0) * (code.v1 - code.v0);
    check("rect area is proportional to Region::len",
          std::fabs(code_area - 1.0f / 16.0f) < 1e-4f,
          "code rect covered " + std::to_string(code_area) + ", wanted 0.0625");

    // Round trip: cell quantisation means the exact address need not survive,
    // but the REGION must — that is what makes the floor decodable.
    for (const Region &rg : p.regions) {
        float u = 0, v = 0;
        check("a region base projects under the atlas", p.project(rg.base, &u, &v),
              "project refused a base inside the domain");
        uint64_t back = 0;
        const Region *got = nullptr;
        check("a projected cell unprojects", p.unproject(u, v, &back, &got),
              "unproject refused a cell the atlas had just placed");
        check("the round trip lands in the same region",
              got != nullptr && got->base == rg.base,
              "a region base round-tripped into a different region");
    }
}
{
    // Serpentine order within a region: neighbouring offsets stay neighbours.
    // This is the locality Hilbert was bought for, kept where it means something.
    static const Ref kOne = {0x0000000000400000ull, 65536, Region::Code};
    const Projection p = atlas_of({kOne});
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    check("first offset projects", p.project(0x400000ull, &u0, &v0), "refused");
    check("next offset projects", p.project(0x400000ull + 64, &u1, &v1), "refused");
    const float cell = 1.0f / static_cast<float>(atlas_cells_per_side(p));
    const float d = std::hypot(u1 - u0, v1 - v0);
    check("adjacent offsets stay adjacent within a region", d <= cell * 1.5f,
          "neighbouring offsets landed " + std::to_string(d / cell) +
              " cells apart");
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make build/desktop_test_projection && ./build/desktop_test_projection`
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

Run: `make build/desktop_test_projection && ./build/desktop_test_projection`
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

Add to the existing `desktop/test/test_locate.cpp`, reusing its `check` helper:

```cpp
// The ONE address route must agree with the projection under BOTH layouts —
// that agreement is what contains the blast radius of a layout swap. If this
// holds, picking, goto, zoning and every (u,v)-keyed layer follow for free.
{
    std::vector<Region> in;
    Region reg;
    reg.base = 0x0000000000400000ull;
    reg.len = 4096;
    reg.kind = Region::Code;
    in.push_back(reg);
    Projection p = build_projection(std::move(in));

    for (const auto layout : {Projection::Layout::Hilbert,
                              Projection::Layout::Atlas}) {
        p.layout = layout;
        rebuild_layout(p);
        const char *name =
            layout == Projection::Layout::Atlas ? "atlas" : "hilbert";
        float lu = 0, lv = 0;
        check(std::string("scene_locate_off places an offset (") + name + ")",
              scene_locate_off(p, /*region=*/0, /*off=*/0x40, &lu, &lv),
              "the one address route refused an in-domain offset");
        float pu = 0, pv = 0;
        check(std::string("project places the same address (") + name + ")",
              p.project(0x400040ull, &pu, &pv), "project refused it");
        check(std::string("locate and project agree (") + name + ")",
              std::fabs(lu - pu) < 1e-5f && std::fabs(lv - pv) < 1e-5f,
              "locate gave (" + std::to_string(lu) + "," + std::to_string(lv) +
                  ") but project gave (" + std::to_string(pu) + "," +
                  std::to_string(pv) + ")");
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/desktop_test_locate && ./build/desktop_test_locate`
Expected: FAIL — mismatch, because the helper re-derives Hilbert arithmetic.

- [ ] **Step 3: Make both helpers delegate to `Projection::project`**

Replace any open-coded Hilbert index maths in `locate.cpp` and `stepplace.cpp` with a call to `p.project()` / `p.unproject()`, so the layout branch lives in exactly one place.

- [ ] **Step 4: Run tests to verify they pass**

Run: `make build/desktop_test_locate && ./build/desktop_test_locate && make build/desktop_test_stepplace && ./build/desktop_test_stepplace`
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

Add to the existing `desktop/test/test_camera.cpp`, reusing its `check` helper:

```cpp
// 3D-axis-budget T4: fit-to-bounds. reset() framed the whole unit plane
// unconditionally, so an atlas occupying a fraction of it sat small in a
// mostly-empty viewport with no way to say so.
{
    Camera whole;
    whole.fit(0.0f, 0.0f, 1.0f, 1.0f);
    Camera quarter;
    quarter.fit(0.0f, 0.0f, 0.5f, 0.5f);
    check("fitting a smaller region dollies closer", quarter.radius < whole.radius,
          "quarter radius " + std::to_string(quarter.radius) +
              " was not closer than whole " + std::to_string(whole.radius));
    check("fit centres the target on the region",
          std::fabs(quarter.target[0] - 0.25f) < 1e-5f &&
              std::fabs(quarter.target[2] - 0.25f) < 1e-5f,
          "target landed at (" + std::to_string(quarter.target[0]) + "," +
              std::to_string(quarter.target[2]) + "), wanted (0.25,0.25)");
    check("fit never lifts the target off the ground plane",
          quarter.target[1] == 0.0f,
          "a camera whose horizon lies about where the ground is");
}
{
    // A degenerate region must not produce a camera inside the near plane —
    // the same clamp-don't-break discipline dolly() already uses.
    Camera c;
    c.fit(0.0f, 0.0f, 0.001f, 0.001f);
    check("fit respects the dolly clamps",
          c.radius >= Camera::kMinRadius && c.radius <= Camera::kMaxRadius,
          "radius " + std::to_string(c.radius) + " escaped [" +
              std::to_string(Camera::kMinRadius) + "," +
              std::to_string(Camera::kMaxRadius) + "]");
}
{
    // reset() must still land where the existing checks in this file expect.
    Camera r;
    r.reset();
    check("reset still frames the whole plane from the default angles",
          std::fabs(r.target[0] - 0.5f) < 1e-5f &&
              std::fabs(r.target[2] - 0.5f) < 1e-5f,
          "reset moved the default target");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/desktop_test_camera && ./build/desktop_test_camera`
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

Run: `make build/desktop_test_camera && ./build/desktop_test_camera`
Expected: PASS (2 new tests plus the existing file).

- [ ] **Step 5: Commit**

```bash
git add desktop/src/scene3d/camera.h desktop/test/test_camera.cpp
git commit -m "scene3d: give the camera a fit-to-bounds preset"
```

---

### Task 5: The PC comet — time out of the spatial budget

**Files:**
- Modify: `desktop/src/scene3d/trajscale.h` — add `traj_vertex_y` and `comet_window` (Task 1 created this header)
- Modify: `desktop/src/scene3d/scene.h`, `desktop/src/scene3d/scene.cpp` (`set_trajectories`, `render`) — call them
- Modify: `desktop/src/scene3d/hud.cpp` — remove the `draw_trajectory_ruler` call from the Plane scene
- Test: `desktop/test/test_scene_traj.cpp` (created and registered by Task 1 — no `mk/desktop.mk` change needed here)

**Interfaces:**
- Consumes: `scene_traj_scale` (Task 1) — retained for the non-Plane scenes that still spatialise time.
- Produces, in `scene3d/trajscale.h` (header-only, so the test needs no GL):
  - `float traj_vertex_y(uint64_t t, float scale, bool comet_mode)`
  - `std::pair<uint64_t, uint64_t> comet_window(uint64_t slice_step, uint32_t tail)`
- Produces, on `Scene`: `uint32_t comet_tail = 256;` (trail length in steps) and `bool comet_mode` (default `true` for the Plane scene). The arithmetic lives in the header; `Scene` only holds the settings and calls it.

- [ ] **Step 1: Write the failing test**

Both rules go in `scene3d/trajscale.h` (Task 1's header) as pure functions, so `test_scene_traj.cpp` keeps linking nothing but its own object — no GL, no `Scene`. `Scene::set_trajectories` and `render()` then *call* them rather than owning the arithmetic.

Append to `desktop/test/test_scene_traj.cpp`, inside `main()`:

```cpp
// Comet mode: trace time is no longer a spatial axis, so every worldline
// vertex sits on the floor and the path is read through the playhead instead.
{
    const float scale = scene_traj_scale(1000, 999, 0.0f);
    for (uint64_t t : {uint64_t(0), uint64_t(1), uint64_t(500), uint64_t(999)}) {
        const float y = traj_vertex_y(t, scale, /*comet_mode=*/true);
        check("comet mode flattens the worldline", y == 0.0f,
              "step " + std::to_string(t) + " sat at y=" + std::to_string(y));
    }
    // With comet mode OFF the old spatial behaviour is unchanged — the other
    // scenes still spatialise time and must not regress.
    check("non-comet mode still lifts by trace time",
          traj_vertex_y(999, scale, false) > 0.0f,
          "the spatial-time path lost its height");
}
// The trail is a window ENDING at the playhead. Nothing outside it is
// discarded — the path is real, just not recent — so this selects emphasis,
// not existence.
{
    const auto w = comet_window(/*slice_step=*/500, /*comet_tail=*/100);
    check("the comet trail ends at the playhead", w.second == 500u,
          "window ended at " + std::to_string(w.second));
    check("the comet trail starts one tail behind it", w.first == 400u,
          "window started at " + std::to_string(w.first));
}
{
    // Saturating at zero: near the start of a recording the trail is short,
    // never negative and never wrapped.
    const auto w = comet_window(/*slice_step=*/10, /*comet_tail=*/100);
    check("the comet trail saturates at the start of the recording",
          w.first == 0u && w.second == 10u,
          "window was [" + std::to_string(w.first) + "," +
              std::to_string(w.second) + "]");
}
```

Add to `scene3d/trajscale.h`:

```cpp
// A worldline vertex's world Y. In comet mode trace time is NOT a spatial
// axis — the path lies on the floor and is read through the playhead — so
// this is flat 0. The other scenes still spatialise time and keep the lift.
inline float traj_vertex_y(uint64_t t, float scale, bool comet_mode) {
    return comet_mode ? 0.0f : static_cast<float>(t) * scale;
}

// The trail window [lo, hi] ending at the playhead, `tail` steps long and
// saturating at 0. render() draws this window full-bright and fades the rest;
// nothing is discarded, because the path outside the window is real, just not
// recent.
inline std::pair<uint64_t, uint64_t> comet_window(uint64_t slice_step,
                                                  uint32_t tail) {
    const uint64_t lo = slice_step > tail ? slice_step - tail : 0;
    return {lo, slice_step};
}
```

(`trajscale.h` gains `#include <utility>` for `std::pair`.)

- [ ] **Step 2: Run tests to verify they fail**

Run: `make build/desktop_test_scene_traj && ./build/desktop_test_scene_traj`
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

Run: `make build/desktop_test_scene_traj && ./build/desktop_test_scene_traj`
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

Add to the existing `desktop/test/test_layers.cpp`, reusing its `check` helper. The file already asserts registry exhaustiveness, so adding a bool without a row fails there by name — this adds the row's own properties:

```cpp
// The motif layer is OPTIONAL: it carries a registry row like every other
// bool, and the scene must stand with it off.
{
    const std::vector<LayerDesc> &all = scene_layers_all();
    const LayerDesc *row = nullptr;
    for (const LayerDesc &d : all)
        if (std::string(d.id) == "motifs")
            row = &d;
    check("motifs has a registry row", row != nullptr,
          "every SceneLayers bool needs exactly one row in scene_layers_all()");
    if (row != nullptr) {
        check("motifs groups under activity",
              row->group == LayerDesc::Group::Activity,
              "a what-happened layer filed under the wrong group");
        // A re-encoding of exact data: no new claim, but not a raw recorded
        // field either. NOT Statistical — nothing here is sampled.
        check("motifs grades as derived", row->grade == LayerGrade::Derived,
              "an opcode/crossing re-encoding is neither exact nor statistical");
        SceneLayers l;
        check("motifs defaults on, matching the registry convention",
              l.*(row->flag), "the default deviates from the all-true convention");
        l.*(row->flag) = false;
        check("motifs can be switched off — that is what optional means",
              !(l.*(row->flag)), "the toggle did not take");
    }
}
// D7: abstain rather than guess. An unclassifiable opcode and an unlisted
// syscall must read as the neutral tint, never as a neighbouring family.
{
    check("an unknown opcode class abstains",
          motif_colour_for(space::OpClass::Unknown) == kMotifNeutral,
          "an unclassified opcode borrowed a real class's colour");
    check("an unlisted syscall abstains",
          motif_colour_for(space::SyscallClass::Other) == kMotifNeutral,
          "an unlisted syscall was folded into a known family");
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make build/desktop_test_layers && ./build/desktop_test_layers`
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

Run: `make build/desktop_test_layers && ./build/desktop_test_layers`
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
- Create: `desktop/test/image_distinct.h` — the comparator; **no such helper exists today** (verified: nothing defines `images_distinct`)
- Modify: `mk/desktop.mk` — register the new test binary in `DESKTOP_GL_TESTS` ([mk/desktop.mk:2310](../../../mk/desktop.mk#L2310)), **not** `DESKTOP_TESTS` — it needs a GL context
- Create: `desktop/test/fixtures/motif-{memcpy,simd,syscalls}.asmtrace`

**Interfaces:**
- Consumes: everything above. Renders through the same offscreen-FBO path `test_scene_fbo.cpp` already uses.
- Produces: `bool images_distinct(const Image &a, const Image &b, float min_fraction = 0.02f);` — true when at least `min_fraction` of pixels differ beyond a per-channel threshold.

- [ ] **Step 0: Decide the fixture-honesty question before writing anything**

`test_scene_fbo.cpp:20-23` records that the rich-`mem` golden scene is **hand-authored** "because `mem` has no producer". So a memory-access fixture may have to be synthetic — and a hand-authored fixture can be tuned until it passes, which would make this gate self-confirming and worthless.

Resolve it explicitly, and write the decision into the test file's header comment:

- **Preferred:** generate all three from real runs via `make asmtrace-golden` ([tools/asmtrace_record.c](../../../tools/asmtrace_record.c)) so the distinctness claim is about real programs. The `simd` and `syscalls` cases need no `mem` stream — opcode class comes from disasm and crossings from the `syscall` kind — so **only** the memcpy case is at risk.
- **If a hand-authored fixture is unavoidable:** say so in the header, and assert distinctness only on channels the fixture did not hand-set. Never assert on a quantity the fixture author chose.

Do not proceed to Step 1 until this is settled — it decides whether the gate means anything.

- [ ] **Step 1: Write the comparator**

```cpp
// image_distinct.h — pairwise frame distinctness. Deliberately crude: this
// answers "would a reader see two different pictures", not "are these images
// similar", so it counts differing pixels rather than computing a perceptual
// metric. A threshold, not a score, because the gate is a yes/no.
inline bool images_distinct(const Image &a, const Image &b,
                            float min_fraction = 0.02f) {
    if (a.w != b.w || a.h != b.h)
        return true; // different geometry is trivially distinct
    size_t differing = 0;
    const size_t n = size_t(a.w) * size_t(a.h);
    for (size_t i = 0; i < n; ++i) {
        const int dr = int(a.px[i * 4 + 0]) - int(b.px[i * 4 + 0]);
        const int dg = int(a.px[i * 4 + 1]) - int(b.px[i * 4 + 1]);
        const int db = int(a.px[i * 4 + 2]) - int(b.px[i * 4 + 2]);
        if (std::abs(dr) + std::abs(dg) + std::abs(db) > 24)
            differing++;
    }
    return float(differing) / float(n) >= min_fraction;
}
```

- [ ] **Step 2: Write the failing test**

Follow `test_scene_fbo.cpp`'s two-half shape: a PURE half that always runs, and a GL half. **The GL half must not self-skip in this binary** — `Dockerfile.desktop` pins software Mesa + EGL so it really renders, and a test that can only self-skip is not a test (CLAUDE.md). Fail loudly if no context is obtainable.

```cpp
static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL: %s (%s)\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

int main() {
    static const char *kFixtures[3] = {"motif-memcpy.asmtrace",
                                       "motif-simd.asmtrace",
                                       "motif-syscalls.asmtrace"};

    // A missing fixture is a FAILURE, never a skip — the same rule
    // test_scene_fbo.cpp:90 already states for its golden scenes.
    for (const char *f : kFixtures)
        check(std::string("fixture is present: ") + f, fixture_exists(f),
              "the acceptance gate cannot run without its inputs");

    if (!gl_context_available())
        // NOT a skip. The container pins software Mesa + EGL for exactly this.
        fail("a GL context is obtainable",
             "no EGL device — run this under `make docker-desktop`, which "
             "pins libgl1-mesa-dri + libegl1-mesa-dev with "
             "LIBGL_ALWAYS_SOFTWARE=1");

    if (failures == 0) {
        // Three recordings whose behaviour we already know must be pairwise
        // DISTINCT with motifs on. Non-blankness is not enough: an encoding
        // that renders every program alike has failed however principled it is.
        Image shots[3];
        for (int i = 0; i < 3; i++)
            shots[i] = render_plane_scene(kFixtures[i], /*motifs=*/true);
        for (int i = 0; i < 3; i++)
            for (int j = i + 1; j < 3; j++)
                check(std::string("distinct: ") + kFixtures[i] + " vs " +
                          kFixtures[j],
                      images_distinct(shots[i], shots[j]),
                      "two programs with different behaviour rendered alike — "
                      "the encoding conveys nothing about what ran");

        // "Optional" has to mean the scene stands without the layer.
        const Image bare = render_plane_scene(kFixtures[0], /*motifs=*/false);
        check("the scene still renders with motifs off", !image_blank(bare),
              "turning the optional layer off emptied the viewport");
        check("the floor is still substantially covered with motifs off",
              image_floor_fraction(bare) >= 0.5f,
              "covered " + std::to_string(image_floor_fraction(bare)) +
                  " of the floor — the atlas should pack it");
    }

    if (failures) {
        std::fprintf(stderr, "%d motif check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_motif_distinctness: all checks passed\n");
    return 0;
}
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `make build/desktop_test_motif_distinctness && ./build/desktop_test_motif_distinctness`
Expected: FAIL — fixtures absent.

- [ ] **Step 4: Generate the three fixtures**

Per the Step 0 decision. Commit them under `desktop/test/fixtures/`, each small enough to keep the test under a second, and record in the test's header comment exactly what produced each one so a later reader can regenerate them.

- [ ] **Step 5: Run tests to verify they pass — in the container, not on the host**

Run: `make docker-desktop`
Expected: PASS. The host may have no EGL device, in which case a GL test *self-skips* and a host-only run would report a false green. `Dockerfile.desktop` pins software Mesa + EGL precisely so this lane really renders — so **this gate must be judged from the container run**.

**If the distinctness case fails, stop and report.** That is the design being refuted, not a test to loosen. Do not lower `min_fraction` to make it pass.

- [ ] **Step 6: Commit**

```bash
git add desktop/test/test_motif_distinctness.cpp desktop/test/image_distinct.h desktop/test/fixtures mk/desktop.mk
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

- [ ] **Step 1: Audit for Hilbert-specific assumptions BEFORE flipping anything**

Some existing tests reason from the Hilbert cell mapping rather than from the projection's contract. **Two are already known:**

- [test_converge.cpp:51-52](../../../desktop/test/test_converge.cpp#L51) argues *"len is a power of four → a 1:1 domain, so distinct addresses land in distinct cells"* — the atlas does not guarantee this.
- [test_projection.cpp:105](../../../desktop/test/test_projection.cpp#L105) asserts `p.order == 9` outright, with the comment *"order is ceil(log4(sum len)) clamped to [6,12]"*. `order` is a **Hilbert** concept; under the atlas it is either meaningless or merely the cell quantisation. Decide which, and make the assertion say so rather than deleting it.

Run: `grep -rn "power of four\|power-of-four\|4\^\|1:1 domain\|hilbert\|Hilbert" desktop/test/ desktop/src/`

Triage each hit into one of: (a) a comment needing an update, (b) a test whose *setup* relies on the mapping — rewrite it against the contract (`project`/`unproject` round-trip), not the layout, or (c) a genuine behavioural dependency, which is a design finding and must be reported, not patched over. **These are not golden churn.** A failure here is a real bug.

- [ ] **Step 2: Flip the default and run the whole suite**

Run: `make docker-desktop`
Expected: golden-image failures across the suite — that is the point of this step. Read every one; any *geometry* or *assertion* failure is a real bug, only *image* churn is expected.

- [ ] **Step 3: Regenerate the doc screenshots — ONCE, from the merged tree**

Run the `--serve` screenshot flow (headless `--record` never emits `codeimage`, and `codeimage` gates every 3D scene). Regenerate **after** the last recorder edit, never per-agent; two agents regenerating one golden must regenerate from the merged tree rather than picking a side.

- [ ] **Step 4: Verify the screenshots are pairwise distinct**

Gate on distinctness, not just non-blankness — the same rule Task 7 encodes.

- [ ] **Step 5: Write the brief**

Add `docs/internal/gui/61-scene-axis-budget.md` following `_conventions.md`, cross-referencing [53](../../internal/gui/53-3d-catalog-build-roadmap.md) to record that the depiction catalog now composes onto the atlas substrate.

- [ ] **Step 6: Commit**

```bash
git add desktop/src/space/projection.cpp docs/_static/gui docs/internal/gui/61-scene-axis-budget.md
git commit -m "scene3d: make the region atlas the default floor layout"
```

---

## Self-review

**Spec coverage.** Component 1 (atlas) → Tasks 2, 3, 8. Component 2 (time as animation) → Tasks 1, 5. Component 3 (optional motifs) → Tasks 6, 7. Component 4 (camera fit) → Task 4. Acceptance gate → Task 7. The spec's interim-guard requirement — that the clamp land first, on its own — is Task 1, ordered first deliberately.

**Type consistency.** `scene_traj_scale(uint32_t, uint64_t, float)` is defined in Task 1 and reused in Task 5. `Projection::Layout` / `rebuild_layout` / `atlas_cells_per_side` are defined in Task 2 and consumed in Tasks 3 and 8. `AtlasRect` is defined once, in Task 2. `SceneLayers::motifs` is defined in Task 6 and consumed in Task 7.

**Verified against the tree (2026-08-03), after a first draft got several of these wrong.**

| Assumption | Verdict |
|---|---|
| `make docker-desktop-test TEST=<x>` runs one test | **false** — no such target, no `TEST=` parameter. Corrected throughout; see Global Constraints |
| `test_scene_traj.cpp` exists | **false** — Task 1 creates *and registers* it. `mk/desktop.mk` registration is now an explicit step; an unregistered test never runs |
| `test_projection` / `test_camera` / `test_locate` / `test_stepplace` / `test_layers` exist | **true** — Tasks 2, 3, 4, 6 extend existing files |
| An image-distinctness helper exists to reuse | **false** — nothing defines `images_distinct`. Task 7 now builds it (Step 1) |
| Pixel assertions can run in CI | **true** — `Dockerfile.desktop` pins software Mesa + EGL for exactly this. The gate must be judged from `make docker-desktop`, since a host without EGL self-skips and reports a false green |
| The observed-data-span projection is still the catalog's open blocker | **false** — doc 54's phase-0 plumbing landed it; `observed data` appears as a region in the live HUD. The atlas has data regions to lay out |
| The repo uses gtest | **false** — no gtest/catch2/doctest anywhere. Every test is a standalone `main()` over a hand-rolled `check(what, cond, why)`. All seven code blocks rewritten in that idiom |
| `Projection::unproject` takes `(u, v, &addr)` | **false** — four args, `(u, v, uint64_t *addr, const Region **r)`. Corrected in Tasks 2 and 3 |
| `build_test_projection` / `region_index_of` exist to reuse | **false** — invented. Task 2 now builds through the real `build_projection(std::vector<Region>)` and a local `atlas_of()` helper |
| `TrajPoint` is `{addr, t, ...}` | **false** — it is `{t, addr, fidelity, is_access, tid, placed}`. The plan now uses named assignment rather than brace-init so a reorder cannot transpose values |
| A test of the scale rule needs `Scene` (and therefore GL) | **false, and it was a design smell** — `scene_traj_scale` and the comet rules now live in a header-only `scene3d/trajscale.h`, joining `camera.h` and `linequad.h`. Tasks 1 and 5 link nothing but their own object |

**Remaining known risk, not resolvable on paper.** Task 7's memcpy fixture may have to be hand-authored, because `mem` has no producer (`test_scene_fbo.cpp:20-23`). A hand-tuned fixture would make the acceptance gate self-confirming. Task 7 Step 0 forces that decision *before* any code is written rather than leaving it to be discovered — and the `simd` and `syscalls` cases are unaffected, since neither needs a `mem` stream.
