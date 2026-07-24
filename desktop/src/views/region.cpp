// region.cpp — the pure builder + dump of region.h. No ImGui, no I/O.
#include "views/region.h"

#include <algorithm>
#include <cstdio>

namespace asmdesk {

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// One event of either kind, in STREAM order — the ordering by_kind cannot give.
struct SeqEvent {
    uint64_t seq;
    const Event *ev;
    bool is_coverage;
};

} // namespace

std::string obs_region_crawl_warning(const std::string &backend) {
    if (backend == "ptrace-region")
        return "single-step capture: the target CRAWLS inside this region "
               "while a snapshot is taken (it runs at native speed everywhere "
               "else, and resumes fully on detach)";
    if (backend == "ptrace-dataflow")
        return "single-step capture with operand reads: the target crawls "
               "inside this region, and each step also reads its operands — "
               "the slowest capture asmspy has, and the most informative";
    if (backend == "ptrace-stream")
        return "single-step capture: every instruction of every thread is "
               "stepped, so the whole process crawls until you detach";
    return "";
}

RegionView obs_region_build(const Recording &r, const ObsLifecycle *lc) {
    RegionView v;
    v.chrome = obs_chrome(r);
    v.skip = obs_skip(lc ? *lc : obs_lifecycle_of(r), "trace");
    v.crawl_warning = obs_region_crawl_warning(r.provenance.backend);
    if (r.by_kind.count("codeimage"))
        v.jit_hint =
            "this region is JIT-tracked (the recording carries codeimage "
            "versions) — single-stepping a JIT is the worst case; the "
            "out-of-band IBS survey (mode \"sample\") finds hot code without "
            "stopping the target at all";

    std::vector<SeqEvent> ordered;
    auto tr = r.by_kind.find("trace");
    if (tr != r.by_kind.end())
        for (const Event &e : tr->second)
            ordered.push_back({e.seq, &e, false});
    auto cov = r.by_kind.find("coverage");
    if (cov != r.by_kind.end())
        for (const Event &e : cov->second)
            ordered.push_back({e.seq, &e, true});
    std::sort(
        ordered.begin(), ordered.end(),
        [](const SeqEvent &a, const SeqEvent &b) { return a.seq < b.seq; });

    RegionInvocation cur;
    cur.number = 1;
    bool any = false;
    auto note_basis = [&v](const std::string &b) {
        if (b.empty())
            return;
        if (v.basis.empty())
            v.basis = b;
        else if (v.basis != b && v.basis_error.empty())
            // `basis` may never be defaulted or reconciled: "rel" offsets and
            // absolute addresses look alike and mean different places.
            v.basis_error = "this recording mixes basis \"" + v.basis +
                            "\" and \"" + b +
                            "\" — offsets and absolute addresses cannot be "
                            "shown on one axis";
    };

    for (const SeqEvent &se : ordered) {
        const nlohmann::json &b = se.ev->body;
        note_basis(b.value("basis", std::string()));
        if (!se.is_coverage) {
            uint64_t off = b.value("off", uint64_t{0});
            cur.insns.push_back(off);
            if (b.contains("disasm") && b["disasm"].is_string())
                cur.disasm[off] = b["disasm"].get<std::string>();
            cur.basis = b.value("basis", cur.basis);
            any = true;
            continue;
        }
        // A `coverage` event CLOSES the invocation it follows.
        cur.basis = b.value("basis", cur.basis);
        if (b.contains("blocks") && b["blocks"].is_array())
            for (const nlohmann::json &x : b["blocks"])
                if (x.is_number())
                    cur.blocks.push_back(x.get<uint64_t>());
        cur.blocks_total = b.value("blocks_total", uint64_t{0});
        cur.insns_total = b.value("insns_total", uint64_t{0});
        cur.truncated = b.value("truncated", false);
        cur.closed = true;
        v.invocations.push_back(std::move(cur));
        cur = RegionInvocation();
        cur.number = v.invocations.size() + 1;
        any = false;
    }
    // Trailing instructions with no coverage footer: a capture that stopped
    // mid-invocation. Kept, and marked open — dropping them would hide the
    // last thing the target did before the recording ended.
    if (any)
        v.invocations.push_back(std::move(cur));

    return v;
}

size_t obs_region_page(RegionView &v, int delta) {
    if (v.invocations.empty()) {
        v.selected = 0;
        return 0;
    }
    long long i = static_cast<long long>(v.selected) + delta;
    if (i < 0)
        i = 0;
    if (i >= static_cast<long long>(v.invocations.size()))
        i = static_cast<long long>(v.invocations.size()) - 1;
    v.selected = static_cast<size_t>(i);
    return v.selected;
}

Streams obs_region_snapshot_streams(const RegionView &v, size_t i) {
    Streams s;
    s.truncated = v.chrome.truncated;
    s.torn = v.chrome.torn;
    s.lost = v.chrome.lost;
    s.throttled = v.chrome.throttled;
    s.statistical = !v.chrome.exact;
    s.redacted = v.chrome.redacted;
    s.backend = v.chrome.backend;
    if (i >= v.invocations.size())
        return s;
    const RegionInvocation &inv = v.invocations[i];
    s.trace.insns = inv.insns;
    s.trace.blocks = inv.blocks;
    s.trace.disasm = inv.disasm;
    s.trace.basis = inv.basis.empty() ? v.basis : inv.basis;
    s.trace.basis_error = v.basis_error;
    s.trace.blocks_total = inv.blocks_total;
    s.trace.insns_total = inv.insns_total;
    // An invocation is truncated if ITS coverage said so, or if it never got a
    // footer at all — both mean what is shown is a prefix.
    s.trace.truncated = inv.truncated || !inv.closed;
    if (s.trace.truncated)
        s.truncated = true;
    return s;
}

std::string obs_region_dump(const RegionView &v) {
    std::string s;
    if (!v.chrome.banner.empty())
        s += "banner=" + v.chrome.banner + "\n";
    s += "chrome=" + obs_chrome_chip(v.chrome) + "\n";
    if (!v.crawl_warning.empty())
        s += "crawl=" + v.crawl_warning + "\n";
    if (!v.jit_hint.empty())
        s += "jit=" + v.jit_hint + "\n";
    if (!v.basis_error.empty())
        s += "BASIS ERROR: " + v.basis_error + "\n";
    if (v.skip.present)
        s += "skip=" + std::to_string(v.skip.code) + " " + v.skip.reason + "\n";
    s += "invocations=" + std::to_string(v.invocations.size()) +
         " selected=" + std::to_string(v.selected) +
         " basis=" + (v.basis.empty() ? "(none)" : v.basis) + "\n";
    for (const RegionInvocation &inv : v.invocations) {
        s += "  #" + std::to_string(inv.number) +
             " insns=" + std::to_string(inv.insns.size()) + "/" +
             std::to_string(inv.insns_total) +
             " blocks=" + std::to_string(inv.blocks.size()) + "/" +
             std::to_string(inv.blocks_total);
        if (inv.truncated)
            s += " TRUNCATED";
        if (!inv.closed)
            s += " OPEN (no coverage footer — the capture stopped here)";
        s += "\n";
        for (uint64_t off : inv.insns) {
            auto d = inv.disasm.find(off);
            s += "    " + hex(off) +
                 (d == inv.disasm.end() ? "" : "  " + d->second) + "\n";
        }
    }
    return s;
}

} // namespace asmdesk
