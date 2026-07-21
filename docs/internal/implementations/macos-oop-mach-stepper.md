# macOS out-of-process single-step via Mach exception ports — implementation

> **Sources.** Actioned from
> [zen2-singlestep-trace-plan.md](../archive/plans/zen2-singlestep-trace-plan.md) — the
> Phase 5 / W2 **"macOS (Intel)."** bullet, which notes that out-of-process needs
> Mach exception ports + the `com.apple.security.cs.debugger` entitlement and says
> the work "Slots into the
> [macOS clean-test plan](../plans/macos-clean-test-plan.md)" but was never filed
> there. Written 2026-07-17. If this doc and a source disagree, this doc wins
> (sources may be stale); if the CODE and this doc disagree, re-verify before
> implementing.

## Why this work exists

The single-step native-trace tier already captures exact instruction/block traces
on macOS **in-process** — the traced routine and the `SIGTRAP` collector share one
process ([src/ss_backend.c](../../../src/ss_backend.c)). On Linux there is also an
**out-of-process** sibling: a tracer parent single-steps a *separate* process and
reconstructs the same trace out of band ([src/ptrace_backend.c](../../../src/ptrace_backend.c)),
which is what lets asm-test trace a foreign JIT/GC managed runtime (JVM/.NET/Node)
whose own signal and code-cache machinery collides with the in-process stepper. On
macOS that out-of-process story does not exist: every `asmtest_ptrace_*` entry point
is compiled out off Linux. This doc builds the macOS equivalent so a user on a
macOS x86-64 host can trace a routine or a foreign process's method out of band, the
same way the Linux ptrace tracer does — using **Mach exception ports** and the
`com.apple.security.cs.debugger` entitlement rather than Linux `ptrace`.

