# Scene render quality — depth cues, readable height, legible translucency

> **Sources.** §5 of [53-3d-catalog-build-roadmap.md](53-3d-catalog-build-roadmap.md)
> (which cuts this brief), the [3D catalog](../analysis/2026-07-29-3d-visualization-catalog.md)
> §9's occlusion/perf limit — *"several graphs stack many translucent primitives on
> the same footprint … needs depth-sorting, per-layer toggles, and possibly a
> cell-count budget so a dense recording does not become an unreadable haze"* —
> and the [UX/dataviz review](../analysis/2026-07-29-gui-ux-dataviz-review.md)
> #37/#40/#56 (height is not readable as a quantity). Read
> [_conventions.md](../implementations/_conventions.md) first; D1–D11 live in this
> directory's [README](README.md).
>
> **Prerequisites: none.** Every task improves the scene that exists today and
> none reads a model this family adds. No new third-party dep — every technique
> below is implemented in-tree as GLSL beside the existing shaders.
>
> Authored 2026-08-02 against HEAD `b657876`. If a cited file:line disagrees with
> the code when you implement, the code wins — re-verify, then fix this doc in the
> same change.
>
> **Status — ☑ 6/6 tasks landed; T6 step 3 alone remains open, and cannot be
> answered here.** T1–T5 and T6 step 1 + step 2 are all in. Step 3 is the
> GLSL-130-on-Apple-core-profile question, which is verify-first on Darwin and
> this tree has no macOS desktop lane — see the end of this banner.
> T1 (Eye-Dome Lighting), T3 (contour bands), T4 (dithered translucency) and
> T5 (MSAA) are fully landed, each via one internal multi-pass refactor of
> `Scene::render()`: the raw geometry now always draws into Scene's OWN
> offscreen target (multisampled when `msaa_samples >= 2`, defaulting to 0 so
> golden/CLI/headless renders stay bit-exact — the interactive app opts in
> via `GlSceneHost::init()`), resolved to single-sample colour+depth
> TEXTURES, then either composited through the EDL fullscreen pass
> (`kEdlVert`/`kEdlFrag`, 8 depth taps at a screen-space pixel radius) or
> plain-blitted into the framebuffer the caller had bound at entry — the pick
> pass (`fbo_pick_`) is untouched throughout, verified by byte-identical
> pick-buffer assertions with each new pass on and off (`dec808a`). Contour
> bands (doc 49's `kContourLevels`) now use `fwidth()` on the pre-fract
> quantity for a screen-space-constant width and exclude `TF_TORN` cells
> (only `TF_UNKNOWN` was excluded before) — a torn cell's height is a known
> lower bound, not a measurement (`acbf5ae`). The ghost-fog terrain and
> statistical trajectory lines composite via Interleaved Gradient Noise
> discard (Jimenez 2014) instead of order-dependent alpha blend: a surviving
> fragment is fully opaque, so compositing is an ordinary z-test with no
> blend state and no draw-order dependency — the doc's own pre-authorized
> capacity-constrained landing (step 1 depth-write fix + step 4 dithered
> fallback, step 2 WBOIT and its step-3
> extension probe deferred as dead code with no consumer until a future pass
> adds the "exact" branch) (`1ebdb69`).
> **Correction, 2026-08-02:** this banner previously also credited **step 5**
> (the HUD stating the compositing mode) to `1ebdb69`. It was not in that
> commit, nor anywhere on `main` — the code had been written but left
> uncommitted in the shared working tree, so the claim was an over-claim for
> roughly a day. `translucency_mode_note()` (declared in `scene3d/hud.h`,
> drawn unconditionally beside the EDL disclosure in `draw_scene_hud`) landed
> separately, with `test_shell` pinning that the wording both NAMES the mode
> ("dithered") and GRADES it ("approximate") — a non-empty check alone would
> have passed a note like "translucency: on" that defeats the step's whole
> purpose. **T4 is complete only as of that landing.** T6 step 2 (query
> `GL_ALIASED_LINE_WIDTH_RANGE` at `init_gl`) landed alongside (`ada0cb3`);
> step 1 (vertex-shader quad expansion, the actual portable-width fix) and
> step 3 (the GLSL-130-on-Apple-core-profile verify-first question) did not.
>
> **T6 step 1 landed 2026-08-03.** Every line the COLOUR pass draws —
> trajectories, access spurs, convergence arcs, and doc 56's misprediction
> arcs and site columns — is now expanded into per-segment quads in the vertex
> shader (`kLineVert`) and widened in SCREEN PIXELS, so not one
> `glLineWidth(>1.0)` remains on any path a viewer sees. The previous
> session's blocker — that the pick pass REUSES the colour pass's position
> buffer, so changing that buffer's layout breaks the sharing — was resolved
> by *not* changing it: the bare-vec3 line VBOs stay exactly as they were and
> the quad geometry is a SECOND buffer built from the same upload, so the
> pick pass's wide-line draw is untouched, call for call. That route stays
> the default wherever `GL_ALIASED_LINE_WIDTH_RANGE` really covers the 5–6px
> click targets, and `Scene::pick_widening` falls to a quad-expanded pick
> pass (the same `kLineVert`, against `kPickPointFrag`, over the same
> per-mark ids) only where the driver would clamp — the core-profile case
> that would otherwise shrink every arc/spur click target to one pixel. The
> quad pick route is not left untested on the lane that does not need it:
> `Lines`/`Quads` force either route, and `test_scene_fbo` asserts the two
> resolve exactly the same overlay ids and the same width. The CPU half of
> the expansion is a pure header (`scene3d/linequad.h`) so `test_camera`
> covers its maths — including the sign inversion at the far endpoint that
> would otherwise fold each quad into a bowtie — with no GL context at all.
> Byte-identity of the pick buffer across the whole change was checked
> directly, not merely argued: an FNV-1a digest of the entire id buffer over
> a fixture exercising all four pick bands at three camera distances is
> `de7e5750c73fcbc9` both at the merge base and after (llvmpipe, EGL
> surfaceless).
>
> **T2 landed 2026-08-03.** Depth-dependent halos, on the quad expansion T6
> step 1 put in place. The construction departs from the doc's own step 1 in
> one way, and the reason is worth recording: a halo drawn as a SECOND, wider
> primitive behind the line self-occludes on a tessellated polyline — a nearer
> segment's halo wins the depth test against a neighbouring segment's core and
> eats its own line (measured: a 16-segment convergence arc lost 78% of
> itself). So the halo is instead the OUTER RING of each line's own quad: one
> primitive, no depth relationship to get wrong, and a *different* mark still
> cut by the ordinary z-test, which is the effect the paper is for. That also
> removes the depth-offset pass the doc's step 1 called for. Two further
> deliberate departures, both because this plane is not the paper's empty
> background but the terrain, whose brightness is a measurement: the ring
> washes toward the background rather than replacing it (the cut comes from
> the ring's DEPTH write, not its colour), and its colour is a fixed constant
> that must never follow the fidelity weather sky, or a depth cue would start
> encoding fidelity. Depth-cued width attenuation (step 3) rides the same
> `SceneLayers::halos` toggle and is floored at `kMinLineWidthPx`. A
> STATISTICAL line's ring inherits the line's own screen-space IGN stipple
> mask by construction — the `discard` is evaluated before the ring branch —
> so its gaps stay gaps rather than being filled with an opaque band that
> would launder a survey into an exact path.
>
> Two things this task turned up that were not defects in it:
> - **`noperspective` is load-bearing.** `vEdgePx`/`vCoreHalfPx` carry
>   screen-space pixel distances; with the default perspective-correct
>   interpolation a foreshortened segment reports a fragment well outside the
>   core as inside it, and ring pixels paint as line.
> - **A golden-scene brightness check was sampling the wrong thing.** "the
>   loop body renders brighter" read whichever pixel of the cell came first in
>   the pick buffer, which under the top-down preset is often a pixel the
>   worldline itself covers. A latent flaw (a line has always been drawn over
>   the terrain) that the wider mark made fire; it now measures a
>   terrain-only frame, which is what the check is named for.
>
> **What's still open, and why.** T6 **step 3** (the GLSL-130-on-Apple-core-profile question)
> remains open and deliberately untouched: it is a verify-first item, this
> tree still has no macOS desktop lane, and nothing in this session could
> answer it — changing the version directive on the strength of the document
> alone is exactly what the task forbids.
> Verified throughout via targeted docker builds of
> `test_scene_fbo`/`test_camera`/`test_goto`/`test_layers` plus the full
> `docker-desktop` lane; earlier sessions saw that lane red on unrelated,
> independently-in-flight failures elsewhere in this shared tree
> (`test_author_vm`, then a RISC-V Capstone API mismatch in
> `dataflow_operands.c` — both doc 60, neither touched here).

## Why this work exists

The scene draws unlit geometry: a height-mapped grid with a colour ramp, and line
strips. There is no lighting model, no shadow, no ambient occlusion and no
anti-aliasing. That was the right call for the MVP — the encodings are the
message, not the pixels — but it has two consequences that are now blocking.

**Depth is unreadable.** A worldline crossing in front of another and a worldline
crossing behind it look identical, because nothing shades either. The scene's
entire contract is *"3D to FIND a place"* — and a viewer who cannot tell which of
two marks is nearer cannot find anything the flat views would not have shown
faster. The catalog is about to make this much worse: it adds spurs, arcs,
ribbons, pillars, columns and braids to the same frame.

**Height is a gradient, not a quantity.** `kTerrainFrag` mixes a base colour
toward a kind hue by `clamp(vHeight,0,1)`
([embedded.h:71](../../../desktop/src/scene3d/shaders/embedded.h#L71)) — so a cell
twice as hot is *some amount brighter*, with no interval, no reference and no key.
That is the review's #37/#40/#56 and [46](46-3d-functional-roadmap.md)'s G8.

Both are solvable with well-established, cheap, screen-space or object-space
techniques that clear this app's narrow GL baseline. This brief takes the four
that do, plus the two portability defects the survey turned up.

## The baseline, and what it admits (verified 2026-08-02)

- **Non-Apple: GL 3.0, GLSL 130.** `glfwWindowHint(CONTEXT_VERSION 3,0)`, no
  profile hint ([main.cpp:121-127](../../../desktop/src/main.cpp#L121)).
- **Apple: GL 3.2 core + forward-compatible, ImGui at `#version 150`**
  ([main.cpp:110-120](../../../desktop/src/main.cpp#L110)).
- **All eight scene shaders are `#version 130`, unconditionally**
  ([embedded.h](../../../desktop/src/scene3d/shaders/embedded.h)), with the
  header's own note explaining that 130 was chosen to match the Linux 3.0 context.
- **No GL loader.** `GL_GLEXT_PROTOTYPES` + `libGL` on Linux, the OpenGL framework
  on Darwin ([scene.cpp:1-24](../../../desktop/src/scene3d/scene.cpp#L1)). Anything
  beyond the core set needs both a runtime probe *and* an entry-point route.
- **No MSAA.** `GLFW_SAMPLES` appears nowhere in the tree.
- **Blending is fixed-function and order-dependent.** The ghost-fog terrain draws
  blended with depth-write off ([scene.cpp:634-639](../../../desktop/src/scene3d/scene.cpp#L634));
  the trajectory pass enables blending and leaves depth-write **on**
  ([scene.cpp:655-656](../../../desktop/src/scene3d/scene.cpp#L655)).
- ~~**Wide lines are used**: `glLineWidth(2.0f)` for trajectories, `1.0f` for
  spurs, `3.0f` for convergence arcs.~~ **Closed by T6 step 1 (2026-08-03):**
  the colour pass calls `glLineWidth` nowhere; widths are quad geometry in
  pixels (`kLineVert`). The only calls left are the pick pass's own
  click-target route, which the driver's queried
  `GL_ALIASED_LINE_WIDTH_RANGE` selects — see T6.
- **The pick pass is a separate FBO render** (`render_pick_into_fbo`,
  [scene.cpp:761-795](../../../desktop/src/scene3d/scene.cpp#L761)) with blending
  disabled — so **no task here may alter it**. A post-process that changed pick
  ids would break every drill-in in the app.

What that admits, and what it does not:

| Needed by | Core 3.0? | Notes |
|---|---|---|
| Depth texture attachment + fullscreen pass (T1) | ✅ | `GL_DEPTH_COMPONENT24` texture, FBO — already the pick-FBO shape |
| `dFdx`/`fwidth` (T3) | ✅ | core in desktop GLSL since 1.10 |
| MSAA (T5) | ✅ | window hint; multisample renderbuffer + blit for the offscreen path |
| MRT + float colour targets (T4) | ✅ | `GL_RGBA16F`, `glDrawBuffers` |
| **Per-target blend funcs** `glBlendFunci` (T4) | ❌ | GL 4.0 / `ARB_draw_buffers_blend` — **this is the one gate**, hence T4's fallback |
| Instancing (`glVertexAttribDivisor`) | ❌ | GL 3.3; out of scope here, already flagged by [43](43-faithful-city-roadmap.md) §3 |

## Tasks

### T1 — Eye-Dome Lighting: make depth readable on unlit geometry (M)

**Goal.** Which mark is in front becomes obvious without shading, lights or
normals.

**Why this technique.** Eye-Dome Lighting (Boucheny; the technique ParaView,
Potree, CloudCompare and ArcGIS Pro all use for point clouds) darkens a pixel in
proportion to how much *nearer* its screen-space neighbours are. It needs only the
depth buffer and one fullscreen pass — no normals, no geometry pass, no G-buffer.
It exists precisely for data that has no surface to light, which is exactly what a
worldline, a spur, an arc and a scatter of PC vertices are. SSAO/HBAO solve a
related problem but need 16–64 samples, a noise texture and a bilateral blur to
not look noisy; EDL is a handful of taps and is *designed* for this input.

**Steps.**
1. `scene.cpp`: give the scene an offscreen colour+**depth-texture** FBO. The
   pick FBO already establishes the pattern (`ensure_pick_fbo`,
   [scene.h:200](../../../desktop/src/scene3d/scene.h#L200)) — clone its shape,
   with `GL_DEPTH_COMPONENT24` as a **texture** rather than the pick FBO's
   renderbuffer, because the shader must sample it.
2. `shaders/embedded.h`: add `kEdlVert` (the NDC quad — reuse `kSkyVert`'s shape)
   and `kEdlFrag`: for each of 4–8 neighbours at a screen-space radius, compute
   `max(0, log2(z_center) - log2(z_neighbour))`, average, and multiply the colour
   by `exp(-strength * response)`. Sample the **linearised** eye-space depth, not
   the raw non-linear buffer value, or the effect will be all cliff at the near
   plane and nothing at the far.
3. Two uniforms, both HUD-exposed: `uEdlStrength` (default a value chosen by
   looking at a golden scene, stated in the header) and `uEdlRadius` in **pixels**
   — a screen-space radius is what keeps the effect stable as the camera dollies.
4. `SceneLayers`: add `bool edl = true`. Off means the pass does not run — never a
   pass that runs and does nothing.
5. **The pick pass is untouched.** EDL runs on the colour path only. Assert it.

**Fidelity.** EDL is a *depth cue*, not an encoding: it must never change the
hue or the height a viewer reads a quantity from, and its strength must not vary
per layer (a darker cell would then mean two things). Keep it a uniform
multiplier on luminance, applied after all colour is decided, and say so in the
shader comment.

**Tests.** `test_scene_fbo.cpp`: with EDL on, a pixel at a large depth
discontinuity is strictly darker than the same pixel with EDL off, and a pixel in
a flat region is unchanged within tolerance; the pick buffer is byte-identical
with EDL on and off (the anti-regression assertion that matters most here).

**Done when.** Depth ordering is readable in the golden scenes and the pick buffer
is provably unaffected.

### T2 — Depth-dependent halos for dense line data (M)

**Goal.** Line crossings read correctly, and bundles read as bundles, when the
scene carries far more lines than it does today.

**Why this technique.** Everts, Bekker, Roerdink & Isenberg, *Depth-Dependent
Halos: Illustrative Rendering of Dense Line Data* (IEEE TVCG 15(6), 2009; best
paper at IEEE Vis 2009) is the published answer to this exact input — dense 3D
line sets, originally DTI fibre tracts. Each line is drawn with a slightly wider,
depth-offset background-coloured halo, so a nearer line visibly *cuts* the ones
behind it; the paper pairs it with depth-cued width attenuation. It is
object-space, needs no extension, and its whole point is that it emphasises tight
bundles while de-emphasising unstructured ones — which is the trajectory pane's
job description.

**Steps.**
1. ✅ ~~Draw each line set twice: first a halo pass in the background/sky colour
   at greater width with a small depth offset away from the viewer, then the
   line itself. Order matters — halo, then line, per line set, so a set does not
   halo itself.~~ **Revised in implementation (2026-08-03), and the revision is
   the point.** A separate wider primitive behind the line DOES self-occlude, and
   "per line set" is not fine-grained enough to prevent it: within ONE polyline,
   a nearer segment's halo wins the depth test against a neighbouring segment's
   core. Measured on a 16-segment convergence arc: 78% of the arc gone. No
   depth offset fixes it either — the offset would have to exceed the depth
   change from one segment to the next, which is the same order as the
   separation between two genuinely crossing lines, so it would stop cutting the
   thing it exists to cut. The halo is therefore the **outer ring of the line's
   own quad**: one primitive, classified per fragment by its screen-space
   distance from the centreline (`vEdgePx` vs `vCoreHalfPx`, both
   `noperspective` — see the status note). A nearer mark still cuts a farther
   one because its now-wider quad wins the ordinary z-test. There is no second
   pass and no `glPolygonOffset`.
2. ✅ **Width comes from geometry, not `glLineWidth`** — T6 step 1, landed
   first. The ring is `kHaloPadPx` (1.5px) on each side, and is deliberately
   NOT depth-attenuated: a far line thins but keeps a constant-width ring, so it
   never loses the ability to cut.
3. ✅ Depth-cued attenuation, floored at `kMinLineWidthPx` — a distant bundle
   that thinned away would be an unknown rendered as an absence (invariant 3).
   Gated on the same bool, so "halos off" is byte-identical to the pre-T2
   geometry rather than a pass that runs and does nothing.
4. ✅ Applied to `traj_lines_`, `access_spurs_` and `conv_arcs_`, gated by one
   `SceneLayers::halos` (default ON, matching `edl` — both are depth cues over
   geometry that is already drawn). Doc 56's misprediction layer gets the
   attenuation (so its marks do not attenuate on a different rule from the
   trajectories beside them) but no ring: halos are scoped to the three sets
   named here.

**Fidelity.** The halo is background-coloured — it must not read as a second mark
class, and it must not be applied to the stippled statistical trajectories in a
way that fills their gaps. A statistical line's stipple is its fidelity grade
([embedded.h](../../../desktop/src/scene3d/shaders/embedded.h)); a
halo that made it look solid would launder a survey into an exact path. Halo the
statistical lines with a **stippled halo** or not at all, and state which.

**As implemented:** a **stippled halo**, and it costs nothing to get right —
because the ring is part of the line's own quad, the stipple `discard` runs
BEFORE the ring branch and on `gl_FragCoord`, so the ring inherits the line's
screen-space mask exactly. A gap in the line is a gap in its ring.
Two further decisions, both departures from the paper, both because this plane
is the terrain and the terrain's brightness is a measurement:
- The ring **washes** toward the background (alpha `kHaloColor[3]`) rather than
  replacing it, so a cell's own density reading survives under every worldline
  crossing it — which matters most in exactly the view this family prescribes
  for READING a cell. Nothing is lost: the cut comes from the ring's depth
  write (a farther line is not drawn at all), not from the colour it paints.
- The ring's colour is a **fixed constant** and must never follow the fidelity
  weather sky, whose hue is derived from fidelity severity. A depth cue whose
  colour moved with fidelity would be read as an encoding (D7). With the sky on
  it therefore reads as a dark outline rather than an exact background match.

**Tests.** ✅ `test_scene_fbo.cpp`. The crossing case is stated as the property
that produces it rather than as two hand-picked lines: turning halos on
**widens the footprint a mark occludes with** (measured on the convergence
layer's own toggle), which is exactly why a farther line is interrupted — every
pixel of that wider footprint now wins the z-test against anything behind it.
Alongside it: the halo does not swallow the mark it belongs to (a regression to
a separate wider primitive fails this immediately — that is the 78% case);
the layer toggle owns pixels; a depth-attenuated line at `kMaxRadius` still
draws something; a worldline past the playhead still DIMS rather than
disappearing with halos on. And the fidelity bar, re-running case (g)'s own
IGN-agreement measurement with halos ON: every pixel the statistical layer owns
— line AND ring — still matches the shader's own discard formula, so the ring
cannot have filled a gap.

**Done when.** ✅ A nearer mark demonstrably cuts what is behind it, and no
statistical mark got more solid.

### T3 — Contour bands and a height key: make Y a quantity (M)

**Goal.** Terrain height stops being "brighter means more" and becomes a number
with an interval and a legend.

**Why this technique.** `fwidth()` gives the screen-space footprint of a value,
which is exactly the width a contour band must be to stay one pixel wide at any
zoom or DPI. The alternative — a fixed-width band in world units — thickens when
you dolly in and aliases into noise when you dolly out.

**Steps.**
1. `kTerrainFrag`: derive the contour from the **pre-normalised** quantity. The
   height texture holds heights already normalised to [0,1]
   ([scene.h:70-72](../../../desktop/src/scene3d/scene.h#L70)), so pass the raw
   log-density range as a uniform pair and reconstruct, or upload a second channel
   — the point is the contour interval must be a stated number of *hits*, not a
   fraction of the tallest cell in this recording (which changes meaning between
   recordings, the worst failure mode available here).
2. Band with `smoothstep(0.0, fwidth(v) * 1.5, abs(fract(v / interval) - 0.5))`
   or equivalent; every Nth band emphasised, exactly as a topographic map indexes
   its contours.
3. `scene3d/hud.cpp`: a height key — "one band = N hits", the tallest cell's
   value, and which quantity the axis is (density for terrain, trace time for
   trajectory Y). Two axes share the vertical and **neither is labelled today**;
   the key must name both, which is the two-axes rule
   ([34](../archive/gui/34-playhead-and-scene-reach.md)) made visible.
4. `SceneLayers::contours`, defaulting **on** — a scale reference is not a
   decoration.

**Fidelity.** The interval is a real unit and is stated. A `TF_UNKNOWN` pit and a
`TF_TORN` cell must not grow contour bands that imply a measured height — an
unknown has no value to contour, and a torn cell's contours are a **lower bound**
and must be drawn dashed or capped with the frayed idiom the flags already own.

**Tests.** A golden-text HUD case for the key at several `nsteps`/max-height
combinations; `test_scene_fbo.cpp`: contour band pixel-width is within tolerance
of constant at two very different camera distances (the whole point of `fwidth`);
a `TF_UNKNOWN` cell carries no bands.

**Done when.** A viewer can read a cell's height as a number, and the same cell
reads the same at any zoom.

### T4 — Order-independent translucency, with a fallback that is not a lie (L)

**Goal.** The stacked translucent surfaces the catalog is full of composite
correctly regardless of draw order — and where they cannot, the scene degrades
visibly rather than silently mis-compositing.

**Why this matters here specifically.** Today's translucency is already
order-dependent and already slightly wrong: the trajectory pass runs with
`GL_BLEND` on and **depth-write left on**
([scene.cpp:655-656](../../../desktop/src/scene3d/scene.cpp#L655)), so a
translucent statistical line drawn earlier occludes an exact line drawn later.
With two surfaces that is a subtle artifact. The catalog's Phase 4 stacks five to
twenty (exploded evidence sheets, sediment bands, JIT strata, ensemble supports) —
at which point draw order silently decides what the user sees, and a fidelity
layer that is *sometimes hidden by another fidelity layer* is not a fidelity
layer.

**Steps.**
1. **First, fix what is already wrong**, independently of the rest: disable depth
   writes for the blended trajectory pass, and draw exact before statistical so
   the exact geometry owns the depth buffer. This is a few lines and is worth
   landing on its own.
2. **Weighted-blended OIT** (McGuire & Bavoil 2013) for the stacked surfaces: two
   render targets — an `RGBA16F` accumulation and an `R8`/`R16F` revealage — a
   depth-derived weight per fragment, and a resolve pass. MRT and float targets
   are core 3.0. **The gate is per-target blend functions**: accumulation needs
   `(ONE, ONE)` and revealage `(ZERO, ONE_MINUS_SRC_ALPHA)`, which needs
   `glBlendFunci` — GL 4.0 core, `ARB_draw_buffers_blend` as an extension.
3. **Probe it, do not assume it.** At `init_gl`, query the GL version and the
   extension string; store the answer on the Scene and expose it. Reaching the
   entry point without a loader is the real work on Linux — `glXGetProcAddress`
   is the route, and it is the first extension entry point this tree would take,
   so it needs a comment saying why the "no glad/glew" property still holds.
4. **The fallback is dithered (stochastic) transparency**, not "sort and hope":
   discard each fragment with probability `1 - alpha` using Interleaved Gradient
   Noise (Jimenez 2014 — a `dot` + `fract`, no texture, no beating against the
   terrain's regular grid the way an ordered Bayer matrix does). It is
   order-independent by construction and needs nothing beyond GLSL 130. It is
   noisier than WBOIT; that is the honest trade and the HUD says which path is
   active.
5. **The HUD states the compositing mode.** "layered translucency: weighted
   (exact)" vs "layered translucency: dithered (approximate)" — because a user
   comparing two machines' screenshots is otherwise comparing two algorithms
   without knowing it.

**Fidelity.** Whichever path runs, **a layer that is drawn must be visible**. The
one outcome forbidden is a stack where a sheet silently disappears behind another,
because the invariant the whole catalog rests on is that statistical, exact,
truncated and unknown are *physically distinct objects* — and an object you cannot
see is indistinguishable from one that was never drawn.

**Tests.** `test_scene_fbo.cpp`: for three overlapping translucent quads, the
resolved pixel is identical under all six draw orders (the definition of
order-independence) within tolerance; each quad contributes non-zero coverage; the
HUD's stated mode matches the path actually taken. On a lane where the extension
is absent, the dithered path must pass the same coverage assertion (not the
identity one).

**Done when.** Draw order does not change the image, or the HUD says the mode is
approximate and why.

### T5 — MSAA (S)

**Goal.** 1–3 px lines over a displaced grid stop crawling.

**Steps.**
1. `main.cpp`: `glfwWindowHint(GLFW_SAMPLES, 4)` beside the existing context
   hints ([main.cpp:110-127](../../../desktop/src/main.cpp#L110)).
2. The scene renders **offscreen** into the host's FBO
   ([scene_host.h:83-85](../../../desktop/src/ui/scene_host.h#L83)), so the window
   hint alone does nothing for it: give that FBO multisampled colour+depth
   renderbuffers and `glBlitFramebuffer` to a single-sample texture for ImGui.
   Both are core 3.0.
3. **The pick FBO stays single-sampled.** An averaged id is a different id; the
   pick pass must never be multisampled. Assert it.
4. Degrade rather than fail: if the multisample FBO is incomplete, fall back to
   the present single-sample path with a note, not a black pane.

**Tests.** `test_scene_fbo.cpp`: the pick buffer is byte-identical with MSAA on;
a diagonal line's edge pixels take intermediate values with MSAA on and only
extremes with it off.

**Done when.** Lines are smooth and picking is bit-for-bit unchanged.

### T6 — Portable line width, and the GLSL version question (M)

**Goal.** The scene's lines have the width the code asks for on every platform it
claims to run on.

**Steps.**
1. ✅ **Wide lines are not portable** (landed 2026-08-03). Core profiles
   deprecated them; a core implementation may report a
   `GL_ALIASED_LINE_WIDTH_RANGE` maximum of 1.0, and `glLineWidth` above the
   supported maximum sets `GL_INVALID_VALUE`. The tree called
   `glLineWidth(2.0f)` and `(3.0f)` on a context that is **core and
   forward-compatible on Apple**
   ([main.cpp:117-120](../../../desktop/src/main.cpp#L117)).
   Every one of those calls is now vertex-shader quad expansion: two triangles
   per segment carrying `pos` + the segment's `other` endpoint + a ±1 `side`
   ([linequad.h](../../../desktop/src/scene3d/linequad.h) builds them;
   [`kLineVert`](../../../desktop/src/scene3d/shaders/embedded.h) widens them),
   with the width a uniform in pixels — `kSpurWidthPx` 1.5, `kTrajWidthPx` 2,
   `kConvWidthPx` 3 ([scene.h](../../../desktop/src/scene3d/scene.h)). Two
   details worth stating because they are not free:
   - The spurs' nominal width rose from 1.0 to 1.5. A screen-space quad thinner
     than about a pixel can fall entirely between pixel centres and vanish,
     where GL's own line rasterizer guarantees a connected 1px chain. The same
     `kMinLineWidthPx` floor is what T2's depth attenuation is bounded by.
   - Each segment carries a square cap of half a width, so a polyline's joins
     close. Without it a per-segment expansion leaves a wedge at every turn —
     harmless at 2px on its own, but a hole in T2's halo, which exists
     precisely to stop an occluded line leaking through.

   The pick pass keeps its wide-line click-target draw unchanged (same buffers,
   same call, same 6px/5px) and gains a quad route selected by
   `Scene::pick_widening` from the queried range — see the status note above
   for why that split, and for the byte-identity check that backs it.
2. ✅ Query and log `GL_ALIASED_LINE_WIDTH_RANGE` once at `init_gl` so the next
   person to wonder has the answer in the error/diagnostic string. (Landed
   2026-08-02; step 1 now also *uses* it, to choose the pick pass's widening
   route. On this tree's Mesa/llvmpipe lane it reads `[1, 255]`, so that lane
   takes the wide-line route and the quad route is exercised by forcing it.)
3. ☐ **The GLSL version question — verify before changing anything.** *(Still
   open as of 2026-08-03: no macOS desktop lane exists in this tree, so no
   session here has been able to answer it. Do not "fix" it blind.)* Every scene
   shader is `#version 130` ([embedded.h](../../../desktop/src/scene3d/shaders/embedded.h)),
   while `main.cpp` selects GL 3.2 **core** on Apple and hands ImGui
   `#version 150` for that same context. Apple's core profile documents GLSL 1.50
   as its shading language. If 130 is rejected there, `init_gl` fails and the pane
   falls to the no-GL placard on macOS — the branch
   [46](46-3d-functional-roadmap.md)'s G13 describes.
   - **Verify it first, on Darwin.** This tree has no macOS desktop lane, so this
     is a claim with no local evidence and must not be "fixed" blind.
   - If confirmed, the fix is a per-platform version prefix prepended at compile
     time (`#version 150` + `in`/`out` are already the spelling these shaders use,
     so the bodies are likely unchanged), plus one compile check per shader in
     `init_gl` that reports *which* shader failed rather than a generic message.
   - If it turns out to work, **record that** in `embedded.h`'s version note so the
     question is not re-opened.

**Fidelity.** Line width is not an encoding today (it separates mark classes:
spurs 1, paths 2, convergences 3). If any future layer maps width to a quantity,
it must be documented at that layer — this task must not quietly turn a class
marker into a magnitude.

**Tests.** ✅ `test_camera.cpp` covers the pure expansion
([linequad.h](../../../desktop/src/scene3d/linequad.h)): segment counts for
strip vs pairs, every corner naming its partner endpoint, half the width either
side of the centreline, the square cap, the side-sign inversion at the far
endpoint (the bowtie guard), and a degenerate zero-length segment staying
finite. ✅ `test_scene_fbo.cpp` (GL): the same arc drawn at 6px and at 3px comes
out exactly twice as thick (measured as the mean scanline run length of the
rendered band — a pixel COUNT is confounded by terrain occlusion, so the fixture
also flattens `y_scale` for the measurement); the width does not move across a
3× camera dolly (screen-space, not world-space); the quad pick route resolves
*exactly* the same overlay ids as the wide-line route and reproduces its width;
a click on the arc returns the same id either way. ✅ A source scan in the pure
half pins the "Done when": every surviving `glLineWidth` argument is `1.0f` or
one of the two named pick-route constants, so a future layer cannot quietly
re-introduce a wide line on the colour path.

**Done when.** ◐ No `glLineWidth` call above 1.0 remains on any path a viewer
sees, and the two that remain are the pick pass's own route-selected click
targets, pinned by test. The version question (step 3) still has no answer and
still needs a Darwin machine.

## Fidelity notes (D7)

- **A depth cue is never an encoding.** EDL darkening and halo width both carry
  *depth*, and depth is not data here — the plane is address and the vertical is
  time or density. Nothing in T1/T2 may vary by fidelity class, region kind or
  magnitude, or a viewer will read shading as a measurement.
- **Anti-aliasing must not soften a fidelity mark.** The stipple that marks
  statistical geometry ([embedded.h:110-113](../../../desktop/src/scene3d/shaders/embedded.h#L110),
  [:171-172](../../../desktop/src/scene3d/shaders/embedded.h#L171)) is a *grade*,
  not a texture. T5's MSAA and T2's halos must both leave a statistical mark
  visibly non-solid; the test asserts coverage fraction for exactly this reason.
- **The pick pass is sacred.** T1, T4 and T5 all touch framebuffers, and each
  carries a "pick buffer is byte-identical" assertion. Every drill-in in the app
  routes through those ids.
- **T4's degraded mode is stated, not hidden.** Two users on two GPUs must be able
  to tell that they are looking at two different compositing algorithms.

## Effort and risk

Six tasks: one large (T4), four medium (T1, T2, T3, T6), one small (T5). The
risks worth naming:

- **T4 is the only task that needs something above core 3.0**, and its value is
  entirely in Phase 4 (the exploded stack, the strata, the ensemble). If capacity
  is short, land step 1 (the depth-write fix) and step 4 (the dithered path) and
  defer WBOIT — the dithered path alone makes stacking honest, which is the
  requirement; WBOIT only makes it prettier.
- **T6 step 3 is a verify-first item with no local lane.** Do not change the
  version directive on the strength of this document; the doc's job is to make
  sure the question gets asked on a machine that can answer it.
- ~~**T2 depends on T6's quad expansion.** They are separable in principle and
  needlessly duplicated work in practice — take them together.~~ Confirmed in
  practice (2026-08-03): T6 step 1 landed first and T2 was then almost entirely
  a fragment-shader branch plus two uniforms.
