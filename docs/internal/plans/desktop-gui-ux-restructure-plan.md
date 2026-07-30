# Desktop GUI — UX restructuring plan

> **What this is.** The actionable roadmap that follows from the heuristic
> evaluation in [desktop-gui-ux-review.md](desktop-gui-ux-review.md) (24 confirmed
> findings, F1–F24). It reframes the app around what it already is, sequences the
> work by leverage into four waves, and maps each change onto the existing
> implementation tracks ([../gui/](../gui/README.md) 13/14/15/16/17) so a junior
> developer can cut a one-doc brief from any row. Every recommendation traces to a
> finding; nothing here removes a truth — the fidelity chrome is *restructured*
> (F5), never removed. Generated 2026-07-27 from the review's synthesis
> (IA-first skeleton, leverage-first sequencing).

## Framing

asm-test already owns the two hardest parts of good information architecture —
**one addressable document format** (`.asmtrace`) and **one deterministic
navigation spine** (`dt_nav_go`) — but ships them hidden behind a stack of
closeable tabs drawn inside a single `Begin("asmtest")` window. The review
confirmed (by hand, against the code) that the severe findings are *wiring or
extension on shipped substrate*, not redesign. So the restructuring reframes the
app as what it is:

> **A Workspace of recordings, read through many linked lenses over one exposed,
> keyboard-driven, orientation-aware spine — with fidelity as a graded system and
> one visual language.**

Two standing rules fall straight out of the project's own fidelity culture and
should govern all of the work below:

1. **Never advertise an affordance that is not wired.** (F1: the help overlay
   lists 10 dead keys.)
2. **Never ship an inert control.** (F2: the View menu / dock presets act on
   windows the shell never opens.)

## The five themes

Recommendation IDs are stable (`T<theme>.<n>`); the **F** column links each to
the review finding it resolves. Impact/effort are the review's calibrated
estimates.

### Theme 1 — Make the spine operable
*The app lies on its most navigational surface; the keyboard-first tool has no
working keyboard. Fix the breach first, then make selection teach.*

