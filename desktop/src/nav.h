// nav.h — the deep-link router (docs/internal/gui/04-replay-views.md T2).
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

enum class dt_view { canvas, timeline, slice, diff };

struct dt_link {
    std::string rec;   // recording id (its basename — doc/streams.h)
    std::string rec_b; // the B side of a diff; empty when there is none
    dt_view view = dt_view::canvas;
    std::optional<uint32_t> step; // a dataflow step index
    std::optional<uint64_t> off;  // a code offset, in the recording's basis
};

// v=<canvas|timeline|slice|diff>; the spelling is the enum's name, both ways.
const char *dt_view_name(dt_view v);
bool dt_view_parse(std::string_view s, dt_view &out);

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
};

void dt_nav_register(dt_nav_table &t, dt_view v, dt_nav_handler h);

// Navigate. False + `last_error` when the link names a recording that is not
// open, or a view with no handler — a silent no-op would leave the user
// looking at the wrong thing believing they had moved.
bool dt_nav_go(dt_nav_table &t, const dt_link &link);

// The keyboard bindings, as data: rendered in the help overlay AND the single
// place the app maps keys, so the two cannot drift.
struct dt_binding {
    const char *keys;
    const char *what;
};
const std::vector<dt_binding> &dt_nav_bindings();

} // namespace asmdesk
#endif // ASMDESK_NAV_H
