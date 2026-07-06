/*
 * branchsnap.c — AMD-P0 deterministic software-event LBR snapshot capture.
 *
 * The sample_period=1 AMD path (hwtrace_begin_amd/_end_amd) floods a sample per taken
 * branch and then GUESSES which window best reflects the region ("richest-in-region"),
 * and it TRUNCATES a routine too fast to be sampled in-region at all. This is the
 * deterministic alternative the AMD plan's P0 #2 specifies: enable the LBR, plant a
 * hardware execution breakpoint at the region EXIT, and let a BPF program call
 * bpf_get_branch_snapshot() when the boundary is reached — reading the frozen 16-entry
 * stack at ONE deterministic point. The raw perf_branch_entry array is handed to the
 * SAME amd_decode() the sampled path uses (it already filters to in-region branches, so
 * kernel-entry entries drop out), so the reconstruction is unchanged and already tested.
 *
 * Feasibility confirmed on a Zen 5 (Ryzen 9 9950X): a #DB breakpoint does NOT evict the
 * region's in-region branches before the helper reads them (the AMD freeze path holds).
 *
 * Real body under ASMTEST_HAVE_LIBBPF (built only in the BPF-toolchain image); the #else
 * stub self-skips so this TU always links. Needs CAP_BPF + CAP_PERFMON + AMD LbrExtV2 +
 * Linux >= 6.10 at run time (asmtest_amd_snapshot_available() reports the static floor).
 */
#define _GNU_SOURCE

#include "asmtest_hwtrace.h"
#include "asmtest_trace.h"

#include <stddef.h>

/* Shared decode from amd_backend.c (newest-first perf_branch_entry array). */
struct perf_branch_entry;
int asmtest_amd_decode(const struct perf_branch_entry *br, size_t nbr,
                       const void *base, size_t len, asmtest_trace_t *trace);
int asmtest_amd_snapshot_available(void);

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

static long bsnap_perf_open(struct perf_event_attr *a, pid_t pid, int cpu, int grp,
                            unsigned long flags) {
    return syscall(SYS_perf_event_open, a, pid, cpu, grp, flags);
}

/* Ring-buffer drain state: keep the snapshot with the MOST in-region entries (a loop
 * hits the exit breakpoint once per iteration; the richest hit reconstructs furthest). */
struct bsnap_drain {
    const void *base;
    size_t len;
    unsigned char best[BSNAP_MAX * BSNAP_ENTRY_SZ];
    unsigned long long best_nr;
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
        if ((w[0] >= base_ip && w[0] < end_ip) || (w[1] >= base_ip && w[1] < end_ip))
            inregion++;
    }
    if (inregion > d->best_inregion) {
        d->best_inregion = inregion;
        d->best_nr = nr;
        memcpy(d->best, ev->raw, (size_t)nr * BSNAP_ENTRY_SZ);
    }
    return 0;
}

/* Armed-capture state: a single process-global slot, matching hwtrace.c's
 * single-active-region invariant (the marker path arms at begin() and drains at
 * end(); the one-shot below is begin -> run_fn -> end over the same slot). */
struct bsnap_state {
    int active;
    int lfd; /* LBR-on branch-stack event (never read; powers the hardware)   */
    int bfd; /* HW execution breakpoint at the region exit                    */
    struct branchsnap_bpf *skel;
    struct bpf_link *link;
    struct ring_buffer *rb;
    struct bsnap_drain drain;
};
static struct bsnap_state g_bsnap;

static void bsnap_teardown(void) {
    if (g_bsnap.rb != NULL)
        ring_buffer__free(g_bsnap.rb);
    if (g_bsnap.link != NULL)
        bpf_link__destroy(g_bsnap.link);
    if (g_bsnap.skel != NULL)
        branchsnap_bpf__destroy(g_bsnap.skel);
    if (g_bsnap.bfd >= 0)
        close(g_bsnap.bfd);
    if (g_bsnap.lfd >= 0)
        close(g_bsnap.lfd);
    memset(&g_bsnap, 0, sizeof g_bsnap);
    g_bsnap.lfd = g_bsnap.bfd = -1;
}

/* ARM the deterministic boundary snapshot for [base, base+len): LBR on + a hardware
 * execution breakpoint at base+exit_off + the BPF snapshot program attached to it.
 * The region begin marker calls this when the `snapshot` option is set; on ANY
 * failure it returns nonzero having released everything, so the caller can fall
 * back to the sample_period=1 path. Single-slot: a second arm while active fails. */
