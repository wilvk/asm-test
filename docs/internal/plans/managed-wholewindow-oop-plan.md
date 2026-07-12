# asm-test — §D3 whole-window out-of-process channel: design proposal

The crash-proof route to tracing a **whole block** of managed code — a scope wrapped
around arbitrary C# (LINQ, exceptions, generics, `pthread_create`-adjacent tiering
work), not a single registered leaf. It is the one investment that turns the
already-shipping out-of-process stepper from a **region** primitive into a
**whole-window** capture the in-process single-step tier is *forbidden* to attempt.

> **Status update (2026-07-08): W-0/W-1/W-3 LANDED; W-2 LANDED for native fixtures.**
> The native substrate this plan called "net-new" **already shipped** and is live-tested:
> `asmtest_ptrace_trace_attached_windowed` (region-free absolute-PC capture across a window
> frame + channel-published regions) with the `PTRACE_STREAM_CAP` budget → `truncated`
> (W-0/W-3), and the cross-process JIT-address channel
> ([asmtest_addr_channel.h](../../../include/asmtest_addr_channel.h)) — both covered by
> `test_ptrace_windowed`. **Added 2026-07-08 (this task):** the fork-internal
> `asmtest_ptrace_trace_window_call` (the plan's W-0 "fork path" — owns its tracee, so a
> caller that cannot fork safely gets a whole-window trace with no attach/run_to
> bookkeeping), exported `asmtest_addr_channel_{new,publish_rec,free}` FFI shims,
> `test_ptrace_window_call` (5 checks, green in `make hwtrace-test`), the .NET
> `Ptrace.TraceWindowCall` + `AddrChannel` wrappers, and the
> [examples/dotnet/localscope_oop](../../../examples/dotnet/localscope_oop/) demo
> (validated live, captures a frame + two published leaves out-of-process). **Remaining
> forward-look:** W-1 BTF block-step *windowed* variant (per-instruction windowed ships;
> the `PTRACE_SINGLEBLOCK` windowed path is not yet wired), and a **live-CoreCLR** W-2
> (`new AsmTrace(outOfProcess: true)` self-attach via the stealth helper + the runtime's
> `MethodLoadVerbose` listener publishing to the channel) — the native fixture proves the
> mechanism; wiring a managed runtime's live JIT addresses in is the remaining integration.
>
> **Status update (2026-07-08b): the MANAGED whole-window path LANDED (partial).**
> `asmtest_hwtrace_stealth_trace_windowed` (a reverse-attach helper child single-steps the
> CALLING thread — SYS_gettid, not the process leader — out of band, capturing the whole
> window + a SHARED address channel), the fork-valid shared channel
> (`asmtest_addr_channel_new_shared`), the windowed loop's step backstop + **forward-all-
> signals** hardening (a managed runtime RAISES AND HANDLES SIGSEGV as normal operation, so
> treating faults as terminal truncated the window on the runtime's first internal fault),
> and the managed `AsmTrace.Window(() => { …block… })` + a `test_stealth_windowed` C test
> (green in `make hwtrace-test`, 296 checks) all ship. The
> [examples/dotnet/localscope_oop_managed](../../../examples/dotnet/localscope_oop_managed/)
> demo runs a whole block of managed C# out-of-process, **crash-proof** — it survives an
> in-scope thrown/caught exception the in-process `localscope` must omit (validated: exit 0,
> deterministic, the block's own JIT'd code named).
>
> **The one remaining gap — deep mid-window JIT attribution — CLOSED 2026-07-12**
> *(extensions plan [E3](asmtrace-extensions-plan.md)): `JitMethodMap.SetPublishChannel`
> now arms exactly the sibling-thread publisher described below (EventPipe callback →
> lock-free queue → never-stepped publisher thread → shared channel, joined before the
> channel is freed); `AsmTrace.Window` reports it via `LiveJitPublished`. The paragraph
> below records the pre-E3 rationale.* Coarse `/proc/self/maps`
> ranges (JIT heap + R2R BCL `.dll` images) are pre-published, so the block's OWN code + the
> already-mapped BCL are captured; but a method JIT'd FRESH mid-window (a first-call generic
> instantiation like `Enumerable.Where<int>`, a local function) lands OUTSIDE the pre-window
> ranges and is elided. The fix is the LIVE per-method publish (the `MethodLoadVerbose`
> listener → shared channel). It is BUILT (`JitMethodMap.SetPublishChannel` +
> `asmtest_addr_channel_publish_rec` in `OnEventWritten`) but **left OFF**: firing the managed
> EventPipe callback ON the thread being single-stepped re-enters the runtime under step and
> ABORTS it (SIGABRT, observed). The safe form is to publish from a SIBLING thread (drain the
> runtime's `MethodLoad` events off a second thread that is not stepped, and publish those to
> the channel) — the documented follow-up. Until then the OOP managed window is honest-partial:
> crash-proof + own-code, not full deep-BCL parity with the in-process form.
>
> This document is the design + phasing; the sections below are the original proposal,
> now largely landed per the updates above.
> Related: [managed-singlestep-posture-plan.md](../archive/plans/managed-singlestep-posture-plan.md)
> (why in-process whole-window cannot be crash-proof),
> [scoped-tracing-managed-plan.md](scoped-tracing-managed-plan.md) (§D3 is item 7's
> "concealed out-of-process ptrace stepper scope"),
> [zen2-singlestep-trace-plan.md](zen2-singlestep-trace-plan.md) Phase 5 (the W2 ptrace
> tier this extends).

