// pick.cpp — the GL-free half of colour-ID picking (pick.h): id decode + the
// resolution of a pick to a 04 deep-link, plus (T2, 47-scene-inspect-and-
// pickable-overlays) the hover-readout hint that shares its classification.
// Standard library + nav.h + the space/ models only; no GL, no ImGui, no
// engine (D4). T3 (50-two-way-brushing) adds doc/streams.h (DataflowStream,
// engine-free itself) and space/locate.h (StepAddrResolver) for the
// reverse-direction address search — still no GL, no ImGui, no engine.
#include "scene3d/pick.h"

#include <cmath>
#include <cstdio>

#include "doc/streams.h"
#include "space/locate.h"

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

// T3 (47): a convergence arc has TWO valid destinations (tid_a @ t_a, tid_b
// @ t_b) — a single dt_link cannot express both, so this resolves to
// whichever side's t is nearer the current follow_step, favouring `a` on an
// exact tie (both equidistant): a deterministic, documented rule rather than
// an arbitrary one. Shared by resolve_pick and resolve_pick_hint so the
// hint's "which side was chosen" can never disagree with what a click does.
bool conv_pick_a(const space::ConvergenceMark &m, uint64_t follow_step) {
    const uint64_t da =
        m.t_a > follow_step ? m.t_a - follow_step : follow_step - m.t_a;
    const uint64_t db =
        m.t_b > follow_step ? m.t_b - follow_step : follow_step - m.t_b;
    return da <= db;
}

// T3 (50-two-way-brushing): the address-first step resolution shared by the
// Vertex, Conv and Spur branches. A per-tid ordinal is a global dataflow step
// index only by luck (a multi-tid trace collides tid 1's and tid 2's ordinals
// onto the same values), so resolve the element's own address to a step the
// SAME way `go to` / StepAddrResolver would, and fall back to an honest CANVAS
// offset rather than pass an ordinal the stream cannot honour as a step. The
// ordinal is trusted ONLY when the trajectory set holds a single tid — the one
// case nothing else could have collided onto its step numbers.
//   placed==false  -> `addr` is a raw wire offset: skip the address route and,
//                     absent the single-tid shortcut, open the canvas at it.
std::optional<dt_link> resolve_addr_link(const space::TerrainModel &terr,
                                         const std::string &rec, uint64_t addr,
                                         bool placed, bool single_tid,
                                         uint64_t ordinal,
                                         const DataflowStream *df) {
    dt_link link;
    link.rec = rec;
    if (placed && df != nullptr) {
        space::StepAddrResolver resolver(terr.proj, *df);
        for (uint32_t i = 0; i < df->insn_off.size(); i++) {
            uint64_t a = 0;
            if (resolver.resolve(i, &a) && a == addr) {
                link.view = dt_view::timeline;
                link.step = i;
                return link;
            }
        }
    }
    if (single_tid) {
        link.view = dt_view::timeline;
        link.step = static_cast<uint32_t>(ordinal);
        return link;
    }
    if (!placed) {
        link.view = dt_view::canvas;
        link.off = addr; // the raw wire offset, already the recording's basis
        return link;
    }
    // A placed address the stream could not name a step for: address it by
    // OFFSET through its owning region rather than fabricate a step.
    float u = 0.0f, v = 0.0f;
    const uint32_t n = terr.w;
    if (n > 0 && terr.proj.project(addr, &u, &v)) {
        uint32_t x = static_cast<uint32_t>(u * n);
        uint32_t y = static_cast<uint32_t>(v * n);
        if (x >= n)
            x = n - 1;
        if (y >= n)
            y = n - 1;
        const CellFacts f = classify_cell(terr, y * n + x);
        if (f.have_region) {
            link.view = dt_view::canvas;
            link.off = f.off;
            return link;
        }
    }
    return std::nullopt; // no route can honestly address this element
}

} // namespace

