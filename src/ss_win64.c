/* ss_win64.c — Windows x86-64 VEH single-step front-end (single-step plan,
 * Phase 5). The same CPU mechanism as src/ss_backend.c — EFLAGS.TF raises #DB
 * after every instruction — but delivered as EXCEPTION_SINGLE_STEP to a Vectored
 * Exception Handler instead of a SIGTRAP handler. Like the Linux/macOS stepper,
 * the handler records the in-region RIP offset and re-asserts TF in the resumed
 * CONTEXT; the region filter drops the out-of-region steps around the call.
 *
 * Stepping is ENDED BY THE HANDLER, not by a flag-clearing instruction: the
 * trampoline publishes the address the traced call returns to (stop_rip), and
 * the trap that lands there is resumed with TF cleared in the CONTEXT. A
 * popfq-based disarm — the Linux stepper's idiom — is NOT reliable here: the
 * hardware suppresses the trap after a POPF that clears TF, but a resume
 * through NtContinue (Wine's, at least) does not reproduce that
 * microarchitectural suppression, so TF survived into the caller and the next
 * trap fired after the stepper had deactivated — an unhandled
 * EXCEPTION_SINGLE_STEP. Ending in the handler is exact on both paths.
 *
 * Scope mirrors the tier's MVP contract: one active stepper, armed and disarmed
 * on the same thread, tracing a deterministic leaf-oriented routine (the args go
 * in RCX/RDX/R8/R9 per the Win64 ABI). Windows has no blocked-signal analogue of
 * the POSIX pthread_create hazard (exception dispatch is not maskable the way a
 * signal is), but the single-active-stepper gate still keys on the arming thread
 * id so a stray EXCEPTION_SINGLE_STEP on another thread is passed on, not eaten.
 *
 * Internal to the win64 native tier (built by mk/win64.mk into the PE test
 * lanes); the seam for a future libasmtest_hwtrace Windows port is the same
 * frame shape ss_backend.c uses.
 */
#if defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))

#include <string.h>
#include <windows.h>

#include "platform_win32.h"

/* One active stepper (the tier's documented single-active-region contract). */
static struct {
    DWORD tid;      /* arming thread: filter foreign traps        */
    ULONG_PTR base; /* region [base, base+len)                    */
    ULONG_PTR len;
    ULONG_PTR stop_rip;       /* the trampoline's return-landing address:   */
                              /* the trap here ends stepping                */
    unsigned long long *offs; /* caller-owned stream of in-region offsets   */
    unsigned cap, n;
    int overflow;
    volatile LONG active;
} g_ss;

static LONG CALLBACK ss_win64_veh(EXCEPTION_POINTERS *xi) {
    if (xi->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    if (!g_ss.active || GetCurrentThreadId() != g_ss.tid)
        return EXCEPTION_CONTINUE_SEARCH; /* not ours: leave it to the owner */
    ULONG_PTR rip = (ULONG_PTR)xi->ContextRecord->Rip;
    if (rip == g_ss.stop_rip) {
        /* The traced call returned: stop stepping HERE, in the handler, by
         * resuming with TF clear (see the file header for why a popfq disarm
         * is not trustworthy across NtContinue). */
        xi->ContextRecord->EFlags &= ~(DWORD)0x100;
        g_ss.active = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (rip - g_ss.base < g_ss.len) { /* unsigned compare: in-region only */
        if (g_ss.n < g_ss.cap)
            g_ss.offs[g_ss.n++] = (unsigned long long)(rip - g_ss.base);
        else
            g_ss.overflow = 1;
    }
    xi->ContextRecord->EFlags |= 0x100; /* keep stepping */
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* Publish the return-landing label, arm TF, call code(a0..a3) under the Win64
 * ABI. The instruction after the arming popfq is the callq, whose first trap
 * lands at the blob's entry; the trap at label 1 (the call's return landing)
 * is the one the handler ends stepping on. 40 bytes of stack give the callee
 * its shadow space (the traced leaves never spill, but keep the call site
 * conventional; the sub/add themselves run un-stepped and stepped-filtered
 * respectively). */
static long long ss_win64_call_stepped(const void *code, const long long a[4]) {
    long long ret;
    ULONG_PTR stop;
    register long long r8v __asm__("r8") = a[2];
    register long long r9v __asm__("r9") = a[3];
    __asm__ __volatile__(
        "leaq 1f(%%rip), %[stop]\n\t"
        "movq %[stop], %[stop_slot]\n\t"
        "subq $40, %%rsp\n\t"
        "pushfq\n\t"
        "orq $0x100, (%%rsp)\n\t"
        "popfq\n\t"
        "callq *%[fn]\n"
        "1:\n\t"
        "addq $40, %%rsp"
        : "=a"(ret), [stop] "=&r"(stop), [stop_slot] "=m"(g_ss.stop_rip)
        : [fn] "r"(code), "c"(a[0]), "d"(a[1]), "r"(r8v), "r"(r9v)
        : "cc", "memory", "r10", "r11");
    return ret;
}

int asmtest_win64_ss_trace_call(const void *code, size_t len,
                                const long long *args, int nargs,
                                long long *result, unsigned long long *offs,
                                unsigned cap, unsigned *n_out, int *truncated) {
    if (code == NULL || len == 0 || nargs < 0 || nargs > 4 ||
        (nargs > 0 && args == NULL) || offs == NULL || cap == 0 ||
        n_out == NULL)
        return -1;
    if (g_ss.active)
        return -2; /* single active stepper */

    long long a[4] = {0, 0, 0, 0};
    for (int i = 0; i < nargs; i++)
        a[i] = args[i];

    g_ss.tid = GetCurrentThreadId();
    g_ss.base = (ULONG_PTR)code;
    g_ss.len = (ULONG_PTR)len;
    g_ss.stop_rip = 0; /* published by the trampoline before arming */
    g_ss.offs = offs;
    g_ss.cap = cap;
    g_ss.n = 0;
    g_ss.overflow = 0;

    /* First handler (CALL_FIRST): the runner's crash-containment VEH must not
     * see (or classify) our single-step traffic. */
    PVOID h = AddVectoredExceptionHandler(1, ss_win64_veh);
    if (h == NULL)
        return -3;
    g_ss.active = 1;

    long long r = ss_win64_call_stepped(code, a);

    g_ss.active = 0; /* normally already 0: the handler ends stepping */
    RemoveVectoredExceptionHandler(h);

    if (result != NULL)
        *result = r;
    *n_out = g_ss.n;
    if (truncated != NULL)
        *truncated = g_ss.overflow;
    return 0;
}

#endif /* _WIN32 && x86-64 */
