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
    SessionFlow,   // 2026-08-10: the session-strip channels as smooth
                   // depth-stacked ribbon rates over stream order
};

// Every kind, for exhaustive iteration (the axis-labelling walk, the band
// non-overlap check, a HUD selector). KEEP IN SYNC with the enum — the
// exhaustiveness test asserts the size matches the last enumerator.
inline const std::vector<SceneKind> &all_scene_kinds() {
    static const std::vector<SceneKind> v = {
        SceneKind::Plane,        SceneKind::Divergence, SceneKind::Invocation,
        SceneKind::ModuleRibbon, SceneKind::LanePrism,  SceneKind::SessionFlow};
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
    case SceneKind::SessionFlow:
        return "session flow";
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
        // 61 T5/T10: the u/v labels no longer name HILBERT. The address->cell
        // mapping is selectable (Projection::Layout), so naming one layout in
        // a user-visible axis label is false under the other; the axis is the
        // ADDRESS plane either way. And Y no longer carries a worldline's
        // trace step at all — T5 took trace time off the spatial budget, so
        // the path lies flat and time is read through the playhead. The three
        // OPT-IN layers do still stand trace time on this axis, which is why
        // the denial names them rather than claiming the axis has one meaning.
        return SceneAxes{
            "address (the address plane's u)",
            "terrain height = access density (log). A worldline does NOT rise "
            "with trace time: it lies flat and is read through the playhead. "
            "The opt-in lifetime / ribbon / sediment layers DO stand trace "
            "time on this axis",
            "address (the address plane's v)",
            "NOT one clock: the terrain playhead (trace-residency time), the "
            "vehicle's own followed step, and the flat views' execution step "
            "are three separate axes (34, 49 T4, 61 T5)"};
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
            "address (the address plane's u)",
            "invocation # — a discrete call ordinal",
            "address (the address plane's v)",
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
    case SceneKind::SessionFlow:
        return SceneAxes{
            "stream order (seq buckets)",
            "activity rate (events per bucket, smoothed for display)",
            "session rows (threads \u00b7 kernel \u00b7 memory)",
            "NOT time and not a duration: stream order only, and a height is "
            "a bucketed rate, never a measured interval"};
    }
    // Unreachable for a complete enum; kept total rather than UB.
    return SceneAxes{"address (the address plane's u)",
                     "terrain height = access density",
                     "address (the address plane's v)", "NOT one clock"};
}

// One line per axis, in X/Y/Z order, then the vertical axis's denial — the
// exact text the HUD renders and the tests pin, so the two cannot drift.
inline std::vector<std::string_view> scene_axis_lines(SceneKind k) {
    const SceneAxes a = scene_axes(k);
    return {a.x, a.y, a.z, a.y_not};
}

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_SCENE_KIND_H
