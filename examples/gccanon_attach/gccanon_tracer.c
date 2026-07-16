/*
 * gccanon_tracer.c — the TRACER half of F4 increment 1, and the lane's assertions.
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
 * usage: gccanon_tracer <pid> <tid> <method-name-substring> [attach-timeout-s]
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
        if (g_ch && g_vt)
            g_ch->step_counter = (uint32_t)g_vt->steps_len;
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
    if (argc < 4) {
        fprintf(stderr, "usage: %s <pid> <tid> <method-name-substring> [attach-timeout-s]\n", argv[0]);
        return 2;
    }
    int pid = atoi(argv[1]);
    int tid = atoi(argv[2]);
    const char *want = argv[3];
    int timeout_s = argc > 4 ? atoi(argv[4]) : 60;

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

    g_ch->magic = 0;
    g_pub_stop = 1;
    pthread_join(pt, NULL);
    g_ch->tracer_done = 1;

    size_t steps = asmtest_valtrace_steps(g_vt), nrecs = asmtest_valtrace_recs(g_vt);
    printf("GCCANON_CAPTURE rc=%d survived=%d steps=%zu recs=%zu truncated=%d result=0x%lx\n", prc,
           survived, steps, nrecs, (int)g_vt->truncated, result);

    if (prc == DF_PTRACE_ENOSYS || prc == DF_PTRACE_ETRACE) {
        printf("# SKIP: the scoped ptrace producer could not run here (rc=%d: no Capstone / "
               "ptrace refused)\n", prc);
        printf("1..0 # skipped\n");
        return 0;
    }

    /* ---- drain the stamped feed ------------------------------------------------------------ */
    uint32_t nm = g_ch->nmoves;
    if (nm > GCCANON_MAX_MOVES)
        nm = GCCANON_MAX_MOVES;
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

    printf("GCCANON_FEED gcs_seen=%u gcs_traced=%u recorded_moves=%u reloc_seen=%u nonreloc_seen=%u "
           "last_s0=%u distinct_gcs_in_feed=%u\n",
           g_ch->gcs_seen, g_ch->gcs_traced, nm, g_ch->moves_total, g_ch->nonreloc_total,
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

    int t = 0, fail = 0;
    printf("== gccanon-attach (F4 inc 1: live GC-move canonicalization on the ptrace attach tier) ==\n");

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

    /* 3 — the capture really spans the compaction. This is the assertion that stops the whole lane
     * being vacuous: the store and the load must be at DIFFERENT addresses (the object genuinely
     * relocated — a PINNED object would not have moved and both would match), and a live
     * MovedReferences2 range must EXPLAIN the difference exactly. */
    const asmtest_gcmove_t *expl = NULL;
    if (store && load) {
        for (uint32_t i = 0; i < nm; i++) {
            if (store_addr_old < moves[i].old_base ||
                store_addr_old - moves[i].old_base >= moves[i].len)
                continue;
            if (moves[i].new_base + (store_addr_old - moves[i].old_base) == load_addr_new) {
                expl = &moves[i];
                break;
            }
        }
    }
    if (store && load && store_addr_old != load_addr_new && expl && store_step < expl->step &&
        load_step >= expl->step) {
        printf("ok %d - the region spans the compaction: store@step=%u addr=0x%llx (OLD) -> "
               "load@step=%u addr=0x%llx (NEW), delta=%+lld, explained by a live moved range "
               "{old=0x%llx new=0x%llx len=%llu step=%u}\n",
               ++t, store_step, (unsigned long long)store_addr_old, load_step,
               (unsigned long long)load_addr_new,
               (long long)(load_addr_new - store_addr_old), (unsigned long long)expl->old_base,
               (unsigned long long)expl->new_base, (unsigned long long)expl->len, expl->step);
    } else {
        printf("not ok %d - the capture does not span a relocation: store=%s load=%s "
               "store_addr=0x%llx load_addr=0x%llx explained=%s — the fixture did not provoke the "
               "bug, so nothing below would mean anything\n",
               ++t, store ? "found" : "MISSING", load ? "found" : "MISSING",
               (unsigned long long)store_addr_old, (unsigned long long)load_addr_new,
               expl ? "yes" : "no");
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

    /* 5 — THE POSITIVE. Stamped triples -> asmtest_gcmove_canonicalize -> asmtest_defuse_build. */
    size_t changed = asmtest_gcmove_canonicalize(g_vt, moves, nm);
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
        printf("ok %d - WITH canonicalization (%zu record addresses remapped by %u stamped range(s)) "
               "the def-use edge store(step %u) -> load(step %u) APPEARS at the canonical address "
               "0x%llx: a managed value's MEMORY def-use SURVIVED an induced GC compaction on a "
               "LIVE ATTACH\n",
               ++t, changed, nm, store_step, load_step, (unsigned long long)key);
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

    /* 6 — no FALSE edge. The other half of what the transform is for: an object that later occupies
     * the VACATED old address must not be forwarded onto the moved object. Checked directly against
     * the LIVE move set: a pre-move access at the old address forwards to the new one, while a
     * post-move access at the same address stays put. */
    if (store && load && expl) {
        uint64_t vacated = store_addr_old; /* the OLD address, snapshotted pre-transform */
        uint64_t pre = asmtest_gcmove_canon(moves, nm, store_step, vacated);
        uint64_t post = asmtest_gcmove_canon(moves, nm, load_step, vacated);
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

    /* 7 — the model's own precondition. The step counter is region-gated, so it is FROZEN across the
     * call-out where the GC lands: every GC in one window is stamped with the same S0, and canon
     * applies at most ONE relocation per step-group (a group models ONE disjoint batch). Two GCs
     * sharing a step would silently collapse. The fixture choreographs exactly one; this CHECKS it
     * rather than trusting it. */
    int collapsed = 0;
    for (uint32_t i = 1; i < nm; i++)
        if (moves[i].step == moves[i - 1].step && g_ch->moves[i].gc_seq != g_ch->moves[i - 1].gc_seq)
            collapsed = 1;
    if (!collapsed) {
        printf("ok %d - the batch model holds: the %u stamped range(s) came from %u GC(s), and no "
               "two distinct GCs share a step boundary (a shared step would collapse two batches "
               "into one and silently under-forward a twice-moved object)\n",
               ++t, nm, distinct_gcs);
    } else {
        printf("not ok %d - TWO DISTINCT GCs share one step boundary: the region-gated counter did "
               "not separate them, so canon would apply only ONE of their relocations. This is a "
               "real limitation of S0 stamping over a frozen counter, not a fixture glitch\n", ++t);
        fail = 1;
    }

    printf("1..%d\n", t);
    printf("GCCANON_SUMMARY rc=%d survived=%d steps=%zu moves=%u gcs_traced=%u remapped=%zu "
           "raw_edge=%d canon_edge=%d fail=%d\n",
           prc, survived, steps, nm, g_ch->gcs_traced, changed, raw_edge, canon_edge, fail);

    asmtest_defuse_free(raw);
    asmtest_defuse_free(canon);
    asmtest_valtrace_free(g_vt);
    free(moves);
    (void)sleep_ms;
    return fail ? 1 : 0;
}
