/*
 * dataflow_dr_client_inlined.c — the INLINED data-flow L0 VALUE producer client
 * (libasmtest_drval_client_inlined.so) for the DynamoRIO taint tier
 * (dynamorio-taint-tier-plan.md, Increment 3).
 *
 * This is the re-platform of the shipped clean-call client (dataflow_dr_client.c)
 * onto DynamoRIO's standard extension stack: drmgr phased instrumentation, drreg
 * scratch-register reservation, and a drx_buf per-thread trace buffer — the
 * drcachesim/memtrace idiom. It fills the SAME app-owned at_drval_t (dataflow_dr.h)
 * the clean-call client fills, so the app-side replay (dataflow_dr.c) and the
 * emulator-oracle cross-check (dr_valtrace) validate it UNCHANGED: point
 * ASMTEST_DRVAL_CLIENT at this .so instead and the same gate runs. The shipped
 * clean-call client stays intact as the fallback/oracle during the swap.
 *
 * WHY IT IS FASTER. The clean-call client pays a full machine-context save/restore
 * per instrumented instruction (dr_insert_clean_call). This client emits the
 * capture inline: drreg hands out a few scratch GPRs (spilling only those), the
 * capture is a handful of movs/leas, and drx_buf batches the records with one
 * bounds check per step, flushing to at_drval_t in a C callback.
 *
 * WHAT IS CAPTURED (byte-identical to the clean-call client on the fixtures):
 *   - off, rip: COMPILE-TIME constants for each instrumented instruction (its
 *     region offset and app PC), emitted as immediates — no runtime read.
 *   - gpr[16]: the app value of each GP register, via drreg_get_app_value (which
 *     restores from drreg's spill slot the ones drreg borrowed for scratch).
 *   - explicit memory SOURCE operands: EA via an inlined lea over the
 *     app-restored base/index (drreg_restore_app_values), value via an inlined
 *     load. mem_n / sizes are compile-time and emitted as immediates.
 *
 * DELIBERATE DIVERGENCES from the clean call, each principled, not a bug:
 *   - rflags VALUE is clean-call-only and is stored 0 here. The only inline way to
 *     read the full rflags is pushfq, which writes the app red zone ([rsp-8]) — and
 *     the fixture literally stores its live value there, so pushfq would corrupt it.
 *     lahf/seto give only the arith subset, not mc.xflags. Flag def-use LOCATIONS
 *     still enter the graph (the enumerator emits them); only the flag VALUE is
 *     absent. The fixtures do not consume flag values, so the oracle gate is
 *     unaffected. (Surfacing this: full-context fields like rflags are a genuine
 *     reason to keep a narrow clean call — noted for the taint tier.)
 *   - The inlined memory-value load assumes a valid EA (a faulting load crashes the
 *     app, where the clean call's dr_safe_read fails gracefully). The enumeration
 *     gate now bounds this to GENUINE loads: `instr_reads_memory` skips no-load
 *     forms (lea agen, `nop [mem]`, prefetch) whose EA the app never dereferences,
 *     so the only remaining exposure is a real load of a genuinely-unmapped address
 *     — where the app instruction itself faults one step later anyway, so there is
 *     no divergence from unmodified execution. A fully fault-safe inline load is
 *     still a later increment, matching the clean-call client's "breadth deferred"
 *     posture.
 *   - A captured memory operand whose base/index aliases the drx_buf-pointer
 *     register (s_ptr) is skipped (value left unfilled): drreg_restore_app_values
 *     restores that register in place and would destroy the buffer pointer. Rare
 *     (needs drreg to pick a base/index register as scratch under register
 *     pressure); the location still enters def-use via the app-side enumerator.
 *   - RIP-relative / far / segmented (fs:/gs:) memory operands are skipped inline
 *     (the clean call resolves RIP-relative via opnd_compute_address; the fixtures
 *     have none). Same far/segmented exclusion the clean-call client documents.
 *   - DEAD register slots are not literal. dr_get_mcontext (the clean call) snapshots
 *     the actual register file including registers dead at that step; drreg treats a
 *     dead register as free scratch, so drreg_get_app_value returns a scratch/stale
 *     value for it. This affects ONLY never-consumed slots: a register's value that
 *     the value trace never reads (a dead-on-entry incoming value). Every value the
 *     def-use graph / slices actually consume — every live read, and every write
 *     valued from the next step's LIVE snapshot — matches the clean call, which is
 *     why the emulator-oracle cross-check passes identically. Byte-for-byte identity
 *     of the full literal register file is therefore a clean-call-only property, like
 *     rflags; the *semantic* value trace is identical.
 *
 * Linux x86-64 only. Uses the BSD-clean extension stack proved to load by
 * Increment 2 (drmgr/drreg/drx); NO umbra (LGPL-2.1) and NO drwrap — marker/arg
 * resolution stays PC-resolved via dr_get_proc_address + a SysV-arg clean call,
 * exactly as the clean-call client does.
 */
