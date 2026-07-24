// test_obs_topo.cpp — the topology map (08-observer-views.md T3).
#include "views/topo.h"

#include "view_test.h"

using namespace asmdesk;

int main() {
    Recording rec = vt::load_fixture("obs-topo.asmtrace");
    TopoView v = obs_topo_build(rec);

    // A snapshot is a complete statement of the tree at one moment, so the LAST
    // one is the view — merging them would describe a tree that never existed.
    vt::eq("snapshots seen", v.snapshots, size_t{2});
    vt::eq("cards", v.cards.size(), size_t{2});
    vt::eq("cards ordered by pid", v.cards[0].tgid, 4200L);
    vt::eq("threads folded into the process", v.cards[0].threads.size(),
           size_t{2});
    vt::check("leader first", v.cards[0].threads[0].leader,
              "the leader carries the card's identity and sorts first");
    vt::eq("leader identity used", v.cards[0].exe, std::string("spy_victim"));
    vt::eq("inv summed over threads", v.cards[0].inv_total, uint64_t{131});
    vt::eq("child process is its own card", v.cards[1].tgid, 4301L);
    vt::eq("child's parent", v.cards[1].ppid, 4200L);

    // The count's UNIT travels with the number: `inv` is syscalls or calls, and
    // the two are not the same measurement.
    vt::eq("count mode", v.count_mode, std::string("syscalls"));
    std::string fp = obs_topo_fingerprint(v, v.cards[0]);
    vt::check("fingerprint names the unit",
              fp.find("131 syscalls") != std::string::npos,
              "a bare invocation count means nothing: " + fp);
    vt::check("fingerprint names the thread count",
              fp.find("2 threads") != std::string::npos, fp);

    // The jack is held TREE-wide while a topology session runs.
    vt::check("jack note states the tree-wide hold",
              std::string(obs_topo_jack_note()).find("descendant tree") !=
                  std::string::npos,
              "the note must say the hold covers every process below");
    vt::check("jack note is in the render",
              obs_topo_dump(v).find("descendant tree") != std::string::npos,
              "the hold must be on screen, not merely available");

    // Drill-in is a deep link through 04's router, so it round-trips as text.
    dt_link l = obs_topo_drill_link("live-session.asmtrace", v.cards[1]);
    vt::eq("drill-in view", std::string(dt_view_name(l.view)),
           std::string("syscalls"));
    vt::check("drill-in names the process", l.pid.has_value(),
              "a per-process link with no process is not a drill-in");
    vt::eq("drill-in pid", *l.pid, 4301L);
    std::string text = dt_nav_format(l);
    dt_link back;
    std::string err;
    vt::check("link parses back", dt_nav_parse(text, back, err), err);
    vt::eq("pid round-trips", back.pid ? *back.pid : 0L, 4301L);
    vt::eq("link is byte-stable", dt_nav_format(back), text);

    vt::golden("obs-topo.txt", obs_topo_dump(v));
    return vt::report("test_obs_topo");
}
