# Live feedback & client-side filtering: ImGuiNotify + ImSearch — implementation

> **Sources.** Actioned from [11-imgui-addons.md](11-imgui-addons.md): ranked
> recommendations **#11** (ImGuiNotify) and **#12** (ImSearch). Written
> 2026-07-26 against HEAD `27cd43e`. **Both were NOT adversarially verified in
> doc 11's pass (marked ∅) — treat their compat claims as researcher-grade and
> compile-check against `build/imgui/imgui-1.91.9` with `mk/desktop.mk` flags at
> pin time before believing them.** This doc wins over doc 11 on disagreement;
> the CODE wins over this doc.
>
> Read [\_conventions.md](../implementations/_conventions.md) first; D1–D11 live
> in this directory's [README](README.md). **Prerequisites:
> [12-addon-supply-chain.md](12-addon-supply-chain.md)** (fetch + compile-gate).
> **ImGuiNotify hard-requires FontAwesome headers → it must land AFTER
> [13-foundation-moves.md](13-foundation-moves.md) F3 fonts** (doc 13 T4).

## Why this work exists

Two smaller navigation/feedback gaps the survey found:

- **Live-session events are silent when the user is in another view** — attach
  succeeded/refused, target exited, bounded-session EOF, SIGALRM teardown,
  `.asmtrace` save/torn warnings, long PT-replay decode completion. A toast
  (ImGuiNotify) surfaces them without stealing the pane. Toasts **supplement**
  the non-collapsible in-pane banners — refusals stay first-class where they
  happen; a toast never replaces a banner (honesty note, doc 11 #11).
- **Zero client-side filtering exists anywhere** — for 10k-routine trace graphs
  that is a navigation gap, not polish. ImSearch wraps existing
  `Selectable`/`TreeNode` draws via callbacks so pure view-models stay untouched.

Both are ∅-unverified in doc 11; each task's first step is a compile-check at the
pin.

## What already exists (verified 2026-07-26)

- **Live sessions** — `desktop/src/live/session.{h,cpp}`, the `--serve` host
  (07), the budget patch-bay (`live/budget.*`), the Inspect door
  (`ui/inspect_door.cpp`). These are the event sources ImGuiNotify surfaces:
  attach/refuse/exit/EOF/teardown/save-torn/PT-decode-done.
- **The cross-door open mechanism** — `open_request` in
  `desktop/src/ui/shell.cpp` (README integration notes: the shell already
  carries a cross-door open request the scrubber/x-ray/3D panes use). A toast's
  "Open recording" callback button composes with it.
- **Fonts/icons** — doc 13 F3 (T4) provides the merged icon font. ImGuiNotify's
  FontAwesome dependency is why it sequences **after** F3; the app's icon set is
  Codicons (doc 13 T4) — see the T1 note on reconciling FA vs Codicons.
- **The filterable lists** — `desktop/src/views/tree.cpp` (call tree, already has
  an *engine-side* filter form; ImSearch adds *client-side* narrowing on top),
  `desktop/src/views/syscalls.cpp` (syscall names), `desktop/src/views/disasm.
  cpp` (routine search), and the Learn-door walkthrough catalog
  (`ui/learn_door.cpp`).
- **No multi-viewports** — doc 13 keeps `ViewportsEnable` off; ImGuiNotify must
  be configured to render inside the main window (see T1).

## Tasks

### T1 — ImGuiNotify toasts for live-session events  (S, depends on: 12, 13 F3)

**Goal.** Non-modal toasts for the live-session events that are silent today,
each optionally carrying a callback button ("Open recording") that composes with
the shell's `open_request` mechanism. Toasts **supplement**, never replace, the
in-pane refusal banners.

**Steps.**
1. **Compile-check first (∅).** Vendor ImGuiNotify (MIT, canonical successor to
   the archived patrickcjk original) at a **Dev-branch commit sha** via doc 12's
   `fetch-addon.sh`; it includes `imgui_internal.h` → compile-probe (doc 12 T3).
   Before wiring anything, confirm it compiles against
   `build/imgui/imgui-1.91.9` with `DESKTOP_CXXFLAGS`. One digest row, one
   license capture (bundled).
2. **Configure for vanilla / no multi-viewports**: set
   `NOTIFY_RENDER_OUTSIDE_MAIN_WINDOW=false` (doc 11 — **must**, since doc 13
   keeps viewports off). Render the toast queue once per frame in the shell's
   top-level draw.
3. **FontAwesome vs Codicons.** ImGuiNotify hard-requires FontAwesome headers
   for its default icons. The app standardised on **Codicons** (doc 13 T4).
   Reconcile explicitly: either (a) merge a minimal FontAwesome range *in
   addition to* Codicons solely for ImGuiNotify's icons, or (b) point
   ImGuiNotify's icon macros at Codicons equivalents. Prefer (b) if the icon set
   maps cleanly (fewer fonts); record the choice. Either way this is why the task
   sequences after F3.
4. Emit toasts from the live event sources (`live/session.*`, `--serve` host,
   teardown/EOF/save paths). The "Open recording" callback calls the same
   `open_request` the panes use. **Refusals**: emit a toast *and* keep the
   in-pane non-collapsible banner (D7) — the toast is a notification, the banner
   is the record.

**Tests.** `test_live_session` / `test_shell`: assert each event kind enqueues a
toast with the right text and, where applicable, a working "Open recording"
callback (drive the toast **model/queue**, not pixels — expose a tiny
`notify_queue` the test can inspect, per D4). Assert a refusal still produces the
banner (the toast does not suppress it). Null backend.

**Docs.** CHANGELOG `Added`: non-modal toasts for live-session events.
`licenses/README.md` row (bundled). `desktop/README.md` note that toasts
supplement, never replace, refusal banners.

**Done when.** attach/refuse/exit/EOF/teardown/save-torn/PT-decode-done raise
toasts; "Open recording" opens via `open_request`; refusals still show their
in-pane banner; `NOTIFY_RENDER_OUTSIDE_MAIN_WINDOW=false`; the FA-vs-Codicons
choice is recorded; it compiles on 1.91.9 and is in the compile-probe.

### T2 — ImSearch client-side narrowing  (S, depends on: 12)

> **Implemented 2026-07-27 (Learn catalog filter) — green (51 suites).** ImSearch
> vendored (`scripts/fetch-imsearch.sh`, branch `main` `7596ac5`, 3 files, MIT,
> C++11; three digest rows; license captured; in the compile-probe —
> `imgui_internal.h`; **compile-verified on `1.91.9b-docking`**, the ∅-unverified
> claim now confirmed). Its `ImSearchContext` is created in `main.cpp`
> **app-only**, and every use is guarded on `ImSearch::GetCurrentContext()` so the
> null test backend shows the plain list. First surface: the **Learn-door
> walkthrough catalog** (`ui/learn_door.cpp`) — a flat `Selectable` list wrapped
> with `BeginSearch`/`SearchBar`/`SearchableItem`, so typing narrows it by title.
> **Honesty-safe by choice of target**: the call TREE was deliberately NOT
> filtered client-side (its `tree.h` warns a client filter makes the surviving
> depths lie — the engine-side filter stays the tree's narrowing). `imsearch.o`
> rides the `learn_door.o` link sites. **Remaining T2**: the syscall-name and
> disasm-routine filters (both tables — need a Selectable/row adapter);
> follow-on.

**Goal.** Client-side filtering over the existing lists — call-tree, syscall
names, disasm routine search, the Learn-door catalog — wrapping current
`Selectable`/`TreeNode` draws via callbacks so **pure view-models stay
untouched** (D4).

**Steps.**
1. **Compile-check first (∅).** Vendor ImSearch (MIT, 3 files, C++11) at a HEAD
   commit sha via doc 12's `fetch-addon.sh`; it includes `imgui_internal.h` →
   compile-probe (doc 12 T3). Confirm it compiles at the 1.91.9 pin before
   wiring. Single maintainer, last push 2025-11 → **vendor-and-own posture** on
   a small surface (doc 11). One digest row, one license capture (bundled).
2. Wrap the target draws via ImSearch's callbacks:
   - `views/tree.cpp` — client-side narrowing **complementing** the existing
     engine-side filter form (both coexist: engine filter narrows the data,
     ImSearch narrows the rendered list).
   - `views/syscalls.cpp` — syscall-name filtering.
   - `views/disasm.cpp` — routine search in the listing.
   - `ui/learn_door.cpp` — the walkthrough catalog.
   The view-models are not touched — ImSearch filters the *draw*, so the golden
   text surfaces (the source of truth) are unchanged.
3. Keep the interaction honest: filtering hides rows, it never renumbers or
   summarises counts in a way that implies the hidden rows don't exist — show
   "showing N of M" where a count matters.

**Tests.** `test_obs_tree` / `test_obs_syscalls` / a disasm test / a learn-door
test: assert the filter callback narrows to the expected subset for a query and
restores the full set when cleared; assert the underlying model is unchanged
(same object, filtered view). Null backend.

**Docs.** CHANGELOG `Added`: client-side filtering for tree/syscalls/disasm/learn
catalog. `licenses/README.md` row (bundled). `desktop/README.md` note that it
complements (not replaces) the engine-side tree filter.

**Done when.** each target list narrows client-side to a typed query and restores
on clear; the tree keeps both its engine-side filter and the new client-side
one; view-models are provably untouched; a "showing N of M" is shown where a
count would otherwise mislead; it compiles on 1.91.9 and is in the compile-probe.

## Task order & parallelism

Independent of each other. **T1 needs doc 13 F3 (fonts/icons)**; T2 needs only
doc 12. Neither blocks anything downstream. A single developer can do both, or
split them. Both begin with a compile-check because doc 11 did not adversarially
verify either.

## Constraints & gates

- **∅-unverified** — both addons' compat is researcher-grade in doc 11. First
  step of each task is a compile-check against the 1.91.9 pin; if it fails,
  that is a fact about the addon, and the fallback is doc 13 F4's 1.92 bump, not
  quietly editing the addon.
- **Honesty (D7)** — toasts supplement refusal banners, never replace them;
  filtering shows "N of M" rather than implying hidden rows are gone.
- **No multi-viewports** — ImGuiNotify renders inside the main window
  (`NOTIFY_RENDER_OUTSIDE_MAIN_WINDOW=false`); doc 13 keeps viewports off.
- **View-model purity (D4)** — ImSearch filters draws via callbacks; models and
  golden text stay the source of truth.

## Out of scope

- The plotting/graph chassis (doc 15) and the editor/test-engine bets (doc 17).
- Any live producer changes (07/08 own the session sources; this doc only
  surfaces their events).
- Replacing the engine-side tree filter (it stays; ImSearch is additive).
