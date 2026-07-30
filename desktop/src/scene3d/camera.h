// camera.h — the 3D spacetime overview's orbit camera, as PURE MATH
// (docs/internal/gui/10-spacetime-3d-overview.md T4 step 4). No GL, no ImGui, no
// engine: just linmath.h (the pinned WTFPL single-header math lib, 10-T4 step 1),
// so the whole camera is exercised headlessly by test_camera.cpp under the null
// harness — the same engine-free closure the pure space/ layers make.
//
// The camera orbits a fixed target (the plane centre) on a sphere parameterised
// by (yaw, pitch, radius). Mouse-drag orbits, the wheel dollies, and two presets
// exist: reset() (the default three-quarter view) and top_down() — the plain
// "2D-ish" fallback that collapses the scene to the classic memory-map heatmap
// when depth confuses (the brief's rule: 3D to find, 2D to read).
//
// Matrices are column-major float[16] (linmath's mat4x4 layout == OpenGL's), so
// view()/proj()/mvp() drop straight into a glUniformMatrix4fv(..., GL_FALSE, ...).
#ifndef ASMDESK_SCENE3D_CAMERA_H
#define ASMDESK_SCENE3D_CAMERA_H

#include <cmath>
#include <cstring>

#include "linmath.h"

namespace asmdesk::scene3d {

// The unit plane lives in XZ over [0,1]^2 (terrain.vert places p = (u, h, v)), so
// the default target is its centre and the default eye looks down at it from a
// three-quarter angle.
struct Camera {
    float yaw = 0.7f;    // radians, azimuth about the target's Y axis
    float pitch = 0.6f;  // radians, elevation above the plane (clamped below)
    float radius = 2.2f; // distance from target to eye
    float target[3] = {0.5f, 0.0f, 0.5f};

    float fovy = 0.8f;   // vertical field of view, radians (brief: 0.8)
    float znear = 0.05f; // brief: 0.05 .. 50
    float zfar = 50.0f;

    // Dolly clamps (a camera that dollies to 0 or through the far plane is a bug,
    // not a view). Public so a caller/test can read the range it will be held to.
    static constexpr float kMinRadius = 0.3f;
    static constexpr float kMaxRadius = 12.0f;
    // Pitch is held just shy of the poles: at exactly +/-pi/2 the eye is colinear
    // with the world up and look_at degenerates. top_down() rides this ceiling.
    static constexpr float kPitchLimit = 1.5533f; // ~89 degrees

    static float clampf(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // Mouse-drag: dyaw/dpitch already scaled to radians by the caller.
    void orbit(float dyaw, float dpitch) {
        yaw += dyaw;
        pitch = clampf(pitch + dpitch, -kPitchLimit, kPitchLimit);
    }
    // Wheel: multiplicative dolly (a fixed fraction per notch feels linear in
    // screen terms), clamped to the working range.
    void dolly(float factor) {
        radius = clampf(radius * factor, kMinRadius, kMaxRadius);
    }

    void reset() { *this = Camera{}; }
    // The 2D-ish preset: look straight down (pitch at the ceiling, so the plane
    // reads as a flat heatmap) from a radius that frames the whole unit plane.
    void top_down() {
        yaw = 0.0f;
        pitch = kPitchLimit;
        radius = 1.9f;
    }

    // Eye position on the orbit sphere. pitch is elevation, so a higher pitch
    // lifts the eye in +Y; yaw sweeps it around the target in the XZ plane.
    void eye(float out[3]) const {
        float cp = std::cos(pitch), sp = std::sin(pitch);
        out[0] = target[0] + radius * cp * std::sin(yaw);
        out[1] = target[1] + radius * sp;
        out[2] = target[2] + radius * cp * std::cos(yaw);
    }

    void view(float m[16]) const {
        float e[3];
        eye(e);
        vec3 up = {0.0f, 1.0f, 0.0f};
        mat4x4 v;
        mat4x4_look_at(v, e, const_cast<float *>(target), up);
        std::memcpy(m, v, sizeof(v));
    }

    void proj(float m[16], float aspect) const {
        mat4x4 p;
        // Guard a degenerate aspect (a zero-height framebuffer during a resize).
        float a = aspect > 0.0f ? aspect : 1.0f;
        mat4x4_perspective(p, fovy, a, znear, zfar);
        std::memcpy(m, p, sizeof(p));
    }

    // MVP for a model at the identity: proj * view. (The terrain/trajectory
    // geometry is authored in world space, so there is no separate model matrix.)
    void mvp(float m[16], float aspect) const {
        mat4x4 v, p, r;
        view(reinterpret_cast<float *>(v));
        proj(reinterpret_cast<float *>(p), aspect);
        mat4x4_mul(r, p, v);
        std::memcpy(m, r, sizeof(r));
    }
};

// A keyboard camera nudge (22-selection-and-search.md T2, F18). ImGui exposes no
// OS screen-reader tree, so keyboard operability is the ONLY accessibility
// substitute — yet the 3D HUD had no keyboard camera even designed. These are the
// intents a focused 3D pane maps its keys onto; `camera_key` applies each through
// the SAME orbit/dolly/reset/top_down the mouse drag uses, so keyboard and mouse
// are one code path and the top-down fallback stays the plain "3D to find, 2D to
// read" collapse. Pure — no ImGui — so test_camera drives it and the HUD only
// maps ImGuiKey -> CamKey.
enum class CamKey {
    OrbitLeft,
    OrbitRight,
    OrbitUp,
    OrbitDown,
    DollyIn,
    DollyOut,
    Reset,
    TopDown,
};

inline void camera_key(Camera &c, CamKey k) {
    // One orbit step ~ a comfortable arrow-key nudge; one dolly step matches the
    // mouse wheel's multiplicative notch (draw_scene_overview: 1/1.1 in, 1.1 out).
    constexpr float kOrbitStep = 0.12f;
    constexpr float kDollyIn = 1.0f / 1.1f;
    constexpr float kDollyOut = 1.1f;
    switch (k) {
    case CamKey::OrbitLeft:
        c.orbit(-kOrbitStep, 0.0f);
        break;
    case CamKey::OrbitRight:
        c.orbit(+kOrbitStep, 0.0f);
        break;
    case CamKey::OrbitUp:
        c.orbit(0.0f, +kOrbitStep);
        break;
    case CamKey::OrbitDown:
        c.orbit(0.0f, -kOrbitStep);
        break;
    case CamKey::DollyIn:
        c.dolly(kDollyIn);
        break;
    case CamKey::DollyOut:
        c.dolly(kDollyOut);
        break;
    case CamKey::Reset:
        c.reset();
        break;
    case CamKey::TopDown:
        c.top_down();
        break;
    }
}

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_CAMERA_H
