// embedded.h — the 3D-overview GLSL, embedded as string literals so the scene
// loads NO shader file at runtime (docs/internal/archive/gui/10-spacetime-3d-overview.md
// T4 step 2/3/5). The terrain pair is the brief's terrain.vert/terrain.frag; the
// trajectory and colour-ID pick pairs are the rest of T4.
//
// Version note: the brief writes `#version 330 core`, but 03's GL context is
// created at 3.0 (main.cpp: CONTEXT_VERSION 3,0 / GLSL 130) and the ImGui backend
// runs at "#version 130". These shaders are therefore written at `#version 130`
// so they compile on that same context (and on software Mesa's higher one). The
// only 330 feature they give up is `layout(location=)` — attribute and integer
// fragment-output locations are bound from C++ instead (scene.cpp / pick.cpp via
// glBindAttribLocation + glBindFragDataLocation), which is the 3.0-era spelling.
#ifndef ASMDESK_SCENE3D_SHADERS_EMBEDDED_H
#define ASMDESK_SCENE3D_SHADERS_EMBEDDED_H

namespace asmdesk::scene3d::shaders {

// --- terrain: a grid displaced by the R32F height texture, coloured by kind +
//     the R32UI flags texture (TORN/STAT/CHURN) — the brief's terrain.{vert,frag}.
inline const char *kTerrainVert = R"GLSL(#version 130
in vec2 cell;                 // (x,y) in [0,n)
uniform sampler2D uHeight;    // R32F, n x n (heights normalised to [0,1])
uniform mat4 uMVP;
uniform float uYScale;
uniform float uN;
out float vHeight;
out vec2 vUV;
void main(){
  vUV = (cell + 0.5) / uN;
  float hgt = texture(uHeight, vUV).r;
  vHeight = hgt;
  vec3 p = vec3(vUV.x, hgt * uYScale, vUV.y);   // plane in XZ, height in Y
  gl_Position = uMVP * vec4(p, 1.0);
}
)GLSL";

// 44-faithful-city-phase-a T1/T2: kindHue mirrors space::region_style()'s six
// RGB triples VERBATIM (space/projection.cpp) — keep the two tables in sync;
// the C++ table is the source of truth (it feeds the HUD legend too), this is
// a shader-side duplicate by the SAME convention TORN/STAT/CHURN already use
// (they duplicate TerrainFlag's C++ bit values). Order matches Region::Kind
// (space/types.h): Code, Stack, Heap, Data, Mmap, Unknown.
//
// 52 T1 (flat-terrain-surface.md): views/scene2d.h's cell_paint() is the
// GL-free C++ mirror of this WHOLE fragment shader's branch chain — kind hue
// (region_style(), same as kindHue above) -> height mix -> churn -> stat ->
// unknown -> torn, in this exact order. The GLSL string above cannot include
// a header, so this comment is the keep-in-sync mechanism: a branch added or
// reordered here must be added or reordered in cell_paint() too, or the flat
// surface and this 3D terrain will disagree about a cell's fidelity state.
inline const char *kTerrainFrag = R"GLSL(#version 130
in float vHeight;
in vec2 vUV;
uniform usampler2D uFlags;    // R32UI, n x n
uniform usampler2D uKind;     // R8UI, n x n — T1: region kind per cell
uniform int uZoning;          // T7: SceneLayers::zoning — 0 = plain amber ramp
uniform float uContourLevels; // T3 (49): band count; <=0 disables banding
uniform float uN;             // 50 T2: cell grid size, to find THIS fragment's cell
uniform int uHighlightCell;   // 50 T2: the flat views' selection, located onto
                              // this plane by its ADDRESS; -1 = no highlight
