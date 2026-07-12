/*
 * branchsnap.c — AMD-P0 deterministic software-event LBR snapshot capture.
 *
 * The sample_period=1 AMD path (hwtrace_begin_amd/_end_amd) floods a sample per taken
 * branch and then GUESSES which window best reflects the region ("richest-in-region"),
 * and it TRUNCATES a routine too fast to be sampled in-region at all. This is the
 * deterministic alternative the AMD plan's P0 #2 specifies: enable the LBR, plant a
 * hardware execution breakpoint at EACH region EXIT (P5: up to ASMTEST_AMD_MAX_EXITS ==
 * the 4 x86 debug registers, so whichever exit the run leaves through is a boundary),
 * and let a BPF program call bpf_get_branch_snapshot() when a boundary is reached —
 * reading the frozen 16-entry stack at a deterministic point. The raw perf_branch_entry
 * array is handed to the SAME amd_decode() the sampled path uses (it already filters to
 * in-region branches, so kernel-entry entries drop out), so the reconstruction is
 * unchanged and already tested.
 *
 * Feasibility confirmed on a Zen 5 (Ryzen 9 9950X): a #DB breakpoint does NOT evict the
 * region's in-region branches before the helper reads them (the AMD freeze path holds).
 *
 * Real body under ASMTEST_HAVE_LIBBPF (built only in the BPF-toolchain image); the #else
 * stub self-skips so this TU always links. Needs CAP_BPF + CAP_PERFMON + AMD LbrExtV2 +
 * Linux >= 6.10 at run time (asmtest_amd_snapshot_available() reports the static floor).
 */
#define _GNU_SOURCE

#include "amd_backend.h" /* shared asmtest_amd_decode / _snapshot_* decls + reduced filter */
#include "asmtest_hwtrace.h"
#include "asmtest_trace.h"
#include "debug.h" /* Phase 4: ASMTEST_HWDBG env-gated tier logging */

#include <stddef.h>

#if defined(__linux__) && defined(__x86_64__) && defined(ASMTEST_HAVE_LIBBPF)
#include <bpf/libbpf.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "branchsnap.skel.h"
#include "branchsnap_event.h"

/* ASMTEST_AMD_REDUCED_FILTER (used below) is defined once in amd_backend.h. */

/* The BPF side fills raw[] as perf_branch_entry[nr] under this fixed stride; the
 * drain recasts / element-indexes on the same assumption — lock it at compile time. */
_Static_assert(
    sizeof(struct perf_branch_entry) == BSNAP_ENTRY_SZ,
    "branchsnap_event.h BSNAP_ENTRY_SZ != sizeof(perf_branch_entry)");

static long bsnap_perf_open(struct perf_event_attr *a, pid_t pid, int cpu,
                            int grp, unsigned long flags) {
    return syscall(SYS_perf_event_open, a, pid, cpu, grp, flags);
}

/* Ring-buffer drain state: keep the snapshot with the MOST in-region entries (a loop
 * hits the exit breakpoint once per iteration; the richest hit reconstructs furthest). */
struct bsnap_drain {
    const void *base;
    size_t len;
    unsigned char best[BSNAP_MAX * BSNAP_ENTRY_SZ];
    unsigned long long best_nr;
    unsigned long long best_exit_ip; /* boundary that froze the best window */
    size_t best_inregion;
};

static int bsnap_on_event(void *ctx, void *data, size_t sz) {
    struct bsnap_drain *d = (struct bsnap_drain *)ctx;
    if (sz < sizeof(struct bsnap_event))
        return 0;
    const struct bsnap_event *ev = (const struct bsnap_event *)data;
    unsigned long long nr = ev->nr <= BSNAP_MAX ? ev->nr : BSNAP_MAX;
    const uint64_t base_ip = (uint64_t)(uintptr_t)d->base;
    const uint64_t end_ip = base_ip + d->len;
    size_t inregion = 0;
    for (unsigned long long i = 0; i < nr; i++) {
        const uint64_t *w = (const uint64_t *)(ev->raw + i * BSNAP_ENTRY_SZ);
        if ((w[0] >= base_ip && w[0] < end_ip) ||
            (w[1] >= base_ip && w[1] < end_ip))
            inregion++;
    }
    if (inregion > d->best_inregion) {
        d->best_inregion = inregion;
        d->best_nr = nr;
        d->best_exit_ip = ev->exit_ip; /* which boundary froze this window */
        memcpy(d->best, ev->raw, (size_t)nr * BSNAP_ENTRY_SZ);
    }
    return 0;
}

