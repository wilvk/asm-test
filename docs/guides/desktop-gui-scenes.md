# The desktop GUI and its 3D scenes

The desktop app replays an `.asmtrace` recording. Most of its views are flat —
a summary, a canvas, a timeline, a register scrubber. This page is mostly about
the one that is not: the **3D overview**, which draws a recording as geometry.

Every image here was produced by `make gui-shots` from a live capture of a
sample program in this repository. Nothing is mocked up, and the section at the
end shows how to reproduce all of it.

## Two time axes, deliberately not fused

The single most common misreading of these images is that they share one clock.
They do not, and the app says so on the plane's own axis label:

> NOT one clock: the terrain playhead (trace-residency time) and a worldline's
> step axis are separate, and neither is the flat views' execution step

The terrain's playhead walks **trace-residency time**. A worldline's height is
its **own trace step**. The flat views have a third axis, **execution step**.
Fusing them would be convenient and wrong, so the app keeps them apart.

## The workspace

![The asmtest desktop workspace](../_static/gui/01-workspace.png)

A persistent task rail on the left, the open recording in the centre, the
Inspector on the right, and the session log below. The view tabs are
data-driven: a recording only offers the views it can actually fill, and the
ones it cannot collapse into a single **"unavailable views (N)"** affordance
that still names each absent view *and the machine reason it is absent*. That is
the app's habit throughout — an absence is stated, never hidden.

![The canvas view](../_static/gui/02-canvas.png)

## The 3D HUD

![The 3D overview HUD](../_static/gui/03-scene-hud.png)

The HUD is where a scene declares what it is. From the top: which **substrate**
is being drawn, what its **axes mean** (including what they are *not*), the
**provenance** chips grading the evidence, the **layer** toggles grouped by what
kind of claim they make, and the **encodings** legend.

Two things in it are worth calling out, because they are the reason the rest of
the page can be trusted. Layers grade *evidence* — what was recorded and how
well — while filters choose *subject*. And a layer that cannot draw something
says why, in place, rather than drawing nothing: *"this recording carries no
`blame` attributions, so there are no cones to overlap, and this layer will not
derive any."*

In the shots below the HUD is collapsed so the geometry is the subject.

## The address plane

The default substrate. X and Z are the **address space**, laid out along a
Hilbert curve so that addresses close in memory stay close on screen. Height is
access density. A trajectory is one execution path threading across it.

Verbatim, from the app:

| Axis | Means |
|---|---|
| X | `address (Hilbert plane, u)` |
| Y | `terrain height = access density; a worldline's height = its own trace step` |
| Z | `address (Hilbert plane, v)` |

![The bare address plane](../_static/gui/10-plane-bare.png)

Layers compose **on this plane only**, never across substrates — two meanings on
one screen position is exactly what the layer registry exists to prevent.

### Zoning and contours

![Zoning and contours](../_static/gui/11-plane-zoning.png)

`zoning` hues the terrain by what *kind* of region an address is in — code,
stack, heap, data, mmap, unknown. `contours` adds iso-density bands, which are a
re-encoding of height rather than a new claim.

### Canopy and opcode

![Module canopy and opcode terrain](../_static/gui/12-plane-canopy.png)

`canopy` answers "which library is hot as a whole"; `opcode` classifies what
kind of work happens in each cell.

### Access and relief

![Access marks and read/write relief](../_static/gui/13-plane-access.png)

`access` draws a spur from a PC vertex to the data cell it touched. `relief`
splits that into read and write twins.

### Weather and ghost fog

![Fidelity weather and ghost fog](../_static/gui/14-plane-weather.png)

These two are **fidelity** layers: they encode how much to trust what you are
seeing. `weather` is the fidelity sky; `ghost fog` is the separate stippled
statistical terrain, kept visually distinct from exact evidence so sampled
residency can never be mistaken for a recorded path.

## Scenes whose axes are not addresses

Four questions have a load-bearing axis that is **not** an address. Forcing them
onto the plane would fabricate a correspondence the recording does not carry, so
each gets its own substrate. **These do not compose** — the pane shows one at a
time, and never two together.

### Invocation stack

![The invocation stack](../_static/gui/20-invocation.png)

One routine's control flow *across its calls*. X and Z stay the Hilbert plane;
Y becomes a discrete call ordinal.

> Y: `invocation # — a discrete call ordinal`
>
> NOT a time and never scrubbed: the gap between two invocations is unobserved,
> of unknown length

Filled by `trace-blend.asmtrace`. Each `coverage` event closes one invocation, so
asking for 12 gives 12 slabs.

### Module excursion ribbon

![The module excursion ribbon](../_static/gui/21-module-ribbon.png)

Which thread is in which library, when. One sheet per thread.

> X: `call order (the recording's own event seq)`
> Y: `call depth (the engine's effective, focus-rebased depth)`
> Z: `thread lane — one sheet per tid`
>
> NOT either playhead and not a magnitude: depth is a stack property, and a
> capped floor is a clipped edge, not a bottom

Filled by `tree.asmtrace`. Unresolved modules keep their own hue and are never
merged into a resolved one.

### SIMD lane prism

![The SIMD lane prism](../_static/gui/22-lane-prism.png)

