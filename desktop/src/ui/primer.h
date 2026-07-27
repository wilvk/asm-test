// primer.h — the first-open in-canvas primer + legend (24 T5 / F4/F3).
//
// The two heaviest surfaces (the Loom and the 3D overview) drop a learner into a
// dense fabric/terrain with no orientation. This is a dismissible, re-openable
// card drawn OVER the view's own draw area (not a modal): a one-paragraph "what
// this is / how to read it" plus the shared legend, with a "Got it" dismiss. It
// shows on FIRST open and never nags again within a session; the per-view "?"
// (ui/terms.h) re-opens it. Until it is dismissed the view holds its lean
// default — nothing heavier than the plain view is front-loaded (F4).
//
// State is a plain per-view struct held in the caller (LoomState / ShellState),
// so the transitions are asserted headlessly (test_primer). Draw-half uses imgui,
// but the state predicates are pure and link into the null backend (D4).
#ifndef ASMDESK_UI_PRIMER_H
#define ASMDESK_UI_PRIMER_H

#include <functional>

#include "imgui.h"

namespace asmdesk {

// One view's primer state. `dismissed` is the only persisted bit; `ever_shown`
// records that the primer was drawn at least once (so a test can tell "not yet
// dismissed" from "never reached").
struct dt_primer_state {
    bool dismissed = false;
    bool ever_shown = false;
};

// Should the primer card be drawn this frame? True until the user dismisses it.
bool dt_primer_active(const dt_primer_state &st);

// Mark it acknowledged (the "Got it" action) / re-open it (the "?" action).
void dt_primer_dismiss(dt_primer_state &st);
void dt_primer_reopen(dt_primer_state &st);

// While the primer is unacknowledged the view holds its LEAN default: heavy
// overlays are suppressed until the user has read the orientation. A view asks
// this before turning on its audit/heaviest layers.
bool dt_primer_lean(const dt_primer_state &st);

// Draw the primer card (title, one-paragraph body, then `legend()`), with a
// "Got it" button that dismisses it. A no-op that returns false once dismissed.
// Returns true if the card was drawn this frame (i.e. still active). `legend`
// may be empty.
bool dt_primer(const char *id, const char *title, const char *body,
               const std::function<void()> &legend, dt_primer_state &st);

} // namespace asmdesk
#endif // ASMDESK_UI_PRIMER_H
