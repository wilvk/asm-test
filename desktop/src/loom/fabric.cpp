// fabric.cpp — the pure fabric builder of fabric.h. No ImGui, no I/O, no
// engine calls (only the pure structs of asmtest_valtrace.h).
#include "loom/fabric.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace asmdesk {

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// Memory records may carry size 0 when the producer's hook could not report a
// width (src/dataflow_emu.c clamps a negative hook size to 0). One byte is the
// smallest thing a memory access can be, so the band still exists — the record
// is not silently dropped, and its span's [lo,hi) says exactly one byte, which
// is a floor, not a claim about the real width.
uint64_t rec_width(const at_val_rec_t &r) { return r.size ? r.size : 1; }

// Bands coalesce across gaps of at most this many bytes. A stack frame's slots
// are adjacent-ish, not contiguous; one lane per 8-byte slot would make the
// fabric unreadable, and one lane for the whole address space would make it
// meaningless.
constexpr uint64_t kBandGap = 64;

struct MemRange {
    uint64_t lo, hi;
    uint32_t first_step;
};

} // namespace

// ---------------------------------------------------------------------------
// Register identity
// ---------------------------------------------------------------------------

// The Capstone x86 register ids this build folds. Kept as literals rather than
// as X86_REG_* so that including capstone/x86.h — and therefore linking
// Capstone — is not needed to name a lane (D4). The values are pinned by
// test_loom_fabric.cpp against the ids the golden corpus actually carries.
namespace {
struct RegFold {
    uint32_t id, container;
};
// Sub-register -> 64-bit container, mirroring src/dataflow_emu.c:64
// (cap_x86_to_uc), which is what the producer folded when it recorded.
const RegFold kFolds[] = {
    {2, 35},    {1, 35},    {3, 35},    {19, 35}, // al ah ax eax -> rax
    {5, 37},    {4, 37},    {8, 37},    {21, 37}, // bl bh bx ebx -> rbx
    {10, 38},   {9, 38},    {12, 38},   {22, 38}, // cl ch cx ecx -> rcx
    {16, 40},   {13, 40},   {18, 40},   {24, 40}, // dl dh dx edx -> rdx
    {46, 43},   {45, 43},   {29, 43},             // sil si esi   -> rsi
    {15, 39},   {14, 39},   {23, 39},             // dil di edi   -> rdi
    {7, 36},    {6, 36},    {20, 36},             // bpl bp ebp   -> rbp
    {48, 44},   {47, 44},   {30, 44},             // spl sp esp   -> rsp
    {218, 106}, {226, 106}, {234, 106},           // r8b  r8d  r8w
    {219, 107}, {227, 107}, {235, 107},           // r9*
    {220, 108}, {228, 108}, {236, 108},           // r10*
    {221, 109}, {229, 109}, {237, 109},           // r11*
    {222, 110}, {230, 110}, {238, 110},           // r12*
    {223, 111}, {231, 111}, {239, 111},           // r13*
    {224, 112}, {232, 112}, {240, 112},           // r14*
    {225, 113}, {233, 113}, {241, 113},           // r15*
};
struct RegName {
    uint32_t id;
    const char *name;
};
const RegName kNames[] = {
    {35, "rax"},  {37, "rbx"},  {38, "rcx"},    {40, "rdx"},  {43, "rsi"},
    {39, "rdi"},  {36, "rbp"},  {44, "rsp"},    {106, "r8"},  {107, "r9"},
    {108, "r10"}, {109, "r11"}, {110, "r12"},   {111, "r13"}, {112, "r14"},
    {113, "r15"}, {41, "rip"},  {25, "eflags"},
};
} // namespace

uint32_t loom_fold_reg(uint32_t cap_reg) {
    for (const RegFold &f : kFolds)
        if (f.id == cap_reg)
            return f.container;
    return cap_reg; // already a container, or one this build does not model
}

