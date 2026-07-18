# ptrace block-step reconstructor and tracer correctness, plus IBS pre-cover integration — implementation

> **Sources.** Actioned from
> [2026-07-17-blockstep-reconstruction-defects.md](../analysis/2026-07-17-blockstep-reconstruction-defects.md),
> [amd-tracing-plan.md](../plans/amd-tracing-plan.md) (Part III Phase 2 and the
> Phase 7 cross-reference correction),
> [2026-07-02-code-review.md](../analysis/2026-07-02-code-review.md) (finding #19),
> [2026-07-03-call-descent-review.md](../analysis/2026-07-03-call-descent-review.md)
> (the "Deferred" list), [asmspy-plan.md](../plans/asmspy-plan.md) (Theme C), and
> [zen2-ibs-tracing-plan.md](../archive/plans/zen2-ibs-tracing-plan.md). Written
> 2026-07-17. If this doc and a source disagree, this doc wins (sources may be
> stale); if the CODE and this doc disagree, re-verify before implementing.
> Line numbers below were re-verified against the working tree on 2026-07-17 and
> supersede the (drifted) numbers quoted in the source analyses.

## Why this work exists

The ptrace tier's block-step reconstructors silently record instructions that never
executed when the traced code contains an application `int3` (a JVM safepoint poll, a
.NET breakpoint — the tier's stated managed-runtime target), and silently under-record
`rep`-prefixed string ops. The level-0 call-out step-over resumes in the wrong
invocation when a stepped-over helper re-enters the traced region. Fixing these makes
"the trace is byte-identical to per-instruction stepping" an honest promise instead of a
fixture-shaped one. On top of that correctness base, this doc wires the already-shipped
statistical IBS covered-block set into the block-step fallback so its per-stop decode
work shrinks on the hosts (Zen 2) where block-step is the only exact capture.

## What already exists (verified 2026-07-17)

- [src/ptrace_backend.c](../../../src/ptrace_backend.c) (3330 lines) — the whole
  out-of-process stepper tier. Key internals, at current line numbers:
  - `run_until` (:678) — the call-out step-over primitive: plants an `int3` (or a
    hardware breakpoint on W^X text), `PTRACE_CONT`s, and resumes at the **first**
    arrival at the target (`hit != target → continue`, :757). No SP check.
  - `classify_region_exit` (:800) — decides return vs call-out; shared by every
    region driver, calls `run_until`.
  - descent (`descend_core`, :1034) — the shadow-stack step loop; its pop predicate
    `sp == f->sp_ret && pc == f->ret_addr && f->last_was_ret` is at :1176.
    `descend_decide` re-parses `/proc/<pid>/maps` per L3 call-out via
    `asmtest_proc_region_by_addr` (:969; parser at :49-79). The L3 watchdog is a
    process-global `ITIMER_REAL` + file-scope `descend_alarm_fired` (:977-1014).
  - `blockstep_reconstruct` (:1695) with `bs_scan_terminator` (:1623) and
    `bs_record_run` (:1669) — the Capstone-only static reconstructor; the
    same-target-conditional ambiguity rule (BS_AMBIGUOUS → truncated) landed in
    `ee696e0` and IS in the tree.
  - The `WSTOPSIG(status) != SIGTRAP` gates — the file-wide "any SIGTRAP is ours"
    assumption: :734 (`run_until`), :1130 (descent), :1381 (per-insn `trace_call`),
    :1771 (region block-step), :1947 (attached block-step), :2112
    (`trace_attached_window_loop`), :2455 (`at_mode` in the windowed block-step),
    :2670 (`trace_window_call`), :2819 (`trace_attached_impl`). **Zero** hits for
    `PTRACE_GETSIGINFO`/`si_code` in this file.
  - `window_block_walk` (:2343) already has a signal-cut mode (`at_mode`, :2455) and
    a per-instruction handoff (:2489-2497) — the shape T1 reuses.
- [src/disasm.c](../../../src/disasm.c) — `asmtest_disas_is_branch` (:225-260) tests
  only JUMP/CALL/RET/IRET groups; no `rep`-prefix query exists anywhere
  (`grep -iE 'rep.?prefix|X86_PREFIX_REP|is_rep' src/ptrace_backend.c src/disasm.c`
  → 0 hits).
- [include/asmtest_ptrace.h](../../../include/asmtest_ptrace.h) — the "reconstructs
  the IDENTICAL per-instruction stream" promises: :100-107 (region block-step),
  :117-124 (attached block-step), :152-176 (windowed block-step). The documented
  "resumes at the FIRST arrival … NOT re-entrancy aware" step-over caveat: :210-217.
- The in-tree si_code precedent (the shape T1/T2 copy):
  [src/dataflow_ptrace.c](../../../src/dataflow_ptrace.c) `dfp_sigtrap_is_app`
  (:788-807) and its Inc5 handling (:920-946);
  [cli/asmspy_engine.c](../../../cli/asmspy_engine.c) `sigtrap_is_app` (:1858-1878),
  the "deliver via `PTRACE_CONT`, never `SINGLESTEP`" rule (:1880-1885, MEASURED
  fatal otherwise), and the "test OURS before `sigtrap_is_app`" ordering (:2387-2389).
- IBS: `asmtest_ibs_normalize_blocks`
  ([src/ibs_backend.c:160](../../../src/ibs_backend.c),
  [include/asmtest_ibs.h:235-278](../../../include/asmtest_ibs.h)) ships and is
  unit-tested ([examples/test_ibs.c](../../../examples/test_ibs.c):123-155, :392),
  but has **zero** consumers in the cascade: `grep -i ibs src/trace_auto.c` → one
  comment at :56; zero hits in `src/ptrace_backend.c`. The block-step rung of
  `asmtest_trace_call_auto` is at [src/trace_auto.c:279-302](../../../src/trace_auto.c);
  policy bits `0x1`/`0x2` at
  [include/asmtest_trace_auto.h:136-141](../../../include/asmtest_trace_auto.h).
  `ibs_backend.o` is already in `HWTRACE_OBJS`
  ([mk/native-trace.mk:2047-2054](../../../mk/native-trace.mk)), so no link changes.
- Tests/harnesses to extend: [examples/test_hwtrace.c](../../../examples/test_hwtrace.c)
  `test_ptrace_blockstep` (:3923), `test_ptrace_attach_blockstep` (:5566), the WDRV
  windowed fixture + `wdrv_run` (:4253, :4325), the WSIG signal-leg fixture (:4356),
  `test_descent_attach` (:6949); [examples/jit_trace.c](../../../examples/jit_trace.c)
  (descend-suffix parse :780-798, vacuous L3 CHECK :384-388).
- Make targets that already pass: `make check` (framework self-tests),
  `make hwtrace-test` ([mk/native-trace.mk:2127](../../../mk/native-trace.mk)),
  `make ibs-test` (:2104). Docker lanes: `make docker-hwtrace`
  ([mk/docker.mk](../../../mk/docker.mk):442), `make docker-hwtrace-amd` (:542),
  `make docker-hwtrace-ibs` (:567), `make docker-hwtrace-privileged`.

**Prove the baseline before touching anything** (this repo prefers Docker lanes —
CLAUDE.md rule; `make help` lists everything):

```
make docker-hwtrace        # expect "== hwtrace-test ==" then "N/N passed" (341/341-ish), exit 0
make docker-hwtrace-ibs    # pure IBS decoder checks pass everywhere; live capture self-skips off AMD
make check                 # framework self-tests all PASS
```

## Tasks

### T1 — Classify application `int3` SIGTRAPs in the three block-step reconstruction drivers  (M, depends on: none)

**Goal.** An application `int3` inside a block-stepped region never again produces
never-executed instructions with `truncated == false` — in the region, attached, and
windowed block-step drivers, proven by a differential fixture.

**Steps.**
1. In `src/ptrace_backend.c` (inside the `#if defined(__x86_64__)` block-step
   section, near `wait_stop_sigtrap` :1495), add a file-static helper mirroring
   `dfp_sigtrap_is_app` (`src/dataflow_ptrace.c:788-807`) **verbatim in shape**:

   ```c
   /* Mirrors dfp_sigtrap_is_app / asmspy's sigtrap_is_app: a SIGTRAP the tracee
    * raised ITSELF (executed int3 -> SI_KERNEL; its own hw breakpoint ->
    * TRAP_HWBKPT). TRAP_TRACE and TRAP_BRKPT are OUR step/#DB completing
    * (TRAP_BRKPT = a step across a syscall, MEASURED — see cli/asmspy_engine.c),
    * NOT an app breakpoint. GETSIGINFO failure => ours (the pre-fix behaviour). */
   static int bs_sigtrap_is_app(pid_t pid);
   ```

   Do **not** use a naive `TRAP_BRKPT` split — that is the refuted shape (see
   Constraints).
2. Add a file-static "cut" recorder beside `blockstep_reconstruct` (:1695):
   `blockstep_reconstruct_cut(code, len, from_off, cut_off, stream, pn, last_off)`
   — record the straight-line run `[from_off, cut_off)` by summing
   `asmtest_disas` lengths, requiring the walk to land **exactly** on `cut_off`
   (an app `int3` trap-stop's PC is one past the `0xCC` byte, so the executed run
   ends exactly there); any overshoot/undecodable byte returns BS_FAIL. This is the
   region-buffer analog of `window_block_walk`'s `at_mode` (:2349-2355).
3. Region driver `asmtest_ptrace_trace_call_blockstep` (loop at :1756): at the
   SIGTRAP path, before treating the stop as a BTF `#DB`, call `bs_sigtrap_is_app`.
   On app-trap: if a block is open and in-region, run the cut recorder for
   `[prev_off, pc - base_ip)`; set `overflow = 1` (honest truncation — BTF cannot
   bridge the kernel-injected transfer into the handler); set
   `pending_sig = SIGTRAP` **but deliver it via `PTRACE_CONT`**, not the loop's
   `PTRACE_SINGLEBLOCK` (see Constraints — SINGLESTEP/SINGLEBLOCK+SIGTRAP is the
   measured-fatal pattern). After the CONT: if the child dies (no handler — the
   fixture case) the existing `WIFEXITED/WIFSIGNALED` break path runs; if it stops
   again (a handler ran), resume block-stepping in resync mode
   (`prev_off = SENTINEL`) so out-of-region handler code is skipped at native block
   speed and the next in-region stop opens a fresh block.
4. Attached driver `asmtest_ptrace_trace_attached_blockstep` (loop at :1934): on
   app-trap, cut-record the open block, set `overflow = 1`, and **break, leaving
   the foreign target in its SIGTRAP signal-delivery stop** — the target is never
   killed and the caller owns signal policy (the exact stance the non-SIGTRAP gate
   at :1947 already takes). Extend the header comment at
   `include/asmtest_ptrace.h:117-124`: the caller that wants the target's own
   breakpoint semantics to proceed must detach with
   `PTRACE_DETACH(pid, 0, SIGTRAP)`.
5. Windowed driver: in `asmtest_ptrace_trace_attached_windowed_blockstep`, the
   `at_mode = (sig != SIGTRAP)` computation (:2455) becomes
   `at_mode = (sig != SIGTRAP) || bs_sigtrap_is_app(pid)`. For the app-SIGTRAP
   case the cut walk must be **exclusive of the stop PC** (the instruction at `pc`
   has not executed; the `int3` at `pc-1` has) — pass that through to
   `window_block_walk` (a flag or `to_pc` adjustment), whereas the existing
   non-SIGTRAP delivery-stop path keeps its shipped inclusive behaviour (the WSIG
   oracle pins it). The existing per-instruction handoff (:2489-2497) then owns the
   remainder — but its first resume must deliver the SIGTRAP via `PTRACE_CONT`,
   which is T2 step 3's job; land T1 step 5 and T2 step 3 in the same commit.
6. `make fmt && make docker-hwtrace` after each step.

**Code.** All in `src/ptrace_backend.c` + one comment block in
`include/asmtest_ptrace.h`. Quote to anchor step 5 — the shipped line :2455 is:

```c
        int sig = WSTOPSIG(status);
        int at_mode = (sig != SIGTRAP);
```

**Tests.** Extend `test_ptrace_blockstep` and `test_ptrace_attach_blockstep` in
`examples/test_hwtrace.c` with the repro fixture from the defects analysis:

```
53 48 89 fb CC 48 89 d8 48 01 c0 5b c3
  push rbx; mov rbx,rdi; int3; mov rax,rbx; add rax,rax; pop rbx; ret
```

Assert, for the region driver: `truncated == 1` and the offset stream is exactly
`+0 +1 +4` (through the `int3`, nothing after) — before the fix the shipped stream
is `+0 +1 +4 +5 +8 +11 +12 +5 +8 +11 +12` with `truncated == 0` (reproduced in the
analysis), so the new CHECKs fail red on unfixed code. For the attached driver:
same prefix + the target is still stopped (waitpid(WNOHANG) shows a stop, not an
exit) and a `PTRACE_GETSIGINFO` on it reads `SI_KERNEL`. For the windowed driver:
add an `int3` variant of the WDRV/WSIG differential (`wdrv_run`-style, block-step vs
per-instruction) asserting both entries produce the identical stream and both are
truncated. All of these need only ptrace-of-own-child: they run in
`make docker-hwtrace` and self-skip via the existing
`asmtest_ptrace_blockstep_available()` gate on aarch64/BTF-masked hosts.

**Docs.** `CHANGELOG.md` under `## [Unreleased]` / `Fixed`: one entry naming the
misclassification and the honest-truncation outcome. Update the three block-step
contract comments in `include/asmtest_ptrace.h` (:100-107, :117-124, :152-176) to
state the app-breakpoint behaviour. Internal: add a "Status: FIXED (T1, this doc)"
line for Defect 1 at the top of
`docs/internal/analysis/2026-07-17-blockstep-reconstruction-defects.md`.

**Done when.**
- The new int3 CHECKs pass in `make docker-hwtrace` (grep the output for the new
  check names; total goes up, 0 failed).
- Reverting only the driver changes makes exactly the new CHECKs fail (red test
  proven once, locally).
- `make docker-hwtrace-amd` on a Zen dev box: all green (the AMD lane runs the same
  harness with `--privileged`).
- On an aarch64 host the block-step tests still print their existing
  `# SKIP ptrace block-step:` line — no new unconditional x86 assumptions.

### T2 — Forward application SIGTRAPs in the per-instruction loops and `run_until`  (M, depends on: T1 (the helper))

**Goal.** No per-instruction ptrace loop in `src/ptrace_backend.c` swallows an
application's own SIGTRAP any more; each either delivers it (owned tracee, via
`PTRACE_CONT`) or ends honestly with the target left at the trap stop (foreign).

**Steps.** Make the `bs_sigtrap_is_app` helper available outside the x86-64
block-step section (move it next to `ptrace_read_mem` (:311); on aarch64 the
`SI_KERNEL`/`TRAP_HWBKPT` whitelist is equally valid). Then, gate by gate:
1. `run_until` (:734/:757): compute `hit` **first**; `hit == target` stays ours
   (a planted `int3` also reports `SI_KERNEL` — the asmspy :2387 ordering).
   Otherwise, if `bs_sigtrap_is_app(pid)`, set `sig = SIGTRAP` so the next
   `PTRACE_CONT` forwards it (the existing unrelated-signal forwarding pattern at
   :737-740); else keep the current silent `continue`.
2. Per-insn region driver `trace_call` loop (:1381): on app-trap at stop `pc`
   (which was recorded pre-execution at the previous stop — nothing to back out),
   set `overflow = 1`, deliver via `PTRACE_CONT(pid, 0, SIGTRAP)`, then reuse the
   existing returned-path reap dance (:1448-1452): if the child stops again,
   kill+reap; if it exited, done. Never `PTRACE_SINGLESTEP` with SIGTRAP.
3. `trace_attached_window_loop` (:2112): on app-trap, set `overflow`, and finish
   the window at native speed: extend `run_until` with a first-signal parameter
   (`run_until_sig(pid, target, first_sig)`; existing callers pass 0) and call it
   with `win_ret` + SIGTRAP for the inline form (`stop == NULL`), recovering
   `*result` at the window end and leaving the target stopped there. For the
   stop-flag form (`win_ret == 0`), deliver via `PTRACE_CONT(…, SIGTRAP)` and
   return truncated — there is no window-end address to run to.
4. `trace_window_call` (:2670) — owned tracee: same policy as step 2.
5. `trace_attached_impl` (:2819) and descent (`descend_core`, :1130) — foreign or
   caller-policy paths: mirror T1 step 4 — truncate (`overflow`/
   `asmtest_descent_mark_truncated`) and break with the target left in the
   signal-delivery stop; for descent's **fork-owned** path (`!c->forward_faults`
   is the existing discriminator, :1132) deliver via `PTRACE_CONT` + reap like
   step 2.

