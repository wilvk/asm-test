// test_strip_model.cpp — the session strip's pure closure: camera math,
// layout, build, plan. No ImGui, no GL, no engine (test_scene2d.cpp idiom).
#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "views/strip.h"

using namespace asmdesk;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

static void camera_math() {
    strip_view_t v;
    v.px_w = 800;
    v.seq0 = 100;
    v.seq_per_px = 2.0;
    double lo = 0, hi = 0;
    strip_view_window(v, &lo, &hi);
    check("window: lo", lo == 100.0, "lo is seq0");
    check("window: hi", hi == 100.0 + 2.0 * 800, "hi is seq0 + seq_per_px*px_w");
    strip_view_set_window(v, 50, 850);
    strip_view_window(v, &lo, &hi);
    check("set_window round-trips", lo == 50.0 && hi == 850.0,
          "set then get must return the same window");
    check("set_window zoom", v.seq_per_px == (850.0 - 50.0) / 800.0, "");
    strip_view_set_window(v, -10, 790);
    strip_view_window(v, &lo, &hi);
    check("set_window clamps lo to 0", lo == 0.0, "no negative stream position");

    v.seq_per_px = 1.0;
    strip_view_follow(v, 10000);
    strip_view_window(v, &lo, &hi);
    check("follow pins tail", hi == 10000.0,
          "follow puts seq_end at the right edge");
    strip_view_follow(v, 10); // shorter than a window
    check("follow clamps at 0", v.seq0 == 0.0, "never a negative origin");

    strip_view_t d;
    d.lane_h = 18;
    check("lanes_full", strip_view_lanes_full(d, 90.0f) == 5, "90/18");
    check("lane_max small deck", strip_view_lane_max(d, 3, 90.0f) == 0,
          "3 lanes fit in 5 rows: no scroll");
    check("lane_max big deck", strip_view_lane_max(d, 12, 90.0f) == 7,
          "12 lanes, 5 visible: max lane0 is 7");
    d.lane0 = 0;
    strip_view_scroll_lanes(d, 12, 90.0f, 100);
    check("scroll clamps high", d.lane0 == 7, "");
    strip_view_scroll_lanes(d, 12, 90.0f, -100);
    check("scroll clamps low", d.lane0 == 0, "");
}

static void layout_sums() {
    StripModel m;
    m.deck_enabled = true;
    m.rail_enabled = true;
    m.bands_enabled = true;
    m.lanes.resize(3);
    m.bands.resize(2);
    strip_view_t v;
    v.px_h = 400;
    v.lane_h = 18;
    StripLayout L = strip_layout(m, v);
    check("layout: channels sum to px_h",
          L.deck_h + L.rail_h + L.bands_h + L.ribbon_h == v.px_h,
          "no unowned pixels");
    check("layout: order", L.deck_y0 == 0 && L.rail_y0 == L.deck_h &&
                               L.bands_y0 == L.rail_y0 + L.rail_h &&
                               L.ribbon_y0 == L.bands_y0 + L.bands_h,
          "deck, rail, bands, ribbon — top to bottom");
    check("layout: equal band heights", L.band_h == L.bands_h / 2.0f,
          "a band's height encodes nothing");
    check("layout: deck fits its lanes", L.deck_h == 3 * v.lane_h,
          "3 lanes need no cap at 400px");

    StripModel none;
    none.deck_enabled = false;
    none.deck_reason = "x";
    none.rail_enabled = false;
    none.rail_reason = "x";
    none.bands_enabled = false;
    none.bands_reason = "x";
    StripLayout L2 = strip_layout(none, v);
    check("layout: disabled channels still sum",
          L2.deck_h + L2.rail_h + L2.bands_h + L2.ribbon_h == v.px_h,
          "absent channels shrink to note rows; the sum invariant holds");
}

static const char *kHdr =
    R"({"asmtrace":1,"container":"ndjson","producer":{"name":"asmtrace_record","version":"1.1.0"},"provenance":{"backend":"emu-l0","exact":true,"trust":"exact"},"arch":"x86_64"})";

