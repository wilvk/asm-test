# One magnitude channel, one stated denominator — implementation

> **Source.** Findings **#2, #3, #6** of the 2026-08-07 GUI/asmspy visualisation
> review, cut per [62-encoding-integrity-roadmap.md](62-encoding-integrity-roadmap.md) §3.
>
> This brief **consumes** the idiom the 2026-07-29 review's finding #1 landed
> (`dt_magnitude_frac` / `dt_cell_magnitude_bar`,
> [theme.h:170,185](../../../desktop/src/ui/theme.h#L170)) and must not re-invent
> it. T1 splits that API, so it **precedes** any other work writing against it —
> see [62](62-encoding-integrity-roadmap.md) §2.
>
> Read [\_conventions.md](../implementations/_conventions.md) first; D1–D11 live in
> this directory's [README](README.md). **Prerequisites: none.** Pure model + draw
> half throughout — no engine, wire or schema change.
>
> Authored 2026-08-07 against `35ef821f`, every citation verified; re-checked at
> `3e2e8cea`, which touches no file cited here. If a cited `file:line` disagrees
> with the code, the code wins — re-verify, then fix this doc in the same change.
>
> **Status — ☐ 0/4.**

## Why this work exists

Doc [24](../archive/gui/24-one-visual-language.md) F14 established *one meaning per
colour*, and [theme.h](../../../desktop/src/ui/theme.h) enforces it by giving each
meaning its own named accessor with its own doc comment. The **magnitude** channel
landed later, as a single function, and never got the same treatment. It now carries
two unrelated meanings through one appearance:

| Call site | `frac` is… | Meaning of a full bar |
|---|---|---|
| [canvas_draw.cpp:198,202](../../../desktop/src/views/canvas_draw.cpp#L198) | `heat / heat_max` | "hottest row **in this table**" |
| [observer_draw.cpp:562](../../../desktop/src/views/observer_draw.cpp#L562) | `count / count_max` | "hottest row **in this table**" |
| [completeness.cpp:121](../../../desktop/src/views/completeness.cpp#L121) | `trace_insns / insns_truth` | "**complete** — every instruction captured" |
| [details_pane.cpp:218](../../../desktop/src/ui/details_pane.cpp#L218) | `cpu_pct / 100.0` | "**saturated** — one core fully busy" |
| [inspect_door.cpp:1735](../../../desktop/src/ui/inspect_door.cpp#L1735) | `pct / 100.0` | "**saturated**" |

The first two are a *rank within the visible set*; the last three are a *share of a
known whole*. They render as the same low-alpha fill with no baseline, no track and
no stated maximum. A reader who learns the bar in the completeness table reads the
canvas wrongly, and vice versa. That is precisely the drift F14 exists to prevent —
applied to colour, but not yet to magnitude.

**And the transform disagrees with the 3D pane over the same quantity.** Terrain
height is `log1p(count)`, normalised at upload, and the HUD **states the transform**
([terrain.cpp:70](../../../desktop/src/space/terrain.cpp#L70),
[:518](../../../desktop/src/space/terrain.cpp#L518);
[hud.cpp:228](../../../desktop/src/scene3d/hud.cpp#L228) — *"height: log(1 + access
count) — brightest band this slice = N"*). The canvas heat bar over the *same
recording* is linear-to-max and states nothing. Execution heat is heavy-tailed: one
hot loop pins `heat_max` and every other row renders as a sub-pixel stub, which is
the exact claim ("nothing else matters") the log height was chosen to avoid.

## What already exists (verified 2026-08-07)

- **`dt_magnitude_frac(value, max)`** clamps to `[0,1]` and returns `0` when
  `max <= 0` **or** `value <= 0` ([theme.h:170-177](../../../desktop/src/ui/theme.h#L170)).
  The `value <= 0` branch is load-bearing: it is what makes "an absence is never
  barred" true. **Both new variants must keep it.**
- **`dt_cell_magnitude_bar(frac, col)`** draws a low-alpha fill of `frac ×
  GetContentRegionAvail().x`, one text line tall, at the cursor **without advancing
  it**, so the caller's `Text` lands on top ([theme.h:185-201](../../../desktop/src/ui/theme.h#L185)).
  It replaces the source colour's alpha with `0.24` and introduces no hue.
- **The null-backend / 2.0×-text-scale fallback is the number itself** — the bar is a
  *second* channel and never a replacement. Every task here keeps the exact number
  as the cell label.
- **`obs_hotedges_build` already carries `mispred` per edge**
  ([hotedges.h:50](../../../desktop/src/views/hotedges.h#L50)) and asmspy already
  computes the percentage form of it (`(mispred * 100) / count`,
  [asmspy.c:1503](../../../cli/asmspy.c#L1503)). T4 needs no new model field.
- **`overview_from_timeline` already uses `n_in + n_out` as its strip weight**
  ([overview.cpp:21](../../../desktop/src/views/overview.cpp#L21)) — the quantity T3
  bars is the one the minimap already ranks by.
- **`dt_hot_u32()` / `dt_dim_u32()`** ([theme.h:98,157](../../../desktop/src/ui/theme.h#L98))
  are the only two colours any bar in this brief uses. **No new hue anywhere.**

## Fidelity rules (binding on every task)

1. **Never bar an absence.** `dt_magnitude_frac`'s `value <= 0 -> 0` branch survives
   into both variants. A `—` cell, a `(unknown)` cell and a dropped step draw no bar.
2. **The number is the truth; the bar is the gloss.** Every task keeps the exact
   integer as the cell label. Nothing becomes bar-only.
3. **A transform is stated or it is not applied.** T2's log bars carry the transform
   in the **column header**, in the same voice `height_scale_note` uses. An unstated
   log bar is worse than a linear one.
4. **A rate is not a count.** T4 bars the misprediction *rate* (a share) and keeps
   `mispred` and `is_return` as exact integers. It never bars `mispred` against
   `count_max`, which would mix the two denominators this brief exists to separate.
5. **Statistical stays statistical.** The hotedges table is `exact:false` by
   construction ([hotedges.h:20-22](../../../desktop/src/views/hotedges.h#L20)); its
   `STATISTICAL` label and chrome line are untouched by T4, and gain no bar.
6. **No golden churn without intent.** Every change here is draw-side. If a golden
   model dump moves, something was computed in the wrong half — stop and move it back.

## Tasks

### T1 — Split the bar into rank and share, and give share a track (S)

The API split is the whole point: after it, "which denominator is this?" is answered
by the function name at every call site instead of by reading upward for a local
`*_max`.

1. In [theme.h](../../../desktop/src/ui/theme.h), replace the single
   `dt_cell_magnitude_bar` with two named entry points, keeping the existing one as a
   thin deprecated alias for `dt_cell_rank_bar` **only** if that eases the migration
   in one commit — otherwise delete it and migrate all five sites here:
   - `dt_cell_rank_bar(float frac, ImU32 col)` — byte-identical to today's
     behaviour. `frac` is a share of the **largest value in the visible set**. No
     track: there is no meaningful "remainder" when the denominator is just the
     biggest peer.
   - `dt_cell_share_bar(float frac, ImU32 col)` — same fill, **plus** a 1px
     full-width track outline (`AddRect`, same hue, alpha ~`0.10`) so the *unfilled
     remainder is visible*. "40% complete" must look different from "40% of the
     hottest row", and the track is what makes it so without a new colour.
2. Migrate the five call sites by meaning, not by mechanical rename:
   `canvas_draw.cpp:198,202` and `observer_draw.cpp:562` → **rank**;
   `completeness.cpp:121`, `details_pane.cpp:218`, `inspect_door.cpp:1735` → **share**.
3. Add both keys to the encoding table
   ([legend.cpp:37-52](../../../desktop/src/ui/legend.cpp#L37)) as `categorical:
   false` signal rows with distinct channel tokens (`"of max"` / `"of total"`), so
   `dt_semantic_legend` states the distinction the pixels now carry.

**Done when** `test_cvd`'s table walk sees both rows, and a `desktop-ui-test` case
asserts a share bar at `frac = 0.4` draws a track wider than its fill while a rank
bar at the same fraction does not.

### T2 — Log-scale the count-like bars, and say so in the header (S)

1. Add `dt_magnitude_frac_log(double value, double max)` beside the linear one:
   `log1p(value) / log1p(max)`, same clamping and same `value <= 0 -> 0` and
   `max <= 0 -> 0` guards. **Mirror `terrain.cpp`'s transform exactly** — `log1p`,
   not `log`, not `log2` — so the flat and 3D readings of one recording agree by
   construction rather than by two authors choosing the same curve.
2. Apply it to the two **count** rank bars: canvas `heat` / `heat B`
   ([canvas_draw.cpp:198,202](../../../desktop/src/views/canvas_draw.cpp#L198)) and
   hotedge `samples` ([observer_draw.cpp:562](../../../desktop/src/views/observer_draw.cpp#L562)).
3. **Do not** apply it to the share bars. `cpu_pct` and `trace/truth` are already
   fractions of a bounded whole; a log fraction there would distort a quantity that
   was never heavy-tailed.
4. State it in the column header — `heat (log)`, `samples (log)` — and add the peak
   to the existing header/chrome line in `height_scale_note`'s voice: *"bar: log(1 +
   count), fullest bar = N"*. The peak is already computed draw-side as
   `heat_max` / `count_max`; this only prints it.

**Done when** `test_canvas`'s golden model dump is **byte-identical** (the transform
is draw-side only) and a `desktop-ui-test` case over a `{1, 2, 4000}` heat column
asserts the middle row's bar is visibly non-zero — the case that fails today.

### T3 — Bar the timeline's `in/out` operand density (S)

1. In [timeline_draw.cpp](../../../desktop/src/views/timeline_draw.cpp), compute
   `inout_max = max(r.n_in + r.n_out)` draw-side over `t.rows`, exactly as
   `canvas_draw` computes `heat_max`.
2. Draw `dt_cell_rank_bar(dt_magnitude_frac_log(r.n_in + r.n_out, inout_max),
   dt_dim_u32())` behind the `in/out` cell
   ([timeline_draw.cpp:187-188](../../../desktop/src/views/timeline_draw.cpp#L187)),
   keeping `"%zu/%zu"` as the label. `dt_dim_u32` (neutral ranking), **not**
   `dt_hot_u32` — operand density is not execution heat, and the two must not read
   as one quantity.
3. A `r.missing` row draws **no bar**: its `df_step` was dropped, so its density is
   unknown, not zero. This is the same rule the `(unknown)` offset cell already
   follows ([timeline_draw.cpp:172-173](../../../desktop/src/views/timeline_draw.cpp#L172))
   — and note `overview_from_timeline` already encodes it
   ([overview.cpp:21](../../../desktop/src/views/overview.cpp#L21)).

**Done when** `test_timeline`'s golden is byte-identical and a `desktop-ui-test`
case with one `missing` row asserts that row draws no bar while its neighbours do.

### T4 — Show the misprediction *rate*, not two integers to divide (S)

1. In `draw_obs_hotedges`
   ([observer_draw.cpp:567-569](../../../desktop/src/views/observer_draw.cpp#L567)),
   keep the exact `mispred / is_return` integers and add the rate as a **share** bar:
   `dt_cell_share_bar(dt_magnitude_frac(e.mispred, e.count), dt_hot_u32())`.
   `e.count == 0` yields `0` through the existing guard and draws nothing.
2. Extend the label to carry the rate as a number too — `"%llu / %llu (%.0f%%)"` —
   because the bar is a second channel, never the only one. Match asmspy's existing
   integer-percent form ([asmspy.c:1503](../../../cli/asmspy.c#L1503)) so the two
   surfaces report one number.
3. **Make the rate sortable.** The sort switch keys column 4 on `e.mispred`
   ([observer_draw.cpp:533-535](../../../desktop/src/views/observer_draw.cpp#L533));
   change that key to the rate `e.count ? double(e.mispred)/e.count : 0.0`. Sorting a
   column by a quantity it no longer leads with is its own drift.
4. Rename the header `mispred / ret` → `mispred / ret (rate)` so the sort key is
   discoverable.

**Done when** `test_obs_hotedges` asserts the rate ordering differs from the raw
`mispred` ordering on a fixture with a low-count high-rate edge — the case where the
two orderings disagree is exactly the one the reader currently cannot see.

## Testing

All headless. `make desktop-test` covers `test_canvas`, `test_timeline`,
`test_obs_hotedges`, `test_completeness_view`, `test_cvd`; `make desktop-ui-test`
covers T1's track, T2's log floor, T3's absent bar. Run both through
`make docker-desktop` per [CLAUDE.md](../../../CLAUDE.md)'s tooling rule.

**No golden dump should move.** Every task is draw-side; T2 and T3 explicitly assert
byte-identical model output. If `desktop/test/golden/` or `desktop/test/expected/`
churns, a transform leaked into a builder — revert it there rather than regenerating.
No `.asmtrace` golden and no `tests/golden-asmtrace/` file is touched.

## Out of scope

- **The asmspy bar column.** It shares T2's log decision but not a line of code (C,
  not C++, and a character cell rather than a draw list). It is
  [66](66-asmspy-tui-channels.md) T3, which cites this task for the transform.
- **The two-up diverging A/B heat bar.** It is a separate change against the same
  API; this brief only splits the API underneath it, so it should be written against
  the split rather than migrated after it ([62](62-encoding-integrity-roadmap.md) §2).
- **`rss KB`** ([details_pane.cpp:224](../../../desktop/src/ui/details_pane.cpp#L224))
  stays a bare integer: there is no denominator to bar it against, and inventing one
  (total RAM) would be a fact the recording does not hold.
