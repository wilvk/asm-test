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
 *   asmspy --stream <pid> [n] [--tid=<t>]   stream n instructions live (function + asm)
 *   asmspy --graph  <pid> [n] [--sort=invocations|fanout] [--json|--dot] [--tid=<t>]  whole-process call graph
 *   asmspy --tree   <pid> [n] [--json|--dot] [--tid=<t>]  whole-process live call tree (indented by depth)
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
#include <sys/uio.h> /* process_vm_readv — read a function's bytes to disassemble */
#include <time.h>
#include <unistd.h>

#include "asmspy.h"
#include "asmspy_graphsort.h" /* gsort_t + gnode_cmp (unit-tested separately) */
#include "asmspy_logview.h"
#include "asmtest_ibs.h" /* --sample: out-of-band statistical hot-edge capture */

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
#define MAX_EDGES  256 /* cap displayed distinct call edges             */

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
        const asmspy_sym_t *s =
            syms ? asmspy_symtab_at(syms, agg[i].tgt) : NULL;
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

/* gsort_t + gnode_cmp live in asmspy_graphsort.h (extracted so the ordering/
 * tiebreak contract is unit-testable — cli/test_graphsort.c). */

/* A retained copy of an engine snapshot (the engine's arrays are transient). */
typedef struct {
    asmspy_gnode_t *v;
    size_t n, cap;
    asmspy_gedge_t *e; /* caller->callee edges, keyed by endpoint addresses */
    size_t ne, ecap;
} graph_snap;

/* Copy the node + edge snapshot in, growing as needed (replace, not append —
 * each engine snapshot is the whole graph so far). `edges` may be NULL. */
static void graph_snap_copy(graph_snap *s, const asmspy_gnode_t *nodes,
                            size_t n, const asmspy_gedge_t *edges, size_t ne) {
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
    if (ne > s->ecap) {
        asmspy_gedge_t *ne2 = realloc(s->e, ne * sizeof *ne2);
        if (ne2) {
            s->e = ne2;
            s->ecap = ne;
        } else {
            ne = 0; /* OOM: drop edges this snapshot, keep the nodes */
        }
    }
    if (ne)
        memcpy(s->e, edges, ne * sizeof *edges);
    s->ne = ne;
}

/* The node's class as a stable machine-readable token (the JSON counterpart of
 * the human [int]/[EXT]/[JIT]/[?] tag): "internal" (target's own exe), "external"
 * (shared/system library or a PLT thunk), "jit" (perf-map managed method), or
 * "unknown" (no symbol resolved). */
static const char *gnode_kind(const asmspy_gnode_t *nd) {
    if (strcmp(nd->module, "?") == 0)
        return "unknown";
    if (strcmp(nd->module, "jit") == 0)
        return "jit";
    return nd->external ? "external" : "internal";
}

/* Escape `s` into `out` as the body of a JSON double-quoted string (demangled C++
 * and JIT names carry quotes, backslashes, and spaces). Always NUL-terminates. */
static void json_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* Escape `s` for a Graphviz DOT double-quoted string (only " and \ matter; a
 * demangled/JIT name never carries a newline). Always NUL-terminates. */
static void dot_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 2 < cap; s++) {
        if (*s == '"' || *s == '\\')
            out[o++] = '\\';
        out[o++] = *s;
    }
    out[o] = '\0';
}

/* Node fill colour by class, for the DOT export. */
static const char *gnode_fill(const asmspy_gnode_t *nd) {
    const char *k = gnode_kind(nd);
    if (strcmp(k, "jit") == 0)
        return "#fff3c4"; /* JIT/managed */
    if (strcmp(k, "external") == 0)
        return "#eeeeee"; /* library / PLT */
    if (strcmp(k, "unknown") == 0)
        return "#ffe0e0"; /* unresolved */
    return "#e8f0ff";     /* internal */
}

/* One call-graph row: an internal/external/JIT marker, the function, its counts
 * (times called / calls made / distinct callees), and the backing module.
 * `[JIT]` marks a managed-runtime method named from the perf-map; `[?]` an
 * address no symbol resolved at all (stripped or anonymous). */
static void graph_format_row(char *buf, size_t cap, const asmspy_gnode_t *nd) {
    const char *tag = strcmp(nd->module, "?") == 0     ? "[?]"
                      : strcmp(nd->module, "jit") == 0 ? "[JIT]"
                      : nd->external                   ? "[EXT]"
                                                       : "[int]";
    snprintf(buf, cap, "%-5s %-30.30s inv=%-7llu calls=%-7llu fanout=%-5u [%s]",
             tag, nd->name, nd->invocations, nd->out_calls, nd->fanout,
             nd->module);
}

/* One caller/callee peer for the node drill-in: the neighbouring function's
 * entry address and how many calls crossed that edge. */
typedef struct {
    uint64_t addr;
    unsigned long long count;
} gpeer_t;

static int gpeer_cmp(const void *a, const void *b) {
    const gpeer_t *x = a, *y = b;
    if (x->count != y->count)
        return x->count < y->count ? 1 : -1; /* descending by call count */
    return x->addr < y->addr ? -1 : x->addr > y->addr ? 1 : 0;
}

/* Find the node with entry address `addr` in the snapshot (linear — the graph is
 * small and this runs once per drill-in). NULL if the address has no node. */
static const asmspy_gnode_t *graph_node_at(const graph_snap *g, uint64_t addr) {
    for (size_t i = 0; i < g->n; i++)
        if (g->v[i].addr == addr)
            return &g->v[i];
    return NULL;
}

/* Format one caller/callee peer line: "  <count>x  <tag> name [module]", falling
 * back to the raw address when the peer address has no resolved node. */
static void gpeer_format(char *buf, size_t cap, const graph_snap *g,
                         const gpeer_t *p) {
    const asmspy_gnode_t *nd = graph_node_at(g, p->addr);
    const char *tag = !nd                              ? "[?]"
                      : strcmp(nd->module, "?") == 0   ? "[?]"
                      : strcmp(nd->module, "jit") == 0 ? "[JIT]"
                      : nd->external                   ? "[EXT]"
                                                       : "[int]";
    if (nd)
        snprintf(buf, cap, "  %-8llu %-5s %-30.30s [%s]", p->count, tag,
                 nd->name, nd->module);
    else
        snprintf(buf, cap, "  %-8llu %-5s 0x%llx", p->count, tag,
                 (unsigned long long)p->addr);
}

/* Build the "called by" (callers) and "calls" (callees) peer lists for `sel`
 * from the frozen edge table, most-frequent first, into `callers`/`callees`
 * (each cleared first). Caller holds the snapshot lock, or has frozen the sink. */
static void graph_build_detail(const graph_snap *g, const asmspy_gnode_t *sel,
                               svec *callers, svec *callees) {
    svec_clear(callers);
    svec_clear(callees);
    gpeer_t *in = NULL, *out = NULL;
    size_t ni = 0, no = 0;
    for (size_t i = 0; i < g->ne; i++) {
        const asmspy_gedge_t *e = &g->e[i];
        if (e->callee_addr == sel->addr) { /* an edge INTO sel: a caller */
            gpeer_t *nn = realloc(in, (ni + 1) * sizeof *in);
            if (nn) {
                in = nn;
                in[ni].addr = e->caller_addr;
                in[ni].count = e->count;
                ni++;
            }
        }
        if (e->caller_addr == sel->addr) { /* an edge OUT of sel: a callee */
            gpeer_t *nn = realloc(out, (no + 1) * sizeof *out);
            if (nn) {
                out = nn;
                out[no].addr = e->callee_addr;
                out[no].count = e->count;
                no++;
            }
        }
    }
    qsort(in, ni, sizeof *in, gpeer_cmp);
    qsort(out, no, sizeof *out, gpeer_cmp);
    char line[256];
    for (size_t i = 0; i < ni; i++) {
        gpeer_format(line, sizeof line, g, &in[i]);
        svec_push(callers, line);
    }
    for (size_t i = 0; i < no; i++) {
        gpeer_format(line, sizeof line, g, &out[i]);
        svec_push(callees, line);
    }
    free(in);
    free(out);
}

/* Disassemble the function containing `addr` (extent from `syms`, else a fixed
 * window) by reading its bytes live from the target, into `out` (header + one
 * line per instruction). Used by the call-tree view's assembly pane. */
