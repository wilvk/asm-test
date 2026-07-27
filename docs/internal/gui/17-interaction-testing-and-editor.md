# Bigger bets: imgui_test_engine + goossens ImGuiColorTextEdit — implementation

> **Sources.** Actioned from [11-imgui-addons.md](11-imgui-addons.md): ranked
> recommendations **#7** (imgui_test_engine) and **#8** (ImGuiColorTextEdit,
> goossens rewrite, + TextDiff), the "Bigger bets:" row of **Sequencing**.
> Written 2026-07-26 against HEAD `27cd43e`. This doc wins over doc 11 on
> disagreement; the CODE wins over this doc — re-verify file:line before editing
> (doc 11 verified at `4f11065`).
>
> Read [\_conventions.md](../implementations/_conventions.md) first; D1–D11 live
> in this directory's [README](README.md). **Prerequisites:
> [12-addon-supply-chain.md](12-addon-supply-chain.md)** (fetch + compile-gate +
> the licensing exception in the admission rule). The two tasks are **independent
> of each other**; both read better after [13-foundation-moves.md](13-foundation-moves.md)
> F3 fonts (a code editor on a bitmap font looks wrong), but neither strictly
> requires it.

## Why this work exists

Two larger, higher-leverage adoptions:

- **imgui_test_engine** tests the *interaction layer* the golden-text strategy
  cannot reach — door flows, the syscalls reveal-all confirm, patch-bay
  queue/swap, scrubber dragging, and the **12-entry advertised keymap of which
  only `[`/`]` is wired**. Doc 11's sharpest point: *writing engine tests for the
  bindings forces implementing them.* It becomes a foundation of its own — every
  future view lands with an interaction test.
- **ImGuiColorTextEdit (goossens)** + **TextDiff** replaces the Author door's
  64 KB `InputTextMultiline` hack with a real editor (undo/redo, find/replace,
  and error markers anchored to the assembler's loud-drop line — verbatim
  machine reasons where they happened, the honesty ethos), gives `disasm` an
  address/bytes gutter + current-PC highlight, and upgrades `diff_view` with
  side-by-side mode.

## What already exists (verified 2026-07-26)

- **The test-engine hooks are already compiled in** — the
  `#ifdef IMGUI_ENABLE_TEST_ENGINE` hooks live in the vendored `imgui.cpp` /
  `imgui_widgets.cpp`; enabling the engine is a `-D` flag (an imconfig change),
  **no tarball/digest change**. A tag-matched **`v1.91.9`** of the engine exists.
- **The untested interaction surfaces** — the doors
  (`desktop/src/ui/{learn,author,inspect}_door.cpp`), the syscalls two-step
  reveal-all confirm (`desktop/src/views/syscalls.cpp`, ~lines 65–72), the
  patch-bay (`desktop/src/live/budget.*`), scrubber dragging
  (`views/scrubber_draw.cpp`), and the advertised keymap
  (`desktop/src/nav.cpp`, ~lines 282–296 list 12 bindings; only `[`/`]` are wired
  — `scrubber_draw.cpp`, `abixray_draw.cpp`, `loom/fabric_imgui.cpp`). The
  binding table is exposed as `dt_nav_bindings()` (`desktop/src/nav.h:119`), and
  the help overlay (`draw_bindings_help`, `views_draw.h`) is already fed from it
  — so the help and the keymap cannot drift.
- **The "manual smoke" admissions** — docs 04/05 state end-to-end flows are
  "manual smoke, noted in the PR" (`04-replay-views.md:399,491`,
  `05-loom-day-one.md:237`). This task closes exactly that gap.
- **The Author editor hack** — `desktop/src/ui/author_door.cpp:129` is
  `InputTextMultiline` over `s.source.data()/capacity()` (see `ui/doors.h:85`),
  a fixed 64 KB buffer with **silent truncation risk**.
- **The disasm + diff views** — `desktop/src/views/disasm.cpp` (no gutter/PC
  highlight today), `desktop/src/views/diff_view.cpp` (summary rows, no
  side-by-side text).
- **Licensing** — imgui_test_engine is **not MIT** (Dear ImGui Test Engine
  License v1.04); doc 12's admission rule already carries the one exception for
  it (test-builds-only, fetch-at-build, never vendored).

## Tasks

### T1 — imgui_test_engine: test the interaction layer + enforce the keymap  (L, depends on: 12)

