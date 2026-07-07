# asm-test — Managed single-step posture: design proposal (the open decision)

The one design decision the 2026-07-06 review of the
[managed tier](scoped-tracing-managed-plan.md) left open, now forced by hard
evidence: **what contract should in-process EFLAGS.TF single-step offer against
a managed runtime's threads?**

> Status legend: **decided (2026-07-07) — B + C-fallback + D approved and
> implemented.** This document stays as the rationale + options analysis; the
> approved posture (lazy-arm the body in-process, out-of-process fallback for the
> signatures the shim set can't cover, the honesty note, and the pinned-worker
> mitigation retained) is built and validated in
> [managed-singlestep-lazy-arm-plan.md](managed-singlestep-lazy-arm-plan.md) — with
> one item still open there: slow-host (throttled-CI) confirmation of the
> crash-avoidance property. Options are ordered by increasing invasiveness; the
> recommendation is at the end.

## The forcing evidence (2026-07-07, `4c26de6`)

A TF-armed thread that executes code which **blocks `SIGTRAP` is killed by the
kernel** — not truncated, not degraded, killed. glibc's `pthread_create` blocks
all signals around `clone()`; the `#DB` raised by the next instruction is then a
synchronous signal delivered while blocked, which the kernel force-resets to
`SIG_DFL` (`force_sig_info` semantics) — no handler runs, exit 133. This is not
theoretical: CoreCLR's tiered-compilation background worker idle-exits (~4 s
default) and is **respawned via `pthread_create` on whichever thread next
enqueues JIT work**. On GitHub-hosted runners the §D0.3 named-method scope's
TF-stepped `DynamicInvoke` machinery kept the window open 7+ seconds and died
deterministically (dmesg: RIP one instruction past `rt_sigprocmask` inside
`pthread_create`, `EFLAGS.TF` set); on a fast Zen 5 host the same window closes
in milliseconds and the crash never reproduces. Discriminators, each verified in
a CI matrix: `DOTNET_TieredCompilation=0` green, worker-kept-alive green,
reflection-path prewarm and frozen call-count both still dead.

Two prior facts frame the same posture question:

- Cross-thread `Dispose` of an in-process scope leaves the armer stepping
  (SIGTRAP crash) — the B5 managed-tid guard turns it into `Truncated`.
- The whole-window managed form is documented best-effort and self-truncating;
  the guide already says the faithful managed paths are out-of-band (PT/LBR) or
  the §D3 out-of-process stepper.

**What ships today (the landed mitigation):** the .NET self-test/example lanes
pin the tiering worker resident (`hwtrace_dotnet_env`,
`DOTNET_TC_BackgroundWorkerTimeoutMs=36EE80`), and the constraint is documented
on `asmtest_hwtrace_begin_scope` and in the hardware-tracing guide. That
protects the suites; a **user's** app arming a long in-process window on a slow
host remains exposed.

## The options

### A — Status quo: document + pin the suites *(landed 2026-07-07)*

Keep in-process arming as-is; the constraint is documented; suites pin the
worker. Zero new code.

- **Protects:** the repo's own lanes; informed users who set the env.
- **Does not protect:** default-config user apps. The failure mode is the worst
  kind — works on the dev box, kills the process in production CI.
- **Cost:** none further.

### B — Lazy-arm around a native-dispatched call (make `AsmTrace.Method` sound)

The §D0.3 scope's promise is "trace this method's body" — the runtime machinery
around it was never part of the contract, and stepping it is both the crash
surface and 99% of the stepped instructions. Restructure the in-process
`Invoke`:

1. **Resolve/prepare everything before arming** (already mostly true):
   `PrepareMethod` on the target (full-tier, no counting stub, no OSR
   patchpoints), plus a **non-generic delegate shim per supported arity**
   (`delegate long AsmBody2(long, long);` …) created via
   `Delegate.CreateDelegate` over the same method, so
   `Marshal.GetFunctionPointerForDelegate` can mint the reverse-P/Invoke stub
   **pre-arm** (generic `Func<>` delegates cannot be marshaled).
