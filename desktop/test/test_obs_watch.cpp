// test_obs_watch.cpp — the watchpoint timeline (08-observer-views.md T2).
//
// Two fixtures, because the two outcomes are equally real: a session that hit,
// and a session whose ARM was refused. The refusal fixture is the arm64 ENOSPC
// class (a host advertising slots that will not reserve one, commit 9184c14),
// and the check is that its measured sentence reaches the screen unedited —
// re-wording it would send an operator looking for a permission problem that
// is not there.
#include "views/watch.h"

#include "view_test.h"

using namespace asmdesk;

int main() {
    Recording rec = vt::load_fixture("obs-watch.asmtrace");
    WatchView v = obs_watch_build(rec);

    vt::eq("hits", v.hits.size(), size_t{3});
    vt::check("arm echoed", v.have_effective,
              "the started params echo is the "
              "only faithful source for the arm");
    vt::eq("armed len", v.effective.len, 4);
    vt::eq("armed rw", v.effective.rw, 1);

    // is_write has THREE values and each keeps its own word.
    vt::eq("write", std::string(obs_watch_dir_word(v.hits[0].is_write)),
           std::string("write"));
    vt::eq("read", std::string(obs_watch_dir_word(v.hits[1].is_write)),
           std::string("read"));
    vt::eq("undecodable", std::string(obs_watch_dir_word(v.hits[2].is_write)),
           std::string("undecodable"));

    // An unread value is not a zero.
    vt::eq("value present", obs_watch_value_cell(v.hits[0]),
           std::string("0x0000002a (4B)"));
    vt::eq("value absent", obs_watch_value_cell(v.hits[2]),
           std::string("(not read back)"));
    std::string dump = obs_watch_dump(v);
    vt::check("no invented zero", dump.find("value=0x0 ") == std::string::npos,
              "an unread value rendered as a number");

    // Location resolution degrades to the bare pc rather than lying.
    vt::eq("resolved loc", obs_watch_loc(v.hits[0]),
           std::string("work+0x12 [watch_victim]"));
    vt::eq("unresolved loc", obs_watch_loc(v.hits[2]), std::string("0x4011f4"));

    // --- the refused arm ---------------------------------------------------
    Recording skipped = vt::load_fixture("obs-watch-skip.asmtrace");
    WatchView sv = obs_watch_build(skipped);
    vt::check("skip seen", sv.skip.present,
              "the session skip event did not reach the view");
    vt::eq("skip code", sv.skip.code, 3);
    vt::eq("reason VERBATIM", sv.skip.reason,
           std::string("host reports 4 breakpoint slots but refused to reserve "
                       "one: No space left on device"));
    vt::check("reason reaches the render",
              obs_watch_dump(sv).find("No space left on device") !=
                  std::string::npos,
              "the measured reason must be on screen, not just in the model");
    vt::eq("no hits, and that is not an error", sv.hits.size(), size_t{0});

    // --- the arm form refuses what the protocol refuses ---------------------
    WatchArm a;
    a.addr = 0;
    vt::eq("addr required", obs_watch_arm_error(a),
           std::string("mode \"watch\" needs \"addr\""));
    a.addr = 0x601048;
    a.len = 3;
    vt::eq("len must be 1/2/4/8", obs_watch_arm_error(a),
           std::string("\"len\" must be 1, 2, 4 or 8 for mode \"watch\""));
    a.len = 4;
    a.addr = 0x601046;
    vt::eq("addr must be len-aligned", obs_watch_arm_error(a),
           std::string("\"addr\" must be \"len\"-aligned (an x86 hardware "
                       "rule)"));
    vt::eq("illegal arm sends nothing", obs_watch_start_command(a, 5150),
           std::string());
    a.addr = 0x601048;
    a.rw = 2;
    vt::eq("rw must be 0 or 1", obs_watch_arm_error(a),
           std::string("\"rw\" must be 0 (writes) or 1 (reads and writes)"));
    a.rw = 1;
    a.max = 8;
    vt::eq("legal arm", obs_watch_arm_error(a), std::string());
    nlohmann::json j = nlohmann::json::parse(obs_watch_start_command(a, 5150));
    vt::eq("mode", j.value("mode", std::string()), std::string("watch"));
    vt::eq("addr", j.value("addr", uint64_t{0}), uint64_t{0x601048});
    vt::eq("max", j.value("max", -1L), 8L);

    // The value-over-hit plot series (15-T1 follow-on) includes ONLY value_ok
    // hits — an un-read-back value is a gap, never a fabricated point.
    WatchPlot wp = obs_watch_plot(v);
    size_t ok = 0;
    for (const WatchHit &h : v.hits)
        if (h.value_ok)
            ok++;
    vt::eq("plot has one point per value_ok hit", wp.steps.size(), ok);
    vt::eq("plot steps and values are parallel", wp.steps.size(),
           wp.values.size());
    vt::check("value_ok hits actually exist to plot", ok >= 1,
              "the fixture should have a readable value");
    bool plotted_unread = false;
    for (double step : wp.steps)
        if (step == static_cast<double>(v.hits[2].hit_no))
            plotted_unread = true; // hit 2 is the undecodable, value_ok == false
    vt::check("plot excludes the unread value (hit 2)", !plotted_unread,
              "an un-read-back value was plotted — a fabricated point");

    vt::golden("obs-watch.txt", dump);
    vt::golden("obs-watch-skip.txt", obs_watch_dump(sv));
    return vt::report("test_obs_watch");
}
