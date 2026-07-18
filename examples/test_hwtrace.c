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
/* REG_RIP/REG_RAX (ucontext gregs) are __USE_GNU-gated, and the windowed block-step
 * signal leg's handler fixes up the tracee's registers through them. */
#define _GNU_SOURCE

#include "asmtest_codeimage.h"
#include "asmtest_hwtrace.h"
#include "asmtest_ibs.h" /* Zen-2 F6: gate the IBS-Op survey-fallback test */
#include "asmtest_ptrace.h"
#include "asmtest_trace.h"
#include "asmtest_trace_auto.h"

#include <limits.h> /* INT_MIN — the perf_event_paranoid absent-file sentinel (F29) */
#include <stddef.h> /* offsetof — the F27 options ABI-guard test */
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
#include <ucontext.h> /* the windowed block-step signal leg fixes up the tracee's
                         RIP/RAX from its SIGSEGV handler */
#include <unistd.h>
#endif

/* AMD branch-stack decoder declarations + perf_event are x86-64-only. */
#if defined(__linux__) && defined(__x86_64__)
#include <linux/perf_event.h>
int asmtest_amd_decode(const struct perf_branch_entry *br, size_t nbr,
                       const void *base, size_t len, asmtest_trace_t *trace);
int asmtest_amd_decode_reach(const struct perf_branch_entry *br, size_t nbr,
                             const void *base, size_t len,
                             asmtest_trace_t *trace, int *reached_exit);
int asmtest_amd_decode_reach_hw(const struct perf_branch_entry *br, size_t nbr,
                                size_t hw_nbr, const void *base, size_t len,
                                asmtest_trace_t *trace, int *reached_exit);
int asmtest_amd_decoder_present(void);
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
int asmtest_amd_msr_decode_entry(uint64_t from, uint64_t to,
                                 struct perf_branch_entry *out);
size_t asmtest_amd_last_exit_off(const void *base, size_t len, int *nexit);
size_t asmtest_amd_all_exits(const void *base, size_t len, size_t *out, int cap,
                             int *nexit);
/* F43 ring-parse seam + P9 cpuinfo-flag matchers (internal, defined in
 * hwtrace.c / amd_backend.c; hand-declared here like the AMD decode entries above). */
void asmtest_amd_ring_parse_decode(uint8_t *buf, size_t span, size_t dsz,
                                   const void *base, size_t len,
                                   asmtest_trace_t *trace);
int asmtest_amd_has_cpu_flag(const char *flag);
int asmtest_amd_flags_have(const char *line, const char *flag);
/* §E5 AutoFDO block-frequency reweighting of the survey endpoints + the survey entry
 * that drives it (internal, defined in hwtrace.c; not in the public header). */
size_t asmtest_amd_block_weight_sample(const struct perf_branch_entry *e,
                                       uint64_t nr, uint64_t *ips, size_t at,
                                       size_t cap);
int asmtest_hwtrace_sample_window_amd_weighted(void (*run_fn)(void *),
                                               void *arg, int period,
                                               uint64_t *ips, size_t cap,
                                               size_t *nips, int *truncated);
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

/* §Z2 Intel PT decode path (pt_backend.c). Real bodies compile only under
 * -DASMTEST_HAVE_LIBIPT; otherwise ENOSYS stubs. asmtest_pt_encode_fixture builds a
 * valid synthetic PT AUX blob with libipt's own packet encoder (no PT hardware), and
 * asmtest_pt_decode[_window] decode it — driven end-to-end by test_wholewindow_decode. */
int asmtest_pt_decoder_present(void);
int asmtest_pt_decode(const uint8_t *aux, size_t aux_len, const void *base,
                      size_t len, asmtest_trace_t *trace);
int asmtest_pt_decode_window(const uint8_t *aux, size_t aux_len,
                             const asmtest_codeimage_t *img, uint64_t when,
                             asmtest_trace_t *trace);
int asmtest_pt_encode_fixture(uint8_t *buf, size_t cap, uint64_t base_ip,
                              int taken, size_t *out_len);

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

/* F27 flag-day idiom (amd-tracing-followup-plan Phase 1): zero the options,
 * self-describe the caller's compiled-in struct size, pick the backend. Every
 * init site below goes through this so no site can miss the size negotiation. */
#define INIT_OPTS(o, b)                                                        \
    do {                                                                       \
        memset(&(o), 0, sizeof(o));                                            \
        (o).struct_size = sizeof(o);                                           \
        (o).backend = (b);                                                     \
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
/* Phase 4 — env-gated debug logging (src/debug.{c,h}). Forked children so the
 * getenv-once cache starts pristine (this runs FIRST in main, before any tier call caches
 * it in the parent): a child with ASMTEST_HWTRACE_DEBUG=1 must emit a "[asmtest hwtrace] "
 * line to stderr on the first tier call (asmtest_hwtrace_init); a child with it unset must
 * keep stderr empty (zero overhead when off). Host-independent (the init log fires on any
 * arch, before the availability gate). */
static void test_debug_logging(void) {
#if defined(__linux__)
    for (int on = 0; on <= 1; on++) {
        int fds[2];
        if (pipe(fds) != 0) {
            CHECK(0, "debug logging: pipe() failed");
            return;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(fds[0]);
            dup2(fds[1], 2); /* stderr -> pipe */
            close(fds[1]);
            if (on)
                setenv("ASMTEST_HWTRACE_DEBUG", "1", 1);
            else {
                unsetenv("ASMTEST_HWTRACE_DEBUG");
                unsetenv("ASMTEST_AMD_DEBUG");
            }
            asmtest_hwtrace_options_t o;
            INIT_OPTS(o, ASMTEST_HWTRACE_SINGLESTEP);
            asmtest_hwtrace_init(
                &o); /* logs "init: backend=..." when enabled */
            asmtest_hwtrace_shutdown();
            fflush(stderr);
            _exit(0);
        }
        close(fds[1]);
        char buf[512];
        ssize_t tot = 0, k;
        while (tot < (ssize_t)sizeof buf - 1 &&
               (k = read(fds[0], buf + tot, sizeof buf - 1 - (size_t)tot)) > 0)
            tot += k;
        buf[tot > 0 ? tot : 0] = '\0';
        close(fds[0]);
        int st = 0;
        waitpid(pid, &st, 0);
        int has = strstr(buf, "[asmtest hwtrace] ") != NULL;
        if (on)
            CHECK(has,
                  "debug logging: ASMTEST_HWTRACE_DEBUG=1 emits a tier log "
                  "line to stderr");
        else
            CHECK(!has, "debug logging: unset env keeps stderr empty (zero "
                        "overhead when off)");
    }
#else
    printf("# SKIP debug logging: Linux only\n");
#endif
}

static void test_amd_snapshot_substrate_probe(void) {
#if defined(__linux__) && defined(__x86_64__)
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
    printf("# SKIP AMD snapshot-substrate probe: x86-64 Linux only\n");
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

    /* Trailing-block fill (lane4 — the sampled small-routine fidelity gap). A window
     * sampled BEFORE the routine returned holds the taken jle (0xc->0x11) but NOT the ret,
     * so the branch loop alone stops at the jle target and undercounts the retired set by
     * its trailing ret. asmtest_amd_decode now fills the straight-line run from the last
     * in-region target to the region exit, so a ret-less window still reconstructs the FULL
     * {0,3,6,0xc,0x11} single-step set (no undercount) and asmtest_amd_decode_reach reports
     * reached_exit — a COMPLETE window, not a truncated fragment. Host-independent (synthetic
     * stack), so it also guards the fix off AMD hardware where the live lane self-skips. */
    struct perf_branch_entry tail[1];
    memset(tail, 0, sizeof tail);
    tail[0].from = b + 0xc;
    tail[0].to =
        b + 0x11; /* jle -> the ret block; the ret itself is NOT recorded */
    asmtest_trace_t *tt = asmtest_trace_new(64, 64);
    int reached = -1;
    int trc = asmtest_amd_decode_reach(tail, 1, ROUTINE, sizeof ROUTINE, tt,
                                       &reached);
    CHECK(trc == 0,
          "AMD decode (reach) succeeds on a ret-less trailing window");
    int tseq = (asmtest_emu_trace_insns_total(tt) == 5);
    for (size_t i = 0; tseq && i < 5; i++)
        tseq = (tt->insns[i] == EXPECT[i]);
    CHECK(tseq,
          "AMD trailing-fill reconstructs the FULL retired set "
          "{0,3,6,0xc,0x11} from a window missing the ret (no undercount)");
    CHECK(reached == 1,
          "AMD trailing-fill reports reached_exit (the last block "
          "ran straight to the ret)");
    CHECK(!asmtest_emu_trace_truncated(tt),
          "AMD trailing-fill window is a complete reconstruction (tail reached "
          "the region exit)");
    CHECK(asmtest_trace_covered(tt, 0) && asmtest_trace_covered(tt, 0x11) &&
              asmtest_emu_trace_blocks_len(tt) == 2,
          "AMD trailing-fill keeps the {0, 0x11} block partition");
    asmtest_trace_free(tt);

    /* Entry-block fill (lane4 — the ROOT cause the live privileged run surfaced). A too-fast
     * tiny routine's frozen stack can carry a spurious mid-routine LANDING (from OUTSIDE the
     * region, to an in-region offset > 0) as its oldest in-region edge: here `X->0x3` ahead
     * of the real jle/ret. The old replay anchored decoding at 0x3 and DROPPED the entry
     * instruction at 0x0, so a window reported COMPLETE (its recorded ret anchors the exit)
     * undercounted the retired set to {3,6,0xc,0x11} = 4 (observed live: insns=4 truncated=0).
     * The prologue [base, first-landing) is now decoded + prepended, so the reconstruction is
     * the FULL {0,3,6,0xc,0x11} = 5 with the true {0, 0x11} partition (the clean straight-line
     * prologue means 0x3 is NOT a real block boundary). This is the deterministic guard for
     * the fix, off AMD too. */
    struct perf_branch_entry entry[3];
    memset(entry, 0, sizeof entry);
    entry[0].from = b + 0x11;
    entry[0].to = b + sizeof ROUTINE; /* ret -> outside (newest) */
    entry[1].from = b + 0xc;
    entry[1].to = b + 0x11; /* jle -> ret block */
    entry[2].from = b + 0x1000;
    entry[2].to = b + 0x3; /* spurious landing at 0x3 from outside (oldest) */
    asmtest_trace_t *et = asmtest_trace_new(64, 64);
    int erc = asmtest_amd_decode(entry, 3, ROUTINE, sizeof ROUTINE, et);
    CHECK(erc == 0, "AMD decode succeeds on a spurious-entry-landing window");
    int eseq = (asmtest_emu_trace_insns_total(et) == 5);
    for (size_t i = 0; eseq && i < 5; i++)
        eseq = (et->insns[i] == EXPECT[i]);
    CHECK(eseq,
          "AMD entry-fill prepends the dropped prologue -> the FULL retired "
          "set {0,3,6,0xc,0x11} (no leading-block undercount)");
    CHECK(
        asmtest_trace_covered(et, 0) && asmtest_trace_covered(et, 0x11) &&
            asmtest_emu_trace_blocks_len(et) == 2,
        "AMD entry-fill keeps the true {0, 0x11} partition (the clean prologue "
        "makes 0x3 a non-boundary, not a spurious block)");
    CHECK(!asmtest_emu_trace_truncated(et),
          "AMD entry-fill: a within-window reconstruction stays complete");
    asmtest_trace_free(et);

    /* Anti-over-count / anti-fabrication: a window whose last in-region target lands on a
     * block that continues to a NON-exit conditional branch (a loop back-edge) must NOT be
     * tail-completed — the unrecorded jnz could have been TAKEN, so completing straight-line
     * past it would fabricate a fall-through the CPU may never have run. Feed a loop's
     * back-edge (jnz 0xd->0x7) alone: the tail from 0x7 reaches the jnz at 0xd and STOPS,
     * reaching no exit — reached_exit stays 0 and the ret at 0xf is NOT appended. (mov rax,0;
     * L: add rax,rdi; dec rsi; jnz L; ret — the same shape as AMD_LOOP.) */
    static const unsigned char LOOPR[] = {0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00,
                                          0x00, 0x48, 0x01, 0xf8, 0x48, 0xff,
                                          0xce, 0x75, 0xf8, 0xc3};
    const uint64_t bl = (uint64_t)(uintptr_t)LOOPR;
    struct perf_branch_entry bedge[1];
    memset(bedge, 0, sizeof bedge);
    bedge[0].from = bl + 0xd;
    bedge[0].to = bl + 0x7; /* the taken back-edge; no ret recorded */
    asmtest_trace_t *tb = asmtest_trace_new(64, 64);
    int reached_b = -1;
    asmtest_amd_decode_reach(bedge, 1, LOOPR, sizeof LOOPR, tb, &reached_b);
    CHECK(reached_b == 0,
          "AMD trailing-fill does NOT complete a back-edge window "
          "(the unrecorded jnz is ambiguous — no fabricated exit)");
    CHECK(
        !asmtest_trace_covered(tb, 0xf),
        "AMD trailing-fill does NOT fabricate the ret (0xf) past an unrecorded "
        "conditional back-edge");
    asmtest_trace_free(tb);

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

/* T1 (amd-branchsnap-lbr-docs) — the depth ceiling counts HARDWARE branch-stack
 * slots (hw_nbr), not the total array length (nbr). branchsnap prepends a synthetic
 * boundary edge, so a provably complete 15-slot window (15 hw slots + 1 synthetic =
 * nbr 16) must NOT be flagged truncated; only a genuinely saturated 16-hw-slot window
 * does. Host-independent (synthetic perf_branch_entry array, no Zen hardware). */
static void test_amd_decode_hw_ceiling(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_amd_decoder_present()) {
        printf("# SKIP AMD hw-ceiling: built without Capstone\n");
        return;
    }
    /* 14 * `jmp +0` (EB 00) at 0x00..0x1A, then `ret` (C3) at 0x1C. */
    static const uint8_t REGION[] = {
        0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00,
        0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00,
        0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xEB, 0x00, 0xC3};
    uint64_t b = (uint64_t)(uintptr_t)REGION;

    /* newest-first: [synthetic boundary edge, 14 jmp edges (0x1A newest), entry call]. */
    struct perf_branch_entry arr[16];
    memset(arr, 0, sizeof arr);
    arr[0].from =
        b + 0x1C;  /* the boundary edge (ret) — a completion, NOT a hw slot */
    arr[0].to = 0; /* exits leave the region                               */
    for (int i = 0; i < 14; i++) {
        uint64_t off = (uint64_t)(13 - i) * 2; /* 0x1A at arr[1] down to 0x00 */
        arr[1 + i].from = b + off;
        arr[1 + i].to = b + off + 2;
    }
    arr[15].from = b - 5; /* entry call edge (a hardware slot) */
    arr[15].to = b;

    /* hw_nbr = 15: 15 real slots fit a 16-deep window -> complete, NOT truncated. */
    asmtest_trace_t *okt = asmtest_trace_new(64, 64);
    int rc = asmtest_amd_decode_reach_hw(arr, 16, 15, REGION, sizeof REGION,
                                         okt, NULL);
    CHECK(rc == 0, "AMD hw-ceiling: decode_reach_hw succeeds (hw_nbr=15)");
    CHECK(!asmtest_emu_trace_truncated(okt),
          "AMD hw-ceiling: 15 hw slots + 1 synthetic boundary edge is NOT "
          "truncated");
    CHECK(asmtest_emu_trace_insns_total(okt) == 15,
          "AMD hw-ceiling: reconstructs 15 insns (14 jmp + ret)");
    asmtest_trace_free(okt);

    /* hw_nbr = 16: a genuinely saturated window still truncates (boundary pinned). */
    asmtest_trace_t *fullt = asmtest_trace_new(64, 64);
    asmtest_amd_decode_reach_hw(arr, 16, 16, REGION, sizeof REGION, fullt,
                                NULL);
    CHECK(asmtest_emu_trace_truncated(fullt),
          "AMD hw-ceiling: 16 hw slots sets truncated (window overflow)");
    asmtest_trace_free(fullt);

    /* the asmtest_amd_decode wrapper is unchanged: hw_nbr == nbr == 16 -> truncated. */
    asmtest_trace_t *wrapt = asmtest_trace_new(64, 64);
    asmtest_amd_decode(arr, 16, REGION, sizeof REGION, wrapt);
    CHECK(asmtest_emu_trace_truncated(wrapt),
          "AMD hw-ceiling: asmtest_amd_decode wrapper semantics unchanged "
          "(truncated)");
    asmtest_trace_free(wrapt);
#else
    printf("# SKIP AMD hw-ceiling: not Linux x86-64\n");
#endif
}

/* AMD #2B — reduced-filter follow-static reconstruction. The opt-in reduced LBR filter
 * (asmtest_hwtrace_options_t.branch_filter) drops direct UNCONDITIONAL jmp edges from the
 * recorded stack — their targets are statically decodable, so they need not consume a
 * 16-deep slot — and amd_replay FOLLOWS them from the region bytes. Host-independent
 * (synthetic perf_branch_entry arrays, no Zen hardware), exactly like
 * test_amd_reconstruction. Covers the residual-risk fixtures the design flagged: a
 * dropped in-region jmp reconstructs byte-identically to the full-filter stack that keeps
 * it (F1/F4, the load-bearing equivalence — the only way this could silently corrupt is a
 * misclassification that appends the dead post-jmp bytes or drops/doubles the target
 * block); a dropped back-edge cycle terminates via the step bound instead of hanging
 * (F2); a dropped jmp leaving the region honestly truncates (F3); and a chain of dropped
 * jmps follows through (F5). */
static void test_amd_reduced_filter(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_amd_decoder_present()) {
        printf("# SKIP AMD reduced filter: built without Capstone\n");
        return;
    }

    /* F1/F4 — xor eax,eax ; jmp 0x06 ; int3 ; int3 ; ret. The direct uncond jmp at
     * 0x02 (target 0x06) skips the two dead int3 bytes. Executed stream {0x00, 0x02,
     * 0x06}, blocks {0x00, 0x06}. */
    static const unsigned char JMP7[] = {0x31, 0xc0, 0xeb, 0x02,
                                         0xcc, 0xcc, 0xc3};
    const uint64_t bj = (uint64_t)(uintptr_t)JMP7;

    /* FULL BRANCH_ANY stack, newest-first: [ret, jmp]. */
    struct perf_branch_entry full[2];
    memset(full, 0, sizeof full);
    full[0].from = bj + 0x06;
    full[0].to = bj + sizeof JMP7; /* ret -> outside */
    full[1].from = bj + 0x02;
    full[1].to = bj + 0x06; /* jmp -> 0x06 */

    /* REDUCED stack: the direct uncond jmp edge is DROPPED. newest-first: [ret]. */
    struct perf_branch_entry reduced[1];
    memset(reduced, 0, sizeof reduced);
    reduced[0].from = bj + 0x06;
    reduced[0].to = bj + sizeof JMP7;

    asmtest_trace_t *tf = asmtest_trace_new(64, 64);
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    /* Assign the decode rc to a local BEFORE CHECK — the CHECK macro evaluates its
     * condition twice, so calling asmtest_amd_decode inside it would decode twice. */
    int rcf = asmtest_amd_decode(full, 2, JMP7, sizeof JMP7, tf);
    int rcr = asmtest_amd_decode(reduced, 1, JMP7, sizeof JMP7, tr);
    CHECK(rcf == 0, "AMD reduced filter: full BRANCH_ANY stack decodes");
    CHECK(rcr == 0, "AMD reduced filter: reduced (jmp-dropped) stack decodes");

    static const uint64_t EXPECT_J[] = {0x00, 0x02, 0x06};
    int fok = (asmtest_emu_trace_insns_total(tf) == 3);
    for (size_t i = 0; fok && i < 3; i++)
        fok = (tf->insns[i] == EXPECT_J[i]);
    CHECK(fok, "AMD reduced filter: full array yields [0,2,6] (jmp taken)");

    /* F1 — the reduced reconstruction must NOT append the dead 0x04/0x05 bytes. */
    int rok = (asmtest_emu_trace_insns_total(tr) == 3);
    for (size_t i = 0; rok && i < 3; i++)
        rok = (tr->insns[i] == EXPECT_J[i]);
    CHECK(
        rok,
        "AMD reduced filter: reduced stack follows the dropped jmp -> [0,2,6] "
        "(dead bytes not decoded)");

    /* F4 — mask-agnostic equivalence: reduced == full, byte for byte. */
    int par =
        (asmtest_emu_trace_insns_total(tr) ==
         asmtest_emu_trace_insns_total(tf)) &&
        (asmtest_emu_trace_blocks_len(tr) ==
         asmtest_emu_trace_blocks_len(tf)) &&
        (asmtest_emu_trace_truncated(tr) == asmtest_emu_trace_truncated(tf));
    for (size_t i = 0; par && i < asmtest_emu_trace_insns_total(tf); i++)
        par = (tr->insns[i] == tf->insns[i]);
    CHECK(par, "AMD reduced filter: reduced stack matches the full stack "
               "byte-for-byte (correct under either mask)");
    CHECK(asmtest_trace_covered(tr, 0x00) && asmtest_trace_covered(tr, 0x06) &&
              asmtest_emu_trace_blocks_len(tr) == 2,
          "AMD reduced filter: reduced stack keeps the {0, 0x06} partition");
    CHECK(!asmtest_trace_covered(tr, 0x04),
          "AMD reduced filter: the skipped byte 0x04 is NOT a block/insn");
    CHECK(!asmtest_emu_trace_truncated(tr),
          "AMD reduced filter: reduced reconstruction is complete");
    asmtest_trace_free(tf);
    asmtest_trace_free(tr);

    /* F2 — dropped back-edge cycle must terminate (step bound), not hang.
     * jmp 0x06 ; int3*4 ; jmp 0x00 ; ret — both jmps dropped, so following 0->6->0
     * never reaches the recorded ret source; the >len step bound bails truncated. */
    static const unsigned char CYCLE9[] = {0xeb, 0x04, 0xcc, 0xcc, 0xcc,
                                           0xcc, 0xeb, 0xf8, 0xc3};
    const uint64_t bc = (uint64_t)(uintptr_t)CYCLE9;
    struct perf_branch_entry cyc[1];
    memset(cyc, 0, sizeof cyc);
    cyc[0].from = bc + 0x08; /* ret, unreachable through the 0<->6 jmp cycle */
    cyc[0].to = bc + sizeof CYCLE9;
    asmtest_trace_t *tc = asmtest_trace_new(64, 64);
    asmtest_amd_decode(cyc, 1, CYCLE9, sizeof CYCLE9,
                       tc); /* must return, no hang */
    CHECK(asmtest_emu_trace_truncated(tc),
          "AMD reduced filter: a dropped back-edge cycle truncates (bounded, "
          "no hang)");
    asmtest_trace_free(tc);

    /* F3 — a dropped jmp leaving the region honestly truncates: the in-region source
     * it was decoding toward becomes unreachable. xor ; jmp 0x24 (out of [b,b+4)). */
    static const unsigned char EXIT4[] = {0x31, 0xc0, 0xeb, 0x20};
    const uint64_t be = (uint64_t)(uintptr_t)EXIT4;
    struct perf_branch_entry ext[1];
    memset(ext, 0, sizeof ext);
    ext[0].from =
        be + 0x03; /* an in-region source the out-of-region jmp strands */
    ext[0].to = be + sizeof EXIT4;
    asmtest_trace_t *te = asmtest_trace_new(64, 64);
    asmtest_amd_decode(ext, 1, EXIT4, sizeof EXIT4, te);
    CHECK(asmtest_emu_trace_truncated(te),
          "AMD reduced filter: a dropped jmp leaving the region truncates");
    asmtest_trace_free(te);

    /* F5 — chained dropped jmps follow through: xor ; jmp A ; A: jmp B ; B: ret,
     * each dead pair skipped. Executed {0x00, 0x02, 0x06, 0x0a}, blocks {0,6,0xa}. */
    static const unsigned char CHAIN11[] = {0x31, 0xc0, 0xeb, 0x02, 0xcc, 0xcc,
                                            0xeb, 0x02, 0xcc, 0xcc, 0xc3};
    const uint64_t bh = (uint64_t)(uintptr_t)CHAIN11;
    struct perf_branch_entry chn[1];
    memset(chn, 0, sizeof chn);
    chn[0].from = bh + 0x0a; /* ret; both jmps dropped */
    chn[0].to = bh + sizeof CHAIN11;
    asmtest_trace_t *th = asmtest_trace_new(64, 64);
    asmtest_amd_decode(chn, 1, CHAIN11, sizeof CHAIN11, th);
    static const uint64_t EXPECT_C[] = {0x00, 0x02, 0x06, 0x0a};
    int hok = (asmtest_emu_trace_insns_total(th) == 4);
    for (size_t i = 0; hok && i < 4; i++)
        hok = (th->insns[i] == EXPECT_C[i]);
    CHECK(hok,
          "AMD reduced filter: chained dropped jmps reconstruct [0,2,6,a]");
    CHECK(asmtest_trace_covered(th, 0x00) && asmtest_trace_covered(th, 0x06) &&
              asmtest_trace_covered(th, 0x0a) &&
              asmtest_emu_trace_blocks_len(th) == 3,
          "AMD reduced filter: chained follow yields blocks {0, 6, 0xa}");
    CHECK(!asmtest_emu_trace_truncated(th),
          "AMD reduced filter: chained follow is complete");
    asmtest_trace_free(th);
#else
    printf("# SKIP AMD reduced filter: not Linux x86-64\n");
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_AMD_LBR);
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

