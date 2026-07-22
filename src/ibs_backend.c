/*
 * ibs_backend.c — statistical AMD IBS-Op tracing lane (asmtest_ibs.h).
 *
 * Two halves, split by portability:
 *
 *   1. asmtest_ibs_decode_op / asmtest_ibs_survey_free — PURE, host-independent.
 *      decode_op parses one IBS-Op PERF_SAMPLE_RAW payload into a control-flow
 *      edge; it touches no hardware and no perf headers, so it compiles and is
 *      unit-tested on EVERY host (the same discipline as amd_backend.c's
 *      synthetic-input asmtest_amd_decode test).
 *
 *   2. asmtest_ibs_available / asmtest_ibs_skip_reason / asmtest_ibs_survey_pid —
 *      Linux/x86-64/AMD only. Raw perf_event_open of the ibs_op PMU with the
 *      kernel `swfilt` bit (user-only sampling, unprivileged at
 *      perf_event_paranoid=2) + exclude_kernel; drain the ring; aggregate edges.
 *      Everywhere else these self-skip (available() -> 0, survey -> EUNAVAIL).
 *
 * The IBS-Op raw payload layout (empirically confirmed on a Zen 2 Ryzen 9 4900HS,
 * kernel 6.14): the PERF_SAMPLE_RAW field is {u32 size; char data[size]}; the
 * `data` payload this decoder consumes is
 *
 *     [u32 caps][u64 reg0][u64 reg1] ... [u64 reg7]      (size = 4 + 8*nregs)
 *
 * where reg[k] sits at payload byte offset 4 + 8*k:
 *     reg[1] = IbsOpRip     (branch SOURCE)
 *     reg[2] = IbsOpData    (the branch-resolution bitfield, below)
 *     reg[7] = IbsBrTarget  (branch TARGET, present when IBS_CAPS_BRNTRGT)
 * Cross-checked by PERF_SAMPLE_IP == reg[1] on live samples. See
 * docs/internal/plans/zen2-ibs-tracing-plan.md and the 2026-07-12 review.
 *
 * INVARIANT: statistical only — never feeds the exact insns[]/blocks[] parity path.
 *
 * Phase 7 adds the front-end IBS-FETCH lane below (asmtest_ibs_fetch_* in the
 * internal ibs_backend.h): the same open/mmap/drain machinery pointed at the
 * ibs_fetch PMU, decoding IbsFetchCtl/IbsFetchLinAd into a fetch-ADDRESS coverage
 * histogram (i-cache/ITLB miss + fetch latency) rather than control-flow edges.
 */
#include "asmtest_grow.h" /* asmtest_grow / _pow2 — overflow-checked pool growth (S6) */
#include "asmtest_ibs.h"

#include "ibs_backend.h" /* internal window primitives (asmtest_ibs_window_*) */

#include <errno.h>  /* asmtest_ibs_unavail_reason: the real perf_open failure */
#include <stddef.h> /* offsetof — the additive-ABI struct_size guard (pure) */
#include <stdlib.h>
#include <string.h>

/* --- IBS-Op raw record layout (shared by the pure decoder + live capture) ------ */
/* Payload is [u32 caps] then u64 registers; reg[k] at byte offset CAPS + 8*k. */
#define IBS_RAW_CAPS_BYTES 4u
#define IBS_RAW_REG_OFF(k) (IBS_RAW_CAPS_BYTES + 8u * (unsigned)(k))
/* Registers we read. reg[7] (target) is the deepest, so a usable edge needs it. */
#define IBS_REG_RIP   1 /* IbsOpRip: branch source    */
#define IBS_REG_DATA  2 /* IbsOpData: resolution bits */
#define IBS_REG_BRTGT 7 /* IbsBrTarget: branch target */
/* Smallest payload that carries the branch target register (reg[7]): 4 + 8*8 = 68. */
#define IBS_RAW_MIN_BYTES (IBS_RAW_CAPS_BYTES + 8u * (IBS_REG_BRTGT + 1u))
/* IbsOpData (reg[2]) bit positions (kernel union ibs_op_data). */
#define IBS_OPDATA_RETURN      34 /* op retired a return                     */
#define IBS_OPDATA_BRN_TAKEN   35 /* the retired branch was taken           */
#define IBS_OPDATA_BRN_MISP    36 /* the retired branch was mispredicted    */
#define IBS_OPDATA_BRN_RET     37 /* this op retired a branch               */
#define IBS_OPDATA_RIP_INVALID 38 /* the tagged RIP is invalid: drop it     */

/* --- IBS-Fetch raw record layout (Phase 7 front-end lane) ---------------------- */
/* Same [u32 caps][u64 regs...] framing; the fetch record carries three registers:
 * reg[0]=IbsFetchCtl, reg[1]=IbsFetchLinAd, reg[2]=IbsFetchPhysAd. */
#define IBS_FETCH_REG_CTL   0 /* IbsFetchCtl: fetch status bitfield (below)  */
#define IBS_FETCH_REG_LINAD 1 /* IbsFetchLinAd: fetched linear instr address */
/* Smallest payload that carries the fetch linear-address register (reg[1]): the
 * physical-address reg[2] is unused, so decode needs only through reg[1]: 4+8*2=20. */
#define IBS_FETCH_RAW_MIN_BYTES                                                \
    (IBS_RAW_CAPS_BYTES + 8u * (IBS_FETCH_REG_LINAD + 1u))
/* IbsFetchCtl (reg[0]) fields (kernel union ibs_fetch_ctl). */
#define IBS_FETCHCTL_LAT_SHIFT  32 /* IbsFetchLat: bits [47:32]           */
#define IBS_FETCHCTL_LAT_MASK   0xFFFFu
#define IBS_FETCHCTL_VAL        49 /* IbsFetchVal: sample valid           */
#define IBS_FETCHCTL_COMP       50 /* IbsFetchComp: fetch completed       */
#define IBS_FETCHCTL_IC_MISS    51 /* IbsIcMiss: i-cache miss             */
#define IBS_FETCHCTL_L1TLB_MISS 55 /* IbsL1TlbMiss: L1 ITLB miss          */

static uint64_t ld_u64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, sizeof v);
    return v;
}
static uint32_t ld_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}
static int bit(uint64_t v, unsigned b) { return (int)((v >> b) & 1u); }

/* ---- Pure decoder + free: defined for ALL platforms (no perf, no hardware) ---- */

int asmtest_ibs_decode_op(const void *raw, size_t raw_len,
                          asmtest_ibs_edge_t *out) {
    if (raw == NULL || out == NULL)
        return ASMTEST_IBS_EINVAL;
    memset(out, 0, sizeof *out);
    /* Need the caps word through the branch-target register (reg[7]). A record
     * shorter than that cannot hold reg[7] — no edge is derivable. Length ALONE
     * cannot say WHICH register reg[7] is; the caps word (below) does that. */
    if (raw_len < IBS_RAW_MIN_BYTES)
        return ASMTEST_IBS_EDECODE;

    const uint8_t *p = (const uint8_t *)raw;
    /* The record's OWN caps word (a copy of the kernel's ibs_caps). Length alone
     * CANNOT identify reg[7]: the kernel appends IbsBrTarget (cap bit 5) then
     * IbsOpData4 (cap bit 10) positionally and cap-conditionally, so
     * BRNTRGT=0/OPDATA4=1 is byte-identical in length (68) to
     * BRNTRGT=1/OPDATA4=0 with a different reg[7]. Because BrTarget is appended
     * strictly FIRST, bit 5 set forces reg[7] = IbsBrTarget — verified against
     * Linux v6.12 arch/x86/events/amd/ibs.c:1084-1099 and v6.14 :1165-1181.
     * The live path is additionally protected by ibs_probe()'s CPUID bit-5
     * gate, but this exported decoder is documented host-independent and can be
     * fed foreign/synthetic records, so it must not rely on that. A shipping
     * part with OpData4 set and BrnTrgt clear is LIKELY nonexistent (reasoned
     * from family docs, not proven — AMD primary source for bits 8-10 was
     * unobtainable); do not claim "cannot happen". Calibration: perf's own
     * amd-sample-raw.c:192-216 reads *(rip+6) with NO caps check and no length
     * floor — this decoder is deliberately stricter than upstream convention. */
    uint32_t caps = ld_u32(p);
    if (!(caps & (1u << 5))) /* BrnTrgt: reg[7] is not a branch target */
        return ASMTEST_IBS_EDECODE;

    uint64_t rip = ld_u64(p + IBS_RAW_REG_OFF(IBS_REG_RIP));
    uint64_t data = ld_u64(p + IBS_RAW_REG_OFF(IBS_REG_DATA));
    uint64_t tgt = ld_u64(p + IBS_RAW_REG_OFF(IBS_REG_BRTGT));

    int brn_ret = bit(data, IBS_OPDATA_BRN_RET);
    int brn_taken = bit(data, IBS_OPDATA_BRN_TAKEN);
    /* IbsOpData bit 38 (RipInvalid) is architecturally defined only when CPUID
     * Fn8000_001B_EAX[7] (RipInvalidChk) is set; the kernel consults it only
     * behind that cap (v6.12 ibs.c:1068). Every family that sets BrnTrgt=1 also
     * documents RipInvalidChk=1 (review §4 row 3, BKDG citations) — LIKELY
     * redundant here, but the caps word is already in hand and a reserved bit
     * must not drop samples. */
    int rip_invalid =
        (caps & (1u << 7)) ? bit(data, IBS_OPDATA_RIP_INVALID) : 0;

    /* A control-flow edge exists only for a retired, TAKEN branch with a valid RIP.
     * A not-taken conditional falls through (no target); a non-branch op has none. */
    if (!brn_ret || !brn_taken || rip_invalid)
        return ASMTEST_IBS_NOEDGE;

    out->from = rip;
    out->to = tgt;
    out->count = 1;
    out->taken = 1;
    out->mispred = (unsigned)bit(data, IBS_OPDATA_BRN_MISP);
    out->is_return = (unsigned)bit(data, IBS_OPDATA_RETURN);
    return ASMTEST_IBS_OK;
}

