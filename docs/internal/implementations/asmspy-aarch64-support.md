# asmspy AArch64 support: single-step engine abstraction and NT_ARM_HW_WATCH data watchpoints — implementation

> **Sources.** Actioned from
> [asmspy-plan.md](../plans/asmspy-plan.md) (Theme F row `F-arch-abstraction`,
> line 140),
> [live-attach-dataflow-followup-plan.md](../archive/plans/live-attach-dataflow-followup-plan.md)
> (F3 carryover, the AArch64 `NT_ARM_HW_WATCH` analog),
> [2026-07-15-hw-watchpoint-spike.md](../analysis/2026-07-15-hw-watchpoint-spike.md)
> (the "AArch64 analog" section), and
> [zen2-singlestep-trace-plan.md](../archive/plans/zen2-singlestep-trace-plan.md)
> (the landed AArch64 library stepper this doc mirrors). Written 2026-07-17. If
> this doc and a source disagree, this doc wins (sources may be stale); if the
> CODE and this doc disagree, re-verify before implementing.

## Why this work exists

`asmspy` (the ncurses + headless out-of-process tracer under [cli/](../../../cli))
is Linux/**x86-64-blind**: point it at a process on an AArch64 box and `make cli`
prints a clean architecture skip and stops there. The single-step engines read
`regs.rip`, write `EFLAGS.TF`, read `orig_rax`, and hardcode
`ASMTEST_ARCH_X86_64` — none of which exist on AArch64 — even though the
disassembler already handles ARM64 and the *library's* out-of-process stepper
([src/ptrace_backend.c](../../../src/ptrace_backend.c)) already single-steps
AArch64 tracees. This work lifts the register/single-step/detach layer into an
architecture shim so all of asmspy's engines run on AArch64, and adds the
`NT_ARM_HW_WATCH` data-watchpoint engine so `asmspy --watch` works there too —
the AArch64 analog of the x86 DR0-3 watchpoint that landed 2026-07-15.

## What already exists (verified 2026-07-17)

The substrate you build on, each claim checked against the working tree:

- **The engines are x86-64-hardcoded.** In
  [cli/asmspy_engine.c](../../../cli/asmspy_engine.c): the Trap-Flag write-back at
  line 1723 (`regs.eflags &= ~ASMSPY_EFLAGS_TF`), the syscall-number read at
  1965 (`ts->ent_nr = (long)regs.orig_rax`), the RIP-rewind at 2104-2106
  (`regs.rip != base + 1` … `regs.rip = base`), and `ASMTEST_ARCH_X86_64` passed
  to the disassembler at 2868, 3281/3285, 3561/3571, 3838 and 4074. `struct
  user_regs_struct` + `PTRACE_GETREGS` appear at 11 sites. The syscalls engine
  funnels its six syscall-argument registers through `scarg()` (line 542,
  `rdi/rsi/rdx/r10/r8/r9`), but ~25 `ap_*` decode helpers **also** read
  `e->rdi` / `e->rsi` / `e->rdx` directly (none of these fields exist on AArch64 —
  T4 ports both the funnel and the direct sites). A grep for
  `__aarch64__|PSTATE|NT_PRSTATUS|GETREGSET` across `cli/` returns **zero
  matches** — no AArch64 support exists in the CLI today.
- **The library already single-steps AArch64.**
  [src/ptrace_backend.c](../../../src/ptrace_backend.c) is compiled for `__linux__
  && (__x86_64__ || __aarch64__)` (line 295). Its `read_pc_ret()` (line 431) reads
  PC + return register + SP + LR via `PTRACE_GETREGS` on x86-64 and via
  `PTRACE_GETREGSET`/`NT_PRSTATUS` on AArch64; `set_hw_bp`/`clear_hw_bp` drive the
  `NT_ARM_HW_BREAK` regset (lines 502-550); the `brk #0` software breakpoint is
  `0xd4200000` (line 420); `probe_singlestep()` (lines 560-598) is a hang-proof
  self-probe that returns 0 under qemu-user, so `asmtest_ptrace_available()`
  self-skips there. **This is the file to mirror** — the shim you write for `cli/`
  is the same seam, reimplemented over asmspy's own register reads.
- **The x86 watch engine is complete and gated.** `asmspy_engine_watch`
  ([cli/asmspy_engine.c](../../../cli/asmspy_engine.c) line 4124) is wrapped in
  `#if defined(__x86_64__)` (line 3989); the `#else` at 4293 is a stub returning
  `ASMSPY_WATCH_UNAVAIL`. Its parts — `watch_dr7_word` (4002), `watch_arm`
  (4025), `watch_disarm` (4040), `watch_dr6` (4048), `watch_decode_dir` (4061,
  uses `asmtest_operands(ASMTEST_ARCH_X86_64, …)`), `watch_teardown` (4097), and
  the main loop with per-thread arming across `PTRACE_O_TRACECLONE` — are the
  template for the AArch64 arm.
- **The disassembler is arch-parametric.** [src/disasm.c](../../../src/disasm.c)
  handles both `ASMTEST_ARCH_X86_64` and `ASMTEST_ARCH_ARM64` (lines 41-45), and
  `asmtest_operands` ([include/asmtest_valtrace.h](../../../include/asmtest_valtrace.h)
  line 159) takes an `asmtest_arch_t` — nothing in the decode path is x86-only.
- **The build gate.** [mk/cli.mk](../../../mk/cli.mk) sets `CLI_ARCH := $(shell
  uname -m)` (line 21) and gates both `cli` (line 72) and `cli-smoke` (line 332)
  to skip with `# SKIP … asmspy is x86-64-only` off x86-64. `docker-cli` (line
  363) runs `cli-smoke` in the `asmtest-cli` image; the arm64-host case hits the
  same gate because `_docker_plat` follows the host.
- **CI already has AArch64 runners.** [.github/workflows/ci.yml](../../../.github/workflows/ci.yml)
  runs several jobs on `ubuntu-24.04-arm` (lines 37, 84, 275, 309). The `cli` job
  (line 557) currently runs on `ubuntu-latest` only.

**Prove the baseline is green before touching anything.** On an x86-64 host with
Docker: `make docker-cli` builds the image and runs `cli-smoke`; the last line is
`SMOKE_RC=0` and the run ends `PASS`. That is the regression floor — every x86-64
assertion in this doc must still print `PASS` after your changes.

## Tasks

### T1 — Extract a `cli/` register/step/detach arch shim  (M, depends on: none)

**Goal.** A header-only `cli/asmspy_arch.h` that gives every engine PC / return
register / SP access and single-step arm/disarm without any engine naming
`regs.rip`, `regs.eflags`, or `PTRACE_GETREGS` directly — with an x86-64 body and
an AArch64 body behind `#if defined(__aarch64__)`. (Syscall-number and
argument-register access are **not** in this header: they live in the syscalls
engine's own `scarg()` funnel and `ap_*` decode helpers, ported per-arch in T4.)

**Steps.**
1. Create [cli/asmspy_arch.h](../../../cli/asmspy_arch.h), header-only, following
   the `asmspy_graphsort.h` / `asmspy_treefilter.h` / `asmspy_dataview.h`
   precedent (pure, `static inline`, unit-testable). Mirror the seam already
   proven in [src/ptrace_backend.c](../../../src/ptrace_backend.c) `read_pc_ret`
   (line 431) and `set_pc` (line 466).
2. Define an opaque `asmspy_regs_t` wrapping `struct user_regs_struct` on x86-64
   and `struct user_regs_struct` (the AArch64 `user_pt_regs`: `regs[31]`, `sp`,
   `pc`, `pstate`) on AArch64.
3. Provide the accessors listed under **Code**. On x86-64 they read the fields
   directly; on AArch64 they read/write via `PTRACE_GETREGSET`/`PTRACE_SETREGSET`
   with `NT_PRSTATUS` and an `iovec`, exactly as the library does.
4. Replace the raw `PTRACE_GETREGS` + `regs.rip` reads in the four single-step
   engines (`asmspy_engine_stream` at 2757, `asmspy_engine_graph` at 3157,
   `asmspy_engine_tree` at 3396, `asmspy_engine_region` at 2483) and the syscalls
   engine (2757's sibling at 1892) with shim calls. Run `make cli` after each
   engine to keep the x86-64 build green.

**Code.** New file, roughly:
```c
/* cli/asmspy_arch.h — register/step access, x86-64 or AArch64, one seam. */
typedef struct { struct user_regs_struct r; } asmspy_regs_t;
static inline int  asmspy_regs_read (pid_t tid, asmspy_regs_t *g);   /* GETREGS | GETREGSET(NT_PRSTATUS) */
static inline int  asmspy_regs_write(pid_t tid, const asmspy_regs_t *g);
static inline uint64_t asmspy_reg_pc (const asmspy_regs_t *g);       /* rip     | pc      */
static inline uint64_t asmspy_reg_ret(const asmspy_regs_t *g);       /* rax     | regs[0] */
static inline uint64_t asmspy_reg_sp (const asmspy_regs_t *g);       /* rsp     | sp      */
static inline void     asmspy_set_pc (asmspy_regs_t *g, uint64_t v); /* rip=    | pc=     */
```
On AArch64 the `user_pt_regs` layout is fixed (verified against the kernel — see
Research notes): `regs[0]` is the return-value register at byte offset 0, `pc` at
256, `sp` at 248, `pstate` at 264; the regset is 272 bytes. Read the whole set
with a single `iovec{&g->r, sizeof g->r}`.

**Tests.** Add `cli/test_arch.c` (a `cli-smoke` prereq, mirroring
`cli/test_symtab.c` / `cli/test_graphsort.c`): compile-only assertions that the
accessors resolve to the right field per arch (`_Static_assert(offsetof(...))`
where it can, otherwise a runtime `fork` + `PTRACE_TRACEME` child whose known
register values are read back through the shim — the pattern `probe_singlestep`
uses). A failure is a wrong offset (the read returns garbage); a pass reads the
value the child set. Runs on **every** host, so it covers the AArch64 body even
when built on x86-64 only by `_Static_assert` on the AArch64 struct behind an
`#ifdef` that the arm64 build compiles.

