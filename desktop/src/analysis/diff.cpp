// diff.cpp — the alignment seam of diff.h.
#include "analysis/diff.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <map>
#include <set>
#include <utility>

namespace asmdesk {

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

std::map<uint64_t, uint32_t> heat_of(const std::vector<uint64_t> &insns) {
    std::map<uint64_t, uint32_t> h;
    for (uint64_t off : insns)
        h[off]++;
    return h;
}

std::map<std::pair<uint64_t, uint64_t>, uint64_t>
edges_of(const std::vector<SurveyEdge> &v) {
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> m;
    for (const SurveyEdge &e : v)
        m[{e.from, e.to}] += e.count;
    return m;
}

} // namespace

bool dt_diff_build(const Streams &a, const Streams &b, dt_diff &out,
                   std::string &err) {
    out = dt_diff{};
    err.clear();

    // --- preconditions: refuse, never produce a garbage diff --------------
    if (!a.trace.basis_error.empty() || !b.trace.basis_error.empty()) {
        err = "one of these recordings cannot be placed on an axis at all: " +
              (a.trace.basis_error.empty() ? b.trace.basis_error
                                           : a.trace.basis_error);
    } else if (!a.trace.present() || !b.trace.present()) {
        err =
            "both recordings need a trace or coverage stream to be aligned; " +
            std::string(a.trace.present() ? "the second" : "the first") +
            " has neither";
    } else if (a.trace.basis != b.trace.basis) {
        err = "these recordings use different address bases (\"" +
              a.trace.basis + "\" vs \"" + b.trace.basis +
              "\") — a region-relative offset and an absolute address are not "
              "the same place; re-record one to match";
    } else if (!a.arch.empty() && !b.arch.empty() && a.arch != b.arch) {
        err = "these recordings are of different architectures (" + a.arch +
              " vs " + b.arch +
              ") — the same offset is a different instruction";
    } else if (a.code_present && b.code_present && a.code_sha != b.code_sha) {
        // Both recordings now carry a `code` routine identity (28 R1 T1), and
        // the byte hashes differ: they are of different routines, so aligning
        // their offsets would compare unrelated code. This is a finding, not a
        // caveat — refuse it, the same way a basis or arch mismatch is refused.
        err = "these recordings are of different routines (code sha256 " +
              a.code_sha.substr(0, 12) + "… vs " + b.code_sha.substr(0, 12) +
              "…) — the same offset is a different instruction; a diff would "
              "align unrelated code";
    }
    if (!err.empty()) {
        out.err = err;
        return false;
    }

    out.a_truncated = a.truncated;
    out.b_truncated = b.truncated;
    // Routine identity: with a matching `code` header on both sides it is now a
    // FINDING (the byte hashes agree), not the reader's assertion. When either
    // side lacks the header the faithful caveat stands — v1 recordings without a
    // producer that carries stable bytes still exist.
    if (a.code_present && b.code_present) {
        out.identity_note =
            "checked: address basis (" + a.trace.basis + "), arch (" +
            (a.arch.empty() ? std::string("unstated") : a.arch) +
            "), and routine identity (code sha256 " + a.code_sha.substr(0, 12) +
            "… matches).";
    } else {
        out.identity_note =
            "checked: address basis (" + a.trace.basis + ") and arch (" +
            (a.arch.empty() ? std::string("unstated") : a.arch) +
            "). NOT checked: that both recordings are of the same routine — " +
            (a.code_present || b.code_present
                 ? std::string("only ") + (a.code_present ? "the first" : "the "
                                                                          "second") +
                       " recording carries a code identity, so the hashes "
                       "cannot be compared"
                 : std::string("neither .asmtrace header carries a code "
                               "identity")) +
            ", so that is the reader's assertion, not this build's finding.";
    }

    // --- coverage ---------------------------------------------------------
    const auto &ba = a.trace.blocks;
    const auto &bb = b.trace.blocks;
    std::set_difference(ba.begin(), ba.end(), bb.begin(), bb.end(),
                        std::back_inserter(out.only_a));
    std::set_difference(bb.begin(), bb.end(), ba.begin(), ba.end(),
                        std::back_inserter(out.only_b));
    std::set_intersection(ba.begin(), ba.end(), bb.begin(), bb.end(),
                          std::back_inserter(out.both));

    // --- per-offset heat --------------------------------------------------
    auto ha = heat_of(a.trace.insns), hb = heat_of(b.trace.insns);
    std::vector<uint64_t> offs;
    for (const auto &kv : ha)
        offs.push_back(kv.first);
    for (const auto &kv : hb)
        offs.push_back(kv.first);
    std::sort(offs.begin(), offs.end());
    offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
    for (uint64_t off : offs) {
        uint32_t ca = ha.count(off) ? ha[off] : 0;
        uint32_t cb = hb.count(off) ? hb[off] : 0;
        if (ca != cb)
            out.heat.push_back({off, ca, cb});
    }

    // --- hot edges (statistical, kept apart from the exact heat) ----------
    auto ea = edges_of(a.survey), eb = edges_of(b.survey);
    std::vector<std::pair<uint64_t, uint64_t>> keys;
    for (const auto &kv : ea)
        keys.push_back(kv.first);
    for (const auto &kv : eb)
        keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    for (const auto &k : keys) {
        uint64_t ca = ea.count(k) ? ea[k] : 0;
        uint64_t cb = eb.count(k) ? eb[k] : 0;
        if (ca != cb)
            out.edges.push_back({k.first, k.second, ca, cb});
    }

    // --- shared-prefix alignment -----------------------------------------
    out.div = dt_first_divergence(a.trace.insns, b.trace.insns, a.truncated,
                                  b.truncated);
    return true;
}

