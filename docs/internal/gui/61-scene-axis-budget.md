# The 3D scene's axis budget — an address atlas, and time off the Y axis

> **Sources.** [The axis-budget spec](../../superpowers/specs/2026-08-03-3d-scene-axis-budget-design.md)
> and [its plan](../../superpowers/plans/2026-08-03-3d-scene-axis-budget.md).
> Composes onto the depiction catalog cut by
> [53-3d-catalog-build-roadmap.md](53-3d-catalog-build-roadmap.md): every layer in
> 56/57/58 still rides one substrate, and that substrate's floor is now an atlas.
> Read [_conventions.md](../implementations/_conventions.md) first; D1–D11 live in
> this directory's [README](README.md).
>
> Authored 2026-08-04 against HEAD `d6e85cb1`+. If a cited file:line disagrees
> with the code when you read it, the code wins — re-verify, then fix this doc in
> the same change.
>
> **Status — 11/12 landed.** T1–T6, T7a, T7b, T8, T9, T10. **T7c (the crossings
> channel against a real capture) is BLOCKED** on a producer gap and has its own
> plan: [2026-08-03-serve-session-recording.md](../../superpowers/plans/2026-08-03-serve-session-recording.md).

## What changed, in one paragraph

The plane's three spatial axes were carrying two quantities and a pre-drawn
future. They now carry two decodable ones: the floor is a **region atlas** (each
region a rectangle whose area is proportional to `Region::len`, serpentine
within, labelled in place), and Y is **access density alone** — trace time left
the spatial budget and is read through the playhead. The camera gained a
fit-to-bounds preset, and a growing capture now says so when its floor re-lays.

## The three facts a later reader is most likely to get wrong

**1. `order` survived, and means the same thing under both layouts.**
`1u << proj.order` is the cell grid's side length, read in thirteen sources and
ten tests — most load-bearingly `terrain.cpp`'s `m.w = m.h = 1 << proj.order`.
It stopped being a *Hilbert* concept and became what those call sites already
used it for: the plane's cell quantisation. There is no second grid size and no
`atlas_cells_per_side()`; an early draft invented one and it would have been a
parallel source of truth for a number `order` already carries.

**2. The byte-exact round trip is a HILBERT-only promise.** An atlas cell covers
`bytes_per_cell` bytes and `unproject` returns the first of them, so
`project`→`unproject` is exact only under Hilbert. What the atlas guarantees is
the **region-level** round trip, and that is what its tests assert. Any test
asserting byte-exactness must pin `Layout::Hilbert` explicitly rather than
inherit the struct default — `test_projection.cpp` does this in three places,
and one of them was added only after a review caught the omission.

**3. The in-place labels are PARTIAL BY DESIGN.** A rect below the legibility
threshold carries no text, and the HUD's side-panel legend remains the complete
list. An unlabelled rectangle is not an unnamed region.

## "100 % packed" means owned, not painted

No layout can decode more cells than the domain has bytes, and `order` is the
smallest with `4^order >= total`, so the decodable fraction `total / 4^order`
sits in `(1/4, 1]` under **both** layouts. The atlas does not change it. What it
changes is **where the undecodable cells sit and what they mean**: under Hilbert
they are one connected blob at the tail of the curve belonging to no region;
under the atlas every cell belongs to some region's rectangle and the slack is a
bounded rounding tail inside each rect. The floor became *decodable* — point at a
rectangle and it names a region, at a size proportional to that region's length.
Do not restate this as "the floor is now fully painted".

## The serpentine's resonance — the encoding's real cost

The atlas lays a region's bytes into rows of its rect and reverses odd rows, so
consecutive cells stay adjacent **across the row break**. That is the locality
Hilbert was bought for, kept where it still means something. It is not free, and
two tests measured the bill:

- A stride of **exactly one row width** lands at the *opposite end* of the next
  row (offsets 0 and 64 on a 64-wide rect became cells `(0,0)` and `(63,1)`).
  Row-major would have kept them in the same column; the serpentine trades that
  for the `k`/`k+1` adjacency.
- Consequently a strided access pattern can hop *less* far on the plane than a
  sequential scan — the inverse of the Hilbert behaviour. `test_datalayers`
  asserts "a stride travels further than a scan" **under Hilbert** and the
  weaker, layout-neutral "the two read as different shapes" under the atlas.
- `test_scene_fbo`'s contour block needed a fixture with a real 2D gradient for
  the same reason: its two heated cells, adjacent under Hilbert, land at
  opposite ends under the atlas and are sub-pixel at a far camera.

This is a genuine trade, not a defect: a decodable, labellable, region-major
floor in exchange for Hilbert's scale-free locality.