static Recording mk_rec(std::initializer_list<const char *> lines) {
    std::string nd = std::string(kHdr) + "\n";
    for (const char *l : lines) {
        nd += l;
        nd += "\n";
    }
    std::istringstream in(nd);
    std::string err;
    auto rec = load_recording(in, err);
    if (!rec) {
        fail("load recording", err);
        return Recording{};
    }
    return *rec;
}

static void lanes_discovery_and_grouping() {
    // Two processes (tgid 10: tids 10,11; tgid 20: tid 20) plus a tid no topo
    // task names (30): discovered from tid-bearing events, labelled from the
    // LAST topo snapshot only.
    Recording r = mk_rec({
        R"({"k":"trace","basis":"abs","off":4096,"tid":10})",
        R"({"k":"trace","basis":"abs","off":4100,"tid":11})",
        R"({"k":"trace","basis":"abs","off":4104,"tid":20})",
        R"({"k":"trace","basis":"abs","off":4108,"tid":30})",
        R"({"k":"topo","mode":"syscalls","tasks":[{"tid":99,"tgid":99,"ppid":1,"leader":true,"comm":"stale","exe":"stale","inv":1}]})",
        R"({"k":"topo","mode":"syscalls","tasks":[{"tid":10,"tgid":10,"ppid":1,"leader":true,"comm":"alpha","exe":"alpha","inv":3},{"tid":11,"tgid":10,"ppid":1,"leader":false,"comm":"alpha-w","exe":"","inv":2},{"tid":20,"tgid":20,"ppid":10,"leader":true,"comm":"beta","exe":"beta","inv":1}]})",
        R"({"k":"end","events":6})",
    });
    StripModel m = strip_build(r, {}, {});
    check("deck enabled", m.deck_enabled, "tid-bearing events exist");
    check("four lanes", m.lanes.size() == 4, "tids 10, 11, 20, 30");
    check("lane order (tgid, leader, tid; unknown last)",
          m.lanes.size() == 4 && m.lanes[0].tid == 10 && m.lanes[1].tid == 11 &&
              m.lanes[2].tid == 20 && m.lanes[3].tid == 30,
          "grouped by tgid; leader first inside a group; unlabelled tids last");
    check("labels from LAST topo snapshot",
          m.lanes.size() == 4 && m.lanes[0].label == "alpha [10]" &&
              m.lanes[2].label == "beta [20]",
          "the stale first snapshot must not label anything");
    check("unknown-tgid label is bare tid",
          m.lanes.size() == 4 && m.lanes[3].label == "[30]" &&
              m.lanes[3].tgid == -1,
          "a tid with no topo task never gets a guessed comm");
    check("multi tgid flags grouping", m.multi_tgid, "two known tgids");
    check("group heads",
          m.lanes.size() == 4 && m.lanes[0].group_head &&
              m.lanes[2].group_head && !m.lanes[1].group_head,
          "first lane of each tgid group");
    check("group label",
          m.lanes.size() == 4 && m.lanes[0].group_label == "alpha [10]",
          "comm [tgid] on the head row");
    check("activity recorded per lane",
          m.lane_activity.size() == 4 && m.lane_activity[0].size() == 1,
          "one trace event for tid 10");
    check("seq_end covers the stream", m.seq_end == r.event_count(),
          "the axis extent is the whole stream");
}

static void lanes_single_stream_and_unknown() {
    Recording r = mk_rec({
        R"({"k":"trace","basis":"rel","off":0})",
        R"({"k":"trace","basis":"rel","off":4})",
        R"({"k":"end","events":2})",
    });
    StripModel m = strip_build(r, {}, {});
    check("single-stream lane", m.lanes.size() == 1 && m.lanes[0].tid == -1,
          "no tids anywhere → ONE lane, never hidden");
    check("single-stream label",
          m.lanes.size() == 1 && m.lanes[0].label == "(single stream)", "");
    check("no grouping", !m.multi_tgid, "");

    Recording bare =
        mk_rec({R"({"k":"note","text":"x"})", R"({"k":"end","events":1})"});
    StripModel mb = strip_build(bare, {}, {});
    check("deck disabled without activity", !mb.deck_enabled,
          "no trace/call/watch events at all");
    check("deck reason verbatim",
          mb.deck_reason ==
              "no trace/call/watch events in this recording — there is no "
              "thread activity to lane",
          "a quietly absent channel is indistinguishable from one that found "
          "nothing");
}

