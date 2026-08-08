// focus.h — the SUBJECT filter of 51-scene-focus-and-scale.md (G11): which
// thread, which region, which kinds is the reader actually looking at. Every
// existing SceneLayers bool is a *class* switch (terrain / exact / statistical
// / ...); nothing in the scene could say "this thread" or "only the heap", so
// on a multi-thread capture every path is drawn at equal weight and the one
// you care about is somewhere in the bundle.
//
// TWO AXES, never one (T3 step 3): SceneLayers grades EVIDENCE (what was
// recorded, and how well); SceneFocus chooses SUBJECT (which thread / region /
// kind you are reading right now). A subject filter is a PRESENTATION channel
// only — alpha and saturation — and never touches a height, a TerrainFlag or a
// TrajectorySet. It reduces salience; it never removes a mark, because absence
// in this app means "not recorded" and a filter must not be able to say that
// on the recording's behalf.
//
// Pure and engine-free (D4): the standard library and the pure space/ models
// only — no GL, no ImGui — so scene.cpp (GL, no ImGui), hud.cpp (ImGui, no GL)
// and a headless test TU all read the SAME rules rather than three copies.
#ifndef ASMDESK_SCENE3D_FOCUS_H
#define ASMDESK_SCENE3D_FOCUS_H

#include <cstdint>
#include <string>
#include <vector>

#include "space/projection.h"
#include "space/trajectory.h"
#include "space/types.h"

namespace asmdesk::scene3d {

// Every Region::Kind admitted (bit i == Region::Kind i). Only bits 0..5 are
// meaningful today (Code, Stack, Heap, Data, Mmap, Unknown); the high bits are
// kept set so a kind added to Region::Kind defaults to VISIBLE rather than
// silently filtered out the day it lands.
inline constexpr uint32_t kAllKinds = 0xFFu;

// How much of its own alpha a non-subject worldline keeps. GHOST, NEVER HIDE
// (this brief's first fidelity note): a hidden path claims the thread did not
// run; a ghosted one says "still there, not the subject". Same reasoning as
// 49's dim-not-discard for the time cut.
inline constexpr float kGhostAlpha = 0.28f;

// The draw-time subject filter. Every field is a per-frame value the caller
// sets before render() — like Scene::follow_step / Scene::highlight_cell — so
// NO code path here writes to TerrainModel, Terrain::flags or TrajectorySet
// (this brief's "filters are draw-time only" invariant, which the tests assert
// as byte-equality on the model either side of a filter round trip).
struct SceneFocus {
    // The focused thread's tid, or -1 for "no thread is the subject" (the
    // default, in which every worldline is drawn exactly as it is today).
    int32_t tid = -1;
    // An index into Projection::regions, or -1 for "no region is the subject".
    //
    // An ORDINAL, and Projection::regions is rebuilt on every weave — so this
    // must be re-resolved through reresolve_region() after one, never merely
    // bounds-checked. See region_base/region_len below.
    int32_t region = -1;
    // The (base,len) `region` was chosen for — its IDENTITY, which survives a
    // weave that its ordinal does not. Write both whenever `region` is set to a
    // real index; 0/0 means "never set" and resolves to nothing.
    uint64_t region_base = 0, region_len = 0;
    // Bit per Region::Kind; kAllKinds == no kind filter.
    uint32_t kind_mask = kAllKinds;
    // 51 T4 ONLY: the distance budget's MID tier drops non-subject worldlines
    // rather than ghosting them. This is the one place the "ghost, never hide"
    // rule yields, and it does so under the degrade discipline instead: the
    // tier is announced on screen and names this class verbatim (lod.h's
    // lod_placard). Never set by a subject filter on its own.
    bool drop_unfocused = false;

