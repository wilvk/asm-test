/*
 * asmspy.c — a small ncurses front-end over the asm-test out-of-process tracer.
 *
 * Pick a running process, then watch either its live syscalls (with data) or a
 * live disassembly + call-graph of a chosen function — all out of band.
 *
 * Interactive:   asmspy
 * Headless (scriptable / CI smoke), sharing the same engine:
 *   asmspy --list [active|scan]       list attachable processes
 *   asmspy --syms   <pid> [filter]    list resolved function symbols
 *   asmspy --log    <pid> [n]         stream n syscalls with data (a mini strace)
 *   asmspy --trace  <pid> <sym> [n]   n live samples: disassembly + functions called
 *   asmspy --stream <pid> [n]         stream n instructions live (function + asm)
 *   asmspy --graph  <pid> [n] [--sort=invocations|fanout]  whole-process call graph
 *   asmspy --tree   <pid> [n]         whole-process live call tree (indented by depth)
 *   asmspy --procs  <pid> [n] [--count=syscalls|calls]  process/thread topology tree
 *
 * A negative n streams until the target exits or you interrupt.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <ncurses.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "asmspy.h"
#include "asmspy_logview.h"

/* ================================================================== */
/* Shared rendering: one captured region sample -> header + 2 panes    */
/* ================================================================== */

typedef struct {
    char **v;
    size_t n, cap;
} svec;

static void svec_push(svec *s, const char *line) {
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 64;
        char **nv = realloc(s->v, nc * sizeof *nv);
        if (!nv)
            return;
        s->v = nv;
        s->cap = nc;
    }
    s->v[s->n] = strdup(line);
    if (s->v[s->n])
        s->n++;
}
static void svec_clear(svec *s) {
    for (size_t i = 0; i < s->n; i++)
        free(s->v[i]);
    s->n = 0;
}
static void svec_free(svec *s) {
    svec_clear(s);
    free(s->v);
    s->v = NULL;
    s->cap = 0;
}

static int u64cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

#define MAX_DISASM 512 /* cap displayed distinct instructions           */
#define MAX_EDGES 256  /* cap displayed distinct call edges             */

/* an aggregated call edge: a callee, a representative call-site, and how many
 * times it was called (so the functions pane can rank most-active first) */
typedef struct {
    uint64_t tgt, site;
    unsigned count;
} edge_agg_t;

static int edge_agg_cmp(const void *a, const void *b) {
    const edge_agg_t *x = a, *y = b;
    if (x->count != y->count)
        return x->count < y->count ? 1 : -1; /* count descending */
    return x->site < y->site ? -1 : x->site > y->site ? 1 : 0;
}

/* Fill `header`, `dis` (assembly), `fn` (functions called) for one sample. */
static void region_render(char *header, size_t hcap, svec *dis, svec *fn,
                          unsigned sample, long result,
                          const asmtest_trace_t *tr,
                          const asmtest_descent_t *desc, const uint8_t *code,
                          size_t len, uint64_t base,
                          const asmspy_symtab_t *syms) {
    unsigned long long total = asmtest_emu_trace_insns_total(tr);
    unsigned long long recorded = asmtest_emu_trace_insns_len(tr);
    unsigned long long blocks = asmtest_emu_trace_blocks_len(tr);
    snprintf(header, hcap,
             "sample #%u   ret=%ld   %llu insns (%llu executed), %llu blocks%s",
             sample, result, recorded, total, blocks,
             asmtest_emu_trace_truncated(tr) ? "  (truncated)" : "");

    /* distinct instruction offsets (ascending) with per-offset execution counts,
     * so hot instructions (a loop body runs N×) are annotated in place */
    size_t ni = (size_t)recorded;
    uint64_t *offs = ni ? malloc(ni * sizeof *offs) : NULL;
    uint32_t *cnts = ni ? malloc(ni * sizeof *cnts) : NULL;
    size_t nd = 0;
    if (offs && cnts) {
        for (size_t i = 0; i < ni; i++)
            offs[i] = asmtest_emu_trace_insn_at(tr, i);
        qsort(offs, ni, sizeof *offs, u64cmp);
        for (size_t i = 0; i < ni; i++) {
            if (i == 0 || offs[i] != offs[i - 1]) {
                offs[nd] = offs[i];
                cnts[nd] = 1;
                nd++;
            } else {
                cnts[nd - 1]++;
            }
        }
    }
    for (size_t i = 0; i < nd && i < MAX_DISASM; i++) {
        char line[200], dbuf[160] = "";
        if (code && asmtest_disas_available())
            asmtest_disas(ASMTEST_ARCH_X86_64, code, len, base, offs[i], dbuf,
                          sizeof dbuf);
        if (dbuf[0])
            snprintf(line, sizeof line, "%4u×  +0x%-4llx  %s", cnts[i],
                     (unsigned long long)offs[i], dbuf);
        else
            snprintf(line, sizeof line, "%4u×  +0x%llx", cnts[i],
                     (unsigned long long)offs[i]);
        svec_push(dis, line);
    }
    if (nd > MAX_DISASM) {
        char more[64];
        snprintf(more, sizeof more, "... (+%zu more)", nd - MAX_DISASM);
        svec_push(dis, more);
    }
    free(offs);
    free(cnts);

    /* call edges aggregated by callee, ranked most-active (most calls) first */
    size_t ne = asmtest_descent_edges_len(desc);
    edge_agg_t agg[MAX_EDGES];
    size_t nagg = 0;
    for (size_t i = 0; i < ne; i++) {
        uint64_t tgt = asmtest_descent_edge_target(desc, i);
        uint64_t site = asmtest_descent_edge_site(desc, i);
        size_t k;
        for (k = 0; k < nagg; k++)
            if (agg[k].tgt == tgt)
                break;
        if (k < nagg) {
            agg[k].count++;
            if (site < agg[k].site) /* keep the lowest site as representative */
                agg[k].site = site;
        } else if (nagg < MAX_EDGES) {
            agg[nagg].tgt = tgt;
            agg[nagg].site = site;
            agg[nagg].count = 1;
            nagg++;
        }
    }
    qsort(agg, nagg, sizeof agg[0], edge_agg_cmp);
    for (size_t i = 0; i < nagg; i++) {
        char line[220];
        const asmspy_sym_t *s = syms ? asmspy_symtab_at(syms, agg[i].tgt) : NULL;
        if (s) {
            uint64_t delta = agg[i].tgt - s->addr;
            if (delta)
                snprintf(line, sizeof line,
                         "%4u×  +0x%-4llx  ->  %s+0x%llx  [%s]", agg[i].count,
                         (unsigned long long)agg[i].site, s->name,
                         (unsigned long long)delta, s->module);
            else
                snprintf(line, sizeof line, "%4u×  +0x%-4llx  ->  %s  [%s]",
                         agg[i].count, (unsigned long long)agg[i].site, s->name,
                         s->module);
        } else {
            snprintf(line, sizeof line, "%4u×  +0x%-4llx  ->  0x%llx",
                     agg[i].count, (unsigned long long)agg[i].site,
                     (unsigned long long)agg[i].tgt);
        }
        svec_push(fn, line);
    }
    if (asmtest_descent_truncated(desc))
        svec_push(fn, "(call record truncated)");
}

/* ================================================================== */
/* Shared call-graph view: sort + row format (used by headless + TUI)  */
/* ================================================================== */

/* How to rank the whole-process call graph. */
typedef enum {
    GSORT_INVOCATIONS = 0, /* most-called functions first                 */
    GSORT_FANOUT = 1,      /* most distinct callees (functions called) first */
} gsort_t;

/* A retained copy of an engine snapshot (the engine's array is transient). */
typedef struct {
    asmspy_gnode_t *v;
    size_t n, cap;
} graph_snap;

/* Copy `nodes[0..n)` into the snapshot, growing it as needed (replace, not
 * append — each engine snapshot is the whole graph so far). */
