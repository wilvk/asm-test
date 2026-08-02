// test_taint.cpp — the taint isochrone (57-causal-layers.md T3). Null harness:
// space/taint.o + stepplace.o + locate.o + projection.o + analysis/slice.o +
// the doc model, and nothing else.
//
// The load-bearing checks are the two the layer would lose FIRST, because both
// render as nothing by default: a register-only write must tint NO cell (not
// "cell 0, dimly"), and a gap in the recording must read as UNKNOWN, not as
// "the value did not go there".
#include <cstdio>
#include <sstream>
#include <string>

#include "doc/recording.h"
#include "doc/streams.h"
#include "space/projection.h"
#include "space/taint.h"
#include "space/terrain.h" // regions_from_codeimage

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

static Recording mk_rec(const std::string &ndjson) {
    std::istringstream in(ndjson);
    std::string err;
    auto rec = load_recording(in, err);
    if (!rec) {
        fail("load recording", err);
        return Recording{};
    }
    return *rec;
}

static const char *kHdr =
    "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\",\"exact\":"
    "true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n";
// Two regions so an ESCAPE (a reached cell whose region KIND differs from the
// origin's) is expressible at all: a code span and a mapped data span.
static const char *kRegions =
    "{\"k\":\"codeimage\",\"base\":4194304,\"len\":4096,\"version\":0,"
    "\"when\":1,\"bytes\":\"90\"}\n"
    "{\"k\":\"maps\",\"regions\":[{\"base\":8388608,\"len\":4096,\"kind\":"
    "\"heap\",\"label\":\"[heap]\"}]}\n";
static const char *kEnd =
    "{\"k\":\"end\",\"events\":12,\"truncated\":false,\"drops\":{\"lost\":0,"
    "\"throttled\":false}}\n";

// One df_step whose single operand is an ABSOLUTE memory write at `addr`.
static std::string mem_write_step(int step, uint64_t off, uint64_t addr,
                                  bool value_valid = true) {
    std::string s = "{\"k\":\"df_step\",\"step\":" + std::to_string(step) +
                    ",\"off\":" + std::to_string(off) +
                    ",\"rbase\":4194304,\"ops\":[{\"space\":\"abs\",\"addr\":" +
                    std::to_string(addr) + ",\"size\":8,\"write\":true";
    // `value_valid` is an explicit WIRE field (doc/streams.cpp's decode_op),
    // never inferred from `value`'s presence — the fixture states it.
    s += std::string(",\"value_valid\":") + (value_valid ? "true" : "false");
    if (value_valid)
        s += ",\"value\":7";
    return s + "}]}\n";
}
static std::string reg_write_step(int step, uint64_t off) {
    return "{\"k\":\"df_step\",\"step\":" + std::to_string(step) +
           ",\"off\":" + std::to_string(off) +
           ",\"rbase\":4194304,\"ops\":[{\"space\":\"reg\",\"reg\":19,"
           "\"size\":8,\"write\":true,\"value_valid\":true,\"value\":7}]}\n";
}
static std::string edge(int from, int to) {
    return "{\"k\":\"df_edge\",\"from\":" + std::to_string(from) +
           ",\"to\":" + std::to_string(to) + "}\n";
}

