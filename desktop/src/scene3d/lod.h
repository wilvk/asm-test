// lod.h — the camera-distance ENTITY BUDGET of 51-scene-focus-and-scale.md T4
// (G12). The tree's existing degrade path is TIME-budgeted, not
// DENSITY-budgeted: kScrubCellBudget / should_degrade / coarse_slice
// (ui/progress.h, ui/shell.cpp) keep a slow *slice* off the UI thread, and
// nothing handles a dense *frame*. A Hilbert order-12 plane is 4096x4096 ~
// 16.7M cells; both source docs name occlusion at scale as the top rendering
// risk.
//
// This generalises that proven idiom along the other axis — distance — and
// keeps its two load-bearing properties verbatim:
//
//   1. THE DECISION IS PURE. lod_tier() is a function of (radius, entity
//      count) and nothing else, so the thresholds are asserted directly with
//      no GL, exactly as should_degrade() is.
//   2. EVERY DROP IS ANNOUNCED. A tier that omits a class names it on screen
//      (lod_placard), the same contract scrub_degrade_note() provides. Silent
//      truncation reads as "there was nothing there" — the exact failure this
//      repo's degrade idiom was built to avoid.
//
// What this is NOT: a geometry LOD. No merged meshes, no aggregate towers, no
// instanced buildings — that is 43's Phase D. This drops and dims existing
// primitives only.
//
// Header-only and engine-free (D4): SceneLayers (a POD in the GL-free
// scene3d/scene.h, the same include layers.h takes) and should_degrade (a pure
// header) — so test_camera.cpp exercises every threshold while still linking
// nothing but its own object.
#ifndef ASMDESK_SCENE3D_LOD_H
#define ASMDESK_SCENE3D_LOD_H

#include <cstdint>
#include <string>
#include <vector>

#include "scene3d/camera.h" // Camera::kMinRadius / kMaxRadius, the tier's axis
#include "scene3d/scene.h"  // SceneLayers
#include "ui/progress.h"    // should_degrade — the SAME predicate, not a copy

