// test_blameforest.cpp — the blame convergence forest (57-causal-layers.md
// T4). Null harness: space/blameforest.o + stepplace.o + locate.o +
// projection.o + the doc model, and nothing else.
//
// The load-bearing checks are the two arithmetic mistakes that would LOOK
// right: a `born_untraced` cone raising some other step's weight (a value with
// no traced producer cannot converge with anything), and two steps in one
// plane cell having their weights summed (that manufactures a convergence no
// single step has).
#include <cstdio>
#include <sstream>
#include <string>

#include "doc/recording.h"
#include "doc/streams.h"
#include "space/blameforest.h"
#include "space/projection.h"
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
    "{\"k\":\"end\",\"events\":16,\"truncated\":false,\"drops\":{\"lost\":0,"
    "\"throttled\":false}}\n";

// A df_step with a wire rbase, so every step places through the ONE route.
static std::string step(int i, uint64_t off) {
    return "{\"k\":\"df_step\",\"step\":" + std::to_string(i) +
           ",\"off\":" + std::to_string(off) +
           ",\"rbase\":4194304,\"ops\":[]}\n";
}

static uint32_t weight_of(const BlameForest &f, uint32_t step) {
    for (const BlameConvergence &c : f.producers)
        if (c.step == step)
            return c.weight;
    return 0;
}

