# Desktop GUI — UX & data-visualization review

Review date: 2026-07-29. Scope: the [`desktop/`](../../../desktop/) Dear ImGui
(1.91.9) + ImPlot viewer over `.asmtrace` recordings — the replay views
([canvas](../../../desktop/src/views/canvas_draw.cpp) /
[timeline](../../../desktop/src/views/timeline_draw.cpp) /
[scrubber](../../../desktop/src/views/scrubber_draw.cpp)), the
[Loom fabric](../../../desktop/src/loom/fabric_imgui.cpp), the
[3D scene](../../../desktop/src/scene3d/scene.cpp), the
[observer/live deck](../../../desktop/src/views/observer_draw.cpp), the analysis
views ([slice](../../../desktop/src/views/slice_view_draw.cpp) /
[diff](../../../desktop/src/views/diff_view_draw.cpp) /
[ABI x-ray](../../../desktop/src/views/abixray_draw.cpp)), the
[shell/nav](../../../desktop/src/ui/shell.cpp), and the live
[capture flow](../../../desktop/src/live/session.cpp). The review is held to the
app's own tested design language — the semantic palette
([ui/theme.h](../../../desktop/src/ui/theme.h)) and the graded honesty vocabulary
([ui/honesty.h](../../../desktop/src/ui/honesty.h)).

**Method.** Eight reviewers each read one cluster of the *real draw code* and
produced recommendations grounded in a file:line; an adversarial pass re-opened
every cited file to reject hallucinations, already-shipped ideas, and anything
that broke a tested constraint (never fabricate structure the recording lacks;
one semantic colour language; both light and dark themes legible; the
render-only viewer stays engine-free; survives 2.0× text scale). A final pass
deduped and ranked the survivors. **65 findings survived, 0 rejected** — the
finders cited real lines, not guesses. Six of the top claims were then
independently spot-checked against the tree and all held (see §2).

A browsable version with triage filters (by impact / kind / quick-win) was
published as an artifact: <https://claude.ai/code/artifact/77943c4c-0e96-4228-b7d2-2efe910e7ca0>. This document is the durable record.

> Line numbers reflect the tree at review time (HEAD `7c1a9ae`). Re-verify
> before cutting a change. Findings are code-derivable; the value here is the
> synthesis and ranking, not private knowledge.

**Shape of the 65:** 20 high / 38 medium / 7 low impact · 37 small / 26 medium /
2 large effort · 24 dataviz / 27 ux / 14 both.

---

## 1. Executive summary

This is a mature, honesty-disciplined GUI: nearly every finding is a refinement inside an already-correct pure-model/draw-half split, not a broken invariant. Two patterns dominate the wins. First, the app repeatedly renders magnitude as a bare integer in sortable tables (canvas heat, hot-edge samples, topology cards, %CPU, completeness N/M, A/B heat deltas) — one shared in-cell-bar treatment, with the number kept as the second channel, is the single highest-leverage change and lands mostly draw-side. Second, the honesty grading in honesty.h is applied unevenly: ordinary data-shape absences and capture-property notes are painted in the same loud caution amber reserved for real truncation, diluting the signal, and one live path (a failed ssh host) actually renders a hidden failure as "Session ended cleanly" — a genuine D7 violation. Beyond those, the biggest structural gaps are wired-but-undiscoverable affordances (the palette/history in menus, the 3D camera controls, an interactive slice DAG, an unreachable Loom lane deck) and missing spatial anchoring in the Loom fabric and 3D scene (no playhead marker, no region hue on terrain, no pick preview). The flagship dead interaction is the Loom fork/take + Reweave overlay, fully built and tested but never consumed in-app.

---

## 2. Two findings to treat as bugs, not polish

Both were independently verified against the tree during review.

