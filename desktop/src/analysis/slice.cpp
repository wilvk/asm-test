// slice.cpp — the closure rule of slice.h. Standard library only (D4).
#include "analysis/slice.h"

#include <algorithm>

namespace asmdesk {

bool dt_slice::contains(uint32_t step) const {
    return std::binary_search(steps.begin(), steps.end(), step);
}

namespace {

// counting-sort adjacency, CSR-style (`head`/`dst`), built once so a walk
// touches only the edges leaving each node — O(V+E). `forward` selects which
// endpoint of an edge is the source: producer->consumer (forward) or
// consumer->producer (backward). Shared by closure() and walk_depth() below.
//
// Both endpoints are bounds-checked here, which is equivalent to the C slicer
// (src/dataflow.c:689-694) even though it checks only the destination: there
// the source endpoint is matched against a DEQUEUED node, which is in range by
// construction, so an edge whose source is >= nsteps can never fire either way.
struct Adjacency {
    std::vector<uint32_t> head, dst;
};

Adjacency build_adjacency(const std::vector<dt_edge> &edges, uint32_t nsteps,
                          bool forward) {
    Adjacency a;
    a.head.assign(static_cast<size_t>(nsteps) + 1, 0);
    a.dst.reserve(edges.size());
    for (const dt_edge &e : edges) {
        uint32_t from = forward ? e.from_step : e.to_step;
        uint32_t to = forward ? e.to_step : e.from_step;
        if (from < nsteps && to < nsteps)
            a.head[from + 1]++;
    }
    for (uint32_t i = 0; i < nsteps; i++)
        a.head[i + 1] += a.head[i];
    a.dst.resize(a.head[nsteps]);
    std::vector<uint32_t> fill(a.head.begin(), a.head.end() - 1);
    for (const dt_edge &e : edges) {
        uint32_t from = forward ? e.from_step : e.to_step;
        uint32_t to = forward ? e.to_step : e.from_step;
        if (from < nsteps && to < nsteps)
            a.dst[fill[from]++] = to;
    }
    return a;
}

dt_slice closure(const std::vector<dt_edge> &edges, uint32_t nsteps,
                 uint32_t origin, bool forward) {
    dt_slice s;
    if (origin >= nsteps)
        return s; // origin outside the trace: the EMPTY slice, not {origin}

    Adjacency adj = build_adjacency(edges, nsteps, forward);
    std::vector<char> seen(nsteps, 0);
    std::vector<uint32_t> queue;
    queue.reserve(nsteps);
    seen[origin] = 1;
    queue.push_back(origin);
    for (size_t qh = 0; qh < queue.size(); qh++) {
        uint32_t u = queue[qh];
        for (uint32_t i = adj.head[u]; i < adj.head[u + 1]; i++) {
            uint32_t v = adj.dst[i];
            if (!seen[v]) {
                seen[v] = 1;
                queue.push_back(v);
            }
        }
    }

    // Ascending and de-duplicated by construction: walk the visited flags in
    // step order rather than sorting the discovery order.
    for (uint32_t k = 0; k < nsteps; k++)
        if (seen[k])
            s.steps.push_back(k);
    return s;
}

} // namespace

dt_slice dt_slice_forward(const std::vector<dt_edge> &edges, uint32_t nsteps,
                          uint32_t origin) {
    return closure(edges, nsteps, origin, true);
}

dt_slice dt_slice_backward(const std::vector<dt_edge> &edges, uint32_t nsteps,
                           uint32_t origin) {
    return closure(edges, nsteps, origin, false);
}

dt_walk dt_walk_depth(const std::vector<dt_edge> &edges, uint32_t nsteps,
                      uint32_t origin, bool forward, int32_t max_depth) {
    dt_walk w;
    if (origin >= nsteps)
        return w; // empty walk, matching dt_slice's out-of-range rule

    Adjacency adj = build_adjacency(edges, nsteps, forward);

    // -1 = unvisited; a real depth is always >= 0 during the BFS (the sign
    // flip for the backward direction happens only when `w.depth` is filled
    // in below, so it never collides with the unvisited sentinel here).
    std::vector<int32_t> hop(nsteps, -1);
    std::vector<uint32_t> queue;
    queue.reserve(nsteps);
    hop[origin] = 0;
    queue.push_back(origin);
    for (size_t qh = 0; qh < queue.size(); qh++) {
        uint32_t u = queue[qh];
        int32_t du = hop[u];
        if (du >= max_depth) {
            // Expanding u would put its unvisited neighbours at du+1, past
            // the cap: that IS the "bounded" condition, but only if such a
            // neighbour genuinely exists — a node at exactly max_depth with
            // no further edges reached the true fixpoint, not the cap.
            for (uint32_t i = adj.head[u]; i < adj.head[u + 1]; i++) {
                if (hop[adj.dst[i]] < 0) {
                    w.bounded = true;
                    break;
                }
            }
            continue;
        }
        for (uint32_t i = adj.head[u]; i < adj.head[u + 1]; i++) {
            uint32_t v = adj.dst[i];
            if (hop[v] < 0) {
                hop[v] = du + 1;
                queue.push_back(v);
            }
        }
    }

    // Ascending and de-duplicated by construction, first-reach depth (BFS
    // visits each node exactly once, at its minimum hop count from origin).
    bool first = true;
    for (uint32_t k = 0; k < nsteps; k++) {
        if (hop[k] < 0)
            continue;
        int32_t d = forward ? hop[k] : -hop[k];
        w.steps.push_back(k);
        w.depth.push_back(d);
        if (first) {
            w.depth_min = w.depth_max = d;
            first = false;
        } else {
            w.depth_min = std::min(w.depth_min, d);
            w.depth_max = std::max(w.depth_max, d);
        }
    }
    return w;
}

} // namespace asmdesk
