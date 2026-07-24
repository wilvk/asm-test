// feed.cpp — replay feeder of feed.h. Pure: no ImGui, no I/O, no engine calls.
#include "loom/feed.h"

namespace asmdesk {

namespace {

at_loc_kind_t space_of(const std::string &s) {
    if (s == "reg")
        return AT_LOC_REG;
    if (s == "off")
        return AT_LOC_MEM_OFF;
    return AT_LOC_MEM_ABS;
}

at_val_rec_t to_rec(const ValRec &r) {
    at_val_rec_t o{};
    o.kind = space_of(r.space);
    o.reg = r.reg;
    o.addr = r.addr;
    o.size = static_cast<uint16_t>(r.size);
    o.is_write = r.write;
    // A value the producer never captured stays uncaptured, and a >8-byte value
    // stays `wide` with no bytes behind it: the v1 schema does not serialise the
    // wide side buffer (a documented limit). Inventing either would put a number
    // on a worldline that no producer ever measured.
    o.value_valid = r.value_valid;
    o.wide = r.wide;
    o.value = r.value;
    o.step = r.step;
    return o;
}

} // namespace

const asmtest_valtrace_t *loom_feed_t::vt() const {
    vt_ = asmtest_valtrace_t{};
    vt_.insn_off =
        const_cast<uint64_t *>(insn_off.empty() ? nullptr : insn_off.data());
    vt_.steps_cap = vt_.steps_len = insn_off.size();
    vt_.steps_total = prov.steps_total ? prov.steps_total : insn_off.size();
    vt_.recs = const_cast<at_val_rec_t *>(recs.empty() ? nullptr : recs.data());
    vt_.recs_cap = vt_.recs_len = recs.size();
    vt_.recs_total = recs.size();
    vt_.truncated = prov.truncated;
    vt_.mem_space = AT_LOC_MEM_ABS;
    for (const at_val_rec_t &r : recs)
        if (r.kind == AT_LOC_MEM_OFF) {
            vt_.mem_space = AT_LOC_MEM_OFF;
            break;
        }
    return &vt_;
}

const asmtest_defuse_t *loom_feed_t::g() const {
    g_ = asmtest_defuse_t{};
    g_.edges = const_cast<asmtest_defuse_edge_t *>(
        edges.empty() ? nullptr : edges.data());
    g_.n = edges.size();
    g_.nsteps = insn_off.size();
    return &g_;
}

loom_feed_t loom_feed_from_streams(const Streams &s) {
    loom_feed_t f;
    const DataflowStream &df = s.df;

    f.insn_off = df.insn_off;
    f.recs.reserve(df.recs.size());
    for (const ValRec &r : df.recs)
        f.recs.push_back(to_rec(r));
    f.edges.reserve(df.edges.size());
    for (size_t i = 0; i < df.edges.size(); i++) {
        asmtest_defuse_edge_t e{};
        e.from_step = df.edges[i].from_step;
        e.to_step = df.edges[i].to_step;
        if (i < df.edge_loc.size())
            e.loc = to_rec(df.edge_loc[i]);
        f.edges.push_back(e);
    }

    f.prov.producer = s.backend;
    f.prov.exact = !s.statistical;
    // Truncation is carried from the recording, never re-derived: a step index
    // no df_step event covered is a DROPPED step, which makes the fabric a lower
    // bound exactly as the footer's flag does.
    f.prov.truncated = s.truncated || df.steps_missing > 0;
    // "isolated guest" is a property of the producer, and the only v1 producer
    // that replays inside an emulator is the emulator L0 value producer.
    f.prov.isolated_guest = s.backend == "emu-l0";
    f.prov.disasm = df.disasm;
    f.prov.steps_recorded = df.nsteps - df.steps_missing;
    // v1 has no dataflow step total. `insns_total` is the trace stream's count
    // of executed instructions SEEN (it counts past the buffer cap), which for a
    // whole-run value trace is the same population — so it is used when a
    // coverage event supplied one, and left 0 (unknown) otherwise. The chrome
    // renders a different sentence for the unknown case; it never invents an M.
    if (s.trace.insns_total > f.prov.steps_recorded)
        f.prov.steps_total = s.trace.insns_total;
    return f;
}

bool loom_fabric_from_streams(const Streams &s, loom_feed_t *feed,
                              loom_fabric_t *out, std::string *err) {
    if (feed == nullptr || out == nullptr)
        return false;
    *feed = loom_feed_from_streams(s);
    if (!s.df.present()) {
        if (err != nullptr)
            *err = "this recording carries no df_step events, so there are no "
                   "per-step values to weave — the day-one fabric's only L0 "
                   "value producer is the x86-64 emulator producer";
        return false;
    }
    return loom_fabric_build(feed->vt(), feed->g(), feed->prov, out, err);
}

} // namespace asmdesk
