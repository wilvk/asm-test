// test_trajectory.cpp — the execution-trajectory builder
// (docs/internal/gui/10-spacetime-3d-overview.md T3). Null harness, no display:
// this binary links space/trajectory.o + space/projection.o + doc/recording.o
// and NOTHING else — that link line is the proof the render-only viewer can
// thread a recording's PC path over the terrain plane with zero engine
// dependencies (D4), the same argument test_projection makes for the plane.
//
// The four cases the brief pins: an `abs` fixture yields vertices at the
// projected cells in step order; a `rel` fixture sets RELATIVE_BASIS; a mixed
// fixture is refused with a diagnostic; a `survey` fixture is all Statistical.
// Three more pin the features the brief also builds: per-tid grouping, the
// gated `mem` access marks, and the `df_step` fallback (a live single-step
// capture with no `trace` — doc 10 T5 / doc 25 T6 — woven region-relative).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "space/projection.h"
#include "space/trajectory.h"
#include "ui/scene_gate.h" // the GL host's pure re-upload gate (headless-testable)

using namespace asmdesk;
using namespace asmdesk::space;

static int failures;

static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}

static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

// Load a recording from an NDJSON string, exactly as the loader parses a file.
static Recording load(const std::string &ndjson) {
    std::istringstream in(ndjson);
    std::string err;
    auto rec = load_recording(in, err);
    if (!rec) {
        fail("fixture load", err);
        return Recording{};
    }
    return *rec;
}

// A valid exact header; callers append event lines.
static const char *kHdrExact =
    "{\"asmtrace\":1,\"container\":\"ndjson\",\"producer\":{\"name\":\"test\","
    "\"version\":\"0\"},\"provenance\":{\"backend\":\"emu-l0\",\"exact\":true,"
    "\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n";
// A statistical header (a survey stream MUST set exact:false).
static const char *kHdrStat =
    "{\"asmtrace\":1,\"container\":\"ndjson\",\"producer\":{\"name\":\"test\","
    "\"version\":\"0\"},\"provenance\":{\"backend\":\"ibs-op\",\"exact\":false,"
    "\"trust\":\"statistical\"},\"arch\":\"x86_64\"}\n";

// Count the PC vertices (is_access == false) in a trajectory.
static size_t pc_count(const Trajectory &t) {
    size_t n = 0;
    for (const TrajPoint &p : t.points)
        if (!p.is_access)
            n++;
    return n;
}

// A single-code-span plane, exactly as regions_from_codeimage yields for a live
// serve session that scoped one region (36 T2 anchors a rel path against it).
static Projection code_span(uint64_t base, uint64_t len) {
    std::vector<Region> regs;
    Region c;
    c.base = base;
    c.len = len;
    c.kind = Region::Code;
    regs.push_back(c);
    return build_projection(std::move(regs));
}

// 36 T2 regression bar: every BUILT PC vertex is either placed on the plane or
// explained (a note / a refusal). The ABSENCE of this bar is what let a total
// placement failure look like success — so it runs at the end of every T2 case.
static void placed_or_explained(const std::string &what,
                                const TrajectorySet &ts,
                                const Projection &proj) {
    const bool explained = !ts.placement_note.empty() || ts.refused();
    for (const Trajectory &tr : ts.trajectories)
        for (const TrajPoint &p : tr.points) {
            if (p.is_access)
                continue;
            float u = 0, v = 0;
            const bool on_plane = proj.project(p.addr, &u, &v);
            check(what + ": a built vertex is placed or explained",
                  on_plane || explained,
                  "vertex addr " + std::to_string(p.addr) +
                      " neither projects nor is explained");
        }
}