dt_divergence dt_first_divergence(const std::vector<uint64_t> &ia,
                                  const std::vector<uint64_t> &ib,
                                  bool a_truncated, bool b_truncated) {
    dt_divergence div;
    size_t n = std::min(ia.size(), ib.size());
    size_t i = 0;
    for (; i < n && ia[i] == ib[i]; i++)
        ;
    if (i < n) {
        div.diverged = true;
        div.step = static_cast<uint32_t>(i);
        div.off_a = ia[i];
        div.off_b = ib[i];
    } else if (ia.size() != ib.size()) {
        // One stream ends where the other continues. That IS a divergence —
        // unless the shorter side was truncated, in which case what we know is
        // only that the recording stopped, not that execution did.
        bool short_a = ia.size() < ib.size();
        bool short_side_truncated = short_a ? a_truncated : b_truncated;
        if (!short_side_truncated) {
            div.diverged = true;
            div.step = static_cast<uint32_t>(n);
            div.off_a = short_a ? 0 : ia[n];
            div.off_b = short_a ? ib[n] : 0;
        }
    }
    // The verdict is bounded whenever either side stopped short of the truth.
    div.bounded = a_truncated || b_truncated;
    return div;
}

std::string dt_diff_dump(const dt_diff &d) {
    std::string s;
    if (!d.err.empty())
        return "refused: " + d.err + "\n";
    s += "identity: " + d.identity_note + "\n";
    s += "truncated: a=" + std::string(d.a_truncated ? "yes" : "no") +
         " b=" + (d.b_truncated ? "yes" : "no") + "\n";
    s += "coverage: both=" + std::to_string(d.both.size()) +
         " only_a=" + std::to_string(d.only_a.size()) +
         " only_b=" + std::to_string(d.only_b.size()) + "\n";
    for (uint64_t o : d.only_a)
        s += "  only_a " + hex(o) + "\n";
    for (uint64_t o : d.only_b)
        s += "  only_b " + hex(o) + "\n";
    for (const auto &h : d.heat)
        s += "  heat " + hex(h.off) + " a=" + std::to_string(h.a) +
             " b=" + std::to_string(h.b) + "\n";
    for (const auto &e : d.edges)
        s += "  edge[statistical] " + hex(e.from) + "->" + hex(e.to) +
             " a=" + std::to_string(e.a) + " b=" + std::to_string(e.b) + "\n";
    if (d.div.diverged)
        s += "divergence: step=" + std::to_string(d.div.step) +
             " a=" + hex(d.div.off_a) + " b=" + hex(d.div.off_b) +
             (d.div.bounded ? " (bounded)" : "") + "\n";
    else if (d.div.bounded)
        s += "divergence: none observed within the recorded window (bounded — "
             "at least one recording is truncated)\n";
    else
        s += "divergence: none — the streams are identical\n";
    return s;
}

