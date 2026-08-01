# The faithful city — adopting the unified 3D design as the gui/ roadmap

> **Sources.** [2026-07-30-computer-as-city-3d.md](../analysis/2026-07-30-computer-as-city-3d.md)
> (the unifying design, 44 city elements / 29 mapping rows / 5 phases) and
> [2026-07-29-3d-visualization-catalog.md](../analysis/2026-07-29-3d-visualization-catalog.md)
> (the 14-layer/12-scene catalog the city doc absorbs — the city doc states this
> outright: *"this design unifies that catalog's 14 layers + 12 scenes under one
> metaphor"*). Both are ideation + implementation proposals, not committed
> schedules. This doc does for them what [27](../archive/gui/27-extension-roadmap.md) did for
> the extension family and [38](38-live-feed-completion-roadmap.md) did for the
> live-feed gaps: gives the family a numbered home in `gui/`, corrects two stale
> claims found while re-verifying, and cuts the first per-phase implementation-ready
> brief ([44](../archive/gui/44-faithful-city-phase-a-mvp-terrain-reskin.md)). Read
> [_conventions.md](../implementations/_conventions.md) first; D1–D11 live in this
> directory's [README](README.md).
>
> Authored 2026-07-31 against HEAD `d0c82b0`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Status — roadmap only, no tasks of its own.** Phase A is cut as
> [44](../archive/gui/44-faithful-city-phase-a-mvp-terrain-reskin.md) and has **landed, ✅ 7/7**
> (2026-07-31). Phases B–E are NOT yet cut into implementation-ready briefs
> (see §4) — B is now unblocked, since it depends on A's zoning/atmosphere
> substrate.

## 1. Why the city design is the frame going forward

The 3D catalog (2026-07-29) is a menu of 14 additive layers + 12 standalone
scenes, ranked value-then-effort, with no organizing narrative connecting them —
useful as an exhaustive survey, hard to sequence as a product. The city design
(2026-07-30) re-derives the SAME 33 surviving concepts under one metaphor (land /
districts / buildings / traffic / weather / two clocks) and adds a concrete
implementation plan (§4 of that doc) plus its own 5-phase roadmap (§5). Per its
own §1: *"this one metaphor absorbs the entire 14-layer/12-scene 3D catalog:
confidence terrain = weather, per-module skyline = module towers, working-set
tide = lit windows, boundary curtain = district commutes, module excursion = the
commuter's trips — they stop being a menu of overlays and become systems of one
place."* Both docs share the same fidelity discipline (§3 of the catalog, §6 of
the city doc) and the same verified substrate (Hilbert `space/projection.cpp` +
`space/terrain.cpp` + `space/trajectory.cpp`), so adopting the city framing loses
nothing the catalog stated — it is the catalog's own sequencing answer.

**Going forward, cut per-phase implementation-ready briefs from the city doc's
Roadmap (§5: Phase A–E), not directly from the raw catalog.** A catalog item not
yet named in a phase (the catalog's "Phase 5 — remaining medium layers", e.g. JIT
churn strata, blame convergence forest, dominant-path ridge, computation
character) is folded into whichever city phase its city-doc row lands in — cross-
reference the city doc's mapping table (§2) when in doubt, and if a catalog item
has no corresponding city-doc row, treat it as **not yet triaged into a phase**
and say so in whatever brief picks it up, rather than silently placing it.

## 2. Two corrections found while re-verifying (2026-07-31)

- **The city doc's §8 "bug this surfaced" is ALREADY FIXED, and Phase C's most
  load-bearing item is already half-landed.** The live GL-upload freeze
  (`gl_scene_host.cpp` re-uploading trajectories/convergences only on `f.key`
  change, while `f.key = hash(a.id)` stayed invariant across a live re-weave) was
  fixed in `55fc624` ("re-upload 3D worldlines as a live capture grows"),
  **before** the city doc was authored against a slightly newer HEAD (`49cfeea`)
  — the doc's own citation of `gl_scene_host.cpp:64`/`shell.cpp:896` for the BUG
  was already stale at authoring time; what it actually captured was the fix
  already in place. Confirmed today: `shell.cpp:900` sets
  `f.gen = r.event_count()` and `gl_scene_host.cpp:68` gates the trajectory
  re-upload on `scene_needs_traj_upload(f.key, f.gen, ...)` — the exact `f.gen`
  threading the doc's Phase C prescribes. **Phase C's "the live-growth re-weave
  fix" line item is DONE**; only the doc's own stated residual (*"this fixes the
  GL upload only — the upstream whole-recording decode re-weave is a separate
  scaling follow-up"*) remains open, and it is a perf question, not a fidelity
  bug. Do not re-open or re-implement the upload-key fix.
