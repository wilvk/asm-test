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

/* Capture the REACH (reconstructed insns_total) of `code(a,b)` via the deterministic
 * snapshot marker path with the given branch_filter (0 = full, 1 = reduced). Returns 0
 * if the tier is unavailable; writes the call result to *result. Used to MEASURE the
 * #2B reduced-filter window stretch: the snapshot is one frozen 16-deep window, so its
 * reach is stable (no sampling variance) and the full-vs-reduced ratio is meaningful. */
static unsigned long long snap_reach(const unsigned char *code, size_t nbytes,
                                     int branch_filter, long a, long b,
                                     long *result) {
    void *cp = mmap(NULL, nbytes, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cp == MAP_FAILED)
        return 0;
    memcpy(cp, code, nbytes);
    mprotect(cp, nbytes, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)cp, (char *)cp + nbytes);
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.struct_size = sizeof opts; /* self-describe (flag-day ABI guard) */
    opts.backend = ASMTEST_HWTRACE_AMD_LBR;
    opts.snapshot = 1;
    opts.branch_filter = branch_filter;
    unsigned long long ni = 0;
    if (asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK) {
        asmtest_trace_t *t = asmtest_trace_new(512, 128);
        if (asmtest_hwtrace_register_region("reach", cp, nbytes, t) ==
            ASMTEST_HW_OK) {
            long (*fn)(long, long) = (long (*)(long, long))cp;
            asmtest_hwtrace_begin("reach");
            long r = fn(a, b);
            asmtest_hwtrace_end("reach");
            if (result != NULL)
                *result = r;
            ni = asmtest_emu_trace_insns_total(t);
        }
        asmtest_hwtrace_shutdown();
        asmtest_trace_free(t);
    }
    munmap(cp, nbytes);
    return ni;
}

/* P5 multi-exit DEFAULT-ON: run `code(a,b)` through the ordinary begin/end markers
 * with opts.snapshot UNSET — hwtrace_begin_amd must select the multi-exit boundary
 * snapshot by DEFAULT (1..4 exits) — and require the entry block covered with
 * truncated==false: the sampled fallback honestly TRUNCATES a tiny single-shot
 * routine, so a clean non-truncated reconstruction proves the deterministic path
 * armed and the taken exit hit its breakpoint. Returns 1 on pass. */
/* `want_off`/`other_off` are the two exits' own path-specific blocks (NOT block 0 —
 * amd_replay appends block 0 unconditionally, amd_backend.c:267, so `covered(t, 0)` is
 * VACUOUS and cannot discriminate a real capture from one that always reports "some
 * data decoded" regardless of which path actually ran; see the Phase 9 comment below
 * for the same fact established the first time). The real evidence that THIS path's
 * exit was captured is want_off covered AND other_off NOT covered — a snapshot that
 * decoded both blocks regardless of which one actually executed would pass a bare
 * `ni > 0` check but fail this one. */
static int snap_default_run(void *cp, size_t nbytes, long a, long b,
                            long expect, uint64_t want_off, uint64_t other_off,
                            const char *what) {
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.struct_size = sizeof opts; /* self-describe (flag-day ABI guard) */
    opts.backend = ASMTEST_HWTRACE_AMD_LBR;
    /* opts.snapshot intentionally UNSET: this drives the DEFAULT-ON selection. */
    if (asmtest_hwtrace_init(&opts) != ASMTEST_HW_OK) {
        printf("# SKIP branchsnap multi-exit %s: AMD LBR tier unavailable\n",
               what);
        return 1;
    }
    int ok = 0;
    asmtest_trace_t *t = asmtest_trace_new(64, 64);
    if (asmtest_hwtrace_register_region("bsnapmx", cp, nbytes, t) ==
        ASMTEST_HW_OK) {
        long (*fn)(long, long) = (long (*)(long, long))cp;
        asmtest_hwtrace_begin("bsnapmx");
        long r = fn(a, b);
        asmtest_hwtrace_end("bsnapmx");
        int cov_want = asmtest_trace_covered(t, want_off);
        int cov_other = asmtest_trace_covered(t, other_off);
        unsigned long long ni = asmtest_emu_trace_insns_total(t);
        int trunc = asmtest_emu_trace_truncated(t);
        printf("branchsnap multi-exit %s: max2(%ld,%ld)=%ld; default-on "
               "snapshot decoded %llu insns, want(0x%02llx)=%d, "
               "other(0x%02llx)=%d, truncated=%d\n",
               what, a, b, r, ni, (unsigned long long)want_off, cov_want,
               (unsigned long long)other_off, cov_other, trunc);
        ok = (r == expect) && ni > 0 && !trunc && cov_want && !cov_other;
    } else {
        printf("not ok - branchsnap multi-exit %s: register_region failed\n",
               what);
    }
    asmtest_hwtrace_shutdown();
    asmtest_trace_free(t);
    return ok;
}

