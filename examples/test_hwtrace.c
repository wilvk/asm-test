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
#include "asmtest_codeimage.h"
#include "asmtest_hwtrace.h"
#include "asmtest_ptrace.h"
#include "asmtest_trace.h"
#include "asmtest_trace_auto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* POSIX + ptrace headers: needed by the out-of-process stepper tests (x86-64 AND
 * AArch64) and the any-Linux /proc + jitdump reader tests (getpid, etc.). */
#if defined(__linux__)

#include <dlfcn.h> /* the default-denylist test resolves poll the same way the
                      backend does */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif

/* AMD branch-stack decoder declarations + perf_event are x86-64-only. */
#if defined(__linux__) && defined(__x86_64__)
#include <linux/perf_event.h>
int asmtest_amd_decode(const struct perf_branch_entry *br, size_t nbr,
                       const void *base, size_t len, asmtest_trace_t *trace);
int asmtest_amd_decoder_present(void);
int asmtest_amd_freeze_available(void);
int asmtest_amd_snapshot_available(void);
int asmtest_amd_lbr_depth(void);
size_t asmtest_amd_stitch(const struct perf_branch_entry *const *samples,
                          const size_t *nrs, size_t n_samples, const void *base,
                          uint64_t base_ip, size_t len,
                          struct perf_branch_entry *out, size_t out_cap,
                          int *gap);
int asmtest_amd_decode_stitched(const struct perf_branch_entry *br, size_t nbr,
                                const void *base, size_t len,
                                asmtest_trace_t *trace, int gap);
#endif

/* CoreSight reconstruction-core interface (decoder-independent half of
 * src/cs_backend.c). KEEP THIS STRUCT IN SYNC with asmtest_cs_range_t there. */
typedef struct {
    uint64_t start_off;
    uint64_t end_off;
    int ends_in_branch;
} cs_range_t;
int asmtest_cs_reconstruct(asmtest_arch_t arch, const cs_range_t *ranges,
                           size_t nranges, const void *base, size_t len,
                           asmtest_trace_t *trace);

/* §2 recorder-backed PT image adapter (pt_backend.c; libipt-independent). */
int asmtest_pt_read_codeimage(const asmtest_codeimage_t *img, uint64_t when,
                              uint64_t ip, uint8_t *buffer, size_t size);

/* mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two blocks). */
static const unsigned char ROUTINE[] = {0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0,
                                        0x48, 0x3d, 0x64, 0x00, 0x00, 0x00,
                                        0x7e, 0x03, 0x48, 0xff, 0xc8, 0xc3};
typedef long (*add2_fn)(long, long);

#if defined(__aarch64__)
/* AArch64 equivalents for the out-of-process ptrace stepper, which (unlike the
 * x86-only EFLAGS.TF / PT / AMD backends) runs on this arch too. add2(20,22)=42 takes
 * the b.le, skipping the sub at 0xc — executed stream {0,4,8,10}, two blocks {0,0x10}.
 *   add x0,x0,x1 ; cmp x0,#100 ; b.le 0x10 ; sub x0,x0,#1 ; ret  */
static const unsigned char ROUTINE_A64[] = {
    0x00, 0x00, 0x01, 0x8b, 0x1f, 0x90, 0x01, 0xf1, 0x4d, 0x00,
    0x00, 0x54, 0x00, 0x04, 0x00, 0xd1, 0xc0, 0x03, 0x5f, 0xd6};
/* loop(1,20)=20 over 63 insns (1 + 20*3 + 2), past any 16-deep branch window.
 *   mov x9,#0 ; L: add x9,x9,x0 ; subs x1,x1,#1 ; b.ne L ; mov x0,x9 ; ret  */
static const unsigned char LOOP_A64[] = {
    0x09, 0x00, 0x80, 0xd2, 0x29, 0x01, 0x00, 0x8b, 0x21, 0x04, 0x00, 0xf1,
    0xc1, 0xff, 0xff, 0x54, 0xe0, 0x03, 0x09, 0xaa, 0xc0, 0x03, 0x5f, 0xd6};
#endif

/* Map bytes W^X-correctly into executable memory (defined with the §0 tests below;
 * forward-declared here for the earlier AMD/concurrency fixtures). */
static void *ss_map_exec(const unsigned char *bytes, size_t len);

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
/* AMD freeze-on-PMI probe (CPUID 0x80000022 EAX[2]). Unprivileged (CPUID needs no
 * perf access), so it runs even where the AMD LBR CAPTURE self-skips. Asserts the probe
 * returns a definite, stable answer; prints this host's actual support so the freeze gate
 * in hwtrace_end_amd is observable. On non-AMD / non-x86 it is honestly 0. */
static void test_amd_freeze_probe(void) {
#if defined(__linux__) && defined(__x86_64__)
    int a = asmtest_amd_freeze_available();
    int b = asmtest_amd_freeze_available();
    CHECK(a == 0 || a == 1, "AMD freeze probe returns a definite 0/1");
    CHECK(a == b, "AMD freeze probe is stable (cached across calls)");
    printf(
        "# AMD LBR freeze-on-PMI (CPUID 0x80000022 EAX[2]) on this host: %s\n",
        a ? "PRESENT (single-window Tier-A trusted)"
          : "ABSENT (Tier-A window trusted only if it captured the region "
            "exit)");

    /* The deterministic software-event LBR-snapshot substrate (P0 #2 gate): AMD
     * LbrExtV2 + perfmon v2 + Linux >= 6.10. Unprivileged (flags + uname), so it
     * reports the hardware+kernel floor even where the capture would need CAP_BPF. */
    int s1 = asmtest_amd_snapshot_available();
    int s2 = asmtest_amd_snapshot_available();
    CHECK(s1 == 0 || s1 == 1,
          "AMD snapshot-substrate probe returns a definite 0/1");
    CHECK(s1 == s2, "AMD snapshot-substrate probe is stable (cached)");
    printf("# AMD deterministic LBR-snapshot substrate "
           "(LbrExtV2+perfmon_v2+kernel>=6.10): %s\n",
           s1 ? "PRESENT (boundary bpf_get_branch_snapshot buildable; run "
                "needs CAP_BPF)"
              : "ABSENT (falls back to sample_period=1 windows)");

    /* AMD Phase 0 — runtime branch-stack depth (CPUID 0x80000022 EBX), replacing the
     * hardcoded 16 that drives the Tier-A/Tier-B overflow split. Every shipping Zen
     * part reports 16; assert a sane, stable positive value and print this host's. */
    int d1 = asmtest_amd_lbr_depth();
    int d2 = asmtest_amd_lbr_depth();
    CHECK(d1 >= 1 && d1 <= 64, "AMD LBR depth is a sane positive value");
    CHECK(d1 == d2, "AMD LBR depth is stable (cached)");
    printf("# AMD LBR branch-stack depth on this host: %d (16 on every "
           "shipping Zen)\n",
           d1);
#else
    printf("# SKIP AMD freeze probe: x86-64 Linux only\n");
#endif
}

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
    br[0].from = b + 0x11;
    br[0].to = b + sizeof ROUTINE; /* ret -> outside */
    br[1].from = b + 0xc;
    br[1].to = b + 0x11; /* jle -> ret     */

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    int rc = asmtest_amd_decode(br, 2, ROUTINE, sizeof ROUTINE, tr);
    CHECK(rc == 0, "AMD decode succeeds on a synthetic Tier-A branch stack");
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int seq = (asmtest_emu_trace_insns_total(tr) == 5);
    for (size_t i = 0; seq && i < 5; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq,
          "AMD reconstruction yields the exact PT/DR instruction sequence");
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

/* AMD Phase 4 — LbrExtV2 speculation-bit filtering. A wrong-path branch record
 * (spec == PERF_BR_SPEC_WRONG_PATH — executed speculatively, never retired) is a
 * phantom edge LbrExtV2 still delivers; amd_replay must DROP it before replay, and
 * dropping it is expected, so it must NOT set truncated. Feed the clean add2 Tier-A
 * stack an extra newest phantom whose target (0x6) would, if trusted, add a spurious
 * block; assert the reconstruction stays byte-identical to the no-phantom case
 * (same insns, the same {0, 0x11} partition, complete). Host-independent, like
 * test_amd_reconstruction. Needs the `spec` bitfield (Linux >= 6.1 header); self-skips
 * otherwise — on such builds the filter is a compile-out no-op (Zen 3 BRS is
 * retired-only and has no spec bits either). */
static void test_amd_spec_filter(void) {
#if defined(__linux__) && defined(__x86_64__) &&                               \
    defined(ASMTEST_HAVE_PERF_BR_SPEC)
    if (!asmtest_amd_decoder_present()) {
        printf("# SKIP AMD spec filter: built without Capstone\n");
        return;
    }
    uint64_t b = (uint64_t)(uintptr_t)ROUTINE;
    /* newest-first: [phantom, ret, jle]. The phantom sits newest, so it replays
     * LAST — after ret has left the region; if TRUSTED its to=0x6 would append a
     * spurious block at 0x6. jle(0xc->0x11) then ret(0x11->out) are the real edges. */
    struct perf_branch_entry br[3];
    memset(br, 0, sizeof br);
    br[0].from =
        b + 0x3; /* phantom source (in-region, arbitrary)              */
    br[0].to =
        b + 0x6; /* phantom target — a spurious block 0x6 if unfiltered */
    br[0].spec =
        PERF_BR_SPEC_WRONG_PATH; /* header enum: exists with the field  */
    br[1].from = b + 0x11;
    br[1].to = b + sizeof ROUTINE; /* ret -> outside */
    br[2].from = b + 0xc;
    br[2].to = b + 0x11; /* jle -> ret */

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    int rc = asmtest_amd_decode(br, 3, ROUTINE, sizeof ROUTINE, tr);
    CHECK(rc == 0,
          "AMD decode succeeds with a wrong-path phantom in the stack");
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int seq = (asmtest_emu_trace_insns_total(tr) == 5);
    for (size_t i = 0; seq && i < 5; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq,
          "AMD wrong-path filter: instruction stream matches the clean case");
    CHECK(asmtest_trace_covered(tr, 0) && asmtest_trace_covered(tr, 0x11),
          "AMD wrong-path filter: block partition stays {0, 0x11}");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2,
          "AMD wrong-path filter: no spurious block from the phantom target");
    CHECK(
        !asmtest_trace_covered(tr, 0x6),
        "AMD wrong-path filter: the phantom target (0x6) is NOT a block start");
    CHECK(!asmtest_emu_trace_truncated(tr),
          "AMD wrong-path filter: dropping a phantom does NOT truncate");
    asmtest_trace_free(tr);
#else
    printf("# SKIP AMD spec filter: needs x86-64 Linux with the perf spec "
           "bitfield\n");
#endif
}

/* CoreSight reconstruction is validated WITHOUT a CoreSight board (exactly as the
 * AMD reconstruction is validated without Zen hardware): feed asmtest_cs_reconstruct
 * the instruction RANGES an ETM/ETE would emit for a known path and assert it
 * rebuilds the same offsets/blocks the PT/AMD/single-step backends produce. The core
 * is arch-independent, so we use the shared x86-64 fixture (ASMTEST_ARCH_X86_64) for
 * byte-for-byte comparability; the live path passes ASMTEST_ARCH_ARM64. Runs on any
 * host with Capstone. */
static void test_cs_reconstruction(void) {
    if (!asmtest_disas_available()) {
        printf("# SKIP CoreSight reconstruction: built without Capstone\n");
        return;
    }
    /* fn(20,22)=42: range 1 covers [0,0xe) (mov;add;cmp;jle) ending at the taken
     * jle; range 2 covers [0x11,0x12) (ret). Both end in a branch waypoint. */
    cs_range_t ranges[2] = {
        {0x0, 0xe, 1},
        {0x11, 0x12, 1},
    };
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    int rc = asmtest_cs_reconstruct(ASMTEST_ARCH_X86_64, ranges, 2, ROUTINE,
                                    sizeof ROUTINE, tr);
    CHECK(rc == ASMTEST_HW_OK,
          "CoreSight reconstruct succeeds on synthetic ranges");
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int seq = (asmtest_emu_trace_insns_total(tr) == 5);
    for (size_t i = 0; seq && i < 5; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(
        seq,
        "CoreSight reconstruction yields the exact PT/AMD instruction stream");
    CHECK(asmtest_trace_covered(tr, 0) && asmtest_trace_covered(tr, 0x11),
          "CoreSight reconstruction yields the matching block partition {0, "
          "0x11}");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2,
          "CoreSight reconstruction records exactly two blocks");
    CHECK(!asmtest_emu_trace_truncated(tr),
          "CoreSight reconstruction is complete");
    asmtest_trace_free(tr);

    /* A single straight-line range (no branch) reconstructs every instruction as one
     * block — the degenerate ETM range. */
    cs_range_t one[1] = {{0x0, 0x6, 0}}; /* mov; add (two insns, no branch) */
    asmtest_trace_t *st = asmtest_trace_new(64, 64);
    asmtest_cs_reconstruct(ASMTEST_ARCH_X86_64, one, 1, ROUTINE, sizeof ROUTINE,
                           st);
    CHECK(asmtest_emu_trace_insns_total(st) == 2 &&
              asmtest_emu_trace_blocks_len(st) == 1,
          "CoreSight straight-line range = 2 insns, 1 block");
    asmtest_trace_free(st);
}

/* mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret  — loop body block at 0x7, the
 * jnz back-edge target. A long trip count makes the routine run long enough that
 * PMU branch samples fire INSIDE the region (a tiny routine completes before a
 * second sample can arm), which is what AMD LBR capture needs. */
static const unsigned char AMD_LOOP[] = {0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00,
                                         0x00, 0x48, 0x01, 0xf8, 0x48, 0xff,
                                         0xce, 0x75, 0xf8, 0xc3};

/* One live AMD-LBR capture of AMD_LOOP(trips). Returns insns_total reconstructed;
 * reports loop-body coverage and the truncation bit. */
static uint64_t amd_capture_loop(void *p, long trips, int *cov7, int *trunc) {
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_AMD_LBR;
    asmtest_hwtrace_init(&opts);
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    asmtest_hwtrace_register_region("amdloop", p, sizeof AMD_LOOP, tr);
    long (*fn)(long, long) = (long (*)(long, long))p;
    asmtest_hwtrace_begin("amdloop");
    fn(1, trips);
    asmtest_hwtrace_end("amdloop");
    uint64_t n = asmtest_emu_trace_insns_total(tr);
    *cov7 = asmtest_trace_covered(tr, 0x7);
    *trunc = asmtest_emu_trace_truncated(tr);
    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    return n;
}

/* AMD LBR LIVE capture: on a Zen 3+ / Zen 4 / Zen 5 host with perf branch-stack
 * permitted, this exercises the REAL perf_event_open branch-record capture + decode
 * path (hwtrace_begin_amd/_end_amd -> asmtest_amd_decode), not the synthetic
 * reconstruction above. Self-skips where AMD LBR is unavailable (non-AMD host, no
 * LbrExtV2/BRS, perf locked down, or built without Capstone).
 *
 * AMD has no continuous trace ring; perf delivers the 16-deep branch stack only AT
 * a PMU sample. So capture works for branch-heavy routines (a sample fires inside
 * the region and snapshots its branches) and honestly TRUNCATES for a tiny
 * single-shot routine (too fast to be sampled in-region) — the fallback signal,
 * never an empty trace claimed complete. Verified on a Zen 5 host (Ryzen 9 9950X,
 * amd_lbr_v2). */
static void test_amd_live(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_AMD_LBR)) {
        char why[160];
        asmtest_hwtrace_skip_reason(ASMTEST_HWTRACE_AMD_LBR, why, sizeof why);
        printf("# SKIP AMD LBR live capture: %s\n", why);
        return;
    }

    /* (a) Tiny, fast single-shot routine: completes before a second PMU sample can
     * fire, so its branches are never sampled in-region. The capture must come back
     * truncated (the dynamic-fallback signal) — never insns=0 with truncated=0,
     * which the old "keep the last sample" logic produced (it decoded a post-routine
     * glue sample with no in-region branches and silently claimed complete). */
    void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        memcpy(p, ROUTINE, sizeof ROUTINE);
        mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);
        asmtest_hwtrace_options_t opts;
        memset(&opts, 0, sizeof opts);
        opts.backend = ASMTEST_HWTRACE_AMD_LBR;
        CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK,
              "AMD LBR live init");
        asmtest_trace_t *tr = asmtest_trace_new(64, 64);
        CHECK(asmtest_hwtrace_register_region("amd", p, sizeof ROUTINE, tr) ==
                  ASMTEST_HW_OK,
              "AMD LBR live register region");
        add2_fn fn = (add2_fn)p;
        asmtest_hwtrace_begin("amd");
        long r = fn(20, 22);
        asmtest_hwtrace_end("amd");
        CHECK(r == 42, "AMD LBR live single-shot call returns 20+22");
        CHECK(asmtest_emu_trace_truncated(tr) ||
                  asmtest_emu_trace_insns_total(tr) > 0,
              "AMD LBR live single-shot: honest result (never "
              "empty-yet-complete)");
        asmtest_hwtrace_shutdown();
        asmtest_trace_free(tr);

        /* opts.snapshot = 1: the deterministic-boundary opt-in. Where the BPF
         * substrate is present (docker-hwtrace-codeimage) begin/end route to the
         * exit-breakpoint snapshot (test_branchsnap asserts the complete capture);
         * on THIS lane (typically built without libbpf) the arm fails and the
         * markers must fall back to the sampled path with the same honest result —
         * never an error, never empty-yet-complete. */
        memset(&opts, 0, sizeof opts);
        opts.backend = ASMTEST_HWTRACE_AMD_LBR;
        opts.snapshot = 1;
        CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK,
              "AMD LBR live snapshot-opt-in init");
        asmtest_trace_t *st = asmtest_trace_new(64, 64);
        CHECK(asmtest_hwtrace_register_region("amdsnap", p, sizeof ROUTINE,
                                              st) == ASMTEST_HW_OK,
              "AMD LBR live snapshot-opt-in register");
        asmtest_hwtrace_begin("amdsnap");
        long r2 = fn(20, 22);
        asmtest_hwtrace_end("amdsnap");
        CHECK(r2 == 42, "AMD LBR live snapshot-opt-in call returns 20+22");
        CHECK(asmtest_emu_trace_truncated(st) ||
                  asmtest_emu_trace_insns_total(st) > 0,
              "AMD LBR live snapshot opt-in: honest result (deterministic "
              "capture, or clean fallback to sampling)");
        asmtest_hwtrace_shutdown();
        asmtest_trace_free(st);
        munmap(p, sizeof ROUTINE);
    }

    /* (b) Branch-heavy loop past the 16-deep window — the LIVE Tier-B path. With
     * sample_period=1 perf emits one branch-stack window per taken branch; the live
     * capture now COLLECTS them all and STITCHES the overlapping windows
     * (asmtest_amd_stitch), so the reconstruction runs far past a single 16-deep
     * snapshot — which alone caps at ~49 insns for this loop. The long run overflows
     * the perf data ring and sample_period=1 is heavily throttled, so the live result
     * stays truncated (an end-to-end *complete* stitch with no drops is what the
     * host-independent test_amd_stitch below proves); what we assert LIVE is that
     * stitching reached BEYOND one window. Sampling is statistical — retry, keep best. */
    void *q = mmap(NULL, sizeof AMD_LOOP, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q != MAP_FAILED) {
        memcpy(q, AMD_LOOP, sizeof AMD_LOOP);
        mprotect(q, sizeof AMD_LOOP, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)q, (char *)q + sizeof AMD_LOOP);
        int cov7 = 0, trunc = 0, best_cov7 = 0, best_trunc = 0;
        uint64_t best = 0;
        for (int attempt = 0; attempt < 12; attempt++) {
            uint64_t n = amd_capture_loop(q, 20000, &cov7, &trunc);
            if (n >= best) {
                best = n;
                best_cov7 = cov7;
                best_trunc = trunc;
            }
        }
        printf("# AMD LBR live loop (Tier-B): best_insns=%llu covered(0x7)=%d "
               "truncated=%d (one 16-window caps ~49)\n",
               (unsigned long long)best, best_cov7, best_trunc);
        CHECK(best > 0, "AMD LBR live loop: reconstructs in-region branches "
                        "from the real LBR");
        CHECK(best == 0 || best_cov7,
              "AMD LBR live loop: reconstructs the loop-body block 0x7");
        CHECK(best > 50, "AMD LBR live loop: Tier-B stitched BEYOND a single "
                         "16-deep window");
        CHECK(best == 0 || best_trunc,
              "AMD LBR live loop: the over-ring run stays honestly truncated");
        munmap(q, sizeof AMD_LOOP);
    }
}

/* AMD Tier-B STITCHING (host-validated, no hardware — like test_amd_reconstruction):
 * a routine that loops past the 16-deep window. With sample_period=1, perf emits one
 * branch-stack sample per taken branch, consecutive windows overlapping by 15 edges.
 * Synthesize those overlapping windows for an 18-iteration loop, stitch them back into
 * the gapless 18-edge sequence, and prove the decode is COMPLETE — where a single
 * Tier-A 16-window honestly truncates. Plus gap detection when overlap is lost. */
static void test_amd_stitch(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_amd_decoder_present()) {
        printf("# SKIP AMD stitch: built without Capstone\n");
        return;
    }
    const uint64_t b = (uint64_t)(uintptr_t)AMD_LOOP;
    enum { K = 18 }; /* taken back-edges, past the 16-deep window */
    /* The loop's only taken branch is the back-edge jnz @0xd -> L @0x7. */
    struct perf_branch_entry full[K];
    memset(full, 0, sizeof full);
    for (int i = 0; i < K; i++) {
        full[i].from = b + 0xd;
        full[i].to = b + 0x7;
    }
    /* sample_period=1 windows: sample j holds the last min(16, j+1) edges, newest-
     * first (window fills to 16, then slides one edge per sample). */
    struct perf_branch_entry windows[K][16];
    const struct perf_branch_entry *samples[K];
    size_t nrs[K];
    for (int j = 0; j < K; j++) {
        int depth = (j + 1 < 16) ? (j + 1) : 16;
        for (int e = 0; e < depth; e++)
            windows[j][e] = full[j - e]; /* newest-first */
        samples[j] = windows[j];
        nrs[j] = (size_t)depth;
    }

    struct perf_branch_entry stitched[64];
    int gap = 0;
    size_t n = asmtest_amd_stitch(samples, nrs, K, AMD_LOOP,
                                  (uint64_t)(uintptr_t)AMD_LOOP,
                                  sizeof AMD_LOOP, stitched, 64, &gap);
    CHECK(n == K && gap == 0,
          "AMD stitch recovers all 18 branches past the 16-deep window");

    /* Tier-A on the richest single (full, depth-16) window honestly truncates. */
    asmtest_trace_t *ta = asmtest_trace_new(64, 64);
    asmtest_amd_decode(samples[K - 1], nrs[K - 1], AMD_LOOP, sizeof AMD_LOOP,
                       ta);
    CHECK(asmtest_emu_trace_truncated(ta),
          "Tier-A single 16-entry window truncates (overflow)");
    asmtest_trace_free(ta);

    /* Tier-B stitched decode is COMPLETE — no depth ceiling. */
    asmtest_trace_t *tb = asmtest_trace_new(256, 64);
    int rc = asmtest_amd_decode_stitched(stitched, n, AMD_LOOP, sizeof AMD_LOOP,
                                         tb, gap);
    CHECK(rc == ASMTEST_HW_OK, "AMD Tier-B stitched decode succeeds");
    CHECK(!asmtest_emu_trace_truncated(tb),
          "AMD Tier-B stitched trace is COMPLETE (not truncated)");
    CHECK(asmtest_emu_trace_insns_total(tb) == 55,
          "AMD Tier-B reconstructs all 55 loop instructions (4 + 17*3)");
    CHECK(asmtest_trace_covered(tb, 0) && asmtest_trace_covered(tb, 0x7),
          "AMD Tier-B covers entry(0) and loop body(0x7)");
    CHECK(asmtest_emu_trace_blocks_len(tb) == 2,
          "AMD Tier-B records exactly two blocks {0, 0x7}");
    asmtest_trace_free(tb);

    /* Gap detection: a second window sharing no edge with the accumulated tail
     * (>= a full window of samples dropped to perf throttling) flags a real gap. */
    struct perf_branch_entry e0[1], e1[16];
    memset(e0, 0, sizeof e0);
    memset(e1, 0, sizeof e1);
    e0[0].from = b + 0xd;
    e0[0].to = b + 0x7;
    for (int e = 0; e < 16; e++) {
        e1[e].from = b + 0x100 + (uint64_t)e; /* disjoint from the seed edge */
        e1[e].to = b + 0x200 + (uint64_t)e;
    }
    const struct perf_branch_entry *gsamples[2] = {e0, e1};
    size_t gnrs[2] = {1, 16};
    struct perf_branch_entry gout[64];
    int gap2 = 0;
    asmtest_amd_stitch(gsamples, gnrs, 2, AMD_LOOP,
                       (uint64_t)(uintptr_t)AMD_LOOP, sizeof AMD_LOOP, gout, 64,
                       &gap2);
    CHECK(gap2 == 1,
          "AMD stitch flags a gap when windows lose overlap (throttled drop)");
#else
    printf("# SKIP AMD stitch: not Linux x86-64\n");
#endif
}

/* AMD Phase 5 — decodable-distance stitch guard. The smallest-overlap heuristic keys
 * on from+to equality alone, so a dropped/throttled sample can make it splice two
 * edges that aren't connected by real straight-line code. The guard requires the
 * tail's newest branch target to Capstone-decode forward to the next branch source;
 * an indecodable splice is rejected (larger shift / honest gap) rather than silently
 * stitched wrong. A legitimate contiguous splice is unaffected. Host-independent. */
static void test_amd_stitch_decodable(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_amd_decoder_present()) {
        printf(
            "# SKIP AMD stitch decodable-distance: built without Capstone\n");
        return;
    }
    const uint64_t b = (uint64_t)(uintptr_t)AMD_LOOP;
    struct perf_branch_entry A; /* the real back-edge jnz @0xd -> L @0x7 */
    memset(&A, 0, sizeof A);
    A.from = b + 0xd;
    A.to = b + 0x7;

    /* (1) Legitimate contiguous splice: seed [A], then window [A, A] whose splice
     * A.to(0x7) -> A.from(0xd) is the decodable loop body. Guard accepts, gap == 0. */
    {
        struct perf_branch_entry w0[1] = {A};
        struct perf_branch_entry w1[2] = {A, A}; /* newest-first */
        const struct perf_branch_entry *ss[2] = {w0, w1};
        size_t nn[2] = {1, 2};
        struct perf_branch_entry out[8];
        int gap = 0;
        size_t n = asmtest_amd_stitch(ss, nn, 2, AMD_LOOP, b, sizeof AMD_LOOP,
                                      out, 8, &gap);
        CHECK(n == 2 && gap == 0,
              "AMD stitch guard: a decodable contiguous splice is accepted");
    }

    /* (2) Corrupt splice: the second window's newest edge B has from=0x3, so the
     * splice A.to(0x7) -> B.from(0x3) is BACKWARDS — not real code. The from+to
     * overlap on A still matches (the old heuristic would append B); the guard
     * rejects it and reports an honest gap instead of a silently-wrong stitch. */
    {
        struct perf_branch_entry B;
        memset(&B, 0, sizeof B);
        B.from =
            b + 0x3; /* before A.to (0x7): a backwards, indecodable splice */
        B.to = b + 0x9;
        struct perf_branch_entry w0[1] = {A};
        struct perf_branch_entry w1[2] = {B, A}; /* newest-first: [B, A] */
        const struct perf_branch_entry *ss[2] = {w0, w1};
        size_t nn[2] = {1, 2};
        struct perf_branch_entry out[8];
        int gap = 0;
        size_t n = asmtest_amd_stitch(ss, nn, 2, AMD_LOOP, b, sizeof AMD_LOOP,
                                      out, 8, &gap);
        CHECK(gap == 1,
              "AMD stitch guard: an indecodable splice yields an honest gap");
        CHECK(
            n == 1,
            "AMD stitch guard: the rejected corrupt window is not spliced in");
    }
