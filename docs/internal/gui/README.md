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

## The documents

**Status** tracks task completion as `done/total`; update the cell as tasks
land (legend as in [../implementations/README.md](../implementations/README.md):
☐ · ◐ · ☑ · ✅).

| Doc | Area | Tasks | Depends on | Status |
|---|---|---|---|---|
| [01-asmtrace-format.md](01-asmtrace-format.md) | `.asmtrace` schema, record modes, golden corpus | 8 | — | ✅ 8/8 |
| [02-exporters-and-readers.md](02-exporters-and-readers.md) | speedscope/Perfetto exporters, completeness readers | 6 | 01 (03 for T5–T6) | ✅ 6/6 |
| [03-desktop-shell.md](03-desktop-shell.md) | desktop/ skeleton, deps, mk/desktop.mk, document model | 8 | 01 (corpus, for T7) | ✅ 8/8 |
| [04-replay-views.md](04-replay-views.md) | canvas, operand timeline, slice explorer, diff, deep links | 8 | 01, 03 | ✅ 8/8 |
| [05-loom-day-one.md](05-loom-day-one.md) | the Loom fabric, lineage, lane annex, forks | 7 | 02 (reader), 03, 04 | ✅ 7/7 |
| [06-doors-and-learning.md](06-doors-and-learning.md) | Learn/Author doors, ct_eq, capability panel, runner record mode | 7 | 01–04 | ✅ 7/7 |
| [07-serve-live-host.md](07-serve-live-host.md) | extract `libasmspy`, `--serve` wrapper, session host, budget patch-bay, Inspect door | 7 | 01, 03 | ✅ 7/7 |
| [08-observer-views.md](08-observer-views.md) | live views: syscalls, watch, topo, hot edges, tree filters, codeimage, PT slice | 8 | 07, 04, 01 | ✅ 8/8 |
| [09-teaching-producers.md](09-teaching-producers.md) | per-step register ring, scrubber, ABI x-ray, blame socket | 5 | 01, 03, 04, 06 | ☑ 5/5 |
| [10-spacetime-3d-overview.md](10-spacetime-3d-overview.md) | 3D memory-terrain + execution-trajectory overview surface (**growth-rung companion**) | 7 | 01, 03, 04, 07, 08 | ☑ 7/7 |
| [11-imgui-addons.md](11-imgui-addons.md) | Dear ImGui addon research + adoption plan (**research/planning doc, not a brief**) — D2 amended (addon-admission rule, above); Track G + 2 no-dep quick wins landed | — | 01–10 (survey) | planning · G done |

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

| Doc | Area | Tasks | Depends on | Status |
|---|---|---|---|---|
| [12-addon-supply-chain.md](12-addon-supply-chain.md) | amend D2 (addon-admission rule); reusable `fetch-addon.sh` + digest/license conventions; imgui-repin compile-gate (**Track G — blocks all**) | 3 | 03 | ☑ 3/3 |
| [13-foundation-moves.md](13-foundation-moves.md) | F1 docking repin (`v1.91.9b-docking`) + in-tree layout manager; F2 32-bit `ImDrawIdx`; F3 freetype + JetBrains Mono + Codicons; F4 the 1.92 bump decision (**Track F**) | 5 | 12; 04, 09, 10 | ◐ 3/5 (T1/T3/T5) |
| [14-quick-wins.md](14-quick-wins.md) | diff "go" bug fix; shared theme header; ProgressBar; imgui_memory_editor; ImZoomSlider; ImGuiTextSelect (v1.1.6+utfcpp); ImGuiFileDialog (v0.6.8) (**Track Q**) | 7 | 12 (T4–T7); 04/05/08 | ◐ 3/7 (T1/T2/T5) |
| [15-plotting-and-graph-nav.md](15-plotting-and-graph-nav.md) | ImPlot v1.0 chassis (perf_history/hotedges/timeline/watch); imgui_canvas de-risk; imgui-node-editor for topo/tree | 3 | 12, 13 (F1/F2) | ☐ 0/3 |
| [16-live-feedback-and-filtering.md](16-live-feedback-and-filtering.md) | ImGuiNotify toasts (live-session events); ImSearch client-side filtering (**both ∅-unverified in doc 11 — compile-check at pin**) | 2 | 12; 13 F3 (Notify) | ☐ 0/2 |
| [17-interaction-testing-and-editor.md](17-interaction-testing-and-editor.md) | imgui_test_engine (interaction tests + keymap enforcement, test-lane-only); goossens ImGuiColorTextEdit + TextDiff (Author editor, disasm gutter, side-by-side diff) (**Bigger bets**) | 2 | 12 | ☐ 0/2 |

22 tasks across docs 12–17. Sequencing (doc 11's tracks): **12 (Track G) first**,
then Track Q (14) and Track F (13) in parallel, then the plotting/graph chassis
(15) and feedback/filtering (16) on the foundations, with the bigger bets (17)
independent of each other at any time after 12. Every addon lands with its
`fetch-<name>.sh` wrapper, a `scripts/third-party-digests.txt` row, a `licenses/`
capture, and — for the `imgui_internal.h` dependents — an entry in the doc-12
compile-probe.

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
