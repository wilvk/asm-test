// projection.h — address space -> unit plane, via a Hilbert curve over a
// compacted address domain (docs/internal/archive/gui/10-spacetime-3d-overview.md T1).
//
// Pure and engine-free: this TU includes no GL, no ImGui and no engine header —
// the standard library only — which is what lets it compile into BOTH desktop
// binaries and the null test harness (D4). A recording's regions are placed on
// the terrain plane with zero engine dependencies, the same closure argument
// analysis/slice.h makes for replay slicing.
#ifndef ASMDESK_SPACE_PROJECTION_H
#define ASMDESK_SPACE_PROJECTION_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "space/types.h"

namespace asmdesk::space {

// Build a Projection from a raw region set: sort by base, pack each into a
// contiguous slot of a compacted domain [0, sum(len)) (so memory neighbours stay
// plane neighbours), and size the Hilbert plane to hold it — order =
// ceil(log4(sum(len))) clamped to [6, 12], a 64x64 .. 4096x4096 plane. `regions`
// is taken by value and moved into the result after sorting.
//
// Precondition: regions do not overlap (a /proc/maps snapshot and codeimage
// bases satisfy this). If two do overlap, project() resolves an address to the
// region with the greatest base <= addr; no region is dropped or merged.
Projection build_projection(std::vector<Region> regions);

// 61 T2: recompute the layout for p.layout. For Atlas this fills p.rects with
// an order-preserving binary-split treemap whose cell budgets are proportional
// to Region::len (any other weighting would be a fabricated emphasis — D7) and
// which tiles the n = 1<<order grid exactly, plus p.nodes, the split tree
// unproject() descends. Clears both for Hilbert. Idempotent.
//
// Falls BACK to Hilbert, silently in code but visibly in p.layout, in two
// cases: the plane has fewer cells than there are regions, and the split
// admits no realisable tiling. Both are impossible for a real map — `order` is
// sized from the domain's byte count, so a plane crowded by regions needs a
// synthetic domain — and the second has never been observed at all. A caller
// that cares reads p.layout back; NEVER assume Atlas stayed Atlas just because
// you set it.
void rebuild_layout(Projection &p);

// The rel->abs anchor (36 T1). Answers one question: "what absolute span is a
// routine-relative `df_step.off` (or a rel `trace` offset) relative to, in this
// recording?" — or refuses with the reason. It derives the answer from a fact the
// recording *states*: a `codeimage` event carries the scoped span's real absolute
// `base`+`len` (asmspy serve_codeimage_emit), while `df_step.off` is that span's
// offset by construction (`pc - base_ip`, dataflow_ptrace.c) and carries **no**
// `basis` and no region tag on the wire
// ([asmtest_trace.h:41-42](include/asmtest_trace.h), the df_step body writer at
// [asmtrace_ndjson.c:276-277](cli/asmtrace_ndjson.c) emits only `step`/`off`/
// `disasm?`/`ops`). So exactly one code span makes `base + off` the *true*
// address — a derivation, not a guess; zero or ≥2 spans is unrecoverable from a
// bare offset and is refused with a stated reason (37 states the region on the
// wire so the multi-span case resolves instead of refusing — this stays the
// permanent fallback for pre-37 recordings and for rel `trace`).
struct Anchor {
    bool ok = false; // true iff exactly one codeimage code span pins it
    uint64_t base = 0,
             len = 0;   // the resolved span (absolute base, byte length)
    std::string reason; // why it could not anchor (empty when ok)

