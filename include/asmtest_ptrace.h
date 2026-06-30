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
#include <sys/types.h> /* pid_t */

#include "asmtest_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes (shared spirit with asmtest_hwtrace.h). */
#define ASMTEST_PTRACE_OK 0
#define ASMTEST_PTRACE_EINVAL (-1)
#define ASMTEST_PTRACE_EUNAVAIL (-3) /* not a Linux x86-64 host                  */
#define ASMTEST_PTRACE_ENOSYS (-5)   /* backend not compiled in                  */
#define ASMTEST_PTRACE_ENOENT (-7)   /* region/symbol not found                  */
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

/* Trace a region in a SEPARATE, already-running process you have attached to — the
 * foreign / managed-runtime path (the building block for tracing a JVM/.NET/Node on
 * a host without Intel PT). `pid` must already be in a ptrace-stop: the caller owns
 * the attach/detach policy (PTRACE_ATTACH or PTRACE_SEIZE+INTERRUPT, then wait for
 * the stop, before this call; PTRACE_DETACH after), because *which* process to attach
 * and *when* are integrator decisions. This single-steps `pid` from its current stop,
 * reading RIP at each stop, and records every in-region instruction/block offset for
 * [base, base+len) IN THE TARGET'S address space into `trace` — until the region is
 * entered and exited (or the target exits). The registered code bytes are read FROM
 * THE TARGET via process_vm_readv (so the tracer need not share the target's memory),
 * then used for the same block normalization as the other backends. On success
 * *result receives the routine's return value (the target's RAX at the ret); `result`
 * may be NULL. The target is left ptrace-stopped just past the region exit for the
 * caller to PTRACE_DETACH. Supported target: a deterministic pure-compute region that
 * does not call out to other regions (same contract as the in-process stepper). */
int asmtest_ptrace_trace_attached(pid_t pid, const void *base, size_t len,
                                  long *result, asmtest_trace_t *trace);

/* ------------------------------------------------------------------ */
/* Code-region resolution — turn the foreign-attach primitive above   */
/* into "point it at a running process" by discovering the (base,len)  */
/* to trace from the OS, the way a debugger or `perf` does. Pure file  */
/* reads; no ptrace, so they may be called before attaching.           */
/* ------------------------------------------------------------------ */

/* Find the executable mapping in /proc/<pid>/maps that CONTAINS `addr` and return its
 * extent: *base_out = the mapping start, *len_out = its byte length. The common
 * "I have one address inside a foreign routine, I need the whole region to trace it"
 * step. Returns ASMTEST_PTRACE_OK, ASMTEST_PTRACE_ENOENT if no executable mapping
 * contains `addr`, or a negative status on a read failure. Either out-pointer may be
 * NULL. */
int asmtest_proc_region_by_addr(pid_t pid, const void *addr, void **base_out,
                                size_t *len_out);

/* Find a JIT method by `name` in the perf map a JIT writes at /tmp/perf-<pid>.map —
 * the de-facto text format `<hex start> <hex size> <symbol>` per line that V8/Node,
 * .NET, and OpenJDK (+perf-map-agent) emit so `perf` can symbolize generated code.
 * Returns the method's [*base_out, *len_out) — the (base,len) to hand
 * asmtest_ptrace_trace_attached, completing "attach to a JIT and trace a method out of
 * band". `name` is matched against the full symbol text after the size field. Returns
 * ASMTEST_PTRACE_OK, ASMTEST_PTRACE_ENOENT (no such symbol or no map file), or a
 * negative status. (The richer binary jitdump format is a follow-on; the text perf-map
 * is the portable lowest common denominator.) */
int asmtest_proc_perfmap_symbol(pid_t pid, const char *name, void **base_out,
                                size_t *len_out);

#ifdef __cplusplus
}
#endif

#endif /* ASMTEST_PTRACE_H */