Pick decode_pick(uint32_t id, uint32_t n, const PickBands &bands) {
    Pick p;
    if (id == 0)
        return p; // background
    // T1 (59-standalone-scenes): the OUTER band. An id whose top bits name a
    // non-Plane SceneKind belongs to a different substrate (a stale read-back
    // after a kind switch, or a caller that mixed the two) — it is not a cell
    // of THIS plane, so it decodes to None rather than aliasing onto one.
    // Every plane id is < 2^28, so this branch is unreachable for anything
    // pick_id_cell/_vertex/_conv/_spur ever produced.
    if (pick_id_kind(id) != SceneKind::Plane)
        return p;
    const uint64_t ncells = static_cast<uint64_t>(n) * n;
    const uint32_t d = id - 1u;
    if (d < ncells) {
        p.kind = Pick::Cell;
        p.cell = d;
        return p;
    }
    uint64_t rest = static_cast<uint64_t>(d) - ncells;

    // PickBands::npts == UINT64_MAX (the default) is the pre-T3 "unbounded
    // vertex band, no conv/spur band" shape — every caller that never learned
    // about bands decodes exactly as it always did.
    if (bands.npts == UINT64_MAX) {
        p.kind = Pick::Vertex;
        p.vertex = rest;
        return p;
    }
    if (rest < bands.npts) {
        p.kind = Pick::Vertex;
        p.vertex = rest;
        return p;
    }
    rest -= bands.npts;
    if (rest < bands.nconv) {
        p.kind = Pick::Conv;
        p.conv = rest;
        return p;
    }
    rest -= bands.nconv;
    if (rest < bands.nspur) {
        p.kind = Pick::Spur;
        p.spur = rest;
        return p;
    }
    return p; // past every known band: None
}

std::vector<PickVertex> pick_vertex_order(const space::TrajectorySet &traj) {
    std::vector<PickVertex> out;
    for (const space::Trajectory &tr : traj.trajectories) {
        const bool st = (tr.flags & space::TRAJ_STATISTICAL) != 0;
        for (const space::TrajPoint &pt : tr.points)
            if (!pt.is_access)
                out.push_back({tr.tid, pt.t, st, pt.addr, pt.placed});
    }
    return out;
}

// T3 (47): mirrors scene.cpp's set_trajectories spur-building loop EXACTLY —
// same trajectory-then-point iteration, same "projected PC vertex precedes a
// projected access point on the SAME trajectory" admission test — so index i
// here names the SAME spur the GL upload gave pick_id_spur(n, npts, nconv, i)
// to. See pick.h's own keep-in-sync comment on this function.
std::vector<PickSpur> pick_spur_order(const space::TrajectorySet &traj,
                                      const space::Projection &proj) {
    std::vector<PickSpur> out;
    for (const space::Trajectory &tr : traj.trajectories) {
        uint64_t last_pc_t = 0;
        uint64_t last_pc_addr = 0;
        bool last_pc_placed = true;
        bool have_last_pc = false;
        for (const space::TrajPoint &pt : tr.points) {
            float u = 0.0f, v = 0.0f;
            const bool projected = proj.project(pt.addr, &u, &v);
            if (!pt.is_access) {
                if (projected) {
                    last_pc_t = pt.t;
                    last_pc_addr = pt.addr;
                    last_pc_placed = pt.placed;
                    have_last_pc = true;
                }
            } else if (projected && have_last_pc) {
                out.push_back(
                    {tr.tid, last_pc_t, last_pc_addr, last_pc_placed});
            }
        }
    }
    return out;
}

