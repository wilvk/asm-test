// overview.cpp — the pure overview/minimap projections (see overview.h).
#include "views/overview.h"

#include <algorithm>
#include <cmath>

namespace asmdesk {

OverviewStrip overview_from_timeline(const dt_timeline &t, double lo, double hi) {
    OverviewStrip o;
    o.from_timeline = true;
    o.lo = lo;
    o.hi = hi;
    uint32_t maxstep = 0;
    bool any = false;
    // One bucket per row the timeline actually holds. No bucket is invented for a
    // step index between two real rows: a sparse trace stays sparse (D7). A row
    // whose df_step event was dropped is UNKNOWN, not zero-activity — but it is a
    // real step index, so it keeps its bucket with weight 0 rather than vanishing.
    for (const dt_timeline_row &r : t.rows) {
        uint32_t w = r.missing ? 0u : static_cast<uint32_t>(r.n_in + r.n_out);
        o.buckets.push_back({r.step, w});
        maxstep = std::max(maxstep, r.step);
        any = true;
    }
    o.nsteps = any ? maxstep + 1 : 0;
    return o;
}

OverviewStrip overview_from_fabric(const loom_fabric_t &f, double lo, double hi) {
    OverviewStrip o;
    o.from_timeline = false;
    o.nsteps = f.steps;
    o.lo = lo;
    o.hi = hi;
    if (f.steps == 0)
        return o;
    // Live-span count per step, from the fabric's OWN spans — a difference array
    // so this is O(spans + steps), and it invents no lane or step the weave did
    // not already hold. A span alive at trace end (t_end == kLoomAlive) counts
    // through the last recorded step, never past it.
    std::vector<int> diff(static_cast<size_t>(f.steps) + 1, 0);
    for (const loom_span_t &sp : f.spans) {
        if (sp.t_write >= f.steps)
            continue;
        uint32_t end =
            sp.t_end == kLoomAlive ? f.steps : std::min(sp.t_end, f.steps);
        if (end <= sp.t_write)
            end = sp.t_write + 1 <= f.steps ? sp.t_write + 1 : f.steps;
        diff[sp.t_write] += 1;
        if (end <= f.steps)
            diff[end] -= 1;
    }
    int running = 0;
    for (uint32_t st = 0; st < f.steps; st++) {
        running += diff[st];
        o.buckets.push_back({st, static_cast<uint32_t>(running < 0 ? 0 : running)});
    }
    return o;
}

uint32_t overview_click_step(const OverviewStrip &o, double frac) {
    if (frac < 0.0)
        frac = 0.0;
    if (frac > 1.0)
        frac = 1.0;
    if (o.nsteps == 0)
        return 0;
    long long st = std::llround(frac * static_cast<double>(o.nsteps));
    uint32_t maxstep = o.nsteps - 1;
    if (st < 0)
        st = 0;
    if (static_cast<uint64_t>(st) > maxstep)
        st = maxstep;
    return static_cast<uint32_t>(st);
}

} // namespace asmdesk
