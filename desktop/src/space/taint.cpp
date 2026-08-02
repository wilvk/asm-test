// taint.cpp — the forward-spread taint isochrone of taint.h. Standard library
// + the analysis/ slicer + the doc/ and space/ models only; no GL, no ImGui,
// no engine (D4).
#include "space/taint.h"

#include <algorithm>
#include <functional>

#include "analysis/slice.h"
#include "space/stepplace.h"

namespace asmdesk::space {

namespace {

// Every memory-write operand of `step`, in the order the recording states
// them. `recs` is grouped and ascending by step (DataflowStream::recs_grouped),
// so a lower_bound finds the block.
//
// "Memory write" is `write && space in {"abs", "off"}`, exactly the brief's
// selector — but only "abs" carries an address this layer may PLACE; see
// build_taint_front's own comment on why an "off" record is counted rather
// than anchored.
void for_each_mem_write(const DataflowStream &df, uint32_t step,
                        const std::function<void(const ValRec &)> &fn) {
    auto lo = std::lower_bound(
        df.recs.begin(), df.recs.end(), step,
        [](const ValRec &r, uint32_t s) { return r.step < s; });
    for (auto it = lo; it != df.recs.end() && it->step == step; ++it) {
        if (!it->write)
            continue;
        if (it->space != "abs" && it->space != "off")
            continue;
        fn(*it);
    }
}

// Does `step` write to a register only (no memory write at all)? Used to count
// the archetypal fabricated placement this layer refuses to make.
bool has_reg_only_write(const DataflowStream &df, uint32_t step) {
    auto lo = std::lower_bound(
        df.recs.begin(), df.recs.end(), step,
        [](const ValRec &r, uint32_t s) { return r.step < s; });
    bool saw_reg_write = false;
    for (auto it = lo; it != df.recs.end() && it->step == step; ++it) {
        if (!it->write)
            continue;
        if (it->space == "reg")
            saw_reg_write = true;
        else if (it->space == "abs" || it->space == "off")
            return false; // it also wrote memory: not register-only
    }
    return saw_reg_write;
}

} // namespace

TaintFront build_taint_front(const DataflowStream &df, const Projection &proj,
                             uint32_t origin, int32_t max_depth,
                             bool truncated) {
    TaintFront out;
    out.truncated = truncated;

    if (df.nsteps == 0) {
        out.disabled_reason =
            "this recording carries no dataflow pass — there are no def-use "
            "edges to spread a value along, and this layer will not infer any";
        return out;
    }
    if (origin >= df.nsteps) {
        out.disabled_reason =
            "the chosen origin step " + std::to_string(origin) +
            " is outside this dataflow pass (" + std::to_string(df.nsteps) +
            " step(s)): nothing about it was recorded, so nothing about its "
            "spread can be said";
        return out;
    }

    // FIRST-REACH BFS DEPTH (54 T5), never dt_slice_forward's flat reachable
    // set — that set carries no depth at all (analysis/slice.h), and "how far"
    // is the entire question this layer answers.
    const dt_walk walk =
        dt_walk_depth(df.edges, df.nsteps, origin, /*forward=*/true, max_depth);
    out.enabled = true;
    out.has_origin = true;
    out.origin_step = origin;
    out.bounded = walk.bounded;
    out.depth_max = walk.depth_max;

    StepPlacer placer(proj, df);

    // --- the origin's region kind: the baseline every escape is measured
    // against. Prefer the origin's own memory write (a value's home is where it
    // was STORED); fall back to the instruction's own address. An origin that
    // places nowhere leaves origin_kind_known false, which disables escape
    // marking entirely rather than comparing against a default.
    {
        bool got = false;
        for_each_mem_write(df, origin, [&](const ValRec &r) {
            if (got || r.space != "abs")
                return;
            const StepPlace pl = place_address(proj, r.addr);
            if (!pl.placed)
                return;
            out.origin_cell = pl.cell;
            if (pl.region) {
                out.origin_kind = pl.region->kind;
                out.origin_kind_known = true;
            }
            got = true;
        });
        if (!got) {
            const StepPlace pl = placer.at(origin);
            if (pl.placed) {
                out.origin_cell = pl.cell;
                if (pl.region) {
                    out.origin_kind = pl.region->kind;
                    out.origin_kind_known = true;
                }
            }
        }
    }

    // --- which reached steps lead into a step the recording never described.
    // A POSITIVE mark on a placeable cell (TaintReach::unknown_beyond): "the
    // value did not go further from here" and "we did not look further from
    // here" are different facts, and both render as nothing by default.
    std::vector<char> reached_mask(df.nsteps, 0);
    for (uint32_t s : walk.steps)
        if (s < df.nsteps)
            reached_mask[s] = 1;
    std::vector<char> leads_to_unknown(df.nsteps, 0);
    std::vector<char> unknown_counted(df.nsteps, 0);
    for (const dt_edge &e : df.edges) {
        if (e.from_step >= df.nsteps || e.to_step >= df.nsteps)
            continue;
        if (!reached_mask[e.from_step])
            continue;
        if (df.has_step(e.to_step))
            continue;
        leads_to_unknown[e.from_step] = 1;
        if (!unknown_counted[e.to_step]) {
            unknown_counted[e.to_step] = 1;
            out.unknown_steps++;
        }
    }
    // A reached step the recording never described is itself an unknown, even
    // if no edge above named it (the origin's own pass may simply not have
    // covered it).
    for (size_t i = 0; i < walk.steps.size(); i++) {
        const uint32_t s = walk.steps[i];
        if (s >= df.nsteps || df.has_step(s) || unknown_counted[s])
            continue;
        unknown_counted[s] = 1;
        out.unknown_steps++;
    }

    // --- the front itself -------------------------------------------------
    for (size_t i = 0; i < walk.steps.size(); i++) {
        const uint32_t s = walk.steps[i];
        const int32_t d = i < walk.depth.size() ? walk.depth[i] : 0;
        if (s >= df.nsteps)
            continue;
        if (!df.has_step(s))
            continue; // already counted as an unknown gap; it has no operands

        bool wrote_memory = false;
        for_each_mem_write(df, s, [&](const ValRec &r) {
            wrote_memory = true;
            if (r.space == "off") {
                // A REGION-RELATIVE data write. The wire states no base for
                // data (df_step.rbase is the CODE base), so placing it would
                // be a fabrication of a different kind — counted and stated,
                // exactly as observed_data_spans already refuses to place
                // these raw (space/projection.h).
                out.off_relative_writes++;
                return;
            }
            const StepPlace pl = place_address(proj, r.addr);
            if (!pl.placed) {
                out.off_plane++;
                return;
            }
            TaintReach tr;
            tr.step = s;
            tr.depth = d;
            tr.addr = r.addr;
            tr.cell = pl.cell;
            tr.u = pl.u;
            tr.v = pl.v;
            if (pl.region) {
                tr.kind = pl.region->kind;
                tr.kind_known = true;
            }
            // The flow was observed, the VALUE was not.
            tr.hollow = !r.value_valid;
            // THE FINDING: two RECORDED region kinds differ. Never marked when
            // either side's kind is unknown — an escape from or to an unknown
            // kind is not a comparison, it is a guess.
            tr.escape = out.origin_kind_known && tr.kind_known &&
                        tr.kind != out.origin_kind;
            tr.unknown_beyond = leads_to_unknown[s] != 0;
            out.reached.push_back(tr);
        });
        if (!wrote_memory && has_reg_only_write(df, s)) {
            // A register-only write carries NO address. Colouring cell 0 for
            // it is the archetypal fabricated placement, so it tints nothing
            // and is counted here instead.
            out.reg_only_writes++;
        }
    }

    std::stable_sort(out.reached.begin(), out.reached.end(),
                     [](const TaintReach &a, const TaintReach &b) {
                         if (a.depth != b.depth)
                             return a.depth < b.depth;
                         return a.step < b.step;
                     });
    return out;
}

} // namespace asmdesk::space