> **Harness + keymap enforcement done 2026-07-27 — green (`make desktop-ui-test`,
> 9/9; docker-desktop clean-room).** The engine is vendored at tag **v1.91.9**
> (tarball sha `da67f93b…`, matched to the imgui 1.91.9b-docking pin — bump the
> two together), TEST-LANE ONLY: `scripts/fetch-imgui-test-engine.sh` (a
> `fetch-addon.sh` tarball wrapper), one digest row, license captured with the
> test-lane marker; it compiles into `desktop_ui_test` ALONE (a `uitest` make
> tree = the shell-test object set + `-DIMGUI_ENABLE_TEST_ENGINE`; the shipped
> binaries never see it, so the repo stays MIT). Runs headless on the null
> backend, writes JUnit XML (`build/desktop-ui-test-results.xml`); `desktop-test`
> stays MIT-only and only *notes* the separate lane. **The keymap payoff (step
> 5) is delivered**: the ten unwired bindings are implemented as a central
> `handle_keymap` in `draw_shell` (pure ShellState moves — `1`/`2`/`3`/`4`,
> `j`/`k`+arrows, `PgDn`/`PgUp`, `Enter`, `b`/`f`/`c`, `d`, `x`, `n`/`p`, `y`,
> `Ctrl+G`), each with a passing engine test (`desktop/test/test_ui.cpp`) that
> presses the key and asserts the state — the model, not pixels (D4). View
> switch + swap go through the tab bar's `want_view`/`want_open_tab` SetSelected
> (a keypress cannot select an ImGui tab directly). `[`/`]` stay view-local
> (scrubber/slice/abixray). Two clean-tree ordering hazards the docker build
> caught and fixed: the engine sources are a grouped (`&:`) fetch output, and
> `test_ui.o` carries the fetch as an order-only prereq.
>
> **Flow tests (step 4): the sharpest one landed 2026-07-27** — the syscall
> **reveal-all two-step confirm** (`flow/syscall_reveal_all_two_step` in
> `test_ui.cpp`, driving `draw_obs_syscalls` through real `ItemClick`s): the
> first click only ARMS, the confirm performs it — a destructive-feeling
> unredact gated behind two steps (D7), exactly the multi-step interaction the
> golden tests cannot reach. `make desktop-ui-test` is now 10/10 (8 keymap + the
> harness self-check + this flow).
> **Follow-on (does not block the payoff)**: the other listed flows are low
> unique value for an engine click-test — the door-open buttons are trivial
> `bool` sets, and the patch-bay swap decision (`inspect_arm_swap`/
> `inspect_confirm_swap`) and the scrubber step (`dt_scrubber_prev`/`next` plus
> the `[`/`]` keys) are ALREADY pure-unit-tested in `test_inspect` /
> `test_scrubber`, so an engine test would only re-cover them. InvisibleButton
> hit-targets for the hand-rolled Loom/slice draw-list views (step 6) also
> remain a follow-on. The keymap enforcement — the doc's stated "real payoff" —
> is done.

**Goal.** Stand up the first-party test engine in a **test-only, fetch-at-build,
never-vendored** posture, write interaction tests for the untested flows, and —
the real payoff — **write keymap tests that force the 12 advertised bindings to
actually be implemented.**

**Steps.**
1. **Fetch-at-build, test-only (license posture).** The engine's license (Test
   Engine License v1.04) is not MIT; doc 12's admission rule permits it **only**
   fetch-at-build for test builds, never vendored, so the repo stays 100% MIT.
   Fetch it at the tag-matched **`v1.91.9`** via a `fetch-addon.sh` wrapper used
   *only by the test target* (mirror how Pin/SDE are fetched for test lanes and
   marked "never bundled" in [licenses/README.md](../../../licenses/README.md)).
   Capture its license text with the test-lane-only marker. It compiles into the
   **test binary only** — never `desktop` or `desktop-render`.
2. **Enable the hooks.** Add `-DIMGUI_ENABLE_TEST_ENGINE` to the test binary's
   flags only (the `IMGUI_ENABLE_TEST_ENGINE` `#ifdef`s are already present in
   the vendored imgui — no tarball change). Wire the engine's context alongside
   the null-backend `ImGuiContext` the desktop tests already use.
3. **Headless CI fit.** Use the engine's **null-backend mode + JUnit XML
   exporter** (present at the tag) so it slots into the existing `desktop-test`
   lane and CI. Add a `desktop-ui-test` target (or fold into `desktop-test`) with
   a `make help` line (D3).
