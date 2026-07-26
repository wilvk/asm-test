// trajectory.cpp — build execution trajectories from a recording (trajectory.h).
// Standard library + nlohmann/json only (D4): no GL, no ImGui, no engine.
#include "space/trajectory.h"

#include <algorithm>
#include <map>

namespace asmdesk::space {

namespace {

// Read one JSON field, leaving `out` untouched when it is absent or the wrong
// type. Absence is normal in this schema (optional fields are OMITTED, never
// null) — a builder degrades instead of erroring. Mirrors streams.cpp's helper
// so the two decoders read the schema the same way.
template <typename T>
void get(const nlohmann::json &j, const char *key, T &out) {
    auto it = j.find(key);
    if (it == j.end())
        return;
    if constexpr (std::is_same_v<T, std::string>) {
        if (it->is_string())
            out = it->get<std::string>();
    } else if constexpr (std::is_same_v<T, bool>) {
        if (it->is_boolean())
            out = it->get<bool>();
    } else {
        if (it->is_number_integer() || it->is_number_unsigned())
            out = it->get<T>();
    }
}

const std::vector<Event> *kind(const Recording &r, const char *k) {
    auto it = r.by_kind.find(k);
    return it == r.by_kind.end() ? nullptr : &it->second;
}

} // namespace

TrajectorySet build_trajectories(const Recording &r) {
    TrajectorySet set;

    // --- PC path from `trace` events, grouped by tid ----------------------
    // The basis check mirrors 04's canvas rule (streams.cpp note_basis): the
    // first basis seen wins; a second, DIFFERENT one — or an absent one — sets
    // the diagnostic, and the whole PC path is then refused rather than having
    // every vertex mis-placed on the plane. A rel offset and an abs address
    // cannot share one axis.
    //
    // tid comes off the event where the schema provides it. A replay `trace`
    // stream omits it, so every vertex falls into the single tid = -1 group; a
    // live feed (07) that tags events per thread splits into one Trajectory per
    // tid. The grouping is the same code either way.
    std::map<int32_t, Trajectory> by_tid;
    std::map<int32_t, uint64_t> next_t; // per-tid running step index

    if (const auto *ev = kind(r, "trace")) {
        for (const Event &e : *ev) {
            std::string basis;
            get(e.body, "basis", basis);
            if (basis.empty()) {
                if (set.diagnostic.empty())
                    set.diagnostic =
                        "a trace event carries no \"basis\" — the schema "
                        "forbids defaulting it, so this trajectory cannot be "
                        "placed";
                continue;
            }
            if (set.basis.empty())
                set.basis = basis;
            else if (set.basis != basis && set.diagnostic.empty())
                set.diagnostic =
                    "mixed address bases in the PC path: \"" + set.basis +
                    "\" and \"" + basis +
                    "\" trace events — a routine-relative offset and an "
                    "absolute address cannot share one trajectory; re-record, "
                    "or build the trajectories separately";

            auto off = e.body.find("off");
            if (off == e.body.end() || !off->is_number())
                continue; // an offset-less trace event places no vertex

            int32_t tid = -1;
            get(e.body, "tid", tid);
            Trajectory &tr = by_tid[tid];
            tr.tid = tid;
            TrajPoint p;
            p.t = next_t[tid]++;
            p.addr = off->get<uint64_t>();
            p.fidelity = TrajPoint::Exact;
            p.is_access = false;
            p.tid = tid;
            tr.points.push_back(p);
        }
    }

    // A rel-basis PC path is routine-relative, not a true address path: flag
    // every trajectory so the HUD can say so and the renderer never treats it
    // as absolute.
    if (set.basis == "rel")
        for (auto &kv : by_tid)
            kv.second.flags |= TRAJ_RELATIVE_BASIS;

    // Refuse: mirror the canvas — a refused build has no trajectories at all,
    // only the diagnostic (and the basis, for the HUD chip). Anything drawn
    // would be mis-attributed.
    if (!set.diagnostic.empty()) {
        set.trajectories.clear();
        return set;
    }

    // --- access marks from `mem` events (rich rung; gated) ----------------
    // The "kind present?" runtime gate (10 doc T2/T3): `mem` has no v1 producer,
    // so a real recording carries none of these and this whole path is inert. A
    // synthetic fixture (hand-authored `mem` lines) exercises it. Absent the
    // stream the data cells stay flat and the HUD shows a "coarse: no per-access
    // memory stream" provenance chip — never a silent zero.
    if (const auto *ev = kind(r, "mem")) {
        set.mem_present = !ev->empty();
        for (const Event &e : *ev) {
            int32_t tid = -1;
            get(e.body, "tid", tid);
            auto host = by_tid.find(tid);
            if (host == by_tid.end())
                continue; // an access for a tid with no PC path: nothing to spur
            uint64_t step = 0, ea = 0;
            get(e.body, "step", step);
            get(e.body, "ea", ea);
            TrajPoint p;
            p.t = step; // attaches to the PC vertex of this step
            p.addr = ea;
            p.fidelity = TrajPoint::Exact;
            p.is_access = true;
            p.tid = tid;
            host->second.points.push_back(p);
        }
    }

    // Emit the exact PC trajectories, tid-ascending for determinism, each
    // sorted by (t, is_access) so a PC vertex precedes its own access mark.
    for (auto &kv : by_tid) {
        std::stable_sort(kv.second.points.begin(), kv.second.points.end(),
                         [](const TrajPoint &a, const TrajPoint &b) {
                             if (a.t != b.t)
                                 return a.t < b.t;
                             return !a.is_access && b.is_access;
                         });
        set.trajectories.push_back(std::move(kv.second));
    }

    // --- statistical residency from `survey` edges ------------------------
    // A survey is aggregate from->to branch edges (ibs-op / sw-clock), always
    // exact:false, its endpoints absolute addresses (from_addr / to_addr). It
    // becomes a SEPARATE Statistical trajectory — never merged into an exact PC
    // path (the "statistical is never exact" invariant, enforced by keeping it a
    // distinct object). Each edge contributes its two endpoints as a segment; t
    // is a synthetic running index, since a survey has no true time order.
    if (const auto *ev = kind(r, "survey")) {
        Trajectory stat;
        stat.tid = -1;
        stat.flags = TRAJ_STATISTICAL;
        uint64_t t = 0;
        for (const Event &e : *ev) {
            auto edges = e.body.find("edges");
            if (edges == e.body.end() || !edges->is_array())
                continue;
            for (const auto &x : *edges) {
                uint64_t from = 0, to = 0;
                get(x, "from_addr", from);
                get(x, "to_addr", to);
                for (uint64_t a : {from, to}) {
                    TrajPoint p;
                    p.t = t++;
                    p.addr = a;
                    p.fidelity = TrajPoint::Statistical;
                    p.is_access = false;
                    p.tid = -1;
                    stat.points.push_back(p);
                }
            }
        }
        if (!stat.points.empty())
            set.trajectories.push_back(std::move(stat));
    }

    return set;
}

} // namespace asmdesk::space
