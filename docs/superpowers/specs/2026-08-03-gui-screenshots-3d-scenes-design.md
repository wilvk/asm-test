# GUI screenshots, the 3D scenes, and a traceable sample app

**Date:** 2026-08-03
**Status:** design approved, ready for implementation planning

## Goal

Produce a committed set of GUI screenshots for the user-facing docs that shows
every 3D scene kind populated with real data, a documented and repeatable way to
regenerate them, and a sample process whose shape can actually fill those scenes.

Three deliverables:

1. `cli/scenes_victim.c` — a sample process designed so that attaching to it
   yields recordings that populate all five scene kinds.
2. `--shot` mode in `asmtest-desktop` — deterministic offscreen PNG capture of
   the real shell, driven by a JSON manifest.
3. `docs/guides/desktop-gui-scenes.md` — a Sphinx guide page carrying the images
   and the reproduction runbook.

## Measured facts this design rests on

Every item below was verified against the real binaries on 2026-08-03 before the
spec was written, not assumed. They are recorded here because three of them
contradict the obvious guess.

| Fact | How it was measured | Consequence |
|---|---|---|
| The `df_step` operand array is `ops`, **not** `vals` | dumped raw NDJSON from a live capture | any tooling that reads value records must use `ops` |
| Wide XMM value records need **no** `--fpregs` | live `--dataflow` on an SSE routine produced **40** wide reg records, all with `bytes`, across 3 registers (`122`/`123`/`124` = XMM0/1/2) | LanePrism is filled by a plain dataflow attach; `--fpregs` instead adds the XMM deck to `regstate`, which is what the Scrubber's register deck reads |
| `--trace <pid> <sym> N` yields **N** invocations | 12 requested → 672 `trace` + **12** `coverage` events; a `coverage` event closes an invocation (`region.cpp:120-131`) | the Invocation scene's slab count is directly controlled by the `n` argument |
| Offscreen EGL here is **NVIDIA GeForce RTX 3090, GL 3.3.0** | surfaceless-EGL probe | real GPU rendering, no llvmpipe fallback; and no X display is needed to render |
| `kernel.yama.ptrace_scope = 1` | `/proc/sys/kernel/yama/ptrace_scope` | attach to a non-child is denied unless the target opts in via `PR_SET_PTRACER_ANY` |
| `perf_event_paranoid = 4` | `/proc/sys/kernel/perf_event_paranoid` | IBS is unavailable: `--sample` and `--auto --sampler=ibs` cannot be used here |
| No golden recording carries `call` events | enumerated event kinds across all 33 goldens | the ModuleRibbon scene cannot be filled from the existing corpus at all — a live capture is mandatory |

## Component 1 — `cli/scenes_victim.c`

Follows the established victim pattern (`cli/auto_victim.c`): `PR_SET_PTRACER_ANY`
opt-in, pid and symbol addresses printed to stderr, built by the existing `.o`
pattern rule in `mk/cli.mk`, carrying the "the shape IS the requirement" comment
that explains why each element exists.

The program's shape is dictated entirely by the scene availability gates in
`desktop/src/ui/shell.cpp:927-946`. Each element earns its place:

| Element | Scene it feeds | Why exactly this shape |
|---|---|---|
| `blend_tile()` — noinline, SSE on a 16-byte tile (`movdqu` / `paddd` / `pshufd` / `punpck*`) | LanePrism, Invocation | LanePrism selects records where `wide && space == "reg"`; `lane_width_for(disasm, guest)` derives lane width from the **mnemonic**, so the chosen mnemonics must be ones whose element width is nameable, or every write degrades to the default lane width |
| `blend_tile` called repeatedly from a driver whose **entry is arrived at constantly** | Invocation | `obs_region_build` groups `trace` events into invocations closed by `coverage`; a routine entered once yields one slab, which is not a scene |
| 3 worker threads, each descending a different depth into libc/libm (`memcpy`, `qsort`, `sin`) | ModuleRibbon | lanes are tids, Y is call depth, colour is module — the scene needs more than one tid **and** more than one module or it degenerates |
| A strided walk over a few hundred KB of heap | Plane terrain, data-cell / relief layers | the address plane needs observed data spans; `--mem` supplies the resolved effective addresses |
| `--seed N` changing **input data only**, never code | Divergence | the gate requires *matching* `code_sha` / basis / arch and then diffs statediff. A variant that changed a compiled constant would produce a **refusal card**, not a scene — the fork must come from data flowing through byte-identical code |

