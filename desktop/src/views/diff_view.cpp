// diff_view.cpp — the pure builder + dump of diff_view.h. No ImGui, no I/O.
#include "views/diff_view.h"

#include <algorithm>
#include <cstdio>

namespace asmdesk {

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

dt_link link_to(dt_view view, const std::string &rec,
                const std::string &rec_b) {
    dt_link l;
    l.view = view;
    l.rec = rec;
    l.rec_b = rec_b;
    return l;
}

const char *kind_name(dt_diff_row_kind k) {
    switch (k) {
    case dt_diff_row_kind::header:
        return "header";
    case dt_diff_row_kind::coverage:
        return "coverage";
    case dt_diff_row_kind::heat:
        return "heat";
    case dt_diff_row_kind::edge:
        return "edge";
    case dt_diff_row_kind::divergence:
        return "divergence";
    case dt_diff_row_kind::note:
        return "note";
    }
    return "note";
}

} // namespace

dt_diff_view dt_diff_view_build(const Streams &a, const Streams &b,
                                size_t top_heat) {
    dt_diff_view v;
    v.a_id = a.id;
    v.b_id = b.id;

    std::string err;
    if (!dt_diff_build(a, b, v.diff, err)) {
        v.refusal = err;
        v.rows.push_back({dt_diff_row_kind::note,
                          "these recordings cannot be compared: " + err, false,
                          ""});
        return v;
    }
    const dt_diff &d = v.diff;

    v.rows.push_back(
        {dt_diff_row_kind::header, "A = " + a.id + "  B = " + b.id, false, ""});
    // The identity gap is a first-class row, not a footnote: the reader is the
    // one asserting these are the same routine, and they should be told so.
    v.rows.push_back({dt_diff_row_kind::note, d.identity_note, false, ""});
    if (d.a_truncated || d.b_truncated)
        v.rows.push_back({dt_diff_row_kind::note,
                          std::string("TRUNCATED: ") +
                              (d.a_truncated && d.b_truncated
                                   ? "both recordings are"
                                   : (d.a_truncated ? "A is" : "B is")) +
                              " truncated — every count below is a lower bound",
                          false, ""});

    v.rows.push_back({dt_diff_row_kind::coverage,
                      "blocks: both=" + std::to_string(d.both.size()) +
                          "  A-only=" + std::to_string(d.only_a.size()) +
                          "  B-only=" + std::to_string(d.only_b.size()),
                      false, ""});
    for (uint64_t off : d.only_a) {
        dt_link l = link_to(dt_view::canvas, a.id, b.id);
        l.off = off;
        v.rows.push_back({dt_diff_row_kind::coverage,
                          "A-only block " + hex(off), false, dt_nav_format(l)});
    }
    for (uint64_t off : d.only_b) {
        dt_link l = link_to(dt_view::canvas, b.id, a.id);
        l.off = off;
        v.rows.push_back({dt_diff_row_kind::coverage,
                          "B-only block " + hex(off), false, dt_nav_format(l)});
    }

    // Heat deltas, largest absolute change first (ties by offset, so the order
    // is total and the dump is byte-stable).
    std::vector<dt_heat_delta> heat = d.heat;
    std::sort(heat.begin(), heat.end(),
              [](const dt_heat_delta &x, const dt_heat_delta &y) {
                  uint32_t dx = x.a > x.b ? x.a - x.b : x.b - x.a;
                  uint32_t dy = y.a > y.b ? y.a - y.b : y.b - y.a;
                  if (dx != dy)
                      return dx > dy;
                  return x.off < y.off;
              });
    size_t shown = std::min(top_heat, heat.size());
    for (size_t i = 0; i < shown; i++) {
        dt_link l = link_to(dt_view::canvas, a.id, b.id);
        l.off = heat[i].off;
        v.rows.push_back({dt_diff_row_kind::heat,
                          "heat " + hex(heat[i].off) +
                              ": A=" + std::to_string(heat[i].a) +
                              " B=" + std::to_string(heat[i].b),
                          false, dt_nav_format(l)});
    }
    if (shown < heat.size())
        v.rows.push_back({dt_diff_row_kind::note,
                          "showing the " + std::to_string(shown) +
                              " largest "
                              "of " +
                              std::to_string(heat.size()) + " changed offsets",
                          false, ""});

    // Hot edges keep their provenance. They are sampled evidence that an edge
    // was taken — never evidence that one was not, and never exact heat.
    for (const dt_edge_delta &e : d.edges)
        v.rows.push_back({dt_diff_row_kind::edge,
                          "edge " + hex(e.from) + "->" + hex(e.to) + ": A=" +
                              std::to_string(e.a) + " B=" + std::to_string(e.b),
                          true, ""});

    if (d.div.diverged) {
        dt_link la = link_to(dt_view::timeline, a.id, b.id);
        la.step = d.div.step;
        la.off = d.div.off_a;
        auto disasm_at = [](const Streams &s, uint64_t off) {
            auto it = s.trace.disasm.find(off);
            return it == s.trace.disasm.end() ? std::string("(no disasm)")
                                              : it->second;
        };
        v.rows.push_back(
            {dt_diff_row_kind::divergence,
             "PATIENT ZERO at step " + std::to_string(d.div.step) + ": A " +
                 hex(d.div.off_a) + " " + disasm_at(a, d.div.off_a) +
                 "  |  B " + hex(d.div.off_b) + " " + disasm_at(b, d.div.off_b),
             false, dt_nav_format(la)});
    } else if (d.div.bounded) {
        // NOT "identical": one side stopped short, so agreement past the
        // recorded prefix was never observed.
        v.rows.push_back({dt_diff_row_kind::divergence,
                          "no divergence observed within the recorded window "
                          "(at least one recording is truncated, so this is "
                          "not a claim that the runs are identical)",
                          false, ""});
    } else {
        v.rows.push_back({dt_diff_row_kind::divergence,
                          "no divergence: the recorded instruction streams are "
                          "identical",
                          false, ""});
    }
    return v;
}

std::string dt_diff_view_dump(const dt_diff_view &v) {
    std::string s = "A=" + v.a_id + " B=" + v.b_id + "\n";
    if (!v.refusal.empty())
        s += "REFUSED: " + v.refusal + "\n";
    for (const dt_diff_row &r : v.rows) {
        s += std::string(kind_name(r.kind)) + "|";
        s += r.statistical ? "statistical|" : "exact|";
        s += r.text;
        if (!r.link.empty())
            s += "  -> " + r.link;
        s += "\n";
    }
    return s;
}

} // namespace asmdesk
