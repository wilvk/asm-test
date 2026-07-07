# asm-test — call-descent code review (2026-07-03)

*Status: review / findings, remediated. A focused code-level review of the
[call-descent feature](../archive/plans/call-descent-plan.md) (the `asmtest_descent_t`
out-of-process descending tracer + its ten language bindings), run immediately after
the feature landed and before it was committed. Scope is the descent diff only —
`src/descent.c`, the descent additions in `src/ptrace_backend.c`, the two new
`src/disasm.c` queries, `include/asmtest_ptrace.h` / `asmtest_trace.h`, the conformance
`ptrace_descent` tier, and the per-binding `Descent` wrappers.*

**Method:** eight independent single-angle finders read the diff and the enclosing code
in full — line-by-line scan, removed-behavior auditor, cross-file caller/callee tracer,
a deep shadow-stack-correctness angle (the highest-risk code), reuse/simplification/
efficiency, per-binding correctness, altitude/build-wiring, and resource/error-path
leaks. Every candidate with a nameable failure was verified against the code; only
survivors are listed. The fork/x86-64 path was validated the whole way through with the
133-check C suite (`examples/test_hwtrace.c`) under AddressSanitizer; the AArch64 objects
were cross-compiled clean; one binding per language family was driven end-to-end.

Paths are repo-relative; `file:line` points at the site as reviewed.

---

## Remediation status

**Complete (2026-07-03): all 9 correctness/quality findings fixed, 2 live-path gaps
documented as known limitations.** Fixes were applied in the same working tree and
re-validated (C suite 133/133, ASan-clean; conformance 20/20; bindings-parity in sync;
representative bindings green; AArch64 compile clean). Two findings (D1, D2) are real but
reachable only on the live/attached managed-runtime path — already documented
best-effort/expected-to-perturb — and are recorded as honest limitations in the plan's
[Correctness core](../archive/plans/call-descent-plan.md) section rather than papered over. A new
regression test (`test_descent_stale_alarm_flag`) was added for the highest-severity fix
and confirmed to fail on the pre-fix code.

| # | Finding | Severity | Status |
|---|---------|----------|--------|
| 1 | Stale L3 watchdog flag (`descend_alarm_fired`) aborted healthy L1/L2 traces on any unrelated `EINTR` — a self-inflicted regression from the L3-only-watchdog gating | High | **Fixed** — flag gated on `watchdog`; regression test added |
| 2 | Fork tracee reaped twice: `descend_core`'s own `waitpid` reaps the exited child, then `trace_call_descend` unconditionally `kill`+`waitpid`s the stale PID (recycled-PID kill in a concurrent harness) | High | **Fixed** — `reaped` flag gates the cleanup |
| 3 | Hard step-cap (`DESCEND_HARD_STEP_CAP`) returned `ASMTEST_PTRACE_OK` with `*result` never set and the tracee killed mid-flight | High | **Fixed** — returns `ETRACE` like the deadline path |
| 4 | L2 descent could not be bounded by a caller's own `alarm()`: the `EINTR`-retry loop swallowed every non-watchdog signal, so a blocked descended syscall could hang past a caller watchdog | Medium | **Fixed** — wait also breaks when the real-time deadline is reached under any interrupt |
| 5 | `depth_capped` set at every call-out past the budget, even for callees that would never descend — "guard tripped" reported spuriously | Medium | **Fixed** — decide independently of budget; flag only a suppressed-would-be-descent |
| 6 | Level-2 resolver's returned `(base,len)` accepted without checking it contains the callee → `pc - base` underflow to a ~2⁶⁴ frame offset | Low | **Fixed** — containment enforced (matches allow-set / proc-maps paths) |
| 7 | Tail-`jmp` out of a descended frame could mis-parent / corrupt the frame tree (stale parent `last_was_call`) | Medium | **Mitigated** — parent pending-call cleared on push → honest truncation, not corruption (full keep-open deferred, see D2) |
| 8 | `descend_probe` did 3× Capstone `cs_open`+decode+`cs_close` per single-stepped instruction (is_call + is_ret + length) on a hot path | Medium (perf) | **Fixed** — new `asmtest_disas_probe` returns all three in one decode |
| 9 | Doc/contract inaccuracies: `asmtest_descent_free` "double-free is a no-op" (false for a raw C pointer); Ruby binding `0 = unbounded`/`disables` budget/watchdog docs; watchdog "caller alarm undisturbed" overstatement; misleading `call_target` test comment; undocumented self-recursion-splits-at-L1 divergence from the level-0 fold | Low | **Fixed** — contracts corrected across header, `descent.c`, Ruby binding, test |
| D1 | **Live path only:** signal-frame SP-pop suspension not implemented — an async safepoint signal (`SIGSEGV`/`SIGPROF`/`SIGURG`) inside a descended frame makes the branch-D catch-all pop prematurely and mis-read a root return | Medium (live only) | **Documented** — known limitation; live L3 is already best-effort/expected-to-perturb |
| D2 | **Live/known path:** tail-call keep-open (record edge + `run_until` the tail-callee's real return) not implemented | Low | **Documented** — defensive mitigation shipped (see #7); full keep-open is future work |

---

## What the review cleared (verified correct)

The highest-risk mechanics all held up:

- **Shadow-stack pop predicate** — `PC == ret_addr && SP == sp_at_call && just-stepped-was-ret`
  is correct for x86-64 `call`/`ret` and AArch64 `bl`/`ret`; `sp_ret` is captured as the
  caller's pre-call SP (no off-by-8, because the callee's `ret` restores exactly that value).
