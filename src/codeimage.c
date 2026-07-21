/*
 * codeimage.c — time-aware code-image recorder (a userspace PERF_RECORD_TEXT_POKE).
 * See asmtest_codeimage.h and docs/internal/analysis/jit-runtime-tracing.md ("approach #2").
 *
 * Records a TIMESTAMPED timeline of a process's code regions so a later query can ask
 * "what bytes were live at address X at trace-position T" — the correct answer for a JIT
 * whose code is patched, freed, or has its address reused mid-trace, where a single late
 * snapshot returns the wrong bytes.
 *
 * Versions are stored per tracked REGION (not per page): when any page in a region is
 * found written, the whole region is re-read and appended as a new version. Regions are
 * small JIT methods, so whole-region copies are cheap and the query becomes a trivial
 * offset into the chosen version's buffer.
 *
 * Change detection is soft-dirty (works cross-process — the foreign-JIT case): arm by
 * clearing the soft-dirty PTE bit via /proc/<pid>/clear_refs, detect set bits via the
 * PAGEMAP_SCAN ioctl (PAGE_IS_SOFT_DIRTY) where available, else by parsing
 * /proc/<pid>/pagemap. Bytes are read with process_vm_readv (self pid included).
 *
 * The optional eBPF emission detector lives at the bottom under ASMTEST_HAVE_LIBBPF; the
 * #else stubs self-skip so this TU always links whether or not libbpf is present.
 */
#define _GNU_SOURCE

#include "asmtest_codeimage.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The public event mirror must stay byte-compatible with the kernel-side struct in
 * bpf/codeimage_event.h (8+8+8+4+4+4+4); the eBPF ring-buffer payload is reinterpreted as
 * this type directly. */
_Static_assert(
    sizeof(asmtest_codeimage_event_t) == 40,
    "asmtest_codeimage_event_t must match bpf/codeimage_event.h layout");

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

#include <linux/fs.h> /* PAGEMAP_SCAN, struct pm_scan_arg, struct page_region, PAGE_IS_* */

/* pagemap bit 55 == soft-dirty (set on write since the last clear_refs). */
#define CI_PM_SOFT_DIRTY (1ULL << 55)
/* /proc/<pid>/clear_refs value that clears the soft-dirty PTE bits. */
#define CI_CLEAR_REFS_SOFT_DIRTY "4\n"

/* ---- internal containers -------------------------------------------------- */

typedef struct {
    uint64_t seq;   /* logical timestamp this version was recorded at */
    uint8_t *bytes; /* `len` bytes (region->len) */
} ci_version_t;

typedef struct {
    uint64_t base;       /* region start as requested by track()        */
    size_t len;          /* region byte length                          */
    uint64_t first_page; /* page-aligned base                           */
    size_t npages;       /* pages spanned                               */
    ci_version_t *vers;  /* versions, ordered by seq ascending          */
    size_t nver, cap_ver;
} ci_region_t;

struct asmtest_codeimage {
    pid_t pid; /* resolved target (0 -> self) */
    long page_size;
    int pagemap_scan_ok; /* 1 if PAGEMAP_SCAN usable (efficient read), 0 -> manual */
    uint64_t seq;        /* last sequence assigned (0 = nothing recorded yet) */
    ci_region_t *regions;
    size_t nreg, cap_reg;

#if defined(ASMTEST_HAVE_LIBBPF)
    struct ci_bpf *bpf; /* opaque eBPF detector state (lazily allocated) */
#endif
};

#if defined(ASMTEST_HAVE_LIBBPF)
static void
ci_bpf_detach(asmtest_codeimage_t *img); /* defined with the eBPF glue below */
#endif

/* ---- soft-dirty helpers --------------------------------------------------- */

static int ci_open_proc(pid_t pid, const char *leaf, int flags) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/%s", (int)pid, leaf);
    return open(path, flags);
}

/* Clear soft-dirty for the whole target so future writes become detectable. Global by
 * design (clear_refs is process-wide); callers arm once after recording, see refresh(). */
