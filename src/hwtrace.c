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

/* Whether each decoder was compiled in (queried by available()). */
int asmtest_pt_decoder_present(void);
int asmtest_cs_decoder_present(void);

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
    return b == ASMTEST_HWTRACE_INTEL_PT ? asmtest_pt_decoder_present()
                                         : asmtest_cs_decoder_present();
}

/* CPU/ISA/vendor check: Intel PT needs GenuineIntel x86-64; CoreSight needs
 * AArch64. */
static int cpu_matches(asmtest_trace_backend_t b) {
    if (b == ASMTEST_HWTRACE_CORESIGHT) {
#if defined(__aarch64__)
        return 1;
#else
        return 0;
#endif
    }
#if defined(__x86_64__)
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f == NULL)
        return 0;
    char line[256];
    int intel = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "vendor_id", 9) == 0) {
            intel = strstr(line, "GenuineIntel") != NULL;
            break;
        }
    }
    fclose(f);
    return intel;
#else
    return 0;
#endif
}

#if defined(__linux__)
static long perf_open(struct perf_event_attr *a, pid_t pid, int cpu, int group,
                      unsigned long flags) {
    return syscall(SYS_perf_event_open, a, pid, cpu, group, flags);
}
#endif

/* A real privilege probe: try to open the PMU event disabled, then close it. */
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
    return decoder_present(backend) && cpu_matches(backend) &&
           pmu_type(backend) >= 0 && perf_permitted(backend);
}

void asmtest_hwtrace_skip_reason(asmtest_trace_backend_t backend, char *buf,
                                 size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *r;
    if (!decoder_present(backend))
        r = (backend == ASMTEST_HWTRACE_INTEL_PT)
                ? "built without libipt"
                : "built without OpenCSD";
    else if (!cpu_matches(backend))
        r = (backend == ASMTEST_HWTRACE_INTEL_PT)
                ? "not a GenuineIntel x86-64 host"
                : "not an AArch64 host";
    else if (pmu_type(backend) < 0)
        r = (backend == ASMTEST_HWTRACE_INTEL_PT)
                ? "no intel_pt PMU (needs bare-metal Intel; absent on AMD/VM)"
                : "no cs_etm PMU (needs a CoreSight-capable AArch64 board)";
    else if (!perf_permitted(backend))
        r = "perf_event capture not permitted (lower perf_event_paranoid or "
            "grant CAP_PERFMON)";
    else
        r = "available";
    snprintf(buf, buflen, "%s", r);
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
/* Capture lifecycle (Intel PT via perf AUX; CoreSight is analogous)   */
/* ------------------------------------------------------------------ */

void asmtest_hwtrace_begin(const char *name) {
#if defined(__linux__)
    if (!g_inited || g_fd >= 0)
        return; /* MVP: one active region at a time */
    hw_region_t *r = find_region(name);
    if (r == NULL)
        return;
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
    g_base_map = mmap(NULL, g_base_sz, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
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

void asmtest_hwtrace_end(const char *name) {
#if defined(__linux__)
    (void)name;
    if (g_fd < 0 || g_active == NULL)
        return;
    ioctl(g_fd, PERF_EVENT_IOC_DISABLE, 0);
    struct perf_event_mmap_page *mp = (struct perf_event_mmap_page *)g_base_map;
    /* Linear ring: valid trace is [0, aux_head). (Snapshot decode would walk the
     * circular ring from aux_tail; left to the snapshot-mode follow-up.) */
    uint64_t head = mp->aux_head;
    if (head > g_aux_sz)
        head = g_aux_sz;
    hw_region_t *r = g_active;
    int rc = (g_opts.backend == ASMTEST_HWTRACE_INTEL_PT)
                 ? asmtest_pt_decode((const uint8_t *)g_aux_map, (size_t)head,
                                     r->base, r->len, r->trace)
                 : asmtest_cs_decode((const uint8_t *)g_aux_map, (size_t)head,
                                     r->base, r->len, r->trace);
    if (rc != ASMTEST_HW_OK && r->trace != NULL)
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
    if (g_fd >= 0)
        asmtest_hwtrace_end(NULL);
#endif
    g_inited = 0;
    g_nregions = 0;
}
