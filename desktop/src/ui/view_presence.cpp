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
                                        Mode mode, bool b_attachable,
                                        bool is_live) {
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
    // is the provenance/fidelity chrome; Canvas and the operand Timeline read from
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
    // recorded (the `--steps` opt-in). The absent-reason is GRADED by why the ring
    // is missing (26 T4): a statistical producer never single-steps, so it can
    // never carry one; an exact per-step capture simply did not arm it — and a
    // LIVE one says so in live terms, distinct from a saved emulator recording; a
    // trace-only recording has no per-step stepping at all.
    {
        std::string reason;
        if (a.statistical)
            reason = "a statistical (sampled) producer never single-steps, so it "
                     "records no register ring — the Scrubber needs an exact "
                     "single-stepped capture (emulator --steps, or a live "
                     "--dataflow --steps)";
        else if (a.df.present())
            reason = is_live
                         ? "this live session did not record the register ring — "
                           "re-run the capture with --steps (it single-steps "
                           "dataflow already; --steps also carries each step's "
                           "register file so the Scrubber time-travels it)"
                         : "this recording captured per-step dataflow but not the "
                           "register ring — re-record with --steps to carry each "
                           "step's register file";
        else
            reason = "no regstate register ring in this recording (record with "
                     "--steps, an exact single-stepped capture, to carry per-step "
                     "register files)";
        add(ViewId::Scrubber, "Scrubber", si.present(), reason, std::nullopt);
    }

    // ABI x-ray — locks the SysV leg against an attached Win64 leg (B).
    add(ViewId::AbiXray, "ABI x-ray", b_attachable,
        "attach the other ABI leg (a second recording, press d) — the x-ray "
        "locks this recording against it",
        std::nullopt);

    // 3D overview — the address-space spacetime surface. Present iff the recording
    // carries codeimage regions to place the plane.
    bool has_regions = !space::regions_from_codeimage(r).empty();
    // The reason names the capture modes that record `codeimage`, never "a live
    // maps snapshot": no regions_from_maps exists anywhere in the tree, so that
    // phrasing sent the reader looking for a capture option nobody built. (The
    // producer DOES read /proc/<pid>/maps — serve_exe_text_span — but it emits
    // a `codeimage` from it; there is no viewer-side maps path.)
    add(ViewId::Scene3D, "3D overview", has_regions,
        "no address-space regions in this recording — the 3D overview places "
        "its plane from `codeimage` events, which the serve host records for "
        "the tree / trace / dataflow / auto modes",
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
