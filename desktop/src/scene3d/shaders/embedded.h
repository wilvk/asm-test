// embedded.h — the 3D-overview GLSL, embedded as string literals so the scene
// loads NO shader file at runtime (docs/internal/gui/10-spacetime-3d-overview.md
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

inline const char *kTerrainFrag = R"GLSL(#version 130
in float vHeight;
in vec2 vUV;
uniform usampler2D uFlags;    // R32UI, n x n
out vec4 frag;
const uint TORN = 1u;
const uint STAT = 2u;
const uint CHURN = 4u;
void main(){
  uint f = texture(uFlags, vUV).r;
  vec3 base = mix(vec3(0.08,0.10,0.16), vec3(0.9,0.55,0.15), clamp(vHeight,0.0,1.0));
  if ((f & CHURN) != 0u) base = mix(base, vec3(0.2,0.7,1.0), 0.5);
  if ((f & STAT)  != 0u) base *= 0.6;           // statistical layer is dimmer
  float torn = ((f & TORN) != 0u) ? 1.0 : 0.0;
  frag = vec4(mix(base, vec3(1.0,0.15,0.15), torn*0.7), 1.0); // torn = red gash
}
)GLSL";

// --- trajectory: a line strip / points at world (u, t*scale, v). Exact fidelity
//     is opaque; statistical is stippled + translucent (uStipple + uColor.a).
inline const char *kTrajVert = R"GLSL(#version 130
in vec3 pos;
uniform mat4 uMVP;
uniform float uPointSize;
void main(){
  gl_Position = uMVP * vec4(pos, 1.0);
  gl_PointSize = uPointSize;
}
)GLSL";

inline const char *kTrajFrag = R"GLSL(#version 130
uniform vec4 uColor;
uniform int uStipple;         // 1 = statistical: never a solid exact tube
out vec4 frag;
void main(){
  if (uStipple == 1) {
    float d = mod(gl_FragCoord.x + gl_FragCoord.y, 8.0);
    if (d < 4.0) discard;     // dashed screen-space pattern
  }
  frag = uColor;
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

} // namespace asmdesk::scene3d::shaders
#endif // ASMDESK_SCENE3D_SHADERS_EMBEDDED_H
