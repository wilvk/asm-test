// converge.h — cross-thread convergence detection for the live-observer overlay
// (docs/internal/gui/10-spacetime-3d-overview.md T5, step 2). When two DIFFERENT
// tids place a recent PC vertex in the SAME projection-plane cell within a sliding
// step window, the two threads touched the same locality at about the same logical
// time — the "two threads on the same line" signal.
//
// It is a HINT, explicitly and always: per-tid step indices are not a global
// clock, so a convergence proves co-location, not ordering — it is never a proven
// race. That label rides in the data (ConvergenceSet::kHint / label()) so the HUD
// and the scene cannot forget to say so.
//
// Pure and engine-free (D4): only the T1 projection and the T3 trajectory model —
// no GL, no ImGui, no Unicorn/Keystone/Capstone — so it compiles into BOTH desktop
// binaries and the null test harness, the same closure projection.h and
// trajectory.h make. Detection is headless; the scene (scene3d/) draws the arcs.
#ifndef ASMDESK_SPACE_CONVERGE_H
#define ASMDESK_SPACE_CONVERGE_H

#include <cstdint>
#include <vector>

#include "space/projection.h"
#include "space/trajectory.h"

namespace asmdesk::space {

struct ConvergeParams {
    // The sliding window over trajectory step indices: two cross-tid vertices in
    // the same cell converge only when |t_a - t_b| <= window. It is a hint's
    // tolerance, not a proof's clock — widening it admits looser co-locations.
    uint64_t window = 64;
};

// One cross-thread convergence: two different tids placed a PC vertex in the same
// plane cell within the window. tid_a < tid_b (canonical), and (t_a, addr_a) /
// (t_b, addr_b) are the two vertices the scene's arc joins.
struct ConvergenceMark {
    int32_t tid_a = -1, tid_b = -1;
    uint32_t cell = 0;         // shared plane cell (y*n + x, n = 2^order)
    uint64_t t_a = 0, t_b = 0; // each thread's step index at the convergence
    uint64_t addr_a = 0,
             addr_b = 0; // the two addresses (same cell; may differ)
    uint64_t gap = 0;    // |t_a - t_b| (0 = same logical step)
};

struct ConvergenceSet {
    // One mark per (tid_a, tid_b, cell): the closest-in-time crossing there. Sorted
    // by (tid_a, tid_b, cell) for a deterministic draw and test.
    std::vector<ConvergenceMark> marks;

    // A convergence is ALWAYS a hint — ordering is not proven. Carried as data so
    // no caller can render it as a fact.
    static constexpr bool kHint = true;
    const char *label() const {
        return "convergence hint — same locality, not a proven race or order";
    }
};

// Detect cross-thread convergences over the exact per-tid trajectories in `ts`,
// placed on `proj`. Only EXACT, address-placed paths take part: a TRAJ_STATISTICAL
// residency layer (a survey is aggregate, not a per-thread path) and a
// TRAJ_RELATIVE_BASIS path (single-step region-relative offsets are labelled, not
// placed on the address plane) are skipped — a convergence over either would be a
// false address-space claim. Access-mark spurs (is_access) are not PC vertices and
// do not converge. Emits exactly one mark per (tid_a, tid_b, cell): the smallest
// |t_a - t_b| crossing there, so two threads that dwell in one cell yield one
// hint, not a flood.
ConvergenceSet detect_convergences(const TrajectorySet &ts,
                                   const Projection &proj,
                                   const ConvergeParams &params = {});

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_CONVERGE_H