int asmtest_amd_snapshot_begin(const void *base, size_t len, size_t exit_off) {
    if (base == NULL || len == 0 || exit_off >= len)
        return ASMTEST_HW_EINVAL;
    if (g_bsnap.active)
        return ASMTEST_HW_ESTATE;
    if (!asmtest_amd_snapshot_available())
        return ASMTEST_HW_EUNAVAIL;

    memset(&g_bsnap, 0, sizeof g_bsnap);
    g_bsnap.lfd = g_bsnap.bfd = -1;
    g_bsnap.drain.base = base;
    g_bsnap.drain.len = len;

    /* (1) Enable LBR recording on this thread (a branch-stack event; we never read its
     * ring — it just turns the hardware on so the snapshot has data). */
    struct perf_event_attr la;
    memset(&la, 0, sizeof la);
    la.size = sizeof la;
    la.type = PERF_TYPE_HARDWARE;
    la.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
    la.sample_type = PERF_SAMPLE_BRANCH_STACK;
    la.branch_sample_type = PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_ANY;
    la.exclude_kernel = 1;
    la.exclude_hv = 1;
    la.disabled = 1;
    long lfd = bsnap_perf_open(&la, 0, -1, -1, 0);
    if (lfd < 0)
        return ASMTEST_HW_EUNAVAIL;
    g_bsnap.lfd = (int)lfd;

    /* (2) HW execution breakpoint at the region exit. */
    struct perf_event_attr ba;
    memset(&ba, 0, sizeof ba);
    ba.size = sizeof ba;
    ba.type = PERF_TYPE_BREAKPOINT;
    ba.bp_type = HW_BREAKPOINT_X;
    ba.bp_addr = (uint64_t)(uintptr_t)base + exit_off;
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
    g_bsnap.bfd = (int)bfd;

    /* (3) Load + attach the snapshot BPF program to the breakpoint event. */
    g_bsnap.skel = branchsnap_bpf__open();
    if (g_bsnap.skel == NULL || branchsnap_bpf__load(g_bsnap.skel) != 0) {
        bsnap_teardown();
        return ASMTEST_HW_EUNAVAIL;
    }
    g_bsnap.link = bpf_program__attach_perf_event(
        g_bsnap.skel->progs.asmtest_branchsnap, g_bsnap.bfd);
    if (g_bsnap.link == NULL) {
        bsnap_teardown();
        return ASMTEST_HW_EUNAVAIL;
    }
    g_bsnap.rb = ring_buffer__new(bpf_map__fd(g_bsnap.skel->maps.snaps),
                                  bsnap_on_event, &g_bsnap.drain, NULL);
    if (g_bsnap.rb == NULL) {
        bsnap_teardown();
        return ASMTEST_HW_EUNAVAIL;
    }

    /* (4) Live: the workload runs with LBR + breakpoint enabled until end(). */
    ioctl(g_bsnap.lfd, PERF_EVENT_IOC_RESET, 0);
    ioctl(g_bsnap.lfd, PERF_EVENT_IOC_ENABLE, 0);
    ioctl(g_bsnap.bfd, PERF_EVENT_IOC_RESET, 0);
    ioctl(g_bsnap.bfd, PERF_EVENT_IOC_ENABLE, 0);
    g_bsnap.active = 1;
    return ASMTEST_HW_OK;
}

/* DRAIN the armed snapshot: disable the events, poll the BPF ring, decode the
 * richest boundary window into `trace` (truncated if the boundary was never hit),
 * and release everything. The region end marker's counterpart to _begin. */
int asmtest_amd_snapshot_end(asmtest_trace_t *trace) {
    if (!g_bsnap.active)
        return ASMTEST_HW_ESTATE;
    if (trace == NULL) {
        bsnap_teardown();
        return ASMTEST_HW_EINVAL;
    }
    ioctl(g_bsnap.bfd, PERF_EVENT_IOC_DISABLE, 0);
    ioctl(g_bsnap.lfd, PERF_EVENT_IOC_DISABLE, 0);
    ring_buffer__poll(g_bsnap.rb, 200);

    int rc;
    if (g_bsnap.drain.best_inregion > 0)
        rc = asmtest_amd_decode(
            (const struct perf_branch_entry *)g_bsnap.drain.best,
            (size_t)g_bsnap.drain.best_nr, g_bsnap.drain.base,
            g_bsnap.drain.len, trace);
    else {
        trace->truncated = true; /* boundary never hit / no in-region branch */
        rc = ASMTEST_HW_OK;
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
    int rc = asmtest_amd_snapshot_begin(base, len, exit_off);
    if (rc != ASMTEST_HW_OK)
        return rc;
    run_fn(arg);
    return asmtest_amd_snapshot_end(trace);
}

#else /* no BPF toolchain / not x86-64 Linux */
int asmtest_amd_snapshot_begin(const void *base, size_t len, size_t exit_off) {
    (void)base;
    (void)len;
    (void)exit_off;
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
