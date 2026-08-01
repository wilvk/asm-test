# Wave 1 keystone: real dockable panes + flatten the 3-deep tab nesting — implementation

> **Sources.** Actioned from the UX restructure plan
> (../archive/plans/desktop-gui-ux-restructure-plan.md) row **T2.1** and the review
> findings **F2, F4, F9** (../archive/reviews/desktop-gui-ux-review.md). Written 2026-07-27
> against HEAD `243f092`. This doc wins over the review/plan on disagreement; the
> CODE wins over this doc — re-verify file:line before editing (the review's
> `shell.cpp:564/586/705`, `layout.cpp:56-71` citations are from the pre-`243f092`
> tree and have drifted; doc 17 T1 inserted `handle_keymap` and the go-to modal
> above `draw_shell`, so every shell line is now ~200 lower — the current numbers
> are captured below).
> Read [_conventions.md](../implementations/_conventions.md) first; D1–D11 live
> in this directory's [README](README.md). **Prerequisites:
> [13-foundation-moves.md](13-foundation-moves.md) F1** (the `v1.91.9b-docking`
> imgui pin + `layout.cpp` DockBuilder — landed; its panes are the orphans this
> brief adopts). **Depends-on (land alongside):
> [18-breach-stops.md](18-breach-stops.md) T2.2** (real Reset with auto-fallback)
> — once panes are real, a stale/corrupt dock `.ini` can strand the user, so the
> recovery path must exist in the same wave.

## Why this work exists

- **F2 — the docking layout manager is wired to nothing.** `layout.cpp` docks
  named windows `kPaneHome/Recording/Scrubber/Inspector/Timeline` and `shell.cpp`
  opens a `DockSpaceOverViewport` with a full View menu (Reset + three presets),
  but **no view is `Begin()`'d under any `kPane*` name anywhere in
  `desktop/src/`** — the shell only ever `Begin()`s `"asmtest"` and
  `"Keyboard bindings"`. So the dockspace, every preset, tear-out and Reset act on
  phantom windows and are all inert. The promised workspace IA — timeline +
  scrubber + disasm shown *together* — does not exist.
- **F4 — the view surface nests three exclusive tab levels deep.** A recording is
  a tab, its 12 views are a second exclusive tab bar, and the Observer deck is a
  third — so only one view is ever visible and wayfinding collapses. Flattening
  this is the same edit as the pane conversion: a docked pane replaces an
  exclusive tab item.
- **F9 — no "where am I / show me a sibling" wayfinding.** Sibling views cannot
  coexist under the single-window nested-tab model, so an analyst cannot hold the
  timeline and the scrubber and the disassembly in view at once. Real panes are
  the substrate the Wave-2 wayfinding chrome (doc 21) needs.
- **This is the keystone.** It unblocks the workspace/preset work in doc 20
  (T2.3/T2.7), the spine navigation in doc 21 (T3.3), and the selection surface in
  doc 22 (T1.2): each of those assumes visible, rearrangeable panes exist.

## What already exists (verified 2026-07-27)

- **The layout manager, fully built and unit-testable, docking phantom windows.**
  `desktop/src/ui/layout.cpp` declares the five pane names
  (`kPaneHome/Recording/Scrubber/Inspector/Timeline`, lines 10–14) and
  `layout_build` (lines 32–73) splits the dockspace into left/right/bottom/center
  and `DockBuilderDockWindow`s those five names per preset (lines 56–68).
  `layout.h:46-51` states the contract in a comment: the names are *"the SAME
  strings the shell passes to `ImGui::Begin`, so the two cannot drift"* — **that
  promise is currently unkept; the shell passes none of them.**
- **The dockspace + the whole View menu, live and inert.** `shell.cpp:766-778`
  enables the dockspace (`DockSpaceOverViewport`, line 770) and builds the default
  `ReplayInspect` layout on first run; `shell.cpp:792` `Begin("asmtest", …)` is
  the single content window; `shell.cpp:793-807` is the `View` menu with
  `Reset layout` (line 795, `layout_build(…, ReplayInspect)`) and the three
  presets (lines 799–803, `layout_build(…, p)`). The only other `Begin()` is
  `"Keyboard bindings"` at `shell.cpp:912`. **All of this menu works the instant
  the panes are real — no change needed there.**
