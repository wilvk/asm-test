# The 3D catalog as a build plan — the depiction axis, cut into phases

> **Source.** [2026-07-29-3d-visualization-catalog.md](../analysis/2026-07-29-3d-visualization-catalog.md)
> — 14 additive layers + 12 standalone scenes, each already checked for
> buildability against the real streams, each carrying its own fidelity rules,
> and each ranked value-then-effort with a five-phase build order (§7 there).
> Read [_conventions.md](../implementations/_conventions.md) first; D1–D11 live in
> this directory's [README](README.md).
>
> Authored 2026-08-02 against HEAD `b657876`. Every file:line below was
> re-verified against that tree while writing, and §2 records the four places the
> catalog's claims have MOVED since it was authored against `4c75edb`. If a
> citation disagrees with the code when you implement, the code wins — re-verify,
> then fix this doc in the same change.
>
> **Status — roadmap only, no tasks of its own.** It cuts six briefs:
> [54](54-3d-catalog-phase0-plumbing.md), [55](55-scene-render-quality.md),
> [56](../archive/gui/56-fidelity-and-module-layers.md), [57](57-causal-layers.md),
> [58](58-memory-data-cell-family.md) and [59](59-standalone-scenes.md). All six
> are cut and unclaimed. §7 states exactly what a Phase-4/5 cut would contain and
> what unblocks it.

## 1. Why this family exists

Three families now touch the 3D pane and they are genuinely orthogonal:

| Family | Axis | State |
|---|---|---|
| [43](43-faithful-city-roadmap.md) + 44 | **Representation** — what the scene *depicts* (zoning, weather, districts, towers) | Phase A landed; B–E uncut |
| [46](46-3d-functional-roadmap.md) → 47–52 | **Instrument** — what a person can *do* with the scene (inspect, navigate, brush, focus) | All six cut 2026-08-02, none started |
| **53 → 54–59 (this one)** | **Depiction content** — *which quantities* the scene draws, and what has to exist before it can draw them | This doc |

The distinction between 43 and 53 is the one that needs stating, because 43 says
outright: *"cut per-phase implementation-ready briefs from the city doc's Roadmap
(§5: Phase A–E), not directly from the raw catalog"*, and the catalog's own
closing note agrees. That instruction was right for **Phase A**, which was a pure
reskin of existing geometry, and it stays right for the city's own inventions
(signage, canals, cranes, seasons) which have no catalog row at all. It is the
wrong instrument for the rest, for one concrete reason:

**The city phases are grouped by metaphor; the work is gated by data.** City
Phase B mixes district signage (pure render) with survey roads (needs `is_return`
carried) and rendezvous plazas (needs the pick bands 47 T3 adds). City Phase C
mixes lit windows (needs an observed-data-span projection that does not exist)
with LOD (pure render). A brief cut along a city phase therefore always contains
one item that blocks the other four. The catalog's own §4 already identified the
right seams — **seven shared plumbing changes, each unblocking several graphs** —
and its §7 phases are grouped by *that*, which is why this family follows the
catalog's phase order and maps each row back to the city phase it serves rather
than the other way round.

Nothing here conflicts with 43. Every graph below lands on the same
projection/terrain/trajectory substrate Phase A reskinned, and §6's mapping table
is the join: when a later city phase is cut, its brief takes these rows as its
already-verified content and adds the metaphor.

## 2. What has moved since the catalog was authored

The catalog was written against `4c75edb` and explicitly asks to be re-verified.
Four of its claims have moved, and one of them changes a priority.