static void graph_snap_copy(graph_snap *s, const asmspy_gnode_t *nodes,
                            size_t n) {
    if (n > s->cap) {
        asmspy_gnode_t *nv = realloc(s->v, n * sizeof *nv);
        if (!nv)
            return; /* keep the previous snapshot on OOM */
        s->v = nv;
        s->cap = n;
    }
    if (n)
        memcpy(s->v, nodes, n * sizeof *nodes);
    s->n = n;
}

/* qsort comparator; the active key is set through this file-scope selector right
 * before each qsort (both callers are single-threaded at the sort point). */
static gsort_t graph_sort_key = GSORT_INVOCATIONS;
static int gnode_cmp(const void *a, const void *b) {
    const asmspy_gnode_t *x = a, *y = b;
    unsigned long long kx = graph_sort_key == GSORT_FANOUT ? x->fanout
                                                           : x->invocations;
    unsigned long long ky = graph_sort_key == GSORT_FANOUT ? y->fanout
                                                           : y->invocations;
    if (kx != ky)
        return kx < ky ? 1 : -1; /* descending */
    /* tie-break on the OTHER metric, then name, so the order is stable */
    unsigned long long tx = graph_sort_key == GSORT_FANOUT ? x->invocations
                                                           : x->fanout;
    unsigned long long ty = graph_sort_key == GSORT_FANOUT ? y->invocations
                                                           : y->fanout;
    if (tx != ty)
        return tx < ty ? 1 : -1;
    return strcmp(x->name, y->name);
}

/* One call-graph row: an internal/external marker, the function, its counts
 * (times called / calls made / distinct callees), and the backing module.
 * `[?]` marks an address no symbol resolved (JIT, stripped, anonymous). */
static void graph_format_row(char *buf, size_t cap, const asmspy_gnode_t *nd) {
    const char *tag = strcmp(nd->module, "?") == 0 ? "[?]"
                      : nd->external               ? "[EXT]"
                                                   : "[int]";
    snprintf(buf, cap, "%-5s %-30.30s inv=%-7llu calls=%-7llu fanout=%-5u [%s]",
             tag, nd->name, nd->invocations, nd->out_calls, nd->fanout,
             nd->module);
}

/* ================================================================== */
/* Shared process/thread topology view (used by headless + TUI)        */
/* ================================================================== */

/* A retained copy of a topology snapshot (the engine's array is transient). */
typedef struct {
    asmspy_task_t *v;
    size_t n, cap;
} topo_snap;

static void topo_snap_copy(topo_snap *s, const asmspy_task_t *tasks, size_t n) {
    if (n > s->cap) {
        asmspy_task_t *nv = realloc(s->v, n * sizeof *nv);
        if (!nv)
            return;
        s->v = nv;
        s->cap = n;
    }
    if (n)
        memcpy(s->v, tasks, n * sizeof *tasks);
    s->n = n;
}

/* One rendered topology row (a process header or a thread), with the ids it maps
 * to so the TUI can select it and drill in. */
typedef struct {
    char text[176];
    pid_t tid, tgid;
    int is_process;
    unsigned long long inv;
} topo_row_t;

typedef struct {
    topo_row_t *v;
    size_t n, cap;
} topo_rows;

static void trows_push(topo_rows *r, const topo_row_t *row) {
    if (r->n == r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 64;
        topo_row_t *nv = realloc(r->v, nc * sizeof *nv);
        if (!nv)
            return;
        r->v = nv;
        r->cap = nc;
    }
    r->v[r->n++] = *row;
}

/* Aggregate of one process (tasks sharing a tgid) for the topology forest. */
typedef struct {
    pid_t tgid, ppid;
    char exe[64];
    unsigned long long inv; /* summed over the process's tasks */
    size_t *task;           /* indices into the task snapshot   */
    size_t ntask, taskcap;
    int emitted;            /* forest guard: render each process once */
} proc_agg;

/* Sort helper: process-aggregate indices by summed inv (desc), then pid. */
static const proc_agg *g_procs_for_cmp;
static int proc_idx_cmp(const void *a, const void *b) {
    const proc_agg *x = &g_procs_for_cmp[*(const size_t *)a];
    const proc_agg *y = &g_procs_for_cmp[*(const size_t *)b];
    if (x->inv != y->inv)
        return x->inv < y->inv ? 1 : -1;
    return x->tgid < y->tgid ? -1 : x->tgid > y->tgid ? 1 : 0;
}

/* Task-index sort by inv (desc), leader first. Uses g_tasks_for_cmp. */
static const asmspy_task_t *g_tasks_for_cmp;
static int task_idx_cmp(const void *a, const void *b) {
    const asmspy_task_t *x = &g_tasks_for_cmp[*(const size_t *)a];
    const asmspy_task_t *y = &g_tasks_for_cmp[*(const size_t *)b];
    if (x->is_leader != y->is_leader)
        return x->is_leader ? -1 : 1; /* leader on top */
    if (x->inv != y->inv)
        return x->inv < y->inv ? 1 : -1;
    return x->tid < y->tid ? -1 : x->tid > y->tid ? 1 : 0;
}

static void topo_emit_proc(proc_agg *P, size_t np, const asmspy_task_t *tasks,
                           size_t k, int depth, topo_rows *out) {
    if (P[k].emitted)
        return; /* cycle / already-rendered guard */
    P[k].emitted = 1;
    int ind = depth * 2;
    if (ind > 40)
        ind = 40;
    topo_row_t row;
    memset(&row, 0, sizeof row);
    snprintf(row.text, sizeof row.text, "%*snode %d%s%s%s  inv=%llu", ind, "",
             (int)P[k].tgid, P[k].exe[0] ? " [" : "", P[k].exe,
             P[k].exe[0] ? "]" : "", P[k].inv);
    row.tgid = row.tid = P[k].tgid;
    row.is_process = 1;
    row.inv = P[k].inv;
    trows_push(out, &row);

    /* threads (only when the process has more than its lone leader) */
    if (P[k].ntask > 1) {
        size_t *ti = malloc(P[k].ntask * sizeof *ti);
        if (ti) {
            for (size_t j = 0; j < P[k].ntask; j++)
                ti[j] = P[k].task[j];
            g_tasks_for_cmp = tasks;
            qsort(ti, P[k].ntask, sizeof *ti, task_idx_cmp);
            for (size_t j = 0; j < P[k].ntask; j++) {
                const asmspy_task_t *t = &tasks[ti[j]];
                memset(&row, 0, sizeof row);
                snprintf(row.text, sizeof row.text, "%*stid %d%s%s%s  inv=%llu",
                         ind + 2, "", (int)t->tid, t->comm[0] ? " (" : "",
                         t->comm, t->comm[0] ? ")" : "", t->inv);
                row.tid = t->tid;
                row.tgid = t->tgid;
                row.is_process = 0;
                row.inv = t->inv;
                trows_push(out, &row);
            }
            free(ti);
        }
    }

    /* child processes: those whose parent is this process, by inv desc */
    size_t *ci = malloc(np * sizeof *ci);
    size_t nc = 0;
    if (ci) {
        for (size_t j = 0; j < np; j++)
            if (j != k && !P[j].emitted && P[j].ppid == P[k].tgid)
                ci[nc++] = j;
        g_procs_for_cmp = P;
        qsort(ci, nc, sizeof *ci, proc_idx_cmp);
        for (size_t j = 0; j < nc; j++)
            topo_emit_proc(P, np, tasks, ci[j], depth + 1, out);
        free(ci);
    }
}

/* Turn a task snapshot into an indented process/thread forest. Roots are the
 * processes whose parent is not itself tracked (typically just the target). */
