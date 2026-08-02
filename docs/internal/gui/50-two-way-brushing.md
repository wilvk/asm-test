# Two-way brushing — light the scene from the flat views, through the address

> **Sources.** Gaps G9–G10 of [46-3d-functional-roadmap.md](46-3d-functional-roadmap.md),
> which cuts this brief, and §4 there (the one fidelity decision this family turns
> on). Closes the item [44-faithful-city-phase-a](../archive/gui/44-faithful-city-phase-a-mvp-terrain-reskin.md)
> deferred by name — *"a later phase may add a real `Selection.step` →
> `TrajPoint.t` resolver and cross-brush where it is verified sound; this brief
> does not"* ([shell.h:91-110](../../../desktop/src/ui/shell.h#L91)). Read
> [_conventions.md](../implementations/_conventions.md) first; D1–D11 live in this
> directory's [README](README.md).
>
> **Prerequisites: none hard.** T2's readout is nicer alongside
> [47](47-scene-inspect-and-pickable-overlays.md)'s `PickHint` and T4 reuses
> [48](48-scene-navigation-and-goto.md)'s `Camera::frame`, but both degrade
> cleanly if this lands first. No producer change, no schema change, no new dep.
>
> Authored 2026-08-02 against HEAD `f110150`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Status — ☐ 0/4, not started.**

## Why this work exists

