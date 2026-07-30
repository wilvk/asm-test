# Wave 1: the workspace container — data-driven tabs, task-language entry rail, persistence + recents, perspectives, Settings/DPI — implementation

> **Sources.** Actioned from the UX restructure plan
> ([../plans/desktop-gui-ux-restructure-plan.md](../plans/desktop-gui-ux-restructure-plan.md))
> rows **T2.3, T2.4, T2.5, T2.7, T2.8** and the review findings **F4, F13, F10,
> F2, F16, F6** ([../plans/desktop-gui-ux-review.md](../plans/desktop-gui-ux-review.md)).
> Written 2026-07-27 against HEAD `243f092`. This doc wins over the review/plan on
> disagreement; the CODE wins over this doc — re-verify file:line before editing.
> Read [\_conventions.md](../implementations/_conventions.md) first; D1–D11 live
> in this directory's [README](README.md). **Prerequisites:
> [19-dockable-panes-keystone.md](19-dockable-panes-keystone.md)** (real dockable
> panes — T1 gates the pane set, T4 names their arrangements; without it the panes
> are still orphaned per F2) and **[13-foundation-moves.md](13-foundation-moves.md)
> F3** (the `load_fonts` atlas T5 rebuilds).

## Why this work exists

The keystone ([19](19-dockable-panes-keystone.md)) makes the panes real; this
brief turns that pane machinery into an actual *Workspace* — the container the
review found missing behind a single-window nested-tab model.

- **F4** — every recording shows all 12 view tabs at once regardless of backing
  data (`shell.cpp:384-540`), so a bare-log recording presents Loom / ABI-x-ray /
  3D / Scrubber tabs that draw only a "producer absent" placard: the "blank
  multi-panel IDE" the plan promised to avoid. The fix already ships in-tree — the
  Observer deck gates its inner tabs on data present (`observer_draw.cpp:562-600`).
- **F13** — first run is a bespoke "choose a door" metaphor with four peer buttons
  (`shell.cpp:182-214`), two of them (Author, Inspect) meaningless before first
  value; doors open as closeable tabs, Home is itself a tab, "door/tab/mode/preset"
  name overlapping concepts. (The `draw_doors` comment says "three doors"; the
  build renders four buttons — a live doc-vs-build drift.)
- **F10** — nothing is remembered between launches: no MRU list, no
  reopen-last-workspace, no drag-drop (`workspace.cpp` has only `open`/`close`;
  `main.cpp:91` persists the dock `.ini` alone). Every start forces recall-and-retype
  of a path.
- **F2 / F16** — with real panes the analyst wants named *perspectives* and named
  saved-filter/query presets (the VS Code / Blender / JetBrains model); today the
  dockspace acts on phantom windows and no task-shaped arrangement can be saved.
- **F6** — the app hardcodes a 15px font (`fonts.cpp:30,41,49`) and a 1280×720
  window (`main.cpp:72`) with no HiDPI awareness, no user text-scale, no settings
  surface. This is the *one* accessibility lever fully inside ImGui's control
  (ImGui exposes no OS screen-reader tree — a real recorded constraint, review F6 /
  [11](11-imgui-addons.md):461), and it is unspent.

## What already exists (verified 2026-07-27)

- **The outer tab strip shows everything.** `draw_shell` opens one `BeginTabBar("main")`
  (`shell.cpp:809`) holding: a `Home` tab (`shell.cpp:810-814`), the Author / Inspect /
  Learn door tabs (`shell.cpp:820-865`), one tab per open recording
  (`shell.cpp:871-886`), and a trailing **legacy `door_tabs` loop**
  (`shell.cpp:897-904`) that renders each string in `ShellState::door_tabs`
  (`shell.h:60`) as an empty "view lands in a later doc" placeholder. `door_tabs`
  is populated **nowhere in the app** — only `test_shell.cpp:85` pushes one to
  exercise the path. It is dead surface.
- **The inner view tab bar is ungated.** `draw_recording_tab` opens
  `BeginTabBar("views")` (`shell.cpp:385`) and unconditionally emits Summary,
  Canvas, Timeline, Slice, Diff, Observer, Loom, Scrubber, ABI x-ray, 3D overview,
  Backends, This host — every one guarded only on `a != nullptr`
  (`shell.cpp:392-533`), never on whether the recording carries the events that
  view needs. Loom/3D/Scrubber/ABI-x-ray each fall through to their own placard.
