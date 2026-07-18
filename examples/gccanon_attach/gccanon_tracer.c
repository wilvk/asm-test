/*
 * gccanon_tracer.c — the TRACER half of F4 increments 1+2, and the lane's assertions.
 *
 * INCREMENT 2 (the composition) fixes the one limitation increment 1 landed with: every GC in one
 * call-out window is stamped with the same S0, so two of them collapse into one batch and
 * UNDER-FORWARD a twice-moved object (A->B->C canonicalizes to B while the load reads C). The fix is
 * to CHAIN the window's moves — see "increment 2: composing the GCs that share one frozen step
 * boundary" below — and hand the UNMODIFIED transform a batch whose ranges are already A->C. Run
 * `gccanon_tracer --selftest` for the composition's own proof against the shipping transform, with
 * no runtime, no dotnet and no ptrace.
 *
 * THE JOIN. F4's two halves both already exist: a proven live GC-move feed (an attach-mode
 * MovedReferences2 profiler — f4-attach-profiler-probe-findings.md) and a landed pure transform
 * (asmtest_gcmove_canonicalize, src/dataflow_gcmove.c, whose only caller today is its unit test).
 * This is the join, on a LIVE ATTACH:
 *
 *   1. resolve the managed region [base, base+len) from the victim's perf map (the tier's normal
 *      JIT method-resolution path);
 *   2. drive the SHIPPING scoped L0 producer — asmtest_dataflow_ptrace_attach_jit
 *      (src/dataflow_ptrace.c) — over one invocation of it on a live, foreign pid;
 *   3. publish that producer's live step counter so the attached profiler can stamp each moved
 *      range with the S0 of its GC;
 *   4. canonicalize the captured trace with those stamped triples and build the def-use graph.
 *
 * WHAT MAKES IT A PROOF RATHER THAN A DEMO. Step 4 is run TWICE over the same capture: once
 * WITHOUT canonicalization (the NEGATIVE CONTROL — the pre-move store's old address and the
 * post-move load's new address must look unrelated, so the edge is MISSING) and once WITH it (the
 * edge must APPEAR). Without the first, the second proves nothing: it would pass against a no-op.
 * A third assertion checks the other direction the transform exists for — that an object reusing
 * the VACATED old address is not forwarded onto the moved object, i.e. no FALSE edge is forged.
 *
 * WHY THE STEP COUNTER IS MIRRORED BY A THREAD RATHER THAN HOOKED. asmtest_gcmove_t.step is an index
 * into the value trace's insn_off[], which is exactly asmtest_valtrace_t.steps_len — and the trace
 * lives in THIS process's memory, filled by the producer as it steps. So a sibling thread can mirror
 * steps_len into shm with no change to the shipping producer at all: no hook, no API, no
 * perturbation of the tier under test. The mirror's polling race also disappears exactly where it
 * would matter — steps_len is FROZEN across the region's call-out (a region-gated counter records
 * nothing over the helper), which is where the GC lands.
 *
 * NO DynamoRIO: this is the out-of-band ptrace tier. Needs CAP_SYS_PTRACE.
 *
 * usage: gccanon_tracer <pid> <tid> <method-name-substring> [attach-timeout-s] [expect-gcs-per-window]
 *        gccanon_tracer --selftest
 *
 * expect-gcs-per-window (default 1) says which fixture the victim is running, i.e. what this run is
 * PROVING — 1 is increment 1's lane unchanged (and asserts that no two GCs share a step); N > 1 is
 * increment 2's (and asserts that N of them DO share one, that the collapse really loses the edge,
 * and that the composition restores it). It is a claim to be checked against the live feed, never a
 * value fed into the transform.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "asmtest_valtrace.h"
#include "gccanon_shm.h"

/* The value the region stores before the move and loads back after it — kept in step with
 * examples/gccanon_attach/victim/Program.cs (GcCanonVictim.Sentinel). */
#define GCCANON_SENTINEL 0x5EEDCAFE12345678ull

/* The scoped ptrace producer's return codes and entry point. It ships NO public header — a value-
 * trace producer is a tier, not part of the shared asmtest_valtrace.h sink API — so, exactly as its
 * own suite (examples/test_dataflow_ptrace.c) and asmspy (cli/asmspy_engine.c) both do, they are
 * re-declared here and kept in step with src/dataflow_ptrace.c. */
#define DF_PTRACE_OK 0      /* clean, complete scoped trace                  */
#define DF_PTRACE_FAULT 1   /* region faulted; a partial trace is filled     */
#define DF_PTRACE_NEVER 2   /* nobody reached the entry within the bound     */
#define DF_PTRACE_EINVAL -1 /* bad arguments                                 */
#define DF_PTRACE_ENOSYS -3 /* off Linux x86-64 / no Capstone: self-skip     */
#define DF_PTRACE_ETRACE -4 /* SEIZE/ptrace/wait failure (seccomp/yama)      */
int asmtest_dataflow_ptrace_attach_jit(pid_t pid, pid_t only_tid, uint64_t base,
                                       size_t code_len, void *img, uint64_t when,
                                       uint64_t max_insns, long *result,
                                       int *survived, asmtest_valtrace_t *vt);

static gccanon_channel_t *g_ch;
static asmtest_valtrace_t *g_vt;
static volatile int g_pub_stop;


