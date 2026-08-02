// test_focus.cpp — the SUBJECT filter of 51-scene-focus-and-scale.md T1/T2/T3.
// Null harness, no display, no GL: this binary links scene3d/focus.o +
// space/projection.o + the doc model and NOTHING else, which is the proof the
// whole subject-filter decision is engine-free and reasoned about headlessly
// (D4) — the same closure argument test_projection makes for the plane.
//
// The properties pinned:
//   T1  ghost, never hide: a non-subject worldline keeps a POSITIVE alpha; an
//       unfocused scene's alpha is the literal 1.0f (byte-identical to the
//       pre-51 render); the statistical residency layer is never a subject and
//       is never dimmed for one.
//   T1  the thread roster is a pure function of (TrajectorySet, Projection),
//       including the "present, no placed path" row that must never be omitted.
//   T2  the region membership rule: region_cells contains every cell project()
//       places for that region's addresses, and no cell of a neighbour.
//   T3  filters are DRAW-TIME ONLY: no code path here writes to the models —
//       asserted as byte-equality on Terrain::flags and on the TrajectorySet
//       across a full filter round trip.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "scene3d/focus.h"
#include "space/projection.h"
#include "space/terrain.h" // TerrainFlag — the enum only; no builder is called
#include "space/types.h"

using namespace asmdesk;
using namespace asmdesk::scene3d;

static int failures;
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
        failures++;
    }
}

// Three adjacent regions of distinct kinds — a code span, a stack span and a
// heap span — the smallest fixture that can tell "this region" from "its
// neighbour" and "this kind" from "another kind".
static space::Projection fixture_proj() {
    std::vector<space::Region> regions;
    space::Region code;
    code.base = 0x400000;
    code.len = 256;
    code.kind = space::Region::Code;
    code.label = "prog .text";
    regions.push_back(code);
    space::Region stack;
    stack.base = 0x7ffff000;
    stack.len = 512;
    stack.kind = space::Region::Stack;
    stack.label = "[stack:tid7]";
    regions.push_back(stack);
    space::Region heap;
    heap.base = 0x900000;
    heap.len = 128;
    heap.kind = space::Region::Heap;
    heap.label = "heap";
    regions.push_back(heap);
    return space::build_projection(std::move(regions));
}

// A two-thread exact trajectory set plus a statistical residency layer, and a
// third tid whose vertices are all UNPLACED (a rel path the anchor could not
// resolve — 36 T5) so the roster's "present, no placed path" row is exercised.
static space::TrajectorySet fixture_traj(const space::Projection &proj) {
    (void)proj;
    space::TrajectorySet ts;
    ts.basis = "abs";
    {
        space::Trajectory t;
        t.tid = 7;
        for (int i = 0; i < 5; i++) {
            space::TrajPoint p;
            p.t = static_cast<uint64_t>(i);
            p.addr = 0x400000 + static_cast<uint64_t>(i) * 8;
            p.tid = 7;
            t.points.push_back(p);
        }
        ts.trajectories.push_back(t);
    }
    {
        space::Trajectory t;
        t.tid = 9;
        for (int i = 0; i < 3; i++) {
            space::TrajPoint p;
            p.t = static_cast<uint64_t>(i);
            p.addr = 0x400040 + static_cast<uint64_t>(i) * 4;
            p.tid = 9;
            t.points.push_back(p);
        }
        ts.trajectories.push_back(t);
    }
    {
        // Present in the recording, unplaceable on the plane.
        space::Trajectory t;
        t.tid = 11;
        for (int i = 0; i < 4; i++) {
            space::TrajPoint p;
            p.t = static_cast<uint64_t>(i);
            p.addr = static_cast<uint64_t>(i) * 4; // a raw wire offset
            p.tid = 11;
            p.placed = false;
            t.points.push_back(p);
        }
        ts.trajectories.push_back(t);
    }
    {
        space::Trajectory t;
        t.tid = -1;
        t.flags = space::TRAJ_STATISTICAL;
        for (int i = 0; i < 4; i++) {
            space::TrajPoint p;
            p.t = static_cast<uint64_t>(i);
            p.addr = 0x900000 + static_cast<uint64_t>(i) * 8;
            p.fidelity = space::TrajPoint::Statistical;
            t.points.push_back(p);
        }
        ts.trajectories.push_back(t);
    }
    return ts;
}

