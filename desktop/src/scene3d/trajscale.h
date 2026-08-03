// trajscale.h — the worldline's world-Y per trace step, as PURE ARITHMETIC.
// Header-only and dependency-free (no GL, no ImGui, no linmath), on the same
// terms as camera.h and linequad.h, so test_scene_traj.cpp links nothing but
// its own object and the rule is checkable with no context at all.
#ifndef ASMDESK_SCENE3D_TRAJSCALE_H
#define ASMDESK_SCENE3D_TRAJSCALE_H

#include <algorithm>
#include <cstdint>
#include <utility>

namespace asmdesk::scene3d {

// `max_t` is the largest TrajPoint.t actually present. `nsteps` is the
// TERRAIN's extent and can UNDER-REPORT it — a `mem` stream can outlast the
// trace's own step count (the same mismatch space/sediment.cpp:33-37 already
// guards, with the same max()). EXTENDING the denominator rather than clamping
// the data is what keeps every real step on the axis: a step past the stated
// extent is still a real step. A positive `time_scale` is a caller STATING a
// scale and is returned verbatim.
inline float scene_traj_scale(uint32_t nsteps, uint64_t max_t,
                              float time_scale) {
    if (time_scale > 0.0f)
        return time_scale;
    const uint64_t top = std::max<uint64_t>(nsteps, max_t + 1);
    return top > 0 ? 0.6f / static_cast<float>(top) : 0.6f;
}

// 61 T5: how far a FLAT worldline rides above the ground plane, in world units.
// A path at y == 0 is coplanar with the terrain floor: it z-fights, and it
// disappears entirely under any cell whose height exceeds zero. This is a
// LEGIBILITY constant and encodes NOTHING — do not read a magnitude into it.
// Sized against the terrain's own y_scale (0.35 by default) so it clears the
// low-heat cells the path most often crosses while staying far below a hot
// column, which the path should read as running BEHIND rather than OVER.
//
// Here rather than in scene.cpp because causal.cpp lifts the spur feet by the
// same amount: the spur hangs on a worldline vertex, so two constants would be
// two chances for the foot to detach from the path it points at.
inline constexpr float kFlatPathLift = 0.012f;

// 61 T5: a worldline vertex's world Y. With `flat`, trace time is NOT a spatial
// axis — the path lies on the floor and is read through the playhead — so this
// is 0 and the caller lifts the whole path clear of the terrain by a constant.
//
// The !flat branch is NOT "for the other scenes": no other scene reaches this
// code (gl_scene_host.cpp routes every non-Plane kind to standalone_). It is
// the pre-flattening path, kept testable, and it is the SAME arithmetic the
// three OPT-IN layers still use on their own axis — the lifetime pillars, the
// sediment strata and the access arcs, which keep trace time on Y because a
// layer you switch on is asking for it.
//
// The causal spur foot calls THIS with the same `flat` the path uses, never
// traj_scale_ directly, so scene.h's "a spur hangs on a worldline vertex at
// world Y = t * traj_scale()" stays true by construction instead of by two call
// sites agreeing.
inline float traj_vertex_y(uint64_t t, float scale, bool flat) {
    return flat ? 0.0f : static_cast<float>(t) * scale;
}

// 61 T5: the trail window [lo, hi] ending at the FOLLOWED step (the vehicle's
// own clock — never slice_step, which is the terrain's residency playhead and a
// deliberately separate axis), `tail` steps long and saturating at 0. render()
// draws this window full-bright and fades the rest; nothing is discarded,
// because the path outside the window is real, just not recent.
inline std::pair<uint64_t, uint64_t> comet_window(uint64_t follow_step,
                                                  uint32_t tail) {
    const uint64_t lo = follow_step > tail ? follow_step - tail : 0;
    return {lo, follow_step};
}

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_TRAJSCALE_H
