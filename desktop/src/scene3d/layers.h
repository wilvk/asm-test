// layers.h — the layer registry (56-fidelity-and-module-layers.md T1): turns
// SceneLayers from a list of bools a caller has to know about by hand into a
// system the HUD, the legend and a test all read from one table.
//
// Engine-free and ImGui-free (D4): only SceneLayers itself (scene3d/scene.h,
// which is GL-free — the GL objects live in scene.cpp) — so hud.cpp (ImGui,
// no GL) and a pure test TU can both include this with no engine pulled in.
#ifndef ASMDESK_SCENE3D_LAYERS_H
#define ASMDESK_SCENE3D_LAYERS_H

#include <vector>

#include "scene3d/scene.h"

namespace asmdesk::scene3d {

// How trustworthy the quantity a layer draws is — drives the shared chrome
// (a Statistical layer's toggle always carries the STATISTICAL warn-coloured
// label, from ui/theme.h's palette, so no layer re-invents the phrasing; D7 /
// 24-one-visual-language). Exact: a direct recorded measurement. Statistical:
// sampled survey residency, never exact. Derived: a re-encoding or aggregate
// of exact data (no new claim, but not a raw recorded field either — e.g. a
// log-scaled sum, or a fidelity-flag re-lift).
enum class LayerGrade { Exact, Statistical, Derived };

// One row of the registry: everything the HUD toggle list, the legend and the
// exhaustiveness test need for one SceneLayers bool. `id` is a stable key
// (never renumbered) used by persistence, so a reordered table never shuffles
// a user's saved toggles.
struct LayerDesc {
    const char *id;       // stable key
    const char *label;    // HUD toggle text
    const char *question; // the overview question this layer answers, one line
    // T1 step 4: which HUD group this toggle collapses under — fidelity
    // (trust), structure (what/where), activity (what happened), survey
    // (statistical-only layers get their own group so the STATISTICAL label
    // reads as a category, not a one-off).
    enum class Group { Fidelity, Structure, Activity, Survey } group;
    LayerGrade grade;
    bool SceneLayers::*flag; // the bool this row drives
};

// The whole registry, in HUD display order (grouped; see LayerDesc::Group).
// Built once (function-local static): SceneLayers' shape is compile-time
// fixed, so every caller (the HUD draws this every frame) shares one instance
// rather than reallocating the table and its string literals per frame.
//
// EXHAUSTIVE BY TEST (T1's own "Done when"): every SceneLayers member appears
// in exactly one row here — test_layers.cpp pins it, so a bool added to
// SceneLayers without a row here fails a named check instead of silently
// shipping an unexplained toggle.
const std::vector<LayerDesc> &scene_layers_all();

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_LAYERS_H
