// test_obs_hotedges.cpp — the hot-edge table (08-observer-views.md T4).
//
// Three properties, all of which fail quietly if nobody asserts them: the two
// samplers do not carry the same grade of evidence and must not be labelled
// alike; the drop record is always on screen; and edges are never stacked into
// frames (the check below feeds a CHAIN of edges and demands the view still has
// exactly as many rows as there were edges — a flame-shaped renderer would have
// synthesised ancestry from those three rows).
#include "views/hotedges.h"

#include "view_test.h"

using namespace asmdesk;

int main() {
    Recording ibs = vt::load_fixture("obs-survey-ibs.asmtrace");
    HotEdgeView v = obs_hotedges_build(ibs);

    vt::eq("edges", v.edges.size(), size_t{3});
    // Deterministic ranking: descending count, ties by address — so two runs
    // of the same recording can be compared, and a screenshot means something.
    vt::eq("rank 1 count", v.edges[0].count, uint64_t{812});
    vt::eq("tie broken by from_addr", v.edges[0].from_addr, uint64_t{4198710});
    vt::eq("rank 2", v.edges[1].from_addr, uint64_t{4198800});
    vt::eq("rank 3 is the small one", v.edges[2].count, uint64_t{40});
    vt::eq("ranks are 1-based", v.edges[0].rank, 1);

    // The edges A->B, B->C, C->A form a chain. A flame graph would fold them
    // into frames; this view must still show three EDGES.
    vt::eq("no frames synthesised", v.edges.size(), size_t{3});
    vt::check("no flame note is stated",
              std::string(obs_hotedges_no_flame_note()).find("not stacks") !=
                  std::string::npos,
              "the view must say why there is no flame graph");

    // The chrome is not optional.
    std::string chrome = obs_hotedges_chrome_line(v);
    for (const char *field : {"samples=10442", "branch_samples=9001", "lost=17",
                              "throttled=yes", "sampler=ibs-op"})
        vt::check(std::string("chrome has ") + field,
                  chrome.find(field) != std::string::npos, chrome);
    vt::check("window shown", chrome.find("0x401000+4096") != std::string::npos,
              chrome);
    std::string dump = obs_hotedges_dump(v);
    vt::check("chrome is in the render",
              dump.find("lost=17") != std::string::npos,
              "the drop record must be on screen, always");
    vt::check("statistical said first",
              dump.rfind("STATISTICAL", 0) == 0 ||
                  dump.find("STATISTICAL") < dump.find("#1"),
              "statistical data must be labelled before its numbers");

    // Evidence grade: IBS is an entry observation, sw-clock is residency.
    vt::check("ibs is strong evidence", !obs_hotedges_weak_evidence(v),
              "an IBS-Op branch edge IS the event a capture waits for");
    vt::check("ibs label says direct",
              obs_hotedges_evidence_label(v).find("DIRECT") !=
                  std::string::npos,
              obs_hotedges_evidence_label(v));

    Recording sw = vt::load_fixture("obs-survey-sw.asmtrace");
    HotEdgeView s = obs_hotedges_build(sw);
    vt::check("sw-clock is weak evidence", obs_hotedges_weak_evidence(s),
              "residency is not entry evidence");
    vt::check("sw label says weaker",
              obs_hotedges_evidence_label(s).find("WEAKER") !=
                  std::string::npos,
              obs_hotedges_evidence_label(s));
    vt::check("the two labels differ",
              obs_hotedges_evidence_label(s) != obs_hotedges_evidence_label(v),
              "two grades of evidence rendered identically");

    // The picker's ranking is this same model, prefixed.
    std::vector<HotEdge> top = obs_hotedges_top(v, 2);
    vt::eq("top-2", top.size(), size_t{2});
    vt::eq("top-2 is the ranked prefix", top[1].from_addr,
           v.edges[1].from_addr);
    vt::eq("top-N past the end is clamped", obs_hotedges_top(s, 99).size(),
           size_t{2});

    // A survey is exact:false BY CONSTRUCTION; a recording claiming otherwise
    // is a producer defect, and the view keeps rendering it as statistical.
    Recording bad = vt::load_fixture("obs-survey-low-fidelity.asmtrace");
    HotEdgeView b = obs_hotedges_build(bad);
    vt::check("provenance conflict caught", !b.provenance_conflict.empty(),
              "a survey declaring exact:true went unremarked");
    vt::check("still rendered as statistical",
              obs_hotedges_dump(b).find("STATISTICAL") != std::string::npos,
              "a producer's wrong claim must not upgrade the data's trust");

    // The ImPlot heatmap's data source (15 T1): a bounded src×dst grid of the
    // edge weights — preserves total weight (never a stack / never invented),
    // sizes correctly, and honours the cap with a truncation flag.
    HotEdgeMatrix hm = obs_hotedges_matrix(v, 24);
    uint64_t total = 0;
    for (const HotEdge &e : v.edges)
        total += e.count;
    double sum = 0;
    for (double c : hm.cells)
        sum += c;
    vt::check("heatmap preserves total edge weight", sum == static_cast<double>(total),
              "the heatmap dropped or invented sample weight");
    vt::eq("heatmap cells sized rows*cols", hm.cells.size(),
           hm.rows.size() * hm.cols.size());
    vt::check("3 edges do not truncate at cap 24", !hm.truncated, "over-capped");
    HotEdgeMatrix cap1 = obs_hotedges_matrix(v, 1);
    vt::check("cap bounds the matrix", cap1.rows.size() <= 1 && cap1.cols.size() <= 1,
              "the cap was not honoured");
    if (hm.rows.size() > 1 || hm.cols.size() > 1)
        vt::check("dropping edges to the cap is flagged", cap1.truncated,
                  "a capped heatmap must say it is incomplete");
    vt::check("cap 0 yields nothing", obs_hotedges_matrix(v, 0).cells.empty(),
              "cap 0 should produce an empty matrix");

    vt::golden("obs-hotedges-ibs.txt", dump);
    vt::golden("obs-hotedges-sw.txt", obs_hotedges_dump(s));

    // 54 T7: the scene accessor — the DECIDED source for a 3D statistical
    // layer. Same edges, same order, with count/mispred/is_return preserved
    // (from/to text and rank are deliberately absent from the returned type).
    HotEdgeSceneView scene = obs_hotedges_for_scene(v);
    vt::eq("scene: same edge count", scene.edges.size(), v.edges.size());
    for (size_t i = 0; i < v.edges.size() && i < scene.edges.size(); i++) {
        vt::eq("scene: from_addr order preserved at " + std::to_string(i),
               scene.edges[i].from_addr, v.edges[i].from_addr);
        vt::eq("scene: to_addr preserved at " + std::to_string(i),
               scene.edges[i].to_addr, v.edges[i].to_addr);
        vt::eq("scene: count preserved at " + std::to_string(i),
               scene.edges[i].count, v.edges[i].count);
        vt::eq("scene: mispred preserved at " + std::to_string(i),
               scene.edges[i].mispred, v.edges[i].mispred);
        vt::eq("scene: is_return preserved at " + std::to_string(i),
               scene.edges[i].is_return, v.edges[i].is_return);
    }

    // The view-level fidelity fields survive the projection — a consumer
    // reads them from HotEdgeSceneView directly, with no second decode path
    // back through HotEdgeView.
    vt::eq("scene: sampler survives", scene.sampler, v.sampler);
    vt::eq("scene: samples survives", scene.samples, v.samples);
    vt::eq("scene: branch_samples survives", scene.branch_samples,
           v.branch_samples);
    vt::eq("scene: lost survives", scene.lost, v.lost);
    vt::eq("scene: throttled survives", scene.throttled, v.throttled);
    vt::eq("scene: have_window survives", scene.have_window, v.have_window);
    vt::eq("scene: window_base survives", scene.window_base, v.window_base);
    vt::eq("scene: window_len survives", scene.window_len, v.window_len);

    // An empty survey yields an empty edge set, never a silent zero-count
    // edge — and the fidelity fields still state WHY (samples=0), rather
    // than the accessor inventing something to show.
    HotEdgeSceneView empty = obs_hotedges_for_scene(HotEdgeView{});
    vt::check("scene: empty survey yields no edges", empty.edges.empty(),
              "an edgeless view must not synthesise one");
    vt::eq("scene: empty survey states zero samples (the reason)",
           empty.samples, uint64_t{0});

    // === 56 T5: build_mispred_layer — the misprediction survey layer's ======
    // ============================== plane-space geometry ====================
    {
        using space::MispredArc;
        using space::MispredLayer;
        using space::Region;
        std::vector<Region> regs;
        Region r1;
        r1.base = 0x1000;
        r1.len = 0x1000;
        r1.kind = Region::Code;
        r1.label = "r1";
        regs.push_back(r1);
        Region r2;
        r2.base = 0x5000;
        r2.len = 0x1000;
        r2.kind = Region::Code;
        r2.label = "r2";
        regs.push_back(r2);
        space::Projection proj = space::build_projection(regs);

        HotEdgeSceneView mv;
        // forward, within r1: 0x1010 -> 0x1020
        mv.edges.push_back({0x1010, 0x1020, 100, 10, 0});
        // BACKWARD, within r1: 0x1020 -> 0x1010 (a latch/loop-back shape)
        mv.edges.push_back({0x1020, 0x1010, 50, 5, 0});
        // from=0x1010 again (aggregates into the SAME site as edge 1), to
        // is off any region entirely.
        mv.edges.push_back({0x1010, 0xdeadbeef00ULL, 20, 0, 1});
        // from is off any region entirely; to=0x1010.
        mv.edges.push_back({0xdeadbeef00ULL, 0x1010, 5, 1, 0});

        MispredLayer layer = build_mispred_layer(mv, proj);
        vt::eq("mispred: 2 arcs (both endpoints placed)", layer.arcs.size(),
               size_t{2});
        vt::eq("mispred: 2 off-plane endpoints (edge 3's to, edge 4's from)",
               layer.off_plane, uint32_t{2});

        const MispredArc *forward = nullptr, *backward = nullptr;
        for (const MispredArc &a : layer.arcs) {
            if (a.count == 100)
                forward = &a;
            else if (a.count == 50)
                backward = &a;
        }
        vt::check("mispred: forward arc found", forward != nullptr, "");
        vt::check("mispred: backward arc found", backward != nullptr, "");
        if (forward)
            vt::check("mispred: forward (to > from) is NOT a derived latch",
                      !forward->derived_latch, "");
        if (backward)
            vt::check(
                "mispred: backward, same region IS a derived latch (rendering "
                "observation, not a recorded fact)",
                backward->derived_latch, "");

        // Sites: from=0x1010 aggregates edges 1 (count 100) and 3 (count 20,
        // is_return=1) into ONE site (same cell, same literal address) =
        // count 120, mispred 10, is_return true. from=0x1020 is its own
        // site: count 50, mispred 5, is_return false. Edge 4's from is
        // off-plane and contributes no site.
        vt::eq("mispred: 2 sites (0x1010 and 0x1020)", layer.sites.size(),
               size_t{2});
        uint64_t site1010_count = 0, site1020_count = 0;
        bool site1010_return = false;
        for (const auto &s : layer.sites) {
            if (s.count == 120) {
                site1010_count = s.count;
                site1010_return = s.is_return;
            } else if (s.count == 50) {
                site1020_count = s.count;
            }
        }
        vt::eq("mispred: 0x1010 site aggregates count (100+20)",
               site1010_count, uint64_t{120});
        vt::check("mispred: 0x1010 site is_return (edge 3 was a return)",
                  site1010_return, "");
        vt::eq("mispred: 0x1020 site count (edge 2 alone)", site1020_count,
               uint64_t{50});

        // An empty survey yields an empty layer, never fabricated geometry.
        MispredLayer none = build_mispred_layer(HotEdgeSceneView{}, proj);
        vt::check("mispred: empty survey yields no arcs/sites",
                  none.arcs.empty() && none.sites.empty(),
                  "an edgeless survey must not synthesise geometry");
        vt::eq("mispred: empty survey has zero off-plane", none.off_plane,
               uint32_t{0});
    }

    return vt::report("test_obs_hotedges");
}
