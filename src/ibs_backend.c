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
 */
#include "asmtest_ibs.h"

#include "ibs_backend.h" /* internal window primitives (asmtest_ibs_window_*) */

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
     * shorter than that lacks BrnTrgt (or is truncated) — no edge is derivable. */
    if (raw_len < IBS_RAW_MIN_BYTES)
        return ASMTEST_IBS_EDECODE;

    const uint8_t *p = (const uint8_t *)raw;
    uint64_t rip = ld_u64(p + IBS_RAW_REG_OFF(IBS_REG_RIP));
    uint64_t data = ld_u64(p + IBS_RAW_REG_OFF(IBS_REG_DATA));
    uint64_t tgt = ld_u64(p + IBS_RAW_REG_OFF(IBS_REG_BRTGT));

    int brn_ret = bit(data, IBS_OPDATA_BRN_RET);
    int brn_taken = bit(data, IBS_OPDATA_BRN_TAKEN);
    int rip_invalid = bit(data, IBS_OPDATA_RIP_INVALID);

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

/* Dispatched-op count between samples (a multiple of 16). */
#define IBS_DEFAULT_PERIOD 0x4000u
/* 256 KiB data ring (64 * 4 KiB pages). */
#define IBS_RING_DATA_PAGES 64u
/* Largest single record we parse: header + IP + TID + RAW(size + caps + 8 regs). */
#define IBS_MAX_RECORD                                                         \
    (sizeof(struct perf_event_header) + 8u + 8u + 4u + IBS_RAW_MIN_BYTES + 16u)

