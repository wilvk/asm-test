// terrain.cpp — the density height field over time of terrain.h. Standard
// library + the document model + the trace canvas + the projection (D4): no GL,
// no ImGui, no engine.
#include "space/terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

#include "doc/streams.h"
#include "views/canvas.h"

namespace asmdesk {
namespace space {

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// The plane cell an address projects into, or ok=false when it maps to no region.
// Rounding of u,v back to (x,y) mirrors projection.cpp's own (x+0.5)/n, so a
// projected address and its cell agree.
uint32_t cell_of(const Projection &proj, uint32_t w, uint32_t h, uint64_t addr,
                 bool *ok) {
    float u = 0, v = 0;
    if (!proj.project(addr, &u, &v)) {
        *ok = false;
        return 0;
    }
    uint32_t x = static_cast<uint32_t>(u * w);
    uint32_t y = static_cast<uint32_t>(v * h);
    if (x >= w)
        x = w - 1;
    if (y >= h)
        y = h - 1;
    *ok = true;
    return y * w + x;
}

// The SEPARATE statistical layer: `survey` residency projected onto its own
// Terrain, every populated cell flagged STAT. Built independently of the trace
// basis (a mixed-basis trace does not taint a sampled survey), and NEVER merged
// into the exact terrain — the T6 isolation invariant.
void build_stat(TerrainModel &m, const Projection &proj, const Streams &s) {
    if (s.survey.empty())
        return;
    // Residency is credited to each edge's arrival (`to`) cell: where the sampled
    // control flow landed. An edge whose target maps to no region is dropped.
    std::map<uint32_t, uint64_t> resid;
    for (const SurveyEdge &e : s.survey) {
        bool ok = false;
        uint32_t c = cell_of(proj, m.w, m.h, e.to, &ok);
        if (ok)
            resid[c] += (e.count ? e.count : 1);
    }
    if (resid.empty())
        return;
    m.has_stat = true;
    m.stat.w = m.w;
    m.stat.h = m.h;
    m.stat.height.assign(static_cast<size_t>(m.w) * m.h, 0.0f);
    m.stat.flags.assign(static_cast<size_t>(m.w) * m.h, 0u);
    for (const auto &kv : resid) {
        m.stat.height[kv.first] = std::log1p(static_cast<float>(kv.second));
        m.stat.flags[kv.first] |= TF_STAT; // the whole layer is statistical
    }
}

} // namespace

std::vector<Region> regions_from_codeimage(const Recording &rec) {
    std::vector<Region> out;
    auto it = rec.by_kind.find("codeimage");
    if (it == rec.by_kind.end())
        return out;
    // One Region per distinct base; keep the widest len and the latest version.
    std::map<uint64_t, Region> by_base;
    for (const Event &e : it->second) {
        uint64_t base = e.body.value("base", uint64_t{0});
        uint64_t len = e.body.value("len", uint64_t{0});
        uint64_t ver = e.body.value("version", uint64_t{0});
        auto f = by_base.find(base);
        if (f == by_base.end()) {
            Region r;
            r.base = base;
            r.len = len;
            r.kind = Region::Code;
            r.version = ver;
            r.label = "code@" + hex(base);
            by_base.emplace(base, std::move(r));
        } else {
            if (len > f->second.len)
                f->second.len = len;
            if (ver > f->second.version)
                f->second.version = ver;
        }
    }
    for (auto &kv : by_base)
        out.push_back(std::move(kv.second));
    return out;
}

TerrainModel build_terrain(Projection proj, const Recording &rec) {
    TerrainModel m;
    m.w = m.h = uint32_t{1} << proj.order;

    Streams s = decode_streams(rec);
    // REUSE 04-T3: the trace canvas owns the per-offset execution count and the
    // basis handling; the coarse height is that count, not a re-derivation.
    dt_canvas canvas = dt_canvas_build(s);

    // Truncation is a property of the whole recording: a footer that declares it,
    // a torn tail with no footer, or dropped events — all mean the heights are a
    // floor, so the touched cells are flagged TORN at slice time.
    m.torn = rec.truncated() || rec.dropped();
    m.basis = canvas.basis;

    // The rich rung's `mem` stream is gated at runtime on the kind being present
    // (there is no producer yet, so a real recording never carries it — the path
    // stays inert). Absent it, data cells stay flat and the note says so, never a
    // silent zero.
    m.mem_present = rec.by_kind.count("mem") != 0;
    m.mem_note =
        m.mem_present ? std::string() : "coarse: no per-access memory stream";

    // The statistical layer is independent of the exact terrain and is built even
    // when the trace refuses (below): a survey is untainted by a mixed-basis
    // trace.
    build_stat(m, proj, s);

    if (!canvas.basis_error.empty()) {
        // Mixed bases: the canvas places no row, and neither can the exact terrain
        // — a density stacked over two incompatible address bases would be wrong
        // at every cell. slice() stays flat; the HUD shows basis_error.
        m.basis_error = canvas.basis_error;
        m.proj = std::move(proj);
        return m;
    }

    // The coarse height source, straight from the canvas (NOT recomputed here):
    // a cell's full heat is the SUM of the canvas per-offset execution counts of
    // the offsets that project into it. The ordered insn stream below is the same
    // multiset the canvas counted; it supplies only the STEP ORDER the canvas
    // discards, so slice(t) can time-slice — and slice(full) reproduces exactly
    // this canvas-sourced heat (pinned in test_terrain.cpp).
    std::map<uint32_t, uint32_t> full_heat_by_cell;
    for (const dt_canvas_row &r : canvas.rows) {
        bool ok = false;
        uint32_t c = cell_of(proj, m.w, m.h, r.off, &ok);
        if (ok)
            full_heat_by_cell[c] += r.heat;
    }

    // --- churn timing --------------------------------------------------------
    // Walk trace + codeimage in stream (seq) order, counting the trace steps that
    // precede each version increase. churn_step[base] is the count before the
    // FIRST bump of that base, so a slice t (inclusive) shows CHURN over the base
    // once t >= churn_step[base].
    struct SE {
        uint64_t seq;
        const Event *ev;
        bool ci;
    };
    std::vector<SE> ord;
    if (auto it = rec.by_kind.find("trace"); it != rec.by_kind.end())
        for (const Event &e : it->second)
            ord.push_back({e.seq, &e, false});
    if (auto it = rec.by_kind.find("codeimage"); it != rec.by_kind.end())
        for (const Event &e : it->second)
            ord.push_back({e.seq, &e, true});
    std::sort(ord.begin(), ord.end(),
              [](const SE &a, const SE &b) { return a.seq < b.seq; });

    std::map<uint64_t, uint64_t> churn_step; // base -> step of the first bump
    {
        std::map<uint64_t, uint64_t> last_ver;
        uint64_t step = 0;
        for (const SE &se : ord) {
            if (!se.ci) {
                // Count a step only for a trace event that PLACES an offset, to
                // match decode_streams (an offset-less trace event is not a step).
                auto o = se.ev->body.find("off");
                if (o != se.ev->body.end() && o->is_number())
                    step++;
                continue;
            }
            uint64_t base = se.ev->body.value("base", uint64_t{0});
            uint64_t ver = se.ev->body.value("version", uint64_t{0});
            auto lv = last_ver.find(base);
            if (lv == last_ver.end()) {
                last_ver[base] = ver;
                continue;
            }
            if (ver > lv->second) {
                if (!churn_step.count(base))
                    churn_step[base] = step;
                lv->second = ver;
            }
        }
    }
    m.churn_present = !churn_step.empty();

    // --- coarse rung: per-cell ordered code hits -----------------------------
    std::map<uint32_t, std::vector<uint64_t>> cell_steps;
    uint64_t step = 0;
    for (uint64_t off : s.trace.insns) {
        bool ok = false;
        uint32_t c = cell_of(proj, m.w, m.h, off, &ok);
        if (ok)
            cell_steps[c].push_back(step);
        step++;
    }
    m.nsteps = s.trace.insns.size();

    m.code.reserve(cell_steps.size());
    for (auto &kv : cell_steps) {
        TerrainModel::CodeCell cc;
        cc.cell = kv.first;
        cc.steps = std::move(kv.second);
        cc.full_heat = full_heat_by_cell[cc.cell]; // canvas-sourced (04-T3)
        // Resolve the region under this cell to inherit its churn timing.
        uint32_t x = cc.cell % m.w, y = cc.cell / m.w;
        float u = (x + 0.5f) / m.w, v = (y + 0.5f) / m.h;
        const Region *r = nullptr;
        uint64_t a = 0;
        if (proj.unproject(u, v, &a, &r) && r) {
            auto f = churn_step.find(r->base);
            if (f != churn_step.end())
                cc.churn_step = f->second;
        }
        m.code.push_back(std::move(cc));
    }

    // --- rich rung: per-cell ordered data accesses (gated on `mem`) -----------
    if (m.mem_present) {
        std::map<uint32_t,
                 std::vector<std::tuple<uint64_t, uint64_t, uint32_t>>>
            acc; // cell -> (step, size, rw-bit)
        for (const Event &e : rec.by_kind.at("mem")) {
            uint64_t st = e.body.value("step", uint64_t{0});
            uint64_t ea = e.body.value("ea", uint64_t{0});
            uint64_t sz = e.body.value("size", uint64_t{0});
            std::string rw = e.body.value("rw", std::string("r"));
            bool ok = false;
            uint32_t c = cell_of(proj, m.w, m.h, ea, &ok);
            if (!ok)
                continue;
            uint32_t rwbit = (rw == "w") ? TF_WRITE : TF_READ;
            acc[c].push_back({st, sz, rwbit});
        }
        m.data.reserve(acc.size());
        for (auto &kv : acc) {
            auto &v = kv.second;
            std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) {
                return std::get<0>(a) < std::get<0>(b);
            });
            TerrainModel::DataCell dc;
            dc.cell = kv.first;
            uint64_t cum = 0;
            uint32_t rwm = 0;
            for (const auto &t : v) {
                dc.steps.push_back(std::get<0>(t));
                cum += std::get<1>(t);
                rwm |= std::get<2>(t);
                dc.cum_size.push_back(cum);
                dc.cum_rw.push_back(rwm);
            }
            m.data.push_back(std::move(dc));
        }
    }

    m.proj = std::move(proj);
    return m;
}

