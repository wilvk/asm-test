/*
 * asmtest_codeimage.h — time-aware code-image recorder (a userspace PERF_RECORD_TEXT_POKE).
 *
 * The W2 out-of-process tracer (asmtest_ptrace.h) reads a foreign region's code bytes
 * with a SINGLE process_vm_readv snapshot. For a live JIT that is wrong the moment the
 * code is patched, freed, or has its address reused mid-trace: a late snapshot returns
 * the bytes that happen to be there at read time, not the bytes that were live when the
 * trace ran (docs/internal/analysis/jit-runtime-tracing.md, "The one hard problem — and it is
 * temporal"). The kernel solves this for its OWN self-modifying code with
 * PERF_RECORD_TEXT_POKE — old+new bytes, time-stamped — but there is no userspace
 * equivalent. This is that equivalent: a TIMESTAMPED CODE-IMAGE TIMELINE.
 *
 * Model. track() snapshots a region's bytes (version 0) and arms write-protect-async on
 * its pages. refresh() asks the kernel which of those pages were written since the last
 * arm and re-snapshots ONLY those, appending a new version stamped with the next
 * monotonic sequence ("logical timestamp"), then re-arms. bytes_at(addr, when) answers
 * "what bytes were live at addr as of sequence `when`" — the query a branch-trace decoder
 * or the W2 block-normalizer needs to reconstruct a method whose address was reused.
 *
 * Change detection is pure userspace, arch-independent (like the asmtest_proc_* readers),
 * and — crucially — works on a FOREIGN process, which is the JIT case:
 *   - Arm by clearing the soft-dirty PTE bit (write "4" to /proc/<pid>/clear_refs); any
 *     subsequent write to a page re-sets soft-dirty (pagemap bit 55).
 *   - Detect changed pages by reading that bit, batched via the PAGEMAP_SCAN ioctl
 *     (Linux >= 6.7, the PAGE_IS_SOFT_DIRTY category) where available, else by parsing
 *     /proc/<pid>/pagemap directly. No special privilege beyond reading the target.
 * (PAGEMAP_SCAN's precise write-protect-async mode — PM_SCAN_WP_MATCHING / PAGE_IS_WRITTEN
 * — is NOT used here: it requires the range be registered with userfaultfd by the owning
 * process, so it cannot monitor a foreign JIT. Soft-dirty is the cross-process primitive.)
 * Bytes are read via process_vm_readv, so a foreign pid needs only ptrace permission;
 * pid == 0 records THIS process (used by the self-test and in-process callers).
 *
 * Optional eBPF emission detector (Phase C, behind ASMTEST_HAVE_LIBBPF). A CO-RE BPF
 * program on the mprotect/mmap/memfd_create syscalls tells the recorder the MOMENT new
 * executable code appears for a target pid, so it can snapshot on the PROT_EXEC edge
 * instead of polling. It is SIDEBAND only (when/where code appears, never the instruction
 * stream) and self-skips cleanly without libbpf / CAP_BPF — the userspace poll path above
 * is the always-available fallback.
 *
 * Ships in libasmtest_hwtrace alongside the ptrace/PT/AMD/single-step backends.
 */
#ifndef ASMTEST_CODEIMAGE_H
#define ASMTEST_CODEIMAGE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* pid_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes (own namespace, in the spirit of asmtest_ptrace.h's). */
#define ASMTEST_CI_OK 0
#define ASMTEST_CI_EINVAL                                                      \
    (-1) /* bad argument                                       */
#define ASMTEST_CI_EUNAVAIL                                                    \
    (-3) /* no PAGEMAP_SCAN and no soft-dirty / no BPF privilege */
#define ASMTEST_CI_ENOSYS                                                      \
    (-5) /* built without the needed support (e.g. no libbpf)  */
#define ASMTEST_CI_ENOENT                                                      \
    (-7) /* address never tracked / no version at-or-before when */
#define ASMTEST_CI_ELOAD                                                       \
    (-8) /* libbpf load/attach failure (Phase C)               */

/* An opaque timestamped code-image timeline for one target process. */
typedef struct asmtest_codeimage asmtest_codeimage_t;

/* 1 if the userspace recorder can detect page changes on this host (PAGEMAP_SCAN, or the
 * soft-dirty fallback), else 0. Cached, hang-proof, side-effect-free self-probe — like
 * asmtest_ptrace_available(). */
int asmtest_codeimage_available(void);

/* A human-readable reason asmtest_codeimage_available() returned 0, into buf (always
 * NUL-terminated). */
void asmtest_codeimage_skip_reason(char *buf, size_t buflen);

/* Create a timeline recording `pid`'s memory (pid == 0 => this process). The caller owns
 * any ptrace attach policy for a foreign pid; the recorder itself only reads memory and
 * scans pagemap, so it needs no ptrace-stop. Returns NULL on allocation failure. */
asmtest_codeimage_t *asmtest_codeimage_new(pid_t pid);

/* Free a timeline and all recorded versions. Detaches any eBPF watch. NULL-safe. */
void asmtest_codeimage_free(asmtest_codeimage_t *img);

/* Begin tracking [base, base+len) in the target: snapshot version 0 now (process_vm_readv)
 * and arm write-protect-async on its pages so the next refresh() sees changes. May be
 * called for several disjoint regions. Returns ASMTEST_CI_OK, ASMTEST_CI_EINVAL, or a
 * negative status on a read/scan failure. */
