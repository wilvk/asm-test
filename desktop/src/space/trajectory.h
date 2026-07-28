// trajectory.h — execution trajectories over the address-space plane
// (docs/internal/gui/10-spacetime-3d-overview.md T3). The PC weaves up through
// the projection (T1) over time; each memory access it makes marks a data
// address at the same height. This TU turns a recording's events into the
// ordered TrajPoints that path is drawn from.
//
// Pure and engine-free (D4): the standard library, nlohmann/json (via
// recording.h) and the T1 data model only — no GL, no ImGui, no Unicorn /
// Keystone / Capstone. That is what lets it compile into BOTH desktop binaries
// and the null test harness, the same closure argument projection.h makes.
#ifndef ASMDESK_SPACE_TRAJECTORY_H
#define ASMDESK_SPACE_TRAJECTORY_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "space/types.h"

namespace asmdesk::space {

// Per-trajectory flags (a bitset). Fidelity is per-TrajPoint (Exact vs
// Statistical); these describe a trajectory as a whole.
enum TrajFlag : uint32_t {
    // A rel-basis trace: the vertices are routine-relative offsets, NOT true
    // addresses. The HUD labels it "routine-relative — not a true address-space
    // path" and the renderer never draws it as an absolute trajectory (honesty:
    // a rel trace is not a real memory trajectory).
    TRAJ_RELATIVE_BASIS = 1u << 0,
    // survey-derived statistical residency: every point is Statistical and this
    // trajectory is NEVER joined into an exact one (the honesty invariant).
    TRAJ_STATISTICAL = 1u << 1,
    // 36 T2: rel offsets PLACED against the recording's single codeimage span —
    // a DERIVED placement (base+off), not a measured absolute address. Set
    // ALONGSIDE TRAJ_RELATIVE_BASIS (the wire basis is still rel and must keep
    // saying so), so the renderer may now place these vertices and convergence
    // (T5) admits an anchored rel path where it still refuses an unanchored one.
    TRAJ_ANCHORED = 1u << 2,
};

// One trajectory: the ordered TrajPoints for a single tid (a replay recording
// is a single trajectory, tid = -1), or the statistical residency layer. PC
// vertices (is_access == false) form the path in step order; access marks
// (is_access == true) are spurs to a data cell at the same t. Points are sorted
// by (t, is_access) so a PC vertex precedes its own access mark.
struct Trajectory {
    int32_t tid = -1;
    uint32_t flags = 0;
    std::vector<TrajPoint> points;
};

// The builder result for one recording.
struct TrajectorySet {
    // The exact PC paths (one Trajectory per tid) followed by the statistical
    // residency layer (if any) — each a DISTINCT Trajectory. An exact path and
    // a statistical one are never the same object, which is how "statistical is
    // never joined into exact" is enforced structurally, not by discipline.
    std::vector<Trajectory> trajectories;

    std::string basis;        // "abs" | "rel" | "" (none, or refused)
    std::string diagnostic;   // non-empty => REFUSED: mixed / absent basis
    bool mem_present = false; // the "kind present?" gate: mem events were seen

    // 36 T2: rel PC-path anchoring against the recording's codeimage span.
    uint64_t pc_points = 0;     // PC vertices offered to the plane
    uint64_t pc_placed = 0;     // of those, how many project onto it
    bool anchored = false;      // a rel path was PLACED against the single span
    std::string placement_note; // why a rel path was not (fully) placed
    // 37 T2: HOW a rel path was placed — a real honesty grade, not cosmetics: a
    // wire-STATED base (df_step.rbase) and a DERIVED single-span anchor are
    // different claims and must not share a label. "wire" (every placed vertex
    // used its own rbase), "single-span" (36's derived anchor), "mixed" (both
    // mechanisms in one recording), "" (nothing anchored, or an abs path).
    std::string anchor_source;

    bool refused() const { return !diagnostic.empty(); }
};

// Build ordered trajectories from a recording:
//   - PC vertices from `trace` events, in step order, grouped by tid; when a
//     recording carries no `trace` (a live serve dataflow/auto session, or a
//     `--dataflow` file, emits `df_step`/`df_edge` only), the PC path falls back
//     to the `df_step` offset stream, woven region-relative (RELATIVE_BASIS) —
//     the live single-step overlay of doc 10 T5 / doc 25 T6;
//   - access marks from `mem` events (the rich rung), attached to the PC vertex
//     of their `step` — gated on the `mem` kind being present (inert until the
//     Wave-1 producer lands);
//   - statistical residency from `survey` edges, as a SEPARATE Statistical
//     trajectory that is never merged into an exact path.
// Refuses (sets `diagnostic`, empties `trajectories`) when the PC path mixes
// address bases, or an event omits `basis` — mirroring 04's canvas rule.
//
// 36 T2 — a rel PC path (df_step offsets, or a rel `trace`) is ANCHORED onto the
// absolute plane when `proj`'s regions pin the span down (exactly one codeimage
// code span): each PC vertex is rewritten to base+off, TRAJ_ANCHORED is set
// alongside TRAJ_RELATIVE_BASIS, and `anchored`/`pc_placed`/`pc_points` record
// the placement. When the span is absent or ambiguous the offsets are left
// verbatim and `placement_note` states why — never a silent drop. Pure and
// engine-free (D4).
TrajectorySet build_trajectories(const Recording &r, const Projection &proj);
// Overload — TEST / PLANE-FREE ONLY. Builds with no Projection, so a rel path is
// never anchored (offsets stay verbatim, exactly as before 36 T2, and
// placement_note carries resolve_anchor's "no codeimage code span" reason).
// Production always passes the terrain's Projection so a rel path can be placed.
TrajectorySet build_trajectories(const Recording &r);

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_TRAJECTORY_H