/* Live AMD_LOOP capture at a given lbr_period (#2A): period 0/1 keeps the exact
 * sample_period=1 path; >1 spaces the PMIs so consecutive 16-deep windows overlap by
 * (depth - period) and the Tier-B stitch splices them at ~period-times fewer interrupts.
 * Returns the reconstructed insns_total (the stitched reach). */
static uint64_t amd_capture_loop_period(void *p, long trips, int period,
                                        int *trunc) {
    asmtest_hwtrace_options_t opts;
    INIT_OPTS(opts, ASMTEST_HWTRACE_AMD_LBR);
    opts.lbr_period = period;
    asmtest_hwtrace_init(&opts);
    asmtest_trace_t *tr = asmtest_trace_new(4096, 64);
    asmtest_hwtrace_register_region("amdper", p, sizeof AMD_LOOP, tr);
    long (*fn)(long, long) = (long (*)(long, long))p;
    asmtest_hwtrace_begin("amdper");
    fn(1, trips);
    asmtest_hwtrace_end("amdper");
    uint64_t n = asmtest_emu_trace_insns_total(tr);
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
        INIT_OPTS(opts, ASMTEST_HWTRACE_AMD_LBR);
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
        INIT_OPTS(opts, ASMTEST_HWTRACE_AMD_LBR);
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

/* One raw single-step capture of add2(20,22) — the exact-retired baseline the AMD_LBR
 * capture is measured against (the ceiling-free reference tier, always available on
 * x86-64 Linux). Returns insns_total. */
static uint64_t ss_capture_add2(void *p) {
    asmtest_hwtrace_options_t opts;
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
    asmtest_hwtrace_init(&opts);
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    asmtest_hwtrace_register_region("ssadd2", p, sizeof ROUTINE, tr);
    add2_fn fn = (add2_fn)p;
    asmtest_hwtrace_begin("ssadd2");
    (void)fn(20, 22);
    asmtest_hwtrace_end("ssadd2");
    uint64_t n = asmtest_emu_trace_insns_total(tr);
    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    return n;
}

/* One raw live AMD-LBR capture of add2(20,22) — the sampled path, no auto-escalation
 * (mirrors amd_capture_loop for the small routine). Returns insns_total; reports the
 * truncation bit. */
static uint64_t amd_capture_add2(void *p, int *trunc) {
    asmtest_hwtrace_options_t opts;
    INIT_OPTS(opts, ASMTEST_HWTRACE_AMD_LBR);
    asmtest_hwtrace_init(&opts);
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    asmtest_hwtrace_register_region("amdadd2", p, sizeof ROUTINE, tr);
    add2_fn fn = (add2_fn)p;
    asmtest_hwtrace_begin("amdadd2");
    (void)fn(20, 22);
    asmtest_hwtrace_end("amdadd2");
    uint64_t n = asmtest_emu_trace_insns_total(tr);
    *trunc = asmtest_emu_trace_truncated(tr);
    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    return n;
}

/* lane4 — the sampled small-routine completeness GUARANTEE, LIVE on AMD. A tiny routine's
 * branches are sampled statistically, so an AMD_LBR window can be taken BEFORE the routine's
 * ret retired (or hold only the entry `call` edge). The decoder now fills the trailing
 * straight-line run from the last in-region target to the region exit, so a window it
 * reports COMPLETE (truncated=0) reconstructs the FULL retired set — closing the old
 * undercount-yet-complete gap (docs/internal/analysis/2026-07-12-zen5-privileged-lbr-
 * findings.md). HARD invariant asserted here: EVERY complete AMD_LBR capture of add2 equals
 * the single-step baseline; honest truncation (the cascade escalates) is the accepted
 * alternative, so a run that never samples in-region is not a failure. Sampling is
 * statistical, so retry and check the invariant on each attempt. Self-skips off AMD LBR. */
static void test_amd_live_smallroutine(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_AMD_LBR)) {
        printf(
            "# SKIP AMD LBR small-routine completeness: AMD LBR unavailable\n");
        return;
    }
    void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return;
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);

    uint64_t baseline = ss_capture_add2(p);
    CHECK(baseline == 5,
          "AMD LBR small-routine: single-step baseline is the full "
          "add2 retired set (5)");

    int bad = 0, completes = 0;
    uint64_t last_complete = 0;
    for (int attempt = 0; attempt < 40; attempt++) {
        int trunc = 0;
        uint64_t n = amd_capture_add2(p, &trunc);
        if (!trunc) {
            completes++;
            last_complete = n;
            if (n != baseline)
                bad =
                    1; /* a COMPLETE capture that undercounts the retired set */
        }
    }
    printf("# AMD LBR small-routine: baseline=%llu complete_captures=%d/40 "
           "last_complete_insns=%llu (complete=>full: %s)\n",
           (unsigned long long)baseline, completes,
           (unsigned long long)last_complete, bad ? "VIOLATED" : "held");
    CHECK(!bad,
          "AMD LBR small-routine: every COMPLETE AMD_LBR capture reproduces "
          "the FULL retired set (no undercount-yet-complete; the tail-fill "
          "closes the gap, else honest truncation escalates)");
    munmap(p, sizeof ROUTINE);
}

/* #2A live reach DIAGNOSTIC — period-spaced Tier-B on real hardware. Measures the
 * stitched reach of AMD_LOOP at lbr_period=1 (exact) vs lbr_period=4 (spaced). HONEST
 * finding, not a "gain": a loop is edge-SELF-SIMILAR (its branch offsets — hence from/to
 * edges — repeat every iteration), so the smallest-overlap stitch cannot tell 1 iteration
 * from P and period=4 UNDERCOUNTS it (each window contributes one edge, as
 * test_amd_stitch_period_spaced asserts host-independently). So period-spacing's reach
 * benefit is confined to DISTINCT-edge straight-line paths (inherently short), NOT loops —
 * this prints the live numbers that confirm it. Loose assertion (both reconstruct
 * something); sampling is statistical, so retry and keep the best. */
static void test_amd_reach_period(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_AMD_LBR)) {
        printf("# SKIP AMD #2A reach: AMD LBR live capture unavailable\n");
        return;
    }
    void *q = mmap(NULL, sizeof AMD_LOOP, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q == MAP_FAILED)
        return;
    memcpy(q, AMD_LOOP, sizeof AMD_LOOP);
    mprotect(q, sizeof AMD_LOOP, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)q, (char *)q + sizeof AMD_LOOP);
    uint64_t best1 = 0, best4 = 0;
    int tr1 = 0, tr4 = 0, t = 0;
    for (int attempt = 0; attempt < 12; attempt++) {
        uint64_t n1 = amd_capture_loop_period(q, 20000, 1, &t);
        if (n1 >= best1) {
            best1 = n1;
            tr1 = t;
        }
        uint64_t n4 = amd_capture_loop_period(q, 20000, 4, &t);
        if (n4 >= best4) {
            best4 = n4;
            tr4 = t;
        }
    }
    printf("# AMD #2A reach (self-similar loop): period=1 best_insns=%llu "
           "(truncated=%d), period=4 best_insns=%llu (truncated=%d)\n",
           (unsigned long long)best1, tr1, (unsigned long long)best4, tr4);
    printf(
        "# AMD #2A note: a loop is edge-self-similar, so period=4 UNDERCOUNTS "
        "(smallest-overlap stitch picks one edge/window) — period-spacing's "
        "reach gain is confined to distinct-edge paths, not loops\n");
    CHECK(best1 > 0 && best4 > 0,
          "AMD #2A: both period=1 and period=4 reconstruct from the live LBR");
    munmap(q, sizeof AMD_LOOP);
}

/* Auto-escalating cross-tier CALL (asmtest_trace_call_auto): run a routine under the
 * fastest exact tier and escalate to a ceiling-free tier when the trace comes back
 * truncated. Host-independent plumbing (any x86-64 Linux: the in-proc single-step floor
 * completes both cases with no PMU); on a Zen 5 host the fast LBR tier truncates the loop
 * and `used` reports the escalated (block-step) backend — printed as the live proof that
 * escalation fired. */
static void test_call_auto(void) {
#if defined(__linux__) && defined(__x86_64__)
    /* (a) small routine — some tier captures it complete, correct result. */
    void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        memcpy(p, ROUTINE, sizeof ROUTINE);
        mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);
        asmtest_trace_t *t = asmtest_trace_new(512, 64);
        long args[2] = {20, 22}, result = 0;
        asmtest_trace_choice_t used;
        memset(&used, 0, sizeof used);
        int rc = asmtest_trace_call_auto(p, sizeof ROUTINE, args, 2,
                                         ASMTEST_TRACE_BEST, &result, t, &used);
        printf("# call_auto basic: rc=%d result=%ld used.backend=%d insns=%llu "
               "truncated=%d\n",
               rc, result, used.backend, asmtest_emu_trace_insns_total(t),
               asmtest_emu_trace_truncated(t));
        CHECK(rc == ASMTEST_HW_OK || rc == ASMTEST_HW_EUNAVAIL,
              "call_auto: a small routine runs via some tier (or none "
              "available)");
        if (rc == ASMTEST_HW_OK) {
            CHECK(result == 42, "call_auto: correct result (20+22)");
            CHECK(!asmtest_emu_trace_truncated(t),
                  "call_auto: small routine traces COMPLETE (no escalation "
                  "needed)");
            CHECK(asmtest_trace_covered(t, 0),
                  "call_auto: entry block covered");
        }
        asmtest_trace_free(t);
        munmap(p, sizeof ROUTINE);
    }

    /* (b) a loop past the 16-taken-branch LBR window — call_auto MUST still return a
     * complete trace (escalating to the ceiling-free block-step / single-step tier). */
    void *q = mmap(NULL, sizeof AMD_LOOP, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q != MAP_FAILED) {
        memcpy(q, AMD_LOOP, sizeof AMD_LOOP);
        mprotect(q, sizeof AMD_LOOP, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)q, (char *)q + sizeof AMD_LOOP);
        asmtest_trace_t *lt = asmtest_trace_new(512, 64);
        long largs[2] = {1, 25},
             lr = 0; /* 25 taken back-edges > the 16-deep window */
        asmtest_trace_choice_t lused;
        memset(&lused, 0, sizeof lused);
        int rc = asmtest_trace_call_auto(q, sizeof AMD_LOOP, largs, 2,
                                         ASMTEST_TRACE_BEST, &lr, lt, &lused);
        printf(
            "# call_auto escalate: rc=%d result=%ld used.backend=%d insns=%llu "
            "truncated=%d (%s)\n",
            rc, lr, lused.backend, asmtest_emu_trace_insns_total(lt),
            asmtest_emu_trace_truncated(lt),
            lused.backend == ASMTEST_HWTRACE_AMD_LBR
                ? "LBR window sufficed"
                : "escalated off the LBR window");
        CHECK(rc == ASMTEST_HW_OK || rc == ASMTEST_HW_EUNAVAIL,
              "call_auto: a loop runs via some tier (or none available)");
        if (rc == ASMTEST_HW_OK) {
            CHECK(lr == 25, "call_auto: loop result correct (25 trips)");
            CHECK(!asmtest_emu_trace_truncated(lt),
                  "call_auto: escalates past the 16-branch window to a "
                  "COMPLETE trace");
            CHECK(asmtest_trace_covered(lt, 0x7),
                  "call_auto: loop-body block (0x7) covered");

            /* Anti-vacuity: a 25-back-edge loop CANNOT fit one 16-deep LbrExtV2
             * window, so a result reported COMPLETE must be a real full
             * reconstruction, never a dropped-branch fragment. Establish the honest
             * retired-instruction baseline via a CEILING_FREE call (which EXCLUDES the
             * fixed-window AMD_LBR tier, so it runs on the ceiling-free block-step /
             * single-step floor and is always complete), then require the fast (BEST)
             * capture to EITHER have escalated off AMD_LBR OR reconstruct that exact
             * full count. Without this, a live-AMD sampled-LBR flake that mis-reported
             * a 4-edge fragment (insns << baseline) as truncated=0 passed vacuously
             * (see docs/internal/analysis/2026-07-12-zen5-privileged-lbr-findings.md);
             * now it is a HARD failure. Host-independent: where AMD LBR is absent the
             * fast tier self-skips, `lused.backend` is already the escalated floor, and
             * the OR's first arm holds trivially. */
            asmtest_trace_t *bt = asmtest_trace_new(512, 64);
            long baseline_r = 0;
            asmtest_trace_choice_t bused;
            memset(&bused, 0, sizeof bused);
            int brc = asmtest_trace_call_auto(q, sizeof AMD_LOOP, largs, 2,
                                              ASMTEST_TRACE_CEILING_FREE,
                                              &baseline_r, bt, &bused);
            if (brc == ASMTEST_HW_OK && !asmtest_emu_trace_truncated(bt)) {
                unsigned long long baseline = asmtest_emu_trace_insns_total(bt);
                /* 1 (mov) + 25*3 (add/dec/jnz) + 1 (ret) = 77 retired instructions. */
                CHECK(
                    baseline == 77,
                    "call_auto: ceiling-free baseline is the full 25-trip loop "
                    "(77 retired insns)");
                int honest = (lused.backend != ASMTEST_HWTRACE_AMD_LBR) ||
                             (asmtest_emu_trace_insns_total(lt) == baseline);
                CHECK(
                    honest,
                    "call_auto: a COMPLETE result reconstructs the FULL "
                    "retired "
                    "path (escalated off AMD_LBR, or a real Tier-B stitch — a "
                    "dropped-branch fragment is NOT passed as complete)");
            }
            asmtest_trace_free(bt);
        }
        asmtest_trace_free(lt);
        munmap(q, sizeof AMD_LOOP);
    }

    /* (c) Phase 8 MSR-direct rung — exercised only where the MSR substrate is present (the
     * privileged docker-hwtrace-msr lane); self-skips otherwise, where (a)/(b) already prove
     * the cascade. When the fast sampled tier truncates a too-fast tiny routine, the
     * zero-interrupt MSR read is tried BEFORE the ~1000x block-step tier. The MSR read shares
     * AMD_LBR's 16-deep ceiling (freeze-glue contaminated), so rather than over-asserting it
     * WINS, this checks the robust contract — correct result + a COMPLETE trace via some tier
     * (the rung never breaks the cascade), with the rung's outcome printed — and that
     * CEILING_FREE EXCLUDES the AMD_LBR-family rung so a truncating capture escalates to a
     * ceiling-free stepper instead. The existing (a)/(b) cases running in this same lane are
     * the regression backstop for a rung crash / wrong result. */
    if (asmtest_amd_msr_available()) {
        void *m = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED) {
            memcpy(m, ROUTINE, sizeof ROUTINE);
            mprotect(m, sizeof ROUTINE, PROT_READ | PROT_EXEC);
            __builtin___clear_cache((char *)m, (char *)m + sizeof ROUTINE);
            long margs[2] = {20, 22};

            asmtest_trace_t *mt = asmtest_trace_new(512, 64);
            long mr = 0;
            asmtest_trace_choice_t mused;
            memset(&mused, 0, sizeof mused);
            int mrc =
                asmtest_trace_call_auto(m, sizeof ROUTINE, margs, 2,
                                        ASMTEST_TRACE_BEST, &mr, mt, &mused);
            printf("# call_auto msr-rung: rc=%d result=%ld used.backend=%d "
                   "truncated=%d (%s)\n",
                   mrc, mr, mused.backend, asmtest_emu_trace_truncated(mt),
                   mused.backend == ASMTEST_HWTRACE_AMD_LBR
                       ? "MSR read completed the tiny routine"
                       : "fell through to a ceiling-free floor");
            CHECK(mrc == ASMTEST_HW_OK,
                  "call_auto (msr lane): tiny routine traced by some tier");
            CHECK(mr == 42, "call_auto (msr lane): correct result via the MSR "
                            "rung / floor");
            CHECK(!asmtest_emu_trace_truncated(mt),
                  "call_auto (msr lane): a COMPLETE trace (MSR rung or the "
                  "block-step floor)");
            asmtest_trace_free(mt);

            /* CEILING_FREE must EXCLUDE the MSR rung (its 16-deep AMD_LBR ceiling). */
            asmtest_trace_t *ct = asmtest_trace_new(512, 64);
            long cr = 0;
            asmtest_trace_choice_t cused;
            memset(&cused, 0, sizeof cused);
            int crc = asmtest_trace_call_auto(m, sizeof ROUTINE, margs, 2,
                                              ASMTEST_TRACE_CEILING_FREE, &cr,
                                              ct, &cused);
            CHECK(crc == ASMTEST_HW_OK && cr == 42,
                  "call_auto CEILING_FREE (msr lane): correct result via a "
                  "ceiling-free tier");
            CHECK(cused.backend != ASMTEST_HWTRACE_AMD_LBR,
                  "call_auto CEILING_FREE (msr lane): the MSR rung (AMD_LBR "
                  "ceiling) is excluded");
            asmtest_trace_free(ct);
            munmap(m, sizeof ROUTINE);
        }
    }
#else
    printf("# SKIP call_auto: not Linux x86-64\n");
#endif
}

static void test_stealth_window_inline(void) {
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
    if (!asmtest_ptrace_available()) {
        printf("# SKIP stealth_window_inline: ptrace unavailable\n");
        return;
    }

    void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP stealth_window_inline: mmap failed\n");
        return;
    }
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);

    asmtest_addr_channel_t *chan = asmtest_addr_channel_new_shared();
    CHECK(chan != NULL, "stealth_window_inline: alloc channel");
    asmtest_addr_channel_publish(chan, (uint64_t)(uintptr_t)p, sizeof ROUTINE,
                                 0);

    void *ctx = NULL;
    int rc = asmtest_hwtrace_stealth_window_begin(chan, &ctx);
    CHECK(rc == ASMTEST_HW_OK || rc == ASMTEST_HW_EUNAVAIL,
          "stealth_window_inline: begin split (or Yama skip)");
    if (rc == ASMTEST_HW_OK) {
        CHECK(ctx != NULL, "stealth_window_inline: valid context returned");

        long (*fn)(long, long) = (long (*)(long, long))p;
        long res = fn(10, 32);
        CHECK(res == 42, "stealth_window_inline: correct result (10+32)");

        asmtest_trace_t *tr = asmtest_trace_new(512, 64);
        int end_rc = asmtest_hwtrace_stealth_window_end(ctx, tr);
        CHECK(end_rc == ASMTEST_HW_OK,
              "stealth_window_inline: end split succeeded");

        printf("# stealth_window_inline: insns=%llu blocks=%llu truncated=%d\n",
               (unsigned long long)asmtest_emu_trace_insns_total(tr),
               (unsigned long long)asmtest_emu_trace_blocks_total(tr),
               asmtest_emu_trace_truncated(tr));

        CHECK(asmtest_emu_trace_insns_total(tr) > 0,
              "stealth_window_inline: captured instructions in window");

        asmtest_trace_free(tr);
    }
    asmtest_addr_channel_free_shared(chan);
    munmap(p, sizeof ROUTINE);
#else
    printf("# SKIP stealth_window_inline: not Linux x86-64/AArch64\n");
#endif
}

#if defined(__linux__) && defined(__x86_64__)
static long g_msr_result;
static void msr_run_loop(void *arg) {
    long (*fn)(long, long) = (long (*)(long, long))arg;
    g_msr_result = fn(1, 4); /* AMD_LOOP: 4 trips -> 3 taken jnz back-edges */
}
#endif

/* AMD MSR-direct LBR snapshot (asmtest_amd_msr_trace): read the LbrExtV2 FROM/TO MSRs
 * directly for a zero-PMU-interrupt Tier-A capture. Needs amd_lbr_v2 + /dev/cpu/N/msr
 * (CAP_SYS_ADMIN + msr module) — self-skips everywhere else (all ordinary CI lanes),
 * runs only in the privileged docker-hwtrace-msr lane on the Zen 5 dev box. Validated
 * empirically: a tiny routine's branches survive the freeze-syscall glue. */
static void test_amd_msr(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_amd_msr_available()) {
        printf("# SKIP AMD MSR-direct: substrate absent (needs amd_lbr_v2 + "
               "/dev/cpu/N/msr + CAP_SYS_ADMIN)\n");
        return;
    }
    void *q = mmap(NULL, sizeof AMD_LOOP, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q == MAP_FAILED)
        return;
    memcpy(q, AMD_LOOP, sizeof AMD_LOOP);
    mprotect(q, sizeof AMD_LOOP, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)q, (char *)q + sizeof AMD_LOOP);
    asmtest_trace_t *t = asmtest_trace_new(64, 64);
    g_msr_result = 0;
    int rc = asmtest_amd_msr_trace(q, sizeof AMD_LOOP, msr_run_loop, q, t);
    printf("# AMD MSR-direct: rc=%d result=%ld insns=%llu covered(0x7)=%d "
           "truncated=%d\n",
           rc, g_msr_result, asmtest_emu_trace_insns_total(t),
           asmtest_trace_covered(t, 0x7), asmtest_emu_trace_truncated(t));
    CHECK(rc == ASMTEST_HW_OK,
          "AMD MSR-direct: zero-interrupt capture succeeds");
    CHECK(g_msr_result == 4, "AMD MSR-direct: the region ran (fn(1,4)=4)");
    CHECK(asmtest_emu_trace_insns_total(t) > 0 ||
              asmtest_emu_trace_truncated(t),
          "AMD MSR-direct: honest (in-region instructions reconstructed, or "
          "truncated)");
    CHECK(asmtest_trace_covered(t, 0x7) || asmtest_emu_trace_truncated(t),
          "AMD MSR-direct: loop-body block 0x7 captured from the real LBR MSRs "
          "(or honest truncation)");
    asmtest_trace_free(t);
    munmap(q, sizeof AMD_LOOP);
#else
    printf("# SKIP AMD MSR-direct: not Linux x86-64\n");
#endif
}

/* AMD MSR-direct SPEC FILTER (host-independent, no /dev/cpu/msr): the MSR path reads raw
 * LbrExtV2 FROM/TO MSRs, where TO[63]=valid(retired) and TO[62]=spec(wrong-path). A
 * spec-only slot (valid=0, spec=1) must be DROPPED at the source — it cannot set
 * perf_branch_entry.spec, so amd_replay's PERF_BR_SPEC_WRONG_PATH filter would never catch
 * it and a phantom edge would enter the reconstruction. Drives asmtest_amd_msr_decode_entry
 * with synthetic MSR words. Regression guard: against the pre-fix `valid || spec` test the
 * spec-only case would wrongly be KEPT. */
static void test_amd_msr_spec_filter(void) {
#if defined(__linux__) && defined(__x86_64__)
    const uint64_t VALID = 1ULL << 63;   /* TO[63] retired      */
    const uint64_t SPEC = 1ULL << 62;    /* TO[62] speculative  */
    const uint64_t MISPRED = 1ULL << 63; /* FROM[63] mispredict */
    const uint64_t IPMASK = (1ULL << 58) - 1;
    const uint64_t FROM_IP = 0x0000401000ULL, TO_IP = 0x0000402000ULL;
    struct perf_branch_entry e;

    /* Retired (valid=1, spec=0): kept; IPs masked to [57:0] (FROM mispredict and TO
     * valid/spec bits stripped). */
    memset(&e, 0, sizeof e);
    int k_ret =
        asmtest_amd_msr_decode_entry(MISPRED | FROM_IP, VALID | TO_IP, &e);
    CHECK(k_ret == 1, "MSR spec filter: a retired entry (valid=1) is kept");
    CHECK(e.from == (FROM_IP & IPMASK) && e.to == (TO_IP & IPMASK),
          "MSR spec filter: kept entry carries the IP-masked from/to");

    /* Spec-only (valid=0, spec=1): the bug case — must be DROPPED at the source. */
    int k_spec = asmtest_amd_msr_decode_entry(FROM_IP, SPEC | TO_IP, &e);
    CHECK(k_spec == 0, "MSR spec filter: a spec-only wrong-path slot "
                       "(valid=0,spec=1) is dropped");

    /* Empty slot (valid=0, spec=0): dropped. */
    int k_empty = asmtest_amd_msr_decode_entry(FROM_IP, TO_IP, &e);
    CHECK(k_empty == 0,
          "MSR spec filter: an empty slot (valid=0,spec=0) is dropped");

    /* Retired AND speculative (valid=1, spec=1, correct-path): it retired -> kept. */
    int k_both =
        asmtest_amd_msr_decode_entry(FROM_IP, VALID | SPEC | TO_IP, &e);
    CHECK(k_both == 1,
          "MSR spec filter: a retired correct-path-speculative entry is kept");
#else
    printf("# SKIP AMD MSR spec filter: not Linux x86-64\n");
#endif
}

