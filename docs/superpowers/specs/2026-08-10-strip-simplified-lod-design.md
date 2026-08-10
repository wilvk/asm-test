# The session strip at Firefox scale — simplified default + plan cache + clear-previous (design)

**Date:** 2026-08-10.
**Status:** approved design, ready for an implementation plan.
**Scope:** desktop/ only: the session strip's PLAN and PANEL halves
(`desktop/src/views/strip.{h,cpp}`, `strip_draw.cpp`), plus one session-level
affordance (`live/session.{h,cpp}`, the Live-capture pane, one
`shell_sync_live_tab` clamp). No model change, no wire change, no schema
change, no 3D change.

## Why this work exists

Tracing a real browser produces hundreds of threads, dozens of processes,
and a JIT-churned region set. The strip's model handles that (it is vectors);
its PLAN does not: the thread deck emits up to one density prim per lane per
pixel column, so 200 lanes × 1200 columns approaches ~240k prims — submitted
per frame, every frame, because `strip_plan` runs unconditionally in
`draw_strip` ([strip_draw.cpp](../../../desktop/src/views/strip_draw.cpp)).
The reader, meanwhile, cannot use 200 eighteen-pixel lanes at once — the
pertinent high-level facts (which processes are hot, where the kernel
crossings burst, which regions memory hammers, where runs begin) drown in
rows.

Two fixes, one honest rule: **the model stays complete — the CAMERA decides
what is drawn.** Simplification is a reading posture, exactly like
`follow_tail`; it never touches the truth, and everything it hides it
COUNTS, on screen, with a one-click way to see all of it.

## Approaches considered

**A — Plan-level LOD + plan cache (chosen).** A `detail` flag on
`strip_view_t`; when off (the default) the planner draws the top-N most
active lanes and bands plus explicit aggregate rows for the remainder, and
the panel caches the prim vector so a static frame re-plans nothing. Model,
tests over it, and drill-ins are untouched; small recordings render
byte-identically (thresholds).

**B — Model-level decimation (rejected).** Dropping/sampling events at build
time. Silent truncation reads as coverage — the exact dishonesty the HUD
counters exist to prevent — and it would break seq-anchored drill-ins.

**C — Custom GL batching for the painter (rejected).** The prim COUNT is the
problem; capping it at the plan fixes the cause. A GL path would also break
the null-backend rule that lets every strip test run headless.

## The simplified view (default)

One new camera field: `strip_view_t::detail` (default `false`). Selection is
PURE and shared (the layout and the plan must agree on row counts):

- `strip_selected_lanes(m, detail)` → indices of the lanes to draw, plus an
  aggregate flag. Detail: every lane. Simplified: the
  `kStripSimplifiedLanes = 8` most active lanes — ranked by that lane's
  activity-event count plus its syscall ticks, ties broken by ascending tid —
  DISPLAYED in their existing model order (the tgid grouping survives among
  the kept lanes; group separator rows draw only in detail mode).
- `strip_selected_bands(m, detail)` → same shape over bands;
  `kStripSimplifiedBands = 6`, ranked by placed mem+pc marks, ties by
  ascending base, displayed in base order.
- **At or below the thresholds the simplified plan is byte-identical to the
  detailed plan** — small recordings see zero change, and a test pins that
  with `strip_plan_dump` equality.

What the hidden remainder becomes — counted, never vanished:

- **The aggregate lane**: one extra deck row, label
  `"(+N lanes, M events)"`, whose per-column density is the SUM of the
  hidden lanes' buckets. Prim identity: the existing `lane_header` /
  `lane_density` kinds with `a = kStripAggRow` (0xFFFFFFFF); hover shows the
  label; clicking nothing (an aggregate is not a lane).
- **The elsewhere band row**: one extra band-area row, label
  `"(+N regions — M access(es), counts only)"`, carrying a per-column COUNT
  ribbon (again `lane_density` with `a = kStripAggRow` — the painter reads
  only `b`). No address→y mapping is ever faked for a hidden band: counts
  only, and the label says so.
- **Hidden syscalls**: none — the rail never hides; it already aggregates
  per column (`kStripRailTicksPerCol` + `"+N"` overflow).
- **The HUD states the posture, pinned verbatim**:
  `"simplified — top 8 of N lanes, top 6 of R regions"` appended to the HUD
  line when anything was actually aggregated (absent when nothing was), so a
  screenshot can never pass off a simplified strip as the whole. Asserted by
  test like the axis label.

