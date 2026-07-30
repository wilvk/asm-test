// terms.h — the in-app term registry + domain-term-first chrome (24 T3 / F3).
//
// The registry is GENERATED from the ONE Sphinx glossary (docs/project/
// glossary.md) by scripts/gen-terms.py into ui/terms_generated.h, exactly as the
// keymap help is fed from dt_nav_bindings() — one source, so a tooltip, a legend
// and the docs can never disagree. This header is the app-facing API over that
// generated table plus the domain-term-first headings, the hoverable term
// wrapper, the per-view "?" caveats, and the searchable Terms pane.
//
// Draw-half only where it draws (imgui.h for the types); the lookup half is pure
// and links into the null test backend (D4).
#ifndef ASMDESK_UI_TERMS_H
#define ASMDESK_UI_TERMS_H

#include <functional>
#include <string>

#include "imgui.h"

namespace asmdesk {

// --- the glossary-sourced registry (one source) ------------------------------
int dt_terms_count();
const char *dt_term_name(int i);
const char *dt_term_def(int i);

// The definition for `word` (case-insensitive exact match on the headword), or
// "" if the glossary does not define it. Never a broken tooltip — an absent term
// degrades to plain text.
std::string dt_term_lookup(const char *word);

// --- the per-view domain-term-first metadata (one table) ---------------------
// Each coined surface leads with the canonical DOMAIN term, the metaphor as a
// subtitle, and carries the verbatim truthful metric CAVEAT the "?" popover shows.
// Stored next to the view (here), not in prose, so the words cannot drift.
struct dt_view_meta {
    const char *key;      // stable id (matches the view/pane)
    const char *domain;   // the canonical domain term (leads)
    const char *metaphor; // the coined metaphor (subtitle)
    const char *caveat;   // what the view shows / what its metric means
};
int dt_view_meta_count();
const dt_view_meta *dt_view_meta_at(int i);
// The metadata for `key`, or nullptr if none is registered.
const dt_view_meta *dt_view_meta_get(const char *key);

// --- draw helpers (app + null backend) ---------------------------------------
// A domain-term-first heading: bold `domain`, dim "(`metaphor`)" subtitle.
void dt_view_heading(const char *domain, const char *metaphor);

// Render `word` and, on hover, a tooltip with its glossary definition. Degrades
// silently to plain text if the term is absent.
void dt_term(const char *word);

// A small "?" affordance for a view header. Opens a popover stating what the
// view shows and what its metric means (the registered caveat for `key`). If
// `on_reopen_primer` is set, the popover also offers "show primer again" (T5's
// re-open handle). Returns true if the caret was clicked this frame.
bool dt_view_help_button(const char *key,
                         const std::function<void()> &on_reopen_primer = {});

// The whole heading block for a view: domain-first heading + the "?" caveat
// button, sourced from dt_view_meta_get(key). A no-op for an unregistered key
// (the caller still draws its body).
void dt_view_header(const char *key,
                    const std::function<void()> &on_reopen_primer = {});

// The searchable Terms pane (reuses the doc-16 ImSearch idiom, guarded on the
// ImSearch context so the null backend degrades to a plain scrollable list).
void dt_terms_pane();

} // namespace asmdesk
#endif // ASMDESK_UI_TERMS_H
