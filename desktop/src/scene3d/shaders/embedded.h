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
void main(){
  vY = pos.y;
  gl_Position = uMVP * vec4(pos, 1.0);
  gl_PointSize = uPointSize;
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

} // namespace asmdesk::scene3d::shaders
#endif // ASMDESK_SCENE3D_SHADERS_EMBEDDED_H
