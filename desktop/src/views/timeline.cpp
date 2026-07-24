// timeline.cpp — the pure builder + dump of timeline.h. No ImGui, no I/O.
#include "views/timeline.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "analysis/diff.h"

// clang-format off
// asmspy_dataview.h is a C header of INLINE functions over plain structs; it
// pulls in asmtest_valtrace.h for the types. Nothing it defines is implemented
// in src/, so including it adds no link dependency and the render-only viewer
// stays engine-free (D4). The include order is load-bearing — sorting would put
// the C++ headers between them.
extern "C" {
#include "asmtest_valtrace.h"
#include "asmspy_dataview.h"
}
// clang-format on

namespace asmdesk {

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

at_loc_kind_t space_of(const std::string &s) {
    if (s == "reg")
        return AT_LOC_REG;
    if (s == "off")
        return AT_LOC_MEM_OFF;
    return AT_LOC_MEM_ABS;
}

// Rebuild the engine's operand records from the decoded stream. This is a VIEW
// in the sense that it copies nothing the recording did not carry: a value the
// producer never captured stays value_valid=false, and a >8-byte value stays
// `wide` with no bytes behind it (the v1 schema does not serialise the side
// buffer, a documented limit — inventing bytes here would be a lie the
// annotation grammar would faithfully print).
std::vector<at_val_rec_t> to_recs(const DataflowStream &df) {
    std::vector<at_val_rec_t> v;
    v.reserve(df.recs.size());
    for (const ValRec &r : df.recs) {
        at_val_rec_t o{};
        o.kind = space_of(r.space);
        o.reg = r.reg;
        o.addr = r.addr;
        o.size = static_cast<uint16_t>(r.size);
        o.is_write = r.write;
        o.value_valid = r.value_valid;
        o.wide = r.wide;
        o.value = r.value;
        o.step = r.step;
        v.push_back(o);
    }
    return v;
}

std::vector<asmtest_defuse_edge_t> to_edges(const DataflowStream &df) {
    std::vector<asmtest_defuse_edge_t> v;
    v.reserve(df.edges.size());
    for (size_t i = 0; i < df.edges.size(); i++) {
        asmtest_defuse_edge_t e{};
        e.from_step = df.edges[i].from_step;
        e.to_step = df.edges[i].to_step;
        if (i < df.edge_loc.size()) {
            const ValRec &l = df.edge_loc[i];
            e.loc.kind = space_of(l.space);
            e.loc.reg = l.reg;
            e.loc.addr = l.addr;
            e.loc.size = static_cast<uint16_t>(l.size);
            e.loc.is_write = l.write;
            e.loc.value_valid = l.value_valid;
            e.loc.wide = l.wide;
            e.loc.value = l.value;
        }
        v.push_back(e);
    }
    return v;
}

std::string build_banner(const Streams &s) {
    std::string b;
    const DataflowStream &df = s.df;
    if (s.truncated || df.steps_missing > 0 || s.lost > 0 || s.throttled) {
        b = "TRUNCATED: annotations computed over " +
            std::to_string(df.nsteps - df.steps_missing) + " of " +
            std::to_string(df.nsteps) + " steps";
        b += ", " + std::to_string(df.recs.size()) + " operand records";
        if (df.steps_missing > 0)
            b += "; " + std::to_string(df.steps_missing) +
                 " step(s) have no df_step event and are shown blank, not as "
                 "offset 0";
        if (s.torn)
            b += "; TORN — no end footer, the producer died mid-record";
        if (s.lost > 0)
            b += "; lost=" + std::to_string(s.lost);
        if (s.throttled)
            b += "; throttled";
    }
    if (!df.recs_grouped) {
        // asmspy_df_annotate walks records with a forward-only cursor. The
        // decoder sorts, so annotation is still correct — but the file was not
        // in the order the format promises, and that is worth saying.
        if (!b.empty())
            b += " | ";
        b += "NOTE: this recording's df_step events were not in ascending step "
             "order; they were sorted before annotating";
    }
    return b;
}

} // namespace