static int ci_arm(asmtest_codeimage_t *img) {
    int fd = ci_open_proc(img->pid, "clear_refs", O_WRONLY);
    if (fd < 0)
        return ASMTEST_CI_EUNAVAIL;
    ssize_t w = write(fd, CI_CLEAR_REFS_SOFT_DIRTY,
                      sizeof CI_CLEAR_REFS_SOFT_DIRTY - 1);
    close(fd);
    return w > 0 ? ASMTEST_CI_OK : ASMTEST_CI_EUNAVAIL;
}

/* Is `page_addr` soft-dirty in the target? Manual pagemap path (the portable fallback). */
static int ci_page_dirty_manual(asmtest_codeimage_t *img, uint64_t page_addr) {
    int fd = ci_open_proc(img->pid, "pagemap", O_RDONLY);
    if (fd < 0)
        return 0;
    off_t off = (off_t)(page_addr / (uint64_t)img->page_size) * 8;
    uint64_t entry = 0;
    ssize_t r = pread(fd, &entry, sizeof entry, off);
    close(fd);
    if (r != (ssize_t)sizeof entry)
        return 0;
    return (entry & CI_PM_SOFT_DIRTY) != 0;
}

/* Mark which of a region's pages are soft-dirty into dirty[0..npages). Returns 1 if any
 * page is dirty. Uses PAGEMAP_SCAN to read the bit in one ioctl when available. */
static int ci_region_dirty(asmtest_codeimage_t *img, const ci_region_t *r,
                           unsigned char *dirty) {
    memset(dirty, 0, r->npages);
    int any = 0;

#ifdef PAGEMAP_SCAN
    if (img->pagemap_scan_ok) {
        int fd = ci_open_proc(img->pid, "pagemap", O_RDONLY);
        if (fd >= 0) {
            struct page_region *vec =
                (struct page_region *)calloc(r->npages, sizeof *vec);
            if (vec != NULL) {
                struct pm_scan_arg arg;
                memset(&arg, 0, sizeof arg);
                arg.size = sizeof arg;
                arg.flags = 0; /* read only — no write-protect */
                arg.start = r->first_page;
                arg.end = r->first_page +
                          (uint64_t)r->npages * (uint64_t)img->page_size;
                arg.vec = (uintptr_t)vec;
                arg.vec_len = r->npages;
                arg.category_anyof_mask =
                    PAGE_IS_SOFT_DIRTY; /* report only dirty pages */
                arg.return_mask = PAGE_IS_SOFT_DIRTY;
                int n = (int)ioctl(fd, PAGEMAP_SCAN, &arg);
                if (n >= 0) {
                    for (int i = 0; i < n; i++) {
                        uint64_t s = vec[i].start, e = vec[i].end;
                        for (uint64_t p = s; p < e;
                             p += (uint64_t)img->page_size) {
                            size_t idx = (size_t)((p - r->first_page) /
                                                  (uint64_t)img->page_size);
                            if (idx < r->npages) {
                                dirty[idx] = 1;
                                any = 1;
                            }
                        }
                    }
                    free(vec);
                    close(fd);
                    return any;
                }
                free(vec);
            }
            close(fd);
        }
        /* ioctl failed at runtime — fall through to the manual path this call. */
    }
#endif /* PAGEMAP_SCAN */

    for (size_t i = 0; i < r->npages; i++) {
        uint64_t pa = r->first_page + (uint64_t)i * (uint64_t)img->page_size;
        if (ci_page_dirty_manual(img, pa)) {
            dirty[i] = 1;
            any = 1;
        }
    }
    return any;
}

/* ---- byte capture --------------------------------------------------------- */