**Docs.** Internal-only; the shim is not user-facing. No changelog entry for T1
alone (T5 carries the user-visible "AArch64 supported" line).

**Done when.**
- `make cli` builds unchanged on x86-64 and `make docker-cli` still prints `PASS`.
- `cli/asmspy_arch.h` contains no engine logic (register access only) and every
  `regs.rip` / `regs.rax` in the four single-step engines and the syscalls engine
  is replaced by a shim call: `grep -n 'regs\.rip\|regs\.rax\|regs\.orig_rax'
  cli/asmspy_engine.c` returns only watch-engine sites (which T6 owns) plus the
  syscalls-engine syscall-number read that T4 finishes off. Raw `struct
  user_regs_struct` field access is otherwise confined to `cli/asmspy_arch.h`'s
  own x86 body — a **different file** that this grep, scoped to `asmspy_engine.c`,
  does not see.

### T2 — Single-step / detach: PSTATE.SS is kernel-owned, not a flag you clear  (S, depends on: T1)

**Goal.** The teardown that stops an AArch64 tracee from being killed by a
lingering step is correct for AArch64's single-step model, where — unlike x86 —
there is no user-writable trap flag.

**Steps.**
1. Gate `clear_trap_flag` ([cli/asmspy_engine.c](../../../cli/asmspy_engine.c)
   line 1719). On x86-64 it writes `EFLAGS.TF` clear via `SETREGS`. On AArch64
   there is **nothing to clear**: `PTRACE_SINGLESTEP` sets `TIF_SINGLESTEP`
   in-kernel and `PTRACE_CONT`/`PTRACE_DETACH` clear it
   (`user_disable_single_step`); the tracer *cannot* set or clear the step bit
   via a `pstate` write (`valid_user_regs` forces `pstate` bit 21 to mirror
   `TIF_SINGLESTEP`). So the AArch64 body of `clear_trap_flag` is a **no-op** —
   document that and return.
2. Gate `at_syscall_insn` (line 1732). x86 tests for `syscall`/`sysenter`/`int
   0x80` bytes; the AArch64 body tests for `svc #0` = `0xd4000001` (little-endian
   `01 00 00 d4`), a 4-byte read. Keep the fail-safe "return 1 (don't step) if the
   bytes can't be read".
3. `drain_pending_step` (line 1753) stays structurally identical — it single-steps
   once to consume a queued `#DB` — but its inner `regs.rip` read routes through
   the shim and its "poised on a syscall instruction" guard now uses the AArch64
   `at_syscall_insn`.

**Code.** No new function; three `#if defined(__aarch64__)` bodies inside the
existing helpers. Quote the existing x86 contract to preserve it — the comment at
1713-1718 explains *why* the x86 write is unconditional (the kernel masks the
forced TF out of `GETREGS`); the AArch64 body's comment must state the opposite
fact: the step bit is invisible **and** irrelevant to the tracer because a
`DETACH` clears `TIF_SINGLESTEP` for us.

