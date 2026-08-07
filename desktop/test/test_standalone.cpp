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

    // =====================================================================
    // T3 — invocation stack
    // =====================================================================
    {
        // obs-region.asmtrace: three invocations of a routine with a
        // conditional branch — #1 {0,24}, #2 {0,10,24} (truncated), #3 open.
        const Recording rec = load_fixture("obs-region.asmtrace");
        const RegionView rv = obs_region_build(rec);
        const InvocationScene s = build_invocation_scene(rv);
        check("T3: three slabs", s.slabs.size() == 3,
              "slabs=" + std::to_string(s.slabs.size()));
        check("T3: the union block set spans every invocation",
              s.blocks.size() == 3 && s.blocks[0] == 0 && s.blocks[1] == 10 &&
                  s.blocks[2] == 24,
              invocation_scene_dump(s));
        // The finding: a block present in some slabs and absent in others.
        check("T3/holes: the conditional block is a HOLE in slab #1",
              s.slabs[0].cells.size() == 3 &&
                  s.slabs[0].cells[1].block == 10 &&
                  s.slabs[0].cells[1].state == InvCellState::Absent,
              "block 0x10 in invocation #1 is not a hole");
        check("T3/holes: and PRESENT in slab #2",
              s.slabs[1].cells[1].state == InvCellState::Counted &&
                  s.slabs[1].cells[1].count == 1,
              "block 0x10 in invocation #2 is not a counted cell");
        check("T3/holes: a hole is never a zero-count cell",
              s.slabs[0].cells[1].count == 0 &&
                  s.slabs[0].cells[1].state != InvCellState::Counted,
              "a hole was encoded as a counted zero");
        check("T3/holes: counted", s.holes >= 1,
              "holes=" + std::to_string(s.holes));
        // An open invocation is FRAYED and labelled a PREFIX.
        check("T3/prefix: the third invocation is open", !s.slabs[2].closed,
              "the unterminated invocation was reported closed");
        check("T3/prefix: counted", s.open_slabs == 1,
              "open_slabs=" + std::to_string(s.open_slabs));
        check("T3/prefix: the dump labels it a prefix",
              invocation_scene_dump(s).find("FRAYED(prefix)") !=
                  std::string::npos,
              invocation_scene_dump(s));
        check("T3/truncated: invocation #2's own truncation survives",
              s.slabs[1].truncated,
              "the truncated coverage footer did not reach the slab");
        // The drill-in uses dt_link::invocation and NEVER dt_link::step.
        const std::vector<InvElem> order = invocation_pick_order(s);
        check("T3/pick: a hole is not pickable",
              [&] {
                  for (const InvElem &e : order)
                      if (s.slabs[e.slab].cells[e.cell].state ==
                          InvCellState::Absent)
                          return false;
                  return true;
              }(),
              "a hole produced a pickable element");
        check("T3/drill-in: routes to the region view", !order.empty() && [&] {
            const auto l = invocation_pick_link(s, "r.asmtrace", 0);
            return l && l->view == dt_view::region;
        }(), "the slab cell did not open the region view");
        check("T3/drill-in: carries dt_link::invocation", [&] {
            const auto l = invocation_pick_link(s, "r.asmtrace", 0);
            return l && l->invocation && *l->invocation == 1;
        }(), "the invocation number did not round-trip");
        check("T3/drill-in: leaves dt_link::step UNTOUCHED", [&] {
            const auto l = invocation_pick_link(s, "r.asmtrace", 0);
            return l && !l->step.has_value();
        }(), "the invocation number was written into `step` — the dataflow "
             "step index — which is exactly the ordinal conflation 46 forbids");
        check("T3/drill-in: refuses an index past the geometry",
              !invocation_pick_link(s, "r.asmtrace", order.size()),
              "an out-of-range element resolved to a link");
        // The legend keeps it distinct from a loop helix.
        check("T3/legend: names the loop-helix distinction",
              std::string(InvocationScene::legend()).find("loop helix") !=
                  std::string::npos,
              InvocationScene::legend());
        // Placement: with a projection the slab's X/Z stays the Hilbert plane.
        {
            std::vector<space::Region> regs;
            regs.push_back(space::Region{4198710, 64, space::Region::Code,
                                         "code", 0});
            const space::Projection proj =
                space::build_projection(std::move(regs));
            const InvocationScene ps = build_invocation_scene(rv, &proj);
            check("T3/place: unplaced without a projection",
                  !s.slabs[0].cells[0].placed,
                  "a cell claimed a plane position with no projection");
            // The fixture's basis is "rel", so a raw block offset is not an
            // address in this projection — the cell must report UNPLACED
            // rather than inventing one.
            check("T3/place: an unplaceable block is marked, not invented",
                  ps.slabs[0].cells.size() == 3,
                  "the projection changed the cell set");
        }
        // The codeimage tint: an untinted recording says "unstated" rather
        // than claiming version 0.
        check("T3/codeimage: unstated when the recording has no timeline",
              !s.slabs[0].codeimage_known,
              "a recording with no codeimage events claimed a version");
        {
            const Recording ci = load_fixture("obs-codeimage.asmtrace");
            const InvocationScene cs =
                build_invocation_scene(obs_region_build(ci));
            for (const InvSlab &sl : cs.slabs)
                check("T3/codeimage: known when the recording carries one",
                      sl.codeimage_known,
                      "a codeimage-carrying recording left a slab unstated");
        }
        // An empty region capture states why rather than showing a void.
        {
            const InvocationScene e = build_invocation_scene(RegionView{});
            check("T3/empty: states its reason", !e.note.empty(),
                  "an empty invocation scene is silent");
        }
    }

    // =====================================================================
    // T4 — module excursion ribbon
    // =====================================================================
    {
        // obs-tree.asmtrace: two tids, a spy_victim -> jit module transition
        // on tid 4243, and an engine-side depth cap of 3.
        const Recording rec = load_fixture("obs-tree.asmtrace");
        const TreeView tv = obs_tree_build(rec);
        const ModuleRibbonScene s = build_module_ribbon(tv);
        check("T4: two lanes, one per tid", s.lanes.size() == 2,
              "lanes=" + std::to_string(s.lanes.size()));
        check("T4: lanes are ascending by tid",
              s.lanes[0].tid == 4242 && s.lanes[1].tid == 4243,
              "lane order is not tid-ascending");
        check("T4: the lanes have independent depth profiles",
              s.lanes[0].max_depth == 2 && s.lanes[1].max_depth == 1,
              "lane depths: " + std::to_string(s.lanes[0].max_depth) + " / " +
                  std::to_string(s.lanes[1].max_depth));
        check("T4: X is the recording's own call order (Event::seq)",
              s.lanes[0].segs.size() == 3 && s.lanes[0].segs[0].seq <
                                                 s.lanes[0].segs[1].seq,
              "segments are not ordered by stream position");
        // The seam is the finding.
        check("T4/seam: the module transition produces a seam at the right "
              "call index",
              s.seams == 1 && s.lanes[1].segs.size() == 2 &&
                  s.lanes[1].segs[1].boundary && !s.lanes[1].segs[0].boundary,
              "seams=" + std::to_string(s.seams) + "\n" +
                  module_ribbon_dump(s));
        // A depth-capped capture marks the lane floor CAPPED.
        check("T4/cap: the capture is depth-capped", s.depth_capped,
              "the engine-side depth filter did not reach the scene");
        check("T4/cap: the note says clipped, not bottom",
              s.cap_note.find("CLIPPED") != std::string::npos,
              s.cap_note);
        check("T4/cap: the floor segment is marked",
              s.lanes[0].segs[2].depth == 2 && s.lanes[0].segs[2].at_cap,
              "the depth-2 call under a cap of 3 is not marked capped");
        check("T4/cap: a shallower call is NOT marked",
              !s.lanes[0].segs[0].at_cap,
              "a depth-0 call was marked as being at the cap");
        check("T4/cap: the cap survives the drill-in", [&] {
            // lane 0, segment 2 — the third element in lane-then-segment order
            const RibbonDrill d = ribbon_pick_link(s, "r.asmtrace", 2);
            return d.ok && d.at_cap && d.tid == 4242;
        }(), "the capped floor did not survive the drill-in");
        check("T4/drill-in: opens the tree, carrying tid + module + symbol",
              [&] {
                  const RibbonDrill d = ribbon_pick_link(s, "r.asmtrace", 0);
                  return d.ok && d.link.view == dt_view::tree &&
                         d.tid == 4242 && d.module == "spy_victim" &&
                         d.symbol == "work" && d.link.off &&
                         *d.link.off == 4198710;
              }(),
              "the segment drill-in lost its tid/module/symbol");
        check("T4/legend: states that returns are inferred from depth",
              std::string(ModuleRibbonScene::legend()).find("depth") !=
                  std::string::npos,
              ModuleRibbonScene::legend());

        // An unresolved module gets its own hue rather than being dropped or
        // merged into a resolved one.
        {
            std::string nd = kHdr;
            nd += "{\"k\":\"call\",\"tid\":1,\"depth\":0,\"addr\":16,\"name\":"
                  "\"a\",\"module\":\"libc\"}\n";
            nd += "{\"k\":\"call\",\"tid\":1,\"depth\":1,\"addr\":32,\"name\":"
                  "\"b\",\"module\":\"?\"}\n";
            nd += "{\"k\":\"call\",\"tid\":2,\"depth\":0,\"addr\":48,\"name\":"
                  "\"c\",\"module\":\"libc\"}\n";
            const ModuleRibbonScene u =
                build_module_ribbon(obs_tree_build(mk_rec(nd)));
            check("T4/unknown: an unresolved module is kept",
                  u.lanes.size() == 2 && u.lanes[0].segs.size() == 2,
                  "the unresolved call was dropped");
            check("T4/unknown: labelled, never blank",
                  u.lanes[0].segs[1].module_unknown &&
                      u.lanes[0].segs[1].module ==
                          std::string(ribbon_unknown_module()),
                  "the unresolved module is blank or borrowed a real name");
            check("T4/unknown: counted", u.unknown_modules == 1,
                  "unknown_modules=" + std::to_string(u.unknown_modules));
            check("T4/unknown: it is a seam like any other module change",
                  u.lanes[0].segs[1].boundary,
                  "the resolved->unresolved transition is not a seam");
            check("T4/unknown: the drill-in does NOT filter by the word "
                  "\"unknown\"",
                  [&] {
                      const RibbonDrill d = ribbon_pick_link(u, "r", 1);
                      return d.ok && d.module.empty();
                  }(),
                  "the tree would be filtered by a module literally named "
                  "\"unknown\"");
        }

        // A single-thread recording degrades to the 2D icicle form.
        {
            std::string nd = kHdr;
            nd += "{\"k\":\"call\",\"tid\":9,\"depth\":0,\"addr\":16,\"name\":"
                  "\"a\",\"module\":\"libc\"}\n";
            const ModuleRibbonScene one =
                build_module_ribbon(obs_tree_build(mk_rec(nd)));
            check("T4/2D: a single-tid recording asks for the 2D form",
                  one.single_thread, "one lane was left as a 3D scene");
            check("T4/2D: and says why", !one.note.empty(), "silent degrade");
            // The note must not describe a fallback nothing implements:
            // `single_thread` has no consumer outside this model's own dump and
            // this test, so nothing anywhere renders an icicle timeline. And the
            // one lane must stay SELECTABLE — 59 T4 requires it, so this note
            // may never migrate into kind_unavailable, which the selector
            // renders under BeginDisabled.
            // The defect is the CLAIM that a substitute view is being
            // displayed, not the word "icicle" — naming the better 2D form is
            // useful advice. Pin the claim: nothing may be "shown instead",
            // because nothing is.
            check("T4/2D: no phantom fallback",
                  one.note.find("instead") == std::string::npos &&
                      one.note.find("showing") == std::string::npos,
                  "the note claimed a substitute view was being shown instead, "
                  "but single_thread has no consumer and nothing renders one: "
                  "got \"" +
                      one.note + "\"");
            check("T4/2D: stays selectable", !one.lanes.empty(),
                  "a one-lane ribbon must remain available — disabling it "
                  "would contradict 59 T4");
        }
        // No call tree at all: a stated reason, not an empty scene.
        {
            const ModuleRibbonScene none = build_module_ribbon(TreeView{});
            check("T4/empty: states its reason", !none.note.empty(),
                  "an empty ribbon is silent");
        }
    }

    // =====================================================================
    // T5 — SIMD lane prism
    // =====================================================================
    {
        // A recorded shuffle: the same four bytes, permuted. Colour continuity
        // is what shows the permutation, so each byte must keep its hue.
        std::string nd = kHdr;
        nd += "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"disasm\":\"movdqa "
              "xmm0, xmmword ptr [rsp]\",\"ops\":[{\"space\":\"reg\",\"reg\":"
              "17,\"size\":16,\"write\":true,\"value_valid\":true,\"wide\":"
              "true,\"bytes\":\"000102030405060708090a0b0c0d0e0f\"}]}\n";
        nd += "{\"k\":\"df_step\",\"step\":1,\"off\":4,\"disasm\":\"pshufb "
              "xmm0, xmm1\",\"ops\":[{\"space\":\"reg\",\"reg\":17,\"size\":16,"
              "\"write\":true,\"value_valid\":true,\"wide\":true,\"bytes\":"
              "\"0f0e0d0c0b0a09080706050403020100\"}]}\n";
        nd += "{\"k\":\"end\",\"events\":2,\"truncated\":false}\n";
        const Streams st = decode_streams(mk_rec(nd));
        const std::vector<uint32_t> regs = lane_prism_registers(st.df);
        check("T5: the wide register is offered", regs.size() == 1 &&
                                                      regs[0] == 17,
              "lane_prism_registers did not find the wide writes");
        const LanePrismScene p = build_lane_prism(st.df, 17, "x86");
        check("T5: two stacked writes", p.writes.size() == 2,
              "writes=" + std::to_string(p.writes.size()));
        check("T5: Z is the stacked-write index, oldest first",
              p.writes[0].z == 0 && p.writes[1].z == 1 &&
                  p.writes[0].step == 0 && p.writes[1].step == 1,
              "the write stacking order is not ascending by step");
        // The permutation reveals itself through colour continuity.
        check("T5/shuffle: a recorded byte keeps its hue across a lane move",
              p.writes[0].bytes.size() == 16 && p.writes[1].bytes.size() == 16 &&
                  p.writes[0].bytes[0].hue == p.writes[1].bytes[15].hue &&
                  p.writes[0].bytes[3].hue == p.writes[1].bytes[12].hue,
              "byte 0x00 changed hue when the shuffle moved it to lane 15");
        check("T5/shuffle: different byte values get different hues",
              p.writes[0].bytes[0].hue != p.writes[0].bytes[1].hue,
              "two distinct byte values share a hue");
        check("T5/hue: the hash is stable across calls",
              prism_byte_hue(0x41) == prism_byte_hue(0x41),
              "prism_byte_hue is not a pure function");
        // Element width (2026-08-08 revised plan): movdqa is a BULK MOVE --
        // NoLaneSemantics, not a disclaimed default; there is no element
        // width to know, so it must not raise width_note (the Unknown
        // disclaimer). pshufb, by contrast, IS in the unambiguous table
        // (byte lanes) and always has been -- this fixture predates that
        // table entry, so pin what it actually resolves to now instead of
        // repeating the stale "both default" claim.
        check("T5/width: a bulk move has no lane semantics, not a default",
              p.writes[0].width == PrismWidth::NoLaneSemantics &&
                  p.writes[0].lane_bytes == kPrismDefaultLaneBytes,
              "movdqa (a bulk move) was not classified NoLaneSemantics");
        check("T5/width: 16 byte-lanes is the RENDERING default, not a claim",
              p.writes[0].bytes.size() / p.writes[0].lane_bytes == 16,
              "the default is not 16 byte-lanes");
        check("T5/width: pshufb is in the table and resolves to byte lanes",
              p.writes[1].width == PrismWidth::Lanes1 &&
                  p.writes[1].lane_bytes == 1,
              "pshufb is in kLaneWidths as 1-byte lanes; it must not share "
              "movdqa's NoLaneSemantics verdict");
        check("T5/width: no_lane_note fires here, NOT width_note",
              !p.no_lane_note.empty() && p.width_note.empty(),
              "a NoLaneSemantics write (movdqa) must raise no_lane_note, "
              "never the Unknown disclaimer (width_note) -- got "
              "width_note='" +
                  p.width_note + "' no_lane_note='" + p.no_lane_note + "'");

        // A mnemonic IN the table subdivides; movq does not -- but now for a
        // more precise reason than "ambiguous, so abstain": movq is
        // unconditionally in the bulk-move family (it has no element width
        // whether or not THIS use is the GPR<->vector form mnemonic_class
        // calls ambiguous, 54 T4). lane_width_class checks the bulk-move
        // family BEFORE that ambiguity gate even runs, so movq never reaches
        // it.
        {
            std::string d = kHdr;
            d += "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"disasm\":\"paddd "
                 "xmm0, xmm1\",\"ops\":[{\"space\":\"reg\",\"reg\":17,\"size\":"
                 "16,\"write\":true,\"value_valid\":true,\"wide\":true,"
                 "\"bytes\":\"000102030405060708090a0b0c0d0e0f\"}]}\n";
            d += "{\"k\":\"df_step\",\"step\":1,\"off\":4,\"disasm\":\"movq "
                 "xmm0, rax\",\"ops\":[{\"space\":\"reg\",\"reg\":17,\"size\":"
                 "16,\"write\":true,\"value_valid\":true,\"wide\":true,"
                 "\"bytes\":\"000102030405060708090a0b0c0d0e0f\"}]}\n";
            const Streams ds = decode_streams(mk_rec(d));
            const LanePrismScene q = build_lane_prism(ds.df, 17, "x86");
            check("T5/width: paddd subdivides to 32-bit lanes",
                  q.writes.size() == 2 &&
                      q.writes[0].width == PrismWidth::Lanes4 &&
                      q.writes[0].lane_bytes == 4,
                  "paddd did not resolve to dword lanes");
            check("T5/width: movq is a bulk move, not a disclaimed guess",
                  q.writes[1].width == PrismWidth::NoLaneSemantics &&
                      q.writes[1].lane_bytes == kPrismDefaultLaneBytes,
                  "movq (also ambiguous per 54 T4) must classify "
                  "NoLaneSemantics, not Unknown -- the bulk-move family check "
                  "runs first");
            check("T5/width: the no-lanes note rides movq, not the Unknown "
                  "disclaimer",
                  !q.no_lane_note.empty() && q.width_note.empty(),
                  "movq is a fact (no lanes), not a guess -- it must not "
                  "raise width_note");
        }

        // Absent bytes -> a [wide] WIREFRAME, never zero bars.
        {
            std::string w = kHdr;
            w += "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"disasm\":\"movdqa "
                 "xmm0, xmm1\",\"ops\":[{\"space\":\"reg\",\"reg\":17,\"size\":"
                 "16,\"write\":true,\"value_valid\":true,\"wide\":true}]}\n";
            w += "{\"k\":\"df_step\",\"step\":1,\"off\":4,\"disasm\":\"movdqa "
                 "xmm0, xmm2\",\"ops\":[{\"space\":\"reg\",\"reg\":17,\"size\":"
                 "16,\"write\":true,\"value_valid\":false,\"wide\":true,"
                 "\"bytes\":\"000102030405060708090a0b0c0d0e0f\"}]}\n";
            const Streams ws = decode_streams(mk_rec(w));
            const LanePrismScene q = build_lane_prism(ws.df, 17, "x86");
            check("T5/wireframe: absent bytes render a wireframe",
                  q.writes.size() == 2 && q.writes[0].wireframe &&
                      q.writes[0].bytes.empty(),
                  "an uncaptured buffer produced byte bars");
            check("T5/wireframe: never zero bars",
                  [&] {
                      for (const LaneByte &b : q.writes[0].bytes)
                          if (b.value == 0)
                              return false;
                      return true;
                  }(),
                  "the wireframe write carries fabricated zero bytes");
            check("T5/wireframe: counted", q.wireframes == 1,
                  "wireframes=" + std::to_string(q.wireframes));
            check("T5/hollow: value_valid == false renders hollow",
                  !q.writes[1].value_valid && q.hollow == 1,
                  "an uncaptured value was not marked hollow");
            check("T5/wireframe: nothing in a wireframe is pickable",
                  [&] {
                      for (const PrismElem &e : prism_pick_order(q))
                          if (e.write == 0)
                              return false;
                      return true;
                  }(),
                  "a wireframe byte was pickable — there is nothing there");
        }

        // The drill-in goes to the timeline at that step, carrying insn_off.
        check("T5/drill-in: opens the timeline at the write's step", [&] {
            const auto l = prism_pick_link(p, "r.asmtrace", 16);
            return l && l->view == dt_view::timeline && l->step &&
                   *l->step == 1 && l->off && *l->off == 4;
        }(), "the byte drill-in lost its step or insn offset");
        check("T5/drill-in: refuses an index past the geometry",
              !prism_pick_link(p, "r.asmtrace", prism_pick_order(p).size()),
              "an out-of-range byte resolved to a link");
        // No wide writes at all: a stated reason, not an empty prism.
        {
            const LanePrismScene none = build_lane_prism(DataflowStream{}, 0,
                                                         "x86");
            check("T5/empty: states its reason", !none.note.empty(),
                  "an empty prism is silent");
        }
    }

    // ---- T3: a slab with no cells is not a scene ----------------------------
    //
    // RegionInvocation::blocks is populated ONLY by a `coverage` event
    // (views/region.cpp). A recording whose trace events are not closed by a
    // coverage footer yields a trailing OPEN invocation with no blocks, so the
    // union set is empty and every slab gets zero cells. The availability gate
    // used to ask slabs.empty(), which is FALSE here — so the scene was offered
    // as available, with no reason text, and drew a frame ring around nothing.
    {
        RegionView rv;
        RegionInvocation inv;
        inv.number = 1;
        inv.closed = false;          // no coverage footer closed it
        inv.insns = {0x0, 0x4, 0x8}; // instructions WERE recorded
        // inv.blocks deliberately left empty — that is the whole case
        rv.invocations.push_back(inv);

        const InvocationScene s = build_invocation_scene(rv, nullptr);
        check("T3/blockless: still has a slab", s.slabs.size() == 1,
              "the builder must still produce the slab — dropping it would hide "
              "the last thing the target did before the recording ended");
        check("T3/blockless: no cells", s.slabs[0].cells.empty(),
              "with no union blocks there is nothing to place in the slab");
        check("T3/blockless: not drawable", !s.drawable(),
              "a slab with zero cells has no geometry; drawable() is what the "
              "availability gate must ask instead of slabs.empty()");
        check("T3/blockless: states its reason", !s.note.empty(),
              "an unavailable kind must state WHY (D7) — a blank refusal "
              "teaches nothing");
        check("T3/blockless: names coverage",
              s.note.find("coverage") != std::string::npos,
              "the note must name the missing coverage footer, the actual gap: "
              "got \"" + s.note + "\"");

        // The positive control: one closed invocation WITH blocks is drawable.
        RegionView ok;
        RegionInvocation good;
        good.number = 1;
        good.closed = true;
        good.insns = {0x0, 0x4};
        good.blocks = {0x0};
        ok.invocations.push_back(good);
        const InvocationScene g = build_invocation_scene(ok, nullptr);
        check("T3/blocked: drawable", g.drawable(),
              "a closed invocation with a block set must stay available");
        check("T3/blocked: no note", g.note.empty(),
              "an available kind carries no refusal text");
    }

    // ---- T5: Z counts WRITES (scene_kind.h) ---------------------------------
    //
    // The x86-64 producer captures wide values for READ operands too, so an
    // unfiltered walk stacks the pre-state read under the post-state write for
    // ONE step, and offers registers that were only ever read as selectable
    // subjects. The axis says "stacked writes, oldest nearest" and "Z counts
    // recorded writes"; the filter has to keep that promise.
    {
        DataflowStream df;
        auto wide_rec = [](uint32_t step, uint32_t reg, bool write) {
            ValRec v;
            v.step = step;
            v.space = "reg";
            v.reg = reg;
            v.size = 16;
            v.wide = true;
            v.write = write;
            v.value_valid = true;
            v.bytes.assign(16, 0u);
            return v;
        };
        // one `paddd xmm0, xmm1`: read xmm0, read xmm1, write xmm0
        df.recs.push_back(wide_rec(0, 100, false));
        df.recs.push_back(wide_rec(0, 101, false));
        df.recs.push_back(wide_rec(0, 100, true));

        const std::vector<uint32_t> regs = lane_prism_registers(df);
        check("T5/writes: only written registers are subjects",
              regs.size() == 1 && regs[0] == 100,
              "a register that was only READ is not a prism subject — Z counts "
              "writes; got " + std::to_string(regs.size()) + " register(s)");

        const LanePrismScene s = build_lane_prism(df, 100, "x86_64");
        check("T5/writes: one Z level per write", s.writes.size() == 1,
              "one step wrote xmm0 once, so there is ONE Z level; stacking the "
              "read under it draws two levels for one write");
        check("T5/writes: the survivor is the write",
              s.writes.empty() || s.writes[0].step == 0,
              "the surviving record must be the write, not the read");
    }

    // =====================================================================
    // T5 (2026-08-08 revised plan) — THREE width verdicts, not two
    // =====================================================================
    //
    // Measured over blend_tile (cli/scenes_victim.c:82, re-verified against a
    // fresh capture during this task): of its 11 wide REGISTER writes -- see
    // the task report for why this is 11 and not the brief's stated 12 -- the
    // table names 4 and misses 7. Of those 7, ALL SEVEN are bulk moves
    // (movdqa x5, movd x1) with no element width to know; there is no
    // "movaps" register write anywhere in this function (its one movaps is
    // the trailing store to memory, which is a REG READ + MEM WRITE, not a
    // register write, so is_prism_write correctly never counts it). Drawing
    // 1-byte lanes and saying "we guessed" about a bulk move is a false
    // claim, and it was the majority case (7 of 11 writes).
    {
        // A lane-bearing op the table has always known: measured, no guess.
        check("T5/width/known-lane-op",
              lane_width_class("paddd xmm0, xmm1", "x86_64") ==
                  PrismWidth::Lanes4,
              "paddd has 4-byte lanes and the table has always known it");

        // A bulk move: NOT a guess, and not 1-byte lanes -- it has no lanes.
        // This is 5 of blend_tile's 11 writes (movdqa) by itself.
        check("T5/width/bulk-move-has-no-lanes",
              lane_width_class("movdqa xmm2, xmmword ptr [rip + 0x2cde]",
                               "x86_64") == PrismWidth::NoLaneSemantics,
              "a bulk move has no element width to know, so the prism must "
              "say that rather than disclaim a guess it never made");

        // The real table gap the measurement found: unambiguously 4-byte
        // lanes (it interleaves two registers' dwords), and the ONE mnemonic
        // among blend_tile's misses that is not a bulk move.
        check("T5/width/punpckldq-is-4",
              lane_width_class("punpckldq xmm0, xmm1", "x86_64") ==
                  PrismWidth::Lanes4,
              "punpckldq unambiguously interleaves 4-byte lanes; it was the "
              "ONE genuine table miss blend_tile exercises");

        // Something the table really cannot classify stays an honest unknown
        // -- collapsing this into NoLaneSemantics is what the mutation test
        // below (and the unknown-stays-unknown name) checks does NOT happen.
        check("T5/width/unknown-stays-unknown",
              lane_width_class("someverynewinsn xmm0, xmm1", "x86_64") ==
                  PrismWidth::Unknown,
              "an instruction the table does not name is not the same as one "
              "with no lanes -- collapsing the two is what this task fixes");

        // The rest of the punpck family the table gained, spot-checked at
        // each width the brief specifies.
        check("T5/width/punpck-family-widths",
              lane_width_class("punpckhdq xmm0, xmm1", "x86_64") ==
                      PrismWidth::Lanes4 &&
                  lane_width_class("punpcklwd xmm0, xmm1", "x86_64") ==
                      PrismWidth::Lanes2 &&
                  lane_width_class("punpckhbw xmm0, xmm1", "x86_64") ==
                      PrismWidth::Lanes1 &&
                  lane_width_class("punpcklqdq xmm0, xmm1", "x86_64") ==
                      PrismWidth::Lanes8,
              "the punpck* family's four widths were not all filled in");

        // Every bulk-move spelling blend_tile's comment names, plus the two
        // the brief adds that this routine happens not to exercise (movdqu,
        // movups) -- all six must share ONE verdict, not five-plus-a-guess.
        check("T5/width/bulk-move-family-is-complete",
              lane_width_class("movaps xmm0, xmm1", "x86_64") ==
                      PrismWidth::NoLaneSemantics &&
                  lane_width_class("movups xmm0, xmm1", "x86_64") ==
                      PrismWidth::NoLaneSemantics &&
                  lane_width_class("movdqu xmm0, xmm1", "x86_64") ==
                      PrismWidth::NoLaneSemantics &&
                  lane_width_class("movd xmm0, eax", "x86_64") ==
                      PrismWidth::NoLaneSemantics,
              "movaps/movups/movdqu/movd must all be NoLaneSemantics, the "
              "same as movdqa/movq");

        // prism_width_known is the renderer's own predicate (standalone_gl.cpp's
        // lane-boundary tick): true for exactly the four measured widths.
        check("T5/width/known-predicate-matches-the-four-lane-widths",
              prism_width_known(PrismWidth::Lanes1) &&
                  prism_width_known(PrismWidth::Lanes2) &&
                  prism_width_known(PrismWidth::Lanes4) &&
                  prism_width_known(PrismWidth::Lanes8) &&
                  !prism_width_known(PrismWidth::NoLaneSemantics) &&
                  !prism_width_known(PrismWidth::Unknown),
              "prism_width_known must be true for the four LanesN verdicts "
              "only -- a lane-boundary tick drawn for the other two would "
              "look like a measurement it is not");
    }

    // ---- T5 (revised plan): the SCENE-level split, not just the classifier -
    //
    // The HUD reads LanePrismScene::width_note / no_lane_note, not the pure
    // classifier above -- so the split has to be proven at THAT level too.
    // A scene where every write is Known must raise NEITHER note; one where
    // every write is a bulk move must raise no_lane_note and NOT width_note
    // (the opposite of today's behaviour, which is the whole defect); and a
    // scene mixing a genuine Unknown with a bulk move must raise BOTH,
    // independently and with different text.
    {
        // All-known: paddd alone. Neither note is a fact worth stating.
        std::string k = kHdr;
        k += "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"disasm\":\"paddd "
             "xmm0, xmm1\",\"ops\":[{\"space\":\"reg\",\"reg\":5,\"size\":16,"
             "\"write\":true,\"value_valid\":true,\"wide\":true,\"bytes\":"
             "\"000102030405060708090a0b0c0d0e0f\"}]}\n";
        const LanePrismScene known =
            build_lane_prism(decode_streams(mk_rec(k)).df, 5, "x86_64");
        check("T5/width-scene/all-known-raises-no-note",
              known.writes.size() == 1 &&
                  known.writes[0].width == PrismWidth::Lanes4 &&
                  known.width_note.empty() && known.no_lane_note.empty(),
              "a lane-bearing op the table names needs NO disclaimer at all "
              "-- got width_note='" +
                  known.width_note + "' no_lane_note='" + known.no_lane_note +
                  "'");

        // Mixed: paddd (Known) + movaps (NoLaneSemantics) + an unrecognised
        // mnemonic (Unknown), one register's write history.
        std::string m = kHdr;
        m += "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"disasm\":\"paddd "
             "xmm0, xmm1\",\"ops\":[{\"space\":\"reg\",\"reg\":9,\"size\":16,"
             "\"write\":true,\"value_valid\":true,\"wide\":true,\"bytes\":"
             "\"000102030405060708090a0b0c0d0e0f\"}]}\n";
        m += "{\"k\":\"df_step\",\"step\":1,\"off\":4,\"disasm\":\"movaps "
             "xmm0, xmm2\",\"ops\":[{\"space\":\"reg\",\"reg\":9,\"size\":16,"
             "\"write\":true,\"value_valid\":true,\"wide\":true,\"bytes\":"
             "\"101112131415161718191a1b1c1d1e1f\"}]}\n";
        m += "{\"k\":\"df_step\",\"step\":2,\"off\":8,\"disasm\":"
             "\"someverynewinsn xmm0, xmm3\",\"ops\":[{\"space\":\"reg\","
             "\"reg\":9,\"size\":16,\"write\":true,\"value_valid\":true,"
             "\"wide\":true,\"bytes\":\"202122232425262728292a2b2c2d2e2f\"}]"
             "}\n";
        const LanePrismScene mix =
            build_lane_prism(decode_streams(mk_rec(m)).df, 9, "x86_64");
        check("T5/width-scene/three-writes-three-verdicts",
              mix.writes.size() == 3 &&
                  mix.writes[0].width == PrismWidth::Lanes4 &&
                  mix.writes[1].width == PrismWidth::NoLaneSemantics &&
                  mix.writes[2].width == PrismWidth::Unknown,
              "paddd/movaps/someverynewinsn did not land in their three "
              "distinct verdicts");
        check("T5/width-scene/both-notes-fire-independently",
              !mix.width_note.empty() && !mix.no_lane_note.empty() &&
                  mix.width_note != mix.no_lane_note,
              "a scene with one Unknown write and one NoLaneSemantics write "
              "must carry BOTH notes, with DIFFERENT text -- got "
              "width_note='" +
                  mix.width_note + "' no_lane_note='" + mix.no_lane_note + "'");
        // The Known write's own geometry is unaffected by its neighbours'
        // verdicts -- lane_bytes stays the measured 4, not pulled to the
        // default by the OTHER writes' fallback.
        check("T5/width-scene/known-write-keeps-its-own-lane-bytes",
              mix.writes[0].lane_bytes == 4,
              "a Known write's lane_bytes must not be affected by a sibling "
              "write's verdict");
    }

    if (failures) {
        std::fprintf(stderr, "%d standalone-scene check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_standalone: all checks passed\n");
    return 0;
}