static void topo_build_rows(const asmspy_task_t *tasks, size_t n,
                            topo_rows *out) {
    proc_agg *P = NULL;
    size_t np = 0, pcap = 0;
    for (size_t i = 0; i < n; i++) {
        pid_t g = tasks[i].tgid;
        size_t k;
        for (k = 0; k < np; k++)
            if (P[k].tgid == g)
                break;
        if (k == np) {
            if (np == pcap) {
                size_t ncp = pcap ? pcap * 2 : 16;
                proc_agg *nv = realloc(P, ncp * sizeof *nv);
                if (!nv)
                    goto done;
                P = nv;
                pcap = ncp;
            }
            memset(&P[np], 0, sizeof P[np]);
            P[np].tgid = g;
            P[np].ppid = tasks[i].ppid;
            np++;
        }
        proc_agg *pa = &P[k == np ? np - 1 : k];
        if (pa->ntask == pa->taskcap) {
            size_t nct = pa->taskcap ? pa->taskcap * 2 : 8;
            size_t *nv = realloc(pa->task, nct * sizeof *nv);
            if (nv) {
                pa->task = nv;
                pa->taskcap = nct;
            }
        }
        if (pa->ntask < pa->taskcap)
            pa->task[pa->ntask++] = i;
        pa->inv += tasks[i].inv;
        if (tasks[i].is_leader) {
            pa->ppid = tasks[i].ppid;
            snprintf(pa->exe, sizeof pa->exe, "%s", tasks[i].exe);
        }
    }

    /* emit roots (parent not tracked) first, by inv desc */
    size_t *roots = malloc((np ? np : 1) * sizeof *roots);
    size_t nr = 0;
    if (roots) {
        for (size_t k = 0; k < np; k++) {
            int parent_tracked = 0;
            for (size_t j = 0; j < np; j++)
                if (P[j].tgid == P[k].ppid) {
                    parent_tracked = 1;
                    break;
                }
            if (!parent_tracked)
                roots[nr++] = k;
        }
        g_procs_for_cmp = P;
        qsort(roots, nr, sizeof *roots, proc_idx_cmp);
        for (size_t j = 0; j < nr; j++)
            topo_emit_proc(P, np, tasks, roots[j], 0, out);
        /* any process not reached (orphaned parent chain) — emit at depth 0 */
        for (size_t k = 0; k < np; k++)
            if (!P[k].emitted)
                topo_emit_proc(P, np, tasks, k, 0, out);
        free(roots);
    }

done:
    for (size_t k = 0; k < np; k++)
        free(P[k].task);
    free(P);
}

/* ================================================================== */
/* Headless subcommands                                                */
/* ================================================================== */

static int cmd_list(asmspy_sort_t sort) {
    asmspy_proc_t *v = NULL;
    size_t n = 0;
    if (asmspy_proclist(&v, &n, sort) < 0) {
        fprintf(stderr, "cannot read /proc\n");
        return 1;
    }
    if (sort == ASMSPY_SORT_SCAN) {
        printf("%-8s %-5s %-8s %-12s %-4s %s\n", "PID", "STR", "CPU", "USER",
               "ATT", "COMMAND");
        for (size_t i = 0; i < n; i++)
            printf("%-8d %-5u %-8llu %-12.12s %-4s %.64s\n", v[i].pid, v[i].scan,
                   v[i].cpu, v[i].user, v[i].attachable ? "yes" : "-", v[i].cmd);
    } else if (sort == ASMSPY_SORT_ACTIVE) {
        printf("%-8s %-8s %-12s %-4s %s\n", "PID", "CPU", "USER", "ATT",
               "COMMAND");
        for (size_t i = 0; i < n; i++)
            printf("%-8d %-8llu %-12.12s %-4s %.72s\n", v[i].pid, v[i].cpu,
                   v[i].user, v[i].attachable ? "yes" : "-", v[i].cmd);
    } else {
        printf("%-8s %-12s %-4s %s\n", "PID", "USER", "ATT", "COMMAND");
        for (size_t i = 0; i < n; i++)
            printf("%-8d %-12.12s %-4s %.80s\n", v[i].pid, v[i].user,
                   v[i].attachable ? "yes" : "-", v[i].cmd);
    }
    free(v);
    return 0;
}

static int cmd_syms(pid_t pid, const char *filter) {
    asmspy_symtab_t t;
    if (asmspy_symtab_load(pid, &t) < 0) {
        fprintf(stderr, "cannot read symbols for pid %d\n", (int)pid);
        return 1;
    }
    size_t shown = 0;
    for (size_t i = 0; i < t.n; i++) {
        if (filter && !strstr(t.v[i].name, filter))
            continue;
        printf("0x%012llx  %6llu  %-40s [%s]\n",
               (unsigned long long)t.v[i].addr,
               (unsigned long long)t.v[i].size, t.v[i].name, t.v[i].module);
        shown++;
    }
    fprintf(stderr, "%zu function symbols%s\n", shown,
            filter ? " (filtered)" : "");
    asmspy_symtab_free(&t);
    return 0;
}

static void log_print_sink(void *ctx, const char *line, const char *str) {
    (void)ctx;
    (void)str; /* the full line already embeds the decoded string */
    printf("%s\n", line);
    fflush(stdout);
}

static int cmd_log(pid_t pid, long n) {
    int rc = asmspy_engine_syscalls(pid, n, NULL, log_print_sink, NULL);
    if (rc != 0) {
        char e[128];
        asmspy_strerror(rc, e, sizeof e);
        fprintf(stderr, "attach failed: %s\n", e);
        return 1;
    }
    return 0;
}

static void stream_print_sink(void *ctx, const char *line) {
    (void)ctx;
    printf("%s\n", line);
    fflush(stdout);
}

static int cmd_stream(pid_t pid, long n) {
    asmspy_symtab_t t;
    asmspy_symtab_load(pid, &t); /* best-effort; raw addresses if empty */
    int rc = asmspy_engine_stream(pid, n, NULL, &t, stream_print_sink, NULL);
    asmspy_symtab_free(&t);
    if (rc != 0) {
        char e[128];
        asmspy_strerror(rc, e, sizeof e);
        fprintf(stderr, "attach failed: %s\n", e);
        return 1;
    }
    return 0;
}

static int cmd_tree(pid_t pid, long n) {
    asmspy_symtab_t t;
    asmspy_symtab_load(pid, &t); /* best-effort; raw addrs if empty */
    int rc = asmspy_engine_tree(pid, n, NULL, &t, stream_print_sink, NULL);
    asmspy_symtab_free(&t);
    if (rc != 0) {
        char e[128];
        asmspy_strerror(rc, e, sizeof e);
        fprintf(stderr, "attach failed: %s\n", e);
        return 1;
    }
    return 0;
}

static void graph_capture_sink(void *ctx, const asmspy_gnode_t *nodes,
                               size_t n) {
    graph_snap_copy(ctx, nodes, n); /* headless: keep only the latest snapshot */
}

static int cmd_graph(pid_t pid, long n, gsort_t sort) {
    asmspy_symtab_t t;
    asmspy_symtab_load(pid, &t); /* best-effort; raw addrs (all internal) if empty */
    graph_snap snap = {0};
    int rc = asmspy_engine_graph(pid, n, NULL, &t, graph_capture_sink, &snap);
    asmspy_symtab_free(&t);
    if (rc != 0) {
        char e[128];
        asmspy_strerror(rc, e, sizeof e);
        fprintf(stderr, "attach failed: %s\n", e);
        free(snap.v);
        return 1;
    }
    graph_sort_key = sort;
    qsort(snap.v, snap.n, sizeof *snap.v, gnode_cmp);
    printf("call graph — %zu functions, sorted by %s (pid %d)\n", snap.n,
           sort == GSORT_FANOUT ? "functions called (fanout)" : "invocations",
           (int)pid);
    if (snap.n == 0)
        printf("(no calls observed — target idle, or single-stepping saw no "
               "call in the window)\n");
    for (size_t i = 0; i < snap.n; i++) {
        char row[256];
        graph_format_row(row, sizeof row, &snap.v[i]);
        printf("%s\n", row);
    }
    free(snap.v);
    return 0;
}

