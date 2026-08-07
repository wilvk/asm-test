# Standalone scenes — a second substrate, and the four that need it

> **Sources.** The [3D catalog](../analysis/2026-07-29-3d-visualization-catalog.md)
> §6 scenes 1 (divergence worldline), 2 (invocation stack), 3 (module excursion
> ribbon) and 4 (SIMD lane prism), and its §7 Phase 3; cut by
> [53-3d-catalog-build-roadmap.md](53-3d-catalog-build-roadmap.md) §4.2 (S1–S4).
> Read [_conventions.md](../implementations/_conventions.md) first; D1–D11 live in
> this directory's [README](README.md).
>
> **Prerequisites.** T1 gates T2–T5 (there is no host for a non-plane scene today).
> T3 needs [54](54-3d-catalog-phase0-plumbing.md) **T6** (the `dt_link` invocation
> field); T4 needs [54](54-3d-catalog-phase0-plumbing.md) **T3** (`Event::seq`). T2
> and T5 have no data prerequisite beyond T1.
>
> Authored 2026-08-02 against HEAD `b657876`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Status — ✅ 5/5, complete (2026-08-03).** T1 landed the second substrate:
> `scene3d/scene_kind.h` (the `SceneKind` discriminant + the required axis
> contract), the outer pick-id band inside 47 T3's own `PickBands`,
> `scene3d/glcommon.{h,cpp}` (the shared program link + R32UI pick target, factored
> out of `scene.cpp`), `scene3d/standalone_gl.{h,cpp}` (the non-plane renderer) and
> the pane/HUD wiring. T2–T5 landed their pure models in
> `scene3d/standalone.{h,cpp}` with `desktop/test/test_scene_kind.cpp` (T1) and
> `desktop/test/test_standalone.cpp` (T2–T5).
>
> **Adopted, not duplicated.** 47 T3's `PickBands` gained a `kind` + `nelem` field
> rather than a parallel allocator; 48 T1's movable `Camera::target`/`frame()` is
> what `standalone_default_camera` frames each scene with. Both were extended in
> place, not forked.
>
> **Deviations, recorded.** (1) T3 needed the codeimage version in force at each
> invocation, which `RegionInvocation` did not carry — `obs_region_build` now folds
> `codeimage` events into its existing seq-ordered walk and reports
> `codeimage_known`/`codeimage_version`, rather than a second walk that could
> disagree with it about invocation boundaries. (2) T4 needed cross-thread call
> order, so `TreeRow` gained `seq` (54 T3's `Event::seq`) — again to keep ONE
> call-tree decode. (3) T4's drill-in carries its tid + module filter in a
> `RibbonDrill` beside the `dt_link`, because `dt_link` has no tid or module field
> and 54 T6 owns that struct's growth. (4) No `glLineWidth(>1.0)` is added anywhere
> (55 T6): a rib's thickness is drawn as stacked strands, and the divergence torn
> cap / ribbon seam are the two draws that would want a wider stroke.

## Why this work exists

Every layer in [56](../archive/gui/56-fidelity-and-module-layers.md), [57](57-causal-layers.md)
and [58](58-memory-data-cell-family.md) shares one substrate: the Hilbert address
plane with a vertical time axis. That is not a limitation to be worked around —
it is what makes the layers compose, because their X and Y stay registered.

Four of the catalog's questions have a load-bearing axis that is **not an
address**, and forcing them onto the plane would be the fabrication the whole
family is written to avoid:

| Scene | Its real axis | Why the plane cannot host it |
|---|---|---|
| Divergence worldline | execution **step** | two recordings, one shared prefix — there is no single address at a step |
| Invocation stack | **invocation index** | a discrete call ordinal, not a time and not an address |
| Module excursion ribbon | **tid** × call order × depth | depth is a stack property; tid is a lane, not a place |
| SIMD lane prism | **byte/element index** | inside one register — there is no address at all |

So this brief's first task is the thing none of the others needed: **a scene host
that does not assume the address plane**. Everything after it is content.

## What already exists (verified 2026-08-02 against `b657876`)

- **`SceneFrame` names exactly one substrate.** `const TerrainModel*`, `const
  TrajectorySet*`, `const ConvergenceSet*`, `const Terrain* slice`, plus `key`,
  `gen`, `slice_t` ([scene_host.h:37-65](../../../desktop/src/ui/scene_host.h#L37)).
  There is no way to hand the host a different model set.
- **`Scene` owns one grid, one height/flags texture pair and one trajectory
  buffer set** ([scene.h:136-196](../../../desktop/src/scene3d/scene.h#L136)), and
  `Scene::render` takes `SceneLayers`, not a scene selector
  ([scene.h:121](../../../desktop/src/scene3d/scene.h#L121)).
- **The abstraction seam is already in the right place, though.** The shell reaches
  GL only through the abstract `SceneHost`
  ([scene_host.h:71-91](../../../desktop/src/ui/scene_host.h#L71)) — `init`,
  `shutdown`, `ready`, `error`, `render(SceneFrame) → ImTextureID`, `pick`. The
  null backend leaves it null and the pane still weaves its pure models and draws
  the HUD ([scene_host.h:1-11](../../../desktop/src/ui/scene_host.h#L1)). Adding a
  scene kind widens `SceneFrame`; it does not breach the seam.
- **The pick id space is contiguous and unbanded**: `0` = background, `[1, n*n]` =
  cells, `[n*n+1, …)` = vertices ([pick.h:27-30](../../../desktop/src/scene3d/pick.h#L27)).
  [47](47-scene-inspect-and-pickable-overlays.md) T3 introduces banding; a second
  scene needs its own band and must not assume the plane's layout.
- **`dt_divergence` / `dt_statediff` land.** `StateDelta{step, changed, computed}`
  ([streams.h:166-170](../../../desktop/src/doc/streams.h#L166)), with
  `computed == false` documented as carrying an **empty** `changed` that *"must
  never be read as 'nothing changed'"* — precisely S1's hollow-rib rule, already
  stated at the model.
- **`ValRec::wide` + `bytes`** are decoded in memory order and are currently used
  only by the 2D views ([streams.h:39-44](../../../desktop/src/doc/streams.h#L39)) —
  T5's whole data requirement, already on the wire.
- **The camera is a plane orbit.** `target[3] = {0.5, 0, 0.5}` — the centre of the
  unit plane ([camera.h:32](../../../desktop/src/scene3d/camera.h#L32)) — with
  `reset()`/`top_down()` restoring it. A scene with different extents needs its own
  framing; [48](48-scene-navigation-and-goto.md) makes `target` movable, which this
  brief should use rather than duplicate.

## Tasks

### T1 — ✅ A scene kind: host a substrate that is not the address plane (L)

**Goal.** `SceneFrame` can describe more than one kind of scene, and `Scene` can
render one, without the plane's assumptions leaking into it or its assumptions
leaking back.

**Steps.**
1. `ui/scene_host.h`: add a `SceneKind` discriminant to `SceneFrame` — `Plane`
   (everything that exists today) plus one value per scene this brief adds — and
   put the per-kind model pointers in a union-like set of optional members rather
   than overloading `terr`/`traj`. **Keep the `Plane` path byte-identical**: the
   default kind is `Plane` and every existing field keeps its meaning, so nothing in
   [56](../archive/gui/56-fidelity-and-module-layers.md)–[58](58-memory-data-cell-family.md) has to
   change.
2. `scene3d/scene.{h,cpp}`: factor the shared machinery out of the plane-specific
   code. Three things are genuinely shared and three are not:
   - **shared** — program compile/link (`link_program`,
     [scene.cpp:65](../../../desktop/src/scene3d/scene.cpp#L65)), the pick FBO and
     its readback (`ensure_pick_fbo` / `render_pick_into_fbo`,
     [scene.cpp:761-795](../../../desktop/src/scene3d/scene.cpp#L761)), the camera
     and MVP;
   - **not shared** — the grid mesh, the height/flags/kind textures, the trajectory
     buffers.
   Pull the first group into a small base or helper; leave the second where it is.
   Do **not** generalise the second group speculatively — a scene whose geometry is
   ribs between two ribbons has nothing to gain from a height texture.
3. **Pick bands, allocated not inferred.** Each scene kind gets its own id band.
   If [47](47-scene-inspect-and-pickable-overlays.md) T3 has landed, use its
   `PickBands`; if not, build the allocator here so 47 can adopt it. An inferred
   band boundary is the aliasing bug both briefs call out.
4. **Axis labelling is part of the host contract, not each scene's choice.** A
   scene declares what each of its axes *is* (`"execution step"`, `"invocation #
   — not time"`, `"byte index"`) and the HUD renders it. This is the two-axes rule
   ([34](../archive/gui/34-playhead-and-scene-reach.md)) made structural: a scene
   cannot ship an unlabelled axis, because the field is required.
5. **One scene at a time.** The pane switches kinds; scenes do not compose. Say so
   — the additive-layer model is a property of the shared plane and does not extend
   to substrates with different axes.
6. The null backend keeps working: with `scene_host == nullptr` the pane draws the
   HUD and the placard exactly as today, for every kind.

**Tests.** `test_shell.cpp` under the null backend: every `SceneKind` produces a
frame with all axes labelled (the exhaustiveness assertion), and the `Plane` kind's
frame is byte-identical to today's. `test_drillin.cpp`: pick bands for two kinds do
not overlap for several sizes, including empty. `test_scene_fbo.cpp`: switching
kinds and switching back reproduces the original image.

**Done when.** A second substrate can be rendered and picked, the plane path is
unchanged, and no scene can have an unlabelled axis.

### T2 — ✅ Divergence worldline (M)

**Goal.** Two recordings of the same routine: where their architectural states
first diverge, and how the disagreement widens.

**Steps.**
1. Vertical axis = **execution step**. A fused tube climbs the shared prefix; at
   `dt_divergence.step` it forks into an A and a B ribbon, region-kind/tid coloured.
   A bright pillar marks patient zero.
2. For each `dt_statediff` step, a horizontal rib between the two ribbons:
   thickness = `changed.size()`, coloured by register class derived from the
   register name.
3. **The admission gate is a refusal card, not a silent empty scene.** A failed
   `code_sha`/basis/arch check renders the refusal with its reason — the same
   gate the diff view already applies.
4. **`dt_divergence.bounded` ends the shared tube in a TORN cap**, never a clean
   proven-identical terminus. "We stopped looking" and "they agreed to the end" are
   different claims.
5. **A bounded or uncomputed step is a HOLLOW rib**, never a zero-width one.
   `StateDelta::computed == false` carries an empty `changed` and the model already
   documents that this *"must never be read as 'nothing changed'"*
   ([streams.h:161-165](../../../desktop/src/doc/streams.h#L161)) — a zero-width rib
   would be exactly that misreading, rendered.
6. Drill-in: fork pillar → the diff view's patient-zero row (`v=diff`, `rec`,
   `rec_b`, `step`); rib → slice explorer or operand timeline at that step.

**Fidelity.** Post-fork ribs are step-indexed **architectural-state** disagreement,
not proof of instruction correspondence after the streams parted — the two
recordings' step *n* need not be the same instruction once they diverge, and the
scene must not imply otherwise. With no statediff, ribs are absent and a note says
so.

**Tests.** A builder test: a bounded divergence produces a torn cap; an uncomputed
step produces a hollow rib distinguishable from an agreeing one; a failed gate
produces the refusal with its reason and no geometry; ribs are absent with an
explicit note when statediff is empty.

**Done when.** Two recordings' divergence is legible and every uncertainty in it is
a visible mark.

### T3 — ✅ Invocation stack (M) · *needs [54](54-3d-catalog-phase0-plumbing.md) T6*

**Goal.** How one routine's control flow varies **across its calls** — where the
region view's pager shows one invocation at a time.

**Steps.**
1. Keep the Hilbert plane's X/Z; swap the vertical axis from trace time to
   **invocation index**. One horizontal block-heat slab per `RegionInvocation` (or
   per `SegmentedDataflow` pass), stacked in call order; per-slab column height and
   opacity = that block's execution count within that one invocation.
2. **The axis is discrete and labelled "invocation #, not time"**, and it is never
   a scrub. This is the axis-labelling contract from T1 doing its job.
3. The finding is the mismatch: a block present in some slabs and absent in others
   pops as a ragged stack. Make absence structural — a missing block is a hole in
   the slab, not a zero-height column.
4. Drill-in: slab cell → the region view at that invocation number and offset,
   using the new `dt_link::invocation` field. **Do not overload `dt_link::step`**,
   which means a dataflow step index ([nav.h:62](../../../desktop/src/nav.h#L62)) —
   that conflation is the trap [46](46-3d-functional-roadmap.md) §4 forbids and the
   reason [54](54-3d-catalog-phase0-plumbing.md) T6 adds a dedicated field.
5. Tint by codeimage version so a slab executed after a JIT rewrite is visibly a
   different code body.
6. **Distinct from a loop helix** — that would count iterations *within* one run's
   loop; this compares control flow *across* calls. Say so in the legend, because
   the two produce similar-looking stacks.

**Fidelity.** An open (`closed == false`) or truncated invocation renders frayed and
is labelled a **prefix**. An undescribed step is an unknown cell, never a zero.
Exact only.

**Tests.** A builder test: three invocations of a routine with a conditional branch
produce slabs whose block sets differ, and the differing block is a hole in the
slabs that lack it; an open invocation is frayed and labelled a prefix;
`dt_link::invocation` round-trips to the region view's pager, and `dt_link::step` is
untouched by this path.

**Done when.** Cross-call variation is visible at a glance and the invocation axis is
never scrubbed as time.

### T4 — ✅ Module excursion ribbon (M) · *needs [54](54-3d-catalog-phase0-plumbing.md) T3*

**Goal.** Which thread is in which library, when — a cross-thread pattern a single
2D icicle cannot show.

**Steps.**
1. **Threads go on the third spatial axis**: one parallel depth-vs-call-order sheet
   per tid. Per lane: X = call order (`Event::seq` — a third axis, never fused with
   either playhead), Y = call depth (the engine's effective focus-rebased depth),
   band colour = the current frame's module.
2. A boundary crossing is the seam where the band colour changes; brighten it. That
   seam is the layer's finding.
3. Source: `TreeRow{tid, depth, addr, name, module}` plus `Event::seq`.
4. **A single-thread recording degrades to a 2D icicle timeline**, not a tilted flat
   chart. One lane in a 3D scene is a worse 2D chart; say so and render the 2D form.
5. Drill-in: segment → the tree at that call, carrying its tid and module filter and
   target symbol.

**Fidelity.** A depth-capped capture marks the lane floor **CAPPED** — a clipped edge,
not a real bottom — and the cap survives the drill-in. Returns are inferred only from
recorded depth decreases, and the legend states that. An unresolved module is an
"unknown" hue, never blank and never merged into a resolved one. No survey data
touches this: it is the exact recorded call tree.

**Tests.** A builder test: two tids produce two lanes with independent depth
profiles; a module transition produces a seam at the right call index; a
depth-capped capture marks the floor capped; a single-tid recording returns the 2D
form; an unresolved module gets the unknown hue rather than being dropped.

**Done when.** Per-thread library residency is readable and truncation is visible on
the axis it truncated.

### T5 — ✅ SIMD lane prism (M)

**Goal.** What happens *inside* a wide vector register over time — byte and element
manipulation no current view looks into, since the loom folds sub-registers into a
64-bit container.

**Steps.**
1. A prism: X = byte/element index, Z = time (stacked writes), Y = byte magnitude,
   colour = a stable value→hue hash. Built entirely from `ValRec::wide` + `bytes` +
   `size` ([streams.h:39-44](../../../desktop/src/doc/streams.h#L39)), decoded today
   and unused in 3D.
2. **The permutation reveals itself through colour continuity**: a recorded byte
   keeps its hue as it moves lanes under a shuffle. That is a faithful rendering of
   recorded bytes, not an inferred mapping.
3. **Drop the element→element def-use thread.** `dt_edge`/`edge_loc` resolve only to
   a whole location — a single register or address — never a byte or a lane. A
   lane→lane thread would fabricate a mapping the recording does not carry. Any hop
   is drawn at **register granularity** (step → step) only.
4. **Element width is not recorded.** `ValRec::size` is the *total* operand width
   ([streams.h:36](../../../desktop/src/doc/streams.h#L36)), so `pshufb` (byte) and
   `paddd` (dword) are indistinguishable from the data. Default to 16 byte-lanes,
   label it **"element width not recorded"**, and subdivide into 32/64-bit only where
   the disassembly mnemonic is unambiguous — reusing
   [54](54-3d-catalog-phase0-plumbing.md) T4's ambiguity flag rather than a second
   guess.
   **SUPERSEDED 2026-08-08** (2026-08-07 `3d-data-capture-gaps` plan, revised Task
   3 — this contract is no longer what ships): a single default-and-label
   conflated "this operand has no element width to know" with "the table does not
   name this mnemonic", disclaiming both as inferred. The shipped form is a
   **three-way** verdict (`PrismWidth` in `standalone.h`): a measured
   `Lanes{1,2,4,8}`; a bulk move (`movdqa`/`movd`/...) with genuinely no element
   width (`NoLaneSemantics`) — the MAJORITY of writes on this doc's own demo
   routine, `scenes_victim.c`'s `blend_tile` — which is a fact, not a guess, and
   gets no disclaimer; and a genuine `Unknown` guess, the only case still
   disclaimed. The geometry fallback is **1** byte-lane
   (`kPrismDefaultLaneBytes`), not 16.
5. `bytes` absent or of odd length → a `[wide]` wireframe, never zero bytes.
   `value_valid == false` → hollow.
6. Drill-in: pick → timeline/slice at that step (the exact hex) plus disasm at
   `insn_off`.

**Fidelity.** Colour continuity shows what the bytes did; it never asserts what the
instruction meant. The default lane width is labelled as a default. An uncaptured
buffer is a wireframe, not zeros.

**Tests.** A builder test: a recorded shuffle preserves each byte's hue across the
lane move; an unambiguous `paddd` subdivides to 32-bit lanes and an ambiguous
mnemonic does not; an empty `bytes` renders the wireframe and no zero bars;
`value_valid == false` renders hollow; the "element width not recorded" label is
present whenever the default is used.
**SUPERSEDED 2026-08-08**: there is no single "element width not recorded"
label to test for — `test_standalone.cpp`'s T5 checks instead pin the three
`PrismWidth` verdicts separately, and at the scene level pin that a known lane
width raises neither HUD note, a bulk move raises `no_lane_note` (information,
not a disclaimer), and only a genuine `Unknown` raises `width_note` (the one
that reads as a disclaimer).

**Done when.** A vector register's internal behaviour is inspectable, and every
lane boundary the scene draws is either recorded or labelled as a default.

## Fidelity notes (D7)

- **A new substrate is a new opportunity to imply a correspondence.** Each scene
  here states what its vertical axis is *and what it is not*: T2's is an execution
  step shared by construction only up to the fork; T3's is a discrete ordinal and
  not a time; T4's is call order and not either playhead; T5's is stacked writes,
  not wall clock. T1's required axis label is what makes this checkable rather than
  aspirational.
- **Empty is a claim, and three of these scenes must refuse instead.** T2's failed
  gate is a refusal card; T3's open invocation is a labelled prefix; T4's
  single-thread case renders the honest 2D form. None of them renders an empty 3D
  scene and lets the user infer why.
- **T5 is the sharpest instance of "the recording does not carry this"** — element
  width genuinely is not recorded, and the temptation to infer it from the mnemonic
  in the ambiguous cases is exactly D7.
- Scenes do not compose (T1 step 5). Two substrates with different axes drawn
  together would put two meanings on one screen position.

## Effort and risk

Five tasks: one large (T1), four medium. Risks:

- **T1 is the real work and everything else waits on it.** Resist generalising the
  plane-specific machinery while factoring — the shared set is small (programs, pick
  FBO, camera) and the temptation to make the height texture "generic" produces an
  abstraction that fits nothing.
- **T1 collides with [47](47-scene-inspect-and-pickable-overlays.md) T3 on pick
  bands.** Same overlap [57](57-causal-layers.md) T1 has with
  [50](50-two-way-brushing.md): whichever lands first owns the allocator. Check
  before starting.
- **Camera framing per scene is unsolved here on purpose.** These scenes have
  extents the plane orbit's fixed `target` was not chosen for
  ([camera.h:32](../../../desktop/src/scene3d/camera.h#L32)).
  [48](48-scene-navigation-and-goto.md) makes `target` movable; if it has not
  landed, add a per-scene default framing and adopt 48's when it does — do not build
  a second camera.
