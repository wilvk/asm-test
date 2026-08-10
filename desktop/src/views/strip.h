// strip.h — the session strip: the whole session on ONE stream-order axis
// (2026-08-10 session-strip spec). Memory accesses, thread activity, kernel
// crossings and run boundaries, stacked as channels over Event::seq — the only
// ordering primitive every consumed kind carries. NOT time: seq orders events
// and measures nothing (space/crossing.h's ban), so the axis label rides in
// the model and no prim may read as a duration.
//
// The Loom split (loom/fabric_plan.h): strip_build → StripModel (pure, from a
// Recording + a caller-supplied region list + caller-supplied capture seams);
// strip_plan → pixel-space prims (pure, deterministic, byte-stable dump);
// strip_draw.cpp walks the prims. Distinct from the timeline's per-recording
// "overview strip" (doc 65) — the two never share an identifier.
#ifndef ASMDESK_VIEWS_STRIP_H
#define ASMDESK_VIEWS_STRIP_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "nav.h"
#include "space/crossing.h" // SyscallClass / SyscallOutcome
#include "space/types.h"    // space::Region

namespace asmdesk {

// ---- model -----------------------------------------------------------------

// A caller-provided capture seam (live sessions only): `seq` is the union seq
// of the FIRST event of the new capture; label uses the existing capture
// ordinal convention ("capture 2").
struct StripSeam {
    uint64_t seq = 0;
    std::string label;
};

enum class StripSeamKind { Invocation, CoverageClose, Capture };

struct StripRunSeam {
    StripSeamKind kind = StripSeamKind::Invocation;
    uint64_t seq = 0;
    std::string label;          // "pass 3 = 42, 8 steps" / "coverage close" / "capture 2"
    bool armed_waiting = false; // df_invocation steps==0 — a marker, not a verdict
    bool truncated = false;     // that pass's own truncation
};

struct StripLane {
    int64_t tid = -1; // -1 = the single-stream lane
    long tgid = -1;   // -1 = unknown (no topo task for this tid)
    bool leader = false;
    std::string label;       // "comm [tid]" / "[tid]" / "(single stream)"
    bool group_head = false; // first lane of a tgid group when >1 tgid known
    std::string group_label; // "comm [tgid]" on group_head rows, else ""
};

struct StripSys {
    size_t row = 0;   // order of appearance among syscall events
    uint64_t seq = 0;
    int64_t tid = -1; // -1 = wire carried no tid: rail only, NEVER a lane tick
    int lane = -1;    // index into lanes when tid known, else -1
    space::SyscallClass cls = space::SyscallClass::Other;
    space::SyscallOutcome outcome = space::SyscallOutcome::Unknown;
    bool has_payload = false;
    uint64_t payload_bytes = 0; // COUNT only; bytes are never copied here
    std::string line;           // payload-free by schema; safe to show
};

struct StripMemMark {
    uint64_t seq = 0;
    uint64_t addr = 0;
    uint64_t size = 0;
    bool is_write = false;
    uint32_t step = 0;  // per-PASS step (restarts each df_invocation)
    int32_t pass = -1;  // 0-based invocation-marker ordinal owning this seq
    int band = -1;      // index into bands (placed marks only)
};

struct StripPcMark {
    uint64_t seq = 0;
    uint64_t addr = 0;
    int64_t tid = -1;
    int band = -1;
};

struct StripBand {
    space::Region region; // base/len/kind/label — y maps [base, base+len) linearly
};

struct StripModel {
    std::vector<StripLane> lanes;
    // parallel to lanes: that lane's activity seqs (trace/call/watch), sorted
    std::vector<std::vector<uint64_t>> lane_activity;
    std::vector<StripSys> sys;       // sorted by seq
    std::vector<StripBand> bands;    // sorted by region base
    std::vector<StripMemMark> mem;   // placed only, sorted by seq
    std::vector<StripPcMark> pc;     // placed only, sorted by seq
    std::vector<StripRunSeam> seams; // sorted by seq (ties: input order)
    uint64_t seq_end = 0;            // r.event_count() — the axis extent
    uint32_t off_band_mem = 0;       // counted, never silently dropped
    uint32_t off_band_pc = 0;
    bool multi_tgid = false; // >1 known tgid → group separators draw

    bool deck_enabled = false;
    std::string deck_reason; // verbatim, non-empty when disabled
    bool rail_enabled = false;
    std::string rail_reason;
    bool bands_enabled = false;
    std::string bands_reason;

    // Fidelity facts, kept DISTINCT (recording.h: truncated() folds torn in;
    // the strip states each on its own terms).
    bool torn = false;           // no `end` footer
    bool end_truncated = false;  // the footer's own truncated flag
    uint64_t drops_lost = 0;     // ring tail-drop count
    bool drops_throttled = false;
    std::string hud; // the one-line honesty summary (built by strip_build)