int asmtest_codeimage_track(asmtest_codeimage_t *img, const void *base,
                            size_t len);

/* Scan the tracked ranges for pages changed since the last arm, re-snapshot each changed
 * page as a NEW version stamped with the next sequence, and re-arm write-protect. Returns
 * the number of new versions recorded (>= 0), or a negative status. Cheap when nothing
 * changed (one ioctl per tracked span, no reads). */
int asmtest_codeimage_refresh(asmtest_codeimage_t *img);

/* The current capture sequence — a monotonic logical timestamp the caller stamps trace
 * positions against. Advances by one for every version recorded (track + each refresh
 * change). 0 before anything is tracked. */
uint64_t asmtest_codeimage_now(const asmtest_codeimage_t *img);

/* The bytes live at `addr` as of capture sequence `when` (when == 0 => the latest
 * version). On success *out points at borrowed bytes owned by the timeline (valid until
 * asmtest_codeimage_free) and *out_len is the number of bytes available from `addr` to the
 * end of that version's region. Returns ASMTEST_CI_OK, ASMTEST_CI_ENOENT (addr not in any
 * tracked region, or no version at/before `when`), or ASMTEST_CI_EINVAL. Either out
 * pointer may be NULL. */
int asmtest_codeimage_bytes_at(const asmtest_codeimage_t *img, const void *addr,
                               uint64_t when, const uint8_t **out,
                               size_t *out_len);

/* Read up to `size` bytes of the target's CURRENT memory at `addr` into `buf`. Unlike
 * bytes_at (which serves only tracked, temporally-versioned regions), this reads live
 * process memory for ANY mapped address — used by the PT decoder to cover the STATIC
 * caller/loader code between a real capture's trace-enable point (TIP.PGE) and the
 * tracked JIT region. That code never moves, so it needs no versioning; serving it lets
 * the decoder walk into the region instead of stopping at the first unmapped IP. Reads
 * are clamped to the addressed page (process_vm_readv fails atomically). Returns the
 * number of bytes read (> 0), ASMTEST_CI_ENOENT if unmapped, or ASMTEST_CI_EINVAL /
 * ASMTEST_CI_ENOSYS. */
int asmtest_codeimage_read_live(const asmtest_codeimage_t *img, uint64_t addr,
                                uint8_t *buf, size_t size);

/* ------------------------------------------------------------------ */
/* Optional eBPF emission detector (Phase C). Self-skips without       */
/* libbpf / CAP_BPF; the userspace poll path above is the fallback.    */
/* ------------------------------------------------------------------ */

/* How a code-emission event was observed (kind field below). */
#define ASMTEST_CI_KIND_MPROTECT                                               \
    1 /* mprotect(...PROT_EXEC...) — the common JIT edge */
#define ASMTEST_CI_KIND_MMAP                                                   \
    2 /* mmap(...PROT_EXEC...); addr is the real base    */
#define ASMTEST_CI_KIND_MEMFD                                                  \
    3 /* memfd_create — staging hint; correlate via fd   */

/* A code-emission event from the eBPF detector. Byte-compatible mirror of the kernel-side
 * struct in bpf/codeimage_event.h (a _Static_assert in src/codeimage.c checks the size). */
typedef struct {
    uint64_t addr;      /* published base address (0 for a memfd hint)       */
    uint64_t len;       /* byte length (0 for a memfd hint)                  */
    uint64_t timestamp; /* bpf_ktime_get_ns() at emission                    */
    uint32_t pid;       /* tgid that published                               */
    uint32_t tid;       /* thread that published                             */
    uint32_t kind;      /* ASMTEST_CI_KIND_*                                 */
    int32_t fd;         /* memfd fd, or -1                                   */
} asmtest_codeimage_event_t;

/* 1 if the eBPF emission detector can load and attach on this host (built with libbpf,
 * kernel BTF present, sufficient privilege), else 0. Cached, hang-proof self-probe. */
int asmtest_codeimage_bpf_available(void);

/* A human-readable reason asmtest_codeimage_bpf_available() returned 0 (into buf, always
 * NUL-terminated). */
void asmtest_codeimage_bpf_skip_reason(char *buf, size_t buflen);

/* Load the CO-RE program, filter it to img's pid, and attach it. Subsequent
 * asmtest_codeimage_poll_bpf() calls drain emission events. Returns ASMTEST_CI_OK,
 * ASMTEST_CI_ENOSYS (built without libbpf), ASMTEST_CI_EUNAVAIL (no privilege / no BTF),
 * or ASMTEST_CI_ELOAD. */
int asmtest_codeimage_watch_bpf(asmtest_codeimage_t *img);

/* Drain ready emission events from the BPF ring buffer into img's internal queue.
 * timeout_ms == 0 is a NON-BLOCKING drain (so it interleaves with a ptrace single-step
 * loop); > 0 waits up to that long. Returns the number of events queued (>= 0) or a
 * negative status. */
int asmtest_codeimage_poll_bpf(asmtest_codeimage_t *img, int timeout_ms);

/* Pop one queued emission event into *out. Returns 1 if an event was returned, 0 if the
 * queue is empty, or a negative status. */
int asmtest_codeimage_next(asmtest_codeimage_t *img,
                           asmtest_codeimage_event_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ASMTEST_CODEIMAGE_H */