4. **Write the interaction tests** for the flows the golden strategy can't reach:
   Learn/Author/Inspect door flows, the syscalls reveal-all two-step confirm
   (`syscalls.cpp:65–72`), patch-bay queue/swap, scrubber dragging.
5. **The keymap enforcement (the point).** For each of the 12 bindings in
   `dt_nav_bindings()` (`nav.cpp:282–296`), write an engine test that exercises
   the binding. The `[`/`]` tests pass today; the other ten **fail until the
   binding is implemented** — implementing them to make the tests pass is the
   deliverable. Keep the help overlay fed from `dt_nav_bindings()` so it stays in
   sync.
6. **Hand-rolled draw-list views are invisible to item-path addressing** (Loom,
   slice graph) until rows gain `InvisibleButton`s — drive those **positionally
   or by screenshot**, or add `InvisibleButton` hit-targets where it's cheap
   (doc 11 risk note).
7. **Bump discipline**: the engine tag moves in the **same commit** as any imgui
   repin (doc 13 F1/F4) — note this in the repin-gate comment (doc 12 T3).

**Tests.** The tests *are* the deliverable. Green criteria: all 12 keymap
bindings have a passing engine test (which means all 12 are implemented); the
door flows, syscalls confirm, patch-bay, and scrubber drag have passing engine
tests; JUnit XML is emitted for CI. Runs headless in `docker-desktop`.

**Docs.** CHANGELOG `Added`: interaction-layer tests; the 10 previously-unwired
key bindings now implemented. `desktop/README.md`: how to run `desktop-ui-test`.
`licenses/README.md` row (test-lane-only, never bundled). Update docs 04/05's
"manual smoke" admissions to point at the new engine tests.

**Done when.** the test engine runs headless in the test binary only (never in a
shipped binary), emits JUnit XML; every advertised keybinding has a passing test
(so every binding works); the door/syscalls/patch-bay/scrubber flows are covered;
the repo stays 100% MIT (engine fetched-at-build, marked test-lane-only); the
tag-bump-with-repin rule is documented.

### T2 — goossens ImGuiColorTextEdit + TextDiff: real editor, disasm gutter, side-by-side diff  (M, depends on: 12)

> **Implemented 2026-07-27 (Author editor) — green (54 host suites + clean-tree
> checked).** goossens ImGuiColorTextEdit vendored (master `f67e5bc`, MIT +
> bundled dtl BSD-3) via `scripts/fetch-ictedit.sh`, which **applies the
> compile-verified 2-line guard** (`#if IMGUI_VERSION_NUM >= 19200` around the
> `PlatformImeData.WantTextInput`/`.ViewportId` SDL3-IME assignments) — doc-11's
> mitigation, reproduced: unguarded fails on our 1.91.9 pin, guarded compiles
> clean (and `TextDiff.cpp` unmodified). In the compile-probe (`imgui_internal.h`).
> `ui/author_door.cpp` now uses a `TextEditor` (`SetText`/`Render`/`GetText`)
> instead of `InputTextMultiline` over `s.source.data()/capacity()` — **the fixed
> 64 KB buffer that silently truncated is gone** (undo/redo + find/replace come
> free). `test_ictedit` pins the no-truncation win directly (an ~80 KB source
> round-trips; read-only toggles). `TextEditor.o` rides `author_door.o`'s link
> sites (app/viewer + shell tests). **Follow-ons** (noted, not blocking): an
> x86/ARM asm **language definition** (syntax highlight) and **error markers
> anchored to the loud-drop line** — the v1 `asm_result` carries no line, so the
> verbatim refusal stays a banner; and `TextDiff` for `diff_view` side-by-side
> (its `dtl.h` is fetched, not yet compiled).

**Goal.** Replace the Author door's `InputTextMultiline` hack with a real code
editor, give `disasm` an address/bytes gutter + current-PC highlight, and upgrade
`diff_view` with side-by-side text — with assembler errors anchored where they
happened.

**Steps.**
1. Vendor the **goossens ImGuiColorTextEdit rewrite** + **TextDiff** (MIT +
   bundled dtl (BSD-3), 2–5 files, no external deps, no regex) at a **commit
   sha** via doc 12's `fetch-addon.sh`. It includes `imgui_internal.h` →
   compile-probe (doc 12 T3). One digest row, license captures for ICTE (MIT) and
   dtl (BSD-3).