static void topo_capture_sink(void *ctx, const asmspy_task_t *tasks, size_t n) {
    topo_snap_copy(ctx, tasks, n); /* headless: keep only the latest snapshot */
}

static int cmd_procs(pid_t pid, long n, asmspy_count_t mode) {
    topo_snap snap = {0};
    int rc = asmspy_engine_procs(pid, n, NULL, mode, topo_capture_sink, &snap);
    if (rc != 0) {
        char e[128];
        asmspy_strerror(rc, e, sizeof e);
        fprintf(stderr, "attach failed: %s\n", e);
        free(snap.v);
        return 1;
    }
    topo_rows rows = {0};
    topo_build_rows(snap.v, snap.n, &rows);
    printf("process/thread topology — %zu tasks, counting %s (pid %d)\n", snap.n,
           mode == ASMSPY_COUNT_CALLS ? "calls" : "syscalls", (int)pid);
    if (snap.n == 0)
        printf("(no tasks observed — target idle or gone)\n");
    for (size_t i = 0; i < rows.n; i++)
        printf("%s\n", rows.v[i].text);
    free(rows.v);
    free(snap.v);
    return 0;
}

static void region_print_sink(void *ctx, unsigned sample_no, long result,
                              const asmtest_trace_t *tr,
                              const asmtest_descent_t *desc,
                              const uint8_t *code, size_t len, uint64_t base) {
    const asmspy_symtab_t *syms = ctx;
    char header[256];
    svec dis = {0}, fn = {0};
    region_render(header, sizeof header, &dis, &fn, sample_no, result, tr, desc,
                  code, len, base, syms);
    printf("\n%s\n  assembly:\n", header);
    for (size_t i = 0; i < dis.n; i++)
        printf("    %s\n", dis.v[i]);
    printf("  functions called:\n");
    if (fn.n == 0)
        printf("    (leaf — no calls)\n");
    for (size_t i = 0; i < fn.n; i++)
        printf("    %s\n", fn.v[i]);
    fflush(stdout);
    svec_free(&dis);
    svec_free(&fn);
}

/* Resolve a region argument to (base,len). Four forms:
 *     <name>           a sized function symbol, looked up by name
 *     0x<addr>         an address a sized symbol covers — uses that symbol's extent
 *     0x<addr>:<len>   an EXPLICIT range: len bytes from addr
 *     0x<addr>+<len>   the same, alternate separator
 * The explicit-length forms need no symbol at all, so they reach stripped code,
 * a PLT stub, or a JIT region no STT_FUNC covers — the address can come from
 * `--syms`, a map, or a disassembler. len is parsed base-0 (so 0x.. or decimal).
 * The engine rejects len==0 and len over its cap, so we needn't re-check here. */
static int resolve_region(const asmspy_symtab_t *t, const char *arg,
                          uint64_t *base, size_t *len) {
    if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
        char *end = NULL;
        uint64_t a = strtoull(arg, &end, 16);
        if (end == arg + 2) /* "0x" with no digits */
            return -1;
        if (*end == ':' || *end == '+') {
            char *lend = NULL;
            unsigned long long l = strtoull(end + 1, &lend, 0);
            if (lend == end + 1 || *lend != '\0' || l == 0)
                return -1;
            *base = a;
            *len = (size_t)l;
            return 0;
        }
        if (*end != '\0') /* trailing garbage after the address */
            return -1;
        const asmspy_sym_t *s = asmspy_symtab_at(t, a);
        if (s && s->size) {
            *base = s->addr;
            *len = s->size;
            return 0;
        }
        return -1;
    }
    const asmspy_sym_t *s = asmspy_symtab_by_name(t, arg);
    if (!s || !s->size)
        return -1;
    *base = s->addr;
    *len = s->size;
    return 0;
}

static int cmd_trace(pid_t pid, const char *sym, long n) {
    asmspy_symtab_t t;
    if (asmspy_symtab_load(pid, &t) < 0) {
        fprintf(stderr, "cannot read symbols for pid %d\n", (int)pid);
        return 1;
    }
    uint64_t base = 0;
    size_t len = 0;
    if (resolve_region(&t, sym, &base, &len) != 0) {
        fprintf(stderr,
                "cannot resolve '%s' to a region in pid %d\n"
                "  want a sized function name, 0xADDR inside one, "
                "or an explicit 0xADDR:LEN\n",
                sym, (int)pid);
        asmspy_symtab_free(&t);
        return 1;
    }
    fprintf(stderr, "tracing %s @ 0x%llx (%zu bytes) in pid %d\n", sym,
            (unsigned long long)base, len, (int)pid);
    int rc = asmspy_engine_region(pid, base, len, n, NULL, region_print_sink,
                                  &t);
    if (rc == ASMSPY_REGION_NEVER_RAN) {
        fprintf(stderr,
                "%s never executed while traced in pid %d\n"
                "  --trace follows only the main thread; if the target is "
                "multi-threaded the\n"
                "  function may run on a worker thread (--stream follows all "
                "threads)\n",
                sym, (int)pid);
    } else if (rc != 0) {
        char e[128];
        asmspy_strerror(rc, e, sizeof e);
        fprintf(stderr, "trace failed: %s\n", e);
    }
    asmspy_symtab_free(&t);
    return rc != 0;
}

/* ================================================================== */
/* Interactive TUI                                                     */
/* ================================================================== */

#define LOG_CAP 2048
/* Named to avoid <limits.h>'s LINE_MAX. 512 holds a full decoded syscall line
 * (the engine formats up to ~1 KB but nothing wider than a real terminal is
 * ever displayed) plus the "[tid] " prefix, so lines are no longer clipped
 * short of the pane width the way the old 320 could be on a wide terminal. */
#define LOG_LINE_MAX 512

typedef struct {
    char lines[LOG_CAP][LOG_LINE_MAX];
    int head, count;
    unsigned long total; /* lines ever pushed (monotonic); drives scrollback */
} logbuf;

static void log_push(logbuf *lg, const char *s) {
    snprintf(lg->lines[lg->head], LOG_LINE_MAX, "%s", s);
    lg->head = (lg->head + 1) % LOG_CAP;
    if (lg->count < LOG_CAP)
        lg->count++;
    lg->total++;
}

/* Render the ring buffer into the (y0,x0) w×h box with `bottom` (an absolute
 * line index) as the last visible line; asmspy_log_window does the clamped
 * viewport math (unit-tested in test_logview.c). A `bottom` past the newest
 * line is clamped, so log_newest() tails the live stream. */
static void draw_log_at(const logbuf *lg, int y0, int x0, int h, int w,
                        long bottom) {
    if (h < 1 || w < 1)
        return;
    long top = 0;
    int n = asmspy_log_window(lg->total, lg->count, bottom, h, &top);
    for (int r = 0; r < n; r++) {
        long abs = top + r;
        int slot = (int)((unsigned long)abs % LOG_CAP);
        mvprintw(y0 + r, x0, "%-*.*s", w, w, lg->lines[slot]);
    }
}

/* absolute index of the newest buffered line (-1 if empty) */
static long log_newest(const logbuf *lg) { return (long)lg->total - 1; }

/* filterable scrolling list over borrowed display strings */
typedef struct {
    char **items;
    int n;
    int *match;
    int nmatch;
    int top, sel;
    char filter[64];
    int flen;
} List;

static void list_refilter(List *L) {
    L->nmatch = 0;
    for (int i = 0; i < L->n; i++) {
        const char *it = L->items[i]; /* may be NULL if a strdup OOM'd */
        if (L->flen == 0 || (it && strcasestr(it, L->filter)))
            L->match[L->nmatch++] = i;
    }
    if (L->sel >= L->nmatch)
        L->sel = L->nmatch ? L->nmatch - 1 : 0;
    if (L->top > L->sel)
        L->top = L->sel;
}
static int list_init(List *L, char **items, int n) {
    L->items = items;
    L->n = n;
    L->nmatch = L->top = L->sel = L->flen = 0; /* init BEFORE the fallible alloc */
    L->filter[0] = '\0';
    L->match = malloc((n ? n : 1) * sizeof(int));
    if (!L->match)
        return -1;
    list_refilter(L);
    return 0;
}
static void list_done(List *L) { free(L->match); }

