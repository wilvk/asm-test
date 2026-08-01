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
inline const char *kTerrainFrag = R"GLSL(#version 130
in float vHeight;
in vec2 vUV;
uniform usampler2D uFlags;    // R32UI, n x n
uniform usampler2D uKind;     // R8UI, n x n — T1: region kind per cell
uniform int uZoning;          // T7: SceneLayers::zoning — 0 = plain amber ramp
out vec4 frag;
const uint TORN = 1u;     // rubble: a KNOWN lower bound (capture truncated/torn)
const uint STAT = 2u;
const uint CHURN = 4u;    // scaffold: a codeimage version changed within [0,t]
const uint UNKNOWN = 32u; // T2: fog-of-war — in-domain, no content at all
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
  float torn = ((f & TORN) != 0u) ? 1.0 : 0.0;
  frag = vec4(mix(base, vec3(1.0,0.15,0.15), torn*0.7), 1.0); // rubble = red gash
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

inline const char *kTrajFrag = R"GLSL(#version 130
in float vY;
uniform vec4 uColor;
uniform int uStipple;         // 1 = statistical: never a solid exact tube
uniform int uHasHead;         // T6: 1 on the followed trajectory's draw call
uniform float uHeadY;         // T6: the head's world Y (followed_t * scale)
uniform float uTailHalf;      // T6: half-width of the brightened tail band
out vec4 frag;
void main(){
  if (uStipple == 1) {
    float d = mod(gl_FragCoord.x + gl_FragCoord.y, 8.0);
    if (d < 4.0) discard;     // dashed screen-space pattern
  }
  vec4 c = uColor;
  if (uHasHead == 1 && abs(vY - uHeadY) < uTailHalf) {
    c = vec4(mix(uColor.rgb, vec3(1.0), 0.6), max(uColor.a, 0.95)); // comet tail
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

// --- T4 (44-faithful-city-phase-a): the ghost-fog terrain. Reuses kTerrainVert
//     (it needs only vUV + the displaced depth); the frag is desaturated,
//     translucent and STIPPLED — the same screen-space dither idiom kTrajFrag
//     uses for a statistical trajectory, not a second technique.
inline const char *kStatFrag = R"GLSL(#version 130
in float vHeight;
in vec2 vUV;
uniform usampler2D uFlags;
out vec4 frag;
void main(){
  float d = mod(gl_FragCoord.x + gl_FragCoord.y, 8.0);
  if (d < 4.0) discard;              // stippled — never a solid fill
  vec3 grey = vec3(0.55, 0.55, 0.60); // desaturated — never the exact palette
  vec3 col = mix(grey * 0.5, grey, clamp(vHeight, 0.0, 1.0));
  frag = vec4(col, 0.55);            // translucent
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

} // namespace asmdesk::scene3d::shaders
#endif // ASMDESK_SCENE3D_SHADERS_EMBEDDED_H
