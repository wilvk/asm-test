# Plotting & graph navigation: ImPlot chassis + node-editor canvas — implementation

> **Sources.** Actioned from [11-imgui-addons.md](11-imgui-addons.md): ranked
> recommendations **#3** (ImPlot v1.0) and **#10** (imgui-node-editor /
> imgui_canvas), the "Then:" row of **Sequencing**. Written 2026-07-26 against
> HEAD `27cd43e`. This doc wins over doc 11 on disagreement; the CODE wins over
> this doc — re-verify file:line before editing (doc 11 verified at `4f11065`).
>
> Read [\_conventions.md](../../implementations/_conventions.md) first; D1–D11 live
> in this directory's [README](../../gui/README.md). **Prerequisites:
> [12-addon-supply-chain.md](12-addon-supply-chain.md)** (fetch + compile-gate)
> and **[13-foundation-moves.md](13-foundation-moves.md)** — ImPlot's dense
> heatmaps need F2's 32-bit `ImDrawIdx`; both views land "better on top of" the
> docking panes (F1). Neither *requires* F3 fonts, but both read better with
> them.

## Why this work exists

Two chassis, adopted around **existing pure view-models** — not new looks:

- **ImPlot** gives the app axes/pan/zoom/cursors it hand-rolls today, and
  finally renders `data/perf_history` (parsed + tested but drawn nowhere) and
  the hotedges/timeline/scrubber/watch numeric surfaces — **without** turning
  any of them into a misleading chart (no stacks for survey data).
- **imgui-node-editor** (and, as a de-risk step, the standalone `imgui_canvas`)
  gives the graph views — topo, tree, hotedges snapshot, slice explorer — a
  real pan/zoom canvas with selection and "fit graph", while keeping the app's
  own **deterministic** layout (force-directed stays banned per docs 04/08).

Both are draw-half chassis over pure models (D4). The models decide; the addon
draws and navigates.

## What already exists (verified 2026-07-26)

- **`data/perf_history` is dead-rendered** — `desktop/src/data/perf_history.{h,
  cpp}` is parsed and covered by `desktop/test/test_data_readers.cpp`, but the
  app draws zero plot lines (doc 11 survey). This is ImPlot's clearest win.
- **The numeric views** — `desktop/src/views/hotedges.{h,cpp}` (ranked edges +
  src×dst heat), `desktop/src/views/timeline.{h,cpp}` + the scrubber
  (`views/scrubber.*`), `desktop/src/views/watch.{h,cpp}` (value-over-step) — all
  hand-drawn on raw draw lists today.
- **The graph views** — `desktop/src/views/topo.{h,cpp}`,
  `desktop/src/views/tree.{h,cpp}`, and the **draw-only** slice explorer
  `desktop/src/views/slice_view_draw.cpp` (83 lines: no hit-testing, no
  zoom/pan, overflows rightward unboundedly — doc 11 survey).
- **The null-backend test tier** — both addons must work there (draw lists only).
  ImPlot does (`GetPlotDrawList`); node-editor's canvas does too.
- **F2's 32-bit indices** (doc 13 T3) — required before ImPlot's dense
  `PlotHeatmap`, or a large heatmap overflows the null tier's 16-bit cap.
- **The deep-link router** — `dt_nav_go` (`desktop/src/nav.h:111`); every
  node/point click routes through it, no view-private navigation state (D4).

## Tasks

### T1 — ImPlot v1.0 as an axes/pan/zoom/cursor chassis  (M, depends on: 12, 13 F2)

