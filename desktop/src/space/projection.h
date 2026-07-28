// projection.h — address space -> unit plane, via a Hilbert curve over a
// compacted address domain (docs/internal/gui/10-spacetime-3d-overview.md T1).
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

// Region-kind colour + legend name, carried through for the HUD legend (T1 step
// 4). Pure data: the renderer (T4) turns it into GL; this returns only floats
// and a static string, so the legend is decided in an engine-free, testable TU.
struct RegionStyle {
    float r, g, b;
    const char *name;
};
RegionStyle region_style(Region::Kind kind);

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_PROJECTION_H