/* Read `len` bytes at `base` from the target into a fresh buffer (NULL on failure). */
static uint8_t *ci_read_region(asmtest_codeimage_t *img, uint64_t base,
                               size_t len) {
    uint8_t *buf = (uint8_t *)malloc(len);
    if (buf == NULL)
        return NULL;
    struct iovec liov = {buf, len};
    struct iovec riov = {(void *)(uintptr_t)base, len};
    if (process_vm_readv(img->pid, &liov, 1, &riov, 1, 0) != (ssize_t)len) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* Append a new version (a copy of `bytes`, ownership transferred) at the next sequence. */
static int ci_region_add_version(asmtest_codeimage_t *img, ci_region_t *r,
                                 uint8_t *bytes) {
    if (r->nver == r->cap_ver) {
        size_t nc = r->cap_ver ? r->cap_ver * 2 : 4;
        ci_version_t *nv = (ci_version_t *)realloc(r->vers, nc * sizeof *nv);
        if (nv == NULL)
            return ASMTEST_CI_EUNAVAIL;
        r->vers = nv;
        r->cap_ver = nc;
    }
    r->vers[r->nver].seq = ++img->seq;
    r->vers[r->nver].bytes = bytes;
    r->nver++;
    return ASMTEST_CI_OK;
}

/* ---- availability self-probe (cached, hang-proof, side-effect-free) ------- */

/* Functional soft-dirty probe: clear our own soft-dirty, dirty a fresh page, confirm the
 * bit reads back set. A kernel without CONFIG_MEM_SOFT_DIRTY silently never sets it, so
 * this distinguishes "supported" from "present but inert". Side effects are confined to a
 * throwaway mmap; clearing our own soft-dirty is harmless to a probe-only process. */
static int ci_probe_softdirty(int *pagemap_scan_ok) {
    *pagemap_scan_ok = 0;
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0)
        return 0;

    int cr = open("/proc/self/clear_refs", O_WRONLY);
    if (cr < 0)
        return 0;
    if (write(cr, CI_CLEAR_REFS_SOFT_DIRTY,
              sizeof CI_CLEAR_REFS_SOFT_DIRTY - 1) <= 0) {
        close(cr);
        return 0;
    }
    close(cr);

    void *p = mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return 0;
    /* Re-clear now that the page exists, then dirty it. */
    cr = open("/proc/self/clear_refs", O_WRONLY);
    if (cr >= 0) {
        (void)!write(cr, CI_CLEAR_REFS_SOFT_DIRTY,
                     sizeof CI_CLEAR_REFS_SOFT_DIRTY - 1);
        close(cr);
    }
    *(volatile unsigned char *)p = 0xa5;

    int pm = open("/proc/self/pagemap", O_RDONLY);
    int ok = 0;
    if (pm >= 0) {
        off_t off = (off_t)((uint64_t)(uintptr_t)p / (uint64_t)ps) * 8;
        uint64_t entry = 0;
        if (pread(pm, &entry, sizeof entry, off) == (ssize_t)sizeof entry)
            ok = (entry & CI_PM_SOFT_DIRTY) != 0;
        close(pm);
    }

    /* Probe PAGEMAP_SCAN (read-only) so refresh() can batch reads when supported.
     * Compile-time gated: the ioctl and its structs landed in linux/fs.h with kernel
     * 6.7, and this file is built wherever libasmtest_dataflow links — including
     * Debian bookworm images whose headers are 6.1. Absent at build time, the manual
     * per-page pagemap read above is the whole story (pagemap_scan_ok stays 0), which
     * is the same path taken when the runtime ioctl is refused. */
#ifdef PAGEMAP_SCAN
    if (ok) {
        int pm2 = open("/proc/self/pagemap", O_RDONLY);
        if (pm2 >= 0) {
            struct page_region pr;
            struct pm_scan_arg arg;
            memset(&arg, 0, sizeof arg);
            arg.size = sizeof arg;
            arg.start = (uintptr_t)p;
            arg.end = (uintptr_t)p + (uint64_t)ps;
            arg.vec = (uintptr_t)&pr;
            arg.vec_len = 1;
            arg.return_mask = PAGE_IS_SOFT_DIRTY;
            if (ioctl(pm2, PAGEMAP_SCAN, &arg) >= 0)
                *pagemap_scan_ok = 1;
            close(pm2);
        }
    }
#endif /* PAGEMAP_SCAN */

    munmap(p, (size_t)ps);
    return ok;
}

