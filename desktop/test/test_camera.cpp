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
//
// 51 T4 (scene-focus-and-scale.md) adds the camera-distance ENTITY BUDGET to
// this file, because the tier is a function of Camera::radius and belongs
// beside the axis it reads. scene3d/lod.h is header-only over a POD, so this
// binary still links nothing but its own object.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "scene3d/camera.h"
#include "scene3d/lod.h"

using asmdesk::scene3d::Camera;
using asmdesk::scene3d::LodTier;
using asmdesk::scene3d::SceneLayers;

static int failures;

static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

// 51 T4: a std::string overload, so a failing LOD check can quote the placard
// text it actually read back.
static void check(const std::string &what, bool cond, const std::string &why) {
    check(what.c_str(), cond, why.c_str());
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

    // === 51 T4 — the camera-distance entity budget ===========================
    using asmdesk::scene3d::kLodEntityBudget;
    using asmdesk::scene3d::kLodFarRadius;
    using asmdesk::scene3d::kLodMidRadius;
    using asmdesk::scene3d::lod_apply;
    using asmdesk::scene3d::lod_dropped;
    using asmdesk::scene3d::lod_drops_unfocused;
    using asmdesk::scene3d::lod_placard;
    using asmdesk::scene3d::lod_tier;
    using asmdesk::scene3d::lod_tier_name;
    const uint64_t dense = kLodEntityBudget + 1;

    {
        // An entity count UNDER budget is always NEAR, however far the camera
        // stands off: distance alone is not a reason to draw less.
        for (float r = Camera::kMinRadius; r <= Camera::kMaxRadius; r += 0.25f)
            check("lod: under budget is always NEAR",
                  lod_tier(r, kLodEntityBudget) == LodTier::Near,
                  "a sparse frame degraded at radius");
        check("lod: an empty frame is NEAR",
              lod_tier(Camera::kMaxRadius, 0) == LodTier::Near,
              "nothing on the plane must never degrade");
    }
    {
        // Exact boundaries: the thresholds are inclusive at the lower edge, so
        // radius == kLodMidRadius is already MID.
        check("lod: just inside the MID threshold is still NEAR",
              lod_tier(kLodMidRadius - 0.01f, dense) == LodTier::Near,
              "MID engaged early");
        check("lod: at the MID threshold the tier is MID",
              lod_tier(kLodMidRadius, dense) == LodTier::Mid,
              "MID did not engage at its own threshold");
        check("lod: just inside the FAR threshold is still MID",
              lod_tier(kLodFarRadius - 0.01f, dense) == LodTier::Mid,
              "FAR engaged early");
        check("lod: at the FAR threshold the tier is FAR",
              lod_tier(kLodFarRadius, dense) == LodTier::Far,
              "FAR did not engage at its own threshold");
    }
    {
        // Monotonic in radius: dollying OUT never re-adds a class, so a drop
        // cannot flicker back in mid-gesture.
        int last = 0;
        bool monotonic = true;
        for (float r = Camera::kMinRadius; r <= Camera::kMaxRadius;
             r += 0.05f) {
            const int t = static_cast<int>(lod_tier(r, dense));
            if (t < last)
                monotonic = false;
            last = t;
        }
        check("lod: the tier is monotonic in radius", monotonic,
              "dollying out re-added a class");
    }
    {
        // NEAR draws exactly what was requested — byte-identical layers.
        SceneLayers want;
        want.access_marks = true;
        const SceneLayers got = lod_apply(want, LodTier::Near);
        check("lod: NEAR is byte-identical to the request",
              std::memcmp(&got, &want, sizeof want) == 0,
              "NEAR changed a layer");
        check("lod: NEAR discloses nothing",
              lod_placard(want, LodTier::Near, false).empty(),
              "a placard appeared with nothing dropped");
        check("lod: NEAR drops no class",
              lod_dropped(want, LodTier::Near, true).empty(),
              "NEAR reported a drop");
    }
    {
        // MID: the two low-weight classes, and non-subject worldlines ONLY
        // when a subject was actually chosen.
        SceneLayers want;
        const SceneLayers mid = lod_apply(want, LodTier::Mid);
        check("lod: MID drops the access spurs", !mid.access_marks,
              "the densest overlay survived MID");
        check("lod: MID keeps the terrain and the exact paths",
              mid.terrain && mid.exact, "MID removed the plane or its paths");
        check("lod: MID keeps the convergence arcs (the cross-thread finding)",
              mid.convergence, "MID dropped a high-weight class");
        check("lod: MID drops non-subject worldlines when a thread is focused",
              lod_drops_unfocused(LodTier::Mid, true),
              "a focused MID frame kept every worldline");
        check("lod: with NO subject chosen no worldline is 'non-subject'",
              !lod_drops_unfocused(LodTier::Mid, false) &&
                  !lod_drops_unfocused(LodTier::Far, false),
              "the budget chose a subject on the reader's behalf");
    }
    {
        SceneLayers want;
        const SceneLayers far = lod_apply(want, LodTier::Far);
        check("lod: FAR keeps the plane itself", far.terrain,
              "FAR removed the terrain — there would be nothing to read");
        check("lod: FAR drops the survey overlays",
              !far.statistical && !far.ghost_fog && !far.mispred,
              "a statistical overlay survived FAR");
        check("lod: FAR drops the module canopies and the vehicle",
              !far.canopy && !far.vehicle, "an overlay survived FAR");
        // T4 step 3 — PROVENANCE SURVIVES LOD, in its strongest structural
        // form: no tier may ever leave the statistical stipple on screen
        // WITHOUT the exact paths it could be mistaken for. Two ways of
        // drawing less must not converge on one appearance.
        for (LodTier t : {LodTier::Near, LodTier::Mid, LodTier::Far}) {
            const SceneLayers got = lod_apply(want, t);
            check(std::string("lod: ") + lod_tier_name(t) +
                      " never keeps the survey while dropping the exact paths",
                  !(got.statistical && !got.exact),
                  "the stipple would be the only path-like mark on screen");
        }
    }
    {
        // The placard: every dropped class is NAMED. A silent drop reads as
        // "there was nothing there" — the failure the degrade idiom exists to
        // prevent, so it is a test, not a guideline.
        SceneLayers want;
        const std::string mid = lod_placard(want, LodTier::Mid, true);
        check("lod: the MID placard names the tier",
              mid.rfind("MID", 0) == 0, "placard was: " + mid);
        check("lod: the MID placard names the dropped spurs",
              mid.find("access spurs") != std::string::npos,
              "placard was: " + mid);
        check("lod: the MID placard names the dropped worldlines",
              mid.find("non-subject worldlines") != std::string::npos,
              "placard was: " + mid);
        check("lod: the MID placard says how to get the data back",
              mid.find("Dolly in") != std::string::npos,
              "placard was: " + mid);
        check("lod: the MID placard states exact provenance survives",
              mid.find("never redrawn in the statistical stipple") !=
                  std::string::npos,
              "placard was: " + mid);

        const std::string far = lod_placard(want, LodTier::Far, false);
        check("lod: the FAR placard names the tier", far.rfind("FAR", 0) == 0,
              "placard was: " + far);
        for (const char *cls :
             {"access spurs", "statistical residency paths",
              "ghost-fog survey terrain", "module canopies",
              "misprediction survey", "convergence arcs",
              "followed-citizen head"})
            check(std::string("lod: the FAR placard names '") + cls + "'",
                  far.find(cls) != std::string::npos, "placard was: " + far);
        check("lod: with no subject, FAR does not claim a worldline drop",
              far.find("non-subject worldlines") == std::string::npos,
              "placard was: " + far);

        // A class the READER already switched off is not a budget drop, and
        // must not be reported as one (the two causes must stay tellable
        // apart).
        SceneLayers off;
        off.access_marks = false;
        off.convergence = false;
        const std::string p = lod_placard(off, LodTier::Far, false);
        check("lod: an already-off class is not reported as a budget drop",
              p.find("access spurs") == std::string::npos &&
                  p.find("convergence arcs") == std::string::npos,
              "placard was: " + p);
    }

    if (failures) {
        std::fprintf(stderr, "%d camera check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_camera: all checks passed\n");
    return 0;
}