That last row is the design's sharpest constraint and the easiest to get wrong.

### Compilation note

At `-O0` the SSE intrinsics expand to ~56 steps dominated by `movdqa` traffic
through stack slots, which makes a cluttered prism. `blend_tile` should carry
`__attribute__((optimize("O2")))` (or equivalent) so the captured routine is
tight and legible, while the rest of the file stays at the tree's default flags.

### Sizing

The program must run long enough to be attached to four times, stay deterministic
under `--seed`, and yield the core periodically (`sched_yield`) so it never wedges
a core on a shared box — the same courtesy `auto_victim` and `sample_victim` pay.

## Component 2 — `--shot` mode in `asmtest-desktop`

### Invocation

```
asmtest-desktop --shot <manifest.json> --out <dir>
```

Lands **only** in the full GPL `asmtest-desktop` binary. The permissive
`asmtest-viewer` link line is untouched, so the licence split (README "Two
binaries, one license split", plan D4) is preserved by construction.

### Rendering path

**Surfaceless EGL + FBO + `imgui_impl_opengl3`, with the platform IO driven
manually** — not a hidden GLFW window.

This is a refinement on the originally sketched approach, justified by the
measured fact that surfaceless EGL works here: it needs **no display at all**, so
the same command runs on this host, over ssh, and inside the `docker-desktop`
lane without Xvfb. `desktop/test/test_scene_fbo.cpp` already proves every piece of
this path (surfaceless EGL context, FBO, `glReadPixels`) works in this tree.

ImGui's platform backend is replaced by manually setting `DisplaySize` and
`DeltaTime` each frame — the standard ImGui pattern, and the same shape the
existing null-backend tests already use. The renderer backend stays the real
`imgui_impl_opengl3`, so the pixels are produced by the same code the app ships.

### Manifest

```json
{ "name": "05-lane-prism",
  "open": ["build/shots/rec/df-a.asmtrace"],
  "pane": "scene3d", "scene": "LanePrism",
  "layers": ["terrain", "exact"],
  "size": [1600, 1000], "warmup": 30 }
```

The manifest lives at `desktop/shots.json` — it drives a build target and is
code-adjacent, so it sits with the code it drives rather than with the images it
produces.

A manifest rather than CLI flags because ~14 shots then cost one process and one
GL context, and because adding or reordering a shot becomes a data edit rather
than a code change. Scene names bind to `SceneKind`; layer names bind to the
stable `LayerDesc::id` keys already defined in `desktop/src/scene3d/layers.cpp`
(`terrain`, `zoning`, `exact`, `access`, `canopy`, `opcode`, `taint`, `blame`,
`sediment`, …), which are documented as never renumbered.

`open` is an array because the Divergence scene needs two recordings: the first
entry is the A side, the second is attached as the B side (the same role the `d`
key fills interactively). Every other shot lists exactly one.

### Determinism

Committed images must not churn on unrelated changes:

- fixed window size per shot; fixed `warmup` frame count before capture, because
  ImGui docking and layout settle over several frames;
- camera from `standalone_default_camera(kind)` for standalone scenes, and a
  pinned camera for the Plane;
- text scale 1.0, dark theme, fixed font atlas;
- **the workspace and settings stores redirected to a temp directory**, so a
  developer's persisted dock layout, recents, or text scale can never leak into a
  committed screenshot. Without this the images depend on whoever ran the target.

### Provenance sidecar

Each run writes a sidecar recording the GL vendor/renderer/version string, the
manifest digest, and the recordings used. Two people comparing images should be
able to tell whether they are comparing two renderers — the same fidelity
discipline the rest of the GUI applies to its own claims.

## Component 3 — the recordings

`make gui-shot-recordings` launches `scenes_victim`, attaches `asmspy` four
times, and writes into `build/shots/rec/`.

**Four recordings are structurally necessary, not a convenience.** `call` events
and `df_*` events come from different engines, `--serve` runs one engine at a
time, and `Session::done_` is a vector of separate `Recording`s — so no single
recording can satisfy every gate. The corpus check above confirms this cannot be
worked around with existing fixtures either.

| File | Command | Feeds |
|---|---|---|
| `tree.asmtrace` | `--tree <pid> 400` | ModuleRibbon |
| `trace-blend.asmtrace` | `--trace <pid> blend_tile 12` | Invocation (12 slabs) |
| `df-a.asmtrace` | `--dataflow <pid> blend_tile --steps --mem --fpregs --statediff --continuous` | Plane, LanePrism, Divergence side A, Scrubber |
| `df-b.asmtrace` | same, against a `--seed 2` process | Divergence side B |

`--fpregs` appears in the dataflow commands for the **Scrubber** tour shot (the
wide register deck), not for LanePrism, which the measurements showed does not
need it.

The exact layer availability per recording (particularly which Plane layers light
up without a `codeimage` stream) is to be confirmed during implementation and
absorbed as manifest edits — a data change, not an architectural one.

## Component 4 — `docs/guides/desktop-gui-scenes.md`

Wired into the `docs/index.md` toctree; images committed under
`docs/_static/gui/`. Built by `make docker-docs`.

Structure:

1. What the 3D pane is, and the two-axes rule stated up front (exec-step time and
   terrain trace-time are deliberately **not** fused).
2. One section per scene kind: the image, what its axes **mean** — quoted from
   `scene_axes()`, including each kind's required `y_not` (what the vertical axis
   is *not*) — and the exact recording that fills it.