2. **Dispatch through C:** a small helper (`asmtest_hwtrace_call_scoped(name,
   fnptr, args…)` — or reuse of the N4 `asmtest_capture_args` shims' calling
   machinery) arms TF, `call`s the raw pointer, disarms, all in native code.
   The armed window then contains: the reverse-P/Invoke transition (runtime
   native code, not tier-counted), the prepared body, and nothing else.
3. `begin_scope`/`end_scope` collapse into the helper call; `Dispose` keeps
   rendering from the trace as today (the frame's normalized trace survives the
   pop).

- **Protects:** every `Method()` user by construction — managed machinery is
  never TF-stepped, so no in-window JIT enqueue, no worker respawn, no blocked
  window. Also erases the 7-second stepped-reflection overhead.
- **Residual:** signatures outside the shim set (ref/out, structs, >N args)
  either keep the old DynamicInvoke-in-window path (documented residual risk)
  or fall through to §D3/oop (see C). The GC can still run in-window if the
  body allocates — suspension via the activation signal is safe (the kernel
  clears TF on handler entry; worst case a dropped TF = truncated trace, not
  death).
- **Cost:** medium — shim table + C helper + tests across the recorded stream
  parity (`Method()` output must stay byte-identical: same region filter, same
  offsets).

### C — Out-of-process by default for managed scopes (§D3 as the default route)

`AsmTrace.Method(..., outOfProcess: true)` already exists and is immune by
construction (the thread is never TF-armed). Flip the default for managed
targets: in-process becomes the opt-in (`inProcess: true`) for users who accept
the documented constraint.

- **Protects:** everyone, including exotic signatures, with today's code.
- **Cost:** behavioral — needs Yama ptrace permission and the bundled helper;
  attach latency per invoke; the §D3 whole-window live-JIT address channel is
  the heavier machinery. Self-skips (uninstrumented run) where attach is
  refused — a silent fidelity downgrade the in-process form doesn't have.

### D — Guardrail telemetry (cheap, orthogonal)

`DegradationNote()`/`SkipReason` already compose the honest-degradation ladder.
Add one sentence when an in-process managed scope arms: "in-process TF window
on a managed thread — fatal if the runtime spawns a thread in-window; pin the
tiering worker (`DOTNET_TC_BackgroundWorkerTimeoutMs`) or use
`outOfProcess: true`." Zero protection, full honesty.

## Recommendation

**B for `Method()` scopes, C as the fallback for signatures B's shims don't
cover, D immediately, A stays regardless.** B is the only option that keeps the
in-process form's advantages (no ptrace, no helper, exact offsets, works in any
container) while removing the crash surface *by construction* rather than by
configuration. The whole-window managed form (`new AsmTrace()` on a runtime
thread) cannot be made sound this way — arbitrary code runs in-window by
definition — so it keeps posture A+D: documented best-effort, now with the
fatal-constraint warning.

Suggested phasing if B is approved:

1. **B0** — D's `DegradationNote` sentence + a `docs/bindings/dotnet.md` caveat
   (1 evening).
2. **B1** — the C helper (`arm → call fnptr → disarm` with the existing region
   frame) + host test over a native leaf (no managed runtime needed to validate
   the mechanics).
3. **B2** — the .NET shim table (common arities: `(long…)->long`, void,
   double variants mirroring the N4 mixed-FP work) + `Invoke` rewiring +
   stream-parity tests on .NET 8/9 lanes.
4. **B3** — decide the uncovered-signature route: DynamicInvoke-with-documented-
   risk vs. auto-fallback to §D3. (Recommend auto-fallback with a
   `SkipReason`-style note.)

## Non-goals

- Intercepting `pthread_create` (LD_PRELOAD) or unblocking `SIGTRAP` from the
  handler: fighting the kernel's `force_sig` contract is not a posture, it's a
  time bomb.
- Making whole-window managed capture crash-proof: impossible in-process by the
  §Z1 form's own definition; PT (§Z2) is the strong tier for that, hardware
  permitting.
