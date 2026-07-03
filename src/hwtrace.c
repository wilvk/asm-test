/*
 * hwtrace.c — hardware-assisted native trace capture + lifecycle (see
 * asmtest_hwtrace.h and docs/native-tracing.md). Linux only.
 *
 * This file owns the gating chain (asmtest_hwtrace_available), the region
 * registry, and the per-region perf_event_open + AUX-ring capture for Intel PT
 * (and, structurally, ARM CoreSight cs_etm). The raw packet stream it captures is
 * handed to a decoder — libipt (src/pt_backend.c) or OpenCSD (src/cs_backend.c) —
 * which replays the registered code bytes to reconstruct the asmtest_trace_t
 * offsets. The decoders are separate translation units so the C++ OpenCSD stays
 * isolated and each is independently dependency-gated.
 *
 * NOTE ON VALIDATION: the capture path requires bare-metal Intel PT (or a
 * CoreSight board) with perf privilege lowered — it cannot run on AMD hosts, VMs,
 * or standard CI, which is exactly why asmtest_hwtrace_available() self-skips. The
 * gating and decode-dispatch logic here is exercised on every host; the live
 * capture is exercised only on capable hardware.
 */
#include "asmtest_hwtrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

/* Decoder entry points (defined in pt_backend.c / cs_backend.c, or the no-decoder
 * stubs below). Decode the raw AUX bytes against [base, base+len) into *trace. */
int asmtest_pt_decode(const uint8_t *aux, size_t aux_len, const void *base,
                      size_t len, asmtest_trace_t *trace);
int asmtest_cs_decode(const uint8_t *aux, size_t aux_len, const void *base,
                      size_t len, asmtest_trace_t *trace);

/* AMD branch-record decode (amd_backend.c): takes the perf branch-stack array,
 * not an AUX byte stream. */
#if defined(__linux__) && defined(__x86_64__)
int asmtest_amd_decode(const struct perf_branch_entry *br, size_t nbr,
                       const void *base, size_t len, asmtest_trace_t *trace);
/* Tier-B: stitch the overlapping sample_period=1 branch-stack windows into one
 * gapless sequence (asmtest_amd_stitch), then decode it without the 16-entry ceiling
 * (asmtest_amd_decode_stitched) — lifts the single-window limit past 16 branches. */
size_t asmtest_amd_stitch(const struct perf_branch_entry *const *samples,
                          const size_t *nrs, size_t n_samples,
                          struct perf_branch_entry *out, size_t out_cap,
                          int *gap);
int asmtest_amd_decode_stitched(const struct perf_branch_entry *br, size_t nbr,
                                const void *base, size_t len,
                                asmtest_trace_t *trace, int gap);
/* AMD branch stack depth: a richest-window count at the ceiling means the routine
 * overflowed one snapshot, so escalate from Tier-A to Tier-B stitching. */
#define AMD_LBR_DEPTH 16
#endif

/* Single-step (EFLAGS.TF / SIGTRAP) stepper (ss_backend.c). Unlike the trace
 * backends there is no post-pass decode: begin() arms TF and the SIGTRAP handler
 * fills the trace live; end() disarms. base/len bound the region, trace is the
 * sink (block normalization needs the Capstone length-decoder). */
#if defined(__linux__) && defined(__x86_64__)
int asmtest_ss_begin(const void *base, size_t len, asmtest_trace_t *trace);
void asmtest_ss_end(void);
#endif

/* Whether each decoder was compiled in (queried by available()). */
int asmtest_pt_decoder_present(void);
int asmtest_cs_decoder_present(void);
int asmtest_amd_decoder_present(void);

/* ------------------------------------------------------------------ */
/* Gating: detect-and-skip                                             */
/* ------------------------------------------------------------------ */

static const char *pmu_name(asmtest_trace_backend_t b) {
    return b == ASMTEST_HWTRACE_INTEL_PT ? "intel_pt" : "cs_etm";
}

/* Read the PMU type id from sysfs, or -1 if the node is absent. */
static int pmu_type(asmtest_trace_backend_t b) {
#if defined(__linux__)
    char path[128];
    snprintf(path, sizeof path, "/sys/bus/event_source/devices/%s/type",
             pmu_name(b));
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    int t = -1;
    if (fscanf(f, "%d", &t) != 1)
        t = -1;
    fclose(f);
    return t;
#else
    (void)b;
    return -1;
#endif
}

