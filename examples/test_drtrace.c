/*
 * test_drtrace.c — smoke test for the optional DynamoRIO in-process native-trace
 * tier (asmtest_drtrace.h). Standalone harness, run directly (NOT through the
 * forking runner): in-process DynamoRIO attach is hostile to per-test fork()
 * isolation and the -jN pool, so this is a single-process, --no-fork smoke test.
 *
 * Self-skips with a clear message (exit 0) when the tier was not built with
 * DynamoRIO, mirroring the emulator/assembler optional tiers. When built, it:
 *   1. initializes DR in-process and takes over;
 *   2. registers a host-native routine range (real executable W^X memory);
 *   3. traces a call through it, asserting block offset 0 is covered;
 *   4. re-runs to confirm coverage accumulates;
 *   5. exercises a small truncation case and instruction mode.
 *
 * The client library path comes from argv[1] or $ASMTEST_DRCLIENT.
 */
#include "asmtest_drtrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        checks++;                                                               \
        if (cond) {                                                            \
            printf("ok %d - %s\n", checks, msg);                                \
        } else {                                                               \
            printf("not ok %d - %s\n", checks, msg);                            \
            failures++;                                                         \
        }                                                                      \
    } while (0)

/* x86-64 SysV: long f(long a, long b) { long r = a + b; if (r > 100) r--; return r; }
 * Hand-assembled, position-independent, two basic blocks (a forward branch):
 *   0:  48 89 f8             mov    rax, rdi
 *   3:  48 01 f0             add    rax, rsi
 *   6:  48 3d 64 00 00 00    cmp    rax, 100
 *   c:  7e 03                jle    .skip   ; +3 skips the 3-byte dec
 *   e:  48 ff c8             dec    rax
 *  .skip:
 *  11:  c3                   ret
 */
static const unsigned char ROUTINE[] = {
    0x48, 0x89, 0xf8,                   /* mov rax, rdi          (off 0)  */
    0x48, 0x01, 0xf0,                   /* add rax, rsi                   */
    0x48, 0x3d, 0x64, 0x00, 0x00, 0x00, /* cmp rax, 100                   */
    0x7e, 0x03,                         /* jle +3 -> ret (skip dec, off 0xc)*/
    0x48, 0xff, 0xc8,                   /* dec rax               (off 0xe)*/
    0xc3                                /* ret                   (off 0x11)*/
};
typedef long (*add2_fn)(long, long);