#include "dr_api.h"
#include "drmgr.h"
#include "drreg.h"
#include "drx.h"

#include "dataflow_dr.h"

#include <string.h>

/* Up to this many explicit memory source operands captured per instruction. The
 * app allocates mem_cap = 4*steps_cap (dataflow_dr.c), so 4/insn matches. */
#define AT_INLINE_MAXMEM 4

/* The fixed-size per-step record the inlined instrumentation writes into the
 * drx_buf trace buffer; the flush callback reconstructs at_drval_t from it. Kept
 * flat and fixed so drx_buf's stride is a compile-time constant. */
typedef struct {
    uint64_t off;
    uint64_t rip;
    uint64_t gpr[AT_GPR_COUNT];
    uint32_t mem_n;
    uint32_t pad0;
    uint64_t mem_ea[AT_INLINE_MAXMEM];
    uint64_t mem_val[AT_INLINE_MAXMEM];
    uint16_t mem_size[AT_INLINE_MAXMEM];
    uint8_t mem_valid[AT_INLINE_MAXMEM];
    uint8_t pad1[4];
} raw_step_t;

/* Single registered value-capture region (same contract as the clean-call client:
 * set once before traced code runs, read unlocked on the hot path). */
typedef struct {
    app_pc base;
    size_t len;
    at_drval_t *drval;
} region_t;
static region_t g_region;
static void *g_lock;

static app_pc pc_marker;
static drx_buf_t *g_buf;

/* GP snapshot order (dataflow_dr.h) in DynamoRIO reg ids. */
static const reg_id_t g_gpr_order[AT_GPR_COUNT] = {
    DR_REG_RAX, DR_REG_RBX, DR_REG_RCX, DR_REG_RDX, DR_REG_RSI, DR_REG_RDI,
    DR_REG_RBP, DR_REG_RSP, DR_REG_R8,  DR_REG_R9,  DR_REG_R10, DR_REG_R11,
    DR_REG_R12, DR_REG_R13, DR_REG_R14, DR_REG_R15};

/* ------------------------------------------------------------------ */
/* drx_buf flush: drain raw records into the app-owned at_drval_t        */
/* ------------------------------------------------------------------ */