## The forcing problem

The in-process whole-window form (`new AsmTrace()` on a managed thread — what
[examples/dotnet/localscope](../../../examples/dotnet/localscope/) demonstrates) is
**best-effort by construction and cannot be made crash-proof** (posture plan §"forcing
evidence"): any code that blocks `SIGTRAP` — glibc `pthread_create` around `clone()`,
the CLR's two-pass exception dispatch — turns the next single-step `#DB` into a masked
synchronous fault the kernel force-resets to `SIG_DFL` (exit 133). No `sigaction` flag
beats `force_sig`; interposition is a time bomb (posture plan §Non-goals).

The **out-of-process** stepper does not have this failure mode *at all*: it drives the
tracee with `PTRACE_SINGLESTEP`/`PTRACE_SINGLEBLOCK`, and the `#DB` is delivered to the
**tracer** via `waitpid` — a ptrace-stop is **not** gated by the tracee's signal mask
([src/ptrace_backend.c](../../../src/ptrace_backend.c) `~1264-1299`). So the tracee may
freely block `SIGTRAP` and never dies. This is exactly why the
`ptrace_dotnet`/`blockstep`/`descent_dotnet` examples call themselves "the scenario
in-process single-step is forbidden to do."

**The gap:** that stepper today traces a **region** — a registered `[base, len)`
method body resolved by name from the perf-map (`asmtest_ptrace_trace_attached` +
`run_to`). It does not have a "record *everything* the thread runs between two points,
by absolute address, and resolve the managed methods afterward" mode. Building that —
the **§D3 whole-window live-JIT address channel** — is this plan.

## What already ships (the substrate this builds on)

- **Out-of-process steppers.** `asmtest_ptrace_trace_attached[_blockstep|_versioned]`
  ([asmtest_ptrace.h](../../../include/asmtest_ptrace.h)) — exact per-instruction or
  per-taken-branch (BTF) capture of a region in a foreign/attached process; reads
  target bytes via `process_vm_readv`; `run_to` plants a breakpoint to reach a method
  entry with no cooperative go-flag.
- **Self-attach without an external harness.** `asmtest_hwtrace_stealth_trace`
  ([src/stealth_helper.c](../../../src/stealth_helper.c)) spawns a bundled helper
  **child** that `prctl(PR_SET_PTRACER)` + `PTRACE_SEIZE`es back onto its parent — so a
  process traces *itself* out-of-process, in a plain unprivileged container. This is
  what `AsmTrace.Method(outOfProcess: true)` already rides.
- **Time-versioned code image.** `asmtest_codeimage_*`
  ([asmtest_codeimage.h](../../../include/asmtest_codeimage.h)) records a timestamped
  byte timeline so a stepper decodes against the bytes that were **live when the code
  ran** — correct under re-JIT / address reuse.
- **Managed method resolution.** `asmtest_proc_perfmap_symbol` (perf-map by name) and
  `asmtest_jitdump_find` (binary jitdump, code bytes + load timestamps); and, on the
  .NET side, the `JitMethodMap` `MethodLoadVerbose` listener + `DiagnosticsIpc` rundown
  (`HwTrace.cs`) that already turn a captured **absolute** address stream into named
  methods for the *in-process* whole-window form.

The observation that makes this tractable: the **attribution half already works
whole-window** (the in-process form captures absolute addresses and `_map.Resolve`s
them). Only the **capture half** needs to move out-of-process.

## The design

A whole-window out-of-process scope = *stepper capturing absolute addresses on the
managed thread, for a whole window, driven from a second observer, with the tracee's
signal mask irrelevant.* Four pieces:

1. **Region-free absolute capture (native).** A new
   `asmtest_ptrace_trace_window_attached(pid, tid, &trace, ...)` that steps the target
   thread from a start marker to an end marker with **no `[base, len)` filter**,
   appending each stop's absolute PC to `trace->insns[]` (the same shape
   `begin_window` fills in-process). BTF block-step
   (`PTRACE_SINGLEBLOCK`) is the default driver — ~4–10× fewer stops than
   per-instruction, byte-identical after `blockstep_reconstruct`, and the only thing
   that makes a million-instruction window remotely affordable out-of-process.
