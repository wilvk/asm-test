/*
 * dataflow_dr_client.c — the in-process DynamoRIO client (libasmtest_drval_client.so)
 * for the data-flow tier's L0 VALUE producer (Phase 5, increment 1): the in-band,
 * whole-process analog of the scoped ptrace value producer (src/dataflow_ptrace.c).
 * See include-side asmtest_valtrace.h, src/dataflow_dr.c, and docs/internal/plans/
 * data-flow-tracing-plan.md Phase 5.
 *
 * Where the sibling control-flow client (src/drtrace_client.c) records which BLOCKS
 * ran into an asmtest_trace_t, this client records, per instrumented instruction, a
 * VALUE snapshot into the app-owned at_drval_t (src/dataflow_dr.h): the GP register
 * file at the step (dr_get_mcontext) and the effective address + loaded value of each
 * explicit memory SOURCE operand (its own decode + opnd_compute_address +
 * dr_safe_read — the "instrumented memory refs" the plan names). The app side replays
 * those snapshots into the shared asmtest_valtrace_t.
 *
 * Like the control client it uses ONLY DynamoRIO's BSD core API (dr_register_bb_event,
 * clean calls, dr_get_mcontext, decode, opnd_compute_address, dr_safe_read) — no
 * drmgr/drreg/umbra: the prebuilt release extensions fail to load under DR's private
 * loader on modern glibc, and staying on the core API keeps the tier license-clean
 * (DR core is BSD). Inlined instrumentation with drreg-reserved scratch regs + umbra
 * shadow taint is the Phase-5 END goal; this first increment is a clean-call VALUE
 * producer that cross-validates against the emulator oracle. DR-side tag propagation
 * and whole-process breadth beyond a single registered region are LATER increments.
 *
 * The at_drval_t layout is shared verbatim via src/dataflow_dr.h, which is <stdint.h>-
 * only (no <stdbool.h>, whose `bool` clashes with DynamoRIO's `bool`), so unlike the
 * control client's hand-mirrored at_trace_t this one includes the real struct.
 */
#include "dr_api.h"

#include "dataflow_dr.h"

#include <string.h>

/* Single registered value-capture region. The producer registers exactly one range
 * per run (a scoped routine), set once before the traced code executes and read
 * unlocked in the per-instruction hot path — the same cheap-default contract the
 * control client documents (register before tracing starts; do not (un)register
 * while traced code runs). */
typedef struct {
    app_pc base;
    size_t len;
    at_drval_t *drval;
} region_t;
static region_t g_region;
static void *g_lock; /* dr_mutex guarding the single registration */

/* Resolved marker PC (NULL until the drapp/executable module exposing it is seen). */
static app_pc pc_marker;

/* The GP snapshot order (src/dataflow_dr.h) expressed in DynamoRIO reg ids. */
static const reg_id_t g_gpr_order[AT_GPR_COUNT] = {
    DR_REG_RAX, DR_REG_RBX, DR_REG_RCX, DR_REG_RDX, DR_REG_RSI, DR_REG_RDI,
    DR_REG_RBP, DR_REG_RSP, DR_REG_R8,  DR_REG_R9,  DR_REG_R10, DR_REG_R11,
    DR_REG_R12, DR_REG_R13, DR_REG_R14, DR_REG_R15};

/* ------------------------------------------------------------------ */
/* Clean-call sinks (run in the app address space)                     */
/* ------------------------------------------------------------------ */

/* Marker clean call: learn the range [base, base+len) and the app-owned capture
 * buffer from the marker's SysV argument registers, then re-instrument the range so
 * a freshly-mmap'd (never-executed) region picks up the value instrumentation. */
static void on_marker(app_pc base, size_t len, at_drval_t *drval) {
    dr_mutex_lock(g_lock);
    g_region.base = base;
    g_region.len = len;
    g_region.drval = drval;
    dr_mutex_unlock(g_lock);
    dr_delay_flush_region(base, len, 0, NULL);
}

/* Per-instruction clean call (inserted BEFORE the instruction, so the mcontext is the
 * instruction's PRE / source state): append one step snapshot to the capture buffer.
 * `ipc` is the instruction's application PC (its bytes are live in the app, so the
 * client decodes them to walk the memory source operands). */
