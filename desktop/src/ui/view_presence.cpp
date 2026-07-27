// view_presence.cpp — see view_presence.h (20-workspace-and-settings.md T1).
#include "ui/view_presence.h"

#include "space/terrain.h" // space::regions_from_codeimage (the 3D gate)

namespace asmdesk {

// Is this view even offered in this mode? Author mode leads with the editor and
// hides the live-only Observer deck; every other view is offered in every mode
// and its DATA decides presence. A view absent because of the mode reports that
// as its reason (T1 step 2) — never a blank pane.
static bool mode_offers(ViewId id, Mode mode) {
    if (id == ViewId::Observer && mode == Mode::Author)
        return false;
    return true;
}

std::vector<ViewPresence> view_presence(const Streams &a, const ObserverState &obs,
                                        const StepIndex &si, const Recording &r,
                                        Mode mode, bool b_attachable) {
    std::vector<ViewPresence> v;
    auto add = [&](ViewId id, const char *label, bool present,
                   std::string reason, std::optional<dt_view> view) {
        ViewPresence e;
        e.id = id;
        e.label = label;
        e.view = view;
        // A mode that does not offer the view overrides the data verdict with the
        // mode reason — the truth stays named, just graded by task.
        if (!mode_offers(id, mode)) {
            e.present = false;
            e.reason = "not shown in this mode (Author leads with the editor; "
                       "the Observer deck is live-capture-only)";
        } else {
            e.present = present;
            e.reason = present ? std::string() : std::move(reason);
        }
        v.push_back(std::move(e));
    };

    // The lean default trio — always present with a recording open (F4). Summary
    // is the provenance/honesty chrome; Canvas and the operand Timeline read from
    // the trace/dataflow the recording always carries at least in outline.
    add(ViewId::Summary, "Summary", true, "", std::nullopt);
    add(ViewId::Canvas, "Canvas", true, "", dt_view::canvas);
    add(ViewId::Timeline, "Timeline", true, "", dt_view::timeline);

    // Slice — the per-step def-use walk. Present iff the recording carries a
    // dataflow stream; a trace-only recording has no steps to slice.
    add(ViewId::Slice, "Slice", a.df.present(),
        "no df_step dataflow events — the slice explorer walks per-step def-use "
        "edges, which this recording did not record",
        dt_view::slice);

    // Diff / ABI x-ray — both cross-examine A against a second recording.
    add(ViewId::Diff, "Diff", b_attachable,
        "no second recording is open to compare — open another and attach it "
        "(press d)",
        dt_view::diff);

    // Observer — the live/observer deck. Present iff the capture actually
    // produced any of its kinds (syscall/watch/topo/hot-edge/codeimage).
    add(ViewId::Observer, "Observer", observer_has_any(obs),
        "no live/observer events (syscall / watch / topo / hot-edge / codeimage) "
        "in this recording",
        std::nullopt);

    // Loom — the exact spacetime fabric. loom_fabric_build REFUSES a non-exact
    // provenance and a recording with no per-step values; mirror both here so a
    // statistical or trace-only recording never shows an empty Loom tab.
    {
        bool present = !a.statistical && a.df.present();
        std::string reason =
            a.statistical
                ? "producer was statistical — the Loom weaves only exact "
                  "per-step values, never a sampled guess"
                : "no per-step dataflow values (df_step events) to weave into a "
                  "fabric";
        add(ViewId::Loom, "Loom", present, reason, std::nullopt);
    }

    // Scrubber — the register time-travel deck. Present iff a regstate ring was
    // recorded (the `--steps` opt-in).
    add(ViewId::Scrubber, "Scrubber", si.present(),
        "no regstate register ring in this recording (record with --steps to "
        "carry per-step register files)",
        std::nullopt);

    // ABI x-ray — locks the SysV leg against an attached Win64 leg (B).
    add(ViewId::AbiXray, "ABI x-ray", b_attachable,
        "attach the other ABI leg (a second recording, press d) — the x-ray "
        "locks this recording against it",
        std::nullopt);

    // 3D overview — the address-space spacetime surface. Present iff the recording
    // carries codeimage regions to place the plane.
    bool has_regions = !space::regions_from_codeimage(r).empty();
    add(ViewId::Scene3D, "3D overview", has_regions,
        "no codeimage regions — the 3D overview needs codeimage events (or a "
        "live maps snapshot) to place the address-space plane",
        std::nullopt);

    return v;
}

size_t view_absent_count(const std::vector<ViewPresence> &vp) {
    size_t n = 0;
    for (const ViewPresence &e : vp)
        if (!e.present)
            n++;
    return n;
}

} // namespace asmdesk
