# Plotting & graph navigation: ImPlot chassis + node-editor canvas — implementation

> **Sources.** Actioned from [11-imgui-addons.md](11-imgui-addons.md): ranked
> recommendations **#3** (ImPlot v1.0) and **#10** (imgui-node-editor /
> imgui_canvas), the "Then:" row of **Sequencing**. Written 2026-07-26 against
> HEAD `27cd43e`. This doc wins over doc 11 on disagreement; the CODE wins over
> this doc — re-verify file:line before editing (doc 11 verified at `4f11065`).
>
> Read [\_conventions.md](../implementations/_conventions.md) first; D1–D11 live
> in this directory's [README](README.md). **Prerequisites:
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
  any of them into a dishonest chart (no stacks for survey data).
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

**Goal.** Adopt ImPlot **at the `v1.0` tag** as a chassis wrapped around the
existing pure view-models — not a restyle. Render the four numeric surfaces
through it and finally show `perf_history`.

**Steps.**
1. Vendor ImPlot at the **`v1.0` tag** (MIT) via doc 12's `fetch-addon.sh`
   (tarball shape): one digest row, one `licenses/ImPlot-LICENSE.txt`, its
   `implot.cpp`/`implot_items.cpp`/`implot.h`/`implot_internal.h` added to the
   desktop object set in [mk/desktop.mk](../../../mk/desktop.mk) beside the imgui
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
     honesty-compliant (this is why it is bars+heatmap, not a stacked area).
   - `data/perf_history`: `PlotLines` — finally rendered, closing the
     parsed-but-never-shown gap.
   - `views/timeline.cpp` + scrubber: `DragLineX` as the playhead (hover/held
     out-params), `TagX` labels, `SetupAxisLinks` to sync the time axis across
     timeline/watch/diff panes. **Axis stays ordinal** — a custom tick formatter
     labels it "step", never wall time (the recording has no honest wall clock).
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
   policy you enforce, and it is the honesty guardrail.
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
  honesty guardrail, not a preference.
- **No stacks for survey data** (honesty R4). Hotedges is bars + heatmap, not a
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