static void on_step(at_drval_t *dv, uint64_t off, app_pc ipc) {
    if (dv == NULL)
        return;
    dv->steps_total++;
    if (dv->steps == NULL || dv->steps_len >= dv->steps_cap) {
        dv->truncated = 1;
        return;
    }
    void *dc = dr_get_current_drcontext();
    dr_mcontext_t mc;
    mc.size = sizeof(mc);
    mc.flags = DR_MC_INTEGER | DR_MC_CONTROL; /* GP file + xsp/xflags/pc */
    if (!dr_get_mcontext(dc, &mc)) {
        dv->truncated = 1;
        return;
    }

    at_vstep_t *st = &dv->steps[dv->steps_len];
    memset(st, 0, sizeof *st);
    st->off = off;
    for (int i = 0; i < AT_GPR_COUNT; i++)
        st->gpr[i] = (uint64_t)reg_get_value(g_gpr_order[i], &mc);
    st->rip = (uint64_t)(ptr_uint_t)mc.pc;
    st->rflags = (uint64_t)mc.xflags;
    st->mem_first = (uint32_t)dv->mem_len;
    st->mem_n = 0;

    /* Explicit memory SOURCE operands: resolve the effective address from the live
     * registers and read the loaded value (pre-instruction). Segmented (fs:/gs:) and
     * VSIB vector-gather operands are skipped here (their EA math is a later
     * increment); a store's value is post-instruction and not captured. */
    instr_t instr;
    instr_init(dc, &instr);
    if (decode(dc, ipc, &instr) != NULL) {
        int nsrc = instr_num_srcs(&instr);
        for (int s = 0; s < nsrc; s++) {
            opnd_t op = instr_get_src(&instr, s);
            if (!opnd_is_memory_reference(op))
                continue;
            if (opnd_is_far_memory_reference(op))
                continue; /* fs:/gs:-segmented: skipped this increment */
            if (dv->mem == NULL || dv->mem_len >= dv->mem_cap) {
                dv->truncated = 1;
                break;
            }
            app_pc ea = opnd_compute_address(op, &mc);
            uint16_t sz = (uint16_t)opnd_size_in_bytes(opnd_get_size(op));
            at_vmem_t *m = &dv->mem[dv->mem_len];
            memset(m, 0, sizeof *m);
            m->ea = (uint64_t)(ptr_uint_t)ea;
            m->size = sz;
            size_t want = sz < 8 ? sz : 8;
            uint64_t val = 0;
            size_t got = 0;
            if (want > 0 && ea != NULL && dr_safe_read(ea, want, &val, &got) &&
                got == want) {
                m->value = val;
                m->valid = 1;
            }
            dv->mem_len++;
            st->mem_n++;
        }
    }
    instr_free(dc, &instr);

    dv->steps_len++;
}

/* ------------------------------------------------------------------ */
/* Basic-block instrumentation                                         */
/* ------------------------------------------------------------------ */

static dr_emit_flags_t event_bb(void *drcontext, void *tag, instrlist_t *bb,
                                bool for_trace, bool translating) {
    (void)for_trace;
    (void)translating;
    app_pc pc = dr_fragment_app_pc(tag);
    instr_t *first = instrlist_first(bb);

    if (pc == pc_marker) {
        dr_insert_clean_call(drcontext, bb, first, (void *)on_marker, false, 3,
                             opnd_create_reg(DR_REG_RDI),
                             opnd_create_reg(DR_REG_RSI),
                             opnd_create_reg(DR_REG_RDX));
        return DR_EMIT_DEFAULT;
    }

    /* Read the single region unlocked (the hot path; see the contract above). */
    at_drval_t *dv = g_region.drval;
    app_pc base = g_region.base;
    size_t len = g_region.len;
    if (dv != NULL && pc >= base && pc < base + len) {
        for (instr_t *in = first; in != NULL; in = instr_get_next(in)) {
            app_pc ipc = instr_get_app_pc(in);
            if (ipc == NULL || ipc < base || ipc >= base + len)
                continue;
            dr_insert_clean_call(drcontext, bb, in, (void *)on_step, false, 3,
                                 OPND_CREATE_INTPTR(dv),
                                 OPND_CREATE_INT64((uint64_t)(ipc - base)),
                                 OPND_CREATE_INTPTR(ipc));
        }
    }
    return DR_EMIT_DEFAULT;
}

/* ------------------------------------------------------------------ */
/* Marker resolution + lifecycle                                       */
/* ------------------------------------------------------------------ */

static void try_resolve(module_handle_t h) {
    if (pc_marker == NULL)
        pc_marker = (app_pc)dr_get_proc_address(h, AT_DRVAL_MARKER_SYM);
}

static void resolve_all_modules(void) {
    dr_module_iterator_t *it = dr_module_iterator_start();
    while (dr_module_iterator_hasnext(it)) {
        module_data_t *m = dr_module_iterator_next(it);
        try_resolve(m->handle);
        dr_free_module_data(m);
    }
    dr_module_iterator_stop(it);
}

static void event_module_load(void *drcontext, const module_data_t *info,
                              bool loaded) {
    (void)drcontext;
    (void)loaded;
    try_resolve(info->handle);
}

static void event_exit(void) { dr_mutex_destroy(g_lock); }

/* Deliver every signal to the application's own handler (managed-runtime
 * coexistence: null-check SIGSEGV etc.), matching the control client. The value
 * tracer never needs to intercept signals. */
static dr_signal_action_t event_signal(void *drcontext, dr_siginfo_t *info) {
    (void)drcontext;
    (void)info;
    return DR_SIGNAL_DELIVER;
}

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
    (void)id;
    (void)argc;
    (void)argv;
    dr_set_client_name("asm-test data-flow value client", "");
    g_lock = dr_mutex_create();
    resolve_all_modules();
    dr_register_module_load_event(event_module_load);
    dr_register_bb_event(event_bb);
    dr_register_signal_event(event_signal);
    dr_register_exit_event(event_exit);
}