#else
    printf("# SKIP AMD stitch decodable-distance: not Linux x86-64\n");
#endif
}

/* §3.2 — AMD data_tail mid-capture drain: the RECONSTRUCTION half, host-testable.
 * The live drain (advancing data_tail from a consumer thread while the region runs)
 * needs Zen 3+ hardware; what CI validates is that the reconstruction of a stream
 * FAR larger than one 16-deep window / one ring's worth stitches gaplessly and
 * decodes complete — the property the live drain feeds. (The PT aux_tail circular
 * drain is the same idea for Intel PT and self-skips off PT hardware.) */
static void test_amd_drain_reconstruction(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_amd_decoder_present()) {
        printf("# SKIP AMD drain reconstruction: built without Capstone\n");
        return;
    }
    const uint64_t b = (uint64_t)(uintptr_t)AMD_LOOP;
    enum { K = 50 }; /* far past one 16-deep window and one ring's worth */
    struct perf_branch_entry full[K];
    memset(full, 0, sizeof full);
    for (int i = 0; i < K; i++) {
        full[i].from = b + 0xd;
        full[i].to = b + 0x7;
    }
    static struct perf_branch_entry
        windows[K][16]; /* static: keep off the stack */
    const struct perf_branch_entry *samples[K];
    size_t nrs[K];
    for (int j = 0; j < K; j++) {
        int depth = (j + 1 < 16) ? (j + 1) : 16;
        for (int e = 0; e < depth; e++)
            windows[j][e] = full[j - e];
        samples[j] = windows[j];
        nrs[j] = (size_t)depth;
    }
    struct perf_branch_entry stitched[K + 16];
    int gap = 0;
    size_t n = asmtest_amd_stitch(samples, nrs, K, AMD_LOOP,
                                  (uint64_t)(uintptr_t)AMD_LOOP,
                                  sizeof AMD_LOOP, stitched, K + 16, &gap);
    CHECK(n == K && gap == 0,
          "AMD drain: stitches a stream far larger than one window, gaplessly");
    asmtest_trace_t *tb = asmtest_trace_new(512, 64);
    int rc = asmtest_amd_decode_stitched(stitched, n, AMD_LOOP, sizeof AMD_LOOP,
                                         tb, gap);
    CHECK(rc == ASMTEST_HW_OK && !asmtest_emu_trace_truncated(tb),
          "AMD drain: the drained long sequence decodes complete (no ceiling)");
    CHECK(asmtest_emu_trace_insns_total(tb) == (uint64_t)(4 + (K - 1) * 3),
          "AMD drain: reconstructs every instruction of the long run");
    asmtest_trace_free(tb);
#else
    printf("# SKIP AMD drain reconstruction: not Linux x86-64\n");
#endif
}

#if defined(__linux__) && defined(__x86_64__)
struct amd_cc_arg {
    const char *name;
    long trips;
    long expect_ret;
    int ok;
};
/* One AMD-LBR-captured loop on its own thread (its own perf fd + capture slot). */
static void *amd_cc_worker(void *v) {
    struct amd_cc_arg *A = (struct amd_cc_arg *)v;
    void *p = ss_map_exec(AMD_LOOP, sizeof AMD_LOOP);
    if (p == NULL) {
        A->ok = 0;
        return NULL;
    }
    asmtest_trace_t *tr = asmtest_trace_new(256, 64);
    asmtest_hwtrace_register_region(A->name, p, sizeof AMD_LOOP, tr);
    long (*fn)(long, long) = (long (*)(long, long))p;
    /* The DETERMINISTIC per-thread proof: each thread's try_begin opens its OWN perf
     * fd and returns OK. Before the per-thread migration the process-global slot
     * refused the second thread's begin with ESTATE. (The captured branch stream
     * itself is best-effort — two concurrent sample_period=1 branch-stack events
     * multiplex on the PMU, so one can come back empty; that is a hardware property,
     * not a collision, so it is not asserted here.) */
    int armed = asmtest_hwtrace_try_begin(A->name) == ASMTEST_HW_OK;
    long r = fn(1, A->trips);
    asmtest_hwtrace_end(A->name);
    A->ok = (r == A->expect_ret) && armed;
    asmtest_trace_free(tr);
    munmap(p, sizeof AMD_LOOP);
    return NULL;
}
#endif

/* §1 (AMD): two threads each AMD-LBR-capture a DIFFERENT loop CONCURRENTLY, proving
 * the per-thread perf fd / capture slot (each thread's own hardware branch stream, no
 * collision). Validated live on a Zen 3+/4/5 host with perf branch-stack permitted —
 * the `make docker-hwtrace-amd` capped lane on this AMD box; self-skips where AMD LBR
 * is unavailable (the plain container / any Intel host). */
static void test_concurrent_amd(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_AMD_LBR)) {
        char why[160];
        asmtest_hwtrace_skip_reason(ASMTEST_HWTRACE_AMD_LBR, why, sizeof why);
        printf("# SKIP concurrent AMD: %s\n", why);
        return;
    }
#if defined(__linux__) && defined(__x86_64__)
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_AMD_LBR;
    if (asmtest_hwtrace_init(&opts) != ASMTEST_HW_OK) {
        printf("# SKIP concurrent AMD: init failed\n");
        return;
    }
    struct amd_cc_arg a = {"amd_A", 40, 40, 0};
    struct amd_cc_arg b = {"amd_B", 55, 55, 0};
    pthread_t ta, tb;
    pthread_create(&ta, NULL, amd_cc_worker, &a);
    pthread_create(&tb, NULL, amd_cc_worker, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    CHECK(a.ok,
          "concurrent AMD: thread A opened its own perf fd (per-thread slot, "
          "not refused)");
    CHECK(b.ok,
          "concurrent AMD: thread B opened its own perf fd (per-thread slot, "
          "not refused)");
    asmtest_hwtrace_shutdown();
#else
    printf("# SKIP concurrent AMD: needs Linux x86-64\n");
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
        asmtest_hwtrace_skip_reason(ASMTEST_HWTRACE_SINGLESTEP, why,
                                    sizeof why);
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
    CHECK(seq,
          "single-step yields the exact live instruction stream [0,3,6,c,11]");
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
    static const unsigned char LOOP[] = {0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00,
                                         0x00, 0x48, 0x01, 0xf8, 0x48, 0xff,
                                         0xce, 0x75, 0xf8, 0xc3};
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
          "single-step captures all 62 insns of a 20-trip loop (no depth "
          "ceiling)");
    /* {0, 0x7, 0xf}: entry, the jnz back-edge target (loop body), AND the
     * fall-through of the final NOT-taken jnz (the ret at 0xf) — the same
     * partition Unicorn/PT/DynamoRIO produce (a block ends after every branch).
     * Verified against the Unicorn emu backend, which yields exactly these three. */
    CHECK(
        asmtest_emu_trace_blocks_len(tr) == 3 && asmtest_trace_covered(tr, 0) &&
            asmtest_trace_covered(tr, 0x7) && asmtest_trace_covered(tr, 0xf),
        "single-step loop block partition {0, 0x7, 0xf} matches Unicorn/PT/DR");
    CHECK(!asmtest_emu_trace_truncated(tr),
          "single-step loop trace complete past LBR's 16-branch window");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    munmap(p, sizeof LOOP);
}

/* ------------------------------------------------------------------ */
/* Scoped-tracing shared-core §0 tests (all single-step, any x86-64    */
/* Linux — no PT/AMD hardware needed).                                  */
/* ------------------------------------------------------------------ */

/* Map ROUTINE-like bytes W^X-correctly for a single-step region; NULL on failure. */
static void *ss_map_exec(const unsigned char *bytes, size_t len) {
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    memcpy(p, bytes, len);
    mprotect(p, len, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + len);
    return p;
}

/* B (lazy-arm) — managed-singlestep-lazy-arm-plan §B1. The managed-SAFE call scope:
 * asmtest_hwtrace_call_scoped arms, calls the region fn, and disarms in ONE native step
 * (nothing the caller runs straddles the window). Over a native leaf — no managed
 * runtime needed — it must (a) return fn's value and (b) record the SAME body offsets
 * as the plain begin/end path BYTE-FOR-BYTE: the stream-parity invariant the .NET
 * Invoke rewire must preserve. Also asserts the guardrails that make the caller fall
 * back rather than mis-dispatch (unknown region, arity > 6). */
static void test_call_scoped(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP call_scoped: single-step unavailable\n");
        return;
    }
    void *p = ss_map_exec(ROUTINE, sizeof ROUTINE);
    if (p == NULL) {
        printf("# SKIP call_scoped: mmap failed\n");
        return;
    }
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK, "call_scoped init");

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    CHECK(asmtest_hwtrace_register_region("csadd2", p, sizeof ROUTINE, tr) ==
              ASMTEST_HW_OK,
          "call_scoped register region");

    /* Arm + call add2(20,22) + disarm, all in native code. */
    const long args[2] = {20, 22};
    long result = -1;
    asmtest_hwtrace_scope_t sc;
    int rc = asmtest_hwtrace_call_scoped("csadd2", p, args, 2, &result, &sc);
    CHECK(rc == ASMTEST_HW_OK, "call_scoped returns OK");
    CHECK(result == 42, "call_scoped returns fn's value (20+22)");

    /* Byte-for-byte parity with test_singlestep_live's begin/end trace. */
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int seq = (asmtest_emu_trace_insns_total(tr) == 5);
    for (size_t i = 0; seq && i < 5; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq, "call_scoped records the exact body stream [0,3,6,c,11] "
               "(byte-for-byte parity with begin/end)");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2,
          "call_scoped records exactly two blocks");
    CHECK(!asmtest_emu_trace_truncated(tr),
          "call_scoped trace is complete (not truncated)");

    /* The handle renders the closed slice (the render-on-close path bindings use). */
    CHECK(asmtest_hwtrace_render_scope(sc, NULL, 0) > 0,
          "call_scoped handle renders the closed slice");

    /* Guardrails: unknown region + unsupported arity both reject cleanly so the caller
     * falls back to the out-of-process stepper rather than mis-dispatching. */
    CHECK(asmtest_hwtrace_call_scoped("nope", p, args, 2, &result, &sc) ==
              ASMTEST_HW_EINVAL,
          "call_scoped rejects an unregistered region");
    CHECK(asmtest_hwtrace_call_scoped("csadd2", p, args, 7, &result, &sc) ==
              ASMTEST_HW_EINVAL,
          "call_scoped rejects arity > 6 (caller falls back out-of-process)");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    munmap(p, sizeof ROUTINE);
}

/* B (lazy-arm) FP shim family: asmtest_hwtrace_call_scoped_fp dispatches a
 * (double…)->double body through the SysV FP ABI (xmm0..7 args, xmm0 return) — the
 * signature family the integer shim set can't express. Over a native double-add leaf
 * (addsd xmm0,xmm1; ret) it must return the FP result and record the body offsets. */
static void test_call_scoped_fp(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP call_scoped_fp: single-step unavailable\n");
        return;
    }
    /* addsd xmm0, xmm1 (F2 0F 58 C1) ; ret (C3) — double add2d(a,b)=a+b. */
    static const unsigned char DADD[] = {0xf2, 0x0f, 0x58, 0xc1, 0xc3};
    void *p = ss_map_exec(DADD, sizeof DADD);
    if (p == NULL) {
        printf("# SKIP call_scoped_fp: mmap failed\n");
        return;
    }
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK, "call_scoped_fp init");

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    CHECK(asmtest_hwtrace_register_region("csdadd", p, sizeof DADD, tr) ==
              ASMTEST_HW_OK,
          "call_scoped_fp register region");

    const double fargs[2] = {1.5, 2.5};
    double dresult = -1.0;
    asmtest_hwtrace_scope_t sc;
    int rc =
        asmtest_hwtrace_call_scoped_fp("csdadd", p, fargs, 2, &dresult, &sc);
    CHECK(rc == ASMTEST_HW_OK, "call_scoped_fp returns OK");
    CHECK(dresult == 4.0, "call_scoped_fp returns fn's FP value (1.5+2.5)");

    /* addsd at 0, ret at 4 — the exact executed body, no straddling caller insns. */
    static const uint64_t EXPECT[] = {0x0, 0x4};
    int seq = (asmtest_emu_trace_insns_total(tr) == 2);
    for (size_t i = 0; seq && i < 2; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq, "call_scoped_fp records the exact FP body stream [0,4]");
    CHECK(!asmtest_emu_trace_truncated(tr),
          "call_scoped_fp trace is complete (not truncated)");
    CHECK(
        asmtest_hwtrace_call_scoped_fp("csdadd", p, fargs, 9, &dresult, &sc) ==
            ASMTEST_HW_EINVAL,
        "call_scoped_fp rejects arity > 8 (caller falls back out-of-process)");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    munmap(p, sizeof DADD);
}

/* Registry-free lazy-arm: asmtest_hwtrace_call_scoped_ex takes [base,len) directly, so it
 * consumes NO region-registry slot — a high-churn caller (the §D0.4 async-hop stitching
 * producer) can capture far more than MAX_REGIONS(=32) one-shot bodies without exhausting
 * the table. Assert one call is correct + renders by handle, then run 64 back-to-back
 * calls (2x the registry ceiling) and require every one to arm and record. */
static void test_call_scoped_ex(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP call_scoped_ex: single-step unavailable\n");
        return;
    }
    void *p = ss_map_exec(ROUTINE, sizeof ROUTINE);
    if (p == NULL) {
        printf("# SKIP call_scoped_ex: mmap failed\n");
        return;
    }
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    asmtest_hwtrace_init(&opts);

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    const long args[2] = {20, 22};
    long r = 0;
    asmtest_hwtrace_scope_t sc;
    int rc = asmtest_hwtrace_call_scoped_ex(p, sizeof ROUTINE, tr, p, args, 2,
                                            &r, &sc);
    CHECK(rc == ASMTEST_HW_OK && r == 42,
          "call_scoped_ex: registry-free call returns 42");
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int seq = (asmtest_emu_trace_insns_total(tr) == 5);
    for (size_t i = 0; seq && i < 5; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq, "call_scoped_ex: records the exact body stream [0,3,6,c,11]");
    CHECK(asmtest_hwtrace_render_scope(sc, NULL, 0) > 0,
          "call_scoped_ex: handle renders on the capturing thread");
    asmtest_trace_free(tr);

    /* 64 back-to-back registry-free captures (2x MAX_REGIONS) — none may EFULL. */
    int all_ok = 1;
    for (int i = 0; i < 64; i++) {
        asmtest_trace_t *t = asmtest_trace_new(64, 64);
        long ri = 0;
        asmtest_hwtrace_scope_t sci;
        int rci = asmtest_hwtrace_call_scoped_ex(p, sizeof ROUTINE, t, p, args,
                                                 2, &ri, &sci);
        if (rci != ASMTEST_HW_OK || ri != 42 ||
            asmtest_emu_trace_insns_total(t) != 5)
            all_ok = 0;
        asmtest_trace_free(t);
    }
    CHECK(all_ok, "call_scoped_ex: 64 registry-free calls all arm (no "
                  "MAX_REGIONS exhaustion)");

    asmtest_hwtrace_shutdown();
    munmap(p, sizeof ROUTINE);
}

/* §D0.4 live-producer bridge: asmtest_hwtrace_stitch_handles takes N real captured
 * trace HANDLES (one per async hop) + parallel (scope_id, seq, tid, version) arrays
 * and merges them in SEQ order (not capture order). Capture two live single-step
 * traces of the same leaf on DIFFERENT arg paths (5 insns vs 6), hand them out of
 * capture order, and assert the merge orders by seq, the bounds mark each slice's
 * start, and the (tid, version) ride through. This is the first LIVE exercise of the
 * stitch core (previously host-tested only from synthetic slices). */
static void test_stitch_handles(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP stitch_handles: single-step unavailable\n");
        return;
    }
    void *p = ss_map_exec(ROUTINE, sizeof ROUTINE);
    if (p == NULL) {
        printf("# SKIP stitch_handles: mmap failed\n");
        return;
    }
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    asmtest_hwtrace_init(&opts);

    /* Hop A: add2(20,22)=42 → jle taken → 5 insns {0,3,6,c,11}. */
    asmtest_trace_t *trA = asmtest_trace_new(64, 64);
    asmtest_hwtrace_register_region("hop", p, sizeof ROUTINE, trA);
    const long argsA[2] = {20, 22};
    long rA = 0;
    asmtest_hwtrace_scope_t scA;
    asmtest_hwtrace_call_scoped("hop", p, argsA, 2, &rA, &scA);
    /* Hop B: add2(60,60): sum 120 > 100 → jle NOT taken → dec runs → returns 119,
     * 6 insns {0,3,6,c,e,11} (the taken-path A skips the dec at 0xe). */
    asmtest_trace_t *trB = asmtest_trace_new(64, 64);
    asmtest_hwtrace_register_region("hop", p, sizeof ROUTINE, trB);
    const long argsB[2] = {60, 60};
    long rB = 0;
    asmtest_hwtrace_scope_t scB;
    asmtest_hwtrace_call_scoped("hop", p, argsB, 2, &rB, &scB);
    CHECK(rA == 42 && rB == 119, "stitch_handles: both hops captured live");

    /* Hand them OUT of seq order (B first, but B is seq 1) to prove seq-ordering. */
    const asmtest_trace_t *traces[2] = {trB, trA};
    const uint64_t scope_ids[2] = {0x51, 0x51};
    const uint32_t seqs[2] = {1, 0};
    const int tids[2] = {701, 700};
    const uint64_t versions[2] = {9, 9};
    asmtest_trace_t *merged = asmtest_trace_new(64, 64);
    asmtest_hwtrace_slice_bound_t bounds[2];
    size_t nb = 0;
    int rc = asmtest_hwtrace_stitch_handles(traces, scope_ids, seqs, tids,
                                            versions, 2, merged, bounds, &nb);
    CHECK(rc == ASMTEST_HW_OK && nb == 2, "stitch_handles: merged 2 slices");
    /* Seq order is A(seq0,5 insns) then B(seq1,6 insns) = 11 total, A's stream first. */
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11, 0x0,
                                      0x3, 0x6, 0xc, 0xe, 0x11};
    int ok = (asmtest_emu_trace_insns_total(merged) == 11);
    for (size_t i = 0; ok && i < 11; i++)
        ok = (merged->insns[i] == EXPECT[i]);
    CHECK(ok, "stitch_handles: merged stream is seq-ordered (hop0 then hop1)");
    CHECK(bounds[0].insn_off == 0 && bounds[0].seq == 0 &&
              bounds[0].tid == 700 && bounds[1].insn_off == 5 &&
              bounds[1].seq == 1 && bounds[1].tid == 701,
          "stitch_handles: bounds mark each slice's start + carry (seq,tid)");
    CHECK(bounds[0].scope_id == 0x51 && bounds[0].version == 9,
          "stitch_handles: bounds carry scope_id + version through");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(trA);
    asmtest_trace_free(trB);
    asmtest_trace_free(merged);
    munmap(p, sizeof ROUTINE);
}

/* §0.1/§1 — try_begin returns EINVAL on an unregistered name; under §1 a second
 * same-thread begin COMPOSES (per-thread range stack), so the refusal moves from
 * ESTATE-on-busy to EFULL when this thread's range stack is full. */
static void test_try_begin_busy(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf(
            "# SKIP try_begin nesting/unregistered: single-step unavailable\n");
        return;
    }
    void *p = ss_map_exec(ROUTINE, sizeof ROUTINE);
    if (p == NULL) {
        printf("# SKIP try_begin nesting: mmap failed\n");
        return;
    }
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    asmtest_hwtrace_init(&opts);
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    asmtest_hwtrace_register_region("add2", p, sizeof ROUTINE, tr);

    /* Unregistered name -> EINVAL (no arming happens). */
    int rc_unreg = asmtest_hwtrace_try_begin("does-not-exist");

    /* §1: nested same-thread begins compose (OK); fill the range stack to its depth
     * bound to provoke EFULL. Keep this window printf-free — TF is armed. */
    int rc_first = asmtest_hwtrace_try_begin("add2");
    int rc_second = asmtest_hwtrace_try_begin("add2");
    int pushes = 2, rc = ASMTEST_HW_OK;
    while (pushes < 64) {
        rc = asmtest_hwtrace_try_begin("add2");
        if (rc != ASMTEST_HW_OK)
            break;
        pushes++;
    }
    int full_rc = rc;
    for (int i = 0; i < pushes; i++)
        asmtest_hwtrace_end("add2"); /* unwind every pushed frame (LIFO) */

    CHECK(rc_unreg == ASMTEST_HW_EINVAL,
          "try_begin on an unregistered name returns EINVAL");
    CHECK(rc_first == ASMTEST_HW_OK && rc_second == ASMTEST_HW_OK,
          "nested same-thread begins compose (return OK), not ESTATE");
    CHECK(full_rc == ASMTEST_HW_EFULL,
          "a full per-thread range stack returns EFULL (the §1 refusal)");
    CHECK(pushes >= 2 && pushes <= 32,
          "the range stack composes several frames before EFULL");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    munmap(p, sizeof ROUTINE);
}

#if defined(__linux__) && defined(__x86_64__)
static volatile int
    g_amt_go; /* main -> closing thread: main has armed, proceed */

/* Close "add2" on THIS spawned thread once main has armed (on main). The close runs
 * on the wrong thread, so the region's arming-tid backstop flags the trace
 * truncated; main then closes its OWN frame to disarm TF cleanly (no leak, no
 * cross-thread TF hazard — main never left another thread stepping). */
static void *close_on_thread(void *arg) {
    (void)arg;
    while (!g_amt_go) {
    } /* wait until main has armed (main sets the region's arm_tid) */
    asmtest_hwtrace_end("add2"); /* cross-thread close: flags truncated */
    return NULL;
}
#endif

/* §0.2/§1 — a close on a different OS thread than begin flags the trace truncated
 * (never emit a cross-thread partial as complete), and arm_tid reports the arming
 * thread. Under §1 the arming thread owns the TF/frame, so main arms and disarms
 * itself; a spawned thread performs the cross-thread close that trips the backstop. */
static void test_arm_tid_mismatch(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP arm-tid mismatch: single-step unavailable\n");
        return;
    }
#if defined(__linux__) && defined(__x86_64__)
    void *p = ss_map_exec(ROUTINE, sizeof ROUTINE);
    if (p == NULL) {
        printf("# SKIP arm-tid mismatch: mmap failed\n");
        return;
    }
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    asmtest_hwtrace_init(&opts);
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    asmtest_hwtrace_register_region("add2", p, sizeof ROUTINE, tr);

    g_amt_go = 0;
    pthread_t th;
    int rc = pthread_create(&th, NULL, close_on_thread, NULL);
    CHECK(rc == 0, "spawn closing thread");

    asmtest_hwtrace_scope_t sc;
    int b = asmtest_hwtrace_begin_scope("add2", &sc); /* arm on MAIN */
    int armtid = asmtest_hwtrace_arm_tid();
    g_amt_go = 1; /* release the spawned thread to close cross-thread */
    pthread_join(th, NULL); /* spawned thread closed on the wrong thread */
    asmtest_hwtrace_end(
        "add2"); /* main closes its own frame (disarms TF, cleanup) */

    CHECK(b == ASMTEST_HW_OK, "begin_scope arms on the main thread");
    CHECK(armtid == (int)syscall(SYS_gettid),
          "arm_tid reports the arming (main) thread id");
    CHECK(asmtest_emu_trace_truncated(tr),
          "cross-thread close flags the trace truncated (never "
          "partial-as-complete)");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    munmap(p, sizeof ROUTINE);
#else
    printf("# SKIP arm-tid mismatch: needs Linux threads\n");
#endif
}

/* §0.3 — render a single-step-traced native leaf to disassembly text and assert it
 * matches a ground-truth asmtest_disas of the same bytes, plus the snprintf
 * size-then-allocate idiom and the name-miss / no-Capstone error convention. */
static void test_render_singlestep(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP render-on-close: single-step unavailable\n");
        return;
    }
    void *p = ss_map_exec(ROUTINE, sizeof ROUTINE);
    if (p == NULL) {
        printf("# SKIP render-on-close: mmap failed\n");
        return;
    }
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    asmtest_hwtrace_init(&opts);
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    asmtest_hwtrace_register_region("add2", p, sizeof ROUTINE, tr);

    add2_fn fn = (add2_fn)p;
    asmtest_hwtrace_begin("add2");
    long r = fn(20, 22);
    asmtest_hwtrace_end("add2");
    CHECK(r == 42, "render fixture: traced call returns 20+22");

    /* Size (buf=NULL,buflen=0), allocate, render, and confirm the two agree. */
    int need = asmtest_hwtrace_render("add2", NULL, 0);
    CHECK(need > 0, "render sizing (buf=NULL) returns a positive length");
    char *buf = (char *)malloc((size_t)need + 1);
    int wrote = asmtest_hwtrace_render("add2", buf, (size_t)need + 1);
    CHECK(wrote == need, "render into a sized buffer returns the same length");
    CHECK(buf[0] != '\0' && strlen(buf) == (size_t)need,
          "rendered text is NUL-terminated and fully written");

    /* Ground truth: the first executed instruction (offset 0) disassembled from the
     * same bytes must appear verbatim in the rendered listing. */
    char gtxt[128];
    asmtest_disas(ASMTEST_ARCH_X86_64, (const uint8_t *)p, sizeof ROUTINE,
                  (uint64_t)(uintptr_t)p, 0, gtxt, sizeof gtxt);
    CHECK(gtxt[0] != '\0' && strstr(buf, gtxt) != NULL,
          "rendered text contains the ground-truth disassembly of insn 0");

    /* Error convention: a name miss is a NEGATIVE code, distinct from any length. */
    CHECK(asmtest_hwtrace_render("no-such-region", NULL, 0) ==
              ASMTEST_HW_EINVAL,
          "render of an unregistered name returns EINVAL");

    /* A short buffer truncates but stays NUL-terminated, and still reports the full
     * length that WOULD be written (snprintf semantics). */
    char small[8];
    int full = asmtest_hwtrace_render("add2", small, sizeof small);
    CHECK(full == need && strlen(small) < sizeof small,
          "short buffer truncates safely and returns the full would-be length");

    free(buf);
    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    munmap(p, sizeof ROUTINE);
}

/* §0.4 — register_region is idempotent by name: a repeat registration refreshes
 * the SAME slot (its trace pointer) in place rather than appending, so a scope
 * object that registers on every construction reuses one slot; the MAX_REGIONS
 * ceiling then counts DISTINCT names, and overflowing it returns EFULL. */
