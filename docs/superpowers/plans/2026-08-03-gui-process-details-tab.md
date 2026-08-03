# Process Details Tab Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `Process details` pane to `asmtest-desktop` that answers "what is this process, and what can I do with it?" for the Processes table's current selection, fed by a new **attach-free** `asmspy --info <pid> [--json]`.

**Architecture:** A new `asmspy_procinfo()` gatherer in `cli/asmspy_proc.c` reads `/proc` and the mapped ELF only — **never ptrace**. `asmspy --info <pid> --json` emits it as a valid one-event `.asmtrace` recording (`procinfo` kind). The desktop spawns that command as a subprocess on selection change (250 ms debounce, `(pid, start_ticks)` cache, 2 s live refresh), parses it with the existing `Recording` loader, and draws it. The desktop links no engine — D9 holds.

**Tech Stack:** C11 (`cli/`), C++17 + Dear ImGui 1.91.9b-docking + nlohmann/json (`desktop/`), GNU make, Docker lanes.

## Global Constraints

- **`asmspy --info` MUST NEVER call ptrace.** Not "attaches briefly" — never. Any field needing an attach is out of scope for this command. This is what makes automatic-on-selection and timer polling safe.
- **The desktop MUST NOT link `libasmspy`** (D9). It reaches asmspy only by spawning a subprocess.
- **Caps are fixed and every truncation is stated** (D7 — a stated absence, never a silent one): threads 64, modules 64, children 32, argv 64 args / 4096 bytes.
- **Counters are emitted RAW** (jiffies, bytes, a `CLOCK_MONOTONIC` timestamp). The command never sleeps to compute a rate; the client derives rates from consecutive snapshots (schema law 2: the client derives).
- **v1 always probes LOCALLY.** The Processes table lists local `/proc` even with an ssh host configured, so a remote probe would render an unrelated remote pid. The pane states the mismatch instead.
- Wall-clock budget for one gather: **250 ms**, after which remaining sections set `budget_exceeded` and are emitted with whatever they have.
- New event kind `procinfo` is a **new registry row** in `docs/internal/gui/asmtrace-schema.md`, never an envelope major bump.
- Commit after each task and **push to `origin/main` immediately** — this tree is worked by concurrent agents.
- This tree is shared. Commit **only your own paths**, by path. Never `git add -A`.

---

### Task 1: `asmspy_procinfo()` — identity, runtime, counters, containment

**Files:**
- Modify: `cli/libasmspy.h` (add the struct + declaration after `asmspy_fingerprint`, ~line 121)
- Modify: `cli/asmspy_proc.c` (add the gatherer after `asmspy_fingerprint`, ~line 1290)
- Create: `cli/test_procinfo.c`
- Modify: `mk/cli.mk` (build rule + `cli-smoke` dependency)

**Interfaces:**
- Consumes: `asmspy_fingerprint_t` / `asmspy_fingerprint()` (`cli/libasmspy.h:95-121`)
- Produces: `asmspy_procinfo_t`, `asmspy_procinfo(pid_t, asmspy_procinfo_t *)`, and the caps `ASMSPY_PI_*`. Tasks 2, 3 and 4 extend and consume this exact struct.

- [ ] **Step 1: Write the failing test**

Create `cli/test_procinfo.c`:

```c
/* test_procinfo.c — the attach-free process snapshot (asmspy_procinfo).
 *
 * The contract that matters is NEGATIVE: this gatherer must never ptrace. A
 * test cannot prove absence directly, so it proves the consequence — the
 * snapshot succeeds against a target this process has no ptrace permission
 * for. Under Yama ptrace_scope=1 (this host) our own PARENT is such a target:
 * an attach would be refused, and a correct asmspy_procinfo still fills in.
 *
 * The rest pins the fields a UI would silently render wrong: identity against
 * values we can independently name (getpid/getppid/getuid), the RAW counter
 * contract (jiffies + a monotonic stamp, never a pre-computed rate), and the
 * caps + their truncation flags.
 *
 * Links asmspy_proc.o directly, like test_symtab. No ptrace, no ncurses.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "asmspy.h"

static int failures;

static void check(const char *what, int cond, const char *why) {
    if (!cond) {
        fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

int main(void) {
    asmspy_procinfo_t *pi = malloc(sizeof *pi); /* ~40 KB — never on the stack */
    if (!pi) {
        fprintf(stderr, "FAIL alloc\n");
        return 1;
    }

    /* --- self: every identity field is independently knowable --------- */
    check("self returns 0", asmspy_procinfo(getpid(), pi) == 0, "nonzero");
    check("self pid", pi->pid == (long)getpid(), "pid mismatch");
    check("self ppid", pi->ppid == (long)getppid(), "ppid mismatch");
    check("self uid", pi->uid == (long)getuid(), "uid mismatch");
    check("self user named", pi->user[0] != '\0', "username unresolved");
    check("self comm", strstr(pi->comm, "test_procinfo") != NULL, pi->comm);
    check("self argv", pi->argc >= 1, "argc < 1");
    check("self exe", strstr(pi->exe, "test_procinfo") != NULL, pi->exe);
    check("self cwd", pi->cwd[0] == '/', "cwd not absolute");
    check("self state", pi->state == 'R' || pi->state == 'S', "odd state");
    check("start_ticks", pi->start_ticks > 0, "no start time");

    /* --- counters are RAW, and stamped ------------------------------- */
    check("clk_tck", pi->clk_tck > 0, "no tick rate");
    check("monotonic stamp", pi->ts_ns > 0, "no timestamp");
    check("rss", pi->rss_kb > 0, "no RSS");
    check("threads>=1", pi->threads >= 1, "no threads");

    /* --- runtime rides along verbatim -------------------------------- */
    check("elf class", pi->fp.elf_class == 64 || pi->fp.elf_class == 32,
          "no ELF class");

    /* --- containment -------------------------------------------------- */
    check("pid ns read", pi->ns_pid != 0, "no pid namespace id");
    check("self ns matches self", pi->ns_differs == 0, "self differs from self");
    check("seccomp known-or-unknown", pi->seccomp >= -1 && pi->seccomp <= 2,
          "seccomp out of range");

    /* --- THE contract: a target we could not ptrace still fills in ---- */
    /* Under ptrace_scope=1 our parent is not a descendant of us, so an
     * attach would be refused. A gatherer that quietly ptraced would fail
     * here; one that only reads /proc succeeds. */
    check("parent returns 0", asmspy_procinfo(getppid(), pi) == 0, "nonzero");
    check("parent pid", pi->pid == (long)getppid(), "pid mismatch");
    check("parent comm", pi->comm[0] != '\0', "no comm");

    /* --- a pid that cannot exist is an honest failure, not a blank ---- */
    check("dead pid refused", asmspy_procinfo(0x7ffffff, pi) < 0,
          "a nonexistent pid must fail, not return an empty snapshot");

    free(pi);
    if (failures) {
        fprintf(stderr, "test_procinfo: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_procinfo: all checks passed\n");
    return 0;
}
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
make build/test_procinfo
```
Expected: FAIL — no rule to make target, and once the rule is added, `unknown type name 'asmspy_procinfo_t'`.

- [ ] **Step 3: Add the struct to `cli/libasmspy.h`**

Insert immediately after the `asmspy_fingerprint` declaration (~line 121):

```c
/* ------------------------------------------------------------------ */
/* Process snapshot (asmspy_proc.c) — the desktop's Process details    */
/* pane, and `asmspy --info`.                                          */
/*                                                                     */
/* HARD RULE: this gatherer NEVER calls ptrace. Not "attaches          */
/* briefly" — never. It reads /proc and the mapped ELF, which is what  */
/* makes it safe to fire automatically as an operator browses a        */
/* process list, and safe to poll on a timer. A field that would need  */
/* an attach belongs to a different entry point, not this one.         */
/*                                                                     */
/* Note /proc/<pid>/syscall is NOT an exception: it needs ptrace       */
/* PERMISSION but performs no attach, and is reported as absent (with  */
/* its reason) where the permission is missing.                        */
/*                                                                     */
/* Counters are RAW — jiffies and bytes with a CLOCK_MONOTONIC stamp,  */
/* never a pre-computed rate. The gatherer therefore never sleeps, and */
/* a client derives %CPU / IO rates from two consecutive snapshots.    */
/* ------------------------------------------------------------------ */
enum {
    ASMSPY_PI_THREADS_CAP = 64,
    ASMSPY_PI_MODULES_CAP = 64,
    ASMSPY_PI_CHILDREN_CAP = 32,
    ASMSPY_PI_ARGV_CAP = 64,
    ASMSPY_PI_ARGV_BYTES = 4096
};

/* The engines `asmspy --info` can report on. Order is the display order. */
typedef enum {
    ASMSPY_MODE_LOG = 0,
    ASMSPY_MODE_STREAM,
    ASMSPY_MODE_TRACE,
    ASMSPY_MODE_DATAFLOW,
    ASMSPY_MODE_TREE,
    ASMSPY_MODE_GRAPH,
    ASMSPY_MODE_PROCS,
    ASMSPY_MODE_SAMPLE,
    ASMSPY_MODE_WATCH,
    ASMSPY_MODE__COUNT
} asmspy_mode_t;

/* "log", "stream", ... — the same spelling the CLI flag and the serve
 * protocol's `mode` use, so a UI never invents a third name. */
const char *asmspy_mode_name(asmspy_mode_t m);

typedef struct {
    long tid;
    char comm[20];
    char state; /* R S D Z T t X from /proc/<tid>/stat                 */
    char wchan[48];                 /* kernel symbol it sleeps in; "" if none */
    unsigned long long cpu_jiffies; /* utime+stime of THIS task            */
    /* /proc/<tid>/syscall: needs ptrace permission, performs no attach.  */
    int have_syscall;    /* 0 -> `syscall_why` says why, and is non-empty */
    long syscall_nr;     /* -1 = running in user mode                     */
    char syscall_name[32];
    unsigned long long syscall_args[6];
    unsigned long long pc, sp;
    char pc_sym[96];     /* pc resolved via the symtab; "" if unresolved  */
    char syscall_why[80];
} asmspy_pi_thread_t;

typedef struct {
    char name[64]; /* basename                                          */
    char path[256];
    unsigned long long base, size;
    int exec;
    unsigned long syms; /* STT_FUNC symbols resolved from this module    */
    /* NOTE: symtab-vs-dynsym provenance and the build-id are deliberately
     * ABSENT. Neither is reachable from here (has_symtab is only knowable
     * inside load_module_syms; build_id needs a .note.gnu.build-id reader),
     * and a field that is permanently false/"" but rendered in the pane is
     * the same "confidently wrong" failure this whole snapshot refuses.
     * `syms` already answers the question they were there for. */
} asmspy_pi_module_t;

typedef struct {
    long pid;
    char comm[20];
} asmspy_pi_child_t;

typedef struct {
    /* --- identity --- */
    long pid, ppid, pgid, sid;
    long uid, euid, gid, egid;
    char user[24], euser[24];
    char comm[20];
    char argv[ASMSPY_PI_ARGV_BYTES]; /* NUL-separated, `argc` entries     */
    int argc;
    int argv_truncated;
    char exe[256];
    int exe_deleted; /* /proc/<pid>/exe target ends in " (deleted)"       */
    char cwd[256];
    char state;
    unsigned long long start_ticks; /* /proc/<pid>/stat field 22          */
    double elapsed_s;

    /* --- runtime (asmspy_fingerprint, verbatim) --- */
    asmspy_fingerprint_t fp;

    /* --- counters: RAW. The client derives rates. --- */
    unsigned long long ts_ns;        /* CLOCK_MONOTONIC at gather time     */
    unsigned long long utime, stime; /* jiffies, whole process             */
    unsigned long clk_tck;           /* sysconf(_SC_CLK_TCK)               */
    unsigned long rss_kb, vsize_kb, peak_rss_kb;
    unsigned long long io_read_bytes, io_write_bytes;
    int io_readable; /* /proc/<pid>/io needs matching creds               */
    int n_fds, fds_readable;
    int oom_score, nice, threads;

    /* --- threads (Task 2) --- */
    asmspy_pi_thread_t threads_v[ASMSPY_PI_THREADS_CAP];
    int n_threads_v, threads_truncated;

    /* --- code surface (Task 3) --- */
    unsigned long syms_total, jit_methods;
    char jit_source[16]; /* "jitdump"|"perf-map"|"both"|""                */
    unsigned long long anon_exec_bytes;

    /* --- modules (Task 3) --- */
    asmspy_pi_module_t modules[ASMSPY_PI_MODULES_CAP];
    int n_modules, modules_truncated;

    /* --- traceability (Task 3) --- */
    int attachable; /* 1 yes, 0 no, -1 unknown                           */
    char attach_why[128], attach_remedy[176];
    int mode_ok[ASMSPY_MODE__COUNT];
    char mode_why[ASMSPY_MODE__COUNT][96];

    /* --- containment --- */
    unsigned long long ns_pid, ns_net, ns_mnt, ns_user; /* inode ids      */
    int ns_differs; /* any of the four differs from OUR own              */
    char cgroup[192];
    int seccomp, no_new_privs, dumpable;

    /* --- children --- */
    asmspy_pi_child_t children[ASMSPY_PI_CHILDREN_CAP];
    int n_children, children_truncated;

    /* Sticky: the 250 ms gather budget ran out and later sections carry
     * only what they had. Stated, never silently short. */
    int budget_exceeded;
} asmspy_procinfo_t;

/* Fill *out for `pid`. Returns 0, or -1 when the process does not exist
 * (an unreadable /proc/<pid>/stat) — a nonexistent pid must FAIL rather
 * than yield a plausible empty snapshot. Individual unreadable fields
 * keep their zero/"" defaults and set their own *_readable flag. */
int asmspy_procinfo(pid_t pid, asmspy_procinfo_t *out);
```

- [ ] **Step 4: Add the build rule to `mk/cli.mk`**

After the `test_symtab` rule (~line 462):

