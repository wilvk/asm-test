# Quick wins: the diff bug, shared palette, progress bars, memory editor, zoom slider, text-select, file dialog — implementation

> **Sources.** Actioned from [11-imgui-addons.md](11-imgui-addons.md): the
> **Quick wins** section (7 items) plus ranked recommendations **#4**
> (imgui_memory_editor), **#5** (ImGuiFileDialog), **#6** (ImGuiTextSelect), and
> **#9** (ImZoomSlider). Written 2026-07-26 against HEAD `27cd43e`. This doc wins
> over doc 11 on disagreement; the CODE wins over this doc — re-verify any
> file:line before editing (doc 11 was verified at `4f11065`).
>
> Read [\_conventions.md](../implementations/_conventions.md) first; D1–D11 live
> in this directory's [README](README.md). **Prerequisite:
> [12-addon-supply-chain.md](12-addon-supply-chain.md)** (T4–T7 each vendor an
> addon through its `fetch-addon.sh` scaffolding). T1–T3 need **no** addon and
> can start the moment doc 12's D2 amendment (T1) lands.

## Why this work exists

These are the "no tarball changes… parallel, any order" items from doc 11 — each
small, each independently landable, together closing the bluntest survey
findings: no copy-out anywhere, no client-side interaction on bytes, bare
`InputText` file paths, a warn colour drifted across five TUs, and one live bug
the survey caught. They are ordered here **cheapest/no-dependency first**
(T1–T3 need no addon) so a developer can pick up any single T and ship it.

## What already exists (verified 2026-07-26)

- **The diff "go" bug** — `desktop/src/views/diff_view_draw.cpp` (in
  `draw_diff_view`) calls `ImGui::SetClipboardText(r.link.c_str())` on the "go"
  button instead of navigating (around line 41). Every *other* clicked link in
  the shell routes through `dt_nav_go(s.nav, …)` (`desktop/src/ui/shell.cpp:358,
  425, 603`). `draw_diff_view(const dt_diff_view &v)`
  (`desktop/src/views/views_draw.h`) currently takes no router — the fix threads
  the click back to the shell (see T1).
- **The router** — `dt_nav_go(dt_nav_table&, const dt_link&)` and
  `dt_nav_parse` (`desktop/src/nav.h:80,111`); `dt_diff_row.link` is already a
  formatted deep link string (`desktop/src/views/diff_view.h`).
- **The drifted warn colour** — declared independently (with drifted values) in
  five TUs: `views/canvas_draw`, `views/observer_draw`, `scene3d/hud`,
  `loom/fabric_imgui`, `views/completeness` (doc 11 survey). `draw_banner`
  (`views/views_draw.h`) already centralises the *placard*, not the colour.
- **The byte panes** — `desktop/src/views/region.cpp` emits bytes only as
  pre-formatted hex strings (zero byte-level interaction); the client-side
  version resolver `obs_disasm_bytes_at` (greatest `when` ≤ t) lives in
  `desktop/src/views/disasm.cpp` (~lines 112–138) and is exactly the `ReadFn` a
  memory editor needs — no flat buffer.
- **The Loom pan gap** — `desktop/src/loom/fabric_plan.h:64–67` carries
  `step0`/`lane0` pan fields no code writes; only `steps_per_px` is mutated.
- **The bare file-path fields** — `draw_open_dialog` at
  `desktop/src/ui/shell.cpp:309` and `draw_save_capture` at
  `desktop/src/ui/inspect_door.cpp:262` are raw `InputText` paths.
- **No selection/copy** — the only clipboard write in the app is the diff link
  copy (which is itself the T1 bug).

## Tasks

### T1 — Fix the diff-view "go" button (route, don't copy)  (S, depends on: 04's router)

> **Landing 2026-07-26 (concurrent).** A parallel change has applied this fix in
> the working tree: `draw_diff_view` now takes a `go` handler and the "go" button
> routes through it instead of `SetClipboardText` (see
> `desktop/src/views/diff_view_draw.cpp` — the `if (ImGui::SmallButton("go") &&
> go)` form — and the signature change in `views_draw.h`). **Verify it is on
> `main`**; if present, T1 is done — confirm the test below exists, else add it.

**Goal.** The "go" button in the diff summary navigates through the deep-link
router like every other clicked link, instead of silently copying the link to
the clipboard. Doc 11 caught this in passing; it is a correctness fix, not an
addon.

