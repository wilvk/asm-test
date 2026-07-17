# asm-test — out-of-process inline-`using` whole-window (`new AsmTrace(outOfProcess: true)`): implementation plan

Make the crash-proof out-of-process whole-window capture usable as a **bare inline scope**

```csharp
using (var ww = new AsmTrace(outOfProcess: true))
{
    // …managed block… runs INLINE on this thread, single-stepped out of band
}                                    // Dispose ends the window, drains, attributes
Report.Print(ww);
```

in ADDITION to the delegate factory `AsmTrace.Window(() => { … })` that ships today. This is
**R4** of the companion [asmtrace-inline-using-plan.md](../../plans/asmtrace-inline-using-plan.md)
(§Roadmap, "build reluctantly, LAST") — the one trace form whose backend is COORDINATION-class
rather than START/STOP, promoted from roadmap sketch to a concrete spec.

> **Status: LANDED** (`578caed`, 2026-07-09). The inline-`using` OOP form ships as the
> `AsmTrace(bool outOfProcess, …)` ctor ([HwTrace.cs:1923](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L1923)),
> reusing `AsmTrace.Window`'s shared channel + attribution seam but swapping the delegate
> call-frame boundary for an async begin/stop split (`asmtest_hwtrace_stealth_window_begin` /
> `…_stop`). `AsmTrace.Window` (the delegate factory,
> [HwTrace.cs:1837](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L1837)) remains the recommended
> form per the note below.

> **Recommendation before building: this is strictly WORSE than the factory** — build only if a
> bare-`using` OOP scope is an explicit requirement. It buys ONLY the `using` syntax for zero
> capture-quality gain (same channel, same deep-BCL elision), and it costs real correctness
> surface (§Why-worse). Keep `AsmTrace.Window` the recommended form in all docs and examples.

## Why the OOP form can't already be a `using` (the COORDINATION problem)

