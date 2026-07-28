// trajectory.cpp — build execution trajectories from a recording (trajectory.h).
// Standard library + nlohmann/json only (D4): no GL, no ImGui, no engine.
#include "space/trajectory.h"

#include <algorithm>
#include <map>
#include <string>

#include "space/projection.h" // resolve_anchor / Anchor (36 T2)

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
    // TEST / PLANE-FREE overload: no plane, so no rel path can be anchored.
    Projection none;
    return build_trajectories(r, none);
}

TrajectorySet build_trajectories(const Recording &r, const Projection &proj) {
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

    // --- fall back to `df_step` for a single-step capture with no `trace` ----
    // A live serve `dataflow`/`auto` session — and a `--dataflow` file — emits
    // df_step/df_edge and NO `trace` (the SERVE_MODES table), so the loop above
    // placed no vertex. Its PC path is the df_step offset stream instead. Those
    // offsets are ROUTINE-RELATIVE by construction (df_step.off is an offset
    // from the scoped region base, exactly like a trace basis:"rel"), so the
    // path is woven as rel. Until 36 T2 the renderer then dropped it silently;
    // now the anchoring pass below PLACES it against the recording's single
    // codeimage span when there is one (base+off is the true address), and when
    // there is not, refuses LOUDER — no geometry AND a stated reason — instead of
    // the old silent empty plane (this is the sentence 36 narrows). `trace`, when
    // present, is authoritative and already fixed the basis above; an emulator
    // dataflow file carries BOTH and its trace wins, so this runs only when the
    // trace path produced nothing (and was not itself refused).
    if (by_tid.empty() && set.diagnostic.empty()) {
        if (const auto *ev = kind(r, "df_step")) {
            for (const Event &e : *ev) {
                auto off = e.body.find("off");
                if (off == e.body.end() || !off->is_number())
                    continue; // an offset-less df_step places no vertex
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
            if (!by_tid.empty())
                set.basis = "rel"; // df_step offsets are region-relative
        }
    }

    // A rel-basis PC path is routine-relative, not a true address path: flag
    // every trajectory so the HUD can say so and the renderer never treats it
    // as absolute.
    if (set.basis == "rel")
        for (auto &kv : by_tid)
            kv.second.flags |= TRAJ_RELATIVE_BASIS;

    // --- 36 T2: anchor the rel PC path to the recording's codeimage span -----
    // A rel path's vertices are routine-relative offsets. When the recording
    // pins the span down — exactly one codeimage code span, resolved over the
    // SAME region vector the projection was built from (so an anchored offset
    // projects exactly as a measured absolute PC at that address would) —
    // base+off IS the true address, a derivation from a stated fact, not a guess.
    // Rewrite each PC vertex onto the absolute plane, flag TRAJ_ANCHORED while
    // KEEPING TRAJ_RELATIVE_BASIS (the wire basis is still rel), and MEASURE
    // p.placed per vertex (place() fails past the 4096-byte codeimage clamp). A
    // span that is absent or ambiguous leaves the offsets verbatim — never a
    // fabricated address — and records why in placement_note. The `mem` spur loop
    // and the `survey` loop below are absolute by construction and become
    // CONSISTENT with the PC path once it is anchored; that is why neither is
    // touched here.
    if (set.basis == "rel") {
        Anchor anchor = resolve_anchor(proj.regions);
        if (anchor.ok) {
            set.anchored = true;
            for (auto &kv : by_tid) {
                kv.second.flags |= TRAJ_ANCHORED;
                for (TrajPoint &p : kv.second.points) {
                    if (p.is_access)
                        continue; // no access marks yet, but stay honest
                    uint64_t abs = 0;
                    if (anchor.place(p.addr, &abs)) {
                        p.addr = abs;
                        p.placed = true;
                    } else {
                        p.placed = false; // out-of-span: leave addr = raw offset
                    }
                }
            }
        } else {
            set.placement_note = anchor.reason;
        }
    }

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
    // Count placement as we go (36 T2 step 3): every PC vertex is offered to the
    // plane and pc_placed counts how many the renderer's own test — proj.project
    // — actually accepts. For an anchored path this equals the count of placed
    // vertices; for an abs path it is the honest projected count. This is the
    // assertion whose ABSENCE let a total placement failure look like success.
    for (auto &kv : by_tid) {
        std::stable_sort(kv.second.points.begin(), kv.second.points.end(),
                         [](const TrajPoint &a, const TrajPoint &b) {
                             if (a.t != b.t)
                                 return a.t < b.t;
                             return !a.is_access && b.is_access;
                         });
        for (const TrajPoint &p : kv.second.points) {
            if (p.is_access)
                continue;
            set.pc_points++;
            float u, v;
            if (proj.project(p.addr, &u, &v))
                set.pc_placed++;
        }
        set.trajectories.push_back(std::move(kv.second));
    }

    // Honest partial placement: name the SERVE_CI_MAX_BYTES=4096 codeimage clamp
    // as the cause, or an honest partial reads as a regression. Only for an
    // anchored path — an abs path that fails to project is a different situation
    // (no code region covers it), not the clamp, and an unanchored rel path
    // already carries resolve_anchor's reason in placement_note.
    if (set.anchored && set.pc_placed < set.pc_points &&
        set.placement_note.empty()) {
        set.placement_note =
            std::to_string(set.pc_points - set.pc_placed) + " of " +
            std::to_string(set.pc_points) +
            " path vertices project off-plane — the 4096-byte codeimage clamp "
            "(SERVE_CI_MAX_BYTES): an in-routine offset past the captured span "
            "has no plane cell";
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
