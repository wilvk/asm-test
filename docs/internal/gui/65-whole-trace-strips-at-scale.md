# The whole-trace strips tell the truth at scale — implementation

> **Source.** Findings **#4, #5** of the 2026-08-07 GUI/asmspy visualisation review,
> cut per [62-encoding-integrity-roadmap.md](62-encoding-integrity-roadmap.md) §3.
> Both are **defects**, not refinements: two widgets stop conveying their data above
> a trace size the app routinely handles.
>
> Read [\_conventions.md](../implementations/_conventions.md) first; D1–D11 live in
> this directory's [README](README.md). **Prerequisites: none.** Pure model + draw
> half; no engine, wire or schema change.
>
> Authored 2026-08-07 against `35ef821f`, every citation verified; re-checked at
> `3e2e8cea`, which touches no file cited here. If a cited `file:line` disagrees
> with the code, the code wins — re-verify, then fix this doc in the same change.
>
> **Status — ☐ 0/4.**

## Why this work exists

**#4 — the overview strip has no bucketing.** `OverviewStrip::buckets` is named for
an aggregation that does not happen: `overview_from_timeline` pushes **one bucket per
timeline row** ([overview.cpp:20-25](../../../desktop/src/views/overview.cpp#L20)),
and the draw half feeds every one of them to `ImPlot::PlotBars(..., 0.9)`
([timeline_draw.cpp:47-48](../../../desktop/src/views/timeline_draw.cpp#L47)) on an
axis spanning `[0, nsteps]` ([timeline_draw.cpp:46](../../../desktop/src/views/timeline_draw.cpp#L46)).

`0.9` is a bar width in **step units**. At 100 000 steps in a 1000px strip one step
is 0.01px, so each bar is ~0.009px wide: what renders is a rasterisation artefact of
which steps happened to land on a pixel boundary, not a density. It is also 100 000
bars submitted per frame for a 48px-tall widget
([timeline_draw.cpp:41](../../../desktop/src/views/timeline_draw.cpp#L41)).

The strip is *"always-visible"* by design (doc [21](../archive/gui/21-spine-navigation.md)
T3) and is the one widget whose whole job is to survive a large trace. It is the one
that does not.

**#5 — the hot-edge heatmap suppresses its own key.** `draw_obs_hotedges` builds a
24×24 src×dst matrix whose row and column **labels are already returned**
(`HotEdgeMatrix::rows` / `::cols`, [hotedges.h:84-89](../../../desktop/src/views/hotedges.h#L84),
built at [hotedges.cpp:319-349](../../../desktop/src/views/hotedges.cpp#L319)) and
then draws it with `ImPlotAxisFlags_NoDecorations` on **both** axes
([observer_draw.cpp:460-462](../../../desktop/src/views/observer_draw.cpp#L460)).

The comment says the names *"live in the table below, so long labels don't fight the
grid"* ([observer_draw.cpp:441](../../../desktop/src/views/observer_draw.cpp#L441)) —
but the table is ordered by **edge rank**, not by matrix row or column, so there is
no mapping from a bright cell back to a function. A reader can see that some edge is
hot and cannot learn which. The colour scale is also linear over `[0, maxc]`
([observer_draw.cpp:463-465](../../../desktop/src/views/observer_draw.cpp#L463)), and
sampled edge counts are heavy-tailed, so one dominant edge pushes the rest into the
bottom Viridis band.

## What already exists (verified 2026-08-07)

- **The sparse-stays-sparse rule is the model's stated contract and is pinned.**
  `overview.h` promises *"one bucket per step the recording actually recorded, never
  a padded `[0, nsteps)` range invented to look complete"*
  ([overview.h:8-11](../../../desktop/src/views/overview.h#L8)), and
  [test_overview.cpp:52-61,110-117](../../../desktop/test/test_overview.cpp#L52)
  asserts *"one bucket per real row"*, *"never padded"* and *"a dropped step is
  weight 0, not fabricated activity"*. **T1 must not weaken any of this** — see its
  design note.
- **The click mapping is already exact and tested.** `overview_click_step` rounds
  `frac × nsteps` and clamps ([overview.cpp:62-76](../../../desktop/src/views/overview.cpp#L62));
  a minimap click and a typed go-to land on the same step by construction (D4).
  T2 must keep routing through it and must **not** re-derive a step from a bin index.
- **`HotEdgeMatrix` already carries labels and a truncation flag**
  ([hotedges.h:84-89](../../../desktop/src/views/hotedges.h#L84)); `truncated` is
  already surfaced in the caption ([observer_draw.cpp:448-450](../../../desktop/src/views/observer_draw.cpp#L448)).
  T3 needs no new model field.
- **Viridis is already the colormap and the `ColormapScale` is already the second
  channel** ([observer_draw.cpp:456,471](../../../desktop/src/views/observer_draw.cpp#L456)) —
  perceptually uniform and CVD-safe, chosen deliberately over the default rainbow.
  T4 changes only the *mapping* onto it, never the colormap.
- **The ImPlot-context guard is the established degradation seam.** Both widgets
  already branch on `ImPlot::GetCurrentContext()` and fall back to text
  ([timeline_draw.cpp:30,71-76](../../../desktop/src/views/timeline_draw.cpp#L30);
  [observer_draw.cpp:442](../../../desktop/src/views/observer_draw.cpp#L442)), which
  is what lets the pure models be tested with no context. Every task keeps it.
- **`overview_from_fabric`** ([overview.cpp:30-60](../../../desktop/src/views/overview.cpp#L30))
  produces one bucket per recorded step and is drawn by the Loom's own strip. It has
  the **same** scale problem and T1's helper is shared by both.

## Fidelity rules (binding on every task)

1. **A bin with no real steps draws nothing.** Not a zero bar — nothing. The
   difference between "no activity here" and "no recorded step here" is exactly what
   `overview.h`'s no-padding rule protects, and binning is the obvious way to lose it.
2. **A dropped step keeps its bin membership at weight 0.** It is a real step index
   whose activity is unknown ([overview.cpp:21](../../../desktop/src/views/overview.cpp#L21));
   it contributes to a bin's `n_real` and 0 to its weight, so a bin of all-dropped
   steps renders as a real-but-empty bin, never as a gap.
3. **The click stays exact.** T2 maps the click through `overview_click_step` on the
   original strip. A bin index is a display artefact and must never become a
   navigation target.
4. **An aggregation states its factor.** When binning bites, the strip says so — in
   the same voice as the diff view's *"showing the N largest of M"* note
   ([diff_view.cpp:117-121](../../../desktop/src/views/diff_view.cpp#L117)). A silent
   aggregation reads as raw data.
5. **A colour scale states its transform.** T4's `ColormapScale` label changes with
   the mapping. An unlabelled log scale is worse than a linear one.
6. **Statistical stays statistical.** The heatmap's `STATISTICAL` label, evidence
   line, chrome line and no-flame-graph note ([observer_draw.cpp:429-434](../../../desktop/src/views/observer_draw.cpp#L429))
   are untouched. Nothing here makes a survey read as exact.

## Tasks

### T1 — An additive display binning that cannot pad (M)

**Design note — do not modify `overview_from_timeline`.** Its no-padding guarantee is
the model's contract and is pinned by three assertions. Binning is a *display*
projection over the strip, so it is a second pure function, and the fidelity rule
lives in its own shape.

1. Add to [overview.h](../../../desktop/src/views/overview.h):

   ```
   struct OverviewBin {
       uint32_t step_lo = 0, step_hi = 0; // the step span this bin covers
       uint64_t weight = 0;               // summed bucket weight in the span
       uint32_t n_real = 0;               // real buckets folded in (0 = empty)
   };
   // Fold `s` into at most `target` equal step-spans. A span containing no real
   // bucket yields a bin with n_real == 0, which the draw half SKIPS — a sparse
   // trace stays sparse (overview.h's rule, now at bin granularity). Returns the
   // buckets one-to-one when buckets.size() <= target, so a small trace is
   // byte-identical to today.
   std::vector<OverviewBin> overview_bins(const OverviewStrip &s, int target);
   ```
2. Implement it in [overview.cpp](../../../desktop/src/views/overview.cpp) as a
   single pass over `s.buckets` (already in step order), so it is O(buckets).
3. Extend [test_overview.cpp](../../../desktop/test/test_overview.cpp) with the four
   cases that matter: identity below the target; a sparse strip whose empty spans
   yield `n_real == 0` bins (rule 1); an all-dropped span yielding
   `n_real > 0, weight == 0` (rule 2); and summed weight equal to the raw total.

**Done when** every existing `test_overview` assertion still passes untouched and the
four new cases pass.

### T2 — Draw the binned strip at bin width, with a stated factor (S)

1. In [timeline_draw.cpp](../../../desktop/src/views/timeline_draw.cpp), call
   `overview_bins(strip, target)` where `target` is derived from the plot width —
   roughly one bin per 2px, clamped to `[64, 1024]`. Compute it from
   `ImGui::GetContentRegionAvail().x` before `BeginPlot`.
2. Feed `PlotBars` the bin **midpoints** and a bar width of the bin span, not `0.9`.
   Skip every `n_real == 0` bin — do not emit a zero-height bar for it (rule 1).
3. When `bins.size() < strip.buckets.size()`, extend the existing caption
   ([timeline_draw.cpp:29](../../../desktop/src/views/timeline_draw.cpp#L29)) to say
   so: *"overview (whole trace, N steps per bar) — click to jump"* (rule 4).
4. Leave the click path alone: it already computes an x-fraction from the plot rect
   and routes through `overview_click_step`
   ([timeline_draw.cpp:57-68](../../../desktop/src/views/timeline_draw.cpp#L57)),
   which is bin-independent and stays exact (rule 3).
5. Apply the same three changes to the Loom's strip in
   [fabric_imgui.cpp](../../../desktop/src/loom/fabric_imgui.cpp) — it consumes
   `overview_from_fabric` and has the identical problem.
6. The no-ImPlot text fallback ([timeline_draw.cpp:71-76](../../../desktop/src/views/timeline_draw.cpp#L71))
   keeps reporting the **raw** bucket count, not the bin count: it is a statement
   about the recording, not about the widget.

**Done when** a `desktop-ui-test` case over a 100 000-step synthetic strip asserts
the drawn bar count is bounded by the target (it is ~100 000 today) and that a
sparse strip's empty spans emit no bars.

### T3 — Give the heatmap its axes back (S)

1. Replace `ImPlotAxisFlags_NoDecorations` on both axes
   ([observer_draw.cpp:460-462](../../../desktop/src/views/observer_draw.cpp#L460))
   with `ImPlot::SetupAxisTicks` fed from `hm.rows` / `hm.cols`, which
   `obs_hotedges_matrix` already returns. 24×24 fits; the cap is already the
   mechanism that keeps it fitting ([observer_draw.cpp:443](../../../desktop/src/views/observer_draw.cpp#L443)).
2. Where a label is too long for the gutter, ellipsize it **from the middle** — for
   `func+0xNN [module]` and mangled C++ the distinguishing part is the tail. This is
   the same defect [66](66-asmspy-tui-channels.md) T4 fixes in asmspy; do the same
   thing here rather than truncating from the right.
3. Add a hover readout: with the plot's mouse position (`ImPlot::GetPlotMousePos`),
   name `from -> to` and the exact cell count in a tooltip. The number is the truth;
   the colour is the gloss — the same rule the in-cell magnitude bar follows.
4. Keep the caption's `truncated` note verbatim
   ([observer_draw.cpp:448-450](../../../desktop/src/views/observer_draw.cpp#L448)):
   labelled axes make it *more* important to say some edges were capped out, not less.

**Done when** `test_obs_hotedges` asserts `rows`/`cols` are non-empty and
order-stable for a fixture with >24 distinct endpoints, and the ellipsize helper is
unit-tested on a name whose tail is its only distinguishing part.

### T4 — Map the heatmap colour through log, and say so (S)

1. Feed `PlotHeatmap` `log1p`-transformed cell values with `scale_min = 0`,
   `scale_max = log1p(maxc)` ([observer_draw.cpp:463-465](../../../desktop/src/views/observer_draw.cpp#L463)).
   Use `log1p` to match `terrain.cpp`'s transform and
   [63](63-magnitude-transform-and-denominator.md) T2's `dt_magnitude_frac_log`, so
   every heavy-tailed magnitude in the app reads on one curve.
2. A zero cell must stay the colormap's zero — `log1p(0) == 0`, so this holds for
   free, but assert it: "no such edge" and "one sample" must not become the same
   colour.
3. Change the `ColormapScale` label from `"samples"` to `"samples (log)"` and pass
   the **untransformed** `[0, maxc]` range so the scale's tick values stay real
   sample counts (rule 5). Verify the tick placement matches the transform; if
   ImPlot's linear scale cannot express it, label the scale with explicit
   `SetupAxisTicks` values at `1, 10, 100, …` instead of letting it interpolate.
4. Keep `PushColormap(ImPlotColormap_Viridis)` exactly as it is (rule from
   [24](../archive/gui/24-one-visual-language.md) T2 — it is the CVD-safe choice).

**Done when** a fixture with counts `{1, 3, 9000}` renders three visibly distinct
cells, which it does not today.

## Testing

All headless: `make desktop-test` covers `test_overview`, `test_timeline`,
`test_obs_hotedges`; `make desktop-ui-test` covers T2's bar bound and T3's ticks. Run
through `make docker-desktop` per [CLAUDE.md](../../../CLAUDE.md)'s tooling rule.

**No golden dump should move.** `overview_bins` is additive and every existing
`test_overview` assertion is required to pass untouched — that requirement is the
proof T1 did not weaken the no-padding contract. If `desktop/test/golden/` churns,
binning leaked into `overview_from_timeline`; move it back out. No `.asmtrace` golden
and no `tests/golden-asmtrace/` file is touched.

## Out of scope

- **The `ImZoomSlider` viewport control** ([timeline_draw.cpp:88-93](../../../desktop/src/views/timeline_draw.cpp#L88))
  is orthogonal and works at any scale.
- **Capping the number of hot edges.** The 24×24 matrix cap already exists and is
  already announced; raising or lowering it is a separate judgement, and this brief
  deliberately makes the existing cap *more* legible rather than changing it.
- **A flame graph.** Still refused, for the reason
  [hotedges.h:3-10](../../../desktop/src/views/hotedges.h#L3) gives: an IBS-Op sample
  observed no call stack, and stacking edges into frames would render inference with
  the same ink as fact. Labelled axes do not change that.