```make
# test_procinfo — the attach-free process snapshot (asmspy_procinfo). Its key
# assertion is NEGATIVE: the snapshot must succeed against a target we hold no
# ptrace permission for (our own parent, under Yama ptrace_scope=1), which a
# gatherer that quietly attached could not do. Links the resolver TU directly,
# like test_symtab.
$(BUILD)/test_procinfo: cli/test_procinfo.c $(BUILD)/asmspy_proc.o cli/asmspy.h \
                        | $(BUILD)
	$(CC) $(CFLAGS) -Icli -pthread cli/test_procinfo.c $(BUILD)/asmspy_proc.o \
	  -lstdc++ -o $@
```

Add `$(BUILD)/test_procinfo` to the `cli-smoke:` prerequisite list (~line 649, beside `$(BUILD)/test_symtab`), and add its invocation to the recipe before the `sh cli/cli_smoke.sh` line:

```make
	@echo "--- procinfo (the attach-free process snapshot) ---"
	@$(BUILD)/test_procinfo
```

- [ ] **Step 5: Run it to confirm it now fails on the missing function**

```bash
make build/test_procinfo
```
Expected: FAIL — `undefined reference to 'asmspy_procinfo'`.

- [ ] **Step 6: Implement the gatherer in `cli/asmspy_proc.c`**

Append after `asmspy_fingerprint()` (~line 1290). Small static helpers first; the file already has `read_first_line`, `is_all_digits` and `scan_modules` in scope.

```c
/* ================================================================== */
/* Process snapshot — `asmspy --info` / the desktop's details pane.    */
/* NEVER ptrace. See the contract in libasmspy.h.                      */
/* ================================================================== */

static unsigned long long pi_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull +
           (unsigned long long)ts.tv_nsec;
}

/* The 250 ms budget. Sections check it and stop filling rather than run
 * long on a pathological target; the flag is sticky and always reported. */
static int pi_over_budget(const asmspy_procinfo_t *pi) {
    return (pi_now_ns() - pi->ts_ns) > 250000000ull;
}

/* /proc/<pid>/ns/<what> -> the inode id, or 0 when unreadable. */
static unsigned long long pi_ns_id(pid_t pid, const char *what) {
    char p[80], buf[64];
    snprintf(p, sizeof p, "/proc/%d/ns/%.8s", (int)pid, what);
    ssize_t n = readlink(p, buf, sizeof buf - 1);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    const char *b = strchr(buf, '[');
    return b ? strtoull(b + 1, NULL, 10) : 0;
}

/* uid -> username, falling back to the decimal uid (never blank). */
static void pi_user(long uid, char *out, size_t cap) {
    struct passwd *pw = getpwuid((uid_t)uid);
    if (pw && pw->pw_name)
        snprintf(out, cap, "%.*s", (int)cap - 1, pw->pw_name);
    else
        snprintf(out, cap, "%ld", uid);
}

int asmspy_procinfo(pid_t pid, asmspy_procinfo_t *out) {
    char p[80], line[512];
    FILE *f;

    memset(out, 0, sizeof *out);
    out->seccomp = -1;
    out->attachable = -1;
    out->ts_ns = pi_now_ns();
    out->clk_tck = (unsigned long)sysconf(_SC_CLK_TCK);
    out->pid = (long)pid;

    /* /proc/<pid>/stat is the existence test: no stat, no process. Parse
     * from the LAST ')' so a comm containing ") (" cannot shift fields. */
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    f = fopen(p, "r");
    if (!f)
        return -1;
    size_t n = fread(line, 1, sizeof line - 1, f);
    fclose(f);
    line[n] = '\0';
    char *rp = strrchr(line, ')');
    if (!rp)
        return -1;
    {
        char *lp = strchr(line, '(');
        if (lp && rp > lp + 1)
            snprintf(out->comm, sizeof out->comm, "%.*s", (int)(rp - lp - 1),
                     lp + 1);
        /* fields from 3 (state) on, space separated */
        char st = 0;
        long ppid = 0, pgid = 0, sid = 0, nice = 0, threads = 0;
        unsigned long long ut = 0, stm = 0, start = 0, vsz = 0;
        sscanf(rp + 2,
               "%c %ld %ld %ld %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d "
               "%*d %ld %ld %*d %llu %llu",
               &st, &ppid, &pgid, &sid, &ut, &stm, &nice, &threads, &start,
               &vsz);
        out->state = st;
        out->ppid = ppid;
        out->pgid = pgid;
        out->sid = sid;
        out->utime = ut;
        out->stime = stm;
        out->nice = nice;
        out->threads = (int)threads;
        out->start_ticks = start;
        out->vsize_kb = (unsigned long)(vsz / 1024);
    }

    /* elapsed = uptime - start/clk_tck */
    if ((f = fopen("/proc/uptime", "r"))) {
        double up = 0;
        if (fscanf(f, "%lf", &up) == 1 && out->clk_tck)
            out->elapsed_s = up - (double)out->start_ticks / out->clk_tck;
        fclose(f);
    }

    /* status: uids/gids, VmRSS, VmPeak, Seccomp, NoNewPrivs — one pass. */
    snprintf(p, sizeof p, "/proc/%d/status", (int)pid);
    if ((f = fopen(p, "r"))) {
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "Uid:", 4))
                sscanf(line + 4, "%ld %ld", &out->uid, &out->euid);
            else if (!strncmp(line, "Gid:", 4))
                sscanf(line + 4, "%ld %ld", &out->gid, &out->egid);
            else if (!strncmp(line, "VmRSS:", 6))
                sscanf(line + 6, "%lu", &out->rss_kb);
            else if (!strncmp(line, "VmHWM:", 6))
                sscanf(line + 6, "%lu", &out->peak_rss_kb);
            else if (!strncmp(line, "Seccomp:", 8))
                sscanf(line + 8, "%d", &out->seccomp);
            else if (!strncmp(line, "NoNewPrivs:", 11))
                sscanf(line + 11, "%d", &out->no_new_privs);
        }
        fclose(f);
    }
    pi_user(out->uid, out->user, sizeof out->user);
    pi_user(out->euid, out->euser, sizeof out->euser);

    /* argv: /proc/<pid>/cmdline is already NUL-separated — keep it that
     * way (out->argv mirrors it) and just count and cap. */
    snprintf(p, sizeof p, "/proc/%d/cmdline", (int)pid);
    if ((f = fopen(p, "r"))) {
        size_t got = fread(out->argv, 1, sizeof out->argv - 1, f);
        int c = (int)fread(line, 1, 1, f); /* anything left = truncated */
        fclose(f);
        out->argv[got] = '\0';
        out->argv_truncated = (c > 0);
        for (size_t i = 0; i < got; i++)
            if (out->argv[i] == '\0') {
                if (out->argc < ASMSPY_PI_ARGV_CAP)
                    out->argc++;
                else {
                    out->argv[i] = '\0';
                    out->argv_truncated = 1;
                    break;
                }
            }
        if (!out->argc && got)
            out->argc = 1; /* a cmdline with no trailing NUL */
    }

    /* exe (with the kernel's " (deleted)" suffix split off) and cwd. */
    snprintf(p, sizeof p, "/proc/%d/exe", (int)pid);
    {
        ssize_t r = readlink(p, out->exe, sizeof out->exe - 1);
        out->exe[r > 0 ? r : 0] = '\0';
        char *d = strstr(out->exe, " (deleted)");
        if (d) {
            *d = '\0';
            out->exe_deleted = 1;
        }
    }
    snprintf(p, sizeof p, "/proc/%d/cwd", (int)pid);
    {
        ssize_t r = readlink(p, out->cwd, sizeof out->cwd - 1);
        out->cwd[r > 0 ? r : 0] = '\0';
    }

    /* io — same-creds only; an absence is flagged, never rendered as 0. */
    snprintf(p, sizeof p, "/proc/%d/io", (int)pid);
    if ((f = fopen(p, "r"))) {
        out->io_readable = 1;
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "read_bytes:", 11))
                sscanf(line + 11, "%llu", &out->io_read_bytes);
            else if (!strncmp(line, "write_bytes:", 12))
                sscanf(line + 12, "%llu", &out->io_write_bytes);
        }
        fclose(f);
    }

    /* fd count */
    snprintf(p, sizeof p, "/proc/%d/fd", (int)pid);
    {
        DIR *d = opendir(p);
        if (d) {
            struct dirent *e;
            out->fds_readable = 1;
            while ((e = readdir(d)))
                if (is_all_digits(e->d_name))
                    out->n_fds++;
            closedir(d);
        }
    }

    snprintf(p, sizeof p, "/proc/%d/oom_score", (int)pid);
    if (read_first_line(p, line, sizeof line) == 0)
        out->oom_score = atoi(line);

    /* containment */
    out->ns_pid = pi_ns_id(pid, "pid");
    out->ns_net = pi_ns_id(pid, "net");
    out->ns_mnt = pi_ns_id(pid, "mnt");
    out->ns_user = pi_ns_id(pid, "user");
    {
        pid_t me = getpid();
        out->ns_differs = (out->ns_pid && out->ns_pid != pi_ns_id(me, "pid")) ||
                          (out->ns_net && out->ns_net != pi_ns_id(me, "net")) ||
                          (out->ns_mnt && out->ns_mnt != pi_ns_id(me, "mnt")) ||
                          (out->ns_user && out->ns_user != pi_ns_id(me, "user"));
    }
    snprintf(p, sizeof p, "/proc/%d/cgroup", (int)pid);
    if (read_first_line(p, line, sizeof line) == 0) {
        const char *c = strrchr(line, ':');
        snprintf(out->cgroup, sizeof out->cgroup, "%s", c ? c + 1 : line);
    }
    out->dumpable = -1; /* only readable via prctl on self; unknown here  */

    /* children: one /proc walk, ppid == pid. Cheap, and never the --procs
     * engine, which SEIZEs the whole descendant tree. */
    {
        DIR *d = opendir("/proc");
        struct dirent *e;
        if (d) {
            while ((e = readdir(d)) && !pi_over_budget(out)) {
                if (!is_all_digits(e->d_name))
                    continue;
                long cand = atol(e->d_name);
                if (cand == (long)pid)
                    continue;
                snprintf(p, sizeof p, "/proc/%s/stat", e->d_name);
                if (read_first_line(p, line, sizeof line) != 0)
                    continue;
                char *r2 = strrchr(line, ')');
                long cpp = 0;
                if (!r2 || sscanf(r2 + 2, "%*c %ld", &cpp) != 1 ||
                    cpp != (long)pid)
                    continue;
                if (out->n_children >= ASMSPY_PI_CHILDREN_CAP) {
                    out->children_truncated = 1;
                    break;
                }
                out->children[out->n_children].pid = cand;
                snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
                read_first_line(p, out->children[out->n_children].comm,
                                sizeof out->children[0].comm);
                out->n_children++;
            }
            closedir(d);
        }
    }

    asmspy_fingerprint(pid, &out->fp);
    if (pi_over_budget(out))
        out->budget_exceeded = 1;
    return 0;
}

const char *asmspy_mode_name(asmspy_mode_t m) {
    switch (m) {
    case ASMSPY_MODE_LOG:      return "log";
    case ASMSPY_MODE_STREAM:   return "stream";
    case ASMSPY_MODE_TRACE:    return "trace";
    case ASMSPY_MODE_DATAFLOW: return "dataflow";
    case ASMSPY_MODE_TREE:     return "tree";
    case ASMSPY_MODE_GRAPH:    return "graph";
    case ASMSPY_MODE_PROCS:    return "procs";
    case ASMSPY_MODE_SAMPLE:   return "sample";
    case ASMSPY_MODE_WATCH:    return "watch";
    default:                   return "?";
    }
}
```

Add `#include <pwd.h>` and `#include <time.h>` to the file's include block if absent.

- [ ] **Step 7: Run the test to verify it passes**

```bash
make build/test_procinfo && ./build/test_procinfo
```
Expected: `test_procinfo: all checks passed`

- [ ] **Step 8: Verify in the container lane**

```bash
make docker-cli 2>&1 | tail -30
```
Expected: `cli-smoke` PASS, including the new `--- procinfo ---` line.

- [ ] **Step 9: Commit and push**

```bash
git add cli/libasmspy.h cli/asmspy_proc.c cli/test_procinfo.c mk/cli.mk
git commit -m "cli: asmspy_procinfo — the attach-free process snapshot, part 1

Identity, runtime, raw counters and containment from /proc + the mapped
ELF. NEVER ptrace: the test proves the consequence by snapshotting our own
parent, which under ptrace_scope=1 we hold no attach permission for.

Counters are RAW (jiffies + a CLOCK_MONOTONIC stamp) so the gatherer never
sleeps; a client derives rates from two consecutive snapshots.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
git push origin main
```

---

### Task 2: per-thread state — `wchan` and the current syscall

**Files:**
- Modify: `cli/asmspy_proc.c` (add `pi_read_threads`, call it from `asmspy_procinfo`)
- Modify: `cli/test_procinfo.c` (add the thread assertions)

**Interfaces:**
- Consumes: `asmspy_procinfo_t` and its `threads_v` / `n_threads_v` / `threads_truncated` fields (Task 1)
- Produces: filled `asmspy_pi_thread_t` rows. Task 3 fills each row's `pc_sym` using the symbol table; Task 4 serializes them.

This is the "what is it doing right now" answer, and it is attach-free. `/proc/<tid>/wchan` is unrestricted. `/proc/<tid>/syscall` needs ptrace *permission* but performs no attach — measured on this host: it returns `EPERM` for a non-descendant under `ptrace_scope=1`, so the **degraded path is the default one under test**.

- [ ] **Step 1: Write the failing test**

Append to `cli/test_procinfo.c`, before `free(pi)`:

```c
    /* --- threads: the attach-free "what is it doing now" ------------- */
    check("self reread", asmspy_procinfo(getpid(), pi) == 0, "nonzero");
    check("thread rows", pi->n_threads_v >= 1, "no thread rows");
    check("row 0 is the leader", pi->threads_v[0].tid == (long)getpid(),
          "leader is not first");
    check("row 0 comm", pi->threads_v[0].comm[0] != '\0', "no comm");
    check("row 0 state", pi->threads_v[0].state == 'R' ||
                             pi->threads_v[0].state == 'S',
          "odd thread state");
    check("rows within cap", pi->n_threads_v <= ASMSPY_PI_THREADS_CAP,
          "cap exceeded");
    check("truncation is stated",
          pi->threads_truncated == (pi->threads > ASMSPY_PI_THREADS_CAP),
          "truncated flag disagrees with the thread count");

    /* An absent syscall row must always carry its reason: a blank cell is
     * indistinguishable from "it is doing nothing", which is never true. */
    for (int i = 0; i < pi->n_threads_v; i++)
        check("absent syscall states why",
              pi->threads_v[i].have_syscall || pi->threads_v[i].syscall_why[0],
              "have_syscall == 0 with an empty syscall_why");

    /* wchan on a SLEEPING task names the kernel function it sleeps in.
     * Our parent (a shell awaiting us) is reliably sleeping; a running
     * task correctly has no wchan, so only assert on a sleeper. */
    check("parent reread", asmspy_procinfo(getppid(), pi) == 0, "nonzero");
    for (int i = 0; i < pi->n_threads_v; i++)
        if (pi->threads_v[i].state == 'S')
            check("sleeping thread names its wchan",
                  pi->threads_v[i].wchan[0] != '\0', "empty wchan on a sleeper");
```

- [ ] **Step 2: Run it to verify it fails**

```bash
make build/test_procinfo && ./build/test_procinfo
```
Expected: FAIL — `no thread rows` (and the leader/comm/state checks).

- [ ] **Step 3: Implement `pi_read_threads`**

Add to `cli/asmspy_proc.c` above `asmspy_procinfo`:

```c
/* x86-64 syscall names for the handful a details pane actually shows. The
 * full table is generated for --log (gen-syscall-names.sh); this snapshot
 * needs only a name beside a number, and an unknown number renders as its
 * decimal, which is honest rather than blank. */
static void pi_syscall_name(long nr, char *out, size_t cap) {
    const char *nm = asmspy_syscall_name((int)nr);
    if (nm && *nm)
        snprintf(out, cap, "%.*s", (int)cap - 1, nm);
    else
        snprintf(out, cap, "%ld", nr);
}

/* One task's /proc rows. `state`/`wchan` are unrestricted; `syscall` needs
 * ptrace PERMISSION (no attach) and its absence is REPORTED, never blank. */
static void pi_read_one_thread(pid_t pid, const char *tid_s,
                               asmspy_pi_thread_t *t) {
    char p[96], line[512];

    t->tid = atol(tid_s);
    t->syscall_nr = -1;

    snprintf(p, sizeof p, "/proc/%d/task/%.20s/comm", (int)pid, tid_s);
    read_first_line(p, t->comm, sizeof t->comm);

    snprintf(p, sizeof p, "/proc/%d/task/%.20s/stat", (int)pid, tid_s);
    if (read_first_line(p, line, sizeof line) == 0) {
        char *rp = strrchr(line, ')');
        unsigned long long ut = 0, st = 0;
        if (rp)
            sscanf(rp + 2,
                   "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
                   &t->state, &ut, &st);
        t->cpu_jiffies = ut + st;
    }

    snprintf(p, sizeof p, "/proc/%d/task/%.20s/wchan", (int)pid, tid_s);
    read_first_line(p, t->wchan, sizeof t->wchan);
    if (!strcmp(t->wchan, "0")) /* the kernel's "not sleeping" spelling */
        t->wchan[0] = '\0';

    /* /proc/<tid>/syscall: "<nr> <a0>..<a5> <sp> <pc>", or "running", or
     * EPERM/ENOENT. Each failure names itself — a UI must be able to tell
     * "in user code" from "we were not allowed to look". */
    snprintf(p, sizeof p, "/proc/%d/task/%.20s/syscall", (int)pid, tid_s);
    errno = 0;
    if (read_first_line(p, line, sizeof line) != 0) {
        snprintf(t->syscall_why, sizeof t->syscall_why,
                 errno == EPERM || errno == EACCES
                     ? "needs ptrace permission (Yama ptrace_scope / uid)"
                     : "unreadable: %s",
                 strerror(errno));
        return;
    }
    if (!strncmp(line, "running", 7)) {
        snprintf(t->syscall_why, sizeof t->syscall_why, "running in user mode");
        return;
    }
    unsigned long long a[6] = {0}, sp = 0, pc = 0;
    long nr = -1;
    if (sscanf(line, "%ld %llx %llx %llx %llx %llx %llx %llx %llx", &nr, &a[0],
               &a[1], &a[2], &a[3], &a[4], &a[5], &sp, &pc) < 9) {
        snprintf(t->syscall_why, sizeof t->syscall_why, "unparsed: %.40s",
                 line);
        return;
    }
    t->have_syscall = 1;
    t->syscall_nr = nr;
    pi_syscall_name(nr, t->syscall_name, sizeof t->syscall_name);
    memcpy(t->syscall_args, a, sizeof a);
    t->sp = sp;
    t->pc = pc;
}

/* Every task of `pid`, leader FIRST (it is the row an operator looks for),
 * then the rest ascending by tid — a stable order, because a row that moves
 * while you read it is a row you cannot point at. */
static void pi_read_threads(pid_t pid, asmspy_procinfo_t *out) {
    char tp[64];
    snprintf(tp, sizeof tp, "/proc/%d/task", (int)pid);
    DIR *d = opendir(tp);
    if (!d)
        return;
    long tids[ASMSPY_PI_THREADS_CAP];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_all_digits(e->d_name))
            continue;
        if (n >= ASMSPY_PI_THREADS_CAP) {
            out->threads_truncated = 1;
            continue;
        }
        tids[n++] = atol(e->d_name);
    }
    closedir(d);

    /* ascending, then swap the leader to the front */
    for (int i = 1; i < n; i++) {
        long v = tids[i];
        int j = i - 1;
        while (j >= 0 && tids[j] > v)
            tids[j + 1] = tids[j--];
        tids[j + 1] = v;
    }
    for (int i = 0; i < n; i++)
        if (tids[i] == (long)pid) {
            long t = tids[0];
            tids[0] = tids[i];
            tids[i] = t;
            break;
        }

    for (int i = 0; i < n; i++) {
        char s[24];
        snprintf(s, sizeof s, "%ld", tids[i]);
        pi_read_one_thread(pid, s, &out->threads_v[out->n_threads_v++]);
        if (pi_over_budget(out)) {
            out->budget_exceeded = 1;
            if (i + 1 < n)
                out->threads_truncated = 1;
            break;
        }
    }
}
```

Call it in `asmspy_procinfo` immediately before `asmspy_fingerprint(pid, &out->fp);`:

```c
    pi_read_threads(pid, out);
```

If `asmspy_syscall_name` does not already exist in this TU, use the table `gen-syscall-names.sh` generates (grep `syscall_name` in `cli/asmspy_proc.c` / `cli/asmspy.c`); if there is no shared accessor, render the decimal number only and drop `pi_syscall_name`'s lookup branch — a number with no name is honest, an invented name is not.

- [ ] **Step 4: Run the test to verify it passes**

```bash
make build/test_procinfo && ./build/test_procinfo
```
Expected: `test_procinfo: all checks passed`

- [ ] **Step 5: Confirm the degraded path is actually exercised**

```bash
./build/test_procinfo && cat /proc/sys/kernel/yama/ptrace_scope
```
Expected: passes, and `1` — so the `syscall_why` branch is the one that ran, not dead code.

- [ ] **Step 6: Commit and push**

```bash
git add cli/asmspy_proc.c cli/test_procinfo.c
git commit -m "cli: procinfo per-thread state — wchan + the current syscall

The attach-free 'what is it doing right now': /proc/<tid>/wchan is
unrestricted, and /proc/<tid>/syscall needs ptrace PERMISSION but performs
no attach. Under this host's ptrace_scope=1 the permission is usually
absent, so the degraded path is the default one under test — and an absent
syscall row always carries its reason, because a blank cell reads as 'doing
nothing', which is never true.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
git push origin main
```

---

### Task 3: code surface, modules, and the per-mode capability verdict

**Files:**
- Modify: `cli/asmspy_proc.c` (add `pi_read_code_and_modules`, `pi_verdict`)
- Modify: `cli/test_procinfo.c` (add the assertions)

**Interfaces:**
- Consumes: `asmspy_procinfo_t` (Task 1), `asmspy_pi_thread_t::pc` (Task 2), plus the existing `scan_modules`, `asmspy_symtab_load`, `asmspy_symtab_at`, `asmspy_symtab_free`, `asmspy_jitmap_init/refresh/free`, `asmspy_elf_class`
- Produces: `syms_total`, `jit_methods`, `jit_source`, `anon_exec_bytes`, `modules[]`, `attachable`/`attach_why`/`attach_remedy`, `mode_ok[]`/`mode_why[]`, and each thread's `pc_sym`

- [ ] **Step 1: Write the failing test**

Append to `cli/test_procinfo.c`, before `free(pi)`:

```c
    /* --- code surface + modules -------------------------------------- */
    check("self reread 2", asmspy_procinfo(getpid(), pi) == 0, "nonzero");
    check("symbols found", pi->syms_total > 0, "no symbols in our own image");
    check("modules found", pi->n_modules > 0, "no modules");
    check("modules within cap", pi->n_modules <= ASMSPY_PI_MODULES_CAP,
          "cap exceeded");
    check("module 0 named", pi->modules[0].name[0] != '\0', "unnamed module");
    check("modules ranked by symbol count",
          pi->n_modules < 2 || pi->modules[0].syms >= pi->modules[1].syms,
          "modules are not symbol-count-descending");
    check("no JIT here", pi->jit_methods == 0, "a static C test has no JIT");

    /* --- the capability verdict -------------------------------------- */
    /* Our own pid: an engine cannot trace its own tracer thread, but the
     * verdict is about the TARGET's facts, and we are a native 64-bit
     * process nothing else traces — so the portable modes are available. */
    check("attachable known", pi->attachable == 1 || pi->attachable == 0 ||
                                  pi->attachable == -1,
          "verdict out of range");
    check("why never empty", pi->attach_why[0] != '\0', "empty why");
    for (int m = 0; m < ASMSPY_MODE__COUNT; m++)
        check("refused mode states why",
              pi->mode_ok[m] || pi->mode_why[m][0] != '\0',
              "a refused mode with an empty reason");
    check("mode names", strcmp(asmspy_mode_name(ASMSPY_MODE_LOG), "log") == 0,
          "mode name mismatch");
    check("dataflow ok on a native 64-bit target",
          pi->fp.elf_class != 64 || pi->mode_ok[ASMSPY_MODE_DATAFLOW],
          "dataflow refused on a 64-bit native target");
```

- [ ] **Step 2: Run it to verify it fails**

```bash
make build/test_procinfo && ./build/test_procinfo
```
Expected: FAIL — `no symbols in our own image`, `no modules`, `empty why`.

- [ ] **Step 3: Implement the code surface, modules, and verdict**

Add to `cli/asmspy_proc.c` above `asmspy_procinfo`:

