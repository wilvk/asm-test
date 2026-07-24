// canvas.cpp — the pure builder + dump of canvas.h. No ImGui, no I/O.
#include "views/canvas.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

#include "analysis/diff.h"

namespace asmdesk {

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// The truncation banner. Always visible when set, never collapsible: a heat map
// computed over part of a run looks exactly like one computed over all of it.
std::string build_banner(const Streams &s) {
    if (!s.truncated && s.lost == 0 && !s.throttled && s.df.steps_missing == 0)
        return {};
    std::string b = "TRUNCATED: ";
    size_t recorded = s.trace.insns.size();
    if (s.trace.insns_total > recorded)
        b += "heat computed over " + std::to_string(recorded) + " of " +
             std::to_string(s.trace.insns_total) + " instructions";
    else
        b += "heat computed over the " + std::to_string(recorded) +
             " instructions that were recorded, which is not all of them";
    if (s.trace.blocks_total > s.trace.blocks.size())
        b += "; " + std::to_string(s.trace.blocks.size()) + " of " +
             std::to_string(s.trace.blocks_total) + " blocks";
    if (s.torn)
        b += "; TORN — the producer died mid-record, so there is no footer";
    if (s.lost > 0)
        b += "; drops: lost=" + std::to_string(s.lost);
    if (s.throttled)
        b += "; drops: throttled";
    return b;
}

void fill_rows(dt_canvas &c, const Streams &s) {
    std::map<uint64_t, uint32_t> heat;
    for (uint64_t off : s.trace.insns)
        heat[off]++;
    std::set<uint64_t> offs;
    for (const auto &kv : heat)
        offs.insert(kv.first);
    for (uint64_t b : s.trace.blocks)
        offs.insert(b);
    for (const auto &kv : s.trace.disasm)
        offs.insert(kv.first);

    std::set<uint64_t> blocks(s.trace.blocks.begin(), s.trace.blocks.end());
    for (uint64_t off : offs) {
        dt_canvas_row r;
        r.off = off;
        auto h = heat.find(off);
        r.heat = h == heat.end() ? 0 : h->second;
        r.block_start = blocks.count(off) != 0;
        r.covered = r.block_start;
        auto d = s.trace.disasm.find(off);
        if (d != s.trace.disasm.end())
            r.disasm = d->second;
        // in_a/in_b are the COVERAGE gutter's two sides, so they track block
        // membership — not "this offset appeared", which is heat's job.
        r.in_a = r.block_start;
        c.rows.push_back(r);
    }
}

} // namespace

dt_canvas dt_canvas_build(const Streams &s) {
    dt_canvas c;
    c.basis = s.trace.basis;
    c.truncated = s.truncated;
    c.banner = build_banner(s);
    if (!s.trace.basis_error.empty()) {
        // Refuse: no rows at all. Anything drawn here would be mis-placed.
        c.basis_error = s.trace.basis_error;
        return c;
    }
    fill_rows(c, s);
    return c;
}

dt_canvas dt_canvas_build2(const Streams &a, const Streams &b) {
    dt_canvas c = dt_canvas_build(a);
    c.two_up = true;
    if (!c.basis_error.empty())
        return c;
    if (!b.trace.basis_error.empty()) {
        c.basis_error =
            "the second recording cannot be placed: " + b.trace.basis_error;
        c.rows.clear();
        return c;
    }
    if (!a.trace.basis.empty() && !b.trace.basis.empty() &&
        a.trace.basis != b.trace.basis) {
        c.basis_error = "these two recordings use different address bases (\"" +
                        a.trace.basis + "\" vs \"" + b.trace.basis +
                        "\") — their offsets are not comparable";
        c.rows.clear();
        return c;
    }

    std::map<uint64_t, uint32_t> hb;
    for (uint64_t off : b.trace.insns)
        hb[off]++;
    std::set<uint64_t> bblocks(b.trace.blocks.begin(), b.trace.blocks.end());

    // Rows are the UNION of both sides' offsets: an offset only B executed is a
    // fact about the pair, and dropping it would hide B-only coverage.
    std::map<uint64_t, dt_canvas_row> by_off;
    for (const dt_canvas_row &r : c.rows)
        by_off[r.off] = r;
    for (const auto &kv : hb)
        by_off[kv.first].off = kv.first;
    for (uint64_t off : bblocks)
        by_off[off].off = off;
    for (const auto &kv : b.trace.disasm)
        if (by_off[kv.first].disasm.empty()) {
            by_off[kv.first].off = kv.first;
            by_off[kv.first].disasm = kv.second;
        }

    c.rows.clear();
    for (auto &kv : by_off) {
        dt_canvas_row &r = kv.second;
        r.off = kv.first;
        auto h = hb.find(r.off);
        r.heat_b = h == hb.end() ? 0 : h->second;
        r.in_b = bblocks.count(r.off) != 0;
        r.delta = true;
        c.rows.push_back(r);
    }

    // The divergence marker comes from the ONE alignment implementation
    // (analysis/diff.cpp) rather than a second copy of the prefix walk.
    dt_diff d;
    std::string err;
    if (dt_diff_build(a, b, d, err) && d.div.diverged) {
        c.div_step = d.div.step;
        c.div_off = d.div.off_a;
    }
    if (b.truncated && !c.banner.empty())
        c.banner += " | the B recording is truncated too";
    else if (b.truncated)
        c.banner = "TRUNCATED: the B recording is truncated — its heat is a "
                   "lower bound";
    return c;
}

std::string dt_canvas_dump(const dt_canvas &c) {
    std::string s;
    s += "basis=" + (c.basis.empty() ? std::string("(none)") : c.basis) + "\n";
    if (!c.basis_error.empty())
        return s + "REFUSED: " + c.basis_error + "\nrows=0\n";
    if (!c.banner.empty())
        s += "banner=" + c.banner + "\n";
    s += "rows=" + std::to_string(c.rows.size()) + "\n";
    if (c.div_step)
        s += "divergence step=" + std::to_string(*c.div_step) +
             " off=" + hex(c.div_off ? *c.div_off : 0) + "\n";
    for (const dt_canvas_row &r : c.rows) {
        s += hex(r.off) + "|" + std::to_string(r.heat);
        if (c.two_up)
            s += "/" + std::to_string(r.heat_b);
        s += "|";
        s += r.block_start ? "B" : "-";
        s += r.covered ? "C" : "-";
        if (c.two_up) {
            s += "|";
            s += r.in_a ? "a" : "-";
            s += r.in_b ? "b" : "-";
        }
        s += "|" + (r.disasm.empty() ? std::string("(no disasm)") : r.disasm);
        s += "\n";
    }
    return s;
}

} // namespace asmdesk
