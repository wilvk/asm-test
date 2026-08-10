// strip.cpp — the pure builder, planner and camera math of strip.h. No ImGui,
// no GL, no I/O, no engine (D4).
#include "views/strip.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
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
        // Drawn rows are the POSTURE's, not the model's: kept lanes plus the
        // aggregate row when anything hides (the selection is the same pure
        // function the plan uses, so the two can never disagree).
        const StripSelection sel = strip_selected_lanes(m, v.detail);
        L.deck_rows =
            static_cast<int>(sel.keep.size() + (sel.hidden ? 1 : 0));
        const int cap = std::max(
            1, static_cast<int>((0.4f * v.px_h) / std::max(1.0f, v.lane_h)));
        L.lanes_visible = std::min<int>(L.deck_rows, cap);
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
    const StripSelection bsel = strip_selected_bands(m, v.detail);
    const size_t band_rows = bsel.keep.size() + (bsel.hidden ? 1 : 0);
    L.band_h = L.bands_h / static_cast<float>(std::max<size_t>(1, band_rows));
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

    // --- run seams: three derivations, labelled by kind ---------------------
    if (auto it = r.by_kind.find("df_invocation"); it != r.by_kind.end())
        for (const Event &e : it->second) {
            StripRunSeam s;
            s.kind = StripSeamKind::Invocation;
            s.seq = e.seq; // the marker precedes its pass's block
            uint64_t pass = 0, steps = 0;
            int64_t result = 0;
            juint(e.body, "pass", &pass);
            juint(e.body, "steps", &steps);
            jint(e.body, "result", &result);
            auto tr = e.body.find("truncated");
            s.truncated =
                tr != e.body.end() && tr->is_boolean() && tr->get<bool>();
            if (steps == 0) {
                // armed-and-waiting, not a verdict (39 T4)
                s.armed_waiting = true;
                s.label =
                    "pass " + std::to_string(pass) + " — armed, region quiet";
            } else {
                s.label = "pass " + std::to_string(pass) + " = " +
                          std::to_string(result) + ", " +
                          std::to_string(steps) + " steps";
            }
            m.seams.push_back(std::move(s));
        }
    if (auto it = r.by_kind.find("coverage"); it != r.by_kind.end())
        for (const Event &e : it->second) {
            StripRunSeam s;
            s.kind = StripSeamKind::CoverageClose;
            s.seq = e.seq + 1; // a coverage event CLOSES the block before it
            s.label = "coverage close";
            m.seams.push_back(std::move(s));
        }
    for (const StripSeam &cs : capture_seams) {
        StripRunSeam s;
        s.kind = StripSeamKind::Capture;
        s.seq = cs.seq;
        s.label = cs.label;
        m.seams.push_back(std::move(s));
    }
    std::stable_sort(m.seams.begin(), m.seams.end(),
                     [](const StripRunSeam &a, const StripRunSeam &b) {
                         return a.seq < b.seq;
                     });
    // Pass back-fill: a mem mark belongs to the last Invocation seam ≤ its
    // seq. The ordinal is the Nth MARKER in stream order (0-based), NOT the
    // marker's own `pass` field — a live union concatenates captures whose
    // `pass` fields both restart at 0, and dt_link.invocation wants the
    // segmented index's stream-order ordinal (analysis/stepindex.cpp keys
    // segments the same way).
    {
        std::vector<std::pair<uint64_t, int32_t>> inv; // seq → marker ordinal
        int32_t ord = 0;
        for (const auto &s : m.seams)
            if (s.kind == StripSeamKind::Invocation)
                inv.emplace_back(s.seq, ord++);
        for (auto &mk : m.mem) {
            auto it2 = std::upper_bound(
                inv.begin(), inv.end(),
                std::make_pair(mk.seq, std::numeric_limits<int32_t>::max()));
            mk.pass = it2 == inv.begin() ? -1 : std::prev(it2)->second;
        }
    }

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

