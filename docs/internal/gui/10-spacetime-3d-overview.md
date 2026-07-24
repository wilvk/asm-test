# 3D spacetime overview: memory terrain + execution trajectories — implementation

> **Sources.** Actioned from [desktop-gui-plan.md](../plans/desktop-gui-plan.md)
> and the 2026-07-24 design discussion that reunified the plan's *killed*
> **Terrane** (address-space atlas) and **Observatory** (live-observer plane)
> as a single **overview surface** — one 3D scene the user navigates to *find*
> a pattern, then drills *out of* into the flat 2D views (the Loom, trace
> canvas, slice explorer) to *read* it. Written 2026-07-24. If this doc and a
> source disagree, this doc wins (sources may be stale); if the CODE and this
> doc disagree, re-verify before implementing.
>
> Read [\_conventions.md](../implementations/_conventions.md) first; shared
> decisions D1–D11 live in this directory's [README](README.md). Siblings:
> [01-asmtrace-format.md](01-asmtrace-format.md) (event kinds — this view is a
> pure consumer of `trace`/`coverage`/`survey`/`mem`/`codeimage` events),
> [03-desktop-shell.md](03-desktop-shell.md) (GLFW+GL3 context, Workspace,
> pinned-dep pattern), [04-replay-views.md](04-replay-views.md) (the deep-link
> router this view navigates through, and the per-offset heat model it reuses),
> [07-serve-live-host.md](07-serve-live-host.md) (`LiveSession`, the live-
> observer feed), [08-observer-views.md](08-observer-views.md) (the `codeimage`
> event kind). Symbols from siblings are marked **(new — 0N)**; everything else
> cited exists at HEAD `149f084`.

