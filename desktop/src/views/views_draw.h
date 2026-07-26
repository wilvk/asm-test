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

#include <functional>

#include "nav.h"
#include "views/canvas.h"
#include "views/diff_view.h"
#include "views/scrubber.h"
#include "views/slice_view.h"
#include "views/timeline.h"
#include "walkthrough.h"

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

// The ABI x-ray (09-teaching-producers.md T4). Two register scrubbers — the
// SysV and Win64 halves of a paired recording — LOCKED to one playhead, with an
// authored walkthrough (`walk`, decoded from the SysV leg) driving it. `walk`
// and `playhead` are caller-owned: the rail's prev/next stop moves both, and the
// slider and `[` / `]` keys move the playhead directly. Both are mutated in place.
void draw_abixray(const StepIndex &sysv, const StepIndex &win64, wt_model &walk,
                  uint64_t &playhead);
void draw_slice_view(const dt_slice_view &v);
// The two-recording summary (04-replay-views.md T7). Every navigable row's "go"
// button routes its deep link through `go` (the shell's dt_nav_go seam, exactly
// as the Observer deck does) — NOT the clipboard: a nav button that only copied
// a link would leave the user where they were, believing they had moved. `go`
// may be null in a draw-only smoke.
void draw_diff_view(const dt_diff_view &v,
                    const std::function<void(const dt_link &)> &go);

// The keyboard-binding help overlay, fed by dt_nav_bindings() so the help and
// the key map cannot drift apart.
void draw_bindings_help();

} // namespace asmdesk
#endif // ASMDESK_VIEWS_DRAW_H
