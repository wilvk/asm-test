// test_ridge.cpp — the dominant-path ridge (57-causal-layers.md T5). Null
// harness: space/ridge.o + stepplace.o + locate.o + projection.o + the doc
// model, and nothing else.
//
// The load-bearing checks are the ones the analysis note
// 2026-07-17-blockstep-reconstruction-defects.md says this tree has already
// got wrong TWICE with the correct rule written in front of it: the ridge must
// never derive a successor. A block whose successor was never recorded caps;
// it does not wrap to offset 0 and it does not join the next block by address
// adjacency. And tie-dimming is mandatory — a 51/49 fork and a 99/1 fork must
// not look alike.
#include <cstdio>
#include <sstream>
#include <string>

#include "doc/recording.h"
#include "doc/streams.h"
#include "space/projection.h"
#include "space/ridge.h"
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
static const char *kCodeimage =
    "{\"k\":\"codeimage\",\"base\":4194304,\"len\":4096,\"version\":0,"
    "\"when\":1,\"bytes\":\"90\"}\n";
static const char *kEnd =
    "{\"k\":\"end\",\"events\":64,\"truncated\":false,\"drops\":{\"lost\":0,"
    "\"throttled\":false}}\n";

static std::string insn(uint64_t addr) {
    return "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":" +
           std::to_string(addr) + "}\n";
}
// A `coverage` event MUST state its basis: the decoder runs note_basis over
// it exactly as it does over a `trace` event (doc/streams.cpp), and the schema
// forbids defaulting it — an omission sets TraceStream::basis_error and the
// ridge then refuses, which is correct but is not what these fixtures test.
static std::string coverage(const std::vector<uint64_t> &blocks) {
    std::string s = "{\"k\":\"coverage\",\"basis\":\"abs\",\"blocks\":[";
    for (size_t i = 0; i < blocks.size(); i++) {
        if (i)
            s += ",";
        s += std::to_string(blocks[i]);
    }
    return s + "],\"truncated\":false}\n";
}

static const RidgeSegment *seg(const PathRidge &r, uint64_t from, uint64_t to) {
    for (const RidgeSegment &s : r.segments)
        if (s.from_addr == from && s.to_addr == to)
            return &s;
    return nullptr;
}

