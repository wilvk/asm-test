/*
 * probe_extensions.c — a THROWAWAY DynamoRIO extension-load probe for the
 * data-flow taint tier (dynamorio-taint-tier-plan.md, Increment 2).
 *
 * WHY THIS EXISTS. The shipped DR clients (src/drtrace_client.c and
 * src/dataflow_dr_client.c) use ONLY DynamoRIO's raw BSD core API because the
 * prebuilt release EXTENSIONS were believed to fail to load under DR's private
 * loader on modern glibc — a blocker recorded generically ("modern glibc") in
 * drclient/CMakeLists.txt and dataflow_dr_client.c with no tested version
 * boundary and no attempted fix. The whole Phase-5 taint re-platform
 * (drmgr/drreg/drx-buf inlined instrumentation) is gated on that blocker being
 * turned into an empirical yes/no. This probe is that test: it links the
 * extension stack, calls one real API from each, instruments a trivial workload
 * run under `drrun`, and prints a load-success line per extension plus a
 * non-zero instrumented-instruction count. It is NOT part of the product — it
 * is built and run only in the throwaway `make docker-drext-probe` lane and
 * never linked into a shipped artifact.
 *
 * LICENSE SPLIT (discovered by this increment — see
 * docs/internal/analysis/dr-extension-load-probe-findings.md):
 *   - drmgr, drreg, drx (which is where the drx_buf trace-buffer API lives —
 *     there is NO separate drx_buf.h; it is folded into drx.h) are DynamoRIO
 *     CORE extensions under ext/, covered by DR's primary BSD license. The
 *     re-platformed taint tier can adopt these and stay LGPL-clean.
 *   - umbra is NOT a core ext/ extension: it ships under drmemory/drmf/ as part
 *     of the Dr. Memory Framework, whose primary license is LGPL-2.1 (only its
 *     drfuzz/drltrace modules are BSD carve-outs; umbra is not). umbra.h itself
 *     carries the LGPL-2.1 header. So umbra is LGPL, exactly like drwrap — the
 *     one extension the tier deliberately avoids.
 *
 * Because of that, the DEFAULT probe covers only the BSD-clean stack
 * (drmgr + drreg + drx/drx_buf); the umbra load-check is compiled in only under
 * -DPROBE_UMBRA (cmake option PROBE_UMBRA=ON), is informational, and is not part
 * of the committed CI gate — its purpose is to record whether the DrMemoryFramework
 * private-loader path works at all, not to bless umbra for adoption.
 */
#include "dr_api.h"
#include "drmgr.h"
#include "drreg.h"
#include "drx.h"
#ifdef PROBE_UMBRA
#    include "umbra.h"
#endif

/* Instrumented-instruction count (proves the bb instrumentation event actually
 * fired, i.e. drmgr's phased pass ordering loaded and ran). Bumped via a clean
 * call so no drreg scratch is required just to count. */
static uint64 g_instr_count;

/* The drx_buf trace buffer — created at init purely to force libdrx.so's
 * drx_buf_* symbols to resolve under the private loader (the load-check). */
static drx_buf_t *g_buf;

static void
count_one(void)
{
    /* Racy across threads, but this is a load probe, not a measurement — an
     * approximate non-zero count is all the exit criterion needs. */
    ++g_instr_count;
}

/* drx_buf full callback (unused here — the buffer is only created to load-check
 * the drx_buf API, never filled). */
static void
buf_full_cb(void *drcontext, void *buf_base, size_t size)
{
    (void)drcontext;
    (void)buf_base;
    (void)size;
}

/* drmgr insertion-phase callback: exercise drreg (reserve+unreserve a scratch
 * GPR, which forces drreg's slot machinery to run) and count the instruction. */
static dr_emit_flags_t
event_insert(void *drcontext, void *tag, instrlist_t *bb, instr_t *instr,
             bool for_trace, bool translating, void *user_data)
{
    reg_id_t scratch;
    (void)tag;
    (void)for_trace;
    (void)translating;
    (void)user_data;

    if (!instr_is_app(instr))
        return DR_EMIT_DEFAULT;

    /* drreg load-check: reserve a scratch register and immediately release it.
     * A reserve/unreserve with no use in between is a no-op on the app but still
     * drives drreg's reservation logic, proving libdrreg.so loaded and works. */
    if (drreg_reserve_register(drcontext, bb, instr, NULL, &scratch) == DRREG_SUCCESS)
        drreg_unreserve_register(drcontext, bb, instr, scratch);

    dr_insert_clean_call(drcontext, bb, instr, (void *)count_one, false, 0);
    return DR_EMIT_DEFAULT;
}