- **SP-sweep** (`sp > sp_ret`) never over-pops normal recursion (a deeper callee's SP is
  *below* the parent's, so it can't trigger); it fires only on a genuine non-local SP rise.
- **Same-region recursion** as a distinct frame; the `pc != fallthru` check correctly
  distinguishes a return-from-descended-call from a recursive self-call, and a backward loop
  branch (not a call) is gated out by `last_was_call`.
- **`entered` gate** records offset 0 exactly once on both the fork and the run_to/attach
  entry conventions.
- **Pointer safety** — the loop holds `dframe_t*` into a fixed local `stack[]` (never
  realloc'd), and every pool mutator re-fetches `&d->frames[fi]` after its own `pool_reserve`,
  so the growable-pool reallocs invalidate nothing held across a push.
- **Build wiring** — `descent.o` accompanies `ptrace_backend.o` in every target that links it
  (`HWTRACE_OBJS`, the pic/shared-hwtrace lists, and the conformance binary); no link gap.
- **The 10 bindings** — FFI signatures match the C prototypes; idempotent free (NULL the
  handle) everywhere; address getters stay 64-bit (Node `BigInt`, Lua boxed `uint64` cdata,
  Ruby unsigned mask); `frame_parent` read signed (`-1` = root); `trace_call_ex` forwards an
  explicit region length; resolver upcall trampolines pinned against GC for the handle
  lifetime and freed after `descent_free`.
- **The `blocks_len 2 → 3` test edits** are a legitimate correction to the pre-existing
  ends-at-branch block model (matching the C reference `test_singlestep_loop`'s `{0,0x7,0xf}`),
  not a masked regression.

---

## Deferred (reported, not fixed — pre-existing or minor)

- A region ending in a bare `call` (fall-through outside the region) reads as a return —
  mirrors existing level-0 behavior, not descent-introduced.
- `/proc/<pid>/maps` re-parsed per L3 call-out (`asmtest_proc_region_by_addr`); a parsed
  snapshot would replace an O(maps) syscall+parse per call-out.
- Block-boundary derivation duplicated across `normalize()` / `asmtest_descent_frame_record`
  / `trace_append_block`'s dedup (three copies of the rule).
- Concurrent L3 descents would clash on the process-global `ITIMER_REAL` + file-scope
  `descend_alarm_fired` (the code assumes single-descent-at-a-time).
- Minor harness nits: `jit_trace`'s L3 `CHECK` is vacuous (frame 0 always exists);
  `node`/`java-bcl` ignore the `-descend` suffix; `test_descent_attach` leaks two mmaps on a
  skip path.