**Steps.**
1. In `draw_diff_view` (`desktop/src/views/diff_view_draw.cpp`, ~line 41),
   replace `ImGui::SetClipboardText(r.link.c_str())` with the routed path.
   Because `draw_diff_view` deliberately takes no shell/router (the draw/build
   split, `views_draw.h`), thread the click back the way `draw_scrubber` returns
   its playhead: have `draw_diff_view` **return** the clicked row's link (e.g.
   `std::optional<dt_link>` parsed from `r.link` via `dt_nav_parse`, or the raw
   `std::string` for the shell to parse), and have the shell caller do
   `if (auto l = draw_diff_view(v); l && !dt_nav_go(s.nav, *l)) s.status =
   s.nav.last_error;` — the exact shape already at `shell.cpp:358`.
2. Update the signature in `desktop/src/views/views_draw.h` and the shell call
   site. Keep `draw_diff_view` free of the shell type (the header comment for
   `draw_scrubber`/`nav.h:87` explains why: `nav.o` must link into every view
   test binary).
3. Preserve the tooltip (`SetTooltip("%s", r.link)`) — showing the link on hover
   is fine; *only* the click behaviour changes.

**Tests.** `desktop/test/test_diff_view.cpp` (exists): assert a navigable row
yields the parsed `dt_link` (not a clipboard write) and a non-navigable row
yields none. If a `test_shell` path drives diff navigation, assert the router is
invoked and `last_error` surfaces on a bad link.

**Docs.** CHANGELOG `Fixed`: "desktop diff view: the row 'go' button now
navigates via the deep-link router instead of copying the link to the
clipboard." No user doc change.

**Done when.** clicking "go" navigates (router called; `last_error` shown on a
dead link); the clipboard is no longer written by this button; the diff-view
test pins the routed behaviour.

### T2 — Shared theme/palette header  (S, depends on: none)

> **Landing 2026-07-26 (concurrent).** A parallel change has added
> `desktop/src/ui/theme.h` and begun consolidating the drifted warn colour across
> `canvas_draw`, `observer_draw`, `hud`, `fabric_imgui`, and `completeness` (all
> five appear modified in the working tree). **Verify it is on `main`**; if
> present, T2 is largely done — confirm no raw warn literal survives outside
> `theme.*` (the grep guard below) and that the accessor test exists.

**Goal.** One in-tree palette header so the warn colour (and any other shared UI
colour) is declared **once**, ending the five-TU drift. No addon.

**Steps.**
1. Add `desktop/src/ui/theme.h` (+ `theme.cpp` if a `.cpp` is warranted)
   exposing the shared colours as named constants/accessors, e.g.
   `asmdesk::col_warn()`, `col_statistical()`, `col_provenance()`. Pick the
   canonical warn value deliberately (the five current values drifted — choose
   one, document why; `draw_banner` is the reference consumer).
2. Replace the five independent warn-colour declarations in
   `views/canvas_draw`, `views/observer_draw`, `scene3d/hud`,
   `loom/fabric_imgui`, `views/completeness` with the shared accessor. Grep for
   the drifted literals to find every site (`grep -rn "IM_COL32\|ImVec4" | grep
   -i warn` and the raw colour literals near the banner code).
3. Optional (doc 11): author a classroom light/dark pair. Keep it minimal — the
   task's core is *consolidation*, not a restyle. If you add themes, do it behind
   one `theme_apply(Theme)` entry point.

**Tests.** `desktop/test/test_theme.cpp` (new, trivial): the accessor returns a
stable value; optionally a golden that the banner colour equals `col_warn()`. The
stronger guard is a grep-based check (no raw warn literal survives outside
`theme.*`) — add it to the format/lint step if the repo has one, else note it in
the doc.

**Docs.** CHANGELOG `Changed`: "desktop GUI: shared theme/palette header;
warn-colour drift across five views consolidated." `desktop/README.md` one line.

**Done when.** exactly one declaration of the warn colour exists (in `theme.*`);
the five TUs consume it; the banner still renders warn-coloured, non-collapsible
placards (D7 unchanged).

### T3 — Progress bars for loads and live sessions  (S, depends on: none)

**Goal.** Honest progress feedback using **built-in `ImGui::ProgressBar`** (in
1.91.9 already; zero fetch): **determinate** for file loads that have an `end`
footer (a real total exists), **indeterminate** for live/torn sessions where no
honest total exists.