static void list_render(const List *L, int y0, int h, int w) {
    attron(A_BOLD);
    mvprintw(y0, 0, "/%s", L->filter);
    clrtoeol();
    attroff(A_BOLD);
    for (int r = 0; r < h; r++) {
        int m = L->top + r;
        move(y0 + 1 + r, 0);
        clrtoeol();
        if (m >= L->nmatch)
            continue;
        int cur = (m == L->sel);
        const char *s = L->items[L->match[m]];
        if (cur)
            attron(A_REVERSE);
        mvprintw(y0 + 1 + r, 0, "%-*.*s", w, w, s ? s : "");
        if (cur)
            attroff(A_REVERSE);
    }
}
/* returns selected real index on Enter, -2 on quit, -1 otherwise */
static int list_key(List *L, int ch, int h) {
    if (h < 1)
        h = 1; /* a tiny terminal (rows<=3) must not make paging math negative */
    switch (ch) {
    case KEY_UP:
        if (L->sel > 0)
            L->sel--;
        break;
    case KEY_DOWN:
        if (L->sel < L->nmatch - 1)
            L->sel++;
        break;
    case KEY_PPAGE:
        L->sel -= h;
        if (L->sel < 0)
            L->sel = 0;
        break;
    case KEY_NPAGE:
        L->sel += h;
        if (L->sel > L->nmatch - 1)
            L->sel = L->nmatch - 1;
        break;
    case KEY_BACKSPACE:
    case 127:
    case 8:
        if (L->flen > 0) {
            L->filter[--L->flen] = '\0';
            list_refilter(L);
        }
        break;
    case '\n':
    case KEY_ENTER:
        return (L->nmatch && L->sel >= 0 && L->sel < L->nmatch)
                   ? L->match[L->sel]
                   : -1;
    default:
        if (ch >= 32 && ch < 127 && L->flen < (int)sizeof L->filter - 1) {
            L->filter[L->flen++] = (char)ch;
            L->filter[L->flen] = '\0';
            list_refilter(L);
        }
    }
    /* clamp sel into range even after paging on a tiny/empty list */
    if (L->sel < 0)
        L->sel = 0;
    if (L->sel > L->nmatch - 1)
        L->sel = L->nmatch ? L->nmatch - 1 : 0;
    if (L->sel < L->top)
        L->top = L->sel;
    if (L->sel >= L->top + h)
        L->top = L->sel - h + 1;
    if (L->top < 0)
        L->top = 0;
    return -1;
}

/* --- live view: shared state between UI thread and tracer thread --- */
typedef struct {
    pthread_mutex_t mu;
    atomic_bool stop;
    atomic_bool finished;
    int rc; /* engine return code (set before finished) */
    /* syscall log (left pane) + the decoded strings it carried (right pane) */
    logbuf log;
    logbuf strlog;
    /* region view */
    char asm_header[256];
    svec asm_dis, asm_fn;
    unsigned asm_sample;
    const asmspy_symtab_t *syms;
    /* call-graph view (mode 3) */
    graph_snap graph;
    gsort_t gsort;
    /* target */
    pid_t pid;
    uint64_t base;
    size_t len;
    int mode; /* 0 syscalls, 1 region, 2 instruction stream, 3 call graph */
} live_t;

static void live_syscall_sink(void *ctx, const char *line, const char *str) {
    live_t *L = ctx;
    pthread_mutex_lock(&L->mu);
    log_push(&L->log, line);
    if (str && *str)
        log_push(&L->strlog, str); /* the decoded-strings pane */
    pthread_mutex_unlock(&L->mu);
}
static void live_region_sink(void *ctx, unsigned sample_no, long result,
                             const asmtest_trace_t *tr,
                             const asmtest_descent_t *desc, const uint8_t *code,
                             size_t len, uint64_t base) {
    live_t *L = ctx;
    pthread_mutex_lock(&L->mu);
    svec_clear(&L->asm_dis);
    svec_clear(&L->asm_fn);
    region_render(L->asm_header, sizeof L->asm_header, &L->asm_dis, &L->asm_fn,
                  sample_no, result, tr, desc, code, len, base, L->syms);
    L->asm_sample = sample_no;
    pthread_mutex_unlock(&L->mu);
}

static void live_stream_sink(void *ctx, const char *line) {
    live_t *L = ctx;
    pthread_mutex_lock(&L->mu);
    log_push(&L->log, line);
    pthread_mutex_unlock(&L->mu);
}

static void live_graph_sink(void *ctx, const asmspy_gnode_t *nodes, size_t n) {
    live_t *L = ctx;
    pthread_mutex_lock(&L->mu);
    graph_snap_copy(&L->graph, nodes, n);
    pthread_mutex_unlock(&L->mu);
}

static void *tracer_thread(void *arg) {
    live_t *L = arg;
    int rc;
    if (L->mode == 0)
        rc = asmspy_engine_syscalls(L->pid, -1, &L->stop, live_syscall_sink, L);
    else if (L->mode == 2)
        rc = asmspy_engine_stream(L->pid, -1, &L->stop, L->syms,
                                  live_stream_sink, L);
    else if (L->mode == 3)
        rc = asmspy_engine_graph(L->pid, -1, &L->stop, L->syms, live_graph_sink,
                                 L);
    else if (L->mode == 4)
        rc = asmspy_engine_tree(L->pid, -1, &L->stop, L->syms, live_stream_sink,
                                L);
    else
        rc = asmspy_engine_region(L->pid, L->base, L->len, -1, &L->stop,
                                  live_region_sink, L);
    pthread_mutex_lock(&L->mu);
    L->rc = rc;
    pthread_mutex_unlock(&L->mu);
    atomic_store(&L->finished, true);
    return NULL;
}

/* Run a live view (mode 0 syscalls / mode 1 region) until the user leaves it.
 * Returns 0 to go back to the process's options (the user pressed 'b'), or 1 to
 * go back to the process list ('q'/ESC). */