**The toggle**: a `"detail"` / `"simplify"` button in the panel header next
to `"follow"`, flipping `st.cam.detail`. Camera state — it survives growth
rebuilds like zoom and follow, and it is per-view, not a Settings field.

## Graphics performance

1. **Plan cache.** `strip_plan_key(v, model_gen, w, h)` — a pure FNV-1a hash
   over the camera's bit patterns (`seq0`, `seq_per_px`, `lane0`, `lane_h`,
   `detail`) + the model generation + pixel size. `StripState` gains
   `plan_cache` / `plan_key` / `model_gen` (incremented on every rebuild);
   `draw_strip` re-plans ONLY when the key changes. A static frame — no
   growth, no interaction — walks the cached vector and plans nothing. The
   key function is unit-tested for sensitivity to every input.
2. **Simplified default caps the prim budget.** Deck rows become ≤ 9 and
   band rows ≤ 7 regardless of target size, so the worst-case plan is
   O(px_w × 16) core prims (~30k at 1920px, vs ~240k+ unbounded) — before
   RLE shrinks it further.
3. **Density run-length merging.** Adjacent columns whose quantized
   intensity `b` is EQUAL merge into one `lane_density` prim spanning the
   run; adjacent `mem_envelope` columns with identical band, rw and pixel
   rects merge the same way. Pure plan change, visible in the dump,
   deterministic; hot loops and idle stretches collapse to a handful of
   prims.
4. **The rail cap already exists** (3 ticks + overflow per column) and is
   unchanged.

## Clearing previous sessions

A long Firefox session accumulates completed captures: `LiveSession::done_`
grows on every Start/Stop, and the union weave (strip, 3D) deliberately
accumulates across them. There is no way to drop that history short of
Disconnect (`LiveSession::reset()`, which requires the host down). Add one:

- **`LiveSession::clear_completed()`** — drops the COMPLETED recordings and
  their accompanying notes, keeping the host, the pipes, the status and any
  still-growing capture. Returns `false` and does nothing while a capture is
  growing: "previous" means finished, and refusing while open keeps the
  lifecycle notes trivially attributable (the current capture's notes are
  never guessed apart from history's). `reset()` remains the Disconnect-only
  full teardown.
- **The affordance** lives in the Live-capture pane, beside the existing
  `"N completed recording(s) this session"` line — the exact place the
  accumulation is stated. Two-step arm (the `obs_syscall_reveal_all`
  precedent): first click arms with the consequence named —
  `"really clear N capture(s)? they are not saved"` — second click performs;
  any frame where the button is not clicked disarms. Greyed with a stated
  reason while a capture is growing (`"stop the capture first"`) or when
  there is nothing to clear.
- **Downstream is automatic**: the growth watermark already keys on
  `recordings().size()`, so the next `shell_sync_live_tab` tick rebuilds the
  union, streams, seams and invalidates the strip/3D exactly as a growth
  does. One clamp is added there: `live_dismissed_done` (the adopted-tab
  dedup watermark) is reset to 0 whenever it exceeds the now-smaller done
  count — self-healing regardless of who cleared.

## What this deliberately does not do

- No model change: `strip_build` output is byte-identical before/after.
- No new Settings field, no persistence of `detail`.
- No 3D-scene changes: the 3D pane has its own LOD machinery (doc 61 axis
  budget); conflating the two surfaces' budgets would help neither.
- No producer-side filtering: what is recorded is a capture-scope question
  (doc 68), not a view question.

## Testing

All in the existing suites, same idioms:

- `test_strip_model.cpp`: selection rules (rank, ties, order preservation,
  threshold no-op), aggregate rows (labels, summed densities, `kStripAggRow`
  identity, hover text), the pinned simplified HUD phrase, RLE (equal
  adjacent columns → one prim; a boundary breaks the run), envelope merge,
  detailed-vs-simplified prim-count inequality on a big synthetic model,
  byte-identical dumps at/below thresholds, `strip_plan_key` sensitivity to
  each field and stability across identical inputs.
- `test_strip_draw.cpp`: the panel over a 20-lane synthetic model draws in
  both postures (geometry oracle both ways); the toggle button exists on the
  header row.
- `test_live_session.cpp`: `clear_completed()` refused while growing (state
  untouched); after close it empties `recordings()` + `notes()` while
  keeping status and the malformed counter; a later capture still records.
- `test_shell.cpp`: after two fed captures + `clear_completed()`, the next
  sync drops/rebuilds the live models and clamps `live_dismissed_done`.
