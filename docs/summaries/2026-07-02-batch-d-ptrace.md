# Implementation summary — Batch D: ptrace / DynamoRIO tier (findings #18–21)

*Source:* [2026-07-02 code review](../analysis/2026-07-02-code-review.md), findings 18–21.
*Validated:* `make hwtrace-test` **95/0** (Docker) — the working call-out
step-over (tests 66–69) and jitdump reader (73–77) paths are unchanged; a
targeted probe confirms the truncated-jitdump fix.

## #18 — trace_call killed the tracee on any non-SIGTRAP signal (Medium)

`src/ptrace_backend.c` `asmtest_ptrace_trace_call` — the single-step loop now
distinguishes routine faults (`SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE` → mark the
capture incomplete via `overflow`, kill+reap) from unrelated process-group signals
(`SIGWINCH`/`SIGINT`/`SIGTSTP`), which are forwarded on the next
`PTRACE_SINGLESTEP` (via a new `pending_sig`) and stepping continues — exactly as
`run_until` already does. A benign signal before region entry no longer produces
an empty trace returned as complete; a pre-entry fault sets `truncated`.

## #19 — call-out step-over is not call-depth aware (Medium)

`include/asmtest_ptrace.h` — the documentation is corrected to drop the
unqualified "(call-depth aware)" claim and state the re-entrancy limitation
explicitly: the step-over resumes at the FIRST arrival at the call's return
address, so if the stepped-over helper calls back into the region the tracer
resumes in that nested invocation. *Rationale:* the finding's implementation
alternative (SP-depth breakpoint re-arming) is high-risk on this working, tested
path, does not translate to AArch64 (LR-based calls leave SP unchanged across a
call), and the nested-callback scenario cannot be validated on this host without a
new crafted fixture. The doc correction resolves the stated defect — the mismatch
between the header's claim and the implementation. A full SP/frame-aware step-over
(x86-64 first) remains a documented follow-up.

## #20 — jitdump_find returned OK with unset length on a truncated record (Medium)

`src/ptrace_backend.c` `asmtest_jitdump_find` — `*bytes_len` is initialized to 0 at
entry, and a short `fread` of the record's code bytes (a live JIT still flushing
the newest record) now sets `rc = ASMTEST_PTRACE_ETRACE` before breaking (as do the
`fseek` failures in the match path), instead of returning OK with an unwritten
length and partial/garbage bytes.

*Evidence:* a jitdump whose JIT_CODE_LOAD claims `code_size=16` but is truncated
after 4 bytes now returns `-8` (ETRACE) with `*bytes_len == 0` (probe primed the
length with `0xdeadbeef`; it is overwritten to 0, not left as garbage).

## #21 — EXIT_RETURNED path left the tracee stopped and unreaped (Low)

`src/ptrace_backend.c` — after harvesting the return value and `PTRACE_CONT`, if
the follow-up `waitpid` reports a stop (a process-group signal in the window
between the routine's return and `_exit`), the child is now `SIGKILL`ed and reaped,
so it cannot survive as a stopped, unreaped tracee (`PTRACE_O_EXITKILL` fires only
on tracer exit). This closes the last path in the function where the child could
outlive the call.
