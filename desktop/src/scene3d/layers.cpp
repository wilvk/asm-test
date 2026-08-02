// layers.cpp — the registry table of layers.h.
#include "scene3d/layers.h"

namespace asmdesk::scene3d {

const std::vector<LayerDesc> &scene_layers_all() {
    using G = LayerDesc::Group;
    // Grouped by the question asked (T1 step 4), in HUD display order. Grade
    // is this file's own judgement call for the nine pre-existing bools (the
    // family's docs never assigned them one, since the registry did not exist
    // before this brief); each later brief's own bool carries a grade its own
    // doc states.
    static const std::vector<LayerDesc> kAll = {
        // --- structure: what is here, and where -----------------------------
        {"terrain", "terrain", "is the address-space surface drawn at all?",
         G::Structure, LayerGrade::Exact, &SceneLayers::terrain},
        {"zoning", "zoning", "what kind of region is this address in?",
         G::Structure, LayerGrade::Exact, &SceneLayers::zoning},
        {"exact", "exact", "where did execution actually go?", G::Structure,
         LayerGrade::Exact, &SceneLayers::exact},
        {"access", "access", "which data cell did this instruction touch?",
         G::Structure, LayerGrade::Exact, &SceneLayers::access_marks},
        {"contours", "contours", "how much taller is one band than the next?",
         G::Structure, LayerGrade::Derived, &SceneLayers::contours},
        {"canopy", "canopy", "which library is hot as a whole?", G::Structure,
         LayerGrade::Derived, &SceneLayers::canopy},
        {"opcode", "opcode", "what kind of work happens here?", G::Structure,
         LayerGrade::Derived, &SceneLayers::opcode},
        // 58 T2: Derived, not Exact — the surfaces are log-scaled sums of the
        // recorded per-access sizes, which is a re-encoding of exact data
        // (LayerGrade::Derived's own definition), not a raw recorded field.
        {"relief", "read/write relief",
         "is this address read-mostly, a write accumulator, or both?",
         G::Structure, LayerGrade::Derived, &SceneLayers::data_relief},
        {"edl", "EDL", "which surface is nearer the camera?", G::Structure,
         LayerGrade::Derived, &SceneLayers::edl},
        // --- activity: what happened, and when ------------------------------
        {"vehicle", "vehicle", "where is the followed thread right now?",
         G::Activity, LayerGrade::Exact, &SceneLayers::vehicle},
        // 58 T3: Derived for the same reason as `relief` — a windowed
        // log-scaled sum of recorded sizes, not a raw recorded field.
        {"working set", "working set",
         "what is being touched now, and what has drifted cold?", G::Activity,
         LayerGrade::Derived, &SceneLayers::working_set},
        // 58 T4: Exact — the pillar's two ends ARE recorded step indices
        // (DataCell::steps.front()/back()), not an aggregate of them. The
        // claim it makes is exactly what the recording states.
        {"lifetime", "lifetime",
         "over what interval is each address OBSERVED alive?", G::Activity,
         LayerGrade::Exact, &SceneLayers::lifetime},
        // 58 T5: Exact — every vertex is a recorded access at its recorded
        // address, and a segment exists only between two accesses that really
        // were consecutive in the record.
        {"access order", "access order",
         "is the access pattern streaming, strided, or pointer-chasing?",
         G::Activity, LayerGrade::Exact, &SceneLayers::data_ribbon},
        {"convergence", "convergence",
         "did two threads come near the same place, close in time?",
         G::Activity, LayerGrade::Derived, &SceneLayers::convergence},
        // --- fidelity: how much can this be trusted --------------------------
        {"weather", "weather", "how much can this session's data be trusted overall?",
         G::Fidelity, LayerGrade::Derived, &SceneLayers::weather},
        {"confidence", "confidence",
         "how much do I trust each region right now?", G::Fidelity,
         LayerGrade::Derived, &SceneLayers::confidence},
        // --- survey: statistical-only layers, never merged into the exact ---
        {"statistical", "statistical",
         "where does sampled residency suggest activity landed?", G::Survey,
         LayerGrade::Statistical, &SceneLayers::statistical},
        {"ghost fog", "ghost fog",
         "where does survey residency suggest content lives, absent an exact "
         "measurement?",
         G::Survey, LayerGrade::Statistical, &SceneLayers::ghost_fog},
        {"branch (statistical)", "branch (statistical)",
         "where does the branch predictor struggle?", G::Survey,
         LayerGrade::Statistical, &SceneLayers::mispred},
    };
    return kAll;
}

} // namespace asmdesk::scene3d
