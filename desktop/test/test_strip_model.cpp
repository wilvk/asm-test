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
    pinned_strings();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
