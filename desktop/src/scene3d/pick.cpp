// pick.cpp — the GL-free half of colour-ID picking (pick.h): id decode + the
// resolution of a pick to a 04 deep-link, plus (T2, 47-scene-inspect-and-
// pickable-overlays) the hover-readout hint that shares its classification.
// Standard library + nav.h + the space/ models only; no GL, no ImGui, no
// engine (D4).
#include "scene3d/pick.h"

#include <cmath>
#include <cstdio>

namespace asmdesk::scene3d {

namespace {

// T2: a small hex formatter, mirroring space/terrain.cpp's own local `hex()`
// (and projection.cpp's/projection.h's Anchor two-span reason) — each pure TU
// in this family keeps its own copy rather than sharing one across the GL-free
// boundary, the established convention here (D4: no new shared header for one
// three-line helper).
std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// The region owning `addr`, or nullptr — a small linear scan (regions.size()
// is a handful even for a busy recording, the same scale resolve_anchor's own
// linear scan over code spans already assumes). Projection has no direct
// address->region lookup (only unproject(u,v), keyed on a PLANE cell); this is
// what a vertex hint needs to name a region without projecting and
// unprojecting back through the plane.
const space::Region *region_containing(const space::Projection &proj,
                                       uint64_t addr) {
    for (const space::Region &r : proj.regions)
        if (addr >= r.base && addr < r.base + r.len)
            return &r;
    return nullptr;
}

// "region label, or its kind's own legend name when unlabelled" — the exact
// fallback hud.cpp's region legend already uses (draw_scene_hud's `regions:`
// block), reused here so a hint's `where` never invents a name a real legend
// row would not also show.
std::string region_name(const space::Region &r) {
    return r.label.empty() ? space::region_style(r.kind).name : r.label;
}

// T2: everything a Cell pick's classification needs, computed ONCE so
// resolve_pick and resolve_pick_hint read the same facts and cannot drift onto
// two different answers for the same cell. Mirrors resolve_pick's original
// inline logic exactly (unproject at the cell centre, then the code/data/stat
// checks in the SAME priority order the "Cell holds at most one region byte"
// comment states).
struct CellFacts {
    bool have_region = false;
    const space::Region *region = nullptr;
    uint64_t addr = 0;
    uint64_t off = 0; // addr - region->base; valid iff have_region
    const space::TerrainModel::CodeCell *code = nullptr;
    const space::TerrainModel::DataCell *data = nullptr;
    bool stat = false; // TF_STAT residency (no exact content at this cell)
};

CellFacts classify_cell(const space::TerrainModel &terr, uint32_t cell) {
    CellFacts f;
    const uint32_t n = terr.w;
    if (n == 0)
        return f;
    const uint32_t x = cell % n, y = cell / n;
    if (y >= n)
        return f;
    const float u = (x + 0.5f) / static_cast<float>(n);
    const float v = (y + 0.5f) / static_cast<float>(n);
    const space::Region *r = nullptr;
    uint64_t addr = 0;
    f.have_region = terr.proj.unproject(u, v, &addr, &r) && r;
    f.region = r;
    f.addr = addr;
    if (f.have_region)
        f.off = addr - r->base;
    f.code = terr.code_at(cell);
    if (!f.code)
        f.data = terr.data_at(cell);
    f.stat = terr.has_stat && cell < terr.stat.flags.size() &&
             (terr.stat.flags[cell] & space::TF_STAT) != 0u;
    return f;
}

// T2: the fidelity wording below is a DELIBERATE keep-in-sync duplicate of
// hud.cpp's existing chip text (terrain_encoding_swatches() for the per-flag
// facts, placement_chips()'s own phrasing for what a graded state says) — the
// brief's own instruction is to reuse that wording rather than invent new
// phrasing for the same facts (D7 / 24-one-visual-language). pick.cpp cannot
// literally CALL hud.cpp's functions (hud.h pulls in ImGui + Scene, which
// would break this TU's GL/ImGui-free D4 guarantee — the same guarantee that
// lets test_drillin run with no GL context at all), so the strings are
// duplicated here with this comment as the sync point, exactly like
// TerrainFlag's TORN/STAT/CHURN bit values already mirror the GLSL constants
// in scene3d/shaders/embedded.h (terrain.h's own doc comment on that
// precedent).
constexpr const char *kFidelityTorn =
    "torn: capture truncated/dropped (rubble, a known lower bound)";
constexpr const char *kFidelityStatCell =
    "statistical: sampled residency (separate ghost-fog layer), never exact";
constexpr const char *kFidelityStatVertex =
    "statistical residency — sampled, never an exact PC (hot-edge view only)";
constexpr const char *kFidelityUnknown =
    "fog-of-war: in-domain, never described (no code/data cell recorded here)";
constexpr const char *kFidelityChurn =
    "churn: codeimage version changed within [0,t] (scaffold) — the disasm "
    "pane resolves which version";
constexpr const char *kFidelityPadding =
    "outside the compacted domain — nothing here";

} // namespace

Pick decode_pick(uint32_t id, uint32_t n) {
    Pick p;
    if (id == 0)
        return p; // background
    const uint64_t ncells = static_cast<uint64_t>(n) * n;
    const uint32_t d = id - 1u;
    if (d < ncells) {
        p.kind = Pick::Cell;
        p.cell = d;
    } else {
        p.kind = Pick::Vertex;
        p.vertex = static_cast<uint64_t>(d) - ncells;
    }
    return p;
}

std::vector<PickVertex> pick_vertex_order(const space::TrajectorySet &traj) {
    std::vector<PickVertex> out;
    for (const space::Trajectory &tr : traj.trajectories) {
        const bool st = (tr.flags & space::TRAJ_STATISTICAL) != 0;
        for (const space::TrajPoint &pt : tr.points)
            if (!pt.is_access)
                out.push_back({tr.tid, pt.t, st, pt.addr});
    }
    return out;
}

std::optional<dt_link> resolve_pick(const space::TerrainModel &terr,
                                    const space::TrajectorySet &traj,
                                    const std::string &rec, const Pick &p) {
    if (p.kind == Pick::None)
        return std::nullopt;

    if (p.kind == Pick::Cell) {
        const uint32_t n = terr.w;
        if (n == 0)
            return std::nullopt;
        if (p.cell / n >= n)
            return std::nullopt;

        // T2 (47): the classification both resolve_pick and resolve_pick_hint
        // share — one place decides "what is under this cell", so the two
        // cannot disagree.
        const CellFacts f = classify_cell(terr, p.cell);

        dt_link link;
        link.rec = rec;

        // Exact code: the trace canvas at the offset, UNLESS the region churned
        // within the recording — then the codeimage-versioned disasm pane (08-T7),
        // because the bytes at that offset differ by trace time and only disasm
        // resolves "which version at time t". The offset is addr - base: the
        // recording's basis is region-relative (offsets from the region base; an
        // abs recording's region base is the codeimage base), the same key the
        // canvas and the disasm pane already use.
        if (f.code) {
            if (!f.have_region)
                return std::nullopt; // a lit code cell must map to a region
            const bool churned = f.code->churn_step != UINT64_MAX;
            link.view = churned ? dt_view::disasm : dt_view::canvas;
            link.off = f.off;
            return link;
        }

        // A data access (rich `mem` rung) -> the slice explorer at the step whose
        // access LAST hit this cell (`steps` is ascending; back() is most recent):
        // the dataflow reader for "what touched this address".
        if (f.data && !f.data->steps.empty()) {
            link.view = dt_view::slice;
            link.step = static_cast<uint32_t>(f.data->steps.back());
            return link;
        }

        // No exact content. A statistical-only cell (survey residency, TF_STAT) ->
        // the hot-edge view (08-T4), NEVER the exact slice explorer: a sampled
        // residency is not an exact density and must not open an exact reader (the
        // "statistical is never exact" invariant).
        if (f.stat) {
            link.view = dt_view::hotedges;
            return link;
        }

        // A bare region cell with neither exact nor statistical content: open the
        // canvas at its offset. A cell mapping to no region at all is padding.
        if (!f.have_region)
            return std::nullopt;
        link.view = dt_view::canvas;
        link.off = f.off;
        return link;
    }

    // Kind::Vertex: replay the canonical PC-vertex order to recover its (step,
    // fidelity). A statistical residency vertex -> the hot-edge view (08-T4), never
    // the exact operand timeline; an exact PC vertex -> the operand timeline at that
    // step.
    std::vector<PickVertex> order = pick_vertex_order(traj);
    if (p.vertex >= order.size())
        return std::nullopt;
    const PickVertex &pv = order[p.vertex];
    dt_link link;
    link.rec = rec;
    if (pv.statistical) {
        link.view = dt_view::hotedges;
        return link;
    }
    link.view =
        dt_view::timeline; // the Loom / operand timeline reads this vertex
    link.step = static_cast<uint32_t>(pv.t);
    return link;
}

PickHint resolve_pick_hint(const space::TerrainModel &terr,
                           const space::TrajectorySet &traj,
                           const space::ConvergenceSet & /*conv*/,
                           const Pick &p) {
    PickHint hint;

    if (p.kind == Pick::None)
        return hint; // empty=true: nothing pickable under the cursor

    if (p.kind == Pick::Cell) {
        const uint32_t n = terr.w;
        if (n == 0 || p.cell / n >= n)
            return hint;
        hint.empty = false;

        // T2: the SAME classification resolve_pick uses — this is the
        // anti-drift guarantee the "Tests" section requires be exhaustive.
        const CellFacts f = classify_cell(terr, p.cell);

        if (f.code) {
            hint.what = "code cell";
            hint.where = f.have_region
                             ? region_name(*f.region) + " +" + hex(f.off)
                             : hex(f.addr);
            char buf[64];
            std::snprintf(buf, sizeof buf, "heat %u hits", f.code->full_heat);
            hint.quantity = buf;
            const bool churned = f.code->churn_step != UINT64_MAX;
            hint.fidelity = churned ? kFidelityChurn : "";
            hint.target =
                dt_view_name(churned ? dt_view::disasm : dt_view::canvas);
            return hint;
        }

        if (f.data && !f.data->steps.empty()) {
            hint.what = "data cell";
            hint.where = f.have_region
                             ? region_name(*f.region) + " +" + hex(f.off)
                             : hex(f.addr);
            char buf[64];
            std::snprintf(buf, sizeof buf,
                          "%llu bytes accessed (%zu access(es))",
                          (unsigned long long)f.data->cum_size.back(),
                          f.data->steps.size());
            hint.quantity = buf;
            hint.fidelity = "";
            hint.target = dt_view_name(dt_view::slice);
            return hint;
        }

        if (f.stat) {
            hint.what = "survey cell";
            hint.where = f.have_region
                             ? region_name(*f.region) + " +" + hex(f.off)
                             : hex(f.addr);
            // The raw per-cell residency count is not stored — only its
            // log1p height is (space/terrain.cpp's build_stat) — so the
            // exact integer is recovered by inverting log1p (exact modulo
            // float rounding), stated as approximate rather than invented
            // (D7: never a fabricated precise count).
            const double approx =
                std::expm1(static_cast<double>(terr.stat.height[p.cell]));
            char buf[80];
            std::snprintf(buf, sizeof buf,
                          "residency ~%lld samples (log-scaled)",
                          (long long)std::llround(approx));
            hint.quantity = buf;
            hint.fidelity = kFidelityStatCell;
            hint.target = dt_view_name(dt_view::hotedges);
            return hint;
        }

        // A bare region cell with neither exact nor statistical content.
        if (f.have_region) {
            const bool is_code = f.region->kind == space::Region::Code;
            hint.what = is_code ? "code cell" : "data cell";
            hint.where = region_name(*f.region) + " +" + hex(f.off);
            // "Coarse is never a silent zero" (terrain.h's own rule): a data
            // region with no `mem` stream says so via TerrainModel::mem_note
            // rather than "0 bytes"; an exact-instrumented code region that
            // truly never ran says the fog-of-war fact instead — never "0
            // hits" either (D7's fidelity note explicitly forbids that).
            if (!is_code && !terr.mem_present && !terr.mem_note.empty()) {
                hint.quantity = terr.mem_note;
                hint.fidelity = "";
            } else {
                hint.quantity = "no content recorded";
                hint.fidelity = kFidelityUnknown;
            }
            hint.target = dt_view_name(dt_view::canvas);
            return hint;
        }

        // No owning region at all: padding beyond the compacted domain.
        hint.what = "padding";
        hint.where = "";
        hint.quantity = "";
        hint.fidelity = kFidelityPadding;
        hint.target = ""; // a click here does nothing
        return hint;
    }

    // Kind::Vertex.
    std::vector<PickVertex> order = pick_vertex_order(traj);
    if (p.vertex >= order.size())
        return hint; // past all uploaded geometry: empty=true
    hint.empty = false;
    const PickVertex &pv = order[p.vertex];
    hint.what = "PC vertex";
    char buf[48];
    std::snprintf(buf, sizeof buf, "trace step %llu", (unsigned long long)pv.t);
    hint.quantity = buf;
    const space::Region *r = region_containing(terr.proj, pv.addr);
    hint.where = r ? region_name(*r) + " +" + hex(pv.addr - r->base)
                   : hex(pv.addr) + " (no owning region)";
    if (pv.statistical) {
        hint.fidelity = kFidelityStatVertex;
        hint.target = dt_view_name(dt_view::hotedges);
    } else {
        hint.fidelity = "";
        hint.target = dt_view_name(dt_view::timeline);
    }
    return hint;
}

} // namespace asmdesk::scene3d