std::optional<dt_link> resolve_pick(const space::TerrainModel &terr,
                                    const space::TrajectorySet &traj,
                                    const std::string &rec, const Pick &p,
                                    const space::ConvergenceSet &conv,
                                    uint64_t follow_step,
                                    const DataflowStream *df) {
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

    if (p.kind == Pick::Vertex) {
        // Replay the canonical PC-vertex order to recover its (step, fidelity).
        // A statistical residency vertex -> the hot-edge view (08-T4), never the
        // exact operand timeline; an exact PC vertex -> the operand timeline at
        // that step, resolved by ADDRESS (resolve_addr_link's contract).
        std::vector<PickVertex> order = pick_vertex_order(traj);
        if (p.vertex >= order.size())
            return std::nullopt;
        const PickVertex &pv = order[p.vertex];
        if (pv.statistical) {
            dt_link link;
            link.rec = rec;
            link.view = dt_view::hotedges;
            return link;
        }
        return resolve_addr_link(terr, rec, pv.addr, pv.placed,
                                 traj.trajectories.size() == 1, pv.t, df);
    }

    if (p.kind == Pick::Conv) {
        // T3 (47): resolve to whichever tid's t is nearer follow_step — see
        // conv_pick_a's own doc comment for the tie rule. A convergence is
        // cross-tid BY DEFINITION, so t_a/t_b are two DIFFERENT threads' step
        // indices, never a shared clock — resolve by the chosen side's ADDRESS
        // (addr_a/addr_b are real placed addresses), never the per-tid ordinal.
        if (p.conv >= conv.marks.size())
            return std::nullopt;
        const space::ConvergenceMark &m = conv.marks[p.conv];
        const bool pick_a = conv_pick_a(m, follow_step);
        return resolve_addr_link(terr, rec, pick_a ? m.addr_a : m.addr_b,
                                 /*placed=*/true, traj.trajectories.size() == 1,
                                 pick_a ? m.t_a : m.t_b, df);
    }

    if (p.kind == Pick::Spur) {
        // T3 (47): a spur drills to its owning PC vertex's step — resolved by
        // that vertex's ADDRESS (pick_spur_order carries it), never the per-tid
        // ordinal, which collides across tids exactly as PC vertices do.
        std::vector<PickSpur> order = pick_spur_order(traj, terr.proj);
        if (p.spur >= order.size())
            return std::nullopt;
        const PickSpur &sp = order[p.spur];
        return resolve_addr_link(terr, rec, sp.addr, sp.placed,
                                 traj.trajectories.size() == 1, sp.t, df);
    }

    return std::nullopt;
}

PickHint resolve_pick_hint(const space::TerrainModel &terr,
                           const space::TrajectorySet &traj,
                           const space::ConvergenceSet &conv, const Pick &p,
                           uint64_t follow_step) {
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

    if (p.kind == Pick::Vertex) {
        std::vector<PickVertex> order = pick_vertex_order(traj);
        if (p.vertex >= order.size())
            return hint; // past all uploaded geometry: empty=true
        hint.empty = false;
        const PickVertex &pv = order[p.vertex];
        hint.what = "PC vertex";
        char buf[48];
        std::snprintf(buf, sizeof buf, "trace step %llu",
                      (unsigned long long)pv.t);
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

    if (p.kind == Pick::Conv) {
        // T3 (47): a convergence is a co-locality HINT, never a proven race
        // or order (converge.h's own contract) — carry `gap` verbatim and
        // say EXPLICITLY which side the pick resolved to, rather than
        // silently picking one (the brief's own "must say which side was
        // chosen" instruction). Making the mark pickable must not upgrade
        // the claim it makes.
        if (p.conv >= conv.marks.size())
            return hint; // empty=true
        hint.empty = false;
        const space::ConvergenceMark &m = conv.marks[p.conv];
        const bool pick_a = conv_pick_a(m, follow_step);
        hint.what = "convergence";
        hint.where = "tid " + std::to_string(m.tid_a) + " @ " + hex(m.addr_a) +
                     " <-> tid " + std::to_string(m.tid_b) + " @ " +
                     hex(m.addr_b);
        char buf[32];
        std::snprintf(buf, sizeof buf, "gap %llu step(s)",
                      (unsigned long long)m.gap);
        hint.quantity = buf;
        hint.fidelity = std::string(conv.label()) + " — resolved to tid " +
                        std::to_string(pick_a ? m.tid_a : m.tid_b) +
                        " (nearer the current playhead)";
        hint.target = dt_view_name(dt_view::timeline);
        return hint;
    }

    if (p.kind == Pick::Spur) {
        std::vector<PickSpur> order = pick_spur_order(traj, terr.proj);
        if (p.spur >= order.size())
            return hint; // empty=true
        hint.empty = false;
        const PickSpur &sp = order[p.spur];
        hint.what = "access spur";
        hint.where = "tid " + std::to_string(sp.tid);
        char buf[48];
        std::snprintf(buf, sizeof buf, "trace step %llu",
                      (unsigned long long)sp.t);
        hint.quantity = buf;
        hint.fidelity = "";
        hint.target = dt_view_name(dt_view::timeline);
        return hint;
    }

    return hint;
}

} // namespace asmdesk::scene3d
