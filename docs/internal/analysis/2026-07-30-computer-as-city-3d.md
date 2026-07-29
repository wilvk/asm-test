# The honest city — the computer as a living city over time

Review date: 2026-07-30. Scope: a design + implementation proposal to represent
the computer's state over time as a 3D **living city**, by reframing and
extending the existing [`desktop/`](../../../desktop/) spacetime overview
([scene3d/](../../../desktop/src/scene3d/) over the Hilbert
[projection](../../../desktop/src/space/projection.cpp) /
[terrain](../../../desktop/src/space/terrain.cpp) substrate). Processes and
low-level concepts become entities in a city — land, districts, buildings,
traffic, weather — held to the app's tested honesty rules. Companion to the
[3D visualization catalog](2026-07-29-3d-visualization-catalog.md) (this design
**unifies** that catalog's 14 layers + 12 scenes under one metaphor) and the
[UX/dataviz review](2026-07-29-gui-ux-dataviz-review.md).

**Method.** 17-agent grounded ideation informed by online prior art: eight
designers each mapped one city system (land, buildings, traffic, time, civic,
weather, multi-city, rendering engineering) against the real code and the
literature, an adversarial pass rejected anything that fabricated structure or
was unbuildable from recorded data, and a synthesis produced the plan below. **44
city elements survived**; the mapping is consolidated to 29 rows. Prior art:
CodeCity (Wettel & Lanza), SynchroVis (concurrency), Gource, Live Dynamic
Software Cities, treemap memory viewers — see §9.

> **Verification.** Load-bearing claims were spot-checked against the tree: the
> Hilbert projection + region-kind model, the terrain fidelity flags
> (`TF_STAT/TORN/CHURN`), and the `resolve_pick` deep-link seam all exist as
> described. The review also surfaced a **real live-capture bug** in the GL
> upload gate — the 3D worldlines freeze while the terrain grows — filed
> separately as
> [2026-07-30-live-3d-trajectory-upload-defect.md](2026-07-30-live-3d-trajectory-upload-defect.md)
> and folded into Phase C below. Line numbers are as of HEAD `49cfeea`;
> re-verify before building. This is an ideation + implementation proposal, not a
> committed schedule.

---

## 1. The concept

The pitch: the computer's execution IS already a city — we just weren't drawing it as one. The existing spacetime scene's substrate does the honest work no aesthetic algorithm could: the virtual address space is the LAND, laid out by a locality-preserving Hilbert curve over a compacted address domain (space/projection.cpp), so every coordinate comes from a real recorded address, never a treemap or force layout. Memory regions become DISTRICTS zoned by kind (code/stack/heap/data/mmap/unknown) with real name banners; hot code cells and touched allocations become BUILDINGS whose height is a real recorded metric (log access-heat, byte-size, survey count); the PC worldline and its threads become the TRAFFIC — a driven vehicle on roads with the control-flow survey network overhead; and the recording's fidelity becomes the WEATHER — clear day for exact traces, overcast for truncation, red-dusk for integrity loss, plus a separate literal fog surface for statistical (IBS) survey residency. Two clocks run and never fuse: a day/night SUN driven by terrain trace-time (HudState.t — how far through the residency), and a followed CITIZEN driven by the execution-step playhead (Selection.step — which instruction the flat views are reading). This one metaphor absorbs the entire 14-layer/12-scene 3D catalog: confidence terrain = weather, per-module skyline = module towers, working-set tide = lit windows, boundary curtain = district commutes, module excursion = the commuter's trips — they stop being a menu of overlays and become systems of one place. What it lets a user SEE that they cannot today: the recording GROWING and AGING as a living city over both clocks — districts building out as libraries dlopen, towers rising as code heats, windows pulsing as the working set moves, scaffolding cracking where JIT churns, storm-rubble where the trace tore, fog where only survey saw — all anchored so nothing ever relays out, orienting the eye to the one anomaly worth a 2D read.

---

## 2. The mapping — computer entity -> city element

The heart of the design. Every position and height comes from a real recorded
quantity (address, heat, size, count), never an aesthetic layout — see §6.

| Entity | City element | Encoding | Built from | Honesty |
|---|---|---|---|---|
| Compacted virtual address space (Hilbert over domain_off prefix sum) | **The Plat — the ground/land** | XY position = Hilbert(compacted address); the one honest distortion (packed-out gaps) confessed by canal seams | `space/projection.cpp project()/unproject(), Projection.domain_off` | Position is a real address, never an aesthetic layout; off-domain padding stays dark water, never drawn as land |
| Memory region by kind (Region.kind) | **District / zoned neighborhood** | Per-cell base hue = kindHue[k] mirroring region_style(); mix(kindHue, hotAccent, vHeight) | `space/types.h Region, terrain regions_from_codeimage/_from_maps, slice-invariant kind_by_cell` | Kind is a per-cell lookup of the region a cell provably owns; a code-only recording is unchanged (Code amber == today's hot ramp) |
| Region identity (base/len/label/version) | **District name banner + civic spire/monument** | Billboard label at address-derived cell-centroid; central monument = the recording's `code` identity; version churn badge | `Projection.regions, Streams.code` | Verbatim label ('code@0x<base>' when unnamed); never invents a module/function name; unknown kind = fog spire |
| Compaction gap between domain-adjacent regions | **Canal / mortar seam** | Thin glyph along the shared Hilbert border; width/label = log(raw gap bytes) | `regions[i+1].base - (regions[i].base+len) from recorded bases` | Confesses the projection's only distortion; contiguous districts get no glyph; wilderness stays void not canal |
| Hot executed code cell (CodeCell.full_heat) | **Code lot / extruded tower** | One cuboid per touched cell; height = slice(t) log1p(hits<=t); Code kind hue | `TerrainModel::CodeCell{cell,steps,full_heat}` | Vacant/never-touched cell places nothing; labelled as an ADDRESS RANGE, never a symbol; building supersedes membrane so heat isn't double-encoded |
| Per-region aggregate heat | **Module tower / library skyline** | Translucent tower per Region; height = LOG(sum of RAW full_heat over its cells), then log — never sum-of-logs | `single pass over m.code joined via proj.unproject to its Region` | Cold-but-mapped region = wire-outline footprint; roof badge = distinct CodeVersion count (recorded), not a fabricated invocation count |
| Recent occupancy window (CodeCell.steps in [t-W,t]) | **Lit windows — day/night occupancy** | Facade lit warm if hit in window, dark if hits only before t-W, vacant if never; density = log(hits in window) | `two upper_bounds over CodeCell.steps (no re-scan)` | Distinguishes occupied/idle/never-built that heat overlays collapse; rides terrain-time only; survey ghosts light only in the stipple channel |
| JIT/self-modifying churn (CodeVersion bumps, TF_CHURN) | **Scaffolding + cranes + version strata** | Scaffold hatch (frag) at t>=churn_step; translucent version-floor stack per distinct CodeVersion | `DisasmView.versions raw list, CodeCell.churn_step` | Only recorded bumps raise it; static district never sprouts; per-floor timing only if the churn walk records a step per bump, else a static stack at the one known step |
| Observed-touch data address (DataCell) | **Heap plot / data building** | Cuboid per touched DataCell; height = log1p(cum bytes<=t); read vs write as SEPARATE facades | `mem ea/size/rw -> DataCell (needs split cum_read/cum_write); ValRec fallback` | LOUD banner: observed-touch NOT allocation lifetime (no malloc/free producer); dormant boards up, never demolishes; needs a data-span projection to place at all |
| Driven PC worldline at exec-step head (TrajPoint at Selection.step) | **The commuter / followed citizen (vehicle)** | tid-coloured head glyph at the placed PC vertex where t==exec-step; comet tail = sub-range of the per-tid line VBO | `TrajectorySet, TrajPoint{t,addr,placed,tid}, transport.h playhead_project` | Only a PLACED exact vertex gets a vehicle; !placed = hollow ghost at map edge; no interpolation across dropped steps (break the strip, don't bridge) |
| Control-flow survey network (SurveyEdge{from,to,count,mispred}) | **Roads & highways with accident blackspots** | Road per SurveyEdge project(from)->project(to); width=log10(count); colour=mispred/count calm->blackspot | `streams.h SurveyEdge (statistical tier)` | SURVEY tier only (exact-tier roads dropped: blocks are dedup-ascending, no order, no mispred); mispred==0 calm road distinct from UNSAMPLED (fog); persistent statistical banner |
| Data access mark (mem ea/rw/size) | **Errand / delivery driveway** | Reframe access spur: cool pickup (load) vs warm dropoff (store) by rw; thickness by size; lights as owning step passes the head | `trajectory access_spurs_, mem event rw/size` | Off-map ea ghosts to map edge (never cell 0); a reg-only ValRec has no cell and draws nothing; unknown cargo only on the ValRec fallback path |
| Region-membership change on the worldline | **District commute (call/return seam)** | CrossingMark glyph where a placed vertex's region index changes; hued by entered kind | `space/commute.cpp over placed vertices + Projection region_index_at()` | Refuses to reconstruct call/return/nesting from the linear worldline (the D7 greedy-reconstructor trap); depth/direction only from full-app TreeRow labels |
| Two threads co-located in address x time (ConvergenceMark) | **Rendezvous plaza** | Existing magenta hint bezier + a ground glyph at the shared cell; labelled tid-pair + gap; lit when t_a/t_b bracket the head | `space/converge.cpp ConvergenceMark{tid_a/b,cell,t_a/t_b,gap}` | gap states its own looseness — a co-locality HINT, never a proven race/order; four-condition admission bar (placed+exact+per-thread+same-clock) |
| Invocation passes (df_invocation, SegmentedDataflow.passes) | **Seasons — the city's calendar / segmented player** | shell_df_pass_pager as season selector; discrete-only chapter ticks; open pass = frayed prefix | `streams.h SegmentedDataflow.passes, build_segmented_dataflow` | Never a tween across the unobserved gap between passes; a one-shot recording = one season, byte-identical chrome; requires pass-scoped weave (not just swapping Streams::df) |
| Terrain trace-time (HudState.t / nsteps) | **Day/night SUN — the wall-clock** | sun = t/nsteps drives uLightDir azimuth+elevation + sky phase; v1 shadow = dot(normal,sunDir) | `HudState.t, terr.nsteps` | Labelled 'trace time (steps), not seconds'; torn caps the arc under a banner; empty land stays dark; never fused with the exec-step playhead |
| Execution-step playhead (Selection.step) | **The followed citizen's clock (the day player)** | A SECOND labelled transport (ui/transport.h) driving uHeadStep for vehicle/windows/errands; chapters from pass boundaries | `Selection.step, ShellState` | Strictly separate from the sun's terrain-time transport (the doc34 'do not fuse the playheads' D7 trap); statistical-only recording -> player disabled |
| Cell first/last touch (CodeCell.steps front/back) | **Sediment (downward) + patina (tint) — age** | Patina = hue tint (touched-span/elapsed-t); sediment = geometry BELOW the plane (depth = founding..last-touch), keeping above-plane height pure density | `CodeCell/DataCell steps front()/back()` | Never an upward plinth competing with density-height; founding step fixed across live growth (base never jitters); torn tail = frayed open cap; unknown pre-history = eroded ground |
| Syscall (SyscallRow, no pc/addr) | **City Hall permit commute (kernel-crossing spur)** | Spur off the worldline vertex preceding it in stream order up to a magnitude-less City Hall rail; hue=class, return-leg tint=outcome | `views/syscalls, Event::seq (needs seq on SyscallRow AND TrajPoint)` | Refuses to place on the plane (no recorded kernel address); anchor labelled 'approx — last insn before this seq'; redacted payload stays redacted; self-disables with a stated reason when no worldline |
| Register file at exec-step (dt_scrubber regs) | **Control Tower status board** | World-anchored HUD text board; ONLY encoded signal is the per-reg change-glow; values read as text | `scrubber regs{name,value,changed}, StepIndex` | NO ordinal-to-height skyline (register index is list position, not a recorded quantity); torn=dark scaffold, synthesized=RE-DERIVED banner, has_prev==false=baseline |
| Watchpoint hit (WatchHit{addr,pc,is_write,value_ok}) | **Watchpoint CCTV mast** | Mast at project(WatchArm.addr); hits stack by hit_no along the mast's OWN local axis; shape=tri-state is_write; hollow when !value_ok | `views/watch WatchArm/WatchHit, obs_watch_build` | Hit has NO trace step (fires between single-steps) — never placed on the terrain vertical; refused arm = placard with skip.reason verbatim; off-plan addr = labelled annex |
| Dominant fidelity tier (honesty_severity) | **Weather / honesty sky** | Neutral=clear, Caution=amber overcast, Integrity=red-dusk front + ambient tint; subtle horizon-only fog | `ui/honesty.h honesty_severity(honesty_facts_of(rec))` | Byte-identical to the 2D banner verdict (cannot invent a tier); global shift stays subtle so one torn tail never repaints the per-district signal; HUD chip is the instant second channel |
| IBS survey residency (TerrainModel.stat, TF_STAT) | **Ghost districts in the fog** | A physically SEPARATE translucent+desaturated+stippled surface a hair above the exact land; height = log1p(survey count) | `build_stat over SurveyEdge.count, survey_samples/survey_lost` | Makes the T6 isolation invariant physical — a distinct GL object over a distinct texture that can never sum into exact; time-INVARIANT (a survey has no exact time), so it does not scrub |
| Truncation/tear (TF_TORN) | **Rubble / sinkhole / storm damage** | Construction-tape hatch + darkened base + jagged UPPER edge (lower-bound cap) | `slice() TF_TORN when rec.truncated()\|\|rec.dropped()` | Fray is a top-EDGE jitter only — a torn cell never reads as LESS density than a non-torn one; distinct from STAT dim; banner survives the pick into 2D |
| In-domain cell with no code/data/stat content | **Fog-of-war — dark hatched pit** | Sunken veiled pit (TF_UNKNOWN bit 5), distinct from a described low cell AND from off-domain (never drawn) | `build_terrain: in-domain minus touched cells` | Unknown-not-zero; scoped to PLANE cells only (df has_step and ValRec value_valid are time/operand gaps surfaced in their own views, not fabricated terrain cells); drill-in routes to the region reader, not a canvas step |
| Redacted payload (Streams.redacted) | **Sealed / frosted building (deferred) — a HUD chip today** | Quiet Neutral redaction chip in the HUD provenance row now; frosted sleeve on kernel-crossing marks once that layer lands | `honesty.h HonestyFacts.redacted` | Known-to-exist-but-withheld (distinct from unknown/never-observed); content never rendered; stays redacted through the drill-in; no frost pass added before geometry exists to attach it to |
| Process (TopoTask tgid/ppid, address-less) | **Archipelago — one island per process** | Islands at discrete tgid RANK on a labelled pid axis (a third spatial axis); primary tgid = real terrain, others = hollow atolls | `views/topo, regions_from_codeimage (keys base/len only)` | Co-locating two address spaces on one plane would fabricate a shared coordinate — the sea denies it (inter-island distance carries NO metric); only the primary tgid has real terrain |
| Thread (Trajectory per tid) | **Citizen / traveler** | HUD roster joins topo threads to trajectories by tid; select ghosts other paths; leader='founder' | `one Trajectory per tid, tid_color palette, topo threads` | Draws only PLACED points; the statistical crowd stays an anonymous stipple, never promoted to a named citizen; a topo tid with no path = roster row 'present, no placed path' |
| Cooperative task / fibre (uncaptured) | **Ghost citizens — the fibre gate** | Legend/placard only: 'fibres not captured — ptrace/PT/IBS see OS threads (tid) only' + a contract note in trajectory.h | `NONE (a fibre switch is indistinguishable from a call)` | Draws NO fibre boundary (fabricating one from PC discontinuities is the trap); no premature TrajPoint.fid field until a producer emits it |

---

## 3. The city as systems

The elements group into coherent systems, each a `SceneLayers` toggle. Ranked by
value.

### Foundation — the engine-free CityModel + GL draw half

`high value` · `large effort`

The model/view split the whole city stands on: a pure space/city.{h,cpp} that aggregates the existing TerrainModel/TrajectorySet/Projection into golden-testable InstanceSets, plus the GL draw passes (instanced/merged buildings, a per-frame vehicle, and the new city buffers) on the strict 3.0/GLSL-130 context.

- **Key elements.** The CityModel plat (engine-free aggregation half); Buildings — hot code as extruded towers; Roads & the Gource traveler (worldline as streets + vehicle)
- **Implementation.** desktop/src/space/city.{h,cpp} + test_city.cpp, std-lib + space/ only, reusing CodeCell.steps binary search for slice(t) so scrubbing stays sub-frame; clones the terrain.cpp model/view split (D4). Buildings render as a DEFAULT baked merged-mesh VBO rebuilt like set_trajectories (glDrawElementsInstanced/glVertexAttribDivisor are 3.1/3.3 extensions — guard true instancing behind an ARB check). District plinths derive from a region's ACTUAL Hilbert cell set (walk domain_off[i]..domain_off[i+1] through proj.project), never a min-max bbox. New building shader pair in scene3d/shaders/embedded.h at #version 130.

### The Land & Zoning — the honest Plat

`high value` · `medium effort`

Turns the invisible districts into visible ground: the terrain surface gains per-cell kind hue, districts gain borders + name banners + civic spires, compaction seams confess the packed gaps, and a compass/minimap orient the eye as the city scales.

- **Key elements.** The Plat — address-honest ground zoned by kind; District signage — borders, banners, anchor-stable orientation; Compaction canals — the honest seam between packed districts; Civic landmarks — the routine under study & module monuments; Compass & minimap
- **Implementation.** Add slice-invariant std::vector<uint8_t> kind_by_cell to TerrainModel (one O(cells) unproject sweep, gated by LOD above order ~10); new Scene::set_zoning uploads an R8UI tex ONCE per weave (never on scrub). kTerrainFrag adds usampler2D uKind + kindHue[6] mirroring region_style() exactly. Borders/canals in engine-free space/zoning.{h,cpp} -> GL_LINES uploaded like conv_arcs_ under SceneLayers.zoning; labels/spires drawn as world-anchored text in the HUD TU (hud.cpp), keeping the scene TU ImGui-free.

### The Built Environment — buildings that grow, light, and age

`high value` · `large effort`

The skyline: code lots and module towers by real heat, windows that pulse with occupancy over the day, scaffolding where JIT churns, and (gated on a data-span projection) observed-touch data buildings — all anchored so growth never relays out.

- **Key elements.** Code lots — the executed-address skyline; Module towers — the library skyline; Lit windows — occupancy over trace-time (day/night); Construction cranes & version strata — JIT churn; Heap plots — observed-touch data buildings; Anchor-stable growth & LOD
- **Implementation.** CityModel::slice(t) and slice(t,W) drive per-instance {height,flags,hue,cell_id} and {hits_in_window,hit_ever}; a fragment facade reads the window values (no new geometry for windows). A building supersedes/flattens the terrain membrane on its own cell to avoid double-encoding heat. Identity = the immutable address footprint cell, so a re-weave rewrites an instance's height/flags in place (cell->VBO-offset index + glBufferSubData) rather than relaying out; height_shown->height_target lerp is host-side render smoothing only, never fed to picks.

### Traffic & Transit — the driven worldline and its errands

`high value` · `medium effort`

The living motion the static tubes cannot show: a followed vehicle on the exec-step clock, the survey control-flow road network with accident blackspots, data-access errands that light as the citizen makes them, district commutes, and rendezvous plazas where two threads meet.

- **Key elements.** Commuter fleet — the driven PC worldline; Roads & highways — control-flow traffic with blackspots; Errands & deliveries — data accesses as stops; District commutes — call/return as trips; Rendezvous points — where two citizens meet; The followed citizen — the exec-step playhead vehicle
- **Implementation.** Add a per-vertex float t (and per-spur rw/t) attribute channel to the traj/spur VBOs so alpha/lighting gate on a uHeadStep uniform (one-time schema change; after that the vehicle is pure per-frame uniform). Break the projected GL_LINE_STRIP into contiguous-run Lines so a dropped off-map step fades out rather than bridging (honesty). Roads = engine-free space/roads.{h,cpp} over SurveyEdge, uploaded as an independent GL_LINES buffer (conv_arcs_ pattern) so live re-weave survives. Vehicle = a GL_PROGRAM_POINT_SIZE point after the road draw; it reuses the underlying PC vertex's existing pick id (no new id).

### Time & the Two Clocks — the living, evolving city

`high value` · `large effort`

The keystone that makes the city move honestly over BOTH axes without fusing them: a day/night sun on terrain trace-time, a separate segmented player on the execution-step, seasons for invocation passes, sediment for age, and the critical live-growth re-weave fix.

- **Key elements.** The day/night sun — terrain trace-time as wall-clock; The day player — segmented playback on the citizen clock; Seasons — invocation passes as the calendar; Sediment & patina — accumulation and aging; The city under construction — live growth + throttling
- **Implementation.** Two independently-labelled ui/transport.h states inside draw_scene_overview: sun = HudState.t/nsteps (rides the existing re-slice, zero re-weave cost, guard nsteps==0->noon); play_exec drives uHeadStep, chapters from seg_df passes — kept strictly separate (reusing the terrain transport is the D7 fuse). THE LOAD-BEARING FIX: gl_scene_host re-uploads trajectories/convergences only on f.key change, but f.key=hash(a.id) is invariant across live re-weave while terrain re-uploads on slice_t — so growing roads/arcs freeze after batch 1. Thread the monotonic live_built_events into f.key (or add f.gen) and prefer append/glBufferSubData over free+regen.

### Weather & the Honesty Atmosphere

`high value` · `medium effort`

The recording's fidelity becomes the sky and ground condition — the most load-bearing honesty system, and mostly cheap frag-shader work on geometry that already exists.

- **Key elements.** The honesty sky — weather front driven by dominant tier; Ghost districts in the fog — IBS survey as a separate vapor surface; Rubble, sinkholes & construction tape — torn districts; Fog-of-war — undescribed lots stay dark; Scaffolding hatch — JIT churn as construction; Sealed & frosted — redacted stays withheld (HUD chip now)
- **Implementation.** Pure header scene3d/atmosphere.h (struct Atmosphere{ambient,front,sun,fog_density}); shell computes the tier via honesty_severity, reads dt_warn/refuse/dim_col into raw floats (theme.h stays out of the engine-free scene TU), damps it ~0.5s in the shell loop, and passes the already-damped Atmosphere into a time-stateless Scene::set_atmosphere. Ghost fog = a NEW Scene::set_stat_terrain(m.stat) drawing a second desaturated+stippled surface with depth-write off, on a separate pick id range routing to hotedges. Rubble/scaffold/fog-of-war are kTerrainFrag branch upgrades on flags already uploaded per re-weave (fray is a top-edge jitter, not a lowering).

### Civic Infrastructure & the Kernel Boundary

`medium value` · `large effort`

The address-less facts hung honestly off the plane: syscalls as permit commutes off the worldline, a watchpoint CCTV mast (the strongest address-honest fit), a register status board, and the boundary curtain re-scoped as a 2D swimlane reading rather than a fabricated wall.

- **Key elements.** City Hall & the permit commute (kernel-crossing spurs); Watchpoint CCTV — surveillance on an address; The Control Tower — register file status board; The Boundary Curtain — kernel-crossing swimlane
- **Implementation.** space/crossings.{h,cpp} bakes worldline-vertex->rail GL_LINES into an independent buffer (set_convergences pattern) — Phase-0 needs seq on BOTH SyscallRow AND TrajPoint (offset-less trace events break positional seq<->vertex correspondence). Watch mast at project(addr) with hits stacked by hit_no on the mast's own local axis (a hit has no trace step). Control tower = world-anchored HUD text with change-glow the ONLY encoded signal (no ordinal-to-height). The boundary curtain drops the magnitude-less 3D wall and renders as a 2D seq x tid class swimlane in the observer TU that already has ImGui, folded onto the same CrossingSet.

### Multi-City, Scale & Liveness — the metropolis

`high value` · `large effort`

The honest scale story and the concurrency closer: an archipelago of process-islands on a discrete pid axis, threads as named citizens, and the LOD/entity-budget discipline that keeps a dense recording from becoming a haze or a frame cliff.

- **Key elements.** Archipelago — one island per process; Citizens — threads as travelers; Rendezvous plazas — pickable meetings; Ghost citizens — the fibre gate; Skyline LOD & the metropolis budget; The zoning board — LOD, culling & entity budget
- **Implementation.** Islands at discrete tgid rank, X-slot pre-multiplied into the CPU-side MVP (scene uploads uMVP only — no per-frame per-terrain re-upload loop; only the primary tgid has real terrain, others are atoll wire-ring+beacon primitives). Add int32_t tid to Scene::Line so render() can ghost non-selected paths. Generalize the proven kScrubCellBudget/should_degrade/coarse_slice precedent into a kCityEntityBudget with three LABELLED camera-distance tiers (per-cell buildings -> region-aggregate towers[log of SUMMED RAW heat] -> the existing camera.top_down flat heatmap). Merge per-tid traj_lines_ into one attributed buffer JOINTLY with the Line.tid change.

---

## 4. Implementation plan

The concrete engineering — the core deliverable. Each names the code seam and
technique.

### Data model — Add an engine-free desktop/src/space/city.{h,cpp} (+ test_city.cpp) as the pure aggregation half, cloning the terrain.cpp model/view split

`medium effort`

build_city(const TerrainModel&, const TrajectorySet&, const Projection&) -> CityModel of InstanceSets: BuildingInstance{vec2 cell, float height=normalised log heat, uint flags, uint cell_id} keyed 1:1 to CodeCell; a SEPARATE stat InstanceSet mirroring TerrainModel.stat; DistrictPlinth = the region's ACTUAL Hilbert cell set (walk domain_off[i]..domain_off[i+1] through proj.project), never a bbox. Aggregate raw full_heat THEN log. slice(t)/slice(t,W) reuse CodeCell.steps binary searches so scrubbing is sub-frame. Golden-dumpable, half-paints only, links into asmtest-viewer and the null harness — no ImGui, no emulator.

### Producer gate — Emit a `maps` event on the asmspy serve+record leg from asmspy_proc.c's existing /proc/<pid>/maps parse — but SHIP zoning on code-only first, land maps after

`large effort`

kKnownKinds is 23 with no maps kind; regions_from_codeimage yields Code only, so Stack/Heap/Data/Mmap are defined-but-unpopulated. Add maps to kKnownKinds (recording.cpp), add regions_from_maps() (terrain.cpp) carrying perms so non-exec data never masquerades as code; degrade [stack] to bare tag when the tid isn't in the snapshot; redacted paths stay redacted. This is the single Phase-0 unlock that turns the Plat/signage/heap-district from a code-only company town into a named city — but the render (Plat + signage) must be built and tested against 'code@0x' districts first so it is not blocked on the large producer work.

### Rendering — buildings — Default to a baked merged-mesh building VBO rebuilt like set_trajectories; guard true hardware instancing behind an ARB check

`medium effort`

embedded.h is strict #version 130 / GL 3.0, so glDrawElementsInstanced (3.1) and glVertexAttribDivisor (3.3) are extensions. New building shader pair (attrs bound C++-side, no layout()). Building height = the same [0,1] log heat the terrain computes AND flatten/dim the terrain membrane under a building so the one heat number isn't double-encoded as both membrane and cuboid. Merged-mesh vertex count is the entity-budget driver.

### Rendering — zoning & atmosphere shaders — Extend kTerrainFrag for kind hue, torn rubble, churn scaffold, and fog-of-war on flags already uploaded; add a pure atmosphere.h + NDC sky quad

`medium effort`

Add usampler2D uKind + kindHue[6] (mirror region_style()), plus uAmbient/uFogDensity/uLightDir/uSkyPhase uniforms. Draw a 2-tri NDC sky quad FIRST at the far plane, depth-write off. Torn fray must be a top-EDGE jitter (never lowers a cell below a non-torn one) and stay distinct from the STAT *0.6 dim; churn scaffold hatch distinct from both; fog-of-war (TF_UNKNOWN bit 5) a dark hatched pit. Precompute frayed height into the R32F upload OR vertex-fetch uFlags (GL 3.0 supports vertex texture fetch) to keep the model golden-testable.

### Live update & anchor stability — Fix the live-growth freeze: thread the monotonic live_built_events into f.key (or add f.gen) so gl_scene_host re-uploads growing trajectories/roads/arcs, and prefer append/glBufferSubData over free+regen

`large effort`

CONFIRMED bug: gl_scene_host.cpp:66 re-uploads set_trajectories/set_convergences only when f.key != up_key_, but f.key=hash(a.id) is invariant across the per-batch re-weave while set_terrain re-uploads on slice_t (line 74). After batch 1 the growing road/arcs never re-upload though the terrain does. Anchor stability is FREE (Hilbert cell = real address, never migrates); the persistent per-cell instance table lives in the GL host (survives the shell.cpp SceneView{} reset), diffed against the fresh slice so only new+grown+reflagged cells upload. Note this fixes the GL UPLOAD only — the upstream whole-recording decode re-weave is a separate scaling follow-up.

### LOD / scale — Generalize the proven kScrubCellBudget/should_degrade/coarse_slice precedent into a kCityEntityBudget with three labelled camera-distance tiers

`medium effort`

NEAR = per-cell buildings + windows; MID = per-region aggregate towers (height = log of SUMMED RAW heat, never summed log heights; tower sits on a REAL region cell e.g. domain-midpoint, not a fabricated centroid); FAR = the existing camera.top_down() flat heatmap. Over budget -> drop to MID + a visible 'coarse city' placard (the existing degrade idiom). The kind_by_cell/fog-of-war O(cells) sweeps and the LOD crowd band (decimated EXACT) must stay provenance-distinct from the TRAJ_STATISTICAL survey stipple — label 'exact (LOD)', never launder exact into sampled.

### Interaction / drill-in — Route every pick through the existing resolve_pick deep-link router, extending the pick id space per new primitive class — 3D to find, 2D to read

`medium effort`

Buildings write pick_id_cell into the same R32UI target (resolve_pick already routes Cell->canvas, or disasm when churn_step set) — no new id space for lots. Add id bands for roads (->hotedges), ghost-fog cells (->hotedges/survey, separate range from exact), convergence arcs (->both tids at t_a/t_b — today arcs are un-pickable dead overview objects, making them pick is the real deliverable), syscall/watch spurs (->syscalls/watch, payload still redacted), and fog-of-war pits (->region reader, NOT a canvas step — a content-less cell has no offset). The vehicle reuses its underlying PC vertex id (no new id).

### Two-clock control — Add two independently-labelled ui/transport.h states in draw_scene_overview — never one global clock

`medium effort`

sun = HudState.t/nsteps (a new SceneFrame.sun float; rides the existing re-slice, needs no f.key bump, guard nsteps==0->noon; for live, hold a constant steps/sec so a growing nsteps lengthens the day rather than snapping the sun back). play_exec drives a new SceneFrame.follow_step (uHeadStep) for the vehicle/windows/errands, chapters from seg_df passes boundaries. Partial infra already exists (a step transport_tick sketch at shell.cpp; s.seg_df populated). Keep them structurally separate — reusing sv.play/hud.t for the exec clock is the doc34 D7 fuse.

### Camera — Preserve the orbit camera + HUD across re-weave (already done), store a persistent compass 'home' at the address-derived code-district centroid, and key LOD tiers to Camera.radius

`small effort`

shell.cpp resets SceneView{} but preserves cam/hud/primer; the stable cell key produces identical instances across rebuilds so the camera frames the same landmarks. Store the code-centroid anchor in SceneView so 'reset view' frames a stable landmark across live growth. A top-down minimap inset (second small viewport in draw_scene_overview) tinted by kind_by_cell with the live frustum overlaid — distinct from the pick FBO (which writes ids), so don't overload it.

### Composition with SceneLayers — One new SceneLayers bool per city system, each buffer independent and rebuilt in the if(!sv.built) weave, defaulting to a coherent 'city' preset

`medium effort`

zoning, buildings, skyline, windows, construction, heap, roads, statistical_terrain(default on, 'fog/survey'), crossings, watch, landmarks, archipelago. Each new geometry buffer follows the conv_arcs_/access_spurs_ pattern (its own GL_LINES/instanced buffer, uploaded independently of the path buffers) so a live path re-upload never wipes it. World-anchored TEXT (labels, spires, control-tower board) lives in the HUD TU (hud.cpp — the only ImGui + space/-model TU), projecting centroids through Camera::mvp; the engine-free scene TU stays text-free and ImGui-free.

### Rendering — statistical isolation — Add Scene::set_stat_terrain(m.stat) as a physically separate surface — the T6 invariant made geometry

`medium effort`

set_terrain takes a single Terrain, so a NEW setter guarded by m.has_stat is required; second tex_height_stat_/tex_flags_stat_ pair + prog_stat_ reusing kTerrainVert with a new kStatFrag; drawn AFTER exact terrain with GL_BLEND on, depth-write off, still depth-tested. It is TIME-INVARIANT (build_stat is an aggregate survey with no per-step axis) so it does not scrub with the playhead — state that; it visibly distinguishes sampled evidence from exact terrain that does scrub.

### Schema/attribute plumbing — Batch the one-time VBO attribute-channel additions the traffic system needs so buffers aren't rewritten twice by conflicting patches

`medium effort`

Add per-vertex float t to traj VBOs + per-spur rw/t to access_spurs_ (so lighting gates on uHeadStep); add int32_t tid to Scene::Line (so render() can ghost non-selected citizens); carry is_return through the SurveyEdge decode in streams.cpp; add uint64_t seq to TrajPoint (and SyscallRow) for kernel-crossing anchoring; split DataCell.cum_rw into parallel cum_read_size/cum_write_size for the heap twin-facades; extend the churn walk to record a step per version bump. Land the Line.tid change and the merge-per-tid-traj-lines refactor TOGETHER.

---

## 5. Roadmap — MVP city first

Cheapest-highest-value first. Phase A ships entirely on geometry that already
exists (a terrain reskin, no new renderer primitive).

### Phase A — MVP city (reskin the existing terrain scene, no new renderer primitive)

- **Ships.** The Plat (kind_by_cell + kTerrainFrag kind hue); The day/night sun on terrain-time; The honesty weather sky (atmosphere.h); Ghost districts in the fog (set_stat_terrain); Rubble/torn + scaffold-churn + fog-of-war frag branches; The followed citizen vehicle + comet tail on the existing traj VBO
- **Needs.** kind_by_cell sweep in build_terrain; atmosphere.h + sky quad; set_stat_terrain; TF_UNKNOWN bit 5; SceneFrame.sun + SceneFrame.follow_step; per-vertex t attribute on traj VBOs; a 'city' SceneLayers preset. All on geometry that already exists — highest value per line of code.

### Phase B — orientation & traffic

- **Ships.** District signage (borders, banners, civic spires/landmarks); Compaction canals; Survey roads with accident blackspots; Errands/deliveries (rw-hued lit spurs); Rendezvous plazas made pickable; District commutes; The day player (second exec-step transport, chapter ticks)
- **Needs.** engine-free space/zoning.{h,cpp} + space/roads.{h,cpp} + space/commute.{h,cpp}; is_return carried through the SurveyEdge decode; per-spur rw/t attributes; conv-arc pick ids; region_index_at() helper; ui/transport.h play_exec state.

### Phase C — the living, scaling city

- **Ships.** The live-growth re-weave fix (f.gen / append uploads); Lit windows (occupancy day/night); Sediment & patina (age); Skyline LOD + metropolis entity budget
- **Needs.** f.gen threading in gl_scene_host + glBufferSubData delta path; cell->VBO-offset index; CityModel::slice(t,W); downward sediment geometry; kCityEntityBudget + camera-radius tiers; merge per-tid traj_lines_ into one attributed buffer.

### Phase D — the built environment & civic

- **Ships.** Code lots + module towers (the instanced building system); Construction cranes & version strata; City Hall syscall permit commutes; Watchpoint CCTV mast; Control tower register board; Boundary-curtain swimlane (2D)
- **Needs.** space/city.{h,cpp} building InstanceSets + building shader pair + building pick draw; churn walk records a step per version bump; seq on TrajPoint + SyscallRow; space/crossings.{h,cpp}; obs_watch/scrubber board in the HUD/observer TU.

### Phase E — the metropolis & named districts

- **Ships.** The maps producer -> named library neighborhoods; Heap plots / data district; Archipelago (island per process); Citizens roster + ghost-select; Seasons (pass-scoped weave); Ghost-citizens fibre placard
- **Needs.** maps event on the asmspy leg + regions_from_maps(); observed-data-span projection + split cum_read/cum_write; space/archipelago.{h,cpp} + atoll primitive + island pick tag; Line.tid + roster panel; pass-scoped recording view + per-pass weave cache; a legend line + trajectory.h contract note.

---

## 6. Honesty reconciliation — why this city cannot lie

This city is more honest than CodeCity or SynchroVis because its honesty is STRUCTURAL, not stylistic — the rules are enforced by where data comes from, not by discipline. (1) Address-honest zoning: every building, district and plinth position comes from Hilbert(compacted address) via projection.cpp — CodeCity LAYS OUT buildings by an aesthetic tree/treemap algorithm and SynchroVis places them freely; we never run a force-directed or treemap layout, and a district plinth is the region's ACTUAL Hilbert cell set, not a bbox that would paint over a neighbour's cells. (2) Sparse -> sparse: a never-touched cell places nothing and an off-domain cell is dark water — a sparse trace reads as a mostly-fog city, which is the honest point, where CodeCity always fills its plane. (3) Statistical never merges into exact: IBS survey residency is a physically separate, desaturated, stippled surface over its own texture (set_stat_terrain) that the renderer literally cannot sum into the exact terrain, and it is time-invariant so it does not scrub — a survey has no exact time. (4) Torn survives the drill-in: TF_TORN cells become rubble with a top-EDGE fray (a lower-bound cap that never reads as LESS activity than a proven cell), and the TRUNCATED banner rides the pick into the 2D reader. (5) Unknown-not-zero: an in-domain cell with no content is a dark hatched fog-of-war pit distinct from a described low cell AND from off-domain not-land — and it drills to the region reader ('this range, never described'), not a fabricated canvas step; df has_step and ValRec value_valid gaps are surfaced in their own views, not fabricated as terrain cells. (6) Ordinal-never-height: the register board is REJECTED as a skyline because register index is list position, not a recorded quantity — every height is a real address/heat/size/count. (7) No reconstruction: district commutes refuse to infer call/return/nesting from the linear worldline (the documented D7 greedy-reconstructor trap), drawing direction/depth only where TreeRow records it; rendezvous state a gap, never a proven race; syscalls refuse a plane position (no recorded kernel address). (8) Two clocks never fuse: the day/night sun (terrain trace-time) and the followed citizen (exec-step) each get their own labelled transport, upholding the doc34 anti-fusion invariant. (9) Redacted stays withheld and weather is byte-identical to the 2D honesty verdict, so the sky can never invent a tier the banner wouldn't.

---

## 7. Open questions & where the metaphor strains

- Occlusion & perf at metropolis scale: at Hilbert order 12 the plane is 4096x4096 ~= 16.7M cells; the kind_by_cell and fog-of-war O(cells) sweeps and any full building instancing must be gated by an order threshold / lazy-per-visible-region path — where exactly is the cutover, and does opaque depth-tested building rendering avoid the translucency haze without a real frame cliff?
- Buildings vs terrain double-encode height: the membrane displacement and the cuboid height both encode log heat over the same cell — is flatten-under-building the right resolution, or should Buildings-ON fully replace the membrane, and how does that read at MID LOD where towers aggregate?
- Multi-process honesty: only the primary tgid can have real terrain today (regions_from_codeimage keys base/len only, no pid), so the archipelago is one real island + N atolls until a pid-tagged codeimage producer lands — is that gap acceptably communicated, and would ASLR make a shared coordinate meaningless anyway?
- Missing producer fields gate several districts: maps snapshot (named districts + data placement), per-module invocation count (module roof badge — degraded to CodeVersion count), mem-carries-tid (co-write beacon, per-thread errands), per-version churn steps (floor phasing), is_return on SurveyEdge (one-way roads), seq on TrajPoint (syscall anchoring) — which are worth the producer work vs an honest degrade?
- The upstream whole-recording decode re-weave (not the GL upload) is the real live-scaling risk — the GL delta fix does not touch decode cost; is an incremental decoder a separate must-do before the city is live-usable on a large growing capture?
- Navigation ergonomics: two clocks + orbit camera + minimap + per-system SceneLayers toggles + pick-drill-in is a lot of surface — does the 'city' preset + a guided default keep it legible, or does the operator drown in controls?
- Where the metaphor strains and should stay 2D: register file (no locality -> a board not a skyline), syscalls (no kernel address -> a fog spur not a viaduct), the boundary curtain (a swimlane, not a 3D wall) — are there other elements we are forcing into 3D that a panel reads better?

---

## 8. The bug this surfaced

Ideation found a real defect in the live GL upload gate: on a growing
`asmspy --serve` capture the terrain re-uploads each batch but the trajectory
tubes and convergence arcs do not, because the upload key
(`SceneFrame.key = hash(recording basename)`,
[`shell.cpp:896`](../../../desktop/src/ui/shell.cpp#L896)) is invariant as the
recording grows while the re-upload gates on it
([`gl_scene_host.cpp:64`](../../../desktop/src/ui/gl_scene_host.cpp#L64)). The
worldlines freeze while the landscape grows. Full writeup + one-line fix:
[2026-07-30-live-3d-trajectory-upload-defect.md](2026-07-30-live-3d-trajectory-upload-defect.md).
Phase C of this plan depends on it.

---

## 9. Prior art

We borrow the core encodings from CodeCity (buildings and districts with height as a real count metric, spatial locality for orientation), the concurrency vocabulary from SynchroVis (thread interactions as marks between citizens -> our rendezvous plazas; trace SEGMENTATION + information filter + 'played like a movie' -> our seasons and the two-clock players), the traveler idea from Gource (an agent travels to the thing it touches -> our commuter making errands to touched data cells, and time scrubbing), the liveness discipline from Live Dynamic Software Cities / High-Rising Cities (incremental updates with no full rebuild, persistent anchor points, distant districts aggregating under LOD, animation throttling to dampen jarring transitions, drill-down), and the memory idiom from treemap/Panopticon viewers (box size = memory, heat = activity, real-time). Where we DIVERGE, and why this is our contribution: (1) Zoning is address-honest — buildings and districts sit at Hilbert(compacted real address), never a treemap/tree/force layout, so orientation is a reproducible property of the recording, not an aesthetic choice; a re-weave never relays out because identity IS the address cell. (2) The full D7 honesty regime none of the prior art enforces: sparse->sparse (empty land stays empty), statistical evidence lives on a physically separate fog surface that cannot merge into exact and does not scrub, truncation reads as rubble whose height is an explicit lower bound and whose banner survives the drill-in, undescribed-but-in-footprint is fog-of-war distinct from an observed zero, redacted stays withheld, and we refuse to reconstruct call/return structure the linear trace doesn't carry. (3) Two unfused time axes (terrain trace-time sun vs exec-step citizen) where the prior art collapses to one global clock. (4) An engine-free, golden-testable CityModel half mirroring the existing terrain model/view split, so the aggregation is unit-tested independently of GL — a testability seam absent from all the cited systems.

Sources:
[CodeCity (Wettel & Lanza)](https://wettel.github.io/download/Wettel07b-vissoft.pdf) ·
[SynchroVis — concurrency in the city metaphor](https://www.academia.edu/20483941/Synchrovis_3D_visualization_of_monitoring_traces_in_the_city_metaphor_for_analyzing_concurrency) ·
[Live Dynamic Software Cities with heat-map overlays](https://www.researchgate.net/publication/354949385_Live_Visualization_of_Dynamic_Software_Cities_with_Heat_Map_Overlays) ·
[High-Rising Cities: real-time performance](https://arxiv.org/pdf/1709.05768) ·
[Gource](https://gource.io/).

---

## 10. Provenance

Generated 2026-07-30 by a 17-agent grounded-ideation workflow (8 city-system
designers -> adversarial honesty+buildability critique -> synthesis) under
[CLAUDE.md](../../../CLAUDE.md)'s ultracode mode, informed by a web-research pass
over the software-city literature. Extends the existing 3D design
([docs/internal/gui/10-spacetime-3d-overview.md](../gui/10-spacetime-3d-overview.md),
[34-playhead-and-scene-reach.md](../gui/34-playhead-and-scene-reach.md)) and the
[3D visualization catalog](2026-07-29-3d-visualization-catalog.md). Sibling
analysis docs live in [`docs/internal/analysis/`](.).
