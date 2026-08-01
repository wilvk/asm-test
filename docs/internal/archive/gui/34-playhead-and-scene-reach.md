# Wave 4 follow-on: one execution-step playhead, reachable 3D, play/pause — implementation

> Grounded in the 2026-07-28 walkthrough of the *"select a process → watch the
> 3D graph change over time"* flow. That flow works, but it is three disconnected
> mechanisms wearing a trenchcoat: picking a process
> ([`draw_processes_pane`](../../../../desktop/src/ui/inspect_door.cpp), `:705`)
> starts a live capture; the capture becomes a Recording tab; the Recording has a
> **"3D overview"** tab you must *find*; and that tab moves through time via its
> **own** playhead, unlinked from every other time-aware pane. This brief closes
> the three seams that make the flow feel like one action — **without faking a
> single global clock**, because there genuinely is not one (see the fidelity note
> below).
>
> **This is a desktop brief.** No producer, no engine, no schema change. Every
> task is a pure `ShellState`/`HudState` move, so the null backend drives it
> headlessly (D4) exactly as [22](22-selection-and-search.md)'s selection model is
> driven.
>
> **Extended 2026-07-31** by [43-faithful-city-roadmap.md](../../gui/43-faithful-city-roadmap.md)/[44](44-faithful-city-phase-a-mvp-terrain-reskin.md):
> the two-clock anti-fusion rule this brief establishes (T1/T3/T4 here) is the
> precedent 44's `SceneFrame.sun` + a second `Transport` for the followed-citizen
> vehicle explicitly follows — re-read T3's fidelity note before extending either
> clock further.

## Why this work exists

Three concrete friction points, each verified in the code:

1. **The register Scrubber is off the shared brush.** [22](22-selection-and-search.md)
   made `selection.step` the ONE brushed entity that the timeline, slice explorer
   and Loom all read ([`selection.h`](../../../../desktop/src/ui/selection.h);
   projected at [`shell.cpp`](../../../../desktop/src/ui/shell.cpp) `:812`). The
   register time-travel Scrubber is the one *execution-step* view left out: it
   drives its own `scrubber_playhead[i]`
   ([`body_scrubber`](../../../../desktop/src/ui/shell.cpp) `:916-930`) that neither
   reads nor writes the shared selection. So stepping with `j`/`k`/arrows moves
   the timeline/slice/Loom but not the register file, and scrubbing registers
   brushes nothing.

2. **The 3D overview is unreachable by keyboard and by handoff.** It is a
   conditionally-present tab (`ViewId::Scene3D`, present iff the recording carries
   `codeimage` regions — [`view_presence.cpp`](../../../../desktop/src/ui/view_presence.cpp)
   `:121`) hosted inside the centre Recording pane
   ([`shell.cpp`](../../../../desktop/src/ui/shell.cpp) `:1991-1995`). The `1/2/3/4`
   keymap only routes the four `dt_view` reading tabs
   ([`shell.cpp`](../../../../desktop/src/ui/shell.cpp) `:1243-1246`); the scene has
   **no `dt_view` spelling** (`nav.h:34`) so `want_view` cannot select it, and a
   full-detail attach only opens the Live-capture pane
   (`want_open_capture`, [`inspect_door.cpp`](../../../../desktop/src/ui/inspect_door.cpp)
   `:144`) — it never points the analyst at the 3D view their capture just filled.

3. **Nothing plays.** Both playheads move only by manual drag or, live, by the
   capture growing ([`shell.cpp`](../../../../desktop/src/ui/shell.cpp) `:229-243`).
   For a replay file, *"watch it change over time"* means dragging a slider by
   hand — there is no play/pause anywhere.

## The fidelity note that shapes the design (D7)

The obvious ask is "unify the playheads into one global clock." **We must not**,
because the two playheads measure **different axes**, and equating them
numerically would fabricate position:

- `selection.step` / `scrubber_playhead` are the **execution-step** axis — an
  absolute instruction step. The Scrubber's `StepIndex` steps are absolute
  (`RegFile::step`, [`stepindex.h`](../../../../desktop/src/analysis/stepindex.h)
  `:43`, space `[0, total_steps())`), and the memory ring aligns with the
  dataflow step (`regstate[i] ↔ df_step[i]`, 26). Both are the same basis.