// T2 (56-fidelity-and-module-layers): SceneLayers::confidence — 1 re-lifts the
// ink toward "how much do I trust this" instead of density (uHeight/vHeight
// themselves are UNCHANGED: this brief re-tints the existing geometry rather
// than re-deriving the mesh, see the doc's own status note on this scoping).
uniform int uConfidence;
// T4 (56-fidelity-and-module-layers): SceneLayers::opcode — 1 tints a CODE
// cell by its dominant instruction class (space::OpClass, uOpClass's per-
// cell R8UI) instead of region kind. uOpClass == 255 means "no
// classification for this cell" (a non-code cell, or a code cell
// build_opcode_terrain never saw) and renders exactly as if uOpcode were 0 —
// never a stray colour for cells this layer has nothing to say about.
uniform int uOpcode;
uniform usampler2D uOpClass; // R8UI, n x n — 0..7 = OpClass, 255 = none
out vec4 frag;
const uint TORN = 1u;     // rubble: a KNOWN lower bound (capture truncated/torn)
const uint STAT = 2u;
const uint CHURN = 4u;    // scaffold: a codeimage version changed within [0,t]
const uint UNKNOWN = 32u; // T2: fog-of-war — in-domain, no content at all
// T2 (56): the coverage-window mask bits (space::TF_INWINDOW_EMPTY / TF_OUTWINDOW),
// set ONLY when a HotEdgeSceneView stated a window (views/hotedges.cpp's
// apply_coverage_window) — mirrored here exactly like TORN/STAT/CHURN/UNKNOWN
// already mirror space::TerrainFlag.
const uint INWINDOW_EMPTY = 64u;
const uint OUTWINDOW = 128u;
// T4 (56): mirrors space::OpClass (mnemonic.h) VERBATIM, the same keep-in-
// sync convention kindHue follows against Region::Kind. Index 0 (Unknown) is
// a neutral grey — "described, but not understood" (an abstain), distinct
// from the fog-of-war UNKNOWN bit above ("never described at all").
const vec3 opClassHue[8] = vec3[8](
  vec3(0.55,0.55,0.55), // Unknown: neutral grey, never coerced
  vec3(0.35,0.75,0.95), // Move
  vec3(0.90,0.55,0.15), // IntArith
  vec3(0.80,0.80,0.40), // Logic
  vec3(1.00,0.35,0.35), // CompareBranch
  vec3(0.70,0.50,0.85), // ScalarFloat
  vec3(0.85,0.30,0.75), // VectorSIMD
  vec3(0.45,0.85,0.45)  // System
);
const vec3 kindHue[6] = vec3[6](
  vec3(0.90,0.55,0.15), // Code
  vec3(0.35,0.75,0.95), // Stack
  vec3(0.45,0.85,0.45), // Heap
  vec3(0.80,0.80,0.40), // Data
  vec3(0.70,0.50,0.85), // Mmap
  vec3(0.55,0.55,0.60)  // Unknown region kind (not the fog-of-war UNKNOWN bit)
);
void main(){
  uint f = texture(uFlags, vUV).r;
  uint k = texture(uKind, vUV).r;
  // an off-domain sentinel (255) clamps to grey; zoning off -> always Code's
  // hue (today's plain amber ramp), ignoring uKind entirely (T7).
  vec3 hot = (uZoning == 1) ? kindHue[k < 6u ? k : 5u] : kindHue[0];
  // T4 (56): opcode mode OVERRIDES the hue for a classified cell only —
  // uOpClass == 255 (no classification) falls through to the hue above
  // unchanged, so a data cell or an un-classified code cell never shows a
  // stray colour this layer has nothing to say about.
  if (uOpcode == 1) {
    uint oc = texture(uOpClass, vUV).r;
    if (oc < 8u)
      hot = opClassHue[oc];
  }
  // kind sets the HUE at full density; height (unchanged) still sets
  // brightness — kindHue[Code] is BYTE-IDENTICAL to the amber this used to
  // hardcode, so a code-only recording renders exactly today's ramp.
  vec3 base = mix(vec3(0.08,0.10,0.16), hot, clamp(vHeight,0.0,1.0));
  if ((f & CHURN) != 0u) base = mix(base, vec3(0.2,0.7,1.0), 0.5); // scaffold
  if ((f & STAT)  != 0u) base *= 0.6;           // statistical layer is dimmer
  if ((f & UNKNOWN) != 0u)
    base = mix(base, vec3(0.02,0.02,0.03), 0.85); // sunken fog-of-war pit
  // T2 (56-fidelity-and-module-layers): confidence mode amplifies the SAME
  // per-flag facts into a trust reading rather than deriving a new one —
  // unknown reads as a harder void, and the coverage-window bits (set only by
  // hotedges.cpp's apply_coverage_window, so a hatch never appears unless a
  // window was actually stated) get their own idiom, distinct from the
  // contour bands below.
  if (uConfidence == 1) {
    if ((f & UNKNOWN) != 0u)
      base = mix(base, vec3(0.0, 0.0, 0.0), 0.5);
    if ((f & (INWINDOW_EMPTY | OUTWINDOW)) != 0u) {
      // A screen-space diagonal hatch: cheap, orientation-stable at any zoom.
      float hatch = fract((gl_FragCoord.x + gl_FragCoord.y) * 0.15);
      float line = step(0.5, hatch);
      vec3 hue = (f & OUTWINDOW) != 0u
                     ? vec3(0.05, 0.05, 0.07)  // never-looked: near-black
                     : vec3(0.45, 0.35, 0.10); // below-rate: dim amber
      base = mix(base, hue, 0.55 * line);
    }
  }
  float torn = ((f & TORN) != 0u) ? 1.0 : 0.0;
  vec3 col = mix(base, vec3(1.0,0.15,0.15), torn*0.7); // rubble = red gash
  // T3 (49, refined 55): iso-density contour bands re-encode vHeight — never
  // drawn on a flat/zero cell (no measured level to band), a fog-of-war/off-
  // domain cell (UNKNOWN already covers "no value"; a band there would imply
  // one), or a TORN cell (55: its height is a KNOWN LOWER BOUND, not a
  // measurement — a band would claim a precision the rubble does not have;
  // the red gash above is TORN's own idiom, so this simply does not layer a
  // second one on top of it). The +0.5 phase offset is deliberate: height is
  // normalised so the BRIGHTEST cell in any slice sits at EXACTLY 1.0, and an
  // unshifted fract(1.0*levels) is exactly 0 for any integer level count —
  // every terrain's single hottest cell would otherwise always land ON a
  // line and read as darkened. Shifting the phase puts 0.0 and 1.0 mid-band
  // instead.
  if (uContourLevels > 0.0 && (f & UNKNOWN) == 0u && (f & TORN) == 0u &&
      vHeight > 0.0) {
    float v = clamp(vHeight, 0.0, 1.0) * uContourLevels;
    float band = fract(v + 0.5);
    // 55 T3: the band's screen-space width stays constant across zoom via
    // fwidth — but taken on `v` (the PRE-fract quantity), never on `band`
    // itself: fract() is discontinuous at every integer, so fwidth(band)
    // would spike to a huge, spurious value exactly AT each line (the
    // classic fwidth-of-a-sawtooth trap), which is precisely where the true
    // width is needed most. A fixed world-space width (the old literal 0.08)
    // thickens when the camera dollies in and aliases into noise dollied
    // out; this does neither.
    float w = max(fwidth(v) * 1.5, 1e-4);
    float line = 1.0 - smoothstep(0.0, w, min(band, 1.0 - band));
    col = mix(col, col * 0.55, line);
  }
  // 50 T2: ring the located cell — an outline near its edge, never a height
  // change or a hue replacement, so the cell's own density/kind stay
  // readable underneath (a spatial POINTER, not a measurement). vUV is
  // already this fragment's cell-space UV (the vertex shader's own (cell+
  // 0.5)/uN); recomputing the integer cell from it is the SAME arithmetic
  // pick.cpp's classify_cell and this file's OWN kPickTerrainFrag use.
  if (uHighlightCell >= 0) {
    ivec2 xy = ivec2(vUV * uN);
    int cell = xy.y * int(uN) + xy.x;
    if (cell == uHighlightCell) {
      vec2 within = fract(vUV * uN);
      float edge = min(min(within.x, 1.0 - within.x),
                       min(within.y, 1.0 - within.y));
      float ring = 1.0 - smoothstep(0.0, 0.14, edge);
      col = mix(col, vec3(1.0, 0.95, 0.25), ring);
    }
  }
  frag = vec4(col, 1.0);
}
)GLSL";