namespace {
// One ranking shape for both selections: keep the top `cap` scorers (ties by
// `tie_lt`), returned IN MODEL ORDER; count what hides. Detail or a small
// model keeps everything — the byte-identical-below-threshold guarantee.
StripSelection select_top(const std::vector<uint64_t> &score, size_t cap,
                          bool detail,
                          const std::function<bool(size_t, size_t)> &tie_lt) {
    StripSelection s;
    const size_t n = score.size();
    if (detail || n <= cap) {
        for (size_t i = 0; i < n; i++)
            s.keep.push_back(i);
        return s;
    }
    std::vector<size_t> rank(n);
    for (size_t i = 0; i < n; i++)
        rank[i] = i;
    std::sort(rank.begin(), rank.end(), [&](size_t A, size_t B) {
        if (score[A] != score[B])
            return score[A] > score[B];
        return tie_lt(A, B);
    });
    std::vector<char> kept(n, 0);
    for (size_t i = 0; i < cap; i++)
        kept[rank[i]] = 1;
    for (size_t i = 0; i < n; i++) {
        if (kept[i])
            s.keep.push_back(i); // model order survives
        else {
            s.hidden++;
            s.hidden_events += score[i];
        }
    }
    return s;
}
} // namespace

StripSelection strip_selected_lanes(const StripModel &m, bool detail) {
    std::vector<uint64_t> score(m.lanes.size(), 0);
    for (size_t i = 0; i < m.lane_activity.size() && i < score.size(); i++)
        score[i] = m.lane_activity[i].size();
    for (const StripSys &sy : m.sys)
        if (sy.lane >= 0 && static_cast<size_t>(sy.lane) < score.size())
            score[static_cast<size_t>(sy.lane)]++;
    return select_top(score, kStripSimplifiedLanes, detail,
                      [&](size_t A, size_t B) {
                          return m.lanes[A].tid < m.lanes[B].tid;
                      });
}

StripSelection strip_selected_bands(const StripModel &m, bool detail) {
    std::vector<uint64_t> score(m.bands.size(), 0);
    for (const StripMemMark &k : m.mem)
        if (k.band >= 0 && static_cast<size_t>(k.band) < score.size())
            score[static_cast<size_t>(k.band)]++;
    for (const StripPcMark &k : m.pc)
        if (k.band >= 0 && static_cast<size_t>(k.band) < score.size())
            score[static_cast<size_t>(k.band)]++;
    return select_top(score, kStripSimplifiedBands, detail,
                      [&](size_t A, size_t B) {
                          return m.bands[A].region.base <
                                 m.bands[B].region.base;
                      });
}

uint64_t strip_plan_key(const strip_view_t &v, uint64_t model_gen) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t x) {
        h ^= x;
        h *= 1099511628211ull;
    };
    auto bits_d = [](double d) {
        uint64_t u;
        std::memcpy(&u, &d, sizeof u);
        return u;
    };
    auto bits_f = [](float f) {
        uint32_t u;
        std::memcpy(&u, &f, sizeof u);
        return static_cast<uint64_t>(u);
    };
    mix(bits_d(v.seq0));
    mix(bits_d(v.seq_per_px));
    mix(static_cast<uint64_t>(v.lane0));
    mix(bits_f(v.lane_h));
    mix(bits_f(v.px_w));
    mix(bits_f(v.px_h));
    mix(v.detail ? 1u : 0u);
    mix(model_gen);
    return h;
}