> **Implemented 2026-07-27 (ImPlot chassis + hot-edge heatmap) — green (51
> suites).** ImPlot **v1.0** vendored (`scripts/fetch-implot.sh`, 2 compiled TUs,
> MIT; digest pinned; license captured; compile-verified on `1.91.9b-docking`,
> warning-clean). **Pin note confirmed by integration**: v1.0 is the new
> `ImPlotSpec` item API — `PlotHeatmap`'s signature differs from ≤0.17, so
> pre-2026 examples would not compile (exactly why the tag, not master, is
> pinned). The `ImPlotContext` is created in `main.cpp` beside the ImGui one
> **for the app only**; every ImPlot draw is guarded on
> `ImPlot::GetCurrentContext()` so the null-backend view tests (which create
> none) degrade to text — the app plots, the tests don't crash. First surface
> wired: `views/hotedges` gains a **src×dst `PlotHeatmap`** over
> `obs_hotedges_matrix` (a pure, tested helper — a bounded grid of edge WEIGHTS,
> never a stack, R4-compliant; `test_obs_hotedges` pins total-weight
> preservation + the cap/truncation). The addon rides the `observer_draw.o` link
> sites. **Second surface (2026-07-27):** `views/watch` gains a value-over-hit
> **`PlotStairs`** over `obs_watch_plot` (a pure tested helper — **only
> `value_ok` hits are plotted; an un-read-back value is a GAP, never a fabricated
> 0**; `test_obs_watch` pins the exclusion), reusing the ImPlot already linked
> into `observer_draw.o` (no new link surgery). **Remaining T1**: `perf_history`
> (`PlotLine`, closing the parsed-but-never-shown gap), and the
> timeline/scrubber `DragLineX` playhead + `SetupAxisLinks` — same chassis,
> follow-on surfaces. F2's 32-bit `ImDrawIdx` (done) covers the dense heatmap.

**Goal.** Adopt ImPlot **at the `v1.0` tag** as a chassis wrapped around the
existing pure view-models — not a restyle. Render the four numeric surfaces
through it and finally show `perf_history`.

**Steps.**
1. Vendor ImPlot at the **`v1.0` tag** (MIT) via doc 12's `fetch-addon.sh`
   (tarball shape): one digest row, one `licenses/ImPlot-LICENSE.txt`, its
   `implot.cpp`/`implot_items.cpp`/`implot.h`/`implot_internal.h` added to the
   desktop object set in [mk/desktop.mk](../../../../mk/desktop.mk) beside the imgui
   core sources. **Pin the tag, not master**: master is v1.1 WIP tracking ImGui
   1.92 signatures, and v1.0's `ImPlotSpec` redesign (PR #519) removed the old
   styling API — virtually all pre-2026 ImPlot examples target ≤v0.17 and will
   not compile. Doc 11 compile-verified v1.0 against vanilla 1.91.9 (back-compat
   guards reach `IMGUI_VERSION_NUM 18102`; no docking requirement).
2. Create/init an `ImPlotContext` next to the `ImGuiContext` in `main.cpp` and in
   the null-backend test harness (so plot tests run headless).
3. Wire the four surfaces via `GetPlotDrawList` + `PlotToPixels` around the
   existing models (do **not** move logic into draw code — D4):
   - `views/hotedges.cpp`: ranked `PlotBars` + src-block × dst-block
     `PlotHeatmap` with `ColormapScale` — **stays "edges not stacks"**,
     fidelity-compliant (this is why it is bars+heatmap, not a stacked area).
   - `data/perf_history`: `PlotLines` — finally rendered, closing the
     parsed-but-never-shown gap.
   - `views/timeline.cpp` + scrubber: `DragLineX` as the playhead (hover/held
     out-params), `TagX` labels, `SetupAxisLinks` to sync the time axis across
     timeline/watch/diff panes. **Axis stays ordinal** — a custom tick formatter
     labels it "step", never wall time (the recording has no real wall clock).
   - `views/watch.cpp`: value-over-step as `PlotStairs`; flag bits as
     `PlotDigital`.
4. All named APIs are verified present at `v1.0` (doc 11). Keep the scrubber's
   existing `[`/`]` key + playhead-persistence behaviour (09) — ImPlot supplies
   the axis/cursor chrome, not the playhead ownership.

**Tests.** Extend the existing view tests (`test_obs_hotedges`, `test_timeline`,
`test_data_readers` → a new render test for perf_history, `test_obs_watch`) under
the null backend: assert the model→plot mapping (e.g. a perf_history series
produces the expected point count; the heatmap cell for a known src×dst pair
carries the right value; the timeline playhead maps step→pixel monotonically).
Do **not** assert pixels — assert the model the plot is fed.

**Docs.** CHANGELOG `Added`: plots for perf history, hot-edge heatmap, watch
values; timeline/scrubber on a synced ordinal axis. `licenses/README.md` row
(bundled). `desktop/README.md` note that the time axis is ordinal-by-design.

**Done when.** `perf_history` renders as lines; hotedges shows ranked bars + a
src×dst heatmap (not a stack); watch shows stairs/digital; the timeline/scrubber
share a synced **ordinal** "step" axis with a draggable playhead; all under the
null backend; ImPlot is pinned at `v1.0`; F2's 32-bit indices are in place.

### T2 — imgui_canvas de-risk for the slice explorer  (S, depends on: 12)

