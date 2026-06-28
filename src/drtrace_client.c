/*
 * drtrace_client.c — the in-process DynamoRIO client (libasmtest_drclient.so) for
 * the native-trace tier. See include/asmtest_drtrace.h and docs/native-tracing.md.
 *
 * Built with DynamoRIO's CMake flow (find_package(DynamoRIO) +
 * configure_DynamoRIO_client) into a separate .so the libasmtest core and the
 * libasmtest_emu superset never link. It uses ONLY DynamoRIO's BSD core API
 * (dr_register_bb_event, clean calls, dr_get_proc_address) — deliberately not
 * drmgr/drwrap/drreg: the prebuilt release extensions fail to load under DR's
 * private loader on modern glibc, and dropping drwrap also drops its LGPL-2.1
 * obligation, so this tier is license-clean (DR core is BSD).
 *
 * It observes the app-side markers (resolved by exported-symbol PC) and inserts
 * clean calls that:
 *   - read the register marker's SysV argument registers to learn a range
 *     [base, base+len) and its app-owned asmtest_trace_t;
 *   - toggle per-thread recording on begin/end;
 *   - record block (and optional instruction) offsets into the app trace.
 *
 * The asmtest_trace_t layout is mirrored locally as at_trace_t: asmtest_trace.h
 * pulls in <stdbool.h>, whose `bool` clashes with DynamoRIO's own `bool` (char).
 * The mirror MUST match include/asmtest_trace.h's struct asmtest_trace.
 */
#include "dr_api.h"

#include <stdint.h>
#include <string.h>

/* Mirror of struct asmtest_trace (include/asmtest_trace.h). Keep in sync. */
typedef struct {
    uint64_t *insns;
    size_t insns_cap;
    size_t insns_len;
    uint64_t insns_total;
    uint64_t *blocks;
    size_t blocks_cap;
    size_t blocks_len;
    uint64_t blocks_total;
    unsigned char truncated; /* bool */
} at_trace_t;

/* ------------------------------------------------------------------ */
/* Client-local region registry (never shares app globals)             */
/* ------------------------------------------------------------------ */
#define MAX_REGIONS 32
#define MAX_NAME 64
typedef struct {
    char name[MAX_NAME]; /* fixed buffer: DR's public API has no string-dup */
    bool used;
    app_pc base;
    size_t len;
    at_trace_t *trace;
    bool insns; /* instruction mode for this region (trace->insns_cap > 0) */
} region_t;
static region_t g_regions[MAX_REGIONS];
static int g_nregions = 0;
static void *g_region_lock; /* dr_mutex */

/* Resolved marker PCs (NULL until the drapp module is seen). */
static app_pc pc_register, pc_unregister, pc_begin, pc_end;

/* Per-thread recording state: a small stack of active trace pointers. */
#define MAX_DEPTH 16
typedef struct {
    at_trace_t *stack[MAX_DEPTH];
    int depth;
} thread_state_t;

static region_t *region_for_pc(app_pc pc) {
    for (int i = 0; i < g_nregions; i++)
        if (g_regions[i].used && pc >= g_regions[i].base &&
            pc < g_regions[i].base + g_regions[i].len)
            return &g_regions[i];
    return NULL;
}

