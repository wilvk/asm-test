// cvd.h — colour-vision-deficiency simulation + WCAG contrast (24 T2 / F15).
//
// Pure maths over ImVec4, no ImGui context and no engine (D4): it links into the
// app, the render-only viewer and the null test backend identically, and every
// value it computes is asserted headlessly (test_cvd). The palette (ui/theme.h)
// is verified through THIS: simulate protanopia / deuteranopia / tritanopia over
// the categorical hues, and check contrast at the smallest font. Where two hues
// collapse under a simulation, the honest guarantee is the SECOND CHANNEL
// (ui/legend.h's encoding table) — this file only measures; it never pretends a
// hue alone suffices.
#ifndef ASMDESK_UI_CVD_H
#define ASMDESK_UI_CVD_H

#include "imgui.h"

namespace asmdesk {

// The three dichromacies simulated. `none` returns the colour unchanged (a
// convenient identity for table-driven loops).
enum class dt_cvd_kind { none, protan, deuter, tritan };

// Simulate how `c` appears to a viewer with the given dichromacy. Machado et al.
// (2009) severity-1.0 transform; an approximation adequate for a redundancy
// check, not clinical colorimetry (documented in cvd.cpp). Alpha is preserved.
ImVec4 dt_cvd_simulate(ImVec4 c, dt_cvd_kind kind);

// WCAG 2.1 relative-luminance contrast ratio between two opaque colours, in
// [1, 21]. Order-independent. Alpha is ignored (the palette's swatches are drawn
// opaque over the panel background).
float dt_contrast_ratio(ImVec4 a, ImVec4 b);

// Perceptual distance between two colours AFTER a CVD simulation — a plain
// Euclidean distance in the simulated RGB cube, in [0, sqrt(3)]. Two hues a
// dichromat can still tell apart sit far in this metric; two that collapse sit
// near 0. Used by the test to decide when the second channel is load-bearing.
float dt_cvd_distance(ImVec4 a, ImVec4 b, dt_cvd_kind kind);

} // namespace asmdesk
#endif // ASMDESK_UI_CVD_H