| ID | Change | F | Impact | Effort |
|---|---|---|---|---|
| **T1.1** | Wire the 10 dead accelerators to the same `dt_nav_go` calls that back the mouse "go" buttons (`1/2/3/4` view-switch, `j/k`+arrows+PgUp/Dn step, `Ctrl+G` go-to, `Enter` open-slice, `b/f/c` cone, `d/x` diff, `n/p` divergence). **Same day, independently:** add a `wired` flag to `dt_binding`, generate the help overlay from wired keys only, grey the rest "planned" (the app's own greyed-shows-why law). Then land doc 17's headless test-engine so advertised==wired can't re-rot. Then align to convention: `F` fit-selection, `,`/`.` sibling step, `W/S` zoom + `A/D` pan (Ctrl+wheel / Shift+drag), `F10/F11` step, `Ctrl+C` copy (keep `y` alias); a labelled context-switch mods WASD between timeline and 3D. | F1, F18 | high | med |
| **T1.2** | Promote selection to **one** shared brushing-and-linking model (entity id → {step, offset, lane}) held at Workspace/shell level, distinct from navigation: a pick in any pane cross-highlights the same entity everywhere and drives detail/disasm/Loom/3D at once; replace the per-view `selected_step`/`cone_active`/clicked-worldline/row state. *(Depends on Theme 2 panes.)* | F7 | high | high |
| **T1.3** | Reach the mouse-only islands: keyboard camera on the 3D HUD (arrows orbit, +/- dolly, keys for reset + top-down faithful fallback), Tab focus into every pane and the `SceneHost`, and the `b/f/Enter` cone keys so the slice DAG is operable without a mouse. | F18 | med | med |
| **T1.4** | Global find (`Ctrl+F`): highlight all hits in timeline + minimap, report match count + aggregate cost, cycle Enter/Shift+Enter — search-as-measurement. Extend ImSearch client-side narrowing to syscalls/disasm/hot-edges; keep the call **tree** engine-filtered so surviving depths never lie. | F17 | med | med |
| **T1.5** | App-level command/undo stack (`Ctrl+Z`/`Ctrl+Y`) over reversible view-model state (filter predicate, perspective/layout, cone/selection, take set), distinct from the Author buffer's own undo; give the Loom takes gutter per-take remove + "clear forks". | F12 | med | med |

### Theme 2 — Name the container
*A Workspace of dockable panes entered by task, not a stack of closeable tabs.
This is the structural unlock the rest of the plan leans on.*

| ID | Change | F | Impact | Effort |
|---|---|---|---|---|
| **T2.1** | Convert each view into a real pane `Begin()`'d under the `kPane*` names `layout.cpp` already docks (Home/Recording/Scrubber/Inspector/Timeline), flattening the 3-deep tab nesting in the same pass — so DockBuilder presets, tear-out and Reset actually rearrange visible panes and timeline+scrubber+disasm can finally coexist. **The keystone.** | F2, F4, F9 | high | high |
| **T2.2** | Make `layout_reset` **real** and always-available in both binaries + a keybinding + palette entry, with a load-fault / zero-visible-pane auto-fallback to the shipped default. **Stop-doing:** do not ship the View menu / presets until the panes are real. | F2 | high | low |
| **T2.3** | Make outer view presence data-driven by copying the Observer deck's own empties-gating: lean default set (Summary/Canvas/Timeline), reveal Loom/3D/PT-slice/heatmaps only when their events/capture are present, scope by mode; collapse the rest into one candid "unavailable views (N)" affordance that still names each view + its machine reason; retire the legacy empty `door_tabs`. | F4 | high | med |
| **T2.4** | Reframe entry from a jargon door-chooser to task-language modes on a **persistent home/nav rail** (not a closeable tab): "Learn how assembly runs" / "Open a trace I have" / "Capture a live process" / "Author a routine", with Author/Inspect below the primary CTAs; auto-land in Learn (dependency-free) on an empty workspace; mode selection drives its dock perspective so label and layout agree. | F13 | high | med |
| **T2.5** | Persist and restore the Workspace (open recordings + active tab + per-pane selection, each an `asmtrace-link`) across launches; add an MRU recents list on the Home rail and a File menu (each a deep-link reopening to the exact prior position) + drag-drop; reframe Home as a recents landing. Store alongside the dock `.ini`. | F10 | high | med |
| **T2.6** | Give Author/live output a save path by reusing `inspect_door`'s confirm-overwrite `draw_save_capture` dialog; treat unsaved output as dirty (close prompts save/discard/cancel, or auto-persist to a scratch recording so close is reversible — `workspace.close()` today only erases the in-memory entry); mark saved vs unsaved in the tab title. | F24 | high | low |
| **T2.7** | Add named perspectives + named saved-filter/query presets (the VS Code/Blender/JetBrains workspace model) once panes are real. | F2, F16 | med | med |
| **T2.8** | Build a DPI-aware font atlas (rebuild on content-scale change) + user text-scale (~0.8×–2.0× via `FontGlobalScale` or re-bake) + persisted window size (replace hardcoded 1280×720) + a light theme, in a small **Settings** pane — the one accessibility lever fully inside ImGui's control. | F6 | high | med |

### Theme 3 — Expose the spine
*Turn the built-but-hidden router into the app's orientation model. All thin
layers over `dt_nav_go`, which today holds only `nav.current`.*

| ID | Change | F | Impact | Effort |
|---|---|---|---|---|
| **T3.1** | Command palette on `Ctrl+Shift+P` / `Ctrl+P`: view-switch, go-to-step/offset, open-recent, attach-pid, run-walkthrough, reset-layout, routine/symbol fuzzy match — each dispatching through `dt_nav_go` and enumerated from `dt_nav_bindings`, making every accelerator (including the freshly-wired ones) discoverable by typing; reuse the ImSearch "showing N of M" idiom. | F8 | high | med |
| **T3.2** | Give `dt_nav_go` a bounded back/forward history stack (`Alt+Left`/`Right` + breadcrumb) — one place, since it is the single choke point; the stack is just a vector of serialisable `asmtrace-link`s, headlessly testable. **Cheapest big lever.** | F11 | high | low |
| **T3.3** | Persistent wayfinding chrome outside the per-tab body: recording → session → view breadcrumb + active step/selection + filter/scope + thread, sourced from `nav.current`; disambiguate same-basename tabs with a parent-dir segment or short hash. | F9 | med | low |
| **T3.4** | Always-visible overview/minimap on the timeline and Loom (whole trace, current viewport drawn, click-to-jump via `dt_nav_go`), reusing Tracy's `TimelineController` and completing the 14 T5 ImZoomSlider stub; the deterministic-layout ban means the map never fabricates structure. | F8 | med | med |

### Theme 4 — Restructure the truth layer
*Fidelity and status as one graded, actionable system. Extends track 16; keeps
every truth.*

| ID | Change | F | Impact | Effort |
|---|---|---|---|---|
| **T4.1** | Collapse the proliferating forms into a small fixed chrome vocabulary (one banner form, one inline chip, one glyph set) with mandated placement (banner pane-top, chip on stream header), built as shared components. Add a **`severity` field beside the existing fidelity schema fields** (so grading stays testable against the deliberate low-fidelity fixtures) and render 3 tiers: **T1 neutral** (skip=success, coarse rung, bounded window, redacted-by-policy, statistical) = quiet chip, no warn colour; **T2 caution** (truncated-but-usable) = amber, collapsible-to-chip after first read; **T3 integrity** (torn, mixed-basis refusal, dropped-prefix→UNKNOWN) = red, loud, non-collapsible. *(Schema change — coordinate with doc 01's Phase-3 freeze.)* | F5 | high | med |
| **T4.2** | Lead `capability_panel` with a positive one-line summary from the same resolvers ("This host: emulator + single-step available; IBS/PT unavailable — details below"; "Learn and Author work here, no root/hardware/attach"); demote unavailable backends into an expandable "why can't I capture X?" with the verbatim reason collapsed under each row; **reuse the `inspect_door` why/remedy map** (paranoid/Yama/i386/CAP_SYS_PTRACE) it does not yet call — verbatim reason stays as a floor, remedy layers on top. | F19 | high | low |
| **T4.3** | Replace the collapsed "ended" state with a persistent in-pane end-of-session placard co-located with the last events, distinguishing {stopped-clean, torn: host exited/crashed, torn: EOF, PROTOCOL-MISMATCH (usage-banner/unparseable lines, already counted)}; render the one-line fix ("rebuild `build/asmspy`; Disconnect+reconnect") in the pane; toasts supplement, never replace. | F20 | high | med |
| **T4.4** | Split the overloaded "paused": operator pause → "PAUSED (you) — Resume"; budget block → "BLOCKED — jack held by \<session\> on \<target\>" with explicit **[Swap]** (confirm naming what detaches), **[Queue]** (visible cancellable chip + one-line note of what Queue does when the jack frees), **[Cancel]**; never auto-swap without confirmation. | F23 | med | med |
| **T4.5** | Gate arming a perturbing single-step mode on a live target behind an inline confirm stating the concrete consequence ("dirties the traced page and perturbs timing; on arm64 can terminate a target blocked in a syscall and detach cannot undo it — prefer IBS/PT"); default the capture picker to the least-perturbing substrate the host supports; grey/annotate single-step for arm64 blocking-syscall targets. Reuse the reveal-all second-confirm pattern. | F22 | high | low |
| **T4.6** | Generalise the landed `progress.h` helper to **every** op that can exceed a frame (PT decode, symbol/codeimage load, terrain/trajectory rebuild), with elapsed-time + Cancel + spinner; give the 3D scrub a degrade-to-coarse-terrain path instead of a silent stall. | F21 | med | low |

### Theme 5 — One visual language
*A single design system for colour, accessibility, and words. Extends `theme.h`
and the Sphinx glossary.*

| ID | Change | F | Impact | Effort |
|---|---|---|---|---|
| **T5.1** | Extend `theme.h` to the full semantic set (good, bad, maybe/weak, changed, cone-backward, cone-forward, selected, statistical) as named accessors; delete every inline literal and local `kGood/kBad/kMaybe/kChanged/kDiffer`/cone colour (inspect_door:91-93, scrubber_draw:66, abixray_draw:18/22, slice_view_draw:18-24, scene3d/hud); add a shared legend component. The substrate the fidelity-tier (T4.1) and CVD (T5.2) work both build on. | F14 | high | med |
| **T5.2** | Define one CVD-verified categorical palette (simulate protan/deuter/tritan) and back every categorical distinction with a **second channel**: Codicons/shape for cone direction, label/fill-pattern for dim/hot/neutral, a CVD-safe ImPlot colormap for the heatmap; verify text ≥4.5:1 and fills/borders ≥3:1 at the smallest font (`dt_warn` amber is large-text-only). Generalise the existing solid-tube-vs-stipple second channel. | F15 | med | med |
| **T5.3** | Lead every surface with the canonical domain term, metaphor as subtitle ("Data-flow lineage (Loom)", "First divergence (patient zero)", "Tracer contention (patch-bay)"); drive a hoverable term registry from the **one** Sphinx glossary source so first occurrence of any coined term gets a definition tooltip (one source → legend/tooltip/teaching can't drift); per-view "?" stating what the view shows and what its metric means ("hot-edges are edge counts, not a call stack"); inline legends in every encoded view + a searchable Terms pane reusing the Learn ImSearch idiom. **Chrome the user cannot decode is not faithful.** | F3 | high | med |
| **T5.4** | Standardise **one** filter affordance (type-to-narrow "showing N of M", doc 16) on every list/table + free ImGui column-sort on the tabular views; **one** shared time-position widget with two faithful variants — continuous scrub where a total exists, discrete step where it does not (Invocations), the discrete case visually marked as intentional. | F16 | med | med |
| **T5.5** | Give Loom and the 3D overview a first-open in-canvas primer + legend, dismissible and re-openable via the per-view "?"; front-load nothing heavier than the lean default until acknowledged. | F4, F3 | med | low |

---

## Sequencing — four waves

Ordered to **lead with the change that most improves the app** while respecting
dependencies. Wave 0 is cheap and removes active breaches (a lie, an inert
control, a data-loss trap, a misleading first impression) — most of it is
independent of the docking refactor and can ship immediately. Wave 1 is the
structural keystone that unblocks the rest.

**Wave 0 — Stop the breaches (low effort, high trust impact, mostly independent)**
`T1.1` keymap + faithful overlay · `T2.2` real Reset + retire inert menu ·
`T2.6` Author save + dirty-close guard · `T4.2` capability-panel positives ·
`T4.5` perturbing-mode confirm · `T3.2` back/forward history.

**Wave 1 — Name the container (the structural unlock)**
`T2.1` real dockable panes + flatten *(keystone — unblocks T1.2, T2.7, T3.3)* ·
`T2.3` data-driven tabs · `T2.4` task-language entry rail · `T2.5` workspace
persistence + recents · `T2.8` DPI/text-scale/Settings pane.

**Wave 2 — Expose and operate the spine**
`T3.1` command palette · `T3.3` wayfinding chrome · `T3.4` overview/minimap ·
`T1.2` shared selection *(needs Wave 1 panes)* · `T1.3` keyboard islands ·
`T1.4` global find · `T1.5` app-level undo.

**Wave 3 — Truth layer + visual language (systematic depth)**
`T4.1` graded fidelity tiers *(schema-coordinated)* · `T4.3` session-end placard ·
`T4.4` split "paused" · `T4.6` progress everywhere · `T5.1` semantic palette ·
`T5.2` CVD + second channel · `T5.3` glossary/term registry · `T5.4` unified
filter/time widget · `T5.5` Loom/3D primer · `T2.7` perspectives + saved filters.

## Briefs cut from this plan (2026-07-27)

All 28 unbuilt recommendations below are now implementation-ready briefs under
[../gui/](../gui/README.md), one per wave/theme cluster (drafting-only — no code
yet), each tracing its tasks to the `T<theme>.<n>` IDs here:

| Brief | Wave | Recommendations |
|---|---|---|
| [18-breach-stops.md](../gui/18-breach-stops.md) | 0 | T1.1 (remainder), T2.2, T2.6, T4.2, T4.5, T3.2 |
| [19-dockable-panes-keystone.md](../gui/19-dockable-panes-keystone.md) | 1 | **T2.1 (keystone)** |
| [20-workspace-and-settings.md](../gui/20-workspace-and-settings.md) | 1 | T2.3, T2.4, T2.5, T2.7, T2.8 |
| [21-spine-navigation.md](../gui/21-spine-navigation.md) | 2 | T3.1, T3.3, T3.4 |
| [22-selection-and-search.md](../gui/22-selection-and-search.md) | 2 | T1.2, T1.3, T1.4, T1.5 |
| [23-graded-truth-layer.md](../gui/23-graded-truth-layer.md) | 3 | T4.1, T4.3, T4.4, T4.6 |
| [24-one-visual-language.md](../gui/24-one-visual-language.md) | 3 | T5.1, T5.2, T5.3, T5.4, T5.5 |

T1.1's keymap *core* already landed (doc 17 T1: `handle_keymap`); doc 18 T1
carries only the remainder (faithful overlay + convention keys).

## Leverage map — redirect what is in flight, brief what is not

**Redirect (sunk-cost-free — the track is unstarted or its landed piece is
orphaned):**
- **Track 17** (interaction-testing + editor, *not started*) → its first
  deliverable becomes `T1.1` (keymap wiring + test-engine enforcement of
  advertised==wired). The ImGuiColorTextEdit half stays as-is.
- **Track 13 T2** (docking landed but panes orphaned, F2) → `T2.1`/`T2.2` finish
  the conversion the doc already scoped as "the follow-on UX refactor". Its F4
  dynamic-DPI decision becomes `T2.8`.

**Extend (a landed foundation, now generalised):**
- **Track 16** (filtering landed; toasts partial) → `T4.3` (session-end placard,
  keeping toasts as supplement), `T5.4` (filter unification), and `T4.1`
  (chrome grading complements the toast layer).
- **Track 15** (ImPlot chassis + hot-edge heatmap) → `T3.4` (minimap reuses the
  plot/`TimelineController` substrate), `T5.2` (CVD-safe heatmap colormap).
- **Track 14 T5** (ImZoomSlider "overview strip" stub) → completed by `T3.4`.
- **`theme.h`** (warn/refuse consolidated) → extended by `T5.1`, then `T4.1`/`T5.2`.
- **`progress.h`** (14 T3) → generalised by `T4.6`.

**New briefs (no track covers these today — cut a `../gui/` doc for each):**
command palette (`T3.1`), shared brushing-and-linking selection (`T1.2`),
router back/forward history (`T3.2`), workspace persistence + recents (`T2.5`),
app-level command/undo stack (`T1.5`), **graded fidelity-chrome + schema
`severity` field** (`T4.1` — coordinate with doc 01's Phase-3 freeze), in-app
term registry / glossary surfacing (`T5.3`), Settings pane + text-scale (`T2.8`),
data-driven outer tabs (`T2.3`), task-language entry rail (`T2.4`).

## Top 10 highest-leverage changes (quick reference)

| # | Change | Wave | Impact | Effort |
|---|---|---|---|---|
| 1 | `T1.1` Wire the keymap; stop the overlay advertising dead keys | 0 | high | med |
| 2 | `T2.1` Convert views to real dockable panes; flatten 3-deep nesting | 1 | high | high |
| 3 | `T2.2` Make Reset real + always-available; retire inert menus | 0 | high | low |
| 4 | `T4.2` Lead capability panel with positives; reuse the remedy map | 0 | high | low |
| 5 | `T2.6` Author output save path + dirty-close guard | 0 | high | low |
| 6 | `T3.2` Bounded back/forward history in the router | 0 | high | low |
| 7 | `T3.1` Command palette (`Ctrl+Shift+P`) over the router | 2 | high | med |
| 8 | `T2.3` Data-driven outer view tabs + lean default set | 1 | high | med |
| 9 | `T4.1` Graded 3-tier fidelity-chrome component system | 3 | high | med |
| 10 | `T5.3` In-app term registry + per-view "?" + legends from the glossary | 3 | high | med |

---

*Findings and method: [desktop-gui-ux-review.md](desktop-gui-ux-review.md).
Parent product plan: [desktop-gui-plan.md](desktop-gui-plan.md).
Implementation briefs: [../gui/](../gui/README.md).*