int main() {
    // === two cones sharing one step: that step is weight 2, others 1 ========
    {
        // steps 0..4 at distinct offsets. Cone A = {0, 1, 3} (sink 3),
        // cone B = {0, 2, 4} (sink 4). Step 0 is the shared root cause.
        Recording rec = mk_rec(
            std::string(kHdr) + kCodeimage + step(0, 0) + step(1, 16) +
            step(2, 32) + step(3, 48) + step(4, 64) +
            "{\"k\":\"blame\",\"step\":3,\"off\":48,\"cone\":[{\"step\":0,\"off\":0,\"kind\":\"insn\"},{\"step\":1,\"off\":16,\"kind\":\"insn\"},{\"step\":3,\"off\":48,\"kind\":\"insn\"}]}\n"
            "{\"k\":\"blame\",\"step\":4,\"off\":64,\"cone\":[{\"step\":0,\"off\":0,\"kind\":\"insn\"},{\"step\":2,\"off\":32,\"kind\":\"insn\"},{\"step\":4,\"off\":64,\"kind\":\"insn\"}]}\n" +
            kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        check("share: setup: two cones decoded", s.blame.size() == 2,
              "got " + std::to_string(s.blame.size()));

        BlameForest f = build_blame_forest(s.blame, s.df, p);
        check("share: enabled", f.enabled, f.disabled_reason);
        check("share: the shared step has weight 2", weight_of(f, 0) == 2,
              "got " + std::to_string(weight_of(f, 0)));
        for (uint32_t st : {1u, 2u, 3u, 4u})
            check("share: step " + std::to_string(st) + " has weight 1",
                  weight_of(f, st) == 1,
                  "got " + std::to_string(weight_of(f, st)));
        check("share: max_weight is 2", f.max_weight == 2,
              "got " + std::to_string(f.max_weight));
        check("share: both sinks are marked", f.sinks.size() == 2, "");
        check("share: the sink steps are flagged as such",
              [&] {
                  for (const BlameConvergence &c : f.producers)
                      if (c.step == 3 && !c.is_sink)
                          return false;
                  return true;
              }(),
              "a step that is some cone's own sink must say so");
        check("share: this is NOT a single-cone recording", !f.single_cone, "");
        check("share: nothing off-plane", f.off_plane == 0 && f.off_plane_note.empty(),
              f.off_plane_note);
        check("the label states the claim is a SET OVERLAP",
              std::string(BlameForest::label()).find("SET OVERLAP") !=
                  std::string::npos,
              BlameForest::label());
        check("the label denies a link",
              std::string(BlameForest::label()).find("not a") !=
                  std::string::npos,
              BlameForest::label());
    }

    // === a repeated step WITHIN one cone still counts once for that cone ====
    {
        Recording rec = mk_rec(
            std::string(kHdr) + kCodeimage + step(0, 0) + step(1, 16) +
            "{\"k\":\"blame\",\"step\":1,\"off\":16,\"cone\":[{\"step\":0,\"off\":0,\"kind\":\"insn\"},{\"step\":0,\"off\":0,\"kind\":\"insn\"},{\"step\":0,\"off\":0,\"kind\":\"insn\"},{\"step\":1,\"off\":16,\"kind\":\"insn\"}]}\n" +
            kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        BlameForest f = build_blame_forest(s.blame, s.df, p);
        check("dedup: a step repeated in one cone has weight 1, not 3",
              weight_of(f, 0) == 1,
              "got " + std::to_string(weight_of(f, 0)) +
                  " — weight counts DISTINCT CONES, not cone entries");
    }

    // === born_untraced contributes ONLY its sink, and raises no weight ======
    {
        // Cone A is a real cone over {0, 1}. Cone B is born_untraced with a
        // cone the producer still wrote out as the sink alone — AND, to make
        // the check adversarial, a cone that names step 0 as well. Even then
        // it must not raise step 0's weight.
        Recording rec = mk_rec(
            std::string(kHdr) + kCodeimage + step(0, 0) + step(1, 16) +
            step(2, 32) +
            "{\"k\":\"blame\",\"step\":1,\"off\":16,\"cone\":[{\"step\":0,\"off\":0,\"kind\":\"insn\"},{\"step\":1,\"off\":16,\"kind\":\"insn\"}]}\n"
            "{\"k\":\"blame\",\"step\":2,\"off\":32,\"cone\":[{\"step\":0,\"off\":0,\"kind\":\"insn\"},{\"step\":2,\"off\":32,\"kind\":\"insn\"}],"
            "\"born_untraced\":true}\n" +
            kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        check("untraced: setup: the verdict decoded",
              s.blame.size() == 2 && s.blame[1].born_untraced,
              "the fixture must carry born_untraced");

        BlameForest f = build_blame_forest(s.blame, s.df, p);
        check("untraced: counted", f.born_untraced == 1,
              "got " + std::to_string(f.born_untraced));
        check("untraced: step 0 stays at weight 1",
              weight_of(f, 0) == 1,
              "got " + std::to_string(weight_of(f, 0)) +
                  " — a value with no traced producer converging with another "
                  "would be a pure artifact");
        check("untraced: step 2 raises NO producer weight at all",
              weight_of(f, 2) == 0,
              "got " + std::to_string(weight_of(f, 2)));
        check("untraced: max_weight stays 1 (no spike was manufactured)",
              f.max_weight == 1, "got " + std::to_string(f.max_weight));
        check("untraced: its sink is still marked",
              [&] {
                  for (const BlameSink &sk : f.sinks)
                      if (sk.step == 2 && sk.born_untraced)
                          return true;
                  return false;
              }(),
              "the verdict is a fact to state, not a row to drop");
        check("the born-untraced label states the verdict",
              std::string(BlameForest::born_untraced_label())
                      .find("no traced producer") != std::string::npos,
              BlameForest::born_untraced_label());
    }

    // === an unplaceable off is counted by T1's placer and emits no beacon ===
    {
        // TWO codeimage spans and NO wire rbase: the anchor refuses, so every
        // step is unplaceable — the placer's own refusal, counted.
        Recording rec = mk_rec(
            std::string(kHdr) + kCodeimage +
            "{\"k\":\"codeimage\",\"base\":8388608,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"ops\":[]}\n"
            "{\"k\":\"df_step\",\"step\":1,\"off\":16,\"ops\":[]}\n"
            "{\"k\":\"blame\",\"step\":1,\"off\":16,\"cone\":[{\"step\":0,\"off\":0,\"kind\":\"insn\"},{\"step\":1,\"off\":16,\"kind\":\"insn\"}]}\n" + kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        BlameForest f = build_blame_forest(s.blame, s.df, p);
        check("off-plane: no beacons", f.producers.empty(),
              "an unplaceable step must emit no beacon, ever");
        check("off-plane: counted by the placer", f.off_plane == 2,
              "got " + std::to_string(f.off_plane));
        check("off-plane: the note uses the family's wording",
              f.off_plane_note == "2 step(s) off-plane", f.off_plane_note);
        check("off-plane: the cone is still stated (the sink row survives)",
              f.sinks.size() == 1, "");
    }

    // === a single-cone recording produces no spike above the baseline =======
    {
        Recording rec = mk_rec(
            std::string(kHdr) + kCodeimage + step(0, 0) + step(1, 16) +
            "{\"k\":\"blame\",\"step\":1,\"off\":16,\"cone\":[{\"step\":0,\"off\":0,\"kind\":\"insn\"},{\"step\":1,\"off\":16,\"kind\":\"insn\"}]}\n" + kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        BlameForest f = build_blame_forest(s.blame, s.df, p);
        check("single: says so", f.single_cone,
              "with one cone the layer must LOOK like a faint bundle, not a "
              "finding — and that has to be a fact of the model");
        check("single: every weight is the baseline 1", f.max_weight == 1,
              "got " + std::to_string(f.max_weight));
    }

    // === no blame at all refuses, with a reason =============================
    {
        Recording rec =
            mk_rec(std::string(kHdr) + kCodeimage + step(0, 0) + kEnd);
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        BlameForest f = build_blame_forest(s.blame, s.df, p);
        check("no-blame: refuses", !f.enabled, "");
        check("no-blame: with a stated reason", !f.disabled_reason.empty(), "");
        check("no-blame: no geometry", f.producers.empty() && f.sinks.empty(),
              "");
    }

    // === truncation rides as a stated lower bound ===========================
    {
        Recording rec = mk_rec(
            std::string(kHdr) + kCodeimage + step(0, 0) + step(1, 16) +
            "{\"k\":\"blame\",\"step\":1,\"off\":16,\"cone\":[{\"step\":0,\"off\":0,\"kind\":\"insn\"},{\"step\":1,\"off\":16,\"kind\":\"insn\"}]}\n"
            "{\"k\":\"end\",\"events\":4,\"truncated\":true,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        BlameForest f = build_blame_forest(s.blame, s.df, p, rec.truncated());
        check("truncated: the forest states the lower bound", f.truncated,
              "a missing convergence under truncation is NOT SEEN, not "
              "absent");
    }

    if (failures) {
        std::fprintf(stderr, "test_blameforest: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "test_blameforest: all checks passed\n");
    return 0;
}
