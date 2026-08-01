# Desktop GUI — UX heuristic review (Nielsen + supporting sources)

> **ARCHIVED 2026-08-01 — FULLY ACTIONED.** All 24 confirmed findings (F1–F24)
> are resolved. They became the 28 recommendations of
> [../plans/desktop-gui-ux-restructure-plan.md](../plans/desktop-gui-ux-restructure-plan.md),
> which shipped as [../../gui/](../../gui/README.md) briefs **18–24** — all ✅,
> all tasks landed. Nothing here is open; the text below is preserved as
> authored.

> **What this is.** A usability heuristic evaluation of the desktop GUI plan set
> ([desktop-gui-plan.md](../../plans/desktop-gui-plan.md) and its implementation briefs
> [../gui/](../../gui/README.md) 01–17 + schema), judged against Jakob Nielsen /
> NN/g's ten usability heuristics and a set of supporting UX sources
> (Shneiderman, Norman, Tognazzini, ISO 9241-110; expert-tool and profiler-tool
> conventions; dataviz accessibility). It evaluates the **design as planned and
> built** — core docs 01–10 and addon tracks 12–17 are largely merged — for the
> purpose of restructuring. The actionable roadmap lives in the companion
> [desktop-gui-ux-restructure-plan.md](../plans/desktop-gui-ux-restructure-plan.md); this
> doc is the *findings*.
>
> **Method.** Produced by a multi-agent review: 7 grouped close-readers over all
> 20 plan docs + 2 surveys of the implemented app under
> [../../../desktop/src/](../../../../desktop/src/), 3 web-research agents grounding
> the heuristics/sources, 7 evaluator lenses (the ten Nielsen heuristics paired,
> plus cross-cutting lenses for information architecture/onboarding and
> accessibility/ecosystem-fit), then a dedup pass and **adversarial verification**
> of every finding (grounding check, "already addressed elsewhere?" grep,
> persona-impact test, severity recalibration). 43 raw findings → 25 merged →
> **24 confirmed, 1 refuted**. Severity uses the NN/g 0–4 scale (1 cosmetic,
> 2 minor, 3 major, 4 catastrophe). Where a finding hinged on a load-bearing
> structural claim it was re-verified by hand against the code (noted inline).
> Generated 2026-07-27.
>
> **Fidelity culture is respected throughout.** The project's rule — that
> truncation/drop/redaction are schema fields renderers must surface, and that
> misleading renderings are forbidden — is treated as a load-bearing product
> value. No finding recommends hiding truth; where the fidelity layer is
> criticised (F5) the fix is to *structure* it, never to remove it.

## Scoreboard

| Severity | Count | Findings |
|---|---|---|
| **3 — major** | 7 | F1 keymap · F2 docking/workspace · F3 lexicon/legends · F4 tab proliferation · F5 fidelity chrome ungraded · F6 font/DPI · F7 shared selection |
| **2 — minor** | 15 | F8–F22 (command palette, wayfinding, recents, nav history, undo, first-run, color drift, CVD palette, control inconsistency, global find, keyboard reach, capability framing, session-end diagnosis, busy signal, perturbing-mode confirm) |
| **1 — cosmetic** | 2 | F23 "paused" overload · F24 ephemeral Author output |

The pattern under the findings is singular and encouraging: **asm-test already
owns the two hardest parts of good information architecture — one addressable
document format (`.asmtrace`) and one deterministic navigation spine
(`dt_nav_go`) — but ships them hidden behind a stack of closeable tabs drawn
inside a single window.** Almost every severe finding is *wiring or extension on
shipped substrate*, not a redesign. That is why the companion plan is
restructuring, not a rewrite.

## What the design already gets right

The evaluation surfaced 35 genuine strengths. The load-bearing ones, because the
restructuring must preserve them:

