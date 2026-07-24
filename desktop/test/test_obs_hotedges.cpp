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
    Recording bad = vt::load_fixture("obs-survey-dishonest.asmtrace");
    HotEdgeView b = obs_hotedges_build(bad);
    vt::check("provenance conflict caught", !b.provenance_conflict.empty(),
              "a survey declaring exact:true went unremarked");
    vt::check("still rendered as statistical",
              obs_hotedges_dump(b).find("STATISTICAL") != std::string::npos,
              "a producer's wrong claim must not upgrade the data's trust");

    vt::golden("obs-hotedges-ibs.txt", dump);
    vt::golden("obs-hotedges-sw.txt", obs_hotedges_dump(s));
    return vt::report("test_obs_hotedges");
}
