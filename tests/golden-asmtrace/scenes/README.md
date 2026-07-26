# `tests/golden-asmtrace/scenes/` — 3D-overview scene fixtures (hand-authored)

Like [`../dishonest/`](../dishonest/), [`../views/`](../views/) and
[`../export/`](../export/), and unlike the flat `*.asmtrace` files in the
parent, **nothing here is generated**. `make asmtrace-golden` writes only flat
`*.asmtrace` files into the parent and never descends into a subdirectory, and
`make asmtrace-golden-check` compares only those flat files, so regenerating the
corpus leaves this directory untouched.

The 3D spacetime overview
([docs/internal/gui/10-spacetime-3d-overview.md](../../../docs/internal/gui/10-spacetime-3d-overview.md))
has **three** golden scenes. Two of them are *generated*, live in the parent, and
are listed here only so the set reads as one:

| Scene | Where | What it pins |
|---|---|---|
| `../scene-abs-loop.asmtrace` | parent (**generated**) | the coarse rung's happy path — `codeimage` + **absolute**-basis `trace` from `scene_hot_loop(3)`, whose 3-iteration loop body makes one hot cell over cold ones: coarse terrain + one exact trajectory |
| `../scene-abs-loop-truncated.asmtrace` | parent (**generated**) | the same bytes with the trace buffer capped at 6 of 13 steps, so the producer flips `truncated` and `drops.lost` counts the rest: every touched cell is `TF_TORN` |
| `mem-rich-synthetic.asmtrace` | **here** (hand-authored) | the **rich rung**, which has no producer at all — see below |

The statistical half of the honesty pair (`TF_STAT` isolation) needs no new file:
[`desktop/test/fixtures/obs-survey-ibs.asmtrace`](../../../desktop/test/fixtures/obs-survey-ibs.asmtrace)
is already a committed `ibs-op` `survey` (`exact:false`), and
`desktop/test/test_scene_fbo.cpp` renders it as the third scene.

## `mem-rich-synthetic.asmtrace` — synthetic, and **schema-unfrozen**

Its header, `codeimage` and 13 `trace` events are copied verbatim from
`../scene-abs-loop.asmtrace`; the four `mem` events are **hand-authored and
measure nothing**. `mem` is a **reserved registry row** in
[asmtrace-schema.md](../../../docs/internal/gui/asmtrace-schema.md) with **no v1
producer** — no recorder in this tree can emit this file — so the
`{step,ea,size,rw}` field shape is the one doc 10 *proposes*, and it is **not
frozen**. When the Wave-1 memory stream lands, **replace this file with a
recorded one** rather than freezing the schema around a fixture. Its
`provenance` says so in the data as well as in prose: `backend`
`"synthetic-fixture"`, `trust` `"weak"`.

It is deliberately **inert but present**. Every `ea` is at `0x200000`, outside
every `codeimage` region, so under the coarse-rung projection the shipping view
builds (`regions_from_codeimage` alone) the accesses map to no region and the
terrain gains **no data cell** — the rich rung's gate stays closed, and the
scene renders exactly as its coarse twin does. A caller that supplies the data
region explicitly (as 07's `/proc/pid/maps` snapshot will) opens the path;
`desktop/test/test_drillin.cpp` covers that opt-in side, and
`desktop/test/test_scene_fbo.cpp` asserts the closed gate here.
