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

    // T1 (44-faithful-city-phase-a): kind_by_cell — one O(cells) sweep, BEFORE
    // any early return, so it is populated on every path (a basis-refused or
    // anchor-refused terrain still has a real plane to zone). Reuses the
    // SAME per-cell-to-region resolution the churn join below already uses
    // (Projection::unproject over the cell centre) rather than a second one.
    // An off-domain cell (unproject fails) gets kKindByCellNone.
    m.kind_by_cell.assign(static_cast<size_t>(m.w) * m.h, kKindByCellNone);
    for (uint32_t y = 0; y < m.h; ++y) {
        for (uint32_t x = 0; x < m.w; ++x) {
            const float u = (x + 0.5f) / m.w, v = (y + 0.5f) / m.h;
            uint64_t a = 0;
            const Region *r = nullptr;
            if (proj.unproject(u, v, &a, &r) && r)
                m.kind_by_cell[static_cast<size_t>(y) * m.w + x] =
                    static_cast<uint8_t>(r->kind);
        }
    }

    Streams s = decode_streams(rec);
    // REUSE 04-T3: the trace canvas owns the per-offset execution count and the
    // basis handling; the coarse height is that count, not a re-derivation.
    dt_canvas canvas = dt_canvas_build(s);

    // Truncation is a property of the whole recording: a footer that declares it,
    // a torn tail with no footer, or dropped events — all mean the heights are a
    // floor, so the touched cells are flagged TORN at slice time.
    m.torn = rec.truncated() || rec.dropped();
    m.basis = canvas.basis;

    // The rich rung's `mem` stream is gated at runtime on the kind being present.
    // Its producer LANDED (29 R2: live `--dataflow --mem` / serve `mem:true`, and
    // the emulator projection), so a live capture with `--mem` DOES carry it and
    // lights the rich rung; a capture without it, and every `trace`/region
    // recording, carries none — the data cells stay flat and the note says so,
    // never a silent zero.
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

    // 36 T3: anchor a rel trace's offsets onto the absolute plane — the SAME
    // derivation the trajectory uses (resolve_anchor over the very region vector
    // the projection was built from). For an "abs" basis the placement is the
    // identity, so every existing fixture and both scene goldens are unchanged.
    // A rel trace with no resolvable span places NO cells (an unanchored offset
    // has no true plane cell) but its time axis is still real (nsteps below); set
    // anchor_error — never basis_error, which is reserved for MIXED bases.
    Anchor anchor = resolve_anchor(proj.regions);
    const bool rel = (m.basis == "rel");
    if (rel && !anchor.ok)
        m.anchor_error = anchor.reason;
    auto place_off = [&](uint64_t off, bool *ok) -> uint64_t {
        if (!rel) {
            *ok = true;
            return off; // abs: the offset IS the absolute address
        }
        uint64_t abs = 0;
        *ok = anchor.ok && anchor.place(off, &abs);
        return abs;
    };

    // The coarse height source, straight from the canvas (NOT recomputed here):
    // a cell's full heat is the SUM of the canvas per-offset execution counts of
    // the offsets that project into it. The ordered insn stream below is the same
    // multiset the canvas counted; it supplies only the STEP ORDER the canvas
    // discards, so slice(t) can time-slice — and slice(full) reproduces exactly
    // this canvas-sourced heat (pinned in test_terrain.cpp).
    std::map<uint32_t, uint32_t> full_heat_by_cell;
    for (const dt_canvas_row &r : canvas.rows) {
        bool aok = false;
        uint64_t addr = place_off(r.off, &aok); // 36 T3: anchor a rel offset
        if (!aok)
            continue;
        bool ok = false;
        uint32_t c = cell_of(proj, m.w, m.h, addr, &ok);
        if (ok)
            full_heat_by_cell[c] += r.heat;
    }

    // --- churn timing --------------------------------------------------------
    // Walk trace + codeimage in stream (seq) order, counting the trace steps that
    // precede each version increase. churn_step[base] is the count before the
    // FIRST bump of that base, so a slice t (inclusive) shows CHURN over the base
    // once t >= churn_step[base].
    //
    // 37 T3: redeemed for a df recording. A dataflow recording carries no `trace`,
    // so counting only trace offsets pinned every detected churn at step 0 (the
    // pre-existing bug 36 T3 flagged) — now its df_step offsets are counted as
    // steps too. It still retains only the FIRST bump per base; the real limit
    // that version resets to 0 mid-recording on a candidate walk (so a re-armed
    // span's baseline never registers as churn under the greater-than rule) is
    // benign while candidates have distinct bases and is stated, not papered over.
    struct SE {
        uint64_t seq;
        const Event *ev;
        bool ci;
    };
    std::vector<SE> ord;
    if (auto it = rec.by_kind.find("trace"); it != rec.by_kind.end())
        for (const Event &e : it->second)
            ord.push_back({e.seq, &e, false});
    // A df recording has no `trace`; count its df_step offsets as steps so the
    // churn walk advances (a recording carrying `trace` uses that and ignores
    // df_step, so the counter stays in one step space).
    if (s.trace.insns.empty())
        if (auto it = rec.by_kind.find("df_step"); it != rec.by_kind.end())
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
        bool aok = false;
        uint64_t addr = place_off(off, &aok); // 36 T3: anchor a rel offset
        if (aok) {
            bool ok = false;
            uint32_t c = cell_of(proj, m.w, m.h, addr, &ok);
            if (ok)
                cell_steps[c].push_back(step);
        }
        step++; // the STEP is real even when its offset places no cell
    }
    m.nsteps = s.trace.insns.size();
    if (!cell_steps.empty())
        m.height_source = "trace";

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

    // --- 36 T3: the df_step height rung --------------------------------------
    // When the trace canvas placed NOTHING (a live serve dataflow/auto session,
    // or a --dataflow file: df_step only, no `trace`), drive the coarse rung from
    // the single-step residency stream instead. df_step.off is ROUTINE-RELATIVE
    // by construction, so it anchors exactly like a rel trace (base+off). This is
    // single-step residency, NOT block coverage: it fills no `blocks`, marks
    // nothing covered, and flags nothing TF_STAT — the stream is exact, not
    // sampled. Per-cell full_heat is the step count, not a canvas heat.
    if (m.code.empty() && s.df.present()) {
        // 37 T3: place each step against its OWN region base (rbase) when the wire
        // states it — so a MULTI-span df recording (an `auto` candidate walk) gets
        // relief, not just the single-span case 36 could anchor. An untagged step
        // falls back to 36's single-codeimage anchor.
        std::map<uint32_t, std::vector<uint64_t>> df_cell_steps;
        std::map<uint32_t, uint64_t>
            df_cell_base; // cell -> region base (churn)
        for (uint32_t st = 0; st < s.df.nsteps; ++st) {
            if (!s.df.has_step(st))
                continue; // an uncovered step index has an UNKNOWN offset, not 0
            const uint64_t rbase =
                st < s.df.insn_rbase.size() ? s.df.insn_rbase[st] : 0;
            uint64_t abs = 0, base_i = 0;
            if (rbase) {
                abs = rbase + s.df.insn_off[st]; // 37: the wire STATES the base
                base_i = rbase;
            } else if (anchor.ok && anchor.place(s.df.insn_off[st], &abs)) {
                base_i = anchor.base; // 36's single-span derivation
            } else {
                continue; // no base to place this step against
            }
            bool ok = false;
            uint32_t c = cell_of(proj, m.w, m.h, abs, &ok);
            if (ok) {
                df_cell_steps[c].push_back(st);
                df_cell_base[c] = base_i;
            }
        }
        m.nsteps = s.df.nsteps; // the time axis is real regardless of placement
        if (!df_cell_steps.empty()) {
            // Cells placed: label the rung — and ONLY now, mirroring the trace
            // rung's `if (!cell_steps.empty())` guard, so a capture that placed
            // NOTHING never advertises df residency over a flat plane. This is
            // single-step residency, NOT block coverage: it fills no `blocks`,
            // marks nothing covered, and flags nothing TF_STAT.
            m.height_source = "df_step";
            m.height_note =
                "coarse: heights from single-step residency (df_step) "
                "— no block coverage";
            m.code.reserve(df_cell_steps.size());
            for (auto &kv : df_cell_steps) {
                TerrainModel::CodeCell cc;
                cc.cell = kv.first;
                cc.steps = std::move(kv.second);
                cc.full_heat =
                    static_cast<uint32_t>(cc.steps.size()); // step count
                // Key the churn join on the step's OWN region base (rbase), NOT
                // the geometric unproject the trace rung uses — sound even for an
                // offset projecting into no region, and correct across the
                // several spans a candidate walk carries. The churn walk above
                // counts df_step offsets, so this lands at the true step, not 0.
                auto f = churn_step.find(df_cell_base[kv.first]);
                if (f != churn_step.end())
                    cc.churn_step = f->second;
                m.code.push_back(std::move(cc));
            }
        } else if (m.anchor_error.empty()) {
            // Steps ran but NOTHING placed — never a silent flat plane over real
            // steps (the steps_explained bar). Say why: no resolvable span (no
            // wire base AND no single codeimage), or every offset fell outside
            // its span / named a base with no matching codeimage (the clamp).
            m.anchor_error =
                (anchor.ok || s.df.rbase_present)
                    ? "every df_step offset fell outside its codeimage span "
                      "(the "
                      "4096-byte clamp), or named a base with no matching "
                      "codeimage — no residency cell could be placed"
                    : anchor.reason;
        }
    }

    // --- rich rung: per-cell ordered data accesses (gated on `mem`) -----------
    if (m.mem_present) {
        // Direction for the T2 split sums: exactly "r" or "w" count into their
        // own running total; anything else (an absent or unrecognised `rw`
        // token) counts into cum_size only — the "unknown is not zero"
        // invariant, kept separate from rwbit below, which is the PRE-EXISTING
        // (and unchanged) TF_READ/TF_WRITE approximation that folds an unknown
        // token into READ.
        enum Dir : uint8_t { kDirNone = 0, kDirRead = 1, kDirWrite = 2 };
        std::map<uint32_t,
                 std::vector<std::tuple<uint64_t, uint64_t, uint32_t, uint8_t>>>
            acc; // cell -> (step, size, rw-bit, direction)
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
            uint8_t dir = (rw == "w")   ? kDirWrite
                          : (rw == "r") ? kDirRead
                                        : kDirNone;
            acc[c].push_back({st, sz, rwbit, dir});
        }
        m.data.reserve(acc.size());
        for (auto &kv : acc) {
            auto &v = kv.second;
            std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) {
                return std::get<0>(a) < std::get<0>(b);
            });
            TerrainModel::DataCell dc;
            dc.cell = kv.first;
            uint64_t cum = 0, cum_r = 0, cum_w = 0;
            uint32_t rwm = 0;
            for (const auto &t : v) {
                dc.steps.push_back(std::get<0>(t));
                cum += std::get<1>(t);
                rwm |= std::get<2>(t);
                switch (std::get<3>(t)) {
                case kDirRead:
                    cum_r += std::get<1>(t);
                    break;
                case kDirWrite:
                    cum_w += std::get<1>(t);
                    break;
                default:
                    break; // neither: counted in cum_size only
                }
                dc.cum_size.push_back(cum);
                dc.cum_rw.push_back(rwm);
                dc.cum_read_size.push_back(cum_r);
                dc.cum_write_size.push_back(cum_w);
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

    // T2 (44-faithful-city-phase-a): fog-of-war. An IN-DOMAIN cell (kind_by_cell
    // != kKindByCellNone — the same in-domain test T1 already builds, reused
    // here rather than a second unproject sweep) that carries no content at
    // THIS slice — no code/data hit yet, and no TF_STAT (the exact slice never
    // carries TF_STAT; the statistical layer is the SEPARATE `stat` Terrain,
    // per the T6 isolation invariant, so this check is a defensive mirror of
    // the doc's stated rule rather than a reachable branch today) — reads as
    // "no content", never a described low cell and never off-domain void. This
    // is inherently PER-SLICE: a cell first touched at step 50 is fog before
    // it, not after, which is the whole point of a fog-of-war frontier.
    for (size_t i = 0; i < n; ++i) {
        if (i < kind_by_cell.size() && kind_by_cell[i] != kKindByCellNone &&
            out.height[i] <= 0.0f && (out.flags[i] & TF_STAT) == 0u)
            out.flags[i] |= TF_UNKNOWN;
    }
    return out;
}

