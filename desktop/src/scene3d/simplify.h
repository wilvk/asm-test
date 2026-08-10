// simplify.h — the 3D pane's simplified reading posture (2026-08-10
// 3d-simplify-and-session-flow spec). The strip's rule, transplanted: the
// model stays complete, the posture decides what is DRAWN, thresholds keep
// small scenes byte-identical, and everything withheld is COUNTED on screen.
//
// Two pure halves, lod.h's shape (header-only, engine-free, testable with no
// context):
//   - simplify_apply gates the per-event SPIKE layers — the encodings that
//     emit one vertical mark per event/cell and read as a spike forest at
//     Firefox scale. It NEVER turns a layer on, and it leaves every
//     aggregate form (terrain, canopy, ridge, convergence, crossings) alone;
//     lod_apply still degrades further on top of its output.
//   - simplify_note is the placard line — non-empty ONLY when something was
//     actually withheld, so a screenshot can never pass a simplified scene
//     off as the whole (the strip HUD's rule).
// The worldline cap itself is space::simplify_trajectories (the weave owns
// the models); this header owns the layer gate and the words.
#ifndef ASMDESK_SCENE3D_SIMPLIFY_H
#define ASMDESK_SCENE3D_SIMPLIFY_H

#include <string>

#include "scene3d/scene.h"       // SceneLayers
#include "space/trajectory.h"    // space::SimplifyNote

namespace asmdesk::scene3d {

// The worldline cap — the strip's kStripSimplifiedLanes, same number, same
// reasoning: eight hue-distinct paths are readable; two hundred are not.
inline constexpr size_t kSceneSimplifiedTrajs = 8;

// The per-event spike layers withheld by the simplified posture. Exactly
// these five; crossings/mispred stay (bounded, load-bearing), aggregates
// stay (they are the point).
inline SceneLayers simplify_apply(SceneLayers in, bool detail) {
    if (detail)
        return in;
    in.access_marks = false; // one spur per mem access on the worldlines
    in.data_relief = false;  // one bar per cell
    in.working_set = false;  // one bar per live cell
    in.lifetime = false;     // one pillar per address
    in.sediment = false;     // one segment per band per column
    return in;
}

// The placard line. Empty when detail, or when nothing was actually
// withheld (no worldline hid AND access marks were off anyway) — the
// under-threshold no-op stays silent.
inline std::string simplify_note(bool detail, const space::SimplifyNote &n,
                                 bool access_marks_were_on) {
    if (detail)
        return std::string();
    if (n.hidden_threads == 0 && !access_marks_were_on)
        return std::string();
    std::string s = "simplified — ";
    if (n.hidden_threads) {
        s += "top " + std::to_string(kSceneSimplifiedTrajs) + " worldlines (+" +
             std::to_string(n.hidden_threads) + " folded, " +
             std::to_string(n.hidden_points) + " points)";
        if (access_marks_were_on)
            s += "; ";
    }
    if (access_marks_were_on)
        s += "access-mark spurs withheld";
    s += " — detail restores";
    return s;
}

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_SIMPLIFY_H
