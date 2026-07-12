/*
 * ibs_probe.c — AMD IBS-Op capability probe (Zen 2 IBS lane, Phase 0).
 *
 * Prints whether this host can sample statistical from->to control-flow edges via
 * AMD IBS-Op, and self-skips (a `# SKIP` line, exit 0) with a precise reason where
 * it cannot — the common case off AMD Zen (and always on non-AMD / VM / CI). Every
 * downstream IBS example/test gates on the same asmtest_ibs_available() chain.
 */
#include "asmtest_ibs.h"

#include <stdio.h>

int main(void) {
    printf("# asm-test AMD IBS-Op capability probe\n");
    if (!asmtest_ibs_available()) {
        printf("# SKIP ibs_probe: %s\n", asmtest_ibs_skip_reason());
        return 0;
    }
    printf("# IBS-Op statistical edge sampling: AVAILABLE\n");
    printf("#   AMD IBS-Op PMU present; OpSam + BrnTrgt capable\n");
    printf("#   kernel swfilt present -> user-only sampling, unprivileged "
           "(paranoid=2)\n");
    printf("#\n");
    printf("# A live, out-of-band from->to edge survey is buildable on this "
           "host:\n");
    printf("#   asmtest_ibs_survey_pid(tid, ms, NULL, &survey);\n");
    printf("# It observes a running target without single-stepping it — the "
           "safe\n");
    printf("# control-flow view for JIT / managed runtimes.\n");
    return 0;
}
