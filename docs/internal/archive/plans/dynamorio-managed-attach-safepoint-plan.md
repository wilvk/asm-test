# asm-test — DynamoRIO MANAGED attach via safepoint coordination: implementation plan

Make DR **external attach** work on an **already-running .NET (dotnet) process** by parking
every managed thread at a **GC-safe point before the seize**, then taking DR over, then
resuming — the one credible path left after the Increment-6 probe closed the naive approach.

This is the **Option-2** follow-on to
[dynamorio-attach-tier-plan.md](dynamorio-attach-tier-plan.md) Increment 6, whose probe
([dr-managed-attach-probe-findings.md](../../analysis/dr-managed-attach-probe-findings.md))
returned a definitive **NO-GO**: `drrun -attach <dotnet-pid>` *delivers* the client
(`dr_client_main` runs) but the takeover immediately crashes the process
(stack-canary trip → SIGSEGV rc 139), and an **Option-1 sweep** proved it is not fixable by
DR options or DR version — **it is the SEIZE of arbitrary managed thread state, not the
instrumentation** (a zero-instrumentation `noinstr` seize crashes identically;
`-no_mangle_app_seg` removes the `%fs` canary symptom but the process still dies; no DR
release helps).

> **Status: SPIKE EXECUTED 2026-07-14 — Increment 1 GO, Increment 2 NO-GO -> Option 2 CLOSED (the
> hypothesis is REFUTED).** The suspension primitive works (Inc 1: `SuspendRuntime`/`ResumeRuntime`
> cycle cleanly natively AND under a coexisting DR), but a runtime with **all managed threads parked
> at GC-safe points still crashes IDENTICALLY** at the DR seize (Inc 2: `SuspendRuntime` succeeded
> hr=0, `drrun -attach` delivered the client, then stack-smashing -> SIGSEGV rc 139 — the SAME crash
> as the Increment-6 baseline, `resumed=0`, no heartbeat past the seize). So safepoint coordination
> does **not** make managed external attach viable: the crash is the **native** runtime threads DR
> also seizes (GC/JIT/finalizer/diagnostics), which a managed suspend cannot park — exactly the risk
> called out below. Increment 3 was not reached. **Managed stays launch-under-DR** (taint tier
> Increment 5, landed) **/ ptrace-attach** (out-of-band, the
> [live-attach-dataflow plan](live-attach-dataflow-plan.md)) — the accepted outcome, not a failure.
> Findings: [dr-managed-attach-probe-findings.md](../../analysis/dr-managed-attach-probe-findings.md).
> Lanes: `make docker-suspendprof-probe` (Inc 1 `dr-suspendprof-test` + Inc 2
> `dr-suspendprof-attach-test`).

---

## The hypothesis

DR's attach seizes every thread via ptrace and redirects it through the code cache. The
Increment-6 evidence is that a **.NET runtime thread caught at an arbitrary point** (deep in the
GC, the JIT, tiered compilation, a P/Invoke transition, a hijacked return) does not survive that
redirection — its stack / TLS / guard / return-address-hijack invariants are inconsistent with
what DR's takeover assumes (the `%fs`-relative stack canary is merely the first casualty).

The .NET runtime already has machinery to put **all managed threads at GC-safe points** and hold
them there: that is exactly what a **runtime suspension** (a GC, an EE suspend, a profiler
`SuspendRuntime`, a debugger stop) does. **Hypothesis:** if the managed threads are parked at
GC-safe points *at the instant DR seizes*, the states DR redirects are the known-good ones the
runtime itself designed for stop-the-world, and the takeover survives.

This is a hypothesis, not a fact — parking managed threads does not park the *native* runtime
threads, and DR still has to seize those. So the spike must prove it empirically, cheaply, before
any capture work rides on it.

## What is already proven in-repo (the reusable scaffolding)

- **A native CLR profiler co-loads and runs under DR on Linux** — `examples/gcprofiler_probe/`
  (`ICorProfilerCallback4::MovedReferences2`) already coexists with the taint client under
  `drrun -c … -- dotnet` (`make docker-gcprofiler-probe`, taint tier Increment 7). A profiler is
  the vehicle for `SuspendRuntime`/`ResumeRuntime`.
