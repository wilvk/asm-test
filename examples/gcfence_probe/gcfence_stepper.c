/*
 * gcfence_stepper.c — the TRACER half of the F4 GC-fence FREEZE probe.
 *
 * A deliberately MINIMAL standalone ptrace single-stepper: it attaches to ONE thread of a live
 * .NET victim (the managed hot-loop worker), drives PTRACE_SINGLESTEP in a loop, and publishes a
 * step counter — "how many in-region instructions the tracer has recorded so far", exactly what
 * asmtest_gcmove_t.step indexes — into a POSIX-shm channel the attached CLR profiler samples at
 * both ends of every GC / EE-suspend window (examples/gcfence_probe/gcfenceprof.cpp).
 *
 * It does NOT drag in src/dataflow_ptrace.c: the whole tier brings a value trace, region gating,
 * JIT method resolution and signal policy, none of which the question needs. The question is only
 * "does the step counter advance across a GC fence", so the probe carries the smallest thing that
 * can produce an honest counter.
 *
 * Three things beyond the counter, each of which turned out to matter:
 *  - BURSTS. It steps in K attach/step/detach cycles, each begun only when NO GC window is open, so
 *    that a fence OPENS while the tracer is live (traced=1) and S0 is a real sample rather than a
 *    pre-tracer zero. After each detach it waits before draining, because a window that could not
 *    close while we were stepping closes right after we let go — and its record is the measurement.
 *  - /proc SAMPLING. A watcher thread polls /proc/<pid>/task/<tid>/{stat,wchan,syscall} while the
 *    profiler holds fence_active / ee_suspended, which answers WHY: parked in a futex (nothing can
 *    retire — the freeze holds for a reason) vs. running (instructions retire).
 *  - RIP SAMPLING. Every GCFENCE_RIP_EVERY steps it does a PTRACE_GETREGS and, at the end, resolves
 *    the sampled RIPs against /proc/<pid>/maps. "The thread retired 4M instructions inside a GC
 *    window" is a fact; "and every one of them was in libcoreclr" is the explanation.
 *
 * usage: gcfence_stepper <pid> <tid> <seconds-per-burst> [bursts]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gcfence_shm.h"

#define GCFENCE_RIP_EVERY 4096   /* PTRACE_GETREGS cadence, in steps */
#define GCFENCE_RIP_SLOTS 24
#define GCFENCE_MAPS_MAX 1024

