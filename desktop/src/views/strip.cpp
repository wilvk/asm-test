// strip.cpp — the pure builder, planner and camera math of strip.h. No ImGui,
// no GL, no I/O, no engine (D4).
#include "views/strip.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <map>

#include "views/syscall_classify.h"

namespace asmdesk {

void strip_view_window(const strip_view_t &v, double *lo, double *hi) {
    *lo = v.seq0;
    *hi = v.seq0 + v.seq_per_px * static_cast<double>(v.px_w);
}

void strip_view_set_window(strip_view_t &v, double lo, double hi) {
    if (lo < 0)
        lo = 0;
    if (hi <= lo || v.px_w <= 0)
        return; // an empty or inverted window is a no-op, never a div-by-zero
    v.seq0 = lo;
    v.seq_per_px = (hi - lo) / static_cast<double>(v.px_w);
}

void strip_view_follow(strip_view_t &v, uint64_t seq_end) {
    const double span = v.seq_per_px * static_cast<double>(v.px_w);
    v.seq0 = std::max(0.0, static_cast<double>(seq_end) - span);
}

int strip_view_lanes_full(const strip_view_t &v, float deck_h) {
    if (v.lane_h <= 0)
        return 0;
    return static_cast<int>(deck_h / v.lane_h);
}

int strip_view_lane_max(const strip_view_t &v, int lane_count, float deck_h) {
    const int full = strip_view_lanes_full(v, deck_h);
    return std::max(0, lane_count - full);
}

void strip_view_scroll_lanes(strip_view_t &v, int lane_count, float deck_h,
                             int delta) {
    const int mx = strip_view_lane_max(v, lane_count, deck_h);
    v.lane0 = std::min(mx, std::max(0, v.lane0 + delta));
}

// Channel heights: a disabled channel is a 14px note row (its verbatim reason
// draws there — quietly absent is indistinguishable from "nothing happened");
// the rail is a fixed 24px band; the ribbon a fixed 14px; the deck takes
// lane_h per lane up to 40% of px_h; the bands take every remaining pixel.
StripLayout strip_layout(const StripModel &m, const strip_view_t &v) {
    StripLayout L;
    const float note_h = 14.0f;
    if (m.deck_enabled) {
        const int cap = std::max(
            1, static_cast<int>((0.4f * v.px_h) / std::max(1.0f, v.lane_h)));
        L.lanes_visible = std::min<int>(static_cast<int>(m.lanes.size()), cap);
        L.deck_h = static_cast<float>(L.lanes_visible) * v.lane_h;
    } else {
        L.deck_h = note_h;
    }
    L.rail_h = m.rail_enabled ? 24.0f : note_h;
    L.ribbon_h = note_h;
    L.deck_y0 = 0;
    L.rail_y0 = L.deck_h;
    L.bands_y0 = L.rail_y0 + L.rail_h;
    L.bands_h = std::max(0.0f, v.px_h - L.deck_h - L.rail_h - L.ribbon_h);
    L.ribbon_y0 = L.bands_y0 + L.bands_h;
    L.band_h =
        L.bands_h / static_cast<float>(std::max<size_t>(1, m.bands.size()));
    return L;
}

const char *strip_prim_name(strip_prim k) {
    switch (k) {
    case strip_prim::lane_header: return "lane_header";
    case strip_prim::group_header: return "group_header";
    case strip_prim::lane_density: return "lane_density";
    case strip_prim::lane_sys_tick: return "lane_sys_tick";
    case strip_prim::rail_frame: return "rail_frame";
    case strip_prim::rail_tick: return "rail_tick";
    case strip_prim::rail_overflow: return "rail_overflow";
    case strip_prim::band_frame: return "band_frame";
    case strip_prim::band_label: return "band_label";
    case strip_prim::gap_notch: return "gap_notch";
    case strip_prim::mem_mark: return "mem_mark";
    case strip_prim::mem_envelope: return "mem_envelope";
    case strip_prim::pc_mark: return "pc_mark";
    case strip_prim::run_seam: return "run_seam";
    case strip_prim::run_tint: return "run_tint";
    case strip_prim::torn_edge: return "torn_edge";
    case strip_prim::hud_note: return "hud_note";
    case strip_prim::channel_absent: return "channel_absent";
    }
    return "?";
}

StripModel strip_build(const Recording &r,
                       const std::vector<space::Region> &regions,
                       const std::vector<StripSeam> &capture_seams) {
    (void)r;
    (void)regions;
    (void)capture_seams;
    return StripModel{}; // built test-first across Tasks 3-6
}

size_t strip_plan(const StripModel &m, const strip_view_t &v,
                  std::vector<strip_prim_t> *out) {
    (void)m;
    (void)v;
    out->clear(); // built test-first in Task 7
    return 0;
}

std::string strip_plan_dump(const std::vector<strip_prim_t> &prims) {
    std::string s;
    char buf[160];
    for (const auto &p : prims) {
        std::snprintf(buf, sizeof buf, "%s %.1f,%.1f..%.1f,%.1f a=%u b=%u %s\n",
                      strip_prim_name(p.kind), p.x0, p.y0, p.x1, p.y1, p.a, p.b,
                      p.text.c_str());
        s += buf;
    }
    return s;
}

std::string strip_hover_text(const StripModel &m, const strip_prim_t &p) {
    (void)m;
    (void)p;
    return std::string(); // Task 7
}

std::optional<dt_link> strip_click_link(const StripModel &m,
                                        const strip_prim_t &p,
                                        const std::string &rec_id) {
    (void)m;
    (void)p;
    (void)rec_id;
    return std::nullopt; // Task 7
}

} // namespace asmdesk
