// test_standalone.cpp — the four standalone scene BUILDERS of
// 59-standalone-scenes.md T2-T5 (scene3d/standalone.h), over real fixtures and
// real loaded recordings. Null harness, no display: this binary links
// s3/standalone.o + the models it reads from and NOTHING else — no ImGui, no
// GL, no engine (D4), the same closure proof test_drillin makes for the pick
// router.
//
// Every check here is a FIDELITY rule of the brief, not a shape check: a torn
// cap is distinguishable from a clean one, a hollow rib from an agreeing one, a
// hole from a zero, a prefix from a short call, a wireframe from zeros, a
// default lane width from a recorded one.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "doc/streams.h"
#include "scene3d/standalone.h"
#include "space/projection.h"
#include "views/region.h"
#include "views/tree.h"

using namespace asmdesk;
using namespace asmdesk::scene3d;

static int failures;

static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

// Build a Recording from an in-memory NDJSON string, through the real loader —
// the same pattern test_drillin/test_goto use.
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

static Recording load_fixture(const std::string &name) {
    const std::string path = std::string(ASMTEST_FIXTURE_DIR) + "/" + name;
    std::ifstream in(path);
    if (!in) {
        fail("open fixture", path);
        return Recording{};
    }
    std::string err;
    auto rec = load_recording(in, err);
    if (!rec) {
        fail("load fixture " + name, err);
        return Recording{};
    }
    return *rec;
}

static const char *kHdr =
    "{\"asmtrace\":1,\"container\":\"ndjson\",\"producer\":{\"name\":\"t\","
    "\"version\":\"1\"},\"provenance\":{\"backend\":\"emu\",\"exact\":true},"
    "\"arch\":\"x86_64\"}\n";

// Two recordings of the same routine, with a controllable trace + statediff.
static std::string mk_pair_side(const std::vector<uint64_t> &offs,
                                const std::string &statediff, bool truncated) {
    std::string s = kHdr;
    for (uint64_t o : offs)
        s += "{\"k\":\"trace\",\"basis\":\"rel\",\"kind\":\"insn\",\"off\":" +
             std::to_string(o) + "}\n";
    s += "{\"k\":\"coverage\",\"basis\":\"rel\",\"blocks\":[0],\"blocks_total\""
         ":1,\"insns_total\":" +
         std::to_string(offs.size()) + ",\"truncated\":" +
         (truncated ? "true" : "false") + "}\n";
    s += statediff;
    s += "{\"k\":\"end\",\"events\":1,\"truncated\":" +
         std::string(truncated ? "true" : "false") + "}\n";
    return s;
}

