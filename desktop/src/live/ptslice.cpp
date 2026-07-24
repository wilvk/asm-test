// ptslice.cpp — see ptslice.h. Compiled in every tree; the producer calls are
// behind ASMTEST_DESKTOP_HAVE_PT_REPLAY, which only the full-app and
// engine-test trees define. Without it this TU still builds, still assembles
// inputs, and still explains — in the render-only viewer that is exactly what a
// user needs, and a viewer that silently offered a button it could not honour
// would be worse than one that says why.
#include "live/ptslice.h"

#include <cstdio>

#include "views/disasm.h"

#ifdef ASMTEST_DESKTOP_HAVE_PT_REPLAY
#include <cstddef>
#include <cstring>

// The L0 sink + the L1 def-use builder — the SHARED public surface (plain
// structs and pure functions), which is a different thing from the producer
// below: the sink is a published header, the producer is a tier.
extern "C" {
#include "asmtest_valtrace.h"
}

// The producer's re-declared surface — the data-flow tier's deliberate pattern
// (see ptslice.h). ONE place in the desktop tree, never scattered.
extern "C" {
#define DF_PT_OK     0
#define DF_PT_FAULT  1
#define DF_PT_EINVAL (-1)
#define DF_PT_ENOSYS (-3)

typedef struct {
    int pure;
    const char *reason;
    uint64_t steps;
    uint64_t path_len;
    uint64_t diverged_at;
    int vec_seeded;
} asmtest_dataflow_pt_info_t;

void asmtest_dataflow_pt_info_layout(size_t *size, size_t *last_off);
int asmtest_dataflow_pt_replay_path(const uint8_t *code, size_t code_len,
                                    const uint64_t *path, size_t path_len,
                                    const long *args, int nargs,
                                    asmtest_valtrace_t *vt,
                                    asmtest_dataflow_pt_info_t *info);
}
#endif

#ifdef ASMTEST_DESKTOP_CAN_PROBE
extern "C" {
#include "asmtest_hwtrace.h"
}
#endif

