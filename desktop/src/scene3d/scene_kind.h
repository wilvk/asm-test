// scene_kind.h — WHICH substrate a 3D pane is showing, and what its axes MEAN
// (docs/internal/gui/59-standalone-scenes.md T1).
//
// Every layer in 56/57/58 rides ONE substrate: the Hilbert address plane with a
// vertical time axis. That is not a limitation — it is what lets those layers
// compose, because their X and Z stay registered. Four of the catalog's
// questions have a load-bearing axis that is NOT an address (an execution step
// shared only up to a fork; a discrete call ordinal; call order across threads;
// a byte index inside one register), and forcing them onto the plane would
// fabricate a correspondence the recording does not carry.
//
// So a scene declares its KIND, and the kind declares its AXES. Two rules are
// structural here rather than aspirational:
//
//  1. **Axis labelling is part of the host contract, not each scene's choice.**
//     `scene_axes()` is a switch with no `default:` over a complete enum, so a
//     new SceneKind that forgets to say what its axes are fails to COMPILE
//     (-Wswitch, which this tree builds with via -Wall). Every axis string is
//     required non-empty, asserted by test_scene_kind's exhaustiveness walk.
//     This is 34-playhead-and-scene-reach's two-axes rule made mechanical: a
//     scene cannot ship an unlabelled axis, because the field is required.
//
//  2. **`y_not` is required too.** A new substrate is a new opportunity to imply
//     a correspondence, so each kind states not only what its vertical axis IS
//     but what it is NOT (D7). An axis label that only makes the positive claim
//     is exactly the fabrication this family exists to avoid.
//
// **Scenes do not compose.** The pane switches kinds; it never draws two at
// once. The additive-layer model (SceneLayers) is a property of the SHARED
// plane and does not extend to substrates with different axes — two meanings on
// one screen position is precisely what the layer registry avoids by keeping
// every layer on one substrate.
//
// Header-only and dependency-free on purpose: pick.h includes this for the
// per-kind id band, and pick.o links into a dozen narrow test binaries whose
// link lines should not grow an object for a table of strings.
#ifndef ASMDESK_SCENE3D_SCENE_KIND_H
#define ASMDESK_SCENE3D_SCENE_KIND_H

#include <cstdint>
#include <string_view>
#include <vector>

namespace asmdesk::scene3d {

// The substrates a 3D pane can host. `Plane` is everything that existed before
// 59 — it MUST stay value 0 and MUST stay the default everywhere, because the
// pick id band scheme below gives kind 0 the low id space, which is what keeps
// every plane id (and therefore every golden pick, every existing test)
// byte-identical.
enum class SceneKind : uint8_t {
    Plane = 0,     // the Hilbert address plane × time × height (10, 56–58)
    Divergence,    // 59 T2: two recordings, one shared prefix, then a fork
    Invocation,    // 59 T3: one routine's control flow across its calls
    ModuleRibbon,  // 59 T4: which thread is in which library, when
    LanePrism,     // 59 T5: inside one wide vector register, over its writes
};

// Every kind, for exhaustive iteration (the axis-labelling walk, the band
// non-overlap check, a HUD selector). KEEP IN SYNC with the enum — the
// exhaustiveness test asserts the size matches the last enumerator.
inline const std::vector<SceneKind> &all_scene_kinds() {
    static const std::vector<SceneKind> v = {
        SceneKind::Plane, SceneKind::Divergence, SceneKind::Invocation,
        SceneKind::ModuleRibbon, SceneKind::LanePrism};
    return v;
}

inline uint32_t scene_kind_index(SceneKind k) {
    return static_cast<uint32_t>(k);
}

// The short, user-facing name of a kind (the HUD selector's label).
inline const char *scene_kind_name(SceneKind k) {
    switch (k) {
    case SceneKind::Plane:
        return "address plane";
    case SceneKind::Divergence:
        return "divergence worldline";
    case SceneKind::Invocation:
        return "invocation stack";
    case SceneKind::ModuleRibbon:
        return "module excursion ribbon";
    case SceneKind::LanePrism:
        return "SIMD lane prism";
    }
    return "address plane";
}

// What each axis of a kind IS, and what its VERTICAL axis is NOT. All four
// fields are required non-empty; `scene_axes` is the single source the HUD
// renders and the tests walk.
struct SceneAxes {
    const char *x;     // the horizontal axis
    const char *y;     // the VERTICAL axis — the load-bearing one
    const char *z;     // the depth axis
    const char *y_not; // what the vertical axis must not be read as (D7)
};

// No `default:` label, deliberately: adding a SceneKind without giving it axes
// is a -Wswitch warning at the point of the omission, not a silent blank label
// discovered by a user.
inline SceneAxes scene_axes(SceneKind k) {
    switch (k) {
    case SceneKind::Plane:
        return SceneAxes{
            "address (Hilbert plane, u)",
            "terrain height = access density; a worldline's height = its own "
            "trace step",
            "address (Hilbert plane, v)",
            "NOT one clock: the terrain playhead (trace-residency time) and a "
            "worldline's step axis are separate, and neither is the flat "
            "views' execution step (34, 49 T4)"};
    case SceneKind::Divergence:
        return SceneAxes{
            "recording side — A on the left, B on the right",
            "execution step",
            "register-class lane of a rib (GPR / vector / flags / PC / other)",
            "NOT a proof of instruction correspondence past the fork: after "
            "the streams part, A's step n and B's step n need not be the same "
            "instruction"};
    case SceneKind::Invocation:
        return SceneAxes{
            "address (Hilbert plane, u)",
            "invocation # — a discrete call ordinal",
            "address (Hilbert plane, v)",
            "NOT a time and never scrubbed: the gap between two invocations is "
            "unobserved, of unknown length (08 T6)"};
    case SceneKind::ModuleRibbon:
        return SceneAxes{
            "call order (the recording's own event seq)",
            "call depth (the engine's effective, focus-rebased depth)",
            "thread lane — one sheet per tid",
            "NOT either playhead and not a magnitude: depth is a stack "
            "property, and a capped floor is a clipped edge, not a bottom"};
    case SceneKind::LanePrism:
        return SceneAxes{
            "byte / element index inside the register",
            "byte magnitude (0..255)",
            "stacked writes, oldest nearest",
            "NOT wall clock and NOT an address: there is no address inside a "
            "register, and Z counts recorded writes, not elapsed time"};
    }
    // Unreachable for a complete enum; kept total rather than UB.
    return SceneAxes{"address (Hilbert plane, u)",
                     "terrain height = access density",
                     "address (Hilbert plane, v)", "NOT one clock"};
}

// One line per axis, in X/Y/Z order, then the vertical axis's denial — the
// exact text the HUD renders and the tests pin, so the two cannot drift.
inline std::vector<std::string_view> scene_axis_lines(SceneKind k) {
    const SceneAxes a = scene_axes(k);
    return {a.x, a.y, a.z, a.y_not};
}

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_SCENE_KIND_H