**Tests.** Covered end-to-end by T5's arm64 `cli-smoke`. The survival tripwire
already exists: the "two-phase detach: target survives single-step + detach"
block in [cli/cli_smoke.sh](../../../cli/cli_smoke.sh) (around line 2118) starts
`threads_victim`, single-steps every thread via `--stream` (and call-steps via
`--tree`), detaches, then asserts the victim is still alive with `kill -0`
immediately after each — the `fail "detach-survival: victim KILLED by $view
detach"` at ~line 2140. A detach that leaves a step armed on AArch64 would kill
the victim with a `SIGTRAP` seconds later, which that `kill -0` catches. (Note the
plain `--stream` block near line 116 has **no** survival assertion of its own — it
only checks disassembly; the detach-survival block at ~2118 is the tripwire.) No
new unit test; the CI verification is that survival assertion on the arm64 leg.

**Docs.** Internal-only.

> ### MEASURED 2026-07-22 on Neoverse-N2: step 1's premise is FALSE, and it costs the target its life
>
> This task's step 1 says the AArch64 `clear_trap_flag` body can be a no-op
> because "a `DETACH` clears `TIF_SINGLESTEP` for us". A detach does clear
> `TIF_SINGLESTEP` — and that is not enough. arm64's `ptrace_disable()` is
> `user_rewind_single_step()` + `user_disable_single_step()`: the rewind **SETS
> `SPSR.SS`** (`TIF_SINGLESTEP` is still set at that instant) and the disable
> then clears only the thread flag. A thread detached while carrying an armed,
> never-consumed step therefore returns to userspace still active-pending and
> takes a **SIGTRAP with no tracer to absorb it**, killing the whole target.
>
> `drain_pending_step` is what normally consumes that step — but it deliberately
> **refuses to step a thread poised on a syscall instruction** (`at_syscall_insn`),
> because that step would only complete when the call returns, which for a thread
> parked in `pause()`/`futex` is never. So exactly those threads keep the armed
> step, and exactly those threads kill the process.
>
> **Reproduced without asmspy** (~40 lines: SEIZE every thread, INTERRUPT, resume
> once, INTERRUPT, DETACH; victim = two spinners + a leader in `pause()`):
>
> | resume mode for the parked leader | outcome |
> |---|---|
> | none (seize + interrupt only) | target **survives** |
> | `PTRACE_SINGLESTEP` | target **DIES** — kernel `SIGTRAP` (`si_pid=0`, `si_code=0`) at `svc+4` |
> | `PTRACE_CONT` | target **survives** |
>
> The kill lands **200-400 ms after the tracer exits** — once the parked syscall
> returns. Two consequences:
>
> 1. **`asmspy --stream <pid>` was fatal to every multi-threaded AArch64 target.**
>    Not the `--tid` filter, which this doc's status cell blamed for months: on a
>    fresh victim `--stream --tid=` works perfectly (20/20 lines, victim alive).
>    It ran *after* the whole-process control run in `cli_smoke.sh` and found the
>    corpse, so it wore the blame. Verified by an experiment matrix on the runner:
>    `--tid` alone ✔, `--tid` twice ✔, whole-process **twice** ✘ (run 2 finds all
>    threads dead, `termsig=5`), whole-process then `--tid` ✘.
> 2. **The existing survival tripwire could not see it.** The detach-survival
>    block checked `kill -0` *immediately*, and the victim is still alive then.
>    It now polls for ~2 s, which is what makes the tripwire test the class of bug
>    it was written for.
>
> **Fix (landed):** never arm a step on a thread that is poised on an `svc`.
> `step_resume()` resumes such a thread with `PTRACE_SYSCALL` instead — the call
> runs, the thread stops on the way *out*, and stepping resumes there, so nothing
> is armed across a call that may block and the thread is not dropped from the
> trace either (`PTRACE_CONT` would survive too, but would lose the thread). The
> syscall-stops are tagged `SIGTRAP|0x80` via `PTRACE_O_TRACESYSGOOD` (AArch64
> only — x86 never resumes with `PTRACE_SYSCALL` here, so its stop stream is
> byte-identical), and entry-vs-exit comes from the existing
> `syscall_stop_is_entry()` (`PTRACE_GET_SYSCALL_INFO`, no toggle to desync).
> Teardown fixes do NOT work and were tried and rejected on hardware: once armed
> inside a blocking call, the step cannot be revoked by the tracer — neither by
> `PTRACE_CONT` + re-INTERRUPT, nor by a `GETREGSET`/`SETREGSET` round-trip after
> the `TIF_SINGLESTEP` clear.
>
> The same rule had to be applied to `drain_pending_step`, which surfaced two more
> defects the first fix exposed:
>
> - it refused to step a thread poised **on** an `svc`, but not one already
>   **inside** the call (at a syscall-entry stop the pc is past the `svc`).
>   Stepping that one blocks until the call returns — never, for `pause()` — and
>   its `waitpid` retries through every `EINTR`, so `timeout` could not even kill
>   asmspy. `in_syscall_now()` (`/proc/<tid>/syscall`) is the guard that actually
>   matches the hazard — **AArch64 only**: applying it on x86 too regressed the
>   gating x86 `cli` leg, because a thread inside a call is exactly the queued-#DB
>   case x86's drain exists to service (and x86 rewinds `rip` onto the `syscall`
>   instruction for a restartable call, so `at_syscall_insn` already covers the
>   shape that would block there). Same symptom, opposite arch: skipping the drain
>   left the trap queued, the victim died after detach, and `--tid` — again — got
>   the blame.
> - its single wait could consume the queued **PTRACE_EVENT_STOP** from phase 1's
>   INTERRUPT instead of the step trap (measured: `status 0x80057f` on both
>   workers), leaving the step armed. x86 shrugs — `clear_trap_flag` disarms it —
>   so on AArch64 the drain now keeps stepping until a genuine trap lands.
>
> **Verified on the runner (Neoverse-N2, 2026-07-22)**: whole-process `--stream`,
> `--graph` and `--tree` each traced the victim and **left it alive** (all three
> killed it before), and a `--stream --tid=` run on the *same* victim afterwards
> returned a full 60-line single-thread trace. x86 is unaffected by construction
> and `make docker-cli` is green (`cli-smoke: PASS`).
>
> **Frontier moved, not closed**: with the kill gone, the arm64 `cli-smoke` now
> runs ~1400 lines further and fails at a *different*, pre-existing item —
> `--trace <pid> alpha_work 1 --tid=<runner>` reports the region as not sampled
> on the thread that runs it (the region engine's per-thread NT_ARM_HW_BREAK
> entry breakpoint; that engine seizes via `seize_threads`, so it is untouched by
> this change and was simply never reached before). The arm64 cli leg therefore
> stays `continue-on-error` until that one is settled too.

