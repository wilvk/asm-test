// forks.cpp — the take engine of forks.h. FULL BUILD ONLY (engine-linked).
#include "loom/forks.h"

#include <cstdio>
#include <cstring>

#include "asmtest_assemble.h"

// The emulator L0 value producer. It is a TIER producer, not part of the pure
// public sink surface, so it ships no public header and consumers re-declare it
// — the examples/test_dataflow_emu.c:25 precedent, which forks.cpp follows
// rather than inventing a header 05 does not own.
extern "C" int asmtest_dataflow_emu_run(const uint8_t *code, size_t code_len,
                                        const long *args, int nargs,
                                        uint64_t max_insns,
                                        asmtest_valtrace_t *vt);

// The resume-from-state seam (30 R3 T1/T2, src/dataflow_resume.c). Same
// re-declare-the-tier-producer precedent as above; only forks.cpp (full build)
// pulls emu.o in through them, so the render-only viewer stays engine-free.
extern "C" {
int asmtest_dataflow_emu_checkpoint(emu_t *e, const uint8_t *code,
                                    size_t code_len, const long *args, int nargs,
                                    uint64_t max_insns, uint64_t k,
                                    asmtest_valtrace_t *vt,
                                    emu_snapshot_t **snap_out);
int asmtest_dataflow_emu_run_from_current(emu_t *e, const uint8_t *code,
                                          size_t code_len,
                                          asmtest_valtrace_t *vt);
bool asmtest_dataflow_emu_edit_gp(emu_t *e, const char *name, uint64_t val);
bool asmtest_dataflow_emu_edit_mem(emu_t *e, uint64_t addr, const void *bytes,
                                   size_t n);
}