// --- trajectory: a line strip / points at world (u, t*scale, v). Exact fidelity
//     is opaque; statistical is stippled + translucent (uStipple + uColor.a).
// T6 (44-faithful-city-phase-a): vY carries pos.y through unchanged — since
// EVERY vertex in one upload shares the SAME world-Y-per-step scale
// (scene.cpp's `scale`, Scene::traj_scale_), pos.y already IS t*scale, so the
// comet tail needs no new per-vertex attribute: the frag shader compares vY
// against uHeadY (= followed_t * traj_scale_, computed on the CPU once per
// frame) directly.
inline const char *kTrajVert = R"GLSL(#version 130
in vec3 pos;
uniform mat4 uMVP;
uniform float uPointSize;
out float vY;
// T2 (55): kTrajFrag is linked against BOTH this shader and kLineVert, and a
// fragment input with no matching vertex output will not link -- so the two
// halo varyings are declared here too, at values that put every fragment of a
// POINT glyph inside the core. A point has no width to ring.
noperspective out float vEdgePx;
noperspective out float vCoreHalfPx;
void main(){
  vY = pos.y;
  vEdgePx = 0.0;
  vCoreHalfPx = 1.0;
  gl_Position = uMVP * vec4(pos, 1.0);
  gl_PointSize = uPointSize;
}
)GLSL";