**Done when.**
- On the arm64 CI leg (T5), the detach-survival block
  ([cli/cli_smoke.sh](../../../cli/cli_smoke.sh)) passes **after its ~2 s wait**:
  `--stream` / `--tree` on `threads_victim` followed by detach leaves the victim
  alive. An immediate `kill -0` is not an acceptable form of this assertion —
  it is blind to the delayed kill above.
- `grep -n 'svc\|0xd4000001' cli/asmspy_engine.c` shows the AArch64 syscall-insn
  test present, and `step_resume` routes every whole-process step resume.

### T3 — Disassembler arch parameter + AArch64 call/return frame semantics  (M, depends on: T1)

**Goal.** The seven `ASMTEST_ARCH_X86_64` constants become the tracee's real
architecture, and the call-attribution / frame-stack logic uses AArch64's
`bl`-writes-LR (not push-return) semantics.

**Steps.**
1. Introduce a compile-time `ASMSPY_HOST_ARCH` (`ASMTEST_ARCH_X86_64` on
   `__x86_64__`, `ASMTEST_ARCH_ARM64` on `__aarch64__`) in `cli/asmspy_arch.h`.
   asmspy traces a process on the **same machine**, so the tracee's arch is the
   host arch — no per-target detection is needed (the i386 refusal at
   `asmspy_elf_class` already rejects the one same-machine mismatch that matters).
2. Replace the literal `ASMTEST_ARCH_X86_64` at
   [cli/asmspy_engine.c](../../../cli/asmspy_engine.c) 2868, 3281, 3285, 3561,
   3571, 3838 and 4074 with `ASMSPY_HOST_ARCH`. (Line 4074 is inside `--dataflow`
   operand enumeration; 2868/3281/3561 are `--stream`/`--graph`/`--tree` length
   decodes; 3285/3571/3838 are call-target/`is_call` probes.)
3. Port the frame-stack and indirect-call verification. The `--tree`/`--graph`
   engines verify a pending call at its resolution stop by checking `rsp ==
   call_site_rsp - 8` **and** `[rsp] == call_site + insn_len` (x86 pushes the
   return address). On AArch64 a `bl` writes the return address to **x30 (LR)**,
   not the stack, and the frame identity is `(entry_lr, sp)` — exactly the note
   [src/ptrace_backend.c](../../../src/ptrace_backend.c) records for its own
   shadow stack ("AArch64 frame identity is `(entry_lr, sp)` because `bl` writes
   x30, not the stack"). Add the AArch64 branch: read LR through the shim
   (`read_pc_ret`'s `lr` out-param on x86-64 is always 0; on AArch64 it is
   `regs[30]`), and verify the callee's entry LR equals `call_site + 4`.
4. Port the region engine's own breakpoint planting. `rgn_plant_bp` (2075) writes
   `int3`; `rgn_rewind_from_bp` (2100) rewinds `rip` from `base + 1`. On AArch64
   the software breakpoint is `brk #0` = `0xd4200000` and the stop-PC lands **AT**
   the instruction (no rewind) — mirror
   [src/ptrace_backend.c](../../../src/ptrace_backend.c)'s `PTRACE_BP_INSN` /
   `PTRACE_BP_LEN` split (lines 414-421). `rgn_hw_bp_arm` (2120) drives x86 DR0;
   its AArch64 form drives `NT_ARM_HW_BREAK` exactly as the library's `set_hw_bp`
   (lines 521-535) — control word `0x1e5` (E=1, PMC=0b10 EL0, BAS=0b1111).

**Code.** Mostly mechanical substitution plus one new `#if defined(__aarch64__)`
branch in the call-resolution path. Add `uint64_t *lr` plumbing to the shim's
register read so `--tree`/`--graph` can read x30. The region engine's `int3` vs
`brk` and `rip+1` vs `pc` split is two small `#ifdef`s.

**Tests.** T5's arm64 leg drives `--stream`/`--graph`/`--tree` against the
existing victims (`spy_victim` has `work → helper`, `tid_victim` has two
threads). Assertions to add for arm64: the `--graph --json` node/edge shape for
`spy_victim` names `work` and `helper` with a real edge between them (proves the
AArch64 `bl` attribution), and `--tree` shows `helper` nested under `work` (proves
the LR-based frame stack). A failure shows the edge missing or the depth wrong; a
pass matches the x86-64 shape.

**Docs.** Internal-only.

**Done when.**
- `grep -n 'ASMTEST_ARCH_X86_64' cli/asmspy_engine.c` returns only sites inside
  the watch engine (T6) or comments — the engine bodies use `ASMSPY_HOST_ARCH`.
- On the arm64 CI leg, `--graph --json` on `spy_victim` contains a `work →
  helper` edge and `--tree` nests `helper` under `work`.

### T4 — Syscalls engine + `--log` on AArch64  (M, depends on: T1, T2)

**Goal.** `asmspy --log` (the strace-ish syscall view — the *safe on any target*
view) decodes AArch64 syscalls: the number comes from `x8`, arguments from
`x0`-`x5`, and the name table matches the AArch64 syscall ABI.

**Steps.**
1. In the syscalls engine ([cli/asmspy_engine.c](../../../cli/asmspy_engine.c)
   `asmspy_engine_syscalls` at 1892), the syscall-number read at line 1965 is
   `ts->ent_nr = (long)regs.orig_rax`. On AArch64 the number is **`x8`** and there
   is no `orig_` shadow — at a syscall-stop the kernel leaves the number in
   `regs[8]` (readable via `NT_PRSTATUS`). Gate that read per-arch (x86-64:
   `orig_rax`; AArch64: `regs[8]`), inline with `#if defined(__aarch64__)` or via a
   small `asmspy_reg_syscall_nr(g)` helper.
2. **The argument registers already have a funnel — reuse it, do not add one.**
   `scarg(const struct user_regs_struct *e, int i)` at line 542 *is* the
   `asmspy_reg_arg(g, i)` accessor: on x86-64 it maps `i = 0..5` to
   `rdi/rsi/rdx/r10/r8/r9`. Gate **`scarg`'s body** per-arch (AArch64:
   `regs[0..5]`) rather than creating a duplicate accessor in the shim.
