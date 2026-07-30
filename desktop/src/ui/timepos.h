// timepos.h — ONE time-position widget, two faithful variants (24 T4 / F16).
//
// Five different "move through time" controls had drifted across the views
// (SliderInt playheads in the scrubber/abixray/loom/3D-HUD, prev/next in
// Invocations, InputInt in Disassembly), so the muscle action for the most
// common trace operation changed per view. This is the one widget:
//
//   dt_timepos_scrub  — a CONTINUOUS slider, for a surface where a real total
//                       exists (a step axis you can scrub end to end).
//   dt_timepos_step   — a DISCRETE prev/next pager, for a surface where no
//                       continuous total exists (between two Invocations the
//                       target ran unobserved; a slider would draw that gap as
//                       elapsed captured time). The discrete case is VISIBLY
//                       MARKED as intentional — a glyph + a hover carrying the
//                       verbatim reason — so it reads as a deliberate fidelity
//                       choice, not a missing scrubber (F16).
//
// The reason strings live in ONE registry (dt_timepos_discrete_reason), so a
// discrete surface and its marker cannot drift, and the T4 test asserts every
// registered discrete surface carries a non-empty reason.
#ifndef ASMDESK_UI_TIMEPOS_H
#define ASMDESK_UI_TIMEPOS_H

#include "imgui.h"

namespace asmdesk {

// A continuous scrub. `t` in [0, total]. Returns true if it moved this frame.
bool dt_timepos_scrub(const char *label, int *t, int total);

// A discrete step. `idx` in [0, count). `reason` is the verbatim fidelity note
// (why this axis is discrete, not scrubbable); it is shown as an always-visible
// marker with the reason on hover. Returns true if `idx` moved this frame.
bool dt_timepos_step(const char *label, int *idx, int count, const char *reason);

// The ONE registry of discrete-time reasons, keyed by surface. Returns the
// verbatim reason, or "" for an unregistered surface. Known surfaces:
// "invocations", "disasm-logical-time".
const char *dt_timepos_discrete_reason(const char *surface);

} // namespace asmdesk
#endif // ASMDESK_UI_TIMEPOS_H
