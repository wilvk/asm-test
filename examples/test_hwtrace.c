/*
 * test_hwtrace.c — smoke test for the optional hardware-assisted native-trace
 * tier (asmtest_hwtrace.h). Self-skips with a clear reason (exit 0) when the
 * decoder library, the intel_pt/cs_etm PMU, the right CPU, or perf_event
 * privilege is absent — the common case off bare metal (and always on AMD/VM/CI).
 *
 * On a capable bare-metal Intel-PT host (perf_event_paranoid lowered) it traces a
 * host-native routine and asserts block offset 0 plus a deterministic ordered
 * instruction stream — matching the Unicorn/DynamoRIO output for the same bytes.
 */
#include "asmtest_hwtrace.h"
#include "asmtest_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#if defined(__linux__) && defined(__x86_64__)
#include <linux/perf_event.h>
int asmtest_amd_decode(const struct perf_branch_entry *br, size_t nbr,
                       const void *base, size_t len, asmtest_trace_t *trace);
int asmtest_amd_decoder_present(void);
#endif

/* mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two blocks). */
static const unsigned char ROUTINE[] = {
    0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0x48, 0x3d, 0x64, 0x00,
    0x00, 0x00, 0x7e, 0x03, 0x48, 0xff, 0xc8, 0xc3};
typedef long (*add2_fn)(long, long);

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf(c ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);            \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* AMD-LBR reconstruction is validated WITHOUT capture hardware: feed the decoder
 * a synthetic branch-record array (what Zen 3/4 would capture for a known path)
 * and assert it reconstructs the exact same offsets the PT/DynamoRIO backends do.
 * Runs on any Linux x86-64 host with Capstone (incl. this Zen 2 box, where live
 * AMD capture self-skips). */
static void test_amd_reconstruction(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_amd_decoder_present()) {
        printf("# SKIP AMD reconstruction: built without Capstone\n");
        return;
    }
    uint64_t b = (uint64_t)(uintptr_t)ROUTINE;
    /* fn(20,22)=42: taken branches are jle (0xc->0x11) then ret (0x11->out).
     * perf delivers the stack newest-first, so: [ret, jle]. */
    struct perf_branch_entry br[2];
    memset(br, 0, sizeof br);
    br[0].from = b + 0x11; br[0].to = b + sizeof ROUTINE; /* ret -> outside */
    br[1].from = b + 0xc;  br[1].to = b + 0x11;           /* jle -> ret     */

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    int rc = asmtest_amd_decode(br, 2, ROUTINE, sizeof ROUTINE, tr);
    CHECK(rc == 0, "AMD decode succeeds on a synthetic Tier-A branch stack");
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int seq = (asmtest_emu_trace_insns_total(tr) == 5);
    for (size_t i = 0; seq && i < 5; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq, "AMD reconstruction yields the exact PT/DR instruction sequence");
    CHECK(asmtest_trace_covered(tr, 0) && asmtest_trace_covered(tr, 0x11),
          "AMD reconstruction yields the matching block partition {0, 0x11}");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2,
          "AMD reconstruction records exactly two blocks");
    asmtest_trace_free(tr);

    /* Overflow: a full 16-entry stack must set truncated (window exceeded). */
    struct perf_branch_entry full[16];
    memset(full, 0, sizeof full);
    for (int i = 0; i < 16; i++) {
        full[i].from = b + 0xc;
        full[i].to = b + 0x11;
    }
    asmtest_trace_t *ot = asmtest_trace_new(64, 64);
    asmtest_amd_decode(full, 16, ROUTINE, sizeof ROUTINE, ot);
    CHECK(asmtest_emu_trace_truncated(ot),
          "AMD full 16-entry stack sets truncated (window overflow)");
    asmtest_trace_free(ot);
#else
    printf("# SKIP AMD reconstruction: not Linux x86-64\n");
#endif
}

/* Single-step (EFLAGS.TF) LIVE capture: unlike PT/AMD this runs on ANY x86-64
 * Linux host — no PMU, no perf_event, no privilege — so it executes here (and on
 * standard CI / in a plain container) instead of self-skipping. Trace the shared
 * fixture on the real CPU and assert byte-for-byte parity with the
 * Unicorn/DynamoRIO/PT instruction+block partition. */