- **Fidelity-as-schema-field is excellent and rare.** A fidelity loss
  (truncated/dropped/redacted/torn) is a recording field, not renderer
  discipline ([01](../../gui/01-asmtrace-format.md), plan D7), so it is *testable*
  against committed deliberate low-fidelity fixtures. Refusals are first-class and
  non-collapsible (shared `draw_banner`: refusal ⇒ red full-pane, nothing else
  drawn), and toasts are specified to **supplement, never replace** banners
  ([16](../../gui/16-live-feedback-and-filtering.md):25-27). Status fidelity is
  nuanced, not binary — a `skip` renders as a *successful* session with nothing
  to report ([schema](../../gui/asmtrace-schema.md):543-544), and `--auto` residency
  picks are labelled weaker-evidence than entry picks.
- **One navigation spine.** A single router (`dt_nav_go`,
  [nav.cpp](../../../../desktop/src/nav.cpp)) is the only navigation state; every
  deep link, Diff "go" button, topology drill-in and 3D pick resolves through it
  to the same addressable positions. This is why most nav findings are a thin
  layer away, not a rewrite.
- **The Learn door is strong onboarding substrate** — dependency-free
  "walkthroughs are themselves recordings" ([06](../../gui/06-doors-and-learning.md):48),
  need no root/hardware/attach, work in the render-only viewer, and double as the
  fidelity-chrome test corpus.
- **Progressive disclosure already works in the Observer deck** — its inner tabs
  appear only when the data supports them. The good pattern to copy for the outer
  tab strip is already in the tree.
- **Empty states teach** rather than blank out (a `TextDisabled` sentence naming
  the absence and the recovery action, e.g. "attach a second recording (press d)").
- **`theme.h` consolidated the drifted warn/refuse colours** — the right pattern,
  it just needs extending to the full semantic axis (see F14).
- **Error prevention by capability gating**: disabled doors/backends render their
  verbatim machine reason and never silently downgrade
  ([06](../../gui/06-doors-and-learning.md):407-409); safe two-phase detach cannot
  orphan/crash the target ([07](../../gui/07-serve-live-host.md):254-262).
- **The 3D overview is a textbook-correct answer to Munzner's 3D pitfalls** —
  strictly an orientation surface routing every pick through the 2D deep-link
  router, a one-key top-down/2D fallback, height (not depth/length) encoding, a
  fixed-size 2D HUD. It avoids occlusion-of-quantity and unreadable in-scene text.
- **Convention-faithful time model**: the axis is an ordinal "step" everywhere,
  never faked as wall-clock, because producers record no timestamps
  ([15](../../gui/15-plotting-and-graph-nav.md):107).

---

## Findings — major (severity 3)

### F1 · The keyboard navigation spine is fiction {#f1}
**Heuristics:** visibility of status · consistency & standards · recognition ·
flexibility (accelerators) · help · fidelity culture (D7) · match with real world.
**Where:** [nav.cpp](../../../../desktop/src/nav.cpp):282-296 · shell.cpp:200,705 ·
[04-replay-views.md](../../gui/04-replay-views.md):214-220 ·
[11-imgui-addons.md](../../gui/11-imgui-addons.md):54-56 ·
[17-interaction-testing-and-editor.md](../../gui/17-interaction-testing-and-editor.md):93-97.

For a self-described keyboard-first tool whose core audience is RE/perf experts,
the only shortcut-help surface advertises 12 bindings but only `[` and `]` are
wired. `dt_nav_bindings()` lists `1/2/3/4, j/k, PgUp/Dn, Ctrl+G, Enter, b/f, c,
[/], d, x, n/p, y`; a repo-wide grep for `IsKeyPressed`/`Shortcut`/`ImGuiKey`
returns **only** `LeftBracket`/`RightBracket` (in `scrubber_draw.cpp:13-15`,
`abixray_draw.cpp:128-130`, `loom/fabric_imgui.cpp:240-242`). An expert presses
`1` to switch view or `b` to light a backward cone and nothing happens — an
actively false affordance, and the app's *most navigational surface* is the one
that lies, which cuts against its own fidelity culture. Even once wired, the
vim-flavoured app-invented scheme diverges from gdb/lldb/IDE and Perfetto/Tracy
muscle memory and omits the two most-used profiler gestures (fit-to-selection,
step-to-adjacent-sibling) plus WASD zoom/pan entirely.
**Status:** doc 17 *schedules* wiring + test-engine enforcement but that track is
**not started**; doc 11 itself already labels the keymap "fiction".