static void buf_flush(void *drcontext, void *buf_base, size_t size) {
    (void)drcontext;
    at_drval_t *dv = g_region.drval;
    if (dv == NULL)
        return;
    size_t n = size / sizeof(raw_step_t);
    const raw_step_t *recs = (const raw_step_t *)buf_base;
    for (size_t i = 0; i < n; i++) {
        const raw_step_t *rs = &recs[i];
        dv->steps_total++;
        if (dv->steps == NULL || dv->steps_len >= dv->steps_cap) {
            dv->truncated = 1;
            continue;
        }
        at_vstep_t *st = &dv->steps[dv->steps_len];
        memset(st, 0, sizeof *st);
        st->off = rs->off;
        for (int g = 0; g < AT_GPR_COUNT; g++)
            st->gpr[g] = rs->gpr[g];
        st->rip = rs->rip;
        st->rflags = 0; /* clean-call-only field; see file header */
        st->mem_first = (uint32_t)dv->mem_len;
        st->mem_n = 0;
        uint32_t mn = rs->mem_n <= AT_INLINE_MAXMEM ? rs->mem_n : AT_INLINE_MAXMEM;
        for (uint32_t j = 0; j < mn; j++) {
            if (dv->mem == NULL || dv->mem_len >= dv->mem_cap) {
                dv->truncated = 1;
                break;
            }
            at_vmem_t *m = &dv->mem[dv->mem_len];
            memset(m, 0, sizeof *m);
            m->ea = rs->mem_ea[j];
            m->value = rs->mem_val[j];
            m->size = rs->mem_size[j];
            m->valid = rs->mem_valid[j];
            dv->mem_len++;
            st->mem_n++;
        }
        dv->steps_len++;
    }
}

/* ------------------------------------------------------------------ */
/* Marker clean call (once; not the hot path) — learn region + buffer   */
/* ------------------------------------------------------------------ */