// --- T6 (55-scene-render-quality) step 1: the PORTABLE line width. Wide lines
//     (`glLineWidth` above 1.0) are deprecated in a core profile and a core
//     implementation may report GL_ALIASED_LINE_WIDTH_RANGE = [1,1] and either
//     clamp or raise GL_INVALID_VALUE — which is exactly the context this app's
//     own Apple path requests (main.cpp: 3.2 core + forward-compatible). This
//     shader replaces every such call: each SEGMENT arrives as two triangles
//     (6 vertices) carrying its own endpoint, the segment's OTHER endpoint, and
//     a ±1 side, and the width is applied in SCREEN SPACE here, in pixels.
//
// It serves BOTH the colour pass (linked against kTrajFrag, which reads vY) and
// — where the pick pass takes its quad route — the id pass (linked against
// kPickPointFrag, which reads fId). One vertex shader, two programs: the
// colour program simply never enables the `vid` attribute, and a vertex output
// no fragment shader reads is legal. That is what keeps the two passes' widened
// geometry provably the SAME expansion rather than two implementations that
// could drift.
//
// Fidelity (D7, this brief's own note): width here separates mark CLASSES
// (spurs, paths, convergences) and — under uDepthCue — carries DEPTH. Neither
// is a magnitude. No layer may map uWidthPx to a quantity without saying so at
// that layer.
inline const char *kLineVert = R"GLSL(#version 130
in vec3 pos;    // this vertex's own endpoint
in vec3 other;  // the segment's OTHER endpoint
in float side;  // -1 / +1: which side of the centreline this corner sits on
in uint vid;    // pick id (id pass only; the colour VAO leaves it disabled)
uniform mat4 uMVP;
uniform vec2 uViewportPx;   // (width, height) in pixels
uniform float uWidthPx;     // the mark class's nominal CORE width, IN PIXELS
uniform float uMinWidthPx;  // the floor -- see uDepthCue
uniform int uDepthCue;      // T2 (55): 1 = attenuate width with eye distance
uniform float uDepthCueRef; // the eye distance at which the width is nominal
uniform float uHaloPadPx;   // T2 (55): halo ring per side, 0 = no halo at all
out float vY;
// noperspective, and it is load-bearing: both carry SCREEN-SPACE PIXEL
// distances, and the default (perspective-correct) interpolation reports the
// value that is linear in EYE space instead. The perpendicular offset below is
// applied as a constant screen offset, so in eye space the quad is a trapezoid
// that widens with depth -- and a foreshortened segment then reports a
// fragment 2px off the centreline as being well inside the core, which paints
// ring pixels as line and visibly fattens the mark. (Measured: it moved a
// terrain cell's sampled pixel from terrain to line colour, silently breaking
// a golden-scene brightness assertion.)
noperspective out float vEdgePx;     // signed distance from the centreline, px
noperspective out float vCoreHalfPx; // half the CORE width here, px
flat out uint fId;
void main(){
  vY = pos.y;
  fId = vid;
  vec4 cp = uMVP * vec4(pos, 1.0);
  vec4 co = uMVP * vec4(other, 1.0);
  // A vertex at or behind the eye plane has no screen position to widen in;
  // emit the un-widened clip position (the segment is being clipped anyway)
  // rather than dividing by a non-positive w and scattering garbage.
  if (cp.w <= 0.0 || co.w <= 0.0) { gl_Position = cp; return; }
  vec2 half_vp = uViewportPx * 0.5;
  vec2 sp = (cp.xy / cp.w) * half_vp;   // this endpoint, in pixels
  vec2 so = (co.xy / co.w) * half_vp;   // the other endpoint, in pixels
  vec2 d = so - sp;
  float len = length(d);
  // A degenerate (zero-length) segment has no direction; pick one rather than
  // normalizing a zero vector (undefined) -- it collapses to a square dot.
  vec2 dir = (len > 1e-6) ? d / len : vec2(1.0, 0.0);
  vec2 perp = vec2(-dir.y, dir.x);
  float w = uWidthPx;
  if (uDepthCue == 1) {
    // T2 step 3: depth-cued attenuation -- a farther line is thinner, which is
    // a DEPTH cue and never a magnitude (D7). cp.w is eye distance for this
    // projection. Bounded below by uMinWidthPx: a distant bundle that thinned
    // to nothing would be an unknown rendered as an absence (invariant 3).
    w *= clamp(uDepthCueRef / max(cp.w, 1e-4), 0.0, 1.0);
  }
  w = max(w, uMinWidthPx);
  vCoreHalfPx = w * 0.5;
  // T2 (55): the halo is the OUTER RING OF THIS SAME QUAD, not a second,
  // wider primitive drawn behind it. That is what makes it safe on a densely
  // tessellated polyline: with two primitives, a nearer segment's halo wins
  // the depth test against a neighbouring segment's core and eats its own
  // line (measured: a 16-segment arc lost 78% of itself). One primitive has
  // no depth relationship to get wrong -- neighbouring segments overlap
  // core-over-core along the shared centreline -- while a DIFFERENT mark is
  // still cut by the ordinary z-test, which is the effect the paper is for.
  // The pad is NOT depth-attenuated: the ring stays a constant screen-space
  // width, so a far line thins but never loses its ability to cut.
  float hw = w * 0.5 + uHaloPadPx;
  vEdgePx = side * hw;
  // Across the centreline: half the core width plus the ring. ALONG it: a
  // square cap of half the CORE width only, never of the ringed width. The cap
  // closes the wedge a per-segment expansion leaves at a polyline's joins, and
  // every fragment inside it reads as core (|vEdgePx| there is whatever the
  // corner carries) — so extending it by the ring as well would paint CORE
  // colour up to a ring's width past every joint, visibly fattening the mark
  // the moment halos turned on. With this, the core's own footprint is
  // identical whether the ring is there or not; the ring only ever adds
  // OUTSIDE it.
  vec2 offs = perp * (hw * side) - dir * (w * 0.5);
  cp.xy += (offs / half_vp) * cp.w;
  gl_Position = cp;
}
)GLSL";

