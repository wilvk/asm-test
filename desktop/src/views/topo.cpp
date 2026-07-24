// topo.cpp — the pure builder + dump of topo.h. No ImGui, no I/O.
#include "views/topo.h"

#include <algorithm>
#include <map>

namespace asmdesk {

TopoView obs_topo_build(const Recording &r, const ObsLifecycle *lc) {
    TopoView v;
    v.chrome = obs_chrome(r);
    v.skip = obs_skip(lc ? *lc : obs_lifecycle_of(r), "procs");

    auto it = r.by_kind.find("topo");
    if (it == r.by_kind.end() || it->second.empty())
        return v;
    v.snapshots = it->second.size();
    const nlohmann::json &snap = it->second.back().body;
    v.count_mode = snap.value("mode", std::string());

    std::map<long, TopoCard> by_tgid;
    if (snap.contains("tasks") && snap["tasks"].is_array()) {
        for (const nlohmann::json &t : snap["tasks"]) {
            if (!t.is_object())
                continue;
            TopoTask task;
            task.tid = t.value("tid", 0L);
            task.tgid = t.value("tgid", 0L);
            task.ppid = t.value("ppid", 0L);
            task.leader = t.value("leader", false);
            task.comm = t.value("comm", std::string());
            task.exe = t.value("exe", std::string());
            task.inv = t.value("inv", uint64_t{0});

            TopoCard &c = by_tgid[task.tgid];
            c.tgid = task.tgid;
            c.inv_total += task.inv;
            // The leader carries the process's identity (`exe` is leader-only
            // by the engine's contract), so it — and only it — sets the card's
            // name and parent.
            if (task.leader) {
                c.comm = task.comm;
                c.exe = task.exe;
                c.ppid = task.ppid;
            }
            c.threads.push_back(std::move(task));
        }
    }

    for (auto &kv : by_tgid) {
        TopoCard &c = kv.second;
        std::sort(c.threads.begin(), c.threads.end(),
                  [](const TopoTask &a, const TopoTask &b) {
                      // The leader first, then ascending tid: a thread list
                      // whose order changes between snapshots is unreadable.
                      if (a.leader != b.leader)
                          return a.leader;
                      return a.tid < b.tid;
                  });
        if (c.comm.empty() && !c.threads.empty()) {
            // No leader task in this snapshot — the engine saw threads of a
            // process whose leader it never enumerated. Say what we have
            // rather than inventing a name for the card.
            c.comm = c.threads.front().comm;
            c.ppid = c.threads.front().ppid;
        }
        v.cards.push_back(std::move(c));
    }
    return v;
}

std::string obs_topo_fingerprint(const TopoView &v, const TopoCard &c) {
    std::string name =
        !c.exe.empty() ? c.exe : (c.comm.empty() ? "(unknown)" : c.comm);
    std::string s = name + " (" + std::to_string(c.tgid) + ")";
    if (c.ppid)
        s += " <- " + std::to_string(c.ppid);
    s += " — " + std::to_string(c.threads.size()) + " thread" +
         (c.threads.size() == 1 ? "" : "s");
    // The unit is never dropped: `inv` counts syscalls OR calls, and the two
    // are not comparable numbers.
    s += ", " + std::to_string(c.inv_total) + " " +
         (v.count_mode.empty() ? "counted event" : v.count_mode);
    return s;
}

const char *obs_topo_jack_note() {
    return "the topology engine SEIZEs the whole descendant tree, so while "
           "this "
           "view is live the ptrace jack is held for EVERY process below — not "
           "just the one you started from";
}

dt_link obs_topo_drill_link(const std::string &rec_id, const TopoCard &c) {
    dt_link l;
    l.rec = rec_id;
    l.view = dt_view::syscalls;
    l.pid = c.tgid;
    return l;
}

std::string obs_topo_dump(const TopoView &v) {
    std::string s;
    if (!v.chrome.banner.empty())
        s += "banner=" + v.chrome.banner + "\n";
    s += "chrome=" + obs_chrome_chip(v.chrome) + "\n";
    s += "jack=" + std::string(obs_topo_jack_note()) + "\n";
    if (v.skip.present)
        s += "skip=" + std::to_string(v.skip.code) + " " + v.skip.reason + "\n";
    s += "snapshots=" + std::to_string(v.snapshots) +
         " cards=" + std::to_string(v.cards.size()) +
         " counting=" + (v.count_mode.empty() ? "(unstated)" : v.count_mode) +
         "\n";
    for (const TopoCard &c : v.cards) {
        s += "  " + obs_topo_fingerprint(v, c) + "\n";
        for (const TopoTask &t : c.threads)
            s += "    tid " + std::to_string(t.tid) + " " + t.comm +
                 (t.leader ? " (leader)" : "") +
                 " inv=" + std::to_string(t.inv) + "\n";
    }
    return s;
}

} // namespace asmdesk