// --- two-recording state-diff (33 R6 T2) --------------------------------

dt_statediff dt_statediff_build(const Streams &a, const Streams &b) {
    dt_statediff out;

    // Gate on routine identity FIRST, reusing dt_diff_build's exact refusal
    // (basis / arch / code_sha) — a wrong-routine pair is refused before any
    // state is merged, so the merge never compares unrelated code.
    dt_diff gate;
    std::string err;
    if (!dt_diff_build(a, b, gate, err)) {
        out.err = err;
        return out;
    }
    out.identity_note = gate.identity_note;

    // No statediff on one (or both) sides: the merge cannot run — say so rather
    // than present an empty comparison as agreement.
    if (a.statediff.empty() || b.statediff.empty()) {
        out.note =
            (a.statediff.empty() && b.statediff.empty())
                ? "neither recording carries a statediff stream"
                : (a.statediff.empty() ? "the first recording carries no "
                                         "statediff stream"
                                       : "the second recording carries no "
                                         "statediff stream");
        return out;
    }

    out.merged = true;

    // Index each side by absolute step.
    std::map<uint32_t, const StateDelta *> ma, mb;
    for (const StateDelta &d : a.statediff)
        ma[d.step] = &d;
    for (const StateDelta &d : b.statediff)
        mb[d.step] = &d;

    // The union of steps, ascending (std::map iterates sorted).
    std::set<uint32_t> steps;
    for (const auto &kv : ma)
        steps.insert(kv.first);
    for (const auto &kv : mb)
        steps.insert(kv.first);

    for (uint32_t step : steps) {
        auto ia = ma.find(step);
        auto ib = mb.find(step);
        const StateDelta *da = ia != ma.end() ? ia->second : nullptr;
        const StateDelta *db = ib != mb.end() ? ib->second : nullptr;

        // Bounded when a step's delta is absent on a side, or was not COMPUTED
        // (the first held step / an evicted boundary): the comparison there is a
        // floor, never an equality claim.
        bool bounded = da == nullptr || db == nullptr ||
                       !da->computed || !db->computed;

        // Diverging registers: the symmetric difference of the two changed sets,
        // plus registers changed on both sides but to different values. Only when
        // both sides have a computed delta can a real divergence be named; a
        // bounded step contributes its step marker but no false register list.
        std::vector<std::string> regs;
        if (da != nullptr && db != nullptr && da->computed && db->computed) {
            for (const auto &kv : da->changed) {
                auto jt = db->changed.find(kv.first);
                if (jt == db->changed.end() || jt->second != kv.second)
                    regs.push_back(kv.first);
            }
            for (const auto &kv : db->changed) {
                if (da->changed.find(kv.first) == da->changed.end())
                    regs.push_back(kv.first);
            }
            std::sort(regs.begin(), regs.end());
            regs.erase(std::unique(regs.begin(), regs.end()), regs.end());
        }

        if (bounded)
            out.bounded = true;
        // Record a step only when it says something: a real divergence, or a
        // bounded step (whose absence of a verdict is itself a fact a view shows).
        if (!regs.empty() || bounded) {
            dt_state_step ss;
            ss.step = step;
            ss.regs = std::move(regs);
            ss.bounded = bounded;
            out.steps.push_back(std::move(ss));
        }
    }
    return out;
}

std::string dt_statediff_dump(const dt_statediff &d) {
    std::string s;
    if (!d.err.empty())
        return "refused: " + d.err + "\n";
    s += "identity: " + d.identity_note + "\n";
    if (!d.merged) {
        s += "merge: not run — " + d.note + "\n";
        return s;
    }
    s += "merge: " + std::to_string(d.steps.size()) + " step(s)" +
         (d.bounded ? " (bounded — a step lacked a computed delta on a side)"
                    : "") +
         "\n";
    for (const dt_state_step &ss : d.steps) {
        s += "  step " + std::to_string(ss.step);
        if (ss.bounded)
            s += " [bounded]";
        for (const std::string &r : ss.regs)
            s += " " + r;
        s += "\n";
    }
    if (d.steps.empty())
        s += "  none — the two recordings evolve state identically\n";
    return s;
}

} // namespace asmdesk