3. **`scarg` is not the whole port surface.** About two dozen `ap_*` decode
   helpers read the argument fields **directly** off the snapshot — `e->rdi` /
   `e->rsi` / `e->rdx` (and a few `e->r10`/`e->r8`/`e->r9`) — for example the
   read/write decode at lines 1105-1109 and `openat` at 1164-1168. `grep -n
   'e->rdi\|e->rsi\|e->rdx\|e->r10\|e->r8\|e->r9' cli/asmspy_engine.c` enumerates
   all ~25 sites. On AArch64 `struct user_regs_struct` has **no `rdi` member**, so
   every one of these is a compile error until it routes through `scarg(e, N)` (or
   an equivalent gated read). Converting them is the bulk of this task — not "two
   call-site substitutions".
4. The register snapshot `ts->entry` (declared `struct user_regs_struct entry` at
   line 1250, stored whole at line 1966 as `ts->entry = regs`) is handed to the
   decoder as the `const struct user_regs_struct *e` those `ap_*` helpers read.
   The struct **copy** compiles on both arches — AArch64's `user_regs_struct` is
   `regs[31]`/`sp`/`pc`/`pstate` — so keep `ts->entry` as the raw struct; it is the
   field reads off it (step 3), not the storage type, that must go through the arch
   gate.
5. The syscall-name include is generated per-host by
   [cli/gen-syscall-names.sh](../../../cli/gen-syscall-names.sh) from the
   compiling host's `<sys/syscall.h>` (`mk/cli.mk` line 39-42), so on an AArch64
   builder it already emits the AArch64 names keyed by AArch64 `__NR_*`. **Verify
   this**: build the include on arm64 and confirm it is non-empty and names differ
   from x86-64 (AArch64 has no `SYS_open`, only `SYS_openat`; `SYS_stat` is
   absent). No script change should be needed — record if one is.
6. The per-syscall argument-shape table (the `~40`-syscall decoder added
   2026-07-17, `ap_cloneflags` and friends around line 949) keys off `SYS_*`
   macros guarded by `#ifdef __NR_x`, so entries for syscalls absent on AArch64
   compile out cleanly. AArch64-only numbering differences (e.g. no legacy
   `open`) are handled by the `#ifdef` guards already present — **verify** no
   entry hard-codes an x86 number.

**Code.** One per-arch gate on the syscall-number read (step 1), one per-arch
body inside the **existing** `scarg` funnel (step 2), and the ~25 direct `e->r*`
field reads in the `ap_*` helpers rerouted through `scarg` (step 3) — that is the
real port surface, not "two substitutions", and `ts->entry` keeps its type
(step 4). The remaining work is verification that the existing `#ifdef __NR_*`
guards make the decoder table port for free — if a guard is missing, add it (do
not delete the entry).

**Tests.** T5's arm64 leg runs `--log` against `syscall_victim` (does file I/O).
Assert the arm64 `--log` output names the syscalls the victim makes (`openat`,
`write`, `nanosleep` — the AArch64 names) with an `= <retval>`. A failure shows
raw numbers or wrong names (the x86 table applied to arm64 numbers); a pass names
them correctly. This is the direct analog of the i386-refusal test's logic
(wrong table → confident nonsense), inverted: the right table names the right
calls.

**Docs.** Internal-only for the engine; the user-visible "AArch64 supported"
statement is T5.

**Done when.**
- On the arm64 CI leg, `--log <syscall_victim-pid>` names `openat`/`write` (not
  raw numbers, not x86 names).
- `$(BUILD)/asmspy_syscall_names.inc` built on arm64 is non-empty and contains
  AArch64 syscall names.

### T5 — Build gate, native arm64 CI leg, and user docs  (S, depends on: T1-T4)

**Goal.** `make cli` builds on AArch64 Linux instead of skipping; a native
`ubuntu-24.04-arm` CI leg runs `cli-smoke` for real; and the guide/changelog stop
saying "x86-64 only".

**Steps.**
1. In [mk/cli.mk](../../../mk/cli.mk), widen the arch gate. Today `cli` (line 72)
   and `cli-smoke` (line 332) skip unless `CLI_ARCH` is `x86_64`. Change the
   condition to allow `x86_64` **and** `aarch64`/`arm64`, keeping the skip for
   every other machine (32-bit ARM, riscv, etc.). Update the skip message —
   asmspy now supports two arches, so the message names the *unsupported* arch and
   no longer points at "the Theme F abstraction row" (that row is this work).