void asmtest_ibs_survey_free(asmtest_ibs_survey_t *s) {
    if (s == NULL)
        return;
    free(s->edges);
    memset(s, 0, sizeof *s);
}

/* Pure like survey_free above: defined for every platform, so a caller can
 * unconditionally free what the (possibly stubbed) survey returned. */
void asmtest_swclock_survey_free(asmtest_swclock_survey_t *s) {
    if (s == NULL)
        return;
    free(s->ips);
    memset(s, 0, sizeof *s);
}

/* ---- Phase 6: edge -> basic-block normalization (pure, host-independent) ------- */
/* Ascending by block-start offset so the output is a deterministic, deduplicated
 * block set (the merge pass below relies on equal starts being adjacent). */
static int ibs_blk_cmp_asc(const void *a, const void *b) {
    const asmtest_ibs_block_t *x = (const asmtest_ibs_block_t *)a;
    const asmtest_ibs_block_t *y = (const asmtest_ibs_block_t *)b;
    if (x->start != y->start)
        return x->start < y->start ? -1 : 1;
    return 0;
}

int asmtest_ibs_normalize_blocks(const asmtest_ibs_survey_t *survey,
                                 uint64_t base, uint64_t len,
                                 asmtest_ibs_blocks_t *out) {
    if (out == NULL)
        return ASMTEST_IBS_EINVAL;
    memset(out, 0, sizeof *out);
    if (survey == NULL)
        return ASMTEST_IBS_EINVAL;
    if (survey->n == 0 || survey->edges == NULL)
        return ASMTEST_IBS_OK; /* nothing sampled: a valid, empty block set */

    /* Every branch TARGET is a basic-block leader — the same normalization the exact
     * AMD-LBR / native tiers apply with trace_append_block(to - base_ip). A region
     * [base, base+len) filters to in-region targets and reports region-relative
     * OFFSETS (offset 0 = base entry), so these line up with the exact blocks[];
     * len == 0 keeps every target as an absolute address. */
    asmtest_ibs_block_t *arr =
        (asmtest_ibs_block_t *)calloc(survey->n, sizeof *arr);
    if (arr == NULL)
        return ASMTEST_IBS_EUNAVAIL; /* OOM: the one allocation-failure signal here */
    size_t m = 0;
    for (size_t i = 0; i < survey->n; i++) {
        uint64_t to = survey->edges[i].to;
        if (len != 0 && (to < base || to - base >= len))
            continue; /* target outside this routine's region: drop it */
        arr[m].start = (len != 0) ? (to - base) : to;
        arr[m].entries = survey->edges[i].count;
        m++;
    }
    if (m == 0) { /* no in-region targets */
        free(arr);
        return ASMTEST_IBS_OK;
    }

    qsort(arr, m, sizeof *arr, ibs_blk_cmp_asc);

    /* Merge duplicate starts (a block reached by several distinct sampled edges),
     * summing their entry counts — the DISTINCT block set with hotness preserved. */
    size_t w = 0;
    for (size_t r = 0; r < m; r++) {
        if (w > 0 && arr[w - 1].start == arr[r].start)
            arr[w - 1].entries += arr[r].entries;
        else
            arr[w++] = arr[r];
    }

    out->blocks = arr;
    out->n = w;
    return ASMTEST_IBS_OK;
}

void asmtest_ibs_blocks_free(asmtest_ibs_blocks_t *b) {
    if (b == NULL)
        return;
    free(b->blocks);
    memset(b, 0, sizeof *b);
}

/* ---- Pure IBS-Fetch decoder + free: all platforms (no perf, no hardware) ------ */

int asmtest_ibs_decode_fetch(const void *raw, size_t raw_len,
                             asmtest_ibs_fetch_sample_t *out) {
    if (raw == NULL || out == NULL)
        return ASMTEST_IBS_EINVAL;
    memset(out, 0, sizeof *out);
    /* Need the caps word through the fetch linear-address register (reg[1]); a
     * record shorter than that is truncated — no fetch address is derivable. */
    if (raw_len < IBS_FETCH_RAW_MIN_BYTES)
        return ASMTEST_IBS_EDECODE;

    const uint8_t *p = (const uint8_t *)raw;
    uint64_t ctl = ld_u64(p + IBS_RAW_REG_OFF(IBS_FETCH_REG_CTL));
    uint64_t linad = ld_u64(p + IBS_RAW_REG_OFF(IBS_FETCH_REG_LINAD));

    /* Decode every status field regardless of validity so a caller can inspect the
     * record; the return code carries whether the fetch tag is usable. */
    out->fetch_addr = linad;
    out->valid = (unsigned)bit(ctl, IBS_FETCHCTL_VAL);
    out->complete = (unsigned)bit(ctl, IBS_FETCHCTL_COMP);
    out->icache_miss = (unsigned)bit(ctl, IBS_FETCHCTL_IC_MISS);
    out->itlb_miss = (unsigned)bit(ctl, IBS_FETCHCTL_L1TLB_MISS);
    out->latency =
        (unsigned)((ctl >> IBS_FETCHCTL_LAT_SHIFT) & IBS_FETCHCTL_LAT_MASK);

    /* IbsFetchVal set => the fetch tag (and its linear address) is meaningful and
     * belongs in the coverage histogram; clear => decoded, nothing to aggregate. */
    return out->valid ? ASMTEST_IBS_OK : ASMTEST_IBS_NOEDGE;
}

void asmtest_ibs_fetch_survey_free(asmtest_ibs_fetch_survey_t *s) {
    if (s == NULL)
        return;
    free(s->hot);
    memset(s, 0, sizeof *s);
}

/* ---- Sample-period + jitter resolution (PURE: no perf, no hardware) ------------ */
/* Kept ABOVE the platform #if so the INTERNAL test seams below and the live path's
 * ibs_cfg_from_opts share ONE implementation — the tested code IS the shipped code. */

/* Dispatched-op count between samples (a multiple of 16). */
#define IBS_DEFAULT_PERIOD 0x4000u
/* Default sample-period jitter magnitude: +/- period/IBS_JITTER_FRAC per sample,
 * re-rolled each drain tick, to de-alias against a periodic loop's own period. */
#define IBS_JITTER_FRAC 8u

/* Resolve the effective sample period from opts (rounded to IBS's /16 granularity;
 * a sub-16 result clamps to the default). */
static uint64_t ibs_period(const asmtest_ibs_opts_t *opts) {
    uint64_t period = (opts != NULL && opts->sample_period != 0)
                          ? opts->sample_period
                          : IBS_DEFAULT_PERIOD;
    period &= ~(uint64_t)0xF; /* IBS max-count granularity is 16 */
    if (period < 16)
        period = IBS_DEFAULT_PERIOD;
    return period;
}

/* Resolve the effective jitter fraction (0 => no jitter). Default IBS_JITTER_FRAC;
 * ASMTEST_IBS_OPT_NO_JITTER forces 0; a caller-set period_jitter is honoured only
 * when struct_size proves the caller compiled with that appended field (the
 * additive-ABI guard) — a legacy caller whose struct_size does not cover the tail
 * keeps the default. */
static unsigned ibs_jitter_frac(const asmtest_ibs_opts_t *opts) {
    unsigned flags = (opts != NULL) ? opts->flags : 0u;
    if (flags & ASMTEST_IBS_OPT_NO_JITTER)
        return 0;
    if (opts != NULL &&
        opts->struct_size >= offsetof(asmtest_ibs_opts_t, period_jitter) +
                                 sizeof opts->period_jitter &&
        opts->period_jitter != 0)
        return opts->period_jitter;
    return IBS_JITTER_FRAC;
}

/* INTERNAL test seams (pure, no ABI): the resolved period and jitter fraction
 * EXACTLY as the live attr fill uses them. */
uint64_t asmtest_ibs_effective_period(const asmtest_ibs_opts_t *opts) {
    return ibs_period(opts);
}
unsigned asmtest_ibs_effective_jitter(const asmtest_ibs_opts_t *opts) {
    return ibs_jitter_frac(opts);
}

/* ------------------------------ live capture ---------------------------------- */
#if defined(__linux__) && defined(__x86_64__)

#include <cpuid.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* IBS-Op attr.config bit 19 = cnt_ctl: 1 => count DISPATCHED OPS between samples
 * (the Phase-5 default: more uniform instruction coverage than cycle counting),
 * 0 => count clock cycles. Only set when the OpCnt capability (EAX bit 4) is present.
 * (IBS_DEFAULT_PERIOD / IBS_JITTER_FRAC now live in the pure section above, shared
 * with the effective_period / effective_jitter test seams.) */
#define IBS_OP_CNT_CTL_BIT 19
/* 256 KiB data ring (64 * 4 KiB pages). */
#define IBS_RING_DATA_PAGES 64u
/* Largest single record we parse WITHOUT callchain: header + IP + TID + RAW(size +
 * caps + 8 regs) + 16 slack. Still valid for every no-callchain lane, including a
 * 9-register BrTarget+OpData4 record (true size 104). */
#define IBS_MAX_RECORD                                                         \
    (sizeof(struct perf_event_header) + 8u + 8u + 4u + IBS_RAW_MIN_BYTES + 16u)
/* Pin the kernel's default (PERF_MAX_STACK_DEPTH) explicitly: the sysctl is
 * tunable to 640K frames, so an unpinned open makes ANY fixed bound unsound.
 * On a host tuned BELOW this the open fails -EOVERFLOW — a loud, reported
 * failure (unavail_reason) instead of silent ring loss: the right trade. */
#define IBS_CALLCHAIN_MAX_STACK 127u
/* Worst-case callchain record: header+ip+tid (24) + (1+nr)*8 with
 * nr <= max_stack + 8 context markers + RAW wire 80 (9 regs, u64-padded)
 * + the same 16-byte slack as the base bound. (= 1208.) */
#define IBS_MAX_RECORD_CALLCHAIN                                               \
    (24u + 8u * (1u + IBS_CALLCHAIN_MAX_STACK + 8u) + 80u + 16u)