### F2 · Docking layout manager and per-mode presets are orphaned {#f2}
**Heuristics:** visibility · control/freedom (reset) · error prevention ·
flexibility/customization · Tognazzini visible navigation.
**Where:** [13-foundation-moves.md](../../gui/13-foundation-moves.md):82-84,124-131,155-177,210-212 ·
layout.cpp:56-71 · shell.cpp:564,578-598.

Doc 13's central restructuring promise — dockable panes with per-mode
perspectives so timeline+scrubber+disasm can finally be shown together — is wired
to nothing. **Re-verified by hand at review time:** `layout.cpp` docks named
windows `kPaneHome/Recording/Scrubber/Inspector/Timeline` via
`DockBuilderDockWindow`, and `shell.cpp:564` opens a `DockSpaceOverViewport`, but
the shell only ever calls `ImGui::Begin("asmtest", …)` (:586) and
`Begin("Keyboard bindings")` (:705) — **no view is `Begin()`'d under any `kPane*`
name anywhere in `desktop/src/`.** So the dockspace and presets act on phantom
windows; every dock/tear/preset/Reset control is inert. The promised workspace IA
does not exist, and no task-shaped workspace or named saved-filter can be
composed/saved/restored. Worse, layout is persisted cross-session and the sole
recovery (`layout_reset`) is orphaned and buried, so a mis-docked or crashed
`.ini` can strand the user across launches (13:82-84 acknowledges the persisted
`.ini` makes an upstream ImGui crash "newly reachable").
**Status:** doc 13 T2 schedules the conversion; its "Done when" (panes shown at
once) is unmet — scheduled but unlanded.

### F3 · No in-app glossary, tooltips, or legends for a dense coined lexicon {#f3}
**Heuristics:** match with real world · recognition rather than recall · help.
**Where:** [04](../../gui/04-replay-views.md):217,366 · [05](../../gui/05-loom-day-one.md):42 ·
[08](../../gui/08-observer-views.md):243-244 · fabric_imgui.cpp:208 ·
`docs/project/glossary` (Sphinx, unsurfaced in-app).

The tool invents a dense private lexicon — Loom, fabric, patch-bay, knot, hollow,
born-untraced, jack, patient-zero, zeroization, worldline, Reweave, terrane —
plus a multi-family colour/mark encoding (dim/hot/neutral, STAT stipple vs exact
tube, cone blue/orange, torn gash, basis rel/abs), and surfaces every term raw
with no in-UI definition. Learners land in the flagship Loom unable to decode
"hollow span / born-untraced / patient zero"; experts already have the words
(def-use lineage, first-divergence, tracer contention) and must translate twice.
**Fidelity chrome the user cannot interpret is not faithful.** A project glossary
already exists in the Sphinx build but is reachable from no pane — and does not
even define the coined GUI terms.
**Status:** not addressed; the glossary exists but is out-of-app and incomplete.

### F4 · All 12 view tabs shown regardless of backing data {#f4}
**Heuristics:** aesthetic/minimalist · match with real world · help · error
prevention · tab-overload IA · progressive disclosure · universal usability.
**Where:** plan:302 · shell.cpp:373-508,603-607 · observer_draw.cpp ·
[11](../../gui/11-imgui-addons.md):38.

Every recording exposes all 12 view tabs at once (Summary, Canvas, Timeline,
Slice, Diff, Observer, Loom, Scrubber, ABI x-ray, 3D overview, Backends, This
host) regardless of whether it carries the events to fill them, with no lean
default — so a minimal-trace or bare-log recording presents Loom/ABI-x-ray/3D/
Scrubber tabs that draw only a "producer absent" placard: the exact "blank
multi-panel IDE" the plan promised to avoid. The strip sits at 13-wide (past the
~7 legibility ceiling) and nests three exclusive tab levels deep, so wayfinding
collapses; the heaviest surfaces (Loom, 3D) front-load for the novice while the
expert wades past teaching tabs. The fix already exists in-tree: the Observer
deck gates its inner tabs on data present.
**Status:** doc 11 condemns the 3-deep nesting; data-driven *outer* tab gating is
planned nowhere.

