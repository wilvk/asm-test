# Desktop GUI implementation documents — index

This directory holds the **implementation-ready specifications** for
[desktop-gui-plan.md](../plans/desktop-gui-plan.md): ten self-contained
briefs (nine core + one growth-rung companion), each for one coherent task set,
written so a junior developer can clone the repo, open exactly one document, and
implement it end to end (code + tests + docs) with no other context. Format and
rules follow [../implementations/](../implementations/README.md) — read
[\_conventions.md](../implementations/_conventions.md) once before starting.

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

  **Amended 2026-07-26 (addon-admission rule, from [11-imgui-addons.md](11-imgui-addons.md)).**
  D2 no longer means "no dep beyond imgui + nlohmann/json"; it means every new
  third-party UI dep clears this bar, so the tree stays permissively licensed,
  reproducible, and honest — no ad-hoc exceptions:
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
  5. **Honesty is a selection filter.** Nothing that renders survey data as
     stacks, imposes force-directed layout, or hides refusals is admissible.

  Implementation briefs for the addons themselves are cut from doc 11's tracks
  under this rule; the in-tree quick wins that add no dependency (the shared
  honesty-chrome palette `desktop/src/ui/theme.h`, the diff "go" routing fix)
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
- **D7 — honesty is tested.** The corpus includes dishonesty fixtures
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
| [01-asmtrace-format.md](01-asmtrace-format.md) | `.asmtrace` schema, record modes, golden corpus | 8 | — | ✅ 8/8 | — |
| [02-exporters-and-readers.md](02-exporters-and-readers.md) | speedscope/Perfetto exporters, completeness readers | 6 | 01 (03 for T5–T6) | ✅ 6/6 | — |
| [03-desktop-shell.md](03-desktop-shell.md) | desktop/ skeleton, deps, mk/desktop.mk, document model | 8 | 01 (corpus, for T7) | ✅ 8/8 | — |
| [04-replay-views.md](04-replay-views.md) | canvas, operand timeline, slice explorer, diff, deep links | 8 | 01, 03 | ✅ 8/8 | — |
| [05-loom-day-one.md](05-loom-day-one.md) | the Loom fabric, lineage, lane annex, forks | 7 | 02 (reader), 03, 04 | ✅ 7/7 | — |
| [06-doors-and-learning.md](06-doors-and-learning.md) | Learn/Author doors, ct_eq, capability panel, runner record mode | 7 | 01–04 | ✅ 7/7 | — |
| [07-serve-live-host.md](07-serve-live-host.md) | extract `libasmspy`, `--serve` wrapper, session host, budget patch-bay, Inspect door | 7 | 01, 03 | ✅ 7/7 | — |
| [08-observer-views.md](08-observer-views.md) | live views: syscalls, watch, topo, hot edges, tree filters, codeimage, PT slice | 8 | 07, 04, 01 | ✅ 8/8 | — |
| [09-teaching-producers.md](09-teaching-producers.md) | per-step register ring, scrubber, ABI x-ray, blame socket | 5 | 01, 03, 04, 06 | ☑ 5/5 | — |
| [10-spacetime-3d-overview.md](10-spacetime-3d-overview.md) | 3D memory-terrain + execution-trajectory overview surface (**growth-rung companion**) | 7 | 01, 03, 04, 07, 08 | ☑ 7/7 | — |
| [11-imgui-addons.md](11-imgui-addons.md) | Dear ImGui addon research + adoption plan (**research/planning doc, not a brief**) — D2 amended (addon-admission rule, above); Track G + 2 no-dep quick wins landed | — | 01–10 (survey) | planning · G done | — |

### Addon-adoption family (docs 12–17) — implementation briefs cut from doc 11

These six briefs are the implementation-ready form of
[11-imgui-addons.md](11-imgui-addons.md), one per track in that doc's
*Sequencing* block, each written so a junior developer can open exactly one and
implement it end to end. **Every one is gated on
[12-addon-supply-chain.md](12-addon-supply-chain.md)**, which amends D2 with the
addon-admission rule (above) and builds the shared fetch/pin/license/compile
scaffolding — land 12 first, then the rest parallelise as the *Depends on* column
shows. Status reflects committed `main`; the D2 amendment (12-T1) and the two
no-dependency quick wins (14-T1 diff-fix, 14-T2 `theme.h`) are already in flight
per the doc-11 row above.