- **The 3-deep exclusive tab nesting, grounded.**
  1. `shell.cpp:809` `BeginTabBar("main", …)` — Home / door tabs / one tab per
     open recording.
  2. `shell.cpp:385` `BeginTabBar("views")` inside `draw_recording_tab` — the 12
     view tab items (Summary, Canvas, Timeline, Slice, Diff, Observer, Loom,
     Scrubber, ABI x-ray, 3D overview, Backends, This host), `shell.cpp:385-535`.
  3. `desktop/src/views/observer_draw.cpp:568` `BeginTabBar("observer")` inside
     the Observer tab — Syscalls/Watch/Topology/Hot edges/Tree/Invocations/
     **Disassembly** (line 596). The Loom tab has its own third-level
     `BeginTabBar("loom-detail")` at `fabric_imgui.cpp:247`.
- **The Observer deck already gates its inner tabs on data present.**
  `observer_draw.cpp:562-566` shows a placard and returns when the recording
  carries none of the Observer kinds, and each `BeginTabItem` is guarded on its
  rows being non-empty (lines 570–598). This is F4's "the fix already exists
  in-tree" — the data-driven *inner* gating to mirror at the pane level (the
  data-driven *outer/pane* gating itself is doc 20's T2.3, not this brief).
- **The view-switch intent plumbing is already pane-shaped.** `handle_keymap`
  records `want_view` (`shell.h:81`) and the view tab bar honours it with
  `ImGuiTabItemFlags_SetSelected` (`shell.cpp:397-399`); `want_open_tab`/
  `want_loom` (`shell.h:154-155`) select the outer recording tab. These "select
  this view next frame" signals port directly to "focus this pane"
  (`ImGui::SetWindowFocus(name)`), so the keymap keeps working across the
  conversion.
- **Docking + persistence are ON in the real app.** `main.cpp:88` sets
  `ImGuiConfigFlags_DockingEnable`; `main.cpp:89-91` points `IniFilename` at
  `build/desktop-imgui.ini` (dock layout persisted cross-session). The headless
  null backend (`test_shell.cpp:70`, `io.IniFilename = nullptr`) leaves docking
  **off** and writes no file — so the `docking` branch in `draw_shell`
  (`shell.cpp:769-778`, `790-807`) is entirely un-exercised by the current tests.
  Turning it on in the new tests is step one.
- **`theme.h` fidelity colours + every view's own placards** already exist
  (`desktop/src/ui/theme.h`, and e.g. the scrubber/ABI-x-ray/3D placards at
  `shell.cpp:428-431,492-494,296-301`). Pane conversion **moves** these into the
  panes; it removes none of them (D7, F5).

## Tasks

> **Landed 2026-07-27 — all three tasks, green (`make desktop-test` +
> `make desktop-ui-test`).** The docked shell (`draw_docked_shell` in
> `ui/shell.cpp`) `Begin()`s each region as a real pane; the inner exclusive
> `BeginTabBar("views")` is gone; the 3-deep nesting is flattened to <=2 (the Loom
> and the Observer deck each moved to their OWN pane so their inner bar sits one
> level below a pane, not two). The non-docked path is preserved verbatim in
> `draw_windowed_shell`, gated on the `docking` bool, so a run with no dockspace
> does not regress. **Two deviations from this brief, code winning (per the
> preamble):** (1) `draw_scene_hud` already `Begin()`s its own `"3D overview"`
> window, so the 3D overview stays a **tab in `kPaneRecording`** rather than a
> pane named "3D overview" (a name collision) — it has no inner tab bar, so the
> nesting rule is satisfied either way; (2) the Loom and the Observer deck are
> their OWN docked panes (`kPaneLoom`, `kPaneObserver`, added to `layout.cpp`)
> because a view carrying an inner tab bar cannot be nested inside another pane's
> tab strip without rebuilding the very 3-deep stack this brief removes — the five
> original region names are unchanged and still each `Begin()`'d. `layout.cpp`
> split the bottom region in two (`bottom`/`bottom2`) so the default preset holds
> the timeline AND the scrubber at once (T3.1), and `ShellState::dockspace_id` is
> published each docked frame so the View menu — and the tests — rebuild presets
> against it. Tests: `test_shell.cpp` gained a docked case (each `kPane*` exists +
> `WasActive`; timeline+scrubber+Observer simultaneously active in distinct nodes;
> the producer-absent scrubber/3D placards survive the move; a preset switch moves
> real panes and Reset restores them), and `test_ui.cpp` a tear-out→Reset
> round-trip on the interaction lane.

### T1 — Convert each view into a real pane `Begin()`'d under the `kPane*` names  (L, depends on: 13 F1)

