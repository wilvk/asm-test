# Wave 2: operate the spine — shared selection, keyboard islands, global find, app-level undo — implementation

> **Sources.** Actioned from the UX restructure plan
> (../plans/desktop-gui-ux-restructure-plan.md) rows **T1.2, T1.3, T1.4, T1.5**
> (Theme 1, "Make the spine operable"; Wave 2) and the review findings **F7, F18,
> F17, F12** (../plans/desktop-gui-ux-review.md). Written 2026-07-27 against HEAD
> `243f092`. This doc wins over the review/plan on disagreement; the CODE wins
> over this doc — re-verify file:line before editing.
> Read [_conventions.md](../implementations/_conventions.md) first; D1–D11 live
> in this directory's [README](README.md). **Prerequisites:
> [19-dockable-panes-keystone.md](19-dockable-panes-keystone.md)** (real panes —
> T1 links across visible panes at once), **[16-live-feedback-and-filtering.md](16-live-feedback-and-filtering.md)**
> (ImSearch client-side narrowing — T3 extends it),
> **[21-spine-navigation.md](21-spine-navigation.md) T3** (the timeline
> overview/minimap — T3 draws its hits there), and
> **[17-interaction-testing-and-editor.md](17-interaction-testing-and-editor.md)**
> (the `desktop-ui-test` engine lane every interaction test here rides, and the
> Author editor's *own* undo, from which T4's app-level stack is deliberately
> distinct).

## Why this work exists

- **F7 — selection is one-pane navigation, not brushing-and-linking.** Every view
  holds its own selection, so the analyst re-finds the same address by hand in
  each pane — the exact recall load recognition-over-recall exists to remove. The
  dataviz convention (Perfetto's linked Current Selection, speedscope's sandwich)
  is *one* selection model that cross-highlights the same entity everywhere.
- **F18 — the 3D camera, slice DAG and Loom canvas are mouse-only islands.**
  Because ImGui exposes no OS screen-reader tree, keyboard operability is the only
  accessibility substitute — yet the 3D HUD has no keyboard camera even designed,
  the `SceneHost` is not Tab-reachable, and a keyboard-only analyst cannot orient
  in 3D or light a slice cone.
- **F17 — no global find / search-as-measurement.** An expert cannot answer "where
  and how much does mnemonic/address/symbol X occur" without manual scrolling;
  client-side search exists only on the Learn cards. There is no highlight-all,
  match count, aggregate cost, or Enter/Shift+Enter cycling.
- **F12 — undo/redo exists only in the Author editor.** An analyst who applies an
  aggressive filter, lights the wrong cone, or forks exploratory Loom takes has no
  reversal path, and the takes gutter accumulates forks with no remove/clear.
  Exploration is unsafe, pushing users toward timidity.

Standing rule from the plan (§Framing) and F5: honesty (D7) is *restructured,
never removed*. None of these tasks hides a truth — the selection model
cross-highlights only where an entity actually appears, find *measures* rather
than filters-away, the call **tree** stays engine-filtered so surviving depths
never lie, and undo restores honest state rather than a faked one.

## What already exists (verified 2026-07-27)

- **Per-view selection state, scattered — exactly what T1 unifies.** The shell
  carries `selected_step`, `selected_off`, `cone_active`, `cone_fwd`
  ([shell.h](../../../desktop/src/ui/shell.h):70-76). Each view then keeps its own
  copy: the timeline's `dt_timeline::selected_step`
  ([timeline.h](../../../desktop/src/views/timeline.h):52), the slice explorer's
  `dt_slice_view::selected_step`
  ([slice_view.h](../../../desktop/src/views/slice_view.h):46, set at
  [slice_view.cpp](../../../desktop/src/views/slice_view.cpp):45), the Loom
  camera's `loom_view_t::selected_steps`
  ([fabric_plan.h](../../../desktop/src/loom/fabric_plan.h):73, populated at
  [fabric_imgui.cpp](../../../desktop/src/loom/fabric_imgui.cpp):214, dimming at
  [fabric_plan.cpp](../../../desktop/src/loom/fabric_plan.cpp):134-137), and the
  Loom panel's `sel`/`has_selection`/`lane`/`playhead`
  ([loom_draw.h](../../../desktop/src/loom/loom_draw.h):38-42).