static long perf_open(struct perf_event_attr *a, pid_t pid, int cpu, int group,
                      unsigned long flags) {
    return syscall(SYS_perf_event_open, a, pid, cpu, group, flags);
}

/* The largest single record a lane can produce: the callchain worst case when the
 * lane enables PERF_SAMPLE_CALLCHAIN, else the base bound. The near-full-ring loss
 * heuristic must reserve this much headroom or callchain-sized records vanish with
 * lost==0 && throttled==0. */
static size_t ibs_max_record(int has_callchain) {
    return has_callchain ? IBS_MAX_RECORD_CALLCHAIN : IBS_MAX_RECORD;
}

/* INTERNAL test seam (no ABI): the max-record bound the drain uses, exposed so a
 * pure test can pin the callchain-aware sizing without opening perf. */
size_t asmtest_ibs_max_record(int has_callchain) {
    return ibs_max_record(has_callchain);
}

/* --- capability probe (cached) ------------------------------------------------- */

/* ibs_op PMU type from sysfs (dynamic, must be read at runtime), or -1 if absent. */
static int ibs_op_type(void) {
    static int cached = -2; /* -2 unread, -1 absent, >=0 type */
    if (cached != -2)
        return cached;
    cached = -1;
    FILE *f = fopen("/sys/bus/event_source/devices/ibs_op/type", "r");
    if (f != NULL) {
        int t = -1;
        if (fscanf(f, "%d", &t) == 1 && t >= 0)
            cached = t;
        fclose(f);
    }
    return cached;
}

static int is_amd(void) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(0, &a, &b, &c, &d))
        return 0;
    /* "AuthenticAMD" = ebx 'htuA', edx 'itne', ecx 'DMAc'. */
    return b == 0x68747541u && d == 0x69746e65u && c == 0x444d4163u;
}

/* CPUID Fn8000_001B EAX (IBS capabilities), or 0 if the leaf is unsupported. */
static unsigned ibs_caps_eax(void) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(0x80000000u, &a, &b, &c, &d) || a < 0x8000001Bu)
        return 0;
    if (!__get_cpuid_count(0x8000001Bu, 0, &a, &b, &c, &d))
        return 0;
    return a;
}

static int g_avail = -1;
static const char *g_reason = "";
/* errno from the last failed perf_event_open — see asmtest_ibs_unavail_reason.
 * Not cached/memoised like g_avail: it describes the last ATTEMPT, not the host. */
static int g_open_errno = 0;

static void ibs_probe(void) {
    if (g_avail >= 0)
        return;
    if (!is_amd()) {
        g_avail = 0;
        g_reason = "not AMD";
        return;
    }
    unsigned caps = ibs_caps_eax();
    if (caps == 0) {
        g_avail = 0;
        g_reason = "no IBS (CPUID 8000_001B absent)";
        return;
    }
    /* IBSFFV (bit 0): feature flags valid. When clear the kernel substitutes
     * IBS_CAPS_DEFAULT (AVAIL|FETCHSAM|OPSAM, BrnTrgt CLEAR), so our CPUID bit-5
     * read would disagree with the caps the kernel actually samples with (plausible
     * under VM CPUID synthesis) — records would be 60 bytes and every decode
     * EDECODE. Refuse rather than advertise a lane that produces no edges. */
    if (!(caps & (1u << 0))) {
        g_avail = 0;
        g_reason = "IBS feature flags not valid (CPUID 8000_001B EAX[0] clear)";
        return;
    }
    if (!(caps & (1u << 2))) { /* OpSam */
        g_avail = 0;
        g_reason = "no IBS op sampling (OpSam)";
        return;
    }
    if (!(caps & (1u << 5))) { /* BrnTrgt */
        g_avail = 0;
        g_reason = "no IBS branch target (BrnTrgt)";
        return;
    }
    if (ibs_op_type() < 0) {
        g_avail = 0;
        g_reason = "no ibs_op PMU";
        return;
    }
    /* `swfilt` (config2:0) is what lets exclude_kernel work at paranoid=2, i.e. the
     * whole point: user-only sampling with no privilege. Present since ~6.2. */
    if (access("/sys/bus/event_source/devices/ibs_op/format/swfilt", F_OK) !=
        0) {
        g_avail = 0;
        g_reason = "no swfilt (kernel < ~6.2)";
        return;
    }
    g_avail = 1;
    g_reason = "";
}

int asmtest_ibs_available(void) {
    ibs_probe();
    return g_avail;
}
const char *asmtest_ibs_skip_reason(void) {
    ibs_probe();
    return g_avail == 1 ? "" : g_reason;
}

const char *asmtest_ibs_unavail_reason(void) {
    ibs_probe();
    if (g_avail != 1)
        return g_reason; /* substrate genuinely absent — that IS the reason */
    /* Substrate present. If a capture has failed, the errno is the whole story;
     * skip_reason() returns "" here by construction. */
    switch (g_open_errno) {
    case 0:
        return "";
    case EACCES:
    case EPERM:
        return "perf_event_open refused (EACCES): IBS is present but perf is "
               "locked down — needs perf_event_paranoid<=2 or CAP_PERFMON "
               "(in Docker: --cap-add=PERFMON --security-opt "
               "seccomp=unconfined)";
    case EINVAL:
        return "perf_event_open rejected the IBS attr (EINVAL)";
    case EMFILE:
    case ENFILE:
        return "perf_event_open hit the open-file limit (EMFILE)";
    case ENOENT:
        return "perf_event_open: no such PMU (ENOENT)";
    case ESRCH:
        return "perf_event_open: target thread exited (ESRCH)";
    case ENOSPC:
        return "perf_event_open: no free PMU counter (ENOSPC)";
    default:
        return "perf_event_open failed";
    }
}

/* --- edge aggregation (open-addressing hash keyed on {from,to}) ---------------- */

typedef struct {
    uint64_t from, to, count;
    unsigned mispred, is_return;
    int used;
} eh_slot;
typedef struct {
    eh_slot *slots;
    size_t cap; /* power of two */
    size_t n;
} edge_hash;

static int eh_init(edge_hash *h, size_t cap) {
    h->slots = (eh_slot *)calloc(cap, sizeof *h->slots);
    if (h->slots == NULL)
        return -1;
    h->cap = cap;
    h->n = 0;
    return 0;
}
static void eh_free(edge_hash *h) {
    free(h->slots);
    h->slots = NULL;
    h->cap = h->n = 0;
}
static size_t eh_hash(uint64_t from, uint64_t to, size_t mask) {
    uint64_t k = from * 0x9E3779B97F4A7C15ull;
    k ^= (to + 0x9E3779B97F4A7C15ull + (k << 6) + (k >> 2));
    return (size_t)(k ^ (k >> 29)) & mask;
}
static int eh_grow(edge_hash *h); /* fwd */

/* Add one sampled edge. Aggregates duplicates; grows past a 0.7 load factor. */
static void eh_add(edge_hash *h, uint64_t from, uint64_t to, unsigned mispred,
                   unsigned is_return) {
    if (h->slots == NULL)
        return;
    if ((h->n + 1) * 10 >= h->cap * 7) {
        if (eh_grow(h) != 0)
            return; /* OOM: drop the sample rather than crash (survey is a prefix) */
    }
    size_t mask = h->cap - 1;
    size_t i = eh_hash(from, to, mask);
    for (;;) {
        eh_slot *s = &h->slots[i];
        if (!s->used) {
            s->used = 1;
            s->from = from;
            s->to = to;
            s->count = 1;
            s->mispred = mispred;
            s->is_return = is_return;
            h->n++;
            return;
        }
        if (s->from == from && s->to == to) {
            s->count++;
            s->mispred += mispred;
            s->is_return += is_return;
            return;
        }
        i = (i + 1) & mask;
    }
}
static int eh_grow(edge_hash *h) {
    edge_hash bigger;
    size_t nc; /* overflow-checked h->cap * 2 (S6) */
    if (!asmtest_grow_pow2(h->cap, h->cap + 1, sizeof *h->slots, &nc))
        return -1;
    if (eh_init(&bigger, nc) != 0)
        return -1;
    for (size_t i = 0; i < h->cap; i++) {
        eh_slot *s = &h->slots[i];
        if (!s->used)
            continue;
        /* re-insert (counts preserved) without re-triggering a grow */
        size_t mask = bigger.cap - 1;
        size_t j = eh_hash(s->from, s->to, mask);
        while (bigger.slots[j].used)
            j = (j + 1) & mask;
        bigger.slots[j] = *s;
        bigger.n++;
    }
    eh_free(h);
    *h = bigger;
    return 0;
}
static int eh_cmp_desc(const void *a, const void *b) {
    const asmtest_ibs_edge_t *x = (const asmtest_ibs_edge_t *)a;
    const asmtest_ibs_edge_t *y = (const asmtest_ibs_edge_t *)b;
    if (x->count != y->count)
        return x->count < y->count ? 1 : -1; /* descending by count */
    /* stable-ish tiebreak so output is deterministic across runs */
    if (x->from != y->from)
        return x->from < y->from ? -1 : 1;
    if (x->to != y->to)
        return x->to < y->to ? -1 : 1;
    return 0;
}
/* Export used slots into a freshly-malloc'd array sorted by descending count. */
static int eh_export(edge_hash *h, asmtest_ibs_survey_t *out) {
    if (h->n == 0) {
        out->edges = NULL;
        out->n = 0;
        return 0;
    }
    asmtest_ibs_edge_t *arr = (asmtest_ibs_edge_t *)calloc(h->n, sizeof *arr);
    if (arr == NULL)
        return -1;
    size_t k = 0;
    for (size_t i = 0; i < h->cap && k < h->n; i++) {
        eh_slot *s = &h->slots[i];
        if (!s->used)
            continue;
        arr[k].from = s->from;
        arr[k].to = s->to;
        arr[k].count = s->count;
        arr[k].taken = 1;
        arr[k].mispred = s->mispred;
        arr[k].is_return = s->is_return;
        k++;
    }
    qsort(arr, k, sizeof *arr, eh_cmp_desc);
    out->edges = arr;
    out->n = k;
    return 0;
}

