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
> **Status — ☐ 0/5, not started.**

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
[pick.cpp:136-139](../../../desktop/src/scene3d/pick.cpp#L136)). Four new layers
each doing their own step→place conversion is four new chances to repeat it. T1
makes there be one conversion.

## What already exists (verified 2026-08-02 against `b657876`)

- **The step→address bridge is real data, per step.** `insn_off` and `insn_rbase`
  with `rbase_present` saying whether the wire stated a base
  ([streams.h:68-76](../../../desktop/src/doc/streams.h#L68)); `Anchor::place`
  ([projection.h:45-56](../../../desktop/src/space/projection.h#L45)) is the
  existing rel→abs resolver and **returns false rather than guessing**, so a miss
  is countable.
- **`Streams::blame` is decoded.** `BlameAttr{step, off, has_loc, loc, cone,
  born_untraced}` ([streams.h:152-159](../../../desktop/src/doc/streams.h#L152)),
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
  ([types.h:51-68](../../../desktop/src/space/types.h#L51)), and `placed` already
  distinguishes a vertex that could be positioned from one that could not.
- **`access_spurs_` is the precedent for a spur layer**: all spurs in one
  `GL_LINES` buffer ([scene.h:187](../../../desktop/src/scene3d/scene.h#L187)),
  drawn from the trajectory program.
- **`ValRec` carries what a taint front needs**: `space` (`"reg"`/`"abs"`/`"off"`),
  `addr`, `write`, `value_valid`
  ([streams.h:31-45](../../../desktop/src/doc/streams.h#L31)).

## Tasks

### T1 — One step→place resolver, with its misses counted (M)

**Goal.** Every layer in this brief converts a step to a cell through one
function, and that function reports what it could not place.

**Steps.**
1. New pure helper in `space/` (engine-free, D4):
   ```
   struct StepPlace {
       bool placed = false;
       uint64_t addr = 0;     // the resolved ABSOLUTE address
       float u = 0, v = 0;    // plane coordinates, valid iff placed
       uint32_t cell = 0;
       const Region *region = nullptr;
       const char *why = "";  // when !placed: which resolution step failed
   };
   class StepPlacer {                 // built once per weave, reused per layer
     public:
       StepPlacer(const Projection &, const DataflowStream &, const Anchor &);
       StepPlace at(uint32_t step) const;
       uint64_t unplaced() const;     // running count, for the HUD chip
       const std::string &note() const;
   };
   ```
2. **The resolution order is the recording's own**, and each rung is a stated
   fact rather than a fallback guess: (a) `insn_rbase[step] != 0` → `rbase + off`
   (37's on-the-wire region tag); (b) otherwise the `Anchor` (36's single-codeimage
   derivation) via `Anchor::place`, which already refuses rather than guessing
   ([projection.h:51-55](../../../desktop/src/space/projection.h#L51)); (c)
   otherwise unplaced, with `why` naming which rung failed.
3. **A miss is counted, never dropped.** `unplaced()` feeds a HUD chip on every
   layer that uses the placer — "N steps off-plane" — using the wording
   `placement_chips` already establishes
   ([hud.h:29-34](../../../desktop/src/scene3d/hud.h#L29)) rather than new phrasing
   (D7 / [24](../archive/gui/24-one-visual-language.md)).
4. **Never invert through an ordinal.** The header must say, in these words, that
   `TrajPoint::t` is a per-tid vertex counter
   ([trajectory.cpp:99](../../../desktop/src/space/trajectory.cpp#L99)) and is not
   interchangeable with a dataflow step index — the mismatch
   [46](46-3d-functional-roadmap.md) G10 found already shipped in the other
   direction. If [50](50-two-way-brushing.md) has landed, use its resolver instead
   of adding a second; if it has not, write this one so 50 can adopt it.

**Tests.** New `test_stepplace.cpp`: an `rbase`-carrying step resolves through the
wire base; a step with no rbase and a single codeimage resolves through the anchor;
a step with no rbase and two codeimage spans is unplaced with the anchor's own
refusal reason; an offset past the span's length is unplaced with the clamp reason;
`unplaced()` counts each of those exactly once. Assert the resolver never returns a
cell for an unplaced step (no cell 0 fallback — the single most likely bug here).

**Done when.** One resolver exists, its misses are countable, and no layer in this
brief converts a step to a place any other way.

### T2 — Crossing spurs on the worldline (M) · *needs [54](54-3d-catalog-phase0-plumbing.md) T3*

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
   pattern ([scene.h:187](../../../desktop/src/scene3d/scene.h#L187)).
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

**Done when.** Kernel crossings appear in situ, claim no duration, and never
anchor by fabrication.

### T3 — Taint isochrone: the forward-spread front (M) · *needs [54](54-3d-catalog-phase0-plumbing.md) T5*

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

**Done when.** The front shows distance, marks escapes, and distinguishes "did not
spread here" from "not recorded".

### T4 — Blame convergence forest (M)

**Goal.** Across all attribution cones in a recording, which producing step is the
shared root cause many sinks trace back to.

**Steps.**
1. For each `BlameAttr` in `Streams::blame`
   ([streams.h:152-159](../../../desktop/src/doc/streams.h#L152)): place the sink
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

**Done when.** Shared root causes are visible, and a convergence is always a real
set overlap.

### T5 — Dominant-path ridge (M)

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