/* Armed-capture state: a single process-global slot, matching hwtrace.c's
 * single-active-region invariant (the marker path arms at begin() and drains at
 * end(); the one-shot below is begin -> run_fn -> end over the same slot). P5: one
 * HW execution breakpoint per region exit (up to ASMTEST_AMD_MAX_EXITS == the 4 x86
 * debug registers), all feeding ONE shared BPF program + ringbuf. */
struct bsnap_state {
    int active;
    int lfd; /* LBR-on branch-stack event (never read; powers the hardware)   */
    int nbp; /* armed exit breakpoints (bfd[0..nbp) / link[0..nbp) valid)     */
    int bfd[ASMTEST_AMD_MAX_EXITS]; /* HW execution breakpoints, one per exit */
    struct branchsnap_bpf *skel;
    struct bpf_link *link[ASMTEST_AMD_MAX_EXITS]; /* same prog, one link/bp   */
    struct ring_buffer *rb;
    struct bsnap_drain drain;
};
static struct bsnap_state g_bsnap;

static void bsnap_reset_fds(void) {
    memset(&g_bsnap, 0, sizeof g_bsnap);
    g_bsnap.lfd = -1;
    for (int i = 0; i < ASMTEST_AMD_MAX_EXITS; i++)
        g_bsnap.bfd[i] = -1;
}

static void bsnap_teardown(void) {
    if (g_bsnap.rb != NULL)
        ring_buffer__free(g_bsnap.rb);
    for (int i = 0; i < ASMTEST_AMD_MAX_EXITS; i++)
        if (g_bsnap.link[i] != NULL)
            bpf_link__destroy(g_bsnap.link[i]);
    if (g_bsnap.skel != NULL)
        branchsnap_bpf__destroy(g_bsnap.skel);
    for (int i = 0; i < ASMTEST_AMD_MAX_EXITS; i++)
        if (g_bsnap.bfd[i] >= 0)
            close(g_bsnap.bfd[i]);
    if (g_bsnap.lfd >= 0)
        close(g_bsnap.lfd);
    bsnap_reset_fds();
}

/* ARM the deterministic boundary snapshot for [base, base+len): LBR on + one hardware
 * execution breakpoint per listed exit (base+exit_offs[i], each in its own debug
 * register) + the SAME BPF snapshot program attached to each (N bpf_links, one shared
 * ringbuf). The region begin marker calls this when snapshot mode is selected; on ANY
 * failure it returns nonzero having released everything, so the caller can fall back to
 * the sample_period=1 path. ALL-OR-NOTHING: every listed exit is armed or none is — a
 * partially-covered exit set could silently miss the taken exit and misreport an honest
 * truncation, so a host where a debugger holds a needed debug register degrades to the
 * sampled path instead (P5). `branch_filter` nonzero requests the reduced LBR filter
 * (drops direct uncond jmp; the decoder follows them) — the deterministic snapshot's
 * window-stretch. Single-slot: a second arm while active fails. */
