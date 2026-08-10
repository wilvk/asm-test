# The session strip — a 2D whole-session scene (design)

**Date:** 2026-08-10.
**Status:** approved design, ready for an implementation plan.
**Scope:** desktop/ only — pure model + draw. No engine, wire, or schema change.

## Why this scene exists

The recording already carries four families of session facts, and today each
one is readable only in its own corner:

- **memory accesses** (`mem` events) are 3D-only substrates — data ribbon,
  terrain relief, sediment — or access spurs on the flat plane at one playhead;
- **syscalls** (`syscall` events) are a flat table (Observer deck) or 3D
  crossing spurs;
- **processes/threads** are a card list built from the last `topo` snapshot,
  plus per-tid hues on 3D worldlines;
- **runs** (continuous-capture passes, region invocations, live captures) are
  `df_invocation` segmentation, Observer lifecycle rows, and the live union.

There is no single surface where a person can *watch the session happen*:
see thread 12841 go hot, hammer one heap span, cross into the kernel with a
burst of `File`-class calls, and then see the pass end and the next one start —
all in one glance, growing live. The session strip is that surface.

It is 2D on purpose: an append-only, multi-channel chart survives scale and
live growth in a way a camera-in-a-world scene does not, and it complements
the 3D pane rather than competing with it (same palettes, same drill-in
router, same honesty rules).

## The one shared axis