```c
/* Rank modules by symbol count DESCENDING: the ones a trace will actually
 * resolve names against are the ones worth the 64 rows. */
static int pi_mod_cmp(const void *a, const void *b) {
    const asmspy_pi_module_t *x = a, *y = b;
    if (x->syms != y->syms)
        return x->syms < y->syms ? 1 : -1;
    return strcmp(x->name, y->name);
}

static void pi_read_code_and_modules(pid_t pid, asmspy_procinfo_t *out) {
    asmspy_symtab_t syms;
    if (asmspy_symtab_load(pid, &syms) == 0) {
        out->syms_total = (unsigned long)syms.n;

        /* per-module counts, and each thread's pc resolved to a name */
        module_t *mods = NULL;
        char exe_path[PATH_MAX];
        int nm = scan_modules(pid, &mods, exe_path, sizeof exe_path);
        for (int i = 0; i < nm; i++) {
            const char *b = strrchr(mods[i].path, '/');
            b = b ? b + 1 : mods[i].path;
            if (out->n_modules >= ASMSPY_PI_MODULES_CAP) {
                out->modules_truncated = 1;
                break;
            }
            asmspy_pi_module_t *m = &out->modules[out->n_modules++];
            snprintf(m->name, sizeof m->name, "%.63s", b);
            snprintf(m->path, sizeof m->path, "%.255s", mods[i].path);
            m->base = mods[i].load_start;
            m->exec = 1;
        }
        free(mods);

        /* Per-module counts in ONE pass over the symbols, not a scan per
         * module. The nested form is 64 modules x 60k symbols on a real
         * target — ~4M string compares against a 250 ms whole-gather budget
         * whose justification is a measured 20 ms. The symbols are sorted by
         * address and so cluster by module, which makes the last-hit memo
         * hit almost always; the linear fallback is bounded by the 64-module
         * cap, never by the symbol count. */
        int last = -1;
        for (size_t k = 0; k < syms.n; k++) {
            const char *mn = syms.v[k].module;
            if (!mn)
                continue;
            if (last >= 0 && !strcmp(out->modules[last].name, mn)) {
                out->modules[last].syms++;
                continue;
            }
            for (int i = 0; i < out->n_modules; i++)
                if (!strcmp(out->modules[i].name, mn)) {
                    out->modules[i].syms++;
                    last = i;
                    break;
                }
        }
        qsort(out->modules, (size_t)out->n_modules, sizeof out->modules[0],
              pi_mod_cmp);

        for (int i = 0; i < out->n_threads_v; i++) {
            const asmspy_sym_t *s =
                out->threads_v[i].have_syscall
                    ? asmspy_symtab_at(&syms, out->threads_v[i].pc)
                    : NULL;
            if (s)
                snprintf(out->threads_v[i].pc_sym,
                         sizeof out->threads_v[i].pc_sym, "%.60s+0x%llx",
                         s->name,
                         (unsigned long long)(out->threads_v[i].pc - s->addr));
        }
        asmspy_symtab_free(&syms);
    }

    /* JIT methods — the code an ELF symtab can never see. */
    {
        asmspy_jitmap_t j;
        asmspy_jitmap_init(&j, pid);
        int n = asmspy_jitmap_refresh(&j);
        if (n > 0) {
            out->jit_methods = (unsigned long)n;
            snprintf(out->jit_source, sizeof out->jit_source, "%s",
                     j.dump_path[0] ? "jitdump" : "perf-map");
        }
        asmspy_jitmap_free(&j);
    }

    /* anon-exec bytes: executable mappings with no backing file — the JIT
     * surface. One /proc/<pid>/maps pass. */
    {
        char p[64], line[512];
        snprintf(p, sizeof p, "/proc/%d/maps", (int)pid);
        FILE *f = fopen(p, "r");
        if (f) {
            while (fgets(line, sizeof line, f)) {
                unsigned long long a = 0, b = 0;
                char perms[8] = "", rest[256] = "";
                if (sscanf(line, "%llx-%llx %7s %*s %*s %*s %255[^\n]", &a, &b,
                           perms, rest) < 3)
                    continue;
                if (perms[2] == 'x' && rest[0] != '/')
                    out->anon_exec_bytes += b - a;
            }
            fclose(f);
        }
    }
}

/* The attach verdict + which engines could run on this target. The facts
 * are not independent, and the DOMINATING one must be reported, or an
 * operator raises a Yama scope when the real problem is an i386 tracee. */
static void pi_verdict(pid_t pid, asmspy_procinfo_t *out) {
    long our_uid = (long)getuid();
    int yama = -1;
    char line[64];
    if (read_first_line("/proc/sys/kernel/yama/ptrace_scope", line,
                        sizeof line) == 0)
        yama = atoi(line);

    if (out->fp.tracer_pid) {
        out->attachable = 0;
        snprintf(out->attach_why, sizeof out->attach_why,
                 "already traced by pid %d", (int)out->fp.tracer_pid);
        snprintf(out->attach_remedy, sizeof out->attach_remedy,
                 "stop that tracer first — the kernel allows one per target");
    } else if (out->euid != our_uid && geteuid() != 0) {
        out->attachable = 0;
        snprintf(out->attach_why, sizeof out->attach_why,
                 "owned by %s, not you", out->euser);
        snprintf(out->attach_remedy, sizeof out->attach_remedy,
                 "run asmspy as that user, or with CAP_SYS_PTRACE");
    } else if (yama >= 3) {
        out->attachable = 0;
        snprintf(out->attach_why, sizeof out->attach_why,
                 "yama ptrace_scope=3 — attach is disabled kernel-wide");
        snprintf(out->attach_remedy, sizeof out->attach_remedy,
                 "scope 3 cannot be lowered without a reboot");
    } else if (yama >= 1 && geteuid() != 0) {
        out->attachable = -1;
        snprintf(out->attach_why, sizeof out->attach_why,
                 "yama ptrace_scope=%d — only a descendant, or a target that "
                 "called PR_SET_PTRACER",
                 yama);
        snprintf(out->attach_remedy, sizeof out->attach_remedy,
                 "sudo sysctl kernel.yama.ptrace_scope=0, or launch the target "
                 "from asmspy");
    } else {
        out->attachable = 1;
        snprintf(out->attach_why, sizeof out->attach_why, "same uid, nothing "
                 "else traces it");
    }

    /* Every mode starts at the attach verdict, then adds its OWN gate. */
    for (int m = 0; m < ASMSPY_MODE__COUNT; m++) {
        out->mode_ok[m] = (out->attachable != 0);
        if (!out->mode_ok[m])
            snprintf(out->mode_why[m], sizeof out->mode_why[m], "%.90s",
                     out->attach_why);
    }
    /* An i386 tracee reports its syscall numbers against a DIFFERENT table,
     * so the single-step engines refuse rather than name every call wrong. */
    if (out->fp.elf_class == 32) {
        const asmspy_mode_t x86_only[] = {ASMSPY_MODE_DATAFLOW,
                                          ASMSPY_MODE_STREAM, ASMSPY_MODE_TRACE,
                                          ASMSPY_MODE_LOG, ASMSPY_MODE_WATCH};
        for (size_t i = 0; i < sizeof x86_only / sizeof x86_only[0]; i++) {
            out->mode_ok[x86_only[i]] = 0;
            snprintf(out->mode_why[x86_only[i]],
                     sizeof out->mode_why[0],
                     "32-bit tracee — the engines are x86-64 only, and would "
                     "name every syscall wrong");
        }
    }
    /* --sample is IBS silicon, a fact about the HOST, not the target. */
    if (out->mode_ok[ASMSPY_MODE_SAMPLE] &&
        access("/sys/devices/ibs_op", F_OK) != 0) {
        out->mode_ok[ASMSPY_MODE_SAMPLE] = 0;
        snprintf(out->mode_why[ASMSPY_MODE_SAMPLE],
                 sizeof out->mode_why[0],
                 "needs an AMD IBS host — no ibs_op PMU here");
    }
    (void)pid;
}
```

Call both in `asmspy_procinfo`, after `pi_read_threads(pid, out);`:

```c
    pi_read_code_and_modules(pid, out);
    pi_verdict(pid, out);
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
make build/test_procinfo && ./build/test_procinfo
```
Expected: `test_procinfo: all checks passed`

- [ ] **Step 5: Confirm the gather is still fast on the heaviest real target**

```bash
BIG=$(for p in $(ls /proc | grep -E '^[0-9]+$'); do \
        n=$(wc -l < /proc/$p/maps 2>/dev/null); [ -n "$n" ] && echo "$n $p"; \
      done 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2)
echo "heaviest pid: $BIG"
```
Expected: a pid prints. (Timing is asserted in Task 4 once the CLI exists.)

- [ ] **Step 6: Commit and push**

```bash
git add cli/asmspy_proc.c cli/test_procinfo.c
git commit -m "cli: procinfo code surface, modules, and the per-mode verdict

Resolved symbol count, per-module counts ranked symbol-descending, JIT
method count and its source, anon-exec bytes (the surface an ELF symtab
cannot see), and each thread's pc resolved to a name.

Plus the part that saves a wasted capture: which asmspy engines can run on
this target and why not — an i386 tracee refuses the single-step engines
(they would name every syscall wrong), --sample refuses off AMD IBS silicon.
A refused mode always carries its reason.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
git push origin main
```

---

### Task 4: `asmspy --info <pid> [--json]` + the `procinfo` schema kind + docs

**Files:**
- Modify: `cli/asmspy.c` (add `cmd_info`, the `usage()` line, the argv branch)
- Modify: `cli/cli_smoke.sh` (add an `--info` case)
- Modify: `docs/internal/gui/asmtrace-schema.md` (define `procinfo`)
- Modify: `docs/guides/tracing/asmspy.md` (document `--info`)
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: `asmspy_procinfo()` and the whole `asmspy_procinfo_t` (Tasks 1-3); `rec_open` / `rec_emit` / `rec_close` (`cli/asmspy.c:149/156/215`); `asmtrace_escape` (`cli/asmtrace_ndjson.h`)
- Produces: **the JSON wire shape** below. Task 5 parses exactly this — the two must not drift.

**The `procinfo` event body (authoritative; Task 5 mirrors it field-for-field):**

```json
{"k":"procinfo",
 "identity":{"pid":123,"ppid":1,"pgid":123,"sid":123,"uid":1000,"euid":1000,
   "gid":1000,"egid":1000,"user":"will","euser":"will","comm":"code",
   "argv":["/usr/share/code/code","--type=renderer"],"argv_truncated":false,
   "exe":"/usr/share/code/code","exe_deleted":false,"cwd":"/home/will",
   "state":"S","start_ticks":123456,"elapsed_s":1234.5},
 "runtime":{"runtime":"Node/V8","evidence":"libnode.so","jitting":true,
   "elf_class":64,"pie":true,"static":false,"interp":"ld-linux-x86-64.so.2"},
 "counters":{"ts_ns":881234567890,"utime":4210,"stime":880,"clk_tck":100,
   "rss_kb":626688,"vsize_kb":12058624,"peak_rss_kb":700000,
   "io_read_bytes":1234,"io_write_bytes":0,"io_readable":true,
   "fds":184,"fds_readable":true,"oom_score":200,"nice":0,"threads":12},
 "threads":[{"tid":123,"comm":"code","state":"S","wchan":"futex_wait",
   "cpu_jiffies":410,
   "syscall":{"nr":202,"name":"futex","args":["0x7f..","0x80","0x0","0x0",
     "0x0","0x0"],"pc":"0x7f1234","sp":"0x7ffd00",
     "pc_sym":"__futex_abstimed_wait+0x1c"}},
  {"tid":124,"comm":"V8 Worker","state":"S","wchan":"poll_schedule_timeout",
   "cpu_jiffies":12,"syscall_why":"needs ptrace permission (Yama ptrace_scope / uid)"}],
 "threads_truncated":false,
 "code":{"syms_total":61530,"jit_methods":1402,"jit_source":"perf-map",
   "anon_exec_bytes":12582912},
 "modules":[{"name":"libnode.so","path":"/usr/lib/libnode.so","base":"0x7f0000",
   "size":2117632,"exec":true,"syms":50123}],
 "modules_truncated":false,
 "trace":{"attachable":1,"why":"same uid, nothing else traces it","remedy":"",
   "modes":[{"mode":"log","ok":true,"why":""},
            {"mode":"sample","ok":false,"why":"needs an AMD IBS host — no ibs_op PMU here"}]},
 "containment":{"ns_pid":4026531836,"ns_net":4026531833,"ns_mnt":4026531832,
   "ns_user":4026531837,"differs":false,"cgroup":"/user.slice/user-1000.slice",
   "seccomp":2,"no_new_privs":0,"dumpable":-1},
 "children":[{"pid":124,"comm":"sh"}],
 "children_truncated":false,
 "budget_exceeded":false}
```

Rules the shape encodes, each load-bearing:

- **A thread with no readable syscall OMITS the `syscall` object and carries `syscall_why`.** Absent-with-a-reason, never a blank object.
- **64-bit quantities that are addresses (`pc`, `sp`, `base`, syscall `args`) are hex STRINGS.** JSON numbers are doubles in many readers and would silently round a pointer.
- **Counts and byte totals stay JSON numbers.**
- `remedy` and `why` are `""` when there is nothing to say, never absent.

- [ ] **Step 1: Write the failing smoke case**

Add to `cli/cli_smoke.sh`, following the file's existing section-comment style:

```sh
# ---------------------------------------------------------------------------
# --info — the attach-free process snapshot
# ---------------------------------------------------------------------------
say "--- --info (attach-free process snapshot) ---"

# Against OUR OWN shell: a target this smoke holds no ptrace permission for
# under ptrace_scope=1, so a run that succeeds proves --info never attached.
info_json="$($BUILD/asmspy --info $$ --json 2>/dev/null)"

echo "$info_json" | head -1 | grep -q '"asmtrace"' \
  || fail "--info --json: no .asmtrace header line"
echo "$info_json" | grep -q '"k":"procinfo"' \
  || fail "--info --json: no procinfo event"
echo "$info_json" | tail -1 | grep -q '"k":"end"' \
  || fail "--info --json: no end footer"
echo "$info_json" | grep -q "\"pid\":$$" \
  || fail "--info --json: wrong pid in identity"
echo "$info_json" | grep -q '"attachable"' \
  || fail "--info --json: no trace verdict"

# The human form names the process and its runtime.
$BUILD/asmspy --info $$ | grep -q "pid $$" \
  || fail "--info text: no pid line"

# A nonexistent pid is refused, not rendered blank.
if $BUILD/asmspy --info 134217727 >/dev/null 2>&1; then
  fail "--info: a nonexistent pid must exit nonzero"
fi

# It must be FAST — this is fired automatically as an operator browses.
t0=$(date +%s%N)
$BUILD/asmspy --info $$ --json >/dev/null 2>&1
t1=$(date +%s%N)
ms=$(( (t1 - t0) / 1000000 ))
say "    --info wall: ${ms}ms"
[ "$ms" -lt 1000 ] || fail "--info took ${ms}ms — too slow to fire on selection"

say "    --info OK"
```

Match the script's existing `say` / `fail` helper names — grep them at the top of `cli/cli_smoke.sh` and use whatever it actually defines.

- [ ] **Step 2: Run it to verify it fails**

```bash
make build/asmspy && ./build/asmspy --info $$ --json
```
Expected: FAIL — the usage text, exit 2.

- [ ] **Step 3: Implement `cmd_info` in `cli/asmspy.c`**

Add before `usage()` (~line 8110):

