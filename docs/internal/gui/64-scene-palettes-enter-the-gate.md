# The scene's categorical palettes enter the CVD gate — implementation

> **Source.** Finding **#1** of the 2026-08-07 GUI/asmspy visualisation review, cut
> per [62-encoding-integrity-roadmap.md](62-encoding-integrity-roadmap.md) §3. **The
> only accessibility defect in that review** — the other nine are legibility.
>
> This brief extends the gate [24](../archive/gui/24-one-visual-language.md) T2 built
> ([test_cvd.cpp](../../../desktop/test/test_cvd.cpp), `dt_cvd_distance`) to the two
> categorical palettes the 3D family grew afterwards. It **adds no new metric and no
> new rule** — F15 already binds; this is F15 reaching the scene.
>
> Read [\_conventions.md](../implementations/_conventions.md) first; D1–D11 live in
> this directory's [README](README.md). **Prerequisites: none.** No engine, wire or
> schema change. T3 touches GLSL.
>
> Authored 2026-08-07 against `35ef821f`, every citation verified; re-checked at
> `3e2e8cea`, which touches no file cited here. If a cited `file:line` disagrees
> with the code, the code wins — re-verify, then fix this doc in the same change.
>
> **Status — ☐ 0/4.**

## Why this work exists

[test_cvd.cpp](../../../desktop/test/test_cvd.cpp) simulates protanopia,
deuteranopia and tritanopia over every `categorical` row of **one** table —
`dt_encoding_table` ([legend.cpp:37-52](../../../desktop/src/ui/legend.cpp#L37)),
thirteen rows — and requires each pair to be either separable (distance ≥ 0.15) or
covered by distinct second channels. That gate is correct and it works.

The app's **largest** categorical axis is not in it. The 3D scene carries 8
opcode-class hues and 6 region-kind hues, each duplicated between GLSL and C++:

| Axis | GLSL | C++ mirror |
|---|---|---|
| opcode class (8) | [`opClassHue[8]`, embedded.h:105-113](../../../desktop/src/scene3d/shaders/embedded.h#L105) | [`opcode_class_swatches()`, hud.cpp:250-269](../../../desktop/src/scene3d/hud.cpp#L250) |
| region kind (6) | [`kindHue[6]`, embedded.h:115-121](../../../desktop/src/scene3d/shaders/embedded.h#L115) | [`region_style()`, projection.cpp:633-649](../../../desktop/src/space/projection.cpp#L633) |

Running the repo's own `dt_cvd_distance` over them, against the repo's own 0.15 bar:

| Deficiency | Pair | Distance | Bar |
|---|---|---|---|
| protan | **Heap vs Data** | **0.053** | 0.15 |
| deuter | Heap vs Data | 0.127 | 0.15 |
| deuter | Stack vs Mmap | 0.149 | 0.15 |
| protan | **Logic vs System** | **0.053** | 0.15 |
| deuter | Logic vs System | 0.127 | 0.15 |
| tritan | CompareBranch vs VectorSIMD | 0.127 | 0.15 |
| deuter | Move vs ScalarFloat | 0.149 | 0.15 |

Seven `(pair, deficiency)` failures — three of them on region kind, which is the
**zoning view's entire output**. Heap-vs-Data at 0.053 is effectively one colour.

The second-channel escape hatch does not rescue this, because on the terrain surface
there is no second channel to escape into. `region_style()` carries a `name`, and
that name reaches the *legend* ([hud.cpp:1287-1300](../../../desktop/src/scene3d/hud.cpp#L1287))
and — conditionally — the floor via
[`draw_atlas_labels`](../../../desktop/src/scene3d/hud.cpp#L467), which returns early
for a Hilbert layout or rects too small to read. It never reaches the *cell*. The
opcode-class layer is per-cell R8UI ([embedded.h:80](../../../desktop/src/scene3d/shaders/embedded.h#L80))
and has no label channel at all. **On both layers, hue is the only channel.**

## Correction found while cutting

`opcode_class_swatches()`'s own comment claims a colour reordered there and not in
the shader *"fails test_layers' exhaustiveness check"*
([hud.cpp:251-253](../../../desktop/src/scene3d/hud.cpp#L251)). It does not:
`desktop_test_layers` links only `s3/layers.o`
([mk/desktop.mk:2070-2072](../../../mk/desktop.mk#L2070)) and its exhaustiveness
check is over `SceneLayers` members. The real assertion lives in
[test_shell.cpp:1021-1038](../../../desktop/test/test_shell.cpp#L1021). Fix the
comment in T1's commit.

More importantly, **what those tests assert is exhaustiveness and range, never
separability** — one swatch per enum value ([test_shell.cpp:997-1013](../../../desktop/test/test_shell.cpp#L997))
and each channel within `[0,1]` ([test_projection.cpp:265-279](../../../desktop/test/test_projection.cpp#L265)).
Both already walk every value of their enum, so T2 is a few lines inside loops that
exist.

## What already exists (verified 2026-08-07)

- **The metric.** `dt_cvd_simulate` / `dt_cvd_distance` / `dt_contrast_ratio`
  ([cvd.h](../../../desktop/src/ui/cvd.h), [cvd.cpp](../../../desktop/src/ui/cvd.cpp))
  — pure maths over `ImVec4`, no ImGui context, no engine. **Reuse verbatim.** This
  brief adds no colour science.
- **The 0.15 separability bar and the "separable OR redundant" rule** are already
  written and justified in [test_cvd.cpp](../../../desktop/test/test_cvd.cpp). T2
  applies the same constant; it does not choose a new one.
- **A screen-space hatch idiom already exists in `kTerrainFrag`** for the coverage-
  window bits — *"a screen-space diagonal hatch: cheap, orientation-stable at any
  zoom"*, `fract((gl_FragCoord.x + gl_FragCoord.y) * 0.15)`
  ([embedded.h:192-198](../../../desktop/src/scene3d/shaders/embedded.h#L192)). T3
  reuses this mechanism rather than inventing a pattern generator.
- **The precedence rule is already stated and load-bearing**: fidelity gets the LAST
  word on a pixel, and moving a fidelity branch below a filter branch *"would
  silently launder a fidelity state into merely filtered out"*
  ([embedded.h:145-149](../../../desktop/src/scene3d/shaders/embedded.h#L145)). T3
  sits **above** every fidelity branch and says so.
- **The desaturate-toward-a-0.12-grey-floor rule** ([embedded.h:167-175](../../../desktop/src/scene3d/shaders/embedded.h#L167))
  keeps "filtered out" from reading as the near-black `TF_UNKNOWN` pit. A kind
  pattern must not break that separation either.
- **`space::op_class_name()`** already gives every `OpClass` a word
  ([test_shell.cpp:1035](../../../desktop/test/test_shell.cpp#L1035) uses it), and
  `RegionStyle::name` gives every kind one. Both are the legend text T4 needs.

## Fidelity rules (binding on every task)

1. **Fidelity keeps the last word.** T3's pattern is applied **before** the CHURN /
   STAT / UNKNOWN / confidence-hatch / TORN branches, never after. A torn cell reads
   torn at every kind setting, exactly as it reads torn at every filter setting.
2. **One hatch never means two things.** T3's kind pattern must be distinguishable
   from the confidence-window hatch in **orientation and frequency**, and must be
   suppressed wherever that hatch is drawn. Two patterns that look alike are worse
   than one pattern and a collision.
3. **A conditional channel is not a channel.** `draw_atlas_labels` returns empty for
   a Hilbert layout or small rects. T2 may **not** record the floor label as region
   kind's second channel; only an unconditional per-cell channel counts.
4. **Never respace the hues to pass.** Every hue here is mirrored in two places and
   pinned by golden frames (`test_motif_distinctness`, the golden corpus). The fix
   is the redundant channel, exactly as `test_cvd`'s own header says: *"the test
   passes BECAUSE of the redundant channel, not by pretending the hues alone
   suffice."*
5. **`Unknown` abstains and stays abstaining.** `OpClass::Unknown` and
   `Region::Unknown` are the neutral greys that mean "not classifiable". They get no
   pattern — a pattern would assert a classification.

## Tasks

### T1 — One pairwise report helper, and fix the stale comment (S)

1. Add to [cvd.h](../../../desktop/src/ui/cvd.h) a pure helper that states the rule
   once instead of three times:

   ```
   struct dt_cvd_collision { int a, b; dt_cvd_kind kind; float distance; };
   // Every (i<j, deficiency) pair whose simulated distance is below `bar`.
   std::vector<dt_cvd_collision>
   dt_cvd_collisions(const ImVec4 *cols, int n, float bar = 0.15f);
   ```

   Pure maths over `ImVec4`, same D4 closure as the rest of `cvd.cpp` — no ImGui
   context, no engine, no new dependency.
2. Refactor `test_cvd`'s existing loop (c) to call it, asserting byte-identical
   behaviour on the semantic table. This is a no-op refactor that proves the helper
   matches the rule already in force.
3. Fix `opcode_class_swatches()`'s comment to name **`test_shell`**, not
   `test_layers` (see *Correction* above).

**Done when** `test_cvd` passes unchanged in outcome and `dt_cvd_collisions` returns
empty for the semantic table.

### T2 — Gate both scene palettes where each already links (M)

Do **not** build a new binary or a new central table. Each palette is already walked
exhaustively by a test that links it; put the gate inside those loops so no link line
grows and D4's closure proofs stay intact.

1. **Region kind** → [test_projection.cpp:265-279](../../../desktop/test/test_projection.cpp#L265).
   It already iterates all six kinds and links `region_style` engine-free. Collect
   the six `RegionStyle` colours into an `ImVec4[6]` and assert
   `dt_cvd_collisions(...)` is empty **or** every collision's pair carries distinct
   second-channel tokens. Link `ui/cvd.o` into `desktop_test_projection`
   ([mk/desktop.mk](../../../mk/desktop.mk)) — it is pure maths and adds no engine.
2. **Opcode class** → [test_shell.cpp:1021-1038](../../../desktop/test/test_shell.cpp#L1021).
   Same shape over the eight `OpClassSwatch::rgb` values. `test_shell` already links
   the swatch table.
3. **Both assertions must FAIL on first run** — that is the point. Record the exact
   seven collisions from the table above in the assertion's failure message so the
   next reader sees the measurement, not just a red test. T3 is what turns them
   green; land T2 and T3 **in the same change** so `main` is never red.
4. Extend `dt_encoding` ([legend.h:45-57](../../../desktop/src/ui/legend.h#L45)) with
   the two palettes as categorical rows carrying their new channel tokens, so
   `dt_semantic_legend` states what T3 draws. Keep them behind a `scene` flag if
   adding fourteen rows makes the shared legend unwieldy — the gate is what matters,
   not where the rows are rendered.

**Done when** `make desktop-test` is green with both new assertions live, and
temporarily reverting T3's shader change turns exactly those two assertions red.

### T3 — A per-cell pattern channel on the two scene layers (M)

1. In `kTerrainFrag` ([embedded.h](../../../desktop/src/scene3d/shaders/embedded.h)),
   add a pattern derived from the cell's class index, using the **existing**
   screen-space hatch machinery (`gl_FragCoord`, `fract`, `step`) rather than a new
   texture or a new uniform format — the class index is already in `uKind` / `uOpClass`.
2. Give each class a distinct `(orientation, frequency)` pair. Orientation is
   `dot(gl_FragCoord.xy, dir)` for a per-class `dir`; use at minimum
   horizontal / vertical / 45° / 135° crossed with two frequencies, which covers
   eight classes with room to spare. **Index 0 (`Unknown`) draws no pattern**
   (rule 5).
3. Place the block **above** CHURN/STAT/UNKNOWN/TORN and above the confidence hatch,
   and suppress it entirely when `uConfidence == 1` (rule 2). Put the precedence
   argument in a comment beside it, in the same voice as the existing
   fidelity-last-word comment it sits next to.
4. Modulate `base` by a small factor (~`0.12`) rather than mixing toward a fixed
   colour: a *lightness* ripple is a genuinely second channel and, unlike a hue mix,
   it cannot push a cell toward the 0.12 grey floor that separates "filtered" from
   "fog-of-war" (rule 5 of the shader's own comment,
   [embedded.h:167-175](../../../desktop/src/scene3d/shaders/embedded.h#L167)).
5. Mirror the pattern in the GL-free 2D surface ([`cell_paint`](../../../desktop/src/views/scene2d_draw.cpp),
   doc [52](52-flat-terrain-surface.md) T1, which exists precisely to mirror
   `kTerrainFrag`'s branch chain) so the flat and 3D readings of one recording agree.
   A pattern in only one of the two would re-open the drift 52 T1 closed.

**Done when** T2's assertions pass via the redundant channel (not via changed hues —
diff the hue literals to prove none moved), and `test_motif_distinctness`'s three
frames stay pairwise distinct.

### T4 — Name the pattern in both legends (S)

1. Add the pattern token to each swatch row: `opcode_class_swatches()` and the region
   legend loop ([hud.cpp:1287-1300](../../../desktop/src/scene3d/hud.cpp#L1287)) draw
   the glyph/word beside the colour, exactly as `dt_legend_row` does for the semantic
   axis ([legend.h:22](../../../desktop/src/ui/legend.h#L22)).
2. Take the label text from `space::op_class_name()` and `RegionStyle::name` — never
   a fresh string literal. The relief legend already models this: *"The LABELS are
   not copied at all: they come from `space::relief_shape_label()`, so the legend and
   the model can never state two different rules"*
   ([hud.cpp:271-283](../../../desktop/src/scene3d/hud.cpp#L271)).
3. Extend `test_shell`'s legend assertions to require a **non-empty, pairwise-distinct**
   pattern token per class — the same shape as `test_cvd`'s second-channel check (a).

**Done when** `test_shell` asserts eight distinct opcode tokens and six distinct kind
tokens, and the HUD legend renders each beside its swatch.

## Testing

All headless: `make desktop-test` covers `test_cvd`, `test_projection`, `test_shell`,
`test_datalayers`; the GL lane covers `test_motif_distinctness`. Run through
`make docker-desktop` per [CLAUDE.md](../../../CLAUDE.md)'s tooling rule.

**T3 changes a shader, so golden frames will move.** That is expected and is the one
place in this roadmap where regeneration is correct: regenerate
`desktop/test/golden/` frames in the same change, and **diff the hue literals in
`embedded.h`, `hud.cpp` and `projection.cpp` to prove none of the fourteen changed**
(rule 4). No `.asmtrace` golden and no `tests/golden-asmtrace/` file is touched.

## Out of scope

- **The Loom fabric palette** ([fabric_imgui.cpp:34-42](../../../desktop/src/loom/fabric_imgui.cpp#L34)).
  It is loom-local by design and its second channel (solid / hollow / dashed fill)
  already exists and is already named
  ([legend.h:34-38](../../../desktop/src/ui/legend.h#L34)), so it is not a
  colour-alone distinction. Making it theme-aware is separate work
  ([62](62-encoding-integrity-roadmap.md) §2).
- **`relief_shape_swatches()`** ([hud.cpp:271-296](../../../desktop/src/scene3d/hud.cpp#L271)).
  `ReadOnly` and `ReadWrite` are **byte-identical** `{0.30, 0.72, 0.95}` by explicit
  design — the twin relief's *shape* is the channel, and the comment says so. It
  needs no CVD gate because it makes no hue claim; if T2's table is extended to it,
  it must be entered as one colour with two shape tokens, not as two colours.
- **`terrain_encoding_swatches()`** — four fidelity flags, not peer categories. They
  are already redundantly encoded (rubble gash, sunken pit, dimming) and already
  exhaustively tested.