| Doc | Area | Tasks | Depends on | Status | Claim |
|---|---|---|---|---|---|
| [12-addon-supply-chain.md](12-addon-supply-chain.md) | amend D2 (addon-admission rule); reusable `fetch-addon.sh` + digest/license conventions; imgui-repin compile-gate (**Track G — blocks all**) | 3 | 03 | ☑ 3/3 | — |
| [13-foundation-moves.md](13-foundation-moves.md) | F1 docking repin (`v1.91.9b-docking`) + in-tree layout manager; F2 32-bit `ImDrawIdx`; F3 freetype + JetBrains Mono + Codicons; F4 the 1.92 bump decision (**Track F**) | 5 | 12; 04, 09, 10 | ☑ 5/5 | — |
| [14-quick-wins.md](14-quick-wins.md) | diff "go" bug fix; shared theme header; ProgressBar; imgui_memory_editor; ImZoomSlider; ImGuiTextSelect (v1.1.6+utfcpp); ImGuiFileDialog (v0.6.8) (**Track Q**) | 7 | 12 (T4–T7); 04/05/08 | ☑ 7/7 | — |
| [15-plotting-and-graph-nav.md](15-plotting-and-graph-nav.md) | ImPlot v1.0 chassis (perf_history/hotedges/timeline/watch); imgui_canvas de-risk; imgui-node-editor for topo/tree | 3 | 12, 13 (F1/F2) | ✅ 3/3 (T1 plots ☑ + T2 canvas ☑ + T3 node-editor topo/tree/hot-edges ☑) | — |
| [16-live-feedback-and-filtering.md](16-live-feedback-and-filtering.md) | ImGuiNotify toasts (live-session events); ImSearch client-side filtering (**both ∅-unverified in doc 11 — compile-check at pin**) | 2 | 12; 13 F3 (Notify) | ✅ 2/2 (T1 toasts `1a9d6d5` + T2 filter `a728d9d`) | — |
| [17-interaction-testing-and-editor.md](17-interaction-testing-and-editor.md) | imgui_test_engine (interaction tests + keymap enforcement, test-lane-only); goossens ImGuiColorTextEdit + TextDiff (Author editor, disasm gutter, side-by-side diff) (**Bigger bets**) | 2 | 12 | ✅ 2/2 (T2 editor `fdb5783` + T1 engine/keymap `245ad3f`/`243f092`) | — |

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
[UX restructuring plan](../plans/desktop-gui-ux-restructure-plan.md) (which
follows the 24-finding [heuristic review](../plans/desktop-gui-ux-review.md)),
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
| [18-breach-stops.md](18-breach-stops.md) | 0 | keymap honesty-remainder + convention keys, real Reset, Author save-guard, capability positives, perturb-confirm, nav back/forward (F1,F18,F2,F24,F19,F22,F11) | 6 | 12 (test-engine, for tests); mostly independent of 19 | ✅ 6/6 | — |
| [19-dockable-panes-keystone.md](19-dockable-panes-keystone.md) | 1 | real `kPane*` panes + flatten 3-deep nesting — **the keystone**, unblocks 20/21/22 (F2,F4,F9) | 3 | 13 F1 (docking, landed); 18 T2.2 alongside | ✅ 3/3 | — |
| [20-workspace-and-settings.md](20-workspace-and-settings.md) | 1 | data-driven tabs, task-language entry rail, workspace persistence + recents, perspectives, Settings/DPI/text-scale (F4,F13,F10,F16,F6) | 5 | 19 (panes); 13 F3 (fonts, for Settings) | ✅ 5/5 | — |
| [21-spine-navigation.md](21-spine-navigation.md) | 2 | command palette, wayfinding breadcrumb, overview/minimap (F8,F9) | 3 | 19 (panes); 15 + 14 T5 (minimap); 16 (ImSearch) | ✅ 3/3 | — |
| [22-selection-and-search.md](22-selection-and-search.md) | 2 | shared brushing-and-linking selection, keyboard islands, global find, app-level undo (F7,F18,F17,F12) | 4 | 19 (panes); 16 (ImSearch); 21 T3 (minimap) | ✅ 4/4 (T1–T4 landed 2026-07-27) | — |
| [23-graded-truth-layer.md](23-graded-truth-layer.md) | 3 | graded 3-tier honesty chrome + schema `severity`, session-end placard, split "paused", progress everywhere (F5,F20,F23,F21) | 4 | 01 (**schema-freeze coordination for T1**); 24 T5.1 (palette); 16 T1; 14 T3 | ✅ 4/4 (T1–T4 landed 2026-07-27) | — |
| [24-one-visual-language.md](24-one-visual-language.md) | 3 | semantic palette (extend `theme.h`), CVD-safe + second channel, glossary/term registry, unified filter/time, Loom/3D primer (F3,F14,F15,F16,F4) | 5 | 16 (ImSearch); 15 (ImPlot colormap); T5.1 precedes 23 T1 | ✅ 5/5 (T1–T5 landed 2026-07-27) | — |
| [25-live-model-wiring.md](25-live-model-wiring.md) | 4 | promote the growing capture into the workspace model so Loom / Slice / Timeline / 3D go live, not just Observer — closes the 2026-07-27 live-vs-replay audit gap; Scrubber was replay-only until doc 26 landed the live `regstate` producer | 7 | 20 T1 (`view_presence`); 07/08 (live host + observer deck) | ✅ 7/7 (T6 completed 2026-07-28: the live single-step 3D overlay — `build_trajectories` weaves the `df_step` offset stream as a region-relative, per-tid path when no `trace` is present) | — |
| [26-live-regstate-producer.md](26-live-regstate-producer.md) | 4 | live `regstate` producer on the serve/`--dataflow` single-step engine (it already `PTRACE_GETREGS` every step) → the Scrubber goes live; the last live-vs-replay gap. Consumer already done (doc 25); producer + serve opt-in + emulator parity | 5 | 25 (consumer wiring); 07 (serve); the `--dataflow` ptrace engine | ✅ 5/5 (T1–T5 landed 2026-07-28; `--dataflow --steps` + serve `steps:true` arm the `user_regs@x86_64/sysv` ring; emulator-parity green) | — |

