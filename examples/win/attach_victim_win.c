/* attach_victim_win.c — Windows mirror of examples/attach_victim.c.
 *
 * An ordinary process asm-test did NOT start: it calls a hot function in a loop
 * and prints its PID + the function's address range on stderr, so a SEPARATE
 * tracer (attach_trace_win) can DebugActiveProcess() it and single-step one call.
 *
 * No PR_SET_PTRACER analog is needed: Windows has no Yama: a same-session process
 * with debug rights can attach. It prints hotfn's start AND a trailing marker so
 * the demo derives the byte length as |marker - hotfn| regardless of link order.
 */
#include <stdint.h>
#include <stdio.h>
#include <windows.h>

__attribute__((noinline)) long hotfn(long n, long k) {
    long s = 0;
    for (long i = 0; i < n; i++)
        s += (i & 1) ? i * k : -i; /* a branch, so the trace has >1 block */
    return s;
}
__attribute__((noinline)) void hotfn_end(void) { /* address marker only */ }

int main(void) {
    fprintf(stderr, "victim pid=%lu hotfn=0x%llx marker=0x%llx\n",
            (unsigned long)GetCurrentProcessId(),
            (unsigned long long)(uintptr_t)hotfn,
            (unsigned long long)(uintptr_t)hotfn_end);
    fflush(stderr);
    volatile long sink = 0;
    for (;;) {
        sink += hotfn(6, 7);
        Sleep(200); /* ~5 Hz: leaves the tracer time to attach + run_to */
    }
    return 0;
}