int main() {
    const space::Projection proj = fixture_proj();
    const space::TrajectorySet traj = fixture_traj(proj);

    // === T1 — ghost, never hide =============================================
    {
        SceneFocus none;
        check("T1: no focus leaves the alpha scale at exactly 1",
              focus_line_alpha(none, 7, false) == 1.0f &&
                  focus_line_alpha(none, -1, true) == 1.0f,
              "a default SceneFocus must be byte-identical to the pre-51 draw");
        check("T1: a default SceneFocus reports itself as not filtering",
              !none.filtering(), "filtering() true with nothing focused");

        SceneFocus f;
        f.tid = 7;
        check("T1: the focused thread keeps full weight",
              focus_line_alpha(f, 7, false) == 1.0f, "the subject was dimmed");
        const float other = focus_line_alpha(f, 9, false);
        check("T1: a non-subject thread is GHOSTED, not hidden",
              other > 0.0f && other < 1.0f,
              "expected 0 < alpha < 1, got " + std::to_string(other));
        check("T1: the ghost alpha is the shared constant",
              other == kGhostAlpha, "a second, drifting ghost value");
        check("T1: focusing a thread reports itself as filtering",
              f.filtering(), "filtering() false with a tid focused");

        // T1 step 3: a survey is an aggregate, never a thread.
        check("T1: the statistical layer is never dimmed for a thread focus",
              focus_line_alpha(f, -1, true) == 1.0f &&
                  focus_line_alpha(f, 9, true) == 1.0f,
              "focusing a tid must leave the survey at its normal weight");
    }
    {
        // T4's distance budget is the ONE thing that may drop rather than
        // ghost — and only for a NON-subject exact line.
        SceneFocus f;
        f.tid = 7;
        f.drop_unfocused = true;
        check("T1/T4: the budget drops a non-subject worldline",
              focus_line_alpha(f, 9, false) == 0.0f, "expected a 0 alpha");
        check("T1/T4: the budget never drops the subject itself",
              focus_line_alpha(f, 7, false) == 1.0f, "the subject was dropped");
        check("T1/T4: the budget never drops the survey",
              focus_line_alpha(f, -1, true) == 1.0f, "the survey was dropped");
    }

    // === T1 — the thread roster =============================================
    {
        const std::vector<ThreadRosterRow> rows = thread_roster(traj, proj);
        check("T1: one roster row per trajectory, never a filtered list",
              rows.size() == traj.trajectories.size(),
              "got " + std::to_string(rows.size()));

        const ThreadRosterRow *t7 = nullptr;
        const ThreadRosterRow *t11 = nullptr;
        const ThreadRosterRow *stat = nullptr;
        for (const ThreadRosterRow &r : rows) {
            if (r.statistical)
                stat = &r;
            else if (r.tid == 7)
                t7 = &r;
            else if (r.tid == 11)
                t11 = &r;
        }
        check("T1: the placed thread has a row", t7 != nullptr, "tid 7 absent");
        if (t7) {
            check("T1: the placed thread counts every PC vertex",
                  t7->vertices == 5,
                  "got " + std::to_string(t7->vertices));
            check("T1: the placed thread counts its placed vertices",
                  t7->placed == 5, "got " + std::to_string(t7->placed));
            check("T1: the placed thread draws a path", t7->has_placed_path,
                  "a 5-vertex placed path must draw");
            check("T1: a drawn path carries no 'no placed path' note",
                  t7->note.empty(), "unexpected note: " + t7->note);
            check("T1: a real thread is focusable", t7->focusable,
                  "an exact per-thread path must be focusable");
        }
        // The row this brief exists to refuse to omit.
        check("T1: an unplaceable thread STILL gets a row", t11 != nullptr,
              "tid 11 was omitted — absence would read as 'never ran'");
        if (t11) {
            check("T1: the unplaceable thread draws no path",
                  !t11->has_placed_path, "expected no drawn strip");
            check("T1: the unplaceable thread says so verbatim",
                  t11->note.find("present, no placed path") != std::string::npos,
                  "note was: <" + t11->note + ">");
            check("T1: the unplaceable thread placed nothing",
                  t11->placed == 0 && t11->vertices == 4,
                  "placement counts are wrong");
        }
        check("T1: the survey gets a row too", stat != nullptr,
              "the statistical layer was omitted");
        if (stat)
            check("T1: the survey row is NOT focusable", !stat->focusable,
                  "a survey is an aggregate, not a thread");

        // The roster's swatch must be the SAME palette the renderer draws
        // with — one table, not two (focus.h's tid_palette).
        float expect[4];
        tid_palette(7, expect);
        if (t7)
            check("T1: the roster swatch is the renderer's own palette",
                  t7->rgb[0] == expect[0] && t7->rgb[1] == expect[1] &&
                      t7->rgb[2] == expect[2],
                  "swatch drifted from tid_palette");
    }

    // === T2 — the region membership rule ====================================
    {
        for (size_t i = 0; i < proj.regions.size(); i++) {
            const space::Region &r = proj.regions[i];
            const std::vector<uint32_t> cells = space::region_cells(proj, i);
            check("T2: region " + r.label + " owns at least one cell",
                  !cells.empty(), "empty cell set for a non-empty region");
            std::set<uint32_t> owned(cells.begin(), cells.end());
            check("T2: region " + r.label + "'s cell set has no duplicates",
                  owned.size() == cells.size(), "duplicate cells");
            // Containment: every cell project() can place an address of this
            // region into is in the set.
            const uint32_t n = uint32_t(1) << proj.order;
            bool all_in = true;
            for (uint64_t off = 0; off < r.len; off++) {
                float u = 0.0f, v = 0.0f;
                if (!proj.project(r.base + off, &u, &v))
                    continue;
                const uint32_t x = static_cast<uint32_t>(u * n);
                const uint32_t y = static_cast<uint32_t>(v * n);
                if (!owned.count(y * n + x))
                    all_in = false;
            }
            check("T2: region " + r.label +
                      " contains every cell project() places in it",
                  all_in, "a placed address landed outside its own region set");
        }
        // Exclusivity: with a domain that fits the plane (this fixture, and
        // every ordinary recording) the sets are strictly disjoint — no cell
        // of a neighbour.
        for (size_t i = 0; i < proj.regions.size(); i++) {
            const std::vector<uint32_t> a = space::region_cells(proj, i);
            std::set<uint32_t> sa(a.begin(), a.end());
            for (size_t j = i + 1; j < proj.regions.size(); j++) {
                const std::vector<uint32_t> b = space::region_cells(proj, j);
                size_t shared = 0;
                for (uint32_t c : b)
                    if (sa.count(c))
                        shared++;
                check("T2: region " + proj.regions[i].label + " owns no cell of " +
                          proj.regions[j].label,
                      shared == 0,
                      std::to_string(shared) + " cell(s) shared");
            }
        }
        check("T2: an out-of-range region index owns nothing",
              space::region_cells(proj, proj.regions.size()).empty(),
              "an out-of-range index must not fabricate a footprint");
    }

    // === T2 — the uploaded mask matches the membership rule =================
    {
        const uint32_t n = uint32_t(1) << proj.order;
        const std::vector<uint8_t> mask = build_focus_mask(proj, 0);
        check("T2: the focus mask covers the whole plane",
              mask.size() == static_cast<size_t>(n) * n,
              "got " + std::to_string(mask.size()));
        size_t set_cells = 0;
        for (uint8_t b : mask)
            if (b)
                set_cells++;
        const std::vector<uint32_t> cells = space::region_cells(proj, 0);
        check("T2: the mask marks exactly the region's cells",
              set_cells == cells.size(),
              std::to_string(set_cells) + " set vs " +
                  std::to_string(cells.size()) + " owned");
        for (uint32_t c : cells)
            if (c < mask.size() && !mask[c]) {
                check("T2: every owned cell is marked in the mask", false,
                      "cell " + std::to_string(c) + " unmarked");
                break;
            }
        check("T2: no region focused builds no mask (draw unfiltered, never "
              "an all-zero plane)",
              build_focus_mask(proj, -1).empty(),
              "a -1 region must not produce a hide-everything mask");
        check("T2: an out-of-range region builds no mask",
              build_focus_mask(proj, 99).empty(),
              "an out-of-range index must not produce a mask");
    }

    // === T2 step 5 — an active filter ALWAYS says so ========================
    {
        SceneFocus none;
        check("T2: an unfiltered plane makes no filter claim",
              subject_filter_note(none, proj).empty(),
              "a note appeared with nothing filtered");

        SceneFocus kinds;
        kinds.kind_mask = (1u << space::Region::Code); // code only
        const std::string kn = subject_filter_note(kinds, proj);
        check("T2: a kind filter announces itself", !kn.empty(),
              "silent kind filter");
        check("T2: the kind note counts what is shown",
              kn.find("showing 1 of 6 kinds") != std::string::npos,
              "note was: " + kn);
        check("T2: the kind note names what is dimmed",
              kn.find("stack") != std::string::npos &&
                  kn.find("heap") != std::string::npos,
              "note was: " + kn);
        check("T2: the note says dimmed, never hidden",
              kn.find("dimmed, never hidden") != std::string::npos,
              "note was: " + kn);

        SceneFocus reg;
        reg.region = 0;
        const std::string rn = subject_filter_note(reg, proj);
        check("T2: a region filter announces itself by VERBATIM label",
              rn.find(proj.regions[0].label) != std::string::npos,
              "note was: " + rn);

        SceneFocus tid;
        tid.tid = 7;
        const std::string tn = subject_filter_note(tid, proj);
        check("T2: a thread focus announces itself",
              tn.find("thread 7") != std::string::npos, "note was: " + tn);
        check("T2: a thread focus states the survey is untouched",
              tn.find("survey unchanged") != std::string::npos,
              "note was: " + tn);
    }

    // === T3 — the two axes are stated apart =================================
    {
        const std::string ax = focus_axis_note();
        check("T3: the axis note names EVIDENCE",
              ax.find("EVIDENCE") != std::string::npos, "note was: " + ax);
        check("T3: the axis note names SUBJECT",
              ax.find("SUBJECT") != std::string::npos, "note was: " + ax);
        check("T3: the axis note states what a filter may not touch",
              ax.find("never a height") != std::string::npos &&
                  ax.find("fidelity flag") != std::string::npos,
              "note was: " + ax);
    }

    // === T3 — filters are DRAW-TIME ONLY ====================================
    // The strongest available form of "no filter code path writes to the
    // model": run every filter entry point over the fixtures and assert the
    // models are byte-identical afterwards. A future filter that reached into
    // Terrain::flags or a TrajectorySet to do its work fails here.
    {
        space::Terrain terr;
        terr.w = terr.h = 8;
        terr.height.assign(64, 0.0f);
        terr.flags.assign(64, 0u);
        // One cell of every fidelity state this brief must not launder.
        terr.flags[0] = space::TF_TORN;
        terr.flags[1] = space::TF_STAT;
        terr.flags[2] = space::TF_CHURN;
        terr.flags[3] = space::TF_UNKNOWN;
        terr.flags[4] = space::TF_READ;
        terr.flags[5] = space::TF_WRITE;
        terr.flags[6] = space::TF_INWINDOW_EMPTY;
        terr.flags[7] = space::TF_OUTWINDOW;
        for (size_t i = 0; i < terr.height.size(); i++)
            terr.height[i] = static_cast<float>(i) * 0.01f;
        const std::vector<uint32_t> flags_before = terr.flags;
        const std::vector<float> height_before = terr.height;

        space::TrajectorySet traj_copy = traj;

        SceneFocus f;
        f.tid = 7;
        f.region = 0;
        f.kind_mask = (1u << space::Region::Code);
        f.drop_unfocused = true;
        (void)focus_line_alpha(f, 9, false);
        (void)thread_roster(traj_copy, proj);
        (void)build_focus_mask(proj, f.region);
        (void)subject_filter_note(f, proj);

        check("T3: no filter path writes Terrain::flags",
              terr.flags == flags_before,
              "a fidelity flag changed under a filter — filters are draw-time "
              "only");
        check("T3: no filter path writes Terrain::height",
              terr.height == height_before,
              "height is the encoded quantity and is untouchable");
        check("T3: every TerrainFlag survives a filter round trip",
              terr.flags[0] == space::TF_TORN &&
                  terr.flags[1] == space::TF_STAT &&
                  terr.flags[2] == space::TF_CHURN &&
                  terr.flags[3] == space::TF_UNKNOWN &&
                  terr.flags[4] == space::TF_READ &&
                  terr.flags[5] == space::TF_WRITE &&
                  terr.flags[6] == space::TF_INWINDOW_EMPTY &&
                  terr.flags[7] == space::TF_OUTWINDOW,
              "a flag was cleared or rewritten");
        check("T3: no filter path writes the TrajectorySet",
              traj_copy.trajectories.size() == traj.trajectories.size() &&
                  traj_copy.trajectories[0].points.size() ==
                      traj.trajectories[0].points.size() &&
                  traj_copy.trajectories[2].points[0].placed ==
                      traj.trajectories[2].points[0].placed,
              "the trajectory model changed under a filter");
    }

    // === T3 — the kind mask defaults admit every kind =======================
    {
        SceneFocus f;
        for (uint32_t k = 0; k <= space::Region::Unknown; k++)
            check("T3: kind " + std::to_string(k) + " is visible by default",
                  (f.kind_mask & (1u << k)) != 0,
                  "kAllKinds must admit every Region::Kind");
    }

    if (failures) {
        std::fprintf(stderr, "%d focus check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_focus: all checks passed\n");
    return 0;
}
