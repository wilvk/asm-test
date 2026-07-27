// test_obs_tree.cpp — the call tree + filter panel (08-observer-views.md T5).
//
// The filter panel is the one place in this doc where the client makes a
// REQUEST rather than rendering a fact, so the checks are about what it sends:
// the refusals match the protocol's word for word, an unset field is omitted
// rather than sent at its default (omitted and `depth:0` are different
// requests, and only one of them is legal), and the effective parameters come
// back off the wire rather than being assumed to be what was asked for.
#include "views/graph_nav.h"
#include "views/tree.h"

#include <vector>

#include "view_test.h"

using namespace asmdesk;

int main() {
    Recording rec = vt::load_fixture("obs-tree.asmtrace");
    TreeView v = obs_tree_build(rec);

    vt::eq("rows", v.rows.size(), size_t{5});
    vt::eq("tids", obs_tree_tids(v).size(), size_t{2});
    vt::eq("depth is the engine's", v.rows[2].depth, 2);
    vt::eq("jit module survives", v.rows[4].module, std::string("jit"));

    // The effective filter is READ, never assumed.
    vt::check("effective params echoed", v.have_effective,
              "the started event's params echo did not reach the view");
    vt::eq("effective depth", v.effective.depth, 3);
    vt::eq("effective focus", v.effective.focus, std::string("work"));
    vt::eq("effective module", v.effective.module, std::string("spy_victim"));
    vt::check("effective follow", v.effective.follow, "follow:true was echoed");

    // --- the refusals, verbatim -------------------------------------------
    TreeFilter f;
    vt::eq("empty filter is legal", obs_tree_filter_error(f), std::string());
    f.tid = 4243;
    f.follow = true;
    vt::eq("tid XOR follow", obs_tree_filter_error(f),
           std::string("\"tid\" pins ONE task; \"follow\" adds child "
                       "processes — drop one"));
    vt::eq("blocked filter sends nothing", obs_tree_start_command(f, 4242),
           std::string());
    f.follow = false;
    vt::eq("tid alone is legal", obs_tree_filter_error(f), std::string());
    f.depth = 0; // omitted == unlimited, and legal
    vt::eq("depth 0 means omitted", obs_tree_filter_error(f), std::string());
    f.depth = -1;
    vt::eq("negative depth refused", obs_tree_filter_error(f),
           std::string("\"depth\" must be 1..1000 (omit it for unlimited)"));
    f.depth = 1001;
    vt::eq("depth over 1000 refused", obs_tree_filter_error(f),
           std::string("\"depth\" must be 1..1000 (omit it for unlimited)"));

    // --- the start command -------------------------------------------------
    TreeFilter g;
    g.depth = 3;
    g.focus = "work";
    g.module = "spy_victim";
    g.follow = true;
    std::string cmd = obs_tree_start_command(g, 4242);
    nlohmann::json j = nlohmann::json::parse(cmd);
    vt::eq("cmd", j.value("cmd", std::string()), std::string("start"));
    vt::eq("mode", j.value("mode", std::string()), std::string("tree"));
    vt::eq("pid", j.value("pid", 0L), 4242L);
    vt::eq("depth on the wire", j.value("depth", 0), 3);
    vt::eq("focus on the wire", j.value("focus", std::string()),
           std::string("work"));
    vt::check("tid omitted when unset", !j.contains("tid"),
              "an unset tid must be OMITTED, not sent as 0");
    // The round trip a live session actually performs: what we send is what the
    // fixture's `started` echo says came back.
    vt::eq("round-trip depth", j.value("depth", 0), v.effective.depth);
    vt::eq("round-trip focus", j.value("focus", std::string()),
           v.effective.focus);
    vt::eq("round-trip module", j.value("module", std::string()),
           v.effective.module);
    vt::eq("round-trip follow", j.value("follow", false), v.effective.follow);

    TreeFilter empty;
    nlohmann::json p = obs_tree_start_params(empty);
    vt::check("nothing at its default is sent", p.empty(),
              "an all-default filter must send no parameters at all");

    // --- the node-editor graph layout (15 T3) ------------------------------
    // Same guardrail as topo: the layout is the app's, fed every frame, and the
    // settings file is disabled so nothing drifts (docs 04/08).
    vt::check("node-editor persists no positions",
              kGraphSettingsFile == nullptr,
              "a non-null SettingsFile would let node-editor own positions");

    GraphLayout graph = graph_from_tree(v, "obs-tree.asmtrace");
    vt::eq("one graph node per call", graph.nodes.size(), v.rows.size());
    // x is the ENGINE's effective depth (re-based under a focus filter, never
    // recomputed), y is emission order — both the app's, not the library's.
    vt::eq("node id is the row's 1-based index", graph.nodes[2].id,
           uint64_t{3});
    vt::eq("depth-2 call sits at layer 2 (x)", graph.nodes[2].x,
           2.0f * kGraphColW);
    vt::eq("third call sits at row 2 (y)", graph.nodes[2].y, 2.0f * kGraphRowH);

    // The parent edge is the nearest preceding shallower row; at least one call
    // in this fixture is nested, so the tree has structure (never a synthesized
    // parent — the depths are the engine's).
    vt::check(
        "the call tree has parent edges", !graph.edges.empty(),
        "a nested call tree must produce at least one parent -> child edge");

    // A node with a resolved address links to that address (region view), routed
    // through 04's dt_nav_go.
    const GraphNode *linked = nullptr;
    for (const GraphNode &n : graph.nodes)
        if (n.has_link) {
            linked = &n;
            break;
        }
    vt::check("a resolved call is clickable", linked != nullptr,
              "no call carried an address to navigate to");
    if (linked) {
        vt::eq("click routes to the address in the region view",
               std::string(dt_view_name(linked->link.view)),
               std::string("region"));
        vt::check("click carries the code offset", linked->link.off.has_value(),
                  "a region link with no offset is not a destination");
        dt_nav_table tbl;
        bool fired = false;
        tbl.have_recording = [](const std::string &) { return true; };
        dt_nav_register(tbl, dt_view::region,
                        [&](const dt_link &) { fired = true; });
        vt::check("dt_nav_go accepts the node link",
                  dt_nav_go(tbl, linked->link), tbl.last_error);
        vt::check("the router actually fired", fired,
                  "the node click never reached a handler");
    }

    // Visible-region culling on a LARGE fixture (doc 11 perf note): a
    // 10k-routine trace must not emit 10k off-screen nodes.
    TreeView big;
    for (int i = 0; i < 10000; i++) {
        TreeRow r;
        r.tid = 1;
        r.depth = i % 8; // eight layers, so a narrow viewport excludes most
        r.addr = 0x1000 + static_cast<uint64_t>(i);
        r.name = "f";
        big.rows.push_back(r);
    }
    GraphLayout bg = graph_from_tree(big, "big");
    vt::eq("a node per routine in the large graph", bg.nodes.size(),
           size_t{10000});
    GraphViewport small{0.0f, 0.0f, kGraphColW * 2.0f, kGraphRowH * 5.0f};
    std::vector<std::size_t> vis = graph_visible(bg, small);
    vt::check("culling keeps the on-viewport nodes", !vis.empty(),
              "nodes inside the viewport must survive the cull");
    vt::check("culling drops the off-viewport nodes", vis.size() < size_t{2500},
              "a 10k-routine graph must emit only the visible region, not all");

    vt::golden("obs-tree.txt", obs_tree_dump(v));
    return vt::report("test_obs_tree");
}