/* --- ring drain ---------------------------------------------------------------- */

/* Locate the PERF_SAMPLE_RAW payload inside a sample record whose body layout is
 * IP|TID|[CALLCHAIN]|RAW (has_callchain selects the optional callchain block, which
 * precedes RAW). On success returns a pointer to the raw payload and fills *rawsz;
 * returns NULL if the record is too short / malformed. The sample's pid
 * (PERF_SAMPLE_TID) is filled when present, so a system-wide drain can filter on it
 * before decoding. */
static const uint8_t *ibs_sample_raw(const uint8_t *rec, size_t recsz,
                                     int has_callchain, uint32_t *pid_out,
                                     uint32_t *rawsz_out) {
    size_t p = sizeof(struct perf_event_header);
    if (p + 8 + 8 > recsz) /* u64 ip; u32 pid, tid */
        return NULL;
    if (pid_out != NULL)
        *pid_out = ld_u32(rec + p + 8); /* pid precedes tid */
    p += 8 + 8;
    if (has_callchain) {
        if (p + 8 > recsz) /* u64 nr */
            return NULL;
        uint64_t nr = ld_u64(rec + p);
        p += 8;
        if (nr > (recsz - p) / 8) /* nr * u64 ips[] must fit in the record */
            return NULL;
        p += (size_t)nr * 8;
    }
    if (p + 4 > recsz) /* u32 raw_size */
        return NULL;
    uint32_t rawsz = ld_u32(rec + p);
    p += 4;
    if ((size_t)rawsz > recsz - p) /* raw payload must fit inside this record */
        return NULL;
    if (rawsz_out != NULL)
        *rawsz_out = rawsz;
    return rec + p;
}

/* Copy [tail,head) out of the (possibly wrapping) non-overwrite ring into `scratch`
 * (dsz bytes), parse each record, aggregate edges, advance data_tail. has_callchain
 * selects the IP|TID|CALLCHAIN|RAW body layout; filter_pid (nonzero) keeps only
 * samples from that pid — the system-wide per-CPU capture aggregates the target only. */
static void ibs_drain(void *base_map, size_t pg, size_t dsz, uint8_t *scratch,
                      edge_hash *h, asmtest_ibs_survey_t *out,
                      int has_callchain, pid_t filter_pid) {
    struct perf_event_mmap_page *mp = (struct perf_event_mmap_page *)base_map;
    uint8_t *data = (uint8_t *)base_map + pg;
    uint64_t head = mp->data_head;
    __sync_synchronize(); /* read data_head before the records (smp_rmb) */
    uint64_t tail = mp->data_tail;
    size_t span = (size_t)(head - tail);
    if (span == 0)
        return;
    if (span > dsz)
        span = dsz; /* defensive: never over-read the scratch buffer */
    /* Ring near-full = the newest samples may have been dropped with NO
     * PERF_RECORD_LOST (a non-overwrite ring stops reserving at the tail). Treat
     * less than one max record of headroom as loss (mirrors hwtrace.c). The bound
     * is callchain-aware: a callchain-enabled record is ~10x the base size, so the
     * base bound would under-reserve and callchain samples would vanish silently. */
    if (span + ibs_max_record(has_callchain) > dsz)
        out->throttled = 1;

    for (size_t i = 0; i < span; i++)
        scratch[i] = data[(tail + i) % dsz];

    for (size_t off = 0; off + sizeof(struct perf_event_header) <= span;) {
        struct perf_event_header hd;
        memcpy(&hd, scratch + off, sizeof hd);
        if (hd.size == 0 || off + hd.size > span)
            break;
        if (hd.type == PERF_RECORD_SAMPLE) {
            uint32_t pid = 0, rawsz = 0;
            const uint8_t *raw = ibs_sample_raw(scratch + off, hd.size,
                                                has_callchain, &pid, &rawsz);
            /* System-wide (per-CPU) capture sees every process; keep the target's. */
            if (filter_pid == 0 || (pid_t)pid == filter_pid) {
                out->samples++;
                if (raw != NULL) {
                    asmtest_ibs_edge_t e;
                    if (asmtest_ibs_decode_op(raw, rawsz, &e) ==
                        ASMTEST_IBS_OK) {
                        out->branch_samples++;
                        eh_add(h, e.from, e.to, e.mispred, e.is_return);
                    }
                }
            }
        } else if (hd.type == PERF_RECORD_LOST) {
            /* body: {u64 id; u64 lost} */
            if (hd.size >= sizeof hd + 16)
                out->lost += ld_u64(scratch + off + sizeof hd + 8);
            else
                out->lost += 1;
        } else if (hd.type == PERF_RECORD_THROTTLE) {
            out->throttled = 1;
        }
        off += hd.size;
    }

    __sync_synchronize(); /* publish reads before advancing data_tail (smp_mb) */
    mp->data_tail = tail + span;
}

static uint64_t elapsed_ms(const struct timespec *t0,
                           const struct timespec *t1) {
    return (uint64_t)(t1->tv_sec - t0->tv_sec) * 1000ull +
           (uint64_t)(t1->tv_nsec - t0->tv_nsec) / 1000000ull;
}

/* --- per-thread capture channel (one perf event + its mmap ring) --------------- */
/* Both surveys are built from these: survey_pid drives one channel, survey_process
 * a vector of them (one per thread). Keeping the open/drain/teardown identical means
 * whole-process capture is exactly single-thread capture, merged. */

#define IBS_MAX_CHANS                                                          \
    512 /* per-thread events; caps fd/mmap use on huge targets */

/* Does this IBS support dispatched-op count control (cnt_ctl / OpCnt, EAX bit 4)? */
static int ibs_has_opcnt(void) { return (ibs_caps_eax() & (1u << 4)) != 0; }

/* Resolved capture configuration, derived once from the public opts (which grows
 * additively). Threaded through the open/drain machinery so the Op survey, the
 * whole-process survey, and the window primitive share one control flow. */
typedef struct {
    uint64_t period; /* base sample period (rounded /16)                   */
    unsigned
        jitter_frac;    /* 0 => no jitter; else +/- period/jitter_frac        */
    int cnt_dispatched; /* set cnt_ctl=1 (config:19) => count dispatched ops  */
    int callchain;      /* PERF_SAMPLE_CALLCHAIN caller stack per sample      */
    int system_wide;    /* per-CPU capture (pid=-1), filtered by pid in drain */
} ibs_cfg;

/* Derive the resolved config from the public opts (NULL => all defaults). The two
 * legacy fields (sample_period, flags) are read for any non-NULL opts; the appended
 * period_jitter is read only when struct_size covers it (additive-ABI guard). */
static void ibs_cfg_from_opts(const asmtest_ibs_opts_t *opts, ibs_cfg *c) {
    memset(c, 0, sizeof *c);
    /* Period + jitter go through the EXACT same effective_* seams the pure test
     * calls: the tested code IS the shipped code. */
    c->period = asmtest_ibs_effective_period(opts);
    c->jitter_frac = asmtest_ibs_effective_jitter(opts);
    /* Phase-5 default (apply even to NULL / legacy callers): dispatched-op count
     * control where the silicon supports it. */
    c->cnt_dispatched = ibs_has_opcnt();
    unsigned flags = (opts != NULL) ? opts->flags : 0u;
    if (flags & ASMTEST_IBS_OPT_COUNT_CYCLES)
        c->cnt_dispatched = 0;
    if (flags & ASMTEST_IBS_OPT_CALLCHAIN)
        c->callchain = 1;
    if (flags & ASMTEST_IBS_OPT_SYSTEM_WIDE)
        c->system_wide = 1;
}

/* A cheap self-contained xorshift64 for period jitter (seeded lazily from the
 * clock). Not cryptographic — jitter only needs to break periodicity. */
static uint64_t ibs_rand(void) {
    static uint64_t s;
    if (s == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        s = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 32) ^
            0x9E3779B97F4A7C15ull;
        if (s == 0)
            s = 0x9E3779B97F4A7C15ull;
    }
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

/* A jittered sample period: base +/- base/frac, kept a positive multiple of 16. */
static uint64_t ibs_jitter_period(uint64_t base, unsigned frac) {
    if (frac == 0)
        return base;
    uint64_t amp = base / frac;
    if (amp == 0)
        return base;
    int64_t off = (int64_t)(ibs_rand() % (2u * amp + 1u)) - (int64_t)amp;
    int64_t p = (int64_t)base + off;
    if (p < 16)
        p = 16;
    uint64_t period = (uint64_t)p & ~(uint64_t)0xF;
    if (period < 16)
        period = 16;
    return period;
}

/* Fill the IBS perf attr: user-only sampling (swfilt + exclude_kernel), IP|TID|RAW
 * records, initially disabled. cfg carries the Op-only knobs (cnt_ctl, callchain);
 * a fetch caller passes a cfg with those clear so the shared opener stays generic. */
static void ibs_fill_attr(struct perf_event_attr *a, int type,
                          const ibs_cfg *cfg) {
    memset(a, 0, sizeof *a);
    a->size = sizeof *a;
    a->type = (uint32_t)type;
    a->sample_period = cfg->period;
    a->config2 = 1; /* swfilt (config2:0): enables exclude_kernel at p=2 */
    if (cfg->cnt_dispatched) /* cnt_ctl=1: sample per dispatched op */
        a->config |= (1ull << IBS_OP_CNT_CTL_BIT);
    a->exclude_kernel = 1; /* user-only — the unprivileged envelope */
    a->sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_RAW;
    if (cfg->callchain) {
        a->sample_type |= PERF_SAMPLE_CALLCHAIN;
        a->exclude_callchain_kernel = 1; /* user-only caller stack */
        /* Pin the stack depth so the max-record bound is sound: without this the
         * sysctl (tunable to 640K frames) makes any fixed bound unsafe. A host
         * tuned BELOW this fails the open -EOVERFLOW — loud, not silent loss. */
        a->sample_max_stack = IBS_CALLCHAIN_MAX_STACK;
    }
    a->disabled = 1;
}