static int decoder_present(asmtest_trace_backend_t b) {
    switch (b) {
    case ASMTEST_HWTRACE_INTEL_PT:
        return asmtest_pt_decoder_present();
    case ASMTEST_HWTRACE_CORESIGHT:
        return asmtest_cs_decoder_present();
    case ASMTEST_HWTRACE_AMD_LBR:
        return asmtest_amd_decoder_present();
    case ASMTEST_HWTRACE_SINGLESTEP:
        /* The stepper records exact instruction offsets with no decoder; block
         * normalization needs only the Capstone length-decoder, already linked. */
        return asmtest_disas_available();
    }
    return 0;
}

/* True if /proc/cpuinfo's vendor_id contains `want` (x86 only). */
static int vendor_is(const char *want) {
#if defined(__x86_64__)
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f == NULL)
        return 0;
    char line[256];
    int hit = 0;
    while (fgets(line, sizeof line, f) != NULL)
        if (strncmp(line, "vendor_id", 9) == 0) {
            hit = strstr(line, want) != NULL;
            break;
        }
    fclose(f);
    return hit;
#else
    (void)want;
    return 0;
#endif
}

/* CPU/ISA/vendor check: Intel PT needs GenuineIntel x86-64; AMD LBR needs
 * AuthenticAMD x86-64; CoreSight needs AArch64. */
static int cpu_matches(asmtest_trace_backend_t b) {
    switch (b) {
    case ASMTEST_HWTRACE_CORESIGHT:
#if defined(__aarch64__)
        return 1;
#else
        return 0;
#endif
    case ASMTEST_HWTRACE_INTEL_PT:
        return vendor_is("GenuineIntel");
    case ASMTEST_HWTRACE_AMD_LBR:
        return vendor_is("AuthenticAMD");
    case ASMTEST_HWTRACE_SINGLESTEP:
        /* TF/#DB single-step is baseline x86-64 — any vendor, Intel or AMD — but
         * the whole capture lifecycle (begin/end, asmtest_ss_*) is implemented only
         * under Linux; off Linux it is a no-op stub. Gate on __linux__ too so
         * available() self-skips (with the "Linux x86-64 only" reason) instead of
         * reporting available and yielding an empty "complete" trace. */
#if defined(__x86_64__) && defined(__linux__)
        return 1;
#else
        return 0;
#endif
    }
    return 0;
}

#if defined(__linux__)
static long perf_open(struct perf_event_attr *a, pid_t pid, int cpu, int group,
                      unsigned long flags) {
    return syscall(SYS_perf_event_open, a, pid, cpu, group, flags);
}

/* AMD capability probe outcomes (no sysfs PMU node exists for branch records).
 * AMD LBR is x86-only and amd_branch_probe is called only from the x86-64 arms
 * below, so gate it to x86-64 to keep the AArch64 build -Wunused-function clean. */
#if defined(__x86_64__)
enum { AMD_OK = 0, AMD_NOHW = 1, AMD_NOPERM = 2 };

/* Probe AMD branch-record support by attempting a branch-stack sampling open
 * (Zen 4 LbrExtV2 / Zen 3 BRS). EOPNOTSUPP/EINVAL => no hardware (e.g. Zen 2);
 * EACCES/EPERM => privilege. */
static int amd_branch_probe(void) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = PERF_TYPE_HARDWARE;
    a.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
    a.sample_period = 1;
    a.sample_type = PERF_SAMPLE_BRANCH_STACK;
    a.branch_sample_type = PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_ANY;
    a.exclude_kernel = 1;
    a.disabled = 1;
    long fd = perf_open(&a, 0, -1, -1, 0);
    if (fd >= 0) {
        close((int)fd);
        return AMD_OK;
    }
    if (errno == EACCES || errno == EPERM)
        return AMD_NOPERM;
    return AMD_NOHW; /* EOPNOTSUPP/EINVAL: no Zen 3 BRS / Zen 4 LbrExtV2 */
}
#endif /* __x86_64__ */
#endif /* __linux__ */

/* A real privilege probe: try to open the AUX PMU event disabled, then close it
 * (Intel PT / CoreSight). AMD uses amd_branch_probe instead. */
static int perf_permitted(asmtest_trace_backend_t b) {
#if defined(__linux__)
    int type = pmu_type(b);
    if (type < 0)
        return 0;
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.size = sizeof attr;
    attr.type = (uint32_t)type;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.disabled = 1;
    long fd = perf_open(&attr, 0, -1, -1, 0);
    if (fd < 0)
        return 0; /* EACCES/EPERM/EINVAL -> not usable here */
    close((int)fd);
    return 1;
#else
    (void)b;
    return 0;
#endif
}

