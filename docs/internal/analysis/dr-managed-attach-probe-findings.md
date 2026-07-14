# DR managed-attach probe — findings (DR ATTACH tier, Increment 6)

**Verdict: NO-GO (2026-07-14).** DynamoRIO's EXPERIMENTAL external attach to an
**already-running .NET (dotnet) process** does **not** survive on the pinned DR
**11.91.20630** on Linux x86-64. `drrun -attach <pid> -c <client>` **delivers and starts
the client** inside the running managed process (`dr_client_main` runs — attach injection
itself works, exactly as for a native target), but the **takeover then immediately trips
glibc stack-smashing detection and the process dies with SIGSEGV (rc 139)** — no managed
heartbeat progresses past the seize. This is the plan's Increment-6 **kill criterion**:
a trivial `dotnet` process cannot survive DR external attach + detach, so **managed stays
on launch-under-DR / ptrace** (both already landed / planned). Do not force it.

Reproducible: identical behavior across two runs (same crash signature, same `rc=139`).

## What was probed

The Increment-2 native external-attach probe move, for **managed**. Artifacts (all
throwaway, not product — a NO-GO is a valid research finding):

- `examples/managed_attach_victim/` — a plain `dotnet` process (a long-running managed
  heartbeat loop whose hot `Work` method tiers up tier-0 → tier-1 mid-run), started FIRST
  and left running natively; prints `MANAGED_VICTIM_START pid=<pid>` + periodic
  `MANAGED_VICTIM_HEARTBEAT` + `MANAGED_VICTIM_END`.
- `drclient/attach_probe.c` — the **same minimal `drmgr`-only counting client the native
  probe used, reused VERBATIM** (counts instrumented instructions executed, prints
  `ATTACH_PROBE_TAKEOVER_OK` iff non-zero). The takeover question is orthogonal to the
  taint client, so the minimal client isolates *"does managed survive DR takeover"* from
  any taint-specific machinery.
- `make dr-taint-managed-attach-probe` / `make docker-taint-managed-attach-probe` — builds
  the managed victim, starts it, warms the JIT (~3 s, counting pre-attach heartbeats),
  parses its self-reported pid, injects DR + the client into the RUNNING pid via
  `drrun -attach`, holds ~5 s, `drconfig -detach <pid>` (reaping the lingering injector —
  the Increment-5 lesson), waits for the victim, and prints `MANAGED ATTACH PROBE OK` (GO)
  iff the client reached `dr_client_main`, instrumented a non-zero count, the victim's
  heartbeats advanced past the detach (return-to-native), it reached `MANAGED_VICTIM_END`,
  and there was no fatal signal — else `MANAGED ATTACH PROBE NO-GO` with the failure mode.
- `Dockerfile.taint-managed-attach-probe` — DR + the .NET SDK + the CMake toolchain (no
  Capstone/Unicorn); run with `--cap-add=SYS_PTRACE`.

Measured (containerized, `--cap-add=SYS_PTRACE`, `ptrace_scope=1`):
`client_reached=1 takeover_ok=0 pre_beats=80 attach_beats=80 detach_beats=80
alive_after_detach=0 victim_end=0 crash_text=1 fatal=1 victim_rc=139 (SIGSEGV)`.

The victim log tail is the whole story:
```
MANAGED_VICTIM_HEARTBEAT beat=79 acc=...
ATTACH_PROBE: dr_client_main reached (attach delivered the client into the running victim)
*** stack smashing detected ***: terminated
```

## What this means (the failure mode)

- **Attach injection is not the blocker — takeover is.** The client is delivered and
  `dr_client_main` runs (so `dr_inject_process_attach` / `drrun -attach` reaches into the
  managed process just as it does a native one). The process dies *after* that, during the
  seize/takeover of the running runtime.
- **The crash is a stack-canary trip (`__stack_chk_fail` → "stack smashing detected") that
  resolves to SIGSEGV (rc 139).** This is consistent with DR seizing the .NET runtime's
  threads at **arbitrary points** — a managed process has many runtime threads (GC, tiered
  compilation, finalizer, JIT) that at any instant may be mid-way through code with stack /
  TLS / guard-page invariants that DR's takeover (redirecting each thread through the code
  cache) does not preserve. This is precisely the hazard the plan flagged: a clean managed
  attach "really wants safepoint coordination — parking managed threads at GC-safe points
  before takeover," and the prior managed+tracing SIGTRAP/SIGSEGV history
  ([[dotnet-scoped-tracing-review]], [[java-stealth-sigtrap]], [[go-fulltest-flaky-crash]])
  is a live warning.
- **No managed instruction progresses past the seize** (heartbeats frozen at 80 across the
  attach and the attempted detach), and `event_exit` never runs (so `takeover_ok=0`) — the
  process is gone before any instrumented-and-cleanly-exited execution.

## Kill criterion — exercised

The plan's Increment-6 kill criterion is met exactly: *"if a trivial `dotnet` process
cannot survive DR external attach + detach without swallowing a .NET signal or crashing,
record the finding and stay with launch-under-DR / ptrace-attach for managed — do not force
it."* It crashed at takeover, reproducibly. So:

- **Managed data-flow / taint stays on the LANDED launch-under-DR path**
  (`drrun -c <taint client> -- dotnet …`, taint-tier Increment 5, exit-crit-3 managed
  seed→sink) and the (planned) ptrace-attach path — both of which own the process from a
  clean start or coordinate out-of-band, avoiding arbitrary-state takeover.
- **Native external attach is unaffected** — it is GO and fully landed (Increments 2-5;
  [dr-attach-probe-findings.md](dr-attach-probe-findings.md)). This NO-GO is scoped to
  **managed** targets, as the plan scoped it.

## Not pursued (the XL / research tail — "may not land")

The plan lists approaches to *evaluate* for a clean managed attach; the kill criterion
triggers on the trivial case failing, so these were deliberately **not** chased (this was
a bounded go/no-go spike, not the XL research effort):

- Request a runtime pause via **.NET diagnostics IPC** (the channel the hwtrace dotnet work
  used) so threads are parked at GC-safe points *before* the seize.
- `DR_SIGNAL_DELIVER` pass-through for the runtime's own SIGSEGV/SIGTRAP handlers.
- A `-late` / delayed injection posture.

Any of these is a research-grade effort with no guarantee of success on this DR; the
managed default staying launch/ptrace is an **accepted outcome, not a failure** (the plan's
own framing). If revisited, start from the diagnostics-IPC safepoint-park approach, and
re-run this probe as the go/no-go gate.