static int run_live_view(pid_t pid, int mode, uint64_t base, size_t len,
                         const char *title, const asmspy_symtab_t *syms) {
    live_t L;
    memset(&L, 0, sizeof L);
    pthread_mutex_init(&L.mu, NULL);
    atomic_store(&L.stop, false);
    atomic_store(&L.finished, false);
    L.pid = pid;
    L.mode = mode;
    L.base = base;
    L.len = len;
    L.syms = syms;
    L.rc = 0;

    /* keep SIGALRM off the UI thread so the tracer thread owns the quit-wake */
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &block, NULL);

    pthread_t th;
    if (pthread_create(&th, NULL, tracer_thread, &L) != 0) {
        pthread_sigmask(SIG_UNBLOCK, &block, NULL);
        pthread_mutex_destroy(&L.mu);
        return 1;
    }

    int back = 1;     /* 1 = to process list (q/ESC), 0 = to options (b) */
    int paused = 0;   /* freeze the log tail to scroll history (modes 0/2) */
    long vbottom = 0; /* main-log bottom line while paused (absolute index) */
    long vstr = 0;    /* strings-pane bottom while paused (mode 0) */
    for (;;) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        int log_h = rows - 3 > 0 ? rows - 3 : 1;
        /* scrollable log views: syscalls, instruction stream, call tree */
        int is_log = (mode == 0 || mode == 2 || mode == 4);
        erase();
        attron(A_BOLD);
        mvprintw(0, 0, "%.*s", cols, title);
        attroff(A_BOLD);

        pthread_mutex_lock(&L.mu);
        int fin = atomic_load(&L.finished);
        int erc = L.rc;
        long lognewest = log_newest(&L.log);
        int logcount = L.log.count;
        unsigned long logtotal = L.log.total;
        long strnewest = log_newest(&L.strlog);
        if (mode == 0) {
            /* left half: the syscall stream; right half: decoded strings */
            int midx = cols / 2;
            int lw = midx - 1;
            int rx = midx + 1;
            int rw = cols - rx;
            if (lw < 1)
                lw = 1;
            if (rw < 1)
                rw = 1;
            int h = rows - 3;
            attron(A_BOLD);
            mvprintw(1, 0, "SYSCALLS");
            if (rx < cols)
                mvprintw(1, rx, "STRINGS");
            attroff(A_BOLD);
            for (int r = 1; r < rows - 1; r++)
                mvaddch(r, midx, ACS_VLINE);
            draw_log_at(&L.log, 2, 0, h, lw, paused ? vbottom : lognewest);
            draw_log_at(&L.strlog, 2, rx, h, rw, paused ? vstr : strnewest);
        } else if (mode == 2 || mode == 4) {
            /* single-pane live log: instruction stream (2) or call tree (4) */
            attron(A_BOLD);
            mvprintw(1, 0,
                     mode == 4
                         ? "CALL TREE  (-> function [module], indent = call depth)"
                         : "LIVE STREAM  (function+off [module]   disassembly)");
            attroff(A_BOLD);
            draw_log_at(&L.log, 2, 0, rows - 3, cols,
                        paused ? vbottom : lognewest);
        } else if (mode == 3) {
            /* whole-process call graph, sorted live; sort in place under mu */
            graph_sort_key = L.gsort;
            qsort(L.graph.v, L.graph.n, sizeof *L.graph.v, gnode_cmp);
            attron(A_BOLD);
            mvprintw(1, 0,
                     "CALL GRAPH  (%zu functions, sort: %s)   [int]=own exe  "
                     "[EXT]=library",
                     L.graph.n,
                     L.gsort == GSORT_FANOUT ? "functions called" : "invocations");
            attroff(A_BOLD);
            if (L.graph.n == 0)
                mvprintw(2, 0, "(waiting for calls — whole-process "
                               "single-stepping is slow)");
            for (int r = 0; r < rows - 3 && r < (int)L.graph.n; r++) {
                char row[256];
                graph_format_row(row, sizeof row, &L.graph.v[r]);
                mvprintw(2 + r, 0, "%-*.*s", cols, cols, row);
            }
        } else {
            mvprintw(1, 0, "%.*s", cols, L.asm_header);
            int split = (rows - 3) * 3 / 5;
            if (split < 1)
                split = 1;
            attron(A_BOLD);
            mvprintw(2, 0, "ASSEMBLY");
            attroff(A_BOLD);
            for (int r = 0; r < split && r < (int)L.asm_dis.n; r++)
                mvprintw(3 + r, 0, "%-*.*s", cols, cols, L.asm_dis.v[r]);
            int fy = 3 + split;
            attron(A_BOLD);
            mvprintw(fy, 0, "FUNCTIONS CALLED");
            attroff(A_BOLD);
            if (L.asm_fn.n == 0)
                mvprintw(fy + 1, 0, "(leaf — no calls, or waiting for a call)");
            for (int r = 0; r < (rows - fy - 2) && r < (int)L.asm_fn.n; r++)
                mvprintw(fy + 1 + r, 0, "%-*.*s", cols, cols, L.asm_fn.v[r]);
        }
        pthread_mutex_unlock(&L.mu);

        if (is_log && paused)
            mvprintw(rows - 1, 0,
                     "[PAUSED %ld/%lu]  up/down PgUp/PgDn Home/End: scroll  "
                     "space: live   b: options   q: processes",
                     vbottom < 0 ? 0 : vbottom + 1, logtotal);
        else if (fin) {
            char e[128];
            asmspy_strerror(erc, e, sizeof e);
            mvprintw(rows - 1, 0,
                     "[tracer stopped: %s]  %sb: options   q: processes",
                     erc ? e : "target exited or done",
                     is_log ? "space: scroll history   " : "");
        } else if (mode == 3)
            mvprintw(rows - 1, 0,
                     "s: sort (invocations/functions-called)   b: options   "
                     "q/ESC: processes   (live)");
        else
            mvprintw(rows - 1, 0,
                     "%sb: back to options   q/ESC: processes   (live)",
                     is_log ? "space: pause   " : "");
        clrtoeol();
        refresh();

        timeout(120);
        int ch = getch();
        if (ch == 'q' || ch == 27) {
            back = 1;
            break;
        }
        if (ch == 'b') {
            back = 0;
            break;
        }
        if (mode == 3 && (ch == 's' || ch == 'S'))
            L.gsort =
                L.gsort == GSORT_INVOCATIONS ? GSORT_FANOUT : GSORT_INVOCATIONS;
        if (is_log) {
            int page = log_h > 1 ? log_h - 1 : 1;
            /* scrolling up while live-tailing enters pause at the current tail */
            if ((ch == KEY_UP || ch == KEY_PPAGE || ch == KEY_HOME) && !paused) {
                paused = 1;
                vbottom = lognewest;
                vstr = strnewest;
            }
            switch (ch) {
            case ' ':
            case 'p': /* toggle pause; freeze both panes at the current tail */
                if (!paused) {
                    paused = 1;
                    vbottom = lognewest;
                    vstr = strnewest;
                } else
                    paused = 0;
                break;
            case KEY_UP:
                vbottom -= 1;
                break;
            case KEY_PPAGE:
                vbottom -= page;
                break;
            case KEY_HOME:
                vbottom = 0; /* clamped up to the oldest line below */
                break;
            case KEY_DOWN:
                if (paused && vbottom + 1 >= lognewest)
                    paused = 0; /* stepped off the bottom -> resume live tail */
                else if (paused)
                    vbottom += 1;
                break;
            case KEY_NPAGE:
                if (paused)
                    vbottom += page; /* clamped to the tail below (stays paused) */
                break;
            case KEY_END:
                paused = 0; /* jump back to the live tail */
                break;
            default:
                break;
            }
            if (paused) { /* keep the anchor within the buffered range */
                long oldest = lognewest - logcount + 1; /* = total - count */
                if (oldest < 0)
                    oldest = 0;
                if (vbottom < oldest)
                    vbottom = oldest;
                if (vbottom > lognewest)
                    vbottom = lognewest;
            }
        }
    }

    atomic_store(&L.stop, true);
    /* Wake the tracer's blocked waitpid. A single edge-triggered SIGALRM can be
     * missed if it lands between the engine's stop-check and the blocking
     * waitpid, so re-arm every 50 ms until the join succeeds — the tracer is
     * then parked in waitpid and the next SIGALRM is guaranteed to interrupt it. */
    for (;;) {
        pthread_kill(th, SIGALRM);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50L * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        if (pthread_timedjoin_np(th, NULL, &ts) == 0)
            break;
    }
    pthread_sigmask(SIG_UNBLOCK, &block, NULL);

    svec_free(&L.asm_dis);
    svec_free(&L.asm_fn);
    free(L.graph.v);
    pthread_mutex_destroy(&L.mu);
    return back;
}