**Mechanism decision, up front (PT_STEP vs. Mach ports).** XNU deliberately cripples
BSD `ptrace`: `PT_STEP`/`PT_CONTINUE` share a case that returns `ENOTSUP` unless the
`addr` argument is the sentinel `(caddr_t)1` ("continue at current PC"), and the
kernel comment says this is to *"force use of Mach SPIs (and `task_for_pid` security
checks) to adjust PC"* — so you cannot read or edit `RIP`/`RFLAGS` through `ptrace`
at all on macOS. `PT_STEP` itself only calls `thread_setsinglestep()`, which sets the
x86 trap flag (`EFL_TF`, `RFLAGS` bit 8 = `0x100`) in the thread's saved state — the
identical hardware mechanism the in-process stepper already uses. The real
debugger-grade path is therefore **`task_for_pid` → a Mach exception port → arm the
trap flag with `thread_set_state` → receive each `#DB` as an `EXC_BREAKPOINT` Mach
message**. This doc takes that path; `ptrace` PT_STEP is evaluated and rejected (see
[Research notes](#research-notes-verified-2026-07-17)).

## What already exists (verified 2026-07-17)

The landed substrate this builds on, each verified against the working tree:

- **In-process Darwin stepper** — [src/ss_backend.c](../../../src/ss_backend.c)
  drives `EFLAGS.TF` in-process and reads `RIP` from the `SIGTRAP` handler. Its
  Darwin/Linux split is two macros: `SS_RIP(uc)` / `SS_SET_TF(uc)` read
  `uc_mcontext->__ss.__rip` / `|= …__rflags` on Apple and `gregs[REG_RIP]` /
  `[REG_EFL]` on Linux (`src/ss_backend.c`, the `#if defined(__APPLE__)` block around
  lines 86–94; `SS_TF` is `0x100ULL`, the same trap-flag bit this doc arms remotely).
  The platform gate is `#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))`
  (line 68). This is the in-process model; nothing here is out-of-process.
- **The Linux out-of-process API to mirror** — [include/asmtest_ptrace.h](../../../include/asmtest_ptrace.h)
  defines the shape: `asmtest_ptrace_available()` / `_skip_reason()`,
  `asmtest_ptrace_trace_call(code, len, args, nargs, result, trace)` (fork a tracee,
  trace a blob), `asmtest_ptrace_trace_attached(pid, base, len, result, trace)` (trace
  a region in an already-attached foreign process), and `asmtest_ptrace_run_to(pid, addr)`
  (run an attached target to `addr` and stop it there). This doc reproduces that exact
  five-symbol shape under `asmtest_mach_*`.
- **The Linux implementation is Linux-only** — [src/ptrace_backend.c](../../../src/ptrace_backend.c)
  wraps its entire body in `#if defined(__linux__)` (the readers block opens at line 45;
  the stepper block at line 295 `#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))`).
  The end-of-file `#else /* stepper unsupported: not Linux … */` at line 3156 makes every
  entry point return `ASMTEST_PTRACE_ENOSYS`/`EUNAVAIL` off Linux. **So on macOS there is
  no out-of-process stepper today — this is the gap.**
- **No Mach code anywhere.** `grep -rn -E 'mach_port|task_set_exception|exception_port|task_for_pid|thread_set_state' src/ include/`
  returns **zero hits** (verified). Likewise `grep -rniE 'codesign|entitlement|com.apple.security' scripts/ .github/ Makefile mk/`
  returns **zero hits** — no self-signing harness exists yet either. This work is genuinely unowned.
- **Build wiring for the tier** — [mk/native-trace.mk](../../../mk/native-trace.mk):
  `HWTRACE_OBJS` (lines ~2047–2053) is the static object list for the C test harness and
  already carries `ss_backend.o` and `ptrace_backend.o`; the shared library
  `libasmtest_hwtrace` is linked from a parallel `$(BUILD)/pic/*.o` list (the
  `$(call shlib_real,libasmtest_hwtrace)` rule at lines ~2784–2801). The in-process
  Darwin stepper is validated by `make hwtrace-test` (target at line ~2127).
- **The validating host exists** — [benchmarks/boxes/intel-macos-x86_64-de7ec54c/features.json](../../../benchmarks/boxes/intel-macos-x86_64-de7ec54c/features.json)
  records a real macOS 14.7.5 / Intel Core i7-8559U (x86-64) box where the `single_step`
  backend is `available: true` and `complete: true` live. That is this dev host; the CI
  Intel-mac leg is `macos-15-intel` (see [Constraints & gates](#constraints--gates)).

**Prove the baseline green before touching anything** (on the macOS x86-64 host):

```sh
make hwtrace-test          # in-process single-step backend runs LIVE; parity asserts pass
grep -rn task_for_pid src/ include/   # prints nothing — confirms the gap this doc fills
```

`make hwtrace-test` printing its `== hwtrace-test ==` banner and exiting `0` with the
in-process single-step assertions passing is the green baseline.

## Tasks

### T1 — Decide the mechanism and stand up the gated Mach TU  (S, depends on: none)
**Goal.** A new, platform-gated translation unit and public header exist, exposing
`asmtest_mach_available()` / `asmtest_mach_skip_reason()`, compiled into the hwtrace
tier, returning a clear self-skip off x86-64 Darwin — the skeleton every later task fills in.
**Steps.**
1. Create [include/asmtest_mach.h](../../../include/asmtest_mach.h) mirroring
   [include/asmtest_ptrace.h](../../../include/asmtest_ptrace.h)'s comment style and
   status-code block, but with `ASMTEST_MACH_*` codes. Reuse the same spirit:
   `OK 0`, `EINVAL -1`, `EUNAVAIL -3` (not x86-64 Darwin), `ENOSYS -5` (not built in),
   `ENOENT -7` (region/symbol not found), `ETRACE -8` (mach/task failure). Add one macOS-only
   code the Linux header has no analog for: `ASMTEST_MACH_EPERM -9` (`task_for_pid` denied
   — no entitlement and not root), so callers self-skip on the *permission* gate distinctly
   from a hard failure.
2. Declare the five-symbol shape to be filled by T2–T5:
   `int asmtest_mach_available(void);`,
   `void asmtest_mach_skip_reason(char *buf, size_t buflen);`,
   `int asmtest_mach_trace_call(const void *code, size_t len, const long *args, int nargs, long *result, asmtest_trace_t *trace);`,
   `int asmtest_mach_trace_attached(pid_t pid, const void *base, size_t len, long *result, asmtest_trace_t *trace);`,
   `int asmtest_mach_run_to(pid_t pid, const void *addr);`.
3. Create [src/mach_backend.c](../../../src/mach_backend.c). Gate the real body with
   `#if defined(__x86_64__) && defined(__APPLE__)` (mirror the `ss_backend.c` gate at its
   line 68, but *without* `__linux__` — this TU is Darwin-only). Provide an `#else` stub that
   makes all five entry points return `ASMTEST_MACH_EUNAVAIL`/`ENOSYS`, so the file compiles
   to a harmless no-op on Linux and Apple-Silicon builds.
4. `asmtest_mach_available()` returns 1 on x86-64 Darwin when the Capstone length-decoder
   is present (`asmtest_disas_available()`, already linked into the tier via `disasm.o` — the
   same dependency `ss_backend.c` block-normalization uses), else 0. It does **not** probe
   `task_for_pid` (that is a per-target runtime check, surfaced by T3/T5 as `EPERM`).
   `asmtest_mach_skip_reason()` writes `"mach stepper is x86-64 macOS only"` off platform and
   `"built without Capstone (mach block normalization)"` when only the decoder is missing.
5. Wire the object into the build in [mk/native-trace.mk](../../../mk/native-trace.mk):
   add `$(BUILD)/mach_backend.o` to `HWTRACE_OBJS` (the list at lines ~2047–2053), add a
   compile rule mirroring the `ss_backend.o` rule at line ~1990
   (`$(BUILD)/mach_backend.o: src/mach_backend.c include/asmtest_mach.h include/asmtest_trace.h | $(BUILD)`),
   add `$(BUILD)/pic/mach_backend.o` to the `libasmtest_hwtrace` PIC list (lines ~2784–2801)
   and a `pic/` compile rule mirroring line ~2736. Because macOS has no Docker lane, the
   Mach frameworks (`<mach/*.h>`) come from the host SDK — no new `-l` flag is needed for
   the core Mach calls (they live in libSystem).
**Code.** Header + stubs + one real function (`available`/`skip_reason`). No Mach logic yet.
**Tests.** Add `make mach-stepper-test` (T6 builds it fully); for this task, a one-line C
probe that prints `asmtest_mach_available()` and `asmtest_mach_skip_reason()` compiled against
the tier. On the Intel macOS host it prints `available=1`; cross-compiled `-arch arm64` or on
Linux it prints `0` with the platform reason.
**Docs.** Internal-only for now; the user-facing page update lands in T7.
**Done when.**
- `make shared-hwtrace` on the macOS x86-64 host builds `libasmtest_hwtrace` with
  `mach_backend.o` linked, no warnings.
- The probe prints `available=1` on the Intel macOS host and `available=0` with the
  platform skip reason on a `-arch arm64` build.
- A Linux `make hwtrace-test` still builds and passes unchanged (the `#else` stub is inert).

### T2 — task_for_pid + Mach exception-port receive loop  (M, depends on: T1)
**Goal.** A reusable helper that obtains a target task port and receives its debug
exceptions: allocate a Mach port, set it as the task's `EXC_MASK_BREAKPOINT` exception
port, and drive a `mach_msg` receive loop that dispatches each exception to a handler.
**Steps.**
1. Implement `static kern_return_t mach_get_task(pid_t pid, task_t *out)` calling
   `task_for_pid(mach_task_self(), pid, out)`. Translate `KERN_FAILURE`/`KERN_INVALID_ARGUMENT`
   into `ASMTEST_MACH_EPERM` at the call sites (this is the entitlement/root gate firing).
2. Implement port setup: `mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &exc_port)`,
   then `mach_port_insert_right(mach_task_self(), exc_port, exc_port, MACH_MSG_TYPE_MAKE_SEND)`,
   then `task_set_exception_ports(task, EXC_MASK_BREAKPOINT, exc_port, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, THREAD_STATE_NONE)`.
   `EXC_MASK_BREAKPOINT` is `(1<<6) = 0x40`; ORing `MACH_EXCEPTION_CODES` (`0x80000000`) into the
   behavior selects the 64-bit `mach_exc_*` message variants.
3. Generate the MIG server for the exception protocol. Add a build step that runs
   `mig -server mach_excServer.c -header mach_exc.h $(shell xcrun --show-sdk-path)/usr/include/mach/mach_exc.defs`.
   Modern macOS ships **no** top-level `/usr/include`, so `mach_exc.defs` lives inside the active
   SDK — on the dev host at
   `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/mach/mach_exc.defs` (verified
   present). `mig` opens that positional `.defs` path literally (it is not resolved on any include
   path), so a hardcoded `/usr/include/mach/mach_exc.defs` fails with "No such file or directory" on
   this host; use `$(shell xcrun --show-sdk-path)` in the T6 make rule (or `$(SDKROOT)`) so the path
   tracks whichever SDK is active. `mig` is part of the Xcode Command Line Tools already on the host.
   Compile `mach_excServer.c` into the test/target. The generated `mach_exc_server(msg, reply)`
   dispatcher calls back into the `catch_mach_exception_raise*` symbols this backend defines.
   Alternatively hand-roll the 64-bit exception message struct if avoiding the MIG step is
   preferred — document whichever is chosen in the TU header comment.
4. Implement the receive loop: `mach_msg(&req.head, MACH_RCV_MSG, 0, sizeof req, exc_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL)`,
   feed the message to `mach_exc_server`, then send the reply. Returning `KERN_SUCCESS` in the
   reply is what **resumes the stopped thread** — the whole stop/continue rhythm rides on that.
5. Store per-trace context (active region `[base, base+len)`, the `asmtest_trace_t*`, the target
   `task_t`, and the faulting `thread_act_t`) in a file-scope struct the `catch_*` callback reads,
   since the MIG callback signature is fixed. Single active trace at a time, exactly as the
   in-process tier's single-active-region MVP.
**Code.** The exception plumbing only; the single-step *policy* (arm TF, record RIP) is T3's
`catch_mach_exception_raise`. Keep this file's Mach glue isolated so T3 is a pure policy fill-in.
**Tests.** Not independently user-testable (no trace produced yet); its correctness is proven
transitively by T3's live trace. Manual check: a debug build logs one received `EXC_BREAKPOINT`
message for a target that hits an `int3`, proving the port claims the exception before the BSD
signal path does.
**Docs.** Internal-only (plumbing).
**Done when.**
- `make shared-hwtrace` links the MIG-generated server with no undefined `catch_*` symbols
  (T3 supplies them; a temporary stub returning `KERN_SUCCESS` satisfies the link for this task).
- On the Intel macOS host, a scratch driver that `task_for_pid`s a cooperating child and sets its
  exception port receives at least one Mach exception message (logged), or returns
  `ASMTEST_MACH_EPERM` cleanly when run without the entitlement/root (proving the gate path).

### T3 — Single-step trace_attached engine (thread_get_state / thread_set_state)  (L, depends on: T2)
**Goal.** `asmtest_mach_trace_attached(pid, base, len, result, trace)` produces the exact same
ordered in-region instruction/block offsets as the Linux `asmtest_ptrace_trace_attached` and the
in-process Darwin stepper — byte-for-byte `[0x0, 0x3, 0x6, 0xc, 0x11]` for the shared fixture.
**Steps.**
1. On entry the caller owns that `pid` is already suspended at (or before) `base` — mirror the
   Linux contract in the header comment (the caller `task_suspend`s / uses `run_to`; T5 supplies
   `run_to`). Get the target task via T2's helper; enumerate threads with
   `task_threads(task, &list, &count)` and select the running thread (the one whose `__rip` is in
   or approaching the region — for the single-threaded supported target it is `list[0]`).
2. **Arm single-step directly in thread state**, not via `ptrace`: read
   `thread_get_state(thread, x86_THREAD_STATE64, (thread_state_t)&s, &count)` where
   `x86_THREAD_STATE64 = 4` and `count = x86_THREAD_STATE64_COUNT` (= 42 ints = 21 `uint64`),
   set `s.__rflags |= 0x100` (EFL_TF), and write it back with `thread_set_state(...)`. This is
   the remote analog of `ss_backend.c`'s `SS_SET_TF`.
3. Resume the thread (`thread_resume` / reply `KERN_SUCCESS` from T2's loop) and enter the receive
   loop. Each `#DB` arrives as `catch_mach_exception_raise(..., exception=EXC_BREAKPOINT, code[0]=EXC_I386_SGL=1, ...)`.
   In the callback: `thread_get_state` to read `s.__rip`; if `base ≤ __rip < base+len`, append the
   offset `__rip - base` to `trace` (mirrors `ss_backend.c`'s in-region filter and the AMD/PT
   decoders' in-region skip — callees and glue are stepped but not recorded). Re-assert
   `s.__rflags |= 0x100` and `thread_set_state` so the next instruction also traps. Reply
   `KERN_SUCCESS` to resume.
4. Read the region bytes for block normalization from the **target's** address space with
   `mach_vm_read(task, (mach_vm_address_t)base, len, &data, &data_cnt)` — the macOS analog of
   the Linux `process_vm_readv`. Feed those bytes to the existing Capstone length-decoder
   (`asmtest_disas`) to derive block boundaries exactly as `ss_backend.c` / `pt_backend.c` do:
   a block starts at region entry and at any recorded `RIP` that is not `prev_RIP + len(prev)`.
5. Termination: the trace ends when control leaves the region back to the caller (first recorded
   in-region instruction followed by an out-of-region return address that is the region's caller),
   matching the Linux tracer. A call *out* of the region to a helper is stepped over (run the
   thread to the call's return address via a temporary breakpoint — reuse T5's `run_until` seam)
   and recording resumes; only a genuine return/tail-jump ends the trace. Clear `__rflags` TF and
   restore the task's original exception ports (saved from `task_get_exception_ports` before T2's
   `task_set_exception_ports`) on exit.
6. Honest truncation: if `asmtest_disas` cannot decode at a recorded `RIP`, or the capture buffer
   overflows, set `trace->truncated = true` and stop — the same loss bit the other backends use.
   Never emit a partial trace as complete.
**Code.** Quote-mirror `ss_backend.c`'s in-region gate and block derivation, but with the state
read/write going through `thread_get_state`/`thread_set_state` instead of the `ucontext`. Reuse
`asmtest_disas` and `asmtest_trace_*` unchanged — no new trace surface.
**Tests.** Extend the C harness (T6's `tests/mach/test_mach_stepper.c`): allocate an executable
region with the shared fixture (`mov; add; cmp; jle; dec; ret`), fork a child that `task_suspend`s
itself at entry (or is driven by `run_to`), attach and trace it, and assert `insns[] ==
{0x0,0x3,0x6,0xc,0x11}` and `blocks[]` equal to the emulator/PT partition. A pass prints the exact
stream; a failure prints the mismatched offset. A `truncated` self-modifying fixture asserts the
loss bit.
**Docs.** Covered by T7.
**Done when.**
- `make mach-stepper-test` on the Intel macOS host (signed per T6) traces the fixture out of
  process and asserts `[0x0,0x3,0x6,0xc,0x11]` — identical to `make hwtrace-test`'s in-process stream.
- A 20+-trip loop fixture reconstructs with no depth ceiling (proving per-instruction completeness).
- Run without the entitlement/root, the test self-skips with `ASMTEST_MACH_EPERM` and exits 0.

### T4 — trace_call: fork-and-trace a self-contained blob  (M, depends on: T3)
**Goal.** `asmtest_mach_trace_call(code, len, args, nargs, result, trace)` forks its own tracee,
runs `code(args…)` in the child, and reconstructs the trace in the parent — the fork-internal
analog of `asmtest_ptrace_trace_call`, needing no external attach bookkeeping.
**Steps.**
1. `fork()`. In the **child**: `raise(SIGSTOP)` to hand control to the parent before executing the
   blob, then (on resume) call `code` with up to six integer args per the SysV/Darwin x86-64 ABI
   and `_exit` with the return value low bits (the parent reads the real return from `__rax` at the
   `ret`, exactly as the Linux tracer reads RAX). The child inherits the parent's executable mapping
   at the same address (the blob must already be executable in this process, e.g. via
   `asmtest_hwtrace_exec_alloc` — same precondition the Linux header states).
2. In the **parent**: `waitpid` for the child's stop, `task_for_pid(child)`, set the exception port
   (T2), arm TF and single-step-record through the region (T3's engine, pointed at `[code, code+len)`),
   then let the child run to exit. `task_for_pid` of one's own child succeeds when the test binary is
   signed with `get-task-allow` (T6) or run as root; surface `ASMTEST_MACH_EPERM` otherwise.
3. `*result` receives the child's `__rax` at the region return; `result` may be NULL.
**Code.** Reuse T3's engine wholesale — this task is the fork/wait/`task_for_pid` wrapper around it.
**Tests.** In `test_mach_stepper.c`, `asmtest_mach_trace_call(fixture, len, {20,22}, 2, &r, t)` asserts
`r == 42` and `insns[] == {0x0,0x3,0x6,0xc,0x11}`. A loop fixture asserts the 62-step stream the Linux
`trace_call` test uses, proving parity across the two OSes' out-of-process paths.
**Docs.** Covered by T7.
**Done when.**
- `make mach-stepper-test` traces a forked blob and asserts the return value and exact offsets.
- Self-skips cleanly (`EPERM`, exit 0) when unsigned and non-root.

### T5 — run_to: breakpoint an attached foreign target to a method entry  (M, depends on: T3)
**Goal.** `asmtest_mach_run_to(pid, addr)` runs an already-attached target forward until it itself
reaches `addr`, then leaves it stopped exactly there — the uncontrolled-timing glue that lets
`trace_attached` trace a real managed runtime's JIT method (the macOS analog of
`asmtest_ptrace_run_to`).
**Steps.**
1. Plant a software breakpoint: read the byte at `addr` with `mach_vm_read`, make the page writable
   if needed (`mach_vm_protect(task, page, size, FALSE, VM_PROT_READ|VM_PROT_WRITE|VM_PROT_COPY)`),
   write `0xCC` (int3) with `mach_vm_write`, restore protection. Save the original byte.
2. Resume the target and run the receive loop until a `catch_mach_exception_raise` arrives with
   `exception=EXC_BREAKPOINT, code[0]=EXC_I386_BPT=2` (the int3 subcode — distinct from the
   single-step subcode `EXC_I386_SGL=1` T3 handles). Restore the original byte, and rewind
   `__rip -= 1` via `thread_set_state` (x86 traps *after* executing int3), leaving the PC exactly at
   `addr` — the precondition `trace_attached` expects.
3. W^X fallback: if `mach_vm_write`/`mach_vm_protect` cannot make the code writable (a hardened JIT
   heap), fall back to an **x86-64 hardware execution breakpoint** by writing DR0/DR7 through
   `thread_set_state(thread, x86_DEBUG_STATE64, …)` — the Darwin analog of the Linux tracer's
   `PTRACE_POKEUSER` DR0/DR7 path, which writes no code and is per-thread. Same shared `run_until`
   seam T3 uses for call-out step-over.
4. Return `ASMTEST_MACH_OK` (stopped at `addr`), `ASMTEST_MACH_ENOENT` (target exited first),
   `ASMTEST_MACH_EINVAL` (NULL addr), or `ASMTEST_MACH_ETRACE` on a Mach failure (breakpoint
   best-effort removed).
**Code.** Mirror the `asmtest_ptrace_run_to` contract comment in
[include/asmtest_ptrace.h](../../../include/asmtest_ptrace.h) (lines ~236–254) exactly, translating
`PTRACE_POKETEXT`→`mach_vm_write`, `PTRACE_CONT`→exception-loop resume, `PTRACE_POKEUSER`→
`thread_set_state(x86_DEBUG_STATE64)`.
**Tests.** In `test_mach_stepper.c`: a fixture that spins calling a function at a known `addr`;
`run_to(pid, addr)` then `trace_attached(pid, addr, len, …)` recovers the method's stream with no
cooperative go-flag — the macOS twin of the Linux `test_run_to_and_trace`. Force the hardware-bp path
with an env flag (`ASMTEST_MACH_HW_BP`, mirroring `ASMTEST_PTRACE_HW_BP`) over ordinary memory to
validate DR0/DR7 deterministically.
**Docs.** Covered by T7.
**Done when.**
- `make mach-stepper-test` runs the resolve→attach→`run_to`→trace flow against a forked victim and
  asserts the exact stream, both software-int3 and (with the env flag) hardware-breakpoint paths.
- Self-skips cleanly without entitlement/root.

### T6 — Codesigning harness, host make target, and self-skip  (M, depends on: T1)
**Goal.** A repeatable way to build, self-sign, and run the Mach stepper's live test on a macOS
x86-64 host — with an honest self-skip when the entitlement/root gate is not satisfied — so the lane
gates cleanly locally and on the `macos-15-intel` CI leg.
**Steps.**
1. Add `scripts/codesign-debugger.sh` (new): write an ad-hoc entitlements plist containing
   `<key>com.apple.security.cs.debugger</key><true/>` and `<key>com.apple.security.get-task-allow</key><true/>`
   (the debugger key authorizes `task_for_pid` for the tracer; `get-task-allow` lets the tracer obtain
   the forked child's task port in T4), then `codesign --entitlements <plist> -f -s - "$1"` (`-s -` =
   ad-hoc self-sign). This is host-only tooling (Command Line Tools ship `codesign`); no Docker lane,
   because macOS cannot run in the project's Linux containers — a legitimate host requirement, not a
   skippable dependency.
2. Add a darwin-guarded `make mach-stepper-test` target in
   [mk/native-trace.mk](../../../mk/native-trace.mk) near `hwtrace-test` (line ~2127). Guard it at
   the recipe level so it prints a skip line and exits 0 off Darwin — mirror the real host-guard
   idiom the `macos-clean-test` target uses in [mk/bindings.mk](../../../mk/bindings.mk)
   (lines ~606–608):
   `@case "$(UNAME_S)" in Darwin) ;; *) echo "mach-stepper-test: darwin-only (host is $(UNAME_S)) — skipping"; exit 0 ;; esac`
   (`UNAME_S := $(shell uname -s)` is already defined in the parent `Makefile` at line 221; the
   equivalent `$(filter Darwin,$(shell uname -s))` primitive works too). Do **not** model this on
   `win64-msabi-test` in [mk/win64.mk](../../../mk/win64.mk) line 53 — its `$(filter Darwin,…)`
   there only selects an object format (`macho64`/`elf64`) and that target runs on both Linux and
   macOS with no skip guard, so it is not a host-guard exemplar. The target: compiles
   `src/mach_backend.c`, `mach_excServer.c` (from the T2 `mig` step), and
   `tests/mach/test_mach_stepper.c` into a test binary, runs `scripts/codesign-debugger.sh` over it,
   then runs it.
3. The test binary self-skips at runtime when `asmtest_mach_trace_call`/`_attached` return
   `ASMTEST_MACH_EPERM` (no entitlement took effect and not root) — print the reason and exit 0, never
   fail. Document the two ways to satisfy the gate: the ad-hoc self-sign (step 1, triggers a one-time
   admin-authorization dialog granting a ~10-hour session on first `task_for_pid`), or `sudo make
   mach-stepper-test` (root reaches unentitled non-SIP targets directly).
4. Add the lane to `make help`'s output (the source-of-truth target listing) under the hwtrace group,
   with a one-line note that it needs a macOS x86-64 host + codesign or sudo.
**Code.** One script + one make target + the test's self-skip branch. No changes to any `Dockerfile.*`
(macOS is not containerizable here).
**Tests.** This task *is* the test harness; its own acceptance is that the lane runs green when signed
and self-skips green when not.
**Docs.** Covered by T7; add the `make help` line here.
**Done when.**
- `make mach-stepper-test` on the Intel macOS host, after `scripts/codesign-debugger.sh`, runs all of
  T3/T4/T5's assertions live and exits 0.
- The same target run unsigned and non-root prints the `EPERM` self-skip and exits 0 (does not fail).
- On Linux / Apple-Silicon the target prints its platform skip and exits 0.

### T7 — User-facing docs + changelog  (S, depends on: T3, T4, T5)
**Goal.** The published tracing guide documents the macOS out-of-process Mach path alongside the
Linux ptrace path, and the change is recorded in the changelog.
**Steps.**
1. Extend [docs/guides/tracing/native-tracing.md](../../../docs/guides/tracing/native-tracing.md)'s
   "Out-of-process variant (W2 — ptrace)" section (around line 416): add a short subsection
   "macOS (Mach exception ports)" explaining that where Linux uses `ptrace`, macOS uses
   `task_for_pid` + a `EXC_MASK_BREAKPOINT` Mach exception port + `thread_set_state` to arm the trap
   flag, exposed as `asmtest_mach_trace_call` / `asmtest_mach_trace_attached` / `asmtest_mach_run_to`,
   and that it needs the `com.apple.security.cs.debugger` entitlement (ad-hoc self-sign) or root.
   The current section's "runs on **Linux x86-64 and AArch64**" wording must be corrected to also name
   the macOS x86-64 Mach path so the guide is not silently Linux-only.
2. Add a one-line row/note to [docs/reference/portability.md](../../../docs/reference/portability.md)'s
   Linux/macOS matrix noting the out-of-process stepper is available on macOS x86-64 via Mach ports.
   **While in that file, also fix a pre-existing stale CI leg:** line 15 still lists the retired
   `macos-13` runner (`ubuntu-24.04-arm`, `macos-latest`, `macos-13`) — the macOS Intel CI leg
   retired 2025-12-08 and must now read `macos-15-intel`, so correct `macos-13` → `macos-15-intel`
   in that runner list (the rest of this doc already uses `macos-15-intel` throughout).
   Keep the Sphinx build warning-clean (`make docs` is `-W` fail-on-warning; internal links only where
   allowed).
3. Add a `CHANGELOG.md` entry under `## [Unreleased]` → `### Added`: one bullet naming the macOS
   out-of-process single-step tracer (`asmtest_mach_*`) and that it completes the W2 foreign-process
   story on macOS.
**Code.** Docs only.
**Tests.** `make docs` (or `make docker-docs`) builds warning-clean.
**Docs.** This task *is* the docs.
**Done when.**
- `make docs` builds with no new warnings and the native-tracing guide shows the macOS Mach path.
- `CHANGELOG.md` has the `Added` bullet under `[Unreleased]`.

## Task order & parallelism

```
T1 (skeleton/gate/build) ──▶ T2 (exception-port loop) ──▶ T3 (trace_attached engine) ─┬─▶ T4 (trace_call)
   │                                                                                    ├─▶ T5 (run_to)
   └──────────────────────────────────────────────────────▶ T6 (codesign + make lane) │
                                                             T7 (docs) ◀────────────────┘ (after T3/T4/T5)
```

- **Critical path:** T1 → T2 → T3 → (T4 ∥ T5) → T7. T3 is the large task; T2 is its prerequisite.
- **Independent / concurrent:** T6 (codesign harness + make target) depends only on T1's TU existing,
  so one developer can build the signing/lane infrastructure while another writes T2→T3. T4 and T5
  both build on T3's engine and can be done by two people in parallel once T3 lands. T7 waits on the
  three entry points being real.

## Constraints & gates

- **Host requirement (not a skippable dependency).** This lane needs a **macOS x86-64 host** — one
  exists ([intel-macos-x86_64-de7ec54c](../../../benchmarks/boxes/intel-macos-x86_64-de7ec54c/features.json),
  macOS 14.7.5 / Intel i7-8559U) and the CI Intel-mac leg is **`macos-15-intel`** (the `macos-13`
  runner was retired 2025-12-08; never target `macos-13`). macOS cannot run in the project's Linux
  Docker containers, so — unlike the CLAUDE.md "add the missing dependency to a `Dockerfile.*`" rule —
  the Mach frameworks, `mig`, and `codesign` come from the host Xcode Command Line Tools. Nothing here
  is installable into a container; the lane is a host lane that self-skips off Darwin.
- **Permission gate (`task_for_pid`) — soft, not hard.** `task_for_pid` requires either the
  `com.apple.security.cs.debugger` entitlement (ad-hoc self-sign, T6), root (`sudo`), or the target
  carrying `com.apple.security.get-task-allow`. This is **not** a hard credential gate: the harness
  obtains it locally via `codesign -s -` (an admin dialog grants a ~10-hour session on first use) or
  `sudo`. When neither is present the lane self-skips with `ASMTEST_MACH_EPERM` and exits 0 — record
  the reason, never fail. It **cannot** attach to SIP-protected / platform binaries lacking
  `get-task-allow`; that is a real OS boundary, out of scope.
- **Apple Silicon (arm64 macOS) is out of scope** and hardware-gated for this doc: the verified
  research is x86-scoped (`x86_THREAD_STATE64`, `EFL_TF`, `EXC_I386_SGL`). The arm64 Mach stepper
  (`arm_thread_state64`, `EXC_ARM_BREAKPOINT`, single-step via `SPSR`) is a future extension; the TU
  gates it out (`__x86_64__ && __APPLE__`) and self-skips.
- **Supported target contract** (same as the in-process and Linux tiers): a deterministic,
  single-threaded, well-behaved compute routine (≤6 integer args) that may call out to helpers. In-routine
  `POPF`/`IRET`/self-modifying code set `trace->truncated` rather than emit a corrupt trace.

## Research notes (verified 2026-07-17)

External facts this doc depends on, each with its source. Numeric constants are stable across macOS
releases even though XNU line numbers drift (Apple ships XNU from a rolling `main` branch).

- **XNU cripples `ptrace`, forcing the Mach path.** In `bsd/kern/mach_process.c`, `PT_STEP` (=9) and
  `PT_CONTINUE` (=7) share a case that returns `ENOTSUP` unless `addr == (caddr_t)1`; the kernel
  comment states this forces use of Mach SPIs and `task_for_pid` security checks to adjust the PC. So
  `RIP`/`RFLAGS` edits are impossible through `ptrace` — the Mach-port + `thread_set_state` path this
  doc takes is the only out-of-process option. `PT_STEP` itself only calls
  `thread_setsinglestep(th, 1)`, which sets `EFL_TF`. Sources:
  <https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/mach_process.c>,
  <https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/ptrace.h>,
  <https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/i386/bsd_i386.c>.
- **Trap → Mach exception mapping.** In `osfmk/i386/trap.c` `user_trap`, `T_DEBUG` (the `#DB` raised by
  the trap flag or a debug-register match) maps to `exc = EXC_BREAKPOINT; code = EXC_I386_SGL`, and
  `T_INT3` maps to `EXC_BREAKPOINT; code = EXC_I386_BPT`. Constants: `EXC_BREAKPOINT = 6`,
  `EXC_MASK_BREAKPOINT = (1<<6) = 0x40`; `EXC_I386_SGL = 1` (single-step subcode), `EXC_I386_BPT = 2`
  (int3 subcode). Sources:
  <https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/i386/trap.c>,
  <https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/exception_types.h>,
  <https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/i386/exception.h>.
- **Exception-port flow.**
  `task_set_exception_ports(task, EXC_MASK_BREAKPOINT, port, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, THREAD_STATE_NONE)`
  after `mach_port_allocate(RECEIVE)` + insert `MAKE_SEND` right; then a `mach_msg` receive loop feeds
  the MIG-generated `mach_exc_server`, which dispatches to `catch_mach_exception_raise`. Returning
  `KERN_SUCCESS` in the reply resumes the thread. `EXCEPTION_DEFAULT = 1`; `MACH_EXCEPTION_CODES =
  0x80000000` selects the 64-bit `mach_exc_*` variants. Sources:
  <https://web.mit.edu/darwin/src/modules/xnu/osfmk/man/task_set_exception_ports.html>,
  <https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/exception_types.h>.
- **Thread state for RIP/RFLAGS.** Flavor `x86_THREAD_STATE64 = 4`; `x86_THREAD_STATE64_COUNT = 42`
  (21 `uint64` = 42 ints, computed — not quoted verbatim). `thread_get_state` reads
  `struct __darwin_x86_thread_state64` (fields `__rax … __rip, __rflags, __cs, __fs, __gs`); set
  `s.__rflags |= 0x100` (EFL_TF, bit 8) to arm single-step, write back with `thread_set_state`. Sources:
  <https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/i386/thread_status.h>,
  <https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/i386/_structs.h>.
- **Entitlement + self-sign.** `com.apple.security.cs.debugger` (Boolean, macOS 10.7+) marks the app a
  debugger that may `task_for_pid` unsigned/third-party apps carrying
  `com.apple.security.get-task-allow`; it cannot reach processes lacking `get-task-allow`
  (SIP/platform binaries). Ad-hoc self-sign: write an entitlements plist with the debugger key
  (`true`), then `codesign --entitlements ent.plist -f -s - <binary>`. Non-root run triggers a one-time
  admin-authorization dialog granting a ~10-hour session; `sudo` reaches unentitled non-SIP targets
  directly. Sources:
  <https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.debugger>,
  <https://afine.com/to-allow-or-not-to-get-task-allow-that-is-the-question>,
  <https://hacktricks.wiki/en/macos-hardening/macos-security-and-privilege-escalation/macos-security-protections/macos-dangerous-entitlements.html>.
- **Caveat.** `DevToolsSecurity -enable`, `_developer`-group membership, and the exact debugserver
  entitlement plist come from secondary/historical sources; Apple publishes no single canonical
  "sudo + codesign" recipe. Treat the ad-hoc self-sign and `sudo` as the two supported paths and record
  which one the CI leg uses.

## Out of scope

- The Linux out-of-process ptrace tracer's correctness/blockstep work — see
  [ptrace-blockstep-tracer-correctness.md](ptrace-blockstep-tracer-correctness.md).
- The Linux AArch64 ptrace single-step live-stream validation — see
  [aarch64-ptrace-single-step-validation.md](aarch64-ptrace-single-step-validation.md).
- The Intel PT foreign-pid attach path (a different capture mechanism entirely) — see
  [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md).
- The macOS clean-room dlopen lanes (Tracks A–E of the clean-test plan) — a different code path,
  owned by [macos-cleanroom-lanes.md](macos-cleanroom-lanes.md).
- The macOS DynamoRIO port — a different tier — owned by
  [macos-dynamorio-port.md](macos-dynamorio-port.md).
- The macOS **in-process** single-step front-end (already landed in
  [src/ss_backend.c](../../../src/ss_backend.c)); this doc is only its out-of-process sibling.