// T1 (49-one-time-truth): uTimeCutY/uHasTimeCut clip the path to the terrain
// playhead — DIM past the cut, never discard (the vertices beyond it are real
// recorded data; hiding them would claim the recording ends there). Applied
// FIRST, before the existing stipple/comet branches, so a statistical path
// still dashes on both sides of the cut and the comet tail still layers over
// the (possibly dimmed) base colour unchanged.
inline const char *kTrajFrag = R"GLSL(#version 130
in float vY;
// T2 (55): both SCREEN-space pixel distances -- see kLineVert for why the
// noperspective qualifier here is load-bearing rather than a micro-optimisation
// (it must match the vertex shader's declaration, or the program will not link).
noperspective in float vEdgePx;     // distance from the centreline, in pixels
noperspective in float vCoreHalfPx; // past this, the fragment is halo, not line
uniform int uHalo; // T2 (55): 1 = this draw carries a halo ring
// T2 (55): the ring's colour+alpha, a FIXED constant handed down from scene.h.
// Partial alpha on purpose: the cut comes from this quad's DEPTH write (a
// farther line simply is not drawn), so the ring does not need to erase what
// is behind it — and on this plane what is behind it is the terrain, whose
// brightness is a measurement. See kHaloColor's own comment.
uniform vec4 uHaloColor;
uniform vec4 uColor;
uniform int uStipple;         // 1 = statistical: never a solid exact tube
uniform int uHasHead;         // T6: 1 on the followed trajectory's draw call
uniform float uHeadY;         // T6: the head's world Y (followed_t * scale)
uniform float uTailHalf;      // T6: half-width of the brightened tail band
uniform float uTimeCutY;      // T1 (49): the terrain playhead's world Y
uniform int uHasTimeCut;      // T1 (49): 0 when the playhead is at the end
out vec4 frag;
// T4 (55-scene-render-quality): Interleaved Gradient Noise (Jimenez 2014) --
// see kStatFrag's own comment for why (duplicated rather than shared: these
// are independent string literals, the existing convention TORN/STAT/CHURN's
// C++-mirrored bit values already follow).
float ign(vec2 p) {
  return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}