static void rail_rows() {
    Recording r = mk_rec({
        R"({"k":"trace","basis":"abs","off":4096,"tid":10})",
        R"({"k":"syscall","line":"[10] openat(AT_FDCWD, <path>) = 3","tid":10})",
        R"({"k":"syscall","line":"write(1, <14 bytes>) = -9","payload":"AAAABBBBCCCCDD"})",
        R"({"k":"syscall","line":"zzz_mystery(1) = ?"})",
        R"({"k":"end","events":4})",
    });
    StripModel m = strip_build(r, {}, {});
    check("rail enabled", m.rail_enabled, "syscall events exist");
    check("three rows", m.sys.size() == 3, "");
    check("rows sorted by seq",
          m.sys.size() == 3 && m.sys[0].seq < m.sys[1].seq &&
              m.sys[1].seq < m.sys[2].seq,
          "");
    check("class from shared parse",
          m.sys.size() == 3 && m.sys[0].cls == space::SyscallClass::File &&
              m.sys[2].cls == space::SyscallClass::Other,
          "openat → File; a miss stays in the visible grey bucket");
    check("outcome from shared parse",
          m.sys.size() == 3 &&
              m.sys[0].outcome == space::SyscallOutcome::Ok &&
              m.sys[1].outcome == space::SyscallOutcome::Error &&
              m.sys[2].outcome == space::SyscallOutcome::Unknown,
          "");
    check("tid-ful syscall maps to its lane",
          m.sys.size() == 3 && m.sys[0].tid == 10 && m.sys[0].lane == 0,
          "a v2 writer's tid joins the thread lane");
    check("tid-less syscall is rail-only",
          m.sys.size() == 3 && m.sys[1].tid == -1 && m.sys[1].lane == -1,
          "v1 writers omit tid — never guessed into a lane");
    check("payload counted, never copied",
          m.sys.size() == 3 && m.sys[1].has_payload &&
              m.sys[1].payload_bytes == 14 &&
              m.sys[1].line.find("AAAA") == std::string::npos,
          "count only; the line is the payload-free rendering");

    Recording none = mk_rec(
        {R"({"k":"trace","basis":"rel","off":0})", R"({"k":"end","events":1})"});
    StripModel mn = strip_build(none, {}, {});
    check("rail disabled without syscalls", !mn.rail_enabled, "");
    check("rail reason verbatim",
          mn.rail_reason == "no syscall events in this recording",
          "stated, never quietly absent");
}

static std::vector<space::Region> two_bands() {
    space::Region code;
    code.base = 0x1000;
    code.len = 0x1000;
    code.kind = space::Region::Code;
    code.label = "code";
    space::Region data;
    data.base = 0x200000;
    data.len = 0x10000;
    data.kind = space::Region::Data;
    data.label = "observed data";
    return {code, data};
}