- `HudState::t` is the **terrain time** axis — *"ordered trace steps"*
  ([`terrain.h`](../../../../desktop/src/space/terrain.h) `:55`), i.e. `mem`/`trace`
  residency steps, a different and often differently-scaled sequence.

So the faithful unification is: **put the Scrubber on the execution-step axis it
already belongs to** (T1), and give the terrain-time playhead its **own** labelled
transport (T3) rather than chaining it to a step index it does not share. Each
playhead is named by its axis (T4). This is a *better* answer than a fake global
clock — it is the app's whole ethos (faithful provenance) applied to time.

## What already exists (verified 2026-07-28)

- **The shared brush + its one writer.** `Selection::set` bumps the epoch so panes
  re-read ([`selection.h`](../../../../desktop/src/ui/selection.h) `:43`); the keymap
  step keys funnel through it ([`shell.cpp`](../../../../desktop/src/ui/shell.cpp)
  `:1349-1361`, epoch bump `:1458`); the recording-scoping gate
  (`selection.rec == a->id`, `:812`) is the D7 fix that keeps a brush from lighting
  a coincident index in another recording — the same gate the Scrubber must honour.
- **The Scrubber seam.** `draw_scrubber(idx, playhead) -> moved`
  ([`views/scrubber_draw.cpp`](../../../../desktop/src/views/scrubber_draw.cpp) `:10`),
  called from `body_scrubber` with the per-recording `scrubber_playhead[i]`. The
  playhead is the caller's; the draw returns the moved value.
- **The 3D HUD playhead.** `dt_timepos_scrub("playhead (step)", &t, tmax)`
  ([`scene3d/hud.cpp`](../../../../desktop/src/scene3d/hud.cpp) `:86`) reports
  `HudState::playhead_moved`; `draw_scene_overview` re-slices on a move
  ([`shell.cpp`](../../../../desktop/src/ui/shell.cpp) `:672-682`). `HudState`
  (`hud.h`) already carries `req_reset_view`/`req_top_down` intents the caller
  applies — the transport's play/pause is one more of the same shape.
- **The intent → tab-select idiom.** `want_view` / `want_loom` set
  `ImGuiTabItemFlags_SetSelected` and are consumed the same frame
  ([`shell.cpp`](../../../../desktop/src/ui/shell.cpp) `:1149-1152`, `:1616`;
  docked `:2017`). A new `want_view_id` (keyed on `ViewId`, which *does* spell the
  scene) mirrors this exactly, for the tabs that have no `dt_view`.
- **The handoff idiom.** The Inspect door reports cross-pane intents on
  `InspectState` (`want_open_capture` `:144`) that `draw_shell` consumes
  (`:1942`); the live capture is promoted to a workspace tab by
  `shell_sync_live_tab` (`:169`, `live_tab`). A `want_scene` intent + the
  `live_tab` index is all the "View in 3D" handoff needs.
- **The bindings-help source of truth.** `dt_nav_bindings()`
  ([`nav.cpp`](../../../../desktop/src/nav.cpp) `:345`) is the ONE list the overlay
  renders (18 T1); every new key MUST land here with `wired:true` or the overlay
  drifts.

## Tasks

### T1 — the register Scrubber joins the shared execution-step brush  (M)

Two-way link `scrubber_playhead[i]` ↔ `selection.step`, gated on the active
recording. In `body_scrubber`
([`shell.cpp`](../../../../desktop/src/ui/shell.cpp) `:916`): **seed** the playhead
from the shared selection when `selection.rec == a->id` and it has a step
(clamped to `total_steps()-1`), draw, and on a change **brush** it back through
the one writer `s.selection.set(a->id, moved, std::nullopt)`. The clamp + the
Scrubber's existing torn-edge handling (`at_step` returns null for a dropped
prefix, [`stepindex.h`](../../../../desktop/src/analysis/stepindex.h) `:75-78`) keep
it faithful: a brushed step in the evicted region shows the torn edge, never a
neighbour's file. Extract the projection as a pure header-only seam
(`ui/transport.h` `playhead_project`) so the test drives it directly.