### F5 · Fidelity chrome is not a designed system — ungraded and proliferating {#f5}
**Heuristics:** aesthetic/minimalist · consistency · status visibility ·
fidelity-chrome-as-system.
**Where:** [04](../../gui/04-replay-views.md):28,548 · [08](../../gui/08-observer-views.md):156 ·
[schema](../../gui/asmtrace-schema.md):543-544 · plan D7:263-264 ·
[10](../../gui/10-spacetime-3d-overview.md):70.

Fidelity is load-bearing and must stay — but the fidelity layer is not a designed
system on two axes at once. **Loudness is ungraded:** a single recording routinely
stacks a redaction placard, a statistical chip, a coarse-provenance chip, a
bounded-window note, an `identity_note` row *and* sometimes a torn banner, all
equally loud and non-collapsible — even though the schema itself grades them very
differently (a `skip` is "a successful session"; a coarse rung/bounded window is
benign; while torn, mixed-basis refusal and dropped-prefix→UNKNOWN are genuine
integrity failures). And the **form proliferates without spec**: banner vs chip vs
inline row vs glyph vs gash, placed differently per view. Both failures bury the
one signal that means "do not trust the tail of this data" among four that mean
"this is normal", so fidelity degrades into noise the user learns to ignore
(banner blindness).
**Status:** `theme.h` consolidated only the two banner *colours*; the schema
grades severity in *data* but no doc proposes a severity/tier field for the
*rendered* chrome.

### F6 · Fixed 15px font, no HiDPI awareness, no text-scale or settings surface {#f6}
**Heuristics:** accessibility · flexibility & efficiency.
**Where:** [11](../../gui/11-imgui-addons.md):50,171 ·
[13](../../gui/13-foundation-moves.md) (fonts) · implemented survey (window
1280×720, 15px hardcoded).

The app hardcodes a 15px font and a 1280×720 window with no HiDPI awareness, no
user text-scale, and no settings surface. On a 4K/scaled display the densest
surfaces (register file, hex memory editor, disassembly, hot-edge table) render as
tiny illegible rows; on low-DPI they are oversized. Low-vision users have no zoom
path. This is the **one accessibility lever fully within ImGui's control** (ImGui
exposes no OS screen-reader tree — a real platform constraint the team recorded
candidly at 11:461), and it is unspent. Persistent every-session cost on the
highest-density views.
**Status:** doc 13 F4 schedules the *dynamic-DPI decision*; user text-scale, a
Settings pane, persisted window size, and a light theme are unplanned anywhere.

### F7 · Selection is one-pane navigation, not shared brushing-and-linking {#f7}
**Heuristics:** recognition rather than recall · Tognazzini anticipation.
**Where:** [04](../../gui/04-replay-views.md) (`dt_nav_go`) · implemented survey
(per-view selection state) · [05](../../gui/05-loom-day-one.md) (loom/canvas/
selected_step separate).

The dataviz convention (Perfetto's linked Current Selection, speedscope's
sandwich) is *one* selection model that cross-highlights the same entity
everywhere it appears. Here each view holds its own selection, so the analyst must
re-find the same address in each pane by hand — the exact recall load
recognition-over-recall exists to remove, and the reason a user "carries an
address seen three panes ago" in their head. It also blocks the anticipation
payoff: selecting a mispredict or taint sink should proactively surface its
disasm/source/lineage in the linked panes.
**Status:** the router jumps *one* pane and cones cross-highlight timeline+canvas
— partial; a shared selection model does not exist.

---

## Findings — minor (severity 2)