std::string loom_reg_name(uint32_t container) {
    for (const RegName &n : kNames)
        if (n.id == container)
            return n.name;
    return "reg#" + std::to_string(container);
}

const std::vector<uint32_t> &loom_reg_deck() {
    // rdi rsi rdx rcx r8 r9        — arg_regs (src/dataflow_emu.c:273)
    // rax rbx rbp r10..r15         — the rest of df_zero_gp, in its order (:229)
    // rsp rip eflags               — the rest of cap_x86_to_uc (:109)
    static const std::vector<uint32_t> deck = {
        39,  43,  40,  38,  106, 107, 35, 37, 36,
        108, 109, 110, 111, 112, 113, 44, 41, 25,
    };
    return deck;
}

// ---------------------------------------------------------------------------
// Fabric queries
// ---------------------------------------------------------------------------

bool loom_fabric_t::is_knot(uint32_t step) const {
    for (const loom_knot_t &k : knots)
        if (k.step == step)
            return true;
    return false;
}

bool loom_fabric_t::code_range(uint64_t *lo, uint64_t *hi) const {
    bool any = false;
    uint64_t a = 0, b = 0;
    for (uint64_t off : insn_off) {
        if (off == UINT64_MAX)
            continue;
        if (!any) {
            a = b = off;
            any = true;
        } else {
            a = std::min(a, off);
            b = std::max(b, off);
        }
    }
    if (!any)
        return false;
    if (lo != nullptr)
        *lo = a;
    if (hi != nullptr)
        *hi = b;
    return true;
}

uint32_t loom_fabric_t::resident(uint32_t lane, uint32_t t) const {
    uint32_t best = kLoomNoSpan;
    for (size_t i = 0; i < spans.size(); i++) {
        const loom_span_t &s = spans[i];
        if (s.lane != lane || s.t_write > t)
            continue;
        if (s.t_end != kLoomAlive && t >= s.t_end)
            continue;
        // spans are sorted by (lane, t_write), so the last match wins
        best = static_cast<uint32_t>(i);
    }
    return best;
}

// ---------------------------------------------------------------------------
// The build
// ---------------------------------------------------------------------------