- **One choke point already writes all of it.** `shell_wire_nav`'s `go()` lambda
  ([shell.cpp](../../../desktop/src/ui/shell.cpp):147-166) is the single place a
  deep link lands: it sets `view`, `active_tab`, `b_index`, `selected_step`,
  `selected_off` and lights the cone — registered for every view
  (shell.cpp:169-170). Plan D4's rule is stated inline there ("no view keeps its
  own navigation state, so a link and a keypress land identically"). This is the
  seam T1 promotes to a shared selection model.
- **The router's link type is the ready-made entity id.** `dt_link` is a
  serialisable `{rec, rec_b, view, step, off, pid}`
  ([nav.h](../../../desktop/src/nav.h):58-68) with byte-stable
  `dt_nav_format`/`dt_nav_parse` (nav.h:80,84) and `dt_nav_table::current`
  (nav.h:103) — so a "selected entity" and an "undo entry" are both just links.
- **The keymap core is wired and centralised.** `handle_keymap`
  (shell.cpp:586-699, doc 17 T1) already moves pure `ShellState` for `1/2/3/4`
  (`want_view`, shell.cpp:594-597), `j/k`+arrows+PgUp/Dn (shell.cpp:618-626),
  `Enter` (open slice + cone, shell.cpp:629-633), and **`b`/`f`/`c` cones**
  (shell.cpp:636-638) — guarded on `io.WantTextInput` (shell.cpp:588). So the
  slice cone keys T1.3 wants already move model state; the mouse-only gap T2 fills
  is the **3D camera** and Tab-reach, not the cone keys.
- **The 3D pane is mouse-only.** `draw_scene_overview` (shell.cpp:253-377) draws
  the HUD (shell.cpp:280), applies `req_reset_view`/`req_top_down`
  (shell.cpp:281-285), then orbits/dollies/picks **only inside `IsItemHovered()` on
  mouse events** (shell.cpp:354-376). The HUD itself exposes the intents as
  `HudState{playhead_moved, req_reset_view, req_top_down}`
  ([hud.h](../../../desktop/src/scene3d/hud.h):21-33) with a `SliderInt` playhead
  and "reset view"/"top-down (2D-ish)" buttons
  ([hud.cpp](../../../desktop/src/scene3d/hud.cpp):78,97-101). The camera is pure
  math already — `orbit`/`dolly`/`reset`/`top_down`
  ([camera.h](../../../desktop/src/scene3d/camera.h):51-68) — so a keyboard scheme
  is new *input*, no new geometry.
- **`SceneHost` is an abstract, GL-only bridge** ([scene_host.h](../../../desktop/src/ui/scene_host.h):49-69),
  null under the null test backend; the pane already degrades to models+HUD+placard
  when it is absent (shell.cpp:308-314). Tab-focus is a shell-side concern, not a
  GL one.
- **The slice DAG draws from the built cone, mouse-panned.**
  `draw_slice_view` ([slice_view_draw.cpp](../../../desktop/src/views/slice_view_draw.cpp):42-121)
  colours nodes by cone (slice_view_draw.cpp:24-38) and pans/zooms its canvas only
  on hovered mouse events (slice_view_draw.cpp:106-120). The cone content is a pure
  function of `selected_step` (shell.cpp:408-423), so once a key sets the
  selection the DAG re-lights with no mouse.
- **Client-side narrowing already exists, in two honesty-aware shapes.** ImSearch
  filters the Learn catalog — `BeginSearch`/`SearchBar`/`SearchableItem`/`Submit`,
  guarded on `ImSearch::GetCurrentContext()` so the null backend shows the plain
  list ([learn_door.cpp](../../../desktop/src/ui/learn_door.cpp):116-128;
  `CreateContext` at [main.cpp](../../../desktop/src/main.cpp):96). The syscalls
  view uses a hand-rolled **order-preserving** filter instead —
  `obs_syscall_filter_indices` returns matching indices *in order* and the draw
  shows "showing N of M" ([syscalls.h](../../../desktop/src/views/syscalls.h):92-107,
  doc 16 T2) — precisely because ImSearch's relevance re-ranking would scramble a
  syscall stream's execution order. Both are the templates T3 extends.
- **The call tree is engine-filtered, and must stay so (D7).**
  `asmspy_tree_filter_t` bounds what the engine *emits* while it keeps tracking;
  the panel filters nothing itself, because a client-side filter over an
  unfiltered stream would show surviving depths that lie
  ([tree.h](../../../desktop/src/views/tree.h):1-13,41-43). T3 must not touch this.