- **`docs/internal/analysis/2026-07-30-live-3d-trajectory-upload-defect.md`** (the
  standalone writeup the city doc's §8 points at) describes the same
  already-fixed defect. It is a historical record of a real, found-and-fixed bug —
  correctly RESOLVED, not stale — but a reader landing on it cold could mistake it
  for an open item; this doc is the correction to check first.

## 3. The city doc's own roadmap (§5), restated with current status

| Phase | Ships | Status |
|---|---|---|
| **A — MVP city** | The Plat (kind hue), day/night sun on terrain-time, fidelity weather sky, ghost districts in the fog, rubble/scaffold/fog-of-war frag branches, the followed-citizen vehicle + comet tail | **Cut as [44](../archive/gui/44-faithful-city-phase-a-mvp-terrain-reskin.md), ✅ 7/7 — LANDED 2026-07-31.** Pure reskin — no new renderer primitive, no producer change, no schema change. |
| **B — orientation & traffic** | District signage, compaction canals, survey roads with blackspots, errands/deliveries, pickable rendezvous plazas, district commutes, the second exec-step transport | **Not cut — but now unblocked (A landed).** Depends on A landing (shares the zoning/atmosphere substrate); needs `is_return` carried through the `SurveyEdge` decode and a `region_index_at()` helper — verify both are still absent before cutting. |
| **C — the living, scaling city** | Lit windows (occupancy day/night), sediment & patina (age), skyline LOD + entity budget | **Live-growth fix already landed** (§2 above). Windows/sediment/LOD remain uncut. |
| **D — the built environment & civic** | Code lots + module towers (the `space/city.{h,cpp}` `CityModel` + instanced building system), construction cranes, City Hall syscall spurs, watchpoint CCTV mast, control tower register board, boundary-curtain swimlane | **Not cut. Large effort** — this is the first phase needing the new engine-free `space/city.{h,cpp}` aggregation module and true GL instancing (guarded behind an ARB check per the city doc's own note, since `glDrawElementsInstanced`/`glVertexAttribDivisor` are 3.1/3.3 extensions over this app's 3.0/GLSL-130 baseline — see [embedded.h:6-12](../../../desktop/src/scene3d/shaders/embedded.h#L6)). |
| **E — the metropolis & named districts** | The `maps` producer (named library districts), heap/data district, archipelago (one island per process), citizens roster, seasons (pass-scoped weave) | **Not cut. Producer + schema work.** Needs a new `maps` event kind (`recording.cpp`'s `kKnownKinds`, currently 23 kinds, no `maps`) — **re-verify `docs/internal/gui/asmtrace-schema.md`'s freeze status before proposing this**; if Phase-3 freeze (D5) has since passed, an additive kind still likely lands under the append-only rule (precedent: [28](../archive/gui/28-schema-freeze-completion.md) T3), but confirm rather than assume. |

## 4. Sequencing guidance for future briefs in this family

- **Land A in full before starting B.** B's "orientation & traffic" system reuses
  A's zoning/atmosphere GL objects and SceneLayers bools; starting it first would
  either duplicate scaffolding or block on A anyway.
- **D is the big one.** It is the only phase requiring the new `space/city.{h,cpp}`
  CityModel (the "Foundation" system in the city doc's §3) — everything in A–C
  reskins the EXISTING terrain/trajectory renderer, never introducing a new
  instanced-mesh primitive. Do not let D's building system creep into A/B/C's
  scope; the city doc's own phase split already drew this line deliberately
  ("Phase A ships entirely on geometry that already exists") — keep that
  restraint when scoping any future brief here.
- **E is gated on real producer work** (the `maps` snapshot) that no other phase
  needs — it can be developed in parallel with C/D once someone picks it up, but
  needs its own schema-freeze-status check first (§3 above).
- **Cut phases one at a time, like [39](../archive/gui/39-auto-capture-reliability.md)/[40](../archive/gui/40-segment-dataflow-by-invocation.md)/[41](../archive/gui/41-live-blame-statediff-serve-leg.md)
  were cut from [38](38-live-feed-completion-roadmap.md)'s gap table**, not all
  at once like the [18–24](README.md) UX-restructure family or the
  [28–33](README.md) extension family — the city vision is larger than either of
  those (5 phases vs. their 6-7), and each phase brief needs its own fresh
  file:line re-verification pass against whatever HEAD exists when it is cut, the
  same discipline this doc applied for Phase A.

## 5. Cross-references

Extends the existing 3D design
([10-spacetime-3d-overview.md](../archive/gui/10-spacetime-3d-overview.md),
[34-playhead-and-scene-reach.md](../archive/gui/34-playhead-and-scene-reach.md) — the two-clock
/ execution-step-brush precedent Phase A's vehicle reuses) and absorbs
[36](../archive/gui/36-anchor-the-3d-plane.md)/[37](../archive/gui/37-region-tag-on-df-step.md)'s placement work
unchanged (the city's Plat sits on the SAME projection/terrain those docs already
place onto). Fidelity chrome D7 / [23](../archive/gui/23-graded-truth-layer.md); wording D7 /
[24](../archive/gui/24-one-visual-language.md). The live-feed roadmap
([38](38-live-feed-completion-roadmap.md)) and this doc are siblings, not a
dependency in either direction — the city reskins the SAME scene 38's live gaps
feed data into, but neither blocks the other.