namespace asmdesk::scene3d {

// The three labelled tiers.
//   Near — the full per-cell terrain plus every overlay the user asked for.
//   Mid  — terrain plus the high-weight overlays; the low-weight classes drop.
//   Far  — the plane itself: the flat, top-down-style reading, overlays off.
enum class LodTier { Near, Mid, Far };

// The density budget, in DRAWN ENTITIES (terrain cells with content +
// trajectory vertices + spurs + arcs). Deliberately the same magnitude as
// ui/shell.cpp's kScrubCellBudget: generous, so a golden-sized recording never
// degrades and this whole mechanism stays invisible until a frame is genuinely
// dense. A recording at or under it is ALWAYS Near, however far the camera
// stands off — distance alone is not a reason to draw less.
inline constexpr uint64_t kLodEntityBudget = 200000;

// The distance thresholds, in Camera::radius units. The default camera sits at
// 2.2 and top_down() at 1.9 — both frame the whole plane — so Near covers
// every ordinary working distance and the tiers engage only when the reader
// has actually pulled back beyond it.
inline constexpr float kLodMidRadius = 3.0f;
inline constexpr float kLodFarRadius = 6.0f;

// The tier for this frame. Monotonic in radius: dollying OUT can only move
// toward Far, never back toward Near, so a class that has dropped never
// flickers back in mid-gesture.
inline LodTier lod_tier(float radius, uint64_t entity_count) {
    if (!should_degrade(entity_count, kLodEntityBudget))
        return LodTier::Near;
    if (radius >= kLodFarRadius)
        return LodTier::Far;
    if (radius >= kLodMidRadius)
        return LodTier::Mid;
    return LodTier::Near;
}

inline const char *lod_tier_name(LodTier t) {
    switch (t) {
    case LodTier::Near:
        return "NEAR";
    case LodTier::Mid:
        return "MID";
    case LodTier::Far:
        return "FAR";
    }
    return "NEAR";
}

// Does this tier drop NON-SUBJECT worldlines rather than ghosting them? Only
// meaningful when a thread is actually focused: with no subject chosen, no
// worldline is "non-subject", and the budget must not choose a subject on the
// reader's behalf. This is the one place 51's "ghost, never hide" rule yields,
// and it yields to the degrade discipline — the placard names the class.
inline bool lod_drops_unfocused(LodTier t, bool has_focus) {
    return has_focus && t != LodTier::Near;
}

// The layers actually drawn at this tier, given what the reader asked for.
// Never turns a layer ON: a budget may draw less, never more.
inline SceneLayers lod_apply(const SceneLayers &requested, LodTier t) {
    SceneLayers l = requested;
    if (t == LodTier::Near)
        return l; // byte-identical to the request
    // MID: the two lowest-weight, highest-count overlay classes. Access spurs
    // are one line per memory access — by far the densest thing on the plane.
    l.access_marks = false;
    if (t == LodTier::Far) {
        // FAR: the plane's own reading. Every remaining overlay class goes;
        // the terrain surface (and its zoning/contours re-encodings, which
        // cost one branch each and are what make a distant plane readable at
        // all) stays.
        //
        // `exact` deliberately survives: a reader dollies out to see the
        // SHAPE of the path, and dropping it while keeping `statistical`
        // below would leave only the survey stipple on screen to be misread
        // as the path — the appearance collision T4 step 3 names. The
        // invariant is asserted in test_camera.cpp, not merely intended.
        l.statistical = false;
        l.ghost_fog = false;
        l.canopy = false;
        l.mispred = false;
        l.convergence = false;
        l.vehicle = false;
    }
    return l;
}

// The classes this tier is NOT drawing, by name, given what was requested — a
// layer the reader had already switched off is not a "drop" and is not
// reported (the placard must name what the BUDGET removed, so the reader can
// tell the two causes apart).
inline std::vector<std::string> lod_dropped(const SceneLayers &requested,
                                            LodTier t, bool has_focus) {
    std::vector<std::string> out;
    if (t == LodTier::Near)
        return out;
    const SceneLayers got = lod_apply(requested, t);
    if (requested.access_marks && !got.access_marks)
        out.push_back("access spurs");
    if (lod_drops_unfocused(t, has_focus))
        out.push_back("non-subject worldlines");
    if (requested.statistical && !got.statistical)
        out.push_back("statistical residency paths");
    if (requested.ghost_fog && !got.ghost_fog)
        out.push_back("ghost-fog survey terrain");
    if (requested.canopy && !got.canopy)
        out.push_back("module canopies");
    if (requested.mispred && !got.mispred)
        out.push_back("misprediction survey");
    if (requested.convergence && !got.convergence)
        out.push_back("convergence arcs");
    if (requested.vehicle && !got.vehicle)
        out.push_back("followed-citizen head");
    return out;
}

// T4 step 3 — PROVENANCE SURVIVES LOD. Two ways of drawing less (decimating an
// exact overlay, and sampling a survey) must never converge on one appearance.
// This tree's budget drops whole CLASSES rather than decimating within one, so
// nothing exact is ever redrawn in the statistical idiom; stating that on
// screen is what keeps a later decimation honest, because the sentence would
// then have to be edited to become false.
inline const char *lod_exact_provenance_note() {
    return "what is still drawn is drawn at full fidelity — an exact path the "
           "budget dropped is ABSENT and named here, never redrawn in the "
           "statistical stipple";
}

// The on-screen placard. Empty at NEAR (nothing to disclose); otherwise names
// the tier, why it engaged, and every class it removed. The wording is a
// single source of truth between the draw call and the test that a drop can
// never go silent — scrub_degrade_note()'s own pattern.
inline std::string lod_placard(const SceneLayers &requested, LodTier t,
                               bool has_focus) {
    if (t == LodTier::Near)
        return std::string();
    std::string s = lod_tier_name(t);
    s += " (dense frame, camera pulled back): not drawn — ";
    const std::vector<std::string> dropped =
        lod_dropped(requested, t, has_focus);
    if (dropped.empty()) {
        s += "nothing (every class this tier drops was already off)";
    } else {
        for (size_t i = 0; i < dropped.size(); i++) {
            if (i)
                s += ", ";
            s += dropped[i];
        }
    }
    s += ". Dolly in to restore. ";
    s += lod_exact_provenance_note();
    return s;
}

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_LOD_H