### F8 · No command palette or overview/minimap {#f8}
**Heuristics:** recognition · flexibility & efficiency. **Where:**
[16](../../gui/16-live-feedback-and-filtering.md):27-28 · [04](../../gui/04-replay-views.md):178-220 ·
[11](../../gui/11-imgui-addons.md):46,387,455.
With 12 views, doors, a live deck, recordings, and a fully addressable deep-link
space, the expert has no one-keystroke finder to jump to a view/step/offset/
recent-recording/PID/routine, and no overview/minimap to keep global position at
PT scale (10k routines). Experts reach for `Ctrl+Shift+P`/`Ctrl+P` by reflex;
here they hunt tabs and scroll. **Status:** minimap half partially planned (14 T5
ImZoomSlider, "not yet wired"); command palette has zero design across all 20 docs.

### F9 · No persistent "where am I" wayfinding {#f9}
**Heuristics:** status visibility · recognition · flexibility · Tognazzini
visible navigation. **Where:** [04](../../gui/04-replay-views.md):194 · shell.cpp:604-607,667-668.
Deep in a nested tab the analyst cannot answer "which recording/session/step/
filter/thread am I in, and how do I jump elsewhere" without re-hunting by eye.
There is no persistent global context chrome; Home is itself a closeable tab
rather than a persistent shell; two same-named `.asmtrace` files are
indistinguishable. Worsened by the single-window tab model (F2) so sibling views
cannot coexist. **Status:** cheap given `nav.current` exists; unbuilt.

### F10 · No recents / session restore {#f10}
**Heuristics:** recognition · flexibility (session restore). **Where:**
[03](../../gui/03-desktop-shell.md):335-344 · [13](../../gui/13-foundation-moves.md):124-131.
The document model is "a set of recordings the analyst returns to repeatedly", yet
nothing is remembered between launches: no MRU list, no reopen-last-workspace, no
drag-drop. Every start forces recall-and-retype of a file path. **Status:** not
solved anywhere; 13 persists only the dock `.ini`, never the workspace of open
recordings.

### F11 · Deep-link nav spine is one-way — no back/forward {#f11}
**Heuristics:** user control & freedom (emergency exit). **Where:**
[04](../../gui/04-replay-views.md) (D4 `dt_nav_go`).
The whole IA is "arrive at a view from a question" via chained deep links, but
once a user follows failure→recording→step→slice→cone (or drills 3D→2D) there is
no marked exit back — no back button, breadcrumb history, or nav stack. Each jump
replaces context. Nielsen's canonical emergency-exit gap, on every navigation
action. **Status:** `nav` holds only `t.current`; not addressed. (04:218's
"back/forward" is the `[`/`]` generation walk, a different thing.)

### F12 · Undo/redo exists only in the Author editor {#f12}
**Heuristics:** user control & freedom (undo) · Tognazzini reversibility.
**Where:** [17](../../gui/17-interaction-testing-and-editor.md):29 · [05](../../gui/05-loom-day-one.md):393-408.
Undo/redo protects only the Author code editor's source buffer. An analyst who
applies an aggressive Tree filter, rearranges panes, lights the wrong cone, scrubs
the playhead, or forks exploratory Loom "takes" has no reversal path; the takes
gutter accumulates fork nodes with no remove/clear. Exploration is unsafe, pushing
users toward timidity. **Status:** no app-level command/undo stack exists.

### F13 · First run is a jargon "door-chooser" with an inconsistent entry model {#f13}
**Heuristics:** match with real world · consistency · help/onboarding ·
progressive disclosure. **Where:** plan:302,496,14 · [06](../../gui/06-doors-and-learning.md):48 ·
shell.cpp:170-201.
First run hands the explicitly-courted Learner a bespoke metaphor — "choose a
door" — and four peer options, two of which (Author, Inspect) and the caption
vocabulary are meaningless before first value. The promised "arrive from a
question / no blank IDE" becomes "decode our IA taxonomy first". The entry model
is also inconsistent: doors open as closeable ✕ tabs, a recording is a tab, and
Home is itself a tab with no persistent home/nav rail; "door"/"tab"/"mode"/
"preset" name overlapping concepts. (The plan says *three* doors; the build
renders *four* peer buttons — a doc-vs-build drift.) **Status:** no auto-land /
recommended-path scheduled.

