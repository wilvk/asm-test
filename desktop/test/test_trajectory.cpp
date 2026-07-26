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
// Two more pin the features the brief also builds: per-tid grouping, and the
// gated `mem` access marks.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "space/projection.h"
#include "space/trajectory.h"

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

    if (failures) {
        std::fprintf(stderr, "%d trajectory check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_trajectory: all checks passed\n");
    return 0;
}
