# F4 GC-fence FREEZE probe — findings (live-attach data-flow tier)

**Verdict: the freeze assumption is FALSE (2026-07-16), and a second, larger hazard sits behind
it.** F4's stamping rests on the claim that *"during a GC fence the EE is fully suspended, so a
ptrace-single-stepped managed thread retires ZERO instructions across the compaction, meaning the
tracer's step counter is frozen and can simply be read at drain time"*. Measured on the pinned .NET 8
SDK, Linux x86-64, with the step counter sampled **inside the victim by an attached profiler** at
both ends of every fence:

| measurement | result |
| --- | --- |
| **S1 − S0 over GC windows** (`GarbageCollectionStarted..Finished`) | **non-zero on 47 of 47** (3 runs) — never once zero |
| distribution of S1 − S0 | min **342**, max **558**, mean **≈423** instructions |
| **S1 − S0 over the EE-suspended window** (`RuntimeSuspendFinished..RuntimeResumeStarted`) | **non-zero on 47 of 47**; min **354**, max **588** |
| `GarbageCollectionStarted` → first `MovedReferences2` (the actual compaction point) | **129..239** instructions |
| traced thread's state during the fence | **BLOCKED** — `futex_do_wait`, syscall **202**, 73/152 fence samples |
| where those retired instructions were | `libcoreclr` + `libc`; **`jit_code_heap=0`** — *no managed code at all* |
| **GC fences that never completed while stepping** | **every GC** in the allocating phase; ~1 per burst even in the compute phase, stalling **~19.8 s** (the whole burst) |

The freeze assumption's **premise is right and its conclusion is wrong**: the thread really is
*blocked in a futex*, not spin-waiting at a GC poll — and it **still** retires ~420 instructions
across the fence, because **single-stepping a futex-blocked thread is what un-blocks it**. Each
ptrace stop interrupts the wait; the step restarts it; the runtime's wait loop retires a handful of
instructions per lap. The tracer's own instrument perturbs the thing it assumed was frozen.