static void on_marker(app_pc base, size_t len, at_drval_t *drval) {
    dr_mutex_lock(g_lock);
    g_region.base = base;
    g_region.len = len;
    g_region.drval = drval;
    dr_mutex_unlock(g_lock);
    dr_delay_flush_region(base, len, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* Inlined per-instruction capture                                      */
/* ------------------------------------------------------------------ */

/* Materialize a 64-bit immediate into val_reg and store it into the buffer slot.
 * On x86-64 a [buf_ptr+disp] store needs no scratch, so pass DR_REG_NULL (the
 * bbbuf sample idiom). */
static void store_imm64(void *dc, instrlist_t *bb, instr_t *where, reg_id_t buf_ptr,
                        reg_id_t val_reg, uint64_t imm, short offset) {
    instrlist_meta_preinsert(
        bb, where,
        INSTR_CREATE_mov_imm(dc, opnd_create_reg(val_reg), OPND_CREATE_INTPTR(imm)));
    drx_buf_insert_buf_store(dc, g_buf, bb, where, buf_ptr, DR_REG_NULL,
                             opnd_create_reg(val_reg), OPSZ_8, offset);
}

/* A memory operand we can resolve inline: a plain base+index*scale+disp load, not
 * RIP-relative, far, or segmented. Returns the byte size (0 = not capturable). */
static uint16_t capturable_mem_size(opnd_t op) {
    if (!opnd_is_base_disp(op))
        return 0; /* rip-rel abs-addr / other forms deferred */
    if (opnd_is_far_memory_reference(op))
        return 0; /* fs:/gs: segmented deferred */
    if (opnd_get_segment(op) != DR_REG_NULL)
        return 0;
    uint16_t sz = (uint16_t)opnd_size_in_bytes(opnd_get_size(op));
    if (sz != 1 && sz != 2 && sz != 4 && sz != 8)
        return 0; /* only 1/2/4/8 inline loads this increment */
    return sz;
}

static dr_emit_flags_t event_insert(void *dc, void *tag, instrlist_t *bb,
                                    instr_t *instr, bool for_trace,
                                    bool translating, void *user_data) {
    (void)tag;
    (void)for_trace;
    (void)translating;
    (void)user_data;

    if (!instr_is_app(instr))
        return DR_EMIT_DEFAULT;
    app_pc ipc = instr_get_app_pc(instr);
    if (ipc == NULL)
        return DR_EMIT_DEFAULT;

    /* Marker: single SysV-arg clean call, PC-resolved, no drwrap (as clean-call). */
    if (ipc == pc_marker) {
        dr_insert_clean_call(dc, bb, instr, (void *)on_marker, false, 3,
                             opnd_create_reg(DR_REG_RDI),
                             opnd_create_reg(DR_REG_RSI),
                             opnd_create_reg(DR_REG_RDX));
        return DR_EMIT_DEFAULT;
    }

    at_drval_t *dv = g_region.drval;
    app_pc base = g_region.base;
    size_t len = g_region.len;
    if (dv == NULL || ipc < base || ipc >= base + len)
        return DR_EMIT_DEFAULT;

    /* Reserve three scratch GPRs FIRST (buffer pointer + two work registers); the
     * memory-operand enumeration below needs s_ptr to skip operands whose base or
     * index aliases it. (Arithmetic flags are reserved later, only around the
     * trace-buffer update — the mov/lea/movzx capture here is flag-neutral.) */
    reg_id_t s_ptr, s_a, s_b;
    if (drreg_reserve_register(dc, bb, instr, NULL, &s_ptr) != DRREG_SUCCESS)
        return DR_EMIT_DEFAULT;
    if (drreg_reserve_register(dc, bb, instr, NULL, &s_a) != DRREG_SUCCESS) {
        drreg_unreserve_register(dc, bb, instr, s_ptr);
        return DR_EMIT_DEFAULT;
    }
    if (drreg_reserve_register(dc, bb, instr, NULL, &s_b) != DRREG_SUCCESS) {
        drreg_unreserve_register(dc, bb, instr, s_a);
        drreg_unreserve_register(dc, bb, instr, s_ptr);
        return DR_EMIT_DEFAULT;
    }

    /* Enumerate the capturable explicit memory SOURCE operands (count/sizes are
     * compile-time, emitted as immediates below). Two conservative gates beyond
     * capturable_mem_size — a skipped operand's VALUE is simply left unfilled; the
     * app-side enumerator still produces its read record and resolves the location
     * from the register snapshot, so def-use/slices are unaffected:
     *  - instr_reads_memory: skip NO-LOAD forms (lea agen, `nop [mem]`, prefetch)
     *    whose "source" memory operand the instruction never dereferences. An
     *    inline load of them would fault on an unmapped/non-pointer address the app
     *    itself never touches — unlike a real load, where the app would fault too,
     *    so the "assumes a valid EA" divergence (file header) stays bounded to
     *    genuine loads.
     *  - base/index != s_ptr: if the buffer-pointer register aliases this operand's
     *    base or index, drreg_restore_app_values (below) would restore it IN PLACE
     *    and destroy the buffer pointer — the same in-place-restore hazard the GPR
     *    loop guards against. Skip rather than corrupt the capture / app memory. */
    opnd_t memops[AT_INLINE_MAXMEM];
    uint16_t memsz[AT_INLINE_MAXMEM];
    uint32_t nmem = 0;
    bool reads_mem = instr_reads_memory(instr);
    for (int s = 0; reads_mem && s < instr_num_srcs(instr) && nmem < AT_INLINE_MAXMEM;
         s++) {
        opnd_t op = instr_get_src(instr, s);
        if (!opnd_is_memory_reference(op))
            continue;
        uint16_t sz = capturable_mem_size(op);
        if (sz == 0)
            continue;
        if (opnd_get_base(op) == s_ptr || opnd_get_index(op) == s_ptr)
            continue;
        memops[nmem] = op;
        memsz[nmem] = sz;
        nmem++;
    }

    drx_buf_insert_load_buf_ptr(dc, g_buf, bb, instr, s_ptr);

    /* off, rip: compile-time immediates. */
    store_imm64(dc, bb, instr, s_ptr, s_a, (uint64_t)(ipc - base),
                (short)offsetof(raw_step_t, off));
    store_imm64(dc, bb, instr, s_ptr, s_a, (uint64_t)(ptr_uint_t)ipc,
                (short)offsetof(raw_step_t, rip));

    /* mem_n immediate (materialize as a full 64-bit imm; store the low 4 bytes). */
    instrlist_meta_preinsert(
        bb, instr,
        INSTR_CREATE_mov_imm(dc, opnd_create_reg(s_a), OPND_CREATE_INTPTR((ptr_uint_t)nmem)));
    drx_buf_insert_buf_store(dc, g_buf, bb, instr, s_ptr, DR_REG_NULL,
                             opnd_create_reg(reg_resize_to_opsz(s_a, OPSZ_4)), OPSZ_4,
                             (short)offsetof(raw_step_t, mem_n));

    /* gpr[16]: each register's APP value. drreg_get_app_value(X, s_a) restores X
     * IN PLACE, so capturing the register that backs s_ptr here would destroy the
     * buffer pointer — skip s_ptr in the loop and capture it last (below), after
     * copying the buffer pointer to s_b. s_a/s_b are safe to restore in place. */
    for (int g = 0; g < AT_GPR_COUNT; g++) {
        if (g_gpr_order[g] == s_ptr)
            continue;
        drreg_get_app_value(dc, bb, instr, g_gpr_order[g], s_a);
        drx_buf_insert_buf_store(dc, g_buf, bb, instr, s_ptr, DR_REG_NULL,
                                 opnd_create_reg(s_a), OPSZ_8,
                                 (short)(offsetof(raw_step_t, gpr) + g * 8));
    }

    /* memory operands: EA (inlined lea over app-restored base/index) + value. */
    for (uint32_t j = 0; j < nmem; j++) {
        opnd_t op = memops[j];
        reg_id_t swap = DR_REG_NULL;
        drreg_restore_app_values(dc, bb, instr, op, &swap);

        opnd_t addr = opnd_create_base_disp(opnd_get_base(op), opnd_get_index(op),
                                            opnd_get_scale(op), opnd_get_disp(op),
                                            OPSZ_lea);
        instrlist_meta_preinsert(
            bb, instr, INSTR_CREATE_lea(dc, opnd_create_reg(s_a), addr));
        drx_buf_insert_buf_store(dc, g_buf, bb, instr, s_ptr, DR_REG_NULL,
                                 opnd_create_reg(s_a), OPSZ_8,
                                 (short)(offsetof(raw_step_t, mem_ea) + j * 8));

        /* Inlined value load [s_a] -> s_b, zero-extended per size (see header:
         * assumes a valid EA). */
        opnd_t src;
        if (memsz[j] == 8) {
            src = opnd_create_base_disp(s_a, DR_REG_NULL, 0, 0, OPSZ_8);
            instrlist_meta_preinsert(
                bb, instr, INSTR_CREATE_mov_ld(dc, opnd_create_reg(s_b), src));
        } else if (memsz[j] == 4) {
            src = opnd_create_base_disp(s_a, DR_REG_NULL, 0, 0, OPSZ_4);
            instrlist_meta_preinsert(
                bb, instr,
                INSTR_CREATE_mov_ld(
                    dc, opnd_create_reg(reg_resize_to_opsz(s_b, OPSZ_4)), src));
        } else { /* 1 or 2 */
            src = opnd_create_base_disp(s_a, DR_REG_NULL, 0, 0,
                                        memsz[j] == 2 ? OPSZ_2 : OPSZ_1);
            instrlist_meta_preinsert(
                bb, instr,
                INSTR_CREATE_movzx(dc, opnd_create_reg(s_b), src));
        }
        drx_buf_insert_buf_store(dc, g_buf, bb, instr, s_ptr, DR_REG_NULL,
                                 opnd_create_reg(s_b), OPSZ_8,
                                 (short)(offsetof(raw_step_t, mem_val) + j * 8));

        if (swap != DR_REG_NULL)
            drreg_unreserve_register(dc, bb, instr, swap);

        /* size + valid: compile-time immediates (materialize 64-bit, store narrow). */
        instrlist_meta_preinsert(
            bb, instr,
            INSTR_CREATE_mov_imm(dc, opnd_create_reg(s_a),
                                 OPND_CREATE_INTPTR((ptr_uint_t)memsz[j])));
        drx_buf_insert_buf_store(dc, g_buf, bb, instr, s_ptr, DR_REG_NULL,
                                 opnd_create_reg(reg_resize_to_opsz(s_a, OPSZ_2)),
                                 OPSZ_2,
                                 (short)(offsetof(raw_step_t, mem_size) + j * 2));
        instrlist_meta_preinsert(
            bb, instr,
            INSTR_CREATE_mov_imm(dc, opnd_create_reg(s_a), OPND_CREATE_INTPTR(1)));
        drx_buf_insert_buf_store(dc, g_buf, bb, instr, s_ptr, DR_REG_NULL,
                                 opnd_create_reg(reg_resize_to_opsz(s_a, OPSZ_1)),
                                 OPSZ_1,
                                 (short)(offsetof(raw_step_t, mem_valid) + j));
    }

    /* Capture the buffer-pointer register's own app value LAST: copy the buffer
     * pointer into s_b, restore app-s_ptr into s_a (clobbering s_ptr, now dead as
     * a pointer), and store it via the s_b copy. The buffer pointer now lives in
     * s_b for the update below. */
    for (int g = 0; g < AT_GPR_COUNT; g++) {
        if (g_gpr_order[g] != s_ptr)
            continue;
        instrlist_meta_preinsert(
            bb, instr,
            INSTR_CREATE_mov_ld(dc, opnd_create_reg(s_b), opnd_create_reg(s_ptr)));
        drreg_get_app_value(dc, bb, instr, s_ptr, s_a);
        drx_buf_insert_buf_store(dc, g_buf, bb, instr, s_b, DR_REG_NULL,
                                 opnd_create_reg(s_a), OPSZ_8,
                                 (short)(offsetof(raw_step_t, gpr) + g * 8));
        break;
    }

    /* Advance the buffer pointer (via the s_b copy). This is the ONE piece of our
     * instrumentation that clobbers arithmetic flags (the trace buffer's fill
     * bounds check), so reserve aflags just for it — all the capture above is
     * flag-neutral mov/lea, so reserving aflags late keeps its lahf/rax spill from
     * perturbing the register capture. */
    drreg_status_t af = drreg_reserve_aflags(dc, bb, instr);
    drx_buf_insert_update_buf_ptr(dc, g_buf, bb, instr, s_b, s_a,
                                  sizeof(raw_step_t));
    if (af == DRREG_SUCCESS)
        drreg_unreserve_aflags(dc, bb, instr);

    drreg_unreserve_register(dc, bb, instr, s_b);
    drreg_unreserve_register(dc, bb, instr, s_a);
    drreg_unreserve_register(dc, bb, instr, s_ptr);
    return DR_EMIT_DEFAULT;
}

/* ------------------------------------------------------------------ */
/* Marker resolution + lifecycle                                        */
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

static dr_signal_action_t event_signal(void *drcontext, dr_siginfo_t *info) {
    (void)drcontext;
    (void)info;
    return DR_SIGNAL_DELIVER;
}

static void event_exit(void) {
    if (g_buf != NULL)
        drx_buf_free(g_buf);
    drx_exit();
    drreg_exit();
    drmgr_exit();
    dr_mutex_destroy(g_lock);
}

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
    drreg_options_t drreg_ops = { sizeof(drreg_ops), 5 /*scratch slots (3 GPR + aflags + margin)*/, false };
    (void)id;
    (void)argc;
    (void)argv;
    dr_set_client_name("asm-test data-flow value client (inlined)", "");

    if (!drmgr_init() || drreg_init(&drreg_ops) != DRREG_SUCCESS || !drx_init()) {
        dr_fprintf(STDERR, "drval-inlined: extension init failed\n");
        dr_abort();
    }
    g_buf = drx_buf_create_trace_buffer(
        (size_t)sizeof(raw_step_t) * 4096, buf_flush);
    if (g_buf == NULL) {
        dr_fprintf(STDERR, "drval-inlined: drx_buf create failed\n");
        dr_abort();
    }

    g_lock = dr_mutex_create();
    resolve_all_modules();
    drmgr_register_module_load_event(event_module_load);
    drmgr_register_bb_instrumentation_event(NULL, event_insert, NULL);
    drmgr_register_signal_event(event_signal);
    drmgr_register_exit_event(event_exit);
}
