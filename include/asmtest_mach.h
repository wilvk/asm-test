/*
 * asmtest_mach.h — macOS OUT-OF-PROCESS single-step native-trace backend (W2, Darwin).
 *
 * The single-step backend in src/ss_backend.c (asmtest_hwtrace.h's
 * ASMTEST_HWTRACE_SINGLESTEP) drives EFLAGS.TF in-process on Darwin too: the traced
 * routine and the SIGTRAP collector share one process. This is that stepper's
 * *out-of-process* sibling on macOS — the Darwin twin of asmtest_ptrace.h's Linux
 * ptrace tracer (the "W2" front of
 * docs/internal/plans/zen2-singlestep-trace-plan.md Phase 5). Where the Linux tracer
 * PTRACE_SINGLESTEPs a forked tracee, XNU deliberately cripples BSD ptrace for PC/flag
 * edits (PT_STEP/PT_CONTINUE return ENOTSUP off the `addr == (caddr_t)1` sentinel, by
 * kernel-comment design, to force use of Mach SPIs) — so this backend instead
 * `task_for_pid`s the target, arms EFLAGS.TF via `thread_set_state`, and receives each
 * `#DB` as an `EXC_BREAKPOINT` Mach exception message on a dedicated exception port. It
 * yields the SAME exact, complete asmtest_trace_t offsets — ordered in-region
 * instruction offsets matching the in-process stepper and the other backends, and block
 * offsets after the same single-entry/ends-at-branch normalization — collected OUT OF
 * BAND, from a separate process, exactly like the Linux tracer.
 *
 * Why out-of-band matters: see asmtest_ptrace.h — the same managed-runtime rationale
 * (a JIT/GC runtime's own signal machinery collides with an in-process stepper) applies
 * here. This is the recommended exact out-of-process path on macOS x86-64, where
 * neither Intel PT nor DynamoRIO-attach exist as alternatives.
 *
 * Call model. Mirrors asmtest_ptrace.h's five-symbol shape exactly
 * (available/skip_reason/trace_call/trace_attached/run_to) so callers already handling
 * the Linux tracer add the Darwin twin with a parallel `#if defined(__APPLE__)` arm.
 *
 * Supported target: a deterministic, single-threaded routine of up to six integer
 * arguments, exactly as asmtest_ptrace.h documents. The code bytes must already live in
 * executable memory in THIS process (e.g. via asmtest_hwtrace_exec_alloc); a forked
 * child inherits that mapping at the same address.
 *
 * Platform gate: x86-64 Darwin only (`#if defined(__x86_64__) && defined(__APPLE__)`).
 * Apple Silicon (arm64 macOS) is a future extension — out of scope here, see
 * docs/internal/implementations/macos-oop-mach-stepper.md's Constraints & gates.
 *
 * No external library beyond the Mach frameworks already in libSystem (part of every
 * macOS host's Xcode Command Line Tools) and the existing Capstone length-decoder used
 * for block normalization. Ships in libasmtest_hwtrace.
 */
#ifndef ASMTEST_MACH_H
#define ASMTEST_MACH_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* pid_t */

#include "asmtest_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes (shared spirit with asmtest_ptrace.h / asmtest_hwtrace.h). */
#define ASMTEST_MACH_OK     0
#define ASMTEST_MACH_EINVAL (-1)
#define ASMTEST_MACH_EUNAVAIL                                                  \
    (-3)                         /* not an x86-64 Darwin host                */
#define ASMTEST_MACH_ENOSYS (-5) /* backend not compiled in                  */
#define ASMTEST_MACH_ENOENT (-7) /* region/target not found                  */
#define ASMTEST_MACH_ETRACE (-8) /* fork / Mach / wait failure               */
#define ASMTEST_MACH_EPERM                                                     \
    (-9) /* task_for_pid denied: no entitlement, not root (soft gate) */