**Done when.** stepping with `j`/`k`/arrows moves the register file, scrubbing the
register file brushes the timeline/slice/Loom to the same step, and a step brushed
in recording A does not move B's Scrubber after a tab switch (the `:812` gate,
asserted).

### T2 — reach the 3D overview: a `5` route + the "View in 3D" handoff  (M)

Give the scene a keypress and a handoff, both through the existing tab-select
idiom, via a new `std::optional<ViewId> want_view_id` on `ShellState`:

- **Keymap.** `5` sets `want_view_id = ViewId::Scene3D` (and clears `want_view`);
  `1/2/3/4` clear `want_view_id` symmetrically
  ([`shell.cpp`](../../../../desktop/src/ui/shell.cpp) `:1243-1246`). Both tab bars
  (non-docked `:1145-1159`, docked `:2010-2027`) honour it with `SetSelected` when
  present, and route it to the "unavailable views" affordance (with the machine
  reason) when absent — never a silent no-op. Consume it beside `want_view`
  (`:1616`).
- **Handoff.** In the Live-capture pane
  ([`draw_capture_pane`](../../../../desktop/src/ui/inspect_door.cpp) `:850` /
  `draw_live_views` `:493`), when a capture exists, show a **"View in 3D overview
  →"** button that sets `InspectState::want_scene`. `draw_shell` consumes it after
  `shell_sync_live_tab` (`:2567`): if `live_tab >= 0`, set
  `active_tab = live_tab` + `want_view_id = Scene3D`; else route the reason to the
  status bar.

**Done when.** pressing `5` selects the 3D tab (or lands on "unavailable views"
explaining why it is absent); the Live-capture button jumps straight from the
picked process's growing capture to its 3D overview.

### T3 — a play/pause transport on each playhead  (M)

One pure mechanism, two independently-labelled transports. Add header-only
`ui/transport.h`: a `Transport { bool playing; float accum; float steps_per_sec; }`
and `transport_tick(Transport&, cur, max, dt) -> next` that accumulates `dt` and
advances whole steps, stopping (and clearing `playing`) at `max`. No `Date`/time
in the seam — the caller passes `ImGui::GetIO().DeltaTime`, the test passes a
synthetic `dt`.

- **Execution-step transport** (`ShellState::play`): a **Play/Pause** button in the
  Scrubber pane; advanced once per frame in `draw_shell` (after
  `shell_sync_live_tab`, `:2567`) over `a->df.nsteps-1`, brushing `selection.step`
  through the one writer so the timeline/slice/Loom/Scrubber all animate together.
  Advancing centrally (not in `body_scrubber`) keeps playback running when the
  Scrubber pane is not the visible tab.
- **Terrain-time transport** (`SceneView::play`): a **Play/Pause** button in the
  scene HUD (`draw_scene_hud`, beside the reset/top-down buttons) reporting a
  `HudState` intent; advanced in `draw_scene_overview` over `terr.nsteps`, moving
  `hud.t` (which already re-slices, `:672`). Animates the trajectory forming across
  the terrain — the literal *"watch the 3D change over time"*, on its own axis.

**Done when.** Play animates each playhead over its own axis at a steady rate,
stops faithfully at the end, and pausing holds position; `transport_tick` is a pure,
tested function.

### T4 — fidelity: name each axis, wire the bindings help  (S, depends on: T1–T3)

- **Axis labels.** The Scrubber pane and the execution-step transport read
  *"step (execution)"*; the 3D HUD playhead + its transport read *"step (trace
  time)"* — so the two playheads are visibly different axes, not a broken global
  clock. A one-line note in the scene primer
  ([`shell.cpp`](../../../../desktop/src/ui/shell.cpp) `:616-623`) states the 3D
  playhead walks trace-residency time, distinct from the execution step the flat
  views brush.
- **Bindings help.** Add rows to `dt_nav_bindings()`
  ([`nav.cpp`](../../../../desktop/src/nav.cpp) `:350`), `wired:true`: extend the
  `1/2/3/4` row to `1/2/3/4/5` (`… / 3D overview`) and add a play/pause row for the
  Scrubber and HUD buttons. (Play/pause is a button, not a global key in v1 — see
  Out of scope for why Space is deferred.)