**Code.** `src/ptrace_backend.c` only. The one signature change is internal
(`run_until` → `run_until_sig` + a 2-arg wrapper keeping every existing call site
textually unchanged).

**Tests.** In `examples/test_hwtrace.c`: (a) per-insn region driver over the T1
fixture — assert `truncated == 1`, stream `+0 +1 +4`, and the child **died of
SIGTRAP** (`WIFSIGNALED && WTERMSIG == SIGTRAP` observed via the driver's rc/status
path — add the assertion hook the fixture needs, e.g. run the fixture and assert
result was never written); the pre-fix behaviour (stream n=7, `trunc=0`, result 42,
signal suppressed) is exactly what must disappear. (b) A handler variant: a fixture
whose child installs a SIGTRAP handler that increments a shared-memory counter —
assert the counter is 1 after the trace (the signal was DELIVERED, the
managed-runtime-safety point of the whole exercise). (c) Windowed: an int3 inside
the WDRV frame — assert `*result` still arrives (the `run_until_sig(win_ret)`
completion) and `truncated == 1`. All run under `make docker-hwtrace`.

**Docs.** `CHANGELOG.md` `Fixed` (can share T1's entry). Update the
`asmtest_ptrace_trace_attached` header note (:197-217) — signal policy wording only
here; the SP-aware rewrite of :210-217 belongs to T4.

**Done when.**
- New CHECKs green in `make docker-hwtrace`; harness totals up, 0 failed.
- `grep -nE "SINGLE(STEP|BLOCK), pid" src/ptrace_backend.c` shows no site that can
  inject SIGTRAP — the widened pattern also catches the `PTRACE_SINGLEBLOCK`
  signal-delivery sites (e.g. :1757, the exact site T1 step 3 modifies) that a bare
  `SINGLESTEP, pid` grep misses (manual audit recorded in the PR/commit message).
- `make docker-hwtrace-jit-dotnet` and `make docker-hwtrace-jit-java` still pass
  (live CoreCLR/HotSpot methods — the tier's real consumers — are unharmed).

### T3 — `rep`-prefix honesty in the block-step reconstructors  (S, depends on: none)

**Goal.** A `rep`-prefixed string op in a block-stepped region sets `truncated`
(honest degradation) instead of silently diverging from the per-instruction stream,
and the header promise is bounded accordingly.

**Steps.**
1. `src/disasm.c`: add `asmtest_disas_is_rep_string(arch, code, code_len, off)`
   mirroring `asmtest_disas_is_branch`'s shape (:225-260): Capstone detail mode,
   return 1 iff the decoded x86 instruction carries a `REP`/`REPE`/`REPNE` prefix
   (`insn->detail->x86.prefix[0]` ∈ {`X86_PREFIX_REP`, `X86_PREFIX_REPE`,
   `X86_PREFIX_REPNE`}); 0 otherwise / without Capstone / non-x86. Declare it in
   [include/asmtest_trace.h](../../../include/asmtest_trace.h) beside the other
   `asmtest_disas_*` queries.
2. `src/ptrace_backend.c`: in `bs_record_run` (:1669) and `window_block_walk`'s
   record loop (:2357-2376), after recording an instruction, if it is a rep string
   op, keep walking (lengths stay correct — the insn is recorded ONCE) but return
   BS_AMBIGUOUS-equivalent truncation to the caller (reuse the existing
   BS_AMBIGUOUS plumbing: prefix recorded, `overflow/truncated` set, tracing
   continues from the next stop). Do **not** attempt to infer the iteration count —
   it is entry `RCX`, which the reconstructor does not have.
3. Bound the promise: amend `include/asmtest_ptrace.h` :100-107, :117-124,
   :152-176 — "byte-identical to the per-instruction stream **except** `rep`-prefixed
   string ops, which retire N times but appear once and mark the capture truncated;
   the per-instruction entries record them once per iteration."
4. `make fmt && make docker-hwtrace`.

**Code.** As above; ~30 lines in `disasm.c`, ~10 in `ptrace_backend.c`.

**Tests.** New differential fixture in `test_ptrace_blockstep`
(`examples/test_hwtrace.c:3923`), e.g.

```
48 c7 c1 08 00 00 00 31 c0 f3 48 ab 48 89 c8 c3
  mov rcx,8; xor eax,eax; rep stosq; mov rax,rcx; ret     (rdi = caller scratch buf)
```

(verify the encoding with `asmtest_trace_disasm` before asserting). Assert: the
per-instruction driver records the `rep stosq` offset **8 times** (RIP parks on it
per TF stop — the real asymmetry); the block-step driver records it **once** and
returns `truncated == 1`; both agree on every other offset; both return the same
result. A pure unit CHECK on `asmtest_disas_is_rep_string` (rep vs plain `stosq`
vs `ret`) goes beside the other disasm checks. Runs in `make docker-hwtrace`;
self-skips with block-step.

**Docs.** `CHANGELOG.md` `Fixed`. Mark Defect 2 fixed in the defects analysis
header (as T1 does for Defect 1). Add one sentence to
[docs/guides/tracing/native-tracing.md](../../guides/tracing/native-tracing.md)'s
block-step passage (~:597) naming the rep-op truncation rule.

**Done when.**
- New CHECKs green in `make docker-hwtrace`; the rep fixture fails red if step 2 is
  reverted.
- `make docs` (or `make docker-docs`) builds warning-free after the guide edit.

### T4 — SP-aware call-out step-over (review finding #19's real fix)  (M, depends on: T2 (run_until_sig))

**Goal.** The level-0 call-out step-over resumes only when the return-address
breakpoint is hit **at the matching stack depth** (`PC == ret && SP == expected`),
so a helper that recurses into or re-enters the region no longer hijacks the trace.

**Steps.**
1. Extend the T2 `run_until_sig` internal into
   `run_until_sp(pid, target, expected_sp, first_sig)` (public-in-file only;
   `expected_sp == 0` keeps today's first-arrival behaviour so `asmtest_ptrace_run_to`
   and descent's already-SP-guarded caller (:1275, protected by the :1176 pop
   predicate) are untouched). On a hit with `SP != expected_sp`: step past and keep
   waiting — software-bp case: restore the original byte, `PTRACE_SINGLESTEP` once
   (no signal attached — this is our own step over a known instruction, not signal
   delivery, so the CONT-only rule does not apply), re-plant the `int3`, continue;
   hardware-bp case: just `PTRACE_CONT` (the kernel sets RF on resume, so the same
   instruction does not re-trap).
2. `classify_region_exit` (:800): read the callee-entry SP (`read_pc_ret` already
   returns SP), compute the expected post-return SP — x86-64: `sp_entry + 8` (the
   `call` pushed the return address; `ret` pops it); aarch64: `sp_entry` (`bl`
   writes the link register, no push) — and pass it to `run_until_sp`. This is the
   same arithmetic the descent shadow stack already relies on
   (`call_sp`/`sp_ret`, :830-846).
3. Rewrite the header caveat at `include/asmtest_ptrace.h:210-217`: the step-over
   is now depth-aware; keep an honest residual note (a callee that *longjmps* past
   the return address is still swept only by region-exit/truncation handling).
4. `make fmt && make docker-hwtrace`.

**Code.** `src/ptrace_backend.c` (`run_until*`, `classify_region_exit`) + the
header comment. Every region driver (per-insn :1421, block-step :1843/:1997,
attached :2842) benefits through the shared classifier — no per-driver edits.

**Tests.** New CHECK-set in `examples/test_hwtrace.c` (beside
`test_ptrace_attach_blockstep`): a two-blob fixture where region R `call`s helper H
(outside the region) and H **calls back to R's entry** exactly once before
returning. Bound the callback to one shot with a flag byte in a shared scratch slot
that H tests-and-sets (H loads the byte; zero → store non-zero and call back into
R's entry; non-zero → skip the callback and return), so the re-entry can never
recurse unboundedly through R's entry (re-entry through the traced region — the
exact #19 failure: the breakpoint at R's call fall-through is executed by the inner
invocation first, at a deeper SP).
Assert: `*result` is the OUTER invocation's value; the recorded stream contains R's
post-call offsets exactly once; `truncated == 0`. Before the fix, the trace resumes
inside the inner invocation and the result/stream assertions fail red (verify once
locally by reverting step 2). Runs under plain `make docker-hwtrace`; on aarch64 it
runs too (per-insn driver) — keep the fixture's machine code per-arch or gate it
`__x86_64__`-only with a printed SKIP, mirroring `test_ptrace_blockstep`'s gate.

**Docs.** `CHANGELOG.md` `Fixed`. Update the re-entrancy caveat sentence in
[docs/guides/tracing/asmspy.md:617](../../guides/tracing/asmspy.md) and the
attached-tracing passage of
[docs/guides/tracing/native-tracing.md](../../guides/tracing/native-tracing.md)
(~:462-510). Internal: strike the "documented follow-up" note in the remediation
row for #19 (`docs/internal/analysis/2026-07-02-code-review.md:55`) with a pointer
here.

**Done when.**
- Re-entry fixture green in `make docker-hwtrace`; red with step 2 reverted.
- `make docker-hwtrace-jit-dotnet-bcl` still passes (Console::WriteLine's step-over
  path exercises `run_until` on W^X hardware breakpoints).
- `make docs` clean after the guide edits.

### T5 — Descent deferred cleanups: maps snapshot, watchdog single-descent guard, block-boundary dedup  (S, depends on: none)

**Goal.** The three code-level items the descent review left "reported, not fixed"
are fixed: no per-call-out `/proc` re-parse, no silent watchdog clash, one copy of
the block-boundary rule.

**Steps.**
1. **Maps snapshot.** Add a small file-static cache in `src/ptrace_backend.c`: on
   first L3 `descend_decide` (:961-975), parse `/proc/<pid>/maps` once into a
   sorted array of executable `[start,end)` ranges held in `dctx_t`; look up there.
   On a MISS, re-parse once and retry (a JIT may map new code mid-descent), then
   step over as today. Free it in `descend_core`'s epilogue.
2. **Watchdog re-entrancy.** The watchdog uses process-global `ITIMER_REAL` + a
   file-scope flag (:977-1014) under a "single-descent-at-a-time" assumption.
   Enforce it: a file-scope `static volatile sig_atomic_t descend_active;` set in
   `descend_watchdog_arm`, cleared in `_disarm`; a second L3 descent arriving while
   it is set runs WITHOUT arming the watchdog (deadline checks still bound it,
   :1016-1027) and marks the descent truncated+depth_capped with a comment naming
   why — honest degradation, no clobbered timers.
3. **Block-boundary dedup.** The rule "new block iff `!have_prev || off !=
   expected_next`" lives thrice: `normalize`
   ([src/ptrace_backend.c:626-649](../../../src/ptrace_backend.c)),
   `asmtest_descent_frame_record` ([src/descent.c:175-211](../../../src/descent.c)),
   and the materializers (:2160-2172, :2710-2727). Extract one tiny tracker into
   [include/asmtest_descent_internal.h](../../../include/asmtest_descent_internal.h)
   (already the shared-internals home):
   `typedef struct { int have_prev; uint64_t expected_next; } asmtest_blockseq_t;`
   + a `static inline int asmtest_blockseq_boundary(asmtest_blockseq_t *s,
   uint64_t off, size_t len);` and use it at all four sites. Pure refactor —
   byte-identical traces.
4. `make fmt && make check && make docker-hwtrace`.

**Code.** As above; no public API change, no behaviour change except the (new,
previously-undefined) concurrent-descent path.

**Tests.** The refactor is guarded by the existing descent + normalization suites
(`make docker-hwtrace` totals unchanged). Add one CHECK for step 2: two
back-to-back L3 descents in one process still both succeed (arm/disarm re-entry is
clean); a genuinely concurrent test would need threads driving two tracees —
out of proportion, so the guard's degradation path is covered by a direct unit call
of arm/arm/disarm asserting the second arm was refused (expose the guard via
`asmtest_descent_internal.h` for the test). Step 1: assert (via a counter hook in
the same internal header) that a 3-call-out L3 descent parses maps at most twice.

**Docs.** Internal-only, no user-facing docs — pure internals; in the Deferred list
at `docs/internal/analysis/2026-07-03-call-descent-review.md:82-95` (five bullets)
strike the maps-re-parse, block-boundary-dedup, and concurrent-descent/watchdog
bullets (the three T5 fixes) with a "fixed, see this doc" note, and — coordinating
with T6's edit — the harness-nits bullet. **Leave the bare-`call` fall-through
bullet standing**: no task here fixes it (it mirrors existing level-0 behavior).
`CHANGELOG.md`: not needed (no user-visible change).

**Done when.**
- `make docker-hwtrace` totals unchanged or higher, 0 failed.
- `make docker-hwtrace-jit-dotnet-descend` and `-descend-all` (the live L2/L3
  lanes, [mk/docker.mk:507-515](../../../mk/docker.mk)) still pass.
- The maps-parse counter CHECK passes.

### T6 — Harness nits: vacuous L3 CHECK, skip-path mmap leaks, dropped `-descend` suffix  (S, depends on: none)

**Goal.** The three test-harness defects from the descent review's deferred list are
gone.

**Steps.**
1. `examples/jit_trace.c:384-388`: the L3 CHECK's `nf >= 1` arm is vacuous (frame 0
   always exists). Replace with a falsifiable disjunction: truncated OR
   depth-capped OR `nf >= 2` OR `ne >= 1` (a real descent produced a frame or an
   edge, or a guard honestly fired).
2. `examples/test_hwtrace.c` `test_descent_attach` (:6949): both skip paths leak —
   :6967-6970 returns without unmapping whichever of the two `mmap`s succeeded;
   :6988-6994 (yama skip) returns without unmapping either. Add the `munmap`s.
3. `examples/jit_trace.c`: `node` (:809) and `java-bcl` (:902) pass literal `0`
   instead of `descend_level`, so `jit_trace node-descend` / `java-bcl-descend`
   silently run without descent. Pass `descend_level` through, matching the
   `dotnet` (:827) / `java` (:878) lanes.
4. `make fmt`, rebuild, run the lanes.

**Code.** Two example files, ~15 lines total.

**Tests.** These ARE test files; the verification is the lanes themselves:
`make docker-hwtrace` (runs `test_descent_attach`), `make docker-hwtrace-jit`
(node), `make docker-hwtrace-jit-java-descend` (proves the suffix now reaches
`trace_runtime` — the run must print a `# descent L2:` line, which before this fix
never appeared for a node/java-bcl `-descend` invocation).