/* mode-select screen; returns 0 syscalls, 1 region, 2 stream, 3 graph, -1 back */
static int screen_mode(const asmspy_proc_t *p) {
    for (;;) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        (void)rows;
        erase();
        attron(A_BOLD);
        mvprintw(0, 0, "asmspy — pid %d (%.*s)", p->pid, cols - 24, p->cmd);
        attroff(A_BOLD);
        if (!p->attachable)
            mvprintw(1, 0, "! not owned by you — attach may fail "
                           "(needs CAP_SYS_PTRACE / ptrace_scope=0)");
        mvprintw(3, 2, "1)  Syscall log        — live syscalls with data (a strace)");
        mvprintw(4, 2, "2)  Assembly & funcs   — live disassembly + call-graph of a function");
        mvprintw(5, 2, "3)  Live stream        — every instruction as it runs (function + assembly)");
        mvprintw(6, 2, "4)  Call graph         — whole-process caller/callee counts (sortable)");
        mvprintw(7, 2, "5)  Call tree          — whole-process live call tree (indented by depth)");
        mvprintw(rows - 1, 0, "1/2/3/4/5: choose   b/ESC: back");
        refresh();
        int ch = getch();
        if (ch == '1')
            return 0;
        if (ch == '2')
            return 1;
        if (ch == '3')
            return 2;
        if (ch == '4')
            return 3;
        if (ch == '5')
            return 4;
        if (ch == 'b' || ch == 27 || ch == 'q')
            return -1;
    }
}

/* symbol picker; returns 0 and fills the region on selection, -1 on back.
 * 'r' reloads the target's function symbols in place (so `t` stays current for
 * the caller, which passes it on to the live view for callee naming). */
static int screen_syms(pid_t pid, uint64_t *base, size_t *len,
                       asmspy_symtab_t *t) {
    for (;;) { /* (re-)build loop — 'r' reloads t and re-enters */
        if (t->n == 0)
            return -1;
        char **items = malloc(t->n * sizeof *items);
        if (!items)
            return -1;
        for (size_t i = 0; i < t->n; i++) {
            char b[256];
            snprintf(b, sizeof b, "%-40s %6llu  0x%llx  [%s]", t->v[i].name,
                     (unsigned long long)t->v[i].size,
                     (unsigned long long)t->v[i].addr, t->v[i].module);
            items[i] = strdup(b);
        }
        List L;
        if (list_init(&L, items, (int)t->n) != 0) {
            for (size_t i = 0; i < t->n; i++)
                free(items[i]);
            free(items);
            return -1;
        }

        int outcome = 0; /* 1 pick, 2 back, 3 reload */
        int selidx = -1;
        while (outcome == 0) {
            int rows, cols;
            getmaxyx(stdscr, rows, cols);
            erase();
            attron(A_BOLD);
            mvprintw(0, 0, "asmspy — pick a function to trace (pid %d)",
                     (int)pid);
            attroff(A_BOLD);
            list_render(&L, 1, rows - 3, cols);
            mvprintw(rows - 1, 0,
                     "type: filter   Enter: trace   r: reload   ESC: back   "
                     "(%d/%zu)",
                     L.nmatch, t->n);
            clrtoeol();
            refresh();
            int ch = getch();
            if (ch == 27)
                outcome = 2;
            else if (ch == 'r' && L.flen == 0)
                outcome = 3;
            else {
                int sel = list_key(&L, ch, rows - 3);
                if (sel >= 0 && t->v[sel].size > 0) { /* need a sized region */
                    selidx = sel;
                    outcome = 1;
                }
            }
        }

        if (outcome == 1) {
            *base = t->v[selidx].addr;
            *len = t->v[selidx].size;
        }
        list_done(&L);
        for (size_t i = 0; i < t->n; i++)
            free(items[i]);
        free(items);

        if (outcome == 1)
            return 0;
        if (outcome == 2)
            return -1;
        /* outcome == 3: reload the target's symbols in place, then rebuild */
        asmspy_symtab_free(t);
        if (asmspy_symtab_load(pid, t) < 0)
            return -1;
    }
}

/* process picker with a live sort toggle; returns 0 and fills *picked on
 * selection, -1 on quit. Tab toggles pid <-> most-recently-active order. */
static int screen_procs(asmspy_proc_t *picked) {
    asmspy_sort_t sort = ASMSPY_SORT_PID;
    for (;;) { /* (re-)scan loop — Tab re-enters with the other sort */
        asmspy_proc_t *procs = NULL;
        size_t np = 0;
        if (asmspy_proclist(&procs, &np, sort) < 0)
            return -1;
        char **items = malloc((np ? np : 1) * sizeof *items);
        if (!items) {
            free(procs);
            return -1;
        }
        for (size_t i = 0; i < np; i++) {
            char b[256];
            if (sort == ASMSPY_SORT_SCAN)
                snprintf(b, sizeof b, "%-7d %4u %5llu %c %-12.12s %.150s",
                         procs[i].pid, procs[i].scan, procs[i].cpu,
                         procs[i].attachable ? ' ' : '!', procs[i].user,
                         procs[i].cmd);
            else if (sort == ASMSPY_SORT_ACTIVE)
                snprintf(b, sizeof b, "%-7d %6llu %c %-12.12s %.170s",
                         procs[i].pid, procs[i].cpu,
                         procs[i].attachable ? ' ' : '!', procs[i].user,
                         procs[i].cmd);
            else
                snprintf(b, sizeof b, "%-7d %c %-12.12s %.200s", procs[i].pid,
                         procs[i].attachable ? ' ' : '!', procs[i].user,
                         procs[i].cmd);
            items[i] = strdup(b);
        }
        List L;
        if (list_init(&L, items, (int)np) != 0) {
            for (size_t i = 0; i < np; i++)
                free(items[i]);
            free(items);
            free(procs);
            return -1;
        }

        int outcome = 0; /* 1 pick, 2 quit, 3 toggle, 4 refresh */
        int selidx = -1;
        while (outcome == 0) {
            int rows, cols;
            getmaxyx(stdscr, rows, cols);
            erase();
            attron(A_BOLD);
            const char *sortname =
                sort == ASMSPY_SORT_SCAN
                    ? "string-scan (alnum density, then recent)  [PID STR CPU]"
                    : sort == ASMSPY_SORT_ACTIVE
                          ? "recent activity (CPU jiffies)  [PID CPU]"
                          : "pid";
            mvprintw(0, 0,
                     "asmspy — select a process   [sort: %s]   (%zu; '!' = not "
                     "yours)",
                     sortname, np);
            attroff(A_BOLD);
            list_render(&L, 1, rows - 3, cols);
            mvprintw(rows - 1, 0, "type: filter   Enter: select   Tab: cycle "
                                  "sort   r: refresh   q: quit");
            clrtoeol();
            refresh();
            int ch = getch();
            if ((ch == 'q' && L.flen == 0) || ch == 27)
                outcome = 2;
            else if (ch == '\t')
                outcome = 3;
            else if (ch == 'r' && L.flen == 0)
                outcome = 4; /* re-scan (re-samples CPU in activity sort) */
            else {
                int sel = list_key(&L, ch, rows - 3);
                if (sel >= 0) {
                    selidx = sel;
                    outcome = 1;
                }
            }
        }

        asmspy_proc_t chosen;
        int have = (outcome == 1);
        if (have)
            chosen = procs[selidx];
        list_done(&L);
        for (size_t i = 0; i < np; i++)
            free(items[i]);
        free(items);
        free(procs);

        if (outcome == 1) {
            *picked = chosen;
            return 0;
        }
        if (outcome == 2)
            return -1;
        if (outcome == 3) /* cycle pid -> active -> scan; outcome 4 keeps it */
            sort = (asmspy_sort_t)((sort + 1) % 3);
        /* outcome 3 or 4: fall through -> outer loop re-scans */
    }
}

