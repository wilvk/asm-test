# Desktop GUI implementation documents — index

This directory holds the **implementation-ready specifications** for
[desktop-gui-plan.md](../plans/desktop-gui-plan.md). It began as ten
self-contained briefs (nine core + one growth-rung companion) and has since
grown to **59 numbered docs** across six families — core 01–10, addon adoption
11–17, UX restructure 18–24, live/extension 25–42, and the three 3D families
43–59 (representation 43–44, instrument 46–52, depiction 53–59).
Each is written for one coherent task set, so a junior developer can clone the
repo, open exactly one document, and implement it end to end (code + tests +
docs) with no other context. Format and
rules follow [../implementations/](../implementations/README.md) — read
[\_conventions.md](../implementations/_conventions.md) once before starting.

> **Archived 2026-08-01 — this directory now holds only the docs with open
> work.** Per the [archive rule](../README.md), the **43 briefs whose tasks are
> all complete** moved to [`../archive/gui/`](../archive/gui/) — that is the
> whole numbered family 01–37, 39–42, 44, 45, 49 and 54, every one of them ✅/☑
> with all tasks landed. The tables and prose below still describe every doc, with
> the completed ones linked into the archive, so this file remains the single
> inventory and dependency map for the family. **The documents below stay here**,
> because each still points at unbuilt work (updated 2026-08-02 with the
> instrument family 46–52 and the depiction family 53–59; 49 and 54 landed the
> same day and moved to the archive):
>
> | Doc | Why it stays |
> |---|---|
> | [38-live-feed-completion-roadmap.md](38-live-feed-completion-roadmap.md) | gaps **L1–L6** are still open (arm64 live-dataflow leg, `df_invocation`-aware decode, live `blame`/`statediff` kinds, the Darwin `libasmtest_dataflow` build, ARM32/RISC-V Author guests, doc-37 T4 on the serve disasm path). L7 closed as doc 39. |
> | [43-faithful-city-roadmap.md](43-faithful-city-roadmap.md) | Phase A landed as doc 44; **Phases B–E are not yet cut** into briefs. |
> | [46-3d-functional-roadmap.md](46-3d-functional-roadmap.md) | the 3D **instrument** axis (what you can *do* with the scene), sibling to 43's representation axis. Cuts docs 47–52; 47 and 48 landed 2026-08-02, 50–52 ☐ not started. |
> | [50-two-way-brushing.md](50-two-way-brushing.md) | ☐ 0/4 — light the scene from the flat views *through the address*; closes 44's deferred resolver and the reverse-direction ordinal. |
> | [51-scene-focus-and-scale.md](51-scene-focus-and-scale.md) | ☐ 0/4 — per-tid/region/kind focus, thread ghosting, a camera-distance entity budget. |
> | [52-flat-terrain-surface.md](52-flat-terrain-surface.md) | ☐ 0/4 — a GL-free 2D terrain surface: the no-GL branches gain a real view and the GL path gains a reading mode. |
> | [53-3d-catalog-build-roadmap.md](53-3d-catalog-build-roadmap.md) | the 3D **depiction** axis (which quantities the scene draws) — the 26-graph catalog cut into phases. Phase 0 (54) landed 2026-08-02; docs 55–59 all ☐ not started. |
> | [55-scene-render-quality.md](55-scene-render-quality.md) | ☐ 0/6 — eye-dome lighting, depth-dependent halos, contour bands + height key, order-independent translucency, MSAA, portable line width. |
> | [56-fidelity-and-module-layers.md](56-fidelity-and-module-layers.md) | ☐ 0/5 — the layer registry, confidence terrain, per-module skyline, opcode-class terrain, misprediction survey layer. |
> | [57-causal-layers.md](57-causal-layers.md) | ☐ 0/5 — one step→place resolver, kernel crossing spurs, taint isochrone, blame convergence forest, dominant-path ridge. |
> | [58-memory-data-cell-family.md](58-memory-data-cell-family.md) | ☐ 0/6 — read/write twin relief, working-set tide, observed-lifetime pillars, data-access ribbon, residency sediment columns. |
> | [59-standalone-scenes.md](59-standalone-scenes.md) | ☐ 0/5 — a scene host that is not the address plane, then divergence worldline, invocation stack, module excursion ribbon, SIMD lane prism. |
> | [asmtrace-schema.md](asmtrace-schema.md) | the live `.asmtrace` schema reference — not a brief, and still the normative format doc. |

> **Provenance.** Generated 2026-07-23/24 from the GUI plan (itself
> cross-reviewed against HEAD `a460d40` the same day), with every cited
> file:line re-verified against the tree at authoring time. Docs 01–06 went
> through independent factual + implementability review during authoring;
> 07–09 were authored against the same charters with the citations verified
> inline. If a claim disagrees with the code when you implement, the code
> wins — re-verify, then fix the doc in the same change. (Docs 02, 03 and 04
> each carry a dated banner recording exactly which of their claims lost to the
> code; 04's fourth correction — **the v1 schema has no routine-identity
> field** — is an open Phase-3-freeze item for 01, not a settled one.)

## Binding shared decisions (D1–D11)

Every doc conforms to these; they are stated once here.

- **D1 — layout.** All GUI code lives under `desktop/`: `desktop/src/`
  (C++17), `desktop/test/` (headless tests), `desktop/README.md`. Third-party
  code is fetched into `build/`, never committed.