static void bands_and_marks() {
    Recording r = mk_rec({
        R"({"k":"trace","basis":"rel","off":16,"tid":10})",
        R"({"k":"df_step","step":0,"off":20,"rbase":4096,"ops":[]})",
        R"({"k":"mem","step":0,"ea":2097160,"size":8,"rw":"w","space":"abs"})",
        R"({"k":"mem","step":0,"ea":16,"size":4,"rw":"r","space":"off"})",
        R"({"k":"mem","step":0,"ea":999999999,"size":8,"rw":"r","space":"abs"})",
        R"({"k":"end","events":5})",
    });
    StripModel m = strip_build(r, two_bands(), {});
    check("bands enabled", m.bands_enabled, "regions were passed");
    check("bands sorted by base",
          m.bands.size() == 2 && m.bands[0].region.base == 0x1000, "");
    check("abs mem placed",
          m.mem.size() == 1 && m.mem[0].band == 1 && m.mem[0].is_write &&
              m.mem[0].addr == 2097160,
          "ea 0x200008 lands in the data band");
    check("off-space and off-band mem COUNTED", m.off_band_mem == 2,
          "space:\"off\" is counted, never placed raw; an unmapped abs "
          "address is counted too");
    check("rel pc placed via the single Code band",
          !m.pc.empty() && m.pc[0].band == 0 && m.pc[0].addr == 0x1000 + 16,
          "basis:rel resolves against the ONE Code band");
    check("df_step pc placed via rbase",
          m.pc.size() == 2 && m.pc[1].addr == 4096 + 20,
          "rbase+off, the df_step's own region identity");
    check("pc marks keep tid",
          m.pc.size() == 2 && m.pc[0].tid == 10 && m.pc[1].tid == -1,
          "df_step never has a tid");
    check("hud states the counts",
          m.hud.find("2 mem access(es) off-band") != std::string::npos,
          "counted facts are stated, not dropped");

    StripModel nb = strip_build(r, {}, {});
    check("no regions → bands disabled", !nb.bands_enabled, "");
    check("bands reason verbatim",
          nb.bands_reason ==
              "no regions to band — the caller assembled no codeimage, "
              "observed-data or vmmap regions",
          "");
    check("disabled bands still count", nb.off_band_mem == 3 && nb.pc.empty(),
          "nothing places when no band exists; everything is counted");
}

static void run_seams() {
    Recording r = mk_rec({
        R"({"k":"df_invocation","pass":0,"result":42,"steps":8,"truncated":false})",
        R"({"k":"df_step","step":0,"off":0,"ops":[]})",
        R"({"k":"df_invocation","pass":1,"result":0,"steps":0,"truncated":false})",
        R"({"k":"trace","basis":"rel","off":0})",
        R"({"k":"coverage","basis":"rel","blocks":[0]})",
        R"({"k":"end","events":5})",
    });
    StripModel m = strip_build(r, {}, {{3, "capture 2"}});
    check("three derivations present", m.seams.size() == 4,
          "2 invocation + 1 coverage-close + 1 capture");
    check("seams sorted by seq",
          std::is_sorted(m.seams.begin(), m.seams.end(),
                         [](const StripRunSeam &a, const StripRunSeam &b) {
                             return a.seq < b.seq;
                         }),
          "");
    check("invocation label carries pass/result/steps",
          !m.seams.empty() && m.seams[0].kind == StripSeamKind::Invocation &&
              m.seams[0].label == "pass 0 = 42, 8 steps",
          "result is a NUMBER (routine return), not a status word");
    check("steps:0 is armed-and-waiting, not a verdict",
          m.seams.size() > 1 && m.seams[1].armed_waiting &&
              m.seams[1].label == "pass 1 — armed, region quiet",
          "39 T4");
    check("capture seam verbatim",
          [&] {
              for (auto &s : m.seams)
                  if (s.kind == StripSeamKind::Capture)
                      return s.label == std::string("capture 2");
              return false;
          }(),
          "the caller's ordinal label passes through");
    check("coverage closes the block BEFORE it",
          [&] {
              for (auto &s : m.seams)
                  if (s.kind == StripSeamKind::CoverageClose)
                      return s.seq > 0;
              return false;
          }(),
          "seam sits after the closer");

    // pass back-fill: same step value, different pass — the marker ordinal is
    // the discriminator (stepindex keys segments the same stream-order way)
    Recording r2 = mk_rec({
        R"({"k":"df_invocation","pass":0,"result":1,"steps":2,"truncated":false})",
        R"({"k":"mem","step":1,"ea":4200,"size":8,"rw":"w","space":"abs"})",
        R"({"k":"df_invocation","pass":1,"result":1,"steps":2,"truncated":false})",
        R"({"k":"mem","step":0,"ea":4208,"size":8,"rw":"w","space":"abs"})",
        R"({"k":"end","events":4})",
    });
    StripModel m2 = strip_build(r2, two_bands(), {});
    check("mem pass back-fill",
          m2.mem.size() == 2 && m2.mem[0].pass == 0 && m2.mem[1].pass == 1,
          "same step value, different pass — the marker is the discriminator");
}

