// dataribbon.cpp — the implementation of dataribbon.h. Standard library + the
// document model + the projection (D4): no GL, no ImGui, no engine.
#include "space/dataribbon.h"

#include <algorithm>

#include "doc/streams.h"

namespace asmdesk::space {

namespace {

// Resolve a vertex's plane placement, exactly as terrain.cpp's cell_of does
// (the same (x+0.5)/n rounding), plus the owning region index the leap rule
// needs. Leaves `placed` false when no region maps the address.
void place(RibbonVertex &v, const Projection &proj) {
    float u = 0, v_ = 0;
    if (!proj.project(v.addr, &u, &v_))
        return;
    v.placed = true;
    v.u = u;
    v.v = v_;
    const uint32_t n = uint32_t{1} << proj.order;
    uint32_t x = static_cast<uint32_t>(u * n), y = static_cast<uint32_t>(v_ * n);
    if (x >= n)
        x = n - 1;
    if (y >= n)
        y = n - 1;
    v.cell = y * n + x;
    // The owning region: the last region with base <= addr that contains it —
    // the same resolution Projection::project performs internally.
    for (size_t i = proj.regions.size(); i-- > 0;) {
        const Region &r = proj.regions[i];
        if (r.base <= v.addr && v.addr - r.base < r.len) {
            v.region = i;
            break;
        }
    }
}

} // namespace

const char *ribbon_source_label(RibbonSource s) {
    switch (s) {
    case RibbonSource::Mem:
        return "source: the `mem` per-access address stream (exact)";
    case RibbonSource::DataflowAbs:
        // Named as a DIFFERENT population, not as a stand-in.
        return "source: dataflow operand records (abs space) — `mem` was "
               "absent. NOT the same population: the emulator's hardware hooks "
               "see implicit stack traffic a live `mem` enumeration does not";
    case RibbonSource::None:
        return "source: none — this recording carries neither a `mem` stream "
               "nor abs-space dataflow operands";
    }
    return "";
}

DataRibbon build_data_ribbon(const Recording &r, const Projection &proj) {
    DataRibbon out;
    out.capped = r.truncated();
    out.drops_present = r.dropped();

    Streams s = decode_streams(r);
    // The COVERED step domain: `mem`.step indexes into the trace (or df_step)
    // stream, so a step at-or-past the number of steps the capture actually
    // holds is outside it. This is what makes "a gap in the step coverage" a
    // real, checkable condition rather than a vibe.
    out.covered_steps = !s.trace.insns.empty() ? s.trace.insns.size()
                                               : s.df.insn_off.size();

    // --- vertices, in step order --------------------------------------------
    if (auto it = r.by_kind.find("mem");
        it != r.by_kind.end() && !it->second.empty()) {
        out.source = RibbonSource::Mem;
        out.verts.reserve(it->second.size());
        for (const Event &e : it->second) {
            RibbonVertex v;
            v.step = e.body.value("step", uint64_t{0});
            v.addr = e.body.value("ea", uint64_t{0});
            v.size = static_cast<uint32_t>(e.body.value("size", uint64_t{0}));
            const std::string rw = e.body.value("rw", std::string());
            v.dir = rw == "w"   ? RibbonVertex::Write
                    : rw == "r" ? RibbonVertex::Read
                                : RibbonVertex::Unknown;
            place(v, proj);
            out.verts.push_back(v);
        }
    } else {
        // Fallback: abs-space dataflow operands. An "off" record is
        // region-RELATIVE and must go through the anchor first, so it is never
        // placed raw here — the same rule observed_data_spans (54 T1) holds.
        for (const ValRec &vr : s.df.recs) {
            if (vr.space != "abs")
                continue;
            RibbonVertex v;
            v.step = vr.step;
            v.addr = vr.addr;
            v.size = vr.size;
            v.dir = vr.write ? RibbonVertex::Write : RibbonVertex::Read;
            place(v, proj);
            out.verts.push_back(v);
        }
        if (!out.verts.empty())
            out.source = RibbonSource::DataflowAbs;
    }

    std::stable_sort(out.verts.begin(), out.verts.end(),
                     [](const RibbonVertex &a, const RibbonVertex &b) {
                         return a.step < b.step;
                     });
    for (const RibbonVertex &v : out.verts)
        if (!v.placed)
            out.off_plane++;

    // --- segments, between CONSECUTIVE recorded accesses only ----------------
    // A segment asserts "the next recorded access after A was B". It is emitted
    // only where that claim is honest:
    //
    //   1. BOTH endpoints must be placed. An access we know happened but cannot
    //      locate must not be joined across — the resulting line would assert a
    //      locality step from A straight to C that the program never made.
    //   2. BOTH steps must lie inside the covered step domain. A pair that
    //      straddles the truncation boundary is a GAP: the capture stopped, and
    //      what happened over that interval is unrecorded, not "nothing".
    //
    // Everything else is a gap, counted so it is never silent. An INTERPOLATED
    // segment across a gap would draw an access that was never recorded, which
    // is the single thing this layer must not do.
    for (size_t i = 0; i + 1 < out.verts.size(); ++i) {
        const RibbonVertex &a = out.verts[i], &b = out.verts[i + 1];
        bool joinable = a.placed && b.placed;
        if (joinable && out.covered_steps > 0 &&
            (a.step >= out.covered_steps || b.step >= out.covered_steps))
            joinable = false; // outside what the capture actually holds
        if (!joinable) {
            out.gaps++;
            continue;
        }
        RibbonSegment sg;
        sg.a = i;
        sg.b = i + 1;
        sg.cross_region = a.region != b.region;
        if (sg.cross_region)
            out.cross_region++;
        out.segs.push_back(sg);
    }
    return out;
}

std::string data_ribbon_note(const DataRibbon &ribbon) {
    std::string s = "access order: one vertex per recorded access at its "
                    "effective address, joined in step order; Y is trace time, "
                    "width is the access size. This is the ORDER data was "
                    "touched in — NOT the PC->data access spurs, which show "
                    "which instruction touched which datum. ";
    s += ribbon_source_label(ribbon.source);
    s += ". A missing segment is a GAP (an unplaceable access, or a step "
         "outside the covered domain) — never an interpolated join";
    if (ribbon.cross_region > 0)
        s += "; " + std::to_string(ribbon.cross_region) +
             " segment(s) leap between distinct observed-data spans — GENUINE "
             "non-locality, not an artefact of the address compaction";
    if (ribbon.off_plane > 0)
        s += "; " + std::to_string(ribbon.off_plane) +
             " access(es) could not be placed and are counted, not drawn";
    if (ribbon.capped)
        s += "; TRUNCATED: the ribbon is CAPPED — nothing is asserted after "
             "the last vertex";
    if (ribbon.drops_present)
        s += "; DROPPED EVENTS: an access may have been lost between any two "
             "surviving ones, so \"consecutive\" is weaker here than elsewhere";
    return s;
}

} // namespace asmdesk::space
