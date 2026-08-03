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