```c
/* --info <pid> — the attach-free process snapshot (asmspy_procinfo).
 *
 * Human text by default; --json emits it as a valid ONE-EVENT .asmtrace
 * recording (header + `procinfo` + end), the same contract every other
 * mode's --json carries: `asmspy --info <pid> --json > x.asmtrace` IS a
 * recording, so the desktop reads it with the ordinary loader.
 *
 * Addresses are emitted as hex STRINGS — a JSON number is a double in many
 * readers, which silently rounds a 64-bit pointer. */
static void info_emit_json(const asmspy_procinfo_t *pi, const char *record,
                           int to_stdout) {
    rec_t rec;
    char *b = malloc(256 * 1024); /* the body; bounded by the struct's caps */
    size_t cap = 256 * 1024, n = 0;
    int overflow = 0; /* sticky: the body did not fit */
    char e1[512], e2[512];
    if (!b)
        return;

    /* A body that overflows would be TRUNCATED MID-TOKEN — syntactically
     * invalid JSON that a reader reports as a corrupt recording rather than
     * as our bug. So overflow is tracked and refused loudly below, never
     * emitted. 256 KB is far above the worst case the caps allow (64 threads
     * + 64 modules + 32 children + 4 KB argv is well under 64 KB); this
     * guard exists so that if that ever stops being true, it fails honestly. */
#define APP(...)                                                               \
    do {                                                                       \
        int _w = snprintf(b + n, n < cap ? cap - n : 0, __VA_ARGS__);          \
        if (_w < 0 || (size_t)_w >= (n < cap ? cap - n : 0))                   \
            overflow = 1;                                                      \
        else                                                                   \
            n += (size_t)_w;                                                   \
    } while (0)

    asmtrace_escape(e1, sizeof e1, pi->comm);
    asmtrace_escape(e2, sizeof e2, pi->exe);
    APP("\"identity\":{\"pid\":%ld,\"ppid\":%ld,\"pgid\":%ld,\"sid\":%ld,"
        "\"uid\":%ld,\"euid\":%ld,\"gid\":%ld,\"egid\":%ld,",
        pi->pid, pi->ppid, pi->pgid, pi->sid, pi->uid, pi->euid, pi->gid,
        pi->egid);
    {
        char eu[64], ee[64];
        asmtrace_escape(eu, sizeof eu, pi->user);
        asmtrace_escape(ee, sizeof ee, pi->euser);
        APP("\"user\":\"%s\",\"euser\":\"%s\",\"comm\":\"%s\",", eu, ee, e1);
    }
    APP("\"argv\":[");
    {
        const char *a = pi->argv;
        for (int i = 0; i < pi->argc; i++) {
            char ea[1024];
            asmtrace_escape(ea, sizeof ea, a);
            APP("%s\"%s\"", i ? "," : "", ea);
            a += strlen(a) + 1;
        }
    }
    APP("],\"argv_truncated\":%s,\"exe\":\"%s\",\"exe_deleted\":%s,",
        pi->argv_truncated ? "true" : "false", e2,
        pi->exe_deleted ? "true" : "false");
    asmtrace_escape(e1, sizeof e1, pi->cwd);
    APP("\"cwd\":\"%s\",\"state\":\"%c\",\"start_ticks\":%llu,"
        "\"elapsed_s\":%.1f},",
        e1, pi->state ? pi->state : '?', pi->start_ticks, pi->elapsed_s);

    asmtrace_escape(e1, sizeof e1, pi->fp.runtime);
    asmtrace_escape(e2, sizeof e2, pi->fp.evidence);
    APP("\"runtime\":{\"runtime\":\"%s\",\"evidence\":\"%s\",\"jitting\":%s,"
        "\"elf_class\":%d,\"pie\":%s,\"static\":%s,",
        e1, e2, pi->fp.jitting ? "true" : "false", pi->fp.elf_class,
        pi->fp.pie ? "true" : "false", pi->fp.static_linked ? "true" : "false");
    asmtrace_escape(e1, sizeof e1, pi->fp.interp);
    APP("\"interp\":\"%s\"},", e1);

    APP("\"counters\":{\"ts_ns\":%llu,\"utime\":%llu,\"stime\":%llu,"
        "\"clk_tck\":%lu,\"rss_kb\":%lu,\"vsize_kb\":%lu,\"peak_rss_kb\":%lu,"
        "\"io_read_bytes\":%llu,\"io_write_bytes\":%llu,\"io_readable\":%s,"
        "\"fds\":%d,\"fds_readable\":%s,\"oom_score\":%d,\"nice\":%d,"
        "\"threads\":%d},",
        pi->ts_ns, pi->utime, pi->stime, pi->clk_tck, pi->rss_kb, pi->vsize_kb,
        pi->peak_rss_kb, pi->io_read_bytes, pi->io_write_bytes,
        pi->io_readable ? "true" : "false", pi->n_fds,
        pi->fds_readable ? "true" : "false", pi->oom_score, pi->nice,
        pi->threads);

    APP("\"threads\":[");
    for (int i = 0; i < pi->n_threads_v; i++) {
        const asmspy_pi_thread_t *t = &pi->threads_v[i];
        asmtrace_escape(e1, sizeof e1, t->comm);
        asmtrace_escape(e2, sizeof e2, t->wchan);
        APP("%s{\"tid\":%ld,\"comm\":\"%s\",\"state\":\"%c\",\"wchan\":\"%s\","
            "\"cpu_jiffies\":%llu",
            i ? "," : "", t->tid, e1, t->state ? t->state : '?', e2,
            t->cpu_jiffies);
        if (t->have_syscall) {
            asmtrace_escape(e1, sizeof e1, t->syscall_name);
            APP(",\"syscall\":{\"nr\":%ld,\"name\":\"%s\",\"args\":[",
                t->syscall_nr, e1);
            for (int k = 0; k < 6; k++)
                APP("%s\"0x%llx\"", k ? "," : "", t->syscall_args[k]);
            asmtrace_escape(e2, sizeof e2, t->pc_sym);
            APP("],\"pc\":\"0x%llx\",\"sp\":\"0x%llx\",\"pc_sym\":\"%s\"}",
                t->pc, t->sp, e2);
        } else {
            asmtrace_escape(e1, sizeof e1, t->syscall_why);
            APP(",\"syscall_why\":\"%s\"", e1);
        }
        APP("}");
    }
    asmtrace_escape(e1, sizeof e1, pi->jit_source);
    APP("],\"threads_truncated\":%s,",
        pi->threads_truncated ? "true" : "false");
    APP("\"code\":{\"syms_total\":%lu,\"jit_methods\":%lu,\"jit_source\":\"%s\","
        "\"anon_exec_bytes\":%llu},",
        pi->syms_total, pi->jit_methods, e1, pi->anon_exec_bytes);

    APP("\"modules\":[");
    for (int i = 0; i < pi->n_modules; i++) {
        const asmspy_pi_module_t *m = &pi->modules[i];
        asmtrace_escape(e1, sizeof e1, m->name);
        asmtrace_escape(e2, sizeof e2, m->path);
        APP("%s{\"name\":\"%s\",\"path\":\"%s\",\"base\":\"0x%llx\","
            "\"size\":%llu,\"exec\":%s,\"syms\":%lu}",
            i ? "," : "", e1, e2, m->base, m->size, m->exec ? "true" : "false",
            m->syms);
    }
    APP("],\"modules_truncated\":%s,",
        pi->modules_truncated ? "true" : "false");

    asmtrace_escape(e1, sizeof e1, pi->attach_why);
    asmtrace_escape(e2, sizeof e2, pi->attach_remedy);
    APP("\"trace\":{\"attachable\":%d,\"why\":\"%s\",\"remedy\":\"%s\","
        "\"modes\":[",
        pi->attachable, e1, e2);
    for (int m = 0; m < ASMSPY_MODE__COUNT; m++) {
        asmtrace_escape(e1, sizeof e1, pi->mode_why[m]);
        APP("%s{\"mode\":\"%s\",\"ok\":%s,\"why\":\"%s\"}", m ? "," : "",
            asmspy_mode_name((asmspy_mode_t)m), pi->mode_ok[m] ? "true" : "false",
            e1);
    }
    asmtrace_escape(e2, sizeof e2, pi->cgroup);
    APP("]},\"containment\":{\"ns_pid\":%llu,\"ns_net\":%llu,\"ns_mnt\":%llu,"
        "\"ns_user\":%llu,\"differs\":%s,\"cgroup\":\"%s\",\"seccomp\":%d,"
        "\"no_new_privs\":%d,\"dumpable\":%d},",
        pi->ns_pid, pi->ns_net, pi->ns_mnt, pi->ns_user,
        pi->ns_differs ? "true" : "false", e2, pi->seccomp, pi->no_new_privs,
        pi->dumpable);

    APP("\"children\":[");
    for (int i = 0; i < pi->n_children; i++) {
        asmtrace_escape(e1, sizeof e1, pi->children[i].comm);
        APP("%s{\"pid\":%ld,\"comm\":\"%s\"}", i ? "," : "",
            pi->children[i].pid, e1);
    }
    APP("],\"children_truncated\":%s,\"budget_exceeded\":%s",
        pi->children_truncated ? "true" : "false",
        pi->budget_exceeded ? "true" : "false");
#undef APP

    if (overflow) {
        /* Refuse rather than emit malformed JSON a reader would call corrupt. */
        fprintf(stderr, "--info: snapshot body exceeded %zu bytes — refusing "
                        "to emit a truncated recording\n",
                cap);
        free(b);
        return;
    }
    /* `exact` because every field was READ, not sampled — the snapshot is a
     * true statement about the instant it was taken. `to_stdout` is 0 for a
     * bare --record=<f>, which records without putting NDJSON on a stdout the
     * human text is already using. */
    if (rec_open(&rec, record, to_stdout ? stdout : NULL, "proc-snapshot", 1,
                 "exact", (pid_t)pi->pid) == 0) {
        rec_emit(&rec, "procinfo", b);
        rec_close(&rec, 0, 0, 0, NULL);
    }
    free(b);
}

static void info_print_text(const asmspy_procinfo_t *pi) {
    printf("pid %ld  %s  [%s]  %c\n", pi->pid, pi->comm,
           pi->fp.runtime[0] ? pi->fp.runtime : "?", pi->state);
    {
        const char *a = pi->argv;
        printf("  cmd      ");
        for (int i = 0; i < pi->argc; i++) {
            printf("%s%s", i ? " " : "", a);
            a += strlen(a) + 1;
        }
        printf("%s\n", pi->argv_truncated ? " …" : "");
    }
    printf("  exe      %s%s\n", pi->exe[0] ? pi->exe : "(unreadable)",
           pi->exe_deleted ? "  (deleted)" : "");
    printf("  user     %s (uid %ld)   ppid %ld   threads %d\n", pi->user,
           pi->uid, pi->ppid, pi->threads);
    printf("  rss      %lu KB   fds %s%d   cpu %llu jiffies @ %lu Hz\n",
           pi->rss_kb, pi->fds_readable ? "" : "~", pi->n_fds,
           pi->utime + pi->stime, pi->clk_tck);
    printf("  attach   %s — %s\n",
           pi->attachable == 1 ? "YES" : pi->attachable == 0 ? "NO" : "MAYBE",
           pi->attach_why);
    if (pi->attach_remedy[0])
        printf("           -> %s\n", pi->attach_remedy);
    printf("  modes    ");
    for (int m = 0; m < ASMSPY_MODE__COUNT; m++)
        printf("%s%s", asmspy_mode_name((asmspy_mode_t)m),
               pi->mode_ok[m] ? " ok  " : " NO  ");
    printf("\n");
    for (int m = 0; m < ASMSPY_MODE__COUNT; m++)
        if (!pi->mode_ok[m])
            printf("           %-9s %s\n", asmspy_mode_name((asmspy_mode_t)m),
                   pi->mode_why[m]);
    printf("  names    %lu symbols · %d modules · %lu JIT methods%s%s\n",
           pi->syms_total, pi->n_modules, pi->jit_methods,
           pi->jit_source[0] ? " via " : "", pi->jit_source);
    printf("  threads\n");
    for (int i = 0; i < pi->n_threads_v; i++) {
        const asmspy_pi_thread_t *t = &pi->threads_v[i];
        printf("    %-8ld %-16.16s %c  %-24.24s %s\n", t->tid, t->comm,
               t->state ? t->state : '?', t->wchan[0] ? t->wchan : "-",
               t->have_syscall ? t->syscall_name : t->syscall_why);
    }
    if (pi->threads_truncated)
        printf("    … (capped at %d of %d)\n", pi->n_threads_v, pi->threads);
    if (pi->budget_exceeded)
        printf("  NOTE     the 250ms gather budget ran out — some sections "
               "carry only what they had\n");
}

static int cmd_info(pid_t pid, int json, const char *record) {
    asmspy_procinfo_t *pi = malloc(sizeof *pi);
    if (!pi) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    if (asmspy_procinfo(pid, pi) != 0) {
        fprintf(stderr, "no such process: %d\n", (int)pid);
        free(pi);
        return 1;
    }
    /* The two output channels are INDEPENDENT, exactly as every other mode
     * treats them: --json puts NDJSON on stdout, --record=<f> puts it in a
     * file, both does both, and --record with no --json still prints the
     * human text. Gating the recording on --json would silently drop a
     * recording the user asked for, which rec_open's own contract calls out
     * as never a detail. */
    if (json || record)
        info_emit_json(pi, record, json);
    if (!json)
        info_print_text(pi);
    free(pi);
    return 0;
}
```

- [ ] **Step 4: Wire the argv branch and usage**

In `main()`, beside the other subcommand branches (~line 8331, next to `--syms`):

```c
    if (strcmp(argv[1], "--info") == 0 && argc >= 3) {
        pid_t pid = (pid_t)atoi(argv[2]);
        int json = 0;
        const char *record = NULL;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--json") == 0)
                json = 1;
            else if (strncmp(argv[i], "--record=", 9) == 0)
                record = argv[i] + 9;
            else
                return usage(argv[0]);
        }
        return cmd_info(pid, json, record);
    }
```

In `usage()`, after the `--syms` line:

```c
        "  %s --info   <pid> [--json] [--record=<f>]  process snapshot: "
        "identity, runtime, threads (wchan + current syscall), symbols, and "
        "WHICH TRACING MODES will work on this target. Reads only /proc and "
        "the mapped ELF — it NEVER attaches, so it is safe to run against "
        "anything you can read, and safe to repeat\n"
```

