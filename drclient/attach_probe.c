/*
 * attach_probe.c — THROWAWAY external-attach go/no-go probe client for the DynamoRIO ATTACH tier
 * (dynamorio-attach-tier-plan.md, Increment 2). NOT a product artifact.
 *
 * The extension-load-probe move, for ATTACH: a MINIMAL client injected into a SEPARATE,
 * already-running native process (examples/attach_probe_victim) via DynamoRIO's EXPERIMENTAL
 * external attach (`drrun -attach <pid>` / dr_inject_process_attach). It answers the yes/no that
 * gates Increments 3-5: does DR take over an already-running native process, does the client's bb
 * instrumentation event fire over the victim's LIVE code (a non-zero instrumented-instruction
 * count PROVES the takeover reached the victim's execution), and does the victim keep running?
 *
 * It counts instrumented instructions EXECUTED (a runtime clean-call bump — the strong takeover
 * proof, since it means the victim actually ran instrumented code, not just that DR decoded it)
 * and, on exit/detach, prints `ATTACH_PROBE_TAKEOVER_OK` iff that count is non-zero. The lane
 * separately checks that the victim's heartbeat continued (survival) and that it exited cleanly.
 * Findings: docs/internal/analysis/dr-attach-probe-findings.md.
 *
 * drmgr only (no drreg/drx) — the takeover question is orthogonal to the extension stack (that
 * was the Increment-2 EXTENSION-load probe; this is the ATTACH probe).
 */
#include "dr_api.h"
#include "drmgr.h"

#include <stdint.h>
#include <string.h> /* strcmp — the `noinstr` sweep control option */

/* Instrumented instructions EXECUTED under the attached client (racy across threads, but this is
 * a go/no-go probe, not a measurement — an approximate non-zero count is all the gate needs). */
static uint64 g_exec;

static void count_one(void) { ++g_exec; }

static dr_emit_flags_t event_insert(void *dc, void *tag, instrlist_t *bb,
                                    instr_t *instr, bool for_trace,
                                    bool translating, void *user_data) {
    (void)tag;
    (void)for_trace;
    (void)translating;
    (void)user_data;
    if (!instr_is_app(instr))
        return DR_EMIT_DEFAULT;
    dr_insert_clean_call(dc, bb, instr, (void *)count_one, false, 0);
    return DR_EMIT_DEFAULT;
}

static void event_exit(void) {
    drmgr_exit();
    dr_fprintf(STDERR, "ATTACH_PROBE: exec_insns=%llu\n",
               (unsigned long long)g_exec);
    if (g_exec == 0)
        dr_fprintf(STDERR,
                   "ATTACH_PROBE: FAIL (zero instrumentation — no takeover)\n");
    else
        dr_fprintf(STDERR, "ATTACH_PROBE_TAKEOVER_OK\n");
}

DR_EXPORT void dr_client_main(client_id_t id, int argc, const char *argv[]) {
    (void)id;
    /* `noinstr` client option (managed-attach sweep control): take the process over but register
     * NO bb-instrumentation event — DR still seizes every thread + builds its code cache, but adds
     * zero clean calls. This isolates the SEIZE from the INSTRUMENTATION: if a .NET process crashes
     * under the counting client but SURVIVES noinstr, the crash is the per-instruction clean call,
     * not DR taking the runtime over; if it crashes under noinstr too, the seize itself is fatal. */
    int noinstr = 0;
    for (int i = 0; i < argc; i++)
        if (argv[i] != NULL && strcmp(argv[i], "noinstr") == 0)
            noinstr = 1;
    dr_set_client_name("asm-test DR external-attach probe", "");
    /* Reaching here at all proves the injector delivered + started the client in the running
     * victim (dr_client_main runs after DR takes the process over). */
    dr_fprintf(STDERR,
               "ATTACH_PROBE: dr_client_main reached (attach delivered the "
               "client into the running victim; noinstr=%d)\n",
               noinstr);
    if (!drmgr_init()) {
        dr_fprintf(STDERR, "ATTACH_PROBE: FAIL drmgr_init\n");
        dr_abort();
    }
    if (!noinstr &&
        !drmgr_register_bb_instrumentation_event(NULL, event_insert, NULL)) {
        dr_fprintf(STDERR, "ATTACH_PROBE: FAIL register bb event\n");
        dr_abort();
    }
    drmgr_register_exit_event(event_exit);
}