- **Search targets carry their own cost/text.** `dt_timeline_row{off, disasm,
  ann, n_in, n_out}` (timeline.h:34-46) and `HotEdge{from, to, count, mispred}`
  ([hotedges.h](../../../desktop/src/views/hotedges.h):~40-50) already hold the
  "how much" a search-as-measurement aggregates; the disasm pane resolves bytes
  per code-image version ([disasm.h](../../../desktop/src/views/disasm.h)).
- **The Loom fork engine + take model exist, but no persistent gutter does.**
  `loom_take_run` builds one take (`forks.h`/`forks.cpp`, **full build only,
  engine-linked** — [forks.h](../../../desktop/src/loom/forks.h):1-17), and
  `loom_take_view` + `loom_take_node_t` describe its result purely
  ([take_view.h](../../../desktop/src/loom/take_view.h):63-93), each unit-tested
  (`test_loom_forks.cpp`, `test_loom_takeview.cpp`). **Drift vs the review:** F12
  says "the takes gutter accumulates fork nodes with no remove/clear", but
  `LoomState` (loom_draw.h:29-42) holds **no vector of takes** today — the
  multi-take gutter is doc 05's design (05:393-408) not yet wired. So T4 *adds* the
  accumulator together with its remove/clear, rather than only bolting controls
  onto an existing list.
- **Test substrate.** Null-backend model tests are `desktop/test/test_*.cpp` files
  listed in `DESKTOP_TESTS` ([mk/desktop.mk](../../../mk/desktop.mk):796+) and run
  by `make desktop-test`. Interaction tests (keys, `Ctrl+F`, `Ctrl+Z`, real
  `ItemClick`/`KeyPress`) live in `desktop/test/test_ui.cpp` and run headless on
  the null backend under `make desktop-ui-test` (the `uitest` tree,
  `-DIMGUI_ENABLE_TEST_ENGINE`; mk/desktop.mk:1277-1389) — its `keymap_gui`
  frame-driver + `ensure_keymap_shell` fixture (test_ui.cpp:39-60) is the pattern
  every new interaction test copies.

## Tasks

### T1 — one shared brushing-and-linking selection model  (L, depends on: [19](19-dockable-panes-keystone.md))

**Goal.** Promote selection to **one** Workspace/shell-level model — an entity id
resolving to `{step, offset, lane}` — held distinctly from navigation, so a pick
in any pane cross-highlights the *same* entity in every pane at once (detail /
disasm / Loom / 3D) and the per-view `selected_step`/`cone_active`/clicked-worldline/
row copies are replaced by reads of the one model (F7). This is the payoff Wave 1's
real panes (doc 19) unlock: cross-highlighting is only visible once sibling panes
coexist.

**Steps.**
1. **Define the model.** Add a `Selection` struct to `ShellState` — the canonical
   entity as a `dt_link`-shaped `{rec, step, off, lane}` plus a monotonically bumped
   `epoch` (so a pane can cheaply notice "selection changed since I last drew").
   Keep the existing cone fields (`cone_active`/`cone_fwd`) *beside* it: a cone is a
   derived highlight *of* the selection, not a second selection. Distinct from
   navigation — `nav.current` still records "where the view is pointed"; the
   Selection records "what entity is brushed", and a pick sets both while a plain
   view-switch (`1/2/3/4`) sets only nav.
2. **Make the router set the shared model.** In `shell_wire_nav`'s `go()` lambda
   (shell.cpp:147-166) write the new `Selection` (rec/step/off) in one place and
   bump `epoch`; `handle_keymap`'s step/cone keys (shell.cpp:618-638) write the same
   model rather than `s.selected_step` directly. One writer, as D4 already intends.
3. **Make each view *read* the shared model.** Replace the per-view selection copies
   with a projection from the shared `Selection` at draw time: `dt_timeline_build`
   takes the selected step from it (drop `dt_timeline::selected_step` as owned
   state), `dt_slice_view_build` reads it (slice_view.cpp:45), the Loom camera's
   `selected_steps` (fabric_imgui.cpp:214) is derived from it, and the 3D pane's
   drill maps a picked scene id → the same `Selection`. A pane that cannot show the
   entity (no such offset in its stream) shows *nothing selected* — never a
   fabricated row (the honesty floor, mirroring how `n/p` refuses rather than
   moving to an unjustifiable position, shell.cpp:666-696).