static void test_register_idempotent(void) {
    if (!asmtest_disas_available()) {
        printf(
            "# SKIP register-idempotent: needs Capstone (single-step floor)\n");
        return;
    }
    void *p = ss_map_exec(ROUTINE, sizeof ROUTINE);
    if (p == NULL) {
        printf("# SKIP register-idempotent: mmap failed\n");
        return;
    }
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;

    /* Part 1: a second registration under the same name refreshes the slot's trace
     * pointer in place. Register "add2" with tr1, then again with tr2; run it. If
     * the second call APPENDED a duplicate, find_region would resolve the FIRST
     * (tr1); idempotent-refresh means the SECOND (tr2) is what fills. */
    asmtest_hwtrace_init(&opts);
    asmtest_trace_t *tr1 = asmtest_trace_new(64, 64);
    asmtest_trace_t *tr2 = asmtest_trace_new(64, 64);
    int r1 = asmtest_hwtrace_register_region("add2", p, sizeof ROUTINE, tr1);
    int r2 = asmtest_hwtrace_register_region("add2", p, sizeof ROUTINE, tr2);
    CHECK(r1 == ASMTEST_HW_OK && r2 == ASMTEST_HW_OK,
          "re-registering the same name succeeds both times");
    add2_fn fn = (add2_fn)p;
    asmtest_hwtrace_begin("add2");
    (void)fn(20, 22);
    asmtest_hwtrace_end("add2");
    CHECK(
        asmtest_emu_trace_insns_total(tr2) == 5 &&
            asmtest_emu_trace_insns_total(tr1) == 0,
        "idempotent register refreshes the SAME slot in place (tr2 fills, tr1 "
        "does not)");
    asmtest_hwtrace_shutdown();

    /* Part 2: the ceiling counts DISTINCT names. Register one name five times
     * (idempotent: one slot), then distinct names until EFULL. If dedup worked, the
     * five same-name calls consumed a single slot, so the remaining distinct
     * capacity is MAX_REGIONS - 1. (MAX_REGIONS is 32 in src/hwtrace.c.) */
    asmtest_hwtrace_init(&opts);
    int dup_ok = 1;
    for (int i = 0; i < 5; i++)
        if (asmtest_hwtrace_register_region("dup", p, sizeof ROUTINE, tr1) !=
            ASMTEST_HW_OK)
            dup_ok = 0;
    CHECK(dup_ok, "five registrations of one name all succeed (idempotent)");
    int distinct = 0, last_rc = ASMTEST_HW_OK;
    for (int i = 0; i < 64; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "reg_%d", i);
        last_rc = asmtest_hwtrace_register_region(nm, p, sizeof ROUTINE, tr1);
        if (last_rc != ASMTEST_HW_OK)
            break;
        distinct++;
    }
    CHECK(distinct == 31,
          "one deduped name + 31 distinct names fill the 32-slot table");
    CHECK(last_rc == ASMTEST_HW_EFULL,
          "registering past MAX_REGIONS distinct names returns EFULL");
    asmtest_hwtrace_shutdown();

    asmtest_trace_free(tr1);
    asmtest_trace_free(tr2);
    munmap(p, sizeof ROUTINE);
}

/* ------------------------------------------------------------------ */
/* §1 — per-thread single-step: nesting + concurrency (any x86-64 Linux) */
/* ------------------------------------------------------------------ */

/* §1 — two nested begin/end pairs on ONE thread over the same routine: an outer
 * frame spanning the whole routine and an inner frame over a sub-range. The inner
 * region's offsets must be a subset of the outer's, and both frames close complete. */
static void test_nested_singlestep(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP nested single-step: single-step unavailable\n");
        return;
    }
#if defined(__linux__) && defined(__x86_64__)
    void *p = ss_map_exec(ROUTINE, sizeof ROUTINE);
    if (p == NULL) {
        printf("# SKIP nested single-step: mmap failed\n");
        return;
    }
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    asmtest_hwtrace_init(&opts);
    asmtest_trace_t *tr_out = asmtest_trace_new(64, 64);
    asmtest_trace_t *tr_in = asmtest_trace_new(64, 64);
    asmtest_hwtrace_register_region("nest_outer", p, sizeof ROUTINE, tr_out);
    asmtest_hwtrace_register_region("nest_inner", p, 12, tr_in); /* [0,0xc) */

    add2_fn fn = (add2_fn)p;
    asmtest_hwtrace_begin("nest_outer");
    asmtest_hwtrace_begin("nest_inner"); /* composes (nested frame) */
    long r = fn(20, 22);
    asmtest_hwtrace_end("nest_inner"); /* LIFO: pop the inner frame first */
    asmtest_hwtrace_end("nest_outer");

    CHECK(r == 42, "nested: traced call returns 42");
    CHECK(asmtest_emu_trace_insns_total(tr_out) == 5,
          "nested: outer frame captures the full routine (5 insns)");
    CHECK(asmtest_emu_trace_insns_total(tr_in) == 3,
          "nested: inner sub-range [0,0xc) captures its subset (3 insns)");
    int subset = (tr_in->insns_len == 3 && tr_in->insns[0] == 0 &&
                  tr_in->insns[1] == 3 && tr_in->insns[2] == 6);
    CHECK(subset,
          "nested: inner offsets {0,3,6} are a subset of outer {0,3,6,c,11}");
    CHECK(!asmtest_emu_trace_truncated(tr_out) &&
              !asmtest_emu_trace_truncated(tr_in),
          "nested: both frames close complete (not truncated)");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr_out);
    asmtest_trace_free(tr_in);
    munmap(p, sizeof ROUTINE);
#else
    printf("# SKIP nested single-step: needs Linux x86-64\n");
#endif
}

#if defined(__linux__) && defined(__x86_64__)
struct ss_cc_arg {
    const char *name;      /* fixed name (concurrent) or ignored (samename) */
    long a0, a1;           /* args to add2 */
    long expect_ret;       /* expected return */
    uint64_t expect_total; /* expected in-region insns */
    int use_handle; /* samename: tid-disambiguate + render via the handle */
    int ok;
};

/* Worker: map its OWN copy of add2, register under a distinct (or tid-disambiguated)
 * name, single-step-trace one call, and self-check its own trace — proving per-thread
 * TLS isolation (two threads single-stepping concurrently, neither trips the other). */
static void *ss_cc_worker(void *v) {
    struct ss_cc_arg *A = (struct ss_cc_arg *)v;
    void *p = ss_map_exec(ROUTINE, sizeof ROUTINE);
    if (p == NULL) {
        A->ok = 0;
        return NULL;
    }
    char nm[64];
    if (A->use_handle)
        snprintf(nm, sizeof nm, "site_%ld", (long)syscall(SYS_gettid));
    else
        snprintf(nm, sizeof nm, "%s", A->name);
    asmtest_trace_t *tr = asmtest_trace_new(256, 64);
    asmtest_hwtrace_register_region(nm, p, sizeof ROUTINE, tr);
    long (*fn)(long, long) = (long (*)(long, long))p;
    int ok;
    if (A->use_handle) {
        asmtest_hwtrace_scope_t sc;
        asmtest_hwtrace_begin_scope(nm, &sc);
        long r = fn(A->a0, A->a1);
        asmtest_hwtrace_end(nm);
        char buf[1024];
        int n = asmtest_hwtrace_render_scope(sc, buf, sizeof buf);
        int lines = 0;
        for (int i = 0; i < n && buf[i] != '\0'; i++)
            if (buf[i] == '\n')
                lines++;
        ok = (r == A->expect_ret) && (n > 0) &&
             (asmtest_emu_trace_insns_total(tr) == A->expect_total) &&
             ((uint64_t)lines == A->expect_total);
    } else {
        asmtest_hwtrace_begin(nm);
        long r = fn(A->a0, A->a1);
        asmtest_hwtrace_end(nm);
        ok = (r == A->expect_ret) &&
             (asmtest_emu_trace_insns_total(tr) == A->expect_total) &&
             !asmtest_emu_trace_truncated(tr);
    }
    A->ok = ok;
    asmtest_trace_free(tr);
    munmap(p, sizeof ROUTINE);
    return NULL;
}
#endif

/* §1 — two threads each single-step-trace a DIFFERENT invocation concurrently; each
 * gets its own complete trace and neither trips the other. This is the regression
 * test for the flaky-crash class the Go binding hit (single-step TF is per-thread). */
static void test_concurrent_singlestep(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP concurrent single-step: single-step unavailable\n");
        return;
    }
#if defined(__linux__) && defined(__x86_64__)
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    asmtest_hwtrace_init(&opts);
    /* add2(20,22)=42 takes the jle -> 5 insns; add2(200,50)=249 falls through the
     * dec -> 6 insns. Different traces prove no cross-thread aliasing. */
    struct ss_cc_arg a = {"cc_A", 20, 22, 42, 5, 0, 0};
    struct ss_cc_arg b = {"cc_B", 200, 50, 249, 6, 0, 0};
    pthread_t ta, tb;
    pthread_create(&ta, NULL, ss_cc_worker, &a);
    pthread_create(&tb, NULL, ss_cc_worker, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    CHECK(a.ok, "concurrent: thread A gets its own complete 5-insn trace");
    CHECK(b.ok, "concurrent: thread B gets its own complete 6-insn trace");
    asmtest_hwtrace_shutdown();
#else
    printf("# SKIP concurrent single-step: needs Linux x86-64\n");
#endif
}

/* §1 — two threads scope the SAME call-site (tid-disambiguated names, the shim
 * model) concurrently and render via the per-scope HANDLE. Each thread's render
 * returns its OWN slice (5 vs 6 lines), proving per-scope trace ownership with no
 * cross-thread aliasing. */
static void test_concurrent_samename(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP concurrent same-site: single-step unavailable\n");
        return;
    }
#if defined(__linux__) && defined(__x86_64__)
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    asmtest_hwtrace_init(&opts);
    struct ss_cc_arg a = {NULL, 20, 22, 42, 5, 1, 0};
    struct ss_cc_arg b = {NULL, 200, 50, 249, 6, 1, 0};
    pthread_t ta, tb;
    pthread_create(&ta, NULL, ss_cc_worker, &a);
    pthread_create(&tb, NULL, ss_cc_worker, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    CHECK(a.ok,
          "same-site: thread A's handle render returns its own 5-insn slice");
    CHECK(b.ok,
          "same-site: thread B's handle render returns its own 6-insn slice");
    asmtest_hwtrace_shutdown();
#else
    printf("# SKIP concurrent same-site: needs Linux x86-64\n");
#endif
}

/* §1 — version-aware render: disassemble a trace of ABSOLUTE addresses against the
 * code-image version live as of `when`, so tiered/moved managed bytes render the
 * bytes that ran (not stale text). Host-testable via the codeimage recorder. */
static void test_render_versioned(void) {
#if defined(__linux__)
    if (!asmtest_codeimage_available() || !asmtest_disas_available()) {
        printf("# SKIP render-versioned: codeimage/Capstone unavailable\n");
        return;
    }
    static const unsigned char CIA[] = {0x48, 0x89, 0xf8, 0x48,
                                        0x01, 0xf0, 0xc3}; /* add */
    static const unsigned char CIB[] = {0x48, 0x89, 0xf8, 0x48,
                                        0x29, 0xf0, 0xc3}; /* sub */
    long ps = sysconf(_SC_PAGESIZE);
    unsigned char *p =
        (unsigned char *)mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP render-versioned: mmap failed\n");
        return;
    }
    memcpy(p, CIA, sizeof CIA);
    asmtest_codeimage_t *img = asmtest_codeimage_new(0);
    asmtest_codeimage_track(img, p, sizeof CIA);
    uint64_t t0 = asmtest_codeimage_now(img);
    memcpy(p, CIB, sizeof CIB);
    asmtest_codeimage_refresh(img);
    uint64_t t1 = asmtest_codeimage_now(img);

    /* A trace holding the ABSOLUTE address of the add/sub instruction (offset 3). */
    asmtest_trace_t *tr = asmtest_trace_new(4, 4);
    trace_append_insn(tr, (uint64_t)(uintptr_t)(p + 3));
    char b0[256], b1[256];
    int n0 = asmtest_hwtrace_render_versioned(img, t0, tr, b0, sizeof b0);
    int n1 = asmtest_hwtrace_render_versioned(img, t1, tr, b1, sizeof b1);
    CHECK(n0 > 0 && strstr(b0, "add") != NULL,
          "render_versioned at t0 shows add (version A bytes)");
    CHECK(n1 > 0 && strstr(b1, "sub") != NULL,
          "render_versioned at t1 shows sub (version B bytes)");
    CHECK(strcmp(b0, b1) != 0,
          "render_versioned is version-aware (t0 text != t1 text)");

    asmtest_trace_free(tr);
    asmtest_codeimage_free(img);
    munmap(p, (size_t)ps);
#else
    printf("# SKIP render-versioned: not Linux\n");
#endif
}

/* §D4 — the async-hop stitching merge core. Backend-independent: synthetic
 * pre-decoded slices with monotonic seq + distinct versions, no PT hardware and no
 * real threads (the merge is a pure ordered concatenation). This is the CI-runnable
 * deliverable that closes the async-hop test hole. */
static void test_stitch_slices(void) {
    asmtest_trace_t *a = asmtest_trace_new(16, 16);
    asmtest_trace_t *b = asmtest_trace_new(16, 16);
    /* slice A (seq 0): offsets {0,3,6}; slice B (seq 1): offsets {0,4,8}. */
    trace_append_insn(a, 0);
    trace_append_insn(a, 3);
    trace_append_insn(a, 6);
    trace_append_block(a, 0);
    trace_append_insn(b, 0);
    trace_append_insn(b, 4);
    trace_append_insn(b, 8);
    trace_append_block(b, 0);

    asmtest_hwtrace_slice_t slices[2];
    memset(slices, 0, sizeof slices);
    /* Deliberately pass them OUT of seq order to prove stitch sorts by seq. */
    slices[0].scope_id = 7;
    slices[0].seq = 1;
    slices[0].tid = 222;
    slices[0].version = 9;
    slices[0].trace = *b;
    slices[1].scope_id = 7;
    slices[1].seq = 0;
    slices[1].tid = 111;
    slices[1].version = 5;
    slices[1].trace = *a;

    asmtest_trace_t *out = asmtest_trace_new(64, 64);
    asmtest_hwtrace_slice_bound_t bounds[2];
    size_t nb = 0;
    int rc = asmtest_hwtrace_stitch(slices, 2, out, bounds, &nb);
    CHECK(rc == ASMTEST_HW_OK && nb == 2, "stitch merges two slices");

    static const uint64_t EXP[] = {0, 3, 6, 0, 4, 8};
    int seq_ok = (asmtest_emu_trace_insns_len(out) == 6);
    for (size_t i = 0; seq_ok && i < 6; i++)
        seq_ok = (out->insns[i] == EXP[i]);
    CHECK(seq_ok, "stitch concatenates slices in seq order (A then B)");
    CHECK(bounds[0].seq == 0 && bounds[0].insn_off == 0 &&
              bounds[0].tid == 111 && bounds[0].version == 5,
          "stitch bounds[0] attributes the seq-0 run (tid 111, ver 5) at "
          "offset 0");
    CHECK(bounds[1].seq == 1 && bounds[1].insn_off == 3 &&
              bounds[1].tid == 222 && bounds[1].version == 9,
          "stitch bounds[1] attributes the seq-1 run (tid 222, ver 9) at "
          "offset 3");

    /* Degenerate single-slice case: identical output to a plain trace. */
    asmtest_trace_t *out1 = asmtest_trace_new(16, 16);
    size_t nb1 = 0;
    asmtest_hwtrace_stitch(&slices[1], 1, out1, NULL, &nb1);
    CHECK(nb1 == 1 && asmtest_emu_trace_insns_len(out1) == 3 &&
              out1->insns[0] == 0 && out1->insns[2] == 6,
          "stitch single-slice degenerate case equals the plain trace");

    asmtest_trace_free(a);
    asmtest_trace_free(b);
    asmtest_trace_free(out);
    asmtest_trace_free(out1);
}

/* §2 — the recorder-backed PT image adapter (host-testable half; no PT hardware, no
 * libipt). Build a codeimage with two versions of the bytes at one address and drive
 * the adapter at two `when` values: it must serve the version live THEN (the
 * temporal-bytes rule), and asmtest_disas of the served bytes must match ground
 * truth. This is the same recorder-backed image callback libipt would call, exercised
 * directly (end-to-end libipt decode is forward-look on real Intel PT). */
static void test_pt_image_from_codeimage(void) {
#if defined(__linux__)
    if (!asmtest_codeimage_available()) {
        char why[200];
        asmtest_codeimage_skip_reason(why, sizeof why);
        printf("# SKIP pt image-from-codeimage: %s\n", why);
        return;
    }
    /* A: add rax,rsi ; B: sub rax,rsi — differ at one byte (the insn at offset 3). */
    static const unsigned char CIA[] = {0x48, 0x89, 0xf8, 0x48,
                                        0x01, 0xf0, 0xc3};
    static const unsigned char CIB[] = {0x48, 0x89, 0xf8, 0x48,
                                        0x29, 0xf0, 0xc3};
    long ps = sysconf(_SC_PAGESIZE);
    unsigned char *p =
        (unsigned char *)mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP pt image-from-codeimage: mmap failed\n");
        return;
    }
    memcpy(p, CIA, sizeof CIA);
    asmtest_codeimage_t *img = asmtest_codeimage_new(0);
    asmtest_codeimage_track(img, p, sizeof CIA);
    uint64_t t0 = asmtest_codeimage_now(img);
    memcpy(p, CIB, sizeof CIB); /* re-JIT in place: same address, new bytes */
    asmtest_codeimage_refresh(img);
    uint64_t t1 = asmtest_codeimage_now(img);

    uint64_t ip = (uint64_t)(uintptr_t)p;
    uint8_t b0[32], b1[32];
    int n0 = asmtest_pt_read_codeimage(img, t0, ip, b0, sizeof b0);
    int n1 = asmtest_pt_read_codeimage(img, t1, ip, b1, sizeof b1);
    CHECK(n0 == (int)sizeof CIA && memcmp(b0, CIA, sizeof CIA) == 0,
          "pt adapter serves the version live at t0 (bytes A)");
    CHECK(n1 == (int)sizeof CIB && memcmp(b1, CIB, sizeof CIB) == 0,
          "pt adapter serves the version live at t1 (bytes B)");

    /* Disassembling the served bytes matches the ground-truth disas of the source,
     * and A (add) vs B (sub) at offset 3 differ — the temporal-bytes proof. */
    char da[64], ga[64], db[64];
    asmtest_disas(ASMTEST_ARCH_X86_64, b0, sizeof CIA, ip, 3, da, sizeof da);
    asmtest_disas(ASMTEST_ARCH_X86_64, CIA, sizeof CIA, ip, 3, ga, sizeof ga);
    asmtest_disas(ASMTEST_ARCH_X86_64, b1, sizeof CIB, ip, 3, db, sizeof db);
    CHECK(da[0] != '\0' && strcmp(da, ga) == 0,
          "asmtest_disas of adapter bytes matches ground truth (t0)");
    CHECK(db[0] != '\0' && strcmp(da, db) != 0,
          "t0 (add) and t1 (sub) disassemble differently — decode is "
          "version-correct");

    CHECK(asmtest_pt_read_codeimage(img, 0, ip + (uint64_t)ps, b0, sizeof b0) <
              0,
          "pt adapter miss on an untracked address returns negative");

    asmtest_codeimage_free(img);
    munmap(p, (size_t)ps);
#else
    printf("# SKIP pt image-from-codeimage: not Linux\n");
#endif
}

/* §3.1(c) — whole-window noise attribution: the address→name reverse resolver +
 * IP bucketer. Feed a synthetic IP list spanning three distinct "regions" (the test
 * binary's own code, an anonymous mmap, and a synthetic perf-map JIT symbol) and
 * assert the bucket counts + resolved labels. Host-testable: no live PT capture. */
static void test_symbolize_bucket(void) {
#if defined(__linux__)
    int pid = (int)getpid();
    char mappath[64];
    snprintf(mappath, sizeof mappath, "/tmp/perf-%d.map", pid);
    FILE *mf = fopen(mappath, "w");
    if (mf == NULL) {
        printf("# SKIP symbolize-bucket: cannot write perf map\n");
        return;
    }
    fprintf(mf, "40000000 1000 MyJitMethod\n"); /* synthetic JIT symbol range */
    fclose(mf);

    void *anon = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (anon == MAP_FAILED) {
        printf("# SKIP symbolize-bucket: mmap failed\n");
        unlink(mappath);
        return;
    }
    uint64_t ip_self =
        (uint64_t)(uintptr_t)&test_symbolize_bucket;   /* binary text */
    uint64_t ip_anon = (uint64_t)(uintptr_t)anon + 16; /* "[anon]"     */
    uint64_t ip_jit = 0x40000500ULL;                   /* MyJitMethod  */
    uint64_t ips[] = {ip_self, ip_self, ip_self, ip_anon, ip_jit, ip_jit};

    asmtest_hwtrace_bucket_t buckets[8];
    memset(buckets, 0, sizeof buckets);
    size_t nb = asmtest_hwtrace_symbolize_bucket(pid, ips, 6, buckets, 8);

    uint64_t total = 0, jit_count = 0, self_count = 0;
    int jit_labeled = 0, unknown = 0;
    for (size_t i = 0; i < nb; i++) {
        total += buckets[i].count;
        if (strcmp(buckets[i].label, "MyJitMethod") == 0) {
            jit_count = buckets[i].count;
            jit_labeled = 1;
        }
        if (strcmp(buckets[i].label, "[unknown]") == 0)
            unknown = 1;
        if (buckets[i].count == 3)
            self_count = 3;
    }
    CHECK(nb == 3,
          "symbolize bucket: three distinct regions (binary, anon, JIT)");
    CHECK(total == 6, "symbolize bucket: every IP is attributed");
    CHECK(
        jit_labeled && jit_count == 2,
        "symbolize bucket: perf-map JIT symbol resolved + counted (MyJitMethod "
        "x2)");
    CHECK(self_count == 3 && !unknown,
          "symbolize bucket: self-code IPs bucket together, none [unknown]");

    char name[128];
    uint64_t s = 0, e = 0;
    int got =
        asmtest_hwtrace_region_name(0, ip_self, name, sizeof name, &s, &e);
    CHECK(got && name[0] != '\0' && s <= ip_self && ip_self < e,
          "region_name keeps the maps pathname + extent for the self-code "
          "address");

    munmap(anon, 4096);
    unlink(mappath);
#else
    printf("# SKIP symbolize-bucket: not Linux\n");
#endif
}

/* Auto-select front-end: the orchestrator picks the most-faithful AVAILABLE
 * hardware-trace backend for this host, so a caller need not hard-code one. The
 * SELECTION invariants hold on every host (even where all backends self-skip); on
 * any x86-64 Linux host the cascade is non-empty (single-step is always available)
 * and we additionally run a live traced call through the auto-picked backend to
 * prove the choice is usable end to end. */
static void test_auto_resolve(void) {
    asmtest_trace_backend_t best[4], cf[4];
    size_t nb = asmtest_hwtrace_resolve(ASMTEST_HWTRACE_BEST, best, 4);
    size_t nc = asmtest_hwtrace_resolve(ASMTEST_HWTRACE_CEILING_FREE, cf, 4);

    /* Every resolved backend is actually available, in descending-fidelity
     * (ascending-enum) order, with no duplicates. */
    int ok_avail = 1, ok_order = 1;
    for (size_t i = 0; i < nb; i++) {
        if (!asmtest_hwtrace_available(best[i]))
            ok_avail = 0;
        if (i && best[i] <= best[i - 1])
            ok_order = 0;
    }
    CHECK(ok_avail, "auto BEST returns only available backends");
    CHECK(ok_order, "auto BEST is ordered by descending fidelity, no dups");

    /* CEILING_FREE drops the one fixed-window backend (AMD LBR); it is otherwise a
     * subset of BEST (so it never adds a backend BEST lacks). */
    int cf_no_amd = 1, cf_subset = 1;
    for (size_t i = 0; i < nc; i++) {
        if (cf[i] == ASMTEST_HWTRACE_AMD_LBR)
            cf_no_amd = 0;
        int in_best = 0;
        for (size_t j = 0; j < nb; j++)
            in_best |= (best[j] == cf[i]);
        cf_subset &= in_best;
    }
    CHECK(cf_no_amd,
          "auto CEILING_FREE never selects AMD LBR (16-branch window)");
    CHECK(cf_subset, "auto CEILING_FREE is a subset of BEST");

    /* auto(policy) is the head of resolve(policy), or EUNAVAIL when empty. */
    int ab = asmtest_hwtrace_auto(ASMTEST_HWTRACE_BEST);
    int head_ok =
        (nb == 0) ? (ab == ASMTEST_HW_EUNAVAIL) : (ab == (int)best[0]);
    CHECK(head_ok, "auto(BEST) is the head of the resolved cascade");

#if defined(__linux__) && defined(__x86_64__)
    /* Universal guarantee: single-step keeps the cascade non-empty on every x86-64
     * Linux host, so the orchestrator never fails to resolve here. */
    CHECK(nb >= 1 && ab >= 0,
          "auto resolves a backend on x86-64 Linux (single-step floor)");

    /* End to end: trace the shared fixture through whatever auto picked. The pick is
     * single-step on a PT-/AMD-LBR-less host (byte-exact parity), AMD LBR on a Zen
     * 3+/4/5 host with perf (which honestly truncates on this tiny single-shot
     * fixture — too short to be sampled in-region), or Intel PT on bare-metal Intel.
     * So assert the call ran and the trace is honest (covered OR truncated), with the
     * byte-exact stream only for the single-step pick. */
    if (ab >= 0) {
        void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            memcpy(p, ROUTINE, sizeof ROUTINE);
            mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
            __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);
            asmtest_hwtrace_options_t opts;
            memset(&opts, 0, sizeof opts);
            opts.backend = (asmtest_trace_backend_t)ab;
            if (asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK) {
                asmtest_trace_t *tr = asmtest_trace_new(64, 64);
                asmtest_hwtrace_register_region("auto", p, sizeof ROUTINE, tr);
                add2_fn fn = (add2_fn)p;
                asmtest_hwtrace_begin("auto");
                long r = fn(20, 22);
                asmtest_hwtrace_end("auto");
                CHECK(r == 42,
                      "auto-selected backend traces a live call (returns 42)");
                CHECK(asmtest_trace_covered(tr, 0) ||
                          asmtest_emu_trace_truncated(tr),
                      "auto-selected backend covers block 0 (or honestly "
                      "truncates)");
                if (ab == ASMTEST_HWTRACE_SINGLESTEP) {
                    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
                    int seq = (asmtest_emu_trace_insns_total(tr) == 5);
                    for (size_t i = 0; seq && i < 5; i++)
                        seq = (tr->insns[i] == EXPECT[i]);
                    CHECK(seq,
                          "auto pick (single-step) yields the exact shared "
                          "offset stream [0,3,6,c,11]");
                }
                asmtest_hwtrace_shutdown();
                asmtest_trace_free(tr);
            }
            munmap(p, sizeof ROUTINE);
        }
    }
#endif
}

/* Cross-tier orchestrator (asmtest_trace_auto.h): the front-end OVER all three
 * tiers. It interleaves the hardware backends around the DynamoRIO tier by overhead
 * (PT, AMD LBR rank above DynamoRIO; single-step, CoreSight below it) and ends at
 * the emulator floor, gating the native->emulator crossing behind
 * ASMTEST_TRACE_NATIVE_ONLY. These structural invariants hold on every host,
 * independent of which tiers happen to be present. */