static int ci_cached_avail = -1;
static int ci_cached_scan_ok = 0;

int asmtest_codeimage_available(void) {
    if (ci_cached_avail < 0)
        ci_cached_avail = ci_probe_softdirty(&ci_cached_scan_ok);
    return ci_cached_avail;
}

void asmtest_codeimage_skip_reason(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *msg =
        asmtest_codeimage_available()
            ? "available"
            : "soft-dirty page tracking unavailable (kernel built without "
              "CONFIG_MEM_SOFT_DIRTY, or no permission for "
              "/proc/<pid>/clear_refs)";
    strncpy(buf, msg, buflen - 1);
    buf[buflen - 1] = '\0';
}

/* ---- lifecycle ------------------------------------------------------------ */

asmtest_codeimage_t *asmtest_codeimage_new(pid_t pid) {
    asmtest_codeimage_t *img = (asmtest_codeimage_t *)calloc(1, sizeof *img);
    if (img == NULL)
        return NULL;
    img->pid = (pid == 0) ? getpid() : pid;
    img->page_size = sysconf(_SC_PAGESIZE);
    if (img->page_size <= 0)
        img->page_size = 4096;
    (void)asmtest_codeimage_available(); /* prime the cache */
    img->pagemap_scan_ok = ci_cached_scan_ok;
    return img;
}

void asmtest_codeimage_free(asmtest_codeimage_t *img) {
    if (img == NULL)
        return;
#if defined(ASMTEST_HAVE_LIBBPF)
    ci_bpf_detach(img);
#endif
    for (size_t i = 0; i < img->nreg; i++) {
        for (size_t v = 0; v < img->regions[i].nver; v++)
            free(img->regions[i].vers[v].bytes);
        free(img->regions[i].vers);
    }
    free(img->regions);
    free(img);
}

/* ---- track / refresh / query ---------------------------------------------- */

/* Detect soft-dirty pages in regions [lo, hi) and snapshot a new version of each
 * that changed. MUST be run over all existing regions before ci_arm(), since
 * clear_refs is process-global and re-arming erases pending soft-dirty state.
 * Returns the number of new versions (>= 0), or a negative ASMTEST_CI_* code. */
static int ci_scan_range(asmtest_codeimage_t *img, size_t lo, size_t hi) {
    int new_versions = 0;
    for (size_t i = lo; i < hi; i++) {
        ci_region_t *r = &img->regions[i];
        unsigned char stackbuf[64];
        unsigned char *dirty = r->npages <= sizeof stackbuf
                                   ? stackbuf
                                   : (unsigned char *)malloc(r->npages);
        if (dirty == NULL)
            return ASMTEST_CI_EUNAVAIL;

        int any = ci_region_dirty(img, r, dirty);
        if (dirty != stackbuf)
            free(dirty);
        if (!any)
            continue;

        uint8_t *bytes = ci_read_region(img, r->base, r->len);
        if (bytes == NULL)
            continue; /* region vanished (freed/unmapped) — keep prior versions */
        if (ci_region_add_version(img, r, bytes) != ASMTEST_CI_OK) {
            free(bytes);
            return ASMTEST_CI_EUNAVAIL;
        }
        new_versions++;
    }
    return new_versions;
}

