/*
 * pin_probe_victim.c — the native capture fixture for the Intel Pin PROBE-MODE
 * argument/return capture lane (PIN-3, pin-probe-mode-capture.md).
 *
 * Two roles, selected by argv[1]:
 *
 *   pin_probe_victim capture|badptr|skip
 *       Run UNDER Pin — `pin -probe -t probe_capture.so -func <fn> -shm /name --
 *       pin_probe_victim <mode>`. Pin's probe tool splices a jump at the target
 *       routine and records its args/return into the shm channel it owns; this
 *       program merely CALLS the routine so a probe fires, then exits. It does not
 *       touch the shm at all (the tool creates/owns it).
 *         capture — call capref() with the known-good args (the primary capture).
 *         badptr  — call capref() with a deliberately invalid buf pointer, to prove
 *                   the tool REFUSES the bad pointer (PIN_SafeCopy) without faulting
 *                   the victim (capref guards its own deref, so it survives too).
 *         skip    — call tiny(), an un-probeable leaf, to prove the tool reports an
 *                   explicit per-target skip with a reason.
 *
 *   pin_probe_victim attach-loop
 *       Run NATIVELY (no Pin) as the OUT-OF-PROCESS ptrace reference producer's
 *       target: print `victim pid=<pid> capref=<addr>` to stderr, then loop calling
 *       capref() with the SAME known-good args so a separate tracer (the validator)
 *       can PTRACE_ATTACH, run_to(capref), and read the same registers Pin saw —
 *       the two independent producers must agree (mirrors examples/attach_victim.c).
 *
 * It opts in to being traced by any same-uid process via PR_SET_PTRACER_ANY so the
 * attach-loop role works under a plain container even at Yama ptrace_scope=1.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "asmtest_valtrace_shm.h"

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

/* The capture target. SysV: a->RDI, b->RSI, d->XMM0, buf->RDX; returns
 * a + b + (long)d + buf[0] in RAX. The buf[0] term is added only for a plausibly
 * valid pointer (>= the first page) so the deliberately-invalid-pointer sub-run
 * (badptr, buf == 0x1) cannot fault the victim — the whole point of that sub-run is
 * that the TOOL refuses the bad pointer, and the victim must survive to prove it.
 * noinline forces a real call so the entry/exit probes have a stable splice point. */
__attribute__((noinline)) long capref(long a, long b, double d,
                                      const char *buf) {
    long r = a + b + (long)d;
    if ((uintptr_t)buf >= 0x1000)
        r += (unsigned char)buf[0];
    return r;
}

/* A deliberately un-probeable leaf (T5): its whole body is a single one-byte `ret`,
 * far below the 14-byte Pin-probe floor. The tool reads its true RTN_Size (measured:
 * 1) and reports a concrete AV_SKIP_TOO_SHORT rather than probing a sub-floor routine
 * (which relies on Pin relocating the whole routine and "may destabilize the
 * application"). Defined in global asm so it is exactly `ret` — no compiler frame —
 * with ELF symbol size 1; it is safe to execute (returns immediately). `.globl` keeps
 * the symbol for RTN_FindByName. */
__asm__(".text\n"
        ".globl tiny\n"
        ".type tiny, @function\n"
        "tiny:\n"
        "    ret\n"
        ".size tiny, .-tiny\n");
void tiny(void);

static volatile long g_sink;

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "capture";

    /* Let any same-uid process attach (Yama opt-in); harmless where scope==0. */
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    fprintf(stderr, "victim pid=%d capref=%p tiny=%p mode=%s\n", (int)getpid(),
            (void *)capref, (void *)tiny, mode);
    fflush(stderr);

    if (strcmp(mode, "attach-loop") == 0) {
        /* The ptrace reference target: loop so a separate tracer has time to
         * attach and catch a call, with the SAME args as the Pin run. */
        for (;;) {
            g_sink +=
                capref(AV_CAPREF_A, AV_CAPREF_B, AV_CAPREF_D, AV_CAPREF_BUF);
            usleep(200 * 1000); /* ~5 Hz */
        }
        return 0; /* unreachable */
    }

    if (strcmp(mode, "skip") == 0) {
        tiny(); /* the un-probeable (sub-floor) target; the skip is decided at
                 * image-load time regardless, but calling it is harmless */
        g_sink += 1;
    } else if (strcmp(mode, "badptr") == 0) {
        /* Deliberately invalid pointer: capref guards its deref (survives), and the
         * tool must REFUSE the bad buffer via PIN_SafeCopy rather than fault. */
        g_sink +=
            capref(AV_CAPREF_A, AV_CAPREF_B, AV_CAPREF_D, (const char *)0x1);
    } else { /* "capture" */
        g_sink += capref(AV_CAPREF_A, AV_CAPREF_B, AV_CAPREF_D, AV_CAPREF_BUF);
    }

    /* Under Pin the exit probe sets the shm done flag synchronously as capref
     * returns; a brief pause keeps the victim alive a beat longer for robustness. */
    usleep(20 * 1000);
    return 0;
}