void main(){
  vec4 c = uColor;
  if (uHasTimeCut == 1 && vY > uTimeCutY) {
    c.rgb *= 0.35; // dim, not discard: "recorded, not yet reached in this view"
  }
  // T2 (55): the halo ring. Taken BEFORE the time-cut dim and the comet tail
  // (both of which are readings of the LINE, and a halo carries no reading of
  // its own) but AFTER uColor is read, so the stipple below still sees the
  // line's own alpha. D7: the colour is a fixed constant handed down from
  // scene.h -- it must never follow the fidelity weather sky, or a depth cue
  // would start encoding fidelity.
  bool halo = (uHalo == 1) && abs(vEdgePx) > vCoreHalfPx;
  if (uStipple == 1) {
    // T4 (55): a fragment survives with probability c.a (uColor's own alpha,
    // 0.45 for a statistical line) and is then drawn FULLY OPAQUE -- the
    // "dithered/stochastic transparency" fallback (McGuire & Bavoil's
    // weighted-blended OIT is the "exact" alternative this brief defers, see
    // its own status note): compositing against whatever is already in the
    // depth buffer is then a NORMAL z-test, with no blend and no draw-order
    // dependency, which is what makes stacking honest regardless of draw
    // order. This replaces the old regular (x+y) mod 8 checkerboard, whose
    // REGULAR screen-space frequency could beat against the terrain's own
    // regular grid (moire) -- IGN's irregular frequency cannot.
    if (ign(gl_FragCoord.xy) > c.a) discard;
    c.a = 1.0;
  }
  // The halo shares the line's stipple mask above by construction (the discard
  // is evaluated on gl_FragCoord, before this branch), so a statistical line's
  // gaps stay gaps and still show what is behind them. An opaque halo would
  // fill them and launder a sampled survey into a solid, exact-looking path.
  if (halo) { frag = uHaloColor; return; }
  if (uHasHead == 1 && abs(vY - uHeadY) < uTailHalf) {
    c = vec4(mix(c.rgb, vec3(1.0), 0.6), max(c.a, 0.95)); // comet tail
  }
  frag = c;
}
)GLSL";

// --- picking (colour-ID): a second pass to an R32UI target. The terrain pass
//     reuses kTerrainVert (it needs vUV + the same displaced depth) and writes
//     id = uIdBase + cell; the vertex pass writes a per-vertex id. id 0 is "no
//     pickable" (the cleared background).
inline const char *kPickTerrainFrag = R"GLSL(#version 130
in float vHeight;
in vec2 vUV;
uniform float uN;
uniform uint uIdBase;         // 1 (so cell 0 is a real id, not the background)
out uint fragid;
void main(){
  uint nn = uint(uN);
  uint cx = uint(vUV.x * uN);
  uint cy = uint(vUV.y * uN);
  if (cx >= nn) cx = nn - 1u;
  if (cy >= nn) cy = nn - 1u;
  fragid = uIdBase + cy * nn + cx;
}
)GLSL";

