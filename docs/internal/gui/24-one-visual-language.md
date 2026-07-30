# Wave 3: one visual language — semantic palette, CVD-safe + second channel, glossary/term registry, unified filter/time, Loom/3D primer — implementation

> **Sources.** Actioned from the UX restructure plan
> (../plans/desktop-gui-ux-restructure-plan.md) rows **T5.1–T5.5** and the review
> findings **F14, F15, F3, F16, F4** (../plans/desktop-gui-ux-review.md).
> Written 2026-07-27 against HEAD `243f092`. This doc wins over the review/plan on
> disagreement; the CODE wins over this doc — re-verify file:line before editing.
> Read [_conventions.md](../implementations/_conventions.md) first; D1–D11 live in
> this directory's [README](README.md). **Prerequisites:
> [16-live-feedback-and-filtering.md](16-live-feedback-and-filtering.md)** (ImSearch,
> the client-side narrowing idiom T3/T4 reuse) and
> **[15-plotting-and-graph-nav.md](15-plotting-and-graph-nav.md)** (the ImPlot
> chassis + hot-edge heatmap T2's colormap re-skins). **T1 of this doc is the base
> layer for [23-graded-truth-layer.md](23-graded-truth-layer.md) T4.1 — land T1
> before 23 T4.1.**

## Why this work exists

- **F14 — semantic colour re-invented per pane.** `theme.h` consolidated only two
  colours (warn/refuse); the good/bad/maybe/changed/cone axis was never
  centralised and is drifting exactly like the amber drift `theme.h` claims to
  have ended — three barely-distinguishable yellows mean three different things
  and two reds encode the same "refused" (T1).
- **F15 — no colour-blind-safe palette.** Back-cone vs forward-cone, dim vs hot,
  refused vs ordinary all ride on hue alone; ~5% of users cannot read the core
  slice/diff task, and a deuteranope could read a genuine statistical/truncated
  distinction as normal — fidelity that collapses onto colour is not faithful (T2).
- **F3 — a dense coined lexicon with no in-app glossary, tooltips, or legends.**
  Loom, fabric, patch-bay, hollow, born-untraced, patient-zero, hot-edges surface
  raw; the Sphinx glossary exists but is reachable from no pane and does not even
  define the coined GUI terms. **Chrome the user cannot decode is not faithful** (T3).
- **F16 — filter, sort, and time-position controls differ across every view.**
  Three unrelated filter idioms, no sort where ImGui table sort is free, and five
  different "move through time" controls, so the muscle action for the most common
  trace operation changes per view — and the one deliberately discrete control
  (Invocations) is not marked as intentional (T4).
- **F4 / F3 — the heaviest surfaces front-load for the novice.** Loom and the 3D
  overview drop the learner straight into a dense fabric/terrain with no primer
  and no legend, the "blank multi-panel IDE" the plan promised to avoid (T5).

Per the review's standing note and F5, fidelity (D7) is load-bearing: every task
here **restructures** fidelity chrome into one designed system — none removes or
softens a truth. The graded-severity work is doc 23's; this doc owns the colour,
the words, and the controls that carry those truths.

## What already exists (verified 2026-07-27)

- **`theme.h` holds two colours only.** [`desktop/src/ui/theme.h`](../../../desktop/src/ui/theme.h):18
  `dt_warn_col()` = `ImVec4(0.95, 0.75, 0.25, 1)` and :22 `dt_refuse_col()` =
  `ImVec4(0.95, 0.45, 0.40, 1)`, with :28–33 the packed `dt_warn_u32()` /
  `dt_refuse_u32()` ImDrawList forms. The header comment states its own scope: it
  consolidated the *banner* amber across five draw TUs — the good/bad/maybe/
  changed/cone axis is out of scope and lives inline in each view.
- **The five inline-literal drift sites (F14).**
  - [`inspect_door.cpp`](../../../desktop/src/ui/inspect_door.cpp):91–93 —
    `kGood(0.45,0.80,0.45)`, `kBad(0.90,0.45,0.40)`, `kMaybe(0.90,0.78,0.35)`,
    routed by `verdict_colour()` (:99–101).
  - [`scrubber_draw.cpp`](../../../desktop/src/views/scrubber_draw.cpp):66 — a
    changed register drawn `ImVec4(1.0, 0.85, 0.3, 1)`, inline at the call site.
  - [`abixray_draw.cpp`](../../../desktop/src/views/abixray_draw.cpp):18
    `kChanged(1.0,0.85,0.3)` (same "changed" yellow, re-declared) and :22
    `kDiffer(0.45,0.85,1.0)` (the cross-convention marshalling contrast).
  - [`slice_view_draw.cpp`](../../../desktop/src/views/slice_view_draw.cpp):24–38 —
    `cone_colour()`: back `IM_COL32(120,170,255)`, fwd `IM_COL32(255,180,110)`,
    both white, dimmed grey.
  - [`scene3d/hud.cpp`](../../../desktop/src/scene3d/hud.cpp):16 `kOk(0.55,0.85,0.55)`,
    :22 `kDim(0.65,0.65,0.70)` (kWarn/kBad already alias `dt_warn_col`/`dt_refuse_col`
    at :20–21 — the pattern to extend), region-legend swatches :107–119.
  - Plus [`fabric_imgui.cpp`](../../../desktop/src/loom/fabric_imgui.cpp):159 — the
    Loom refusal placard hard-codes `ImVec4(0.95,0.45,0.40)` (a third copy of the
    refuse red, drifted 0.90-vs-0.95 against `inspect_door`'s `kBad`).
  So three yellows (`1.0,0.85,0.3` changed · `0.90,0.78,0.35` weak · `0.95,0.75,0.25`
  truncated-statistical) and two reds (`0.90…` vs `0.95…` refused) already encode
  overlapping meanings across panes — exactly F14.
- **The good second-channel pattern already ships (F15).** The statistical-vs-exact
  distinction on the 3D trajectories is a *real* second channel, not colour alone:
  [`scene3d/scene.cpp`](../../../desktop/src/scene3d/scene.cpp):282–285 draws a
  statistical layer translucent (`color[3]=0.45` — "never a solid exact tube"), and
  :454–477 feeds a `uStipple` shader uniform so a statistical tube is *stippled* and
  an exact one is *solid*. The Loom mirrors it in 2D: `take_dashed_tail`
  ([`fabric_imgui.cpp`](../../../desktop/src/loom/fabric_imgui.cpp):117–124) draws an
  unaligned tail as dashes ("never drawn as agreement"), while `take_dim` (:109–112)
  is a hollow outline and `take_hot` (:113–116) a filled rect. **This is the pattern
  T2 generalises to every categorical axis.**
- **The Loom dim/hot palette.** [`fabric_imgui.cpp`](../../../desktop/src/loom/fabric_imgui.cpp):19–28
  `kSpan/kSpanDim/kHollow/kHop/kKnot/kHot` as `IM_COL32` constants; :18 already
  reads `dt_warn_u32()`.
- **The ImPlot hot-edge heatmap (doc 15).** doc 15 T1 shipped a src×dst
  `PlotHeatmap` with `ColormapScale` in `views/hotedges`
  ([15-plotting-and-graph-nav.md](15-plotting-and-graph-nav.md):67,99); it takes
  whatever ImPlot colormap is set — today the default, not a CVD-safe one.
- **The one Sphinx glossary.** [`docs/project/glossary.md`](../../../docs/project/glossary.md)
  is a single MyST `{glossary}` directive (`:sorted:`) of `term` / indented-definition
  pairs (AArch64 … ZF). It defines the *domain* vocabulary but **none** of the coined
  GUI terms — Loom, fabric, patch-bay, hollow, born-untraced, patient-zero, hot-edges,
  knot, jack, Reweave, worldline are absent (F3). It is surfaced in no pane.
- **The three filter idioms + the search-list idiom (F16).**
  - ImSearch, Learn only: [`learn_door.cpp`](../../../desktop/src/ui/learn_door.cpp):116–124
    (`BeginSearch`/`SearchBar`/`SearchableItem`/`Submit`), guarded on
    `ImSearch::GetCurrentContext()` so the null backend degrades to a plain list;
    its "stop N of M" text idiom is at :159.
  - Engine-side text on Tree: [`tree.cpp`](../../../desktop/src/views/tree.cpp):47
    `obs_tree_filter_error`, :62 `obs_tree_start_params` — a filter that changes what
    the *engine captures*, not a client-side narrowing.
  - A combo on Backends: [`completeness.cpp`](../../../desktop/src/views/completeness.cpp):39–57
    `BeginCombo("box", …)`.
  - Canvas/Timeline/Observer tables have no filter or sort (ImGui table sort is free).
- **The five time-position controls (F16).** `SliderInt` playheads in
  [`hud.cpp`](../../../desktop/src/scene3d/hud.cpp):78 ("playhead (step)") and
  [`fabric_imgui.cpp`](../../../desktop/src/loom/fabric_imgui.cpp):202 ("playhead"),
  the scrubber/abixray shared slider; prev/next **discrete paging** in
  [`observer_draw.cpp`](../../../desktop/src/views/observer_draw.cpp):412–428 —
  Invocations, with the comment (:418–420) "Discrete paging, never a scrub … a
  slider would draw that gap as elapsed captured time" (a *deliberate* fidelity
  choice, undistinguished visually); and `InputInt` in disasm
  ([`observer_draw.cpp`](../../../desktop/src/views/observer_draw.cpp):482, "as of
  logical time (0 = latest)").
- **The keymap-help pattern to reuse.** [`views_draw.h`](../../../desktop/src/views/views_draw.h):54–56
  `draw_bindings_help()` is fed from `dt_nav_bindings()`
  ([`nav.h`](../../../desktop/src/nav.h):115–119) so help and keymap cannot drift —
  the *one-source* pattern T3's term registry copies. The shell draws it as a
  "Keyboard bindings" window ([`shell.cpp`](../../../desktop/src/ui/shell.cpp):912).
- **The shell tab strip carries metaphor-only titles.** [`shell.cpp`](../../../desktop/src/ui/shell.cpp):
  "Loom" (:463), "3D overview" (:518), "Diff" (:433), "Scrubber" (:477),
  "ABI x-ray" (:490), "Observer" (:447) — the surfaces T3 re-leads with a domain
  term + metaphor subtitle. There is **no** per-view "?" and **no** first-open
  primer anywhere in `desktop/src/` today (T5 is greenfield on this axis).

## Tasks

### T1 — extend `theme.h` to the full semantic set; delete every inline literal  (L, depends on: —; blocks 23 T4.1)

> **Landed 2026-07-27 — green.** `desktop/src/ui/theme.h` now holds the whole
> semantic axis as named accessors — `dt_good`/`dt_bad`/`dt_maybe` (verdicts),
> `dt_changed`, `dt_cone_{back,fwd,both,dim}`, `dt_selected`, `dt_statistical`,
> each with a paired `_u32` (pure float-pack, static-init-safe) and a
> one-meaning doc comment. `dt_bad_col()` **is** `dt_refuse_col()` — the
> 0.90-vs-0.95 refuse-red split is gone (F14). Every cited drift site is routed
> through the accessors: `inspect_door.cpp` (verdict axis, `kBad` now the shared
> refuse red), `scrubber_draw.cpp` (`dt_changed_col`), `abixray_draw.cpp`
> (`kChanged`→`dt_changed_col`, `kDiffer`→`dt_selected_col`),
> `slice_view_draw.cpp`'s `cone_colour()` (the four `dt_cone_*_u32`),
> `scene3d/hud.cpp` (`kOk`→`dt_good_col`, `kDim`→new `dt_dim_col`), and the Loom
> refusal placard `fabric_imgui.cpp`:159 (the third refuse-red copy →
> `dt_refuse_col`). The Loom-local structural palette (`kSpan`/`kHollow`/`kHot`
> …) stays loom-local by design — it is not the semantic axis. A shared
> `desktop/src/ui/legend.{h,cpp}` (`dt_legend_row` + `dt_semantic_legend` +
> `dt_cone_legend`) renders the palette with a second-channel token beside each
> swatch; the slice explorer uses `dt_cone_legend`. New `desktop/test/test_theme.cpp`
> (wired into `make desktop-test`) pins the accessor values, asserts the three
> former yellows stay distinct and `dt_bad==dt_refuse`, source-lints the drift
> files for any remaining inline colour literal, and smokes the legend headless.
> `make docker-desktop` + `make desktop-test` green. **This unblocks 23 T4.1.**
> Deferred to T2 (not this task): the CVD-safe hue *values* and the contrast
> gate — the accessors are the seam T2 fills in.

**Goal.** Make `theme.h` the ONE place the good/bad/maybe/changed/cone/selected/
statistical axis lives, as named accessors, and delete every inline literal at the
five drift sites (plus the Loom refusal placard). This is the substrate doc 23 T4.1
(graded fidelity chrome) and T2 (CVD) both build on — **land it first.**

**Steps.**
1. In [`theme.h`](../../../desktop/src/ui/theme.h) add named accessors for the full
   semantic axis, each an `ImVec4 dt_*_col()` with a paired `ImU32 dt_*_u32()`
   (same pure-float pack as the existing warn/refuse pair, safe at static-init):
   `dt_good_col` (verdict-yes / attachable), `dt_bad_col` (verdict-no / refused —
   **reuse `dt_refuse_col`'s exact `0.95,0.45,0.40`**, killing the 0.90-vs-0.95
   split), `dt_maybe_col` (weak / uncertain verdict), `dt_changed_col` (per-step
   register delta — the `1.0,0.85,0.3` yellow), `dt_cone_back_col`,
   `dt_cone_fwd_col`, `dt_cone_both_col`, `dt_cone_dim_col` (the four cone hues),
   `dt_selected_col`, `dt_statistical_col` (alias `dt_warn_col` if it must equal the
   amber, but *named* so a later graded-fidelity change (23 T4.1) can move statistical
   off the caution amber without touching call sites). Keep each accessor's doc
   comment stating its ONE meaning (the F14 discipline).
2. **Delete the literals, route through the accessors:**
   - `inspect_door.cpp`:91–93 → `verdict_colour()` returns `dt_good/bad/maybe_col()`.
   - `scrubber_draw.cpp`:66 → `dt_changed_col()`.
   - `abixray_draw.cpp`:18 `kChanged` → `dt_changed_col()`; :22 `kDiffer` →
     `dt_selected_col()` (the marshalling-contrast tint is a *selection/pairing*
     highlight — give it the shared selected accessor, not a fourth bespoke blue).
   - `slice_view_draw.cpp`:24–38 `cone_colour()` → the four `dt_cone_*_u32()`.
   - `hud.cpp`:16/:22 `kOk`/`kDim` → `dt_good_col()` / a new `dt_dim_col()`; leave
     :20–21 as they already alias the shared colours.
   - `fabric_imgui.cpp`:159 refusal placard → `dt_refuse_col()`; audit :19–28's
     `kHot`/`kSpanDim` — `kHot`→`dt_hot`/keep loom-local only if genuinely
     Loom-specific, but the *refuse* red must be shared.
3. **Add a shared legend component** in a new `desktop/src/ui/legend.{h,cpp}`:
   `dt_legend_row(ImU32 col, const char *shape_or_pattern, const char *label)` and
   a `dt_semantic_legend()` that renders the whole palette with its meanings. Every
   encoded view calls into it instead of hand-rolling a swatch row (hud.cpp's
   region legend :107–119 is the model to generalise). The legend renders the
   swatch AND the T2 second-channel token, so legend and encoding cannot drift.
4. `theme.h` stays engine-free and addon-free (imgui.h only) — it links into
   `desktop`, `desktop-render`, and the null test backend alike (D4).

**Tests.** `desktop/test/test_theme.cpp` (new), null backend:
(a) assert each semantic accessor exists, returns its documented canonical value,
and that `dt_bad_col()==dt_refuse_col()` and the three former yellows are now the
*named* distinct constants (no accidental collision, no accidental merge);
(b) a **source-lint assertion** — the test reads the five named source files from
the tree and asserts they contain no raw `ImVec4(` / `IM_COL32(` semantic-colour
constructor at the cited sites (regex over the file text; the only colour tokens
left are `dt_*_col`/`dt_*_u32` calls). This is a model/source assertion, not a
pixel check (D4). Wire into `make desktop-test`.

**Docs.** CHANGELOG `Changed`: "desktop: one semantic colour palette in `theme.h`
(good/bad/maybe/changed/cone/selected/statistical) + shared legend; per-pane colour
literals removed." `desktop/README.md`: a short "semantic palette" note pointing at
`theme.h` as the single source.

**Done when.** every named drift site reads from a `theme.h` accessor; no semantic
colour literal remains at the cited lines; the shared legend component exists and is
used by ≥1 view; `test_theme` is green; the palette compiles into the render-only
viewer with zero engine deps.

### T2 — one CVD-verified palette, every categorical distinction backed by a second channel  (M, depends on: T1, 15)

> **Landed 2026-07-27 — green.** A pure `desktop/src/ui/cvd.{h,cpp}` simulates
> protan/deuter/tritan (Machado 2009 severity-1) and computes WCAG contrast, no
> ImGui context and no engine. The T1 accessors keep their hues (they are already
> a separated categorical set; changing byte-exact cone values would cascade
> through the golden/byte tests for no fidelity gain), and the F15 fix is carried
> by the SECOND CHANNEL per the brief's own test (c): a shape glyph beside every
> cone node (`slice_view_draw.cpp`), the Loom take axis read by fill pattern with
> a named inline legend `dt_loom_take_legend` (`fabric_imgui.cpp` — `kHot` now
> routes through a shared `dt_hot_*` accessor), and a CVD-safe Viridis colormap +
> its `ColormapScale` on the hot-edge heatmap (`observer_draw.cpp`). The ONE
> encoding table lives in `ui/legend.cpp` (`dt_encoding_table`) and drives both
> the legend and the test, so the legend is the proof no distinction is
> colour-only. `theme.h` gains `dt_panel_bg_col` + the `dt_warn_large_text_only`
> policy marker. New `desktop/test/test_cvd.cpp` (on `make desktop-test`) asserts
> every categorical entry has a non-empty second channel, contrast ≥4.5:1 text /
> ≥3:1 fills at the smallest font (amber marked large-text-only at ≥3:1), and that
> each CVD pair is separable OR covered by a distinct second channel — passing
> *because of* the redundancy (green/red is asserted to collapse under
> deuteranopia, proving the test is not vacuous).

**Goal.** Make the T1 palette colour-blind-safe *and* redundant: simulate protan/
deuter/tritan over it, verify contrast, and guarantee every categorical distinction
also carries a non-colour channel (shape / pattern / label). Generalise the existing
solid-tube-vs-stipple pattern to the whole encoding, not just statistical-vs-exact.

**Steps.**
1. **Pick the categorical hues on a CVD-verified basis.** Choose the cone-back /
   cone-fwd / good / bad / maybe / selected hues from a known CVD-safe categorical
   set (Okabe–Ito or equivalent) and set them as the T1 accessors' values. Add a
   pure headless helper `desktop/src/ui/cvd.{h,cpp}`: `dt_cvd_simulate(ImVec4, kind)`
   (protan/deuter/tritan matrices) and `dt_contrast_ratio(fg, bg)` (WCAG relative
   luminance). No ImGui context needed — pure math, testable.
2. **Bind a SECOND CHANNEL to every categorical axis** (generalising
   `scene.cpp`'s `uStipple` + Loom's dashed/hollow/filled):
   - **cone direction** → a Codicon/shape glyph per direction (back = ◄ inflow,
     fwd = ► outflow, both = ●) drawn beside the coloured node in
     `slice_view_draw.cpp`, so a monochrome reader still reads direction. (Codicons
     shipped via doc 13 F3.)
   - **dim / hot / neutral** (Loom takes, hot-edges) → fill pattern + label:
     hot = solid fill, dim = hollow outline, neutral = plain — already true for
     `take_dim`/`take_hot`; add the missing **label token** ("hot"/"dim") to the
     legend and to the hover so the channel is nameable, not just visual.
   - **statistical vs exact** → keep stipple/dashed (it is the reference pattern);
     ensure the *2D* surfaces (hot-edges, diff) carry the `[statistical]` text token
     that `diff_view_draw.cpp` already prints, everywhere the amber appears.
   - **the heatmap** (doc 15) → set a **CVD-safe ImPlot colormap** (a
     perceptually-uniform sequential map, e.g. Viridis, which ImPlot ships) on the
     `PlotHeatmap`/`ColormapScale` in `views/hotedges`, replacing the default; the
     `ColormapScale` legend is the heatmap's second channel (magnitude is readable
     off the scale, not the hue).
3. **Verify contrast at the smallest font.** Assert every semantic *text* colour is
   ≥4.5:1 against the panel background and every *fill/border* is ≥3:1, computed by
   `dt_contrast_ratio`. `dt_warn` amber fails 4.5:1 as small body text — mark it
   **large-text-only** in `theme.h`'s doc comment and confine it to headers/banners
   (≥ the large-text size), never small body rows; where amber must label a small
   row, the row also carries the text token (step 2).
4. Extend the T1 shared legend to render the second-channel token next to each
   swatch, so the legend is itself the proof that no distinction is colour-only.

**Tests.** extend `test_theme.cpp` / new `desktop/test/test_cvd.cpp`, null backend —
this is a **headless assert on the encoding, not a pixel check**:
(a) a table `{semantic → (colour, second_channel_token)}` drives both the legend and
the views; assert **every** categorical entry has a non-empty second-channel token
(shape/pattern/label) — a colour-only distinction fails the test;
(b) assert `dt_contrast_ratio` ≥ 4.5:1 for text colours and ≥ 3:1 for fills/borders,
and that `dt_warn` carries the large-text-only marker;
(c) run `dt_cvd_simulate` for the three CVD kinds over the categorical hues and
assert each pair that must be distinguished either stays separable OR is covered by
a second channel (so the test passes *because* of the redundant channel, genuinely,
not by pretending the hues alone suffice).

**Docs.** CHANGELOG `Changed`: "desktop: CVD-safe categorical palette + a second
(shape/pattern/label) channel on every colour-coded distinction; CVD-safe heatmap
colormap; contrast-verified text/fills." `desktop/README.md`: an accessibility note
(palette basis, the second-channel rule, `dt_warn` large-text-only).

**Done when.** the palette is CVD-verified in test; every categorical distinction
has a tested second channel; text ≥4.5:1 / fills ≥3:1 at the smallest font;
`dt_warn` is confined to large text; the heatmap uses a CVD-safe colormap.

### T3 — domain-term-first surfaces + a term registry driven from the ONE glossary  (L, depends on: 16)

> **Landed 2026-07-27 — green.** The coined GUI terms (Loom, fabric, patch-bay,
> hollow span, born-untraced, patient-zero, hot-edges, knot, jack, worldline,
> Reweave, dim/hot take, terrane) are now defined in `docs/project/glossary.md`
> with plain definition + expert synonym; `scripts/gen-terms.py` parses the
> `{glossary}` directive into `ui/terms_generated.h` at build (order-only prereq
> of `terms.o`, generated under `$(BUILD)/desktop/gen`, `-I`'d onto the `ui/`
> path), mirroring the `dt_nav_bindings()`→help pattern — no hand-copied second
> list. `desktop/src/ui/terms.{h,cpp}` wraps it: `dt_term_lookup`, a per-view
> metadata table (`dt_view_meta`: domain term + metaphor + verbatim caveat),
> `dt_view_header(key)` (drawn at the head of the Loom, 3D, hot-edges, slice,
> diff, scrubber, ABI x-ray bodies), a hoverable `dt_term`, a per-view "?" popover
> carrying the caveat (and the T5 primer re-open handle), and a searchable
> **Terms** tab (`dt_terms_pane` — ImSearch in the app, plain list under the null
> backend). New `desktop/test/test_terms.cpp` parses the same glossary the build
> parses and asserts every headword resolves, the coined terms carry their expert
> synonym, and every registered view has a domain/metaphor/caveat. The glossary's
> intro now notes its second consumer. **Note (drift from the brief):** the CODE
> keeps the accessors' hues (T2 banner); everything else follows the brief.

**Goal.** Lead every coined surface with the canonical domain term (metaphor as
subtitle), and surface the ONE Sphinx glossary in-app as a hoverable term registry,
per-view "?", inline legends, and a searchable Terms pane — one source, so
legend / tooltip / teaching cannot drift (the `dt_nav_bindings()`→help pattern,
applied to words). **Chrome the user cannot decode is not faithful** (F3).

**Steps.**
1. **Complete the one source (F3 gap).** Add the coined GUI terms to
   [`docs/project/glossary.md`](../../../docs/project/glossary.md): Loom, fabric,
   patch-bay, hollow span, born-untraced, patient-zero, hot-edges, knot, jack,
   worldline, Reweave, dim/hot take, terrane — each with the plain-language
   definition *and* its expert synonym ("Loom — data-flow lineage (def-use)",
   "patient-zero — first divergence", "patch-bay — tracer contention/budget"). The
   glossary stays the single source of truth (`make docker-docs` still `-W` clean).
2. **Build the in-app term registry from that one file.** A build step parses
   `glossary.md`'s `{glossary}` directive into a generated header (or a fetched-at-
   build data file) `desktop/src/ui/terms_generated.h` — a `{term → definition}`
   table. A small parser (the directive is a regular `term` / indented-lines shape)
   in `scripts/gen-terms.py`, wired into `mk/desktop.mk` as an order-only prereq of
   the desktop objects, mirroring how the keymap help is fed from one table. **No
   hand-copied second list** — the app's words come from the docs' words.
