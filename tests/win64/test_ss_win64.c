/* test_ss_win64.c — Phase 5 slice: the VEH single-step front-end, live.
 *
 * Verifies asmtest_win64_ss_trace_call (src/ss_win64.c): EFLAGS.TF armed around
 * a library-owned call, EXCEPTION_SINGLE_STEP delivered to a vectored handler,
 * the exact in-region instruction stream recorded. The fixtures are the Win64-ABI
 * twins of the Linux hwtrace suite's ROUTINE and LOOP blobs — same instruction
 * lengths, so the SAME expected offset streams ([0x0,0x3,0x6,0xc,0x11]; 62 steps
 * across the loop back-edge) prove the two front-ends reconstruct identically.
 *
 * Self-skips (exit 0) when no EXCEPTION_SINGLE_STEP arrives at all — an
 * environment that does not deliver single-step exceptions (a limited Wine
 * build) — but asserts the full streams wherever at least one step lands.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "platform_win32.h"

/* mov rax,rcx; add rax,rdx; cmp rax,100; jle +3; dec rax; ret
 * (two basic blocks; jle taken for 20+22 skips the dec) */
static const unsigned char ROUTINE_W64[] = {
    0x48, 0x89, 0xC8, 0x48, 0x01, 0xD0, 0x48, 0x3D, 0x64,
    0x00, 0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3,
};

/* mov rax,0; L: add rax,rcx; dec rdx; jnz L; ret — a real loop, every
 * back-edge stepped (1 + 20*3 + 1 = 62 instructions for (1, 20)) */
static const unsigned char LOOP_W64[] = {
    0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x48,
    0x01, 0xC8, 0x48, 0xFF, 0xCA, 0x75, 0xF8, 0xC3,
};

static int fails = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            printf("ok   - %s\n", (msg));                                      \
        } else {                                                               \
            printf("FAIL - %s\n", (msg));                                      \
            fails++;                                                           \
        }                                                                      \
    } while (0)

static void *exec_blob(const unsigned char *bytes, size_t len) {
    void *p = VirtualAlloc(NULL, len, MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE);
    if (p == NULL)
        return NULL;
    memcpy(p, bytes, len);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return p;
}

int main(void) {
    void *p = exec_blob(ROUTINE_W64, sizeof ROUTINE_W64);
    void *q = exec_blob(LOOP_W64, sizeof LOOP_W64);
    if (p == NULL || q == NULL) {
        printf("# SKIP VEH single-step: RWX VirtualAlloc failed\n");
        return 0;
    }

    /* --- fixture 1: ROUTINE, exact stream + arg/result plumbing --- */
    long long args[2] = {20, 22}, result = 0;
    unsigned long long offs[64];
    unsigned n = 0;
    int trunc = -1;
    int rc = asmtest_win64_ss_trace_call(p, sizeof ROUTINE_W64, args, 2,
                                         &result, offs, 64, &n, &trunc);
    CHECK(rc == 0, "trace_call returns 0");
    CHECK(result == 42, "routine(20,22) == 42 through the stepped call");

    if (n == 0) {
        /* The call ran (result checked) but no single-step exception landed:
         * this environment does not deliver EXCEPTION_SINGLE_STEP. */
        printf("# SKIP VEH single-step: no EXCEPTION_SINGLE_STEP delivered "
               "(limited Wine?)\n");
        return fails ? 1 : 0;
    }

    static const unsigned long long want[] = {0x0, 0x3, 0x6, 0xC, 0x11};
    int eq = (n == 5);
    for (unsigned i = 0; eq && i < n; i++)
        eq = offs[i] == want[i];
    printf("     (stream:");
    for (unsigned i = 0; i < n && i < 8; i++)
        printf(" 0x%llx", offs[i]);
    printf("%s)\n", n > 8 ? " ..." : "");
    CHECK(eq, "exact stream [0x0,0x3,0x6,0xc,0x11] (jle taken, dec skipped)");
    CHECK(trunc == 0, "not truncated");

    /* --- fixture 2: LOOP, every back-edge captured, no depth ceiling --- */
    long long largs[2] = {1, 20};
    unsigned long long loffs[128];
    result = 0;
    rc = asmtest_win64_ss_trace_call(q, sizeof LOOP_W64, largs, 2, &result,
                                     loffs, 128, &n, &trunc);
    CHECK(rc == 0 && result == 20, "loop(1,20) == 20 (sum of 1, 20x)");
    CHECK(n == 62, "62 instructions stepped (1 + 20*3 + 1)");
    CHECK(n >= 2 && loffs[0] == 0x0 && loffs[n - 1] == 0xF,
          "stream runs entry -> ret (0x0 .. 0xf)");
    CHECK(trunc == 0, "loop stream not truncated");

    /* --- overflow: cap smaller than the stream truncates, call completes --- */
    unsigned long long tiny[4];
    result = 0;
    rc = asmtest_win64_ss_trace_call(q, sizeof LOOP_W64, largs, 2, &result,
                                     tiny, 4, &n, &trunc);
    CHECK(rc == 0 && result == 20,
          "overflowing trace still completes the call");
    CHECK(n == 4 && trunc == 1, "cap=4: 4 recorded, truncated flagged");

    /* --- argument registers r8/r9 reach the routine (4-arg plumbing) --- */
    /* mov rax,r8; add rax,r9; ret */
    static const unsigned char ADD_R8R9[] = {0x4C, 0x89, 0xC0, 0x4C,
                                             0x01, 0xC8, 0xC3};
    void *s = exec_blob(ADD_R8R9, sizeof ADD_R8R9);
    if (s != NULL) {
        long long four[4] = {0, 0, 40, 2};
        result = 0;
        rc = asmtest_win64_ss_trace_call(s, sizeof ADD_R8R9, four, 4, &result,
                                         offs, 64, &n, &trunc);
        CHECK(rc == 0 && result == 42, "r8+r9 args reach the routine (40+2)");
        CHECK(n == 3, "3 instructions stepped (mov, add, ret)");
    }

    /* --- misuse is rejected, cleanly --- */
    rc =
        asmtest_win64_ss_trace_call(NULL, 1, NULL, 0, NULL, offs, 64, &n, NULL);
    CHECK(rc == -1, "NULL code rejected with -1");

    if (fails == 0)
        printf("VEH single-step front-end: all checks passed\n");
    return fails ? 1 : 0;
}