/* AMD tail-call EXIT CLASSIFICATION (host-independent): asmtest_amd_last_exit_off derives
 * the boundary-snapshot breakpoint site and counts region exits. Phase 9 widened "exit"
 * from ret-only to ALSO a region-LEAVING direct unconditional jmp (a tail call), so a
 * genuinely single-exit tail-call routine takes the deterministic snapshot by default
 * instead of the truncating sampled path. Both guards must hold: an INDIRECT jmp
 * (unprovable target) and an IN-region direct jmp (an ordinary loop/forward branch) must
 * NOT count. Needs Capstone. */
static void test_amd_tailcall_exit(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_amd_decoder_present()) {
        printf("# SKIP AMD tail-call exit: built without Capstone\n");
        return;
    }
    /* (1) A lone region-LEAVING tail-call jmp, no ret: mov rax,rdi; jmp rel32(+0x10, past
     * the len-8 region). One exit, at the jmp (offset 3). The pre-Phase-9 ret-only deriver
     * would report ZERO exits here and force the sampled fallback. */
    {
        const unsigned char R[] = {0x48, 0x89, 0xf8, /* mov rax,rdi */
                                   0xe9, 0x10, 0x00,
                                   0x00, 0x00}; /* jmp +0x10 */
        int nexit = -1;
        size_t off = asmtest_amd_last_exit_off(R, sizeof R, &nexit);
        CHECK(nexit == 1 && off == 3, "tail-call exit: a lone region-leaving "
                                      "direct jmp is the single exit");
    }
    /* (2) ret + region-leaving tail jmp = TWO exits -> default-on withheld (not single). */
    {
        const unsigned char R[] = {0xc3, /* ret */
                                   0xe9, 0x10, 0x00,
                                   0x00, 0x00}; /* jmp +0x10 */
        int nexit = -1;
        size_t off = asmtest_amd_last_exit_off(R, sizeof R, &nexit);
        CHECK(nexit == 2 && off == 1, "tail-call exit: ret + tail-jmp counts "
                                      "as two exits (last at the jmp)");
    }
    /* (3) INDIRECT jmp (jmp rax): is_uncond_jump but no decodable target -> NOT an exit. */
    {
        const unsigned char R[] = {0xff, 0xe0}; /* jmp rax */
        int nexit = -1;
        size_t off = asmtest_amd_last_exit_off(R, sizeof R, &nexit);
        CHECK(
            nexit == 0 && off == (size_t)-1,
            "tail-call exit: an indirect jmp is unprovable and does NOT count");
    }
    /* (4) IN-region direct jmp (jmp +1 -> offset 6, inside the len-7 region): an ordinary
     * loop/forward branch, NOT an exit. */
    {
        const unsigned char R[] = {0xe9, 0x01, 0x00, 0x00, 0x00, /* jmp +1 */
                                   0x90, 0x90};                  /* nops */
        int nexit = -1;
        size_t off = asmtest_amd_last_exit_off(R, sizeof R, &nexit);
        CHECK(nexit == 0 && off == (size_t)-1,
              "tail-call exit: an in-region direct jmp (loop/forward) does NOT "
              "count");
    }
    /* --- P5 asmtest_amd_all_exits: full exit ENUMERATION (the multi-exit boundary
     * snapshot arms one breakpoint per enumerated offset). --- */
    /* (5) A 3-ret region: all three offsets written ASCENDING, count 3, return == last. */
    {
        const unsigned char R[] = {0xc3, 0x90, 0xc3, 0x90, 0xc3};
        size_t offs[4] = {(size_t)-2, (size_t)-2, (size_t)-2, (size_t)-2};
        int nexit = -1;
        size_t off = asmtest_amd_all_exits(R, sizeof R, offs, 4, &nexit);
        CHECK(nexit == 3 && off == 4 && offs[0] == 0 && offs[1] == 2 &&
                  offs[2] == 4 && offs[3] == (size_t)-2,
              "all exits: a 3-ret region enumerates 3 ascending offsets, "
              "return == last");
    }
    /* (6) FIVE exits with cap=4 (more exits than debug registers): the total count is
     * still 5 — so hwtrace_begin_amd withholds the default-on snapshot — only the first
     * 4 are written (out[4] untouched), and the return is the TRUE last exit even
     * though it did not fit the buffer. */
    {
        const unsigned char R[] = {0xc3, 0xc3, 0xc3, 0xc3, 0xc3};
        size_t offs[5] = {(size_t)-2, (size_t)-2, (size_t)-2, (size_t)-2,
                          (size_t)-2};
        int nexit = -1;
        size_t off = asmtest_amd_all_exits(R, sizeof R, offs, 4, &nexit);
        CHECK(nexit == 5 && off == 4 && offs[0] == 0 && offs[1] == 1 &&
                  offs[2] == 2 && offs[3] == 3 && offs[4] == (size_t)-2,
              "all exits: 5 exits at cap=4 reports the TRUE total (5) and "
              "last (4), writes only 4");
    }
    /* (7) ret + region-leaving tail jmp: BOTH enumerated (n==2), ascending. */
    {
        const unsigned char R[] = {0xc3, /* ret */
                                   0xe9, 0x10, 0x00,
                                   0x00, 0x00}; /* jmp +0x10 */
        size_t offs[4] = {(size_t)-2, (size_t)-2, (size_t)-2, (size_t)-2};
        int nexit = -1;
        size_t off = asmtest_amd_all_exits(R, sizeof R, offs, 4, &nexit);
        CHECK(nexit == 2 && off == 1 && offs[0] == 0 && offs[1] == 1,
              "all exits: ret + tail-jmp enumerates both exits (ret then jmp)");
        /* Classification contract lock: the thin last_exit_off wrapper agrees with
         * all_exits byte-for-byte (same count, same last offset). */
        int nexit_w = -1;
        size_t off_w = asmtest_amd_last_exit_off(R, sizeof R, &nexit_w);
        CHECK(nexit_w == nexit && off_w == off,
              "all exits: last_exit_off is a thin wrapper (same count + last)");
    }
#else
    printf("# SKIP AMD tail-call exit: not Linux x86-64\n");
#endif
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

/* AMD #2A — period-spaced Tier-B stitching (validates the opt-in lbr_period path). With
 * lbr_period=P the live capture fires a PMI every P taken branches instead of every one,
 * so consecutive 16-deep windows overlap by (depth - P) instead of (depth - 1) — ~P x
 * fewer interrupts before throttling/ring-overflow truncates the run. The SAME
 * asmtest_amd_stitch splices them: for a path of DISTINCT edges the smallest-overlap
 * heuristic finds the true shift P (the distinct edges disambiguate the alignment), so
 * all edges are recovered gaplessly. Host-independent (synthetic period-spaced windows,
 * base=NULL so the decodable-distance guard is inert on the abstract edges).
 *
 * CAVEAT (why the default stays lbr_period=0 / period=1): a SELF-SIMILAR loop whose
 * every taken edge is identical gives the smallest-overlap heuristic no way to tell 1
 * iteration from P, so under period>1 it silently UNDERCOUNTS (picks shift d=1). period=1
 * is the only universally exact value; lbr_period>1 is a coverage/throttle trade the
 * caller opts into for distinct-edge hot paths, verified for reach on a live Zen host. */
static void test_amd_stitch_period_spaced(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_amd_decoder_present()) {
        printf("# SKIP AMD period-spaced stitch: built without Capstone\n");
        return;
    }
    const uint64_t b = (uint64_t)(uintptr_t)AMD_LOOP;
    enum { K = 18, P = 4 };          /* 18 distinct edges, sampled every P=4 */
    struct perf_branch_entry seq[K]; /* execution order, all edges DISTINCT */
    memset(seq, 0, sizeof seq);
    for (int i = 0; i < K; i++) {
        seq[i].from = b + 0x100 + (uint64_t)i;
        seq[i].to = b + 0x300 + (uint64_t)i;
    }
    /* sample_period=P: a sample fires at global edge index g = P-1, 2P-1, ... plus a
     * final flush at the last edge (K-1). Each window holds the last min(16, g+1)
     * edges, newest-first — consecutive windows overlap by (depth - P). */
    int gpos[8];
    int ns = 0;
    for (int g = P - 1; g < K; g += P)
        gpos[ns++] = g;
    if (ns == 0 || gpos[ns - 1] != K - 1)
        gpos[ns++] = K - 1; /* final exit sample reaches the newest edge */
    struct perf_branch_entry win[8][16];
    const struct perf_branch_entry *samples[8];
    size_t nrs[8];
    for (int s = 0; s < ns; s++) {
        int g = gpos[s];
        int depth = (g + 1 < 16) ? (g + 1) : 16;
        for (int e = 0; e < depth; e++)
            win[s][e] = seq[g - e]; /* newest-first */
        samples[s] = win[s];
        nrs[s] = (size_t)depth;
    }

    struct perf_branch_entry out[64];
    int gap = 0;
    size_t n =
        asmtest_amd_stitch(samples, nrs, (size_t)ns, NULL, 0, 0, out, 64, &gap);
    CHECK(
        n == K && gap == 0,
        "AMD period-spaced (P=4) distinct-edge windows stitch to all 18 edges");
    /* Output is newest-first: out[0] == seq[K-1] ... out[K-1] == seq[0]. */
    int order = (n == K);
    for (size_t i = 0; order && i < (size_t)K; i++)
        order = (out[i].from == seq[K - 1 - i].from &&
                 out[i].to == seq[K - 1 - i].to);
    CHECK(order,
          "AMD period-spaced stitch recovers the exact edge sequence (distinct "
          "edges disambiguate each window's shift)");

    /* Caveat, made concrete: the SAME period-spacing over a homogeneous loop (every
     * edge identical) undercounts — the smallest-overlap heuristic cannot recover P.
     * This is why the default is lbr_period=0 (period=1, universally exact). */
    struct perf_branch_entry same[K];
    memset(same, 0, sizeof same);
    for (int i = 0; i < K; i++) {
        same[i].from =
            b + 0xd; /* AMD_LOOP back-edge, identical every iteration */
        same[i].to = b + 0x7;
    }
    struct perf_branch_entry hwin[8][16];
    const struct perf_branch_entry *hsamples[8];
    size_t hnrs[8];
    for (int s = 0; s < ns; s++) {
        int g = gpos[s];
        int depth = (g + 1 < 16) ? (g + 1) : 16;
        for (int e = 0; e < depth; e++)
            hwin[s][e] = same[g - e];
        hsamples[s] = hwin[s];
        hnrs[s] = (size_t)depth;
    }
    struct perf_branch_entry hout[64];
    int hgap = 0;
    size_t hn = asmtest_amd_stitch(hsamples, hnrs, (size_t)ns, AMD_LOOP, b,
                                   sizeof AMD_LOOP, hout, 64, &hgap);
    CHECK(hn < K,
          "AMD period-spaced stitch UNDERCOUNTS a self-similar loop (documents "
          "why default period=1)");
#else
    printf("# SKIP AMD period-spaced stitch: not Linux x86-64\n");
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
/* Append a PERF_RECORD_SAMPLE {u64 nr; perf_branch_entry[n]} to buf at *off. When
 * claim_nr is nonzero it OVERRIDES the encoded nr field (to forge a corrupt/oversized
 * nr) while only `n` real entries are written — the F5 malicious-nr fixture. */
static void ring_put_sample(uint8_t *buf, size_t *off,
                            const struct perf_branch_entry *e, size_t n,
                            uint64_t claim_nr) {
    struct perf_event_header h;
    memset(&h, 0, sizeof h);
    h.type = PERF_RECORD_SAMPLE;
    h.size = (uint16_t)(sizeof h + sizeof(uint64_t) +
                        n * sizeof(struct perf_branch_entry));
    memcpy(buf + *off, &h, sizeof h);
    uint64_t nr = claim_nr ? claim_nr : (uint64_t)n;
    memcpy(buf + *off + sizeof h, &nr, sizeof nr);
    if (n > 0)
        memcpy(buf + *off + sizeof h + sizeof(uint64_t), e,
               n * sizeof(struct perf_branch_entry));
    *off += h.size;
}
/* Append a bare PERF_RECORD_LOST header (the "ring dropped the tail" marker). */
static void ring_put_lost(uint8_t *buf, size_t *off) {
    struct perf_event_header h;
    memset(&h, 0, sizeof h);
    h.type = PERF_RECORD_LOST;
    h.size = (uint16_t)sizeof h;
    memcpy(buf + *off, &h, sizeof h);
    *off += h.size;
}
#endif

/* F43 — host-independent test of the AMD perf data-ring parse/select
 * (asmtest_amd_ring_parse_decode, the seam hwtrace_end_amd calls after linearizing the
 * mmap'd ring). Every LIVE path into this logic self-skips off AMD LBR hardware (always,
 * on Intel/CI), so a crafted {perf_event_header + u64 nr + perf_branch_entry[]} buffer is
 * the only coverage the ring framing / richest-in-region pick / oversized-nr clamp / LOST
 * / near-full / Tier-A-overflow logic gets. Runs on any Linux x86-64 with Capstone. */
static void test_amd_ring_parse(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_amd_decoder_present()) {
        printf("# SKIP AMD ring parse: built without Capstone\n");
        return;
    }
    const uint64_t b = (uint64_t)(uintptr_t)ROUTINE;

    /* (1) Richest-in-region window pick. An earlier glue-only window (both endpoints
     * far outside the region -> inregion 0) then the real add2 stack [ret(0x11->out),
     * jle(0xc->0x11)] (2 in-region). The parser must select the richer window and
     * reconstruct the exact add2 stream; because the window holds the ret exit branch
     * it is COMPLETE on freeze and non-freeze parts alike. */
    {
        uint8_t buf[512];
        size_t off = 0;
        struct perf_branch_entry glue[1];
        memset(glue, 0, sizeof glue);
        glue[0].from = b + 0x10000; /* far outside [b, b+len) */
        glue[0].to = b + 0x10008;
        struct perf_branch_entry rich[2];
        memset(rich, 0, sizeof rich);
        rich[0].from = b + 0x11;
        rich[0].to = b + sizeof ROUTINE; /* ret -> outside (the exit edge) */
        rich[1].from = b + 0xc;
        rich[1].to = b + 0x11; /* jle -> ret */
        ring_put_sample(buf, &off, glue, 1, 0);
        ring_put_sample(buf, &off, rich, 2, 0);

        asmtest_trace_t *tr = asmtest_trace_new(64, 64);
        asmtest_amd_ring_parse_decode(buf, off, 0, ROUTINE, sizeof ROUTINE, tr);
        static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
        int seq = (asmtest_emu_trace_insns_total(tr) == 5);
        for (size_t i = 0; seq && i < 5; i++)
            seq = (tr->insns[i] == EXPECT[i]);
        CHECK(seq, "AMD ring parse: picks the richest-in-region window "
                   "([0,3,6,c,11])");
        CHECK(!asmtest_emu_trace_truncated(tr),
              "AMD ring parse: a richest window holding the exit branch is "
              "COMPLETE");
        asmtest_trace_free(tr);
    }

    /* (2) Malicious/oversized nr (the F5 clamp). A SAMPLE whose nr field claims 2^63
     * while its body holds none: nr * sizeof(perf_branch_entry) WRAPS to 0, so without
     * the nr<=64 clamp the size check passes and the e[i] scan runs 2^63 times off the
     * end of the buffer. The clamp rejects the record; the parser returns with an empty,
     * honestly-truncated trace and no out-of-bounds read (proven under ASan). */
    {
        uint8_t buf[64];
        size_t off = 0;
        ring_put_sample(buf, &off, NULL, 0, (uint64_t)1 << 63);
        asmtest_trace_t *tr = asmtest_trace_new(64, 64);
        asmtest_amd_ring_parse_decode(buf, off, 0, ROUTINE, sizeof ROUTINE, tr);
        CHECK(asmtest_emu_trace_insns_total(tr) == 0 &&
                  asmtest_emu_trace_truncated(tr),
              "AMD ring parse: an oversized nr is rejected (F5 clamp; "
              "empty+truncated, no OOB)");
        asmtest_trace_free(tr);
    }

    /* (3) LOST detection. A COMPLETE add2 window followed by a PERF_RECORD_LOST record
     * must be flagged truncated even though the window decoded fully (the ring dropped
     * the run's tail — the surviving prefix is not the whole run). */
    {
        uint8_t buf[512];
        size_t off = 0;
        struct perf_branch_entry rich[2];
        memset(rich, 0, sizeof rich);
        rich[0].from = b + 0x11;
        rich[0].to = b + sizeof ROUTINE;
        rich[1].from = b + 0xc;
        rich[1].to = b + 0x11;
        ring_put_sample(buf, &off, rich, 2, 0);
        ring_put_lost(buf, &off);
        asmtest_trace_t *tr = asmtest_trace_new(64, 64);
        asmtest_amd_ring_parse_decode(buf, off, 0, ROUTINE, sizeof ROUTINE, tr);
        CHECK(asmtest_emu_trace_insns_total(tr) == 5 &&
                  asmtest_emu_trace_truncated(tr),
              "AMD ring parse: a PERF_RECORD_LOST after a full window flags "
              "truncated");
        asmtest_trace_free(tr);
    }

    /* (4) Tier-A overflow prefix. A single depth-deep window of identical AMD_LOOP
     * back-edges (best_nr >= depth, n_samples == 1 so no Tier-B stitch) is honestly
     * truncated — the window overflowed and there is no second window to stitch. */
    {
        const uint64_t lb = (uint64_t)(uintptr_t)AMD_LOOP;
        int depth = asmtest_amd_lbr_depth();
        struct perf_branch_entry full[64];
        memset(full, 0, sizeof full);
        for (int i = 0; i < depth && i < 64; i++) {
            full[i].from = lb + 0xd;
            full[i].to = lb + 0x7;
        }
        uint8_t buf[64 * sizeof(struct perf_branch_entry) + 64];
        size_t off = 0;
        ring_put_sample(buf, &off, full, (size_t)depth, 0);
        asmtest_trace_t *tr = asmtest_trace_new(256, 64);
        asmtest_amd_ring_parse_decode(buf, off, 0, AMD_LOOP, sizeof AMD_LOOP,
                                      tr);
        CHECK(asmtest_emu_trace_truncated(tr),
              "AMD ring parse: a lone depth-deep window (Tier-A overflow) "
              "truncates");
        asmtest_trace_free(tr);
    }

    /* (5) Near-full ring, no LOST record. A span within one max-size sample of `dsz`
     * is treated as loss even though the window decoded complete (a filled non-overwrite
     * ring emits no LOST — the kernel never gets the next reservation). */
    {
        uint8_t buf[512];
        size_t off = 0;
        struct perf_branch_entry rich[2];
        memset(rich, 0, sizeof rich);
        rich[0].from = b + 0x11;
        rich[0].to = b + sizeof ROUTINE;
        rich[1].from = b + 0xc;
        rich[1].to = b + 0x11;
        ring_put_sample(buf, &off, rich, 2, 0);
        asmtest_trace_t *tr = asmtest_trace_new(64, 64);
        /* dsz only a few bytes above span, so span + one max-size sample overruns it. */
        asmtest_amd_ring_parse_decode(buf, off, off + 8, ROUTINE,
                                      sizeof ROUTINE, tr);
        CHECK(asmtest_emu_trace_truncated(tr),
              "AMD ring parse: a near-full ring (span+max_sample>dsz) flags "
              "truncated");
        asmtest_trace_free(tr);
    }
#else
    printf("# SKIP AMD ring parse: not Linux x86-64\n");
#endif
}

/* F44 — the dropped-uncond-jmp FOLLOW in amd_span_decodable (amd_backend.c), reached only
 * through asmtest_amd_stitch's splice-decodability guard. The three existing stitch tests
 * never enter that branch (AMD_LOOP's back-edge is a conditional jnz, and the period-spaced
 * test passes base=NULL, short-circuiting the guard). Here a reduced-filter loop drops a
 * direct in-region jmp, so validating each window's overlap requires FOLLOWING that jmp
 * across the splice — assert the stitch stays gapless. A second window pair puts the splice
 * across a region-LEAVING jmp (the "cannot disprove" arm). Host-independent. */
static void test_amd_stitch_reduced_filter(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_amd_decoder_present()) {
        printf("# SKIP AMD reduced-filter stitch: built without Capstone\n");
        return;
    }
    /* mov rax,0 ; L: add rax,rsi ; jmp 0x0f (DROPPED, skips 3 dead bytes) ; dec rsi ;
     * jnz L (RECORDED back-edge) ; ret. Under the reduced filter only the jnz is
     * recorded, so the splice out[..].to(L=0x07) -> next.from(jnz=0x12) must follow the
     * dropped jmp at 0x0a to decode. */
    static const unsigned char RLOOP[] = {
        0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00, 0x00, /* 0x00 mov rax,0 */
        0x48, 0x01, 0xf8,                         /* 0x07 add rax,rdi */
        0xeb, 0x03,                               /* 0x0a jmp 0x0f    */
        0xcc, 0xcc, 0xcc,                         /* 0x0c dead        */
        0x48, 0xff, 0xce,                         /* 0x0f dec rsi     */
        0x75, 0xf3,                               /* 0x12 jnz 0x07    */
        0xc3};                                    /* 0x14 ret         */
    const uint64_t b = (uint64_t)(uintptr_t)RLOOP;
    enum { K = 18 };
    struct perf_branch_entry
        edge[K]; /* the lone recorded jnz back-edge, K times */
    memset(edge, 0, sizeof edge);
    for (int i = 0; i < K; i++) {
        edge[i].from = b + 0x12;
        edge[i].to = b + 0x07;
    }
    struct perf_branch_entry win[K][16];
    const struct perf_branch_entry *samples[K];
    size_t nrs[K];
    for (int j = 0; j < K; j++) {
        int depth = (j + 1 < 16) ? (j + 1) : 16;
        for (int e = 0; e < depth; e++)
            win[j][e] = edge[0]; /* every edge identical */
        samples[j] = win[j];
        nrs[j] = (size_t)depth;
    }
    struct perf_branch_entry out[64];
    int gap = 0;
    size_t n = asmtest_amd_stitch(samples, nrs, K, RLOOP, b, sizeof RLOOP, out,
                                  64, &gap);
    CHECK(n == K && gap == 0,
          "AMD reduced-filter stitch: splice follows the dropped in-region jmp "
          "(gapless)");
    asmtest_trace_t *tb = asmtest_trace_new(256, 64);
    int rc = asmtest_amd_decode_stitched(out, n, RLOOP, sizeof RLOOP, tb, gap);
    CHECK(rc == ASMTEST_HW_OK && !asmtest_emu_trace_truncated(tb),
          "AMD reduced-filter stitch: the stitched sequence decodes COMPLETE");
    CHECK(asmtest_trace_covered(tb, 0x07) && !asmtest_trace_covered(tb, 0x0c),
          "AMD reduced-filter stitch: loop body (0x07) covered, dead bytes "
          "(0x0c) are not");
    asmtest_trace_free(tb);

    /* Region-LEAVING jmp arm: the splice from-point decodes a direct uncond jmp whose
     * target is OUTSIDE the region, so amd_span_decodable cannot disprove the splice and
     * accepts it (gap == 0). jmp +0x20 (target 0x22, past the len-8 region) at 0x00. */
    static const unsigned char LEAVE[] = {0xeb, 0x20, 0xcc, 0xcc,
                                          0xcc, 0x90, 0x90, 0x90};
    const uint64_t lb = (uint64_t)(uintptr_t)LEAVE;
    struct perf_branch_entry A;
    memset(&A, 0, sizeof A);
    A.from = lb + 0x05; /* the next-window source the splice must reach */
    A.to = lb + 0x00;   /* lands on the region-leaving jmp              */
    struct perf_branch_entry w0[1] = {A};
    struct perf_branch_entry w1[2] = {A, A};
    const struct perf_branch_entry *ls[2] = {w0, w1};
    size_t ln[2] = {1, 2};
    struct perf_branch_entry lout[8];
    int lgap = 0;
    size_t ln2 =
        asmtest_amd_stitch(ls, ln, 2, LEAVE, lb, sizeof LEAVE, lout, 8, &lgap);
    CHECK(ln2 == 2 && lgap == 0,
          "AMD reduced-filter stitch: a splice across a region-leaving jmp is "
          "accepted (cannot disprove)");
#else
    printf("# SKIP AMD reduced-filter stitch: not Linux x86-64\n");
#endif
}

/* P9 — the shared /proc/cpuinfo flag probe. asmtest_amd_flags_have is pure (no I/O), so
 * synthetic `flags` lines exercise the exact token semantics host-independently; the cached
 * whole-file asmtest_amd_has_cpu_flag is asserted stable (the dedup must not change any
 * availability verdict). */