static void plan_determinism_and_modes() {
    Recording r = mk_rec({
        R"({"k":"df_invocation","pass":0,"result":42,"steps":8,"truncated":false})",
        R"({"k":"trace","basis":"rel","off":16,"tid":10})",
        R"({"k":"syscall","line":"openat(AT_FDCWD, <path>) = 3","tid":10})",
        R"({"k":"mem","step":0,"ea":4200,"size":8,"rw":"w","space":"abs"})",
        R"({"k":"end","events":4})",
    });
    StripModel m = strip_build(r, two_bands(), {});
    strip_view_t v;
    v.px_w = 400;
    v.px_h = 300;
    v.seq_per_px = 0.05; // mark mode (≤ 4)
    std::vector<strip_prim_t> a, b;
    strip_plan(m, v, &a);
    strip_plan(m, v, &b);
    check("plan deterministic", strip_plan_dump(a) == strip_plan_dump(b),
          "same (model, view) → byte-identical plans");
    auto has = [&](strip_prim k) {
        for (auto &p : a)
            if (p.kind == k)
                return true;
        return false;
    };
    check("mark mode emits marks",
          has(strip_prim::mem_mark) && has(strip_prim::rail_tick) &&
              has(strip_prim::pc_mark),
          "");
    check("hud carries the pinned notes",
          [&] {
              for (auto &p : a)
                  if (p.kind == strip_prim::hud_note)
                      return p.text.find(StripModel::axis_label()) !=
                                 std::string::npos &&
                             p.text.find(StripModel::mem_tid_note()) !=
                                 std::string::npos;
              return false;
          }(),
          "the axis claim and the no-tid claim ride in the plan itself");
    check("run seam emitted with label",
          [&] {
              for (auto &p : a)
                  if (p.kind == strip_prim::run_seam)
                      return p.text == "pass 0 = 42, 8 steps";
              return false;
          }(),
          "");
    check("syscall tick is a POINT",
          [&] {
              for (auto &p : a)
                  if (p.kind == strip_prim::rail_tick)
                      return (p.x1 - p.x0) <= 2.0f;
              return true;
          }(),
          "no along-axis extent a reader could mistake for a duration");
    check("rail tick carries class+outcome in b",
          [&] {
              for (auto &p : a)
                  if (p.kind == strip_prim::rail_tick)
                      return p.b ==
                             static_cast<uint32_t>(space::SyscallClass::File) *
                                     4 +
                                 static_cast<uint32_t>(
                                     space::SyscallOutcome::Ok);
              return false;
          }(),
          "the painter has no model; hue rides in the prim");

    strip_view_t vz = v;
    vz.seq_per_px = 100.0; // envelope mode
    std::vector<strip_prim_t> c;
    strip_plan(m, vz, &c);
    bool env_ok = true;
    for (auto &p : c)
        if (p.kind == strip_prim::mem_mark || p.kind == strip_prim::pc_mark)
            env_ok = false;
    check("envelope mode has no per-event marks", env_ok,
          "the doc-65 lesson: never one drawable per event above threshold");
    check("envelope mode emits envelopes/density",
          [&] {
              bool env = false, den = false;
              for (auto &p : c) {
                  if (p.kind == strip_prim::mem_envelope)
                      env = true;
                  if (p.kind == strip_prim::lane_density)
                      den = true;
              }
              return env && den;
          }(),
          "aggregation replaces marks; the channels do not go blank");

    // hover + click
    for (auto &p : a)
        if (p.kind == strip_prim::rail_tick) {
            check("rail hover text",
                  strip_hover_text(m, p).find("openat") != std::string::npos,
                  "");
            auto lk = strip_click_link(m, p, "rec-x");
            check("rail click links to syscalls view",
                  lk.has_value() && lk->view == dt_view::syscalls &&
                      lk->rec == "rec-x",
                  "");
        }
    for (auto &p : a)
        if (p.kind == strip_prim::mem_mark) {
            auto lk = strip_click_link(m, p, "rec-x");
            check("mem click links to timeline step+invocation",
                  lk.has_value() && lk->view == dt_view::timeline &&
                      lk->step.has_value() && *lk->step == 0 &&
                      lk->invocation.has_value() && *lk->invocation == 0,
                  "step is per-pass; the invocation ordinal disambiguates");
        }
}