3. **Lead every surface with the domain term, metaphor as subtitle.** In
   `shell.cpp` (tab bodies) and each view's header, render a
   `dt_view_heading("Data-flow lineage", "Loom")` helper (bold domain term, dim
   metaphor subtitle): "Data-flow lineage (Loom)" (:463), "First divergence
   (patient zero)", "Tracer contention (patch-bay)", etc. Tab *labels* stay short
   (strip width, F4); the heading inside the pane carries both.
4. **Hoverable definitions on first occurrence.** A `dt_term(const char *word)`
   wrapper that renders the word and, on hover, `ImGui::SetTooltip` with the
   registry definition; used at the first occurrence of any coined term in each
   view. Degrades silently if the term is absent (never a broken tooltip).
5. **Per-view "?" button.** Add a small "?" affordance to each view header that
   opens a popover stating **what the view shows and what its metric means** —
   verbatim the accurate caveat, e.g. "hot-edges are edge counts, not a call stack",
   "Loom lineage is def-use, not control flow". Store the text next to the view, not
   in prose. (This "?" is also T5's primer re-open handle.)
6. **Inline legend in every encoded view** (reuse T1's shared legend) + a
   **searchable Terms pane**: a new `Terms` surface reusing the Learn ImSearch idiom
   (`learn_door.cpp`:116–124 — `BeginSearch`/`SearchBar`/`SearchableItem`/`Submit`,
   guarded on `ImSearch::GetCurrentContext()` so the null backend degrades to a plain
   scrollable list) over the registry table.

**Tests.** `desktop/test/test_terms.cpp` (new), null backend:
(a) assert the registry is sourced from `glossary.md` — parse the same file the build
step parses and assert the generated table matches it (one source);
(b) assert a coined term resolves: `dt_term_lookup("Loom")` returns a non-empty
definition containing its expert synonym, and that the coined GUI terms added in
step 1 are all present (the F3 completeness gap is closed);
(c) assert every view exposes a non-empty per-view "?" caveat string and a heading
with both domain term and metaphor. The Terms-pane *interaction* (typing to filter)
is an imgui_test_engine flow — add it to `test_ui.cpp` on the `desktop-ui-test` lane
(doc 17 T1); the registry/lookup asserts run on the plain `desktop-test` lane.

**Docs.** CHANGELOG `Added`: "desktop: in-app term registry (glossary-sourced
tooltips), per-view '?' with metric caveats, domain-term-first headings, searchable
Terms pane." Note in `docs/project/glossary.md`'s intro that it now also feeds the
desktop term registry (so an editor knows the file has two consumers).

**Done when.** the glossary defines the coined GUI terms; the app's registry is
generated from that one file (no second copy); coined terms resolve to definitions
on hover; every view has a "?" caveat and a domain-first heading; the Terms pane
filters via the ImSearch idiom.

### T4 — one filter affordance + one time-position widget (two faithful variants)  (M, depends on: 16)

> **Landed 2026-07-27 — green.** `desktop/src/ui/filter.h` is the one affordance:
> a pure case-insensitive `dt_filter_match` / `dt_filter_count` ("showing N of M")
> + a pure `dt_sorted_order` (stable, reorders VIEW indices, never the model), and
> a `dt_filter_bar` draw helper that works under the null backend (plain
> InputText, not an addon). Free ImGui column-sort landed on the hot-edge table
> (`observer_draw.cpp`): `ImGuiTableFlags_Sortable`, keys by column, reordered
> through `dt_sorted_order`. `desktop/src/ui/timepos.{h,cpp}` is the one
> time-position widget: `dt_timepos_scrub` (continuous — now behind the Loom and
> 3D-HUD playheads, replacing their bespoke `SliderInt`s) and `dt_timepos_step`
> (discrete — the Invocations pager, with an always-visible intentional-discrete
> marker + the verbatim reason from the one `dt_timepos_discrete_reason` registry;
> the disassembly logical-time control carries the same marker + registry reason).
> Tree's engine-side filter and Backends' combo are left as-is by design (a
> capture bound vs a selector, not client-side narrowing). New
> `desktop/test/test_filter.cpp` asserts N of M (empty ⇒ N==M, case-insensitive),
> the sort reorders the view while the model is untouched (stable on ties), and the
> discrete surfaces carry non-empty reasons.

**Goal.** One filter idiom on every list/table (type-to-narrow "showing N of M") plus
free ImGui column-sort on the tabular views, and ONE shared time-position widget with
two faithful variants — continuous where a total exists, discrete where it does not —
the discrete case marked as an intentional fidelity choice, not an oversight.

**Steps.**
1. **One filter affordance.** Wrap the Learn ImSearch idiom into a reusable
   `dt_filter_list(...)` (or a thin `ui/filter.h`) that renders a "filter" search
   bar and a **"showing N of M"** count, guarded on `ImSearch::GetCurrentContext()`
   (null backend → plain list). Apply it to every list/table that lacks client-side
   narrowing — Canvas, Timeline, Observer tables, the hot-edge table — replacing the
   ad-hoc idioms where they are *client-side* filters. **Leave Tree's engine-side
   filter (`tree.cpp`:47,62) as-is** — it changes what the engine *captures* and is
   a different, faithful thing; the client-side narrowing sits *on top* (doc 16's
   framing). Backends' combo (`completeness.cpp`:39–57) is a selector, not a filter —
   leave it.
2. **Free column-sort** on the tabular views: add `ImGuiTableFlags_Sortable` +
   `ImGuiTableSortSpecs` handling to the Observer/hot-edge/timeline tables (ImGui
   gives it for free once the flag is set and the model exposes a sort key). Sorting
   reorders the *view*, never the model (D4/D7 — the underlying order stays the
   recorded order).
3. **One time-position widget, two faithful variants.** A shared
   `desktop/src/ui/timepos.{h,cpp}`:
   - `dt_timepos_scrub(label, &t, total)` — a continuous `SliderInt` for surfaces
     where a real total exists (scrubber, abixray, loom, 3D-HUD — replacing the four
     bespoke `SliderInt` playheads at `hud.cpp`:78, `fabric_imgui.cpp`:202, etc.).
   - `dt_timepos_step(label, &idx, count, intentional_reason)` — the discrete
     prev/next pager for surfaces where **no continuous total exists** (Invocations:
     `observer_draw.cpp`:412–428). It renders "#i of N" prev/next AND a small,
     always-visible marker (a Codicon + hover) stating *why* it is discrete —
     verbatim the existing comment's reason ("between two invocations the target ran
     unobserved; a slider would draw that gap as elapsed captured time"). The
     discreteness is thus **visibly intentional**, a fidelity choice, not a missing
     scrubber (F16). Disasm's "as of logical time" InputInt (`observer_draw.cpp`:482)
     is a discrete logical-time step too — route it through the step variant.
   Both variants live in one widget so the muscle action (and the fidelity marker)
   is consistent everywhere.

**Tests.** `desktop/test/test_filter.cpp` + extend `test_obs_draw`/`test_timeline`,
null backend, model state not pixels:
(a) assert the unified filter's model reports the correct **"N of M"** for a known
query (M total, N matching), and that an empty query shows N==M;
(b) assert the sortable tables reorder the *view indices* under a given sort spec
while the underlying model order is unchanged;
(c) assert `dt_timepos_step` carries a non-empty `intentional_reason` for Invocations
(so the discrete case is marked), and `dt_timepos_scrub` is used only where a total
exists. The *typing* interaction is a `desktop-ui-test` flow (doc 17 T1); the N-of-M
and sort model asserts run on `desktop-test`.

**Docs.** CHANGELOG `Changed`: "desktop: one type-to-narrow filter ('showing N of M')
+ column-sort on every table; one time-position widget with continuous-scrub and
marked-intentional discrete-step variants." `desktop/README.md`: the filter + time
conventions.

**Done when.** every list/table filters with the one "N of M" idiom; tabular views
sort; the time control is one widget with two variants; the discrete variant shows
its intentional-fidelity reason.

### T5 — Loom & 3D overview first-open primer + legend  (M, depends on: T1, T3)

> **Landed 2026-07-27 — green.** `desktop/src/ui/primer.{h,cpp}`: a per-view
> `dt_primer_state` (a plain bool, held in `LoomState` and `SceneView`),
> `dt_primer` draws the first-open in-canvas card (title + one-paragraph body +
> the shared legend + "Got it"), and pure predicates `dt_primer_active` /
> `dt_primer_lean` / `dt_primer_dismiss` / `dt_primer_reopen`. Wired into the Loom
> (`fabric_imgui.cpp`: lineage = def-use, the solid/hollow/dashed take channels)
> and the 3D overview (`shell.cpp` `draw_scene_overview`: terrain = address space,
> trajectory = path, exact-vs-statistical, the promoted "3D to find, 2D to read"
> line). Re-open rides the T3 per-view "?" (`dt_view_header(key, on_reopen)`
> clears `dismissed`). Until acknowledged the view holds its lean default
> (`dt_primer_lean`). New `desktop/test/test_primer.cpp` asserts: shows on first
> open, "Got it" hides it, the "?" re-opens it, and the lean gate holds while
> unacknowledged — plus a null-backend draw smoke.

**Goal.** Give the two heaviest surfaces a dismissible, re-openable first-open
in-canvas primer + legend, so a novice is not dropped into a raw fabric/terrain;
front-load nothing heavier than the lean default until the user acknowledges (F4/F3).

**Steps.**
1. **A generic primer component** `desktop/src/ui/primer.{h,cpp}`:
   `dt_primer(id, title, body, legend, bool &dismissed)` renders an in-canvas card
   (over the view's own draw area, not a modal) the *first* time a view opens for a
   session — a one-paragraph "what this is / how to read it" plus the T1 shared
   legend — with a "Got it" dismiss. State (`dismissed`) is per-view, held in
   `ShellState` (persisted per workspace where doc 20's settings exist; a plain
   bool otherwise — never re-nagging within a session).
2. **Wire Loom** (`fabric_imgui.cpp`, `draw_loom`) and the **3D overview**
   (`hud.cpp` / the 3D pane) to open the primer on first draw: Loom's primer
   explains lineage = def-use, the dim/hot/dashed channels, the step window; the 3D
   primer explains terrain = address space, trajectory = execution path, exact vs
   statistical (stipple), "3D to find, 2D to read" (already a HUD string at
   `hud.cpp`:103 — promote it into the primer).
3. **Re-openable via the per-view "?"** (T3 step 5): the "?" popover carries a
   "show primer again" action that clears `dismissed`, so the teaching is never lost
   after first dismiss.
4. **Lean default until acknowledged.** Until the primer is dismissed, keep the
   view at its lean default (do not front-load the audit overlay / heaviest layers);
   this composes with F4's data-gated tabs and doc 20's lean-default work — this task
   only owns the primer + the "don't front-load until acknowledged" gate for these
   two views.

**Tests.** `desktop/test/test_primer.cpp` (new) + extend `test_loom_draw` /
`test_shell`, null backend, model state:
(a) assert the primer's `dismissed` flag is false on first open (primer shows) and
that drawing once + "Got it" sets it true (primer hidden thereafter);
(b) assert the per-view "?" re-open action clears `dismissed` (primer shows again);
(c) assert that while `!dismissed` the view reports its lean-default layer set. The
click interactions (dismiss, re-open) also get an imgui_test_engine flow in
`test_ui.cpp` on the `desktop-ui-test` lane (doc 17 T1); the state transitions run on
`desktop-test`.

**Docs.** CHANGELOG `Added`: "desktop: first-open primer + legend on Loom and the 3D
overview, dismissible and re-openable from the per-view '?'." `desktop/README.md`:
the primer convention.

**Done when.** Loom and the 3D overview show a first-open primer with the shared
legend; it dismisses and stays dismissed for the session; the per-view "?" re-opens
it; the lean default holds until acknowledged.

## Task order & parallelism

**T1 first, and it must land before [23-graded-truth-layer.md](23-graded-truth-layer.md)
T4.1** — the graded fidelity chrome (23 T4.1) re-tiers colours through the same
accessors T1 introduces; doing them in the other order forces a rework. T2 depends on
T1 (it fills in the accessors' CVD-safe values and the second-channel tokens) and on
doc 15 (the heatmap colormap). T3 depends on doc 16 (ImSearch) and is otherwise
independent — it can run parallel to T1/T2. T4 depends on doc 16 (ImSearch) and is
parallel to T3. T5 depends on T1 (shared legend) and T3 (the "?" is its re-open
handle). Suggested order: **T1 → (T2 ∥ T3 ∥ T4) → T5**. Different developers can hold
T2, T3, T4 concurrently once T1 lands.

## Constraints & gates

- **Fidelity (D7) is restructured, never removed.** Every task moves an existing
  fidelity signal into one designed system: T1/T2 keep the statistical amber, the torn
  banner, the "never drawn as agreement" dashes — they only make them consistent and
  colour-blind-safe; T3's per-view "?" carries the verbatim metric caveat; T4's
  discrete time variant makes the *deliberate* discreteness visible rather than
  hiding it; T5's primer front-loads nothing that would overstate the data. No task
  may collapse a real integrity signal onto colour alone (F15) or bury a caveat.
- **View-model purity (D2.4 / D4).** Colours, second-channel tokens, term
  definitions, filter counts, and time-position state are model data asserted
  headlessly; the draw halves consume them. No test checks pixels.
- **`theme.h` stays engine-free and addon-free** (imgui.h only), so it links into
  `desktop`, `desktop-render`, and the null backend identically.
- **One source, always.** T3's registry is generated from `glossary.md`; T1's legend
  is generated from the palette; T4's help stays the `dt_nav_bindings()` pattern —
  no hand-copied second list may exist (the drift F3/F14 are about).
- **Contrast is a gate, not a suggestion** (T2): text ≥4.5:1, fills/borders ≥3:1 at
  the smallest font; `dt_warn` amber is large-text-only.
- **CVD-safe colormap** comes from ImPlot's shipped set (doc 15) — no new dependency.
  Any new addon (unlikely here) clears the D2 addon-admission rule.

## Out of scope

- The **graded severity / tier field** for fidelity chrome (banner-vs-chip-vs-glyph
  loudness) — that is F5, owned by [23-graded-truth-layer.md](23-graded-truth-layer.md)
  T4.1, which builds *on* T1's palette.
- **Data-gated outer tabs** and the lean-default IA at large (F4's tab-overload) —
  owned by doc 20 (workspace/settings) and the spine work; T5 only owns the two
  heavy-view primers and their local don't-front-load gate.
- **Global find / search-as-measurement** (F17 — highlight-all, aggregate cost,
  Enter/Shift+Enter cycling) — a separate item; T3/T4 reuse the ImSearch idiom but do
  not build the measurement affordance.
- **HiDPI / user text-scale / a settings surface** (F6) — owned by doc 20 (T2.8);
  T2's contrast check assumes the shipped 15px baseline as "smallest font".
- Rebindable colours / a theme editor — the palette is fixed and tested; a user
  theme picker is not in this brief.
