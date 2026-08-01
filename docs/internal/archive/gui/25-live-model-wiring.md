# Wave 4: live model wiring — the growing capture as a first-class workspace recording, so Loom / Slice / 3D go genuinely live — implementation

> Closes the gap the 2026-07-27 live-vs-replay audit surfaced: today only the
> Observer deck streams during a capture; the Loom, Slice explorer, operand
> Timeline and 3D overview are reachable over a live session **only** by saving
> the capture to `.asmtrace` and reopening it as a replay
> ([`draw_save_capture`](../../../../desktop/src/ui/inspect_door.cpp),
> "Open in Loom" → `handle_inspect_open_request` → `shell_open`). The data is
> already there — serve `dataflow`/`auto` emit exact `df_step`; `trace`/`dataflow`/
> `auto` emit `codeimage` — so this is a **wiring** brief, not a new subsystem:
> promote the growing `Recording` into the same workspace model the docked panes
> and `view_presence` already read.
>
> **Status (2026-07-28).** ✅ **7/7 — all tasks landed.** T1–T5, T7 landed
> 2026-07-27; T6 completed 2026-07-28. T6's live re-weave core
> (camera-preserving 3D re-weave-on-growth) landed first; its per-tid trajectory
> overlay — the last tail, tracked with [10](10-spacetime-3d-overview.md) T5 —
> now lands too: `build_trajectories` weaves the `df_step` offset stream as a
> region-relative, per-tid path when a recording carries no `trace` (the live
> serve `dataflow`/`auto` case), so the 3D overview shows a faithful
> routine-relative execution path during a single-step capture instead of empty
> terrain. The Scrubber's `regstate` ring, once out of scope here, went live
> under [26](26-live-regstate-producer.md).

## Why this work exists

`view_presence` (doc 20 T1) made the view set a **pure function of a recording's
data × mode** — Loom present iff exact + `df_step`, Slice iff `df_step`, 3D iff
`codeimage`, Observer iff any live kind, Scrubber iff a `regstate` ring. Live and
replay share that one predicate and the same view bodies. The only reason a live
capture doesn't light those views is mechanical: the full parallel indices
(`streams` / `observers` / `stepidx` / `scenes`) are built **exclusively in
`shell_open`**, which takes a file path and calls `Workspace::open`
(`desktop/src/doc/workspace.cpp` — the only `push_back` into `ws.recordings` in
the tree). A `LiveSession`'s `growing()` / `recordings()` are `Recording` objects
that never enter the workspace; the Inspect door builds only a single
`ObserverState` over them (`draw_live_views`, `inspect_door.cpp`).

The design already anticipates the fix. `ShellState::observers` is documented as
"parallel to ws.recordings … **A live session's recording and a replayed file
feed the SAME deck, which is the property the whole doc is built on**"
(`shell.h`). This brief realises that sentence for the whole index set, not just
the observer deck — and in doing so lands doc 05's deferred **Phase-3 "ptrace
dataflow" Loom rung** for the (perturbing) live case.

## What already exists (verified 2026-07-27)

- **The producer.** Serve modes `dataflow` and `auto` run a live ptrace
  single-step engine (`asmspy_engine_dataflow`) that emits `df_step`/`df_edge`
  stamped `exact:1 / trust:"exact"` (`cli/asmspy.c` SERVE_MODES table); `trace`/
  `dataflow`/`auto` arm and emit `codeimage` (host-gated, Linux ≥ 6.7 soft-dirty).
  So live captures can carry the streams Slice / Loom / 3D need. **No `regstate`
  producer exists in serve** — the register ring is an emulator `--steps` product
  ([09](09-teaching-producers.md)), so the **Scrubber stays faithfully absent live**
  and this brief does not change that.
- **The builders are Recording-pure.** `decode_streams` / `observer_build` /
  `build_step_index` and the scene weave all take a `const Recording &` — they do
  not care whether it came from a file or `LiveSession::growing()`.
- **The dock preset is ready.** `LayoutPreset::LiveObserver` already docks the
  Loom, Timeline, Scrubber and Observer panes (`layout.cpp`); they are empty
  during capture only because no workspace recording is active.
- **The rebuild-on-growth gate exists.** `draw_live_views` already rebuilds its
  observer only when `event_count()` moves (`inspect_door.cpp`) — the exact gate
  this brief reuses for the full index set.
- **Testing.** `test_live_session` drives the wire protocol with no subprocess
  (`feed_line`); `test_shell` / `test_view_presence` link the full shell object
  set (`DESKTOP_TEST_SHELL_OBJ`) and drive `view_presence` + `draw_shell` on the
  null backend. T7 lands in `test_shell` — no new mk binary needed.

## Tasks

### T1 — promote the growing capture into the workspace as one live tab  (M)