Inside one wide vector register, over its writes.

> X: `byte / element index inside the register`
> Y: `byte magnitude (0..255)`
> Z: `stacked writes, oldest nearest`
>
> NOT wall clock and NOT an address: there is no address inside a register, and
> Z counts recorded writes, not elapsed time

Filled by `df-a.asmtrace`. One caveat the scene states itself: **the recording
does not carry SIMD element width**, so the lane width is derived from the
*mnemonic* — `paddd` is dword lanes, `paddb` is byte lanes. A mnemonic the
classifier cannot name renders at a default width and says so.

### Divergence worldline

![The divergence worldline](../_static/gui/23-divergence.png)

Two recordings, one shared prefix, then a fork.

> X: `recording side — A on the left, B on the right`
> Y: `execution step`
> Z: `register-class lane of a rib (GPR / vector / flags / PC / other)`
>
> NOT a proof of instruction correspondence past the fork: after the streams
> part, A's step n and B's step n need not be the same instruction

Needs **two** recordings of the same code run on different data. The two sides
here differ only in the `--seed` passed to the sample program, which fixes a
runtime variable that selects a branch inside `blend_tile`. Both runs execute
the same binary and share an identical prefix; the branch is where they part.

Making that selection at *compile* time would break the scene rather than
sharpen it: the two sides would then be different code, and the pair would be
refused rather than drawn.

This scene is also the one that needs the most from a recording — three streams
at once, which is why the capture asks for `insns` (see below).

A rib's thickness is how many registers disagree at that step. A **hollow** rib
means at least one side had no computed delta there, so agreement was never
observed — drawn at a minimum width rather than zero, because a zero-width rib
would render "we did not look" as "they agreed".

## Reproducing these

### 1. Set up the host

Attaching to a process you did not launch needs permissions a hardened desktop
Linux does not grant by default. See
[Host setup for tracing](../getting-started/host-setup.md).

On the reference host these images were made on, two gates were measured:

- `kernel.yama.ptrace_scope` was `1`, the Ubuntu default. The sample program
  calls `prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, …)`, which is what lets the
  attach succeed *without* changing the host at all.
- `kernel.perf_event_paranoid` was `4`, so IBS was unavailable and `--sample` /
  `--auto --sampler=ibs` could not be used. Read yours with
  `sysctl kernel.perf_event_paranoid`.

### 2. Build and capture

```
make cli desktop
make gui-shot-recordings
make gui-shots
```

`gui-shot-recordings` starts the sample program and captures it four times.
`gui-shots` renders the manifest and then checks the images are correctly sized,
non-blank, and all distinct.

### 3. Why four recordings, and why `--serve`

**Four recordings, not one**, and that is structural: `call` events and `df_*`
events come from different engines, and a capture session runs one engine at a
time.

| Recording | Session | Fills |
|---|---|---|
| `tree.asmtrace` | `mode: tree` | Module excursion ribbon |
| `trace-blend.asmtrace` | `mode: trace` over `blend_tile` | Invocation stack |
| `df-a.asmtrace` | `mode: dataflow` over `blend_tile`, continuous | Address plane, lane prism, divergence A |
| `df-b.asmtrace` | the same, against a `--seed 2` process | Divergence B |

They are captured through `asmspy --serve` rather than the headless
`--record=` path, and that is not a stylistic choice. The 3D overview is present
only when a recording carries `codeimage` events to place the address plane, and
`codeimage` is emitted only inside a serve session. Captured the other way, every
3D scene is simply absent — the tab does not appear, and a screenshot silently
photographs whatever else was on screen.

`--fpregs` appears in the dataflow sessions for the **Scrubber's** wide register
deck. The lane prism does not need it: the dataflow producer reads XMM operands
directly.

`insns` is what makes the divergence pair comparable at all. That scene needs
three streams in one recording — `codeimage` so the 3D pane exists, `trace` or
`coverage` to **align** the two sides, and `statediff` for the ribs — and until
`insns` existed no single session produced all three. Only the dataflow engine
emits `statediff` (it is a delta of the per-step register ring) and only the
region engine emitted `trace`, so the scene refused the pair with *"both
recordings need a trace or coverage stream to be aligned"*. `insns` has the
dataflow session also state the instruction order it already single-stepped
through. It is off by default, so existing recordings are unchanged.

### 4. The sample program

`cli/scenes_victim.c` exists so these scenes have something honest to draw. Its
shape is the requirement — each part is there to satisfy one scene's data gate:

| Part | Feeds |
|---|---|
| `blend_tile()` — SSE work on a 16-byte tile, entered constantly | the lane prism's wide register writes, and the invocation stack's repeated entries |
| three worker threads descending through `walk_heap` / `sort_batch` / `mix_math` into libc and libm | the ribbon's thread lanes, call depths and module colours |
| a strided walk over a few hundred KB of heap | the plane's observed data spans |
| `--seed N`, which fixes a runtime variable selecting a branch in `blend_tile` | the divergence fork — a shared prefix, then two parting streams |

The seed changes **runtime data only, never the compiled code**. That is
deliberate: the divergence scene compares two recordings of the same routine, so
a variant that selected its path with a `#define` would make the two sides
different code and get the pair refused instead of drawn.