/* 1 if the out-of-process Mach stepper can run on this host (x86-64 Darwin with the
 * Capstone length-decoder linked in), else 0. Does NOT probe task_for_pid — that is a
 * per-target runtime permission check, surfaced by asmtest_mach_trace_call /
 * _trace_attached as ASMTEST_MACH_EPERM, not a build-time or host-class property. */
int asmtest_mach_available(void);

/* A human-readable reason asmtest_mach_available() returned 0, into buf (always
 * NUL-terminated): "mach stepper is x86-64 macOS only" off platform, or "built without
 * Capstone (mach block normalization)" when only the decoder is missing. */
void asmtest_mach_skip_reason(char *buf, size_t buflen);

/* Trace `code` (`len` bytes of host-native machine code, already executable in this
 * process at address `code`) OUT OF PROCESS: fork a tracee that calls it with the first
 * `nargs` (0..6) integer arguments per the Darwin x86-64 ABI while the parent single-
 * steps the child through a Mach exception port and records every in-region
 * instruction/block offset into `trace` (allocate it with asmtest_trace_new; recording
 * follows its capacities, exactly like the other backends). On success *result receives
 * the routine's return value (the child's RAX at the ret) and the call returns
 * ASMTEST_MACH_OK. `result` may be NULL to ignore the return. Sets trace->truncated on
 * an undecodable instruction (self-modifying / relocated bytes) or capture-buffer
 * overflow, never emitting a partial trace as complete. Returns ASMTEST_MACH_EPERM when
 * task_for_pid of the forked child is denied (see the header comment's entitlement /
 * root note in
 * docs/internal/implementations/macos-oop-mach-stepper.md#constraints--gates). */
int asmtest_mach_trace_call(const void *code, size_t len, const long *args,
                            int nargs, long *result, asmtest_trace_t *trace);

/* Trace a region [base, base+len) in a SEPARATE, already-suspended process (the caller
 * owns getting `pid` stopped at or before `base` — task_suspend or asmtest_mach_run_to,
 * mirroring asmtest_ptrace_trace_attached's contract exactly), reconstructing the
 * identical per-instruction/block stream via task_for_pid + thread_get_state /
 * thread_set_state instead of ptrace register peeks. The target is never killed; on a
 * region return it is left stopped past the region. Same *result / trace->truncated
 * contract as asmtest_mach_trace_call. Returns ASMTEST_MACH_ENOENT if the target does
 * not reach the region, ASMTEST_MACH_EPERM if task_for_pid is denied. */
int asmtest_mach_trace_attached(pid_t pid, const void *base, size_t len,
                                long *result, asmtest_trace_t *trace);

/* Run an already-attached target forward until it reaches `addr`, then stop it there —
 * the Darwin twin of asmtest_ptrace_run_to, and the missing step between resolving a
 * foreign method and tracing it (asmtest_mach_trace_attached requires `pid` to already
 * be stopped AT the region entry). Plants a software breakpoint (mach_vm_write of
 * 0xCC), resumes the target through the exception-port loop until it hits that
 * breakpoint itself, restores the original byte, and rewinds the PC back to `addr` via
 * thread_set_state — leaving the target stopped exactly at `addr`. When the code page
 * is W^X and cannot be made writable (mach_vm_protect refused — e.g. a hardened JIT
 * heap), transparently falls back to an x86-64 hardware execution breakpoint
 * (thread_set_state(x86_DEBUG_STATE64), DR0/DR7), which writes no code. The caller
 * still owns attach/detach. Unrelated exceptions delivered while running are forwarded
 * to the target unchanged. Returns ASMTEST_MACH_OK (stopped at addr), ASMTEST_MACH_-
 * ENOENT if the target exited before reaching `addr`, ASMTEST_MACH_EINVAL on a NULL
 * `addr`, ASMTEST_MACH_EPERM if task_for_pid is denied, or ASMTEST_MACH_ETRACE on a
 * Mach failure (the breakpoint is best-effort removed). */
int asmtest_mach_run_to(pid_t pid, const void *addr);

#ifdef __cplusplus
}
#endif

#endif /* ASMTEST_MACH_H */
