// atmosphere.h — the fidelity weather sky's pure config
// (docs/internal/gui/44-faithful-city-phase-a-mvp-terrain-reskin.md T3). A
// plain data struct: no GL, no ImGui. This lives under scene3d/ rather than
// space/ because it is GL-ADJACENT render config (raw floats a shader reads
// as uniforms), not a space/ data model derived from a recording — the same
// organisational call this directory's closest precedent, camera.h, already
// makes (pure math, GL-adjacent, zero GL/ImGui include).
//
// The shell (ImGui-linked, ui/shell.cpp) computes these floats from
// fidelity_severity() (ui/fidelity.h) and ui/theme.h's shared palette —
// theme.h stays OUT of this header and out of scene3d/scene.cpp (D4-adjacent:
// the engine-free scene TU must not gain an ImGui include), then calls
// Scene::set_atmosphere(...) every frame with the already-damped result.
// Atmosphere itself does no damping and holds no time state; it is a pure
// snapshot the shell overwrites each frame.
#ifndef ASMDESK_SCENE3D_ATMOSPHERE_H
#define ASMDESK_SCENE3D_ATMOSPHERE_H

namespace asmdesk::scene3d {

struct Atmosphere {
    // Sky gradient: the low/ambient colour and the "front" (higher/sunward)
    // colour the fragment shader mixes between by screen-space height.
    float ambient[3] = {0.08f, 0.10f, 0.16f};
    float front[3] = {0.08f, 0.10f, 0.16f};
    // A unit-ish direction toward the "sun" — purely a highlight hint for the
    // sky quad, no lighting model elsewhere reads it.
    float sun_dir[3] = {0.3f, 0.8f, 0.5f};
    // 0 = clear; higher values wash the sky toward `ambient` (an integrity-
    // tier "red dusk" reads hazier, not just redder).
    float fog_density = 0.0f;
};

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_ATMOSPHERE_H