static void test_cross_tier_resolve(void) {
    asmtest_trace_choice_t best[8], nat[8], cf[8];
    size_t nb = asmtest_trace_resolve(ASMTEST_TRACE_BEST, best, 8);
    size_t nn = asmtest_trace_resolve(ASMTEST_TRACE_NATIVE_ONLY, nat, 8);
    size_t ncf = asmtest_trace_resolve(ASMTEST_TRACE_CEILING_FREE, cf, 8);

    /* Every HW choice satisfies the hardware-tier probe; fidelity is consistent
     * (NATIVE for HW/DynamoRIO, VIRTUAL for the emulator); and once a VIRTUAL row
     * appears no NATIVE row follows it (the cascade is weakly descending). */
    int ok_hw = 1, ok_fidelity = 1, emu_count = 0;
    for (size_t i = 0; i < nb; i++) {
        if (best[i].tier == ASMTEST_TIER_HWTRACE &&
            !asmtest_hwtrace_available(best[i].backend))
            ok_hw = 0;
        if (best[i].tier == ASMTEST_TIER_EMULATOR) {
            emu_count++;
            if (best[i].fidelity != ASMTEST_FIDELITY_VIRTUAL)
                ok_fidelity = 0;
        } else if (best[i].fidelity != ASMTEST_FIDELITY_NATIVE) {
            ok_fidelity = 0;
        }
        if (i && best[i - 1].fidelity == ASMTEST_FIDELITY_VIRTUAL &&
            best[i].fidelity == ASMTEST_FIDELITY_NATIVE)
            ok_fidelity = 0;
    }
    CHECK(ok_hw,
          "cross-tier BEST: every HW choice is asmtest_hwtrace_available");
    CHECK(ok_fidelity,
          "cross-tier BEST: NATIVE choices precede the VIRTUAL emulator floor");
    CHECK(nb >= 1 && best[nb - 1].tier == ASMTEST_TIER_EMULATOR &&
              emu_count == 1,
          "cross-tier BEST ends at exactly one emulator floor on every host");

    /* NATIVE_ONLY forbids the native->emulator crossing: no emulator row, and the
     * result is exactly BEST with the trailing emulator floor removed. */
    int nat_no_emu = 1;
    for (size_t i = 0; i < nn; i++)
        if (nat[i].tier == ASMTEST_TIER_EMULATOR)
            nat_no_emu = 0;
    CHECK(nat_no_emu,
          "cross-tier NATIVE_ONLY drops the emulator (no fidelity crossing)");
    CHECK(nn == nb - 1,
          "cross-tier NATIVE_ONLY is BEST minus the emulator floor");

    /* CEILING_FREE drops the one ceiling-bounded backend (AMD LBR, ring-bounded). */
    int cf_no_amd = 1;
    for (size_t i = 0; i < ncf; i++)
        if (cf[i].tier == ASMTEST_TIER_HWTRACE &&
            cf[i].backend == ASMTEST_HWTRACE_AMD_LBR)
            cf_no_amd = 0;
    CHECK(cf_no_amd, "cross-tier CEILING_FREE never selects AMD LBR");

    /* auto(policy) is the head of resolve(policy), or EUNAVAIL when empty. */
    asmtest_trace_choice_t one;
    int rc = asmtest_trace_auto(ASMTEST_TRACE_BEST, &one);
    int head_ok = (nb == 0)
                      ? (rc == ASMTEST_HW_EUNAVAIL)
                      : (rc == ASMTEST_HW_OK && one.tier == best[0].tier &&
                         one.backend == best[0].backend);
    CHECK(head_ok, "cross-tier auto(BEST) is the head of the resolved cascade");

#if defined(__linux__) && defined(__x86_64__)
    /* On x86-64 Linux the single-step backend is a NATIVE floor, so even NATIVE_ONLY
     * resolves — the cascade never collapses to nothing here. */
    asmtest_trace_choice_t natpick;
    int nrc = asmtest_trace_auto(ASMTEST_TRACE_NATIVE_ONLY, &natpick);
    CHECK(
        nrc == ASMTEST_HW_OK && natpick.fidelity == ASMTEST_FIDELITY_NATIVE &&
            nn >= 1,
        "cross-tier NATIVE_ONLY still resolves a native tier on x86-64 Linux");
    int has_ss = 0;
    for (size_t i = 0; i < nn; i++)
        has_ss |= (nat[i].tier == ASMTEST_TIER_HWTRACE &&
                   nat[i].backend == ASMTEST_HWTRACE_SINGLESTEP);
    CHECK(has_ss, "cross-tier native cascade includes the single-step floor "
                  "(x86-64 Linux)");
#endif
}

/* Out-of-process single-step (W2): a tracer parent PTRACE_SINGLESTEPs a forked
 * tracee that runs the routine, collecting the same exact offsets out of band. Prove
 * it is byte-identical to the in-process stepper for the shared fixture, and that a
 * 20-trip loop reconstructs with no depth ceiling (it never touches a branch-record
 * window). Runs live on any x86-64 Linux host. */
static void test_ptrace_oop(void) {
    if (!asmtest_ptrace_available()) {
        char why[160];
        asmtest_ptrace_skip_reason(why, sizeof why);
        printf("# SKIP out-of-process ptrace stepper: %s\n", why);
        return;
    }

    /* Arch-selected fixture: x86-64 and AArch64 both run the out-of-process stepper
     * (the only single-step form on AArch64), each yielding its own exact stream. */
#if defined(__aarch64__)
    const unsigned char *RT = ROUTINE_A64;
    const size_t RTN = sizeof ROUTINE_A64;
    static const uint64_t EXPECT[] = {0x0, 0x4, 0x8, 0x10};
    const uint64_t RET_OFF = 0x10;
    const unsigned char *LP = LOOP_A64;
    const size_t LPN = sizeof LOOP_A64;
    const unsigned long long LOOP_INSNS = 63; /* 1 + 20*3 + (mov,ret) */
#else
    const unsigned char *RT = ROUTINE;
    const size_t RTN = sizeof ROUTINE;
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    const uint64_t RET_OFF = 0x11;
    static const unsigned char LOOP_X86[] = {0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00,
                                             0x00, 0x48, 0x01, 0xf8, 0x48, 0xff,
                                             0xce, 0x75, 0xf8, 0xc3};
    const unsigned char *LP = LOOP_X86;
    const size_t LPN = sizeof LOOP_X86;
    const unsigned long long LOOP_INSNS = 62; /* 1 + 20*3 + ret */
#endif
    const size_t NEXP = sizeof EXPECT / sizeof EXPECT[0];

    void *p = mmap(NULL, RTN, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP ptrace oop: mmap failed\n");
        return;
    }
    memcpy(p, RT, RTN);
    mprotect(p, RTN, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + RTN);

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    long args[2] = {20, 22};
    long result = 0;
    int rc = asmtest_ptrace_trace_call(p, RTN, args, 2, &result, tr);
    CHECK(rc == ASMTEST_PTRACE_OK, "ptrace oop trace_call succeeds");
    CHECK(result == 42,
          "ptrace oop traced call returns 20+22 (ret reg read via ptrace)");
    int seq = (asmtest_emu_trace_insns_total(tr) == NEXP);
    for (size_t i = 0; seq && i < NEXP; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq, "ptrace oop yields the exact in-process instruction stream");
    CHECK(asmtest_trace_covered(tr, 0) && asmtest_trace_covered(tr, RET_OFF),
          "ptrace oop block partition matches every other backend");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2,
          "ptrace oop records exactly two blocks");
    CHECK(!asmtest_emu_trace_truncated(tr), "ptrace oop trace is complete");
    asmtest_trace_free(tr);
    munmap(p, RTN);

    /* No depth ceiling: a 20-trip loop (19 back-edges, past AMD LBR's 16) captured
     * exactly — the property an out-of-band hardware branch window cannot match. */
    void *q = mmap(NULL, LPN, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q == MAP_FAILED)
        return;
    memcpy(q, LP, LPN);
    mprotect(q, LPN, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)q, (char *)q + LPN);
    asmtest_trace_t *lt = asmtest_trace_new(256, 64);
    long largs[2] = {1, 20};
    long lresult = 0;
    asmtest_ptrace_trace_call(q, LPN, largs, 2, &lresult, lt);
    CHECK(lresult == 20, "ptrace oop loop returns 20 (sum of 1, 20 times)");
    CHECK(asmtest_emu_trace_insns_total(lt) == LOOP_INSNS,
          "ptrace oop loop captures all loop insns, no depth ceiling");
    CHECK(!asmtest_emu_trace_truncated(lt), "ptrace oop loop is complete");
    asmtest_trace_free(lt);
    munmap(q, LPN);
}

/* BTF block-step (PTRACE_SINGLEBLOCK) must reconstruct the IDENTICAL per-instruction
 * stream as per-instruction single-step, at ~1 trap/branch instead of 1 trap/insn.
 * The proof is a byte-for-byte stream match against the single-step ground truth over
 * two fixtures: ROUTINE (a TAKEN forward conditional — jle), and LOOP (a backward
 * conditional taken 19x then falling through — the reconstruction across a loop
 * back-edge, and the not-taken tail). x86-64 only (SINGLEBLOCK has no AArch64 form). */
static void test_ptrace_blockstep(void) {
#if defined(__x86_64__)
    if (!asmtest_ptrace_blockstep_available()) {
        printf(
            "# SKIP ptrace block-step: PTRACE_SINGLEBLOCK/Capstone unavailable "
            "here\n");
        return;
    }

    /* --- ROUTINE: taken forward conditional (42 <= 100 -> jle taken, dec skipped) --- */
    static const uint64_t R_EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    const size_t R_NEXP = sizeof R_EXPECT / sizeof R_EXPECT[0];
    void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP ptrace block-step: mmap failed\n");
        return;
    }
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);

    asmtest_trace_t *bs = asmtest_trace_new(64, 64);
    long args[2] = {20, 22}, bres = 0;
    int rc = asmtest_ptrace_trace_call_blockstep(p, sizeof ROUTINE, args, 2,
                                                 &bres, bs);
    CHECK(rc == ASMTEST_PTRACE_OK, "block-step trace_call succeeds");
    CHECK(bres == 42, "block-step traced call returns 20+22");
    int seq = (asmtest_emu_trace_insns_total(bs) == R_NEXP);
    for (size_t i = 0; seq && i < R_NEXP; i++)
        seq = (bs->insns[i] == R_EXPECT[i]);
    CHECK(seq,
          "block-step reconstructs the exact single-step stream (ROUTINE)");
    CHECK(asmtest_emu_trace_blocks_len(bs) == 2,
          "block-step yields the same 2-block partition (ROUTINE)");
    CHECK(!asmtest_emu_trace_truncated(bs),
          "block-step ROUTINE trace is complete");
    asmtest_trace_free(bs);
    munmap(p, sizeof ROUTINE);

    /* --- LOOP: 20 trips (19 taken back-edges + 1 not-taken tail to ret) --- */
    static const unsigned char LOOP_X86[] = {0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00,
                                             0x00, 0x48, 0x01, 0xf8, 0x48, 0xff,
                                             0xce, 0x75, 0xf8, 0xc3};
    void *q = mmap(NULL, sizeof LOOP_X86, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q == MAP_FAILED)
        return;
    memcpy(q, LOOP_X86, sizeof LOOP_X86);
    mprotect(q, sizeof LOOP_X86, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)q, (char *)q + sizeof LOOP_X86);

    /* Ground truth: the SAME loop under per-instruction single-step. */
    asmtest_trace_t *ss = asmtest_trace_new(256, 64);
    long largs[2] = {1, 20}, sres = 0;
    asmtest_ptrace_trace_call(q, sizeof LOOP_X86, largs, 2, &sres, ss);

    asmtest_trace_t *lb = asmtest_trace_new(256, 64);
    long lres = 0;
    asmtest_ptrace_trace_call_blockstep(q, sizeof LOOP_X86, largs, 2, &lres,
                                        lb);
    CHECK(lres == 20 && sres == 20,
          "block-step loop returns 20 (sum of 1, 20x)");

    unsigned long long sn = asmtest_emu_trace_insns_total(ss);
    unsigned long long bn = asmtest_emu_trace_insns_total(lb);
    int loop_seq = (bn == sn && bn == 62); /* 1 + 20*3 + ret */
    for (unsigned long long i = 0; loop_seq && i < bn; i++)
        loop_seq = (lb->insns[i] == ss->insns[i]);
    CHECK(loop_seq, "block-step reconstructs single-step across the loop "
                    "back-edge (62 insns)");
    CHECK(asmtest_emu_trace_blocks_len(lb) == asmtest_emu_trace_blocks_len(ss),
          "block-step loop yields the same block partition as single-step");
    CHECK(!asmtest_emu_trace_truncated(lb),
          "block-step loop trace is complete");
    asmtest_trace_free(ss);
    asmtest_trace_free(lb);
    munmap(q, sizeof LOOP_X86);
#else
    printf("# SKIP ptrace block-step: x86-64 only (no AArch64 "
           "PTRACE_SINGLEBLOCK)\n");
#endif
}

/* §D3 whole-window multi-region capture over the cross-process JIT-address channel.
 * The out-of-process analog of the in-process whole-window scope: the stepper is a
 * SEPARATE process that cannot see the runtime's MethodLoadVerbose events, so it learns
 * the JIT'd method addresses through a shared-memory channel the parent publishes into.
 * Fixture (no managed runtime needed): a DRIVER blob that calls two leaf "methods" at
 * their own mmaps via absolute-address indirect calls; the tracee publishes both method
 * regions to the channel BEFORE the window, then runs the driver. The windowed capture
 * must record instructions from the driver AND both channel-published methods (following
 * the indirect calls into them), in execution order, proving the cross-process address
 * handoff + multi-region record/follow. x86-64 only. */
static void test_ptrace_windowed(void) {
#if defined(__x86_64__)
    if (!asmtest_ptrace_available()) {
        char why[160];
        asmtest_ptrace_skip_reason(why, sizeof why);
        printf("# SKIP ptrace windowed: %s\n", why);
        return;
    }

    /* Two "JIT'd methods": pure-register leaves at separate executable mappings. */
    static const unsigned char M1[] = {
        0x48, 0x89, 0xf8, 0x48,
        0x01, 0xf0, 0xc3}; /* mov rax,rdi; add rax,rsi; ret */
    static const unsigned char M2[] = {
        0x48, 0x89, 0xf8, 0x48,
        0x29, 0xf0, 0xc3}; /* mov rax,rdi; sub rax,rsi; ret */
    void *m1 = mmap(NULL, sizeof M1, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *m2 = mmap(NULL, sizeof M2, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m1 == MAP_FAILED || m2 == MAP_FAILED) {
        printf("# SKIP ptrace windowed: mmap failed\n");
        return;
    }
    memcpy(m1, M1, sizeof M1);
    memcpy(m2, M2, sizeof M2);
    mprotect(m1, sizeof M1, PROT_READ | PROT_EXEC);
    mprotect(m2, sizeof M2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)m1, (char *)m1 + sizeof M1);
    __builtin___clear_cache((char *)m2, (char *)m2 + sizeof M2);
    uint64_t a1 = (uint64_t)(uintptr_t)m1, a2 = (uint64_t)(uintptr_t)m2;

    /* Driver: movabs rax,m1; call rax; movabs rax,m2; call rax; ret. */
    unsigned char DRV[25] = {0x48, 0xB8, 0,    0,    0,    0,    0,   0, 0,
                             0,    0xFF, 0xD0, 0x48, 0xB8, 0,    0,   0, 0,
                             0,    0,    0,    0,    0xFF, 0xD0, 0xC3};
    memcpy(DRV + 2, &a1, 8);
    memcpy(DRV + 14, &a2, 8);
    void *drv = mmap(NULL, sizeof DRV, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (drv == MAP_FAILED) {
        printf("# SKIP ptrace windowed: mmap failed\n");
        return;
    }
    memcpy(drv, DRV, sizeof DRV);
    mprotect(drv, sizeof DRV, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)drv, (char *)drv + sizeof DRV);

    /* The cross-process channel lives in shared memory the forked tracee inherits. */
    asmtest_addr_channel_t *chan =
        mmap(NULL, sizeof *chan, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (chan == MAP_FAILED) {
        printf("# SKIP ptrace windowed: channel mmap failed\n");
        return;
    }
    asmtest_addr_channel_init(chan);

    pid_t pid = fork();
    if (pid < 0) {
        printf("# SKIP ptrace windowed: fork failed\n");
        return;
    }
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        /* The runtime's listener publishes each JIT'd method's (base,len) — here,
         * before the window opens (the pre-arm rundown case). */
        asmtest_addr_channel_publish(chan, a1, sizeof M1, 0);
        asmtest_addr_channel_publish(chan, a2, sizeof M2, 0);
        raise(SIGSTOP);
        ((void (*)(void))drv)();
        _exit(0);
    }

    int st = 0;
    long res = 0;
    int rc = ASMTEST_PTRACE_ETRACE;
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    if (waitpid(pid, &st, 0) >= 0 && WIFSTOPPED(st) &&
        asmtest_ptrace_run_to(pid, drv) == ASMTEST_PTRACE_OK)
        rc = asmtest_ptrace_trace_attached_windowed(pid, drv, sizeof DRV, chan,
                                                    &res, tr);
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);

    CHECK(rc == ASMTEST_PTRACE_OK, "windowed multi-region trace succeeds");
    uint64_t dv = (uint64_t)(uintptr_t)drv;
    int hit_drv = 0, hit_m1 = 0, hit_m2 = 0;
    unsigned long long ni = asmtest_emu_trace_insns_len(tr);
    long first_m1 = -1, first_m2 = -1;
    for (unsigned long long i = 0; i < ni; i++) {
        uint64_t at = tr->insns[i];
        if (at >= dv && at < dv + sizeof DRV)
            hit_drv = 1;
        if (at >= a1 && at < a1 + sizeof M1) {
            hit_m1 = 1;
            if (first_m1 < 0)
                first_m1 = (long)i;
        }
        if (at >= a2 && at < a2 + sizeof M2) {
            hit_m2 = 1;
            if (first_m2 < 0)
                first_m2 = (long)i;
        }
    }
    CHECK(hit_drv && hit_m1 && hit_m2, "windowed capture records the driver "
                                       "AND both channel-published methods");
    CHECK(first_m1 >= 0 && first_m2 > first_m1,
          "windowed capture follows the calls in order (m1 before m2)");
    CHECK(!asmtest_emu_trace_truncated(tr), "windowed capture is complete");

    /* chan == NULL degrades to just the window frame (no published regions). */
    asmtest_trace_t *tr2 = asmtest_trace_new(64, 64);
    pid_t pid2 = fork();
    if (pid2 == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        ((void (*)(void))drv)();
        _exit(0);
    }
    int only_drv = 1;
    if (waitpid(pid2, &st, 0) >= 0 && WIFSTOPPED(st) &&
        asmtest_ptrace_run_to(pid2, drv) == ASMTEST_PTRACE_OK &&
        asmtest_ptrace_trace_attached_windowed(
            pid2, drv, sizeof DRV, NULL, &res, tr2) == ASMTEST_PTRACE_OK) {
        unsigned long long n2 = asmtest_emu_trace_insns_len(tr2);
        for (unsigned long long i = 0; i < n2; i++) {
            uint64_t at = tr2->insns[i];
            if (!(at >= dv && at < dv + sizeof DRV))
                only_drv =
                    0; /* nothing outside the window frame (m1/m2 not published) */
        }
        CHECK(only_drv, "windowed with chan==NULL records only the window "
                        "frame, not the methods");
    } else {
        printf("# SKIP windowed chan==NULL leg: setup failed\n");
    }
    kill(pid2, SIGKILL);
    waitpid(pid2, &st, 0);

    asmtest_trace_free(tr);
    asmtest_trace_free(tr2);
    munmap(chan, sizeof *chan);
    munmap(drv, sizeof DRV);
    munmap(m1, sizeof M1);
    munmap(m2, sizeof M2);
#else
    printf("# SKIP ptrace windowed: x86-64 only\n");
#endif
}

/* Fork-internal windowed capture (asmtest_ptrace_trace_window_call): the SAME
 * multi-region whole-window trace as test_ptrace_windowed, but the primitive OWNS its
 * tracee — no caller-side fork / PTRACE_TRACEME / run_to. Publishes the two leaves to a
 * channel via the exported shims (asmtest_addr_channel_new/_publish_rec), then ONE call
 * forks, run_to's the frame, and captures driver + both leaves. This is the entry a
 * managed binding uses (it cannot fork safely). */
static void test_ptrace_window_call(void) {
#if defined(__x86_64__)
    if (!asmtest_ptrace_available()) {
        char why[160];
        asmtest_ptrace_skip_reason(why, sizeof why);
        printf("# SKIP ptrace window_call: %s\n", why);
        return;
    }
    static const unsigned char M1[] = {0x48, 0x89, 0xf8, 0x48,
                                       0x01, 0xf0, 0xc3}; /* rax=rdi+rsi */
    static const unsigned char M2[] = {0x48, 0x89, 0xf8, 0x48,
                                       0x29, 0xf0, 0xc3}; /* rax=rdi-rsi */
    void *m1 = mmap(NULL, sizeof M1, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *m2 = mmap(NULL, sizeof M2, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m1 == MAP_FAILED || m2 == MAP_FAILED) {
        printf("# SKIP ptrace window_call: mmap failed\n");
        return;
    }
    memcpy(m1, M1, sizeof M1);
    memcpy(m2, M2, sizeof M2);
    mprotect(m1, sizeof M1, PROT_READ | PROT_EXEC);
    mprotect(m2, sizeof M2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)m1, (char *)m1 + sizeof M1);
    __builtin___clear_cache((char *)m2, (char *)m2 + sizeof M2);
    uint64_t a1 = (uint64_t)(uintptr_t)m1, a2 = (uint64_t)(uintptr_t)m2;

    /* Driver: movabs rax,m1; call rax; movabs rax,m2; call rax; ret — the leaves
     * inherit the frame's rdi/rsi (the window_call args). */
    unsigned char DRV[25] = {0x48, 0xB8, 0,    0,    0,    0,    0,   0, 0,
                             0,    0xFF, 0xD0, 0x48, 0xB8, 0,    0,   0, 0,
                             0,    0,    0,    0,    0xFF, 0xD0, 0xC3};
    memcpy(DRV + 2, &a1, 8);
    memcpy(DRV + 14, &a2, 8);
    void *drv = mmap(NULL, sizeof DRV, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (drv == MAP_FAILED) {
        printf("# SKIP ptrace window_call: mmap failed\n");
        return;
    }
    memcpy(drv, DRV, sizeof DRV);
    mprotect(drv, sizeof DRV, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)drv, (char *)drv + sizeof DRV);

    /* Process-local channel via the exported FFI shims (no MAP_SHARED needed: the caller
     * pre-publishes and only the tracer parent drains). */
    asmtest_addr_channel_t *chan = asmtest_addr_channel_new();
    if (chan == NULL) {
        printf("# SKIP ptrace window_call: channel alloc failed\n");
        return;
    }
    asmtest_addr_channel_publish_rec(chan, a1, sizeof M1, 0);
    asmtest_addr_channel_publish_rec(chan, a2, sizeof M2, 0);

    long args[2] = {7, 3};
    long res = 0;
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    int rc = asmtest_ptrace_trace_window_call(drv, sizeof DRV, args, 2, chan,
                                              &res, tr);
    CHECK(rc == ASMTEST_PTRACE_OK,
          "window_call fork-internal windowed trace succeeds");
    CHECK(res == 4, "window_call frame returns m2(7,3)=4 (last call's value)");

    uint64_t dv = (uint64_t)(uintptr_t)drv;
    int hit_drv = 0, hit_m1 = 0, hit_m2 = 0;
    long first_m1 = -1, first_m2 = -1;
    unsigned long long ni = asmtest_emu_trace_insns_len(tr);
    for (unsigned long long i = 0; i < ni; i++) {
        uint64_t at = tr->insns[i];
        if (at >= dv && at < dv + sizeof DRV)
            hit_drv = 1;
        if (at >= a1 && at < a1 + sizeof M1) {
            hit_m1 = 1;
            if (first_m1 < 0)
                first_m1 = (long)i;
        }
        if (at >= a2 && at < a2 + sizeof M2) {
            hit_m2 = 1;
            if (first_m2 < 0)
                first_m2 = (long)i;
        }
    }
    CHECK(hit_drv && hit_m1 && hit_m2,
          "window_call records the driver AND both channel-published leaves");
    CHECK(first_m1 >= 0 && first_m2 > first_m1,
          "window_call follows the calls in order (m1 before m2)");
    CHECK(!asmtest_emu_trace_truncated(tr), "window_call capture is complete");

    asmtest_addr_channel_free(chan);
    asmtest_trace_free(tr);
    munmap(drv, sizeof DRV);
    munmap(m1, sizeof M1);
    munmap(m2, sizeof M2);
#else
    printf("# SKIP ptrace window_call: x86-64 only\n");
#endif
}

#if defined(__x86_64__)
/* The window body for test_stealth_windowed — invoked as run_region; it calls the native
 * driver blob, whose entry is the window frame (win_base). File-scope because a C
 * run_region callback cannot be a nested function. */
static void *g_sw_drv;
static void sw_run_window(void *arg) {
    (void)arg;
    ((void (*)(void))g_sw_drv)();
}
#endif

/* Reverse-attach WHOLE-WINDOW capture (asmtest_hwtrace_stealth_trace_windowed): the
 * out-of-process, crash-proof analog of the in-process whole-window scope. A helper child
 * reverse-attaches to THIS process (PR_SET_PTRACER + PTRACE_SEIZE) and single-steps the
 * window body out of band — the body calls a native driver (the window frame) that calls
 * two PRE-PUBLISHED leaf "methods". The capture must record the driver AND both leaves in
 * order, across the process boundary, with NO caller-side fork/attach/run_to. x86-64 only. */