Add one more `argv0` to the trailing argument list of that `fprintf` (the count must match the format's `%s` count exactly, or the build warns and the text shifts).

- [ ] **Step 5: Run the smoke case to verify it passes**

```bash
make cli && ./build/asmspy --info $$ && ./build/asmspy --info $$ --json | head -3
make cli-smoke 2>&1 | grep -A6 'attach-free process snapshot'
```
Expected: the text form prints; the JSON form's first line is an `asmtrace` header; the smoke section reports `--info OK` with a wall time well under 1000 ms.

- [ ] **Step 6: Define the `procinfo` kind in the schema**

In `docs/internal/gui/asmtrace-schema.md`, add a row to the **Reserved kinds** table:

```markdown
| `procinfo` | one attach-free process snapshot | [gui process-details](../superpowers/specs/2026-08-03-gui-process-details-tab-design.md) |
```

...then immediately below that table's trailing notes, add the definition paragraph in the same style as the `codeimage` / `mem` notes:

```markdown
`procinfo` is **defined** — see *`procinfo` — one attach-free process snapshot*
below. An ordinary recording event, emitted by `asmspy --info` as the sole event
of a one-event recording.
```

And add the full section near `codeimage` at the end of the file, carrying the JSON shape from this task's **Interfaces** block verbatim plus the four encoding rules (`syscall` omitted-with-`syscall_why`; addresses as hex strings; counts as numbers; `why`/`remedy` `""` not absent).

- [ ] **Step 7: Document `--info` for users**

Add a section to `docs/guides/tracing/asmspy.md` matching the page's existing per-flag style: what it shows, the "never attaches" guarantee, the `--json` recording contract, and a worked example against a shell's own `$$`.

Add a CHANGELOG entry under the unreleased heading:

```markdown
- `asmspy --info <pid>` — an attach-free process snapshot (identity, runtime,
  per-thread `wchan` + current syscall, symbol/JIT surface, and which tracing
  modes will work on the target). `--json` emits it as a one-event `.asmtrace`
  recording carrying the new `procinfo` kind.
```

- [ ] **Step 8: Verify the docs build and the container lane**

```bash
make docker-docs 2>&1 | tail -20
make docker-cli 2>&1 | tail -20
```
Expected: docs build clean (it is warnings-as-errors), `cli-smoke` PASS.

- [ ] **Step 9: Commit and push**

```bash
git add cli/asmspy.c cli/cli_smoke.sh docs/internal/gui/asmtrace-schema.md \
        docs/guides/tracing/asmspy.md CHANGELOG.md
git commit -m "cli: asmspy --info — the attach-free snapshot as text or a recording

--json emits header + one procinfo event + end, so it is a valid .asmtrace
by the same contract every other mode's --json carries, and the desktop
reads it with the ordinary loader instead of a bespoke JSON path.

Addresses go out as hex STRINGS: a JSON number is a double in many readers,
which silently rounds a 64-bit pointer. A thread with no readable syscall
omits the object and carries syscall_why instead of a blank.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
git push origin main
```

---

### Task 5: desktop model — parse `procinfo`, derive rates, label capability

**Files:**
- Create: `desktop/src/live/procinfo.h`
- Create: `desktop/src/live/procinfo.cpp`
- Create: `desktop/test/test_procinfo.cpp`
- Create: `desktop/test/fixtures/procinfo_full.asmtrace`
- Create: `desktop/test/fixtures/procinfo_refused.asmtrace`
- Modify: `mk/desktop.mk` (object dir + test link rule + `DESKTOP_TESTS`)

**Interfaces:**
- Consumes: the Task 4 wire shape; `Recording` / `Event` (`desktop/src/doc/recording.h`)
- Produces: `asmdesk::ProcInfo`, `procinfo_parse(const Recording&)`, `procinfo_rates(prev, cur)`, `procinfo_names_verdict(p)`. Tasks 6 and 7 consume these.

- [ ] **Step 1: Generate the fixtures from the real CLI**

```bash
./build/asmspy --info $$ --json > desktop/test/fixtures/procinfo_full.asmtrace
head -c 300 desktop/test/fixtures/procinfo_full.asmtrace
```

Then hand-edit a copy into `procinfo_refused.asmtrace` with `"attachable":0`, a non-empty `"why"` and `"remedy"`, `"ok":false` on every mode, `"threads_truncated":true`, and at least one thread carrying `syscall_why` instead of a `syscall` object. Fixtures are checked in so the tests need no live process.

- [ ] **Step 2: Write the failing test**

Create `desktop/test/test_procinfo.cpp`:

```cpp
// test_procinfo.cpp — the Process details model (the details pane's half that
// needs no subprocess and no ImGui).
//
// Three things here are wrong in ways nothing else would catch:
//
//  - RATES. The wire carries RAW jiffies and a monotonic stamp, never a rate,
//    so %CPU exists only as a difference between two snapshots. A first
//    snapshot must therefore report NO rate rather than 0% — "measured zero"
//    and "not yet measurable" are different claims, and one of them is a lie.
//
//  - ABSENCE. A thread with no readable syscall omits the object and carries
//    syscall_why. A model that folded that into an empty string would render
//    a blank cell, which reads as "doing nothing" — never true.
//
//  - HEX STRINGS. pc/sp/base/args cross the wire as strings precisely so a
//    64-bit pointer is not rounded through a double. Parsing them as numbers
//    passes every small-value test and corrupts every real address.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "doc/recording.h"
#include "live/procinfo.h"

#ifndef ASMTEST_FIXTURE_DIR
#error "ASMTEST_FIXTURE_DIR must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;

static int failures;

static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

static ProcInfo load(const char *name) {
    std::ifstream f(std::string(ASMTEST_FIXTURE_DIR) + "/" + name);
    Recording r = recording_load(f);
    return procinfo_parse(r);
}

int main() {
    ProcInfo p = load("procinfo_full.asmtrace");

    check("parsed", p.valid, p.parse_error);
    check("pid", p.pid > 0, "no pid");
    check("comm", !p.comm.empty(), "no comm");
    check("argv", !p.argv.empty(), "no argv");
    check("threads", !p.threads.empty(), "no threads");
    check("clk_tck", p.clk_tck > 0, "no tick rate");
    check("ts_ns", p.ts_ns > 0, "no timestamp");
    check("modes listed", p.modes.size() >= 9, "not every mode reported");

    // A refused mode ALWAYS carries its reason.
    for (const PiMode &m : p.modes)
        check("refused mode states why", m.ok || !m.why.empty(),
              "mode " + m.mode + " refused with an empty why");

    // Absence is preserved as absence, with a reason.
    for (const PiThread &t : p.threads)
        check("absent syscall states why", t.have_syscall || !t.why.empty(),
              "a thread with no syscall and no why");

    // Hex strings survive as full 64-bit values.
    {
        ProcInfo h = p;
        h.threads.clear();
        PiThread t;
        t.have_syscall = true;
        t.pc = procinfo_parse_hex("0xdeadbeefcafef00d");
        check("64-bit pc survives", t.pc == 0xdeadbeefcafef00dull,
              "hex parsed lossily — a double would have rounded this");
    }

    // --- rates: the first snapshot has NO rate -----------------------
    ProcRates r0 = procinfo_rates(ProcInfo{}, p);
    check("no rate from one snapshot", !r0.have,
          "a single snapshot cannot yield a rate — 0% would be a lie");

    // A second snapshot one second later, 50 jiffies busier at 100 Hz = 50%.
    ProcInfo q = p;
    q.ts_ns = p.ts_ns + 1000000000ull;
    q.utime = p.utime + 50;
    q.clk_tck = 100;
    q.io_read_bytes = p.io_read_bytes + 2048;
    ProcRates r1 = procinfo_rates(p, q);
    check("rate available", r1.have, "two snapshots must yield a rate");
    check("cpu 50%", r1.cpu_pct > 49.0 && r1.cpu_pct < 51.0,
          "cpu_pct = " + std::to_string(r1.cpu_pct));
    check("read 2048 B/s", r1.read_bps > 2040.0 && r1.read_bps < 2056.0,
          "read_bps = " + std::to_string(r1.read_bps));

    // A snapshot of a DIFFERENT process must never produce a rate: the
    // counters are unrelated and the difference is meaningless.
    ProcInfo other = q;
    other.pid = p.pid + 1;
    check("no cross-pid rate", !procinfo_rates(p, other).have,
          "rates derived across two different pids");

    // Backwards time (a cached snapshot replayed) yields no rate.
    ProcInfo back = p;
    back.ts_ns = p.ts_ns - 1000;
    check("no backwards rate", !procinfo_rates(p, back).have,
          "a negative interval produced a rate");

    // --- the names verdict -------------------------------------------
    check("names verdict non-empty", !procinfo_names_verdict(p).empty(),
          "no verdict");

    // --- the refused fixture ------------------------------------------
    ProcInfo x = load("procinfo_refused.asmtrace");
    check("refused parses", x.valid, x.parse_error);
    check("refused verdict", x.attachable == 0, "expected attachable 0");
    check("refused why", !x.attach_why.empty(), "empty why on a refusal");
    check("refused truncation stated", x.threads_truncated,
          "fixture sets threads_truncated");
    for (const PiMode &m : x.modes)
        check("all modes refused", !m.ok, "mode " + m.mode + " unexpectedly ok");

    // --- a non-procinfo recording is a stated failure, not a blank ----
    Recording empty;
    ProcInfo none = procinfo_parse(empty);
    check("empty is invalid", !none.valid, "an empty recording parsed as valid");
    check("empty says why", !none.parse_error.empty(),
          "invalid without a reason");

    if (failures) {
        std::fprintf(stderr, "test_procinfo: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_procinfo: all checks passed\n");
    return 0;
}
```

Confirm the loader entry point's real name before writing (`grep -n "Recording .*load" desktop/src/doc/recording.h`) and use it; `recording_load(std::istream&)` above is the expected spelling.

- [ ] **Step 3: Run it to verify it fails**

```bash
make build/desktop_test_procinfo
```
Expected: FAIL — no such target, then `live/procinfo.h: No such file`.

- [ ] **Step 4: Write `desktop/src/live/procinfo.h`**

```cpp
// procinfo.h — the Process details model (`asmspy --info`'s payload).
//
// The pane's facts arrive as ONE `procinfo` event in a one-event .asmtrace,
// so this file is a decode plus two derivations, and nothing else: no ImGui,
// no fork, no engine. That split is what lets the whole model be driven from
// checked-in fixtures with no live process.
//
// The one rule worth stating twice: the wire carries RAW counters and a
// monotonic stamp, never a rate. %CPU therefore EXISTS only as a difference
// between two snapshots, and a first snapshot must report that it has none —
// rendering 0% would claim a measurement that was never taken.
#ifndef ASMDESK_LIVE_PROCINFO_H
#define ASMDESK_LIVE_PROCINFO_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/recording.h"

namespace asmdesk {

struct PiThread {
    long tid = 0;
    std::string comm;
    char state = '?';
    std::string wchan;
    uint64_t cpu_jiffies = 0;
    bool have_syscall = false;
    long nr = -1;
    std::string name;
    std::vector<uint64_t> args;
    uint64_t pc = 0, sp = 0;
    std::string pc_sym;
    std::string why; // non-empty exactly when have_syscall is false
};

struct PiModule {
    std::string name, path;
    uint64_t base = 0, size = 0;
    bool exec = false;
    uint64_t syms = 0;
    // No has_symtab / build_id: the producer does not emit them (see the
    // asmspy_pi_module_t note). A field the wire never carries must not exist
    // here either, or the pane renders a default as if it were measured.
};

struct PiChild {
    long pid = 0;
    std::string comm;
};

struct PiMode {
    std::string mode; // "log" | "stream" | ... — the CLI/serve spelling
    bool ok = false;
    std::string why; // non-empty exactly when ok is false
};

struct ProcInfo {
    bool valid = false;
    std::string parse_error; // non-empty exactly when valid is false

    // identity
    long pid = 0, ppid = 0, pgid = 0, sid = 0;
    long uid = 0, euid = 0, gid = 0, egid = 0;
    std::string user, euser, comm;
    std::vector<std::string> argv;
    bool argv_truncated = false;
    std::string exe;
    bool exe_deleted = false;
    std::string cwd;
    char state = '?';
    uint64_t start_ticks = 0; // the cache key's second half — pid reuse guard
    double elapsed_s = 0;

    // runtime
    std::string runtime, evidence, interp;
    bool jitting = false, pie = false, static_linked = false;
    int elf_class = 0;

    // counters — RAW
    uint64_t ts_ns = 0, utime = 0, stime = 0;
    uint64_t clk_tck = 0;
    uint64_t rss_kb = 0, vsize_kb = 0, peak_rss_kb = 0;
    uint64_t io_read_bytes = 0, io_write_bytes = 0;
    bool io_readable = false, fds_readable = false;
    int fds = 0, oom_score = 0, nice = 0;
    int n_threads = 0; // the KERNEL's count; `threads` below is the rows we got

    std::vector<PiThread> threads; // capped at 64; n_threads may exceed it
    bool threads_truncated = false;

    uint64_t syms_total = 0, jit_methods = 0, anon_exec_bytes = 0;
    std::string jit_source;

    std::vector<PiModule> modules;
    bool modules_truncated = false;

    int attachable = -1; // 1 yes, 0 no, -1 unknown
    std::string attach_why, attach_remedy;
    std::vector<PiMode> modes;

    uint64_t ns_pid = 0, ns_net = 0, ns_mnt = 0, ns_user = 0;
    bool ns_differs = false;
    std::string cgroup;
    int seccomp = -1, no_new_privs = 0, dumpable = -1;

    std::vector<PiChild> children;
    bool children_truncated = false;
    bool budget_exceeded = false;
};

// Decode the single `procinfo` event of `r`. A recording without one yields
// valid=false and a stated parse_error — never a blank ProcInfo, which would
// render as a real process with every field zero.
ProcInfo procinfo_parse(const Recording &r);

// "0x1f" / "1f" -> 0x1f. Addresses cross the wire as hex STRINGS so a 64-bit
// pointer is not rounded through JSON's double; this is the one decoder.
uint64_t procinfo_parse_hex(const std::string &s);

struct ProcRates {
    bool have = false; // false = not yet measurable, NOT "measured zero"
    double cpu_pct = 0;
    double read_bps = 0, write_bps = 0;
};

// Derive rates from two snapshots OF THE SAME PROCESS. Returns have=false for
// a first snapshot, a different pid or start_ticks (pid reuse), a non-positive
// interval, or a missing tick rate — each of which makes the difference
// meaningless rather than zero.
ProcRates procinfo_rates(const ProcInfo &prev, const ProcInfo &cur);

// "a trace of this process will show names" / "...will show raw addresses",
// with the counts that justify it. The single sentence the code-surface
// numbers exist to support.
std::string procinfo_names_verdict(const ProcInfo &p);

} // namespace asmdesk
#endif // ASMDESK_LIVE_PROCINFO_H
```

Note the two thread fields are deliberately distinct and must not be merged: `n_threads` is the **kernel's** count from `/proc/<pid>/status`, and `threads` is the vector of rows actually gathered — capped at 64. When they disagree, `threads_truncated` is true and the pane says "capped at N of M". Collapsing them into one field is what makes a 200-thread process silently claim it has 64.

- [ ] **Step 5: Write `desktop/src/live/procinfo.cpp`**

Implement the three functions. The decode is mechanical `json` field reads with `.value(key, default)`; the two that carry judgement:

```cpp
uint64_t procinfo_parse_hex(const std::string &s) {
    if (s.empty())
        return 0;
    return std::strtoull(s.c_str(), nullptr, 16); // base 16 accepts "0x…" too
}

ProcRates procinfo_rates(const ProcInfo &prev, const ProcInfo &cur) {
    ProcRates r;
    // Every guard below marks a case where the DIFFERENCE is meaningless.
    // Reporting 0 instead would claim a measurement that was never taken.
    if (!prev.valid || !cur.valid)
        return r;
    if (prev.pid != cur.pid || prev.start_ticks != cur.start_ticks)
        return r; // a different process (or a reused pid)
    if (cur.ts_ns <= prev.ts_ns || cur.clk_tck == 0)
        return r;
    const double dt = double(cur.ts_ns - prev.ts_ns) / 1e9;
    const uint64_t pj = prev.utime + prev.stime, cj = cur.utime + cur.stime;
    if (cj < pj)
        return r; // counters went backwards: not a rate
    r.have = true;
    r.cpu_pct = 100.0 * (double(cj - pj) / double(cur.clk_tck)) / dt;
    if (cur.io_readable && prev.io_readable) {
        if (cur.io_read_bytes >= prev.io_read_bytes)
            r.read_bps = double(cur.io_read_bytes - prev.io_read_bytes) / dt;
        if (cur.io_write_bytes >= prev.io_write_bytes)
            r.write_bps = double(cur.io_write_bytes - prev.io_write_bytes) / dt;
    }
    return r;
}

std::string procinfo_names_verdict(const ProcInfo &p) {
    const uint64_t named = p.syms_total + p.jit_methods;
    if (named == 0)
        return "no symbols and no JIT map — a trace of this process will show "
               "raw addresses";
    std::string s = std::to_string(p.syms_total) + " symbols";
    if (p.jit_methods)
        s += " + " + std::to_string(p.jit_methods) + " JIT methods (" +
             (p.jit_source.empty() ? "unknown source" : p.jit_source) + ")";
    s += " — a trace of this process will show names";
    if (p.anon_exec_bytes && !p.jit_methods)
        s += ", except in " + std::to_string(p.anon_exec_bytes / 1024) +
             " KB of anonymous executable memory no symtab covers";
    return s;
}
```

`procinfo_parse` finds the first event with `kind == "procinfo"` in `r.by_kind`; if absent, set `parse_error` to `"no procinfo event in this recording"` and return. Decode each section into the struct, reading `pc`/`sp`/`base`/`args` through `procinfo_parse_hex`. A thread sets `have_syscall = body.contains("syscall")`, and otherwise takes `why` from `syscall_why`.

- [ ] **Step 6: Wire the build in `mk/desktop.mk`**

Add `procinfo` to whatever list generates `$(BUILD)/desktop/%/lv/*.o` from `desktop/src/live/*.cpp` (grep `lv/inspect.o` to find it). Then add the test link rule beside `desktop_test_inspect` (~line 1482):

```make
# The Process details model. Links procinfo.o + the doc model — no ImGui, no
# GL, no engines, and no subprocess: the whole model is driven from checked-in
# fixtures, which is why the pane's facts are testable with nothing running.
$(BUILD)/desktop/test/t/test_procinfo.o: DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"'
$(BUILD)/desktop_test_procinfo: $(BUILD)/desktop/test/t/test_procinfo.o \
    $(BUILD)/desktop/test/lv/procinfo.o $(DESKTOP_TEST_DOC)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@
```

Add `$(BUILD)/desktop_test_procinfo` to `DESKTOP_TESTS` (~line 1199).

- [ ] **Step 7: Run the test to verify it passes**

```bash
make build/desktop_test_procinfo && ./build/desktop_test_procinfo
```
Expected: `test_procinfo: all checks passed`

- [ ] **Step 8: Commit and push**

```bash
git add desktop/src/live/procinfo.h desktop/src/live/procinfo.cpp \
        desktop/test/test_procinfo.cpp desktop/test/fixtures/procinfo_full.asmtrace \
        desktop/test/fixtures/procinfo_refused.asmtrace mk/desktop.mk
git commit -m "desktop: the Process details model — parse, rates, names verdict

The wire carries RAW counters and a monotonic stamp, so %CPU exists only as
a difference between two snapshots. A first snapshot, a pid change, a reused
pid (start_ticks differs), a non-positive interval or backwards counters all
report have=false — rendering 0% would claim a measurement never taken.

Addresses decode through one hex-string path: parsing them as JSON numbers
passes every small-value test and corrupts every real 64-bit pointer.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
git push origin main
```

---

### Task 6: the runner — debounce, spawn, cache, timeout

**Files:**
- Modify: `desktop/src/live/procinfo.h` (append the runner)
- Modify: `desktop/src/live/procinfo.cpp` (implement it)
- Modify: `desktop/test/test_procinfo.cpp` (append the state-machine tests)
- Create: `desktop/test/fixtures/fake_asmspy_info.sh`

**Interfaces:**
- Consumes: `ProcInfo`, `procinfo_parse` (Task 5); `resolve_asmspy_path()` (`desktop/src/live/session.h:164`)
- Produces: `ProcInfoRunner`, `procinfo_tick(runner, selected_pid, now_s)`, `procinfo_current(runner)`, `procinfo_status(runner)`. Task 7 calls exactly these.

The clock is a parameter, never `now()` read internally — that is what makes the debounce, the refresh interval and the timeout assertable without sleeping.

- [ ] **Step 1: Write the failing test**

Append to `desktop/test/test_procinfo.cpp` before the final `if (failures)`:

```cpp
    // --- the runner: a pure state machine over an injected clock ------
    // Every timing rule here is a real hazard: without the debounce, arrowing
    // down a process table spawns one subprocess PER ROW; without the cache
    // key including start_ticks, a reused pid serves another process's card.
    {
        ProcInfoRunner run;
        run.asmspy_path = std::string(ASMTEST_FIXTURE_DIR) +
                          "/fake_asmspy_info.sh";

        // Selecting arms the debounce; it must NOT spawn on the same tick.
        procinfo_tick(run, 100, 0.0);
        check("no spawn on select", run.spawns == 0,
              "spawned immediately — arrowing a table would fork per row");
        procinfo_tick(run, 100, 0.10);
        check("no spawn before 250ms", run.spawns == 0, "debounce too short");

        // Moving the selection before expiry re-arms rather than spawning.
        procinfo_tick(run, 101, 0.20);
        procinfo_tick(run, 101, 0.40);
        check("re-armed by a new selection", run.spawns == 0,
              "a moving selection must re-arm, not spawn");

        // Stable past 250ms -> exactly one spawn.
        procinfo_tick(run, 101, 0.46);
        check("spawned after the debounce", run.spawns == 1,
              "no spawn after 250ms of a stable selection");
        procinfo_tick(run, 101, 0.50);
        check("no double spawn", run.spawns == 1, "spawned twice");

        // Drain the child, then the result is current.
        for (int i = 0; i < 200 && !procinfo_current(run).valid; i++)
            procinfo_tick(run, 101, 0.50 + 0.01 * i);
        check("result arrived", procinfo_current(run).valid,
              procinfo_current(run).parse_error);

        // A cached pid re-renders with NO new spawn.
        const int after = run.spawns;
        procinfo_tick(run, 100, 3.0);
        procinfo_tick(run, 100, 3.30);
        procinfo_tick(run, 101, 3.60);
        procinfo_tick(run, 101, 3.90);
        check("cache hit is instant", procinfo_current(run).valid,
              "a cached pid did not render");
        check("cache avoids a respawn", run.spawns <= after + 1,
              "a cached selection respawned");

        // The refresh timer only fires while visible.
        run.visible = false;
        const int before_hidden = run.spawns;
        procinfo_tick(run, 101, 10.0);
        check("hidden pane does not poll", run.spawns == before_hidden,
              "a hidden pane kept spawning");
    }

    // A child that never exits is killed and SAYS it timed out.
    {
        ProcInfoRunner run;
        run.asmspy_path = "/bin/sleep"; // never emits, never exits in time
        procinfo_tick(run, 100, 0.0);
        procinfo_tick(run, 100, 0.30); // spawn
        procinfo_tick(run, 100, 3.00); // past the 2s deadline
        check("timeout is stated", procinfo_status(run).find("timed out") !=
                                       std::string::npos,
              "a hung probe must say so: " + procinfo_status(run));
    }
```

- [ ] **Step 2: Create the fake asmspy**

`desktop/test/fixtures/fake_asmspy_info.sh` — mirrors the existing `fake_serve.sh` idiom (a script standing in for the real subprocess so the process path is tested without a tracer):

```sh
#!/bin/sh
# fake_asmspy_info.sh — stands in for `asmspy --info <pid> --json` so the
# runner's fork/exec/read/reap path is tested with no tracer and no live
# target. Echoes the checked-in full fixture regardless of its arguments.
cat "$(dirname "$0")/procinfo_full.asmtrace"
```

```bash
chmod +x desktop/test/fixtures/fake_asmspy_info.sh
```

- [ ] **Step 3: Run it to verify it fails**

```bash
make build/desktop_test_procinfo
```
Expected: FAIL — `'ProcInfoRunner' was not declared`.

- [ ] **Step 4: Append the runner to `desktop/src/live/procinfo.h`**

```cpp
// --- the runner: automatic on selection, without a fork per row ----------
//
// The pane probes AUTOMATICALLY as the selection moves, which is only safe
// because `asmspy --info` never attaches. Three rules keep it cheap as well
// as safe, and all three are timing, so the clock is a PARAMETER rather than
// read internally — that is what makes them assertable with no sleeping:
//
//  - a 250 ms DEBOUNCE, so arrowing down a table probes the row you stop on,
//    not every row you pass;
//  - a cache keyed on (pid, start_ticks) — the second half is the pid-reuse
//    guard, without which a recycled pid serves another process's card;
//  - a 2 s DEADLINE, after which the child is killed and the pane says so.
struct ProcInfoRunner {
    std::string asmspy_path; // "" -> resolve_asmspy_path()
    bool visible = true;     // the pane is shown AND the window is focused

    // Tunables, named so the test can pin them rather than guess.
    double debounce_s = 0.25;
    double refresh_s = 2.0;
    double deadline_s = 2.0;

    int spawns = 0; // lifetime count — the test's evidence of a fork

    // Everything below is internal state; the pane reads it through the
    // accessors, never directly.
    long want_pid = 0, in_flight_pid = 0;
    double want_since = -1, spawned_at = -1, last_ok_at = -1;
    int child_pid = 0, child_fd = -1;
    std::string buf, status;
    ProcInfo shown, prev;
    ProcRates rates;
    std::vector<std::pair<std::pair<long, uint64_t>, ProcInfo>> cache;
    static constexpr size_t kCacheCap = 32;

    ~ProcInfoRunner();
};

// Advance one frame. `selected_pid` is InspectState::selected_pid; `now_s` is
// a monotonic seconds clock (ImGui::GetTime() in the pane, a literal in the
// tests). Spawns, reads, reaps, expires and caches — the pane calls only this.
void procinfo_tick(ProcInfoRunner &r, long selected_pid, double now_s);

// The snapshot to draw (valid=false while nothing has arrived yet).
const ProcInfo &procinfo_current(const ProcInfoRunner &r);
// Rates against the previous snapshot of the SAME process (have=false until a
// second one lands).
const ProcRates &procinfo_current_rates(const ProcInfoRunner &r);
// A human line for the pane's header: how fresh, or what went wrong. Never
// empty — "nothing yet" is itself a state worth naming.
std::string procinfo_status(const ProcInfoRunner &r);
```

- [ ] **Step 5: Implement the runner in `desktop/src/live/procinfo.cpp`**

The shape, with the parts that carry judgement written out:

```cpp
namespace {
// Kill and reap, unconditionally. Called on every path that abandons a child
// — a new selection, a timeout, the pane closing, destruction — because the
// alternative is a probe that outlives the reason it was started.
void reap(ProcInfoRunner &r) {
    if (r.child_fd >= 0) {
        ::close(r.child_fd);
        r.child_fd = -1;
    }
    if (r.child_pid > 0) {
        ::kill(r.child_pid, SIGKILL);
        int st = 0;
        ::waitpid(r.child_pid, &st, 0);
        r.child_pid = 0;
    }
    r.buf.clear();
    r.in_flight_pid = 0;
}

// fork/exec `asmspy --info <pid> --json`, stdout on a NON-BLOCKING pipe: the
// UI thread polls it from the frame loop and never waits on the child.
bool spawn(ProcInfoRunner &r, long pid) { /* pipe, fork, dup2, execvp */ }
} // namespace

void procinfo_tick(ProcInfoRunner &r, long selected_pid, double now_s) {
    if (selected_pid != r.want_pid) {
        r.want_pid = selected_pid;
        r.want_since = now_s;
        reap(r); // abandon the in-flight probe for a target nobody is viewing
        r.shown = ProcInfo{};
        r.rates = ProcRates{};
        r.status = selected_pid > 0 ? "reading…" : "no process selected";
        // A cache hit renders immediately; a refresh may still follow.
        for (auto &e : r.cache)
            if (e.first.first == selected_pid) {
                r.shown = e.second;
                r.status = "cached";
                break;
            }
    }
    if (selected_pid <= 0)
        return;

    // read / reap / parse the in-flight child ...
    // (non-blocking read into r.buf; on EOF parse and cache; on deadline
    //  reap and set status to "timed out after 2.0s — the probe was killed")

    const bool idle = r.child_pid == 0;
    const bool debounced = r.want_since >= 0 &&
                           now_s - r.want_since >= r.debounce_s;
    const bool due = r.last_ok_at < 0 || now_s - r.last_ok_at >= r.refresh_s;
    if (idle && debounced && r.visible && due)
        spawn(r, selected_pid);
}
```

On a successful parse: set `r.prev = r.shown` first, then `r.shown = parsed`, then `r.rates = procinfo_rates(r.prev, r.shown)`, `r.last_ok_at = now_s`, cache under `{pid, shown.start_ticks}` evicting the oldest past `kCacheCap`, and set `status` to `"read <n.n>s ago · attach-free (no ptrace)"`.

`~ProcInfoRunner()` calls `reap`.

- [ ] **Step 6: Run the test to verify it passes**

```bash
make build/desktop_test_procinfo && ./build/desktop_test_procinfo
```
Expected: `test_procinfo: all checks passed`

- [ ] **Step 7: Commit and push**

```bash
git add desktop/src/live/procinfo.h desktop/src/live/procinfo.cpp \
        desktop/test/test_procinfo.cpp desktop/test/fixtures/fake_asmspy_info.sh
git commit -m "desktop: the procinfo runner — debounce, cache, deadline

Probing automatically on selection is only cheap if three timing rules hold,
so the clock is a parameter and all three are asserted without sleeping: a
250ms debounce (else arrowing a table forks per row), a (pid, start_ticks)
cache key (else a reused pid serves another process's card), and a 2s
deadline after which the child is killed and the pane says it timed out.

Every path that abandons a child kills and reaps it, because a probe that
outlives the reason it was started is the whole hazard.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
git push origin main
```

---

### Task 7: the pane — registration, draw, and a null-backend test

**Files:**
- Modify: `desktop/src/ui/layout.cpp:23` (add `kPaneDetails`)
- Modify: `desktop/src/ui/layout.h` (declare it)
- Modify: `desktop/src/ui/shell.cpp` (`pctx_details`, `kManagedPanes`, `mode_wants_pane`, the `Begin` block)
- Modify: `desktop/src/ui/doors.h` (add `ProcInfoRunner details;` to `InspectState`, declare `draw_details_pane`)
- Create: `desktop/src/ui/details_pane.cpp`
- Create: `desktop/test/test_details_draw.cpp`
- Modify: `mk/desktop.mk`

**Interfaces:**
- Consumes: `ProcInfoRunner`, `procinfo_tick`, `procinfo_current`, `procinfo_current_rates`, `procinfo_status`, `procinfo_names_verdict` (Tasks 5-6); `InspectState::selected_pid` (`doors.h:157`); `dt_cell_magnitude_bar` / `dt_magnitude_frac`
- Produces: `void draw_details_pane(InspectState &s);` and the pane constant `kPaneDetails`

- [ ] **Step 1: Write the failing draw test**

Create `desktop/test/test_details_draw.cpp`, modelled on the tree's other null-backend draw tests (`grep -l ImGuiTestEngine desktop/test/test_*_draw.cpp` for the exact harness idiom to copy):

```cpp
// test_details_draw.cpp — the Process details pane renders in all three of
// its real states without a live process: nothing selected, a full snapshot,
// and a refusal. Drives ImGui's null backend.
//
// The assertion that matters is that the REFUSAL renders: a pane that draws
// beautifully with data and blanks on an error is a pane whose most important
// frame was never seen.
#include <cstdio>
#include <fstream>

#include "imgui.h"
#include "live/procinfo.h"
#include "ui/doors.h"

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

int main() {
    // ... null-backend setup, copied from the sibling draw tests ...

    InspectState s;

    // 1. nothing selected
    s.selected_pid = 0;
    // NewFrame / draw_details_pane(s) / Render
    check("no-selection frame", true, "drew without a selection");

    // 2. a full snapshot, injected straight into the runner
    {
        std::ifstream f(std::string(ASMTEST_FIXTURE_DIR) +
                        "/procinfo_full.asmtrace");
        Recording r = recording_load(f);
        s.selected_pid = 4242;
        s.details.shown = procinfo_parse(r);
        check("fixture parsed", s.details.shown.valid, "bad fixture");
    }
    // NewFrame / draw_details_pane(s) / Render

    // 3. a refusal
    {
        std::ifstream f(std::string(ASMTEST_FIXTURE_DIR) +
                        "/procinfo_refused.asmtrace");
        Recording r = recording_load(f);
        s.details.shown = procinfo_parse(r);
        s.details.status = "timed out after 2.0s — the probe was killed";
    }
    // NewFrame / draw_details_pane(s) / Render
    check("refusal frame", true, "drew a refusal without crashing");

    if (failures) {
        std::fprintf(stderr, "test_details_draw: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_details_draw: all checks passed\n");
    return 0;
}
```

- [ ] **Step 2: Run it to verify it fails**

```bash
make build/desktop_test_details_draw
```
Expected: FAIL — no such target, then `'draw_details_pane' was not declared`.

- [ ] **Step 3: Add the pane constant**

`desktop/src/ui/layout.cpp`, after `kPanePtSlice` (line 23):

```cpp
const char *const kPaneDetails = "Process details";
```

Declare it in `desktop/src/ui/layout.h` beside the other `kPane*` externs.

- [ ] **Step 4: Add the runner to `InspectState` and declare the draw entry**

In `desktop/src/ui/doors.h`, add `#include "live/procinfo.h"` and, inside `InspectState` beside `proc_filter` (~line 160):

```cpp
    // The Process details pane's probe runner. It spawns `asmspy --info` —
    // which never attaches — so it needs no host, no budget and no jack, and
    // is driven purely by selected_pid.
    ProcInfoRunner details;
```

Declare beside `draw_capture_pane`:

```cpp
void draw_details_pane(InspectState &s);
```

- [ ] **Step 5: Register the pane in `shell.cpp`**

Add the context gate beside `pctx_capture` (~line 3157):

```cpp
// Process details needs ONLY a selection — deliberately not pctx_capture's
// host_started, because `asmspy --info` is a one-shot spawn rather than a
// serve session. The tab therefore works before a host is ever connected,
// which is exactly when the operator is deciding whether this is the right
// process.
static bool pctx_details(const ShellState &s) {
    return s.inspect.selected_pid > 0;
}
```

Add to `kManagedPanes` immediately after the `kPaneProcesses` row:

```cpp
    {kPaneDetails, false, pctx_details,
     "pick a process in the Processes pane first"},
```

Add `kPaneDetails` to the Capture-mode arm of `mode_wants_pane` (~line 3246), beside `kPaneProcesses`. Add the `Begin` block beside the `kPaneProcesses` one (~line 3668):

```cpp
    if (pane_shown(s, kPaneDetails)) {
        bool open = true;
        if (ImGui::Begin(kPaneDetails, &open))
            draw_details_pane(s.inspect);
        ImGui::End();
        if (!open)
            s.pane_open[kPaneDetails] = false;
    }
```

- [ ] **Step 6: Write `desktop/src/ui/details_pane.cpp`**

```cpp
// details_pane.cpp — the Process details pane.
//
// It probes AUTOMATICALLY as the Processes selection moves, which is only
// defensible because `asmspy --info` never attaches: browsing a process list
// costs its targets nothing. The header says so out loud, because an operator
// who believes a details pane is attaching will avoid using it.
//
// Two sections are open by default — "Can I trace this?" and "What is it doing
// now" — because they are the two that change what the operator does next.
#include "imgui.h"
#include "live/procinfo.h"
#include "ui/doors.h"
#include "ui/palette.h"

namespace asmdesk {

void draw_details_pane(InspectState &s) {
    s.details.visible = ImGui::IsWindowFocused(
        ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_DockHierarchy);
    procinfo_tick(s.details, s.selected_pid, ImGui::GetTime());

    if (s.selected_pid <= 0) {
        ImGui::TextDisabled("pick a process in the Processes pane first.");
        return;
    }
    const ProcInfo &p = procinfo_current(s.details);
    // The status line is NEVER empty — "nothing yet" is a state worth naming.
    ImGui::TextDisabled("%s", procinfo_status(s.details).c_str());
    if (s.ssh_host[0])
        ImGui::TextDisabled(
            "note: this pid is LOCAL. The capture host is %s — the Processes "
            "list is this machine's /proc.",
            s.ssh_host);
    if (!p.valid) {
        if (!p.parse_error.empty())
            ImGui::TextWrapped("%s", p.parse_error.c_str());
        return;
    }
    // header, then the sections of the design's layout ...
}

} // namespace asmdesk
```

Fill the sections per the spec's layout: header (`pid`, `comm`, runtime badge, state, argv, status line); `Can I trace this?` (verdict + why/remedy, the mode grid from `p.modes`, `procinfo_names_verdict(p)`); `What is it doing now` (the thread table — tid, comm, state, and `wchan` or `syscall_name`, falling back to the thread's `why`); `Resources` (rates from `procinfo_current_rates`, each magnitude via `dt_cell_magnitude_bar(dt_magnitude_frac(v, max), dt_dim_u32())`, and an em dash where `have == false` — never `0%`); then collapsed `Identity`, `Code surface`, `Containment`, `Children`. Every `*_truncated` flag renders a stated "… (capped at N of M)" line.