Terrain TerrainModel::slice(uint64_t t) const {
    Terrain out;
    out.w = w;
    out.h = h;
    const size_t n = static_cast<size_t>(w) * h;
    out.height.assign(n, 0.0f);
    out.flags.assign(n, 0u);
    if (!basis_error.empty())
        return out; // refused: a flat plane; the HUD carries why (basis_error)

    for (const CodeCell &cc : code) {
        // trace steps with index <= t (inclusive slice [0, t]).
        size_t hits = static_cast<size_t>(
            std::upper_bound(cc.steps.begin(), cc.steps.end(), t) -
            cc.steps.begin());
        if (hits == 0)
            continue;
        out.height[cc.cell] = std::log1p(static_cast<float>(hits));
        if (cc.churn_step != UINT64_MAX && t >= cc.churn_step)
            out.flags[cc.cell] |= TF_CHURN;
        if (torn)
            out.flags[cc.cell] |= TF_TORN;
    }
    for (const DataCell &dc : data) {
        size_t idx = static_cast<size_t>(
            std::upper_bound(dc.steps.begin(), dc.steps.end(), t) -
            dc.steps.begin());
        if (idx == 0)
            continue;
        out.height[dc.cell] =
            std::log1p(static_cast<float>(dc.cum_size[idx - 1]));
        out.flags[dc.cell] |= dc.cum_rw[idx - 1];
        if (torn)
            out.flags[dc.cell] |= TF_TORN;
    }
    return out;
}

} // namespace space
} // namespace asmdesk
