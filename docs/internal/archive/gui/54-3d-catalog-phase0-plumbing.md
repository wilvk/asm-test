# Phase 0 — the seven shared changes the 3D catalog is gated on

> **Sources.** §4 of the [3D visualization catalog](../../analysis/2026-07-29-3d-visualization-catalog.md)
> ("Phase 0 — shared plumbing"), its §9 open-limits list, and §2 of
> [53-3d-catalog-build-roadmap.md](53-3d-catalog-build-roadmap.md), which cuts
> this brief and records what has moved since the catalog was authored. Read
> [_conventions.md](../implementations/_conventions.md) first; D1–D11 live in this
> directory's [README](README.md).
>
> **Prerequisites: none.** Every task is a model, decode or schema change under
> `desktop/src/` plus one optional producer field (T6). No new third-party dep, no
> new engine link, no GL.
>
> Authored 2026-08-02 against HEAD `b657876`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Status — ✅ 7/7.** T3–T7 landed 2026-08-02; T1 (observed-data-span
> projection) and T2 (read/write prefix-sum split) landed the same day,
> completing the brief. T1 adds `space::observed_data_spans` (clustered,
> page-rounded, clipped against `existing`, capped-not-dropped) plus
> `Projection::data_span_note`, wired into `shell.cpp`'s scene weave right
> before `build_projection` and surfaced in the HUD beside `mem_note`; a
> recording with no `mem` and no `abs`-space dataflow value stays
> byte-identical (no existing golden carries either, so nothing churned).
> T2 adds `DataCell::cum_read_size`/`cum_write_size`, parallel prefix sums
> alongside the existing `cum_size`/`cum_rw`, with an access whose `rw` token
> is neither `"r"` nor `"w"` counted into the total and into neither
> direction. Both are tested in `test_projection.cpp`/`test_terrain.cpp`;
> `desktop-test` is green in full, including the GL `test_scene_fbo` smoke.

## Why this work exists

The catalog proposes 26 graphs and gates 12 of them behind seven enabling
changes. None of the seven is large; each unblocks two to five graphs; and doing
them first turns most of the rest of the family into pure render work with no
producer or model risk. That is the whole argument for this brief existing
separately: **the expensive thing is not any one of these changes, it is
discovering mid-layer that one of them was needed.**

The seven tasks are **mutually independent** — take any subset in any order. Two
agents can split this brief cleanly (a natural cut is T1+T2 / T3–T7).

One of them is not merely an enabler. See T1.

## What already exists (verified 2026-08-02 against `b657876`)