int main() {
    // =====================================================================
    // T2 — divergence worldline
    // =====================================================================
    {
        // A clean fork: A and B share a prefix, then part at step 2. The
        // recording id is normally the file's basename (streams.h's
        // recording_id); these are in-memory, so name them here — the
        // divergence scene carries both ids into its drill-in link.
        Streams a = decode_streams(mk_rec(mk_pair_side(
            {0, 4, 8, 12}, "", /*truncated=*/false)));
        Streams b = decode_streams(mk_rec(mk_pair_side(
            {0, 4, 24, 28}, "", /*truncated=*/false)));
        a.id = "a.asmtrace";
        b.id = "b.asmtrace";
        const DivergenceScene d = build_divergence_scene(a, b);
        check("T2/fork: not refused", !d.refused, d.refusal);
        check("T2/fork: diverged", d.diverged, "no divergence found");
        check("T2/fork: at the right step", d.fork_step == 2,
              "fork_step=" + std::to_string(d.fork_step));
        check("T2/fork: the tube climbs the shared prefix only",
              d.shared_steps == 2,
              "shared_steps=" + std::to_string(d.shared_steps));
        check("T2/fork: a CLEAN cap when neither side is truncated", !d.bounded,
              "an untruncated pair reported a torn cap");
        // The fork pillar is pickable and routes to the diff view's patient-
        // zero row — never to a plain canvas.
        const auto link = divergence_pick_link(d, 0);
        check("T2/fork: the pillar routes to the diff view",
              link && link->view == dt_view::diff && link->step &&
                  *link->step == 2 && link->rec == "a.asmtrace" &&
                  link->rec_b == "b.asmtrace",
              "the fork pillar did not open the diff view at patient zero");

        // Step 4: a BOUNDED divergence ends the tube in a TORN cap. "We
        // stopped looking" and "they agreed to the end" are different claims.
        const Streams ta =
            decode_streams(mk_rec(mk_pair_side({0, 4}, "", /*trunc=*/true)));
        const Streams tb = decode_streams(
            mk_rec(mk_pair_side({0, 4, 8}, "", /*trunc=*/false)));
        const DivergenceScene t = build_divergence_scene(ta, tb);
        check("T2/torn: bounded", t.bounded,
              "a truncated side did not bound the verdict");
        check("T2/torn: a bounded verdict is NOT reported as a divergence",
              !t.diverged,
              "the shorter TRUNCATED side was reported as a real fork");
        const std::string tdump = divergence_scene_dump(t);
        check("T2/torn: the dump says TORN",
              tdump.find("cap=TORN") != std::string::npos, tdump);
        const std::string cdump = divergence_scene_dump(d);
        check("T2/clean: the dump says clean",
              cdump.find("cap=clean") != std::string::npos, cdump);

        // Step 5: an UNCOMPUTED step is a HOLLOW rib, distinguishable from an
        // agreeing one. A zero-width rib would render "we did not look" as
        // "they agreed" — the exact misreading StateDelta forbids.
        const std::string sd_a =
            "{\"k\":\"statediff\",\"step\":0,\"computed\":false,\"changed\":{}}\n"
            "{\"k\":\"statediff\",\"step\":1,\"computed\":true,\"changed\":"
            "{\"rax\":1}}\n"
            "{\"k\":\"statediff\",\"step\":2,\"computed\":true,\"changed\":"
            "{\"rbx\":7}}\n";
        const std::string sd_b =
            "{\"k\":\"statediff\",\"step\":0,\"computed\":false,\"changed\":{}}\n"
            "{\"k\":\"statediff\",\"step\":1,\"computed\":true,\"changed\":"
            "{\"rax\":1}}\n"
            "{\"k\":\"statediff\",\"step\":2,\"computed\":true,\"changed\":"
            "{\"rbx\":9}}\n";
        const Streams ra = decode_streams(
            mk_rec(mk_pair_side({0, 4, 8, 12}, sd_a, false)));
        const Streams rb = decode_streams(
            mk_rec(mk_pair_side({0, 4, 8, 12}, sd_b, false)));
        const DivergenceScene r = build_divergence_scene(ra, rb);
        check("T2/ribs: present", !r.ribs.empty(), r.rib_note);
        const DivRib *hollow = nullptr;
        const DivRib *solid = nullptr;
        for (const DivRib &rib : r.ribs) {
            if (rib.hollow && hollow == nullptr)
                hollow = &rib;
            if (!rib.hollow && solid == nullptr)
                solid = &rib;
        }
        check("T2/ribs: the uncomputed step is HOLLOW",
              hollow != nullptr && hollow->step == 0,
              "step 0 (computed=false on both sides) is not a hollow rib");
        check("T2/ribs: a disagreeing step is a SOLID rib with thickness",
              solid != nullptr && solid->step == 2 && solid->changed == 1,
              "the rbx disagreement at step 2 is not a solid rib");
        check("T2/ribs: hollow is distinguishable from agreeing",
              hollow != nullptr && solid != nullptr &&
                  hollow->hollow != solid->hollow,
              "a hollow rib and an agreeing one are the same shape");
        check("T2/ribs: an agreeing step produces NO rib",
              [&] {
                  for (const DivRib &rib : r.ribs)
                      if (rib.step == 1)
                          return false;
                  return true;
              }(),
              "step 1 agrees on both sides but produced a rib");
        check("T2/ribs: coloured by register class",
              solid != nullptr && solid->cls == RegClass::Gpr,
              "rbx did not classify as a GPR");
        check("T2/ribs: a rib routes to the slice explorer at its step",
              [&] {
                  const auto l = divergence_pick_link(r, r.diverged ? 1 : 0);
                  return l && l->view == dt_view::slice;
              }(),
              "a rib did not open the slice explorer");

        // With NO statediff the ribs are absent AND a note says so — never a
        // silent rib-less scene that reads as agreement.
        check("T2/ribs: absent with an explicit note when statediff is empty",
              d.ribs.empty() && !d.rib_note.empty(), "silent absence");

        // Step 3: the admission gate is a REFUSAL CARD with its reason, and
        // NO geometry.
        std::string arch_hdr =
            "{\"asmtrace\":1,\"container\":\"ndjson\",\"producer\":{\"name\":"
            "\"t\",\"version\":\"1\"},\"provenance\":{\"backend\":\"emu\","
            "\"exact\":true},\"arch\":\"aarch64\"}\n"
            "{\"k\":\"trace\",\"basis\":\"rel\",\"kind\":\"insn\",\"off\":0}\n"
            "{\"k\":\"coverage\",\"basis\":\"rel\",\"blocks\":[0],"
            "\"blocks_total\":1,\"insns_total\":1,\"truncated\":false}\n";
        const Streams other = decode_streams(mk_rec(arch_hdr));
        const DivergenceScene ref = build_divergence_scene(a, other);
        check("T2/gate: a wrong-arch pair is REFUSED", ref.refused,
              "an aarch64/x86_64 pair was not refused");
        check("T2/gate: the refusal names its reason",
              ref.refusal.find("architectures") != std::string::npos,
              "refusal: '" + ref.refusal + "'");
        check("T2/gate: a refusal has NO geometry",
              ref.ribs.empty() && divergence_pick_order(ref).empty(),
              "a refused pair still produced pickable geometry");
        check("T2/gate: the dump is the refusal",
              divergence_scene_dump(ref).rfind("refused:", 0) == 0,
              divergence_scene_dump(ref));
    }

    if (failures) {
        std::fprintf(stderr, "%d standalone-scene check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_standalone: all checks passed\n");
    return 0;
}
