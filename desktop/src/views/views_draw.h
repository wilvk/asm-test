// views_draw.h — the thin ImGui half of each replay view
// (docs/internal/gui/04-replay-views.md T3-T7).
//
// Every function here takes an ALREADY-BUILT model and only draws it. The split
// is enforced by the build: the builders live in <view>.cpp, which the view
// tests link on their own, so a builder that reached for ImGui would fail to
// link in its own test. Nothing in this header decides anything — if a rule is
// interesting enough to test, it lives in the builder.
#ifndef ASMDESK_VIEWS_DRAW_H
#define ASMDESK_VIEWS_DRAW_H

#include "views/canvas.h"
#include "views/diff_view.h"
#include "views/scrubber.h"
#include "views/slice_view.h"
#include "views/timeline.h"

namespace asmdesk {

// A refusal or truncation placard. Both are drawn in the warn colour and
// neither is collapsible: the whole point is that it cannot be dismissed and
// then forgotten about while the numbers below it are still wrong.
void draw_banner(const char *text, bool refusal);

void draw_canvas(const dt_canvas &c);
void draw_timeline(const dt_timeline &t);

// The register time-travel scrubber (09-teaching-producers.md T3). `idx` is the
// shared step index; `playhead` is owned by the caller and moved by the slider
// and the `[` / `]` step keys (04's bindings). Returns the (possibly changed)
// playhead so the shell can persist it.
uint64_t draw_scrubber(const StepIndex &idx, uint64_t playhead);
void draw_slice_view(const dt_slice_view &v);
void draw_diff_view(const dt_diff_view &v);

// The keyboard-binding help overlay, fed by dt_nav_bindings() so the help and
// the key map cannot drift apart.
void draw_bindings_help();

} // namespace asmdesk
#endif // ASMDESK_VIEWS_DRAW_H
