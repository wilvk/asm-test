/* scenes_victim.c — the documented sample process for the desktop GUI's 3D
 * scenes (docs/guides/desktop-gui-scenes.md).
 *
 * THE SHAPE IS THE REQUIREMENT. Every element below exists to satisfy exactly
 * one scene availability gate in desktop/src/ui/shell.cpp; drop any one of them
 * and the corresponding scene renders an "unavailable" card instead of geometry.
 *
 *   blend_tile()   SSE work on a 16-byte tile. The dataflow producer decodes its
 *                  XMM operands into WIDE value records (>8 bytes, carried in
 *                  the `bytes` field), which is the LanePrism scene's only
 *                  input. Most of its writes are bulk moves (movdqa/movd) that
 *                  lane_width_class() correctly reports as having no element
 *                  width to know; paddd/psubd/pshufd/punpckldq are the ones
 *                  with a real, table-named lane width (2026-08-08 revised T5:
 *                  lane_width_for() no longer exists — see scene3d/
 *                  standalone.h's PrismWidth for the three-way verdict that
 *                  replaced its two-way "recorded or not"). Its ENTRY is also
 *                  arrived at constantly, which is what an --trace region
 *                  capture needs: a routine entered once yields one
 *                  invocation, and one invocation is not a scene.
 *
 *   worker()       Three threads descending through walk_heap/sort_batch/mix_math
 *                  into libc and libm. The ModuleRibbon scene's lanes are tids,
 *                  its Y is call depth and its colour is the module — so it needs
 *                  more than one thread AND more than one library, or it
 *                  degenerates into a single stripe.
 *
 *   walk_heap()    A strided walk over a few hundred KB. The address plane needs
 *                  observed data spans to have terrain at all, and the data-cell
 *                  and relief layers read the resolved effective addresses that
 *                  `asmspy --dataflow --mem` emits.
 *
 *   --seed N       Changes INPUT DATA ONLY, never code. The Divergence scene
 *                  gates on a matching code_sha/basis/arch and only then diffs
 *                  the statediff streams; a variant that changed a compiled
 *                  constant would produce a refusal card rather than a fork.
 *                  Two runs at different seeds share an identical prefix and
 *                  then diverge, which is exactly the scene's subject.
 *
 * blend_tile carries its own -O2 attribute: at the file's default -O0 the SSE
 * intrinsics expand into ~56 steps of movdqa traffic through stack slots, which
 * renders as a cluttered prism. The rest of the file stays at the tree's flags.
 *
 * Opts in via PR_SET_PTRACER_ANY like every other victim here: Yama
 * ptrace_scope=1 is the Ubuntu default and is NOT namespaced, so without it the
 * sibling attach is denied and the failure reads like a tracer bug.
 */
#include <emmintrin.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

#define HEAP_CELLS  32768 /* 256 KB of long — the terrain's data spans */
#define HEAP_STRIDE 97    /* coprime with HEAP_CELLS: a spread, not a sweep */
#define SORT_N      64

static volatile long g_sink;
static long *g_heap;
static int g_tile[4] __attribute__((aligned(16)));

/* Which path blend_tile takes, fixed once from --seed. THIS IS THE DIVERGENCE
 * SCENE'S SUBJECT: two runs of the SAME binary share a prefix and then part.
 *
 * It must be a runtime value, never a compile-time one. The divergence scene
 * compares two recordings of the same routine; if the seed selected the path at
 * compile time the two sides would be different code and the scene would refuse
 * the pair rather than draw a fork. `volatile` keeps the branch in the
 * instruction stream instead of letting -O2 fold it away. */
static volatile int g_variant;

/* The SSE hot routine: the LanePrism scene's whole input, and the region an
 * --trace capture pages by invocation. -O2 locally so the captured window is
 * tight enough to read; see the file comment. */
__attribute__((noinline, optimize("O2"))) long blend_tile(long x) {
    __m128i a = _mm_loadu_si128((const __m128i *)g_tile);
    __m128i b = _mm_set1_epi32((int)(x & 0x7fffffff));
    __m128i c;
    /* The shared prefix ends here. Both runs executed everything above
     * identically; from this branch on they part, which is exactly what the
     * divergence worldline draws. Both arms are 4-byte-lane ops, so the lane
     * prism reads the same width either way. */
    if (g_variant)
        c = _mm_sub_epi32(a, b); /* psubd — 4-byte lanes, nameable */
    else
        c = _mm_add_epi32(a, b);    /* paddd — 4-byte lanes, nameable */
    c = _mm_shuffle_epi32(c, 0x1B); /* pshufd — reverses the lane order */
    c = _mm_unpacklo_epi32(c, a);   /* punpckldq — interleaves two writes */
    _mm_storeu_si128((__m128i *)g_tile, c);
    return (long)_mm_cvtsi128_si32(c);
}