int asmtest_codeimage_track(asmtest_codeimage_t *img, const void *base,
                            size_t len) {
    if (img == NULL || base == NULL || len == 0)
        return ASMTEST_CI_EINVAL;
    if (!asmtest_codeimage_available())
        return ASMTEST_CI_EUNAVAIL;

    if (img->nreg == img->cap_reg) {
        size_t nc = img->cap_reg ? img->cap_reg * 2 : 4;
        ci_region_t *nr = (ci_region_t *)realloc(img->regions, nc * sizeof *nr);
        if (nr == NULL)
            return ASMTEST_CI_EUNAVAIL;
        img->regions = nr;
        img->cap_reg = nc;
    }

    uint64_t b = (uint64_t)(uintptr_t)base;
    uint64_t ps = (uint64_t)img->page_size;
    uint64_t first = b & ~(ps - 1);
    uint64_t last = (b + len - 1) & ~(ps - 1);

    ci_region_t *r = &img->regions[img->nreg];
    memset(r, 0, sizeof *r);
    r->base = b;
    r->len = len;
    r->first_page = first;
    r->npages = (size_t)((last - first) / ps) + 1;

    uint8_t *bytes = ci_read_region(img, b, len);
    if (bytes == NULL)
        return ASMTEST_CI_EUNAVAIL;
    if (ci_region_add_version(img, r, bytes) != ASMTEST_CI_OK) {
        free(bytes);
        return ASMTEST_CI_EUNAVAIL;
    }
    img->nreg++;

    /* Arm: any write after this point is detectable on the next refresh. But
     * ci_arm() clears soft-dirty for the WHOLE process, so it would wipe any
     * pending (as-yet-unsnapshotted) writes to previously-tracked regions. Scan
     * and snapshot those existing regions first — the region added above was
     * just snapshotted, so it is excluded to avoid a redundant version. This is
     * the same scan-then-arm discipline refresh() uses. */
    if (img->nreg > 1) {
        int sc = ci_scan_range(img, 0, img->nreg - 1);
        if (sc < 0)
            return sc;
    }
    return ci_arm(img);
}

int asmtest_codeimage_refresh(asmtest_codeimage_t *img) {
    if (img == NULL)
        return ASMTEST_CI_EINVAL;
    if (img->nreg == 0)
        return 0;

    /* Detect + snapshot ALL regions BEFORE re-arming (clear_refs is global, so
     * re-arming would wipe pending soft-dirty state for regions not yet scanned). */
    int new_versions = ci_scan_range(img, 0, img->nreg);
    if (new_versions < 0)
        return new_versions;

    int rc = ci_arm(img);
    if (rc != ASMTEST_CI_OK)
        return rc;
    return new_versions;
}

uint64_t asmtest_codeimage_now(const asmtest_codeimage_t *img) {
    return img ? img->seq : 0;
}

int asmtest_codeimage_bytes_at(const asmtest_codeimage_t *img, const void *addr,
                               uint64_t when, const uint8_t **out,
                               size_t *out_len) {
    if (img == NULL || addr == NULL)
        return ASMTEST_CI_EINVAL;
    uint64_t a = (uint64_t)(uintptr_t)addr;

    for (size_t i = 0; i < img->nreg; i++) {
        const ci_region_t *r = &img->regions[i];
        if (a < r->base || a >= r->base + r->len)
            continue;

        /* Latest version with seq <= when (when == 0 => the newest). vers are seq-ascending. */
        const ci_version_t *pick = NULL;
        for (size_t v = 0; v < r->nver; v++) {
            if (when == 0 || r->vers[v].seq <= when)
                pick = &r->vers[v];
            else
                break;
        }
        if (pick == NULL)
            return ASMTEST_CI_ENOENT; /* tracked, but no version at/before `when` */

        size_t off = (size_t)(a - r->base);
        if (out != NULL)
            *out = pick->bytes + off;
        if (out_len != NULL)
            *out_len = r->len - off;
        return ASMTEST_CI_OK;
    }
    return ASMTEST_CI_ENOENT; /* addr not in any tracked region */
}

int asmtest_codeimage_read_live(const asmtest_codeimage_t *img, uint64_t addr,
                                uint8_t *buf, size_t size) {
    if (img == NULL || buf == NULL || size == 0)
        return ASMTEST_CI_EINVAL;
    /* One process_vm_readv fails atomically if any byte in the range is unmapped, so
     * clamp to the current page's remaining bytes — the decoder asks for up to a
     * max-instruction-length window that can straddle an unmapped next page. */
    long pg = sysconf(_SC_PAGESIZE);
    uint64_t psz = pg > 0 ? (uint64_t)pg : 4096;
    uint64_t page_end = (addr + psz) & ~(psz - 1);
    size_t avail = (size_t)(page_end - addr);
    if (size > avail)
        size = avail;
    struct iovec liov = {buf, size};
    struct iovec riov = {(void *)(uintptr_t)addr, size};
    ssize_t n = process_vm_readv(img->pid, &liov, 1, &riov, 1, 0);
    if (n <= 0)
        return ASMTEST_CI_ENOENT;
    return (int)n;
}

