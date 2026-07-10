/*
 * asmspy.c — a small ncurses front-end over the asm-test out-of-process tracer.
 *
 * Pick a running process, then watch either its live syscalls (with data) or a
 * live disassembly + call-graph of a chosen function — all out of band.
 *
 * Interactive:   asmspy
 * Headless (scriptable / CI smoke), sharing the same engine:
 *   asmspy --list                     list attachable processes
 *   asmspy --syms  <pid> [filter]     list resolved function symbols
 *   asmspy --log   <pid> [n]          stream n syscalls with data (a mini strace)
 *   asmspy --trace <pid> <sym> [n]    n live samples: disassembly + functions called
 */
#define _GNU_SOURCE
#include <ctype.h>
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

    /* distinct instruction offsets, ascending */
    size_t ni = (size_t)recorded;
    uint64_t *offs = ni ? malloc(ni * sizeof *offs) : NULL;
    size_t nd = 0;
    if (offs) {
        for (size_t i = 0; i < ni; i++)
            offs[i] = asmtest_emu_trace_insn_at(tr, i);
        qsort(offs, ni, sizeof *offs, u64cmp);
        for (size_t i = 0; i < ni; i++)
            if (i == 0 || offs[i] != offs[i - 1])
                offs[nd++] = offs[i];
    }
    for (size_t i = 0; i < nd && i < MAX_DISASM; i++) {
        char line[200], dbuf[160] = "";
        if (code && asmtest_disas_available())
            asmtest_disas(ASMTEST_ARCH_X86_64, code, len, base, offs[i], dbuf,
                          sizeof dbuf);
        if (dbuf[0])
            snprintf(line, sizeof line, "+0x%-4llx  %s",
                     (unsigned long long)offs[i], dbuf);
        else
            snprintf(line, sizeof line, "+0x%llx", (unsigned long long)offs[i]);
        svec_push(dis, line);
    }
    if (nd > MAX_DISASM) {
        char more[64];
        snprintf(more, sizeof more, "... (+%zu more)", nd - MAX_DISASM);
        svec_push(dis, more);
    }
    free(offs);

    /* distinct call edges -> resolved callee names */
    size_t ne = asmtest_descent_edges_len(desc);
    uint64_t seen[MAX_EDGES];
    size_t nseen = 0;
    for (size_t i = 0; i < ne && nseen < MAX_EDGES; i++) {
        uint64_t tgt = asmtest_descent_edge_target(desc, i);
        uint64_t site = asmtest_descent_edge_site(desc, i);
        int dup = 0;
        for (size_t k = 0; k < nseen; k++)
            if (seen[k] == tgt) {
                dup = 1;
                break;
            }
        if (dup)
            continue;
        seen[nseen++] = tgt;

        char line[220];
        const asmspy_sym_t *s = syms ? asmspy_symtab_at(syms, tgt) : NULL;
        if (s) {
            uint64_t delta = tgt - s->addr;
            if (delta)
                snprintf(line, sizeof line, "+0x%-4llx  ->  %s+0x%llx  [%s]",
                         (unsigned long long)site, s->name,
                         (unsigned long long)delta, s->module);
            else
                snprintf(line, sizeof line, "+0x%-4llx  ->  %s  [%s]",
                         (unsigned long long)site, s->name, s->module);
        } else {
            snprintf(line, sizeof line, "+0x%-4llx  ->  0x%llx",
                     (unsigned long long)site, (unsigned long long)tgt);
        }
        svec_push(fn, line);
    }
    if (asmtest_descent_truncated(desc))
        svec_push(fn, "(call record truncated)");
}

/* ================================================================== */
/* Headless subcommands                                                */
/* ================================================================== */