4. **Cross-highlight, don't cross-navigate.** A pick brushes the entity everywhere
   it appears but only the active pane scrolls to it; the others highlight in place
   (dim outside, as `fabric_plan.cpp:134-137` already does for `selected_steps`), so
   selecting a mispredict/taint-sink surfaces its disasm/lineage without yanking
   every pane's viewport (Tognazzini anticipation, F7).

**Tests.** New `desktop/test/test_selection.cpp` (null backend, in `DESKTOP_TESTS`):
build a shell over a golden recording, set the shared `Selection` to a step, and
assert the timeline model, the slice-view model, and the Loom camera's
`selected_steps` all report that same entity — i.e. a pick in pane A cross-
highlights the same entity id in panes B/C from **one** model (model state, not
pixels, D4). Assert an offset absent from a pane yields "nothing selected" there,
never a synthesised row. A `desktop-ui-test` section (test_ui.cpp) drives a real
`ItemClick` in one pane and asserts the shared `Selection.epoch` bumped and the
other panes' derived state followed.

**Docs.** CHANGELOG `Changed`: "Selection is now one shared brushing-and-linking
model — a pick in any pane cross-highlights the same entity in detail/disasm/Loom/3D
at once." `desktop/README.md`: a short "Selection vs navigation" note (brush vs
point). No new dependency, no `licenses/` row.

**Done when.** one `Selection` lives on `ShellState`; the per-view `selected_step`/
row/worldline copies are gone or derived; a pick in any pane cross-highlights the
same entity everywhere it appears (and only there); panes that cannot show it say
so; `test_selection` + the ui-test section are green.

### T2 — reach the mouse-only islands: keyboard camera, Tab focus, cone keys  (M, depends on: T1 optional; ties to [18](18-breach-stops.md) F1)

**Goal.** Make the 3D overview and slice DAG operable without a mouse (F18):
keyboard camera on the 3D HUD (arrows orbit, `+`/`-` dolly, keys for reset and the
honest top-down fallback), Tab focus into every pane and the `SceneHost` viewport,
and — already landed for cones (shell.cpp:636-638) — confirm `b`/`f`/`Enter` light
the slice DAG from selection so it is operable keyboard-only.

**Steps.**
1. **Keyboard camera in the HUD.** In `draw_scene_hud` (hud.cpp), when the HUD (or
   the viewport `Image`, step 2) has focus, map `Left`/`Right`/`Up`/`Down` →
   `cam.orbit(±dyaw, ±dpitch)`, `+`/`-` (and `=`) → `cam.dolly()`, `R` →
   `req_reset_view`, `T` → `req_top_down`, `[`/`]` or `,`/`.` → nudge the playhead
   `t` (setting `playhead_moved`). Route them through the *same* `HudState` intents
   the buttons already set (hud.h:28-32) and the same `Camera` methods the mouse
   drag uses (camera.h:51-68), so keyboard and mouse are one code path — the
   top-down fallback stays the honest "3D to find, 2D to read" collapse (camera.h:64,
   hud.cpp:100-103). Guard on `io.WantTextInput` exactly as `handle_keymap` does
   (shell.cpp:588).
2. **Tab-reach the panes and the viewport.** Give the 3D pane's `ImGui::Image`
   (shell.cpp:347) an focusable `InvisibleButton`/`ItemAdd` hit-target (the HUD
   region is already an ImGui window and Tab-reachable) so the `SceneHost` viewport
   is reachable by Tab and carries a visible focus ring; when it has focus, the
   step-1 keys drive the camera. Under the null backend (`scene_host == nullptr`,
   shell.cpp:308) the focus target and key handling still exist — the camera moves
   `HudState`/`Camera` state with no GL, which is what makes it headlessly testable.
3. **Slice DAG keyboard operability.** Confirm and pin that `b`/`f` light the
   backward/forward cone and `Enter` opens the slice at the selection
   (shell.cpp:629-638) so `draw_slice_view` re-lights with no mouse (its content is
   a pure function of the selection, slice_view.cpp/shell.cpp:408-423); add `Home`/
   `End`-style keys to fit the canvas view (slice_view_draw.cpp:106-120 is otherwise
   drag-only) so a keyboard user is not stranded off-screen. No cone geometry
   changes — input only.