So **drain-time stamping is unsound** and F4 must stamp `asmtest_gcmove_t.step` with the
**profiler-sampled S0** captured at `GarbageCollectionStarted`. As the F4 brief anticipated, S0
stamping is correct either way, so this **refines** the design rather than killing it. See
[What this means for F4](#what-this-means-for-f4) — including a caveat that cuts the other way.

**The bigger finding is the STALL**, which dominates the S1−S0 question and was not on the risk
list at all: **a GC fence very often cannot complete *at all* while a managed thread is being
single-stepped**. The runtime enters `RuntimeSuspendStarted`, fails to park the stepped thread, and
hammers it with `SIGRTMIN` activation injections indefinitely — never reaching
`RuntimeSuspendFinished`. The GC, and therefore **every allocating thread in the process**, stalls
until the tracer detaches, at which point suspension completes in milliseconds. Measured stalls of
**19.8 s**, ending exactly at detach.

Reproducible: identical verdicts across **three** consecutive full runs (phase A `MOOT`/stall every
time, stalling 19.8–20.0 s; phase B `FALSE` with **16/16, 10/10 and 21/21** non-zero deltas, max 554,
529 and 558). The victim **survived every run** (`rc=0`, no fatal signal) — a live .NET process tolerates profiler-attach *plus*
ptrace-single-stepping of a managed thread *plus* detach.

Reproduce with:

```
make docker-gcfence-probe      # the only way on a host without the .NET SDK; runs both phases
make gcfence-probe             # host-direct; self-skips without dotnet/git; needs CAP_SYS_PTRACE
```

## The question

F4 consumes `MovedReferences2` `{old, new, len}` triples "stamped with the value-trace step boundary
the compaction takes effect at" and runs `asmtest_gcmove_canonicalize` over a captured trace. That
boundary is `asmtest_gcmove_t.step` ([asmtest_valtrace.h:320](../../../include/asmtest_valtrace.h#L320)),
which is **only an index into `insn_off[]`** — how many in-region instructions the tracer has
recorded so far, stamped by `_append`. The design assumed the GC fence freezes that counter, so the
boundary could be read after the fact, at drain time.

That assumption deserved measurement rather than deference. CoreCLR parks threads at GC-safe points,
but *how* matters: a thread that **blocks** retires nothing and the claim holds; a thread that
**spin-waits** at a GC poll retires instructions and drain-time stamping is silently wrong. The
probe was built to measure it, not to confirm it — and a non-zero result was defined up front as a
valuable outcome, since S0-stamping is robust either way.

## What was probed

Deliberately **no DynamoRIO** — this is the out-of-band ptrace tier. The lane is the .NET SDK + a
C/C++ toolchain + git (the pinned CoreCLR profiler headers, shared with the
gcprofiler/attachprof probes) and, unlike [the attach probe](f4-attach-profiler-probe-findings.md),
**`--cap-add=SYS_PTRACE`**, because the stepper ptrace-attaches to a *sibling* process.

- [examples/gcfence_probe/gcfenceprof.cpp](../../../examples/gcfence_probe/gcfenceprof.cpp) — the
  measuring **attach-mode** profiler; `attachprof.cpp`'s sibling (same `CINTERFACE` C-vtable +
  generic-stub array-fill, same strict QI, same `InitializeForAttach` entry) plus the sampling of
  the tracer's step counter at both ends of both windows. Everything it does inside a GC callback is
  integer loads/stores on an already-mapped shm page: no allocation, no locks, no calls into the
  runtime.
- [examples/gcfence_probe/gcfence_stepper.c](../../../examples/gcfence_probe/gcfence_stepper.c) — a
  minimal standalone ptrace stepper. It deliberately does **not** drag in
  [src/dataflow_ptrace.c](../../../src/dataflow_ptrace.c): the whole tier brings region gating, JIT
  method resolution and signal policy, none of which the question needs.
- [examples/gcfence_probe/gcfence_shm.h](../../../examples/gcfence_probe/gcfence_shm.h) — the
  probe-local shm channel, modelled on the DR tier's
  [asmtest_taint_gcmove.h](../../../include/asmtest_taint_gcmove.h) but necessarily separate: that
  tier is *in-process* with its target (so it can publish a function pointer), whereas here the
  tracer and the profiler are in **different processes**. The DR tier's shipping header is untouched.
- [examples/gcfence_probe/victim/](../../../examples/gcfence_probe/victim/) — a **plain** dotnet
  process (`env -u CORECLR_ENABLE_PROFILING …`, enforced not assumed), with a managed hot loop on a
  worker thread that publishes its own `gettid`, plus a main thread forcing **compacting** gen2 GCs.
- [examples/gcfence_probe/attacher/](../../../examples/gcfence_probe/attacher/) — the
  `DiagnosticsClient.AttachProfiler` harness.

### The two windows, kept apart

`GarbageCollectionStarted..Finished` is **not** the window in which the EE is suspended, and
conflating them is how one would get this question wrong. So the probe measures both:
`COR_PRF_MONITOR_SUSPENDS` (also in `COR_PRF_ALLOWABLE_AFTER_ATTACH`; `SetEventMask(0x40080)`
returns **S_OK** post-attach) delivers `RuntimeSuspendFinished` / `RuntimeResumeStarted`, i.e. the
window where the EE is *literally, fully* parked — the assumption's own wording. Both windows give
the same verdict, and the EE-suspended delta is consistently the larger of the two (it contains the
GC window) — an internal consistency check the numbers pass on every fence.

### Two false zeros that had to be designed out

Both would have manufactured exactly the answer the design hoped for:

1. **A window that opens or closes while the tracer is not stepping reads a *dead* counter at that
   end**, and a dead counter at both ends reports S1−S0 = 0. An early build reported a
   confident-looking `delta=0` that was purely this artifact. Only windows with `traced=1
   traced_close=1` are counted; everything else is excluded and reported separately.
2. **A window the tracer joins late** has an S0 from before the tracer existed. Hence bursts, each
   begun only when no GC window is open — which selects *when* stepping starts, never what the fence
   then does.

### Ordering, as instructed

The profiler is attached **first** (its attach travels the diagnostics IPC socket, which the runtime
must be *running* to service), the ptrace stepper second. **Stepping one thread does not interfere
with the diagnostics thread servicing the attach**: the stepper attaches to the single worker TID,
so every other thread — including the diagnostics thread — keeps running natively. No re-ordering
problem was observed in any run.

## The measurement

Two phases, because the traced worker's shape gives **genuinely different answers** and neither may
stand for the other. Both are run by `make gcfence-probe`.

### Phase B — pure-compute managed hot loop: the fence is NOT frozen

This is the phase that answers the question. The worker parks within ~1.4–2.4k stepped instructions,
fences complete while stepping, and S1−S0 is measurable. Every measured fence, across both runs:

```
run 1 (16 fences)  GC window  S1-S0: 451 486 549 408 542 554 364 377 407 383 342 343 430 361 348 400
                   EE-susp    S1-S0: 469 508 573 429 555 575 389 403 423 399 354 370 452 374 359 424
run 2 (10 fences)  GC window  S1-S0: 358 347 515 367 353 491 529 438 517 358
                   EE-susp    S1-S0: 372 356 545 383 367 505 544 456 548 376
```

**26 of 26 non-zero. Not one zero** — and a third run reproduced it at 21/21 (max 558 / 588). Tight
and stable: 342–558 (GC window), 354–588 (EE-suspended), mean ≈423. The compaction itself is **129–239 instructions after `GarbageCollectionStarted`**, so
even a stamp taken at the *fence start* is a few hundred instructions early relative to the moves —
irrelevant to F4 (the whole fence is one boundary) but worth knowing.

### Blocked or spinning? — BLOCKED, and it retires instructions anyway

The cheap diagnostic turned out to be the explanation. During the EE-suspended window the traced
thread's `/proc/<tid>/` state is:

```
wchan: futex_do_wait=73  ptrace_stop=32  0=47
syscall (202 = futex): 202=73  -1=55  running=24
```

It is genuinely **parked in a futex** — CoreCLR does *not* spin-wait at the GC poll, so the premise
behind the assumption was sound. The instructions retire anyway, and the RIP samples say where:

```
GCFENCE_WHERE rip_samples=965 libcoreclr=265 libc=420 jit_code_heap=0
```

**Zero samples in the JIT code heap.** The ~420 instructions per fence are the runtime's futex wait
loop in `libcoreclr`/`libc`, executed *because we are stepping the thread through it*: each ptrace
stop interrupts the wait, each step restarts it. The counter advances not despite the thread being
blocked but **because our instrument is dragging a blocked thread around the loop it is blocked in**.

### Phase A — allocating managed hot loop: no fence completes at all

With an ordinary allocating worker the result is not a number but a stall. In both runs:

```
measured_gc_windows=0  measured_ee_windows=0  stalled_windows=1  stall_max_us=19,815,282
signals forwarded to the traced thread: sig34=9246   (SIGRTMIN — CoreCLR's activation injection)
steps_per_signal=470   rip: libcoreclr=916 libc=78 jit_code_heap=0
```

`RuntimeSuspendStarted` fires, and `RuntimeSuspendFinished` **never arrives** — for the entire 20 s
burst, across the phase's 4.3M single-steps. The whole process's GC stalls; the victim's main thread
sits in `GC.Collect` for the duration (its heartbeat gaps confirm it: `round=14 t=2.8` →
`round=15 t=15.0` under a 12 s burst); suspension completes within milliseconds of the tracer
detaching.

Four facts are directly measured, in both runs: `RuntimeSuspendFinished` never arrives; a `SIGRTMIN`
storm is delivered to the stepped thread throughout; **not one** of the ~1–2k RIP samples lands in
the JIT code heap, so the thread never gets back to its own code and therefore never reaches a safe
point; and the fence closes immediately on detach. The mechanism they imply is a **livelock between
CoreCLR's activation retry rate and the cost of running that handler one instruction at a time** —
the thread is consumed by activation handling and never makes forward progress. The absolute
cadence is load-dependent rather than fixed (run 1: ~470 steps/signal at ~105k steps/s; run 2: ~2025
at ~220k), but the *ratio* is the invariant: in both runs the interval between injections is about
what the stepped thread needs to service one, and managed-code progress is nil either way.

This is not confined to the pathological phase: even in phase B, one suspension per burst falls into
the same livelock (~19 s, ending at detach) after the first handful of GCs succeed.

## What this means for F4

1. **Stamp with the profiler-sampled S0, not at drain time.** Directly measured: 47/47 fences
   advanced the counter. `asmtest_gcmove_t.step` must be stamped inside `GarbageCollectionStarted`,
   by the profiler, from the shm counter — which is exactly what this probe already does, so the
   mechanism is proven, not merely proposed. The magnitude (~420) is small but is not a rounding
   error: it is *hundreds of `insn_off[]` entries* of misattribution per compaction, straddling
   exactly the pre/post-move boundary the transform exists to get right.
2. **A caveat that cuts the other way, and must not be leaned on.** Every retired instruction was in
   `libcoreclr`/`libc` and **none in managed code**. If the real tier's counter is *region-gated* —
   `insn_off[]` only advancing for in-region managed instructions — then those instructions would
   never reach `step`, and drain-time stamping might survive by accident. That is a property of the
   tier's gating, not of the fence, and it is one region-definition change away from silently
   breaking. S0-stamping is exact under both, costs nothing, and is already built. Prefer it.
3. **The stall is an availability hazard for the whole live-attach managed tier, and it is bigger
   than the stamping question.** Any GC that wants to run while a managed thread is stepped may hang
   the entire process until the tracer detaches — not just the traced thread, but every allocating
   thread. This is not an F4 problem; F4 merely surfaced it. It bounds what the ptrace live-attach
   tier can responsibly do to a managed process: step in **short bursts** whose length is well under
   the target's GC interval, and treat a suspension that opens mid-region as a signal to let go.
   Worth confirming against `src/dataflow_ptrace.c`'s actual region residency before F4 wiring.
4. **The unglamorous good news.** The victim survived profiler attach + managed-thread
   single-stepping + detach in every run (`rc=0`, `GCFENCE_VICTIM_END` reached, no fatal signal) —
   notably unlike the DR managed-attach route, which took SIGSEGV at takeover
   ([dr-managed-attach-probe-findings.md](dr-managed-attach-probe-findings.md)). Attaching a
   profiler and stepping a managed thread of a live .NET process is *safe*; it is merely
   *slow enough to livelock the GC*.

## Surprises worth recording

- **The premise was right and the conclusion still wrong.** "Blocked, therefore zero" is the
  intuition the whole design rested on. The thread *is* blocked. The zero still does not follow,
  because the measuring instrument is what makes it retire. A probe that had checked only
  blocked-vs-spinning would have confidently confirmed the assumption.
- **`GarbageCollectionStarted..Finished` is not the suspended window** — it is strictly *inside* it.
  Measuring only the GC callbacks would have measured the wrong window and never known.
- **The allocating worker is the *harder* case, not the easier one.** The intuition is backwards: a
  pure-compute loop has no GC poll of its own and might be expected to resist suspension, yet it
  parks in ~1.5k instructions, while the allocating loop — full of allocation-helper polls — never
  parks at all. The alloc helper puts its RIP inside `libcoreclr`, where activation cannot be
  injected, and the retry storm does the rest.
- **The first build "measured" a clean `delta=0`** and would have reported the assumption as HOLDING.
  It was reading a dead counter at both ends. The `traced_close` filter exists because of it.
