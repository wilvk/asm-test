// test_camera.cpp — the orbit camera's PURE MATH (docs/internal/gui/
// 10-spacetime-3d-overview.md T4 step 4). Null harness, no display, no GL: this
// binary links NOTHING but the header-only camera (and linmath.h), which is the
// proof the camera is engine- and GL-free and can be reasoned about headlessly.
// 55 T6 step 1 added a second header-only citizen on the same terms —
// scene3d/linequad.h, the CPU half of the portable line width — for the same
// reason: geometry that can be checked with no context at all should be.
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
#include <vector>

#include "scene3d/camera.h"
#include "scene3d/linequad.h" // T6 (55) step 1: the pure quad expansion

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
        // R resets to the default pose; T is the plain top-down 2D-ish fallback.
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

    // === 48 T1: Camera::pan clamps target to [0,1] on both axes ==============
    {
        Camera c;
        for (int i = 0; i < 100; i++)
            c.pan(-1.0f, -1.0f);
        check("pan floors target[0]", c.target[0] >= 0.0f - 1e-6f,
              "target[0] fell below 0");
        check("pan floors target[2]", c.target[2] >= 0.0f - 1e-6f,
              "target[2] fell below 0");
        for (int i = 0; i < 100; i++)
            c.pan(+1.0f, +1.0f);
        check("pan ceils target[0]", c.target[0] <= 1.0f + 1e-6f,
              "target[0] rose above 1");
        check("pan ceils target[2]", c.target[2] <= 1.0f + 1e-6f,
              "target[2] rose above 1");
        check("pan never moves target[1] off the plane",
              c.target[1] == 0.0f, "target[1] drifted off the plane's height");
    }

    // === 48 T1: pan and orbit are commutative on the target ===================
    // orbit() touches only yaw/pitch and pan() touches only target — this locks
    // that separation in so a future change cannot quietly couple them.
    {
        Camera a, b;
        a.pan(0.2f, -0.15f);
        a.orbit(0.4f, -0.2f);
        b.orbit(0.4f, -0.2f);
        b.pan(0.2f, -0.15f);
        check("pan-then-orbit == orbit-then-pan (target)",
              a.target[0] == b.target[0] && a.target[1] == b.target[1] &&
                  a.target[2] == b.target[2],
              "pan and orbit must not interact on the target");
        check("pan-then-orbit == orbit-then-pan (yaw/pitch)",
              a.yaw == b.yaw && a.pitch == b.pitch,
              "pan must never touch yaw/pitch");
    }

    // === 48 T1: Camera::frame moves the target, clamps radius, and leaves
    // yaw/pitch bit-identical (a recentre must not also reorient) ==============
    {
        Camera c;
        float y0 = c.yaw, p0 = c.pitch;
        c.frame(0.25f, 0.75f, 5.0f);
        check("frame sets target[0]", c.target[0] == 0.25f, "target[0] wrong");
        check("frame sets target[2]", c.target[2] == 0.75f, "target[2] wrong");
        check("frame leaves target[1] at the plane", c.target[1] == 0.0f,
              "target[1] must stay 0");
        check("frame leaves yaw bit-identical", c.yaw == y0,
              "frame must not reorient (yaw moved)");
        check("frame leaves pitch bit-identical", c.pitch == p0,
              "frame must not reorient (pitch moved)");
        check("frame sets radius", c.radius == 5.0f, "radius not applied");
        c.frame(0.5f, 0.5f, 1000.0f);
        check("frame clamps radius to the ceiling",
              c.radius <= Camera::kMaxRadius + 1e-6f,
              "frame must honour kMaxRadius");
        c.frame(0.5f, 0.5f, -1000.0f);
        check("frame clamps radius to the floor",
              c.radius >= Camera::kMinRadius - 1e-6f,
              "frame must honour kMinRadius");
    }

    // === 48 T1: reset() still returns a panned/framed camera to the default ===
    {
        Camera c;
        c.pan(0.3f, -0.2f);
        c.frame(0.9f, 0.1f, 7.0f);
        c.reset();
        Camera d;
        check("reset restores target after pan/frame",
              c.target[0] == d.target[0] && c.target[1] == d.target[1] &&
                  c.target[2] == d.target[2],
              "reset must fully restore the default target");
    }

    // === 48 T1: the keyboard pan keys apply the SAME deltas Camera::pan does ==
    {
        Camera viaKey, viaCall;
        camera_key(viaKey, CamKey::PanRight);
        viaCall.pan(0.04f, 0.0f); // kPanStep, mirrored (see camera.h)
        check("key/pan-right matches Camera::pan",
              viaKey.target[0] == viaCall.target[0] &&
                  viaKey.target[2] == viaCall.target[2],
              "PanRight must apply the documented kPanStep");
    }
    {
        Camera c;
        float u0 = c.target[0], v0 = c.target[2];
        camera_key(c, CamKey::PanLeft);
        check("key/pan-left decreases target[0]", c.target[0] < u0,
              "PanLeft did not move target[0] left");
        check("key/pan-left leaves target[2] alone", c.target[2] == v0,
              "PanLeft must be a pure X-axis nudge");
        camera_key(c, CamKey::PanRight);
        camera_key(c, CamKey::PanRight);
        check("key/pan-right increases target[0]", c.target[0] > u0,
              "PanRight did not move target[0] right");
        float u1 = c.target[0];
        camera_key(c, CamKey::PanForward);
        check("key/pan-forward decreases target[2]", c.target[2] < v0,
              "PanForward did not move target[2] forward");
        check("key/pan-forward leaves target[0] alone", c.target[0] == u1,
              "PanForward must be a pure Z-axis nudge");
        camera_key(c, CamKey::PanBack);
        camera_key(c, CamKey::PanBack);
        check("key/pan-back increases target[2]", c.target[2] > v0,
              "PanBack did not move target[2] back");
    }
    {
        // Keyboard pan clamps exactly like the mouse path (Camera::pan itself).
        Camera c;
        for (int i = 0; i < 200; i++)
            camera_key(c, CamKey::PanLeft);
        check("key/pan-left clamps at the floor", c.target[0] >= 0.0f - 1e-6f,
              "keyboard pan must honour the [0,1] clamp");
        for (int i = 0; i < 200; i++)
            camera_key(c, CamKey::PanRight);
        check("key/pan-right clamps at the ceiling", c.target[0] <= 1.0f + 1e-6f,
              "keyboard pan must honour the [0,1] clamp");
    }

    // === T6 (55-scene-render-quality) step 1: the quad expansion's pure ====
    // === maths -- the half that has to be right before any pixel can be. ===
    //
    // The brief asks for exactly this here ("test_camera.cpp covers the pure
    // maths of the expansion if it is factored out (it should be)"), and it
    // calls the SHIPPED function (scene3d/linequad.h, which scene.cpp uses to
    // build its buffers), never a re-derivation of it.
    {
        using asmdesk::scene3d::expand_line_quads;
        using asmdesk::scene3d::kQuadStrideFloats;
        using asmdesk::scene3d::kQuadVertsPerSegment;
        using asmdesk::scene3d::line_quad_segments;

        // A 3-vertex strip -> 2 segments; the same 3 vertices as disjoint
        // pairs -> 1 segment (the odd vertex has no partner and is dropped,
        // exactly as GL_LINES itself drops it).
        const std::vector<float> pts = {0, 0, 0, 1, 0, 0, 1, 0, 1};
        std::vector<float> strip, pairs;
        expand_line_quads(pts, /*strip=*/true, strip);
        expand_line_quads(pts, /*strip=*/false, pairs);
        check("linequad: a strip of 3 vertices expands to 2 segments",
              strip.size() == 2u * kQuadVertsPerSegment * kQuadStrideFloats,
              "wrong vertex count for a 3-vertex strip");
        check("linequad: 3 vertices as PAIRS expand to 1 segment",
              pairs.size() == 1u * kQuadVertsPerSegment * kQuadStrideFloats,
              "an unpaired trailing vertex must not invent a segment");
        check("linequad: line_quad_segments agrees with the expansion",
              line_quad_segments(3, true) == 2 &&
                  line_quad_segments(3, false) == 1 &&
                  line_quad_segments(1, true) == 0,
              "the segment count a caller sizes its id array with is wrong");

        // Every corner carries BOTH endpoints and a unit side, and the six
        // corners of one segment use each (endpoint, side) combination the
        // shader needs: two at A (+1, -1) and two at B (+1, -1), with the
        // shared corners appearing twice because the two triangles share an
        // edge.
        int at_a[2] = {0, 0}, at_b[2] = {0, 0};
        for (size_t v = 0; v + kQuadStrideFloats <= pairs.size();
             v += kQuadStrideFloats) {
            const float *c = &pairs[v];
            const float side = c[6];
            check("linequad: side is exactly +1 or -1",
                  side == 1.0f || side == -1.0f,
                  "a corner carried a side that is not a unit sign");
            const bool is_a = (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f);
            const bool partner_ok =
                is_a ? (c[3] == 1.0f && c[4] == 0.0f && c[5] == 0.0f)
                     : (c[3] == 0.0f && c[4] == 0.0f && c[5] == 0.0f);
            check("linequad: a corner's `other` is the segment's far end",
                  partner_ok, "a corner named the wrong partner endpoint");
            (is_a ? at_a : at_b)[side > 0 ? 0 : 1]++;
        }
        check("linequad: both sides are emitted at both endpoints",
              at_a[0] > 0 && at_a[1] > 0 && at_b[0] > 0 && at_b[1] > 0,
              "a quad missing one of its four corners cannot be a rectangle");

        // The bowtie guard, and the one thing a reader of linequad.h is most
        // likely to get wrong: because the shader derives the perpendicular
        // from (other - pos), the perpendicular at B points the OPPOSITE way
        // to the one at A -- so the corner physically opposite (A,+1) must be
        // recorded as (B,+1), not (B,-1). Re-derive that here by applying the
        // shader's own screen-space rule (mirrored below, the same convention
        // test_scene_fbo already uses for kTrajFrag's ign()) rather than
        // trusting linequad.h's comment.
        auto screen_offset = [](float px, float py, float ox, float oy, float w,
                                float side, float *out_x, float *out_y) {
            float dx = ox - px, dy = oy - py;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1e-6f) {
                dx /= len;
                dy /= len;
            } else {
                dx = 1.0f;
                dy = 0.0f;
            }
            const float hw = w * 0.5f;
            *out_x = px + (-dy) * hw * side - dx * hw;
            *out_y = py + (dx)*hw * side - dy * hw;
        };
        // Segment A=(0,0) -> B=(4,0) in screen pixels, width 2.
        float ax_p, ay_p, ax_m, ay_m, bx_p, by_p;
        screen_offset(0, 0, 4, 0, 2.0f, +1.0f, &ax_p, &ay_p);
        screen_offset(0, 0, 4, 0, 2.0f, -1.0f, &ax_m, &ay_m);
        screen_offset(4, 0, 0, 0, 2.0f, +1.0f, &bx_p, &by_p);
        check("linequad: half the width lands on each side of the centreline",
              std::fabs(ay_p - 1.0f) < 1e-5f && std::fabs(ay_m + 1.0f) < 1e-5f,
              "a width-2 line must reach 1px either side of its centreline");
        check("linequad: (B,+1) is the corner OPPOSITE (A,+1)",
              (ay_p > 0.0f) != (by_p > 0.0f),
              "the side sign does not invert at the far endpoint -- the quad "
              "would be a bowtie");
        check("linequad: the square cap extends half a width past each end",
              std::fabs(ax_p + 1.0f) < 1e-5f && std::fabs(bx_p - 5.0f) < 1e-5f,
              "the cap that closes a polyline's joins is missing or wrong");
        check("linequad: the expanded width is exactly the width asked for",
              std::fabs((ay_p - ay_m) - 2.0f) < 1e-5f,
              "a width-2 line did not span 2 screen pixels across");

        // A degenerate (zero-length) segment must not normalize a zero vector.
        float dx0 = 0.0f, dy0 = 0.0f;
        screen_offset(2, 2, 2, 2, 3.0f, +1.0f, &dx0, &dy0);
        check("linequad: a zero-length segment produces finite corners",
              std::isfinite(dx0) && std::isfinite(dy0),
              "a degenerate segment produced NaN/inf instead of collapsing to "
              "a dot");
    }

    if (failures) {
        std::fprintf(stderr, "%d camera check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_camera: all checks passed\n");
    return 0;
}
