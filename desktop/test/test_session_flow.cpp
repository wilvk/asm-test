// test_session_flow.cpp — the Session flow's pure closure: StripModel →
// space::SessionFlowScene. No ImGui, no GL, no engine (D4). The rules pinned
// here are the scene's honesty bar: raw counts survive smoothing, hidden
// lanes are counted into one row, a class tie stays grey, and the pinned
// smoothing note never rewords.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "views/strip_flow.h"

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

static StripModel twelve_lane_model() {
    StripModel m;
    for (int i = 0; i < 12; i++) {
        StripLane ln;
        ln.tid = 100 + i;
        ln.tgid = (i < 6) ? 10 : -1;
        ln.label = "[" + std::to_string(100 + i) + "]";
        m.lanes.push_back(ln);
        std::vector<uint64_t> act;
        for (int k = 0; k < i; k++)
            act.push_back(static_cast<uint64_t>(k));
        m.lane_activity.push_back(act);
    }
    // syscalls: with seq_end == 192 == kFlowBuckets, seq IS the bucket —
    // bucket 0 gets File×2 + Net×1 (a strict File win), bucket 180 Net alone
    auto sys = [&](uint64_t seq, space::SyscallClass c) {
        StripSys s;
        s.seq = seq;
        s.cls = c;
        s.line = "x";
        m.sys.push_back(s);
    };
    sys(0, space::SyscallClass::File);
    sys(0, space::SyscallClass::File);
    sys(0, space::SyscallClass::Net);
    sys(180, space::SyscallClass::Net);
    // memory: three accesses late in the stream
    for (uint64_t q = 150; q <= 152; q++) {
        StripMemMark k;
        k.seq = q;
        k.addr = 0x1000 + q;
        k.band = 0;
        m.mem.push_back(k);
    }
    StripRunSeam seam;
    seam.kind = StripSeamKind::Capture;
    seam.seq = 96;
    seam.label = "capture 2";
    m.seams.push_back(seam);
    m.deck_enabled = true;
    m.seq_end = 192; // one seq per bucket: seq == bucket
    return m;
}

static void rows_and_counts() {
    StripModel m = twelve_lane_model();
    space::SessionFlowScene f = build_session_flow(m);
    check("enabled", f.enabled, "twelve lanes of activity");
    // 8 kept lanes + aggregate + kernel + memory
    check("row count", f.rows.size() == 11, "8 + agg + kernel + mem");
    check("row order",
          f.rows.size() == 11 &&
              f.rows[0].kind == space::FlowRowKind::Lane &&
              f.rows[8].kind == space::FlowRowKind::AggregateLanes &&
              f.rows[9].kind == space::FlowRowKind::Kernel &&
              f.rows[10].kind == space::FlowRowKind::Memory,
          "near→far: lanes, aggregate, kernel, memory");
    check("aggregate counted",
          f.rows.size() == 11 && f.rows[8].events == 0 + 1 + 2 + 3 &&
              f.rows[8].label == "(+4 lanes, 6 events)",
          "hidden lanes sum; the label is the claim");
    // raw counts survive smoothing: sum(counts) == channel totals
    uint64_t lane_total = 0;
    for (size_t i = 0; i < 9 && i < f.rows.size(); i++)
        lane_total += f.rows[i].events;
    check("lane totals", lane_total == 0 + 1 + 2 + /*...*/ 3 + 4 + 5 + 6 + 7 +
                                           8 + 9 + 10 + 11,
          "every activity event lands in exactly one lane row's counts");
    check("kernel total", f.rows.size() == 11 && f.rows[9].events == 4, "");
    check("memory total", f.rows.size() == 11 && f.rows[10].events == 3, "");
    // heights normalized to the scene max, in [0,1]
    float mx = 0;
    for (auto &r : f.rows)
        for (float h : r.heights)
            mx = std::max(mx, h);
    check("heights normalized", mx > 0.999f && mx <= 1.0f,
          "1.0 is the busiest bucket anywhere in the scene");
    // dominant class: bucket 0 has File×2 + Net×1 → File+1; bucket 180 Net+1
    check("dominant class strict winner",
          f.rows.size() == 11 &&
              f.rows[9].bucket_class[0] ==
                  static_cast<uint8_t>(space::SyscallClass::File) + 1 &&
              f.rows[9].bucket_class[180] ==
                  static_cast<uint8_t>(space::SyscallClass::Net) + 1,
          "");
    // seam mapped to its bucket
    check("seam bucket", f.seams.size() == 1 && f.seams[0].bucket == 96 &&
                             f.seams[0].label == "capture 2",
          "seq==bucket at seq_end==192");
    // determinism
    check("dump deterministic",
          session_flow_dump(f) == session_flow_dump(build_session_flow(m)),
          "");
    // lane hue = MODEL index (the strip pc-mark hue), not the kept position
    check("lane hue is model index",
          f.rows[0].kind == space::FlowRowKind::Lane && f.rows[0].lane_ord == 4,
          "12 lanes: kept lanes are 4..11, so row 0 wears lane 4's hue");
}

