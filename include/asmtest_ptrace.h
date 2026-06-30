/*
 * asmtest_ptrace.h — OUT-OF-PROCESS single-step native-trace backend (W2).
 *
 * The single-step backend in src/ss_backend.c (asmtest_hwtrace.h's
 * ASMTEST_HWTRACE_SINGLESTEP) drives EFLAGS.TF in-process: the traced routine and
 * the SIGTRAP collector share one process. This is the *out-of-process* sibling
 * (the "W2" front of docs/plans/zen2-singlestep-trace-plan.md Phase 5): a tracer
 * PARENT PTRACE_SINGLESTEPs a forked tracee and reads RIP per stop. It yields the
 * SAME exact, complete asmtest_trace_t offsets — ordered in-region instruction
 * offsets matching Unicorn / DynamoRIO / Intel PT / the in-process stepper, and
 * block offsets after the same single-entry/ends-at-branch normalization — but
 * collects them OUT OF BAND, from a separate process.
 *
 * Why out-of-band matters: the in-process stepper installs a SIGTRAP handler and
 * sets TF on its own thread, which collides with a JIT/GC managed runtime's own
 * signal and code-cache machinery (the same reason in-process DynamoRIO cannot take
 * over a JVM/.NET/Node runtime's threads). An out-of-process ptrace tracer touches
 * none of the tracee's state, so it is the recommended exact path for managed
 * runtimes on AMD (where Intel PT is unavailable), and — because the ARM64
 * single-step bit (MDSCR_EL1.SS) is kernel-only with no in-process form — the only
 * single-step variant that can exist on AArch64 at all. This implementation is
 * Linux/x86-64; the AArch64 tracer rides the same PTRACE_SINGLESTEP seam.
 *
 * Call model. Unlike the begin/end region markers of the other backends (which run
 * the routine in the collector's own process), the out-of-process tracer must own
 * the tracee, so it offers a single self-contained entry point that forks, runs the
 * registered code in the child, and reconstructs the trace in the parent from
 * ptrace register reads — no shared memory, because the parent observes every step.
 *
 * Supported target (same contract as the in-process stepper): a deterministic,
 * single-threaded, pure-compute routine of up to six integer arguments that does
 * not call out into other traced regions. The code bytes must already live in
 * executable memory in THIS process (e.g. via asmtest_hwtrace_exec_alloc); the
 * forked child inherits that mapping at the same address.
 *
 * No external library and no privilege beyond ptrace of one's own child (no
 * perf_event, no PMU, no decoder beyond the existing Capstone length-decoder used
 * for block normalization). Ships in libasmtest_hwtrace.
 */
#ifndef ASMTEST_PTRACE_H
#define ASMTEST_PTRACE_H

#include <stddef.h>

#include "asmtest_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes (shared spirit with asmtest_hwtrace.h). */
#define ASMTEST_PTRACE_OK 0
#define ASMTEST_PTRACE_EINVAL (-1)
#define ASMTEST_PTRACE_EUNAVAIL (-3) /* not a Linux x86-64 host                  */
#define ASMTEST_PTRACE_ENOSYS (-5)   /* backend not compiled in                  */
#define ASMTEST_PTRACE_ETRACE (-8)   /* fork / ptrace / wait failure             */

/* 1 if the out-of-process single-step tracer can run on this host (Linux x86-64),
 * else 0. Block normalization additionally wants the Capstone length-decoder
 * (asmtest_disas_available()); without it instruction offsets are still exact but
 * blocks degrade. */
int asmtest_ptrace_available(void);

/* A human-readable reason asmtest_ptrace_available() returned 0, into buf (always
 * NUL-terminated). */
void asmtest_ptrace_skip_reason(char *buf, size_t buflen);

/* Trace `code` (`len` bytes of host-native machine code, already executable in this
 * process at address `code`) OUT OF PROCESS: fork a tracee that calls it with the
 * first `nargs` (0..6) integer arguments per the SysV ABI while the parent
 * PTRACE_SINGLESTEPs the child and records every in-region instruction/block offset
 * into `trace` (allocate it with asmtest_trace_new; recording follows its
 * capacities, exactly like the other backends). On success *result receives the
 * routine's return value (the child's RAX at the ret) and the call returns
 * ASMTEST_PTRACE_OK. `result` may be NULL to ignore the return. Sets
 * trace->truncated on an undecodable instruction (self-modifying / relocated bytes)
 * or capture-buffer overflow, never emitting a partial trace as complete. */
int asmtest_ptrace_trace_call(const void *code, size_t len, const long *args,
                              int nargs, long *result, asmtest_trace_t *trace);

#ifdef __cplusplus
}
#endif

#endif /* ASMTEST_PTRACE_H */