int asmtest_hwtrace_available(asmtest_trace_backend_t backend) {
    if (!decoder_present(backend) || !cpu_matches(backend))
        return 0;
    if (backend == ASMTEST_HWTRACE_SINGLESTEP)
        /* No PMU, no perf_event, no privilege: decoder (Capstone) + x86-64 is all
         * the gate needs. This is why it runs where PT/AMD self-skip. */
        return 1;
    if (backend == ASMTEST_HWTRACE_AMD_LBR) {
#if defined(__linux__) && defined(__x86_64__)
        return amd_branch_probe() == AMD_OK;
#else
        return 0;
#endif
    }
    return pmu_type(backend) >= 0 && perf_permitted(backend);
}

void asmtest_hwtrace_skip_reason(asmtest_trace_backend_t backend, char *buf,
                                 size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *r = "available";
    if (!decoder_present(backend))
        r = (backend == ASMTEST_HWTRACE_INTEL_PT)    ? "built without libipt"
            : (backend == ASMTEST_HWTRACE_CORESIGHT) ? "built without OpenCSD"
            : (backend == ASMTEST_HWTRACE_SINGLESTEP)
                ? "built without Capstone (single-step block normalization)"
                : "built without Capstone (AMD reconstruction)";
    else if (!cpu_matches(backend))
        r = (backend == ASMTEST_HWTRACE_INTEL_PT)
                ? "not a GenuineIntel x86-64 host"
            : (backend == ASMTEST_HWTRACE_CORESIGHT) ? "not an AArch64 host"
            : (backend == ASMTEST_HWTRACE_SINGLESTEP)
                ? "single-step backend is Linux x86-64 only (Windows/macOS "
                  "planned)"
                : "not an AuthenticAMD x86-64 host";
    else if (backend == ASMTEST_HWTRACE_SINGLESTEP)
        r = "available"; /* decoder + x86-64 satisfied: no PMU/perf/privilege gate */
    else if (backend == ASMTEST_HWTRACE_AMD_LBR) {
#if defined(__linux__) && defined(__x86_64__)
        int p = amd_branch_probe();
        r = (p == AMD_NOHW)
                ? "no AMD branch records (needs Zen 3 BRS / Zen 4 LbrExtV2)"
            : (p == AMD_NOPERM) ? "perf branch-stack not permitted (lower "
                                  "perf_event_paranoid or "
                                  "grant CAP_PERFMON)"
                                : "available";
#else
        r = "AMD LBR is Linux x86-64 only";
#endif
    } else if (pmu_type(backend) < 0)
        r = (backend == ASMTEST_HWTRACE_INTEL_PT)
                ? "no intel_pt PMU (needs bare-metal Intel; absent on AMD/VM)"
                : "no cs_etm PMU (needs a CoreSight-capable AArch64 board)";
    else if (!perf_permitted(backend))
        r = "perf_event capture not permitted (lower perf_event_paranoid or "
            "grant CAP_PERFMON)";
    snprintf(buf, buflen, "%s", r);
}

/* ------------------------------------------------------------------ */
/* Backend auto-selection (the hardware-tier fallback cascade)         */
/*                                                                     */
/* All four backends fill the same asmtest_trace_t and self-skip via    */
/* asmtest_hwtrace_available(), so the cascade is just those probes in   */
/* priority order. Order is descending fidelity == ascending enum:      */
/* Intel PT (near-zero overhead, exact, unbounded ring) > AMD LBR        */
/* (HW-attributed; Tier-B stitches past the 16-deep stack, ring-bounded) */
/* > single-step (exact + unbounded, per-instruction cost) > CoreSight   */
/* (AArch64). CEILING_FREE drops AMD LBR — the one backend whose capture */
/* is ceiling-bounded (data ring / PMI throttling) — so its pick has no  */
/* completeness ceiling: the re-resolve target after trace.truncated.    */
/* ------------------------------------------------------------------ */
size_t asmtest_hwtrace_resolve(asmtest_hwtrace_policy_t policy,
                               asmtest_trace_backend_t *out, size_t cap) {
    if (out == NULL || cap == 0)
        return 0;
    static const asmtest_trace_backend_t order[] = {
        ASMTEST_HWTRACE_INTEL_PT,
        ASMTEST_HWTRACE_AMD_LBR,
        ASMTEST_HWTRACE_SINGLESTEP,
        ASMTEST_HWTRACE_CORESIGHT,
    };
    size_t n = 0;
    for (size_t i = 0; i < sizeof order / sizeof order[0] && n < cap; i++) {
        if (policy == ASMTEST_HWTRACE_CEILING_FREE &&
            order[i] == ASMTEST_HWTRACE_AMD_LBR)
            continue; /* fixed 16-branch window: excluded from the ceiling-free set */
        if (asmtest_hwtrace_available(order[i]))
            out[n++] = order[i];
    }
    return n;
}