static int cmd_list(void) {
    asmspy_proc_t *v = NULL;
    size_t n = 0;
    if (asmspy_proclist(&v, &n) < 0) {
        fprintf(stderr, "cannot read /proc\n");
        return 1;
    }
    printf("%-8s %-12s %-4s %s\n", "PID", "USER", "ATT", "COMMAND");
    for (size_t i = 0; i < n; i++)
        printf("%-8d %-12.12s %-4s %.80s\n", v[i].pid, v[i].user,
               v[i].attachable ? "yes" : "-", v[i].cmd);
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

static void log_print_sink(void *ctx, const char *line) {
    (void)ctx;
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

/* Resolve a "<symbol>" or "0x<addr>" argument to a (base,len) region. */
static int resolve_region(const asmspy_symtab_t *t, const char *arg,
                          uint64_t *base, size_t *len) {
    if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
        uint64_t a = strtoull(arg, NULL, 16);
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
        fprintf(stderr, "no sized function symbol '%s' in pid %d\n", sym,
                (int)pid);
        asmspy_symtab_free(&t);
        return 1;
    }
    fprintf(stderr, "tracing %s @ 0x%llx (%zu bytes) in pid %d\n", sym,
            (unsigned long long)base, len, (int)pid);
    int rc = asmspy_engine_region(pid, base, len, n, NULL, region_print_sink,
                                  &t);
    if (rc != 0) {
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
#define LINE_MAX 256

typedef struct {
    char lines[LOG_CAP][LINE_MAX];
    int head, count;
} logbuf;

static void log_push(logbuf *lg, const char *s) {
    snprintf(lg->lines[lg->head], LINE_MAX, "%s", s);
    lg->head = (lg->head + 1) % LOG_CAP;
    if (lg->count < LOG_CAP)
        lg->count++;
}

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
    /* syscall log */
    logbuf log;
    /* region view */
    char asm_header[256];
    svec asm_dis, asm_fn;
    unsigned asm_sample;
    const asmspy_symtab_t *syms;
    /* target */
    pid_t pid;
    uint64_t base;
    size_t len;
    int mode; /* 0 = syscalls, 1 = region */
} live_t;

static void live_syscall_sink(void *ctx, const char *line) {
    live_t *L = ctx;
    pthread_mutex_lock(&L->mu);
    log_push(&L->log, line);
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

static void *tracer_thread(void *arg) {
    live_t *L = arg;
    int rc;
    if (L->mode == 0)
        rc = asmspy_engine_syscalls(L->pid, -1, &L->stop, live_syscall_sink, L);
    else
        rc = asmspy_engine_region(L->pid, L->base, L->len, -1, &L->stop,
                                  live_region_sink, L);
    pthread_mutex_lock(&L->mu);
    L->rc = rc;
    pthread_mutex_unlock(&L->mu);
    atomic_store(&L->finished, true);
    return NULL;
}

/* Run a live view (mode 0 syscalls / mode 1 region) until the user quits. */
static void run_live_view(pid_t pid, int mode, uint64_t base, size_t len,
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
        return;
    }

    for (;;) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        erase();
        attron(A_BOLD);
        mvprintw(0, 0, "%.*s", cols, title);
        attroff(A_BOLD);

        pthread_mutex_lock(&L.mu);
        int fin = atomic_load(&L.finished);
        int erc = L.rc;
        if (mode == 0) {
            int h = rows - 3;
            int show = L.log.count < h ? L.log.count : h;
            int start = L.log.count - show;
            for (int r = 0; r < show; r++) {
                int logical = start + r;
                int slot = ((L.log.head - L.log.count + logical) % LOG_CAP +
                            LOG_CAP) % LOG_CAP;
                mvprintw(2 + r, 0, "%-*.*s", cols, cols, L.log.lines[slot]);
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

        if (fin) {
            char e[128];
            asmspy_strerror(erc, e, sizeof e);
            mvprintw(rows - 1, 0,
                     "[tracer stopped: %s]  press any key to return",
                     erc ? e : "target exited or done");
        } else {
            mvprintw(rows - 1, 0, "q/ESC: back   (live)");
        }
        clrtoeol();
        refresh();

        timeout(120);
        int ch = getch();
        if (ch == 'q' || ch == 27 || (fin && ch != ERR))
            break;
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
    pthread_mutex_destroy(&L.mu);
}

/* mode-select screen; returns 0 syscalls, 1 region, -1 back */
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
        mvprintw(rows - 1, 0, "1/2: choose   b/ESC: back");
        refresh();
        int ch = getch();
        if (ch == '1')
            return 0;
        if (ch == '2')
            return 1;
        if (ch == 'b' || ch == 27 || ch == 'q')
            return -1;
    }
}

/* symbol picker; returns 0 and fills the region on selection, -1 on back */
static int screen_syms(pid_t pid, uint64_t *base, size_t *len,
                       const asmspy_symtab_t *t) {
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

    int result = -1;
    for (;;) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);
        erase();
        attron(A_BOLD);
        mvprintw(0, 0, "asmspy — pick a function to trace (pid %d)", (int)pid);
        attroff(A_BOLD);
        list_render(&L, 1, rows - 3, cols);
        mvprintw(rows - 1, 0,
                 "type: filter   Enter: trace   ESC: back   (%d/%zu)", L.nmatch,
                 t->n);
        clrtoeol();
        refresh();
        int ch = getch();
        if (ch == 27)
            break;
        int sel = list_key(&L, ch, rows - 3);
        if (sel >= 0) {
            if (t->v[sel].size == 0)
                continue; /* need a sized region */
            *base = t->v[sel].addr;
            *len = t->v[sel].size;
            result = 0;
            break;
        }
    }
    list_done(&L);
    for (size_t i = 0; i < t->n; i++)
        free(items[i]);
    free(items);
    return result;
}

