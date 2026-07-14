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

## Option-1 sweep — DR options + versions (all NO-GO, 2026-07-14)

Before concluding, a bounded sweep tested whether a DR runtime option or a different DR release
flips the result (the probe is the go/no-go gate; `PROBE_DROPS` / `PROBE_CLIENT_ARGS` on
`make dr-taint-managed-attach-probe` parameterize it). **Every cell is NO-GO** — the running
`dotnet` never survives the takeover:

| DR version | baseline (counting client) | `noinstr` (seize only, zero instrumentation) |
|---|---|---|
| **11.91.20630** (pinned) | stack-smashing → SIGSEGV (rc 139) | **stack-smashing → SIGSEGV (rc 139)** |
| **11.91.20644** (newest cronbuild) | stack-smashing → SIGSEGV (rc 139) | **stack-smashing → SIGSEGV (rc 139)** |
| **11.3.0** (newest stable) | dies rc 255 (no canary msg) | dies rc 255 (no canary msg) |

DR runtime-option variants on the pinned DR (baseline counting client unless noted):

| DR option | result |
|---|---|
| `-no_mangle_app_seg` | **no "stack smashing" message**, but still SIGSEGV (rc 139) |
| `-no_mangle_app_seg` + `noinstr` | no "stack smashing" message, still SIGSEGV (rc 139) |
| `-thread_private` | stack-smashing → SIGSEGV (rc 139) |
| `-native_exec` | stack-smashing → SIGSEGV (rc 139) |

Whole-process **SIGSTOP-then-attach** (`make dr-taint-managed-attach-probe PROBE_SIGSTOP=1`, `kill
-STOP` ALL threads — native GC/JIT/finalizer included — before the seize, `kill -CONT` after) — the
test of whether the crash is a seize-time *race* on live threads vs a fundamental arbitrary-*state*
problem. Result (both `noinstr` and counting): **NO-GO, and it fails EARLIER + differently** — DR
prints `ERROR: unable to attach; check pid and system ptrace permissions` (the client is never
delivered, `client_reached=0`), i.e. **DR's ptrace-seize refuses a group-stopped target**, so you
cannot even use whole-process freezing as a pre-attach quiesce step. And freezing the live CLR for a
few seconds then resuming it **destabilized the runtime regardless** — the victim aborted (`rc 134
SIGABRT`) even though DR never took over. So freezing does not rescue managed attach; it removes the
ability to attach at all and perturbs the runtime on its own.

Three findings that sharpen the diagnosis and rule out the easy fixes:

1. **It is the SEIZE, not the instrumentation.** The `noinstr` control takes the process over
   with **zero** bb-instrumentation (DR still seizes every thread + builds its code cache, but
   adds no clean calls) and crashes **identically** (same canary trip, same SIGSEGV). So the per-
   instruction counting client is not the cause — DR *taking the running .NET runtime over* is.
2. **`-no_mangle_app_seg` is a partial-but-insufficient lead.** Disabling DR's app-segment
   (`%fs`/`%gs`) mangling **removes the stack-canary trip** (the "stack smashing detected" message
   disappears) — confirming the `%fs`-relative canary read *was* one facet of the takeover damage —
   **but the process still SIGSEGVs**, just without that specific symptom. So segment/TLS handling
   is one layer of a **deeper arbitrary-state-takeover incompatibility**, not the whole problem.
3. **No DR release helps.** The newest cronbuild (11.91.20644) is byte-identical in behavior; the
   newest stable (11.3.0) fails *differently* (exit 255, no canary message) but still never
   survives. External attach into a .NET runtime is not a version-specific regression.

**Conclusion: Option 1 (cheap DR-option / version experiments) is exhausted — NO-GO.** The failure
is fundamental (seizing arbitrary managed thread state), not a narrow segment bug or a bad build.
The only credible path left was **Option 2: park all managed threads at GC-safe points *before* the
seize** — planned + then EXECUTED as a spike
([dynamorio-managed-attach-safepoint-plan.md](../plans/dynamorio-managed-attach-safepoint-plan.md)).

## Option-2 safepoint spike — EXECUTED, also NO-GO (2026-07-14)

The spike went end to end and **closed the question with direct evidence**:

- **Increment 1 (GO):** a co-loaded CLR profiler (`ICorProfilerInfo10::SuspendRuntime`/
  `ResumeRuntime`, `examples/suspendprof_probe/`) cycles the runtime 5/5 times clean — natively AND
  under `drrun -c <taint client> -- dotnet` (`make dr-suspendprof-test`). The suspension primitive
  is sound and survives DR coexistence.
- **Increment 2 (NO-GO — the make-or-break):** `make dr-suspendprof-attach-test` parks the running
  victim with `SuspendRuntime` (hr=0, all managed threads at GC-safe points), then DR-attaches the
  **parked** process. The client is delivered (`dr_client_main` runs) — and the seize **still trips
  stack-smashing → SIGSEGV (rc 139), IDENTICAL to the baseline above**; `ResumeRuntime` never runs,
  no heartbeat advances past the seize. **Parking managed threads changed nothing.**

This is decisive: the fatal takeover is **not** of the managed thread states (those were parked and
it crashed identically) — it is the **native runtime threads** DR also seizes (GC / JIT / finalizer
/ diagnostics-IPC), which a managed-only suspend cannot park. The ICorDebug fallback would fail for
the same reason (it also stops only managed threads), so it was not pursued. **Both options are
exhausted; managed data-flow/taint of an already-running process stays on launch-under-DR /
ptrace-attach (out-of-band).**

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
