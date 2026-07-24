// observer.cpp — see observer.h. Pure: no ImGui, no I/O, no engine.
#include "views/observer.h"

namespace asmdesk {

ObsChrome obs_chrome(const Recording &r) {
    ObsChrome c;
    c.backend = r.provenance.backend;
    c.trust = r.provenance.trust;
    c.exact = r.provenance.exact;
    c.redacted = r.provenance.redacted;
    c.torn = r.torn;
    c.truncated = r.truncated();
    c.lost = r.drops_lost;
    c.throttled = r.drops_throttled;

    if (c.torn)
        c.banner = "TORN recording — no end footer; the capture stopped "
                   "mid-record and what follows is a prefix, not a whole";
    else if (r.end_truncated)
        c.banner = "TRUNCATED recording — buffers filled; rows below are a "
                   "prefix of what happened";
    if (c.dropped()) {
        if (!c.banner.empty())
            c.banner += "; ";
        c.banner += "dropped " + std::to_string(c.lost) + " event(s)";
        if (c.throttled)
            c.banner += " (the kernel throttled the sample rate)";
    }
    return c;
}

std::string obs_chrome_chip(const ObsChrome &c) {
    std::string b = c.backend.empty() ? "(unknown backend)" : c.backend;
    // The word is the schema's, not a synonym: a statistical view that read
    // "exact" here would be the interface lying about what it measured.
    std::string word = c.exact ? "exact" : "statistical";
    std::string chip = b + " / " + word;
    // `trust` is a finer grade than exact/statistical ("weak", "strong"), so it
    // is shown only when it says something the first word did not.
    if (!c.trust.empty() && c.trust != word)
        chip += " (" + c.trust + ")";
    return chip;
}

ObsLifecycle obs_lifecycle_of(const Recording &r) {
    ObsLifecycle lc;
    auto it = r.by_kind.find("session");
    if (it == r.by_kind.end())
        return lc;
    for (const Event &e : it->second)
        lc.sessions.push_back(e.body);
    return lc;
}

const nlohmann::json *obs_started_params(const ObsLifecycle &lc,
                                         const std::string &mode) {
    const nlohmann::json *found = nullptr;
    for (const nlohmann::json &b : lc.sessions) {
        if (!b.is_object())
            continue;
        if (b.value("state", std::string()) != "started")
            continue;
        if (!mode.empty() && b.value("mode", std::string()) != mode)
            continue;
        if (b.contains("params") && b["params"].is_object())
            // The LAST started wins: successive lifecycle events are a history,
            // and what is running now is the current fact.
            found = &b["params"];
    }
    return found;
}

ObsSkip obs_skip(const Recording &r, const std::string &mode) {
    return obs_skip(obs_lifecycle_of(r), mode);
}

ObsSkip obs_skip(const ObsLifecycle &lc, const std::string &mode) {
    ObsSkip s;
    for (const nlohmann::json &b : lc.sessions) {
        if (!b.is_object() || !b.contains("state") || !b["state"].is_string())
            continue;
        if (b["state"].get<std::string>() != "skip")
            continue;
        std::string m = b.value("mode", std::string());
        if (!mode.empty() && m != mode)
            continue;
        s.present = true;
        s.mode = m;
        if (b.contains("skip") && b["skip"].is_object()) {
            s.code = b["skip"].value("code", 0);
            s.reason = b["skip"].value("reason", std::string());
        }
        // The LAST skip wins: successive lifecycle events are a history, and
        // what the session ended up reporting is the current fact.
    }
    return s;
}

} // namespace asmdesk