3. The Plane's layer stack.
4. The short GUI tour: entry rail, Summary, Timeline, Scrubber, Loom.
5. **Reproducing this on your own machine.**

### Framing decision for host-specific values

The runbook states the two gates as **measured on a reference host, alongside the
general rule**, rather than either bare numbers or a vague generality:

- `kernel.yama.ptrace_scope` — 1 here, and the default on Ubuntu. Attach to a
  non-child is denied. This is why `PR_SET_PTRACER_ANY` in the sample app is
  load-bearing rather than decorative: without it the denial reads like a tracer
  bug rather than a policy.
- `perf_event_paranoid` — 4 here. IBS is unavailable, so `--sample` and
  `--auto --sampler=ibs` cannot be used; name symbols explicitly or use
  `--sampler=sw`.

Both are stated with the command to read them and the command to change them, so
a reader on a different host can tell whether the advice applies.

## Testing

- `desktop/test/test_shot_manifest.cpp`, riding `desktop-test`: manifest parsing,
  plus an **exhaustive** walk asserting every `SceneKind` is covered by at least
  one shot. A new scene kind that nobody screenshotted therefore fails the build
  — the same anti-drift discipline `scene_axes()` already enforces for axis
  labels via a `default:`-less switch.
- A per-PNG non-blank and expected-dimension assertion in the shot lane. This
  catches the failure that would otherwise ship silently: the GL context dies and
  14 black rectangles get committed as documentation.
- The recordings target asserts each capture is non-empty and carries the event
  kind its scene needs, so a producer regression surfaces as a failed capture
  rather than an empty scene in a screenshot.

## Out of scope

- Screenshots of the Author/emulator doors, live-capture flows, or ARM32/RISC-V
  author mode.
- Any change to scene rendering, layer models, or the recorder. This work only
  *drives* existing surfaces; if a scene looks wrong, that is a finding to report,
  not a thing to fix here.
- Animated captures or video.
- Wiring the shot lane into CI as a gate. The images are committed artifacts;
  regenerating them stays a deliberate `make` invocation.

## File manifest

| Path | Change |
|---|---|
| `cli/scenes_victim.c` | new — the sample process |
| `mk/cli.mk` | new build rule, following the `auto_victim` pattern |
| `desktop/src/ui/shot.cpp` / `.h` | new — manifest model, EGL/FBO capture, PNG write |
| `desktop/src/main.cpp` | `--shot` / `--out` argument handling |
| `mk/desktop.mk` | `gui-shots` + `gui-shot-recordings` targets, stb fetch wiring |
| `scripts/fetch-stb.sh` | new — pinned single-header fetch, mirroring the 25 existing `fetch-*.sh` |
| `scripts/third-party-digests.txt` | new pinned `git-commit` line for stb |
| `desktop/test/test_shot_manifest.cpp` | new — manifest + scene-kind exhaustiveness |
| `desktop/shots.json` | the shot manifest |
| `docs/guides/desktop-gui-scenes.md` | new guide page |
| `docs/_static/gui/*.png` | ~14 committed images |
| `docs/index.md` | toctree entry |

## Risks

- **Plane layer coverage.** Which Plane layers populate from a non-JIT recording
  is unconfirmed; resolved by manifest edits during implementation.
- **Image churn.** Committed PNGs will re-diff whenever the UI legitimately
  changes. Accepted: that is what makes them documentation rather than decoration,
  and the determinism measures above keep the churn attributable.
- **GPU dependence.** Images generated here come from an NVIDIA RTX 3090. The
  provenance sidecar records this so a mismatch is visible rather than puzzling.