uint64_t TerrainModel::max_full_heat(uint64_t t) const {
    if (!basis_error.empty())
        return 0; // refused: slice() itself would return a flat plane too
    uint64_t maxh = 0;
    for (const CodeCell &cc : code) {
        size_t hits = static_cast<size_t>(
            std::upper_bound(cc.steps.begin(), cc.steps.end(), t) -
            cc.steps.begin());
        if (hits > 0)
            maxh = std::max(maxh, static_cast<uint64_t>(hits));
    }
    for (const DataCell &dc : data) {
        size_t idx = static_cast<size_t>(
            std::upper_bound(dc.steps.begin(), dc.steps.end(), t) -
            dc.steps.begin());
        if (idx > 0)
            maxh = std::max(maxh, dc.cum_size[idx - 1]);
    }
    return maxh;
}

Terrain TerrainModel::coarse_slice() const {
    // A flat plane the size of the projection, no per-cell work: the degrade
    // target for an over-budget scrub (T4). Carries the torn flag globally so the
    // degraded frame is labelled, never a silent measured zero (D7).
    Terrain out;
    out.w = w;
    out.h = h;
    const size_t n = static_cast<size_t>(w) * h;
    out.height.assign(n, 0.0f);
    out.flags.assign(n, torn ? TF_TORN : 0u);
    return out;
}

} // namespace space
} // namespace asmdesk