int main() {
    // === abs: vertices at the projected cells, in step order ================
    {
        std::string nd = kHdrExact;
        nd += "{\"k\":\"trace\",\"basis\":\"abs\",\"kind\":\"insn\",\"off\":"
              "4194304,\"disasm\":\"a\"}\n"; // 0x400000
        nd +=
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194308}\n"; // 0x400004
        nd +=
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194312}\n"; // 0x400008
        TrajectorySet ts = build_trajectories(load(nd));

        check("abs is not refused", !ts.refused(), ts.diagnostic);
        check("abs basis recorded", ts.basis == "abs",
              "got '" + ts.basis + "'");
        check("abs is a single replay trajectory", ts.trajectories.size() == 1,
              "got " + std::to_string(ts.trajectories.size()));
        if (ts.trajectories.size() == 1) {
            const Trajectory &tr = ts.trajectories[0];
            check("abs trajectory is tid -1 (replay)", tr.tid == -1, "tid set");
            check("abs is not flagged relative-basis",
                  (tr.flags & TRAJ_RELATIVE_BASIS) == 0, "rel flag set");
            check("abs has three PC vertices", pc_count(tr) == 3,
                  "got " + std::to_string(pc_count(tr)));

            // The vertices are the three offsets, in step order, all Exact.
            const uint64_t want[3] = {0x400000, 0x400004, 0x400008};
            for (size_t i = 0; i < tr.points.size() && i < 3; i++) {
                check("abs vertex is Exact",
                      tr.points[i].fidelity == TrajPoint::Exact, "statistical");
                check("abs vertex t is the step index", tr.points[i].t == i,
                      "t=" + std::to_string(tr.points[i].t));
                check("abs vertex addr is the recorded address",
                      tr.points[i].addr == want[i],
                      "got " + std::to_string(tr.points[i].addr));
            }

            // "at the projected cells": each abs address projects onto the T1
            // plane, and the three land in step order without a projection
            // failure — an abs trajectory IS a real address-space path.
            std::vector<Region> regs;
            Region code;
            code.base = 0x400000;
            code.len = 0x1000;
            code.kind = Region::Code;
            regs.push_back(code);
            Projection proj = build_projection(std::move(regs));
            for (const TrajPoint &p : tr.points) {
                float u = 0, v = 0;
                check("abs vertex projects onto the plane",
                      proj.project(p.addr, &u, &v),
                      "addr " + std::to_string(p.addr) + " did not project");
            }
        }
        check("abs has no mem stream", !ts.mem_present, "mem_present set");
    }

    // === rel: RELATIVE_BASIS, offsets carried verbatim ======================
    {
        std::string nd = kHdrExact;
        nd +=
            "{\"k\":\"trace\",\"basis\":\"rel\",\"kind\":\"insn\",\"off\":0}\n";
        nd += "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":4}\n";
        nd += "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":8}\n";
        TrajectorySet ts = build_trajectories(load(nd));

        check("rel is not refused", !ts.refused(), ts.diagnostic);
        check("rel basis recorded", ts.basis == "rel",
              "got '" + ts.basis + "'");
        check("rel is a single trajectory", ts.trajectories.size() == 1,
              "got " + std::to_string(ts.trajectories.size()));
        if (ts.trajectories.size() == 1) {
            const Trajectory &tr = ts.trajectories[0];
            check("rel trajectory is flagged RELATIVE_BASIS",
                  (tr.flags & TRAJ_RELATIVE_BASIS) != 0, "flag not set");
            check("rel is not flagged statistical",
                  (tr.flags & TRAJ_STATISTICAL) == 0, "stat flag set");
            const uint64_t want[3] = {0, 4, 8};
            for (size_t i = 0; i < tr.points.size() && i < 3; i++)
                check("rel vertex is the region-relative offset",
                      tr.points[i].addr == want[i] &&
                          tr.points[i].fidelity == TrajPoint::Exact,
                      "off " + std::to_string(tr.points[i].addr));
        }
    }

    // === mixed: refused with a diagnostic ===================================
    {
        std::string nd = kHdrExact;
        nd += "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":0}\n";
        nd += "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n";
        TrajectorySet ts = build_trajectories(load(nd));

        check("mixed bases are refused", ts.refused(), "not refused");
        check("mixed refusal names both bases",
              ts.diagnostic.find("rel") != std::string::npos &&
                  ts.diagnostic.find("abs") != std::string::npos,
              "diagnostic: " + ts.diagnostic);
        check("mixed refusal yields no trajectories (nothing coerced)",
              ts.trajectories.empty(),
              "got " + std::to_string(ts.trajectories.size()));
    }

    // A trace event with NO basis is refused the same way (the schema forbids
    // defaulting it — mirrors the canvas absent-basis path).
    {
        std::string nd = kHdrExact;
        nd += "{\"k\":\"trace\",\"off\":0}\n";
        TrajectorySet ts = build_trajectories(load(nd));
        check("an absent basis is refused", ts.refused(), "not refused");
        check("absent-basis refusal yields no trajectories",
              ts.trajectories.empty(), "got trajectories");
    }

    // === survey: all Statistical, isolated from any exact path ==============
    {
        std::string nd = kHdrStat;
        nd += "{\"k\":\"survey\",\"sampler\":\"ibs-op\",\"edges\":["
              "{\"from_addr\":4194304,\"to_addr\":4194320,\"count\":9},"
              "{\"from_addr\":4194320,\"to_addr\":4194352,\"count\":3}],"
              "\"samples\":12}\n";
        TrajectorySet ts = build_trajectories(load(nd));

        check("survey is not refused", !ts.refused(), ts.diagnostic);
        check("survey produces a residency layer", !ts.trajectories.empty(),
              "no trajectories");
        size_t exact_paths = 0;
        for (const Trajectory &tr : ts.trajectories) {
            check("every survey trajectory is flagged STATISTICAL",
                  (tr.flags & TRAJ_STATISTICAL) != 0, "flag missing");
            for (const TrajPoint &p : tr.points)
                check("every survey point is Statistical",
                      p.fidelity == TrajPoint::Statistical, "exact point");
            bool any_exact = false;
            for (const TrajPoint &p : tr.points)
                if (p.fidelity == TrajPoint::Exact)
                    any_exact = true;
            if (any_exact || (tr.flags & TRAJ_STATISTICAL) == 0)
                exact_paths++;
        }
        check("a survey-only recording has NO exact trajectory (isolation)",
              exact_paths == 0,
              "an exact path leaked into the statistical set");
        // Two edges -> four endpoints, none joined to an exact tube.
        size_t pts = 0;
        for (const Trajectory &tr : ts.trajectories)
            pts += tr.points.size();
        check("survey endpoints are present", pts == 4,
              "got " + std::to_string(pts));
    }

    // === per-tid grouping: a live feed tags events; each tid is its own path =
    // Replay omits tid (single -1 trajectory, covered above); a feed that tags
    // trace events per thread splits into one Trajectory per tid, in step order.
    {
        std::string nd = kHdrExact;
        nd += "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304,\"tid\":7}\n";
        nd += "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":8192,\"tid\":9}\n";
        nd += "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194308,\"tid\":7}\n";
        TrajectorySet ts = build_trajectories(load(nd));

        check("per-tid: two trajectories", ts.trajectories.size() == 2,
              "got " + std::to_string(ts.trajectories.size()));
        bool saw7 = false, saw9 = false;
        for (const Trajectory &tr : ts.trajectories) {
            if (tr.tid == 7) {
                saw7 = true;
                check("tid 7 kept both its vertices", pc_count(tr) == 2,
                      "got " + std::to_string(pc_count(tr)));
                if (tr.points.size() == 2) {
                    check("tid 7 vertices are in step order",
                          tr.points[0].t == 0 && tr.points[1].t == 1 &&
                              tr.points[0].addr == 0x400000 &&
                              tr.points[1].addr == 0x400004,
                          "out of order");
                    check("tid 7 stamps its points with its tid",
                          tr.points[0].tid == 7, "tid not stamped");
                }
            } else if (tr.tid == 9) {
                saw9 = true;
                check("tid 9 has its one vertex", pc_count(tr) == 1,
                      "got " + std::to_string(pc_count(tr)));
            }
        }
        check("per-tid: both threads present", saw7 && saw9,
              "a tid is missing");
    }

    // === df_step fallback: a live single-step capture carries no `trace` =====
    // A serve dataflow/auto session (and a --dataflow file) emits df_step and no
    // trace (SERVE_MODES). Its PC path is the df_step offset stream, woven
    // region-relative (RELATIVE_BASIS) so the 3D overview labels it rather than
    // faking an absolute address-space path (doc 10 T5 / doc 25 T6). Without
    // this the live single-step 3D pane shows terrain with no execution path.
    {
        std::string nd = kHdrExact;
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"disasm\":\"a\"}\n";
        nd += "{\"k\":\"df_step\",\"step\":1,\"off\":2}\n";
        nd += "{\"k\":\"df_step\",\"step\":2,\"off\":6}\n";
        TrajectorySet ts = build_trajectories(load(nd));

        check("df_step is not refused", !ts.refused(), ts.diagnostic);
        check("df_step path is region-relative", ts.basis == "rel",
              "got '" + ts.basis + "'");
        check("df_step is a single trajectory", ts.trajectories.size() == 1,
              "got " + std::to_string(ts.trajectories.size()));
        if (ts.trajectories.size() == 1) {
            const Trajectory &tr = ts.trajectories[0];
            check("df_step trajectory is flagged RELATIVE_BASIS",
                  (tr.flags & TRAJ_RELATIVE_BASIS) != 0, "flag not set");
            check("df_step is not flagged statistical",
                  (tr.flags & TRAJ_STATISTICAL) == 0, "stat flag set");
            check("df_step has three PC vertices", pc_count(tr) == 3,
                  "got " + std::to_string(pc_count(tr)));
            const uint64_t want[3] = {0, 2, 6};
            for (size_t i = 0; i < tr.points.size() && i < 3; i++)
                check("df_step vertex is the offset in step order",
                      tr.points[i].addr == want[i] && tr.points[i].t == i &&
                          tr.points[i].fidelity == TrajPoint::Exact,
                      "off " + std::to_string(tr.points[i].addr));
        }
    }

    // `trace` is authoritative: an emulator dataflow file carries BOTH df_step
    // and an explicit trace basis line. The trace path wins and df_step adds no
    // second, duplicate trajectory (else the path would be drawn twice).
    {
        std::string nd = kHdrExact;
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":0}\n";
        nd += "{\"k\":\"df_step\",\"step\":1,\"off\":2}\n";
        nd += "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n";
        TrajectorySet ts = build_trajectories(load(nd));
        check("trace wins over df_step (basis stays abs)", ts.basis == "abs",
              "got '" + ts.basis + "'");
        check("trace wins over df_step (one path, no duplicate)",
              ts.trajectories.size() == 1,
              "got " + std::to_string(ts.trajectories.size()));
        if (ts.trajectories.size() == 1)
            check("the surviving path is the trace vertex, not df_step",
                  pc_count(ts.trajectories[0]) == 1 &&
                      ts.trajectories[0].points[0].addr == 0x400000,
                  "df_step leaked into the trace path");
    }

    // df_step also splits per tid, when a live multi-thread feed tags the steps
    // (the same grouping the trace path uses).
    {
        std::string nd = kHdrExact;
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"tid\":7}\n";
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"tid\":9}\n";
        nd += "{\"k\":\"df_step\",\"step\":1,\"off\":4,\"tid\":7}\n";
        TrajectorySet ts = build_trajectories(load(nd));
        check("df_step per-tid: two trajectories", ts.trajectories.size() == 2,
              "got " + std::to_string(ts.trajectories.size()));
        check("df_step per-tid: still region-relative", ts.basis == "rel",
              "got '" + ts.basis + "'");
        for (const Trajectory &tr : ts.trajectories)
            check("df_step per-tid path stays RELATIVE_BASIS",
                  (tr.flags & TRAJ_RELATIVE_BASIS) != 0, "flag not set");
    }

    // === mem access marks: gated on the kind, attached to the PC step =======
    // Hand-authored `mem` lines (the kind is reserved, no v1 producer) exercise
    // the rich rung. The gate is "is the mem kind present?" — inert otherwise.
    {
        std::string nd = kHdrExact;
        nd += "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"; // step 0
        nd += "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194308}\n"; // step 1
        nd += "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194312}\n"; // step 2
        // an access at step 1 to a data address
        nd +=
            "{\"k\":\"mem\",\"step\":1,\"ea\":6291456,\"size\":8,\"rw\":\"r\"}"
            "\n"; // 0x600000
        TrajectorySet ts = build_trajectories(load(nd));

        check("mem: the kind-present gate fired", ts.mem_present,
              "mem_present not set");
        check("mem: still one trajectory", ts.trajectories.size() == 1,
              "got " + std::to_string(ts.trajectories.size()));
        if (ts.trajectories.size() == 1) {
            const Trajectory &tr = ts.trajectories[0];
            check("mem: three PC vertices survive", pc_count(tr) == 3,
                  "got " + std::to_string(pc_count(tr)));
            size_t accesses = 0;
            const TrajPoint *mark = nullptr;
            for (const TrajPoint &p : tr.points)
                if (p.is_access) {
                    accesses++;
                    mark = &p;
                }
            check("mem: exactly one access mark", accesses == 1,
                  "got " + std::to_string(accesses));
            if (mark) {
                check("mem: access mark sits at its PC step", mark->t == 1,
                      "t=" + std::to_string(mark->t));
                check("mem: access mark carries the data address",
                      mark->addr == 0x600000,
                      "got " + std::to_string(mark->addr));
                check("mem: an access mark is exact when the trace is",
                      mark->fidelity == TrajPoint::Exact, "statistical");
            }
            // Sorted so the PC vertex at t=1 precedes its access mark.
            for (size_t i = 1; i < tr.points.size(); i++)
                if (tr.points[i].is_access)
                    check("mem: the access mark follows its PC vertex",
                          !tr.points[i - 1].is_access &&
                              tr.points[i - 1].t == tr.points[i].t,
                          "access mark is not adjacent to its vertex");
        }
    }

    // A recording with NO mem kind leaves the gate closed (inert path).
    {
        std::string nd = kHdrExact;
        nd += "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n";
        TrajectorySet ts = build_trajectories(load(nd));
        check("no mem kind -> gate stays closed", !ts.mem_present, "gate open");
    }

    // === 36 T2: a df_step rel path ANCHORS to the recording's single span ====
    // The two-arg builder gets the terrain's Projection; its single codeimage
    // code span pins the offsets down, so base+off is the true address.
    {
        const uint64_t base = 0x400000, len = 0x1000;
        Projection proj = code_span(base, len);
        std::string nd = kHdrExact;
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"disasm\":\"a\"}\n";
        nd += "{\"k\":\"df_step\",\"step\":1,\"off\":2}\n";
        nd += "{\"k\":\"df_step\",\"step\":2,\"off\":6}\n";
        TrajectorySet ts = build_trajectories(load(nd), proj);

        check("36 T2 df_step: not refused", !ts.refused(), ts.diagnostic);
        check("36 T2 df_step: basis stays rel (wire basis unchanged)",
              ts.basis == "rel", "got '" + ts.basis + "'");
        check("36 T2 df_step: the set is anchored", ts.anchored,
              "not anchored");
        check("36 T2 df_step: one trajectory", ts.trajectories.size() == 1,
              "got " + std::to_string(ts.trajectories.size()));
        if (ts.trajectories.size() == 1) {
            const Trajectory &tr = ts.trajectories[0];
            check("36 T2 df_step: BOTH rel and anchored flags set",
                  (tr.flags & TRAJ_RELATIVE_BASIS) &&
                      (tr.flags & TRAJ_ANCHORED),
                  "flags=" + std::to_string(tr.flags));
            const uint64_t off[3] = {0, 2, 6};
            for (size_t i = 0; i < tr.points.size() && i < 3; i++) {
                check("36 T2 df_step: addr == base + off",
                      tr.points[i].addr == base + off[i],
                      "got " + std::to_string(tr.points[i].addr));
                check("36 T2 df_step: vertex measured placed",
                      tr.points[i].placed, "placed false");
                // THE assertion whose absence hid the bug: every anchored vertex
                // projects onto the plane the renderer draws it on.
                float u = 0, v = 0;
                check("36 T2 df_step: every anchored vertex projects",
                      proj.project(tr.points[i].addr, &u, &v),
                      "addr " + std::to_string(tr.points[i].addr) +
                          " did not project");
            }
        }
        check("36 T2 df_step: placement counted (all placed)",
              ts.pc_points == 3 && ts.pc_placed == 3,
              std::to_string(ts.pc_placed) + "/" +
                  std::to_string(ts.pc_points));
        placed_or_explained("36 T2 df_step", ts, proj);
    }

    // === 36 T2: a rel `trace` anchors the same way ==========================
    {
        const uint64_t base = 0x400000;
        Projection proj = code_span(base, 0x1000);
        std::string nd = kHdrExact;
        nd += "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":0}\n";
        nd += "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":4}\n";
        nd += "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":8}\n";
        TrajectorySet ts = build_trajectories(load(nd), proj);
        check("36 T2 rel trace: anchored", ts.anchored, "not anchored");
        check("36 T2 rel trace: basis still rel", ts.basis == "rel", ts.basis);
        if (!ts.trajectories.empty()) {
            const Trajectory &tr = ts.trajectories[0];
            check("36 T2 rel trace: both flags",
                  (tr.flags & TRAJ_RELATIVE_BASIS) &&
                      (tr.flags & TRAJ_ANCHORED),
                  "flags");
            check("36 T2 rel trace: addr == base + off",
                  tr.points.size() == 3 && tr.points[0].addr == base &&
                      tr.points[1].addr == base + 4 &&
                      tr.points[2].addr == base + 8,
                  "not placed");
        }
        check("36 T2 rel trace: all placed",
              ts.pc_placed == ts.pc_points && ts.pc_points == 3, "count");
        placed_or_explained("36 T2 rel trace", ts, proj);
    }

    // === 36 T2: two code spans refuse FAITHFULLY (not pick-the-first) ==========
    {
        std::vector<Region> regs;
        Region a;
        a.base = 0x400000;
        a.len = 0x1000;
        a.kind = Region::Code;
        Region b;
        b.base = 0x800000;
        b.len = 0x2000;
        b.kind = Region::Code;
        regs.push_back(a);
        regs.push_back(b);
        Projection proj = build_projection(std::move(regs));
        std::string nd = kHdrExact;
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":0}\n";
        nd += "{\"k\":\"df_step\",\"step\":1,\"off\":2}\n";
        TrajectorySet ts = build_trajectories(load(nd), proj);
        check("36 T2 two-span: NOT anchored", !ts.anchored, "anchored");
        check(
            "36 T2 two-span: refused() still false (each vertex merely failed)",
            !ts.refused(), "refused");
        check("36 T2 two-span: placement_note names both bases",
              ts.placement_note.find("0x400000") != std::string::npos &&
                  ts.placement_note.find("0x800000") != std::string::npos,
              "note: " + ts.placement_note);
        check("36 T2 two-span: nothing placed", ts.pc_placed == 0,
              std::to_string(ts.pc_placed));
        if (!ts.trajectories.empty()) {
            const Trajectory &tr = ts.trajectories[0];
            check("36 T2 two-span: NO TRAJ_ANCHORED flag",
                  (tr.flags & TRAJ_ANCHORED) == 0, "anchored flag set");
            check("36 T2 two-span: addrs left VERBATIM (raw offsets)",
                  tr.points.size() == 2 && tr.points[0].addr == 0 &&
                      tr.points[1].addr == 2,
                  "addrs mutated");
        }
        placed_or_explained("36 T2 two-span", ts, proj);
    }

    // === 36 T2: an offset past the span is COUNTED, not placed (the clamp) ===
    {
        const uint64_t base = 0x400000, len = 0x1000;
        Projection proj = code_span(base, len);
        std::string nd = kHdrExact;
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":0}\n"; // in span
        nd +=
            "{\"k\":\"df_step\",\"step\":1,\"off\":4096}\n"; // == len: clamped out
        TrajectorySet ts = build_trajectories(load(nd), proj);
        check("36 T2 clamp: anchored", ts.anchored, "not anchored");
        check("36 T2 clamp: two points offered", ts.pc_points == 2,
              std::to_string(ts.pc_points));
        check("36 T2 clamp: one placed, one not (counted, not dropped)",
              ts.pc_placed == 1, std::to_string(ts.pc_placed));
        check("36 T2 clamp: the shortfall note names the clamp",
              ts.placement_note.find("clamp") != std::string::npos,
              "note: " + ts.placement_note);
        if (!ts.trajectories.empty() && ts.trajectories[0].points.size() == 2) {
            const Trajectory &tr = ts.trajectories[0];
            check("36 T2 clamp: in-span vertex placed at base+off",
                  tr.points[0].placed && tr.points[0].addr == base,
                  "in-span not placed");
            check("36 T2 clamp: out-of-span vertex NOT placed, addr verbatim",
                  !tr.points[1].placed && tr.points[1].addr == 4096,
                  "clamp vertex mutated or marked placed");
        }
        placed_or_explained("36 T2 clamp", ts, proj);
    }

    // === 36 T2: no code span leaves the path unanchored and SAYS SO =========
    {
        std::vector<Region> regs;
        Region d;
        d.base = 0x600000;
        d.len = 0x1000;
        d.kind = Region::Data; // a non-code region does not anchor
        regs.push_back(d);
        Projection proj = build_projection(std::move(regs));
        std::string nd = kHdrExact;
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":0}\n";
        nd += "{\"k\":\"df_step\",\"step\":1,\"off\":4}\n";
        TrajectorySet ts = build_trajectories(load(nd), proj);
        check("36 T2 no-code: NOT anchored", !ts.anchored, "anchored");
        check("36 T2 no-code: reason names the missing codeimage code span",
              ts.placement_note.find("no codeimage code span") !=
                  std::string::npos,
              "note: " + ts.placement_note);
        placed_or_explained("36 T2 no-code", ts, proj);
    }

    // === 37 T2: a tagged df_step is placed from a STATED base (rbase) =========
    // THE HEADLINE: two codeimage spans + df_steps tagged with each ⇒ every
    // vertex placed, both spans, NO refusal — precisely the recording 36 T1 must
    // refuse (it cannot derive one base from two spans).
    {
        std::vector<Region> regs;
        Region a;
        a.base = 0x400000;
        a.len = 0x1000;
        a.kind = Region::Code;
        Region b;
        b.base = 0x800000;
        b.len = 0x1000;
        b.kind = Region::Code;
        regs.push_back(a);
        regs.push_back(b);
        Projection proj = build_projection(std::move(regs));
        std::string nd = kHdrExact;
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"rbase\":4194304}\n";
        nd += "{\"k\":\"df_step\",\"step\":1,\"off\":16,\"rbase\":8388608}\n";
        TrajectorySet ts = build_trajectories(load(nd), proj);
        check("37 T2 wire: NOT refused (two spans resolved from the wire)",
              !ts.refused() && ts.anchored, "refused or not anchored");
        check("37 T2 wire: anchor_source is wire", ts.anchor_source == "wire",
              ts.anchor_source);
        check("37 T2 wire: every vertex placed",
              ts.pc_placed == 2 && ts.pc_points == 2,
              std::to_string(ts.pc_placed) + "/" +
                  std::to_string(ts.pc_points));
        if (!ts.trajectories.empty() && ts.trajectories[0].points.size() == 2) {
            const Trajectory &tr = ts.trajectories[0];
            check("37 T2 wire: step0 at rbaseA+0, step1 at rbaseB+16",
                  tr.points[0].addr == 0x400000 &&
                      tr.points[1].addr == 0x800010,
                  "wrong wire addresses");
            check("37 T2 wire: both flags set (still rel, now anchored)",
                  (tr.flags & TRAJ_RELATIVE_BASIS) &&
                      (tr.flags & TRAJ_ANCHORED),
                  "flags");
        }
        placed_or_explained("37 T2 wire", ts, proj);
    }

    // rbase ABSENT with two spans ⇒ STILL refused with 36's reason (fallback).
    {
        std::vector<Region> regs;
        Region a;
        a.base = 0x400000;
        a.len = 0x1000;
        a.kind = Region::Code;
        Region b;
        b.base = 0x800000;
        b.len = 0x1000;
        b.kind = Region::Code;
        regs.push_back(a);
        regs.push_back(b);
        Projection proj = build_projection(std::move(regs));
        std::string nd = kHdrExact;
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":0}\n";
        nd += "{\"k\":\"df_step\",\"step\":1,\"off\":16}\n";
        TrajectorySet ts = build_trajectories(load(nd), proj);
        check("37 T2 fallback: two untagged spans still refuse (36 intact)",
              !ts.anchored && ts.anchor_source.empty() && ts.pc_placed == 0,
              "the fallback refusal broke");
        check("37 T2 fallback: 36's reason names both bases",
              ts.placement_note.find("0x400000") != std::string::npos &&
                  ts.placement_note.find("0x800000") != std::string::npos,
              ts.placement_note);
    }

    // rbase matching NO codeimage span ⇒ placed (base stated), even off-plane.
    {
        Projection proj = code_span(0x400000, 0x1000); // only span A exists
        std::string nd = kHdrExact;
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":16,\"rbase\":8388608}\n";
        TrajectorySet ts = build_trajectories(load(nd), proj);
        check("37 T2 no-image: the wire-stated vertex is PLACED (base stated)",
              ts.anchored && ts.anchor_source == "wire", "not wire-placed");
        if (!ts.trajectories.empty() && !ts.trajectories[0].points.empty())
            check("37 T2 no-image: addr is rbase+off even with no codeimage",
                  ts.trajectories[0].points[0].addr == 0x800010 &&
                      ts.trajectories[0].points[0].placed,
                  "wrong address / not placed");
    }

    // MIXED: one tagged (wire), one untagged (36 single-span) ⇒ anchor_source
    // names both mechanisms.
    {
        Projection proj = code_span(0x400000, 0x1000);
        std::string nd = kHdrExact;
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"rbase\":4194304}\n";
        nd += "{\"k\":\"df_step\",\"step\":1,\"off\":16}\n";
        TrajectorySet ts = build_trajectories(load(nd), proj);
        check("37 T2 mixed: anchor_source is mixed",
              ts.anchor_source == "mixed", ts.anchor_source);
        check("37 T2 mixed: both vertices placed at base+off",
              ts.pc_placed == 2 && !ts.trajectories.empty() &&
                  ts.trajectories[0].points.size() == 2 &&
                  ts.trajectories[0].points[0].addr == 0x400000 &&
                  ts.trajectories[0].points[1].addr == 0x400010,
              "mixed placement wrong");
    }

    // rbase + off equals the ABS path's address for the same instruction.
    {
        Projection proj = code_span(0x400000, 0x1000);
        std::string ndrel = kHdrExact;
        ndrel +=
            "{\"k\":\"df_step\",\"step\":0,\"off\":16,\"rbase\":4194304}\n";
        TrajectorySet rel = build_trajectories(load(ndrel), proj);
        std::string ndabs = kHdrExact;
        ndabs += "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194320}\n";
        TrajectorySet abs = build_trajectories(load(ndabs), proj);
        check("37 T2 equiv: rbase+off == the abs address for the same insn",
              !rel.trajectories.empty() && !abs.trajectories.empty() &&
                  !rel.trajectories[0].points.empty() &&
                  !abs.trajectories[0].points.empty() &&
                  rel.trajectories[0].points[0].addr ==
                      abs.trajectories[0].points[0].addr &&
                  rel.trajectories[0].points[0].addr == 0x400010,
              "wire and abs disagree");
    }

    // The GL host's re-upload gate (ui/scene_gate.h): a LIVE, growing capture
    // keeps ONE identity while its generation advances, so the worldlines must
    // re-upload on growth — not identity alone (the live-3D-freeze defect).
    {
        // First frame: nothing uploaded yet -> must upload.
        check("gate/first", scene_needs_traj_upload(7, 0, 0, 0, false),
              "the first frame always uploads");
        // Same identity, generation advanced (a grown live capture) -> re-upload.
        check("gate/growth", scene_needs_traj_upload(7, 42, 7, 41, true),
              "a growing capture (same id, higher gen) must re-upload worldlines");
        // A new recording (identity changed) -> re-upload.
        check("gate/identity", scene_needs_traj_upload(9, 5, 7, 5, true),
              "a different recording must re-upload");
        // Same identity AND generation (a mere scrub) -> no worldline upload.
        check("gate/scrub-noop", !scene_needs_traj_upload(7, 42, 7, 42, true),
              "an unchanged recording must NOT re-upload the worldlines");
    }

    if (failures) {
        std::fprintf(stderr, "%d trajectory check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_trajectory: all checks passed\n");
    return 0;
}
