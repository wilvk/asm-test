# The causal layers — kernel crossings, taint spread, blame convergence, the modal path

> **Sources.** The [3D catalog](../analysis/2026-07-29-3d-visualization-catalog.md)
> §5 layers 5 (crossing spurs), 6 (taint isochrone), 11 (blame convergence forest)
> and 14 (dominant-path ridge); cut by
> [53-3d-catalog-build-roadmap.md](53-3d-catalog-build-roadmap.md) §4.1 (L5, L6,
> L11, L14). Read [_conventions.md](../implementations/_conventions.md) first;
> D1–D11 live in this directory's [README](README.md).
>
> **Prerequisites.** T2 needs [54](54-3d-catalog-phase0-plumbing.md) **T3**
> (`Event::seq` on `SyscallRow`); T3 needs [54](54-3d-catalog-phase0-plumbing.md)
> **T5** (`dt_walk_depth`). T4 and T5 have no prerequisite. T1 is shared by all
> four and is the first task for that reason. Layer registration assumes
> [56](../archive/gui/56-fidelity-and-module-layers.md) T1 has landed; if it has not, register the
> bools the old way and migrate.
>
> Authored 2026-08-02 against HEAD `b657876`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Citations re-verified and corrected 2026-08-03** (this rule, applied). Six
> of the file:line references above and below pointed at the wrong lines and
> have been repointed against the tree as this brief leaves it: `BlameAttr` is
> `streams.h:164-171` (was 152-159), `TrajPoint` is `types.h:58-75` (was
> 51-68), `access_spurs_` is `scene.h:431` (was 187 — it was already 371 before
> this brief), `resolve_pick`'s `link.step = pv.t` is `pick.cpp:327` (was
> 136-139), `Anchor` is `projection.h:46-57` with `place` at `:56`, and
> `placement_chips` is `hud.h:32-37`. The SUBSTANCE of every claim those
> citations support was verified against the code and holds — only the line
> numbers were wrong.
>
> **Status — ✅ 5/5 landed 2026-08-03.**
> T1 landed as `desktop/src/space/stepplace.{h,cpp}` — a THIN ADAPTER over
> [50](50-two-way-brushing.md)'s `space::StepAddrResolver`
> (`desktop/src/space/locate.h`), not a second resolver. 50 landed first, so
> per this brief's own risk note 57 adopts: the rbase/anchor resolution order,
> the anchor caching and every refusal reason come from `locate.cpp` verbatim,
> and `StepPlacer` adds only the plane coordinates, the region and the running
> miss count the HUD chip needs. Three signature deviations from the sketch
> below, each recorded there: `why` is a `std::string` (the refusal reasons are
> the anchor's own, built at runtime — a `const char *` would dangle), the
> constructor takes no `Anchor` (`StepAddrResolver` already derives and caches
> one; a second would be the parallel source of truth T1 exists to remove), and
> `at()` is non-const (a miss mutates the count — that is the point).
>
> **T2** landed as `space/crossing.h` (the POD geometry scene3d/ consumes) +
> `views/crossing.{h,cpp}` (the builder, which needs a `SyscallView`) — the
> same type/builder split 56 T5 made for `MispredLayer`/`build_mispred_layer`,
> and its own TU rather than an addition to `syscalls.cpp` because that object
> rides in the `DESKTOP_OBS_PURE` bundle a dozen link lines pull in. The draw
> half is `scene3d/causal.cpp`, a new TU defining `Scene` methods declared in
> `scene.h`, behind an opaque `CausalGL` pimpl: the whole four-layer family
> costs `scene.cpp` two lines (a `draw_causal()` call and a `free_causal()`
> call) and `scene.h` one contiguous block, which is what keeps this brief out
> of the way of the four sibling briefs editing those files at the same time.
>
> **Scope landed against T2's steps.** Steps 1-5 in full. Step 6 (pick →
> the syscalls row) landed as far as the DATA — every spur carries
> `CrossingSpur::row`, the drill-in target, and the layer test pins it — but
> the pick-pass wiring (a fifth `PickBands` band, decoded in `pick.cpp` and
> routed in `shell.cpp`) is NOT in this landing. It would mean reworking the
> shared pick id space and its band decode, which is exactly the kind of edit
> four concurrent briefs cannot resolve, and 56 T5 set the same precedent for
> `MispredLayer` (drawn, not yet pickable). Stated here rather than quietly
> dropped.
>
> **Two rendering deviations, both forced and both stated.** (a) Line width:
> 55 T6 is concurrently removing every `glLineWidth(>1.0)` call because it is
> invalid on a core forward-compatible context, so `causal.cpp` adds none. The
> "thickness by `row.payload.size()`" channel rides on the rail glyph's
> POINT SIZE instead (portable), and the spur line itself is a draw that WANTS
> a heavier stroke — listed here for 55 T6 step 1's quad-expansion work.
> (b) Hatching a `record_redacted` spur is real DASHED GEOMETRY, not the
> `uStipple` shader mode: the stipple is this family's stated STATISTICAL
> mark, and reusing it for "withheld at record time" would make one idiom
> carry two unrelated claims.
>
> **The line-width wants this brief is parking**, for
> [55](55-scene-render-quality.md) T6 step 1's quad-expansion pass to pick up.
> Every line `scene3d/causal.cpp` draws is at width 1.0; these are the draws
> that would read better wider, in descending order of how much it costs them:
>
> | draw | wanted | why |
> |---|---|---|
> | T5 ridge segments | width ∝ `log1p(count)` | the ridge is described as a *raised tube*; at 1px the transition count reads only through the lift and the brightness band |
> | T2 crossing spurs | ~2px, and ∝ payload bytes | the payload channel currently rides on the rail glyph's point size instead |
> | T5 cap brackets, T4 sink rings / born-untraced brackets | ~2px | four small line idioms have to stay distinguishable from each other at camera distance |
> | T3 hollow rings, escape crosses, fray ticks | ~2px | same reason: three idioms, one plane, all currently 1px |
>
> None of these is a fidelity problem — every one of them still states its
> quantity through a channel that works at 1px (a colour band, a point size, a
> lift). They are legibility wants.
>
> **T3** landed as `space/taint.{h,cpp}` (pure, engine-free) + its geometry in
> `scene3d/causal.cpp`. It is EXACT-ONLY BY TYPE: the builder's input is a
> `DataflowStream`, so a `SurveyEdge` has physically nowhere to enter — D7
> invariant 1 as a property of the signature rather than a rule to remember.
> Generation comes from `dt_walk_depth` (54 T5), never `dt_slice_forward`.
>
> **One deliberate narrowing of step 2's selector, and why.** The brief says
> to tint a reached step's memory write where `write && space in {"abs",
> "off"}`. `"abs"` places. An `"off"` record is region-RELATIVE, and the wire
> states no base for *data* — `df_step.rbase` is the CODE base, so placing a
> data offset against it would be a fabrication of a different kind from the
> one step 3 forbids. Those writes are therefore counted
> (`TaintFront::off_relative_writes`) and stated in the HUD rather than
> placed, which is exactly what `observed_data_spans` (`space/projection.h`)
> already does with the same records for the same reason.
>
> **The "not reached" vs "not recorded" distinction is a POSITIVE mark**, per
> the brief's own warning that an implementation loses it first because both
> render as nothing. `TaintReach::unknown_beyond` marks the placeable cell the
> front runs OFF into an undescribed step, drawn as radial fray ticks in a
> deliberately off-ramp grey-violet (it is not a distance, so it is not on the
> distance ramp), and a `bounded` walk's own rim frays identically because it
> is the same claim. Depth is a HUE and never a height: giving the def-use
> generation the vertical would make two quantities share one axis (46 G8) and
> would silently fuse two clocks 34 left unfused.
>
> **T4** landed as `space/blameforest.{h,cpp}` + geometry in
> `scene3d/causal.cpp`. Weight is keyed by STEP, not by cell, and two distinct
> steps that project into the same cell keep SEPARATE weights — summing them
> would manufacture a convergence that no single step has, which is the one
> arithmetic mistake here that would look right. A renderer wanting a per-cell
> brightness takes the MAX over a cell's entries, and the header says so.
> The beacon rides on its own channel (point size + brightness at plane
> level), never on terrain height, per step 3 and 46 G8.
>
> **A `cone[]` entry is a `{step, off, kind}` OBJECT, not a bare integer** —
> `doc/streams.cpp`'s decoder reads `c["step"]`, matching the schema's own
> `blame` example. A fixture that wrote bare integers silently decoded every
> cone entry as step 0, which is worth recording because it produces a
> *plausible* forest (one huge spike at step 0) rather than an obvious
> failure.
>
> **Step 4's drill-in** is data-only in this landing, on the same terms as
> T2's: every producer carries its `step` and every sink its `step`/`off`, but
> the pick-pass wiring is deferred (see T2's note).
>
> **T5** landed as `space/ridge.{h,cpp}` (the exact ridge) +
> `views/hotedges.cpp`'s `build_ridge_survey` (the survey fallback, returning
> its own `space::RidgeSurvey`) + geometry in `scene3d/causal.cpp`. **Never
> blended** is structural, not a convention: two builders over two inputs
> returning two types into two `SceneFrame` fields and two GL buffers. The
> exact builder physically never sees a survey; the survey builder never sees
> the trace.
>
> **The greedy rule the brief warns about is refused in two places, not one.**
> `2026-07-17-blockstep-reconstruction-defects.md` records this tree shipping a
> static successor guess twice with the rule in front of it, and notes that the
> emulator-replay tier is immune "because it does not statically guess the
> terminator". This builder is immune the same way. A transition exists only
> when the recorded instruction stream actually went from one recorded block
> start to another. Additionally — and this is a hazard the brief does not
> name — an instruction is **never attributed to a block by "the greatest
> recorded block start below it"**: that containment guess needs block LENGTHS
> the wire does not carry, and it would fabricate membership for an instruction
> in a block the recording never opened. Instructions before the first recorded
> block start are counted (`unattributed_insns`). A recording with instructions
> but NO `coverage` blocks is refused outright rather than having its block
> boundaries inferred.
>
> **Tie-dimming is a pure function** (`space::ridge_brightness`, with
> `kRidgeSplitBrightness` a named constant) and segments are BUCKETED by it in
> the uploader, so a 51/49 fork and a 99/1 fork land in different GL buffers
> and cannot be drawn alike even by accident.
>
> **Landed caveat on `coverage`.** A `coverage` event must state its own
> `basis`: `decode_streams` runs `note_basis` over it exactly as over a
> `trace` event, and omitting it sets `TraceStream::basis_error`, which this
> layer (correctly) treats as a refusal. Worth knowing when hand-writing a
> fixture.

## Why this work exists

[56](../archive/gui/56-fidelity-and-module-layers.md)'s four layers describe *state*: how much a
place is trusted, how hot it is, what kind of work it does. These four describe
*cause* — where control left the process, where a value went, what a value came
from, and which way a fork usually goes. They share one mechanic and one hazard.

**The mechanic.** All four have a step or an event in hand and need a *place*. The
recording carries the bridge: `DataflowStream::insn_off[step]` plus
`insn_rbase[step]` ([streams.h:68-76](../../../desktop/src/doc/streams.h#L68)) give
a step's address; `Projection::project` maps an address to a cell.

**The hazard.** That bridge is easy to skip, and skipping it is the documented
trap. [46](46-3d-functional-roadmap.md) §4 states the rule once for the whole 3D
family — *cross-axis brushing goes through the ADDRESS, never through an ordinal* —
and its G10 shows the rule already being broken in the shipped code
(`resolve_pick` sets `link.step = pv.t`, a per-tid vertex counter,
[pick.cpp:327](../../../desktop/src/scene3d/pick.cpp#L327)). Four new layers
each doing their own step→place conversion is four new chances to repeat it. T1
makes there be one conversion.

## What already exists (verified 2026-08-02 against `b657876`)

- **The step→address bridge is real data, per step.** `insn_off` and `insn_rbase`
  with `rbase_present` saying whether the wire stated a base
  ([streams.h:68-76](../../../desktop/src/doc/streams.h#L68)); `Anchor::place`
  ([projection.h:46-57](../../../desktop/src/space/projection.h#L46)) is the
  existing rel→abs resolver and **returns false rather than guessing**, so a miss
  is countable.
- **`Streams::blame` is decoded.** `BlameAttr{step, off, has_loc, loc, cone,
  born_untraced}` ([streams.h:164-171](../../../desktop/src/doc/streams.h#L164)),
  where `cone` is *"ascending producing steps (sink included)"* and
  `born_untraced` is the explicit verdict that a value has no traced producer —
  never an empty cone presented as "nothing found".
- **`dt_slice_forward` gives reachability without depth**
  ([slice.h:51-54](../../../desktop/src/analysis/slice.h#L51)); the BFS-depth walk
  T3 needs is [54](54-3d-catalog-phase0-plumbing.md) T5.
- **`TraceStream` carries the ordered instruction stream and de-duplicated
  blocks** ([streams.h:48-51](../../../desktop/src/doc/streams.h#L48)) — ordered
  `insns`, ascending `blocks`, plus per-offset `disasm`.
- **The worldline geometry T2 hangs spurs on already exists.**
  `TrajectorySet`/`TrajPoint{t, addr, fidelity, is_access, tid, placed}`
  ([types.h:58-75](../../../desktop/src/space/types.h#L58)), and `placed` already
  distinguishes a vertex that could be positioned from one that could not.
- **`access_spurs_` is the precedent for a spur layer**: all spurs in one
  `GL_LINES` buffer ([scene.h:431](../../../desktop/src/scene3d/scene.h#L431)),
  drawn from the trajectory program.
- **`ValRec` carries what a taint front needs**: `space` (`"reg"`/`"abs"`/`"off"`),
  `addr`, `write`, `value_valid`
  ([streams.h:31-45](../../../desktop/src/doc/streams.h#L31)).

## Tasks

### T1 — ☑ One step→place resolver, with its misses counted (M)

**Goal.** Every layer in this brief converts a step to a cell through one
function, and that function reports what it could not place.

**Steps.**
1. New pure helper in `space/` (engine-free, D4) — landed as
   `desktop/src/space/stepplace.{h,cpp}`, with the three sketch deviations
   the status banner records marked inline:
   ```
   struct StepPlace {
       bool placed = false;
       uint64_t addr = 0;     // the resolved ABSOLUTE address
       float u = 0, v = 0;    // plane coordinates, valid iff placed
       uint32_t cell = 0;
       const Region *region = nullptr;
       std::string why;       // when !placed: which resolution step failed
                              // (a std::string, not a const char *: the
                              // reasons are Anchor's own, built at runtime)
   };
   class StepPlacer {                 // built once per weave, reused per layer
     public:
       // No Anchor parameter: StepAddrResolver (50 T1) already derives and
       // caches it from proj.regions, and a caller-supplied one would be a
       // second source of truth.
       StepPlacer(const Projection &, const DataflowStream &);
       StepPlace at(uint32_t step);   // non-const: a miss mutates the count
       uint64_t unplaced() const;     // running count, for the HUD chip
       const std::string &note() const;
   };
   // Three of the four layers also place addresses the recording states
   // directly (a ValRec::addr, a block start), which have no step rung:
   StepPlace place_address(const Projection &, uint64_t addr);
   ```
2. **The resolution order is the recording's own**, and each rung is a stated
   fact rather than a fallback guess: (a) `insn_rbase[step] != 0` → `rbase + off`
   (37's on-the-wire region tag); (b) otherwise the `Anchor` (36's single-codeimage
   derivation) via `Anchor::place`, which already refuses rather than guessing
   ([projection.h:50-56](../../../desktop/src/space/projection.h#L50)); (c)
   otherwise unplaced, with `why` naming which rung failed.
3. **A miss is counted, never dropped.** `unplaced()` feeds a HUD chip on every
   layer that uses the placer — "N steps off-plane" — using the wording
   `placement_chips` already establishes
   ([hud.h:32-37](../../../desktop/src/scene3d/hud.h#L32)) rather than new phrasing
   (D7 / [24](../archive/gui/24-one-visual-language.md)).
4. **Never invert through an ordinal.** The header must say, in these words, that
   `TrajPoint::t` is a per-tid vertex counter
   ([trajectory.cpp:99](../../../desktop/src/space/trajectory.cpp#L99)) and is not
   interchangeable with a dataflow step index — the mismatch
   [46](46-3d-functional-roadmap.md) G10 found already shipped in the other
   direction. If [50](50-two-way-brushing.md) has landed, use its resolver instead
   of adding a second; if it has not, write this one so 50 can adopt it. **50 had
   landed**, so `StepPlacer` delegates every resolution rung to
   `space::StepAddrResolver` and re-derives nothing.

**Tests.** New `test_stepplace.cpp`: an `rbase`-carrying step resolves through the
wire base; a step with no rbase and a single codeimage resolves through the anchor;
a step with no rbase and two codeimage spans is unplaced with the anchor's own
refusal reason; an offset past the span's length is unplaced with the clamp reason;
`unplaced()` counts each of those exactly once. Assert the resolver never returns a
cell for an unplaced step (no cell 0 fallback — the single most likely bug here).
Landed with all of the above, plus: re-querying one unplaceable step does not
double-count (`unplaced()` counts DISTINCT steps, never calls — a chip that said
"2" for one bad step would be a fabricated quantity), an out-of-range index is
counted, and `place_address` refuses an unmapped address without a cell.

**Done when.** One resolver exists, its misses are countable, and no layer in this
brief converts a step to a place any other way.

### T2 — ☑ Crossing spurs on the worldline (M) · *needs [54](54-3d-catalog-phase0-plumbing.md) T3*

**Goal.** Where control actually left userspace, shown *on the path being read*
rather than in a separate flat list.

**Steps.**
1. Additive layer on the existing trajectory geometry — no new axes. For each
   syscall event, anchor to the last trace instruction with a smaller `seq`
   (the field [54](54-3d-catalog-phase0-plumbing.md) T3 adds). From that worldline
   vertex a spur shoots off the address plane to a thin floating **kernel rail**;
   a return spur comes back to the resume vertex.
2. **Out and return meet at the rail with no span between them.** Kernel dwell is
   not measured by anything in the recording, so the geometry must not imply a
   duration — that is why the rail is at a constant height and the two spurs meet.
   The HUD says "kernel dwell not measured".
3. Hue by a derived syscall class; thickness by `row.payload.size()` when
   `has_payload`; the return spur tinted by a parsed outcome. **Every parser
   buckets to a visible "other"/grey on any miss** — never folded into a known
   class, never green-on-unknown.
4. Reuse the per-tid trajectory colouring and the `access_spurs_` single-buffer
   pattern ([scene.h:431](../../../desktop/src/scene3d/scene.h#L431)).
5. **Self-gate.** With no `TraceStream` worldline there is nothing to hang a spur
   on: disable the layer and have the HUD say why. Never synthesise a path to
   decorate.
6. Drill-in: pick → the syscalls row, payload still redacted.

**Fidelity.** The anchor is labelled **"approx (last insn)"** and drawn hollow
where the trace is sparse or truncated — it is the nearest recorded instruction,
not a measured crossing point. A `record_redacted` spur is hatched *"withheld at
record time"* and its content is never rendered, in 3D or in the drill-in.

**Tests.** `test_obs_syscalls.cpp` + a layer builder test: a syscall whose `seq`
falls between two trace instructions anchors to the earlier one; a syscall before
any trace instruction is unplaced and counted, not anchored to instruction 0; an
unparsed class and an unparsed return both land in the "other" bucket;
`record_redacted` rows produce hatched spurs whose payload text never appears in
the golden output. With `seq_present == false`, the layer self-disables with a
stated reason.

Landed as `desktop/test/test_crossing.cpp`, with all of the above plus: the
anchor carries the anchoring instruction's own per-tid vertex ordinal (so the
spur hangs on the vertex the worldline really drew), `rail_span() == 0` is
asserted by name (deleting the meet-at-one-point property fails a check rather
than silently reintroducing a fabricated duration), a truncated recording draws
its anchors hollow, and a recording with no `trace` worldline self-disables.
The `seq` coverage the brief asks of `test_obs_syscalls.cpp` already landed
with 54 T3 and is unchanged. The payload-leak check is a blunt negative over
`crossing_layer_dump()` rather than a committed golden — the layer needed no
golden corpus file, so none was regenerated.

**Done when.** Kernel crossings appear in situ, claim no duration, and never
anchor by fabrication.

### T3 — ☑ Taint isochrone: the forward-spread front (M) · *needs [54](54-3d-catalog-phase0-plumbing.md) T5*

**Goal.** From a chosen definition, how far and where a value spreads across the
plane — and whether it escapes into a different kind of region.

**Steps.**
1. Compute generation with `dt_walk_depth(edges, nsteps, origin, forward=true, …)`
   from [54](54-3d-catalog-phase0-plumbing.md) T5 — **first-reach BFS depth**, not
   `dt_slice_forward`'s flat set, which carries no depth at all
   ([slice.h:51-54](../../../desktop/src/analysis/slice.h#L51)).
2. Tint each reached step's **memory-write** `ValRec` (`write && space in
   {"abs","off"}`) at its `addr` → cell, coloured by BFS depth on a near-hot ramp.
3. **A register-only write has no cell and tints nothing.** `space == "reg"`
   carries no address; colouring cell 0 for it is the archetypal fabricated
   placement.
4. An **escape glyph** where a reached cell's region kind differs from the
   origin's — this is the layer's actual finding, and it is a comparison of two
   recorded region kinds, not an inference.
5. **The front advances on the def-use generation**, i.e. the execution-step
   playhead — *never* the terrain residency slice. Two axes, unfused
   ([34](../archive/gui/34-playhead-and-scene-reach.md)); a taint front that moved
   with `hud.t` would be claiming a temporal correspondence the recording does not
   state.
6. Seed from `Streams::blame` where present; otherwise from the current
   `Selection`. Drill-in: reached cell → slice at the writing step; origin →
   blame; escape cell → slice at the store.

**Fidelity.** `steps_missing` / `!has_step()` regions of the walk are **unknown
gaps in the front**, never "not reached" — the difference between *the value did
not go here* and *we did not look* is the whole point of the layer.
`value_valid == false` spreads as a **hollow** route (the flow was observed, the
value was not). Under truncation the front carries the slice view's lower-bound
banner. Exact only: `SurveyEdge` never feeds this.

**Tests.** `test_slice.cpp` + a layer test: a diamond dataflow gives first-reach
depth; a register-only write produces no tinted cell (assert the cell set is
empty, not that cell 0 is untinted); a `value_valid == false` record produces a
hollow mark; a `steps_missing` gap renders as unknown rather than closing the
front; a `bounded` walk frays its rim.

Landed as `desktop/test/test_taint.cpp` with all five, plus: an escape is a
comparison of two RECORDED region kinds (and is never marked when either side's
kind is unknown), the front CONTINUES past a gap rather than closing at it (the
edges through the gap are recorded even where the step body is not), a truncated
recording carries the lower-bound fact, and both refusals (no dataflow pass, an
out-of-range origin) state a reason and emit no geometry. `test_slice.cpp`'s own
`dt_walk_depth` coverage landed with 54 T5 and is unchanged.

**Done when.** The front shows distance, marks escapes, and distinguishes "did not
spread here" from "not recorded".

### T4 — ☑ Blame convergence forest (M)

**Goal.** Across all attribution cones in a recording, which producing step is the
shared root cause many sinks trace back to.

**Steps.**
1. For each `BlameAttr` in `Streams::blame`
   ([streams.h:164-171](../../../desktop/src/doc/streams.h#L164)): place the sink
   at `off` → cell (through T1's placer), and place each step in `cone[]` the same
   way.
2. A producer cell's **convergence weight is the count of distinct cones whose
   `cone[]` contains that step** — a set-overlap count over recorded data, with no
   edge synthesised between cones.
3. Render as a beacon or brightness **on its own channel**, not as terrain height:
   height already encodes access density and overloading it would make two
   quantities share one axis, which is the exact complaint
   [46](46-3d-functional-roadmap.md) G8 raises about the vertical.
4. Drill-in: convergence spike → slice at the shared step; sink → blame; producer
   cell → disasm.
5. **Degrade honestly.** The layer is only rich when a recording blames several
   sinks; with one it is a faint single bundle and should look like one rather than
   like a finding.

**Fidelity.** A `born_untraced` cone is **the sink alone** — it must never produce
a shared spike, because a value with no traced producer converging with another
would be a pure artifact. Under truncation every cone is a lower bound and a
missing convergence is *not seen*, reusing the slice view's existing banner rather
than new wording. Exact blame only.

**Tests.** A builder test: two cones sharing one step give that step weight 2 and
every other step weight 1; a `born_untraced` cone contributes only its sink and
never raises another cell's weight; an unplaceable `off` is counted by T1's placer
and emits no beacon; a single-cone recording produces no spike above the baseline.

Landed as `desktop/test/test_blameforest.cpp` with all four, plus: a step
repeated WITHIN one cone counts once (weight counts distinct CONES, not cone
entries), the `born_untraced` check is adversarial — the fixture's untraced cone
deliberately names another cone's step and must still not raise it — and
truncation rides as a stated lower bound.

**Done when.** Shared root causes are visible, and a convergence is always a real
set overlap.

### T5 — ☑ Dominant-path ridge (M)

**Goal.** At each fork, which successor control usually takes — and how much mass
leaves the other way.

**Steps.**
1. Aggregate consecutive-block transitions from `TraceStream::insns`/`blocks`
   ([streams.h:48-51](../../../desktop/src/doc/streams.h#L48)) into a
   per-block successor histogram. Thread a raised tube through the `project()`ed
   block cells in program-visit order, height = `log(transition count)`.
2. **Tie-dimming is mandatory, not cosmetic.** `modal_fraction → 1` renders solid
   and bright; `→ 0.5` renders dim and visibly split. A 51/49 fork and a 99/1 fork
   must not look alike — that is the whole difference between a backbone and a
   coin toss.
3. A fork glyph on the plane sized by the leaving mass (`1 - modal_fraction`).
4. **Frame it as an aggregate, and say so in the label**: *"modal path
   (aggregate)"*. It is not a claim that any single run followed the whole chain.
   This is the documented greedy-reconstructor trap
   ([2026-07-17-blockstep-reconstruction-defects.md](../analysis/2026-07-17-blockstep-reconstruction-defects.md)),
   which both static reconstructors in this tree shipped before it was caught — the
   one prior mistake in this repo most likely to be repeated by this task.
5. A block whose successor was never recorded **caps the ridge "unknown
   continuation"** — never wraps to offset 0 and never joins to the next block by
   address adjacency.
6. Survey fallback (the max-count outgoing `HotEdge` per block) is drawn in
   statistical ink, on the statistical layer's terms, and never blended with the
   exact ridge.
7. Drill-in: segment → canvas/disasm at the offset; fork → hotedges at
   `off = block_addr` for the exact successor split.

**Fidelity.** Aggregate, not a path. A truncated trace's counts are a stated lower
bound. An unrecorded successor is an unknown cap. The survey fallback is separate
ink.

**Tests.** A builder test: a loop body with a 90/10 exit produces one bright
segment and one dim one with the documented fractions; a 50/50 fork renders both
at the split threshold; a block with a single recorded successor and a block with
none produce visibly different geometry (the second capped); the label text
contains "aggregate"; the survey fallback never appears in the exact geometry
buffer.

Landed as `desktop/test/test_ridge.cpp` with all five (the last one structurally
— the survey has its own type, built by a function in another TU that is not
even on the exact test's link line), plus: a self-loop is recorded as the real
observed transition it is rather than dropped, instructions before any recorded
block are counted rather than attributed by containment, a block with one
observed successor is NOT reported as a fork, brightness is monotonic and a 99/1
fork differs from a 51/49 one by a pinned margin, and both refusals (no
instruction stream, no recorded blocks) state a reason and emit no geometry.

**Done when.** Fork bias is readable, and nothing in the layer or its labels
claims an observed path.

## Fidelity notes (D7)

- **These four are the layers most able to fabricate causality**, which is why
  each carries an explicit "this is not that" clause: T2's spur is an *approximate
  anchor*, not a measured crossing; T3's front is *def-use generation*, not time;
  T4's weight is a *set overlap*, not a link; T5's ridge is a *per-fork
  aggregate*, not a run.
- **"Not reached" vs "not recorded" is the distinction T3 exists to preserve** and
  the one an implementation will lose first, because both render as nothing by
  default. The gap must be a positive mark.
- **`born_untraced` is a verdict, not an absence** — T4 must not let it interact
  with any other cone.
- All four use T1's placer and therefore all four surface an off-plane count. A
  layer that silently draws less than the data contains is worse than one that
  refuses.

## Effort and risk

Five medium tasks. Risks:

- **T1 collides with [50](50-two-way-brushing.md)** by design — both are the
  address-first resolver [46](46-3d-functional-roadmap.md) §4 mandates. Whichever
  lands first owns it; the second must adopt rather than duplicate. Check before
  starting.
- **T5 has a known-repeated failure mode.** Read
  [2026-07-17-blockstep-reconstruction-defects.md](../analysis/2026-07-17-blockstep-reconstruction-defects.md)
  before writing the aggregation — the greedy rule it documents was shipped twice
  in this tree by implementations that had the rule in front of them.
- **T2 and T3 both depend on Phase 0** and cannot start before it. T4 and T5 can
  start immediately and are the sensible first cut of this brief.

**How the two named risks actually played out (2026-08-03).**

- **T1 vs [50](50-two-way-brushing.md).** 50 landed first, so 57 adopted:
  `StepPlacer` delegates every rung to `space::StepAddrResolver` and adds only
  the plane coordinates, the region and the miss count. No second resolver
  exists.
- **T5's greedy failure mode.** Refused in the builder, and the refusal is
  asserted by name in `test_ridge.cpp`. A second instance of the same family —
  attributing an instruction to a block by "the greatest recorded start below
  it" — was found while implementing and is refused too; see the status note.