/* T2 (amd-branchsnap-lbr-docs) — a jmp-chain fixture is a valid leaf callable:
 * each `jmp +0` (EB 00) transfers to the immediately-following instruction, so a
 * chain of N falls straight through to the trailing `ret`. Run via the direct
 * snapshot entry to freeze a NEAR-SATURATED 16-deep window. */
static void run_jmpchain(void *arg) {
    void (*fn)(void) = (void (*)(void))arg;
    fn();
}

/* Capture a jmp-chain fixture through asmtest_amd_snapshot_trace and assert the
 * reconstructed in-region insn count (when expect_ni >= 0) and the truncated flag.
 * Returns 1 on pass. Called only under the outer rc==ASMTEST_HW_OK gate, so a
 * non-OK snapshot rc here is a real failure, not a self-skip. */
static int snap_jmpchain_check(const unsigned char *code, size_t nbytes,
                               size_t exit_off, long long expect_ni,
                               int expect_trunc, const char *what) {
    void *cp = mmap(NULL, nbytes, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cp == MAP_FAILED) {
        printf("not ok - branchsnap %s: mmap failed\n", what);
        return 0;
    }
    memcpy(cp, code, nbytes);
    mprotect(cp, nbytes, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)cp, (char *)cp + nbytes);
    asmtest_trace_t *t = asmtest_trace_new(64, 64);
    int rc =
        asmtest_amd_snapshot_trace(cp, nbytes, exit_off, run_jmpchain, cp, t);
    unsigned long long ni = asmtest_emu_trace_insns_total(t);
    int trunc = asmtest_emu_trace_truncated(t);
    printf(
        "branchsnap %s: decoded %llu in-region insns, truncated=%d (rc=%d)\n",
        what, ni, trunc, rc);
    int ok = (rc == ASMTEST_HW_OK) && (trunc == expect_trunc) &&
             (expect_ni < 0 || ni == (unsigned long long)expect_ni);
    if (ok)
        printf("ok - branchsnap %s\n", what);
    else
        printf("not ok - branchsnap %s: expected ni=%lld trunc=%d\n", what,
               expect_ni, expect_trunc);
    asmtest_trace_free(t);
    munmap(cp, nbytes);
    return ok;
}

/* --- Phase 9: the tail-`jmp` boundary NON-EVICTION experiment -------------------
 *
 * The AMD plan validated the Zen-5 non-eviction property (a planted #DB does NOT evict
 * the region's in-region branches before bpf_get_branch_snapshot() reads the frozen
 * stack) with a `ret` exit, and carried the tail-`jmp` exit as an ASSUMPTION to
 * re-confirm live. These fixtures settle it on the target substrate.
 *
 * WHY THIS MEASUREMENT DISCRIMINATES (the whole point — a test that passes either way
 * would be worthless). Both fixtures run the SAME body: a taken `jle` at 0x0c that
 * SKIPS the `dec rax` at 0x0e. Reconstruction of that skip is what depends on the
 * jle's in-region LBR edge surviving to the snapshot read:
 *
 *   - Non-eviction HOLDS  -> the jle edge {from=0x0c, to=0x11} is in the frozen window,
 *                            amd_replay FOLLOWS it, and 0x0e is NEVER decoded.
 *   - The #DB EVICTED it  -> the jle edge is gone; amd_replay reaches 0x0c with no
 *                            recorded edge there, treats it as a NOT-taken conditional,
 *                            FALLS THROUGH, and decodes 0x0e — a SILENTLY WRONG trace
 *                            with truncated==0 (amd_backend.c:378-379).
 *   - TOTAL eviction      -> best_inregion==0 -> branchsnap.c:360 sets truncated.
 *
 * So `!covered(0x0e) && !truncated` fails under BOTH eviction modes. The discriminator
 * is the DEC block at 0x0e, not the entry block: amd_replay appends block 0
 * UNCONDITIONALLY (amd_backend.c:267), so `covered(0, ...)` is VACUOUS here and is
 * asserted only as a shape check, never as the eviction evidence.
 *
 * NEGATIVE CONTROL (proves 0x0e is PRODUCIBLE, so its absence above is an observation
 * and not a structural impossibility of the fixture): the same routine run with the jle
 * NOT taken (a=200,b=1 -> 201 > 100) must decode 0x0e and report covered(0x0e)==1. If
 * that control did not pass, `!covered(0x0e)` in the taken runs would prove nothing.
 *
 * POSITIVE CONTROL: the identical body exiting via `ret` — the shape the plan ALREADY
 * validated — must give the identical answer, tying the new tail-jmp measurement to the
 * known-good one. */