bool loom_fabric_build(const asmtest_valtrace_t *vt, const asmtest_defuse_t *g,
                       const loom_provenance_t &prov, loom_fabric_t *out,
                       std::string *err) {
    auto refuse = [&](const std::string &why) {
        if (err != nullptr)
            *err = why;
        return false;
    };
    if (out == nullptr)
        return refuse("loom_fabric_build: no output fabric");
    if (vt == nullptr)
        return refuse("loom_fabric_build: no value trace");
    if (!prov.exact)
        return refuse(
            "refusing to weave \"" +
            (prov.producer.empty() ? std::string("(unnamed)") : prov.producer) +
            "\": it is a statistical producer, and a statistical feed can "
            "never appear as fabric — a sample cannot say what a value was at "
            "a step. It joins the lane annex as corroboration instead.");

    loom_fabric_t f;
    f.prov = prov;
    f.steps = static_cast<uint32_t>(vt->steps_len);

    const at_val_rec_t *recs = vt->recs;
    const size_t nrecs = vt->recs == nullptr ? 0 : vt->recs_len;
    for (size_t i = 0; i < nrecs; i++)
        if (recs[i].step + 1 > f.steps)
            f.steps = recs[i].step + 1;

    // A step past what insn_off recorded keeps UINT64_MAX: unknown, not zero.
    f.insn_off.assign(f.steps, UINT64_MAX);
    for (uint32_t s = 0; s < f.steps && s < vt->steps_len; s++)
        f.insn_off[s] = vt->insn_off == nullptr ? UINT64_MAX : vt->insn_off[s];

    const char *space = vt->mem_space == AT_LOC_MEM_OFF ? "off" : "abs";

    // --- lanes: registers in deck order, then memory bands by first touch ----
    std::map<uint32_t, uint32_t> reg_first; // container -> first-touch step
    std::vector<MemRange> ranges;
    for (size_t i = 0; i < nrecs; i++) {
        const at_val_rec_t &r = recs[i];
        if (r.kind == AT_LOC_REG) {
            uint32_t c = loom_fold_reg(r.reg);
            auto it = reg_first.find(c);
            if (it == reg_first.end())
                reg_first.emplace(c, r.step);
            else
                it->second = std::min(it->second, r.step);
        } else {
            ranges.push_back({r.addr, r.addr + rec_width(r), r.step});
        }
    }

    for (uint32_t c : loom_reg_deck()) {
        auto it = reg_first.find(c);
        if (it == reg_first.end())
            continue;
        loom_lane_t l;
        l.kind = loom_lane_kind::reg;
        l.reg = c;
        l.name = loom_reg_name(c);
        l.first_touch_step = it->second;
        f.lanes.push_back(l);
    }
    // Registers the deck does not name (an unmodelled id folds to itself) still
    // get a lane — dropping one would silently hide a location the producer
    // measured. They follow the deck, ordered by first touch then id.
    {
        std::vector<std::pair<uint32_t, uint32_t>> extra; // (first_step, id)
        for (const auto &kv : reg_first) {
            const std::vector<uint32_t> &deck = loom_reg_deck();
            if (std::find(deck.begin(), deck.end(), kv.first) == deck.end())
                extra.emplace_back(kv.second, kv.first);
        }
        std::sort(extra.begin(), extra.end());
        for (const auto &e : extra) {
            loom_lane_t l;
            l.kind = loom_lane_kind::reg;
            l.reg = e.second;
            l.name = loom_reg_name(e.second);
            l.first_touch_step = e.first;
            f.lanes.push_back(l);
        }
    }

    std::vector<MemRange> bands;
    if (!ranges.empty()) {
        std::sort(ranges.begin(), ranges.end(),
                  [](const MemRange &a, const MemRange &b) {
                      return a.lo != b.lo ? a.lo < b.lo : a.hi < b.hi;
                  });
        MemRange cur = ranges[0];
        for (size_t i = 1; i < ranges.size(); i++) {
            const MemRange &r = ranges[i];
            if (r.lo <= cur.hi + kBandGap) {
                cur.hi = std::max(cur.hi, r.hi);
                cur.first_step = std::min(cur.first_step, r.first_step);
            } else {
                bands.push_back(cur);
                cur = r;
            }
        }
        bands.push_back(cur);
        std::sort(bands.begin(), bands.end(),
                  [](const MemRange &a, const MemRange &b) {
                      return a.first_step != b.first_step
                                 ? a.first_step < b.first_step
                                 : a.lo < b.lo;
                  });
    }
    for (const MemRange &b : bands) {
        loom_lane_t l;
        l.kind = loom_lane_kind::mem_band;
        l.lo = b.lo;
        l.hi = b.hi;
        l.name = "mem[" + hex(b.lo) + ".." + hex(b.hi) + ")";
        l.space = space;
        l.first_touch_step = b.first_step;
        f.lanes.push_back(l);
    }

    auto lane_of = [&](const at_val_rec_t &r) -> uint32_t {
        if (r.kind == AT_LOC_REG) {
            uint32_t c = loom_fold_reg(r.reg);
            for (size_t i = 0; i < f.lanes.size(); i++)
                if (f.lanes[i].kind == loom_lane_kind::reg &&
                    f.lanes[i].reg == c)
                    return static_cast<uint32_t>(i);
            return kLoomNoSpan;
        }
        uint64_t lo = r.addr, hi = r.addr + rec_width(r);
        for (size_t i = 0; i < f.lanes.size(); i++) {
            const loom_lane_t &l = f.lanes[i];
            if (l.kind == loom_lane_kind::mem_band && lo < l.hi && hi > l.lo)
                return static_cast<uint32_t>(i);
        }
        return kLoomNoSpan;
    };

    // --- spans: one per write record, plus the synthetic entry spans --------
    for (size_t i = 0; i < nrecs; i++) {
        const at_val_rec_t &r = recs[i];
        if (!r.is_write)
            continue;
        uint32_t lane = lane_of(r);
        if (lane == kLoomNoSpan)
            continue;
        loom_span_t s;
        s.lane = lane;
        s.t_write = r.step;
        s.rec = i;
        s.value_valid = r.value_valid;
        s.value = r.value;
        if (r.kind != AT_LOC_REG) {
            s.lo = r.addr;
            s.hi = r.addr + rec_width(r);
        }
        f.spans.push_back(s);
    }

    // Born of untraced state: a lane whose FIRST record is a read held a value
    // the instrumentation never saw written. That is a real worldline and it is
    // drawn — with a glyph saying provenance starts at instrumentation, never as
    // if the fabric knew where it came from.
    for (uint32_t lane = 0; lane < f.lanes.size(); lane++) {
        const at_val_rec_t *first = nullptr;
        for (size_t i = 0; i < nrecs; i++) {
            if (lane_of(recs[i]) != lane)
                continue;
            first = &recs[i];
            break;
        }
        if (first == nullptr || first->is_write)
            continue;
        uint32_t first_write = kLoomAlive;
        for (const loom_span_t &s : f.spans)
            if (s.lane == lane)
                first_write = std::min(first_write, s.t_write);
        loom_span_t s;
        s.lane = lane;
        s.t_write = 0;
        s.t_end = first_write;
        s.rec = kLoomNoRec;
        s.born_untraced = true;
        s.value_valid = first->value_valid;
        s.value = first->value;
        s.lo = f.lanes[lane].lo;
        s.hi = f.lanes[lane].hi;
        f.spans.push_back(s);
    }

    std::stable_sort(f.spans.begin(), f.spans.end(),
                     [](const loom_span_t &a, const loom_span_t &b) {
                         if (a.lane != b.lane)
                             return a.lane < b.lane;
                         if (a.t_write != b.t_write)
                             return a.t_write < b.t_write;
                         return a.rec < b.rec;
                     });

    // t_end: the next write on the same lane whose bytes intersect. Register
    // lanes always intersect (the fold made them one location); memory spans
    // only end where they are actually overwritten.
    for (size_t i = 0; i < f.spans.size(); i++) {
        loom_span_t &s = f.spans[i];
        if (s.born_untraced)
            continue; // already bounded by the lane's first write
        uint32_t end = kLoomAlive;
        for (const loom_span_t &o : f.spans) {
            if (o.lane != s.lane || o.born_untraced || o.t_write <= s.t_write)
                continue;
            bool overlaps = f.lanes[s.lane].kind == loom_lane_kind::reg ||
                            (o.lo < s.hi && o.hi > s.lo);
            if (overlaps)
                end = std::min(end, o.t_write);
        }
        s.t_end = end;
    }

    // --- hops ---------------------------------------------------------------
    const size_t nedges = (g == nullptr || g->edges == nullptr) ? 0 : g->n;
    for (size_t i = 0; i < nedges; i++) {
        const asmtest_defuse_edge_t &e = g->edges[i];
        uint32_t lane = lane_of(e.loc);
        if (lane == kLoomNoSpan)
            continue; // the carried location is not on this fabric: never guess
        uint64_t lo = e.loc.addr, hi = e.loc.addr + rec_width(e.loc);
        uint32_t from = kLoomNoSpan;
        for (size_t j = 0; j < f.spans.size(); j++) {
            const loom_span_t &s = f.spans[j];
            if (s.lane != lane || s.t_write != e.from_step || s.born_untraced)
                continue;
            if (f.lanes[lane].kind == loom_lane_kind::mem_band &&
                !(lo < s.hi && hi > s.lo))
                continue;
            from = static_cast<uint32_t>(j); // last write at that step wins
        }
        if (from == kLoomNoSpan)
            continue; // unresolvable producer: skip the hop rather than guess
        // The consumer's own span, preferring the same lane (a read-modify-write
        // stays on its worldline); otherwise the first span the step defines.
        uint32_t to = kLoomNoSpan;
        for (size_t j = 0; j < f.spans.size(); j++) {
            const loom_span_t &s = f.spans[j];
            if (s.t_write != e.to_step || s.born_untraced)
                continue;
            if (to == kLoomNoSpan)
                to = static_cast<uint32_t>(j);
            if (s.lane == lane) {
                to = static_cast<uint32_t>(j);
                break;
            }
        }
        loom_hop_t h;
        h.from_span = from;
        h.to_span = to;
        h.edge = static_cast<uint32_t>(i);
        f.hops.push_back(h);
    }

    // --- reads --------------------------------------------------------------
    for (size_t i = 0; i < nrecs; i++) {
        const at_val_rec_t &r = recs[i];
        if (r.is_write)
            continue;
        uint32_t lane = lane_of(r);
        if (lane == kLoomNoSpan)
            continue;
        loom_read_t rd;
        rd.step = r.step;
        rd.lane = lane;
        for (size_t j = 0; j < nedges; j++) {
            const asmtest_defuse_edge_t &e = g->edges[j];
            if (e.to_step == r.step && lane_of(e.loc) == lane) {
                rd.has_producer = true;
                break;
            }
        }
        f.reads.push_back(rd);
    }

    // --- knots --------------------------------------------------------------
    {
        std::map<uint32_t, std::pair<bool, bool>> rw; // step -> (read, write)
        for (size_t i = 0; i < nrecs; i++) {
            auto &e = rw[recs[i].step];
            (recs[i].is_write ? e.second : e.first) = true;
        }
        for (const auto &kv : rw)
            if (kv.second.first && kv.second.second)
                f.knots.push_back({kv.first});
    }

    *out = std::move(f);
    return true;
}