static void test_singlestep_live(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        char why[160];
        asmtest_hwtrace_skip_reason(ASMTEST_HWTRACE_SINGLESTEP, why, sizeof why);
        printf("# SKIP single-step live capture: %s\n", why);
        return;
    }
    void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP single-step: mmap failed\n");
        return;
    }
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);

    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK, "single-step init");

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    CHECK(asmtest_hwtrace_register_region("add2", p, sizeof ROUTINE, tr) ==
              ASMTEST_HW_OK,
          "single-step register region");

    add2_fn fn = (add2_fn)p;
    asmtest_hwtrace_begin("add2");
    long r = fn(20, 22); /* 42 <= 100: jle taken, dec (0xe) skipped */
    asmtest_hwtrace_end("add2");

    CHECK(r == 42, "single-step traced call returns 20+22");
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int seq = (asmtest_emu_trace_insns_total(tr) == 5);
    for (size_t i = 0; seq && i < 5; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq, "single-step yields the exact live instruction stream [0,3,6,c,11]");
    CHECK(asmtest_trace_covered(tr, 0) && asmtest_trace_covered(tr, 0x11),
          "single-step block partition {0, 0x11} matches PT/AMD/DynamoRIO");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2,
          "single-step records exactly two blocks");
    CHECK(!asmtest_emu_trace_truncated(tr),
          "single-step trace is complete (not truncated)");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    munmap(p, sizeof ROUTINE);
}

/* The single-step differentiator: NO depth ceiling. A 20-trip loop takes 19 taken
 * back-edges — past AMD LBR's 16-entry window (which would flag truncated) — yet
 * single-step reconstructs every instruction exactly and stays complete. */
static void test_singlestep_loop(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP))
        return; /* already reported by test_singlestep_live */
    /* mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret   (rdi=step, rsi=trips) */
    static const unsigned char LOOP[] = {0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00, 0x00,
                                         0x48, 0x01, 0xf8, 0x48, 0xff, 0xce,
                                         0x75, 0xf8, 0xc3};
    void *p = mmap(NULL, sizeof LOOP, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return;
    memcpy(p, LOOP, sizeof LOOP);
    mprotect(p, sizeof LOOP, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof LOOP);

    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    asmtest_hwtrace_init(&opts);
    asmtest_trace_t *tr = asmtest_trace_new(256, 64);
    asmtest_hwtrace_register_region("loop", p, sizeof LOOP, tr);

    long (*fn)(long, long) = (long (*)(long, long))p;
    asmtest_hwtrace_begin("loop");
    long r = fn(1, 20); /* 20 trips, returns 20 */
    asmtest_hwtrace_end("loop");

    CHECK(r == 20, "single-step loop call returns sum");
    /* 1 (mov) + 20*(add,dec,jnz) + 1 (ret) = 62 instructions, all captured. */
    CHECK(asmtest_emu_trace_insns_total(tr) == 62,
          "single-step captures all 62 insns of a 20-trip loop (no depth ceiling)");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2 && asmtest_trace_covered(tr, 0) &&
              asmtest_trace_covered(tr, 0x7),
          "single-step loop block partition {0, 0x7}");
    CHECK(!asmtest_emu_trace_truncated(tr),
          "single-step loop trace complete past LBR's 16-branch window");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    munmap(p, sizeof LOOP);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Backend-independent: validate the AMD reconstruction decoder. */
    test_amd_reconstruction();

    /* Live, on this very host: the single-step backend (no PMU/perf/privilege). */
    test_singlestep_live();
    test_singlestep_loop();

    asmtest_trace_backend_t backend = ASMTEST_HWTRACE_INTEL_PT;
    if (!asmtest_hwtrace_available(backend)) {
        char why[160];
        asmtest_hwtrace_skip_reason(backend, why, sizeof why);
        printf("# SKIP hwtrace PT capture (Intel PT): %s\n", why);
        char awhy[160];
        asmtest_hwtrace_skip_reason(ASMTEST_HWTRACE_AMD_LBR, awhy, sizeof awhy);
        printf("# SKIP hwtrace AMD capture (AMD LBR): %s\n", awhy);
        if (checks == 0)
            printf("1..0 # skipped\n");
        else
            printf("1..%d\n# %d passed, %d failed\n", checks, checks - failures,
                   failures);
        return failures == 0 ? 0 : 1;
    }

    /* Capable host: exercise the real capture + decode path. */
    void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP hwtrace: mmap failed\n1..0 # skipped\n");
        return 0;
    }
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);

    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = backend;
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK, "hwtrace init");

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    CHECK(asmtest_hwtrace_register_region("add2", p, sizeof ROUTINE, tr) ==
              ASMTEST_HW_OK,
          "register native range");

    add2_fn fn = (add2_fn)p;
    asmtest_hwtrace_begin("add2");
    long r = fn(20, 22);
    asmtest_hwtrace_end("add2");
    CHECK(r == 42, "traced call returns 20+22");
    CHECK(asmtest_trace_covered(tr, 0), "block offset 0 covered");
    CHECK(asmtest_emu_trace_insns_total(tr) >= 4,
          "ordered instruction stream reconstructed");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    munmap(p, sizeof ROUTINE);

    printf("1..%d\n# %d passed, %d failed\n", checks, checks - failures,
           failures);
    return failures == 0 ? 0 : 1;
}