static void test_amd_cpu_flag(void) {
#if defined(__linux__) && defined(__x86_64__)
    const char *line = "flags\t: fpu vme de amd_lbr_v2 perfmon_v2 lm\n";
    CHECK(asmtest_amd_flags_have(line, "amd_lbr_v2") == 1,
          "cpu flag: a present token matches");
    CHECK(asmtest_amd_flags_have(line, "perfmon_v2") == 1,
          "cpu flag: a second present token matches");
    CHECK(asmtest_amd_flags_have(line, "fpu") == 1,
          "cpu flag: the first token (right after the colon) matches");
    CHECK(asmtest_amd_flags_have(line, "lm") == 1,
          "cpu flag: the last token (newline-terminated) matches");
    CHECK(asmtest_amd_flags_have(line, "avx512") == 0,
          "cpu flag: an absent token does not match");
    CHECK(asmtest_amd_flags_have(line, "amd_lbr_v") == 0,
          "cpu flag: a right-truncated token ('amd_lbr_v') does not match");
    CHECK(asmtest_amd_flags_have("flags\t: xamd_lbr_v2 lm\n", "amd_lbr_v2") ==
              0,
          "cpu flag: a left-prefixed token ('xamd_lbr_v2') does not match");
    CHECK(asmtest_amd_flags_have(line, "") == 0 &&
              asmtest_amd_flags_have(NULL, "lm") == 0 &&
              asmtest_amd_flags_have(line, NULL) == 0,
          "cpu flag: empty / NULL inputs are rejected");
    int lbr = asmtest_amd_has_cpu_flag("amd_lbr_v2");
    CHECK(lbr == 0 || lbr == 1,
          "cpu flag: cached /proc/cpuinfo probe returns a definite 0/1");
    CHECK(lbr == asmtest_amd_has_cpu_flag("amd_lbr_v2"),
          "cpu flag: cached probe is stable across calls");
#else
    printf("# SKIP AMD cpu flag: not Linux x86-64\n");
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_AMD_LBR);
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
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
    CHECK(asmtest_hwtrace_arm_tid() == -1,
          "arm_tid reads idle (-1) after the single-step scope closes");

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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);

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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
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

/* §D4 scripted-hop coverage — the host-independent native analog of the Node/Java
 * AsyncStitchedTrace tests. Exercises the binding-facing merge (stitch_handles) that
 * those wrappers drive, but from a SCRIPTED set of faked hops instead of a live
 * single-step capture: three pre-decoded hop bodies of ONE logical operation, all on a
 * single (continuation) tid, handed OUT of seq order, must stitch back densely by seq
 * with each hop's bound marking its concatenated start offset. Complements
 * test_stitch_slices (the slice-struct form) and test_stitch_handles (the LIVE,
 * single-step-gated capture): this is the handle form with NO backend dependency. */
static void test_stitch_hops_scripted(void) {
    /* Three faked hop bodies of DISTINCT length so the concatenated offsets are
     * non-uniform (an off-by-one in the merge math cannot pass by coincidence). */
    asmtest_trace_t *hop0 = asmtest_trace_new(16, 16); /* seq 0: 3 insns */
    asmtest_trace_t *hop1 = asmtest_trace_new(16, 16); /* seq 1: 4 insns */
    asmtest_trace_t *hop2 = asmtest_trace_new(16, 16); /* seq 2: 2 insns */
    trace_append_insn(hop0, 0x0);
    trace_append_insn(hop0, 0x5);
    trace_append_insn(hop0, 0xa);
    trace_append_block(hop0, 0);
    trace_append_insn(hop1, 0x0);
    trace_append_insn(hop1, 0x4);
    trace_append_insn(hop1, 0x8);
    trace_append_insn(hop1, 0xc);
    trace_append_block(hop1, 1);
    trace_append_insn(hop2, 0x0);
    trace_append_insn(hop2, 0x6);
    trace_append_block(hop2, 2);

    /* Hand the hops OUT of seq order (hop2, hop0, hop1), all on the continuation tid
     * 909 with one scope_id + one version — three await-continuation hops of a single
     * logical operation, captured out of arrival order. */
    const asmtest_trace_t *traces[3] = {hop2, hop0, hop1};
    const uint64_t scope_ids[3] = {0xA5, 0xA5, 0xA5};
    const uint32_t seqs[3] = {2, 0, 1};
    const int tids[3] = {909, 909, 909};
    const uint64_t versions[3] = {11, 11, 11};

    asmtest_trace_t *merged = asmtest_trace_new(64, 64);
    asmtest_hwtrace_slice_bound_t bounds[3];
    size_t nb = 0;
    int rc = asmtest_hwtrace_stitch_handles(traces, scope_ids, seqs, tids,
                                            versions, 3, merged, bounds, &nb);
    CHECK(rc == ASMTEST_HW_OK && nb == 3,
          "stitch_handles: merges 3 scripted async hops");

    /* Seq order hop0(3) then hop1(4) then hop2(2) = 9 insns, hop bodies back to back. */
    static const uint64_t EXP[] = {0x0, 0x5, 0xa, 0x0, 0x4, 0x8, 0xc, 0x0, 0x6};
    int seq_ok = (asmtest_emu_trace_insns_len(merged) == 9);
    for (size_t i = 0; seq_ok && i < 9; i++)
        seq_ok = (merged->insns[i] == EXP[i]);
    CHECK(seq_ok, "stitch_handles: scripted hops merge densely by seq "
                  "(hop0,hop1,hop2) despite out-of-order input");
    CHECK(bounds[0].insn_off == 0 && bounds[0].seq == 0 &&
              bounds[1].insn_off == 3 && bounds[1].seq == 1 &&
              bounds[2].insn_off == 7 && bounds[2].seq == 2,
          "stitch_handles: per-hop bounds mark each hop's concatenated start "
          "offset (0/3/7)");
    CHECK(
        bounds[0].tid == 909 && bounds[1].tid == 909 && bounds[2].tid == 909,
        "stitch_handles: continuation hops carry the single (tid 909) thread");
    CHECK(bounds[1].scope_id == 0xA5 && bounds[1].version == 11,
          "stitch_handles: per-hop bounds carry (scope_id, version) through");
    CHECK(!asmtest_emu_trace_truncated(merged),
          "stitch_handles: merged scripted trace is not truncated");

    /* NULL scalar arrays: seq defaults to the input index, the rest to 0 (the
     * documented binding default). Feed the same hops in natural (seq) order. */
    const asmtest_trace_t *nat[3] = {hop0, hop1, hop2};
    asmtest_trace_t *merged2 = asmtest_trace_new(64, 64);
    asmtest_hwtrace_slice_bound_t db[3];
    size_t nb2 = 0;
    int rc2 = asmtest_hwtrace_stitch_handles(nat, NULL, NULL, NULL, NULL, 3,
                                             merged2, db, &nb2);
    int nat_ok = (rc2 == ASMTEST_HW_OK && nb2 == 3 &&
                  asmtest_emu_trace_insns_len(merged2) == 9);
    for (size_t i = 0; nat_ok && i < 9; i++)
        nat_ok = (merged2->insns[i] == EXP[i]);
    CHECK(nat_ok, "stitch_handles: NULL arrays default seq to the input index "
                  "(same merged stream)");
    CHECK(db[0].seq == 0 && db[1].seq == 1 && db[2].seq == 2 &&
              db[0].insn_off == 0 && db[1].insn_off == 3 && db[2].insn_off == 7,
          "stitch_handles: index-defaulted seq bounds keep the 0/3/7 offsets");
    CHECK(db[0].scope_id == 0 && db[0].tid == 0 && db[0].version == 0,
          "stitch_handles: NULL scope_id/tid/version arrays default to 0");

    asmtest_trace_free(hop0);
    asmtest_trace_free(hop1);
    asmtest_trace_free(hop2);
    asmtest_trace_free(merged);
    asmtest_trace_free(merged2);
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

/* §Z2 — end-to-end synthetic Intel-PT decode (the trust-gate on the STRONG PT tier).
 * Encode a VALID Intel PT packet stream with libipt's own packet encoder
 * (asmtest_pt_encode_fixture — userspace, NO PT hardware, no intel_pt PMU) for the
 * ROUTINE taken-jle walk, then drive it through the REAL libipt decode bodies
 * (previously never executed end-to-end in CI — every host compiled the ENOSYS stub):
 *   (A) asmtest_pt_decode        — region-scoped (read_region + in-region IP filter),
 *   (B) asmtest_pt_decode_window — whole-window (read_recorder over a self code image,
 *                                  NO in-region filter; the §Z2 path).
 * Both must reconstruct the known-good walk {0x0,0x3,0x6,0xc,0x11} / blocks {0x0,0x11}
 * — the SAME sequence the AMD / CoreSight / DynamoRIO backends produce for these bytes
 * (test_amd_reconstruction, test_cs_reconstruction). FAILS if the decoder regresses.
 * Self-skips cleanly where libipt is absent (ENOSYS stub); the recorder-backed half
 * additionally needs the soft-dirty code-image recorder and self-notes if it is off. */
static void test_wholewindow_decode(void) {
#if defined(__linux__)
    if (!asmtest_pt_decoder_present()) {
        printf(
            "# SKIP wholewindow decode: built without libipt (ENOSYS stub)\n");
        return;
    }

    /* Map ROUTINE executable; its live address is the PT stream's enable IP + the
     * decode's offset origin, so decoded offsets come out relative to it. */
    long ps = sysconf(_SC_PAGESIZE);
    unsigned char *p =
        (unsigned char *)mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP wholewindow decode: mmap failed\n");
        return;
    }
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, (size_t)ps, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);

    /* Synthesize the PT AUX stream for the taken-jle walk at ROUTINE's live address. */
    uint8_t aux[256];
    size_t aux_len = 0;
    int enc = asmtest_pt_encode_fixture(aux, sizeof aux, (uint64_t)(uintptr_t)p,
                                        1, &aux_len);
    CHECK(enc == ASMTEST_HW_OK && aux_len > 0,
          "wholewindow decode: libipt encoded a synthetic PT AUX stream");

    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};

    /* (A) Region-scoped decode: the shipped read_region path (in-region IP filter)
     * over the raw bytes. Needs no soft-dirty, so it runs wherever libipt is built. */
    if (enc == ASMTEST_HW_OK) {
        asmtest_trace_t *rt = asmtest_trace_new(64, 64);
        int rc = asmtest_pt_decode(aux, aux_len, p, sizeof ROUTINE, rt);
        CHECK(rc == ASMTEST_HW_OK,
              "pt_decode (region-scoped): decodes the synthetic stream (real "
              "libipt body, no PT hardware)");
        int seq = (asmtest_emu_trace_insns_total(rt) == 5);
        for (size_t i = 0; seq && i < 5; i++)
            seq = (rt->insns[i] == EXPECT[i]);
        CHECK(seq, "pt_decode (region-scoped): instruction offsets == "
                   "{0,3,6,c,11} (matches the AMD/CoreSight/DR walk)");
        CHECK(asmtest_trace_covered(rt, 0x0) &&
                  asmtest_trace_covered(rt, 0x11) &&
                  asmtest_emu_trace_blocks_len(rt) == 2,
              "pt_decode (region-scoped): block partition == {0, 0x11}");
        CHECK(!asmtest_trace_covered(rt, 0xe),
              "pt_decode (region-scoped): the 0xe dec block is NOT covered — "
              "the taken TNT skipped it (see the not-taken control below)");
        asmtest_trace_free(rt);
    }

    /* (A2) NOT-TAKEN CONTROL — the fixture's discriminating other side.
     * Everything above asserts a walk the fixture could only ever emit ONE of, so on
     * its own it cannot tell a decoder that FOLLOWS the TNT from one whose answer is
     * merely baked in: {0,3,6,c,11} is the only stream the encoder produced. Flip the
     * TNT to NOT-taken and the jle falls through, so the `dec` at 0xe RUNS and the walk
     * becomes {0,3,6,c,e,11} with blocks {0,0xe}. The 0xe `dec` is the discriminator:
     * present here, absent above, from the SAME bytes at the SAME address — the only
     * difference is the one TNT bit. That is the proof the decode is driven by the
     * packet stream. (Mutation-checked: hardcoding the encoder's TNT payload back to 1
     * makes exactly this block fail.) */
    if (enc == ASMTEST_HW_OK) {
        uint8_t naux[256];
        size_t naux_len = 0;
        int nenc = asmtest_pt_encode_fixture(
            naux, sizeof naux, (uint64_t)(uintptr_t)p, 0, &naux_len);
        CHECK(nenc == ASMTEST_HW_OK && naux_len > 0,
              "pt_decode (not-taken control): libipt encoded the NOT-taken TNT "
              "variant");
        if (nenc == ASMTEST_HW_OK) {
            static const uint64_t NEXPECT[] = {0x0, 0x3, 0x6, 0xc, 0xe, 0x11};
            asmtest_trace_t *nt = asmtest_trace_new(64, 64);
            int rc = asmtest_pt_decode(naux, naux_len, p, sizeof ROUTINE, nt);
            CHECK(
                rc == ASMTEST_HW_OK,
                "pt_decode (not-taken control): decodes the NOT-taken stream");
            int seq = (asmtest_emu_trace_insns_total(nt) == 6);
            for (size_t i = 0; seq && i < 6; i++)
                seq = (nt->insns[i] == NEXPECT[i]);
            CHECK(seq,
                  "pt_decode (not-taken control): offsets == "
                  "{0,3,6,c,e,11} — the decode FOLLOWS the TNT bit (the "
                  "0xe dec runs), so the taken walk above is not baked in");
            CHECK(asmtest_trace_covered(nt, 0xe),
                  "pt_decode (not-taken control): the 0xe dec block IS covered "
                  "— the discriminator the taken walk must lack");
            asmtest_trace_free(nt);
        }
    }

    /* (B) Whole-window decode: read_recorder over a self (pid==0) code-image timeline,
     * NO in-region filter — the actual STRONG-tier §Z2 path. Needs the soft-dirty
     * recorder; self-notes (does not fail) where it is unavailable. */
    if (enc == ASMTEST_HW_OK && asmtest_codeimage_available()) {
        asmtest_codeimage_t *img = asmtest_codeimage_new(0);
        int trk = img ? asmtest_codeimage_track(img, p, sizeof ROUTINE) : -1;
        CHECK(trk == ASMTEST_CI_OK,
              "wholewindow decode: ROUTINE tracked in a self code image");
        uint64_t when = img ? asmtest_codeimage_now(img) : 0;
        if (trk == ASMTEST_CI_OK) {
            asmtest_trace_t *tr = asmtest_trace_new(64, 64);
            int rc = asmtest_pt_decode_window(aux, aux_len, img, when, tr);
            CHECK(rc == ASMTEST_HW_OK,
                  "pt_decode_window: decodes the synthetic stream through the "
                  "recorder-backed image (real libipt body)");
            int seq = (asmtest_emu_trace_insns_total(tr) == 5);
            for (size_t i = 0; seq && i < 5; i++)
                seq = (tr->insns[i] == EXPECT[i]);
            CHECK(seq, "pt_decode_window: instruction offsets == {0,3,6,c,11} "
                       "(recorder-served bytes, offset from the first IP)");
            CHECK(asmtest_trace_covered(tr, 0x0) &&
                      asmtest_trace_covered(tr, 0x11) &&
                      asmtest_emu_trace_blocks_len(tr) == 2,
                  "pt_decode_window: block partition == {0, 0x11}");
            CHECK(!asmtest_emu_trace_truncated(tr),
                  "pt_decode_window: complete — no in-region filter truncated "
                  "it");
            CHECK(!asmtest_trace_covered(tr, 0xe),
                  "pt_decode_window: the 0xe dec block is NOT covered (taken "
                  "TNT skipped it)");
            asmtest_trace_free(tr);

            /* The whole-window path's own NOT-TAKEN control: same recorder-backed
             * image, same bytes, one flipped TNT bit -> the 0xe dec must appear.
             * Without this, pt_decode_window's assertions above are one-sided too. */
            uint8_t waux[256];
            size_t waux_len = 0;
            if (asmtest_pt_encode_fixture(waux, sizeof waux,
                                          (uint64_t)(uintptr_t)p, 0,
                                          &waux_len) == ASMTEST_HW_OK) {
                static const uint64_t WNEXPECT[] = {0x0, 0x3, 0x6,
                                                    0xc, 0xe, 0x11};
                asmtest_trace_t *wn = asmtest_trace_new(64, 64);
                int wrc =
                    asmtest_pt_decode_window(waux, waux_len, img, when, wn);
                int wseq = (wrc == ASMTEST_HW_OK &&
                            asmtest_emu_trace_insns_total(wn) == 6);
                for (size_t i = 0; wseq && i < 6; i++)
                    wseq = (wn->insns[i] == WNEXPECT[i]);
                CHECK(wseq && asmtest_trace_covered(wn, 0xe),
                      "pt_decode_window (not-taken control): offsets == "
                      "{0,3,6,c,e,11} and the 0xe dec block IS covered — the "
                      "recorder-backed decode follows the TNT too");
                asmtest_trace_free(wn);
            }
        }
        asmtest_codeimage_free(img);
    } else if (enc == ASMTEST_HW_OK) {
        printf(
            "# NOTE wholewindow decode: soft-dirty recorder unavailable; ran "
            "the region-scoped half only\n");
    }

    munmap(p, (size_t)ps);
#else
    printf("# SKIP wholewindow decode: not Linux\n");
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
            INIT_OPTS(opts, (asmtest_trace_backend_t)ab);
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

/* F27/F36 — the size-negotiated options ABI guard (amd-tracing-followup-plan
 * Phase 1). Host-independent: (a) a caller compiled against an OLDER, SMALLER
 * options struct hands init exactly its legacy-sized allocation and must never
 * be read out of bounds (an ASan build makes this a hard guard, not a hope);
 * (b) a nonzero struct_size too small to even carry `backend` is EINVAL; (c) a
 * pretend-FUTURE caller with a larger struct + struct_size inits fine (the
 * unknown tail is ignored); (d) reject-0: a caller that never set struct_size
 * is a bug (a dropped trailing field), so struct_size==0 is EINVAL — every
 * in-tree caller now self-describes (INIT_OPTS, or an explicit set post-memset). */
static void test_options_abi_guard(void) {
#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP options ABI guard: single-step backend unavailable\n");
        return;
    }
    const int ss_backend = (int)ASMTEST_HWTRACE_SINGLESTEP;

    /* (a) legacy-sized caller: EXACTLY sizeof - sizeof(size_t) bytes (the
     * pre-flag-day 48), self-described. Fields are poked through offsetof so
     * nothing here reads or writes past the short allocation either. */
    const size_t legacy = sizeof(asmtest_hwtrace_options_t) - sizeof(size_t);
    unsigned char *lo = (unsigned char *)calloc(1, legacy);
    if (lo == NULL) {
        printf("# SKIP options ABI guard: calloc failed\n");
        return;
    }
    memcpy(lo + offsetof(asmtest_hwtrace_options_t, struct_size), &legacy,
           sizeof legacy);
    memcpy(lo + offsetof(asmtest_hwtrace_options_t, backend), &ss_backend,
           sizeof ss_backend);
    int rc = asmtest_hwtrace_init((const asmtest_hwtrace_options_t *)lo);
    CHECK(rc == ASMTEST_HW_OK,
          "ABI guard: a legacy-sized (smaller) options struct inits OK with "
          "no out-of-bounds read");
    asmtest_hwtrace_shutdown();
    free(lo);

    /* (b) a struct_size that cannot reach `backend` is not a self-description. */
    asmtest_hwtrace_options_t bad;
    INIT_OPTS(bad, ASMTEST_HWTRACE_SINGLESTEP);
    bad.struct_size = sizeof(size_t); /* covers only the negotiator itself */
    CHECK(asmtest_hwtrace_init(&bad) == ASMTEST_HW_EINVAL,
          "ABI guard: struct_size too small to reach `backend` is EINVAL");

    /* (c) pretend-FUTURE caller: larger allocation + struct_size, junk tail —
     * the library copies only its own sizeof and ignores the tail. */
    const size_t future = sizeof(asmtest_hwtrace_options_t) + 64;
    unsigned char *fo = (unsigned char *)calloc(1, future);
    if (fo != NULL) {
        memcpy(fo + offsetof(asmtest_hwtrace_options_t, struct_size), &future,
               sizeof future);
        memcpy(fo + offsetof(asmtest_hwtrace_options_t, backend), &ss_backend,
               sizeof ss_backend);
        memset(fo + sizeof(asmtest_hwtrace_options_t), 0xa5, 64);
        rc = asmtest_hwtrace_init((const asmtest_hwtrace_options_t *)fo);
        CHECK(rc == ASMTEST_HW_OK,
              "ABI guard: a future (larger) struct_size inits OK, unknown "
              "tail ignored");
        asmtest_hwtrace_shutdown();
        free(fo);
    }

    /* (d) reject-0: a zero-filled caller that never set struct_size is not
     * self-describing (a set trailing field would be silently dropped), so init
     * refuses it with EINVAL rather than guessing a legacy size. */
    asmtest_hwtrace_options_t zf;
    memset(&zf, 0, sizeof zf);
    zf.backend = ASMTEST_HWTRACE_SINGLESTEP;
    CHECK(asmtest_hwtrace_init(&zf) == ASMTEST_HW_EINVAL,
          "ABI guard: struct_size==0 is EINVAL (caller must self-describe)");
#else
    printf("# SKIP options ABI guard: single-step tier not on this arch/OS\n");
#endif
}

/* F29 — the machine-readable EPERM-vs-EUNAVAIL status surface. The
 * host-independent invariants lock status() to available()/skip_reason() (one
 * classifier, no drift); the LIVE lane then asserts the very distinction the
 * surface exists for: on an AMD host whose branch-record probe REACHED the
 * perf open (so decoder+vendor gates passed) while unprivileged perf is
 * blocked kernel-wide (paranoid > 2, non-root — e.g. the Zen 5 dev box at
 * paranoid=4), the verdict must be EPERM (substrate present, permission
 * denied), never the EUNAVAIL a missing-silicon host reports. */
static void test_status_surface(void) {
    static const asmtest_trace_backend_t all[] = {
        ASMTEST_HWTRACE_INTEL_PT, ASMTEST_HWTRACE_CORESIGHT,
        ASMTEST_HWTRACE_AMD_LBR, ASMTEST_HWTRACE_SINGLESTEP};
    CHECK(asmtest_hwtrace_status(ASMTEST_HWTRACE_INTEL_PT, NULL) ==
              ASMTEST_HW_EINVAL,
          "status: NULL out is EINVAL");

    const int paranoid = asmtest_hwtrace_perf_event_paranoid();
    int inv_avail = 1, inv_code = 1, inv_reason = 1, inv_paranoid = 1,
        inv_errno = 1;
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        asmtest_hwtrace_status_t st;
        if (asmtest_hwtrace_status(all[i], &st) != ASMTEST_HW_OK) {
            inv_avail = 0;
            continue;
        }
        char why[160];
        asmtest_hwtrace_skip_reason(all[i], why, sizeof why);
        printf("# status[%d]: available=%d code=%d stage=%d errno=%d "
               "reason=\"%s\"\n",
               (int)all[i], st.available, st.code, st.stage, st.probe_errno,
               st.reason);
        inv_avail &=
            (st.available == (asmtest_hwtrace_available(all[i]) ? 1 : 0));
        inv_code &= ((st.code == ASMTEST_HW_OK) == (st.available == 1));
        inv_code &=
            (st.code == ASMTEST_HW_OK || st.code == ASMTEST_HW_EUNAVAIL ||
             st.code == ASMTEST_HW_EPERM);
        inv_reason &= (strcmp(st.reason, why) == 0);
        inv_paranoid &= (st.perf_event_paranoid == paranoid);
        inv_errno &=
            ((st.stage == ASMTEST_HW_STAGE_PROBE) == (st.probe_errno != 0));
#if defined(__linux__)
        if (st.code == ASMTEST_HW_EPERM)
            inv_errno &= (st.probe_errno == EACCES || st.probe_errno == EPERM);
#endif
    }
    CHECK(inv_avail, "status: available mirrors asmtest_hwtrace_available for "
                     "all four backends");
    CHECK(inv_code,
          "status: code is OK exactly when available (else EUNAVAIL/EPERM)");
    CHECK(inv_reason,
          "status: reason is byte-identical to skip_reason (one classifier)");
    CHECK(inv_paranoid,
          "status: perf_event_paranoid matches the standalone reader");
    CHECK(inv_errno, "status: probe_errno set exactly at the probe stage "
                     "(EPERM implies a permission errno)");

#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))
    asmtest_hwtrace_status_t ss;
    CHECK(asmtest_hwtrace_status(ASMTEST_HWTRACE_SINGLESTEP, &ss) ==
                  ASMTEST_HW_OK &&
              ss.code == ASMTEST_HW_OK && ss.stage == ASMTEST_HW_STAGE_OK,
          "status: single-step is OK on x86-64 (no PMU/perf/privilege gate)");
#endif