**Tests.** Extend `test_camera.cpp` (pure, null-free) to assert the key→`Camera`
mapping: an orbit key changes `yaw`/`pitch` within clamps, a dolly key changes
`radius` within `[kMinRadius, kMaxRadius]`, `T` yields the top-down pose
(camera.h:64-68). A `desktop-ui-test` section (test_ui.cpp) focuses the 3D pane
under the null backend and asserts arrow/`+`/`-`/`R`/`T` keypresses move `SceneView`
camera + `HudState` (model state, no pixels, no GL — this is why it must be the null
lane, per CLAUDE.md: a lane that could only self-skip is not a test) and that Tab
reaches the viewport target. Cone-key operability is already covered by the doc-17
keymap tests; add an assert that `Enter`/`b`/`f` leave the slice view drawable with
no mouse event.

**Docs.** CHANGELOG `Added`: "Keyboard camera for the 3D overview (arrows orbit,
`+`/`-` dolly, `R` reset, `T` top-down), Tab-focusable panes and 3D viewport, and
keyboard-operable slice cones." `desktop/README.md`: the 3D/slice key table. No new
dependency.

**Done when.** the 3D camera orbits/dollies/resets/top-downs from the keyboard
through the existing `HudState`/`Camera` seam; Tab reaches every pane and the
`SceneHost` viewport with a visible focus ring; the slice DAG lights and fits from
keys alone; the pure + ui-test assertions are green and run under the null backend.

### T3 — global find: search-as-measurement  (M, depends on: [16](16-live-feedback-and-filtering.md), [21](21-spine-navigation.md) T3)

**Goal.** A global find on `Ctrl+F` that highlights **all** hits in the timeline and
the minimap, reports the match **count** and the aggregate **cost**, and cycles
matches with Enter / Shift+Enter — a find that is a measurement, not just a jump
(F17). Extend the existing client-side narrowing (doc 16) to syscalls / disasm /
hot-edges; keep the call **tree** engine-filtered so surviving depths never lie
(D7).

**Steps.**
1. **A find model on `ShellState`.** Add `FindState{query, matches (vector of
   entity refs = step/offset/edge), active_index, total_cost}`; `Ctrl+F` opens it
   (a `IsKeyChordPressed(Ctrl|F)`, mirroring `Ctrl+G` at shell.cpp:604). The model
   is pure — a function of the active recording's decoded streams — so it is
   unit-testable with no draw.
2. **Compute matches + cost per surface.** Over the active recording, match `query`
   (case-insensitive substring) against the timeline rows' `disasm`/`ann`/offset
   (timeline.h:34-46), the disasm listing (disasm.h), the syscalls' order-preserving
   `line` (reuse `obs_syscall_filter_indices`, syscalls.h:106), and the hot-edge
   `from`/`to` labels (hotedges.h). For each surface aggregate the "how much": step
   count for the timeline/disasm, sample `count` summed for hot-edges
   (hotedges.h) — that sum is the search-as-measurement payoff ("this mnemonic
   retires N samples across M sites"). Keep matches **in stream order** (like
   syscalls, syscalls.h:100-105), never relevance-ranked.
3. **Highlight-all, don't filter-away.** Find *marks* hits; it never removes rows.
   Draw the hit set in the timeline and — reusing doc 21 T3's overview/minimap as
   the place hits are painted — as ticks on the minimap, with "N matches ·
   \<aggregate cost\>" in the find bar. Enter / Shift+Enter advance/retreat
   `active_index` and drive `dt_nav_go` to that entity (so cycling reuses the one
   spine). This is the honesty distinction from a filter: nothing is hidden, so
   "showing N of M" is unnecessary — every row stays, hits are merely lit.
4. **Extend the narrowing filter (not the tree).** Add the doc-16 client-side
   "showing N of M" type-to-narrow filter (the syscalls idiom, syscalls.h:92-107) to
   the disasm and hot-edges lists where narrowing the *display* is honesty-safe.
   **Do not** add a client-side filter to the call tree: it stays engine-filtered
   (tree.h:1-13) because a client filter over an unfiltered stream would leave
   surviving depths claiming a parentage the engine never emitted (D7). State this
   refusal in the code comment, as the tree already does.

