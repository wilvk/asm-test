# The faithful city, Phase A: the MVP terrain reskin — implementation

> **Sources.** Phase A of [43-faithful-city-roadmap.md](../../gui/43-faithful-city-roadmap.md),
> cut from [2026-07-30-computer-as-city-3d.md](../../analysis/2026-07-30-computer-as-city-3d.md)
> §5 ("Phase A — MVP city") and its "Land & Zoning" / "Weather & the Fidelity
> Atmosphere" / part of "Traffic & Transit" systems (§3), cross-checked against
> [2026-07-29-3d-visualization-catalog.md](../../analysis/2026-07-29-3d-visualization-catalog.md)'s
> Confidence-terrain (#1) and Crossing-spurs-adjacent framing where the two docs
> describe the same underlying data. Read
> [_conventions.md](../../implementations/_conventions.md) first; D1–D11 live in this
> directory's [README](../../gui/README.md). **Prerequisites: none** — every task here reads
> data [10](10-spacetime-3d-overview.md) already builds (`TerrainModel`,
> `TrajectorySet`, `Projection`); this is a pure render-side reskin, no producer or
> schema change, no new engine link.
>
> Authored 2026-07-31 against HEAD `d0c82b0`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change. **One citation already needed correcting from the source docs**:
> the "bug this surfaced" both source docs point at
> (`gl_scene_host.cpp` re-upload freeze) was already fixed in `55fc624` before
> either doc was authored — see [43§2](../../gui/43-faithful-city-roadmap.md#2-two-corrections-found-while-re-verifying-2026-07-31).
> Do not re-touch that fix here.
>
> **Status (2026-07-31) — ✅ 7/7. COMPLETE.** T1 `kind_by_cell` + the zoning
> shader and T2 `TF_UNKNOWN` fog-of-war (`dbca44c`); T3 the fidelity weather sky
> (`scene3d/atmosphere.h`, `scene_atmosphere_for_tier()` sourced only from
> `ui/theme.h`'s shared palette, byte-identical to the 2D banner), T4
> `Scene::set_stat_terrain` (the ghost-district survey surface), T5 the two-clock
> plumbing (`SceneFrame.sun` + an independent `follow_step` `Transport`), T6 the
> followed-citizen vehicle + comet tail, T7 the `zoning`/`weather`/`ghost_fog`/
> `vehicle` `SceneLayers` bools (`88407a8`). Re-validated on merged main
> (`8685a2e`): `docker-desktop` green — `desktop-test` incl.
> `test_terrain`/`test_scene_fbo`, `desktop-engine-boundary-check` (D4 intact),
> `desktop-ui-test` 28/28. No `.asmtrace` schema touched. Full per-task detail in
> the [README](../../gui/README.md)'s faithful-city row.

## Why this work exists

The existing 3D overview ([10](10-spacetime-3d-overview.md)) already draws a
correct, tested, fidelity-honest terrain + trajectory scene — but it reads as an
abstract density field, not a place. The city design's Phase A is the cheapest
possible step toward the "living city" metaphor: it reskins the SAME geometry
(no new renderer primitive, no `space/city.{h,cpp}` module yet — that is
[Phase D](../../gui/43-faithful-city-roadmap.md)) so the plane gains legible zoning by
memory-region kind, a fidelity-driven "weather" sky that makes the recording's
trust level readable at a glance instead of only per-cell, a physically separate
"ghost fog" surface for statistical (IBS survey) residency, and a followed
"citizen" vehicle riding the existing trajectory geometry. Every element reuses
data [10](10-spacetime-3d-overview.md) already computes or is a small, bounded
extension of it (`TerrainModel::CodeCell`/`DataCell`, `region_style()`,
`TerrainModel.stat`, `TrajPoint`) — nothing here waits on a producer change.

## What already exists (verified 2026-07-31)

- **The fidelity flag bits.** `TerrainFlag` (`space/terrain.h:49-55`) defines
  `TF_TORN=1u<<0`, `TF_STAT=1u<<1`, `TF_CHURN=1u<<2`, `TF_READ=1u<<3`,
  `TF_WRITE=1u<<4`. Bits 5+ are free — T2 below claims bit 5 for `TF_UNKNOWN`
  (fog-of-war), matching the city doc's own citation of "TF_UNKNOWN bit 5".
- **The statistical layer is already a separate object in the DATA MODEL.**
  `TerrainModel::has_stat` / `TerrainModel::stat` (`space/terrain.h:83-84`)
  already carry the survey residency as a distinct `Terrain`, per T6's isolation
  invariant. What is missing is the GL SIDE: `Scene` (`scene3d/scene.h`) has no
  `set_stat_terrain` and no second height/flags texture pair — T4 below adds
  only the render half, not new data.
- **`region_style(Region::Kind)`** (`space/projection.cpp:222-238`) already
  returns an `{r,g,b,label}` per kind for exactly the 6 kinds T1 needs
  (`Code`/`Stack`/`Heap`/`Data`/`Mmap`/`Unknown`, `space/types.h:21`) — T1 mirrors
  these six colours into a new shader-side `kindHue[6]`, it does not invent a
  new palette.
- **The terrain texture-upload pattern to clone.** `Scene::set_terrain`
  (`scene3d/scene.cpp:180-214`) uploads two textures per weave: `tex_height_`
  (R32F) and `tex_flags_` (R32UI). T1's `kind_by_cell` upload and T4's
  `set_stat_terrain` both follow this exact shape (gen-if-absent, `GL_NEAREST` +
  `GL_CLAMP_TO_EDGE`, one `glTexImage2D` per weave, never per frame).
  Confirmed: this is a straight clone, not a new pattern.
- **The shader baseline.** `kTerrainVert`/`kTerrainFrag`
  (`scene3d/shaders/embedded.h:20-53`) are `#version 130`, matching the app's GL
  3.0/GLSL-130 baseline (see the file's own banner comment,
  `embedded.h:6-12`, and `main.cpp:110-127` for why: macOS needs a 3.2-core/150
  context GLFW hint, everywhere else gets 3.0/130 — the SHADER SOURCE stays 130
  either way since a 3.2 core context accepts 130 shaders). T1–T3/T4 add uniforms
  and branches to this SAME pair; no new shader stage, no instancing (that is
  [Phase D](../../gui/43-faithful-city-roadmap.md)).
- **`SceneLayers`** (`scene3d/scene.h:30-37`) has exactly 5 bools today:
  `terrain`, `exact`, `statistical`, `access_marks`, `convergence`. T7 adds new
  bools for zoning/weather/vehicle rather than overloading these.
- **The trajectory vertex's time axis is a PER-TID COUNTER, not the flat views'
  execution-step Selection.** `TrajPoint.t` (`space/types.h:52`) is populated by
  `next_t[tid]++` in `build_trajectories` (`space/trajectory.cpp:66-92`, `p.t =
  next_t[tid]++`) — the Nth vertex for that tid, NOT `Selection.step` (a
  dataflow-step index used by the flat 2D views and seeded via
  `playhead_project`, `ui/transport.h:72-78`). **This is a real gap the source
  docs did not surface**: the city doc's vehicle encoding assumes "the placed PC
  vertex where t==exec-step" as if the two axes already coincide; they do not in
  general (a multi-tid recording's per-tid counters diverge from any single
  global step index). T6 below states the resolution this brief adopts — read it
  before assuming the naive `t == Selection.step` comparison is correct.
- **The play/pause precedent to reuse.** `ui/transport.h`'s `Transport` struct +
  `transport_tick()` (`transport.h:31-58`) is already the generic one-playhead
  pure-state type; `draw_scene_overview` (`ui/shell.cpp:718`) already owns one
  instance (`sv.play`) driving `sv.hud.t` (the terrain-time sun axis). T5 adds a
  SECOND, independent `Transport` for the exec-step axis, per the doc34
  anti-fusion rule already enforced at `shell.cpp:777-792`'s own comments.
- **`fidelity_severity`** (`ui/fidelity.h:122`, over `FidelityFacts`,
  `ui/fidelity.h:99`) is already the byte-identical-to-the-2D-banner tier
  computation T3 must reuse for the weather sky — never a second, GL-side
  fidelity judgment.
- **The LOD/degrade precedent.** `should_degrade` (`ui/progress.h:80`, used at
  `shell.cpp:814`) is the existing "flip to a cheaper labelled rung under a cell
  budget" idiom later phases will generalize into `kCityEntityBudget`
  ([43](../../gui/43-faithful-city-roadmap.md) Phase C); Phase A does not need it (its
  per-cell texture sweeps are O(cells), same order as the existing terrain
  upload) but T1's `kind_by_cell` sweep should follow the same "gated, not
  unconditional" spirit if profiling shows it matters at large Hilbert orders —
  not a blocking requirement for T1's Done-when.

## Tasks

### T1 — The Plat: `kind_by_cell` + zoning shader (S–M)

**Goal.** The terrain reads as zoned land, not a uniform amber field: every cell
gets a base hue from the memory-region kind that provably owns it.

**Steps.**
1. `space/terrain.h`: add `std::vector<uint8_t> kind_by_cell;` to `TerrainModel`
   (slice-invariant — built once, not per `slice(t)`, since a cell's owning
   region never changes within one recording).
2. `space/terrain.cpp`'s `build_terrain`: one O(cells) sweep over `proj`,
   `Projection::unproject`-ing (or the per-cell region lookup `terrain.cpp`
   already uses for `regions_from_codeimage`/`kind_by_cell`'s only-existing
   sibling logic — locate the exact existing per-cell-to-region resolution path
   before adding a second one) each in-domain cell to its owning `Region::kind`
   (0–5, `Region::Kind`, `space/types.h:21`); an off-domain / unowned cell gets a
   sentinel (reuse `Region::Unknown`'s numeric value only if that is genuinely
   indistinguishable from "no region data at all" for this task's purposes — if
   not, add a distinct sentinel and say so in the doc comment, since T2's
   fog-of-war pit is a DIFFERENT concept — "no content" not "no owning region").
3. `scene3d/scene.h`/`.cpp`: new `Scene::set_zoning(const std::vector<uint8_t>
   &kind_by_cell, uint32_t w, uint32_t h)` uploading an R8UI texture
   `tex_kind_` — clone `set_terrain`'s texture-creation shape exactly (step
   above). Called ONCE per weave (when `sv.built` flips true), never on scrub —
   it does not vary with `t`.
4. `scene3d/shaders/embedded.h`: extend `kTerrainFrag` with
   `uniform usampler2D uKind;` and a `const vec3 kindHue[6]` mirroring
   `region_style()`'s six RGB triples verbatim (keep the two tables in sync —
   note the duplication and where the source of truth lives, mirroring how
   `kTerrainFrag`'s existing `TORN`/`STAT`/`CHURN` constants already duplicate
   `TerrainFlag`'s C++ values by convention). Blend: `base = mix(kindHue[kind],
   hotAccent, vHeight)` per the city doc's encoding (§2 table, "District /
   zoned neighborhood" row) — kind sets the HUE, height (unchanged) still sets
   brightness/accent, so a code-only recording renders identically to today
   (amber ramp) modulo the kind tint.
5. Wire the new uniform/texture bind alongside the existing `uHeight`/`uFlags`
   binds in `Scene::render`'s terrain draw call (`scene3d/scene.cpp`, the
   `draw_terrain_common` path — locate the exact bind site before adding a
   third).

**Tests.** `desktop/test/test_terrain.cpp`: a fixture with 2+ region kinds
(mirror the existing region-kind fixtures used elsewhere, e.g. `test_projection`
or `test_terrain`'s own multi-region cases) asserts `kind_by_cell[cell] ==`
the expected kind for a cell known to belong to a given region, and that an
off-domain cell carries the sentinel from step 2. No GL test needed for the
shader half beyond the existing `test_scene_fbo` smoke (extend it only if a
visible regression risk is identified — do not add a pixel-exact GL golden for
colour, which this repo does not do elsewhere for terrain hue).

**Done when.** `kind_by_cell` sized `w*h`, byte-stable across `slice(t)` calls
(same weave, different t); a code-only recording (no maps/stack/heap producer,
[43](../../gui/43-faithful-city-roadmap.md) Phase E's gap) renders EXACTLY today's amber
ramp with no visible regression — `test_scene_fbo` still passes unmodified.

### T2 — Fog-of-war: `TF_UNKNOWN` + torn/churn as rubble/scaffold (S)

**Goal.** An in-domain cell with no code/data/statistical content reads as a
distinct sunken "fog-of-war" pit — never a described low cell, never off-domain
void — closing the one genuinely NEW fidelity state Phase A adds (torn/churn
already render today, just not yet "city-styled").

**Steps.**
1. `space/terrain.h`: add `TF_UNKNOWN = 1u << 5` to `TerrainFlag`
   (`space/terrain.h:49-55`).
2. `space/terrain.cpp`: in `TerrainModel::slice`, flag a cell `TF_UNKNOWN` when
   it is in-domain (within `w*h`) but carries neither a `CodeCell`/`DataCell`
   entry nor `TF_STAT` for that slice — "in-domain minus touched cells", per the
   catalog's Confidence-terrain encoding (#1) and the city doc's "Fog-of-war"
   row. Off-domain cells (padding outside `Projection.domain_off`'s packed
   range) stay undrawn/dark water, per both docs' explicit "never drawn as
   land" rule — do not flag those `TF_UNKNOWN` too; verify the exact in-domain
   test this file already uses (it must exist for the grid to size `w,h`
   correctly today) before adding a redundant one.
3. `scene3d/shaders/embedded.h`'s `kTerrainFrag`: add the `UNKNOWN=32u`
   constant; branch a sunken/hatched dark pit distinct from the existing
   `TORN`-red-gash and `STAT`-dim branches (a literal height/brightness
   reduction toward near-black, NOT the same red used for torn — the two must
   stay visually distinct since they mean different things: torn is a KNOWN
   lower bound, unknown is NO content at all). Re-brand (comment-only, unless a
   real visual change is warranted) the existing `TORN` branch as "rubble" and
   `CHURN` as "scaffold" per the city doc's vocabulary — this is a naming/theme
   pass over already-correct logic, not a behavior change to those two flags.

**Tests.** `test_terrain.cpp`: a fixture recording with an in-domain,
never-touched cell (construct one deliberately — most existing fixtures may
have full coverage) asserts that cell carries `TF_UNKNOWN` and no other content
flag; an off-domain cell (if the test harness can construct one) asserts it does
NOT carry `TF_UNKNOWN`. A described-but-empty cell (if such a state is
representable — e.g. a `CodeCell` entry with zero `full_heat`, if that's
possible) must NOT be flagged `TF_UNKNOWN` — unknown-not-zero cuts both ways;
verify this distinction is actually reachable before asserting it, and note in
the test if it is not.

**Done when.** `TF_UNKNOWN` is set only for genuinely undescribed in-domain
cells; a fully-covered recording (the golden corpus's typical case) shows zero
`TF_UNKNOWN` cells and renders unchanged from before this task.

### T3 — The fidelity weather sky (`atmosphere.h`) (M)

**Goal.** The recording's dominant fidelity tier reads as sky/ground condition —
clear for exact, amber overcast for caution, red-dusk for integrity loss — byte-
identical to the 2D fidelity banner, never a second judgment.

**Steps.**
1. New pure header `scene3d/atmosphere.h`: `struct Atmosphere { float
   ambient[3]; float front[3]; float sun_dir[3]; float fog_density; };` — no GL,
   no ImGui (mirrors the existing `space/`-model purity convention; this lives
   under `scene3d/` since it is GL-adjacent config, not a `space/` data model —
   confirm against how `camera.h` is organized, the closest existing precedent
   in this directory, before finalizing the placement).
2. In the shell (`ui/shell.cpp`'s `draw_scene_overview` or a small new helper it
   calls): compute the tier via `fidelity_severity(fidelity_facts_of(rec))`
   (`ui/fidelity.h:122` — locate `fidelity_facts_of` or the equivalent
   FidelityFacts-construction call this view already makes, if any; if
   `draw_scene_overview` does not already build `FidelityFacts` for this
   recording, find the nearest existing call site — e.g. the 2D fidelity banner
   — and reuse its construction, do not hand-roll a second one). Map the tier to
   raw `Atmosphere` floats reading `dt_warn`/`dt_refuse`/`dt_dim_col`
   (`ui/theme.h` — confirm exact symbol names) — theme.h stays OUT of the
   engine-free scene TU (D4-adjacent: `scene3d/scene.cpp` must not gain an
   `ImGui`/`theme.h` include), so this mapping happens in the ImGui-linked shell
   TU, producing plain floats the pure `Atmosphere` struct carries across.
3. Damp the tier transition (~0.5s, per the city doc's implementation note) in
   the shell loop — a one-tier flicker must not strobe the sky; a simple
   lerp-toward-target each frame, gated on `ImGui::GetIO().DeltaTime` (mirrors
   `transport_tick`'s `dt` handling).
4. `Scene::set_atmosphere(const Atmosphere&)` (new, time-stateless — the shell
   passes the already-damped value every frame, the Scene does no damping of its
   own) stores it for the next `render()` call.
5. Shader: a 2-triangle NDC-space sky quad drawn FIRST (before the terrain, at
   the far plane, depth-write off) in `Scene::render`, using
   `uAmbient`/`uFrontColor`/`uSunDir`/`uFogDensity` uniforms from the stored
   `Atmosphere`. New minimal vert/frag pair in `embedded.h` (or fold into the
   existing terrain frag if a full-screen pass is awkward to add as a separate
   draw call — pick whichever is the smaller, more consistent diff against this
   file's existing structure).

**Tests.** A pure test (no GL) asserting the tier-to-`Atmosphere` MAPPING is
correct and byte-identical in COLOR SOURCE to whatever the 2D banner reads
(same `dt_warn`/`dt_refuse` constants, not independently chosen RGB literals) —
this is the load-bearing fidelity invariant (§6 point 9 of the city doc: "weather
is byte-identical to the 2D fidelity verdict"). A GL smoke (extend
`test_scene_fbo`) that the sky quad draws without error under each tier.

**Done when.** Three distinct recordings (exact/caution/integrity-loss fixtures —
locate or construct minimal ones, likely reusing existing low-fidelity golden
fixtures such as `low-fidelity/continuous-df` per [37](37-region-tag-on-df-step.md)'s
own citation of that fixture) produce three visibly distinct, correctly-tiered
skies; the damping means no single-frame flicker crosses tiers within one
sustained recording state.

### T4 — Ghost districts in the fog: `Scene::set_stat_terrain` (M)

**Goal.** IBS survey residency (`TerrainModel.stat`, already computed) becomes a
physically separate, desaturated, stippled GL surface that can never sum into
the exact terrain and does not scrub with the playhead (a survey has no exact
time) — the T6 isolation invariant made geometry, not just data-model.

**Steps.**
1. `scene3d/scene.h`/`.cpp`: `void Scene::set_stat_terrain(const
   space::Terrain &stat)`, guarded by the caller checking
   `TerrainModel::has_stat` first (`space/terrain.h:83`) — an absent survey
   uploads nothing and the second surface simply does not draw. New
   `tex_height_stat_`/`tex_flags_stat_` pair, same upload shape as `set_terrain`
   (clone, per "What already exists" above).
2. New `prog_stat_` shader program reusing `kTerrainVert` (it already needs only
   `vUV` + displaced depth — verify it takes no flags-dependent branch that
   would need duplicating) with a NEW `kStatFrag`: desaturated, translucent,
   STIPPLED (screen-space dither pattern — reuse `kTrajFrag`'s existing stipple
   technique, `embedded.h:67-78`'s `mod(gl_FragCoord.x+gl_FragCoord.y, 8.0)`
   idiom, rather than inventing a second one).
3. Draw AFTER the exact terrain, `GL_BLEND` on, depth-write off, still
   depth-tested (so it renders "a hair above" the exact land per the city doc's
   phrasing, without z-fighting).
4. Call `set_stat_terrain` ONCE per weave (when `sv.built` flips true, alongside
   T1's `set_zoning`) — `TerrainModel.stat` does not vary with the playhead `t`
   (it is a whole-survey aggregate), so it must NOT be re-uploaded on every
   scrub the way `set_terrain` is.

**Tests.** `test_scene_fbo`: a recording with `has_stat=true` renders BOTH an
exact-terrain pick id AND a distinct stat-surface pick id (extend the pick id
space per [43](../../gui/43-faithful-city-roadmap.md)'s Phase B note that arcs/roads etc.
will need new bands too — for T4, a stat-terrain pick id band routing to
`hotedges`, mirroring the catalog's Confidence-terrain drill-in rule); a
recording with `has_stat=false` uploads and draws nothing extra (verify no GL
error, no visible surface).

**Done when.** The stat surface never appears for an exact-only recording; for a
mixed recording it renders as a visually separate, non-scrubbing overlay whose
presence survives a playhead move (`hud.t` change) unchanged, proving it truly
does not key on `t`.

### T5 — The two-clock plumbing: `SceneFrame.sun` + a second `Transport` (M)

**Goal.** Land the data plumbing both the sun (already almost free) and the
vehicle (T6) need, WITHOUT fusing the terrain-time and execution-step axes —
the doc34 D7 trap this codebase already guards against explicitly
(`shell.cpp:777-792`'s comments).

**Steps.**
1. `SceneFrame` (`ui/shell.cpp`, near its `f.key`/`f.gen` fields at
   `shell.cpp:891-901` — locate its full struct definition, likely in a header
   this file includes, before editing): add `float sun = 0.0f;` set from
   `sv.hud.t / max(sv.hud.nsteps, 1)` each frame (guard `nsteps==0` → a fixed
   "noon" constant, per the city doc's own note) — this needs NO `f.key`/`f.gen`
   bump, since it rides the value already recomputed every frame from existing
   state.
2. Add a SECOND, independent `Transport sv.follow_play;` to `SceneView` (locate
   its struct, likely `ui/shell.h` or wherever `SceneView` — used at
   `shell.cpp:722`, `sv.play`, `sv.hud` etc. — is declared) and a
   `uint64_t sv.follow_step = 0;`. Seed it from the shared selection exactly
   like the Scrubber does (`ui/transport.h:72-78`'s `playhead_project`,
   `shell.cpp:1183`'s call site is the pattern to mirror) — **but resolve the
   axis mismatch flagged in "What already exists" above first**: decide and
   document EXPLICITLY whether `follow_step` is seeded against
   `Selection.step`'s NATIVE axis (a dataflow-step index) or against
   `TrajPoint.t` (the per-tid trace-vertex counter), since they are not the same
   space. The safest correct choice for Phase A (revisit in a later phase if it
   proves too limiting): `follow_step` walks `TrajPoint.t`'s OWN axis as an
   independent playhead (like `sv.hud.t` does for terrain-time) — advanced by
   its own `Transport` and, when `Selection.step` names THIS recording, seeded
   by `playhead_project` ONLY when a reliable step→`TrajPoint.t` mapping can be
   established (e.g. the common single-tid case, where the per-tid counter IS a
   dense 0..N-1 sequence and a `Selection.step` that is itself a trace-ordinal —
   verify this holds for the recording kinds this view actually opens before
   relying on it); otherwise `follow_step` free-runs on Play/Pause alone and is
   NOT cross-brushed from the shared selection. State whichever choice is made,
   plainly, in this file's own doc comment — do not leave it implicit.
3. `SceneFrame.follow_step` (new field) carries `sv.follow_step` down to the GL
   host each frame, same shape as `f.slice_t` already does for terrain-time.

**Tests.** A pure test (no GL, mirrors `ui/transport.h`'s own header-only
testability) exercising `transport_tick` against the NEW `sv.follow_play`
instance independently of `sv.play` — advancing one must not advance the other;
a `test_shell.cpp` case asserting `SceneFrame.sun` tracks `hud.t/nsteps`
correctly at `t=0`, `t=nsteps/2`, `t=nsteps` (including the `nsteps==0` guard).

**Done when.** Both playheads advance independently under Play; `sun` is
provably a pure function of existing `HudState` fields (no new persisted UI
state duplicating it); the axis-mismatch decision from step 2 is written down
where a future Phase-B implementer will find it before extending this code.

### T6 — The followed citizen: vehicle glyph + comet tail (M)

**Goal.** A single glowing head glyph rides the placed PC vertex at
`follow_step` (T5), with a short fading tail behind it on the SAME trajectory
geometry already uploaded — no new VBO, no new pick id space beyond what the
underlying vertex already has.

**Steps.**
1. Add a per-vertex `float t_norm` (or reuse the existing vertex position's
   ordinal directly, whichever requires less new attribute plumbing — decide
   after reading `Scene::set_trajectories`'s current vertex-buffer layout,
   `scene3d/scene.cpp`, the function starting at line 239 in the version
   verified here) to the trajectory VBO IF the vehicle needs per-vertex data the
   position alone cannot supply; if the existing per-vertex position plus a
   single `uHeadStep` uniform and a CPU-side "which vertex is at
   `follow_step`" lookup suffices (likely, since `TrajPoint.t` is already
   monotonic per tid), prefer that — it needs no VBO schema change at all,
   only a uniform and a draw-time lookup.
2. Render the head as a `GL_PROGRAM_POINT_SIZE` point positioned at that
   vertex's existing world coordinate, tid-coloured (reuse whatever per-`Line`
   colour `traj_lines_` already carries, `scene3d/scene.h:104-113`). The comet
   tail is a short contiguous sub-range of the SAME `GL_LINE_STRIP`/`Line`
   drawn brighter/wider than the rest — a shader branch on distance-from-head
   (`abs(vertex_t_norm - uHeadStep_norm) < uTailWidth`) rather than a second
   geometry buffer.
3. `!placed` vertices (per `TrajPoint`'s existing placement flag — locate its
   exact name, referenced in "What already exists" as part of the placement
   provenance work) must NOT host a vehicle — per the city doc's own rule
   ("only a PLACED exact vertex gets a vehicle; `!placed` = hollow ghost at map
   edge"). If `follow_step`'s resolved vertex is unplaced, draw nothing (or the
   stated hollow-ghost-at-edge form) rather than snapping to a wrong location.

**Tests.** `test_scene_fbo` (or a new focused GL smoke): at `follow_step`
matching a known placed vertex, the pick buffer at that vertex's screen
position returns the SAME pick id the underlying PC vertex already carries (no
new id space introduced, per the city doc's "the vehicle reuses its underlying
PC vertex id" rule) — this is the one hard behavioral assertion available
without a pixel-value golden.

**Done when.** The vehicle tracks `follow_step` visibly across a Play run
(manual/visual check acceptable here, matching how this codebase treats other
GL-visual-only correctness — e.g. `test_scene_fbo`'s existing "GL smoke ran"
level of assertion); an unplaced target step draws no vehicle, never a
mis-snapped one.

### T7 — The "city" `SceneLayers` preset + wiring (S)

**Goal.** One coherent default turns every Phase-A system on together, and each
new buffer is independently toggleable without breaking the existing 5 bools.

**Steps.**
1. `scene3d/scene.h`'s `SceneLayers`: add `bool zoning = true;` (T1),
   `bool weather = true;` (T3), `bool ghost_fog = true;` (T4, renames/aliases
   the existing `statistical` bool's INTENT for the terrain surface
   specifically if `statistical` today only gates the trajectory stipple —
   verify whether `statistical` already covers what T4 needs or whether a
   genuinely new bool is warranted before adding a redundant one),
   `bool vehicle = true;` (T6).
2. Wire each new bool through `Scene::render`'s existing layer-gated draw calls
   (mirror how `layers.terrain`/`layers.exact` already gate their draws) and
   through the HUD's toggle checkboxes (`scene3d/hud.cpp`, wherever
   `SceneLayers` fields are currently exposed as checkboxes — extend that same
   list).
3. No literal "preset" mechanism needs inventing if the struct's default-member
   initializers (`= true`) already make "city" the out-of-the-box default,
   matching how the existing 5 bools default to true today — confirm this
   reading is consistent with the doc's intent ("defaulting to a coherent
   'city' preset") before adding a separate named-preset system, which would be
   new surface Phase A does not need.

**Tests.** `test_shell.cpp` or `test_terrain`/`test_scene_fbo`-adjacent: each
new bool independently gates its draw (toggling one off leaves the others'
pick ids intact) — mirror whatever test pattern already proves this for the
existing 5 `SceneLayers` bools.

**Done when.** All 4 new bools default `true`; toggling any one off changes
only that system's visible output; `make desktop-test` and `make docker-desktop`
both green with the full new city reskin on by default.

## Constraints & gates

- **D4 unchanged.** Every file this brief touches (`space/terrain.{h,cpp}`,
  `scene3d/scene.{h,cpp}`, `scene3d/atmosphere.h`, `scene3d/shaders/embedded.h`)
  is already part of the engine-free closure (`asmtest-viewer` links these
  today) — nothing here may introduce a Unicorn/Keystone/Capstone include.
  Verify via `make desktop-engine-boundary-check` (the D4 gate landed in
  `a86c5eb`) before considering any task done.
- **No schema change.** Nothing here touches `.asmtrace` wire format,
  `asmtrace-schema.md`, or the golden corpus — a passing
  `make asmtrace-golden-check` with ZERO diff is a correctness signal that this
  brief stayed in its lane.
- **Two clocks never fuse (D7/doc34).** `SceneFrame.sun` and
  `SceneFrame.follow_step` (T5) must stay two independently-advancing values;
  a reviewer should specifically check no code path sets one from the other.
- **`theme.h` stays out of the engine-free scene TU.** T3's atmosphere colours
  are computed in the ImGui-linked shell TU and passed as plain floats — verify
  `scene3d/scene.cpp`/`atmosphere.h` gain no new `#include` of anything under
  `ui/` that pulls ImGui in.

## Non-goals / acknowledged limits (Phase A)

- **No `space/city.{h,cpp}` `CityModel`, no buildings, no instancing.** That is
  [Phase D](../../gui/43-faithful-city-roadmap.md) — this brief only reskins the existing
  terrain/trajectory renderer.
- **No `maps` producer, no named districts, no data/heap district.** Region
  kinds are whatever `regions_from_codeimage` already provides (Code-only for
  most recordings today) — a code-only recording is the expected common case
  and must render correctly, not degrade.
- **No roads, no errands, no district-commute crossings, no rendezvous
  pick-ability.** [Phase B](../../gui/43-faithful-city-roadmap.md).
- **No LOD/entity-budget system.** [Phase C](../../gui/43-faithful-city-roadmap.md); T1's
  per-cell sweeps are the same order as the existing terrain upload and need no
  budget gate yet.
- **The `follow_step` ↔ `Selection.step` axis question (T5) is a real open
  design decision, not a solved one** — this brief states the safe default
  (an independent playhead, cross-brushed only where the mapping is verified
  sound) rather than asserting the source docs' simpler "t==exec-step" framing,
  which this investigation found does not hold in general for multi-tid
  recordings. A future phase may need a real `Selection.step` → `TrajPoint.t`
  resolver; this brief does not attempt one.

## Cross-references

[43-faithful-city-roadmap.md](../../gui/43-faithful-city-roadmap.md) (the family this cuts
from); [10-spacetime-3d-overview.md](10-spacetime-3d-overview.md) (the substrate,
unchanged); [34-playhead-and-scene-reach.md](34-playhead-and-scene-reach.md) (the
two-clock precedent T5 extends); [36](36-anchor-the-3d-plane.md)/[37](37-region-tag-on-df-step.md)
(placement, unchanged by this brief); fidelity chrome D7 /
[23-graded-truth-layer.md](23-graded-truth-layer.md); wording D7 /
[24-one-visual-language.md](24-one-visual-language.md).