static void plan_stable_tint_parity() {
    Recording r = mk_rec({
        R"({"k":"df_invocation","pass":0,"result":1,"steps":1,"truncated":false})",
        R"({"k":"trace","basis":"rel","off":0})",
        R"({"k":"df_invocation","pass":1,"result":1,"steps":1,"truncated":false})",
        R"({"k":"trace","basis":"rel","off":4})",
        R"({"k":"end","events":4})",
    });
    StripModel m = strip_build(r, two_bands(), {});
    strip_view_t v;
    v.px_w = 100;
    v.px_h = 300;
    v.seq_per_px = 0.05;
    v.seq0 = 3.0; // window shows only the SECOND run
    std::vector<strip_prim_t> p;
    strip_plan(m, v, &p);
    bool saw_tint = false;
    for (auto &q : p)
        if (q.kind == strip_prim::run_tint) {
            saw_tint = true;
            check("tint ordinal is global", q.a == 2,
                  "interval index over ALL seams (0: pre-pass0, 1: pass0, "
                  "2: pass1) — parity stable while panning");
        }
    check("a tint interval was emitted", saw_tint, "");
}

static StripModel big_model() {
    StripModel m;
    for (int i = 0; i < 12; i++) {
        StripLane ln;
        ln.tid = 100 + i;
        ln.label = "[" + std::to_string(100 + i) + "]";
        m.lanes.push_back(ln);
        // lane i gets i activity events (lane 11 busiest, lane 0 silent)
        std::vector<uint64_t> act;
        for (int k = 0; k < i; k++)
            act.push_back(static_cast<uint64_t>(k));
        m.lane_activity.push_back(act);
    }
    for (int b = 0; b < 9; b++) {
        StripBand bd;
        bd.region.base = 0x1000u * static_cast<uint64_t>(b + 1);
        bd.region.len = 0x1000;
        bd.region.label = "r" + std::to_string(b);
        m.bands.push_back(bd);
    }
    // band b gets b placed mem marks (band 8 busiest)
    for (int b = 0; b < 9; b++)
        for (int k = 0; k < b; k++) {
            StripMemMark mk;
            mk.seq = static_cast<uint64_t>(k);
            mk.addr = 0x1000u * static_cast<uint64_t>(b + 1) +
                      static_cast<uint64_t>(k);
            mk.band = b;
            m.mem.push_back(mk);
        }
    std::sort(m.mem.begin(), m.mem.end(),
              [](const StripMemMark &a, const StripMemMark &b) {
                  return a.seq < b.seq;
              });
    m.deck_enabled = true;
    m.bands_enabled = true;
    m.seq_end = 16;
    return m;
}

static void simplified_selection() {
    StripModel m = big_model();
    StripSelection ls = strip_selected_lanes(m, false);
    check("lanes: top 8 kept", ls.keep.size() == 8 && ls.hidden == 4,
          "12 lanes, threshold 8");
    check("lanes: busiest kept, model order",
          ls.keep.size() == 8 && ls.keep.front() == 4 && ls.keep.back() == 11,
          "lanes 4..11 are the 8 most active; keep[] stays in model order");
    check("lanes: hidden events summed", ls.hidden_events == 0 + 1 + 2 + 3,
          "lanes 0-3 hide");
    StripSelection ld = strip_selected_lanes(m, true);
    check("lanes: detail keeps all", ld.keep.size() == 12 && ld.hidden == 0,
          "");
    StripSelection bs = strip_selected_bands(m, false);
    check("bands: top 6 kept",
          bs.keep.size() == 6 && bs.hidden == 3 && bs.keep.front() == 3,
          "bands 3..8; base order preserved");
    check("bands: hidden accesses summed", bs.hidden_events == 0 + 1 + 2, "");
    StripModel small;
    small.lanes.resize(3);
    small.lane_activity.resize(3);
    small.deck_enabled = true;
    check("threshold no-op",
          strip_selected_lanes(small, false).keep.size() == 3 &&
              strip_selected_lanes(small, false).hidden == 0,
          "");
}