## Measured properties (61 T10 Step 4)

Over a realistic 12-region `/proc/maps`, a one-huge-plus-twenty-tiny map,
sixteen powers of two, and 400 randomised skewed length distributions:

| property | result |
|---|---|
| exact tiling (every cell in exactly one rect) | **always**, 0 failures in 400 random sets |
| worst aspect ratio, realistic map | 13:1 |
| worst aspect ratio, randomised skew | **4096:1** — slivers are real |
| rects thinner than 2 cells, 400 random sets | 1177 |

**Open risk 5 of the plan is therefore CONFIRMED, not cleared**: the split never
backtracks, and a pathological length distribution does produce slivers. The
tiling and the round trip hold regardless; what a sliver costs is a label.

That measurement is also why the label threshold is a **fraction of the plane's
side, applied to the rect's SHORTER side** (`min_side_frac`, 1/16 by default),
not an absolute cell count. An absolute count is scale-broken — a rect's cell
count grows with `order`, so "64 cells" filters everything at order 6 and
nothing at order 12, where 12 of 12 regions were labelled and the nearest pair
sat **6 px apart at 1600 px wide**. Gating on the minor side takes the realistic
map to 7 of 12 labels at 436 px apart, and the huge-plus-tiny map to the single
rect that can actually hold a word. Area is the wrong measure: a 5×205 rect has
1025 cells and nowhere to put text.

## Time is the playhead, not an axis

`traj_scale_` was never the worldline's private scale — five subsystems place
geometry on it (the line shader, causal spur feet, lifetime pillars, access
arcs, sediment strata), and `scene.h` documents that a spur *hangs on a
worldline vertex at that Y*. The split follows `scene.h`'s own ON/OFF
convention: the default-ON layers (`vehicle`, `crossings`) flatten through one
shared `traj_vertex_y` call, so the spur foot stays welded to the path by
construction; the default-OFF layers (`lifetime`, `data_ribbon`, `sediment`)
keep trace time on Y, because a layer you switch on is asking for it — and two
of the three are vertical-only, so flattening them would be a synonym for
deleting them.

Two consequences the spec did not predict, both found by the GL smoke:

- **A constant lift does not work.** The path spends its length over the hottest
  — therefore tallest — cells, so a small constant buries it (the df tube
  rendered *zero* pixels) and a constant tall enough to clear the highest column
  detaches it everywhere else. The path **drapes**: each vertex sits just clear
  of the terrain surface at its own cell, from the same height field the terrain
  shader displaces by. It is still flat in the sense that matters — its height
  carries no trace time.
- **Flattening collapses coincident pick points.** N visits to one address are
  one point in space, so one id owns that pixel — under `GL_LESS` the earliest,
  forever. The scene re-draws the **followed** vertex with `GL_LEQUAL` so it
  wins its own pixel, which is doc 44's own rule (the followed citizen reuses
  its underlying PC vertex's pick id). The other coincident visits are
  unreachable by click, which is honest: they are not distinct places, and the
  playhead is what selects among them.

## The reflow notice

`layout_fingerprint()` digests what decides which cell a domain offset lands in
(`order`, `domain_off`, the layout, the rects). It deliberately does **not** mix
`Region::base`: two region sets with identical lengths at different bases digest
the same, and that is correct for the question — the picture is identical, only
which address sits under a cell has moved.

The digest lives on `SceneView`'s live-growth preserve-list, beside the camera,
because a live batch *replaces the whole SceneView*. Two rules that a reader
will otherwise get wrong:

- Carrying it is the notice's hinge — drop it and `prev.valid` is false on every
  batch and the notice can never fire, with the rule itself still correct.
- Carry it **only within one capture**. It is per-*recording* state on a
  per-*view* list, so a continuous re-arm that swaps a new recording into the
  slot must drop it; otherwise the new recording's *first* weave reports a
  reflow naming a region count no single recording ever had. `layout_fp_capture`
  is what distinguishes the two, and `test_shell` pins both halves.

Silence is by rule, not by threshold: no note on a first weave, none for a
recording with no regions, none when a recompute reproduced the same layout. The
note reports **that** the floor moved, never how much.

## What is not done

**T7c — the crossings channel against a real capture.** No single `asmspy`
serve engine emits `codeimage` + `trace` + `syscall` together (`trace` gives the
first two, `log` the third), and a two-engine session writes a header per engine,
which `load_recording_file` rejects on line 1. Splicing the two recordings would
be hand-authoring the container the test exists to avoid. The fix is a
session-level `--serve --record=<f>` sink; see
[its plan](../../superpowers/plans/2026-08-03-serve-session-recording.md).