- **The data half of the terrain is not merely under-fed — it is structurally
  unreachable.** The catalog frames the observed-data-span projection as the
  blocker for four memory graphs. Verified today it is worse than that: the shell
  builds the plane from **code regions only** —
  `space::build_terrain(space::build_projection(regions_from_codeimage(r)), r)`
  ([shell.cpp:921-924](../../../desktop/src/ui/shell.cpp#L921)) — and the `mem`
  scan drops every access whose `ea` no region maps
  ([terrain.cpp:397-399](../../../desktop/src/space/terrain.cpp#L397): `cell_of`
  fails → `continue`). A heap or stack address is mapped by no region, so in the
  shipped app `TerrainModel::data` is **empty for every recording**, and with it
  `DataCell`, `cum_size`, `cum_rw`, `TF_READ` and `TF_WRITE` — code that exists,
  is tested against hand-built projections, and can never fire from the UI. The
  projection extension is therefore not a Phase-2 enabler; it is the change that
  makes half the existing terrain model reachable at all. [54](54-3d-catalog-phase0-plumbing.md)
  T1 is the highest-leverage single task in this family.
- **The syscall stream still drops `Event::seq`, confirmed.** `SyscallRow` carries
  `{index, line, has_payload, payload, tid}`
  ([syscalls.h:39-45](../../../desktop/src/views/syscalls.h#L39)) — `index` is the
  row's position in the syscall view, not the stream position `Event::seq`
  ([recording.h:74](../../../desktop/src/doc/recording.h#L74)) that lets a syscall
  be interleaved against a trace instruction. Still a real prerequisite.
- **`SurveyEdge` vs `HotEdge`, confirmed and now decidable.** `SurveyEdge` is
  `{from, to, count, mispred}` ([streams.h:142-145](../../../desktop/src/doc/streams.h#L142));
  `HotEdge` is `{from_addr, to_addr, from, to, count, mispred, is_return, rank}`
  ([hotedges.h:40-48](../../../desktop/src/views/hotedges.h#L40)). The catalog says
  "pick one before building both". [54](54-3d-catalog-phase0-plumbing.md) T7 picks
  `HotEdge` and says why.
- **The mnemonic classifier's source is worse-coupled than stated.** The catalog
  asks for the `asm_language.cpp` word-lists to be factored into an "engine-free,
  ImGui-free" header. `ui/asm_language.h` includes **both** `TextEditor.h` (the
  ImGuiColorTextEdit addon, line 26) **and** `asmtest_assemble.h` (an engine
  header, line 29) — so it is excluded from the render-only viewer twice over, not
  once. The factored header must take neither.

## 3. The fidelity frame is inherited, not restated

Every brief in this family obeys the catalog's four invariants (§3 there),
which are the same four 43 (§6 of the city doc) and 46 (§4) hold to:

1. **Statistical is physically distinct** — separate ink, separate object, never
   summed into an exact surface.
2. **Truncation survives the drill-in** — a `TF_TORN` cell frays in 3D *and*
   carries its banner into whichever 2D reader the pick opens.
3. **Unknown is not zero** — a sunken hatched pit, a hollow rib, a wireframe; never
   a zero-height cell that reads as a measured absence.
4. **No fabricated structure (D7)** — no force-directed layout, no CFG
   reconstruction of loop nesting, no element→lane threads the edges never carry.

One decision is new to *this* family, because it is the one the depiction axis can
get wrong in a way the other two cannot:

**A new layer never invents a channel; it re-reads one the model already carries.**

Every layer below is either (a) the same cells re-lifted by a different recorded
quantity, (b) an aggregate of recorded per-cell values, or (c) a projection of a
recorded address/step stream onto the existing plane. Where a layer wants a
quantity the recording does not carry, the answer is a Phase-0 producer or model
change with its own task and its own test — never a render-time estimate. That is
why [54](54-3d-catalog-phase0-plumbing.md) exists and why it is first.

## 4. The graph inventory — all 26, with today's verified gate

Ranked as the catalog ranks them. **Brief** is where the work is cut; **Gate** is
what must land first, verified against `b657876`.

### 4.1 Additive layers (compose on the shared address × time × height plane)

| # | Layer | Value/effort | Gate today | Brief |
|---|---|---|---|---|
| L1 | Confidence terrain + coverage-window mask | high / med | none — `TF_STAT/TORN/CHURN/UNKNOWN` all exist ([terrain.h:49-62](../../../desktop/src/space/terrain.h#L49)) | [56](../archive/gui/56-fidelity-and-module-layers.md) T2 |
| L2 | Per-module residency skyline | high / med | none — `CodeCell::full_heat` ([terrain.h:117](../../../desktop/src/space/terrain.h#L117)) + `Projection::regions` | [56](../archive/gui/56-fidelity-and-module-layers.md) T3 |
| L3 | Opcode-class code terrain | high / med | [54](54-3d-catalog-phase0-plumbing.md) T4 (`mnemonic_class`) | [56](../archive/gui/56-fidelity-and-module-layers.md) T4 |
| L4 | Misprediction survey layer (arcs + site columns) | high / med | [54](54-3d-catalog-phase0-plumbing.md) T7 (`HotEdge` sourcing) | [56](../archive/gui/56-fidelity-and-module-layers.md) T5 |
| L5 | Crossing spurs on the worldline | high / med | [54](54-3d-catalog-phase0-plumbing.md) T3 (`Event::seq`) | [57](57-causal-layers.md) T2 |
| L6 | Taint isochrone (forward-spread front) | high / med | [54](54-3d-catalog-phase0-plumbing.md) T5 (BFS-depth walk) | [57](57-causal-layers.md) T3 |
| L7 | Read/write twin relief | high / med | [54](54-3d-catalog-phase0-plumbing.md) T1 **+** T2 | [58](58-memory-data-cell-family.md) T2 |
| L8 | Working-set tide | high / med | [54](54-3d-catalog-phase0-plumbing.md) T1 + T2 | [58](58-memory-data-cell-family.md) T3 |
| L9 | Observed-lifetime pillars | high / med | [54](54-3d-catalog-phase0-plumbing.md) T1 | [58](58-memory-data-cell-family.md) T4 |
| L10 | JIT churn strata | med / med | none — raw `codeimage` events + `Event::seq` are decoded already | **not cut** (§7) |
| L11 | Blame convergence forest | med / med | none — `Streams::blame` exists (33 R6) | [57](57-causal-layers.md) T4 |
| L12 | Data-access worldline ribbon | med / med | [54](54-3d-catalog-phase0-plumbing.md) T1 | [58](58-memory-data-cell-family.md) T5 |
| L13 | Residency sediment columns | med / med | none — `CodeCell::steps`/`DataCell::steps` precomputed | [58](58-memory-data-cell-family.md) T6 |
| L14 | Dominant-path ridge | med / med | none — `TraceStream::insns/blocks` | [57](57-causal-layers.md) T5 |

### 4.2 Standalone scenes (their own coordinate system)

| # | Scene | Value/effort | Gate today | Brief |
|---|---|---|---|---|
| S1 | Divergence worldline | high / med | none — `dt_divergence`/`dt_statediff` land (33 R6) | [59](59-standalone-scenes.md) T2 |
| S2 | Invocation stack | high / med | [54](54-3d-catalog-phase0-plumbing.md) T6 (`dt_link` invocation field) | [59](59-standalone-scenes.md) T3 |
| S3 | Module excursion ribbon | high / med | [54](54-3d-catalog-phase0-plumbing.md) T3 (`Event::seq`) | [59](59-standalone-scenes.md) T4 |
| S4 | SIMD lane prism | high / med | none — `ValRec::wide/bytes` decoded ([streams.h:39-44](../../../desktop/src/doc/streams.h#L39)), unused in 3D | [59](59-standalone-scenes.md) T5 |
| S5 | Exploded evidence stack | high / **large** | [55](55-scene-render-quality.md) T4 (order-independent translucency) | **not cut** (§7) |
| S6 | The boundary curtain | high / large | [54](54-3d-catalog-phase0-plumbing.md) T3 + a syscall class/return parser | **not cut** (§7) |
| S7 | Module interaction braid | high / large | [54](54-3d-catalog-phase0-plumbing.md) T7 | **not cut** (§7) |
| S8 | Cone frustum (+ articulation bottleneck) | high / large | [54](54-3d-catalog-phase0-plumbing.md) T5 | **not cut** (§7) |
| S9 | Value-ribbon braid | high / large | none — `loom_fabric_build` exists | **not cut** (§7) |
| S10 | Ensemble stability terrain | high / large | a canonical code-only ensemble projection | **not cut** (§7) |
| S11 | Loop helix nest | high / large | none, but constrained by the block-step ambiguity | **not cut** (§7) |
| S12 | Computation character over passes | med / med | a `Selection`/`timeline_build` class-width filter facet | **not cut** (§7) |

Nineteen of the twenty-six are cut here, plus all seven Phase-0 prerequisites and
the render work. The seven left are the catalog's own Phase 4/5 — §7 states what
each needs.

## 5. GL effects — what would materially improve these scenes

The user-facing ask that produced this doc was not only "schedule the catalog"
but "find what render techniques would make these scenes better". The survey is
in [55](55-scene-render-quality.md); this is the summary and, more importantly,
the **gate**, because the app's GL baseline is narrow and every candidate has to
clear it.

**The baseline, verified.** Non-Apple hosts get a GL **3.0** context and the
shaders are written at `#version 130`
([main.cpp:121-127](../../../desktop/src/main.cpp#L121),
[embedded.h:5-12](../../../desktop/src/scene3d/shaders/embedded.h#L5)); Apple gets
**3.2 core / GLSL 150** ([main.cpp:110-120](../../../desktop/src/main.cpp#L110)).
Entry points come from `GL_GLEXT_PROTOTYPES` + `libGL` on Linux and the OpenGL
framework on Darwin, with **no glad/glew/gl3w**
([scene.cpp:1-15](../../../desktop/src/scene3d/scene.cpp#L1)) — so anything above
core 3.0 needs a runtime probe *and* a way to reach the entry point, which is
itself a decision, not a detail.

| Technique | What it buys these scenes | Clears the baseline? |
|---|---|---|
| **Eye-Dome Lighting** (Boucheny/Kitware; used by ParaView, Potree, CloudCompare, ArcGIS Pro) | Depth ordering becomes readable on *unlit* geometry — which is exactly what a worldline, a spur and an arc are. One post pass over the depth buffer, darkening pixels whose neighbours are nearer. | ✅ core 3.0 — depth texture + one fullscreen pass. **Highest value, lowest risk.** |
| **Depth-dependent halos** (Everts et al., IEEE Vis 2009 best paper) | Designed literally for *dense line data*: a depth-tested halo around each line so bundles read as bundles and crossings read as crossings. The scene is heading for many more lines (spurs, arcs, ribbons, braids). | ✅ core 3.0 — object-space, extra geometry per line. |
| **`fwidth`-based contour bands** | Terrain height stops being an unreadable gradient and becomes a *quantity with an interval* — the review's #37/#40/#56 and 46's G8. Resolution-independent by construction. | ✅ derivatives are core in desktop GLSL 130. |
| **MSAA** | 1–3 px lines over a displaced grid alias badly; `GLFW_SAMPLES` is never set anywhere in the tree. | ✅ a window hint + FBO sample count. |
| **Weighted-blended OIT** (McGuire & Bavoil 2013) | The catalog's §9 top perf/legibility risk is stacked translucency — exploded sheets, sediment bands, JIT strata, ensemble supports. WBOIT removes the sort. | ⚠️ MRT + float targets are core 3.0, but **per-target blend functions** (`glBlendFunci`) are GL 4.0 / `ARB_draw_buffers_blend`. Needs a probe + a fallback. |
| **Dithered (stochastic) transparency** | The same problem, solved without MRT: a per-fragment discard threshold. Interleaved Gradient Noise (Jimenez 2014) is a `dot`+`fract` and avoids the beating a Bayer grid shows against a regular terrain grid. | ✅ ALU only — the **fallback** WBOIT needs. |
| **Instancing** | The city's building system and any per-cell column layer (sediment, pillars, site columns) at plane scale. | ⚠️ GL 3.1/3.3 core; 43 already flags this. Not taken in this family. |

Two defects the survey turned up on the way, both cited and both owned by
[55](55-scene-render-quality.md):

- **`glLineWidth(2.0f)` / `(3.0f)`** ([scene.cpp:664](../../../desktop/src/scene3d/scene.cpp#L664),
  [:695](../../../desktop/src/scene3d/scene.cpp#L695)) — wide lines are deprecated
  in core profiles and a core implementation may cap `GL_ALIASED_LINE_WIDTH_RANGE`
  at 1.0, which would silently flatten the trajectory tubes and the deliberately
  "thick, so it pops" convergence arcs on the Apple 3.2-core path.
- **`#version 130` on a 3.2 core context.** Every shader in
  `shaders/embedded.h` is `#version 130`, unconditionally, while `main.cpp`
  selects a 3.2 **core** profile on Apple and hands ImGui `#version 150` for the
  same context. Apple's core profile documents GLSL 1.50 as its shading language.
  If that combination rejects 130, `init_gl` fails and the whole pane falls to the
  no-GL placard on that platform — the same branch 46's G13 describes for a driver
  failure. **This is stated as a verify-first item, not a confirmed break**: it
  needs one run on Darwin silicon, which this tree has no lane for today.

## 6. Where each brief lands in the city

So 43 stays the representation frame and no future city brief re-derives content
already verified here.

| This family | City phase it feeds ([43](43-faithful-city-roadmap.md) §3) |
|---|---|
| [54](54-3d-catalog-phase0-plumbing.md) T1 (data spans) | **C** — lit windows; **E** — the heap/data district |
| [54](54-3d-catalog-phase0-plumbing.md) T3 (`Event::seq`) | **B** — district commutes; **D** — City Hall syscall spurs |
| [54](54-3d-catalog-phase0-plumbing.md) T7 (`HotEdge`) | **B** — survey roads with blackspots |
| [55](55-scene-render-quality.md) | **C** — skyline LOD / entity budget (the legibility half) |
| [56](../archive/gui/56-fidelity-and-module-layers.md) | **A** (weather, landed) → **D** — module towers |
| [57](57-causal-layers.md) | **B** — errands & deliveries; **D** — City Hall |
| [58](58-memory-data-cell-family.md) | **C** — lit windows, sediment & patina |
| [59](59-standalone-scenes.md) | **D**/**E** — the boundary swimlane, the citizens roster |

## 7. What a Phase-4/5 cut would contain

Left uncut deliberately, following 43's own discipline (*"cut phases one at a
time… each phase brief needs its own fresh file:line re-verification pass against
whatever HEAD exists when it is cut"*). Each row states the one thing that makes
it cuttable, so the next cut is mechanical rather than exploratory.

| Would-be brief | Contents | Cuttable once |
|---|---|---|
| **60 — the evidence stack & the translucency capstone** | S5 exploded evidence stack, L10 JIT churn strata | [55](55-scene-render-quality.md) T4 lands — both are literally stacks of translucent sheets, and neither is honest without order-independent compositing |
| **61 — the kernel & module domains** | S6 boundary curtain, S7 module interaction braid | [54](54-3d-catalog-phase0-plumbing.md) T3 + T7 land **and** a syscall class/return parser exists with its bucket-to-unknown rule tested |
| **62 — the dataflow scenes** | S8 cone frustum, S9 value-ribbon braid | [54](54-3d-catalog-phase0-plumbing.md) T5 lands and [59](59-standalone-scenes.md) T1's non-plane scene host is proven by S1–S4 |
| **63 — cross-recording & iteration structure** | S10 ensemble stability terrain, S11 loop helix nest, S12 computation character | A canonical code-only ensemble projection is designed (the code_sha-does-not-imply-same-projection problem, catalog §9) and the `Selection` class/width filter facet exists |

## 8. Sequencing

```
54 phase-0 plumbing ──┬── 56 fidelity & module layers  (needs 54 T4, T7)
                      ├── 57 causal layers             (needs 54 T3, T5)
                      ├── 58 memory data-cell family   (needs 54 T1, T2)
                      └── 59 standalone scenes         (needs 54 T6)

55 render quality ───── independent of all of the above; land before 58
```

- **54 first, and it is splittable.** Its seven tasks are independent of each
  other; two agents can take disjoint halves. T1 (data spans) is the single
  highest-leverage task in the family — see §2.
- **55 is independent and should not wait.** Every task in it improves the scene
  that exists today, and T1 (EDL) + T3 (contours) are what make a *multi-layer*
  scene legible at all. Landing it after 58 means building the memory family into
  a scene that cannot show it.
- **56 before 57.** 56 T1 builds the layer registry the other three briefs' layers
  register into; doing it twice is the only real duplication risk in the family.
- **59 T1 gates 59 T2–T5** — a standalone scene needs a scene host that does not
  assume the address plane, which does not exist today (`SceneFrame` names exactly
  one `TerrainModel`, one `TrajectorySet`, one `ConvergenceSet`,
  [scene_host.h:37-65](../../../desktop/src/ui/scene_host.h#L37)).
- **This family and 47–52 do not block each other**, with one overlap worth
  naming: 47 T3 adds pick id *bands* for overlay geometry, and every new layer
  here needs a band of its own. Whichever lands first owns the band allocator; the
  second reuses it. Both briefs say so.

## 9. Cross-references

Extends [10-spacetime-3d-overview.md](../archive/gui/10-spacetime-3d-overview.md)
(the substrate every layer rides), [34-playhead-and-scene-reach.md](../archive/gui/34-playhead-and-scene-reach.md)
(the two-axes rule every time-varying layer holds to) and
[44-faithful-city-phase-a-mvp-terrain-reskin.md](../archive/gui/44-faithful-city-phase-a-mvp-terrain-reskin.md)
(the zoning/atmosphere/vehicle systems these layers compose with). Sibling
roadmaps: [43](43-faithful-city-roadmap.md) (representation),
[46](46-3d-functional-roadmap.md) (instrument) and
[38](38-live-feed-completion-roadmap.md) (the live feed — every layer here
inherits the live re-weave, catalog §8.1). Fidelity chrome D7 /
[23](../archive/gui/23-graded-truth-layer.md); wording D7 /
[24](../archive/gui/24-one-visual-language.md).
