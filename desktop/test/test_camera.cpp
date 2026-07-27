// test_camera.cpp — the orbit camera's PURE MATH (docs/internal/gui/
// 10-spacetime-3d-overview.md T4 step 4). Null harness, no display, no GL: this
// binary links NOTHING but the header-only camera (and linmath.h), which is the
// proof the camera is engine- and GL-free and can be reasoned about headlessly.
//
// The properties pinned: the eye rides a sphere of the given radius about the
// target; orbit clamps pitch off the poles and dolly clamps radius to its working
// range; reset and the top-down (2D-ish) preset land where they claim to; the
// view matrix carries the eye to the origin; and the full MVP projects the
// look-at target to the centre of the screen (NDC ~ 0,0) — an end-to-end matrix
// check with no GL in sight.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "scene3d/camera.h"

using asmdesk::scene3d::Camera;

static int failures;

static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

static float dist(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Column-major mat4 (m[col*4+row]) times a vec4 -> out vec4.
static void mul_v4(const float m[16], const float v[4], float out[4]) {
    for (int r = 0; r < 4; r++)
        out[r] = m[0 * 4 + r] * v[0] + m[1 * 4 + r] * v[1] +
                 m[2 * 4 + r] * v[2] + m[3 * 4 + r] * v[3];
}

int main() {
    // === the eye rides a sphere of `radius` about the target =================
    {
        Camera c;
        float e[3];
        c.eye(e);
        check("eye is at radius from target",
              std::fabs(dist(e, c.target) - c.radius) < 1e-4f,
              "eye-target distance != radius");
    }

    // === orbit clamps pitch off the poles ====================================
    {
        Camera c;
        c.orbit(0.0f, +100.0f); // way past the pole
        check("pitch clamped below +pi/2",
              c.pitch <= Camera::kPitchLimit + 1e-6f,
              "pitch exceeded the ceiling");
        c.orbit(0.0f, -100.0f);
        check("pitch clamped above -pi/2",
              c.pitch >= -Camera::kPitchLimit - 1e-6f, "pitch under the floor");
        float y0 = c.yaw;
        c.orbit(0.5f, 0.0f);
        check("orbit advances yaw", std::fabs((c.yaw - y0) - 0.5f) < 1e-6f,
              "yaw did not advance");
    }

    // === dolly clamps radius to its working range ============================
    {
        Camera c;
        for (int i = 0; i < 100; i++)
            c.dolly(0.5f); // repeatedly halve
        check("dolly floors radius", c.radius >= Camera::kMinRadius - 1e-6f,
              "radius fell below the floor");
        for (int i = 0; i < 100; i++)
            c.dolly(2.0f); // repeatedly double
        check("dolly ceils radius", c.radius <= Camera::kMaxRadius + 1e-6f,
              "radius rose above the ceiling");
    }

    // === reset restores the default view =====================================
    {
        Camera c;
        c.orbit(1.0f, 0.3f);
        c.dolly(1.7f);
        c.reset();
        Camera d;
        check("reset restores yaw/pitch/radius",
              c.yaw == d.yaw && c.pitch == d.pitch && c.radius == d.radius,
              "reset did not restore defaults");
    }

    // === top-down (2D-ish) preset: eye straight above the target =============
    {
        Camera c;
        c.top_down();
        float e[3];
        c.eye(e);
        check("top-down eye is above the target", e[1] > c.target[1] + 0.5f,
              "eye not lifted above the plane");
        check("top-down eye is over the target in X",
              std::fabs(e[0] - c.target[0]) < 0.05f, "eye drifted in X");
        check("top-down eye is over the target in Z",
              std::fabs(e[2] - c.target[2]) < 0.05f, "eye drifted in Z");
    }

    // === the view matrix carries the eye to the origin =======================
    {
        Camera c;
        float e[3];
        c.eye(e);
        float m[16];
        c.view(m);
        float ev[4] = {e[0], e[1], e[2], 1.0f}, out[4];
        mul_v4(m, ev, out);
        check("view maps the eye to the origin",
              std::fabs(out[0]) < 1e-3f && std::fabs(out[1]) < 1e-3f &&
                  std::fabs(out[2]) < 1e-3f,
              "eye did not map to the origin in view space");
    }

    // === the MVP projects the target to screen centre (NDC ~ 0,0) ============
    {
        Camera c;
        float m[16];
        c.mvp(m, 16.0f / 9.0f);
        float tv[4] = {c.target[0], c.target[1], c.target[2], 1.0f}, clip[4];
        mul_v4(m, tv, clip);
        check("target is in front of the camera (w > 0)", clip[3] > 0.0f,
              "target behind the near plane");
        if (clip[3] > 0.0f) {
            float ndc_x = clip[0] / clip[3], ndc_y = clip[1] / clip[3];
            check("target projects to screen centre in X",
                  std::fabs(ndc_x) < 1e-4f, "ndc.x not centred");
            check("target projects to screen centre in Y",
                  std::fabs(ndc_y) < 1e-4f, "ndc.y not centred");
        }
    }

    // === the KEYBOARD camera (22-selection-and-search.md T2, F18) =============
    // camera_key routes each key intent through the SAME orbit/dolly/reset/top_down
    // the mouse uses, so keyboard and mouse are one code path — and it is pure math
    // (no ImGui, no GL), which is what makes the keyboard camera headlessly
    // testable at all (CLAUDE.md: a lane that could only self-skip is not a test).
    using asmdesk::scene3d::camera_key;
    using asmdesk::scene3d::CamKey;
    {
        // Orbit keys move yaw/pitch, and pitch stays clamped off the poles.
        Camera c;
        float y0 = c.yaw, p0 = c.pitch;
        camera_key(c, CamKey::OrbitRight);
        check("key/orbit-right advances yaw", c.yaw > y0, "yaw did not advance");
        camera_key(c, CamKey::OrbitLeft);
        camera_key(c, CamKey::OrbitLeft);
        check("key/orbit-left retreats yaw", c.yaw < y0, "yaw did not retreat");
        camera_key(c, CamKey::OrbitUp);
        check("key/orbit-up raises pitch", c.pitch > p0, "pitch did not rise");
        for (int i = 0; i < 100; i++)
            camera_key(c, CamKey::OrbitUp); // way past the pole
        check("key/orbit clamps pitch", c.pitch <= Camera::kPitchLimit + 1e-6f,
              "keyboard orbit must honour the pitch clamp");
    }
    {
        // Dolly keys move radius within its working range.
        Camera c;
        float r0 = c.radius;
        camera_key(c, CamKey::DollyIn);
        check("key/dolly-in shortens radius", c.radius < r0, "radius did not shrink");
        camera_key(c, CamKey::DollyOut);
        camera_key(c, CamKey::DollyOut);
        check("key/dolly-out lengthens radius", c.radius > r0, "radius did not grow");
        for (int i = 0; i < 100; i++)
            camera_key(c, CamKey::DollyOut);
        check("key/dolly clamps radius", c.radius <= Camera::kMaxRadius + 1e-6f,
              "keyboard dolly must honour the radius clamp");
    }
    {
        // R resets to the default pose; T is the honest top-down 2D-ish fallback.
        Camera c;
        c.orbit(1.0f, 0.3f);
        c.dolly(1.7f);
        camera_key(c, CamKey::Reset);
        Camera d;
        check("key/R resets the view",
              c.yaw == d.yaw && c.pitch == d.pitch && c.radius == d.radius,
              "R did not restore the default pose");
        camera_key(c, CamKey::TopDown);
        check("key/T yields the top-down pose",
              c.pitch >= Camera::kPitchLimit - 1e-6f,
              "T must ride the pitch ceiling (the 2D-ish collapse)");
    }

    if (failures) {
        std::fprintf(stderr, "%d camera check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_camera: all checks passed\n");
    return 0;
}
