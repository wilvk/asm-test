# The 3D scene's axis budget — a decodable address atlas, time as animation

**Date:** 2026-08-03
**Status:** design approved, ready for implementation planning

## Goal

Re-encode the desktop 3D overview so its three spatial axes carry three
*decodable* quantities instead of one scrambled quantity and one unit collision.

Three deliverables:

1. **An address atlas** replacing the Hilbert curve as the floor layout —
   region-major, 100 % packed, labelled in place.
2. **Time leaves the spatial budget** — the playhead animates the scene; the
   worldline stops being a vertical spaghetti and becomes a scrubbable PC comet.
3. **An optional motif layer** — opcode class and syscall-crossing family, the
   semantic channels that let a reader recognise *what the program is doing*.
   Off-switchable; the scene must be correct and readable without it.

## Why this exists

The current scene spends its axes like this:

| Axis | Carries | Decodable by eye? |
|---|---|---|
| X + Z | address — **one** variable, via a space-filling curve | no — must pick to decode |
| Y | access density **and** trace step — **two** variables, different units | no — units incommensurable |

Address is a one-dimensional quantity. Spending two axes on it via a Hilbert
curve is what leaves only one axis for everything else, which is why density and
trace time collide on Y. The HUD states the collision outright —
*"two vertical meanings share this screen axis: terrain height = access density
(log), path height = trace time (steps)"*
([hud.cpp:298-299](../../../desktop/src/scene3d/hud.cpp#L298-L299)) — but a label
cannot fix an axis budget. Every one of the 14 catalogued additive layers
inherits both defects.

This spec is therefore **upstream of** the depiction family
([53](../../internal/gui/53-3d-catalog-build-roadmap.md) → 54–59), which composes
layers onto this substrate and assumes it sound. Nothing here invalidates that
catalog; it repairs the coordinate system all of it stands on.

## Measured facts this design rests on

Verified against HEAD on 2026-08-03, before the spec was written. Four of them
changed the design.

| Fact | Where | Consequence |
|---|---|---|
| Hilbert order is the smallest in `[6,12]` with `4^order >= total` | [projection.cpp:99-101](../../../desktop/src/space/projection.cpp#L99-L101) | floor occupancy is `total / 4^order` — anywhere in **(25 %, 100 %]**. One byte over a power-of-4 boundary quadruples the plane. This is the "only 1/4 of the floor" report, and it is structural, not a tuning error |
| The camera is hard-pinned to the whole unit plane | [camera.h:29-32](../../../desktop/src/scene3d/camera.h#L29-L32) — `target {0.5,0,0.5}`, `radius 2.2`; `reset()` is `*this = Camera{}` | there is **no** fit-to-content anywhere, so the empty floor is always on screen. Fixing occupancy alone would not fix framing |
| Worldline height is `pt.t * (0.6f / nsteps)`, unclamped | [scene.cpp:800-801](../../../desktop/src/scene3d/scene.cpp#L800-L801), [scene.cpp:825](../../../desktop/src/scene3d/scene.cpp#L825) | intended to top out at 0.6 world units |
| `nsteps` is the **terrain's** extent; `pt.t` for an access is the **`mem` event's own** step | [terrain.cpp:285](../../../desktop/src/space/terrain.cpp#L285), [terrain.cpp:345](../../../desktop/src/space/terrain.cpp#L345); [trajectory.cpp:263](../../../desktop/src/space/trajectory.cpp#L263) | the two are different axes. Any `mem` step past `nsteps` pushes Y above 0.6 **without bound** — the "yellow line leaves the screen" report |
| `sediment.cpp` already guards this exact mismatch | [sediment.cpp:33-37](../../../desktop/src/space/sediment.cpp#L33-L37) — *"a `mem` stream can outlast the trace's own step count"*, guarded with `max(nsteps, steps.back()+1)` | the mismatch is **known**; `set_trajectories` simply lacks the guard. Precedent exists one directory over |
| `nsteps == 0` falls back to a fixed `0.02f` per step | [scene.cpp:801](../../../desktop/src/scene3d/scene.cpp#L801) | at the golden's 35 560 steps that is **711 world units** — ~300× the camera radius and past `zfar = 50` |
| `TerrainModel::slice(t)` is already O(touched cells), built for 16 ms scrubbing | [terrain.h:9-13](../../../desktop/src/space/terrain.h#L9-L13) | **time-as-animation is already half-built.** The terrain already animates; only the trajectory spatialises time. This is the single biggest reason the design is affordable |
| All address→plane arithmetic funnels through two helpers | `space/locate.h::scene_locate_off` (*"the ONE address route (50 T1)"*) and `space/stepplace.h::place_address` (*"the shared plane arithmetic"*), cited at [crossing.cpp:9-10](../../../desktop/src/views/crossing.cpp#L9-L10) | the blast radius of a layout swap is **contained**. Picking, goto and the layers do not each re-derive `(u,v)` |
| Regions already carry kind and label | [types.h:19-25](../../../desktop/src/space/types.h#L19-L25) — `{Code, Stack, Heap, Data, Mmap, Unknown}` + `label` | the atlas can label its rectangles from data that already exists; no producer change |
| Compaction already drops sparse gaps | [projection.h:23](../../../desktop/src/space/projection.h#L23) | the domain is already dense. The wasted floor is **padding to a power of 4**, not real address sparsity — so a packing layout recovers all of it |
| `SyscallClass` exists, conservative, with an inline name fn for `scene3d/` | [crossing.h](../../../desktop/src/space/crossing.h) `{File,Net,Process,Memory,Signal,Time,Other}`; table at [crossing.cpp:45+](../../../desktop/src/views/crossing.cpp#L45) | the I/O motif channel needs **no new classifier**. The header comment already says scene3d/ draws its legend from these names |
| `CellOpcode` / `OpClass` already built, and abstain on ambiguity | [opcode_terrain.h](../../../desktop/src/space/opcode_terrain.h), [mnemonic.h](../../../desktop/src/space/mnemonic.h) | the opcode motif channel needs no new classifier either |
| `SceneLayers` is an exhaustive-by-test registry | [layers.h](../../../desktop/src/scene3d/layers.h) — *"every SceneLayers member appears in exactly one row here — test_layers.cpp pins it"* | the motif toggle must add a `LayerDesc` row or a named test fails. This is the mechanism that makes motifs optional |

## Component 1 — the address atlas

A new layout mode on `Projection`, selected by an enum, with `project()` /
`unproject()` keeping their current contract so every existing caller compiles
unchanged.

```
enum class Layout { Hilbert, Atlas };
```

**Layout algorithm — squarified treemap over regions, serpentine within.**

1. Regions are already sorted, non-overlapping and compacted. Give each a
   rectangle whose **area is proportional to `len`** — that is what "address
   space" means, and any other weighting would be a fabricated emphasis (D7).
2. Within a region's rectangle, walk offsets in **serpentine (boustrophedon)
   row order**, so address adjacency is preserved locally — the property Hilbert
   was bought for, retained where it actually carries meaning.
3. The treemap packs to **100 %** by construction. There is no power-of-4
   padding, so the 25 % floor cannot recur.

**Why this is more decodable than Hilbert**: region boundaries become visible
rectangles, so the floor can be **labelled in place** from `Region::label`. A
Hilbert region is a blobby snake with nowhere to put a label. This — not
locality — is the real win.

**Honest cost, recorded so it is not re-litigated:** cross-region address
adjacency is lost. Two regions adjacent in the compacted domain may not be
adjacent on the floor. This is acceptable because (a) compaction had already
destroyed *true* address adjacency, and (b) cross-region adjacency in a compacted
domain is an artefact of packing order, not a property of the program.

**Fidelity:** the atlas re-lays out already-observed regions. It infers nothing,
so it is `LayerGrade::Exact` territory and introduces no D7 exposure.

## Component 2 — time leaves the spatial budget

### What already animates today, and what does not

Verified, because it decides how much of this component is new work:

- **The terrain animates.** `render()` re-uploads the slice whenever
  `f.slice_t != up_t_` ([gl_scene_host.cpp:148-150](../../../desktop/src/ui/gl_scene_host.cpp#L148-L150)),
  and `TerrainModel::slice(t)` is O(touched cells) precisely so scrubbing stays
  under 16 ms. Buildings grow as the playhead moves. There is a Play control.
- **A live capture grows.** Trajectory upload is gated on `(key, gen)`, not
  identity alone — the comment records why: *"Gating the worldlines on identity
  alone froze a growing capture's trajectories after batch 1"*
  ([gl_scene_host.cpp:82-87](../../../desktop/src/ui/gl_scene_host.cpp#L82-L87)).
  That freeze was a real defect and is fixed.
- **The worldline does *not* animate.** It is uploaded once as a complete static
  object spanning the whole recording, and `render()` merely *dims* it past
  `slice_step` rather than withholding it. So the program's entire future path is
  always on screen as geometry.

So the honest answer to "does it change as the process changes" is **yes for the
terrain and for live growth, no for the path**. That asymmetry is the problem:
the future is pre-drawn, which is both the source of the runaway Y and the reason
watching it builds no sense of sequence. Replacing the pre-drawn line with a
comet is what makes the scene actually depict *progression* rather than a
finished object with a dimming mask over it.

**Y carries access density, and nothing else.**

The worldline stops being a vertical line and becomes:

- a **PC comet** — a marker at the current playhead's `(u,v)`, with a decaying
  trail over the last *N* steps, `N` being a HUD control.
- **scrubbable**: paused, the trail is the recent path and the playhead scrubs
  it. This is the "traceable and replayable once paused or stopped" requirement.

`Scene::time_scale`, `Scene::nsteps` as a Y-scale input, and
`draw_trajectory_ruler` all lose their reason to exist in the Plane scene.

**This deletes the unbounded-Y defect at the root** rather than clamping it: with
no step-to-Y mapping, a `mem` step past `nsteps` has nowhere to escape to.

**Interim guard.** The atlas is the larger change and may land later. The
`sediment.cpp` guard —`max(nsteps, max_t + 1)` — must be applied to
`set_trajectories` **first, as its own commit**, so the live defect is fixed
regardless of when the re-encoding lands. A user hitting this today should not
wait for a substrate rewrite.

## Component 3 — the motif layer (optional)

Two semantic channels, both reusing classifiers that already exist:

- **Opcode class** — per-cell emission colour from `CellOpcode::dominant`,
  abstaining to a neutral tint on `OpClass::Unknown` or low `purity`.
- **Syscall crossing family** — a mark at the crossing site, coloured by
  `SyscallClass`, legend drawn from `syscall_class_name()`.

**Optional, per the explicit instruction.** This is one `SceneLayers` bool with
one `LayerDesc` row, group `Activity`, grade `Derived` (a re-encoding of exact
data, no new claim). **It defaults ON**, matching the registry's existing
all-true convention — "optional" here means *fully toggleable, and the scene is
correct and legible with it off*, which is a hard requirement, not that it starts
hidden. Flipping the default is a one-line change if that reading is wrong.

### What the I/O channel can and cannot say

Recorded verbatim so it is not over-promised in the UI:

| Question | Answerable? |
|---|---|
| Did the program cross into the kernel here? | **yes** — `syscall` is a registered v1 kind |
| Was that crossing file / net / process / memory / signal / time? | **yes** — `SyscallClass`, conservative, unlisted names → grey `Other` |
| Is this territory file-backed? | **yes** — `Region::Mmap` + `label` |
| Did *this load* hit the disk? | **no** — an `mmap`'d read is an ordinary load with no syscall and no event |
| Was that a device or a regular file? | **no** — `ioctl` is bucketed `File`; separating them needs fd backing, which nothing records |
| *Which* file or socket? | **no** — no fd identity is recorded; §9 demoted FD lifelines to 2D |
| Which **CPU core** did this run on? | **no** — the schema carries no `cpu`/`core` field at all. `topo` is a *process* topology snapshot, not a core id. Needs a producer change (perf and IBS samples both carry a cpu field) |
| Is the program using the **GPU**? | **no** — GPU work executes off-CPU and is invisible to a CPU tracer. Only the submitting `ioctl` on a DRM fd is observable, and it is indistinguishable from a file `ioctl` without fd backing |
| Did the **user** interact (key, mouse)? | **no** — input arrives as `read` on `/dev/input/*` or `recvmsg` on the display socket. Both are visible as crossings, neither is identifiable as input without fd backing |

The UI must not imply the "no" rows. A device-vs-file distinction would require a
producer change and is explicitly **out of scope**.

### One producer field gates four of these

Device-vs-file, GPU submission, *which* file/socket, and user input all fail for
the **same** reason: nothing records what an fd points at. A single well-defined
producer change — carrying fd → backing path (or at least a backing *class*:
regular / device / socket / pipe / tty) on the `syscall` kind — unlocks all four
at once, and would let `SyscallClass` grow honest `Device` and `Gpu` members
instead of over-loading `File`.

That change is **out of scope here** and belongs in a producer brief. It is
recorded because it is the highest-leverage single field for this scene's
semantic reach, and because knowing it is one field, not four features, should
shape how it gets prioritised. CPU core is a *separate* second field, cheap on
the sampling tiers (perf/IBS already carry it) and unavailable on the
single-step tiers.

## Component 4 — camera fit-to-content

`reset()` frames the **occupied** atlas rather than the unit square. With a
100 %-packed atlas these coincide, which is the point: the fit becomes a
regression guard rather than a workaround. `top_down()` gets the same treatment.

## Acceptance: the motif-distinctness test

A principled encoding that renders every program identically has failed. So the
acceptance criterion is **pairwise distinguishability**, not merely
non-blankness:

> Render recordings whose behaviour is already known — a memcpy-dominated run, a
> SIMD-heavy run, a syscall-heavy run — and assert the resulting frames are
> pairwise distinct by the same image-distinctness gate the doc screenshots
> already use.

This runs **with the motif layer on**. With it off, the weaker gate (non-blank,
correct geometry) applies — that is what "optional" has to mean.

## Non-goals

- Any producer or schema change. Every quantity used here is already recorded.
- Device-vs-file discrimination (needs fd backing).
- Fusing the two clocks. The terrain-residency playhead and the execution step
  stay distinct; `slice_step` and `follow_step` keep their separate identities.
  **"Unify the playheads" is a known trap** and is not what "time as animation"
  means here — this spec removes step from the *spatial* axis, and does not merge
  the two time axes with each other.
- Re-deriving the depiction catalog. The 14 layers and 12 scenes stand.
- Replacing the 2D readers. "3D to find, 2D to read" is retained: the scene's job
  is to make a phase change unmissable and route the click into a reader that
  says what happened.

## Risks

| Risk | Mitigation |
|---|---|
| Treemap area ∝ `len` makes a large `mmap` dwarf the interesting code region | measure on real goldens first. If confirmed, the honest fix is a **labelled** equal-area mode the user opts into — never a silent reweighting |
| Layout instability across a growing live capture — a new region reflows the treemap and resets the reader's mental map | layout is keyed on the region set; recompute only when that set changes, and record the reflow in the HUD. A growing capture that reflows silently is the failure mode to avoid |
| Golden-image churn across the whole `desktop/` suite | expected and large. Regenerate **once**, from the merged tree, after the last recorder edit — never per-agent |
| The atlas turns out no more legible than Hilbert | the distinctness test is the gate. Prototype on known recordings **before** the substrate swap lands |