static void sleep_ms(unsigned ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* ---- the shm channel ---------------------------------------------------------------------- */
/* Both sides open O_CREAT; the profiler usually wins (it is attached FIRST — its attach travels the
 * diagnostics IPC socket, which needs a RUNNING runtime, whereas this program stops its target).
 * Whoever loses finds the segment already sized. */
static int map_channel(void) {
    int fd = shm_open(GCCANON_SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        fprintf(stderr, "TRACER: shm_open(%s) failed: %s\n", GCCANON_SHM_NAME, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && (size_t)st.st_size < sizeof(gccanon_channel_t))
        if (ftruncate(fd, (off_t)sizeof(gccanon_channel_t)) != 0) {
            fprintf(stderr, "TRACER: ftruncate failed: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
    void *p = mmap(NULL, sizeof(gccanon_channel_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        fprintf(stderr, "TRACER: mmap failed: %s\n", strerror(errno));
        return -1;
    }
    g_ch = (gccanon_channel_t *)p;
    return 0;
}

/* ---- the step-counter mirror --------------------------------------------------------------- */
/* Publishes the LIVE producer's steps_len — the very index asmtest_gcmove_t.step means — so the
 * profiler can sample it as S0 from inside the GC fence. 20 us cadence; the value it must be right
 * about is a frozen one (see the file header), so this is belt-and-braces rather than a race. */
static void *publisher(void *arg) {
    (void)arg;
    while (!g_pub_stop) {
        if (g_ch && g_vt) {
            g_ch->step_counter = (uint32_t)g_vt->steps_len;
            /* recs_len alongside steps_len (increment 2). A record between two of a window's GCs is
             * exactly what would make chaining them wrong, and recs_len is the counter that would
             * have to move for one to exist. The profiler samples both at each fence's start and
             * end — see gccanon_gcinfo_t — and the tracer checks the window's GCs against them. */
            g_ch->recs_counter = (uint32_t)g_vt->recs_len;
        }
        struct timespec ts = {0, 20000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* ---- region resolution: the victim's perf map ---------------------------------------------- */
/* DOTNET_PerfMapEnabled=1 writes /tmp/perf-<pid>.map as "<addr> <size> <name>" — the same JIT
 * method map asmspy and the taint lanes already resolve managed methods through, so this is the
 * tier's normal path, not a fixture-only shortcut. Addresses may or may not be 0x-prefixed. */
static int resolve_region(int pid, const char *want, uint64_t *base, uint64_t *len, char *name,
                          size_t nname) {
    char path[64], line[1024];
    snprintf(path, sizeof path, "/tmp/perf-%d.map", pid);
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "TRACER: cannot open %s: %s (is DOTNET_PerfMapEnabled=1 set?)\n", path,
                strerror(errno));
        return -1;
    }
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long a = 0, s = 0;
        char nm[512];
        if (sscanf(line, "%llx %llx %511[^\n]", &a, &s, nm) != 3)
            continue;
        if (!strstr(nm, want))
            continue;
        /* Non-tiered (DOTNET_TieredCompilation=0) means one body per method; if a map ever carries
         * several, the LAST is the newest — the same greatest-version rule asmtest_method_resolve_pc
         * applies. Keep looking so the last match wins. */
        *base = (uint64_t)a;
        *len = (uint64_t)s;
        snprintf(name, nname, "%s", nm);
        found++;
    }
    fclose(f);
    if (found == 0) {
        fprintf(stderr, "TRACER: no perf-map entry matching '%s' in %s\n", want, path);
        return -1;
    }
    if (found > 1)
        printf("# NOTE: %d perf-map entries matched '%s' (tiering?); using the last\n", found, want);
    return 0;
}

/* ---- move-set assembly --------------------------------------------------------------------- */
static int move_cmp_step(const void *a, const void *b) {
    uint32_t x = ((const asmtest_gcmove_t *)a)->step, y = ((const asmtest_gcmove_t *)b)->step;
    return (x > y) - (x < y);
}

/* ============================================================================================== */
/* increment 2: composing the GCs that share one frozen step boundary                             */
/* ============================================================================================== */
/*
 * THE PROBLEM INCREMENT 1 LANDED WITH. asmtest_gcmove_t.step is stamped with the profiler-sampled
 * S0, and this tier's step counter is REGION-GATED: it freezes across the region's call-out
 * step-over — which is exactly where the GCs land. So every GC in one call-out window carries the
 * SAME S0, and asmtest_gcmove_canon applies at most ONE relocation per step-group. Two GCs in one
 * window therefore collapse into a single batch and UNDER-FORWARD a twice-moved object: A->B->C
 * canonicalizes to B while the load reads C, and the def-use edge goes missing — looking for all the
 * world like a transform bug.
 *
 * WHY THE OBVIOUS FIX IS WRONG. The one-relocation-per-batch rule is not an oversight to relax; it
 * is load-bearing. Within one GC the old ranges are disjoint, so an address matches at most one
 * range — and applying only one, then advancing past the group, is what stops a relocated address
 * from being re-relocated by a SIBLING whose OLD span the compaction slid the new range over. That
 * hazard is routine in a sliding compaction. Iterating within a batch would fix the two-GC case by
 * breaking the one-GC case. So the transform is left EXACTLY as it ships, and the fix goes on the
 * FEED side, where the collapse is actually caused.
 *
 * WHY CHAINING IS SOUND — the licence comes from the same freeze that causes the problem. `step` is
 * the only ordering information the trace carries, so two GCs sharing a step means NO RECORD CAN LIE
 * BETWEEN THEM: every record is either before both moves or after both. This lane checks that rather
 * than assuming it, two ways. Structurally: src/dataflow_ptrace.c runs a call-out via int3 +
 * PTRACE_CONT and records NOTHING over the helper, so for the whole window the producer sits in
 * waitpid and cannot append. Empirically: the freeze witness above samples the live trace's steps_len
 * AND recs_len from inside the fences and finds both unmoved across the window (ok 9), and the two
 * GCs are seen to sample the same S0 (ok 8). So for a pre-window record the correct canonical
 * address is the COMPOSITION of both moves. The GCs do not need SEPARATING — a frozen counter cannot
 * express a boundary between them and no re-stamp can invent one — their moves need CHAINING.
 *
 * THE SPEC, IN ONE LINE. The composed batch must make asmtest_gcmove_canon compute exactly what it
 * would have computed for the same GCs had the counter given them DISTINCT step boundaries. That is
 * an identity, and it is checked (build_separated + the differential oracle) against the shipping
 * transform itself — on the live feed, and exhaustively on randomized N-GC feeds in --selftest.
 * Composition is a counter-freeze WORKAROUND, not a change of semantics: it adds no rule of its own.
 */

typedef struct rng {
    uint64_t old_base, new_base, len;
} rng_t;

typedef struct rvec {
    rng_t *v;
    size_t n, cap;
} rvec_t;

static int rvec_push(rvec_t *a, uint64_t old_base, uint64_t new_base, uint64_t len) {
    if (len == 0)
        return 1; /* an empty piece carries no information */
    if (a->n == a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 64;
        rng_t *v = (rng_t *)realloc(a->v, cap * sizeof *v);
        if (!v)
            return 0;
        a->v = v;
        a->cap = cap;
    }
    a->v[a->n].old_base = old_base;
    a->v[a->n].new_base = new_base;
    a->v[a->n].len = len;
    a->n++;
    return 1;
}

static void rvec_free(rvec_t *a) {
    free(a->v);
    a->v = NULL;
    a->n = a->cap = 0;
}

static int rng_cmp_old(const void *a, const void *b) {
    uint64_t x = ((const rng_t *)a)->old_base, y = ((const rng_t *)b)->old_base;
    return (x > y) - (x < y);
}

/* Enforce the transform's precondition on ONE GC's batch: OLD ranges disjoint. MovedReferences2's
 * contract already implies it (two live objects do not overlap) and the live feed has always agreed,
 * but the composition's disjointness proof RESTS on it, so it is enforced rather than trusted and
 * `overlaps` keeps any violation visible as a finding. The handling is the tier's house style: drop
 * BOTH sides of an overlap — a dropped range forwards nothing, i.e. a CONSERVATIVE MISS (a missing
 * edge), never a false one — rather than guess which relocation owns the ambiguous address. Sorting
 * by old_base is semantically free: with disjoint olds an address matches at most one range, so
 * canon's first-match-wins cannot observe the order. */
static void rvec_disjointify(rvec_t *a, uint32_t *overlaps) {
    qsort(a->v, a->n, sizeof *a->v, rng_cmp_old);
    size_t w = 0;
    uint64_t maxend = 0; /* the furthest old END seen so far, INCLUDING already-dropped ranges */
    int have = 0;
    for (size_t i = 0; i < a->n; i++) {
        /* Look BACK at every earlier range, not just the adjacent one. Sorted by old_base, a long
         * range can overlap a later SHORT one it is not adjacent to (a third range starting between
         * them); comparing only with i-1 would drop the long one and keep the short one, and the
         * address they both claimed would then be forwarded by the survivor — a GUESS, and exactly
         * the false edge this is here to refuse. A running max of the ends catches it. Looking
         * FORWARD one is still needed for the other half of the policy: it is what drops the long
         * range itself rather than letting it out-vote its first overlapper. */
        int bad = (have && a->v[i].old_base < maxend) ||
                  (i + 1 < a->n && a->v[i + 1].old_base < a->v[i].old_base + a->v[i].len);
        uint64_t end = a->v[i].old_base + a->v[i].len;
        if (!have || end > maxend) { /* AFTER the test: a range never overlaps itself */
            maxend = end;
            have = 1;
        }
        if (bad) {
            (*overlaps)++;
            continue;
        }
        a->v[w++] = a->v[i];
    }
    a->n = w;
}

/* First range of `a` (sorted by old_base, olds disjoint — hence also sorted by old END) whose old
 * range ends strictly after `x`: the first one that can possibly cover `x`. */
static size_t rng_first_after(const rng_t *a, size_t n, uint64_t x) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a[mid].old_base + a[mid].len <= x)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/*
 * out = G o F: the map a pre-window address takes through the earlier GC `F` and then the later GC
 * `G`, both extended by the identity outside their own old ranges (an object a GC does not report is
 * an object it did not move). Both inputs must be sorted by old_base with DISJOINT olds.
 *
 * Unlike the shipping asmtest_gcmove_canon — which is deliberately wrap-safe, comparing
 * `cur - old_base < len` — the piecewise walks here compute `old_base + len` directly, so a range
 * that wrapped the top of the address space would be mis-walked. A managed heap cannot produce one,
 * and the feed is the runtime's own MovedReferences2, so this is recorded as an assumption rather
 * than defended against.
 *
 * PART 1 chains F's ranges: A->B, then wherever B lands in G, giving A->C. An image [b, b+len) may
 * cross SEVERAL of G's ranges — the runtime coalesces adjacent objects that move by one delta into
 * a single range, and the next GC is free to split that run — so it is walked PIECEWISE. Each piece
 * inside one of G's old ranges is emitted chained; each piece in a GAP is emitted with its original
 * A->B mapping, because that part did not move again. This is the PARTIAL-OVERLAP case, and it needs
 * no truncation: a relocation delta is uniform within a range, so every piece's mapping is exact.
 *
 * PART 2 adds G's own movers: the parts of G's ranges whose OLD address is NOT in F's old domain.
 * Those objects sat still through F, so F maps them to themselves and the composition is just G's own
 * relocation. Subtracting F's DOMAIN is exactly what keeps the composed OLD ranges DISJOINT, which
 * the transform requires: part 1 only ever emits subsets of dom(F), so anything part 2 emits is
 * disjoint from all of it by construction, and each part is internally disjoint because it emits
 * subsets of an already-disjoint set. It is also, address for address, what asmtest_gcmove_canon
 * itself computes for these two GCs when they land in different step groups (cur = F(x) if x is in
 * dom(F); then cur = G(cur) if cur is in dom(G)) — which is the identity the oracle checks.
 */
static int compose_pair(const rvec_t *F, const rvec_t *G, rvec_t *out) {
    out->n = 0;
    for (size_t i = 0; i < F->n; i++) {
        uint64_t a = F->v[i].old_base, b = F->v[i].new_base, end = b + F->v[i].len, p = b;
        size_t j = rng_first_after(G->v, G->n, p);
        while (p < end) {
            if (j >= G->n || G->v[j].old_base >= end) { /* no later range covers [p, end) */
                if (!rvec_push(out, a + (p - b), p, end - p))
                    return -1;
                break;
            }
            uint64_t cs = G->v[j].old_base, ce = cs + G->v[j].len;
            if (p < cs) { /* a gap: this piece is where F left it */
                if (!rvec_push(out, a + (p - b), p, cs - p))
                    return -1;
                p = cs;
            }
            uint64_t e = (end < ce) ? end : ce; /* the overlap [p, e) — chained */
            if (!rvec_push(out, a + (p - b), G->v[j].new_base + (p - cs), e - p))
                return -1;
            p = e;
            j++;
        }
    }
    for (size_t i = 0; i < G->n; i++) {
        uint64_t c = G->v[i].old_base, d = G->v[i].new_base, end = c + G->v[i].len, p = c;
        size_t j = rng_first_after(F->v, F->n, p);
        while (p < end) {
            if (j >= F->n || F->v[j].old_base >= end) { /* none of dom(F) left in [p, end) */
                if (!rvec_push(out, p, d + (p - c), end - p))
                    return -1;
                break;
            }
            uint64_t as = F->v[j].old_base, ae = as + F->v[j].len;
            if (p < as && !rvec_push(out, p, d + (p - c), as - p))
                return -1;
            p = (ae > end) ? end : (ae > p ? ae : p); /* skip what dom(F) already claims */
            j++;
        }
    }
    qsort(out->v, out->n, sizeof *out->v, rng_cmp_old);
    return 0;
}

typedef struct {
    uint32_t groups;           /* step boundaries in the feed                          */
    uint32_t collapsed_groups; /* boundaries carrying MORE THAN ONE GC — the collapse  */
    uint32_t max_gcs_in_group; /* the worst collapse                                   */
    uint32_t in_overlaps;      /* input ranges dropped: one GC's OLD ranges overlapped */
    uint32_t identity_dropped; /* composed ranges that came out old == new             */
    int oom;
} compose_stats_t;

/* A stamped move, sortable into (boundary, GC, address) order. */
typedef struct {
    uint64_t old_base, new_base, len;
    uint32_t step, gc_seq;
} smove_t;

static int smove_cmp(const void *a, const void *b) {
    const smove_t *x = (const smove_t *)a, *y = (const smove_t *)b;
    if (x->step != y->step)
        return x->step < y->step ? -1 : 1;
    if (x->gc_seq != y->gc_seq)
        return x->gc_seq < y->gc_seq ? -1 : 1;
    return (x->old_base > y->old_base) - (x->old_base < y->old_base);
}

static int gcmove_push(asmtest_gcmove_t **v, size_t *n, size_t *cap, uint64_t old_base,
                       uint64_t new_base, uint64_t len, uint32_t step) {
    if (*n == *cap) {
        size_t c = *cap ? *cap * 2 : 256;
        asmtest_gcmove_t *p = (asmtest_gcmove_t *)realloc(*v, c * sizeof *p);
        if (!p)
            return 0;
        *v = p;
        *cap = c;
    }
    (*v)[*n].old_base = old_base;
    (*v)[*n].new_base = new_base;
    (*v)[*n].len = len;
    (*v)[*n].step = step;
    (*n)++;
    return 1;
}

/* THE ENTRY POINT. Fold every step boundary's GCs — in gc_seq order — into ONE batch whose ranges
 * are already chained A->C, and hand THAT to the unmodified asmtest_gcmove_canonicalize. A boundary
 * carrying a single GC folds to itself: increment 1's path, unchanged. Returns a malloc'd array
 * sorted ascending by step (the transform's precondition), NULL if the feed is empty. */
static asmtest_gcmove_t *gccanon_compose(const gccanon_move_t *in, uint32_t n, uint32_t *out_n,
                                         compose_stats_t *st) {
    memset(st, 0, sizeof *st);
    *out_n = 0;
    if (n == 0)
        return NULL;

    smove_t *s = (smove_t *)malloc((size_t)n * sizeof *s);
    if (!s) {
        st->oom = 1;
        return NULL;
    }
    for (uint32_t i = 0; i < n; i++) {
        s[i].old_base = in[i].old_base;
        s[i].new_base = in[i].new_base;
        s[i].len = in[i].len;
        s[i].step = in[i].step;
        s[i].gc_seq = in[i].gc_seq;
    }
    qsort(s, n, sizeof *s, smove_cmp);

    asmtest_gcmove_t *out = NULL;
    size_t on = 0, ocap = 0;
    rvec_t f = {NULL, 0, 0}, g = {NULL, 0, 0}, t = {NULL, 0, 0};
    int ok = 1;

    for (uint32_t i = 0; i < n && ok;) {
        uint32_t step = s[i].step, j = i, gcs = 0;
        while (j < n && s[j].step == step)
            j++;
        for (uint32_t k = i; k < j; k++)
            if (k == i || s[k].gc_seq != s[k - 1].gc_seq)
                gcs++;
        st->groups++;
        if (gcs > 1)
            st->collapsed_groups++;
        if (gcs > st->max_gcs_in_group)
            st->max_gcs_in_group = gcs;

        f.n = 0;
        for (uint32_t p = i, first = 1; p < j && ok;) {
            uint32_t q = p;
            while (q < j && s[q].gc_seq == s[p].gc_seq)
                q++;
            g.n = 0;
            for (uint32_t k = p; k < q && ok; k++)
                if (!rvec_push(&g, s[k].old_base, s[k].new_base, s[k].len))
                    ok = 0;
            if (!ok)
                break;
            rvec_disjointify(&g, &st->in_overlaps);
            rvec_t tmp; /* swap, so the retired buffer is reused rather than reallocated */
            if (first) {
                tmp = f;
                f = g;
                g = tmp;
                first = 0;
            } else if (compose_pair(&f, &g, &t) != 0) {
                ok = 0;
            } else {
                tmp = f;
                f = t;
                t = tmp;
            }
            p = q;
        }
        for (size_t k = 0; k < f.n && ok; k++) {
            /* An identity piece maps an address to itself, so dropping it changes nothing: the
             * composed olds are disjoint, so no other range can claim that address instead. */
            if (f.v[k].old_base == f.v[k].new_base) {
                st->identity_dropped++;
                continue;
            }
            if (!gcmove_push(&out, &on, &ocap, f.v[k].old_base, f.v[k].new_base, f.v[k].len, step))
                ok = 0;
        }
        i = j;
    }

    rvec_free(&f);
    rvec_free(&g);
    rvec_free(&t);
    free(s);
    if (!ok) {
        st->oom = 1;
        free(out);
        return NULL;
    }
    *out_n = (uint32_t)on;
    return out;
}

/* ---- the differential oracle's reference model ----------------------------------------------- */
/*
 * Re-stamp the SAME GCs so the SHIPPING transform sees them as the DISTINCT batches the frozen
 * counter could not express, and let it chain them the long way — through its own multi-batch walk,
 * one relocation per batch, exactly as it does today for two GCs in two different windows. That is
 * the ground truth the composition must reproduce.
 *
 * There is no room BETWEEN two adjacent step indices, so the step space is scaled by K (at least the
 * largest number of GCs at any one boundary): a record at step r is probed at K*r, and the k-th of a
 * boundary's m GCs is stamped K*S0 - (m-k). That preserves the forwarding predicate EXACTLY — canon
 * forwards iff move.step > rec.step, and
 *     K*S0 - (m-k) > K*r   <=>   K*(S0-r) > m-k   <=>   r < S0
 * since 0 <= m-k <= m-1 < K — while making the GCs distinct and ordered by gc_seq, and keeping the
 * whole group inside (K*(S0-1), K*S0] so it cannot collide with a neighbouring boundary. The same
 * per-GC disjointify runs here, so the oracle compares the composition against the walk on identical
 * input, rather than comparing truncation policies.
 */
static asmtest_gcmove_t *build_separated(const gccanon_move_t *in, uint32_t n, uint32_t K,
                                         uint32_t *out_n) {
    *out_n = 0;
    if (n == 0 || K == 0)
        return NULL;
    smove_t *s = (smove_t *)malloc((size_t)n * sizeof *s);
    if (!s)
        return NULL;
    for (uint32_t i = 0; i < n; i++) {
        s[i].old_base = in[i].old_base;
        s[i].new_base = in[i].new_base;
        s[i].len = in[i].len;
        s[i].step = in[i].step;
        s[i].gc_seq = in[i].gc_seq;
        /* Refuse rather than wrap. step 0 has nothing below it to separate INTO (and the profiler
         * never stamps one — a step-0 move is inert and is dropped at the source); a step that
         * overflows the scaled space would silently alias another boundary. */
        if (s[i].step == 0 || (uint64_t)K * s[i].step >= 0xffffffffull) {
            free(s);
            return NULL;
        }
    }
    qsort(s, n, sizeof *s, smove_cmp);

    asmtest_gcmove_t *out = NULL;
    size_t on = 0, ocap = 0;
    rvec_t g = {NULL, 0, 0};
    int ok = 1;
    uint32_t sink = 0;
    for (uint32_t i = 0; i < n && ok;) {
        uint32_t step = s[i].step, j = i, m = 0;
        while (j < n && s[j].step == step)
            j++;
        for (uint32_t k = i; k < j; k++)
            if (k == i || s[k].gc_seq != s[k - 1].gc_seq)
                m++;
        /* The scaling identity above holds only for K >= m — otherwise K*step - (m-kth) UNDERFLOWS
         * and a wrapped stamp forwards everything while a zeroed one forwards nothing, quietly
         * turning the ORACLE (the thing that decides whether the lane passes) into a liar. Every
         * caller derives K from the same feed's max_gcs_in_group, so this refuses rather than
         * corrects: a NULL here surfaces as an honest "oracle unavailable". */
        if (K < m) {
            ok = 0;
            break;
        }
        uint32_t kth = 0;
        for (uint32_t p = i; p < j && ok;) {
            uint32_t q = p;
            while (q < j && s[q].gc_seq == s[p].gc_seq)
                q++;
            kth++;
            g.n = 0;
            for (uint32_t k = p; k < q && ok; k++)
                if (!rvec_push(&g, s[k].old_base, s[k].new_base, s[k].len))
                    ok = 0;
            rvec_disjointify(&g, &sink);
            for (size_t k = 0; k < g.n && ok; k++)
                if (!gcmove_push(&out, &on, &ocap, g.v[k].old_base, g.v[k].new_base, g.v[k].len,
                                 K * step - (m - kth)))
                    ok = 0;
            p = q;
        }
        i = j;
    }
    rvec_free(&g);
    free(s);
    if (!ok) {
        free(out);
        return NULL;
    }
    /* canon needs ascending step; emission order is already (boundary, then k), which is ascending,
     * but sorting keeps that a fact rather than a comment. */
    qsort(out, on, sizeof *out, move_cmp_step);
    *out_n = (uint32_t)on;
    return out;
}

/* Probe the identity the composition must satisfy at one (step, address): the composed batch and the
 * shipping transform's own multi-batch walk over the separated form must agree. */
static int oracle_agrees(const asmtest_gcmove_t *comp, uint32_t cn, const asmtest_gcmove_t *sep,
                         uint32_t sn, uint32_t K, uint32_t step, uint64_t addr, uint64_t *got,
                         uint64_t *want) {
    *got = asmtest_gcmove_canon(comp, cn, step, addr);
    *want = asmtest_gcmove_canon(sep, sn, K * step, addr);
    return *got == *want;
}

/* ---- --selftest: the composition vs. the shipping transform, on synthetic feeds --------------- */
/*
 * The live lane can only exercise the shapes a real .NET compaction happens to produce, on the one
 * object the fixture choreographs. This mode exercises the SPEC directly — "the composed batch makes
 * asmtest_gcmove_canon compute what it would have computed for the same GCs at distinct boundaries"
 * — over randomized feeds, EXHAUSTIVELY over every address of a small synthetic heap, with no
 * runtime, no dotnet, no ptrace and no privileges. It covers what a live fixture cannot guarantee it
 * will produce: N > 2 GCs in one window, an earlier range's image landing only PARTLY inside a later
 * range (the split), a later GC's own movers, and a new range landing on a sibling's OLD span — the
 * very hazard canon's one-relocation-per-batch rule exists for, which is why the new bases here are
 * placed anywhere rather than kept tidy. Deterministic: fixed seed, no time or address dependence.
 */
#define ST_HEAP 512u /* the synthetic heap: addresses [0, ST_HEAP) */
#define ST_S0 5u     /* the boundary every synthetic GC is stamped with — the collapse */

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

static uint32_t st_rnd(uint32_t m) {
    g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)((g_rng >> 33) % m);
}

/* One randomized feed: `ngcs` GCs, all stamped ST_S0 (i.e. collapsed), each with a random set of
 * disjoint old ranges and unconstrained new bases. Returns the number of ranges. */
static uint32_t st_gen(gccanon_move_t *mv, uint32_t cap, uint32_t ngcs) {
    uint32_t n = 0;
    for (uint32_t g = 1; g <= ngcs; g++) {
        uint32_t p = st_rnd(8);
        while (p < ST_HEAP && n < cap) {
            uint32_t len = 1 + st_rnd(24);
            if (p + len > ST_HEAP)
                break;
            uint32_t nb = st_rnd(ST_HEAP);
            if (nb != p) {
                mv[n].old_base = p;
                mv[n].new_base = nb;
                mv[n].len = len;
                mv[n].step = ST_S0;
                mv[n].gc_seq = g;
                mv[n].flags = 0; /* synthetic feed is all in-capture */
                n++;
            }
            p += len + st_rnd(16);
        }
    }
    return n;
}

static int gccanon_selftest(void) {
    int t = 0, fail = 0;
    printf("== gccanon-compose selftest (F4 inc 2: the composition vs. the SHIPPING transform, pure "
           "— no runtime, no ptrace) ==\n");

    /* 1 — the shape the whole increment is about, by hand: one object, moved twice, in one window.
     * A=0x1000 -> B=0x2000 -> C=0x3000, plus a bystander that only the second GC moves. */
    {
        gccanon_move_t mv[3] = {
            {0x1000, 0x2000, 0x40, ST_S0, 1, 0},
            {0x2000, 0x3000, 0x40, ST_S0, 2, 0},
            {0x8000, 0x9000, 0x40, ST_S0, 2, 0},
        };
        uint32_t cn = 0;
        compose_stats_t st;
        asmtest_gcmove_t *c = gccanon_compose(mv, 3, &cn, &st);
        uint64_t a = asmtest_gcmove_canon(c, cn, ST_S0 - 1, 0x1000 + 8);
        uint64_t b = asmtest_gcmove_canon(c, cn, ST_S0 - 1, 0x8000 + 8);
        uint64_t post = asmtest_gcmove_canon(c, cn, ST_S0, 0x1000 + 8);
        if (a == 0x3008 && b == 0x9008 && post == 0x1008 && st.collapsed_groups == 1) {
            printf("ok %d - A->B->C chains to C: a pre-window access at 0x1008 canonicalizes to "
                   "0x%llx (the FINAL home, not the intermediate 0x2008), a second-GC-only mover "
                   "to 0x%llx, and a post-window access stays at 0x%llx\n",
                   ++t, (unsigned long long)a, (unsigned long long)b, (unsigned long long)post);
        } else {
            printf("not ok %d - chain: got 0x%llx (want 0x3008), bystander 0x%llx (want 0x9008), "
                   "post 0x%llx (want 0x1008), collapsed_groups=%u\n",
                   ++t, (unsigned long long)a, (unsigned long long)b, (unsigned long long)post,
                   st.collapsed_groups);
            fail = 1;
        }
        free(c);
    }

    /* 2 — THE SHARP EDGE, by hand: an earlier range's image lands only PARTLY inside a later range.
     * F: [0x100,0x200) -> [0x500,0x600).  G: [0x550,0x650) -> [0x900,0xa00).
     * So [0x500,0x550) is where F left it and [0x550,0x600) moves again: the earlier range MUST
     * split, and G's own [0x600,0x650) part must survive as a mover in its own right. */
    {
        gccanon_move_t mv[2] = {
            {0x100, 0x500, 0x100, ST_S0, 1, 0},
            {0x550, 0x900, 0x100, ST_S0, 2, 0},
        };
        uint32_t cn = 0;
        compose_stats_t st;
        asmtest_gcmove_t *c = gccanon_compose(mv, 2, &cn, &st);
        uint64_t lo = asmtest_gcmove_canon(c, cn, ST_S0 - 1, 0x120);  /* -> 0x520, unmoved by G   */
        uint64_t hi = asmtest_gcmove_canon(c, cn, ST_S0 - 1, 0x1a0);  /* -> 0x5a0 -> 0x950        */
        uint64_t own = asmtest_gcmove_canon(c, cn, ST_S0 - 1, 0x620); /* G's own: -> 0x9d0        */
        if (lo == 0x520 && hi == 0x950 && own == 0x9d0) {
            printf("ok %d - PARTIAL OVERLAP splits exactly: the half of the earlier range whose "
                   "image the later GC does not touch stays at 0x%llx, the half it does chains on "
                   "to 0x%llx, and the later range's own remainder still maps (0x620 -> 0x%llx) — "
                   "no truncation needed, a delta is uniform within a range\n",
                   ++t, (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)own);
        } else {
            printf("not ok %d - partial overlap: lo=0x%llx (want 0x520) hi=0x%llx (want 0x950) "
                   "own=0x%llx (want 0x9d0)\n",
                   ++t, (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)own);
            fail = 1;
        }
        free(c);
    }

    /* 3 — THE CONSERVATIVE-TRUNCATION POLICY, which nothing else here can reach. One GC whose OLD
     * ranges OVERLAP violates MovedReferences2' contract (two live objects cannot occupy one
     * address) and the live feed has never done it — but the composition's disjointness proof rests
     * on it not happening, so the code enforces it rather than trusting it, and the house style says
     * an ambiguous address must be forwarded by NOBODY (a missing edge) rather than guessed at.
     *
     * The shape is chosen to be the one a neighbour-only check gets WRONG: the long range overlaps a
     * NON-ADJACENT short one, with a third range in between. Address 0x55 is claimed by [0,0x100)
     * and by [0x50,0x60); if either survives, 0x55 is forwarded — a false edge. All three must go. */
    {
        gccanon_move_t mv[3] = {
            {0x0, 0x5000, 0x100, ST_S0, 1, 0},
            {0x10, 0x7000, 0x2, ST_S0, 1, 0},
            {0x50, 0x9000, 0x10, ST_S0, 1, 0},
        };
        uint32_t cn = 0;
        compose_stats_t st;
        asmtest_gcmove_t *c = gccanon_compose(mv, 3, &cn, &st);
        uint64_t amb = asmtest_gcmove_canon(c, cn, ST_S0 - 1, 0x55);
        if (amb == 0x55 && cn == 0 && st.in_overlaps == 3) {
            printf("ok %d - CONSERVATIVE TRUNCATION: a GC reporting OVERLAPPING old ranges has all "
                   "%u of them dropped, so the doubly-claimed address 0x55 canonicalizes to itself "
                   "— a MISSING edge, never a guessed one — even though the overlap is with a "
                   "NON-adjacent range\n",
                   ++t, st.in_overlaps);
        } else {
            printf("not ok %d - conservative truncation: 0x55 forwarded to 0x%llx (want 0x55), "
                   "%u range(s) survived (want 0), %u dropped (want 3) — an ambiguous address was "
                   "GUESSED at\n",
                   ++t, (unsigned long long)amb, cn, st.in_overlaps);
            fail = 1;
        }
        free(c);
    }

    /* 4 — THE IDENTITY, exhaustively, on randomized N-GC feeds: composed == the shipping
     * transform's own walk over the separated form, at EVERY address of the synthetic heap and
     * either side of the boundary. This is the assertion that makes the composition a workaround
     * for a frozen counter rather than a second opinion about GC semantics. */
    {
        gccanon_move_t mv[512];
        uint64_t probes = 0, bad = 0;
        uint32_t trials = 0, maxgcs = 0, split_seen = 0, bad_disjoint = 0;
        uint64_t first_bad_addr = 0, first_bad_got = 0, first_bad_want = 0;
        uint32_t first_bad_step = 0;
        for (uint32_t trial = 0; trial < 300; trial++) {
            uint32_t ngcs = 2 + st_rnd(4); /* 2..5 GCs in ONE window */
            uint32_t n = st_gen(mv, 512, ngcs);
            if (n == 0)
                continue;
            uint32_t cn = 0, sn = 0;
            compose_stats_t st;
            asmtest_gcmove_t *c = gccanon_compose(mv, n, &cn, &st);
            uint32_t K = st.max_gcs_in_group < 2 ? 2 : st.max_gcs_in_group;
            asmtest_gcmove_t *sep = build_separated(mv, n, K, &sn);
            /* Skip only a trial the ALLOCATOR lost: `!c` is not the discriminator, because a feed
             * whose ranges all composed to identities legitimately yields c == NULL / cn == 0 and
             * must still be probed (canon must return the address unchanged for all of them). */
            if (!sep || st.oom) {
                free(c);
                free(sep);
                continue;
            }
            trials++;
            if (st.max_gcs_in_group > maxgcs)
                maxgcs = st.max_gcs_in_group;
            if (cn > n)
                split_seen++; /* composition emitted more ranges than it was given: real splits */
            /* the composed batch's OLD ranges must be disjoint — the transform's precondition.
             * Counted APART from the probe disagreements: they are different failures, and mixing
             * them makes the first-disagreement diagnostic below lie about which one fired. */
            for (uint32_t i = 1; i < cn; i++)
                if (c[i].step == c[i - 1].step && /* disjointness is a WITHIN-boundary property */
                    c[i].old_base < c[i - 1].old_base + c[i - 1].len) {
                    bad_disjoint++;
                    break;
                }
            for (uint32_t step = ST_S0 - 2; step <= ST_S0 + 1; step++)
                for (uint64_t addr = 0; addr < ST_HEAP + 64; addr++) {
                    uint64_t got, want;
                    probes++;
                    if (!oracle_agrees(c, cn, sep, sn, K, step, addr, &got, &want)) {
                        if (!bad) {
                            first_bad_addr = addr;
                            first_bad_step = step;
                            first_bad_got = got;
                            first_bad_want = want;
                        }
                        bad++;
                    }
                }
            free(c);
            free(sep);
        }
        if (bad == 0 && bad_disjoint == 0 && trials > 0 && probes > 0 && maxgcs >= 3 &&
            split_seen > 0) {
            printf("ok %d - DIFFERENTIAL ORACLE: over %u randomized collapsed feeds (up to %u GCs "
                   "sharing ONE boundary; %u produced range splits) the pre-composed batch and the "
                   "SHIPPING asmtest_gcmove_canon's own multi-batch walk over the separated form "
                   "agree on ALL %llu probes — every address of the heap, both sides of the "
                   "boundary — and every composed batch's OLD ranges are disjoint\n",
                   ++t, trials, maxgcs, split_seen, (unsigned long long)probes);
        } else {
            printf("not ok %d - differential oracle: %llu/%llu probes DISAGREE and %u/%u composed "
                   "batches were NOT old-disjoint (maxgcs=%u splits=%u); first disagreement at "
                   "step %u addr 0x%llx: composed 0x%llx vs. separated-walk 0x%llx\n",
                   ++t, (unsigned long long)bad, (unsigned long long)probes, bad_disjoint, trials,
                   maxgcs, split_seen, first_bad_step, (unsigned long long)first_bad_addr,
                   (unsigned long long)first_bad_got, (unsigned long long)first_bad_want);
            fail = 1;
        }
    }

    /* 5 — the negative control for the ORACLE itself. The collapsed feed, fed to the transform as
     * increment 1 fed it, must DISAGREE with the separated walk — otherwise test 4 would be
     * comparing two things that were never different and would pass against a no-op composition. */
    {
        gccanon_move_t mv[512];
        uint64_t probes = 0, differ = 0;
        for (uint32_t trial = 0; trial < 50; trial++) {
            uint32_t n = st_gen(mv, 512, 2 + st_rnd(3));
            if (n == 0)
                continue;
            uint32_t sn = 0;
            asmtest_gcmove_t *sep = build_separated(mv, n, 8, &sn);
            asmtest_gcmove_t *raw = (asmtest_gcmove_t *)calloc(n, sizeof *raw);
            if (!sep || !raw) {
                free(sep);
                free(raw);
                continue;
            }
            for (uint32_t i = 0; i < n; i++) { /* exactly what increment 1 hands the transform */
                raw[i].old_base = mv[i].old_base;
                raw[i].new_base = mv[i].new_base;
                raw[i].len = mv[i].len;
                raw[i].step = mv[i].step;
            }
            for (uint64_t addr = 0; addr < ST_HEAP; addr++) {
                probes++;
                if (asmtest_gcmove_canon(raw, n, ST_S0 - 1, addr) !=
                    asmtest_gcmove_canon(sep, sn, 8 * (ST_S0 - 1), addr))
                    differ++;
            }
            free(sep);
            free(raw);
        }
        if (differ > 0) {
            printf("ok %d - ORACLE NEGATIVE CONTROL: the COLLAPSED feed (increment 1's path) "
                   "disagrees with the separated walk on %llu/%llu probes — the collapse is real, "
                   "so the agreement above is a result and not a tautology\n",
                   ++t, (unsigned long long)differ, (unsigned long long)probes);
        } else {
            printf("not ok %d - oracle negative control: the collapsed feed already agrees with the "
                   "separated walk on all %llu probes — test 4 proves nothing\n",
                   ++t, (unsigned long long)probes);
            fail = 1;
        }
    }

    printf("1..%d\n", t);
    printf("GCCANON_SELFTEST fail=%d\n", fail);
    return fail ? 1 : 0;
}

/* ---- reading the window's chain straight out of the live feed --------------------------------- */
/* The distinct GCs (gc_seq, ascending) that stamped one step boundary. More than one IS the
 * collapse: the region-gated counter could not tell them apart. */
static uint32_t window_seqs(const gccanon_move_t *in, uint32_t n, uint32_t step, uint32_t *seqs,
                            uint32_t cap) {
    uint32_t ns = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (in[i].step != step)
            continue;
        int dup = 0;
        for (uint32_t k = 0; k < ns; k++)
            if (seqs[k] == in[i].gc_seq)
                dup = 1;
        if (!dup && ns < cap)
            seqs[ns++] = in[i].gc_seq;
    }
    /* ascending gc_seq IS the order the GCs ran in, which is the order the chain must be walked. */
    for (uint32_t i = 1; i < ns; i++) {
        uint32_t v = seqs[i], j = i;
        while (j > 0 && seqs[j - 1] > v) {
            seqs[j] = seqs[j - 1];
            j--;
        }
        seqs[j] = v;
    }
    return ns;
}

/* Walk `addr` through the window's GCs ONE AT A TIME, in gc_seq order, applying at most one of each
 * GC's ranges (its olds are disjoint). This is the physical chain A->B->C the victim's own pins
 * report — read here from the live feed, independently of the composition, so that "the object
 * really moved N times" is a MEASUREMENT and not an artefact of the code under test. chain[] must
 * hold nseqs+1 entries. Returns how many of the GCs actually relocated it. */
static uint32_t feed_chain(const gccanon_move_t *in, uint32_t n, uint32_t step, const uint32_t *seqs,
                           uint32_t nseqs, uint64_t addr, uint64_t *chain) {
    uint32_t moves = 0;
    chain[0] = addr;
    for (uint32_t k = 0; k < nseqs; k++) {
        uint64_t cur = chain[k];
        chain[k + 1] = cur;
        for (uint32_t i = 0; i < n; i++) {
            if (in[i].step != step || in[i].gc_seq != seqs[k])
                continue;
            if (cur < in[i].old_base || cur - in[i].old_base >= in[i].len)
                continue;
            chain[k + 1] = in[i].new_base + (cur - in[i].old_base);
            moves++;
            break;
        }
    }
    return moves;
}

/* Is there a def-use edge from `from` to `to` carried by an absolute memory location at `addr`? */
static int has_mem_edge(const asmtest_defuse_t *g, uint32_t from, uint32_t to, uint64_t addr) {
    if (!g)
        return 0;
    for (size_t i = 0; i < g->n; i++)
        if (g->edges[i].from_step == from && g->edges[i].to_step == to &&
            g->edges[i].loc.kind == AT_LOC_MEM_ABS && g->edges[i].loc.addr == addr)
            return 1;
    return 0;
}

int main(int argc, char **argv) {
    /* The composition's own proof, with no runtime and no privileges — see gccanon_selftest(). */
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
        return gccanon_selftest();

    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <pid> <tid> <method-name-substring> [attach-timeout-s] "
                "[expect-gcs-per-window]\n       %s --selftest\n",
                argv[0], argv[0]);
        return 2;
    }
    int pid = atoi(argv[1]);
    int tid = atoi(argv[2]);
    const char *want = argv[3];
    int timeout_s = argc > 4 ? atoi(argv[4]) : 60;
    /* What the victim's fixture claims to be doing, i.e. what THIS run proves. Never fed to the
     * transform — only checked against the live feed. */
    int expect_gcs = argc > 5 ? atoi(argv[5]) : 1;
    if (expect_gcs < 1)
        expect_gcs = 1;

    uint64_t base = 0, len = 0;
    char mname[512] = {0};
    if (resolve_region(pid, want, &base, &len, mname, sizeof mname) != 0) {
        printf("GCCANON_TRACER_FAIL could not resolve the managed region\n");
        return 1;
    }
    printf("# region resolved from the victim's perf map: base=0x%llx len=%llu  %s\n",
           (unsigned long long)base, (unsigned long long)len, mname);

    if (map_channel() != 0) {
        printf("GCCANON_TRACER_FAIL shm channel unavailable\n");
        return 1;
    }

    /* Caller-owned L0 buffers, sized for one small managed region (the shipped tier's own sizing
     * discipline: overflow is honest via vt->truncated rather than a lie). */
    g_vt = asmtest_valtrace_new(4096, 4096 * 8, 4096 * 16);
    if (!g_vt) {
        printf("GCCANON_TRACER_FAIL out of memory\n");
        return 1;
    }

    pthread_t pt;
    pthread_create(&pt, NULL, publisher, NULL);

    /* From here the profiler's S0 samples are of a LIVE counter. Set BEFORE the producer runs: it
     * has to run_to the region entry first (the worker calls the region every ~20 ms), and a GC that
     * lands during that wait samples a counter of 0 — which the profiler drops as inert, since canon
     * only forwards a record when a move's step is strictly greater than the record's. */
    g_ch->magic = GCCANON_MAGIC;

    printf("# driving the SHIPPING scoped L0 producer (asmtest_dataflow_ptrace_attach_jit) over one\n"
           "# invocation of the managed region on live pid=%d tid=%d (timeout %ds) ...\n",
           pid, tid, timeout_s);
    fflush(stdout);

    long result = 0;
    int survived = 0;
    int prc = asmtest_dataflow_ptrace_attach_jit(pid, tid, base, (size_t)len, NULL, 0, 0, &result,
                                                 &survived, g_vt);

    /* Publish tracer_done BEFORE clearing magic (increment 4 / T2): a GC that starts between the two
     * stores must be visible to the profiler's POST rule (tracer_done=1), not fall between "traced"
     * and "done" and be dropped. The publisher keeps mirroring the now-final, stable steps_len until
     * it is stopped just below, so a post-capture GC samples that frozen final count as its S0 —
     * strictly greater than every captured record's step, i.e. a trace-to-snapshot translation. */
    g_ch->tracer_done = 1;
    g_ch->magic = 0;
    g_pub_stop = 1;
    pthread_join(pt, NULL);

    size_t steps = asmtest_valtrace_steps(g_vt), nrecs = asmtest_valtrace_recs(g_vt);
    printf("GCCANON_CAPTURE rc=%d survived=%d steps=%zu recs=%zu truncated=%d result=0x%lx\n", prc,
           survived, steps, nrecs, (int)g_vt->truncated, result);

    if (prc == DF_PTRACE_ENOSYS || prc == DF_PTRACE_ETRACE) {
        printf("# SKIP: the scoped ptrace producer could not run here (rc=%d: no Capstone / "
               "ptrace refused)\n", prc);
        printf("1..0 # skipped\n");
        return 0;
    }
    /* NEVER is NOT a skip: the producer worked, the region simply did not arrive inside
     * the entry-wait bound. Before that bound existed this case HUNG here, so anything
     * that reaches this line is new information, not a regression. Fail loudly rather
     * than fall through and drain an empty feed as though the capture succeeded — and
     * name the lever, because on a slow/loaded box the honest fix is a longer wait, not
     * a code change. */
    if (prc == DF_PTRACE_NEVER) {
        printf("not ok - the traced region never arrived within the entry-wait bound "
               "(rc=%d). Raise ASMTEST_DF_ENTRY_WAIT_MS if this box is merely slow.\n", prc);
        return 1;
    }

    /* Give the victim a bounded moment to stamp at least one POST-capture move (increment 4 / T2):
     * its driver keeps dropping compacting gen2 GCs after the capture, but the drain runs within
     * milliseconds of detach, so without this the post-move observable would race. Bounded + honest:
     * it waits for the first post move or ~3 s, whichever comes first, and never stalls the lane. */
    for (int w = 0; w < 600 && g_ch->post_moves_total == 0; w++) {
        struct timespec pw = {0, 5 * 1000 * 1000}; /* 5 ms */
        nanosleep(&pw, NULL);
    }

    /* ---- drain the stamped feed ------------------------------------------------------------ */
    uint32_t nm_all = g_ch->nmoves;
    if (nm_all > GCCANON_MAX_MOVES)
        nm_all = GCCANON_MAX_MOVES;
    /* Partition on flags (increment 4 / T2): all in-capture (flags==0) moves PRECEDE all
     * post-capture ones — the profiler appends the former only while `magic` is set, the latter only
     * after tracer_done, and `magic` transitions exactly once — so the flags==0 prefix is exactly
     * what the landed increment-2 lane consumed. `nm` stays that prefix count, so moves[],
     * gccanon_compose, the window search, the differential oracle, and assertions 1-11 are all
     * byte-identical to before. The post moves stay in g_ch->moves[nm..] for the objid join (T5). */
    uint32_t nm = 0, npost = 0;
    for (uint32_t i = 0; i < nm_all; i++) {
        if (g_ch->moves[i].flags == GCCANON_MOVE_POST)
            npost++;
        else
            nm++;
    }
    asmtest_gcmove_t *moves = nm ? calloc(nm, sizeof *moves) : NULL;
    uint32_t distinct_gcs = 0, seqs[64];
    for (uint32_t i = 0; i < nm; i++) {
        moves[i].old_base = g_ch->moves[i].old_base;
        moves[i].new_base = g_ch->moves[i].new_base;
        moves[i].len = g_ch->moves[i].len;
        moves[i].step = g_ch->moves[i].step;
        uint32_t s = g_ch->moves[i].gc_seq;
        int seen = 0;
        for (uint32_t k = 0; k < distinct_gcs; k++)
            if (seqs[k] == s)
                seen = 1;
        if (!seen && distinct_gcs < 64)
            seqs[distinct_gcs++] = s;
    }
    /* asmtest_gcmove_canon requires ascending `step`; canonicalize sorts internally, but the direct
     * canon calls below (the false-alias check) need it too. */
    if (moves)
        qsort(moves, nm, sizeof *moves, move_cmp_step);

    printf("GCCANON_FEED gcs_seen=%u gcs_traced=%u recorded_moves=%u post_moves=%u reloc_seen=%u "
           "nonreloc_seen=%u last_s0=%u distinct_gcs_in_feed=%u\n",
           g_ch->gcs_seen, g_ch->gcs_traced, nm, npost, g_ch->moves_total, g_ch->nonreloc_total,
           g_ch->last_s0, distinct_gcs);

    /* ---- find the store and the load ------------------------------------------------------- */
    /* The region is Volatile.Write(ref obj[0], Sentinel) -> call Park() -> Volatile.Read(ref obj[0]):
     * an 8-byte MEM_ABS write of the sentinel, then an 8-byte MEM_ABS read of the same value at a
     * LATER step. The producer fills a write's value from the post-instruction state and a read's
     * from the pre-instruction state, so both carry the sentinel. */
    const at_val_rec_t *store = NULL, *load = NULL;
    for (size_t i = 0; i < g_vt->recs_len; i++) {
        const at_val_rec_t *r = &g_vt->recs[i];
        if (r->kind != AT_LOC_MEM_ABS || r->size != 8 || !r->value_valid ||
            r->value != GCCANON_SENTINEL)
            continue;
        if (r->is_write && !store)
            store = r;
        else if (!r->is_write && store && r->step > store->step && !load)
            load = r;
    }

    /* SNAPSHOT the two records' identity BEFORE anything canonicalizes the trace.
     * asmtest_gcmove_canonicalize rewrites recs[].addr IN PLACE, and `store` / `load` point straight
     * into that array — so after the transform runs, store->addr is already the NEW address. Reading
     * it afterwards silently turns the false-alias check into canon(new) == new, which is trivially
     * true: it passed, and it was checking nothing. Measured on this lane, not hypothesized. */
    uint32_t store_step = store ? store->step : 0, load_step = load ? load->step : 0;
    uint64_t store_addr_old = store ? store->addr : 0; /* the OLD (pre-move) address */
    uint64_t load_addr_new = load ? load->addr : 0;    /* the NEW (post-move) address */

    /* Same trap, whole-trace scale (increment 2). The lane now canonicalizes the SAME capture more
     * than once — the collapsed feed to reproduce the bug, then the chained one to fix it — and the
     * differential oracle probes every address the region touched. All of that needs the addresses
     * as CAPTURED, so they are snapshotted once, here, before anything rewrites recs[].addr in
     * place. Reading them back out of the trace afterwards would silently check canon(new) == new. */
    uint64_t *addr_snap = (uint64_t *)malloc((g_vt->recs_len + 1) * sizeof *addr_snap);
    if (!addr_snap) {
        /* Fail rather than skip: without the snapshot the assertions below would quietly stop
         * checking (and a shorter TAP plan is easy to miss), which is the failure mode this lane
         * exists to prevent in the first place. */
        printf("GCCANON_TRACER_FAIL out of memory snapshotting %zu record address(es)\n",
               g_vt->recs_len);
        return 1;
    }
    for (size_t i = 0; i < g_vt->recs_len; i++)
        addr_snap[i] = g_vt->recs[i].addr;

    /* ---- increment 2: CHAIN the GCs that share a boundary ---------------------------------- */
    /* Done unconditionally, for every run: a window carrying one GC folds to itself, so this is
     * increment 1's path when the fixture is increment 1's. The transform is handed the composed
     * batch; the RAW one is kept alongside precisely so the collapse can be reproduced below. */
    compose_stats_t cst;
    uint32_t cn = 0;
    asmtest_gcmove_t *comp = gccanon_compose(g_ch->moves, nm, &cn, &cst);
    printf("GCCANON_COMPOSE in=%u out=%u boundaries=%u collapsed=%u max_gcs_at_one_boundary=%u "
           "input_overlap_ranges_dropped=%u identity_ranges_dropped=%u oom=%d\n",
           nm, cn, cst.groups, cst.collapsed_groups, cst.max_gcs_in_group, cst.in_overlaps,
           cst.identity_dropped, cst.oom);

    int t = 0, fail = 0;
    printf("== gccanon-attach (F4 inc %d: live GC-move canonicalization on the ptrace attach tier"
           "%s) ==\n",
           expect_gcs > 1 ? 2 : 1,
           expect_gcs > 1 ? " — TWO-OR-MORE GCs IN ONE CALL-OUT WINDOW, chained" : "");

    /* 1 — the feed is live and NOT vacuous. */
    if (nm > 0 && g_ch->gcs_traced > 0) {
        printf("ok %d - live GC-move feed: %u relocating range(s) (old != new) stamped with a "
               "profiler-sampled S0 across %u GC(s) that opened with a live step counter "
               "(%u non-relocating ranges excluded as vacuous)\n",
               ++t, nm, g_ch->gcs_traced, g_ch->nonreloc_total);
    } else {
        printf("not ok %d - no stamped relocating ranges reached the tracer (moves=%u "
               "gcs_traced=%u) — nothing to canonicalize\n", ++t, nm, g_ch->gcs_traced);
        fail = 1;
    }

    /* 2 — the live scoped capture happened at all, and the target survived it. */
    if ((prc == DF_PTRACE_OK || prc == DF_PTRACE_FAULT) && steps > 0) {
        printf("ok %d - live-attach scoped L0 capture of the managed region: %zu steps / %zu operand "
               "records over one invocation on a FOREIGN pid, target survived=%d\n",
               ++t, steps, nrecs, survived);
    } else {
        printf("not ok %d - the scoped capture produced nothing (rc=%d steps=%zu)\n", ++t, prc, steps);
        fail = 1;
    }

    /* 3 — the capture really spans the compaction(s). This is the assertion that stops the whole
     * lane being vacuous: the store and the load must be at DIFFERENT addresses (the object
     * genuinely relocated — a PINNED object would not have moved and both would match), and the live
     * MovedReferences2 feed must EXPLAIN the difference exactly, by walking the window's GCs in
     * gc_seq order. For increment 1's one-GC window that is one hop, exactly as it was. For
     * increment 2's it is the CHAIN A->B->C: the object really moved TWICE inside one window, which
     * is the fixture's entire claim, measured here from the feed rather than asserted.
     *
     * `window` is found rather than assumed: it is the step boundary whose GCs carry the store's old
     * address to the address the load ACTUALLY READ. */
    uint32_t wseqs[64], nseqs = 0, window = 0;
    uint64_t chain[65];
    uint32_t hops = 0;
    int found_window = 0;
    uint32_t tried[16], ntried = 0;
    if (store && load) {
        for (uint32_t i = 0; i < nm && !found_window; i++) {
            uint32_t s = g_ch->moves[i].step;
            int seen = 0; /* each distinct boundary is worth testing exactly once */
            for (uint32_t k = 0; k < ntried; k++)
                if (tried[k] == s)
                    seen = 1;
            if (seen || ntried >= 16)
                continue;
            tried[ntried++] = s;
            uint32_t ns = window_seqs(g_ch->moves, nm, s, wseqs, 64);
            if (ns == 0 || ns > 64)
                continue;
            uint64_t c[65];
            uint32_t mv = feed_chain(g_ch->moves, nm, s, wseqs, ns, store_addr_old, c);
            if (mv == 0 || c[ns] != load_addr_new)
                continue;
            window = s;
            nseqs = ns;
            hops = mv;
            memcpy(chain, c, sizeof(uint64_t) * (ns + 1));
            found_window = 1;
        }
    }
    if (store && load && found_window && store_addr_old != load_addr_new && store_step < window &&
        load_step >= window) {
        char buf[1024];
        int off = snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)chain[0]);
        for (uint32_t k = 1; k <= nseqs && off > 0 && (size_t)off < sizeof buf; k++)
            off += snprintf(buf + off, sizeof buf - (size_t)off, " -[GC seq %u]-> 0x%llx", wseqs[k - 1],
                            (unsigned long long)chain[k]);
        printf("ok %d - the region spans %u compaction(s) in ONE call-out window: store@step=%u -> "
               "load@step=%u, and the live feed's GCs at boundary S0=%u carry the store's address "
               "to the address the load ACTUALLY READ: %s (delta=%+lld, %u of the %u GC(s) really "
               "relocated it)\n",
               ++t, nseqs, store_step, load_step, window, buf,
               (long long)(load_addr_new - store_addr_old), hops, nseqs);
    } else {
        printf("not ok %d - the capture does not span a relocation: store=%s load=%s "
               "store_addr=0x%llx load_addr=0x%llx explained=%s — the fixture did not provoke the "
               "bug, so nothing below would mean anything\n",
               ++t, store ? "found" : "MISSING", load ? "found" : "MISSING",
               (unsigned long long)store_addr_old, (unsigned long long)load_addr_new,
               found_window ? "yes" : "no");
        fail = 1;
    }

    /* 4 — THE NEGATIVE CONTROL. Without canonicalization the pre-move store (old address) and the
     * post-move load (new address) key on unrelated addresses, so the def-use edge is LOST. If this
     * passed, the positive result below could be a no-op. */
    asmtest_defuse_t *raw = asmtest_defuse_build(g_vt);
    int raw_edge = (store && load) ? has_mem_edge(raw, store_step, load_step, load_addr_new) : 1;
    if (store && load && !raw_edge) {
        printf("ok %d - NEGATIVE CONTROL: WITHOUT asmtest_gcmove_canonicalize the def-use edge "
               "store(step %u) -> load(step %u) is MISSING from the raw trace (%zu edges) — the "
               "GC-aliasing bug is real and this capture provokes it\n",
               ++t, store_step, load_step, raw ? raw->n : 0);
    } else {
        printf("not ok %d - NEGATIVE CONTROL FAILED: the edge is already present (or the records "
               "were not found) WITHOUT canonicalization — the positive result below would prove "
               "nothing\n", ++t);
        fail = 1;
    }

    /* 5 (increment 2 only) — THE COLLAPSE, REPRODUCED AS A FAILING CASE. Before proving the fix,
     * prove the bug: hand the transform the RAW feed exactly as increment 1 did — every GC in the
     * window stamped with the same S0 — and the twice-moved object UNDER-FORWARDS. canon applies one
     * relocation per step-group, so A->B->C stops at the intermediate B while the load reads C, and
     * the edge is still missing. Without this, "the edge appears with composition" could be the
     * fix for a bug that was never reachable.
     *
     * The trace's addresses are rewritten IN PLACE, so they are restored from the pre-transform
     * snapshot afterwards: the composed run below must start from the same raw capture, not from
     * this one's output. */
    uint64_t raw_canon_addr = 0;
    int collapse_edge = 0;
    if (expect_gcs > 1) {
        raw_canon_addr = asmtest_gcmove_canon(moves, nm, store_step, store_addr_old);
        size_t rch = asmtest_gcmove_canonicalize(g_vt, moves, nm);
        asmtest_defuse_t *collapsed = asmtest_defuse_build(g_vt);
        collapse_edge =
            (store && load) ? has_mem_edge(collapsed, store_step, load_step, load_addr_new) : 1;
        asmtest_defuse_free(collapsed);
        for (size_t i = 0; i < g_vt->recs_len; i++) /* undo: the composed run needs the RAW capture */
            g_vt->recs[i].addr = addr_snap[i];
        int is_intermediate = found_window && nseqs >= 2 && raw_canon_addr == chain[1];
        if (!collapse_edge && raw_canon_addr != load_addr_new) {
            printf("ok %d - THE COLLAPSE IS REAL, and this is it failing: with the %u GC(s) of one "
                   "window all stamped S0=%u, the UNMODIFIED transform (increment 1's path, "
                   "remapped=%zu) forwards the store's 0x%llx only as far as 0x%llx%s — but the "
                   "load read 0x%llx, so the def-use edge is STILL MISSING. One relocation per "
                   "step-group is applied, and a twice-moved object needs two\n",
                   ++t, nseqs, window, rch, (unsigned long long)store_addr_old,
                   (unsigned long long)raw_canon_addr,
                   is_intermediate ? " (the INTERMEDIATE address B — under-forwarded exactly as "
                                     "documented)"
                                   : " (an arbitrary range of the collapsed batch: with a shared "
                                     "step the batch's order decides, which is the same bug)",
                   (unsigned long long)load_addr_new);
        } else {
            printf("not ok %d - the collapse did NOT reproduce: the raw stamped feed already "
                   "forwards 0x%llx to 0x%llx (edge=%d) — the fixture did not put two GCs in one "
                   "window, so the composition below would be fixing nothing\n",
                   ++t, (unsigned long long)store_addr_old, (unsigned long long)raw_canon_addr,
                   collapse_edge);
            fail = 1;
        }
    }

    /* 6 — THE POSITIVE. Stamped triples -> chained by gccanon_compose -> the UNMODIFIED
     * asmtest_gcmove_canonicalize -> asmtest_defuse_build. */
    size_t changed = asmtest_gcmove_canonicalize(g_vt, comp, cn);
    asmtest_defuse_t *canon = asmtest_defuse_build(g_vt);
    /* After canonicalization the store's address IS the load's address (its final resting place). */
    uint64_t key = load_addr_new;
    int canon_edge = (store && load) ? has_mem_edge(canon, store_step, load_step, key) : 0;
    /* The positive is only allowed to REPORT a pass if the negative control held: an edge that was
     * already there without the transform is not an edge the transform restored. Measured on this
     * lane's first run, where the object did not relocate: store and load shared an address, canon
     * remapped 0 records, and the edge was trivially present at both ends. Without this gate that
     * reads as "ok 5 - ... def-use SURVIVED an induced GC compaction", which is exactly the false
     * pass the negative control exists to catch. */
    if (canon_edge && !raw_edge) {
        printf("ok %d - WITH canonicalization over the CHAINED feed (%u stamped range(s) from %u "
               "GC(s) composed into %u already-chained range(s); %zu record addresses remapped) the "
               "def-use edge store(step %u) -> load(step %u) APPEARS at the canonical address "
               "0x%llx — the object's TRUE final home, %s: a managed value's MEMORY def-use "
               "SURVIVED %u induced GC compaction(s) in ONE call-out window on a LIVE ATTACH\n",
               ++t, nm, cst.max_gcs_in_group, cn, changed, store_step, load_step,
               (unsigned long long)key,
               expect_gcs > 1 ? "not the intermediate the collapse stopped at" : "reached in one hop",
               nseqs);
    } else if (canon_edge && raw_edge) {
        printf("not ok %d - VACUOUS: the edge is present after canonicalization but it was ALREADY "
               "present before it (remapped=%zu) — the transform restored nothing; see the negative "
               "control above\n", ++t, changed);
        fail = 1;
    } else {
        printf("not ok %d - the def-use edge did NOT appear after canonicalization (remapped=%zu "
               "moves=%u) — F4's exit criterion is NOT met\n", ++t, changed, nm);
        fail = 1;
    }

    /* 7 — no FALSE edge. The other half of what the transform is for: an object that later occupies
     * the VACATED old address must not be forwarded onto the moved object. Checked directly against
     * the LIVE (composed) move set: a pre-move access at the old address forwards all the way to the
     * final home, while a post-move access at the same address stays put. Chaining must not weaken
     * this — a composed batch that forwarded post-window accesses too would forge exactly the alias
     * the transform exists to prevent. */
    if (store && load && found_window) {
        uint64_t vacated = store_addr_old; /* the OLD address, snapshotted pre-transform */
        uint64_t pre = asmtest_gcmove_canon(comp, cn, store_step, vacated);
        uint64_t post = asmtest_gcmove_canon(comp, cn, load_step, vacated);
        if (pre == load_addr_new && post == vacated && vacated != load_addr_new) {
            printf("ok %d - no FALSE alias forged: at the pre-move step %u the vacated address "
                   "0x%llx canonicalizes to the object's new home 0x%llx, but at the post-move step "
                   "%u it stays 0x%llx — an unrelated object reusing the vacated slot keeps a "
                   "DIFFERENT key\n",
                   ++t, store_step, (unsigned long long)vacated, (unsigned long long)pre,
                   load_step, (unsigned long long)post);
        } else {
            printf("not ok %d - false-alias check failed: pre=0x%llx (want 0x%llx) post=0x%llx "
                   "(want 0x%llx)\n", ++t, (unsigned long long)pre,
                   (unsigned long long)load_addr_new, (unsigned long long)post,
                   (unsigned long long)vacated);
            fail = 1;
        }
    } else {
        printf("not ok %d - false-alias check not reached (no explained relocation)\n", ++t);
        fail = 1;
    }

    /* 8 — the model's own precondition, and what the fixture claims about it. The step counter is
     * region-gated, so it is FROZEN across the call-out where the GCs land: every GC in one window
     * is stamped with the same S0, and canon applies at most ONE relocation per step-group (a group
     * models ONE disjoint batch). What the run PROVES depends on which fixture ran, so each is
     * checked against the feed rather than trusted:
     *   expect_gcs == 1  increment 1's window. No two GCs may share a boundary — the batch model
     *                    holds as shipped and nothing needs chaining.
     *   expect_gcs >  1  increment 2's. The GCs MUST share one, or the collapse this increment
     *                    exists to fix was never provoked and everything above is theatre. */
    if (expect_gcs == 1) {
        if (cst.collapsed_groups == 0) {
            printf("ok %d - the batch model holds: the %u stamped range(s) came from %u GC(s), and "
                   "no two distinct GCs share a step boundary (a shared step would collapse two "
                   "batches into one and silently under-forward a twice-moved object)\n",
                   ++t, nm, distinct_gcs);
        } else {
            printf("not ok %d - TWO DISTINCT GCs share one step boundary in a fixture that "
                   "choreographs ONE: %u boundary(ies) collapsed, worst %u GCs. Not a fixture "
                   "glitch — the region-gated counter cannot separate them\n",
                   ++t, cst.collapsed_groups, cst.max_gcs_in_group);
            fail = 1;
        }
    } else if (cst.collapsed_groups >= 1 && cst.max_gcs_in_group >= 2 && nseqs >= 2) {
        printf("ok %d - THE LIMITATION IS PROVOKED, not simulated: %u distinct GC(s) (seq %u..%u) "
               "really did land in ONE call-out window and were all stamped with the SAME "
               "profiler-sampled S0=%u, because the region-gated counter froze across the "
               "call-out — %u boundary(ies) collapsed, worst %u GCs at one boundary\n",
               ++t, nseqs, wseqs[0], wseqs[nseqs - 1], window, cst.collapsed_groups,
               cst.max_gcs_in_group);
    } else {
        printf("not ok %d - the fixture did NOT put %d GCs in one window: collapsed=%u max_gcs=%u "
               "gcs_in_window=%u — the collapse was not provoked\n",
               ++t, expect_gcs, cst.collapsed_groups, cst.max_gcs_in_group, nseqs);
        fail = 1;
    }

    /* 9 — THE COMPOSITION'S LICENCE, MEASURED. Chaining a window's GCs is only right because no
     * trace record can lie BETWEEN two of them. The profiler sampled the live trace's steps_len AND
     * recs_len at each fence's START and END (gccanon_gcinfo_t); this looks up exactly the GCs the
     * feed says are in this window — by gc_seq, so the victim's out-of-window fragmentation GCs
     * cannot get in — and requires every one of those samples to be the same. Both counters are
     * MONOTONIC, so equal values at the first GC's start and the last GC's end mean nothing was
     * appended anywhere across the whole window, the gaps between the GCs included.
     *
     * Structurally it could not be otherwise — src/dataflow_ptrace.c runs the call-out via int3 +
     * PTRACE_CONT and records NOTHING over the helper, so the producer is blocked in waitpid for the
     * entire window — but a claim this load-bearing is measured rather than argued. */
    uint32_t fz_found = 0, fz_bad = 0, fz_recs = 0;
    int fz_init = 0;
    for (uint32_t k = 0; k < nseqs; k++)
        for (uint32_t i = 0; i < g_ch->ngcinfo && i < GCCANON_MAX_GCINFO; i++) {
            if (g_ch->gcs[i].seq != wseqs[k])
                continue;
            fz_found++;
            if (!fz_init) {
                fz_recs = g_ch->gcs[i].s0_recs;
                fz_init = 1;
            }
            if (g_ch->gcs[i].s0_steps != window || g_ch->gcs[i].s1_steps != window ||
                g_ch->gcs[i].s0_recs != fz_recs || g_ch->gcs[i].s1_recs != fz_recs)
                fz_bad++;
        }
    if (found_window && nseqs > 0 && fz_found == nseqs && fz_bad == 0) {
        printf("ok %d - NO RECORD LIES BETWEEN THE WINDOW'S GCs: at the start AND the end of every "
               "one of the %u fence(s) in this window, the live trace's steps_len was %u and its "
               "recs_len was %u — identical across all %u sample(s), and both counters are "
               "monotonic, so the whole window (the GCs and the gaps between them) appended "
               "NOTHING. Every record is therefore before ALL the window's moves or after ALL of "
               "them, which is exactly what makes CHAINING them the right answer — and what no "
               "re-stamping of a frozen counter could achieve instead\n",
               ++t, fz_found, window, fz_recs, fz_found * 4);
    } else {
        printf("not ok %d - the trace did NOT freeze across the window: %u of %u window GC(s) have "
               "a fence record and %u of them saw the counters MOVE (steps want %u, recs want %u). "
               "A record between two GCs makes chaining them WRONG — the composition's precondition "
               "does not hold here and the design needs revisiting\n",
               ++t, fz_found, nseqs, fz_bad, window, fz_recs);
        fail = 1;
    }

    /* 10 — the transform's precondition, preserved BY the composition. Old ranges disjoint within a
     * batch is what lets canon apply exactly one relocation and stop; a composed batch that broke it
     * would silently reintroduce the double-apply the rule exists to prevent. */
    uint32_t notdisj = 0;
    for (uint32_t i = 1; i < cn; i++)
        if (comp[i].step == comp[i - 1].step && comp[i].old_base < comp[i - 1].old_base + comp[i - 1].len)
            notdisj++;
    if (cn > 0 && notdisj == 0) {
        printf("ok %d - the composed batch KEEPS the transform's precondition: all %u chained "
               "range(s) have DISJOINT old ranges within their boundary (%u input range(s) dropped "
               "for overlapping, %u identity range(s) dropped), so canon still matches at most one "
               "and its one-relocation-per-batch rule is untouched\n",
               ++t, cn, cst.in_overlaps, cst.identity_dropped);
    } else {
        printf("not ok %d - the composed batch has %u OVERLAPPING old range(s) of %u — canon's "
               "one-relocation-per-batch guard would pick one arbitrarily\n",
               ++t, notdisj, cn);
        fail = 1;
    }

    /* 11 — THE DIFFERENTIAL ORACLE, on the LIVE feed. The composition claims to be a workaround for
     * a frozen counter, not a new opinion about GC semantics. So: re-stamp the same live GCs into a
     * scaled step space where they are DISTINCT batches, let the SHIPPING transform chain them its
     * own way (one relocation per batch, walking the groups), and require the two to agree — on
     * every address this trace actually touched, plus a stride sample across the whole live feed.
     * `--selftest` does the same thing exhaustively on randomized N-GC feeds. */
    uint32_t K = cst.max_gcs_in_group < 2 ? 2 : cst.max_gcs_in_group;
    uint32_t sn = 0;
    asmtest_gcmove_t *sep = build_separated(g_ch->moves, nm, K, &sn);
    uint64_t oprobes = 0, obad = 0;
    if (sep != NULL) {
        for (size_t i = 0; i < g_vt->recs_len; i++) { /* every address the region touched */
            if (g_vt->recs[i].kind != AT_LOC_MEM_ABS)
                continue;
            uint64_t got, wnt;
            oprobes++;
            if (!oracle_agrees(comp, cn, sep, sn, K, g_vt->recs[i].step, addr_snap[i], &got, &wnt))
                obad++;
        }
        uint32_t stride = nm / 512 ? nm / 512 : 1; /* ... and a spread of the live feed itself */
        for (uint32_t i = 0; i < nm; i += stride) {
            uint64_t pts[3] = {g_ch->moves[i].old_base, g_ch->moves[i].old_base + g_ch->moves[i].len - 1,
                               g_ch->moves[i].new_base};
            for (int p = 0; p < 3; p++)
                for (uint32_t st2 = (window > 1 ? window - 2 : 0); st2 <= window + 1; st2++) {
                    uint64_t got, wnt;
                    oprobes++;
                    if (!oracle_agrees(comp, cn, sep, sn, K, st2, pts[p], &got, &wnt))
                        obad++;
                }
        }
    }
    if (sep != NULL && oprobes > 0 && obad == 0) {
        printf("ok %d - DIFFERENTIAL ORACLE on the LIVE feed: the chained batch and the SHIPPING "
               "asmtest_gcmove_canon's own multi-batch walk over the same GCs re-stamped as "
               "DISTINCT boundaries agree on all %llu probe(s) (every MEM_ABS address of the "
               "capture, plus a stride sample of the %u live range(s), either side of S0=%u). The "
               "composition computes what the transform would have computed if the counter had "
               "separated the GCs — no new semantics\n",
               ++t, (unsigned long long)oprobes, nm, window);
    } else {
        printf("not ok %d - differential oracle on the live feed: %llu/%llu probe(s) DISAGREE "
               "(separated=%s) — the composition is not equivalent to the shipping transform's own "
               "multi-batch walk\n",
               ++t, (unsigned long long)obad, (unsigned long long)oprobes,
               sep ? "built" : "UNAVAILABLE");
        fail = 1;
    }
    free(sep);

    printf("1..%d\n", t);
    printf("GCCANON_SUMMARY rc=%d survived=%d steps=%zu moves=%u composed=%u gcs_traced=%u "
           "gcs_in_window=%u collapsed_boundaries=%u remapped=%zu raw_edge=%d collapse_edge=%d "
           "canon_edge=%d oracle_probes=%llu oracle_bad=%llu fail=%d\n",
           prc, survived, steps, nm, cn, g_ch->gcs_traced, nseqs, cst.collapsed_groups, changed,
           raw_edge, collapse_edge, canon_edge, (unsigned long long)oprobes,
           (unsigned long long)obad, fail);

    asmtest_defuse_free(raw);
    asmtest_defuse_free(canon);
    asmtest_valtrace_free(g_vt);
    free(moves);
    free(comp);
    free(addr_snap);
    (void)sleep_ms;
    return fail ? 1 : 0;
}