#else /* not Linux — link-compatible stubs */

int asmtest_codeimage_available(void) { return 0; }
void asmtest_codeimage_skip_reason(char *buf, size_t buflen) {
    if (buf && buflen)
        strncpy(buf, "code-image recorder is Linux-only", buflen - 1),
            buf[buflen - 1] = '\0';
}
asmtest_codeimage_t *asmtest_codeimage_new(pid_t pid) {
    (void)pid;
    return NULL;
}
void asmtest_codeimage_free(asmtest_codeimage_t *img) { (void)img; }
int asmtest_codeimage_track(asmtest_codeimage_t *img, const void *base,
                            size_t len) {
    (void)img, (void)base, (void)len;
    return ASMTEST_CI_ENOSYS;
}
int asmtest_codeimage_refresh(asmtest_codeimage_t *img) {
    (void)img;
    return ASMTEST_CI_ENOSYS;
}
uint64_t asmtest_codeimage_now(const asmtest_codeimage_t *img) {
    (void)img;
    return 0;
}
int asmtest_codeimage_bytes_at(const asmtest_codeimage_t *img, const void *addr,
                               uint64_t when, const uint8_t **out,
                               size_t *out_len) {
    (void)img, (void)addr, (void)when, (void)out, (void)out_len;
    return ASMTEST_CI_ENOSYS;
}
int asmtest_codeimage_read_live(const asmtest_codeimage_t *img, uint64_t addr,
                                uint8_t *buf, size_t size) {
    (void)img, (void)addr, (void)buf, (void)size;
    return ASMTEST_CI_ENOSYS;
}

#endif /* __linux__ */

/* ====================================================================== */
/* Optional eBPF emission detector (Phase C).                             */
/* Real body under ASMTEST_HAVE_LIBBPF; the #else stubs self-skip so      */
/* callers degrade cleanly (Linux without libbpf, or non-Linux).          */
/* ====================================================================== */
#if defined(ASMTEST_HAVE_LIBBPF)

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "codeimage.skel.h" /* bpftool-generated: struct codeimage_bpf + codeimage_bpf__* */

#define CI_BPF_QCAP                                                            \
    256 /* drained events buffered for asmtest_codeimage_next() */

struct ci_bpf {
    struct codeimage_bpf *skel;
    struct ring_buffer *rb;
    asmtest_codeimage_event_t q[CI_BPF_QCAP]; /* simple ring queue */
    size_t head, count;
};

static void ci_bpf_detach(asmtest_codeimage_t *img) {
    if (img == NULL || img->bpf == NULL)
        return;
    if (img->bpf->rb != NULL)
        ring_buffer__free(img->bpf->rb);
    if (img->bpf->skel != NULL)
        codeimage_bpf__destroy(img->bpf->skel);
    free(img->bpf);
    img->bpf = NULL;
}

/* Silence libbpf's stderr chatter (the availability probe must not spew). Set
 * ASMTEST_CODEIMAGE_DEBUG=1 to let libbpf's diagnostics through (load/attach failures). */
static int ci_bpf_silent(enum libbpf_print_level lvl, const char *fmt,
                         va_list ap) {
    (void)lvl;
    (void)fmt;
    (void)ap;
    return 0;
}
static int ci_bpf_verbose(enum libbpf_print_level lvl, const char *fmt,
                          va_list ap) {
    (void)lvl;
    return vfprintf(stderr, fmt, ap);
}
static libbpf_print_fn_t ci_bpf_print_fn(void) {
    return getenv("ASMTEST_CODEIMAGE_DEBUG") != NULL ? ci_bpf_verbose
                                                     : ci_bpf_silent;
}

static int ci_bpf_cached = -1;
static char ci_bpf_reason[160] = "available";