inline const char *kPickPointVert = R"GLSL(#version 130
in vec3 pos;
in uint vid;
uniform mat4 uMVP;
uniform float uPointSize;
flat out uint fId;
void main(){
  fId = vid;
  gl_Position = uMVP * vec4(pos, 1.0);
  gl_PointSize = uPointSize;
}
)GLSL";

inline const char *kPickPointFrag = R"GLSL(#version 130
flat in uint fId;
out uint fragid;
void main(){ fragid = fId; }
)GLSL";

// --- T4 (44-faithful-city-phase-a, dithering refined 55-scene-render-quality):
//     the ghost-fog terrain. Reuses kTerrainVert (it needs only vUV + the
//     displaced depth); the frag is desaturated and STIPPLED — the same
//     screen-space dither idiom kTrajFrag uses for a statistical trajectory,
//     not a second technique.
inline const char *kStatFrag = R"GLSL(#version 130
in float vHeight;
in vec2 vUV;
uniform usampler2D uFlags;
out vec4 frag;
// T4 (55): Interleaved Gradient Noise (Jimenez 2014) — a fragment survives
// with probability 0.55 and is then drawn FULLY OPAQUE, so this surface
// composites against the exact terrain (and anything else already in the
// depth buffer) via a normal z-test: no blend, no draw-order dependency. This
// is "dithered (stochastic) transparency" — the T4 fallback this brief takes
// over weighted-blended OIT (deferred; see the doc's own status note) — and
// it replaces the old regular (x+y) mod 8 checkerboard, whose REGULAR
// screen-space frequency could beat against the terrain's own regular grid
// (moire); IGN's irregular frequency cannot. The 0.55 keep-probability is the
// same number the old code spent as a blend alpha — the surface's apparent
// translucency is now spent as COVERAGE instead of per-pixel opacity.
float ign(vec2 p) {
  return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}
void main(){
  if (ign(gl_FragCoord.xy) > 0.55) discard;
  vec3 grey = vec3(0.55, 0.55, 0.60); // desaturated — never the exact palette
  vec3 col = mix(grey * 0.5, grey, clamp(vHeight, 0.0, 1.0));
  frag = vec4(col, 1.0); // opaque: the discard pattern IS the translucency
}
)GLSL";

// --- T3 (44-faithful-city-phase-a): the fidelity weather sky, a full-screen
//     NDC quad drawn FIRST (before the terrain, far plane, depth-write off).
//     uAmbient/uFrontColor/uSunDir/uFogDensity mirror Atmosphere verbatim
//     (scene3d/atmosphere.h) — the shell computes them from fidelity_severity
//     and passes plain floats every frame via Scene::set_atmosphere.
inline const char *kSkyVert = R"GLSL(#version 130
in vec2 pos; // NDC quad corners, [-1, 1]
out vec2 vScreen;
void main(){
  vScreen = pos;
  gl_Position = vec4(pos, 0.9999, 1.0);
}
)GLSL";

inline const char *kSkyFrag = R"GLSL(#version 130
in vec2 vScreen;
uniform vec3 uAmbient;
uniform vec3 uFrontColor;
uniform vec3 uSunDir;
uniform float uFogDensity;
out vec4 frag;
void main(){
  float t = clamp(vScreen.y * 0.5 + 0.5, 0.0, 1.0);
  vec3 col = mix(uAmbient, uFrontColor, t);
  float sun = clamp(dot(normalize(vec3(vScreen, 0.6)), normalize(uSunDir)), 0.0, 1.0);
  col += sun * 0.15 * uFrontColor;
  col = mix(col, uAmbient * 0.6, clamp(uFogDensity, 0.0, 1.0));
  frag = vec4(col, 1.0);
}
)GLSL";

