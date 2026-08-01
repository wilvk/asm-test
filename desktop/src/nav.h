// nav.h — the deep-link router (docs/internal/archive/gui/04-replay-views.md T2).
//
// Plan D4's navigation spine: ONE internal API for "open recording R (optionally
// against R'), at step S / offset O, in view V". Every view here registers with
// it, every keyboard binding routes through it, and 06's test-failure producer
// and 08's live views emit into it. It exists before the views so they can.
//
// The textual form is the point. A position in a recording is a thing people
// paste into a bug, a commit message, or a test's failure output:
//
//   asmtrace-link:v=slice&rec=add_signed.asmtrace&step=4
//
// so parse and format are pure, total, and round-trip byte-stable — pinned by
// test_nav rather than assumed. Unknown keys are IGNORED (the schema's
// forward-compat rule applied to links: a link written by a newer build must
// still navigate an older one, to its best ability); a missing `v` or `rec` is
// rejected with a reason, because those two are what make a link mean anything.
#ifndef ASMDESK_NAV_H
#define ASMDESK_NAV_H

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asmdesk {

// The four replay views (04), then the live Observer views (08), then the blame
// intake target (09). A link names one of these by the enum's own spelling; an
// older build that does not know a name refuses the link WITH the name in the
// message rather than landing somewhere arbitrary.
enum class dt_view {
    canvas,
    timeline,
    slice,
    diff,
    syscalls,
    watch,
    topo,
    hotedges,
    tree,
    region,
    disasm,
    // NOT a view of its own: the failure-attribution ENTRY POINT (09-T5). The
    // Wave-2 backward-slice blame producer will emit `v=blame&rec=…&step=…`; the
    // shell resolves it onto the SLICE explorer at the failure step with the
    // backward cone ("what produced this wrong value") pre-selected — no new
    // heavy view. Kept a distinct target, not an alias for `v=slice`, so the
    // producer has a stable, greppable socket AND the resolution can later grow
    // (a Loom thread, the specific failing def) without the producer's link
    // format changing. Appended LAST so every link written before it stays
    // byte-identical, exactly as `pid` was on dt_link.
    blame,
};

struct dt_link {
    std::string rec;   // recording id (its basename — doc/streams.h)
    std::string rec_b; // the B side of a diff; empty when there is none
    dt_view view = dt_view::canvas;
    std::optional<uint32_t> step; // a dataflow step index
    std::optional<uint64_t> off;  // a code offset, in the recording's basis
    // A process, for the live views (08-T3's topology drill-in): a session
    // covers a whole descendant TREE, so "which process" is a real coordinate
    // there in a way it never is for a single-routine replay. Appended LAST in
    // the textual form, so every link written before it stays byte-identical.
    std::optional<long> pid;
};

// v=<canvas|timeline|slice|diff|syscalls|watch|topo|hotedges|tree|region|disasm
//   |blame>; the spelling is the enum's name, both ways.
const char *dt_view_name(dt_view v);
bool dt_view_parse(std::string_view s, dt_view &out);
// Every view, for exhaustive iteration (registration, and the round-trip test).
const std::vector<dt_view> &dt_all_views();
// True for the reading views that own an OUTER tab/pane the shell switches to on
// a bare `want_view` intent (canvas/timeline/slice/diff). False for the Observer-
// deck sub-views (syscalls/watch/topo/hotedges/tree/region/disasm — inner tabs
// with no outer switch, reached by drill-in) and for `blame` (an entry point, not
// a view). The command palette offers a "Show X view" only where this is true, so
// it never advertises a switch the shell silently drops.
bool dt_view_has_outer_tab(dt_view v);

// Parse the textual form. False + a human-readable `err` on anything that is
// not a navigable link; never throws, never partially fills `out`.
bool dt_nav_parse(std::string_view s, dt_link &out, std::string &err);

// Format the canonical textual form. Key order is fixed (v, rec, rec_b, step,
// off) so format(parse(x)) is byte-stable and a link can be diffed.
std::string dt_nav_format(const dt_link &link);

// --- the router ------------------------------------------------------------
// Kept free of the shell type so nav.o links into every test binary that needs
// only parse/format. The app owns the table; a view registers one handler per
// dt_view and the shell calls dt_nav_go.
using dt_nav_handler = std::function<void(const dt_link &)>;

struct dt_nav_table {
    struct Entry {
        dt_view view;
        dt_nav_handler handler;
    };
    std::vector<Entry> entries;
    // Which recording ids exist right now (the workspace's, by basename).
    std::function<bool(const std::string &)> have_recording;
    // Set by dt_nav_go on refusal, rendered verbatim in the status bar.
    std::string last_error;
    // The most recent successful navigation, for the `y` copy-link action.
    std::optional<dt_link> current;

    // Bounded back/forward history (18-breach-stops.md T6, F11). A successful
    // dt_nav_go pushes the PREVIOUS `current` onto `back` and clears `forward`
    // (browser-history discipline); dt_nav_back/forward walk them without
    // recording new history. Both are plain serialisable dt_links, so the whole
    // stack round-trips through dt_nav_format/parse and test_nav pins it. `cap`
    // bounds the depth — a long session drops the oldest rather than growing
    // unboundedly.
    std::vector<dt_link> back;
    std::vector<dt_link> forward;
    size_t history_cap = 64;
};

void dt_nav_register(dt_nav_table &t, dt_view v, dt_nav_handler h);

// Navigate. False + `last_error` when the link names a recording that is not
// open, or a view with no handler — a silent no-op would leave the user
// looking at the wrong thing believing they had moved. On success it pushes the
// prior position onto `back` and clears `forward` (F11's history discipline);
// a no-op re-navigation to the current link is NOT pushed.
bool dt_nav_go(dt_nav_table &t, const dt_link &link);

// Walk the history (18-breach-stops.md T6). `dt_nav_back` pops `back`, pushes
// the current position onto `forward`, and re-dispatches through the same router
// path so a back-jump lands identically to a fresh navigation; `dt_nav_forward`
// is the mirror. False + `last_error` at the ends of the stacks, or when the
// target recording is no longer open (the same loud refusal dt_nav_go gives).
// Free of the shell type, exactly like dt_nav_go, so test_nav links them.
bool dt_nav_back(dt_nav_table &t);
bool dt_nav_forward(dt_nav_table &t);

// The keyboard bindings, as data: rendered in the help overlay AND the single
// place the app maps keys, so the two cannot drift. `wired` is the fidelity flag
// (18-breach-stops.md T1, F1/F18): true for a binding handle_keymap (or a
// view-local draw) actually acts on, false for one that is advertised as
// "planned" but not yet mapped. The help overlay greys every !wired row so it
// can never again advertise a dead key as live.
struct dt_binding {
    const char *keys;
    const char *what;
    bool wired = true;
};
const std::vector<dt_binding> &dt_nav_bindings();

} // namespace asmdesk
#endif // ASMDESK_NAV_H
