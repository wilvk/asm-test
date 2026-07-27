// capview.cpp — the pure capability view-model of capview.h. No ImGui, no
// probing: every number here arrived as an argument.
#include "capview.h"

#include <climits>
#include <cstdio>

#include "live/inspect.h" // attach_verdict — reuse the remedy map (T4, F19)

namespace asmdesk {

const char *const kCapNativeOnlyEmpty =
    "no native tier on this host — the library returns EUNAVAIL rather than "
    "silently downgrading; untick to allow the virtual floor (explicit choice)";
const char *const kCapFidelityLine = "native → virtual fidelity line";
const char *const kCapStatisticalChip = "sampled, never exact";
const char *const kCapViewerNoProbe =
    "render-only viewer: this shows the LOADED RECORDING's provenance, not "
    "this "
    "host's capabilities — a viewer probes nothing";
const char *const kCapLearnAuthorFloor =
    "Learn and Author work here — no root, hardware, or attach needed";
const char *const kCapWhyNotHeader = "why can't I capture X?";

const char *cap_stage_name(int stage) {
    switch (stage) {
    case ASMTEST_HW_STAGE_OK:
        return "ok";
    case ASMTEST_HW_STAGE_DECODER:
        return "decoder";
    case ASMTEST_HW_STAGE_CPU:
        return "cpu";
    case ASMTEST_HW_STAGE_PMU:
        return "pmu";
    case ASMTEST_HW_STAGE_PROBE:
        return "probe";
    }
    return "?";
}

const char *cap_backend_name(int backend) {
    switch (backend) {
    case ASMTEST_HWTRACE_INTEL_PT:
        return "Intel PT";
    case ASMTEST_HWTRACE_CORESIGHT:
        return "ARM CoreSight";
    case ASMTEST_HWTRACE_AMD_LBR:
        return "AMD LBR";
    case ASMTEST_HWTRACE_SINGLESTEP:
        return "single-step (TF)";
    }
    return "?";
}

const char *cap_tier_name(int tier) {
    switch (tier) {
    case ASMTEST_TIER_HWTRACE:
        return "hwtrace";
    case ASMTEST_TIER_DYNAMORIO:
        return "dynamorio";
    case ASMTEST_TIER_EMULATOR:
        return "emulator";
    }
    return "?";
}

const char *cap_fidelity_name(int fidelity) {
    switch (fidelity) {
    case ASMTEST_FIDELITY_NATIVE:
        return "native";
    case ASMTEST_FIDELITY_VIRTUAL:
        return "virtual";
    case ASMTEST_FIDELITY_STATISTICAL:
        return "statistical";
    }
    return "?";
}

std::vector<cap_row> capview_build(const asmtest_trace_choice_t *cascade,
                                   size_t n,
                                   const asmtest_hwtrace_status_t st[4],
                                   int ibs_avail, const char *ibs_substrate,
                                   const char *ibs_capture, bool native_only) {
    std::vector<cap_row> rows;

    // --- the resolved cascade, in the library's order ----------------------
    for (size_t i = 0; i < n; i++) {
        const asmtest_trace_choice_t &c = cascade[i];
        cap_row r;
        r.kind = cap_kind::cascade;
        r.label = std::string(cap_tier_name(c.tier));
        if (c.tier == ASMTEST_TIER_HWTRACE)
            r.label += " / " + std::string(cap_backend_name(c.backend));
        r.available = true;
        r.fidelity = c.fidelity;
        r.statistical = c.fidelity == ASMTEST_FIDELITY_STATISTICAL;
        r.chip =
            r.statistical ? kCapStatisticalChip : cap_fidelity_name(c.fidelity);
        r.below_fidelity_line = c.fidelity != ASMTEST_FIDELITY_NATIVE;
        rows.push_back(std::move(r));
    }

    // The refusal, and ONLY when the native-only cascade really is empty. It is
    // a row rather than a toast because it is the answer to the question the
    // panel exists to ask.
    if (n == 0 && native_only) {
        cap_row r;
        r.kind = cap_kind::refusal;
        r.label = "native only";
        r.reason = kCapNativeOnlyEmpty;
        r.available = false;
        rows.push_back(std::move(r));
    }

    // --- one row per hardware backend, with its MEASURED reason ------------
    for (int b = 0; b < 4; b++) {
        cap_row r;
        r.kind = cap_kind::backend;
        r.label = cap_backend_name(b);
        r.available = st[b].available != 0;
        r.code = st[b].code;
        r.stage = st[b].stage;
        r.paranoid = st[b].perf_event_paranoid;
        // UI LAW 1: a greyed row always carries its machine reason, verbatim.
        if (!r.available)
            r.reason = st[b].reason;
        r.chip = std::string("stage: ") + cap_stage_name(st[b].stage);
        if (!r.available && st[b].perf_event_paranoid != INT_MIN) {
            char p[64];
            std::snprintf(p, sizeof p, "; perf_event_paranoid=%d",
                          st[b].perf_event_paranoid);
            r.chip += p;
        }
        rows.push_back(std::move(r));
    }

    // --- IBS: BOTH reasons, labelled, never collapsed ----------------------
    // They answer different questions — "is the substrate here?" and "why did
    // the last capture fail?" — and skip_reason() is "" BY CONSTRUCTION in the
    // case an operator actually cares about.
    {
        cap_row r;
        r.kind = cap_kind::ibs;
        r.label = "AMD IBS-Op (sampling)";
        r.available = ibs_avail != 0;
        r.statistical = true;
        r.chip = kCapStatisticalChip;
        r.below_fidelity_line = false; // sampling is still real silicon
        std::string sub = ibs_substrate == nullptr ? "" : ibs_substrate;
        std::string cap = ibs_capture == nullptr ? "" : ibs_capture;
        r.reason =
            "substrate: " + (sub.empty() ? std::string("(present)") : sub) +
            "\nlast capture: " +
            (cap.empty() ? std::string("(nothing has failed)") : cap);
        rows.push_back(std::move(r));
    }

    // --- the emulator floor, below the fidelity line -----------------------
    {
        cap_row r;
        r.kind = cap_kind::emulator;
        r.label = "emulator (Unicorn guest)";
        r.available = true;
        r.fidelity = ASMTEST_FIDELITY_VIRTUAL;
        r.chip = "isolated guest — not silicon";
        r.below_fidelity_line = true;
        rows.push_back(std::move(r));
    }
    return rows;
}

std::string capview_summary(const std::vector<cap_row> &rows) {
    // Lead with what WORKS. An available cascade/backend/ibs/emulator row is a
    // capability the host has; collect their labels in the order they were built
    // so the summary reads the same as the deck below it.
    std::vector<std::string> can, cannot;
    for (const cap_row &r : rows) {
        if (r.kind == cap_kind::refusal)
            continue;
        (r.available ? can : cannot).push_back(r.label);
    }
    std::string s = "This host: ";
    if (can.empty()) {
        s += "no capture backend available";
    } else {
        for (size_t i = 0; i < can.size(); i++)
            s += (i ? ", " : "") + can[i];
        s += " available";
    }
    if (!cannot.empty()) {
        s += "; ";
        for (size_t i = 0; i < cannot.size(); i++)
            s += (i ? ", " : "") + cannot[i];
        s += " unavailable — details below";
    }
    s += ". ";
    s += kCapLearnAuthorFloor;
    return s;
}

std::string capview_remedy(const cap_row &r) {
    if (r.available)
        return std::string(); // nothing to remedy on a working backend
    const std::string reason = r.reason;
    auto has = [&](const char *n) {
        return reason.find(n) != std::string::npos;
    };

    // perf_event_paranoid is the most common capability gate, and attach_verdict
    // (a ptrace-attach map) has no branch for it — so it is answered here, in the
    // panel's own terms. Recognised from the verbatim reason ONLY: a backend that
    // merely carries a paranoid LEVEL but fails for another cause (a wrong-vendor
    // LBR, a wrong-ISA CoreSight) must NOT be told to lower a sysctl that would
    // not help.
    if (has("perf_event_paranoid") || has("paranoid"))
        return "lower /proc/sys/kernel/perf_event_paranoid to <= 2 "
               "(sysctl kernel.perf_event_paranoid=2), or grant the process "
               "CAP_PERFMON — the reason above names the exact level";

    // The ptrace family REUSES the inspect_door attach_verdict map, so the two
    // panels never give divergent advice. Synthesise the AttachFacts the reason
    // implies and read back its remedy.
    if (has("i386") || has("32-bit")) {
        AttachFacts f;
        f.elf_class = 32;
        return attach_verdict(f).remedy;
    }
    if (has("ptrace_scope=2") || has("ptrace_scope") || has("Yama") ||
        has("CAP_SYS_PTRACE")) {
        AttachFacts f;
        f.yama_scope = 2; // the scope whose remedy is exactly "grant CAP_SYS_PTRACE"
        return attach_verdict(f).remedy;
    }
    return std::string();
}

std::string capview_dump(const std::vector<cap_row> &rows) {
    std::string o;
    for (const cap_row &r : rows) {
        o += r.available ? "[ok]   " : "[grey] ";
        o += r.label;
        if (!r.chip.empty())
            o += "  (" + r.chip + ")";
        if (r.below_fidelity_line)
            o += "  [below the fidelity line]";
        o += "\n";
        if (!r.reason.empty()) {
            // Multi-line reasons (IBS's two labelled strings) stay multi-line:
            // collapsing them is exactly the "never collapse them" rule.
            std::string s = r.reason;
            size_t pos = 0, nl;
            while ((nl = s.find('\n', pos)) != std::string::npos) {
                o += "       " + s.substr(pos, nl - pos) + "\n";
                pos = nl + 1;
            }
            o += "       " + s.substr(pos) + "\n";
        }
    }
    return o;
}

} // namespace asmdesk