> **Status: growth-rung companion, NOT scheduled against Phase 2–4.** This view
> is an *overview*, not a reading surface — see [The one rule](#the-one-rule-3d-to-find-2d-to-read).
> Its **coarse rung** (region-level terrain + PC trajectory on absolute-basis
> traces + per-thread activity) is buildable once 01/03/04/07/08 land; its
> **rich rung** (per-access data-address density and per-access trajectories)
> is gated on the Wave-1 `mem[]` address stream ([01](01-asmtrace-format.md)
> reserved kind, no producer yet). Nothing here blocks the nine core docs, and
> the plan's Phase 1–4 does not depend on it.

## Why this work exists

Tracing, live observer, and memory are three readings of one execution that
share two axes — **the virtual address space** and **time**. A trace is a walk
through *code* addresses over time; a memory access stream is a walk through
*data* addresses over time; live attach is *several* such walks at once. That
shared-axis structure is the one condition under which a single 3D scene beats
three flat panels: you stop context-switching between "the trace", "the memory
map", and "the thread timeline" because they are the same trajectory-over-terrain
seen three ways.

The scene: the address space is a horizontal **plane** (code and data regions
placed by address through a locality-preserving Hilbert curve); **time is the
vertical axis**; **access density is terrain height**; and execution is a
**trajectory** threading up through the plane — the program counter weaving
through code regions while each access it makes marks its data address at the
same height. Live observer adds *N* colored trajectories over the one shared
terrain, so two threads converging on the same cache line, or one reaching into
another's stack, is a visible crossing.

This answers overview questions no list or timeline does — where is the working
set, how fragmented is it, is locality tight or scattered, does a JIT region
churn, do threads share or contend — for the reverse-engineer/security and perf
personas. It is deliberately **not** for the classroom (the teaching views stay
2D and reproducible).

## The one rule: 3D to find, 2D to read

3D is tolerable for *overview / gestalt / anomaly-spotting*, where you read
approximate shape and occlusion is acceptable; it is bad for *precise per-item
reading* (which value, same-thread-or-not, exact step), which is every core
interaction's real task. Therefore this view is **strictly an orientation
surface**: every pick (a region, a trajectory vertex, a terrain cell) routes
through [04](04-replay-views.md)'s deep-link router into the flat 2D views that
do the reading. Two honesty invariants ride along and are **tested** (T6):

1. **Truncation survives the drill-in.** A terrain tear or a clipped trajectory
   can be occluded in 3D, so the loud-truncation signal must also appear in the
   2D view the pick opens — never only in the 3D scene.
2. **Statistical is never exact.** An exact PC trajectory (from a `trace`/PT
   recording, `basis:"abs"`) and statistical residency (from an `ibs-op`
   `survey`) are **different marks** (solid tube vs translucent stipple) and
   labelled; a sampled terrain never renders as an exact path.

## What already exists (verified 2026-07-24)

- **GL context + pinned-dep pattern** — [03](03-desktop-shell.md) stands up a
  GLFW + OpenGL 3 context for both binaries and vendors deps via
  `scripts/fetch-*.sh` mirroring
  [scripts/fetch-dynamorio.sh](../../../scripts/fetch-dynamorio.sh) (digest row
  in [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt),
  license under [licenses/](../../../licenses/)). This view draws raw GL under
  the ImGui HUD in that same context — no new windowing.
- **Event feeds (pure consumer, zero engines)** — [01](01-asmtrace-format.md)
  defines `trace` (`{"basis":"rel"|"abs","off":u64,...}`,
  [01:125](01-asmtrace-format.md)), `coverage`, `survey`
  (`sampler:"ibs-op"|"sw-clock"`, always `exact:false`,
  [01:134](01-asmtrace-format.md)), and the **reserved** `mem` kind
  ([01:148](01-asmtrace-format.md), fields unfrozen — this doc proposes the
  shape it needs, T2). [08](08-observer-views.md) T7 adds the `codeimage` kind
  (`{"base","len","version","when","bytes"}`). All are read through
  [03](03-desktop-shell.md)'s Recording model.
- **Region geometry** — [include/asmtest_codeimage.h](../../../include/asmtest_codeimage.h):
  `asmtest_codeimage_track(img, base, len, ...)`
  ([:90](../../../include/asmtest_codeimage.h#L90)) is the producer whose
  base/len become terrain code-regions; the live host records them as
  `codeimage` events (08-T7). `/proc/pid/maps` is read client-side by
  [07](07-serve-live-host.md)'s host for data regions.
- **Per-offset heat** — [04](04-replay-views.md) T3 already computes
  per-offset execution counts from the ordered `trace` insn stream
  ([04:210](04-replay-views.md)); T2 here reuses that model as the coarse
  terrain height.
- **Deep-link router** — [04](04-replay-views.md) T2 (**new — 04**), the single
  navigation API `open(recording, step, view)` this view calls on every pick.
- **Live feed** — [07](07-serve-live-host.md) T3's `LiveSession` (**new — 07**)
  streams the same event kinds from an attached process; both binaries may host
  it (no engine linked, D9).
- **Nothing 3D exists** — no scene graph, camera, or GL mesh code in the tree;
  greenfield. Dear ImGui is 2D immediate-mode; this view adds a small GL layer
  beneath it.

## Data model (shared by all tasks)

```cpp
// desktop/src/space/types.h  (new; pure, no GL, no engines)
namespace asmdesk::space {

// A contiguous region of the virtual address space, from a codeimage event
// (code) or a /proc/maps snapshot (data/stack/heap/mmap).
struct Region {
  uint64_t base, len;
  enum Kind { Code, Stack, Heap, Data, Mmap, Unknown } kind;
  std::string label;          // "libc .text", "[stack:tid7]", "jit#3", ...
  uint64_t version = 0;       // codeimage version (JIT churn); 0 for static
};

// The projection: address space -> unit plane [0,1]^2 via a Hilbert curve over
// a compacted address domain (regions are packed to kill the sparse gaps that
// make a raw address axis useless — see T1).
struct Projection {
  std::vector<Region> regions;      // sorted, non-overlapping, compacted
  uint32_t order;                   // Hilbert order n: plane is 2^n x 2^n cells
  // maps an absolute address to a plane cell (or {-1,-1} if unmapped)
  bool project(uint64_t addr, float* u, float* v) const;
  // inverse, for picking: a plane cell -> the address (region + offset) it holds
  bool unproject(float u, float v, uint64_t* addr, const Region** r) const;
};

// One trajectory sample: the PC (or a data access) at a logical time.
struct TrajPoint {
  uint64_t t;                 // trace step index (time = vertical axis)
  uint64_t addr;             // absolute address (basis:"abs") or region-relative
  enum { Exact, Statistical } fidelity;   // trace/PT vs ibs survey
  bool is_access = false;    // false = PC vertex; true = a data-access mark
  int32_t tid = -1;          // -1 = single-trajectory replay; else per-thread
};

struct Terrain {            // height field over the projection's cells
  uint32_t w, h;            // = 2^order each
  std::vector<float> height;// [w*h], access density (log-scaled) at a time slice
  std::vector<uint32_t> flags; // per-cell: TORN (truncated), STAT (sampled only)
};

} // namespace asmdesk::space
```

## Tasks

### T1 — Address-space projection: Hilbert curve + region compaction  (M, depends on: 03)

**Goal.** `desktop/src/space/projection.{h,cpp}` (**new**, pure/headless): turn
a set of `Region`s into a `Projection` that maps any address to a plane cell and
back, packing regions so the sparse gaps of a raw 64-bit axis do not waste the
plane.

**Steps.**
1. **Compaction.** Sort regions by `base`; assign each a contiguous slot in a
   *compacted domain* `[0, sum(len))`, preserving order (so neighbours in memory
   stay neighbours in the domain). Keep a `std::vector<std::pair<uint64_t
   domain_off, Region*>>` for O(log n) address↔domain lookup. Round the total
   domain up to the next power of four and pick `order = ceil(log4(domain))`
   (clamped to `[6,12]` → a 64×64 … 4096×4096 plane).
2. **Hilbert mapping.** Map a compacted domain index `d ∈ [0, 4^order)` to plane
   cell `(x,y)` with the standard iterative Hilbert `d2xy`, and the inverse
   `xy2d` for picking:

```cpp
// n = 2^order (cells per side). Public-domain algorithm (Wikipedia "Hilbert
// curve"); include a one-line attribution comment, no external dep.
static void d2xy(uint32_t n, uint64_t d, uint32_t* x, uint32_t* y) {
  uint32_t rx, ry; uint64_t t = d; *x = *y = 0;
  for (uint32_t s = 1; s < n; s <<= 1) {
    rx = 1u & (t / 2); ry = 1u & (t ^ rx);
    if (ry == 0) { if (rx == 1) { *x = s-1-*x; *y = s-1-*y; } std::swap(*x,*y); }
    *x += s * rx; *y += s * ry; t /= 4;
  }
}
static uint64_t xy2d(uint32_t n, uint32_t x, uint32_t y) {
  uint64_t d = 0; uint32_t rx, ry;
  for (uint32_t s = n/2; s > 0; s >>= 1) {
    rx = (x & s) > 0; ry = (y & s) > 0; d += (uint64_t)s * s * ((3*rx) ^ ry);
    // rotate
    if (ry == 0) { if (rx == 1) { x = s-1-x; y = s-1-y; } std::swap(x,y); }
  }
  return d;
}
```

3. `project(addr)`: address → region (binary search) → domain index → `d2xy` →
   `(u,v) = ((x+0.5)/n, (y+0.5)/n)`. `unproject(u,v)`: `(x,y)` → `xy2d` → domain
   index → region+offset → address.
4. Region **kind** colour/label are carried through for the HUD legend.

**Code.** Signatures as in the data model; no GL, no engine, no ImGui — this TU
compiles into **both** binaries and the null test harness.

**Tests.** `desktop/test/test_projection.cpp` (runs under 03's null harness, no
display): round-trip `project`∘`unproject` for 10k random addresses across three
regions is exact; two addresses 1 byte apart land in the same or 4-neighbour
cells (locality); an unmapped address returns false. `make desktop-test`.

**Docs.** A "Projection" section in `desktop/README.md` (03's file, append).

**Done when.** round-trip + locality + unmapped tests pass headlessly on x86-64
and arm64 (`make docker-desktop DOCKER_PLATFORM=linux/arm64`).

### T2 — Terrain builder: density height field over time  (M, depends on: T1; 04 heat)

**Goal.** `desktop/src/space/terrain.{h,cpp}` (**new**, pure/headless): build a
`Terrain` for a time slice `[0,t]` from a recording's events.

**Steps.**
1. **Coarse rung (buildable now).** Height cell `(x,y)` = `log1p(sum of
   per-offset heat for the code offsets that project into that cell)`, reusing
   04-T3's per-offset execution counts; data regions get height 0 until (2).
   `codeimage` events populate/label code regions and set `version` (JIT churn:
   a region whose `version` changed within `[0,t]` gets a `flags |= CHURN` bit).
2. **Rich rung (Wave-1 `mem` stream).** This doc **proposes** the reserved
   `mem` kind's consumer shape (reconcile with 01 when its producer lands):
   `{"k":"mem","step":u64,"ea":u64,"size":u32,"rw":"r"|"w"}`. Each `mem` event
   with `step <= t` adds `size` to its data cell's height; `rw` drives a
   read/write tint channel. Absent the stream, data cells stay flat and the HUD
   shows a "coarse: no per-access memory stream" provenance chip — never a
   silent zero.
3. **Time slicing.** Precompute a prefix structure (per-cell cumulative height
   by step, or rebuild on scrub if small) so the playhead scrubs the terrain in
   ≤16 ms for a golden-sized recording. Truncated recordings (an `end` event
   with drops, or a missing `end`) set `flags |= TORN` on affected cells.
4. **Statistical isolation.** `survey`-derived residency, if shown, writes a
   **separate** `Terrain` with every cell `flags |= STAT`; the exact terrain and
   the statistical terrain are distinct layers the renderer never merges.

**Tests.** `test_terrain.cpp` (headless): a hand-built recording of 3 offsets in
2 regions yields the expected non-zero cells; a truncated fixture sets `TORN`; a
`survey`-only fixture produces a `STAT` layer and an empty exact layer (proving
the isolation invariant). `make desktop-test`.

**Done when.** coarse terrain builds from an existing golden recording; the
`mem`-fed path is exercised by a synthetic `mem` fixture and gated behind a
`#if` / runtime "kind present?" check so it is inert until the producer lands.

### T3 — Trajectory builder: PC path + access marks  (M, depends on: T1)

**Goal.** `desktop/src/space/trajectory.{h,cpp}` (**new**, pure/headless):
ordered `TrajPoint`s from `trace` events (the PC path) and `mem` events (access
marks), tagged exact/statistical and by tid.

**Steps.**
1. PC vertices from `trace` events in step order; `basis:"abs"` → the address
   projects directly; `basis:"rel"` → offset from the recording's region base,
   with a `RELATIVE_BASIS` flag so the HUD labels "routine-relative — not a true
   address-space path" (honesty: a rel trace is not a real memory trajectory).
   **Refuse to mix bases** in one trajectory (mirrors 04's canvas rule).
2. Access marks from `mem` events (rich rung) attached to the PC vertex of their
   `step` — a short spur from the code trajectory to the data cell.
3. `survey` edges → `fidelity = Statistical`; rendered as translucent stipple,
   never joined into an exact tube.
4. Per-tid grouping for the live overlay (T5): `tid` from the event (`syscall`/
   stitch/PT carry it); replay recordings are `tid = -1` (single trajectory).

**Tests.** `test_trajectory.cpp` (headless): an `abs` fixture yields vertices at
the projected cells in step order; a `rel` fixture sets `RELATIVE_BASIS`; a mixed
fixture is refused with a diagnostic; a `survey` fixture is all `Statistical`.

**Done when.** all four cases pass headlessly; the builder is engine-free and in
both binaries.

### T4 — The GL scene: camera, terrain mesh, trajectory tubes, picking  (L → split; depends on: T1–T3, 03)

> Intermediate difficulty (GL/shaders) — the one non-junior task; the pure
> layers above and the wiring below are junior-friendly. Split across the four
> sub-steps; each is independently testable via FBO readback.

**Goal.** `desktop/src/scene3d/` (**new**): render the `Projection` + `Terrain`
+ `Trajectory` as a 3D scene in 03's GL context, with an orbit camera and
colour-ID picking, ImGui drawn on top as the HUD.

**Steps.**
1. **Math dep (D2).** Add `scripts/fetch-linmath.sh` mirroring
   [03](03-desktop-shell.md)'s `fetch-imgui.sh` for a pinned permissive
   single-header vector/matrix lib (`linmath.h`, public-domain — vendor the
   header's notice as `licenses/linmath-LICENSE.txt`, digest row added). Provides
   `mat4x4_perspective`, `_look_at`, `_mul`. (If the implementer prefers glm/MIT,
   substitute and record it — the rest of this task is lib-agnostic.)
2. **Terrain mesh.** A `2^order × 2^order` grid VBO; per-vertex `height` from the
   `Terrain` in a float texture (`GL_R32F`), sampled in the vertex shader to
   displace Y; per-cell colour from region kind + a `flags` texture (`GL_R32UI`)
   for TORN/STAT/CHURN. GLSL:

```glsl
// terrain.vert
#version 330 core
layout(location=0) in vec2 cell;          // (x,y) in [0,n)
uniform sampler2D uHeight;                 // R32F, n x n
uniform mat4 uMVP; uniform float uYScale; uniform float uN;
out float vHeight; out vec2 vUV;
void main(){
  vUV = (cell + 0.5) / uN;
  float hgt = texture(uHeight, vUV).r;
  vHeight = hgt;
  vec3 p = vec3(vUV.x, hgt * uYScale, vUV.y);   // plane in XZ, height in Y
  gl_Position = uMVP * vec4(p, 1.0);
}
// terrain.frag
#version 330 core
in float vHeight; in vec2 vUV;
uniform usampler2D uFlags;                 // R32UI, n x n
out vec4 frag;
const uint TORN=1u, STAT=2u, CHURN=4u;
void main(){
  uint f = texture(uFlags, vUV).r;
  vec3 base = mix(vec3(0.08,0.10,0.16), vec3(0.9,0.55,0.15), clamp(vHeight,0,1));
  if ((f & CHURN)!=0u) base = mix(base, vec3(0.2,0.7,1.0), 0.5);
  if ((f & STAT)!=0u)  base *= 0.6;        // statistical layer is dimmer
  float torn = ((f & TORN)!=0u) ? 1.0 : 0.0;
  frag = vec4(mix(base, vec3(1.0,0.15,0.15), torn*0.7), 1.0); // torn = red gash
}
```

3. **Trajectory tubes.** Each `Trajectory` → a line strip (position = projected
   `(u, t*uTimeScale, v)`), uploaded as a VBO; exact fidelity → opaque tube
   (a geometry-shader-expanded strip, or a simple `GL_LINE_STRIP` with width),
   statistical → a stippled translucent line (alpha + a dashed pattern via
   `gl_FragCoord`); per-tid colour for the live overlay (T5). Access-mark spurs
   are short lines from the PC vertex down to the data cell top.
4. **Orbit camera + HUD.** `mat4x4_perspective(proj, fovy=0.8, aspect, 0.05,
   50)`; `_look_at` from spherical `(yaw, pitch, radius)` about the plane centre;
   mouse-drag orbits, wheel dollies, a "reset view" and a "top-down (2D-ish)"
   preset (top-down collapses to the classic memory-map heatmap — the honest
   fallback when depth confuses). ImGui HUD: playhead (drives `t` → T2/T3
   rebuild), layer toggles (terrain / exact / statistical / access marks),
   provenance chips (coarse-vs-rich, exact-vs-statistical, truncation), the
   region legend.
5. **Picking (colour-ID FBO).** Render a second pass to an offscreen
   `GL_R32UI` framebuffer where every pickable (terrain cell, trajectory vertex)
   writes a unique id; on click, `glReadPixels` the 1×1 under the cursor →
   resolve id → (region+addr) or (recording, step) → **04's router**. This
   avoids CPU ray-casting and doubles as the headless test hook (read the FBO
   without a visible window).

**Code.** `scene3d/scene.{h,cpp}` (state, GL objects, draw), `scene3d/camera.h`
(pure math, headless-testable), `scene3d/pick.{h,cpp}` (FBO + readback),
`scene3d/shaders/` (the `.vert`/`.frag` above, embedded as string literals so no
runtime file load). Register the TUs and shader embedding in `mk/desktop.mk`
(03's file — additive). The whole scene links **zero engines** → ships in
`desktop-render` too.

**Tests.** `test_scene_fbo.cpp` (gated GL smoke, below): create a **hidden**
GLFW window with software Mesa (`LIBGL_ALWAYS_SOFTWARE=1`), build a scene from a
golden recording, render terrain+trajectory to an FBO, `glReadPixels` and assert
(a) a known hot cell is brighter than a cold cell, (b) a TORN fixture writes red
in its region, (c) the pick FBO returns the right id for a known cell centre.
Camera math tested purely in `test_camera.cpp` under the null harness.

**Docs.** `desktop/README.md` scene section + controls.

**Done when.** the FBO smoke passes under `make docker-desktop` (software Mesa in
`Dockerfile.desktop`); a manual `./build/desktop-render <golden>.asmtrace` shows
the terrain + trajectory and a click opens the 2D view at that step.

### T5 — Live-observer overlay: N trajectories, convergence marks  (M, depends on: T4, 07)

**Goal.** Stream a [07](07-serve-live-host.md) `LiveSession` into the scene as
per-tid trajectories over the shared terrain, growing in real time, with
inter-thread convergence surfaced.

**Steps.**
1. Feed T3's builder incrementally from the `LiveSession` event stream (07-T3);
   each tid is a coloured trajectory; the terrain (T2) updates as `mem`/heat
   events arrive (coarse: from `codeimage` + `survey` residency, labelled
   statistical). Absolute-basis live traces (PT) give real trajectories;
   single-step live traces are region-relative → labelled, not projected as a
   true path.
2. **Convergence marks.** When two tids' recent vertices fall in the same plane
   cell within a sliding step window, draw a bright cross-thread arc between them
   — the "two threads on the same line" signal (a race/sharing hint, explicitly
   labelled *hint*, since ordering is not proven). Pure detection in
   `space/converge.{h,cpp}` (headless-testable), rendering in the scene.
3. Respect the D6 budget via 07's patch-bay: the live overview consumes whatever
   the active session produces; it never itself opens a second ptrace consumer.
4. The affinity arrangement (threads placed by physical core/NUMA) is **out of
   scope** — no scheduling feed exists in-repo; threads are coloured, not
   spatially placed by core. Record this gap in Constraints.

**Tests.** `test_converge.cpp` (headless): two synthetic per-tid trajectories
that touch a shared cell produce exactly one convergence mark; disjoint ones
produce none. A fake-serve fixture (07-T3's `fake_serve.sh` pattern) drives the
incremental builder and asserts trajectory growth.

**Done when.** the convergence detector passes headlessly; a manual live run
against a two-thread victim (`threads_victim`, [cli/](../../../cli/)) shows two
trajectories and a convergence arc; detach leaves the target untouched (07's
guarantee).

### T6 — Drill-in + honesty guarantees  (S, depends on: T4, T5; 04)

**Goal.** Enforce "3D to find, 2D to read" and the two honesty invariants as
tested behaviour.

**Steps.**
1. Every pick (T4 step 5) calls [04](04-replay-views.md)'s router:
   terrain cell → trace canvas / slice explorer at the code offset or the step
   whose access hit that data cell; trajectory vertex → the Loom / operand
   timeline at that step; churned region → the codeimage-versioned disasm pane
   (08-T7).
2. **Truncation-survives-drill-in:** assert that opening a TORN cell's 2D view
   shows 04/08's truncation banner (the 3D tear is never the only signal).
3. **Statistical-never-exact:** assert a `survey`-only recording produces no
   exact trajectory tube and the HUD carries the statistical provenance chip;
   picking a statistical mark opens the hot-edge view (08-T4), never the exact
   slice explorer.

**Tests.** `test_drillin.cpp` (headless, via the pick-id resolution path without
GL): each pickable kind resolves to the expected router call; the TORN and
`survey`-only fixtures assert the two invariants.

**Done when.** all router mappings and both invariants are covered by
`desktop-test`.

### T7 — Golden scenes, gated GL lane, docs, changelog  (M, depends on: T1–T6)

**Goal.** Deterministic fixtures, a CI-runnable GL smoke, and the user/internal
docs.

**Steps.**
1. Commit two golden recordings under `tests/golden-asmtrace/` (regenerated by
   `make asmtrace-golden`, 01-owned — add the two scenes there): a single-routine
   `abs`-basis trace with heat (coarse terrain + one trajectory), and a
   truncated/`survey`-only fixture (exercises TORN + STAT isolation). The rich
   `mem` scene is a **synthetic** fixture (hand-authored `mem` lines) marked
   schema-unfrozen until the Wave-1 producer lands.
2. `Dockerfile.desktop` (03): add software Mesa (`libgl1-mesa-dri`,
   `LIBGL_ALWAYS_SOFTWARE=1` in the lane) so `make docker-desktop` runs the FBO
   smoke (T4/T5) headlessly; the pure-layer tests (T1–T3, T6) already run under
   `desktop-test` with no GL.
3. `mk/desktop.mk`: fold the new TUs, shaders, and the `test_*` binaries into the
   `desktop-test` / `docker-desktop` targets (additive); no new user-visible
   make target (the view lives inside `desktop`/`desktop-render`).
4. Docs: a "3D spacetime overview" section in `desktop/README.md` (controls, the
   coarse-vs-rich staging, the "3D to find, 2D to read" rule); a
   [CHANGELOG.md](../../../CHANGELOG.md) `## [Unreleased]` `Added` line.
5. Update this directory's [README](README.md) status row and the plan pointer
   count when the coarse rung lands.

**Done when.** `make desktop-test` (pure layers) and `make docker-desktop` (FBO
smoke) are green; the two golden scenes render; the synthetic `mem` scene is
inert-but-present.

## Task order & parallelism

`T1 → T2, T3` (parallel, both need only T1) `→ T4` (needs T1–T3) `→ T5, T6`
(parallel) `→ T7`. T1–T3 and T6's logic are pure/headless and junior-friendly
and can be built and tested before any GL exists; T4 is the one intermediate
(GL/shader) task and is the critical path. The whole doc sits **after** the nine
core docs — it consumes 04's router, 07's session, and 08's `codeimage` kind —
and blocks nothing in Phase 1–4.

## Constraints & gates

- **Overview only.** This view never becomes a reading surface; if a task starts
  needing precise in-3D reading, that judgment belongs in a 2D view reached via
  the router (the whole design rests on this).
- **Zero engines.** Projection/terrain/trajectory/scene link no Unicorn/Keystone
  → the view ships in the permissive `desktop-render` binary (D4). Live data
  comes via 07's `asmspy --serve` subprocess (D9), not a linked engine.
- **Feed staging is honest, never faked.** Coarse rung (region terrain + heat +
  absolute-basis PC path + per-thread activity) is the shippable target; the
  rich rung (per-access `mem` height, per-access spurs) is **gated on the Wave-1
  `mem` stream** and must render a provenance chip, never a silent flat plane,
  until it lands. The affinity/core-placement layer is gated on a scheduling
  feed the repo does not have — threads are coloured, not core-placed.
- **Honesty invariants are tested** (T6): truncation survives the drill-in;
  statistical is never rendered or picked as exact.
- **Headless CI** uses software Mesa + FBO readback; a machine with no GL at all
  self-skips the GL smoke with a printed reason (the pure-layer tests still run).
- **Not a classroom view.** The teaching goal stays with the 2D reproducible
  views ([06](06-doors-and-learning.md), [09](09-teaching-producers.md)); this
  surface targets the RE/security and perf personas.

## Out of scope

- A free-camera **reading** mode; 3D graphs (slice DAG, call/hot-edge graphs stay
  2D — the plan's standing rule); the `tier×backend×arch` completeness "cube"
  (faceted 2D, [02](02-exporters-and-readers.md)).
- Physical-**core/NUMA** thread placement and any microarchitecture diagram (no
  cycle-accurate / scheduling feed — the plan's llvm-mca item is a Wave-3 report
  backend, not a producer for this).
- VR / immersive / stereo output; the Loom's fabric becoming 3D (it stays 2D —
  the 2026-07-24 assessment, recorded in the plan's Loom section).
- Producing the Wave-1 `mem` stream (that is 01's reserved kind + the expansion
  wave); this doc only **consumes** it and proposes the consumer shape.
