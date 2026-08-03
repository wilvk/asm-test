// types.h — the shared data model for the 3D spacetime overview
// (docs/internal/archive/gui/10-spacetime-3d-overview.md, "Data model (shared by all
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

// 61 T2: one region's rectangle on the cell grid, half-open in CELL
// coordinates over the n = 1<<order plane, in the SAME index order as
// Projection::regions so a caller joins them by index with no second key. Cell
// coordinates rather than floats because the tiling guarantee is about cells:
// the rects tile the grid exactly — every cell belongs to exactly one rect,
// none overlap — and that is checkable in integers with no epsilon.
struct AtlasRect {
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// 61 T2: the binary space partition `rects` was cut from, so a cell resolves to
// its region in O(log regions) instead of a scan. This is NOT premature:
// unproject() runs ONCE PER CELL over the WHOLE plane in build_terrain (one
// O(cells) sweep), which at order 12 is 16.7 M calls, so a linear walk of
// `rects` would make terrain construction O(cells x regions).
struct AtlasNode {
    uint32_t cut = 0; // the cell boundary this split fell on
    uint8_t axis = 0; // 0 = cut in x (vertical), 1 = cut in y (horizontal)
    // Child references. A NON-NEGATIVE value indexes `nodes`; a NEGATIVE one
    // is a leaf holding the region index, encoded as -(index + 1) — so a leaf
    // costs no node and the tree has exactly regions.size()-1 entries.
    int32_t lo = 0, hi = 0;
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

    // 54 T1: how the `observed data` regions (space/projection.h's
    // observed_data_spans) were derived — span count, source address count, the
    // gap threshold, and whether the cap merged any. Empty when the recording
    // carried no observed-touch addresses (nothing to explain). The HUD surfaces
    // it exactly like TerrainModel::mem_note.
    std::string data_span_note;

    // 61 T9: set on a weave whose layout MOVED relative to the previous one —
    // a growing live capture gaining a region, mostly. Empty on a first weave
    // and on any weave that re-laid the floor identically, so a NON-empty note
    // always means the reader's mental map just changed under them. The HUD
    // surfaces it exactly like data_span_note above.
    std::string layout_note;

    // 61 T2: which address->cell mapping this projection uses. Hilbert is the
    // historical space-filling walk: it spends BOTH plane axes on address and
    // leaves its unused cells as one connected padding blob at the tail of the
    // curve, owned by no region. Atlas gives each region a RECTANGLE of the
    // SAME n = 1<<order grid, area proportional to Region::len, serpentine
    // within — so every cell belongs to a named region and a region is a
    // labellable rectangle rather than a blobby snake. Neither layout decodes
    // more than `total` cells (there are only `total` bytes); what changes is
    // whether the slack has an owner. `order` means the same thing under both.
    enum class Layout { Hilbert, Atlas };
    Layout layout = Layout::Hilbert;
    // The per-region rectangles, index-parallel to `regions`, and the split
    // tree they were cut from. Both empty under Hilbert.
    std::vector<AtlasRect> rects;
    std::vector<AtlasNode> nodes;

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
