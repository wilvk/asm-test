// test_scene_traj.cpp — the worldline's vertical scale rule. Null harness, no
// GL: this binary links nothing but the header-only scene3d/trajscale.h, on
// the same terms as test_camera.cpp's citizens.
#include <cmath>
#include <cstdio>
#include <string>

#include "scene3d/trajscale.h"

using asmdesk::scene3d::scene_traj_scale;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL: %s (%s)\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

int main() {
    // A `mem` stream can outlast the trace's own step count — the same mismatch
    // space/sediment.cpp:33-37 already guards. The worldline must not escape
    // its 0.6 world-unit envelope when it does.
    {
        const float scale = scene_traj_scale(/*nsteps=*/10, /*max_t=*/100000,
                                             /*time_scale=*/0.0f);
        const float top = 100000.0f * scale;
        check("a mem step past nsteps stays inside the 0.6 envelope",
              top <= 0.6f + 1e-4f,
              "top of the worldline reached " + std::to_string(top));
    }
    // The ordinary case is unchanged: nsteps covers every step, so the path
    // still tops out AT 0.6 rather than being needlessly compressed.
    {
        const float scale = scene_traj_scale(1000, 999, 0.0f);
        check("a well-formed trace still spans the full envelope",
              std::fabs(999.0f * scale - 0.6f) < 0.01f,
              "top was " + std::to_string(999.0f * scale) + ", wanted ~0.6");
    }
    // nsteps == 0 must NOT fall back to a fixed per-step constant: at the
    // golden's 35560 steps the old 0.02f produced 711 world units, ~300x the
    // camera radius and past zfar.
    {
        const float scale = scene_traj_scale(0, 35560, 0.0f);
        const float top = 35560.0f * scale;
        check("nsteps == 0 does not blow the envelope", top <= 0.6f + 1e-4f,
              "top of the worldline reached " + std::to_string(top));
    }
    // An explicit time_scale is still honoured verbatim — callers that set it
    // are stating a scale, not asking for one.
    check("an explicit time_scale is passed through",
          scene_traj_scale(1000, 999, 0.25f) == 0.25f, "explicit scale ignored");

    if (failures) {
        std::fprintf(stderr, "%d scene_traj check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_scene_traj: all checks passed\n");
    return 0;
}
