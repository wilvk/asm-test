// test_view_presence.cpp — the data-driven view set (20-workspace-and-settings.md
// T1, F4). Null backend, model state (D4): over the committed fixtures assert
// that the view set is a PURE function of the recording's data × the mode — a
// bare recording yields Summary/Canvas/Timeline present and the reveal views
// absent, EACH with a non-empty machine reason; a codeimage recording flips 3D
// to present; and the "unavailable views (N)" count equals the absent entries.
#include <cstdio>
#include <string>

#include "analysis/stepindex.h"
#include "doc/recording.h"
#include "doc/streams.h"
#include "ui/view_presence.h"
#include "views/observer_draw.h"

#ifndef ASMTEST_FIXTURE_DIR
#error "ASMTEST_FIXTURE_DIR must be defined by the build (mk/desktop.mk)"
#endif
#ifndef ASMTEST_GOLDEN_DIR
#error "ASMTEST_GOLDEN_DIR must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

static const ViewPresence *find(const std::vector<ViewPresence> &vp, ViewId id) {
    for (const ViewPresence &e : vp)
        if (e.id == id)
            return &e;
    return nullptr;
}

// Load a recording, decode/build everything the predicate reads, and return the
// presence set for `mode` × `b_attachable`.
static std::vector<ViewPresence> presence_of(const std::string &path, Mode mode,
                                             bool b_attachable) {
    std::string err;
    auto rec = load_recording_file(path, err);
    if (!rec) {
        std::fprintf(stderr, "FAIL load %s: %s\n", path.c_str(), err.c_str());
        failures++;
        return {};
    }
    Streams a = decode_streams(*rec);
    StepIndex si = build_step_index(*rec);
    ObserverState obs;
    observer_build(obs, *rec);
    return view_presence(a, obs, si, *rec, mode, b_attachable);
}

int main() {
    const std::string fx = std::string(ASMTEST_FIXTURE_DIR) + "/";
    const std::string gd = std::string(ASMTEST_GOLDEN_DIR) + "/";

    // A bare recording: the lean trio present, the reveal views absent + named.
    {
        auto vp = presence_of(fx + "min-trace.asmtrace", Mode::Open,
                              /*b_attachable=*/false);
        check("min/summary", find(vp, ViewId::Summary) &&
                                 find(vp, ViewId::Summary)->present,
              "Summary is always present");
        check("min/canvas", find(vp, ViewId::Canvas) &&
                                find(vp, ViewId::Canvas)->present,
              "Canvas is in the lean default");
        check("min/timeline", find(vp, ViewId::Timeline) &&
                                  find(vp, ViewId::Timeline)->present,
              "Timeline is in the lean default");
        for (ViewId id : {ViewId::Loom, ViewId::Scrubber, ViewId::Scene3D,
                          ViewId::Diff, ViewId::AbiXray}) {
            const ViewPresence *e = find(vp, id);
            check("min/reveal-absent", e && !e->present,
                  "a reveal view must be absent for a bare recording");
            check("min/reveal-named", e && !e->reason.empty(),
                  "an absent view must carry a non-empty machine reason (never "
                  "a vague 'unavailable')");
            // A reason may not send the reader after something that does not
            // exist. The Scene3D reason offered "a live maps snapshot" as an
            // alternative region source; the only Region producers in the tree
            // are regions_from_codeimage and observed_data_spans, and no
            // regions_from_maps exists anywhere.
            check("min/reason-no-phantom-source",
                  e && e->reason.find("maps snapshot") == std::string::npos,
                  "an absent-reason must not name a capability the tree does "
                  "not implement — no regions_from_maps exists");
        }
        // The affordance count equals the absent entries.
        size_t nabs = view_absent_count(vp), counted = 0;
        for (const ViewPresence &e : vp)
            if (!e.present)
                counted++;
        check("min/absent-count", nabs == counted && nabs > 0,
              "unavailable-views (N) must equal the absent entries");
    }

    // b_attachable flips Diff + ABI x-ray present (a second recording is open).
    {
        auto vp =
            presence_of(fx + "min-trace.asmtrace", Mode::Open, /*b=*/true);
        check("battach/diff", find(vp, ViewId::Diff) &&
                                  find(vp, ViewId::Diff)->present,
              "Diff is present once a B is attachable");
        check("battach/abixray", find(vp, ViewId::AbiXray) &&
                                     find(vp, ViewId::AbiXray)->present,
              "ABI x-ray is present once a B is attachable");
    }

    // A codeimage-bearing golden recording flips 3D to present.
    {
        auto vp =
            presence_of(gd + "scene-abs-loop.asmtrace", Mode::Open, false);
        const ViewPresence *e = find(vp, ViewId::Scene3D);
        check("codeimage/3d-present", e && e->present,
              "a codeimage recording makes the 3D overview present");
    }

    // 59 T1: three of the five substrates need no address plane, so codeimage
    // must not gate the whole pane. build_divergence_scene, build_module_ribbon
    // and build_lane_prism take no space:: parameter, and StandaloneFrame
    // carries no terrain field — the plane is ONE substrate among five, not a
    // precondition for the other four. Gating ViewId::Scene3D on
    // regions_from_codeimage made a tree-only or dataflow-only recording unable
    // to reach its own scene.
    {
        // A call tree and nothing else: no codeimage, so no plane.
        auto vp = presence_of(fx + "obs-tree.asmtrace", Mode::Open, false);
        const ViewPresence *e = find(vp, ViewId::Scene3D);
        check("3d/tree-fixture-present", e && e->present,
              "a recording with a call tree can fill the module excursion "
              "ribbon, which needs no plane — the pane must open for it");
    }
    {
        // Nothing at all: no codeimage, no calls, no coverage blocks, no wide
        // writes. The pane must STILL be absent — widening a gate is not
        // removing it — and must name what WOULD have opened it.
        auto vp = presence_of(fx + "min-trace.asmtrace", Mode::Open, false);
        const ViewPresence *e = find(vp, ViewId::Scene3D);
        check("3d/min-absent", e && !e->present,
              "a recording with no substrate at all must not open the pane");
        check("3d/min-reason-names-all",
              e && e->reason.find("codeimage") != std::string::npos &&
                  e->reason.find("call") != std::string::npos,
              "the reason must name every substrate that would have opened the "
              "pane, not codeimage alone");
    }

    // Author mode hides the live-only Observer deck, and names WHY.
    {
        auto vp = presence_of(fx + "min-trace.asmtrace", Mode::Author, false);
        const ViewPresence *e = find(vp, ViewId::Observer);
        check("author/observer-hidden", e && !e->present,
              "the Observer deck is live-capture-only — absent in Author mode");
        check("author/observer-named", e && !e->reason.empty(),
              "a mode-scoped absence must still name its reason");
    }

    if (failures) {
        std::fprintf(stderr, "test_view_presence: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_view_presence: all checks passed\n");
    return 0;
}