- **D2 — pinned deps.** Dear ImGui via `scripts/fetch-imgui.sh` (pinned
  release, SHA-256 in `scripts/third-party-digests.txt`, MIT license vendored
  under `licenses/`); nlohmann/json single header the same way. App backends:
  GLFW + OpenGL3 (`libglfw3-dev` in `Dockerfile.desktop`); headless tests use
  the ImGui `example_null` pattern.

  **Amended 2026-07-26 (addon-admission rule, from [11-imgui-addons.md](../archive/gui/11-imgui-addons.md)).**
  D2 no longer means "no dep beyond imgui + nlohmann/json"; it means every new
  third-party UI dep clears this bar, so the tree stays permissively licensed,
  reproducible, and faithful — no ad-hoc exceptions:
  1. **License.** MIT/zlib/BSD-class only, captured under `licenses/` the way
     Capstone/DearImGui/DynamoRIO already are. The one standing carve-out is
     `imgui_test_engine` (its own Test-Engine-License): fetch-at-build,
     test builds only, never vendored.
  2. **Pin + digest.** A pinned tarball with a SHA-256 row in
     `scripts/third-party-digests.txt`, fetched by a `fetch-<name>.sh` that
     follows `scripts/fetch-imgui.sh` (release tags where they exist,
     commit-sha tarballs where they don't).
  3. **Compile-check gate.** Any addon that includes `imgui_internal.h` is
     rebuilt on every imgui repin before it lands — wired into the fetch path.
  4. **View-model purity preserved.** Addons are draw-half chassis only; the
     pure models and their golden-text surfaces stay the source of truth (D7).
  5. **Fidelity is a selection filter.** Nothing that renders survey data as
     stacks, imposes force-directed layout, or hides refusals is admissible.

  Implementation briefs for the addons themselves are cut from doc 11's tracks
  under this rule; the in-tree quick wins that add no dependency (the shared
  fidelity-chrome palette `desktop/src/ui/theme.h`, the diff "go" routing fix)
  need only D7/D4, not a new dep, and have already landed.
- **D3 — Makefile.** A new `mk/desktop.mk`, included from the top-level
  Makefile right after `mk/cli.mk`, owns `desktop`, `desktop-render`,
  `desktop-test`, `docker-desktop` (rule mirrors `mk/cli.mk`'s docker rule);
  new user-visible targets get `make help` echo lines. Shared property:
  additive rules only; whichever doc lands first creates it.
- **D4 — licensing.** The full app links bundled Unicorn/Keystone → GPL-2.0
  as a whole; `desktop-render` builds with **zero engine deps** and stays
  permissively distributable. Pin/SDE/libdft64 are never bundled.
- **D5 — schema home.** The `.asmtrace` draft schema lives at
  [asmtrace-schema.md](asmtrace-schema.md) (owned by 01, append-only for
  other docs) until the Phase-3 freeze.
- **D6 — golden corpus.** `tests/golden-asmtrace/`, regenerated
  deterministically by `make asmtrace-golden` from the conformance corpus,
  committed, byte-stable.
- **D7 — fidelity is tested.** The corpus includes low-fidelity fixtures
  (truncated/dropped/redacted); renderer tests assert banners, provenance
  chrome, and redaction defaults.
- **D8 — style.** The repo `.clang-format` (Language: Cpp) covers desktop/
  C++; `fmt-check` stays informational.
- **D9 — capture host.** Observer capture is the `asmspy` binary via
  `--serve` (local subprocess or ssh); the desktop app never links the ptrace
  engines. Author mode links the C library tiers (emulator/valtrace/assemble).
- **D10 — offline disasm.** Producers may attach a `disasm` string to
  trace/dataflow events at record time so the render-only viewer needs no
  Capstone; absence degrades to offsets.
- **D11 — naming.** The nine docs and their cross-reference names are exactly
  the filenames below.

## Claiming work (parallel agents)

Every doc table carries a **Claim** column so several agents can work this family
at once without colliding. The column is the only coordination channel — there is
no lock server; the git history is the arbiter.

- **Claim before you edit code.** Write your claim into the row, commit *that
  edit alone*, and push. If the push rejects, `git pull --rebase` and re-read the
  cell: someone claimed it first, so pick another row. Winning the push is what
  makes the claim real.
- **Format:** `<who> · <task ids> · <YYYY-MM-DD>` — e.g.
  `agent-a · T1–T2 · 2026-07-28`. Task-level granularity is expected; two agents
  may share a doc if their tasks are independent, so list the tasks, never just
  the doc. Separate multiple claims on one row with `;`.
- **`*free*`** means unclaimed and open. **`—`** means nothing left to claim
  (doc complete, or not a brief).
- **Release when you land or stop.** On landing, update **Status** and set Claim
  back to `*free*` (or `—` when the doc is complete). If you abandon a claim,
  clear it in the same commit as your last push — a stale claim blocks others,
  and any claim older than a day with no commits touching it is fair game to take
  over.
- **Respect *Depends on*.** A row whose dependency is unlanded is not claimable
  yet, even if the Claim cell reads `*free*`; the docs 28–33 order in the
  sequencing note below is the intended parallel schedule.

## The documents

**Status** tracks task completion as `done/total`; update the cell as tasks
land (legend as in [../implementations/README.md](../implementations/README.md):
☐ · ◐ · ☑ · ✅).

**Claim** is the parallel-work lane — see
[Claiming work (parallel agents)](#claiming-work-parallel-agents) below. `—` means
nothing left to claim; *free* means the row is open.

| Doc | Area | Tasks | Depends on | Status | Claim |
|---|---|---|---|---|---|
| [01-asmtrace-format.md](../archive/gui/01-asmtrace-format.md) | `.asmtrace` schema, record modes, golden corpus | 8 | — | ✅ 8/8 | — |
| [02-exporters-and-readers.md](../archive/gui/02-exporters-and-readers.md) | speedscope/Perfetto exporters, completeness readers | 6 | 01 (03 for T5–T6) | ✅ 6/6 | — |
| [03-desktop-shell.md](../archive/gui/03-desktop-shell.md) | desktop/ skeleton, deps, mk/desktop.mk, document model | 8 | 01 (corpus, for T7) | ✅ 8/8 | — |
| [04-replay-views.md](../archive/gui/04-replay-views.md) | canvas, operand timeline, slice explorer, diff, deep links | 8 | 01, 03 | ✅ 8/8 | — |
| [05-loom-day-one.md](../archive/gui/05-loom-day-one.md) | the Loom fabric, lineage, lane annex, forks | 7 | 02 (reader), 03, 04 | ✅ 7/7 | — |
| [06-doors-and-learning.md](../archive/gui/06-doors-and-learning.md) | Learn/Author doors, ct_eq, capability panel, runner record mode | 7 | 01–04 | ✅ 7/7 | — |
| [07-serve-live-host.md](../archive/gui/07-serve-live-host.md) | extract `libasmspy`, `--serve` wrapper, session host, budget patch-bay, Inspect door | 7 | 01, 03 | ✅ 7/7 | — |
| [08-observer-views.md](../archive/gui/08-observer-views.md) | live views: syscalls, watch, topo, hot edges, tree filters, codeimage, PT slice | 8 | 07, 04, 01 | ✅ 8/8 | — |
| [09-teaching-producers.md](../archive/gui/09-teaching-producers.md) | per-step register ring, scrubber, ABI x-ray, blame socket | 5 | 01, 03, 04, 06 | ☑ 5/5 | — |
| [10-spacetime-3d-overview.md](../archive/gui/10-spacetime-3d-overview.md) | 3D memory-terrain + execution-trajectory overview surface (**growth-rung companion**) | 7 | 01, 03, 04, 07, 08 | ☑ 7/7 | — |
| [11-imgui-addons.md](../archive/gui/11-imgui-addons.md) | Dear ImGui addon research + adoption plan (**research/planning doc, not a brief**) — D2 amended (addon-admission rule, above); Track G + 2 no-dep quick wins landed | — | 01–10 (survey) | planning · G done | — |

### Addon-adoption family (docs 12–17) — implementation briefs cut from doc 11

These six briefs are the implementation-ready form of
[11-imgui-addons.md](../archive/gui/11-imgui-addons.md), one per track in that doc's
*Sequencing* block, each written so a junior developer can open exactly one and
implement it end to end. **Every one is gated on
[12-addon-supply-chain.md](../archive/gui/12-addon-supply-chain.md)**, which amends D2 with the
addon-admission rule (above) and builds the shared fetch/pin/license/compile
scaffolding — land 12 first, then the rest parallelise as the *Depends on* column
shows. Status reflects committed `main`; the D2 amendment (12-T1) and the two
no-dependency quick wins (14-T1 diff-fix, 14-T2 `theme.h`) are already in flight
per the doc-11 row above.

| Doc | Area | Tasks | Depends on | Status | Claim |
|---|---|---|---|---|---|
| [12-addon-supply-chain.md](../archive/gui/12-addon-supply-chain.md) | amend D2 (addon-admission rule); reusable `fetch-addon.sh` + digest/license conventions; imgui-repin compile-gate (**Track G — blocks all**) | 3 | 03 | ☑ 3/3 | — |
| [13-foundation-moves.md](../archive/gui/13-foundation-moves.md) | F1 docking repin (`v1.91.9b-docking`) + in-tree layout manager; F2 32-bit `ImDrawIdx`; F3 freetype + JetBrains Mono + Codicons; F4 the 1.92 bump decision (**Track F**) | 5 | 12; 04, 09, 10 | ☑ 5/5 | — |
| [14-quick-wins.md](../archive/gui/14-quick-wins.md) | diff "go" bug fix; shared theme header; ProgressBar; imgui_memory_editor; ImZoomSlider; ImGuiTextSelect (v1.1.6+utfcpp); ImGuiFileDialog (v0.6.8) (**Track Q**) | 7 | 12 (T4–T7); 04/05/08 | ☑ 7/7 | — |
| [15-plotting-and-graph-nav.md](../archive/gui/15-plotting-and-graph-nav.md) | ImPlot v1.0 chassis (perf_history/hotedges/timeline/watch); imgui_canvas de-risk; imgui-node-editor for topo/tree | 3 | 12, 13 (F1/F2) | ✅ 3/3 (T1 plots ☑ + T2 canvas ☑ + T3 node-editor topo/tree/hot-edges ☑) | — |
| [16-live-feedback-and-filtering.md](../archive/gui/16-live-feedback-and-filtering.md) | ImGuiNotify toasts (live-session events); ImSearch client-side filtering (**both ∅-unverified in doc 11 — compile-check at pin**) | 2 | 12; 13 F3 (Notify) | ✅ 2/2 (T1 toasts `1a9d6d5` + T2 filter `a728d9d`) | — |
| [17-interaction-testing-and-editor.md](../archive/gui/17-interaction-testing-and-editor.md) | imgui_test_engine (interaction tests + keymap enforcement, test-lane-only); goossens ImGuiColorTextEdit + TextDiff (Author editor, disasm gutter, side-by-side diff) (**Bigger bets**) | 2 | 12 | ✅ 2/2 (T2 editor `fdb5783` + T1 engine/keymap `245ad3f`/`243f092`) | — |

22 tasks across docs 12–17 — **all 22 landed** (the last was 15 T3,
imgui-node-editor for topo/tree/hotedges pan-zoom + fit-graph). Sequencing (doc 11's tracks): **12 (Track G) first**,
then Track Q (14) and Track F (13) in parallel, then the plotting/graph chassis
(15) and feedback/filtering (16) on the foundations, with the bigger bets (17)
independent of each other at any time after 12. Every addon lands with its
`fetch-<name>.sh` wrapper, a `scripts/third-party-digests.txt` row, a `licenses/`
capture, and — for the `imgui_internal.h` dependents — an entry in the doc-12
compile-probe.

### UX-restructure family (docs 18–24) — implementation briefs cut from the review

These seven briefs are the implementation-ready form of the
[UX restructuring plan](../archive/plans/desktop-gui-ux-restructure-plan.md) (which
follows the 24-finding [heuristic review](../archive/reviews/desktop-gui-ux-review.md)),
one per coherent recommendation cluster, each written so a junior developer can
open exactly one and implement it end to end. They were drafted 2026-07-27
against HEAD `243f092` with every cited file:line re-verified against the tree
(the review's own citations predate doc 17 T1, which shifted `shell.cpp` ~200
lines and added `handle_keymap`; the briefs cite current lines and flag the
drifts). This began as a drafting/planning pass; implementation has since
started — **doc 24 (one visual language) landed complete 2026-07-27** (T1 the
semantic palette, then T2 CVD-safe + second channel, T3 the glossary-sourced term
registry, T4 the unified filter/time widgets, T5 the Loom/3D first-open primer),
and the rest is future work. Recommendation IDs (`T<theme>.<n>`) and finding IDs (`F<n>`)
map to the plan and review.

| Doc | Wave | Recommendations (findings) | Tasks | Depends on | Status | Claim |
|---|---|---|---|---|---|---|
| [18-breach-stops.md](../archive/gui/18-breach-stops.md) | 0 | keymap fidelity-remainder + convention keys, real Reset, Author save-guard, capability positives, perturb-confirm, nav back/forward (F1,F18,F2,F24,F19,F22,F11) | 6 | 12 (test-engine, for tests); mostly independent of 19 | ✅ 6/6 | — |
| [19-dockable-panes-keystone.md](../archive/gui/19-dockable-panes-keystone.md) | 1 | real `kPane*` panes + flatten 3-deep nesting — **the keystone**, unblocks 20/21/22 (F2,F4,F9) | 3 | 13 F1 (docking, landed); 18 T2.2 alongside | ✅ 3/3 | — |
| [20-workspace-and-settings.md](../archive/gui/20-workspace-and-settings.md) | 1 | data-driven tabs, task-language entry rail, workspace persistence + recents, perspectives, Settings/DPI/text-scale (F4,F13,F10,F16,F6) | 5 | 19 (panes); 13 F3 (fonts, for Settings) | ✅ 5/5 | — |
| [21-spine-navigation.md](../archive/gui/21-spine-navigation.md) | 2 | command palette, wayfinding breadcrumb, overview/minimap (F8,F9) | 3 | 19 (panes); 15 + 14 T5 (minimap); 16 (ImSearch) | ✅ 3/3 | — |
| [22-selection-and-search.md](../archive/gui/22-selection-and-search.md) | 2 | shared brushing-and-linking selection, keyboard islands, global find, app-level undo (F7,F18,F17,F12) | 4 | 19 (panes); 16 (ImSearch); 21 T3 (minimap) | ✅ 4/4 (T1–T4 landed 2026-07-27) | — |
| [23-graded-truth-layer.md](../archive/gui/23-graded-truth-layer.md) | 3 | graded 3-tier fidelity chrome + schema `severity`, session-end placard, split "paused", progress everywhere (F5,F20,F23,F21) | 4 | 01 (**schema-freeze coordination for T1**); 24 T5.1 (palette); 16 T1; 14 T3 | ✅ 4/4 (T1–T4 landed 2026-07-27) | — |
| [24-one-visual-language.md](../archive/gui/24-one-visual-language.md) | 3 | semantic palette (extend `theme.h`), CVD-safe + second channel, glossary/term registry, unified filter/time, Loom/3D primer (F3,F14,F15,F16,F4) | 5 | 16 (ImSearch); 15 (ImPlot colormap); T5.1 precedes 23 T1 | ✅ 5/5 (T1–T5 landed 2026-07-27) | — |
| [25-live-model-wiring.md](../archive/gui/25-live-model-wiring.md) | 4 | promote the growing capture into the workspace model so Loom / Slice / Timeline / 3D go live, not just Observer — closes the 2026-07-27 live-vs-replay audit gap; Scrubber was replay-only until doc 26 landed the live `regstate` producer | 7 | 20 T1 (`view_presence`); 07/08 (live host + observer deck) | ✅ 7/7 (T6 completed 2026-07-28: the live single-step 3D overlay — `build_trajectories` weaves the `df_step` offset stream as a region-relative, per-tid path when no `trace` is present) | — |
| [26-live-regstate-producer.md](../archive/gui/26-live-regstate-producer.md) | 4 | live `regstate` producer on the serve/`--dataflow` single-step engine (it already `PTRACE_GETREGS` every step) → the Scrubber goes live; the last live-vs-replay gap. Consumer already done (doc 25); producer + serve opt-in + emulator parity | 5 | 25 (consumer wiring); 07 (serve); the `--dataflow` ptrace engine | ✅ 5/5 (T1–T5 landed 2026-07-28; `--dataflow --steps` + serve `steps:true` arm the `user_regs@x86_64/sysv` ring; emulator-parity green) | — |

30 tasks across docs 18–24. Sequencing follows the plan's four waves:
**Wave 0 (doc 18)** stops active breaches and is mostly independent of the
docking refactor — ship it first. **Wave 1 (docs 19, 20)** is the structural
unlock: doc 19 (the keystone) converts the orphaned `kPane*` docking into real
panes and unblocks the rest; land doc 18's real-Reset alongside so a bad `.ini`
cannot strand the user. **Wave 2 (docs 21, 22)** exposes and operates the
`dt_nav_go` spine on the real panes. **Wave 3 (docs 23, 24)** does the systematic
truth-layer + visual-language depth; **doc 24 T5.1 (semantic palette) lands
before doc 23 T1 (fidelity tiers)**, and doc 23 T1's `severity` field is a schema
change that must coordinate with doc 01's Phase-3 freeze (D5). Nothing here
removes a truth — the fidelity chrome is *restructured* (F5), never removed.

### Extension family (docs 27–33) — unblocking the deferred views

The ~30 DEFERRED / BLOCKED / REFUSED markers across docs 01–26 and the plan's
Acknowledged-limits / Killed-in-grounding lists collapse onto **six root
prerequisites** — one engine or schema change apiece, each fanning out to
several stuck views. [27-extension-roadmap.md](../archive/gui/27-extension-roadmap.md) is the
family overview (mapping + dependency graph + sequencing, like doc 11 for its
addon family); docs 28–33 are the full per-root briefs. Authored 2026-07-28
against HEAD `da566c9`; **not yet implemented** (planning). Excludes what already
shipped (live `regstate` doc 26, `severity` doc 23, offline scrubber doc 09).

| Doc | Root | Prerequisite | Tasks | Depends on | Status | Claim |
|---|---|---|---|---|---|---|
| [27-extension-roadmap.md](../archive/gui/27-extension-roadmap.md) | — | family overview + dependency graph (**not a brief**) | — | 01–26 (survey) | overview | — |
| [28-schema-freeze-completion.md](../archive/gui/28-schema-freeze-completion.md) | R1 | `code` header (routine hash), footer `steps_total`, serialize `wide[]` | 3 | 01 (**Phase-3 freeze, D5**) | ✅ 3/3 (T1 `5803086` + T2 `47c3280` + T3 `878de40`) | — |
| [29-mem-address-stream.md](../archive/gui/29-mem-address-stream.md) | R2 | the reserved `mem` kind — no producer today | 3 | 01; 10 (consumer, inert-ready) | ✅ 3/3 (T1 writer+schema+unit, T2 emulator projection + golden `mem-df-chain`(+torn), T3 live `--dataflow --mem`/serve `mem:true` + `test_mem_parity`) | — |
| [30-resume-from-state-and-reweave.md](../archive/gui/30-resume-from-state-and-reweave.md) | R3 | route the value producer through `emu_snapshot`/`emu_restore` | 4 | 05 (forks); 28 T1 (for T4) | ✅ 4/4 (T1+T2 `emu_t`-hosted producer byte-identical via R5's `df_guest` seed/capture seam, `6d16cac`; T3 Reweave — `loom_take_run_from_step` fork-from-K stitched worldline, `cc4ad3b`; T4 `dt_scrubber_replayable` retires the "not a day-one feature" refusal with a synthesized+bannered register history, `8f38b81`) | — |
| [31-wide-register-deck.md](../archive/gui/31-wide-register-deck.md) | R4 | `fpenv` kind + XMM/YMM/MXCSR capture + SSE-class args | 3 | 28 T3 (`wide[]` format); 01/D5 | ✅ 3/3 (T1 XMM+MXCSR deck both producers + `movq` parity; T2 `fpenv` decode + graceful degradation; T3 `run_fp` SSE args + XMM `df_step` values + `fp-scale-add` golden; 128-bit only, YMM deferred) | — |
| [32-per-guest-value-producer.md](../archive/gui/32-per-guest-value-producer.md) | R5 | arch-parameterize `dataflow_emu.c` (arm64 first) | 3 | independent axis; demand-gated | ✅ 3/3 (T1 `df_guest` vtable, byte-identical x86-64; T2 arm64 value fabric + golden `arm64-df-chain`, PLUS its `regstate`/Scrubber sub-item — `emu_arm64_t` gains its own per-step register ring mirroring `emu_x86_regs_t`'s, `2f9f071`, zero change to `emu_t`/Reweave; T3 Author-mode arm64 run — the door dispatches arm64 through the value-fabric producer with a faithful, distinct result shape + a materialised `trace`/`df_step`/`df_edge` recording + the accurate gate-flip label, `2873973`; see brief status) | — |
| [33-backward-attribution-producers.md](../archive/gui/33-backward-attribution-producers.md) | R6 | the reserved `blame` + `statediff` kinds | 2 | 28 T1 (statediff pairing); 05 (L1 edges) | ✅ 2/2 (T1 `blame` cone + born-untraced fidelity, T2 `statediff` delta + two-recording merge gated on R1 identity; recorder-only producers, asmspy leg deferred) | — |

18 tasks across docs 28–33. Suggested order (per the roadmap): **R1 first**
(cheapest, no new engine, prerequisite for R4/R6, closes 04/05 fidelity gaps —
land it through the Phase-3 freeze), then **R2 and R6 in parallel** (new
producers over an existing recording; R6's `statediff` waits on R1 T1), then
**R4** (reuses R1's `wide[]` format — **✅ landed 2026-07-28**: the XMM/MXCSR deck +
`fpenv` on both producers, 128-bit only with YMM deferred), then **R3** (the
headline — it is what "not a day-one feature" gates — largest, lower urgency), with
**R5** an independent demand-gated arch axis. The plan's permanent Acknowledged limits (exact-only fabric, no
cross-thread hops, forks-never-touch-live, statistical-absence-proves-nothing)
are out of scope by design — a brief there would fight the design.

**Follow-on briefs (beyond the six roots).**
[34-playhead-and-scene-reach.md](../archive/gui/34-playhead-and-scene-reach.md) — close the three
seams in the *"pick a process → watch the 3D graph change over time"* flow: put the
register Scrubber on the shared **execution-step** brush (T1), give the 3D overview a
`5` keyroute + a "View in 3D" handoff from the Live-capture pane (T2), add a
**play/pause transport** to each playhead (T3), and name each axis so the two
playheads read as different axes, not a faked global clock (T4/fidelity). A pure
`ShellState`/`HudState` desktop brief — no producer/engine/schema change, driven by
the null backend (D4). Authored 2026-07-28 against HEAD `326afde`; **implemented
2026-07-28 (`e962fb5`)** — all five tasks (execution-step brush, `5`/handoff reach,
per-axis play/pause transports, axis labels), both docker-desktop lanes green.
☑ 5/5 · —.

[35-continuous-live-dataflow.md](../archive/gui/35-continuous-live-dataflow.md) — make the live
`dataflow` / `auto` engine **re-arm and keep capturing until Stop** (today it is
one-shot: "auto + Start starts then stops"), into one continuously growing
recording. Mirrors the region engine's `while (!stop)` loop; the surgery is a
per-invocation `df_invocation` discriminator (each pass's `df_step` restarts at 0)
and threading `stop` into the ptrace step loop. **Not** R3 resume-from-state (no
state restore/edit) and **not** whole-process-continuous (each pass stays scoped +
bounded). Authored 2026-07-28 against HEAD `f2b6cdf`. **T1/T3/T4 landed 2026-07-28**:
the engine re-arm loop (`asmspy_engine_dataflow` gains a `continuous` param + loop,
one-shot byte-identical) + the `df_invocation` per-pass delimiter (schema-defined,
`asmtrace_df_invocation_body`) + serve `continuous:true` / CLI `--dataflow
--continuous` (`fa2cef4`); the desktop segments the growing recording per pass
(`build_segmented_step_index`, latest = the live default) + the capture-pane
`continuous` checkbox + the hand-authored `low-fidelity/continuous-df.asmtrace`
fixture (`fc836e0`). **T2 step 1 (interruptible Stop) landed 2026-07-28**
(`c2313be`): `stop` is threaded through a new `asmtest_dataflow_ptrace_attach_jit_stop`
entry (the old signature forwards NULL, so every existing caller stays
byte-identical) into `dfp_run_to_multi` (the entry wait) and `dfp_step_loop`,
which terminates through the crash-safe `dfp_dirty_exit` on stop — so a Stop
during a live continuous capture is honored **within one in-flight pass** rather
than only between passes (a timed `cli_smoke` test yields inside a bounded entry
wait, victim surviving the detach). arm64 is architecturally unreachable here (the
single-step producer is x86-64-only and self-skips), so the detach-fatal hazard
cannot bite. **T2b (seize-once — hold one seize across passes) DEFERRED as a
beyond-bar optimization** — the seize-once + re-arm-per-pass refactor rewrites
`dfp_step_loop`'s most fragile path for a per-pass re-SEIZE that is O(threads) and
negligible against a pass's 10³–10⁵× single-step cost, so T2's "no per-pass
re-SEIZE cost regression" bar is already met with no measurable regression; a cost
optimization, not a correctness gap or a required step. ☑ 4/4 · —.

[36-anchor-the-3d-plane.md](../archive/gui/36-anchor-the-3d-plane.md) — **place a
routine-relative path on the 3D plane, or say why not.** The 3D overview places
geometry only for a `basis:"abs"` recording, and the only producer of
`basis:"abs"` in the tree is the synthetic golden-scene generator
(`record_scene_abs`) — every real capture (live `trace`, live `dataflow`/`auto`,
every corpus file) is `basis:"rel"`, so its plane comes up **empty and
unlabelled**: the "3D overview" tab opens onto nothing after a live attach, with
no error. Three defects stack: the rel chip [25](../archive/gui/25-live-model-wiring.md) T6
promised never fires (the HUD keys it on the *canvas* basis, which is empty when a
recording carries no `trace`); nothing counts placement, so every vertex silently
failing `Projection::project` is indistinguishable from success (`refused()` stays
false — nothing was *detected*, each point merely failed); and the value producer
emits no `coverage`, so even a correctly anchored capture draws a flat plane. This
brief narrows — explicitly, not silently — doc 10 T3's *"never projected as a true
absolute path"* rule: when a recording carries exactly one `codeimage` code span,
`base + off` **is** the true address (a derivation from a fact the recording
states, not a guess), so it is placed and flagged `TRAJ_ANCHORED`; when the span
is absent or ambiguous it refuses **louder** than today — no geometry *and* a
stated reason. Adds a labelled `df_step` residency height rung (never block
coverage), the placement counters + HUD chips, and a `scene-df-loop` golden in the
live shape no existing fixture carries. T5 then narrows `converge.cpp`'s exclusion
the same way — an *anchored* rel path takes part in convergence detection while an
unanchored one and any individually-unplaced vertex still cannot (with the faithful
limit that `df_step` carries no `tid`, so a live dataflow capture is one trajectory
and yields no marks regardless). Pure `space/` + HUD: no engine, no GL, no wire
change, no existing golden regenerated. Authored 2026-07-29 against HEAD `24778e4`.
✅ 5/5 (T1 `resolve_anchor` `38d05d6`; T2 the rel PC path ANCHORED onto the plane
`1622aaa`; T3 the labelled `df_step` height rung + anchored trace rung `ff650c3`;
T4 the fidelity chrome — pure testable `placement_chips`, the dropped rel chip
fixed, an unplaced-plane pane placard, the `scene-df-loop` golden (byte-stable
under docker-cli) + `test_shell`/`test_scene_fbo` end-to-end incl. the GL
"anchored tube puts pixels on screen", and doc/schema reconciliation `a3b2247`;
T5 convergence admits an anchored rel path — four-condition bar, `!placed`
per-vertex skip, the unanchored/statistical canaries kept). **COMPLETE — 37 next
(land 36 in full, then 37).** · —.

[37-region-tag-on-df-step.md](../archive/gui/37-region-tag-on-df-step.md) — **the producer half of
36**: state the region on the wire instead of deriving it. 36 anchors a
routine-relative offset by deriving the base from the recording's single `codeimage`
span, and must **refuse** whenever a recording carries zero or ≥2 spans — which a
live `auto` candidate walk produces routinely — because `df_step` says only `step`,
`off`, `disasm?`, `ops`. Yet the producer *knows* the base as it writes the offset
(it is already a `dataflow_record` parameter, used today only for disassembly) and
simply does not emit it. This brief adds an optional `rbase` (a base, not a version:
`version`/`when` both reset to 0 mid-recording on a candidate walk, so base is the
only globally distinguishing field `codeimage` carries), turning 36's derivation into
a stated fact and making the multi-span case resolvable rather than refusable. It
also redeems 36 T3's promise — the terrain churn walk becomes a sound "region as-of
this step" resolver, fixing a pre-existing bug where a dataflow recording's churn
always pins at step 0. Additive optional field on a known kind ⇒ no envelope bump and
no break (*"Ignore unknown fields"*, and no reader in the tree rejects them); the
schema is still *draft, not frozen*, so it lands under the append-only rule. Costs
one 20-file golden regen inside `docker-cli`; 3 hand-authored goldens + 1 desktop
fixture stay deliberately untagged as standing coverage for the absent-`rbase`
fallback. **36's `resolve_anchor` is retired to *the fallback*, never deleted** — it
remains permanent for pre-37 recordings and for rel `trace`, which this brief
deliberately does not tag. T4 (`when`: which bytes were live at this step) is
severable. Authored 2026-07-29 against HEAD `81e6ade`. ☑ 6/6 (T1 the `rbase` tag —
optional u64 after `off`, emitted only when nonzero so a `rbase==0` df_step is
byte-identical to pre-37; all 3 C producers + Author VM + `record_scene_df` state
their base; exact-body unit test; schema field/order + freeze-item closed; 20 flat
goldens regenerated byte-stable under docker-cli; live `cli_smoke` asserts
`df_step.rbase` == the region base; T2 the desktop reader resolves the span from
the wire — `streams` decodes `insn_rbase`, the trajectory places a tagged df_step
per-event at `rbase+off` (per-event precedence over 36's anchor) and grades it in
`anchor_source` (`wire`/`single-span`/`mixed`), `ptslice` populates it for the
live pane; the two-span recording 36 must refuse now places every vertex, the
untagged fallback stays intact; T3 the terrain churn walk is redeemed — it counts
`df_step` offsets as steps (a df recording's churn no longer pins at step 0) and
keys the churn join + df-rung placement on each step's own `rbase`, so a
multi-span `auto` capture gets relief and two spans churn at distinct steps; T5
the HUD grades the placement by `anchor_source` — `wire` ("span stated on the
wire (rbase)") distinct from `single-span` ("derived placement") and `mixed`;
36's fallback reconciled in doc 36 + CHANGELOG; T6 the end-to-end proof —
`record_scene_df_multi` emits `scene-df-two-span.asmtrace` (two `codeimage` spans +
`df_step`s tagged to each, no `trace`; byte-stable under docker-cli), and
`test_shell` asserts both spans place, `pc_placed==pc_points>0`, and
`anchor_source=="wire"` — the recording 36 alone renders labelled-empty). **☑ 6/6
LANDED IN FULL (T1–T6).** T4 the `when` tag — the serve-path disasm-version
refinement, severable from the multi-span resolution the other five tasks deliver:
the serve sink samples `asmtest_codeimage_now` once per invocation and stamps it on
every `df_step`, keyed with `rbase` (never `when` alone, since it restarts per
re-armed span); the headless sink and both corpus recorders have no codeimage
timeline and omit it, so no golden churned; `DataflowStream`/`ptslice` carry it and
`observer_build` seeds the Observer disasm pane's manual "as of logical time"
default from it, a hand override still winning. · will · 2026-07-31.

[38-live-feed-completion-roadmap.md](38-live-feed-completion-roadmap.md) — **the
live-feed audit + roadmap** (a family overview like [27](../archive/gui/27-extension-roadmap.md),
not a brief). After 36/37 anchored the live 3D plane, a 2026-07-29 audit of every
desktop visualization against *"can this be produced from a **live** process?"*
found the live **x86-64** pipeline already close to comprehensive — Summary, Canvas,
Slice, Timeline, Loom, Scrubber (+`fpenv`), the full Observer deck, and the 3D
terrain/trajectory/HUD all feed live off a growing recording. It maps the short list
of **closable** gaps (L1 the arm64 live-dataflow leg — biggest, hazard-gated; L2
`decode_streams` `df_invocation`-awareness for continuous per-pass — cleanest;
L3 the precomputed `blame`/`statediff` serve kinds — low, the *views* already work
live; L4/L5 Author macOS/ARM32/RISC-V; L6 = 37 T4) and records the **permanent**
gates (AMD-IBS survey, Intel-PT observer, Watch HW debug regs, signed installers)
so they are not re-opened. **Corrected two audit over-claims** (a `tid`-on-`df_step`
convergence brief — dead, the live df engine is single-threaded by scope — and the
blame/statediff *views*, which already work live client-side) and **fixed two stale
"`mem` has no producer" comments** (R2/doc 29 landed the live `--mem` producer).
Authored 2026-07-29. overview · —.

[39-auto-capture-reliability.md](../archive/gui/39-auto-capture-reliability.md) — **make `auto`
reliably capture**, and stop a self-ended session wedging the pane. Cut from
[38](38-live-feed-completion-roadmap.md) as its first per-gap brief. Two halves
produce one complaint (*"start and arm, it starts then stops, `refused: no session
is running`"*). **The headline is host-shaped and backwards:** the portable
software-clock *residency* picker gets a documented retry walk over up to three
ranked candidates, while the AMD IBS-Op *entry-arrival* picker — the stronger
evidence — gets **one candidate and no walk**, because `auto_pick` ranks `nc`
candidates, reports the count, then returns only `cands[0]`, so the walk's guard
`attempt + 1 < ncand` sees `ncand == 0` and never fires. On an AMD box the better
sampler yields the less resilient capture. Second half: nothing reconciles
`InspectState::active` against a session that ends on its own, so the jack stays
held, the pane offers only Swap, and Swap's `stop` is refused by a host whose own
`serve_reap` — which runs at the top of the command loop, before dispatch — just
cleared the precondition. Adds: a pure candidate-walk decision (killing the
duplicated inline walk), ranked candidates on the IBS path, empty-window retry plus
a wire-settable sample window, `continuous` re-arming through a quiet region
(today `DF_PTRACE_NEVER` breaks the loop regardless of the flag), and the
session-lifecycle repairs — including passing `inspect_start_params` on the swap and
queue restarts, which is what makes `continuous` un-settable in practice. **No
hardware unlock:** every policy change is proven by pure tests in `test_autoregion`
(no backend link, both `cli-smoke` lanes), because no CI lane has AMD silicon — per
CLAUDE.md, *"a test that can only ever self-skip is not a test."* Authored
2026-07-29 against HEAD `0b52704`, measured on the Zen 2 dev box. ✅ 6/6 (T1
pure `asmspy_autoregion_walk` + T2 `auto_pick` ranked list — both inline walks
retired — `7de837c`; T3 empty-window retry + `--window`/wire `ms` + capture-pane
input `bc46d49`; T4 continuous through a quiet region + 0-step `df_invocation`
marker + `quiet_hot_victim` smoke `9710a79`; T5 self-end announced from the tracer
tail + `inspect_reconcile_self_end` frees the jack + stop-reap ack + `last_err`
clear `2427a64`; T6 idle-window pick rendered faithfully + doc 38/CHANGELOG
reconciled). **Corrected stale premises:** the swap/queue param-drop (T5.5) was
**already repaired** and `InspectState::active` has **7** mutation sites, not 5;
the pre-existing `test_shell` `attach/no-host reveals panes` bar was made
environment-adaptive (it was RED on any checkout that had built `./build/asmspy`).
Both docker lanes green (`test_ui` 28/28). · —.

[40-segment-dataflow-by-invocation.md](../archive/gui/40-segment-dataflow-by-invocation.md) —
**segment the dataflow decode by `df_invocation`** (gap **L2** from
[38](38-live-feed-completion-roadmap.md), the cleanest self-contained desktop win).
`decode_streams` indexed `df_step`/`df_edge` by a flat `step` space, so a continuous
`dataflow`/`auto` capture's passes (each restarting `step` at 0, 35 T1) aliased —
offsets last-write-wins, ops/edges merged across passes sharing a step number — the
last dataflow consumer that still conflated passes after the Scrubber segmented its
register ring (35 T3). T1 refactors the df decode into a pure `decode_dataflow` core
and adds `build_segmented_dataflow` (one `DataflowStream` per pass, bucketed by the
`df_invocation` markers' `seq` exactly as `build_segmented_step_index` buckets
regstate); `decode_streams` resolves `Streams::df` to the LATEST pass (the live
default, mirroring `build_step_index`), so passes never conflate and a one-shot
recording stays byte-identical to the flat decode (`test_streams`, `4b15f87`). T2
caches the segments per recording and gives the Slice/Timeline/Loom panes a discrete
per-pass invocation pager (`shell_apply_df_pass` / `shell_df_pass_pager`) — following
the latest by default, pinnable to an earlier pass, no chrome for a one-shot. Pure
decode + a selector: no engine, wire, or schema change. Authored 2026-07-29. ✅ 3/3
· —.

[41-live-blame-statediff-serve-leg.md](../archive/gui/41-live-blame-statediff-serve-leg.md) — **emit
the `blame` + `statediff` kinds from the live serve leg** (gap **L3** from
[38](38-live-feed-completion-roadmap.md)). [R6/doc 33](../archive/gui/33-backward-attribution-producers.md)
landed both producers recorder-only; the live `asmspy --dataflow`/serve leg emitted
`df_step`/`df_edge`/`regstate`/`mem` but not those two. Both are pure projections over
data the serve leg already has — `blame` is the backward cone (`dataflow_emit_blame`,
a port of the recorder's `emit_blame`, seeded at the penultimate step over the def-use
graph the sink already builds) and `statediff` is a delta of the `regstate` ring
(`--statediff` self-arms it) — so they ride the sink ctx like `emit_mem` with **no
engine, wire, or schema change** (both kinds were already defined), spelling the wire
with the SHARED body builders so a live and a golden one are byte-identical. Serve
`blame:true`/`statediff:true` + CLI `--blame`/`--statediff`, echoed in the
started-params announce. **Genuinely low value** (the *views* already worked live
client-side; this is the reproducible/deep-linkable *convenience* only) but cheap and
host-testable — `cli_smoke` asserts the live cone + the step-0-`computed:false` delta.
Authored 2026-07-29. ✅ 3/3 · —.

[42-loom-reweave-consumption.md](../archive/gui/42-loom-reweave-consumption.md) — **consume the
Reweave request** (review #20): wires the fully-built-but-unreachable
fork-from-step-K counterfactual ([30 R3 T3](../archive/gui/30-resume-from-state-and-reweave.md))
into the Loom UI — `ReweaveSource`/`loom_reweave_available` (the `code_sha`
identity latch), the app-only `reweave_apply.cpp` engine leg (verified via `ldd`
to never link into `asmtest-viewer`), and a `LoomState::take_views` paint
overlay. A 4-dimension adversarial review of the first landing caught 4 real
bugs (frozen Author `args`/`nargs`, stale takes surviving a Loom tab switch, a
refused reweave painting a fabricated alignment, `take.err` dropped on a
faulted success path) — all fixed in the same change; see the doc's own status
banner for detail. **This entry was missing from this table** (added
2026-07-31 while cutting [43](43-faithful-city-roadmap.md)/[44](../archive/gui/44-faithful-city-phase-a-mvp-terrain-reskin.md) —
a pre-existing README gap, not new drift). ✅ 5/5 · —. Follow-up: rich
`fault_card()` for a Reweave (needed a `src/dataflow_resume.c` C-API change) —
**CLOSED 2026-07-31** (`65b2a54`, unrelated to the city family below).

### The 3D families (docs 43–59) — representation, instrument, depiction

Three families now work on [doc 10](../archive/gui/10-spacetime-3d-overview.md)'s
spacetime scene. They are genuinely orthogonal — one changes what the scene
*depicts*, one what a person can *do* with it, one *which quantities* it draws —
and each has its own roadmap doc that cuts implementation-ready briefs the same
way [11](../archive/gui/11-imgui-addons.md) and [27](../archive/gui/27-extension-roadmap.md)
did for their families. All three read the same three analysis docs
([3D catalog](../analysis/2026-07-29-3d-visualization-catalog.md),
[computer-as-city](../analysis/2026-07-30-computer-as-city-3d.md),
[UX/dataviz review](../analysis/2026-07-29-gui-ux-dataviz-review.md)) and share the
same substrate and the same four fidelity invariants, so they compose rather than
conflict; the prose after these tables is the commentary on each.

**Representation — the faithful city (docs 43–44).** What the scene depicts.

| Doc | Area | Tasks | Depends on | Status | Claim |
|---|---|---|---|---|---|
| [43-faithful-city-roadmap.md](43-faithful-city-roadmap.md) | adopts the computer-as-city design as the 3D roadmap; restates its 5 phases (A–E) with status; corrects two stale claims (**roadmap, not a brief**) | — | 10 | roadmap · A cut as 44; **B–E uncut** | — |
| [44-faithful-city-phase-a-mvp-terrain-reskin.md](../archive/gui/44-faithful-city-phase-a-mvp-terrain-reskin.md) | Phase A: zoning (`kind_by_cell`), fog-of-war `TF_UNKNOWN`, fidelity weather sky, separate ghost-fog survey surface, two-clock plumbing, followed-citizen vehicle, 4 new `SceneLayers` bools | 7 | 43; 10, 23, 24 | ✅ 7/7 | — |

**Instrument — the 3D as a thing you operate (docs 46–52).** What a person can do
with the scene. Cut 2026-08-02 from the UX review's 3D findings, the half neither
the catalog nor the city doc acted on.

| Doc | Area | Tasks | Depends on | Status | Claim |
|---|---|---|---|---|---|
| [46-3d-functional-roadmap.md](46-3d-functional-roadmap.md) | the 13-gap table (G1–G13), sequencing, and the one fidelity decision the family turns on — *cross-axis brushing goes through the ADDRESS, never an ordinal* (**roadmap, not a brief**) | — | 10, 44 | roadmap · cuts 47–52 | — |
| [47-scene-inspect-and-pickable-overlays.md](47-scene-inspect-and-pickable-overlays.md) | G1–G3: throttled hover pick + cell→content index, `resolve_pick_hint`, pickable convergence arcs / access spurs, hover readout, HUD legend | 5 | none | ✅ 5/5 (T1 the `TerrainModel` sorted-vector cell→content index + throttled hover-pick, `resolve_pick` routed through it, `3b7ebf1`; T2 `resolve_pick_hint` sharing one `classify_cell()` helper with `resolve_pick` so a preview can never drift from a click, golden-tested per branch incl. the anti-drift assertion, `3b7ebf1`; T3 pickable convergence arcs/access spurs — new id bands past the now-bounded vertex band via an explicit `PickBands`, both functions resolve an arc to whichever tid is nearer `follow_step` and say which side was chosen, `ee82b9a`; T4 the hover tooltip (what/where/quantity/fidelity/click-target), null-backend-safe, tooltip itself untestable in this tree (no GL interaction lane) so the gap is stated rather than implied, `90f3296`; T5 HUD legend swatches for both overlay classes + a `convergence` checkbox + the persistent "hover to inspect" hint, exhaustive-by-test, `c64c495`) | — |
| [48-scene-navigation-and-goto.md](48-scene-navigation-and-goto.md) | G4–G6: camera pan/recentre, address & region goto, landmark home, discoverable controls | 5 | none | ✅ 5/5 | — |
| [49-one-time-truth-in-the-scene.md](../archive/gui/49-one-time-truth-in-the-scene.md) | G7–G8: clip the worldline to the playhead, mark the execution front, make height readable | 4 | none | ✅ 4/4 | — |
| [50-two-way-brushing.md](50-two-way-brushing.md) | G9–G10: light the scene from the flat views *through the address*; closes 44's deferred resolver and the reverse-direction ordinal | 4 | 47 (hint helper, else free) | ☐ 0/4 | will · T1-T4 · 2026-08-02 |
| [51-scene-focus-and-scale.md](51-scene-focus-and-scale.md) | G11–G12: per-tid/region/kind focus, thread ghosting, camera-distance entity budget | 4 | 48 (landmarks, else free) | ☐ 0/4 | *free* |
| [52-flat-terrain-surface.md](52-flat-terrain-surface.md) | G13: a GL-free 2D terrain surface — the no-GL branches gain a real view, the GL path gains a reading mode | 4 | none | ☐ 0/4 | *free* |

**Depiction — the 3D catalog as a build plan (docs 53–59).** Which quantities the
scene draws, and what must exist before it can draw them. Cut 2026-08-02 from the
catalog's own 26-graph inventory and its five-phase build order.

| Doc | Area | Tasks | Depends on | Status | Claim |
|---|---|---|---|---|---|
| [53-3d-catalog-build-roadmap.md](53-3d-catalog-build-roadmap.md) | the 26-graph inventory with each gate re-verified, the GL-effects survey and its baseline gate, the city-phase mapping, and what a Phase-4/5 cut would contain (**roadmap, not a brief**) | — | 10, 43, 46 | roadmap · cuts 54–59 | — |
| [54-3d-catalog-phase0-plumbing.md](../archive/gui/54-3d-catalog-phase0-plumbing.md) | observed-data-span projection (**makes the terrain's data half reachable at all**), read/write prefix-sum split, `Event::seq` on `SyscallRow`, engine-free `mnemonic_class`, BFS-depth dataflow walk, `dt_link` invocation field, the `HotEdge` sourcing decision | 7 | none | ✅ 7/7 | — |
| [55-scene-render-quality.md](55-scene-render-quality.md) | eye-dome lighting, depth-dependent halos, `fwidth` contour bands + height key, order-independent translucency (WBOIT + dithered fallback), MSAA, portable line width + the GLSL-version question | 6 | none | ☐ 0/6 | *free* |
| [56-fidelity-and-module-layers.md](56-fidelity-and-module-layers.md) | L1–L4: the layer registry, confidence terrain + coverage-window mask, per-module residency skyline, opcode-class code terrain, misprediction survey layer | 5 | 54 T4 (T4), 54 T7 (T5) | ☐ 0/5 | *free* |
| [57-causal-layers.md](57-causal-layers.md) | L5, L6, L11, L14: one step→place resolver, kernel crossing spurs, taint isochrone, blame convergence forest, dominant-path ridge | 5 | 54 T3 (T2), 54 T5 (T3); 56 T1 | ☐ 0/5 | *free* |
| [58-memory-data-cell-family.md](58-memory-data-cell-family.md) | L7–L9, L12, L13: data-rung HUD contract, read/write twin relief, working-set tide, observed-lifetime pillars, data-access worldline ribbon, residency sediment columns | 6 | **54 T1 + T2**; 55, 56 T1 | ☐ 0/6 | *free* |
| [59-standalone-scenes.md](59-standalone-scenes.md) | S1–S4: a scene host that is not the address plane, then divergence worldline, invocation stack, module excursion ribbon, SIMD lane prism | 5 | 54 T6 (T3), 54 T3 (T4); 48, 47 T3 | ☐ 0/5 | *free* |

39 tasks now open across the three families (28 landed — 7 in representation
via 44, 14 in instrument via 47/48/49, 7 in depiction via 54 (complete) — and
12 + 27 still open in instrument/depiction). Sequencing across
them: the two roadmaps' own orders hold within each family
([46](46-3d-functional-roadmap.md)§3, [53](53-3d-catalog-build-roadmap.md)§8), and
the families do not block each other — with three overlaps that must be
coordinated rather than duplicated. **Pick-id bands**: 47 T3, 59 T1. **The
address-first step→place resolver**: 50, 57 T1. **The movable camera target**: 48,
59 T5's per-scene framing. In each case whichever lands first owns the mechanism
and the second adopts it; all six briefs say so in their own text. Landing
[55](55-scene-render-quality.md) early pays off across both open families —
[58](58-memory-data-cell-family.md) is where stacked translucency stops being
decorative, and [49](../archive/gui/49-one-time-truth-in-the-scene.md)'s height readability was the
same problem 55 T3 solves in the shader.

**The faithful city (docs 43–44, 2026-07-31): a new numbered family, not part of
the ten core docs.** [43-faithful-city-roadmap.md](43-faithful-city-roadmap.md)
adopts the [computer-as-city 3D design](../analysis/2026-07-30-computer-as-city-3d.md)
(which unifies the [3D visualization catalog](../analysis/2026-07-29-3d-visualization-catalog.md)'s
14 layers + 12 scenes under one land/districts/buildings/traffic/weather
metaphor) as the roadmap for future work on [doc 10](../archive/gui/10-spacetime-3d-overview.md)'s
scene, restates its 5-phase plan (A–E) with current status, and corrects two
stale claims found while re-verifying (the live-GL-upload-freeze bug both source
docs cite as open was already fixed in `55fc624`, before either doc was
authored — do not re-touch that fix).
[44-faithful-city-phase-a-mvp-terrain-reskin.md](../archive/gui/44-faithful-city-phase-a-mvp-terrain-reskin.md)
is the first per-phase implementation-ready brief cut from it (mirroring how
[39](../archive/gui/39-auto-capture-reliability.md)/[40](../archive/gui/40-segment-dataflow-by-invocation.md)/[41](../archive/gui/41-live-blame-statediff-serve-leg.md)
were cut one at a time from [38](38-live-feed-completion-roadmap.md)'s gap
table, not landed all at once like the UX-restructure or extension families):
a pure terrain/trajectory reskin (zoned districts, a fidelity-driven weather
sky, a physically separate statistical "ghost fog" surface, a followed-citizen
vehicle) with **no new renderer primitive, no producer change, no schema
change**. Phases B–E remain uncut; see [43](43-faithful-city-roadmap.md)§4 for
sequencing guidance before starting one. 43: roadmap only, no tasks · —. 44:
✅ 7/7 (T1 `kind_by_cell` + the zoning shader, an off-domain sentinel distinct
from `Region::Unknown`; T2 `TF_UNKNOWN` fog-of-war, per-slice frontier,
off-domain cells never flagged; T3 the fidelity weather sky —
`scene3d/atmosphere.h`, `scene_atmosphere_for_tier()` sourced only from
`ui/theme.h`'s shared palette, byte-identical to the 2D banner, damped ~0.5s;
T4 `Scene::set_stat_terrain` — the ghost-district survey surface, its own
texture pair, uploaded once per weave, never scrubs; T5 the two-clock
plumbing — `SceneFrame.sun` + a second, independent `follow_step` `Transport`,
the axis-mismatch decision (`follow_step` free-runs `TrajPoint.t`'s own axis,
never seeded from `Selection.step`) written down in `shell.h`'s own comment;
T6 the followed-citizen vehicle + comet tail, sharing the underlying PC
vertex's pick id, no VBO schema change, unplaced steps draw nothing; T7 the
`zoning`/`weather`/`ghost_fog`/`vehicle` `SceneLayers` bools, default true —
the "city" preset is the struct's own defaults). `docker-desktop` green on
merged main (`desktop-test` incl. `test_terrain`/`test_scene_fbo`,
`desktop-engine-boundary-check` — D4 intact, `desktop-ui-test` 28/28,
alongside doc 45's Xvfb lane); no `.asmtrace` schema touched. · —.

[45-launch-and-window-target.md](../archive/gui/45-launch-and-window-target.md) — **two new
Home-rail entries** (not cut from a prior analysis doc): **launch** a fresh
process and trace it from birth (Part A, T1–T5), and **target** an already-
running process by dragging a crosshair onto its window (Part B, T6–T9).
Neither exists in any form today — every headless mode and the `--serve`
wire protocol take only a `pid` (attach), and no window→PID resolver exists
anywhere in the tree. Part A's real cost is engine-side: the nearest
fork+`PTRACE_TRACEME` idiom in the tree (`dataflow_ptrace.c`/`ptrace_backend.c`)
execs an in-memory JIT stub, not an arbitrary external command, so T1 lands a
launch primitive with an honestly-flagged interim fidelity gap (a brief
detach/re-SEIZE window) and T2 closes it by threading "already traced" through
each mode's `PTRACE_SEIZE` entry point in `asmspy_engine.c` instead — a real,
named design decision, not silently assumed. A nice side effect of doing the
fork server-side (D9): launch is ssh-transparent for free, and a launched
child sidesteps the Yama `ptrace_scope=1` attach uncertainty entirely (it's
asmspy's own descendant by construction, per `inspect.cpp:133-150`). Part B is
new X11-only platform code (`desktop/src/platform/window_picker.*`, T6),
gated off honestly on Wayland/macOS/ssh-remote capture — mirroring the
window-icon precedent already in `main.cpp` for degrading a per-window OS
feature that Wayland has no equivalent of — reusing GLFW's built-in
`GLFW_CROSSHAIR_CURSOR` and, on release, the exact same
`inspect_attach_full_detail` path a Processes-row click already uses (T7–T8).
Authored 2026-07-31 against HEAD `5ef06e5`. ✅ 9/9 (T1+T2 landed as one
integrated change — building the launch primitive surfaced that the fork must
run on the exact OS thread that keeps tracing the child, so T1's interim
detach/re-SEIZE gap was never shipped standalone and needed no retiring;
`already_traced` threads through every direct SEIZE site — log/stream/graph/
tree/procs/watch/trace — `dataflow`/`auto` attach through a separate, deeper
subsystem this brief doesn't touch and are refused with a stated wire error
rather than risked; T6+T9 the X11 window→PID resolver + `libx11-dev`/`xvfb`
pinned in `Dockerfile.desktop` + a genuine Xvfb-backed integration lane
(`test_window_picker_xvfb`, a live virtual display + a real second window,
not a mock); T3/T4/T7/T8 the Launch pane/CTA + crosshair drag-to-attach,
reusing `inspect_attach_full_detail` unchanged on release). Both
`docker-desktop` lanes green on merged main (`desktop-test`,
`desktop-engine-boundary-check` — D9 stays intact, the desktop process itself
never calls `PTRACE_TRACEME`/`PTRACE_SEIZE` — `desktop-ui-test` 28/28,
`desktop-test-xvfb`); `cli-smoke` green headless. `asmtrace-golden-check`'s
one diff (`abixray-make_pair-sysv`) is a pre-existing host-Capstone-version
artifact, confirmed unrelated via baseline comparison. · —.

**The 3D as an instrument (docs 46–52, 2026-08-02): the second 3D family, cut
from the same three analysis docs 43 read — but from the half none of them
acted on.** [46-3d-functional-roadmap.md](46-3d-functional-roadmap.md) is the
roadmap; [47](47-scene-inspect-and-pickable-overlays.md)–[52](52-flat-terrain-surface.md)
are its six implementation-ready briefs, all ☐ not started, all unclaimed.
Where [43](43-faithful-city-roadmap.md) is the **representation** axis (what the
scene depicts — zoning, weather, districts, towers), this family is the
**instrument** axis (what a person can do with it). That split is not
editorial: the [3D catalog](../analysis/2026-07-29-3d-visualization-catalog.md)
and the [city doc](../analysis/2026-07-30-computer-as-city-3d.md) between them
propose 33 concepts and 44 city elements, and every one is something to *draw* —
neither proposes a single new thing to *do*. Only the
[UX/dataviz review](../analysis/2026-07-29-gui-ux-dataviz-review.md) looked at
operation, and its 3D findings (#14, #36, #37, #38, #40, #50, #55, #56, #58)
were never absorbed by the city doc, which says outright that it absorbs the
*catalog*. 46§2 is the resulting gap table — 13 gaps, each with its evidence
re-verified against `f110150`: no hover/inspect before a pick commits (and
`resolve_pick` already computes the answer and discards it); half the drawn
geometry not pickable at all (arcs, spurs); a camera that cannot pan, recentre
or go to a named address; a worldline that ignores the playhead while the
terrain slices on it (two contradictory time truths on one screen); height with
no scale, no contours, no key; navigation that only runs 3D→2D; no per-thread or
per-region focus; no distance-based LOD; and no spatial channel at all without
GL. Two findings are worth calling out on their own: **G10** — `resolve_pick`
sets `link.step = pv.t`, handing a per-tid vertex counter
([trajectory.cpp:99](../../../desktop/src/space/trajectory.cpp#L99)) to a field
documented as a dataflow step index ([nav.h:62](../../../desktop/src/nav.h#L62)),
i.e. the *same* axis conflation [44](../archive/gui/44-faithful-city-phase-a-mvp-terrain-reskin.md)
correctly declined in the forward direction, already shipped unguarded in the
reverse one (50-T3 opens by verifying that reading before changing anything);
and the family's single load-bearing design decision, **cross-axis brushing goes
through the ADDRESS, never through an ordinal** (46§4) — which is what makes
[50](50-two-way-brushing.md) able to close 44's deferred cross-brush soundly,
since `Selection.off → base+off → Projection::project` is exactly invertible and
`DataflowStream::insn_off[step]`/`insn_rbase[step]` supply the same bridge from a
step. Five of the six briefs are pure render/UI work over models that already
exist; none needs a producer change, a schema change, a new dep, or the Phase-D
instanced-building system, and none conflicts with a later city phase (two of
them — 48's landmarks, 51's entity budget — answer open questions the city doc
raises in its own §7). Sequencing in 46§3: 47/48/49 are independent and land in
any order (49 first if capacity is short — it is the only one fixing something
currently *misleading* rather than absent), then 50/51, with 52 parallel
throughout. Status and claims are in the instrument table above.

**The 3D catalog as a build plan (docs 53–59, 2026-08-02): the third 3D family,
cut from the catalog's own 26-graph inventory.**
[53-3d-catalog-build-roadmap.md](53-3d-catalog-build-roadmap.md) is the roadmap;
[54](../archive/gui/54-3d-catalog-phase0-plumbing.md)–[59](59-standalone-scenes.md) are its six
briefs. [43](43-faithful-city-roadmap.md) says to cut future 3D briefs from the
city doc rather than the raw catalog, and 53§1 explains where that stops holding:
**the city phases are grouped by metaphor, the work is gated by data**, so a brief
cut along a city phase always contains one item that blocks the other four (Phase
B mixes pure-render signage with `is_return` plumbing; Phase C mixes lit windows,
which need a projection extension that does not exist, with LOD). The catalog's own
§4 already found the right seams — seven shared plumbing changes, each unblocking
several graphs — so this family follows the catalog's phase order and 53§6 maps
every row back to the city phase it feeds, leaving 43 the representation frame.
Re-verifying the catalog against `b657876` moved four of its claims, one of which
changes a priority: **the terrain's data half is not merely under-fed, it is
structurally unreachable.** The shell builds the plane from
`regions_from_codeimage(r)` alone ([shell.cpp:921](../../../desktop/src/ui/shell.cpp#L921))
and the `mem` scan drops any access no region maps
([terrain.cpp:397-399](../../../desktop/src/space/terrain.cpp#L397)) — so
`DataCell`, `cum_size`, `cum_rw`, `TF_READ` and `TF_WRITE` are all code that exists,
is tested against hand-built projections, and can never fire from the UI.
[54](../archive/gui/54-3d-catalog-phase0-plumbing.md) T1 was therefore the
single highest-leverage task in either open family — and landed the same day
as this analysis (2026-08-02), alongside T2. The other new work is
[55](55-scene-render-quality.md), from an online survey of what would materially
improve these scenes at this app's narrow GL baseline (GL 3.0 / GLSL 130 on Linux,
3.2 core / GLSL 150 on Apple, no glad/glew): **eye-dome lighting** (the depth cue
ParaView/Potree/CloudCompare use for exactly this kind of unlit point-and-line
data) and **depth-dependent halos** (Everts et al., IEEE Vis 2009 — designed for
dense line data) both clear core 3.0 outright; `fwidth` contour bands turn terrain
height into a readable quantity; weighted-blended OIT is the one candidate that
does *not* clear it (`glBlendFunci` is GL 4.0 / `ARB_draw_buffers_blend`), so 55 T4
probes it and falls back to dithered transparency rather than assuming. The survey
also turned up two portability defects: `glLineWidth(2.0f)`/`(3.0f)`
([scene.cpp:664](../../../desktop/src/scene3d/scene.cpp#L664), [:695](../../../desktop/src/scene3d/scene.cpp#L695))
on a context that is core and forward-compatible on Apple, and every scene shader
pinned at `#version 130` while `main.cpp` selects a 3.2 core profile there — the
second recorded as **verify-first on Darwin**, not as a confirmed break, since this
tree has no macOS desktop lane. Sequencing in 53§8: 54 and 55 are independent and
both land first; then 56 (whose T1 builds the layer registry 57–58 register into),
57 and 59 in parallel; 58 last, after 54 T1+T2 and 55. The catalog's Phase 4/5 —
seven large scenes — is deliberately uncut, with 53§7 stating exactly what each
would contain and what unblocks it.

71 tasks across the ten core docs (01–10). Suggested start order: 01 and 03 in parallel (03's
T1–T6 need no corpus), then 02/04, then 05/06/07 in parallel, then 08, then
09 (09-T1 — the emulator ring — is engine-only and can start any time).
**All ten docs have landed.** 01–08 landed 2026-07-24; 2026-07-26 landed all of
doc 09 — register ring (T1), regstate recorder (T2), scrubber (T3), ABI x-ray
(T4), blame socket (T5) — and all of doc 10 — Hilbert projection (T1), terrain
builder (T2), trajectory builder (T3), GL scene (T4), live-observer overlay
(T5), drill-in + fidelity invariants (T6), golden scenes + gated GL lane + docs
(T7). **Integration surfacing pass, 2026-07-26:** the two doc-09 teaching views
are now **hosted in visible shell panes** — the register scrubber as a
per-recording `Scrubber` tab (its `regstate` seek index built at open, its
playhead persisted per recording), and the ABI x-ray as an `ABI x-ray` tab that
locks the active recording (the SysV leg) against the attached B (the Win64 leg,
the `d` binding), reusing the Diff tab's A/B mechanism; both degrade to their own
fidelity placards (absent producer, unaligned pair, torn ring) exactly as their
standalone draws do, and `test_shell` pins the wiring end to end. **Integration
surfacing pass 2, 2026-07-26:** the doc-10 3D overview is now **hosted in a
visible shell pane** too — a per-recording `3D overview` tab. Because `draw_shell`
links no GL (that is what keeps the null test backend and the render-only viewer's
engine-free closure), it reaches the GL scene only through an abstract
`SceneHost` (`ui/scene_host.h`): `main.cpp` builds a real render-to-texture host
(`ui/gl_scene_host.cpp`, an offscreen FBO around the doc-10 `scene3d::Scene`) and
points `ShellState::scene_host` at it, while the null test backend leaves it null
and the pane weaves its pure `space/` models, draws the HUD, and shows a placard
where the viewport would be. The pane blits the scene with `ImGui::Image`, orbits
/ dollies the camera on drag / wheel, re-slices the terrain on a playhead move,
and routes every click OUT through 04's deep-link router (3D to find, 2D to read).
A codeimage-less recording takes a truthful "no address-space regions" placard
rather than an empty plane; `test_shell` drives the pane under the null backend
(models woven, HUD drawn, placard shown, vectors parallel across a close) and the
GL scene itself stays pinned by `test_scene_fbo` and `test_drillin`. **All ten
docs and both teaching-view integration passes have now landed.** 07 shipped
`libasmspy` (the
tracer engine as a linkable tier), `asmspy --serve` and its normative protocol,
and the desktop's live capture host; 08 shipped the seven live views over those
sessions, the `codeimage` kind (defined in the schema, produced by `--serve`,
consumed by a bytes-as-of-trace-time pane) and the PT-replay slice. One schema
consequence worth carrying into the Phase-3 freeze: `codeimage` is the first
reserved kind to be **defined**, and it pairs with `stitch` by version — a
decoded PT path is only meaningful against the bytes it was decoded against. **Doc 10
is a growth-rung companion, not scheduled against Phase 1–4** — it consumes the
core docs' feeds and blocks nothing; start it only after 04/07/08 land, and only
its coarse rung is buildable before the Wave-1 `mem[]` stream.

## Plan phase mapping

Phase 1 = 01 + 02(T1–T4); Phase 2 = 03 + 04 + 05 + 06 + 02(T5–T6);
Phase 3 = 07 + 08 (+ the schema freeze checkpoint); Phase 4 = 09.
Doc 10 (the 3D overview) is a **growth-rung companion** outside the Phase 1–4
mapping — it reunifies the plan's killed Terrane + Observatory as an overview
surface, is gated on the Wave-1 `mem[]` stream for its rich rung, and is
prioritized only on demand from the RE/security and perf personas.
The plan's standing expansion-intake table stays in the plan; each wave item
lands as a new brief here in the same format.

## UX heuristic review (2026-07-27)

A usability heuristic evaluation of this whole doc set against Nielsen/NN·g's ten
heuristics and supporting UX sources (Shneiderman, Norman, Tognazzini, ISO
9241-110; profiler-tool conventions; dataviz accessibility), with a restructuring
roadmap. 24 confirmed findings (7 major / 15 minor / 2 cosmetic); the recurring
theme is that the app owns the two hardest IA pieces (one `.asmtrace` format, one
`dt_nav_go` spine) but hides them behind single-window nested tabs, so most fixes
are wiring on shipped substrate, not redesign.

Both documents are **archived** (2026-08-01): all 24 findings are actioned and
all 28 recommendations shipped as docs 18–24, so they moved out of `plans/` and
`reviews/` per the [archive rule](../README.md).

- [../archive/reviews/desktop-gui-ux-review.md](../archive/reviews/desktop-gui-ux-review.md) — the
  findings (F1–F24), method, severities, strengths, sources.
- [../archive/plans/desktop-gui-ux-restructure-plan.md](../archive/plans/desktop-gui-ux-restructure-plan.md)
  — the roadmap: five themes, four waves, and which tracks (13/14/15/16/17) to
  redirect vs. new briefs to cut.

**Briefs cut 2026-07-27.** The roadmap's 28 unbuilt recommendations are now
implementation-ready briefs — **docs 18–24** above, one per wave/theme cluster,
each tracing every task to its `T<theme>.<n>` recommendation and `F<n>` finding.
This was a drafting-only pass (no code); the keystone is doc 19 (real dockable
panes). The addon-adoption family (docs 12–17) is now fully landed — **15 T3**
(node-editor topo/tree/hot-edges) was the last item. The [asmspy](../plans/asmspy-plan.md) / [desktop-gui](../plans/desktop-gui-plan.md)
product plans are unchanged.
