// test_stepplace.cpp — space/stepplace.h (57-causal-layers.md T1): the ONE
// step->place resolver the four causal layers share, and its countable misses.
// Null harness, no display: this binary links stepplace.o + locate.o +
// projection.o + the doc model and NOTHING else — the same engine-free closure
// test_locate.cpp makes for the resolver this one adapts.
#include <cstdio>
#include <sstream>
#include <string>

#include "doc/recording.h"
#include "doc/streams.h"
#include "space/projection.h"
#include "space/stepplace.h"
#include "space/terrain.h" // regions_from_codeimage (the fixtures' region builder)

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

static const char *kHdrExact =
    "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\",\"exact\":"
    "true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n";

int main() {
    // === (a) an rbase-carrying step resolves through the WIRE base ===========
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"df_step\",\"step\":0,\"off\":16,\"rbase\":4194304,"
            "\"ops\":[]}\n"
            "{\"k\":\"df_step\",\"step\":1,\"off\":32,\"rbase\":4194304,"
            "\"ops\":[]}\n"
            "{\"k\":\"end\",\"events\":3,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        check("wire-rbase: setup decoded two steps", s.df.insn_off.size() == 2,
              "fixture must decode two df_step entries");

        StepPlacer placer(p, s.df);
        StepPlace sp = placer.at(0);
        check("wire-rbase: places", sp.placed, sp.why);
        check("wire-rbase: addr is rbase+off", sp.addr == 4194304 + 16,
              "the wire base must win over any derived anchor");
        check("wire-rbase: region is the code span",
              sp.region != nullptr && sp.region->base == 4194304,
              "a placed step must name the region that holds its address");
        check("wire-rbase: u/v are inside the unit plane",
              sp.u >= 0.0f && sp.u <= 1.0f && sp.v >= 0.0f && sp.v <= 1.0f,
              "plane coordinates must be normalised");
        check("wire-rbase: why is empty when placed", sp.why.empty(),
              "a placed step states no failure");
        check("wire-rbase: nothing unplaced yet", placer.unplaced() == 0,
              "a fully-resolvable pass must report zero misses");
        check("wire-rbase: note is empty when nothing missed",
              placer.note().empty(),
              "a clean weave must render no off-plane chip");

        // The cell agrees with place_address on the SAME address — the
        // resolver never invents its own plane arithmetic.
        StepPlace direct = place_address(p, 4194304 + 16);
        check("wire-rbase: cell matches place_address on the same address",
              direct.placed && direct.cell == sp.cell,
              "the step route and the address route must agree");
    }

    // === (b) no rbase + a single codeimage span resolves through the ANCHOR ==
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"df_step\",\"step\":0,\"off\":16,\"ops\":[]}\n"
            "{\"k\":\"end\",\"events\":2,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        check("derived-anchor: setup has no wire rbase", !s.df.rbase_present,
              "fixture must omit rbase");

        StepPlacer placer(p, s.df);
        StepPlace sp = placer.at(0);
        check("derived-anchor: places", sp.placed, sp.why);
        check("derived-anchor: addr is base+off", sp.addr == 4194304 + 16,
              "the single-span anchor must place base+off");
        check("derived-anchor: nothing unplaced", placer.unplaced() == 0, "");
    }

    // === (c) no rbase + TWO codeimage spans is unplaced, with the anchor's ===
    // === OWN refusal reason (never a re-derived one) ========================
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"codeimage\",\"base\":8388608,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"df_step\",\"step\":0,\"off\":16,\"ops\":[]}\n"
            "{\"k\":\"end\",\"events\":3,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        const Anchor anchor = resolve_anchor(regions_from_codeimage(rec));
        check("two-span: setup: the anchor itself refuses", !anchor.ok,
              "two code spans must make resolve_anchor refuse");

        StepPlacer placer(p, s.df);
        StepPlace sp = placer.at(0);
        check("two-span: unplaced", !sp.placed,
              "a bare offset with two codeimage spans must refuse, not guess");
        check("two-span: why is the anchor's OWN reason", sp.why == anchor.reason,
              "got: <" + sp.why + ">");
        // THE bug this brief names as most likely: an unplaced step must NOT
        // come back as cell 0. Cell 0 is a REAL cell.
        check("two-span: NO cell for an unplaced step", sp.cell == 0 &&
                  sp.region == nullptr && sp.addr == 0 && sp.u == 0.0f &&
                  sp.v == 0.0f,
              "an unplaced StepPlace must leave every place field at its "
              "default, and a caller must never read cell");
        check("two-span: counted once", placer.unplaced() == 1,
              "one unplaceable step is one miss");
        // Asking twice does not inflate the count: the chip states DISTINCT
        // steps, and a layer that re-queries a step has not lost a second one.
        placer.at(0);
        placer.at(0);
        check("two-span: re-querying the same step does not double-count",
              placer.unplaced() == 1,
              "unplaced() counts distinct steps, never calls");
        check("two-span: note uses the family's off-plane wording",
              placer.note() == "1 step(s) off-plane", placer.note());
    }

    // === (d) an offset PAST the span's length is unplaced, with the clamp ====
    // === reason (the SERVE_CI_MAX_BYTES case Anchor::place already names) ====
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":64,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"df_step\",\"step\":0,\"off\":16,\"ops\":[]}\n"
            "{\"k\":\"df_step\",\"step\":1,\"off\":4096,\"ops\":[]}\n"
            "{\"k\":\"end\",\"events\":3,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);

        StepPlacer placer(p, s.df);
        StepPlace ok = placer.at(0);
        check("clamp: the in-span step still places", ok.placed, ok.why);
        StepPlace past = placer.at(1);
        check("clamp: the past-the-span step is unplaced", !past.placed,
              "off >= len must refuse (Anchor::place's own clamp)");
        check("clamp: why names the span clamp",
              past.why.find("past the anchored span") != std::string::npos,
              "got: <" + past.why + ">");
        check("clamp: NO cell for the clamped step", past.cell == 0 &&
                                                        past.region == nullptr,
              "an unplaced step must never come back as cell 0");
        check("clamp: counted exactly once", placer.unplaced() == 1,
              "one clamped step is one miss");
    }

    // === (e) an out-of-range step index is unplaced and counted =============
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"df_step\",\"step\":0,\"off\":16,\"rbase\":4194304,"
            "\"ops\":[]}\n"
            "{\"k\":\"end\",\"events\":2,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        Streams s = decode_streams(rec);
        StepPlacer placer(p, s.df);
        StepPlace oor = placer.at(7);
        check("out-of-range: unplaced", !oor.placed,
              "step 7 is past this pass's single step");
        check("out-of-range: why is non-empty", !oor.why.empty(),
              "a refusal must always carry a reason");
        check("out-of-range: counted", placer.unplaced() == 1,
              "an out-of-range query is still a step this layer could not "
              "draw");
    }

    // === place_address: an unmapped address refuses, never cell 0 ===========
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"end\",\"events\":1,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        StepPlace miss = place_address(p, 0xdeadbeefULL);
        check("place_address: an unmapped address refuses", !miss.placed,
              "no region maps 0xdeadbeef in this recording");
        check("place_address: refusal carries a reason", !miss.why.empty(), "");
        check("place_address: NO cell on refusal",
              miss.cell == 0 && miss.region == nullptr && miss.addr == 0,
              "an unmapped address must never come back as cell 0");
        StepPlace hit = place_address(p, 4194304 + 8);
        check("place_address: a mapped address places", hit.placed, hit.why);
        check("place_address: names its region",
              hit.region != nullptr && hit.region->base == 4194304, "");
    }

    if (failures) {
        std::fprintf(stderr, "test_stepplace: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "test_stepplace: all checks passed\n");
    return 0;
}