2. Add a native arm64 leg to the `cli` job in
   [.github/workflows/ci.yml](../../../.github/workflows/ci.yml) (line 557).
   Today it is `runs-on: ubuntu-latest`. Convert it to a matrix over
   `[ubuntu-latest, ubuntu-24.04-arm]` (the pattern lines 37/84/275/309 already
   use), so `cli-smoke` builds and runs on a **real** AArch64 VM. This is the
   correct validation vehicle: GitHub's `ubuntu-24.04-arm` runners are real VMs
   (not qemu) where ptrace tracer/tracee workloads pass — verified by strace's and
   CRIU's green aarch64 CI (see Research notes). The `gcc-multilib` step (for the
   i386 victim) is x86-only; guard it so the arm64 leg does not try to install it
   (there is no `-m32` on arm64 — the i386-refusal block already self-drops via
   `mk/cli.mk`'s parse-time `-m32` probe).
3. Update the user guide. [docs/guides/tracing/asmspy.md](../../guides/tracing/asmspy.md)
   says "**Linux x86-64 only.**" (line 30) and "x86-64 only (debug registers)"
   for `--watch` (lines 513, 638). Change the top statement to "Linux x86-64 and
   AArch64". Leave `--watch`'s wording to T6 (which lands the AArch64 watch arm).
4. Add a `CHANGELOG.md` entry under `## [Unreleased]` → `### Added`: asmspy now
   runs on AArch64 Linux (single-step engines + syscall log), validated on the
   `ubuntu-24.04-arm` CI runner.

**Code.** Makefile condition edit, a CI matrix edit, two docs edits. No C.

**Tests.** The arm64 CI leg IS the test for T1-T4 — it runs the full `cli_smoke.sh`
natively. A failure is a red arm64 job (build error, wrong decode, or a killed
victim); a pass is `SMOKE_RC=0` on both matrix legs. The docs build
(`make docker-docs`, `-W` fail-on-warning) must stay green after the guide edit.

**Docs.** This task IS the docs task: the guide's arch statement and the changelog
`Added` entry.

**Done when.**
- `make cli` on an AArch64 Linux host (or the arm64 CI leg) builds `build/asmspy`
  instead of printing `# SKIP cli`.
- The `cli` CI job is green on **both** `ubuntu-latest` and `ubuntu-24.04-arm`.
- `make docker-cli` on an x86-64 host still prints `PASS` (no regression).
- `make docker-docs` succeeds; the guide no longer says "x86-64 only" at the top.

### T6 — `NT_ARM_HW_WATCH` data-watchpoint engine (`asmspy --watch` on AArch64)  (M, depends on: T1)

**Goal.** Replace the `#else` stub of `asmspy_engine_watch` with a real AArch64
implementation over the `NT_ARM_HW_WATCH` ptrace regset (`DBGWCR`/`DBGWVR`),
mirroring the landed x86 DR0-3 engine and the library's `NT_ARM_HW_BREAK` path.

**Steps.**
1. Add an `#elif defined(__aarch64__)` arm to the `#if defined(__x86_64__)` block
   ([cli/asmspy_engine.c](../../../cli/asmspy_engine.c) line 3989); the existing
   `#else` stub (4293) stays as the non-x86/non-arm64 fallback.
2. Arm/disarm via `PTRACE_GETREGSET`/`PTRACE_SETREGSET` with `NT_ARM_HW_WATCH` and
   `struct user_hwdebug_state` — the *identical shape* the library uses for
   `NT_ARM_HW_BREAK` ([src/ptrace_backend.c](../../../src/ptrace_backend.c)
   521-535), only the note type differs. `struct user_hwdebug_state` is `{ __u32
   dbg_info; __u32 pad; struct { __u64 addr; __u32 ctrl; __u32 pad; }
   dbg_regs[16]; }` from `<asm/ptrace.h>` (already `#include`d by the aarch64
   hw-break path at 509).
3. Encode the `DBGWCR` control word. It is the low bits of the architectural
   register: `E` = bit 0, `PAC` (privilege) = bits 2:1, `LSC` (load/store control)
   = bits 4:3, `BAS` (byte-address-select) = bits 12:5. For an 8-byte write watch:
   `BAS = 0xff`, `LSC = 0b10` (store; `0b01` load, `0b11` both), `PAC = 0b10`
   (EL0/user), `E = 1` → `(0xff << 5) | (0b10 << 3) | (0b10 << 1) | 1 = 0x1FF5`.
   For `--rw`, `LSC = 0b11`. (The kernel recomputes `PAC` from the address, so the
   written value is effectively ignored and read-back shows EL0=2 — do not depend
   on the written PAC.)
4. Handle `DBGWVR` alignment and `BAS` for widths < 8. `DBGWVR` must hold the
   **8-byte-aligned** address; `BAS` bit *i* selects byte `DBGWVR + i`. For a
   `len`-byte watch at `addr`: `offset = addr & 7`, `DBGWVR = addr & ~7`, `BAS =
   ((1u << len) - 1) << offset` — and the window **may not cross the 8-byte
   boundary** (reject `offset + len > 8` with `ASMTEST_PTRACE_EINVAL`, the AArch64
   analog of the x86 length-alignment reject at 4136). So a 4-byte watch at
   `base+4` is `DBGWVR=base, BAS=0xf0`; a 4-byte watch at `base+6` is rejected.
5. Per-thread arming: reuse the x86 engine's `seize_threads(pid,
   PTRACE_O_TRACECLONE, …)` + arm-on-first-stop + `PTRACE_O_TRACECLONE`
   new-thread arming (debug registers are per-thread on AArch64 too), and its
   `watch_teardown` (disarm every slot, then detach — the survival step). None of
   that structure changes; only `watch_arm`/`watch_disarm`/`watch_dr7_word` gain
   AArch64 bodies (`watch_dr7_word` → `watch_dbgwcr_word`).
6. Hit detection and labeling. A watchpoint hit delivers `SIGTRAP` with `si_code
   TRAP_HWBKPT` and `si_addr` = the trigger address. Read the value with
   `process_vm_readv` through the leader `pid` (as x86 does). For read/write
   labeling, `LSC = 0b10` (write-only) is self-labeling like the x86 write-only
   arm; for `--rw`, decode the faulting instruction via `asmtest_operands(
   ASMSPY_HOST_ARCH, …)` (T3's arch parameter) — the AArch64 access instruction is
   at the PC, not one back (AArch64 has no variable-length insn window like x86's
   `watch_decode_dir`; the faulting PC *is* the access, or one instruction's `pc`,
   simpler than x86).
7. Self-skip on zero slots. `dbg_info & 0xff` is the watchpoint-slot count; it is
   **0 under qemu-user** (and possibly on a hypervisor that withholds debug
   registers — see Research notes). When it is 0, return `ASMSPY_WATCH_UNAVAIL`,
   the same clean skip the x86 arm returns when `PTRACE_POKEUSER` is refused.

**Code.** New `#elif defined(__aarch64__)` block of ~150 lines paralleling the
x86 block, sharing `seize_threads`/`detach_threads`/`watch_teardown`/the main
loop verbatim where possible. The only genuinely new logic is `watch_dbgwcr_word`
+ the `DBGWVR`/`BAS` alignment split (step 4) and the `GETREGSET`/`SETREGSET`
arm/disarm.

**Tests.** T7 owns the arm-and-fire CI probe (the debug-register half is not
guaranteed to fire on hosted runners). At the unit level, add a pure test of the
`DBGWCR`/`BAS` encoder to `cli/test_arch.c` (T1): assert `watch_dbgwcr_word(write,
8) == 0x1FF5`, that a 4-byte watch at `base+4` yields `BAS=0xf0`/`DBGWVR=base`,
and that `base+6, len=4` is rejected. This runs on **every** host (no hardware),
so the encoding is covered even where no watchpoint can fire — the same
"pure-module carries the burden, live leg covers the wiring" discipline the
`asmspy_autoregion.h` split uses.

**Docs.** Update [docs/guides/tracing/asmspy.md](../../guides/tracing/asmspy.md)
`--watch` wording (lines 513, 638): "x86-64 only (debug registers)" becomes
"x86-64 (DR0-3) and AArch64 (`NT_ARM_HW_WATCH`); self-skips where the host exposes
no watchpoint slots (qemu-user, some hypervisors)". Add a `CHANGELOG.md`
`### Added` line for the AArch64 `--watch` arm.

**Done when.**
- `grep -rn 'NT_ARM_HW_WATCH\|DBGWCR' cli/` returns the new engine (today: 0
  hits across `src/ cli/ include/ mk/`).
- On x86-64, `make docker-cli` still prints `PASS` (the AArch64 arm is behind
  `#elif`, so x86 is untouched).
- `cli/test_arch.c`'s `DBGWCR`/`BAS` assertions pass on every host.
- The lane self-skips cleanly (`# SKIP --watch`, exit 0) under qemu-user and where
  `dbg_info & 0xff == 0`.

### T7 — Watchpoint arm-and-fire CI probe  (S, depends on: T6)