### F14 · Semantic colours re-invented per pane {#f14}
**Heuristics:** consistency (colour coding must mean one thing everywhere).
**Where:** [theme.h](../../../../desktop/src/ui/theme.h) · inspect_door.cpp:91-93 ·
scrubber_draw.cpp:66 · abixray_draw.cpp:18,22 · slice_view_draw.cpp:18-24 ·
scene3d/hud.cpp.
`theme.h` consolidates only two colours (warn/refuse), so the good/bad/maybe/
changed/cone axis was never centralized and is drifting exactly like the amber
drift `theme.h` claims to have ended: three barely-distinguishable yellows mean
three different things (changed / truncated-statistical / weak-evidence) across
scrubber vs ABI x-ray vs a truncation banner, and two reds (0.90 vs 0.95) already
encode the same "bad/refused". **Status:** not addressed.

### F15 · No colour-blind-safe palette {#f15}
**Heuristics:** accessibility · consistency. **Where:** theme.h ·
[04](../../gui/04-replay-views.md) cone hues · [05](../../gui/05-loom-day-one.md) dim/hot ·
[15](../../gui/15-plotting-and-graph-nav.md):218 heatmap.
~5% of users (mostly red-green CVD) cannot reliably distinguish back-cone from
forward-cone, dim from hot, or a refused row's red from an ordinary one, because
those distinctions ride on colour alone. For a teaching tool this silently
excludes learners; for the expert it corrupts the core slice/diff reading task;
and it undercuts fidelity — a deuteranope could read a statistical/truncated
distinction that collapsed onto colour as exact. **Status:** the good pattern
already exists (statistical-vs-exact = solid tube vs translucent stipple, a real
second channel) — generalise it.

### F16 · Filter, sort, and time-position controls differ across every view {#f16}
**Heuristics:** consistency · match with real world · recognition. **Where:**
[15](../../gui/15-plotting-and-graph-nav.md):107 · implemented survey.
Filtering uses three unrelated idioms (ImSearch on Learn only, engine-side text
inputs on Tree, a combo on Backends) while core Canvas/Timeline/Observer tables
have no filter or sort at all despite ImGui table sort being free; and the
"move through time" control varies (SliderInt in scrubber/abixray/loom/3D-HUD,
prev/next in Invocations, InputInt in Disassembly), so the muscle action for the
single most common trace operation changes per view. The Invocations prev/next is
a *deliberate* fidelity choice (discrete snapshots) but is not visually
distinguished as such. **Status:** unified widget scheduled nowhere.

### F17 · No global find / search-as-measurement {#f17}
**Heuristics:** help · flexibility & efficiency. **Where:**
[16](../../gui/16-live-feedback-and-filtering.md) (ImSearch, Learn only) · [11](../../gui/11-imgui-addons.md):387.
An expert cannot answer "where and how much does mnemonic/address/symbol X occur"
without manual scrolling, and the teaching persona loses the search-as-measurement
affordance that turns a find into a lesson about aggregate cost. Client-side
search exists only on the Learn cards; there is no highlight-all, match count,
aggregate cost, or Enter/Shift+Enter cycling. **Status:** not planned.

### F18 · 3D camera and hand-rolled views are mouse-only {#f18}
**Heuristics:** accessibility · flexibility & efficiency. **Where:**
[10](../../gui/10-spacetime-3d-overview.md):346-348,581 · implemented survey.
Because ImGui exposes no OS screen-reader tree, keyboard operability is the only
accessibility substitute — yet the 3D overview, the slice DAG, and the Loom canvas
are mouse-only islands: the 3D camera has no keyboard scheme even designed, the
`SceneHost` is not Tab-reachable, and the slice explorer draws cones from
selection state no key sets. A keyboard-only analyst cannot orient in 3D or light a
slice cone at all. Narrower than the other gaps (3D is supplementary; drill-out is
mouse-reachable). **Status:** ties to F1; unwired.