int asmtest_hwtrace_auto(asmtest_hwtrace_policy_t policy) {
    asmtest_trace_backend_t b;
    if (asmtest_hwtrace_resolve(policy, &b, 1) == 0)
        return ASMTEST_HW_EUNAVAIL;
    return (int)b;
}

/* ------------------------------------------------------------------ */
/* Region registry + per-thread capture                                */
/* ------------------------------------------------------------------ */
#define MAX_REGIONS 32
typedef struct {
    char name[64];
    int used;
    void *base;
    size_t len;
    asmtest_trace_t *trace;
} hw_region_t;

static hw_region_t g_regions[MAX_REGIONS];
static int g_nregions = 0;
static asmtest_hwtrace_options_t g_opts;
static int g_inited = 0;

/* Active capture state (single region at a time in the MVP). */
#if defined(__linux__)
static int g_fd = -1;
static void *g_base_map; /* perf base ring (metadata page + data)  */
static size_t g_base_sz;
static void *g_aux_map; /* AUX trace ring                          */
static size_t g_aux_sz;
static hw_region_t *g_active;
#endif

static size_t round_pages(size_t want, size_t dflt) {
    long pg = 4096;
#if defined(__linux__)
    pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        pg = 4096;
#endif
    size_t v = want ? want : dflt;
    size_t pages = (v + (size_t)pg - 1) / (size_t)pg;
    size_t p = 1; /* AUX/data ring needs a power-of-two page count */
    while (p < pages)
        p <<= 1;
    return p * (size_t)pg;
}

int asmtest_hwtrace_init(const asmtest_hwtrace_options_t *opts) {
    if (opts == NULL)
        return ASMTEST_HW_EINVAL;
#if defined(__linux__)
    /* Refuse to re-init while a capture is live: end() dispatches teardown on the
     * CURRENT backend, so switching backend mid-capture would run the wrong end
     * path and leak the old backend's perf fd/mmaps, permanently wedging the tier.
     * The caller must end() the active capture first. */
    if (g_fd >= 0 || g_active != NULL)
        return ASMTEST_HW_ESTATE;
#endif
    if (!asmtest_hwtrace_available(opts->backend))
        return ASMTEST_HW_EUNAVAIL;
    g_opts = *opts;
    g_nregions = 0;
    memset(g_regions, 0, sizeof g_regions);
    g_inited = 1;
    return ASMTEST_HW_OK;
}

int asmtest_hwtrace_register_region(const char *name, void *base, size_t len,
                                    asmtest_trace_t *trace) {
    if (!g_inited)
        return ASMTEST_HW_ESTATE;
    if (name == NULL || base == NULL || len == 0 || trace == NULL)
        return ASMTEST_HW_EINVAL;
    if (g_nregions >= MAX_REGIONS)
        return ASMTEST_HW_EFULL;
    hw_region_t *r = &g_regions[g_nregions++];
    snprintf(r->name, sizeof r->name, "%s", name);
    r->used = 1;
    r->base = base;
    r->len = len;
    r->trace = trace;
    return ASMTEST_HW_OK;
}