int main() {
    const uint64_t A = 4194304, B = 4194304 + 64, C = 4194304 + 128;

    // === a loop body with a 90/10 exit: one bright segment, one dim ==========
    {
        // The loop head A jumps to B nine times and to C once. Blocks are
        // named by a coverage event; the instruction stream is what states
        // which transitions were actually observed.
        std::string ev = std::string(kHdr) + kCodeimage + coverage({A, B, C});
        for (int i = 0; i < 9; i++) {
            ev += insn(A);
            ev += insn(A + 4); // a non-block-start instruction inside A
            ev += insn(B);
            ev += insn(B + 4);
        }
        ev += insn(A);
        ev += insn(A + 4);
        ev += insn(C);
        ev += kEnd;
        Recording rec = mk_rec(ev);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        check("90/10: setup: blocks decoded", s.trace.blocks.size() == 3,
              "got " + std::to_string(s.trace.blocks.size()));

        PathRidge r = build_path_ridge(s.trace, p);
        check("90/10: enabled", r.enabled, r.disabled_reason);

        const RidgeSegment *ab = seg(r, A, B);
        const RidgeSegment *ac = seg(r, A, C);
        const RidgeSegment *ba = seg(r, B, A);
        check("90/10: A->B observed", ab != nullptr, "");
        check("90/10: A->C observed", ac != nullptr, "");
        check("90/10: B->A observed (the loop back-edge)", ba != nullptr, "");
        if (ab && ac) {
            check("90/10: A->B counted 9", ab->count == 9,
                  "got " + std::to_string(ab->count));
            check("90/10: A->C counted 1", ac->count == 1,
                  "got " + std::to_string(ac->count));
            check("90/10: A->B fraction is 0.9",
                  ab->fraction > 0.899 && ab->fraction < 0.901,
                  "got " + std::to_string(ab->fraction));
            check("90/10: A->C fraction is 0.1",
                  ac->fraction > 0.099 && ac->fraction < 0.101,
                  "got " + std::to_string(ac->fraction));
            check("90/10: A->B is the modal successor", ab->modal, "");
            check("90/10: A->C is NOT modal", !ac->modal, "");
            check("90/10: the modal segment is BRIGHTER than the minority one",
                  ridge_brightness(ab->fraction) >
                      ridge_brightness(ac->fraction),
                  "tie-dimming is mandatory, not cosmetic");
            check("90/10: the minority segment is dim (below the split)",
                  ridge_brightness(ac->fraction) < kRidgeSplitBrightness,
                  "got " + std::to_string(ridge_brightness(ac->fraction)));
            check("90/10: the modal segment is bright (above the split)",
                  ridge_brightness(ab->fraction) > kRidgeSplitBrightness, "");
            check("90/10: height is log-scaled by count",
                  ab->height > ac->height, "");
        }
        check("90/10: A is a fork with two successors",
              r.forks.size() == 1 && r.forks[0].successors == 2,
              "got " + std::to_string(r.forks.size()) + " fork(s)");
        if (r.forks.size() == 1) {
            check("90/10: the fork's modal fraction is 0.9",
                  r.forks[0].modal_fraction > 0.899 &&
                      r.forks[0].modal_fraction < 0.901,
                  "got " + std::to_string(r.forks[0].modal_fraction));
            check("90/10: the leaving mass is 0.1 (the glyph's size)",
                  r.forks[0].leaving_mass > 0.099 &&
                      r.forks[0].leaving_mass < 0.101,
                  "got " + std::to_string(r.forks[0].leaving_mass));
        }
        // C was ENTERED and never observed to leave: the ridge CAPS there.
        check("90/10: exactly one cap (C, entered and never left)",
              r.caps.size() == 1, "got " + std::to_string(r.caps.size()));
        if (r.caps.size() == 1)
            check("90/10: the cap is at C, not wrapped to the routine start",
                  r.caps[0].addr == C,
                  "got 0x" + std::to_string(r.caps[0].addr) +
                      " — a capped block must never wrap to offset 0 nor join "
                      "the next block by address adjacency");
    }

    // === a 50/50 fork renders BOTH at the split threshold ====================
    {
        std::string ev = std::string(kHdr) + kCodeimage + coverage({A, B, C});
        for (int i = 0; i < 2; i++) {
            ev += insn(A);
            ev += insn(B);
            ev += insn(A);
            ev += insn(C);
        }
        ev += kEnd;
        Recording rec = mk_rec(ev);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        PathRidge r = build_path_ridge(s.trace, p);
        const RidgeSegment *ab = seg(r, A, B);
        const RidgeSegment *ac = seg(r, A, C);
        check("50/50: both successors observed", ab && ac, "");
        if (ab && ac) {
            check("50/50: both fractions are 0.5",
                  ab->fraction > 0.499 && ab->fraction < 0.501 &&
                      ac->fraction > 0.499 && ac->fraction < 0.501,
                  "");
            check("50/50: BOTH render at exactly the split threshold",
                  ridge_brightness(ab->fraction) == kRidgeSplitBrightness &&
                      ridge_brightness(ac->fraction) == kRidgeSplitBrightness,
                  "a coin toss must be visibly split, and the threshold has "
                  "to be a named constant a test can pin");
        }
        check("brightness is monotonic in the fraction",
              ridge_brightness(0.0) < ridge_brightness(0.5) &&
                  ridge_brightness(0.5) < ridge_brightness(1.0),
              "");
        check("a 99/1 fork and a 51/49 fork do NOT look alike",
              ridge_brightness(0.99) - ridge_brightness(0.51) > 0.2f,
              "that difference is the whole distinction between a backbone "
              "and a coin toss");
    }

    // === one recorded successor vs NONE: visibly different geometry ==========
    {
        // A -> B (recorded, once). B is entered and never left.
        std::string ev = std::string(kHdr) + kCodeimage + coverage({A, B}) +
                         insn(A) + insn(B) + kEnd;
        Recording rec = mk_rec(ev);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        PathRidge r = build_path_ridge(s.trace, p);
        check("cap: A has a recorded successor -> a segment",
              r.segments.size() == 1 && r.segments[0].from_addr == A, "");
        check("cap: B has NONE -> a cap, and no segment leaving it",
              r.caps.size() == 1 && r.caps[0].addr == B,
              "got " + std::to_string(r.caps.size()) + " cap(s)");
        check("cap: A is NOT capped (it was observed to leave)",
              [&] {
                  for (const RidgeCap &c : r.caps)
                      if (c.addr == A)
                          return false;
                  return true;
              }(),
              "");
        check("cap: A with one successor is NOT a fork", r.forks.empty(),
              "a block with a single observed successor is a backbone, not a "
              "fork");
        check("the cap note names the unknown continuation",
              std::string(PathRidge::cap_note()).find("never recorded") !=
                  std::string::npos,
              PathRidge::cap_note());
    }

    // === a self-loop is a real observed transition, not a dropped one ========
    {
        std::string ev = std::string(kHdr) + kCodeimage + coverage({A}) +
                         insn(A) + insn(A) + insn(A) + kEnd;
        Recording rec = mk_rec(ev);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        PathRidge r = build_path_ridge(s.trace, p);
        const RidgeSegment *aa = seg(r, A, A);
        check("self-loop: recorded", aa != nullptr,
              "a block re-entered from itself is one observed transition, and "
              "dropping it would silently under-count the loop");
        if (aa) {
            check("self-loop: counted twice", aa->count == 2,
                  "got " + std::to_string(aa->count));
            check("self-loop: flagged", aa->self_loop, "");
        }
    }

    // === the LABEL says aggregate ===========================================
    {
        check("the label contains \"aggregate\"",
              std::string(PathRidge::label()).find("aggregate") !=
                  std::string::npos,
              PathRidge::label());
        check("the note denies the single-run reading",
              std::string(PathRidge::aggregate_note()).find("NOT a claim") !=
                  std::string::npos,
              PathRidge::aggregate_note());
    }

    // === instructions before the first recorded block are COUNTED ===========
    {
        // Two instructions arrive before any block start is seen.
        std::string ev = std::string(kHdr) + kCodeimage + coverage({B, C}) +
                         insn(A) + insn(A + 4) + insn(B) + insn(C) + kEnd;
        Recording rec = mk_rec(ev);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        PathRidge r = build_path_ridge(s.trace, p);
        check("unattributed: counted, never attributed by containment",
              r.unattributed_insns == 2,
              "got " + std::to_string(r.unattributed_insns) +
                  " — \"the greatest recorded block start below this address\" "
                  "would fabricate membership");
        check("unattributed: the B->C transition is still observed",
              seg(r, B, C) != nullptr, "");
    }

    // === refusals ===========================================================
    {
        Recording rec = mk_rec(std::string(kHdr) + kCodeimage + kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        PathRidge r = build_path_ridge(s.trace, p);
        check("no-trace: refuses", !r.enabled, "");
        check("no-trace: with a stated reason", !r.disabled_reason.empty(), "");

        // Instructions but no coverage: block boundaries would have to be
        // GUESSED, which is the documented failure mode.
        Recording rec2 = mk_rec(std::string(kHdr) + kCodeimage + insn(A) +
                                insn(B) + kEnd);
        Streams s2 = decode_streams(rec2);
        PathRidge r2 = build_path_ridge(s2.trace, p);
        check("no-blocks: refuses", !r2.enabled, "");
        check("no-blocks: the reason names the static-guess hazard",
              r2.disabled_reason.find("static guess") != std::string::npos,
              r2.disabled_reason);
        check("no-blocks: no geometry", r2.segments.empty() && r2.forks.empty(),
              "");
    }

    // === truncation rides as a stated lower bound ===========================
    {
        std::string ev = std::string(kHdr) + kCodeimage + coverage({A, B}) +
                         insn(A) + insn(B) +
                         "{\"k\":\"end\",\"events\":4,\"truncated\":true,"
                         "\"drops\":{\"lost\":0,\"throttled\":false}}\n";
        Recording rec = mk_rec(ev);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        PathRidge r = build_path_ridge(s.trace, p, rec.truncated());
        check("truncated: the counts are a stated lower bound", r.truncated,
              "");
    }

    if (failures) {
        std::fprintf(stderr, "test_ridge: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "test_ridge: all checks passed\n");
    return 0;
}