/* Load-then-destroy probe (no attach): cheap, hang-proof, leaves nothing attached. */
static int ci_bpf_probe(void) {
#if !(defined(__x86_64__) || defined(__aarch64__))
    snprintf(ci_bpf_reason, sizeof ci_bpf_reason,
             "eBPF detector is x86-64/aarch64 only");
    return 0;
#else
    if (access("/sys/kernel/btf/vmlinux", R_OK) != 0) {
        snprintf(ci_bpf_reason, sizeof ci_bpf_reason,
                 "kernel BTF unavailable (CONFIG_DEBUG_INFO_BTF off); CO-RE "
                 "needs it");
        return 0;
    }
    libbpf_print_fn_t prev = libbpf_set_print(ci_bpf_print_fn());
    struct codeimage_bpf *skel = codeimage_bpf__open();
    int ok = 0;
    if (skel == NULL) {
        snprintf(ci_bpf_reason, sizeof ci_bpf_reason,
                 "BPF skeleton open failed");
    } else if (codeimage_bpf__load(skel) != 0) {
        int e = errno;
        if (e == EPERM || e == EACCES)
            snprintf(ci_bpf_reason, sizeof ci_bpf_reason,
                     "insufficient privilege to load BPF (need CAP_BPF; "
                     "container must "
                     "--cap-add=BPF --security-opt seccomp=unconfined)");
        else if (e == EINVAL)
            snprintf(ci_bpf_reason, sizeof ci_bpf_reason,
                     "BPF program rejected (kernel too old? need >= 5.8 for "
                     "bpf_ringbuf)");
        else
            snprintf(ci_bpf_reason, sizeof ci_bpf_reason, "BPF load failed: %s",
                     strerror(e));
    } else {
        ok = 1;
    }
    if (skel != NULL)
        codeimage_bpf__destroy(skel);
    libbpf_set_print(prev);
    return ok;
#endif
}

int asmtest_codeimage_bpf_available(void) {
    if (ci_bpf_cached < 0)
        ci_bpf_cached = ci_bpf_probe();
    return ci_bpf_cached;
}

void asmtest_codeimage_bpf_skip_reason(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *msg =
        asmtest_codeimage_bpf_available() ? "available" : ci_bpf_reason;
    strncpy(buf, msg, buflen - 1);
    buf[buflen - 1] = '\0';
}

/* Ring-buffer callback: the payload is byte-identical to asmtest_codeimage_event_t
 * (asserted above), so reinterpret and enqueue. Never blocks/allocates. */
static int ci_bpf_on_event(void *ctx, void *data, size_t sz) {
    asmtest_codeimage_t *img = (asmtest_codeimage_t *)ctx;
    if (img == NULL || img->bpf == NULL ||
        sz < sizeof(asmtest_codeimage_event_t))
        return 0;
    struct ci_bpf *b = img->bpf;
    if (getenv("ASMTEST_CODEIMAGE_DEBUG") != NULL) {
        const asmtest_codeimage_event_t *e =
            (const asmtest_codeimage_event_t *)data;
        fprintf(stderr,
                "ci_bpf_on_event: kind=%u addr=0x%llx len=%llu pid=%u\n",
                e->kind, (unsigned long long)e->addr,
                (unsigned long long)e->len, e->pid);
    }
    if (b->count >= CI_BPF_QCAP)
        return 0; /* queue full — drop */
    size_t slot = (b->head + b->count) % CI_BPF_QCAP;
    b->q[slot] = *(const asmtest_codeimage_event_t *)data;
    b->count++;
    return 0;
}

