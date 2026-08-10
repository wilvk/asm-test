// terrain.h — the density height field over time of the 3D spacetime overview
// (docs/internal/archive/gui/10-spacetime-3d-overview.md T2). Pure and engine-free: this
// TU includes no GL, no ImGui and no engine header — only the standard library,
// the document model (doc/), the trace canvas (views/canvas.h, whose per-offset
// heat is reused as the coarse height source, 04-T3) and the projection
// (space/projection.h, 10-T1). That is what lets the terrain compile into BOTH
// desktop binaries and the null test harness (D4).
//
// The terrain is built ONCE from a recording into a TerrainModel that precomputes
// a per-cell prefix structure; TerrainModel::slice(t) then produces the Terrain
// for the time slice [0, t] in O(touched cells) with no event re-scan, so
// scrubbing the playhead stays far under the 16 ms budget for a golden-sized
// recording (T2 step 3).
//
// Two fidelity rules ride here and are tested (test_terrain.cpp, D7):
//
//   - THE STATISTICAL LAYER IS SEPARATE. `survey`-derived residency is written
//     into a DISTINCT Terrain (`stat`) with every populated cell flagged STAT;
//     the exact terrain slice() never merges it. A sampled residency is not an
//     exact density and must never render as one (the T6 invariant).
//   - COARSE IS NEVER A SILENT ZERO. Absent the Wave-1 `mem` stream the data
//     cells stay flat, and `mem_note` carries a provenance chip that says so —
//     the flat plane is labelled coarse, not presented as measured emptiness.
//   - A DF HEIGHT IS NOT BLOCK COVERAGE (36 T3). When the coarse rung is fed by
//     the single-step residency stream (`df_step`, a live dataflow/auto capture
//     that carries no `trace`), `height_source` says "df_step" and `height_note`
//     labels it single-step residency, NOT block coverage — it fills no `blocks`,
//     marks nothing covered, and flags nothing TF_STAT (the stream is exact, not
//     sampled). A rel path with no resolvable codeimage span places no cells and
//     sets `anchor_error` — a flat plane that SAYS WHY, distinct from the
//     mixed-basis refusal `basis_error` carries.
#ifndef ASMDESK_SPACE_TERRAIN_H
#define ASMDESK_SPACE_TERRAIN_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "space/projection.h"

namespace asmdesk {
namespace space {

// Per-cell flag bits carried in Terrain::flags. The low three mirror the
// terrain.frag constants in 10-T4's shader (TORN=1, STAT=2, CHURN=4) so the CPU
// builder and the GPU renderer agree on the bit layout without a second table;
// READ/WRITE are the rich rung's read/write tint channel.
enum TerrainFlag : uint32_t {
    TF_TORN = 1u << 0,  // capture truncated/torn: this cell's height is a floor
    TF_STAT = 1u << 1,  // statistical (survey) residency — never exact
    TF_CHURN = 1u << 2, // a codeimage version changed within [0, t] over here
    TF_READ = 1u << 3,  // rich rung: a `mem` read tinted this data cell
    TF_WRITE = 1u << 4, // rich rung: a `mem` write tinted this data cell
    // T2 (44-faithful-city-phase-a): the fog-of-war pit — an IN-DOMAIN cell
    // that carries neither code/data content nor TF_STAT at this slice. Set
    // per-slice in TerrainModel::slice(), never in build_terrain() (a cell
    // untouched so far can still be hit later). Distinct from an off-domain
    // cell (never drawn as land at all) and from TF_TORN (a KNOWN lower
    // bound) — TF_UNKNOWN means "no content", not "a floor on real content".
    TF_UNKNOWN = 1u << 5,
    // T2 (56-fidelity-and-module-layers): the coverage-window mask — set only
    // when a HotEdgeSceneView states a window (views/hotedges.h's
    // apply_coverage_window), never inferred from an absent window. An
    // in-window cell with real height needs neither bit (it already renders as
    // a mound); these two grade the other two cases the confidence layer must
    // not conflate.
    TF_INWINDOW_EMPTY = 1u << 6, // in the stated window, credited nothing
                                 // (below-rate, not confidently cold)
    TF_OUTWINDOW = 1u << 7,      // outside the stated window (never looked)
};

// T1 (44-faithful-city-phase-a): the kind_by_cell sentinel for an off-domain /
// unowned cell (Projection::unproject fails there) — DISTINCT from
// Region::Kind::Unknown (space/types.h), which names a REAL region whose kind
// the recording did not classify. kind_by_cell never stores Region::Unknown's
// raw value (5) to mean "no region data at all"; it stores this instead, so
// "no owning region" and "an unclassified region" cannot be confused.
inline constexpr uint8_t kKindByCellNone = 255;

// T2 (58-memory-data-cell-family): the OBSERVED direction of a `mem` access,
// as carried by DataCell::cum_dir. Distinct from TF_READ/TF_WRITE, which are
// the terrain's tint channel and fold an unrecognised `rw` token into READ.
// Neither bit set means the direction was NOT recorded — which is a third
// state, never a synonym for "read" and never a synonym for zero.
enum DataDir : uint8_t {
    DD_READ = 1u << 0,
    DD_WRITE = 1u << 1,
};

// The precomputed, re-sliceable terrain. Built ONCE by build_terrain(); slice(t)
// is O(touched cells). Also carries the provenance a HUD must surface so a flat
// or refused plane is always labelled, never mistaken for a measured zero.
struct TerrainModel {
    Projection proj; // the plane the cells live on (10-T1), kept for the HUD
    uint32_t w = 0, h = 0; // = 2^order each (== proj plane side)
    uint64_t nsteps = 0; // ordered trace steps — the extent of the time axis