/* A strided walk: the terrain's observed data spans and the data-cell layers. */
__attribute__((noinline)) long walk_heap(long x) {
    long acc = 0;
    for (int i = 0; i < 256; i++) {
        size_t idx = (size_t)((x * HEAP_STRIDE + i * HEAP_STRIDE) % HEAP_CELLS);
        g_heap[idx] += x + i;
        acc += g_heap[idx];
    }
    return acc;
}

static int cmp_long(const void *a, const void *b) {
    long x = *(const long *)a, y = *(const long *)b;
    return (x > y) - (x < y);
}

/* Calls into libc (qsort, memcpy): a second module in the ribbon. */
__attribute__((noinline)) long sort_batch(long x) {
    long buf[SORT_N], tmp[SORT_N];
    for (int i = 0; i < SORT_N; i++)
        buf[i] = (x * 2654435761u + (unsigned)i * 40503u) & 0xffff;
    qsort(buf, SORT_N, sizeof(buf[0]), cmp_long);
    memcpy(tmp, buf, sizeof(buf));
    return tmp[0] + tmp[SORT_N - 1];
}

/* Calls into libm (sin/sqrt): a third module, at a deeper call depth. */
__attribute__((noinline)) long mix_math(long x) {
    double d = sin((double)(x & 0xff) * 0.017453292519943295);
    return (long)(sqrt(fabs(d) + 1.0) * 1000.0);
}

struct worker_arg {
    long seed;
    int depth; /* which helpers this thread descends through */
};

static void *worker(void *p) {
    struct worker_arg *w = (struct worker_arg *)p;
    for (long i = 0;; i++) {
        long x = w->seed + i;
        g_sink += blend_tile(x);
        if (w->depth >= 1)
            g_sink += walk_heap(x);
        if (w->depth >= 2)
            g_sink += sort_batch(x);
        if (w->depth >= 3)
            g_sink += mix_math(x);
        if ((i & 0x3ff) == 0)
            sched_yield(); /* be a good citizen on a shared box */
    }
    return NULL;
}

/* One deterministic pass, printed. --selftest runs this and exits, so the shape
 * test can assert determinism and divergence without attaching a tracer. */
static void selftest(long seed) {
    long acc = 0;
    for (long i = 0; i < 32; i++) {
        long x = seed + i;
        acc += blend_tile(x) + walk_heap(x) + sort_batch(x) + mix_math(x);
    }
    printf("selftest seed=%ld acc=%ld\n", seed, acc);
}

int main(int argc, char **argv) {
    long seed = 1;
    int nthreads = 3, self = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
            seed = strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            nthreads = (int)strtol(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--selftest") == 0)
            self = 1;
        else {
            fprintf(stderr, "usage: %s [--seed N] [--threads N] [--selftest]\n",
                    argv[0]);
            return 2;
        }
    }
    if (nthreads < 1)
        nthreads = 1;
    if (nthreads > 8)
        nthreads = 8;

    /* Fixed once, at runtime, from the seed: the two runs are the same binary
     * taking different paths — which is what keeps the divergence pair
     * comparable while still giving it a fork to find. */
    g_variant = (int)(seed & 1);

    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    g_heap = (long *)calloc(HEAP_CELLS, sizeof(long));
    if (g_heap == NULL) {
        fprintf(stderr, "scenes_victim: out of memory\n");
        return 1;
    }
    for (int i = 0; i < 4; i++)
        g_tile[i] = i + 1;

    fprintf(stderr, "scenes_victim pid=%d blend_tile=%p seed=%ld\n",
            (int)getpid(), (void *)blend_tile, seed);
    fflush(stderr);

    if (self) {
        selftest(seed);
        free(g_heap);
        return 0;
    }

    pthread_t th[8];
    struct worker_arg args[8];
    for (int i = 0; i < nthreads; i++) {
        args[i].seed = seed + i * 1000;
        args[i].depth = i + 1; /* distinct depths => distinct ribbon lanes */
        if (pthread_create(&th[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "scenes_victim: pthread_create failed\n");
            return 1;
        }
    }
    for (int i = 0; i < nthreads; i++)
        pthread_join(th[i], NULL);
    free(g_heap);
    return 0;
}
