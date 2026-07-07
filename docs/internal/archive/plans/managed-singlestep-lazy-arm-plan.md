# asm-test — Managed single-step: the lazy-arm implementation (Option B + C-fallback + D)

Implementation plan for the posture the
[managed single-step decision doc](../../plans/managed-singlestep-posture-plan.md)
recommended and that is now approved: **B** (lazy-arm around a native-dispatched call)
as the in-process default for `AsmTrace.Method()`, **C** (out-of-process stepper) as the
automatic fallback for signatures B cannot cover, **D** (a runtime honesty note)
immediately, and **A** (the pinned-worker suite mitigation) retained regardless.

> **COMPLETE (2026-07-07) — archived.** All of B0–B3, C-fallback, D, and A shipped and
> are green in CI, and **Sharpening 1 is now validated**: the unpinned tiering-worker
> stress lane (`make hwtrace-dotnet-stress`, the `hwtrace-bindings` CI job) passed on a
> real loaded GitHub runner — the exact environment where the old stepped-`DynamicInvoke`
> path died with exit 133. Extended since: the **(double…)->double FP shim family**
> (`asmtest_hwtrace_call_scoped_fp`, §B2) so double-signature methods trace in-process
> instead of falling back out-of-process. The remaining forward-look items (mixed int/FP
> signatures, per-binding adoption of `call_scoped`) are tracked as ordinary follow-ups,
> not part of this plan's committed scope.

> Status legend: **landed** — built and host-tested; **landed (managed-lane)** —
> exercised by the .NET Docker lane; **validated (slow-host)** — Sharpening 1, now
> confirmed on a loaded CI runner; **forward-look** — deferred, named so the seams show.

## Why (one paragraph, full evidence in the decision doc)

An in-process EFLAGS.TF window over a *managed* method is fatal, not degraded, when
the runtime runs code that blocks `SIGTRAP` inside it: glibc's `pthread_create`
blocks all signals around `clone()`, so the `#DB` the next instruction raises is a
blocked synchronous signal the kernel force-resets to `SIG_DFL` — exit 133, no
handler. CoreCLR's tiered-compilation worker idle-exits (~4 s) and respawns via
`pthread_create` on whatever thread next enqueues JIT work; the old `Invoke` ran
`DynamicInvoke` (heavy reflection machinery) *inside* the armed window, so on a slow
host it deterministically died and on a fast one never reproduced. The fix is to stop
stepping the runtime machinery at all.

## The mechanism

The single-step backend already filters recorded RIPs to the registered
`[base, len)` ([src/ss_backend.c](../../../../src/ss_backend.c) `ss_on_sigtrap`). So if
we (1) resolve the method body's `[base, len)` (already done —
[HwTrace.cs](../../../../bindings/dotnet/hwtrace/HwTrace.cs) `Method()` via
`JitMethodMap`/`PrepareMethod`), (2) mint a native function pointer for the body
*before* arming (reverse-P/Invoke over a non-generic per-arity delegate shim), and
(3) **arm, call the pointer, disarm — all in native C**, then the armed window
contains only the reverse-P/Invoke transition (runtime native code, outside
`[base,len)`, filtered) and the prepared body (inside `[base,len)`, recorded). No
managed reflection, no in-window JIT enqueue, no worker respawn, no blocked window.
The crash surface is removed *by construction* for covered signatures, and the
7-second stepped-reflection overhead disappears with it.

## Phases

### B0 — D's runtime honesty note *(landed)*

Add one sentence to `HwTrace.DegradationNote()` and a caveat to
[docs/bindings/dotnet.md](../../../../docs/bindings/dotnet.md): an in-process managed
window is fatal if the runtime spawns a thread in-window; pin the tiering worker or
use the out-of-process path. Zero protection, full honesty — lands first because it
protects users on day one independent of the rest.

### B1 — the C helper `asmtest_hwtrace_call_scoped` *(landed, host-tested)*