#if defined(__linux__)
    CHECK(paranoid != INT_MIN, "status: perf_event_paranoid readable on Linux");
    asmtest_hwtrace_status_t amd;
    memset(&amd, 0, sizeof amd);
    if (asmtest_hwtrace_status(ASMTEST_HWTRACE_AMD_LBR, &amd) ==
            ASMTEST_HW_OK &&
        amd.stage == ASMTEST_HW_STAGE_PROBE && paranoid > 2 && geteuid() != 0) {
        CHECK(amd.code == ASMTEST_HW_EPERM,
              "status LIVE: paranoid-blocked AMD branch probe is EPERM "
              "(permission), never EUNAVAIL (missing silicon)");
        CHECK(amd.available == 0 &&
                  !asmtest_hwtrace_available(ASMTEST_HWTRACE_AMD_LBR),
              "status LIVE: available() still collapses EPERM to 0 (its 0/1 "
              "ABI is unchanged)");
        CHECK(amd.probe_errno == EACCES || amd.probe_errno == EPERM,
              "status LIVE: the blocking errno is threaded out of the probe");
    } else {
        printf("# SKIP status live-EPERM lane: needs an AMD probe reaching "
               "the perf open under paranoid>2 without root (stage=%d "
               "paranoid=%d root=%d)\n",
               amd.stage, paranoid, geteuid() == 0);
    }
#endif
}

/* F22/F26/F37 — the escalation-rung/mechanism discriminator, motivated by the
 * 2026-07-12 Zen-2 IBS review: resolve() rows carry the concrete capture
 * mechanism each row drives; asmtest_trace_call_auto's *used reports the rung
 * that actually WON (in-process TF step vs fork-isolated block-step vs
 * per-instruction re-run vs the AMD branch-record/MSR rungs); and no exact
 * producer ever reports STATISTICAL — a statistical (IBS/sampled survey)
 * result can never be mistaken for an exact one. */
static void test_mechanism_discriminator(void) {
    /* (a) static cascade rows map tier/backend -> mechanism, all EXACT. */
    asmtest_trace_choice_t rows[8];
    size_t n = asmtest_trace_resolve(ASMTEST_TRACE_BEST, rows, 8);
    int ok_map = 1, ok_exact = 1;
    for (size_t i = 0; i < n; i++) {
        asmtest_trace_mechanism_t want = ASMTEST_TRACE_MECH_NONE;
        switch (rows[i].tier) {
        case ASMTEST_TIER_HWTRACE:
            want = (rows[i].backend == ASMTEST_HWTRACE_SINGLESTEP)
                       ? ASMTEST_TRACE_MECH_TF_STEP
                       : ASMTEST_TRACE_MECH_HW_BRANCH;
            break;
        case ASMTEST_TIER_DYNAMORIO:
            want = ASMTEST_TRACE_MECH_DBI;
            break;
        case ASMTEST_TIER_EMULATOR:
            want = ASMTEST_TRACE_MECH_EMULATOR;
            break;
        }
        ok_map &= (rows[i].mechanism == want);
        ok_exact &= (rows[i].mechanism != ASMTEST_TRACE_MECH_STATISTICAL &&
                     rows[i].fidelity != ASMTEST_FIDELITY_STATISTICAL);
    }
    CHECK(n >= 1, "mechanism: the cross-tier cascade is non-empty");
    CHECK(ok_map,
          "mechanism: every resolved row carries its tier's capture mechanism");
    CHECK(ok_exact,
          "mechanism: no exact cascade row is STATISTICAL (fidelity or rung)");

#if defined(__linux__) && defined(__x86_64__)
    /* (b) call_auto stamps the WINNING rung into *used — the F22 triage the
     * cascade exists to support. *used is poisoned first to prove the stamp. */
    void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP mechanism call_auto: mmap failed\n");
        return;
    }
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);
    asmtest_trace_t *t = asmtest_trace_new(512, 64);
    long args[2] = {20, 22}, result = 0;
    asmtest_trace_choice_t used;
    memset(&used, 0xff, sizeof used); /* poison: call_auto must write it */
    int rc = asmtest_trace_call_auto(p, sizeof ROUTINE, args, 2,
                                     ASMTEST_TRACE_BEST, &result, t, &used);
    if (rc == ASMTEST_HW_OK) {
        printf("# mechanism: call_auto won via mechanism=%d (tier=%d "
               "backend=%d fidelity=%d)\n",
               (int)used.mechanism, (int)used.tier, (int)used.backend,
               (int)used.fidelity);
        CHECK(used.mechanism == ASMTEST_TRACE_MECH_HW_BRANCH ||
                  used.mechanism == ASMTEST_TRACE_MECH_TF_STEP ||
                  used.mechanism == ASMTEST_TRACE_MECH_MSR_LBR ||
                  used.mechanism == ASMTEST_TRACE_MECH_BLOCKSTEP ||
                  used.mechanism == ASMTEST_TRACE_MECH_PER_INSN,
              "mechanism: call_auto reports a concrete winning rung, never "
              "NONE/stale");
        CHECK(used.mechanism != ASMTEST_TRACE_MECH_STATISTICAL &&
                  used.fidelity == ASMTEST_FIDELITY_NATIVE,
              "mechanism: the exact call-owning ladder never reports "
              "STATISTICAL");
        if (used.tier == ASMTEST_TIER_HWTRACE &&
            used.backend == ASMTEST_HWTRACE_SINGLESTEP)
            CHECK(used.mechanism == ASMTEST_TRACE_MECH_TF_STEP ||
                      used.mechanism == ASMTEST_TRACE_MECH_BLOCKSTEP ||
                      used.mechanism == ASMTEST_TRACE_MECH_PER_INSN,
                  "mechanism: the three SINGLESTEP-reporting rungs no longer "
                  "collapse (F22)");
    } else {
        CHECK(rc == ASMTEST_HW_EUNAVAIL,
              "mechanism: call_auto returns OK or EUNAVAIL");
        CHECK(used.mechanism == ASMTEST_TRACE_MECH_NONE,
              "mechanism: EUNAVAIL clears *used to MECH_NONE (no stale rung)");
    }
    asmtest_trace_free(t);
    munmap(p, sizeof ROUTINE);
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
#if defined(__linux__) && defined(__x86_64__)
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

    /* --- GUARD: the same-target-conditional ambiguity (amd-tracing-plan
     * "Same-target-conditional ambiguity -> truncated"). Two direct conditionals to the
     * SAME label — the `||` / dual-guard shape every bounds check and null check
     * compiles to — with the first NOT taken and the second taken. BTF gives no signal
     * for WHICH jumped, so the greedy "first Jcc whose target == next-stop" rule ends
     * the block at the FIRST one and silently drops the instructions between them. The
     * plan's prescription is to detect the >1-candidate case and set truncated rather
     * than guess. Single-step is the oracle that those instructions really ran. --- */
    static const unsigned char GUARD_X86[] = {
        0x48, 0x85, 0xff,                         /*  0 test rdi,rdi   */
        0x74, 0x0c,                               /*  3 je  17         */
        0x48, 0x85, 0xf6,                         /*  5 test rsi,rsi   */
        0x74, 0x07,                               /*  8 je  17         */
        0x48, 0x89, 0xf8,                         /* 10 mov rax,rdi    */
        0x48, 0x01, 0xf0,                         /* 13 add rax,rsi    */
        0xc3,                                     /* 16 ret            */
        0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, /* 17 mov rax,-1    */
        0xc3,                                     /* 24 ret            */
    };
    void *g = mmap(NULL, sizeof GUARD_X86, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g == MAP_FAILED)
        return;
    memcpy(g, GUARD_X86, sizeof GUARD_X86);
    mprotect(g, sizeof GUARD_X86, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)g, (char *)g + sizeof GUARD_X86);

    /* rdi=5 (first je NOT taken), rsi=0 (second je TAKEN). */
    long gargs[2] = {5, 0}, gsres = 0, gbres = 0;
    asmtest_trace_t *gs = asmtest_trace_new(64, 64);
    asmtest_trace_t *gb = asmtest_trace_new(64, 64);
    int grs =
        asmtest_ptrace_trace_call(g, sizeof GUARD_X86, gargs, 2, &gsres, gs);
    int grb = asmtest_ptrace_trace_call_blockstep(g, sizeof GUARD_X86, gargs, 2,
                                                  &gbres, gb);
    static const uint64_t G_EXP[6] = {0, 3, 5, 8, 17, 24};
    int gseq =
        (grs == ASMTEST_PTRACE_OK && asmtest_emu_trace_insns_total(gs) == 6);
    for (int i = 0; gseq && i < 6; i++)
        gseq = (gs->insns[i] == G_EXP[i]);
    CHECK(gseq && !asmtest_emu_trace_truncated(gs),
          "block-step guard oracle: single-step proves 0,3,5,8,17,24 all ran "
          "(both same-target je's execute)");
    CHECK(
        gsres == -1 && gbres == -1,
        "block-step guard: both drivers run the frame identically (returns -1 "
        "via the second je)");
    /* The block-step stream may legitimately be SHORT here — what it must never be is
     * short AND reported complete. That is the whole rule. */
    CHECK(grb == ASMTEST_PTRACE_OK && asmtest_emu_trace_truncated(gb),
          "block-step guard: two conditionals sharing a target make the block "
          "AMBIGUOUS -> truncated, never a silently short stream called "
          "complete");
    int gsub = 1, gi = 0; /* block-step must never invent an address */
    for (unsigned long long i = 0; gsub && i < asmtest_emu_trace_insns_len(gb);
         i++) {
        while (gi < 6 && G_EXP[gi] != gb->insns[i])
            gi++;
        gsub = (gi < 6);
        gi++;
    }
    CHECK(gsub, "block-step guard: every reconstructed address is one that "
                "single-step saw execute, in order (an ordered subsequence)");
    asmtest_trace_free(gs);
    asmtest_trace_free(gb);
    munmap(g, sizeof GUARD_X86);

    /* --- int3 (T1): the tracee's OWN int3 must never fabricate never-executed
     * instructions. push rbx; mov rbx,rdi; int3; mov rax,rbx; add rax,rax; pop
     * rbx; ret. Pre-fix the block-step driver mis-read the app int3 as a BTF #DB
     * and emitted +0+1+4+5+8+11+12+5+8+11+12 with truncated=0. --- */
    static const unsigned char INT3_X86[] = {0x53, 0x48, 0x89, 0xfb, 0xCC,
                                             0x48, 0x89, 0xd8, 0x48, 0x01,
                                             0xc0, 0x5b, 0xc3};
    void *ip = mmap(NULL, sizeof INT3_X86, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ip != MAP_FAILED) {
        memcpy(ip, INT3_X86, sizeof INT3_X86);
        mprotect(ip, sizeof INT3_X86, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)ip, (char *)ip + sizeof INT3_X86);
        asmtest_trace_t *ib = asmtest_trace_new(64, 64);
        long iargs[2] = {7, 0}, ires = 0;
        int irc = asmtest_ptrace_trace_call_blockstep(ip, sizeof INT3_X86,
                                                      iargs, 2, &ires, ib);
        static const uint64_t I_EXP[] = {0x0, 0x1, 0x4};
        int iseq = (irc == ASMTEST_PTRACE_OK &&
                    asmtest_emu_trace_insns_total(ib) == 3);
        for (int i = 0; iseq && i < 3; i++)
            iseq = (ib->insns[i] == I_EXP[i]);
        CHECK(iseq, "block-step int3: records exactly +0 +1 +4 through the app "
                    "int3, nothing after");
        CHECK(
            asmtest_emu_trace_truncated(ib),
            "block-step int3: an application int3 truncates (BTF cannot bridge "
            "into the handler)");
        (void)ires;
        asmtest_trace_free(ib);
        munmap(ip, sizeof INT3_X86);
    }
#else
    printf("# SKIP ptrace block-step: Linux x86-64 only (no AArch64 "
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
#if defined(__linux__) && defined(__x86_64__)
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
    printf("# SKIP ptrace windowed: Linux x86-64 only\n");
#endif
}

/* ------------------------------------------------------------------ */
/* §D3 W-1 — the BTF block-step DRIVER for the whole-window capture.    */
/* ------------------------------------------------------------------ */
#if defined(__linux__) && defined(__x86_64__)

/* Mid-window publisher, called BY the tracee from inside the window (see WDRV below).
 * Its globals are set before the fork, so the tracee inherits them. It is glue — outside
 * every recorded region — so it is stepped through and not recorded; the point is that
 * the store it makes to the SHARED channel must be drained by the stepper at one of the
 * stops that follow, before the window's code first calls the region it publishes. */
static asmtest_addr_channel_t *g_wpub_chan;
static uint64_t g_wpub_base, g_wpub_len;
static int g_wpub_enable;
__attribute__((noinline)) static void wpub_publish(void) {
    if (g_wpub_enable)
        asmtest_addr_channel_publish(g_wpub_chan, g_wpub_base, g_wpub_len, 0);
}

/* WDRV — the window frame. A T-trip loop that calls two leaf "methods" through
 * absolute indirect calls, preceded by one call out to wpub_publish:
 *
 *   push rbx; push r12; mov rbx,rdi; xor r12,r12
 *   movabs rax, wpub_publish; call rax                 ; publish M2 MID-window
 * L: movabs rax,m1; mov rdi,r12; mov esi,1;  call rax  ; r12 = m1(r12, 1)
 *   mov r12,rax
 *   movabs rax,m2; mov rdi,r12; mov esi,3;  call rax   ; r12 = m2(r12, 3)
 *   mov r12,rax; dec rbx; jnz L
 *   mov rax,r12; pop r12; pop rbx; ret
 *
 * Chosen so the block-step and per-instruction drivers have plenty to disagree about:
 * a hot back-edge, two call/ret pairs per trip into SEPARATE mappings, straight-line
 * runs inside the leaves (the instructions block-step must RECONSTRUCT rather than
 * observe), a not-taken conditional on the last trip, and a call out to glue. */
#define WDRV_LEN  79u
#define WDRV_LOFF 21u /* the loop head, and the publish call's return address */
static void wdrv_build(unsigned char *d, uint64_t pub, uint64_t m1,
                       uint64_t m2) {
    static const unsigned char T[WDRV_LEN] = {
        0x53,                                        /*  0 push rbx      */
        0x41, 0x54,                                  /*  1 push r12      */
        0x48, 0x89, 0xfb,                            /*  3 mov rbx,rdi   */
        0x4d, 0x31, 0xe4,                            /*  6 xor r12,r12   */
        0x48, 0xb8, 0,    0,    0,    0, 0, 0, 0, 0, /*  9 movabs rax,pub*/
        0xff, 0xd0,                                  /* 19 call rax      */
        0x48, 0xb8, 0,    0,    0,    0, 0, 0, 0, 0, /* 21 movabs rax,m1 */
        0x4c, 0x89, 0xe7,                            /* 31 mov rdi,r12   */
        0xbe, 0x01, 0x00, 0x00, 0x00,                /* 34 mov esi,1     */
        0xff, 0xd0,                                  /* 39 call rax      */
        0x49, 0x89, 0xc4,                            /* 41 mov r12,rax   */
        0x48, 0xb8, 0,    0,    0,    0, 0, 0, 0, 0, /* 44 movabs rax,m2 */
        0x4c, 0x89, 0xe7,                            /* 54 mov rdi,r12   */
        0xbe, 0x03, 0x00, 0x00, 0x00,                /* 57 mov esi,3     */
        0xff, 0xd0,                                  /* 62 call rax      */
        0x49, 0x89, 0xc4,                            /* 64 mov r12,rax   */
        0x48, 0xff, 0xcb,                            /* 67 dec rbx       */
        0x75, 0xcd,                                  /* 70 jnz L (21-72) */
        0x4c, 0x89, 0xe0,                            /* 72 mov rax,r12   */
        0x41, 0x5c,                                  /* 75 pop r12       */
        0x5b,                                        /* 77 pop rbx       */
        0xc3,                                        /* 78 ret           */
    };
    memcpy(d, T, WDRV_LEN);
    memcpy(d + 11, &pub, 8);
    memcpy(d + 23, &m1, 8);
    memcpy(d + 46, &m2, 8);
}

/* The tracee's ptrace-stop count, read straight from the kernel — every ptrace-stop
 * parks the tracee in TASK_TRACED, which is one VOLUNTARY context switch. This is the
 * stop-count measurement: an OS-owned counter neither driver can influence except by
 * actually stopping the tracee fewer times. 0 if unreadable. */
static unsigned long read_vcsw(pid_t pid) {
    char path[64], line[256];
    snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;
    unsigned long v = 0;
    while (fgets(line, sizeof line, f) != NULL)
        if (strncmp(line, "voluntary_ctxt_switches:", 24) == 0) {
            v = strtoul(line + 24, NULL, 10);
            break;
        }
    fclose(f);
    return v;
}

/* Run WDRV once under `blockstep` (1) or the per-instruction windowed entry (0),
 * publishing M1 before the window and (when g_wpub_enable) M2 from inside it. Returns
 * the rc; *stops_out gets the tracee's ptrace-stop count for the window. */
static int wdrv_run(int blockstep, const void *drv, uint64_t a1, size_t m1len,
                    asmtest_addr_channel_t *chan, long trips, long *res_out,
                    unsigned long *stops_out, asmtest_trace_t *tr) {
    asmtest_addr_channel_init(chan);
    pid_t pid = fork();
    if (pid < 0)
        return ASMTEST_PTRACE_ETRACE;
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        /* M1 is the pre-arm rundown case: published before the window opens. M2 is
         * published MID-window, by wpub_publish, from inside the frame. */
        asmtest_addr_channel_publish(chan, a1, m1len, 0);
        raise(SIGSTOP);
        ((void (*)(long))drv)(trips);
        _exit(0);
    }
    int st = 0, rc = ASMTEST_PTRACE_ETRACE;
    if (waitpid(pid, &st, 0) >= 0 && WIFSTOPPED(st) &&
        asmtest_ptrace_run_to(pid, drv) == ASMTEST_PTRACE_OK) {
        unsigned long v0 = read_vcsw(pid);
        rc = blockstep ? asmtest_ptrace_trace_attached_windowed_blockstep(
                             pid, drv, WDRV_LEN, chan, res_out, tr)
                       : asmtest_ptrace_trace_attached_windowed(
                             pid, drv, WDRV_LEN, chan, res_out, tr);
        *stops_out = read_vcsw(pid) - v0;
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return rc;
}

/* WSIG — a window frame that FAULTS mid-block, for the signal leg. BTF cannot see a
 * kernel-injected control transfer, so signal delivery cuts a block the block-stepper is
 * in the middle of; the driver must reconstruct the cut block and finish on the
 * per-instruction loop rather than truncate (a managed runtime raises and handles
 * SIGSEGV as normal operation).
 *
 *   push rbx; mov rbx,rdi; xor rax,rax; mov rax,[rax]  <- SIGSEGV, mid-block
 *   add rax,rbx; pop rbx; ret                          <- resumed by the handler
 *
 * wsig_handler skips the faulting load and zeroes rax, so the frame returns its
 * argument. The handler itself is glue: outside the frame, so never recorded. */
#define WSIG_LEN 15u
static const unsigned char WSIG[WSIG_LEN] = {
    0x53,             /*  0 push rbx        */
    0x48, 0x89, 0xfb, /*  1 mov rbx,rdi     */
    0x48, 0x31, 0xc0, /*  4 xor rax,rax     */
    0x48, 0x8b, 0x00, /*  7 mov rax,[rax]  <- faults */
    0x48, 0x01, 0xd8, /* 10 add rax,rbx     */
    0x5b,             /* 13 pop rbx         */
    0xc3,             /* 14 ret             */
};
__attribute__((noinline)) static void wsig_handler(int sig, siginfo_t *si,
                                                   void *uc) {
    (void)sig;
    (void)si;
    ucontext_t *c = (ucontext_t *)uc;
    c->uc_mcontext.gregs[REG_RIP] += 3; /* step over the 3-byte faulting load */
    c->uc_mcontext.gregs[REG_RAX] = 0;
}

/* Run WSIG once under `blockstep` (1) or per-instruction (0). */
static int wsig_run(int blockstep, const void *frame, long arg, long *res_out,
                    asmtest_trace_t *tr) {
    pid_t pid = fork();
    if (pid < 0)
        return ASMTEST_PTRACE_ETRACE;
    if (pid == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = wsig_handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, NULL);
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        ((void (*)(long))frame)(arg);
        _exit(0);
    }
    int st = 0, rc = ASMTEST_PTRACE_ETRACE;
    if (waitpid(pid, &st, 0) >= 0 && WIFSTOPPED(st) &&
        asmtest_ptrace_run_to(pid, frame) == ASMTEST_PTRACE_OK)
        rc = blockstep ? asmtest_ptrace_trace_attached_windowed_blockstep(
                             pid, frame, WSIG_LEN, NULL, res_out, tr)
                       : asmtest_ptrace_trace_attached_windowed(
                             pid, frame, WSIG_LEN, NULL, res_out, tr);
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return rc;
}

/* WCUT — a window frame that never returns: it calls out to wcut_die, which _exit()s.
 * The tracee therefore dies WHILE the driver is inside its own SINGLEBLOCK/waitpid, with
 * a partial stream already recorded — the only way to reach the driver's "the window
 * never hit win_ret" guard. (Killing the tracee BEFORE the call instead kills it one
 * layer earlier: the driver's first read_pc_ret gets ESRCH and returns ETRACE from the
 * prologue, having never recorded, stopped, or evaluated reached_end.)
 *
 *   push rbx; mov rbx,rdi; add rbx,rbx; movabs rax,wcut_die; call rax
 *   mov rax,rbx; pop rbx; ret        <- never reached
 */
#define WCUT_LEN 24u
static const unsigned char WCUT[WCUT_LEN] = {
    0x53,                                  /*  0 push rbx           */
    0x48, 0x89, 0xfb,                      /*  1 mov rbx,rdi        */
    0x48, 0x01, 0xdb,                      /*  4 add rbx,rbx        */
    0x48, 0xb8, 0,    0, 0, 0, 0, 0, 0, 0, /*  7 movabs rax,wcut_die*/
    0xff, 0xd0,                            /* 17 call rax           */
    0x48, 0x89, 0xd8,                      /* 19 mov rax,rbx        */
    0x5b,                                  /* 22 pop rbx            */
    0xc3,                                  /* 23 ret                */
};
__attribute__((noinline)) static void wcut_die(void) { _exit(0); }

/* WVANISH — a window frame that exit_group()s with NO taken branch first:
 *
 *   mov eax,231 (__NR_exit_group); xor edi,edi; syscall
 *
 * `syscall` is not a branch, an interrupt or an exception, so BTF raises no #DB: the
 * driver's FIRST PTRACE_SINGLEBLOCK returns WIFEXITED without one block having closed —
 * the window vanished having recorded nothing.
 *
 * This shape exists because it is the ONLY way to falsify the driver's "never reached
 * win_ret" guard. Every path to that guard leaves the tracee dead, and whenever the
 * stream is NON-empty the materializer independently sets truncated (its foreign reads
 * of the dead tracee's memory all fail). So the guard's one observable effect is exactly
 * this: an EMPTY stream from a vanished window must be reported truncated, not as a
 * clean complete capture of nothing. A frame that merely dies mid-window (WCUT above)
 * does NOT exercise it — it passes on the materializer's behaviour instead. */
#define WVANISH_LEN 10u
static const unsigned char WVANISH[WVANISH_LEN] = {
    0xb8, 0xe7, 0x00, 0x00, 0x00, /* 0 mov eax,231 (exit_group) */
    0x31, 0xff,                   /* 5 xor edi,edi              */
    0x0f, 0x05,                   /* 7 syscall                  */
    0xc3,                         /* 9 ret (never reached)      */
};

/* Run a two-argument window frame (rdi=5, rsi=0) once under `blockstep` or the
 * per-instruction windowed entry. Used by the same-target-conditional guard leg. */
static int wguard_run(int blockstep, const void *frame, size_t flen,
                      long *res_out, asmtest_trace_t *tr) {
    pid_t pid = fork();
    if (pid < 0)
        return ASMTEST_PTRACE_ETRACE;
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        ((void (*)(long, long))frame)(5, 0);
        _exit(0);
    }
    int st = 0, rc = ASMTEST_PTRACE_ETRACE;
    if (waitpid(pid, &st, 0) >= 0 && WIFSTOPPED(st) &&
        asmtest_ptrace_run_to(pid, frame) == ASMTEST_PTRACE_OK)
        rc = blockstep ? asmtest_ptrace_trace_attached_windowed_blockstep(
                             pid, frame, flen, NULL, res_out, tr)
                       : asmtest_ptrace_trace_attached_windowed(
                             pid, frame, flen, NULL, res_out, tr);
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return rc;
}

/* Compare two captures of the same window instruction-for-instruction. */
static int wdrv_streams_identical(const asmtest_trace_t *a,
                                  const asmtest_trace_t *b) {
    unsigned long long na = asmtest_emu_trace_insns_len(a);
    if (na == 0 || na != asmtest_emu_trace_insns_len(b))
        return 0;
    for (unsigned long long i = 0; i < na; i++)
        if (a->insns[i] != b->insns[i])
            return 0;
    unsigned long long ba = asmtest_emu_trace_blocks_len(a);
    if (ba != asmtest_emu_trace_blocks_len(b))
        return 0;
    for (unsigned long long i = 0; i < ba; i++)
        if (a->blocks[i] != b->blocks[i])
            return 0;
    return 1;
}
#endif /* __linux__ && __x86_64__ */