static size_t ibs_page(void) {
    long pg = sysconf(_SC_PAGESIZE);
    return pg > 0 ? (size_t)pg : 4096u;
}

typedef struct {
    long fd;        /* perf event fd, or -1                */
    void *base_map; /* mmap base (header page + data ring) */
    size_t base_sz; /* mmap length                         */
    pid_t tid;      /* the thread this channel samples     */
} ibs_chan;

/* Open + mmap + enable one IBS channel on (`tid`, `cpu`) — tid 0 => the calling
 * thread, cpu -1 => any CPU; a system-wide channel is (tid -1, cpu k). Returns 0 on
 * success; -1 if it cannot be attached (the thread exited, or open/mmap/enable
 * failed) — the caller skips it rather than aborting the whole survey. */
static int ibs_chan_open(ibs_chan *ch, pid_t tid, int cpu, int type,
                         const ibs_cfg *cfg, size_t pg, size_t dsz) {
    ch->fd = -1;
    ch->base_map = MAP_FAILED;
    ch->base_sz = 0;
    ch->tid = tid;
    struct perf_event_attr a;
    ibs_fill_attr(&a, type, cfg);
    if (cfg->jitter_frac) /* jitter the opening period too, not just per tick */
        a.sample_period = ibs_jitter_period(cfg->period, cfg->jitter_frac);
    long fd = perf_open(&a, tid, cpu, -1, 0);
    if (fd < 0) {
        /* Record WHY. Every caller collapses this to ASMTEST_IBS_EUNAVAIL, and
         * asmtest_ibs_skip_reason() answers only the detect chain — which has
         * PASSED whenever we get here — so without this the operator is told
         * "# SKIP --sample: " with an empty reason on exactly the host where the
         * reason matters (AMD, IBS present, perf locked down). */
        g_open_errno = errno;
        return -1;
    }
    size_t base_sz = pg + dsz;
    void *m =
        mmap(NULL, base_sz, PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
    if (m == MAP_FAILED) {
        close((int)fd);
        return -1;
    }
    ioctl((int)fd, PERF_EVENT_IOC_RESET, 0);
    if (ioctl((int)fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        munmap(m, base_sz);
        close((int)fd);
        return -1;
    }
    ch->fd = fd;
    ch->base_map = m;
    ch->base_sz = base_sz;
    return 0;
}

static void ibs_chan_disable(ibs_chan *ch) {
    if (ch->fd >= 0)
        ioctl((int)ch->fd, PERF_EVENT_IOC_DISABLE, 0);
}
/* Roll a fresh jittered period onto a live channel (a no-op without jitter). Called
 * each drain tick so a long single-thread window keeps varying its sample phase. */
static void ibs_chan_rejitter(ibs_chan *ch, const ibs_cfg *cfg) {
    if (ch->fd < 0 || cfg->jitter_frac == 0)
        return;
    uint64_t p = ibs_jitter_period(cfg->period, cfg->jitter_frac);
    ioctl((int)ch->fd, PERF_EVENT_IOC_PERIOD, &p);
}
static void ibs_chan_free(ibs_chan *ch) {
    if (ch->base_map != MAP_FAILED)
        munmap(ch->base_map, ch->base_sz);
    if (ch->fd >= 0)
        close((int)ch->fd);
    ch->fd = -1;
    ch->base_map = MAP_FAILED;
}

/* Read up to `max` thread ids of `pid` from /proc/<pid>/task into `out`. Returns the
 * count (>=0), or -1 if the task directory cannot be read (no such process). */
static int ibs_list_tids(pid_t pid, pid_t *out, int max) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task", (int)pid);
    DIR *d = opendir(path);
    if (d == NULL)
        return -1;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue; /* skip "." / ".." */
        long t = strtol(e->d_name, NULL, 10);
        if (t > 0)
            out[n++] = (pid_t)t;
    }
    closedir(d);
    return n;
}

/* Drain every channel once, then sleep a 2 ms slice — the shared inner tick. Draining
 * often keeps each 256 KiB non-overwrite ring from filling across a long window. */
static void ibs_drain_all(ibs_chan *chans, int nch, size_t pg, size_t dsz,
                          uint8_t *scratch, edge_hash *h,
                          asmtest_ibs_survey_t *out, int has_callchain,
                          pid_t filter_pid) {
    for (int i = 0; i < nch; i++)
        ibs_drain(chans[i].base_map, pg, dsz, scratch, h, out, has_callchain,
                  filter_pid);
}

int asmtest_ibs_survey_pid(pid_t tid, unsigned ms,
                           const asmtest_ibs_opts_t *opts,
                           asmtest_ibs_survey_t *out) {
    if (out == NULL)
        return ASMTEST_IBS_EINVAL;
    memset(out, 0, sizeof *out);
    if (!asmtest_ibs_available())
        return ASMTEST_IBS_EUNAVAIL;
    int type = ibs_op_type();
    if (type < 0)
        return ASMTEST_IBS_EUNAVAIL;

    ibs_cfg cfg;
    ibs_cfg_from_opts(opts, &cfg);
    size_t pg = ibs_page();
    size_t dsz = pg * IBS_RING_DATA_PAGES;

    ibs_chan ch;
    if (ibs_chan_open(&ch, tid, -1, type, &cfg, pg, dsz) != 0)
        return ASMTEST_IBS_EUNAVAIL;

    uint8_t *scratch = (uint8_t *)malloc(dsz);
    edge_hash h;
    if (scratch == NULL || eh_init(&h, 1024) != 0) {
        free(scratch);
        ibs_chan_disable(&ch);
        ibs_chan_free(&ch);
        return ASMTEST_IBS_EUNAVAIL;
    }

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        ibs_drain(ch.base_map, pg, dsz, scratch, &h, out, cfg.callchain, 0);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (elapsed_ms(&t0, &now) >= ms)
            break;
        ibs_chan_rejitter(&ch, &cfg);
        struct timespec slice = {0, 2 * 1000 * 1000}; /* 2 ms */
        nanosleep(&slice, NULL);
    }
    ibs_chan_disable(&ch);
    ibs_drain(ch.base_map, pg, dsz, scratch, &h, out, cfg.callchain,
              0); /* final drain */

    eh_export(&h, out);
    eh_free(&h);
    free(scratch);
    ibs_chan_free(&ch);
    return ASMTEST_IBS_OK;
}

int asmtest_ibs_survey_process(pid_t pid, unsigned ms,
                               const asmtest_ibs_opts_t *opts,
                               asmtest_ibs_survey_t *out) {
    if (out == NULL)
        return ASMTEST_IBS_EINVAL;
    memset(out, 0, sizeof *out);
    if (!asmtest_ibs_available())
        return ASMTEST_IBS_EUNAVAIL;
    int type = ibs_op_type();
    if (type < 0)
        return ASMTEST_IBS_EUNAVAIL;
    if (pid == 0)
        pid = getpid();

    ibs_cfg cfg;
    ibs_cfg_from_opts(opts, &cfg);
    size_t pg = ibs_page();
    size_t dsz = pg * IBS_RING_DATA_PAGES;

    ibs_chan *chans = (ibs_chan *)calloc(IBS_MAX_CHANS, sizeof *chans);
    uint8_t *scratch = (uint8_t *)malloc(dsz);
    edge_hash h;
    if (chans == NULL || scratch == NULL || eh_init(&h, 2048) != 0) {
        free(chans);
        free(scratch);
        return ASMTEST_IBS_EUNAVAIL;
    }

    /* Two coverage strategies. DEFAULT: one out-of-band event per pre-existing
     * thread, with one mid-window task/ rescan (races a thread born-and-dead inside
     * the window). SYSTEM-WIDE: one per-CPU event (pid=-1) covers all present AND
     * future threads with no race — at the cost of CAP_PERFMON / paranoid<=0 — and the
     * drain filters samples down to the target `pid`. */
    int nch = 0;
    pid_t filter_pid = 0;
    if (cfg.system_wide) {
        long ncpu = sysconf(_SC_NPROCESSORS_CONF);
        if (ncpu < 1)
            ncpu = 1;
        for (long c = 0; c < ncpu && nch < IBS_MAX_CHANS; c++) {
            if (ibs_chan_open(&chans[nch], -1, (int)c, type, &cfg, pg, dsz) ==
                0)
                nch++;
        }
        filter_pid =
            pid; /* per-CPU sees the whole system; keep the target only */
    } else {
        pid_t tids[IBS_MAX_CHANS];
        int ntid = ibs_list_tids(pid, tids, IBS_MAX_CHANS);
        if (ntid <= 0) { /* no such process / empty task list */
            eh_free(&h);
            free(scratch);
            free(chans);
            return ASMTEST_IBS_EUNAVAIL;
        }
        for (int i = 0; i < ntid; i++) {
            if (ibs_chan_open(&chans[nch], tids[i], -1, type, &cfg, pg, dsz) ==
                0)
                nch++;
        }
    }
    if (nch ==
        0) { /* every thread gone, or perf_event_open blocked/unpermitted */
        eh_free(&h);
        free(scratch);
        free(chans);
        return ASMTEST_IBS_EUNAVAIL;
    }

    /* Drain all rings for `ms`. Per-thread mode does ONE mid-window task/ rescan to
     * attach threads spawned after we started; system-wide needs none (the per-CPU
     * events already see new threads). */
    int rescanned = cfg.system_wide;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        ibs_drain_all(chans, nch, pg, dsz, scratch, &h, out, cfg.callchain,
                      filter_pid);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t el = elapsed_ms(&t0, &now);
        if (!rescanned && el >= (uint64_t)ms / 2) {
            rescanned = 1;
            if (nch < IBS_MAX_CHANS) {
                pid_t cur[IBS_MAX_CHANS];
                int m = ibs_list_tids(pid, cur, IBS_MAX_CHANS);
                for (int i = 0; i < m && nch < IBS_MAX_CHANS; i++) {
                    int known = 0;
                    for (int j = 0; j < nch; j++) {
                        if (chans[j].tid == cur[i]) {
                            known = 1;
                            break;
                        }
                    }
                    if (!known && ibs_chan_open(&chans[nch], cur[i], -1, type,
                                                &cfg, pg, dsz) == 0)
                        nch++;
                }
            }
        }
        if (el >= ms)
            break;
        for (int i = 0; i < nch; i++)
            ibs_chan_rejitter(&chans[i], &cfg);
        struct timespec slice = {0, 2 * 1000 * 1000}; /* 2 ms */
        nanosleep(&slice, NULL);
    }

    for (int i = 0; i < nch; i++)
        ibs_chan_disable(&chans[i]);
    ibs_drain_all(chans, nch, pg, dsz, scratch, &h, out, cfg.callchain,
                  filter_pid); /* final drain */

    eh_export(&h, out);
    eh_free(&h);
    free(scratch);
    for (int i = 0; i < nch; i++)
        ibs_chan_free(&chans[i]);
    free(chans);
    return ASMTEST_IBS_OK;
}

