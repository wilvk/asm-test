# Wave 2: expose the spine — command palette, wayfinding chrome, overview/minimap — implementation

> **Sources.** Actioned from the UX restructure plan
> (../plans/desktop-gui-ux-restructure-plan.md) rows **T3.1, T3.3, T3.4** and the
> review findings **F8, F9** (../plans/desktop-gui-ux-review.md). Written
> 2026-07-27 against HEAD `243f092`. This doc wins over the review/plan on
> disagreement; the CODE wins over this doc — re-verify file:line before editing.
> Read [_conventions.md](../implementations/_conventions.md) first; D1–D11 live
> in this directory's [README](README.md). **Prerequisites:
> [19-dockable-panes-keystone.md](19-dockable-panes-keystone.md)** (real panes —
> hard prereq for T2's chrome home), **[15-plotting-and-graph-nav.md](15-plotting-and-graph-nav.md)**
> T1 (ImPlot chassis) + **[14-quick-wins.md](14-quick-wins.md)** T5 (ImZoomSlider)
> for T3, and **[16-live-feedback-and-filtering.md](16-live-feedback-and-filtering.md)**
> T2 (ImSearch) for T1.

## Why this work exists

- **F8 (no command palette or overview/minimap).** The app owns a fully
  addressable deep-link space (`dt_nav_go`) and twelve views, but the expert has
  no one-keystroke finder to jump to a view/step/offset/recent/PID/routine, and
  no overview to hold global position at PT scale. Experts reach for
  `Ctrl+Shift+P`/`Ctrl+P` by reflex; here they hunt tabs and scroll. **T3.1** and
  **T3.4** close the two halves.
- **F9 (no persistent "where am I" wayfinding).** Deep in a nested tab the
  analyst cannot answer "which recording/session/step/filter/thread am I in, and
  how do I jump elsewhere" without re-hunting by eye. There is no persistent
  global context chrome, and two same-basename `.asmtrace` files are
  indistinguishable. The review calls this *cheap given `nav.current` exists;
  unbuilt.* **T3.3** builds it.
- **All three are thin layers over the shipped spine.** `dt_nav_go` already holds
  the last successful navigation in `nav.current`
  ([nav.h](../../../desktop/src/nav.h):103, set at
  [nav.cpp](../../../desktop/src/nav.cpp):273); the twelve advertised bindings are
  data (`dt_nav_bindings`, [nav.cpp](../../../desktop/src/nav.cpp):282–298). This
  brief turns that built-but-hidden router into the app's visible orientation
  model. No new engine, no schema change.
- **Fidelity (D7) is restructured, never removed.** The overview/minimap draws the
  app's own deterministic layout and must not fabricate structure to fill space
  (docs 04/08 layout ban); the wayfinding chrome surfaces provenance/scope, it
  does not hide it.

## What already exists (verified 2026-07-27)

- **The router and its current position.** `dt_nav_go`
  ([nav.cpp](../../../desktop/src/nav.cpp):252–280) resolves a `dt_link`
  (recording, optional B, `dt_view`, optional step/off/pid —
  [nav.h](../../../desktop/src/nav.h):58–69) onto the registered handler, records
  it in `t.current` on success (line 273), and sets `last_error` verbatim on
  refusal (no silent no-op). `dt_nav_table::current` is
  `std::optional<dt_link>` ([nav.h](../../../desktop/src/nav.h):103) — **the whole
  "where am I" carrier T3.3 reads, already populated.** The review cites
  `04:194`/`shell.cpp:604-607,667-668` for F9; at this HEAD the go-to and n/p code
  moved into `handle_keymap`, and the durable current-position field is
  `nav.current` — cite it, not the review's lines.
- **The bindings as data.** `dt_nav_bindings()`
  ([nav.cpp](../../../desktop/src/nav.cpp):282–298) returns twelve
  `{keys, what}` rows ([nav.h](../../../desktop/src/nav.h):115–119); the help
  overlay (`draw_bindings_help`) is fed from it so help and keymap cannot drift
  (doc 17 T1). **T3.1 enumerates the same table**, so every accelerator —
  including the ten doc-17 freshly-wired ones — becomes discoverable by typing.
- **The keymap dispatch mechanism T3.1 reuses.** `handle_keymap`
  ([shell.cpp](../../../desktop/src/ui/shell.cpp):586–698) already maps `1/2/3/4`
  to a view switch via `s.want_view` (585–597 — a keypress cannot select an ImGui
  tab directly, so it records intent the tab bar honours with `SetSelected` next
  pass), `Ctrl+G` to `s.show_goto` (604–605), `y` to a clipboard copy of
  `dt_nav_format(*s.nav.current)` (600–601). The palette's own entries drive these
  exact fields.
- **The go-to modal — the pattern for palette "go to step/offset".**
  `draw_goto_modal` ([shell.cpp](../../../desktop/src/ui/shell.cpp):704–752)
  parses typed text with the **same `dt_nav_parse`** the deep links use and jumps
  via `dt_nav_go`, so a typed target and a clicked link land identically (D4). A
  palette "go to" row is this logic behind a fuzzy filter.
- **The nav handlers.** `shell_wire_nav`
  ([shell.cpp](../../../desktop/src/ui/shell.cpp):141–171) registers one handler
  per view over `dt_all_views()`; each **only moves the selection** (`s.view`,
  `s.active_tab`, `s.b_index`, `s.selected_step/off`, cone) — plan D4: no view
  keeps its own nav state. Note the gap the palette must bridge: the handler sets
  `s.view` (a field) but **not** `s.want_view` (the tab-selection intent), so a
  view-switch dispatched purely through `dt_nav_go` updates model state without
  forcing the visible ImGui tab. The palette's view-switch entries set
  `s.want_view` too, exactly as `handle_keymap`'s `1/2/3/4` do.
- **The outer shell + where chrome would live (T3.3).** `draw_shell`
  ([shell.cpp](../../../desktop/src/ui/shell.cpp):754–907) opens one full-viewport
  `Begin("asmtest")` window (784–792), an optional `MenuBar` with the View/Reset
  menu (793–807), then a single `BeginTabBar("main")` (809) holding Home, the
  doors, and **one tab per recording titled `base_name(r.path)+"###rec"+i`**
  (871–886 — this is exactly the same-basename ambiguity F9 names: two
  `add.asmtrace` tabs are visually identical). The per-recording body
  (`draw_recording_tab`, 384–541) ends by drawing `s.status` verbatim (536–538).
  **There is no chrome band above the tab bar today** — the region between
  line 807 (`EndMenuBar`) and line 809 (`BeginTabBar`) is where T3.3's breadcrumb
  lands, in the outer window, outside every tab body. Doc 19 is the hard prereq:
  until the views are real docked panes, sibling context cannot coexist and the
  chrome has no stable home outside a single tab.
- **`ShellState` — every field the chrome reads.**
  ([shell.h](../../../desktop/src/ui/shell.h):51–156): `active_tab`, `view`,
  `selected_step`/`selected_off`, `b_index`, `want_view`, `show_goto`/`goto_buf`,
  `nav` (the `dt_nav_table`), `status`, `want_open_tab`. `ws.recordings[i].path`
  gives the parent dir for basename disambiguation.
- **The ImSearch "showing N of M" idiom T3.1 reuses (doc 16).** Two shipped
  patterns: the guarded ImSearch relevance filter in `draw_learn_door`
  ([learn_door.cpp](../../../desktop/src/ui/learn_door.cpp):112–128 —
  `BeginSearch`/`SearchBar`/`SearchableItem`/`Submit`, wrapped in
  `if (ImSearch::GetCurrentContext())` so the null test backend shows the plain
  list); and the **order-preserving** count in `draw_obs_syscalls`
  ([observer_draw.cpp](../../../desktop/src/views/observer_draw.cpp):107–118 —
  `"showing %zu of %zu"`, deliberately NOT ImSearch, because relevance re-ranking
  would scramble execution order). The palette wants relevance ranking (ImSearch
  fits a command list), so it follows the Learn-door form. The context is created
  app-only ([main.cpp](../../../desktop/src/main.cpp):96), so the palette degrades
  to an unranked list under the null backend.
- **The ImZoomSlider T3.4 completes (doc 14 T5).** The Loom already wires
  `ImZoomSlider` for step-window pan/zoom
  ([fabric_imgui.cpp](../../../desktop/src/loom/fabric_imgui.cpp):184–199,
  include at 2–5; `loom_view_step_window`/`loom_view_set_step_window` are the pure
  tested camera math). Doc 14 T5's own status: *"the timeline windowing + the
  hotedges/diff overview strip reuse the same helper — a small follow-on, not yet
  wired"* (14:249–251). So the review's F8 "not yet wired" is accurate for the
  **overview-strip half** (the drawn-trace backdrop + timeline windowing); the
  Loom *pan control* is wired. T3.4 adds the backdrop the slider rides over and
  brings the timeline into the same model.
- **The timeline is a plain table today (no viewport / no minimap).**
  `draw_timeline` ([timeline_draw.cpp](../../../desktop/src/views/timeline_draw.cpp):8–64)
  is a scrolling `BeginTable` over `dt_timeline::rows` (step order,
  [timeline.h](../../../desktop/src/views/timeline.h):48–50) — there is no
  windowed camera and no whole-trace map. T3.4 gives it an overview strip above
  the table.
- **The Loom canvas draw + its click-to-select.**
  ([fabric_imgui.cpp](../../../desktop/src/loom/fabric_imgui.cpp):220–233) —
  `draw_loom_plan` paints the fabric into `loom-canvas` and maps a click back to
  `(lane, step)` via the camera. The minimap reuses this coordinate math for
  click-to-jump.
- **The ImPlot chassis (doc 15 T1).** `ImPlot::CreateContext()` lives beside the
  ImGui context, **app-only** and guarded on `ImPlot::GetCurrentContext()` so
  headless view tests degrade to text
  ([main.cpp](../../../desktop/src/main.cpp):92–96, 141–142). The minimap's
  whole-trace density strip is an ImPlot draw under the same guard.
- **The test lane.** `desktop-test` is the null-backend headless lane; the
  interaction lane is `desktop-ui-test` (the `uitest` tree,
  [mk/desktop.mk](../../../mk/desktop.mk):338–376, built with
  `-DIMGUI_ENABLE_TEST_ENGINE`), whose `test_ui.cpp`
  ([test_ui.cpp](../../../desktop/test/test_ui.cpp):1–70) drives the **real**
  `draw_shell` through simulated keys/clicks and asserts `ShellState` — the model,
  not pixels (doc 17 T1). `test_nav.cpp` pins router parse/format;
  `test_shell.cpp` pins wiring under the null backend.

## Tasks

### T1 — command palette on `Ctrl+Shift+P` / `Ctrl+P`, over the router  (M, depends on: 16 T2 ImSearch; 17 T1 for the interaction test)

> **LANDED 2026-07-27.** `desktop/src/ui/palette.{h,cpp}` — a pure `PaletteEntry`
> table (`build_palette`) enumerating view-switch, go-to-step/offset, open-recent,
> attach-pid, run-walkthrough, reset-layout, routine/recorded-offset, and a
> NON-dispatching hint per `dt_nav_bindings()` row; every command dispatches ONLY
> through `dt_nav_go` (or the exact `want_view`/`show_*` intent `handle_keymap`
> uses). Opens on `Ctrl+Shift+P` **and** `Ctrl+P` (`handle_keymap`), drawn in
> `draw_shell` beside the go-to modal, filtered with the "showing N of M" idiom +
> the app-only guarded-ImSearch relevance path (degrades to an unranked list under
> the null backend, scoped like `terms.o`). Pinned by
> `desktop/test/test_palette.cpp` (enumeration one-for-one with the bindings,
> want_view, go-to == `dt_nav_parse`, a refusal fills `s.status`) and the
> `flow/command_palette` interaction test in `desktop/test/test_ui.cpp`.

**Goal.** A modal fuzzy finder that makes the whole spine reachable by typing.
Every entry dispatches through `dt_nav_go` (or the exact `s.want_view`/`show_goto`
intent the keymap uses), and the accelerator list is enumerated from
`dt_nav_bindings()` so it can never advertise a key the app does not honour.

**Steps.**
1. **State + open.** Add `bool show_palette` + `char palette_buf[…]` to
   `ShellState` (beside `show_goto`/`goto_buf`,
   [shell.h](../../../desktop/src/ui/shell.h):85–86). Open on
   `Ctrl+Shift+P` **and** `Ctrl+P` in `handle_keymap`
   ([shell.cpp](../../../desktop/src/ui/shell.cpp):586–605) via
   `IsKeyChordPressed`, guarded on `!io.WantTextInput` like the rest. Draw it as a
   modal in `draw_shell` next to `draw_goto_modal`
   ([shell.cpp](../../../desktop/src/ui/shell.cpp):916).
2. **The command model — a pure, testable table.** Build a
   `std::vector<PaletteEntry>` (a new `ui/palette.h`/`.cpp`, so it links into the
   test binary free of ImGui) where each entry is `{label, category, action}` and
   `action` is a `std::function<void(ShellState&)>`. Populate it from:
   - **view-switch** — one per `dt_all_views()`; the action sets `s.want_view`
     (mirroring `handle_keymap`'s `1/2/3/4`, so the visible tab actually
     switches — not only `s.view`).
   - **go-to-step / go-to-offset** — reuse `draw_goto_modal`'s parse: a bare
     number → a step link on the active recording; anything else →
     `dt_nav_parse`; dispatch via `dt_nav_go`.
   - **open-recent** — one per open `ws.recordings[i]` (label = basename +
     disambiguating parent dir, sharing T3.3's helper); the action selects the
     tab via `s.want_open_tab = i`. (True cross-launch recents are T2.5/doc 20;
     this enumerates the open workspace, which needs no persistence.)
   - **attach-pid** — opens the Inspect door (`s.show_inspect = true`), the
     `asmspy --serve` capture host (doc 07).
   - **run-walkthrough** — one per Learn-door card (`s.learn.cards`), opening the
     Learn door at that card (doc 06).
   - **reset-layout** — calls the same `layout_build(…, ReplayInspect)` the View
     menu does ([shell.cpp](../../../desktop/src/ui/shell.cpp):795–797); pairs
     with doc 20's T2.2 always-available reset.
   - **routine/symbol fuzzy match** — one per routine/symbol name known to the
     active recording; the action dispatches `dt_nav_go` to that offset.
   - **the accelerators, from `dt_nav_bindings()`** — one non-dispatching *hint*
     row per binding (`keys` + `what`), so typing surfaces "there is a key for
     this" for all twelve, the doc-17 freshly-wired ones included.
3. **Fuzzy filter + "showing N of M".** Wrap the entry draw in the guarded
   ImSearch idiom from `draw_learn_door`
   ([learn_door.cpp](../../../desktop/src/ui/learn_door.cpp):112–128):
   `BeginSearch`/`SearchBar`/`SearchableItem(label, draw)`/`Submit`, inside
   `if (ImSearch::GetCurrentContext())`; the `else` branch lists unranked so the
   null backend still functions. Show `"showing %zu of %zu"` (the doc-16 idiom).
   Enter runs the top-ranked entry's action and closes; Esc closes.
4. **Dispatch discipline.** Actions route through the spine only — no view
   reaches around `dt_nav_go`/`want_view`/`show_*`. A dispatch that
   `dt_nav_go`-refuses lands its `nav.last_error` in `s.status` verbatim, exactly
   as the keymap/go-to paths already do.

**Tests.** `desktop/test/test_palette.cpp` (null backend, in `desktop-test`),
model-only: assert `build_palette(shell)` **enumerates every `dt_nav_bindings()`
row** (count + `keys`/`what` match, so a future binding cannot silently drop from
the palette); assert a view-switch entry sets `s.want_view` to the right
`dt_view`; assert the "go to <link>" entry produces the same `dt_link` as
`dt_nav_parse` and, dispatched, moves `nav.current` to it; assert a refusing
dispatch fills `s.status`. In `desktop/test/test_ui.cpp` (doc-17
`desktop-ui-test`) add a `flow/command_palette` test: press `Ctrl+Shift+P`, assert
`s.show_palette`, `KeyChars` a query, `ItemClick` the row, assert the resulting
`ShellState` (the model, not pixels) — the same harness style as the existing
keymap/reveal-all flows.

**Docs.** CHANGELOG `Added`: command palette (`Ctrl+Shift+P`/`Ctrl+P`) over the
`dt_nav_go` router. `desktop/README.md`: the palette + its keys and that it is
generated from `dt_nav_bindings`. No new dependency (ImSearch already vendored,
doc 16) → no `licenses/` row.

**Done when.** `Ctrl+Shift+P`/`Ctrl+P` opens a fuzzy palette; every category
dispatches through the spine; the accelerator hints and the palette both derive
from `dt_nav_bindings()` (a test pins the enumeration); it degrades to an unranked
list under the null backend; the interaction flow is green in `desktop-ui-test`.

### T2 — persistent wayfinding chrome outside the per-tab body  (S, depends on: 19 real panes; sourced from `nav.current`)

> **LANDED 2026-07-27.** `desktop/src/ui/wayfinding.{h,cpp}` — `disambiguated_label`
> (basename + the shortest distinguishing parent-dir segment, else a short path
> hash), used in the breadcrumb, the recording tab titles **and** T1's open-recent
> labels so all three agree; `breadcrumb_model` sourced from `nav.current`
> (recording ▸ session ▸ view + step/selection + filter + process scope), prompting
> ("no position — open a recording or press Ctrl+Shift+P") when empty rather than
> blanking. `draw_wayfinding_bar` lives in the OUTER shell — the docked menu bar
> and the windowed top strip, outside every pane — built ON doc-18's back/forward
> affordance (the duplicated "here:" position label was removed from
> `draw_breadcrumb`, not the buttons). Pinned by `desktop/test/test_wayfinding.cpp`.

**Goal.** A persistent global-context band — **recording → session → view**
breadcrumb + active step/selection + filter/scope + thread — sourced from
`nav.current`, living in the outer shell so it is visible from every pane, with
same-basename recordings disambiguated.

**Steps.**
1. **Home = a persistent band, not a tab body.** Draw the chrome in `draw_shell`
   between the menu bar and `BeginTabBar("main")`
   ([shell.cpp](../../../desktop/src/ui/shell.cpp):807–809) — or, once doc 19
   lands the real panes, as a fixed strip in the dock host so it survives tab/pane
   switches. It is **not** inside `draw_recording_tab`, which is exactly the
   per-tab body F9 says the analyst gets lost in.
2. **Source it from `nav.current`.** Read `s.nav.current`
   ([nav.h](../../../desktop/src/nav.h):103): its `rec` (→ recording segment),
   `rec_b` (→ "vs <B>" when diffing), `view` (`dt_view_name`), `step`/`off` (→
   active step/offset), `pid` (→ session/process, the live coordinate). Render as
   a breadcrumb `recording ▸ session ▸ view` with the step/selection trailing.
   When `nav.current` is empty (nothing navigated yet), show "no position — open a
   recording or press Ctrl+Shift+P", never a blank bar.
3. **Filter / scope / thread.** Append the active client-side filter (from the
   view's ImSearch/tree state) and, for a live/observer recording, the thread/pid
   scope, so the band answers "what am I looking at *and* through what filter".
   Where a filter hides rows, mirror the fidelity rule — the band states scope, it
   does not imply the hidden rows are absent (D7; the "showing N of M" ethos).
4. **Disambiguate same-basename tabs.** Two `add.asmtrace` are indistinguishable
   today ([shell.cpp](../../../desktop/src/ui/shell.cpp):871–886). Add a pure
   helper `disambiguated_label(ws, i)` (in `ui/palette.h` or a small
   `ui/wayfinding.h`) that returns the basename plus the **shortest distinguishing
   parent-dir segment** across the open set, or a short path hash when even that
   collides. Use it in the breadcrumb **and** the recording tab titles (so both
   agree), and reuse it for T1's open-recent labels.

**Tests.** `desktop/test/test_wayfinding.cpp` (null backend, `desktop-test`),
model-only: after a deep-link jump via `dt_nav_go`, assert
`breadcrumb_model(shell)` reflects `nav.current` (recording, view name, step);
assert `disambiguated_label` returns distinct strings for two same-basename
recordings in different dirs and stable ones when they differ already; assert the
empty-`nav.current` model yields the "no position" prompt, not a blank. Model the
breadcrumb as a struct/string the test reads — never assert pixels (D4/D7).

**Docs.** CHANGELOG `Added`: persistent wayfinding breadcrumb (recording ▸ session
▸ view + step/filter/thread) with same-basename disambiguation.
`desktop/README.md`: a line on the context band. No new dependency.

**Done when.** A persistent band outside every tab body reflects `nav.current`
after any jump; two same-basename recordings are distinguishable in both the band
and the tab titles; the empty state prompts rather than blanks; `test_wayfinding`
is green. (Depends on doc 19: real panes give the band a home where sibling
context coexists.)

### T3 — always-visible overview/minimap on timeline + Loom, click-to-jump  (M, depends on: 15 T1 ImPlot; 14 T5 ImZoomSlider)

> **LANDED 2026-07-27.** `desktop/src/views/overview.{h,cpp}` — the pure
> `overview_from_timeline` / `overview_from_fabric` / `overview_click_step`
> projection: a compressed view of the recording's OWN rows that NEVER fabricates
> structure (a sparse trace yields a sparse strip — exactly the real steps, pinned
> by `desktop/test/test_overview.cpp`; see Constraints). The timeline strip
> (`draw_timeline_overview` in `timeline_draw.cpp`: an ImPlot density + viewport
> markers + the adopted doc-14 T5 `ImZoomSlider` window control, context-guarded to
> a text ratio under the null backend) and the Loom minimap (an `ImDrawList`
> backdrop behind the Loom's own `ImZoomSlider`, `fabric_imgui.cpp`) both draw the
> whole trace with the current viewport marked and route a click through
> `dt_nav_go` (a new optional `go` on `draw_loom`). Closes doc-14 T5's "not yet
> wired" overview-strip / timeline-windowing follow-on.

**Goal.** An always-visible strip that draws the **whole trace** with the current
viewport marked, on both the timeline and the Loom, with click-to-jump routed
through `dt_nav_go`. This **completes the doc 14 T5 overview-strip stub** (the
part its status calls "not yet wired") by giving the ImZoomSlider a drawn
whole-trace backdrop and bringing the timeline into the same windowed model.

**Steps.**
1. **A shared, pure overview model.** Add `overview_strip(const dt_timeline&)` /
   the Loom equivalent that returns a whole-trace density series (steps → a count
   the strip draws) plus the current viewport `[lo, hi]`. Pure and tested; the
   draw half consumes it. Borrow Tracy's `TimelineController` idea — the map is a
   compressed projection of the same rows the main view shows, never a separate
   layout.
2. **Timeline strip.** Above `draw_timeline`'s table
   ([timeline_draw.cpp](../../../desktop/src/views/timeline_draw.cpp):8–64) draw
   the density series as an ImPlot area/heat strip, **guarded on
   `ImPlot::GetCurrentContext()`** so the null backend degrades to a text ratio
   (doc 15 T1, [main.cpp](../../../desktop/src/main.cpp):92–96). Overlay the
   current viewport rectangle (the table's visible scroll range). Adopt the
   `ImZoomSlider` from the Loom
   ([fabric_imgui.cpp](../../../desktop/src/loom/fabric_imgui.cpp):184–199) as the
   timeline's window control — this is precisely doc 14 T5's "timeline windowing"
   follow-on.
3. **Loom strip.** Draw the whole-fabric density behind the existing Loom
   `ImZoomSlider` ([fabric_imgui.cpp](../../../desktop/src/loom/fabric_imgui.cpp):184–199)
   so the slider rides over a visible map of what it is windowing, not an empty
   track. Reuse the fabric's own step/lane density (`loom_plan` output) — no new
   structure.
4. **Click-to-jump through the spine.** A click on either strip maps the x
   position to a step (reusing the Loom canvas's coordinate math,
   [fabric_imgui.cpp](../../../desktop/src/loom/fabric_imgui.cpp):220–233, and the
   timeline's row/step mapping) and dispatches `dt_nav_go` with a step link on the
   active recording — so a minimap click and a typed go-to land identically (D4).

**Tests.** `desktop/test/test_overview.cpp` (null backend, `desktop-test`),
model-only: assert `overview_strip` returns one bucket per trace region with the
viewport `[lo,hi]` matching the current window (whole trace covered, viewport
correct); assert a click at fractional position `p` produces the `dt_link` for
`step = round(p·nsteps)` and that dispatching it moves `nav.current` there; assert
the **strip's series is derived from the recording's own `dt_timeline`/fabric —
never a fabricated/padded layout** (feed a sparse trace, assert the strip has
exactly the real steps, no invented buckets). Model state, not pixels; the ImPlot
draw is context-guarded so these run headless.

**Docs.** CHANGELOG `Added`: always-visible overview/minimap on timeline + Loom
(whole trace, viewport marked, click-to-jump); note it completes the doc 14 T5
overview-strip stub. `desktop/README.md`: the minimap + click-to-jump. No new
dependency (ImPlot doc 15, ImZoomSlider doc 14 already vendored).

**Done when.** Both timeline and Loom show an always-visible strip drawing the
whole trace with the current viewport marked; a click jumps via `dt_nav_go`; the
strip draws only real steps/regions (a test pins no-fabrication); the doc 14 T5
"not yet wired" overview-strip/timeline-windowing follow-on is closed; headless
tests green.

## Task order & parallelism

All three are independent layers over `dt_nav_go` and share no files beyond
`ShellState` and a small disambiguation helper (T1 and T2 both use it — land it in
whichever goes first, in `ui/wayfinding.h`). **T2 is the hard-blocked one:** it
needs doc 19's real panes to have a home outside a single tab, so start it after
19 lands (its pure breadcrumb model + disambiguation helper + tests can be written
first, against the null backend, and wired into the pane host when 19 is ready).
T1 needs only doc 16 (landed) for the ranked filter and doc 17 (landed) for the
interaction test. T3 needs docs 15 T1 and 14 T5 (both landed). Suggested order:
**T1 and T3 in parallel now; T2 after doc 19.** Different developers, no conflict.

## Constraints & gates

- **Everything routes through the spine.** No task adds a second navigation path:
  palette entries, minimap clicks, and breadcrumb jumps all dispatch `dt_nav_go`
  (or the exact `want_view`/`show_*` intent `handle_keymap` uses), so a typed
  target, a clicked link, a keypress, and a minimap click are indistinguishable at
  the model (D4). A refusal lands in `s.status` verbatim, never a silent no-op.
- **The overview never fabricates structure (D7).** Docs 04/08 ban deterministic
  layout that invents relationships; the minimap is a compressed projection of the
  recording's *own* `dt_timeline`/fabric rows — a sparse trace yields a sparse
  strip, not a padded one. A test pins this. This is the load-bearing fidelity
  constraint of this brief: a map that fills gaps to look complete would lie about
  coverage exactly where PT-scale users most need the truth.
- **Fidelity chrome is restructured, never removed (D7 / review F5).** The
  wayfinding band surfaces scope/filter/provenance; where a filter hides rows the
  band states the scope (the "showing N of M" ethos), it does not imply the hidden
  rows are gone.
- **App-only render deps degrade under the null backend.** ImSearch (T1) and
  ImPlot (T3) draws are guarded on `…::GetCurrentContext()` so `desktop-test` and
  the render-only viewer function without them — the pure models and their tests
  do not depend on a context (D2 addon-admission rule; both deps already
  vendored).
- **No new dependency, no schema change.** All three are wiring on shipped
  substrate; no `licenses/` row, no `.asmtrace` field (D5).
- **Every task is headlessly testable.** Pure models in `desktop-test`; the
  palette's `Ctrl+Shift+P` interaction additionally in the doc-17
  `desktop-ui-test` lane. Nothing here is manual-smoke-only.

## Out of scope

- **Cross-launch recents / session restore** (F10) — the palette's open-recent
  enumerates the *open* workspace only; persisted MRU is doc 20 (T2.5).
- **Router back/forward history / nav stack** (F11, T3.2) — `nav.current` holds
  only the latest position; a history ring is a separate brief (doc 18 wave-0
  scope), and this brief must not grow one.
- **Shared brushing-and-linking selection** (F7, T1.2) and **global find**
  (F17, T1.4) — sibling Wave-2 work in [22-selection-and-search.md](22-selection-and-search.md).
- **Rebindable keys / hotkey editor** — ImHotKey is broken on the pin (doc 11/17
  out-of-scope); the palette makes keys *discoverable*, not *editable*.
- **CVD-safe minimap colormap** (T5.2) and **the unified filter/time widget**
  (T5.4) — Wave-3 visual-language work in [24-one-visual-language.md](24-one-visual-language.md).
