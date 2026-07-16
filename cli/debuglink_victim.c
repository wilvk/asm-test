/* debuglink_victim.c — a victim for the separate-debug-info smoke
 * (.gnu_debuglink + build-id resolution in cli/asmspy_proc.c).
 *
 * The smoke STRIPS a copy of this binary and attaches the debug info back as a
 * separate file, reproducing what a distro ships: /usr/bin/foo with no .symtab
 * and its symbols in a -dbg(sym) package. So the one thing that matters here is
 * that debuglink_only_fn survives into .symtab (the debug file's copy) and is
 * absent from .dynsym — i.e. that after `strip --strip-all` it is genuinely
 * unresolvable WITHOUT the debug file, which is what gives the smoke's negative
 * control its teeth. Plain (not static) so it cannot be inlined away, and never
 * exported: an executable's .dynsym carries only what dynamic linking needs, so
 * a locally-called function is not in it (verified — the smoke asserts it).
 *
 * It returns a known value from known arguments so the smoke can also drive
 * --trace against the recovered symbol: that proves the address resolved out of
 * the separate debug file is the real RUNTIME address (the load bias applied),
 * not merely a name that reappeared. Opts in via PR_SET_PTRACER_ANY like the
 * other victims so attach works in a plain container.
 */
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

/* debuglink_only_fn(6, 7) == 51 — a value distinct from the other victims' so a
 * copy-pasted assertion cannot pass by accident. */
__attribute__((noinline)) long debuglink_only_fn(long a, long b) {
    long s = 0;
    for (long i = 0; i < 3; i++) /* a loop: --trace has real steps to count */
        s += a;
    return s * b / 3 + 9;
}

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    fprintf(stderr, "debuglink_victim pid=%d\n", (int)getpid());
    fflush(stderr);
    volatile long sink = 0;
    for (;;) {
        sink += debuglink_only_fn(6, 7);
        usleep(50 * 1000); /* hot enough for --trace to catch an invocation */
    }
    return 0;
}