**X is `Event::seq` — stream position across every kind**
([recording.h:66-75](../../../desktop/src/doc/recording.h#L66)). It is the only
ordering primitive every consumed kind actually carries: `syscall` events have
*only* seq; `mem`/`trace`/`df_step` events have their own seq by construction;
and `merge_session_recordings` reassigns seq as a running offset so a
multi-capture live session stays monotonic
([recording_union.h:31-35](../../../desktop/src/doc/recording_union.h#L31)).

Because every mark sits at its **own** event's seq, this scene needs none of
the crossing layer's anchor-approximation machinery — a syscall tick's x *is*
its stream position, not "the nearest recorded instruction before it".

The normative ban travels with the axis: seq orders events and measures
nothing ([space/crossing.h:11-19](../../../desktop/src/space/crossing.h#L11)).
The axis label is pinned in the model as a `static const char *` —
**"stream order — not time"** — and a test asserts it verbatim (the
`CrossingLayer::dwell_note()` precedent). No prim may have along-axis extent
that could read as a duration: syscall ticks are points, and density/envelope
prims aggregate *counts within a pixel column*, never spans between events.

## Approaches considered

**A — Session strip (chosen).** Stream-order X; stacked channel bands
(thread deck, kernel rail, address strip, run ribbon); live follow-tail
camera; pure planner → pixel prims → dumb painter (the Loom architecture).
Every requested fact shares one honest coordinate system, and "real-time"
means the picture *grows* at the right edge rather than blinks.

**B — Animated address map (rejected).** The flat plane (scene2d) with
accesses/threads animated at a playhead. Time collapses to one instant — no
history at a glance; syscalls and runs have no spatial home; and it
duplicates what `scene2d` + the scrubber already are.

**C — Composite dashboard (rejected).** Topo cards + syscall table + a mem
ribbon docked together. No shared axis, so no cross-fact reading — precisely
the gap this work exists to close. It is also not a new scene.

## What the strip shows (top to bottom)

One camera drives every band: `seq0` + `seq_per_px` horizontal, exactly the
Loom's `loom_view_t` shape ([fabric_plan.h:64-74](../../../desktop/src/loom/fabric_plan.h#L64)).

### 1. Thread deck — one lane per tid

- Tids are discovered from what the strip actually draws: `trace.tid`
  (optional), `call.tid`, `watch.tid`, `syscall.tid` (v2 writers). NOT
  `stitch.tid` — a tid known only from PT slices has no strip-visible events
  and would make an empty lane. A recording with no tids anywhere gets ONE
  lane labelled `(single stream)` — the replay/tid-−1 case, never hidden.
- Lane content: an activity **density ribbon** — count of that tid's
  `trace`/`call`/`watch` events per pixel column — plus that tid's syscall
  ticks when the wire carried `tid`.
- **Process grouping:** lanes order by (tgid, leader first, tid ascending)
  using the LAST `topo` snapshot only
  ([topo.h:64-67](../../../desktop/src/views/topo.h#L64)); a thin separator +
  `comm [tgid]` header row splits groups when >1 tgid is known. Lanes whose
  tid the snapshot does not know keep a bare `[tid]` label — the label never
  guesses. When no `topo`/`procinfo` exists, lanes are flat ascending-tid.
- Vertical scroll reuses the Loom's pure lane math (`lanes_full` /
  `lane_max` / `scroll_lanes` — [fabric_plan.h:103-105](../../../desktop/src/loom/fabric_plan.h#L103)).

### 2. Kernel rail — every syscall, one shared band

- Every `syscall` event is a **point tick** at its seq: hue = derived
  `SyscallClass`, ring tint = `SyscallOutcome`, thickness = payload byte
  count when one was on the wire. Class/outcome come from the SAME parse the
  crossing layer uses — `class_of()` is currently file-local in
  [crossing.cpp:45](../../../desktop/src/views/crossing.cpp#L45) and is
  extracted to a shared header so the two consumers cannot drift.
  `Other`/`Unknown` stay visible grey buckets, per the crossing rules.
- A tid-less syscall (v1 writers) exists ONLY here — it is never guessed
  into a thread lane. A tid-ful one appears both here and as a lane tick.
- Rail disabled states, `disabled_reason` never empty
  ([crossing.h:139-144](../../../desktop/src/space/crossing.h#L139) precedent):
  no `syscall` events at all; or `seq_present == false`
  ([syscalls.h:62-67](../../../desktop/src/views/syscalls.h#L62)) — a
  recording predating seq cannot be ordered, and the rail says so instead of
  anchoring everything at x=0.
- Payload bytes are NEVER copied into the model — count only (the
  [crossing.h:121-126](../../../desktop/src/space/crossing.h#L121) redaction
  bar, kept structural).

### 3. Address strip — where memory activity lands

- Y is a stack of **region bands**: the same region list the 3D weave
  assembles (codeimage regions → `observed_data_spans` →
  `vmmap_apply_names`, [shell.cpp:1345-1385](../../../desktop/src/ui/shell.cpp#L1345)),
  passed in by the caller so the strip and the 3D pane can never disagree
  about regions. Each band maps its own address range linearly; inter-region
  gaps are elided with an explicit gap notch (never drawn to scale — a 47-bit
  gap must not own 47 bits of pixels).
- Marks, at high zoom (few seq per pixel): each `mem` event is a dot at
  (its seq, `ea` within its band), read/write hued to match the data-cell
  family's convention; each placed `trace`/`df_step` pc is a small per-tid
  hued mark (`kTidPalette`, shared verbatim with 2D/3D —
  [scene2d_draw.cpp:32-34](../../../desktop/src/views/scene2d_draw.cpp#L32)).
- At low zoom: per pixel column, per band, an **envelope** (min..max
  addr touched that column, split r/w) replaces individual dots. The
  mark↔envelope switch is a deterministic threshold on `seq_per_px`, decided
  in the plan and stated in the plan dump — the doc-65 lesson (bucket in
  pixel space; never one drawable per step).
- **`mem` carries no tid** ([asmtrace_ndjson.c:415-419](../../../cli/asmtrace_ndjson.c#L415)) —
  access marks are r/w-hued, never thread-hued, and the legend states this
  fact verbatim (pinned string, tested). No inference from adjacency.
- Addresses no band maps are COUNTED (`off_band`), surfaced in the HUD line,
  never silently dropped (the `mem_dropped` / `off_plane` precedent).

### 4. Run ribbon + run tint — where one run ends and the next starts

A **run seam** is a full-height vertical boundary; runs alternate a faint
background tint. Seams come from exactly three derivations, each labelled by
its kind (never blurred):

1. **`df_invocation`** events — continuous-capture passes; the seam label
   carries `pass`, `result`, `steps`, `truncated`
   ([streams.cpp:363-373](../../../desktop/src/doc/streams.cpp#L363)).
   A `steps:0` pass renders as armed-and-waiting, not as a verdict.
2. **`coverage`-close** — a `coverage` event closes the `[trace…]` block
   before it, recovered from stream order alone
   ([region.h:11-16](../../../desktop/src/views/region.h#L11)).
3. **Capture seams** — live sessions only: the caller passes the seq offsets
   where one capture's events end and the next begin (it owns the parts —
   see "Live behaviour"). Labelled with the capture ordinal, the existing
   identity convention ([shell.cpp:1408-1412](../../../desktop/src/ui/shell.cpp#L1408)).

`session` lifecycle events are serve-only and never in a `--record` file
(schema §sessions); when the Observer's `ObsLifecycle` is available the
started/stopped facts annotate the capture seam labels, but seams never
*depend* on them.

### Fidelity chrome

- Union torn / missing footer → a torn-edge glyph at the strip's right edge
  (the Loom's `torn_edge` meaning, kept verbatim).
- `truncated` / `drops{lost,throttled}` from the footer → stated in the HUD
  line with counts ("ring dropped N — tail-drop"), because a live ring that
  tail-dropped means the strip's LEFT edge is not step 0 of the run.
- Every channel that is absent says why, in place, verbatim — a quietly
  absent channel is indistinguishable from "nothing happened" (D7).

## Live behaviour

The strip is a per-recording view; the live tab is already an ordinary
recording tab (`shell_sync_live_tab`,
[shell.cpp:227](../../../desktop/src/ui/shell.cpp#L227)), so live support is
mostly free:

- **Substrate:** on the live tab the strip builds over the SAME union
  Recording the 3D pane weaves (`s.live_union` when the union weave is on,
  else the tab's Recording). Capture seams = running `event_count()` offsets
  of `sess.recordings()` parts, computed at the call site that already holds
  them.
- **Rebuild:** on the existing 3-component growth watermark
  ([shell.cpp:283-289](../../../desktop/src/ui/shell.cpp#L283)), alongside
  streams/observers/step indexes. No new change-detection mechanism.
- **Camera carry-over across rebuilds** (the SceneView reset policy,
  [shell.cpp:315-366](../../../desktop/src/ui/shell.cpp#L315)): `seq_per_px`,
  lane scroll, and the follow flag survive; only the model is rebuilt.
- **Follow-tail:** default ON. While following, `seq0` pins the window to the
  growing right edge each frame. Any manual pan/zoom that moves the window
  off the tail disables following; an explicit "follow" affordance (and the
  End key) re-enables it. Deliberately per-view state, not a Settings field —
  it is a reading posture, not a preference.

## Architecture

The Loom split, exactly ([fabric_plan.h:1-16](../../../desktop/src/loom/fabric_plan.h#L1)):

| Unit | File | Depends on |
|---|---|---|
| **Model** `strip_build(...)` → `StripModel` | `desktop/src/views/strip.h/.cpp` | doc/, space/ types, views/syscall classify |
| **Plan** `strip_plan(model, cam)` → `std::vector<strip_prim_t>` + `strip_plan_dump` | same TU | model + camera only |
| **Camera math** `strip_view_t` + pure pan/zoom/window/lane helpers | same header | nothing |
| **Painter** `draw_strip(...)` walks prims → ImDrawList; hover = prim-rect test | `desktop/src/views/strip_draw.cpp` | ImGui + plan |
| **Classify (extraction)** `syscall_class_of(line)` / `syscall_outcome_of(line)` | `desktop/src/views/syscall_classify.h` | space/crossing.h enums |

- `strip_build(const Recording &, const Streams &, const std::vector<space::Region> &, const std::vector<StripSeam> &capture_seams)` —
  regions and capture seams are INPUTS, so the model stays engine-free and
  session-free and the whole closure tests headlessly (D4).
- `strip_plan` is deterministic: same (model, camera) → byte-identical prim
  vector; `strip_plan_dump` is the golden surface (the
  `loom_plan_dump` precedent).
- Prims carry `a`/`b` indices back into the model (lane, syscall row, mem
  event, seam) so hit-testing resolves without a parallel map
  ([fabric_plan.h:52-60](../../../desktop/src/loom/fabric_plan.h#L52)).
- Drill-in goes through the deep-link router (`dt_link` + `go`), never a
  direct reach into another view: a rail tick → that syscall row; a mem
  mark → the timeline at that step. Others hover-only in this cut.
- `crossing.cpp` switches to the extracted classify helper in the same
  change (one parse, two consumers, zero drift).

## Registration

New `ViewId::SessionStrip` in the recording tab bar
([view_presence.h:36-47](../../../desktop/src/ui/view_presence.h#L36)), label
**"Session strip"**. Present iff the recording carries ANY strip channel — a
`mem`, `syscall`, `trace`, `call`, `watch`, or `df_step` event — else absent
with the verbatim reason
`"recording carries no mem/syscall/trace/call/watch/df_step events"`
(it joins the one "unavailable views (N)" affordance like every other view).
No new dock pane in this cut — the tab bar lives inside the Recording pane,
which the docked shell already hosts; a dedicated pane can come later the way
the Loom's did.

In prose and UI copy, "session strip" (whole-session composite view) is kept
distinct from the timeline's "overview strip" (doc 65's per-recording density
widget); the two never share an identifier.

## Testing

The desktop harness idiom — hand-rolled `check(what, cond, why)`, standalone
`main()`, no gtest; register both binaries in `DESKTOP_TESTS`
([mk/desktop.mk:1214](../../../mk/desktop.mk#L1214)); `make desktop-test`
locally, `make docker-desktop` as the authoritative lane.

1. **`test_strip_model.cpp`** (mimics
   [test_scene2d.cpp](../../../desktop/test/test_scene2d.cpp) — no ImGui):
   golden-fixture builds asserting lane discovery + ordering (incl. the
   no-tid single-stream lane and the >1-tgid grouping), seam derivation from
   `df_invocation` / coverage-close / caller seams, rail disabled reasons
   (no syscalls; `seq_present == false`), `off_band` counting, the pinned
   axis/legend strings verbatim, mark↔envelope thresholds, camera math
   (window set/get round-trip, lane scroll clamp, follow-tail pinning), and
   plan determinism (two identical builds → byte-identical `strip_plan_dump`).
2. **`test_strip_draw.cpp`** (mimics
   [test_loom_draw.cpp](../../../desktop/test/test_loom_draw.cpp)): headless
   ImGui context; painter smoke over a synthetic plan containing EVERY prim
   kind × three cameras (geometry oracle: `GetDrawData()->TotalVtxCount > 0`
   — never a LogToClipboard text oracle, which cannot see geometry); then the
   real view over golden recordings.
3. **Presence rows** in the existing `test_view_presence.cpp` for present /
   absent-with-reason.
4. **Classify extraction**: crossing's existing tests keep passing unchanged
   (the parse moved; behaviour didn't).

## Out of scope (recorded so they are decisions, not omissions)

- **No wire/schema change.** `mem.tid`, structured syscalls (nr/args/ret),
  and thread-lifecycle events (fork/clone/exec/exit — traced by the engine
  but never emitted, [asmspy_engine.c:2781-2824](../../../cli/asmspy_engine.c#L2781))
  would each enrich this scene; each is producer work with its own doc.
- **No dedicated dock pane**, no perspectives entry — tab view only.
- **No syscall payload rendering** anywhere in the strip (count only).
- **No wall-clock anything.** The recording does not carry it; the strip
  never implies it.
