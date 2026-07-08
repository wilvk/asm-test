# asm-test — auto-escalating cross-tier trace (`asmtest_trace_call_auto`)

A single call-owning entry point that traces a native routine under the fastest exact
tier and **automatically escalates to a ceiling-free tier when the trace comes back
`truncated`** — closing the "arm → detect truncation → re-resolve → re-run" loop that is
today only a *documented idiom*, never implemented.

This is the concrete follow-through of the AMD-tracing determination that the AMD LBR
window is at its safe ceiling (measured: `#2B` reduced filter buys ~1.86× per window,
`#2A` period-spacing can't help loops): for anything past the ~16–32-branch window the LBR
tier truncates *by design*, so the way to make Zen 5 tracing **reliably complete** is to
stop treating LBR as the answer for loops and make "exact-and-fast where it fits,
complete-everywhere-else" the automatic default. See
[amd-tracing-plan.md](amd-tracing-plan.md) for the AMD-specific levers this composes.

> Status legend: **planned** unless a phase says otherwise. House rule holds: no untested
> code — the live escalation (LBR truncates → block-step completes) is validated on the
> Zen 5 dev box (Ryzen 9 9950X), the plumbing host-independently on any x86-64 Linux.

---

## The gap (confirmed against the source)

Three facts, cross-checked against [src/trace_auto.c](../../../src/trace_auto.c),
[include/asmtest_trace_auto.h](../../../include/asmtest_trace_auto.h), and every tier's
entry points:

1. **The resolvers only *pick*; they never *run*.** `asmtest_trace_resolve` /
   `asmtest_trace_auto` ([trace_auto.c:84-106](../../../src/trace_auto.c)) filter a static
   `CASCADE[]` by availability + policy and return `{tier, backend, fidelity}` choices.
   Neither arms a PMU, executes a routine, reads `trace.truncated`, or re-runs anything.
2. **There is no `choice → trace` dispatcher.** `asmtest_trace_choice_t` is only ever
   *written* (by the resolvers) and never *read* by any library function. A caller must
   `switch (choice.tier)` and hand-drive each tier's own separate API.
3. **The dynamic-fallback loop is written nowhere.** "Resolve BEST → if `truncated`,
   re-resolve CEILING_FREE → re-run under the new tier" appears only as prose in three
   places ([asmtest_trace_auto.h:117-122](../../../include/asmtest_trace_auto.h),
   [trace-parity-matrix.md](../analysis/trace-parity-matrix.md), and
   [native-tracing.md](../../guides/tracing/native-tracing.md), where the re-run body is a
   literal `/* ... */` comment). No example, test, or binding closes the loop; the one
   arming example that touches `truncated` (`examples/dotnet/amdlbr`) retries the *same*
   backend and only *reports* the flag.

So the win is the missing glue: a **call-owning** entry point that owns the invocation
(so it *can* re-run), dispatches to the right tier's real API, and walks the ladder until
the trace is complete or the tiers are exhausted.

---

## The invocation-model constraint (the crux)

The tiers split into two structurally different models — this is what makes the loop
non-trivial to hand-write and dictates the design:

- **Call-owning** — the tier takes `(code, args, nargs)` and issues the call itself, so it
  *can* re-run the routine under a fresh tier. The unified tuple
  `(code, len, long args[nargs], nargs) → (long result, asmtest_trace_t*, truncated)` is
  already the exact shape of three entries:
  - `asmtest_ptrace_trace_call` and `asmtest_ptrace_trace_call_blockstep`
    ([asmtest_ptrace.h:95/108](../../../include/asmtest_ptrace.h)) — fork a tracee, call
    `code(args…)` in the child, single-step / block-step from the parent; **re-run is
    side-effect-isolated in the child.**
  - `asmtest_hwtrace_call_scoped_ex` ([asmtest_hwtrace.h:301](../../../include/asmtest_hwtrace.h))
    — the in-process single-step variant, same tuple plus a scope handle.
- **Marker-bracket** — `register_region` + `begin`/`end` around the *caller's own* call.
  The AMD-LBR tier and DynamoRIO tier are marker-only; they observe one in-line execution
  and never take `fn`+args. **They do not re-run.**

**Design consequence.** `asmtest_trace_call_auto` OWNS the call, so:

- The **fast HWTRACE step** drives the marker API but calls `fn` *itself* between
  `begin`/`end` (in-process), so it is call-owning at the wrapper level while reusing the
  proven marker path — and the AMD `#3` single-exit-snapshot default kicks in automatically
  for a lone-`ret` region. The backend is whatever `asmtest_hwtrace_auto(BEST)` selects
  (AMD LBR on Zen 5, Intel PT on Intel, in-proc single-step elsewhere).
- The **escalation steps** use the ptrace call-owning entries (fork-isolated).
- **DynamoRIO and the emulator are excluded** from the call-auto ladder: DR has no
  call-owning entry (marker-only), and the emulator is a native→virtual *fidelity crossing*
  the resolver gates behind `NATIVE_ONLY` and calls "never an automatic last resort." A
  caller that wants those reaches them through their own model, unchanged.

---

## The ladder

`asmtest_trace_call_auto(code, len, args, nargs, policy, result, trace, used)` walks, in
order, stopping at the first tier that returns a **non-`truncated`** trace (or the last
tier, honestly flagged):

1. **Fast exact — best HWTRACE backend.** `asmtest_hwtrace_auto(BEST)` → `init(backend)`,
   `register_region(code,len,trace)`, `begin`, `*result = invoke(code,args,nargs)`, `end`,
   `shutdown`. On Zen 5 this is AMD LBR (deterministic `#3` snapshot for the single-`ret`
   region, else the sampled/stitched path); on Intel, PT (unbounded — normally completes
   with no escalation). If `!trace->truncated`, done.
2. **Complete rootless — BTF block-step.** If step 1 truncated or was unavailable:
   reset `trace`, then `asmtest_ptrace_blockstep_available()` →
   `asmtest_ptrace_trace_call_blockstep(code,len,args,nargs,result,trace)`. Complete flow at
   ~1 `#DB` per taken branch, rootless, no ceiling. **This is the key middle rung that the
   static `CASCADE[]` lacks** — block-step is a ptrace entry, not a resolve backend, so
   `call_auto` inserts it explicitly. If `!trace->truncated`, done.
3. **Complete floor — per-instruction single-step.** If block-step is unavailable
   (non-x86 / `PTRACE_SINGLEBLOCK` unwired) or still truncated: reset `trace`, then
   `asmtest_ptrace_trace_call(code,len,args,nargs,result,trace)`.

`*used` (optional) reports the `{tier, backend}` that produced the final trace, so a caller
can see whether escalation happened (e.g. `used.backend != AMD_LBR`). Returns
`ASMTEST_HW_OK` whenever *some* tier ran (completeness is read from `trace->truncated`),
`ASMTEST_HW_EUNAVAIL` if no call-owning tier is available, `ASMTEST_HW_EINVAL` on bad args,
`ASMTEST_HW_ENOSYS` off x86-64 Linux.

Because the trace is caller-owned and reused across attempts, each step first **resets** it
(clears `insns`/`blocks`/`truncated`) so the winning tier's reconstruction stands alone.

---

## Phases

### Phase 1 — the in-process call dispatcher + fast HWTRACE step *(planned)*

- Add a small static `invoke(const void *code, const long *args, int nargs)` in
  [trace_auto.c](../../../src/trace_auto.c): a `switch (nargs)` casting `code` to the SysV
  `long(long,…)` prototype for 0–6 integer args, returning `long`. (Mirrors the dispatch in
  `asmtest_hwtrace_call_scoped`'s impl.) `>6` or FP is rejected upstream with `EINVAL`.
- Drive the fast tier: `asmtest_hwtrace_auto(ASMTEST_TRACE_BEST-equivalent)` → `init` with
  that backend → `register_region` → `begin` → `invoke` → `end` → `shutdown`.
- Acceptance: on a non-AMD x86-64 Linux host, `call_auto(add2,{20,22})` returns 42 with a
  complete single-step trace and `used.backend == SINGLESTEP`.

### Phase 2 — the escalation ladder *(planned)*

- After the fast step, if `trace->truncated`: reset the trace and try block-step, then
  per-insn single-step, per the ladder above. Reuse `asmtest_ptrace_blockstep_available()`
  and the two ptrace call entries (same TU / library).
- Trace reset: a small helper (or the existing trace-reset primitive) that zeroes
  `insns_len/total`, `blocks_len/total`, and `truncated` on the caller's `asmtest_trace_t`
  between attempts.
- Acceptance (host-independent): a routine block-step reconstructs completely is returned
  non-truncated with `used` a ceiling-free tier when the fast tier truncates.

### Phase 3 — public surface + policy/arg validation *(planned)*

- Declare `asmtest_trace_call_auto` in
  [asmtest_trace_auto.h](../../../include/asmtest_trace_auto.h) with the docstring
  extending the existing "dynamic-fallback idiom" note (now *implemented*, not idiom).
- Validate args (`nargs` 0–6, non-NULL `code`/`trace`), honor `policy` (`BEST` default;
  `NATIVE_ONLY` is a no-op here since every ladder tier is native), and fill `*used`.
- Non-x86-64 / non-Linux: `#else` stub returns `ASMTEST_HW_ENOSYS` (mirrors the other
  backends' host gating), so the symbol always links.

### Phase 4 — tests *(planned)*

Added to [examples/test_hwtrace.c](../../../examples/test_hwtrace.c) (`make hwtrace-test`),
alongside `test_cross_tier_resolve`:

- **`test_call_auto_basic` (host-independent).** `call_auto(add2,{20,22})` → result 42,
  trace not truncated, block 0 covered, `used` a valid tier. Exercises the call-owning
  plumbing + the fast→floor path on any x86-64 Linux (single-step where no AMD).
- **`test_call_auto_escalate` (host-independent structural + live-sharpened).** A loop
  sized to overflow the 16-entry LBR window but fit a block-step trace cap. Assert: result
  correct, **trace NOT truncated** (the ladder reached a ceiling-free tier), loop-body block
  covered. On Zen 5 (`docker-hwtrace-amd` / `-codeimage` with CAP_PERFMON) the fast LBR tier
  truncates and `used` reports the escalated (block-step) tier — printed as the live proof
  that escalation fired; on non-AMD the single-step floor completes it directly.

### Phase 5 — parity, docs *(planned)*

- **Bindings parity:** `asmtest_trace_auto.h` is scanned by the gate. `call_auto` is a
  call-owning C-level entry in the `call_scoped` family (which is `ALL`-exempt, dotnet-lead)
  rather than the pure-query `trace_auto`/`trace_resolve` family (wrapped everywhere), so it
  ships C-first with an `ALL asmtest_trace_call_auto` allow-list entry (reason inline);
  bindings gain it when they grow a call-auto wrapper. Also fix the stale two-header comment
  in [bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt).
- **Docs:** the cross-tier front-end section of
  [trace-parity-matrix.md](../analysis/trace-parity-matrix.md) (Matrix 8) documents the new
  entry point and its escalation contract; the header docstring is the API-level doc.

---

## Risks & open points

- **Re-run requires re-runnable code.** Escalation invokes the routine once per attempted
  tier (fast in-process, then fork-isolated block-step / single-step). Pure-compute
  registered routines (asm-test's common case) are idempotent and safe. A *side-effecting*
  routine gets the fast tier's in-process side effects for real; the ptrace re-runs are
  isolated in a child, but the double-execution must be documented — `call_auto` is for
  deterministic routines, exactly like `trace_call` / `call_scoped` already are.
- **Arg model: 0–6 integer (SysV), no FP/mixed.** Matches every call-owning entry's ceiling.
  Wider or `double` signatures return `EINVAL` in v1; a `call_auto_fp` sibling is a
  follow-up only if a caller needs it (same posture as `call_scoped` vs `call_scoped_fp`).
- **Fast-tier truncation is the trigger, not overflow-of-cap.** The ladder escalates on
  `trace->truncated`. A trace that truncates because the caller's `insns` cap is too small
  (not because the tier hit a ceiling) would escalate pointlessly and truncate again on the
  larger tier — so the test sizes the cap generously and the docstring notes the cap should
  fit the expected reconstruction.
- **DynamoRIO / emulator excluded.** Deliberate (no call-owning entry / fidelity crossing).
  If a rootless-and-native-speed rung is ever wanted, DR would need a call-owning
  `asmtest_dr_trace_call` first; out of scope here.

## Validation (per lane, self-skipping)

- **Host, privilege-free (`make hwtrace-test`, ordinary CI):** `test_call_auto_basic` and
  the structural half of `test_call_auto_escalate` run live (single-step / block-step need
  only ptrace of one's own child) and self-skip where unsupported.
- **Bare-metal AMD (`docker-hwtrace-amd` / `docker-hwtrace-codeimage`, Zen 5 + CAP_PERFMON):**
  the escalation fires for real — the LBR fast tier truncates a loop, `call_auto` escalates
  to block-step, and the trace comes back complete with `used` ≠ AMD LBR.
- **Parity:** `make check-bindings-parity` stays green (allow-listed).
