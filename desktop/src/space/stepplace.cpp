// stepplace.cpp — the shared step->place resolver of stepplace.h. Standard
// library + the space/ and doc/ models only; no GL, no ImGui, no engine (D4).
#include "space/stepplace.h"

namespace asmdesk::space {

namespace {

// The region holding `addr`, or null. `Projection::regions` is sorted by base
// and non-overlapping (build_projection's own precondition), so this is the
// same "greatest base <= addr, then bounds-check the length" lookup
// Projection::project performs internally — repeated here rather than exposed
// on Projection because every other space/ caller of this two-line derivation
// (pick.cpp's region_containing, locate.cpp's cell_of) keeps its own copy too.
const Region *region_containing(const Projection &proj, uint64_t addr) {
    const std::vector<Region> &rs = proj.regions;
    size_t lo = 0, hi = rs.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (rs[mid].base <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return nullptr;
    const Region &r = rs[lo - 1];
    if (addr - r.base >= r.len)
        return nullptr;
    return &r;
}

} // namespace

StepPlace place_address(const Projection &proj, uint64_t addr) {
    StepPlace out;
    float u = 0.0f, v = 0.0f;
    if (!proj.project(addr, &u, &v)) {
        out.why = "the address is not mapped by any region in this recording";
        return out;
    }
    // The SAME cell-centre-recovering arithmetic locate.cpp's cell_of,
    // pick.cpp's classify_cell and unproject() itself use: project() always
    // returns (x+0.5)/n, so truncating u*n recovers x exactly.
    const uint32_t n = uint32_t(1) << proj.order;
    uint32_t x = static_cast<uint32_t>(u * n);
    uint32_t y = static_cast<uint32_t>(v * n);
    if (x >= n)
        x = n - 1;
    if (y >= n)
        y = n - 1;
    out.placed = true;
    out.addr = addr;
    out.u = u;
    out.v = v;
    out.cell = y * n + x;
    out.region = region_containing(proj, addr);
    return out;
}

StepPlace StepPlacer::at(uint32_t step) {
    StepPlace out;
    uint64_t addr = 0;
    std::string how, fail_reason;
    // Rungs (a) and (b) — and every refusal reason — come straight from
    // locate.h's StepAddrResolver (50 T1). Nothing is re-derived here.
    if (!resolver_.resolve(step, &addr, &how, &fail_reason)) {
        out.why = fail_reason;
        if (step < missed_.size()) {
            if (!missed_[step]) {
                missed_[step] = 1;
                distinct_missed_++;
            }
        } else {
            out_of_range_++;
        }
        return out;
    }

    out = place_address(proj_, addr);
    if (!out.placed) {
        // Rung (c) reached by the PLANE rather than the base: the address
        // resolved, but no region maps it. Counted exactly like a base
        // failure — the layer draws nothing either way, so the chip must
        // include it.
        if (step < missed_.size() && !missed_[step]) {
            missed_[step] = 1;
            distinct_missed_++;
        }
    }
    return out;
}

const std::string &StepPlacer::note() const {
    const uint64_t n = unplaced();
    if (note_for_ != n) {
        note_for_ = n;
        note_ = n == 0 ? std::string()
                       : std::to_string(n) + " step(s) off-plane";
    }
    return note_;
}

} // namespace asmdesk::space