**Goal.** Replace the inner `BeginTabBar("views")` (`shell.cpp:385`) with real
docked panes: each of the five region names `layout.cpp` docks is `Begin()`'d by
the shell, hosting the active recording's views, so the dockspace, presets,
tear-out and Reset finally act on windows that exist. The five `kPane*` names are
**regions/roles**, not one-per-view; the 12 views are distributed across them.

**Steps.**
1. **Decide the pane → view mapping** (put it in a comment next to the `Begin`
   calls so it does not drift, mirroring `layout.h:46`'s contract). The five
   regions `layout_build` docks, and what each hosts:
   - **`kPaneHome`** (left rail) — the doors (`draw_doors`, `shell.cpp:182-214`)
     and a compact list of open recordings that sets `s.active_tab` on click. This
     is the persistent home the review wants (F9 notes "Home is itself a closeable
     tab"); the *full* recentre/spine polish is doc 21, so keep this minimal — a
     selectable list, not a redesign.
   - **`kPaneRecording`** (center) — the primary reading views for the active
     recording: Summary, Canvas, Slice, Diff, Loom, 3D overview. These may share
     the pane as a **single flat** `BeginTabBar` (one level, tear-nothing) *or*
     each be its own `Begin()` docked into `L.center` (ImGui renders co-docked
     windows as tear-able dock-tabs). Prefer the latter for the heavy views
     (Loom, 3D) so they can be torn out; a flat inner tab bar for the light ones
     is acceptable — the ban is on *exclusive nesting*, not on one flat tab strip.
   - **`kPaneScrubber`** (bottom) — the register scrubber (`draw_scrubber`,
     today `shell.cpp:477-483`).
   - **`kPaneTimeline`** (bottom) — the operand timeline (`draw_timeline`,
     `shell.cpp:405-420`).
   - **`kPaneInspector`** (right) — the inspector role: ABI x-ray, Backends
     (`draw_completeness`), This host (`draw_capability_panel`). Diff lives with
     the reading views in `kPaneRecording` but its A/B chrome reads here.
   Every view keeps its exact current body (the `draw_*` calls and their fidelity
   placards) — only its *container* changes from a `BeginTabItem` to a pane.
2. **Emit the panes only when docking is on**; when it is off (the null backend's
   default) keep drawing the old single-window tab layout so nothing regresses for
   a run with no dockspace. Gate on the same `docking` bool already computed at
   `shell.cpp:766-767`. (The new tests flip docking *on* — step T3.)
3. **Panes render the active recording.** A pane's body is a pure function of
   `shell_a(s)` / `s.active_tab` (as the views already are). With no active
   recording, a pane shows its own "open a recording" placard rather than
   vanishing — F4's data-driven *hiding* of panes is doc 20 (T2.3), so until then
   nothing is silently dropped (D7): an empty view states why, in place.
4. **Port the view-switch intents to focus.** `want_view` → `SetWindowFocus` on
   the target view's pane/dock-tab; `want_loom` → focus the Loom pane;
   `want_open_tab` still selects the active recording in the `kPaneHome` list.
   Consume them once per frame exactly as `shell.cpp:890-892` does now.
5. **Preserve `[`/`]` view-local semantics** (they stay the dependence-generation
   walk in scrubber/slice/abixray, unchanged — doc 17 T1).

**Tests.** `desktop/test/test_shell.cpp` (null backend): a new case that sets
`io.ConfigFlags |= ImGuiConfigFlags_DockingEnable` on its own context (keep
`IniFilename = nullptr`), opens a fixture, drives `draw_shell` for a few frames,
then asserts via `ImGui::FindWindowByName(kPaneRecording)` (and each other
`kPane*`) that the window **exists and is `WasActive`** — i.e. actually
`Begin()`'d, the exact thing that is false today. Assert the pane bodies still
show their fidelity placards for a producer-absent view (feed `min-trace.asmtrace`
and assert the scrubber/3D placard strings survive the move). Model/window state,
never pixels (D4/D7).

**Docs.** CHANGELOG `Changed`: "desktop views are now real dockable panes — the
docking layout manager, presets and Reset act on visible windows". `desktop/README.md`:
document the pane→region mapping and that panes host the active recording.

**Done when.** Each `kPane*` name `layout.cpp` docks is `Begin()`'d by the shell
under the active recording; `FindWindowByName` finds every one active in a docked
run; the inner exclusive `BeginTabBar("views")` is gone (T2); the keymap
view-switch still lands via focus; nothing regresses in a non-docked run.

### T2 — Flatten the 3-deep exclusive tab nesting  (M, depends on: T1)

**Goal.** Collapse main→views→observer from three exclusive tab levels to at most
two, in the same pass as T1 (this is the same edit viewed as a nesting cut).

**Steps.**
1. **Level 2 is deleted by T1** — replacing `BeginTabBar("views")` with panes
   removes the middle exclusive level outright.
2. **The Observer deck becomes a single `kPane`-hosted pane** whose *one*
   remaining `BeginTabBar("observer")` (`observer_draw.cpp:568`) is now the only
   sub-level — nesting is ≤2 deep (recording context → observer's data-gated
   inner tabs). Keep its existing data-driven gating (`observer_draw.cpp:562-598`)
   as-is; that gating is the F4-endorsed pattern, not the problem. The Observer
   pane may dock into `kPaneTimeline`/bottom or `kPaneInspector` — pick one in the
   preset; **the point is disasm (`observer_draw.cpp:596`) can now be visible at
   the same time as the timeline and scrubber panes**, not buried two exclusive
   tabs away.
3. **The Loom's `BeginTabBar("loom-detail")`** (`fabric_imgui.cpp:247`) is
   internal to the Loom pane and stays a single flat level — after T1 it is
   pane → loom-detail, ≤2 deep. No change needed beyond it living in a pane.
4. **The recording selector stops being level-1 exclusive tabs.** The per-recording
   `BeginTabBar("main")` (`shell.cpp:809`) that mixed Home/doors/recordings is
   replaced by the `kPaneHome` recording list (T1 step 1) driving `s.active_tab`;
   doors open as panes or stay as the small tab set inside `kPaneHome` (the
   door-chooser redesign, F13, is doc 21 — do not do it here).

**Tests.** In the same `test_shell.cpp` docked case: assert **timeline, scrubber
and disasm are simultaneously `WasActive`** in one frame (dock them into distinct
nodes, drive a frame, `FindWindowByName` all three and assert each active) — the
concrete refutation of "only one view visible" (F2/F9). Contrast is implicit: the
old exclusive tab bar can never make three siblings active at once.

**Docs.** Fold into T1's CHANGELOG `Changed` line (mention "flattened the 3-deep
tab nesting"). Update the F4 substrate note is not required (internal doc).

**Done when.** No exclusive `BeginTabBar` is nested more than one level below a
pane; the Observer/Loom inner tab bars are the only sub-levels and stay
data-gated; timeline + scrubber + disasm can be shown together.

### T3 — Presets, Reset and tear-out become real; coordinate the `.ini` reset  (M, depends on: T1, T2; land with 18 T2.2)

**Goal.** Prove the View menu (`shell.cpp:793-807`) now rearranges *visible* panes,
that tear-out and Reset round-trip, and that a persisted dock `.ini` cannot strand
the user now that the panes it targets are real.

**Steps.**
1. **Presets rearrange real panes.** No new preset code is needed —
   `layout_build` already docks the five names per `LayoutPreset`
   (`layout.cpp:56-70`) and the menu already calls it. Verify the
   `ReplayInspect` default actually places Scrubber + Inspector, and that the
   bottom region can hold Timeline **and** Scrubber for the coexistence the review
   names — if the current single `L.bottom` split cannot show both at once, split
   the bottom (a small `layout.cpp` change) so the default preset satisfies "shown
   together". Keep preset *design* minimal; the workspace/named-perspective work
   is doc 20 (T2.3).
2. **Reset is reachable and real.** The menu's `Reset layout`
   (`shell.cpp:795-797`) now rebuilds visible panes. **Coordinate with doc 18
   T2.2**: docking persistence is on (`main.cpp:89-91`) and doc 13:82-84 records
   that the persisted `.ini` makes an upstream ImGui table-settings crash "newly
   reachable" — now that panes are real, a corrupt/stale `.ini` docks real windows
   and can also strand the user (F2). 18 T2.2 adds the auto-fallback (detect a
   broken/empty load, rebuild the default). This brief lands the panes it protects;
   do not ship the panes without that Reset in the same wave.
3. **Tear-out + Reset round-trip** — verified by the doc-17 imgui_test_engine
   lane (below), the only tier that can drive a real drag.

**Tests.**
- **Null backend (`test_shell.cpp`), preset rearrangement:** build `ReplayInspect`,
  read each pane's `ImGuiWindow::DockId` (via `FindWindowByName`), switch to
  `LiveObserver`, drive a frame, assert the dock assignment **changed** (e.g. the
  Timeline pane is now in `L.bottom` and the Scrubber's placement differs) — the
  preset moved real panes. Assert `Reset layout` restores the default assignment.
- **Interaction lane (doc 17, `desktop/test/test_ui.cpp`, `make desktop-ui-test`):**
  an imgui_test_engine test that **tears a pane out** (`ctx->UndockWindow` /
  drag its dock-tab to float), asserts it is undocked (`window->DockId == 0`),
  then invokes Reset and asserts it is re-docked into its region. This is the one
  assertion that needs a real backend drag, so it belongs in the doc-17 lane, not
  the null smoke test — model/window state, not pixels.

**Docs.** CHANGELOG `Changed`: "View-menu presets and Reset now rearrange visible
panes; panes can be torn out and restored". `desktop/README.md`: note that Reset
recovers from a bad persisted `.ini` (cross-ref doc 18 T2.2).

**Done when.** A preset switch and Reset visibly move real panes (asserted by
dock-id change in the null test); tear-out→Reset round-trips in the doc-17 lane;
Reset recovers a broken `.ini` (with 18 T2.2); `make desktop-test` and
`make desktop-ui-test` are green in `docker-desktop`.

## Task order & parallelism

One developer, one keystone, in order: **T1** (the conversion) and **T2** (the
flatten) are literally the same edit and land together; **T3** (menu now works +
tests + `.ini` safety) follows immediately. The whole thing is Size **L** — a
single structural surface (`shell.cpp`'s `draw_recording_tab` + `draw_shell`, plus
a small `layout.cpp` bottom-split if step T3.1 needs it). **Land doc 18 T2.2 in
the same wave** so real panes can never be stranded by a bad `.ini`. Nothing else
in Wave 1 starts until this lands: docs 20/21/22 assume visible panes.

## Constraints & gates

- **Fidelity (D7/F5) is restructured, never removed.** Every view's placards,
  banners and provenance chrome (`theme.h`, the per-view "producer absent"
  placards at `shell.cpp:296-301,428-431,492-494`, the Observer gating at
  `observer_draw.cpp:562-598`) move *into* the panes intact. A view with no
  backing data shows its placard **in its pane** — this brief does **not** hide
  empty panes (that data-driven pane visibility is doc 20 T2.3); until then,
  nothing is silently dropped.
- **The null test tier stays file-free and deterministic** (doc 13:394). The new
  docked-mode tests set `ConfigFlags |= DockingEnable` on their own context but
  keep `IniFilename = nullptr` (`test_shell.cpp:70`) — they drive DockBuilder
  programmatically, never load/write an `.ini`.
- **The persisted `.ini` crash is newly reachable** (doc 13:82-84): real panes +
  `main.cpp:91` persistence mean a stale/corrupt dock `.ini` targets live windows.
  The `v1.91.9b-docking` pin (doc 13 F1) is what makes loading table settings
  safe; **doc 18 T2.2's real Reset auto-fallback is the required companion** — do
  not ship the panes without it.
- **`layout.cpp` stays the only `imgui_internal.h` consumer in the shell**
  (`layout.h:4-9`, the doc-12 repin gate). The pane conversion needs only public
  `ImGui::Begin`/`SetWindowFocus`; if a test reads `ImGuiWindow::DockId` it
  includes `imgui_internal.h` in the *test* only, not in `shell.cpp`.
- **Docking-off path must not regress.** A run with no dockspace (the null
  backend's default, and any future no-docking build) keeps the current
  single-window tab layout — the pane emission is gated on the `docking` bool.

## Out of scope

- **Data-driven pane/tab *hiding*** (show a pane only when its recording carries
  the events) — that is F4's outer gating, doc 20 **T2.3**. Here an empty view
  shows its placard in-pane.
- **Named/saved workspaces, task-shaped perspectives, MRU/session restore** — doc
  20 (T2.4/T2.5 + T2.7 perspectives) and F10; this brief ships only the built-in `LayoutPreset`s
  that already exist.
- **The persistent global "where am I" breadcrumb / nav spine and the Home
  door-chooser redesign** — doc 21 (F9/F13). This brief only stops Home from being
  an exclusive level-1 tab.
- **Back/forward nav history** (F11) — doc 21/22.
- **Font/HiDPI/text-scale settings** (F6) — a separate track.