static void asm_of_function(pid_t pid, const asmspy_symtab_t *syms,
                            uint64_t addr, svec *out) {
    svec_clear(out);
    if (!addr)
        return;
    const asmspy_sym_t *s = syms ? asmspy_symtab_at(syms, addr) : NULL;
    uint64_t base = addr;
    size_t len = 96; /* no symbol: show a small window from the entry */
    if (s && s->size) {
        base = s->addr;
        len = s->size;
    }
    if (len > 4096)
        len = 4096;
    char hdr[176];
    if (s)
        snprintf(hdr, sizeof hdr, "%s [%s]  0x%llx", s->name, s->module,
                 (unsigned long long)base);
    else
        snprintf(hdr, sizeof hdr, "0x%llx (no symbol)",
                 (unsigned long long)base);
    svec_push(out, hdr);
    uint8_t *code = malloc(len);
    if (!code) {
        svec_push(out, "  (out of memory)");
        return;
    }
    struct iovec l = {code, len};
    struct iovec r = {(void *)(uintptr_t)base, len};
    ssize_t got = process_vm_readv(pid, &l, 1, &r, 1, 0);
    if (got <= 0)
        svec_push(out, "  (unreadable)");
    else if (!asmtest_disas_available())
        svec_push(out, "  (no disassembler)");
    else {
        uint64_t off = 0;
        while (off < (uint64_t)got) {
            char dis[160];
            size_t ilen = asmtest_disas(ASMTEST_ARCH_X86_64, code, (size_t)got,
                                        base, off, dis, sizeof dis);
            if (!ilen)
                break;
            char line[200];
            snprintf(line, sizeof line, "  +0x%-4llx  %s",
                     (unsigned long long)off, dis[0] ? dis : "(?)");
            svec_push(out, line);
            off += ilen;
        }
    }
    free(code);
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
    char text
        [256]; /* room for a deep glyph prefix + "node/tid <id> [exe] inv=" */
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
    int emitted; /* forest guard: render each process once */
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

/* Box-drawing connectors for the process/thread forest (UTF-8; the TUI is
 * ncursesw and setlocale(LC_ALL,"") is set, so wide glyphs render). */
#define TG_TEE  "├─ " /* ├─  a child with more siblings below */
#define TG_ELB  "└─ " /* └─  the last child                  */
#define TG_PIPE "│  " /* │   an ancestor with more below     */
#define TG_GAP  "   " /*     an ancestor that was last       */

/* Emit the subtree rooted at process P[k]. `prefix` is the glyph column string
 * for this node's descendants' ancestors; `is_last` is whether P[k] is the last
 * of its own siblings (picks its elbow and its children's continuation column);
 * `is_root` suppresses a connector (top-level processes draw flush-left). A
 * process's children are its threads (when it has more than its lone leader),
 * then its child processes — the last of the combined set gets the └─ elbow. */
static void topo_emit_proc(proc_agg *P, size_t np, const asmspy_task_t *tasks,
                           size_t k, const char *prefix, int is_last,
                           int is_root, topo_rows *out) {
    if (P[k].emitted)
        return; /* cycle / already-rendered guard */
    P[k].emitted = 1;

    topo_row_t row;
    memset(&row, 0, sizeof row);
    snprintf(row.text, sizeof row.text, "%s%snode %d%s%s%s  inv=%llu", prefix,
             is_root   ? ""
             : is_last ? TG_ELB
                       : TG_TEE,
             (int)P[k].tgid, P[k].exe[0] ? " [" : "", P[k].exe,
             P[k].exe[0] ? "]" : "", P[k].inv);
    row.tgid = row.tid = P[k].tgid;
    row.is_process = 1;
    row.inv = P[k].inv;
    trows_push(out, &row);

    /* column prefix for THIS node's children: extend the parent's with a pipe if
     * this node has more siblings below it, else blank. Roots start empty. */
    char cpx[160];
    if (is_root)
        cpx[0] = '\0';
    else
        snprintf(cpx, sizeof cpx, "%s%s", prefix, is_last ? TG_GAP : TG_PIPE);

    /* gather the threads (sorted) — only when more than the lone leader — and the
     * child processes (sorted), so the combined last child gets the elbow. */
    size_t *ti = NULL, nth = 0;
    if (P[k].ntask > 1) {
        ti = malloc(P[k].ntask * sizeof *ti);
        if (ti) {
            for (size_t j = 0; j < P[k].ntask; j++)
                ti[j] = P[k].task[j];
            g_tasks_for_cmp = tasks;
            qsort(ti, P[k].ntask, sizeof *ti, task_idx_cmp);
            nth = P[k].ntask;
        }
    }
    size_t *ci = malloc(np * sizeof *ci);
    size_t nc = 0;
    if (ci) {
        for (size_t j = 0; j < np; j++)
            if (j != k && !P[j].emitted && P[j].ppid == P[k].tgid)
                ci[nc++] = j;
        g_procs_for_cmp = P;
        qsort(ci, nc, sizeof *ci, proc_idx_cmp);
    }

    size_t total = nth + nc, idx = 0;
    for (size_t j = 0; j < nth; j++, idx++) {
        const asmspy_task_t *t = &tasks[ti[j]];
        memset(&row, 0, sizeof row);
        snprintf(row.text, sizeof row.text, "%s%stid %d%s%s%s  inv=%llu", cpx,
                 idx + 1 == total ? TG_ELB : TG_TEE, (int)t->tid,
                 t->comm[0] ? " (" : "", t->comm, t->comm[0] ? ")" : "",
                 t->inv);
        row.tid = t->tid;
        row.tgid = t->tgid;
        row.is_process = 0;
        row.inv = t->inv;
        trows_push(out, &row);
    }
    for (size_t j = 0; j < nc; j++, idx++)
        topo_emit_proc(P, np, tasks, ci[j], cpx, idx + 1 == total, 0, out);

    free(ti);
    free(ci);
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
            topo_emit_proc(P, np, tasks, roots[j], "", 0, 1, out);
        /* any process not reached (orphaned parent chain) — emit as a root */
        for (size_t k = 0; k < np; k++)
            if (!P[k].emitted)
                topo_emit_proc(P, np, tasks, k, "", 0, 1, out);
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
            printf("%-8d %-5u %-8llu %-12.12s %-4s %.64s\n", v[i].pid,
                   v[i].scan, v[i].cpu, v[i].user,
                   v[i].attachable ? "yes" : "-", v[i].cmd);
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
               (unsigned long long)t.v[i].addr, (unsigned long long)t.v[i].size,
               t.v[i].name, t.v[i].module);
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

static int cmd_stream(pid_t pid, pid_t tid, long n) {
    asmspy_symtab_t t;
    asmspy_symtab_load(pid, &t); /* best-effort; raw addresses if empty */
    int rc =
        asmspy_engine_stream(pid, tid, n, NULL, &t, stream_print_sink, NULL);
    asmspy_symtab_free(&t);
    if (rc != 0) {
        char e[128];
        asmspy_strerror(rc, e, sizeof e);
        fprintf(stderr, "attach failed: %s\n", e);
        return 1;
    }
    return 0;
}

static void tree_print_sink(void *ctx, const char *line,
                            const asmspy_tree_call_t *call) {
    (void)ctx;
    (void)call; /* headless text: the pre-rendered line is the whole output */
    printf("%s\n", line);
    fflush(stdout);
}

/* One retained call-tree entry for the --json/--dot exporters (the sink's
 * name/module pointers are transient, so the strings are copied in). */
typedef struct {
    pid_t tid;
    int depth;
    uint64_t addr;
    char name[128];
    char module[64];
} tree_rec;

typedef struct {
    tree_rec *v;
    size_t n, cap;
} tree_capture;

static void tree_capture_sink(void *ctx, const char *line,
                              const asmspy_tree_call_t *call) {
    (void)line;
    tree_capture *tc = ctx;
    if (tc->n == tc->cap) {
        size_t nc = tc->cap ? tc->cap * 2 : 256;
        tree_rec *nv = realloc(tc->v, nc * sizeof *nv);
        if (!nv)
            return; /* OOM: drop this entry, keep what we have */
        tc->v = nv;
        tc->cap = nc;
    }
    tree_rec *r = &tc->v[tc->n++];
    r->tid = call->tid;
    r->depth = call->depth;
    r->addr = call->addr;
    snprintf(r->name, sizeof r->name, "%s", call->name);
    snprintf(r->module, sizeof r->module, "%s", call->module);
}

/* Node fill colour for the --tree DOT export. The tree records carry no
 * internal/external split (unlike the graph engine's nodes), so this colours
 * what they do know: JIT/managed, unresolved, or named. */
static const char *tree_fill(const char *module) {
    if (strcmp(module, "jit") == 0)
        return "#fff3c4"; /* JIT/managed */
    if (strcmp(module, "?") == 0)
        return "#ffe0e0"; /* unresolved */
    return "#e8f0ff";     /* named (exe or library) */
}

/* Emit the captured call tree as a Graphviz digraph, mirroring the --graph
 * exporter's conventions (addr-keyed nodes, count-labelled edges). The tree is
 * a temporal log, so edges are AGGREGATED here: each entry at depth d is a call
 * from the latest depth-(d-1) entry on the same thread — the same shadow-stack
 * discipline the engine used to compute the depths in the first place. */
static void tree_export_dot(const tree_capture *tc, pid_t pid) {
    typedef struct { /* per-addr node: last-seen naming + times entered */
        uint64_t addr;
        const tree_rec *rec;
        unsigned long long entered;
    } dnode;
    typedef struct { /* aggregated caller->callee edge */
        uint64_t from, to;
        unsigned long long count;
    } dedge;
    typedef struct { /* one thread's live shadow stack of entry addresses */
        pid_t tid;
        uint64_t at[64];
    } dstack;
    dnode *nodes = NULL;
    dedge *edges = NULL;
    dstack *stacks = NULL;
    size_t nn = 0, ne = 0, ns = 0;
    if (tc->n) { /* one alloc each — the capture bounds every table */
        nodes = calloc(tc->n, sizeof *nodes);
        edges = calloc(tc->n, sizeof *edges);
        stacks = calloc(tc->n, sizeof *stacks);
    }
    if (nodes && edges && stacks) {
        for (size_t i = 0; i < tc->n; i++) {
            const tree_rec *r = &tc->v[i];
            size_t k;
            for (k = 0; k < nn && nodes[k].addr != r->addr; k++)
                ;
            if (k == nn) {
                nodes[nn].addr = r->addr;
                nn++;
            }
            nodes[k].rec = r; /* newest naming wins (a JIT may recompile) */
            nodes[k].entered++;

            for (k = 0; k < ns && stacks[k].tid != r->tid; k++)
                ;
            if (k == ns) {
                stacks[ns].tid = r->tid;
                ns++;
            }
            int d = r->depth < 63 ? r->depth : 63;
            /* the caller is the latest shallower entry on this thread; depth 0
             * (or a parent the capture never saw) roots a new subtree */
            uint64_t parent = d > 0 ? stacks[k].at[d - 1] : 0;
            stacks[k].at[d] = r->addr;
            if (parent) {
                size_t e;
                for (e = 0; e < ne && !(edges[e].from == parent &&
                                        edges[e].to == r->addr);
                     e++)
                    ;
                if (e == ne) {
                    edges[ne].from = parent;
                    edges[ne].to = r->addr;
                    ne++;
                }
                edges[e].count++;
            }
        }
    }
    printf("digraph asmspy {\n  rankdir=LR;\n  node [shape=box, style=filled,"
           " fontname=monospace, fontsize=10];\n");
    for (size_t i = 0; i < nn; i++) {
        char lbl[4 * sizeof nodes[i].rec->name];
        dot_escape(nodes[i].rec->name, lbl, sizeof lbl);
        printf("  \"0x%llx\" [label=\"%s\\n[%s] entered=%llu\","
               " fillcolor=\"%s\"];\n",
               (unsigned long long)nodes[i].addr, lbl, nodes[i].rec->module,
               nodes[i].entered, tree_fill(nodes[i].rec->module));
    }
    for (size_t i = 0; i < ne; i++)
        printf("  \"0x%llx\" -> \"0x%llx\" [label=\"%llu\"];\n",
               (unsigned long long)edges[i].from,
               (unsigned long long)edges[i].to, edges[i].count);
    printf("}\n");
    (void)pid;
    free(nodes);
    free(edges);
    free(stacks);
}

/* Emit the captured call tree as JSON: the faithful temporal call log (one
 * object per call, in emission order, with tid/depth/addr/name/module), same
 * top-level shape as the --graph exporter ({"pid":…, one array}). */
static void tree_export_json(const tree_capture *tc, pid_t pid) {
    printf("{\"pid\":%d,\"calls\":[", (int)pid);
    for (size_t i = 0; i < tc->n; i++) {
        const tree_rec *r = &tc->v[i];
        char en[4 * sizeof r->name], em[4 * sizeof r->module];
        json_escape(r->name, en, sizeof en);
        json_escape(r->module, em, sizeof em);
        printf("%s\n  {\"seq\":%zu,\"tid\":%d,\"depth\":%d,\"addr\":\"0x%llx\","
               "\"name\":\"%s\",\"module\":\"%s\"}",
               i ? "," : "", i, (int)r->tid, r->depth,
               (unsigned long long)r->addr, en, em);
    }
    printf("%s]}\n", tc->n ? "\n" : "");
}

static int cmd_tree(pid_t pid, pid_t tid, long n, int json, int dot) {
    asmspy_symtab_t t;
    asmspy_symtab_load(pid, &t); /* best-effort; raw addrs if empty */
    tree_capture tc = {0};
    int export = json || dot;
    int rc = asmspy_engine_tree(pid, tid, n, NULL, &t,
                                export ? tree_capture_sink : tree_print_sink,
                                export ? &tc : NULL);
    asmspy_symtab_free(&t);
    if (rc != 0) {
        char e[128];
        asmspy_strerror(rc, e, sizeof e);
        fprintf(stderr, "attach failed: %s\n", e);
        free(tc.v);
        return 1;
    }
    if (dot) /* like --graph: --dot wins when both flags are given */
        tree_export_dot(&tc, pid);
    else if (json)
        tree_export_json(&tc, pid);
    free(tc.v);
    return 0;
}

static void graph_capture_sink(void *ctx, const asmspy_gnode_t *nodes,
                               size_t nn, const asmspy_gedge_t *edges,
                               size_t ne) {
    graph_snap_copy(ctx, nodes, nn, edges,
                    ne); /* keep only the latest snapshot */
}

static int cmd_graph(pid_t pid, pid_t tid, long n, gsort_t sort, int json,
                     int dot) {
    asmspy_symtab_t t;
    asmspy_symtab_load(pid,
                       &t); /* best-effort; raw addrs (all internal) if empty */
    graph_snap snap = {0};
    int rc =
        asmspy_engine_graph(pid, tid, n, NULL, &t, graph_capture_sink, &snap);
    asmspy_symtab_free(&t);
    if (rc != 0) {
        char e[128];
        asmspy_strerror(rc, e, sizeof e);
        fprintf(stderr, "attach failed: %s\n", e);
        free(snap.v);
        free(snap.e);
        return 1;
    }
    graph_sort_key = sort;
    qsort(snap.v, snap.n, sizeof *snap.v, gnode_cmp);

    if (dot) { /* Graphviz: asmspy --graph <pid> --dot | dot -Tsvg -o graph.svg */
        printf(
            "digraph asmspy {\n  rankdir=LR;\n  node [shape=box, style=filled,"
            " fontname=monospace, fontsize=10];\n");
        for (size_t i = 0; i < snap.n; i++) {
            const asmspy_gnode_t *nd = &snap.v[i];
            char lbl[4 * sizeof nd->name];
            dot_escape(nd->name, lbl, sizeof lbl);
            printf(
                "  \"0x%llx\" [label=\"%s\\n[%s] inv=%llu\", fillcolor=\"%s\"];"
                "\n",
                (unsigned long long)nd->addr, lbl, gnode_kind(nd),
                nd->invocations, gnode_fill(nd));
        }
        for (size_t i = 0; i < snap.ne; i++)
            printf("  \"0x%llx\" -> \"0x%llx\" [label=\"%llu\"];\n",
                   (unsigned long long)snap.e[i].caller_addr,
                   (unsigned long long)snap.e[i].callee_addr, snap.e[i].count);
        printf("}\n");
        free(snap.v);
        free(snap.e);
        return 0;
    }

    if (json) { /* machine-readable nodes + edges (pipe to jq / a visualizer) */
        printf("{\"pid\":%d,\"sort\":\"%s\",\"functions\":[", (int)pid,
               sort == GSORT_FANOUT ? "fanout" : "invocations");
        for (size_t i = 0; i < snap.n; i++) {
            const asmspy_gnode_t *nd = &snap.v[i];
            char en[4 * sizeof nd->name], em[4 * sizeof nd->module];
            json_escape(nd->name, en, sizeof en);
            json_escape(nd->module, em, sizeof em);
            printf(
                "%s\n  {\"addr\":\"0x%llx\",\"name\":\"%s\",\"module\":\"%s\","
                "\"kind\":\"%s\",\"invocations\":%llu,\"out_calls\":%llu,"
                "\"fanout\":%u}",
                i ? "," : "", (unsigned long long)nd->addr, en, em,
                gnode_kind(nd), nd->invocations, nd->out_calls, nd->fanout);
        }
        printf("%s],\"edges\":[", snap.n ? "\n" : "");
        for (size_t i = 0; i < snap.ne; i++)
            printf("%s\n  {\"caller\":\"0x%llx\",\"callee\":\"0x%llx\","
                   "\"count\":%llu}",
                   i ? "," : "", (unsigned long long)snap.e[i].caller_addr,
                   (unsigned long long)snap.e[i].callee_addr, snap.e[i].count);
        printf("%s]}\n", snap.ne ? "\n" : "");
        free(snap.v);
        free(snap.e);
        return 0;
    }

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
    free(snap.e);
    return 0;
}

/* ================================================================== */
/* Statistical hot-edge sampler view (headless; TUI shares the engine) */
/* ================================================================== */

/* A retained copy of a sample snapshot (the engine's array is transient). */
typedef struct {
    asmspy_sample_edge_t *v;
    size_t n, cap;
    uint64_t samples, branch_samples, lost;
    int throttled;
} sample_snap;

/* Replace a retained snapshot with the engine's transient one (grows as needed;
 * keeps the previous snapshot on OOM). Shared by the headless sink and the TUI. */
static void sample_snap_set(sample_snap *s, const asmspy_sample_edge_t *edges,
                            size_t n, uint64_t samples, uint64_t branch_samples,
                            uint64_t lost, int throttled) {
    if (n > s->cap) {
        asmspy_sample_edge_t *nv = realloc(s->v, n * sizeof *nv);
        if (!nv)
            return; /* keep the previous snapshot on OOM */
        s->v = nv;
        s->cap = n;
    }
    if (n)
        memcpy(s->v, edges, n * sizeof *edges);
    s->n = n;
    s->samples = samples;
    s->branch_samples = branch_samples;
    s->lost = lost;
    s->throttled = throttled;
}

static void sample_capture_sink(void *ctx, const asmspy_sample_edge_t *edges,
                                size_t n, uint64_t samples,
                                uint64_t branch_samples, uint64_t lost,
                                int throttled) {
    sample_snap_set(ctx, edges, n, samples, branch_samples, lost, throttled);
}

/* How to rank the statistical hot-edge table. */
typedef enum {
    SSORT_COUNT = 0,   /* hottest (most-sampled) edges first — the default */
    SSORT_MISPRED = 1, /* most-mispredicted edges first                    */
} ssort_t;

/* qsort comparator; the active key is set through this file-scope selector right
 * before each qsort (the sort point is single-threaded under the view lock). */
static ssort_t sample_sort_key = SSORT_COUNT;
static int sedge_cmp(const void *a, const void *b) {
    const asmspy_sample_edge_t *x = a, *y = b;
    unsigned long long kx =
        sample_sort_key == SSORT_MISPRED ? x->mispred : x->count;
    unsigned long long ky =
        sample_sort_key == SSORT_MISPRED ? y->mispred : y->count;
    if (kx != ky)
        return kx < ky ? 1 : -1; /* descending */
    if (x->count != y->count)    /* stable tiebreak: heavier edge first */
        return x->count < y->count ? 1 : -1;
    return 0;
}

/* A short human tag for an edge's misprediction / return character. */
static void sample_edge_tag(const asmspy_sample_edge_t *e, char *out,
                            size_t cap) {
    int mp = e->count ? (int)((e->mispred * 100ull) / e->count) : 0;
    if (e->is_return && e->mispred)
        snprintf(out, cap, " [ret misp %d%%]", mp);
    else if (e->is_return)
        snprintf(out, cap, " [ret]");
    else if (e->mispred)
        snprintf(out, cap, " [misp %d%%]", mp);
    else
        out[0] = '\0';
}

/* One hot-edge row: "<count>  <from> -> <to>  [misp N%]/[ret]". */
static void sample_format_row(char *buf, size_t cap,
                              const asmspy_sample_edge_t *e) {
    char tag[32];
    sample_edge_tag(e, tag, sizeof tag);
    snprintf(buf, cap, "%8llu  %-30.30s -> %-30.30s%s", e->count, e->from,
             e->to, tag);
}

static int cmd_sample(pid_t pid, long ms, int json) {
    if (!asmtest_ibs_available()) {
        /* Clean self-skip (exit 0), like examples/ibs_probe.c — the whole lane is
         * AMD-IBS-only, so on any other host --sample is simply unavailable. */
        printf("# SKIP --sample: %s\n", asmtest_ibs_skip_reason());
        return 0;
    }
    asmspy_symtab_t t;
    asmspy_symtab_load(pid, &t); /* best-effort; raw addrs if empty */
    asmspy_jitmap_t jit;
    asmspy_jitmap_init(&jit, pid);
    asmspy_jitmap_refresh(
        &jit); /* name methods already JIT-compiled at attach */
    sample_snap snap = {0};
    int rc = asmspy_engine_sample(pid, (unsigned)ms, NULL, &t, &jit,
                                  sample_capture_sink, &snap);
    asmspy_jitmap_free(&jit);
    asmspy_symtab_free(&t);

    if (rc ==
        ASMSPY_SAMPLE_UNAVAIL) { /* substrate present but perf_open blocked */
        printf("# SKIP --sample: %s\n", asmtest_ibs_skip_reason());
        free(snap.v);
        return 0;
    }
    if (rc != 0) {
        char e[128];
        asmspy_strerror(rc, e, sizeof e);
        fprintf(stderr, "sample failed: %s\n", e);
        free(snap.v);
        return 1;
    }

    if (json) { /* machine-readable: statistical edges + honest provenance */
        printf("{\"pid\":%d,\"mode\":\"ibs-op\",\"samples\":%llu,"
               "\"branch_samples\":%llu,\"lost\":%llu,\"throttled\":%s,"
               "\"edges\":[",
               (int)pid, (unsigned long long)snap.samples,
               (unsigned long long)snap.branch_samples,
               (unsigned long long)snap.lost,
               snap.throttled ? "true" : "false");
        for (size_t i = 0; i < snap.n; i++) {
            const asmspy_sample_edge_t *e = &snap.v[i];
            char ef[4 * sizeof e->from], et[4 * sizeof e->to];
            json_escape(e->from, ef, sizeof ef);
            json_escape(e->to, et, sizeof et);
            printf("%s\n  {\"from\":\"0x%llx\",\"from_name\":\"%s\","
                   "\"to\":\"0x%llx\",\"to_name\":\"%s\",\"count\":%llu,"
                   "\"mispred\":%u,\"is_return\":%u}",
                   i ? "," : "", (unsigned long long)e->from_addr, ef,
                   (unsigned long long)e->to_addr, et, e->count, e->mispred,
                   e->is_return);
        }
        printf("%s]}\n", snap.n ? "\n" : "");
        free(snap.v);
        return 0;
    }

    printf("statistical hot edges (AMD IBS-Op, out of band) — %zu edges, "
           "%llu/%llu branch/total samples%s (pid %d)\n",
           snap.n, (unsigned long long)snap.branch_samples,
           (unsigned long long)snap.samples,
           snap.throttled ? ", throttled" : "", (int)pid);
    printf("statistical, not exact: an edge here WAS taken; absence proves "
           "nothing.\n");
    if (snap.n == 0)
        printf("(no taken-branch samples in the window — target idle, or the "
               "window is too short)\n");
    for (size_t i = 0; i < snap.n; i++) {
        const asmspy_sample_edge_t *e = &snap.v[i];
        char tag[32];
        sample_edge_tag(e, tag, sizeof tag);
        printf("%8llu  %-34.34s -> %-34.34s%s\n", e->count, e->from, e->to,
               tag);
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
    printf("process/thread topology — %zu tasks, counting %s (pid %d)\n",
           snap.n, mode == ASMSPY_COUNT_CALLS ? "calls" : "syscalls", (int)pid);
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
    int rc =
        asmspy_engine_region(pid, base, len, n, NULL, region_print_sink, &t);
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
    uint64_t aux[LOG_CAP]; /* per-line datum (call-tree: the callee address) */
    int head, count;
    unsigned long total; /* lines ever pushed (monotonic); drives scrollback */
} logbuf;

static void log_push_aux(logbuf *lg, const char *s, uint64_t aux) {
    snprintf(lg->lines[lg->head], LOG_LINE_MAX, "%s", s);
    lg->aux[lg->head] = aux;
    lg->head = (lg->head + 1) % LOG_CAP;
    if (lg->count < LOG_CAP)
        lg->count++;
    lg->total++;
}
static void log_push(logbuf *lg, const char *s) { log_push_aux(lg, s, 0); }

/* The aux datum of an absolute line index (0 if out of the buffered range). */
static uint64_t log_aux_at(const logbuf *lg, long abs) {
    if (abs < 0)
        return 0;
    return lg->aux[(unsigned long)abs % LOG_CAP];
}

/* Render the ring buffer into the (y0,x0) w×h box with `bottom` (an absolute
 * line index) as the last visible line; asmspy_log_window does the clamped
 * viewport math (unit-tested in test_logview.c). A `bottom` past the newest
 * line is clamped, so log_newest() tails the live stream. */
/* Render the ring buffer, highlighting the absolute line `cursor` (none if <0). */
static void draw_log_cursor(const logbuf *lg, int y0, int x0, int h, int w,
                            long bottom, long cursor) {
    if (h < 1 || w < 1)
        return;
    long top = 0;
    int n = asmspy_log_window(lg->total, lg->count, bottom, h, &top);
    for (int r = 0; r < n; r++) {
        long abs = top + r;
        int slot = (int)((unsigned long)abs % LOG_CAP);
        int cur = (abs == cursor);
        if (cur)
            attron(A_REVERSE);
        mvprintw(y0 + r, x0, "%-*.*s", w, w, lg->lines[slot]);
        if (cur)
            attroff(A_REVERSE);
    }
}

static void draw_log_at(const logbuf *lg, int y0, int x0, int h, int w,
                        long bottom) {
    draw_log_cursor(lg, y0, x0, h, w, bottom, -1);
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
    L->nmatch = L->top = L->sel = L->flen =
        0; /* init BEFORE the fallible alloc */
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
    int rpaused; /* UI froze the region: skip re-sampling so the scroll is stable */
    const asmspy_symtab_t *syms;
    /* call-graph view (mode 3) */
    graph_snap graph;
    gsort_t gsort;
    int gpaused; /* UI froze the graph: skip snapshot copies so scroll is stable */
    /* call-tree view (mode 4): disassembly of the selected function (UI-thread only) */
    svec tree_asm;
    uint64_t tree_asm_addr;
    /* process/thread topology view (mode 5) */
    topo_snap topo;
    asmspy_count_t count_mode;
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
    if (!L->rpaused) { /* while the user scrolls a frozen sample, drop updates */
        svec_clear(&L->asm_dis);
        svec_clear(&L->asm_fn);
        region_render(L->asm_header, sizeof L->asm_header, &L->asm_dis,
                      &L->asm_fn, sample_no, result, tr, desc, code, len, base,
                      L->syms);
        L->asm_sample = sample_no;
    }
    pthread_mutex_unlock(&L->mu);
}

static void live_stream_sink(void *ctx, const char *line) {
    live_t *L = ctx;
    pthread_mutex_lock(&L->mu);
    log_push(&L->log, line);
    pthread_mutex_unlock(&L->mu);
}

static void live_graph_sink(void *ctx, const asmspy_gnode_t *nodes, size_t nn,
                            const asmspy_gedge_t *edges, size_t ne) {
    live_t *L = ctx;
    pthread_mutex_lock(&L->mu);
    if (!L->gpaused) /* while the user scrolls a frozen snapshot, drop updates */
        graph_snap_copy(&L->graph, nodes, nn, edges, ne);
    pthread_mutex_unlock(&L->mu);
}

/* Call-tree line + its callee address (for the assembly pane's disassembly). */
static void live_tree_sink(void *ctx, const char *line,
                           const asmspy_tree_call_t *call) {
    live_t *L = ctx;
    pthread_mutex_lock(&L->mu);
    log_push_aux(&L->log, line, call->addr);
    pthread_mutex_unlock(&L->mu);
}

static void live_topo_sink(void *ctx, const asmspy_task_t *tasks, size_t n) {
    live_t *L = ctx;
    pthread_mutex_lock(&L->mu);
    topo_snap_copy(&L->topo, tasks, n);
    pthread_mutex_unlock(&L->mu);
}

static void *tracer_thread(void *arg) {
    live_t *L = arg;
    int rc;
    if (L->mode == 0)
        rc = asmspy_engine_syscalls(L->pid, -1, &L->stop, live_syscall_sink, L);
    else if (L->mode == 2)
        rc = asmspy_engine_stream(L->pid, 0, -1, &L->stop, L->syms,
                                  live_stream_sink, L);
    else if (L->mode == 3)
        rc = asmspy_engine_graph(L->pid, 0, -1, &L->stop, L->syms,
                                 live_graph_sink, L);
    else if (L->mode == 4)
        rc = asmspy_engine_tree(L->pid, 0, -1, &L->stop, L->syms,
                                live_tree_sink, L);
    else if (L->mode == 5)
        rc = asmspy_engine_procs(L->pid, -1, &L->stop, L->count_mode,
                                 live_topo_sink, L);
    else
        rc = asmspy_engine_region(L->pid, L->base, L->len, -1, &L->stop,
                                  live_region_sink, L);
    pthread_mutex_lock(&L->mu);
    L->rc = rc;
    pthread_mutex_unlock(&L->mu);
    atomic_store(&L->finished, true);
    return NULL;
}

/* Modal drill-in for one call-graph node: the selected function's header plus
 * its callers ("CALLED BY") and callees ("CALLS"), each with the per-edge call
 * count and scrollable together. `callers`/`callees` are the caller's private
 * copies (see graph_build_detail), so this reads no shared state. Returns when
 * the user presses Enter/b/q/ESC — the graph view underneath then resumes. */
static void run_graph_detail(const char *title, const asmspy_gnode_t *sel,
                             const svec *callers, const svec *callees) {
    /* compose the scrollable body once: two labelled sections */
    svec body = {0};
    char hdr[128];
    snprintf(hdr, sizeof hdr, "CALLED BY  (%zu)", callers->n);
    svec_push(&body, hdr);
    if (callers->n == 0)
        svec_push(&body, "  (none — a root/thread entry, or its caller was not "
                         "sampled)");
    for (size_t i = 0; i < callers->n; i++)
        svec_push(&body, callers->v[i]);
    svec_push(&body, "");
    snprintf(hdr, sizeof hdr, "CALLS  (%zu)", callees->n);
    svec_push(&body, hdr);
    if (callees->n == 0)
        svec_push(&body, "  (none — a leaf function)");
    for (size_t i = 0; i < callees->n; i++)
        svec_push(&body, callees->v[i]);

    const char *tag = strcmp(sel->module, "?") == 0     ? "[?]"
                      : strcmp(sel->module, "jit") == 0 ? "[JIT]"
                      : sel->external                   ? "[EXT]"
                                                        : "[int]";
    int top = 0;
    for (;;) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        int bodytop = 4;              /* header rows 0..3 */
        int vis = rows - bodytop - 1; /* keep the last row for the hint */
        if (vis < 1)
            vis = 1;
        int bn = (int)body.n;
        if (top > bn - vis)
            top = bn - vis;
        if (top < 0)
            top = 0;
        erase();
        attron(A_BOLD);
        mvprintw(0, 0, "%.*s", cols, title);
        mvprintw(1, 0, "%s %-.60s  [%s]", tag, sel->name, sel->module);
        attroff(A_BOLD);
        mvprintw(2, 0,
                 "called %llu times   makes %llu calls across %u functions   "
                 "0x%llx",
                 sel->invocations, sel->out_calls, sel->fanout,
                 (unsigned long long)sel->addr);
        for (int r = 0; r < vis && top + r < bn; r++)
            mvprintw(bodytop + r, 0, "%-*.*s", cols, cols, body.v[top + r]);
        mvprintw(
            rows - 1, 0,
            "up/down PgUp/PgDn Home/End: scroll   Enter/b/q: back to graph");
        clrtoeol();
        refresh();

        timeout(150);
        int ch = getch();
        int page = vis > 1 ? vis - 1 : 1;
        if (ch == 'q' || ch == 27 || ch == 'b' || ch == '\n' || ch == KEY_ENTER)
            break;
        switch (ch) {
        case KEY_UP:
            top -= 1;
            break;
        case KEY_DOWN:
            top += 1;
            break;
        case KEY_PPAGE:
            top -= page;
            break;
        case KEY_NPAGE:
            top += page;
            break;
        case KEY_HOME:
            top = 0;
            break;
        case KEY_END:
            top = bn; /* clamped to the last page above */
            break;
        default:
            break;
        }
    }
    svec_free(&body);
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
    int paused = 0;   /* freeze the log tail / graph snapshot to scroll it */
    long vbottom = 0; /* main-log bottom line while paused (absolute index) */
    long vstr = 0;    /* strings-pane bottom while paused (mode 0) */
    int gtop = 0;  /* call-graph (mode 3): first visible row while scrolling */
    int gsel = 0;  /* call-graph (mode 3): selected row (for Enter: drill-in) */
    int apane = 0; /* region (mode 1): focused pane (0 = assembly, 1 = funcs) */
    int atop = 0,
        ftop = 0; /* region: per-pane first visible row while scrolling */
    for (;;) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        int log_h = rows - 3 > 0 ? rows - 3 : 1;
        /* scrollable log views: syscalls, instruction stream, call tree */
        int is_log = (mode == 0 || mode == 2 || mode == 4);
        /* call-tree (mode 4) two-pane geometry + selected function, filled under
         * the lock and used to draw the assembly pane after it is released */
        uint64_t tree_addr = 0;
        int tree_rx = 0, tree_rw = 0;
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
        } else if (mode == 2) {
            /* single-pane live instruction stream */
            attron(A_BOLD);
            mvprintw(1, 0,
                     "LIVE STREAM  (function+off [module]   disassembly)");
            attroff(A_BOLD);
            draw_log_at(&L.log, 2, 0, rows - 3, cols,
                        paused ? vbottom : lognewest);
        } else if (mode == 4) {
            /* two-pane call tree: the tree feed on the left, and the assembly of
             * the SELECTED function (the cursor line) on the right. The cursor is
             * the tail while live, or the scrolled-to line while paused. */
            int asmw = cols / 2;
            if (asmw < 20)
                asmw = 20;
            int lw = cols - asmw - 1;
            if (lw < 1)
                lw = 1;
            tree_rx = lw + 1;
            tree_rw = cols - tree_rx;
            long cursor = paused ? vbottom : lognewest;
            tree_addr = log_aux_at(&L.log, cursor);
            attron(A_BOLD);
            mvprintw(1, 0, "CALL TREE  (-> function [module], indent = depth)");
            if (tree_rx < cols)
                mvprintw(1, tree_rx + 1, "ASSEMBLY (selected)");
            attroff(A_BOLD);
            for (int r = 1; r < rows - 1; r++)
                mvaddch(r, lw, ACS_VLINE);
            /* highlight the selected line only while paused (i.e. selecting); the
             * live tail shows the newest call's assembly without a reverse bar */
            draw_log_cursor(&L.log, 2, 0, rows - 3, lw, cursor,
                            paused ? cursor : -1);
            /* the assembly pane itself is drawn after the lock is released */
        } else if (mode == 3) {
            /* whole-process call graph, sorted live; sort in place under mu.
             * Frozen (paused or the tracer finished) so scroll is stable — see
             * gpaused, which stops the sink overwriting the snapshot. */
            int gfrozen = paused || fin;
            L.gpaused = gfrozen;
            graph_sort_key = L.gsort;
            qsort(L.graph.v, L.graph.n, sizeof *L.graph.v, gnode_cmp);
            int gn = (int)L.graph.n;
            int vis = rows - 3 > 0 ? rows - 3 : 1;
            if (!gfrozen) {
                gtop = 0; /* live view is top-anchored (highest-ranked first) */
                gsel = 0; /* selection resets to the top-ranked node       */
            } else {
                /* keep the selection in range and scrolled on-screen */
                if (gsel > gn - 1)
                    gsel = gn - 1;
                if (gsel < 0)
                    gsel = 0;
                if (gsel < gtop)
                    gtop = gsel;
                if (gsel >= gtop + vis)
                    gtop = gsel - vis + 1;
            }
            if (gtop > gn - vis)
                gtop = gn - vis;
            if (gtop < 0)
                gtop = 0;
            attron(A_BOLD);
            mvprintw(1, 0,
                     "CALL GRAPH  (%d functions, sort: %s)   [int]=own exe  "
                     "[EXT]=library",
                     gn,
                     L.gsort == GSORT_FANOUT ? "functions called"
                                             : "invocations");
            attroff(A_BOLD);
            if (gn == 0)
                mvprintw(2, 0,
                         "(waiting for calls — whole-process "
                         "single-stepping is slow)");
            for (int r = 0; r < vis && gtop + r < gn; r++) {
                char row[256];
                graph_format_row(row, sizeof row, &L.graph.v[gtop + r]);
                /* highlight the selected row while frozen (i.e. selecting) */
                int cur = gfrozen && (gtop + r == gsel);
                if (cur)
                    attron(A_REVERSE);
                mvprintw(2 + r, 0, "%-*.*s", cols, cols, row);
                if (cur)
                    attroff(A_REVERSE);
            }
        } else {
            /* region view: two stacked panes (function disassembly + its
             * callees), each scrollable. Frozen (paused or the tracer finished)
             * so scroll is stable — rpaused stops the sink re-sampling under the
             * reader. Tab moves focus between the panes. */
            int rfrozen = paused || fin;
            L.rpaused = rfrozen;
            mvprintw(1, 0, "%.*s", cols, L.asm_header);
            int split = (rows - 3) * 3 / 5;
            if (split < 1)
                split = 1;
            int fy = 3 + split;       /* FUNCTIONS CALLED header row        */
            int asm_h = split;        /* assembly rows: screen 3..fy-1      */
            int fn_h = rows - fy - 2; /* funcs rows: screen fy+1..rows-2     */
            if (fn_h < 1)
                fn_h = 1;
            int an = (int)L.asm_dis.n, fnn = (int)L.asm_fn.n;
            if (!rfrozen) /* live view is top-anchored (newest sample at top) */
                atop = ftop = 0;
            if (atop > an - asm_h)
                atop = an - asm_h;
            if (atop < 0)
                atop = 0;
            if (ftop > fnn - fn_h)
                ftop = fnn - fn_h;
            if (ftop < 0)
                ftop = 0;
            /* highlight the focused pane's header while frozen (i.e. selecting) */
            int afocus = rfrozen && apane == 0, ffocus = rfrozen && apane == 1;
            attron(afocus ? A_REVERSE : A_BOLD);
            mvprintw(2, 0, "ASSEMBLY%s", an > asm_h ? "  (scroll)" : "");
            attroff(afocus ? A_REVERSE : A_BOLD);
            for (int r = 0; r < asm_h && atop + r < an; r++)
                mvprintw(3 + r, 0, "%-*.*s", cols, cols, L.asm_dis.v[atop + r]);
            attron(ffocus ? A_REVERSE : A_BOLD);
            mvprintw(fy, 0, "FUNCTIONS CALLED%s",
                     fnn > fn_h ? "  (scroll)" : "");
            attroff(ffocus ? A_REVERSE : A_BOLD);
            if (fnn == 0)
                mvprintw(fy + 1, 0, "(leaf — no calls, or waiting for a call)");
            for (int r = 0; r < fn_h && ftop + r < fnn; r++)
                mvprintw(fy + 1 + r, 0, "%-*.*s", cols, cols,
                         L.asm_fn.v[ftop + r]);
        }
        pthread_mutex_unlock(&L.mu);

        /* Call-tree assembly pane (mode 4): reading the target's memory can block,
         * so do it OUTSIDE the lock. Re-disassemble only when the selection moves. */
        if (mode == 4 && tree_rw > 1) {
            if (tree_addr != L.tree_asm_addr) {
                asm_of_function(L.pid, L.syms, tree_addr, &L.tree_asm);
                L.tree_asm_addr = tree_addr;
            }
            for (int r = 0; r < rows - 3; r++) {
                const char *s = r < (int)L.tree_asm.n ? L.tree_asm.v[r] : "";
                mvprintw(2 + r, tree_rx + 1, "%-*.*s", tree_rw - 1, tree_rw - 1,
                         s ? s : "");
            }
        }

        if (is_log && paused)
            mvprintw(rows - 1, 0,
                     "[PAUSED %ld/%lu]  up/down PgUp/PgDn Home/End: select fn  "
                     "space: live   b: options   q: processes",
                     vbottom < 0 ? 0 : vbottom + 1, logtotal);
        else if (mode == 3 && paused && !fin)
            mvprintw(
                rows - 1, 0,
                "[PAUSED]  up/down: select   Enter: drill in   Tab: sort   "
                "space: live   b: options   q: processes");
        else if (mode == 1 && paused && !fin)
            mvprintw(
                rows - 1, 0,
                "[PAUSED]  up/down PgUp/PgDn Home/End: scroll   Tab: pane   "
                "space: live   b: options   q: processes");
        else if (fin) {
            char e[128];
            asmspy_strerror(erc, e, sizeof e);
            mvprintw(rows - 1, 0,
                     "[tracer stopped: %s]  %sb: options   q: processes",
                     erc ? e : "target exited or done",
                     is_log      ? "space: scroll history   "
                     : mode == 3 ? "up/down: select   Enter: drill in   "
                     : mode == 1 ? "up/down PgUp/PgDn Home/End: scroll   "
                                   "Tab: pane   "
                                 : "");
        } else if (mode == 3)
            mvprintw(rows - 1, 0,
                     "up/down: select   Enter: drill in   Tab: sort   "
                     "space: pause   b: options   q/ESC: processes   (live)");
        else if (mode == 1)
            mvprintw(rows - 1, 0,
                     "space: pause+scroll   Tab: pane   b: back to options   "
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
        if (mode == 3 && ch == '\t') /* Tab toggles the sort option */
            L.gsort =
                L.gsort == GSORT_INVOCATIONS ? GSORT_FANOUT : GSORT_INVOCATIONS;
        if (is_log) {
            int page = log_h > 1 ? log_h - 1 : 1;
            /* scrolling up while live-tailing enters pause at the current tail */
            if ((ch == KEY_UP || ch == KEY_PPAGE || ch == KEY_HOME) &&
                !paused) {
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
                    vbottom +=
                        page; /* clamped to the tail below (stays paused) */
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
        if (mode ==
            3) { /* call graph: freeze to select a node, Enter to drill in */
            int page = log_h > 1 ? log_h - 1 : 1;
            int gscroll =
                paused || fin; /* a finished graph is already frozen */
            /* any navigation (or Enter) into the list freezes the live re-sort */
            if ((ch == KEY_UP || ch == KEY_DOWN || ch == KEY_PPAGE ||
                 ch == KEY_NPAGE || ch == KEY_HOME || ch == KEY_END ||
                 ch == '\n' || ch == KEY_ENTER) &&
                !gscroll) {
                paused = 1;
                gscroll = 1;
            }
            switch (ch) {
            case ' ':
            case 'p':
                if (!fin) {
                    paused = !paused;
                    if (!paused) {
                        gtop =
                            0; /* resume live -> back to the top-ranked rows */
                        gsel = 0;
                    }
                }
                break;
            case KEY_UP:
                if (gscroll)
                    gsel -= 1;
                break;
            case KEY_DOWN:
                if (gscroll)
                    gsel += 1;
                break;
            case KEY_PPAGE:
                if (gscroll)
                    gsel -= page;
                break;
            case KEY_NPAGE:
                if (gscroll)
                    gsel += page;
                break;
            case KEY_HOME:
                if (gscroll)
                    gsel = 0;
                break;
            case KEY_END:
                if (gscroll)
                    gsel =
                        INT_MAX; /* clamped to the last node in the renderer */
                break;
            case '\n':
            case KEY_ENTER:
                if (gscroll) { /* drill into the selected node's callers/callees */
                    pthread_mutex_lock(&L.mu);
                    int gn = (int)L.graph.n;
                    int i = gsel;
                    if (i > gn - 1)
                        i = gn - 1;
                    if (i < 0)
                        i = 0;
                    if (gn > 0) {
                        /* the row array is sorted in the render pass and the sink
                         * is frozen, so v[i] is exactly the highlighted node */
                        asmspy_gnode_t selnode = L.graph.v[i];
                        svec callers = {0}, callees = {0};
                        graph_build_detail(&L.graph, &selnode, &callers,
                                           &callees);
                        pthread_mutex_unlock(&L.mu);
                        char dt[176];
                        snprintf(dt, sizeof dt,
                                 "asmspy — %.40s (call-graph drill-in, pid %d)",
                                 selnode.name, (int)pid);
                        run_graph_detail(dt, &selnode, &callers, &callees);
                        svec_free(&callers);
                        svec_free(&callees);
                    } else {
                        pthread_mutex_unlock(&L.mu);
                    }
                }
                break;
            default:
                break;
            }
            if (gsel < 0)
                gsel = 0;
        }
        if (mode ==
            1) { /* region: pause to freeze the sample; Tab switches pane */
            int split = (rows - 3) * 3 / 5;
            if (split < 1)
                split = 1;
            int ph =
                apane == 0 ? split : rows - (3 + split) - 2; /* pane height */
            if (ph < 1)
                ph = 1;
            int page = ph > 1 ? ph - 1 : 1;
            int rscroll =
                paused || fin; /* a finished sample is already frozen */
            /* scrolling into a pane freezes the live re-sampling */
            if ((ch == KEY_UP || ch == KEY_PPAGE) && !rscroll) {
                paused = 1;
                rscroll = 1;
            }
            int *off =
                apane == 0 ? &atop : &ftop; /* the focused pane's offset */
            switch (ch) {
            case ' ':
            case 'p':
                if (!fin) {
                    paused = !paused;
                    if (!paused)
                        atop = ftop =
                            0; /* resume live -> both panes to the top */
                }
                break;
            case '\t':
                apane ^= 1; /* switch the focused pane */
                break;
            case KEY_UP:
                if (rscroll)
                    *off -= 1;
                break;
            case KEY_DOWN:
                if (rscroll)
                    *off += 1;
                break;
            case KEY_PPAGE:
                if (rscroll)
                    *off -= page;
                break;
            case KEY_NPAGE:
                if (rscroll)
                    *off += page;
                break;
            case KEY_HOME:
                if (rscroll)
                    *off = 0;
                break;
            case KEY_END:
                if (rscroll)
                    *off =
                        INT_MAX; /* clamped to the last page in the renderer */
                break;
            default:
                break;
            }
            if (atop < 0)
                atop = 0;
            if (ftop < 0)
                ftop = 0;
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
    svec_free(&L.tree_asm);
    free(L.graph.v);
    free(L.graph.e);
    pthread_mutex_destroy(&L.mu);
    return back;
}

/* run_topo_view action (out-param): what the user asked for on exit. */
#define TOPO_ACT_NONE 0 /* left the view (b/q) — use the returned nav        */
#define TOPO_ACT_TOGGLE                                                        \
    1 /* Tab: flip the count mode and re-enter             */
#define TOPO_ACT_DRILL                                                         \
    2 /* Enter: drill into *out_drill's call graph          */

/* Live process/thread topology view (internal mode 5). Scroll the tree, Tab to
 * toggle the syscalls/calls count, Enter to drill into the selected node's
 * process. Detaches its tracer before returning, so a drill-in can re-attach the
 * (now free) subtree. Returns 0 to go to options, 1 to the process list; sets
 * *action to one of TOPO_ACT_* and, for DRILL, *out_drill to the chosen pid. */
static int run_topo_view(pid_t pid, asmspy_count_t cmode, const char *title,
                         int *action, pid_t *out_drill) {
    live_t L;
    memset(&L, 0, sizeof L);
    pthread_mutex_init(&L.mu, NULL);
    atomic_store(&L.stop, false);
    atomic_store(&L.finished, false);
    L.pid = pid;
    L.mode = 5;
    L.count_mode = cmode;
    L.rc = 0;
    *action = TOPO_ACT_NONE;

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

    int back = 1, sel = 0, top = 0;
    for (;;) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        int vis = rows - 3 > 0 ? rows - 3 : 1;
        erase();
        attron(A_BOLD);
        mvprintw(0, 0, "%.*s", cols, title);

        pthread_mutex_lock(&L.mu);
        int fin = atomic_load(&L.finished);
        int erc = L.rc;
        topo_snap snap = {0};
        topo_snap_copy(&snap, L.topo.v, L.topo.n);
        pthread_mutex_unlock(&L.mu);

        topo_rows tr = {0};
        topo_build_rows(snap.v, snap.n, &tr);
        mvprintw(1, 0,
                 "PROCESS/THREAD TREE  (%zu tasks, count: %s)   inv = per task",
                 snap.n, cmode == ASMSPY_COUNT_CALLS ? "calls" : "syscalls");
        attroff(A_BOLD);

        int nrows = (int)tr.n;
        if (sel >= nrows)
            sel = nrows ? nrows - 1 : 0;
        if (sel < 0)
            sel = 0;
        if (sel < top)
            top = sel;
        if (sel >= top + vis)
            top = sel - vis + 1;
        if (top < 0)
            top = 0;
        for (int r = 0; r < vis && top + r < nrows; r++) {
            int idx = top + r, cur = (idx == sel);
            if (cur)
                attron(A_REVERSE);
            mvprintw(2 + r, 0, "%-*.*s", cols, cols, tr.v[idx].text);
            if (cur)
                attroff(A_REVERSE);
        }
        if (nrows == 0)
            mvprintw(2, 0, "(waiting for activity — the target may be idle)");
        pid_t sel_tgid = (nrows && sel < nrows) ? tr.v[sel].tgid : 0;
        int sel_is_proc = (nrows && sel < nrows) ? tr.v[sel].is_process : 0;
        free(tr.v);
        free(snap.v);

        if (fin) {
            char e[128];
            asmspy_strerror(erc, e, sizeof e);
            mvprintw(rows - 1, 0,
                     "[tracer stopped: %s]   b: options   q: processes",
                     erc ? e : "target exited");
        } else {
            mvprintw(rows - 1, 0,
                     "up/down: select   Enter: drill into call graph   Tab: "
                     "syscalls/calls   b: options   q/ESC: processes");
        }
        clrtoeol();
        refresh();

        timeout(150);
        int ch = getch();
        if (ch == 'q' || ch == 27) {
            back = 1;
            break;
        }
        if (ch == 'b') {
            back = 0;
            break;
        }
        if (ch ==
            '\t') { /* Tab toggles the count option (restarts the engine) */
            *action = TOPO_ACT_TOGGLE;
            back = 0;
            break;
        }
        if ((ch == '\n' || ch == KEY_ENTER) && sel_tgid) {
            *action = TOPO_ACT_DRILL;
            *out_drill = sel_tgid;
            (void)
                sel_is_proc; /* a thread's tgid == its process, so either works */
            back = 0;
            break;
        }
        if (ch == KEY_UP)
            sel--;
        else if (ch == KEY_DOWN)
            sel++;
        else if (ch == KEY_PPAGE)
            sel -= vis;
        else if (ch == KEY_NPAGE)
            sel += vis;
        else if (ch == KEY_HOME)
            sel = 0;
        else if (ch == KEY_END)
            sel = nrows ? nrows - 1 : 0;
    }

    atomic_store(&L.stop, true);
    for (
        ;
        ;) { /* wake the tracer's blocked waitpid, then join (as run_live_view) */
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
    free(L.topo.v);
    pthread_mutex_destroy(&L.mu);
    return back;
}

/* --- statistical hot-edge view (mode 6): the ONLY rich TUI view that never
 * ptraces or single-steps its target. The tracer thread runs the IBS-Op sampler
 * OUT OF BAND, so this is the safe view on a live JIT. Each ~window_ms window's
 * hot-edge histogram REPLACES the table (recent hotness), which the UI can freeze
 * and scroll like the call-graph view. --- */
typedef struct {
    pthread_mutex_t mu;
    atomic_bool stop;
    atomic_bool finished;
    int rc;             /* engine return code (set before finished)   */
    sample_snap snap;   /* latest resolved hot-edge window            */
    int paused;         /* freeze the snapshot so scrolling is stable */
    pid_t pid;          /* target                                     */
    unsigned window_ms; /* per-window sample duration                 */
    const asmspy_symtab_t
        *syms;            /* ELF resolver (owned by caller)             */
    asmspy_jitmap_t *jit; /* JIT/perf-map resolver (owned by caller)    */
} sample_view_t;

static void sample_view_sink(void *ctx, const asmspy_sample_edge_t *edges,
                             size_t n, uint64_t samples,
                             uint64_t branch_samples, uint64_t lost,
                             int throttled) {
    sample_view_t *V = ctx;
    pthread_mutex_lock(&V->mu);
    if (!V->paused) /* while the user scrolls a frozen window, drop updates */
        sample_snap_set(&V->snap, edges, n, samples, branch_samples, lost,
                        throttled);
    pthread_mutex_unlock(&V->mu);
}

static void *sample_tracer(void *arg) {
    sample_view_t *V = arg;
    /* Loops surveying `window_ms` windows until *stop; unlike the ptrace engines
     * it is never blocked in waitpid, so no SIGALRM wake is needed — it notices
     * *stop within one window. */
    int rc = asmspy_engine_sample(V->pid, V->window_ms, &V->stop, V->syms,
                                  V->jit, sample_view_sink, V);
    pthread_mutex_lock(&V->mu);
    V->rc = rc;
    pthread_mutex_unlock(&V->mu);
    atomic_store(&V->finished, true);
    return NULL;
}

static int run_sample_view(pid_t pid, const char *title,
                           const asmspy_symtab_t *syms, asmspy_jitmap_t *jit) {
    sample_view_t V;
    memset(&V, 0, sizeof V);
    pthread_mutex_init(&V.mu, NULL);
    atomic_store(&V.stop, false);
    atomic_store(&V.finished, false);
    V.pid = pid;
    V.window_ms =
        250; /* responsive live windows; quit lands within one window */
    V.syms = syms;
    V.jit = jit;

    pthread_t th;
    if (pthread_create(&th, NULL, sample_tracer, &V) != 0) {
        pthread_mutex_destroy(&V.mu);
        return 1;
    }

    int back = 1; /* 1 = to process list (q/ESC), 0 = to options (b) */
    int top = 0;  /* first visible row while scrolling a frozen window */
    ssort_t sort = SSORT_COUNT;
    for (;;) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        int vis = rows - 4 > 0 ? rows - 4 : 1;
        erase();
        attron(A_BOLD);
        mvprintw(0, 0, "%.*s", cols, title);
        attroff(A_BOLD);

        pthread_mutex_lock(&V.mu);
        int fin = atomic_load(&V.finished);
        int erc = V.rc;
        int frozen = V.paused || fin;
        V.paused = frozen; /* stop the sink overwriting the frozen window */
        sample_sort_key = sort;
        qsort(V.snap.v, V.snap.n, sizeof *V.snap.v, sedge_cmp);
        int en = (int)V.snap.n;
        if (!frozen)
            top = 0; /* live view is top-anchored (hottest first) */
        if (top > en - vis)
            top = en - vis;
        if (top < 0)
            top = 0;
        attron(A_BOLD);
        mvprintw(1, 0,
                 "HOT EDGES  (AMD IBS-Op, out of band — safe on a JIT)   "
                 "sort: %s",
                 sort == SSORT_MISPRED ? "mispredicts" : "sample count");
        mvprintw(2, 0,
                 "%d edges   %llu/%llu branch/total samples%s%s   window %ums",
                 en, (unsigned long long)V.snap.branch_samples,
                 (unsigned long long)V.snap.samples,
                 V.snap.throttled ? "   THROTTLED" : "",
                 V.snap.lost ? "   (samples lost)" : "", V.window_ms);
        attroff(A_BOLD);
        if (en == 0)
            mvprintw(3, 0,
                     fin ? "(no taken-branch samples — target idle?)"
                         : "(sampling out of band — no ptrace, no "
                           "single-step…)");
        for (int r = 0; r < vis && top + r < en; r++) {
            char row[256];
            sample_format_row(row, sizeof row, &V.snap.v[top + r]);
            mvprintw(3 + r, 0, "%-*.*s", cols, cols, row);
        }
        pthread_mutex_unlock(&V.mu);

        if (fin && erc == ASMSPY_SAMPLE_UNAVAIL)
            mvprintw(rows - 1, 0,
                     "[IBS-Op unavailable: %.*s]   b: options   "
                     "q/ESC: processes",
                     cols - 40, asmtest_ibs_skip_reason());
        else if (fin)
            mvprintw(rows - 1, 0,
                     "[tracer stopped]   up/down PgUp/PgDn Home/End: scroll   "
                     "b: options   q/ESC: processes");
        else if (V.paused)
            mvprintw(
                rows - 1, 0,
                "[PAUSED]   up/down PgUp/PgDn Home/End: scroll   Tab: sort   "
                "space: live   b: options   q: processes");
        else
            mvprintw(rows - 1, 0,
                     "Tab: sort (count/mispredicts)   space: pause+scroll   "
                     "b: options   q/ESC: processes   (live)");
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
        if (ch == '\t')
            sort = sort == SSORT_COUNT ? SSORT_MISPRED : SSORT_COUNT;
        int page = vis > 1 ? vis - 1 : 1;
        int scroll = V.paused || fin; /* a finished window is already frozen */
        if ((ch == KEY_UP || ch == KEY_PPAGE) && !scroll) {
            V.paused = 1; /* scrolling into the list freezes the live re-sort */
            scroll = 1;
        }
        switch (ch) {
        case ' ':
        case 'p':
            if (!fin) {
                V.paused = !V.paused;
                if (!V.paused)
                    top = 0; /* resume live -> back to the hottest rows */
            }
            break;
        case KEY_UP:
            if (scroll)
                top -= 1;
            break;
        case KEY_DOWN:
            if (scroll)
                top += 1;
            break;
        case KEY_PPAGE:
            if (scroll)
                top -= page;
            break;
        case KEY_NPAGE:
            if (scroll)
                top += page;
            break;
        case KEY_HOME:
            if (scroll)
                top = 0;
            break;
        case KEY_END:
            if (scroll)
                top = INT_MAX; /* clamped to the last page in the renderer */
            break;
        default:
            break;
        }
        if (top < 0)
            top = 0;
    }

    atomic_store(&V.stop, true);
    pthread_join(th,
                 NULL); /* returns within one window (sampler checks stop) */
    free(V.snap.v);
    pthread_mutex_destroy(&V.mu);
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
            mvprintw(1, 0,
                     "! not owned by you — attach may fail "
                     "(needs CAP_SYS_PTRACE / ptrace_scope=0)");
        mvprintw(3, 2,
                 "1)  Syscall log        — live syscalls with data (a strace)");
        mvprintw(4, 2,
                 "2)  Assembly & funcs   — live disassembly + call-graph of a "
                 "function");
        mvprintw(5, 2,
                 "3)  Live stream        — every instruction as it runs "
                 "(function + assembly)");
        mvprintw(6, 2,
                 "4)  Call graph         — whole-process caller/callee counts "
                 "(sortable; Enter drills into a node)");
        mvprintw(7, 2,
                 "5)  Call tree          — whole-process live call tree "
                 "(indented by depth)");
        mvprintw(8, 2,
                 "6)  Process tree       — procs+threads+children with counts "
                 "(drill into a call graph)");
        mvprintw(9, 2,
                 "7)  Hot edges (sample) — statistical hot edges via AMD "
                 "IBS-Op, OUT OF BAND (safe on a JIT)");
        mvprintw(10, 2,
                 "8)  Process details    — runtime (JVM/.NET/py/Go/…), "
                 "threads, modules, ELF traits (safe, no attach)");
        mvprintw(rows - 1, 0, "1/2/3/4/5/6/7/8: choose   b/ESC: back");
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
        if (ch == '6')
            return 5;
        if (ch == '7')
            return 6;
        if (ch == '8')
            return 7;
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
                     "type: filter   Enter: trace   F3: reload   ESC: back   "
                     "(%d/%zu)",
                     L.nmatch, t->n);
            clrtoeol();
            refresh();
            int ch = getch();
            if (ch == 27)
                outcome = 2;
            else if (ch == KEY_F(3))
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

/* Sort process-array indices by pid ascending (roots/siblings in the tree). */
static const asmspy_proc_t *g_pr_for_cmp;
static int pr_pid_idx_cmp(const void *a, const void *b) {
    pid_t x = g_pr_for_cmp[*(const size_t *)a].pid;
    pid_t y = g_pr_for_cmp[*(const size_t *)b].pid;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Append process `k` and its descendants to items[]/order[] in tree order, drawn
 * with the same box glyphs as the --procs forest. `emitted` guards cycles; `*ni`
 * runs; order[row] maps a display row back to its index in `pr`. Children are the
 * processes whose ppid is this pid, by pid asc; the last gets the └─ elbow. */
static const char *seccomp_label(int s) {
    switch (s) {
    case 0:
        return "disabled";
    case 1:
        return "strict";
    case 2:
        return "filtered";
    default:
        return "unknown";
    }
}

/* Human-readable RSS from KiB. */
static void rss_human(unsigned long kb, char *out, size_t n) {
    if (kb >= 1024UL * 1024)
        snprintf(out, n, "%.1f GB", kb / (1024.0 * 1024.0));
    else if (kb >= 1024)
        snprintf(out, n, "%.1f MB", kb / 1024.0);
    else
        snprintf(out, n, "%lu KB", kb);
}

/* Draw the process-list detail strip (bottom of the picker): a separator naming
 * the selected pid + runtime badge, then a compact fingerprint — runtime,
 * threads/RSS/seccomp, thread names, notable modules, exe. Adapts to `h` rows
 * (separator + up to 5 content lines); the caller clears the screen each frame,
 * so no padding is needed. Draws nothing for h < 2 or a NULL selection. */
static void proc_details_render(int y0, int h, int cols, const asmspy_proc_t *p,
                                const asmspy_fingerprint_t *fp) {
    if (h < 2 || !p)
        return;
    char sep[96];
    int sl = snprintf(sep, sizeof sep, "── details: pid %d  [%s] ", (int)p->pid,
                      fp->runtime[0] ? fp->runtime : "?");
    attron(A_BOLD);
    mvprintw(y0, 0, "%.*s", cols, sep);
    for (int x = sl; x < cols; x++) /* extend the rule to the right margin */
        mvaddch(y0, x, ACS_HLINE);
    attroff(A_BOLD);

    int y = y0 + 1, last = y0 + h - 1;
    char line[600];

    char ev[100] = "", elf[64] = "";
    if (fp->evidence[0])
        snprintf(ev, sizeof ev, " (%s)", fp->evidence);
    if (fp->elf_class)
        snprintf(elf, sizeof elf, "   %d-bit %s %s", fp->elf_class,
                 fp->pie ? "PIE" : "non-PIE",
                 fp->static_linked ? "static" : "dynamic");
    if (y <= last) {
        snprintf(line, sizeof line, "runtime  %s%s   jit:%s%s", fp->runtime, ev,
                 fp->jitting ? "yes" : "no", elf);
        mvprintw(y++, 0, "%.*s", cols, line);
    }
    if (y <= last) {
        char rss[32];
        rss_human(fp->rss_kb, rss, sizeof rss);
        snprintf(line, sizeof line,
                 "threads  %-4d  rss %-10s  seccomp %-9s  tracer %s",
                 fp->threads, rss, seccomp_label(fp->seccomp),
                 fp->tracer_pid ? "YES" : "none");
        mvprintw(y++, 0, "%.*s", cols, line);
    }
    if (y <= last) {
        char tn[400] = "";
        for (int i = 0; i < fp->n_threadnames; i++) {
            if (i)
                strncat(tn, ", ", sizeof tn - strlen(tn) - 1);
            strncat(tn, fp->threadnames[i], sizeof tn - strlen(tn) - 1);
        }
        if (fp->more_threadnames)
            strncat(tn, ", …", sizeof tn - strlen(tn) - 1);
        snprintf(line, sizeof line, "names    %s", tn[0] ? tn : "(none)");
        mvprintw(y++, 0, "%.*s", cols, line);
    }
    if (y <= last) {
        char md[400] = "";
        for (int i = 0; i < fp->n_modules; i++) {
            if (i)
                strncat(md, ", ", sizeof md - strlen(md) - 1);
            strncat(md, fp->modules[i], sizeof md - strlen(md) - 1);
        }
        if (fp->more_modules)
            strncat(md, ", …", sizeof md - strlen(md) - 1);
        snprintf(line, sizeof line, "modules  %s",
                 md[0] ? md : "(none notable)");
        mvprintw(y++, 0, "%.*s", cols, line);
    }
    if (y <= last) {
        snprintf(line, sizeof line, "exe      %s",
                 fp->exe[0] ? fp->exe : "(unreadable — not owned by you?)");
        mvprintw(y++, 0, "%.*s", cols, line);
    }
}

static void picker_emit(const asmspy_proc_t *pr, size_t np, size_t k,
                        const char *prefix, int is_last, int is_root,
                        char **items, size_t *order, size_t *ni,
                        char *emitted) {
    if (emitted[k])
        return;
    emitted[k] = 1;
    char row[320];
    snprintf(row, sizeof row, "%s%s%-7d %c %-12.12s %-5s %.180s", prefix,
             is_root   ? ""
             : is_last ? TG_ELB
                       : TG_TEE,
             (int)pr[k].pid, pr[k].attachable ? ' ' : '!', pr[k].user,
             pr[k].runtime, pr[k].cmd);
    items[*ni] = strdup(row);
    order[*ni] = k;
    (*ni)++;

    char cpx[224];
    if (is_root)
        cpx[0] = '\0';
    else
        snprintf(cpx, sizeof cpx, "%s%s", prefix, is_last ? TG_GAP : TG_PIPE);

    size_t *ci = malloc((np ? np : 1) * sizeof *ci), nc = 0;
    if (ci) {
        for (size_t j = 0; j < np; j++)
            if (j != k && !emitted[j] && pr[j].ppid == pr[k].pid)
                ci[nc++] = j;
        g_pr_for_cmp = pr;
        qsort(ci, nc, sizeof *ci, pr_pid_idx_cmp);
        for (size_t j = 0; j < nc; j++)
            picker_emit(pr, np, ci[j], cpx, j + 1 == nc, 0, items, order, ni,
                        emitted);
        free(ci);
    }
}

/* Build the tree-ordered picker rows: a root (a process whose parent is not in
 * the list) then its descendants, repeated. Returns the row count (== np; every
 * process appears exactly once). items[]/order[] must hold >= np entries. */
static size_t picker_build_tree(const asmspy_proc_t *pr, size_t np,
                                char **items, size_t *order) {
    char *emitted = calloc(np ? np : 1, 1);
    if (!emitted)
        return 0;
    size_t ni = 0, *roots = malloc((np ? np : 1) * sizeof *roots), nr = 0;
    if (roots) {
        for (size_t k = 0; k < np; k++) {
            int parent_listed = 0;
            for (size_t j = 0; j < np; j++)
                if (pr[j].pid == pr[k].ppid) {
                    parent_listed = 1;
                    break;
                }
            if (!parent_listed)
                roots[nr++] = k;
        }
        g_pr_for_cmp = pr;
        qsort(roots, nr, sizeof *roots, pr_pid_idx_cmp);
        for (size_t j = 0; j < nr; j++)
            picker_emit(pr, np, roots[j], "", 0, 1, items, order, &ni, emitted);
        for (size_t k = 0; k < np; k++) /* orphaned parent chains -> roots */
            if (!emitted[k])
                picker_emit(pr, np, k, "", 0, 1, items, order, &ni, emitted);
        free(roots);
    }
    free(emitted);
    return ni;
}

/* process picker; Tab cycles the (flat) sort, F2 toggles flat<->process-tree,
 * F3 refreshes. Returns 0 and fills *picked on selection, -1 on quit. */
static int screen_procs(asmspy_proc_t *picked) {
    asmspy_sort_t sort = ASMSPY_SORT_PID;
    int view = 0; /* 0 = flat sorted list, 1 = process tree */
    for (;
         ;) { /* (re-)scan loop — Tab re-sorts, F2 toggles tree, F3 refreshes */
        asmspy_proc_t *procs = NULL;
        size_t np = 0;
        /* the tree re-orders by pid/ppid, so it only needs the cheap pid scan */
        if (asmspy_proclist(&procs, &np, view ? ASMSPY_SORT_PID : sort) < 0)
            return -1;
        char **items = malloc((np ? np : 1) * sizeof *items);
        size_t *order = malloc((np ? np : 1) * sizeof *order);
        if (!items || !order) {
            free(items);
            free(order);
            free(procs);
            return -1;
        }
        size_t nitems;
        if (view) {
            nitems = picker_build_tree(procs, np, items, order);
        } else {
            nitems = np;
            for (size_t i = 0; i < np; i++) {
                char b[256];
                if (sort == ASMSPY_SORT_SCAN)
                    snprintf(b, sizeof b,
                             "%-7d %4u %5llu %c %-12.12s %-5s %.150s",
                             procs[i].pid, procs[i].scan, procs[i].cpu,
                             procs[i].attachable ? ' ' : '!', procs[i].user,
                             procs[i].runtime, procs[i].cmd);
                else if (sort == ASMSPY_SORT_ACTIVE)
                    snprintf(b, sizeof b, "%-7d %6llu %c %-12.12s %-5s %.170s",
                             procs[i].pid, procs[i].cpu,
                             procs[i].attachable ? ' ' : '!', procs[i].user,
                             procs[i].runtime, procs[i].cmd);
                else
                    snprintf(b, sizeof b, "%-7d %c %-12.12s %-5s %.200s",
                             procs[i].pid, procs[i].attachable ? ' ' : '!',
                             procs[i].user, procs[i].runtime, procs[i].cmd);
                items[i] = strdup(b);
                order[i] = i;
            }
        }
        List L;
        if (list_init(&L, items, (int)nitems) != 0) {
            for (size_t i = 0; i < nitems; i++)
                free(items[i]);
            free(items);
            free(order);
            free(procs);
            return -1;
        }

        int outcome = 0; /* 1 pick, 2 quit, 3 toggle, 4 refresh */
        int selidx = -1;
        /* fingerprint cache for the bottom detail strip: recomputed when the
         * highlighted pid changes, and periodically so RSS/threads stay live */
        asmspy_fingerprint_t fp;
        memset(&fp, 0, sizeof fp);
        int fp_pid = -1, refresh_tick = 0;
        while (outcome == 0) {
            int rows, cols;
            getmaxyx(stdscr, rows, cols);
            /* reserve a bottom strip for the selected process's details */
            int detail_h = 6;
            if (detail_h > rows - 6)
                detail_h =
                    rows - 6; /* keep the list usable on a short screen */
            if (detail_h < 0)
                detail_h = 0;
            int list_h = rows - 3 - detail_h;
            if (list_h < 1)
                list_h = 1;
            erase();
            attron(A_BOLD);
            const char *sortname =
                sort == ASMSPY_SORT_SCAN
                    ? "string-scan (alnum density, then recent)  [PID STR CPU]"
                : sort == ASMSPY_SORT_ACTIVE
                    ? "recent activity (CPU jiffies)  [PID CPU]"
                    : "pid";
            if (view)
                mvprintw(
                    0, 0,
                    "asmspy — select a process   [process tree]   (%zu; '!' "
                    "= not yours)",
                    np);
            else
                mvprintw(0, 0,
                         "asmspy — select a process   [sort: %s]   (%zu; '!' = "
                         "not yours)",
                         sortname, np);
            attroff(A_BOLD);
            list_render(&L, 1, list_h, cols);

            /* fingerprint the highlighted process for the detail strip */
            const asmspy_proc_t *selp = NULL;
            if (L.nmatch > 0 && L.sel >= 0 && L.sel < L.nmatch)
                selp = &procs[order[L.match[L.sel]]];
            if (detail_h >= 2 && selp &&
                ((int)selp->pid != fp_pid || refresh_tick <= 0)) {
                asmspy_fingerprint(selp->pid, &fp);
                fp_pid = (int)selp->pid;
                refresh_tick = 2; /* ~2 idle ticks (≈2s) between refreshes */
            }
            if (detail_h >= 2)
                proc_details_render(rows - 1 - detail_h, detail_h, cols, selp,
                                    &fp);

            mvprintw(rows - 1, 0,
                     "type: filter   Enter: select   Tab: sort   F2: %s   F3: "
                     "refresh   q: quit",
                     view ? "flat list" : "tree");
            clrtoeol();
            refresh();
            timeout(1000); /* poll so the detail strip refreshes while idle */
            int ch = getch();
            refresh_tick--;
            if (ch == ERR)
                continue; /* idle tick: redraw (refreshes the detail strip) */
            if ((ch == 'q' && L.flen == 0) || ch == 27)
                outcome = 2;
            else if (ch == '\t')
                outcome = 3;
            else if (ch == KEY_F(2))
                outcome = 5; /* toggle flat list <-> process tree */
            else if (ch == KEY_F(3))
                outcome = 4; /* re-scan (re-samples CPU in activity sort) */
            else {
                int sel = list_key(&L, ch, list_h);
                if (sel >= 0) {
                    selidx = sel;
                    outcome = 1;
                }
            }
        }

        asmspy_proc_t chosen;
        int have = (outcome == 1);
        if (have)
            chosen = procs[order[selidx]]; /* row -> proc (tree reorders) */
        list_done(&L);
        for (size_t i = 0; i < nitems; i++)
            free(items[i]);
        free(items);
        free(order);
        free(procs);

        if (outcome == 1) {
            *picked = chosen;
            return 0;
        }
        if (outcome == 2)
            return -1;
        if (outcome == 3) /* cycle pid -> active -> scan; outcome 4 keeps it */
            sort = (asmspy_sort_t)((sort + 1) % 3);
        if (outcome == 5) /* flat list <-> process tree */
            view = !view;
        /* outcome 3/4/5: fall through -> outer loop re-scans */
    }
}

/* Per-process details view (menu 8): what kind of process this is — runtime,
 * threads, notable modules, ELF traits — all from /proc + the mapped ELF, so it
 * NEVER attaches/ptraces and is safe on any target (even a JIT). Re-fingerprints
 * on a ~1s timer so threads/RSS/JIT status track the live process. Returns 0 to
 * the options menu (b), 1 to the process list (q/ESC). */
static int run_details_view(pid_t pid, const char *title) {
    int back = 1, top = 0, tick = 0, have = 0, vis = 1;
    asmspy_fingerprint_t fp;
    memset(&fp, 0, sizeof fp);
    for (;;) {
        if (!have || tick <= 0) { /* (re)fingerprint on entry and every ~1s */
            asmspy_fingerprint(pid, &fp);
            have = 1;
            tick = 8; /* 8 * 125ms getch timeout ≈ 1s */
        }
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        erase();
        attron(A_BOLD);
        mvprintw(0, 0, "%.*s", cols, title);
        attroff(A_BOLD);

        int y = 2;
        attron(A_BOLD);
        mvprintw(y, 0, "Runtime:  %s", fp.runtime);
        attroff(A_BOLD);
        if (fp.evidence[0])
            mvprintw(y, 12 + (int)strlen(fp.runtime), "(%s)", fp.evidence);
        y++;
        if (fp.jitting)
            mvprintw(y++, 10,
                     "actively JIT-compiling — /tmp/perf-%d.map present",
                     (int)pid);
        y++;

        if (fp.exe[0])
            mvprintw(y++, 0, "Exe:      %.*s", cols - 11, fp.exe);
        else
            mvprintw(y++, 0, "Exe:      (unreadable — not owned by you?)");

        char elf[176];
        if (fp.elf_class) {
            int o =
                snprintf(elf, sizeof elf, "%d-bit, %s, %s", fp.elf_class,
                         fp.pie ? "PIE" : "non-PIE",
                         fp.static_linked ? "statically linked" : "dynamic");
            if (!fp.static_linked && fp.interp[0] && o > 0 &&
                o < (int)sizeof elf)
                snprintf(elf + o, sizeof elf - (size_t)o, " (loader %s)",
                         fp.interp);
            mvprintw(y++, 0, "ELF:      %.*s", cols - 11, elf);
        } else {
            mvprintw(y++, 0, "ELF:      (exe not readable)");
        }

        char rss[32];
        rss_human(fp.rss_kb, rss, sizeof rss);
        mvprintw(y++, 0, "Threads:  %-6d   RSS: %-10s   Seccomp: %s",
                 fp.threads, rss, seccomp_label(fp.seccomp));
        mvprintw(y++, 0, "TracerPid: %d%s", (int)fp.tracer_pid,
                 fp.tracer_pid ? "   (already being traced by another process!)"
                               : "   (not currently traced)");

        /* thread names on one line — a managed runtime self-identifies here */
        char tn[512] = "";
        for (int i = 0; i < fp.n_threadnames; i++) {
            if (i)
                strncat(tn, ", ", sizeof tn - strlen(tn) - 1);
            strncat(tn, fp.threadnames[i], sizeof tn - strlen(tn) - 1);
        }
        if (fp.more_threadnames)
            strncat(tn, ", …", sizeof tn - strlen(tn) - 1);
        mvprintw(y++, 0, "Threads named: %.*s", cols - 15,
                 tn[0] ? tn : "(none readable)");

        y++;
        attron(A_BOLD);
        mvprintw(y++, 0, "Notable modules (%d%s):", fp.n_modules,
                 fp.more_modules ? "+" : "");
        attroff(A_BOLD);
        int listrow = y;
        vis = rows - 1 - listrow;
        if (vis < 1)
            vis = 1;
        if (top > fp.n_modules - vis)
            top = fp.n_modules - vis;
        if (top < 0)
            top = 0;
        if (fp.n_modules == 0)
            mvprintw(listrow, 2, "(only ubiquitous libs, or none mapped)");
        for (int i = 0; i < vis && top + i < fp.n_modules; i++)
            mvprintw(listrow + i, 2, "%.*s", cols - 2, fp.modules[top + i]);

        mvprintw(rows - 1, 0,
                 "up/down: scroll modules   b: options   q/ESC: processes   "
                 "(live)");
        clrtoeol();
        refresh();

        timeout(125);
        int ch = getch();
        tick--;
        if (ch == 'q' || ch == 27) {
            back = 1;
            break;
        }
        if (ch == 'b') {
            back = 0;
            break;
        }
        switch (ch) {
        case KEY_UP:
            top -= 1;
            break;
        case KEY_DOWN:
            top += 1;
            break;
        case KEY_PPAGE:
            top -= vis > 1 ? vis - 1 : 1;
            break;
        case KEY_NPAGE:
            top += vis > 1 ? vis - 1 : 1;
            break;
        case KEY_HOME:
            top = 0;
            break;
        case KEY_END:
            top = INT_MAX; /* clamped above on next draw */
            break;
        default:
            break;
        }
        if (top < 0)
            top = 0;
    }
    return back;
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

        for (
            ;
            ;) { /* per-process options loop ('b' in a live view returns here) */
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
            } else if (mode == 5) {
                /* process/thread topology; Tab toggles the count, Enter drills
                 * into the selected node's call graph (topology detaches first) */
                asmspy_count_t cmode = ASMSPY_COUNT_SYSCALLS;
                nav = 0;
                for (;;) {
                    char title[160];
                    snprintf(title, sizeof title,
                             "asmspy — process tree of pid %d (%.*s)",
                             picked.pid, 40, picked.cmd);
                    int action = TOPO_ACT_NONE;
                    pid_t drill = 0;
                    nav = run_topo_view(picked.pid, cmode, title, &action,
                                        &drill);
                    if (action == TOPO_ACT_TOGGLE) {
                        cmode = cmode == ASMSPY_COUNT_SYSCALLS
                                    ? ASMSPY_COUNT_CALLS
                                    : ASMSPY_COUNT_SYSCALLS;
                        continue;
                    }
                    if (action == TOPO_ACT_DRILL && drill > 0) {
                        asmspy_symtab_t t;
                        asmspy_symtab_load(drill, &t);
                        char dt[128];
                        snprintf(dt, sizeof dt,
                                 "asmspy — call graph of pid %d (drill-in)",
                                 (int)drill);
                        run_live_view(drill, 3, 0, 0, dt, &t);
                        asmspy_symtab_free(&t);
                        continue; /* back to the topology */
                    }
                    break; /* b / q */
                }
            } else if (mode == 6) {
                /* statistical hot edges via AMD IBS-Op, OUT OF BAND (no ptrace /
                 * single-step) — the safe rich view on a live JIT. Gate up front
                 * so an IBS-less host gets a clear message, not an empty pane. */
                if (!asmtest_ibs_available()) {
                    erase();
                    mvprintw(0, 0,
                             "hot-edge sampling needs an AMD IBS-Op host — %s",
                             asmtest_ibs_skip_reason());
                    mvprintw(2, 0, "press a key");
                    refresh();
                    getch();
                    nav = 0; /* back to options */
                } else {
                    asmspy_symtab_t t;
                    asmspy_symtab_load(picked.pid, &t); /* raw addrs if empty */
                    asmspy_jitmap_t jit;
                    asmspy_jitmap_init(&jit, picked.pid);
                    asmspy_jitmap_refresh(
                        &jit); /* name already-JITted methods */
                    char title[128];
                    snprintf(title, sizeof title,
                             "asmspy — hot edges of pid %d (%.*s)", picked.pid,
                             40, picked.cmd);
                    nav = run_sample_view(picked.pid, title, &t, &jit);
                    asmspy_jitmap_free(&jit);
                    asmspy_symtab_free(&t);
                }
            } else if (mode == 7) {
                /* process details: /proc + ELF only, no attach (safe on a JIT) */
                char title[160];
                snprintf(title, sizeof title,
                         "asmspy — details of pid %d (%.*s)", picked.pid, 60,
                         picked.cmd);
                nav = run_details_view(picked.pid, title);
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
    fprintf(
        stderr,
        "asmspy — watch a running process out of band\n\n"
        "  %s                         interactive TUI\n"
        "  %s --list [active|scan]    list processes (active=recent CPU; "
        "scan=string-rich memory)\n"
        "  %s --syms   <pid> [filter] list resolved function symbols\n"
        "  %s --log    <pid> [n]      stream n syscalls with data\n"
        "  %s --trace  <pid> <sym|0xADDR[:LEN]> [n]  live samples of a "
        "function/region\n"
        "  %s --stream <pid> [n] [--tid=<t>]  stream n instructions live "
        "(function + asm)\n"
        "  %s --graph  <pid> [n] [--sort=invocations|fanout] [--json|--dot] "
        "[--tid=<t>]  whole-process call graph over n calls\n"
        "  %s --tree   <pid> [n] [--json|--dot] [--tid=<t>]  whole-process "
        "live call tree, indented by depth (n call lines)\n"
        "                                   (--tid=<t> traces only thread t; "
        "others run full speed)\n"
        "  %s --procs  <pid> [n] [--count=syscalls|calls]  process/thread tree "
        "(procs+threads+children) with counts\n"
        "  %s --sample <pid> [ms] [--json]  statistical hot edges via AMD "
        "IBS-Op, OUT OF BAND — safe on a JIT (no single-step); needs an AMD "
        "IBS host\n"
        "\n"
        "A negative n runs until the target exits or you interrupt (Ctrl-C).\n"
        "Note: the single-step views deliver a target's OWN int3 breakpoints\n"
        "faithfully, which suspends fine-grained stepping of that thread "
        "until\n"
        "its next stop — so a target that uses software breakpoints (a JIT or\n"
        "debugger) may never reach a fixed n in batch mode; interrupt with "
        "Ctrl-C.\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
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
        pid_t tid = 0;
        for (int i = 3; i < argc; i++) { /* [n] and --tid= in any order */
            if (strncmp(argv[i], "--tid=", 6) == 0) {
                if (parse_pid(argv[i] + 6, &tid) != 0)
                    return bad_arg("tid", argv[i] + 6);
            } else if (parse_count(argv[i], &n) != 0) {
                return bad_arg("count", argv[i]);
            }
        }
        return cmd_stream(pid, tid, n);
    }
    if (strcmp(argv[1], "--tree") == 0 && argc >= 3) {
        if (parse_pid(argv[2], &pid) != 0)
            return bad_arg("pid", argv[2]);
        n = 40;
        pid_t tid = 0;
        int json = 0, dot = 0;
        for (int i = 3; i < argc; i++) { /* [n], --json/--dot, --tid= */
            if (strncmp(argv[i], "--tid=", 6) == 0) {
                if (parse_pid(argv[i] + 6, &tid) != 0)
                    return bad_arg("tid", argv[i] + 6);
            } else if (strcmp(argv[i], "--json") == 0) {
                json = 1;
            } else if (strcmp(argv[i], "--dot") == 0) {
                dot = 1;
            } else if (parse_count(argv[i], &n) != 0) {
                return bad_arg("count", argv[i]);
            }
        }
        return cmd_tree(pid, tid, n, json, dot);
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
        int json = 0, dot = 0;
        pid_t tid = 0;
        for (int i = 3; i < argc;
             i++) { /* [n], --sort=, --json/--dot, --tid= */
            if (strncmp(argv[i], "--sort=", 7) == 0) {
                const char *v = argv[i] + 7;
                if (strcmp(v, "invocations") == 0)
                    sort = GSORT_INVOCATIONS;
                else if (strcmp(v, "fanout") == 0 ||
                         strcmp(v, "functions-called") == 0)
                    sort = GSORT_FANOUT;
                else
                    return bad_arg("sort (want 'invocations' or 'fanout')", v);
            } else if (strcmp(argv[i], "--json") == 0) {
                json = 1;
            } else if (strcmp(argv[i], "--dot") == 0) {
                dot = 1;
            } else if (strncmp(argv[i], "--tid=", 6) == 0) {
                if (parse_pid(argv[i] + 6, &tid) != 0)
                    return bad_arg("tid", argv[i] + 6);
            } else if (parse_count(argv[i], &n) != 0) {
                return bad_arg("count", argv[i]);
            }
        }
        return cmd_graph(pid, tid, n, sort, json, dot);
    }
    if (strcmp(argv[1], "--sample") == 0 && argc >= 3) {
        if (parse_pid(argv[2], &pid) != 0)
            return bad_arg("pid", argv[2]);
        long ms = 300; /* sample window in milliseconds */
        int json = 0;
        for (int i = 3; i < argc; i++) { /* [ms] and --json in any order */
            if (strcmp(argv[i], "--json") == 0)
                json = 1;
            else if (parse_count(argv[i], &ms) != 0 || ms <= 0)
                return bad_arg("milliseconds (positive)", argv[i]);
        }
        return cmd_sample(pid, ms, json);
    }
    return usage(argv[0]);
}