int asmtest_amd_snapshot_begin_multi(const void *base, size_t len,
                                     const size_t *exit_offs, int nexit,
                                     int branch_filter) {
    if (base == NULL || len == 0 || exit_offs == NULL || nexit < 1 ||
        nexit > ASMTEST_AMD_MAX_EXITS)
        return ASMTEST_HW_EINVAL;
    for (int i = 0; i < nexit; i++)
        if (exit_offs[i] >= len)
            return ASMTEST_HW_EINVAL;
    if (g_bsnap.active)
        return ASMTEST_HW_ESTATE;
    if (!asmtest_amd_snapshot_available())
        return ASMTEST_HW_EUNAVAIL;

    bsnap_reset_fds();
    g_bsnap.drain.base = base;
    g_bsnap.drain.len = len;

    /* (1) Enable LBR recording on this thread (a branch-stack event; we never read its
     * ring — it just turns the hardware on so the snapshot has data). With branch_filter
     * set, use the reduced filter (window-stretch); retry full on rejection. */
    struct perf_event_attr la;
    memset(&la, 0, sizeof la);
    la.size = sizeof la;
    la.type = PERF_TYPE_HARDWARE;
    la.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
    la.sample_type = PERF_SAMPLE_BRANCH_STACK;
    la.branch_sample_type =
        branch_filter ? ASMTEST_AMD_REDUCED_FILTER
                      : (PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_ANY);
    la.exclude_kernel = 1;
    la.exclude_hv = 1;
    la.disabled = 1;
    long lfd = bsnap_perf_open(&la, 0, -1, -1, 0);
    if (lfd < 0 &&
        branch_filter) { /* type-filter rejected: fall back to full */
        la.branch_sample_type =
            PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_ANY;
        lfd = bsnap_perf_open(&la, 0, -1, -1, 0);
    }
    if (lfd < 0)
        return ASMTEST_HW_EUNAVAIL;
    g_bsnap.lfd = (int)lfd;

    /* (2) One HW execution breakpoint per region exit, each in its own debug register
     * (the LBR event above is a PMU counter, not a DR, so all 4 x86 debug registers are
     * free). ALL-OR-NOTHING: any failed open (a debugger holding a DR, HBP_NUM
     * exhausted) releases everything so the caller falls back to sampling. */
    for (int i = 0; i < nexit; i++) {
        struct perf_event_attr ba;
        memset(&ba, 0, sizeof ba);
        ba.size = sizeof ba;
        ba.type = PERF_TYPE_BREAKPOINT;
        ba.bp_type = HW_BREAKPOINT_X;
        ba.bp_addr = (uint64_t)(uintptr_t)base + exit_offs[i];
        ba.bp_len = sizeof(long);
        ba.sample_period = 1;
        ba.exclude_kernel = 1;
        ba.exclude_hv = 1;
        ba.disabled = 1;
        long bfd = bsnap_perf_open(&ba, 0, -1, -1, 0);
        if (bfd < 0) {
            bsnap_teardown();
            return ASMTEST_HW_EUNAVAIL;
        }
        g_bsnap.bfd[i] = (int)bfd;
        g_bsnap.nbp = i + 1;
    }

    /* (3) Load the snapshot program once and attach the SAME program to every
     * breakpoint event — N bpf_links feeding ONE shared ringbuf; the richest-in-region
     * selection in bsnap_on_event already works across hits from different exits. Each
     * link's attach COOKIE carries its absolute breakpoint address, so every ring
     * record self-identifies WHICH boundary froze it (snapshot_end appends that exit's
     * deterministic boundary edge to the decode). */
    g_bsnap.skel = branchsnap_bpf__open();
    if (g_bsnap.skel == NULL || branchsnap_bpf__load(g_bsnap.skel) != 0) {
        bsnap_teardown();
        return ASMTEST_HW_EUNAVAIL;
    }
    for (int i = 0; i < nexit; i++) {
        LIBBPF_OPTS(bpf_perf_event_opts, popts,
                    .bpf_cookie =
                        (uint64_t)(uintptr_t)base + (uint64_t)exit_offs[i]);
        g_bsnap.link[i] = bpf_program__attach_perf_event_opts(
            g_bsnap.skel->progs.asmtest_branchsnap, g_bsnap.bfd[i], &popts);
        if (g_bsnap.link[i] == NULL) {
            bsnap_teardown();
            return ASMTEST_HW_EUNAVAIL;
        }
    }
    g_bsnap.rb = ring_buffer__new(bpf_map__fd(g_bsnap.skel->maps.snaps),
                                  bsnap_on_event, &g_bsnap.drain, NULL);
    if (g_bsnap.rb == NULL) {
        bsnap_teardown();
        return ASMTEST_HW_EUNAVAIL;
    }

    /* (4) Live: the workload runs with LBR + every breakpoint enabled until end(). A
     * failed ENABLE yields an empty ring the boundary drain cannot tell from "boundary
     * never hit," so fail loudly — tear down and self-skip — rather than silently
     * truncate. */
    ioctl(g_bsnap.lfd, PERF_EVENT_IOC_RESET, 0);
    if (ioctl(g_bsnap.lfd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        bsnap_teardown();
        return ASMTEST_HW_EUNAVAIL;
    }
    for (int i = 0; i < nexit; i++) {
        ioctl(g_bsnap.bfd[i], PERF_EVENT_IOC_RESET, 0);
        if (ioctl(g_bsnap.bfd[i], PERF_EVENT_IOC_ENABLE, 0) != 0) {
            bsnap_teardown();
            return ASMTEST_HW_EUNAVAIL;
        }
    }
    g_bsnap.active = 1;
    ASMTEST_HWDBG("snapshot_begin: armed nbp=%d exit_off[0]=%zu filter=%d",
                  nexit, exit_offs[0], branch_filter);
    return ASMTEST_HW_OK;
}

/* Single-exit thin wrapper: the public standalone entry's shape, and the legacy
 * LAST-exit best-effort hwtrace_begin_amd keeps for an explicit opts.snapshot on a
 * region with more exits than ASMTEST_AMD_MAX_EXITS debug registers. */
int asmtest_amd_snapshot_begin(const void *base, size_t len, size_t exit_off,
                               int branch_filter) {
    return asmtest_amd_snapshot_begin_multi(base, len, &exit_off, 1,
                                            branch_filter);
}

/* DRAIN the armed snapshot: disable the events, drain the BPF ring, decode the
 * richest boundary window into `trace` (truncated if the boundary was never hit),
 * and release everything. The region end marker's counterpart to _begin. */
int asmtest_amd_snapshot_end(asmtest_trace_t *trace) {
    if (!g_bsnap.active)
        return ASMTEST_HW_ESTATE;
    if (trace == NULL) {
        bsnap_teardown();
        return ASMTEST_HW_EINVAL;
    }
    for (int i = 0; i < g_bsnap.nbp; i++)
        ioctl(g_bsnap.bfd[i], PERF_EVENT_IOC_DISABLE, 0);
    ioctl(g_bsnap.lfd, PERF_EVENT_IOC_DISABLE, 0);
    /* P8: NON-BLOCKING drain. The exit breakpoint fires synchronously during run_fn and
     * the BPF program bpf_ringbuf_submit()s immediately, so by here every record is
     * already committed; the disable above guarantees no later producer. ring_buffer__poll
     * (…, 200) did a 200 ms epoll_wait FIRST — and on the no-hit / honest-truncation path
     * (now the common case, snapshot is default-on) nothing ever wakes epoll, so it burned
     * the full 200 ms and drained nothing. ring_buffer__consume reads the producer position
     * directly, drains all queued records, and returns at once (libbpf >= 0.4.0; the only
     * image building this TU carries 1.3.0). */
    ring_buffer__consume(g_bsnap.rb);

    int rc;
    if (g_bsnap.drain.best_inregion > 0) {
        /* P5 boundary completion — make the DETERMINISTIC properties of the frozen
         * window explicit to the shared decoder, which is written for the sampled
         * path's arbitrary windows:
         *
         * (a) TRIM the pre-entry glue. LBR eviction is strictly oldest-first, so if
         *     any glue entry OLDER than the oldest region-involved edge survived in
         *     the window, every in-region edge (all newer) survived too — the
         *     in-region history is COMPLETE since that point, and the pre-entry
         *     entries carry no region content (amd_replay skips them). Passing the
         *     trimmed suffix keeps the decoder's depth-ceiling honesty flag
         *     (nbr >= 16 -> truncated) armed EXACTLY when it should be: a window
         *     still full after trimming is all-region-involved, so in-region edges
         *     (the entry edge included) may have been evicted -> honest truncation
         *     stands. A glue-padded window of a tiny routine no longer misreports
         *     its complete reconstruction as truncated.
         *
         * (b) APPEND the boundary edge. The execution breakpoint fires BEFORE the
         *     exit instruction executes, so the frozen stack can never contain the
         *     exit's own edge and the replay used to stop at the last RECORDED
         *     branch — dropping the final straight-line run (all of it, for a
         *     no-taken-branch fall-through path). The freeze point is deterministic
         *     (the attach cookie says which exit fired) and every exit is a
         *     region-LEAVING instruction (ret / tail-jmp), so append the one edge
         *     that is guaranteed to retire next: from = the frozen exit, to =
         *     outside the region. The replay then decodes entry -> exit inclusive,
         *     byte-identical to what the MSR-direct sibling (which freezes AFTER
         *     the exit) captures. Without a cookie (exit_ip 0 / out of region) the
         *     completion is skipped — never guessed. */
        const uint64_t base_ip = (uint64_t)(uintptr_t)g_bsnap.drain.base;
        const uint64_t end_ip = base_ip + g_bsnap.drain.len;
        size_t nr = (size_t)g_bsnap.drain.best_nr;
        size_t use = 0; /* newest-first suffix length up to the oldest
                         * region-involved entry (>=1: best_inregion > 0) */
        for (size_t k = nr; k-- > 0;) {
            const uint64_t *w =
                (const uint64_t *)(g_bsnap.drain.best + k * BSNAP_ENTRY_SZ);
            if ((w[0] >= base_ip && w[0] < end_ip) ||
                (w[1] >= base_ip && w[1] < end_ip)) {
                use = k + 1;
                break;
            }
        }
        struct perf_branch_entry arr[BSNAP_MAX + 1];
        size_t n_dec = 0;
        const uint64_t exit_ip = g_bsnap.drain.best_exit_ip;
        if (exit_ip >= base_ip && exit_ip < end_ip) {
            memset(&arr[0], 0, sizeof arr[0]); /* flags clear: retired edge */
            arr[0].from = exit_ip;             /* newest: the boundary edge  */
            arr[0].to = 0;                     /* exits leave the region     */
            n_dec = 1;
        }
        memcpy(&arr[n_dec], g_bsnap.drain.best, use * BSNAP_ENTRY_SZ);
        n_dec += use;
        ASMTEST_HWDBG("snapshot_end: decode best_nr=%llu best_inregion=%zu "
                      "trimmed=%zu boundary=%d",
                      g_bsnap.drain.best_nr, g_bsnap.drain.best_inregion, use,
                      exit_ip >= base_ip && exit_ip < end_ip);
        rc = asmtest_amd_decode(arr, n_dec, g_bsnap.drain.base,
                                g_bsnap.drain.len, trace);
    } else {
        ASMTEST_HWDBG("snapshot_end: truncated — boundary never hit / no "
                      "in-region branch");
        trace->truncated = true; /* boundary never hit / no in-region branch */
        rc = ASMTEST_HW_OK;
    }
    /* F13 truncated-on-drop contract: a saturated ringbuf made the BPF program drop
     * >=1 boundary hit (bpf_ringbuf_reserve returned NULL and the program counted it).
     * The dropped hit may have been the RICHEST in-region window — a data-dependent
     * deeper path late in a hot run — so the selected best cannot be trusted as the
     * complete picture. Flag honest truncation, never a silent drop. (The .bss counter
     * starts at zero on every arm: the skeleton is freshly loaded per _begin_multi.) */
    if (g_bsnap.skel->bss->asmtest_bsnap_drops > 0) {
        ASMTEST_HWDBG(
            "snapshot_end: %llu ringbuf drop(s) -> truncated",
            (unsigned long long)g_bsnap.skel->bss->asmtest_bsnap_drops);
        trace->truncated = true;
    }
    bsnap_teardown();
    return rc;
}

int asmtest_amd_snapshot_trace(const void *base, size_t len, size_t exit_off,
                               void (*run_fn)(void *), void *arg,
                               asmtest_trace_t *trace) {
    if (base == NULL || len == 0 || exit_off >= len || run_fn == NULL ||
        trace == NULL)
        return ASMTEST_HW_EINVAL;
    /* Public standalone entry: full branch filter (branch_filter=0), unchanged. The
     * marker path (hwtrace_begin_amd) threads opts.branch_filter for the reduced filter. */
    int rc = asmtest_amd_snapshot_begin(base, len, exit_off, 0);
    if (rc != ASMTEST_HW_OK)
        return rc;
    run_fn(arg);
    return asmtest_amd_snapshot_end(trace);
}

#else /* no BPF toolchain / not x86-64 Linux */
int asmtest_amd_snapshot_begin_multi(const void *base, size_t len,
                                     const size_t *exit_offs, int nexit,
                                     int branch_filter) {
    (void)base;
    (void)len;
    (void)exit_offs;
    (void)nexit;
    (void)branch_filter;
    return ASMTEST_HW_ENOSYS;
}

int asmtest_amd_snapshot_begin(const void *base, size_t len, size_t exit_off,
                               int branch_filter) {
    (void)base;
    (void)len;
    (void)exit_off;
    (void)branch_filter;
    return ASMTEST_HW_ENOSYS;
}

int asmtest_amd_snapshot_end(asmtest_trace_t *trace) {
    (void)trace;
    return ASMTEST_HW_ENOSYS;
}

int asmtest_amd_snapshot_trace(const void *base, size_t len, size_t exit_off,
                               void (*run_fn)(void *), void *arg,
                               asmtest_trace_t *trace) {
    (void)base;
    (void)len;
    (void)exit_off;
    (void)run_fn;
    (void)arg;
    (void)trace;
    return ASMTEST_HW_ENOSYS;
}
#endif
