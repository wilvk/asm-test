// hotedges.cpp — the pure builder + dump of hotedges.h. No ImGui, no I/O.
#include "views/hotedges.h"

#include <algorithm>
#include <cstdio>

namespace asmdesk {

namespace {
std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}
} // namespace

HotEdgeView obs_hotedges_build(const Recording &r, const ObsLifecycle *lc) {
    HotEdgeView v;
    v.chrome = obs_chrome(r);
    v.skip = obs_skip(lc ? *lc : obs_lifecycle_of(r), "sample");

    auto it = r.by_kind.find("survey");
    if (it == r.by_kind.end() || it->second.empty())
        return v;
    v.snapshots = it->second.size();
    // A survey snapshot is the whole histogram as of that window, so the last
    // one supersedes the others; summing them would double-count every sample.
    const nlohmann::json &s = it->second.back().body;

    v.sampler = s.value("sampler", std::string());
    v.samples = s.value("samples", uint64_t{0});
    v.branch_samples = s.value("branch_samples", uint64_t{0});
    v.lost = s.value("lost", uint64_t{0});
    v.throttled = s.value("throttled", false);
    if (r.provenance.raw.is_object() && r.provenance.raw.contains("window") &&
        r.provenance.raw["window"].is_object()) {
        const nlohmann::json &w = r.provenance.raw["window"];
        v.have_window = true;
        v.window_base = w.value("base", uint64_t{0});
        v.window_len = w.value("len", uint64_t{0});
    }

    if (s.contains("edges") && s["edges"].is_array()) {
        for (const nlohmann::json &e : s["edges"]) {
            if (!e.is_object())
                continue;
            HotEdge h;
            h.from_addr = e.value("from_addr", uint64_t{0});
            h.to_addr = e.value("to_addr", uint64_t{0});
            h.from = e.value("from", std::string());
            h.to = e.value("to", std::string());
            h.count = e.value("count", uint64_t{0});
            h.mispred = e.value("mispred", uint64_t{0});
            h.is_return = e.value("is_return", uint64_t{0});
            v.edges.push_back(std::move(h));
        }
    }
    // Descending count, ties broken by the addresses — so the same recording
    // ranks identically every time and a screenshot can be compared with a
    // later one. (The engine already sorts; this does not trust that.)
    std::stable_sort(v.edges.begin(), v.edges.end(),
                     [](const HotEdge &a, const HotEdge &b) {
                         if (a.count != b.count)
                             return a.count > b.count;
                         if (a.from_addr != b.from_addr)
                             return a.from_addr < b.from_addr;
                         return a.to_addr < b.to_addr;
                     });
    for (size_t i = 0; i < v.edges.size(); i++)
        v.edges[i].rank = static_cast<int>(i) + 1;

    if (r.provenance.exact)
        v.provenance_conflict =
            "this recording carries survey events but declares "
            "provenance.exact = true — a sampled histogram is never exact "
            "(schema: a survey is always exact:false). Shown as STATISTICAL "
            "regardless; the producer is wrong, not the data's trust level";
    return v;
}

bool obs_hotedges_weak_evidence(const HotEdgeView &v) {
    // Anything that is not an IBS-Op branch edge is residency-grade for the
    // purpose a picker puts this to. Unstated counts as weaker: an unlabelled
    // sampler is not a promise of the stronger one.
    return v.sampler != "ibs-op";
}

std::string obs_hotedges_evidence_label(const HotEdgeView &v) {
    if (v.sampler == "ibs-op")
        return "IBS-Op retired-branch edges — a DIRECT observation of control "
               "arriving here, which is the same event a capture waits for";
    if (v.sampler == "sw-clock")
        return "software-clock residency — WEAKER EVIDENCE: it says a function "
               "was executing, not that it was entered, so a hot row here can "
               "be something entered once and never re-entered";
    return "sampler unstated — treated as the weaker (residency) evidence, "
           "because an unlabelled sampler is not a promise of the stronger one";
}

std::string obs_hotedges_chrome_line(const HotEdgeView &v) {
    std::string s = "sampler=" + (v.sampler.empty() ? "(unstated)" : v.sampler);
    s += " samples=" + std::to_string(v.samples);
    s += " branch_samples=" + std::to_string(v.branch_samples);
    s += " lost=" + std::to_string(v.lost);
    s += " throttled=" + std::string(v.throttled ? "yes" : "no");
    s += " window=";
    s += v.have_window ? hex(v.window_base) + "+" + std::to_string(v.window_len)
                       : "(whole process)";
    return s;
}

const char *obs_hotedges_no_flame_note() {
    return "no flame graph: these samples are branch EDGES, not stacks — "
           "nothing here observed a call stack, so frames would be inferred "
           "ancestry drawn in the same ink as measurement";
}

std::vector<HotEdge> obs_hotedges_top(const HotEdgeView &v, size_t n) {
    std::vector<HotEdge> out;
    for (size_t i = 0; i < v.edges.size() && i < n; i++)
        out.push_back(v.edges[i]);
    return out;
}

std::string obs_hotedges_dump(const HotEdgeView &v) {
    std::string s;
    if (!v.chrome.banner.empty())
        s += "banner=" + v.chrome.banner + "\n";
    // Statistical, always, and said before the numbers rather than after.
    s += "STATISTICAL — " + obs_hotedges_evidence_label(v) + "\n";
    if (!v.provenance_conflict.empty())
        s += "CONFLICT: " + v.provenance_conflict + "\n";
    s += obs_hotedges_chrome_line(v) + "\n";
    if (v.skip.present)
        s += "skip=" + std::to_string(v.skip.code) + " " + v.skip.reason + "\n";
    s += "snapshots=" + std::to_string(v.snapshots) +
         " edges=" + std::to_string(v.edges.size()) + "\n";
    for (const HotEdge &e : v.edges)
        s += "  #" + std::to_string(e.rank) + " " +
             (e.from.empty() ? hex(e.from_addr) : e.from) + " -> " +
             (e.to.empty() ? hex(e.to_addr) : e.to) +
             " count=" + std::to_string(e.count) +
             " mispred=" + std::to_string(e.mispred) +
             " ret=" + std::to_string(e.is_return) + "\n";
    return s;
}

} // namespace asmdesk