static void
event_exit(void)
{
    if (g_buf != NULL)
        drx_buf_free(g_buf);
#ifdef PROBE_UMBRA
    umbra_exit();
#endif
    drx_exit();
    drreg_exit();
    drmgr_exit();

    dr_fprintf(STDERR, "drext-probe: instrumented %llu instructions\n",
               (unsigned long long)g_instr_count);
    if (g_instr_count == 0) {
        dr_fprintf(STDERR, "drext-probe: FAIL (zero instructions instrumented)\n");
        dr_abort();
    }
#ifdef PROBE_UMBRA
    dr_fprintf(STDERR, "drext-probe: PROBE OK (drmgr+drreg+drx+umbra)\n");
#else
    dr_fprintf(STDERR, "drext-probe: PROBE OK (drmgr+drreg+drx)\n");
#endif
}

DR_EXPORT void
dr_client_main(client_id_t id, int argc, const char *argv[])
{
    drreg_options_t drreg_ops = { sizeof(drreg_ops), 2 /*max scratch slots*/, false };
    (void)argc;
    (void)argv;

    dr_set_client_name("asm-test DR extension-load probe", "");

    /* Each _init() below only returns success if its extension .so resolved and
     * initialized under DR's private loader — which is precisely the blocker
     * this probe exists to characterize. A failed load aborts the whole client
     * before dr_client_main even runs, so reaching each line at all is the load
     * proof; the explicit checks catch a soft init failure. */
    if (!drmgr_init()) {
        dr_fprintf(STDERR, "drext-probe: FAIL drmgr_init\n");
        dr_abort();
    }
    dr_fprintf(STDERR, "drext-probe: drmgr loaded\n");

    if (drreg_init(&drreg_ops) != DRREG_SUCCESS) {
        dr_fprintf(STDERR, "drext-probe: FAIL drreg_init\n");
        dr_abort();
    }
    dr_fprintf(STDERR, "drext-probe: drreg loaded\n");

    if (!drx_init()) {
        dr_fprintf(STDERR, "drext-probe: FAIL drx_init\n");
        dr_abort();
    }
    dr_fprintf(STDERR, "drext-probe: drx loaded\n");

    g_buf = drx_buf_create_trace_buffer(1024, buf_full_cb);
    if (g_buf == NULL) {
        dr_fprintf(STDERR, "drext-probe: FAIL drx_buf_create_trace_buffer\n");
        dr_abort();
    }
    dr_fprintf(STDERR, "drext-probe: drx_buf trace-buffer created\n");

#ifdef PROBE_UMBRA
    /* Informational only — umbra is LGPL-2.1 (Dr. Memory Framework); this checks
     * whether the DRMF private-loader path works, it does NOT bless umbra for the
     * license-clean tier. umbra_init() returning DRMF_SUCCESS is the load proof:
     * it is a call into libumbra.so, so the .so must have resolved under the
     * private loader to reach it. (Creating a shadow MAPPING is a separate usage
     * concern, out of scope for a load probe.) */
    if (umbra_init(id) != DRMF_SUCCESS) {
        dr_fprintf(STDERR, "drext-probe: FAIL umbra_init\n");
        dr_abort();
    }
    dr_fprintf(STDERR, "drext-probe: umbra loaded (LGPL-2.1 — informational)\n");
#else
    (void)id;
#endif

    if (!drmgr_register_bb_instrumentation_event(NULL, event_insert, NULL)) {
        dr_fprintf(STDERR, "drext-probe: FAIL drmgr_register_bb_instrumentation_event\n");
        dr_abort();
    }
    /* drmgr owns the exit event (it poisons dr_register_exit_event to force
     * ordering through its own registration), so register through drmgr. */
    drmgr_register_exit_event(event_exit);
}