**Goal.** Settle whether AArch64 hardware watchpoints actually **fire** on the
CI runner (not merely report nonzero slots), and gate honestly on the answer.

**Steps.**
1. In `cli_smoke.sh`'s `--watch` block (currently around line 1590, x86-only
   today), make the AArch64 leg assert a **delivered** hit, not just that the arm
   succeeded. Reuse `watch_victim` (a worker thread writes a known sentinel
   `0xd15ea5eddeadbeef` to a published field) — the existing x86 assertions
   (value captured, labeled a write, from the worker tid, PC resolved) are the
   same shape.
2. The decisive check: assert the smoke observes at least one hit whose value
   equals the sentinel. If the arm succeeds (`dbg_info & 0xff > 0`) but **no hit
   is ever delivered**, that is the "accepted-but-never-firing" hypervisor mode
   (documented on WSL2 x86; unverified for-or-against on GitHub's arm64 runners —
   see Research notes) — treat a timeout with zero hits as a **skip with a named
   reason** (`# SKIP --watch: watchpoint armed but never fired — host may not
   route debug exceptions`), not a failure, because it is a host property, not an
   asmspy bug. The `timeout 30` guard already present catches the hang.
3. Do **not** assert nonzero `dbg_info` as sufficiency anywhere — the whole point
   of this probe is that reporting slots and firing are different questions on a
   virtualized host.

**Code.** Shell only — extend the existing `--watch` smoke block with the arm64
path and the "armed-but-never-fired → named skip" branch.

**Tests.** This task *is* a test. On x86-64 nothing changes (the block already
asserts firing). On the arm64 CI leg the outcome is one of: hits delivered →
`PASS` with the value assertion; slots absent → clean `# SKIP` (qemu/hypervisor);
armed-but-silent → clean `# SKIP` with the specific reason. All three exit 0; only
a *wrong value* or a *killed victim* is a failure.

**Docs.** Fold the outcome into the `CHANGELOG.md` line from T6 (whether AArch64
`--watch` fires on the hosted runner is worth recording once the CI run settles
it). Internal note only until then.

**Done when.**
- The arm64 `cli` CI job is green, taking exactly one of {hit-asserted,
  slots-absent skip, armed-but-silent skip} — recorded in the run log.
- A never-firing host produces a **named** skip, not a hang or a false pass.

## Task order & parallelism

```
T1 (shim) ──┬── T2 (single-step/detach)  ─┐
            ├── T3 (disas arch + frames)  ─┼── T5 (build gate + arm64 CI + docs)
            ├── T4 (syscalls/--log)  ──────┘
            └── T6 (NT_ARM_HW_WATCH) ── T7 (arm-and-fire probe)
```

- **T1 is the critical-path foundation** — every other task consumes the shim.
  Do it first, alone.
- **T2, T3, T4, and T6 are independent of each other** once T1 lands (two people
  can take the single-step-engine track T2/T3/T4 and the watchpoint track T6/T7
  concurrently).
- **T5 is the join** for the single-step track: it needs T2-T4 in place so the
  arm64 CI leg is green rather than red. T6/T7 can land before or after T5 — the
  watch engine is behind its own `#elif` and does not block the build gate.
- Critical path: **T1 → (T3 or T4) → T5**. The watch track (T1 → T6 → T7) is
  shorter and fully parallel.

## Constraints & gates

- **Real hardware is NOT a self-skip gate for the single-step half.** The bundle's
  premise ("qemu-user does not emulate the ptrace tracer/tracee relationship")
  is true, but GitHub's `ubuntu-24.04-arm` runners are **real VMs**, not qemu, and
  full ptrace tracer/tracee workloads pass there (strace, CRIU — see Research
  notes). Per [CLAUDE.md](../../../CLAUDE.md)'s dependency rule, a lane that *can*
  test a feature must, so T5 adds the native arm64 CI leg rather than leaving the
  single-step engines to self-skip. The **docker `linux/arm64`** path (qemu binfmt
  on an x86 host) still self-skips — `asmtest_ptrace_available()` returns 0 under
  emulation — and that is correct; it is not the validation vehicle.
- **The watchpoint-firing half IS an honest gate — but a probeable one.** Whether
  an armed `NT_ARM_HW_WATCH` slot actually delivers a `SIGTRAP` on the hosted
  hypervisor is undetermined by public evidence (T7 settles it in-repo). Record
  the CI outcome; if the runner never fires, that is a host fact to note, not an
  asmspy defect — the engine still works on bare-metal AArch64.
- **No pinned external dependency is added.** The AArch64 support is pure source
  behind `#if defined(__aarch64__)`; `struct user_hwdebug_state` and
  `NT_ARM_HW_WATCH` come from the kernel headers already present in the toolchain.
  Capstone (already pinned 5.0.1) provides the ARM64 decode.
- **Formatting.** Run `make fmt` (clang-format, CI-gated via `fmt-check`) before
  committing.

## Research notes (verified 2026-07-17)

Externally verified against upstream Linux master (fetched 2026-07-17 via
`raw.githubusercontent.com/torvalds/linux`; line numbers are from master and will
drift). Sources inline.

