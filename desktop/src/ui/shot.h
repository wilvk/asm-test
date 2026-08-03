// shot.h — the screenshot manifest: WHICH recording, WHICH view, WHICH scene
// substrate and layers each documented image is rendered from
// (docs/guides/desktop-gui-scenes.md).
//
// A manifest rather than CLI flags so the whole set costs one process and one GL
// context, and so adding or reordering an image is a data edit rather than a
// code change. The model here is PURE — no GL, no ImGui, no EGL — in the same
// split every view in this tree uses, which is what lets test_shot_manifest
// assert the exhaustiveness rule headlessly.
#ifndef ASMDESK_UI_SHOT_H
#define ASMDESK_UI_SHOT_H

#include <string>
#include <vector>

#include "scene3d/scene.h"      // SceneLayers
#include "scene3d/scene_kind.h" // SceneKind
#include "ui/view_presence.h"   // ViewId

namespace asmdesk {

struct ShotSpec {
    std::string name; // output basename, no extension
    // The recordings to open. [0] is the A side. [1], when present, is attached
    // as the B side — the role the `d` key fills interactively, and the only way
    // the Divergence scene has anything to diverge FROM.
    std::vector<std::string> open;
    ViewId view = ViewId::Scene3D;
    scene3d::SceneKind scene = scene3d::SceneKind::Plane;
    std::vector<std::string> layers; // LayerDesc::id keys; empty => defaults
    int width = 1600;
    int height = 1000;
    // Show the "3D overview" HUD window expanded. It is a SEPARATE dockable
    // window rather than a child of the scene pane, and it is long enough
    // (every layer toggle, every encoding legend) to cover the viewport
    // completely — so a scene shot with the HUD up photographs the text and
    // none of the geometry. Default false: collapse it, and let the guide page
    // carry the axis wording, which it quotes verbatim anyway. Set true for the
    // one shot that documents the HUD itself.
    bool hud = false;
    // Frames rendered before the capture. ImGui's docking and layout settle over
    // several frames, and the terrain weave lands over a few more; capturing
    // frame 0 photographs a half-built UI.
    int warmup = 30;
};

// Parse a manifest (a JSON array of entries). Returns false with `err` set on
// malformed JSON, an unknown view/scene/layer name, a missing `name`, or a
// non-positive size.
//
// An unknown name is REFUSED rather than defaulted. A shot that silently
// documented a different substrate than its author asked for is worse than a
// shot that does not exist, because nothing downstream can tell it went wrong.
bool shot_manifest_parse(const std::string &json, std::vector<ShotSpec> &out,
                         std::string &err);

// Name -> enum. False (leaving `out` untouched) if the name is not known.
bool shot_view_from_name(const std::string &name, ViewId &out);
bool shot_scene_from_name(const std::string &name, scene3d::SceneKind &out);

// Turn ON each named layer, by the stable LayerDesc::id key. Returns false with
// `err` naming the first unknown id. Does NOT touch layers it was not given.
bool shot_apply_layers(const std::vector<std::string> &ids,
                       scene3d::SceneLayers &out, std::string &err);

// Turn every layer OFF, via the registry (which test_layers pins as exhaustive
// over SceneLayers' members).
//
// This exists because SceneLayers defaults every flag to TRUE. A shot that only
// switched its named layers on would therefore be identical to one that named
// nothing — every layer would be lit in both. Documenting what one layer
// contributes needs the others off, so a manifest `layers` list means ONLY
// these, and that requires clearing first.
void shot_clear_layers(scene3d::SceneLayers &out);

} // namespace asmdesk
#endif // ASMDESK_UI_SHOT_H
