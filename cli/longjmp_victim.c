/* longjmp_victim.c — the case a push/pop depth COUNTER cannot get right
 * (asmspy-plan Theme C).
 *
 * main() setjmp()s, calls three functions deep, and the innermost longjmp()s
 * straight back. Then it calls after_jump() from main, at depth 0.
 *
 * That last call is the whole test. longjmp restores rsp and rip directly: the
 * three intervening frames are discarded WITHOUT a single `ret` retiring. A
 * tracer that counts +1 per CALL and -1 per RET therefore never comes back down
 * — it sits at 3 forever, and every line it prints from then on is indented
 * three levels too deep, permanently, on a process that is behaving perfectly
 * normally.
 *
 * A real return-address stack keyed on rsp sees the stack pointer jump back
 * above all three frames and pops them, so after_jump renders at depth 0 where
 * it belongs. Same binary, same run — only the indentation differs, which is
 * what makes this a clean negative control.
 *
 * Opts in via PR_SET_PTRACER_ANY like the other victims.
 */
#include <setjmp.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

static jmp_buf jb;
static volatile long sink;

/* depth 2 from main: jumps out without returning through anything */
__attribute__((noinline)) static void jump_out(void) {
    sink++;
    longjmp(jb, 1);
}
/* depth 1 from main */
__attribute__((noinline)) static void level_two(void) {
    sink++;
    jump_out();
}
/* depth 0 from main */
__attribute__((noinline)) static void level_one(void) {
    sink++;
    level_two();
}

/* Called from main at depth 0, AFTER the longjmp discarded three frames.
 * Its rendered depth is the assertion. */
__attribute__((noinline)) static void after_jump(long x) { sink += x * 3 + 1; }

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    fprintf(stderr, "longjmp_victim pid=%d\n", (int)getpid());
    fflush(stderr);
    for (long i = 0;; i++) {
        if (setjmp(jb) == 0)
            level_one(); /* -> level_two -> jump_out -> longjmp back here */
        after_jump(i);   /* depth 0: three frames were just discarded */
        usleep(5 * 1000);
    }
    return 0;
}
