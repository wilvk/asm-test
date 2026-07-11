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
#include "amd_backend.h" /* shared AMD branch-record backend decls + reduced-filter macro */
#include "asmtest_addr_channel.h" /* §D3 windowed multi-region channel */
#include "asmtest_codeimage.h"
#include "asmtest_hwtrace.h"
#include "asmtest_ptrace.h" /* §D3 stealth stepper reuses the W2 attach tracer */
#include "stealth_helper.h" /* §D3 shared stepping body + bundled-binary discovery */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__x86_64__) || defined(__aarch64__)
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#endif
#endif

#if defined(__APPLE__)
#include <pthread.h> /* g_reg_lock, the g_arm_tid __thread, pthread_threadid_np */
#include <sys/mman.h> /* asmtest_hwtrace_exec_alloc W^X mmap/mprotect (x86-64 Darwin) */
#endif

/* The EFLAGS.TF single-step tier (ss_backend.c) runs on x86-64 Linux AND macOS; the
 * Intel PT / AMD LBR / CoreSight / perf machinery is Linux-only. Two gates keep the
 * split honest without duplicating the facade:
 *   HWTRACE_HAVE_SINGLESTEP — x86-64 Linux OR macOS: the stepper + its handle path.
 *   HWTRACE_LIFECYCLE       — the shared facade (region table + mutex, init/register,
 *                             begin/end/scope single-step dispatch, arm-tid backstop).
 * On Linux HWTRACE_LIFECYCLE is ALWAYS set (it is a superset of __linux__), so every
 * Linux code path below is byte-for-byte unchanged; each perf/PT/AMD block stays behind
 * a narrower __linux__ guard, so it simply drops out of the macOS build. */
#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))
#define HWTRACE_HAVE_SINGLESTEP 1
#endif
#if defined(__linux__) || defined(HWTRACE_HAVE_SINGLESTEP)
#define HWTRACE_LIFECYCLE 1
#endif

/* Portable OS thread id for the arming-thread backstop: SYS_gettid on Linux,
 * pthread_threadid_np on macOS. Only ever compared for equality (a cross-thread-close
 * tripwire), never used as an index, so the 64->32 bit narrowing on macOS is benign. */
#if defined(HWTRACE_LIFECYCLE)
static int hw_current_tid(void) {
#if defined(__linux__)
    return (int)syscall(SYS_gettid);
#else
    __uint64_t t = 0;
    pthread_threadid_np(NULL, &t);
    return (int)t;
#endif
}
#endif

/* Decoder entry points (defined in pt_backend.c / cs_backend.c, or the no-decoder
 * stubs below). Decode the raw AUX bytes against [base, base+len) into *trace. */
int asmtest_pt_decode(const uint8_t *aux, size_t aux_len, const void *base,
                      size_t len, asmtest_trace_t *trace);
int asmtest_cs_decode(const uint8_t *aux, size_t aux_len, const void *base,
                      size_t len, asmtest_trace_t *trace);

/* AMD branch-record decode, Tier-B stitch, runtime depth, and boundary-snapshot
 * begin/end decls live in the shared internal header "amd_backend.h" (included above). */

/* Single-step (EFLAGS.TF / SIGTRAP) stepper (ss_backend.c). Unlike the trace
 * backends there is no post-pass decode: begin() arms TF and the SIGTRAP handler
 * fills the trace live; end() disarms. base/len bound the region, trace is the
 * sink (block normalization needs the Capstone length-decoder). */
#if defined(HWTRACE_HAVE_SINGLESTEP)
int asmtest_ss_begin(const void *base, size_t len, asmtest_trace_t *trace);
/* §1: handle-producing push (per-thread range stack) + calling-thread frame lookup. */
int asmtest_ss_begin_ex(const void *base, size_t len, asmtest_trace_t *trace,
                        uint32_t *out_idx, uint32_t *out_gen);
/* §Z1: region-free whole-window push (records absolute RIPs; no [base,len)). */
int asmtest_ss_begin_window(asmtest_trace_t *trace, uint32_t *out_idx,
                            uint32_t *out_gen);
/* B (lazy-arm): arm [base,len), dispatch fn(args…) natively, disarm — nothing the
 * caller runs between arm and disarm is stepped (see managed-singlestep-lazy-arm). */
int asmtest_ss_call_scoped(const void *base, size_t len, asmtest_trace_t *trace,
                           void *fn, const long *args, int nargs,
                           long *result_out, uint32_t *out_idx,
                           uint32_t *out_gen);
/* FP sibling: the (double…)->double shim family (xmm0..7 args, xmm0 return). */
int asmtest_ss_call_scoped_fp(const void *base, size_t len,
                              asmtest_trace_t *trace, void *fn,
                              const double *args, int nargs, double *result_out,
                              uint32_t *out_idx, uint32_t *out_gen);
int asmtest_ss_frame_lookup(uint32_t idx, uint32_t gen, const void **base,
                            size_t *len, asmtest_trace_t **trace);
void asmtest_ss_end(void);
#endif

/* Whether each decoder was compiled in (queried by available()). */
int asmtest_pt_decoder_present(void);
int asmtest_cs_decoder_present(void);
/* asmtest_amd_decoder_present / _freeze_available / _snapshot_available are declared in
 * the shared "amd_backend.h". */

/* ------------------------------------------------------------------ */
/* Gating: detect-and-skip                                             */
/* ------------------------------------------------------------------ */

#if defined(__linux__)
static const char *pmu_name(asmtest_trace_backend_t b) {
    return b == ASMTEST_HWTRACE_INTEL_PT ? "intel_pt" : "cs_etm";
}
#endif

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
        /* TF/#DB single-step is baseline x86-64 — any vendor, Intel or AMD — and the
         * capture lifecycle (begin/end, asmtest_ss_*) is implemented for x86-64 Linux
         * AND macOS (both deliver the #DB as an in-process SIGTRAP). Elsewhere it is a
         * no-op stub, so gate to HWTRACE_HAVE_SINGLESTEP: available() then self-skips
         * (with the "x86-64 Linux/macOS only" reason) on Windows/AArch64/etc. instead
         * of reporting available and yielding an empty "complete" trace. */
#if defined(HWTRACE_HAVE_SINGLESTEP)
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
                ? "single-step backend is x86-64 Linux/macOS only "
                  "(Windows/AArch64 "
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
    int arm_tid; /* §1: OS tid that armed this region (single-step backstop); 0=idle */
} hw_region_t;

static hw_region_t g_regions[MAX_REGIONS];
static int g_nregions = 0;
static asmtest_hwtrace_options_t g_opts;
static int g_inited = 0;

/* §1: the region registry is process-global and shared; §0.4's idempotent
 * register_region is a find-then-append read-modify-write over it, and find_region
 * scans it — a data race once concurrent multi-thread scopes are blessed. A plain
 * mutex (registration is off the hot capture path; the single-step handler never
 * touches the registry, so no async-signal-safety constraint) makes it safe. */
#if defined(HWTRACE_LIFECYCLE)
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static hw_region_t *find_region(const char *name); /* locked; below */
static hw_region_t *
find_region_unlocked(const char *name); /* caller holds lock */

/* §1: per-thread active-capture state for the PT / AMD LBR / CoreSight backends.
 * These are `__thread` so concurrent scopes on different threads each own their perf
 * fd + rings (the perf event is already per-thread, `pid == 0`), lifting the
 * process-global single-region limit for the hardware backends too — the single-step
 * backend carries its own per-thread range stack in ss_backend.c. General-dynamic TLS
 * is fine here: unlike the single-step handler, none of this is touched in signal
 * context. Per-thread `g_active` also makes a cross-thread close impossible (the
 * closing thread's slot is empty), the same isolation the single-step frames give. */
#if defined(__linux__)
static __thread int g_fd = -1;
static __thread void *g_base_map; /* perf base ring (metadata page + data)  */
static __thread size_t g_base_sz;
static __thread void *g_aux_map; /* AUX trace ring                          */
static __thread size_t g_aux_sz;
static __thread hw_region_t *g_active;
#endif
/* §0.2: OS thread id that armed this thread's active capture, -1 idle. Shared by the
 * single-step backstop too (set in the ss begin/end paths), so it lives outside the
 * Linux-only perf block above — the macOS single-step build reads/writes it. */
#if defined(HWTRACE_LIFECYCLE)
static __thread int g_arm_tid = -1;
#endif

/* AUX/data-ring sizing for the perf backends — Linux-only (its callers are the
 * PT/AMD perf paths). Left out of the macOS single-step build entirely. */
#if defined(__linux__)
static size_t round_pages(size_t want, size_t dflt) {
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        pg = 4096;
    size_t v = want ? want : dflt;
    size_t pages = (v + (size_t)pg - 1) / (size_t)pg;
    size_t p = 1; /* AUX/data ring needs a power-of-two page count */
    while (p < pages)
        p <<= 1;
    return p * (size_t)pg;
}
#endif

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
        /* §0.4: idempotent by name. A self-naming scope object registers on EVERY
     * construction under a call-site-constant auto-name, so a `using`/RAII scope in
     * a loop — or more than MAX_REGIONS scope sites over process lifetime — would
     * otherwise exhaust the fixed table or alias a stale earlier duplicate. On a
     * name that already has a slot, refresh its [base, len) + trace in place and
     * return it instead of appending: repeated entry reuses one slot, g_nregions
     * never grows, and the MAX_REGIONS ceiling counts DISTINCT scope sites. (This
     * find-then-refresh-or-append is a read-modify-write over g_regions; §1 runs it
     * under the registry mutex once concurrent scopes are blessed.) */
#if defined(HWTRACE_LIFECYCLE)
    pthread_mutex_lock(&g_reg_lock); /* §1: RMW over the shared registry */
#endif
    hw_region_t *existing = find_region_unlocked(name);
    if (existing != NULL) {
        existing->base = base;
        existing->len = len;
        existing->trace = trace;
#if defined(HWTRACE_LIFECYCLE)
        pthread_mutex_unlock(&g_reg_lock);
#endif
        return ASMTEST_HW_OK;
    }
    if (g_nregions >= MAX_REGIONS) {
#if defined(HWTRACE_LIFECYCLE)
        pthread_mutex_unlock(&g_reg_lock);
#endif
        return ASMTEST_HW_EFULL;
    }
    hw_region_t *r = &g_regions[g_nregions++];
    snprintf(r->name, sizeof r->name, "%s", name);
    r->used = 1;
    r->base = base;
    r->len = len;
    r->trace = trace;
    r->arm_tid = 0; /* idle until begin */
#if defined(HWTRACE_LIFECYCLE)
    pthread_mutex_unlock(&g_reg_lock);
#endif
    return ASMTEST_HW_OK;
}

static hw_region_t *find_region_unlocked(const char *name) {
    for (int i = 0; i < g_nregions; i++)
        if (g_regions[i].used && strcmp(g_regions[i].name, name) == 0)
            return &g_regions[i];
    return NULL;
}

