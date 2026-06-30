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
 * single-step variant that can exist on AArch64 at all. Implemented for Linux x86-64
 * AND AArch64: the AArch64 tracer rides the same PTRACE_SINGLESTEP seam, differing
 * only in the register read (PC + return reg via PTRACE_GETREGSET/NT_PRSTATUS, since
 * AArch64 has no PTRACE_GETREGS) and the Capstone arch used for block normalization.
 *
 * Call model. Unlike the begin/end region markers of the other backends (which run
 * the routine in the collector's own process), the out-of-process tracer must own
 * the tracee, so it offers a single self-contained entry point that forks, runs the
 * registered code in the child, and reconstructs the trace in the parent from
 * ptrace register reads — no shared memory, because the parent observes every step.
 *
 * Supported target: a deterministic, single-threaded routine of up to six integer
 * arguments. The routine MAY call out to helper functions outside the registered
 * region — those call-outs (runtime helpers, GC barriers, PLT stubs) are stepped OVER
 * at native speed and not recorded, so a real method that calls helpers traces
 * correctly, not just a pure-compute leaf. (Call-out detection uses the Capstone
 * is-call query; without Capstone the region must be a leaf, the previous contract.)
 * The code bytes must already live in executable memory in THIS process (e.g. via
 * asmtest_hwtrace_exec_alloc); the forked child inherits that mapping at the same
 * address.
 *
 * No external library and no privilege beyond ptrace of one's own child (no
 * perf_event, no PMU, no decoder beyond the existing Capstone length-decoder used
 * for block normalization). Ships in libasmtest_hwtrace.
 */
#ifndef ASMTEST_PTRACE_H
#define ASMTEST_PTRACE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* pid_t */

#include "asmtest_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes (shared spirit with asmtest_hwtrace.h). */
#define ASMTEST_PTRACE_OK 0
#define ASMTEST_PTRACE_EINVAL (-1)
#define ASMTEST_PTRACE_EUNAVAIL (-3) /* not a Linux x86-64 / AArch64 host         */
#define ASMTEST_PTRACE_ENOSYS (-5)   /* backend not compiled in                  */
#define ASMTEST_PTRACE_ENOENT (-7)   /* region/symbol not found                  */
#define ASMTEST_PTRACE_ETRACE (-8)   /* fork / ptrace / wait failure             */

/* 1 if the out-of-process single-step tracer can run on this host (Linux x86-64, or
 * Linux AArch64 with a functional PTRACE_SINGLESTEP), else 0. On AArch64 this is a
 * cached runtime self-probe: x86-64 always supports single-step, but qemu-user
 * emulation does NOT emulate the ptrace tracer/tracee relationship, so the tracer
 * needs a real AArch64 host — the probe detects that without ever blocking. Block
 * normalization additionally uses the Capstone length-decoder already linked into the
 * tier; without it instruction offsets are still exact but blocks degrade. */
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
 * caller to PTRACE_DETACH. The region MAY call out to helpers outside [base, base+len)
 * — call-outs are stepped over at native speed and not recorded (call-depth aware), so a
 * real JIT method that calls runtime helpers traces correctly; the body must still be
 * deterministic and single-threaded. */
int asmtest_ptrace_trace_attached(pid_t pid, const void *base, size_t len,
                                  long *result, asmtest_trace_t *trace);

/* Run an already-attached target forward until it reaches `addr`, then stop it there —
 * the missing step between resolving a foreign method and tracing it. asmtest_ptrace_-
 * trace_attached requires `pid` to be stopped AT the region entry; against a real
 * managed runtime you do not control WHEN the program calls the method, so you cannot
 * just attach at the right instant. This plants a breakpoint at `addr`, PTRACE_CONTs the
 * target until the program ITSELF calls into `addr`, then removes the breakpoint and
 * leaves the target ptrace-stopped with its PC exactly at `addr` — precisely the
 * precondition trace_attached expects. It uses a software int3 (PTRACE_POKETEXT, which
 * patches even an r-x text page the way a debugger does); when the code is W^X and the
 * executable page is not writable (POKETEXT refused with EIO — e.g. a hardened JIT code
 * heap like .NET's default), it transparently falls back to a HARDWARE execution
 * breakpoint (x86-64 debug registers), which writes no code and is per-thread. The
 * caller still owns attach/detach: PTRACE_ATTACH + wait, then run_to(addr), then
 * trace_attached(addr, len, …), then PTRACE_DETACH. Unrelated signals delivered while
 * running are forwarded to the target. Returns ASMTEST_PTRACE_OK (stopped at addr),
 * ASMTEST_PTRACE_ENOENT if the target exited before reaching `addr`, ASMTEST_PTRACE_-
 * EINVAL on a NULL `addr`, or ASMTEST_PTRACE_ETRACE on a ptrace/wait failure (the
 * breakpoint is best-effort removed). */
int asmtest_ptrace_run_to(pid_t pid, const void *addr);

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

/* A JIT method as recorded in a jitdump JIT_CODE_LOAD record. */
typedef struct {
    uint64_t code_addr;  /* load address (the base to trace)                     */
    uint64_t code_size;  /* code length in bytes                                 */
    uint64_t timestamp;  /* record timestamp (load order)                        */
    uint64_t code_index; /* the JIT's unique index for this code                 */
} asmtest_jitdump_entry_t;

/* Read the Linux perf **jitdump** image (`jit-<pid>.dump`) a JIT writes and resolve a
 * method by `name` to its load address, size, and — unlike the text perf-map — its
 * actual recorded native code BYTES. jitdump is the bytes-accurate, time-stamped
 * format (CoreCLR/HotSpot/V8 emit it; `perf inject --jit` consumes it); because each
 * JIT_CODE_LOAD record carries a timestamp, a method re-emitted at a reused address
 * (tiered/OSR recompilation) resolves to the **latest** body — the temporal
 * same-address-different-bytes problem. Reads from `path`, or from `/tmp/jit-<pid>.dump`
 * when `path` is NULL. Endianness is auto-detected from the header magic.
 *
 * On the most recent JIT_CODE_LOAD matching `name`: fills *out (if non-NULL) and, if
 * `bytes_out` is non-NULL, copies up to `bytes_cap` of the recorded code into it and
 * sets *bytes_len. Returns ASMTEST_PTRACE_OK, ASMTEST_PTRACE_ENOENT (no such method /
 * no file), ASMTEST_PTRACE_EINVAL (not a jitdump), or a negative status. */
int asmtest_jitdump_find(const char *path, pid_t pid, const char *name,
                         asmtest_jitdump_entry_t *out, uint8_t *bytes_out,
                         size_t bytes_cap, size_t *bytes_len);

#ifdef __cplusplus
}
#endif

#endif /* ASMTEST_PTRACE_H */