- **`NT_PRSTATUS` GPR layout (T1).** `PTRACE_GETREGSET(NT_PRSTATUS)` on AArch64
  uses `struct user_pt_regs { __u64 regs[31]; __u64 sp; __u64 pc; __u64 pstate;
  }` — 272 bytes. Byte offsets: `x0` (return value) 0, `x30`/LR 240, `sp` 248,
  `pc` 256, `pstate` 264. `NT_PRSTATUS = 1`.
  ([arch/arm64/include/uapi/asm/ptrace.h](https://github.com/torvalds/linux/blob/master/arch/arm64/include/uapi/asm/ptrace.h);
  [include/uapi/linux/elf.h](https://github.com/torvalds/linux/blob/master/include/uapi/linux/elf.h))
- **Single-step is kernel-owned (T2).** `PTRACE_SINGLESTEP` is supported;
  `user_enable_single_step` sets `TIF_SINGLESTEP` + `DBG_SPSR_SS`,
  `user_disable_single_step` clears the `TIF` flag. `MDSCR_EL1.SS` is bit 0; the
  kernel sets/clears it on user exit/entry keyed on `TIF_SINGLESTEP`. A tracer
  **cannot** set/clear the step bit via a `pstate` write —
  `user_regs_reset_single_step` forces `pstate` bit 21 to mirror `TIF_SINGLESTEP`
  in `valid_user_regs`. **`PSR_SS_BIT` is not a Linux uapi macro** (absent from
  uapi headers on master, v6.12, v5.10, v5.4, v4.19) — do not `#include`-hunt for
  it; the tracer never needs the constant because it drives stepping through
  `PTRACE_SINGLESTEP`, not a register write.
  ([debug-monitors.c](https://github.com/torvalds/linux/blob/master/arch/arm64/kernel/debug-monitors.c),
  [ptrace.c](https://github.com/torvalds/linux/blob/master/arch/arm64/kernel/ptrace.c))
- **`NT_ARM_HW_WATCH` ABI (T6).** `NT_ARM_HW_WATCH = 0x403`; `struct
  user_hwdebug_state { __u32 dbg_info; __u32 pad; struct { __u64 addr; __u32
  ctrl; __u32 pad; } dbg_regs[16]; }`; `dbg_info & 0xff` is the slot count (max
  16); a hit delivers `SIGTRAP` `si_code TRAP_HWBKPT` with `si_addr` = trigger
  address.
  ([ptrace.c](https://github.com/torvalds/linux/blob/master/arch/arm64/kernel/ptrace.c),
  [hw_breakpoint.h](https://github.com/torvalds/linux/blob/master/arch/arm64/include/asm/hw_breakpoint.h))
- **`DBGWCR` ctrl-word encoding (T6).** `E` bit 0, `PAC` bits 2:1, `LSC` bits 4:3,
  `BAS` bits 12:5 (`encode_ctrl_reg = (len<<5)|(type<<3)|(privilege<<1)|enabled`).
  `LSC`: load=1, store=2, both=3. `BAS`: `LEN_1=0x1`, `LEN_2=0x3`, `LEN_4=0xf`,
  `LEN_8=0xff`. Privilege EL0=2. The kernel **recomputes** PAC from the address
  (EL0 for user addresses), so the written PAC is effectively ignored and
  read-back shows EL0=2. Corroborated by the KVM selftest's numeric shifts
  (`DBGWCR_LEN8 = 0xff<<5`, `WR = 2<<3`, `E = 1<<0`).
  ([hw_breakpoint.h](https://github.com/torvalds/linux/blob/master/arch/arm64/include/asm/hw_breakpoint.h),
  [debug-exceptions.c selftest](https://github.com/torvalds/linux/blob/master/tools/testing/selftests/kvm/arm64/debug-exceptions.c))
- **`DBGWVR`/`BAS` alignment (T6, step 4).** The kernel decodes `offset =
  __ffs(BAS)`, watch address = `DBGWVR + offset`; the reverse rejects `(BAS <<
  offset) > 0xff`, then `address &= ~7` and `ctrl.len <<= offset`. So `DBGWVR`
  holds the 8-byte-aligned address, `BAS` bit *i* selects byte `DBGWVR + i`, and a
  window may not cross the 8-byte boundary. Watch alignment mask is `0x7`.
  ([hw_breakpoint.c](https://github.com/torvalds/linux/blob/master/arch/arm64/kernel/hw_breakpoint.c))
- **The arm64 CI runner is real, and ptrace works there (T5).** GitHub's
  `ubuntu-24.04-arm` standard runners are 4-vCPU Azure Cobalt 100 (Neoverse N2)
  **VMs**, GA for public repos 2025-08-07 and available for private repos since
  2026-01-29, with passwordless sudo. Full ptrace tracer/tracee suites pass on
  these exact runners: strace's `make check` (all `PTRACE_TRACEME`/`PTRACE_SYSCALL`
  pairs) is green on `ubuntu-24.04-arm`, and CRIU's `PTRACE_SEIZE` jobs pass. So
  the single-step half of AArch64 asmspy is **CI-closable today**.
  ([strace ci.yml](https://github.com/strace/strace/blob/master/.github/workflows/ci.yml),
  [strace green run](https://github.com/strace/strace/actions/runs/29562526806),
  [CRIU run](https://github.com/checkpoint-restore/criu/actions/runs/23529675698),
  [runner hardware discussion](https://github.com/orgs/community/discussions/148648))
- **Watchpoint firing on the hosted hypervisor is undetermined (T7).** No public
  evidence either way that these runners deliver debug-register exceptions. The
  guest will very likely **report** nonzero slots (`dbg_info` comes from
  `ID_AA64DFR0_EL1` via the kernel's `hw_breakpoint` driver), but whether an armed
  watchpoint **fires** depends on the hypervisor context-switching debug registers
  and routing debug exceptions — which cannot be assumed: the hypervisor
  demonstrably withholds PMU hardware (perf collects zero samples on arm64
  runners), and there is documented precedent for the accepted-but-never-firing
  mode (WSL2 x86 gdb hardware watchpoints, microsoft/WSL#5741). **T7's probe must
  assert a delivered `SIGTRAP`, not just nonzero `dbg_info`** — matching the
  self-probe design in
  [2026-07-15-hw-watchpoint-spike.md](../analysis/2026-07-15-hw-watchpoint-spike.md).
  ([perf-on-arm64 issue](https://github.com/actions/runner-images/issues/11689),
  [WSL#5741](https://github.com/microsoft/WSL/issues/5741))

## Out of scope

- **Validating the *library's* single-step *stream* on real AArch64 silicon** —
  the `asmtest_ptrace_*` stepper in
  [src/ptrace_backend.c](../../../src/ptrace_backend.c) whose live capture is "the
  one genuinely remaining Phase-5 front" in
  [zen2-singlestep-trace-plan.md](../archive/plans/zen2-singlestep-trace-plan.md) — belongs
  to [aarch64-ptrace-single-step-validation.md](aarch64-ptrace-single-step-validation.md).
  This doc consumes that stepper (the region engine calls
  `asmtest_ptrace_trace_attached_ex`); it does not re-validate it.
- **CI runner provisioning / self-hosted bare-metal AArch64** belongs to
  [self-hosted-ci-runners.md](self-hosted-ci-runners.md). T5 only joins the
  already-available hosted `ubuntu-24.04-arm` matrix.
- **AArch64 SVE/vector value capture** belongs to
  [aarch64-sve-capture.md](aarch64-sve-capture.md).
- **Block-step (`PTRACE_SINGLEBLOCK`) tracer correctness** belongs to
  [ptrace-blockstep-tracer-correctness.md](ptrace-blockstep-tracer-correctness.md).
- **macOS / Apple-silicon (`mach_vm_read`/`thread_get_state`) and Windows-on-ARM**
  are separate ports —
  [macos-oop-mach-stepper.md](macos-oop-mach-stepper.md) and the macOS cleanroom
  docs own those; this doc is Linux/AArch64 only.
- **The F3 x86 `--watch` engine and its spike** are already landed (2026-07-15) —
  this doc adds only the AArch64 analog; the x86 arm is unchanged.