**Steps.**
1. Determinate: on `.asmtrace` file load, drive `ProgressBar(fraction)` from
   events-parsed / total-from-`end`-footer. Where the recording is **torn** (no
   `end` footer — the truncation case D7 makes first-class), switch to
   `ProgressBar(-1.0f * ImGui::GetTime(), …)` indeterminate mode — because a
   total would be a lie.
2. Live sessions (`desktop/src/live/session.*`): indeterminate while attached
   with no bound; determinate only if the session carries an honest budget/total
   (patch-bay budgets, `live/budget.*`).
3. Optionally one subtle spinner style to distinguish "attached-but-no-events
   stall" from a dead UI (doc 11). Keep it built-in / hand-drawn — do **not**
   pull imspinner for this (a quick win must stay fetch-free).

**Tests.** `test_shell` / the relevant view test: assert the load path chooses
determinate for a footered fixture and indeterminate for the torn fixture (the
model-level choice, not the pixels — expose a tiny `progress_mode(recording)`
helper so the *decision* is testable, per D4/the honesty-is-tested rule).

**Docs.** CHANGELOG `Added`: progress feedback for loads/live sessions. One
`desktop/README.md` line noting the determinate-vs-indeterminate honesty rule.

**Done when.** a footered file load shows a determinate bar; a torn file and an
unbounded live session show indeterminate (never a fabricated percentage); the
mode decision is unit-tested.

### T4 — imgui_memory_editor (imgui_club) over the codeimage bytes  (S, depends on: 12; 08)

