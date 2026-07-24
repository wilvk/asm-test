// tree.cpp — the pure builder + filter rules of tree.h. No ImGui, no I/O.
#include "views/tree.h"

#include <algorithm>
#include <set>

namespace asmdesk {

TreeView obs_tree_build(const Recording &r, const ObsLifecycle *lc) {
    TreeView v;
    const ObsLifecycle life = lc ? *lc : obs_lifecycle_of(r);
    v.chrome = obs_chrome(r);
    v.skip = obs_skip(life, "tree");

    auto it = r.by_kind.find("call");
    if (it != r.by_kind.end()) {
        for (const Event &e : it->second) {
            TreeRow row;
            row.tid = e.body.value("tid", 0L);
            row.depth = e.body.value("depth", 0);
            row.addr = e.body.value("addr", uint64_t{0});
            row.name = e.body.value("name", std::string());
            row.module = e.body.value("module", std::string());
            v.rows.push_back(std::move(row));
        }
    }

    const nlohmann::json *p = obs_started_params(life, "tree");
    if (p != nullptr) {
        v.have_effective = true;
        v.effective.depth = p->value("depth", 0);
        v.effective.focus = p->value("focus", std::string());
        v.effective.module = p->value("module", std::string());
        v.effective.tid = p->value("tid", 0L);
        v.effective.follow = p->value("follow", false);
    }
    return v;
}

std::vector<long> obs_tree_tids(const TreeView &v) {
    std::set<long> s;
    for (const TreeRow &r : v.rows)
        s.insert(r.tid);
    return std::vector<long>(s.begin(), s.end());
}

std::string obs_tree_filter_error(const TreeFilter &f) {
    // The wording is the serve loop's, character for character (cli/asmspy.c,
    // the flag matrix; asmtrace-schema.md, "Refusals"). Two front ends that
    // paraphrase the same rule differently are two rules as far as a user is
    // concerned.
    if (f.tid != 0 && f.follow)
        return "\"tid\" pins ONE task; \"follow\" adds child processes — "
               "drop one";
    if (f.tid < 0)
        return "\"tid\" must be a positive task id";
    if (f.depth != 0 && (f.depth < 1 || f.depth > kTreeDepthMax))
        return "\"depth\" must be 1..1000 (omit it for unlimited)";
    return "";
}

nlohmann::json obs_tree_start_params(const TreeFilter &f) {
    nlohmann::json p = nlohmann::json::object();
    // Omission is meaningful: an omitted parameter takes the subcommand
    // default, and for depth that default is UNLIMITED — which `depth:0` is
    // not (it is refused). So a field at its default is left off the wire.
    if (f.tid != 0)
        p["tid"] = f.tid;
    if (f.follow)
        p["follow"] = true;
    if (f.depth != 0)
        p["depth"] = f.depth;
    if (!f.focus.empty())
        p["focus"] = f.focus;
    if (!f.module.empty())
        p["module"] = f.module;
    return p;
}

std::string obs_tree_start_command(const TreeFilter &f, long pid) {
    if (!obs_tree_filter_error(f).empty())
        return "";
    nlohmann::json cmd = obs_tree_start_params(f);
    // Key order: `cmd`/`mode`/`pid` first, as the protocol's examples spell it.
    nlohmann::json out = nlohmann::json::object();
    out["cmd"] = "start";
    out["mode"] = "tree";
    out["pid"] = pid;
    for (auto it = cmd.begin(); it != cmd.end(); ++it)
        out[it.key()] = it.value();
    return out.dump();
}

std::string obs_tree_dump(const TreeView &v) {
    std::string s;
    if (!v.chrome.banner.empty())
        s += "banner=" + v.chrome.banner + "\n";
    s += "chrome=" + obs_chrome_chip(v.chrome) + "\n";
    if (v.skip.present)
        s += "skip=" + std::to_string(v.skip.code) + " " + v.skip.reason + "\n";
    if (v.have_effective) {
        const TreeFilter &f = v.effective;
        s += "effective: depth=" + std::to_string(f.depth) +
             " focus=" + (f.focus.empty() ? "(none)" : f.focus) +
             " module=" + (f.module.empty() ? "(none)" : f.module) +
             " tid=" + std::to_string(f.tid) +
             " follow=" + (f.follow ? "yes" : "no") + "\n";
    }
    std::vector<long> tids = obs_tree_tids(v);
    s += "rows=" + std::to_string(v.rows.size()) +
         " tids=" + std::to_string(tids.size()) + "\n";
    for (const TreeRow &r : v.rows) {
        s += "  ";
        // The indent IS the engine's effective depth — under a focus filter it
        // is re-based, which is exactly why the client must not recompute it.
        s.append(static_cast<size_t>(std::max(0, r.depth)) * 2, ' ');
        s += "-> " + (r.name.empty() ? std::string("(unresolved)") : r.name);
        if (!r.module.empty())
            s += " [" + r.module + "]";
        s += " tid=" + std::to_string(r.tid) +
             " depth=" + std::to_string(r.depth) + "\n";
    }
    return s;
}

} // namespace asmdesk
