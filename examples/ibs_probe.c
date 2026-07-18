/*
 * ibs_probe.c — AMD IBS-Op capability probe (Zen 2 IBS lane, Phase 0).
 *
 * Prints whether this host can sample statistical from->to control-flow edges via
 * AMD IBS-Op, and self-skips (a `# SKIP` line, exit 0) with a precise reason where
 * it cannot — the common case off AMD Zen (and always on non-AMD / VM / CI). Every
 * downstream IBS example/test gates on the same asmtest_ibs_available() chain.
 */
#include "asmtest_ibs.h"

#include "ibs_backend.h" /* internal IBS-Fetch front-end probe (Phase 7) */

#include <limits.h>
#include <stdio.h>

/* The live kernel.perf_event_paranoid level, or INT_MIN where the node is absent
 * (non-Linux / masked /proc). ibs_probe links only ibs_backend, not libasmtest_hwtrace,
 * so it reads /proc directly rather than calling asmtest_hwtrace_perf_event_paranoid(). */
static int perf_event_paranoid(void) {
    FILE *f = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    if (f == NULL)
        return INT_MIN;
    int v = INT_MIN;
    if (fscanf(f, "%d", &v) != 1)
        v = INT_MIN;
    fclose(f);
    return v;
}

/* Report the front-end IBS-Fetch lane (Phase 7): an independent substrate (its own
 * FetchSam cap + ibs_fetch PMU), so probe and print it separately from the Op lane. */
static void report_fetch(void) {
    printf("#\n");
    if (!asmtest_ibs_fetch_available()) {
        printf("# IBS-Fetch front-end coverage: unavailable — %s\n",
               asmtest_ibs_fetch_skip_reason());
        return;
    }
    /* Substrate present is NOT "sampling can open": attempt a real fetch capture on
     * this thread before claiming AVAILABLE. asmtest_ibs_unavail_reason() carries the
     * real open errno (ibs_chan_open records g_open_errno for every lane). */
    asmtest_ibs_fetch_survey_t fs;
    int rc = asmtest_ibs_survey_fetch_pid(0, 50, NULL, &fs);
    asmtest_ibs_fetch_survey_free(&fs);
    if (rc != ASMTEST_IBS_OK) {
        printf("# IBS-Fetch front-end coverage: substrate present but sampling "
               "is BLOCKED — %s\n",
               asmtest_ibs_unavail_reason());
        return;
    }
    printf("# IBS-Fetch front-end coverage: AVAILABLE\n");
    printf("#   ibs_fetch PMU present; FetchSam capable; swfilt user-only "
           "sampling\n");
    printf("#   asmtest_ibs_survey_fetch_pid(tid, ms, NULL, &fsurvey);\n");
    printf("# Samples FETCH addresses (i-cache / ITLB miss, fetch latency) — "
           "the\n");
    printf("# front-end coverage the retire-side Op sampler can miss.\n");
}

int main(void) {
    printf("# asm-test AMD IBS-Op capability probe\n");
    if (!asmtest_ibs_available()) {
        printf("# SKIP ibs_probe: %s\n", asmtest_ibs_skip_reason());
        report_fetch();
        return 0;
    }
    /* The CPUID/sysfs/swfilt substrate is present — but that is NOT "sampling can
     * open". On a locked-down host (perf_event_paranoid / seccomp) the real
     * perf_event_open still fails, so attempt a short real capture on this thread
     * before claiming AVAILABLE: the open/enable is what is under test, not sample
     * density. (Measured false on a paranoid-locked box — asmspy-plan.md Theme E.) */
    asmtest_ibs_survey_t s;
    int rc = asmtest_ibs_survey_pid(0, 50, NULL, &s);
    asmtest_ibs_survey_free(&s);
    if (rc != ASMTEST_IBS_OK) {
        printf("# IBS-Op: substrate present but sampling is BLOCKED — %s\n",
               asmtest_ibs_unavail_reason());
        printf("# SKIP ibs_probe: IBS-Op sampling not openable on this host "
               "(substrate present)\n");
        report_fetch();
        return 0; /* a blocked host is a skip case, not a failure */
    }
    printf("# IBS-Op statistical edge sampling: AVAILABLE\n");
    printf("#   AMD IBS-Op PMU present; OpSam + BrnTrgt capable\n");
    /* Report the ACTUAL paranoid level, not a static "paranoid=2": we only reach
     * here after a real open SUCCEEDED, and IBS-Op's kernel swfilt can permit
     * user-only sampling even on a paranoid-raised box, so a hardcoded
     * "unprivileged (paranoid=2)" would misreport the machine it ran on. */
    int paranoid = perf_event_paranoid();
    if (paranoid == INT_MIN)
        printf("#   kernel swfilt present -> user-only sampling "
               "(perf_event_paranoid unreadable)\n");
    else
        printf("#   kernel swfilt present -> user-only sampling "
               "(perf_event_paranoid=%d)\n",
               paranoid);
    printf("#\n");
    printf("# A live, out-of-band from->to edge survey is buildable on this "
           "host:\n");
    printf("#   asmtest_ibs_survey_pid(tid, ms, NULL, &survey);\n");
    printf("# It observes a running target without single-stepping it — the "
           "safe\n");
    printf("# control-flow view for JIT / managed runtimes.\n");
    report_fetch();
    return 0;
}
