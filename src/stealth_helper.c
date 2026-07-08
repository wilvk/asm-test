/*
 * stealth_helper.c — the shared stepping body + bundled-binary discovery for the
 * §D3 concealed ptrace-stealth stepper. Compiled BOTH into libasmtest_hwtrace (so
 * asmtest_hwtrace_stealth_trace can run it as an in-process forked child when no
 * bundled binary is present) AND into the standalone asmtest-stealth-helper binary
 * (src/stealth_helper_main.c). Linux x86-64/AArch64 only; see stealth_helper.h and
 * docs/internal/archive/plans/scoped-tracing-managed-plan.md §D3.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* dladdr / Dl_info */
#endif

#include "stealth_helper.h"

#include "asmtest_addr_channel.h" /* §D3 windowed multi-region channel */
#include "asmtest_hwtrace.h"      /* ASMTEST_HW_* */
#include "asmtest_ptrace.h" /* run_to + trace_attached[_windowed], ASMTEST_PTRACE_OK */

#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>

/* Bundled-binary discovery. Mirrors drtrace_app.c's dr_bundled_lib: an explicit
 * env override, else dladdr on one of our own symbols to find the sibling next to
 * the library/executable that carries this code. Returns an X_OK path or NULL. */
const char *asmtest_stealth_helper_path(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return NULL;
    const char *env = getenv("ASMTEST_STEALTH_HELPER");
    if (env != NULL && env[0] != '\0') {
        if (access(env, X_OK) == 0) {
            snprintf(buf, buflen, "%s", env);
            return buf;
        }
        return NULL; /* an explicit override that isn't runnable is a hard miss */
    }
    Dl_info info;
    if (dladdr((void *)asmtest_stealth_helper_path, &info) &&
        info.dli_fname != NULL) {
        const char *slash = strrchr(info.dli_fname, '/');
        if (slash != NULL) {
            snprintf(buf, buflen, "%.*s/asmtest-stealth-helper",
                     (int)(slash - info.dli_fname), info.dli_fname);
            if (access(buf, X_OK) == 0)
                return buf;
        }
    }
    return NULL;
}
#else
const char *asmtest_stealth_helper_path(char *buf, size_t buflen) {
    (void)buf;
    (void)buflen;
    return NULL;
}
#endif

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

int asmtest_stealth_helper_run(asmtest_stealth_scratch_t *sc, pid_t parent,
                               const void *base, size_t len) {
    if (sc == NULL)
        return ASMTEST_HW_EINVAL;
    /* Recompute the shadow buffer pointers in THIS address space (the caller's are
     * meaningless once we are a separate exec'd process mapping the same memfd). */
    uint64_t *ibuf = (uint64_t *)((char *)sc + sizeof(*sc));
    uint64_t *bbuf = ibuf + sc->icap;
    sc->shadow.insns = ibuf;
    sc->shadow.insns_cap = sc->icap;
    sc->shadow.insns_len = 0;
    sc->shadow.insns_total = 0;
    sc->shadow.blocks = bbuf;
    sc->shadow.blocks_cap = sc->bcap;
    sc->shadow.blocks_len = 0;
    sc->shadow.blocks_total = 0;
    sc->shadow.truncated = 0;

    /* Watchdog: a stuck attach/step self-truncates instead of hanging the caller. */
    alarm(15);
    if (ptrace(PTRACE_SEIZE, parent, (void *)0, (void *)0) != 0 ||
        ptrace(PTRACE_INTERRUPT, parent, (void *)0, (void *)0) != 0) {
        sc->rc = ASMTEST_HW_EUNAVAIL;
        sc->ready = 1;
        return sc->rc;
    }
    int st = 0;
    if (waitpid(parent, &st, 0) < 0 || !WIFSTOPPED(st)) {
        sc->rc = ASMTEST_HW_EUNAVAIL;
        sc->ready = 1;
        return sc->rc;
    }
    /* The caller is now INTERRUPT-stopped; publishing `ready` + run_to's CONT is
     * what releases it into run_region, so the region cannot run untraced. */
    sc->ready = 1;
    if (asmtest_ptrace_run_to(parent, base) != ASMTEST_PTRACE_OK) {
        ptrace(PTRACE_DETACH, parent, (void *)0, (void *)0);
        sc->rc = ASMTEST_HW_EUNAVAIL;
        return sc->rc;
    }
    long res = 0;
    int tr =
        asmtest_ptrace_trace_attached(parent, base, len, &res, &sc->shadow);
    ptrace(PTRACE_DETACH, parent, (void *)0, (void *)0);
    sc->result = res;
    sc->rc = (tr == ASMTEST_PTRACE_OK) ? ASMTEST_HW_OK : ASMTEST_HW_EDECODE;
    return sc->rc;
}