/* asmtest_symbol_demo (the symbol-mode fixture, Phase 7) now lives in
 * libasmtest_drapp and is declared in asmtest_drtrace.h, so every language
 * binding shares one resolvable symbol; this harness links drtrace_app.o (and
 * -rdynamic), so it resolves and calls the same definition. */

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0); /* unbuffered: progress survives a hard kill */
    if (!asmtest_dr_available()) {
        printf("# SKIP drtrace: built without DynamoRIO\n");
        printf("1..0 # skipped\n");
        return 0;
    }
    const char *client = (argc > 1) ? argv[1] : getenv("ASMTEST_DRCLIENT");

    asmtest_drtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.client_path = client;
    opts.mode = ASMTEST_DRTRACE_BLOCKS;

    int rc = asmtest_dr_init(&opts);
    if (rc != ASMTEST_DR_OK) {
        printf("# SKIP drtrace: dr_init failed (%d) — is the client path set?\n",
               rc);
        printf("1..0 # skipped\n");
        return 0;
    }
    CHECK(asmtest_dr_start() == ASMTEST_DR_OK, "dr_start takes over in-process");

    /* Materialize the routine into real executable memory. */
    asmtest_exec_code_t code;
    CHECK(asmtest_exec_alloc(ROUTINE, sizeof ROUTINE, &code) == ASMTEST_DR_OK,
          "exec_alloc maps host-native W^X code");

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    CHECK(tr != NULL, "trace allocated");
    CHECK(asmtest_dr_register_region("add2", code.base, code.len, tr) ==
              ASMTEST_DR_OK,
          "register_region records the native range");

    add2_fn fn = (add2_fn)code.base;
    asmtest_trace_begin("add2");
    long r = fn(20, 22);
    asmtest_trace_end("add2");
    CHECK(r == 42, "traced call returns the right value (20+22)");
    CHECK(asmtest_trace_covered(tr, 0), "block offset 0 (entry) covered");

    /* Re-run with a value taking the dec branch; coverage accumulates. */
    unsigned long long blocks_before = asmtest_emu_trace_blocks_len(tr);
    asmtest_trace_begin("add2");
    long r2 = fn(60, 60); /* 120 > 100 -> dec -> 119 */
    asmtest_trace_end("add2");
    CHECK(r2 == 119, "second traced call takes the dec branch (60+60-1)");
    /* The first run (20+22=42, <=100) never took the dec path, so the block at
     * offset 0xe is new coverage from this run. Assert it directly, and require
     * strict growth — >= against a monotonic counter can never fail even if the
     * second run recorded nothing at all. */
    CHECK(asmtest_trace_covered(tr, 0xe),
          "re-running covers the dec-branch block (offset 0xe)");
    CHECK(asmtest_emu_trace_blocks_len(tr) > blocks_before,
          "re-running the region accumulates coverage");
    CHECK(asmtest_dr_marker_error() == 0, "all begin/end markers balanced");

    /* Instruction mode: a trace with insns_cap > 0 records the ordered stream.
     * Use a SEPARATE executable allocation — regions must be non-overlapping, so
     * we cannot register a second region over the same range as "add2". */
    asmtest_exec_code_t code2;
    CHECK(asmtest_exec_alloc(ROUTINE, sizeof ROUTINE, &code2) == ASMTEST_DR_OK,
          "second exec allocation for instruction mode");
    asmtest_trace_t *itr = asmtest_trace_new(64, 64);
    asmtest_dr_register_region("add2i", code2.base, code2.len, itr);
    add2_fn fn2 = (add2_fn)code2.base;
    asmtest_trace_begin("add2i");
    long ri = fn2(1, 2);
    asmtest_trace_end("add2i");
    CHECK(ri == 3, "instruction-mode routine computes correctly (1+2)");
    /* fn(1,2): 3 <= 100, so the jle is taken and the dec at 0xe is skipped. The
     * deterministic ordered stream is exactly mov(0) add(3) cmp(6) jle(0xc) ret(0x11). */
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int seq_ok = (itr->insns_len == 5);
    for (size_t i = 0; seq_ok && i < 5; i++)
        seq_ok = (itr->insns[i] == EXPECT[i]);
    CHECK(seq_ok, "instruction mode records the exact ordered offset sequence");
    asmtest_dr_unregister_region("add2i");
    asmtest_exec_free(&code2);
    asmtest_trace_free(itr);

    /* Truncation: a tiny block buffer (cap 1) over a routine that enters two
     * distinct blocks must set the truncated bit (Phase 2 acceptance). */
    asmtest_exec_code_t code3;
    CHECK(asmtest_exec_alloc(ROUTINE, sizeof ROUTINE, &code3) == ASMTEST_DR_OK,
          "third exec allocation for truncation case");
    asmtest_trace_t *ttr = asmtest_trace_new(0, 1); /* blocks_cap = 1 */
    asmtest_dr_register_region("trunc", code3.base, code3.len, ttr);
    add2_fn fn3 = (add2_fn)code3.base;
    asmtest_trace_begin("trunc");
    (void)fn3(60, 60); /* 120 > 100 -> two distinct blocks (entry + dec path) */
    asmtest_trace_end("trunc");
    CHECK(asmtest_emu_trace_truncated(ttr),
          "tiny block buffer sets the truncated bit on overflow");
    asmtest_dr_unregister_region("trunc");
    asmtest_exec_free(&code3);
    asmtest_trace_free(ttr);

    /* Symbol mode (Phase 7): trace a named function with NO begin/end markers. */
    asmtest_trace_t *str = asmtest_trace_new(0, 64);
    int src = asmtest_dr_register_symbol("asmtest_symbol_demo", 256, str);
    CHECK(src == ASMTEST_DR_OK, "register_symbol resolves an exported function");
    volatile long sr = asmtest_symbol_demo(3, 4); /* no begin/end */
    CHECK(sr == 10, "symbol-mode function computes correctly (3*2+4)");
    CHECK(asmtest_trace_covered(str, 0),
          "symbol mode records coverage with no manual region calls");
    asmtest_dr_unregister_region("asmtest_symbol_demo");
    asmtest_trace_free(str);

    asmtest_dr_unregister_region("add2");
    asmtest_exec_free(&code);
    asmtest_trace_free(tr);
    asmtest_dr_shutdown();

    printf("1..%d\n", checks);
    printf("# %d passed, %d failed\n", checks - failures, failures);
    return failures == 0 ? 0 : 1;
}