    static const char *axis_label() { return "stream order — not time"; }
    static const char *mem_tid_note() {
        return "mem carries no tid — access marks are r/w-hued, never "
               "thread-hued";
    }
};

// Build the model. Engine-free and session-free: `regions` is the SAME list
// the 3D weave assembles (codeimage → observed spans → vmmap names), passed in
// so the strip and the 3D pane cannot disagree; `capture_seams` come from the
// live session's parts (empty for a replayed file).
StripModel strip_build(const Recording &r,
                       const std::vector<space::Region> &regions,
                       const std::vector<StripSeam> &capture_seams);

// ---- camera ----------------------------------------------------------------

struct strip_view_t {
    double seq0 = 0;
    double seq_per_px = 0; // <= 0 means "fit whole session" (resolved at draw)
    int lane0 = 0;         // deck scroll, in lanes
    float lane_h = 18.0f;
    float px_w = 800.0f, px_h = 400.0f;
    bool follow_tail = true; // reading posture, not a Settings field
};

void strip_view_window(const strip_view_t &v, double *lo, double *hi);
void strip_view_set_window(strip_view_t &v, double lo, double hi);
// Pin the window's right edge to the growing tail.
void strip_view_follow(strip_view_t &v, uint64_t seq_end);
int strip_view_lanes_full(const strip_view_t &v, float deck_h);
int strip_view_lane_max(const strip_view_t &v, int lane_count, float deck_h);
void strip_view_scroll_lanes(strip_view_t &v, int lane_count, float deck_h,
                             int delta);

// ---- vertical layout (pure) --------------------------------------------------

// Fixed stacking, top→bottom: thread deck, kernel rail, address bands, run
// ribbon. Heights are deterministic; deck_h + rail_h + bands_h + ribbon_h ==
// px_h exactly. Band heights are EQUAL — a band's height encodes nothing.
struct StripLayout {
    float deck_y0 = 0, deck_h = 0;
    float rail_y0 = 0, rail_h = 0;
    float bands_y0 = 0, bands_h = 0;
    float ribbon_y0 = 0, ribbon_h = 0;
    float band_h = 0; // bands_h / max(1, band_count)
    int lanes_visible = 0;
};
StripLayout strip_layout(const StripModel &m, const strip_view_t &v);

// ---- plan --------------------------------------------------------------------

enum class strip_prim {
    lane_header,    // a=lane
    group_header,   // a=lane (separator + group_label at a tgid boundary)
    lane_density,   // a=lane, b=quantized 0..255 intensity, one per px col
    lane_sys_tick,  // a=index into sys, b=cls*4+outcome
    rail_frame,     //
    rail_tick,      // a=index into sys, b=cls*4+outcome
    rail_overflow,  // a=first sys index in the column, text="+N"
    band_frame,     // a=band
    band_label,     // a=band
    gap_notch,      // a=band (the boundary ABOVE band a elides a gap)
    mem_mark,       // a=index into mem, b bit0 = is_write
    mem_envelope,   // a=band, b bit0 = is_write (one per px col per band per rw)
    pc_mark,        // a=index into pc, b=lane ordinal (palette hue)
    run_seam,       // a=index into seams
    run_tint,       // a=run ordinal (global parity — stable while panning)
    torn_edge,      //
    hud_note,       // the HUD line + axis label + mem tid note
    channel_absent, // a: 0=deck 1=rail 2=bands; text=verbatim reason
};
const char *strip_prim_name(strip_prim k);

struct strip_prim_t {
    strip_prim kind = strip_prim::hud_note;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t a = 0, b = 0;
    std::string text;
};

// Deterministic: same (model, view) → byte-identical vector. Individual marks
// when seq_per_px <= kStripEnvelopeSeqPerPx, per-pixel-column envelopes above
// it (the doc-65 lesson: bucket in pixel space, never one drawable per event).
inline constexpr double kStripEnvelopeSeqPerPx = 4.0;
inline constexpr int kStripRailTicksPerCol = 3;
size_t strip_plan(const StripModel &m, const strip_view_t &v,
                  std::vector<strip_prim_t> *out);
std::string strip_plan_dump(const std::vector<strip_prim_t> &prims);

// ---- hover / drill-in (pure) ---------------------------------------------------

std::string strip_hover_text(const StripModel &m, const strip_prim_t &p);
// rail_tick → the syscalls view (pid set when the tick's tid maps to a known
// tgid); mem_mark → the timeline at that step (invocation set when the mark
// falls inside a df_invocation pass). Everything else: nullopt (hover only).
std::optional<dt_link> strip_click_link(const StripModel &m,
                                        const strip_prim_t &p,
                                        const std::string &rec_id);

// ---- per-recording UI state (shell-owned) --------------------------------------

struct StripState {
    StripModel model;
    strip_view_t cam;
    bool built = false;
};

} // namespace asmdesk
#endif // ASMDESK_VIEWS_STRIP_H