static gcfence_channel_t *g_ch;
static volatile int g_watch_stop;
static int g_pid, g_tid;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void sleep_ms(unsigned ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* ---- the shm channel ------------------------------------------------------------------- */
/* Both sides open O_CREAT; the profiler usually wins (it is attached FIRST, because profiler
 * attach travels the diagnostics IPC socket, which the runtime must be RUNNING to service —
 * whereas this program stops its target). Whoever loses finds the segment already sized. */
static int map_channel(void) {
    int fd = shm_open(GCFENCE_SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { fprintf(stderr, "STEPPER: shm_open(%s) failed: %s\n", GCFENCE_SHM_NAME, strerror(errno)); return -1; }
    struct stat st;
    if (fstat(fd, &st) == 0 && (size_t)st.st_size < sizeof(gcfence_channel_t))
        if (ftruncate(fd, (off_t)sizeof(gcfence_channel_t)) != 0) {
            fprintf(stderr, "STEPPER: ftruncate failed: %s\n", strerror(errno)); close(fd); return -1;
        }
    void *p = mmap(NULL, sizeof(gcfence_channel_t), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { fprintf(stderr, "STEPPER: mmap failed: %s\n", strerror(errno)); return -1; }
    g_ch = (gcfence_channel_t *)p;
    return 0;
}

/* ---- /proc sampling: BLOCKED vs SPINNING ----------------------------------------------- */
/* State chars: 't' = ptrace-stopped (a step is in flight / just trapped, so the thread is being
 * driven and CAN retire), 'R' = running, 'S'/'D' = parked inside a syscall — the shape that makes a
 * zero delta inevitable rather than lucky. */
#define NSTR 8
struct strtab { char s[NSTR][64]; unsigned long n[NSTR]; int used; };
static void tab_add(struct strtab *t, const char *s) {
    for (int i = 0; i < t->used; i++) if (strcmp(t->s[i], s) == 0) { t->n[i]++; return; }
    if (t->used < NSTR) { snprintf(t->s[t->used], sizeof t->s[0], "%s", s); t->n[t->used] = 1; t->used++; }
}
static void tab_print(const char *label, struct strtab *t) {
    printf("# %s:", label);
    if (!t->used) printf(" (none)");
    for (int i = 0; i < t->used; i++) printf(" %s=%lu", t->s[i], t->n[i]);
    printf("\n");
}
static int read_first_line(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r <= 0) return -1;
    buf[r] = 0;
    char *nl = strchr(buf, '\n'); if (nl) *nl = 0;
    return 0;
}
static char read_state(void) {
    char path[128], buf[512];
    snprintf(path, sizeof path, "/proc/%d/task/%d/stat", g_pid, g_tid);
    if (read_first_line(path, buf, sizeof buf) != 0) return '?';
    char *close_paren = strrchr(buf, ')');       /* comm can contain spaces and parens */
    if (!close_paren || !close_paren[1] || !close_paren[2]) return '?';
    return close_paren[2];
}

/* Three buckets: inside the EE-SUSPENDED window, inside the wider GC window, and outside both. */
struct bucket {
    const char *name;
    unsigned long samples;
    unsigned long state[128];
    struct strtab wchan, syscall;
};
static struct bucket g_ee   = {"EE-SUSPENDED (RuntimeSuspendFinished..RuntimeResumeStarted)", 0, {0}, {{{0}}, {0}, 0}, {{{0}}, {0}, 0}};
static struct bucket g_gcw  = {"GC WINDOW (GarbageCollectionStarted..Finished, EE not necessarily parked)", 0, {0}, {{{0}}, {0}, 0}, {{{0}}, {0}, 0}};
static struct bucket g_out  = {"OUTSIDE any GC window (contrast)", 0, {0}, {{{0}}, {0}, 0}, {{{0}}, {0}, 0}};

static void sample_into(struct bucket *b) {
    char path[128], buf[256];
    b->state[(unsigned char)read_state() & 127]++;
    b->samples++;
    snprintf(path, sizeof path, "/proc/%d/task/%d/wchan", g_pid, g_tid);
    if (read_first_line(path, buf, sizeof buf) == 0 && buf[0]) tab_add(&b->wchan, buf);
    snprintf(path, sizeof path, "/proc/%d/task/%d/syscall", g_pid, g_tid);
    if (read_first_line(path, buf, sizeof buf) == 0 && buf[0]) {
        char *sp = strchr(buf, ' '); if (sp) *sp = 0;    /* the syscall NUMBER (202 = futex) */
        tab_add(&b->syscall, buf);
    }
}
static void bucket_print(struct bucket *b) {
    printf("GCFENCE_STATE bucket=\"%s\" samples=%lu t=%lu R=%lu S=%lu D=%lu Z=%lu other=%lu\n",
           b->name, b->samples, b->state['t'], b->state['R'], b->state['S'], b->state['D'], b->state['Z'],
           b->samples - b->state['t'] - b->state['R'] - b->state['S'] - b->state['D'] - b->state['Z']);
    tab_print("  wchan", &b->wchan);
    tab_print("  syscall (202 = futex, 'running' = not in a syscall)", &b->syscall);
}

static void *watcher(void *arg) {
    (void)arg;
    while (!g_watch_stop) {
        if (g_ch && g_ch->ee_suspended)      sample_into(&g_ee);
        else if (g_ch && g_ch->fence_active) sample_into(&g_gcw);
        else                                 sample_into(&g_out);
        struct timespec ts = {0, 200000}; /* 200 us */
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* ---- a cached snapshot of the victim's maps, for classifying sampled RIPs --------------- */
/* Loaded once per burst (the JIT adds mappings, so it is not loaded once for the whole run). The
 * classification is the crux of the explanation: instructions retired in JITTED MANAGED CODE are the
 * thread making progress toward a GC-safe point, whereas instructions retired in libcoreclr are the
 * thread being carried around by the runtime — and it turns out to be all of the latter. */
struct maprange { uint64_t lo, hi; int x; char name[64]; };
static struct maprange g_maps[GCFENCE_MAPS_MAX];
static int g_nmaps;
static unsigned long g_cls_coreclr, g_cls_libc, g_cls_jit, g_cls_other;

static void maps_load(void) {
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%d/maps", g_pid);
    FILE *f = fopen(path, "r");
    if (!f) return;
    g_nmaps = 0;
    while (g_nmaps < GCFENCE_MAPS_MAX && fgets(line, sizeof line, f)) {
        unsigned long lo, hi; char perms[8]; char file[256]; file[0] = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]", &lo, &hi, perms, file) < 3) continue;
        struct maprange *m = &g_maps[g_nmaps++];
        m->lo = lo; m->hi = hi; m->x = (strchr(perms, 'x') != NULL);
        char *p = file; while (*p == ' ') p++;
        const char *base = strrchr(p, '/'); base = base ? base + 1 : p;
        snprintf(m->name, sizeof m->name, "%s", base);
    }
    fclose(f);
}
/* Classify a RIP into: the runtime, libc, anonymous executable memory (the JIT's code heap — where
 * the victim's managed hot loop actually lives), or anything else. */
static void classify(uint64_t rip) {
    for (int i = 0; i < g_nmaps; i++) {
        if (rip < g_maps[i].lo || rip >= g_maps[i].hi) continue;
        if (strstr(g_maps[i].name, "libcoreclr")) g_cls_coreclr++;
        else if (strncmp(g_maps[i].name, "libc", 4) == 0) g_cls_libc++;
        else if (!g_maps[i].name[0] && g_maps[i].x) g_cls_jit++;
        else g_cls_other++;
        return;
    }
    g_cls_other++;  /* not in the snapshot: freshly JITted code, most likely */
}

/* ---- RIP sampling: WHERE is the thread while the fence is open? ------------------------- */
struct ripslot { uint64_t rip; unsigned long n; uint32_t in_gc, in_ee; };
static struct ripslot g_rips[GCFENCE_RIP_SLOTS];
static int g_nrips;
static unsigned long g_rip_dropped;

static void rip_add(uint64_t rip, uint32_t in_gc, uint32_t in_ee) {
    /* Collapse to 64-byte granularity: a hot loop spans a few instructions, and the point is WHICH
     * code it is, not which byte. */
    uint64_t key = rip & ~63ull;
    for (int i = 0; i < g_nrips; i++)
        if (g_rips[i].rip == key) { g_rips[i].n++; g_rips[i].in_gc += in_gc; g_rips[i].in_ee += in_ee; return; }
    if (g_nrips < GCFENCE_RIP_SLOTS) {
        g_rips[g_nrips].rip = key; g_rips[g_nrips].n = 1;
        g_rips[g_nrips].in_gc = in_gc; g_rips[g_nrips].in_ee = in_ee; g_nrips++;
    } else g_rip_dropped++;
}
/* Resolve against the victim's maps. Anonymous executable mappings are the JIT's code heap — worth
 * distinguishing from libcoreclr, since "spinning in the runtime" and "spinning in JITted managed
 * code" are different answers to the same question. */
static void rip_resolve(uint64_t rip, char *out, size_t n) {
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%d/maps", g_pid);
    FILE *f = fopen(path, "r");
    snprintf(out, n, "?");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi; char perms[8]; char file[256]; file[0] = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]", &lo, &hi, perms, file) < 3) continue;
        if (rip < lo || rip >= hi) continue;
        char *p = file; while (*p == ' ') p++;
        const char *base = strrchr(p, '/'); base = base ? base + 1 : p;
        if (!*base) snprintf(out, n, "anon(%s)+0x%lx  [JIT code heap if x]", perms, (unsigned long)(rip - lo));
        else        snprintf(out, n, "%s+0x%lx", base, (unsigned long)(rip - lo));
        break;
    }
    fclose(f);
}

/* ---- drain ------------------------------------------------------------------------------ */
static void drain(void) {
    if (!g_ch) return;
    uint32_t n = g_ch->nrec, skipped = 0;
    printf("# --- profiler-sampled windows (drained from %s): %u recorded (%u GC windows, %u EE suspends seen) ---\n",
           GCFENCE_SHM_NAME, n, g_ch->gcs_seen, g_ch->susp_seen);
    for (uint32_t i = 0; i < n && i < GCFENCE_MAX_RECS; i++) {
        gcfence_rec_t *r = &g_ch->recs[i];
        /* A window nowhere near the tracer measures nothing (both ends read a dead counter, which
         * would report a FALSE zero); print only the ones that touched a burst. */
        if (!r->traced && !r->traced_close) { skipped++; continue; }
        printf("GCFENCE_REC kind=%s seq=%u gens=0x%x reason=%u traced=%u traced_close=%u S0=%u S1=%u delta=%d "
               "s_pre_delta=%d s_move=%u move_delta=%d moved_calls=%u ranges=%u reloc=%u us=%llu "
               "sample={old=0x%llx new=0x%llx len=%llu}\n",
               r->kind == GCFENCE_KIND_SUSPEND ? "SUSPEND" : "GCWINDOW",
               r->seq, r->gens, r->reason, r->traced, r->traced_close, r->s0, r->s1, (int)(r->s1 - r->s0),
               (int)(r->s0 - r->s_pre), r->s_move, (int)(r->s_move - r->s0), r->moved_calls,
               r->moved_ranges, r->reloc_ranges, (unsigned long long)((r->t1_ns - r->t0_ns) / 1000),
               (unsigned long long)r->old0, (unsigned long long)r->new0, (unsigned long long)r->len0);
    }
    printf("# (%u further windows neither opened nor closed while the tracer was live — omitted)\n", skipped);
}

/* ---- one attach/step/detach burst -------------------------------------------------------- */
struct burst_result { unsigned long steps, sigs; int died; };

static void run_burst(int burst, double secs, unsigned long *total_steps, struct strtab *sigtab,
                      struct burst_result *res) {
    memset(res, 0, sizeof *res);
    /* Begin the burst OUTSIDE any GC window, so that the next window OPENS while we are stepping and
     * its S0 is a genuine sample of a live counter (a window we join late has an S0 from before we
     * existed, which measures nothing). Not a thumb on the scale: it selects WHEN we start, never
     * what the fence then does. */
    for (int i = 0; i < 200 && g_ch->fence_active; i++) sleep_ms(5);
    if (g_ch->fence_active)
        printf("# burst %d: WARNING — a GC window has been open for 1s; attaching anyway\n", burst);

    if (ptrace(PTRACE_ATTACH, g_tid, 0, 0) != 0) {
        printf("GCFENCE_STEPPER_FAIL burst=%d PTRACE_ATTACH tid=%d: %s\n", burst, g_tid, strerror(errno));
        res->died = 1; return;
    }
    int status = 0;
    if (waitpid(g_tid, &status, __WALL) < 0) {
        printf("GCFENCE_STEPPER_FAIL burst=%d waitpid after attach: %s\n", burst, strerror(errno));
        res->died = 1; return;
    }

    maps_load();
    /* Publish: from here the profiler's S0/S1 samples are of a LIVE counter. */
    g_ch->magic = GCFENCE_MAGIC;
    uint64_t t0 = now_ns(), deadline = t0 + (uint64_t)(secs * 1e9);
    int deliver = 0;
    struct user_regs_struct regs;

    while (now_ns() < deadline) {
        if (ptrace(PTRACE_SINGLESTEP, g_tid, 0, (void *)(long)deliver) != 0) {
            printf("# burst %d: PTRACE_SINGLESTEP failed after %lu steps: %s\n", burst, res->steps, strerror(errno));
            res->died = 1; break;
        }
        deliver = 0;
        if (waitpid(g_tid, &status, __WALL) < 0) {
            printf("# burst %d: waitpid failed after %lu steps: %s\n", burst, res->steps, strerror(errno));
            res->died = 1; break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            printf("# burst %d: traced thread GONE after %lu steps (status=0x%x)\n", burst, res->steps, status);
            res->died = 1; break;
        }
        if (!WIFSTOPPED(status)) continue;
        int sig = WSTOPSIG(status);
        if (sig == SIGTRAP) {
            /* A COMPLETED single step == one instruction the tracer would have recorded. */
            res->steps++;
            g_ch->step_counter = (uint32_t)++*total_steps;
            if ((res->steps % GCFENCE_RIP_EVERY) == 0 && ptrace(PTRACE_GETREGS, g_tid, 0, &regs) == 0) {
                rip_add((uint64_t)regs.rip, g_ch->fence_active ? 1u : 0u, g_ch->ee_suspended ? 1u : 0u);
                classify((uint64_t)regs.rip);
            }
        } else {
            /* Anything else — notably CoreCLR's activation/suspension signals — must be DELIVERED,
             * not swallowed: a runtime whose suspension signal we ate would hang the very fence we
             * are trying to measure, and we would be measuring our own bug. */
            char b[16]; snprintf(b, sizeof b, "sig%d", sig); tab_add(sigtab, b);
            res->sigs++;
            deliver = sig;
        }
    }
    /* Stop counting BEFORE detaching, so a window that closes during the detach is not credited
     * with steps we did not drive. */
    g_ch->magic = 0;
    if (!res->died && ptrace(PTRACE_DETACH, g_tid, 0, 0) != 0)
        printf("# burst %d: PTRACE_DETACH failed: %s\n", burst, strerror(errno));
    double el = (double)(now_ns() - t0) / 1e9;
    printf("GCFENCE_BURST burst=%d steps=%lu elapsed_s=%.2f rate=%.0f/s signals_forwarded=%lu died=%d\n",
           burst, res->steps, el, el > 0 ? res->steps / el : 0.0, res->sigs, res->died);
    fflush(stdout);
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <pid> <tid> <seconds-per-burst> [bursts]\n", argv[0]); return 2; }
    g_pid = atoi(argv[1]);
    g_tid = atoi(argv[2]);
    double secs = atof(argv[3]);
    int bursts = argc > 4 ? atoi(argv[4]) : 1;
    if (bursts < 1) bursts = 1;
    if (map_channel() != 0) return 1;
    g_ch->traced_tid = (uint32_t)g_tid;

    printf("# stepper: PTRACE_ATTACH to managed worker tid=%d of pid=%d — %d burst(s) of %.1fs\n",
           g_tid, g_pid, bursts, secs);
    fflush(stdout);

    pthread_t wt;
    pthread_create(&wt, NULL, watcher, NULL);

    unsigned long total = 0, sigs = 0;
    int died = 0;
    struct strtab sigtab; memset(&sigtab, 0, sizeof sigtab);
    for (int b = 1; b <= bursts && !died; b++) {
        struct burst_result r;
        run_burst(b, secs, &total, &sigtab, &r);
        sigs += r.sigs; died = r.died;
        /* A window that could not close while we were stepping closes as soon as we let go — and
         * its record IS the measurement, so wait for it before the next burst / the drain. */
        sleep_ms(1500);
    }
    g_ch->tracer_done = 1;
    g_watch_stop = 1;
    pthread_join(wt, NULL);

    printf("GCFENCE_STEPS total=%lu bursts=%d signals_forwarded=%lu died=%d\n", total, bursts, sigs, died);
    tab_print("signals forwarded to the traced thread", &sigtab);

    /* The blocked-vs-spinning evidence. */
    bucket_print(&g_ee);
    bucket_print(&g_gcw);
    bucket_print(&g_out);

    unsigned long cls_total = g_cls_coreclr + g_cls_libc + g_cls_jit + g_cls_other;
    printf("GCFENCE_WHERE rip_samples=%lu libcoreclr=%lu libc=%lu jit_code_heap=%lu other=%lu"
           "  steps_per_signal=%.1f\n",
           cls_total, g_cls_coreclr, g_cls_libc, g_cls_jit, g_cls_other,
           sigs ? (double)total / (double)sigs : 0.0);
    printf("# --- where the traced thread's RIP was, sampled every %d steps (%d distinct 64B blocks%s) ---\n",
           GCFENCE_RIP_EVERY, g_nrips, g_rip_dropped ? ", TABLE FULL — some dropped" : "");
    for (int i = 0; i < g_nrips; i++) {
        char sym[320];
        rip_resolve(g_rips[i].rip, sym, sizeof sym);
        printf("GCFENCE_RIP 0x%llx n=%lu in_gc_window=%u in_ee_suspended=%u  %s\n",
               (unsigned long long)g_rips[i].rip, g_rips[i].n, g_rips[i].in_gc, g_rips[i].in_ee, sym);
    }
    drain();
    fflush(stdout);
    return 0;
}
