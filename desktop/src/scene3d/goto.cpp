// goto.cpp — the pure resolvers of goto.h. Standard library + nav.h + the
// space/ models only; no GL, no ImGui, no engine (D4).
#include "scene3d/goto.h"

#include <algorithm>
#include <cmath>

#include "scene3d/camera.h"

namespace asmdesk::scene3d {

bool scene_recentre_target(const space::TerrainModel &terr,
                           const space::TrajectorySet &traj, const Pick &p,
                           float *u, float *v) {
    if (p.kind == Pick::Cell) {
        const uint32_t n = terr.w;
        if (n == 0)
            return false;
        const uint32_t x = p.cell % n, y = p.cell / n;
        if (y >= n)
            return false;
        // Mirrors resolve_pick's own (x+0.5)/n rounding (pick.cpp) — the same
        // cell-centre form, so a recentre lands where the pick itself reads.
        const float uu = (x + 0.5f) / static_cast<float>(n);
        const float vv = (y + 0.5f) / static_cast<float>(n);
        uint64_t addr = 0;
        const space::Region *r = nullptr;
        if (!terr.proj.unproject(uu, vv, &addr, &r) || r == nullptr)
            return false; // padding: no owning region, an explicit no-op
        *u = uu;
        *v = vv;
        return true;
    }
    if (p.kind == Pick::Vertex) {
        // Replay pick_vertex_order's own walk (pick.cpp) in lockstep so index
        // p.vertex names the SAME vertex there and here — the one place this
        // order is defined, never a second derivation.
        uint64_t idx = 0;
        for (const space::Trajectory &tr : traj.trajectories) {
            for (const space::TrajPoint &pt : tr.points) {
                if (pt.is_access)
                    continue;
                if (idx == p.vertex) {
                    // An unplaced vertex (36 T5) has no true cell to recentre
                    // on — its addr is a raw wire offset, not a plane address.
                    if (!pt.placed)
                        return false;
                    return terr.proj.project(pt.addr, u, v);
                }
                idx++;
            }
        }
        return false; // vertex index past the live geometry
    }
    return false; // Pick::None
}

bool scene_goto_addr(const space::Projection &proj, uint64_t addr, float *u,
                     float *v) {
    return proj.project(addr, u, v);
}

bool scene_goto_region(const space::Projection &proj, size_t region_index,
                       float *u, float *v, float *radius) {
    if (region_index >= proj.regions.size())
        return false;
    const space::Region &r = proj.regions[region_index];
    if (r.len == 0)
        return false;

    const size_t n =
        r.len < kGotoRegionSamples ? static_cast<size_t>(r.len)
                                   : kGotoRegionSamples;
    float umin = 2.0f, umax = -1.0f, vmin = 2.0f, vmax = -1.0f;
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        // Evenly spaced across [0, len-1], always including both endpoints —
        // exhaustive (every address) whenever len <= kGotoRegionSamples.
        const uint64_t off =
            n == 1 ? 0
                   : static_cast<uint64_t>(static_cast<double>(i) /
                                          static_cast<double>(n - 1) *
                                          static_cast<double>(r.len - 1));
        float uu = 0.0f, vv = 0.0f;
        if (proj.project(r.base + off, &uu, &vv)) {
            umin = std::min(umin, uu);
            umax = std::max(umax, uu);
            vmin = std::min(vmin, vv);
            vmax = std::max(vmax, vv);
            any = true;
        }
    }
    if (!any)
        return false;

    *u = (umin + umax) * 0.5f;
    *v = (vmin + vmax) * 0.5f;
    // A camera-ergonomics heuristic, not a fidelity claim: a footprint that
    // spans a larger fraction of the plane frames from further back. Scaled
    // against Camera's own working range so it always lands in-bounds.
    const float extent = std::max(umax - umin, vmax - vmin);
    *radius = scene3d::Camera::clampf(0.4f + extent * 3.0f,
                                      scene3d::Camera::kMinRadius,
                                      scene3d::Camera::kMaxRadius);
    return true;
}

bool scene_home_target(const space::TerrainModel &terr, float *u, float *v) {
    double su = 0.0, sv = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < terr.proj.regions.size(); i++) {
        if (terr.proj.regions[i].kind != space::Region::Code)
            continue;
        float ru = 0.0f, rv = 0.0f, rad = 0.0f;
        if (!scene_goto_region(terr.proj, i, &ru, &rv, &rad))
            continue;
        su += ru;
        sv += rv;
        n++;
    }
    if (n == 0)
        return false;
    *u = static_cast<float>(su / static_cast<double>(n));
    *v = static_cast<float>(sv / static_cast<double>(n));
    return true;
}

bool scene_on_screen(const Camera &cam, float u, float v, float aspect,
                     std::string *edge) {
    float m[16];
    cam.mvp(m, aspect);
    // The plane at height 0 (a disclosure check, not a fidelity measurement —
    // see this function's own doc comment in goto.h).
    vec4 world = {u, 0.0f, v, 1.0f};
    vec4 clip;
    mat4x4_mul_vec4(clip, reinterpret_cast<vec4 *>(m), world);
    if (clip[3] <= 0.0f) {
        if (edge)
            *edge = "behind";
        return false;
    }
    const float ndc_x = clip[0] / clip[3];
    const float ndc_y = clip[1] / clip[3];
    const bool on =
        ndc_x >= -1.0f && ndc_x <= 1.0f && ndc_y >= -1.0f && ndc_y <= 1.0f;
    if (!on && edge) {
        if (std::fabs(ndc_x) >= std::fabs(ndc_y))
            *edge = ndc_x < 0.0f ? "left" : "right";
        else
            *edge = ndc_y < 0.0f ? "bottom" : "top";
    }
    return on;
}

} // namespace asmdesk::scene3d
