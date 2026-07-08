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
        printf("# SKIP branchsnap: substrate absent (needs AMD LbrExtV2 + "
               "perfmon_v2 + "
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
    int rc = asmtest_amd_snapshot_trace(p, sizeof ROUTINE, RET_OFF, run_routine,
                                        p, tr);

    if (rc == ASMTEST_HW_ENOSYS) {
        printf("# SKIP branchsnap: built without the BPF toolchain\n");
    } else if (rc == ASMTEST_HW_EUNAVAIL) {
        printf("# SKIP branchsnap: capture unavailable (need CAP_BPF + "
               "CAP_PERFMON)\n");
    } else if (rc != ASMTEST_HW_OK) {
        printf("not ok - branchsnap: asmtest_amd_snapshot_trace rc=%d\n", rc);
        return 1;
    } else {
        int covered0 = asmtest_trace_covered(tr, 0);
        unsigned long long ni = asmtest_emu_trace_insns_total(tr);
        printf("branchsnap: add2(20,22)=%ld; deterministic snapshot decoded "
               "%llu in-region "
               "instructions, entry-block covered=%d, truncated=%d\n",
               (long)g_result, ni, covered0, asmtest_emu_trace_truncated(tr));
        if (covered0 && ni > 0)
            printf("ok - branchsnap: the boundary snapshot captured the tiny "
                   "single-shot "
                   "routine (entry block reconstructed) where sampling "
                   "truncates\n");
        else
            printf("not ok - branchsnap: snapshot decoded no in-region entry "
                   "block\n");
        if (!(covered0 && ni > 0)) {
            asmtest_trace_free(tr);
            munmap(p, sizeof ROUTINE);
            return 1;
        }
    }
    asmtest_trace_free(tr);

    /* Marker-path routing (AMD plan Phase 3 follow-up): opts.snapshot = 1 makes the
     * ORDINARY region begin/end markers route to this same deterministic capture —
     * no explicit exit offset, no run_fn callback; hwtrace derives the exit (the
     * region's last ret) and arms/drains around the user's own call. The same tiny
     * single-shot routine that the sampled path honestly truncates must reconstruct
     * its entry block through the plain begin/end surface. */
    {
        asmtest_hwtrace_options_t opts;
        memset(&opts, 0, sizeof opts);
        opts.backend = ASMTEST_HWTRACE_AMD_LBR;
        opts.snapshot = 1;
        if (asmtest_hwtrace_init(&opts) != ASMTEST_HW_OK) {
            printf("# SKIP branchsnap markers: AMD LBR tier unavailable\n");
        } else {
            asmtest_trace_t *mt = asmtest_trace_new(64, 64);
            if (asmtest_hwtrace_register_region("bsnap", p, sizeof ROUTINE,
                                                mt) != ASMTEST_HW_OK) {
                printf("not ok - branchsnap markers: register_region failed\n");
                asmtest_hwtrace_shutdown();
                asmtest_trace_free(mt);
                munmap(p, sizeof ROUTINE);
                return 1;
            }
            add2_fn fn = (add2_fn)p;
            asmtest_hwtrace_begin("bsnap");
            long r = fn(20, 22);
            asmtest_hwtrace_end("bsnap");
            int covered0 = asmtest_trace_covered(mt, 0);
            unsigned long long ni = asmtest_emu_trace_insns_total(mt);
            printf("branchsnap markers: add2(20,22)=%ld; begin/end snapshot "
                   "decoded "
                   "%llu insns, entry-block covered=%d, truncated=%d\n",
                   (long)r, ni, covered0, asmtest_emu_trace_truncated(mt));
            int okm = (r == 42) && covered0 && ni > 0;
            if (okm)
                printf("ok - branchsnap markers: opts.snapshot routes "
                       "begin/end to "
                       "the deterministic boundary capture\n");
            else
                printf("not ok - branchsnap markers: begin/end snapshot missed "
                       "the "
                       "single-shot routine\n");
            asmtest_hwtrace_shutdown();
            asmtest_trace_free(mt);
            if (!okm) {
                munmap(p, sizeof ROUTINE);
                return 1;
            }
        }
    }

    /* #2B live reduced-filter follow (deterministic). A routine with a DIRECT
     * UNCONDITIONAL jmp on the executed path plus a kept conditional anchor: with
     * opts.branch_filter=1 the reduced HW filter DROPS the jmp, so a reconstruction that
     * covers the jmp's TARGET block (0x08) proves amd_replay FOLLOWED the dropped jmp
     * from the region bytes on live LbrExtV2 — deterministic (one frozen snapshot), not
     * timing-sampled. (If perf rejects the type-filter combo the capture falls back to
     * the full filter and the jmp edge is recorded instead — the reconstruction is
     * identical either way, so the assertion is robust.) */
    {
        /* mov rax,rdi; jmp L; int3*3 (dead); L: add rax,rsi; cmp rax,100; jle DONE;
         * dec rax; DONE: ret. add2(20,22)=42 takes the jmp (skipping the dead bytes)
         * then the jle (skipping dec). jmp@0x03 -> 0x08 is the dropped direct uncond
         * branch; jle@0x11 (kept conditional) is the anchor; single ret @0x16. */
        static const unsigned char JMP_ROUTINE[] = {
            0x48, 0x89, 0xf8,                   /* 0x00 mov rax, rdi    */
            0xeb, 0x03,                         /* 0x03 jmp 0x08 (L)    */
            0xcc, 0xcc, 0xcc,                   /* 0x05 int3*3 (dead)   */
            0x48, 0x01, 0xf0,                   /* 0x08 L: add rax,rsi  */
            0x48, 0x3d, 0x64, 0x00, 0x00, 0x00, /* 0x0b cmp rax, 100    */
            0x7e, 0x03,                         /* 0x11 jle 0x16 (DONE) */
            0x48, 0xff, 0xc8,                   /* 0x13 dec rax         */
            0xc3};                              /* 0x16 DONE: ret       */
        void *jp = mmap(NULL, sizeof JMP_ROUTINE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (jp == MAP_FAILED) {
            printf("# SKIP branchsnap #2B: mmap failed\n");
        } else {
            memcpy(jp, JMP_ROUTINE, sizeof JMP_ROUTINE);
            mprotect(jp, sizeof JMP_ROUTINE, PROT_READ | PROT_EXEC);
            __builtin___clear_cache((char *)jp,
                                    (char *)jp + sizeof JMP_ROUTINE);
            asmtest_hwtrace_options_t opts;
            memset(&opts, 0, sizeof opts);
            opts.backend = ASMTEST_HWTRACE_AMD_LBR;
            opts.snapshot = 1;
            opts.branch_filter =
                1; /* reduced filter: drop the direct uncond jmp */
            if (asmtest_hwtrace_init(&opts) != ASMTEST_HW_OK) {
                printf("# SKIP branchsnap #2B: AMD LBR tier unavailable\n");
            } else {
                asmtest_trace_t *jt = asmtest_trace_new(64, 64);
                if (asmtest_hwtrace_register_region("bsnap2b", jp,
                                                    sizeof JMP_ROUTINE,
                                                    jt) != ASMTEST_HW_OK) {
                    printf("not ok - branchsnap #2B: register_region failed\n");
                    asmtest_hwtrace_shutdown();
                    asmtest_trace_free(jt);
                    munmap(jp, sizeof JMP_ROUTINE);
                    munmap(p, sizeof ROUTINE);
                    return 1;
                }
                add2_fn fn = (add2_fn)jp;
                asmtest_hwtrace_begin("bsnap2b");
                long r = fn(20, 22);
                asmtest_hwtrace_end("bsnap2b");
                int cov0 = asmtest_trace_covered(jt, 0);
                int covL =
                    asmtest_trace_covered(jt, 0x08); /* jmp TARGET block */
                unsigned long long ni = asmtest_emu_trace_insns_total(jt);
                printf(
                    "branchsnap #2B: add2(20,22)=%ld; reduced-filter snapshot "
                    "decoded %llu insns, entry=%d, jmp-target(0x08)=%d, "
                    "truncated=%d\n",
                    (long)r, ni, cov0, covL, asmtest_emu_trace_truncated(jt));
                int ok2b = (r == 42) && cov0 && covL && ni > 0;
                if (ok2b)
                    printf(
                        "ok - branchsnap #2B: reduced-filter snapshot follows "
                        "the dropped jmp to its target block 0x08 on live "
                        "LbrExtV2\n");
                else
                    printf("not ok - branchsnap #2B: reduced-filter snapshot "
                           "missed the jmp-target block\n");
                asmtest_hwtrace_shutdown();
                asmtest_trace_free(jt);
                if (!ok2b) {
                    munmap(jp, sizeof JMP_ROUTINE);
                    munmap(p, sizeof ROUTINE);
                    return 1;
                }
            }
            munmap(jp, sizeof JMP_ROUTINE);
        }
    }

    munmap(p, sizeof ROUTINE);
    return 0;
}

#else
int main(void) {
    printf("# SKIP branchsnap: x86-64 Linux only\n");
    return 0;
}
#endif