/* §D3 W-1: the PTRACE_SINGLEBLOCK driver for the WHOLE-WINDOW capture. The per-
 * instruction windowed path is exact, so this is verified as a DIFFERENTIAL ORACLE
 * against it: run the identical window both ways and require byte-identical absolute-
 * address streams, block partitions, return values and truncated flags. "The block-step
 * path works" would be far weaker — the oracle can only pass if block-step reconstructs
 * every instruction it never stopped on, in order.
 *
 * Legs:
 *   1. Both leaves recorded (M1 pre-published, M2 published MID-window from inside the
 *      frame): streams identical, and the measured stop count drops ~6x.
 *   2. M2 NOT published: M2 still EXECUTES but must not be recorded, so this exercises
 *      the "block starts outside every region" glue path in the reconstructor — and is
 *      the negative control for leg 1's mid-window-pickup assert.
 *   3. chan == NULL: only the frame is recorded, streams still identical.
 *   4. A window cut short (the tracee dies inside it) must set truncated, not report a
 *      partial stream as complete.
 * The MUTATION control (that this oracle can actually fail) is asserted directly: a
 * one-instruction edit to a copy of leg 1's stream must be caught by the comparator. */
static void test_ptrace_windowed_blockstep(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_ptrace_blockstep_available()) {
        printf("# SKIP windowed block-step: no functional PTRACE_SINGLEBLOCK "
               "(DEBUGCTL.BTF masked) or no Capstone\n");
        return;
    }

    /* Two "JIT'd methods" at separate mappings, each an 8-instruction straight-line run
     * plus a ret — the instructions block-step must RECONSTRUCT, never having stopped
     * on them. */
    static const unsigned char M1[] = {
        0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0x48, 0x01, 0xf0, 0x48,
        0x01, 0xf0, 0x48, 0x01, 0xf0, 0x48, 0x01, 0xf0, 0x48, 0x01,
        0xf0, 0x48, 0x01, 0xf0, 0x48, 0x01, 0xf0, 0xc3};
    static const unsigned char M2[] = {
        0x48, 0x89, 0xf8, 0x48, 0x29, 0xf0, 0x48, 0x29, 0xf0, 0x48,
        0x29, 0xf0, 0x48, 0x29, 0xf0, 0x48, 0x29, 0xf0, 0x48, 0x29,
        0xf0, 0x48, 0x29, 0xf0, 0x48, 0x29, 0xf0, 0xc3};
    void *m1 = mmap(NULL, sizeof M1, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *m2 = mmap(NULL, sizeof M2, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void *drv = mmap(NULL, WDRV_LEN, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    asmtest_addr_channel_t *chan =
        mmap(NULL, sizeof *chan, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (m1 == MAP_FAILED || m2 == MAP_FAILED || drv == MAP_FAILED ||
        chan == MAP_FAILED) {
        printf("# SKIP windowed block-step: mmap failed\n");
        return;
    }
    memcpy(m1, M1, sizeof M1);
    memcpy(m2, M2, sizeof M2);
    uint64_t a1 = (uint64_t)(uintptr_t)m1, a2 = (uint64_t)(uintptr_t)m2;
    wdrv_build((unsigned char *)drv, (uint64_t)(uintptr_t)&wpub_publish, a1,
               a2);
    mprotect(m1, sizeof M1, PROT_READ | PROT_EXEC);
    mprotect(m2, sizeof M2, PROT_READ | PROT_EXEC);
    mprotect(drv, WDRV_LEN, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)m1, (char *)m1 + sizeof M1);
    __builtin___clear_cache((char *)m2, (char *)m2 + sizeof M2);
    __builtin___clear_cache((char *)drv, (char *)drv + WDRV_LEN);

    g_wpub_chan = chan;
    g_wpub_base = a2;
    g_wpub_len = sizeof M2;

    const long TRIPS = 200;
    /* 10 + 32*TRIPS recorded instructions when both leaves are published. */
    const unsigned long long EXP_INSNS =
        10ull + 32ull * (unsigned long long)TRIPS;

    /* ---- leg 1: both leaves recorded; M2 published MID-window ---- */
    g_wpub_enable = 1;
    asmtest_trace_t *ss = asmtest_trace_new(8192, 4096);
    asmtest_trace_t *bs = asmtest_trace_new(8192, 4096);
    long sres = 0, bres = 0;
    unsigned long sstops = 0, bstops = 0;
    int rcs = wdrv_run(0, drv, a1, sizeof M1, chan, TRIPS, &sres, &sstops, ss);
    int rcb = wdrv_run(1, drv, a1, sizeof M1, chan, TRIPS, &bres, &bstops, bs);
    CHECK(rcs == ASMTEST_PTRACE_OK && rcb == ASMTEST_PTRACE_OK,
          "windowed block-step: both drivers capture the window");
    /* r12 = ((r12+1*8) - 3*8) per trip = -16 per trip. */
    CHECK(sres == -16 * TRIPS && bres == sres,
          "windowed block-step: same frame return value as single-step");
    CHECK(!asmtest_emu_trace_truncated(ss) && !asmtest_emu_trace_truncated(bs),
          "windowed block-step: both captures are complete (not truncated)");
    CHECK(asmtest_emu_trace_insns_len(ss) == EXP_INSNS,
          "windowed block-step: the per-instruction oracle recorded the whole "
          "window (10 + 32*trips)");
    CHECK(wdrv_streams_identical(ss, bs),
          "windowed block-step: DIFFERENTIAL ORACLE — reconstructed absolute "
          "stream + block partition are IDENTICAL to per-instruction");

    /* The oracle is only meaningful if it can fail: mutate ONE address of a copy of the
     * block-step stream and require the comparator to reject it. */
    asmtest_trace_t *mut = asmtest_trace_new(8192, 4096);
    for (unsigned long long i = 0; i < asmtest_emu_trace_insns_len(bs); i++)
        mut->insns[i] = bs->insns[i];
    for (unsigned long long i = 0; i < asmtest_emu_trace_blocks_len(bs); i++)
        mut->blocks[i] = bs->blocks[i];
    mut->insns_len = bs->insns_len;
    mut->blocks_len = bs->blocks_len;
    CHECK(wdrv_streams_identical(ss, mut),
          "windowed block-step: the mutation control's copy starts identical");
    mut->insns[EXP_INSNS / 2] ^= 0x10; /* one instruction, one bit */
    CHECK(!wdrv_streams_identical(ss, mut),
          "windowed block-step: MUTATION CONTROL — the oracle rejects a stream "
          "that differs by a single instruction address");
    asmtest_trace_free(mut);

    /* Mid-window pickup: M2 was published from INSIDE the window, after the stepper's
     * pre-loop drain, so these instructions can only be here if the block-step loop
     * drains the channel at its stops too. */
    int hit_m2 = 0;
    for (unsigned long long i = 0; i < asmtest_emu_trace_insns_len(bs); i++)
        if (bs->insns[i] >= a2 && bs->insns[i] < a2 + sizeof M2)
            hit_m2 = 1;
    CHECK(hit_m2, "windowed block-step: a region published MID-window (from "
                  "inside the frame) is drained and recorded");

    /* The measurement: stops are counted by the kernel (the tracee's voluntary
     * context switches), not by the code under test. */
    printf("# windowed block-step stops: single-step %lu, block-step %lu "
           "(%.1fx fewer) over %llu recorded instructions\n",
           sstops, bstops, bstops ? (double)sstops / (double)bstops : 0.0,
           asmtest_emu_trace_insns_len(bs));
    CHECK(bstops > 0 && sstops >= bstops * 4,
          "windowed block-step: >=4x fewer ptrace stops than per-instruction "
          "(the plan's 4-10x claim, measured)");

    /* ---- leg 2 (also leg 1's negative control): M2 never published ---- */
    g_wpub_enable = 0;
    asmtest_trace_t *ss2 = asmtest_trace_new(8192, 4096);
    asmtest_trace_t *bs2 = asmtest_trace_new(8192, 4096);
    long sres2 = 0, bres2 = 0;
    unsigned long st2a = 0, st2b = 0;
    int rcs2 = wdrv_run(0, drv, a1, sizeof M1, chan, TRIPS, &sres2, &st2a, ss2);
    int rcb2 = wdrv_run(1, drv, a1, sizeof M1, chan, TRIPS, &bres2, &st2b, bs2);
    int hit_m2_np = 0;
    for (unsigned long long i = 0; i < asmtest_emu_trace_insns_len(bs2); i++)
        if (bs2->insns[i] >= a2 && bs2->insns[i] < a2 + sizeof M2)
            hit_m2_np = 1;
    CHECK(rcs2 == ASMTEST_PTRACE_OK && rcb2 == ASMTEST_PTRACE_OK && !hit_m2_np,
          "windowed block-step: NEGATIVE CONTROL — with M2 unpublished its "
          "(still-executed) instructions are absent, so the pickup above is "
          "real");
    CHECK(asmtest_emu_trace_insns_len(bs2) ==
              10ull + 22ull * (unsigned long long)TRIPS,
          "windowed block-step: unpublished leaf leaves exactly its 10 "
          "insns/trip out");
    CHECK(wdrv_streams_identical(ss2, bs2),
          "windowed block-step: streams identical when a called leaf is glue "
          "(the block-starts-outside-every-region path)");
    CHECK(bres2 == sres2 && !asmtest_emu_trace_truncated(bs2),
          "windowed block-step: unpublished-leaf capture is complete");
    asmtest_trace_free(ss2);
    asmtest_trace_free(bs2);

    /* ---- leg 3: chan == NULL records only the window frame ---- */
    asmtest_trace_t *ss3 = asmtest_trace_new(8192, 4096);
    asmtest_trace_t *bs3 = asmtest_trace_new(8192, 4096);
    long r3a = 0, r3b = 0;
    unsigned long st3a = 0, st3b = 0;
    int rcs3 = wdrv_run(0, drv, a1, sizeof M1, NULL, TRIPS, &r3a, &st3a, ss3);
    int rcb3 = wdrv_run(1, drv, a1, sizeof M1, NULL, TRIPS, &r3b, &st3b, bs3);
    uint64_t dv = (uint64_t)(uintptr_t)drv;
    int only_frame = (rcs3 == ASMTEST_PTRACE_OK && rcb3 == ASMTEST_PTRACE_OK);
    for (unsigned long long i = 0;
         only_frame && i < asmtest_emu_trace_insns_len(bs3); i++)
        only_frame = (bs3->insns[i] >= dv && bs3->insns[i] < dv + WDRV_LEN);
    CHECK(only_frame && asmtest_emu_trace_insns_len(bs3) ==
                            10ull + 12ull * (unsigned long long)TRIPS,
          "windowed block-step: chan==NULL records the frame only (10 + "
          "12*trips), no leaf");
    CHECK(
        wdrv_streams_identical(ss3, bs3),
        "windowed block-step: chan==NULL streams identical to per-instruction");
    asmtest_trace_free(ss3);
    asmtest_trace_free(bs3);

    /* ---- leg 4: a window cut short must be flagged, never reported complete ----
     * The tracee _exit()s from INSIDE the window, so it dies while the driver is in its
     * own SINGLEBLOCK/waitpid with a partial stream already recorded. That is the only
     * route to the driver's "never reached win_ret" guard: the asserts below are an
     * unconditional conjunction (rc == OK AND truncated AND a non-empty partial), so a
     * capture that errored out of the prologue instead cannot satisfy them. */
    {
        void *cf = mmap(NULL, WCUT_LEN, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (cf == MAP_FAILED) {
            printf("# SKIP windowed block-step cut-short leg: mmap failed\n");
        } else {
            unsigned char cut[WCUT_LEN];
            memcpy(cut, WCUT, WCUT_LEN);
            uint64_t die = (uint64_t)(uintptr_t)&wcut_die;
            memcpy(cut + 9, &die, 8);
            memcpy(cf, cut, WCUT_LEN);
            mprotect(cf, WCUT_LEN, PROT_READ | PROT_EXEC);
            __builtin___clear_cache((char *)cf, (char *)cf + WCUT_LEN);

            asmtest_trace_t *tr4 = asmtest_trace_new(8192, 4096);
            pid_t pid = fork();
            if (pid == 0) {
                ptrace(PTRACE_TRACEME, 0, NULL, NULL);
                raise(SIGSTOP);
                ((void (*)(long))cf)(3);
                _exit(0);
            }
            int st = 0, rc4 = ASMTEST_PTRACE_ETRACE;
            if (waitpid(pid, &st, 0) >= 0 && WIFSTOPPED(st) &&
                asmtest_ptrace_run_to(pid, cf) == ASMTEST_PTRACE_OK)
                rc4 = asmtest_ptrace_trace_attached_windowed_blockstep(
                    pid, cf, WCUT_LEN, NULL, NULL, tr4);
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            /* The frame runs 0,1,4,7,17 and then never comes back. */
            uint64_t cv = (uint64_t)(uintptr_t)cf;
            static const uint64_t COFF[5] = {0, 1, 4, 7, 17};
            int partial = (asmtest_emu_trace_insns_len(tr4) == 5);
            for (int i = 0; partial && i < 5; i++)
                partial = (tr4->insns[i] == cv + COFF[i]);
            CHECK(rc4 == ASMTEST_PTRACE_OK && partial,
                  "windowed block-step: a tracee that dies INSIDE the window "
                  "still yields its partial stream (0,1,4,7,17)");
            CHECK(
                rc4 == ASMTEST_PTRACE_OK && asmtest_emu_trace_truncated(tr4),
                "windowed block-step: that partial is flagged TRUNCATED — a "
                "window that never reached win_ret is never reported complete");
            asmtest_trace_free(tr4);
            munmap(cf, WCUT_LEN);
        }
    }

    /* ---- leg 4b: the window that VANISHES — the ONLY falsifier of the
     * "never reached win_ret" guard (see WVANISH). The tracee exit_group()s with no
     * taken branch, so nothing is recorded and the materializer has no failed read to
     * set truncated from: only the guard can. Without it this returns a clean, complete,
     * EMPTY capture of a window that ran and died. ---- */
    {
        void *vf = mmap(NULL, WVANISH_LEN, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (vf == MAP_FAILED) {
            printf("# SKIP windowed block-step vanish leg: mmap failed\n");
        } else {
            memcpy(vf, WVANISH, WVANISH_LEN);
            mprotect(vf, WVANISH_LEN, PROT_READ | PROT_EXEC);
            __builtin___clear_cache((char *)vf, (char *)vf + WVANISH_LEN);
            asmtest_trace_t *tr4b = asmtest_trace_new(64, 64);
            pid_t pid = fork();
            if (pid == 0) {
                ptrace(PTRACE_TRACEME, 0, NULL, NULL);
                raise(SIGSTOP);
                ((void (*)(void))vf)();
                _exit(0);
            }
            int st = 0, rc4b = ASMTEST_PTRACE_ETRACE;
            if (waitpid(pid, &st, 0) >= 0 && WIFSTOPPED(st) &&
                asmtest_ptrace_run_to(pid, vf) == ASMTEST_PTRACE_OK)
                rc4b = asmtest_ptrace_trace_attached_windowed_blockstep(
                    pid, vf, WVANISH_LEN, NULL, NULL, tr4b);
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            CHECK(rc4b == ASMTEST_PTRACE_OK &&
                      asmtest_emu_trace_insns_len(tr4b) == 0,
                  "windowed block-step: a window that exit_group()s before any "
                  "block closes records nothing and still returns OK");
            CHECK(rc4b == ASMTEST_PTRACE_OK &&
                      asmtest_emu_trace_truncated(tr4b),
                  "windowed block-step: that EMPTY vanished window is flagged "
                  "TRUNCATED — the !reached_end guard's one observable effect");
            asmtest_trace_free(tr4b);
            munmap(vf, WVANISH_LEN);
        }
    }

    /* ---- leg 5: a SIGSEGV raised and handled INSIDE the window ---- */
    {
        void *sf = mmap(NULL, WSIG_LEN, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (sf == MAP_FAILED) {
            printf("# SKIP windowed block-step signal leg: mmap failed\n");
        } else {
            memcpy(sf, WSIG, WSIG_LEN);
            mprotect(sf, WSIG_LEN, PROT_READ | PROT_EXEC);
            __builtin___clear_cache((char *)sf, (char *)sf + WSIG_LEN);
            asmtest_trace_t *ssg = asmtest_trace_new(64, 64);
            asmtest_trace_t *bsg = asmtest_trace_new(64, 64);
            long rsg = 0, rbg = 0;
            int rcsg = wsig_run(0, sf, 7, &rsg, ssg);
            int rcbg = wsig_run(1, sf, 7, &rbg, bsg);
            uint64_t sv = (uint64_t)(uintptr_t)sf;
            /* The frame runs exactly 7 instructions; the faulting load at +7 is
             * recorded ONCE, by both drivers. */
            static const uint64_t EOFF[7] = {0, 1, 4, 7, 10, 13, 14};
            int exact = (asmtest_emu_trace_insns_len(bsg) == 7);
            for (int i = 0; exact && i < 7; i++)
                exact = (bsg->insns[i] == sv + EOFF[i]);
            CHECK(rcsg == ASMTEST_PTRACE_OK && rcbg == ASMTEST_PTRACE_OK,
                  "windowed block-step: a window that FAULTS mid-block still "
                  "captures (signal delivery is not terminal)");
            CHECK(
                rsg == 7 && rbg == 7,
                "windowed block-step: the faulting frame returns its argument "
                "(the handler resumed it)");
            CHECK(exact, "windowed block-step: the block CUT by SIGSEGV is "
                         "reconstructed through the faulting instruction "
                         "(0,1,4,7,10,13,14 — recorded once each)");
            CHECK(wdrv_streams_identical(ssg, bsg),
                  "windowed block-step: DIFFERENTIAL ORACLE across a handled "
                  "SIGSEGV — identical to per-instruction");
            CHECK(!asmtest_emu_trace_truncated(bsg) &&
                      !asmtest_emu_trace_truncated(ssg),
                  "windowed block-step: a handled in-window fault does not "
                  "truncate the capture");
            asmtest_trace_free(ssg);
            asmtest_trace_free(bsg);
            munmap(sf, WSIG_LEN);
        }
    }

    /* ---- leg 6: the same-target-conditional ambiguity, through the WINDOW ----
     * The `||` / dual-guard shape (two direct conditionals to one label, first not
     * taken, second taken). BTF cannot say which jumped, so the block is ambiguous and
     * must be reported truncated rather than silently short. Same rule and same fixture
     * shape as the region-form leg in test_ptrace_blockstep. */
    {
        static const unsigned char WGUARD[] = {
            0x48, 0x85, 0xff,                         /*  0 test rdi,rdi */
            0x74, 0x0c,                               /*  3 je  17       */
            0x48, 0x85, 0xf6,                         /*  5 test rsi,rsi */
            0x74, 0x07,                               /*  8 je  17       */
            0x48, 0x89, 0xf8,                         /* 10 mov rax,rdi  */
            0x48, 0x01, 0xf0,                         /* 13 add rax,rsi  */
            0xc3,                                     /* 16 ret          */
            0x48, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, /* 17 mov rax,-1   */
            0xc3,                                     /* 24 ret          */
        };
        void *gf = mmap(NULL, sizeof WGUARD, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (gf == MAP_FAILED) {
            printf("# SKIP windowed block-step guard leg: mmap failed\n");
        } else {
            memcpy(gf, WGUARD, sizeof WGUARD);
            mprotect(gf, sizeof WGUARD, PROT_READ | PROT_EXEC);
            __builtin___clear_cache((char *)gf, (char *)gf + sizeof WGUARD);
            asmtest_trace_t *gsw = asmtest_trace_new(64, 64);
            asmtest_trace_t *gbw = asmtest_trace_new(64, 64);
            long grw = 0, gbw_res = 0;
            int rcgs = wguard_run(0, gf, sizeof WGUARD, &grw, gsw);
            int rcgb = wguard_run(1, gf, sizeof WGUARD, &gbw_res, gbw);
            uint64_t gv = (uint64_t)(uintptr_t)gf;
            static const uint64_t GW_EXP[6] = {0, 3, 5, 8, 17, 24};
            int gwseq = (rcgs == ASMTEST_PTRACE_OK &&
                         asmtest_emu_trace_insns_len(gsw) == 6);
            for (int i = 0; gwseq && i < 6; i++)
                gwseq = (gsw->insns[i] == gv + GW_EXP[i]);
            CHECK(
                gwseq && !asmtest_emu_trace_truncated(gsw),
                "windowed block-step guard oracle: the per-instruction window "
                "proves 0,3,5,8,17,24 all ran");
            CHECK(grw == -1 && gbw_res == -1,
                  "windowed block-step guard: both drivers run the frame "
                  "identically (returns -1 via the second je)");
            CHECK(
                rcgb == ASMTEST_PTRACE_OK && asmtest_emu_trace_truncated(gbw),
                "windowed block-step guard: two conditionals sharing a target "
                "make the block AMBIGUOUS -> truncated, never silently short");
            int gwsub = 1, gwi = 0;
            for (unsigned long long i = 0;
                 gwsub && i < asmtest_emu_trace_insns_len(gbw); i++) {
                while (gwi < 6 && gv + GW_EXP[gwi] != gbw->insns[i])
                    gwi++;
                gwsub = (gwi < 6);
                gwi++;
            }
            CHECK(gwsub,
                  "windowed block-step guard: every reconstructed address "
                  "is one the per-instruction window saw execute, in order");
            asmtest_trace_free(gsw);
            asmtest_trace_free(gbw);
            munmap(gf, sizeof WGUARD);
        }
    }

    /* ---- argument validation ---- */
    {
        asmtest_trace_t *tr5 = asmtest_trace_new(8, 8);
        CHECK(asmtest_ptrace_trace_attached_windowed_blockstep(
                  0, NULL, WDRV_LEN, NULL, NULL, tr5) ==
                      ASMTEST_PTRACE_EINVAL &&
                  asmtest_ptrace_trace_attached_windowed_blockstep(
                      0, drv, 0, NULL, NULL, tr5) == ASMTEST_PTRACE_EINVAL &&
                  asmtest_ptrace_trace_attached_windowed_blockstep(
                      0, drv, WDRV_LEN, NULL, NULL, NULL) ==
                      ASMTEST_PTRACE_EINVAL,
              "windowed block-step: NULL base / zero len / NULL trace are "
              "EINVAL");
        asmtest_trace_free(tr5);
    }

    asmtest_trace_free(ss);
    asmtest_trace_free(bs);
    munmap(chan, sizeof *chan);
    munmap(drv, WDRV_LEN);
    munmap(m1, sizeof M1);
    munmap(m2, sizeof M2);
#else
    printf("# SKIP windowed block-step: Linux x86-64 only (no AArch64 "
           "PTRACE_SINGLEBLOCK)\n");
#endif
}

/* Fork-internal windowed capture (asmtest_ptrace_trace_window_call): the SAME
 * multi-region whole-window trace as test_ptrace_windowed, but the primitive OWNS its
 * tracee — no caller-side fork / PTRACE_TRACEME / run_to. Publishes the two leaves to a
 * channel via the exported shims (asmtest_addr_channel_new/_publish_rec), then ONE call
 * forks, run_to's the frame, and captures driver + both leaves. This is the entry a
 * managed binding uses (it cannot fork safely). */
static void test_ptrace_window_call(void) {
#if defined(__linux__) && defined(__x86_64__)
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
    printf("# SKIP ptrace window_call: Linux x86-64 only\n");
#endif
}

#if defined(__linux__) && defined(__x86_64__)
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
#if defined(__linux__) && defined(__x86_64__)
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
    unsigned char DRV[35] = {0xBF, 7,    0,    0,    0,    0xBE, 3,    0,   0,
                             0,    0x48, 0xB8, 0,    0,    0,    0,    0,   0,
                             0,    0,    0xFF, 0xD0, 0x48, 0xB8, 0,    0,   0,
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
    CHECK(!asmtest_emu_trace_truncated(tr),
          "stealth windowed capture complete");

    asmtest_addr_channel_free_shared(chan);
    asmtest_trace_free(tr);
    munmap(drv, sizeof DRV);
    munmap(m1, sizeof M1);
    munmap(m2, sizeof M2);
#else
    printf("# SKIP stealth windowed: Linux x86-64 only\n");
#endif
}

#if defined(__linux__) && defined(__x86_64__)
/* run_fn for test_amd_sample_window: invoke the hot-loop blob with its trip count. */
static void *g_asw_fn;
static long g_asw_arg;
static void asw_run(void *arg) {
    (void)arg;
    ((long (*)(long))g_asw_fn)(g_asw_arg);
}
#endif

/* §E5 AutoFDO/BOLT block-frequency reweighting (asmtest_amd_block_weight_sample): a
 * host-independent, DETERMINISTIC guard for the reweighting math, run on every host with
 * a synthetic branch stack (like test_amd_reconstruction) so it does not depend on live
 * AMD hardware. The plain drain records one endpoint per branch; this instead credits the
 * BLOCK [to_i, from_{i+1}] by its byte span (approx instruction count), so a long
 * straight-line block outweighs a tiny branchy one — the E5 fidelity upgrade. */
static void test_amd_block_weight(void) {
#if defined(__linux__) && defined(__x86_64__)
    /* Three retired branches, newest-first (as perf delivers): e[0] newest .. e[2] oldest.
     * The blocks are the runs between an older branch's TARGET and the next branch's
     * SOURCE: [0x2000, 0x2040) = 0x40 bytes (big straight-line run) and [0x3000, 0x3004)
     * = 4 bytes (tiny). The oldest target 0x2000 heads the big block; the newest target
     * 0x4000 opens a block the sample never closed, credited once. */
    struct perf_branch_entry br[3];
    memset(br, 0, sizeof br);
    br[0].from = 0x3004;
    br[0].to = 0x4000; /* newest */
    br[1].from = 0x2040;
    br[1].to = 0x3000;
    br[2].from = 0x1000;
    br[2].to = 0x2000; /* oldest */

    uint64_t ips[64];
    size_t n = asmtest_amd_block_weight_sample(br, 3, ips, 0, 64);
    /* w(big) = 0x40/4 + 1 = 17 copies of 0x2000; w(tiny) = 4/4 + 1 = 2 copies of 0x3000;
     * newest target 0x4000 once. Total 17 + 2 + 1 = 20. */
    size_t c2000 = 0, c3000 = 0, c4000 = 0, other = 0;
    for (size_t i = 0; i < n; i++) {
        if (ips[i] == 0x2000)
            c2000++;
        else if (ips[i] == 0x3000)
            c3000++;
        else if (ips[i] == 0x4000)
            c4000++;
        else
            other++;
    }
    CHECK(n == 20 && other == 0,
          "E5 block-weight: 3-branch stack yields 20 span-weighted endpoints");
    CHECK(c2000 == 17 && c3000 == 2 && c4000 == 1,
          "E5 block-weight: big block (0x2000) outweighs the tiny block "
          "(0x3000)");
    CHECK(c2000 > c3000,
          "E5 block-weight: a long straight-line block is NOT under-weighted "
          "vs. branchy code");

    /* Abort skip + backward-span degrade. e[1] aborted (rolled back) -> skipped, so the
     * surviving adjacency is oldest(0x100) -> newest, whose from (0x50) is BEFORE the head
     * (0x100): not a real forward fall-through, so it degrades to a single weight-1 hit. */
    struct perf_branch_entry deg[3];
    memset(deg, 0, sizeof deg);
    deg[0].from = 0x50;
    deg[0].to = 0x60; /* newest */
    deg[1].from = 0xFF;
    deg[1].to = 0x80;
    deg[1].abort = 1; /* rolled back: skipped */
    deg[2].from = 0x40;
    deg[2].to = 0x100; /* oldest */
    size_t dn = asmtest_amd_block_weight_sample(deg, 3, ips, 0, 64);
    CHECK(dn == 2 && ips[0] == 0x100 && ips[1] == 0x60,
          "E5 block-weight: aborts are skipped and a backward span degrades to "
          "weight 1");

    /* The cap is honored: starting near the end appends at most (cap - at) endpoints. */
    size_t cn = asmtest_amd_block_weight_sample(br, 3, ips, 62, 64);
    CHECK(cn == 64, "E5 block-weight: respects the ips[] buffer cap");
#else
    printf("# SKIP AMD block-weight: Linux x86-64 only\n");
#endif
}

/* §D3 statistical AMD-LBR whole-window survey (asmtest_hwtrace_sample_window_amd): a
 * region-free branch-stack SAMPLE of a hot loop, collecting absolute branch-target
 * endpoints. Not an exact trace — a sampled hot-address histogram. Needs Zen 3+/LBR +
 * CAP_PERFMON, so it self-skips off that hardware/permission (the docker-hwtrace-amd lane
 * runs it live). x86-64 only. */
static void test_amd_sample_window(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_AMD_LBR)) {
        char why[160];
        asmtest_hwtrace_skip_reason(ASMTEST_HWTRACE_AMD_LBR, why, sizeof why);
        printf("# SKIP AMD sample window: %s\n", why);
        return;
    }
    /* A hot loop: sum(n) via a decrement loop — one taken back-edge per iteration, so
     * the branch TARGET (loop top, offset 0x3) dominates the sampled endpoints. */
    static const unsigned char LOOP[] = {
        0x48, 0x31, 0xC0, /* 0x0: xor rax, rax        */
        0x48, 0x01, 0xF8, /* 0x3: L: add rax, rdi     */
        0x48, 0xFF, 0xCF, /* 0x6: dec rdi             */
        0x75, 0xF8,       /* 0x9: jnz L (-> 0x3)      */
        0xC3,             /* 0xb: ret                 */
    };
    void *fn = mmap(NULL, sizeof LOOP, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fn == MAP_FAILED) {
        printf("# SKIP AMD sample window: mmap failed\n");
        return;
    }
    memcpy(fn, LOOP, sizeof LOOP);
    mprotect(fn, sizeof LOOP, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)fn, (char *)fn + sizeof LOOP);
    g_asw_fn = fn;
    g_asw_arg =
        500000; /* 500k taken branches — long enough to accumulate PMU samples */
    uint64_t base = (uint64_t)(uintptr_t)fn, end = base + sizeof LOOP;

    size_t cap = 65536;
    uint64_t *ips = (uint64_t *)malloc(cap * sizeof(uint64_t));
    size_t nips = 0;
    int trunc = 0;
    int rc = asmtest_hwtrace_sample_window_amd(asw_run, NULL, 16, ips, cap,
                                               &nips, &trunc);
    CHECK(rc == ASMTEST_HW_OK, "AMD sample_window returns OK");
    CHECK(nips > 0, "AMD sample_window collected branch-target endpoints");
    size_t inregion = 0;
    for (size_t i = 0; i < nips; i++)
        if (ips[i] >= base && ips[i] < end)
            inregion++;
    /* The hot loop's back-edge target dominates: the vast majority of sampled endpoints
     * land in the tiny [base,end) blob. Statistical, so assert a strong majority. */
    CHECK(inregion * 2 > nips,
          "AMD sample_window: most sampled endpoints land in the hot loop");
    printf("# AMD sample_window: nips=%zu in-loop=%zu truncated=%d\n", nips,
           inregion, trunc);

    /* §E5: the SAME survey with AutoFDO/BOLT block-frequency reweighting. It weights each
     * fall-through block by its span, so the loop body [0x3,0x9) is credited by length
     * rather than one hit per back-edge — the in-loop dominance still holds, and (since a
     * span-of-6 block contributes >1 copy) the reweighted survey collects at least as many
     * endpoints as the plain one for the same run. */
    size_t wnips = 0;
    int wtrunc = 0;
    int wrc = asmtest_hwtrace_sample_window_amd_weighted(asw_run, NULL, 16, ips,
                                                         cap, &wnips, &wtrunc);
    CHECK(wrc == ASMTEST_HW_OK,
          "AMD sample_window (E5 block-weighted) returns OK");
    CHECK(wnips > 0,
          "AMD sample_window (E5 block-weighted) collected weighted endpoints");
    size_t winregion = 0;
    for (size_t i = 0; i < wnips; i++)
        if (ips[i] >= base && ips[i] < end)
            winregion++;
    CHECK(winregion * 2 > wnips, "AMD sample_window (E5 block-weighted): most "
                                 "weight lands in the hot loop");
    printf("# AMD sample_window E5: nips=%zu in-loop=%zu truncated=%d\n", wnips,
           winregion, wtrunc);

    free(ips);
    munmap(fn, sizeof LOOP);
#else
    printf("# SKIP AMD sample window: Linux x86-64 only\n");
#endif
}

/* Zen-2 F6: the whole-window survey's IBS-Op FALLBACK. On Zen 2 the branch stack is
 * absent, so asmtest_hwtrace_sample_window_amd delegates to the statistical IBS-Op
 * survey and still returns a hot-method histogram (was EUNAVAIL). We force that path
 * with ASMTEST_FORCE_IBS_SURVEY so it also exercises + cross-validates on Zen 3+/CI
 * boxes that DO have the branch stack. Gated on asmtest_ibs_available(), so it self-
 * skips on any non-AMD-IBS host. Also covers the begin/end (using-block) split. */
static void test_amd_sample_window_ibs(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_ibs_available()) {
        printf("# SKIP AMD IBS survey fallback: %s\n",
               asmtest_ibs_skip_reason());
        return;
    }
    /* Same hot loop as test_amd_sample_window; its taken back-edge target (loop top,
     * offset 0x3) dominates the sampled endpoints. A larger trip count than the
     * branch-stack test gives IBS-Op (coarser period) enough samples. */
    static const unsigned char LOOP[] = {
        0x48, 0x31, 0xC0, /* 0x0: xor rax, rax    */
        0x48, 0x01, 0xF8, /* 0x3: L: add rax, rdi */
        0x48, 0xFF, 0xCF, /* 0x6: dec rdi         */
        0x75, 0xF8,       /* 0x9: jnz L (-> 0x3)  */
        0xC3,             /* 0xb: ret             */
    };
    void *fn = mmap(NULL, sizeof LOOP, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (fn == MAP_FAILED) {
        printf("# SKIP AMD IBS survey fallback: mmap failed\n");
        return;
    }
    memcpy(fn, LOOP, sizeof LOOP);
    mprotect(fn, sizeof LOOP, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)fn, (char *)fn + sizeof LOOP);
    g_asw_fn = fn;
    g_asw_arg =
        40000000; /* 40M taken branches — enough IBS-Op samples in-window */
    uint64_t base = (uint64_t)(uintptr_t)fn, end = base + sizeof LOOP;

    setenv("ASMTEST_FORCE_IBS_SURVEY", "1",
           1); /* force the IBS path everywhere */

    size_t cap = 65536;
    uint64_t *ips = (uint64_t *)malloc(cap * sizeof(uint64_t));
    size_t nips = 0;
    int trunc = 0;
    int rc = asmtest_hwtrace_sample_window_amd(asw_run, NULL, 16, ips, cap,
                                               &nips, &trunc);
    /* available() is a substrate probe; perf_event_open can still be blocked. */
    if (rc == ASMTEST_HW_EUNAVAIL) {
        printf("# SKIP AMD IBS survey fallback: IBS perf_open blocked\n");
        unsetenv("ASMTEST_FORCE_IBS_SURVEY");
        free(ips);
        munmap(fn, sizeof LOOP);
        return;
    }
    CHECK(rc == ASMTEST_HW_OK,
          "AMD IBS survey fallback returns OK (not EUNAVAIL)");
    CHECK(nips > 0, "AMD IBS survey fallback collected endpoints");
    size_t inregion = 0;
    for (size_t i = 0; i < nips; i++)
        if (ips[i] >= base && ips[i] < end)
            inregion++;
    CHECK(inregion * 2 > nips,
          "AMD IBS survey fallback: most endpoints land in the hot loop");
    printf("# AMD IBS survey fallback: nips=%zu in-loop=%zu truncated=%d\n",
           nips, inregion, trunc);

    /* The begin/end (using-block) split must take the same IBS path. */
    void *ctx = NULL;
    int brc = asmtest_hwtrace_sample_begin_amd(16, &ctx);
    if (brc == ASMTEST_HW_OK) {
        asw_run(NULL); /* window body runs inline while IBS samples */
        size_t bn = 0;
        int btr = 0;
        int erc = asmtest_hwtrace_sample_end_amd(ctx, ips, cap, &bn, &btr);
        CHECK(erc == ASMTEST_HW_OK, "AMD IBS begin/end split returns OK");
        size_t bin = 0;
        for (size_t i = 0; i < bn; i++)
            if (ips[i] >= base && ips[i] < end)
                bin++;
        CHECK(bn > 0 && bin * 2 > bn,
              "AMD IBS begin/end split: most endpoints in the hot loop");
    } else {
        CHECK(brc == ASMTEST_HW_EUNAVAIL,
              "AMD IBS begin returns OK or a clean EUNAVAIL");
    }

    unsetenv("ASMTEST_FORCE_IBS_SURVEY");
    free(ips);
    munmap(fn, sizeof LOOP);
#else
    printf("# SKIP AMD IBS survey fallback: Linux x86-64 only\n");
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

    /* --- int3 (T1): a foreign target's OWN int3 must truncate and leave the target
     * in its SIGTRAP delivery-stop (never killed). --- */
    static const unsigned char INT3_X86[] = {0x53, 0x48, 0x89, 0xfb, 0xCC,
                                             0x48, 0x89, 0xd8, 0x48, 0x01,
                                             0xc0, 0x5b, 0xc3};
    volatile int *igo = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    void *iq = mmap(NULL, sizeof INT3_X86, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (igo != MAP_FAILED && iq != MAP_FAILED) {
        *igo = 0;
        memcpy(iq, INT3_X86, sizeof INT3_X86);
        mprotect(iq, sizeof INT3_X86, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)iq, (char *)iq + sizeof INT3_X86);
        pid_t ipid = fork();
        if (ipid == 0) {
            while (!*igo) {
            }
            volatile long r = ((add2_fn)iq)(7, 0);
            (void)r;
            _exit(0);
        }
        nanosleep(&ts, NULL);
        int istatus = 0;
        if (ptrace(PTRACE_ATTACH, ipid, NULL, NULL) == 0 &&
            waitpid(ipid, &istatus, 0) >= 0) {
            *igo = 1;
            asmtest_trace_t *it = asmtest_trace_new(64, 64);
            long ires = 0;
            int irc = asmtest_ptrace_trace_attached_blockstep(
                ipid, iq, sizeof INT3_X86, &ires, it);
            static const uint64_t I_EXP[] = {0x0, 0x1, 0x4};
            int iseq = (irc == ASMTEST_PTRACE_OK &&
                        asmtest_emu_trace_insns_total(it) == 3);
            for (int i = 0; iseq && i < 3; i++)
                iseq = (it->insns[i] == I_EXP[i]);
            CHECK(iseq, "attach block-step int3: stream is exactly +0 +1 +4");
            CHECK(asmtest_emu_trace_truncated(it),
                  "attach block-step int3: app int3 truncates");
            /* Left in the SIGTRAP delivery-stop, NOT killed: a WNOHANG wait shows
             * no new state change and GETSIGINFO reads the app trap's SI_KERNEL. */
            int wst = 0;
            pid_t w = waitpid(ipid, &wst, WNOHANG);
            siginfo_t si;
            memset(&si, 0, sizeof si);
            int gsi = ptrace(PTRACE_GETSIGINFO, ipid, NULL, &si);
            CHECK(w == 0 && gsi == 0 && si.si_code == SI_KERNEL,
                  "attach block-step int3: target left stopped at its own int3 "
                  "(SI_KERNEL), never killed");
            asmtest_trace_free(it);
        }
        kill(ipid, SIGKILL);
        waitpid(ipid, &istatus, 0);
        munmap(iq, sizeof INT3_X86);
        munmap((void *)igo, sizeof(int));
    }
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
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP whole-window scope: single-step unavailable\n");
        return;
    }
    /* begin_window rejects a NULL trace deterministically (no backend needed). */
    asmtest_hwtrace_scope_t bad = {0, 0, -1};
    CHECK(asmtest_hwtrace_begin_window(NULL, &bad) == ASMTEST_HW_EINVAL,
          "whole-window: begin_window(NULL trace) is EINVAL");

    void *p = ss_map_exec(ROUTINE, sizeof ROUTINE);
    if (p == NULL) {
        printf("# SKIP whole-window: mmap failed\n");
        return;
    }
    asmtest_hwtrace_options_t opts;
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK, "whole-window init");

    asmtest_trace_t *tr = asmtest_trace_new(4096, 0);
    asmtest_hwtrace_scope_t scope = {0xffffffffu, 0, -1};
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
    asmtest_hwtrace_scope_t phantom = {5, 999, -1};
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
#if defined(__linux__) && defined(__x86_64__)
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK,
          "whole-window buckets init");

    asmtest_trace_t *tr = asmtest_trace_new(4096, 0);
    asmtest_hwtrace_scope_t scope = {0xffffffffu, 0, -1};
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

