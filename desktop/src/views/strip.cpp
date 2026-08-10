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

namespace {
// json field helpers, mirroring the get()-style reads the other views use
inline bool jint(const nlohmann::json &b, const char *k, int64_t *out) {
    auto it = b.find(k);
    if (it == b.end() || !it->is_number_integer())
        return false;
    *out = it->get<int64_t>();
    return true;
}
inline bool juint(const nlohmann::json &b, const char *k, uint64_t *out) {
    auto it = b.find(k);
    if (it == b.end() || !it->is_number())
        return false;
    *out = it->get<uint64_t>();
    return true;
}
inline std::string jstr(const nlohmann::json &b, const char *k) {
    auto it = b.find(k);
    return (it != b.end() && it->is_string()) ? it->get<std::string>()
                                              : std::string();
}
} // namespace

StripModel strip_build(const Recording &r,
                       const std::vector<space::Region> &regions,
                       const std::vector<StripSeam> &capture_seams) {
    (void)regions;
    (void)capture_seams;
    StripModel m;
    m.seq_end = r.event_count();
    m.torn = r.torn;
    m.end_truncated = r.end_truncated;
    m.drops_lost = r.drops_lost;
    m.drops_throttled = r.drops_throttled;

    // --- lanes: tids discovered from what the strip actually draws ----------
    // (NOT stitch.tid — a tid known only from PT slices has no strip-visible
    // events and would make an empty lane.)
    std::map<int64_t, std::vector<uint64_t>> activity; // tid → seqs
    size_t activity_events = 0;
    for (const char *kind : {"trace", "call", "watch"}) {
        auto it = r.by_kind.find(kind);
        if (it == r.by_kind.end())
            continue;
        for (const Event &e : it->second) {
            activity_events++;
            int64_t tid = -1;
            jint(e.body, "tid", &tid);
            activity[tid].push_back(e.seq);
        }
    }
    // A syscall with a tid (a v2 writer) creates a lane too — its rail tick is
    // its mark; it carries no activity count.
    if (auto it = r.by_kind.find("syscall"); it != r.by_kind.end())
        for (const Event &e : it->second) {
            int64_t tid = -1;
            if (jint(e.body, "tid", &tid))
                activity.emplace(tid, std::vector<uint64_t>{});
        }

    if (activity.empty()) {
        m.deck_reason = "no trace/call/watch events in this recording — "
                        "there is no thread activity to lane";
    } else {
        m.deck_enabled = true;
        // The LAST topo snapshot names tasks (topo.h: a snapshot is a complete
        // statement of the tree at a moment; merging several would invent a
        // tree that never existed at any one time).
        struct Task {
            long tgid = -1;
            bool leader = false;
            std::string comm;
        };
        std::map<int64_t, Task> tasks;
        if (auto it = r.by_kind.find("topo");
            it != r.by_kind.end() && !it->second.empty()) {
            const Event &last = it->second.back();
            auto ts = last.body.find("tasks");
            if (ts != last.body.end() && ts->is_array())
                for (const auto &t : *ts) {
                    int64_t tid = -1;
                    if (!jint(t, "tid", &tid))
                        continue;
                    Task k;
                    int64_t tg = -1;
                    jint(t, "tgid", &tg);
                    k.tgid = static_cast<long>(tg);
                    auto ld = t.find("leader");
                    k.leader =
                        ld != t.end() && ld->is_boolean() && ld->get<bool>();
                    k.comm = jstr(t, "comm");
                    tasks[tid] = k;
                }
        }
        // Order: known tgids first (grouped, leader first, tid asc), then
        // unknown-tgid lanes ascending tid (the -1 stream lane lands there).
        std::vector<int64_t> tids;
        tids.reserve(activity.size());
        for (auto &kv : activity)
            tids.push_back(kv.first);
        std::sort(tids.begin(), tids.end(), [&](int64_t A, int64_t B) {
            auto ka = tasks.find(A), kb = tasks.find(B);
            const bool ha = ka != tasks.end(), hb = kb != tasks.end();
            if (ha != hb)
                return ha; // known before unknown
            if (ha) {
                if (ka->second.tgid != kb->second.tgid)
                    return ka->second.tgid < kb->second.tgid;
                if (ka->second.leader != kb->second.leader)
                    return ka->second.leader; // leader first
            }
            return A < B;
        });
        {
            std::vector<long> gs;
            for (auto &kv : tasks)
                gs.push_back(kv.second.tgid);
            std::sort(gs.begin(), gs.end());
            m.multi_tgid =
                static_cast<size_t>(std::unique(gs.begin(), gs.end()) -
                                    gs.begin()) > 1;
        }
        long seen_tgid = -2;
        for (int64_t tid : tids) {
            StripLane ln;
            ln.tid = tid;
            auto k = tasks.find(tid);
            if (tid == -1) {
                ln.label = "(single stream)";
            } else if (k != tasks.end()) {
                ln.tgid = k->second.tgid;
                ln.leader = k->second.leader;
                ln.label = k->second.comm + " [" + std::to_string(tid) + "]";
                if (k->second.tgid != seen_tgid) {
                    ln.group_head = true;
                    ln.group_label = k->second.comm + " [" +
                                     std::to_string(k->second.tgid) + "]";
                    seen_tgid = k->second.tgid;
                }
            } else {
                ln.label = "[" + std::to_string(tid) + "]";
            }
            m.lanes.push_back(std::move(ln));
            auto &v = activity[tid];
            std::sort(v.begin(), v.end());
            m.lane_activity.push_back(std::move(v));
        }
    }

    // --- kernel rail: every syscall, at its OWN Event::seq ------------------
    // (Loader-assigned stream position, recording.h — no anchor approximation
    // is needed here, and the crossing layer's seq_present guard does not
    // apply: that guard is about a RECORDED per-row seq field, which the
    // strip never reads.)
    if (auto it = r.by_kind.find("syscall");
        it != r.by_kind.end() && !it->second.empty()) {
        m.rail_enabled = true;
        size_t row = 0;
        for (const Event &e : it->second) {
            StripSys s;
            s.row = row++;
            s.seq = e.seq;
            s.line = jstr(e.body, "line");
            int64_t tid = -1;
            if (jint(e.body, "tid", &tid))
                s.tid = tid;
            auto pl = e.body.find("payload");
            if (pl != e.body.end() && pl->is_string()) {
                s.has_payload = true;
                s.payload_bytes = pl->get<std::string>().size();
            }
            s.cls = syscall_class_of(syscall_name_of(s.line));
            s.outcome = syscall_outcome_of(s.line);
            if (s.tid != -1)
                for (size_t i = 0; i < m.lanes.size(); i++)
                    if (m.lanes[i].tid == s.tid) {
                        s.lane = static_cast<int>(i);
                        break;
                    }
            m.sys.push_back(std::move(s));
        }
        std::sort(m.sys.begin(), m.sys.end(),
                  [](const StripSys &a, const StripSys &b) {
                      return a.seq < b.seq;
                  });
    } else {
        m.rail_reason = "no syscall events in this recording";
    }

    // --- address bands + marks ----------------------------------------------
    for (const auto &rg : regions)
        m.bands.push_back(StripBand{rg});
    std::sort(m.bands.begin(), m.bands.end(),
              [](const StripBand &a, const StripBand &b) {
                  return a.region.base < b.region.base;
              });
    auto band_of = [&](uint64_t addr) -> int {
        for (size_t i = 0; i < m.bands.size(); i++)
            if (addr >= m.bands[i].region.base &&
                addr - m.bands[i].region.base < m.bands[i].region.len)
                return static_cast<int>(i);
        return -1;
    };
    // The rel→abs anchor: EXACTLY ONE codeimage-anchored Code band, the same
    // refusal resolve_anchor makes (space/types.h Region::from_vmmap — a
    // vmmap-named Code mapping can never anchor).
    int code_band = -1, code_bands = 0;
    for (size_t i = 0; i < m.bands.size(); i++)
        if (m.bands[i].region.kind == space::Region::Code &&
            !m.bands[i].region.from_vmmap) {
            code_band = static_cast<int>(i);
            code_bands++;
        }
    if (code_bands != 1)
        code_band = -1;
    if (m.bands.empty())
        m.bands_reason = "no regions to band — the caller assembled no "
                         "codeimage, observed-data or vmmap regions";
    else
        m.bands_enabled = true;

    if (auto it = r.by_kind.find("mem"); it != r.by_kind.end())
        for (const Event &e : it->second) {
            uint64_t ea = 0, size = 0, step = 0;
            juint(e.body, "ea", &ea);
            juint(e.body, "size", &size);
            juint(e.body, "step", &step);
            // space:"off" is region-relative with no region identity on the
            // event: counted, never placed raw (projection.cpp's own rule).
            const bool abs = jstr(e.body, "space") == "abs";
            const int band = abs ? band_of(ea) : -1;
            if (!abs || band < 0) {
                m.off_band_mem++;
                continue;
            }
            StripMemMark mk;
            mk.seq = e.seq;
            mk.addr = ea;
            mk.size = size;
            mk.step = static_cast<uint32_t>(step);
            mk.is_write = jstr(e.body, "rw") == "w";
            mk.band = band;
            m.mem.push_back(mk);
        }
    auto place_pc = [&](const Event &e, bool is_df) {
        uint64_t off = 0;
        if (!juint(e.body, "off", &off))
            return;
        uint64_t abs_addr = 0;
        bool placed = false;
        uint64_t rbase = 0;
        if (is_df && juint(e.body, "rbase", &rbase)) {
            abs_addr = rbase + off; // the df_step's own region identity
            placed = true;
        } else if (!is_df && jstr(e.body, "basis") == "abs") {
            abs_addr = off;
            placed = true;
        } else if (code_band >= 0) {
            abs_addr =
                m.bands[static_cast<size_t>(code_band)].region.base + off;
            placed = true;
        }
        const int band = placed ? band_of(abs_addr) : -1;
        if (band < 0) {
            m.off_band_pc++;
            return;
        }
        StripPcMark p;
        p.seq = e.seq;
        p.addr = abs_addr;
        p.band = band;
        int64_t tid = -1;
        if (!is_df && jint(e.body, "tid", &tid))
            p.tid = tid;
        m.pc.push_back(p);
    };
    if (auto it = r.by_kind.find("trace"); it != r.by_kind.end())
        for (const Event &e : it->second)
            place_pc(e, false);
    if (auto it = r.by_kind.find("df_step"); it != r.by_kind.end())
        for (const Event &e : it->second)
            place_pc(e, true);
    std::sort(m.mem.begin(), m.mem.end(),
              [](const StripMemMark &a, const StripMemMark &b) {
                  return a.seq < b.seq;
              });
    std::sort(m.pc.begin(), m.pc.end(),
              [](const StripPcMark &a, const StripPcMark &b) {
                  return a.seq < b.seq;
              });

    // --- the one-line honesty summary ---------------------------------------
    {
        std::string h;
        h += std::to_string(m.mem.size()) + " access(es), " +
             std::to_string(m.sys.size()) + " syscall(s), " +
             std::to_string(m.lanes.size()) + " lane(s)";
        if (m.off_band_mem)
            h += " · " + std::to_string(m.off_band_mem) +
                 " mem access(es) off-band";
        if (m.off_band_pc)
            h += " · " + std::to_string(m.off_band_pc) + " pc mark(s) off-band";
        if (m.end_truncated)
            h += " · truncated";
        if (m.drops_lost)
            h += " · ring dropped " + std::to_string(m.drops_lost) +
                 " (tail-drop)";
        if (m.drops_throttled)
            h += " · throttled";
        if (m.torn)
            h += " · torn (no footer)";
        m.hud = h;
    }
    return m;
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
