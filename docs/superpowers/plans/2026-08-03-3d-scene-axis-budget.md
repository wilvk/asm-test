# 3D scene axis budget — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Re-encode the desktop 3D overview so its three spatial axes carry three decodable quantities — a 100 %-packed region atlas on the floor, access density on Y, and time moved out of the spatial budget into playhead animation.

**Architecture:** A new `Layout::Atlas` mode on `space::Projection` replaces the Hilbert curve with an order-preserving binary-split treemap of regions (serpentine within each), **keeping the existing `n = 1 << order` cell grid** so `Terrain` and every cell-indexed layer are untouched: only the domain-offset → cell *mapping* changes. `project()`/`unproject()` keep their signatures. The trajectory stops mapping trace step to world Y, and the head/tail the `vehicle` layer already draws becomes the sole reader of execution time.

**Tech Stack:** C++17, `desktop/src/space/` (pure, engine-free — no GL, no ImGui), `desktop/src/scene3d/` (GL), `desktop/src/ui/` (ImGui), the null test harness under `desktop/test/`.

**Spec:** [2026-08-03-3d-scene-axis-budget-design.md](../specs/2026-08-03-3d-scene-axis-budget-design.md)

## Global Constraints

- **D4 — purity.** `space/` includes no GL, no ImGui, no engine header. Anything added there must compile into both desktop binaries *and* the null test harness. `scene3d/layers.h` may include `scene3d/scene.h` only.
- **D7 — no fabricated structure.** No inferred layout weighting, no guessed classification. `OpClass::Unknown` and `SyscallClass::Other` abstain visibly; they are never folded into a neighbouring class.
- **Treemap area is proportional to `Region::len`.** Any other weighting is a fabricated emphasis. An equal-area mode, if ever added, must be user-selected and labelled.
- **`SceneLayers` is exhaustive by test.** Every bool needs exactly one `LayerDesc` row in `scene_layers_all()` or `test_layers.cpp` fails by name.
- **Statistical never merges into exact.** A `TF_STAT` surface stays in its own `Terrain`, drawn in separate ink with a persistent `STATISTICAL — survey` label.
- **How to run tests.** There is **no** `docker-desktop-test` target and no `TEST=` parameter. One test: `make build/desktop_test_<name> && ./build/desktop_test_<name>`. Whole suite: `make desktop-test`. Containerised lane (what CI runs): `make docker-desktop`. Format gate: `make docker-fmt-check`. Never `make X >/dev/null 2>&1` — it hides compile errors and leaves a stale binary "passing".
- **Registering a new test.** `desktop/test/*.cpp` compiles via the pattern rule at [mk/desktop.mk:421](../../../mk/desktop.mk#L421), so a new file needs only two additions: a link rule (`$(BUILD)/desktop_test_<name>: $(BUILD)/desktop/test/t/test_<name>.o` + the two-line `$(CXX)` recipe, pattern at [mk/desktop.mk:1686-1687](../../../mk/desktop.mk#L1686)) and an entry in `DESKTOP_TESTS` ([mk/desktop.mk:1199](../../../mk/desktop.mk#L1199)). A test that includes `linmath.h` also needs the order-only prereq at [mk/desktop.mk:1684-1685](../../../mk/desktop.mk#L1684). **A test not in `DESKTOP_TESTS` never runs in CI** — it is not a test.
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
- **Exact API signatures** (copy these; two earlier drafts got them wrong):
  - `struct Projection` lives in **[`space/types.h:30-55`](../../../desktop/src/space/types.h#L30-L55)**, *not* `projection.h`. Every member added to it in this plan goes in `types.h`; `projection.h` holds only free functions.
  - `bool Projection::project(uint64_t addr, float *u, float *v) const`
  - `bool Projection::unproject(float u, float v, uint64_t *addr, const Region **r) const` — **four** args, and the last is a `const Region **`, not an index
  - `Projection build_projection(std::vector<Region> regions)` — takes a `std::vector<Region>` by value; build one by filling `Region{base, len, kind}` and moving it in
  - `Located scene_locate_off(const Projection &proj, const Recording &rec, uint64_t off)` — **three** args, and it returns a `Located` carrying a `cell`; it does **not** take `(region, off, &u, &v)` and does not return `bool`
  - `StepPlace place_address(const Projection &proj, uint64_t addr)` — two args, returns `{placed, addr, u, v, cell, region, why}`. This is the convenient layout-agnostic probe: it needs no `Recording`.
  - `space::TrajPoint` field order is `{t, addr, fidelity, is_access, tid, placed}` — `t` comes **first**. Prefer named field assignment over brace-init so a field reorder cannot silently transpose values.

## The one decision everything else rests on: `order` survives

`1u << proj.order` is the **cell grid's side length**, and it is read in thirteen source files and ten tests — most load-bearingly [terrain.cpp:111](../../../desktop/src/space/terrain.cpp#L111) (`m.w = m.h = 1 << proj.order`, the height field's own dimensions), plus `converge.cpp:37`, `focus.cpp:86`, `dataribbon.cpp:23`, `hotedges.cpp:112`, `locate.cpp:21`, `stepplace.cpp:45` and `pick.cpp`.

**The atlas keeps that grid.** `order` stops being a *Hilbert* concept and becomes exactly what those call sites already use it for — the plane's cell quantisation — and its selection rule (`smallest order in [6,12] with 4^order >= total`) is unchanged, so `test_projection.cpp:105`'s `p.order == 9` stays true and is merely re-worded. There is no second grid size and no `atlas_cells_per_side()`; an earlier draft invented one and it would have been a parallel source of truth for a number `1 << order` already carries.

What actually changes is only **which cell a domain offset lands in**: a Hilbert index walk becomes a treemap rectangle, a per-region byte→cell quantisation, and a serpentine walk inside the rectangle. Consequences, stated up front because they are contracts, not details:

| | Hilbert (today) | Atlas |
|---|---|---|
| Cells **owned** by a named region | `total / 4^order` — the rest is one connected padding blob at the tail of the curve, owned by nobody | **all `4^order`**, by construction: the rects tile the grid |
| Cells that **decode** to an address | `total / 4^order` | `total / 4^order` — unchanged; see the honesty note below |
| Byte-exact `project`→`unproject` round trip | guaranteed when the domain fits the plane | **not guaranteed** — a cell covers `bytes_per_cell` bytes and `unproject` returns the first of them |
| Region-level round trip | guaranteed | **guaranteed** for every cell that decodes; a cell inside a rect but past its region's bytes *refuses*, exactly as a Hilbert padding cell already does |
| `Terrain` dimensions, cell indices, `y*n+x` arithmetic | `1 << order` | **unchanged** |

The byte-exact row is the one to watch: [test_projection.cpp:110-120](../../../desktop/test/test_projection.cpp#L110) asserts byte-exact round trip over 10 000 addresses. That assertion is a **Hilbert-layout property** and must be scoped to `Layout::Hilbert` in Task 2, with the atlas asserting the region-level contract instead. Silently weakening it would be the wrong move — the two layouts genuinely promise different things, and the tests should say so.

**Honesty note — what "100 % packed" does and does not mean.** No layout can decode more cells than the domain has bytes, and `order` is the smallest with `4^order >= total`, so the decodable fraction `total / 4^order` sits in `(1/4, 1]` under **both** layouts. The atlas does not change it, and an earlier draft's claim that packing "is the occupancy fix" was wrong on that point. What the atlas changes is **where the undecodable cells sit and what they mean**: under Hilbert they are one connected blob at the tail of the curve that belongs to no region and can be labelled as nothing; under the atlas every cell belongs to some region's rectangle, and the slack is the bounded rounding tail inside each rect. The floor becomes *decodable* — point at a rectangle and it names a region, at a size proportional to that region's length — and that is Component 1's real claim. Do not restate it as "the floor is now fully painted": it is not, and a test asserting so would fail. Task 7b's screen-space gate is written accordingly.

---

### Task 1: Clamp the runaway worldline height (standalone defect fix)

Lands first and independently of the re-encoding, so the live defect is fixed regardless of when the substrate work completes.

**And it is not merely interim.** The spec calls this an "interim guard" and says Component 2 "deletes the unbounded-Y defect at the root", which reads as though Task 5 makes this commit redundant. It does not: `traj_scale_` also places the observed-lifetime pillars, the sediment strata and the access arcs — three opt-in layers that **keep** trace time on Y (see Task 5's resolution for why, and for the line references) — and a `mem` step past `nsteps` blows *their* geometry out of the envelope by exactly the same arithmetic. This clamp keeps bounding all three after the worldline flattens.

**Files:**
- Create: `desktop/src/scene3d/trajscale.h` — the scale rule, **header-only**
- Modify: `desktop/src/scene3d/scene.cpp:798-802` — call it
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
#include <cmath>
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

Then in `Scene::set_trajectories`, `#include "scene3d/trajscale.h"` and replace the `const float scale = ...` at [scene.cpp:800-801](../../../desktop/src/scene3d/scene.cpp#L800) with a pre-pass for `max_t` followed by:

```cpp
uint64_t max_t = 0;
for (const space::Trajectory &tr : ts.trajectories)
    for (const space::TrajPoint &pt : tr.points)
        max_t = std::max(max_t, pt.t);
const float scale = scene_traj_scale(nsteps, max_t, time_scale);
```

`traj_scale_ = scale` on the next line is unchanged: `render()` derives `uHeadY`, the vehicle's `tail_half` and `time_cut_y` from it ([scene.cpp:1470-1475](../../../desktop/src/scene3d/scene.cpp#L1470)), and all three stay correct — they simply now sit inside the envelope too.

- [ ] **Step 5: Run test to verify it passes**

Run: `make build/desktop_test_scene_traj && ./build/desktop_test_scene_traj`
Expected: PASS.

- [ ] **Step 6: Run the whole suite before committing — this task can churn goldens**

Run: `make desktop-test`

`traj_scale_` feeds the rendered frame, and this task **changes** it for exactly
the recordings where `max_t + 1 > nsteps`. The ordinary case is bit-identical
(`max(nsteps, max_t + 1) == nsteps` whenever the trace covers its own steps, so
the scale is the same `0.6f / nsteps` it was), which is why no churn is
*expected* — but "expected" is not "verified", and a golden that does carry a
`mem` step past `nsteps` is precisely the recording this task exists for. If an
image golden moves here, that is the defect being fixed becoming visible, not a
regression: regenerate it in the same commit and say so in the message.

- [ ] **Step 7: Commit**

```bash
git add desktop/src/scene3d/trajscale.h desktop/src/scene3d/scene.cpp desktop/test/test_scene_traj.cpp mk/desktop.mk
git commit -m "scene3d: bound the worldline envelope when a mem step outlasts nsteps"
```

---

### Task 2: The atlas layout in `space::Projection`

**Files:**
- Modify: `desktop/src/space/types.h` — the enum, the `layout` member, `rects` (the `Projection` struct lives here, **not** in `projection.h`)
- Modify: `desktop/src/space/projection.h` — declare `rebuild_layout()`
- Modify: `desktop/src/space/projection.cpp` — `project()`/`unproject()` (`:335-370`), `region_cells()` (`:404-434`)
- Test: `desktop/test/test_projection.cpp`

**Interfaces:**
- Consumes: `Projection::regions`, `Projection::domain_off`, `Projection::order` (all unchanged).
- Produces: `enum class Projection::Layout { Hilbert, Atlas }`, a `Layout layout` member defaulting to `Hilbert`, and two vectors populated only for `Atlas` — `std::vector<AtlasRect> rects` (index-parallel to `regions`) and `std::vector<AtlasNode> nodes` (the split tree `unproject` descends).

Both structs go in `space/types.h` **above `struct Projection`**, at namespace scope:

```cpp
// One region's rectangle on the cell grid, half-open in CELL coordinates over
// the n = 1<<order plane, in the SAME index order as Projection::regions so a
// caller joins them by index with no second key. Cell coordinates rather than
// floats because the tiling guarantee is about cells: the rects tile the grid
// exactly — every cell belongs to exactly one rect, none overlap — and that is
// checkable in integers with no epsilon.
struct AtlasRect {
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// The binary space partition `rects` was cut from, so a cell resolves to its
// region in O(log regions) instead of a scan. This is NOT premature:
// unproject() runs ONCE PER CELL over the WHOLE plane in build_terrain
// ([terrain.cpp:119](../../../desktop/src/space/terrain.cpp#L119) — "one
// O(cells) sweep"), which at order 12 is 16.7 M calls, so a linear walk of
// `rects` would make terrain construction O(cells x regions).
struct AtlasNode {
    uint32_t cut = 0; // the cell boundary this split fell on
    uint8_t axis = 0; // 0 = cut in x (vertical), 1 = cut in y (horizontal)
    // Child references. A NON-NEGATIVE value indexes `nodes`; a NEGATIVE one
    // is a leaf holding the region index, encoded as -(index + 1) — so a leaf
    // costs no node and the tree has exactly regions.size()-1 entries.
    int32_t lo = 0, hi = 0;
};
```

**The layout algorithm, in full.** Three parts, each of which an earlier draft left unstated. Put the helpers in the anonymous namespace of `projection.cpp` beside `d2xy`/`domain_shift`.

***(a) Cell budgets, exact by boundaries rather than by remainders.*** Region *i* owns cells `[b[i], b[i+1])` of the walk, where `b[i] = floor(plane * domain_off[i] / total)`, `b[0] = 0` and `b[nreg] = plane`. Differencing a monotone boundary sequence makes `sum(cells_i) == plane` **exactly**, with no remainder pass to get wrong, and leaves each `cells_i` within one cell of proportional to `len_i`.

```cpp
// floor(plane * p / total) with no 128-bit intermediate. plane == 1<<s is a
// power of two, so the product is a SHIFT; where p is large enough that the
// shift would overflow (p >= 2^40 at the order-12 ceiling — a single 1 TiB
// mapping, rare but not impossible), halve the denominator and the shift
// together instead. That costs at most one cell, and cannot accumulate: the
// boundaries are computed independently and the last one is pinned, not
// computed.
uint64_t plane_boundary(uint32_t order, uint64_t p, uint64_t total) {
    if (total == 0)
        return 0;
    uint32_t s = 2 * order; // plane == 1u << s
    while (s > 0 && p != 0 && (p >> (64 - s)) != 0) {
        total = (total >> 1) + (total & 1); // ceil-halve: never reaches 0
        s--;
    }
    return total ? (p << s) / total : 0;
}
```

***(b) The rectangles: an ORDER-PRESERVING binary split.*** **Not** the Bruls/Huizing/van Wijk squarify an earlier draft named. BHvW sorts by descending area, and `regions` is sorted by base *precisely so memory neighbours become plane neighbours* ([build_projection's own comment](../../../desktop/src/space/projection.cpp#L80)); reordering would break that **and** break `rects`' index-parallelism with `regions`, which the whole "join by index, no second key" contract rests on. BHvW's strip packing also does not tile exactly once budgets are integers — the earlier draft's "the budgets sum to `n*n` and the strips tile, so the grid is covered by construction" does not follow, because tiling is a property of the geometry, not of the budgets. This split keeps address order, always cuts the **longer** side so aspect ratios stay readable, and cuts on an integer cell boundary — so children tile their parent exactly at every level, and the root tiling is therefore exact.

**Feasibility is chosen, never assumed.** An earlier draft picked `mid` purely by budget and then *clamped* the cut to reserve `need_l`/`need_r` columns. That clamp can invert: when `need_l + need_r > w` the `std::min(std::max(...))` collapses the cut past the opposite edge, a child gets zero width, and the recursion divides by zero at `(mid - lo + h - 1) / h`. It is a `ceil()` rounding artefact — `ceil(a/h) + ceil(b/h)` can exceed `ceil((a+b)/h)` — so it fires even though the rect *does* have a cell per region. Reproduced under ASan at 3809 regions on a 4096-cell plane; the draft's stated precondition ("the clamps below preserve it") was simply false, and its `nreg > plane` guard is far too loose to catch it.

The fix is to restrict the scan that already runs: **consider only splits this rect can actually realise.** No second pass, no clamp that can invert, and the reserved columns come out of the same test that chose `mid`.

```cpp
// Fills *out with this subtree's encoded child reference — a `nodes` index, or
// -(region index + 1) for a leaf — and returns false if the subtree admits no
// realisable split at all, which rebuild_layout answers by falling back to
// Hilbert. Verified exhaustively for every nreg in [2, 4096] on the order-6
// plane and over 3000 randomised skewed length distributions: the refusal
// never fires while nreg <= plane, so it is a guard rather than a live path.
bool split_rect(const std::vector<uint64_t> &b, size_t lo, size_t hi,
                AtlasRect r, std::vector<AtlasRect> &rects,
                std::vector<AtlasNode> &nodes, int32_t *out) {
    const uint32_t w = r.x1 - r.x0, h = r.y1 - r.y0;
    if (uint64_t(w) * uint64_t(h) < uint64_t(hi - lo))
        return false; // fewer cells than regions: unrepresentable, not clampable
    if (hi - lo == 1) {
        // The leaf takes the WHOLE rect. This is what makes the tiling exact,
        // and it is why a region's final cell count can differ from its budget
        // by the rounding accumulated down the tree — the budget picks the
        // cut, the geometry picks the area, and the geometry wins.
        rects[lo] = r;
        *out = -static_cast<int32_t>(lo) - 1;
        return true;
    }
    const uint64_t span = b[hi] - b[lo];
    // Always cut the LONGER side, so aspect ratios stay readable.
    const bool cut_x = w >= h;
    const uint32_t across = cut_x ? h : w; // cells in one column (or row)
    const uint32_t along = cut_x ? w : h;  // columns (or rows) to divide
    if (across == 0)
        return false;
    // Cut the region RUN as near the half-budget as possible — but only among
    // the splits that FIT: each side needs a cell per region, so a candidate is
    // admissible only when the columns it forces on both sides still add up to
    // `along`. Testing feasibility HERE rather than clamping afterwards is what
    // makes the invariant real. Linear, not binary-searched: nreg is small next
    // to the cell count and a scan cannot get the tie-breaking wrong.
    size_t mid = 0;
    uint64_t best = UINT64_MAX;
    uint32_t need_l = 0, need_r = 0;
    for (size_t i = lo + 1; i < hi; i++) {
        const uint32_t nl = uint32_t((i - lo + across - 1) / across);
        const uint32_t nr = uint32_t((hi - i + across - 1) / across);
        if (uint64_t(nl) + uint64_t(nr) > along)
            continue; // this rect cannot realise that split
        const uint64_t left = b[i] - b[lo];
        const uint64_t d =
            left * 2 > span ? left * 2 - span : span - left * 2;
        if (d < best) {
            best = d;
            mid = i;
            need_l = nl;
            need_r = nr;
        }
    }
    if (best == UINT64_MAX)
        return false; // no realisable split anywhere in the run
    const uint64_t left = b[mid] - b[lo];
    // need_l <= along - need_r is guaranteed by the scan, so this clamp can
    // only narrow — it can no longer invert the rect.
    uint32_t cut = uint32_t((uint64_t(along) * left + span / 2) / span);
    cut = std::min(std::max(cut, need_l), along - need_r);
    AtlasNode nd;
    AtlasRect a = r, c = r;
    if (cut_x) {
        nd.axis = 0;
        nd.cut = r.x0 + cut;
        a.x1 = c.x0 = nd.cut;
    } else {
        nd.axis = 1;
        nd.cut = r.y0 + cut;
        a.y1 = c.y0 = nd.cut;
    }
    // Reserve OUR slot before recursing — the children append to `nodes`.
    const size_t self = nodes.size();
    nodes.push_back(AtlasNode{});
    if (!split_rect(b, lo, mid, a, rects, nodes, &nd.lo))
        return false;
    if (!split_rect(b, mid, hi, c, rects, nodes, &nd.hi))
        return false;
    nodes[self] = nd;
    *out = static_cast<int32_t>(self);
    return true;
}
```

***(c) Bytes per cell — the step the earlier draft omitted entirely.*** `off` is a **byte** offset and the serpentine walk indexes **cells**; the draft's `r = off / w, c = off % w` silently equated the two, which sends any region whose `len` exceeds its rect's cell count straight out of its own rectangle. Hilbert quantises globally, `d >> domain_shift(order, total)` ([projection.cpp:340](../../../desktop/src/space/projection.cpp#L340)); the atlas quantises **per region**, because each region has its own cell budget.

```cpp
// Bytes covered by one cell of region i's rect. max(1, ...) because a region
// can be granted MORE cells than it has bytes (the plane is up to 4x the
// domain), in which case one byte per cell is the finest honest quantisation
// and the rect's tail cells simply decode to nothing.
uint64_t atlas_bytes_per_cell(uint64_t len, uint64_t cells) {
    if (cells == 0)
        return 1;
    const uint64_t bpc = (len + cells - 1) / cells; // ceil
    return bpc ? bpc : 1;
}
```

`k = off / bytes_per_cell` is then always `< cells`, because `k_max = (len-1)/ceil(len/cells) <= (len-1)*cells/len < cells`. That bound is what keeps a region's addresses inside its own rect, and it is worth asserting in the implementation.

***(d) The serpentine walk, both directions.*** Row-major would put cell `w-1` and cell `w` at opposite ends of the rect; reversing odd rows keeps consecutive cells adjacent **across the row break**. That is the locality Hilbert was bought for, kept where it still means something — inside one region.

```cpp
void atlas_cell(const AtlasRect &r, uint64_t k, uint32_t *x, uint32_t *y) {
    const uint32_t w = r.x1 - r.x0;
    const uint32_t row = uint32_t(k / w);
    uint32_t col = uint32_t(k % w);
    if (row & 1u)
        col = w - 1u - col; // serpentine: odd rows run right-to-left
    *x = r.x0 + col;
    *y = r.y0 + row;
}
uint64_t atlas_ordinal(const AtlasRect &r, uint32_t x, uint32_t y) {
    const uint32_t w = r.x1 - r.x0;
    const uint32_t row = y - r.y0;
    uint32_t col = x - r.x0;
    if (row & 1u)
        col = w - 1u - col; // its own inverse
    return uint64_t(row) * w + col;
}
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

**First, scope the existing byte-exact round-trip loop — and pin it, do not merely comment it.** The 10 000-address check at [test_projection.cpp:110-120](../../../desktop/test/test_projection.cpp#L110) is a Hilbert-layout property (see "the one decision" above). It runs on `main()`'s `p`, which is default-constructed today — but **Task 10 flips that default to `Atlas`**, so "it already runs on a Hilbert projection" stops being true the moment the last task lands. Make the layout explicit rather than inherited:

```cpp
// This loop asserts the BYTE-EXACT round trip, which is a Hilbert-layout
// promise and not an atlas one (the plan's contract table says so: an atlas
// cell covers bytes_per_cell bytes and unproject returns the first of them).
// Pinned to Hilbert EXPLICITLY rather than relying on the struct default,
// because Task 10 changes that default. The atlas's own contract — the
// REGION-level round trip — is asserted in the atlas blocks below.
Projection h = p;
h.layout = Projection::Layout::Hilbert;
rebuild_layout(h);
```

and run the existing loop against `h` instead of `p`. (It happens to survive the flip unpinned, because this fixture's plane exceeds its domain so every region gets `bytes_per_cell == 1` — but that is an accident of three fixture lengths, not a property, and a later fixture edit would turn it into a mystery failure in an unrelated-looking test.)

Then, inside `main()`:

```cpp
// --- the atlas layout: every cell claimed, decodable, locally contiguous ----
{
    // 4096 : 61440 == 1 : 15, so the code rect gets 1/16 of the cell budget.
    static const Ref kSmall = {0x0000000000400000ull, 4096, Region::Code};
    static const Ref kBig = {0x0000000001000000ull, 61440, Region::Mmap};
    const Projection p = atlas_of({kSmall, kBig});
    const uint32_t n = 1u << p.order;

    // The whole point: no power-of-4 padding, so no empty three-quarters. In
    // cells, not area — the grid is what Terrain and every (u,v)-keyed layer
    // are indexed by, so cells are the unit the claim has to be true in.
    uint64_t claimed = 0;
    for (const AtlasRect &r : p.rects)
        claimed += uint64_t(r.x1 - r.x0) * uint64_t(r.y1 - r.y0);
    check("the atlas claims every cell of the grid",
          claimed == uint64_t(n) * uint64_t(n),
          "claimed " + std::to_string(claimed) + " of " +
              std::to_string(uint64_t(n) * uint64_t(n)) + " cells");

    // Disjoint as well as covering: a cell owned twice would make unproject
    // ambiguous and quietly hand a caller the wrong region.
    std::vector<uint8_t> seen(size_t(n) * n, 0);
    bool overlap = false;
    for (const AtlasRect &r : p.rects)
        for (uint32_t y = r.y0; y < r.y1; y++)
            for (uint32_t x = r.x0; x < r.x1; x++)
                if (seen[size_t(y) * n + x]++)
                    overlap = true;
    check("no two atlas rects share a cell", !overlap,
          "a cell was claimed by more than one region");

    // regions come out sorted by base, so index 0 is the code region.
    const AtlasRect &code = p.rects[0];
    const double code_frac = double(code.x1 - code.x0) *
                             double(code.y1 - code.y0) / (double(n) * n);
    check("rect area is proportional to Region::len",
          std::fabs(code_frac - 1.0 / 16.0) < 0.02,
          "code rect covered " + std::to_string(code_frac) + ", wanted 0.0625");

    // The REGION round trip is the atlas's contract — cell quantisation means
    // the exact byte need not survive (see the plan's contract table), but the
    // region must, because that is what makes the floor decodable.
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

    // region_cells() walks the layout's own mapping (projection.cpp:404), so it
    // needs an atlas branch. Under the atlas it returns the region's RECT —
    // every cell the region OWNS, a superset of the cells its addresses reach
    // (the rect's rounding tail is owned but does not decode). Ownership is
    // what zoning and labelling want, and it keeps test_focus's containment
    // and disjointness contracts true under both layouts.
    for (size_t i = 0; i < p.regions.size(); i++) {
        const std::vector<uint32_t> cells = region_cells(p, i);
        const AtlasRect &r = p.rects[i];
        check("region_cells matches the region's rect under the atlas",
              cells.size() == size_t(r.x1 - r.x0) * size_t(r.y1 - r.y0),
              "region " + std::to_string(i) + " reported " +
                  std::to_string(cells.size()) + " cells for a " +
                  std::to_string(r.x1 - r.x0) + "x" +
                  std::to_string(r.y1 - r.y0) + " rect");
    }
}
{
    // Serpentine order within a region: neighbouring CELLS stay neighbours
    // ACROSS THE ROW BREAK. That break is the whole content of the claim —
    // plain row-major already keeps neighbours adjacent WITHIN a row, and an
    // earlier draft's version of this test only ever exercised that, on byte
    // offsets 64 apart which are not adjacent under any layout.
    static const Ref kOne = {0x0000000000400000ull, 65536, Region::Code};
    const Projection p = atlas_of({kOne});
    const uint32_t n = 1u << p.order;
    const float cell = 1.0f / static_cast<float>(n);
    // 65536 bytes over a 256x256 plane is exactly one byte per cell, so a byte
    // offset IS a cell ordinal here. STATED, not assumed: every other fixture
    // quantises, and the next block is the one that pins that.
    check("the serpentine fixture is 1 byte per cell",
          uint64_t(n) * n == 65536ull && p.rects.size() == 1,
          "the fixture no longer maps one byte to one cell; the offsets below "
          "would stop being cell ordinals");
    const uint32_t w = p.rects[0].x1 - p.rects[0].x0;
    auto cells_apart = [&](uint64_t off_a, uint64_t off_b) {
        float ua = 0, va = 0, ub = 0, vb = 0;
        if (!p.project(0x400000ull + off_a, &ua, &va) ||
            !p.project(0x400000ull + off_b, &ub, &vb))
            return 1e9f;
        return std::hypot(ub - ua, vb - va) / cell;
    };
    check("consecutive cells within a row are adjacent", cells_apart(0, 1) < 1.5f,
          "cells 0 and 1 landed " + std::to_string(cells_apart(0, 1)) +
              " cells apart");
    // The discriminating case: under row-major these are w-1 cells apart.
    check("the row break stays adjacent — the serpentine reverses odd rows",
          cells_apart(w - 1, w) < 1.5f,
          "the last cell of row 0 and the first of row 1 landed " +
              std::to_string(cells_apart(w - 1, w)) +
              " cells apart; row-major would give " + std::to_string(w - 1));
}
{
    // The per-region byte->cell quantisation. A domain larger than the order-12
    // ceiling (4^12 == 16777216 cells) forces bytes_per_cell > 1, which is the
    // case an earlier draft's algorithm had no rule for at all: it treated a
    // byte offset as a cell ordinal and walked straight out of the region's
    // own rectangle.
    static const Ref kBigOne = {0x0000000010000000ull, 64ull << 20,
                                Region::Code};
    const Projection p = atlas_of({kBigOne});
    check("a domain past the cell ceiling pins order at 12", p.order == 12,
          "got order " + std::to_string(p.order));
    // 64 MiB over 4^12 cells is 4 bytes per cell.
    float u0 = 0, v0 = 0, u3 = 0, v3 = 0, u4 = 0, v4 = 0;
    const uint64_t b = 0x10000000ull;
    check("offset 0 projects", p.project(b, &u0, &v0), "refused");
    check("offset 3 projects", p.project(b + 3, &u3, &v3), "refused");
    check("offset 4 projects", p.project(b + 4, &u4, &v4), "refused");
    check("bytes inside one cell share that cell", u0 == u3 && v0 == v3,
          "offsets 0 and 3 should quantise to the same cell at 4 bytes/cell");
    check("the next cell's worth of bytes moves on", u0 != u4 || v0 != v4,
          "offset 4 should have crossed into the next cell");
    // And the region contract still holds where the byte-exact one cannot:
    // every one of these lands back in the region it came from.
    for (uint64_t off : {uint64_t(0), uint64_t(3), uint64_t(4),
                         (64ull << 20) - 1}) {
        float u = 0, v = 0;
        if (!p.project(b + off, &u, &v)) {
            fail("quantised project", "refused an in-domain offset");
            continue;
        }
        uint64_t back = 0;
        const Region *got = nullptr;
        check("a quantised cell still decodes to its own region",
              p.unproject(u, v, &back, &got) && got != nullptr &&
                  got->base == b,
              "offset " + std::to_string(off) + " left its region");
    }
}
{
    // A SATURATED plane: as many regions as the smallest plane has cells. This
    // is the case the earlier draft's clamp crashed on — at 3809 one-byte
    // regions on the 4096-cell order-6 plane, need_l + need_r exceeded the
    // rect's width, the cut collapsed past the far edge, a child rect got zero
    // height and split_rect divided by zero. Unreachable from a real /proc/maps
    // (every mapping is at least a page, so `order` outruns the region count
    // long before this), but a synthetic Projection is exactly what this
    // directory builds, so the guard is tested rather than assumed.
    std::vector<Ref> many;
    for (uint64_t i = 0; i < 4000; i++)
        many.push_back({0x1000ull + i * 0x10000ull, 1, Region::Code});
    const Projection sat = atlas_of(many);
    check("a saturated plane still produces an atlas", sat.rects.size() == 4000,
          "rebuild_layout fell back, or built a partial rects vector");
    bool degenerate = false;
    for (const AtlasRect &r : sat.rects)
        if (r.x1 <= r.x0 || r.y1 <= r.y0)
            degenerate = true;
    check("no rect is empty or inverted at saturation", !degenerate,
          "a zero-area rect divides by zero in atlas_cell, and an inverted one "
          "wraps r.x1 - r.x0 to a huge unsigned width");
    // And the layout is still a layout: every region places, and lands home.
    for (const Region &rg : sat.regions) {
        float u = 0, v = 0;
        uint64_t back = 0;
        const Region *got = nullptr;
        check("a saturated-plane region round-trips",
              sat.project(rg.base, &u, &v) && sat.unproject(u, v, &back, &got) &&
                  got != nullptr && got->base == rg.base,
              "a region of a saturated plane lost its own cell");
    }
}
{
    // `order` keeps its meaning and its VALUE under the atlas: it is the
    // plane's cell quantisation, which is what every 1<<order call site
    // already reads it as. Only the address->cell mapping changed. `p` and
    // `kRefs` are main()'s existing locals — this block adds no fixture.
    const Projection a = atlas_of({kRefs[0], kRefs[1], kRefs[2]});
    check("the layout does not change order", a.order == p.order,
          "atlas reported order " + std::to_string(a.order) +
              ", hilbert reported " + std::to_string(p.order));
    check("Terrain's plane side is therefore unchanged",
          (1u << a.order) == (1u << p.order), "the cell grid resized");
}
```

`std::hypot` needs `<cmath>` — add it to the file's includes if it is not already there.

- [ ] **Step 2: Run tests to verify they fail**

Run: `make build/desktop_test_projection && ./build/desktop_test_projection`
Expected: FAIL — `Projection::Layout` not declared.

- [ ] **Step 3: Implement the atlas mapping**

In `space/types.h`, add to `struct Projection` (with `AtlasRect`/`AtlasNode` above it, per the Interfaces block):

```cpp
// Which address->cell mapping this projection uses. Hilbert is the historical
// space-filling walk: it spends BOTH plane axes on address and leaves its
// unused cells as one connected padding blob at the tail of the curve, owned
// by no region. Atlas gives each region a RECTANGLE of the SAME n = 1<<order
// grid, area proportional to Region::len, serpentine within — so every cell
// belongs to a named region and a region is a labellable rectangle rather
// than a blobby snake. Neither layout decodes more than `total` cells (there
// are only `total` bytes); what changes is whether the slack has an owner.
// `order` means the same thing under both.
enum class Layout { Hilbert, Atlas };
Layout layout = Layout::Hilbert;
// The per-region rectangles, index-parallel to `regions`, and the split tree
// they were cut from. Both empty under Hilbert.
std::vector<AtlasRect> rects;
std::vector<AtlasNode> nodes;
```

In `projection.h`, declare:

```cpp
// Recompute the layout for p.layout. For Atlas this fills p.rects with an
// order-preserving binary-split treemap whose cell budgets are proportional to
// Region::len (any other weighting would be a fabricated emphasis — D7) and
// which tiles the n = 1<<order grid exactly, plus p.nodes, the split tree
// unproject() descends. Clears both for Hilbert. Idempotent.
//
// Falls BACK to Hilbert, silently in code but visibly in p.layout, in two
// cases: the plane has fewer cells than there are regions, and the split
// admits no realisable tiling. Both are impossible for a real map — `order` is
// sized from the domain's byte count, so a plane crowded by regions needs a
// synthetic domain — and the second has never been observed at all (see
// split_rect). A caller that cares reads p.layout back; NEVER assume Atlas
// stayed Atlas just because you set it.
void rebuild_layout(Projection &p);
```

`build_projection()` calls `rebuild_layout()` at the end, so a default-constructed projection is consistent without a caller remembering to.

In `projection.cpp`, add the four helpers from the Interfaces block and then:

```cpp
void rebuild_layout(Projection &p) {
    p.rects.clear();
    p.nodes.clear();
    if (p.layout != Projection::Layout::Atlas)
        return;
    const size_t nreg = p.regions.size();
    const uint64_t total = p.domain_off.empty() ? 0 : p.domain_off.back();
    const uint32_t n = uint32_t(1) << p.order;
    const uint64_t plane = uint64_t(n) * n;
    if (nreg == 0 || total == 0 || nreg > plane) {
        p.layout = Projection::Layout::Hilbert; // see the header's note
        return;
    }
    std::vector<uint64_t> b(nreg + 1, 0);
    for (size_t i = 1; i < nreg; i++)
        b[i] = plane_boundary(p.order, p.domain_off[i], total);
    b[nreg] = plane; // PINNED, not computed: the last boundary IS the plane.
    // A cell each, and strictly increasing: a region granted zero cells would
    // get an empty rect that project() could never place an address into.
    // Sweep up, then back down off the pinned top; both fit because nreg <=
    // plane. The upward sweep stops at nreg-1 and NOT at nreg: running it over
    // the pinned entry lets a zero-length trailing region push b[nreg] to
    // plane+1, and the downward sweep starts one below and would never pull it
    // back. Harmless for the tiling — the geometry sets the areas, not the
    // budgets — but it would quietly falsify the pin this comment claims.
    for (size_t i = 1; i < nreg; i++)
        if (b[i] < b[i - 1] + 1)
            b[i] = b[i - 1] + 1;
    for (size_t i = nreg; i-- > 1;)
        if (b[i] + 1 > b[i + 1])
            b[i] = b[i + 1] - 1;
    p.rects.assign(nreg, AtlasRect{});
    p.nodes.reserve(nreg ? nreg - 1 : 0);
    int32_t root = 0;
    if (!split_rect(b, 0, nreg, AtlasRect{0, 0, n, n}, p.rects, p.nodes,
                    &root)) {
        // No realisable tiling. Leave NOTHING half-built: a partially filled
        // `rects` would hand project() a zero-area rect and divide by zero.
        p.rects.clear();
        p.nodes.clear();
        p.layout = Projection::Layout::Hilbert; // see the header's note
    }
}
```

Then branch the two methods. **The Hilbert path stays byte-for-byte unchanged** — the atlas branch goes in front of the existing tail in each.

```cpp
// In Projection::project, after the existing region lookup has produced `lo`
// and `const Region &r` (so the atlas reuses the SAME domain search — there is
// no second address->region resolution to drift):
if (layout == Layout::Atlas && lo - 1 < rects.size()) {
    const AtlasRect &rect = rects[lo - 1];
    const uint64_t cells =
        uint64_t(rect.x1 - rect.x0) * uint64_t(rect.y1 - rect.y0);
    const uint64_t k =
        (addr - r.base) / atlas_bytes_per_cell(r.len, cells);
    uint32_t x = 0, y = 0;
    atlas_cell(rect, k, &x, &y); // k < cells, by the bound in (c)
    *u = (x + 0.5f) / float(uint32_t(1) << order);
    *v = (y + 0.5f) / float(uint32_t(1) << order);
    return true;
}

// In Projection::unproject, after the existing x/y clamp:
if (layout == Layout::Atlas) {
    if (rects.empty())
        return false;
    // O(log regions) descent, NOT a scan over rects — build_terrain calls this
    // once per cell over the whole plane (terrain.cpp:119).
    size_t idx = 0;
    if (regions.size() > 1) {
        int32_t cur = 0;
        for (;;) {
            const AtlasNode &nd = nodes[size_t(cur)];
            const int32_t nxt = ((nd.axis == 0 ? x : y) < nd.cut) ? nd.lo : nd.hi;
            if (nxt < 0) {
                idx = size_t(-(nxt + 1));
                break;
            }
            cur = nxt;
        }
    }
    const Region &reg = regions[idx];
    const AtlasRect &rect = rects[idx];
    const uint64_t cells =
        uint64_t(rect.x1 - rect.x0) * uint64_t(rect.y1 - rect.y0);
    const uint64_t off = atlas_ordinal(rect, x, y) *
                         atlas_bytes_per_cell(reg.len, cells);
    if (off >= reg.len)
        return false; // inside the rect but past the region's bytes — the SAME
                      // refusal the Hilbert path gives a padding cell, not a
                      // clamp onto the last byte, which would be a fabricated
                      // address the user could click and be lied to about
    if (addr)
        *addr = reg.base + off;
    if (r)
        *r = &reg;
    return true;
}
```

And `region_cells()` gets an atlas branch. **Placement is load-bearing, so read this before pasting.** The branch goes immediately after the existing `region_index >= proj.regions.size()` bound check at [projection.cpp:405](../../../desktop/src/space/projection.cpp#L405) — that is, *above* the `hi <= lo` early return, and *above* the `const uint32_t n = ...` the Hilbert path declares at `:412`, so it carries its own `n`. Two reasons, and both bite:

- **Above `hi <= lo`.** That guard returns `{}` for a zero-length region. Under the atlas a zero-length region still *owns* a rect — `rebuild_layout`'s strictly-increasing clamp grants every region at least one cell — so returning `{}` there would contradict this task's own "region_cells matches the region's rect" assertion, and `test_focus`'s "owns at least one cell" check with it.
- **Its own `n`.** Pasting the branch "before the Hilbert run walk" but after `:412` compiles; pasting it where it belongs does not, unless it declares `n` itself.

```cpp
if (proj.layout == Projection::Layout::Atlas) {
    if (region_index >= proj.rects.size())
        return out; // rects is empty under a fallback — see rebuild_layout
    const uint32_t n = uint32_t(1) << proj.order; // the Hilbert path's own `n`
    const AtlasRect &r = proj.rects[region_index]; // is declared further down
    out.reserve(size_t(r.x1 - r.x0) * size_t(r.y1 - r.y0));
    for (uint32_t y = r.y0; y < r.y1; y++)
        for (uint32_t x = r.x0; x < r.x1; x++)
            out.push_back(y * n + x);
    std::sort(out.begin(), out.end()); // already ascending, but the caller is
    return out;                        // promised sorted and cheap is cheap
}
```

`region_cells`'s doc comment in `projection.h` currently argues from the Hilbert index run — extend it to state both mappings rather than replacing the Hilbert half, since both remain live. Say explicitly that the atlas set is the region's **rect**: every cell the region *owns*, which is a superset of the cells its addresses *reach* (the rect's rounding tail is owned but does not decode). That superset is what the layer wants — zoning and labelling are about ownership — and it keeps [test_focus.cpp](../../../desktop/test/test_focus.cpp)'s containment and disjointness contracts true under both layouts.

- [ ] **Step 4: Run tests to verify they pass**

Run: `make build/desktop_test_projection && ./build/desktop_test_projection`
Expected: PASS, including every pre-existing Hilbert check — run the whole file.

- [ ] **Step 5: Commit**

```bash
git add desktop/src/space/types.h desktop/src/space/projection.h desktop/src/space/projection.cpp desktop/test/test_projection.cpp
git commit -m "space: add the region-atlas projection layout, tiling the cell grid exactly"
```

---

### Task 3: Prove the shared address funnel is layout-agnostic

**Files:**
- Test only: `desktop/test/test_stepplace.cpp`, `desktop/test/test_focus.cpp`

**Interfaces:**
- Consumes: `Projection::Layout` and `rebuild_layout()` from Task 2.
- Produces: no signature or behaviour change anywhere. This task is a **regression gate**, not a refactor.

**Why this is smaller than it looks.** An earlier draft claimed these helpers "re-derive Hilbert arithmetic" and had to be rewritten. They do not: [locate.cpp:19](../../../desktop/src/space/locate.cpp#L19) and [stepplace.cpp:38](../../../desktop/src/space/stepplace.cpp#L38) already call `proj.project()`, and what they open-code — deliberately, with comments saying so — is only `cell = y*n + x` with `n = 1 << order`, which Task 2 leaves valid by keeping the grid. So the funnel is *already* layout-agnostic and this task's job is to **pin that**, so a later change cannot quietly reintroduce a layout assumption. `region_cells()` was the one genuine exception and Task 2 fixed it.

- [ ] **Step 1: Write the test**

Add to `desktop/test/test_stepplace.cpp`, reusing its helpers. `place_address` is the right probe — it takes `(Projection, addr)` and needs no `Recording`. **Add `#include <cmath>`** to that file first: it includes `<cstdio>`, `<sstream>` and `<string>` but no `<cmath>`, and the block below uses `std::fabs`.

```cpp
// The shared plane arithmetic must agree with the projection under BOTH
// layouts — that agreement is what contains the blast radius of a layout swap.
// If it holds, picking, goto, zoning and every (u,v)-keyed layer follow for
// free, because all of them read `cell` from this one derivation.
{
    std::vector<Region> in;
    Region reg;
    reg.base = 0x0000000000400000ull;
    reg.len = 0x20000;
    reg.kind = Region::Code;
    in.push_back(reg);
    Projection p = build_projection(std::move(in));

    for (const auto layout :
         {Projection::Layout::Hilbert, Projection::Layout::Atlas}) {
        p.layout = layout;
        rebuild_layout(p);
        const std::string name =
            layout == Projection::Layout::Atlas ? "atlas" : "hilbert";
        const uint64_t addr = 0x400040ull;

        const StepPlace sp = place_address(p, addr);
        check("place_address places an in-domain address (" + name + ")",
              sp.placed, sp.why);

        float pu = 0, pv = 0;
        check("project places the same address (" + name + ")",
              p.project(addr, &pu, &pv), "project refused it");
        check("place_address and project agree (" + name + ")",
              std::fabs(sp.u - pu) < 1e-5f && std::fabs(sp.v - pv) < 1e-5f,
              "place gave (" + std::to_string(sp.u) + "," +
                  std::to_string(sp.v) + ") but project gave (" +
                  std::to_string(pu) + "," + std::to_string(pv) + ")");

        // And the cell derivation itself, which is the thing four call sites
        // each keep their own copy of.
        const uint32_t n = uint32_t(1) << p.order;
        uint32_t x = uint32_t(pu * n), y = uint32_t(pv * n);
        if (x >= n) x = n - 1;
        if (y >= n) y = n - 1;
        check("the cell derivation is layout-independent (" + name + ")",
              sp.cell == y * n + x,
              "place_address reported cell " + std::to_string(sp.cell) +
                  ", the shared y*n+x rule gives " + std::to_string(y * n + x));

        check("the region resolves back (" + name + ")",
              sp.region != nullptr && sp.region->base == 0x400000ull,
              "an in-region address resolved to the wrong region");
    }
}
```

Then add the same two-layout sweep around `test_focus.cpp`'s existing `region_cells` membership checks (`:242`, `:269`, `:284`, `:299`) — that file already asserts the containment and disjointness contract, so running it under `Atlas` too is the strongest single guard on Task 2's `region_cells` branch.

**One mechanical obstacle to expect:** `test_focus.cpp:130` is `const space::Projection proj = fixture_proj();`, so you cannot flip its layout in place. Take a copy at the top of the T2 block and sweep over the two:

```cpp
space::Projection hil = proj, atl = proj;
hil.layout = space::Projection::Layout::Hilbert;
atl.layout = space::Projection::Layout::Atlas;
space::rebuild_layout(hil);
space::rebuild_layout(atl);
for (const space::Projection &pj : {std::cref(hil), std::cref(atl)}) {
    // ...the existing :242-:299 bodies, reading `pj` where they read `proj`,
    // and naming the layout in each check string so a failure says which one.
}
```

All four contracts hold under the atlas by construction — a rect is non-empty, its cells are distinct, `project` places inside the region's own rect, and the rects tile without overlap — so this should pass on the first run. If it does not, the bug is in Task 2's `region_cells` branch, not here.

- [ ] **Step 2: Run tests to verify they fail**

Run: `make build/desktop_test_stepplace && ./build/desktop_test_stepplace && make build/desktop_test_focus && ./build/desktop_test_focus`
Expected: FAIL — `Projection::Layout` not declared if Task 2 has not landed; otherwise **these may pass immediately**, and that is the correct outcome for a regression gate over already-correct code. Do not manufacture a failure to satisfy the red-green ritual. If `test_focus` fails under `Atlas`, that is a real Task 2 bug in `region_cells` — fix it there.

- [ ] **Step 3: Run tests to verify they pass**

Run the two binaries above in full, not just the new cases.

- [ ] **Step 4: Commit**

```bash
git add desktop/test/test_stepplace.cpp desktop/test/test_focus.cpp
git commit -m "desktop(test): pin the shared address funnel as layout-agnostic"
```

---

### Task 4: Camera fit-to-content

**Files:**
- Modify: `desktop/src/scene3d/camera.h` — `reset()` is the one-liner at [`:81`](../../../desktop/src/scene3d/camera.h#L81), `top_down()` is [`:84-88`](../../../desktop/src/scene3d/camera.h#L84). (An earlier draft said `:99-107`; that is `eye()`/`view()`.)
- Test: `desktop/test/test_camera.cpp`

**Reuse `frame()`, do not re-derive it.** [`camera.h:75`](../../../desktop/src/scene3d/camera.h#L75) already has `void frame(float u, float v, float new_radius)`, which clamps the target into `[0,1]²` and the radius through `kMinRadius`/`kMaxRadius` — exactly `fit()`'s body minus the extent arithmetic, and its comment already states the non-reorienting rule this task depends on. `fit()` is therefore a two-liner over it (see Step 3), not a second copy of the same clamps.

**Interfaces:**
- Consumes: nothing from earlier tasks — takes plain floats so `camera.h` stays linmath-only.
- Produces: `float Camera::fit_radius(float u0, float v0, float u1, float v1, float whole) const;` and `void Camera::fit(float u0, float v0, float u1, float v1, float whole);`, plus `kWholePlaneRadius` / `kWholePlaneRadiusTopDown`. `reset()` routes through `fit`; `top_down()` routes through `fit_radius` **only**.

**`fit` must reproduce the historical framing EXACTLY — and "exactly" here means bit-identical.** [test_camera.cpp:111](../../../desktop/test/test_camera.cpp#L111) is `c.radius == d.radius`, an exact float comparison against a default-constructed `Camera`, and the same check recurs at `:208`. The golden images depend on it too.

An earlier draft answered this with two *calibrated pad factors*: `radius = extent / tan(0.5 * fovy) * pad`, with `pad` chosen so the whole-plane fit lands on 2.2 and 1.9. **That cannot work.** `1.0f / tanf(0.4f) * 0.9301f` evaluates to ≈ 2.19988, and `2.19988f == 2.2f` is *false* — the check at `:111` would fail, and no pad value makes a two-rounding float product reliably bit-identical across libm versions and architectures. The draft's own Step 4 note ("a failure at `:111` means the calibration is off") pointed the implementer at an unwinnable tuning loop.

**So derive the radius linearly from the known whole-plane framing instead of from `fovy`.** The radius needed to frame an axis-aligned extent is proportional to that extent at a fixed fov, so scaling the *shipped* whole-plane radius by the extent is both the correct geometry and exact by construction: extent `1.0f` returns `2.2f` bit-for-bit, because it is a multiply by one. This drops two magic constants, a `tan` call, and the dependence on `fovy` and on libm.

With a fully-tiled atlas the occupied bounds *are* the unit square, so the fit is a **regression guard**: it only starts moving the camera if the floor ever stops being fully claimed.

- [ ] **Step 1: Write the failing test**

Add to the existing `desktop/test/test_camera.cpp`, reusing its `check` helper:

```cpp
// 3D-axis-budget T4: fit-to-bounds. reset() framed the whole unit plane
// unconditionally, so a floor occupying a fraction of it sat small in a
// mostly-empty viewport with no way to say so.
{
    // BIT-IDENTICAL, not merely close: test_camera.cpp:111 compares radius
    // with ==, and every golden image was rendered at these exact framings.
    // A tolerance here would let a drifting fit through and churn the goldens.
    Camera c;
    c.fit(0.0f, 0.0f, 1.0f, 1.0f, Camera::kWholePlaneRadius);
    check("fitting the whole plane reproduces the default framing exactly",
          c.radius == Camera{}.radius,
          "fit gave radius " + std::to_string(c.radius) + ", the default is " +
              std::to_string(Camera{}.radius));
    Camera t;
    t.fit(0.0f, 0.0f, 1.0f, 1.0f, Camera::kWholePlaneRadiusTopDown);
    check("fitting the whole plane top-down reproduces its framing exactly",
          t.radius == 1.9f,
          "fit gave radius " + std::to_string(t.radius) + ", wanted 1.9");
}
{
    // top_down() must NOT recentre. It is the "3D to find, 2D to read"
    // collapse: you look straight down at WHERE YOU ARE. Snapping the target
    // back to the plane centre would lose the mental map the user just built —
    // the same reason frame() refuses to reorient (camera.h's own comment).
    Camera c;
    c.pan(0.3f, -0.2f);
    const float tx = c.target[0], tz = c.target[2];
    c.top_down();
    check("top_down keeps the target it was given",
          c.target[0] == tx && c.target[2] == tz,
          "top_down moved the target to (" + std::to_string(c.target[0]) + "," +
              std::to_string(c.target[2]) + "), losing the user's place");
    check("top_down still reframes the whole plane", c.radius == 1.9f,
          "radius " + std::to_string(c.radius));
}
{
    Camera whole;
    whole.fit(0.0f, 0.0f, 1.0f, 1.0f, Camera::kWholePlaneRadius);
    Camera quarter;
    quarter.fit(0.0f, 0.0f, 0.5f, 0.5f, Camera::kWholePlaneRadius);
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
    c.fit(0.0f, 0.0f, 0.001f, 0.001f, Camera::kWholePlaneRadius);
    check("fit respects the dolly clamps",
          c.radius >= Camera::kMinRadius && c.radius <= Camera::kMaxRadius,
          "radius " + std::to_string(c.radius) + " escaped [" +
              std::to_string(Camera::kMinRadius) + "," +
              std::to_string(Camera::kMaxRadius) + "]");
}
```

The file's existing `reset restores yaw/pitch/radius` check at `:111` is the real gate on the calibration and must be left untouched.

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/desktop_test_camera && ./build/desktop_test_camera`
Expected: FAIL — no member `fit`.

- [ ] **Step 3: Implement `fit`**

```cpp
// The radii that frame the WHOLE unit plane — this struct's shipped defaults,
// stated once rather than re-derived. Every other framing is these scaled by
// the extent being framed, which is the correct geometry at a fixed fov AND
// exact by construction: an extent of 1 is a multiply by one, so fit() over
// the whole plane reproduces reset()/top_down() BIT-IDENTICALLY. That matters
// because test_camera.cpp:111 compares with == and every golden was rendered
// here. (Deriving these from fovy via 1/tan(fovy/2) and a calibrated pad does
// NOT round back to 2.2f — an earlier draft tried, and it cannot be made to.)
static constexpr float kWholePlaneRadius = 2.2f;
static constexpr float kWholePlaneRadiusTopDown = 1.9f;

// The radius that frames an axis-aligned region of the floor. Clamped through
// the SAME kMinRadius/kMaxRadius dolly uses, so a degenerate region cannot
// produce a camera inside the near plane. const, and separate from fit(),
// because top_down() wants the radius WITHOUT the recentring.
float fit_radius(float u0, float v0, float u1, float v1, float whole) const {
    const float extent = std::fmax(std::fabs(u1 - u0), std::fabs(v1 - v0));
    return clampf(extent * whole, kMinRadius, kMaxRadius);
}

// Frame a region: centre the target on it and pull the radius back to hold it.
// Routed through frame() (:75) rather than re-clamping, because frame() IS
// "recentre without reorienting" and already owns both clamps — a second copy
// of them here is how the two drift.
void fit(float u0, float v0, float u1, float v1, float whole) {
    frame(0.5f * (u0 + u1), 0.5f * (v0 + v1), fit_radius(u0, v0, u1, v1, whole));
    target[1] = 0.0f; // the plane's height is not a camera axis (see pan()).
                      // Already 0 on every path today; stated so the test's
                      // "never lifts the target off the ground plane" check is
                      // guaranteed by this function rather than by its callers.
}
```

`reset()` becomes `*this = Camera{}; fit(0.0f, 0.0f, 1.0f, 1.0f, kWholePlaneRadius);` — every field bit-identical to today, including the target, which `reset` legitimately restores.

`top_down()` keeps `yaw = 0`, `pitch = kPitchLimit` and takes **only the radius**: `radius = fit_radius(0.0f, 0.0f, 1.0f, 1.0f, kWholePlaneRadiusTopDown);`. It must **not** call `fit`. Today's `top_down()` leaves `target` alone, and [shell.cpp:1464](../../../desktop/src/ui/shell.cpp#L1464) invokes it on a camera the user may have panned; recentring would silently teleport them to the plane centre on a keypress whose whole purpose is "show me this, flat". No existing test catches that — the file's top-down block uses a default camera, where the two are indistinguishable — so Step 1 adds one.

- [ ] **Step 4: Run test to verify it passes**

Run: `make build/desktop_test_camera && ./build/desktop_test_camera`
Expected: PASS — the new checks **and** the pre-existing reset/top_down ones. `:111` compares with `==`; if it fails, the radius is being *computed* rather than scaled from `kWholePlaneRadius`. Do not relax that check to a tolerance and do not try to tune a pad factor — see the Interfaces block for why that road has no end.

- [ ] **Step 5: Commit**

```bash
git add desktop/src/scene3d/camera.h desktop/test/test_camera.cpp
git commit -m "scene3d: give the camera a fit-to-bounds preset, calibrated to the current framing"
```

---

### Task 5: Time leaves the spatial budget

> ## The trace-time Y axis has five consumers, not one — and the rule for which of them flatten
>
> **`traj_scale_` is not the worldline's private Y scale.** Earlier drafts' consumer table listed three *uniforms* and missed four *geometry producers* standing on the same axis:
>
> | Consumer of `t * traj_scale_` | Where | Default | Geometry |
> |---|---|---|---|
> | the worldline itself (line shader: `pos.y already IS t*scale`) | [embedded.h:255-257](../../../desktop/src/scene3d/shaders/embedded.h#L255) | **on** | diagonal |
> | causal spurs — anchor and resume heights | [causal.cpp:280-282](../../../desktop/src/scene3d/causal.cpp#L280) | **on** | diagonal |
> | observed-lifetime pillars (58 T4) | [data_layers_gl.cpp:201-204](../../../desktop/src/scene3d/data_layers_gl.cpp#L201) | off | **vertical-only** |
> | access arcs / data ribbon | [data_layers_gl.cpp:246-247](../../../desktop/src/scene3d/data_layers_gl.cpp#L246) | off | diagonal |
> | sediment strata columns | [data_layers_gl.cpp:294-299](../../../desktop/src/scene3d/data_layers_gl.cpp#L294) | off | **vertical-only** |
>
> And the coupling is *documented*: [scene.h:365](../../../desktop/src/scene3d/scene.h#L365) states that **"a spur hangs on a worldline vertex at world Y = t * traj_scale()"**. Flatten the worldline without the spurs and every spur foot detaches from the path it points at.
>
> ### The resolution: flatten what is ON by default; leave the opt-in layers on the axis
>
> The spec appears to contradict itself — Component 2's *"Y carries access density, and nothing else"* against the Non-goal *"the 14 layers and 12 scenes stand"*. It does not, once "Y" is read as **the scene you get by default**, which is the only thing Component 2's complaint is about (a pre-drawn future crowding the default view). An overlay you have to switch on is not competing for that budget: when you ask for a Gantt, you are asking for time on an axis.
>
> That reading is not a compromise invented here — it is [scene.h:60-144](../../../desktop/src/scene3d/scene.h#L60)'s own documented convention, the same one Task 6 pins: a layer that adds geometry defaults ON, a layer that re-lifts the terrain's reading defaults OFF. The split falls exactly along it.
>
> | | Layers | Task 5 does |
> |---|---|---|
> | default **ON** | `vehicle` (the worldline), `crossings` (the spurs) | **flatten both**, through the same `flat_time` so the spur foot stays welded to the path by construction |
> | default **OFF** | `lifetime`, `data_ribbon`, `sediment` | **untouched.** They keep `traj_scale_`, and `scene_traj_scale` (Task 1) keeps bounding them |
>
> **Why not flatten all five.** Two of the opt-in layers are *vertical-only*: both endpoints of every segment they emit share the same `(u,v)`, so trace-time Y is their entire extent. Flattening a lifetime pillar makes it zero-length and GL draws nothing; flattening a sediment column collapses all its bands to one coincident point. For those two, "flatten" is a synonym for "delete" — and the spec's own rationale does not reach them anyway: a Gantt bar is not a pre-drawn future, it is a summary, and [data_layers_gl.cpp:184](../../../desktop/src/scene3d/data_layers_gl.cpp#L184) says so ("Whole-recording geometry — it does not move with the playhead, which is the point of a Gantt"). Deleting them would also undo a deliberate guard: a single-touch pillar is given a one-step stub precisely so it is "never a vanished touch", and flattening turns every pillar into the vanished thing that code exists to prevent.
>
> **The spurs survive flattening**, which is why they can go in the ON group without loss: `rail_lift` is already a constant that [encodes nothing](../../../desktop/src/scene3d/causal.cpp#L247), so a flattened spur is still a tent — out of the floor, across, back down. It loses only the anchor-vs-resume height difference, an *incidental* dwell cue the layer already refuses to claim (`rail_span() == 0`, pinned by `test_crossing.cpp`).
>
> **One residual to look at in the container render, not a blocker:** with the path flat and `sediment` switched on, the worldline runs *underneath* strata that still rise from it. That is a legibility question for Task 10 Step 4, not a correctness one.

**Files:**
- Modify: `desktop/src/scene3d/trajscale.h` — add `traj_vertex_y` and `comet_window` (Task 1 created this header)
- Modify: `desktop/src/scene3d/scene.h`, `desktop/src/scene3d/scene.cpp` (`set_trajectories`, `render`) — call them
- Modify: `desktop/src/scene3d/causal.cpp:280-282` — flatten the spur feet through the **same** `traj_vertex_y` the path uses, so `scene.h:365`'s promise holds by construction rather than by coincidence
- Modify: `desktop/src/scene3d/shaders/embedded.h:255-257` — the line shader compares `pos.y` against `uHeadY`; with flat Y that degenerates (see Step 3)
- Modify: `desktop/src/ui/shell.cpp:1829` — **gate** the `draw_trajectory_ruler` call, do not delete it (see Step 3)
- Modify: `desktop/src/scene3d/hud.cpp:383` + `desktop/src/scene3d/hud.h:170-180` — re-caption the ruler: it now measures the axis the three opt-in layers ride, **not** the trajectory, which has left it
- **Not modified:** `data_layers_gl.cpp`. The lifetime pillars, access arcs and sediment strata keep `traj_scale_` untouched — see the resolution above. If a diff to that file appears in this task, something has gone wrong.
- Test: `desktop/test/test_scene_traj.cpp` (created and registered by Task 1 — no `mk/desktop.mk` change needed here)

**Interfaces:**
- Consumes: `scene_traj_scale` (Task 1) — **still live, and permanently so**: the three opt-in layers ride the scale it computes, and its `max(nsteps, max_t + 1)` guard bounds their geometry as well as the path's. An earlier readiness review claimed this function lost its last consumer once the worldline flattened; that was wrong, and `data_layers_gl.cpp` is why.
- Produces, in `scene3d/trajscale.h` (header-only, so the test needs no GL):
  - `float traj_vertex_y(uint64_t t, float scale, bool flat)`
  - `std::pair<uint64_t, uint64_t> comet_window(uint64_t follow_step, uint32_t tail)`
- Produces, on `Scene`: `uint32_t comet_tail = 256;` (trail length in **steps**, the spec's HUD control) and `bool flat_time = true;`.

**On `flat_time` needing no setter.** An earlier draft wrote "default `true` for the Plane scene", which reads as though some caller must select it per kind — `Scene` has no `SceneKind` member, so there would be nowhere to hang that. It does not need one: `Scene` **is** the Plane scene's renderer, exclusively. [gl_scene_host.cpp:74](../../../desktop/src/ui/gl_scene_host.cpp#L74) routes every other kind to `standalone_` before `Scene` is touched, and the four standalone kinds carry their own vertical quantities (execution step, invocation ordinal, call depth, byte magnitude). So a plain member default is the whole of it, and `flat_time = false` exists only to keep the pre-flattening path testable — **not** for "the other scenes", which never call `set_trajectories` at all.

**The comet already exists — this task does not build one.** `SceneLayers::vehicle` is documented as "the followed-citizen head + comet tail" ([scene.h:59](../../../desktop/src/scene3d/scene.h#L59)), registered as *"where is the followed thread right now?"* ([layers.cpp:44](../../../desktop/src/scene3d/layers.cpp#L44)), and `render()` already locates the head via `find_head()` and feeds the shader `uHeadY` ([scene.cpp:1436](../../../desktop/src/scene3d/scene.cpp#L1436), `:1466-1502`). What this task does is **remove trace time from Y** and repair the three consumers that read time *through* Y once it is gone:

| Consumer | Today | After |
|---|---|---|
| `head_y = follow_step * traj_scale_` (`:1470`) | the vehicle head's Y | the head is found by **step**, not height — `find_head` already returns a position; the uniform becomes the window from `comet_window` |
| `tail_half = 3.0f * traj_scale_` (`:1471`) | a ±Y band ≈ 3 steps | collapses to zero when Y is flat — replaced by `comet_tail` **in steps**, which is what it was always approximating |
| `time_cut_y = slice_step * traj_scale_` (`:1475`) | the terrain-playhead dimming cut, a *different clock* | becomes a step comparison, not a height one. **The two clocks stay distinct** — this is the spec's explicit non-goal; do not fuse `slice_step` and `follow_step` |
| `ay/ry = anchor_t/resume_t * traj_scale_` ([causal.cpp:280-282](../../../desktop/src/scene3d/causal.cpp#L280)) | the spur's foot, on the worldline it hangs from | flattens **with** the path, through the same `traj_vertex_y` call. `scene.h:365` promises the two coincide; leaving the spur on `traj_scale_` while the path flattens would *break* that promise, not preserve it. `rail_y = max(ay, ry) + rail_lift` needs no change — `rail_lift` is already a constant, so the tent survives |

**Z-fighting is a real consequence, not a detail.** A flat worldline at `y = 0` is coplanar with the terrain floor and will z-fight or vanish under any cell with nonzero height. Lift the path by a small constant above the terrain's own surface at that cell (or draw it with a depth offset) — and say which in a comment, because "the path disappeared into the ground" is the failure this note exists to prevent.

- [ ] **Step 1: Write the failing test**

Both rules go in `scene3d/trajscale.h` (Task 1's header) as pure functions, so `test_scene_traj.cpp` keeps linking nothing but its own object — no GL, no `Scene`.

Append to `desktop/test/test_scene_traj.cpp`, inside `main()`:

```cpp
// Flat time: trace time is no longer a spatial axis, so every worldline vertex
// sits on the floor and the path is read through the playhead instead.
{
    const float scale = scene_traj_scale(1000, 999, 0.0f);
    for (uint64_t t : {uint64_t(0), uint64_t(1), uint64_t(500), uint64_t(999)}) {
        const float y = traj_vertex_y(t, scale, /*flat=*/true);
        check("flat time flattens the worldline", y == 0.0f,
              "step " + std::to_string(t) + " sat at y=" + std::to_string(y));
    }
    // With flat time OFF the pre-flattening behaviour is bit-identical. NOT
    // because "the other scenes still spatialise time" — they never call this
    // at all (gl_scene_host.cpp:74 routes them to standalone_) — but because
    // the same scale still places the lifetime pillars, sediment strata,
    // access arcs and spur feet, and this is the arithmetic they share.
    check("spatial time still lifts by trace step",
          traj_vertex_y(999, scale, false) == 999.0f * scale,
          "the spatial-time path lost its height");
}
// The trail is a window ENDING at the followed step. Nothing outside it is
// discarded — the path is real, just not recent — so this selects emphasis,
// not existence. Note it is keyed on follow_step (the vehicle's clock), NOT
// slice_step (the terrain's): the spec forbids fusing the two.
{
    const auto w = comet_window(/*follow_step=*/500, /*tail=*/100);
    check("the comet trail ends at the followed step", w.second == 500u,
          "window ended at " + std::to_string(w.second));
    check("the comet trail starts one tail behind it", w.first == 400u,
          "window started at " + std::to_string(w.first));
}
{
    // Saturating at zero: near the start of a recording the trail is short,
    // never negative and never wrapped.
    const auto w = comet_window(/*follow_step=*/10, /*tail=*/100);
    check("the comet trail saturates at the start of the recording",
          w.first == 0u && w.second == 10u,
          "window was [" + std::to_string(w.first) + "," +
              std::to_string(w.second) + "]");
}
```

Add to `scene3d/trajscale.h` (which gains `#include <utility>` for `std::pair`):

```cpp
// A worldline vertex's world Y. With `flat`, trace time is NOT a spatial axis
// — the path lies on the floor and is read through the playhead — so this is
// 0 and the caller lifts the whole path clear of the terrain by a constant.
//
// The !flat branch is NOT "for the other scenes": no other scene reaches this
// code (gl_scene_host.cpp:74 routes every non-Plane kind to standalone_). It
// is the pre-flattening path, kept testable, and it is the SAME arithmetic the
// three OPT-IN layers still use on their own axis — the lifetime pillars, the
// sediment strata and the access arcs, which keep trace time on Y because a
// layer you switch on is asking for it (see this task's resolution).
//
// The causal spur foot calls THIS with the same `flat` the path uses, never
// traj_scale_ directly, so scene.h:365's "a spur hangs on a worldline vertex"
// stays true by construction instead of by two call sites agreeing.
inline float traj_vertex_y(uint64_t t, float scale, bool flat) {
    return flat ? 0.0f : static_cast<float>(t) * scale;
}

// The trail window [lo, hi] ending at the FOLLOWED step (the vehicle's own
// clock — never slice_step, which is the terrain's residency playhead and a
// deliberately separate axis), `tail` steps long and saturating at 0.
// render() draws this window full-bright and fades the rest; nothing is
// discarded, because the path outside the window is real, just not recent.
inline std::pair<uint64_t, uint64_t> comet_window(uint64_t follow_step,
                                                  uint32_t tail) {
    const uint64_t lo = follow_step > tail ? follow_step - tail : 0;
    return {lo, follow_step};
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `make build/desktop_test_scene_traj && ./build/desktop_test_scene_traj`
Expected: FAIL — `traj_vertex_y` not declared.

- [ ] **Step 3: Implement flat time**

In `set_trajectories`, emit `traj_vertex_y(pt.t, scale, flat_time)` for every vertex Y (the PC strip, the pick points `pts_pos`, and `pt_pos`) plus the constant terrain lift. In `render()`, replace the Y-derived quantities per the table above and pass the `comet_window` bounds as a step-space uniform, fading by distance from `hi`.

The **line shader** reads the same axis and must move with it: [shaders/embedded.h:255-257](../../../desktop/src/scene3d/shaders/embedded.h#L255) documents that `pos.y already IS t*scale` and compares it against `uHeadY`. With flat Y that comparison degenerates to `0 <= 0` for every vertex, so the head/tail fade would light the whole path at once. Carry the step per vertex (or the window in step space) instead — this is a shader change, not only a CPU one, and an earlier draft's consumer table did not mention it.

In `causal.cpp:280-282`, take the spur foot from the same `traj_vertex_y(t, scale, flat_time)` call the path uses — one expression, both consumers — so `scene.h:365`'s promise holds by construction rather than by coincidence. `rail_y` is untouched.

**Gate the `draw_trajectory_ruler` call; do not delete it and do not leave it unconditional.** An earlier draft deleted it, on the reasoning that the function "stays in hud.cpp for the scenes that still spatialise time" — it does not: [shell.cpp:1829](../../../desktop/src/ui/shell.cpp#L1829) is its **only** caller and is already gated to `SceneKind::Plane`. After the resolution above, the trace-time axis exists exactly when one of the three opt-in layers is drawn, so that is the gate:

```cpp
// `f.layers` and NOT `sv.hud.layers`: f.layers is the set AFTER lod_apply
// (shell.cpp:1762), which can clear a layer the reader asked for at distance.
// Gating on the requested set would leave a ruler labelling an axis the
// entity budget had already dropped the geometry for.
if (sv.kind == scene3d::SceneKind::Plane &&
    (f.layers.lifetime || f.layers.data_ribbon || f.layers.sediment))
    scene3d::draw_trajectory_ruler(
        ImGui::GetWindowDrawList(), sv.cam, vp_origin,
        ImVec2(static_cast<float>(fbw), static_cast<float>(fbh)),
        sv.terr.nsteps, s.scene_host->traj_scale());
```

`f` is in scope at the call site — it is built at [shell.cpp:1755-1770](../../../desktop/src/ui/shell.cpp#L1755), in the same function.

Leaving the call unconditional would label an axis the default scene no longer uses — a ruler for nothing, which is worse than no ruler, and exactly the fabricated correspondence the `SceneKind::Plane` gate beside it already guards against. Deleting it would strand the three layers on an unlabelled axis the moment a reader switches one on. Re-caption it too ([hud.h:170-177](../../../desktop/src/scene3d/hud.h#L170) and the caption in `hud.cpp`): it measures trace time for the pillars, strata and arcs — **not** for the trajectory, which has left the axis.

- [ ] **Step 4: Run tests to verify they pass**

Run: `make build/desktop_test_scene_traj && ./build/desktop_test_scene_traj`
Then the whole suite, because `pick.cpp` replays vertex order against these positions: `make desktop-test`.
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add desktop/src/scene3d/trajscale.h desktop/src/scene3d/scene.h desktop/src/scene3d/scene.cpp desktop/src/scene3d/causal.cpp desktop/src/scene3d/hud.cpp desktop/src/scene3d/hud.h desktop/src/scene3d/shaders/embedded.h desktop/test/test_scene_traj.cpp
git commit -m "scene3d: trace time animates rather than occupying the Y axis"
```

---

### Task 6: The motif channels — already shipped; wire and verify only

**Files:**
- Modify: `desktop/test/test_layers.cpp` (assertions only)

**Interfaces:**
- Consumes: `SceneLayers::opcode` and `SceneLayers::crossings`, both of which **already exist**.
- Produces: no new bool, no new registry row, no new classifier.

**Why this task shrank to nothing.** The spec's Component 3 asks for two semantic channels. Both already ship:

| Spec asks for | Already in the tree |
|---|---|
| per-cell opcode-class emission from `CellOpcode::dominant`, abstaining on `Unknown`/low purity | `SceneLayers::opcode`, registered *"what kind of work happens here?"* ([layers.cpp:27](../../../desktop/src/scene3d/layers.cpp#L27)), drawn at [scene.cpp:1389](../../../desktop/src/scene3d/scene.cpp#L1389), HUD at [hud.cpp:914](../../../desktop/src/scene3d/hud.cpp#L914) |
| a crossing mark coloured by `SyscallClass`, legend from `syscall_class_name()` | `SceneLayers::crossings` (57 T2), registered *"where did control leave userspace…"* ([layers.cpp:80](../../../desktop/src/scene3d/layers.cpp#L80)), coloured by `crossing_hue(SyscallClass)` ([causal.cpp:141](../../../desktop/src/scene3d/causal.cpp#L141)), gated at `causal.cpp:644` |

Adding a `motifs` bool would have been a **third** toggle re-tinting cells the `opcode` layer already tints and re-marking crossings the `crossings` layer already marks. Two earlier drafts of this plan proposed exactly that.

**And the "all-true convention" the spec cites does not exist.** `SceneLayers`' actual documented rule ([scene.h:60-144](../../../desktop/src/scene3d/scene.h#L60)) is: a layer that draws an **additional surface or geometry** defaults ON (`canopy`, `mispred`, `crossings`, `taint`, `blame`, `ridge`, `halos`); a layer that **re-lifts the reading of the terrain already on screen** defaults OFF (`confidence`, `opcode`, `data_relief`, `working_set`, `lifetime`, `data_ribbon`, `sediment`). `opcode` is OFF for that stated reason and should stay OFF; `crossings` is ON for that stated reason and should stay ON. The spec's Component 3 paragraph on defaults is wrong on the premise and should be read as superseded by this task.

- [ ] **Step 1: Add the assertions that pin what the spec actually requires**

Add to the existing `desktop/test/test_layers.cpp`, reusing its `check` helper:

```cpp
// 3D-axis-budget T6: the spec's two motif channels are the PRE-EXISTING
// `opcode` and `crossings` layers, not a third toggle. Pin them by id so a
// later brief cannot quietly add a duplicate, and pin the default each takes
// from this struct's real convention (an additional-geometry layer defaults
// ON; a terrain re-lift defaults OFF) — NOT from an "all true" rule, which
// this registry has never followed.
{
    const std::vector<LayerDesc> &all = scene_layers_all();
    const LayerDesc *op = nullptr, *cr = nullptr;
    for (const LayerDesc &d : all) {
        if (std::string(d.id) == "opcode") op = &d;
        if (std::string(d.id) == "crossings") cr = &d;
    }
    check("the opcode-class channel has exactly one row", op != nullptr,
          "the spec's opcode motif channel is the existing `opcode` layer");
    check("the crossing-family channel has exactly one row", cr != nullptr,
          "the spec's I/O motif channel is the existing `crossings` layer");
    if (op != nullptr && cr != nullptr) {
        SceneLayers l;
        check("opcode defaults off — it re-lifts the terrain already on screen",
              !(l.*(op->flag)),
              "a re-lift layer must not open a session in place of the "
              "density view");
        check("crossings defaults on — it adds geometry rather than re-lifting",
              l.*(cr->flag), "an additive layer defaults on by convention");
        // Optional has to mean toggleable, in both directions.
        l.*(op->flag) = true;
        l.*(cr->flag) = false;
        check("both motif channels toggle", l.*(op->flag) && !(l.*(cr->flag)),
              "a toggle did not take");
    }
}
```

- [ ] **Step 2: Run tests**

Run: `make build/desktop_test_layers && ./build/desktop_test_layers`
Expected: PASS immediately — this is a regression gate over correct code. If either default is *not* what this asserts, stop: that is a finding about the tree, not a test to adjust.

- [ ] **Step 3: Verify the D7 abstention already holds**

`crossing_hue(SyscallClass::Other)` ([causal.cpp:161](../../../desktop/src/scene3d/causal.cpp#L161)) and the opcode tint's `OpClass::Unknown` path must be visibly neutral, never a neighbouring family's hue. Read both; if either already abstains, record that in the test comment rather than adding an assertion that restates the switch. If either does **not** abstain, that is a genuine D7 defect — fix it here, with a test.

- [ ] **Step 4: Commit**

```bash
git add desktop/test/test_layers.cpp
git commit -m "desktop(test): pin the two motif channels to their existing layers and defaults"
```

---

### Task 7a: The shared offscreen render harness

**Split from what was one Task 7**, because six helpers its test code called turned out not to exist: `Image`, `render_plane_scene`, `gl_context_available`, `image_blank`, `image_floor_fraction` and `fixture_exists` all return **zero** grep hits under `desktop/`. Only the comparator had been flagged as new. Building the harness is therefore a task in its own right, and it delivers something reviewable on its own: two GL tests sharing one context path instead of two that drift.

**Files:**
- Create: `desktop/test/gl_offscreen.h` — the EGL bring-up, FBO, readback and scene build, **factored out of `test_scene_fbo.cpp`** (`egl_up()` at [`:485`](../../../desktop/test/test_scene_fbo.cpp#L485), `ColorFbo` at `:545`, `capture()` at `:576`, `build_scene()` at `:112`, `upload()` at `:643`)
- Modify: `desktop/test/test_scene_fbo.cpp` — include the factored header instead of its local copies
- Modify: `mk/desktop.mk` — factor the GL test's object closure into a variable so a second GL binary cannot drift from it

**Interfaces:**
- Consumes: `Projection::Layout` and `rebuild_layout()` (Task 2).
- Produces, in `asmdesk::testing`:
  - `struct Image { int w, h; std::vector<uint8_t> px; }` — RGBA8, `glReadPixels` row order
  - `bool gl_context_available(std::string *why)` — idempotent, brings the context up once
  - `struct ColorFbo` with `bool create(int w, int h)`
  - `Image capture_image(scene3d::Scene &, const scene3d::Camera &, const ColorFbo &, const scene3d::SceneLayers &)`
  - `Image render_plane_scene(scene3d::Scene &, const ColorFbo &, const scene3d::Camera &, const Recording &, const scene3d::SceneLayers &, space::Projection::Layout)`
  - `float image_ink_fraction(const Image &)` and `bool image_blank(const Image &)`
  - `bool scene_exists(const char *dir, const char *name)`
- Produces, in `mk/desktop.mk`: `DESKTOP_GL_TEST_OBJS`.

**Qualify the `scene3d` types; the header cannot borrow a `using`.** `Scene`, `Camera` and `SceneLayers` live in **`asmdesk::scene3d`**, not `asmdesk` ([scene.h:41](../../../desktop/src/scene3d/scene.h#L41), [camera.h:23](../../../desktop/src/scene3d/camera.h#L23)); only `Recording` is in `asmdesk` ([recording.h:24](../../../desktop/src/doc/recording.h#L24)) and so resolves unqualified from inside `asmdesk::testing`. `test_scene_fbo.cpp` names them bare only because it carries a file-scope `using namespace asmdesk::scene3d;` at `:67` — a header must not depend on that, and 7b's `using` lines come *after* its `#include` anyway. So every signature below says `scene3d::`, and an earlier draft of this header that dropped the qualifier would not have compiled. Do **not** "fix" it by putting a `using namespace` inside the header.

**`image_floor_fraction` is deliberately NOT produced.** The earlier draft asserted `image_floor_fraction(bare) >= 0.5f` — "the atlas should claim all of it". That is unsound twice over: what fraction of the *viewport* the floor covers is a camera-framing artefact (at the default `radius` 2.2 the plane does not fill the frame, so the threshold measures the camera, not the layout), and per the honesty note in "the one decision", the atlas does not increase the number of cells that carry data anyway. The packing claim is a cell-space property and Task 2 proves it in integers with no epsilon. What the screen can honestly gate is *non-blankness* and *pairwise distinctness*, which is what 7b does.

- [ ] **Step 1: Write `desktop/test/gl_offscreen.h`**

```cpp
// gl_offscreen.h — a surfaceless EGL context, an RGBA8 FBO, and one call that
// turns a Recording into a rendered Plane-scene frame. The EGL/FBO/readback
// bodies are LIFTED UNCHANGED from test_scene_fbo.cpp (egl_up :485, ColorFbo
// :545, capture :576) so that the FBO smoke and the motif gate share one
// context path instead of two that can silently diverge.
//
// Header-only but NOT dependency-free: including this pulls in EGL, GL and the
// whole scene3d/space object closure, so a binary using it must link
// DESKTOP_GL_TEST_OBJS and belongs in DESKTOP_GL_TESTS, never DESKTOP_TESTS.
// (camera.h and trajscale.h are the engine-free citizens; this is not one.)
#ifndef ASMDESK_TEST_GL_OFFSCREEN_H
#define ASMDESK_TEST_GL_OFFSCREEN_H

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include "doc/recording.h"
#include "scene3d/camera.h"
#include "scene3d/scene.h"
#include "space/converge.h"
#include "space/opcode_terrain.h" // build_opcode_terrain, opcode_guest_from_arch
#include "space/projection.h"
#include "space/terrain.h"
#include "space/trajectory.h"

namespace asmdesk::testing {

// A read-back frame. Row 0 is the BOTTOM row — glReadPixels' own order, kept
// rather than flipped because every consumer compares frames to frames.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px; // w*h*4, RGBA8
};

// The clear colour capture_image() uses, as the bytes glReadPixels returns.
// Same value test_scene_fbo.cpp:580 already clears to, so a frame means the
// same thing whichever test produced it — and "blank" has one definition.
inline constexpr uint8_t kClearRGB[3] = {5, 5, 8};

// Fraction of pixels carrying anything other than the clear colour. A
// FRACTION, not a count, so a caller's threshold does not silently depend on
// the framebuffer size. The 12 is a driver-rounding slack, not a perceptual
// threshold — llvmpipe need not land exactly on kClearRGB.
inline float image_ink_fraction(const Image &a) {
    if (a.px.empty())
        return 0.0f;
    const size_t n = size_t(a.w) * size_t(a.h);
    size_t ink = 0;
    for (size_t i = 0; i < n; ++i) {
        const int dr = int(a.px[i * 4 + 0]) - int(kClearRGB[0]);
        const int dg = int(a.px[i * 4 + 1]) - int(kClearRGB[1]);
        const int db = int(a.px[i * 4 + 2]) - int(kClearRGB[2]);
        if (std::abs(dr) + std::abs(dg) + std::abs(db) > 12)
            ink++;
    }
    return float(ink) / float(n);
}

inline bool image_blank(const Image &a) { return image_ink_fraction(a) < 0.001f; }

// Is a committed recording present? A missing one is a broken checkout, never
// a reason to skip; this only reports, and the caller says what it means.
inline bool scene_exists(const char *dir, const char *name) {
    std::ifstream f(std::string(dir) + "/" + name);
    return f.good();
}

// --- the EGL context, brought up at most once per process --------------------
// NOT a verbatim lift, and an earlier draft said it was. The existing helper is
//   static bool egl_up(EGLDisplay *out_dpy, EGLContext *out_ctx, std::string *why)
// (test_scene_fbo.cpp:485) — it HANDS BACK the display and context rather than
// owning them, because its caller keeps them in locals. Here they must outlive
// the call, so keep egl_up's body byte-for-byte and add the two statics around
// it; do not "simplify" by dropping the out-params, since test_scene_fbo.cpp
// still calls it with them after Step 2 re-points that file at this header.
inline bool egl_up(EGLDisplay *out_dpy, EGLContext *out_ctx, std::string *why) {
    /* ...test_scene_fbo.cpp:485's body, unchanged... */
}
inline bool egl_up_once(std::string *why) {
    static EGLDisplay dpy = EGL_NO_DISPLAY;
    static EGLContext ctx = EGL_NO_CONTEXT;
    return egl_up(&dpy, &ctx, why);
}

// Returns false with a reason where no GL device is reachable. Callers decide
// what that MEANS: test_scene_fbo self-skips (its pure half has already run);
// the motif gate FAILS, because its entire content is the picture.
inline bool gl_context_available(std::string *why) {
    static bool tried = false, ok = false;
    static std::string reason;
    if (!tried) {
        tried = true;
        ok = egl_up_once(&reason);
    }
    if (!ok && why)
        *why = reason;
    return ok;
}

// --- the framebuffer and the readback ---------------------------------------
struct ColorFbo { /* ...test_scene_fbo.cpp:545 verbatim... */ };

inline Image capture_image(scene3d::Scene &scene, const scene3d::Camera &cam,
                           const ColorFbo &cf,
                           const scene3d::SceneLayers &layers) {
    glBindFramebuffer(GL_FRAMEBUFFER, cf.fbo);
    glViewport(0, 0, cf.w, cf.h);
    glClearColor(0.02f, 0.02f, 0.03f, 1.0f); // == kClearRGB
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    scene.render(cam, cf.w, cf.h, layers);
    Image img;
    img.w = cf.w;
    img.h = cf.h;
    img.px.resize(size_t(cf.w) * size_t(cf.h) * 4);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, cf.w, cf.h, GL_RGBA, GL_UNSIGNED_BYTE, img.px.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return img;
}

// --- one recording -> one rendered frame ------------------------------------
// The layout is set on the Projection BEFORE build_terrain, and this is the
// whole reason this function exists rather than a caller doing it inline: the
// terrain's cells are keyed on the projection's mapping, so switching layout
// afterwards would paint an atlas floor with Hilbert heights and the picture
// would be a composite of two coordinate systems.
inline Image render_plane_scene(scene3d::Scene &scene, const ColorFbo &cf,
                                const scene3d::Camera &cam,
                                const Recording &rec,
                                const scene3d::SceneLayers &layers,
                                space::Projection::Layout layout) {
    space::Projection proj =
        space::build_projection(space::regions_from_codeimage(rec));
    proj.layout = layout;
    space::rebuild_layout(proj);
    space::TerrainModel terr = space::build_terrain(std::move(proj), rec);
    const space::TrajectorySet traj =
        space::build_trajectories(rec, terr.proj);
    scene.nsteps = static_cast<uint32_t>(terr.nsteps);
    scene.set_terrain(terr.full());
    scene.set_trajectories(traj, terr.proj);
    scene.set_convergences(space::ConvergenceSet{}, terr.proj);
    // THE OPCODE CHANNEL'S DATA. `SceneLayers::opcode` only selects a tint —
    // the byte map it samples is the R8UI tex_opclass_ that set_opcode_terrain
    // uploads ([scene.h:309](../../../desktop/src/scene3d/scene.h#L309)), and
    // WITHOUT this call `opcode = true` re-tints nothing. An earlier draft of
    // this harness omitted it, which would have made 7b's "the motif channel
    // changes the picture" check fail for a reason having nothing to do with
    // the encoding. Slice-invariant, so once per weave is right.
    scene.set_opcode_terrain(
        space::build_opcode_terrain(
            terr, rec, space::opcode_guest_from_arch(rec.arch)),
        terr.w, terr.h);
    return capture_image(scene, cam, cf, layers);
}

} // namespace asmdesk::testing
#endif // ASMDESK_TEST_GL_OFFSCREEN_H
```

- [ ] **Step 2: Re-point `test_scene_fbo.cpp` at the header and confirm it still passes**

Delete its local `egl_up`, `ColorFbo` and `capture`, include `gl_offscreen.h`, and adapt the call sites (`capture()` returned a bare `std::vector<unsigned char>`; `capture_image()` returns an `Image` whose `.px` is that same buffer, so `pixels_differ(a.px, b.px)` and `lum(&a.px[i*4])` are the mechanical edits).

Run: `make docker-desktop`
Expected: PASS, with `test_scene_fbo` reporting exactly what it reported before. **Do this before writing anything new** — refactoring a working GL path is where this task can silently break an existing gate, and 7b is much harder to debug on top of a broken harness.

- [ ] **Step 3: Factor the object closure in `mk/desktop.mk`**

The link rule at [mk/desktop.mk:1800-1821](../../../mk/desktop.mk#L1800) lists twenty-odd objects. Lift them into a variable so 7b's binary cannot drift from it:

```make
# The object closure a GL test links. Factored (3D-axis-budget T7a) so the FBO
# smoke and the motif gate cannot drift apart.
DESKTOP_GL_TEST_OBJS := \
    $(BUILD)/desktop/test/s3/scene.o $(BUILD)/desktop/test/s3/pick.o \
    ...the rest of the existing list, verbatim, minus the test's own .o... \
    $(DESKTOP_TEST_DOC)

$(BUILD)/desktop_test_scene_fbo: $(BUILD)/desktop/test/t/test_scene_fbo.o \
    $(DESKTOP_GL_TEST_OBJS)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ $(EGL_LIBS) $(GL_LIBS) -o $@
```

- [ ] **Step 4: Commit**

```bash
git add desktop/test/gl_offscreen.h desktop/test/test_scene_fbo.cpp mk/desktop.mk
git commit -m "desktop(test): factor the offscreen GL harness out of the FBO smoke"
```

---

### Task 7b: The motif-distinctness acceptance gate

The gate that decides whether the encoding actually builds intuition. A principled encoding that renders every program identically has failed.

**Files:**
- Create: `desktop/test/image_distinct.h` — the comparator
- Create: `desktop/test/test_motif_distinctness.cpp`
- Modify: `mk/desktop.mk` — four additions, listed in Step 3; a test that is not in a `*_TESTS` list never runs in CI
- Modify: `tools/asmtrace_record.c` — three new `SCENE_*`-style byte arrays and three `record_scene_abs()` calls (Step 5 has the mechanism and why it is **not** `ROUTINES[]`), which `make asmtrace-golden` then writes into `tests/golden-asmtrace/` (where `test_scene_fbo` already loads its scenes from — **not** `desktop/test/fixtures/`, which holds the hand-committed inputs)
- **Not created:** any hand-authored `.asmtrace` under `tests/golden-asmtrace/scenes/`. Step 0 rules it out for this gate.

**Interfaces:**
- Consumes: Task 7a's whole harness, and everything above it.
- Produces: `bool images_distinct(const Image &a, const Image &b, float min_fraction = 0.02f);`

- [x] **Step 0: The fixture-honesty question — RESOLVED, do not re-open**

The earlier draft left this open and mis-stated the constraint. Both halves are settled below; copy the decision verbatim into the test file's header comment.

**What the earlier draft got wrong.** It claimed the `simd` and `syscalls` cases could both come from `make asmtrace-golden` and that "**only** the memcpy case is at risk". The tree says otherwise, in both directions:

- **`tools/asmtrace_record.c` emits no `syscall` events at all** — zero occurrences in 2511 lines. It is a Unicorn emulator over code it never enters the kernel from: either a compiled corpus symbol looked up by name (`ROUTINES[]` → `asmtest_corpus_routine`) or a hand-assembled byte array (`SCENE_HOT_LOOP[]` → `record_scene_abs`, which is the path Step 5 uses). `syscall` rows come from [cli/asmspy.c:997](../../../cli/asmspy.c#L997), the live ptrace tracer. The *preferred* path could never have produced the syscalls fixture.
- **The memcpy case does not need a `mem` stream.** Terrain height is driven by the trace canvas (`m.height_source = "trace"`, [terrain.cpp:287](../../../desktop/src/space/terrain.cpp#L287)); `mem` is a separate rung gated on `mem_present`. This gate renders the terrain and the `opcode` tint, neither of which reads `mem`. So `test_scene_fbo.cpp:20-23`'s "`mem` has no producer" does not reach this task at all — and its hand-authored rich-`mem` scene is a *warning*, not a template: that file records the scene as **INERT**, rendering "pixel-identically to its coarse twin".

**The resolution: generate the image fixtures, freeze a real capture for crossings, and scope the image gate to what the frame can honestly carry.**

| | Decision | Why |
|---|---|---|
| the three image fixtures | **generated** — three new byte arrays emitted through `record_scene_abs`, per Step 5 (**not** `ROUTINES[]`, which holds no bytes) | real Capstone-recorded disasm drives `OpClass`, and [asmtrace-golden-check](../../../mk/cli.mk#L534) then holds them byte-stable under a CI gate. The bytes are author-chosen, so this buys honest *classification*, not an honest *program* — say so in the header rather than overclaiming |
| the crossings fixture | **a frozen live `asmspy` capture**, committed under `desktop/test/fixtures/` | the only producer of real `syscall` rows. Recorded ONCE and committed: `desktop/test/fixtures/` has no regeneration or byte-check gate (it appears in `mk/desktop.mk` only as a `-DASMTEST_FIXTURE_DIR` define), so "an `asmspy` run is not byte-reproducible" bears on regenerating it, never on using it. This is the precedent `test_scene_fbo` already set by reusing `obs-survey-ibs.asmtrace` |
| the crossings assertion | **not an image check** — Task 7c pins it in the pure `test_crossing.cpp` | see Task 7c |
| the image gate's layers | **`opcode` only** | `crossings` geometry cannot be uploaded here: `build_crossing_layer` lives in `views/`, and its data is unrelated to the axis budget this plan is gating |

**Never capture live at test time.** A distinctness gate over a nondeterministic input **cannot fail** — run-to-run variation supplies the very difference the gate looks for, so it would pass even if the encoding conveyed nothing about what ran. That is strictly worse than the self-confirming hand-authored fixture this step exists to avoid: self-confirming is a gate someone tuned until it passed; self-satisfying is a gate that cannot not pass. (ASLR specifically would *not* be the culprit — `build_projection` compacts regions, so `project()` depends on region lengths and sort order, never on bases. What varies is step counts and syscall ordering, which is exactly the signal under test.)

**Where a tolerance belongs, and where it does not.** `images_distinct`'s 2 % threshold and `image_ink_fraction`'s slack of 12 exist to absorb **llvmpipe** variation over a **deterministic** input. Stacking input nondeterminism on top does not extend that budget, it consumes it. And a "range" on the picture itself needs a reference range that one capture cannot yield — you would widen bounds until it passed, which is this step's own problem relocated to the threshold. See Step 6's standing prohibition on lowering `min_fraction`.

**A shape check replaces the byte gate for the frozen fixture.** Task 7c asserts the capture still carries ≥1 `codeimage`, a non-empty `trace` and ≥2 `syscall` rows — a has-it-rotted precondition, not an approximation of any result.

- [ ] **Step 1: Write the comparator**

```cpp
// image_distinct.h — pairwise frame distinctness. Deliberately crude: this
// answers "would a reader see two different pictures", not "are these images
// similar", so it counts differing pixels rather than computing a perceptual
// metric. A threshold, not a score, because the gate is a yes/no.
#ifndef ASMDESK_TEST_IMAGE_DISTINCT_H
#define ASMDESK_TEST_IMAGE_DISTINCT_H

#include <cstdlib>

#include "gl_offscreen.h" // Image

namespace asmdesk::testing {

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

} // namespace asmdesk::testing
#endif // ASMDESK_TEST_IMAGE_DISTINCT_H
```

- [ ] **Step 2: Write the failing test**

**The GL half must not self-skip in this binary** — `Dockerfile.desktop` pins software Mesa + EGL so it really renders, and a test that can only self-skip is not a test (CLAUDE.md). Fail loudly if no context is obtainable. Unlike `test_scene_fbo` there is no pure half to fall back on: the picture *is* the subject.

Note the render call must enable the `opcode` channel explicitly, since it defaults OFF by the convention Task 6 pinned — and must **not** enable `crossings`, which has no geometry uploaded here (Step 0; Task 7c pins it instead):

```cpp
// test_motif_distinctness.cpp — the acceptance gate for the axis budget.
//
// STEP 0 DECISION (do not re-open; the plan's Task 7b Step 0 has the argument):
//   All three recordings are GENERATED by `make asmtrace-golden` from
//   tools/asmtrace_record.c's record_scene_abs() byte arrays — a scalar
//   integer loop, a SIMD routine and a store-heavy routine — so the OpClass
//   each cell reports comes
//   from the recording's own Capstone-recorded disasm, not from a fixture
//   author. The byte arrays are still author-chosen: this buys honest
//   CLASSIFICATION, not an honest program, and the claim below is scoped to
//   that. asmtrace-golden-check (mk/cli.mk:534) holds all three byte-stable.
//   Nothing here is hand-authored, so there is no channel this test must
//   refrain from asserting on.
//
//   The crossings channel is NOT gated here: its geometry comes from
//   views/build_crossing_layer, which this binary does not link, and its
//   fixture is a frozen live asmspy capture. Task 7c pins it, in a pure test.
#include <cstdio>
#include <string>

#include "gl_offscreen.h"
#include "image_distinct.h"

#ifndef ASMTEST_GOLDEN_DIR
#error "ASMTEST_GOLDEN_DIR must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;
using namespace asmdesk::scene3d;
using namespace asmdesk::testing;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL: %s (%s)\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

// Loads through the real loader, like test_scene_fbo.cpp:92.
static Recording load_scene(const char *name) {
    std::string err;
    auto rec = load_recording_file(std::string(ASMTEST_GOLDEN_DIR) + "/" + name,
                                   err);
    if (!rec) {
        fail(std::string("load ") + name, err);
        return Recording{};
    }
    return *rec;
}

int main() {
    // Three GENERATED recordings (Step 0). Named for the work they do, not for
    // a stream they carry: `motif-stores` is store-heavy CODE, which is what
    // the opcode channel reads — it needs no `mem` stream, and the earlier
    // draft's "motif-memcpy" name invited exactly that confusion.
    static const char *kScenes[3] = {"motif-scalar-loop.asmtrace",
                                     "motif-simd.asmtrace",
                                     "motif-stores.asmtrace"};

    // A missing recording is a FAILURE, never a skip — the same rule
    // test_scene_fbo.cpp:90 already states for its golden scenes.
    for (const char *f : kScenes)
        check(std::string("scene is present: ") + f,
              scene_exists(ASMTEST_GOLDEN_DIR, f),
              "the acceptance gate cannot run without its inputs");

    std::string why;
    if (!gl_context_available(&why))
        // NOT a skip. The container pins software Mesa + EGL for exactly this.
        fail("a GL context is obtainable",
             why + " — run this under `make docker-desktop`, which pins "
                   "libgl1-mesa-dri + libegl1-mesa-dev with "
                   "LIBGL_ALWAYS_SOFTWARE=1");

    if (failures == 0) {
        Scene scene;
        std::string serr;
        if (!scene.init_gl(&serr))
            fail("the scene shaders build", serr);
        ColorFbo cf;
        if (failures == 0 && !cf.create(160, 160))
            fail("the colour framebuffer is complete",
                 "incomplete FBO on this driver");
        const Camera cam; // the default three-quarter view, as shipped

        // `opcode` defaults OFF (it re-lifts the terrain), so the gate sets it
        // explicitly. `crossings` is deliberately LEFT AT ITS DEFAULT and never
        // asserted on: render_plane_scene uploads no crossing geometry, so
        // toggling it here would test nothing while claiming a second channel.
        SceneLayers on;
        on.opcode = true;
        const auto kAtlas = space::Projection::Layout::Atlas;

        if (failures == 0) {
            // Three recordings whose behaviour we already know must be pairwise
            // DISTINCT. Non-blankness is not enough: an encoding that renders
            // every program alike has failed however principled it is.
            Image shots[3];
            for (int i = 0; i < 3; i++) {
                const Recording rec = load_scene(kScenes[i]);
                shots[i] =
                    render_plane_scene(scene, cf, cam, rec, on, kAtlas);
                check(std::string("the scene renders at all: ") + kScenes[i],
                      !image_blank(shots[i]),
                      "the frame came back at the clear colour");
            }
            for (int i = 0; i < 3; i++)
                for (int j = i + 1; j < 3; j++)
                    check(std::string("distinct: ") + kScenes[i] + " vs " +
                              kScenes[j],
                          images_distinct(shots[i], shots[j]),
                          "two programs with different behaviour rendered "
                          "alike — the encoding conveys nothing about what ran");

            // "Optional" has to mean the scene stands without the layer. Two
            // claims, and BOTH matter: the frame is still drawn, and it is
            // still a DIFFERENT frame, which is what makes the channel a
            // channel rather than decoration.
            SceneLayers off; // opcode already defaults false
            const Recording rec = load_scene(kScenes[0]);
            const Image bare =
                render_plane_scene(scene, cf, cam, rec, off, kAtlas);
            check("the scene still renders with the opcode channel off",
                  !image_blank(bare),
                  "turning the optional layer off emptied the viewport, at "
                  "ink fraction " + std::to_string(image_ink_fraction(bare)));
            check("the opcode channel actually changes the picture",
                  images_distinct(shots[0], bare),
                  "switching the channel on changed nothing — it is drawing "
                  "no information (check set_opcode_terrain ran: without it "
                  "the tint samples an empty byte map)");
        }
    }

    if (failures) {
        std::fprintf(stderr, "%d motif check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_motif_distinctness: all checks passed\n");
    return 0;
}
```

**Note what is deliberately absent.** There is no "the floor is ≥ 50 % covered" assertion. An earlier draft had one; see Task 7a's Interfaces block for why it was unsound in both of its halves. The packing claim is proved in cells, in Task 2, with no epsilon and no camera in the way.

- [ ] **Step 3: Register the binary in `mk/desktop.mk` — four additions, not one**

An earlier draft named only the `DESKTOP_GL_TESTS` entry. The object also needs the golden-directory define (target-specific, exactly like [mk/desktop.mk:505-507](../../../mk/desktop.mk#L505)) or the `#error` above fires, and it includes `linmath.h` transitively through `camera.h`, so it needs the order-only prereq too.

```make
# 1. the golden dir, target-specific like every other test .o that reads one
$(BUILD)/desktop/test/t/test_motif_distinctness.o: \
    DESKTOP_TEST_EXTRA = -DASMTEST_GOLDEN_DIR='"tests/golden-asmtrace"'

# 2. linmath, alongside test_camera.o/test_scene_fbo.o at :1684
$(BUILD)/desktop/test/t/test_motif_distinctness.o: | $(LINMATH_HOME)/linmath.h

# 3. the link rule, over 7a's factored closure
$(BUILD)/desktop_test_motif_distinctness: \
    $(BUILD)/desktop/test/t/test_motif_distinctness.o $(DESKTOP_GL_TEST_OBJS)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ $(EGL_LIBS) $(GL_LIBS) -o $@

# 4. the GL list at :2310 — NOT DESKTOP_TESTS, which is the no-GL lane
DESKTOP_GL_TESTS := $(BUILD)/desktop_test_scene_fbo \
                    $(BUILD)/desktop_test_motif_distinctness
```

- [ ] **Step 4: Run tests to verify they fail**

Run: `make build/desktop_test_motif_distinctness && ./build/desktop_test_motif_distinctness`
Expected: FAIL — recordings absent.

- [ ] **Step 5: Generate the three recordings**

**Not `ROUTINES[]` — that table cannot carry bytes.** An earlier draft said "add three `ROUTINES[]` entries … following the byte-literal pattern the file already uses for `SCENE_HOT_LOOP`", and those two halves are mutually exclusive. `ROUTINES[]` is `rec_routine_t` = `{name, args[3], nargs, steps_cap}` ([:219](../../../tools/asmtrace_record.c#L219)) — **no bytes field** — and `record_one` resolves each entry through `asmtest_corpus_routine(r->name)` ([:1416](../../../tools/asmtrace_record.c#L1416)), a lookup of a **compiled symbol's address** in the linked corpus objects (`$(ASMTRACE_ROUTINE_OBJS)` = `fp.o simd.o structs.o fault.o corpus_routines.o`, [mk/cli.mk:505](../../../mk/cli.mk#L505)). A new entry would therefore need new corpus assembly, not a byte array; and `record_one` names its output after the routine, so it could not produce a `motif-*.asmtrace` either. Following the draft literally, `make asmtrace-golden` fails with `asmtrace_record: no corpus routine 'motif-scalar-loop'`.

**The byte-literal path is `record_scene_abs`**, which is what `SCENE_HOT_LOOP` actually feeds:

```c
static int record_scene_abs(const char *dir, const char *out, const char *label,
                            const uint8_t *code, size_t code_len,
                            const long *args, int nargs, size_t trace_cap);
```

It takes an **explicit output name** (`"%s/%s.asmtrace"`) and a **raw byte array**, and it emits exactly the two kinds this gate consumes — `codeimage` and absolute-basis `trace` — *including the per-instruction Capstone disasm text*, which is the field `build_opcode_terrain` reads ([opcode_terrain.cpp:59-63](../../../desktop/src/space/opcode_terrain.cpp#L59), `s.trace.disasm`). That last point is why this path is not merely the convenient one but the correct one: a producer that emitted no disasm text would classify every cell `Unknown` and the distinctness gate would fail for a reason having nothing to do with the encoding.

So: add three `static const uint8_t` arrays beside [`SCENE_HOT_LOOP` (:311)](../../../tools/asmtrace_record.c#L311), each with the instruction listing in the comment above the bytes as that file requires, and three calls in the scene block at [:2374](../../../tools/asmtrace_record.c#L2374), following its shape exactly:

```c
/* 3D-axis-budget T7b: three scenes whose DOMINANT OpClass differs, so the
 * opcode motif channel has something to distinguish. Same emitter as the
 * scene-abs goldens above — codeimage + absolute trace, with the recorded
 * disasm text build_opcode_terrain classifies from. */
{
    static const long motif_args[1] = {3};
    if (record_scene_abs(dir, "motif-scalar-loop", "motif_scalar_loop",
                         MOTIF_SCALAR_LOOP, sizeof MOTIF_SCALAR_LOOP,
                         motif_args, 1, 0) != 0)
        failed++;
    if (record_scene_abs(dir, "motif-simd", "motif_simd", MOTIF_SIMD,
                         sizeof MOTIF_SIMD, motif_args, 1, 0) != 0)
        failed++;
    if (record_scene_abs(dir, "motif-stores", "motif_stores", MOTIF_STORES,
                         sizeof MOTIF_STORES, motif_args, 1, 0) != 0)
        failed++;
}
```

The three byte arrays must differ in the quantity the gate reads, which is **dominant `OpClass` per cell**, so pick instruction mixes that classify apart under [space/mnemonic.h](../../../desktop/src/space/mnemonic.h):

| recording | mix | dominant class |
|---|---|---|
| `motif-scalar-loop` | `add`/`cmp`/`jne` over a counter | `IntArith` / `CompareBranch` |
| `motif-simd` | packed XMM ops | `VectorSIMD` |
| `motif-stores` | a `mov`-to-memory run | `Move` |

Keep each small enough to hold the test under a second. Three constraints the emitter imposes, all of which fail loudly rather than silently:

- **The bytes must run to a `ret` under Unicorn**, because `record_scene_abs` drives the same emulator L0 producer the other scene goldens use; a routine that faults produces `emulator producer failed for <out>` and a non-zero exit, not a truncated file.
- **The window is bounded** — the emitter refuses with `scene %s exceeds the %d-byte window` past `REC_WINDOW`.
- **`motif-simd` needs real packed XMM instructions**, not merely SIMD-looking ones, since `OpClass` is decided by the recorded mnemonic. Verify the classification landed rather than assuming it: after regenerating, `grep -o '"[a-z]*"' tests/golden-asmtrace/motif-simd.asmtrace | sort -u | head` should show the packed mnemonics you wrote. If all three scenes classify the same way, the gate in Step 6 will fail and it will be *this* that is wrong, not the encoding.

Then:

```
make docker-cli && make asmtrace-golden
```

`asmtrace-golden` is gated on x86_64 + libunicorn (`ASMTRACE_GOLDEN_OK`, [mk/cli.mk:521](../../../mk/cli.mk#L521)), so regenerate from the `docker-cli` image — the target's own echo warns that host Capstone 4.x renders different disasm text, and this gate reads that text. Commit the three `.asmtrace` files under `tests/golden-asmtrace/`. `asmtrace-golden-check` ([mk/cli.mk:534](../../../mk/cli.mk#L534)) then holds them byte-stable in CI, which is the point; if a later disasm or link-set change churns them, regenerate from the container rather than editing them.

- [ ] **Step 6: Run tests to verify they pass — in the container, not on the host**

Run: `make docker-desktop`
Expected: PASS. The host may have no EGL device, in which case the GL lane is not even built (`DESKTOP_GL_MISSING`, [mk/desktop.mk:2311](../../../mk/desktop.mk#L2311)) and a host-only run would report a false green. `Dockerfile.desktop` pins software Mesa + EGL precisely so this lane really renders — so **this gate must be judged from the container run**.

**If the distinctness case fails, stop and report.** That is the design being refuted, not a test to loosen. Do not lower `min_fraction` to make it pass.

- [ ] **Step 7: Commit**

```bash
git add desktop/test/test_motif_distinctness.cpp desktop/test/image_distinct.h tools/asmtrace_record.c tests/golden-asmtrace mk/desktop.mk
git commit -m "desktop(test): gate the opcode motif encoding on pairwise distinctness"
```

---

### Task 7c: The crossings channel, pinned against a real capture

Step 0's second half. The spec's I/O motif channel is the existing `crossings` layer (Task 6), and every assertion on it today runs off **hand-written NDJSON string literals** in `test_crossing.cpp` (`mk_rec(ndjson)` at `:30`, headers at `:42-45`). Those are fine for the anchoring edge cases they were written for — a syscall before the first instruction, a `seq_present == false` self-gate — but nothing in the tree has ever built a crossing layer from syscalls a real kernel actually serviced. This task closes that, and it is where Step 0's frozen `asmspy` capture lands.

**Deliberately not a GL test.** [views/crossing.h](../../../desktop/src/views/crossing.h) is engine-free by design — its own header says the geometry POD lives in `space/` "so scene3d/ can consume it without ever depending on views/". So the honest place to pin this channel is the pure test that already owns the contract, not a rendered frame that would drag `views/` into the GL closure to assert a colour.

**Files:**
- Create: `desktop/test/fixtures/motif-crossings.asmtrace` — one frozen `asmspy` capture (see Step 1)
- Modify: `desktop/test/test_crossing.cpp` — a real-capture block
- Modify: `mk/desktop.mk` — `test_crossing.o` needs the `ASMTEST_FIXTURE_DIR` define it does not currently have

**Interfaces:**
- Consumes: nothing from earlier tasks — `build_crossing_layer` and `SyscallClass` both already ship.
- Produces: no signature change. A regression gate over real data.

- [ ] **Step 1: Record and freeze the capture**

Run `asmspy` over a small binary that makes a handful of distinguishable syscalls (a `write` and an `openat`/`close` pair give two different `SyscallClass` values, which is what makes the class channel checkable rather than a single-colour smear). It must carry a `codeimage` and a non-empty `trace` as well as the `syscall` rows — `desktop/test/fixtures/obs-syscalls.asmtrace` is `{session×2, syscall×4, end}` with neither, which is why it cannot be reused here.

Commit the result under `desktop/test/fixtures/`. It is **frozen, never regenerated**: that directory has no byte-check gate (it appears in `mk/desktop.mk` only as a `-DASMTEST_FIXTURE_DIR` define), so a live capture's non-reproducibility never comes up. Record in the test's comment the exact command and binary that produced it.

- [ ] **Step 2: Write the test**

Add to `desktop/test/test_crossing.cpp`, reusing its helpers. Two blocks — the shape precondition, then the contract:

```cpp
// 3D-axis-budget T7c: the crossing channel over a REAL capture. Every other
// block in this file feeds build_crossing_layer hand-written NDJSON; this one
// feeds it syscalls a kernel actually serviced, which is the only way the
// class channel is tested against something no test author chose.
//
// PROVENANCE: <the exact asmspy command and target binary>. Frozen — this
// directory is never regenerated, so the recording is deterministic at test
// time even though the capture that produced it was not.
{
    std::string err;
    // load_recording_file returns std::optional<Recording> (recording.h:134),
    // NOT a pointer — `rec != nullptr` does not compile against an optional.
    // Test it as a bool, exactly as test_scene_fbo.cpp:92 does.
    std::optional<Recording> rec = load_recording_file(
        std::string(ASMTEST_FIXTURE_DIR) + "/motif-crossings.asmtrace", err);
    check("the real-capture fixture loads", rec.has_value(), err);
    if (rec.has_value()) {
        // The shape check that stands in for the byte-stability gate the
        // GENERATED corpus gets. This is a has-it-rotted precondition, not an
        // approximation of any result — if it trips, the fixture is wrong, and
        // every assertion below would otherwise fail for a misleading reason.
        check("the capture still carries a codeimage",
              rec->by_kind.count("codeimage") != 0,
              "no codeimage: there is no plane to anchor a spur onto");
        check("the capture still carries a trace",
              rec->by_kind.count("trace") != 0 &&
                  !rec->by_kind.at("trace").empty(),
              "no trace worldline: build_crossing_layer self-gates and the "
              "assertions below would pass vacuously");
        check("the capture still carries at least two syscalls",
              rec->by_kind.count("syscall") != 0 &&
                  rec->by_kind.at("syscall").size() >= 2,
              "fewer than two syscall rows: the class channel cannot be shown "
              "to distinguish anything");

        const Projection p = build_projection(regions_from_codeimage(*rec));
        const SyscallView sv = obs_syscalls_build(*rec);
        const CrossingLayer layer = build_crossing_layer(sv, *rec, p);

        // The channel carries information: spurs exist, and they are placed by
        // ADDRESS onto the plane rather than defaulted to a corner.
        check("a real capture produces crossing spurs", !layer.spurs.empty(),
              "no spur from a capture carrying " +
                  std::to_string(rec->by_kind.at("syscall").size()) +
                  " syscalls and a trace");
        // D7: an unclassified syscall abstains as Other. That is CORRECT and
        // must not be asserted away — what would be wrong is EVERY spur landing
        // on Other, which would mean the class channel conveys nothing.
        bool any_classified = false;
        for (const CrossingSpur &s : layer.spurs)
            if (s.cls != SyscallClass::Other)
                any_classified = true;
        check("the class channel distinguishes at least one real syscall",
              any_classified,
              "every spur classified as Other — the channel is a single "
              "colour and names nothing about what the program did");
    }
}
```

- [ ] **Step 3: Register the fixture define**

`test_crossing.o` has no `ASMTEST_FIXTURE_DIR` today. Add it beside the others at [mk/desktop.mk:513-516](../../../mk/desktop.mk#L513):

```make
$(BUILD)/desktop/test/t/test_crossing.o: DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"'
```

The link rule ([mk/desktop.mk:1397](../../../mk/desktop.mk#L1397)) and the `DESKTOP_TESTS` entry (`:1277`) already exist, and the loader is already linked: that rule ends in `$(DESKTOP_TEST_DOC)`, which is `doc/recording.o` and four siblings ([mk/desktop.mk:721](../../../mk/desktop.mk#L721)). So the define is the only makefile change this task needs. The test file also gains `#include <optional>` for the declaration above.

- [ ] **Step 4: Run**

Run: `make build/desktop_test_crossing && ./build/desktop_test_crossing`
Expected: PASS, including every pre-existing NDJSON block — run the whole file.

**If "the class channel distinguishes at least one real syscall" fails, do not relax it.** Either the capture caught only syscalls the classifier ([`class_of`, views/crossing.cpp:45](../../../desktop/src/views/crossing.cpp#L45)) has no word for — recapture with a `write` and an `openat` — or it abstains where it should not, which is a real D7 finding to report.

- [ ] **Step 5: Commit**

```bash
git add desktop/test/fixtures/motif-crossings.asmtrace desktop/test/test_crossing.cpp mk/desktop.mk
git commit -m "desktop(test): pin the crossing class channel against a real capture"
```

---

### Task 8: Label the rectangles in place — Component 1's actual deliverable

**This task exists because a readiness review found it missing.** Component 1's deliverable bullet is *"an address atlas … region-major, 100 % packed, **labelled in place**"*, and the spec is explicit that this — not locality — is the payoff: *"region boundaries become visible rectangles, so the floor can be **labelled in place** from `Region::label`. A Hilbert region is a blobby snake with nowhere to put a label. **This — not locality — is the real win.**"* Tasks 2, 3 and 9 build the rectangles, prove the funnel and flip the default; none of them draws a label. Without this task the plan ships the substrate and drops the reason given for wanting it.

What exists today is a **side-panel legend** — [hud.cpp:1176-1187](../../../desktop/src/scene3d/hud.cpp#L1176) lists every region with a kind swatch. That stays: it is the complete list, and it is what keeps this task honest, because the in-place labels are necessarily *partial* (see the threshold below).

**Files:**
- Modify: `desktop/src/space/projection.h` — declare `AtlasLabel` + `atlas_labels()`
- Modify: `desktop/src/space/projection.cpp` — implement, beside `region_cells()`
- Modify: `desktop/src/scene3d/hud.h`, `desktop/src/scene3d/hud.cpp` — `draw_atlas_labels()`, a sibling of `draw_trajectory_ruler`
- Modify: `desktop/src/ui/shell.cpp` — call it beside the ruler call
- Test: `desktop/test/test_projection.cpp` (already registered — no `mk/desktop.mk` change)

**Interfaces:**
- Consumes: `Projection::rects` and `Projection::layout` (Task 2); `region_style()` ([projection.h:108](../../../desktop/src/space/projection.h#L108)) for the fallback name.
- Produces:
  - `struct space::AtlasLabel { float u, v; uint32_t cells; size_t region; std::string text; }`
  - `std::vector<space::AtlasLabel> space::atlas_labels(const Projection &proj, uint32_t min_cells = 64);`
  - `void scene3d::draw_atlas_labels(ImDrawList *, const Camera &, ImVec2 origin, ImVec2 size, const space::Projection &);`

**Why the placement rule is pure `space/` code and not a lambda in the draw call.** Which rects get a label, where its anchor sits, and what it says are all decidable from the `Projection` alone — no GL, no ImGui, no camera. Putting them in `space/` puts them in the null harness, on the same terms as `region_cells()` next door, so the rule is checkable with no context at all. `hud.cpp` is then only the world→screen projection it already does for the ruler. This is the same split `trajectory_axis_ticks()` ([hud.h:148](../../../desktop/src/scene3d/hud.h#L148)) already uses — a pure tick-selection function tested in `test_shell.cpp:976-992`, with the drawing untested — and it is why that ruler's *rule* has a test at all.

**Three rules, each of which is a judgement the spec constrains:**

1. **Under `Hilbert`, return nothing.** Not a degraded label, not a centroid of a snake — nothing. The spec's own words are that a Hilbert region has "nowhere to put a label"; fabricating an anchor on a space-filling curve would be exactly the invented structure D7 forbids. This is also what makes the function safe to call unconditionally from `shell.cpp`.
2. **Skip rects too small to carry text.** Below the threshold the labels overlap into an unreadable pile, which is strictly worse than the legend that is already on screen. `min_cells = 64` (an 8×8 rect) is the default, and it is a *legibility* threshold, not a fidelity one — say so, because a reader who sees three of nine regions labelled must not conclude the other six are unnamed. The side-panel legend remains complete, which is the disclosure.
3. **Empty `Region::label` falls back to the kind name**, `region_style(kind).name` — reusing [hud.cpp:1185](../../../desktop/src/scene3d/hud.cpp#L1185)'s existing `r.label.empty() ? st.name : r.label` rule rather than inventing a second naming convention.

- [ ] **Step 1: Write the failing test**

Add to `desktop/test/test_projection.cpp`, after Task 2's atlas blocks, reusing `atlas_of()`:

```cpp
// --- Component 1's deliverable: the floor names its own rectangles ----------
{
    // Task 2's first fixture, so the rects are the ones already pinned above:
    // code owns (0,0)-(16,256) = 4096 cells, mmap owns (16,0)-(256,256).
    static const Ref kSmall = {0x0000000000400000ull, 4096, Region::Code};
    static const Ref kBig = {0x0000000001000000ull, 61440, Region::Mmap};
    const Projection p = atlas_of({kSmall, kBig});
    const std::vector<AtlasLabel> labels = atlas_labels(p);
    check("every rect big enough to read gets a label", labels.size() == 2,
          "got " + std::to_string(labels.size()) + " labels for 2 regions");

    const uint32_t n = 1u << p.order;
    for (const AtlasLabel &l : labels) {
        // The anchor is the rect's GEOMETRIC centre, so the label sits on the
        // thing it names rather than beside it.
        const AtlasRect &r = p.rects[l.region];
        const uint32_t cx = uint32_t(l.u * n), cy = uint32_t(l.v * n);
        check("the label anchor lies inside the rect it names",
              cx >= r.x0 && cx < r.x1 && cy >= r.y0 && cy < r.y1,
              "anchor (" + std::to_string(l.u) + "," + std::to_string(l.v) +
                  ") fell outside region " + std::to_string(l.region) +
                  "'s rect");
        // And it points at that region under the layout's own inverse — the
        // label is not merely near the rect, it decodes to it.
        uint64_t back = 0;
        const Region *got = nullptr;
        check("the anchor unprojects to the region it names",
              p.unproject(l.u, l.v, &back, &got) && got != nullptr &&
                  got->base == p.regions[l.region].base,
              "a label anchored on a cell belonging to a different region");
        check("a label always says something", !l.text.empty(),
              "an unnamed rectangle is an unlabelled floor");
    }
    // The fallback: these fixture Regions carry no `label`, so each must fall
    // back to its KIND name rather than to an empty string. Reuses the rule
    // hud.cpp:1185 already applies in the side-panel legend. Guarded on the
    // size, because indexing a short vector to report a failure would turn a
    // clean FAIL into a crash that says nothing.
    if (labels.size() == 2)
        check("an unlabelled region falls back to its kind name",
              labels[0].text == std::string(region_style(Region::Code).name) &&
                  labels[1].text == std::string(region_style(Region::Mmap).name),
              "got \"" + labels[0].text + "\" and \"" + labels[1].text + "\"");
}
{
    // Hilbert has nowhere to put a label and must say so by refusing, not by
    // anchoring on a snake's centroid — that would be fabricated structure.
    std::vector<Region> in;
    Region reg;
    reg.base = 0x400000ull;
    reg.len = 4096;
    reg.kind = Region::Code;
    in.push_back(reg);
    const Projection h = build_projection(std::move(in));
    check("the Hilbert layout labels nothing", atlas_labels(h).empty(),
          "a space-filling curve was given a label anchor it cannot support");
}
{
    // The legibility threshold, on the saturated plane Task 2 already builds:
    // 4000 regions on a 4096-cell grid, so the LARGEST rect is 2 cells. Every
    // one is dropped — 4000 strings over 4096 cells is not a labelled floor.
    // The side-panel legend still lists all 4000, which is the disclosure.
    std::vector<Ref> many;
    for (uint64_t i = 0; i < 4000; i++)
        many.push_back({0x1000ull + i * 0x10000ull, 1, Region::Code});
    const Projection sat = atlas_of(many);
    check("rects too small to read are dropped, not piled up",
          atlas_labels(sat).empty(),
          "a 1-cell rectangle was given a text label");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/desktop_test_projection && ./build/desktop_test_projection`
Expected: FAIL — `atlas_labels` not declared.

- [ ] **Step 3: Implement the placement rule**

In `projection.h`, above `region_cells`'s declaration:

```cpp
// One region's in-place label on the atlas floor. `u`/`v` are the rect's
// GEOMETRIC centre (not a cell centre — the anchor names a rectangle, not a
// cell), `cells` is its area for a caller's own LOD, `region` indexes
// Projection::regions, and `text` is Region::label or, when that is empty,
// the kind name — the SAME fallback the HUD's side-panel legend already
// applies (scene3d/hud.cpp:1185), never a second naming convention.
struct AtlasLabel {
    float u = 0, v = 0;
    uint32_t cells = 0;
    size_t region = 0;
    std::string text;
};

// The labels for an ATLAS projection's rectangles. Empty under Hilbert, and
// that refusal is the honest answer rather than a missing feature: a Hilbert
// region is a connected snake through the plane with no rectangle and so no
// anchor, and inventing one would be fabricated structure (D7).
//
// PARTIAL BY DESIGN. A rect smaller than `min_cells` is skipped, because below
// roughly 8x8 the labels overlap into a pile that reads worse than the empty
// floor did. That is a LEGIBILITY threshold, not a fidelity one — every
// region, labelled or not, is still listed in the HUD's region legend, which
// is what keeps a partially-labelled floor from implying the unlabelled rects
// are unnamed. Callers must not present these as the complete set.
std::vector<AtlasLabel> atlas_labels(const Projection &proj,
                                     uint32_t min_cells = 64);
```

In `projection.cpp`, beside `region_cells`:

```cpp
std::vector<AtlasLabel> atlas_labels(const Projection &proj,
                                     uint32_t min_cells) {
    std::vector<AtlasLabel> out;
    // rects is empty under Hilbert AND after a rebuild_layout fallback; the
    // size equality covers a half-built projection a caller assembled by hand.
    if (proj.layout != Projection::Layout::Atlas ||
        proj.rects.size() != proj.regions.size())
        return out;
    const uint32_t n = uint32_t(1) << proj.order;
    out.reserve(proj.rects.size());
    for (size_t i = 0; i < proj.rects.size(); i++) {
        const AtlasRect &r = proj.rects[i];
        const uint64_t cells =
            uint64_t(r.x1 - r.x0) * uint64_t(r.y1 - r.y0);
        if (cells < min_cells)
            continue; // too small to read — see the header's note
        AtlasLabel l;
        l.u = 0.5f * static_cast<float>(r.x0 + r.x1) / static_cast<float>(n);
        l.v = 0.5f * static_cast<float>(r.y0 + r.y1) / static_cast<float>(n);
        l.cells = static_cast<uint32_t>(
            cells > UINT32_MAX ? UINT32_MAX : cells);
        l.region = i;
        const Region &reg = proj.regions[i];
        l.text = reg.label.empty() ? region_style(reg.kind).name : reg.label;
        out.push_back(std::move(l));
    }
    return out;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make build/desktop_test_projection && ./build/desktop_test_projection`
Expected: PASS. On the first fixture the two anchors are `(0.031250, 0.500000)` and `(0.531250, 0.500000)`; on `kRefs` all three regions label and all three anchors unproject home. On the saturated plane the largest rect is **2 cells**, so the list is empty.

- [ ] **Step 5: Draw them**

In `hud.h`, declare beside `draw_trajectory_ruler` ([:178](../../../desktop/src/scene3d/hud.h#L178)):

```cpp
// Draw the atlas's region labels into the 3D VIEWPORT (not the HUD window),
// each at its rectangle's centre on the ground plane, projected through `cam`
// into `origin`/`size`'s screen rect — the same world->clip transform
// draw_trajectory_ruler uses. A no-op for a Hilbert projection, because
// space::atlas_labels refuses one (see projection.h). Partial by design: the
// HUD's region legend remains the complete list.
void draw_atlas_labels(ImDrawList *draw_list, const Camera &cam, ImVec2 origin,
                       ImVec2 size, const space::Projection &proj);
```

In `hud.cpp`, beside `draw_trajectory_ruler` ([:383](../../../desktop/src/scene3d/hud.cpp#L383)), reusing its transform verbatim — one `cam.mvp` + `mat4x4_mul_vec4`, the behind-the-camera `clip[3] <= 0.0f` skip, and the same NDC→screen mapping. The world point is `{l.u, 0.0f, l.v}` (the ground plane) rather than the ruler's `{0, t*scale, 0}`. Colour each label with `region_style(kind)`'s swatch so the floor label and the legend row agree by construction, and centre the text on the anchor with `ImGui::CalcTextSize(...).x * 0.5f`.

- [ ] **Step 6: Call it**

In `shell.cpp`, immediately after the `draw_trajectory_ruler` call at [:1828](../../../desktop/src/ui/shell.cpp#L1828), under the same `SceneKind::Plane` gate:

```cpp
if (sv.kind == scene3d::SceneKind::Plane)
    scene3d::draw_atlas_labels(
        ImGui::GetWindowDrawList(), sv.cam, vp_origin,
        ImVec2(static_cast<float>(fbw), static_cast<float>(fbh)),
        sv.terr.proj);
```

No layout gate is needed here — `atlas_labels` returns empty under Hilbert, which is exactly why the refusal lives in the pure function rather than at the call site.

- [ ] **Step 7: Run the suite**

Run: `make desktop-test`, then `make docker-desktop`.
Expected: PASS. Nothing renders differently yet — `Projection::layout` still defaults to `Hilbert` until Task 10 — so any golden movement here is a real defect, not churn. That ordering is deliberate: it lets this task's drawing land while the picture is still frozen, so Task 10's regeneration has exactly one cause.

- [ ] **Step 8: Commit**

```bash
git add desktop/src/space/projection.h desktop/src/space/projection.cpp desktop/src/scene3d/hud.h desktop/src/scene3d/hud.cpp desktop/src/ui/shell.cpp desktop/test/test_projection.cpp
git commit -m "space: label the atlas rectangles in place, from the regions themselves"
```

---

### Task 9: Say when the floor reflows under a growing capture

The spec's risk table asks for two things — *"layout is keyed on the region set; recompute only when that set changes, and record the reflow in the HUD"* — and names the failure mode outright: **"a growing capture that reflows silently is the failure mode to avoid."**

**The first half is already true and needs no code.** `rebuild_layout()` runs inside `build_projection()`, which the shell calls only from the lazy weave at [shell.cpp:1176](../../../desktop/src/ui/shell.cpp#L1176) (`if (!sv.built)`), so the layout is recomputed once per weave, never per frame. The second half is the work here.

**Where the reflow actually happens, and why the note cannot live in `space/` alone.** A live batch does not set `sv.built = false`; [shell.cpp:279-285](../../../desktop/src/ui/shell.cpp#L279) **replaces the whole `SceneView`** — `s.scenes[i] = SceneView{}` — preserving only the camera, the HUD state and the primer, with a comment saying why ("else a growing capture would snap the user's 3D view back to the default orbit on every event batch"). The previous projection is destroyed at that line. So detecting a reflow means carrying a digest of the old layout through that reset, exactly the way the camera is already carried. That is the whole shape of this task: a pure digest + comparison in `space/`, and one more field on the preserve-list.

**Not Hilbert-specific, and not atlas-specific.** Adding a region reflows *both* layouts — compaction shifts every later region's domain offset, so a Hilbert floor re-scrambles just as surely as a treemap re-tiles. The digest therefore covers what determines the mapping under either: `order`, `domain_off` and `rects`. This is why the task stands on its own rather than being folded into Task 2, and why it is worth landing before the flip.

**Files:**
- Modify: `desktop/src/space/types.h` — `layout_note` on `Projection`, beside `data_span_note`
- Modify: `desktop/src/space/projection.h`, `desktop/src/space/projection.cpp` — `LayoutFingerprint`, `layout_fingerprint()`, `layout_reflow_note()`
- Modify: `desktop/src/ui/shell.h` — `layout_fp` on `SceneView`
- Modify: `desktop/src/ui/shell.cpp` — preserve it across the growth reset (`:279-285`), set the note in the weave (`:1176-1188`)
- Modify: `desktop/src/scene3d/hud.cpp:532` — surface it beside `data_span_note`
- Test: `desktop/test/test_projection.cpp` (the rule) and `desktop/test/test_shell.cpp` (the wiring)

**Interfaces:**
- Consumes: `Projection::rects` (Task 2) — but degrades to `order` + `domain_off` under Hilbert, so it does not require the atlas.
- Produces:
  - `struct space::LayoutFingerprint { bool valid; size_t regions; uint64_t digest; }`
  - `space::LayoutFingerprint space::layout_fingerprint(const Projection &);`
  - `std::string space::layout_reflow_note(const LayoutFingerprint &prev, const LayoutFingerprint &now);`
  - `std::string Projection::layout_note` — empty except on a weave that moved the floor.

**What the note must not do.** It reports that the floor was re-laid out; it does not claim *how much* moved, and it must never fire on a first weave. A reader who has no mental map yet cannot have it reset, so `prev.valid == false` returns no note — and a recording with no regions has no floor at all. Both are silence by rule, not by threshold.

- [ ] **Step 1: Write the failing test for the rule**

Add to `desktop/test/test_projection.cpp`, after Task 8's label blocks:

```cpp
// --- the reflow notice: a growing capture must not re-lay the floor silently -
{
    // Rebuilding the SAME region set is not a reflow. build_projection is a
    // pure function of the regions, so a weave that changes nothing produces
    // an identical layout — the spec's "recompute only when that set changes"
    // falls out of that, and this pins it rather than assuming it.
    const Projection a = atlas_of({kRefs[0], kRefs[1], kRefs[2]});
    const Projection b = atlas_of({kRefs[0], kRefs[1], kRefs[2]});
    const LayoutFingerprint fa = layout_fingerprint(a);
    const LayoutFingerprint fb = layout_fingerprint(b);
    check("an unchanged region set digests identically", fa.digest == fb.digest,
          "two builds of one region set disagreed");
    check("recomputing an unchanged layout is not a reflow",
          layout_reflow_note(fa, fb).empty(),
          "warned about a floor that did not move: \"" +
              layout_reflow_note(fa, fb) + "\"");
}
{
    // A region appears — the live-capture case the spec is about.
    static const Ref kOne = {0x0000000000400000ull, 4096, Region::Code};
    static const Ref kTwo = {0x0000000000900000ull, 4096, Region::Heap};
    const std::string note =
        layout_reflow_note(layout_fingerprint(atlas_of({kOne})),
                           layout_fingerprint(atlas_of({kOne, kTwo})));
    check("a new region reflows the floor, and says so", !note.empty(),
          "a growing capture re-laid its floor silently — the exact failure "
          "the spec's risk table names");
    check("the note names what changed",
          note.find("1 region became 2") != std::string::npos, note);
}
{
    // Same region COUNT, one of them grew. Under Hilbert this shifts every
    // later region's domain offset, so the floor re-scrambles just as surely
    // as a treemap re-tiles — the notice is not an atlas feature.
    std::vector<Region> small, grown;
    Region r0;
    r0.base = 0x400000ull;
    r0.len = 4096;
    r0.kind = Region::Code;
    Region r1 = r0;
    r1.base = 0x900000ull;
    r1.len = 4096;
    small = {r0, r1};
    r1.len = 8192;
    grown = {r0, r1};
    const std::string note = layout_reflow_note(
        layout_fingerprint(build_projection(std::move(small))),
        layout_fingerprint(build_projection(std::move(grown))));
    check("a region that GREW reflows the floor under Hilbert too",
          !note.empty(),
          "compaction shifted every later region and the HUD said nothing");
    check("the same-count wording says what actually changed",
          note.find("extents changed") != std::string::npos, note);
}
{
    // Silence by rule, not by threshold.
    const Projection p = atlas_of({kRefs[0]});
    check("the first layout is not a reflow",
          layout_reflow_note(LayoutFingerprint{}, layout_fingerprint(p)).empty(),
          "warned about a floor the reader had never seen");
    const Projection empty = build_projection({});
    check("a recording with no regions has no fingerprint",
          !layout_fingerprint(empty).valid,
          "an empty plane is not a layout to compare against");
    check("appearing from nothing is not a reflow",
          layout_reflow_note(layout_fingerprint(empty),
                             layout_fingerprint(p))
              .empty(),
          "a floor drawn for the first time is not a floor that moved");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/desktop_test_projection && ./build/desktop_test_projection`
Expected: FAIL — `layout_fingerprint` not declared.

- [ ] **Step 3: Implement the rule**

In `types.h`, beside `data_span_note` on `struct Projection`:

```cpp
// 3D-axis-budget T9: set on a weave whose layout MOVED relative to the
// previous one — a growing live capture gaining a region, mostly. Empty on a
// first weave and on any weave that re-laid the floor identically, so a
// non-empty note always means the reader's mental map just changed under
// them. The HUD surfaces it exactly like data_span_note above.
std::string layout_note;
```

In `projection.h`:

```cpp
// A digest of everything that decides WHERE an address lands on the plane:
// `order`, the compacted domain boundaries, the layout, and (under Atlas) the
// rectangles. Two projections with equal digests place every address
// identically, so comparing digests across a weave answers "did the floor
// move" without keeping the old projection alive.
//
// `valid` is false for a projection with no regions — there is no floor to
// have moved — and a comparison involving an invalid fingerprint is never a
// reflow.
struct LayoutFingerprint {
    bool valid = false;
    size_t regions = 0;
    uint64_t digest = 0;
};
LayoutFingerprint layout_fingerprint(const Projection &proj);

// The HUD note for a weave that moved the floor, or "" when it did not. NOT
// atlas-specific: adding a region shifts every later region's domain offset,
// so a Hilbert floor re-scrambles too, and a growing capture deserves the same
// notice under either layout.
std::string layout_reflow_note(const LayoutFingerprint &prev,
                               const LayoutFingerprint &now);
```

In `projection.cpp`, beside `region_cells`:

```cpp
namespace {
// FNV-1a, byte-wise over each value so the digest is byte-order-independent
// and an inserted region cannot collide with a resized one by luck of layout.
void fp_mix(uint64_t &h, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        h ^= (v >> (i * 8)) & 0xffull;
        h *= 1099511628211ull;
    }
}
std::string fp_regions(size_t n) {
    return std::to_string(n) + (n == 1 ? " region" : " regions");
}
} // namespace

LayoutFingerprint layout_fingerprint(const Projection &proj) {
    LayoutFingerprint fp;
    if (proj.regions.empty())
        return fp; // no floor to have moved
    fp.valid = true;
    fp.regions = proj.regions.size();
    uint64_t h = 14695981039346656037ull;
    fp_mix(h, proj.order);
    fp_mix(h, static_cast<uint64_t>(proj.layout));
    for (uint64_t d : proj.domain_off)
        fp_mix(h, d); // Hilbert's whole mapping, given order
    for (const AtlasRect &r : proj.rects) {
        fp_mix(h, r.x0);
        fp_mix(h, r.y0);
        fp_mix(h, r.x1);
        fp_mix(h, r.y1);
    }
    fp.digest = h;
    return fp;
}

std::string layout_reflow_note(const LayoutFingerprint &prev,
                               const LayoutFingerprint &now) {
    if (!prev.valid || !now.valid)
        return std::string(); // a first floor is not a floor that moved
    if (prev.digest == now.digest)
        return std::string(); // recomputed, but identical: not a reflow
    if (prev.regions != now.regions)
        return "floor re-laid out: " + fp_regions(prev.regions) + " became " +
               std::to_string(now.regions);
    return "floor re-laid out: " + fp_regions(now.regions) +
           ", extents changed";
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `make build/desktop_test_projection && ./build/desktop_test_projection`
Expected: PASS.

- [ ] **Step 5: Wire it through the shell**

Add to `SceneView` ([shell.h:67](../../../desktop/src/ui/shell.h#L67)), beside the other per-view state:

```cpp
// 3D-axis-budget T9: the layout digest of the LAST weave, so a growing
// capture can tell the reader its floor was re-laid out. Survives the
// live-growth SceneView reset (shell.cpp:279-285) on the same terms as the
// camera — it is the only thing that remembers the layout the reader was
// actually looking at.
space::LayoutFingerprint layout_fp;
```

Preserve it across the growth reset at [shell.cpp:279-285](../../../desktop/src/ui/shell.cpp#L279), adding one line to the block that already saves the camera, HUD and primer:

```cpp
scene3d::Camera cam = s.scenes[i].cam;
scene3d::HudState hud = s.scenes[i].hud;
dt_primer_state primer = s.scenes[i].primer;
space::LayoutFingerprint fp = s.scenes[i].layout_fp; // T9
s.scenes[i] = SceneView{};
s.scenes[i].cam = cam;
s.scenes[i].hud = hud;
s.scenes[i].primer = primer;
s.scenes[i].layout_fp = fp; // T9
```

**This line is the whole task's hinge.** Drop it and the fingerprint resets with everything else, `prev.valid` is false on every batch, and the notice can never fire — a silent failure that no pure test can catch, which is why Step 6 tests it through the shell.

Then in the weave at [shell.cpp:1188](../../../desktop/src/ui/shell.cpp#L1188), immediately after `proj.data_span_note = std::move(span_note);` — while `proj` is still in scope, *before* `build_terrain` moves it:

```cpp
// T9: did this weave move the floor the reader was already looking at?
const space::LayoutFingerprint fp = space::layout_fingerprint(proj);
proj.layout_note = space::layout_reflow_note(sv.layout_fp, fp);
sv.layout_fp = fp;
```

And surface it in `hud.cpp`, beside `data_span_note` at [:532](../../../desktop/src/scene3d/hud.cpp#L532):

```cpp
if (!terr.proj.layout_note.empty())
    ImGui::TextColored(kDim, "%s", terr.proj.layout_note.c_str());
```

- [ ] **Step 6: Write the wiring test**

The rule is tested; what is not is that the fingerprint *survives the reset*. Model it on the camera-preservation check that already guards this exact block ([test_shell.cpp:1858-1865](../../../desktop/test/test_shell.cpp#L1858)) — set a sentinel, grow the capture, re-sync, assert it survived:

```cpp
// 3D-axis-budget T9: the layout fingerprint must survive the live-growth
// SceneView reset, exactly as the camera above does. If it does not, every
// batch compares against an invalid fingerprint, the reflow notice can never
// fire, and the failure is SILENT — no pure test can see it, because the rule
// itself is correct.
ls.scenes[i].layout_fp.valid = true;
ls.scenes[i].layout_fp.digest = 0xD00Dull;
ls.scenes[i].layout_fp.regions = 7;
sess.feed_line(
    R"({"k":"df_step","step":1,"off":0,"disasm":"nop","ops":[]})");
shell_sync_live_tab(ls);
check("25t6/layout fingerprint preserved",
      ls.scenes[i].layout_fp.valid &&
          ls.scenes[i].layout_fp.digest == 0xD00Dull,
      "a live re-weave dropped the previous layout digest, so the reflow "
      "notice can never fire");
```

Note this asserts on the state *after* `shell_sync_live_tab` but *before* the lazy 3D weave runs (the weave is gated on `!sv.built` inside the pane draw, which this test does not reach), so the sentinel is still the value that was preserved rather than a freshly-computed digest. That ordering is what makes the check specific to the preserve-list.

- [ ] **Step 7: Run both**

Run: `make build/desktop_test_projection && ./build/desktop_test_projection`
Then: `make build/desktop_test_shell && ./build/desktop_test_shell`

Expected: PASS. **`test_shell` has pre-existing failures unrelated to this plan** (the attach / no-host cases), so read the named checks rather than the exit code, and confirm `25t6/layout fingerprint preserved` and `25t6/camera preserved` both pass.

- [ ] **Step 8: Commit**

```bash
git add desktop/src/space/types.h desktop/src/space/projection.h desktop/src/space/projection.cpp desktop/src/ui/shell.h desktop/src/ui/shell.cpp desktop/src/scene3d/hud.cpp desktop/test/test_projection.cpp desktop/test/test_shell.cpp
git commit -m "space: tell the reader when a growing capture re-lays the floor"
```

---

### Task 10: Flip the default, regenerate goldens, document

**Files:**
- Modify: `desktop/src/space/types.h` — default `Projection::layout` to `Atlas` (the member default lives on the struct, in `types.h`)
- Modify: `docs/_static/gui/*.png` (regenerate), `docs/internal/gui/` (a short brief recording the change)

**Interfaces:**
- Consumes: all prior tasks.
- Produces: the user-visible change.

- [ ] **Step 1: Audit for layout-specific assumptions BEFORE flipping anything**

Task 2 settled the two structural questions — `order` keeps its meaning and its value, and the byte-exact round trip is scoped to Hilbert — so this audit is now about *residual* assumptions rather than about the coordinate system. One is already known:

- [test_converge.cpp:51-52](../../../desktop/test/test_converge.cpp#L51) argues *"len is a power of four → a 1:1 domain, so distinct addresses land in distinct cells"*. The atlas does not guarantee this (see the contract table). Rewrite the setup against the contract — `project`/`unproject` region-level round trip — not against the mapping.

Run: `grep -rn "power of four\|power-of-four\|4\^\|1:1 domain\|hilbert\|Hilbert" desktop/test/ desktop/src/`

Triage each hit into one of: (a) a comment needing an update, (b) a test whose *setup* relies on the mapping — rewrite it against the contract, or (c) a genuine behavioural dependency, which is a design finding and must be reported, not patched over. **These are not golden churn.** A failure here is a real bug.

- [ ] **Step 2: Flip the default and run the whole suite**

Run: `make docker-desktop`
Expected: golden-image failures across the suite — that is the point of this step. Read every one; any *geometry* or *assertion* failure is a real bug, only *image* churn is expected. The camera is **not** a source of churn here: Task 4's calibration keeps `reset()`/`top_down()` bit-identical.

- [ ] **Step 3: Regenerate the doc screenshots — ONCE, from the merged tree**

Run the `--serve` screenshot flow (headless `--record` never emits `codeimage`, and `codeimage` gates every 3D scene). Regenerate **after** the last recorder edit, never per-agent; two agents regenerating one golden must regenerate from the merged tree rather than picking a side.

- [ ] **Step 4: Verify the screenshots are pairwise distinct — and that the labels actually landed**

Gate on distinctness, not just non-blankness — the same rule Task 7b encodes.

Then look at the floor, because this is the first render where Task 8's labels are live and three of this plan's five open risks can only be settled by eye:

- **the labels read** — right rectangle, not overlapping, legible at the default `radius`. If they pile up, raise `min_cells`; that is a threshold, not a redesign.
- **no sliver rects** (open risk 5) — the binary split never backtracks, so a pathological length distribution can still produce one. A labelled floor is where it becomes visible.
- **the flat worldline is not swallowed by the terrain** (open risk 2), and **the sediment strata do not read as rising from a path that no longer rises** (open risk 4). Both are the same lift constant.

Record what you saw in the brief either way — "looked at, nothing to fix" is a result, and these risks are marked unresolvable on paper precisely so this step is where they close.

- [ ] **Step 5: Write the brief**

Add `docs/internal/gui/61-scene-axis-budget.md` following `_conventions.md`, cross-referencing [53](../../internal/gui/53-3d-catalog-build-roadmap.md) to record that the depiction catalog now composes onto the atlas substrate. State plainly that `order` survived as the cell quantisation and that the byte-exact round trip is a Hilbert-only promise — those are the two facts a later reader is most likely to get wrong. Record the third alongside them: the in-place labels are **partial by design** (a legibility threshold, with the HUD's region legend as the complete list), so a later reader does not take an unlabelled rectangle for an unnamed region.

- [ ] **Step 6: Commit**

```bash
git add desktop/src/space/types.h docs/_static/gui docs/internal/gui/61-scene-axis-budget.md
git commit -m "scene3d: make the region atlas the default floor layout"
```

---

## Self-review

**Spec coverage.** Component 1 (atlas) → Tasks 2, 3, **8 (the in-place labels — its stated payoff)**, **9 (the reflow notice its risk table asks for)** and 10. Component 2 (time as animation) → Tasks 1, 5. Component 3 (optional motifs) → Task 6 (already shipped — verify only), Tasks 7a/7b (the harness and the opcode-channel image gate) and Task 7c (the crossings channel, in a pure test). Component 4 (camera fit) → Task 4. The spec's interim-guard requirement — that the clamp land first, on its own — is Task 1, ordered first deliberately.

**Why the motif gate is split across 7b and 7c.** The two channels are not testable by the same instrument, and pretending otherwise was the earlier draft's mistake. `opcode` is a terrain tint, so it is only visible in a rendered frame and its data comes from `space/` — 7b. `crossings` is geometry built by `views/build_crossing_layer`, which a GL test would have to link `views/` to produce, and whose real fixture can only come from a live tracer — 7c, in the pure test that already owns the contract. Gating both from one frame would have meant either dragging `views/` into the GL closure or, as written, asserting on a layer with no data uploaded.

**Type consistency.** `AtlasLabel` and `atlas_labels(const Projection &, uint32_t)` are defined once, in Task 8, and `LayoutFingerprint` / `layout_fingerprint` / `layout_reflow_note` once, in Task 9; all five live in `projection.h`/`projection.cpp` beside `region_cells`. `Projection::layout_note` (Task 9) and `Projection::data_span_note` are both plain `std::string` members set by the shell and read by `hud.cpp`, never computed in the HUD. `draw_atlas_labels` is declared once, in `hud.h`, beside `draw_trajectory_ruler`. `load_recording_file` returns **`std::optional<Recording>`** — Tasks 7b and 7c both test it as a bool (`!rec` / `rec.has_value()`), never against `nullptr`. `Scene`, `Camera` and `SceneLayers` are **`asmdesk::scene3d::`** and are written qualified everywhere in 7a's header, because `asmdesk::testing` does not enclose `asmdesk::scene3d`. `scene_traj_scale(uint32_t, uint64_t, float)` is defined in Task 1 and reused in Task 5. `Projection::Layout` / `rebuild_layout` are defined in Task 2 and consumed in Tasks 3 and 8. `AtlasRect` and `AtlasNode` are defined once, in Task 2, in cell coordinates, above `struct Projection` in `types.h`. `plane_boundary` / `split_rect` / `atlas_bytes_per_cell` / `atlas_cell` / `atlas_ordinal` are defined once, in Task 2's Interfaces block, and live in `projection.cpp`'s anonymous namespace beside `d2xy`/`domain_shift`. `split_rect` returns `bool` and takes an `int32_t *out` — it is the one helper here whose signature is not obvious from its job, because it can refuse. `Camera::fit` and its two pad constants are defined once, in Task 4. `Image` / `gl_context_available` / `render_plane_scene` / `image_ink_fraction` / `image_blank` / `scene_exists` are defined once, in Task 7a's `gl_offscreen.h`; `images_distinct` once, in Task 7b's `image_distinct.h`; and `test_scene_fbo.cpp` is re-pointed at the harness rather than keeping a second copy.

**Where the spec is superseded.** Two of the spec's statements did not survive verification and this plan overrides them; both should be read as corrected here rather than followed as written:

- Component 3's *"it defaults ON, matching the registry's existing all-true convention"* — there is no all-true convention (Task 6). And both channels already exist as layers, so Component 3 requires no new layer at all.
- The measured-facts table's *"all address→plane arithmetic funnels through two helpers … the blast radius of a layout swap is contained"* — true of `locate.h`/`stepplace.h`, which already delegate to `project()`, but `region_cells()` was a third route that walks the mapping directly (Task 2 fixes it) and `1 << order` cell arithmetic is copied deliberately in four places (Task 2's decision keeps all four valid).

**Verified against the tree (2026-08-03), across three passes. Corrections made in this revision:**

| Assumption | Verdict |
|---|---|
| `Projection` lives in `projection.h` | **false** — it is in `space/types.h:30-55`. Tasks 2 and 9 corrected |
| `atlas_cells_per_side()` is a needed new concept | **false** — `1 << order` already is the grid side, read in 13 sources and 10 tests. Dropped; the decision to keep `order` is now stated up front instead of deferred to Task 10 |
| `scene_locate_off(p, region, off, &u, &v) -> bool` | **false** — `Located scene_locate_off(const Projection&, const Recording&, uint64_t)`. Task 3 now probes through `place_address(proj, addr)`, which needs no `Recording` |
| `locate.cpp`/`stepplace.cpp` re-derive Hilbert arithmetic | **false** — both already call `proj.project()`. Task 3 is a regression gate, not a refactor, and says so |
| `region_cells()` is layout-neutral | **false** — it walks `d2xy` directly (`projection.cpp:426`). Task 2 gives it an atlas branch; `test_focus.cpp` guards it |
| `reset()` can be routed through a fresh `fit()` | **false** — `test_camera.cpp:111` pins `reset()`'s radius to the default. Task 4's pad factors are calibrated so the whole-plane fit reproduces 2.2/1.9 exactly |
| The motif layer is new work | **false** — `SceneLayers::opcode` and `SceneLayers::crossings` both ship, the latter already coloured by `crossing_hue(SyscallClass)`. Task 6 no longer adds a bool |
| `SceneLayers` defaults are all-true | **false** — additive layers ON, terrain re-lifts OFF, documented per field. Task 6 pins the real rule |
| The PC comet is new work | **false** — `SceneLayers::vehicle` is "the followed-citizen head + comet tail", with `find_head` and `uHeadY` already wired. Task 5 is now about removing time from Y and repairing the three Y-derived consumers |
| Task 7 needs only an image comparator | **false** — five of its six assumed helpers do not exist. Now split into 7a (the harness, with every helper written out) and 7b (the gate) |
| `draw_trajectory_ruler`'s call site is in `hud.cpp` | **false** — it is `shell.cpp:1829`, and that is its *only* caller, already gated to `SceneKind::Plane`. No other scene calls it, because no other scene uses `Scene` at all. Task 5 re-captions rather than deletes |
| `reset()`/`top_down()` can share one `fit()` | **false** — `reset()` restores the default target, `top_down()` must PRESERVE a panned one. Task 4 splits `fit_radius` out for exactly this |
| `Camera::radius` assertions use a tolerance | **false** — `test_camera.cpp:111` and `:208` compare with `==`. Task 4's radius rule is exact by construction, not calibrated |
| `traj_scale_` is the worldline's own Y scale | **false** — five subsystems place geometry on it (causal spurs, lifetime pillars, access arcs, sediment strata, the line shader). Task 5 is blocked on a spec decision because of it |
| `Scene` knows which `SceneKind` it is drawing | **false, and it does not need to** — `Scene` IS the Plane renderer; `gl_scene_host.cpp:74` routes every other kind to `standalone_` first. `flat_time` is a plain member default with no setter |
| Fixtures belong in `desktop/test/fixtures/` | **it depends on the producer, and that is now the deciding fact** — `make asmtrace-golden` writes to `tests/golden-asmtrace/` and `asmtrace-golden-check` holds those byte-stable, so 7b's three generated scenes go there; `desktop/test/fixtures/` has NO regeneration or byte gate, which is exactly why 7c's frozen live capture belongs there. Neither directory takes a hand-authored scene in this plan |
| `tools/asmtrace_record.c` can produce a syscall-bearing recording | **false** — zero `syscall` occurrences in 2511 lines; it is a Unicorn emulator over hand-assembled byte arrays. `syscall` rows come from `cli/asmspy.c`, the live tracer. This is what forced the 7b/7c split |
| `make docker-desktop-test TEST=<x>` runs one test | **false** — no such target, no `TEST=` parameter. See Global Constraints |
| `test_scene_traj.cpp` exists | **false** — Task 1 creates *and registers* it |
| `test_projection` / `test_camera` / `test_locate` / `test_stepplace` / `test_layers` / `test_focus` exist | **true** — Tasks 2, 3, 4, 6 extend existing files |
| Pixel assertions can run in CI | **true** — `Dockerfile.desktop` pins software Mesa + EGL for exactly this. The gate must be judged from `make docker-desktop`; on a host without EGL the GL lane is not built at all |
| The repo uses gtest | **false** — no gtest/catch2/doctest anywhere. Every test is a standalone `main()` over a hand-rolled `check(what, cond, why)` |
| `Projection::unproject` takes `(u, v, &addr)` | **false** — four args, `(u, v, uint64_t *addr, const Region **r)` |
| `TrajPoint` is `{addr, t, ...}` | **false** — it is `{t, addr, fidelity, is_access, tid, placed}` |
| A test of the scale rule needs `Scene` (and therefore GL) | **false, and it was a design smell** — the rules live in a header-only `scene3d/trajscale.h`, joining `camera.h` and `linequad.h` |

**Fourth pass — what a readiness review found still wrong, and what this revision changed.** The three passes above verified every *signature* and *file reference*; what they did not do was execute the algorithm on the plan's own fixtures. Doing that surfaced four defects, three of them in Task 2's core.

| Defect | Fix in this revision |
|---|---|
| The layout algorithm had **no byte→cell quantisation at all**: `r = off / w, c = off % w` treats a byte offset as a cell ordinal. Hilbert quantises at [projection.cpp:340](../../../desktop/src/space/projection.cpp#L340); the atlas had no equivalent, so any region whose `len` exceeded its rect's cell count walked straight out of its own rectangle — breaking the region round trip the contract table lists as *guaranteed* | Part (c), `atlas_bytes_per_cell`, with the `k < cells` bound proved. A new test block forces `bytes_per_cell == 4` and pins it |
| The serpentine adjacency test was **arithmetically unsatisfiable**. `kOne` len 65536 → order 8 → 1 byte per cell, so offsets 0 and 64 are 64 cells apart; the check asserted ≤ 1.5. It would have failed under Hilbert too | Rewritten to test the **row break** (`w-1` vs `w`), which is the only thing serpentine buys over row-major, with the 1-byte-per-cell premise asserted rather than assumed |
| "The budgets sum to `n*n` and the strips tile, so the grid is covered by construction" **does not follow** — tiling is a property of the geometry. BHvW squarify also sorts by descending area, which would break both base-ordering and `rects`' index-parallelism with `regions` | Replaced by an order-preserving binary split whose children tile their parent exactly at every level, with the `need_l`/`need_r` clamps that keep a cell per region |
| A linear scan of `rects` in `unproject` would be **O(cells × regions)**: [terrain.cpp:119](../../../desktop/src/space/terrain.cpp#L119) calls it once per cell over the whole plane ("one O(cells) sweep"), 16.7 M times at order 12 | `AtlasNode` — the split tree, descended in O(log regions) |
| Task 7 called four helpers it never defined, `render_plane_scene` most of all (a whole Recording→Terrain→Scene→FBO→readback path) | Split into 7a/7b; every helper is now written out |
| `render_plane_scene` uploaded no motif data, so 7b would have set `opcode = true` against an empty `tex_opclass_` byte map and `crossings = true` with no spurs uploaded at all — both channels asserted with nothing behind them, failing for a reason unrelated to the encoding | 7a now calls `set_opcode_terrain(build_opcode_terrain(...))`; the crossings channel moved out of the frame entirely, into Task 7c |
| `image_floor_fraction(bare) >= 0.5` was unsound twice: viewport coverage is a camera-framing artefact, and the atlas does not raise the decodable-cell count anyway | Dropped, with the reasoning recorded in 7a's Interfaces block and the honesty note under "the one decision" |

**Fifth pass — mapping the deletion surface rather than the addition surface.** Asking "what stops being referenced?" rather than "what gets written?" found three more, one of which stops Task 5 outright.

| Defect | Fix in this revision |
|---|---|
| **Task 4's calibrated pad factors cannot satisfy the test they were calibrated against.** [test_camera.cpp:111](../../../desktop/test/test_camera.cpp#L111) is `c.radius == d.radius` — *exact* float equality — and `1.0f / tanf(0.4f) * 0.9301f` is ≈ 2.19988, not `2.2f`. No pad value fixes a two-rounding product reliably across libm versions. The draft's "a failure at `:111` means the calibration is off" sent the implementer into an unwinnable loop | Radius is now `extent * kWholePlaneRadius` — linear, exact at extent 1 by construction, and free of `fovy`, `tan` and both magic constants |
| **`top_down()` would silently start recentring the target.** Today it leaves `target` alone and [shell.cpp:1464](../../../desktop/src/ui/shell.cpp#L1464) calls it on a possibly-panned camera; routing it through `fit()` would teleport the user to the plane centre. No existing test catches it — the file's top-down block uses a default camera | `top_down()` takes `fit_radius` only, never `fit`; a new test pans first and pins the target |
| **`traj_scale_` is a five-consumer shared axis, not the worldline's private scale.** Causal spurs, lifetime pillars, access arcs, sediment strata and the line shader all place geometry at `t * traj_scale_`, and [scene.h:365](../../../desktop/src/scene3d/scene.h#L365) documents that a spur *hangs on a worldline vertex at that Y*. Flattening the path alone detaches every spur foot. The spec appears to contradict itself here — "Y carries access density, and nothing else" versus "the 14 layers stand" | **Resolved** (see below). Task 5 now names all five call sites, flattens the two default-ON consumers and leaves the three opt-in ones on the axis, and adds the spur flatten, the shader change and the ruler gate + re-caption to its file list |

**Sixth pass — running the algorithm instead of reading it.** The fifth pass verified every signature and line reference; it still did not *execute* Task 2's layout. Transcribing the helpers verbatim and running them found one crash and three smaller errors. The four plan fixtures were re-run after each fix and produce byte-identical numbers to the ones the tests assert (orders 8/8/12/9, code-rect fraction 0.0625, `n*n == 65536`, row-break distance 1 cell, `bytes_per_cell == 4`, exact tiling, no overlap, clean region round trip).

| Defect | Fix in this revision |
|---|---|
| **`split_rect` divided by zero.** Its stated precondition — "the `need_l`/`need_r` clamps below preserve it at every level" — was false. When `need_l + need_r > w` (a `ceil()` rounding artefact, since `ceil(a/h) + ceil(b/h)` can exceed `ceil((a+b)/h)`), `std::min(std::max(...))` collapses the cut past the far edge, a child rect gets zero height, and the recursion divides by zero. Reproduced under ASan at 3809 one-byte regions on the 4096-cell order-6 plane. `rebuild_layout`'s `nreg > plane` guard is far too loose to catch it | The scan that picks `mid` now **tests feasibility instead of clamping afterwards**: a candidate split is admissible only when the columns it forces on both sides fit. `split_rect` returns `bool`; `rebuild_layout` falls back to Hilbert and clears `rects`/`nodes` rather than leaving a half-built layout. Verified over every `nreg` in [2, 4096] on the order-6 plane and 3000 randomised skewed length distributions: **0 crashes, 0 fallbacks, 0 contract violations** — so the refusal path is a guard, not a live one. A saturated-plane block in Task 2 Step 1 pins it |
| `region_cells`'s atlas branch was placed "before the Hilbert run walk", which is *after* the `hi <= lo` guard and after the Hilbert path's `n` | Placement is now specified exactly — above `hi <= lo` (a zero-length region owns a rect but has no domain span, so the guard would contradict this task's own assertion and `test_focus`'s "owns at least one cell"), with its own `n` |
| `b[nreg] = plane` was documented as PINNED but the upward clamp loop ran `i = 1..nreg` *inclusive*, so a zero-length trailing region pushes it to `plane+1` and the downward loop, starting one below, never restores it | The upward sweep stops at `nreg-1`. Harmless for the tiling either way — the geometry sets the areas, not the budgets — but the comment now describes what the code does |
| `egl_up` was described as lifted verbatim; its real signature is `egl_up(EGLDisplay *, EGLContext *, std::string *)`, which hands the display and context back to a caller that keeps them in locals | 7a now keeps `egl_up` intact (`test_scene_fbo.cpp` still calls it with the out-params after Step 2) and wraps it in `egl_up_once`, which owns the two statics |

**The Component 2 / Non-goals contradiction, resolved.** Read "Y" in *"Y carries access density, and nothing else"* as **the scene you get by default** — which is the only thing Component 2's complaint is about — and both spec statements hold at once. An overlay the reader switches on is not competing for the default view's axis budget. The split falls exactly along [scene.h:60-144](../../../desktop/src/scene3d/scene.h#L60)'s own documented ON/OFF convention, the same one Task 6 pins, so it is the registry's rule rather than a compromise invented for this plan:

| | Layers (verified defaults) | Task 5 |
|---|---|---|
| default **ON** | `vehicle` ([:59](../../../desktop/src/scene3d/scene.h#L59)), `crossings` ([:123](../../../desktop/src/scene3d/scene.h#L123)) | flatten both, through one shared `traj_vertex_y` call so the spur foot stays welded to the path |
| default **OFF** | `lifetime` ([:103](../../../desktop/src/scene3d/scene.h#L103)), `data_ribbon` ([:108](../../../desktop/src/scene3d/scene.h#L108)), `sediment` ([:113](../../../desktop/src/scene3d/scene.h#L113)) | untouched; `data_layers_gl.cpp` takes no diff in this task |

The decisive fact is geometric, not aesthetic: the lifetime pillars and the sediment strata emit segments whose **two endpoints share the same `(u,v)`**, so trace-time Y is their entire extent — flattening makes them zero-length and GL draws nothing. For those two, "flatten" is a synonym for "delete", and the spec's rationale (a pre-drawn future crowding the view) does not reach a Gantt summary anyway. The spurs, by contrast, are diagonal and survive as tents, because `rail_lift` is already a constant that encodes nothing.

**Two claims from the fourth-pass readiness review were themselves wrong, and are corrected here rather than left standing:**

- *"`scene_traj_scale` has no surviving consumer once Task 5 lands."* False — the three opt-in layers ride the scale it computes. Task 1 is load-bearing permanently, not interim, and Task 1 and Task 5 both now say so.
- *"`draw_trajectory_ruler` becomes dead code, so delete the call."* Half right: it is the function's only caller and it is Plane-gated, but the axis it labels stays alive for those three layers. Deleting it would strand them on an unlabelled axis, and leaving it unconditional would label an axis the *default* scene no longer has. It is gated on the effective (post-`lod_apply`) layer set and re-captioned, not removed.

**Seventh pass — executing the algorithm independently, and asking what the SPEC promised rather than what the plan wrote.** The sixth pass claimed to have run Task 2's helpers; this pass re-ran them from scratch (verbatim transcription, ASan + UBSan) and **confirmed every number**: orders 8/8/12/9, code-rect fraction exactly 0.062500, row-break distance 1.0 cells, `bytes_per_cell == 4`, exact tiling, no overlap, clean region round trip — plus **0 fallbacks, 0 degenerate rects, 0 non-tiling** over every `nreg` in [2, 4096] on the order-6 plane and 3000 randomised skewed length distributions. The feasibility-scan rewrite is sound. What this pass found instead was three compile-blockers in the *newest* material (7a/7b/7c, added after the previous readiness review) and one deliverable the plan had silently dropped.

| Defect | Fix in this revision |
|---|---|
| **Task 7b Step 5 named a mechanism that cannot do the job.** "Add three `ROUTINES[]` entries … following the byte-literal pattern the file already uses for `SCENE_HOT_LOOP`" is two mutually exclusive instructions. `ROUTINES[]` is `{name, args[3], nargs, steps_cap}` — no bytes — and `record_one` resolves it through `asmtest_corpus_routine(name)`, a lookup of a **compiled symbol** in the linked corpus objects. Followed literally, `make asmtrace-golden` fails with `no corpus routine`; the fixtures the whole gate stands on would never exist | Step 5 rewritten around `record_scene_abs(dir, out, label, bytes, len, args, nargs, cap)` — the emitter `SCENE_HOT_LOOP` actually feeds, which takes an explicit output name and a raw byte array, with the call block written out. Confirmed it emits the per-instruction disasm text `build_opcode_terrain` reads, so the opcode channel has real data. Three emitter constraints (runs to `ret` under Unicorn, `REC_WINDOW` bound, verify the SIMD mnemonics classified) added |
| **Task 7c could not compile.** `load_recording_file` returns `std::optional<Recording>` (`recording.h:134`) and the test wrote `rec != nullptr` twice. 7b used `if (!rec)` correctly — the two halves disagreed | `std::optional<Recording>` + `rec.has_value()`, with the reason in a comment so it does not regress. `<optional>` added to the includes |
| **Task 7a's header could not compile.** It declared everything in `namespace asmdesk::testing` while naming `Scene`, `Camera` and `SceneLayers` unqualified — those are `asmdesk::scene3d`, and only `Recording` is `asmdesk`. `test_scene_fbo.cpp` gets away with it via a file-scope `using namespace asmdesk::scene3d;`; a header cannot, and 7b's `using` lines come after its `#include` | Every signature qualified `scene3d::`, in both the Interfaces block and the header body, with an explicit "do not fix this with a `using namespace` in a header" note |
| **Component 1's stated payoff had no task.** The spec's deliverable is "region-major, 100 % packed, **labelled in place**" and it says outright *"This — not locality — is the real win."* Tasks 2/3/9 build the rects, prove the funnel and flip the default; **none of them drew a label.** Today's region names exist only as a side-panel legend (`hud.cpp:1176-1187`) | **New Task 8**, ordered before the default flip so the labels are live when the screenshots regenerate. Pure placement rule in `space/` (`atlas_labels`, tested in the null harness beside `region_cells`), thin `draw_atlas_labels` in `hud.cpp` reusing `draw_trajectory_ruler`'s world→screen transform. Refuses under Hilbert rather than fabricating an anchor on a snake, and is partial by design above a legibility threshold — with the legend as the disclosure |

**Eighth pass — the spec's Risks table is a requirements section too.** Pass 7 checked the spec's deliverable bullets and found the missing labels; it still read the Risks table as commentary. It is not: each row names a mitigation, and one of them — *"record the reflow in the HUD"*, against a failure mode the spec spells out as **"a growing capture that reflows silently is the failure mode to avoid"** — had no task. It was briefly recorded as a deferred open risk on the grounds that `Projection` has nowhere to hold a previous region set. That reasoning was wrong about the tree: the state does not belong on `Projection` at all.

| Defect | Fix in this revision |
|---|---|
| **The reflow notice had no task**, and the deferral argued that nothing could hold the previous layout. But a live batch does not re-weave in place — [shell.cpp:279-285](../../../desktop/src/ui/shell.cpp#L279) **replaces the whole `SceneView`**, already carrying the camera, HUD and primer across the reset for exactly this class of reason. The place to hold a previous-layout digest is that preserve-list, which costs one field and one line | **New Task 9.** A pure `layout_fingerprint()` / `layout_reflow_note()` pair in `space/` (tested in the null harness beside `region_cells` and `atlas_labels`), a `layout_fp` on `SceneView` preserved across the growth reset, and a `Projection::layout_note` surfaced beside `data_span_note` — the note mechanism the tree already has. Code compiled and run under `-Wall -Wextra` + ASan before landing |
| The deferral also assumed the notice was atlas-specific, so it could wait for the flip | **False, and it is why the task stands alone.** Adding a region shifts every later region's `domain_off`, so a *Hilbert* floor re-scrambles just as surely as a treemap re-tiles. The digest covers `order` + `domain_off` + `rects`, works under both layouts, and lands before Task 10 rather than after it |
| The spec's *other* half — "recompute only when that set changes" — was never checked, only assumed | Verified: `rebuild_layout()` runs inside `build_projection()`, which the shell calls only from the `!sv.built` weave gate ([shell.cpp:1176](../../../desktop/src/ui/shell.cpp#L1176)), so it is already once per weave and needs no code. Task 9 says so rather than re-implementing it |

Four smaller corrections in the seventh pass:

| Defect | Fix |
|---|---|
| Task 2 said to leave the 10 000-address byte-exact loop alone "because it already runs on a default-constructed (`Hilbert`) projection" — **Task 10 flips that default to `Atlas`**, falsifying the sentence and leaving a Hilbert-only assertion running under the atlas. It happens to survive (that fixture's plane exceeds its domain, so every region gets `bytes_per_cell == 1`), but by accident of three fixture lengths, not by contract | The loop is now **pinned** to `Layout::Hilbert` on an explicit copy, with the accident recorded so nobody restores the reliance on it |
| Task 4's file reference `camera.h:99-107` is `eye()`/`view()`; `reset()` is `:81` and `top_down()` is `:84-88`. And `frame(u, v, radius)` at `:75` already owns both of `fit()`'s clamps plus the non-reorienting rule the task depends on | References corrected; `fit()` now routes through `frame()` instead of re-deriving the clamps |
| Task 3 called the `test_focus.cpp` sweep "a small loop change", but `:130` is `const space::Projection proj` — it cannot be flipped in place. `test_stepplace.cpp` also has no `<cmath>` for the `std::fabs` the new block uses | Both stated, with the two-copy sweep written out |
| Task 1 committed after running only its own binary, yet it changes `traj_scale_` — a rendered-frame input — for exactly the recordings this plan exists to fix | A full-suite step added before the commit, with the "ordinary case is bit-identical, so churn means the defect became visible" reasoning |

**Remaining known risks, not resolvable on paper.**

1. ~~Task 7b's memcpy recording may have to be hand-authored~~ — **resolved, see Task 7b Step 0.** The premise was wrong twice: the opcode channel never reads `mem`, so nothing about that gate is at risk from `mem` having no producer; and the *syscalls* case, which the draft called safe, was the one the generated corpus could never produce — `asmtrace_record.c` emits no `syscall` events at all. Nothing is hand-authored now: the three image fixtures are generated, and the crossings fixture is a frozen real capture. What remains is a bounded, stated limit rather than a risk: the generated routines are author-chosen byte arrays, so 7b's claim is about honest *classification* of real disasm, not about real programs, and the test header says so.
2. Task 5's flat worldline is coplanar with the terrain floor. The lift constant is an eyeball judgement that only the container render can settle — a path that vanishes under a tall cell is the failure mode, and it will not show up in any pure test.
3. Task 2's cell budgets can give a region fewer cells than it has bytes (quantised away, `bytes_per_cell > 1`) **or more** (`bytes_per_cell == 1`, the rect's tail decoding to nothing). Both are honest and both are tested; the plan asserts only the region-level round trip, which is what the atlas can promise. If a caller is later found to depend on byte-exactness under `Atlas`, that is a design finding, not a rounding bug to paper over.
4. With the path flat and `sediment` switched on, the worldline runs *underneath* strata that still rise from it — the two layers now read at different scales on the same axis. Legibility, not correctness; Task 10 Step 4 is where it shows up, and the fix if needed is the same lift constant risk 2 already covers.
5. The binary split's aspect ratios are *reasonable*, not *optimal* — it always cuts the longer side, but it does not backtrack the way squarify does. A pathological length distribution could still produce a sliver. That is a legibility question only a rendered floor can answer, and Task 10 Step 4 is where it would show up.
6. The reflow notice (Task 9) reports **that** the floor moved, never **how much**. Quantifying the disruption would need the previous rects kept alive and a distance metric over two tilings, and no threshold on that metric could be justified from anything measured here — so the note is a fact, and whether a reflow was disruptive stays the reader's judgement. If live use shows the notice firing constantly on ordinary growth, the fix is to make the layout stable under append (a real design change), not to add a silence threshold.
