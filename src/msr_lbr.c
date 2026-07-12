/*
 * msr_lbr.c — AMD MSR-direct LBR snapshot: read the LbrExtV2 branch-record MSRs directly
 * around a region for an exact Tier-A capture with ZERO PMU interrupts (no sample_period=1
 * flood, no BPF toolchain). See docs/internal/plans/amd-msr-direct-lbr-plan.md.
 *
 * The frozen 16-entry FROM/TO stack is read straight out of /dev/cpu/N/msr and handed to the
 * SAME asmtest_amd_decode the sampled and boundary-snapshot paths use — so reconstruction is
 * unchanged and already tested. It captures a TINY routine (the case the sampled path
 * truncates), zero interrupts, needing only CAP_SYS_ADMIN + the `msr` module — the niche
 * where the deterministic BPF snapshot (CAP_BPF/libbpf) is unavailable.
 *
 * CONTAMINATION LIMIT (measured on Zen 5, empirical-first plan): freezing the LBR from
 * userspace is a /dev/cpu/N/msr write syscall whose glue branches occupy stack slots. A
 * user-only LBR_SELECT drops the syscall's kernel branches; the surviving userspace glue
 * (~a few) plus the routine's own branches must fit the 16-deep window, so this is complete
 * only for small routines and amd_decode honestly flags `truncated` when the window overflowed
 * (its existing depth check) — never partial-as-complete. Needs amd_lbr_v2 + /dev/cpu/N/msr
 * openable; self-skips (asmtest_amd_msr_available() -> 0) otherwise.
 */
#define _GNU_SOURCE

#include "amd_backend.h" /* shared asmtest_amd_decode / _lbr_depth decls + ASMTEST_HW_* */
#include "asmtest_hwtrace.h"
#include "asmtest_trace.h"
#include "debug.h" /* Phase 4: ASMTEST_HWDBG env-gated tier logging */

#include <stddef.h>

#if defined(__linux__) && defined(__x86_64__)
#include <fcntl.h>
#include <linux/perf_event.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* AMD LbrExtV2 MSRs (arch/x86/include/asm/msr-index.h, arch/x86/events/amd/lbr.c). */
#define MSR_DBG_EXTN_CFG                                                       \
    0xc000010fu /* bit 6 (LBRV2EN) enables the extended LBR */
#define MSR_LBR_SELECT                                                         \
    0xc000010eu /* suppress-mode filter: bit 0 -> user-only  */
#define MSR_SAMP_BR_FROM                                                       \
    0xc0010300u /* entry i: FROM = base + i*2, TO = +1        */
#define LBRV2EN (1ULL << 6)
#define LBR_IP_MASK                                                            \
    ((1ULL << 58) - 1) /* FROM/TO ip[57:0]                     */

static int msr_rd(int fd, uint32_t reg, uint64_t *out) {
    return pread(fd, out, 8, (off_t)reg) == 8 ? 0 : -1;
}
static int msr_wr(int fd, uint32_t reg, uint64_t v) {
    return pwrite(fd, &v, 8, (off_t)reg) == 8 ? 0 : -1;
}

/* amd_lbr_v2 flag from /proc/cpuinfo — the LBR-stack MSRs exist only on parts that report
 * it (the same gate the kernel's snapshot path uses, minus perfmon_v2/kernel which this
 * path does not need). P9: delegates to the single cached probe in amd_backend.c so this
 * path and the snapshot path can never drift on the flag test. */
static int amd_lbr_v2_present(void) {
    return asmtest_amd_has_cpu_flag("amd_lbr_v2");
}

/* Open /dev/cpu/N/msr O_RDWR for CPU `cpu` (needs CAP_SYS_ADMIN + the `msr` module). */
static int open_msr(int cpu) {
    char path[64];
    snprintf(path, sizeof path, "/dev/cpu/%d/msr", cpu);
    return open(path, O_RDWR);
}

/* Whether the MSR-direct LBR substrate is present: amd_lbr_v2 AND /dev/cpu/N/msr openable
 * read-write (CAP_SYS_ADMIN + msr module). Returns 1 (present) or 0 (self-skip). Does not
 * modify any MSR — pure probe. */