namespace asmdesk {

const char *ptslice_disclosure() {
    // Two grades of evidence in one view, and neither may be read as the other.
    return "PT slice: the PATH is hardware-recorded (Intel PT, zero "
           "single-steps of the target); the VALUES are RECONSTRUCTED by "
           "replaying that path through the emulator against the recorded code "
           "image — exact about what ran, a replay about what it held";
}

PtSliceGate ptslice_gate(const PtSliceFacts &f) {
    PtSliceGate g;
    if (!f.producer_linked) {
        // D4: the render-only viewer links no engine at all. Naming the BUILD
        // rather than the host is the difference between "install the full app"
        // and "buy a different CPU".
        g.reason = "this build does not link the PT replay producer — the "
                   "render-only viewer is engine-free by design (D4); the full "
                   "app replays a recorded PT path on any host";
        return g;
    }
    if (!f.layout_ok) {
        // A silent field skew would mis-read every telemetry number the
        // producer returns, so this refuses rather than reporting numbers it
        // cannot trust.
        g.reason = "refusing to run: the PT producer's info struct does not "
                   "match this build's re-declaration — " +
                   f.layout_detail;
        return g;
    }
    // Replay needs no PT hardware: the path is already decoded and recorded.
    g.can_replay = true;
    g.can_capture = f.capture_available;
    if (!g.can_capture)
        g.reason = f.capture_reason.empty()
                       ? std::string("live PT capture is unavailable on this "
                                     "host (no measured reason reported)")
                       : f.capture_reason;
    return g;
}

PtSliceFacts ptslice_facts() {
    PtSliceFacts f;
#ifdef ASMTEST_DESKTOP_HAVE_PT_REPLAY
    f.producer_linked = true;
    size_t size = 0, last_off = 0;
    asmtest_dataflow_pt_info_layout(&size, &last_off);
    // The F6 hazard defence: `size` alone misses a field landing in tail
    // padding, so the LAST field's offset is checked too.
    const size_t mine = sizeof(asmtest_dataflow_pt_info_t);
    const size_t mine_last = offsetof(asmtest_dataflow_pt_info_t, vec_seeded);
    f.layout_ok = (size == mine && last_off == mine_last);
    if (!f.layout_ok) {
        char buf[192];
        std::snprintf(buf, sizeof buf,
                      "producer says size=%zu last_off=%zu, this build has "
                      "size=%zu last_off=%zu",
                      size, last_off, mine, mine_last);
        f.layout_detail = buf;
    }
#endif
#ifdef ASMTEST_DESKTOP_CAN_PROBE
    asmtest_hwtrace_status_t st;
    std::memset(&st, 0, sizeof st);
    if (asmtest_hwtrace_status(ASMTEST_HWTRACE_INTEL_PT, &st) == 0) {
        f.capture_available = st.available != 0;
        f.capture_reason = st.reason;
    }
#endif
    return f;
}

PtSliceInput ptslice_input_from(const Recording &r, long tid) {
    PtSliceInput in;
    in.tid = tid;

    auto st = r.by_kind.find("stitch");
    if (st == r.by_kind.end() || st->second.empty()) {
        in.error = "this recording carries no `stitch` event — a PT slice "
                   "needs the decoded path, and nothing recorded one";
        return in;
    }
    const nlohmann::json *chosen = nullptr;
    for (const Event &e : st->second) {
        long t = e.body.value("tid", 0L);
        if (tid == 0 || t == tid) {
            chosen = &e.body;
            in.tid = t;
            break;
        }
    }
    if (chosen == nullptr) {
        in.error = "no `stitch` event for tid " + std::to_string(tid);
        return in;
    }
    if (!chosen->contains("slices") || !(*chosen)["slices"].is_array() ||
        (*chosen)["slices"].empty()) {
        in.error = "the `stitch` event for tid " + std::to_string(in.tid) +
                   " carries no slices";
        return in;
    }
    const nlohmann::json &slice = (*chosen)["slices"][0];
    uint64_t version = slice.value("version", uint64_t{0});
    if (slice.contains("offs") && slice["offs"].is_array())
        for (const nlohmann::json &o : slice["offs"])
            if (o.is_number())
                in.path.push_back(o.get<uint64_t>());
    if (in.path.empty()) {
        in.error = "the PT slice for tid " + std::to_string(in.tid) +
                   " decoded to no offsets — there is no path to replay";
        return in;
    }

    // The bytes the path was decoded AGAINST: the code-image version the slice
    // names. Not the newest — on a JIT that is a different method (see
    // views/disasm.h).
    DisasmView img = obs_disasm_build(r);
    const CodeVersion *v = nullptr;
    for (const CodeVersion &c : img.versions)
        if (c.version == version && (v == nullptr || c.base < v->base))
            v = &c;
    if (v == nullptr) {
        in.error =
            "no `codeimage` version " + std::to_string(version) +
            " in this recording — the path was decoded against bytes nobody "
            "recorded, and replaying it against any other version would "
            "reconstruct a different routine's values";
        if (!img.unavailable_reason.empty())
            in.error += " (the producer said: " + img.unavailable_reason + ")";
        return in;
    }
    in.base = v->base;
    in.code = v->bytes;
    return in;
}

PtSliceResult ptslice_run(const PtSliceInput &in) {
    PtSliceResult out;
    PtSliceGate gate = ptslice_gate(ptslice_facts());
    if (!gate.can_replay) {
        out.reason = gate.reason;
        return out;
    }
    if (!in.error.empty()) {
        out.reason = in.error;
        return out;
    }
    if (in.code.empty() || in.path.empty()) {
        out.reason = "nothing to replay: the input carries no code or no path";
        return out;
    }
#ifndef ASMTEST_DESKTOP_HAVE_PT_REPLAY
    out.reason = gate.reason;
    return out;
#else
    // Caps sized so the day-one corpus never truncates; the producer flips
    // `truncated` when they do, and this reports that rather than showing a
    // short trace as a complete one.
    asmtest_valtrace_t *vt = asmtest_valtrace_new(1u << 16, 1u << 18, 1u << 16);
    if (vt == nullptr) {
        out.reason = "out of memory allocating the value trace";
        return out;
    }
    asmtest_dataflow_pt_info_t info;
    std::memset(&info, 0, sizeof info);
    const long *args = in.args.empty() ? nullptr : in.args.data();
    int rc = asmtest_dataflow_pt_replay_path(
        in.code.data(), in.code.size(), in.path.data(), in.path.size(), args,
        static_cast<int>(in.args.size()), vt, &info);

    out.steps = info.steps;
    out.path_len = info.path_len;
    out.diverged_at = info.diverged_at;
    out.pure = info.pure != 0;
    if (info.reason != nullptr)
        out.reason = info.reason; // VERBATIM: the producer measured it
    if (rc == DF_PT_ENOSYS) {
        if (out.reason.empty())
            out.reason = "the PT replay producer is a stub in this build (it "
                         "needs Linux x86-64 + Capstone + Unicorn)";
        asmtest_valtrace_free(vt);
        return out;
    }
    if (rc == DF_PT_EINVAL) {
        if (out.reason.empty())
            out.reason = "the producer refused the input as invalid";
        asmtest_valtrace_free(vt);
        return out;
    }
    // DF_PT_FAULT is a PARTIAL replay, not a failure: the path diverged or a
    // gate declined, and everything before that point is real. Reporting it as
    // nothing would throw away a measurement; reporting it as complete would be
    // a lie. So it is `ok` AND `truncated`, with the reason kept.
    out.truncated = (rc == DF_PT_FAULT) || vt->truncated;
    out.diverged = (rc == DF_PT_FAULT) && info.diverged_at != 0;

    for (size_t i = 0; i < vt->steps_len; i++) {
        out.df.insn_off.push_back(vt->insn_off[i]);
        out.df.disasm.push_back(std::string()); // D10 text is a recorder's, not
                                                // a replay's, to invent
        out.df.step_present.push_back(1);
    }
    out.df.nsteps = static_cast<uint32_t>(vt->steps_len);
    for (size_t i = 0; i < vt->recs_len; i++) {
        const at_val_rec_t &r = vt->recs[i];
        ValRec v;
        v.step = r.step;
        v.space = r.kind == AT_LOC_REG
                      ? "reg"
                      : (r.kind == AT_LOC_MEM_OFF ? "off" : "abs");
        v.reg = r.reg;
        v.addr = r.addr;
        v.size = r.size;
        v.write = r.is_write;
        v.value_valid = r.value_valid;
        v.wide = r.wide;
        v.value = r.value;
        out.df.recs.push_back(std::move(v));
    }
    // L1 from the SAME builder the replay tier uses — a second def-use rule
    // here would be a second answer to the same question.
    asmtest_defuse_t *g = asmtest_defuse_build(vt);
    if (g != nullptr) {
        for (size_t i = 0; i < g->n; i++) {
            dt_edge e;
            e.from_step = g->edges[i].from_step;
            e.to_step = g->edges[i].to_step;
            out.df.edges.push_back(e);
            const at_val_rec_t &l = g->edges[i].loc;
            ValRec v;
            v.step = l.step;
            v.space = l.kind == AT_LOC_REG
                          ? "reg"
                          : (l.kind == AT_LOC_MEM_OFF ? "off" : "abs");
            v.reg = l.reg;
            v.addr = l.addr;
            v.size = l.size;
            v.write = l.is_write;
            v.value_valid = l.value_valid;
            v.wide = l.wide;
            v.value = l.value;
            out.df.edge_loc.push_back(std::move(v));
        }
        asmtest_defuse_free(g);
    }
    asmtest_valtrace_free(vt);
    out.ok = out.df.nsteps > 0;
    if (!out.ok && out.reason.empty())
        out.reason = "the replay produced no steps";
    return out;
#endif
}

} // namespace asmdesk
