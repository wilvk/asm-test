// graph_nav.h — deterministic graph layouts for the node-editor canvas
// (15-plotting-and-graph-nav.md T3). No ImGui, no node-editor, no I/O.
//
// The graph views — topo (process tree), tree (call tree) and the hotedges
// frozen snapshot — gain a pan/zoom canvas with selection and "fit graph"
// (imgui-node-editor). The library imposes NO layout: node-editor is fed the
// app's own deterministic positions every frame, and its settings file is
// disabled, so it can neither persist nor invent a position. **Force-directed
// layout stays banned** (docs 04/08) — the fidelity guardrail is that the layout
// lives HERE, in a pure, tested builder, not in the drawing library.
//
// This TU is the pure half: it turns an already-built view model into a
// GraphLayout (nodes at fixed coordinates, edges, and the deep link a click
// routes through), and it culls off-viewport nodes for large graphs. The draw
// half (observer_draw.cpp) only feeds these positions to ed::SetNodePosition and
// routes clicks through 04's dt_nav_go — it decides nothing.
#ifndef ASMDESK_VIEWS_GRAPH_NAV_H
#define ASMDESK_VIEWS_GRAPH_NAV_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nav.h"
#include "views/hotedges.h"
#include "views/topo.h"
#include "views/tree.h"

namespace asmdesk {

// The fidelity guardrail, stated as a constant the draw half MUST hand to
// ed::Config::SettingsFile. nullptr disables node-editor's own settings file, so
// it never persists a dragged position or invents one on load — the layout is
// always the app's, every frame. Tested directly (a non-null value here would be
// a silent force-of-habit drift back toward library-owned positions).
inline constexpr const char *kGraphSettingsFile = nullptr;

// Layout geometry, in node-editor canvas units. Public so the tests can assert a
// node's coordinate IS `column * kGraphColW` (the app's layout) rather than
// whatever the library would have chosen.
inline constexpr float kGraphColW = 220.0f;  // one layer / grid column
inline constexpr float kGraphRowH = 90.0f;   // one row within a layer
inline constexpr float kGraphNodeW = 180.0f; // node extent, for culling
inline constexpr float kGraphNodeH = 60.0f;

// One node at a fixed, app-decided position. `id` is stable and non-zero (0 is
// node-editor's Invalid), `link` is where a click on it navigates (through 04's
// router); `has_link` is false for a node with no meaningful destination.
struct GraphNode {
    uint64_t id = 0;
    float x = 0.0f, y = 0.0f;
    std::string label;
    dt_link link;
    bool has_link = false;
};

// A directed edge between two node ids (parent -> child, caller -> callee,
// from-addr -> to-addr). No weight is drawn as anything but a line — the
// hotedges heat lives in the ImPlot matrix (T1), never as node-editor geometry.
struct GraphEdge {
    uint64_t from_id = 0;
    uint64_t to_id = 0;
};

struct GraphLayout {
    std::vector<GraphNode> nodes;
    std::vector<GraphEdge> edges;
};

// The visible canvas rectangle, in the same units as the node coordinates.
struct GraphViewport {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

// --- deterministic, layered builders (never force-directed) -----------------
// topo: one node per process, laid out in layers by ancestry depth (a root is a
// process whose parent is not in the snapshot), ordered by pid within a layer;
// edges are parent -> child. Each node's link is the syscall drill-in (04's
// router), exactly as the card list's "open this process" button.
GraphLayout graph_from_topo(const TopoView &v, const std::string &rec_id);

// tree: one node per emitted call, x by the engine's EFFECTIVE depth (never
// recomputed — under a focus filter it is re-based, doc 08), y by emission
// order; the parent edge is the nearest preceding row one level shallower. A
// node with a resolved address links to that address in the region view.
GraphLayout graph_from_tree(const TreeView &v, const std::string &rec_id);

// hotedges: the FROZEN snapshot as a graph — distinct branch endpoints in rank
// order on a stable grid, edges from-addr -> to-addr. A node links to its
// address in the region view. No parent/child/total is synthesized (this is a
// survey of edges, doc 08 T4).
GraphLayout graph_from_hotedges(const HotEdgeView &v,
                                const std::string &rec_id);

// Visible-region culling (doc 11 perf note; a 10k-routine trace must not emit
// 10k off-screen nodes). Returns the indices of nodes whose box intersects the
// viewport, preserving input order. A degenerate (non-positive) viewport culls
// nothing — the first frame, before the view is established, shows everything so
// "fit graph" has a graph to fit.
std::vector<std::size_t> graph_visible(const GraphLayout &g,
                                       const GraphViewport &vp);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_GRAPH_NAV_H