The find/read split the pane is built on — *"3D to FIND a place, then the flat 2D
views to READ it"* — implies a round trip, and only half of it exists. A pick
navigates out ([shell.cpp:1164-1175](../../../desktop/src/ui/shell.cpp#L1164)).
Nothing comes back. `Selection` ([selection.h:33-60](../../../desktop/src/ui/selection.h#L33))
is the app's canonical brushed entity, shared by every 2D view, and it never
reaches `SceneFrame` ([scene_host.h:37-65](../../../desktop/src/ui/scene_host.h#L37)).
The scene's own `follow_step` free-runs on its own transport and is deliberately
never seeded from the selection.

That deliberate choice was correct. 44 declined to seed it because
`Selection.step` is a dataflow step index ([selection.h:35](../../../desktop/src/ui/selection.h#L35),
[nav.h:62](../../../desktop/src/nav.h#L62)) while `TrajPoint.t` is a per-tid vertex
counter (`p.t = next_t[tid]++`, [trajectory.cpp:99](../../../desktop/src/space/trajectory.cpp#L99)),
and for a multi-tid recording those cannot coincide by construction. Fusing them
would have been a D7 violation.

**But the same conflation is already shipped in the opposite direction.**
`resolve_pick` resolves a picked PC vertex to `link.step = pv.t`
([pick.cpp:136-139](../../../desktop/src/scene3d/pick.cpp#L136)) — a per-tid
ordinal handed to a field documented as a dataflow step index, with no guard, no
tid check and no label. On a single-tid replay this is usually right; on a
multi-tid capture it opens the timeline at an unrelated step. Verify this reading
against the code before acting on it — but if it holds, closing G9 without also
fixing G10 would leave the round trip sound in one direction and quietly wrong in
the other.

The resolution is the same in both directions and it is not a compromise: **brush
through the address, not through an ordinal.** The plane *is* an address plane.
`Selection.off` is a code offset in the recording's basis — the exact key
`resolve_pick` already emits ([pick.cpp:90](../../../desktop/src/scene3d/pick.cpp#L90)) —
so `off → base+off → Projection::project → cell` is exactly invertible and states
nothing the recording did not. Where only a step is in hand, the recording itself
carries the bridge: `DataflowStream::insn_off[step]` with `insn_rbase[step]`
([streams.h:68-76](../../../desktop/src/doc/streams.h#L68)) give that step's
address directly, from the wire.

## What already exists (verified 2026-08-02 against `f110150`)

- **`Selection` is already the shared brushing model** and already
  recording-scoped: `s.selection.rec == a->id` guards projection into a view
  ([shell.cpp](../../../desktop/src/ui/shell.cpp), the timeline model helper), so a
  selection brushed in recording A does not light recording B. The scene must use
  the same guard.
- **`DataflowStream` carries the step→address bridge on the wire.**
  `insn_off[step]` per step, `insn_rbase[step]` per step with `rbase_present`
  stating whether the wire supplied it ([streams.h:68-76](../../../desktop/src/doc/streams.h#L68)) —
  doc 37's contribution, exactly so a bare offset resolves against a real base.
- **`Projection::project(addr, &u, &v)`** returns false for an address no region
  maps ([types.h:44](../../../desktop/src/space/types.h#L44)) — the refusal path is
  already there and must be honoured, not clamped.
- **`resolve_anchor` / `Anchor::place`** ([projection.h](../../../desktop/src/space/projection.h))
  are the existing, tested rel→abs derivation with a stated `reason` on failure.
  Reuse them; do not write a second anchor.
- **The terrain fragment shader already computes its cell from `vUV`** — the pick
  variant does exactly that (`cx = uint(vUV.x * uN)`,
  [embedded.h](../../../desktop/src/scene3d/shaders/embedded.h)) — so a highlight
  branch needs no new attribute or texture, just the same arithmetic and one
  uniform.
- **`SegmentedDataflow` splits a continuous capture into passes** (doc 40), so
  `Streams::df` is one pass. A step index means "step N *of this pass*" — T1's
  resolver must resolve against the pass the selection belongs to, not a global
  index.

## Tasks

### T1 — `scene_locate`: the pure selection→cell resolver, with refusal (M)

**Goal.** One engine-free function that answers "where on the plane is the thing
the flat views have brushed?", or states why it cannot.

**Steps.**
1. New engine-free header (`space/locate.h` + `locate.cpp`, beside `projection`/
   `terrain` — std-lib and `space/`/`doc/` only, no ImGui, no GL, no engine):
   ```
   struct Located {
       bool ok = false;
       uint32_t cell = 0;      // plane cell, valid iff ok
       uint64_t addr = 0;      // the absolute address it resolved through
       std::string how;        // "offset" | "step→offset" — which route was taken
       std::string reason;     // why not, when !ok (verbatim, never empty on !ok)
       bool ambiguous = false; // the address is real but not uniquely the target
   };
   Located scene_locate_off(const Projection &, const Recording &, uint64_t off);
   Located scene_locate_step(const Projection &, const DataflowStream &,
                             uint32_t step);
   ```
2. **The offset route.** `off` is in the recording's basis. For an `abs` basis the
   region base is the codeimage base; for `rel` use the existing `resolve_anchor` /
   `Anchor::place` and carry its `reason` verbatim when it refuses. Then
   `Projection::project`. Never clamp an unmapped address to a neighbouring cell.
3. **The step route.** `insn_off[step]` + `insn_rbase[step]` (when
   `rbase_present`) give the address directly; without an rbase fall back to the
   anchor, with `how` recording which mechanism was used — a wire-stated base and
   a derived anchor are different claims and must not share a label (the same
   distinction `TrajectorySet::anchor_source` already draws,
   [trajectory.h:74-79](../../../desktop/src/space/trajectory.h#L74)).
4. **Bounds and gaps are refusals, not guesses.** `step >= insn_off.size()`, or a
   step the stream did not cover (`steps_missing`), refuses with the reason. A
   step whose offset the anchor cannot place refuses. There is no "nearest step"
   fallback: answering a different question is worse than declining this one.
5. `ambiguous` is set when the resolved address is real but many steps share it (a
   loop body): the *cell* is correct, but "the selection is here" is one of several
   truths. Callers surface it; they do not silence it.

**Tests.** New `desktop/test/test_locate.cpp`: an `abs` offset resolves to the
same cell `Projection::project` gives; a `rel` offset resolves through the anchor
and matches; an unanchorable `rel` offset refuses with `resolve_anchor`'s reason
verbatim; an out-of-range step refuses; a step covered by no `df_step` refuses; a
loop offset sets `ambiguous`. Assert `reason` is non-empty for every `!ok` return
— exhaustively, so a silent refusal cannot ship.

**Done when.** Every route either produces a cell derived from a recorded address
or states a reason; no code path guesses.

### T2 — Light the located cell in the scene (M)

**Goal.** Brushing a step or offset in any 2D view visibly marks where it is on
the plane.

**Steps.**
1. `ui/scene_host.h`, `SceneFrame`: add `bool has_highlight` + `uint32_t
   highlight_cell`. Document it as **the flat views' selection projected through
   its ADDRESS** — not a third clock — so a later reader cannot mistake it for an
   axis.
2. `ui/shell.cpp`, `draw_scene_overview`: run `scene_locate_off` when
   `s.selection.off` is set, else `scene_locate_step` when `s.selection.step` is,
   both guarded by `s.selection.rec == a.id` (the existing cross-recording rule).
   Cache the result keyed on `(selection epoch, weave gen)`; do not re-resolve
   every frame.
3. `scene3d/scene.h`/`.cpp`: a public `int32_t highlight_cell = -1;` (a draw-time
   value like `follow_step`, no upload) and a `uHighlightCell` uniform on
   `kTerrainFrag`. The fragment recomputes its cell from `vUV` exactly as
   `kPickTerrainFrag` already does, and rings the matching cell — an outline or
   rim, **not** a height change and **not** a hue replacement, so the cell's own
   density and kind stay readable underneath. The highlight is a *pointer*, not a
   measurement.
4. When the location refuses, the scene shows no highlight and the HUD states the
   reason verbatim in `dt_warn_col()`. A selection that cannot be placed must read
   as "cannot be placed, because X", never as "not selected".
5. When `ambiguous`, the readout says so — "this offset is executed at several
   steps; the cell is where, not when".

**Tests.** `test_shell.cpp` under the null backend can assert the pure half: given
a `ShellState` with a selection, the `SceneFrame` the pane would build carries the
expected `highlight_cell`, or `has_highlight == false` with a stated reason. Add a
GL smoke assertion only if the existing `test_scene_fbo` can sample the ringed
cell; if it self-skips, say so in the test file.

**Done when.** A brushed step or offset in a 2D view marks a cell in the 3D pane;
an unplaceable selection states why; the highlight never alters the cell's encoded
quantities.

### T3 — Fix the reverse direction's unguarded ordinal (M)

**Goal.** A picked PC vertex opens the flat reader at a step derived the same
honest way, or says it cannot.

**Steps.**
1. **First, verify the defect.** Confirm that `link.step = pv.t`
   ([pick.cpp:136-139](../../../desktop/src/scene3d/pick.cpp#L136)) is reached for
   a multi-tid recording and that `pv.t` there is the per-tid counter from
   [trajectory.cpp:99](../../../desktop/src/space/trajectory.cpp#L99)/`:144`. Write
   the failing case as a test before changing behaviour — if the reading is wrong,
   the test will say so and this task shrinks to a comment.
2. Prefer the **address route**: a picked vertex knows its `addr`; resolve that to
   a step by searching `DataflowStream::insn_off` (plus `insn_rbase`) for a step at
   that address. Exactly one match → open there. Several matches (a loop) → open at
   the first and **label the result as one of N occurrences** rather than
   presenting it as *the* step. No match → open the reader that *can* address it by
   offset (the canvas at `addr - base`) instead of the timeline, rather than
   passing an index the stream cannot honour.
3. Keep the ordinal route **only** where it is provably sound — a single-tid
   recording whose trajectory came from a stream that is 1:1 with the flat views'
   step index — and gate it on that condition explicitly rather than on luck. If
   the condition cannot be established from the model, drop the route; do not keep
   it as a default.
4. Whatever survives, state it in `pick.h`'s contract block, which currently
   documents the vertex case as "the operand TIMELINE at that step"
   ([pick.h:69-70](../../../desktop/src/scene3d/pick.h#L69)) without naming which
   step axis it means.

**Tests.** `test_drillin.cpp`: a two-tid fixture where tid B's third vertex has a
known address asserts the resolved link points at the step whose `insn_off`
matches that address — not at index 3. A single-tid fixture keeps its existing
expected link (no regression). A vertex whose address appears at no df step
resolves to the canvas, not to a fabricated step.

**Done when.** No `dt_link::step` is produced from a per-tid ordinal without a
stated, tested soundness condition.

### T4 — Follow the selection (S)

**Goal.** A located cell that is off-screen can be reached without hunting.

**Steps.**
1. HUD affordance: a "frame the selection" button, enabled only when
   `has_highlight`, calling [48](48-scene-navigation-and-goto.md)'s
   `Camera::frame` on the located cell. If 48 has not landed, this task ships the
   readout only and the button waits — say so in the row rather than adding a
   second camera-mutation path.
2. Optional, off by default: a "follow selection" toggle that reframes on every
   selection change. Off by default because an auto-moving camera fights the
   user's own navigation — the review's #55 rationale, inverted.
3. When the highlight is off-screen and follow is off, show a small directional
   cue at the viewport edge rather than moving the camera. This is a pointer, not
   a navigation.

**Tests.** The pure half: given a camera and a located cell, a helper decides
on-screen vs. off-screen and (for the cue) which edge — golden-testable through
`Camera::mvp` with no GL.

**Done when.** A located selection can be framed in one action, and an off-screen
one is disclosed rather than silently absent.

## Fidelity notes (D7)

- **Address, never ordinal.** Every mapping in this brief goes through a recorded
  address. Two indices that are both called "step" are not the same axis.
- **Refusals are loud and verbatim.** Every `!ok` carries a non-empty reason, and
  the test asserts that exhaustively. An unplaceable selection reads as
  "unplaceable, because X" — never as no selection.
- **Ambiguity is surfaced, not resolved.** An offset executed at many steps yields
  a correct cell and an explicit "this is where, not when". Picking a
  representative silently is the greedy-reconstructor trap in miniature.
- **The highlight adds no quantity.** It rings a cell; it does not raise, recolour
  or resize it. A pointer that changes an encoded value is a lie about the data.
- **The three clocks stay three.** Terrain-residency time (`slice_t`),
  `TrajPoint.t` (`follow_step`) and the flat views' step index remain separately
  named and separately carried. This brief adds a *spatial* link between them, not
  a temporal one — which is precisely why it can be sound where a step-to-step
  fusion could not.

## Effort and risk

Four tasks, three medium and one small. The risk is entirely in T3: it changes
existing behaviour that current tests may pin. Step 1 exists to make that
explicit — write the failing case first, and if the reading is wrong, say so and
shrink the task rather than changing working code to match this doc.