int asmtest_amd_msr_available(void) {
    if (!amd_lbr_v2_present())
        return 0;
    int cpu = sched_getcpu();
    if (cpu < 0)
        cpu = 0;
    int fd = open_msr(cpu);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

/* Decode one raw LbrExtV2 FROM/TO MSR pair (arch/x86/include/asm/msr-index.h layout:
 * TO[63]=valid/retired, TO[62]=spec/wrong-path, IP in [57:0]) into a perf_branch_entry.
 * Returns 1 (fills *out->from/to, IP-masked) for a RETIRED branch; 0 to skip — an empty
 * slot OR a speculative WRONG-PATH entry (valid=0, spec=1). Dropping the spec-only slot at
 * the SOURCE is the correctness point: this path never sets perf_branch_entry.spec, so
 * amd_replay's PERF_BR_SPEC_WRONG_PATH filter could never catch such an entry and a phantom
 * edge would leak into the reconstruction (and .spec is absent on pre-6.1 headers, where
 * that filter compiles out entirely). A retired-AND-speculative (valid=1, spec=1,
 * correct-path) branch retired, so it is kept. Pure — no MSR I/O — so a host-independent
 * unit test can drive the spec filter directly. */
int asmtest_amd_msr_decode_entry(uint64_t from, uint64_t to,
                                 struct perf_branch_entry *out) {
    if (!((to >> 63) & 1))
        return 0; /* valid=0: empty slot or spec-only wrong-path — drop at source */
    out->from = from & LBR_IP_MASK;
    out->to = to & LBR_IP_MASK;
    return 1;
}

/* MSR-direct Tier-A capture of [base, base+len): pin the calling thread to one CPU, enable
 * the LBR (user-only filter), run run_fn(arg), freeze, read the frozen 16-entry FROM/TO
 * stack, and decode it via the shared asmtest_amd_decode. Callback-thunk model like
 * asmtest_amd_snapshot_trace. On ANY MSR failure restores state and returns EUNAVAIL. */
int asmtest_amd_msr_trace(const void *base, size_t len, void (*run_fn)(void *),
                          void *arg, asmtest_trace_t *trace) {
    if (base == NULL || len == 0 || run_fn == NULL || trace == NULL)
        return ASMTEST_HW_EINVAL;
    if (!amd_lbr_v2_present())
        return ASMTEST_HW_EUNAVAIL;

    /* Pin to the current CPU so enable->run->freeze->read all hit the same core's stack. */
    int cpu = sched_getcpu();
    if (cpu < 0)
        cpu = 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((unsigned)cpu, &set);
    cpu_set_t prev;
    CPU_ZERO(&prev);
    int had_prev = sched_getaffinity(0, sizeof prev, &prev) == 0;
    if (sched_setaffinity(0, sizeof set, &set) != 0)
        return ASMTEST_HW_EUNAVAIL;

    int fd = open_msr(cpu);
    if (fd < 0) {
        ASMTEST_HWDBG("msr_trace: /dev/cpu/%d/msr open failed (need "
                      "CAP_SYS_ADMIN + msr module)",
                      cpu);
        if (had_prev)
            sched_setaffinity(0, sizeof prev, &prev);
        return ASMTEST_HW_EUNAVAIL;
    }

    int depth = asmtest_amd_lbr_depth(); /* 16 on every shipping part */
    if (depth < 1 || depth > 64)
        depth = 16;

    uint64_t save_cfg = 0, save_sel = 0;
    int rc = ASMTEST_HW_EUNAVAIL;
    if (msr_rd(fd, MSR_DBG_EXTN_CFG, &save_cfg) != 0 ||
        msr_rd(fd, MSR_LBR_SELECT, &save_sel) != 0)
        goto out;

    /* Disable while we reset; user-only filter (suppress kernel = bit 0); clear the stack. */
    if (msr_wr(fd, MSR_DBG_EXTN_CFG, save_cfg & ~LBRV2EN) != 0 ||
        msr_wr(fd, MSR_LBR_SELECT, 0x1) != 0)
        goto restore;
    for (int i = 0; i < depth; i++) {
        msr_wr(fd, MSR_SAMP_BR_FROM + (uint32_t)(i * 2), 0);
        msr_wr(fd, MSR_SAMP_BR_FROM + (uint32_t)(i * 2 + 1), 0);
    }

    /* Enable, run the region, freeze with minimal glue (this single wrmsr). */
    if (msr_wr(fd, MSR_DBG_EXTN_CFG, (save_cfg & ~LBRV2EN) | LBRV2EN) != 0)
        goto restore;
    run_fn(arg);
    msr_wr(fd, MSR_DBG_EXTN_CFG, save_cfg & ~LBRV2EN);

    /* Read the frozen stack into a newest-first perf_branch_entry array (entry 0 = TOS). */
    {
        struct perf_branch_entry br[64];
        memset(br, 0, sizeof br);
        size_t n = 0;
        const uint64_t base_ip = (uint64_t)(uintptr_t)base;
        const uint64_t end_ip = base_ip + len;
        int any_in_region = 0;
        int short_read = 0; /* abnormal partial pread of the frozen stack */
        for (int i = 0; i < depth; i++) {
            uint64_t f = 0, t = 0;
            if (msr_rd(fd, MSR_SAMP_BR_FROM + (uint32_t)(i * 2), &f) != 0 ||
                msr_rd(fd, MSR_SAMP_BR_FROM + (uint32_t)(i * 2 + 1), &t) != 0) {
                short_read =
                    1; /* stack read cut short: capture is incomplete */
                break;
            }
            if (!asmtest_amd_msr_decode_entry(f, t, &br[n]))
                continue; /* empty slot or speculative wrong-path (see helper) */
            if ((br[n].from >= base_ip && br[n].from < end_ip) ||
                (br[n].to >= base_ip && br[n].to < end_ip))
                any_in_region = 1;
            n++;
        }
        /* Decode via the shared path (in-region-filtered; window overflow -> truncated). */
        if (n > 0 && any_in_region) {
            ASMTEST_HWDBG("msr_trace: decode %zu retired entries short_read=%d",
                          n, short_read);
            rc = asmtest_amd_decode(br, n, base, len, trace);
            if (short_read)
                trace->truncated =
                    true; /* a partial stack is never a complete capture */
        } else {
            ASMTEST_HWDBG("msr_trace: truncated — no in-region branch in the "
                          "frozen stack (n=%zu)",
                          n);
            trace->truncated =
                true; /* nothing in-region: honest, never empty-complete */
            rc = ASMTEST_HW_OK;
        }
    }

restore:
    msr_wr(fd, MSR_DBG_EXTN_CFG, save_cfg);
    msr_wr(fd, MSR_LBR_SELECT, save_sel);
out:
    close(fd);
    if (had_prev)
        sched_setaffinity(0, sizeof prev, &prev);
    return rc;
}

#else /* not Linux x86-64 */
int asmtest_amd_msr_available(void) { return 0; }

int asmtest_amd_msr_trace(const void *base, size_t len, void (*run_fn)(void *),
                          void *arg, asmtest_trace_t *trace) {
    (void)base;
    (void)len;
    (void)run_fn;
    (void)arg;
    (void)trace;
    return ASMTEST_HW_ENOSYS;
}
#endif
