// hud.h — the ImGui HUD drawn over the 3D scene (docs/internal/gui/
// 10-spacetime-3d-overview.md T4 step 4). A SEPARATE TU from scene.cpp so the GL
// scene links no ImGui and the HUD links no GL: the playhead, the layer toggles,
// the provenance chips (coarse-vs-rich, exact-vs-statistical, truncation) and the
// region legend are pure ImGui over the pure space/ models. Engine-free (D4).
//
// The HUD never draws GL and never mutates the models; it reports the user's
// intent back through HudState (a moved playhead, a camera preset) and the caller
// re-slices the terrain / rebuilds the trajectory and drives the Camera.
#ifndef ASMDESK_SCENE3D_HUD_H
#define ASMDESK_SCENE3D_HUD_H

#include <cstdint>
#include <string>
#include <vector>

#include "scene3d/scene.h"
#include "space/terrain.h"
#include "space/trajectory.h"

namespace asmdesk::scene3d {

// 36 T4: the placement-provenance chips, as PURE DATA so the honesty chrome is
// testable without an ImGui frame — deleting a branch fails a named check. Bad is
// a refusal (rendered ALL CAPS, in the refuse colour); Warn is a graded fact
// (lowercase `label: explanation`, in the warn colour). draw_scene_hud renders
// this list into the chip row; the tests assert its contents per scenario.
struct PlacementChip {
    enum Sev { Bad, Warn } sev;
    std::string text;
};
std::vector<PlacementChip> placement_chips(const space::TerrainModel &terr,
                                           const space::TrajectorySet &traj);

struct HudState {
    uint64_t t = 0;      // playhead: the inclusive terrain slice [0, t]
    uint64_t nsteps = 0; // the recording's step extent (the slider's max)
    SceneLayers layers;  // terrain / exact / statistical / access toggles

    // Set true by draw_scene_hud when the user moved the playhead this frame, so
    // the caller re-slices T2 and rebuilds T3 for the new t. The caller clears it.
    bool playhead_moved = false;
    // Camera preset requests (the "reset view" and honest "top-down 2D-ish"
    // buttons); the caller applies them to its Camera and need not clear them.
    bool req_reset_view = false;
    bool req_top_down = false;

    // 34 T3: the terrain-time play/pause transport, same shape as the camera
    // presets. `playing` is set by the caller before the draw so the button reads
    // "Play"/"Pause"; `req_play_toggle` is the HUD's intent, which the caller
    // applies to SceneView::play and clears. The 3D playhead walks trace-residency
    // time, a DIFFERENT axis from the flat views' execution step (34 honesty note).
    bool playing = false;
    bool req_play_toggle = false;

    // Set true by draw_scene_hud when the HUD window holds keyboard focus this
    // frame (22-selection-and-search.md T2, F18). The caller ORs it with the 3D
    // viewport's focus to decide whether the arrow/dolly/reset camera keys act, so
    // a keyboard-only analyst can orbit the scene from either the HUD or the
    // viewport — the accessibility substitute for ImGui's absent screen-reader tree.
    bool kbd_focus = false;
};

// Draw the HUD for the current ImGui frame. `terr`/`traj` supply the provenance
// and the legend; nothing here is mutated.
void draw_scene_hud(HudState &s, const space::TerrainModel &terr,
                    const space::TrajectorySet &traj);

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_HUD_H