int asmspy_tui(void) {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    for (;;) { /* process-list loop */
        asmspy_proc_t picked;
        if (screen_procs(&picked) != 0)
            break; /* quit */

        for (;;) { /* per-process options loop ('b' in a live view returns here) */
            int mode = screen_mode(&picked);
            if (mode < 0)
                break; /* back to the process list */

            int nav; /* run_live_view: 0 = back to options, 1 = to process list */
            if (mode == 0) {
                char title[128];
                snprintf(title, sizeof title,
                         "asmspy — syscalls of pid %d (%.*s)", picked.pid, 40,
                         picked.cmd);
                nav = run_live_view(picked.pid, 0, 0, 0, title, NULL);
            } else if (mode == 2 || mode == 3 || mode == 4) {
                /* stream / graph / tree: symbols best-effort (raw addrs if absent) */
                asmspy_symtab_t t;
                asmspy_symtab_load(picked.pid, &t);
                const char *what = mode == 3   ? "call graph"
                                   : mode == 4 ? "call tree"
                                               : "live stream";
                char title[128];
                snprintf(title, sizeof title, "asmspy — %s of pid %d (%.*s)",
                         what, picked.pid, 40, picked.cmd);
                nav = run_live_view(picked.pid, mode, 0, 0, title, &t);
                asmspy_symtab_free(&t);
            } else {
                asmspy_symtab_t t;
                if (asmspy_symtab_load(picked.pid, &t) < 0 || t.n == 0) {
                    erase();
                    mvprintw(0, 0,
                             "no function symbols for pid %d (stripped? "
                             "permission?) — press a key",
                             picked.pid);
                    refresh();
                    getch();
                    asmspy_symtab_free(&t);
                    continue; /* back to options */
                }
                uint64_t base = 0;
                size_t len = 0;
                if (screen_syms(picked.pid, &base, &len, &t) == 0) {
                    const asmspy_sym_t *s = asmspy_symtab_at(&t, base);
                    char title[160];
                    snprintf(title, sizeof title,
                             "asmspy — %s @ 0x%llx of pid %d",
                             s ? s->name : "region", (unsigned long long)base,
                             picked.pid);
                    nav = run_live_view(picked.pid, 1, base, len, title, &t);
                } else {
                    nav = 0; /* symbol-picker ESC -> back to options */
                }
                asmspy_symtab_free(&t);
            }
            if (nav == 1)
                break; /* back to the process list */
            /* nav == 0: loop -> options again */
        }
    }

    endwin();
    return 0;
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

/* atoi("nginx") is 0 and atoi("-5") is -5, either of which would sail past
 * argument handling and surface as a confusing failure from deep inside an
 * attach. Parse strictly, and say which argument was wrong. */
static int parse_pid(const char *s, pid_t *out) {
    errno = 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v <= 0 || v > INT_MAX)
        return -1;
    *out = (pid_t)v;
    return 0;
}

/* A sample/instruction count. Negative means "until the target exits", which
 * the engines already implement as max < 0. */
static int parse_count(const char *s, long *out) {
    errno = 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    *out = v;
    return 0;
}

static int bad_arg(const char *what, const char *got) {
    fprintf(stderr, "asmspy: '%s' is not a valid %s\n", got, what);
    return 2;
}

static int usage(const char *argv0) {
    fprintf(stderr,
            "asmspy — watch a running process out of band\n\n"
            "  %s                         interactive TUI\n"
            "  %s --list [active|scan]    list processes (active=recent CPU; scan=string-rich memory)\n"
            "  %s --syms   <pid> [filter] list resolved function symbols\n"
            "  %s --log    <pid> [n]      stream n syscalls with data\n"
            "  %s --trace  <pid> <sym|0xADDR[:LEN]> [n]  live samples of a function/region\n"
            "  %s --stream <pid> [n]      stream n instructions live (function + asm)\n"
            "  %s --graph  <pid> [n] [--sort=invocations|fanout]  whole-process call graph over n calls\n"
            "  %s --tree   <pid> [n]      whole-process live call tree, indented by depth (n call lines)\n"
            "  %s --procs  <pid> [n] [--count=syscalls|calls]  process/thread tree (procs+threads+children) with counts\n",
            argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
    return 2;
}

int main(int argc, char **argv) {
    if (argc == 1)
        return asmspy_tui();
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)
        return usage(argv[0]);
    if (strcmp(argv[1], "--list") == 0) {
        asmspy_sort_t s = ASMSPY_SORT_PID;
        if (argc >= 3) {
            if (strcmp(argv[2], "active") == 0)
                s = ASMSPY_SORT_ACTIVE;
            else if (strcmp(argv[2], "scan") == 0)
                s = ASMSPY_SORT_SCAN;
            else /* silently sorting by pid instead would just look broken */
                return bad_arg("sort (want 'active' or 'scan')", argv[2]);
        }
        return cmd_list(s);
    }

    pid_t pid = 0;
    long n = 0;
    if (strcmp(argv[1], "--syms") == 0 && argc >= 3) {
        if (parse_pid(argv[2], &pid) != 0)
            return bad_arg("pid", argv[2]);
        return cmd_syms(pid, argc >= 4 ? argv[3] : NULL);
    }
    if (strcmp(argv[1], "--log") == 0 && argc >= 3) {
        if (parse_pid(argv[2], &pid) != 0)
            return bad_arg("pid", argv[2]);
        n = 20;
        if (argc >= 4 && parse_count(argv[3], &n) != 0)
            return bad_arg("count", argv[3]);
        return cmd_log(pid, n);
    }
    if (strcmp(argv[1], "--trace") == 0 && argc >= 4) {
        if (parse_pid(argv[2], &pid) != 0)
            return bad_arg("pid", argv[2]);
        n = 3;
        if (argc >= 5 && parse_count(argv[4], &n) != 0)
            return bad_arg("count", argv[4]);
        return cmd_trace(pid, argv[3], n);
    }
    if (strcmp(argv[1], "--stream") == 0 && argc >= 3) {
        if (parse_pid(argv[2], &pid) != 0)
            return bad_arg("pid", argv[2]);
        n = 20;
        if (argc >= 4 && parse_count(argv[3], &n) != 0)
            return bad_arg("count", argv[3]);
        return cmd_stream(pid, n);
    }
    if (strcmp(argv[1], "--tree") == 0 && argc >= 3) {
        if (parse_pid(argv[2], &pid) != 0)
            return bad_arg("pid", argv[2]);
        n = 40;
        if (argc >= 4 && parse_count(argv[3], &n) != 0)
            return bad_arg("count", argv[3]);
        return cmd_tree(pid, n);
    }
    if (strcmp(argv[1], "--procs") == 0 && argc >= 3) {
        if (parse_pid(argv[2], &pid) != 0)
            return bad_arg("pid", argv[2]);
        n = 200; /* invocations to count before reporting; negative = until exit */
        asmspy_count_t mode = ASMSPY_COUNT_SYSCALLS;
        for (int i = 3; i < argc; i++) { /* [n] and --count= in any order */
            if (strncmp(argv[i], "--count=", 8) == 0) {
                const char *v = argv[i] + 8;
                if (strcmp(v, "syscalls") == 0)
                    mode = ASMSPY_COUNT_SYSCALLS;
                else if (strcmp(v, "calls") == 0)
                    mode = ASMSPY_COUNT_CALLS;
                else
                    return bad_arg("count (want 'syscalls' or 'calls')", v);
            } else if (parse_count(argv[i], &n) != 0) {
                return bad_arg("count", argv[i]);
            }
        }
        return cmd_procs(pid, n, mode);
    }
    if (strcmp(argv[1], "--graph") == 0 && argc >= 3) {
        if (parse_pid(argv[2], &pid) != 0)
            return bad_arg("pid", argv[2]);
        n = 200; /* calls to record before reporting; negative = until exit */
        gsort_t sort = GSORT_INVOCATIONS;
        for (int i = 3; i < argc; i++) { /* [n] and --sort= in any order */
            if (strncmp(argv[i], "--sort=", 7) == 0) {
                const char *v = argv[i] + 7;
                if (strcmp(v, "invocations") == 0)
                    sort = GSORT_INVOCATIONS;
                else if (strcmp(v, "fanout") == 0 ||
                         strcmp(v, "functions-called") == 0)
                    sort = GSORT_FANOUT;
                else
                    return bad_arg("sort (want 'invocations' or 'fanout')", v);
            } else if (parse_count(argv[i], &n) != 0) {
                return bad_arg("count", argv[i]);
            }
        }
        return cmd_graph(pid, n, sort);
    }
    return usage(argv[0]);
}