int asmspy_tui(void) {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    for (;;) {
        /* ---- process picker ---- */
        asmspy_proc_t *procs = NULL;
        size_t np = 0;
        if (asmspy_proclist(&procs, &np) < 0) {
            endwin();
            fprintf(stderr, "cannot read /proc\n");
            return 1;
        }
        char **items = malloc((np ? np : 1) * sizeof *items);
        if (!items) {
            free(procs);
            continue; /* transient OOM — retry the picker */
        }
        for (size_t i = 0; i < np; i++) {
            char b[256];
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
            continue;
        }

        int chosen = -1;
        for (;;) {
            int rows, cols;
            getmaxyx(stdscr, rows, cols);
            erase();
            attron(A_BOLD);
            mvprintw(0, 0, "asmspy — select a process    (%zu; '!' = not "
                           "owned by you)",
                     np);
            attroff(A_BOLD);
            list_render(&L, 1, rows - 3, cols);
            mvprintw(rows - 1, 0, "type: filter   Enter: select   q: quit");
            clrtoeol();
            refresh();
            int ch = getch();
            if (ch == 'q' && L.flen == 0) {
                chosen = -2;
                break;
            }
            if (ch == 27) {
                chosen = -2;
                break;
            }
            int sel = list_key(&L, ch, rows - 3);
            if (sel >= 0) {
                chosen = sel;
                break;
            }
        }
        list_done(&L);

        asmspy_proc_t picked;
        int have = 0;
        if (chosen >= 0) {
            picked = procs[chosen];
            have = 1;
        }
        for (size_t i = 0; i < np; i++)
            free(items[i]);
        free(items);
        free(procs);
        if (!have)
            break; /* quit */

        /* ---- mode select ---- */
        int mode = screen_mode(&picked);
        if (mode < 0)
            continue; /* back to process list */

        if (mode == 0) {
            char title[128];
            snprintf(title, sizeof title, "asmspy — syscalls of pid %d (%.*s)",
                     picked.pid, 40, picked.cmd);
            run_live_view(picked.pid, 0, 0, 0, title, NULL);
        } else {
            asmspy_symtab_t t;
            if (asmspy_symtab_load(picked.pid, &t) < 0 || t.n == 0) {
                /* brief message, then back */
                erase();
                mvprintw(0, 0, "no function symbols for pid %d (stripped? "
                               "permission?) — press a key",
                         picked.pid);
                refresh();
                getch();
                asmspy_symtab_free(&t);
                continue;
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
                run_live_view(picked.pid, 1, base, len, title, &t);
            }
            asmspy_symtab_free(&t);
        }
    }

    endwin();
    return 0;
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

static int usage(const char *argv0) {
    fprintf(stderr,
            "asmspy — watch a running process out of band\n\n"
            "  %s                         interactive TUI\n"
            "  %s --list                  list attachable processes\n"
            "  %s --syms  <pid> [filter]  list resolved function symbols\n"
            "  %s --log   <pid> [n]       stream n syscalls with data\n"
            "  %s --trace <pid> <sym> [n] n live samples of a function\n",
            argv0, argv0, argv0, argv0, argv0);
    return 2;
}

int main(int argc, char **argv) {
    if (argc == 1)
        return asmspy_tui();
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)
        return usage(argv[0]);
    if (strcmp(argv[1], "--list") == 0)
        return cmd_list();
    if (strcmp(argv[1], "--syms") == 0 && argc >= 3)
        return cmd_syms((pid_t)atoi(argv[2]), argc >= 4 ? argv[3] : NULL);
    if (strcmp(argv[1], "--log") == 0 && argc >= 3)
        return cmd_log((pid_t)atoi(argv[2]), argc >= 4 ? atol(argv[3]) : 20);
    if (strcmp(argv[1], "--trace") == 0 && argc >= 4)
        return cmd_trace((pid_t)atoi(argv[2]), argv[3],
                         argc >= 5 ? atol(argv[4]) : 3);
    return usage(argv[0]);
}
