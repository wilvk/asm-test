# 11 — Dear ImGui addon adoption: research findings and plan

> **Status: research/planning document — not an implementation brief.** The
> ten numbered briefs (D11) are unchanged; implementation briefs should be cut
> from the tracks below once decision D2 is amended (see
> [Governance](#governance-amend-d2-first)). Written 2026-07-26 against HEAD
> `4f11065` (survey started at `5779f93`).
>
> **Progress (2026-07-26).** Track G is done — D2 now carries the
> addon-admission rule ([README.md](README.md), "D2 — pinned deps"). The two
> no-dependency Track Q quick wins landed with it: the shared honesty-chrome
> palette (`desktop/src/ui/theme.h`, #6) and the diff "go" routing fix (#7).
> The remaining tracks (foundations F1–F4, the fetch-based addons) now have
> implementation briefs (docs 12–17); no addon code is written yet.
>
> **Provenance.** Produced by a 29-agent research pass: six readers over docs
> 01–10 and the full `desktop/src` tree, twelve independent researchers over
> the addon ecosystem (primary sources only), one synthesis, and ten
> adversarial verifiers who re-checked maintenance, license, and version
> compatibility — including **actual compiles of every top-ranked addon
> against this repo's pinned `build/imgui/imgui-1.91.9` with the desktop
> lane's exact flags** (`-std=c++17 -Wall -Wextra -O2`, `mk/desktop.mk:27`),
> and a full headless desktop-test run (42/42 suites) under both the vanilla
> and docking tarballs. Two compatibility claims were refuted and corrected
> during verification; those corrections are folded in below and flagged
> inline. Ranks 11–12 were not adversarially verified — compile-check them at
> pin time. If a claim disagrees with the code when you implement, the code
> wins — re-verify, then fix this doc in the same change.

## Baseline: what the survey found

The app runs on **vanilla Dear ImGui v1.91.9, zero addons, zero loaded
fonts**. Every view is hand-rolled on raw draw lists. The gaps below are the
demand side of this doc; each recommendation cites the ones it closes.

- **No layout persistence and no multi-pane layout.** `main.cpp:65` sets
  `IniFilename = nullptr`; the shell is one fullscreen window with 3-deep
  nested exclusive tab bars (`ui/shell.cpp`). Timeline + scrubber + disasm
  cannot be shown at once. (At `4f11065` an in-flight shell change surfaces
  Scrubber and ABI x-ray as more nested tabs — that strengthens, not weakens,
  the case for real docking.)
- **No text selection, copy, or find in any text/table view.** The single
  clipboard write in the app is the diff link copy. For a tool whose users
  constantly copy addresses, payloads, and machine reasons out, this is the
  most user-visible missing affordance.
- **No client-side filtering anywhere** — hotedges/syscalls/canvas have no
  narrowing; the call tree relies on the engine-side filter form alone.
- **Default 13 px ProggyClean bitmap font** for a hex-and-registers-heavy UI:
  zero `AddFont*` call sites, a `PushFont(nullptr)` placeholder at
  `desktop/src/ui/learn_door.cpp:149`, no monospace guarantee, no HiDPI story.
- **`data/perf_history.{h,cpp}` is parsed and tested but rendered nowhere**
  (`test_data_readers.cpp` covers it; the only reference is a dead `#include`
  in `completeness.h`; the app contains zero plot lines).
- **The advertised keymap is fiction.** `desktop/src/nav.cpp:282-296` lists 12
  bindings; repo-wide, only `[` / `]` are wired (`scrubber_draw.cpp`,
  `abixray_draw.cpp`, `loom/fabric_imgui.cpp`).
- **The slice explorer is draw-only** (`views/slice_view_draw.cpp`, 83 lines):
  no hit-testing, no zoom/pan, overflows rightward unboundedly.
- **The Loom cannot pan.** `loom/fabric_plan.h:64-67` carries `step0`/`lane0`
  pan fields that no code ever writes; only `steps_per_px` is mutated.
- **No virtualization anywhere** — zero `ImGuiListClipper` uses; fine today,
  a wall at PT scale.
- **Style drift**: the warn color is independently declared (with drifted
  values) in five TUs — `canvas_draw`, `observer_draw`, `hud`,
  `fabric_imgui`, `completeness`.
- **A live bug caught in passing**: the diff view's "go" button calls
  `SetClipboardText` instead of `dt_nav_go`
  (`views/diff_view_draw.cpp:39-43`). One-line fix, not an addon.

## Governance: amend D2 first

> **Amended 2026-07-26.** D2 now carries this addon-admission rule verbatim —
> see [README.md](README.md) ("D2 — pinned deps", Amended clause). Track G is
> done; the addon tracks below are now cut into implementation briefs
> ([12-addon-supply-chain.md](12-addon-supply-chain.md) through
> [17-interaction-testing-and-editor.md](17-interaction-testing-and-editor.md)).
> The two in-tree quick wins that add no dependency (the shared honesty-chrome
> palette and the diff "go" routing fix) landed with the amendment.

D2 (doc 03 / README: "no third-party dep beyond pinned ImGui +
nlohmann/json") blocks everything below. Amend it with an **addon admission
rule** rather than ad-hoc exceptions:

1. MIT/zlib/BSD-class license only; `licenses/` capture per the existing
   Capstone/DearImGui/DynamoRIO pattern. (One exception: `imgui_test_engine`,
   see rank 7 — fetch-at-build, test builds only, never vendored.)
2. Pinned tarball + SHA-256 row in `scripts/third-party-digests.txt`, via a
   `fetch-<name>.sh` following `scripts/fetch-imgui.sh`. Pin release tags
   where they exist; commit-sha tarballs where they don't (`imgui_club`,
   `imgui-node-editor` master, ImGuiNotify Dev).
3. **Compile-check gate wired to any imgui repin.** Five recommended addons
   include `imgui_internal.h` (TextSelect, ICTE, node-editor, ImGuiNotify,
   ImSearch) — safe at the frozen 1.91.9 pin, but every future repin must
   rebuild them before landing. Cheap to add to the fetch scripts.
4. **View-model purity preserved**: addons are draw-half chassis only. The
   pure models and golden-text test surfaces stay the source of truth.
5. Honesty ethos is a selection filter, not an afterthought: nothing below
   renders statistical data as stacks, imposes force-directed layout (banned
   in 04-replay-views.md:357 and 08-observer-views.md:253), or hides
   refusals.

## Foundation moves

Order matters; ranks 2, 3, 10, 11 all land better on top of these.

### F1 — Repin imgui to the docking branch, same version

Swap the tarball to **`v1.91.9b-docking`** (verified to exist; tarball sha256
`466fdef9b18de15f0bb6e288e3d00ffa3d82200ec458ce5e4f724a161d9528a5` — re-verify
at pin time as `tp_digest` requires). The switch is exactly two lines: a
digest row in `scripts/third-party-digests.txt` (without it
`scripts/fetch-imgui.sh:43` hard-fails, by design) and
`mk/desktop.mk:14` `IMGUI_VERSION`. Same six TUs compile; the full headless
desktop suite passes 42/42 under both tarballs at `4f11065` (verified in an
isolated worktree).

- **Why the `b` hotfix and not plain `1.91.9-docking`**: `b` fixes asserts
  when *loading* `.ini` table settings (reordered/hidden/sorted columns,
  upstream #8496/#7934). The app never loads ini today, so plain 1.91.9
  never hits it — but this move's headline feature (layout persistence)
  makes that crash newly reachable, and the app has tables (syscalls etc.).
- **What it buys**: dockable/tearable panes (host the scrubber, ABI x-ray,
  and 3D scene as real panes instead of ever-deeper tabs; side-by-side
  `diff_view`), layout persistence, per-mode presets for Learn/Author/Inspect
  vs replay vs live observer.
- **Layout manager**: a ~200-line in-tree wrapper over `DockBuilder` (20
  decls present at the pinned tag) + `Save/LoadIniSettingsToMemory` for a
  shipped default layout, "reset layout", and per-mode presets. DockBuilder
  is deliberately internal and will churn in the announced docking rewrite —
  contain all `imgui_internal.h` exposure to this one TU. (Hello ImGui's
  `DockingParams` named-layout model is the right *reference*; do not adopt
  Hello ImGui itself — see skips.)
- **Multi-viewports stay OFF**: dead by design on Wayland, "tends to be
  broken" on X11 per the official wiki. macOS-only opt-in at most.
- Risk is low for the branch switch itself — nothing docks until the user
  drags, and `ConfigDockingWithShift` can gate it. Docking branch is actively
  maintained (commits 2026-07-25).

### F2 — 32-bit draw indices via `IMGUI_USER_CONFIG`

Add `#define ImDrawIdx unsigned int` through an `IMGUI_USER_CONFIG` header
injected via `DESKTOP_CXXFLAGS` (covers all imgui + addon TUs; the
digest-pinned tarball is never touched). The binding risk is the **headless
null-backend golden-test tier**, which has no renderer backend and therefore
never sets `RendererHasVtxOffset` — 16-bit indices cap any dense draw there
at 64k vertices. (The live app is usually fine: macOS requests a 3.2 core
context, `main.cpp`, and Linux compat contexts typically resolve to 4.x.
Cite the null-backend tier, not "GL 3.0", as the reason.) Future-proofs
ImPlot heatmaps, node canvases, and the app's own draw-list views.

### F3 — Real fonts: freetype + one monospace TTF + icon merge

`misc/freetype/imgui_freetype.{cpp,h}` **already ships inside the pinned
imgui-1.91.9 tarball** — integration is one `-D IMGUI_ENABLE_FREETYPE`, one
extra TU, and `-lfreetype` in `Dockerfile.desktop` **and the macOS host
build** (the app builds on Darwin since `816999f`; brew/pkg-config). Vendor
JetBrains Mono (OFL-1.1) for all hex/register/disasm surfaces and merge
**Codicons** (the VS Code debugger icon set: step-into/over, watch,
breakpoint — CC-BY-4.0 font, MIT code) via **IconFontCppHeaders** (zlib,
pure `#define` headers, version-agnostic, active — Codicons refresh
2026-06-05). Fixes the `PushFont(nullptr)` placeholder and gives the
scrubber/observer/patch-bay real icon labels. Fonts land **before** any
visual-polish or text-editor addon — nothing text-heavy looks right on the
bitmap font. New license captures under `licenses/`: FreeType FTL, OFL,
CC-BY-4.0.

### F4 — Decide the 1.92.x bump timing now, even if the answer is "not yet"

The app has **zero `AddFont`/`PushFont` call sites today — the cheapest
migration moment it will ever have** (1.92's dynamic fonts delete the glyph
ranges + atlas-rebuild-on-DPI bookkeeping that 1.91.x forces F3 to build).
Multiple addon HEADs are already 1.92-only: ImGuiTextSelect ≥v1.2.0, goossens
ICTE master, ImGuiFileDialog master. If a bump is plausible within two
quarters, do it before building elaborate font/DPI infrastructure, and bump
`imgui_test_engine`'s tag in the same commit. The docking-branch parity pin
(F1) is sound today, but upstream has moved a full minor series — the bump
cost only grows.

## Ranked recommendations

Verification column: ✅ = adversarially confirmed; ⚠ = confirmed with
corrections (folded in below); ∅ = not adversarially verified.

| # | Addon | Pin | Closes | Effort | Verify |
|---|-------|-----|--------|--------|--------|
| 1 | imgui docking branch + in-tree layout mgr | `v1.91.9b-docking` tarball | shell layout, unhosted views, persistence | medium | ⚠ |
| 2 | imgui_freetype + JetBrains Mono + IconFontCppHeaders | in-tarball / OFL TTF / IconFontCppHeaders HEAD sha | legibility, icons, HiDPI | medium | ⚠ |
| 3 | ImPlot | `v1.0` tag | hotedges, perf_history, timeline/scrubber chassis, watch plots | medium | ⚠ |
| 4 | imgui_memory_editor (imgui_club) | commit sha | byte-level region/codeimage interaction | small | ✅ |
| 5 | ImGuiFileDialog | `v0.6.8` tag | open/save dialogs, .asmtrace preview | small | ⚠ |
| 6 | ImGuiTextSelect | `v1.1.6` tag **+ utfcpp** | copy-out everywhere | small | ⚠ |
| 7 | imgui_test_engine | `v1.91.9` tag, fetch-at-build, test-only | interaction-layer testing, keymap enforcement | large | ⚠ |
| 8 | ImGuiColorTextEdit (goossens) + TextDiff | commit sha + 2-line guard | Author editor, disasm decorators, diff | medium | ⚠ |
| 9 | ImZoomSlider (ImGuizmo repo) | commit sha, one header | Loom/timeline pan+zoom | small | ⚠ |
| 10 | imgui-node-editor (thedmd) | master sha `021aa0ea` | topo/tree/slice navigation | medium | ✅ |
| 11 | ImGuiNotify (TyomaVader) | Dev branch sha | non-modal live-session feedback | small | ∅ |
| 12 | ImSearch (GuusKemperman) | HEAD sha | client-side filtering | small | ∅ |

### 1. Docking branch — see F1 above

Covered as the first foundation move; it is also the #1 UX recommendation.

### 2. Fonts/icons — see F3 above

Also a foundation move. Fact note from verification: IconFontCppHeaders'
Font Awesome 7 support landed 2025-08 (refreshed 2026-02); the 2026-06
activity was Codicons/Material/Lucide — Codicons is the set we use.

### 3. ImPlot v1.0 — plotting chassis, not a new look

MIT; maintainership passed to brenocq (v1.0 released 2026-04-05, repo pushed
2026-06-02, ocornut has contributed compat fixes). **Compile-verified against
the pinned vanilla 1.91.9** — back-compat guards reach to `IMGUI_VERSION_NUM
18102`; no docking requirement. Adopt it as an axes/pan/zoom/cursor chassis
around existing pure view-models via `GetPlotDrawList` + `PlotToPixels`:

- `views/hotedges.cpp`: ranked `PlotBars` + src-block × dst-block
  `PlotHeatmap` with `ColormapScale` — stays "edges not stacks",
  honesty-compliant.
- `data/perf_history`: finally rendered (`PlotLines`), closing the
  parsed-but-never-shown gap.
- `views/timeline.cpp` + scrubber: `DragLineX` as the playhead (hover/held
  out-params), `TagX` labels, `SetupAxisLinks` to sync time across
  timeline/watch/diff panes. Axis stays ordinal — custom tick formatters
  label it "step", never wall time.
- `views/watch.cpp`: value-over-step as `PlotStairs`, flag bits as
  `PlotDigital`.

All named APIs verified present at the `v1.0` tag. **Pin the tag**: master is
v1.1 WIP tracking ImGui 1.92 signatures, and v1.0's ImPlotSpec redesign
(PR #519) removed the old styling API — virtually all pre-2026 ImPlot
examples online target ≤v0.17 and will not compile. Works under the
null-backend golden harness (draw lists only); F2 covers the dense-heatmap
index headroom.

### 4. imgui_memory_editor — cheapest strong upgrade (confirmed outright)

Single 836-line header from ocornut's own `imgui_club`; **public API only —
no `imgui_internal.h`**; plain MIT; repo commit 2026-07-22 (the component
itself last touched 2025-08-04, by ocornut, stable). Compile-verified against
the pinned tree, including the null-mem + `base_display_addr` form — so
**`ReadFn` maps directly onto the existing client-side version resolution**
in `views/disasm.cpp:112-138` (`obs_disasm_bytes_at`: greatest `when` ≤ t),
no flat buffer needed. `BgColorFn`/`HighlightFn` paint bytes differing
between codeimage versions (JIT churn), dirtied bytes, and slice membership;
`GotoAddrAndHighlight` wires disasm/syscall-arg clicks to exact bytes;
`ReadOnly` honors never-re-read-live-memory in replay. Today `region.cpp`
emits bytes only as pre-formatted hex strings with zero byte-level
interaction. Limitations: background-tint only (foreground `ColorFn` is open
request #59); no tags/releases → commit-sha tarball pin.

### 5. ImGuiFileDialog v0.6.8 — pure-ImGui open/save

Replaces the raw `InputText` path fields at `desktop/src/ui/shell.cpp:309`
(`draw_open_dialog` — the deferral is verbatim in
`03-desktop-shell.md:422`) and `desktop/src/ui/inspect_door.cpp:262`
(`draw_save_capture`). Pure ImGui: identical behavior on Linux, macOS, and
inside `docker-desktop` — no zenity/osascript runtime deps, which is what
keeps the lane testable (CLAUDE.md: a lane that can only self-skip is not a
test). The per-extension side pane can preview an `.asmtrace` (arch, event
count, torn/end-footer status) before opening — provenance-first. MIT,
active (master 2026-03-11). **Correction from verification**: v0.6.8
(2025-10-03) is *not* "contemporaneous with 1.91.x" — it works on 1.91.9
because of an explicit `#if IMGUI_VERSION_NUM < 19201` guard
(`ImGuiFileDialog.cpp:4251`), and it compile-verifies clean with the desktop
lane's exact flags. It includes `imgui_internal.h`: couple its pin to the
imgui pin. Do not track master (targets 1.92.3 / `ImTextureRef`).

### 6. ImGuiTextSelect — copy-out for every line-oriented pane

Closes the bluntest survey finding (no selection/copy anywhere). MIT, active
(v1.3.2 released 2026-07-04), callback-driven (`getLineAtIdx`/`getNumLines`)
so it overlays `observer_draw` syscalls, `disasm` listings,
`capability_panel` machine reasons, diff halves, and `completeness`
skip-reason cells without touching view-models. **Refuted and corrected
during verification**: v1.2.0+ *dropped* support for pre-1.92 ImGui —
v1.3.2 calls `ImGui::GetFontBaked()` and `ImFont::CalcWordWrapPosition()`
(`textselect.cpp:171,180`), which do not exist in 1.91.9 and **do not
compile**. At the current pin, use **v1.1.6 (2025-06-10) + its utfcpp
dependency** (BSL-1.0; refutes "two files" — one more digest to pin); the
current line becomes available only after a 1.92 bump (F4). Host pane needs
`ImGuiWindowFlags_NoMove`; selection math assumes uniform-height lines, so
multi-column table cells map to whole rows; excluded from the Loom prim
canvas by design.

### 7. imgui_test_engine — test the interaction layer (bigger bet)

First-party, **tag-matched `v1.91.9` exists**, vanilla-compatible (the
`#ifdef IMGUI_ENABLE_TEST_ENGINE` hooks are already in the vendored
`imgui.cpp`/`imgui_widgets.cpp`; enable via a `-D` flag — imconfig change,
no tarball/digest change). Headless null-backend mode + JUnit XML exporter
(present at the tag) fit the existing CI lanes. Targets the layer the
golden-text strategy cannot reach: Learn/Author/Inspect door flows
(`desktop/src/ui/{learn,author,inspect}_door.cpp`), the syscalls two-step
reveal-all confirm (`views/syscalls.cpp:65-72`), patch-bay queue/swap,
scrubber dragging, and the 12-entry advertised keymap of which only `[`/`]`
is wired — **writing engine tests for the bindings forces implementing
them**. Docs 04/05 admit end-to-end flows are "manual smoke, noted in the PR"
(`04-replay-views.md:399,491`, `05-loom-day-one.md:237`); this closes that
gap and becomes a foundation of its own: every future view lands with an
interaction test. **License**: *not* MIT — "Dear ImGui Test Engine License"
v1.04; its fourth free-use bullet (derivative software released publicly
under an OSI license) applies to this repo (public, MIT). Keep it
fetch-at-build, test builds only, never vendored, so the repo stays 100%
MIT. Risks: hand-rolled draw-list views (Loom, slice graph) are invisible to
item-path addressing until rows gain `InvisibleButton`s — drive those
positionally or by screenshot; bump the engine tag in the same commit as any
imgui repin.

### 8. ImGuiColorTextEdit (goossens rewrite) + TextDiff

Very active (PRs merged 2026-07-24), MIT + bundled dtl (BSD-3), 2–5 files, no
external deps, no regex. Replaces the Author door's
`InputTextMultiline`-over-`s.source.data()/capacity()` 64 KB hack
(`desktop/src/ui/author_door.cpp:129`, `doors.h:85` — silent truncation
risk) with a real editor: undo/redo, find/replace, and **error markers
anchored to the assembler's loud-drop line** — verbatim machine reasons
anchored where they happened, exactly the honesty ethos. Read-only mode +
line decorators give `views/disasm.cpp` an address/bytes gutter and
current-PC highlight; `TextDiff::SetSideBySideMode` upgrades
`views/diff_view.cpp`. A custom x86/ARM asm language def via its
state-transition colorizer API is a bounded, testable task. **Refuted and
corrected during verification**: "no 1.92-only APIs at master" is false —
every commit since 2025-06-29 requires ImGui 1.92
(`ImGuiPlatformImeData::WantTextInput`/`ViewportId`, one SDL3-specific IME
block), and the "Legacy" tag has the same requirement. **Mitigation is
compile-verified**: wrap those 2 lines in `#if IMGUI_VERSION_NUM >= 19200`
and master's `TextEditor.cpp` compiles clean on 1.91.9; `TextDiff.cpp`
compiles unmodified. Ship as pin-a-commit + the 2-line local patch (or after
F4). Upstream README announces a compat-breaking "future" branch — hard-pin
and re-audit on every bump. Includes `imgui_internal.h`.

### 9. ImZoomSlider — the missing pan/zoom control, one header

245-line self-contained header at `src/ImZoomSlider.h` in the ImGuizmo repo
(MIT, repo pushed 2026-07-17); vendor just the header at a commit sha — no
gizmo code built. Gives the Loom the pan interaction its model already
carries but nothing mutates (`fabric_plan.h:64-67`), the timeline a windowed
view into a long trace's step range, and hotedges/diff an overview strip.
**Corrections from verification**: it is *not* public-API-only — it uses
`ImRect`/`ImMin`/`ImMax` and math operators, so the include recipe is
mandatory: `#define IMGUI_DEFINE_MATH_OPERATORS`, then `imgui.h`, then
`imgui_internal.h`, then `ImZoomSlider.h`. Wrap **each instance in
`PushID`** (fixed internal `"ImZoomSlider"` label would collide across the
three proposed sites), and note the function-local static drag state is
single-ImGuiContext-only (fine here).

### 10. imgui-node-editor — navigation for the graph views (confirmed outright)

MIT (Michał Cichoń); maintenance-mode upstream (compat fixes 2026-03-29
after a two-year gap; last release v0.9.3 is 2023 and **fails to compile
against 1.91.9** — the `operator==` redefinition; **pin master sha
`021aa0ea`**, whose 4 TUs compile clean with the desktop lane's exact flags;
both facts independently reproduced). Keep the app's own deterministic
layout — feed `ed::SetNodePosition` every frame; force-directed stays banned
per docs 04/08 and the library imposes no layout. Buys `views/topo.cpp` and
`views/tree.cpp` a pan/zoom canvas, selection, and `NavigateToContent` ("fit
graph"); `views/hotedges.cpp`'s frozen snapshot gains legible navigation.
De-risk path: adopt the standalone `imgui_canvas` (846 LOC; .h+.cpp) alone
first for `views/slice_view_draw.cpp` — pan/zoom + input remapping without
node semantics — **but note it includes `imgui_internal.h` too**
(`imgui_canvas.h:52`), so the repin compile-gate applies either way. Set
`config.SettingsFile = nullptr` to keep layout fully app-deterministic.
Adoption pulls 4 TUs including the vendored `crude_json.cpp`. Large-graph
perf needs visible-region culling.

### 11. ImGuiNotify — non-modal session feedback *(not adversarially verified)*

Today a live-session event that happens while the user is in another view is
silent: attach succeeded/refused, target exited, bounded-session EOF,
SIGALRM teardown, `.asmtrace` save/torn warnings, long PT-replay decode
completion. ImGuiNotify (MIT, canonical successor to the archived patrickcjk
original) adds toasts with callback buttons ("Open recording") that compose
with the cross-door `open_request` mechanism already in `shell.cpp`. Honesty
note: toasts *supplement* the non-collapsible banners — refusals stay
first-class in-pane. Constraints: **must** set
`NOTIFY_RENDER_OUTSIDE_MAIN_WINDOW=false` on vanilla (no multi-viewports);
includes `imgui_internal.h`; hard-requires FontAwesome headers — **sequence
after F3**. Pin the Dev branch at a sha; compile-check at pin time.

### 12. ImSearch — client-side narrowing *(not adversarially verified)*

Zero client-side filtering exists anywhere in the app; for 10k-routine trace
graphs that is a navigation gap, not polish. ImSearch (MIT, 3 files, C++11)
wraps existing `Selectable`/`TreeNode` draws via callbacks — pure view-models
untouched. Targets: call-tree narrowing (`views/tree.cpp`, complementing the
engine-side filter form), syscall-name filtering, routine search in the
disasm listing, and the Learn-door walkthrough catalog. Risk: single
maintainer, last push 2025-11, includes `imgui_internal.h` —
vendor-and-own posture on a small surface; compile-check against 1.91.9 at
pin time.

## Quick wins (no tarball changes; can proceed in parallel, any order)

1. **ImZoomSlider** (#9) — one header; unblocks the Loom pan today.
2. **ImGuiTextSelect** (#6, at the corrected v1.1.6+utfcpp pin) — copy-out.
3. **imgui_memory_editor** (#4) — `ReadFn` drops onto `obs_disasm_bytes_at`.
4. **Built-in `ImGui::ProgressBar` indeterminate mode** — already in 1.91.9,
   zero fetch: determinate for file loads with an end footer, indeterminate
   for live/torn sessions where no total honestly exists. Optionally one
   subtle imspinner style for "attached-but-no-events stall vs dead UI".
5. **ImGuiFileDialog** (#5) — one TU, replaces both bare path fields.
6. ✅ **Shared theme/palette header** (in-tree, no addon) — **landed
   2026-07-26** as `desktop/src/ui/theme.h`: the caution/refusal colours are one
   source (`dt_warn_col`/`dt_refuse_col` + their ImU32 forms), consolidating the
   amber that had drifted to three values across `canvas_draw`, `fabric_imgui`,
   `hud`, `completeness`, and `observer_draw`. `test_obs_draw` pins the invariant
   the Loom's draw-list form depends on. (A classroom light/dark pair via
   ImThemes stays optional and unstarted.)
7. ✅ **Fix `views/diff_view_draw.cpp`** — **landed 2026-07-26**. The "go" button
   now parses its row's canonical link and routes the `dt_link` through the
   shell's `dt_nav_go` seam (the same path the Observer deck uses), instead of
   `SetClipboardText`. `draw_diff_view` grew the `go` callback param to reach it.

## Deliberate skips (all verified negative results — do not re-litigate)

- **imgui-flame-graph** — technically fine and MIT, but honesty rule R4 bans
  stacks for survey data, and doc 02 deliberately ships speedscope/Perfetto
  exporters "instead of rebuilding those views". Adopting it re-litigates a
  settled product decision.
- **imnodes (Nelarius)** — actively maintained, has the category's only
  minimap, but **zoom does not exist** (issues #51/#109/#118/#134/#192);
  pan-without-zoom is a regression vs the hand-rolled canvas.
- **ImPlot3D** — does not replace `scene3d/`: no per-item picking (ray API
  only), no real tubes, CPU-transform rendering. Migrating deletes working
  features (color-ID FBO picking, lit terrain).
- **ImSequencer / imgui-neo-sequencer** — editing-shaped (keyframe
  add/delete semantics) for a read-only ordinal event stream; neo-sequencer
  is dead (2023-02) and won't compile on 1.91.9 (pre-1.90 `ItemHoverable`).
- **Hello ImGui** — its `DockingParams` named-layout model is the right
  reference, but it owns the main loop, vendors its own imgui, and is
  CMake-first: adopting it replaces the doc-03 shell and the digest-pinned
  supply chain. Reimplement the ~200-line idea in-tree (F1).
- **Multi-viewports** — dead on Wayland by design, "tends to be broken" on
  X11 per the official wiki; can never be baseline for a Linux+macOS app.
- **ImHex** — GPL-2.0 *application* with a patched imgui fork, not a widget;
  license alone disqualifies vendoring.
- **portable-file-dialogs** — native dialogs need zenity/kdialog at runtime
  on Linux; collides with "a lane that can only self-skip is not a test"
  unless the desktop image grows zenity. One dialog stack, not two.
- **Zep** — a mini-Vim with its own buffer/mode/theme object model,
  CMake-first; goossens ICTE covers Author-door editing at a fraction of the
  surface.
- **santaclose ImGuiColorTextEdit** — dominated by the goossens rewrite
  (TextDiff + decorators, zero extra deps); drags in a vendored Boost.Regex
  submodule.
- **ImHotKey** — the only hotkey-editor addon, verified **broken on 1.91.9**
  (legacy int key-index API removed). Rebindable keys are worth revisiting
  only after the advertised keymap actually works (see rank 7).
- **sequentity** — conceptually the closest sequencer to an event timeline,
  but **no license file** (legally unvendorable) plus an EnTT dependency.
- **ImCoolBar / imgui_toggle** — motion/mobile aesthetics; eye candy, not
  legibility. Contrary to the honesty-chrome design language (toggle is
  harmless but belongs to a deliberate polish pass, if ever).
- **Tracy's timeline** — not extractable (coupled to its Worker model and a
  patched imgui), but BSD-3: read `TracyTimelineController` and its
  sub-pixel LOD merge as the **design reference** when the hand-rolled
  timeline needs virtualization at PT scale (zero `ImGuiListClipper` today).
- **VisualNodeSystem** — 520 files, vendored GLM+jsoncpp, compiles its own
  imgui by default; framework-weight, bus-factor-one.
- **Accessibility addons** — negative result worth recording for a teaching
  tool: **no ImGui/AccessKit binding exists as of 2026-07** (imgui #4122 /
  #8022 open). Plan around keyboard nav + the text exporters; don't wait for
  the ecosystem.

## Sequencing

```
Track G (governance):  amend D2 with the addon-admission rule   ← blocks all
Track Q (quick wins):  ImZoomSlider · TextSelect(v1.1.6+utfcpp) ·
                       memory_editor · ProgressBar · FileDialog ·
                       palette header · diff_view "go" fix      ← parallel, anytime after G
Track F (foundations): F1 docking repin (v1.91.9b-docking)
                       F2 ImDrawIdx user-config
                       F3 fonts/freetype/icons
                       F4 decide 1.92 bump timing               ← F1–F3 parallel; F4 is a decision
Then:                  ImPlot v1.0 · node-editor/imgui_canvas ·
                       ImGuiNotify (needs F3) · ImSearch
Bigger bets:           imgui_test_engine · goossens ICTE        ← independent of each other
```

Every addon lands with: `fetch-<name>.sh`, a sha256 row in
`scripts/third-party-digests.txt`, a `licenses/` capture, and — for the five
`imgui_internal.h` dependents — a compile-check wired to the imgui repin
path.

## Verification appendix

The top ten recommendations went through an adversarial verify pass
(2026-07-26) instructed to refute maintenance, license, compatibility, and
fit claims from primary sources. Outcomes: **0 rejected; 2 confirmed
untouched** (imgui_memory_editor, imgui-node-editor); **8 adjusted**. The
material corrections, all folded in above:

1. **ImGuiTextSelect**: compat claim inverted — v1.3.2 does not compile on
   1.91.9 (1.92-only APIs); corrected pin v1.1.6 + utfcpp.
2. **goossens ICTE**: "no 1.92-only APIs at master" refuted — 1.92 required
   since 2025-06; a compile-verified 2-line version guard restores 1.91.9.
3. **Docking**: pin moved to `v1.91.9b-docking` (ini-table-load assert fix
   becomes reachable exactly when persistence lands); "fetch-imgui.sh
   unchanged" corrected to digest-row + one mk line.
4. **ImZoomSlider**: "public-API-only" refuted — needs the
   math-operators/imgui_internal include recipe + per-instance `PushID`.
5. **ImGuiFileDialog**: works on 1.91.9 via an explicit version guard, not
   contemporaneity; paths corrected; `--record-dir` GUI claim dropped (it is
   a runner/CLI flag with no GUI surface).
6. **ImPlot**: the ImDrawIdx risk is the null-backend golden tier, not "GL
   3.0"; pin the v1.0 tag (master is 1.92-tracking v1.1 WIP).
7. **imgui_test_engine**: license basis is the fourth free-use bullet of the
   Test Engine License v1.04 (public OSI-licensed derivative); the "manual
   smoke" admissions live in docs 04/05, not 02/04.
8. **Fonts**: FA7 support date corrected (2025-08, not 2026-06); macOS host
   build also needs freetype, not only `Dockerfile.desktop`.

Compile evidence was produced against this repo's own pinned
`build/imgui/imgui-1.91.9` with `mk/desktop.mk`'s exact flags; the docking
comparison ran the full headless desktop suite (42/42 both branches) in an
isolated worktree at `4f11065`. Ranks 11–12 (ImGuiNotify, ImSearch) were not
in the verified set — treat their claims as researcher-grade and
compile-check at pin time.