static void simplified_plan_rows() {
    StripModel m = big_model();
    strip_view_t v;
    v.px_w = 300;
    v.px_h = 700; // tall enough that the 40% deck cap fits all 12 rows
    v.seq_per_px = 100.0; // envelope mode
    std::vector<strip_prim_t> simp, det;
    strip_plan(m, v, &simp);
    strip_view_t vd = v;
    vd.detail = true;
    strip_plan(m, vd, &det);
    size_t sh = 0, dh = 0, agg_lane = 0, agg_band = 0;
    for (auto &p : simp) {
        if (p.kind == strip_prim::lane_header) {
            sh++;
            if (p.a == kStripAggRow)
                agg_lane++;
        }
        if (p.kind == strip_prim::band_label && p.a == kStripAggRow)
            agg_band++;
    }
    for (auto &p : det)
        if (p.kind == strip_prim::lane_header)
            dh++;
    check("simplified: 8 lanes + 1 aggregate", sh == 9 && agg_lane == 1, "");
    check("simplified: elsewhere band row", agg_band == 1, "");
    check("detailed: all 12 lanes, no aggregate", dh == 12, "");
    check("aggregate lane label",
          [&] {
              for (auto &p : simp)
                  if (p.kind == strip_prim::lane_header && p.a == kStripAggRow)
                      return p.text == "(+4 lanes, 6 events)";
              return false;
          }(),
          "counted, never vanished");
    check("aggregate hover is the claim",
          [&] {
              for (auto &p : simp)
                  if (p.kind == strip_prim::lane_header && p.a == kStripAggRow)
                      return strip_hover_text(m, p) == p.text;
              return false;
          }(),
          "");
    check("elsewhere band label",
          [&] {
              for (auto &p : simp)
                  if (p.kind == strip_prim::band_label && p.a == kStripAggRow)
                      return p.text ==
                             "(+3 regions — 3 access(es), counts only)";
              return false;
          }(),
          "");
    check("simplified fewer prims", simp.size() < det.size(),
          "the budget claim");
    check("hud states the posture",
          [&] {
              for (auto &p : simp)
                  if (p.kind == strip_prim::hud_note)
                      return p.text.find("simplified — top 8 of 12 lanes, "
                                         "top 6 of 9 regions") !=
                             std::string::npos;
              return false;
          }(),
          "pinned like the axis label");
    check("hud silent when nothing hidden",
          [&] {
              for (auto &p : det)
                  if (p.kind == strip_prim::hud_note)
                      return p.text.find("simplified") == std::string::npos;
              return true;
          }(),
          "");
    // byte-identical at/below thresholds
    Recording r = mk_rec({R"({"k":"trace","basis":"rel","off":16,"tid":10})",
                          R"({"k":"end","events":1})"});
    StripModel sm = strip_build(r, two_bands(), {});
    strip_view_t sv;
    sv.px_w = 200;
    sv.px_h = 300;
    sv.seq_per_px = 0.05;
    std::vector<strip_prim_t> a2, b2;
    strip_plan(sm, sv, &a2);
    strip_view_t svd = sv;
    svd.detail = true;
    strip_plan(sm, svd, &b2);
    check("small model: simplified == detailed byte-identical",
          strip_plan_dump(a2) == strip_plan_dump(b2),
          "zero change below thresholds");
}