// The plan. Deterministic by construction: sorted vectors only, pixel-space
// bucketing (doc 65's lesson — never one drawable per event above the
// threshold), fixed emission order so the dump is byte-stable.
size_t strip_plan(const StripModel &m, const strip_view_t &v,
                  std::vector<strip_prim_t> *out) {
    out->clear();
    if (v.seq_per_px <= 0 || v.px_w <= 0 || v.px_h <= 0)
        return 0;
    const StripLayout L = strip_layout(m, v);
    const double lo = v.seq0;
    const double hi = v.seq0 + v.seq_per_px * static_cast<double>(v.px_w);
    auto X = [&](double seq) {
        return static_cast<float>((seq - lo) / v.seq_per_px);
    };
    auto push = [&](strip_prim k, float x0, float y0, float x1, float y1,
                    uint32_t a, uint32_t b, std::string text) {
        out->push_back(strip_prim_t{k, x0, y0, x1, y1, a, b, std::move(text)});
    };
    const bool marks = v.seq_per_px <= kStripEnvelopeSeqPerPx;
    const int cols = static_cast<int>(v.px_w);
    auto col_range = [&](const std::vector<uint64_t> &seqs, int col,
                         size_t *i0, size_t *i1) {
        const double a = lo + v.seq_per_px * col;
        const double b = a + v.seq_per_px;
        *i0 = std::lower_bound(seqs.begin(), seqs.end(),
                               static_cast<uint64_t>(std::max(0.0, a))) -
              seqs.begin();
        *i1 = std::lower_bound(seqs.begin(), seqs.end(),
                               static_cast<uint64_t>(std::max(0.0, b))) -
              seqs.begin();
    };

    // 0) the posture's selections — the same pure functions strip_layout
    //    used, so the row counts agree by construction. The aggregate lane's
    //    density source is the hidden lanes' activity, merged and sorted once.
    const StripSelection lsel = strip_selected_lanes(m, v.detail);
    const StripSelection bsel = strip_selected_bands(m, v.detail);
    std::vector<uint64_t> agg_activity;
    if (lsel.hidden) {
        std::vector<char> kept(m.lanes.size(), 0);
        for (size_t k : lsel.keep)
            kept[k] = 1;
        for (size_t i = 0; i < m.lane_activity.size(); i++)
            if (!kept[i])
                agg_activity.insert(agg_activity.end(),
                                    m.lane_activity[i].begin(),
                                    m.lane_activity[i].end());
        std::sort(agg_activity.begin(), agg_activity.end());
    }

    // 1) hud_note — the HUD line + the two pinned claims (and, in envelope
    //    mode, the density normalisation so the ribbon's scale is stated;
    //    and the simplification posture, stated ONLY when something hid —
    //    a screenshot must never pass a simplified strip off as the whole).
    std::string hud = m.hud;
    uint64_t density_max = 0;
    if (!marks && m.deck_enabled) {
        auto max_of = [&](const std::vector<uint64_t> &act) {
            for (int c = 0; c < cols; c++) {
                size_t i0, i1;
                col_range(act, c, &i0, &i1);
                density_max = std::max<uint64_t>(density_max, i1 - i0);
            }
        };
        for (size_t k : lsel.keep)
            max_of(m.lane_activity[k]);
        if (!agg_activity.empty())
            max_of(agg_activity);
        if (density_max)
            hud += " · density max " + std::to_string(density_max) + "/col";
    }
    if (lsel.hidden || bsel.hidden)
        hud += " · simplified — top " + std::to_string(lsel.keep.size()) +
               " of " + std::to_string(m.lanes.size()) + " lanes, top " +
               std::to_string(bsel.keep.size()) + " of " +
               std::to_string(m.bands.size()) + " regions";
    hud += std::string(" · ") + StripModel::axis_label() + " · " +
           StripModel::mem_tid_note();
    push(strip_prim::hud_note, 0, L.ribbon_y0, v.px_w,
         L.ribbon_y0 + L.ribbon_h, 0, 0, hud);

    // 2) channel_absent rows — a quietly absent channel is indistinguishable
    //    from one that found nothing, so the reason draws where the channel
    //    would have.
    if (!m.deck_enabled)
        push(strip_prim::channel_absent, 0, L.deck_y0, v.px_w,
             L.deck_y0 + L.deck_h, 0, 0, m.deck_reason);
    if (!m.rail_enabled)
        push(strip_prim::channel_absent, 0, L.rail_y0, v.px_w,
             L.rail_y0 + L.rail_h, 1, 0, m.rail_reason);
    if (!m.bands_enabled)
        push(strip_prim::channel_absent, 0, L.bands_y0, v.px_w,
             L.bands_y0 + L.bands_h, 2, 0, m.bands_reason);

    // 3) run_tint — boundaries are {0} ∪ seam seqs ∪ {seq_end}; the ordinal
    //    is the GLOBAL interval index so alternation is stable while panning.
    {
        std::vector<uint64_t> bounds;
        bounds.push_back(0);
        for (const auto &s : m.seams)
            bounds.push_back(s.seq);
        bounds.push_back(m.seq_end);
        for (size_t i = 0; i + 1 < bounds.size(); i++) {
            const double a = static_cast<double>(bounds[i]);
            const double b = static_cast<double>(bounds[i + 1]);
            if (b <= a || b <= lo || a >= hi)
                continue;
            push(strip_prim::run_tint, std::max(0.0f, X(a)), 0,
                 std::min(v.px_w, X(b)), v.px_h, static_cast<uint32_t>(i), 0,
                 std::string());
        }
    }

    // 4) the thread deck — rows are the POSTURE's: kept lanes in model
    //    order, then the aggregate row when anything hid. `row_of` maps a
    //    model lane index to its deck row (-1 = hidden this posture).
    if (m.deck_enabled) {
        std::vector<int> row_of(m.lanes.size(), -1);
        for (size_t r = 0; r < lsel.keep.size(); r++)
            row_of[lsel.keep[r]] = static_cast<int>(r);
        const int rows = L.deck_rows;
        const int last = std::min<int>(rows, v.lane0 + L.lanes_visible);
        auto emit_density = [&](const std::vector<uint64_t> &act, float ly,
                                uint32_t a) {
            for (int c = 0; c < cols; c++) {
                size_t i0, i1;
                col_range(act, c, &i0, &i1);
                const uint64_t n = i1 - i0;
                if (!n || !density_max)
                    continue;
                const uint32_t q = static_cast<uint32_t>(
                    (n * 255 + density_max - 1) / density_max);
                push(strip_prim::lane_density, static_cast<float>(c),
                     ly + 2.0f, static_cast<float>(c + 1),
                     ly + v.lane_h - 2.0f, a, std::min<uint32_t>(255, q),
                     std::string());
            }
        };
        for (int rrow = v.lane0; rrow < last; rrow++) {
            const float ly =
                L.deck_y0 + static_cast<float>(rrow - v.lane0) * v.lane_h;
            if (static_cast<size_t>(rrow) < lsel.keep.size()) {
                const size_t i = lsel.keep[static_cast<size_t>(rrow)];
                const auto &ln = m.lanes[i];
                // group separators only in detail: the simplified deck is a
                // flat top-N (labels still carry the comm), and a kept lane
                // whose group head hid would otherwise orphan a header
                if (v.detail && ln.group_head && m.multi_tgid)
                    push(strip_prim::group_header, 0, ly, v.px_w, ly + 2.0f,
                         static_cast<uint32_t>(i), 0, ln.group_label);
                push(strip_prim::lane_header, 0, ly, 120.0f, ly + v.lane_h,
                     static_cast<uint32_t>(i), 0, ln.label);
                if (!marks)
                    emit_density(m.lane_activity[i], ly,
                                 static_cast<uint32_t>(i));
            } else {
                // the aggregate row: everything hidden, COUNTED — its label
                // is the claim, its density the hidden lanes' sum
                push(strip_prim::lane_header, 0, ly, 160.0f, ly + v.lane_h,
                     kStripAggRow, 0,
                     "(+" + std::to_string(lsel.hidden) + " lanes, " +
                         std::to_string(lsel.hidden_events) + " events)");
                if (!marks)
                    emit_density(agg_activity, ly, kStripAggRow);
            }
        }
        // per-thread syscall ticks (only syscalls whose wire carried a tid);
        // a HIDDEN lane's syscalls still show on the rail — nothing vanishes
        for (size_t si = 0; si < m.sys.size(); si++) {
            const StripSys &s = m.sys[si];
            if (s.lane < 0 ||
                static_cast<size_t>(s.lane) >= row_of.size())
                continue;
            const int rrow = row_of[static_cast<size_t>(s.lane)];
            if (rrow < v.lane0 || rrow >= last)
                continue;
            if (static_cast<double>(s.seq) < lo ||
                static_cast<double>(s.seq) >= hi)
                continue;
            const float ly =
                L.deck_y0 + static_cast<float>(rrow - v.lane0) * v.lane_h;
            const float x = X(static_cast<double>(s.seq));
            push(strip_prim::lane_sys_tick, x, ly + 2.0f, x + 2.0f,
                 ly + v.lane_h - 2.0f, static_cast<uint32_t>(si),
                 static_cast<uint32_t>(s.cls) * 4 +
                     static_cast<uint32_t>(s.outcome),
                 std::string());
        }
    }

    // 5) the kernel rail — ticks are POINTS (2px, no along-axis extent);
    //    a column with more than kStripRailTicksPerCol shows "+N".
    if (m.rail_enabled) {
        push(strip_prim::rail_frame, 0, L.rail_y0, v.px_w,
             L.rail_y0 + L.rail_h, 0, 0, std::string());
        std::vector<uint64_t> sys_seqs;
        sys_seqs.reserve(m.sys.size());
        for (const auto &s : m.sys)
            sys_seqs.push_back(s.seq);
        for (int c = 0; c < cols; c++) {
            size_t i0, i1;
            col_range(sys_seqs, c, &i0, &i1);
            const size_t n = i1 - i0;
            if (!n)
                continue;
            const size_t shown =
                std::min<size_t>(n, static_cast<size_t>(kStripRailTicksPerCol));
            for (size_t k = 0; k < shown; k++) {
                const StripSys &s = m.sys[i0 + k];
                const float x = X(static_cast<double>(s.seq));
                push(strip_prim::rail_tick, x, L.rail_y0 + 2.0f, x + 2.0f,
                     L.rail_y0 + L.rail_h - 2.0f,
                     static_cast<uint32_t>(i0 + k),
                     static_cast<uint32_t>(s.cls) * 4 +
                         static_cast<uint32_t>(s.outcome),
                     std::string());
            }
            if (n > shown)
                push(strip_prim::rail_overflow, static_cast<float>(c),
                     L.rail_y0 + 2.0f, static_cast<float>(c) + 14.0f,
                     L.rail_y0 + L.rail_h - 2.0f,
                     static_cast<uint32_t>(i0), 0,
                     "+" + std::to_string(n - shown));
        }
    }

    // 6) address bands + marks/envelopes — rows are the posture's kept bands
    //    (base order), then the elsewhere COUNTS row when anything hid. A
    //    hidden band's addresses are never mapped into a wrong band's y:
    //    counts only, and the row label says so.
    if (m.bands_enabled) {
        std::vector<int> brow_of(m.bands.size(), -1);
        for (size_t r = 0; r < bsel.keep.size(); r++)
            brow_of[bsel.keep[r]] = static_cast<int>(r);
        auto band_y = [&](int band, uint64_t addr) {
            const auto &rg = m.bands[static_cast<size_t>(band)].region;
            const double f =
                rg.len ? static_cast<double>(addr - rg.base) /
                             static_cast<double>(rg.len)
                       : 0.0;
            const int rrow = brow_of[static_cast<size_t>(band)];
            return L.bands_y0 + static_cast<float>(rrow) * L.band_h +
                   static_cast<float>(f) * L.band_h;
        };
        for (size_t r = 0; r < bsel.keep.size(); r++) {
            const size_t bi = bsel.keep[r];
            const float by = L.bands_y0 + static_cast<float>(r) * L.band_h;
            push(strip_prim::band_frame, 0, by, v.px_w, by + L.band_h,
                 static_cast<uint32_t>(bi), 0, std::string());
            push(strip_prim::band_label, 2.0f, by, 160.0f,
                 std::min(by + 12.0f, by + L.band_h),
                 static_cast<uint32_t>(bi), 0, m.bands[bi].region.label);
            if (r > 0) {
                const auto &prev = m.bands[bsel.keep[r - 1]].region;
                if (prev.base + prev.len != m.bands[bi].region.base)
                    push(strip_prim::gap_notch, 0, by - 1.0f, v.px_w,
                         by + 1.0f, static_cast<uint32_t>(bi), 0,
                         std::string());
            }
        }
        if (bsel.hidden) {
            // the elsewhere row: label + a per-column count ribbon over the
            // hidden bands' accesses (lane_density's painter reads only `b`)
            const float by = L.bands_y0 +
                             static_cast<float>(bsel.keep.size()) * L.band_h;
            uint64_t hidden_mem = 0;
            std::vector<uint64_t> hidden_seqs;
            for (const auto &k : m.mem)
                if (k.band >= 0 && brow_of[static_cast<size_t>(k.band)] < 0) {
                    hidden_mem++;
                    hidden_seqs.push_back(k.seq);
                }
            push(strip_prim::band_frame, 0, by, v.px_w, by + L.band_h,
                 kStripAggRow, 0, std::string());
            push(strip_prim::band_label, 2.0f, by, 260.0f,
                 std::min(by + 12.0f, by + L.band_h), kStripAggRow, 0,
                 "(+" + std::to_string(bsel.hidden) + " regions — " +
                     std::to_string(hidden_mem) + " access(es), counts only)");
            uint64_t row_max = 0;
            for (int c = 0; c < cols; c++) {
                size_t i0, i1;
                col_range(hidden_seqs, c, &i0, &i1);
                row_max = std::max<uint64_t>(row_max, i1 - i0);
            }
            for (int c = 0; c < cols && row_max; c++) {
                size_t i0, i1;
                col_range(hidden_seqs, c, &i0, &i1);
                const uint64_t n = i1 - i0;
                if (!n)
                    continue;
                const uint32_t q = static_cast<uint32_t>(
                    (n * 255 + row_max - 1) / row_max);
                push(strip_prim::lane_density, static_cast<float>(c),
                     by + 12.0f, static_cast<float>(c + 1),
                     by + L.band_h - 2.0f, kStripAggRow,
                     std::min<uint32_t>(255, q), std::string());
            }
        }
        std::vector<uint64_t> mem_seqs;
        mem_seqs.reserve(m.mem.size());
        for (const auto &k : m.mem)
            mem_seqs.push_back(k.seq);
        if (marks) {
            const size_t i0 = std::lower_bound(mem_seqs.begin(),
                                               mem_seqs.end(),
                                               static_cast<uint64_t>(
                                                   std::max(0.0, lo))) -
                              mem_seqs.begin();
            for (size_t i = i0; i < m.mem.size(); i++) {
                const StripMemMark &k = m.mem[i];
                if (static_cast<double>(k.seq) >= hi)
                    break;
                if (brow_of[static_cast<size_t>(k.band)] < 0)
                    continue; // hidden band: the elsewhere row counted it
                const float x = X(static_cast<double>(k.seq));
                const float y = band_y(k.band, k.addr);
                const float h = std::min(
                    6.0f, std::max(1.0f, 1.0f + static_cast<float>(k.size) /
                                                    4.0f));
                push(strip_prim::mem_mark, x, y, x + 2.0f, y + h,
                     static_cast<uint32_t>(i), k.is_write ? 1u : 0u,
                     std::string());
            }
        } else {
            for (int c = 0; c < cols; c++) {
                size_t i0, i1;
                col_range(mem_seqs, c, &i0, &i1);
                if (i0 == i1)
                    continue;
                // per band per rw: the column's touched min..max addr
                struct Env {
                    uint64_t lo = 0, hi = 0;
                    bool any = false;
                };
                std::map<std::pair<int, bool>, Env> envs;
                for (size_t i = i0; i < i1; i++) {
                    const StripMemMark &k = m.mem[i];
                    if (brow_of[static_cast<size_t>(k.band)] < 0)
                        continue; // hidden band: counts only, never a fake y
                    Env &e = envs[{k.band, k.is_write}];
                    if (!e.any) {
                        e.lo = e.hi = k.addr;
                        e.any = true;
                    } else {
                        e.lo = std::min(e.lo, k.addr);
                        e.hi = std::max(e.hi, k.addr);
                    }
                }
                for (const auto &kv : envs) {
                    const int band = kv.first.first;
                    const float y0 = band_y(band, kv.second.lo);
                    const float y1 = band_y(band, kv.second.hi) + 1.0f;
                    push(strip_prim::mem_envelope, static_cast<float>(c), y0,
                         static_cast<float>(c + 1), y1,
                         static_cast<uint32_t>(band),
                         kv.first.second ? 1u : 0u, std::string());
                }
            }
        }
        if (marks) {
            std::vector<uint64_t> pc_seqs;
            pc_seqs.reserve(m.pc.size());
            for (const auto &k : m.pc)
                pc_seqs.push_back(k.seq);
            const size_t i0 =
                std::lower_bound(pc_seqs.begin(), pc_seqs.end(),
                                 static_cast<uint64_t>(std::max(0.0, lo))) -
                pc_seqs.begin();
            for (size_t i = i0; i < m.pc.size(); i++) {
                const StripPcMark &k = m.pc[i];
                if (static_cast<double>(k.seq) >= hi)
                    break;
                if (brow_of[static_cast<size_t>(k.band)] < 0)
                    continue; // hidden band this posture
                const float x = X(static_cast<double>(k.seq));
                const float y = band_y(k.band, k.addr);
                uint32_t lane_ord = 0; // palette hue; 0 when the tid has no
                for (size_t l = 0; l < m.lanes.size(); l++) // lane (df tid -1
                    if (m.lanes[l].tid == k.tid) {          // in a multi-tid
                        lane_ord = static_cast<uint32_t>(l); // recording)
                        break;
                    }
                push(strip_prim::pc_mark, x, y, x + 2.0f, y + 2.0f,
                     static_cast<uint32_t>(i), lane_ord, std::string());
            }
        }
    }

    // 7) run seams — full-height, labelled by their derivation
    for (size_t i = 0; i < m.seams.size(); i++) {
        const double sq = static_cast<double>(m.seams[i].seq);
        if (sq < lo || sq >= hi)
            continue;
        const float x = X(sq);
        push(strip_prim::run_seam, x, 0, x + 1.0f, v.px_h,
             static_cast<uint32_t>(i), m.seams[i].armed_waiting ? 1u : 0u,
             m.seams[i].label);
    }

    // 8) torn edge — the recorded window ended before the run did
    if (m.torn && static_cast<double>(m.seq_end) >= lo &&
        static_cast<double>(m.seq_end) < hi)
        push(strip_prim::torn_edge, X(static_cast<double>(m.seq_end)), 0,
             X(static_cast<double>(m.seq_end)) + 4.0f, v.px_h, 0, 0,
             std::string());

    return out->size();
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
    switch (p.kind) {
    case strip_prim::rail_tick:
    case strip_prim::lane_sys_tick: {
        if (p.a >= m.sys.size())
            return std::string();
        const StripSys &s = m.sys[p.a];
        std::string t = s.line + " — " + space::syscall_class_name(s.cls) +
                        ", " + space::syscall_outcome_name(s.outcome) +
                        ", seq " + std::to_string(s.seq);
        if (s.tid != -1)
            t += " [tid " + std::to_string(s.tid) + "]";
        return t;
    }
    case strip_prim::mem_mark: {
        if (p.a >= m.mem.size())
            return std::string();
        const StripMemMark &k = m.mem[p.a];
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s %lluB @ 0x%llx, seq %llu",
                      k.is_write ? "w" : "r",
                      static_cast<unsigned long long>(k.size),
                      static_cast<unsigned long long>(k.addr),
                      static_cast<unsigned long long>(k.seq));
        std::string t = buf;
        if (k.pass >= 0)
            t += " (pass " + std::to_string(k.pass) + ")";
        return t;
    }
    case strip_prim::pc_mark: {
        if (p.a >= m.pc.size())
            return std::string();
        const StripPcMark &k = m.pc[p.a];
        char buf[64];
        std::snprintf(buf, sizeof buf, "pc 0x%llx, seq %llu",
                      static_cast<unsigned long long>(k.addr),
                      static_cast<unsigned long long>(k.seq));
        std::string t = buf;
        if (k.tid != -1)
            t += " [tid " + std::to_string(k.tid) + "]";
        return t;
    }
    case strip_prim::lane_header:
        if (p.a == kStripAggRow)
            return p.text; // the aggregate row's own counted claim
        return p.a < m.lanes.size() ? m.lanes[p.a].label : std::string();
    case strip_prim::band_label:
        if (p.a == kStripAggRow)
            return p.text;
        return p.a < m.bands.size() ? m.bands[p.a].region.label
                                    : std::string();
    case strip_prim::run_seam:
        return p.a < m.seams.size() ? m.seams[p.a].label : std::string();
    default:
        return std::string();
    }
}

std::optional<dt_link> strip_click_link(const StripModel &m,
                                        const strip_prim_t &p,
                                        const std::string &rec_id) {
    if (p.kind == strip_prim::rail_tick ||
        p.kind == strip_prim::lane_sys_tick) {
        if (p.a >= m.sys.size())
            return std::nullopt;
        dt_link l;
        l.rec = rec_id;
        l.view = dt_view::syscalls;
        const StripSys &s = m.sys[p.a];
        if (s.lane >= 0 && m.lanes[static_cast<size_t>(s.lane)].tgid != -1)
            l.pid = m.lanes[static_cast<size_t>(s.lane)].tgid;
        return l;
    }
    if (p.kind == strip_prim::mem_mark) {
        if (p.a >= m.mem.size())
            return std::nullopt;
        dt_link l;
        l.rec = rec_id;
        l.view = dt_view::timeline;
        l.step = m.mem[p.a].step;
        if (m.mem[p.a].pass >= 0)
            l.invocation = static_cast<uint32_t>(m.mem[p.a].pass);
        return l;
    }
    return std::nullopt;
}

} // namespace asmdesk