- **The data-driven pattern to copy is `observer_draw.cpp`.** `observer_has_any`
  (`observer_draw.cpp:76-81`) is the OR of every deck's non-emptiness; the inner
  tab bar emits each tab only when its data is present
  (`observer_draw.cpp:570-599`, e.g. `!s.syscalls.rows.empty() && BeginTabItem(...)`);
  and when *nothing* is present it draws one truthful placard that **names the kinds
  it looked for** (`observer_draw.cpp:562-567`). This is the exact "reveal-when-present,
  otherwise name the absence" shape T1 lifts to the outer level.
- **The entry point today.** `draw_doors` (`shell.cpp:182-214`) prints
  "asmtest desktop — choose a door" then four buttons — `Learn`, `Open a
  recording...`, `Author`, `Inspect` — plus `Keyboard bindings`; each opens a
  closeable tab (`s.show_learn/_author/_inspect`) or the file dialog. Home is a
  non-closeable tab, not a persistent rail (`shell.cpp:810-814`, which also sets
  `active_tab = -1`). The Learn door already links no engine and needs no deps
  (`shell.cpp:848-865`), so it is the dependency-free landing.
- **The dockspace + presets.** `draw_shell` hosts `DockSpaceOverViewport`
  (`shell.cpp:770`) and a `View` menu with `Reset layout` + three named presets
  (`shell.cpp:793-807`); `layout.cpp:10-14` names the panes (`kPaneHome`,
  `kPaneRecording`, `kPaneScrubber`, `kPaneInspector`, `kPaneTimeline`) and
  `layout_build` (`layout.cpp:32-73`) splits them per `LayoutPreset`
  (`ReplayInspect` / `Author` / `LiveObserver`, `layout.h:19`). Per **F2** these
  panes are orphaned until [19](19-dockable-panes-keystone.md) lands — no view is
  `Begin()`'d under any `kPane*` name; the shell only `Begin("asmtest")`
  (`shell.cpp:792`). T4 builds on the arrangement machinery [19](19-dockable-panes-keystone.md)
  makes real.
- **The document / workspace model.** `Workspace` is a `std::vector<Recording>`
  with `open(path,err)` / `close(idx)` (`workspace.h:15-24`, `workspace.cpp:6-18`),
  no persistence. Each `Recording` keeps its `path` (`recording.h:85`); its stream
  form keeps `id`, the basename, the deep-link `rec` key (`streams.h:112`,
  `recording_id`, `streams.cpp:76`). `ShellState` holds `active_tab`, `b_index`,
  `view`, `selected_step`, `selected_off` (`shell.h:53-71`) — the per-workspace
  position T3 must round-trip.
- **The deep-link serialisation to reuse.** `dt_link { rec, rec_b, view, step,
  off, pid }` (`nav.h:58-69`) with pure, byte-stable `dt_nav_format` /
  `dt_nav_parse` (`nav.h:80-84`), textual form
  `asmtrace-link:v=slice&rec=add_signed.asmtrace&step=4` (`nav.h:1-17`). Every
  persisted position T3 stores is one of these strings; unknown keys are ignored
  (forward-compat), so a newer build's saved workspace still restores on an older
  one.
- **The font + window setup.** `main.cpp:72` creates the window at a hardcoded
  `1280, 720`; `main.cpp:88-91` sets `ConfigFlags |= DockingEnable` and
  `IniFilename = "build/desktop-imgui.ini"`; `main.cpp:99-101` calls `load_fonts`
  then `StyleColorsDark`. `load_fonts` (`fonts.cpp:23-54`) bakes JetBrains Mono +
  merged Codicons/FontAwesome at a hardcoded **15.0f** (`fonts.cpp:30,41,49`) and
  finalises with `io.Fonts->Build()` (`fonts.cpp:52`); it degrades gracefully to the
  bitmap font when the TTF is absent (`fonts.cpp:26-28`). `theme.h` holds only
  `dt_warn_col` / `dt_refuse_col` (the fidelity-chrome amber + red) — no light
  variant, no theme switch.
- **The headless test harness.** `test_shell.cpp` drives `draw_shell` for null-backend
  frames over the fixture workspace (`test_shell.cpp:57-85`), asserting model state
  (`shell_banner`) not pixels; `test_layout.cpp` drives `layout_build` under the
  null backend with `DockingEnable` and inspects the split tree. `make desktop-test`
  runs both; `make desktop-ui-test` (doc 17 T1, `test_ui.cpp`, `imgui_test_engine`)
  is the click-driven interaction lane.