- [ ] **Step 7: Wire the build**

Add `details_pane` to the `DESKTOP_UI` source list (grep `inspect_door` in `mk/desktop.mk` to find every list it appears in — app, render, test and uitest variants). Add the draw-test rule beside `desktop_test_loom_draw`:

```make
$(BUILD)/desktop/test/t/test_details_draw.o: DESKTOP_TEST_EXTRA = -DASMTEST_FIXTURE_DIR='"desktop/test/fixtures"'
$(BUILD)/desktop_test_details_draw: $(BUILD)/desktop/test/t/test_details_draw.o \
    $(BUILD)/desktop/test/lv/procinfo.o $(BUILD)/desktop/test/ui/details_pane.o \
    $(DESKTOP_TEST_UI_OBJ) $(DESKTOP_TEST_DOC) $(DESKTOP_TEST_IG)
	$(CXX) $(DESKTOP_CXXFLAGS) $^ -o $@
```

Add `$(BUILD)/desktop_test_details_draw` to `DESKTOP_TESTS`.

- [ ] **Step 8: Run the tests to verify they pass**

```bash
make build/desktop_test_details_draw && ./build/desktop_test_details_draw
./build/desktop_test_procinfo
```
Expected: both print `all checks passed`.

- [ ] **Step 9: Verify the whole desktop suite and the container lanes**

