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
#include <string>

#include "nav.h"
#include "ui/honesty.h" // HonestyTier — the ONE graded honesty vocabulary (23 T1)
#include "ui/progress.h" // LongOp — the uniform busy signal (23 T4)
#include "views/canvas.h"
#include "views/diff_view.h"
#include "views/scrubber.h"
#include "views/slice_view.h"
#include "views/timeline.h"
#include "walkthrough.h"

namespace asmdesk {

// --- the ONE graded honesty-chrome vocabulary (23-graded-truth-layer.md T1) --
// F5's fix: honesty chrome is now ONE banner form + ONE inline chip + ONE glyph
// set, graded into three tiers, with MANDATED placement enforced by the API — a
// banner is a pane-top full-width placard, a chip is an inline stream-header
// mark. Colours come from doc 24 T5.1's semantic accessors (ui/theme.h); each
// tier also carries a Codicon glyph + a text token so the tier reads without
// colour alone (24 T5.2). NONE of this hides a field — it only decides loudness.

// The tier-graded banner (pane-top placard contract). Neutral banners are quiet,
// caution is amber, integrity is loud red. A caution banner MAY collapse to its
// chip after first read (the caller passes/owns `collapsed`); an integrity banner
// NEVER collapses (`collapsed` is ignored for it) — exactly as the refusal path
// was always non-dismissable. `collapsed` may be null (never collapses).
void draw_honesty_banner(const char *text, HonestyTier tier,
                         bool *collapsed = nullptr);

// The quiet inline chip (stream-header contract): a neutral honesty mark that
// sits on a header row without a banner's weight. A T1 signal (skip, statistical,
// redacted, coarse, bounded) renders as this, never as an amber banner.
void draw_honesty_chip(const char *text, HonestyTier tier);

// The copyable terminal-command hint: one rung more concrete than a "-> remedy"
// line. When an action is gated behind a command the app cannot run itself
// (raising a Yama scope, lowering perf_event_paranoid, rebuilding asmspy), the
// remedy prose already names it; this renders that command as a `$ …` line in the
// default monospace font with a one-click Copy button (ImGui::SetClipboardText).
// `cmd` empty -> nothing drawn, so callers pass remedy_command(...) directly.
// `id` scopes the button's ImGui id (a per-row pid, a backend label, ...).
void draw_command_hint(const char *id, const std::string &cmd);

// The legacy two-level placard, kept as a THIN SHIM over the graded banner so the
// ten existing call sites migrate incrementally: refusal -> integrity (loud, non
// collapsible), else -> caution (amber). Both are drawn in the graded colours and
// neither is collapsible here — the whole point is that a wrong-number placard
// cannot be dismissed and then forgotten.
void draw_banner(const char *text, bool refusal);

// The uniform busy signal (23-graded-truth-layer.md T4): an elapsed-time +
// spinner/determinate-bar + Cancel for any op that can exceed a frame. The
// decision half is pure (ui/progress.h, LongOp); this is the thin draw half. It
// sets `op.cancel_requested` when the user clicks Cancel; the caller polls it and
// abandons the op, leaving the last good state (never a half-built model).
void draw_progress(LongOp &op);

void draw_canvas(const dt_canvas &c);
void draw_timeline(const dt_timeline &t);

// The always-visible timeline overview/minimap strip (21-spine-navigation.md T3,
// completing 14-quick-wins.md T5). Draws the whole-trace density (ImPlot when a
// context exists, a text ratio otherwise) with the current viewport `[*lo,*hi]`
// marked, then the ImZoomSlider window control that writes `*lo`/`*hi` back (step
// space). A click on the strip calls `jump(step)` with the step under the cursor
// — the caller routes it through dt_nav_go, so a minimap click and a typed go-to
// land identically (D4). `nsteps` is the recording's dataflow step count.
void draw_timeline_overview(const dt_timeline &t, uint32_t nsteps, double *lo,
                            double *hi,
                            const std::function<void(uint32_t)> &jump);

// The register time-travel scrubber (09-teaching-producers.md T3). `idx` is the
// shared step index; `playhead` is owned by the caller and moved by the slider
// and the `[` / `]` step keys (04's bindings). Returns the (possibly changed)
// playhead so the shell can persist it.
//
// 30 R3 T4: `rec` (the recording `idx` was built from, or null in a standalone
// draw) drives the producer-absent path. When it is emulator-replayable, the
// deck offers a "synthesize register history" button that RE-DERIVES the history
// under the emulator ring (regsynth.h, full app only) and REPLACES `idx` in place
// (hence non-const) with the synthesised, clearly-labelled index; where it is not
// replayable, the honest refusal + its reason stand.
uint64_t draw_scrubber(StepIndex &idx, uint64_t playhead,
                       const Recording *rec = nullptr);

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