/* --- portable software-clock IP sampler ----------------------------------------- */
/* The same channel/ring plumbing pointed at PERF_TYPE_SOFTWARE instead of the
 * ibs_op PMU, so it samples on any vendor and inside VMs. The attr is built
 * FRESH here, deliberately NOT via ibs_cfg/ibs_fill_attr: that path defaults
 * cnt_dispatched from ibs_has_opcnt() and then sets attr.config bit 19
 * (IbsOpCntCtl) — and for a software event `config` IS the event id, so bit 19
 * would turn TASK_CLOCK into a nonsense id and the open hard-fails (the carry
 * recorded when asmspy-plan.md §H filed this row). Frequency mode replaces the
 * period+jitter machinery: the kernel adapts the period to hold freq_hz, and a
 * PRIME default de-aliases a periodic loop the way jitter does for IBS. */
#define SWCLOCK_DEFAULT_FREQ 997u

static int g_sw_open_errno = 0;

static void sw_fill_attr(struct perf_event_attr *a, unsigned freq_hz) {
    memset(a, 0, sizeof *a);
    a->size = sizeof *a;
    a->type = PERF_TYPE_SOFTWARE;
    a->config = PERF_COUNT_SW_TASK_CLOCK;
    a->freq = 1;
    a->sample_freq = freq_hz ? freq_hz : SWCLOCK_DEFAULT_FREQ;
    a->exclude_kernel = 1; /* user-only — the same unprivileged envelope */
    a->exclude_hv = 1;
    a->sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
    a->disabled = 1;
}

/* Open + mmap + enable one software-clock channel on `tid`. Mirrors
 * ibs_chan_open (and shares ibs_chan + the ring geometry, so the disable/free
 * teardown helpers are reused verbatim); only the attr and the errno slot
 * differ — the sw sampler's failure reason must not overwrite IBS's. */
