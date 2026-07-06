/*
 * test_branchsnap.c — AMD-P0 deterministic LBR snapshot: the capture PRODUCES A CORRECT
 * in-region trace, not just "some entries." Drives the public
 * asmtest_amd_snapshot_trace() (src/branchsnap.c): enable LBR, HW-breakpoint the region
 * exit, snapshot the frozen stack via bpf_get_branch_snapshot(), decode via the shared
 * amd_decode. Asserts the tiny single-shot routine's ENTRY BLOCK is captured — the exact
 * case the sample_period=1 path truncates. Self-skips (exit 0) without the BPF toolchain /
 * CAP_BPF / AMD LbrExtV2, so it never fails a lane that cannot run it.
 */
#define _GNU_SOURCE

#include "asmtest_hwtrace.h"
#include "asmtest_trace.h"

#include <stdio.h>
#include <string.h>

#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#include <unistd.h>

int asmtest_amd_snapshot_available(void);

/* mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (jle 0xc -> 0x11 taken). */
static const unsigned char ROUTINE[] = {0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0,
                                        0x48, 0x3d, 0x64, 0x00, 0x00, 0x00,
                                        0x7e, 0x03, 0x48, 0xff, 0xc8, 0xc3};
#define RET_OFF 0x11

typedef long (*add2_fn)(long, long);
static long g_result;

static void run_routine(void *arg) {
    add2_fn fn = (add2_fn)arg;
    g_result = fn(20, 22);
}

int main(void) {
    printf("== branchsnap-test (AMD deterministic LBR snapshot) ==\n");
    if (!asmtest_amd_snapshot_available()) {
        printf("# SKIP branchsnap: substrate absent (needs AMD LbrExtV2 + perfmon_v2 + "
               "kernel >= 6.10)\n");
        return 0;
    }

    void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP branchsnap: mmap failed\n");
        return 0;
    }
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    int rc = asmtest_amd_snapshot_trace(p, sizeof ROUTINE, RET_OFF, run_routine, p, tr);

    if (rc == ASMTEST_HW_ENOSYS) {
        printf("# SKIP branchsnap: built without the BPF toolchain\n");
    } else if (rc == ASMTEST_HW_EUNAVAIL) {
        printf("# SKIP branchsnap: capture unavailable (need CAP_BPF + CAP_PERFMON)\n");
    } else if (rc != ASMTEST_HW_OK) {
        printf("not ok - branchsnap: asmtest_amd_snapshot_trace rc=%d\n", rc);
        return 1;
    } else {
        int covered0 = asmtest_trace_covered(tr, 0);
        unsigned long long ni = asmtest_emu_trace_insns_total(tr);
        printf("branchsnap: add2(20,22)=%ld; deterministic snapshot decoded %llu in-region "
               "instructions, entry-block covered=%d, truncated=%d\n",
               (long)g_result, ni, covered0, asmtest_emu_trace_truncated(tr));
        if (covered0 && ni > 0)
            printf("ok - branchsnap: the boundary snapshot captured the tiny single-shot "
                   "routine (entry block reconstructed) where sampling truncates\n");
        else
            printf("not ok - branchsnap: snapshot decoded no in-region entry block\n");
        if (!(covered0 && ni > 0)) {
            asmtest_trace_free(tr);
            munmap(p, sizeof ROUTINE);
            return 1;
        }
    }
    asmtest_trace_free(tr);
    munmap(p, sizeof ROUTINE);
    return 0;
}

#else
int main(void) {
    printf("# SKIP branchsnap: x86-64 Linux only\n");
    return 0;
}
#endif