static void test_stealth_windowed(void) {
#if defined(__x86_64__)
    static const unsigned char M1[] = {0x48, 0x89, 0xf8, 0x48,
                                       0x01, 0xf0, 0xc3}; /* rax=rdi+rsi */
    static const unsigned char M2[] = {0x48, 0x89, 0xf8, 0x48,
                                       0x29, 0xf0, 0xc3}; /* rax=rdi-rsi */
    void *m1 = mmap(NULL, sizeof M1, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *m2 = mmap(NULL, sizeof M2, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m1 == MAP_FAILED || m2 == MAP_FAILED) {
        printf("# SKIP stealth windowed: mmap failed\n");
        return;
    }
    memcpy(m1, M1, sizeof M1);
    memcpy(m2, M2, sizeof M2);
    mprotect(m1, sizeof M1, PROT_READ | PROT_EXEC);
    mprotect(m2, sizeof M2, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)m1, (char *)m1 + sizeof M1);
    __builtin___clear_cache((char *)m2, (char *)m2 + sizeof M2);
    uint64_t a1 = (uint64_t)(uintptr_t)m1, a2 = (uint64_t)(uintptr_t)m2;

    /* Driver (the window frame): mov edi,7; mov esi,3; movabs rax,m1; call rax;
     * movabs rax,m2; call rax; ret. Self-contained args → m1(7,3)=10, m2(7,3)=4. */
    unsigned char DRV[35] = {
        0xBF, 7,    0,    0,    0,    0xBE, 3,    0,    0,
        0,    0x48, 0xB8, 0,    0,    0,    0,    0,    0,
        0,    0,    0xFF, 0xD0, 0x48, 0xB8, 0,    0,    0,
        0,    0,    0,    0,    0,    0xFF, 0xD0, 0xC3};
    memcpy(DRV + 12, &a1, 8);
    memcpy(DRV + 24, &a2, 8);
    void *drv = mmap(NULL, sizeof DRV, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (drv == MAP_FAILED) {
        printf("# SKIP stealth windowed: mmap failed\n");
        return;
    }
    memcpy(drv, DRV, sizeof DRV);
    mprotect(drv, sizeof DRV, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)drv, (char *)drv + sizeof DRV);
    g_sw_drv = drv;

    asmtest_addr_channel_t *chan = asmtest_addr_channel_new_shared();
    if (chan == NULL) {
        printf("# SKIP stealth windowed: shared channel alloc failed\n");
        return;
    }
    asmtest_addr_channel_publish(chan, a1, sizeof M1, 0);
    asmtest_addr_channel_publish(chan, a2, sizeof M2, 0);
    long res = 0;
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    int rc = asmtest_hwtrace_stealth_trace_windowed(drv, sizeof DRV, chan, tr,
                                                    &res, sw_run_window, NULL);
    if (rc == ASMTEST_HW_EUNAVAIL) {
        printf("# SKIP stealth windowed: reverse-attach not permitted (Yama / "
               "no ptrace)\n");
        asmtest_addr_channel_free_shared(chan);
        asmtest_trace_free(tr);
        munmap(drv, sizeof DRV);
        munmap(m1, sizeof M1);
        munmap(m2, sizeof M2);
        return;
    }
    CHECK(rc == ASMTEST_HW_OK,
          "stealth windowed reverse-attach whole-window trace succeeds");
    CHECK(res == 4, "stealth windowed frame returns m2(7,3)=4");

    uint64_t dv = (uint64_t)(uintptr_t)drv;
    int hit_drv = 0, hit_m1 = 0, hit_m2 = 0;
    long first_m1 = -1, first_m2 = -1;
    unsigned long long ni = asmtest_emu_trace_insns_len(tr);
    for (unsigned long long i = 0; i < ni; i++) {
        uint64_t at = tr->insns[i];
        if (at >= dv && at < dv + sizeof DRV)
            hit_drv = 1;
        if (at >= a1 && at < a1 + sizeof M1) {
            hit_m1 = 1;
            if (first_m1 < 0)
                first_m1 = (long)i;
        }
        if (at >= a2 && at < a2 + sizeof M2) {
            hit_m2 = 1;
            if (first_m2 < 0)
                first_m2 = (long)i;
        }
    }
    CHECK(hit_drv && hit_m1 && hit_m2,
          "stealth windowed records the driver frame AND both pre-published "
          "leaves");
    CHECK(first_m1 >= 0 && first_m2 > first_m1,
          "stealth windowed follows the calls in order (m1 before m2)");
    CHECK(!asmtest_emu_trace_truncated(tr), "stealth windowed capture complete");

    asmtest_addr_channel_free_shared(chan);
    asmtest_trace_free(tr);
    munmap(drv, sizeof DRV);
    munmap(m1, sizeof M1);
    munmap(m2, sizeof M2);
#else
    printf("# SKIP stealth windowed: x86-64 only\n");
#endif
}

/* Faulting-routine trace must NOT leak the forked tracee. Tracing a routine that
 * takes a real signal (SIGILL/SIGSEGV) is the whole point of the out-of-process
 * stepper — a buggy routine is exactly what you trace. The single-step loop breaks
 * on the non-SIGTRAP stop with rc==OK, so unless that path reaps the tracee it stays
 * a stopped, unreaped child (PTRACE_O_EXITKILL fires only when the *tracer* exits),
 * and a suite of faulting routines exhausts PIDs. Fixture: `ud2` (SIGILL) — enter the
 * region, record offset 0, fault on the next step. We assert (a) the call still
 * returns OK with a truncated trace, and (b) after each of several traces there is no
 * child left to reap (waitpid(-1) -> ECHILD), which fails loudly if the tracee leaks. */
static void test_ptrace_faulting_no_leak(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_ptrace_available()) {
        char why[160];
        asmtest_ptrace_skip_reason(why, sizeof why);
        printf("# SKIP ptrace faulting no-leak: %s\n", why);
        return;
    }

    /* Drain any straggler children so the ECHILD assertion below measures only what
     * this test forks (peer tests reap their own tracees, but be defensive). */
    while (waitpid(-1, NULL, WNOHANG | WUNTRACED) > 0) {
    }

    static const unsigned char FAULT[] = {0x0f, 0x0b}; /* ud2 -> SIGILL */
    void *p = mmap(NULL, sizeof FAULT, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP ptrace faulting no-leak: mmap failed\n");
        return;
    }
    memcpy(p, FAULT, sizeof FAULT);
    mprotect(p, sizeof FAULT, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof FAULT);

    int all_ok = 1, all_truncated = 1, no_leak = 1;
    long args[2] = {1, 2};
    for (int i = 0; i < 8; i++) {
        asmtest_trace_t *tr = asmtest_trace_new(16, 16);
        long result = 0;
        int rc =
            asmtest_ptrace_trace_call(p, sizeof FAULT, args, 2, &result, tr);
        if (rc != ASMTEST_PTRACE_OK)
            all_ok = 0;
        if (!asmtest_emu_trace_truncated(tr))
            all_truncated = 0;
        asmtest_trace_free(tr);
        /* The tracee must already be reaped: with no children left waitpid returns
         * -1/ECHILD. A leaked (stopped, still-traced) child would instead be reported
         * here — WUNTRACED surfaces the stop — so this is 0/pid, not ECHILD. */
        errno = 0;
        pid_t w = waitpid(-1, NULL, WNOHANG | WUNTRACED);
        if (!(w == -1 && errno == ECHILD))
            no_leak = 0;
    }
    munmap(p, sizeof FAULT);

    CHECK(all_ok,
          "ptrace faulting routine still returns OK (records what it has)");
    CHECK(all_truncated, "ptrace faulting trace is flagged truncated");
    CHECK(no_leak,
          "ptrace faulting routine leaves no unreaped tracee (no PID leak)");
#else
    printf("# SKIP ptrace faulting no-leak: x86-64 Linux only\n");
#endif
}

/* W2 live ATTACH: trace a region in a SEPARATE, externally-attached process — the
 * building block for tracing a managed runtime out of band. A child spins on a shared
 * flag, then calls the fixture; the parent PTRACE_ATTACHes it (the child never called
 * TRACEME — a true external attach), traces the region with
 * asmtest_ptrace_trace_attached (which reads the child's code via process_vm_readv,
 * not a shared mapping), and asserts the SAME offsets the in-process stepper yields. */
static void test_ptrace_attach(void) {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    if (!asmtest_ptrace_available()) {
        char why[160];
        asmtest_ptrace_skip_reason(why, sizeof why);
        printf("# SKIP ptrace attach: %s\n", why);
        return;
    }
#if defined(__aarch64__)
    const unsigned char *RT = ROUTINE_A64;
    const size_t RTN = sizeof ROUTINE_A64;
    static const uint64_t EXPECT[] = {0x0, 0x4, 0x8, 0x10};
#else
    const unsigned char *RT = ROUTINE;
    const size_t RTN = sizeof ROUTINE;
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
#endif
    const size_t NEXP = sizeof EXPECT / sizeof EXPECT[0];
    volatile int *go = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    void *p = mmap(NULL, RTN, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (go == MAP_FAILED || p == MAP_FAILED) {
        printf("# SKIP ptrace attach: mmap failed\n");
        return;
    }
    *go = 0;
    memcpy(p, RT, RTN);
    mprotect(p, RTN, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + RTN);

    pid_t pid = fork();
    if (pid == 0) {
        while (!*go) {
            /* user-space spin (no syscall) so the external attach catches us cleanly */
        }
        volatile long r = ((add2_fn)p)(20, 22);
        (void)r;
        _exit(0);
    }

    /* Let the child reach the spin, then attach from the OUTSIDE (it never opted in
     * via TRACEME). The attach stops the child; only then do we release it, so it
     * cannot run the region before we are tracing. */
    struct timespec ts = {0, 3 * 1000 * 1000}; /* 3 ms */
    nanosleep(&ts, NULL);
    int status = 0;
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0 ||
        waitpid(pid, &status, 0) < 0) {
        printf("# SKIP ptrace attach: PTRACE_ATTACH not permitted (yama "
               "ptrace_scope)\n");
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return;
    }
    *go = 1;

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    long result = 0;
    int rc = asmtest_ptrace_trace_attached(pid, p, RTN, &result, tr);
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    waitpid(pid, &status, 0);

    CHECK(
        rc == ASMTEST_PTRACE_OK,
        "ptrace attach: trace_attached succeeds on an externally-attached PID");
    CHECK(
        result == 42,
        "ptrace attach: result 42 read from the foreign process's return reg");
    int seq = (asmtest_emu_trace_insns_total(tr) == NEXP);
    for (size_t i = 0; seq && i < NEXP; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq, "ptrace attach: foreign-process trace == the in-process stream");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2 &&
              !asmtest_emu_trace_truncated(tr),
          "ptrace attach: two blocks, complete (bytes read via "
          "process_vm_readv)");
    asmtest_trace_free(tr);
    munmap(p, RTN);
    munmap((void *)go, sizeof(int));
#else
    printf("# SKIP ptrace attach: not Linux x86-64/AArch64\n");
#endif
}

/* Attached BLOCK-STEP: the same true-external-attach harness as test_ptrace_attach,
 * but traced with asmtest_ptrace_trace_attached_blockstep — one #DB per TAKEN branch
 * with the intra-block instructions reconstructed — asserting the stream is
 * byte-identical to the per-instruction attached tracer. This is the rootless
 * managed-runtime completeness fallback at a fraction of the stops (the third
 * block-step entry point the AMD plan's Phase 2 scopes). x86-64 only. */
static void test_ptrace_attach_blockstep(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_ptrace_blockstep_available()) {
        printf("# SKIP ptrace attach block-step: PTRACE_SINGLEBLOCK/Capstone "
               "unavailable here\n");
        return;
    }
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    const size_t NEXP = sizeof EXPECT / sizeof EXPECT[0];
    volatile int *go = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (go == MAP_FAILED || p == MAP_FAILED) {
        printf("# SKIP ptrace attach block-step: mmap failed\n");
        return;
    }
    *go = 0;
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);

    pid_t pid = fork();
    if (pid == 0) {
        while (!*go) {
            /* user-space spin (no syscall) so the external attach catches us cleanly */
        }
        volatile long r = ((add2_fn)p)(20, 22);
        (void)r;
        _exit(0);
    }

    struct timespec ts = {0, 3 * 1000 * 1000}; /* 3 ms */
    nanosleep(&ts, NULL);
    int status = 0;
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0 ||
        waitpid(pid, &status, 0) < 0) {
        printf("# SKIP ptrace attach block-step: PTRACE_ATTACH not permitted "
               "(yama ptrace_scope)\n");
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return;
    }
    *go = 1;

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    long result = 0;
    int rc = asmtest_ptrace_trace_attached_blockstep(pid, p, sizeof ROUTINE,
                                                     &result, tr);
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    waitpid(pid, &status, 0);

    CHECK(rc == ASMTEST_PTRACE_OK,
          "attach block-step: trace_attached_blockstep succeeds externally");
    CHECK(result == 42,
          "attach block-step: result 42 read from the foreign return reg");
    int seq = (asmtest_emu_trace_insns_total(tr) == NEXP);
    for (size_t i = 0; seq && i < NEXP; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq,
          "attach block-step: stream byte-identical to the per-insn attached "
          "tracer");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2 &&
              !asmtest_emu_trace_truncated(tr),
          "attach block-step: two blocks, complete");
    asmtest_trace_free(tr);
    munmap(p, sizeof ROUTINE);
    munmap((void *)go, sizeof(int));
#else
    printf("# SKIP ptrace attach block-step: not Linux x86-64\n");
#endif
}

/* Region resolution + trace: the step that turns the attach primitive into "point it
 * at a running process". Discover a foreign process's executable region from
 * /proc/<pid>/maps using only an interior address (what you'd have from a function
 * pointer or a jitdump), then attach and trace THAT region — no hardcoded base. */
static void test_proc_resolve_and_trace(void) {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    if (!asmtest_ptrace_available()) {
        char why[160];
        asmtest_ptrace_skip_reason(why, sizeof why);
        printf("# SKIP proc resolve: %s\n", why);
        return;
    }
#if defined(__aarch64__)
    const unsigned char *RT = ROUTINE_A64;
    const size_t RTN = sizeof ROUTINE_A64;
    static const uint64_t EXPECT[] = {0x0, 0x4, 0x8, 0x10};
#else
    const unsigned char *RT = ROUTINE;
    const size_t RTN = sizeof ROUTINE;
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
#endif
    const size_t NEXP = sizeof EXPECT / sizeof EXPECT[0];
    volatile int *go = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    void *p = mmap(NULL, RTN, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (go == MAP_FAILED || p == MAP_FAILED) {
        printf("# SKIP proc resolve: mmap failed\n");
        return;
    }
    *go = 0;
    memcpy(p, RT, RTN);
    mprotect(p, RTN, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + RTN);

    pid_t pid = fork();
    if (pid == 0) {
        while (!*go) {
        }
        volatile long r = ((add2_fn)p)(20, 22);
        (void)r;
        _exit(0);
    }
    struct timespec ts = {0, 3 * 1000 * 1000};
    nanosleep(&ts, NULL);

    /* Discover the region from the OS given only an interior address. */
    void *base = NULL;
    size_t rlen = 0;
    int found = asmtest_proc_region_by_addr(pid, (char *)p + 4, &base, &rlen);
    CHECK(found == ASMTEST_PTRACE_OK,
          "proc maps: resolved the foreign region from an interior address");
    CHECK(base == p && rlen >= RTN,
          "proc maps: discovered base == region start, len spans the routine");

    int status = 0;
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0 ||
        waitpid(pid, &status, 0) < 0) {
        printf("# SKIP proc resolve: PTRACE_ATTACH not permitted\n");
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        munmap(p, sizeof ROUTINE);
        munmap((void *)go, sizeof(int));
        return;
    }
    *go = 1;
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    long result = 0;
    int rc = asmtest_ptrace_trace_attached(pid, base, rlen, &result, tr);
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    waitpid(pid, &status, 0);

    CHECK(rc == ASMTEST_PTRACE_OK && result == 42,
          "proc resolve+trace: traced the OS-discovered region (result 42)");
    int seq = (asmtest_emu_trace_insns_total(tr) == NEXP);
    for (size_t i = 0; seq && i < NEXP; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq, "proc resolve+trace: same stream from the OS-discovered region");
    asmtest_trace_free(tr);
    munmap(p, RTN);
    munmap((void *)go, sizeof(int));
#else
    printf("# SKIP proc resolve: not Linux x86-64/AArch64\n");
#endif
}

/* W2 END-TO-END, UNCONTROLLED TIMING — the real managed-runtime flow. Every test above
 * arranges the stop with a cooperative go-flag (the parent releases the child so it calls
 * the region next); a real JIT gives you no such flag — the program calls the method on
 * its own schedule. Here a child publishes its generated routine to /tmp/perf-<pid>.map
 * (the format V8/Node/.NET/OpenJDK emit) and then calls it in a LOOP the parent does not
 * gate. The parent attaches from the outside, resolves the method by NAME, then
 * asmtest_ptrace_run_to()s the target to the method entry — a software breakpoint that
 * fires when the program ITSELF next calls in — and traces that one invocation. This is
 * the step that turns the W2 primitives into "attach to a live JIT and trace a method
 * whose call timing you do not control." */
static void test_run_to_and_trace(void) {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    if (!asmtest_ptrace_available()) {
        char why[160];
        asmtest_ptrace_skip_reason(why, sizeof why);
        printf("# SKIP run_to: %s\n", why);
        return;
    }
#if defined(__aarch64__)
    const unsigned char *RT = ROUTINE_A64;
    const size_t RTN = sizeof ROUTINE_A64;
    static const uint64_t EXPECT[] = {0x0, 0x4, 0x8, 0x10};
#else
    const unsigned char *RT = ROUTINE;
    const size_t RTN = sizeof ROUTINE;
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
#endif
    const size_t NEXP = sizeof EXPECT / sizeof EXPECT[0];
    const char *METHOD = "void asmtest::jit::run_to_demo(long, long)";

    void *p = mmap(NULL, RTN, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP run_to: mmap failed\n");
        return;
    }
    memcpy(p, RT, RTN);
    mprotect(p, RTN, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + RTN);

    pid_t pid = fork();
    if (pid == 0) {
        /* Publish the JIT method the way a managed runtime does, then call it forever on
         * OUR schedule — the parent never signals when. */
        char mp[64];
        snprintf(mp, sizeof mp, "/tmp/perf-%d.map", (int)getpid());
        FILE *mf = fopen(mp, "w");
        if (mf != NULL) {
            fprintf(mf, "%lx %zx %s\n", (unsigned long)(uintptr_t)p, RTN,
                    METHOD);
            fclose(mf);
        }
        for (;;) {
            volatile long r = ((add2_fn)p)(20, 22);
            (void)r;
            struct timespec t = {0, 1 * 1000 * 1000}; /* 1 ms between calls */
            nanosleep(&t, NULL);
        }
        _exit(0);
    }

    /* Give the child time to publish its perf-map and start looping. */
    struct timespec ts = {0, 10 * 1000 * 1000}; /* 10 ms */
    nanosleep(&ts, NULL);

    /* Resolve the method by NAME from the foreign perf-map (no hardcoded address) —
     * exactly what a tracer holds when pointed at a live JIT. */
    void *base = NULL;
    size_t rlen = 0;
    int found = asmtest_proc_perfmap_symbol(pid, METHOD, &base, &rlen);
    CHECK(found == ASMTEST_PTRACE_OK && base == p && rlen == RTN,
          "run_to: resolved the JIT method by name from the foreign perf-map");

    char mp[64];
    snprintf(mp, sizeof mp, "/tmp/perf-%d.map", (int)pid);
    int status = 0;
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0 ||
        waitpid(pid, &status, 0) < 0) {
        printf(
            "# SKIP run_to: PTRACE_ATTACH not permitted (yama ptrace_scope)\n");
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        remove(mp);
        munmap(p, RTN);
        return;
    }

    /* Run the target forward until IT next calls the method (timing we do not control),
     * leaving it stopped at the entry, then trace that one invocation. */
    int rc_run = asmtest_ptrace_run_to(pid, base);
    CHECK(rc_run == ASMTEST_PTRACE_OK,
          "run_to: target reached the method entry via a software breakpoint");

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    long result = 0;
    int rc = (rc_run == ASMTEST_PTRACE_OK)
                 ? asmtest_ptrace_trace_attached(pid, base, rlen, &result, tr)
                 : ASMTEST_PTRACE_ETRACE;

    /* The child loops forever, so end it rather than detach-and-resume. */
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);

    CHECK(rc == ASMTEST_PTRACE_OK && result == 42,
          "run_to: traced the JIT method at its next real call (result 42)");
    int seq = (asmtest_emu_trace_insns_total(tr) == NEXP);
    for (size_t i = 0; seq && i < NEXP; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq, "run_to: same exact stream as the in-process stepper");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2 &&
              !asmtest_emu_trace_truncated(tr),
          "run_to: two blocks, complete (breakpoint removed, PC rewound)");
    asmtest_trace_free(tr);
    remove(mp);
    munmap(p, RTN);
#else
    printf("# SKIP run_to: not Linux x86-64/AArch64\n");
#endif
}

/* Phase B (codeimage x W2): asmtest_ptrace_trace_attached_versioned decodes the region
 * against TIME-CORRECT bytes from the code-image recorder rather than a single live
 * snapshot. The recorder is given two versions at ONE address — the real body A and a
 * stale wrong body (NOPs) — captured before the trace. The child executes A; the executed
 * instruction stream comes from single-stepping (so it is identical regardless of the
 * byte version), but the BLOCK partition is derived by decoding instruction lengths from
 * the supplied bytes. Decoding A's stream against the tA snapshot yields the correct
 * 2-block partition; decoding the SAME stream against the stale bytes misreads every
 * instruction as a new block — exactly the corruption a single late process_vm_readv would
 * suffer once the address was re-JITted, and the reason a time-aware byte source exists. */
static void test_ptrace_versioned(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_ptrace_available() || !asmtest_codeimage_available()) {
        printf("# SKIP ptrace versioned: %s\n",
               asmtest_ptrace_available() ? "no soft-dirty page tracking"
                                          : "no ptrace single-step");
        return;
    }
    if (!asmtest_disas_available()) {
        printf("# SKIP ptrace versioned: needs Capstone for block "
               "normalization\n");
        return;
    }
    const size_t RTN = sizeof ROUTINE;
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    const size_t NEXP = sizeof EXPECT / sizeof EXPECT[0];
    unsigned char NOPS[sizeof ROUTINE];
    memset(NOPS, 0x90,
           sizeof NOPS); /* a different-length encoding of the same byte span */

    /* MAP_PRIVATE: the child keeps an untouched copy of A (COW), so the parent's later
     * rewrites build the timeline WITHOUT ever changing what the child executes. */
    unsigned char *p = (unsigned char *)mmap(
        NULL, RTN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP ptrace versioned: mmap failed\n");
        return;
    }
    memcpy(p, ROUTINE, RTN);
    mprotect(p, RTN, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + RTN);

    pid_t pid = fork();
    if (pid == 0) {
        for (
            ;
            ;) { /* call the real body forever; the parent traces one invocation */
            volatile long r = ((add2_fn)p)(20, 22);
            (void)r;
            struct timespec t = {0, 1000 * 1000}; /* 1 ms */
            nanosleep(&t, NULL);
        }
        _exit(0);
    }

    /* Build the timeline on the PARENT's own copy (codeimage tracks self, so soft-dirty —
     * which is per-process — reflects the parent's writes). The recorder is an
     * address-keyed byte source, so its pid need not be the traced pid. */
    asmtest_codeimage_t *img = asmtest_codeimage_new(0);
    int okT = (asmtest_codeimage_track(img, p, RTN) == ASMTEST_CI_OK);
    uint64_t tA = asmtest_codeimage_now(img);
    mprotect(p, RTN, PROT_READ | PROT_WRITE);
    memcpy(
        p, NOPS,
        RTN); /* re-JIT the SAME address to a different body (COW: child keeps A) */
    mprotect(p, RTN, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + RTN);
    int nvB = asmtest_codeimage_refresh(img);
    uint64_t tB = asmtest_codeimage_now(img);
    CHECK(okT && nvB >= 1 && tB > tA, "ptrace versioned: recorder holds A@tA "
                                      "and a stale body @tB at one address");

    struct timespec ts5 = {0, 5 * 1000 * 1000};
    nanosleep(&ts5, NULL); /* let the child get into its call loop */
    int status = 0;
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0 ||
        waitpid(pid, &status, 0) < 0) {
        printf("# SKIP ptrace versioned: PTRACE_ATTACH not permitted (yama "
               "ptrace_scope)\n");
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        asmtest_codeimage_free(img);
        munmap(p, RTN);
        return;
    }

    asmtest_trace_t *trA = asmtest_trace_new(64, 64);
    asmtest_trace_t *trB = asmtest_trace_new(64, 64);
    long resA = 0, resB = 0;
    int rcA = (asmtest_ptrace_run_to(pid, p) == ASMTEST_PTRACE_OK)
                  ? asmtest_ptrace_trace_attached_versioned(pid, p, RTN, img,
                                                            tA, &resA, trA)
                  : ASMTEST_PTRACE_ETRACE;
    int rcB = (asmtest_ptrace_run_to(pid, p) == ASMTEST_PTRACE_OK)
                  ? asmtest_ptrace_trace_attached_versioned(pid, p, RTN, img,
                                                            tB, &resB, trB)
                  : ASMTEST_PTRACE_ETRACE;
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);

    CHECK(rcA == ASMTEST_PTRACE_OK && resA == 42,
          "ptrace versioned: traced the live method decoding against tA "
          "(result 42)");
    int seqA = (asmtest_emu_trace_insns_total(trA) == NEXP);
    for (size_t i = 0; seqA && i < NEXP; i++)
        seqA = (trA->insns[i] == EXPECT[i]);
    CHECK(seqA, "ptrace versioned: tA (time-correct) bytes reconstruct the "
                "exact A stream");
    CHECK(asmtest_emu_trace_blocks_len(trA) == 2 &&
              !asmtest_emu_trace_truncated(trA),
          "ptrace versioned: tA bytes -> the correct 2-block partition");

    int seqB = (asmtest_emu_trace_insns_total(trB) == NEXP);
    for (size_t i = 0; seqB && i < NEXP; i++)
        seqB = (trB->insns[i] == EXPECT[i]);
    CHECK(rcB == ASMTEST_PTRACE_OK && seqB,
          "ptrace versioned: the SAME executed stream decodes against the "
          "stale tB bytes");
    CHECK(asmtest_emu_trace_blocks_len(trB) == NEXP &&
              asmtest_emu_trace_blocks_len(trB) !=
                  asmtest_emu_trace_blocks_len(trA),
          "ptrace versioned: stale bytes MISREAD the block partition (the "
          "temporal bug)");

    asmtest_trace_free(trA);
    asmtest_trace_free(trB);
    asmtest_codeimage_free(img);
    munmap(p, RTN);
#else
    printf("# SKIP ptrace versioned: not Linux x86-64\n");
#endif
}

/* CALL-DEPTH AWARENESS: a registered region that CALLS OUT to a helper OUTSIDE it (a
 * runtime helper / GC barrier / PLT stub — what a real managed-runtime method does). The
 * old "first region exit == return" model truncated at that call; the stepper now decodes
 * the call, runs the callee at native speed to its return, and resumes — so the trace
 * holds the region's OWN instructions only (helper skipped), and still finds the real
 * return. Uses trace_call (self-contained: fork + single-step), exercising the same
 * call-out path trace_attached shares.
 *
 * Runs twice: once normally (software int3 over the call-out) and once with
 * ASMTEST_PTRACE_HW_BP forcing the HARDWARE-breakpoint path — the path that traces W^X
 * JIT code (e.g. .NET as-shipped), exercised here deterministically on ordinary memory,
 * on real x86-64 debug registers, in a plain container. Both must yield the same trace. */