int asmtest_stealth_helper_run_windowed(asmtest_stealth_scratch_t *sc,
                                        pid_t parent, const void *win_base,
                                        size_t win_len) {
    if (sc == NULL)
        return ASMTEST_HW_EINVAL;
    /* Recompute the shadow buffer pointers AND the channel in THIS address space. */
    uint64_t *ibuf = (uint64_t *)((char *)sc + sizeof(*sc));
    uint64_t *bbuf = ibuf + sc->icap;
    sc->shadow.insns = ibuf;
    sc->shadow.insns_cap = sc->icap;
    sc->shadow.insns_len = 0;
    sc->shadow.insns_total = 0;
    sc->shadow.blocks = bbuf;
    sc->shadow.blocks_cap = sc->bcap;
    sc->shadow.blocks_len = 0;
    sc->shadow.blocks_total = 0;
    sc->shadow.truncated = 0;
    /* The SHARED channel pointer is fork-valid (identical address space), so use it
     * directly — the runtime publishes into it live while we drain. */
    asmtest_addr_channel_t *chan = (asmtest_addr_channel_t *)sc->win_chan;

    alarm(15);
    if (ptrace(PTRACE_SEIZE, parent, (void *)0, (void *)0) != 0 ||
        ptrace(PTRACE_INTERRUPT, parent, (void *)0, (void *)0) != 0) {
        sc->rc = ASMTEST_HW_EUNAVAIL;
        sc->ready = 1;
        return sc->rc;
    }
    int st = 0;
    if (waitpid(parent, &st, 0) < 0 || !WIFSTOPPED(st)) {
        sc->rc = ASMTEST_HW_EUNAVAIL;
        sc->ready = 1;
        return sc->rc;
    }
    sc->ready = 1;
    if (asmtest_ptrace_run_to(parent, win_base) != ASMTEST_PTRACE_OK) {
        ptrace(PTRACE_DETACH, parent, (void *)0, (void *)0);
        sc->rc = ASMTEST_HW_EUNAVAIL;
        return sc->rc;
    }
    long res = 0;
    int tr = asmtest_ptrace_trace_attached_windowed(parent, win_base, win_len,
                                                    chan, &res, &sc->shadow);
    ptrace(PTRACE_DETACH, parent, (void *)0, (void *)0);
    sc->result = res;
    sc->rc = (tr == ASMTEST_PTRACE_OK) ? ASMTEST_HW_OK : ASMTEST_HW_EDECODE;
    return sc->rc;
}
#else
int asmtest_stealth_helper_run(asmtest_stealth_scratch_t *sc, pid_t parent,
                               const void *base, size_t len) {
    (void)sc;
    (void)parent;
    (void)base;
    (void)len;
    return ASMTEST_HW_ENOSYS;
}
int asmtest_stealth_helper_run_windowed(asmtest_stealth_scratch_t *sc,
                                        pid_t parent, const void *win_base,
                                        size_t win_len) {
    (void)sc;
    (void)parent;
    (void)win_base;
    (void)win_len;
    return ASMTEST_HW_ENOSYS;
}
#endif
