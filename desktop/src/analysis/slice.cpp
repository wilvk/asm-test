// slice.cpp — the closure rule of slice.h. Standard library only (D4).
#include "analysis/slice.h"

#include <algorithm>

namespace asmdesk {

bool dt_slice::contains(uint32_t step) const {
    return std::binary_search(steps.begin(), steps.end(), step);
}

namespace {

// BFS from `origin` over an adjacency list built once. `forward` selects which
// endpoint of an edge is the source: producer->consumer (forward) or
// consumer->producer (backward).
//
// Both endpoints are bounds-checked here, which is equivalent to the C slicer
// (src/dataflow.c:689-694) even though it checks only the destination: there
// the source endpoint is matched against a DEQUEUED node, which is in range by
// construction, so an edge whose source is >= nsteps can never fire either way.
dt_slice closure(const std::vector<dt_edge> &edges, uint32_t nsteps,
                 uint32_t origin, bool forward) {
    dt_slice s;
    if (origin >= nsteps)
        return s; // origin outside the trace: the EMPTY slice, not {origin}

    // adjacency: counting sort into a CSR-style pair of vectors, so the build
    // is O(V+E) and the walk touches only the edges leaving each node.
    std::vector<uint32_t> head(static_cast<size_t>(nsteps) + 1, 0);
    std::vector<uint32_t> dst;
    dst.reserve(edges.size());
    for (const dt_edge &e : edges) {
        uint32_t from = forward ? e.from_step : e.to_step;
        uint32_t to = forward ? e.to_step : e.from_step;
        if (from < nsteps && to < nsteps)
            head[from + 1]++;
    }
    for (uint32_t i = 0; i < nsteps; i++)
        head[i + 1] += head[i];
    dst.resize(head[nsteps]);
    std::vector<uint32_t> fill(head.begin(), head.end() - 1);
    for (const dt_edge &e : edges) {
        uint32_t from = forward ? e.from_step : e.to_step;
        uint32_t to = forward ? e.to_step : e.from_step;
        if (from < nsteps && to < nsteps)
            dst[fill[from]++] = to;
    }

    std::vector<char> seen(nsteps, 0);
    std::vector<uint32_t> queue;
    queue.reserve(nsteps);
    seen[origin] = 1;
    queue.push_back(origin);
    for (size_t qh = 0; qh < queue.size(); qh++) {
        uint32_t u = queue[qh];
        for (uint32_t i = head[u]; i < head[u + 1]; i++) {
            uint32_t v = dst[i];
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

} // namespace asmdesk