int asmtest_codeimage_watch_bpf(asmtest_codeimage_t *img) {
    if (img == NULL)
        return ASMTEST_CI_EINVAL;
    if (!asmtest_codeimage_bpf_available())
        return ASMTEST_CI_EUNAVAIL;
    if (img->bpf != NULL)
        return ASMTEST_CI_OK; /* already watching */

    struct ci_bpf *b = (struct ci_bpf *)calloc(1, sizeof *b);
    if (b == NULL)
        return ASMTEST_CI_EUNAVAIL;

    libbpf_print_fn_t prev = libbpf_set_print(ci_bpf_print_fn());
    int rc = ASMTEST_CI_ELOAD;
    b->skel = codeimage_bpf__open();
    if (b->skel != NULL && codeimage_bpf__load(b->skel) == 0) {
        /* Identify the target by its PID NAMESPACE (dev+ino of /proc/<pid>/ns/pid) plus
         * its namespace-local tgid, written BEFORE attach (no race). This matches the
         * kernel-side bpf_get_ns_current_pid_tgid() translation, so the filter is correct
         * both on the host and inside a container's PID namespace (where the global tgid
         * the bare bpf_get_current_pid_tgid() would see differs from getpid()). */
        struct {
            uint64_t dev, ino, pid;
        } cfgv = {0, 0, (uint64_t)img->pid};
        char nspath[64];
        struct stat st;
        snprintf(nspath, sizeof nspath, "/proc/%d/ns/pid", (int)img->pid);
        if (stat(nspath, &st) == 0) {
            cfgv.dev = (uint64_t)st.st_dev;
            cfgv.ino = (uint64_t)st.st_ino;
        }
        uint32_t key = 0;
        bpf_map_update_elem(bpf_map__fd(b->skel->maps.cfg), &key, &cfgv,
                            BPF_ANY);
        if (codeimage_bpf__attach(b->skel) == 0) {
            b->rb = ring_buffer__new(bpf_map__fd(b->skel->maps.events),
                                     ci_bpf_on_event, img, NULL);
            if (b->rb != NULL)
                rc = ASMTEST_CI_OK;
        }
    }
    libbpf_set_print(prev);

    if (rc != ASMTEST_CI_OK) {
        if (b->rb != NULL)
            ring_buffer__free(b->rb);
        if (b->skel != NULL)
            codeimage_bpf__destroy(b->skel);
        free(b);
        return rc;
    }
    img->bpf = b;
    return ASMTEST_CI_OK;
}

int asmtest_codeimage_poll_bpf(asmtest_codeimage_t *img, int timeout_ms) {
    if (img == NULL || img->bpf == NULL || img->bpf->rb == NULL)
        return ASMTEST_CI_EINVAL;
    size_t before = img->bpf->count;
    int rc = ring_buffer__poll(img->bpf->rb, timeout_ms);
    if (rc < 0)
        return ASMTEST_CI_ELOAD;
    return (int)(img->bpf->count - before);
}

int asmtest_codeimage_next(asmtest_codeimage_t *img,
                           asmtest_codeimage_event_t *out) {
    if (img == NULL || img->bpf == NULL)
        return ASMTEST_CI_EINVAL;
    struct ci_bpf *b = img->bpf;
    if (b->count == 0)
        return 0;
    if (out != NULL)
        *out = b->q[b->head];
    b->head = (b->head + 1) % CI_BPF_QCAP;
    b->count--;
    return 1;
}

#else /* !ASMTEST_HAVE_LIBBPF — self-skip stubs (Linux without libbpf, or non-Linux) */

int asmtest_codeimage_bpf_available(void) { return 0; }

void asmtest_codeimage_bpf_skip_reason(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *msg =
        "built without libbpf (eBPF emission detector); the userspace "
        "soft-dirty recorder is the fallback";
    strncpy(buf, msg, buflen - 1);
    buf[buflen - 1] = '\0';
}

int asmtest_codeimage_watch_bpf(asmtest_codeimage_t *img) {
    (void)img;
    return ASMTEST_CI_ENOSYS;
}
int asmtest_codeimage_poll_bpf(asmtest_codeimage_t *img, int timeout_ms) {
    (void)img, (void)timeout_ms;
    return ASMTEST_CI_ENOSYS;
}
int asmtest_codeimage_next(asmtest_codeimage_t *img,
                           asmtest_codeimage_event_t *out) {
    (void)img, (void)out;
    return ASMTEST_CI_ENOSYS;
}

#endif /* ASMTEST_HAVE_LIBBPF */
