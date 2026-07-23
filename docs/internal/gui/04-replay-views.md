# Replay views over recordings (Phase 2 core, minus the Loom) — implementation

> **Sources.** Actioned from [desktop-gui-plan.md](../plans/desktop-gui-plan.md) — Phase 2
> ("the viewer, Author mode"), [D3 "Diff as a first-class primitive"](../plans/desktop-gui-plan.md#d3-diff-as-a-first-class-primitive),
> [D4 "The navigation spine"](../plans/desktop-gui-plan.md#d4-the-navigation-spine), and the
> view-catalog rows *Def-use slice explorer*, *Operand-value timeline*, *Trace canvas*.
> Written 2026-07-23. If this doc and a source disagree, this doc wins (sources may be
> stale); if the CODE and this doc disagree, re-verify before implementing.
>
> Set decisions D1–D11 in [README.md](README.md) bind this doc. Siblings consumed:
> [01-asmtrace-format.md](01-asmtrace-format.md) (schema + golden corpus),
> [02-exporters-and-readers.md](02-exporters-and-readers.md) (the `.asmtrace` reader),
> [03-desktop-shell.md](03-desktop-shell.md) (shell, `dt_recording` document model,
> `mk/desktop.mk`, headless harness). Siblings consuming this doc:
> [05-loom-day-one.md](05-loom-day-one.md) (diff alignment seam),
> [06-doors-and-learning.md](06-doors-and-learning.md) (test-failure deep-link producer).

## Why this work exists

Phase 2's acceptance is "a student on any OS opens a golden walkthrough and slices a wrong
value with no root or deps; two recordings diff." The shell (03) opens a recording; this
doc builds what the user then *looks at*: trace canvas, operand-value timeline, def-use
slice explorer (the plan's crown jewel), the two-recording diff primitive, and the
deep-link router every view and every future producer (06, 08) navigates through. Per plan
D5, replay slicing is computed **client-side from recorded defuse edges** — the render-only
viewer never links `libasmtest_dataflow` — so this doc also owns the C++ closure library
whose semantics must match the C slicer exactly. Nothing here exists yet (no `desktop/`
directory, no `asmtrace` make target — verified 2026-07-23); everything below is new code
under `desktop/src/` + `desktop/test/` (set D1).

## What already exists (verified 2026-07-23)

- [cli/asmspy_dataview.h](../../../cli/asmspy_dataview.h) — pure (no-ncurses, no-Capstone,
  header-only) view logic the timeline reuses:
  [`asmspy_df_annotate`](../../../cli/asmspy_dataview.h#L51) (token grammar + `*cur`
  cursor contract at [:39–50](../../../cli/asmspy_dataview.h#L39));
  [`asmspy_df_rowstyle`](../../../cli/asmspy_dataview.h#L112) decides NORMAL/IN-SLICE/
  DIMMED ([`asmspy_df_rowstyle_t`](../../../cli/asmspy_dataview.h#L106)) but **calls
  [`asmtest_slice_contains`](../../../cli/asmspy_dataview.h#L116)**, a `src/dataflow.c`
  symbol — NOT usable in the render-only binary (T1);
  [`asmspy_df_defuse_counts`](../../../cli/asmspy_dataview.h#L124) and
  [`asmspy_df_loc_str`](../../../cli/asmspy_dataview.h#L144) are pure inline, safe anywhere.
- [include/asmtest_valtrace.h](../../../include/asmtest_valtrace.h) — the dataflow types
  (plain structs; including it adds no link dep):
  [`at_val_rec_t`](../../../include/asmtest_valtrace.h#L61),
  [`asmtest_valtrace_t`](../../../include/asmtest_valtrace.h#L93) (honest overflow:
  [`truncated`](../../../include/asmtest_valtrace.h#L113) + `*_total` counters past caps),
  [`asmtest_defuse_edge_t`](../../../include/asmtest_valtrace.h#L171)
  `{from_step, to_step, loc}`,
  [`asmtest_defuse_t`](../../../include/asmtest_valtrace.h#L180) (`nsteps` bounds slices),
  [`asmtest_slice_t`](../../../include/asmtest_valtrace.h#L199), and the slice API
  ([:206–225](../../../include/asmtest_valtrace.h#L206)).
- [src/dataflow.c](../../../src/dataflow.c) — the C slicer to match:
  [`slice_dir`](../../../src/dataflow.c#L665) BFS; origin `>= nsteps` yields the **empty**
  slice ([:673–674](../../../src/dataflow.c#L673)); forward follows `from_step → to_step`,
  backward the reverse ([:689–690](../../../src/dataflow.c#L689)); endpoints `>= nsteps`
  ignored; result deduped + sorted ascending ([:709](../../../src/dataflow.c#L709)).
- [include/asmtest_trace.h](../../../include/asmtest_trace.h) —
  [`asmtest_trace_t`](../../../include/asmtest_trace.h#L44): ordered `insns[]` + deduped
  `blocks[]` ([:54–57](../../../include/asmtest_trace.h#L54)), `*_total` counters past
  caps, [`truncated`](../../../include/asmtest_trace.h#L59), and the base contract
  "offsets from the start of the registered routine, offset 0 = entry"
  ([:41–42](../../../include/asmtest_trace.h#L41)). Whole-window/absolute producers fill
  the same struct — why trace events carry a basis tag (01) and the canvas refuses mixes.
- [cli/test_view.c](../../../cli/test_view.c) — the test pattern to mirror: check helpers
  + `failures` counter ([:22–41](../../../cli/test_view.c#L22)), hand-built record
  builders ([:44–65](../../../cli/test_view.c#L44)), **real Capstone GP-container reg ids
  as literals** ([:77–80](../../../cli/test_view.c#L77)) — arbitrary small ids alias
  inside RAX after register canonicalization; reuse 35=RAX, 37=RBX, 38=RCX, 40=RDX.
- [mk/cli.mk](../../../mk/cli.mk) — build shapes to mirror in `mk/desktop.mk`: the
  pure-header test rule ([:301–303](../../../mk/cli.mk#L301) — `test_view` links
  `dataflow.o` for the C slicer), test aggregation ([:384–397](../../../mk/cli.mk#L384)),
  and [`docker-cli`](../../../mk/cli.mk#L407), which set D3 says `docker-desktop` mirrors.
- [Makefile](../../../Makefile) — the hand-maintained [`help`](../../../Makefile#L91)
  target; [`include mk/cli.mk`](../../../Makefile#L929) is the line `mk/desktop.mk`
  follows (03 owns that edit).
- [`asmtest_corpus_routine`](../../../bindings/conformance/corpus_routines.c#L32)
  (bindings/conformance/corpus_routines.c) names the corpus routines the goldens record
  ([`add_signed`](../../../bindings/conformance/corpus_routines.c#L35),
  [`read_fault`](../../../bindings/conformance/corpus_routines.c#L55), …).

**New symbols from siblings (do not invent — verify against the sibling before coding):**
`dt_recording`, `dt_app`, the headless harness, and the `desktop` / `desktop-render` /
`desktop-test` / `docker-desktop` targets are 03's; the `.asmtrace` reader filling
`dt_recording` is 02's; event field names (`kind`, `basis` = `"region"`|`"abs"`, `off`,
`step`, `disasm`, `truncated`, provenance/drop counters) plus `make asmtrace-golden` and
`tests/golden-asmtrace/` are 01's ([asmtrace-schema.md](asmtrace-schema.md)). This doc
needs `dt_recording` to expose: the trace stream (ordered insn offsets, deduped block set,
`basis`, `truncated`, totals), the dataflow stream (per-step `insn_off`, flattened
`at_val_rec_t`-shaped records, defuse edges, truncation), optional per-offset `disasm`
(D10), hot-edge survey entries (statistical provenance), and the metadata header (routine
name + code-bytes hash, producer, provenance).

## Tasks

### T1 — Client-side slice closure library  (M, depends on: none)

**Goal.** `desktop/src/analysis/slice.cpp`: backward/forward closures over recorded defuse
edges, semantics identical to `asmtest_slice_backward`/`_forward`, zero engine deps.

**Steps.**
1. Create `desktop/src/analysis/slice.h` + `slice.cpp` (C++17, standard library only — not
   even `asmtest_valtrace.h`, so the fixture tests need no repo headers).
2. Implement the closure rule below over a prebuilt adjacency list (O(V+E) per query; the
   C slicer rescans edges, O(V·E) — same result; the difference matters at PT scale).
3. Add the two test binaries below and wire them into `mk/desktop.mk`'s `desktop-test`
   (03 owns the target), mirroring the [cli/test_view.c](../../../cli/test_view.c#L22)
   check-helper pattern.

**Code.**

```cpp
// desktop/src/analysis/slice.h  (new)
struct dt_edge { uint32_t from_step, to_step; };   // mirrors asmtest_defuse_edge_t endpoints
struct dt_slice {
    std::vector<uint32_t> steps;                   // ascending, deduplicated
    bool contains(uint32_t step) const;            // binary search
};
// Forward: everything the value at `origin` influences; backward: everything that produced
// it. nsteps = the step count the graph spans (analogue of asmtest_defuse_t.nsteps).
dt_slice dt_slice_forward (const std::vector<dt_edge>&, uint32_t nsteps, uint32_t origin);
dt_slice dt_slice_backward(const std::vector<dt_edge>&, uint32_t nsteps, uint32_t origin);
```

**The closure rule (must match [src/dataflow.c:665–714](../../../src/dataflow.c#L665)):**
the slice is the least set S such that (a) `origin ∈ S` if `origin < nsteps`, else S is
**empty** (an out-of-range origin yields the empty slice, *not* `{origin}`); (b) forward:
for every edge `(from → to)` with `from ∈ S` and `to < nsteps`, `to ∈ S`; backward: for
every edge with `to ∈ S` and `from < nsteps`, `from ∈ S`; followed transitively to the
fixed point. Report ascending, deduplicated. A valid origin with no edges yields
`{origin}`; an empty edge set likewise.

**Tests.** `desktop/test/test_slice.cpp` fixture (6 steps, edges
`{0→2, 1→2, 2→4, 3→4, 4→5}`, `nsteps = 6`): backward(4) = `{0,1,2,3,4}`; backward(2) =
`{0,1,2}`; forward(1) = `{1,2,4,5}`; forward(5) = `{5}`; backward(0) = `{0}`;
forward(6) = backward(7) = `{}`; an edge with `to_step = 9 ≥ nsteps` is ignored;
`contains()` agrees with membership. `desktop/test/test_slice_diff.cpp` (links
`$(BUILD)/dataflow.o` — **full-app test half only**): fill an `asmtest_defuse_t` by hand
for the fixture plus 200 pseudo-random graphs from a fixed seed (≤64 steps, ≤256 edges);
for every step assert `dt_slice_forward/backward` equals the C
[`_forward_seed`/`_backward_seed`](../../../include/asmtest_valtrace.h#L218) results and
`dt_slice::contains` equals [`asmtest_slice_contains`](../../../src/dataflow.c#L743).

**Docs.** File-top comment stating the parity contract and pointing at `src/dataflow.c`.

**Done when.**
- `make desktop-test` runs both binaries green; `test_slice` also builds in the
  render-only half — its link line shows no `dataflow.o`, no Unicorn, no Capstone.

### T2 — Deep-link router + keyboard bindings  (S, depends on: 03)

**Goal.** One internal navigation API — "open recording R (optionally vs R′) at step S /
offset O in view V" — used by every view here, 06's test-failure producer, and 08. Plan
D4's spine; it exists before the views so they register with it.

**Steps.**
1. Create `desktop/src/nav.h` + `nav.cpp` (new).
2. Parse/format the textual form (golden tests, the `y` copy-link action, 06's failure
   events): `asmtrace-link:v=<canvas|timeline|slice|diff>&rec=<id>[&rec_b=<id>][&step=<u32>][&off=<hex>]`.
   Unknown keys ignored (the schema's forward-compat rule); missing `v` or `rec` fails.
3. Views register one handler per `dt_view`; `dt_nav_go` switches the shell view (03's
   API) then invokes the handler. Every keyboard binding routes through `dt_nav_go` — no
   view-private navigation state.

**Code.**

```cpp
// desktop/src/nav.h  (new)
enum class dt_view { canvas, timeline, slice, diff };
struct dt_link {
    std::string rec, rec_b;          // recording id(s), 03's document-model key; rec_b optional
    dt_view     view = dt_view::canvas;
    std::optional<uint32_t> step;    // dataflow step index
    std::optional<uint64_t> off;     // code offset in the recording's basis
};
bool        dt_nav_parse(std::string_view s, dt_link& out, std::string& err);
std::string dt_nav_format(const dt_link&);
void        dt_nav_register(dt_app&, dt_view, std::function<void(const dt_link&)>);
bool        dt_nav_go(dt_app&, const dt_link&);  // false + status-bar reason if rec unknown
```

Keyboard-first bindings (rendered in 03's help overlay): `1/2/3/4` switch view · `j/k` or
`Down/Up` next/prev row or step · `PgDn/PgUp` page · `Ctrl+G` go-to step/offset prompt ·
`Enter` on a canvas/timeline row → slice explorer at that step · `b`/`f` backward/forward
cone from the selected step · `c` clear cones · `[`/`]` walk one dependence generation
back/forward (matches the Loom's generation walk, [05-loom-day-one.md](05-loom-day-one.md))
· `d` attach/detach a second recording (diff) · `x` swap A/B · `n`/`p` next/prev divergence
· `y` copy `dt_nav_format` of the current position to the clipboard.

**Tests.** `desktop/test/test_nav.cpp`: parse↔format round-trip byte-stable over a table
of links (including empty optionals); unknown key ignored; bad links rejected with a
reason string. Plus one assertion per view golden test (T3–T5): a parsed link lands on
the expected step/offset (headless, on the view-model's selection field).

**Docs.** Binding table in a "Replay views" section of `desktop/README.md` (03's file).

**Done when.**
- `make desktop-test` runs `test_nav` green; round-trip and rejection cases pass.

### T3 — Trace canvas  (M, depends on: T2)

**Goal.** Per-offset heat + block boundaries + coverage gutter over trace/coverage
events; disasm column from recorded strings (D10) with offset fallback; loud truncation
banner; hard refusal to mix basis tags.

**Steps.**
1. Create `desktop/src/views/canvas.h` + `canvas.cpp`: a pure view-model builder + a thin
   ImGui draw (the builder carries all logic and all tests).
2. Rows: every distinct offset in the insn stream ∪ block set ∪ disasm map, ascending.
   Per row: `heat` = occurrences in the ordered insn stream; `block_start` = membership
   in the deduped block set; `covered` gutter mark = `block_start` (coverage is
   block-granular — never imply per-instruction coverage the data does not carry);
   `disasm` = recorded string or `""` (bare offset, dimmed — absence degrades, never
   errors).
3. Basis rule: record the first `basis` seen; any event with a different basis sets
   `basis_error` and the renderer draws a full-pane refusal placard ("mixed address
   bases: region-relative and absolute events in one canvas — re-record or open the
   streams separately"), **no rows**. Mixed bases mis-attribute every row; refusal is the
   only honest output.
4. Truncation banner: when `truncated` is set or `insns_total` exceeds recorded events, a
   top banner (warning color, never collapsible): "TRUNCATED: heat computed over N of M
   instructions; drops: <provenance counters>". Heat comes from exact trace events only —
   statistical streams are never folded in.
5. Router registration (`dt_view::canvas`): `off` selects/scrolls; `step` resolves
   through the dataflow stream's `insn_off[step]` when present.

**Code.**

```cpp
// desktop/src/views/canvas.h  (new)
struct dt_canvas_row { uint64_t off; uint32_t heat; bool block_start; bool covered; std::string disasm; };
struct dt_canvas {
    std::vector<dt_canvas_row> rows;       // ascending by off
    std::string basis, basis_error;        // "region"|"abs" (01's tag); non-empty error => NO rows
    bool truncated; std::string banner;    // banner "" when the recording is complete
    std::optional<uint64_t> selected_off;  // router target
};
dt_canvas   dt_canvas_build(const dt_recording&);
std::string dt_canvas_dump (const dt_canvas&);    // golden-test surface
```

**Tests.** `desktop/test/test_canvas.cpp`: build over golden fixtures from
`tests/golden-asmtrace/` (01), compare a deterministic dump (`dt_canvas_dump`, one
`off|heat|B?|C?|disasm` line per row) against files committed under
`desktop/test/expected/`. Fixtures: `add_signed` (straight-line: every row heat 1, one
block) and a branching/loop recording (heat > 1 on loop rows). Dishonesty (D7, T8):
truncated-trace asserts `truncated == true` and a banner containing `"TRUNCATED"`, N, M;
mixed-basis asserts `basis_error != ""` and `rows.empty()`; no-disasm asserts offset
fallback and a byte-stable dump.

**Docs.** `desktop/README.md`: what heat/gutter mean, the basis rule, the banner contract.

**Done when.**
- `make desktop-test` runs `test_canvas` green; `make asmtrace-golden` then re-running is
  byte-stable (D6); truncated/mixed-basis fixtures produce banner/refusal (asserted).

### T4 — Operand-value timeline  (M, depends on: T2)

**Goal.** The per-step timeline (disasm/offset + value annotations + def-use in/out
counts) over recorded dataflow events, reusing
[cli/asmspy_dataview.h](../../../cli/asmspy_dataview.h)'s helpers verbatim — one
annotation semantics for TUI and GUI.

**Steps.**
1. Create `desktop/src/views/timeline.h` + `timeline.cpp`; `#include "asmspy_dataview.h"`
   (add `-Icli` to desktop compile flags in `mk/desktop.mk`). The header is inline-only;
   **do not call** `asmspy_df_rowstyle` — it pulls in the `asmtest_slice_contains` link
   dep ([cli/asmspy_dataview.h:116](../../../cli/asmspy_dataview.h#L116)); use the
   T1-based `dt_rowstyle` below.
2. Reconstitute an `asmtest_valtrace_t` **view**: point `insn_off`/`recs` (+ `wide`) at
   `dt_recording`'s own vectors; set `steps_len`/`recs_len`/`*_total`/`truncated`/
   `mem_space` from the stream. No copy, and no `asmtest_valtrace_new` (a `dataflow.c`
   symbol — filling the struct by hand keeps the render-only binary engine-free). The
   reader (02) delivers records grouped by ascending step, as
   [`asmspy_df_annotate`'s cursor contract](../../../cli/asmspy_dataview.h#L39) requires —
   assert once at build time; set the banner if violated.
3. Per visible step: `asmspy_df_annotate(&vt, s, &cur, 4, ann, sizeof ann)` threading
   `cur` top-down; `asmspy_df_defuse_counts` over an `asmtest_defuse_t` view filled by
   hand from recorded edges; labels via `asmspy_df_loc_str`; disasm = recorded string for
   `insn_off[s]`, else the offset (D10). Row emphasis: `dt_rowstyle(slice, step)` —
   NORMAL with no cone active, IN-SLICE when `slice->contains(step)`, DIMMED otherwise; a
   mirror of [`asmspy_df_rowstyle`](../../../cli/asmspy_dataview.h#L112) over `dt_slice`.
4. Truncation banner as T3, from the dataflow stream's `truncated` + `*_total` vs stored
   counts ("annotations computed over N of M steps / R of Q operand records").
5. Router registration (`dt_view::timeline`): `step` selects the row; `Enter` emits a
   `dt_view::slice` link at the same step.

**Code.**

```cpp
// desktop/src/views/timeline.h  (new)
struct dt_timeline_row { uint32_t step; uint64_t off; std::string disasm, ann; size_t n_in, n_out; };
struct dt_timeline {
    std::vector<dt_timeline_row> rows;      // step order
    bool truncated; std::string banner;
    std::optional<uint32_t> selected_step;
};
dt_timeline dt_timeline_build(const dt_recording&);   // + dt_timeline_dump for goldens
```

**Tests.** `desktop/test/test_timeline.cpp` with `dt_timeline_dump`
(`step|off|disasm-or-offset|ann|n_in/n_out` per row) over a golden fixture with known
operand records (01's emulator L0 producer is deterministic): assert exact `ann` strings —
grammar per [:39–50](../../../cli/asmspy_dataview.h#L39): register writes `->0xV`,
register reads skipped, memory `[0xEA]<-0xV` / `[0xEA]->0xV`, `?` for uncaptured, `[wide]`
for >8-byte, trailing `...` past 4 tokens. Dishonesty: the dropped-records fixture asserts
the banner and that retained steps still annotate. Rowstyle: with a backward cone active,
assert IN-SLICE/DIMMED for a hand-picked pair of steps.

**Docs.** `desktop/README.md`: one line pointing at the grammar's home
(`asmspy_dataview.h`).

**Done when.**
- `make desktop-test` runs `test_timeline` green with byte-stable dumps; two annotation
  strings copied as literals match the TUI semantics — frontend drift fails the build.

### T5 — Slice explorer view  (M, depends on: T1, T2)

**Goal.** The interactive def-use DAG: layered layout by step index, click a step →
backward and forward cones lit. Producer-agnostic (recorded edges only — emulator L0
today, PT replay when 07/08 land).

**Steps.**
1. Create `desktop/src/views/slice_view.h` + `slice_view.cpp` (builder + thin draw).
2. Layout — **layered by step index, never force-directed** (the plan's killed list bans
   live force layouts): x-column = step index (a column per step carrying any edge
   endpoint or the selection); nodes sit on lane 0; each edge gets the lowest arc lane
   free over `[from,to]`, assigned greedily in ascending `(from_step, to_step)` order.
   Identical input ⇒ identical layout: no randomness, no iteration.
3. Selection: clicking a node (or arriving via router `step`) computes `dt_slice_backward`
   + `dt_slice_forward` (T1); backward cone one hue, forward another, the step itself
   both, everything else dimmed. `[`/`]` move one generation along the cone (nearest
   in-cone step by step distance; ties toward the lower step).
4. Node label: offset + recorded disasm (D10 fallback as T3); edge tooltip:
   `asmspy_df_loc_str`. Truncation banner as T4 — cones over a truncated dataflow stream
   are lower bounds; say "cones incomplete: trace truncated".

**Code.**

```cpp
// desktop/src/views/slice_view.h  (new)
enum class dt_cone { none, back, fwd, both, dimmed };
struct dt_slice_node { uint32_t step; uint64_t off; std::string label; dt_cone style; };
struct dt_slice_edge { uint32_t from_step, to_step; int lane; std::string loc; };
struct dt_slice_view {
    std::vector<dt_slice_node> nodes;   // ascending by step (column order)
    std::vector<dt_slice_edge> edges;
    std::optional<uint32_t> selected_step;
    bool truncated; std::string banner;
};
dt_slice_view dt_slice_view_build(const dt_recording&, std::optional<uint32_t> sel); // + _dump
```

**Tests.** `desktop/test/test_slice_view.cpp` with `dt_slice_view_dump` (nodes:
`step@col/lane [style]`; edges: `from->to lane=N loc=<label>`) over T1's hand fixture
**and** a golden recording. Hand fixture: select step 4; assert styles — steps 0,1,2,3
`back`, 5 `fwd`, 4 `both`, an edge-less step `dimmed`; the overlapping edges `0→2`/`1→2`
get different, stable lanes. Golden: byte-stable dump; router link
`...v=slice&rec=...&step=4` lands with `selected_step == 4`. Dishonesty: truncated
fixture asserts the "cones incomplete" banner.

**Docs.** `desktop/README.md`: what the cones mean; cones over a truncated recording are
lower bounds.

**Done when.**
- `make desktop-test` runs `test_slice_view` green, dumps byte-stable; full app:
  canvas→timeline→slice click-through works end to end (manual smoke, noted in the PR).

### T6 — Diff library  (M, depends on: none)

**Goal.** `desktop/src/analysis/diff.cpp`: align two recordings of the same routine (same
entry) — coverage union/delta, per-offset heat delta, hot-edge delta, shared-prefix first
divergence. Plan D3: a primitive, not a view feature. **This alignment seam is the Loom's
patient-zero mechanism** — [05-loom-day-one.md](05-loom-day-one.md) consumes
`dt_divergence` for its fork comparison; do not fork the logic there.

**Steps.**
1. Create `desktop/src/analysis/diff.h` + `diff.cpp` (pure over two `dt_recording`s).
2. Preconditions (refuse with a reason, never a garbage diff): same routine identity
   (metadata code-bytes hash must match; name informational — per 01's header), same
   `basis` tag, both have a trace stream. On mismatch: `err` set, empty diff, `false`.
3. Coverage: `only_a` / `only_b` / `both` over the deduped block sets. Heat delta: per
   distinct offset, `(count_a, count_b)` from the ordered insn streams; report offsets
   where they differ, plus block starts. Hot-edge delta: join survey edges on `(from,to)`,
   delta the counts; keep the statistical provenance attached — views must label this
   panel statistical and never merge it with exact heat (plan D1 provenance law).
4. Shared-prefix alignment: walk both ordered insn streams by step index; the first `i`
   with `off_a[i] != off_b[i]` (or one stream ending) is the divergence
   `{diverged, step, off_a, off_b}`; identical streams report `diverged = false`. If
   either stream is truncated the verdict only covers the shorter recorded prefix — set
   `bounded = true`; views must say "no divergence observed within the recorded window",
   never "identical".

**Code.**

```cpp
// desktop/src/analysis/diff.h  (new)
struct dt_divergence { bool diverged; bool bounded; uint32_t step; uint64_t off_a, off_b; };
struct dt_heat_delta { uint64_t off; uint32_t a, b; };
struct dt_edge_delta { uint64_t from, to; uint64_t a, b; };
struct dt_diff {
    std::vector<uint64_t> only_a, only_b, both;    // block coverage
    std::vector<dt_heat_delta> heat;
    std::vector<dt_edge_delta> edges;              // statistical — label as such
    dt_divergence div;
};
bool dt_diff_build(const dt_recording& a, const dt_recording& b, dt_diff& out,
                   std::string& err);                 // false + err on a refused pair
```

**Tests.** `desktop/test/test_diff.cpp`, hand-built: two 5-step streams diverging at step
3 → `{true, false, 3, offA, offB}`; identical → `diverged = false`; A truncated at step 2
with agreement so far → `bounded = true`; different code hash → refusal with non-empty
`err`; mixed basis → refusal. Golden: 01's same-routine two-input pair — assert the
committed expected coverage delta; byte-stable across `make asmtrace-golden`.

**Docs.** File-top comment: the same-entry precondition and the bounded rule.

**Done when.**
- `make desktop-test` runs `test_diff` green; refusals produce `err` strings, not empty
  diffs; the bounded case never reports "identical".

### T7 — One-or-two recordings in every view  (M, depends on: T3, T4, T5, T6)

**Goal.** Plan D3's document-model law: every view accepts one **or two** recordings.
Wire `dt_diff` into the view chrome and add the diff view proper.

**Steps.**
1. With `rec_b` set, views render A/B. **Canvas:** heat becomes a signed delta (diverging
   color scale), gutter shows ∪ / A-only / B-only marks, the divergence offset gets a
   "patient zero" marker; `n`/`p` jump divergences (one in v1 — first divergence).
   **Timeline:** rows past `div.step` render the unaligned treatment (dashed separator,
   per-side columns) — never drawn as agreement (the Loom's honesty rule). **Slice
   explorer:** single-recording in v1 — with `rec_b` set it renders A plus a status line
   "slice diff lands with the state-diff producer (Wave 2)"; never a fake merged graph.
2. Add `desktop/src/views/diff_view.h/.cpp` (`dt_view::diff`): the summary panel — the
   refusal reason, or coverage counts (∪, A-only, B-only), top heat deltas, hot-edge
   deltas under a "statistical" provenance chip, and the divergence card (step + both
   offsets + each side's disasm, D10 fallback). Every row is a deep link (`dt_nav_go`)
   into canvas/timeline. `d` attaches recording B via the shell's open dialog (03); `x`
   swaps.

**Code.** `dt_diff_view_build(const dt_recording&, const dt_recording&)` → row/dump
structs following the T3–T5 builder + dump + thin-draw pattern.

**Tests.** `desktop/test/test_diff_view.cpp`: dump over a golden pair + the refusal pair;
assert the divergence card's links parse back (`dt_nav_parse`) to the right step. Plus a
canvas-with-B golden dump (delta column present; banner shows both recordings' truncation
states) and a timeline post-divergence dump showing the dashed treatment.

**Docs.** `desktop/README.md`: the two-recording model, refusal rules, "patient zero"
(cite [05-loom-day-one.md](05-loom-day-one.md)).

**Done when.**
- `make desktop-test` green across `test_diff_view` and the extended T3/T4 golden dumps;
  opening two same-routine goldens and pressing `d` shows deltas (manual smoke).

### T8 — Dishonesty fixtures + suite consolidation  (S, depends on: T3, T4, T5, T6, T7)

**Goal.** Make D7 real for these views: every dishonesty path asserted by a committed
fixture; all view tests riding `desktop-test`; changelog.

**Steps.**
1. Extend 01's corpus generator (`make asmtrace-golden`) with the fixtures these views
   need **if 01 has not already shipped them**: `trunc-trace` (`truncated` +
   `insns_total >` recorded), `trunc-dataflow` (dropped operand records), `mixed-basis`
   (hand-authored NDJSON with one region + one abs trace event — invalid by intent,
   excluded from round-trip tests; hand-authoring is valid per the plan's D1),
   `no-disasm` (D10 absence), and the same-routine diff pair. Coordinate names with 01.
2. Confirm every T3–T7 test consumes fixtures from `tests/golden-asmtrace/` only, so
   `make asmtrace-golden && git diff --exit-code tests/golden-asmtrace` stays the one
   regeneration gate (D6). Re-verify the render-only/full split: nothing in T1–T7 links
   `dataflow.o` except `test_slice_diff`.
3. CHANGELOG.md: one `Added` bullet under `## [Unreleased]` ("desktop replay views: trace
   canvas, operand timeline, slice explorer, recording diff, deep links"). Run
   `make docker-desktop` (03's lane) — the whole suite green in-lane.

**Tests.** This task is the consolidation; its check is the lane.

**Docs.** The changelog bullet; a fixture inventory table appended to
[asmtrace-schema.md](asmtrace-schema.md)'s corpus section (owned by 01 — append only).

**Done when.**
- `make desktop-test` and `make docker-desktop` are green;
  `make asmtrace-golden && git diff --exit-code tests/golden-asmtrace/` passes (D6).
- Every dishonesty fixture is asserted by at least one view test (grep the fixture names
  across `desktop/test/` — none unused).

## Task order & parallelism

**T1** and **T6** are pure libraries with no view/shell dependency — start both
immediately, in parallel, even before 03 merges (their hand fixtures need no
`dt_recording`). **T2** needs 03's shell and lands next. **T3/T4/T5** are mutually
independent once T1/T2 exist — three people can work them concurrently (different files).
**T7** integrates and follows T3–T6; **T8** closes. Critical path:
**T2 → (T3|T4|T5) → T7 → T8**, with T1/T6 as off-path feeders.

## Constraints & gates

- **License split (set D4/D9, plan D7).** Everything here must build in the
  **render-only** `desktop-render` binary with zero engine deps: no `dataflow.o`, no
  Unicorn/Keystone/Capstone, no asmspy engine TUs. Single exception:
  `test_slice_diff.cpp` (links `dataflow.o`, full-app test half only). Including
  `asmtest_valtrace.h` / `asmtest_trace.h` / `asmspy_dataview.h` is fine (types + inline
  functions); *calling* a `src/`-implemented symbol is not.
- **Determinism.** Every builder/dump is a pure function of the recording bytes: no
  wall-clock, locale surprises, pointer-order iteration, or layout randomness. Golden
  byte-stability (D6) is the enforced form.
- **Honesty chrome (asserted, not aspirational).** Truncation banners always visible;
  statistical hot-edge data labeled, never merged into exact heat; bounded diffs never
  claim identity; mixed bases refuse to render; cones on truncated recordings are labeled
  lower bounds. Each is pinned by a T3–T8 test.
- **Threading.** Builders run on the UI thread at open; a recording above an event-count
  threshold (start at 1M) builds on one worker `std::thread` with a "computing…" placard,
  handed over via a `std::future` polled once per frame. Analysis structs are immutable
  after build; no view mutates `dt_recording`. Nothing here touches `asmspy_graphsort.h`
  (its file-scope qsort-latch caveat, plan D5, is 08's concern).
- **Formatting.** Repo `.clang-format` applies to all `desktop/` C++ (set D8);
  clang-format 18 per [_conventions.md](../implementations/_conventions.md).
- **Dependency rule.** No third-party dep beyond 03's pinned ImGui + nlohmann/json (set
  D2); if a task appears to need one, the answer is a pure-C++ implementation.
- **No new make targets.** All tests ride 03's `desktop-test`; no
  [Makefile help](../../../Makefile#L91) edits needed.

## Out of scope

- **The Loom** — composes these views but is [05-loom-day-one.md](05-loom-day-one.md)'s;
  this doc only exports `dt_slice` and `dt_diff`/`dt_divergence` for it.
- **Test-failure deep-link producer** (06 emits, this doc parses); **live capture,
  `--serve`, observer views, syscall redaction UI**
  ([07-serve-live-host.md](07-serve-live-host.md) /
  [08-observer-views.md](08-observer-views.md)); **the `.asmtrace` reader/writer and
  exporters** (01/02 — this doc consumes `dt_recording` only).
- **Slice-diff / state-diff rendering** — needs the Wave 2 state-diff producer; the slice
  explorer explicitly refuses a merged two-recording graph until then.
- **Register time-travel scrubber, blame cones, FP-env panels** — Phase 4 / expansion
  sockets. **Fresh local capture on macOS** — the darwin `libasmtest_dataflow` build is a
  plan work item; replay of recordings (this doc) works everywhere by construction.