### F19 · Capability/refusal surface leads with negatives and renders verbatim errno {#f19}
**Heuristics:** status visibility · minimalist · diagnose & recover · help.
**Where:** [06](../../gui/06-doors-and-learning.md):53,397-409 · [07](../../gui/07-serve-live-host.md):341-345.
The fidelity is correct but the refusal surface fails twice. **Framing:** on a
common host (`perf_event_paranoid=4`, no PT/IBS silicon) the capability panel is
mostly red greyed rows of low-level errno text, leading with what is broken — a
learner reads catastrophe and concludes the tool does not work on their machine,
when the entire Learn ladder and Author door need none of those capabilities.
**Action:** the verbatim machine reason *is* the whole message, so a user faced
with "ENOSPC" or "perf_event_paranoid=4" gets a precise diagnosis with no offered
next step. **Status:** `inspect_door` already implements the exact
recognized-error→remedy map (paranoid/Yama/i386/CAP_SYS_PTRACE); the capability
panel does not reuse it.

### F20 · Live session termination causes are collapsed and end-of-stream chrome is silent {#f20}
**Heuristics:** status visibility · diagnose & recover. **Where:**
[07](../../gui/07-serve-live-host.md):277-278,292-293 · [16](../../gui/16-live-feedback-and-filtering.md):26-27 ·
desktop/README.md:685-713.
At the moment streaming stops the user cannot tell *why* the session ended — clean
detach, a wrong/old `asmspy` binary (the "overwhelmingly common cause", which
prints a usage banner and exits 0), a mid-capture crash, a torn drop, or an
EOF/quit — because all collapse into a single "ended" state, and the
differentiating chrome is renderer-discretionary and possibly silent. The three
termination modes carry different trust for the partial data on screen, yet
diagnosis is offloaded to an external README essay. **Status:** doc 16 T1
schedules distinct exit toasts; the persistent in-pane placard distinction is not
yet built.

### F21 · No uniform busy signal for long operations {#f21}
**Heuristics:** status visibility. **Where:** [04](../../gui/04-replay-views.md):553 ·
[10](../../gui/10-spacetime-3d-overview.md) · [14](../../gui/14-quick-wins.md):162-164.
A stuck PT decode or terrain rebuild that exceeds a frame budget freezes the UI
thread with no consistent "working" indicator: an expert cannot tell the tool
apart from a hang and may kill it. **Status:** doc 14 T3 shipped a shared faithful
`progress.h` helper for file loads + live sessions — generalise it to *every*
long op (PT decode, symbol/codeimage load, terrain rebuild).

### F22 · Perturbing / target-killing single-step modes have no pre-commit confirm {#f22}
**Heuristics:** error prevention. **Where:** [08](../../gui/08-observer-views.md):29-33,177 ·
[07](../../gui/07-serve-live-host.md).
A user can select a perturbing single-step observer/region mode and hit Start with
no warning that it dirties the traced page and perturbs timing, and on arm64 no
warning that it can kill a running target blocked in a syscall — a hazard that
survives DETACH and teardown cannot undo. The "crawl warning" teaches cost only
after you are already crawling. **Status:** a second-confirm pattern exists in-tree
(reveal-all, 08:123) but is not applied to mode arming.

---

## Findings — cosmetic (severity 1)

### F23 · "paused" is overloaded — operator pause vs budget preemption {#f23}
**Heuristics:** status visibility · recover from unwanted state · control/freedom
· error prevention. **Where:** [07](../../gui/07-serve-live-host.md):320-323,261 ·
[schema](../../gui/asmtrace-schema.md):476,638-640.
The bare word "paused" names two states with disjoint recoveries: an operator
pause (resume to continue) and a concurrency-budget preemption (another live view
holds the ptrace jack — must swap/queue/stop the blocker). Neither recovery is
offered at the state, and the queue-vs-swap fork's consequences are unspecified.
**Status:** 07 already makes swap/queue explicit user actions; the labelling and
in-state recovery affordances are the gap.