static void test_ptrace_callout(const char *label, int force_hw) {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    if (!asmtest_ptrace_available()) {
        char why[160];
        asmtest_ptrace_skip_reason(why, sizeof why);
        printf("# SKIP ptrace callout (%s): %s\n", label, why);
        return;
    }
    if (!asmtest_disas_available()) {
        printf("# SKIP ptrace callout (%s): needs Capstone (call detection)\n",
               label);
        return;
    }
#if defined(__aarch64__)
    if (force_hw) { /* the hardware-breakpoint path is x86-64 only for now */
        printf("# SKIP ptrace callout (%s): hardware breakpoints are x86-64 "
               "only\n",
               label);
        return;
    }
    /* R: bl H ; add x0,x0,x1 ; ret    H@0xc: add x0,x0,#1 ; ret */
    static const unsigned char BLOB[] = {
        0x03, 0x00, 0x00, 0x94, 0x00, 0x00, 0x01, 0x8b, 0xc0, 0x03,
        0x5f, 0xd6, 0x00, 0x04, 0x00, 0x91, 0xc0, 0x03, 0x5f, 0xd6};
    static const uint64_t EXPECT[] = {0x0, 0x4, 0x8};
#else
    /* R: mov rax,rdi ; call H ; add rax,rsi ; ret    H@0xc: inc rax ; ret */
    static const unsigned char BLOB[] = {0x48, 0x89, 0xf8, 0xe8, 0x04, 0x00,
                                         0x00, 0x00, 0x48, 0x01, 0xf0, 0xc3,
                                         0x48, 0xff, 0xc0, 0xc3};
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x8, 0xb};
#endif
    const size_t REGION =
        0xc; /* trace R only; the helper H at 0xc is outside it */
    const size_t NEXP = sizeof EXPECT / sizeof EXPECT[0];
    char msg[96];

    if (force_hw)
        setenv("ASMTEST_PTRACE_HW_BP", "1", 1);

    void *p = mmap(NULL, sizeof BLOB, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP ptrace callout (%s): mmap failed\n", label);
        if (force_hw)
            unsetenv("ASMTEST_PTRACE_HW_BP");
        return;
    }
    memcpy(p, BLOB, sizeof BLOB);
    mprotect(p, sizeof BLOB, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof BLOB);

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    const long args[2] = {20, 22};
    long result = 0;
    int rc = asmtest_ptrace_trace_call(p, REGION, args, 2, &result, tr);

    snprintf(msg, sizeof msg,
             "ptrace callout (%s): trace_call over a region that calls out",
             label);
    CHECK(rc == ASMTEST_PTRACE_OK, msg);
    snprintf(msg, sizeof msg,
             "ptrace callout (%s): result 43 (helper ran: 20 +1 +22)", label);
    CHECK(result == 43, msg);
    int seq = (asmtest_emu_trace_insns_total(tr) == NEXP);
    for (size_t i = 0; seq && i < NEXP; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    snprintf(msg, sizeof msg,
             "ptrace callout (%s): in-region stream only (helper skipped)",
             label);
    CHECK(seq, msg);
    snprintf(msg, sizeof msg,
             "ptrace callout (%s): complete (call-out not a false return)",
             label);
    CHECK(!asmtest_emu_trace_truncated(tr), msg);
    asmtest_trace_free(tr);
    munmap(p, sizeof BLOB);
    if (force_hw)
        unsetenv("ASMTEST_PTRACE_HW_BP");
#else
    (void)force_hw;
    printf("# SKIP ptrace callout (%s): not Linux x86-64/AArch64\n", label);
#endif
}

/* §D3: the concealed out-of-process ptrace-stealth stepper. A helper CHILD
 * reverse-attaches to THIS process (PR_SET_PTRACER + PTRACE_SEIZE) and single-steps
 * the region while we run it — the hardware-free scope path for Zen 2 / Docker-on-Mac.
 * CI-runnable on any ptrace-capable Linux (self-skips where the reverse attach is
 * refused); asserts exact offsets vs the in-process/ground-truth stream. */
#if defined(__linux__) && defined(__x86_64__)
static void stealth_run_region(void *arg) {
    volatile long r = ((add2_fn)arg)(20, 22);
    (void)r;
}
/* Internal §D3 discovery (src/stealth_helper.c) — not a public header symbol, so
 * forward-declare it here to assert the dladdr-sibling lookup finds the bundled
 * binary the makefile builds next to this test. */
const char *asmtest_stealth_helper_path(char *buf, size_t buflen);

/* Drive the stealth scope once and assert the exact stream. `label` distinguishes
 * the in-process forked-child fallback from the exec'd bundled-binary path; both
 * must reconstruct the identical [0,3,6,c,11] offsets. Returns 1 when the reverse
 * attach was refused (a skip, not a failure), else 0. */
static int stealth_trace_once(const char *label, void *p) {
    char msg[128];
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    long result = 0;
    int rc = asmtest_hwtrace_stealth_trace(p, sizeof ROUTINE, tr, &result,
                                           stealth_run_region, p);
    if (rc == ASMTEST_HW_EUNAVAIL) {
        printf("# SKIP ptrace stealth (%s): reverse-attach not permitted (yama "
               "ptrace_scope)\n",
               label);
        asmtest_trace_free(tr);
        return 1;
    }
    snprintf(msg, sizeof msg, "ptrace stealth (%s): helper traced the region",
             label);
    CHECK(rc == ASMTEST_HW_OK, msg);
    snprintf(msg, sizeof msg, "ptrace stealth (%s): result 42 from caller reg",
             label);
    CHECK(result == 42, msg);
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int seq = (asmtest_emu_trace_insns_total(tr) == 5);
    for (size_t i = 0; seq && i < 5; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    snprintf(msg, sizeof msg,
             "ptrace stealth (%s): exact offsets [0,3,6,c,11] vs ground truth",
             label);
    CHECK(seq, msg);
    snprintf(msg, sizeof msg,
             "ptrace stealth (%s): two blocks, complete (stepped out of band)",
             label);
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2 &&
              !asmtest_emu_trace_truncated(tr),
          msg);
    asmtest_trace_free(tr);
    return 0;
}
#endif
static void test_ptrace_scoped_stealth(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_ptrace_available()) {
        char why[160];
        asmtest_ptrace_skip_reason(why, sizeof why);
        printf("# SKIP ptrace stealth: %s\n", why);
        return;
    }
    void *p = ss_map_exec(ROUTINE, sizeof ROUTINE);
    if (p == NULL) {
        printf("# SKIP ptrace stealth: mmap failed\n");
        return;
    }

    /* §D3 bundling: with no override the standalone helper is discovered as our
     * dladdr sibling (the makefile builds asmtest-stealth-helper next to
     * test_hwtrace in build/), mirroring how a managed package finds the payload. */
    unsetenv("ASMTEST_STEALTH_HELPER");
    char hp[4096], bundled[4096];
    bundled[0] = '\0';
    const char *found = asmtest_stealth_helper_path(hp, sizeof hp);
    if (found != NULL && strstr(found, "asmtest-stealth-helper") != NULL) {
        snprintf(bundled, sizeof bundled, "%s", found);
        CHECK(1, "ptrace stealth: dladdr discovers the bundled helper sibling");
    } else {
        printf("# SKIP stealth discovery: no bundled helper next to the test "
               "binary (build it via `make stealth-helper`)\n");
    }

    /* (a) in-process forked-child fallback: an unrunnable override forces it, so
     * this re-runs the original proven anonymous-mmap path as a regression check. */
    setenv("ASMTEST_STEALTH_HELPER", "/nonexistent/asmtest-stealth-helper", 1);
    int skipped = stealth_trace_once("fork", p);

    /* (b) the bundled exec'd-binary path: point the override at the real helper —
     * memfd hand-off + fork+execv — and assert byte-identical offsets. */
    if (!skipped && bundled[0] != '\0') {
        setenv("ASMTEST_STEALTH_HELPER", bundled, 1);
        stealth_trace_once("bundled", p);
    }
    unsetenv("ASMTEST_STEALTH_HELPER");
    munmap(p, sizeof ROUTINE);
#else
    printf("# SKIP ptrace stealth: needs Linux x86-64\n");
#endif
}

/* JIT method resolution: a JIT writes /tmp/perf-<pid>.map so perf can symbolize its
 * generated code; we parse it to recover a method's (base,len) for trace_attached.
 * Emulate one entry (with a spaces-bearing symbol) and resolve it by name. */
static void test_perfmap_resolve(void) {
    /* Pure /proc + perf-file parsing — arch-independent, so it runs on ANY Linux
     * (it needs no PTRACE_SINGLESTEP, unlike the stepper above). */
#if defined(__linux__)
    char path[64];
    snprintf(path, sizeof path, "/tmp/perf-%d.map", (int)getpid());
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        printf("# SKIP perfmap: cannot write %s\n", path);
        return;
    }
    fprintf(f, "400000 1a void asmtest::jit::demo(long, long)\n");
    fprintf(f, "500000 8 other_stub\n");
    fclose(f);

    void *base = NULL;
    size_t len = 0;
    int rc = asmtest_proc_perfmap_symbol(
        getpid(), "void asmtest::jit::demo(long, long)", &base, &len);
    CHECK(rc == ASMTEST_PTRACE_OK, "perfmap: resolved the JIT method by name");
    CHECK(
        base == (void *)0x400000 && len == 0x1a,
        "perfmap: base/len match the entry (symbol with spaces parsed whole)");
    int rc2 =
        asmtest_proc_perfmap_symbol(getpid(), "no_such_method", &base, &len);
    CHECK(rc2 == ASMTEST_PTRACE_ENOENT, "perfmap: missing symbol -> ENOENT");
    remove(path);
#else
    printf("# SKIP perfmap: not Linux\n");
#endif
}

#if defined(__linux__)
/* Serialize one jitdump JIT_CODE_LOAD record (host is little-endian, which the reader
 * detects from the header magic). Layout: prefix{id,total,ts} + body{pid,tid,vma,
 * code_addr,code_size,code_index} + name(NUL) + native code. */
static void write_jit_load(FILE *f, uint64_t ts, uint64_t addr, uint64_t idx,
                           const char *name, const void *code,
                           uint32_t code_size, uint32_t pid) {
    uint32_t id = 0; /* JIT_CODE_LOAD */
    uint32_t namelen = (uint32_t)strlen(name) + 1;
    uint32_t total = 16 + 40 + namelen + code_size;
    uint64_t vma = addr, code_addr = addr, csz = code_size;
    fwrite(&id, 4, 1, f);
    fwrite(&total, 4, 1, f);
    fwrite(&ts, 8, 1, f);
    fwrite(&pid, 4, 1, f);
    fwrite(&pid, 4, 1, f); /* tid */
    fwrite(&vma, 8, 1, f);
    fwrite(&code_addr, 8, 1, f);
    fwrite(&csz, 8, 1, f);
    fwrite(&idx, 8, 1, f);
    fwrite(name, 1, namelen, f);
    fwrite(code, 1, code_size, f);
}
#endif

/* Binary jitdump reader: the bytes-accurate, time-stamped code image a JIT writes
 * (jit-<pid>.dump). Synthesize one with a skipped non-LOAD record and the SAME method
 * re-JITted at a new address, and assert the reader resolves the latest body — name ->
 * (addr, size, code_index) AND the recorded native bytes (which the text perf-map
 * cannot give), latest-timestamp wins. */
static void test_jitdump_reader(void) {
    /* Arch-independent binary parsing (the recorded code bytes are an opaque payload),
     * so it validates on ANY Linux — no single-step needed. */
#if defined(__linux__)
    char path[80];
    snprintf(path, sizeof path, "/tmp/asmtest-jit-%d.dump", (int)getpid());
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("# SKIP jitdump: cannot write %s\n", path);
        return;
    }
    uint32_t magic = 0x4A695444u, version = 1, hsize = 40, elf_mach = 62,
             pad1 = 0, mpid = (uint32_t)getpid();
    uint64_t z = 0;
    fwrite(&magic, 4, 1, f);
    fwrite(&version, 4, 1, f);
    fwrite(&hsize, 4, 1, f);
    fwrite(&elf_mach, 4, 1, f);
    fwrite(&pad1, 4, 1, f);
    fwrite(&mpid, 4, 1, f);
    fwrite(&z, 8, 1, f); /* timestamp */
    fwrite(&z, 8, 1, f); /* flags */
    /* A non-LOAD record (id=4, JIT_CODE_UNWINDING_INFO) with an 8-byte payload, to
     * exercise record skipping. */
    {
        uint32_t id = 4, tot = 24;
        uint64_t ts = 1, payload = 0;
        fwrite(&id, 4, 1, f);
        fwrite(&tot, 4, 1, f);
        fwrite(&ts, 8, 1, f);
        fwrite(&payload, 8, 1, f);
    }
    const char *name = "void asmtest::jit::method(long, long)";
    write_jit_load(f, 2, 0x1000, 7, name, ROUTINE, (uint32_t)sizeof ROUTINE,
                   mpid);
    /* The SAME method re-JITted at a new address (tiered/OSR) — latest must win. */
    write_jit_load(f, 3, 0x2000, 9, name, ROUTINE, (uint32_t)sizeof ROUTINE,
                   mpid);
    fclose(f);

    asmtest_jitdump_entry_t e;
    memset(&e, 0, sizeof e);
    uint8_t bytes[64];
    size_t blen = 0;
    int rc =
        asmtest_jitdump_find(path, 0, name, &e, bytes, sizeof bytes, &blen);
    CHECK(rc == ASMTEST_PTRACE_OK,
          "jitdump: found the method by name (non-LOAD record skipped)");
    CHECK(e.code_addr == 0x2000 && e.timestamp == 3,
          "jitdump: re-JIT latest body wins (addr 0x2000, ts 3)");
    CHECK(e.code_size == sizeof ROUTINE && e.code_index == 9,
          "jitdump: code_size/code_index parsed from the record");
    CHECK(blen == sizeof ROUTINE && memcmp(bytes, ROUTINE, sizeof ROUTINE) == 0,
          "jitdump: captured the recorded native bytes (not just symbol+size)");
    int rc2 = asmtest_jitdump_find(path, 0, "no_such", &e, NULL, 0, NULL);
    CHECK(rc2 == ASMTEST_PTRACE_ENOENT, "jitdump: missing method -> ENOENT");
    remove(path);
#else
    printf("# SKIP jitdump: not Linux\n");
#endif
}

/* Phase 1 (call-descent): the two new Capstone queries the descent loop needs —
 * asmtest_disas_is_ret (the pop-predicate's third term) and asmtest_disas_call_target
 * (the direct-call-target query — a public helper; the descent loop itself reads the callee
 * from the live post-step PC, but the query is exercised here for both arches). The
 * disassembly layer is cross-arch, so both the x86-64 and the AArch64 encodings are
 * decoded here on ANY Capstone host (no single-step needed), exactly like the emulator
 * tier decodes foreign bytes. Also spot-checks is_call for symmetry. */
static void test_disas_queries(void) {
    if (!asmtest_disas_available()) {
        printf("# SKIP disas queries (is_ret/call_target): needs Capstone\n");
        return;
    }
    const uint64_t BASE = 0x1000;

    /* x86-64: [0] call rel32(+4)  [5] add rax,rsi  [8] ret  [9] call rax (indirect). */
    static const unsigned char X[] = {0xe8, 0x04, 0x00, 0x00, 0x00, 0x48,
                                      0x01, 0xf0, 0xc3, 0xff, 0xd0};
    const size_t XL = sizeof X;
    uint64_t t = 0;
    CHECK(asmtest_disas_is_call(ASMTEST_ARCH_X86_64, X, XL, 0) == 1,
          "disas x86: call rel32 is a call");
    CHECK(asmtest_disas_is_ret(ASMTEST_ARCH_X86_64, X, XL, 0) == 0,
          "disas x86: call is not a ret");
    CHECK(asmtest_disas_call_target(ASMTEST_ARCH_X86_64, X, XL, BASE, 0, &t) ==
                  1 &&
              t == BASE + 5 + 4,
          "disas x86: direct call target = base+insnlen+rel32 (0x1009)");
    CHECK(asmtest_disas_is_call(ASMTEST_ARCH_X86_64, X, XL, 5) == 0,
          "disas x86: add is not a call");
    CHECK(asmtest_disas_is_ret(ASMTEST_ARCH_X86_64, X, XL, 8) == 1,
          "disas x86: ret is a ret");
    t = 0xdead;
    CHECK(asmtest_disas_is_call(ASMTEST_ARCH_X86_64, X, XL, 9) == 1 &&
              asmtest_disas_call_target(ASMTEST_ARCH_X86_64, X, XL, BASE, 9,
                                        &t) == 0,
          "disas x86: indirect call (call rax) has no direct target");

    /* AArch64: [0] bl #0x10  [4] add x0,x0,x1  [8] ret  [12] blr x0 (indirect). */
    static const unsigned char A[] = {0x04, 0x00, 0x00, 0x94, 0x00, 0x00,
                                      0x01, 0x8b, 0xc0, 0x03, 0x5f, 0xd6,
                                      0x00, 0x00, 0x3f, 0xd6};
    const size_t AL = sizeof A;
    t = 0;
    CHECK(asmtest_disas_is_call(ASMTEST_ARCH_ARM64, A, AL, 0) == 1,
          "disas arm64: bl is a call");
    CHECK(asmtest_disas_call_target(ASMTEST_ARCH_ARM64, A, AL, BASE, 0, &t) ==
                  1 &&
              t == BASE + 0x10,
          "disas arm64: bl target = base + imm26<<2 (0x1010)");
    CHECK(asmtest_disas_is_ret(ASMTEST_ARCH_ARM64, A, AL, 4) == 0,
          "disas arm64: add is not a ret");
    CHECK(asmtest_disas_is_ret(ASMTEST_ARCH_ARM64, A, AL, 8) == 1,
          "disas arm64: ret is a ret");
    t = 0xdead;
    CHECK(asmtest_disas_is_call(ASMTEST_ARCH_ARM64, A, AL, 12) == 1 &&
              asmtest_disas_call_target(ASMTEST_ARCH_ARM64, A, AL, BASE, 12,
                                        &t) == 0,
          "disas arm64: indirect call (blr x0) has no direct target");
}

#if defined(__linux__) && defined(__x86_64__)
static void noop_sigalrm(int s) { (void)s; }
#endif

/* Map `blob` (`n` bytes) as private R+X executable memory, or NULL on failure. */
static void *map_exec(const void *blob, size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    memcpy(p, blob, n);
    mprotect(p, n, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + n);
    return p;
}

/* Phase 2 (call-descent): the descent handle exists, allocates, reads back empty, and
 * frees idempotently; the _ex entry points reproduce the flat trace exactly whether the
 * descent handle is NULL or OFF (the loop is still level-0-only until Phase 3). */
static void test_descent_handle(void) {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    /* Accessors are NULL-safe. */
    CHECK(asmtest_descent_frames_len(NULL) == 0 &&
              asmtest_descent_edges_len(NULL) == 0 &&
              asmtest_descent_truncated(NULL) == 0 &&
              asmtest_descent_depth_capped(NULL) == 0,
          "descent: NULL handle accessors return 0");

    asmtest_descent_t *d = asmtest_descent_new(ASMTEST_DESCENT_OFF);
    CHECK(d != NULL, "descent: asmtest_descent_new allocates a handle");
    CHECK(asmtest_descent_frames_len(d) == 0 &&
              asmtest_descent_edges_len(d) == 0,
          "descent: fresh handle reads back empty");
    CHECK(asmtest_descent_frame_base(d, 0) == 0 &&
              asmtest_descent_edge_target(d, 5) == 0,
          "descent: out-of-range accessors return 0");
    CHECK(asmtest_descent_allow_region(d, (void *)0x1000, 0x40) ==
              ASMTEST_PTRACE_OK,
          "descent: allow_region succeeds");

    if (!asmtest_ptrace_available()) {
        char why[160];
        asmtest_ptrace_skip_reason(why, sizeof why);
        printf("# SKIP descent handle _ex trace: %s\n", why);
        asmtest_descent_free(d);
        asmtest_descent_free(NULL); /* idempotent / NULL-safe */
        return;
    }

#if defined(__aarch64__)
    const unsigned char *RT = ROUTINE_A64;
    const size_t RTN = sizeof ROUTINE_A64;
    static const uint64_t EXP[] = {0x0, 0x4, 0x8, 0x10};
#else
    const unsigned char *RT = ROUTINE;
    const size_t RTN = sizeof ROUTINE;
    static const uint64_t EXP[] = {0x0, 0x3, 0x6, 0xc, 0x11};
#endif
    const size_t NEXP = sizeof EXP / sizeof EXP[0];
    void *p = map_exec(RT, RTN);
    if (p == NULL) {
        printf("# SKIP descent handle _ex trace: mmap failed\n");
        asmtest_descent_free(d);
        return;
    }
    const long args[2] = {20, 22};

    /* _ex with descent == NULL is identical to the plain call. */
    asmtest_trace_t *t0 = asmtest_trace_new(64, 64);
    long r0 = 0;
    int rc0 = asmtest_ptrace_trace_call_ex(p, RTN, args, 2, &r0, t0, NULL);
    int ok0 = (rc0 == ASMTEST_PTRACE_OK && r0 == 42 &&
               asmtest_emu_trace_insns_total(t0) == NEXP);
    for (size_t i = 0; ok0 && i < NEXP; i++)
        ok0 = (t0->insns[i] == EXP[i]);
    CHECK(ok0, "descent: _ex(descent=NULL) reproduces the flat trace");

    /* _ex with an OFF handle fills the flat trace and leaves the handle empty. */
    asmtest_trace_t *t1 = asmtest_trace_new(64, 64);
    long r1 = 0;
    int rc1 = asmtest_ptrace_trace_call_ex(p, RTN, args, 2, &r1, t1, d);
    int ok1 = (rc1 == ASMTEST_PTRACE_OK && r1 == 42 &&
               asmtest_emu_trace_insns_total(t1) == NEXP);
    for (size_t i = 0; ok1 && i < NEXP; i++)
        ok1 = (t1->insns[i] == EXP[i]);
    CHECK(ok1, "descent: _ex(level=OFF) still fills the flat trace");
    CHECK(asmtest_descent_frames_len(d) == 0 &&
              asmtest_descent_edges_len(d) == 0,
          "descent: OFF level records nothing into the handle");

    asmtest_trace_free(t0);
    asmtest_trace_free(t1);
    munmap(p, RTN);
    asmtest_descent_free(d);
    asmtest_descent_free(NULL); /* idempotent / NULL-safe */
#else
    printf("# SKIP descent handle: not Linux x86-64/AArch64\n");
#endif
}

/* Match a descent frame's ordered instruction stream against `exp`. */
static int frame_insns_eq(asmtest_descent_t *d, size_t f, const uint64_t *exp,
                          size_t nexp) {
    if (asmtest_descent_frame_insn_count(d, f) != nexp)
        return 0;
    for (size_t i = 0; i < nexp; i++)
        if (asmtest_descent_frame_insn_at(d, f, i) != exp[i])
            return 0;
    return 1;
}

/* Phases 3-5 (call-descent): the fork-path descending step loop across all four levels.
 * One in-page fixture (R calls sibling S and sibling K) drives L1 (edges) and L2 (descend
 * the known S, step over the unknown K); a separate-mapping fixture drives L3 (descend
 * everything, extent from /proc/maps) plus the denylist and budget guards; a self-calling
 * fixture drives the same-region recursion frame + the max_depth cap. x86-64 machine code;
 * the loop itself is arch-neutral (AArch64 rides the same path on real hardware). */