int main() {
    // === a DIAMOND gives first-reach depth ==================================
    // 0 -> 1, 0 -> 2, 1 -> 3, 2 -> 3.  Depth: 0@0, 1@1, 2@1, 3@2.
    {
        Recording rec = mk_rec(std::string(kHdr) + kRegions +
                               mem_write_step(0, 0, 4194304 + 64) +
                               mem_write_step(1, 4, 4194304 + 128) +
                               mem_write_step(2, 8, 4194304 + 192) +
                               mem_write_step(3, 12, 4194304 + 256) +
                               edge(0, 1) + edge(0, 2) + edge(1, 3) +
                               edge(2, 3) + kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        check("diamond: setup: four steps", s.df.nsteps == 4,
              "got " + std::to_string(s.df.nsteps));

        TaintFront f = build_taint_front(s.df, p, 0, 64);
        check("diamond: enabled", f.enabled, f.disabled_reason);
        check("diamond: four reached cells", f.reached.size() == 4,
              "got " + std::to_string(f.reached.size()));
        if (f.reached.size() == 4) {
            check("diamond: origin at depth 0",
                  f.reached[0].step == 0 && f.reached[0].depth == 0, "");
            check("diamond: both middles at depth 1 (FIRST reach)",
                  f.reached[1].depth == 1 && f.reached[2].depth == 1, "");
            check("diamond: the join is depth 2, not 3 — first reach, not a "
                  "path length",
                  f.reached[3].step == 3 && f.reached[3].depth == 2,
                  "got depth " + std::to_string(f.reached[3].depth));
            check("diamond: each mark sits at its WRITE's address, not its "
                  "instruction's",
                  f.reached[0].addr == 4194304 + 64, "");
        }
        check("diamond: depth_max is 2", f.depth_max == 2,
              "got " + std::to_string(f.depth_max));
        check("diamond: not bounded (the cap is past the diameter)", !f.bounded,
              "");
        check("diamond: nothing unknown, nothing register-only",
              f.unknown_steps == 0 && f.reg_only_writes == 0 &&
                  f.off_plane == 0,
              "");
        check("the axis is stated as generation, never as time",
              std::string(TaintFront::axis_note()).find("not time") !=
                  std::string::npos,
              TaintFront::axis_note());
    }

    // === a register-only write tints NO cell (the cell set is EMPTY) =========
    {
        Recording rec =
            mk_rec(std::string(kHdr) + kRegions + reg_write_step(0, 0) +
                   reg_write_step(1, 4) + edge(0, 1) + kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        TaintFront f = build_taint_front(s.df, p, 0, 64);
        check("reg-only: enabled", f.enabled, f.disabled_reason);
        // The assertion the brief names explicitly: the cell SET is empty, not
        // "cell 0 happens to be untinted".
        check("reg-only: the tinted-cell set is EMPTY", f.reached.empty(),
              "got " + std::to_string(f.reached.size()) +
                  " cells — a register write carries no address, and "
                  "colouring cell 0 for it is the archetypal fabricated "
                  "placement");
        check("reg-only: counted and stated instead", f.reg_only_writes == 2,
              "got " + std::to_string(f.reg_only_writes));
    }

    // === value_valid == false spreads as a HOLLOW route ======================
    {
        Recording rec = mk_rec(std::string(kHdr) + kRegions +
                               mem_write_step(0, 0, 4194304 + 64) +
                               mem_write_step(1, 4, 4194304 + 128,
                                              /*value_valid=*/false) +
                               edge(0, 1) + kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        TaintFront f = build_taint_front(s.df, p, 0, 64);
        check("hollow: two cells", f.reached.size() == 2,
              "got " + std::to_string(f.reached.size()));
        if (f.reached.size() == 2) {
            check("hollow: the value-bearing route is solid",
                  !f.reached[0].hollow, "");
            check("hollow: the value-LESS route is hollow, not absent",
                  f.reached[1].hollow,
                  "the flow was observed; only the value was not");
        }
    }

    // === an ESCAPE is a comparison of two RECORDED region kinds =============
    {
        Recording rec = mk_rec(std::string(kHdr) + kRegions +
                               mem_write_step(0, 0, 4194304 + 64) +   // code
                               mem_write_step(1, 4, 8388608 + 64) +   // heap
                               edge(0, 1) + kEnd);
        std::vector<Region> regs = regions_from_codeimage(rec);
        std::string note;
        std::vector<Region> obs = observed_data_spans(rec, regs, &note);
        regs.insert(regs.end(), obs.begin(), obs.end());
        Projection p = build_projection(std::move(regs));
        Streams s = decode_streams(rec);
        TaintFront f = build_taint_front(s.df, p, 0, 64);
        check("escape: two cells", f.reached.size() == 2,
              "got " + std::to_string(f.reached.size()) + " (note: " + note +
                  ")");
        if (f.reached.size() == 2) {
            check("escape: the origin's own cell is not an escape",
                  !f.reached[0].escape, "");
            check("escape: a reached cell in a DIFFERENT region kind is",
                  f.reached[1].escape,
                  "kind " + std::to_string(int(f.reached[1].kind)) +
                      " vs origin kind " + std::to_string(int(f.origin_kind)));
        }
        check("escape: the origin's kind is known (else escapes are guesses)",
              f.origin_kind_known, "");
    }

    // === a steps_missing gap renders as UNKNOWN, not as a closed front =======
    {
        // Steps 0 and 2 are described; step 1 is NOT (no df_step covers it),
        // but nsteps reaches it via the edges below.
        Recording rec = mk_rec(std::string(kHdr) + kRegions +
                               mem_write_step(0, 0, 4194304 + 64) +
                               mem_write_step(2, 8, 4194304 + 192) +
                               edge(0, 1) + edge(1, 2) + kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        check("gap: setup: step 1 was never described", !s.df.has_step(1),
              "the fixture must leave a genuine hole");
        check("gap: setup: the decoder counted it", s.df.steps_missing >= 1,
              "got " + std::to_string(s.df.steps_missing));

        TaintFront f = build_taint_front(s.df, p, 0, 64);
        check("gap: the gap is counted as UNKNOWN", f.unknown_steps >= 1,
              "got " + std::to_string(f.unknown_steps) +
                  " — a step the recording never described is not evidence "
                  "the value stopped");
        bool marked = false;
        for (const TaintReach &r : f.reached)
            if (r.step == 0 && r.unknown_beyond)
                marked = true;
        check("gap: the cell the front runs off is POSITIVELY marked", marked,
              "\"the value did not go further\" and \"we did not look\" both "
              "render as nothing by default — the gap must be a positive mark");
        // And the front did NOT close: step 2 is still reached through the gap
        // (the EDGES are recorded even where the step body is not).
        bool reached2 = false;
        for (const TaintReach &r : f.reached)
            if (r.step == 2)
                reached2 = true;
        check("gap: the front continues past the gap", reached2,
              "the edges through the gap are recorded; only the step body is "
              "missing");
    }

    // === a BOUNDED walk frays its rim =======================================
    {
        Recording rec = mk_rec(std::string(kHdr) + kRegions +
                               mem_write_step(0, 0, 4194304 + 64) +
                               mem_write_step(1, 4, 4194304 + 128) +
                               mem_write_step(2, 8, 4194304 + 192) +
                               edge(0, 1) + edge(1, 2) + kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);

        TaintFront full = build_taint_front(s.df, p, 0, 64);
        check("bounded: the unbounded walk reaches all three",
              full.reached.size() == 3 && !full.bounded,
              "got " + std::to_string(full.reached.size()));

        TaintFront cut = build_taint_front(s.df, p, 0, /*max_depth=*/1);
        check("bounded: the capped walk stops short", cut.reached.size() == 2,
              "got " + std::to_string(cut.reached.size()));
        check("bounded: and SAYS its rim is a lower bound", cut.bounded,
              "a rim the cap produced is not a boundary the graph has");
        check("bounded: the note says so in words",
              std::string(TaintFront::bounded_note()).find("lower bound") !=
                  std::string::npos,
              TaintFront::bounded_note());
    }

    // === refusals ===========================================================
    {
        Recording rec = mk_rec(std::string(kHdr) + kRegions + kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        TaintFront f = build_taint_front(s.df, p, 0, 64);
        check("no-df: refuses", !f.enabled, "");
        check("no-df: with a stated reason", !f.disabled_reason.empty(), "");

        Recording rec2 = mk_rec(std::string(kHdr) + kRegions +
                                mem_write_step(0, 0, 4194304 + 64) + kEnd);
        Streams s2 = decode_streams(rec2);
        TaintFront f2 = build_taint_front(s2.df, p, 99, 64);
        check("bad-origin: refuses", !f2.enabled, "");
        check("bad-origin: with a stated reason", !f2.disabled_reason.empty(),
              "");
        check("bad-origin: no geometry", f2.reached.empty(), "");
    }

    // === a truncated recording carries the lower-bound fact ==================
    {
        Recording rec = mk_rec(std::string(kHdr) + kRegions +
                               mem_write_step(0, 0, 4194304 + 64) +
                               "{\"k\":\"end\",\"events\":3,\"truncated\":true,"
                               "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        TaintFront f = build_taint_front(s.df, p, 0, 64, rec.truncated());
        check("truncated: the front states the lower bound", f.truncated,
              "under truncation the front's extent is a lower bound");
    }

    if (failures) {
        std::fprintf(stderr, "test_taint: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "test_taint: all checks passed\n");
    return 0;
}
