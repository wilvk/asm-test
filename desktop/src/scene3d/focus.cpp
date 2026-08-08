// focus.cpp — the subject filter of focus.h. Standard library + the pure
// space/ models only; no GL, no ImGui, no engine (D4).
#include "scene3d/focus.h"

#include <cstdio>

namespace asmdesk::scene3d {

int32_t reresolve_region(const std::vector<space::Region> &regions,
                         uint64_t base, uint64_t len) {
    // A zero length is the never-set sentinel, and no real region has one — so
    // refuse before scanning rather than letting a degenerate entry match it.
    if (len == 0)
        return -1;
    for (size_t i = 0; i < regions.size(); i++)
        if (regions[i].base == base && regions[i].len == len)
            return static_cast<int32_t>(i);
    return -1;
}

void tid_palette(int idx, float out[4]) {
    static const float pal[6][3] = {
        {0.95f, 0.85f, 0.25f}, {0.40f, 0.80f, 1.00f}, {0.55f, 0.95f, 0.45f},
        {1.00f, 0.55f, 0.55f}, {0.80f, 0.60f, 1.00f}, {0.35f, 0.95f, 0.85f}};
    const float *c = pal[((idx % 6) + 6) % 6];
    out[0] = c[0];
    out[1] = c[1];
    out[2] = c[2];
    out[3] = 1.0f;
}

float focus_line_alpha(const SceneFocus &f, int32_t line_tid,
                       bool statistical) {
    // Nothing focused: byte-identical to the pre-51 render, deliberately
    // returning the literal 1.0f rather than a computed value.
    if (f.tid < 0)
        return 1.0f;
    // A survey is an aggregate, not a thread (see focus.h): focusing a tid
    // leaves the statistical residency line at its normal weight.
    if (statistical)
        return 1.0f;
    if (line_tid == f.tid)
        return 1.0f;
    // T4's distance budget is the ONLY thing that may drop rather than ghost,
    // and it announces the class it dropped by name.
    return f.drop_unfocused ? 0.0f : kGhostAlpha;
}

std::vector<ThreadRosterRow> thread_roster(const space::TrajectorySet &ts,
                                           const space::Projection &proj) {
    std::vector<ThreadRosterRow> out;
    out.reserve(ts.trajectories.size());
    int seq = 0;
    for (const space::Trajectory &tr : ts.trajectories) {
        ThreadRosterRow row;
        row.tid = tr.tid;
        row.statistical = (tr.flags & space::TRAJ_STATISTICAL) != 0;
        row.focusable = !row.statistical;
        // Replay Scene::set_trajectories' own per-vertex walk: a PC vertex
        // counts, a vertex that is BOTH TrajPoint::placed and projectable is
        // placed, and the strip is drawn only at two or more of those.
        for (const space::TrajPoint &pt : tr.points) {
            if (pt.is_access)
                continue;
            row.vertices++;
            float u = 0.0f, v = 0.0f;
            if (pt.placed && proj.project(pt.addr, &u, &v))
                row.placed++;
        }
        row.has_placed_path = row.placed >= 2;
        if (!row.has_placed_path) {
            // Never omission (T1 step 4): a trajectory the recording carries
            // but the plane cannot draw still gets a row, and it says why.
            row.note = row.placed == 0
                           ? "present, no placed path"
                           : "present, no placed path (one placed vertex, "
                             "which draws no strip)";
        }
        // The SAME index scene.cpp's tid_color call uses: the tid where there
        // is one, else this trajectory's ordinal.
        float rgba[4];
        tid_palette(tr.tid >= 0 ? tr.tid : seq, rgba);
        row.rgb[0] = rgba[0];
        row.rgb[1] = rgba[1];
        row.rgb[2] = rgba[2];
        out.push_back(std::move(row));
        seq++;
    }
    return out;
}

std::vector<uint8_t> build_focus_mask(const space::Projection &proj,
                                      int32_t region_index) {
    std::vector<uint8_t> mask;
    if (region_index < 0 ||
        static_cast<size_t>(region_index) >= proj.regions.size())
        return mask; // out of range: the caller draws unfiltered
    const uint32_t n = uint32_t(1) << proj.order;
    mask.assign(static_cast<size_t>(n) * n, 0u);
    for (uint32_t cell :
         space::region_cells(proj, static_cast<size_t>(region_index)))
        if (cell < mask.size())
            mask[cell] = 1u;
    return mask;
}

const char *kind_label(space::Region::Kind k) {
    // ONE palette and ONE set of names between the filter UI and the plane
    // (T2 step 4): region_style() already feeds the HUD legend and mirrors
    // kTerrainFrag's kindHue table.
    return space::region_style(k).name;
}

std::string subject_filter_note(const SceneFocus &f,
                                const space::Projection &proj) {
    if (!f.filtering())
        return std::string();
    std::string s = "subject filter ON — dimmed, never hidden:";
    bool any = false;
    if (f.tid >= 0) {
        char buf[64];
        std::snprintf(buf, sizeof buf, " thread %d", f.tid);
        s += buf;
        s += " (other threads ghosted, survey unchanged)";
        any = true;
    }
    if (f.kind_mask != kAllKinds) {
        static const space::Region::Kind kKinds[6] = {
            space::Region::Code, space::Region::Stack, space::Region::Heap,
            space::Region::Data, space::Region::Mmap,  space::Region::Unknown};
        int shown = 0;
        std::string hidden;
        for (space::Region::Kind k : kKinds) {
            if (f.kind_mask & (1u << static_cast<uint32_t>(k))) {
                shown++;
            } else {
                if (!hidden.empty())
                    hidden += ", ";
                hidden += kind_label(k);
            }
        }
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s showing %d of 6 kinds",
                      any ? ";" : "", shown);
        s += buf;
        if (!hidden.empty())
            s += " (desaturated: " + hidden + ")";
        any = true;
    }
    if (f.region >= 0 &&
        static_cast<size_t>(f.region) < proj.regions.size()) {
        const space::Region &r = proj.regions[static_cast<size_t>(f.region)];
        // The region's own verbatim label, never an invented name (D7, and
        // 48 T3's own rule for the goto combo).
        char fallback[32];
        std::snprintf(fallback, sizeof fallback, "region@0x%llx",
                      static_cast<unsigned long long>(r.base));
        s += any ? "; region " : " region ";
        s += r.label.empty() ? fallback : r.label;
        s += " only (the rest of the plane desaturated)";
        any = true;
    }
    // Nothing was actually named: the only "filter" set was a region index
    // this projection does not have, which build_focus_mask refuses to build a
    // mask for — so nothing IS being dimmed. Claiming a filter that is not
    // happening is the same category of error as hiding one that is.
    if (!any)
        return std::string();
    return s;
}

const char *focus_axis_note() {
    return "layers grade EVIDENCE (what was recorded, and how well); filters "
           "choose SUBJECT (which thread, region or kind you are reading) — a "
           "filter changes saturation and alpha only, never a height, a "
           "fidelity flag, or the model behind them";
}

} // namespace asmdesk::scene3d