static void test_descent_fork(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_ptrace_available() || !asmtest_disas_available()) {
        printf("# SKIP descent fork: needs ptrace single-step + Capstone\n");
        return;
    }
    const long args[2] = {20, 22};
    long r = 0;

    /* R@0: mov rax,rdi; call S; call K; add rax,rsi; ret   S@0x11: inc rax; ret
     * K@0x15: dec rax; ret.  R region = [0,0x11); S,K are out-of-region call-outs. */
    static const unsigned char BLOB1[] = {
        0x48, 0x89, 0xf8,             /* 0x00 mov rax,rdi   */
        0xe8, 0x09, 0x00, 0x00, 0x00, /* 0x03 call S(0x11)  */
        0xe8, 0x08, 0x00, 0x00, 0x00, /* 0x08 call K(0x15)  */
        0x48, 0x01, 0xf0,             /* 0x0d add rax,rsi   */
        0xc3,                         /* 0x10 ret           */
        0x48, 0xff, 0xc0, 0xc3,       /* 0x11 S: inc rax;ret*/
        0x48, 0xff, 0xc8, 0xc3};      /* 0x15 K: dec rax;ret*/
    const size_t REGION_R = 0x11, S_OFF = 0x11, K_OFF = 0x15, LEAF_LEN = 4;
    static const uint64_t R_STREAM[] = {0x0, 0x3, 0x8, 0xd, 0x10};
    static const uint64_t S_STREAM[] = {0x0, 0x3};

    /* --- Phase 3: Level 1 RECORD_EDGES (both calls stepped over, edges recorded). --- */
    void *b1 = map_exec(BLOB1, sizeof BLOB1);
    if (b1 == NULL) {
        printf("# SKIP descent fork: mmap failed\n");
        return;
    }
    uint64_t base1 = (uint64_t)(uintptr_t)b1;
    {
        asmtest_descent_t *d =
            asmtest_descent_new(ASMTEST_DESCENT_RECORD_EDGES);
        asmtest_trace_t *t = asmtest_trace_new(64, 64);
        r = 0;
        int rc = asmtest_ptrace_trace_call_ex(b1, REGION_R, args, 2, &r, t, d);
        int flat_ok = (rc == ASMTEST_PTRACE_OK && r == 42 &&
                       asmtest_emu_trace_insns_total(t) == 5);
        for (size_t i = 0; flat_ok && i < 5; i++)
            flat_ok = (t->insns[i] == R_STREAM[i]);
        CHECK(
            flat_ok,
            "descent L1: flat trace is R's own body (both calls stepped over)");
        CHECK(
            asmtest_descent_frames_len(d) == 1 &&
                frame_insns_eq(d, 0, R_STREAM, 5),
            "descent L1: frame 0 mirrors the flat trace, no descended frames");
        int e_ok = (asmtest_descent_edges_len(d) == 2 &&
                    asmtest_descent_edge_site(d, 0) == 0x3 &&
                    asmtest_descent_edge_target(d, 0) == base1 + S_OFF &&
                    asmtest_descent_edge_site(d, 1) == 0x8 &&
                    asmtest_descent_edge_target(d, 1) == base1 + K_OFF &&
                    asmtest_descent_edge_depth(d, 0) == 0);
        CHECK(e_ok, "descent L1: two edges (call->S, call->K) with correct "
                    "sites/targets");
        CHECK(!asmtest_descent_truncated(d),
              "descent L1: complete (not truncated)");
        asmtest_trace_free(t);
        asmtest_descent_free(d);
    }

    /* --- Phase 4: Level 2 DESCEND_KNOWN (descend S via allow-set, step over K). --- */
    {
        asmtest_descent_t *d =
            asmtest_descent_new(ASMTEST_DESCENT_DESCEND_KNOWN);
        asmtest_descent_allow_region(d, (void *)(uintptr_t)(base1 + S_OFF),
                                     LEAF_LEN);
        asmtest_trace_t *t = asmtest_trace_new(64, 64);
        r = 0;
        int rc = asmtest_ptrace_trace_call_ex(b1, REGION_R, args, 2, &r, t, d);
        int flat_ok = (rc == ASMTEST_PTRACE_OK && r == 42 &&
                       asmtest_emu_trace_insns_total(t) == 5);
        for (size_t i = 0; flat_ok && i < 5; i++)
            flat_ok = (t->insns[i] == R_STREAM[i]);
        CHECK(flat_ok, "descent L2: frame-0 flat body byte-identical to L1 (S "
                       "not folded)");
        CHECK(asmtest_descent_frames_len(d) == 2 &&
                  frame_insns_eq(d, 0, R_STREAM, 5),
              "descent L2: two frames (R + descended S)");
        int f1_ok = (asmtest_descent_frame_base(d, 1) == base1 + S_OFF &&
                     asmtest_descent_frame_depth(d, 1) == 1 &&
                     asmtest_descent_frame_parent(d, 1) == 0 &&
                     frame_insns_eq(d, 1, S_STREAM, 2));
        CHECK(f1_ok,
              "descent L2: frame 1 is S's own body (inc; ret) at depth 1");
        CHECK(asmtest_descent_edges_len(d) == 1 &&
                  asmtest_descent_edge_target(d, 0) == base1 + K_OFF,
              "descent L2: unknown K still stepped over as an edge (not "
              "descended)");
        asmtest_trace_free(t);
        asmtest_descent_free(d);
    }
    munmap(b1, sizeof BLOB1);

    /* --- Phase 4: same-region recursion is a distinct frame; max_depth caps it. --- */
    /* rec(n)@0: test rdi,rdi; je ret; dec rdi; call rec; ret   (region = [0,0xe)). */
    static const unsigned char REC[] = {
        0x48, 0x85, 0xff,             /* 0x00 test rdi,rdi        */
        0x74, 0x08,                   /* 0x03 je 0xd              */
        0x48, 0xff, 0xcf,             /* 0x05 dec rdi             */
        0xe8, 0xf3, 0xff, 0xff, 0xff, /* 0x08 call rec (->0)      */
        0xc3};                        /* 0x0d ret                 */
    const size_t REGION_REC = sizeof REC;
    void *br = map_exec(REC, sizeof REC);
    if (br != NULL) {
        {
            asmtest_descent_t *d =
                asmtest_descent_new(ASMTEST_DESCENT_DESCEND_KNOWN);
            long a2[1] = {2};
            r = 0;
            int rc = asmtest_ptrace_trace_call_ex(br, REGION_REC, a2, 1, &r,
                                                  NULL, d);
            /* rec(2) recurses to depth 2 -> frames at depth 0,1,2. */
            int ok =
                (rc == ASMTEST_PTRACE_OK &&
                 asmtest_descent_frames_len(d) == 3 &&
                 asmtest_descent_frame_depth(d, 0) == 0 &&
                 asmtest_descent_frame_depth(d, 1) == 1 &&
                 asmtest_descent_frame_depth(d, 2) == 2 &&
                 asmtest_descent_frame_base(d, 1) == (uint64_t)(uintptr_t)br &&
                 asmtest_descent_frame_parent(d, 2) == 1);
            CHECK(ok, "descent recursion: same-region self-call is a distinct "
                      "nested frame");
            asmtest_descent_free(d);
        }
        {
            asmtest_descent_t *d =
                asmtest_descent_new(ASMTEST_DESCENT_DESCEND_KNOWN);
            asmtest_descent_set_max_depth(d, 2);
            long a5[1] = {5};
            r = 0;
            int rc = asmtest_ptrace_trace_call_ex(br, REGION_REC, a5, 1, &r,
                                                  NULL, d);
            /* Depth ceiling 2: frames at depth 0,1,2 only; deeper folds + flags capped. */
            int ok = (rc == ASMTEST_PTRACE_OK &&
                      asmtest_descent_frames_len(d) == 3 &&
                      asmtest_descent_depth_capped(d) == 1);
            CHECK(ok, "descent max_depth: recursion capped at the depth "
                      "ceiling (flagged)");
            asmtest_descent_free(d);
        }
        munmap(br, sizeof REC);
    }

    /* --- Phase 5: Level 3 DESCEND_ALL into a separate-mapping helper + guards. --- */
    /* M (own page): inc rax; ret.  R2 (own page): mov rax,rdi; movabs r11,M; call r11;
     * add rax,rsi; ret.  The indirect call keeps M in a DISTINCT mapping (so L3's
     * /proc/maps extent for M does not overlap R2 — the in-page-sibling hazard). */
    static const unsigned char M_BLOB[] = {0x48, 0xff, 0xc0,
                                           0xc3}; /* inc rax; ret */
    void *m = map_exec(M_BLOB, sizeof M_BLOB);
    unsigned char R2[] = {0x48, 0x89, 0xf8, /* mov rax,rdi          */
                          0x49, 0xbb, 0,    0, 0,
                          0,    0,    0,    0, 0, /* movabs r11, <M>      */
                          0x41, 0xff, 0xd3,       /* call r11             */
                          0x48, 0x01, 0xf0,       /* add rax,rsi          */
                          0xc3};                  /* ret                  */
    if (m != NULL) {
        uint64_t maddr = (uint64_t)(uintptr_t)m;
        memcpy(&R2[5], &maddr, sizeof maddr); /* patch the movabs immediate */
        const size_t REGION_R2 = sizeof R2, CALL_SITE2 = 0xd;
        void *b2 = map_exec(R2, sizeof R2);
        if (b2 != NULL) {
            /* L3: descend into M (resolved from /proc/maps). */
            asmtest_descent_t *d =
                asmtest_descent_new(ASMTEST_DESCENT_DESCEND_ALL);
            r = 0;
            int rc = asmtest_ptrace_trace_call_ex(b2, REGION_R2, args, 2, &r,
                                                  NULL, d);
            int ok = (rc == ASMTEST_PTRACE_OK && r == 43 &&
                      asmtest_descent_frames_len(d) == 2 &&
                      asmtest_descent_frame_base(d, 1) <= maddr &&
                      asmtest_descent_frame_depth(d, 1) == 1);
            CHECK(ok, "descent L3: descends an arbitrary callee (extent from "
                      "/proc/maps)");
            asmtest_descent_free(d);

            /* L3 denylist: deny M's page -> stepped over, recorded as an edge. */
            d = asmtest_descent_new(ASMTEST_DESCENT_DESCEND_ALL);
            asmtest_descent_deny_region(d, m, sizeof M_BLOB);
            r = 0;
            rc = asmtest_ptrace_trace_call_ex(b2, REGION_R2, args, 2, &r, NULL,
                                              d);
            int deny_ok = (rc == ASMTEST_PTRACE_OK && r == 43 &&
                           asmtest_descent_frames_len(d) == 1 &&
                           asmtest_descent_edges_len(d) == 1 &&
                           asmtest_descent_edge_site(d, 0) == CALL_SITE2);
            CHECK(deny_ok, "descent L3 denylist: a denied callee is stepped "
                           "over, not descended");
            asmtest_descent_free(d);

            /* L3 built-in default denylist (plan Phase 5): a call landing exactly on a
             * blocking libc entry point (poll, resolved with dlsym just as the backend
             * resolves it) is stepped over as an edge, not descended — with NO caller-
             * supplied deny region. poll(0,0,0) returns immediately, so the step-over
             * completes fast; rax is then set to 42 for a deterministic result. */
            void *poll_addr = dlsym(RTLD_DEFAULT, "poll");
            if (poll_addr != NULL) {
                unsigned char R3[] = {
                    0x31, 0xff, /* xor edi,edi          */
                    0x31, 0xf6, /* xor esi,esi          */
                    0x31, 0xd2, /* xor edx,edx          */
                    0x49, 0xbb, 0,    0,    0,
                    0,    0,    0,    0,    0,    /* movabs r11, <poll>   */
                    0x41, 0xff, 0xd3,             /* 0x10 call r11        */
                    0xb8, 0x2a, 0x00, 0x00, 0x00, /* mov eax,42           */
                    0xc3};                        /* ret                  */
                memcpy(&R3[8], &poll_addr, sizeof poll_addr);
                void *b3 = map_exec(R3, sizeof R3);
                if (b3 != NULL) {
                    d = asmtest_descent_new(ASMTEST_DESCENT_DESCEND_ALL);
                    asmtest_descent_use_default_denylist(d);
                    r = 0;
                    rc = asmtest_ptrace_trace_call_ex(b3, sizeof R3, NULL, 0,
                                                      &r, NULL, d);
                    int def_ok = (rc == ASMTEST_PTRACE_OK && r == 42 &&
                                  asmtest_descent_frames_len(d) == 1 &&
                                  asmtest_descent_edges_len(d) == 1 &&
                                  asmtest_descent_edge_site(d, 0) == 0x10 &&
                                  asmtest_descent_edge_target(d, 0) ==
                                      (uint64_t)(uintptr_t)poll_addr);
                    CHECK(def_ok,
                          "descent L3 default denylist: a blocking libc entry "
                          "(poll) is stepped over, not descended");
                    asmtest_descent_free(d);
                    munmap(b3, sizeof R3);
                }
            }

            /* Budget guard: a tiny instruction budget declines descent (depth_capped). */
            d = asmtest_descent_new(ASMTEST_DESCENT_DESCEND_ALL);
            asmtest_descent_set_insn_budget(d, 1);
            r = 0;
            rc = asmtest_ptrace_trace_call_ex(b2, REGION_R2, args, 2, &r, NULL,
                                              d);
            int bud_ok = (rc == ASMTEST_PTRACE_OK &&
                          asmtest_descent_frames_len(d) == 1 &&
                          asmtest_descent_depth_capped(d) == 1);
            CHECK(bud_ok, "descent L3 budget: an exhausted budget declines "
                          "descent (flagged)");
            asmtest_descent_free(d);
            munmap(b2, sizeof R2);
        }
        munmap(m, sizeof M_BLOB);
    }

    /* --- Phase 5: the watchdog terminates a descent that never returns (no hang). --- */
    /* SPIN (own page): jmp $ (an infinite 1-insn loop).  RS (own page): mov rax,rdi;
     * movabs r11,SPIN; call r11; ret.  Descending SPIN at L3 with a short watchdog must
     * self-truncate on the deadline rather than single-step forever. */
    static const unsigned char SPIN_BLOB[] = {0xeb, 0xfe}; /* jmp $ */
    void *sp = map_exec(SPIN_BLOB, sizeof SPIN_BLOB);
    unsigned char RS[] = {0x48, 0x89, 0xf8, /* mov rax,rdi     */
                          0x49, 0xbb, 0,    0, 0,
                          0,    0,    0,    0, 0, /* movabs r11,SPIN */
                          0x41, 0xff, 0xd3,       /* call r11        */
                          0xc3};                  /* ret             */
    if (sp != NULL) {
        uint64_t saddr = (uint64_t)(uintptr_t)sp;
        memcpy(&RS[5], &saddr, sizeof saddr);
        void *bs = map_exec(RS, sizeof RS);
        if (bs != NULL) {
            asmtest_descent_t *d =
                asmtest_descent_new(ASMTEST_DESCENT_DESCEND_ALL);
            asmtest_descent_set_insn_budget(
                d, 1u << 20); /* don't decline on budget */
            asmtest_descent_set_watchdog_ms(d, 60);
            r = 0;
            int rc = asmtest_ptrace_trace_call_ex(bs, sizeof RS, args, 2, &r,
                                                  NULL, d);
            /* Reaching this CHECK at all proves it did not hang. */
            int wd_ok =
                (rc != ASMTEST_PTRACE_OK && asmtest_descent_truncated(d) &&
                 asmtest_descent_depth_capped(d) &&
                 asmtest_descent_frames_len(d) >= 2);
            CHECK(wd_ok, "descent watchdog: a non-returning descent "
                         "self-terminates (no hang)");
            asmtest_descent_free(d);
            munmap(bs, sizeof RS);
        }
        munmap(sp, sizeof SPIN_BLOB);
    }
#else
    printf("# SKIP descent fork: x86-64 Linux only (fork fixtures)\n");
#endif
}

/* Regression (call-descent): after an L3 descent trips the watchdog (leaving the internal
 * alarm flag set), a later L2 descent must NOT be aborted by that stale flag when its own
 * waitpid is interrupted by an UNRELATED signal (a host process's own repeating timer). The
 * L3-only watchdog gate means L1/L2 must ignore the flag; before that fix a stale flag +
 * EINTR spuriously truncated a healthy trace. */
static void test_descent_stale_alarm_flag(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_ptrace_available() || !asmtest_disas_available()) {
        printf("# SKIP descent stale-alarm: needs ptrace single-step + "
               "Capstone\n");
        return;
    }
    /* (1) Trip the L3 watchdog on a non-returning callee, setting the internal flag. */
    static const unsigned char SPIN[] = {0xeb, 0xfe}; /* jmp $ */
    void *sp = map_exec(SPIN, sizeof SPIN);
    unsigned char RS[] = {0x48, 0x89, 0xf8, 0x49, 0xbb, 0,    0,    0,   0,
                          0,    0,    0,    0,    0x41, 0xff, 0xd3, 0xc3};
    if (sp != NULL) {
        uint64_t saddr = (uint64_t)(uintptr_t)sp;
        memcpy(&RS[5], &saddr, sizeof saddr);
        void *bs = map_exec(RS, sizeof RS);
        if (bs != NULL) {
            asmtest_descent_t *d =
                asmtest_descent_new(ASMTEST_DESCENT_DESCEND_ALL);
            asmtest_descent_set_insn_budget(d, 1u << 20);
            asmtest_descent_set_watchdog_ms(d, 50);
            long r = 0;
            const long a[2] = {1, 2};
            asmtest_ptrace_trace_call_ex(bs, sizeof RS, a, 2, &r, NULL, d);
            asmtest_descent_free(d);
            munmap(bs, sizeof RS);
        }
        munmap(sp, sizeof SPIN);
    }

    /* (2) Install an unrelated repeating SIGALRM so the next descent's waitpid gets EINTR. */
    struct sigaction old_sa;
    struct itimerval old_it, it;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = noop_sigalrm; /* no SA_RESTART -> interrupts waitpid */
    sigaction(SIGALRM, &sa, &old_sa);
    it.it_value.tv_sec = 0;
    it.it_value.tv_usec =
        200; /* fire quickly + repeatedly during the L2 descent */
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 200;
    setitimer(ITIMER_REAL, &it, &old_it);

    /* (3) A healthy L2 descent must complete despite the stale flag + the EINTRs.
     * The descent's real-time deadline is raised well past the default 2 s: the 200us
     * signal storm can stretch the few-step descent past 2 s on a loaded CI runner,
     * and a deadline trip is a LEGITIMATE truncation — not the stale-flag regression
     * this test guards. With the deadline out of reach, only the stale-flag bug (the
     * L1/L2 gate on the L3-owned alarm flag) can fail the assertion. */
    static const unsigned char BLOB1[] = {0x48, 0x89, 0xf8, 0xe8, 0x04, 0x00,
                                          0x00, 0x00, 0x48, 0x01, 0xf0, 0xc3,
                                          0x48, 0xff, 0xc0, 0xc3};
    void *b = map_exec(BLOB1, sizeof BLOB1);
    int ok = 0;
    if (b != NULL) {
        uint64_t base = (uint64_t)(uintptr_t)b;
        asmtest_descent_t *d =
            asmtest_descent_new(ASMTEST_DESCENT_DESCEND_KNOWN);
        asmtest_descent_set_watchdog_ms(d, 60000);
        asmtest_descent_allow_region(d, (void *)(uintptr_t)(base + 0xc), 4);
        long r = 0;
        const long a[2] = {20, 22};
        int rc = asmtest_ptrace_trace_call_ex(b, 0xc, a, 2, &r, NULL, d);
        ok = (rc == ASMTEST_PTRACE_OK && r == 43 &&
              asmtest_descent_frames_len(d) == 2 &&
              !asmtest_descent_truncated(d));
        asmtest_descent_free(d);
        munmap(b, sizeof BLOB1);
    }

    /* (4) Restore the caller's signal/timer state. */
    setitimer(ITIMER_REAL, &old_it, NULL);
    sigaction(SIGALRM, &old_sa, NULL);

    CHECK(ok, "descent stale-alarm: L2 completes despite a stale L3 watchdog "
              "flag + EINTRs");
#else
    printf("# SKIP descent stale-alarm: x86-64 Linux only\n");
#endif
}

/* Phase 8 (call-descent): cascade invariants + the honest limitation. Frame-0's body is
 * byte-identical across all levels (higher levels only ADD directly-reachable frames), and
 * a known region reachable only THROUGH a stepped-over intermediary is invisible to
 * descent — the documented `recorded(L2) is not a superset of recorded(L3)` caveat. */
static void test_descent_cascade(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_ptrace_available() || !asmtest_disas_available()) {
        printf("# SKIP descent cascade: needs ptrace single-step + Capstone\n");
        return;
    }
    const long args[2] = {20, 22};
    /* R@0 calls sibling S and sibling K; both out of R's region [0,0x11). */
    static const unsigned char BLOB1[] = {
        0x48, 0x89, 0xf8, 0xe8, 0x09, 0x00, 0x00, 0x00, 0xe8,
        0x08, 0x00, 0x00, 0x00, 0x48, 0x01, 0xf0, 0xc3, 0x48,
        0xff, 0xc0, 0xc3, 0x48, 0xff, 0xc8, 0xc3};
    const size_t REGION_R = 0x11, S_OFF = 0x11, LEAF_LEN = 4;
    static const uint64_t R_STREAM[] = {0x0, 0x3, 0x8, 0xd, 0x10};
    void *b = map_exec(BLOB1, sizeof BLOB1);
    if (b == NULL) {
        printf("# SKIP descent cascade: mmap failed\n");
        return;
    }
    uint64_t base = (uint64_t)(uintptr_t)b;

    /* Frame-0 body is identical at L1, L2, L3; frame count grows with reachability. */
    int frame0_same = 1, counts_ok = 1;
    size_t want_frames[3] = {1, 2, 3}; /* L1: R; L2: R+S; L3: R+S+K */
    asmtest_descent_level_t lv[3] = {ASMTEST_DESCENT_RECORD_EDGES,
                                     ASMTEST_DESCENT_DESCEND_KNOWN,
                                     ASMTEST_DESCENT_DESCEND_ALL};
    for (int i = 0; i < 3; i++) {
        asmtest_descent_t *d = asmtest_descent_new(lv[i]);
        if (lv[i] ==
            ASMTEST_DESCENT_DESCEND_KNOWN) /* only S known -> L2 adds one frame */
            asmtest_descent_allow_region(d, (void *)(uintptr_t)(base + S_OFF),
                                         LEAF_LEN);
        long r = 0;
        int rc =
            asmtest_ptrace_trace_call_ex(b, REGION_R, args, 2, &r, NULL, d);
        if (rc != ASMTEST_PTRACE_OK || !frame_insns_eq(d, 0, R_STREAM, 5))
            frame0_same = 0;
        if (asmtest_descent_frames_len(d) != want_frames[i])
            counts_ok = 0;
        asmtest_descent_free(d);
    }
    CHECK(frame0_same,
          "descent cascade: frame-0 body byte-identical across L1/L2/L3");
    CHECK(counts_ok,
          "descent cascade: higher levels add only directly-reachable frames");
    munmap(b, sizeof BLOB1);

    /* Limitation: R -> (unknown) I -> (known, allow-set) G. I is stepped over at L2, so its
     * call to G is invisible: G is NOT descended even though it is in the allow-set. */
    static const unsigned char CHAIN[] = {
        0x48, 0x89, 0xf8,             /* R@0  mov rax,rdi       */
        0xe8, 0x01, 0x00, 0x00, 0x00, /* R@3  call I (->9)      */
        0xc3,                         /* R@8  ret  (region=9)   */
        0xe8, 0x01, 0x00, 0x00, 0x00, /* I@9  call G (->0xf)    */
        0xc3,                         /* I@e  ret               */
        0x48, 0xff, 0xc0,             /* G@f  inc rax           */
        0xc3};                        /* G@12 ret               */
    const size_t REGION_RC = 9, I_OFF = 9, G_OFF = 0xf, G_LEN = 4;
    void *c = map_exec(CHAIN, sizeof CHAIN);
    if (c != NULL) {
        uint64_t cb = (uint64_t)(uintptr_t)c;
        asmtest_descent_t *d =
            asmtest_descent_new(ASMTEST_DESCENT_DESCEND_KNOWN);
        asmtest_descent_allow_region(d, (void *)(uintptr_t)(cb + G_OFF),
                                     G_LEN); /* known G */
        long r = 0;
        int rc =
            asmtest_ptrace_trace_call_ex(c, REGION_RC, args, 2, &r, NULL, d);
        /* G is behind the stepped-over I: only R is a frame, and the recorded edge is
         * R->I (never R->G / a G frame). */
        int ok = rc == ASMTEST_PTRACE_OK &&
                 asmtest_descent_frames_len(d) == 1 &&
                 asmtest_descent_edges_len(d) == 1 &&
                 asmtest_descent_edge_target(d, 0) == cb + I_OFF;
        CHECK(ok, "descent limitation: a known region behind a stepped-over "
                  "intermediary "
                  "is NOT recorded");
        asmtest_descent_free(d);
        munmap(c, sizeof CHAIN);
    }
#else
    printf("# SKIP descent cascade: x86-64 Linux only\n");
#endif
}

/* Phase 3-4 (call-descent): the ATTACHED path threads the same descending loop through a
 * foreign, externally-attached process (bytes read via process_vm_readv). Mirrors
 * test_ptrace_attach but descends the known sibling S at L2. */
static void test_descent_attach(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_ptrace_available() || !asmtest_disas_available()) {
        printf("# SKIP descent attach: needs ptrace single-step + Capstone\n");
        return;
    }
    static const unsigned char BLOB1[] = {
        0x48, 0x89, 0xf8, 0xe8, 0x09, 0x00, 0x00, 0x00, 0xe8,
        0x08, 0x00, 0x00, 0x00, 0x48, 0x01, 0xf0, 0xc3, 0x48,
        0xff, 0xc0, 0xc3, 0x48, 0xff, 0xc8, 0xc3};
    const size_t REGION_R = 0x11, S_OFF = 0x11, LEAF_LEN = 4;
    static const uint64_t R_STREAM[] = {0x0, 0x3, 0x8, 0xd, 0x10};
    static const uint64_t S_STREAM[] = {0x0, 0x3};

    volatile int *go = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    void *p = mmap(NULL, sizeof BLOB1, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (go == MAP_FAILED || p == MAP_FAILED) {
        printf("# SKIP descent attach: mmap failed\n");
        return;
    }
    *go = 0;
    memcpy(p, BLOB1, sizeof BLOB1);
    mprotect(p, sizeof BLOB1, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof BLOB1);
    uint64_t base = (uint64_t)(uintptr_t)p;

    pid_t pid = fork();
    if (pid == 0) {
        while (!*go) {
        }
        volatile long r = ((add2_fn)p)(20, 22);
        (void)r;
        _exit(0);
    }
    struct timespec ts = {0, 3 * 1000 * 1000};
    nanosleep(&ts, NULL);
    int status = 0;
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0 ||
        waitpid(pid, &status, 0) < 0) {
        printf("# SKIP descent attach: PTRACE_ATTACH not permitted (yama)\n");
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return;
    }
    *go = 1;

    asmtest_descent_t *d = asmtest_descent_new(ASMTEST_DESCENT_DESCEND_KNOWN);
    asmtest_descent_allow_region(d, (void *)(uintptr_t)(base + S_OFF),
                                 LEAF_LEN);
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    long result = 0;
    int rc = asmtest_ptrace_trace_attached_ex(pid, p, REGION_R, &result, tr, d);
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    waitpid(pid, &status, 0);

    int ok = (rc == ASMTEST_PTRACE_OK && result == 42 &&
              asmtest_emu_trace_insns_total(tr) == 5 &&
              asmtest_descent_frames_len(d) == 2 &&
              frame_insns_eq(d, 0, R_STREAM, 5) &&
              asmtest_descent_frame_base(d, 1) == base + S_OFF &&
              frame_insns_eq(d, 1, S_STREAM, 2));
    CHECK(ok, "descent attach: L2 descends the known sibling in a foreign "
              "attached process");
    asmtest_trace_free(tr);
    asmtest_descent_free(d);
    munmap(p, sizeof BLOB1);
    munmap((void *)go, sizeof(int));
#else
    printf("# SKIP descent attach: x86-64 Linux only\n");
#endif
}

/* §Z0/§Z1 — the region-free (empty-ctor) whole-window scope, WEAK single-step tier.
 * begin_window arms with NO registered region and NO [base,len); the handler records
 * the ABSOLUTE address of every instruction the thread runs in the window, so the
 * routine's executed addresses appear as a SUBSET of the captured stream (which also
 * holds the surrounding harness instructions — whole-window is honest-but-noisy).
 * render_window decodes those absolute addresses from live self memory. Any x86-64
 * Linux; no PMU/perf/privilege. */
static void test_wholewindow_singlestep(void) {
#if defined(__x86_64__)
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP whole-window scope: single-step unavailable\n");
        return;
    }
    /* begin_window rejects a NULL trace deterministically (no backend needed). */
    asmtest_hwtrace_scope_t bad = {0, 0};
    CHECK(asmtest_hwtrace_begin_window(NULL, &bad) == ASMTEST_HW_EINVAL,
          "whole-window: begin_window(NULL trace) is EINVAL");

    void *p = ss_map_exec(ROUTINE, sizeof ROUTINE);
    if (p == NULL) {
        printf("# SKIP whole-window: mmap failed\n");
        return;
    }
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK, "whole-window init");

    asmtest_trace_t *tr = asmtest_trace_new(4096, 0);
    asmtest_hwtrace_scope_t scope = {0xffffffffu, 0};
    add2_fn fn = (add2_fn)p;
    /* Bracket ONLY the call — no CHECK/printf inside the window (single-step steps
     * every instruction, so keep the window tight; check the return codes after). */
    int rc_begin = asmtest_hwtrace_begin_window(tr, &scope);
    if (rc_begin != ASMTEST_HW_OK) {
        /* Whole-window is Linux/x86-64 today; on other single-step platforms (macOS
         * x86-64, where available(SINGLESTEP) is true) begin_window self-skips. */
        printf("# SKIP whole-window capture: begin_window unavailable here "
               "(rc=%d)\n",
               rc_begin);
        asmtest_hwtrace_shutdown();
        asmtest_trace_free(tr);
        munmap(p, sizeof ROUTINE);
        return;
    }
    long r = fn(20, 22); /* 42 <= 100: jle taken, dec (0xe) skipped */
    int rc_end = asmtest_hwtrace_end_window(scope, tr);

    /* The empty-ctor form: no register_region, no [base,len) — just arm the thread. */
    CHECK(rc_begin == ASMTEST_HW_OK,
          "whole-window: begin_window arms with no registered region");
    CHECK(rc_end == ASMTEST_HW_OK,
          "whole-window: end_window closes the arming-thread frame");
    CHECK(r == 42, "whole-window: the traced call still returns 20+22");

    /* insns[] hold ABSOLUTE addresses. The routine's five executed addresses
     * [p+0,+3,+6,+c,+11] must all appear (as a subset of the noisy whole window). */
    uint64_t b = (uint64_t)(uintptr_t)p;
    static const uint64_t OFF[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int found_all = 1;
    for (size_t k = 0; k < 5; k++) {
        int hit = 0;
        for (size_t i = 0; i < tr->insns_len; i++)
            if (tr->insns[i] == b + OFF[k]) {
                hit = 1;
                break;
            }
        found_all = found_all && hit;
    }
    CHECK(found_all,
          "whole-window: the routine's absolute addresses are all captured");
    CHECK(tr->insns_len >= 5,
          "whole-window: captured at least the routine (plus harness noise)");

    /* render_window decodes the absolute addresses from live self memory. Where
     * Capstone is built the routine's `ret` (at p+0x11) renders as text. */
    int need = asmtest_hwtrace_render_window(scope, NULL, 0);
    if (need > 0) {
        char *buf = (char *)malloc((size_t)need + 1);
        asmtest_hwtrace_render_window(scope, buf, (size_t)need + 1);
        CHECK(strstr(buf, "ret") != NULL,
              "whole-window: render_window disassembles the live bytes (ret)");
        free(buf);
    } else {
        printf("# SKIP whole-window render text: %s\n",
               need == ASMTEST_HW_ENOSYS ? "built without Capstone"
                                         : "render unavailable");
    }

    /* §Z4 thread-scope honesty: a close whose handle does NOT resolve on the calling
     * thread (the cross-thread-hop case, simulated with a never-armed handle) flags
     * the trace truncated rather than presenting a thread window as complete. */
    asmtest_trace_t *tr2 = asmtest_trace_new(16, 0);
    asmtest_hwtrace_scope_t phantom = {5, 999};
    asmtest_hwtrace_end_window(phantom, tr2);
    CHECK(asmtest_emu_trace_truncated(tr2) != 0,
          "whole-window: a cross-thread (unresolvable-handle) close flags "
          "truncated");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    asmtest_trace_free(tr2);
    munmap(p, sizeof ROUTINE);
#else
    printf("# SKIP whole-window scope: x86-64 Linux only\n");
#endif
}