static void plan_key_sensitivity() {
    strip_view_t v;
    v.px_w = 800;
    v.px_h = 400;
    v.seq_per_px = 1.0;
    const uint64_t k0 = strip_plan_key(v, 7);
    check("key stable", strip_plan_key(v, 7) == k0,
          "identical inputs → identical key (the cache's whole premise)");
    strip_view_t w;
    w = v; w.seq0 = 1;       check("key: seq0", strip_plan_key(w, 7) != k0, "");
    w = v; w.seq_per_px = 2; check("key: zoom", strip_plan_key(w, 7) != k0, "");
    w = v; w.lane0 = 1;      check("key: lane0", strip_plan_key(w, 7) != k0, "");
    w = v; w.lane_h = 20;    check("key: lane_h", strip_plan_key(w, 7) != k0, "");
    w = v; w.px_w = 801;     check("key: px_w", strip_plan_key(w, 7) != k0, "");
    w = v; w.px_h = 401;     check("key: px_h", strip_plan_key(w, 7) != k0, "");
    w = v; w.detail = true;  check("key: detail", strip_plan_key(w, 7) != k0, "");
    check("key: model_gen", strip_plan_key(v, 8) != k0, "");
    check("key: follow does NOT key",
          [&] {
              strip_view_t f = v;
              f.follow_tail = false;
              return strip_plan_key(f, 7) == k0;
          }(),
          "follow only moves seq0, which is keyed on its own");
}

static void density_rle() {
    StripModel m;
    m.deck_enabled = true;
    StripLane ln;
    ln.tid = 1;
    ln.label = "[1]";
    m.lanes.push_back(ln);
    // seq_per_px=8, px_w=8 → columns are [0,8),[8,16),…: cols 0,1 hold 2
    // events each; cols 2,3 empty; cols 4,5 hold 2 each — two equal runs
    // split by a gap
    m.lane_activity.push_back({0, 1, 8, 9, 32, 33, 40, 41});
    m.seq_end = 64;
    strip_view_t v;
    v.px_w = 8;
    v.px_h = 200;
    v.seq_per_px = 8.0; // envelope mode
    std::vector<strip_prim_t> p;
    strip_plan(m, v, &p);
    size_t density = 0;
    for (auto &q : p)
        if (q.kind == strip_prim::lane_density) {
            density++;
            check("rle: run spans the equal stretch", q.x1 - q.x0 >= 2.0f,
                  "adjacent equal columns merged into one prim");
        }
    check("rle: two runs, not four columns", density == 2,
          "equal-intensity neighbours collapse; the gap breaks the run");

    // envelope merge: one band, identical min/max in adjacent columns
    StripModel me;
    me.bands_enabled = true;
    StripBand bd;
    bd.region.base = 0x1000;
    bd.region.len = 0x1000;
    bd.region.label = "code";
    me.bands.push_back(bd);
    for (int i = 0; i < 8; i++) {
        StripMemMark mk;
        mk.seq = static_cast<uint64_t>(i * 8); // one per column at spp=8
        mk.addr = 0x1800;                      // SAME address every column
        mk.size = 8;
        mk.is_write = true;
        mk.band = 0;
        me.mem.push_back(mk);
    }
    me.seq_end = 64;
    std::vector<strip_prim_t> pe;
    strip_plan(me, v, &pe);
    size_t envs = 0;
    for (auto &q : pe)
        if (q.kind == strip_prim::mem_envelope)
            envs++;
    check("envelope merge: one run, not eight columns", envs == 1,
          "identical adjacent column rects extend, never restack");
}

static void pinned_strings() {
    check("axis label pinned",
          std::string(StripModel::axis_label()) == "stream order — not time",
          "the axis's own honesty claim, verbatim");
    check("mem tid note pinned",
          std::string(StripModel::mem_tid_note()) ==
              "mem carries no tid — access marks are r/w-hued, never "
              "thread-hued",
          "the legend's no-inference claim, verbatim");
}

int main() {
    camera_math();
    layout_sums();
    lanes_discovery_and_grouping();
    lanes_single_stream_and_unknown();
    rail_rows();
    bands_and_marks();
    run_seams();
    plan_determinism_and_modes();
    plan_stable_tint_parity();
    plan_key_sensitivity();
    simplified_selection();
    simplified_plan_rows();
    density_rle();
    pinned_strings();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
