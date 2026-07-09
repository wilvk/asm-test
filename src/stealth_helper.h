/*
 * stealth_helper.h — internal contract shared between the §D3 concealed
 * ptrace-stealth stepper (asmtest_hwtrace_stealth_trace, src/hwtrace.c) and the
 * bundled standalone helper binary (src/stealth_helper_main.c). NOT a public
 * header — it ships no symbols in include/ and carries no ABI promise.
 *
 * The stepping body is identical whether it runs as an in-process forked child
 * (the fallback when no bundled binary is discoverable) or as an exec'd separate
 * binary mapping the same memfd (the bundled path). Both drive the caller through
 * the reverse-attach protocol (PR_SET_PTRACER + PTRACE_SEIZE) and single-step the
 * region out of band. See docs/internal/archive/plans/scoped-tracing-managed-plan.md §D3.
 */
#ifndef ASMTEST_STEALTH_HELPER_H
#define ASMTEST_STEALTH_HELPER_H

#include <stddef.h>
#include <sys/types.h> /* pid_t */

#include "asmtest_trace.h"

/* Shared scratch: the caller fills the header + region info, the stepping side
 * (forked child OR exec'd helper) writes the reconstructed trace back into
 * `shadow`. The insns[icap]/blocks[bcap] buffers follow this struct in the same
 * mapping; each side recomputes those pointers in its OWN address space from
 * icap/bcap — the caller's shadow.insns pointer is meaningless once the helper is
 * a separate exec'd process mapping the memfd at a different base. Only the scalar
 * fields (ready, rc, result and shadow's len/total/truncated counts) are read
 * across the process boundary; the buffer contents are the same shared pages. */
typedef struct {
    volatile int
        ready; /* stepper -> caller: seized + about to plant the run_to bp */
    volatile int
        rc;      /* stepper's outcome (ASMTEST_HW_*)                          */
    volatile int
        stop;    /* caller -> helper: end the async window at the next step   */
    volatile int
        done;    /* helper -> caller: detached + shadow lens final            */
    long result; /* the region's return value                                */
    size_t icap; /* insns buffer capacity (to recompute the pointer)         */
    size_t bcap; /* blocks buffer capacity                                   */
    int win;     /* 1 = windowed (multi-region) whole-window capture         */
    size_t chan_off; /* byte offset from sc to the addr channel (windowed)   */
    void *win_chan;  /* windowed: the SHARED addr channel pointer (fork-valid,
                        the runtime publishes into it live; the helper drains) */
    asmtest_trace_t shadow;
    /* uint64_t insns[icap]; uint64_t blocks[bcap]; [asmtest_addr_channel_t] follow here */
} asmtest_stealth_scratch_t;

/* The stepping body, shared by the in-process fallback and the standalone binary.
 * SEIZE+INTERRUPT `parent`, publish `ready`, run_to `base`, then single-step
 * [base, base+len) into sc->shadow (recomputing the buffer pointers from
 * sc->icap/sc->bcap in this address space). Sets sc->rc / sc->result and returns
 * the ASMTEST_HW_* status. An alarm() watchdog inside guarantees it never hangs. */
int asmtest_stealth_helper_run(asmtest_stealth_scratch_t *sc, pid_t parent,
                               const void *base, size_t len);

/* §D3 WHOLE-WINDOW stepping body: like asmtest_stealth_helper_run, but after run_to
 * `win_base` it single-steps the WHOLE window via asmtest_ptrace_trace_attached_windowed
 * — recording the absolute address of every instruction in [win_base, win_base+win_len)
 * OR any region the caller pre-published on the channel at sc->chan_off (recomputed in
 * this address space). The out-of-band analog of the in-process whole-window scope. */
int asmtest_stealth_helper_run_windowed(asmtest_stealth_scratch_t *sc,
                                        pid_t parent, const void *win_base,
                                        size_t win_len);
int asmtest_stealth_helper_run_window_async(asmtest_stealth_scratch_t *sc,
                                            pid_t parent);

/* Locate the bundled `asmtest-stealth-helper` binary: the ASMTEST_STEALTH_HELPER
 * env override first (an explicit path), else a dladdr-sibling lookup next to the
 * library that carries this code (mirrors drtrace_app.c's dr_bundled_lib for the
 * DynamoRIO payload). Writes an X_OK-executable path into buf and returns it, or
 * NULL when none is found — the caller then falls back to the in-process fork. */
const char *asmtest_stealth_helper_path(char *buf, size_t buflen);

#endif /* ASMTEST_STEALTH_HELPER_H */