// ---------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------

std::string loom_fabric_dump(const loom_fabric_t &f) {
    std::string o;
    o += "fabric producer=" + f.prov.producer +
         " exact=" + (f.prov.exact ? "1" : "0") +
         " truncated=" + (f.prov.truncated ? "1" : "0") +
         " isolated_guest=" + (f.prov.isolated_guest ? "1" : "0") +
         " steps=" + std::to_string(f.steps) + "\n";
    for (size_t i = 0; i < f.lanes.size(); i++) {
        const loom_lane_t &l = f.lanes[i];
        o += "lane " + std::to_string(i) + " " + l.name;
        if (l.kind == loom_lane_kind::mem_band)
            o += " space=" + l.space;
        o += " first_touch=" + std::to_string(l.first_touch_step) + "\n";
    }
    for (size_t i = 0; i < f.spans.size(); i++) {
        const loom_span_t &s = f.spans[i];
        o += "span " + std::to_string(i) + " lane=" + std::to_string(s.lane) +
             " t=[" + std::to_string(s.t_write) + "," +
             (s.t_end == kLoomAlive ? std::string("alive")
                                    : std::to_string(s.t_end)) +
             ")";
        o += s.value_valid ? " value=" + hex(s.value) : std::string(" value=?");
        if (s.born_untraced)
            o += " born_untraced";
        if (!s.value_valid)
            o += " hollow";
        o += "\n";
    }
    for (const loom_hop_t &h : f.hops)
        o += "hop span" + std::to_string(h.from_span) + " -> " +
             (h.to_span == kLoomNoSpan ? std::string("(reads only)")
                                       : "span" + std::to_string(h.to_span)) +
             " edge=" + std::to_string(h.edge) + "\n";
    for (const loom_knot_t &k : f.knots)
        o += "knot step=" + std::to_string(k.step) + "\n";
    return o;
}

} // namespace asmdesk