30 tasks across docs 18–24. Sequencing follows the plan's four waves:
**Wave 0 (doc 18)** stops active breaches and is mostly independent of the
docking refactor — ship it first. **Wave 1 (docs 19, 20)** is the structural
unlock: doc 19 (the keystone) converts the orphaned `kPane*` docking into real
panes and unblocks the rest; land doc 18's real-Reset alongside so a bad `.ini`
cannot strand the user. **Wave 2 (docs 21, 22)** exposes and operates the
`dt_nav_go` spine on the real panes. **Wave 3 (docs 23, 24)** does the systematic
truth-layer + visual-language depth; **doc 24 T5.1 (semantic palette) lands
before doc 23 T1 (honesty tiers)**, and doc 23 T1's `severity` field is a schema
change that must coordinate with doc 01's Phase-3 freeze (D5). Nothing here
removes a truth — the honesty chrome is *restructured* (F5), never removed.

### Extension family (docs 27–33) — unblocking the deferred views

The ~30 DEFERRED / BLOCKED / REFUSED markers across docs 01–26 and the plan's
Honest-limits / Killed-in-grounding lists collapse onto **six root
prerequisites** — one engine or schema change apiece, each fanning out to
several stuck views. [27-extension-roadmap.md](27-extension-roadmap.md) is the
family overview (mapping + dependency graph + sequencing, like doc 11 for its
addon family); docs 28–33 are the full per-root briefs. Authored 2026-07-28
against HEAD `da566c9`; **not yet implemented** (planning). Excludes what already
shipped (live `regstate` doc 26, `severity` doc 23, offline scrubber doc 09).