static void tie_is_grey_and_thresholds() {
    StripModel m = twelve_lane_model();
    // make bucket 0 a tie: add one more Net at seq 0 → File×2 Net×2
    StripSys s;
    s.seq = 0;
    s.cls = space::SyscallClass::Net;
    s.line = "x";
    m.sys.push_back(s);
    space::SessionFlowScene f = build_session_flow(m);
    bool found = false;
    for (auto &r : f.rows)
        if (r.kind == space::FlowRowKind::Kernel) {
            found = true;
            check("tie stays grey", r.bucket_class[0] == 0,
                  "\"could not tell\" and \"File won\" are different facts");
        }
    check("kernel row present", found, "");

    // under-threshold identity: 3 lanes → 3 lane rows, no aggregate
    StripModel small;
    for (int i = 0; i < 3; i++) {
        StripLane ln;
        ln.tid = i;
        ln.label = "[" + std::to_string(i) + "]";
        small.lanes.push_back(ln);
        small.lane_activity.push_back({static_cast<uint64_t>(i)});
    }
    small.deck_enabled = true;
    small.seq_end = 8;
    space::SessionFlowScene sf = build_session_flow(small);
    size_t lanes = 0, aggs = 0;
    for (auto &r : sf.rows) {
        if (r.kind == space::FlowRowKind::Lane)
            lanes++;
        if (r.kind == space::FlowRowKind::AggregateLanes)
            aggs++;
    }
    check("under threshold: all lanes, no aggregate", lanes == 3 && aggs == 0,
          "");

    // a silent kept lane stays (flat), never invented away
    StripModel quiet;
    StripLane ln;
    ln.tid = 7;
    ln.label = "[7]";
    quiet.lanes.push_back(ln);
    quiet.lane_activity.push_back({}); // zero events
    quiet.deck_enabled = true;
    quiet.seq_end = 4;
    space::SessionFlowScene qf = build_session_flow(quiet);
    check("silent lane kept",
          qf.enabled && qf.rows.size() == 1 && qf.rows[0].events == 0,
          "a kept silent lane draws flat");

    // disabled: empty stream, verbatim reason
    StripModel empty;
    space::SessionFlowScene ef = build_session_flow(empty);
    check("disabled with reason",
          !ef.enabled && ef.disabled_reason == "no stream to bucket — the "
                                               "recording holds no events at "
                                               "all",
          "quietly absent is indistinguishable from nothing happened");
    check("smoothing note pinned",
          std::string(space::SessionFlowScene::smoothing_note()) ==
              "heights are events per bucket of stream order, smoothed for "
              "display — not time, not duration",
          "the display claim, verbatim");
}

static void picks() {
    StripModel m = twelve_lane_model();
    space::SessionFlowScene f = build_session_flow(m);
    check("pick order covers rows+seams",
          flow_pick_order(f) == f.rows.size() + f.seams.size(), "");
    auto l0 = flow_pick_link(f, 0, "rec-x");
    check("lane link → syscalls+pid",
          l0.has_value() && l0->view == dt_view::syscalls &&
              l0->pid.has_value() && *l0->pid == 10,
          "lanes 4..5 kept from tgid 10");
    auto lm = flow_pick_link(f, 10, "rec-x");
    check("memory link → timeline",
          lm.has_value() && lm->view == dt_view::timeline, "");
    check("seam is hover-only",
          !flow_pick_link(f, f.rows.size(), "rec-x").has_value(), "");
    check("seam hint", flow_pick_hint(f, f.rows.size()) == "capture 2", "");
    check("row hint",
          flow_pick_hint(f, 9).find("kernel crossings — 4 events") !=
              std::string::npos,
          "");
}

int main() {
    rows_and_counts();
    tie_is_grey_and_thresholds();
    picks();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