struct snap_facts {
    int ran; /* the capture was live (tier up + region registered) */
    int truncated;
    int cov0; /* entry block 0 — VACUOUS (block 0 is unconditional); shape only */
    int cov_dec; /* the 0x0e `dec` block — THE discriminator */
    unsigned long long ni;
    long result;
};

/* Run `code(a,b)` through the ordinary begin/end markers with opts.snapshot UNSET (the
 * DEFAULT-ON boundary snapshot), registering only [base, base+reglen) so a tail `jmp`
 * whose target sits at/after `reglen` is a genuine region-LEAVING exit. Reports the
 * decoded facts; ran==0 means the tier self-skipped (never a pass). */
static struct snap_facts snap_facts_run(const unsigned char *code,
                                        size_t nbytes, size_t reglen, long a,
                                        long b, const char *what) {
    struct snap_facts f;
    memset(&f, 0, sizeof f);
    void *cp = mmap(NULL, nbytes, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cp == MAP_FAILED) {
        printf("# SKIP branchsnap phase9 %s: mmap failed\n", what);
        return f;
    }
    memcpy(cp, code, nbytes);
    mprotect(cp, nbytes, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)cp, (char *)cp + nbytes);

    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.struct_size = sizeof opts; /* self-describe (flag-day ABI guard) */
    opts.backend = ASMTEST_HWTRACE_AMD_LBR;
    /* opts.snapshot intentionally UNSET: this drives the DEFAULT-ON exit selection,
     * which is exactly what e9ca70e widened to cover a region-leaving tail jmp. */
    if (asmtest_hwtrace_init(&opts) != ASMTEST_HW_OK) {
        printf("# SKIP branchsnap phase9 %s: AMD LBR tier unavailable\n", what);
        munmap(cp, nbytes);
        return f;
    }
    asmtest_trace_t *t = asmtest_trace_new(64, 64);
    if (asmtest_hwtrace_register_region("bsnap9", cp, reglen, t) ==
        ASMTEST_HW_OK) {
        add2_fn fn = (add2_fn)cp;
        asmtest_hwtrace_begin("bsnap9");
        f.result = fn(a, b);
        asmtest_hwtrace_end("bsnap9");
        f.ran = 1;
        f.truncated = asmtest_emu_trace_truncated(t);
        f.cov0 = asmtest_trace_covered(t, 0);
        f.cov_dec = asmtest_trace_covered(t, 0x0e);
        f.ni = asmtest_emu_trace_insns_total(t);
        printf("branchsnap phase9 %s: fn(%ld,%ld)=%ld; decoded %llu insns, "
               "entry=%d, dec-block(0x0e)=%d, truncated=%d\n",
               what, a, b, f.result, f.ni, f.cov0, f.cov_dec, f.truncated);
    } else {
        printf("not ok - branchsnap phase9 %s: register_region failed\n", what);
    }
    asmtest_hwtrace_shutdown();
    asmtest_trace_free(t);
    munmap(cp, nbytes);
    return f;
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
        opts.struct_size = sizeof opts; /* self-describe (flag-day ABI guard) */
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

    /* P5 multi-exit default-on (the plan's Phase 5 cap-lane fixture): a TWO-ret
     * routine — max2 = (rdi < rsi) ? rsi : rdi — driven with opts.snapshot UNSET,
     * once down EACH exit path. The old single-exit gate (nexit == 1) routed any
     * multi-exit region to the sampled path, which honestly truncates a tiny
     * single-shot routine; the multi-exit snapshot plants one breakpoint per ret
     * (2 of the 4 debug registers), so BOTH paths end on a deterministic boundary:
     * entry block covered and !truncated on both. Gated on the direct capture above
     * having run live (rc==OK) — where caps/substrate are absent the whole tier
     * self-skips, and this proves nothing. */
    if (rc == ASMTEST_HW_OK) {
        static const unsigned char MAX2[] = {
            0x48, 0x39, 0xf7, /* 0x00 cmp rdi, rsi          */
            0x7c, 0x04,       /* 0x03 jl 0x09 (L)           */
            0x48, 0x89, 0xf8, /* 0x05 mov rax, rdi          */
            0xc3,             /* 0x08 ret      (exit 1)     */
            0x48, 0x89, 0xf0, /* 0x09 L: mov rax, rsi       */
            0xc3};            /* 0x0c ret      (exit 2)     */
        void *mp = mmap(NULL, sizeof MAX2, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mp == MAP_FAILED) {
            printf("# SKIP branchsnap multi-exit: mmap failed\n");
        } else {
            memcpy(mp, MAX2, sizeof MAX2);
            mprotect(mp, sizeof MAX2, PROT_READ | PROT_EXEC);
            __builtin___clear_cache((char *)mp, (char *)mp + sizeof MAX2);
            /* path A (a>=b, jl NOT taken): falls through into the 0x05 "mov
             * rax,rdi" block and must NEVER touch path B's 0x09 block, which a
             * taken jl alone reaches. want/other reversed for path B. */
            int ok_a = snap_default_run(mp, sizeof MAX2, 10, 3, 10, 0x05, 0x09,
                                        "path-A(ret@0x08)");
            int ok_b = snap_default_run(mp, sizeof MAX2, 3, 10, 10, 0x09, 0x05,
                                        "path-B(ret@0x0c)");
            munmap(mp, sizeof MAX2);
            if (ok_a && ok_b)
                printf("ok - branchsnap multi-exit: default-on snapshot covers "
                       "BOTH exits of a two-ret routine (!truncated down each "
                       "path, no opts.snapshot)\n");
            else {
                printf("not ok - branchsnap multi-exit: a default-on path "
                       "missed its boundary\n");
                munmap(p, sizeof ROUTINE);
                return 1;
            }
        }
    } else {
        printf("# SKIP branchsnap multi-exit: snapshot capture not live\n");
    }

    /* T2 (amd-branchsnap-lbr-docs) — the T1 use==15/16 boundary, pinned live on
     * real LbrExtV2. These are NEAR-SATURATED windows, not "tiny routines" (the
     * review refuted that framing — shipped fixtures run use ~ 1-4 and the trim
     * already rescues them). JMP14 fills 15 of the 16 hardware slots (14 taken
     * `jmp +0` edges + the entry-call edge, to==base region-involved) leaving one
     * pre-entry glue slot; trimming drops the glue -> use==15, n_dec==16 with the
     * synthetic boundary edge — exactly the count that spuriously tripped the old
     * ceiling. It must reconstruct !truncated with 15 in-region insns (14 jmp +
     * ret). JMP15 saturates all 16 slots (use==16) and must truncate honestly (an
     * older in-region edge may have been evicted). Pre-fix, JMP14 fails here with
     * truncated=1 — run it once against the unfixed tree to watch it discriminate.
     * Gated on the direct capture above having run live (rc==OK). */
    if (rc == ASMTEST_HW_OK) {
        /* 14 * `jmp +0` (EB 00) at 0x00..0x1A, then `ret` at 0x1C. */
        static const unsigned char JMP14[] = {
            0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00,
            0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00,
            0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xC3};
        /* 15 * `jmp +0` at 0x00..0x1C, then `ret` at 0x1E. */
        static const unsigned char JMP15[] = {
            0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB,
            0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00,
            0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xC3};
        int ok15 = snap_jmpchain_check(JMP14, sizeof JMP14, 0x1C, 15, 0,
                                       "use15: 15 hw slots + boundary edge "
                                       "reconstructs !truncated (T1 boundary)");
        int ok16 = snap_jmpchain_check(JMP15, sizeof JMP15, 0x1E, -1, 1,
                                       "use16: 16 saturated hw slots truncate "
                                       "honestly");
        if (!(ok15 && ok16)) {
            munmap(p, sizeof ROUTINE);
            return 1;
        }
    } else {
        printf("# SKIP branchsnap use15/use16: snapshot capture not live\n");
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
            opts.struct_size =
                sizeof opts; /* self-describe (flag-day ABI guard) */
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

    /* #2B reach-gain MEASUREMENT (deterministic). A loop whose body has a direct uncond
     * jmp AND a conditional back-edge: mov rax,0; L: add rax,rdi; jmp M; int3*3; M: dec
     * rsi; jnz L; ret. Per iteration the FULL filter records two taken branches (the jmp
     * + the jnz), the REDUCED filter records ONE (only the kept jnz; the dropped jmp is
     * followed from the bytes) — so a single 16-deep snapshot window spans ~2x more
     * executed instructions under the reduced filter. The snapshot is one frozen window,
     * so this ratio is stable (no sampling variance). reduced >= full always (if perf
     * rejects the type-filter combo the fallback ties); the printed ratio is the gain. */
    {
        static const unsigned char JMP_JNZ_LOOP[] = {
            0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00, 0x00, /* 0x00 mov rax, 0     */
            0x48, 0x01, 0xf8,                         /* 0x07 L: add rax,rdi */
            0xeb, 0x03,                               /* 0x0a jmp 0x0f (M)   */
            0xcc, 0xcc, 0xcc,                         /* 0x0c int3*3 (dead)  */
            0x48, 0xff, 0xce,                         /* 0x0f M: dec rsi     */
            0x75, 0xf3,                               /* 0x12 jnz 0x07 (L)   */
            0xc3};                                    /* 0x14 ret            */
        long rf = 0, rr = 0;
        unsigned long long full =
            snap_reach(JMP_JNZ_LOOP, sizeof JMP_JNZ_LOOP, 0, 1, 50, &rf);
        unsigned long long reduced =
            snap_reach(JMP_JNZ_LOOP, sizeof JMP_JNZ_LOOP, 1, 1, 50, &rr);
        if (full == 0 && reduced == 0) {
            /* No snapshot capture on this host (AMD LBR tier unavailable /
             * unprivileged: init or register self-skipped, so the routine never
             * ran under capture). Self-skip like every sibling block rather than
             * asserting a reach ratio that needs a live capture. */
            printf("# SKIP branchsnap #2B reach: snapshot capture not live\n");
            munmap(p, sizeof ROUTINE);
            return 0;
        }
        printf("branchsnap #2B reach: full-filter=%llu reduced-filter=%llu "
               "insns/window (reduced/full = %.2fx), results %ld/%ld\n",
               full, reduced, full ? (double)reduced / (double)full : 0.0, rf,
               rr);
        int okr = (full > 0) && (reduced >= full) && (rf == 50) && (rr == 50);
        if (okr)
            printf(
                "ok - branchsnap #2B reach: reduced filter reconstructs >= "
                "the full filter per 16-deep window (measured %.2fx stretch)\n",
                (double)reduced / (double)full);
        else
            printf("not ok - branchsnap #2B reach: full=%llu reduced=%llu "
                   "rf=%ld rr=%ld\n",
                   full, reduced, rf, rr);
        if (!okr) {
            munmap(p, sizeof ROUTINE);
            return 1;
        }
    }

    /* --- Phase 9: tail-`jmp` boundary non-eviction, live on the target substrate ---
     * Gated on the direct capture above having run live (rc==OK): where the caps /
     * substrate are absent the whole tier self-skips and this would prove nothing. */
    if (rc == ASMTEST_HW_OK) {
        /* TAILJMP — identical body to ROUTINE, but the exit at 0x11 is a region-LEAVING
         * direct `jmp` (a tail call), not a ret. The registered region is [base, 0x16);
         * the jmp's target 0x16 is the FIRST byte OUTSIDE it, where a `ret` stub returns
         * to our caller. So: zero ret-class instructions in-region, exactly one exit,
         * and that exit is a tail jmp — the shape e9ca70e widened default-on to cover.
         *   0x00 mov rax,rdi | 0x03 add rax,rsi | 0x06 cmp rax,100 | 0x0c jle 0x11
         *   0x0e dec rax     | 0x11 jmp 0x16 (rel32=0, leaves the region)
         *   ---- region ends at 0x16 ----      | 0x16 ret  (outside; the tail callee) */
        static const unsigned char TAILJMP[] = {
            0x48, 0x89, 0xf8,                   /* 0x00 mov rax, rdi     */
            0x48, 0x01, 0xf0,                   /* 0x03 add rax, rsi     */
            0x48, 0x3d, 0x64, 0x00, 0x00, 0x00, /* 0x06 cmp rax, 100     */
            0x7e, 0x03,                         /* 0x0c jle 0x11         */
            0x48, 0xff, 0xc8,                   /* 0x0e dec rax          */
            0xe9, 0x00, 0x00, 0x00, 0x00,       /* 0x11 jmp 0x16 (tail)  */
            0xc3};                              /* 0x16 ret (OUT of rgn) */
#define TAILJMP_REGLEN 0x16

        /* (1) THE MEASUREMENT: tail-jmp exit, jle TAKEN (42 <= 100 -> dec skipped).
         *     !cov_dec proves the jle's in-region edge SURVIVED the #DB at the tail jmp. */
        struct snap_facts tj =
            snap_facts_run(TAILJMP, sizeof TAILJMP, TAILJMP_REGLEN, 20, 22,
                           "tailjmp/jle-taken");

        /* (2) NEGATIVE CONTROL: same fixture, jle NOT taken (201 > 100 -> dec RUNS).
         *     cov_dec MUST be 1 here — proving 0x0e is producible by this fixture, so
         *     !cov_dec in (1) is a real observation rather than a shape the fixture can
         *     never emit. This is the guard against a vacuously-passing (1). */
        struct snap_facts nt =
            snap_facts_run(TAILJMP, sizeof TAILJMP, TAILJMP_REGLEN, 200, 1,
                           "tailjmp/jle-not-taken(control)");

        /* (3) POSITIVE CONTROL: the ALREADY-VALIDATED `ret`-exit shape, same body and
         *     same taken jle, through the same default-on path — the tail-jmp answer
         *     must match the known-good ret answer. */
        struct snap_facts rt =
            snap_facts_run(ROUTINE, sizeof ROUTINE, sizeof ROUTINE, 20, 22,
                           "ret-exit(control)");

        if (!tj.ran || !nt.ran || !rt.ran) {
            printf("# SKIP branchsnap phase9: default-on snapshot not live\n");
        } else {
            /* The control FIRST: if 0x0e cannot be produced, the measurement is void. */
            int ctl_ok = (nt.result == 200) && nt.cov_dec && !nt.truncated;
            if (!ctl_ok)
                printf("not ok - branchsnap phase9 control: the not-taken run "
                       "did not produce the 0x0e dec block (result=%ld "
                       "dec=%d truncated=%d) — the measurement below would be "
                       "vacuous\n",
                       nt.result, nt.cov_dec, nt.truncated);

            /* The measurement. !cov_dec is the non-eviction evidence; !truncated rules
             * out the total-eviction mode; ni==5 pins the exact reconstruction
             * {0,3,6,0xc,0x11}. cov0 is asserted for shape only (it is vacuous). */
            int meas_ok = (tj.result == 42) && !tj.truncated && !tj.cov_dec &&
                          (tj.ni == 5) && tj.cov0;
            /* The ret twin must agree — same body, same taken jle, validated exit. */
            int pos_ok = (rt.result == 42) && !rt.truncated && !rt.cov_dec &&
                         (rt.ni == 5) && rt.cov0;

            if (ctl_ok && meas_ok && pos_ok) {
                printf(
                    "ok - branchsnap phase9: the #DB at a region-leaving tail "
                    "`jmp` does NOT evict the region's in-region branches "
                    "before the snapshot read — the jle edge survived (0x0e "
                    "unreached, 5/5 insns, !truncated), matching the "
                    "validated `ret` exit; the not-taken control shows 0x0e "
                    "IS producible, so the measurement discriminates\n");
            } else {
                if (!meas_ok)
                    printf("not ok - branchsnap phase9: tail-jmp boundary "
                           "NON-EVICTION VIOLATED or reconstruction wrong "
                           "(result=%ld truncated=%d dec-block=%d insns=%llu "
                           "entry=%d) — dec-block=1 with truncated=0 means the "
                           "#DB evicted the jle edge and the trace is silently "
                           "WRONG\n",
                           tj.result, tj.truncated, tj.cov_dec, tj.ni, tj.cov0);
                if (!pos_ok)
                    printf("not ok - branchsnap phase9: the validated ret-exit "
                           "control itself failed (result=%ld truncated=%d "
                           "dec-block=%d insns=%llu)\n",
                           rt.result, rt.truncated, rt.cov_dec, rt.ni);
                munmap(p, sizeof ROUTINE);
                return 1;
            }
        }
    } else {
        printf("# SKIP branchsnap phase9: snapshot capture not live\n");
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