| Doc | Root | Prerequisite | Tasks | Depends on | Status | Claim |
|---|---|---|---|---|---|---|
| [27-extension-roadmap.md](27-extension-roadmap.md) | — | family overview + dependency graph (**not a brief**) | — | 01–26 (survey) | overview | — |
| [28-schema-freeze-completion.md](28-schema-freeze-completion.md) | R1 | `code` header (routine hash), footer `steps_total`, serialize `wide[]` | 3 | 01 (**Phase-3 freeze, D5**) | ✅ 3/3 (T1 `5803086` + T2 `47c3280` + T3 `878de40`) | — |
| [29-mem-address-stream.md](29-mem-address-stream.md) | R2 | the reserved `mem` kind — no producer today | 3 | 01; 10 (consumer, inert-ready) | ✅ 3/3 (T1 writer+schema+unit, T2 emulator projection + golden `mem-df-chain`(+torn), T3 live `--dataflow --mem`/serve `mem:true` + `test_mem_parity`) | — |
| [30-resume-from-state-and-reweave.md](30-resume-from-state-and-reweave.md) | R3 | route the value producer through `emu_snapshot`/`emu_restore` | 4 | 05 (forks); 28 T1 (for T4) | ✅ 4/4 (T1+T2 `emu_t`-hosted producer byte-identical via R5's `df_guest` seed/capture seam, `6d16cac`; T3 Reweave — `loom_take_run_from_step` fork-from-K stitched worldline, `cc4ad3b`; T4 `dt_scrubber_replayable` retires the "not a day-one feature" refusal with a synthesized+bannered register history, `8f38b81`) | — |
| [31-wide-register-deck.md](31-wide-register-deck.md) | R4 | `fpenv` kind + XMM/YMM/MXCSR capture + SSE-class args | 3 | 28 T3 (`wide[]` format); 01/D5 | ✅ 3/3 (T1 XMM+MXCSR deck both producers + `movq` parity; T2 `fpenv` decode + honest degradation; T3 `run_fp` SSE args + XMM `df_step` values + `fp-scale-add` golden; 128-bit only, YMM deferred) | — |
| [32-per-guest-value-producer.md](32-per-guest-value-producer.md) | R5 | arch-parameterize `dataflow_emu.c` (arm64 first) | 3 | independent axis; demand-gated | ✅ 3/3 (T1 `df_guest` vtable, byte-identical x86-64; T2 arm64 value fabric + golden `arm64-df-chain`, PLUS its `regstate`/Scrubber sub-item — `emu_arm64_t` gains its own per-step register ring mirroring `emu_x86_regs_t`'s, `2f9f071`, zero change to `emu_t`/Reweave; T3 Author-mode arm64 run — the door dispatches arm64 through the value-fabric producer with an honest, distinct result shape + a materialised `trace`/`df_step`/`df_edge` recording + the accurate gate-flip label, `2873973`; see brief status) | — |
| [33-backward-attribution-producers.md](33-backward-attribution-producers.md) | R6 | the reserved `blame` + `statediff` kinds | 2 | 28 T1 (statediff pairing); 05 (L1 edges) | ✅ 2/2 (T1 `blame` cone + born-untraced honesty, T2 `statediff` delta + two-recording merge gated on R1 identity; recorder-only producers, asmspy leg deferred) | — |

18 tasks across docs 28–33. Suggested order (per the roadmap): **R1 first**
(cheapest, no new engine, prerequisite for R4/R6, closes 04/05 honesty gaps —
land it through the Phase-3 freeze), then **R2 and R6 in parallel** (new
producers over an existing recording; R6's `statediff` waits on R1 T1), then
**R4** (reuses R1's `wide[]` format — **✅ landed 2026-07-28**: the XMM/MXCSR deck +
`fpenv` on both producers, 128-bit only with YMM deferred), then **R3** (the
headline — it is what "not a day-one feature" gates — largest, lower urgency), with
**R5** an independent demand-gated arch axis. The plan's permanent Honest-limits (exact-only fabric, no
cross-thread hops, forks-never-touch-live, statistical-absence-proves-nothing)
are out of scope by design — a brief there would fight the design.

**Follow-on briefs (beyond the six roots).**
[34-playhead-and-scene-reach.md](34-playhead-and-scene-reach.md) — close the three
seams in the *"pick a process → watch the 3D graph change over time"* flow: put the
register Scrubber on the shared **execution-step** brush (T1), give the 3D overview a
`5` keyroute + a "View in 3D" handoff from the Live-capture pane (T2), add a
**play/pause transport** to each playhead (T3), and name each axis so the two
playheads read as different axes, not a faked global clock (T4/honesty). A pure
`ShellState`/`HudState` desktop brief — no producer/engine/schema change, driven by
the null backend (D4). Authored 2026-07-28 against HEAD `326afde`; **implemented
2026-07-28 (`e962fb5`)** — all five tasks (execution-step brush, `5`/handoff reach,
per-axis play/pause transports, axis labels), both docker-desktop lanes green.
☑ 5/5 · —.

[35-continuous-live-dataflow.md](35-continuous-live-dataflow.md) — make the live
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
`continuous` checkbox + the hand-authored `dishonest/continuous-df.asmtrace`
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

[36-anchor-the-3d-plane.md](36-anchor-the-3d-plane.md) — **place a
routine-relative path on the 3D plane, or say why not.** The 3D overview places
geometry only for a `basis:"abs"` recording, and the only producer of
`basis:"abs"` in the tree is the synthetic golden-scene generator
(`record_scene_abs`) — every real capture (live `trace`, live `dataflow`/`auto`,
every corpus file) is `basis:"rel"`, so its plane comes up **empty and
unlabelled**: the "3D overview" tab opens onto nothing after a live attach, with
no error. Three defects stack: the rel chip [25](25-live-model-wiring.md) T6
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
unanchored one and any individually-unplaced vertex still cannot (with the honest
limit that `df_step` carries no `tid`, so a live dataflow capture is one trajectory
and yields no marks regardless). Pure `space/` + HUD: no engine, no GL, no wire
change, no existing golden regenerated. Authored 2026-07-29 against HEAD `24778e4`.
◐ 3/5 (T1 `resolve_anchor`; T2 the rel PC path is ANCHORED onto the plane —
`base+off`, `TRAJ_ANCHORED` alongside `TRAJ_RELATIVE_BASIS`, placement counted,
every anchored vertex projects, and `placement_note` states any shortfall/refusal
— plus `TrajPoint::placed` for T5; T3 terrain gains a labelled `df_step`
single-step-residency height rung + anchors the trace rung, with `height_source`/
`height_note`/`anchor_error` and a "steps without cells are explained" bar) ·
will · T4–T5 · 2026-07-29.

[37-region-tag-on-df-step.md](37-region-tag-on-df-step.md) — **the producer half of
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
severable. Authored 2026-07-29 against HEAD `81e6ade`. ☐ 0/6 · *free*.

71 tasks across the ten core docs (01–10). Suggested start order: 01 and 03 in parallel (03's
T1–T6 need no corpus), then 02/04, then 05/06/07 in parallel, then 08, then
09 (09-T1 — the emulator ring — is engine-only and can start any time).
**All ten docs have landed.** 01–08 landed 2026-07-24; 2026-07-26 landed all of
doc 09 — register ring (T1), regstate recorder (T2), scrubber (T3), ABI x-ray
(T4), blame socket (T5) — and all of doc 10 — Hilbert projection (T1), terrain
builder (T2), trajectory builder (T3), GL scene (T4), live-observer overlay
(T5), drill-in + honesty invariants (T6), golden scenes + gated GL lane + docs
(T7). **Integration surfacing pass, 2026-07-26:** the two doc-09 teaching views
are now **hosted in visible shell panes** — the register scrubber as a
per-recording `Scrubber` tab (its `regstate` seek index built at open, its
playhead persisted per recording), and the ABI x-ray as an `ABI x-ray` tab that
locks the active recording (the SysV leg) against the attached B (the Win64 leg,
the `d` binding), reusing the Diff tab's A/B mechanism; both degrade to their own
honesty placards (absent producer, unaligned pair, torn ring) exactly as their
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
A codeimage-less recording takes an honest "no address-space regions" placard
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

- [../plans/desktop-gui-ux-review.md](../plans/desktop-gui-ux-review.md) — the
  findings (F1–F24), method, severities, strengths, sources.
- [../plans/desktop-gui-ux-restructure-plan.md](../plans/desktop-gui-ux-restructure-plan.md)
  — the roadmap: five themes, four waves, and which tracks (13/14/15/16/17) to
  redirect vs. new briefs to cut.

**Briefs cut 2026-07-27.** The roadmap's 28 unbuilt recommendations are now
implementation-ready briefs — **docs 18–24** above, one per wave/theme cluster,
each tracing every task to its `T<theme>.<n>` recommendation and `F<n>` finding.
This was a drafting-only pass (no code); the keystone is doc 19 (real dockable
panes). The addon-adoption family (docs 12–17) is now fully landed — **15 T3**
(node-editor topo/tree/hot-edges) was the last item. The [asmspy](../plans/asmspy-plan.md) / [desktop-gui](../plans/desktop-gui-plan.md)
product plans are unchanged.