/* §Z1 attribution: MULTIPLE native leaves in one region-free whole-window scope come
 * back as SEPARATE, named buckets. Two distinct exec_alloc'd routines would both
 * resolve to "[anon]" via /proc/self/maps, so asmtest_hwtrace_attribute_window keys
 * on the caller's named ranges first — proving the leaves are told apart. Also
 * exercises the sparse whole-window buffer (no fixed 64k cap). */
static void test_wholewindow_buckets(void) {
#if defined(__x86_64__)
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP whole-window buckets: single-step unavailable\n");
        return;
    }
    void *a = ss_map_exec(ROUTINE, sizeof ROUTINE); /* leaf A */
    void *b =
        ss_map_exec(ROUTINE, sizeof ROUTINE); /* leaf B (distinct mapping) */
    if (a == NULL || b == NULL) {
        printf("# SKIP whole-window buckets: mmap failed\n");
        return;
    }
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK,
          "whole-window buckets init");

    asmtest_trace_t *tr = asmtest_trace_new(4096, 0);
    asmtest_hwtrace_scope_t scope = {0xffffffffu, 0};
    add2_fn fa = (add2_fn)a, fb = (add2_fn)b;
    /* Bracket ONLY the two calls — both leaves run inside one empty scope. */
    int rc_begin = asmtest_hwtrace_begin_window(tr, &scope);
    if (rc_begin != ASMTEST_HW_OK) {
        printf("# SKIP whole-window buckets: begin_window unavailable here "
               "(rc=%d)\n",
               rc_begin);
        asmtest_hwtrace_shutdown();
        asmtest_trace_free(tr);
        munmap(a, sizeof ROUTINE);
        munmap(b, sizeof ROUTINE);
        return;
    }
    long ra = fa(20, 22);
    long rb = fb(30, 12);
    int rc_end = asmtest_hwtrace_end_window(scope, tr);

    CHECK(rc_begin == ASMTEST_HW_OK && rc_end == ASMTEST_HW_OK,
          "whole-window buckets: scope opened and closed");
    CHECK(ra == 42 && rb == 42,
          "whole-window buckets: both leaves returned 42");

    asmtest_hwtrace_named_region_t regions[2];
    memset(regions, 0, sizeof regions);
    snprintf(regions[0].name, sizeof regions[0].name, "leafA");
    regions[0].base = (uint64_t)(uintptr_t)a;
    regions[0].len = sizeof ROUTINE;
    snprintf(regions[1].name, sizeof regions[1].name, "leafB");
    regions[1].base = (uint64_t)(uintptr_t)b;
    regions[1].len = sizeof ROUTINE;

    asmtest_hwtrace_bucket_t buckets[8];
    memset(buckets, 0, sizeof buckets);
    size_t nb = 0;
    int rc =
        asmtest_hwtrace_attribute_window(scope, regions, 2, buckets, 8, &nb);
    CHECK(rc == ASMTEST_HW_OK, "whole-window buckets: attribute_window ok");

    /* add2(_, _) executes 5 in-region instructions (mov,add,cmp,jle,ret), so each
     * leaf's named bucket must hold exactly 5 — told apart despite identical bytes. */
    uint64_t ca = 0, cb = 0;
    int seen_a = 0, seen_b = 0;
    for (size_t i = 0; i < nb; i++) {
        if (strcmp(buckets[i].label, "leafA") == 0) {
            ca = buckets[i].count;
            seen_a = 1;
        }
        if (strcmp(buckets[i].label, "leafB") == 0) {
            cb = buckets[i].count;
            seen_b = 1;
        }
    }
    CHECK(seen_a && seen_b && nb >= 2,
          "whole-window buckets: two leaves are SEPARATE named buckets");
    CHECK(ca == 5 && cb == 5, "whole-window buckets: each leaf attributed "
                              "exactly its 5 instructions");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    munmap(a, sizeof ROUTINE);
    munmap(b, sizeof ROUTINE);
#else
    printf("# SKIP whole-window buckets: x86-64 Linux only\n");
#endif
}

/* §Z0 gate extras — empty-ctor scope hygiene. (a) NESTED scopes: a region-free
 * whole window composes with a region scope registered+begun INSIDE it (a shim
 * nesting `using (new AsmTrace())` around the region form); both close in LIFO
 * order, the inner region still yields exact offsets, and the process-wide SIGTRAP
 * disposition is installed ONCE (the 0->1 arm-refcount transition) and restored
 * ONCE to the caller's pre-init handler after full teardown — a double install
 * would overwrite g_old_sa and restore asm-test's own handler; a premature restore
 * would show the pre-init handler while the outer window is still armed. The
 * before/mid/after sigaction probes catch both. (b) construct/dispose CHURN: 300
 * begin_window/end_window (+ trace_new/trace_free) cycles on one thread must all
 * return OK — the per-thread frame stack unwinds to empty each cycle and the
 * generation counter neither exhausts nor aliases — and a capture AFTER the churn
 * still records exactly. */
static void test_zeroctor_scope_hygiene(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP zeroctor scope hygiene: single-step unavailable\n");
        return;
    }
    void *p = ss_map_exec(ROUTINE, sizeof ROUTINE);
    if (p == NULL) {
        printf("# SKIP zeroctor scope hygiene: mmap failed\n");
        return;
    }
    /* The pre-init SIGTRAP disposition (glibc/musl union sa_handler/sa_sigaction,
     * so the one field read covers both registration forms). */
    struct sigaction sa_before, sa_mid, sa_after;
    memset(&sa_before, 0, sizeof sa_before);
    sigaction(SIGTRAP, NULL, &sa_before);

    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK, "scope hygiene init");

    /* (a) Nested: open a region-free window, then a region scope INSIDE it. */
    asmtest_trace_t *tr_win = asmtest_trace_new(65536, 0);
    asmtest_trace_t *tr_in = asmtest_trace_new(64, 64);
    asmtest_hwtrace_register_region("hyg_inner", p, sizeof ROUTINE, tr_in);
    asmtest_hwtrace_scope_t win = {0xffffffffu, 0};
    int rc_win = asmtest_hwtrace_begin_window(tr_win, &win);
    if (rc_win != ASMTEST_HW_OK) {
        /* Whole-window is Linux/x86-64-only today; self-skip cleanly elsewhere. */
        printf("# SKIP zeroctor scope hygiene: begin_window unavailable here "
               "(rc=%d)\n",
               rc_win);
        asmtest_hwtrace_shutdown();
        asmtest_trace_free(tr_win);
        asmtest_trace_free(tr_in);
        munmap(p, sizeof ROUTINE);
        return;
    }
    int rc_in =
        asmtest_hwtrace_try_begin("hyg_inner"); /* nested region frame */
    add2_fn fn = (add2_fn)p;
    long r = (rc_in == ASMTEST_HW_OK) ? fn(20, 22) : -1;
    if (rc_in == ASMTEST_HW_OK)
        asmtest_hwtrace_end("hyg_inner"); /* LIFO: pop the inner frame first */
    /* Probe while the OUTER window is still armed: the installed disposition must
     * be asm-test's (not the pre-init one), i.e. the nested inner close did NOT
     * prematurely restore it (the restore belongs to the outermost close only). */
    memset(&sa_mid, 0, sizeof sa_mid);
    sigaction(SIGTRAP, NULL, &sa_mid);
    int rc_endwin = asmtest_hwtrace_end_window(win, tr_win);

    CHECK(rc_win == ASMTEST_HW_OK && rc_in == ASMTEST_HW_OK,
          "hygiene: region-free window + nested region scope both open OK");
    CHECK(
        rc_endwin == ASMTEST_HW_OK && r == 42,
        "hygiene: both scopes close in LIFO order and the traced call returns "
        "42");
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int exact = (asmtest_emu_trace_insns_total(tr_in) == 5);
    for (size_t i = 0; exact && i < 5; i++)
        exact = (tr_in->insns[i] == EXPECT[i]);
    CHECK(exact && !asmtest_emu_trace_truncated(tr_in),
          "hygiene: the nested inner region still yields exact offsets "
          "[0,3,6,c,11]");
    uint64_t b = (uint64_t)(uintptr_t)p;
    int found_all = 1;
    for (size_t k = 0; k < 5; k++) {
        int hit = 0;
        for (size_t i = 0; i < tr_win->insns_len && !hit; i++)
            hit = (tr_win->insns[i] == b + EXPECT[k]);
        found_all = found_all && hit;
    }
    CHECK(found_all,
          "hygiene: the outer window captured the routine's absolute addresses "
          "too");
    CHECK(
        sa_mid.sa_sigaction != sa_before.sa_sigaction,
        "hygiene: SIGTRAP disposition installed while armed; the nested close "
        "did not prematurely restore it");

    /* (b) Churn: 300 construct/dispose cycles on this one thread. Every rc must
     * be OK — an EFULL here would mean the frame stack or generation state leaks
     * across begin/end pairs. */
    int bad = 0;
    for (int i = 0; i < 300; i++) {
        asmtest_trace_t *t = asmtest_trace_new(16, 0);
        asmtest_hwtrace_scope_t sc = {0xffffffffu, 0};
        int rb = (t != NULL) ? asmtest_hwtrace_begin_window(t, &sc)
                             : ASMTEST_HW_EINVAL;
        int re = (rb == ASMTEST_HW_OK) ? asmtest_hwtrace_end_window(sc, t)
                                       : ASMTEST_HW_EINVAL;
        if (rb != ASMTEST_HW_OK || re != ASMTEST_HW_OK)
            bad++;
        asmtest_trace_free(t);
    }
    CHECK(bad == 0,
          "hygiene churn: 300 begin/end_window + trace_new/free cycles all "
          "return OK (no frame/generation exhaustion)");

    /* Capture #301: the surface still works after the churn. */
    asmtest_trace_t *tr_f = asmtest_trace_new(65536, 0);
    asmtest_hwtrace_scope_t fsc = {0xffffffffu, 0};
    int rb_f = asmtest_hwtrace_begin_window(tr_f, &fsc);
    long r_f = (rb_f == ASMTEST_HW_OK) ? fn(20, 22) : -1;
    int re_f = (rb_f == ASMTEST_HW_OK) ? asmtest_hwtrace_end_window(fsc, tr_f)
                                       : ASMTEST_HW_EINVAL;
    int final_ok =
        (rb_f == ASMTEST_HW_OK && re_f == ASMTEST_HW_OK && r_f == 42);
    for (size_t k = 0; final_ok && k < 5; k++) {
        int hit = 0;
        for (size_t i = 0; i < tr_f->insns_len && !hit; i++)
            hit = (tr_f->insns[i] == b + EXPECT[k]);
        final_ok = hit;
    }
    CHECK(final_ok,
          "hygiene churn: capture #301 still records the routine's absolute "
          "addresses");

    asmtest_hwtrace_shutdown();
    /* Full teardown done (the last end_window dropped the arm-refcount to 0):
     * the pre-init SIGTRAP handler must be back — installed once, restored once. */
    memset(&sa_after, 0, sizeof sa_after);
    sigaction(SIGTRAP, NULL, &sa_after);
    CHECK(sa_after.sa_sigaction == sa_before.sa_sigaction,
          "hygiene: SIGTRAP handler restored to the pre-init value after full "
          "teardown (installed once, restored once)");

    asmtest_trace_free(tr_win);
    asmtest_trace_free(tr_in);
    asmtest_trace_free(tr_f);
    munmap(p, sizeof ROUTINE);
#else
    printf("# SKIP zeroctor scope hygiene: x86-64 Linux only\n");
#endif
}

/* §Z5.3 — the never-emit-partial presentation contract: a genuinely OVERFLOWED
 * whole-window capture renders as a labelled, BANNERED prefix, never
 * cap-as-complete. The trace is allocated with a tiny 8-insn cap and the window
 * brackets a 40-trip loop, so the recorded stream far exceeds it: trace_append_insn
 * keeps the first 8 entries, bumps insns_total for the rest, and flags `truncated`;
 * render_window must then END its output with the "; trace truncated — N of M
 * instructions shown" banner over well-formed "<hex>:\t<text>" prefix lines. */
static void test_wholewindow_banner(void) {
#if defined(__x86_64__)
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP whole-window banner: single-step unavailable\n");
        return;
    }
    void *p = ss_map_exec(AMD_LOOP, sizeof AMD_LOOP);
    if (p == NULL) {
        printf("# SKIP whole-window banner: mmap failed\n");
        return;
    }
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK,
          "whole-window banner init");

    asmtest_trace_t *tr =
        asmtest_trace_new(8, 0); /* tiny cap: force overflow */
    asmtest_hwtrace_scope_t scope = {0xffffffffu, 0};
    long (*fn)(long, long) = (long (*)(long, long))p;
    int rc_begin = asmtest_hwtrace_begin_window(tr, &scope);
    if (rc_begin != ASMTEST_HW_OK) {
        printf("# SKIP whole-window banner: begin_window unavailable here "
               "(rc=%d)\n",
               rc_begin);
        asmtest_hwtrace_shutdown();
        asmtest_trace_free(tr);
        munmap(p, sizeof AMD_LOOP);
        return;
    }
    long r =
        fn(1, 40); /* 40 trips: 122 in-region insns alone, far past cap 8 */
    int rc_end = asmtest_hwtrace_end_window(scope, tr);
    CHECK(rc_begin == ASMTEST_HW_OK && rc_end == ASMTEST_HW_OK && r == 40,
          "banner: the overflowing whole window opens, runs, and closes OK");
    CHECK(
        tr->insns_len == 8 && tr->insns_total > tr->insns_len &&
            asmtest_emu_trace_truncated(tr),
        "banner: the tiny cap yields an honest truncated prefix (8 kept, more "
        "ran)");

    int need = asmtest_hwtrace_render_window(scope, NULL, 0);
    if (need == ASMTEST_HW_ENOSYS) {
        /* No Capstone: the render half self-skips; the truncation-honesty half
         * above already ran. */
        printf("# SKIP whole-window banner render: built without Capstone\n");
        asmtest_hwtrace_shutdown();
        asmtest_trace_free(tr);
        munmap(p, sizeof AMD_LOOP);
        return;
    }
    char *buf = (char *)malloc(need > 0 ? (size_t)need + 1 : 1);
    buf[0] = '\0';
    int wrote =
        (need > 0) ? asmtest_hwtrace_render_window(scope, buf, (size_t)need + 1)
                   : need;
    CHECK(need > 0 && wrote == need,
          "banner: render sizes and fills consistently (snprintf semantics)");

    /* The output must END with the truncation banner naming N of M. */
    char banner[160];
    snprintf(banner, sizeof banner,
             "; trace truncated — %llu of %llu instructions shown\n",
             (unsigned long long)tr->insns_len,
             (unsigned long long)tr->insns_total);
    size_t tlen = strlen(buf), blen = strlen(banner);
    CHECK(strstr(buf, "trace truncated") != NULL && tlen >= blen &&
              memcmp(buf + tlen - blen, banner, blen) == 0,
          "banner: render ENDS with the truncation banner (8 of M instructions "
          "shown)");

    /* Every line before the banner is a well-formed "<hex>:\t<text>" row:
     * space-padded lowercase hex, then ':', then '\t', then nonempty text. */
    int rows = 0, well = (tlen >= blen);
    const char *line = buf;
    const char *stop = buf + (tlen >= blen ? tlen - blen : 0);
    while (well && line < stop) {
        const char *nl = memchr(line, '\n', (size_t)(stop - line));
        const char *tab =
            (nl != NULL) ? memchr(line, '\t', (size_t)(nl - line)) : NULL;
        if (nl == NULL || tab == NULL || tab == line || tab[-1] != ':' ||
            tab + 1 >= nl) {
            well = 0;
            break;
        }
        int digits = 0;
        for (const char *q = line; well && q < tab - 1; q++) {
            if (*q == ' ' && digits == 0)
                continue; /* %12llx left-pads with spaces */
            if ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f'))
                digits++;
            else
                well = 0;
        }
        if (digits == 0)
            well = 0;
        rows++;
        line = nl + 1;
    }
    CHECK(
        well && rows == 8,
        "banner: the 8 shown prefix lines are well-formed \"<hex>:\\t<text>\"");

    free(buf);
    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    munmap(p, sizeof AMD_LOOP);
#else
    printf("# SKIP whole-window banner: x86-64 Linux only\n");
#endif
}

/* §Z3 (host-testable half) — the managed name→(addr,size)→track→versioned-render
 * composition, with NO managed runtime. A synthetic §D0.1 MethodLoad event hands
 * over the (addr,size) of a "JIT'd" method: its v0 bytes are published W^X-style
 * (mmap RW -> copy -> mprotect RX), tracked into a self code-image (when0); then
 * the method is re-JIT'd IN PLACE — same address, new bytes, the tier-up — and
 * refresh() stamps when1. A synthetic whole-window trace holding v0's ABSOLUTE
 * executed addresses must render_versioned at when0 as v0's text (its distinctive
 * `add`) and at when1 as v1's (`sub`) — decode-at-VERSION, not live bytes (live
 * memory holds only v1 at render time). Mirrors how test_pt_image_from_codeimage
 * drives the recorder. */
static void test_zeroctor_managed_compose(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_codeimage_available() || !asmtest_disas_available()) {
        char why[200];
        asmtest_codeimage_skip_reason(why, sizeof why);
        printf("# SKIP zeroctor managed compose: %s\n",
               asmtest_disas_available() ? why : "built without Capstone");
        return;
    }
    /* v0 — the file-scope add2 fixture; v1 — the same method re-JIT'd with its
     * `add rax,rsi` (48 01 f0 at offset 3) flipped to `sub rax,rsi` (48 29 f0):
     * same layout, one distinctive instruction — the minimal tier-up. */
    unsigned char v1[sizeof ROUTINE];
    memcpy(v1, ROUTINE, sizeof ROUTINE);
    v1[4] = 0x29;
    long ps = sysconf(_SC_PAGESIZE);
    unsigned char *p =
        (unsigned char *)mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP zeroctor managed compose: mmap failed\n");
        return;
    }
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, (size_t)ps,
             PROT_READ | PROT_EXEC); /* the JIT publishes X bytes */

    /* The synthetic MethodLoad event — what the §D0.1 listener would deliver for
     * a method JIT'd inside the window; its (addr,size) feeds track(). */
    struct {
        const char *name;
        const void *addr;
        size_t size;
    } method_load = {"HotPath", p, sizeof ROUTINE};

    asmtest_codeimage_t *img = asmtest_codeimage_new(0);
    if (img == NULL) {
        printf("# SKIP zeroctor managed compose: codeimage alloc failed\n");
        munmap(p, (size_t)ps);
        return;
    }
    int rc_track =
        asmtest_codeimage_track(img, method_load.addr, method_load.size);
    uint64_t when0 = asmtest_codeimage_now(img);

    /* The tier-up: same address, new bytes (RX -> RW to overwrite -> RX). */
    mprotect(p, (size_t)ps, PROT_READ | PROT_WRITE);
    memcpy(p, v1, sizeof v1);
    mprotect(p, (size_t)ps, PROT_READ | PROT_EXEC);
    int nver = asmtest_codeimage_refresh(img);
    uint64_t when1 = asmtest_codeimage_now(img);
    CHECK(rc_track == ASMTEST_CI_OK && nver > 0 && when1 > when0,
          "managed compose: track()+refresh() record the tier-up as a second "
          "version (when0 < when1)");

    /* The synthetic whole-window trace: ABSOLUTE addresses of v0's executed path
     * {0,3,6,c,11} — what the region-free capture recorded while v0 was live. */
    asmtest_trace_t *tr = asmtest_trace_new(8, 0);
    static const uint64_t OFF[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    for (size_t i = 0; i < 5; i++)
        trace_append_insn(tr, (uint64_t)(uintptr_t)p + OFF[i]);

    char b0[512], b1[512], g_add[64], g_sub[64];
    int n0 = asmtest_hwtrace_render_versioned(img, when0, tr, b0, sizeof b0);
    int n1 = asmtest_hwtrace_render_versioned(img, when1, tr, b1, sizeof b1);
    /* Ground truth for the distinctive instruction at p+3, per version. */
    asmtest_disas(ASMTEST_ARCH_X86_64, ROUTINE, sizeof ROUTINE,
                  (uint64_t)(uintptr_t)p, 3, g_add, sizeof g_add);
    asmtest_disas(ASMTEST_ARCH_X86_64, v1, sizeof v1, (uint64_t)(uintptr_t)p, 3,
                  g_sub, sizeof g_sub);
    CHECK(n0 > 0 && g_add[0] != '\0' && strstr(b0, g_add) != NULL,
          "managed compose: when0 renders the FIRST routine's distinctive add");
    CHECK(n1 > 0 && g_sub[0] != '\0' && strstr(b1, g_sub) != NULL,
          "managed compose: when1 renders the re-JIT'd sub at the same "
          "addresses");
    /* Live memory holds ONLY v1 at render time, so when0's text showing no `sub`
     * (and differing from when1's) proves decode-at-version, not live bytes. */
    CHECK(strstr(b0, "sub") == NULL && strcmp(b0, b1) != 0,
          "managed compose: decode is at-version, not live bytes (when0 has no "
          "sub; when0 != when1)");

    asmtest_trace_free(tr);
    asmtest_codeimage_free(img);
    munmap(p, (size_t)ps);
#else
    printf(
        "# SKIP zeroctor managed compose: x86-64 Linux only (x86 fixture)\n");
#endif
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Backend-independent: validate the AMD reconstruction decoder. */
    test_amd_freeze_probe();
    test_amd_reconstruction();
    test_amd_spec_filter();

    /* Backend-independent: the §D4 async-hop stitching merge core. */
    test_stitch_slices();

    /* §2: the recorder-backed PT image adapter (host-testable, no libipt/PT hw). */
    test_pt_image_from_codeimage();

    /* §3.1(c): whole-window noise attribution — reverse resolver + IP bucketer. */
    test_symbolize_bucket();

    /* Phase 1 call-descent prerequisites: the is_ret / call_target disasm queries. */
    test_disas_queries();

    /* Phase 2 call-descent: the descent handle lifecycle + OFF-level no-op. */
    test_descent_handle();

    /* Phases 3-5 call-descent: the fork-path descending loop (L1 edges, L2 descend-known,
     * L3 descend-all + guards, same-region recursion, max_depth). */
    test_descent_fork();

    /* Phase 3-4 call-descent: the attached-path descending loop (foreign process). */
    test_descent_attach();

    /* Phase 8 call-descent: cascade invariants + the honest step-over-intermediary limit. */
    test_descent_cascade();

    /* Regression: a stale L3 watchdog flag must not abort a later L1/L2 descent on EINTR. */
    test_descent_stale_alarm_flag();

    /* Backend-independent: validate the CoreSight reconstruction core (synthetic
     * ranges; the live OpenCSD decode tree awaits a board). */
    test_cs_reconstruction();

    /* AMD LBR LIVE capture — runs on a Zen 3+/4/5 host with perf branch-stack
     * permitted (e.g. this Zen 5 dev box); self-skips elsewhere. */
    test_amd_live();

    /* §1 (AMD): concurrent per-thread AMD-LBR capture (capped lane on AMD). */
    test_concurrent_amd();

    /* AMD Tier-B stitching past the 16-deep window (host-validated, synthetic). */
    test_amd_stitch();
    test_amd_stitch_decodable();

    /* §3.2 AMD data_tail drain reconstruction (host-testable half; live needs Zen 3+). */
    test_amd_drain_reconstruction();

    /* Live, on this very host: the single-step backend (no PMU/perf/privilege). */
    test_singlestep_live();
    test_singlestep_loop();

    /* B (lazy-arm): the managed-safe arm→call→disarm scope, over a native leaf. */
    test_call_scoped();
    test_call_scoped_fp();
    test_call_scoped_ex();
    /* §D0.4 live-producer bridge: stitch real captured trace handles by seq. */
    test_stitch_handles();

    /* Scoped-tracing shared-core §0: try_begin signal, arm-thread assert,
     * render-on-close, and idempotent-by-name register (all single-step). */
    test_try_begin_busy();
    test_arm_tid_mismatch();
    test_render_singlestep();
    test_register_idempotent();

    /* §1 per-thread state: nesting, concurrency, handle-keyed per-scope slices,
     * and version-aware render (all single-step / codeimage — any x86-64 Linux). */
    test_nested_singlestep();
    test_concurrent_singlestep();
    test_concurrent_samename();
    test_render_versioned();

    /* §Z0/§Z1: the region-free (empty-ctor) whole-window scope — WEAK single-step
     * tier + the §Z4 cross-thread honesty flag (any x86-64 Linux, no hardware). */
    test_wholewindow_singlestep();
    test_wholewindow_buckets();

    /* §Z0 gate extras: nested region-free + region scopes (SIGTRAP installed
     * once, restored once to the pre-init handler) and 300-cycle
     * construct/dispose churn. */
    test_zeroctor_scope_hygiene();

    /* §Z5.3: an overflowed whole window renders a labelled, BANNERED prefix —
     * never cap-as-complete. */
    test_wholewindow_banner();

    /* §Z3 (host-testable half): the synthetic managed name→(addr,size)→track→
     * versioned-render chain — decode-at-version against the window-live image. */
    test_zeroctor_managed_compose();

    /* Live: the out-of-process ptrace single-step backend (W2). */
    test_ptrace_oop();
    test_ptrace_blockstep();
    test_ptrace_windowed();
    test_ptrace_window_call();
    test_stealth_windowed();

    /* Live: tracing a routine that faults (SIGILL) must reap its tracee, not leak it. */
    test_ptrace_faulting_no_leak();

    /* Live: tracing a region in a SEPARATE, externally-attached process (W2 attach). */
    test_ptrace_attach();
    test_ptrace_attach_blockstep();

    /* Live: discover a foreign region from /proc/<pid>/maps then attach+trace it, and
     * parse a JIT perf-map (the "point W2 at a running process" layer). */
    test_proc_resolve_and_trace();

    /* Live: the full uncontrolled-timing managed-runtime flow — resolve a JIT method by
     * name, run the target to it (software breakpoint), then trace that invocation. */
    test_run_to_and_trace();

    /* Live: the time-aware code-image recorder feeding the W2 stepper — decode a foreign
     * method against the bytes that were live when it ran, not a single late snapshot. */
    test_ptrace_versioned();

    /* Live: call-depth awareness — trace a region that calls OUT to a helper, stepping
     * over the callee at native speed instead of mistaking the call for the return. Run
     * the step-over with a software int3 (default) and with a forced HARDWARE breakpoint
     * (the path that traces W^X JIT code as-shipped), asserting identical results. */
    test_ptrace_callout("software int3", 0);
    test_ptrace_callout("hardware bp", 1);

    /* §D3: the concealed reverse-attach ptrace-stealth stepper scope (CI-runnable). */
    test_ptrace_scoped_stealth();

    test_perfmap_resolve();
    test_jitdump_reader();

    /* The auto-select orchestrator: pick + use the best available backend. */
    test_auto_resolve();

    /* The cross-tier orchestrator: resolve over hwtrace + DynamoRIO + emulator. */
    test_cross_tier_resolve();

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