static hw_region_t *find_region(const char *name) {
    for (int i = 0; i < g_nregions; i++)
        if (g_regions[i].used && strcmp(g_regions[i].name, name) == 0)
            return &g_regions[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* AMD branch-record capture (data ring, NOT AUX)                      */
/*                                                                     */
/* AMD branch records arrive as PERF_RECORD_SAMPLE records in the base */
/* (data) ring, each carrying a perf_branch_stack ({nr, entries[]}).   */
/* sample_period=1 emits a sample at every taken branch, so the sample */
/* at the region's last branch holds the complete <=16-entry history.  */
/* ------------------------------------------------------------------ */
#if defined(__linux__) && defined(__x86_64__)
static int hwtrace_begin_amd(hw_region_t *r) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = PERF_TYPE_HARDWARE;
    a.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
    a.sample_period = 1;
    a.sample_type = PERF_SAMPLE_BRANCH_STACK;
    a.branch_sample_type = PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_ANY;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    a.disabled = 1;
    long fd = perf_open(&a, 0, -1, -1, 0);
    if (fd < 0)
        return -1;
    g_fd = (int)fd;
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        pg = 4096;
    g_base_sz = (size_t)pg + round_pages(g_opts.data_size, 64 * 1024);
    g_base_map =
        mmap(NULL, g_base_sz, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (g_base_map == MAP_FAILED) {
        g_base_map = NULL;
        close(g_fd);
        g_fd = -1;
        return -1;
    }
    g_aux_map = NULL; /* AMD uses no AUX ring */
    g_aux_sz = 0;
    g_active = r;
    ioctl(g_fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(g_fd, PERF_EVENT_IOC_ENABLE, 0);
    return 0;
}

static void hwtrace_end_amd(void) {
    ioctl(g_fd, PERF_EVENT_IOC_DISABLE, 0);
    struct perf_event_mmap_page *mp = (struct perf_event_mmap_page *)g_base_map;
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        pg = 4096;
    uint8_t *data = (uint8_t *)g_base_map + (size_t)pg;
    size_t dsz = g_base_sz - (size_t)pg;
    uint64_t head = mp->data_head;
    __sync_synchronize(); /* read data_head before the records (smp_rmb) */
    uint64_t tail = mp->data_tail;
    hw_region_t *r = g_active;

    /* Linearize [tail, head) (it may wrap the circular data ring) into scratch,
     * then walk perf_event_header records to pick the branch sample to decode.
     *
     * With sample_period=1 every taken branch emits a sample whose branch stack is
     * the 16 most-recent branches AT THAT POINT. A small registered routine's own
     * branches live in the stack only briefly: the post-routine glue (returning
     * into asmtest_hwtrace_end and its callees, before the perf DISABLE) is itself
     * a dozen-plus taken branches that push the routine's branches out of the
     * 16-deep window. So the LAST sample is all glue and decodes to nothing —
     * instead keep the sample with the MOST in-region branch entries: the one taken
     * at/just after the routine, whose window still holds its jcc/ret. (Verified on
     * a Zen 5 host: last-sample gave insns_total=0; richest-sample reconstructs the
     * exact stream.) */
    const uint64_t base_ip = (uint64_t)(uintptr_t)r->base;
    const uint64_t end_ip = base_ip + r->len;
    size_t span = (size_t)(head - tail);
    struct perf_branch_entry *best = NULL;
    uint64_t best_nr = 0;
    size_t best_inregion = 0;
    int lost = 0; /* a dropped-sample record: the ring could not hold the run */
    size_t n_samples = 0; /* branch-stack samples, time order (tail -> head) */
    struct perf_branch_entry **samples = NULL;
    size_t *nrs = NULL;
    uint8_t *buf = NULL;
    if (span > 0 && span <= dsz) {
        buf = (uint8_t *)malloc(span);
        if (buf != NULL) {
            for (size_t i = 0; i < span; i++)
                buf[i] = data[(tail + i) % dsz];

            /* Pass 1: count samples and detect drops (ring overflow / rate throttle).
             * The data ring is non-overwrite, so on overflow the kernel drops the
             * NEWEST samples and emits PERF_RECORD_LOST — the precise "the run did not
             * fit" signal that the surviving windows alone cannot show (they stitch
             * gaplessly yet are missing the tail). */
            for (size_t off = 0;
                 off + sizeof(struct perf_event_header) <= span;) {
                struct perf_event_header *h =
                    (struct perf_event_header *)(buf + off);
                if (h->size == 0 || off + h->size > span)
                    break;
                if (h->type == PERF_RECORD_SAMPLE)
                    n_samples++;
                else if (h->type == PERF_RECORD_LOST ||
                         h->type == PERF_RECORD_THROTTLE)
                    lost = 1;
                off += h->size;
            }
            if (n_samples > 0) {
                samples = (struct perf_branch_entry **)malloc(n_samples *
                                                              sizeof *samples);
                nrs = (size_t *)malloc(n_samples * sizeof *nrs);
            }

            /* Pass 2: record each sample in time order (for Tier-B stitching) and
             * track the single richest-in-region window (Tier-A pick / fallback). */
            size_t si = 0;
            for (size_t off = 0;
                 off + sizeof(struct perf_event_header) <= span;) {
                struct perf_event_header *h =
                    (struct perf_event_header *)(buf + off);
                if (h->size == 0 || off + h->size > span)
                    break;
                if (h->type == PERF_RECORD_SAMPLE) {
                    /* Only PERF_SAMPLE_BRANCH_STACK is set, so the body is
                     * {u64 nr; perf_branch_entry[nr]}. */
                    uint8_t *body = buf + off + sizeof *h;
                    uint64_t nr = *(uint64_t *)body;
                    if (nr > 0 &&
                        sizeof *h + sizeof(uint64_t) +
                                nr * sizeof(struct perf_branch_entry) <=
                            h->size) {
                        struct perf_branch_entry *e =
                            (struct perf_branch_entry *)(body +
                                                         sizeof(uint64_t));
                        if (samples != NULL && nrs != NULL) {
                            samples[si] = e;
                            nrs[si] = (size_t)nr;
                            si++;
                        }
                        size_t inregion = 0;
                        for (uint64_t i = 0; i < nr; i++)
                            if ((e[i].from >= base_ip && e[i].from < end_ip) ||
                                (e[i].to >= base_ip && e[i].to < end_ip))
                                inregion++;
                        /* Richest-in-region wins; on a tie keep the earliest (closest
                         * to the routine, least surrounding glue). */
                        if (best == NULL || inregion > best_inregion) {
                            best = e;
                            best_nr = nr;
                            best_inregion = inregion;
                        }
                    }
                }
                off += h->size;
            }
            n_samples = si; /* only well-formed samples retained */
        }
    }

    /* The data ring is never drained mid-capture (data_tail only advances at the
     * very end, line below), so the kernel never gets the next successful
     * reservation needed to emit a pending PERF_RECORD_LOST — a ring that filled
     * therefore shows NO loss record even though it dropped the newest samples
     * (the run's tail, where sample_period=1 windows would otherwise stitch
     * gaplessly). Treat a (near-)full ring as loss: if less than one maximum-size
     * branch-stack sample of headroom remains, the tail was almost certainly
     * dropped and the trace must be honestly truncated rather than claimed complete. */
    {
        size_t max_sample =
            sizeof(struct perf_event_header) + sizeof(uint64_t) +
            (size_t)AMD_LBR_DEPTH * sizeof(struct perf_branch_entry);
        if (dsz > 0 && span + max_sample > dsz)
            lost = 1;
    }

    /* Decode. Tier-A (the single richest in-region window) is complete when the
     * routine's branches fit one 16-deep stack (best_nr < AMD_LBR_DEPTH) — the common
     * small-routine case, unchanged. When that window overflowed (best_nr >= depth)
     * the routine took more taken branches than the stack is deep, so escalate to
     * Tier-B: stitch the overlapping sample_period=1 windows (collected above, time
     * order) into one gapless sequence and decode THAT past the ceiling. A stitch gap
     * or a dropped-sample record (`lost`) means the ring could not hold the whole run,
     * so the result is honestly truncated; otherwise Tier-B reconstructs the full
     * >16-branch trace where a single window cannot. */
    int done = 0;
    if (best != NULL && best_nr > 0 && best_inregion > 0) {
        if (best_nr >= AMD_LBR_DEPTH && n_samples > 1 && samples != NULL &&
            nrs != NULL) {
            size_t out_cap = n_samples + AMD_LBR_DEPTH;
            struct perf_branch_entry *out =
                (struct perf_branch_entry *)malloc(out_cap * sizeof *out);
            if (out != NULL) {
                int gap = 0;
                size_t st = asmtest_amd_stitch(
                    (const struct perf_branch_entry *const *)samples, nrs,
                    n_samples, out, out_cap, &gap);
                if (st > 0) {
                    asmtest_amd_decode_stitched(out, st, r->base, r->len,
                                                r->trace, gap || lost);
                    done = 1;
                }
                free(out);
            }
        }
        if (!done)
            asmtest_amd_decode(best, (size_t)best_nr, r->base, r->len,
                               r->trace);
    } else if (r->trace != NULL) {
        r->trace->truncated =
            true; /* no in-region branches captured: not complete */
    }

    /* A dropped-sample record, or a ring that filled (detected above), means the
     * capture holds only a prefix of the run — flag it truncated regardless of
     * which tier decoded, so the single-window Tier-A path can't report a
     * ring-truncated capture as complete. */
    if (lost && r->trace != NULL)
        r->trace->truncated = true;

    free(samples);
    free(nrs);
    free(buf);
    mp->data_tail = head; /* consume */
    munmap(g_base_map, g_base_sz);
    close(g_fd);
    g_base_map = NULL;
    g_fd = -1;
    g_active = NULL;
}
#endif /* __linux__ && __x86_64__ */

/* ------------------------------------------------------------------ */
/* Capture lifecycle (Intel PT via perf AUX; CoreSight is analogous)   */
/* ------------------------------------------------------------------ */

void asmtest_hwtrace_begin(const char *name) {
#if defined(__linux__)
    if (!g_inited || g_fd >= 0 || g_active != NULL)
        return; /* MVP: one active region at a time (g_active covers the fd-less
                 * single-step backend, whose capture uses no perf fd) */
    hw_region_t *r = find_region(name);
    if (r == NULL)
        return;
#if defined(__x86_64__)
    if (g_opts.backend == ASMTEST_HWTRACE_SINGLESTEP) {
        g_active =
            r; /* set before arming TF: the handler reads it immediately */
        asmtest_ss_begin(r->base, r->len, r->trace);
        return;
    }
    if (g_opts.backend == ASMTEST_HWTRACE_AMD_LBR) {
        hwtrace_begin_amd(r);
        return;
    }
#endif
    int type = pmu_type(g_opts.backend);
    if (type < 0)
        return;
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.size = sizeof attr;
    attr.type = (uint32_t)type;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.disabled = 1;
    long fd = perf_open(&attr, 0, -1, -1, 0);
    if (fd < 0)
        return;
    g_fd = (int)fd;

    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        pg = 4096;
    g_base_sz = (size_t)pg + round_pages(g_opts.data_size, 8 * 1024);
    g_base_map =
        mmap(NULL, g_base_sz, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
    if (g_base_map == MAP_FAILED) {
        g_base_map = NULL;
        close(g_fd);
        g_fd = -1;
        return;
    }
    struct perf_event_mmap_page *mp = (struct perf_event_mmap_page *)g_base_map;
    g_aux_sz = round_pages(g_opts.aux_size, 64 * 1024);
    mp->aux_offset = g_base_sz;
    mp->aux_size = g_aux_sz;
    /* PROT_READ-only AUX is a circular snapshot ring; RW is a linear ring. */
    int aprot = g_opts.snapshot ? PROT_READ : (PROT_READ | PROT_WRITE);
    g_aux_map = mmap(NULL, g_aux_sz, aprot, MAP_SHARED, g_fd, (off_t)g_base_sz);
    if (g_aux_map == MAP_FAILED) {
        g_aux_map = NULL;
        munmap(g_base_map, g_base_sz);
        g_base_map = NULL;
        close(g_fd);
        g_fd = -1;
        return;
    }
    g_active = r;
    ioctl(g_fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(g_fd, PERF_EVENT_IOC_ENABLE, 0);
#else
    (void)name;
#endif
}

/* Scan the base (data) ring for PERF_RECORD_AUX records and report whether any
 * carried PERF_AUX_FLAG_TRUNCATED — the precise "AUX trace was lost" signal that
 * complements the head>=size heuristic. */
#if defined(__linux__)
static int aux_data_ring_truncated(void) {
    struct perf_event_mmap_page *mp = (struct perf_event_mmap_page *)g_base_map;
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        pg = 4096;
    uint8_t *data = (uint8_t *)g_base_map + (size_t)pg;
    size_t dsz = g_base_sz - (size_t)pg;
    uint64_t dhead = mp->data_head;
    __sync_synchronize();
    uint64_t dtail = mp->data_tail;
    int trunc = 0;
    for (uint64_t off = dtail;
         off + sizeof(struct perf_event_header) <= dhead;) {
        struct perf_event_header h;
        for (size_t i = 0; i < sizeof h; i++)
            ((uint8_t *)&h)[i] = data[(off + i) % dsz];
        if (h.size < sizeof h)
            break;
        if (h.type == PERF_RECORD_AUX) {
            /* body: u64 aux_offset, u64 aux_size, u64 flags */
            uint64_t flags = 0;
            uint64_t fpos = off + sizeof h + 2 * sizeof(uint64_t);
            for (size_t i = 0; i < sizeof flags; i++)
                ((uint8_t *)&flags)[i] = data[(fpos + i) % dsz];
            if (flags & PERF_AUX_FLAG_TRUNCATED)
                trunc = 1;
        }
        off += h.size;
    }
    mp->data_tail = dhead; /* consume */
    return trunc;
}
#endif

void asmtest_hwtrace_end(const char *name) {
#if defined(__linux__)
    (void)name;
    if (g_active == NULL) {
        /* No active region, but release any orphaned perf fd/mmaps (defensive:
         * the init guard now prevents the mismatched-teardown path that could
         * strand them) so the tier can't be permanently wedged. */
        if (g_fd >= 0) {
            ioctl(g_fd, PERF_EVENT_IOC_DISABLE, 0);
            if (g_aux_map != NULL)
                munmap(g_aux_map, g_aux_sz);
            if (g_base_map != NULL)
                munmap(g_base_map, g_base_sz);
            close(g_fd);
            g_aux_map = NULL;
            g_base_map = NULL;
            g_fd = -1;
        }
        return;
    }
#if defined(__x86_64__)
    if (g_opts.backend == ASMTEST_HWTRACE_SINGLESTEP) {
        asmtest_ss_end(); /* disarms TF + restores SIGTRAP; trace already filled */
        g_active = NULL;
        return;
    }
    if (g_opts.backend == ASMTEST_HWTRACE_AMD_LBR) {
        hwtrace_end_amd();
        return;
    }
#endif
    if (g_fd < 0)
        return; /* PT/CoreSight need the perf fd; nothing captured if open failed */
    ioctl(g_fd, PERF_EVENT_IOC_DISABLE, 0);
    struct perf_event_mmap_page *mp = (struct perf_event_mmap_page *)g_base_map;
    /* Linear ring: valid trace is [0, aux_head). (Snapshot decode would walk the
     * circular ring from aux_tail; left to the snapshot-mode follow-up.) */
    uint64_t head = mp->aux_head;
    /* Precise overflow: PERF_AUX_FLAG_TRUNCATED on a PERF_RECORD_AUX in the data
     * ring; plus the head>=size heuristic as a backstop for the tiny-buffer case. */
    int overflow = aux_data_ring_truncated();
    if (head >= g_aux_sz) {
        head = g_aux_sz;
        overflow = 1;
    }
    hw_region_t *r = g_active;
    int rc = (g_opts.backend == ASMTEST_HWTRACE_INTEL_PT)
                 ? asmtest_pt_decode((const uint8_t *)g_aux_map, (size_t)head,
                                     r->base, r->len, r->trace)
                 : asmtest_cs_decode((const uint8_t *)g_aux_map, (size_t)head,
                                     r->base, r->len, r->trace);
    if (r->trace != NULL && (overflow || rc != ASMTEST_HW_OK))
        r->trace->truncated = true;

    munmap(g_aux_map, g_aux_sz);
    munmap(g_base_map, g_base_sz);
    close(g_fd);
    g_aux_map = NULL;
    g_base_map = NULL;
    g_fd = -1;
    g_active = NULL;
#else
    (void)name;
#endif
}

void asmtest_hwtrace_shutdown(void) {
#if defined(__linux__)
    /* g_active covers an unbalanced region for every backend, including the
     * single-step one (g_fd stays -1): end() must still run to disarm TF. */
    if (g_fd >= 0 || g_active != NULL)
        asmtest_hwtrace_end(NULL);
#endif
    g_inited = 0;
    g_nregions = 0;
}

/* ------------------------------------------------------------------ */
/* W^X executable-memory helper (self-contained; for language bindings) */
/* ------------------------------------------------------------------ */
int asmtest_hwtrace_exec_alloc(const void *bytes, size_t len, void **base_out,
                               size_t *len_out) {
#if defined(__linux__)
    if (bytes == NULL || len == 0 || base_out == NULL)
        return ASMTEST_HW_EINVAL;
    /* PROT_NONE first, then RW to copy, then RX: never simultaneously W and X. */
    void *p = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return ASMTEST_HW_EUNAVAIL;
    if (mprotect(p, len, PROT_READ | PROT_WRITE) != 0) {
        munmap(p, len);
        return ASMTEST_HW_EUNAVAIL;
    }
    memcpy(p, bytes, len);
    if (mprotect(p, len, PROT_READ | PROT_EXEC) != 0) {
        munmap(p, len);
        return ASMTEST_HW_EUNAVAIL;
    }
    __builtin___clear_cache((char *)p, (char *)p + len);
    *base_out = p;
    if (len_out != NULL)
        *len_out = len;
    return ASMTEST_HW_OK;
#else
    (void)bytes;
    (void)len;
    (void)base_out;
    (void)len_out;
    return ASMTEST_HW_ENOSYS;
#endif
}

void asmtest_hwtrace_exec_free(void *base, size_t len) {
#if defined(__linux__)
    if (base != NULL && len != 0)
        munmap(base, len);
#else
    (void)base;
    (void)len;
#endif
}