- **The .NET diagnostics IPC channel** is understood here — the hwtrace dotnet work used it, and
  it is how a profiler can be **attached to an already-running process** (`AttachProfiler` over
  `/tmp/dotnet-diagnostic-<pid>-*-socket`), preserving the tier's "attach to a process I did not
  start" contract.
- **The managed-attach probe** (`make dr-taint-managed-attach-probe`, Increment 6) is the ready
  **go/no-go gate** — every increment below re-runs it (or a variant) as its exit test.
- **The taint client + shm channel + out-of-process validator** are attach-agnostic and reused
  verbatim once (if) the seize survives.

---

## Increment 1 — Suspension primitive under DR (safe, launch-first) *(**GO 2026-07-14** — `dr-suspendprof-test`)*

> **RESULT — GO.** `make dr-suspendprof-test` (`examples/suspendprof_probe/`): a co-loaded profiler
> (`ICorProfilerInfo10::SuspendRuntime`/`ResumeRuntime` on a profiler-created native thread) cycled
> the runtime **5/5 times clean, both natively and under `drrun -c <taint client> -- dotnet`** —
> every `SuspendRuntime` returned S_OK, the managed victim's heartbeats advanced across each cycle,
> the process exited rc 0 with no crash/hang. The suspension primitive is sound and survives DR
> coexistence, so the suspend-then-seize ordering (Increment 2) was worth building.

Prove the suspension primitive works **before** touching attach. Under the SAFE launch path
(`drrun -c <taint client> -- dotnet <victim>`, where DR is already in from a clean start), a
co-loaded profiler calls `ICorProfilerInfo::SuspendRuntime()` then `ResumeRuntime()` on a nudge,
in a loop, while the managed victim runs its hot method — and the process must survive (no
SIGSEGV/SIGTRAP/hang), managed work resuming each cycle.

- Extend the `gcprofiler_probe` scaffold with a suspend/resume path (its own callback + a trigger).
- Exit: N suspend/resume cycles under DR launch, clean, managed output intact.
- Kill: if `SuspendRuntime` under DR-launch coexistence already crashes/hangs, the primitive is
  unusable and Option 2 is dead here (record it; stay launch/ptrace). **Effort: M.**

## Increment 2 — Suspend-then-seize ordering harness (the core experiment) *(**NO-GO 2026-07-14** — hypothesis refuted; `dr-suspendprof-attach-test`)*

> **RESULT — NO-GO (the make-or-break).** `make dr-suspendprof-attach-test`: the profiler
> `SuspendRuntime`'d the running victim (hr=0, all managed threads parked at GC-safe points, a
> "suspended" sentinel published), the harness DR-attached the PARKED process (`drrun -attach`,
> noinstr) and the client was delivered (`dr_client_main` ran) — but the seize **still tripped
> stack-smashing -> SIGSEGV (rc 139), IDENTICAL to the Increment-6 baseline**; `ResumeRuntime` never
> ran (`resumed=0`), no heartbeat advanced past the seize. **Parking managed threads did not help at
> all** — decisive evidence that the fatal takeover is of the **native** runtime threads (GC / JIT /
> finalizer / diagnostics-IPC), which `SuspendRuntime` does not park. Per the kill criterion, Option
> 2 is exhausted here; the ICorDebug fallback below would fail for the same reason (it also only
> stops managed threads), so it was not pursued. Managed stays launch/ptrace.

The choreography, and the crux. On an already-running `dotnet`:

1. Attach the suspend-capable profiler to the running victim via the **diagnostics-IPC
   `AttachProfiler`** command (or, as a first cut to de-risk ordering only, load it via
   `CORECLR_PROFILER*` env at victim start).
2. On a nudge, the profiler `SuspendRuntime()` — all managed threads park at GC-safe points — and
   signals "suspended" over a shm/pipe handshake.
3. The external injector then `drrun -attach <pid>` **while the runtime is suspended**.
4. Once `dr_client_main` is up (DR has seized), signal the profiler to `ResumeRuntime()`.
5. Re-run the Increment-6 probe's survival checks: does the managed process now **survive the
   seize** (heartbeats advance past attach, no fatal signal), and detach cleanly?