- `asmtest_ss_call_scoped(base, len, trace, fn, args, nargs, result_out, out_idx,
  out_gen)` in [src/ss_backend.c](../../../../src/ss_backend.c): push a region frame
  (arms TF), dispatch `fn(args…)` through the SysV integer ABI (0–6 register args),
  pop+normalize (disarms). Only `fn`'s in-`[base,len)` body is recorded; the
  dispatcher and any thunk are filtered. `>6` args or non-integer args return
  `ASMTEST_HW_EINVAL` so the caller falls back (B3).
- `asmtest_hwtrace_call_scoped(name, fn, args, nargs, result_out, out)` in
  [src/hwtrace.c](../../../../src/hwtrace.c): region-name lookup + arm-tid bookkeeping
  (mirrors `begin_scope`), routes to the single-step helper; `ASMTEST_HW_EUNAVAIL`
  on the PT/AMD/CoreSight backends (they observe out-of-band and have no crash
  surface — the normal begin/end path is correct there).
- Host test in [examples/test_hwtrace.c](../../../../examples/test_hwtrace.c): a native
  leaf `exec_alloc`'d, called via `call_scoped`, asserting (a) the recorded body
  offsets match the plain begin/end trace **byte-for-byte** (the parity invariant the
  managed rewire must preserve) and (b) the return value is correct. No managed
  runtime needed — this validates the mechanism on any x86-64 host with the single
  step tier.

### B2 — the .NET shim table + `Invoke` rewire *(landed, managed-lane)*

- A per-arity non-generic delegate table (`AsmBody0 … AsmBodyN`, `(long…)->long`)
  created via `Delegate.CreateDelegate` over the target method, marshaled to a native
  pointer with `Marshal.GetFunctionPointerForDelegate` **pre-arm** (generic `Func<>`
  cannot be marshaled — that is why the shims are concrete).
- `Invoke` (in-process): marshal args to `long[]`, call `asmtest_hwtrace_call_scoped`,
  box the result. The armed window no longer contains `DynamicInvoke`.
- Stream-parity test on the .NET lanes: the `Method()` output must stay byte-identical
  to the prior region-filtered trace (same offsets, same blocks).

### B3 — auto-fallback + the fail-loud invariant *(landed, managed-lane)*

When the target's signature is outside the shim set (ref/out, structs, `>N` args) or
marshalling fails, `Invoke` **auto-falls back to the out-of-process stealth stepper**
(§D3) rather than stepping `DynamicInvoke` in-window. If the out-of-process attach is
then refused (Yama `ptrace_scope`, missing helper), it records a `SkipReason` and runs
the call uninstrumented — **never a silent empty trace**. For a tracing tool a silent
no-trace is worse than a documented downgrade, because users assert on traces.

## Sharpening 1 — validate on a *slow* host (required follow-up)

B's safety rests on "no managed code is stepped" and "in-window GC suspension via the
activation signal is safe (kernel clears TF on handler entry; worst case a dropped TF
= truncated trace, not death)." Those are exactly the assumptions that only fail on a
slow host — a fast-box green is the false signal that produced this whole
investigation. The managed-lane parity test proves *correctness*; a
throttled/GitHub-runner run of the same `Method()` scope with the tiering worker left
at its default idle-exit is what proves *crash-avoidance*. This is wired as a
CI note on the .NET lane and is the one item that stays open until a runner confirms it.

## Sharpening 2 — fail loud, never silent

The durable contract is not "make TF safe" — it is that the tool always either
produces a sound trace or says exactly why it degraded. B3's loud `SkipReason` on a
refused fallback is the load-bearing part; keep it.

## Non-goals (unchanged from the decision doc)

- `LD_PRELOAD`-intercepting `pthread_create` or unblocking `SIGTRAP` from the handler
  — fighting the kernel's `force_sig` contract is a time bomb, not a posture.
- Making whole-window managed capture (`new AsmTrace()` on a runtime thread)
  crash-proof in-process — impossible by definition; PT (§Z2) is the strong tier for
  that, hardware permitting. That form keeps posture A + D.