- **The plane is code-only, and nothing else can reach it.** `shell.cpp:921-924`
  builds the projection from `regions_from_codeimage(r)` alone
  ([shell.cpp:921](../../../desktop/src/ui/shell.cpp#L921)), and the `mem` scan
  drops any access whose `ea` no region maps
  ([terrain.cpp:397-399](../../../desktop/src/space/terrain.cpp#L397)).
- **The data model for data cells is complete and unreachable.** `DataCell`
  carries `{cell, steps, cum_size, cum_rw}`
  ([terrain.h:121-126](../../../desktop/src/space/terrain.h#L121)); `TF_READ`/
  `TF_WRITE` exist ([terrain.h:53-54](../../../desktop/src/space/terrain.h#L53));
  the rich rung fills them at [terrain.cpp:387-420](../../../desktop/src/space/terrain.cpp#L387).
  All of it is tested against hand-built projections and none of it fires from the
  UI.
- **`Event::seq` exists on every event** ([recording.h:74](../../../desktop/src/doc/recording.h#L74))
  — "its position in the STREAM, counted across every kind" — and `by_kind` is
  what loses it, which is why 08 added `seq` in the first place.
- **`SyscallRow` has no `seq`** ([syscalls.h:39-45](../../../desktop/src/views/syscalls.h#L39)).
  `index` is the row's position in the *view*, not the stream.
- **`SurveyEdge` is `{from, to, count, mispred}`** ([streams.h:142-145](../../../desktop/src/doc/streams.h#L142));
  **`HotEdge` is `{from_addr, to_addr, from, to, count, mispred, is_return, rank}`**
  ([hotedges.h:40-48](../../../desktop/src/views/hotedges.h#L40)) with the whole
  `HotEdgeView` fidelity channel beside it (`sampler`, `lost`, `throttled`,
  `have_window`, `window_base/len`, [hotedges.h:50-64](../../../desktop/src/views/hotedges.h#L50)).
- **`dt_slice_forward` returns a flat step set with no depth**
  ([slice.h:51-54](../../../desktop/src/analysis/slice.h#L51)) — `dt_slice` is
  `{steps, contains()}`. The generation walk that *does* exist,
  `loom_selection_t::generation` ([lineage.h:39](../../../desktop/src/loom/lineage.h#L39)),
  is Loom-fabric-coupled and takes `asmtest_defuse_edge_t` (an engine type), so
  it cannot serve a scene layer.
- **`dt_link` has the append-last precedent T6 needs.** `pid` is documented as
  *"Appended LAST in the textual form, so every link written before it stays
  byte-identical"* ([nav.h:64-68](../../../desktop/src/nav.h#L64)).
- **`ui/asm_language.h` is double-coupled**: `#include "TextEditor.h"` (line 26,
  the ImGuiColorTextEdit addon) and `#include "asmtest_assemble.h"` (line 29, an
  engine header). Its word-lists are the only mnemonic vocabulary in the tree.

## Tasks

### T1 — Observed-data-span projection extension (L)

**Goal.** A data address that the recording actually touched lands on a cell. This
is the change that makes `TerrainModel::data` reachable from the running app for
the first time.

**Steps.**
1. `space/projection.h`: add
   ```
   // Cluster the addresses a recording OBSERVED touching into compacted spans,
   // so an access that no codeimage region maps still places. Every span is
   // Region::Kind::Unknown and labelled "observed data" — the recording states
   // that these bytes were touched, and NOTHING about what they are.
   std::vector<Region> observed_data_spans(const Recording &rec,
                                           const std::vector<Region> &existing);
   ```
   Keep it in the pure, engine-free `space/` TU (`projection.cpp`) so it compiles
   into the viewer and the null harness (D4).
2. **The address sources, in this order**, each already decoded: raw `mem` events'
   `ea` (`rec.by_kind.at("mem")`, the same field
   [terrain.cpp:395](../../../desktop/src/space/terrain.cpp#L395) reads) and
   `DataflowStream::recs` `ValRec.addr` where `space` is `"abs"` (an `"off"`
   record is region-relative and must go through the anchor first, or be skipped
   with a counted reason — never placed raw).
3. **The clustering rule is the fidelity question of this task.** Sort the
   observed addresses; open a span at the first; extend it while the next address
   is within a gap threshold; close and start a new span otherwise. Then:
   - The threshold is a **stated constant with a named unit**, not a tuned magic
     number — start at one page (4096) and say so in the header, because a page is
     the granularity the OS actually allocates in and is therefore the smallest
     gap that is not evidence of a distinct object.
   - Round each span out to page boundaries, then **subtract any existing region**
     (the `existing` argument) so an observed address inside a known code region
     never creates a shadow span. Overlap is a precondition violation for
     `build_projection` ([projection.h:26-28](../../../desktop/src/space/projection.h#L26)).
   - Cap the span count and, when the cap bites, **merge the nearest neighbours
     and say so in a note** — never silently drop a span. A dropped span is an
     address that vanishes from the plane.
4. `space/projection.h`: add to `Projection` a `std::string data_span_note` — how
   many spans, from how many observed addresses, at what threshold, and whether
   the cap merged any. The HUD surfaces it exactly like `mem_note` already is.
5. `ui/shell.cpp:921`: append the observed spans to the region vector before
   `build_projection`. Keep the existing behaviour byte-identical when the
   recording has no `mem` and no `abs` `ValRec` — an empty span list must produce
   the same projection as today.

**Fidelity.** The spans are `Kind::Unknown` and labelled *observed data*, never
`Heap`/`Stack`/`Data`: no producer emits allocation kinds (`recording.cpp`'s kind
list has no `malloc`/`free`/`mmap`/`brk`), so naming one would be fabricated
structure (D7). The span is **an observed-touch extent, not an allocation
extent** — the real object may start before and end after, and the header must say
so in those words, because L9's pillars inherit the claim.

**Tests.** `test_projection.cpp`: clustering is deterministic and byte-stable for
a fixed address set; two addresses one page apart share a span, two a megabyte
apart do not; a span never overlaps an `existing` region; the cap merges rather
than drops, and the note says it did. `test_terrain.cpp`: with the spans in the
projection, a `mem` fixture places `DataCell`s where it previously placed none —
assert the count, not just non-emptiness. `test_shell.cpp`: a recording with no
`mem` produces the same projection as before this task.

**Done when.** A golden recording carrying `mem` produces a non-empty
`TerrainModel::data` through the real shell path, and `data_span_note` states how
the spans were derived.

### T2 — Split `DataCell::cum_rw` into read and write prefix sums (S)

**Goal.** "How much was read here" and "how much was written here" become separate
answerable questions. One re-aggregation of a scan that already runs.

**Steps.**
1. `space/terrain.h`, `DataCell`: add `std::vector<uint64_t> cum_read_size` and
   `cum_write_size`, parallel to the existing `steps`/`cum_size`. **Keep
   `cum_size` and `cum_rw`** — every existing reader stays byte-identical, and
   `cum_size == cum_read_size + cum_write_size` becomes a test invariant.
2. `space/terrain.cpp:410-420`: the accumulation loop already has the `rwbit` in
   hand ([terrain.cpp:400](../../../desktop/src/space/terrain.cpp#L400)); add two
   running totals beside the existing one. No new scan, no new event read.
3. Document in the header that an access recorded with neither bit set (a `rw`
   token that is neither `"r"` nor `"w"`) counts into `cum_size` but into
   **neither** direction — the "unknown is not zero" invariant made arithmetic,
   and the reason L7 renders an *absent* surface rather than a flat one.

**Tests.** `test_terrain.cpp`: for a mixed read/write fixture the two prefix sums
are each monotonic, sum to `cum_size` at every index, and the unknown-direction
case lands in neither. Assert the existing `cum_rw` assertions still pass
unchanged.

**Done when.** Both prefix sums exist, sum to the old one, and no existing terrain
test changed.

### T3 — Carry `Event::seq` through the syscall view (S)

**Goal.** A syscall can be placed in the execution stream, which is the only thing
that lets it be drawn *in situ* on a worldline instead of in a flat list.

**Steps.**
1. `views/syscalls.h`, `SyscallRow`: add `uint64_t seq = 0`. Document that it is
   `Event::seq` — the stream position across every kind
   ([recording.h:74](../../../desktop/src/doc/recording.h#L74)) — and **not**
   `index` (this view's own row number), because the two are both integers and
   both look like an order.
2. `views/syscalls.cpp`, `obs_syscalls_build`: copy `e.seq` when building each
   row. It is already in hand — `by_kind` holds the `Event`, `seq` is a member.
3. Add a `bool seq_present` to `SyscallView`: a recording whose events carry no
   meaningful seq (all zero) must be distinguishable from one at seq 0, or L5's
   spurs will anchor everything to the first instruction.

**Fidelity.** `seq` orders a syscall against a trace instruction; it does **not**
measure kernel dwell, and no consumer may present it as a duration. State that in
the header so the constraint travels with the field.

**Tests.** `test_obs_syscalls.cpp`: rows carry ascending `seq` matching the source
events; an all-zero-seq recording sets `seq_present = false`; existing golden text
is unchanged (`seq` is not rendered by the 2D view).

**Done when.** Every `SyscallRow` carries its stream position and the view says
whether that position is real.

### T4 — An engine-free, ImGui-free `mnemonic_class()` (M)

**Goal.** "What kind of instruction is this?" becomes answerable in the
render-only viewer, which is where the 3D scene lives.

**Steps.**
1. New TU `space/mnemonic.h` / `.cpp` (in `space/`, not `ui/`, because the
   consumer is the terrain builder and `space/` is already the engine-free,
   ImGui-free island — D4).
   ```
   enum class OpClass : uint8_t {
       Unknown = 0, Move, IntArith, Logic, CompareBranch, ScalarFloat,
       VectorSIMD, System,
   };
   // `guest` selects the vocabulary ("x86" | "arm64"); an unrecognised guest,
   // an empty token, or a token in no list ALL return Unknown.
   OpClass mnemonic_class(std::string_view first_token, std::string_view guest);
   const char *op_class_name(OpClass);
   ```
2. **Copy the word-lists, do not include the header.** `ui/asm_language.h` pulls
   in `TextEditor.h` and `asmtest_assemble.h` (lines 26 and 29) and cannot be
   depended on from `space/`. Copying is the correct call here and the tree has
   the precedent (`kindHue` in
   [embedded.h:54-61](../../../desktop/src/scene3d/shaders/embedded.h#L54)
   duplicates `region_style()` deliberately) — but copy it **with a test that pins
   the two together**, exactly as `test_asm_language` pins the comment rules
   against `stmt_rules()`.
3. **Ambiguity is data, not a coin flip.** `movd`/`movq`/`cvtsi2sd` legitimately
   sit between classes. Return the class *and* a `bool ambiguous` (or a
   `MnemonicClass` struct) so L3 can mark the cell rather than silently bucket it.
4. **Memory-touch is never derived here.** Whether an instruction touches memory
   comes from `ValRec.space` (`"abs"`/`"off"`), never from the mnemonic. Say so in
   the header; it is the single most tempting wrong inference in this file.

**Tests.** New `test_mnemonic.cpp`: a table of representative tokens per class per
guest; every token in `asm_language.cpp`'s lists classifies to something (the
anti-drift assertion); an unknown token, an empty token and an unknown guest all
return `Unknown`; the ambiguous set is exactly the documented one.
`test_view_presence.cpp` (or the viewer link check): assert the new TU is in the
render-only closure.

**Done when.** `mnemonic_class` links into `asmtest-viewer` and the drift test
fails if `asm_language.cpp` gains a mnemonic this file lacks.

### T5 — A client-side BFS-depth walk over `DataflowStream::edges` (M)

**Goal.** Def-use *generation* — how many hops from the origin — becomes available
to a scene layer. `dt_slice_forward` gives the reachable set and throws the depth
away.

**Steps.**
1. `analysis/slice.h`: add beside the existing pair
   ```
   struct dt_walk {
       std::vector<uint32_t> steps;   // ascending, deduped
       std::vector<int32_t> depth;    // parallel; first-reach BFS depth,
                                      // negative for the backward direction
       int32_t depth_min = 0, depth_max = 0;
       bool bounded = false;          // the walk hit max_depth, not the fixpoint
   };
   dt_walk dt_walk_depth(const std::vector<dt_edge> &edges, uint32_t nsteps,
                         uint32_t origin, bool forward, int32_t max_depth);
   ```
   Same TU, same origin/`nsteps` contract as `dt_slice_forward`
   ([slice.h:39-50](../../../desktop/src/analysis/slice.h#L39)) — an out-of-range
   origin yields an EMPTY walk, never `{origin}`.
2. **First-reach depth, breadth-first.** A step reachable at depths 2 and 5 has
   depth 2. Build the adjacency once, then BFS; do not recurse per node.
3. `bounded` is load-bearing: a walk stopped by `max_depth` is a **lower bound**
   and its rim must render frayed. A walk that reached the fixpoint is complete.
   The two must not be confusable, so `bounded` is a field, not an inference from
   `depth_max == max_depth`.
4. Assert (in the test, not at runtime) that `dt_walk_depth(..., forward).steps`
   equals `dt_slice_forward(...).steps` for the unbounded case — the new function
   must not quietly disagree with the old one about *reachability*.

**Tests.** `test_slice.cpp`: a diamond graph gives first-reach depth, not
last-reach; a cycle terminates; an empty edge set gives `{origin}` at depth 0; an
out-of-range origin gives `{}`; `max_depth` sets `bounded` and truncates; the
set-equality assertion against `dt_slice_forward` for several fixtures.

**Done when.** Depth is available for both directions and agrees with the existing
slice on membership.

### T6 — An optional invocation index on `dt_link` (S)

**Goal.** A pick can say "invocation #7 of this routine" — a coordinate `dt_link`
cannot express today.

**Steps.**
1. `nav.h`, `dt_link`: add `std::optional<uint32_t> invocation`, **appended LAST**
   in the textual form, exactly as `pid` was and for the identical reason
   ([nav.h:64-68](../../../desktop/src/nav.h#L64)) — every link written before this
   change stays byte-identical.
2. `nav.cpp`: parse and emit it in the same place `pid` is handled. An unknown key
   in a parsed link must keep behaving as it does today.
3. **Do not overload `step`.** `dt_link::step` is documented as *"a dataflow step
   index"* ([nav.h:62](../../../desktop/src/nav.h#L62)); an invocation number is a
   different axis and putting it there is the exact ordinal-conflation trap
   [46](46-3d-functional-roadmap.md) §4 forbids. The two may both be set.
4. Check the schema-freeze status in [asmtrace-schema.md](asmtrace-schema.md)
   before touching anything on the wire. **This task changes no wire format** —
   `dt_link` is an in-app deep link, not a recorded event — so D5 does not bite;
   confirm that reading rather than assuming it.

**Tests.** `test_nav.cpp`: round-trip with and without `invocation`; a link with
no invocation serialises byte-identically to today (golden); a link carrying both
`step` and `invocation` round-trips both; an unparseable value is rejected the way
`pid`'s is.

**Done when.** The field round-trips and every existing link's text is unchanged.

### T7 — Decide the survey-edge source: standardize on `HotEdge` (M)

**Goal.** One answer to "where do statistical control-flow edges come from",
before two layers implement two.

**Steps.**
1. **The decision, and it is `HotEdge`.** The catalog offers "extend `SurveyEdge`
   with `is_return`+`from_addr`, OR standardize on `HotEdge`". Take `HotEdge`,
   for three reasons that are facts about the tree rather than preferences:
   - It already carries everything both consumers need — `from_addr`, `to_addr`,
     `is_return`, `mispred`, plus resolved `from`/`to` symbol text
     ([hotedges.h:40-48](../../../desktop/src/views/hotedges.h#L40)).
   - The **fidelity channel a statistical layer is required to render** lives on
     `HotEdgeView`, not beside `SurveyEdge`: `sampler`, `samples`, `lost`,
     `throttled`, `have_window`, `window_base/len`
     ([hotedges.h:50-64](../../../desktop/src/views/hotedges.h#L50)). A layer
     sourced from `SurveyEdge` would have to reach for `HotEdgeView` anyway to
     obey invariant 1, so sourcing from `SurveyEdge` buys nothing and adds a
     second decode path.
   - The drill-in target for every statistical pick is the hotedges view itself,
     so the layer and its reader share one model — which is what stops the two
     from disagreeing about the same edge.
2. `views/hotedges.h`: add a pure accessor the scene can use — the layers need
   `{from_addr, to_addr, count, mispred, is_return}` and the view-level fidelity
   fields, nothing else. **No factoring is needed**: `hotedges.cpp`'s own banner
   is *"the pure builder + dump of hotedges.h. No ImGui, no I/O"*, the header
   names no ImGui type, and `hotedges` is already in `DESKTOP_OBS_PURE`
   ([mk/desktop.mk:526](../../../mk/desktop.mk#L526)) — so `space/` may depend on
   it directly and this task is small. Re-verify that before writing code; if it
   has since gained a draw half, factor the model out rather than including a draw
   TU from `space/`.
3. Record the decision in `doc/streams.h` beside `SurveyEdge` — a one-line comment
   pointing at `HotEdge` as the source for rendered survey edges, so the next
   reader of that struct does not re-derive the choice.
4. **Do not delete or change `SurveyEdge`.** It is the decoded wire form and other
   readers use it. This task chooses a *rendering* source, not a schema change.

**Fidelity.** Anything drawn from this source wears the `STATISTICAL — survey`
label and the `provenance_conflict` stamp, is never summed into an exact surface,
and grades its opacity by `sampler` (a crisp `ibs-op` vs a washed `sw-clock`).
That rule belongs in the accessor's header comment so it travels with the data.

**Tests.** `test_obs_hotedges.cpp`: the accessor returns the same edges the view
ranks, in the same deterministic order, with `is_return` preserved; the fidelity
fields survive; an empty survey yields an empty set and a stated reason rather
than a silent zero.

**Done when.** One documented source exists, it is reachable from an engine-free
TU, and `streams.h` points at it.

## Fidelity notes (D7)

- T1's spans say **observed touch**, never allocation. No producer emits
  allocation events; a span that claimed to be "the heap object" would be
  fabricated structure. Every consumer (L7–L9, L12) inherits this wording.
- T2's third direction — an access with neither read nor write recorded — is
  counted in the total and in neither direction. It must never be silently
  attributed to one, and L7 renders it as an absent surface.
- T3's `seq` orders; it does not measure. No consumer may derive a duration.
- T4 returns `Unknown` for anything outside its lists and flags the genuinely
  ambiguous mnemonics. A classifier that guesses is worse than one that abstains,
  because the 3D tint reads as a measurement.
- T5's `bounded` walk is a **lower bound** and every consumer must render it as
  one.
- T7 keeps the statistical source physically distinct from the exact one at the
  model layer, which is the cheapest place to make invariant 1 structural rather
  than a rendering convention.

## Effort and risk

Seven tasks: one large (T1), two medium (T4, T5), four small (T2, T3, T6, T7).
The two risks worth naming:

- **T1's clustering threshold is a judgement that shows up as geometry.** Too
  tight and a single array shatters into a dozen spans; too loose and unrelated
  objects merge into one. The mitigation is that the threshold is a named
  constant, the note states it, and the test pins the behaviour at two
  scales — not that the first value chosen is right.
- **T1 changes what every existing 3D test sees.** Adding regions changes the
  compacted domain, which changes the Hilbert order, which moves every cell — so
  terrain/projection/trajectory goldens will churn. That is correct behaviour, not
  a regression, but it means T1 should land in one commit with its golden
  regeneration and **not** be split across two agents' trees (see the shared-tree
  rule in [README](README.md#claiming-work-parallel-agents)).