static hw_region_t *find_region(const char *name) {
#if defined(HWTRACE_LIFECYCLE)
    pthread_mutex_lock(&g_reg_lock);
#endif
    hw_region_t *r = find_region_unlocked(name);
#if defined(HWTRACE_LIFECYCLE)
    pthread_mutex_unlock(&g_reg_lock);
#endif
    return r;
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
/* ASMTEST_AMD_REDUCED_FILTER (used at branch_sample_type below) is defined once in the
 * shared amd_backend.h — see there for the drop-direct-uncond-jmp rationale (#2B). */

/* Nonzero while the active AMD region is captured by the deterministic boundary
 * snapshot (branchsnap.c) instead of the sampled data ring below. Single-slot,
 * reset in hwtrace_end_amd — the same invariant as g_fd/g_base_map/g_active. */
static int g_amd_snap = 0;

/* The offset of the region's LAST region-EXIT instruction — the exit the boundary
 * snapshot plants its hardware breakpoint on. An exit is a ret-class instruction OR a
 * region-LEAVING direct unconditional jmp (a tail call: `jmp target` whose target is
 * outside [base, base+len)). Walks the registered bytes with the Capstone length-decoder;
 * returns (size_t)-1 when no exit is found before the walk ends or a byte fails to decode
 * (tails past undecodable padding simply fall back to the sampled path; a breakpoint on a
 * wrong exit is harmless — the boundary is never hit and end() reports an honest
 * truncated). When `nexit` is non-NULL it receives the total exit count — the caller uses
 * `*nexit == 1` to detect a SINGLE-exit region, where the one breakpoint is guaranteed to
 * be hit on any normal completion.
 *
 * Both tail-call guards are load-bearing: asmtest_disas_is_uncond_jump also matches an
 * INDIRECT `jmp r/m` (an unprovable jump-table tail call, which must stay on the sampled
 * fallback), so a DIRECT jmp is isolated by ALSO requiring
 * asmtest_disas_branch_target()==1; and an IN-region direct uncond jmp is an ordinary
 * loop/forward branch, not an exit, so it must NOT count (else a loopy single-ret region
 * misreads as multi-exit). A region with one `ret` AND one tail-`jmp` is correctly two
 * exits, and default-on is withheld. Non-static (declared in amd_backend.h) for the
 * host-independent tail-call exit-classification test. */
size_t asmtest_amd_last_exit_off(const void *base, size_t len, int *nexit) {
    if (nexit != NULL)
        *nexit = 0;
    if (!asmtest_disas_available())
        return (size_t)-1;
    const uint64_t base_ip = (uint64_t)(uintptr_t)base;
    const uint64_t end_ip = base_ip + len;
    size_t o = 0, last_exit = (size_t)-1;
    while (o < len) {
        size_t l = asmtest_disas(ASMTEST_ARCH_X86_64, (const uint8_t *)base,
                                 len, base_ip, o, NULL, 0);
        if (l == 0)
            break;
        int is_exit = asmtest_disas_is_ret(ASMTEST_ARCH_X86_64,
                                           (const uint8_t *)base, len, o);
        if (!is_exit &&
            asmtest_disas_is_uncond_jump(ASMTEST_ARCH_X86_64,
                                         (const uint8_t *)base, len, o)) {
            uint64_t tgt = 0;
            if (asmtest_disas_branch_target(ASMTEST_ARCH_X86_64,
                                            (const uint8_t *)base, len, base_ip,
                                            o, &tgt) &&
                (tgt < base_ip || tgt >= end_ip))
                is_exit = 1; /* region-leaving direct jmp = tail-call exit */
        }
        if (is_exit) {
            last_exit = o;
            if (nexit != NULL)
                (*nexit)++;
        }
        o += l;
    }
    return last_exit;
}

static int hwtrace_begin_amd(hw_region_t *r) {
    /* Deterministic boundary snapshot (AMD plan Phase 3): read the frozen 16-entry
     * stack at the region's exit breakpoint instead of flooding sample_period=1 PMIs
     * and guessing the richest window — the tiny single-shot routine the sampled path
     * honestly truncates reconstructs completely here, with no post-glue window
     * contamination. Taken when explicitly requested (opts.snapshot) OR, on the Zen 4/5
     * substrate that supports it (amd_lbr_v2 + perfmon_v2 + Linux >= 6.10, via
     * asmtest_amd_snapshot_available), by DEFAULT for a SINGLE-exit region only. An exit
     * is a ret OR a region-leaving tail-call jmp (asmtest_amd_last_exit_off). The snapshot
     * plants ONE breakpoint at the region's last exit; a MULTI-exit routine that leaves via
     * an EARLIER exit would miss it and honestly truncate — with no fall-through, since
     * snapshot mode is committed — whereas the sampled richest-window path can still
     * reconstruct such a run. So the default-on is gated to a lone exit, where the boundary
     * is guaranteed to be hit; an explicit opts.snapshot is the caller's choice and is
     * honored for any region. Any arm failure — no BPF toolchain, missing
     * CAP_BPF/CAP_PERFMON at load, no decodable exit — falls through to the sampled path
     * unchanged. */
    int amd_nexit = 0;
    size_t exit_off = asmtest_amd_last_exit_off(r->base, r->len, &amd_nexit);
    if ((g_opts.snapshot ||
         (asmtest_amd_snapshot_available() && amd_nexit == 1)) &&
        exit_off != (size_t)-1) {
        if (asmtest_amd_snapshot_begin(r->base, r->len, exit_off,
                                       g_opts.branch_filter) == ASMTEST_HW_OK) {
            g_amd_snap = 1;
            g_active = r; /* g_fd stays -1: no sampled ring in snapshot mode */
            return 0;
        }
    }
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = PERF_TYPE_HARDWARE;
    a.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
    /* sample_period: default 1 (a PMI per branch — the exact, small-window Tier-A/B
     * path, unchanged). opts.lbr_period (opt-in) spaces the PMIs out to extend the
     * stitched window before throttling/ring-overflow truncates it: a period of P
     * leaves the consecutive 16-deep windows overlapping by (depth - P), so the
     * Tier-B stitcher still splices them gaplessly at ~P-times fewer interrupts.
     * Clamp to [1, depth-1] so an overlap always remains — a bad value degrades to a
     * stitch gap (honest truncation), never a silent mis-stitch. */
    unsigned period = 1;
    if (g_opts.lbr_period > 1) {
        int depth = asmtest_amd_lbr_depth();
        int hi = depth > 1 ? depth - 1 : 1;
        period = (unsigned)(g_opts.lbr_period < hi ? g_opts.lbr_period : hi);
    }
    a.sample_period = period;
    a.sample_type = PERF_SAMPLE_BRANCH_STACK;
    /* Default: record every taken branch. opts.branch_filter (opt-in) requests the
     * reduced filter that drops statically-decodable direct uncond jmp edges so each
     * 16-deep window spans more instructions; amd_replay follows those jmps for a
     * byte-identical trace. On EOPNOTSUPP/EINVAL retry with the full filter so the tier
     * stays available (forgoing the stretch). See amd-tracing-plan.md (#2B). */
    a.branch_sample_type =
        g_opts.branch_filter
            ? ASMTEST_AMD_REDUCED_FILTER
            : (PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_ANY);
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    a.disabled = 1;
    long fd = perf_open(&a, 0, -1, -1, 0);
    if (fd < 0 &&
        g_opts.branch_filter) { /* type-filter rejected: fall back to full */
        a.branch_sample_type = PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_ANY;
        fd = perf_open(&a, 0, -1, -1, 0);
    }
    if (fd < 0)
        return -1;
    g_fd = (int)fd;
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        pg = 4096;
    /* The AMD data ring holds the sample_period=1 branch-stack windows the Tier-B
     * stitch splices; its size bounds how long a run reconstructs before the kernel
     * drops the newest samples (a PERF_RECORD_LOST -> honest truncation). 256KB (vs
     * Intel PT's 8KB base ring) buys more gapless stitch reach; raise data_size, and
     * kernel.perf_event_max_sample_rate / kernel.perf_cpu_time_max_percent=0 on the
     * runner, to extend it further (see docs/internal/plans/amd-tracing-plan.md Phase 5). */
    g_base_sz = (size_t)pg + round_pages(g_opts.data_size, 256 * 1024);
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
    if (ioctl(g_fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        munmap(g_base_map, g_base_sz);
        g_base_map = NULL;
        g_active = NULL;
        close(g_fd);
        g_fd = -1;
        return -1;
    }
    return 0;
}

static void hwtrace_end_amd(void) {
    if (g_amd_snap) {
        /* Boundary-snapshot mode: drain + decode the frozen exit window. The same
         * honesty invariant as the sampled path: an empty reconstruction is never
         * a complete empty trace. */
        hw_region_t *sr = g_active;
        if (sr != NULL && sr->trace != NULL) {
            asmtest_amd_snapshot_end(sr->trace);
            if (sr->trace->insns_total == 0)
                sr->trace->truncated = true;
        } else {
            asmtest_amd_snapshot_end(
                NULL); /* no trace: just release the slot */
        }
        g_amd_snap = 0;
        g_active = NULL;
        return;
    }
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
                /* memcpy the record header out of the malloc'd ring into a local: a
                 * plain struct-pointer cast is -fstrict-aliasing UB (the buffer's
                 * effective type is bytes). Alignment is fine (malloc'd, 8-multiple
                 * records), so this is a sanitize-lane fix, not a misalignment guard. */
                struct perf_event_header h;
                memcpy(&h, buf + off, sizeof h);
                if (h.size == 0 || off + h.size > span)
                    break;
                if (h.type == PERF_RECORD_SAMPLE)
                    n_samples++;
                else if (h.type == PERF_RECORD_LOST ||
                         h.type == PERF_RECORD_THROTTLE)
                    lost = 1;
                off += h.size;
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
                struct perf_event_header h;
                memcpy(&h, buf + off, sizeof h);
                if (h.size == 0 || off + h.size > span)
                    break;
                if (h.type == PERF_RECORD_SAMPLE) {
                    /* Only PERF_SAMPLE_BRANCH_STACK is set, so the body is
                     * {u64 nr; perf_branch_entry[nr]}. `e` stays a typed pointer INTO
                     * buf — samples[]/best retain it for the post-loop stitch/decode —
                     * so only the transient nr scalar is memcpy'd out. */
                    uint8_t *body = buf + off + sizeof h;
                    uint64_t nr;
                    memcpy(&nr, body, sizeof nr);
                    if (nr > 0 &&
                        sizeof h + sizeof(uint64_t) +
                                nr * sizeof(struct perf_branch_entry) <=
                            h.size) {
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
                off += h.size;
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
    const int amd_depth = asmtest_amd_lbr_depth();
    {
        size_t max_sample =
            sizeof(struct perf_event_header) + sizeof(uint64_t) +
            (size_t)amd_depth * sizeof(struct perf_branch_entry);
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
        if (best_nr >= (uint64_t)amd_depth && n_samples > 1 &&
            samples != NULL && nrs != NULL) {
            size_t out_cap = n_samples + (size_t)amd_depth;
            struct perf_branch_entry *out =
                (struct perf_branch_entry *)malloc(out_cap * sizeof *out);
            if (out != NULL) {
                int gap = 0;
                size_t st = asmtest_amd_stitch(
                    (const struct perf_branch_entry *const *)samples, nrs,
                    n_samples, r->base, (uint64_t)(uintptr_t)r->base, r->len,
                    out, out_cap, &gap);
                if (st > 0) {
                    asmtest_amd_decode_stitched(out, st, r->base, r->len,
                                                r->trace, gap || lost);
                    done = 1;
                }
                free(out);
            }
        }
        if (!done) {
            asmtest_amd_decode(best, (size_t)best_nr, r->base, r->len,
                               r->trace);
            /* Freeze gate (CPUID 0x80000022 EAX[2]). WITHOUT LBR freeze-on-PMI the
             * sampled stack keeps advancing after the overflow reaches CPL0, so a
             * single Tier-A window may not have halted at the region exit — trust it
             * complete only if it actually CONTAINS the region-exit branch (from
             * in-region, to outside the region, i.e. the routine's ret/tail-jump).
             * Otherwise the exit was not captured, so flag truncated rather than
             * present a possibly-mid-routine window as complete. On a freeze-capable
             * part this check is skipped and behavior is unchanged. */
            if (r->trace != NULL && !asmtest_amd_freeze_available()) {
                int saw_exit = 0;
                for (uint64_t i = 0; i < best_nr; i++) {
                    uint64_t f = best[i].from, t = best[i].to;
                    if (f >= base_ip && f < end_ip &&
                        (t < base_ip || t >= end_ip)) {
                        saw_exit = 1;
                        break;
                    }
                }
                if (!saw_exit)
                    r->trace->truncated = true;
            }
        }
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

    /* Honesty invariant (matches the other backends): an in-region branch window that
     * reconstructs to ZERO instructions — a boundary branch, or endpoints that do not
     * bound a decodable run — is not a complete empty trace. Flag it truncated so the
     * AMD path never reports empty-yet-complete (the intermittent case on a tiny
     * single-shot routine whose branches barely enter the region). */
    if (r->trace != NULL && r->trace->insns_total == 0)
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
/* §D3 statistical AMD-LBR whole-window survey (region-FREE)           */
/* ------------------------------------------------------------------ */
/* Exact whole-window is a hardware dead end on AMD (16-deep stack + non-overwrite ring +
 * throttle truncate a period=1 capture at ~10^2 branches). This is the HONEST AMD whole-
 * window shape: a STATISTICAL branch-stack SURVEY. Arm PERF_SAMPLE_BRANCH_STACK at
 * sample_period=`period` (>1 — the OPPOSITE of the exact region path's default 1, so the
 * event stays under kernel.perf_event_max_sample_rate / perf_cpu_time_max_percent), run
 * run_fn(arg) on the CALLING thread, and collect the ABSOLUTE branch-target endpoints of
 * every drained sample into ips[cap]. No [base,len), no disassembly, no amd_replay — the
 * caller buckets the endpoint PCs by method to get a sample-weighted HOT-METHOD histogram
 * (the AutoFDO/BOLT shape), NOT an exact instruction trace. Crash-proof + out-of-band: no
 * EFLAGS.TF and no SIGTRAP, so it survives managed code the in-process single-step tier is
 * forbidden to step. *nips = endpoint count; *truncated set on a dropped/throttled sample
 * (a coverage signal, not an error). Returns ASMTEST_HW_OK, EINVAL on bad args, EUNAVAIL
 * when the branch-stack event cannot open (no Zen 3+ / no CAP_PERFMON), ENOSYS off
 * x86-64 Linux. */
#if defined(__linux__) && defined(__x86_64__)
int asmtest_hwtrace_sample_window_amd(void (*run_fn)(void *), void *arg,
                                      int period, uint64_t *ips, size_t cap,
                                      size_t *nips, int *truncated) {
    if (run_fn == NULL || ips == NULL || cap == 0)
        return ASMTEST_HW_EINVAL;
    if (nips != NULL)
        *nips = 0;
    if (truncated != NULL)
        *truncated = 0;
    if (period < 2)
        period =
            2; /* period>1 keeps the event under the sample-rate throttle */

    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = PERF_TYPE_HARDWARE;
    a.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
    a.sample_period = (unsigned)period;
    a.sample_type = PERF_SAMPLE_BRANCH_STACK;
    a.branch_sample_type = PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_ANY;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    a.disabled = 1;
    long fd = perf_open(&a, 0, -1, -1, 0); /* pid=0: the calling thread */
    if (fd < 0)
        return ASMTEST_HW_EUNAVAIL;
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        pg = 4096;
    size_t base_sz = (size_t)pg + round_pages(0, 256 * 1024);
    void *base_map =
        mmap(NULL, base_sz, PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
    if (base_map == MAP_FAILED) {
        close((int)fd);
        return ASMTEST_HW_EUNAVAIL;
    }
    ioctl((int)fd, PERF_EVENT_IOC_RESET, 0);
    if (ioctl((int)fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        munmap(base_map, base_sz);
        close((int)fd);
        return ASMTEST_HW_EUNAVAIL;
    }

    run_fn(
        arg); /* the window body (a managed delegate thunk) runs at native speed */

    ioctl((int)fd, PERF_EVENT_IOC_DISABLE, 0);
    struct perf_event_mmap_page *mp = (struct perf_event_mmap_page *)base_map;
    uint8_t *data = (uint8_t *)base_map + (size_t)pg;
    size_t dsz = base_sz - (size_t)pg;
    uint64_t head = mp->data_head;
    __sync_synchronize(); /* read data_head before the records (smp_rmb) */
    uint64_t tail = mp->data_tail;
    size_t span = (size_t)(head - tail);
    size_t n = 0;
    int lost = 0;
    /* Near-full ring = loss. The ring is non-overwrite and data_tail advances only at the
     * end, so a ring that FILLED during run_fn drops the NEWEST samples (the window's tail)
     * and emits NO PERF_RECORD_LOST (the kernel never gets the next reservation). Treat less
     * than one max-size sample of headroom as loss, so a long window that overran the ring is
     * honestly a prefix, not reported complete (mirrors hwtrace_end_amd). */
    {
        int depth = asmtest_amd_lbr_depth();
        size_t max_sample = sizeof(struct perf_event_header) +
                            sizeof(uint64_t) +
                            (size_t)depth * sizeof(struct perf_branch_entry);
        if (dsz > 0 && span + max_sample > dsz)
            lost = 1;
    }
    if (span > 0 && span <= dsz) {
        uint8_t *buf = (uint8_t *)malloc(span);
        if (buf != NULL) {
            for (size_t i = 0; i < span; i++)
                buf[i] = data[(tail + i) % dsz];
            for (size_t off = 0;
                 off + sizeof(struct perf_event_header) <= span;) {
                /* memcpy the header/nr out of the byte ring (strict-aliasing clean);
                 * `e` stays a typed pointer into buf, read transiently in-loop. */
                struct perf_event_header h;
                memcpy(&h, buf + off, sizeof h);
                if (h.size == 0 || off + h.size > span)
                    break;
                if (h.type == PERF_RECORD_LOST ||
                    h.type == PERF_RECORD_THROTTLE) {
                    lost = 1;
                } else if (h.type == PERF_RECORD_SAMPLE) {
                    /* body = {u64 nr; perf_branch_entry[nr]} (only BRANCH_STACK set). */
                    uint8_t *body = buf + off + sizeof h;
                    uint64_t nr;
                    memcpy(&nr, body, sizeof nr);
                    /* Bound nr BEFORE the multiply so a corrupt/huge nr cannot wrap the
                     * size check into a pass and drive an out-of-bounds read (the branch
                     * stack is <=32 deep on any part; 64 is a safe ceiling). */
                    if (nr > 0 && nr <= 64 &&
                        sizeof h + sizeof(uint64_t) +
                                nr * sizeof(struct perf_branch_entry) <=
                            h.size) {
                        struct perf_branch_entry *e =
                            (struct perf_branch_entry *)(body +
                                                         sizeof(uint64_t));
                        for (uint64_t i = 0; i < nr && n < cap; i++) {
                            if (e[i].abort)
                                continue; /* transactional abort: not executed */
                            /* Record the branch TARGET (block head / method entry) —
                             * the label the caller buckets by method for hotness. */
                            ips[n++] = e[i].to;
                        }
                        if (n >= cap)
                            lost =
                                1; /* endpoint buffer full: survey is a prefix */
                    }
                }
                off += h.size;
            }
            free(buf);
        }
    }
    __sync_synchronize(); /* publish the reads before advancing data_tail (smp_mb) */
    mp->data_tail = head; /* consume */
    munmap(base_map, base_sz);
    close((int)fd);
    if (nips != NULL)
        *nips = n;
    /* Honest: loss (ring/throttle/near-full/buffer-full) OR an empty survey (nothing
     * sampled — too short a window, or the whole run dropped) is a prefix, not complete. */
    if (truncated != NULL)
        *truncated = (lost || n == 0) ? 1 : 0;
    return ASMTEST_HW_OK;
}

/* BEGIN/END split of the survey above — lets a caller ARM the sampler, run a block INLINE,
 * and DRAIN later (the .NET `using (new AsmTrace(HwBackend.AmdLbr)) { block }` shape, where
 * the ctor calls _begin and Dispose calls _end, no run_fn callback). The ctx handle carries
 * the perf fd + ring; _end DISABLEs, drains the same way as the monolith, and frees it. */
struct asmtest_amd_sample_ctx {
    int fd;
    void *base_map;
    size_t base_sz;
};

int asmtest_hwtrace_sample_begin_amd(int period, void **ctx_out) {
    if (ctx_out == NULL)
        return ASMTEST_HW_EINVAL;
    *ctx_out = NULL;
    if (period < 2)
        period = 2;
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = PERF_TYPE_HARDWARE;
    a.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
    a.sample_period = (unsigned)period;
    a.sample_type = PERF_SAMPLE_BRANCH_STACK;
    a.branch_sample_type = PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_ANY;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    a.disabled = 1;
    long fd = perf_open(&a, 0, -1, -1, 0); /* pid=0: the calling thread */
    if (fd < 0)
        return ASMTEST_HW_EUNAVAIL;
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        pg = 4096;
    size_t base_sz = (size_t)pg + round_pages(0, 256 * 1024);
    void *base_map =
        mmap(NULL, base_sz, PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
    if (base_map == MAP_FAILED) {
        close((int)fd);
        return ASMTEST_HW_EUNAVAIL;
    }
    struct asmtest_amd_sample_ctx *c =
        (struct asmtest_amd_sample_ctx *)malloc(sizeof *c);
    if (c == NULL) {
        munmap(base_map, base_sz);
        close((int)fd);
        return ASMTEST_HW_EUNAVAIL;
    }
    c->fd = (int)fd;
    c->base_map = base_map;
    c->base_sz = base_sz;
    ioctl((int)fd, PERF_EVENT_IOC_RESET, 0);
    if (ioctl((int)fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        free(c);
        munmap(base_map, base_sz);
        close((int)fd);
        return ASMTEST_HW_EUNAVAIL;
    }
    *ctx_out = c;
    return ASMTEST_HW_OK;
}

int asmtest_hwtrace_sample_end_amd(void *ctxp, uint64_t *ips, size_t cap,
                                   size_t *nips, int *truncated) {
    if (ctxp == NULL)
        return ASMTEST_HW_EINVAL;
    struct asmtest_amd_sample_ctx *c = (struct asmtest_amd_sample_ctx *)ctxp;
    if (nips != NULL)
        *nips = 0;
    if (truncated != NULL)
        *truncated = 0;
    ioctl(c->fd, PERF_EVENT_IOC_DISABLE, 0);
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        pg = 4096;
    struct perf_event_mmap_page *mp =
        (struct perf_event_mmap_page *)c->base_map;
    uint8_t *data = (uint8_t *)c->base_map + (size_t)pg;
    size_t dsz = c->base_sz - (size_t)pg;
    uint64_t head = mp->data_head;
    __sync_synchronize();
    uint64_t tail = mp->data_tail;
    size_t span = (size_t)(head - tail);
    size_t n = 0;
    int lost = 0;
    {
        int depth = asmtest_amd_lbr_depth();
        size_t max_sample = sizeof(struct perf_event_header) +
                            sizeof(uint64_t) +
                            (size_t)depth * sizeof(struct perf_branch_entry);
        if (dsz > 0 && span + max_sample > dsz)
            lost = 1;
    }
    if (ips != NULL && cap > 0 && span > 0 && span <= dsz) {
        uint8_t *buf = (uint8_t *)malloc(span);
        if (buf != NULL) {
            for (size_t i = 0; i < span; i++)
                buf[i] = data[(tail + i) % dsz];
            for (size_t off = 0;
                 off + sizeof(struct perf_event_header) <= span;) {
                /* memcpy the header/nr out of the byte ring (strict-aliasing clean);
                 * `e` stays a typed pointer into buf, read transiently in-loop. */
                struct perf_event_header h;
                memcpy(&h, buf + off, sizeof h);
                if (h.size == 0 || off + h.size > span)
                    break;
                if (h.type == PERF_RECORD_LOST ||
                    h.type == PERF_RECORD_THROTTLE) {
                    lost = 1;
                } else if (h.type == PERF_RECORD_SAMPLE) {
                    uint8_t *body = buf + off + sizeof h;
                    uint64_t nr;
                    memcpy(&nr, body, sizeof nr);
                    if (nr > 0 && nr <= 64 &&
                        sizeof h + sizeof(uint64_t) +
                                nr * sizeof(struct perf_branch_entry) <=
                            h.size) {
                        struct perf_branch_entry *e =
                            (struct perf_branch_entry *)(body +
                                                         sizeof(uint64_t));
                        for (uint64_t i = 0; i < nr && n < cap; i++) {
                            if (e[i].abort)
                                continue;
                            ips[n++] = e[i].to;
                        }
                        if (n >= cap)
                            lost = 1;
                    }
                }
                off += h.size;
            }
            free(buf);
        }
    }
    __sync_synchronize();
    mp->data_tail = head;
    munmap(c->base_map, c->base_sz);
    close(c->fd);
    free(c);
    if (nips != NULL)
        *nips = n;
    if (truncated != NULL)
        *truncated = (lost || n == 0) ? 1 : 0;
    return ASMTEST_HW_OK;
}
#else
int asmtest_hwtrace_sample_window_amd(void (*run_fn)(void *), void *arg,
                                      int period, uint64_t *ips, size_t cap,
                                      size_t *nips, int *truncated) {
    (void)run_fn;
    (void)arg;
    (void)period;
    (void)ips;
    (void)cap;
    (void)nips;
    (void)truncated;
    return ASMTEST_HW_ENOSYS;
}
int asmtest_hwtrace_sample_begin_amd(int period, void **ctx_out) {
    (void)period;
    (void)ctx_out;
    return ASMTEST_HW_ENOSYS;
}
int asmtest_hwtrace_sample_end_amd(void *ctx, uint64_t *ips, size_t cap,
                                   size_t *nips, int *truncated) {
    (void)ctx;
    (void)ips;
    (void)cap;
    (void)nips;
    (void)truncated;
    return ASMTEST_HW_ENOSYS;
}
#endif /* __linux__ && __x86_64__ */

/* ------------------------------------------------------------------ */
/* Capture lifecycle (Intel PT via perf AUX; CoreSight is analogous)   */
/* ------------------------------------------------------------------ */

int asmtest_hwtrace_try_begin(const char *name) {
#if defined(HWTRACE_LIFECYCLE)
    if (!g_inited)
        return ASMTEST_HW_ESTATE;
#if defined(__x86_64__)
    /* §1: single-step is per-thread + nesting-safe via the ss range stack, so the
     * process-global g_active/g_fd busy guard does NOT apply here — a second
     * same-thread begin composes (returns OK), and the refusal moves to a full
     * per-thread range stack (ASMTEST_HW_EFULL). */
    if (g_opts.backend == ASMTEST_HWTRACE_SINGLESTEP) {
        hw_region_t *r = find_region(name);
        if (r == NULL)
            return ASMTEST_HW_EINVAL;
        int tid = hw_current_tid();
        r->arm_tid = tid; /* per-region cross-thread backstop */
        g_arm_tid = tid;  /* best-effort most-recent-arming-tid accessor */
        return asmtest_ss_begin(r->base, r->len,
                                r->trace); /* OK / EFULL / EINVAL */
    }
#endif
#if defined(__linux__)
    /* PT / AMD / CoreSight: still the process-global single slot (per-thread AUX
     * migration is forward-look, needs PT hardware to validate). */
    if (g_fd >= 0 || g_active != NULL)
        return ASMTEST_HW_ESTATE;
    hw_region_t *r = find_region(name);
    if (r == NULL)
        return ASMTEST_HW_EINVAL; /* unregistered name (constraint #1: a shim must
                                   * register-then-begin under the same name) */
    /* §0.2: record the arming thread so end() can flag a cross-thread close. */
    g_arm_tid = (int)syscall(SYS_gettid);
#if defined(__x86_64__)
    if (g_opts.backend == ASMTEST_HWTRACE_AMD_LBR) {
        if (hwtrace_begin_amd(r) != 0) {
            g_arm_tid = -1;
            return ASMTEST_HW_EUNAVAIL; /* perf_open / mmap failed */
        }
        return ASMTEST_HW_OK;
    }
#endif
    int type = pmu_type(g_opts.backend);
    if (type < 0) {
        g_arm_tid = -1;
        return ASMTEST_HW_EUNAVAIL;
    }
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.size = sizeof attr;
    attr.type = (uint32_t)type;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.disabled = 1;
    long fd = perf_open(&attr, 0, -1, -1, 0);
    if (fd < 0) {
        g_arm_tid = -1;
        return ASMTEST_HW_EUNAVAIL;
    }
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
        g_arm_tid = -1;
        return ASMTEST_HW_EUNAVAIL;
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
        g_arm_tid = -1;
        return ASMTEST_HW_EUNAVAIL;
    }
    g_active = r;
    ioctl(g_fd, PERF_EVENT_IOC_RESET, 0);
    if (ioctl(g_fd, PERF_EVENT_IOC_ENABLE, 0) != 0) {
        munmap(g_aux_map, g_aux_sz);
        g_aux_map = NULL;
        munmap(g_base_map, g_base_sz);
        g_base_map = NULL;
        g_active = NULL;
        close(g_fd);
        g_fd = -1;
        g_arm_tid = -1;
        return ASMTEST_HW_EUNAVAIL;
    }
    return ASMTEST_HW_OK;
#else /* HWTRACE_LIFECYCLE && !__linux__ (macOS): single-step is the only backend,
        * handled above; a non-single-step backend never passes init()'s available() */
    (void)name;
    return ASMTEST_HW_EUNAVAIL;
#endif
#else
    (void)name;
    return ASMTEST_HW_ENOSYS;
#endif
}

/* §0.1: the shipped void ABI — a thin wrapper that discards try_begin's status, so
 * the ten already-shipped shims are behaviourally unchanged (a busy slot / bad name
 * still silently no-ops). New scope shims call try_begin for the signal. */
void asmtest_hwtrace_begin(const char *name) {
    (void)asmtest_hwtrace_try_begin(name);
}

/* §0.2: the OS thread id that armed the active capture (-1 when idle). */
int asmtest_hwtrace_arm_tid(void) {
#if defined(HWTRACE_LIFECYCLE)
    return g_arm_tid;
#else
    return -1;
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
#if defined(HWTRACE_LIFECYCLE)
#if defined(__x86_64__)
    /* §1: single-step closes the CALLING thread's top frame (per-thread range
     * stack). If the closing thread differs from the region's arming thread, the
     * traced thread hopped — flag truncated (the arming thread's TLS frame is
     * invisible here; this is the misuse backstop, errs false-truncated over
     * false-complete). */
    if (g_opts.backend == ASMTEST_HWTRACE_SINGLESTEP) {
        if (name != NULL) {
            hw_region_t *r = find_region(name);
            if (r != NULL && r->arm_tid != 0 &&
                r->arm_tid != hw_current_tid() && r->trace != NULL)
                r->trace->truncated = true;
        }
        asmtest_ss_end(); /* disarms TF + restores SIGTRAP; trace already filled */
        g_arm_tid = -1; /* §0.2: capture closed — accessor must read idle (-1),
                           * matching the PT/AMD/whole-window end paths below */
        return;
    }
#endif
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
    /* §0.2: a close on a different OS thread than begin means the traced thread
     * hopped (await / Task.Run / go func() / thread-pool continuation) and the
     * capture followed the arming thread, not the work — flag the partial trace
     * truncated rather than presenting it as complete. This is the C half of the
     * shims' thread-scope backstop; it errs toward false-truncated over
     * false-complete. */
    if (g_arm_tid != -1 && hw_current_tid() != g_arm_tid &&
        g_active->trace != NULL)
        g_active->trace->truncated = true;
#if defined(__x86_64__)
    if (g_opts.backend == ASMTEST_HWTRACE_AMD_LBR) {
        hwtrace_end_amd();
        g_arm_tid = -1;
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
    g_arm_tid = -1;
#else /* HWTRACE_LIFECYCLE && !__linux__ (macOS): single-step returned above */
    (void)name;
#endif
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
#elif defined(HWTRACE_LIFECYCLE)
    /* macOS single-step: no perf fd / active-slot to test; end(NULL) disarms this
     * thread's TF if a scope was left open (a no-op when the range stack is empty). */
    if (g_inited)
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
/* The PROT_NONE→RW→RX W^X dance is plain POSIX mmap/mprotect, identical on x86-64
 * Linux and x86-64 macOS — the two hosts the single-step tier runs on. (Apple Silicon
 * would additionally need MAP_JIT + pthread_jit_write_protect_np, but the single-step
 * front-end is x86-64-Darwin only and self-skips on AArch64, so this never runs there.) */
#if defined(__linux__) || defined(__APPLE__)
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
#if defined(__linux__) || defined(__APPLE__)
    if (base != NULL && len != 0)
        munmap(base, len);
#else
    (void)base;
    (void)len;
#endif
}

/* ------------------------------------------------------------------ */
/* §0.3 — shared render-on-close path                                  */
/*                                                                     */
/* Turn a closed region's recorded asmtest_trace_t insn offsets into    */
/* Capstone-disassembled text. The offsets already sit on the trace     */
/* (every backend's end() fills them) and Capstone already renders a     */
/* single instruction (asmtest_disas) — this is the one glue that was    */
/* unwired in all ten bindings, so it belongs once in the C core.       */
/* ------------------------------------------------------------------ */

/* Append `n` bytes of `src` to buf at cursor `total`, keeping NUL termination and
 * never writing past buflen. Returns nothing; the caller advances `total` by the
 * full formatted length (snprintf semantics) regardless of what fit. */
static void render_emit(char *buf, size_t buflen, size_t total, const char *src,
                        size_t n) {
    if (buf == NULL || total >= buflen)
        return;
    size_t room = buflen - total - 1; /* reserve one byte for the NUL */
    size_t cpy = (n <= room) ? n : room;
    memcpy(buf + total, src, cpy);
    buf[total + cpy] = '\0';
}

/* Render trace `t`'s recorded insn offsets against the live bytes at [base, len).
 * snprintf semantics; returns the would-be length or a negative ASMTEST_HW_*. Shared
 * by the name-keyed render (§0.3) and the handle-keyed render_scope (§1). */
static int render_trace_into(const asmtest_trace_t *t, const void *base,
                             size_t len, char *buf, size_t buflen) {
    if (t == NULL || base == NULL)
        return ASMTEST_HW_EINVAL;
    if (!asmtest_disas_available())
        return ASMTEST_HW_ENOSYS; /* no Capstone: cannot render text */
        /* The traced region is host-native machine code, so decode it for the host
     * ISA (x86-64 for PT/AMD/single-step; AArch64 for CoreSight). */
#if defined(__aarch64__)
    const asmtest_arch_t host_arch = ASMTEST_ARCH_ARM64;
#else
    const asmtest_arch_t host_arch = ASMTEST_ARCH_X86_64;
#endif
    const uint64_t base_addr = (uint64_t)(uintptr_t)base;
    if (buf != NULL && buflen > 0)
        buf[0] = '\0';
    size_t total = 0;
    char line[192];
    for (size_t i = 0; i < t->insns_len; i++) {
        uint64_t off = t->insns[i];
        char txt[128];
        asmtest_disas(host_arch, (const uint8_t *)base, len, base_addr, off,
                      txt, sizeof txt);
        int n =
            snprintf(line, sizeof line, "%8llx:\t%s\n", (unsigned long long)off,
                     txt[0] != '\0' ? txt : "(undecodable)");
        if (n < 0)
            continue;
        render_emit(buf, buflen, total, line, (size_t)n);
        total += (size_t)n;
    }
    /* Never present a capped / truncated stream as complete — label the prefix. */
    if (t->truncated || t->insns_total > t->insns_len) {
        int n =
            snprintf(line, sizeof line,
                     "; trace truncated — %llu of %llu instructions shown\n",
                     (unsigned long long)t->insns_len,
                     (unsigned long long)t->insns_total);
        if (n > 0) {
            render_emit(buf, buflen, total, line, (size_t)n);
            total += (size_t)n;
        }
    }
    return (int)total;
}

int asmtest_hwtrace_render(const char *name, char *buf, size_t buflen) {
    if (name == NULL)
        return ASMTEST_HW_EINVAL;
    hw_region_t *r = find_region(name);
    if (r == NULL || r->trace == NULL)
        return ASMTEST_HW_EINVAL; /* name miss / no trace buffer to render */
    return render_trace_into(r->trace, r->base, r->len, buf, buflen);
}

/* ------------------------------------------------------------------ */
/* §1 — handle-keyed scope API (per-thread, disambiguates concurrent    */
/* same-site scopes). Additive: try_begin/end/render stay name-keyed for */
/* the single-owner path the bindings slice ships against.              */
/* ------------------------------------------------------------------ */

int asmtest_hwtrace_begin_scope(const char *name,
                                asmtest_hwtrace_scope_t *out) {
    if (out != NULL) {
        out->idx = 0xffffffffu;
        out->gen = 0;
    }
#if defined(HWTRACE_LIFECYCLE)
    if (!g_inited)
        return ASMTEST_HW_ESTATE;
#if defined(__x86_64__)
    if (g_opts.backend == ASMTEST_HWTRACE_SINGLESTEP) {
        hw_region_t *r = find_region(name);
        if (r == NULL)
            return ASMTEST_HW_EINVAL;
        int tid = hw_current_tid();
        r->arm_tid = tid;
        g_arm_tid = tid;
        uint32_t idx = 0, gen = 0;
        int rc = asmtest_ss_begin_ex(r->base, r->len, r->trace, &idx, &gen);
        if (rc != ASMTEST_HW_OK)
            return rc;
        if (out != NULL) {
            out->idx = idx;
            out->gen = gen;
        }
        return ASMTEST_HW_OK;
    }
#endif
    /* PT/AMD/CS: the per-thread handle path is forward-look (needs PT hardware); a
     * shim on those backends drives the name-keyed try_begin + render instead. */
    return asmtest_hwtrace_try_begin(name);
#else
    (void)name;
    return ASMTEST_HW_ENOSYS;
#endif
}

/* B (lazy-arm) — managed-singlestep-lazy-arm-plan §B1. Register-region + arm + call
 * fn(args…) + disarm as ONE native step, so the armed window holds only fn's body (the
 * region filter drops the native dispatcher and any reverse-P/Invoke thunk). This is
 * the managed-safe replacement for "begin_scope; DynamicInvoke; end": no caller or
 * runtime code is stepped, so an in-window pthread_create (which blocks SIGTRAP) cannot
 * force-kill the process. Only the single-step backend has that crash surface; PT / AMD
 * / CoreSight observe out-of-band and return ASMTEST_HW_EUNAVAIL here (the caller uses
 * begin/end there). fn takes nargs (0-6) integer/pointer args; *result_out (may be
 * NULL) gets its return value; *out is the scope handle for asmtest_hwtrace_render_scope. */
int asmtest_hwtrace_call_scoped(const char *name, void *fn, const long *args,
                                int nargs, long *result_out,
                                asmtest_hwtrace_scope_t *out) {
    if (out != NULL) {
        out->idx = 0xffffffffu;
        out->gen = 0;
    }
#if defined(HWTRACE_LIFECYCLE)
    if (!g_inited)
        return ASMTEST_HW_ESTATE;
#if defined(__x86_64__)
    if (g_opts.backend == ASMTEST_HWTRACE_SINGLESTEP) {
        hw_region_t *r = find_region(name);
        if (r == NULL)
            return ASMTEST_HW_EINVAL;
        int tid = hw_current_tid();
        r->arm_tid = tid;
        g_arm_tid = tid;
        uint32_t idx = 0, gen = 0;
        int rc = asmtest_ss_call_scoped(r->base, r->len, r->trace, fn, args,
                                        nargs, result_out, &idx, &gen);
        if (rc != ASMTEST_HW_OK)
            return rc;
        if (out != NULL) {
            out->idx = idx;
            out->gen = gen;
        }
        return ASMTEST_HW_OK;
    }
#endif
    /* Out-of-band backends have no in-process TF crash surface, so the lazy-arm
     * restructure is single-step-only; signal that and let the caller use begin/end.
     * `name` is only read in the __x86_64__ block above, so cast it here too — on
     * aarch64-Linux (LIFECYCLE defined, not x86_64) it would otherwise be unused. */
    (void)name;
    (void)fn;
    (void)args;
    (void)nargs;
    (void)result_out;
    return ASMTEST_HW_EUNAVAIL;
#else
    (void)name;
    (void)fn;
    (void)args;
    (void)nargs;
    (void)result_out;
    return ASMTEST_HW_ENOSYS;
#endif
}

/* FP sibling of asmtest_hwtrace_call_scoped: the (double…)->double lazy-arm scope for
 * managed methods whose signature is homogeneous double (0-8 args, double return),
 * which the integer shim family cannot dispatch. Same routing, guarantees, and return
 * codes as the integer form; *result_out (may be NULL) receives fn's double return. */
int asmtest_hwtrace_call_scoped_fp(const char *name, void *fn,
                                   const double *args, int nargs,
                                   double *result_out,
                                   asmtest_hwtrace_scope_t *out) {
    if (out != NULL) {
        out->idx = 0xffffffffu;
        out->gen = 0;
    }
#if defined(HWTRACE_LIFECYCLE)
    if (!g_inited)
        return ASMTEST_HW_ESTATE;
#if defined(__x86_64__)
    if (g_opts.backend == ASMTEST_HWTRACE_SINGLESTEP) {
        hw_region_t *r = find_region(name);
        if (r == NULL)
            return ASMTEST_HW_EINVAL;
        int tid = hw_current_tid();
        r->arm_tid = tid;
        g_arm_tid = tid;
        uint32_t idx = 0, gen = 0;
        int rc = asmtest_ss_call_scoped_fp(r->base, r->len, r->trace, fn, args,
                                           nargs, result_out, &idx, &gen);
        if (rc != ASMTEST_HW_OK)
            return rc;
        if (out != NULL) {
            out->idx = idx;
            out->gen = gen;
        }
        return ASMTEST_HW_OK;
    }
#endif
    (void)
        name; /* only read in the __x86_64__ block above (aarch64-Linux: unused) */
    (void)fn;
    (void)args;
    (void)nargs;
    (void)result_out;
    return ASMTEST_HW_EUNAVAIL;
#else
    (void)name;
    (void)fn;
    (void)args;
    (void)nargs;
    (void)result_out;
    return ASMTEST_HW_ENOSYS;
#endif
}

/* Registry-free lazy-arm call. Same arm→call→disarm managed-safe guarantee as
 * asmtest_hwtrace_call_scoped, but the code region is given DIRECTLY as [base,len) with a
 * caller-owned `trace`, so NO named region is registered and NO fixed-table slot
 * (MAX_REGIONS) is consumed. For high-churn callers that capture many distinct one-shot
 * bodies — notably the §D0.4 async-hop stitching producer, which captures one fresh body
 * per hop and would otherwise exhaust the 32-slot registry over process lifetime (the
 * registry has no release path and assumes call-site-constant names). *out (may be NULL)
 * is the scope handle for asmtest_hwtrace_render_scope, valid on the CAPTURING thread and
 * only until that thread pushes another scope. Same status codes as the named form;
 * ASMTEST_HW_EINVAL on a NULL base / zero len / NULL trace. */
int asmtest_hwtrace_call_scoped_ex(void *base, size_t len,
                                   asmtest_trace_t *trace, void *fn,
                                   const long *args, int nargs,
                                   long *result_out,
                                   asmtest_hwtrace_scope_t *out) {
    if (out != NULL) {
        out->idx = 0xffffffffu;
        out->gen = 0;
    }
#if defined(HWTRACE_LIFECYCLE)
    if (!g_inited)
        return ASMTEST_HW_ESTATE;
#if defined(__x86_64__)
    if (g_opts.backend == ASMTEST_HWTRACE_SINGLESTEP) {
        if (base == NULL || len == 0 || trace == NULL)
            return ASMTEST_HW_EINVAL;
        uint32_t idx = 0, gen = 0;
        int rc = asmtest_ss_call_scoped(base, len, trace, fn, args, nargs,
                                        result_out, &idx, &gen);
        if (rc != ASMTEST_HW_OK)
            return rc;
        if (out != NULL) {
            out->idx = idx;
            out->gen = gen;
        }
        return ASMTEST_HW_OK;
    }
#endif
    (void)base;
    (void)len;
    (void)trace;
    (void)fn;
    (void)args;
    (void)nargs;
    (void)result_out;
    return ASMTEST_HW_EUNAVAIL;
#else
    (void)base;
    (void)len;
    (void)trace;
    (void)fn;
    (void)args;
    (void)nargs;
    (void)result_out;
    return ASMTEST_HW_ENOSYS;
#endif
}

int asmtest_hwtrace_render_scope(asmtest_hwtrace_scope_t handle, char *buf,
                                 size_t buflen) {
#if defined(HWTRACE_HAVE_SINGLESTEP)
    const void *base = NULL;
    size_t len = 0;
    asmtest_trace_t *trace = NULL;
    if (asmtest_ss_frame_lookup(handle.idx, handle.gen, &base, &len, &trace))
        return render_trace_into(trace, base, len, buf, buflen);
#endif
    (void)handle;
    (void)buf;
    (void)buflen;
    return ASMTEST_HW_EINVAL; /* stale/unknown handle (or non-single-step backend) */
}

int asmtest_hwtrace_render_versioned(asmtest_codeimage_t *img, uint64_t when,
                                     const asmtest_trace_t *trace, char *buf,
                                     size_t buflen) {
    if (img == NULL || trace == NULL)
        return ASMTEST_HW_EINVAL;
    if (!asmtest_disas_available())
        return ASMTEST_HW_ENOSYS;
#if defined(__aarch64__)
    const asmtest_arch_t host_arch = ASMTEST_ARCH_ARM64;
#else
    const asmtest_arch_t host_arch = ASMTEST_ARCH_X86_64;
#endif
    /* The whole-window / managed decode records ABSOLUTE addresses; render each by
     * disassembling the version-live bytes at that address as of `when` (the
     * temporal-bytes rule — the bytes tier/move, so the version-blind render is
     * wrong here). */
    if (buf != NULL && buflen > 0)
        buf[0] = '\0';
    size_t total = 0;
    char line[192];
    for (size_t i = 0; i < trace->insns_len; i++) {
        uint64_t addr = trace->insns[i];
        const uint8_t *bytes = NULL;
        size_t avail = 0;
        char txt[128];
        txt[0] = '\0';
        if (asmtest_codeimage_bytes_at(img, (const void *)(uintptr_t)addr, when,
                                       &bytes, &avail) == ASMTEST_CI_OK &&
            bytes != NULL && avail > 0)
            asmtest_disas(host_arch, bytes, avail, addr, 0, txt, sizeof txt);
        int n = snprintf(line, sizeof line, "%12llx:\t%s\n",
                         (unsigned long long)addr,
                         txt[0] != '\0' ? txt : "(no bytes @version)");
        if (n < 0)
            continue;
        render_emit(buf, buflen, total, line, (size_t)n);
        total += (size_t)n;
    }
    if (trace->truncated || trace->insns_total > trace->insns_len) {
        int n =
            snprintf(line, sizeof line,
                     "; trace truncated — %llu of %llu instructions shown\n",
                     (unsigned long long)trace->insns_len,
                     (unsigned long long)trace->insns_total);
        if (n > 0) {
            render_emit(buf, buflen, total, line, (size_t)n);
            total += (size_t)n;
        }
    }
    return (int)total;
}

/* ------------------------------------------------------------------ */
/* §Z0/§Z1 — the region-free (empty-ctor) whole-window scope surface    */
/*                                                                     */
/* The aspirational `using (new AsmTrace())` form: no NativeCode, no    */
/* [base,len). begin_window arms the calling thread with NO registered  */
/* region; the single-step handler records the absolute RIP of every    */
/* instruction the thread runs in the window (the WEAK tier — native    */
/* leaves only; whole-window PT/LBR is the forward-look STRONG tier and  */
/* self-skips here). end_window closes the handle's frame and, on a      */
/* cross-thread close (the handle does not resolve on the closing        */
/* thread), flags `truncated` — the §Z4 thread-scope honesty default.   */
/* render_window decodes the recorded ABSOLUTE addresses from LIVE self  */
/* memory (valid for non-moving native code); moving/managed bytes use   */
/* asmtest_hwtrace_render_versioned against a code-image instead (§Z3).  */
/* ------------------------------------------------------------------ */

int asmtest_hwtrace_begin_window(asmtest_trace_t *trace,
                                 asmtest_hwtrace_scope_t *out) {
    if (out != NULL) {
        out->idx = 0xffffffffu;
        out->gen = 0;
    }
    if (trace == NULL)
        return ASMTEST_HW_EINVAL;
#if defined(__linux__)
    if (!g_inited)
        return ASMTEST_HW_ESTATE;
#if defined(__x86_64__)
    if (g_opts.backend == ASMTEST_HWTRACE_SINGLESTEP) {
        uint32_t idx = 0, gen = 0;
        g_arm_tid =
            (int)syscall(SYS_gettid); /* §Z4: best-effort arming-tid accessor */
        int rc = asmtest_ss_begin_window(trace, &idx, &gen);
        if (rc != ASMTEST_HW_OK) {
            g_arm_tid = -1;
            return rc;
        }
        if (out != NULL) {
            out->idx = idx;
            out->gen = gen;
        }
        return ASMTEST_HW_OK;
    }
#endif
    /* PT / AMD LBR / CoreSight whole-window capture is the forward-look STRONG tier
     * (needs bare-metal Intel PT / Zen 3+ to validate live); self-skip cleanly. */
    return ASMTEST_HW_EUNAVAIL;
#else
    (void)trace;
    return ASMTEST_HW_ENOSYS;
#endif
}

int asmtest_hwtrace_end_window(asmtest_hwtrace_scope_t handle,
                               asmtest_trace_t *trace) {
#if defined(__linux__) && defined(__x86_64__)
    const void *base = NULL;
    size_t len = 0;
    asmtest_trace_t *ftrace = NULL;
    /* §Z4: the region-free frame lives in the ARMING thread's TLS. If the handle
     * resolves here, this is the arming thread — close normally. If it does NOT
     * (the traced work hopped threads and Dispose ran elsewhere), the frame is
     * invisible and uncloseable here: flag the trace truncated rather than present
     * a thread-window as a complete logical-operation trace. Errs false-truncated. */
    if (asmtest_ss_frame_lookup(handle.idx, handle.gen, &base, &len, &ftrace)) {
        asmtest_ss_end(); /* closes the calling thread's top frame + normalizes */
        g_arm_tid = -1;
        return ASMTEST_HW_OK;
    }
    if (trace != NULL)
        trace->truncated = true;
    return ASMTEST_HW_OK;
#else
    (void)handle;
    if (trace != NULL)
        trace->truncated = true;
    return ASMTEST_HW_ENOSYS;
#endif
}

int asmtest_hwtrace_render_window(asmtest_hwtrace_scope_t handle, char *buf,
                                  size_t buflen) {
#if defined(__linux__) && defined(__x86_64__)
    const void *base = NULL;
    size_t len = 0;
    asmtest_trace_t *trace = NULL;
    if (!asmtest_ss_frame_lookup(handle.idx, handle.gen, &base, &len, &trace) ||
        trace == NULL)
        return ASMTEST_HW_EINVAL; /* stale/unknown handle (or non-single-step) */
    if (!asmtest_disas_available())
        return ASMTEST_HW_ENOSYS; /* no Capstone: cannot render text */
    /* WEAK-tier render: insns[] hold ABSOLUTE addresses; disassemble each from LIVE
     * self memory (the code we just stepped through is mapped + non-moving for a
     * native leaf). Moving/managed bytes route to _render_versioned (§Z3) instead. */
    if (buf != NULL && buflen > 0)
        buf[0] = '\0';
    size_t total = 0;
    char lineb[192];
    for (size_t i = 0; i < trace->insns_len; i++) {
        uint64_t addr = trace->insns[i];
        char txt[128];
        txt[0] = '\0';
        asmtest_disas(ASMTEST_ARCH_X86_64, (const uint8_t *)(uintptr_t)addr, 16,
                      addr, 0, txt, sizeof txt);
        int n = snprintf(lineb, sizeof lineb, "%12llx:\t%s\n",
                         (unsigned long long)addr,
                         txt[0] != '\0' ? txt : "(undecodable)");
        if (n < 0)
            continue;
        render_emit(buf, buflen, total, lineb, (size_t)n);
        total += (size_t)n;
    }
    if (trace->truncated || trace->insns_total > trace->insns_len) {
        int n =
            snprintf(lineb, sizeof lineb,
                     "; trace truncated — %llu of %llu instructions shown\n",
                     (unsigned long long)trace->insns_len,
                     (unsigned long long)trace->insns_total);
        if (n > 0) {
            render_emit(buf, buflen, total, lineb, (size_t)n);
            total += (size_t)n;
        }
    }
    return (int)total;
#else
    (void)handle;
    (void)buf;
    (void)buflen;
    return ASMTEST_HW_ENOSYS;
#endif
}

/* ------------------------------------------------------------------ */
/* §D4 — async-hop stitching merge core                                */
/*                                                                     */
/* A pure ordered concatenation of already-decoded per-thread slices    */
/* into one logical-operation stream. No decode happens here (each      */
/* slice was decoded against its own code-image version at disable      */
/* time), which is exactly what keeps test_stitch_slices host-testable. */
/* ------------------------------------------------------------------ */
#define STITCH_MAX_SLICES 4096

int asmtest_hwtrace_stitch(const asmtest_hwtrace_slice_t *slices, size_t n,
                           asmtest_trace_t *out,
                           asmtest_hwtrace_slice_bound_t *bounds,
                           size_t *nbounds) {
    if (slices == NULL || out == NULL)
        return ASMTEST_HW_EINVAL;
    if (n > STITCH_MAX_SLICES)
        return ASMTEST_HW_EINVAL;
    if (n == 0) {
        if (nbounds != NULL)
            *nbounds = 0;
        return ASMTEST_HW_OK;
    }
    /* Order slice indices by `seq` (stable selection sort; n is tiny). */
    size_t *order = (size_t *)malloc(n * sizeof *order);
    if (order == NULL)
        return ASMTEST_HW_EUNAVAIL;
    for (size_t i = 0; i < n; i++)
        order[i] = i;
    for (size_t i = 0; i + 1 < n; i++) {
        size_t min = i;
        for (size_t j = i + 1; j < n; j++)
            if (slices[order[j]].seq < slices[order[min]].seq)
                min = j;
        if (min != i) {
            size_t tmp = order[i];
            order[i] = order[min];
            order[min] = tmp;
        }
    }
    /* Concatenate each slice's insns (and blocks) in seq order, recording where in
     * out->insns each slice begins in the companion bounds array. */
    for (size_t k = 0; k < n; k++) {
        const asmtest_hwtrace_slice_t *s = &slices[order[k]];
        if (bounds != NULL) {
            bounds[k].insn_off = out->insns_len;
            bounds[k].scope_id = s->scope_id;
            bounds[k].seq = s->seq;
            bounds[k].tid = s->tid;
            bounds[k].version = s->version;
        }
        const asmtest_trace_t *st = &s->trace;
        for (size_t i = 0; i < st->insns_len; i++)
            trace_append_insn(out, st->insns[i]);
        for (size_t i = 0; i < st->blocks_len; i++)
            trace_append_block(out, st->blocks[i]);
        if (st->truncated)
            out->truncated = true; /* a truncated slice truncates the whole */
    }
    if (nbounds != NULL)
        *nbounds = n;
    free(order);
    return ASMTEST_HW_OK;
}

/* §D0.4 live-producer bridge for asmtest_hwtrace_stitch. A binding captures one
 * lazy-arm slice per async hop (each a populated asmtest_trace_t behind a handle) and
 * hands the HANDLES here with their per-slice (scope_id, seq, tid, version); this
 * assembles the asmtest_hwtrace_slice_t array (a shallow struct copy of each trace —
 * the insns/blocks heap arrays stay owned by the handles, which must outlive this
 * call) and stitches into `out` in seq order. Exists because the slice struct embeds
 * an asmtest_trace_t with heap pointers, which a language binding cannot marshal by
 * value; an array of opaque handles + parallel scalar arrays is blittable. The scalar
 * arrays may each be NULL (defaults: seq=index, others 0). Same bounds/nbounds and
 * status codes as asmtest_hwtrace_stitch; ASMTEST_HW_EINVAL on a NULL traces/out or a
 * NULL entry, ASMTEST_HW_EUNAVAIL on allocation failure. */
int asmtest_hwtrace_stitch_handles(const asmtest_trace_t *const *traces,
                                   const uint64_t *scope_ids,
                                   const uint32_t *seqs, const int *tids,
                                   const uint64_t *versions, size_t n,
                                   asmtest_trace_t *out,
                                   asmtest_hwtrace_slice_bound_t *bounds,
                                   size_t *nbounds) {
    if (traces == NULL || out == NULL)
        return ASMTEST_HW_EINVAL;
    if (n == 0) {
        if (nbounds != NULL)
            *nbounds = 0;
        return ASMTEST_HW_OK;
    }
    asmtest_hwtrace_slice_t *sl =
        (asmtest_hwtrace_slice_t *)calloc(n, sizeof *sl);
    if (sl == NULL)
        return ASMTEST_HW_EUNAVAIL;
    for (size_t i = 0; i < n; i++) {
        if (traces[i] == NULL) {
            free(sl);
            return ASMTEST_HW_EINVAL;
        }
        sl[i].scope_id = scope_ids != NULL ? scope_ids[i] : 0;
        sl[i].seq = seqs != NULL ? seqs[i] : (uint32_t)i;
        sl[i].tid = tids != NULL ? tids[i] : 0;
        sl[i].version = versions != NULL ? versions[i] : 0;
        sl[i].trace =
            *traces[i]; /* shallow copy: heap arrays owned by the handle */
    }
    int rc = asmtest_hwtrace_stitch(sl, n, out, bounds, nbounds);
    free(sl);
    return rc;
}

/* ------------------------------------------------------------------ */
/* §3.1(c) — whole-window noise attribution: reverse resolver + bucketer */
/* ------------------------------------------------------------------ */

int asmtest_hwtrace_region_name(int pid, uint64_t addr, char *name,
                                size_t namelen, uint64_t *start,
                                uint64_t *end) {
    if (name != NULL && namelen > 0)
        name[0] = '\0';
#if defined(__linux__)
    char path[64];
    if (pid == 0)
        snprintf(path, sizeof path, "/proc/self/maps");
    else
        snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;
    char line[512];
    int hit = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        unsigned long lo = 0, hi = 0;
        int np = 0;
        /* "lo-hi perms offset dev inode  pathname" — %n records the pathname start. */
        if (sscanf(line, "%lx-%lx %*s %*s %*s %*s %n", &lo, &hi, &np) >= 2 &&
            addr >= lo && addr < hi) {
            if (start != NULL)
                *start = lo;
            if (end != NULL)
                *end = hi;
            const char *pn = (np > 0) ? line + np : "";
            while (*pn == ' ' || *pn == '\t')
                pn++;
            size_t l = strlen(pn);
            while (l > 0 && (pn[l - 1] == '\n' || pn[l - 1] == '\r'))
                l--;
            if (name != NULL && namelen > 0) {
                if (l == 0)
                    snprintf(name, namelen, "[anon]");
                else {
                    size_t c = (l < namelen - 1) ? l : namelen - 1;
                    memcpy(name, pn, c);
                    name[c] = '\0';
                }
            }
            hit = 1;
            break;
        }
    }
    fclose(f);
    return hit;
#else
    (void)pid;
    (void)addr;
    (void)start;
    (void)end;
    return 0;
#endif
}

#if defined(__linux__)
/* Reverse perf-map search: the JIT symbol whose [start, start+size) range contains
 * `addr` in /tmp/perf-<pid>.map. 1 + fills sym on a hit, 0 otherwise. */
static int perfmap_symbol_at(int pid, uint64_t addr, char *sym, size_t symlen) {
    char path[64];
    int p = (pid == 0) ? (int)getpid() : pid;
    snprintf(path, sizeof path, "/tmp/perf-%d.map", p);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;
    char line[512];
    int hit = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        unsigned long st = 0, sz = 0;
        int np = 0;
        if (sscanf(line, "%lx %lx %n", &st, &sz, &np) >= 2 && np > 0 &&
            addr >= st && addr < st + sz) {
            const char *nm = line + np;
            while (*nm == ' ' || *nm == '\t')
                nm++;
            size_t l = strlen(nm);
            while (l > 0 && (nm[l - 1] == '\n' || nm[l - 1] == '\r'))
                l--;
            if (sym != NULL && symlen > 0) {
                size_t c = (l < symlen - 1) ? l : symlen - 1;
                memcpy(sym, nm, c);
                sym[c] = '\0';
            }
            hit = 1;
            break;
        }
    }
    fclose(f);
    return hit;
}
#endif

size_t asmtest_hwtrace_symbolize_bucket(int pid, const uint64_t *ips, size_t n,
                                        asmtest_hwtrace_bucket_t *buckets,
                                        size_t cap) {
    if (ips == NULL || buckets == NULL || cap == 0)
        return 0;
    size_t nb = 0;
    for (size_t i = 0; i < n; i++) {
        char label[128];
        label[0] = '\0';
        /* Prefer a JIT perf-map symbol; else the mapped-file region; else unknown. */
#if defined(__linux__)
        if (!perfmap_symbol_at(pid, ips[i], label, sizeof label))
#endif
        {
            uint64_t s = 0, e = 0;
            if (!asmtest_hwtrace_region_name(pid, ips[i], label, sizeof label,
                                             &s, &e))
                snprintf(label, sizeof label, "[unknown]");
        }
        size_t j;
        for (j = 0; j < nb; j++)
            if (strcmp(buckets[j].label, label) == 0)
                break;
        if (j == nb) {
            if (nb >= cap)
                continue; /* out of bucket space: surplus label dropped */
            snprintf(buckets[nb].label, sizeof buckets[nb].label, "%s", label);
            buckets[nb].count = 0;
            j = nb;
            nb++;
        }
        buckets[j].count++;
    }
    return nb;
}

/* §Z1: attribute a whole-window scope's captured addresses to labelled buckets.
 * Each recorded ABSOLUTE address is classified against the caller's NAMED regions
 * first (so several leaves — distinct exec_alloc'd blobs that would otherwise both
 * resolve to "[anon]" — come back as separate, named buckets), then falls back to
 * the perf-map JIT symbol / mapped-file region for the runtime remainder. Buckets
 * accumulate by label; *nbuckets gets the distinct count; surplus labels past `cap`
 * are dropped (size `cap` to the expected label count for exact totals). */
int asmtest_hwtrace_attribute_window(
    asmtest_hwtrace_scope_t handle,
    const asmtest_hwtrace_named_region_t *regions, size_t nregions,
    asmtest_hwtrace_bucket_t *buckets, size_t cap, size_t *nbuckets) {
    if (nbuckets != NULL)
        *nbuckets = 0;
    if (buckets == NULL || cap == 0)
        return ASMTEST_HW_EINVAL;
#if defined(__linux__) && defined(__x86_64__)
    const void *base = NULL;
    size_t len = 0;
    asmtest_trace_t *trace = NULL;
    if (!asmtest_ss_frame_lookup(handle.idx, handle.gen, &base, &len, &trace) ||
        trace == NULL)
        return ASMTEST_HW_EINVAL; /* stale/unknown handle (or non-single-step) */
    /* A whole-window frame is uniquely base==NULL/len==0 (ss_push_frame). Reject a
     * REGION-scope handle: its insns[] hold base-RELATIVE offsets, not the absolute
     * addresses this classifies — silently bucketing them all as "[unknown]". */
    if (base != NULL || len != 0)
        return ASMTEST_HW_EINVAL;
    size_t nb = 0;
    for (size_t i = 0; i < trace->insns_len; i++) {
        uint64_t addr = trace->insns[i];
        char label[128];
        label[0] = '\0';
        /* Caller's named regions win (exact, symbol-free). */
        for (size_t r = 0; regions != NULL && r < nregions; r++)
            if (regions[r].len != 0 && addr >= regions[r].base &&
                addr < regions[r].base + regions[r].len) {
                snprintf(label, sizeof label, "%s", regions[r].name);
                break;
            }
        if (label[0] == '\0') {
            /* Runtime remainder: prefer a JIT perf-map symbol, else the mapped file. */
            if (!perfmap_symbol_at(0, addr, label, sizeof label)) {
                uint64_t s = 0, e = 0;
                if (!asmtest_hwtrace_region_name(0, addr, label, sizeof label,
                                                 &s, &e))
                    snprintf(label, sizeof label, "[unknown]");
            }
        }
        size_t j;
        for (j = 0; j < nb; j++)
            if (strcmp(buckets[j].label, label) == 0)
                break;
        if (j == nb) {
            if (nb >= cap)
                continue; /* out of bucket space: surplus label dropped */
            snprintf(buckets[nb].label, sizeof buckets[nb].label, "%s", label);
            buckets[nb].count = 0;
            j = nb;
            nb++;
        }
        buckets[j].count++;
    }
    if (nbuckets != NULL)
        *nbuckets = nb;
    return ASMTEST_HW_OK;
#else
    (void)handle;
    (void)regions;
    (void)nregions;
    return ASMTEST_HW_ENOSYS;
#endif
}

/* ------------------------------------------------------------------ */
/* §D3 — concealed out-of-process ptrace-stealth stepper               */
/* ------------------------------------------------------------------ */
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
/* The shared scratch (asmtest_stealth_scratch_t) and the stepping body
 * (asmtest_stealth_helper_run) live in src/stealth_helper.h/.c so the SAME code
 * runs whether the stepper is an in-process forked child (the fallback below) or
 * the exec'd standalone asmtest-stealth-helper binary (the bundled path). */
int asmtest_hwtrace_stealth_trace(const void *base, size_t len,
                                  asmtest_trace_t *trace, long *result_out,
                                  void (*run_region)(void *), void *arg) {
    if (base == NULL || len == 0 || trace == NULL || run_region == NULL)
        return ASMTEST_HW_EINVAL;
    size_t icap = trace->insns_cap ? trace->insns_cap : 256;
    size_t bcap = trace->blocks_cap ? trace->blocks_cap : 64;
    if (icap > 65536)
        icap = 65536;
    if (bcap > 4096)
        bcap = 4096;
    size_t total =
        sizeof(asmtest_stealth_scratch_t) + (icap + bcap) * sizeof(uint64_t);

    /* Prefer the bundled standalone helper binary if one is discoverable next to us
     * (or named via ASMTEST_STEALTH_HELPER); fall back to an in-process forked child
     * otherwise. The bundled path needs a memfd — a file the exec'd image can re-map
     * by fd — since an anonymous MAP_SHARED mapping survives fork but not exec. */
    char helperbuf[4096];
    const char *helper_path =
        asmtest_stealth_helper_path(helperbuf, sizeof helperbuf);
    int use_exec = 0;
    int shm_fd = -1;
#ifdef __NR_memfd_create
    if (helper_path != NULL) {
        shm_fd = (int)syscall(__NR_memfd_create, "asmtest_stealth", 0);
        if (shm_fd >= 0 && ftruncate(shm_fd, (off_t)total) == 0) {
            use_exec = 1;
        } else if (shm_fd >= 0) {
            close(shm_fd);
            shm_fd = -1;
        }
    }
#endif

    asmtest_stealth_scratch_t *sc = (asmtest_stealth_scratch_t *)mmap(
        NULL, total, PROT_READ | PROT_WRITE,
        use_exec ? MAP_SHARED : (MAP_SHARED | MAP_ANONYMOUS),
        use_exec ? shm_fd : -1, 0);
    if (sc == MAP_FAILED) {
        if (shm_fd >= 0)
            close(shm_fd);
        return ASMTEST_HW_EUNAVAIL;
    }
    memset(sc, 0, total);
    uint64_t *ibuf =
        (uint64_t *)((char *)sc + sizeof(asmtest_stealth_scratch_t));
    uint64_t *bbuf = ibuf + icap;
    sc->icap = icap; /* the stepping side recomputes its own buffer pointers */
    sc->bcap = bcap;
    sc->rc = ASMTEST_HW_EDECODE; /* until the stepper reports otherwise */

    /* Nominate any process to ptrace us so a Yama ptrace_scope=1 host permits the
     * helper's reverse attach; harmless when Yama is off. */
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    /* The CALLING THREAD, not the process leader: on a managed runtime (e.g. HotSpot)
     * the thread invoking run_region is a JVM-created thread whose tid != getpid(), so
     * seizing getpid() would step the wrong (idle primordial) thread and the run_to
     * breakpoint would fire on the UNTRACED calling thread — a fatal SIGTRAP. Target the
     * caller's own tid, matching asmtest_hwtrace_stealth_trace_windowed. */
    pid_t parent = (pid_t)syscall(SYS_gettid);
    pid_t helper = fork();
    if (helper < 0) {
        munmap(sc, total);
        if (shm_fd >= 0)
            close(shm_fd);
        return ASMTEST_HW_EUNAVAIL;
    }
    if (helper == 0) {
        /* HELPER (child): exec the bundled binary if we have one; on any exec
         * failure fall through to the identical in-process stepping body. Either
         * way asmtest_stealth_helper_run drives the reverse-attach + step. */
        if (use_exec) {
            char apid[24], afd[24], abase[32], alen[32];
            snprintf(apid, sizeof apid, "%d", (int)parent);
            snprintf(afd, sizeof afd, "%d", shm_fd);
            snprintf(abase, sizeof abase, "%#llx",
                     (unsigned long long)(uintptr_t)base);
            snprintf(alen, sizeof alen, "%zu", len);
            char *hargv[] = {(char *)helper_path, apid, afd, abase, alen, NULL};
            execv(helper_path, hargv);
            /* exec failed → fall through to the in-process stepper */
        }
        asmtest_stealth_helper_run(sc, parent, base, len);
        _exit(0);
    }

    /* CALLER (tracee): wait (busy, no syscall) until the helper has seized us and is
     * ready; we are INTERRUPT-stopped during this spin and only resume — seeing
     * `ready` — once the helper's run_to CONTs us with the entry breakpoint planted,
     * so the region cannot run untraced. In the bundled path also break if the
     * exec'd helper dies before publishing `ready` (a bad binary), so we never spin
     * forever waiting on a process that will never signal us. */
    int helper_gone = 0;
    while (!sc->ready) {
        if (use_exec) {
            int ws = 0;
            if (waitpid(helper, &ws, WNOHANG) == helper) {
                helper_gone = 1;
                break;
            }
        }
    }
    if (helper_gone) {
        munmap(sc, total);
        if (shm_fd >= 0)
            close(shm_fd);
        return ASMTEST_HW_EUNAVAIL;
    }
    if (sc->rc == ASMTEST_HW_EUNAVAIL) { /* attach refused before run_to */
        int st = 0;
        waitpid(helper, &st, 0);
        munmap(sc, total);
        if (shm_fd >= 0)
            close(shm_fd);
        return ASMTEST_HW_EUNAVAIL;
    }

    run_region(
        arg); /* invoke the region; the helper single-steps it out of band */

    int st = 0;
    waitpid(helper, &st, 0);

    int rc = sc->rc;
    if (rc == ASMTEST_HW_OK) {
        size_t ni = sc->shadow.insns_len;
        if (ni > trace->insns_cap)
            ni = trace->insns_cap;
        if (trace->insns != NULL)
            for (size_t i = 0; i < ni; i++)
                trace->insns[i] = ibuf[i];
        trace->insns_len = ni;
        trace->insns_total = sc->shadow.insns_total;
        size_t nb = sc->shadow.blocks_len;
        if (nb > trace->blocks_cap)
            nb = trace->blocks_cap;
        if (trace->blocks != NULL)
            for (size_t i = 0; i < nb; i++)
                trace->blocks[i] = bbuf[i];
        trace->blocks_len = nb;
        trace->blocks_total = sc->shadow.blocks_total;
        trace->truncated = sc->shadow.truncated;
        if (result_out != NULL)
            *result_out = sc->result;
    }
    munmap(sc, total);
    if (shm_fd >= 0)
        close(shm_fd);
    return rc;
}

/* §D3 WHOLE-WINDOW reverse-attach stepper — the out-of-process analog of the in-process
 * whole-window scope. Like asmtest_hwtrace_stealth_trace, a forked helper reverse-attaches
 * and single-steps the caller out of band, but it captures the WHOLE window
 * [win_base, win_base+win_len) PLUS every region the caller pre-published in `regions`
 * (the JIT/BCL code the window calls into), via asmtest_ptrace_trace_attached_windowed —
 * recording absolute addresses (classify by region). `run_region(arg)` invokes the window
 * body (a delegate whose call frame delimits the window: it ends when the body returns).
 * Two differences from the region variant: (1) it targets the CALLING THREAD's tid
 * (SYS_gettid), not getpid() — a managed worker is not the process leader; (2) it uses the
 * in-process fork path only (channel + scratch in an inherited MAP_SHARED mapping; no
 * exec'd bundled binary, so no memfd re-map). Crash-proof by construction: a ptrace-stop is
 * not gated by the tracee's signal mask, so the body survives code the in-process
 * single-step tier is forbidden to step. Linux x86-64/AArch64. */
int asmtest_hwtrace_stealth_trace_windowed(const void *win_base, size_t win_len,
                                           asmtest_addr_channel_t *chan,
                                           asmtest_trace_t *trace,
                                           long *result_out,
                                           void (*run_region)(void *),
                                           void *arg) {
    if (win_base == NULL || win_len == 0 || trace == NULL || run_region == NULL)
        return ASMTEST_HW_EINVAL;
    size_t icap = trace->insns_cap ? trace->insns_cap : 256;
    size_t bcap = trace->blocks_cap ? trace->blocks_cap : 64;
    if (icap > 65536)
        icap = 65536;
    if (bcap > 4096)
        bcap = 4096;
    size_t total =
        sizeof(asmtest_stealth_scratch_t) + (icap + bcap) * sizeof(uint64_t);

    asmtest_stealth_scratch_t *sc = (asmtest_stealth_scratch_t *)mmap(
        NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sc == MAP_FAILED)
        return ASMTEST_HW_EUNAVAIL;
    memset(sc, 0, total);
    sc->icap = icap;
    sc->bcap = bcap;
    sc->win = 1;
    /* The channel is a SHARED mapping the CALLER owns and pre-published coarse ranges
     * into (and the JIT listener publishes new methods into LIVE during the window). The
     * fork preserves its address, so the helper uses the same pointer directly. */
    sc->win_chan = chan;
    sc->rc = ASMTEST_HW_EDECODE;

    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    pid_t parent =
        (pid_t)syscall(SYS_gettid); /* the CALLING thread, not getpid() */
    pid_t helper = fork();
    if (helper < 0) {
        munmap(sc, total);
        return ASMTEST_HW_EUNAVAIL;
    }
    if (helper == 0) {
        asmtest_stealth_helper_run_windowed(sc, parent, win_base, win_len);
        _exit(0);
    }

    /* Busy-wait until the helper has seized us and is ready (INTERRUPT-stopped until its
     * run_to CONTs us with the entry breakpoint planted, so the window cannot run
     * untraced); break if the helper dies before publishing ready. */
    int helper_gone = 0;
    while (!sc->ready) {
        int ws = 0;
        if (waitpid(helper, &ws, WNOHANG) == helper) {
            helper_gone = 1;
            break;
        }
    }
    if (helper_gone || sc->rc == ASMTEST_HW_EUNAVAIL) {
        int st = 0;
        waitpid(helper, &st, 0);
        munmap(sc, total);
        return ASMTEST_HW_EUNAVAIL;
    }

    run_region(
        arg); /* the window body; the helper single-steps it out of band */

    int st = 0;
    waitpid(helper, &st, 0);

    int rc = sc->rc;
    if (rc == ASMTEST_HW_OK) {
        uint64_t *ibuf =
            (uint64_t *)((char *)sc + sizeof(asmtest_stealth_scratch_t));
        uint64_t *bbuf = ibuf + icap;
        size_t ni = sc->shadow.insns_len;
        if (ni > trace->insns_cap)
            ni = trace->insns_cap;
        if (trace->insns != NULL)
            for (size_t i = 0; i < ni; i++)
                trace->insns[i] = ibuf[i];
        trace->insns_len = ni;
        trace->insns_total = sc->shadow.insns_total;
        size_t nb = sc->shadow.blocks_len;
        if (nb > trace->blocks_cap)
            nb = trace->blocks_cap;
        if (trace->blocks != NULL)
            for (size_t i = 0; i < nb; i++)
                trace->blocks[i] = bbuf[i];
        trace->blocks_len = nb;
        trace->blocks_total = sc->shadow.blocks_total;
        trace->truncated = sc->shadow.truncated;
        if (result_out != NULL)
            *result_out = sc->result;
    }
    munmap(sc, total);
    return rc;
}

typedef struct {
    pid_t helper_pid;
    asmtest_stealth_scratch_t *sc;
    size_t total_size;
    size_t icap;
    size_t bcap;
} stealth_window_ctx_t;

int asmtest_hwtrace_stealth_window_begin(asmtest_addr_channel_t *chan,
                                         void **ctx_out) {
    if (ctx_out == NULL)
        return ASMTEST_HW_EINVAL;

    size_t icap = 65536;
    size_t bcap = 4096;
    size_t total =
        sizeof(asmtest_stealth_scratch_t) + (icap + bcap) * sizeof(uint64_t);

    asmtest_stealth_scratch_t *sc = (asmtest_stealth_scratch_t *)mmap(
        NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sc == MAP_FAILED)
        return ASMTEST_HW_EUNAVAIL;
    memset(sc, 0, total);
    sc->icap = icap;
    sc->bcap = bcap;
    sc->win = 1;
    sc->win_chan = chan;
    sc->rc = ASMTEST_HW_EDECODE;
    sc->stop = 0;
    sc->done = 0;

    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    pid_t parent = (pid_t)syscall(SYS_gettid);
    pid_t helper = fork();
    if (helper < 0) {
        munmap(sc, total);
        return ASMTEST_HW_EUNAVAIL;
    }
    if (helper == 0) {
        asmtest_stealth_helper_run_window_async(sc, parent);
        _exit(0);
    }

    int helper_gone = 0;
    while (!sc->ready) {
        int ws = 0;
        if (waitpid(helper, &ws, WNOHANG) == helper) {
            helper_gone = 1;
            break;
        }
    }
    if (helper_gone || sc->rc == ASMTEST_HW_EUNAVAIL) {
        int st = 0;
        waitpid(helper, &st, 0);
        munmap(sc, total);
        return ASMTEST_HW_EUNAVAIL;
    }

    stealth_window_ctx_t *ctx =
        (stealth_window_ctx_t *)malloc(sizeof(stealth_window_ctx_t));
    if (ctx == NULL) {
        kill(helper, SIGKILL);
        int st = 0;
        waitpid(helper, &st, 0);
        munmap(sc, total);
        return ASMTEST_HW_EUNAVAIL;
    }
    ctx->helper_pid = helper;
    ctx->sc = sc;
    ctx->total_size = total;
    ctx->icap = icap;
    ctx->bcap = bcap;

    *ctx_out = ctx;
    return ASMTEST_HW_OK;
}

int asmtest_hwtrace_stealth_window_end(void *ctx, asmtest_trace_t *trace) {
    if (ctx == NULL)
        return ASMTEST_HW_EINVAL;
    stealth_window_ctx_t *c = (stealth_window_ctx_t *)ctx;

    c->sc->stop = 1;

    while (!c->sc->done) {
        int ws = 0;
        if (waitpid(c->helper_pid, &ws, WNOHANG) == c->helper_pid) {
            break;
        }
        usleep(100);
    }

    int st = 0;
    waitpid(c->helper_pid, &st, 0);

    int rc = c->sc->rc;
    if (rc == ASMTEST_HW_OK && trace != NULL) {
        uint64_t *ibuf =
            (uint64_t *)((char *)c->sc + sizeof(asmtest_stealth_scratch_t));
        uint64_t *bbuf = ibuf + c->icap;

        size_t ni = c->sc->shadow.insns_len;
        if (ni > trace->insns_cap)
            ni = trace->insns_cap;
        if (trace->insns != NULL) {
            for (size_t i = 0; i < ni; i++)
                trace->insns[i] = ibuf[i];
        }
        trace->insns_len = ni;
        trace->insns_total = c->sc->shadow.insns_total;

        size_t nb = c->sc->shadow.blocks_len;
        if (nb > trace->blocks_cap)
            nb = trace->blocks_cap;
        if (trace->blocks != NULL) {
            for (size_t i = 0; i < nb; i++)
                trace->blocks[i] = bbuf[i];
        }
        trace->blocks_len = nb;
        trace->blocks_total = c->sc->shadow.blocks_total;
        trace->truncated = c->sc->shadow.truncated;
    }

    munmap(c->sc, c->total_size);
    free(c);
    return rc;
}
#else
int asmtest_hwtrace_stealth_trace(const void *base, size_t len,
                                  asmtest_trace_t *trace, long *result_out,
                                  void (*run_region)(void *), void *arg) {
    (void)base;
    (void)len;
    (void)trace;
    (void)result_out;
    (void)run_region;
    (void)arg;
    return ASMTEST_HW_ENOSYS;
}
int asmtest_hwtrace_stealth_trace_windowed(const void *win_base, size_t win_len,
                                           asmtest_addr_channel_t *chan,
                                           asmtest_trace_t *trace,
                                           long *result_out,
                                           void (*run_region)(void *),
                                           void *arg) {
    (void)win_base;
    (void)win_len;
    (void)chan;
    (void)trace;
    (void)result_out;
    (void)run_region;
    (void)arg;
    return ASMTEST_HW_ENOSYS;
}
int asmtest_hwtrace_stealth_window_begin(asmtest_addr_channel_t *chan,
                                         void **ctx_out) {
    (void)chan;
    (void)ctx_out;
    return ASMTEST_HW_ENOSYS;
}
int asmtest_hwtrace_stealth_window_end(void *ctx, asmtest_trace_t *trace) {
    (void)ctx;
    (void)trace;
    return ASMTEST_HW_ENOSYS;
}
#endif
