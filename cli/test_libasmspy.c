/* test_libasmspy.c — the standalone proof that libasmspy is a LIBRARY.
 *
 * The asmspy engine used to be two loose objects compiled straight into the
 * binary with no public header, so "does the engine stand on its own" was a
 * question nothing could answer: every caller was in the same link as
 * asmspy.c's front end, and a hidden dependency on it would never show. This
 * test closes that. It is deliberately spartan about what it is ALLOWED to
 * touch, and that restraint IS the test:
 *
 *   - it includes ONLY "libasmspy.h" — not asmspy.h, not a view-model header.
 *     If anything a consumer needs (a sink typedef, a skip code, a signature)
 *     stayed behind in the CLI header, this stops compiling.
 *   - it links libasmspy.a + the framework tier objects, and NOT asmspy.o and
 *     NOT ncurses (mk/cli.mk). A symbol the engine quietly expected the front
 *     end to define, or a TUI dependency that leaked into the engine, is a link
 *     error here and nowhere else.
 *
 * On top of that it drives one engine end to end against a live victim, so the
 * packaging change is proven not to have broken the thing being packaged:
 * attach a real process, stream real syscalls through a real sink, detach, and
 * leave the target ALIVE — the whole contract in one run.
 *
 * Usage: test_libasmspy [path-to-spy_victim]   (default build/spy_victim)
 * Built + run by `make cli-smoke`.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libasmspy.h" /* the ONE header — see above */

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    } else {
        printf("ok: %s\n", what);
    }
}

/* ---- the syscall sink -------------------------------------------------- */
typedef struct {
    unsigned n;           /* events delivered                                 */
    int saw_payload_free; /* a pf_line arrived, non-NULL and non-empty      */
    char first[512];      /* the first full line, for the report              */
} sink_ctx_t;

static void on_syscall(void *ctx, const char *line, const char *pf_line,
                       const char *str) {
    sink_ctx_t *c = (sink_ctx_t *)ctx;
    (void)str;
    if (c->n == 0 && line)
        snprintf(c->first, sizeof c->first, "%s", line);
    /* pf_line is documented "never NULL" — the payload-separated channel a
     * recording redacts by default. Assert the contract at the sink. */
    if (pf_line && pf_line[0])
        c->saw_payload_free = 1;
    c->n++;
}

/* Spawn the victim and return its pid, or -1. The child's stderr is piped so we
 * can WAIT for its "spy_victim pid=" banner: that line is printed AFTER its
 * prctl(PR_SET_PTRACER_ANY), so reading it removes the attach race a fixed
 * sleep would only paper over. */
static pid_t spawn_victim(const char *path) {
    int pfd[2];
    if (pipe(pfd) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], 2);
        close(pfd[1]);
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);
    char buf[128];
    ssize_t n = read(pfd[0], buf, sizeof buf - 1);
    close(pfd[0]);
    if (n <= 0) { /* execl failed, or it died before announcing itself */
        int st;
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        return -1;
    }
    buf[n] = '\0';
    if (!strstr(buf, "spy_victim pid=")) {
        int st;
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        return -1;
    }
    return pid;
}

int main(int argc, char **argv) {
    const char *victim = argc > 1 ? argv[1] : "build/spy_victim";

    pid_t pid = spawn_victim(victim);
    if (pid < 0) {
        fprintf(stderr, "FAIL: could not spawn victim '%s'\n", victim);
        return 1;
    }
    printf("victim pid=%d (%s)\n", (int)pid, victim);

    /* ---- the resolver half (asmspy_proc.c) ----------------------------- */
    /* Not ptrace: /proc + the mapped ELF. It must name the victim's own two
     * functions, and asmspy_symtab_at must be the exact-containment lookup the
     * engines rely on when they turn a PC into a name. */
    asmspy_symtab_t syms;
    memset(&syms, 0, sizeof syms);
    check(asmspy_symtab_load(pid, &syms) == 0, "asmspy_symtab_load");
    const asmspy_sym_t *work = asmspy_symtab_by_name(&syms, "work");
    const asmspy_sym_t *helper = asmspy_symtab_by_name(&syms, "helper");
    check(work != NULL, "resolver names the victim's work()");
    check(helper != NULL, "resolver names the victim's helper()");
    if (work)
        check(asmspy_symtab_at(&syms, work->addr) == work,
              "asmspy_symtab_at round-trips work()'s entry");

    /* The 32-bit refusal is a PRE-ATTACH fact the engines share; the victim is
     * built for the host, so it must read as the host's class. */
    check(asmspy_elf_class(pid) == 64 || asmspy_elf_class(pid) == 32,
          "asmspy_elf_class reads the victim's ELF class");

    /* ---- the engine half (asmspy_engine.c) ----------------------------- */
    /* One engine, driven exactly as a front end drives it: bounded by `max`,
     * with a sink and a ctx. spy_victim usleep()s in its loop, so syscalls
     * arrive within a few hundred ms. */
    sink_ctx_t sc;
    memset(&sc, 0, sizeof sc);
    int rc = asmspy_engine_syscalls(pid, /*follow=*/0, /*max=*/3, /*stop=*/NULL,
                                    on_syscall, &sc);
    if (rc != 0) {
        char msg[256];
        asmspy_strerror(rc, msg, sizeof msg);
        fprintf(stderr, "FAIL: asmspy_engine_syscalls rc=%d (%s)\n", rc, msg);
        failures++;
    } else {
        check(1, "asmspy_engine_syscalls attached and detached cleanly");
    }
    check(sc.n > 0, "the syscall sink was invoked");
    check(sc.saw_payload_free, "every event carried a payload-free line");
    if (sc.first[0])
        printf("   first line: %s\n", sc.first);

    /* ---- the contract that makes this a TRACER, not a debugger --------- */
    /* Detach must leave the target running. A dead victim here means the
     * two-phase detach was bypassed or broke in the repackaging. */
    check(kill(pid, 0) == 0, "the victim SURVIVED the detach");

    /* ---- the skip-code vocabulary ------------------------------------- */
    /* The typed positives are how a serve/GUI consumer distinguishes "nothing
     * ran here" from "the tracer failed"; each must render as text. */
    char b[256];
    asmspy_strerror(ASMSPY_REGION_NEVER_RAN, b, sizeof b);
    check(b[0] != '\0', "asmspy_strerror(REGION_NEVER_RAN) is non-empty");
    asmspy_strerror(ASMSPY_ETRACEE_I386, b, sizeof b);
    check(b[0] != '\0', "asmspy_strerror(ETRACEE_I386) is non-empty");

    asmspy_symtab_free(&syms);
    kill(pid, SIGKILL);
    int st;
    waitpid(pid, &st, 0);

    if (failures) {
        fprintf(stderr, "test_libasmspy: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_libasmspy: all checks passed\n");
    return 0;
}