Add a `shell_sync_live_tab(ShellState &)` that, each frame, keeps exactly one
synthetic workspace entry mirroring `s.inspect.session.growing()` (or, once a
session ends, its last completed recording):

1. **Create.** When a live recording first exists and `live_tab < 0`, append a
   copy to `ws.recordings`, resize the five parallel vectors
   (`streams`/`observers`/`stepidx`/`scrubber_playhead`/`scenes`) to match, record
   `live_tab`, and — only if `active_tab < 0` — select it so the docked panes
   light up. The entry carries the live recording's empty path; its identity for
   links is its stream id (`"live-session"`, as the Inspect door already uses).
2. **Teardown.** When no live recording exists (session `reset()` / never
   started), erase the slot via the existing `shell_close` index-shift machinery,
   clear `live_tab`, and clamp `active_tab`.
3. The slot is appended at the **end**, after any file tabs; `shell_open` also
   appends at the end, so opening a file never shifts `live_tab`. Only closing an
   *earlier* tab does — handled in T4.

**Done when.** a running `dataflow` session appears as one workspace tab; ending
the session freezes it; `reset()` removes it; opening/closing file recordings
alongside it never corrupts the parallel-vector indices.

### T2 — rebuild the live tab's indices on growth, not per frame  (M, depends on: T1)

