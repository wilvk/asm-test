// types.h — the shared data model for the 3D spacetime overview
// (docs/internal/gui/10-spacetime-3d-overview.md, "Data model (shared by all
// tasks)"). Pure: no GL, no ImGui, no engine includes, so every layer that
// builds on it (the projection T1, the terrain T2, the trajectory T3) links
// into BOTH desktop binaries and the null test harness (D4). Only Projection
// carries logic in T1 (implemented in projection.cpp); the other structs are
// plain data the later tasks fill in.
#ifndef ASMDESK_SPACE_TYPES_H
#define ASMDESK_SPACE_TYPES_H

#include <cstdint>
#include <string>
#include <vector>

namespace asmdesk::space {

// A contiguous region of the virtual address space, from a codeimage event
// (code) or a /proc/maps snapshot (data/stack/heap/mmap).
struct Region {
    uint64_t base = 0, len = 0;
    enum Kind { Code, Stack, Heap, Data, Mmap, Unknown } kind = Unknown;
    std::string label;    // "libc .text", "[stack:tid7]", "jit#3", ...
    uint64_t version = 0; // codeimage version (JIT churn); 0 for static
};

// The projection: address space -> unit plane [0,1]^2 via a Hilbert curve over a
// compacted address domain (regions are packed to kill the sparse gaps that make
// a raw address axis useless — see T1). Build one with build_projection()
// (projection.h); the two methods below are its read side.
struct Projection {
    std::vector<Region> regions; // sorted by base, non-overlapping, compacted
    uint32_t order = 0; // Hilbert order: plane is 2^order x 2^order cells

    // domain_off[i] is the start of regions[i] in the compacted domain
    // [0, sum(len)); the vector has regions.size()+1 entries, the last being the
    // total compacted length. This is the copy-safe realization of T1's
    // (domain_off, Region*) table — an index into `regions` stands in for the
    // pointer, so a copied Projection never dangles — and it gives the O(log n)
    // address<->domain lookup both project() and unproject() need.
    std::vector<uint64_t> domain_off;

    // maps an absolute address to a plane cell (u,v) in [0,1]^2; false if the
    // address is mapped by no region.
    bool project(uint64_t addr, float *u, float *v) const;
    // inverse, for picking: a plane cell -> the address (region + offset) it
    // holds; false if the cell is padding beyond the compacted domain.
    bool unproject(float u, float v, uint64_t *addr, const Region **r) const;
};

// One trajectory sample: the PC (or a data access) at a logical time.
struct TrajPoint {
    uint64_t t = 0;    // trace step index (time = vertical axis)
    uint64_t addr = 0; // an address in the recording's address space: absolute
                       // for a measured abs vertex; the DERIVED absolute address
                       // base+off for a rel offset the anchor placed (36 T2 —
                       // set.basis still reads "rel"); the raw wire offset for a
                       // rel offset the anchor could NOT place (with placed=false)
    enum { Exact, Statistical } fidelity = Exact; // trace/PT vs ibs survey
    bool is_access = false; // false = PC vertex; true = a data-access mark
    int32_t tid = -1;       // -1 = single-trajectory replay; else per-thread
    // 36 T5: true for a measured absolute vertex and for a rel offset the anchor
    // placed; FALSE for one it could not place (off past the codeimage span,
    // where `addr` is still the raw wire offset). Defaulting true leaves every
    // existing producer and hand-built test point unchanged; the anchoring pass
    // MEASURES it. Convergence skips a !placed vertex so a raw offset — which
    // could otherwise alias a real cell — is never bucketed.
    bool placed = true;
};

// Height field over the projection's cells.
struct Terrain {
    uint32_t w = 0, h = 0;     // = 2^order each
    std::vector<float> height; // [w*h], access density (log-scaled) at a slice
    std::vector<uint32_t> flags; // per-cell: TORN (truncated), STAT (sampled)
};

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_TYPES_H