```bash
make desktop-test 2>&1 | tail -30
make docker-desktop 2>&1 | tail -20
make desktop-ui-test 2>&1 | tail -20
```
Expected: green. Note `test_shell`'s attach/no-host FAILs are **pre-existing** — verify any test you touched individually rather than trusting the for-loop's exit code.

- [ ] **Step 10: See it actually work**

```bash
make desktop && ./build/asmtest-desktop
```
Click a row in Processes; the Process details tab should populate within ~300 ms and refresh every 2 s. Arrow down several rows quickly and confirm it probes only where you stop.

- [ ] **Step 11: Commit and push**

```bash
git add desktop/src/ui/layout.cpp desktop/src/ui/layout.h desktop/src/ui/shell.cpp \
        desktop/src/ui/doors.h desktop/src/ui/details_pane.cpp \
        desktop/test/test_details_draw.cpp mk/desktop.mk
git commit -m "desktop: the Process details pane

Probes automatically as the Processes selection moves. Gated on the
SELECTION alone, not host_started: asmspy --info is a one-shot spawn rather
than a serve session, so the tab works before a host is ever connected —
which is exactly when you are deciding whether this is the right process.

The header states 'attach-free (no ptrace)', because an operator who
believes a details pane is attaching will avoid using it. With an ssh host
configured it also states that the pid is local while the capture host is
elsewhere, rather than implying they match.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
git push origin main
```

---

## Self-review

**Spec coverage.** Every spec section maps to a task: the `--info` command and its `.asmtrace` envelope → Task 4; identity/runtime/counters/containment/children → Task 1; threads/wchan/syscall → Task 2; code surface, modules, traceability → Task 3; the desktop model and rate derivation → Task 5; debounce/cache/refresh/timeout → Task 6; pane registration, layout, refusals and the ssh statement → Task 7. The 250 ms budget is implemented in Task 1 (`pi_over_budget`) and surfaced in Tasks 4 and 7. All four caps are pinned in the Task 1 struct and their truncation flags asserted in Tasks 1-3 and rendered in Task 7. The "never ptrace" rule is a Global Constraint and is *tested* — Task 1 snapshots a process the test holds no attach permission for.

**Type consistency.** `asmspy_procinfo_t` is defined once (Task 1) and only extended thereafter. The wire shape is stated once (Task 4 Interfaces) and mirrored field-for-field by `ProcInfo` (Task 5). `procinfo_tick` / `procinfo_current` / `procinfo_current_rates` / `procinfo_status` are declared in Task 6 and called with those exact names in Task 7. One naming hazard is called out inline in Task 5 Step 4: the vector must be `ProcInfo::threads` and the kernel count `ProcInfo::n_threads`, since the tests read `p.threads` as the vector.

**Known soft spots**, flagged rather than hidden — each is a "confirm before writing" note in its step, not a placeholder:
- `asmspy_syscall_name`'s real accessor (Task 2 Step 3) — the fallback is stated: render the decimal, never invent a name.
- The `Recording` loader's exact entry point (Task 5 Step 2).
- The null-backend harness idiom (Task 7 Step 1) — copy from a sibling `*_draw` test.
- `mk/desktop.mk`'s per-variant source lists (Tasks 5 and 7) — grep `inspect_door` for every list to touch.