### F24 · Author-produced recording is ephemeral {#f24}
**Heuristics:** error prevention (dangerous default). **Where:**
[06](../../gui/06-doors-and-learning.md):373 · [14](../../gui/14-quick-wins.md):350 ·
inspect_door.cpp (`draw_save_capture`).
A user can open the Author door, assemble and run a routine, explore the trace,
then close the tab and lose the recording irretrievably — no "unsaved work"
confirmation and no Save for Author output. `workspace.close()` only erases the
in-memory vector entry. For a teaching-and-authoring tool this is a
high-frequency data-loss trap. **Status:** a confirm-overwrite `ImGuiFileDialog`
already exists for live capture — reuse it.

---

## The one refuted finding (recorded for completeness)

- **"3D scene can occlude the integrity marks that must stay visible."** *Refuted.*
  The premise — that `torn` is only an occludable in-scene red gash with no
  fixed-screen guarantee before drill-in — is factually wrong: the built HUD
  already carries a persistent screen-fixed torn indicator
  ([10-spacetime-3d-overview.md](../../gui/10-spacetime-3d-overview.md)). The fidelity
  invariant holds in 3D.

---

## Sources

**Heuristics & method (NN/g).**
[Ten Usability Heuristics](https://www.nngroup.com/articles/ten-usability-heuristics/) ·
[How to Conduct a Heuristic Evaluation](https://www.nngroup.com/articles/how-to-conduct-a-heuristic-evaluation/) ·
[How to Rate the Severity of Usability Problems](https://www.nngroup.com/articles/how-to-rate-the-severity-of-usability-problems/).

**Other principle sets.**
[Shneiderman's Eight Golden Rules](https://capian.co/shneiderman-eight-golden-rules-interface-design) ·
[Applying the 8 Golden Rules (UXmatters)](https://www.uxmatters.com/mt/archives/2022/10/applying-the-8-golden-rules-of-user-interface-design.php) ·
[Tognazzini, First Principles of Interaction Design](https://asktog.com/atc/principles-of-interaction-design/) ·
[ISO 9241-110:2020 Interaction principles](https://www.iso.org/obp/ui/#iso:std:iso:9241:-110:ed-2:v1:en).

**Expert-tool & IA patterns.**
[Command Palette pattern](https://uxpatterns.dev/patterns/advanced/command-palette) ·
[VS Code keybindings / Command Palette](https://code.visualstudio.com/docs/configure/keybindings) ·
[Progressive disclosure (UXPin)](https://www.uxpin.com/studio/blog/what-is-progressive-disclosure/) ·
[Tab UI: when tabs work and fail (Setproduct)](https://www.setproduct.com/blog/tabs-ui-design) ·
[Blender Workspace presets](https://blender-addons.org/workspace-tools/).

**Profiler/trace-tool conventions.**
[Perfetto UI (WASD, F fit, pin, Ctrl+P, area select)](https://perfetto.dev/docs/visualization/perfetto-ui) ·
[speedscope (minimap, sandwich, Ctrl+F cycle)](https://jamie-wong.com/post/speedscope/) ·
[Tracy timeline profiler (WASD, find-zone stats)](https://wiki.thedarkmod.com/index.php?title=Tracy%3A_timeline_profiler) ·
[Grafana flame-graph sandwich view](https://grafana.com/blog/2022/12/13/flame-graph-sandwich-view-mode-what-it-is-and-how-to-use-it/).

**3D infovis pitfalls & dataviz accessibility.**
[Munzner, Process and Pitfalls in Writing InfoVis Papers (2D good / 3D better)](https://www.cs.ubc.ca/labs/imager/tr/2008/pitfalls/pitfalls.pdf) ·
[Occlusion Management for 3D Networks](https://dl.acm.org/doi/10.1145/3356422.3356445) ·
[Colorblind-friendly palettes (CVD prevalence)](https://www.audioeye.com/post/colorblind-friendly-palettes/) ·
[Color-blindness palette simulator](https://colorblindsimulator.app/check-color-palette-for-color-blindness) ·
[WCAG contrast (4.5:1 text / 3:1 graphics)](https://venngage.com/tools/accessible-color-palette-generator).