The companion plan's two backend classes decide this. START/STOP forms (single-step
`EFLAGS.TF`, AMD-LBR / Intel-PT perf `ENABLE`/`DISABLE`) split trivially into ctor-arms /
Dispose-ends because arm and disarm are two independent operations with **no handshake and
nothing coordinating the block between them** — which is exactly why the AMD-LBR inline form
already shipped ([HwTrace.cs:1831](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L1831), native
`asmtest_hwtrace_sample_begin_amd` / `_end_amd`,
[hwtrace.c:1117](../../../../src/hwtrace.c#L1117) / [:1163](../../../../src/hwtrace.c#L1163)).

The out-of-process stepper is COORDINATION-class: a live helper child must single-step the
calling thread **while** it runs the block. Today the delegate's call frame *is* the window —
the helper `run_to`s the frame entry and ends when control returns to the caller
(`pc == win_ret`, [ptrace_backend.c:1981](../../../../src/ptrace_backend.c#L1981)). An inline
block has no call frame, so the window boundary has to become an **async stop-flag** the
managed `Dispose` raises, and the begin/end split re-exposes the ctor tail + Dispose head to
the stepper. That is the work below.

## Native layer — split `stealth_trace_windowed` into begin/stop/end

The shipped monolith
[`asmtest_hwtrace_stealth_trace_windowed`](../../../../src/hwtrace.c#L2574) does the whole dance
in one call: mmap shared scratch → fork helper → helper SEIZE+INTERRUPTs the caller,
publishes `ready`, `run_to`s `win_base` → caller calls `run_region()` (the delegate) → helper
steps until `pc == win_ret` → caller `waitpid`s + copies back. We keep it (node/java/dotnet
`stealthWindow` and the C `windowed_trace` oracle depend on it) and add a **begin/stop/end**
sibling beside it.

### 1. Scratch handshake fields — [src/stealth_helper.h:29](../../../../src/stealth_helper.h#L29)

Add two `volatile` flags to `asmtest_stealth_scratch_t` (both cross the fork boundary in the
same `MAP_SHARED` mapping as the existing `ready`/`rc`):

```c
volatile int stop; /* caller -> helper: end the async window at the next step */
volatile int done; /* helper -> caller: detached + shadow lens final; safe to read back */
```

`ready` (stepper→caller, already present) is reused unchanged: the helper still publishes it
after SEIZE+INTERRUPT so the caller knows the window is armed before it returns from `_begin`.

### 2. Stop-aware step loop — [src/ptrace_backend.c:1886](../../../../src/ptrace_backend.c#L1886)

The synchronous windowed stepper exits on `pc == win_ret`. Add a sibling
`asmtest_ptrace_trace_attached_window_stop(pid, chan, volatile int *stop, trace)` that shares
the identical inner body (drain channel, `PTRACE_SINGLESTEP`, forward non-SIGTRAP signals,
record `in_region_set` hits as ABSOLUTE addresses) but:

- **exit condition** = `*stop != 0` checked after each step's `waitpid` (instead of
  `pc == win_ret`); there is no frame and no `win_ret` read.
- **no `win_base`/`win_len` region** — an inline window records ONLY channel-published
  regions (the caller pre-publishes the managed code ranges, exactly as the factory does at
  [HwTrace.cs:2019](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L2019)). `in_region_set` is
  called with `win_len == 0`, so only the drained `regs[]` match.
- keep the `PTRACE_WINDOW_STEP_CAP` backstop and the non-SIGTRAP-forward logic verbatim (the
  crash-proof property lives there — a ptrace-stop is not gated by the tracee's signal mask,
  [ptrace_backend.c:1950](../../../../src/ptrace_backend.c#L1950)).

Factor the shared inner loop into a `static` helper so the two public variants (`_windowed`
frame-bounded, `_window_stop` flag-bounded) don't duplicate the record/decode logic.

### 3. Helper stepping body — [src/stealth_helper.c:116](../../../../src/stealth_helper.c#L116)

Add `asmtest_stealth_helper_run_window_async(sc, parent)` beside
`asmtest_stealth_helper_run_windowed`. Differences from the frame-bounded body:

- After SEIZE+INTERRUPT + `waitpid`, publish `sc->ready = 1` and **do NOT `run_to`** — the
  caller is INTERRUPT-stopped inside `_begin`; stepping starts from there.
- Loop `asmtest_ptrace_trace_attached_window_stop(parent, chan, &sc->stop, &sc->shadow)` — the
  first `PTRACE_SINGLESTEP` resumes the caller for one instruction (finishing `_begin`'s
  return glue) and steps forward through the managed block until the caller sets `sc->stop`.
- On loop exit: `PTRACE_DETACH`, set `sc->rc`, then `sc->done = 1` LAST (release-ordered after
  the shadow lens are final), then the helper `_exit(0)`.
- **Watchdog:** the existing `alarm(15)` bounds a hung *attach*. An inline block legitimately
  runs longer than a delegate leaf, so the async body must re-`alarm()` on progress OR raise
  the bound; document the ceiling. If the alarm fires mid-window the helper dies → `sc->done`
  is never set → the caller's `_end` spin (below) MUST also break on helper reap (see §4).

### 4. Begin / end public API — [include/asmtest_hwtrace.h:457](../../../../include/asmtest_hwtrace.h#L457)

Add beside the windowed monolith, documented in the header §D3 block. Mirror the AMD split's
`begin`→ctx→`end` shape ([:487](../../../../include/asmtest_hwtrace.h#L487)):

```c
/* §D3 out-of-process inline whole-window: BEGIN/STOP/END split of stealth_trace_windowed,
 * for the `using (new AsmTrace(outOfProcess:true)) { block }` shape (no delegate frame).
 * _begin forks the reverse-attach helper, which SEIZEs the CALLING thread and single-steps
 * it out of band from this point on; it returns a live ctx handle in *ctx_out with the
 * window ARMED (the block runs lexically after, on this thread, under the stepper). _end
 * raises the async stop flag, joins the helper, and copies the recorded ABSOLUTE-address
 * stream into *trace. Records only regions pre-published on `chan`. Call _end on the SAME
 * thread that called _begin (the helper targets that tid). Crash-proof (ptrace-stops ignore
 * the tracee's signal mask). Returns OK / EINVAL / EUNAVAIL (reverse-attach refused — a
 * clean self-skip; the block then runs uninstrumented). Linux x86-64/AArch64. */
int asmtest_hwtrace_stealth_window_begin(asmtest_addr_channel_t *chan, void **ctx_out);
int asmtest_hwtrace_stealth_window_end(void *ctx, asmtest_trace_t *trace);
```

`_begin` ([src/hwtrace.c](../../../../src/hwtrace.c), beside the monolith at :2574): allocate the
shared scratch + shadow buffers (same layout as the windowed path), `prctl(PR_SET_PTRACER_ANY)`,
`parent_tid = syscall(SYS_gettid)`, `fork`; child runs
`asmtest_stealth_helper_run_window_async(sc, parent_tid)` then `_exit`. Parent busy-waits
`while (!sc->ready)` with a `waitpid(helper, …, WNOHANG)` liveness check (identical to
[hwtrace.c:2621](../../../../src/hwtrace.c#L2621)); on helper death / `rc == EUNAVAIL` it reaps,
`munmap`s, returns `EUNAVAIL`. Otherwise heap-alloc a ctx `{ helper_pid, sc, total, icap, bcap }`,
`*ctx_out = ctx`, return `OK`. **On return the caller is already under single-step** — every
instruction until `_end` is stepped (that is the disclosed pollution, §Why-worse).

`_end`: set `sc->stop = 1`; spin `while (!sc->done && waitpid(helper, …, WNOHANG) != helper)`
(break on the helper reaping itself, so an alarm-killed helper can't hang the caller); final
`waitpid(helper, …, 0)`; if `sc->rc == OK` copy `sc->shadow` insns/blocks + `truncated` into
`*trace` (the same read-back as [hwtrace.c:2643](../../../../src/hwtrace.c#L2643)); `munmap`; free
ctx. The spin itself runs under step until the helper detaches — it is native `.so` glue,
unpublished, so it is stepped-*over*, not recorded.

Provide the `#else` no-op stubs (non-Linux / non-x86-64-aarch64) returning `ASMTEST_HW_ENOSYS`,
matching the monolith's [hwtrace.c:2682](../../../../src/hwtrace.c#L2682) guard.

## Managed layer — a DISTINCT ctor + Dispose branch

Per the companion plan's R4 mandate: **a distinct ctor, never a peer bool on the backend-keyed
`AsmTrace(HwBackend)` ctor** — its lifecycle (a live helper context + stop→join→read-back) is a
different teardown from the in-band arm-a-register / drain-a-buffer forms.

### 5. `new AsmTrace(bool outOfProcess)` — [HwTrace.cs:1487](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L1487)

- Add `readonly bool _oopInlineWindow;` and `IntPtr _oopWinCtx; IntPtr _oopWinChan;` fields
  beside `_oopWindow` ([:1493](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L1493)). Follow the
  minimal-flag precedent set by `_amdWindow`
  ([:1494](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L1494)); the `_kind`-enum cleanup (R1) is
  recommended-first but not required.
- Ctor `AsmTrace(bool outOfProcess, bool byMethod = true, bool withRundown = true, int
  rundownSettleMs = 300, [CallerMemberName]…, [CallerLineNumber]…)`: guard `outOfProcess ==
  true` (else route to the empty-ctor whole-window via `SkipReason`); set up `_map` + rundown
  (same pre-window prep as the factory ctor [:1963](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L1963));
  self-skip with `SkipReason` when `!HwNative.LibAvailable` or `!Ptrace.Available()`; allocate
  the shared channel + publish `EnumerateManagedCodeRanges()`
  ([:2019](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L2019)); `Thread.BeginThreadAffinity()`;
  call `asmtest_hwtrace_stealth_window_begin(chan, out ctx)`. On `OK` store the ctx + chan, set
  `_oopInlineWindow = true`, `Armed = true`. On `EUNAVAIL` self-skip (tear down map/chan/rundown,
  `Armed = false`, directive `SkipReason`) so the block runs uninstrumented.
- **DO NOT** pre-JIT via `PrepareDelegate` (there is no delegate); instead document that the
  first-call JIT of methods the block invokes is stepped-through (the same live-publish gap the
  factory discloses at [:2021](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L2021)).

### 6. Dispose branch — [HwTrace.cs:2416](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L2416)

Add an `_oopInlineWindow` arm before the `_wholeWindow` block, mirroring the `_amdWindow` arm
([:2429](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L2429)):

```csharp
if (_oopInlineWindow)
{
    if (Armed) {
        var tr = HwTrace.Create(blocks: 4096, instructions: 65536);
        int rc = HwNative.asmtest_hwtrace_stealth_window_end(_oopWinCtx, tr.Handle);
        Thread.EndThreadAffinity();                 // paired with the ctor's pin
        if (rc == HwNative.ASMTEST_HW_OK) {
            // read back ABSOLUTE addresses; reuse AttributeAddresses(img, when) — the shared
            // seam that branches on IsStatistical (false here) to fill Methods/Addresses.
        }
        tr.Free();
    }
    if (_oopWinChan != IntPtr.Zero) HwNative.asmtest_addr_channel_free_shared(_oopWinChan);
    if (_map != null) { _map.Stop(); _map.Dispose(); }
    if (_rundownRequested) DiagnosticsIpc.DisablePerfMap();
    return;
}
```

Note the ordering asymmetry vs. the OOP factory: the factory's `Dispose` is a no-op because
`RunWindowOutOfProcess` already closed the scope and set `_disposed`
([:1988](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L1988)); here `Dispose` is where the window
actually ENDS. `_oopInlineWindow` scopes are **must-dispose** (a leaked scope leaves the helper
stepping until its watchdog fires) — document it on the ctor like the AMD form's must-dispose
contract ([:1827](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L1827)), and add a finalizer that
force-`_end`s to bound a leak.

### 7. P/Invokes — the `HwNative` block

Add `asmtest_hwtrace_stealth_window_begin(IntPtr chan, out IntPtr ctx)` and
`asmtest_hwtrace_stealth_window_end(IntPtr ctx, IntPtr trace)` next to the existing
`stealth_trace_windowed` import.

## Parity gate

Two new tier symbols land in `include/asmtest_hwtrace.h`, so the bindings-parity gate
([scripts/check-bindings-parity.sh](../../../../scripts/check-bindings-parity.sh)) will demand each
of the ten bindings either wrap them or carry an exemption. Add two `ALL` lines to
[scripts/bindings-parity-allow.txt](../../../../scripts/bindings-parity-allow.txt), same posture as
the AMD `sample_begin_amd` / `sample_end_amd` pair ([allow.txt:81-82](../../../../scripts/bindings-parity-allow.txt#L81)):

```
ALL asmtest_hwtrace_stealth_window_begin   # begin/end split of the windowed stealth stepper for the inline `using (new AsmTrace(outOfProcess:true))` shape: dotnet wraps the pair (ctor calls _begin, Dispose calls _end); other nine gain it if they grow an inline OOP scope
ALL asmtest_hwtrace_stealth_window_end      #   (paired with stealth_window_begin above)
```

dotnet wraps the pair; the other nine reach the windowed capture C-internally and gain the
inline shape only if they grow one. Run `scripts/check-bindings-parity.sh` after — a green gate
means "wrapped or documented-exempt," not feature parity (the roadmap framing in
[dotnet-parity-roadmap.md](dotnet-parity-roadmap.md)).

## Why this is strictly worse than `AsmTrace.Window` (state it, don't hide it)

1. **Harness-frame pollution.** The factory's delegate call frame isolates stepping to exactly
   the block. The inline split single-steps from inside `_begin`'s return through the ctor tail,
   the whole block, and into `Dispose`/`_end` — so the ctor's own JIT'd managed tail and
   Dispose's head land in the recorded stream (they are in the JIT heap, a published coarse
   range). The factory records neither.
2. **Thread-affinity across an arbitrary block.** The helper targets one OS tid; the inline form
   must hold `BeginThreadAffinity` across the entire `using` body. An `await` / thread hop inside
   the block moves execution off the stepped thread — the window then captures the wrong thread's
   glue. The `_armTid` hop-guard flags it (never crashes), but the factory's synchronous delegate
   sidesteps it structurally.
3. **Zero capture-quality gain.** Same shared channel, same coarse-range publish, same
   deep-BCL-elision limit (live per-method publish stays OFF — firing the EventPipe callback on
   the stepped thread re-enters the runtime under step and `SIGABRT`s,
   [HwTrace.cs:2021](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L2021)). The inline form buys the
   `using` keyword and nothing else.

So: ship it as an ergonomic alternative for callers who require a bare `using`, keep
`AsmTrace.Window` the documented recommendation, and never let the unified ctor swallow it behind
a peer bool.

## Tests

- **C oracle** (`examples/` beside [windowed_trace.c](../../../../examples/windowed_trace.c), wired
  like the `windowed-trace` target [native-trace.mk:351](../../../../mk/native-trace.mk#L351)):
  `stealth_window_inline` — `_begin(chan)` over a channel publishing two native leaves, run a
  block that calls both **plus spawns and joins a thread in-window** (the exit-133 hazard the
  in-process single-step tier dies on), `_end`, assert (a) both leaves appear in the recorded
  absolute-address stream, (b) the process SURVIVED the in-window `pthread_create` (crash-proof
  proof — the whole point), (c) clean `EUNAVAIL` self-skip when ptrace is refused (Yama).
- **dotnet** ([HwTraceTest / an example](../../../../examples/dotnet/)): the inline
  `using (new AsmTrace(outOfProcess:true)) { SpawnThreadInWindow(); }` beside the factory in
  [crashproof-showdown](../../../../examples/dotnet/crashproof-showdown/Program.cs) — assert it
  survives (parity with the factory's SAFE leg) and captures ≥ the leaf, and assert the disclosed
  pollution honestly (the recorded stream is a superset of the block). Self-skips off ptrace.
- **CI:** runs on any ptrace-capable Linux (a `--cap-add=SYS_PTRACE` docker lane, no PT
  hardware), x86-64 and AArch64. qemu-user cannot `PTRACE_SINGLESTEP`, so the arm64-under-qemu
  lane self-skips (see the arm64-docker-ptrace memory / decision matrix). Extend the existing
  `docker-hwtrace` fan-out; no new shared lib.

## Effort & risks

- **~2–3d.** Native begin/stop/end split + async helper body (~1–1.5d, the shared-loop refactor
  is the care point), managed ctor + Dispose branch + finalizer (~0.5–1d), C + dotnet tests
  (~0.5d).
- **Risk — the stop→detach handshake.** The caller executes `_end` (writing `stop`) *while being
  stepped by the helper*; the helper must observe `stop` at its next step and the caller must not
  race past the read-back before the helper's shadow lens are final. `done` (release-ordered
  after the lens writes, acquire-read by the caller) is the barrier; the helper-reap escape in
  the `_end` spin is the watchdog-death backstop. Get this ordering wrong and you either hang the
  caller or read a torn trace.
- **Risk — don't regress the synchronous windowed path.** node/java/dotnet `stealthWindow` and
  the `windowed_trace` oracle use the monolith; the stop-aware loop must be a *sibling*, sharing
  an inner `static` helper without changing the `pc == win_ret` frame-bounded behavior.
- **Must-dispose.** A leaked inline scope leaves the helper stepping the process until `alarm()`
  fires — measurable slowdown. Finalizer force-`_end` + a documented contract, as the AMD inline
  form does.

## Sources

- Companion roadmap (this is its R4): [asmtrace-inline-using-plan.md](../../plans/asmtrace-inline-using-plan.md)
  §Roadmap; the shipped AMD-LBR inline split is the START/STOP template.
- Windowed stealth monolith to split: [src/hwtrace.c:2574](../../../../src/hwtrace.c#L2574),
  helper body [src/stealth_helper.c:116](../../../../src/stealth_helper.c#L116), step loop
  [src/ptrace_backend.c:1886](../../../../src/ptrace_backend.c#L1886) (exit at :1981).
- AMD begin/end template: [src/hwtrace.c:1117](../../../../src/hwtrace.c#L1117) /
  [:1163](../../../../src/hwtrace.c#L1163); header [asmtest_hwtrace.h:487](../../../../include/asmtest_hwtrace.h#L487).
- Managed OOP factory to mirror: [HwTrace.cs:1803](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L1803)
  (`Window`), [:1986](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L1986) (`RunWindowOutOfProcess`),
  Dispose seam [:2416](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L2416).
- Parity gate: [scripts/check-bindings-parity.sh](../../../../scripts/check-bindings-parity.sh),
  [scripts/bindings-parity-allow.txt](../../../../scripts/bindings-parity-allow.txt).
- Live crash-proof contrast the inline form must match: [examples/dotnet/crashproof-showdown/Program.cs](../../../../examples/dotnet/crashproof-showdown/Program.cs).