#if defined(__linux__) && defined(__x86_64__)
/* Is `addr` among a whole-window trace's recorded ABSOLUTE addresses? */
static int ww_has_addr(const asmtest_trace_t *t, uint64_t addr) {
    for (size_t i = 0; i < t->insns_len; i++)
        if (t->insns[i] == addr)
            return 1;
    return 0;
}
#endif

/* §Z1.1 WEAK-tier DESCEND_ALL + the capture ring's OWN truncation. A region-free
 * window has no [base,len) to step OVER, so "capture whatever ran here" IS descent
 * level L3: TF traps every retired instruction and, with no in-range filter, the
 * handler records a CALLEE's absolute RIPs too. The load-bearing assert is one
 * caller traced BOTH ways — the shipped region-scoped frame filters ss_on_sigtrap's
 * store to [base,len) and drops the callee entirely (3 in-region insns), while the
 * region-free window keeps it (the callee's 3 IPs are all present). That difference
 * IS the lifted record policy; the walk is never asserted to be transparent. The
 * ring is then rendered through render_versioned against a self (pid==0) code-image
 * — §Z1's byte source for an absolute-RIP stream. (test_wholewindow_singlestep
 * renders LIVE memory via render_window; test_zeroctor_managed_compose drives
 * render_versioned from a SYNTHETIC trace — only here does a REAL region-free
 * capture meet the versioned render.) Last, the ring's own self-truncation: a
 * runaway overruns the frame's bounded RIP buffer and the post-pass flags
 * `truncated` — a DIFFERENT path from the trace-cap prefix test_wholewindow_banner
 * asserts, and isolated from it here by a trace cap large enough that
 * insns_total == insns_len, so the flag can only have come from the ring.
 *
 * NOT asserted, and not assertable at this tier: the plan's second truncation path,
 * the §L3 instruction budget + ITIMER_REAL/SIGALRM watchdog. Those are the
 * OUT-OF-PROCESS descent handle's guards (asmtest_descent_set_insn_budget /
 * _set_watchdog_ms), gated by test_descent_fork; the in-process TF window has
 * neither. Any x86-64 Linux; no PMU/perf/privilege. */
static void test_wholewindow_ss_descend(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP whole-window descend: single-step unavailable\n");
        return;
    }
    /* The CALLEE, on its OWN mapping: mov rax,rdi; add rax,rsi; ret — executed IPs
     * {0x0,0x3,0x6}. A separate page, so its RIPs can never fall inside the
     * caller's [base,len) and the two runs below discriminate cleanly. */
    static const unsigned char WW_CALLEE[] = {
        0x48, 0x89, 0xf8, /* mov rax,rdi */
        0x48, 0x01, 0xf0, /* add rax,rsi */
        0xc3};            /* ret         */
    void *kp = ss_map_exec(WW_CALLEE, sizeof WW_CALLEE);
    if (kp == NULL) {
        printf("# SKIP whole-window descend: mmap failed\n");
        return;
    }
    /* The CALLER: movabs r11,<callee>; call r11; ret — executed IPs {0x0,0xa,0xd}
     * (the call's own trap lands on the callee's entry, not on 0xd). */
    unsigned char WW_CALLER[] = {
        0x49, 0xbb, 0,    0, 0, 0, 0, 0, 0, 0, /* 0x0  movabs r11,<callee> */
        0x41, 0xff, 0xd3,                      /* 0xa  call r11            */
        0xc3};                                 /* 0xd  ret                 */
    uint64_t kb = (uint64_t)(uintptr_t)kp;
    memcpy(&WW_CALLER[2], &kb, sizeof kb);
    void *cp = ss_map_exec(WW_CALLER, sizeof WW_CALLER);
    if (cp == NULL) {
        printf("# SKIP whole-window descend: mmap failed\n");
        munmap(kp, sizeof WW_CALLEE);
        return;
    }
    asmtest_hwtrace_options_t opts;
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK,
          "whole-window descend init");

    uint64_t cb = (uint64_t)(uintptr_t)cp;
    add2_fn fn = (add2_fn)cp;

    /* Run 1 — the REGION-FREE window: no register_region, no [base,len). Bracket
     * ONLY the call (every instruction between arm and disarm is stepped). */
    asmtest_trace_t *tr = asmtest_trace_new(4096, 0);
    asmtest_hwtrace_scope_t scope = {0xffffffffu, 0, -1};
    int rc_begin = asmtest_hwtrace_begin_window(tr, &scope);
    if (rc_begin != ASMTEST_HW_OK) {
        printf("# SKIP whole-window descend: begin_window unavailable here "
               "(rc=%d)\n",
               rc_begin);
        asmtest_hwtrace_shutdown();
        asmtest_trace_free(tr);
        munmap(cp, sizeof WW_CALLER);
        munmap(kp, sizeof WW_CALLEE);
        return;
    }
    long rw = fn(20, 22);
    int rc_end = asmtest_hwtrace_end_window(scope, tr);
    CHECK(rc_begin == ASMTEST_HW_OK && rc_end == ASMTEST_HW_OK && rw == 42,
          "whole-window descend: the region-free window opens, calls out, and "
          "closes");

    int caller_ok = ww_has_addr(tr, cb + 0x0) && ww_has_addr(tr, cb + 0xa) &&
                    ww_has_addr(tr, cb + 0xd);
    int callee_ok = ww_has_addr(tr, kb + 0x0) && ww_has_addr(tr, kb + 0x3) &&
                    ww_has_addr(tr, kb + 0x6);
    CHECK(caller_ok,
          "whole-window descend: the caller's absolute IPs are captured");
    CHECK(callee_ok, "whole-window descend: DESCEND_ALL descended — the "
                     "CALLEE's out-of-(former-)range IPs are captured too");

    /* Run 2 — the SAME caller, REGION-scoped. ss_on_sigtrap stores rip-base only
     * in-range, so the callee is dropped: exactly the caller's 3 in-region insns
     * and nothing outside [0,len). This is what makes run 1's callee_ok a lifted
     * POLICY rather than an accident of the fixture. */
    asmtest_trace_t *trr = asmtest_trace_new(64, 64);
    asmtest_hwtrace_register_region("wwdescend", cp, sizeof WW_CALLER, trr);
    asmtest_hwtrace_begin("wwdescend");
    long rr = fn(20, 22);
    asmtest_hwtrace_end("wwdescend");
    int in_range = 1;
    for (size_t i = 0; i < trr->insns_len; i++)
        if (trr->insns[i] >= sizeof WW_CALLER)
            in_range = 0;
    CHECK(rr == 42 && trr->insns_total == 3 && in_range,
          "whole-window descend: the REGION-scoped run of the SAME caller "
          "filters the callee out (3 in-region insns)");

    /* The §Z1 render swap: an absolute-RIP ring has no single contiguous
     * [base,len), so render_scope cannot render it — only render_versioned against
     * a code-image can. Snapshot both leaves into a self (pid==0) image and render
     * the REAL captured window through it: the two leaves decode to text, the
     * untracked harness noise honestly reads "(no bytes @version)". */
    if (!asmtest_codeimage_available() || !asmtest_disas_available()) {
        printf("# SKIP whole-window descend versioned render: %s\n",
               asmtest_disas_available() ? "codeimage unavailable"
                                         : "built without Capstone");
    } else {
        asmtest_codeimage_t *img = asmtest_codeimage_new(0);
        int t1 =
            (img != NULL) &&
            asmtest_codeimage_track(img, cp, sizeof WW_CALLER) == ASMTEST_CI_OK;
        int t2 =
            (img != NULL) &&
            asmtest_codeimage_track(img, kp, sizeof WW_CALLEE) == ASMTEST_CI_OK;
        uint64_t when = (img != NULL) ? asmtest_codeimage_now(img) : 0;
        int need = (img != NULL) ? asmtest_hwtrace_render_versioned(img, when,
                                                                    tr, NULL, 0)
                                 : ASMTEST_HW_EINVAL;
        char *buf = (char *)malloc(need > 0 ? (size_t)need + 1 : 1);
        buf[0] = '\0';
        int wrote = (need > 0) ? asmtest_hwtrace_render_versioned(
                                     img, when, tr, buf, (size_t)need + 1)
                               : need;
        CHECK(t1 && t2 && need > 0 && wrote == need,
              "whole-window descend: render_versioned sizes and fills the REAL "
              "captured ring (snprintf semantics)");
        /* Ground truth per leaf, from the source blobs at their live addresses. */
        char g_call[128], g_add[128];
        asmtest_disas(ASMTEST_ARCH_X86_64, WW_CALLER, sizeof WW_CALLER, cb, 0xa,
                      g_call, sizeof g_call);
        asmtest_disas(ASMTEST_ARCH_X86_64, WW_CALLEE, sizeof WW_CALLEE, kb, 0x3,
                      g_add, sizeof g_add);
        CHECK(g_call[0] != '\0' && strstr(buf, g_call) != NULL,
              "whole-window descend: the versioned render decodes the caller's "
              "call site");
        CHECK(
            g_add[0] != '\0' && strstr(buf, g_add) != NULL,
            "whole-window descend: the versioned render decodes the DESCENDED "
            "callee's body");
        free(buf);
        asmtest_codeimage_free(img);
    }
    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    asmtest_trace_free(trr);
    munmap(cp, sizeof WW_CALLER);
    munmap(kp, sizeof WW_CALLEE);

    /* The capture ring's OWN self-truncation. AMD_LOOP(1,trips) runs 3 insns per
     * trip, so a big trip count overruns the frame's bounded absolute-RIP buffer;
     * the handler sets the frame's overflow flag and the post-pass turns it into
     * trace.truncated. The trace cap is deliberately far LARGER than the ring, so
     * insns_total == insns_len proves trace_append_insn's cap was never reached and
     * the flag can ONLY be the ring's — the two paths, told apart. */
    void *lp = ss_map_exec(AMD_LOOP, sizeof AMD_LOOP);
    if (lp == NULL) {
        printf("# SKIP whole-window descend ring overflow: mmap failed\n");
        return;
    }
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
    asmtest_hwtrace_init(&opts);
    asmtest_trace_t *tro = asmtest_trace_new(1u << 22, 0); /* >> the ring cap */
    asmtest_hwtrace_scope_t oscope = {0xffffffffu, 0, -1};
    long (*loop)(long, long) = (long (*)(long, long))lp;
    int ob = asmtest_hwtrace_begin_window(tro, &oscope);
    if (ob == ASMTEST_HW_OK) {
        loop(1, 600000); /* 1.8M+ insns — far past the bounded ring */
        asmtest_hwtrace_end_window(oscope, tro);
        CHECK(asmtest_emu_trace_truncated(tro) &&
                  tro->insns_total == tro->insns_len && tro->insns_len > 0,
              "whole-window descend: a runaway window self-truncates on the "
              "BOUNDED RING (not the trace cap)");
    } else {
        printf("# SKIP whole-window descend ring overflow: begin_window "
               "unavailable (rc=%d)\n",
               ob);
    }
    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tro);
    munmap(lp, sizeof AMD_LOOP);