static int sw_chan_open(ibs_chan *ch, pid_t tid, unsigned freq_hz, size_t pg,
                        size_t dsz) {
    ch->fd = -1;
    ch->base_map = MAP_FAILED;
    ch->base_sz = 0;
    ch->tid = tid;
    struct perf_event_attr a;
    sw_fill_attr(&a, freq_hz);
    long fd = perf_open(&a, tid, -1, -1, 0);
    if (fd < 0) {
        g_sw_open_errno = errno; /* the reason the operator will be shown */
        return -1;
    }
    size_t base_sz = pg + dsz;
    void *m =
        mmap(NULL, base_sz, PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
    if (m == MAP_FAILED) {
        close((int)fd);
        return -1;
    }
    ioctl((int)fd, PERF_EVENT_IOC_RESET, 0);
    if (ioctl((int)fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        munmap(m, base_sz);
        close((int)fd);
        return -1;
    }
    ch->fd = fd;
    ch->base_map = m;
    ch->base_sz = base_sz;
    return 0;
}

/* Drain one software-clock ring: PERF_SAMPLE_IP|TID records ({u64 ip; u32 pid,
 * tid} bodies — no RAW payload to decode), aggregated as {ip -> count} in the
 * edge hash with to = 0. That reuse is INTERNAL only: the public surface
 * exports honest ip buckets (asmtest_swclock_ip_t), never fake edges. The ring
 * walk mirrors ibs_drain, including the near-full-ring loss heuristic. */
static void sw_drain(void *base_map, size_t pg, size_t dsz, uint8_t *scratch,
                     edge_hash *h, asmtest_swclock_survey_t *out) {
    struct perf_event_mmap_page *mp = (struct perf_event_mmap_page *)base_map;
    uint8_t *data = (uint8_t *)base_map + pg;
    uint64_t head = mp->data_head;
    __sync_synchronize(); /* read data_head before the records (smp_rmb) */
    uint64_t tail = mp->data_tail;
    size_t span = (size_t)(head - tail);
    if (span == 0)
        return;
    if (span > dsz)
        span = dsz;
    if (span + IBS_MAX_RECORD > dsz)
        out->throttled = 1;

    for (size_t i = 0; i < span; i++)
        scratch[i] = data[(tail + i) % dsz];

    for (size_t off = 0; off + sizeof(struct perf_event_header) <= span;) {
        struct perf_event_header hd;
        memcpy(&hd, scratch + off, sizeof hd);
        if (hd.size == 0 || off + hd.size > span)
            break;
        if (hd.type == PERF_RECORD_SAMPLE) {
            if (hd.size >= sizeof hd + 8 + 8) { /* u64 ip; u32 pid, tid */
                out->samples++;
                eh_add(h, ld_u64(scratch + off + sizeof hd), 0, 0, 0);
            }
        } else if (hd.type == PERF_RECORD_LOST) {
            if (hd.size >= sizeof hd + 16)
                out->lost += ld_u64(scratch + off + sizeof hd + 8);
            else
                out->lost += 1;
        } else if (hd.type == PERF_RECORD_THROTTLE) {
            out->throttled = 1;
        }
        off += hd.size;
    }

    __sync_synchronize(); /* publish reads before advancing data_tail (smp_mb) */
    mp->data_tail = tail + span;
}

/* Descending count, ties ascending ip — deterministic run to run, the same
 * posture as eh_cmp_desc and the picker's own tie rule. */
static int sw_cmp_desc(const void *a, const void *b) {
    const asmtest_swclock_ip_t *x = (const asmtest_swclock_ip_t *)a;
    const asmtest_swclock_ip_t *y = (const asmtest_swclock_ip_t *)b;
    if (x->count != y->count)
        return x->count < y->count ? 1 : -1;
    return x->ip < y->ip ? -1 : x->ip > y->ip ? 1 : 0;
}

static int sw_export(edge_hash *h, asmtest_swclock_survey_t *out) {
    if (h->n == 0) {
        out->ips = NULL;
        out->n = 0;
        return 0;
    }
    asmtest_swclock_ip_t *arr =
        (asmtest_swclock_ip_t *)calloc(h->n, sizeof *arr);
    if (arr == NULL)
        return -1;
    size_t k = 0;
    for (size_t i = 0; i < h->cap && k < h->n; i++) {
        eh_slot *s = &h->slots[i];
        if (!s->used)
            continue;
        arr[k].ip = s->from;
        arr[k].count = s->count;
        k++;
    }
    qsort(arr, k, sizeof *arr, sw_cmp_desc);
    out->ips = arr;
    out->n = k;
    return 0;
}

int asmtest_swclock_survey_process(pid_t pid, unsigned ms, unsigned freq_hz,
                                   asmtest_swclock_survey_t *out) {
    if (out == NULL)
        return ASMTEST_IBS_EINVAL;
    memset(out, 0, sizeof *out);
    if (pid == 0)
        pid = getpid();
    if (ms == 0)
        ms = 300;

    size_t pg = ibs_page();
    size_t dsz = pg * IBS_RING_DATA_PAGES;
    ibs_chan *chans = (ibs_chan *)calloc(IBS_MAX_CHANS, sizeof *chans);
    uint8_t *scratch = (uint8_t *)malloc(dsz);
    edge_hash h;
    if (chans == NULL || scratch == NULL || eh_init(&h, 2048) != 0) {
        free(chans);
        free(scratch);
        return ASMTEST_IBS_EUNAVAIL;
    }

    int nch = 0;
    pid_t tids[IBS_MAX_CHANS];
    int ntid = ibs_list_tids(pid, tids, IBS_MAX_CHANS);
    if (ntid <= 0) { /* no such process / empty task list */
        g_sw_open_errno = ESRCH;
        eh_free(&h);
        free(scratch);
        free(chans);
        return ASMTEST_IBS_EUNAVAIL;
    }
    for (int i = 0; i < ntid; i++) {
        if (sw_chan_open(&chans[nch], tids[i], freq_hz, pg, dsz) == 0)
            nch++;
    }
    if (nch == 0) { /* perf_event_open refused for every thread */
        eh_free(&h);
        free(scratch);
        free(chans);
        return ASMTEST_IBS_EUNAVAIL;
    }

    /* Drain for `ms`, with one mid-window /proc rescan for threads spawned
     * after the start — the same coverage posture (and the same residual
     * born-and-died race) as asmtest_ibs_survey_process. */
    int rescanned = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        for (int i = 0; i < nch; i++)
            sw_drain(chans[i].base_map, pg, dsz, scratch, &h, out);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t el = elapsed_ms(&t0, &now);
        if (!rescanned && el >= (uint64_t)ms / 2) {
            rescanned = 1;
            if (nch < IBS_MAX_CHANS) {
                pid_t cur[IBS_MAX_CHANS];
                int m = ibs_list_tids(pid, cur, IBS_MAX_CHANS);
                for (int i = 0; i < m && nch < IBS_MAX_CHANS; i++) {
                    int known = 0;
                    for (int j = 0; j < nch; j++) {
                        if (chans[j].tid == cur[i]) {
                            known = 1;
                            break;
                        }
                    }
                    if (!known && sw_chan_open(&chans[nch], cur[i], freq_hz, pg,
                                               dsz) == 0)
                        nch++;
                }
            }
        }
        if (el >= ms)
            break;
        struct timespec slice = {0, 2 * 1000 * 1000}; /* 2 ms */
        nanosleep(&slice, NULL);
    }

    for (int i = 0; i < nch; i++)
        ibs_chan_disable(&chans[i]);
    for (int i = 0; i < nch; i++)
        sw_drain(chans[i].base_map, pg, dsz, scratch, &h, out); /* final */

    int xrc = sw_export(&h, out);
    eh_free(&h);
    free(scratch);
    for (int i = 0; i < nch; i++)
        ibs_chan_free(&chans[i]);
    free(chans);
    return xrc == 0 ? ASMTEST_IBS_OK : ASMTEST_IBS_EUNAVAIL;
}

const char *asmtest_swclock_unavail_reason(void) {
    switch (g_sw_open_errno) {
    case 0:
        return "";
    case EACCES:
    case EPERM:
        return "perf_event_open refused (EACCES/EPERM): the software-clock "
               "sampler needs perf_event_paranoid<=2 or CAP_PERFMON, and "
               "Docker's default seccomp profile blocks perf_event_open "
               "outright (--cap-add=PERFMON, or the docker-cli-ibs lane)";
    case ESRCH:
        return "perf_event_open: target process/thread exited (ESRCH)";
    case EMFILE:
    case ENFILE:
        return "perf_event_open hit the open-file limit (EMFILE)";
    case EINVAL:
        return "perf_event_open rejected the software-clock attr (EINVAL)";
    default:
        return "perf_event_open failed";
    }
}

/* --- window primitives (arm on the calling thread, run body, drain) ------------ */
/* The begin/run/end shape hwtrace.c's F6 fallback needs: unlike survey_pid (which
 * owns a timed drain loop), here the CALLER runs its own workload between begin and
 * end while IBS-Op samples the calling thread — exactly the branch-stack
 * sample_begin_amd/_end_amd control flow, so the two survey paths line up. */
typedef struct {
    ibs_chan ch;
    size_t pg, dsz;
    uint8_t *scratch;
    edge_hash h;
    ibs_cfg cfg;
} ibs_window;

int asmtest_ibs_window_begin(const asmtest_ibs_opts_t *opts, void **ctx_out) {
    if (ctx_out == NULL)
        return ASMTEST_IBS_EINVAL;
    *ctx_out = NULL;
    if (!asmtest_ibs_available())
        return ASMTEST_IBS_EUNAVAIL;
    int type = ibs_op_type();
    if (type < 0)
        return ASMTEST_IBS_EUNAVAIL;

    ibs_window *w = (ibs_window *)calloc(1, sizeof *w);
    if (w == NULL)
        return ASMTEST_IBS_EUNAVAIL;
    ibs_cfg_from_opts(opts, &w->cfg);
    /* No callchain in the window lane: no in-tree consumer decodes it, and a
     * callchain-sized record stream can silently overrun the single end-of-window
     * drain (the ring stays full -> loss with lost==0 && throttled==0). Mirrors
     * the fetch lane's cfg clear. See docs/internal/archive/plans/
     * zen2-ibs-tracing-plan.md Phase 5 and amd-review-followup-plan.md P3. */
    w->cfg.callchain = 0;
    w->pg = ibs_page();
    w->dsz = w->pg * IBS_RING_DATA_PAGES;
    if (ibs_chan_open(&w->ch, 0, -1, type, &w->cfg, w->pg, w->dsz) != 0) {
        free(w);
        return ASMTEST_IBS_EUNAVAIL; /* tid 0 => the calling thread */
    }
    w->scratch = (uint8_t *)malloc(w->dsz);
    if (w->scratch == NULL || eh_init(&w->h, 1024) != 0) {
        free(w->scratch);
        ibs_chan_disable(&w->ch);
        ibs_chan_free(&w->ch);
        free(w);
        return ASMTEST_IBS_EUNAVAIL;
    }
    *ctx_out = w;
    return ASMTEST_IBS_OK; /* sampling is live; caller runs its window body next */
}

int asmtest_ibs_window_end(void *ctx, asmtest_ibs_survey_t *out) {
    if (out == NULL)
        return ASMTEST_IBS_EINVAL;
    memset(out, 0, sizeof *out);
    if (ctx == NULL)
        return ASMTEST_IBS_EINVAL;
    ibs_window *w = (ibs_window *)ctx;
    ibs_chan_disable(&w->ch);
    /* Single drain at end (the caller's thread was busy running the body, so there was
     * no chance to drain mid-window). A window that overran the 256 KiB non-overwrite
     * ring loses its tail — ibs_drain flags that as out->throttled, mirroring the
     * branch-stack survey's near-full handling. */
    ibs_drain(w->ch.base_map, w->pg, w->dsz, w->scratch, &w->h, out,
              w->cfg.callchain, 0);
    eh_export(&w->h, out);
    eh_free(&w->h);
    free(w->scratch);
    ibs_chan_free(&w->ch);
    free(w);
    return ASMTEST_IBS_OK;
}

/* ============================ IBS-Fetch front-end lane ========================= */
/* Phase 7: the same open/mmap/drain machinery (ibs_chan_*, ibs_period, ibs_page) as
 * the Op survey, pointed at the ibs_fetch PMU, decoding fetch records into a
 * fetch-ADDRESS coverage histogram instead of control-flow edges. */

/* ibs_fetch PMU type from sysfs (dynamic, read at runtime), or -1 if absent. */
static int ibs_fetch_type(void) {
    static int cached = -2; /* -2 unread, -1 absent, >=0 type */
    if (cached != -2)
        return cached;
    cached = -1;
    FILE *f = fopen("/sys/bus/event_source/devices/ibs_fetch/type", "r");
    if (f != NULL) {
        int t = -1;
        if (fscanf(f, "%d", &t) == 1 && t >= 0)
            cached = t;
        fclose(f);
    }
    return cached;
}

static int g_fetch_avail = -1;
static const char *g_fetch_reason = "";

static void ibs_fetch_probe(void) {
    if (g_fetch_avail >= 0)
        return;
    if (!is_amd()) {
        g_fetch_avail = 0;
        g_fetch_reason = "not AMD";
        return;
    }
    unsigned caps = ibs_caps_eax();
    if (caps == 0) {
        g_fetch_avail = 0;
        g_fetch_reason = "no IBS (CPUID 8000_001B absent)";
        return;
    }
    if (!(caps & (1u << 1))) { /* FetchSam */
        g_fetch_avail = 0;
        g_fetch_reason = "no IBS fetch sampling (FetchSam)";
        return;
    }
    if (ibs_fetch_type() < 0) {
        g_fetch_avail = 0;
        g_fetch_reason = "no ibs_fetch PMU";
        return;
    }
    if (access("/sys/bus/event_source/devices/ibs_fetch/format/swfilt", F_OK) !=
        0) {
        g_fetch_avail = 0;
        g_fetch_reason = "no swfilt (kernel < ~6.2)";
        return;
    }
    g_fetch_avail = 1;
    g_fetch_reason = "";
}

int asmtest_ibs_fetch_available(void) {
    ibs_fetch_probe();
    return g_fetch_avail;
}
const char *asmtest_ibs_fetch_skip_reason(void) {
    ibs_fetch_probe();
    return g_fetch_avail == 1 ? "" : g_fetch_reason;
}

/* --- fetch-address aggregation (open-addressing hash keyed on `addr`) ---------- */

typedef struct {
    uint64_t addr, count, icache_miss, itlb_miss, latency_sum;
    int used;
} fh_slot;
typedef struct {
    fh_slot *slots;
    size_t cap; /* power of two */
    size_t n;
} fetch_hash;

static int fh_init(fetch_hash *h, size_t cap) {
    h->slots = (fh_slot *)calloc(cap, sizeof *h->slots);
    if (h->slots == NULL)
        return -1;
    h->cap = cap;
    h->n = 0;
    return 0;
}
static void fh_free(fetch_hash *h) {
    free(h->slots);
    h->slots = NULL;
    h->cap = h->n = 0;
}
static size_t fh_hash(uint64_t addr, size_t mask) {
    uint64_t k = addr * 0x9E3779B97F4A7C15ull;
    k ^= k >> 29;
    return (size_t)k & mask;
}
static int fh_grow(fetch_hash *h); /* fwd */

/* Add one valid fetch sample. Aggregates duplicates; grows past a 0.7 load factor. */
static void fh_add(fetch_hash *h, uint64_t addr, unsigned icmiss,
                   unsigned itlbmiss, unsigned lat) {
    if (h->slots == NULL)
        return;
    if ((h->n + 1) * 10 >= h->cap * 7) {
        if (fh_grow(h) != 0)
            return; /* OOM: drop the sample rather than crash (survey is a prefix) */
    }
    size_t mask = h->cap - 1;
    size_t i = fh_hash(addr, mask);
    for (;;) {
        fh_slot *s = &h->slots[i];
        if (!s->used) {
            s->used = 1;
            s->addr = addr;
            s->count = 1;
            s->icache_miss = icmiss;
            s->itlb_miss = itlbmiss;
            s->latency_sum = lat;
            h->n++;
            return;
        }
        if (s->addr == addr) {
            s->count++;
            s->icache_miss += icmiss;
            s->itlb_miss += itlbmiss;
            s->latency_sum += lat;
            return;
        }
        i = (i + 1) & mask;
    }
}
static int fh_grow(fetch_hash *h) {
    fetch_hash bigger;
    size_t nc; /* overflow-checked h->cap * 2 (S6) */
    if (!asmtest_grow_pow2(h->cap, h->cap + 1, sizeof *h->slots, &nc))
        return -1;
    if (fh_init(&bigger, nc) != 0)
        return -1;
    for (size_t i = 0; i < h->cap; i++) {
        fh_slot *s = &h->slots[i];
        if (!s->used)
            continue;
        size_t mask = bigger.cap - 1;
        size_t j = fh_hash(s->addr, mask);
        while (bigger.slots[j].used)
            j = (j + 1) & mask;
        bigger.slots[j] = *s;
        bigger.n++;
    }
    fh_free(h);
    *h = bigger;
    return 0;
}
static int fh_cmp_desc(const void *a, const void *b) {
    const asmtest_ibs_fetch_hot_t *x = (const asmtest_ibs_fetch_hot_t *)a;
    const asmtest_ibs_fetch_hot_t *y = (const asmtest_ibs_fetch_hot_t *)b;
    if (x->count != y->count)
        return x->count < y->count ? 1 : -1; /* descending by count */
    if (x->addr != y->addr)
        return x->addr < y->addr ? -1 : 1; /* deterministic tiebreak */
    return 0;
}
/* Export used slots into a freshly-malloc'd array sorted by descending count. */
static int fh_export(fetch_hash *h, asmtest_ibs_fetch_survey_t *out) {
    if (h->n == 0) {
        out->hot = NULL;
        out->n = 0;
        return 0;
    }
    asmtest_ibs_fetch_hot_t *arr =
        (asmtest_ibs_fetch_hot_t *)calloc(h->n, sizeof *arr);
    if (arr == NULL)
        return -1;
    size_t k = 0;
    for (size_t i = 0; i < h->cap && k < h->n; i++) {
        fh_slot *s = &h->slots[i];
        if (!s->used)
            continue;
        arr[k].addr = s->addr;
        arr[k].count = s->count;
        arr[k].icache_miss = s->icache_miss;
        arr[k].itlb_miss = s->itlb_miss;
        arr[k].latency_sum = s->latency_sum;
        k++;
    }
    qsort(arr, k, sizeof *arr, fh_cmp_desc);
    out->hot = arr;
    out->n = k;
    return 0;
}

/* --- fetch ring drain ---------------------------------------------------------- */
/* Mirrors ibs_drain, but decodes fetch records (asmtest_ibs_decode_fetch) into the
 * fetch-address hash and tracks fetch provenance (valid samples, aggregate misses). */
static void ibs_fetch_drain(void *base_map, size_t pg, size_t dsz,
                            uint8_t *scratch, fetch_hash *h,
                            asmtest_ibs_fetch_survey_t *out) {
    struct perf_event_mmap_page *mp = (struct perf_event_mmap_page *)base_map;
    uint8_t *data = (uint8_t *)base_map + pg;
    uint64_t head = mp->data_head;
    __sync_synchronize(); /* read data_head before the records (smp_rmb) */
    uint64_t tail = mp->data_tail;
    size_t span = (size_t)(head - tail);
    if (span == 0)
        return;
    if (span > dsz)
        span = dsz; /* defensive: never over-read the scratch buffer */
    if (span + IBS_MAX_RECORD > dsz)
        out->throttled =
            1; /* less than one record of headroom == silent loss */

    for (size_t i = 0; i < span; i++)
        scratch[i] = data[(tail + i) % dsz];

    for (size_t off = 0; off + sizeof(struct perf_event_header) <= span;) {
        struct perf_event_header hd;
        memcpy(&hd, scratch + off, sizeof hd);
        if (hd.size == 0 || off + hd.size > span)
            break;
        if (hd.type == PERF_RECORD_SAMPLE) {
            out->samples++;
            /* body order for sample_type IP|TID|RAW: u64 ip; u32 pid,tid;
             * u32 raw_size; char raw[raw_size]. */
            size_t need = sizeof hd + 8 + 8 + 4;
            if (hd.size >= need) {
                uint8_t *rawsz_p = scratch + off + sizeof hd + 8 + 8;
                uint32_t rawsz = ld_u32(rawsz_p);
                uint8_t *raw = rawsz_p + 4;
                if ((size_t)(raw - (scratch + off)) + rawsz <= hd.size) {
                    asmtest_ibs_fetch_sample_t fs;
                    if (asmtest_ibs_decode_fetch(raw, rawsz, &fs) ==
                        ASMTEST_IBS_OK) {
                        out->valid_samples++;
                        out->icache_misses += fs.icache_miss;
                        out->itlb_misses += fs.itlb_miss;
                        fh_add(h, fs.fetch_addr, fs.icache_miss, fs.itlb_miss,
                               fs.latency);
                    }
                }
            }
        } else if (hd.type == PERF_RECORD_LOST) {
            if (hd.size >= sizeof hd + 16)
                out->lost += ld_u64(scratch + off + sizeof hd + 8);
            else
                out->lost += 1;
        } else if (hd.type == PERF_RECORD_THROTTLE) {
            out->throttled = 1;
        }
        off += hd.size;
    }

    __sync_synchronize(); /* publish reads before advancing data_tail (smp_mb) */
    mp->data_tail = tail + span;
}

int asmtest_ibs_survey_fetch_pid(pid_t tid, unsigned ms,
                                 const asmtest_ibs_opts_t *opts,
                                 asmtest_ibs_fetch_survey_t *out) {
    if (out == NULL)
        return ASMTEST_IBS_EINVAL;
    memset(out, 0, sizeof *out);
    if (!asmtest_ibs_fetch_available())
        return ASMTEST_IBS_EUNAVAIL;
    int type = ibs_fetch_type();
    if (type < 0)
        return ASMTEST_IBS_EUNAVAIL;

    /* Reuse the shared config resolver for period + jitter, but clear the Op-only
     * knobs (cnt_ctl / callchain are ibs_op fields; system_wide is an Op-survey mode)
     * so the fetch attr stays a plain user-only fetch sampler. */
    ibs_cfg cfg;
    ibs_cfg_from_opts(opts, &cfg);
    cfg.cnt_dispatched = 0;
    cfg.callchain = 0;
    cfg.system_wide = 0;
    size_t pg = ibs_page();
    size_t dsz = pg * IBS_RING_DATA_PAGES;

    ibs_chan ch;
    if (ibs_chan_open(&ch, tid, -1, type, &cfg, pg, dsz) != 0)
        return ASMTEST_IBS_EUNAVAIL;

    uint8_t *scratch = (uint8_t *)malloc(dsz);
    fetch_hash h;
    if (scratch == NULL || fh_init(&h, 1024) != 0) {
        free(scratch);
        ibs_chan_disable(&ch);
        ibs_chan_free(&ch);
        return ASMTEST_IBS_EUNAVAIL;
    }

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        ibs_fetch_drain(ch.base_map, pg, dsz, scratch, &h, out);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (elapsed_ms(&t0, &now) >= ms)
            break;
        ibs_chan_rejitter(&ch, &cfg);
        struct timespec slice = {0, 2 * 1000 * 1000}; /* 2 ms */
        nanosleep(&slice, NULL);
    }
    ibs_chan_disable(&ch);
    ibs_fetch_drain(ch.base_map, pg, dsz, scratch, &h, out); /* final drain */

    fh_export(&h, out);
    fh_free(&h);
    free(scratch);
    ibs_chan_free(&ch);
    return ASMTEST_IBS_OK;
}