    // T1 (44-faithful-city-phase-a): the memory-region KIND that provably
    // owns each cell — slice-invariant (a cell's owning region never changes
    // within one recording), so this is built ONCE by build_terrain, never
    // re-derived per slice(t). Sized w*h; an off-domain / unowned cell
    // carries kKindByCellNone (see that constant's comment for why it is not
    // Region::Unknown). The GL side (Scene::set_zoning) uploads this once per
    // weave, never on a playhead scrub.
    std::vector<uint8_t> kind_by_cell;

    // Provenance chips (T2 step 2 / step 4): never a silent flat/zero plane.
    std::string basis;       // the trace basis ("abs" | "rel"), from the canvas
    std::string basis_error; // non-empty => the EXACT terrain is refused (mixed
                             // bases); slice() returns a flat plane and the HUD
                             // shows this rather than a mis-placed density
    // 36 T3: which stream fed the height field, and its provenance.
    std::string height_source; // "trace" | "df_step" | "" (nothing placed)
    std::string height_note;   // labels a df_step rung as single-step residency
    // anchor_error non-empty => a rel path had no resolvable codeimage span: no
    // cells, but nsteps is real. DISTINCT from basis_error (mixed bases).
    std::string anchor_error;
    bool torn = false; // the recording is truncated/torn or dropped events
    bool mem_present = false; // the rich rung's `mem` stream fed the data cells
    std::string mem_note; // "coarse: no per-access memory stream" when absent
    bool churn_present = false; // some code region churned within the recording

    // T1 (58-memory-data-cell-family): the data rung's own placement census.
    // The `mem` scan used to `continue` past an access no region mapped, in
    // silence — which is exactly how the whole data half stayed empty for
    // every shipped recording until 54 T1 (the observed-data-span projection)
    // landed, with nothing on screen saying so. These two counters make that
    // failure mode STATABLE: after 54 T1 `mem_dropped` should be near zero,
    // and if it is not, the span clustering is wrong and this is how anyone
    // finds out. Both stay 0 when `mem` is absent (there was nothing to place)
    // or when the terrain refused on `basis_error` before the scan.
    uint64_t mem_accesses = 0; // `mem` events the scan considered
    uint64_t mem_dropped = 0;  // of those, how many no region mapped (dropped)

    // The SEPARATE statistical layer (survey residency), distinct from every
    // exact slice() and never merged into one (the T6 isolation invariant).
    bool has_stat = false;
    Terrain stat;

    // --- prefix structure (T2 step 3): per touched cell, the ascending steps at
    // which it was hit, so slice(t) is a binary search per cell, not a re-scan.
    struct CodeCell {
        uint32_t cell = 0; // y*w + x
        std::vector<uint64_t>
            steps;              // ascending trace-step indices hitting here
        uint32_t full_heat = 0; // the canvas per-offset heat summed over the
                                // offsets in this cell (== steps.size()):
                                // the reused 04-T3 count, kept for the HUD
        uint64_t churn_step = UINT64_MAX; // slice is CHURN here for t >= this
    };
    struct DataCell {
        uint32_t cell = 0;
        std::vector<uint64_t> steps;    // ascending `mem` steps
        std::vector<uint64_t> cum_size; // parallel prefix sum of `size`
        std::vector<uint32_t> cum_rw;   // parallel prefix OR of READ/WRITE bits
        // 54 T2: cum_size split by direction, so "how much was read here" and
        // "how much was written here" are separately answerable. Parallel to
        // cum_size; cum_read_size[i] + cum_write_size[i] == cum_size[i] at every
        // index EXCEPT when an access's `rw` token is neither "r" nor "w" — that
        // access still counts into cum_size but into NEITHER direction (the
        // "unknown is not zero" invariant made arithmetic; L7 renders that gap as
        // an absent surface, never a flat one).
        std::vector<uint64_t> cum_read_size;
        std::vector<uint64_t> cum_write_size;
        // T2 (58-memory-data-cell-family): the prefix OR of the HONEST
        // direction bits (DataDir below) — 0 for an access whose `rw` token is
        // neither "r" nor "w". This is DELIBERATELY not cum_rw: cum_rw folds
        // an unknown token into TF_READ (the pre-existing approximation the
        // tint channel has always used), so "has this cell ever been observed
        // being read?" cannot be answered from it without asserting a read
        // that was never recorded. The twin-relief layer's central rule —
        // an uncaptured direction is an ABSENT surface, not a zero-height one
        // — needs a presence signal that is exact, and this is it. One byte
        // per access beside two existing 8-byte prefix sums.
        //
        // Why presence is not read off cum_read_size > 0: a zero-BYTE access
        // moves no bytes and would leave that sum at 0 while a direction was
        // genuinely observed. Magnitude and presence are different questions.
        std::vector<uint8_t> cum_dir;
    };
    std::vector<CodeCell> code;
    std::vector<DataCell> data;