namespace asmdesk {

const char *const kLoomForkDisclosure =
    "forks re-run the emulator replay — an explicit crossing of the "
    "native→virtual line; never evidence about a live process or silicon "
    "timing";

namespace {

// Buffer caps for one take. Generous enough that the day-one corpus never
// truncates, and honest when it does: the producer flips `truncated` and the
// fabric's chrome tears rather than the fork quietly showing a short run.
constexpr size_t kTakeSteps = 1u << 16;
constexpr size_t kTakeRecs = 1u << 18;
constexpr size_t kTakeWide = 1u << 16;

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

} // namespace

std::string loom_edit_t::describe() const {
    if (k == kind::entry_arg)
        return "arg" + std::to_string(arg_index) +
               " := " + std::to_string(arg_value);
    return "code patch (" + std::to_string(patched_source.size()) +
           " bytes of source)";
}

std::string loom_from_step_edit_t::describe() const {
    if (k == kind::reg)
        return reg + "@step" + std::to_string(step) + " := " + hex(reg_value);
    return "[" + hex(mem_addr) + "]@step" + std::to_string(step) + " := " +
           std::to_string(mem_bytes.size()) + " byte(s)";
}

const asmtest_valtrace_t *loom_take_t::vt() const {
    vt_ = asmtest_valtrace_t{};
    vt_.insn_off =
        const_cast<uint64_t *>(insn_off.empty() ? nullptr : insn_off.data());
    vt_.steps_cap = vt_.steps_len = insn_off.size();
    vt_.steps_total = insn_off.size();
    vt_.recs = const_cast<at_val_rec_t *>(recs.empty() ? nullptr : recs.data());
    vt_.recs_cap = vt_.recs_len = recs.size();
    vt_.recs_total = recs.size();
    vt_.mem_space = AT_LOC_MEM_ABS;
    return &vt_;
}

const asmtest_defuse_t *loom_take_t::g() const {
    g_ = asmtest_defuse_t{};
    g_.edges = const_cast<asmtest_defuse_edge_t *>(
        edges.empty() ? nullptr : edges.data());
    g_.n = edges.size();
    g_.nsteps = insn_off.size();
    return &g_;
}

loom_provenance_t loom_take_t::provenance() const {
    loom_provenance_t p;
    p.producer = producer;
    p.exact = true;
    p.isolated_guest = true; // a take is ALWAYS an emulator replay
    p.steps_recorded = insn_off.size();
    p.steps_total = insn_off.size();
    return p;
}

std::string loom_take_t::fault_card() const {
    if (!result.faulted)
        return std::string();
    const char *kind = result.fault_kind == EMU_FAULT_READ    ? "read"
                       : result.fault_kind == EMU_FAULT_WRITE ? "write"
                       : result.fault_kind == EMU_FAULT_FETCH ? "fetch"
                                                              : "unknown";
    char line[256];
    // Capstone-less builds get the degraded (offset-only) form of this line,
    // which is exactly what emu_fault_describe promises — accepted, not papered
    // over (D10).
    emu_fault_describe(&result, EMU_ARCH_X86_64,
                       code.empty() ? nullptr : code.data(), code.size(),
                       EMU_CODE_BASE, line, sizeof line);
    return std::string(kind) + " fault at " + hex(result.fault_addr) + " — " +
           line;
}

bool loom_take_run(emu_t *session, const emu_snapshot_t *base_state,
                   const uint8_t *code, size_t code_len, const long *args,
                   int nargs, const loom_edit_t &edit, loom_take_t *out) {
    if (out == nullptr)
        return false;
    *out = loom_take_t{};
    out->edit = edit;
    if (code == nullptr || code_len == 0) {
        out->err = "no base code to fork from";
        return false;
    }
    if (nargs < 0 || nargs > 6) {
        out->err = "the SysV integer-argument table holds at most 6 arguments";
        return false;
    }

    // --- 1. the ONE edit ----------------------------------------------------
    out->args.assign(args, args + nargs);
    if (edit.k == loom_edit_t::kind::code_patch) {
        asm_result_t r{};
        // The all-or-nothing contract (asmtest_assemble.h:83) is doing real
        // work here: Keystone reports success after silently DROPPING a
        // statement it could only partially parse. Without the guarantee, a
        // typo'd take would weave a fabric of code the user never wrote and
        // look completely ordinary.
        asmtest_assemble(ASM_X86_64, static_cast<asm_syntax_t>(edit.syntax),
                         edit.patched_source.c_str(), EMU_CODE_BASE, &r);
        if (!r.ok) {
            out->err = r.err[0] ? r.err : "the patch did not assemble";
            asmtest_asm_free(&r);
            return false;
        }
        out->code.assign(r.bytes, r.bytes + r.len);
        asmtest_asm_free(&r);
    } else {
        out->code.assign(code, code + code_len);
        if (edit.arg_index < 0 || edit.arg_index >= nargs) {
            out->err = "arg" + std::to_string(edit.arg_index) +
                       " is outside this routine's " + std::to_string(nargs) +
                       " recorded arguments";
            return false;
        }
        out->args[edit.arg_index] = edit.arg_value;
    }

    // --- 2. the value fabric ------------------------------------------------
    // Hermetic by construction: the producer opens its own engine per call and
    // zeroes the guest, so this leg needs no snapshot bracket and two identical
    // takes are byte-identical.
    asmtest_valtrace_t *vt =
        asmtest_valtrace_new(kTakeSteps, kTakeRecs, kTakeWide);
    if (vt == nullptr) {
        out->err = "could not allocate the take's value trace";
        return false;
    }
    int rc =
        asmtest_dataflow_emu_run(out->code.data(), out->code.size(),
                                 out->args.empty() ? nullptr : out->args.data(),
                                 static_cast<int>(out->args.size()), 0, vt);
    if (rc < 0) {
        asmtest_valtrace_free(vt);
        out->err = "the emulator L0 producer could not set up this take";
        return false;
    }
    out->insn_off.assign(vt->insn_off, vt->insn_off + vt->steps_len);
    out->recs.assign(vt->recs, vt->recs + vt->recs_len);

    asmtest_defuse_t *g = asmtest_defuse_build(vt);
    if (g != nullptr) {
        out->edges.assign(g->edges, g->edges + g->n);
        asmtest_defuse_free(g);
    }
    const bool producer_truncated = vt->truncated;
    asmtest_valtrace_free(vt);

    // --- 3. the fault card + the ordered offsets ----------------------------
    if (session != nullptr) {
        // MANDATORY bracket. Mapped memory persists across emu_call_*
        // (asmtest_emu.h:586) — that is the caller's preload mechanism, and it
        // is exactly why take N would otherwise inherit take N-1's dirt and the
        // fork verdicts would depend on click order.
        if (base_state != nullptr && !emu_restore(session, base_state))
            out->err = "could not restore the fork session's base state; this "
                       "take's fault card may reflect a previous take's memory";
        uint64_t insns[4096];
        asmtest_trace_t tr{};
        tr.insns = insns;
        tr.insns_cap = sizeof insns / sizeof insns[0];
        emu_call_traced(session, out->code.data(), out->code.size(),
                        out->args.empty() ? nullptr : out->args.data(),
                        static_cast<int>(out->args.size()), 0, &out->result,
                        &tr);
        out->trace_insns.assign(tr.insns, tr.insns + tr.insns_len);
        out->trace_truncated = tr.truncated;
    } else if (out->err.empty()) {
        out->err = "no emulator session: this take carries its value fabric "
                   "but no fault card";
    }

    if (producer_truncated && out->err.empty())
        out->err = "the take's operand buffers filled: its fabric is a lower "
                   "bound";
    return true;
}

bool loom_take_run_from_step(const uint8_t *code, size_t code_len,
                             const long *args, int nargs,
                             const loom_from_step_edit_t &edit,
                             loom_take_t *out) {
    if (out == nullptr)
        return false;
    *out = loom_take_t{};
    out->producer = "dataflow-emu (reweave@" + std::to_string(edit.step) + ")";
    if (code == nullptr || code_len == 0) {
        out->err = "no base code to reweave from";
        return false;
    }
    if (nargs < 0 || nargs > 6) {
        out->err = "the SysV integer-argument table holds at most 6 arguments";
        return false;
    }
    out->code.assign(code, code + code_len);
    out->args.assign(args, args + nargs);

    // The resume seam maps CODE + STACK at fixed guest bases, so it needs a FRESH
    // handle — a reweave opens its own (and is hermetic like the value producer:
    // two identical reweaves are byte-identical, needing no shared session).
    emu_t *e = emu_open();
    if (e == nullptr) {
        out->err = "could not open the emulator handle for the reweave";
        return false;
    }
    auto fail = [&](const std::string &why, asmtest_valtrace_t *b,
                    asmtest_valtrace_t *t, emu_snapshot_t *s) {
        out->err = why;
        if (t != nullptr)
            asmtest_valtrace_free(t);
        if (b != nullptr)
            asmtest_valtrace_free(b);
        if (s != nullptr)
            emu_snapshot_free(s);
        emu_close(e);
        return false;
    };

    // --- 1. checkpoint the run at step K (yields the full PARENT trace too) --
    asmtest_valtrace_t *base =
        asmtest_valtrace_new(kTakeSteps, kTakeRecs, kTakeWide);
    if (base == nullptr)
        return fail("could not allocate the reweave's base value trace", nullptr,
                    nullptr, nullptr);
    emu_snapshot_t *snap = nullptr;
    int rcb = asmtest_dataflow_emu_checkpoint(
        e, code, code_len, out->args.empty() ? nullptr : out->args.data(), nargs,
        0, edit.step, base, &snap);
    if (rcb < 0)
        return fail("the emulator L0 producer could not set up this reweave",
                    base, nullptr, nullptr);
    if (snap == nullptr)
        return fail("step " + std::to_string(edit.step) + " is at or past this "
                    "run's " + std::to_string(base->steps_len) +
                    " steps — no checkpoint to reweave from",
                    base, nullptr, nullptr);

    // --- 2. restore to pre-K, apply the ONE edit, resume forward ------------
    if (!emu_restore(e, snap))
        return fail("could not restore the checkpoint at step " +
                        std::to_string(edit.step),
                    base, nullptr, snap);
    bool edited =
        edit.k == loom_from_step_edit_t::kind::reg
            ? asmtest_dataflow_emu_edit_gp(e, edit.reg.c_str(), edit.reg_value)
            : (!edit.mem_bytes.empty() &&
               asmtest_dataflow_emu_edit_mem(e, edit.mem_addr,
                                             edit.mem_bytes.data(),
                                             edit.mem_bytes.size()));
    if (!edited)
        return fail(edit.k == loom_from_step_edit_t::kind::reg
                        ? "unknown register '" + edit.reg +
                              "' for the reweave edit"
                        : "the reweave memory edit could not be applied "
                          "(unmapped address or empty bytes)",
                    base, nullptr, snap);
    asmtest_valtrace_t *tail =
        asmtest_valtrace_new(kTakeSteps, kTakeRecs, kTakeWide);
    if (tail == nullptr)
        return fail("could not allocate the reweave's tail value trace", base,
                    nullptr, snap);
    int rct = asmtest_dataflow_emu_run_from_current(e, code, code_len, tail);
    if (rct < 0)
        return fail("the reweave resume could not run forward from step " +
                        std::to_string(edit.step),
                    base, tail, snap);

    // --- 3. STITCH the unchanged parent prefix [0,K) with the edited tail ---
    // A reweave is a full worldline that shares its parent's prefix and diverges
    // at the edit, so it drops straight into the take-view divergence card.
    const uint32_t K = static_cast<uint32_t>(edit.step);
    for (size_t i = 0; i < K && i < base->steps_len; i++)
        out->insn_off.push_back(base->insn_off[i]);
    for (size_t j = 0; j < tail->steps_len; j++)
        out->insn_off.push_back(tail->insn_off[j]);
    for (size_t i = 0; i < base->recs_len; i++)
        if (base->recs[i].step < K)
            out->recs.push_back(base->recs[i]);
    for (size_t i = 0; i < tail->recs_len; i++) {
        at_val_rec_t r = tail->recs[i];
        r.step += K; // re-base the tail's step 0 onto absolute step K
        out->recs.push_back(r);
    }
    const bool truncated = base->truncated || tail->truncated;
    asmtest_valtrace_free(base);
    asmtest_valtrace_free(tail);
    emu_snapshot_free(snap);
    emu_close(e);

    // --- 4. def-use over the stitched worldline (rebuilt, so the edges across
    //        the K boundary reflect the counterfactual, not the parent) -------
    asmtest_defuse_t *g = asmtest_defuse_build(out->vt());
    if (g != nullptr) {
        out->edges.assign(g->edges, g->edges + g->n);
        asmtest_defuse_free(g);
    }

    if (truncated)
        out->err = "the reweave's operand buffers filled: its fabric is a lower "
                   "bound";
    else if (rct == 1)
        // A guest fault AFTER the edit is a valid counterfactual outcome, not a
        // build failure — the (partial) fabric stands; name it, never hide it.
        out->err = "the reweave faulted after the edit at step " +
                   std::to_string(edit.step);
    return true;
}

} // namespace asmdesk