> **Implemented 2026-07-27 — green (53 host suites + clean-tree checked).** The
> standalone `imgui_canvas` (from the imgui-node-editor tree, master `021aa0ea`,
> MIT; fetched by `scripts/fetch-nodeeditor.sh`, digest pinned in 53b3b6f;
> compile-verified on `1.91.9b-docking`; in the compile-probe — `imgui_internal.h`)
> now wraps `views/slice_view_draw.cpp`. It is a **drop-in**: the canvas transforms
> the coordinate system, so the existing *layered, deterministic* layout draws
> unchanged inside `canvas.Begin/End` — no force-directed anything (docs 04/08).
> Dropping the old space-reserving `Dummy` + clipping to the viewport **fixes the
> survey's "overflows rightward unboundedly"**; drag pans, wheel zooms (clamped
> 0.1–8×). **New test coverage**: `test_slice_view_draw` drives the draw headless
> across frames (imgui_canvas is draw-list-only, so it runs under the null
> backend) + the empty early-return path — the slice draw had *no* test before.
> `imgui_canvas.o` rides the `slice_view_draw.o` link sites; the full node editor
> (T3) reuses this same fetch.

**Goal.** Before pulling the full node-editor, adopt the **standalone
`imgui_canvas`** (846 LOC; `.h`+`.cpp`) alone to give the draw-only slice
explorer pan/zoom + input remapping — no node semantics. This is doc 11's
explicit de-risk path and a shippable improvement on its own.

**Steps.**
1. Vendor `imgui_canvas.{h,cpp}` from the imgui-node-editor repo (MIT) at the
   pinned master sha (doc 11 pins the node-editor at master `021aa0ea`; use the
   same sha for the canvas files). **It includes `imgui_internal.h`**
   (`imgui_canvas.h:52`) → add to the compile-probe (doc 12 T3). One digest
   row(s), one license capture (shared with T3's node-editor row).
2. Wrap `views/slice_view_draw.cpp` (currently 83 lines, overflows rightward) in
   a canvas: pan/zoom + input remapping so the slice no longer overflows
   unboundedly and gains hit-testing. Keep the slice **model** (`views/slice_
   view.cpp`, `analysis/slice.*`) untouched — canvas is transform + input only.
3. Route clicks through `dt_nav_go` (D4) — a clicked slice node navigates, no
   view-private state.

**Tests.** `desktop/test/test_slice_view.cpp`: assert a click at a canvas
coordinate hit-tests the right slice node (transform round-trip), and that the
canvas bounds contain the slice (no unbounded overflow). Null backend.

**Docs.** CHANGELOG `Added`: slice explorer pan/zoom + hit-testing.

**Done when.** the slice explorer pans/zooms, hit-tests nodes, and no longer
overflows rightward; the slice model is unchanged; clicks route through the
router; `imgui_canvas` is in the compile-probe.

### T3 — imgui-node-editor for topo/tree navigation  (M, depends on: 12, T2; 13 F1)

> **Implemented 2026-07-27 — green (`make desktop-test`; compile-probe OK at
> `1.91.9b-docking`).** imgui-node-editor vendored at master **`021aa0ea`** (MIT),
> reusing T2's `scripts/fetch-nodeeditor.sh` fetch/pin/license row — its three
> further TUs (`imgui_node_editor.cpp`, `imgui_node_editor_api.cpp`, the bundled
> `crude_json.cpp`; `imgui_canvas.cpp` is the fourth, already built by T2) join the
> desktop object set beside ImPlot, and the public header is in the doc-12
> compile-probe (`-DASMDESK_HAVE_IMGUI_NODE_EDITOR`). **The pin is confirmed by
> integration**: `021aa0ea` compiles clean at the `1.91.9b-docking` pin (the
> compile-probe and both binaries build), where the last release v0.9.3 does not.
> The layout is a pure, tested builder (`desktop/src/views/graph_nav.{h,cpp}` — no
> ImGui, no node-editor): `graph_from_topo` / `graph_from_tree` /
> `graph_from_hotedges` lay nodes on the app's own deterministic layers, and
> `graph_visible` culls off-viewport nodes. The draw half (`observer_draw.cpp`)
> feeds those positions to `ed::SetNodePosition` **every frame** with
> `config.SettingsFile = nullptr` (`kGraphSettingsFile`), so node-editor never
> persists or invents a position — **force-directed layout stays banned** (docs
> 04/08). Node-editor is **app-only**, gated exactly as the ImPlot context is
> (`obs_graph_enable`, set in `main.cpp`); the headless null-backend deck falls
> back to the list/table and the objects are linked but never executed there.
> Double-clicking a node routes through 04's `dt_nav_go`. `test_obs_topo` /
> `test_obs_tree` pin the layout == the app's positions, `SettingsFile == nullptr`,
> the click routing, and the 10k-node cull — all on the pure model, no pixels.

**Goal.** A pan/zoom canvas with selection and `NavigateToContent` ("fit graph")
for the graph views — while keeping the app's **own deterministic layout**
(force-directed stays banned, docs 04/08).

**Steps.**
1. Vendor imgui-node-editor at **master sha `021aa0ea`** (MIT). Doc 11: the last
   release **v0.9.3 (2023) fails to compile on 1.91.9** (the `operator==`
   redefinition); master `021aa0ea`'s 4 TUs (including the vendored
   `crude_json.cpp`) compile clean at the desktop lane's flags — both facts
   independently reproduced. Add the 4 TUs to the object set; it includes
   `imgui_internal.h` → compile-probe (doc 12 T3). One license capture (shared
   with T2).
