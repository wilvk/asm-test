// test_obs_topo.cpp — the topology map (08-observer-views.md T3) and its
// node-editor graph layout (15-plotting-and-graph-nav.md T3).
#include "views/graph_nav.h"
#include "views/topo.h"

#include <algorithm>
#include <vector>

#include "view_test.h"

using namespace asmdesk;

int main() {
    Recording rec = vt::load_fixture("obs-topo.asmtrace");
    TopoView v = obs_topo_build(rec);

    // A snapshot is a complete statement of the tree at one moment, so the LAST
    // one is the view — merging them would describe a tree that never existed.
    vt::eq("snapshots seen", v.snapshots, size_t{2});
    vt::eq("cards", v.cards.size(), size_t{2});
    vt::eq("cards ordered by pid", v.cards[0].tgid, 4200L);
    vt::eq("threads folded into the process", v.cards[0].threads.size(),
           size_t{2});
    vt::check("leader first", v.cards[0].threads[0].leader,
              "the leader carries the card's identity and sorts first");
    vt::eq("leader identity used", v.cards[0].exe, std::string("spy_victim"));
    vt::eq("inv summed over threads", v.cards[0].inv_total, uint64_t{131});
    vt::eq("child process is its own card", v.cards[1].tgid, 4301L);
    vt::eq("child's parent", v.cards[1].ppid, 4200L);

    // The count's UNIT travels with the number: `inv` is syscalls or calls, and
    // the two are not the same measurement.
    vt::eq("count mode", v.count_mode, std::string("syscalls"));
    std::string fp = obs_topo_fingerprint(v, v.cards[0]);
    vt::check("fingerprint names the unit",
              fp.find("131 syscalls") != std::string::npos,
              "a bare invocation count means nothing: " + fp);
    vt::check("fingerprint names the thread count",
              fp.find("2 threads") != std::string::npos, fp);

    // The jack is held TREE-wide while a topology session runs.
    vt::check("jack note states the tree-wide hold",
              std::string(obs_topo_jack_note()).find("descendant tree") !=
                  std::string::npos,
              "the note must say the hold covers every process below");
    vt::check("jack note is in the render",
              obs_topo_dump(v).find("descendant tree") != std::string::npos,
              "the hold must be on screen, not merely available");

    // Drill-in is a deep link through 04's router, so it round-trips as text.
    dt_link l = obs_topo_drill_link("live-session.asmtrace", v.cards[1]);
    vt::eq("drill-in view", std::string(dt_view_name(l.view)),
           std::string("syscalls"));
    vt::check("drill-in names the process", l.pid.has_value(),
              "a per-process link with no process is not a drill-in");
    vt::eq("drill-in pid", *l.pid, 4301L);
    std::string text = dt_nav_format(l);
    dt_link back;
    std::string err;
    vt::check("link parses back", dt_nav_parse(text, back, err), err);
    vt::eq("pid round-trips", back.pid ? *back.pid : 0L, 4301L);
    vt::eq("link is byte-stable", dt_nav_format(back), text);

    // --- the node-editor graph layout (15 T3) ------------------------------
    // The fidelity guardrail: node-editor is fed the app's own positions every
    // frame and its settings file is DISABLED, so it can neither persist a
    // dragged position nor invent one on load — the layout stays the app's, and
    // force-directed layout stays banned (docs 04/08).
    vt::check("node-editor persists no positions",
              kGraphSettingsFile == nullptr,
              "a non-null SettingsFile would let node-editor own positions");

    GraphLayout g = graph_from_topo(v, "live-session.asmtrace");
    vt::eq("one graph node per process", g.nodes.size(), v.cards.size());

    auto find = [&](uint64_t id) -> const GraphNode * {
        for (const GraphNode &n : g.nodes)
            if (n.id == id)
                return &n;
        return nullptr;
    };
    const GraphNode *root = find(4200), *kid = find(4301);
    vt::check("root process is a node", root != nullptr,
              "no node for tgid 4200");
    vt::check("child process is a node", kid != nullptr,
              "no node for tgid 4301");
    if (root && kid) {
        // The coordinate IS layer*width / row*height — the app's deterministic
        // layers — NOT whatever a settling pass would have produced.
        vt::eq("root sits at layer 0 (x)", root->x, 0.0f);
        vt::eq("root sits at row 0 (y)", root->y, 0.0f);
        vt::eq("child is one layer deeper", kid->x, kGraphColW);
        vt::eq("child at row 0 of its layer", kid->y, 0.0f);
    }
    // Parent -> child edge, straight off the snapshot's ppid — never synthesized.
    bool has_edge = false;
    for (const GraphEdge &e : g.edges)
        has_edge = has_edge || (e.from_id == 4200 && e.to_id == 4301);
    vt::check("parent -> child edge present", has_edge,
              "the 4200 -> 4301 ancestry edge is missing");

    // A node click routes through 04's dt_nav_go — the graph's equivalent of the
    // card list's "open this process" button, never a private reach into a view.
    if (kid) {
        vt::check("a clickable node carries a link", kid->has_link,
                  "a node with no destination cannot be a drill-in");
        vt::eq("click routes to the syscall drill-in",
               std::string(dt_view_name(kid->link.view)),
               std::string("syscalls"));
        vt::eq("click targets the process", kid->link.pid ? *kid->link.pid : 0L,
               4301L);
        dt_nav_table tbl;
        bool fired = false;
        tbl.have_recording = [](const std::string &) { return true; };
        dt_nav_register(tbl, dt_view::syscalls,
                        [&](const dt_link &) { fired = true; });
        vt::check("dt_nav_go accepts the node link", dt_nav_go(tbl, kid->link),
                  tbl.last_error);
        vt::check("the router actually fired", fired,
                  "the node click never reached a handler");
    }

    // Culling drops off-viewport nodes: a viewport over layer 0 alone must not
    // emit the layer-1 child (doc 11 perf note — a large graph emits only what
    // shows).
    GraphViewport layer0{-10.0f, -10.0f, kGraphColW - 20.0f, kGraphRowH};
    std::vector<std::size_t> vis = graph_visible(g, layer0);
    vt::check("cull keeps the on-screen root", !vis.empty(),
              "the root is inside the viewport and must survive");
    vt::check("cull drops the off-screen child", vis.size() < g.nodes.size(),
              "the layer-1 child is off-viewport and must be culled");

    vt::golden("obs-topo.txt", obs_topo_dump(v));
    return vt::report("test_obs_topo");
}