2. **Window delimiting without a cooperative flag.** `begin`/`end` plant breakpoints
   (`run_until`) at the scope's entry/exit return addresses in the managed caller, so
   the observer knows the exact `[start, end)` of the window on the target thread —
   reusing `run_to`'s breakpoint-rewind mechanics. The managed side marks the window by
   calling two tiny library thunks (the reverse-P/Invoke landing sites), so no source
   cooperation beyond the `using` block is needed.
3. **Thread targeting.** The window rides **one** managed thread (the arming thread, as
   the in-process form already requires). The observer attaches that tid specifically;
   sibling GC/tiering threads run free (an `int3` on a thread the tracer does not own
   kills the process — the same rule the HotSpot lane already handles by attaching the
   right tid). This side-steps the whole `pthread_create` hazard: the tiering worker
   can spawn freely because it is never stepped.
4. **Attribution reuse.** After close, feed the absolute `insns[]` through the existing
   `JitMethodMap` + jitdump rundown (whole-window attribution already implemented in
   `HwTrace.cs`), decoding against the `asmtest_codeimage` version live at each stop.

The result is a whole-window scope that (a) survives every managed feature by
construction, (b) reuses the shipped attribution, and (c) is honest — BTF-degraded
hosts and dropped stops self-truncate exactly as the region path does.

## Opt-in surface (where it makes sense)

Out-of-process capture costs a ptrace round-trip per stop (~100–1000× the stepped
thread's native speed) and needs ptrace permission (Yama `ptrace_scope`, or the
`PR_SET_PTRACER` self-attach helper). It must therefore be **opt-in**, never a silent
default that changes a whole-window scope's cost/permission profile:

- **.NET:** `new AsmTrace(outOfProcess: true, ...)` on the whole-window ctor —
  mirroring the flag `AsmTrace.Method(outOfProcess:)` already exposes. Default stays
  the in-process best-effort WEAK tier (works anywhere, no ptrace), with the
  `DegradationNote`/`SkipReason` ladder pointing at this flag when the in-process form
  would be fatal. Self-skips cleanly (records `SkipReason`) where ptrace is denied — a
  documented fidelity downgrade the in-process form does not have.
- **C API:** a distinct entry point (`asmtest_ptrace_trace_window_attached`), not a
  mode bit on `begin_window`, so the in-process fast path is untouched and the
  permission/toolchain requirements are explicit at the call site.

## Phasing

1. **W-0 — native region-free absolute capture.** `trace_window_attached` over a
   *native* fixture (no managed runtime needed): fork a child that runs a known blob,
   step the whole window absolute, assert the absolute-address stream matches the
   in-process `begin_window` result byte-for-byte. Host-testable in a plain container
   (`make hwtrace-test`), honoring "no untested hardware code."
2. **W-1 — BTF block-step driver + delimiting.** Add the `PTRACE_SINGLEBLOCK` path and
   the begin/end breakpoint delimiting; assert identical stream vs W-0 at ~1 stop per
   taken branch; self-skip where a hypervisor masks `DEBUGCTL.BTF`
   (`blockstep_available`).
3. **W-2 — .NET whole-window `outOfProcess: true`.** Wire the ctor flag through the
   stealth self-attach helper; capture a whole inline block (the localscope body) on
   the managed thread; fold absolute addresses through the existing `JitMethodMap` +
   rundown; a companion example (`examples/dotnet/localscope_oop_managed/`) proving the
   caught exception that crashes the in-process form is captured cleanly (LANDED; deep
   mid-window JITs elided — see the live-publish follow-up). `localscope_oop/` is the
   native-fixture demo of the same primitive.
4. **W-3 — cost controls + honesty.** An instruction/stop budget with self-truncation
   (a ~1M managed window is seconds out-of-process — bound it and flag `truncated`
   rather than hang), BTF-degrade / dropped-stop truncation parity, and the
   `SkipReason` wording for ptrace-denied hosts.

## Non-goals

- **Making the in-process whole-window form crash-proof.** Impossible by the §Z1
  form's definition (posture plan). This plan is the crash-proof *alternative*, not a
  fix to the in-process path — which stays as documented best-effort.
- **Whole-*program* (all-threads) capture.** The window is one managed thread by
  design; multi-thread out-of-process stepping is a separate, much larger effort and is
  not required for a scoped block.
- **Exact capture of an arbitrarily large window as the default.** Even out-of-process,
  a million stops is slow; W-3 bounds it. The point is *reliability and reach for
  bounded managed blocks*, not an unbounded profiler (that is the DynamoRIO tier's job).

## Validation

Every phase self-validates on hardware asm-test actually has (plain x86-64 Linux,
unprivileged container) — W-0/W-1 against native fixtures with no managed runtime, W-2
against a live CoreCLR the way the `ptrace_dotnet` lane already does. The BTF path
carries the existing `blockstep_available` functional probe so a `DEBUGCTL.BTF`-masking
hypervisor self-skips rather than silently degrading. No step depends on Intel PT or a
perf-permitted host — the whole point of the out-of-process route is that it runs where
the strong hardware tiers do not.