## Tasks

### T1 — data-driven outer view presence + candid "unavailable views" affordance  (M, depends on: 19)

> **LANDED.** `desktop/src/ui/view_presence.{h,cpp}` is the pure predicate; the
> non-docked `draw_recording_tab` and the docked `kPaneRecording` tab bar both
> drive their tab set from it, emitting only present views and collapsing the rest
> into one "unavailable views (N)" tab that names each absent view + its verbatim
> reason. A keymap request for an absent view lands on that affordance
> pre-explained. `door_tabs` is retired (field + render loop + `test_shell` push).
> Tested by `desktop/test/test_view_presence.cpp` + `test_shell`.

**Goal.** Make the outer/inner view set a pure function of the recording's data
and the active mode, by lifting `observer_draw.cpp`'s own empties-gating up a
level: a lean default (Summary / Canvas / Timeline) always; Loom / 3D / PT-slice /
Scrubber / ABI-x-ray / hot-edge heatmaps revealed only when their backing events
or capture are present; the rest collapsed into **one** candid "unavailable views
(N)" affordance that still names each hidden view and its machine reason. Retire
the dead `door_tabs`.

**Steps.**
1. Add a pure predicate module `desktop/src/ui/view_presence.{h,cpp}`:
   `struct ViewPresence { dt_view view; const char *label; bool present;
   std::string reason; }` and
   `std::vector<ViewPresence> view_presence(const Streams &a, const ObserverState
   &obs, const Recording &r, Mode mode)`. Each entry decides `present` from the
   SAME model fields the tab body reads — e.g. Loom present iff the recording is
   exact and carries per-step values (mirror `draw_loom`'s own refusal condition),
   Scrubber present iff `stepidx` is non-empty, 3D present iff
   `SceneView::has_regions` / `space::regions_from_codeimage(r)` is non-empty
   (`shell.cpp:263-273`), ABI x-ray present iff a B is attachable, Observer present
   iff `observer_has_any` (`observer_draw.cpp:76-81`). When absent, `reason` is the
   verbatim machine reason ("no `regstate` ring", "producer was statistical", "no
   codeimage regions") — never a vague "unavailable".
2. `mode` scopes the set (from T2's active mode): Author mode leads with the editor
   + assemble/faults views and hides live-only decks; Inspect/live mode leads with
   the Observer deck; Learn/Open lead with the replay trio. Default (no mode) =
   the lean replay set. A view absent in a mode reports that as its reason too.
3. `draw_recording_tab` (`shell.cpp:384`) drives the inner tab bar from
   `view_presence`: emit a `BeginTabItem` only for present entries, exactly as
   `observer_draw.cpp:570-599` does for the Observer inner tabs. Keep the
   `want_view` / `want_loom` SetSelected honouring (`shell.cpp:397-399,460-462`) —
   but a keymap request for an absent view must land on the "unavailable views"
   affordance with that view pre-explained, never silently no-op.
4. Collapse the absent entries into one trailing affordance — a
   `TreeNode`/`Selectable` labelled `unavailable views (N)` that lists each absent
   view's label + `reason`. This is D7 restructured, not removed: the truth "this
   recording cannot fill view X" stays on screen and named; it is merely graded
   below the present set instead of shown as N empty peer tabs (F4 + the review's
   standing fidelity note — never hide, name the absence).
5. Delete the `door_tabs` field (`shell.h:60`) and its render loop
   (`shell.cpp:897-904`); drop the `test_shell.cpp:85` push. It is dead legacy
   surface the F13 lexicon ("door/tab") also wants gone.

**Tests.** `desktop/test/test_view_presence.cpp` (new, null backend): over the
committed fixtures assert the model, not pixels — a minimal-trace fixture yields
Summary/Canvas/Timeline present and Loom/Scrubber/3D absent *each with a non-empty
reason*; a `codeimage`-bearing fixture flips 3D to present; the "unavailable views
(N)" list names every absent view (count == absent entries, each with its reason).
Extend `test_shell.cpp` to assert `view_presence` flips as recordings are opened.
The click that expands the affordance is a doc-17 `test_ui.cpp` interaction case.

**Docs.** CHANGELOG `Changed`: "Desktop view tabs are now data-driven — only views
a recording can fill are shown; the rest are named in one 'unavailable views'
affordance." `desktop/README.md`: note the lean default set + the presence rule.

**Done when.** the inner (and any outer) view set is a pure function of data ×
mode; absent views are collapsed into one affordance that names each with its
machine reason; `door_tabs` is gone; `test_view_presence` is green.

### T2 — task-language entry on a persistent home/nav rail  (M, depends on: 19)

> **LANDED.** `enum class Mode` + `mode_preset`/`mode_cta` live in the shared
> `desktop/src/ui/mode.h`; `ShellState::mode` defaults to `Learn` (auto-land) and
> `shell_select_mode` sets the mode + a `pending_preset` seam the docked frame
> applies. `draw_home_rail` (the persistent task rail — kPaneHome when docked, a
> left child otherwise) replaces `draw_doors`; the "Home" tab and the "choose a
> door" vocabulary are gone. F13 reconciled: four *task modes*, named as tasks.
> Tested by `test_shell` (auto-land + pending_preset) and `test_ui` (CTA click).

**Goal.** Replace the jargon "choose a door" chooser with task-language modes on a
**persistent** home/nav rail (not a closeable tab): "Learn how assembly runs" /
"Open a trace I have" / "Capture a live process" / "Author a routine", with
Author/Inspect framed below the primary CTAs; auto-land in Learn (dependency-free)
on an empty workspace; the chosen mode drives its dock perspective so the label and
the layout agree.

**Steps.**
1. Add `enum class Mode { Learn, Open, Capture, Author, Inspect }` and
   `ShellState::mode` (default `Learn`). Retire `draw_doors`
   (`shell.cpp:182-214`); build a persistent left rail drawn every frame — dock it
   into `kPaneHome` (`layout.cpp:56`) once [19](19-dockable-panes-keystone.md) hosts
   the pane, so it is a fixed home/nav surface, never a `BeginTabItem` in the `main`
   strip. Remove the `Home` tab (`shell.cpp:810-814`).
2. The rail's primary CTAs are task sentences, not nouns: **"Learn how assembly
   runs"** (opens the Learn player, no deps, no root), **"Open a trace I have"**
   (the file dialog), **"Capture a live process"** (the Inspect flow), **"Author a
   routine"** (the Author editor). Author/Inspect keep their existing state flags
   (`s.show_author` etc.) but are grouped *below* the two learner-first CTAs with a
   one-line "what you get / what it needs" caption each (reuse the existing
   `TextDisabled` captions from `shell.cpp:189,196,201,209`).
3. Resolve the F13 doc-vs-build drift explicitly: the plan says three doors, the
   build renders four peer buttons (`shell.cpp:186-210`). The rail supersedes both
   — there are four *task modes*, named as tasks; the "door" metaphor and the
   `draw_doors` "choose a door" caption are deleted. Note this reconciliation in the
   PR and strike the F13 row.
4. **Auto-land:** when the workspace is empty (`ws.recordings.empty()` and no
   session), `mode` defaults to `Learn` and the rail highlights it — the
   dependency-free path is the recommended one, not a taxonomy the user must decode.
5. **Mode drives perspective:** selecting a mode calls `layout_build` with the
   matching `LayoutPreset` (`Learn`/`Open` → `ReplayInspect`, `Capture` →
   `LiveObserver`, `Author` → `Author`; `layout.cpp:58-70`) so the label and the
   pane arrangement never disagree. This is the payoff of no-longer-orphaned panes
   ([19](19-dockable-panes-keystone.md) / F2). Reuse the existing `layout_build`
   call site (`shell.cpp:795-803`).

**Tests.** `test_shell.cpp` (null backend, model state): an empty `ShellState` has
`mode == Learn` (auto-land); selecting each mode sets the expected `LayoutPreset`
(assert via a seam — have the rail set a `pending_preset` the frame applies, and
assert that field rather than the DockBuilder tree, which `test_layout.cpp` already
covers); the rail is drawn on the empty-workspace frame with no crash. The
click-through (press a CTA → mode changes → matching preset requested) is a doc-17
`test_ui.cpp` interaction case (`ItemClick` the rail button, assert `s.mode`).

**Docs.** CHANGELOG `Changed`: "First run is a persistent task rail (Learn / Open /
Capture / Author), not a 'choose a door' chooser; empty workspaces auto-land in
Learn; the chosen mode sets its dock layout." `desktop/README.md`: the four modes
and what each needs. Strike review F13; update `06-doors-and-learning.md`'s door
framing note to point here.

**Done when.** the rail is persistent (not a closeable tab), task-language,
learner-first; an empty workspace auto-lands in Learn; selecting a mode requests its
perspective; the "door" vocabulary and `draw_doors` are gone.

### T3 — workspace persistence, recents, File menu + drag-drop  (M, depends on: 19)

> **LANDED.** `desktop/src/doc/workspace_state.{h,cpp}` is the pure
> serialise/parse (defensive; unknown keys ignored) + `recents_push`.
> `shell_capture_workspace` / `shell_restore_workspace` round-trip the open set +
> active position + per-pane selection as `asmtrace-link`s; `main.cpp` loads at
> startup, saves debounced on change + on clean exit, and registers the GLFW
> drop callback (manual-smoke). Recents surface on the rail + a File ▸ Open Recent
> menu; a vanished recording is kept-with-error (D7). Tested by
> `test_workspace_state` + `test_shell`.

**Goal.** Persist and restore the Workspace across launches — the open recordings,
the active tab, and each pane's selection, every position an `asmtrace-link` — and
add an MRU recents list on the Home rail and a File menu (each entry a deep-link
reopening to the exact prior position), plus drag-drop. Reframe Home as a recents
landing.

**Steps.**
1. Add `desktop/src/doc/workspace_state.{h,cpp}`: a pure serialise/deserialise of
   `struct WorkspaceState { std::vector<std::string> open; // recording paths
   std::string active; // dt_nav_format of active pos std::vector<std::string>
   pane_links; // one asmtrace-link per open recording's per-pane selection }` to a
   small JSON (nlohmann/json is already a D2 dep). Each position is built with
   `dt_nav_format` (`nav.h:84`) and restored with `dt_nav_parse` (`nav.h:80`) —
   reusing the deep-link spine, so a save is diffable and forward-compatible
   (unknown keys ignored, `nav.h:15-17`).
2. Store it **alongside the dock `.ini`**: `build/desktop-workspace.json`, next to
   `IniFilename = "build/desktop-imgui.ini"` (`main.cpp:91`), git-ignored under
   `build/`. Load once at startup (after `ShellState` construction,
   `main.cpp:105`); save on change (open/close/selection) debounced, and on clean
   exit (`main.cpp:137`). A recording whose path no longer loads is **kept in
   recents with its load error** (mirror `learn_scan`'s "a walkthrough that
   vanished is something the user must see", `doors.h:50-53`) — never silently
   dropped.
3. Restore: for each `open` path call `shell_open` (`shell.cpp:61`); then replay
   `active` + `pane_links` through `dt_nav_go` (`nav.h:111`) so the restored
   workspace lands on the exact prior view/step/offset, identical to a pasted link
   (D4). A path that fails to open routes its error to `s.status`, as `shell_open`
   already does.
4. Add an MRU recents model `struct Recents { std::vector<std::string> paths; }`
   with pure `recents_push` (dedup, most-recent-first, cap ~12) tested directly.
   `shell_open` pushes; persisted in the same JSON.
5. Surface recents in **two** places: (a) the Home rail's landing — reframe it from
   "choose a door" to a recents list, each a `Selectable` that reopens via the
   deep-link path; (b) a `File` menu (add to the `MenuBar`, `shell.cpp:793`) with
   `Open…`, an `Open Recent ▸` submenu, and `Reopen last workspace`. Each recent
   entry reopens to its stored position.
6. Drag-drop: register `glfwSetDropCallback` in `main.cpp` calling `shell_open` per
   dropped path (a one-line adapter). The OS drop-event delivery needs a real
   window, so **that binding is manual-smoke** (noted in the PR, per
   `_conventions.md`); its target — `shell_open` — is already unit-tested and the
   drop handler adds no logic of its own, so nothing testable is left self-skipping.

**Tests.** `test_workspace_state.cpp` (new): assert a `WorkspaceState` round-trips
byte-stably through serialise→parse (open set + active + pane links), and that a
saved workspace restores the same open set and selection after a save/load cycle
(model state — open `Streams` ids and `active_tab`/`selected_step`, not pixels).
`recents_push` unit test: dedup + order + cap. Extend `test_shell.cpp`: open two
fixtures, snapshot, clear, restore, assert the two are open and the active position
matches.

**Docs.** CHANGELOG `Added`: "Desktop remembers your workspace — open recordings,
active view and per-pane selection restore across launches; a recents list and
File ▸ Open Recent; drag-drop to open." `desktop/README.md`: the
`build/desktop-workspace.json` location + that a vanished recording stays in
recents with its error. Strike review F10.

**Done when.** the open set + active tab + per-pane selection persist and restore
across launches as `asmtrace-link`s; a recents MRU shows on the Home rail and in a
File menu, each entry reopening to its exact prior position; drag-drop opens a file;
a vanished recording is kept-with-error, never silently dropped.

### T4 — named perspectives + named saved filter/query presets  (M, depends on: 19, 03/T3 store)

> **LANDED.** `desktop/src/ui/perspectives.{h,cpp}` encodes a perspective as
> `preset:<name>` (built-in, re-split via `layout_build`) or `ini:<blob>` (a
> `SaveIniSettingsToMemory` snapshot), applied by `perspective_apply`; the three
> built-in presets seed the map and a user "Save perspective" adds a snapshot
> (View menu). `FilterPreset{name,query}` + `filter_preset_apply` write a query
> into a view's filter buffer (the "showing N of M" fidelity untouched); both live
> in the T3 store. Tested by `test_perspectives` + `test_workspace_state`.

**Goal.** Once panes are real, let the user name and recall dock *perspectives*
(the VS Code / Blender / JetBrains workspace model) and name saved
filter/query presets, so a task-shaped arrangement and a repeated query are both
first-class, recallable state.

> **Sequencing note.** The restructure plan schedules T2.7 in **Wave 3** (after the
> panes prove out in real use), not Wave 1. It is placed in this brief because it
> belongs to the *container* thematically — perspectives and saved presets are
> workspace state, alongside T1/T2/T3/T5. Land T1–T3 and
> [19](19-dockable-panes-keystone.md) first; T4 is the tail of this brief and may
> defer to Wave 3 without blocking the rest.

**Steps.**
1. **Named perspectives.** ImGui already persists the *current* dock layout to the
   `.ini`. Add `desktop/src/ui/perspectives.{h,cpp}`: a named map
   `{ name → serialised dock layout }` saved in `build/desktop-workspace.json`
   (T3's store). "Save perspective as…" snapshots the current DockBuilder layout
   under a name; selecting one restores it. Build on `layout.cpp`'s existing
   `layout_build`/preset machinery (`layout.cpp:32-73`) — the three built-in
   presets become the seed perspectives, and a user perspective is just a saved
   arrangement over the same `kPane*` windows [19](19-dockable-panes-keystone.md)
   makes real. Add to the `View` menu (`shell.cpp:794`) beside `Reset layout`.
2. **Named saved presets.** Add `struct FilterPreset { std::string name, query; }`
   persisted the same way. A preset names a filter/query string — e.g. the syscall
   name filter (`observer_draw.cpp:112-119`, `ObserverState::syscall_filter`) or a
   Tree filter (`observer_draw.cpp:361-373`). Applying a preset writes its `query`
   into the active view's filter buffer; the view is unchanged otherwise, so
   presets are draw-half chassis over the existing pure filter models (D2 amendment
   point 4). "Showing N of M" fidelity stays exactly as `observer_draw.cpp:116-119`
   renders it — a preset never reads as "the trace only did these".
3. Both live in the T3 workspace JSON so they persist and diff, and both reuse the
   round-trip already tested there.

**Tests.** `test_perspectives.cpp` (new, null backend): a saved perspective
round-trips through serialise/restore and, re-applied under `DockingEnable`,
reproduces the same split tree (assert with `DockBuilderGetNode`, as
`test_layout.cpp` does). `FilterPreset` round-trips and applying one sets the
target filter buffer (model state). The save/apply *click* is a doc-17
`test_ui.cpp` case.

**Docs.** CHANGELOG `Added`: "Named dock perspectives and named saved filter
presets, persisted with the workspace." `desktop/README.md`: how to save/recall a
perspective; note it depends on real panes ([19](19-dockable-panes-keystone.md)).

**Done when.** a user can name, save and recall a dock perspective and a filter
preset; both persist in the workspace store; the "showing N of M" fidelity is
untouched; `test_perspectives` is green.

### T5 — DPI-aware atlas + user text-scale + persisted window size + light theme, in a Settings pane  (L, depends on: 13/F3 fonts)

> **LANDED.** `desktop/src/ui/settings.{h,cpp}` is the pure `Settings` model
> (text_scale / content_scale / win_w×h / light_theme) + serialise/parse (to
> `build/desktop-settings.json`) + `settings_apply_text_scale`. `load_fonts` takes
> `base_px × content_scale` and a `fonts_rebuild_count` seam; `main.cpp` bakes
> DPI-aware, re-bakes on a content-scale change, opens at the remembered size, and
> writes the size back on resize. `theme.h` gains a light-theme flag so the
> warn/refuse chrome keeps contrast in both themes (default dark preserves every
> 24-family value). A Settings pane hosts the slider/theme toggle + the candid
> a11y-scope note (no OS screen-reader tree). Tested by `test_settings` +
> `test_fonts`.

**Goal.** Spend the one accessibility lever fully inside ImGui's control: a
DPI-aware font atlas that rebuilds on content-scale change, a user text-scale
(~0.8×–2.0×), a persisted window size (retiring the hardcoded 1280×720), and a
light theme — all in a small Settings pane.

> **Scope note.** Review F6 / doc 13 F4 scheduled only the *dynamic-DPI decision*.
> User text-scale, the Settings pane, persisted window size and the light theme
> were unplanned anywhere — they are new here. ImGui exposes no OS screen-reader
> tree ([11](11-imgui-addons.md):461, recorded faithfully), so this text-scale lever
> is the accessibility surface the platform actually permits; the pane must say so
> rather than imply broader a11y coverage.

**Steps.**
1. **Settings model.** Add `desktop/src/ui/settings.{h,cpp}`:
   `struct Settings { float text_scale = 1.0f; float content_scale = 1.0f; int
   win_w = 1280, win_h = 720; bool light_theme = false; }` — pure, with
   serialise/parse into T3's `build/desktop-workspace.json` (or a sibling
   `desktop-settings.json`; keep it beside the dock `.ini` either way).
2. **DPI-aware atlas.** Parameterise `load_fonts` (`fonts.cpp:23`) to take a base
   px × `content_scale` instead of the hardcoded `15.0f` (`fonts.cpp:30,41,49`),
   and rebuild the atlas (`io.Fonts->Clear()` → add → `Build()`, plus the backend's
   `*_CreateFontsTexture`) when the GLFW content scale changes
   (`glfwGetWindowContentScale` / the content-scale callback, `main.cpp`). Keep the
   graceful bitmap-font degrade (`fonts.cpp:26-28`).
3. **User text-scale.** A slider ~0.8×–2.0× in Settings. Prefer `io.FontGlobalScale`
   for the cheap live path; re-bake the atlas at the scaled px only when the user
   asks for a crisp rebuild (a note in the pane explains the trade). Persist
   `text_scale`; apply at startup.
4. **Persisted window size.** Read `Settings::win_w/win_h` at `glfwCreateWindow`
   (`main.cpp:72`) instead of the literals; write them back on resize (a GLFW
   size callback) into the store, so the app reopens at the size the user left.
5. **Light theme.** Add a `light`/`dark` toggle that calls `StyleColorsLight` /
   `StyleColorsDark` (`main.cpp:101`) and — critically — swaps `theme.h`'s
   `dt_warn_col`/`dt_refuse_col` for light-legible variants so the fidelity chrome
   (banners, refusals) keeps contrast in both themes (D7 — the warn/refuse signal
   must not wash out on a light background). Add the two variants to `theme.h`
   selected by the active theme.
6. **Settings pane.** A small pane (dock into `kPaneInspector` or a modal from the
   rail) with: text-scale slider, theme toggle, and a one-line candid statement
   that this is the extent of in-app a11y (no screen-reader tree). Wire a seam so
   the pure `Settings` struct is what the tests assert.

**Tests.** `test_settings.cpp` (new, null backend, model state): `Settings`
round-trips through serialise/parse; setting `text_scale` and calling the apply
function sets `io.FontGlobalScale` to that value (assert the IO field, not pixels);
a content-scale change triggers an atlas rebuild (assert a rebuild counter / the
atlas is non-empty at the new px). Extend `test_fonts.cpp`: `load_fonts` at a
scaled px produces a built atlas (`GetTexDataAsRGBA32` non-null). Window-size read
at `glfwCreateWindow` is the one manual-smoke seam (needs a real window); the
`Settings` values it consumes are unit-tested, so nothing testable self-skips.

**Docs.** CHANGELOG `Added`: "Settings pane: user text-scale (0.8×–2.0×), DPI-aware
font atlas, remembered window size, and a light theme." `desktop/README.md`: the
Settings pane + the candid note that text-scale is the in-app a11y lever (no OS
screen-reader tree). Strike review F6. Note doc 13 F4's dynamic-DPI decision is now
implemented here.

**Done when.** the atlas rebuilds on content-scale change; a persisted text-scale
drives `FontGlobalScale`/re-bake; the window reopens at its last size (1280×720
literal retired); a light theme exists and keeps fidelity-chrome contrast; all in a
Settings pane that states its a11y scope faithfully; `test_settings` is green.

## Task order & parallelism

- **T1 and T2 are the entry into the container** and read best together, but are
  independent files — different developers can take them once
  [19](19-dockable-panes-keystone.md) lands (both need real panes: T1 for the pane
  set, T2 for `kPaneHome`). T2's `Mode` feeds T1's mode-scoping, so if split, land
  the `Mode` enum first (a trivial shared header).
- **T3 depends on T2** for the Home-as-recents-landing surface and on the deep-link
  spine (already shipped, `nav.h`); its `workspace_state` store is the substrate
  **T4 and T5** persist into, so land T3's store before T4/T5's persistence.
- **T5 depends only on doc 13 F3 fonts** (the `load_fonts` atlas) and can start in
  parallel with T1/T2; only its *persistence* half waits on T3's store.
- **T4 is the tail** — plan-sequenced to Wave 3; land it last (or defer) without
  blocking T1/T2/T3/T5.

Order: `19` → (T2 `Mode`, then T1 ∥ T2 ∥ T5-core) → T3 store → (T4, T5-persist).

## Constraints & gates

- **Fidelity (D7) is restructured, never removed.** T1's "unavailable views (N)"
  affordance still names every absent view and its verbatim machine reason — it
  grades the absence below the present set, it does not hide it (the review's
  standing note; F4/F5). T4's presets keep `observer_draw.cpp:116-119`'s "showing N
  of M". T5's light theme must keep `theme.h`'s warn/refuse contrast in both themes,
  and the Settings pane states its a11y scope faithfully (no screen-reader tree,
  [11](11-imgui-addons.md):461).
- **Every task is headlessly testable** on the null backend (`make desktop-test`) —
  the pure models (`view_presence`, `WorkspaceState`, `Recents`, `Settings`,
  perspectives) are what the tests assert (D4: model state, not pixels). The three
  click-driven paths (rail CTA, affordance expand, save-preset) are covered by the
  doc-17 `imgui_test_engine` lane (`make desktop-ui-test`, `test_ui.cpp`). The only
  manual-smoke seams — the GLFW drop callback (T3) and the window-size read/write
  (T5) — need a real window, add no logic of their own over already-tested targets,
  and are noted in the PR per `_conventions.md`; nothing is left self-skipping.
- **No new third-party dep.** All of this is in-tree over shipped substrate
  (ImGui docking, `nav`, `layout`, `fonts`, nlohmann/json) — no addon-admission
  (D2) review needed.
- **Persistence lives under `build/`** beside `desktop-imgui.ini` (`main.cpp:91`),
  git-ignored; a corrupt/stale store must degrade to defaults, never crash (F2's
  note that a persisted `.ini` made an upstream crash newly reachable applies to
  the workspace JSON too — parse defensively).
- **Deep-link forward-compat.** T3/T4 store positions as `asmtrace-link`s; unknown
  keys are ignored (`nav.h:15-17`), so a workspace saved by a newer build still
  restores on an older one.

## Out of scope

- **Real dockable panes / flatten the 3-deep nesting** — that is the keystone
  [19](19-dockable-panes-keystone.md) (T2.1); this brief assumes its panes exist.
- **The command palette, wayfinding chrome, overview/minimap, shared selection**
  — Wave 2, [21-spine-navigation.md](21-spine-navigation.md) /
  [22-selection-and-search.md](22-selection-and-search.md).
- **A colour-blind-safe palette / one semantic palette / the unified filter+time
  widget** — Wave 3, [24-one-visual-language.md](24-one-visual-language.md) (F14/
  F15/F16's widget half; T4 here adds only *named* presets over existing filters).
- **Graded fidelity-chrome tiers (schema `severity`)** — Wave 3,
  [23-graded-truth-layer.md](23-graded-truth-layer.md) (F5); T1 here reorganises
  the *view set*, not the per-banner grading.
- **A rebindable-keys / hotkey editor** — ImHotKey is verified broken on the pin
  (doc 11 skip / doc 17 out-of-scope); the keymap itself is doc 17 T1.