    // Place a region-relative offset onto the absolute plane. False — so the
    // caller *counts* the miss instead of silently dropping it — when unanchored
    // or when `off >= len` (the common SERVE_CI_MAX_BYTES=4096 clamp case, where
    // a genuinely in-routine offset lands past the captured span).
    bool place(uint64_t off, uint64_t *abs) const;
};
// Resolve the anchor from a recording's region set: exactly one Region::Code span
// anchors; zero or two-or-more refuse with a reason (the two-span reason names
// each hex base). Non-code regions never make an anchor ambiguous.
Anchor resolve_anchor(const std::vector<Region> &regions);

// 54 T1: the gap that closes one observed-data span and opens the next — one
// page (4096 bytes), the granularity the OS actually allocates in, and
// therefore the smallest gap that is not evidence of a distinct object.
inline constexpr uint64_t kObservedSpanGap = 4096;
// 54 T1: the cap on emitted observed-data spans. When a scatter of touches
// would produce more, the nearest neighbours are merged (never dropped) and
// data_span_note says so.
inline constexpr size_t kObservedSpanCap = 4096;
// 54 T1 / 58 T1: the Region::label every observed-data span carries. Named
// rather than repeated as a literal, because the HUD now COUNTS these spans to
// state the data rung's provenance (58 T1) — a second spelling of the string
// would silently report zero spans forever.
inline constexpr const char *kObservedDataLabel = "observed data";

// Cluster the addresses a recording OBSERVED touching into compacted spans, so
// an access that no codeimage region maps still places on the plane. Every span
// is Region::Kind::Unknown and labelled "observed data" — the recording states
// that these bytes were touched, and NOTHING about what they are (not an
// allocation extent: the real object may start before and end after).
//
// Address sources, in this order: raw `mem` events' `ea`, and
// `DataflowStream::recs`' `ValRec.addr` where `space == "abs"` (an `"off"`
// record is region-relative and must go through the anchor first, so it is
// skipped here, counted in the note, never placed raw).
//
// Spans are opened/closed by `kObservedSpanGap`, rounded out to page
// boundaries, then clipped against `existing` so an observed address inside a
// known region never creates a shadow span (overlap is a precondition
// violation for build_projection). If clipping still leaves more than
// `kObservedSpanCap` spans, the nearest neighbours are merged until it fits —
// never silently dropped. `note`, when non-null, is always assigned exactly
// once: a human-readable summary (span/address counts, the threshold, whether
// the cap merged), or the empty string when the recording carried no observed
// addresses at all.
std::vector<Region> observed_data_spans(const Recording &rec,
                                        const std::vector<Region> &existing,
                                        std::string *note = nullptr);

// Region-kind colour + legend name, carried through for the HUD legend (T1 step
// 4). Pure data: the renderer (T4) turns it into GL; this returns only floats
// and a static string, so the legend is decided in an engine-free, testable TU.
struct RegionStyle {
    float r, g, b;
    const char *name;
};
RegionStyle region_style(Region::Kind kind);

// 61 T8: one region's in-place label on the atlas floor. `u`/`v` are the
// rect's GEOMETRIC centre (not a cell centre — the anchor names a rectangle,
// not a cell), `cells` is its area for a caller's own LOD, `region` indexes
// Projection::regions, and `text` is Region::label or, when that is empty, the
// kind name — the SAME fallback the HUD's side-panel legend already applies
// (scene3d/hud.cpp), never a second naming convention.
struct AtlasLabel {
    float u = 0, v = 0;
    uint32_t cells = 0;
    size_t region = 0;
    std::string text;
};

// 61 T8: the labels for an ATLAS projection's rectangles — Component 1's
// stated payoff ("region-major, 100 % packed, LABELLED IN PLACE ... this, not
// locality, is the real win"). Empty under Hilbert, and that refusal is the
// honest answer rather than a missing feature: a Hilbert region is a connected
// snake through the plane with no rectangle and so no anchor, and inventing one
// would be fabricated structure (D7). It is also what lets a caller invoke this
// unconditionally, with no layout test at the call site.
//
// PARTIAL BY DESIGN. A rect smaller than `min_cells` is skipped, because below
// roughly 8x8 the labels overlap into a pile that reads worse than the empty
// floor did. That is a LEGIBILITY threshold, not a fidelity one — every region,
// labelled or not, is still listed in the HUD's region legend, which is what
// keeps a partially-labelled floor from implying the unlabelled rects are
// unnamed. Callers must not present these as the complete set.
std::vector<AtlasLabel> atlas_labels(const Projection &proj,
                                     uint32_t min_cells = 64);

// 51 T2 (scene-focus-and-scale.md): the plane cells one region occupies —
// row-major indices `y * n + x` over the n = 2^order plane, ascending, no
// duplicates. THE one rule for "which cells are this region's".
//
// 61 T2: BOTH layouts are live here, and they answer slightly different
// questions. Under `Atlas` the set is the region's RECTANGLE — every cell the
// region OWNS, which is a SUPERSET of the cells its addresses can reach, since
// a rect granted more cells than the region has bytes has a rounding tail that
// is owned but does not decode. Ownership is what zoning and labelling want,
// and it keeps this function's containment and disjointness contracts true
// under both layouts. Under `Hilbert`, as before: it walks the
// region's own compacted-domain range `[domain_off[i], domain_off[i+1])`
// through the SAME Hilbert index -> cell mapping project() itself uses, so it
// contains every cell project() can place an address of this region into, by
// construction — never a bounding box (a Hilbert footprint for one contiguous
// byte range is a real, non-rectangular SET of cells, and a rectangle assumed
// from base/len would necessarily swallow a neighbour's cells; the same rule
// 48's scene_goto_region states for framing).
//
// Cost is O(cells this region owns), never O(len) and never O(plane): the
// domain range maps to a CONTIGUOUS run of Hilbert indices, so the walk is one
// d2xy per owned cell. Empty for an out-of-range index or a zero-length region.
//
// Boundary note (exact, not a caveat): when the compacted domain is larger
// than the plane, build_projection shifts the domain down onto it, so several
// bytes — possibly the tail of one region and the head of the next — share one
// cell. Two adjacent regions can therefore both legitimately own that single
// shared cell. That is a fact about the plane's resolution, not an error, and
// it is at most one cell per adjacent pair; with a domain that fits the plane
// (every fixture and every ordinary recording) the sets are strictly disjoint.
std::vector<uint32_t> region_cells(const Projection &proj, size_t region_index);

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_PROJECTION_H
