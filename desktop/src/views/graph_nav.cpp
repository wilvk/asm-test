// graph_nav.cpp — the pure builders + culling of graph_nav.h. No ImGui, no
// node-editor, no I/O. Every position here is a fixed function of the model, so
// the same recording lays out the same graph on every frame and host — which is
// what lets the draw half feed node-editor positions it never chose.
#include "views/graph_nav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_map>

namespace asmdesk {

namespace {

std::string hexs(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// A region deep-link for a resolved address (04's router). Absolute addresses
// are the region view's basis (it deals in the process's own address space), so
// a call target or branch endpoint lands there honestly.
dt_link region_link(const std::string &rec_id, uint64_t addr) {
    dt_link l;
    l.rec = rec_id;
    l.view = dt_view::region;
    l.off = addr;
    return l;
}

} // namespace

GraphLayout graph_from_topo(const TopoView &v, const std::string &rec_id) {
    GraphLayout g;

    // Which tgids are present, so a parent that is not in the snapshot marks its
    // child as a layer-0 root rather than dangling an edge to nothing.
    std::map<long, const TopoCard *> by_tgid;
    for (const TopoCard &c : v.cards)
        by_tgid[c.tgid] = &c;

    // Ancestry depth: walk the ppid chain up to a root (a ppid not in the set),
    // capped by the card count so a malformed cycle terminates deterministically.
    auto depth_of = [&](const TopoCard &c) {
        int d = 0;
        long p = c.ppid;
        size_t guard = 0;
        while (p != 0 && by_tgid.count(p) && guard++ < by_tgid.size()) {
            d++;
            p = by_tgid[p]->ppid;
        }
        return d;
    };

    std::map<int, int> row_of_layer; // next free row index per depth
    for (const TopoCard &c : v.cards) {
        int d = depth_of(c);
        int row = row_of_layer[d]++;
        GraphNode n;
        n.id = static_cast<uint64_t>(c.tgid);
        n.x = static_cast<float>(d) * kGraphColW;
        n.y = static_cast<float>(row) * kGraphRowH;
        n.label = obs_topo_fingerprint(v, c);
        n.link = obs_topo_drill_link(rec_id, c);
        n.has_link = true;
        g.nodes.push_back(std::move(n));
        // Parent -> child edge only when the parent is in this snapshot.
        if (c.ppid != 0 && by_tgid.count(c.ppid))
            g.edges.push_back(
                {static_cast<uint64_t>(c.ppid), static_cast<uint64_t>(c.tgid)});
    }
    return g;
}

GraphLayout graph_from_tree(const TreeView &v, const std::string &rec_id) {
    GraphLayout g;
    // last_at_depth[d] = the id of the most recent row seen at depth d. A row's
    // parent is the last row one level shallower — the conventional call-tree
    // parent inference, and deterministic because the rows are in emission order.
    std::unordered_map<int, uint64_t> last_at_depth;
    for (size_t i = 0; i < v.rows.size(); i++) {
        const TreeRow &r = v.rows[i];
        int d = r.depth < 0 ? 0 : r.depth;
        GraphNode n;
        n.id = static_cast<uint64_t>(i + 1); // 1-based; 0 is Invalid
        n.x = static_cast<float>(d) * kGraphColW;
        n.y = static_cast<float>(i) * kGraphRowH;
        n.label = r.name.empty() ? std::string("(unresolved)") : r.name;
        if (!r.module.empty())
            n.label += " [" + r.module + "]";
        if (r.addr != 0) {
            n.link = region_link(rec_id, r.addr);
            n.has_link = true;
        }
        // Parent edge to the nearest preceding row one level up.
        if (d > 0) {
            auto it = last_at_depth.find(d - 1);
            if (it != last_at_depth.end())
                g.edges.push_back({it->second, n.id});
        }
        last_at_depth[d] = n.id;
        // A deeper subtree that follows must not reattach to a stale sibling at a
        // level we have since left, so forget everything below this row's depth.
        for (auto it = last_at_depth.begin(); it != last_at_depth.end();) {
            if (it->first > d)
                it = last_at_depth.erase(it);
            else
                ++it;
        }
        g.nodes.push_back(std::move(n));
    }
    return g;
}

GraphLayout graph_from_hotedges(const HotEdgeView &v,
                                const std::string &rec_id) {
    GraphLayout g;
    // Distinct branch endpoints in first-seen (rank) order. A stable grid keeps
    // the frozen snapshot frozen: the same survey draws the same picture.
    std::unordered_map<uint64_t, size_t> index_of;
    auto intern = [&](uint64_t addr, const std::string &label) -> size_t {
        auto it = index_of.find(addr);
        if (it != index_of.end())
            return it->second;
        size_t idx = g.nodes.size();
        index_of[addr] = idx;
        GraphNode n;
        n.id = static_cast<uint64_t>(idx + 1);
        n.label = label.empty() ? hexs(addr) : label;
        n.link = region_link(rec_id, addr);
        n.has_link = addr != 0;
        g.nodes.push_back(std::move(n));
        return idx;
    };
    for (const HotEdge &e : v.edges) {
        size_t f = intern(e.from_addr, e.from);
        size_t t = intern(e.to_addr, e.to);
        g.edges.push_back({g.nodes[f].id, g.nodes[t].id});
    }
    // Place the interned nodes on a near-square grid, in interned order.
    int cols = static_cast<int>(std::ceil(
        std::sqrt(static_cast<double>(std::max<size_t>(1, g.nodes.size())))));
    if (cols < 1)
        cols = 1;
    for (size_t k = 0; k < g.nodes.size(); k++) {
        g.nodes[k].x = static_cast<float>(k % cols) * kGraphColW;
        g.nodes[k].y = static_cast<float>(k / cols) * kGraphRowH;
    }
    return g;
}

std::vector<std::size_t> graph_visible(const GraphLayout &g,
                                       const GraphViewport &vp) {
    std::vector<std::size_t> out;
    // A degenerate viewport (first frame, before node-editor has a view) culls
    // nothing — everything shows so "fit graph" has the whole graph to fit.
    const bool cull = vp.w > 0.0f && vp.h > 0.0f;
    for (std::size_t i = 0; i < g.nodes.size(); i++) {
        const GraphNode &n = g.nodes[i];
        if (cull) {
            const bool overlap = n.x < vp.x + vp.w &&
                                 n.x + kGraphNodeW > vp.x &&
                                 n.y < vp.y + vp.h && n.y + kGraphNodeH > vp.y;
            if (!overlap)
                continue;
        }
        out.push_back(i);
    }
    return out;
}

} // namespace asmdesk