#else
    printf("# SKIP whole-window descend: x86-64 Linux only\n");
#endif
}

#if defined(__linux__) && defined(__x86_64__)
static volatile int g_ahf_go;       /* main -> hopped thread: main has armed */
static volatile int g_ahf_rc = 999; /* the hopped end_window's return code */
static volatile int g_ahf_tid;      /* the hopped closer's OS tid */
static asmtest_hwtrace_scope_t g_ahf_scope; /* the handle armed on MAIN */
static asmtest_trace_t *g_ahf_tr;           /* the hopped closer's OWN trace */

/* The async hop: close MAIN's REGION-FREE window handle from THIS thread, exactly as
 * a continuation resuming on a thread-pool thread would run `using`'s Dispose. The
 * handle is REAL and live — it just belongs to another thread's TLS frame stack. */
static void *asynchop_close_on_thread(void *arg) {
    (void)arg;
    while (!g_ahf_go) {
    } /* wait until main has armed (main owns the frame + TF) */
    g_ahf_tid = (int)syscall(SYS_gettid);
    g_ahf_rc = asmtest_hwtrace_end_window(g_ahf_scope, g_ahf_tr);
    return NULL;
}
#endif

/* §Z4 — the async-hop honesty DEFAULT for the region-free (empty-ctor) window: the
 * scope is a THREAD window, so a `using (new AsmTrace())` whose work hops threads is
 * Disposed somewhere else, and end_window must flag `truncated` rather than present
 * a thread window as a complete logical-operation trace. The region-keyed §0.2
 * backstop CANNOT fire here — it keys on find_region(name), which returns NULL for a
 * window that registered no region — so end_window re-implements the check on the
 * handle: asmtest_ss_frame_lookup reads the CALLING thread's initial-exec TLS frame
 * stack, so the arming thread's frame is invisible from the closer and the miss is
 * the hop signal. Errs false-truncated over false-complete.
 *
 * Distinct from the two shipped neighbours, deliberately: test_arm_tid_mismatch
 * covers the REGION-keyed begin_scope/end path (via asmtest_hwtrace_arm_tid), and
 * test_wholewindow_singlestep's phantom {5,999} handle only SIMULATES the hop with a
 * never-armed handle — which a process-global frame table would still miss, so it
 * cannot see a cross-thread visibility regression. Here the handle is genuinely
 * armed on main and genuinely closed from a second thread, and main's own frame must
 * SURVIVE the foreign close (asserted below). Main arms and disarms itself: the
 * arming thread owns TF, so it is never left stepping behind a foreign close. */
static void test_asynchop_flag(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP async-hop flag: single-step unavailable\n");
        return;
    }
    void *p = ss_map_exec(ROUTINE, sizeof ROUTINE);
    if (p == NULL) {
        printf("# SKIP async-hop flag: mmap failed\n");
        return;
    }
    asmtest_hwtrace_options_t opts;
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK, "async-hop flag init");

    asmtest_trace_t *tr = asmtest_trace_new(4096, 0); /* main's window */
    /* The hopped close gets its OWN trace, so `truncated` on it can only have come
     * from the frame-lookup miss — never from main's window overflowing. */
    asmtest_trace_t *trh = asmtest_trace_new(16, 0);
    g_ahf_tr = trh;
    g_ahf_go = 0;
    g_ahf_rc = 999;
    g_ahf_tid = 0;
    g_ahf_scope.idx = 0xffffffffu;
    g_ahf_scope.gen = 0;

    /* Spawn BEFORE arming: pthread_create under TF would step the whole runtime.
     * Spawn OUTSIDE CHECK(): the macro evaluates its condition TWICE, so a
     * pthread_create inside it starts a second, unjoined thread that races the
     * close and outlives the test. */
    pthread_t th;
    int rc_spawn = pthread_create(&th, NULL, asynchop_close_on_thread, NULL);
    CHECK(rc_spawn == 0, "async-hop flag: spawn the hopped closing thread");

    add2_fn fn = (add2_fn)p;
    int rc_begin = asmtest_hwtrace_begin_window(tr, &g_ahf_scope);
    long r = 0;
    if (rc_begin == ASMTEST_HW_OK)
        r = fn(20, 22); /* the window's real work, on the arming thread */
    g_ahf_go = 1;       /* release the hop (also unblocks the skip path) */
    pthread_join(th, NULL);
    /* Main closes its OWN frame: resolves, so it disarms TF and normalizes. */
    int rc_end = asmtest_hwtrace_end_window(g_ahf_scope, tr);

    if (rc_begin != ASMTEST_HW_OK) {
        printf("# SKIP async-hop flag: begin_window unavailable here (rc=%d)\n",
               rc_begin);
        asmtest_hwtrace_shutdown();
        asmtest_trace_free(tr);
        asmtest_trace_free(trh);
        munmap(p, sizeof ROUTINE);
        return;
    }
    CHECK(rc_begin == ASMTEST_HW_OK && r == 42,
          "async-hop flag: begin_window arms the region-free window on main");
    CHECK(g_ahf_tid != 0 && g_ahf_tid != (int)syscall(SYS_gettid),
          "async-hop flag: the close really ran on a SECOND OS thread");
    CHECK(g_ahf_rc == ASMTEST_HW_OK,
          "async-hop flag: the hopped close returns OK (honest degradation, "
          "not an error)");
    /* THE §Z4 default: a region-free window closed off its arming thread is
     * flagged, never silently dropped and never presented as complete. */
    CHECK(asmtest_emu_trace_truncated(trh),
          "async-hop flag: closing a REGION-FREE window from another thread "
          "flags truncated");
    /* And the foreign close must not have stolen or closed main's frame: main's
     * own close still resolves (OK) and its window still holds the real capture. */
    CHECK(
        rc_end == ASMTEST_HW_OK && ww_has_addr(tr, (uint64_t)(uintptr_t)p) &&
            ww_has_addr(tr, (uint64_t)(uintptr_t)p + 0x11),
        "async-hop flag: the arming thread's frame SURVIVES the foreign close "
        "(TLS-local) and still captured the routine");
    CHECK(asmtest_hwtrace_arm_tid() == -1,
          "async-hop flag: arm_tid reads idle (-1) after the window closes");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    asmtest_trace_free(trh);
    munmap(p, sizeof ROUTINE);
#else
    printf("# SKIP async-hop flag: needs x86-64 Linux threads\n");
#endif
}

#if defined(__linux__) && defined(__x86_64__)
/* §Z4 handle-COLLISION fixture. tls_gen_ctr is __thread, so EVERY thread's FIRST
 * window is handle {idx=0, gen=1}: two FRESH threads therefore hold handle values
 * that are LITERALLY EQUAL yet name two different frames in two different TLS
 * tables. Main can play neither role — its gen counter is far past 1 by the time
 * this runs — which is exactly why two spawned threads are needed to see it. */
static volatile int
    g_hc_a_armed; /* A -> B: A's handle is published and LIVE   */
static volatile int
    g_hc_a_fail; /* A -> B: A could not arm; do not hang       */
static volatile int
    g_hc_b_done; /* B -> A: B is finished; A may leave its     */
static asmtest_hwtrace_scope_t
    g_hc_ha; /* the handle armed on thread A         */
static asmtest_hwtrace_scope_t
    g_hc_hb; /* the handle armed on thread B         */
static int g_hc_a_tid, g_hc_b_tid;
static int g_hc_rc_foreign =
    999; /* B's end_window(A's handle) return code    */
static asmtest_trace_t
    *g_hc_tra; /* A's window                                */
static asmtest_trace_t
    *g_hc_trb; /* B's OWN window                            */
static asmtest_trace_t
    *g_hc_trh;        /* the foreign close's own trace             */
static void *g_hc_pa; /* A's routine                               */
static void *g_hc_pb1,
    *g_hc_pb2; /* B's routines, pre- and post-foreign-close */
static long g_hc_ra, g_hc_rb1, g_hc_rb2;

/* Thread A — the ARMING thread whose window B will try to close. */
static void *hc_thread_a(void *arg) {
    (void)arg;
    g_hc_a_tid = (int)syscall(SYS_gettid);
    /* A's FIRST window on a FRESH thread => {idx=0, gen=1}. */
    if (asmtest_hwtrace_begin_window(g_hc_tra, &g_hc_ha) != ASMTEST_HW_OK) {
        g_hc_a_fail = 1;
        return NULL;
    }
    g_hc_ra = ((add2_fn)g_hc_pa)(20, 22); /* A's real in-window work */
    g_hc_a_armed = 1;
    /* Stay INSIDE the window across B's foreign close — the async-hop shape: A is
     * still in its `using` block when the continuation Disposes elsewhere. This spin
     * is single-stepped, but B is already parked and its armed section is a few
     * hundred instructions, so it is bounded far below SS_WINDOW_CAP (1<<20). Its
     * RIPs are window noise: nothing below asserts A's window is un-truncated. */
    while (!g_hc_b_done) {
    }
    asmtest_hwtrace_end_window(g_hc_ha, g_hc_tra);
    return NULL;
}

/* Thread B — holds its OWN window at the SAME {idx,gen} and closes A's handle. */
static void *hc_thread_b(void *arg) {
    (void)arg;
    g_hc_b_tid = (int)syscall(SYS_gettid);
    while (!g_hc_a_armed && !g_hc_a_fail) { /* wait until A's handle is live */
    }
    if (g_hc_a_fail) {
        g_hc_b_done = 1;
        return NULL;
    }
    /* B's FIRST window on a FRESH thread => ALSO {idx=0, gen=1}: identical to A's. */
    if (asmtest_hwtrace_begin_window(g_hc_trb, &g_hc_hb) != ASMTEST_HW_OK) {
        g_hc_b_done = 1;
        return NULL;
    }
    g_hc_rb1 =
        ((add2_fn)g_hc_pb1)(20, 22); /* captured BEFORE the foreign close */
    /* THE FOREIGN CLOSE: close A's window from B while B holds its own window at the
     * very same {idx,gen}. The handle is real, live, and belongs to A. */
    g_hc_rc_foreign = asmtest_hwtrace_end_window(g_hc_ha, g_hc_trh);
    g_hc_rb2 =
        ((add2_fn)g_hc_pb2)(20, 22); /* captured only if B's window LIVES */
    asmtest_hwtrace_end_window(g_hc_hb, g_hc_trb); /* B closes its OWN window */
    g_hc_b_done = 1;
    return NULL;
}
#endif

/* §Z4 — the COLLIDING-HANDLE false-complete. The neighbour test_asynchop_flag covers
 * the case where the closing thread holds NO window: its TLS frame table is empty, so
 * the lookup misses and end_window honestly truncates. This covers the case it cannot:
 * the closer holds its OWN window at the SAME {idx,gen}, because a handle carrying only
 * {idx,gen} is NOT unique across threads — tls_gen_ctr is __thread, so every thread's
 * first window is {0,1}. asmtest_ss_frame_lookup then indexes the CLOSER's TLS table
 * and matches on gen alone, resolving a foreign handle to the closer's OWN frame. The
 * result is the one thing this tier must never do: the WRONG window is closed and the
 * close reports a false-COMPLETE (truncated=0, OK). The tier's house style is a
 * conservative miss — false-truncated, never false-complete.
 *
 * The fix is the §Z4 "arm_tid carry": the arming OS tid rides in the HANDLE, so a
 * foreign close is detected explicitly and flags truncated. (Comparing arm_tid inside
 * frame_lookup alone cannot work: it reads the CALLER's own TLS table, where the
 * frame's arm_tid is always the caller's own tid — the tid must travel in the handle.)
 * Any x86-64 Linux; no PMU/perf/privilege. */
static void test_crossthread_handle_collision(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        printf("# SKIP handle collision: single-step unavailable\n");
        return;
    }
    /* Three SEPARATE mappings: A's routine, and B's pre- and post-close routines.
     * Distinct pages, so each thread's capture is attributable to exactly one of
     * them and "B's window kept recording after the foreign close" is decidable. */
    g_hc_pa = ss_map_exec(ROUTINE, sizeof ROUTINE);
    g_hc_pb1 = ss_map_exec(ROUTINE, sizeof ROUTINE);
    g_hc_pb2 = ss_map_exec(ROUTINE, sizeof ROUTINE);
    if (g_hc_pa == NULL || g_hc_pb1 == NULL || g_hc_pb2 == NULL) {
        printf("# SKIP handle collision: mmap failed\n");
        return;
    }
    asmtest_hwtrace_options_t opts;
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK,
          "handle collision init");

    g_hc_tra =
        asmtest_trace_new(4096, 0); /* A's window (holds the TF spin too) */
    g_hc_trb =
        asmtest_trace_new(8192, 0); /* B's own window                     */
    /* The foreign close gets its OWN trace, never a window's, so `truncated` on it
     * can only have come from the frame-lookup miss — never from an overflow. */
    g_hc_trh = asmtest_trace_new(16, 0);
    g_hc_a_armed = 0;
    g_hc_a_fail = 0;
    g_hc_b_done = 0;
    g_hc_a_tid = 0;
    g_hc_b_tid = 0;
    g_hc_rc_foreign = 999;
    g_hc_ra = g_hc_rb1 = g_hc_rb2 = 0;
    g_hc_ha.idx = g_hc_hb.idx = 0xffffffffu;
    g_hc_ha.gen = g_hc_hb.gen = 0;

    /* Spawn BOTH before either arms: pthread_create under TF would step the runtime
     * (and blocks SIGTRAP around clone(), which is fatal inside a window).
     * NB: spawn OUTSIDE CHECK() — the macro evaluates its condition TWICE (once to
     * pick the format, once to count the failure), so a pthread_create inside it
     * silently starts a SECOND, unjoined thread. */
    pthread_t ta, tb;
    int rc_spawn_b = pthread_create(&tb, NULL, hc_thread_b, NULL);
    CHECK(rc_spawn_b == 0,
          "handle collision: spawn the colliding closer thread B");
    int rc_spawn_a = pthread_create(&ta, NULL, hc_thread_a, NULL);
    CHECK(rc_spawn_a == 0, "handle collision: spawn the arming thread A");
    pthread_join(tb, NULL);
    pthread_join(ta, NULL);

    if (g_hc_a_fail || g_hc_rc_foreign == 999) {
        printf("# SKIP handle collision: begin_window unavailable on a spawned "
               "thread here\n");
        asmtest_hwtrace_shutdown();
        asmtest_trace_free(g_hc_tra);
        asmtest_trace_free(g_hc_trb);
        asmtest_trace_free(g_hc_trh);
        munmap(g_hc_pa, sizeof ROUTINE);
        munmap(g_hc_pb1, sizeof ROUTINE);
        munmap(g_hc_pb2, sizeof ROUTINE);
        return;
    }

    /* The PREMISE this test rests on: the two handles really are indistinguishable by
     * {idx,gen}. If this ever stops holding, the test below stops testing what it
     * claims, so assert it rather than assume it. (The fix does NOT change this —
     * idx and gen still collide; only the carried arm_tid tells them apart.) */
    CHECK(g_hc_ha.idx == g_hc_hb.idx && g_hc_ha.gen == g_hc_hb.gen,
          "handle collision: two fresh threads' first windows really do share "
          "one "
          "{idx,gen} handle value");
    CHECK(g_hc_a_tid != 0 && g_hc_b_tid != 0 && g_hc_a_tid != g_hc_b_tid,
          "handle collision: A and B are genuinely two different OS threads");
    CHECK(g_hc_ra == 42 && g_hc_rb1 == 42 && g_hc_rb2 == 42,
          "handle collision: every routine really ran");
    CHECK(g_hc_rc_foreign == ASMTEST_HW_OK,
          "handle collision: the foreign close returns OK (honest degradation, "
          "not an error)");

    /* THE BUG (1/2): a close of ANOTHER thread's window must be a conservative MISS.
     * Against the {idx,gen}-only handle this resolves to B's OWN frame and reports a
     * false-COMPLETE: truncated=0. */
    CHECK(asmtest_emu_trace_truncated(g_hc_trh),
          "handle collision: closing ANOTHER thread's window from a thread "
          "holding "
          "its own colliding {idx,gen} flags truncated (never false-complete)");

    /* THE BUG (2/2): and it must not close the WRONG window. B's own frame has to
     * survive the foreign close, so work B runs AFTER it is still captured. Against
     * the {idx,gen}-only handle the foreign close pops B's frame and disarms B's TF,
     * so pb2 never lands in B's window. */
    CHECK(ww_has_addr(g_hc_trb, (uint64_t)(uintptr_t)g_hc_pb1),
          "handle collision: B's own window captured its pre-close work");
    CHECK(
        ww_has_addr(g_hc_trb, (uint64_t)(uintptr_t)g_hc_pb2),
        "handle collision: B's OWN window SURVIVES the foreign close and still "
        "captures B's post-close work (the WRONG window must not be closed)");

    /* And A's window — the one actually named by the handle — still holds A's own
     * capture (it is closed by A itself, on the thread that armed it). */
    CHECK(ww_has_addr(g_hc_tra, (uint64_t)(uintptr_t)g_hc_pa),
          "handle collision: A's window still holds A's own capture");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(g_hc_tra);
    asmtest_trace_free(g_hc_trb);
    asmtest_trace_free(g_hc_trh);
    munmap(g_hc_pa, sizeof ROUTINE);
    munmap(g_hc_pb1, sizeof ROUTINE);
    munmap(g_hc_pb2, sizeof ROUTINE);
#else
    printf("# SKIP handle collision: needs x86-64 Linux threads\n");
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK, "scope hygiene init");

    /* (a) Nested: open a region-free window, then a region scope INSIDE it. */
    asmtest_trace_t *tr_win = asmtest_trace_new(65536, 0);
    asmtest_trace_t *tr_in = asmtest_trace_new(64, 64);
    asmtest_hwtrace_register_region("hyg_inner", p, sizeof ROUTINE, tr_in);
    asmtest_hwtrace_scope_t win = {0xffffffffu, 0, -1};
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
        asmtest_hwtrace_scope_t sc = {0xffffffffu, 0, -1};
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
    asmtest_hwtrace_scope_t fsc = {0xffffffffu, 0, -1};
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
#if defined(__linux__) && defined(__x86_64__)
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
    INIT_OPTS(opts, ASMTEST_HWTRACE_SINGLESTEP);
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK,
          "whole-window banner init");

    asmtest_trace_t *tr =
        asmtest_trace_new(8, 0); /* tiny cap: force overflow */
    asmtest_hwtrace_scope_t scope = {0xffffffffu, 0, -1};
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

    /* Phase 4 debug logging FIRST — its forked children need the getenv-once cache still
     * pristine in the parent (no tier call has run yet). */
    test_debug_logging();

    /* Backend-independent: validate the AMD reconstruction decoder. */
    test_amd_snapshot_substrate_probe();
    test_amd_reconstruction();
    test_amd_spec_filter();
    test_amd_decode_hw_ceiling();
    test_amd_msr_spec_filter();
    test_amd_tailcall_exit();
    test_amd_reduced_filter();
    test_amd_block_weight(); /* §E5 AutoFDO block-frequency reweighting (deterministic) */
    /* F43 ring-parse seam + F44 reduced-filter stitch follow + P9 cpuinfo-flag probe:
     * host-independent, exercise logic that self-skips off AMD LBR hardware. */
    test_amd_ring_parse();
    test_amd_stitch_reduced_filter();
    test_amd_cpu_flag();

    /* Backend-independent: the §D4 async-hop stitching merge core. */
    test_stitch_slices();
    /* §D4 scripted-hop coverage: the handle-form merge from faked hops (the
     * host-independent native analog of the Node/Java AsyncStitchedTrace tests). */
    test_stitch_hops_scripted();

    /* §2: the recorder-backed PT image adapter (host-testable, no libipt/PT hw). */
    test_pt_image_from_codeimage();

    /* §Z2: end-to-end synthetic Intel-PT decode — encode a valid PT stream with
     * libipt's encoder and drive the real asmtest_pt_decode[_window] bodies (no PT
     * hardware). Self-skips where libipt is absent. */
    test_wholewindow_decode();

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
    test_amd_live_smallroutine();
    test_amd_reach_period();
    test_call_auto();
    test_stealth_window_inline();
    test_amd_msr();

    /* §1 (AMD): concurrent per-thread AMD-LBR capture (capped lane on AMD). */
    test_concurrent_amd();

    /* AMD Tier-B stitching past the 16-deep window (host-validated, synthetic). */
    test_amd_stitch();
    test_amd_stitch_decodable();
    test_amd_stitch_period_spaced();

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

    /* §Z1.1: the WEAK tier's lifted record policy — DESCEND_ALL (the callee's IPs
     * are captured where the region-scoped run filters them out), the versioned
     * render of a REAL ring against a self code-image, and the bounded ring's own
     * self-truncation told apart from the trace cap. */
    test_wholewindow_ss_descend();

    /* §Z4: the async-hop honesty DEFAULT — a REGION-FREE window closed from a
     * second thread flags truncated (the region-keyed backstop cannot fire), and
     * the arming thread's TLS frame survives the foreign close. */
    test_asynchop_flag();

    /* §Z4: the COLLIDING-handle false-complete — a closer holding its OWN window at
     * the same {idx,gen} must not resolve another thread's handle to its own frame. */
    test_crossthread_handle_collision();

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
    test_ptrace_windowed_blockstep();
    test_ptrace_window_call();
    test_stealth_windowed();
    test_amd_sample_window();
    test_amd_sample_window_ibs();

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

    /* 2026-07 API flag day: the F27 size-negotiated options ABI guard, the F29
     * EPERM-vs-EUNAVAIL status surface (live permission-distinction lane on a
     * paranoid-blocked AMD host), and the F22/F26/F37 escalation-rung
     * discriminator. */
    test_options_abi_guard();
    test_status_surface();
    test_mechanism_discriminator();

    asmtest_trace_backend_t backend = ASMTEST_HWTRACE_INTEL_PT;
    if (!asmtest_hwtrace_available(backend)) {
        char why[160];
        asmtest_hwtrace_skip_reason(backend, why, sizeof why);
        printf("# SKIP hwtrace PT capture (Intel PT): %s\n", why);
        /* This early-return block is PT-specific (the AMD capture is exercised by the
         * AMD-only tests that already ran — test_amd_live, the Tier-B live loop, the
         * ring-parse seam). Report AMD's status HONESTLY: only "# SKIP … : <reason>"
         * when it is genuinely unavailable; naming its skip reason unconditionally
         * printed "# SKIP hwtrace AMD capture (AMD LBR): available" on a Zen 5 host
         * (CAP_PERFMON present, only PT missing) — a SKIP line whose reason says the
         * opposite. */
        if (asmtest_hwtrace_available(ASMTEST_HWTRACE_AMD_LBR)) {
            printf(
                "# hwtrace AMD capture (AMD LBR): available (exercised by the "
                "AMD-specific tests above)\n");
        } else {
            char awhy[160];
            asmtest_hwtrace_skip_reason(ASMTEST_HWTRACE_AMD_LBR, awhy,
                                        sizeof awhy);
            printf("# SKIP hwtrace AMD capture (AMD LBR): %s\n", awhy);
        }
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
    INIT_OPTS(opts, backend);
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