**Tests.** New `desktop/test/test_find.cpp` (null backend): over a golden recording,
run a query and assert the match **count** and the aggregate **cost** the model
reports (e.g. summed hot-edge samples), assert matches are in stream order, and
assert Enter/Shift+Enter step `active_index` and target the right entity via the
router. A guard test asserts the call tree's filter is still the engine-side one
(no client-side tree narrowing was added) — surviving depths unchanged. A
`desktop-ui-test` section drives `Ctrl+F`, types a query, and asserts the model +
that Enter cycles (model state, D4).

**Docs.** CHANGELOG `Added`: "Global find (`Ctrl+F`): highlight-all across timeline
+ minimap, match count and aggregate cost, Enter/Shift+Enter cycling; type-to-narrow
filtering extended to disasm and hot-edges (the call tree stays engine-filtered)."
`desktop/README.md`: the find key + the "search is a measurement" note. No new
dependency (ImSearch already vendored, doc 16).

**Done when.** `Ctrl+F` opens a find that highlights every hit in timeline + minimap,
reports count and aggregate cost, and cycles with Enter/Shift+Enter through the
router; disasm and hot-edges gain the narrowing filter; the call tree is untouched
and provably still engine-filtered; `test_find` + the ui-test section are green.

### T4 — app-level command / undo stack  (M, depends on: [17](17-interaction-testing-and-editor.md) T2)

**Goal.** An app-level command/undo stack on `Ctrl+Z`/`Ctrl+Y` over reversible
view-model state — the filter predicate, the perspective/layout, the cone/selection,
the Loom take set — **distinct** from the Author editor's own text undo (doc 17 T2,
which owns its buffer); and give the Loom takes gutter per-take **remove** plus a
**clear forks** action (F12).

**Steps.**
1. **A command stack on `ShellState`.** Add `struct Command { enum kind; /* before
   */ ; /* after */ }` and an `UndoStack{vector<Command>, cursor}`. Reversible state
   is small and already serialisable: the shared `Selection` + cone (T1), the active
   filter predicate (tree/syscalls/find query), the perspective/layout id (doc 19
   panes), and the Loom take set (step 3). Represent each command's before/after as
   the value itself (a `dt_link`-shaped selection, a filter string, a layout id) —
   no diffing needed at this scale. `Ctrl+Z`/`Ctrl+Y` (`IsKeyChordPressed`, like
   `Ctrl+G` at shell.cpp:604) pop/replay, guarded on `io.WantTextInput` so they do
   not steal the Author editor's own undo.
2. **Record the reversible mutations.** Push a `Command` when the user changes a
   filter, switches perspective, lights/clears a cone or moves the selection
   (T1's one writer, shell.cpp:147-166 + handle_keymap), or adds/removes a take.
   Explicitly **do not** record: navigation-only view switches (`1/2/3/4` — those go
   through the back/forward history of doc 21 T3.2, a different stack) and the Author
   text buffer (its own undo, doc 17 T2). State that boundary in the code comment so
   the two undos never entangle.
3. **The Loom takes gutter — build the accumulator + remove/clear.** `LoomState`
   (loom_draw.h:29-42) holds no take set today (see the drift note above), so add
   `std::vector<loom_take_node_t>` (the pure view model, take_view.h:63-78) to it,
   append each `loom_take_run` result (forks.h — full-build tab only; the render-only
   viewer shows recorded takes but assembles none), and draw the gutter with a per-
   node **[remove]** and a gutter-level **[clear forks]**. Removing a take and
   clearing the set are `Command`s (step 1), so `Ctrl+Z` restores a removed take.
   Each take keeps its `disclosure`/`err` verbatim while shown (take_view.h:68-70),
   so clearing forks never quietly drops a take's loud refusal — it removes the whole
   node, refusal and all, reversibly.

**Tests.** New `desktop/test/test_undo.cpp` (null backend): apply a filter change, a
cone change, and a take add/remove, then assert `Ctrl+Z` reverses each to the exact
prior value and `Ctrl+Y` re-applies it (model state, D4); assert the Author text
buffer's undo is *not* touched by the app stack (distinct stacks). Extend
`test_loom_takeview.cpp` / a new `test_loom_gutter.cpp` to assert per-take remove
drops one node and "clear forks" empties the set, both reversible. A
`desktop-ui-test` section drives `Ctrl+Z`/`Ctrl+Y` and the gutter buttons via real
clicks.

**Docs.** CHANGELOG `Added`: "App-level undo/redo (`Ctrl+Z`/`Ctrl+Y`) over filter /
layout / cone / selection / take-set state — distinct from the Author editor's text
undo; Loom takes gutter gains per-take remove and clear-forks." `desktop/README.md`:
"Two undos — the Author editor's text vs the app's view-model" note. No new
dependency.

**Done when.** `Ctrl+Z`/`Ctrl+Y` reverse and replay a filter/cone/selection/take
change without touching the Author buffer's own undo; the Loom takes gutter accumulates
takes with working per-take remove and clear-forks, both reversible and honesty-
preserving; `test_undo` + the Loom gutter test + the ui-test section are green.

## Task order & parallelism

T1 is the keystone of this brief and is gated on doc 19's real panes (cross-
highlighting is only visible once sibling panes coexist), so start it once 19
lands. T2 (keyboard islands) is independent of T1 — it moves camera/HUD model
state and can proceed in parallel on the doc-18/F1 keymap foundation. T3 (global
find) depends on doc 16 (ImSearch) and doc 21 T3 (minimap, where hits are drawn) —
begin its pure find *model* immediately; wire the minimap highlight after 21 T3
lands. T4 (undo) reads best after T1 (the shared `Selection` is one of its
reversible values) but its Loom-gutter half is independent and can start any time.
All four end in the shared `desktop-ui-test` lane, so land them so their test_ui.cpp
sections do not collide (append, do not rewrite the fixture — the concurrent-agents
rule).