2. **Apply the compile-verified 2-line version guard (doc 11 correction).**
   "No 1.92-only APIs at master" is **false** — every commit since 2025-06-29
   requires ImGui 1.92
   (`ImGuiPlatformImeData::WantTextInput`/`ViewportId`, one SDL3-specific IME
   block). Doc 11's mitigation is compile-verified: **wrap those 2 lines in
   `#if IMGUI_VERSION_NUM >= 19200`** and master's `TextEditor.cpp` compiles
   clean on 1.91.9; `TextDiff.cpp` compiles unmodified. Ship as
   pin-a-commit + the 2-line local patch (or drop the patch after doc 13 F4's
   1.92 bump). Upstream announces a compat-breaking "future" branch — **hard-pin
   the commit and re-audit on every bump.**
3. **Author door** (`ui/author_door.cpp:129`): replace the
   `InputTextMultiline`-over-`s.source.data()/capacity()` 64 KB hack (`doors.h:85`
   — the silent-truncation risk) with the editor: undo/redo, find/replace, and
   **error markers anchored to the assembler's loud-drop line** (the verbatim
   machine reason, anchored where it happened — the honesty ethos). Write a
   bounded, testable **x86/ARM asm language def** via the editor's
   state-transition colorizer API.
4. **disasm** (`views/disasm.cpp`): read-only mode + line decorators give an
   address/bytes **gutter** and a **current-PC highlight**.
5. **diff_view** (`views/diff_view.cpp`): `TextDiff::SetSideBySideMode` upgrades
   the summary into a real side-by-side text diff.

**Tests.** `test_author_vm` / a new editor test: assert the editor round-trips
source with no truncation (the 64 KB hack's failure mode is gone — feed >64 KB);
assert an assembler loud-drop produces an error marker on the correct line
(verbatim reason). `test_diff_view`: assert side-by-side mode aligns the two
halves. `test_obs_disasm`: assert the gutter shows address/bytes and the PC line
is highlighted. Null backend; the 2-line guard means it compiles on 1.91.9.

**Docs.** CHANGELOG `Added`: real Author editor (undo/redo, find/replace, error
markers); disasm gutter + PC highlight; side-by-side diff. `licenses/README.md`
rows (ICTE, dtl; bundled). `desktop/README.md` note on the asm language def.

**Done when.** the Author door uses the editor (no 64 KB truncation; feeding
>64 KB is safe); assembler errors mark their exact line with the verbatim reason;
disasm shows an address/bytes gutter + PC highlight; diff has a working
side-by-side mode; the 2-line `#if IMGUI_VERSION_NUM >= 19200` guard is in place
and the commit is hard-pinned + in the compile-probe.

## Task order & parallelism

Independent of each other; either can go first, different developers. Both are
larger than the doc-14 quick wins — **T1 (test engine) is the "large" effort in
doc 11's table** and pays off across every future view, so prioritise it if you
can only do one. Both read better after doc 13 F3 fonts but neither blocks on it.
T1 must bump its engine tag in lockstep with any imgui repin (doc 13 F1/F4).

## Constraints & gates

- **The repo stays 100% MIT.** imgui_test_engine (T1) is the *only* non-MIT
  admission, and only because it is fetched-at-build for test builds and never
  vendored/shipped (doc 12 admission rule, exception 1). It must never compile
  into `desktop` or `desktop-render`.
- **Honesty (D7)** — the editor's error markers carry the assembler's **verbatim
  machine reason** on the exact loud-drop line; the Author door no longer
  silently truncates at 64 KB.
- **The 2-line guard is mandatory on 1.91.9** — ICTE master is 1.92-only without
  it (doc 11 correction). Do not adopt without the guard (or without doc 13 F4's
  bump).
- **Bump discipline** — the test-engine tag moves with the imgui pin; ICTE's
  commit is hard-pinned and re-audited on every bump (upstream has a
  compat-breaking "future" branch).

## Out of scope

- Rebindable keys / a hotkey editor — ImHotKey is verified broken on 1.91.9
  (doc 11 skip); worth revisiting only after T1 makes the advertised keymap
  actually work.
- Zep / santaclose ICTE — doc 11 skips (dominated by goossens; Boost.Regex /
  own object model).
- The plotting/graph/feedback/filtering addons (docs 15–16).