dt_rowstyle dt_row_style(const dt_slice *cone, uint32_t step) {
    if (cone == nullptr)
        return dt_rowstyle::normal;
    return cone->contains(step) ? dt_rowstyle::in_slice : dt_rowstyle::dimmed;
}

dt_timeline dt_timeline_build(const Streams &s, const dt_slice *cone) {
    dt_timeline t;
    t.truncated = s.truncated;
    t.banner = build_banner(s);

    const DataflowStream &df = s.df;
    std::vector<at_val_rec_t> recs = to_recs(df);
    std::vector<asmtest_defuse_edge_t> edges = to_edges(df);

    asmtest_valtrace_t vt{};
    vt.insn_off = const_cast<uint64_t *>(
        df.insn_off.empty() ? nullptr : df.insn_off.data());
    vt.steps_cap = vt.steps_len = df.insn_off.size();
    vt.steps_total = df.nsteps;
    vt.recs = recs.empty() ? nullptr : recs.data();
    vt.recs_cap = vt.recs_len = recs.size();
    vt.recs_total = recs.size();
    vt.truncated = s.truncated;
    vt.mem_space = AT_LOC_MEM_ABS;

    asmtest_defuse_t g{};
    g.edges = edges.empty() ? nullptr : edges.data();
    g.n = edges.size();
    g.nsteps = df.nsteps;

    size_t cur = 0; // threaded top-down: annotate's documented cursor contract
    for (uint32_t step = 0; step < df.nsteps; step++) {
        dt_timeline_row r;
        r.step = step;
        r.missing = !df.has_step(step);
        r.off = df.insn_off[step];
        r.disasm = df.disasm[step];
        char ann[256];
        asmspy_df_annotate(&vt, step, &cur, 4, ann, sizeof ann);
        r.ann = ann;
        asmspy_df_defuse_counts(&g, step, &r.n_in, &r.n_out);
        r.style = dt_row_style(cone, step);
        t.rows.push_back(r);
    }
    return t;
}

dt_timeline dt_timeline_build2(const Streams &a, const Streams &b,
                               const dt_slice *cone) {
    dt_timeline t = dt_timeline_build(a, cone);
    t.two_up = true;
    dt_diff d;
    std::string err;
    if (!dt_diff_build(a, b, d, err)) {
        t.banner = (t.banner.empty() ? "" : t.banner + " | ") +
                   ("NOT COMPARABLE: " + err);
        return t;
    }
    if (!d.div.diverged)
        return t;
    t.div_step = d.div.step;
    // Past the divergence the two runs are executing different code, so those
    // rows are NOT an A/B comparison of the same thing. Mark them; the draw
    // separates them. Rendering them as agreement is the Loom's cardinal sin.
    for (dt_timeline_row &r : t.rows)
        if (r.step >= d.div.step)
            r.unaligned = true;
    return t;
}

std::string dt_timeline_dump(const dt_timeline &t) {
    std::string s;
    if (!t.banner.empty())
        s += "banner=" + t.banner + "\n";
    s += "rows=" + std::to_string(t.rows.size()) + "\n";
    if (t.div_step)
        s += "divergence step=" + std::to_string(*t.div_step) + "\n";
    for (const dt_timeline_row &r : t.rows) {
        if (r.missing) {
            s += std::to_string(r.step) +
                 "|(unknown)|(no df_step event — this step was dropped)||" +
                 std::to_string(r.n_in) + "/" + std::to_string(r.n_out) + "\n";
            continue;
        }
        s += std::to_string(r.step) + "|" + hex(r.off) + "|" +
             (r.disasm.empty() ? hex(r.off) : r.disasm) + "|" + r.ann + "|" +
             std::to_string(r.n_in) + "/" + std::to_string(r.n_out);
        switch (r.style) {
        case dt_rowstyle::normal:
            break;
        case dt_rowstyle::in_slice:
            s += "|IN-SLICE";
            break;
        case dt_rowstyle::dimmed:
            s += "|DIMMED";
            break;
        }
        if (r.unaligned)
            s += "|UNALIGNED";
        s += "\n";
    }
    return s;
}

} // namespace asmdesk