- This is a *modified* managed-attach probe — same victim + counting client, plus the profiler +
  the suspend/attach/resume ordering. The `noinstr` control still applies (isolate seize vs work).
- Exit: the probe flips to **GO** — the suspended-at-seize `dotnet` survives DR external attach +
  detach. Kill: it still crashes at seize even when parked → the hypothesis is wrong (native
  runtime threads, or the seize itself, are the problem); **record NO-GO and stop** — Option 2 is
  exhausted, managed stays launch/ptrace. **Effort: XL / research** — the make-or-break increment.

## Increment 3 — Managed seed→sink over the attached, resumed process *(NOT REACHED — gated on Increment 2 GO, which failed)*

Compose the survival with capture: attach + suspend + seize + resume, then the reused taint client
seeds a managed buffer and the branch-condition sink fires, drained + validated out of process
(the taint tier's managed seed→sink, but *attached* instead of launched). Reuses the shm channel +
validator + atomic report verbatim; the new surface is only the suspend/attach/resume wrapper.

- Exit: a `drrun -attach <dotnet-pid>` window captures a seed→sink over a managed buffer and the
  process continues + detaches native, validated out of process. **Effort: L (if 2 is GO).**

---

## Fallbacks (if the profiler `SuspendRuntime` path fails Increment 2)

- **ICorDebug (managed-debugger) suspend.** The CLR debugger interface stops the process at
  safepoints natively. Risk: ICorDebug + DR both want the process's ptrace/debug state — a probable
  two-debuggers conflict. Evaluate only if the profiler suspend is *insufficient* (parks managed
  but not enough of the runtime) rather than *conflicting*.
- **Accept the NO-GO.** The sanctioned managed-already-running path is **ptrace-attach + emulator
  replay** ([live-attach-dataflow-plan.md](live-attach-dataflow-plan.md)) — out-of-band, never
  seizes the runtime into a code cache, structurally immune to this crash (value/def-use, not
  in-band taint). This is the honest default and is *not* a failure of this tier.

## Risks and open points

- **Native runtime threads are not parked by a managed suspend.** `SuspendRuntime` parks *managed*
  threads at GC-safe points; the GC/finalizer/JIT/IPC native threads keep running and DR still
  seizes them mid-flight. If the crash is a *native*-thread seize, safepoint parking cannot fix it —
  this is the most likely way Increment 2 returns NO-GO.
- **Seizing the profiler/suspender thread itself.** DR attaches to *all* threads, including the one
  holding the suspension. The handshake must leave the runtime in a state where DR taking over the
  suspender thread does not deadlock resume.
- **Two ptrace clients.** The diagnostics-IPC profiler attach is in-process (no ptrace), so it does
  not conflict with DR's ptrace-seize — but ICorDebug (the fallback) would.
- **This may simply not be viable** — an accepted outcome (the parent Increment-6 kill criterion),
  not a failure. Managed stays launch/ptrace.

## Recommended first milestone

*(As written, before the spike ran. Retained for provenance — the sequence below is exactly what
happened, and its final branch is the one that fired.)*

**Increment 1** (suspend/resume under DR *launch*) — it is safe (no attach, no experimental seize),
reuses the proven `gcprofiler_probe` coexistence, and answers the cheapest prerequisite question —
*can the runtime even be suspended/resumed under a DR that is already coexisting?* — before the
expensive Increment-2 ordering experiment. If Increment 1 is clean, Increment 2 is the make-or-break
go/no-go; if Increment 2 stays NO-GO, close Option 2 and keep managed on launch/ptrace.

**Outcome 2026-07-14: Increment 1 was clean (GO), Increment 2 stayed NO-GO — so Option 2 is CLOSED
and managed stays on launch/ptrace.** The cheap-prerequisite-first ordering did its job: Increment 1
cost little and isolated the make-or-break question, and Increment 2 answered it decisively enough
that Increment 3 was never reached and the ICorDebug fallback was not worth pursuing (it stops only
managed threads, so it would fail for the same reason). **No further work is planned or recommended
on this plan** — it is a closed research spike, not an open track. The sanctioned managed-
already-running path is ptrace-attach + emulator replay
([live-attach-dataflow-plan.md](live-attach-dataflow-plan.md), Increment 1 landed).