- **Loom lanes beyond the first screenful are unreachable** (finding #3).
  `L.cam.lane0` is *read* (`desktop/src/loom/fabric_imgui.cpp:321`, and
  `fabric_plan.cpp` renders only lanes `[lane0, lane0+visible)`) but is
  **assigned nowhere** — permanently 0, with no wheel, scrollbar, or drag. Any
  recording with more lanes than fit has its remaining worldlines unseeable,
  unhoverable and unselectable. Fix: write `lane0` from the mouse wheel through a
  unit-tested `loom_view_clamp_lane0`, and disclose a clamped viewport rather
  than hide it.
- **A failed ssh/host connection is reported as success** (finding #7).
  `reap()` maps only exit 127 to `Failed` (`desktop/src/live/session.cpp:270-273`),
  so an ssh exit 255 falls through to `Ended`; `end_cause()` then returns
  `StoppedClean`, whose placard reads *"Session ended cleanly"*
  (`desktop/src/live/end_state.h:102-104`). A hidden failure rendered as success
  is a genuine D7 violation on a supported path. Fix: a new
  `EndCause(HostFailedNoData)` at the Integrity tier, taken when the host exited
  non-zero with no recording and no tear, *before* the `StoppedClean` return.

---

## 3. How to read the findings

Each finding carries three tags and cites the file(s) it was verified against.

- **Impact** — `high` moves the daily experience · `medium` a clear improvement
  · `low` polish.
- **Kind** — `dataviz` (encoding / legibility) · `ux` (interaction /
  discoverability) · `both`.
- **Effort** — `small` mostly draw-half · `medium` model + draw · `large` a new
  surface or plumbing.
- **★** marks a quick win: high impact, small effort.

### Start here — nine quick wins

- **#1** In-cell magnitude bars for every naked-integer column
- **#2** Surface find-hits on the whole-trace minimap and unify find-hit color through theme.h
- **#3** Make the Loom vertical lane deck scrollable — off-screen lanes are unreachable
- **#4** Preserve the invocation pager across live rebuilds (stop the yank to #0)
- **#5** Auto-scroll the live syscall/watch/tree streams to the newest row
- **#6** Expose palette / find / go-to / history in the menu bar and bindings table
- **#7** Stop reporting a failed ssh/host connection as 'Session ended cleanly'
- **#8** Draw the def-use edge's carried location on the slice cone
- **#9** Surface the ABI x-ray's differing-register set, and withdraw it when panes aren't step-aligned

### Six strategic bets

- **#20** Wire the fork/take verdict overlay and Reweave gesture into the app
- **#13** Make the slice DAG interactive: click a node to re-root, [/] to walk the cone
- **#15** Paint region-kind hue on the terrain plane
- **#14** Show hover feedback and a pick preview on the 3D viewport
- **#17** Show live capture rate and per-kind composition, not just a rising integer
- **#58** Provide an honest GL-free 2D terrain fallback and a flat reading surface

---

## 4. Findings by theme

### 4.1 Honesty grading is applied unevenly

*honesty.h defines a three-tier language (Neutral chip / Caution amber collapsible / Integrity red non-collapsible), but many sites paint every ordinary capture-property in one loud caution amber, diluting the real refusals two lines away — and worse, one supported path renders a hidden failure as success. The fix is to route each note through its true tier and make one buried failure loud.*

#### #7 Stop reporting a failed ssh/host connection as 'Session ended cleanly' ★

`ux` · **high** impact · small effort — Session-end placard (host spawn diagnostics, ssh path)

- **Today.** reap() maps only status 127 to Failed (session.cpp:270-273), so an ssh exit 255 falls to Ended; only child fds 0/1 are dup'd so ssh's stderr never reaches our pipe (malformed stays 0); end_cause() then falls through to StoppedClean whose placard reads 'Session ended cleanly' (end_state.h:102-104). A hidden failure rendered as success is a real D7 violation on a supported path.
- **Change.** Add an EndCause (HostFailedNoData) taken when host_exited && host_status!=0 && !any_recording && !torn, BEFORE the StoppedClean return — Integrity tier, message naming a probable connection/auth failure, fix pointing at 'check the ssh host and that asmspy is on the remote $PATH'. Pure and unit-testable via EndFacts; the draw half already renders title/message/fix.
- **Files.** `desktop/src/live/end_state.h` · `desktop/src/live/session.cpp` · `desktop/src/live/inspect.cpp`

#### #10 Grade observer provenance and capture-property notes by honesty tier, not flat amber

`both` · **high** impact · medium effort — Observer deck — shared chrome_line() and per-invocation notes

- **Today.** chrome_line() draws the whole provenance banner in one flat non-collapsible kWarn amber (observer_draw.cpp:157-163), so a torn recording and a merely-truncated one render at identical loudness; draw_obs_region likewise paints crawl_warning and jit_hint — normal capture properties honesty.h grades Neutral — in the same loud amber (610-613). This dilutes the real caution/integrity banners two lines up.
- **Change.** In obs_chrome()/region compute the dominant HonestyTier from the discrete facts and store it; in the draw half route torn/drop-on-exact through draw_honesty_banner(Integrity, red, non-collapsible), truncated through draw_honesty_banner(Caution) with a per-view collapsed bool in ObserverState (one chrome_line serves all 8 tabs), and backend/statistical/crawl/JIT provenance through draw_honesty_chip(Neutral). Keeps basis_error red. Same regrade philosophy at both sites.
- **Files.** `desktop/src/views/observer_draw.cpp` · `desktop/src/views/observer.cpp` · `desktop/src/ui/honesty.h` · `desktop/src/views/canvas_draw.cpp` · `desktop/src/views/region.cpp`

#### #21 Grade unavailable-view reasons neutral, not amber

`dataviz` · **medium** impact · small effort — Unavailable-views (N) affordance

- **Today.** draw_unavailable_views pushes dt_warn_col (caution amber) for every absent view's reason (shell.cpp:1363 and the docked copy 2613-2614), but these are ordinary data-shape absences (no df_step, no second recording, no regstate ring) that honesty.h explicitly grades NEUTRAL — exactly the pre-honesty amber dilution the header was written to kill.
- **Change.** Drop the PushStyleColor at both sites so the verbatim reason renders in neutral/TextDisabled style; keep amber only if a ViewPresence ever carries a genuinely Caution-tier reason. Cleanest is to add a HonestyTier to ViewPresence and pick color from it; minimal is the two-site color drop.
- **Files.** `desktop/src/ui/shell.cpp` · `desktop/src/ui/honesty.h` · `desktop/src/ui/view_presence.cpp`

#### #28 Let 'Reveal every payload' be undone — add Hide all

`ux` · **medium** impact · small effort — Syscalls tab — payload redaction UX

- **Today.** obs_syscall_reveal_all sets v.reveal_all=true and no code path ever clears it; per-row reveal has a hide toggle but the bulk reveal has no exit. A reader who revealed to check one call cannot re-blindfold for a screenshot or screen-share.
- **Change.** Add obs_syscall_hide_all(v) clearing reveal_all and zeroing revealed[]; show a 'Hide all payloads' button whenever reveal_all || any revealed[]. Display-only (payload never written back), symmetric with the existing per-row hide path, restoring the redaction placard.
- **Files.** `desktop/src/views/observer_draw.cpp` · `desktop/src/views/syscalls.cpp`

#### #29 Collapse N identical torn banners into one counted integrity banner

`dataviz` · **medium** impact · small effort — Live capture status (completed recordings)

- **Today.** inspect_door.cpp:383-391 draws a full-width non-collapsible Integrity banner with byte-identical text for EACH torn recording, so a continuous/auto session that tore several passes stacks identical red banners while clean recordings get only a scalar count.
- **Change.** Replace the per-torn loop with ONE Integrity banner carrying a count ('3 of 5 recordings this session are TORN — do not trust their tails') from a pure fold over recordings(), plus a compact strip one cell per completed recording sized/labelled by event_count() and colored torn=dt_bad vs clean=neutral. Stays loud, red, non-collapsible — strictly more information.
- **Files.** `desktop/src/ui/inspect_door.cpp`

#### #52 Distinguish 'spawned' from 'reachable' after Connect (the ssh latch)

`ux` · **medium** impact · medium effort — Connect pane (host liveness, ssh path)

- **Today.** inspect_connect sets host_started purely from session.start() succeeding, and start() sets Idle the instant the fork/exec returns — before any handshake — so for an ssh host still authenticating or doomed the UI shows full connection with no liveness indication.
- **Change.** Set a host_spoke/confirmed flag on the first successful feed_line and expose it on LiveStatus; in draw_connect_pane show 'connecting — no reply from the host yet' (dt_maybe, indeterminate) until it flips, stating that reachability is confirmed only once bytes arrive. Pairs with the failed-ssh placard so a doomed connection resolves to a failure. Testable via feed_line.
- **Files.** `desktop/src/ui/inspect_door.cpp` · `desktop/src/live/inspect.cpp` · `desktop/src/live/session.cpp`

---

### 4.2 Magnitude is a bare integer — give it a visual channel

*Across canvas, observer, completeness, processes and A/B diff the app prints exact counts as naked integers in sortable rankings, so the long-tail shape (is #1 10x #2 or a plateau?) is invisible. A row-height in-cell fill (dt_dim/dt_hot, number kept on top as the second channel and the null-backend fallback) is an honest, 2.0x-safe re-encoding that reuses existing accessors and adds no new color meaning.*

#### #1 In-cell magnitude bars for every naked-integer column ★

`dataviz` · **high** impact · small effort — Canvas heat, hot-edge samples, topology cards, %CPU, completeness N/M, A/B heat deltas

- **Today.** Magnitude is drawn as a bare integer in sortable rankings across the app: canvas heat as Text("%u") (canvas_draw.cpp:180), hot-edge samples as %llu (observer_draw.cpp:539), topology inv counts, the %CPU column (inspect_door.cpp:1148), completeness N/M (completeness_model.h:55-81) and A/B heat deltas (diff_view.cpp:109). The long-tail shape is invisible and severity never reads at a glance.
- **Change.** Draw a row-height horizontal fill proportional to value/max behind each cell (dt_hot_col for execution heat, dt_dim_col for neutral rankings) with the exact number kept on top as the label / null-backend / 2.0x fallback. Compute the per-table max in the model (canvas heat_max; completeness needs numeric trace_insns/insns_truth added to CompletenessRow; A/B needs numeric a/b on the heat row). Honesty carve-outs: never bar an absence ('—' draws nothing), never bar statistical edge rows in the diff (keep the [statistical] chip), and use no new hue.
- **Files.** `desktop/src/views/canvas_draw.cpp` · `desktop/src/views/canvas.cpp` · `desktop/src/views/canvas.h` · `desktop/src/ui/theme.h` · `desktop/src/views/observer_draw.cpp` · `desktop/src/views/topo.cpp` · `desktop/src/ui/inspect_door.cpp` · `desktop/src/views/completeness.cpp` · `desktop/src/views/completeness_model.h` · `desktop/src/views/diff_view_draw.cpp` · `desktop/src/views/diff_view.cpp` · `desktop/src/views/diff_view.h`

#### #22 Merge the redundant cov/blk columns in single-recording canvas mode

`both` · **medium** impact · small effort — Canvas coverage gutter

- **Today.** canvas.cpp:65 sets r.covered = r.block_start, and single mode prints cov as 'C' (canvas_draw.cpp:166) and blk as 'B' (:186) — the identical bit twice. canvas.h:33 documents they carry one predicate. The freed width is exactly what the heat bar needs.
- **Change.** In single-recording mode collapse cov+blk to one 'block' column and reclaim the width for the heat bar. Keep the two-side AB/A/B gutter untouched in two_up mode, where in_a/in_b carry real per-side coverage. Draw-half only.
- **Files.** `desktop/src/views/canvas_draw.cpp` · `desktop/src/views/canvas.cpp`

#### #35 Add a per-tier completeness rollup above the flat backend table

`dataviz` · **medium** impact · small effort — Completeness (tier x backend) table

- **Today.** completeness prints one global summary then a flat 7-column table whose first column is 'Tier' but with no per-tier subtotal or boundary, in producer order — so tier-level completeness is not scannable.
- **Change.** In build_completeness compute per-tier rollup counts (available/measured/complete/truncated) — pure aggregation, no row reorder — and render them as a compact rollup strip above the table, keeping truncation loud (amber + count) at the rollup level. Only add SeparatorText tier group headers if producer order is verified to keep each tier contiguous; otherwise lead with the order-independent rollup strip.
- **Files.** `desktop/src/views/completeness.cpp` · `desktop/src/views/completeness_model.h`

#### #42 Two-up canvas: A/B heat as a back-to-back diverging bar

`dataviz` · **medium** impact · medium effort — Canvas A/B diff heat

- **Today.** In two_up mode heat A and heat B are two independent integer columns (canvas_draw.cpp:180-184), forcing mental subtraction row by row even though the union-of-offsets model keeps B-only rows.
- **Change.** Compute a shared heat_max over A and B in dt_canvas_build2 and paint a centered back-to-back bar (A grows left, B grows right from a shared baseline), BOTH sides in dt_hot_col — side encodes A/B, so do NOT repurpose dt_selected_col for the B side (that would mint a new color meaning). Keep the two integers as exact/null-backend labels; the truncation banner still applies.
- **Files.** `desktop/src/views/canvas_draw.cpp` · `desktop/src/views/canvas.cpp` · `desktop/src/ui/theme.h`

---

### 4.3 One color language, and never rely on hue alone

*Text-scale is the only accessibility lever and the theme ships both light and dark, so any signal carried by color alone fails colorblind readers, 2.0x scale, or the light theme. Several signals ride hue only (changed register, torn prefix, watch write/read), one find-hit tint bypasses theme.h entirely with a hardcoded dark literal and no light branch, and the whole Loom fabric palette is dark-theme-only. Every one needs a themed accessor or a shape/text second channel.*

#### #2 Surface find-hits on the whole-trace minimap and unify find-hit color through theme.h ★

`dataviz` · **high** impact · small effort — Timeline overview / minimap strip + row highlighting

- **Today.** draw_timeline_overview receives t.find_hits and t.selected_step but uses them only in the table loop, never on the overview strip (timeline_draw.cpp:24,47,52), so the minimap omits the whole-trace hit distribution it exists to show. Separately the find-hit row tint is a hardcoded IM_COL32(120,100,0,90) dark-olive (timeline_draw.cpp:142) that bypasses theme.h and has no light branch — the one genuine break of the 'one color language, both themes' rule in the cluster.
- **Change.** Add a dt_find_hit_bg_u32() accessor in theme.h with dark AND light branches (keyed on dt_light_theme_active()), use it at timeline_draw.cpp:142, and overlay a marker per t.find_hits step plus a distinct dt_selected_col marker at t.selected_step on the overview density plot. In the null/text branch append '(N find hits)'. One accessor means find reads as one color on the minimap, the table, and everywhere else.
- **Files.** `desktop/src/views/timeline_draw.cpp` · `desktop/src/ui/theme.h`

#### #16 Make the Loom fabric palette theme-aware

`dataviz` · **high** impact · medium effort — Loom -> fabric canvas + minimap

- **Today.** kSpan/kSpanDim/kHollow/kHop/kKnot/kLabel etc. are hardcoded constexpr (fabric_imgui.cpp:28-36) and only kWarn/kHot switch on theme. In light theme the near-white lane-header kLabel and hop-ink kHop are drawn directly on the theme-following canvas ChildBg and wash out, and the minimap paints a hardcoded dark IM_COL32(28,30,38) box inside a light panel — a genuine violation of the both-themes constraint.
- **Change.** Move the fabric ink into theme.h semantic accessors with light branches (labels->dt_dim/dt_selected, spans->a dt_worldline pair, hop ink->a theme foreground) and derive the minimap background from WindowBg/ChildBg rather than the literal; add the fabric to the T2 contrast test. The near-black value chip sits on the blue span fill and is already theme-safe — leave it.
- **Files.** `desktop/src/loom/fabric_imgui.cpp` · `desktop/src/ui/theme.h`

#### #23 Give the register 'changed' highlight a second channel (asterisk)

`ux` · **medium** impact · small effort — Scrubber register file

- **Today.** scrubber_draw.cpp:96-100 signals a changed register only by coloring the value dt_changed_col — no shape/text channel — yet the pure model's golden dump already appends ' *' to changed regs (scrubber.cpp:180). The drawn deck rides on hue alone, invisible to colorblind readers and weaker at 2.0x.
- **Change.** Add a shape channel to changed rows — the same ' *' the dump uses, or a small arrow codicon — alongside dt_changed_col. No new color; survives 2.0x; the model already asserts the second channel.
- **Files.** `desktop/src/views/scrubber_draw.cpp` · `desktop/src/views/scrubber.cpp`

#### #24 Mark the torn prefix on the Scrubber playhead slider

`ux` · **medium** impact · small effort — Scrubber playhead transport

- **Today.** The playhead is a uniform SliderInt over [0,total-1] (scrubber_draw.cpp:64-68); the torn prefix [0,dropped) is discovered only by seeking into it and hitting the 'UNKNOWN, not zero' text. s.dropped/s.total are already in the model and the torn region is a first-class tested fact (a non-collapsible TORN banner).
- **Change.** Draw a thin draw-list strip under the slider spanning the [0,dropped)/total fraction in dt_warn/dt_refuse so the torn region is visible before the user seeks into it. Pure draw-half.
- **Files.** `desktop/src/views/scrubber_draw.cpp` · `desktop/src/views/scrubber.cpp`

#### #33 Color slice edges by cone membership

`dataviz` · **medium** impact · small effort — Slice explorer (edge encoding)

- **Today.** Nodes are tinted through cone_colour() with the four cone hues but every edge uses a hardcoded IM_COL32(150,150,150,200) gray (slice_view_draw.cpp:120), so flow reads on the nodes but not on the connective tissue.
- **Change.** Add a dt_cone style field to dt_slice_edge, set it in the builder by endpoint cone membership (both in back->back, both in fwd->fwd, selection-incident->both, else dimmed, none when no selection), and route the edge color through cone_colour(). Reuses the four existing accessors; off-cone edges use dt_cone_dim, keeping contrast in both themes.
- **Files.** `desktop/src/views/slice_view_draw.cpp` · `desktop/src/views/slice_view.cpp` · `desktop/src/views/slice_view.h`

#### #43 Mark writes vs reads on the watch value plot (shape, not color)

`dataviz` · **medium** impact · medium effort — Watch — value-over-hit PlotStairs

- **Today.** obs_watch_plot pushes one (hit_no,value) point per hit with no is_write branch, rendered as a single undifferentiated PlotStairs; direction lives only in the table's word column — so the view's own premise ('who wrote this field, and what did they write') can't be answered at a glance.
- **Change.** Extend WatchPlot with a parallel is_write channel (golden-tested), keep the PlotStairs step line, then overlay writes as filled PlotScatter markers and reads as hollow ones (marker fill is the second channel — no new color); undecodable stays dim/omitted, preserving the value_ok gate so no point is fabricated.
- **Files.** `desktop/src/views/observer_draw.cpp` · `desktop/src/views/watch.cpp` · `desktop/src/views/watch.h`

---

### 4.4 Comparisons force mental math

*The app's differential views (Scrubber deltas, ABI x-ray, A/B diff) show two states but make the reader do the subtraction or hunt for the answer — the register file shows only the new value, the ABI x-ray computes but never displays the differing-register set, and its two panes scroll independently so the cross-pane pairing drifts. Make the difference itself the rendered artifact.*

#### #9 Surface the ABI x-ray's differing-register set, and withdraw it when panes aren't step-aligned ★

`both` · **high** impact · small effort — ABI x-ray (cross-pane contrast + alignment honesty)

- **Today.** abixray.cpp:144-150 computes x.differ (the sorted differing-register names) and x.n_differ — the file's stated value-add — but the draw half surfaces it only as a per-row blue tint; the count and list are never displayed. Worse, when total_steps differ the builder sets x.aligned=false yet still populates row.differs at the same playhead (no !aligned guard), so it tints 'differs' on a comparison the misalignment invalidates.
- **Change.** After the descriptor line render 'N registers differ across conventions: <names>' in dt_selected_col (matching the in-table tint, so summary and rows can never disagree). In the builder, when !x.aligned do NOT populate row.differs/x.differ (keep each pane's own values; withdraw only the invalid cross-pane claim); optionally escalate the not-aligned banner to a non-collapsible integrity refusal. Land both halves together.
- **Files.** `desktop/src/views/abixray.cpp` · `desktop/src/views/abixray_draw.cpp`

#### #12 Show prev -> new for changed registers in the Scrubber

`dataviz` · **high** impact · medium effort — Scrubber register file

- **Today.** scrubber_draw.cpp:100 prints only the current 0x%llx and dt_scrubber_reg carries just {name,value,changed}, so to see a delta the user must step back and re-read. RegField.changed is already computed vs the previous held step, and StepIndex::at_step + RegFile::find make prev_value an O(1) lookup in the pure layer.
- **Change.** In scrubber.cpp, when s.has_prev, look up idx.at_step(ph-1) and fill prev_value/has_prev_value on dt_scrubber_reg (golden-tested). In scrubber_draw.cpp render changed rows as 'oldhex -> newhex' (old in dt_dim_col, new in dt_changed_col); where has_prev is false show only the current value so the first held step / torn edge shows nothing rather than a fabricated zero.
- **Files.** `desktop/src/views/scrubber.cpp` · `desktop/src/views/scrubber_draw.cpp` · `desktop/src/analysis/stepindex.h`

#### #19 Lock the two ABI x-ray panes' rows together

`ux` · **high** impact · medium effort — ABI x-ray (locked comparison layout)

- **Today.** Each convention is its own BeginChild+BeginTable(ScrollY) over the shared x.rows (abixray_draw.cpp:30,45-47), each with an independent scrollbar — so a deck taller than the pane (wide-vector rows) desyncs the panes and the cross-pane blue pairing drifts.
- **Change.** Prefer the lower-risk scroll-sync: keep the two side-by-side panes (honoring the stated two-scrubber premise) but drive both BeginChild scroll offsets from one shared SetScrollY. If a single [register | SysV | Win64] table is chosen for adjacency it is viable, but you must port the per-pane torn_here early-out to per-cell UNKNOWN (sysv_known/win64_known already exist) and preserve the per-pane changed tint so no honesty signal is lost.
- **Files.** `desktop/src/views/abixray_draw.cpp`

#### #34 Cap the A/B diff coverage block rows like heat does

`ux` · **medium** impact · small effort — A/B Diff coverage section

- **Today.** Heat is capped at top_heat (16) with an announced 'showing N of M' note when it bites, but coverage emits one navigable 'go' row for EVERY only_a/only_b offset with no cap — and coverage rows are pushed first, so a large diff buries the divergence card (the headline output) far below the fold.
- **Change.** Apply a top-N cap to only_a/only_b (reuse top_heat or add top_cov) and emit the identical 'showing N of M' note row when it bites, mirroring the announced-truncation contract. Pure model change; capped blocks stay reachable via canvas/timeline exactly as capped heat offsets are.
- **Files.** `desktop/src/views/diff_view.cpp`

---

### 4.5 Wired but undiscoverable — surface hidden affordances

*This is the project's known 'keymap advertises keys the UI never shows' pattern generalized: the command palette, history, recording-switch, camera controls and a whole interactive slice DAG exist in code but have no mouse-invocable or advertised surface, and some controls (palette KeyHint rows, dead-end graph nodes) actively read as broken buttons. Add menu/keyboard/hover surfaces that reuse the existing intent seams.*

#### #6 Expose palette / find / go-to / history in the menu bar and bindings table ★

`ux` · **high** impact · small effort — Menu bar (File / View / Help)

- **Today.** The docked menu bar is File/View/Help only with no mouse-invocable entry that raises the palette, find, go-to, or calls nav back/forward — all four exist only as keymap chords (shell.cpp:1546-1598). Ctrl+P is the true blind spot: it is absent from dt_nav_bindings() entirely, so it never appears in the Help overlay or the palette's own KeyHint list, discoverable only via the breadcrumb empty-state prompt.
- **Change.** Add a 'Go' menu whose MenuItems set the same intent flags the keymap sets (show_palette / find.open+focus / show_goto / dt_nav_back|forward, with BeginDisabled on empty stacks), and add a Ctrl+P / Ctrl+Shift+P row to dt_nav_bindings() (wired:true) so the palette self-advertises in the Help overlay and its own list.
- **Files.** `desktop/src/ui/shell.cpp` · `desktop/src/nav.cpp` · `desktop/src/views/diff_view_draw.cpp`

#### #11 Add a views-for-this-recording presence roster to Home

`both` · **high** impact · medium effort — Home pane — view findability

- **Today.** view_presence() produces a present/absent verdict for all 11 ViewIds but the docked shell consumes only the hosts() subset as center tabs plus one 'unavailable views (N)' affordance; the standalone-pane views' availability is learnable only by opening the pane or hunting View ▸ Panels. draw_home_rail has recents but no presence roster.
- **Change.** In draw_home_rail, under 'open recordings', draw a compact per-ViewId presence roster for the active recording: a present/absent text token per row (survives 2.0x and colorblind) on the good/neutral axis, click-to-open present views, absent rows carrying the verbatim view_presence reason on hover. Draw-only, reading the already-pure shell_view_presence.
- **Files.** `desktop/src/ui/shell.cpp` · `desktop/src/ui/view_presence.cpp`

#### #13 Make the slice DAG interactive: click a node to re-root, [/] to walk the cone

`ux` · **high** impact · medium effort — Slice explorer (def-use cone view)

- **Today.** Nodes are AddCircleFilled+AddText on a raw ImDrawList with only pan/zoom (slice_view_draw.cpp:125-153) — no per-node hit-test, no go callback; body_slice calls draw_slice_view one-way with no seam, and dt_slice_view_walk is referenced only by tests (dead in-app).
- **Change.** Add a per-node InvisibleButton (or manual hit-test against the circle) and on click invoke a new go/select callback that re-roots by writing s.selection.step through dt_nav_go — the exact single-writer seam body_timeline uses, not view-private state. Wire ImGuiKey_LeftBracket/RightBracket in body_slice to dt_slice_view_walk (brackets are free in the global keymap). Rebuild is the same tested dt_slice_view_build, so no fabricated structure.
- **Files.** `desktop/src/views/slice_view_draw.cpp` · `desktop/src/views/slice_view.cpp` · `desktop/src/ui/shell.cpp`

#### #26 Add a keyboard binding to switch between open recordings

`ux` · **medium** impact · small effort — Keymap / recording switching

- **Today.** handle_keymap binds 1-5 to switch the active recording's VIEWS but nothing switches which recording is active; the only keyboard route is the palette's 'Switch to <rec>'. The docked recording selector is the Home pane's mouse-only list.
- **Change.** Bind Ctrl+PageDown/Ctrl+PageUp to advance/retreat active_tab across s.ws.recordings via want_open_tab (both shells honor it), and add the row to dt_nav_bindings() as wired:true so it auto-surfaces in the help overlay and palette KeyHint list. Avoid Ctrl+Tab (ImGui docking nav).
- **Files.** `desktop/src/ui/shell.cpp` · `desktop/src/nav.cpp` · `desktop/src/ui/shell.h`

#### #27 Co-locate back/forward with the wayfinding readout

`ux` · **medium** impact · small effort — Navigation history affordance

- **Today.** The breadcrumb rides the top menu bar (shell.cpp:2326) while the '< back'/'forward >' buttons render at the BOTTOM inside a second Begin on kPaneRecording (2787-2807) — so if the Recording pane is closed the history controls vanish while the position readout stays up top. The two halves of the nav chrome sit at opposite ends.
- **Change.** Draw back/forward at the left of draw_wayfinding_bar (before the breadcrumb text) so history controls live with the position they act on and persist independent of the Recording pane, in both shells. Keep dt_nav_back/forward as the single spine and the existing BeginDisabled + target tooltip.
- **Files.** `desktop/src/ui/shell.cpp` · `desktop/src/ui/wayfinding.cpp`

#### #38 Surface the 3D camera controls — wired but undiscoverable

`ux` · **medium** impact · small effort — HUD / viewport control hint

- **Today.** The HUD exposes only reset/top-down buttons; left-drag orbit, wheel dolly and the full keyboard camera (arrows orbit, +/-/= dolly, R reset, T top-down) are advertised nowhere, and the scene primer mentions only '5' and 'press Play'. The project's known 'keymap advertises keys the UI never shows' pattern.
- **Change.** Add one dim (dt_dim_col) controls line — a corner ImDrawList overlay in draw_scene_overview ('drag orbit · wheel zoom · arrows/± · R reset · T top-down') or a HUD line beside the preset buttons. Keep it dim so it does not compete.
- **Files.** `desktop/src/scene3d/hud.cpp` · `desktop/src/ui/shell.cpp`

#### #41 De-emphasize command-palette KeyHint rows so they can't read as dead buttons

`both` · **medium** impact · small effort — Command palette (Ctrl+P / Ctrl+Shift+P)

- **Today.** build_palette appends every dt_nav_bindings() row as a null-action KeyHint (palette.cpp:171) and both draw paths gate dispatch on '&& e->action', so clicking a KeyHint is a silent no-op that leaves the modal open and reads as a broken button. Both the app and the render-only viewer take the ImSearch relevance-ranked branch at runtime.
- **Change.** Render KeyHint rows as a TextDisabled label + right-aligned key badge instead of a Selectable, in BOTH draw halves (the ImSearch SearchableItem loop and the plain loop), so a reference row can never read as a broken button in either binary. The SeparatorText category grouping is worth adding only to the plain null/uitest fallback, not the shipping ImSearch path (which relevance-ranks and cannot group by category).
- **Files.** `desktop/src/ui/palette.cpp` · `desktop/src/ui/palette.h` · `mk/desktop.mk`

#### #46 Make the canvas heat table sortable and jump to the divergence row

`ux` · **medium** impact · medium effort — Canvas table interaction

- **Today.** The canvas table is built with Borders|RowBg|ScrollY only (not Sortable), rows fixed ascending-by-off; the divergence 'patient zero' row is tinted and labeled but has no auto-navigation in a large scroll.
- **Change.** Add ImGuiTableFlags_Sortable and, when a sort spec is set, iterate a draw-local sorted copy of c.rows on the heat column (the pure model stays ascending-by-off for goldens — sorting is display-only, nothing filtered). Separately, when c.div_off is set, call SetScrollHereY on the patient-zero row once (guard with a one-shot bool). Both land in the draw half.
- **Files.** `desktop/src/views/canvas_draw.cpp`

#### #47 Don't auto-open placard-only viz panes for a sparse recording

`ux` · **medium** impact · medium effort — Docked pane visibility vs view presence

- **Today.** kPaneLoom/Observer/Timeline/Scrubber gate context solely on 'a recording exists' with default_open=true, ignoring the richer view_presence() verdict — so opening a trace-only or statistical recording opens panes that each render only their 'cannot fill' placard. The exact precedent (shell_apply_live_panes) already opens only the panes a live capture fills.
- **Change.** Compute shell_view_presence once per active recording and, on open/switch (guarded like shell_apply_live_panes' once-per-transition ordinal, not per-frame so a manual reopen holds), default-close the standalone panes whose ViewId is absent. Keep them reachable + reasoned in View ▸ Panels. Reuse the existing predicate.
- **Files.** `desktop/src/ui/shell.cpp` · `desktop/src/ui/view_presence.cpp`

#### #48 Group the Scrubber register deck by class

`ux` · **medium** impact · medium effort — Scrubber register file

- **Today.** The register deck is a single flat 2-column scrolling table in descriptor order (scrubber_draw.cpp:82-103), so a 30+ entry file is unscannable; stepindex_reg_order() supplies the canonical order and appends extras sorted.
- **Change.** Add a pure dt_scrubber_group classifier in scrubber.cpp (general-purpose; pointer/control rsp,rbp,rip,rflags; then appended extras — golden-tested) and lay the single 2-column table out under SeparatorText section headers per group. Avoid the multi-column small-multiple grid, which can overflow at 2.0x. A subset-emitting producer still shows only its recorded fields.
- **Files.** `desktop/src/views/scrubber_draw.cpp` · `desktop/src/analysis/stepindex.h` · `desktop/src/views/scrubber.cpp`

#### #53 Make the 'why/remedy' attach cell scannable: verdict tag + wrapped reason

`ux` · **medium** impact · medium effort — Processes picker ('cannot trace' diagnostics)

- **Today.** The why is drawn via unwrapped TextUnformatted (inspect_door.cpp:1159) and the procs table has ScrollY but no ScrollX, so a long refusal reason is CLIPPED at the cell boundary with no ellipsis — the tail of a first-class refusal reason is silently truncated (an honesty concern).
- **Change.** In attach_verdict (pure + tested) add a short reason_tag ('i386 ABI','already traced','scope 1','different uid','kernel thread','self') alongside the full why; show the tag as the primary scannable text and render the full why with TextWrapped bounded to the column so it is no longer clipped. Keep remedy + command hint; nothing removed.
- **Files.** `desktop/src/ui/inspect_door.cpp` · `desktop/src/live/inspect.cpp` · `desktop/src/live/inspect.h`

#### #54 Stop the wayfinding breadcrumb from clipping in the menu bar at scale

`ux` · **medium** impact · medium effort — Wayfinding breadcrumb (where-am-I band)

- **Today.** In the docked shell the breadcrumb is a single TextUnformatted line inside BeginMainMenuBar (wayfinding.cpp:177) — no wrap, scroll, or ellipsis — so a long disambiguated recording, 'vs B', 'pid N' and the filter tag run off the right and clip silently, far sooner at 2.0x. The windowed shell already draws it on its own row.
- **Change.** Have breadcrumb_model return ordered segments with priorities (recording+view+selection load-bearing; parent-path, diff-B, filter elidable) and drop low-priority segments to a trailing '…' with full text on hover. Lower-risk alternative: move the docked breadcrumb onto its own always-visible row beneath the menu bar (matching shell.cpp:1851). Never elide recording/view/selection.
- **Files.** `desktop/src/ui/wayfinding.cpp` · `desktop/src/ui/shell.cpp`

#### #59 Enter-to-attach on the focused process row

`ux` · **low** impact · small effort — Processes picker (keyboard)

- **Today.** The row Selectable single-click only sets selected_pid; full-detail attach requires a double-click or the right-click context menu (inspect_door.cpp:1105-1136) — there is no keyboard commit path.
- **Change.** When a row is selected and the table is focused, treat Enter as the same commit as double-click — call inspect_attach_full_detail(s, s.selected_pid) — and extend the existing double-click hint to mention it. Pure interaction, reuses the existing entry point, no model change.
- **Files.** `desktop/src/ui/inspect_door.cpp`

#### #60 Visually distinguish dead-end (unnavigable) graph nodes

`ux` · **low** impact · small effort — Graph-nav canvas (topo / call-tree / hot-edges)

- **Today.** graph nodes with addr==0 have has_link=false, but the draw half renders every node's label identically and gates double-click nav on has_link — while the panel advertises 'double-click a node to open it', so a dead-end node looks navigable but silently no-ops.
- **Change.** Wrap the label draw (observer_draw.cpp:102) with PushStyleColor(ImGuiCol_Text, dt_dim_col())/Pop when !n.has_link so a dead-end reads as quiet/non-navigable, matching the app's greyed-shows-why idiom. Draw-half only; reuses the dim accessor; the node still renders with its edges — only its navigability affordance is made honest.
- **Files.** `desktop/src/views/graph_nav.cpp` · `desktop/src/views/observer_draw.cpp`

#### #61 Carry the active invocation pass in the breadcrumb

`dataviz` · **low** impact · small effort — Wayfinding breadcrumb (position coordinate)

- **Today.** shell_df_pass_pager draws 'pass k of N — following the latest / pinned' inline in each dataflow view, but breadcrumb_model never includes the pass, so switching to a non-pass-aware view drops the pass from the persistent 'where am I' band.
- **Change.** Add a pass field to BreadcrumbModel populated read-only from s.seg_df/s.df_pass for active_tab ('pass 2/5' following latest / 'pinned'), only when passes>1, drawn between view and selection. Compute the applied index inline — do NOT call the mutating shell_apply_df_pass from the pure model. Honesty language reused, no fabricated continuity.
- **Files.** `desktop/src/ui/wayfinding.cpp` · `desktop/src/ui/shell.cpp` · `desktop/src/ui/shell.h`

#### #62 Mark a vanished or broken recent in the Home list

`ux` · **low** impact · small effort — Recents list (Home rail)

- **Today.** draw_home_rail renders every recent identically; on a failed reopen the path is deliberately kept 'with its error (D7)' but the error only lands transiently in s.status on the next click, so a dead recent looks identical to a healthy one and invites repeated failed clicks — the D7 promise is only half-kept.
- **Change.** Persist a per-recent last-open-error (change s.recents from vector<string> to a small struct, or probe existence at draw time for the short list) and render a broken recent with a neutral/caution text token + reason on hover, visually distinct from a healthy row. Model piece in the recents structure; draw half reads it.
- **Files.** `desktop/src/ui/shell.cpp` · `desktop/src/ui/shell.h`

---

### 4.6 Spatial anchoring and channel completeness in the Loom fabric and 3D scene

*The two spatial views under-use the data they already hold: the Loom fabric never marks the playhead or brushed step, has an unreachable lane deck and unlabeled worldlines; the 3D terrain paints one height ramp and drops the six region hues, the trajectory ignores the playhead, and the viewport gives no pick preview. Each fix renders an existing, tested fact into the spatial channel — no fabricated structure.*

#### #3 Make the Loom vertical lane deck scrollable — off-screen lanes are unreachable ★

`ux` · **high** impact · small effort — Loom -> fabric canvas (vertical deck)

- **Today.** px_h is fixed at avail.y*0.62 and loom_plan renders only lanes [lane0, lane0+visible), but L.cam.lane0 is read (fabric_imgui.cpp:321, fabric_plan.cpp) and assigned nowhere — permanently 0. There is no wheel, scrollbar, or drag, so any recording with more lanes than fit has its remaining worldlines unseeable, unhoverable and unselectable. This is a correctness-level gap, not cosmetic.
- **Change.** When the canvas is hovered, write L.cam.lane0 from GetIO().MouseWheel, clamped by a pure unit-tested loom_view_clamp_lane0 to [0, max(0, lanes - visible)]; draw a thin scrollbar or an 'N lanes below' affordance when clamped so the truncated viewport is disclosed, not hidden.
- **Files.** `desktop/src/loom/fabric_imgui.cpp` · `desktop/src/loom/fabric_plan.cpp`

#### #8 Draw the def-use edge's carried location on the slice cone ★

`dataviz` · **high** impact · small effort — Slice explorer (def-use edges)

- **Today.** slice_view.cpp:141 sets se.loc = loc_str(edge_loc) and dumps it in the golden text, but the draw half renders each edge as three bracket AddLine segments (slice_view_draw.cpp:120-123) and never references e.loc — so the value that flows along each def-use edge is invisible in the GUI.
- **Change.** AddText e.loc at the midpoint of the top bracket segment in dt_dim_col — the trivial always-safe half needing no new infra. Defer the near-arc hover-tooltip variant to when the slice hit-test lands (arcs are raw ImDrawList lines with no InvisibleButton). Midpoint labels may overlap on dense DAGs at 2.0x, but that is honest surfacing of real data.
- **Files.** `desktop/src/views/slice_view.cpp` · `desktop/src/views/slice_view_draw.cpp` · `desktop/src/views/slice_view.h`

#### #14 Show hover feedback and a pick preview on the 3D viewport

`ux` · **high** impact · medium effort — Viewport picking / hover

- **Today.** Picking fires only on left-release-without-drag (shell.cpp:939-949); vp_hover only gates orbit/dolly, nothing is surfaced on hover. resolve_pick returns only a dt_link and discards the region label/address/density it computes internally, returning nullopt for padding — so nothing tells the user what a click will do.
- **Change.** Add a pure resolve_pick_hint returning {region label, address/offset, cell density, target-view name via dt_view_name, is_padding} (golden-testable). In the vp_hover branch run scene_host->pick for the hovered pixel THROTTLED to actual mouse-move (it is a GL FBO readback), decode+resolve, and show a tooltip; say 'padding — nothing here' on nullopt. Text tooltip survives 2.0x.
- **Files.** `desktop/src/ui/shell.cpp` · `desktop/src/scene3d/pick.cpp`

#### #15 Paint region-kind hue on the terrain plane

`dataviz` · **high** impact · medium effort — Terrain fragment shading / terrain model

- **Today.** kTerrainFrag colors every cell with one dark-blue->orange height ramp plus TF_* tints (embedded.h:45-52); region_style's six hues (code/stack/heap/data/mmap/unknown) are used only as HUD legend swatches, never on the plane. The ramp's hot color even coincides with the code hue, so stack/heap/data hills read identically today.
- **Change.** Add a per-cell region-kind array to the Terrain model from the same proj.unproject already used per cell (pure, golden-testable), upload it as an R8UI lookup texture in Scene::set_terrain, and in kTerrainFrag tint the base by the region_style hue modulated by clamp(vHeight); apply the TF_* honesty tints AFTER so torn/churn/statistical still override. Cells with no region stay neutral.
- **Files.** `desktop/src/scene3d/shaders/embedded.h` · `desktop/src/space/projection.cpp` · `desktop/src/scene3d/scene.cpp` · `desktop/src/scene3d/hud.cpp`

#### #20 Wire the fork/take verdict overlay and Reweave gesture into the app

`both` · **high** impact · large effort — Loom -> Takes tab + fabric canvas overlay

- **Today.** The flagship 'what-if' interaction is dead: fabric_imgui.cpp:525-534 sets reweave.requested but shell.cpp never consumes it; loom_take_run_from_step/loom_take_view/loom_take_plan are referenced only by tests; and draw_loom_plan carries take_dim/take_hot/patient_zero painter cases no in-app plan ever emits.
- **Change.** Consume L.loom.reweave.requested each frame, but gate the engine leg (loom_take_run_from_step -> loom_take_view -> append reversible TakeSet) behind the codebase's per-object compile-define seam (reuse ASMTEST_DESKTOP_CAN_AUTHOR or a sibling on the shell.o app build) — shell.cpp is compiled into the engine-free viewer too, so no raw engine call in shared code. The take overlay itself is pure: call loom_take_plan after loom_plan so patient-zero/dim/hot verdicts paint in both binaries; loom_take_plan only draws patient_zero on real divergence and carries fault/err verbatim.
- **Files.** `desktop/src/loom/fabric_imgui.cpp` · `desktop/src/ui/shell.cpp` · `desktop/src/loom/forks.cpp` · `desktop/src/loom/take_view.cpp` · `mk/desktop.mk`

#### #25 Draw the timeline viewport as a shaded window, not two hairlines

`dataviz` · **medium** impact · small effort — Timeline overview / minimap strip

- **Today.** The viewport is PlotInfLines with bounds {*lo,*hi} (timeline_draw.cpp:51-52) — two vertical lines with no fill between them — so the selected span reads as two disconnected hairlines rather than a window.
- **Change.** Shade the [lo,hi] span with a low-alpha fill in dt_selected_col (an ImPlot shaded region or a draw-list rect over the plot rect) and keep the two edge InfLines for a crisp boundary. Pure draw-half; span still sourced only from the ImZoomSlider, so nothing is fabricated.
- **Files.** `desktop/src/views/timeline_draw.cpp`

#### #30 Give Loom worldlines a hover identity and highlight

`ux` · **medium** impact · small effort — Loom -> fabric canvas hover/tooltip

- **Today.** note() surfaces a tooltip only for honesty prims; span/span_hollow/hop push empty text and their switch cases never call note(), so hovering an ordinary worldline yields nothing and there is no hover highlight; narrow spans additionally suppress their value chip.
- **Change.** In loom_plan fill each span/hop prim's text with a compact identity ('<reg> = 0x.. @ step N', 'mem[lo..hi) store @ step N', '(value never captured)' for hollow, matching loom_biography's wording) and add note(p) calls to those switch cases; in the draw half redraw the hovered span with a brighter dt_selected outline. Golden-test the deterministic text.
- **Files.** `desktop/src/loom/fabric_imgui.cpp` · `desktop/src/loom/fabric_plan.cpp`

#### #31 Draw the playhead as a marker on the Loom fabric canvas

`both` · **medium** impact · small effort — Loom -> fabric canvas

- **Today.** L.playhead drives loom_audit and is set by scrub and minimap click, but loom_view_t has no playhead field, loom_plan emits no playhead prim, and draw_loom_plan draws nothing at the playhead x — so the Zeroization-audit rows have no spatial anchor to a column.
- **Change.** Add a playhead field to loom_view_t, set it from L.playhead in draw_loom, and emit a full-height playhead prim in loom_plan at x_of(playhead) (skip when off-window), painted as a 1px vertical in dt_selected_u32 (distinct from the amber torn edge and hot patient-zero). Model half so a golden pins that scrubbing moves the marker.
- **Files.** `desktop/src/loom/fabric_imgui.cpp` · `desktop/src/loom/fabric_plan.cpp` · `desktop/src/loom/fabric_plan.h`

#### #36 Give convergence arcs a HUD toggle and legend entry

`both` · **medium** impact · small effort — HUD layer toggles + legend

- **Today.** render() already gates the bright magenta convergence arcs and lavender access spurs on layers.convergence (scene.cpp:475), but the HUD layer row offers only terrain/exact/statistical/access and the legend lists only regions — so the user sees unexplained magenta arcs with no key and no way to hide them, though the plumbing to hide them exists.
- **Change.** Add a 'convergence' Checkbox bound to s.layers.convergence plus legend swatches for the magenta arc, the lavender access spur, and the exact-vs-statistical path — entirely in draw_scene_hud; no render change since the gate already honors the flag. Reuses existing colors.
- **Files.** `desktop/src/scene3d/scene.cpp` · `desktop/src/scene3d/hud.cpp` · `desktop/src/scene3d/scene.h`

#### #37 Complete the 3D HUD legend: label the terrain's own encodings

`dataviz` · **medium** impact · small effort — HUD legend

- **Today.** The HUD legend lists region-kind swatches only; the plane also encodes height->density, CHURN->cyan, STAT->dim and TORN->red gash, none of which appear as a visual key — the provenance chips name them in words but the color->meaning key is absent.
- **Change.** Add an 'encodings' block to the HUD legend: a height->density gradient swatch and swatches for churn (cyan), torn (red gash) and statistical (dim). Define the swatch colors as C++ constants mirroring the GLSL literals with a 'keep in sync with kTerrainFrag' comment (the GLSL string can't include a C++ header — mirror the existing TF_* bit-sharing pattern). Giving torn/statistical a key reinforces D7.
- **Files.** `desktop/src/scene3d/hud.cpp` · `desktop/src/scene3d/shaders/embedded.h`

#### #39 Fix redundant in-canvas Loom lane labels; add lane bands and deck linkage

`both` · **medium** impact · small effort — Loom -> lane rows / deck linkage

- **Today.** loom_plan emits a lane_header at x0=0 for every visible lane, painted at the far-left over any span/ribbon reaching the left edge; the same names also appear in the 200px scrollable left deck; there are no separators or zebra, so rows are hard to map to identity.
- **Change.** Emit a faint alternating lane-band prim (or draw-half zebra keyed to lane index) plus a thin separator, and give the in-canvas label a small opaque backing chip so it stops colliding with left-edge worldlines — do NOT drop it (the deck scrolls independently of lane0, so the in-canvas label is the fabric's only inline row identity). Highlight the fabric row of the deck-selected lane and vice-versa.
- **Files.** `desktop/src/loom/fabric_imgui.cpp` · `desktop/src/loom/fabric_plan.cpp`

#### #40 Add iso-density contour lines to the terrain

`dataviz` · **medium** impact · small effort — Terrain fragment shading

- **Today.** kTerrainFrag applies the height ramp + TF_* tints with NO lighting, no normals, no gridlines — relief is conveyed only by color + perspective on an unlit orbit height field, and there is no quantitative banding.
- **Change.** In kTerrainFrag darken the base where fract(vHeight*kLevels) is near a small threshold to draw a few iso-density isolines (relief cue + honest banding), guarded so a zero/flat cell shows no line; optionally expose kLevels as a uniform. Contours are a faithful re-encoding of the height value — sparse stays sparse. Purely embedded.h.
- **Files.** `desktop/src/scene3d/shaders/embedded.h`

#### #49 Give the generation walk ([ / ]) spatial feedback on the fabric

`dataviz` · **medium** impact · medium effort — Loom -> lineage selection / fabric dim

- **Today.** loom_select fills per-step generation[] plus gen_lo/gen_hi and [ / ] move sel.gen_view, but loom_plan does only a binary in/out-closure dim and never reads generation — so [ / ] changes a printed number and highlights nothing on the fabric.
- **Change.** Extend loom_view_t with the per-step generation map + gen_view (populated in draw_loom when a Loom selection is active), have loom_plan widen the dim 'b' bit to a 2-bit emphasis level (current-generation bright, other in-lineage mid, off-cone kSpanDim), and let the painter map level->alpha of the same blue. Emphasis-by-brightness is a legitimate second channel; golden-test that ']' brightens the next ring.
- **Files.** `desktop/src/loom/lineage.cpp` · `desktop/src/loom/fabric_imgui.cpp` · `desktop/src/loom/fabric_plan.cpp` · `desktop/src/loom/loom_draw.h`

#### #50 Clip the 3D trajectory to the playhead and mark the execution front

`both` · **medium** impact · medium effort — Trajectory upload / render + playhead

- **Today.** Each trajectory is drawn as a full GL_LINE_STRIP over all vertices with no clip, while the terrain re-slices on the playhead — so at playhead t the terrain shows [0,t] residency but the worldline shows the full [0,nsteps]. The primer's 'press Play to watch the path form' is only half-kept.
- **Change.** Add a uTimeCutY = playhead*scale uniform to the trajectory shaders, set from SceneFrame::slice_t (already carries the playhead — no new field), and in kTrajFrag dim (not discard) fragments past the cut and draw a bright point at the execution front. Uses the same trace-time t the terrain slices on, so the two axes stay separate and no data is hidden.
- **Files.** `desktop/src/ui/gl_scene_host.cpp` · `desktop/src/scene3d/scene.cpp` · `desktop/src/ui/scene_host.h`

#### #55 Let the 3D camera pan / recenter on a region of interest

`ux` · **medium** impact · medium effort — Camera controls

- **Today.** Camera fixes target={0.5,0,0.5}; only orbit() and dolly() mutate it and reset()/top_down() reset them — there is no pan and no recenter, so after dollying toward a corner region you can only orbit the fixed center, blocking close study of an off-center cell (a gap against 'use 3D to FIND a place').
- **Change.** Add Camera::pan(dx,dz) translating target in XZ clamped to [0,1]^2 (pure, add a test_camera case), wired to middle-drag or shift+left-drag; add double-click-to-recenter setting target to a picked cell's (u,v) via the existing pick path. Orbit stays left-drag (no conflict); clamping keeps the target on the model.
- **Files.** `desktop/src/scene3d/camera.h` · `desktop/src/ui/shell.cpp`

#### #56 Add a vertical scale reference and clarify the dual vertical meaning

`dataviz` · **medium** impact · medium effort — Scene axis overlay + HUD note

- **Today.** Trajectory vertices sit at world-Y = t*scale (trace-time) while terrain Y = normalized-density*y_scale — two different quantities share the vertical axis with no ticks, labels, or scale marker anywhere.
- **Change.** Draw a thin dim vertical ruler at a plane corner with a few ticks labeled 0..nsteps for the TRAJECTORY time axis only (ImDrawList projecting world points through cam.mvp), clearly scoped so it isn't read as terrain height, plus a one-line HUD note that terrain-height=density while path-height=trace-step. Under the null backend the HUD note still reads.
- **Files.** `desktop/src/scene3d/scene.cpp` · `desktop/src/scene3d/scene.h` · `desktop/src/scene3d/hud.cpp`

#### #57 Collapse only dense runs, not the whole Loom lane, on one thin span

`dataviz` · **medium** impact · medium effort — Loom -> zoom-collapse / density ribbon

- **Today.** loom_plan sets collapse=true if ANY visible span in the lane is thinner than 3px, then replaces the ENTIRE lane with density buckets — but this whole-lane rule is a deliberate documented choice (fabric_plan.cpp:187-189) to avoid wide rectangles reading as 'the only thing that happened', so a naive per-run change could under-represent activity.
- **Change.** Change collapse to per-RUN: coalesce only contiguous sub-threshold spans into local ribbon segments and keep individually-resolvable spans as rectangles in the same lane — but weight/label the ribbon segment (intensity/height reflecting the hidden count, or a small 'N here' cue) so surviving wide neighbours cannot imply nothing dense happened, answering the documented rationale rather than reverting it. Pure model; golden-test the segmentation.
- **Files.** `desktop/src/loom/fabric_plan.cpp`

#### #58 Provide an honest GL-free 2D terrain fallback and a flat reading surface

`both` · **medium** impact · large effort — 3D overview pane — null-backend / no-GL path

- **Today.** Under the null backend the pane shows a text placard + full HUD (an honest text degradation, so no constraint is broken), but the height field / trajectories have no VISUAL rendering, and even top_down() projects through mat4x4_perspective, so there is no perspective-free surface to read exact density. The pure Terrain slice makes a flat ImDrawList heatmap fully feasible.
- **Change.** Add a pure-model 2D top-down surface (views/scene2d_draw.cpp): one ImDrawList filled rect per Terrain cell colored by the same height ramp + TF_* flags (+ region hue via proj.unproject), each trajectory a proj.project polyline. Render it in the null-backend branch (a richer degradation than the placard) and expose it as a 'flat 2D' HUD toggle for the GL path. Keep the cell->color mapping in a golden-testable model helper. An enhancement, not a constraint fix.
- **Files.** `desktop/src/ui/shell.cpp` · `desktop/src/scene3d/camera.h` · `desktop/src/ui/scene_host.h` · `desktop/src/space/terrain.h` · `desktop/src/space/types.h`

#### #63 Encode hop direction with an arrowhead at the consumer end

`dataviz` · **low** impact · small effort — Loom -> hops (def-use edges)

- **Today.** A hop is a symmetric cubic bezier at uniform 1.5px width, dim or bright (fabric_imgui.cpp:92-96); the producer is always the left endpoint so coarse direction is implied by the time axis, but an individual crossing edge is hard to trace through dense bezier crossings.
- **Change.** In the draw half only, add a small filled arrowhead (or a producer-dim->consumer-bright alpha taper) at the consumer end, sized to survive 2.0x. No model change. Frame the benefit as tracing individual edges through dense crossings, since coarse direction is already implied by the left=producer axis.
- **Files.** `desktop/src/loom/fabric_imgui.cpp` · `desktop/src/loom/fabric_plan.cpp`

#### #64 A brushed single step should also drop a fabric column marker

`dataviz` · **low** impact · small effort — Loom -> cross-pane brushing / fabric dim

- **Today.** For a single cross-pane brushed step the fabric lights only the span(s) written AT that step (a coherent 'what this step produced' semantic) and dims the rest — but there is no spatial marker anchoring the brushed column, so the brush is easy to lose.
- **Change.** Add a brushed-step vertical marker (reuse the playhead prim mechanism in a distinct-but-related tint) so the brushed column is anchored regardless. Separately, OFFER residency-based highlighting (select via f.resident: t_write <= step < t_end) behind an explicit golden-tested model flag — present it as a semantic choice, not a bug fix, since the existing exact-write behavior is meaningful.
- **Files.** `desktop/src/loom/loom_draw.h` · `desktop/src/loom/fabric_plan.cpp`

#### #65 Give the Loom density ribbon a scale (log intensity + legend)

`dataviz` · **low** impact · small effort — Loom -> density ribbon

- **Today.** Ribbon alpha = 30 + 40*(count>5?5:count), capping at 5 concurrent spans with no legend — a silent clip, and the minimap's maxw is step-density (not worldline concurrency) so it can't be reused for normalization.
- **Change.** Map ribbon intensity through a log curve of the live-span count (absolute, monotonic past 5) rather than max-normalization, so the high end stays legible without cross-recording ambiguity; if normalization is used, compute a dedicated max-concurrency (not the minimap's maxw). Add a one-line scale hint near the take legend ('ribbon darkness = concurrent worldlines'). Mapping in the model, hint draw-side.
- **Files.** `desktop/src/loom/fabric_imgui.cpp` · `desktop/src/loom/fabric_plan.cpp`

---

### 4.7 Live capture: follow the tail and keep view state as data streams in

*Under live capture the incremental rebuild fights the reader: streams don't auto-scroll to new rows, the region pager gets yanked back to invocation #0 (and syscall reveals re-hide) on every arriving event, and the status shows only a rising integer with no rate, composition, or clear picture of which mode holds the ptrace jack. Preserve view state across rebuild and surface the flow.*

#### #4 Preserve the invocation pager across live rebuilds (stop the yank to #0) ★

`ux` · **high** impact · small effort — Invocations tab under live region/dataflow capture

- **Today.** observer_build() reassigns s.region wholesale (observer_draw.cpp:189) and RegionView::selected defaults to 0 (region.h:61), while live_observer_build re-runs it on every event-count move (inspect_door.cpp:714-726). So on a live region capture every arriving instruction resets the pager to invocation #0, and the same rebuild resets SyscallView::reveal_all/revealed[], silently re-hiding revealed payloads.
- **Change.** Lift the page index into ObserverState (as disasm_when already is) and thread it into obs_region, or carry the old s.region.selected forward clamped to the new invocation count. Re-hiding syscall reveals on new data is a defensible safe default — call it out explicitly rather than leaving it silent.
- **Files.** `desktop/src/views/observer_draw.cpp` · `desktop/src/views/region.h` · `desktop/src/ui/inspect_door.cpp`

#### #5 Auto-scroll the live syscall/watch/tree streams to the newest row ★

`ux` · **high** impact · small effort — Syscalls / Watch / Tree tables under live capture

- **Today.** The syscall (observer_draw.cpp:248-276) and watch (344-375) tables use ScrollY but never call SetScrollHereY, and the flat call list isn't even in a scroll child — so live rows arrive off-screen while the view sits pinned at the top. The Log pane already does the correct tail-follow idiom (shell.cpp:2203-2204).
- **Change.** After the last row of each table add `if (GetScrollY() >= GetScrollMaxY()) SetScrollHereY(1.0f);`, lifting the shell.cpp:2203 idiom verbatim so it follows the tail only while already pinned to the tail. Wrap the Tree list in a ScrollY child first (see the call-tree rec) so the same line applies.
- **Files.** `desktop/src/views/observer_draw.cpp` · `desktop/src/ui/shell.cpp`

#### #17 Show live capture rate and per-kind composition, not just a rising integer

`dataviz` · **high** impact · medium effort — Live capture status (growing-recording feedback)

- **Today.** draw_status shows only 'capturing: %llu event(s) so far' (inspect_door.cpp:359) plus an indeterminate bar — no rate, no composition — even though Recording::by_kind is a public map and event_count() sums it, so a per-kind tally and a rate derivative are both trivially available.
- **Change.** Add a pure rate helper (ring of (now,event_count) samples -> events/sec over a trailing window, clock injected like s.stream_op) and render an events/sec figure or tiny dt_dim sparkline plus a one-line per-kind tally from g->by_kind ('df_step 1.2k · syscall 412 · mem 88'). Keep the indeterminate bar — no honest total is fabricated.
- **Files.** `desktop/src/ui/inspect_door.cpp` · `desktop/src/doc/recording.h` · `desktop/src/doc/recording.cpp`

#### #18 Encode ptrace-vs-free in the mode picker and show the jack's occupant inline

`both` · **high** impact · medium effort — Patch bay (mode picker / budget mental model)

- **Today.** draw_patch_bay renders 10 modes as a flat RadioButton row under 'one ptrace jack per target', but mode_uses_ptrace (the free-vs-jack fact) surfaces only inside per-mode hover tooltips, and the running occupant is never shown; arm64 hazard rows are BeginDisabled with the reason only in the tooltip. All the facts (mode_uses_ptrace, s.active, status().pid) already exist.
- **Change.** Annotate the picker by mode_uses_ptrace (a 'free · out-of-band' dt_dim tag on sample, a divider from the jack modes), draw a standing 'jack:' line derived from s.active (a vector<LiveMode>) + status().pid ('jack: free' / 'jack: auto capturing on pid N'), and add a caution glyph on the greyed arm64 rows. Reuse dt_dim and the existing caution token; no new color.
- **Files.** `desktop/src/ui/inspect_door.cpp` · `desktop/src/live/budget.cpp` · `desktop/src/ui/doors.h`

#### #32 Give the hot-edge heatmap a hover read-back and rank ticks

`dataviz` · **medium** impact · small effort — Hot edges — src×dst Viridis heatmap

- **Today.** The heatmap sets NoDecorations axes, NoMouseText, and label_fmt=nullptr (observer_draw.cpp:445-452), so mapping a hot cell to its from/to pair means counting grid cells against the separate table. HotEdgeMatrix already holds rows/cols/cells, so the answer needs no new model.
- **Change.** On IsPlotHovered(), map GetPlotMousePos() to (r,c) and show a tooltip 'rows[r] -> cols[c]: <cells> samples'; add short rank tick labels (#1..#N) on both axes so a cell maps to the table's # column; for small matrices pass label_fmt "%.0f". Pure draw — keeps Viridis + the labelled ColormapScale, no interpolation.
- **Files.** `desktop/src/views/observer_draw.cpp` · `desktop/src/views/hotedges.cpp`

#### #44 Turn the Invocations pager into a clickable filmstrip overview

`both` · **medium** impact · medium effort — Invocations tab — discrete invocation paging

- **Today.** draw_obs_region uses a '< prev / #k of N / next >' pager only, so finding the one large or truncated invocation among many is blind linear stepping, even though RegionInvocation carries insns_total, truncated and closed.
- **Change.** Draw one discrete bar per invocation, length ∝ insns_total, selected highlighted with dt_selected_col, truncated/open flagged in caution amber; clicking a bar pages to it. Keep visible gaps between bars — never a continuous slider — so the 'never a scrub' honesty marker is complemented, not replaced. Discrete random-access is legitimate here.
- **Files.** `desktop/src/views/observer_draw.cpp` · `desktop/src/views/region.cpp` · `desktop/src/views/region.h`

#### #45 Make the interleaved call tree legible: thread count, boundaries, scroll

`both` · **medium** impact · medium effort — Tree tab — indented call list

- **Today.** draw_obs_tree renders a flat Text list with manual Indent by depth*12 (observer_draw.cpp:597-604), not in a scroll child, not collapsible; obs_tree_tids() exists to count interleaved threads but no draw half consults it, so with >1 thread rows braid by arrival with the only thread cue buried at end-of-line.
- **Change.** Wrap the list in a ScrollY child (also enabling the auto-scroll rec), show 'N threads interleaved' from obs_tree_tids() at the top, and draw a subtle monochrome separator whenever tid changes between rows. Higher-effort add-on: collapsible TreeNodes built from the emitted effective depth (never recomputing depth). Explicitly NOT per-tid categorical color.
- **Files.** `desktop/src/views/observer_draw.cpp` · `desktop/src/views/tree.cpp` · `desktop/src/views/tree.h`

#### #51 Summarize the --auto evidence: promote the pick, fold retries into a count

`ux` · **medium** impact · medium effort — Auto-capture evidence (entry-vs-residency)

- **Today.** draw_status iterates ALL notes() and prints full-sentence pick_walk_note + pick_evidence_label per candidate and per idle window, so a multi-candidate walk becomes a growing transcript with the landed pick buried.
- **Change.** Add a pure fold over the parsed AutoPicks returning {candidates_walked, idle_windows, final_pick}; render the landed pick prominently with its entry(kGood)/weak-residency(kMaybe) label un-collapsed, and collapse the history to one counted line ('walked 3 candidates · 2 idle windows — show') expandable to the existing per-note detail. Keep the weak-residency label first-class for the landed pick.
- **Files.** `desktop/src/ui/inspect_door.cpp` · `desktop/src/live/inspect.cpp`

---

## 5. Provenance

Generated 2026-07-29 by a 17-agent grounded-review workflow (8 cluster reviewers
→ adversarial ground-check → synthesis) driven from
[CLAUDE.md](../../../CLAUDE.md)'s ultracode mode. Sibling review docs live beside
this one in [`docs/internal/analysis/`](.); the earlier Nielsen heuristic pass and
the UX-restructure briefs (gui docs 18–24) are the prior GUI-UX work this
extends.