    // T1 (47-scene-inspect-and-pickable-overlays.md): a cell -> content index,
    // built ONCE by build_terrain() (never re-derived per slice, exactly like
    // kind_by_cell) so a cell's CodeCell/DataCell resolves in O(log n) instead of
    // the O(code.size()+data.size()) linear scan resolve_pick used to run on
    // every click — and, without this, would have had to run on every THROTTLED
    // hover-pick frame too. Two candidate shapes were weighed: a flat
    // vector<int32_t> of size w*h (O(1), one entry per cell) or a sorted-by-cell
    // parallel index (O(log n), one entry per TOUCHED cell). This picks the
    // SORTED shape: build_projection clamps order to [6,12] (projection.h), so a
    // busy recording can size the plane to 4096x4096 = 16.7M cells — a flat
    // vector<int32_t> table at that order is ~67MB for what is typically a few
    // hundred to a few thousand TOUCHED cells, and the plane is largest exactly
    // when it is sparsest (a small domain never needs order 12). Memory that
    // scales with touched cells, not domain size, is the right trade here;
    // std::lower_bound over a few thousand entries is comfortably sub-microsecond,
    // far under the per-frame hover budget T1 exists to protect.
    std::vector<std::pair<uint32_t, uint32_t>>
        code_index; // cell -> index in code, sorted by cell
    std::vector<std::pair<uint32_t, uint32_t>>
        data_index;     // cell -> index in data, sorted by cell
    void build_index(); // (re)builds code_index/data_index from code/data
    // O(log n) lookups through code_index/data_index — nullptr when the cell
    // holds no code/data content. resolve_pick (pick.cpp) and T1's hover pick
    // both route through these, so a click and a hover cannot drift onto two
    // different answers for the same cell.
    const CodeCell *code_at(uint32_t cell) const;
    const DataCell *data_at(uint32_t cell) const;

    // The exact/coarse terrain for the INCLUSIVE time slice [0, t] (t is a trace
    // step index; a code cell counts hits with index <= t, a data cell sums the
    // `mem` sizes with step <= t). O(touched cells).
    Terrain slice(uint64_t t) const;
    // The whole recording (t past the last step).
    Terrain full() const { return slice(UINT64_MAX); }

    // 49 T3: the raw (pre-log, pre-normalise) magnitude behind the slice's
    // brightest cell — a code cell's hit count or a data cell's cumulative
    // access size, whichever is larger, at the inclusive slice [0, t]. This
    // is what a contour band is WORTH: Terrain::height is log1p'd and then
    // normalised to [0,1] at GL upload (scene.cpp's set_terrain), so neither
    // alone states the scale a legend needs. 0 for an empty slice or a
    // refused (basis_error) terrain.
    uint64_t max_full_heat(uint64_t t) const;

    // The degrade-to-coarse plane for the 3D scrub (23-graded-truth-layer.md T4):
    // a FLAT plane (no per-cell binary searches over the potentially huge
    // code/data cell lists), carrying the torn flag so it is never a silent zero.
    // O(plane) instead of O(touched cells) — the caller renders THIS while a full
    // slice() would exceed the frame budget (should_degrade), then swaps to the
    // full slice when it lands. It is the same labelled coarse rung the terrain
    // shows normally, so degrading to it hides nothing (D7).
    Terrain coarse_slice() const;
};

// Build the terrain model for a recording over an already-built projection
// (10-T1 — the caller places code regions from `codeimage` and data regions from
// a /proc/maps snapshot). `proj` is taken by value and moved into the result.
TerrainModel build_terrain(Projection proj, const Recording &rec);

// Derive code Regions from a recording's `codeimage` events (08-T7): one Region
// per distinct base, labelled and carrying the LATEST version seen. Data/stack
// regions come from a maps snapshot the caller adds (07); this gives T2 the code
// half of the plane on its own, so `build_projection(regions_from_codeimage(r))`
// is the coarse-rung projection.
std::vector<Region> regions_from_codeimage(const Recording &rec);

// The same region set in FIRST-SEEN event order instead of by-base order: the
// feed for build_projection(..., keep_order=true), where a session-union
// recording's later captures must APPEND regions rather than re-sort the ones
// already placed. Same dedup as regions_from_codeimage (one Region per base,
// widest len, latest version) — only the order differs.
std::vector<Region> regions_from_codeimage_seen(const Recording &rec);

} // namespace space
} // namespace asmdesk
#endif // ASMDESK_SPACE_TERRAIN_H