2. **Keep layout app-deterministic**: feed `ed::SetNodePosition` every frame from
   the app's own layout; set `config.SettingsFile = nullptr` so node-editor never
   persists or invents positions. The library imposes no layout — this is a
   policy you enforce, and it is the fidelity guardrail.
3. Wire the graph views:
   - `views/topo.cpp` and `views/tree.cpp` gain the canvas, selection, and
     `NavigateToContent` (fit-graph).
   - `views/hotedges.cpp`'s frozen snapshot gains legible navigation (pan/zoom
     over the same deterministic layout).
4. Route node/edge clicks through `dt_nav_go` (D4). Large graphs (10k-routine
   traces) need **visible-region culling** — cull nodes outside the viewport
   before emitting them (doc 11 perf note).

**Tests.** `test_obs_topo` / `test_obs_tree`: assert node positions equal the
app's deterministic layout (not the library's), that `SettingsFile == nullptr`
(no persisted drift), that a node click routes, and that culling drops
off-viewport nodes for a large fixture. Null backend.

**Docs.** CHANGELOG `Added`: pan/zoom + fit-graph + selection for topo/tree/hot
edges. `licenses/README.md` row (imgui-node-editor + bundled `crude_json`;
bundled).

**Done when.** topo/tree/hotedges pan/zoom with selection and fit-graph; node
positions are the app's deterministic layout every frame (`SetNodePosition`,
`SettingsFile=nullptr`); clicks route through the router; large graphs cull;
the pin is master `021aa0ea` (compiles on 1.91.9; v0.9.3 does not).

## Task order & parallelism

`12` + `13 F2` → **T1** (ImPlot, independent). `12` → **T2** (imgui_canvas
de-risk) → **T3** (node-editor, which reuses T2's vendored canvas + license row
and lands better on F1's docking panes). T1 and the T2→T3 chain are independent
of each other and can be different developers. Do **T2 before T3** — it proves
the canvas/`imgui_internal.h` include recipe on a small surface (the slice
explorer) before the node-editor's 4-TU adoption.

## Constraints & gates

- **No force-directed layout, ever** (docs 04/08). Node-editor is fed the app's
  deterministic positions every frame; `SettingsFile=nullptr`. This is the
  fidelity guardrail, not a preference.
- **No stacks for survey data** (fidelity R4). Hotedges is bars + heatmap, not a
  stacked area; that is why ImPlot is adopted as a *chassis*, not a chart
  gallery.
- **Ordinal time axis.** The timeline/scrubber axis is "step", never wall time —
  custom tick formatter, `SetupAxisLinks` for cross-pane sync only.
- **Pins are the corrected ones** — ImPlot **v1.0** (not master), node-editor
  **master `021aa0ea`** (not v0.9.3). Both re-verify at pin time.
- **F2 first** — dense heatmaps overflow the null tier's 16-bit indices without
  doc 13 T3.

## Out of scope

- ImPlot3D (doc 11 skip: no per-item picking, CPU-transform — does not replace
  `scene3d/`).
- imnodes (doc 11 skip: no zoom).
- Notifications/filtering (doc 16) and the editor/test-engine bets (doc 17).
- Any live re-layout / animation of graph nodes beyond pan/zoom/selection.
