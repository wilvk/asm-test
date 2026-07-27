# Wave 0: stop the breaches — keymap honesty, real Reset, save guard, capability positives, perturb confirm, nav history — implementation

> **Sources.** Actioned from the UX restructure plan
> ([../plans/desktop-gui-ux-restructure-plan.md](../plans/desktop-gui-ux-restructure-plan.md))
> rows **T1.1** (remainder), **T2.2**, **T2.6**, **T4.2**, **T4.5**, **T3.2** —
> the plan's **Wave 0** — and the review findings **F1, F18, F2, F24, F19, F22,
> F11** ([../plans/desktop-gui-ux-review.md](../plans/desktop-gui-ux-review.md)).
> Written 2026-07-27 against HEAD `243f092`. This doc wins over the review/plan on
> disagreement; the CODE wins over this doc — re-verify file:line before editing.
> Read [_conventions.md](../implementations/_conventions.md) first; D1–D11 live in
> this directory's [README](README.md). **Prerequisites:
> [12-addon-supply-chain.md](12-addon-supply-chain.md)** (the test-engine lane
> `make desktop-ui-test`, landed by [17](17-interaction-testing-and-editor.md) T1,
> which T1 below extends).
>
> This whole brief is **independent of the doc-19 docking refactor** (the plan's
> Wave-0 note: most of Wave 0 does not touch the dockable-panes keystone), with
> **one** exception — T2's *stop-doing* clause references
> [19-dockable-panes-keystone.md](19-dockable-panes-keystone.md) (T2.1), which owns
> the View menu / dock presets those panes make real. T2 here does the opposite:
> it makes Reset work *without* waiting for real panes.

## Why this work exists

Wave 0 is the cheap, mostly-independent set of changes that each remove an
**active trust breach** — a lie, an inert control, a data-loss trap, or a
misleading first impression — on shipped substrate:

- **The most navigational surface lies (F1/F18).** The keymap *core* now works
  (doc 17 T1 wired ten bindings in `handle_keymap`), but the help overlay still
  advertises all twelve identically, so a still-unwired convention key (`F` fit,
  `W/S`/`A/D` camera) would advertise as live — and the review's convention-
  alignment keys are not wired at all. The overlay must show only what is wired
  and grey the rest "planned" (the app's own greyed-shows-why law), and the
  convention keys must land, all pinned by the doc-17 engine lane so advertised
  can never again diverge from wired.
- **An inert control persists (F2).** "Reset layout" lives behind a View menu that
  only appears when docking is on, acts through phantom windows, and cannot fire
  in the render-only viewer or in tests. Reset must become real and
  always-available, with a load-fault / zero-visible-pane auto-fallback so a
  mis-docked or crashed `.ini` can never strand the user.
- **A data-loss trap ships (F24).** Author output is ephemeral: assemble, run,
  explore, close the tab, and the recording is gone — `workspace.close()` only
  erases the in-memory entry. Author needs the save path Inspect already has, plus
  a dirty-close guard and a saved/unsaved tab title.
- **A first impression misleads (F19).** On a common host the capability panel
  leads with red greyed errno rows — a learner reads catastrophe when Learn and
  Author need none of it. Lead with a positive summary; demote the negatives; and
  reuse the remedy map `inspect_door` already owns.
- **A target-killing default is unguarded (F22).** Arming a perturbing single-step
  mode dirties the traced page, perturbs timing, and on arm64 can terminate a
  target blocked in a syscall — with no pre-commit confirm. Gate it behind the
  reveal-all two-step, default to the least-perturbing substrate, annotate the
  arm64 hazard.
- **The spine is one-way (F11).** Every deep link replaces context with no marked
  exit back — Nielsen's canonical emergency-exit gap, on the one choke point
  (`dt_nav_go`). A bounded, serialisable back/forward stack closes it.

None of these removes a truth. Where a task touches honesty chrome (T1's overlay,
T4's verbatim reasons, T5's perturb warning) it **restructures, never removes**
(D7, and the review's standing note on F5): the verbatim machine reason stays as a
floor; framing and grading layer on top.

## What already exists (verified 2026-07-27)

The load-bearing inventory — every task below is wiring on this shipped substrate,
not redesign:

- **The keymap core landed (doc 17 T1).** `handle_keymap(ShellState&)`
  (`desktop/src/ui/shell.cpp:586-699`, called at `:759`) is the *one* place keys
  map to state: `1/2/3/4` (`:594-597`), `y` copy-link (`:600`), `Ctrl+G`
  (`:604`), `j/k`+arrows / `PgUp/Dn` (`:618-626`), `Enter` (`:629-633`), `b/f/c`
  cones (`:636-638`), `d/x` diff (`:642-658`), `n/p` divergence (`:663-698`).
  Each is a pure `ShellState` mutation, guarded on `io.WantTextInput` (`:588`) so
  letters are text while a field has focus. View-switch and swap go through
  `want_view` / `want_open_tab` `SetSelected` (shell.cpp:877-892) because a
  keypress cannot select an ImGui tab directly. `[`/`]` stay view-local (scrubber
  / slice / abixray draws).
- **The binding table + overlay.** `dt_binding { const char *keys; const char
  *what; }` (`desktop/src/nav.h:115-118`) — **no `wired` flag today.**
  `dt_nav_bindings()` (`desktop/src/nav.cpp:282-298`) lists the twelve. The help
  overlay `draw_bindings_help()` (`desktop/src/views/diff_view_draw.cpp:58-69`)
  renders **every** row identically — it cannot express wired-vs-planned. It is
  shown from `shell.cpp:911-914`.
- **The engine test lane (doc 17 T1).** `desktop/test/test_ui.cpp` drives the real
  `draw_shell` on the null backend through simulated keypresses and asserts the
  resulting `ShellState` (the model, not pixels — D4); it links imgui_test_engine
  test-lane-only in the `uitest` make tree and runs headless via `make
  desktop-ui-test` (`mk/desktop.mk:360,678`). The keymap tests are
  `register_keymap_tests` (`test_ui.cpp:69`).
- **The layout manager (doc 13 T2).** `layout_build(dockspace_id, size, preset)`
  (`desktop/src/ui/layout.cpp:32-73`) destructively rebuilds the dock tree; it
  docks `kPaneHome/Recording/Scrubber/Inspector/Timeline` (`layout.cpp:10-14,
  56-70`). `layout_exists()` (`layout.cpp:28-30`) tests for a built node. **There
  is no `layout_reset` symbol** — the "reset" is a `MenuItem("Reset layout")`
  calling `layout_build(...ReplayInspect)` (`shell.cpp:795-797`), reachable **only
  when `docking` is true** (`shell.cpp:766-767,793`). `docking` is gated on
  `ImGuiConfigFlags_DockingEnable`, which **only the real app sets**
  (`desktop/src/main.cpp:88`); the null test backend and (per the gate) the
  render-only path leave it off. The real app persists layout to
  `build/desktop-imgui.ini` (`main.cpp:89-91`) — so a bad `.ini` survives across
  launches. `test_layout.cpp` already unit-tests `layout_build` headlessly.
  *(Drift note, load-bearing for T2: the shell comment at `shell.cpp:782` says
  "IniFilename disabled in main.cpp" — stale; `main.cpp:91` now enables it.)*
- **The confirm-overwrite save dialog.** `draw_save_capture(InspectState&)`
  (`desktop/src/ui/inspect_door.cpp:274-342`) writes a recording to `.asmtrace`
  via an `ImGuiFileDialog` with `ImGuiFileDialogFlags_ConfirmOverwrite`
  (`:291-308`), reports the verbatim result (`:328-330`), warns when saving a
  still-growing (torn) capture (`:309-313`), and offers "Open in Loom" for a
  weavable save (`:335-341`). This is the exact reusable save path F24 names.
- **The Author door (doc 17 T2 rewrote it).** `draw_author_door(AuthorState&)`
  (`desktop/src/ui/author_door.cpp:89-200`) now uses a real `TextEditor`
  (`:138-145`) — no 64 KB truncation — assembles + runs (`author_run`, `:29-84`),
  and shows faults as data. It has **no save path**: the run result lives in
  `AuthorState`, is never added to the Workspace, and vanishes on tab close.
- **The workspace / close seam.** `Workspace::close(size_t)`
  (`desktop/src/doc/workspace.h:23`) just erases the vector entry; `shell_close`
  (`shell.cpp:97-100`) calls it; the outer tab's `✕` drives it (`shell.cpp:884,
  893-894`). `Recording` (`desktop/src/doc/recording.h:67`) carries `path`,
  `torn`, `statistical()`, `event_count()` — but **no "authored / unsaved"
  flag.**
- **The capability panel.** `draw_capability_panel(CapState&, const Recording*)`
  (`desktop/src/ui/capability_panel.cpp:58-119`) probes once via `cap_probe`
  (`:29-56`, resolving the library's own cascade + `asmtest_hwtrace_status`), then
  renders **rows first, no positive summary**: `[ok]`/`[grey]` per row with the
  verbatim machine reason *always* wrapped inline (`:99-116`), refusals as
  full-width banners (`:95-98`). It never expands/collapses and never calls the
  remedy map.
- **The remedy map `inspect_door` owns.** `attach_verdict(const AttachFacts&)`
  (`desktop/src/live/inspect.cpp:24-117`) maps concrete conditions to a `why` +
  a `remedy`: i386 tracee (`:48-53`), an existing tracer (`:57-60`),
  `CAP_SYS_PTRACE` override (`:65-71`), Yama `ptrace_scope` 3/2/1 and the uid
  check (`:75-113`). It is rendered in the Inspect process table's "why / remedy"
  column (`inspect_door.cpp:458,477-479`) — but the capability panel does **not**
  reuse it (F19's exact gap). *(Note: `perf_event_paranoid` is a capture gate
  carried in `CapState` / `capview.h:50`, distinct from this attach map; T4 leads
  with the capview positives and layers the attach remedies where they apply.)*
- **The perturbing-mode substrate.** The live-capture mode table
  (`desktop/src/live/budget.cpp:18-51`) carries, per mode, a `ptrace` bool — true
  for every single-stepping mode (`log`, `stream`, `trace`, `dataflow`, `tree`,
  `graph`, `watch`, `auto`) and **false** for out-of-band `sample`
  (`budget.cpp:40-42`, AMD IBS) — plus a `why` string ("SEIZEs every thread and
  single-steps it", etc.). Exposed as `mode_uses_ptrace(LiveMode)`
  (`budget.h:51`) and `mode_jack_reason(LiveMode)` (`budget.h:55`). The arm point
  is the "Start" button in `draw_patch_bay` (`inspect_door.cpp:192`) →
  `inspect_request_start` (`doors.h:169`). A **two-step confirm already exists
  next to it** for swaps: `swap_pending`/`swap_blocker`/`swap_reason`
  (`doors.h:125-127`), armed by `inspect_request_start`, drawn at
  `inspect_door.cpp:212-222`, confirmed by `inspect_confirm_swap`.
- **The reveal-all second-confirm pattern (doc 08).** `obs_syscall_reveal_all`
  (`desktop/src/views/syscalls.cpp`) arms on the first call (returns false), acts
  on the second; `obs_syscall_reveal_all_prompt` returns the verbatim consequence
  sentence. This is the pure arm/confirm pattern T5 reuses; the doc-17 flow test
  `flow/syscall_reveal_all_two_step` already drives it.
- **The router choke point.** `dt_nav_go(dt_nav_table&, const dt_link&)`
  (`desktop/src/nav.cpp:252-280`) is the *single* place navigation happens; on
  success it sets `t.current` (`nav.cpp:273`), an `std::optional<dt_link>`
  (`nav.h:103`) that today holds only the latest position. `dt_link` round-trips
  byte-stable through `dt_nav_format` / `dt_nav_parse` (`nav.cpp:129-240`,
  pinned by `test_nav.cpp`) — so a history of them is trivially serialisable and
  headlessly testable.

## Tasks

### T1 — Honest keymap overlay + convention-alignment keys (T1.1 remainder)  (M, depends on: 12; independent of doc 19)

**Goal.** Close F1/F18's *remaining* breach now that the keymap core is wired:
(a) the help overlay must advertise **only wired keys** and grey the rest
"planned"; (b) land the convention-alignment gestures the review calls for. Every
advertised binding ends the task with a passing engine test, so advertised==wired
cannot re-rot.

**Steps.**
1. **Add a `wired` flag to `dt_binding`** (`nav.h:115-118`): `bool wired;`. Set it
   per row in `dt_nav_bindings()` (`nav.cpp:283-297`) — true for the ten keys
   `handle_keymap` acts on plus the view-local `[`/`]`, false for anything not yet
   mapped. This is the app's own greyed-shows-why law: an unwired row is *shown*,
   labelled, and visibly inert — never hidden, never advertised as live.
2. **Generate the overlay from `wired`** (`draw_bindings_help`,
   `diff_view_draw.cpp:58-69`): render wired rows normally; render `!wired` rows in
   the disabled/planned colour (`ImGui::TextDisabled`, matching the panel's
   grey-row treatment `capability_panel.cpp:100-104`) with a "planned" tag in the
   description column. The overlay stays fed *only* from `dt_nav_bindings()` so
   help and behaviour cannot drift.
3. **Wire the convention keys in `handle_keymap`** (all pure `ShellState`
   mutations, same posture as the landed core, guarded by the existing
   `WantTextInput` check at `shell.cpp:588`), and add each to `dt_nav_bindings()`
   as `wired:true`:
   - `F` — **fit-selection** (frame the current selection; the profiler-standard
     gesture, Perfetto/Tracy `F`). Set a `want_fit` intent the active view honours,
     mirroring `want_view`.
   - `,` / `.` — **step to previous / next sibling** (adjacent invocation /
     sibling node), the second most-used profiler gesture the review names.
   - `W`/`S` zoom + `A`/`D` pan, plus `Ctrl+wheel` zoom and `Shift+drag` pan, on
     the spatial views. **`D` already means diff-detach** in the core — resolve
     with the **labelled context-switch** the plan asks for: WASD means camera
     only when a spatial pane (timeline / 3D) holds focus, and the mode is shown
     in the overlay ("WASD: camera — timeline/3D"); outside that context `d` keeps
     its diff meaning. Make the context an explicit, testable `ShellState` field
     (e.g. `wasd_context`), not an implicit focus guess.
   - `F10` / `F11` — **step / step-into** aliases for `j`/`k`, matching debugger
     muscle memory.
   - `Ctrl+C` — **copy deep link**, an alias of the existing `y` (`shell.cpp:600`);
     keep `y` wired and listed.
4. **Pin every advertised binding** with an engine test in `test_ui.cpp`'s
   `register_keymap_tests` (`test_ui.cpp:69`), following the landed pattern
   (`KeyPress` then `IM_CHECK` on `ShellState`): the new keys, and a test that the
   overlay's wired set equals the set `handle_keymap` acts on (drive
   `dt_nav_bindings()` and assert no row is `wired:true` without a corresponding
   state move — the advertised==wired invariant as a test).

**Tests.** `desktop/test/test_ui.cpp` (the doc-17 `make desktop-ui-test` lane, null
backend, JUnit XML): one section per new key asserting its `ShellState` effect;
one asserting `draw_bindings_help` greys exactly the `!wired` rows; the
advertised==wired invariant test. A pure `test_nav.cpp` case pins the `wired`
flags on `dt_nav_bindings()` (no engine needed for the data itself).

**Docs.** CHANGELOG `Changed`: keyboard-help overlay now marks planned-vs-wired
bindings; `Added`: `F` fit-selection, `,`/`.` sibling step, WASD camera + Ctrl/Shift
mouse, F10/F11 step, Ctrl+C copy-link. `desktop/README.md`: the keymap table with
the WASD context note.

**Done when.** the overlay greys every unwired binding and advertises no dead key;
`F`, `,`/`.`, WASD (+Ctrl-wheel/Shift-drag), F10/F11, Ctrl+C are wired with the
labelled WASD context; every `wired:true` binding has a passing `desktop-ui-test`;
the advertised==wired invariant is a test that fails if a row is added without its
handler.

### T2 — Real, always-available `layout_reset` with auto-fallback (T2.2)  (S, depends on: nothing; references doc 19)

**Goal.** Make F2's inert Reset control **real and always-available in both
binaries** — with a keybinding and (once T3.1's palette lands, doc 21) a palette
entry — and add a load-fault / zero-visible-pane auto-fallback to the shipped
default, so a mis-docked or crashed `build/desktop-imgui.ini` cannot strand the
user across launches.

**Steps.**
1. **Introduce a real `layout_reset(ImGuiID dockspace_id, ImVec2 size)`** in
   `layout.cpp` (beside `layout_build`, `layout.cpp:32`) that rebuilds the shipped
   default (`LayoutPreset::ReplayInspect`) and, distinct from the destructive
   `layout_build`, is safe to call as a recovery: it removes a corrupt node and
   re-splits. Keep it a thin, headlessly-testable wrapper so `test_layout.cpp` can
   drive it without a display.
2. **Make it always-reachable, not View-menu-only.** Add a keybinding through the
   same `handle_keymap` seam (a `ShellState` intent like `want_layout_reset`
   consumed near the dockspace build, `shell.cpp:769-778`), so Reset works whether
   or not the menu bar is shown, and list it in `dt_nav_bindings()` as `wired`
   (feeding T1's overlay). The command-palette entry is deferred to doc 21 T3.1;
   this task only leaves the dispatch seam ready.
3. **Auto-fallback on a bad layout.** Where the shell builds/loads the dockspace
   (`shell.cpp:769-778`), detect the two fault shapes and rebuild from default:
   (a) `!layout_exists(dockspace_id)` after a load attempt (the `.ini` failed to
   produce a node), and (b) **zero visible panes** — no `kPane*` node is visible
   — which is the strand the persisted `.ini` (`main.cpp:91`) makes reachable.
   Make the "is any pane visible?" decision a pure predicate in `layout.cpp` so it
   is testable on synthetic node state. *(Fix the stale `shell.cpp:782` comment —
   the `.ini` is enabled now.)*
4. **Both binaries.** Ensure the reset intent and fallback compile and run with
   docking off too: in the render-only viewer and the null test backend `docking`
   is false, so guard the DockBuilder calls but keep the intent/fallback *decision*
   reachable and asserted headlessly.

**Stop-doing (per the plan).** Do **not** ship the View menu / dock presets as a
finished feature here — the panes are still orphaned (no view is `Begin()`'d under
a `kPane*` name; the presets act on phantom windows). Making them real is
[19-dockable-panes-keystone.md](19-dockable-panes-keystone.md) T2.1's job. T2's
scope is exactly: Reset works, is always available, and auto-recovers — so a bad
layout is never a dead end even before the panes are real.

**Tests.** `desktop/test/test_layout.cpp` (headless, already drives
`layout_build`): assert `layout_reset` produces the default split from a corrupt /
empty node; assert the "zero visible pane" predicate fires on a synthetic no-pane
state and not on a healthy one. A `test_ui.cpp` case (or a `test_shell` case)
drives the `want_layout_reset` intent and asserts the shell requests a rebuild.

**Docs.** CHANGELOG `Fixed`: "Reset layout" is now a real, always-available action
with auto-fallback from a corrupt persisted layout (previously inert). `Added`:
the reset keybinding. `desktop/README.md`: how to recover a broken layout (the key,
and that a bad `build/desktop-imgui.ini` now self-heals).

**Done when.** `layout_reset` restores the shipped default from any state including
a corrupt/empty `.ini`; it is bound to a key and fires with or without the menu bar,
in both binaries; a zero-visible-pane launch auto-falls-back rather than showing an
empty window; the presets/View-menu-as-feature are explicitly left to doc 19; all
of it is asserted headlessly.

### T3 — Author output save path + dirty-close guard (T2.6)  (S, depends on: nothing)

**Goal.** Close F24's data-loss trap: give Author (and any live output) a save
path by reusing `draw_save_capture`, treat unsaved output as **dirty** so a close
cannot silently lose it, and mark saved-vs-unsaved in the tab title.

**Steps.**
1. **Give the Author run a save action** by reusing `draw_save_capture`
   (`inspect_door.cpp:274-342`) — the confirm-overwrite `ImGuiFileDialog` path —
   rather than writing a second saver. The Author run currently lives only in
   `AuthorState` (`author_door.cpp`); to save it, materialise the run as a
   `Recording` (the same NDJSON a `--record` author run writes) so the existing
   `save_recording_file` + dialog apply unchanged. Add the "Save .asmtrace" +
   "Browse…" affordance to `draw_author_door` (after the run result,
   `author_door.cpp:159-198`).
2. **Track dirty state.** Add an "authored / unsaved" flag on the owning entry
   (an `AuthorState` field, and — when the authored run is promoted into the
   Workspace — a `Recording`-level flag, `recording.h:67`, since `Recording` has
   none today). A fresh run sets dirty; a successful save clears it.
3. **Guard the close.** `workspace.close()` (`workspace.h:23`) / `shell_close`
   (`shell.cpp:97-100`) today only erases the entry. Before erasing a dirty entry,
   either **prompt save/discard/cancel** (a modal, driven by a `ShellState`
   intent so it is testable), **or** auto-persist to a scratch recording so the
   close is reversible. Prefer the explicit prompt for Author output (the user
   asked to author it); the scratch-recording fallback is acceptable if the prompt
   is impractical for a given path — state which you chose.
4. **Mark the title.** In the outer tab loop (`shell.cpp:871-886`) append a dirty
   marker (a trailing `*`, VS-Code style) to a dirty entry's title, cleared on
   save. Keep the stable `###recN` id (`shell.cpp:873-874`) so the marker never
   reorders or duplicates tabs.

**Tests.** `desktop/test/test_shell.cpp` (headless): closing a dirty authored entry
does **not** silently drop it — assert the save/discard/cancel intent is raised (or
the scratch recording exists) rather than an immediate erase; a save clears dirty
and the title marker; a clean (saved) entry closes without a prompt. Reuse
`test_author_vm` for the run→Recording materialisation. The save-file mechanics are
already covered where `draw_save_capture` is (`test_inspect`); assert only the new
dirty/close/title logic here, on the pure model.

**Docs.** CHANGELOG `Fixed`: Author output can now be saved and a close no longer
loses it silently. `Added`: unsaved-work marker in the tab title. `desktop/README.md`:
note that Author output is saved the same way live captures are.

**Done when.** an Author run can be saved via the confirm-overwrite dialog; a dirty
Author tab cannot be closed without a save/discard/cancel choice (or an
auto-persisted scratch recording); the tab title shows saved-vs-unsaved; a saved
tab closes cleanly; all asserted headlessly.

### T4 — Capability panel leads with positives + reuses the remedy map (T4.2)  (S, depends on: nothing)

**Goal.** Fix F19's misleading first impression: lead `capability_panel` with a
**positive one-line summary** from the same resolvers, demote unavailable backends
into an expandable "why can't I capture X?" with the verbatim reason collapsed
under each row, and reuse the `inspect_door` why→remedy map so a refusal offers a
next step — verbatim reason stays as a floor, remedy layers on top.

**Steps.**
1. **Derive a positive summary line** from the already-resolved `CapState` rows
   (`capability_panel.cpp:58-119`; the cascade + statuses come from `cap_probe`,
   `:29-56`). Compose it in a pure helper in `capview.h`/`capview.cpp` (so
   `test_capview` drives it), e.g. "This host: emulator + single-step available;
   IBS/PT unavailable — details below", and always include the honesty floor the
   review names: "Learn and Author work here — no root, hardware, or attach
   needed" (both are engine-light doors that need none of the capture backends).
   Render this **first**, before any row.
2. **Demote the negatives.** Move the unavailable / below-fidelity-line rows
   (`capability_panel.cpp:88-116`) under an expandable "why can't I capture X?"
   (`ImGui::CollapsingHeader`, collapsed by default). Keep the verbatim machine
   reason (UI LAW 1, `capability_panel.cpp:109-116`) **collapsed under each row**,
   not deleted — this is a restructure of honesty chrome, not a removal (D7).
   Available backends stay visible above the fold.
3. **Layer the remedy map.** For a row whose reason matches a recognised
   condition, call `attach_verdict` (`inspect.cpp:24-117`) — the map `inspect_door`
   already renders (`inspect_door.cpp:477-479`) — and show its `remedy` under the
   verbatim `why` (paranoid/Yama/i386/CAP_SYS_PTRACE). Keep the resolver pure and
   in `capview.h` so the mapping is tested, not rebuilt inline in the draw.

**Tests.** `desktop/test/test_capview.cpp` (headless, drives the capability model on
synthetic data): assert the positive summary is present and correct for
(a) a bare host (paranoid=4, no PT/IBS) — summary leads with what *does* work, the
Learn/Author floor is stated, negatives are collapsed; (b) a capable host —
positives lead. Assert each recognised reason yields the matching remedy from the
shared map (a refused row carries a next step, not just an errno). Assert no
verbatim reason is dropped — every negative row still carries its machine reason
under the fold.

**Docs.** CHANGELOG `Changed`: the capability panel now leads with what the host
*can* do and offers remedies for what it can't (previously an errno wall).
`desktop/README.md`: note the "why can't I capture X?" expander.

**Done when.** the panel opens with a positive one-line summary and the
Learn/Author floor; unavailable backends are collapsed under one expander with the
verbatim reason preserved; recognised refusals show the shared remedy; a bare host
no longer reads as "the tool does not work here"; asserted on synthetic data in
`test_capview`.

### T5 — Perturbing single-step arm confirm + least-perturbing default (T4.5)  (M, depends on: nothing)

**Goal.** Close F22: gate arming a perturbing single-step mode on a live target
behind an inline confirm stating the concrete consequence; default the capture
picker to the least-perturbing substrate the host supports; grey/annotate
single-step for arm64 blocking-syscall targets. Reuse the reveal-all
second-confirm pattern.

**Steps.**
1. **Gate the arm behind a two-step confirm** for a perturbing mode. `Start`
   (`inspect_door.cpp:192`) → `inspect_request_start` (`doors.h:169`) fires the
   command directly today. When `mode_uses_ptrace(s.want)` (`budget.h:51`) is
   true, first *arm* (like the swap confirm already beside it,
   `inspect_door.cpp:212-222`, and the reveal-all arm/confirm in
   `syscalls.cpp`): add a `perturb_pending` / `perturb_reason` to `InspectState`
   (`doors.h:105-157`, mirroring `swap_pending`), draw the consequence prompt, and
   only the second click starts. The out-of-band `sample` mode
   (`mode_uses_ptrace==false`, `budget.cpp:40-42`) skips the gate.
2. **State the concrete consequence, verbatim.** The prompt is a pure model
   sentence (a `mode_perturb_warning(LiveMode, arch)` helper in `budget.h`, so
   `test_budget` pins it): "single-step **dirties the traced page and perturbs
   timing**; on **arm64** it can terminate a target blocked in a syscall and
   **detach cannot undo it** — prefer IBS/PT." Reuse `mode_jack_reason`
   (`budget.h:55`) for the per-mode "what it does" detail.
3. **Default to the least-perturbing substrate the host supports.** The picker
   defaults `s.want = LiveMode::Log` (`doors.h:122`). Change the default to the
   least-perturbing mode the *host* supports: `sample` (IBS) where the capability
   probe reports it available, otherwise the lightest ptrace mode — a pure choice
   over the resolved `CapState` / `budget` model, testable in `test_budget`.
4. **Grey/annotate arm64 blocking-syscall targets.** In `draw_patch_bay`
   (`inspect_door.cpp:175-181`) annotate (and, where the target arch is known to
   be arm64, `BeginDisabled`) the single-step modes with the blocking-syscall
   hazard, driven by the same pure `mode_perturb_warning` predicate keyed on arch
   — never a hidden refusal, always a stated one. Reference the recorded hazard
   (PTRACE_SINGLESTEP on a thread in a blocking syscall survives DETACH on arm64).

**Tests.** `desktop/test/test_budget.cpp` (headless): assert `mode_uses_ptrace` gates
which modes need the confirm; assert `mode_perturb_warning` returns the arm64
sentence for a ptrace mode on arm64 and the generic one elsewhere; assert the
least-perturbing default resolves to `sample` when available and the lightest
ptrace mode otherwise. `desktop/test/test_inspect.cpp`: assert `inspect_request_start`
on a perturbing mode **arms** rather than fires, and only a confirm starts (mirror
the existing `swap_pending` test); a non-perturbing (`sample`) start fires directly.

**Docs.** CHANGELOG `Added`: pre-commit confirm before arming a perturbing
single-step capture, with the arm64 target-termination warning; least-perturbing
default. `desktop/README.md`: the perturbation/arm64 note (link the recorded
detach-fatal hazard).

**Done when.** arming any `mode_uses_ptrace` mode requires an explicit second
confirm stating the page-dirty / timing / arm64-kill consequence; `sample` and
other non-perturbing modes start without it; the picker defaults to the
least-perturbing supported substrate; arm64 single-step is greyed/annotated with
the stated hazard; asserted headlessly in `test_budget` + `test_inspect`.

### T6 — Bounded back/forward history in the router (T3.2)  (S, depends on: nothing)

**Goal.** Close F11's one-way spine: give `dt_nav_go` a bounded back/forward
history stack (`Alt+Left`/`Alt+Right` + a breadcrumb), in the one place navigation
happens, as a vector of serialisable `asmtrace-link`s — the plan's "cheapest big
lever".

**Steps.**
1. **Add the stack to `dt_nav_table`** (`nav.h:92-104`, beside `current`,
   `nav.h:103`): a `std::vector<dt_link> back`, a `std::vector<dt_link> forward`,
   and a bound (drop the oldest past a cap so a long session cannot grow
   unboundedly). On a **successful** `dt_nav_go` (`nav.cpp:252-280`, at the
   `t.current = link` point, `nav.cpp:273`), push the *previous* `current` onto
   `back` and clear `forward` — the standard browser-history discipline. Guard
   against pushing a no-op re-navigation to the same link.
2. **Add `dt_nav_back` / `dt_nav_forward`** free functions (nav.cpp, beside
   `dt_nav_go`) that pop from one stack, push `current` onto the other, and
   re-run the handler through the *same* `dt_nav_go` path (so a back-jump lands
   identically to a fresh navigation — no divergent code path). They return false
   (with `last_error`) at the ends of the stacks. Keep them free of the shell type,
   exactly as `dt_nav_go` is (`nav.h:86-89`), so `test_nav` links them.
3. **Bind the keys** through `handle_keymap` (`shell.cpp:586`): `Alt+Left` →
   `dt_nav_back`, `Alt+Right` → `dt_nav_forward` (via `IsKeyChordPressed`, like
   `Ctrl+G` at `shell.cpp:604`). Add both to `dt_nav_bindings()` as `wired` so
   T1's overlay advertises them.
4. **Breadcrumb.** Render a compact breadcrumb of the recent stack near the status
   line, each segment a clickable `dt_link` (formatted via `dt_nav_format`,
   `nav.cpp:213`) that jumps through `dt_nav_back`/`forward`. The persistent
   wayfinding chrome proper is doc 21 T3.3; here it is the minimal back/forward
   affordance only.

**Tests.** `desktop/test/test_nav.cpp` (pure, headless — already tests
parse/format/route): a scripted sequence of `dt_nav_go` calls then
`dt_nav_back`/`dt_nav_forward` lands on the expected links; a new navigation after
a back clears `forward`; the stack respects its bound (oldest dropped); back/forward
at the ends return false with a reason; the whole stack round-trips through
`dt_nav_format`/`parse` (it is just serialisable links). An engine test in
`test_ui.cpp` asserts `Alt+Left`/`Alt+Right` drive the router.

**Docs.** CHANGELOG `Added`: back/forward navigation history (`Alt+Left`/`Right`) +
breadcrumb over the deep-link router. `desktop/README.md`: the history keys.

**Done when.** `dt_nav_go` maintains a bounded back/forward stack of serialisable
links; `Alt+Left`/`Right` walk it and land identically to fresh navigations; a new
jump clears forward; the breadcrumb jumps to prior positions; all asserted in
`test_nav` (pure) + `test_ui` (keys).

## Task order & parallelism

All six tasks are **independent** and can go to six developers at once — they touch
disjoint surfaces (overlay/keymap; layout; author/workspace; capability panel;
patch-bay; router). Two soft couplings only:

- **T1 owns the `wired` flag on `dt_binding` and the overlay generator.** T2 and T6
  each add a `wired:true` row to `dt_nav_bindings()` (Reset key; `Alt+Left/Right`);
  whoever lands first adds the flag, the others add rows. Trivial merge.
- **T4 and T5 both read the capability model** (`CapState`/`capview`): T4 for the
  positive summary, T5 for the least-perturbing default. Non-conflicting reads; no
  ordering needed.

Prefer landing **T1** first (it establishes the honest-overlay substrate the plan
makes a standing rule — "never advertise an affordance that is not wired"), but
nothing blocks on it.

## Constraints & gates

- **Honesty (D7) is restructured, never removed.** T1's overlay *shows* unwired
  keys greyed (never hides them); T4 keeps every verbatim machine reason under the
  fold; T5's confirm states the consequence in full. No task hides a truth (the
  review's standing note; F5).
- **Model, not pixels (D4).** Every task's logic lives in a pure, headlessly
  testable helper (`dt_binding.wired`, `layout_reset` + visible-pane predicate, the
  dirty/close decision, the `capview` summary + remedy resolver,
  `mode_perturb_warning` + least-perturbing default, the router stack); the draw
  code only renders it. UI-interaction assertions go through the doc-17
  imgui_test_engine lane (`make desktop-ui-test`, `test_ui.cpp`) — the null
  backend, JUnit XML. **No task is manual-smoke-only.**
- **Both binaries (D4/D9).** T2's Reset and T3's save/close must compile and behave
  in the render-only viewer (docking off, no engine) as well as the full app; guard
  DockBuilder/engine calls, keep the decisions reachable and asserted.
- **No new dependency.** Every task wires shipped substrate; nothing here clears the
  D2 addon-admission bar because nothing here adds an addon. `make desktop-test` and
  `make desktop-ui-test` stay the lanes.
- **Test-lane licensing unchanged.** T1's new tests ride the existing test-lane-only
  imgui_test_engine posture (doc 17 T1); the shipped binaries never link it.

## Out of scope

- **Real dockable panes / the View menu as a finished feature** — the T2 keystone
  ([19-dockable-panes-keystone.md](19-dockable-panes-keystone.md) T2.1). T2 here only
  makes Reset real + auto-recovering.
- **Shared brushing-and-linking selection** (T1.2), **keyboard camera on the 3D/DAG
  islands** (T1.3), **global find** (T1.4), **app-level undo** (T1.5) — Wave 2,
  [22-selection-and-search.md](22-selection-and-search.md) / doc 21. T1 wires the
  keys; it does not build shared selection or per-view undo.
- **The command palette** (T3.1) and **persistent wayfinding chrome / minimap**
  (T3.3/T3.4) — [21-spine-navigation.md](21-spine-navigation.md). T2/T6 leave the
  palette dispatch seam and a minimal breadcrumb; the palette and full chrome are
  doc 21.
- **Workspace persistence + recents** (T2.5), **DPI/text-scale/Settings pane**
  (T2.8), **task-language entry rail** (T2.4), **data-driven outer tabs** (T2.3) —
  [20-workspace-and-settings.md](20-workspace-and-settings.md) / doc 19.
- **Graded honesty tiers + schema `severity`** (T4.1), **session-end placard**
  (T4.3), **split "paused"** (T4.4), **progress everywhere** (T4.6), and the whole
  **one-visual-language** theme (T5.1–T5.5) — Wave 3,
  [23-graded-truth-layer.md](23-graded-truth-layer.md) /
  [24-one-visual-language.md](24-one-visual-language.md).