**Goal.** Byte-level interaction on the codeimage/region bytes — the cheapest
strong upgrade (doc 11 #4, confirmed outright). `region.cpp` today shows bytes
as static hex; this makes them navigable and diffable.

**Steps.**
1. Vendor the single 836-line header `imgui_memory_editor.h` from ocornut's
   `imgui_club` (MIT, **public API only — no `imgui_internal.h`**) via doc 12's
   `fetch-addon.sh`: one commit-sha digest row (`imgui_club` publishes no tags),
   one `licenses/imgui_club-LICENSE.txt`. Because it has no internal header, it
   does **not** go in the compile-probe (doc 12 T3) — but pin the commit sha and
   record it.
2. Wire `MemoryEditor::ReadFn` directly onto `obs_disasm_bytes_at`
   (`desktop/src/views/disasm.cpp:~112–138`: greatest `when` ≤ t) — the null-mem
   + `base_display_addr` form (compile-verified in doc 11) means **no flat
   buffer**; the editor asks for byte `addr` and you resolve it as-of-trace-time.
3. `ReadOnly` in replay (honours "never re-read live memory"); `BgColorFn` paints
   bytes that differ between codeimage versions (JIT churn), dirtied bytes, and
   slice membership; `GotoAddrAndHighlight` wires disasm/syscall-arg clicks to
   exact bytes. Foreground colour is an open upstream request (#59) — background
   tint only; do not fake foreground.
4. Host it in the region/codeimage pane (`views/region.cpp` +
   `views/region_draw` if split; else the codeimage pane 08 added).

**Tests.** `desktop/test/test_obs_region.cpp` (exists): assert `ReadFn` returns
the as-of-trace-time byte for a version boundary (the byte differs across two
codeimage versions at the same address); assert `ReadOnly` in a replay recording;
assert a `GotoAddr` from a disasm click lands on the right offset. Null-backend:
the editor draws draw-list only.

**Docs.** CHANGELOG `Added`: interactive byte view over codeimage versions.
`licenses/README.md` row (bundled). `desktop/README.md` note.

**Done when.** the region/codeimage pane is an interactive hex editor sourced
from `obs_disasm_bytes_at` (no flat buffer); version-differing/dirtied/slice
bytes are tinted; replay is read-only; goto-address works; the resolver test
pins the as-of-trace-time read.

### T5 — ImZoomSlider: the Loom/timeline pan+zoom control  (S, depends on: 12; 05, 04)

> **Implemented 2026-07-27 (Loom pan) — green.** ImZoomSlider vendored via
> `scripts/fetch-imzoomslider.sh` (commit `dc25afb9`, MIT; digest pinned; license
> captured) and wired into the Loom (`loom/fabric_imgui.cpp`) as a windowed
> pan+zoom over the whole step range — dragging the middle **writes `step0`** (the
> field `fabric_plan.h` always carried but nothing ever set), an edge writes
> `steps_per_px`. The window↔camera math is a pure, tested pair
> (`loom_view_step_window` / `loom_view_set_step_window` in `fabric_plan.cpp`;
> `test_loom_plan` covers pan, zoom, round-trip, and the degenerate clamps). This
> also **established the reusable addon Makefile pattern** (12 T2/T3):
> `DESKTOP_ADDON_INCLUDES` / `ADDON_PROBE_FLAGS` / `ADDON_PROBE_DEPS` accumulators
> + a fetch rule + an order-only prereq on the user object + a digest row + a
> license row + the compile-probe entry. The include recipe
> (`IMGUI_DEFINE_MATH_OPERATORS`, `imgui.h`, `imgui_internal.h`, `ImZoomSlider.h`
> + `PushID`) is verified on the docking pin. **Remaining T5 scope**: the timeline
> windowing + the hotedges/diff overview strip reuse the same helper — a small
> follow-on, not yet wired.

**Goal.** Give the Loom the pan interaction its model already carries but nothing
mutates (`fabric_plan.h:64–67`), and give the timeline a windowed view into a
long trace's step range. One 245-line self-contained header (doc 11 #9).

**Steps.**
1. Vendor **only** `src/ImZoomSlider.h` from the ImGuizmo repo (MIT) via
   `fetch-addon.sh` at a commit sha — no gizmo code built; one digest row, one
   `licenses/ImGuizmo-LICENSE.txt`.
2. **Include recipe is mandatory** (doc 11 correction — it is *not*
   public-API-only; it uses `ImRect`/`ImMin`/`ImMax` and math operators):
   `#define IMGUI_DEFINE_MATH_OPERATORS`, then `imgui.h`, then
   `imgui_internal.h`, then `ImZoomSlider.h`. Because it includes
   `imgui_internal.h`, **add it to `addon_compile_probe.cpp`** (doc 12 T3).
3. **Wrap each instance in `PushID`** — the fixed internal `"ImZoomSlider"` label
   collides across the three proposed sites (Loom, timeline, hotedges/diff
   overview strip). The function-local static drag state is
   single-`ImGuiContext`-only, which is fine here.
4. Wire it: Loom pan writes `fabric_plan.h`'s `step0`/`lane0` (the dead fields);
   timeline gets a step-range window; optionally an overview strip for
   hotedges/diff.

**Tests.** `desktop/test/test_loom_plan.cpp` / `test_timeline.cpp`: assert the
slider mutates `step0`/`lane0` (Loom) and the timeline window; assert two
instances on one frame don't collide (the `PushID` guard). Null-backend: the
slider is draw-list interaction.

**Docs.** CHANGELOG `Added`: Loom/timeline pan+zoom. `licenses/README.md` row.

**Done when.** the Loom pans (its `step0`/`lane0` are finally written); the
timeline windows a long trace; multiple instances coexist via `PushID`; the
internal-header include recipe is in the compile-probe.

### T6 — ImGuiTextSelect (v1.1.6 + utfcpp): copy-out everywhere  (S, depends on: 12)

**Goal.** Close the bluntest survey finding — no selection/copy anywhere — with a
callback-driven text-selection overlay for every line-oriented pane
(observer syscalls, disasm listings, capability machine-reasons, diff halves,
completeness skip-reason cells).

**Steps.**
1. **Pin v1.1.6 (2025-06-10), NOT the latest.** Doc 11's refuted-and-corrected
   finding: v1.2.0+ dropped pre-1.92 support — v1.3.2 calls
   `ImGui::GetFontBaked()` / `ImFont::CalcWordWrapPosition()` which **do not
   exist in 1.91.9 and do not compile.** v1.1.6 needs its **utfcpp** dependency
   (BSL-1.0) — so **two digest rows** (`textselect`, `utfcpp`) and two license
   captures (doc 12 T2's two-file case). Includes `imgui_internal.h` → add to
   the compile-probe (doc 12 T3).
2. Overlay it on line-oriented panes via `getLineAtIdx`/`getNumLines` callbacks
   so **view-models are untouched** (D4). Host panes need
   `ImGuiWindowFlags_NoMove` (selection drag vs window drag).
3. Selection math assumes uniform-height lines: multi-column table cells map to
   **whole rows** — document this per pane. **Exclude the Loom prim canvas** by
   design (non-line-oriented).
4. Note in the code that the "current line" affordance arrives only after the
   1.92 bump (doc 13 F4) — do not reach for the v1.3.x API.

**Tests.** A null-backend test that the callback adapter returns the right line
count/text for a syscalls fixture and that a selection range maps to the expected
copied string. Assert `NoMove` is set on host panes.

**Docs.** CHANGELOG `Added`: text selection + copy across line-oriented views.
Two `licenses/README.md` rows (ImGuiTextSelect, utfcpp; both bundled).

**Done when.** a user can select and copy addresses/payloads/machine-reasons out
of syscalls, disasm, capability, diff, and completeness panes; the pin is v1.1.6
+ utfcpp (compiles on 1.91.9); host panes use `NoMove`; the Loom canvas is
excluded; both addons are in the compile-probe/digests/licenses.

### T7 — ImGuiFileDialog (v0.6.8): pure-ImGui open/save  (S, depends on: 12)

**Goal.** Replace the raw `InputText` path fields with a real open/save dialog
that behaves identically on Linux, macOS, and inside `docker-desktop` — **no
zenity/osascript runtime deps** (which is what keeps the lane testable; a lane
that can only self-skip is not a test).

**Steps.**
1. Pin **v0.6.8 (2025-10-03)** — doc 11 correction: it works on 1.91.9 via an
   explicit `#if IMGUI_VERSION_NUM < 19201` guard in `ImGuiFileDialog.cpp:4251`,
   **not** by contemporaneity. Do **not** track master (targets 1.92.3 /
   `ImTextureRef`). It includes `imgui_internal.h` → **couple its pin to the
   imgui pin** and add it to the compile-probe (doc 12 T3). Vendor via
   `fetch-addon.sh` (tarball tag shape); MIT; one license capture.
2. Replace `draw_open_dialog` (`desktop/src/ui/shell.cpp:309` — the deferral is
   verbatim in `03-desktop-shell.md:422`) and `draw_save_capture`
   (`desktop/src/ui/inspect_door.cpp:262`) with the dialog.
3. Use the per-extension **side pane** to preview an `.asmtrace` before opening:
   arch, event count, torn/`end`-footer status — provenance-first, reusing the
   existing recording header reader (03/`doc/recording.*`).

**Tests.** `test_shell` / `test_inspect`: assert the open path yields a selected
file and the save path a target path (drive the dialog's model, not the pixels);
assert the preview pane reports torn-vs-footered correctly for the golden torn
fixture. Must pass headless in `docker-desktop` (no native dialog).

**Docs.** CHANGELOG `Changed`: pure-ImGui open/save with `.asmtrace` preview.
`licenses/README.md` row (bundled). `desktop/README.md` note that the dialog is
pure-ImGui (no runtime dialog deps).

**Done when.** open and save go through ImGuiFileDialog on all three lanes with
no zenity/osascript; the side pane previews arch/count/torn-status; the pin is
v0.6.8 (1.91.9 guard), coupled to the imgui pin in the compile-probe.

## Task order & parallelism

All seven are independent and can be a different developer each. **T1–T3 need no
addon** — start them the moment doc 12 T1 (D2 amendment) lands, even before doc
12 T2/T3 exist. **T4–T7 need doc 12's fetch scaffolding (T2) and, for the
internal-header ones (T5/T6/T7), the compile-gate (T3).** No ordering among
T4–T7. None of the seven depend on doc 13's foundation moves, though T5/T6 *read
better* after F3 fonts (doc 13 T4) — not a blocker.

## Constraints & gates

- **View-models stay pure (D4).** Every addon here overlays draws via callbacks
  or read-fns; none becomes the place a rule is decided. The golden-text tests
  stay the source of truth.
- **Honesty (D7).** T3's indeterminate mode exists *because* a torn session has
  no honest total; T4's foreground colour is not faked; the diff fix (T1) makes
  a dead link show `last_error` rather than silently copying.
- **Testable lanes (CLAUDE.md).** T7's pure-ImGui dialog is chosen specifically
  so `docker-desktop` can test open/save without native dialog daemons.
- **Pins are the corrected ones** — TextSelect **v1.1.6 + utfcpp**, FileDialog
  **v0.6.8**, never their latest (both are 1.92-only at HEAD).

## Out of scope

- ImPlot / node-editor / notifications / filtering (docs 15–16) and the bigger
  bets (doc 17).
- The 1.92 bump that would unlock TextSelect's current-line affordance (doc 13
  F4/T5).
- Rebindable keys / a hotkey editor — doc 11 verified ImHotKey broken on 1.91.9;
  revisit only after the advertised keymap works (doc 17's test-engine task).