## Constraints & gates

- **Honesty is restructured, never removed (D7 / F5).** T1's selection shows
  "nothing selected" where an entity is absent rather than a fabricated row; T3's
  find *marks* hits and never hides rows, and the call tree stays engine-filtered so
  surviving depths never lie (tree.h:1-13); T4's clear-forks removes a whole take
  node with its verbatim refusal intact, never silently dropping a loud failure.
  State each of these in the relevant code comment.
- **Model state, not pixels (D4).** Every assertion here is over `ShellState` /
  view-model / `Camera` / `HudState` values under the null backend; interaction
  timing (keys, `Ctrl+F`, `Ctrl+Z`, gutter clicks) rides the doc-17
  `desktop-ui-test` engine lane (mk/desktop.mk:1379-1389). No task is manual-smoke
  only — the keyboard camera is deliberately testable *because* it moves model state
  with no GL (CLAUDE.md: a lane that can only self-skip is not a test).
- **No new dependency.** ImSearch (doc 16) and imgui_test_engine (doc 17 T1) are
  already vendored; this brief adds no `scripts/third-party-digests.txt` row and no
  `licenses/` capture. New null-backend tests join `DESKTOP_TESTS`
  (mk/desktop.mk:796+); interaction sections join `test_ui.cpp`.
- **Two undos stay distinct (T4).** The app stack must never consume the Author
  editor's text-undo keys, and vice versa — both guard on `io.WantTextInput` and own
  disjoint state.
- **Selection is distinct from navigation (T1).** `nav.current` points a view; the
  shared `Selection` brushes an entity. A pick sets both; a view switch sets only
  nav; find-cycling (T3) and back/forward (doc 21 T3.2) drive nav, not the undo
  stack.

## Out of scope

- **The command palette** (`Ctrl+Shift+P`, T3.1) and **back/forward router history**
  (`Alt+Left/Right`, T3.2) — doc 21 (spine navigation). T3's Enter-cycling reuses
  `dt_nav_go` but adds no history stack.
- **Real dockable panes** (T2.1) — doc 19, a hard prerequisite for T1, not built
  here.
- **The Author editor's own text undo/redo and find/replace** — doc 17 T2, already
  landed; T4's app stack is deliberately distinct from it.
- **Rebindable keys / a hotkey editor** — ImHotKey is verified broken on 1.91.9
  (doc 11 skip); the keys here are fixed accelerators.
- **Convention-alignment of the keymap** (`F` fit, `,`/`.` sibling, WASD) — that is
  T1.1's tail in doc 18; this brief only adds the 3D-camera and find/undo keys.
- **The graded honesty-chrome severity field** (T4.1) and **the semantic/CVD
  palette** (T5.x) — Wave 3, docs 23/24.