Gate on `growing()->event_count()` and `recordings().size()` exactly as
`draw_live_views` does. When either moves: refresh the workspace copy
(`ws.recordings[live_tab] = *live`), then re-run `decode_streams` +
`observer_build` (with the `ObsLifecycle` from `session.notes()`, since a live
session's lifecycle is outside its recording, 07-T3) + `build_step_index`, and
reset the scene slot to force a lazy re-weave. All under the one gate, so a
static frame costs nothing and the O(n) rebuild matches the Observer deck's
already-accepted profile (a single-stepped live capture's event rate is low).

**Done when.** `view_presence` over the live tab reports **Loom + Slice + Timeline
present** for a `dataflow`/`auto` session and **Observer present** for any live
kind; feeding more events moves the built-event count and updates the decoded
streams; a static frame triggers no rebuild.

### T3 — lifecycle: end-of-session, disconnect, and no duplicate with save→reopen  (S, depends on: T1)

The frozen live tab and the existing "Open in Loom" (which reopens the *saved
file* as a separate normal tab) coexist. Keep both — the live tab is ephemeral
(not persisted), the reopened file is the permanent copy. Make "Open in Loom" a
no-op-or-repoint when the live tab already shows that capture, so a mid-capture
save does not spawn a confusing duplicate.

**Done when.** ending → saving → "Open in Loom" yields one persistent file tab
and at most one ephemeral live tab, never two live-looking tabs of the same run.

### T4 — guards: persistence, recents, close, index-shift  (S, depends on: T1)

- `shell_capture_workspace` must **skip `live_tab`** in both `ws.open` (an empty
  path would fail to reopen) and `pane_links`. `recents_push` already early-returns
  on an empty path, so recents is safe by construction.
- `shell_close` must adjust `live_tab` the way it already adjusts `b_index` /
  `close_pending`: decrement when an earlier tab closes, clear when the live tab
  itself is closed.

**Done when.** a workspace with a live tab open round-trips through
serialize/restore with the live tab silently dropped (not a failed reopen), and
closing any tab leaves `live_tab` pointing at the right recording or `-1`.

### T5 — fidelity chrome for the live weave  (S, depends on: T2)

While the active tab is the live tab and it is still growing, the Loom / Slice /
center panes carry a banner: **"live weave — the target is being single-stepped
(perturbing), and this recording is still growing (torn)."** This is the graded
truth ([23](23-graded-truth-layer.md)) for a live exact-dataflow capture: the
values are real but the run is perturbed and incomplete. The Scrubber pane keeps
its existing "record with `--steps`" placard — no false promise that live gains a
register ring.

**Done when.** the live Loom/Slice show the perturb+growing banner; a replayed
file shows neither; the Scrubber placard is unchanged.

### T6 — 3D overview live (optional / follow-on, tie to [10](10-spacetime-3d-overview.md) T5)  ✅ DONE

3D *presence* already works live at T2 (the tab gates on `regions_from_codeimage`,
which is cheap; the terrain weave is lazy on first view). This task is only the
**live re-weave-on-growth** of an opened 3D pane + doc 10 T5's per-tid trajectory
overlay (absolute-basis PT traces give real paths; single-step traces are
region-relative → labelled). Deferrable without blocking T1–T5.

> **Landed 2026-07-28.** The re-weave-on-growth core landed first
> (`shell_sync_live_tab` resets `scenes[i]` to force a lazy 3D re-weave over the
> grown recording while preserving the interactive camera / HUD / primer). The
> trajectory overlay was the remaining tail: a live serve `dataflow`/`auto`
> session emits `df_step`/`df_edge` and **no** `trace` (the SERVE_MODES table),
> so `build_trajectories` — which built the PC path from `trace` only — produced
> an empty set and the live single-step 3D pane drew terrain with no execution
> path. It now falls back to the `df_step` offset stream when `trace` is absent,
> woven **region-relative** (`TRAJ_RELATIVE_BASIS`, `basis:"rel"`) and grouped
> per tid, exactly as a `trace basis:"rel"` path is. Those region-relative
> offsets do not project onto the absolute plane, so the renderer never fakes a
> true address-space path (doc 10 T5's fidelity rule) and the HUD shows the
> existing *"rel: routine-relative (not a true path)"* chip. `trace`, when
> present (an emulator `--dataflow` file carries both), stays authoritative — the
> fallback is inert, so nothing regresses. Pinned by three `test_trajectory`
> cases (df_step-only → rel per-tid path; trace-wins-over-df_step; df_step
> per-tid split); the shell weave reaches it through the unchanged
> `draw_scene_overview` → `build_trajectories` call, and the live re-weave
> through `shell_sync_live_tab`.
>
> **Corrected by [36](36-anchor-the-3d-plane.md) (2026-07-29).** The chip claim
> above was a D7 breach: the HUD basis chip keyed on `terr.basis` (the *canvas*
> basis), which is empty for a df-only recording — so **no basis chip drew at all**
> for a live single-step capture, and its region-relative path projected onto
> nothing, so the tube was silently dropped. 36 fixes both: the HUD now falls back
> to the trajectory's basis (the rel chip finally fires), and the rel path is
> ANCHORED to the recording's single `codeimage` span (`base+off`) so it draws —
> with the derivation stated in the HUD, never faked as a measured address.

### T7 — headless test in `test_shell`  (S, depends on: T2, T4)

Feed a synthetic live `dataflow` session through `s.inspect.session.feed_line`
(exact header + `df_step` events, no `end` → growing), call
`shell_sync_live_tab`, and assert `view_presence` over `live_tab`: **Loom present,
Slice present, Scrubber absent (its `--steps` reason verbatim), Observer per
kinds**. Feed more `df_step`, re-sync, assert the built-event count moved and the
decoded `df` grew. Assert `shell_capture_workspace(s).open` omits the live tab.

**Done when.** the assertions above pass on the null backend under
`make desktop-test` / `make docker-desktop`.

## Task order & parallelism

`T1` (the slot) is the entry; `T2` (rebuild) and `T4` (guards) both build on it and
can land together; `T3` and `T5` are small polish over T1/T2; `T7` verifies T2+T4.
`T6` is the tail — schedule with doc 10 T5 or defer.

Order: `T1` → (`T2` ∥ `T4`) → (`T3`, `T5`, `T7`) → `T6`.

## Constraints & gates

- **Fidelity (D7 / [23](23-graded-truth-layer.md)) is graded, never faked.** The
  Scrubber stays absent live (no `regstate` producer); T5's live weave is labelled
  perturbing + torn. A live `log`/`trace`/`watch` session carries no `df_step`, so
  Loom/Slice correctly stay absent for it — `view_presence` already decides this.
- **No new cost floor on a static UI.** All rebuilds sit behind the growth gate;
  a paused or idle session re-decodes nothing.
- **The render-only viewer keeps D9.** This adds no engine dependency — the live
  host is still the `asmspy --serve` subprocess, and the builders are engine-free.
- **Deep-link / persistence forward-compat.** The live tab is never written to the
  workspace store; a restored workspace is exactly the file tabs.

## Out of scope

- **A live register producer.** The Scrubber's `regstate` ring has no serve
  equivalent; building a live per-step register capture is its own brief —
  [26-live-regstate-producer.md](26-live-regstate-producer.md) (the serve
  `--dataflow` engine already `PTRACE_GETREGS` every step, so it is a capture-and-
  serialize job, not new machinery). This brief leaves the Scrubber faithfully
  replay/emulator-only until 26 lands.
- **Incremental (non-O(n)) builders.** The rebuild-on-growth matches the Observer
  deck's existing profile; streaming/incremental `decode_streams` /
  `build_step_index` is a later optimization if a hot capture demands it.
- **Diff / ABI x-ray across a live leg.** These need two recordings; whether the
  live tab may serve as a `B` leg is a follow-on (T4 leaves it attachable-eligible
  but does not add the live-vs-file diff affordance).
- **The doc reconciliation.** Docs [04](04-replay-views.md)/[05](05-loom-day-one.md)
  still say exact dataflow has "no live producer path"; that copy is corrected
  alongside this brief, not by it.