**Done when.** the two playheads are labelled by axis, the primer states the
distinction, and the help overlay lists `5` and the transports with no dead rows.

### T5 — tests  (M, depends on: T1–T3)

Null backend, model state not pixels — extend the existing lanes, add no new TU:

1. **`transport.h` seam** (a small block in `test_selection.cpp` or `test_shell.cpp`):
   `transport_tick` advances the right number of whole steps for a given
   `dt × steps_per_sec`, clamps and stops at `max`, and `playhead_project` returns
   the clamped shared step only when the selection belongs to the recording (the
   `:812` gate) — nullopt otherwise.
2. **Scrubber ↔ selection** (extend `test_selection.cpp`): a shared step brushed
   through `Selection::set` seeds the Scrubber's projected playhead; the reverse
   brush lights the timeline/slice model at the same step; and A's brush leaves
   B's projection empty after a tab switch (the recording-scoping regression).
3. **`want_view_id` + handoff** (extend `test_shell.cpp`): pressing the `5` intent
   selects `ViewId::Scene3D` when present and routes to the affordance when absent;
   `InspectState::want_scene` with a synthetic live tab sets `active_tab` +
   `want_view_id`. Reuses the doc-25 live-tab fixture block.

**Done when.** all three pass under `make docker-desktop` (null-backend
`desktop-test`); reverting the `:812` gate or the clamp fails a named check.

## Task order & parallelism

`T3`'s `ui/transport.h` seam underpins `T1` (`playhead_project`) and its own
transports, so land the header first. Then `T1` (Scrubber-on-axis) and `T2`
(reach-3D) are independent and parallelise; `T3`'s two transports parallelise once
the header exists; `T4` is labels over all three; `T5` verifies.

Order: `transport.h` → (`T1` ∥ `T2` ∥ `T3`) → `T4` → `T5`.

## Constraints & gates

- **No fake global clock (D7).** The two playheads are different axes; each is
  named and driven on its own. The only *link* is faithful: the Scrubber and the flat
  views share the execution-step basis, and the recording-scoping gate + the
  torn-edge handling keep a projected step from ever fabricating a row.
- **One selection writer (D4).** Every Scrubber/transport brush funnels through
  `Selection::set`, so a scrub, a keypress and a play-tick are indistinguishable at
  the model — exactly as [22](22-selection-and-search.md) requires.
- **Reuse the intent idiom, don't invent a mechanism.** `want_view_id` mirrors
  `want_view`; `want_scene` mirrors `want_open_capture`; the transport intents
  mirror `req_reset_view`. No new frame-timing machinery beyond the pure
  `transport_tick`.
- **Backend-free + zero-cost when idle.** Header-only seam, no engine/GL touch, no
  makefile change; a paused transport and an unbrushed Scrubber cost one untaken
  branch per frame.
- **Help cannot drift.** Every new key is a `wired:true` row in `dt_nav_bindings()`
  or it does not ship (18 T1).

## Out of scope

- **A Space play/pause key.** Space activates a focused ImGui item, and the 3D
  viewport is an `InvisibleButton` (the pick target,
  [`shell.cpp`](../../../../desktop/src/ui/shell.cpp) `:751`) — a global Space binding
  would fire an unwanted pick when the viewport holds focus. Play/pause is a button
  in v1; a guarded Space binding is a follow-on once the viewport-activation
  interaction is resolved.
- **A live "follow the tail" camera.** The live re-weave already keeps the whole
  trace and holds the camera (`:229-243`); an explicit "stick to newest step"
  toggle is a small follow-on, not this brief.
- **Cross-axis linking (a 3D marker at the brushed execution step).** Faithful only
  where the terrain-time ↔ execution-step correspondence is exact; establishing
  that mapping is its own analysis rung, deliberately deferred (see the fidelity
  note).
- **An explicit "Attach & then 3D" one-shot from the Processes row.** The row's
  double-click stays "attach at full detail" (`:808`); the 3D handoff is the
  Live-capture button, so the two gestures stay separable.
