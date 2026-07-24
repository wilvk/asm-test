/*
 * asmspy.h — the CLI/TUI half of asmspy (cli/asmspy.c).
 *
 * asmspy is a small ncurses front-end over the asm-test out-of-process tracer:
 * pick a running process, then watch either its live syscalls (with data) or a
 * live disassembly + call-graph of a chosen function — all out of band via the
 * ptrace attach seam (examples/attach_trace.c, examples/syscall_log.c).
 *
 * The ENGINE it drives is no longer declared here: the resolvers
 * (cli/asmspy_proc.c) and the ptrace engines (cli/asmspy_engine.c) are a
 * library, libasmspy, behind ONE public header — cli/libasmspy.h — which this
 * file includes and re-exports so every existing includer keeps compiling
 * unchanged. New consumers should include libasmspy.h directly; this header is
 * only for code that also needs the front-end entry point below.
 *
 * The ptrace-per-thread contract that governs every engine call is stated in
 * libasmspy.h; the TUI honours it by running engines on a dedicated tracer
 * thread and never touching ptrace from the UI thread.
 */
#ifndef ASMSPY_H
#define ASMSPY_H

#include "libasmspy.h" /* the engine + resolver surface (all of it) */

/* ------------------------------------------------------------------ */
/* Interactive TUI entry point (asmspy.c). Returns a process exit code. */
/* ------------------------------------------------------------------ */
int asmspy_tui(void);

#endif /* ASMSPY_H */