// --- T1 (55-scene-render-quality): Eye-Dome Lighting, a fullscreen pass that
//     darkens a pixel by how much nearer its screen-space neighbours are —
//     the depth cue that makes which mark is in front legible with no
//     lighting model. Reuses kSkyVert's NDC-quad geometry (vao_sky_) rather
//     than a second quad VAO. Runs on the COLOUR path only; the pick pass
//     never binds this program (see scene.cpp's render()/draw_edl_pass split).
inline const char *kEdlVert = R"GLSL(#version 130
in vec2 pos; // NDC quad corners, [-1,1] -- the same geometry as the sky quad
out vec2 vUV;
void main(){
  vUV = pos * 0.5 + 0.5;
  gl_Position = vec4(pos, 0.0, 1.0);
}
)GLSL";

inline const char *kEdlFrag = R"GLSL(#version 130
in vec2 vUV;
uniform sampler2D uColor;
uniform sampler2D uDepth;
uniform vec2 uTexel;        // 1/resolution, in texels -- the neighbour step unit
uniform float uEdlStrength; // T1 step 3: HUD-exposed
uniform float uEdlRadiusPx; // T1 step 3: HUD-exposed, in PIXELS (screen-space
                            // stable across dolly, never a world-space radius)
uniform float uNear;
uniform float uFar;
out vec4 frag;
// Perspective depth -> linear eye-space distance (T1 step 2: EDL must read the
// LINEARISED depth or the response is all cliff near the near plane and flat
// everywhere else, since the raw buffer value is non-linear in eye distance).
float linearize(float d) {
  float ndc = d * 2.0 - 1.0;
  return (2.0 * uNear * uFar) / (uFar + uNear - ndc * (uFar - uNear));
}
void main(){
  vec3 baseColor = texture(uColor, vUV).rgb;
  float d0 = texture(uDepth, vUV).r;
  // The cleared far value: nothing drawn here (sky/background), so there is no
  // depth discontinuity to shade -- an EDL response here would darken the sky
  // itself, which is not a depth cue, it is a lighting change to nothing.
  if (d0 >= 1.0) { frag = vec4(baseColor, 1.0); return; }
  float z0 = linearize(d0);
  const int NTAPS = 8;
  vec2 dirs[8] = vec2[8](
    vec2(1.0,0.0), vec2(-1.0,0.0), vec2(0.0,1.0), vec2(0.0,-1.0),
    vec2(0.7071,0.7071), vec2(-0.7071,0.7071), vec2(0.7071,-0.7071), vec2(-0.7071,-0.7071)
  );
  float response = 0.0;
  for (int i = 0; i < NTAPS; i++) {
    vec2 uv = vUV + dirs[i] * uTexel * uEdlRadiusPx;
    float dn = texture(uDepth, uv).r;
    float zn = (dn >= 1.0) ? z0 : linearize(dn); // an off-scene neighbour adds no cue
    response += max(0.0, log2(z0) - log2(zn));
  }
  response /= float(NTAPS);
  // A uniform multiplier on luminance ONLY (D7, this brief's own fidelity
  // note): EDL is a depth cue, never an encoding, so it must never touch hue
  // and must apply identically regardless of fidelity class or region kind.
  float shade = exp(-uEdlStrength * response);
  frag = vec4(baseColor * shade, 1.0);
}
)GLSL";

// --- T3 (56-fidelity-and-module-layers.md): the per-module residency
//     skyline — one flat translucent quad per region's footprint bounding
//     box, hued by region_style(kind), height = log-scaled summed heat.
//     Scoped simplification (see canopy.h's own doc comment): plain alpha
//     blend, not the dithered-discard idiom 55 established for the stat
//     terrain/statistical trajectories — a second translucent-compositing
//     technique on the SAME plane, stated rather than silently mismatched.
inline const char *kCanopyVert = R"GLSL(#version 130
in vec3 pos;
uniform mat4 uMVP;
void main(){
  gl_Position = uMVP * vec4(pos, 1.0);
}
)GLSL";

inline const char *kCanopyFrag = R"GLSL(#version 130
uniform vec4 uColor;
out vec4 frag;
void main(){ frag = uColor; }
)GLSL";

} // namespace asmdesk::scene3d::shaders
#endif // ASMDESK_SCENE3D_SHADERS_EMBEDDED_H