static bool thread_recording(thread_state_t *ts, at_trace_t *tr) {
    if (ts == NULL)
        return false;
    for (int i = 0; i < ts->depth; i++)
        if (ts->stack[i] == tr)
            return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Clean-call sinks (run in the app address space; may touch app memory) */
/* ------------------------------------------------------------------ */

static void on_register(app_pc name, app_pc base, size_t len, at_trace_t *tr) {
    const char *nm = (const char *)name;
    dr_mutex_lock(g_region_lock);
    /* Replace an existing same-name entry, else append. */
    int slot = -1;
    for (int i = 0; i < g_nregions; i++)
        if (g_regions[i].used && strcmp(g_regions[i].name, nm) == 0) {
            slot = i;
            break;
        }
    if (slot < 0 && g_nregions < MAX_REGIONS)
        slot = g_nregions++;
    if (slot >= 0) {
        dr_snprintf(g_regions[slot].name, MAX_NAME, "%s", nm);
        g_regions[slot].name[MAX_NAME - 1] = '\0';
        g_regions[slot].used = true;
        g_regions[slot].base = base;
        g_regions[slot].len = len;
        g_regions[slot].trace = tr;
        g_regions[slot].insns = (tr != NULL && tr->insns_cap > 0);
    }
    dr_mutex_unlock(g_region_lock);
    /* Re-instrument the range so any already-cached translation picks up our
     * recording instrumentation. dr_delay_flush_region is the clean-call-safe
     * flush (dr_flush_region requires not returning to the cache, which would
     * hang an ordinary clean call); for freshly mmap'd code that has never
     * executed this is simply a no-op. */
    dr_delay_flush_region(base, len, 0, NULL);
}

static void on_unregister(app_pc name) {
    const char *nm = (const char *)name;
    dr_mutex_lock(g_region_lock);
    for (int i = 0; i < g_nregions; i++)
        if (g_regions[i].used && strcmp(g_regions[i].name, nm) == 0) {
            app_pc base = g_regions[i].base;
            size_t len = g_regions[i].len;
            g_regions[i] = g_regions[--g_nregions];
            dr_mutex_unlock(g_region_lock);
            /* Drop cached translation (clean-call-safe variant) before the
             * caller may unmap/reuse the range. */
            dr_delay_flush_region(base, len, 0, NULL);
            return;
        }
    dr_mutex_unlock(g_region_lock);
}

static void on_begin(app_pc name) {
    void *dc = dr_get_current_drcontext();
    thread_state_t *ts = (thread_state_t *)dr_get_tls_field(dc);
    if (ts == NULL)
        return;
    at_trace_t *tr = NULL;
    const char *nm = (const char *)name;
    dr_mutex_lock(g_region_lock);
    for (int i = 0; i < g_nregions; i++)
        if (g_regions[i].used && strcmp(g_regions[i].name, nm) == 0) {
            tr = g_regions[i].trace;
            break;
        }
    dr_mutex_unlock(g_region_lock);
    if (tr != NULL && ts->depth < MAX_DEPTH)
        ts->stack[ts->depth++] = tr;
}

static void on_end(app_pc name) {
    (void)name;
    void *dc = dr_get_current_drcontext();
    thread_state_t *ts = (thread_state_t *)dr_get_tls_field(dc);
    if (ts != NULL && ts->depth > 0)
        ts->depth--;
}

static void on_block(at_trace_t *tr, uint64_t off) {
    void *dc = dr_get_current_drcontext();
    thread_state_t *ts = (thread_state_t *)dr_get_tls_field(dc);
    if (!thread_recording(ts, tr))
        return;
    tr->blocks_total++;
    if (tr->blocks != NULL) {
        for (size_t i = 0; i < tr->blocks_len; i++)
            if (tr->blocks[i] == off)
                return;
        if (tr->blocks_len < tr->blocks_cap)
            tr->blocks[tr->blocks_len++] = off;
        else
            tr->truncated = 1;
    }
}

static void on_insn(at_trace_t *tr, uint64_t off) {
    void *dc = dr_get_current_drcontext();
    thread_state_t *ts = (thread_state_t *)dr_get_tls_field(dc);
    if (!thread_recording(ts, tr))
        return;
    if (tr->insns != NULL) {
        if (tr->insns_len < tr->insns_cap)
            tr->insns[tr->insns_len++] = off;
        else
            tr->truncated = 1;
    }
    tr->insns_total++;
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

    if (pc == pc_register) {
        dr_insert_clean_call(drcontext, bb, first, (void *)on_register, false, 4,
                             opnd_create_reg(DR_REG_RDI),
                             opnd_create_reg(DR_REG_RSI),
                             opnd_create_reg(DR_REG_RDX),
                             opnd_create_reg(DR_REG_RCX));
        return DR_EMIT_DEFAULT;
    }
    if (pc == pc_unregister) {
        dr_insert_clean_call(drcontext, bb, first, (void *)on_unregister, false,
                             1, opnd_create_reg(DR_REG_RDI));
        return DR_EMIT_DEFAULT;
    }
    if (pc == pc_begin) {
        dr_insert_clean_call(drcontext, bb, first, (void *)on_begin, false, 1,
                             opnd_create_reg(DR_REG_RDI));
        return DR_EMIT_DEFAULT;
    }
    if (pc == pc_end) {
        dr_insert_clean_call(drcontext, bb, first, (void *)on_end, false, 1,
                             opnd_create_reg(DR_REG_RDI));
        return DR_EMIT_DEFAULT;
    }

    region_t *r = region_for_pc(pc);
    if (r != NULL) {
        dr_insert_clean_call(drcontext, bb, first, (void *)on_block, false, 2,
                             OPND_CREATE_INTPTR(r->trace),
                             OPND_CREATE_INT64((uint64_t)(pc - r->base)));
        if (r->insns) {
            for (instr_t *in = first; in != NULL; in = instr_get_next(in)) {
                app_pc ipc = instr_get_app_pc(in);
                if (ipc == NULL || ipc < r->base || ipc >= r->base + r->len)
                    continue;
                dr_insert_clean_call(
                    drcontext, bb, in, (void *)on_insn, false, 2,
                    OPND_CREATE_INTPTR(r->trace),
                    OPND_CREATE_INT64((uint64_t)(ipc - r->base)));
            }
        }
    }
    return DR_EMIT_DEFAULT;
}

/* ------------------------------------------------------------------ */
/* Marker resolution + lifecycle                                       */
/* ------------------------------------------------------------------ */

static void try_resolve(module_handle_t h) {
    if (pc_register == NULL)
        pc_register =
            (app_pc)dr_get_proc_address(h, "asmtest_dr_register_region_marker");
    if (pc_unregister == NULL)
        pc_unregister = (app_pc)dr_get_proc_address(
            h, "asmtest_dr_unregister_region_marker");
    if (pc_begin == NULL)
        pc_begin = (app_pc)dr_get_proc_address(h, "asmtest_trace_begin");
    if (pc_end == NULL)
        pc_end = (app_pc)dr_get_proc_address(h, "asmtest_trace_end");
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

static void event_thread_init(void *drcontext) {
    thread_state_t *ts = (thread_state_t *)dr_thread_alloc(drcontext,
                                                           sizeof(thread_state_t));
    memset(ts, 0, sizeof *ts);
    dr_set_tls_field(drcontext, ts);
}

static void event_thread_exit(void *drcontext) {
    thread_state_t *ts = (thread_state_t *)dr_get_tls_field(drcontext);
    if (ts != NULL)
        dr_thread_free(drcontext, ts, sizeof(thread_state_t));
}

static void event_exit(void) { dr_mutex_destroy(g_region_lock); }

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
    (void)id;
    (void)argc;
    (void)argv;
    dr_set_client_name("asm-test native trace client", "");
    g_region_lock = dr_mutex_create();
    resolve_all_modules();
    dr_register_module_load_event(event_module_load);
    dr_register_thread_init_event(event_thread_init);
    dr_register_thread_exit_event(event_thread_exit);
    dr_register_bb_event(event_bb);
    dr_register_exit_event(event_exit);
}