#else /* not Linux x86-64 --------------------------------------------------------- */

int asmtest_ibs_available(void) { return 0; }
const char *asmtest_ibs_skip_reason(void) {
    return "IBS is Linux/x86-64 AMD only";
}
const char *asmtest_ibs_unavail_reason(void) {
    /* No substrate here, so the detect-chain reason IS the whole reason — there
     * is no perf_event_open on this build to have failed. */
    return asmtest_ibs_skip_reason();
}
int asmtest_ibs_survey_pid(pid_t tid, unsigned ms,
                           const asmtest_ibs_opts_t *opts,
                           asmtest_ibs_survey_t *out) {
    (void)tid;
    (void)ms;
    (void)opts;
    if (out != NULL)
        memset(out, 0, sizeof *out);
    return ASMTEST_IBS_EUNAVAIL;
}
int asmtest_ibs_survey_process(pid_t pid, unsigned ms,
                               const asmtest_ibs_opts_t *opts,
                               asmtest_ibs_survey_t *out) {
    (void)pid;
    (void)ms;
    (void)opts;
    if (out != NULL)
        memset(out, 0, sizeof *out);
    return ASMTEST_IBS_EUNAVAIL;
}
/* Portable software-clock sampler: perf_event_open is Linux-only, so off Linux
 * these self-skip. (On Linux non-x86-64 they are stubbed out today only
 * because this file's whole live half is x86-64-gated — nothing about
 * SW_TASK_CLOCK is arch-specific; lifting that is part of the ARM64 rows.) */
int asmtest_swclock_survey_process(pid_t pid, unsigned ms, unsigned freq_hz,
                                   asmtest_swclock_survey_t *out) {
    (void)pid;
    (void)ms;
    (void)freq_hz;
    if (out != NULL)
        memset(out, 0, sizeof *out);
    return ASMTEST_IBS_EUNAVAIL;
}
const char *asmtest_swclock_unavail_reason(void) {
    return "software-clock sampling needs Linux perf_event_open (this build's "
           "live half is Linux/x86-64-gated)";
}

int asmtest_ibs_window_begin(const asmtest_ibs_opts_t *opts, void **ctx_out) {
    (void)opts;
    if (ctx_out != NULL)
        *ctx_out = NULL;
    return ASMTEST_IBS_EUNAVAIL;
}
int asmtest_ibs_window_end(void *ctx, asmtest_ibs_survey_t *out) {
    (void)ctx;
    if (out != NULL)
        memset(out, 0, sizeof *out);
    return ASMTEST_IBS_EUNAVAIL;
}

/* IBS-Fetch front-end lane: self-skips off Linux/x86-64/AMD (the pure decoder +
 * asmtest_ibs_fetch_survey_free are defined above for ALL platforms). */
int asmtest_ibs_fetch_available(void) { return 0; }
const char *asmtest_ibs_fetch_skip_reason(void) {
    return "IBS is Linux/x86-64 AMD only";
}
int asmtest_ibs_survey_fetch_pid(pid_t tid, unsigned ms,
                                 const asmtest_ibs_opts_t *opts,
                                 asmtest_ibs_fetch_survey_t *out) {
    (void)tid;
    (void)ms;
    (void)opts;
    if (out != NULL)
        memset(out, 0, sizeof *out);
    return ASMTEST_IBS_EUNAVAIL;
}

#endif