**Docs.** Internal-only, no user-facing docs (test-harness hygiene). Strike the
"harness nits" bullet in the descent review (with T5's edit).

**Done when.**
- `make docker-hwtrace` green; jit lanes green.
- `make docker-hwtrace-jit-java-descend` output contains `# descent L`.

### T7 — Block-step pre-cover: an IBS covered-block table that memoizes reconstruction  (M, depends on: T1 (touches the same functions; land after))

**Goal.** A covered-block set from `asmtest_ibs_normalize_blocks` can be handed to
the block-step drivers, which pre-decode those blocks once and skip the per-stop
Capstone re-scan on cache hits — with the output stream **provably byte-identical**
to the uncached walk.

**Design (why memoization).** The exact contract forbids using statistical coverage
to skip *recording* anything (the IBS lane's INVARIANT,
`include/asmtest_ibs.h:21-23`, and the archived plan's "no statistical result can
reach the parity assertions"). What block-step can legitimately shed is its
tracer-side decode cost: every `#DB` stop re-runs `bs_scan_terminator` +
`bs_record_run` over the block's bytes, and each `asmtest_disas_probe` call opens
and closes a Capstone handle (`src/disasm.c`), so a hot loop re-decodes the same
block millions of times. The IBS covered set is exactly the statistically-hot block
leaders — the highest-value cache seeds. A cache miss falls back to the shipped
scan, so fidelity is untouched by construction. The DynamoRIO half of the plan's
phrase is explicitly **not** built: DR's code cache already runs at native speed
and takes no per-block tracer work a coverage hint could remove (see Out of scope).

**Steps.**
1. New internal header `include/asmtest_blockstep_internal.h` (modelled on
   `asmtest_descent_internal.h` — internal, shipped uninstalled, so **no new public
   ABI symbols and no bindings-parity churn**):

   ```c
   typedef struct asmtest_bs_precover asmtest_bs_precover_t;
   /* Build from a region snapshot + region-relative covered leaders
    * (asmtest_ibs_normalize_blocks(survey, base, len, …) output). Pure; NULL ok. */
   asmtest_bs_precover_t *asmtest_bs_precover_build(
       const uint8_t *code, size_t len, uint64_t base_ip,
       const asmtest_ibs_blocks_t *covered);
   void asmtest_bs_precover_free(asmtest_bs_precover_t *p);
   /* test hooks */
   void asmtest_bs_stats(uint64_t *probe_calls, uint64_t *precover_hits);
   void asmtest_bs_stats_reset(void);
   ```

2. In `src/ptrace_backend.c`: implement the table — for each covered leader,
   pre-walk its straight-line run once with `classify_branch`'s primitives,
   storing per instruction `{off, len, kind, static_target}` up to the first
   always-taken instruction / region edge / `PTRACE_BLOCK_WALK_CAP`. Give
   `blockstep_reconstruct` (:1695) an optional `const asmtest_bs_precover_t *`:
   on a leader hit, resolve the terminator against `next_pc` by replaying the
   BS_OK/BS_AMBIGUOUS/BS_FAIL state machine (:1623-1665) over the **cached**
   entries — the same decisions from the same data, no Capstone calls — and emit
   the stream from the cached offsets. Miss → shipped path. Thread the pointer
   through the region and attached drivers via file-static plumbing (a
   `bs_precover_current` set/cleared around the loop) so the public signatures do
   not change; increment the two counters in both paths.
3. Unit-test the build purely (no AMD needed): synthesize an
   `asmtest_ibs_survey_t` the way `examples/test_ibs.c:123-155` does, normalize it
   against a fixture buffer, build the table, and assert leaders/lengths/kinds.
4. Differential: run `test_ptrace_blockstep`'s LOOP fixture twice — precover NULL
   vs precover covering the loop head — assert identical `insns[]`, `blocks[]`,
   `truncated`, result; assert `precover_hits > 0` and `probe_calls` strictly
   smaller with the table. Include a **wrong/hostile** precover case (leaders that
   are not real instruction boundaries): the build must reject or the walk must
   miss-and-fallback — output still identical, never corrupted.
5. `make fmt && make docker-hwtrace`.

**Code.** `src/ptrace_backend.c` (+~150 lines), the new internal header,
`examples/test_hwtrace.c` tests. `ibs_backend.o` is already linked everywhere
`ptrace_backend.o` is (`HWTRACE_OBJS`), and `asmtest_ibs_normalize_blocks` is pure
and host-independent, so nothing here needs AMD hardware.

**Tests.** As steps 3-4; all synthetic, all green on any Linux x86-64 host in
`make docker-hwtrace`; the block-step availability gate provides the aarch64
self-skip.

**Docs.** `CHANGELOG.md` `Added` (one entry shared with T8). Amend the
cross-reference note at
[amd-tracing-plan.md](../plans/amd-tracing-plan.md) ~:1377-1388: the pre-cover
integration is no longer "unscheduled and unowned" — owned here.

**Done when.**
- New CHECKs green in `make docker-hwtrace`, including the identical-stream and
  fewer-probe-calls assertions and the hostile-precover case.
- `asmtest_bs_stats` shows `probe_calls` reduced by >30% on the LOOP differential
  (record the observed number in the commit message).

### T8 — Cascade wiring: `ASMTEST_TRACE_IBS_PRECOVER` policy bit in `asmtest_trace_call_auto`  (S, depends on: T7)

**Goal.** An opt-in policy bit makes the auto cascade's block-step rung
(`src/trace_auto.c:279-302`) survey the routine under IBS first and run block-step
with the resulting pre-cover — off AMD (or without the bit) the cascade is
byte-identical to today.

**Steps.**
1. `include/asmtest_trace_auto.h`: add `#define ASMTEST_TRACE_IBS_PRECOVER 0x4`
   beside `0x1`/`0x2` (:136-141), documented honestly: *opt-in; when IBS is
   available it runs the routine additional times in a fork-isolated warm-up child
   (bounded, ~30 ms) to gather statistical coverage — non-idempotent side effects
   repeat; capture fidelity is unchanged* (the framing precedent is the MSR rung's
   in-process-re-run honesty note, `src/trace_auto.c:239-256`).
2. `src/trace_auto.c`, block-step rung: when the bit is set and
   `asmtest_ibs_available()` (declare via `#include "asmtest_ibs.h"`), fork a
   warm-up child that loops `call_auto_invoke(code, args, nargs)` until a pipe
   close / ~30 ms elapses; run `asmtest_ibs_survey_process(child, 30, NULL, &sv)`
   against it; reap; `asmtest_ibs_normalize_blocks(&sv, (uint64_t)code, len, &blk)`;
   `asmtest_bs_precover_build(...)`; install it around the
   `asmtest_ptrace_trace_call_blockstep` call via the T7 plumbing; free
   everything. ANY failure along the way (survey EUNAVAIL, zero blocks, OOM)
   degrades silently to the plain rung — the bit may never make the cascade fail
   where it previously succeeded.
3. Do NOT touch `CASCADE[]`/`asmtest_trace_resolve` — IBS stays out of the
   exact-cascade rows and out of `*used` (the winning rung still reports
   `MECH_BLOCKSTEP`; pre-cover is an accelerator, not a mechanism).
4. `make fmt && make docker-hwtrace && make ibs-test`.

**Code.** `src/trace_auto.c` (+~60 lines), one `#define`, no new public symbols
(bindings pass `policy` as an integer already; mirroring the constant into the ten
bindings is a docs-level nicety, deliberately not done here).

**Tests.** In `examples/test_hwtrace.c` (it links both backends): call
`asmtest_trace_call_auto` on the LOOP fixture with and without the bit under
`ASMTEST_TRACE_CEILING_FREE` (forcing past the LBR tiers) and assert identical
traces + rc + `used->mechanism`. On a non-AMD host `asmtest_ibs_available()` is 0
and the test degenerates to bit-is-a-no-op — still a real assertion (identical
output), printed as such. The live shrink itself is asserted via `asmtest_bs_stats`
**only when** `asmtest_ibs_available()` — self-skip with the printed reason
`asmtest_ibs_skip_reason()` elsewhere. Live validation lane:
`make docker-hwtrace-privileged` (CAP_PERFMON, runs `hwtrace-test ibs-test`) on the
Zen 2 (Ryzen 4900HS) and Zen 5 (Ryzen 9950X) dev boxes.

**Docs.** `CHANGELOG.md` `Added` (with T7). Add a short "IBS pre-cover" paragraph
to [docs/guides/tracing/amd-lbr-tuning.md](../../guides/tracing/amd-lbr-tuning.md)
or the AMD section of
[docs/guides/tracing/hardware-tracing.md](../../guides/tracing/hardware-tracing.md)
(whichever hosts the Zen 2 story) naming the policy bit and its extra-runs caveat;
`make docs` must stay warning-free.

**Done when.**
- `make docker-hwtrace` green everywhere (bit-as-no-op asserted off AMD).
- On a Zen dev box: `make docker-hwtrace-privileged` green, the precover-hit
  assertion actually ran (its CHECK line printed, not the SKIP line), and the
  recorded probe-call shrink is quoted in the commit message.
- Without the bit, `asmtest_trace_call_auto`'s behaviour is bit-for-bit unchanged
  (the with/without differential proves it).

## Task order & parallelism

- **T1 → T2** are one storyline (T2 reuses the helper; T1 step 5 and T2 step 3
  co-land). **T2 → T4** (T4 extends `run_until_sig`). **T1 → T7 → T8** (T7 edits
  the functions T1 rewrites; T8 consumes T7).
- Independent of everything: **T3**, **T5**, **T6** — three people can start T1,
  T3, and T5/T6 concurrently.
- Critical path: **T1 → T2 → T4** (tracer correctness), then **T7 → T8**.
- Because T1/T2/T3/T4/T7 all edit `src/ptrace_backend.c`'s SIGTRAP gates and
  reconstruction functions, do not parallelize any two of THOSE beyond (T3 vs
  T1/T2) without agreeing on merge order — this doc exists to keep those edits
  under one owner.

## Constraints & gates

- **The int3 fix shape is fixed by measurement, not by the defects analysis.** The
  analysis says "separate TRAP_TRACE/TRAP_BRANCH from TRAP_BRKPT"; that split is
  refuted in-tree: on x86-64 a step completing across a syscall reports
  `TRAP_BRKPT` (ours), and a real executed `int3` reports `SI_KERNEL`
  (`src/dataflow_ptrace.c:788-807`, `cli/asmspy_engine.c:1858-1878`, both marked
  MEASURED). Use the `SI_KERNEL`/`TRAP_HWBKPT` app-whitelist, test "ours"
  (breakpoint-address match) before the whitelist, and forward app breakpoints via
  `PTRACE_CONT` — never `PTRACE_SINGLESTEP`/`PTRACE_SINGLEBLOCK` with the signal
  attached (measured fatal: the re-armed trap fires inside the masked handler).
- **IBS is statistical and must never feed the exact parity contract**
  (`include/asmtest_ibs.h:21-23`). Pre-cover may only memoize; any design change
  that lets coverage skip *recording* is out.
- **No new hardware gates for the code:** everything above through T7 is
  ptrace-of-own-child + Capstone and runs in plain Docker. The only real gates are
  live-silicon validation legs: T8's precover-hit assertion needs an AMD host with
  the `ibs_op` PMU + kernel `swfilt` (the Zen 2 / Zen 5 dev boxes; the lane
  self-skips elsewhere printing `asmtest_ibs_skip_reason()`), and BTF block-step
  self-skips where a hypervisor masks `DEBUGCTL.BTF` (the existing
  `asmtest_ptrace_blockstep_available()` probe). When a gate blocks a validation
  leg, record host + skip line in the commit message — never weaken the test.
- **No new third-party dependencies**; Capstone (5.0.1, `scripts/build-capstone.sh`)
  already covers every decode need, so the CLAUDE.md pinned-dependency rule is
  satisfied vacuously. No new public ABI symbols (bindings parity gate stays at its
  current count); new internals go in internal headers.
- Formatting is CI-gated: `make fmt` before every commit.

## Research notes (verified 2026-07-17)

No external research was needed for this doc; every load-bearing fact is
repo-internal and was re-verified against the working tree as cited inline. The
three empirical facts a reader might otherwise re-derive, with their in-tree
provenance:

- x86-64 si_code semantics under ptrace single-step (`TRAP_BRKPT` can be ours,
  real `int3` is `SI_KERNEL`, app hw-breakpoint is `TRAP_HWBKPT`): measured
  findings recorded at `cli/asmspy_engine.c:1858-1878` and
  `src/dataflow_ptrace.c:788-807`, plus
  [asmspy-plan.md](../plans/asmspy-plan.md) Theme C (:102).
- `rep`-prefixed string ops retire once per iteration under TF single-step (8 stops
  with RIP parked, RCX 8→1): reproduced in
  [2026-07-17-blockstep-reconstruction-defects.md](../analysis/2026-07-17-blockstep-reconstruction-defects.md)
  (which cites Intel SDM Vol 3 §18.3.1.4).
- Signal-injected `PTRACE_SINGLESTEP` kills a target whose SIGTRAP handler masks
  SIGTRAP: measured at `cli/asmspy_engine.c:1880-1885`.

## Out of scope

- IBS header/window-lane honesty (retiring the unconsumed `PERF_SAMPLE_CALLCHAIN`
  advertisement, callchain-aware `IBS_MAX_RECORD`) and the AMD validation-doc
  rewrite — [amd-ibs-backend-honesty.md](amd-ibs-backend-honesty.md).
- AMD LBR docs corrections (Zen 4+ floor, `asmtest_amd_freeze_available`
  retirement) — [amd-branchsnap-lbr-docs.md](amd-branchsnap-lbr-docs.md).
- A DynamoRIO-side pre-cover consumer: the drtrace tier executes in a native-speed
  code cache with no per-block tracer round-trip for coverage to shrink, and that
  tier remains `dr_app_*`-cooperative; anything Pin-shaped lives in
  [pin-xed-trace-tier.md](pin-xed-trace-tier.md) /
  [pin-probe-mode-capture.md](pin-probe-mode-capture.md).
- The in-process BTF block-step arm — [inproc-btf-block-step.md](inproc-btf-block-step.md).
- AArch64 single-step validation on real hardware —
  [aarch64-ptrace-single-step-validation.md](aarch64-ptrace-single-step-validation.md);
  macOS out-of-process stepping — [macos-oop-mach-stepper.md](macos-oop-mach-stepper.md).
- The emulator-replay block-step design (immune to Defects 1-2 by construction,
  `src/dataflow_blockstep.c:1-101` — the file-head design comment) and all
  data-flow-tier work —
  [dataflow-producer-correctness.md](dataflow-producer-correctness.md).
- asmspy engine features (its si_code split already shipped) —
  [asmspy-cli-enhancements.md](asmspy-cli-enhancements.md).