    bool filtering() const {
        return tid >= 0 || region >= 0 || kind_mask != kAllKinds;
    }
};

// Resolve a region by IDENTITY rather than ordinal: the index of the region
// with exactly this (base,len), or -1 when none has it.
//
// Projection::regions is rebuilt from scratch on every weave, and a growing live
// capture gains regions as it runs — so any index held ACROSS a weave
// (SceneFocus::region, HudState::goto_region_sel) silently names a different
// region afterwards. A bounds check cannot catch that, which is exactly why the
// bug survived: the stale index is in range, it is just pointing somewhere else.
//
// A caller re-resolves after each weave and treats -1 as "the subject went away,
// clear it" — never as "fall back to region 0", which would be the same defect
// wearing a different number. The 0/0 never-set sentinel resolves to -1 because
// no real region has zero length.
int32_t reresolve_region(const std::vector<space::Region> &regions,
                         uint64_t base, uint64_t len);

// The per-trajectory colour palette. Lives here (pure) rather than in
// scene.cpp so the HUD roster's swatch and the drawn worldline are ONE table,
// not two copies that could drift — unlike overlay_encoding_swatches(), which
// has to mirror scene.cpp's literals because hud.cpp cannot include a GL TU.
// `idx` is the trajectory's tid when it has one, else its ordinal in
// TrajectorySet::trajectories (scene.cpp's own `tid_seq` fallback).
void tid_palette(int idx, float out[4]);

// The alpha MULTIPLIER for one worldline under this focus. 1.0 whenever
// nothing is focused, so a default SceneFocus leaves every draw call
// byte-identical to the pre-51 render.
//
// The statistical residency trajectory is NEVER a subject and is never ghosted
// for one: a survey is a whole-recording aggregate, not a per-thread path, and
// dimming it "because you focused thread 7" would imply it belongs to some
// other thread. It keeps its normal weight at every focus setting (the same
// admission reasoning converge.h states for convergence).
float focus_line_alpha(const SceneFocus &f, int32_t line_tid, bool statistical);

// One row of the HUD thread roster (T1 step 4). Pure data, so the row list is
// golden-testable with no ImGui frame — including the row a tid with no drawn
// path still gets.
struct ThreadRosterRow {
    int32_t tid = -1;
    bool statistical = false; // the survey aggregate, never a thread
    bool focusable = true;    // false for the statistical row (see above)
    uint64_t vertices = 0;    // PC vertices this trajectory offered the plane
    uint64_t placed = 0;      // of those, how many the projection placed
    // Does the renderer actually draw a strip for this trajectory? Mirrors
    // scene.cpp's own admission test (a strip needs at least two PLACED,
    // PROJECTED vertices) so the roster can never claim a path the scene does
    // not draw, nor omit one it does.
    bool has_placed_path = false;
    // Verbatim, never omission: "present, no placed path" for a trajectory the
    // recording carries but the plane cannot draw. Empty when the path draws.
    std::string note;
    float rgb[3] = {1.0f, 1.0f, 1.0f}; // the swatch, from tid_palette
};

// The roster for a weave. Takes the SAME (TrajectorySet, Projection) pair
// Scene::set_trajectories takes, and replays its admission test in lockstep,
// so "does this thread have a path on screen" is answered once, not twice.
std::vector<ThreadRosterRow> thread_roster(const space::TrajectorySet &ts,
                                           const space::Projection &proj);

// The per-cell subject mask a focused REGION needs: 1 where the cell belongs
// to `region_index`, 0 elsewhere; sized (1<<proj.order)^2, row-major like
// TerrainModel::kind_by_cell. Empty when `region_index` is out of range (the
// caller then draws unfiltered rather than an all-zero plane — a filter that
// hid everything would be a lie about the recording, not a filter).
//
// Membership comes from space::region_cells() — the compacted-domain range
// walk, the ONE rule for "which cells are this region's".
std::vector<uint8_t> build_focus_mask(const space::Projection &proj,
                                      int32_t region_index);

// The HUD's statement that a filter is ON and what it is dimming — a filtered
// plane that looks like an unfiltered one is a false reading of the recording
// (T2 step 5), so this is drawn in the warn colour whenever it is non-empty,
// exactly as scrub_degrade_note() labels the coarse rung. Empty string when
// nothing is filtered.
std::string subject_filter_note(const SceneFocus &f,
                                const space::Projection &proj);

// T3 step 3: the one line that keeps the two axes apart in the reader's head.
// A single source of truth between the draw call and the test that the wording
// did not silently drift or vanish (vertical_axes_note()'s own pattern).
const char *focus_axis_note();

// The legend name for a Region::Kind, for the kind checkboxes — routed through
// space::region_style() so the filter UI and the plane share ONE palette and
// ONE set of names (T2 step 4).
const char *kind_label(space::Region::Kind k);

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_FOCUS_H