static long perf_open(struct perf_event_attr *a, pid_t pid, int cpu, int group,
                      unsigned long flags) {
    return syscall(SYS_perf_event_open, a, pid, cpu, group, flags);
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
    if (eh_init(&bigger, h->cap * 2) != 0)
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

/* Copy [tail,head) out of the (possibly wrapping) non-overwrite ring into `scratch`
 * (dsz bytes), parse each record, aggregate edges, advance data_tail. */
static void ibs_drain(void *base_map, size_t pg, size_t dsz, uint8_t *scratch,
                      edge_hash *h, asmtest_ibs_survey_t *out) {
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
     * less than one max record of headroom as loss (mirrors hwtrace.c). */
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
            out->samples++;
            /* body order for sample_type IP|TID|RAW: u64 ip; u32 pid,tid;
             * u32 raw_size; char raw[raw_size]. */
            size_t need = sizeof hd + 8 + 8 + 4;
            if (hd.size >= need) {
                uint8_t *rawsz_p = scratch + off + sizeof hd + 8 + 8;
                uint32_t rawsz = ld_u32(rawsz_p);
                uint8_t *raw = rawsz_p + 4;
                /* raw payload must fit inside this record */
                if ((size_t)(raw - (scratch + off)) + rawsz <= hd.size) {
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

/* Fill the IBS-Op perf attr: user-only branch sampling (swfilt + exclude_kernel),
 * IP|TID|RAW records, initially disabled. Shared by every channel. */
static void ibs_fill_attr(struct perf_event_attr *a, int type,
                          uint64_t period) {
    memset(a, 0, sizeof *a);
    a->size = sizeof *a;
    a->type = (uint32_t)type;
    a->sample_period = period;
    a->config2 = 1; /* swfilt (config2:0): enables exclude_kernel at p=2 */
    a->exclude_kernel =
        1; /* user-only — the unprivileged envelope            */
    a->sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_RAW;
    a->disabled = 1;
}

/* Resolve the effective sample period from opts (rounded to IBS's /16 granularity). */
static uint64_t ibs_period(const asmtest_ibs_opts_t *opts) {
    uint64_t period = (opts != NULL && opts->sample_period != 0)
                          ? opts->sample_period
                          : IBS_DEFAULT_PERIOD;
    period &= ~(uint64_t)0xF; /* IBS max-count granularity is 16 */
    if (period < 16)
        period = IBS_DEFAULT_PERIOD;
    return period;
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

/* Open + mmap + enable one IBS-Op channel on `tid` (0 => the calling thread). Returns
 * 0 on success; -1 if the thread cannot be attached (it exited, or the open/mmap/
 * enable failed) — the caller skips it rather than aborting the whole survey. */
static int ibs_chan_open(ibs_chan *ch, pid_t tid, int type, uint64_t period,
                         size_t pg, size_t dsz) {
    ch->fd = -1;
    ch->base_map = MAP_FAILED;
    ch->base_sz = 0;
    ch->tid = tid;
    struct perf_event_attr a;
    ibs_fill_attr(&a, type, period);
    long fd = perf_open(&a, tid, -1, -1, 0);
    if (fd < 0)
        return -1;
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
                          asmtest_ibs_survey_t *out) {
    for (int i = 0; i < nch; i++)
        ibs_drain(chans[i].base_map, pg, dsz, scratch, h, out);
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

    size_t pg = ibs_page();
    size_t dsz = pg * IBS_RING_DATA_PAGES;

    ibs_chan ch;
    if (ibs_chan_open(&ch, tid, type, ibs_period(opts), pg, dsz) != 0)
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
        ibs_drain(ch.base_map, pg, dsz, scratch, &h, out);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (elapsed_ms(&t0, &now) >= ms)
            break;
        struct timespec slice = {0, 2 * 1000 * 1000}; /* 2 ms */
        nanosleep(&slice, NULL);
    }
    ibs_chan_disable(&ch);
    ibs_drain(ch.base_map, pg, dsz, scratch, &h, out); /* final drain */

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
    uint64_t period = ibs_period(opts);

    size_t pg = ibs_page();
    size_t dsz = pg * IBS_RING_DATA_PAGES;

    pid_t tids[IBS_MAX_CHANS];
    int ntid = ibs_list_tids(pid, tids, IBS_MAX_CHANS);
    if (ntid <= 0)
        return ASMTEST_IBS_EUNAVAIL; /* no such process / empty task list */

    ibs_chan *chans = (ibs_chan *)calloc(IBS_MAX_CHANS, sizeof *chans);
    uint8_t *scratch = (uint8_t *)malloc(dsz);
    edge_hash h;
    if (chans == NULL || scratch == NULL || eh_init(&h, 2048) != 0) {
        free(chans);
        free(scratch);
        return ASMTEST_IBS_EUNAVAIL;
    }

    /* Attach one out-of-band event per pre-existing thread. Threads that vanished
     * since the enumeration just fail to open and are skipped. */
    int nch = 0;
    for (int i = 0; i < ntid; i++) {
        if (ibs_chan_open(&chans[nch], tids[i], type, period, pg, dsz) == 0)
            nch++;
    }
    if (nch == 0) { /* every thread gone, or perf_event_open blocked */
        eh_free(&h);
        free(scratch);
        free(chans);
        return ASMTEST_IBS_EUNAVAIL;
    }

    /* Drain all rings for `ms`, with ONE mid-window rescan of task/ to attach any
     * thread spawned after we started (see the header's residual-race note). */
    int rescanned = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        ibs_drain_all(chans, nch, pg, dsz, scratch, &h, out);
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
                    if (!known && ibs_chan_open(&chans[nch], cur[i], type,
                                                period, pg, dsz) == 0)
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
    ibs_drain_all(chans, nch, pg, dsz, scratch, &h, out); /* final drain */

    eh_export(&h, out);
    eh_free(&h);
    free(scratch);
    for (int i = 0; i < nch; i++)
        ibs_chan_free(&chans[i]);
    free(chans);
    return ASMTEST_IBS_OK;
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
    w->pg = ibs_page();
    w->dsz = w->pg * IBS_RING_DATA_PAGES;
    if (ibs_chan_open(&w->ch, 0, type, ibs_period(opts), w->pg, w->dsz) != 0) {
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
    ibs_drain(w->ch.base_map, w->pg, w->dsz, w->scratch, &w->h, out);
    eh_export(&w->h, out);
    eh_free(&w->h);
    free(w->scratch);
    ibs_chan_free(&w->ch);
    free(w);
    return ASMTEST_IBS_OK;
}

#else /* not Linux x86-64 --------------------------------------------------------- */

int asmtest_ibs_available(void) { return 0; }
const char *asmtest_ibs_skip_reason(void) {
    return "IBS is Linux/x86-64 AMD only";
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

#endif
