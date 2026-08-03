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

- [ ] **Step 6: Commit**

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

```cpp
// Returns this subtree's encoded child reference: a `nodes` index, or
// -(region index + 1) for a leaf. PRECONDITION: the rect has at least
// (hi - lo) cells; rebuild_layout establishes it at the root and the
// need_l/need_r clamps below preserve it at every level.
int32_t split_rect(const std::vector<uint64_t> &b, size_t lo, size_t hi,
                   AtlasRect r, std::vector<AtlasRect> &rects,
                   std::vector<AtlasNode> &nodes) {
    if (hi - lo == 1) {
        // The leaf takes the WHOLE rect. This is what makes the tiling exact,
        // and it is why a region's final cell count can differ from its budget
        // by the rounding accumulated down the tree — the budget picks the
        // cut, the geometry picks the area, and the geometry wins.
        rects[lo] = r;
        return -static_cast<int32_t>(lo) - 1;
    }
    const uint64_t span = b[hi] - b[lo];
    // Cut the region RUN as near the half-budget as possible, keeping at least
    // one region on each side. Linear, not binary-searched: nreg is small next
    // to the cell count and a scan cannot get the tie-breaking wrong.
    size_t mid = lo + 1;
    uint64_t best = UINT64_MAX;
    for (size_t i = lo + 1; i < hi; i++) {
        const uint64_t left = b[i] - b[lo];
        const uint64_t d =
            left * 2 > span ? left * 2 - span : span - left * 2;
        if (d < best) {
            best = d;
            mid = i;
        }
    }
    const uint64_t left = b[mid] - b[lo];
    const uint32_t w = r.x1 - r.x0, h = r.y1 - r.y0;
    AtlasNode nd;
    AtlasRect a = r, c = r;
    if (w >= h) {
        // Reserve enough columns that each side still has a cell per region.
        const uint32_t need_l = uint32_t((mid - lo + h - 1) / h);
        const uint32_t need_r = uint32_t((hi - mid + h - 1) / h);
        uint32_t cut =
            r.x0 + uint32_t((uint64_t(w) * left + span / 2) / span);
        cut = std::min(std::max(cut, r.x0 + need_l), r.x1 - need_r);
        nd.axis = 0;
        nd.cut = cut;
        a.x1 = cut;
        c.x0 = cut;
    } else {
        const uint32_t need_l = uint32_t((mid - lo + w - 1) / w);
        const uint32_t need_r = uint32_t((hi - mid + w - 1) / w);
        uint32_t cut =
            r.y0 + uint32_t((uint64_t(h) * left + span / 2) / span);
        cut = std::min(std::max(cut, r.y0 + need_l), r.y1 - need_r);
        nd.axis = 1;
        nd.cut = cut;
        a.y1 = cut;
        c.y0 = cut;
    }
    // Reserve OUR slot before recursing — the children append to `nodes`.
    const size_t self = nodes.size();
    nodes.push_back(AtlasNode{});
    nd.lo = split_rect(b, lo, mid, a, rects, nodes);
    nd.hi = split_rect(b, mid, hi, c, rects, nodes);
    nodes[self] = nd;
    return static_cast<int32_t>(self);
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

**First, scope the existing byte-exact round-trip loop.** The 10 000-address check at [test_projection.cpp:110-120](../../../desktop/test/test_projection.cpp#L110) is a Hilbert-layout property (see "the one decision" above). Leave the loop exactly as it is and give it a comment saying so — it already runs on a default-constructed (`Hilbert`) projection, so no code changes, but the reason must be recorded or a later reader will "fix" the atlas to satisfy a promise it never made.

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
// Falls BACK to Hilbert, silently in code but visibly in p.layout, when the
// plane has fewer cells than there are regions — impossible for a real map,
// since `order` is sized from the domain's byte count, but reachable with a
// synthetic domain. A caller that cares reads p.layout back.
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
    b[nreg] = plane; // PINNED, not computed: the last boundary IS the plane,
                     // which is what makes the areas sum exactly.
    // A cell each, and strictly increasing: a region granted zero cells would
    // get an empty rect that project() could never place an address into.
    // Sweep up, then back down off the pinned top; both fit because nreg <= plane.
    for (size_t i = 1; i <= nreg; i++)
        if (b[i] < b[i - 1] + 1)
            b[i] = b[i - 1] + 1;
    for (size_t i = nreg; i-- > 1;)
        if (b[i] + 1 > b[i + 1])
            b[i] = b[i + 1] - 1;
    p.rects.assign(nreg, AtlasRect{});
    p.nodes.reserve(nreg ? nreg - 1 : 0);
    split_rect(b, 0, nreg, AtlasRect{0, 0, n, n}, p.rects, p.nodes);
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

And `region_cells()` gets an atlas branch, before the Hilbert run walk:

```cpp
if (proj.layout == Projection::Layout::Atlas) {
    if (region_index >= proj.rects.size())
        return out;
    const AtlasRect &r = proj.rects[region_index];
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

Add to `desktop/test/test_stepplace.cpp`, reusing its helpers. `place_address` is the right probe — it takes `(Projection, addr)` and needs no `Recording`:

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

Then add the same two-layout sweep around `test_focus.cpp`'s existing `region_cells` membership checks (`:242`, `:269`, `:284`, `:299`) — that file already asserts the containment and disjointness contract, so running it under `Atlas` too is a small loop change and it is the strongest single guard on Task 2's `region_cells` branch.

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
- Modify: `desktop/src/scene3d/camera.h` (`reset()`/`top_down()` at `:99-107`)
- Test: `desktop/test/test_camera.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks — takes four plain floats so `camera.h` stays linmath-only.
- Produces: `void Camera::fit(float u0, float v0, float u1, float v1, float pad);` with `reset()`/`top_down()` routed through it.

**`fit` must reproduce the historical framing exactly.** [test_camera.cpp:109-113](../../../desktop/test/test_camera.cpp#L109) asserts `reset()` restores `radius` to the default-constructed value, and the same check recurs at `:208` and `:283`; the golden images depend on it too. So the pad factors are **calibrated** so that `fit` over the whole unit square returns exactly today's `2.2` (reset) and `1.9` (top_down). With a 100 %-packed atlas the occupied bounds *are* the unit square, which is the spec's own point: the fit becomes a regression guard rather than a workaround, and it only starts moving the camera if the floor ever stops being fully packed.

- [ ] **Step 1: Write the failing test**

Add to the existing `desktop/test/test_camera.cpp`, reusing its `check` helper:

```cpp
// 3D-axis-budget T4: fit-to-bounds. reset() framed the whole unit plane
// unconditionally, so a floor occupying a fraction of it sat small in a
// mostly-empty viewport with no way to say so.
{
    // The calibration that keeps every existing reset()/top_down() assertion
    // and every golden image true: fitting the WHOLE plane must reproduce the
    // historical radius exactly, not merely closely.
    Camera c;
    c.fit(0.0f, 0.0f, 1.0f, 1.0f, Camera::kFitPadDefault);
    check("fitting the whole plane reproduces the default framing",
          std::fabs(c.radius - Camera{}.radius) < 1e-3f,
          "fit gave radius " + std::to_string(c.radius) + ", the default is " +
              std::to_string(Camera{}.radius));
    Camera t;
    t.fit(0.0f, 0.0f, 1.0f, 1.0f, Camera::kFitPadTopDown);
    check("fitting the whole plane top-down reproduces its framing",
          std::fabs(t.radius - 1.9f) < 1e-3f,
          "fit gave radius " + std::to_string(t.radius) + ", wanted 1.9");
}
{
    Camera whole;
    whole.fit(0.0f, 0.0f, 1.0f, 1.0f, Camera::kFitPadDefault);
    Camera quarter;
    quarter.fit(0.0f, 0.0f, 0.5f, 0.5f, Camera::kFitPadDefault);
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
    c.fit(0.0f, 0.0f, 0.001f, 0.001f, Camera::kFitPadDefault);
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
// The pad factors, calibrated so fit() over the WHOLE unit plane reproduces
// this struct's historical framing exactly: 1/tan(fovy/2) == 2.36522 at the
// fixed fovy 0.8, so 2.2 == 2.36522 * 0.9301 and 1.9 == 2.36522 * 0.8033.
// Calibrated rather than chosen, because reset()/top_down() are pinned by
// test_camera.cpp and by every golden image — a fit that framed the packed
// atlas "about right" would churn both for no gain. With a 100%-packed floor
// the fit is therefore a REGRESSION GUARD: it only moves the camera if the
// floor ever stops being fully claimed.
static constexpr float kFitPadDefault = 0.9301f;
static constexpr float kFitPadTopDown = 0.8033f;

// Frame an axis-aligned region of the floor: centre the target on it and pull
// the radius back far enough that its larger extent fits the vertical fov.
// Clamped through the SAME kMinRadius/kMaxRadius dolly uses, so a degenerate
// region cannot produce a camera inside the near plane.
void fit(float u0, float v0, float u1, float v1, float pad) {
    target[0] = clampf(0.5f * (u0 + u1), 0.0f, 1.0f);
    target[1] = 0.0f;
    target[2] = clampf(0.5f * (v0 + v1), 0.0f, 1.0f);
    const float extent = std::fmax(std::fabs(u1 - u0), std::fabs(v1 - v0));
    radius = clampf(extent / std::tan(0.5f * fovy) * pad, kMinRadius, kMaxRadius);
}
```

`reset()` becomes: `*this = Camera{}; fit(0,0,1,1, kFitPadDefault);` — which by calibration leaves every field bit-identical to today. `top_down()` keeps `yaw = 0`, `pitch = kPitchLimit` and calls `fit(0,0,1,1, kFitPadTopDown)`.

- [ ] **Step 4: Run test to verify it passes**

Run: `make build/desktop_test_camera && ./build/desktop_test_camera`
Expected: PASS — the new checks **and** the pre-existing reset/top_down ones. A failure at `:111` means the calibration is off, not that the old check is stale.

- [ ] **Step 5: Commit**

```bash
git add desktop/src/scene3d/camera.h desktop/test/test_camera.cpp
git commit -m "scene3d: give the camera a fit-to-bounds preset, calibrated to the current framing"
```

---

### Task 5: Time leaves the spatial budget

**Files:**
- Modify: `desktop/src/scene3d/trajscale.h` — add `traj_vertex_y` and `comet_window` (Task 1 created this header)
- Modify: `desktop/src/scene3d/scene.h`, `desktop/src/scene3d/scene.cpp` (`set_trajectories`, `render`) — call them
- Modify: `desktop/src/ui/shell.cpp:1829` — drop the `draw_trajectory_ruler` call for the Plane scene (the call site is in **shell.cpp**, not hud.cpp; `draw_trajectory_ruler` itself stays in hud.cpp for the scenes that still spatialise time)
- Test: `desktop/test/test_scene_traj.cpp` (created and registered by Task 1 — no `mk/desktop.mk` change needed here)

**Interfaces:**
- Consumes: `scene_traj_scale` (Task 1) — retained for the non-Plane scenes that still spatialise time.
- Produces, in `scene3d/trajscale.h` (header-only, so the test needs no GL):
  - `float traj_vertex_y(uint64_t t, float scale, bool flat)`
  - `std::pair<uint64_t, uint64_t> comet_window(uint64_t follow_step, uint32_t tail)`
- Produces, on `Scene`: `uint32_t comet_tail = 256;` (trail length in **steps**) and `bool flat_time` (default `true` for the Plane scene). The arithmetic lives in the header; `Scene` only holds the settings and calls it.

**The comet already exists — this task does not build one.** `SceneLayers::vehicle` is documented as "the followed-citizen head + comet tail" ([scene.h:59](../../../desktop/src/scene3d/scene.h#L59)), registered as *"where is the followed thread right now?"* ([layers.cpp:44](../../../desktop/src/scene3d/layers.cpp#L44)), and `render()` already locates the head via `find_head()` and feeds the shader `uHeadY` ([scene.cpp:1436](../../../desktop/src/scene3d/scene.cpp#L1436), `:1466-1502`). What this task does is **remove trace time from Y** and repair the three consumers that read time *through* Y once it is gone:

| Consumer | Today | After |
|---|---|---|
| `head_y = follow_step * traj_scale_` (`:1470`) | the vehicle head's Y | the head is found by **step**, not height — `find_head` already returns a position; the uniform becomes the window from `comet_window` |
| `tail_half = 3.0f * traj_scale_` (`:1471`) | a ±Y band ≈ 3 steps | collapses to zero when Y is flat — replaced by `comet_tail` **in steps**, which is what it was always approximating |
| `time_cut_y = slice_step * traj_scale_` (`:1475`) | the terrain-playhead dimming cut, a *different clock* | becomes a step comparison, not a height one. **The two clocks stay distinct** — this is the spec's explicit non-goal; do not fuse `slice_step` and `follow_step` |

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
    // With flat time OFF the old spatial behaviour is unchanged — the other
    // scenes still spatialise time and must not regress.
    check("spatial time still lifts by trace step",
          traj_vertex_y(999, scale, false) > 0.0f,
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
// The other scenes still spatialise time and keep the lift.
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

In `set_trajectories`, emit `traj_vertex_y(pt.t, scale, flat_time)` for every vertex Y (the PC strip, the pick points `pts_pos`, and `pt_pos`) plus the constant terrain lift. In `render()`, replace the three Y-derived quantities per the table above and pass the `comet_window` bounds as a step-space uniform, fading by distance from `hi`. Drop the `draw_trajectory_ruler` call at [shell.cpp:1829](../../../desktop/src/ui/shell.cpp#L1829) for the Plane scene only — a ruler labelling an axis that no longer carries time is worse than no ruler.

- [ ] **Step 4: Run tests to verify they pass**

Run: `make build/desktop_test_scene_traj && ./build/desktop_test_scene_traj`
Then the whole suite, because `pick.cpp` replays vertex order against these positions: `make desktop-test`.
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add desktop/src/scene3d/trajscale.h desktop/src/scene3d/scene.h desktop/src/scene3d/scene.cpp desktop/src/ui/shell.cpp desktop/test/test_scene_traj.cpp
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
  - `Image capture_image(Scene &, const Camera &, const ColorFbo &, const SceneLayers &)`
  - `Image render_plane_scene(Scene &, const ColorFbo &, const Camera &, const Recording &, const SceneLayers &, space::Projection::Layout)`
  - `float image_ink_fraction(const Image &)` and `bool image_blank(const Image &)`
  - `bool scene_exists(const char *dir, const char *name)`
- Produces, in `mk/desktop.mk`: `DESKTOP_GL_TEST_OBJS`.

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
// Body lifted verbatim from test_scene_fbo.cpp:485.
inline bool egl_up_once(std::string *why) { /* ...the existing egl_up body... */ }

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

inline Image capture_image(Scene &scene, const Camera &cam, const ColorFbo &cf,
                           const SceneLayers &layers) {
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
inline Image render_plane_scene(Scene &scene, const ColorFbo &cf,
                                const Camera &cam, const Recording &rec,
                                const SceneLayers &layers,
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
- Modify: `mk/desktop.mk` — four additions, listed in Step 4; a test that is not in a `*_TESTS` list never runs in CI
- Create: three recordings under `tests/golden-asmtrace/` (where `make asmtrace-golden` writes and where `test_scene_fbo` loads its scenes from — **not** `desktop/test/fixtures/`, which holds the hand-committed inputs). A *hand-authored* scene goes in `tests/golden-asmtrace/scenes/` instead, beside the existing `mem-rich-synthetic.asmtrace` — see Step 0.

**Interfaces:**
- Consumes: Task 7a's whole harness, and everything above it.
- Produces: `bool images_distinct(const Image &a, const Image &b, float min_fraction = 0.02f);`

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

Note the render call must enable **both** motif channels, since `opcode` defaults OFF by the convention Task 6 pinned:

```cpp
// test_motif_distinctness.cpp — the acceptance gate for the axis budget.
// <RECORD THE STEP 0 DECISION HERE: exactly what produced each recording,
//  and if any is hand-authored, which channels this test may NOT assert on.>
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
    static const char *kScenes[3] = {"motif-memcpy.asmtrace",
                                     "motif-simd.asmtrace",
                                     "motif-syscalls.asmtrace"};

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

        // Motifs ON means BOTH channels: `crossings` is on by default, but
        // `opcode` defaults OFF (it re-lifts the terrain), so the gate must
        // set it explicitly or it would be testing one channel and claiming two.
        SceneLayers on;
        on.opcode = true;
        on.crossings = true;
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

            // "Optional" has to mean the scene stands without the layers. Two
            // claims, and BOTH matter: the frame is still drawn, and it is
            // still a DIFFERENT frame, which is what makes the channels a
            // channel rather than decoration.
            SceneLayers off;
            off.opcode = false;
            off.crossings = false;
            const Recording rec = load_scene(kScenes[0]);
            const Image bare =
                render_plane_scene(scene, cf, cam, rec, off, kAtlas);
            check("the scene still renders with both motif channels off",
                  !image_blank(bare),
                  "turning the optional layers off emptied the viewport, at "
                  "ink fraction " + std::to_string(image_ink_fraction(bare)));
            check("the motif channels actually change the picture",
                  images_distinct(shots[0], bare),
                  "switching both channels on changed nothing — they are "
                  "drawing no information");
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

Per the Step 0 decision. Commit them under `tests/golden-asmtrace/` (a hand-authored one goes in `tests/golden-asmtrace/scenes/` instead, beside `mem-rich-synthetic.asmtrace`, and the path in `kScenes` changes to match), each small enough to keep the test under a second, and record in the test's header comment exactly what produced each one so a later reader can regenerate them. If they go through `make asmtrace-golden`, note that `asmtrace-golden-check` ([mk/cli.mk:534](../../../mk/cli.mk#L534)) will then hold them byte-stable — which is a benefit, not an obstacle.

- [ ] **Step 6: Run tests to verify they pass — in the container, not on the host**

Run: `make docker-desktop`
Expected: PASS. The host may have no EGL device, in which case the GL lane is not even built (`DESKTOP_GL_MISSING`, [mk/desktop.mk:2311](../../../mk/desktop.mk#L2311)) and a host-only run would report a false green. `Dockerfile.desktop` pins software Mesa + EGL precisely so this lane really renders — so **this gate must be judged from the container run**.

**If the distinctness case fails, stop and report.** That is the design being refuted, not a test to loosen. Do not lower `min_fraction` to make it pass.

- [ ] **Step 7: Commit**

```bash
git add desktop/test/test_motif_distinctness.cpp desktop/test/image_distinct.h tests/golden-asmtrace mk/desktop.mk
git commit -m "desktop(test): gate the motif encoding on pairwise distinctness"
```

---

### Task 8: Flip the default, regenerate goldens, document

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

- [ ] **Step 4: Verify the screenshots are pairwise distinct**

Gate on distinctness, not just non-blankness — the same rule Task 7b encodes.

- [ ] **Step 5: Write the brief**

Add `docs/internal/gui/61-scene-axis-budget.md` following `_conventions.md`, cross-referencing [53](../../internal/gui/53-3d-catalog-build-roadmap.md) to record that the depiction catalog now composes onto the atlas substrate. State plainly that `order` survived as the cell quantisation and that the byte-exact round trip is a Hilbert-only promise — those are the two facts a later reader is most likely to get wrong.

- [ ] **Step 6: Commit**

```bash
git add desktop/src/space/types.h docs/_static/gui docs/internal/gui/61-scene-axis-budget.md
git commit -m "scene3d: make the region atlas the default floor layout"
```

---

## Self-review

**Spec coverage.** Component 1 (atlas) → Tasks 2, 3, 8. Component 2 (time as animation) → Tasks 1, 5. Component 3 (optional motifs) → Task 6 (already shipped — verify only) and Tasks 7a/7b (the harness and the gate). Component 4 (camera fit) → Task 4. The spec's interim-guard requirement — that the clamp land first, on its own — is Task 1, ordered first deliberately.

**Type consistency.** `scene_traj_scale(uint32_t, uint64_t, float)` is defined in Task 1 and reused in Task 5. `Projection::Layout` / `rebuild_layout` are defined in Task 2 and consumed in Tasks 3 and 8. `AtlasRect` and `AtlasNode` are defined once, in Task 2, in cell coordinates, above `struct Projection` in `types.h`. `plane_boundary` / `split_rect` / `atlas_bytes_per_cell` / `atlas_cell` / `atlas_ordinal` are defined once, in Task 2's Interfaces block, and live in `projection.cpp`'s anonymous namespace beside `d2xy`/`domain_shift`. `Camera::fit` and its two pad constants are defined once, in Task 4. `Image` / `gl_context_available` / `render_plane_scene` / `image_ink_fraction` / `image_blank` / `scene_exists` are defined once, in Task 7a's `gl_offscreen.h`; `images_distinct` once, in Task 7b's `image_distinct.h`; and `test_scene_fbo.cpp` is re-pointed at the harness rather than keeping a second copy.

**Where the spec is superseded.** Two of the spec's statements did not survive verification and this plan overrides them; both should be read as corrected here rather than followed as written:

- Component 3's *"it defaults ON, matching the registry's existing all-true convention"* — there is no all-true convention (Task 6). And both channels already exist as layers, so Component 3 requires no new layer at all.
- The measured-facts table's *"all address→plane arithmetic funnels through two helpers … the blast radius of a layout swap is contained"* — true of `locate.h`/`stepplace.h`, which already delegate to `project()`, but `region_cells()` was a third route that walks the mapping directly (Task 2 fixes it) and `1 << order` cell arithmetic is copied deliberately in four places (Task 2's decision keeps all four valid).

**Verified against the tree (2026-08-03), across three passes. Corrections made in this revision:**

| Assumption | Verdict |
|---|---|
| `Projection` lives in `projection.h` | **false** — it is in `space/types.h:30-55`. Tasks 2 and 8 corrected |
| `atlas_cells_per_side()` is a needed new concept | **false** — `1 << order` already is the grid side, read in 13 sources and 10 tests. Dropped; the decision to keep `order` is now stated up front instead of deferred to Task 8 |
| `scene_locate_off(p, region, off, &u, &v) -> bool` | **false** — `Located scene_locate_off(const Projection&, const Recording&, uint64_t)`. Task 3 now probes through `place_address(proj, addr)`, which needs no `Recording` |
| `locate.cpp`/`stepplace.cpp` re-derive Hilbert arithmetic | **false** — both already call `proj.project()`. Task 3 is a regression gate, not a refactor, and says so |
| `region_cells()` is layout-neutral | **false** — it walks `d2xy` directly (`projection.cpp:426`). Task 2 gives it an atlas branch; `test_focus.cpp` guards it |
| `reset()` can be routed through a fresh `fit()` | **false** — `test_camera.cpp:111` pins `reset()`'s radius to the default. Task 4's pad factors are calibrated so the whole-plane fit reproduces 2.2/1.9 exactly |
| The motif layer is new work | **false** — `SceneLayers::opcode` and `SceneLayers::crossings` both ship, the latter already coloured by `crossing_hue(SyscallClass)`. Task 6 no longer adds a bool |
| `SceneLayers` defaults are all-true | **false** — additive layers ON, terrain re-lifts OFF, documented per field. Task 6 pins the real rule |
| The PC comet is new work | **false** — `SceneLayers::vehicle` is "the followed-citizen head + comet tail", with `find_head` and `uHeadY` already wired. Task 5 is now about removing time from Y and repairing the three Y-derived consumers |
| Task 7 needs only an image comparator | **false** — five of its six assumed helpers do not exist. Now split into 7a (the harness, with every helper written out) and 7b (the gate) |
| `draw_trajectory_ruler`'s call site is in `hud.cpp` | **false** — it is `shell.cpp:1829`; the function itself stays in hud.cpp for the scenes that still spatialise time |
| Fixtures belong in `desktop/test/fixtures/` | **partly** — `make asmtrace-golden` writes to `tests/golden-asmtrace/`, which is where `test_scene_fbo` loads scenes from; a *hand-authored* scene goes one level down in `.../scenes/`. Task 7b corrected |
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
| `image_floor_fraction(bare) >= 0.5` was unsound twice: viewport coverage is a camera-framing artefact, and the atlas does not raise the decodable-cell count anyway | Dropped, with the reasoning recorded in 7a's Interfaces block and the honesty note under "the one decision" |

**Remaining known risks, not resolvable on paper.**

1. Task 7b's memcpy recording may have to be hand-authored, because `mem` has no producer (`test_scene_fbo.cpp:20-23`). A hand-tuned input would make the acceptance gate self-confirming. Step 0 forces that decision *before* any code is written; the `simd` and `syscalls` cases are unaffected.
2. Task 5's flat worldline is coplanar with the terrain floor. The lift constant is an eyeball judgement that only the container render can settle — a path that vanishes under a tall cell is the failure mode, and it will not show up in any pure test.
3. Task 2's cell budgets can give a region fewer cells than it has bytes (quantised away, `bytes_per_cell > 1`) **or more** (`bytes_per_cell == 1`, the rect's tail decoding to nothing). Both are honest and both are tested; the plan asserts only the region-level round trip, which is what the atlas can promise. If a caller is later found to depend on byte-exactness under `Atlas`, that is a design finding, not a rounding bug to paper over.
4. The binary split's aspect ratios are *reasonable*, not *optimal* — it always cuts the longer side, but it does not backtrack the way squarify does. A pathological length distribution could still produce a sliver. That is a legibility question only a rendered floor can answer, and Task 8 Step 4 is where it would show up.
