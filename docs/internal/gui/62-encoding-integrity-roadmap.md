# Encoding integrity — where the visual discipline has not yet reached

> **A family-overview follow-on** (like [53](53-3d-catalog-build-roadmap.md) for the
> 3D catalog and [43](43-faithful-city-roadmap.md) for the city), over a
> **2026-08-07 code review of the GUI and asmspy visualisation layers**.
> **Not a brief** — it records the ten findings with their measurements, clusters
> them into four implementation-ready briefs by shared *mechanism*, and sequences
> them against each other.
>
> Read [\_conventions.md](../implementations/_conventions.md) first; D1–D11 live in
> this directory's [README](README.md).
>
> Authored 2026-08-07 against `35ef821f`; re-checked at `3e2e8cea`, which touches
> no file cited here. Every citation was verified against the working tree when
> written. If a cited `file:line` disagrees with the code when
> you implement, **the code wins** — re-verify, then fix this doc in the same change.
>
> **Status — ☐ 0/16** across four briefs ([63](63-magnitude-transform-and-denominator.md)
> 4, [64](64-scene-palettes-enter-the-gate.md) 4,
> [65](65-whole-trace-strips-at-scale.md) 4, [66](66-asmspy-tui-channels.md) 4).

## 1. Why this doc exists

Docs [23](../archive/gui/23-graded-truth-layer.md) and
[24](../archive/gui/24-one-visual-language.md) built a real encoding system: one
semantic colour axis with one documented meaning per accessor
([theme.h](../../../desktop/src/ui/theme.h)), a mandatory non-colour second channel
per categorical distinction ([legend.h:45-60](../../../desktop/src/ui/legend.h#L45)),
and a headless gate that measures both ([test_cvd.cpp](../../../desktop/test/test_cvd.cpp)).
That system is not the problem. **The problem is its reach.**

Three surfaces sit outside it, and each was built by a different family at a
different time:

1. **The 3D scene** (docs 44, 51, 54–59) grew its own categorical palettes — 8
   opcode classes and 6 region kinds — which are mirrored between GLSL and C++ with
   a sync test, but **never enter the CVD gate**. → brief [64](64-scene-palettes-enter-the-gate.md)
2. **The magnitude channel** landed as one function
   ([`dt_cell_magnitude_bar`](../../../desktop/src/ui/theme.h#L185), review finding #1)
   and was then adopted by five call sites with **four different denominators and two
   different transforms**, none stated. → brief [63](63-magnitude-transform-and-denominator.md)
3. **asmspy's TUI** predates the whole visual language and has **no colour channel at
   all** and **no magnitude channel at all**. → brief [66](66-asmspy-tui-channels.md)

Plus two contained defects where a widget stops working at real trace scale. →
brief [65](65-whole-trace-strips-at-scale.md)

**This is the D-rule applied outward, not a new rule.** F14 ("one meaning per
colour") and F15 ("never colour alone, and measure it") are already binding; every
task below is one of those two rules reaching a surface that predates it.

## 2. Relationship to the 2026-07-29 UX & dataviz review

The [2026-07-29 UX & data-visualization review](../analysis/2026-07-29-gui-ux-dataviz-review.md)
filed 65 findings over the same surfaces. **This roadmap is not a re-run of it**, and
nothing below duplicates a finding in it — its text was read while cutting this one.
The two are complementary by construction: that review worked from the *rendered
surfaces* outward and is largely about what the app fails to say; this one worked
from the *encoding system* outward and is about where that system does not yet reach.
Its headline finding #1 (in-cell magnitude bars) is **already landed** and is the
idiom [63](63-magnitude-transform-and-denominator.md) consumes rather than re-invents.

**A caution about earlier working-tree drafts.** During 2026-08-01 a set of briefs
was cut from that review into this directory (numbered 46–49, on the fidelity-tiering,
magnitude/comparison and one-colour-language themes). They were **never committed**
and are **no longer present in the tree** as of 2026-08-07. Two of their themes touch
this family, so if that work is re-cut, coordinate rather than duplicate:

| Adjacency | Rule |
|---|---|
| The two-up diverging A/B heat bar | It writes against `dt_cell_magnitude_bar`. **Land [63](63-magnitude-transform-and-denominator.md) T1 first** — it splits that API into rank vs share — so the diverging bar is written against the split rather than migrated after it. |
| Making the Loom fabric palette theme-aware | [64](64-scene-palettes-enter-the-gate.md) does **not** touch the Loom; its scope is the scene's `opClassHue`/`kindHue` only. The Loom's second channel (solid / hollow / dashed fill) already exists and is already named ([legend.h:34-38](../../../desktop/src/ui/legend.h#L34)). |

Numbers **46–49 are already taken** in this directory by the 3D families. A re-cut of
that work needs fresh numbers.

## 3. The ten findings, with their measurements

Ordered by severity. "Measured" means a number was produced from the repo's own
code, not asserted.

| # | Finding | Evidence | Brief |
|---|---|---|---|
| **1** | The scene's 8 opcode-class and 6 region-kind hues never enter the CVD gate. Measured with the repo's own `dt_cvd_distance` against its own 0.15 bar: **7 (pair, deficiency) failures**, worst **Heap vs Data = 0.053 under protanopia** — and the terrain surface carries no second channel, so hue is the only channel. | [embedded.h:105-121](../../../desktop/src/scene3d/shaders/embedded.h#L105), [projection.cpp:633-649](../../../desktop/src/space/projection.cpp#L633), [hud.cpp:250-269](../../../desktop/src/scene3d/hud.cpp#L250), gate scope at [test_cvd.cpp:44-46](../../../desktop/test/test_cvd.cpp#L44) | [64](64-scene-palettes-enter-the-gate.md) T1–T3 |
| **2** | One quantity, two transforms. Terrain height is `log1p` then normalised and **says so**; the canvas heat bar over the same recording is linear-to-max and says nothing. Execution heat is heavy-tailed, so linear-to-max renders every non-peak row as a sub-pixel stub. | [terrain.cpp:70](../../../desktop/src/space/terrain.cpp#L70),[:518](../../../desktop/src/space/terrain.cpp#L518); [hud.cpp:228](../../../desktop/src/scene3d/hud.cpp#L228); [theme.h:170](../../../desktop/src/ui/theme.h#L170) | [63](63-magnitude-transform-and-denominator.md) T2 |
| **3** | `dt_cell_magnitude_bar` has **four denominators and one appearance**: column-max ([canvas heat](../../../desktop/src/views/canvas_draw.cpp#L198), [hotedge samples](../../../desktop/src/views/observer_draw.cpp#L562)) and absolute-whole ([completeness](../../../desktop/src/views/completeness.cpp#L121), [cpu%](../../../desktop/src/ui/details_pane.cpp#L218), [inspect](../../../desktop/src/ui/inspect_door.cpp#L1735)). A full bar means "hottest row here" in one table and "complete" in another. | the five call sites above | [63](63-magnitude-transform-and-denominator.md) T1 |
| **4** | The whole-trace overview strip **never buckets** despite the type name — one `OverviewBucket` per timeline row — and is drawn at `PlotBars(..., 0.9)`, a width of 0.9 *step units*. At 100k steps in a 1000px strip that is 0.009px per bar: aliased to invisibility, and 100k bars submitted per frame for a 48px widget. | [overview.cpp:20-25](../../../desktop/src/views/overview.cpp#L20), [timeline_draw.cpp:47](../../../desktop/src/views/timeline_draw.cpp#L47) | [65](65-whole-trace-strips-at-scale.md) T1–T2 |
| **5** | The hot-edge heatmap sets `ImPlotAxisFlags_NoDecorations` on **both** axes, so a bright cell cannot be mapped back to a function — the table below is rank-ordered by edge, not by matrix row. Colour is linear over `[0, maxc]`, so one dominant edge pushes the rest into the bottom Viridis band. | [observer_draw.cpp:460-465](../../../desktop/src/views/observer_draw.cpp#L460); labels already returned by [`obs_hotedges_matrix`](../../../desktop/src/views/hotedges.cpp#L319) | [65](65-whole-trace-strips-at-scale.md) T3–T4 |
| **6** | Two naked-integer columns finding #1 never reached: timeline `in/out` (`"%zu/%zu"`) — the same quantity `overview_from_timeline` already uses as its strip weight — and hotedges `mispred / ret`, where the interesting quantity is a **rate** the reader must divide by eye. asmspy already computes that percentage. | [timeline_draw.cpp:188](../../../desktop/src/views/timeline_draw.cpp#L188), [observer_draw.cpp:568](../../../desktop/src/views/observer_draw.cpp#L568); asmspy's tag at [asmspy.c:1503](../../../cli/asmspy.c#L1503) | [63](63-magnitude-transform-and-denominator.md) T3–T4 |
| **7** | asmspy's TUI has **no colour**: no `start_color`, `init_pair`, `COLOR_PAIR` or `use_default_colors` anywhere in [cli/](../../../cli/). Every distinction rides on three attributes, and `A_BOLD` is already spent on headings. **This is a live bug**, not only a limit: the dataflow slice assigns them exclusively, so selecting a row *erases its slice membership* — at exactly the moment the reader needs it. | measured absence across `cli/*.c`, `cli/*.h`; the exclusive assignment at [asmspy.c:7401-7407](../../../cli/asmspy.c#L7401) | [66](66-asmspy-tui-channels.md) T1–T2 |
| **8** | Every ranked asmspy view formats counts as bare digits, and the numerics are **left-aligned** (`%-7llu`, `%-8llu`), discarding even the free digit-column-width cue a monospace grid gives. | [asmspy.c:1519](../../../cli/asmspy.c#L1519), [:551](../../../cli/asmspy.c#L551), [:590](../../../cli/asmspy.c#L590), [:803](../../../cli/asmspy.c#L803) | [66](66-asmspy-tui-channels.md) T3 |
| **9** | Symbol names truncate from the **wrong end** (`%-30.30s` keeps the first 30 chars). For mangled C++, JIT method names and `func+0xNN [module]` forms the distinguishing part is the tail, so two hot edges into different methods of one class render as one string. | [asmspy.c:1519](../../../cli/asmspy.c#L1519), [:551](../../../cli/asmspy.c#L551), [:590](../../../cli/asmspy.c#L590), [:1652](../../../cli/asmspy.c#L1652) | [66](66-asmspy-tui-channels.md) T4 |
| **10** | Live sampling shows a window, never a trend: each survey window replaces the last on both surfaces, so a stable rank and a thrashing one are indistinguishable — and neither side retains enough state to tell them apart. | [asmspy.c:6820-6880](../../../cli/asmspy.c#L6820); `HotEdgeView` keeps only `snapshots`, [hotedges.h:66](../../../desktop/src/views/hotedges.h#L66) | **not cut** — see §5 |

## 4. Sequencing

The four briefs are **independent of each other** and may be claimed in parallel by
four agents. Within the set:

- **[63](63-magnitude-transform-and-denominator.md) first if anything is.** Its T1
  splits an API that its own T3/T4 — and any re-cut two-up diverging heat bar (§2) —
  write against; landing it first means one migration instead of three.
- **[64](64-scene-palettes-enter-the-gate.md) is the only accessibility defect**
  here — the others are legibility. If only one brief lands, land this one.
- **[65](65-whole-trace-strips-at-scale.md)** and
  **[66](66-asmspy-tui-channels.md)** touch no shared file with 63/64 or with each
  other. 66 touches only [cli/](../../../cli/); 65 touches only
  `views/overview.*`, `views/timeline_draw.cpp` and `views/observer_draw.cpp`.

Nothing here changes the wire schema, the engine, or any `.asmtrace` golden. Every
task is model + draw half, and every task is asserted headlessly.

## 5. Deliberately not cut: finding #10 (sampled-rank stability)

Finding #10 is the largest *information* gain available on the statistical surface —
everything else in this roadmap re-encodes data already on screen; #10 adds data.
It is nonetheless **not cut**, for one reason: it is the only finding that needs
**new retained model state on both surfaces** (a bounded per-edge ring of recent
window counts in `sample_snap` and in `HotEdgeView`), and that state has a fidelity
question this review did not resolve —

> when a survey window drops samples ([`lost`](../../../desktop/src/views/hotedges.h#L62))
> or is throttled, is that window a *low* point in the trend, or a *gap* in it?

Rendering it as a low point fabricates a decline the sampler never measured; the
`WatchPlot` precedent says it must be a **gap**
([observer_draw.cpp:340-341](../../../desktop/src/views/observer_draw.cpp#L340): "an
un-read-back value is a GAP, never a fabricated 0"). Confirm that reading against a
real throttled capture, then cut #10 as brief **67** with the gap rule as its first
fidelity rule. It is roughly two tasks (an asmspy sparkline column reusing
[66](66-asmspy-tui-channels.md) T3's bar helper, and a desktop delta/band) once
that question is settled.
