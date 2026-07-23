# Cover the unwarmed/PT in-window-JIT premise (stop the permanent self-skip) — implementation

> **Source.** Later addition (2026-07-23): found by
> [dotnet-managed-pt-concurrency-plan.md](../plans/dotnet-managed-pt-concurrency-plan.md)
> T4's validation on the bare-metal Intel PT box (Core i7-8559U) — the plan's
> own honest note records that `MethodsObserved == 0` there **consistently**,
> so the `unwarmed/PT compose: >=1 method JIT'd inside the window` premise
> self-skips on **every** run of the only host class that can arm it. The
> plan's sanctioned alternative — "or force a guaranteed in-window JIT" — is
> this doc. Verified against the tree on 2026-07-23; if the code disagrees,
> re-verify before implementing.

## Why this work exists

`WholeWindowUnwarmedPtChecks`
([HwTraceProgram.cs:1421](../../../bindings/dotnet/hwtrace/HwTraceProgram.cs#L1421))
is the PT prong of the managed unwarmed compose: arm the STRONG Intel PT
whole-window inline ctor, first-call a never-called fixture inside it, and
assert the JIT map saw a method compiled in-window. On the one host class where
the PT tier arms at all, the premise check never runs — the map's close-time
snapshot is always empty, so the check takes its sanctioned timing self-skip
**every time**. A check that can only ever self-skip on its target hardware is
not covering its premise (the repo's self-skip honesty rule, CLAUDE.md shape).

The cause is delivery, not compilation. `UnwarmedPtPath` is JIT'd at first call
**inside the window** — compilation completes before the body executes, and the
runtime emits the method-load event at compile time. But the `JitMethodMap` is
an in-process `EventListener`: the event reaches it on the runtime's EventPipe
dispatch thread **asynchronously**, and a PT window is a native-speed hardware
capture that closes sub-millisecond after the call returns — far ahead of the
delivery. `MethodsObserved` is snapshotted from the map at close
([HwTrace.cs:3808](../../../bindings/dotnet/hwtrace/HwTrace.cs#L3808)), so it
reads 0. The in-process TF sibling never sees this because single-stepping a
20 000-iteration loop holds its window open for wall-clock seconds — delivery
lands in-window for free. The fix gives the PT window the same property
deliberately: **hold the window open (bounded) until the delivery lands**, then
close. On timeout the honest self-skip remains (the never-flake rule).

## Tasks

### T1 — Bounded in-window wait for the JIT-map delivery, premise covered live  (S, gate: live leg needs the bare-metal Intel PT box)

**Goal.** On PT silicon the `unwarmed/PT compose` premise check reports `ok`
(MethodsObserved ≥ 1) deterministically; the self-skip arm survives only for a
genuine EventPipe delivery stall, and the check still never emits `not ok` on
timing.

**Steps.**

1. Add a public bounded wait to `AsmTrace`
   ([HwTrace.cs](../../../bindings/dotnet/hwtrace/HwTrace.cs), beside the
   close-time `MethodsObserved`): poll the live map's thread-safe
   `CountFor(nameSubstring)` (the map is concurrent by design — the listener
   thread appends while the scope thread reads) with a short sleep, up to
   `timeoutMs`; return whether the method was observed. No-op `false` on an
   unarmed or mapless scope. Do **not** put any wait in `Dispose` — closes must
   stay non-blocking for every caller, and "wait for ≥1 method" is wrong for
   windows that JIT nothing.
2. In `WholeWindowUnwarmedPtChecks`
   ([HwTraceProgram.cs:1421](../../../bindings/dotnet/hwtrace/HwTraceProgram.cs#L1421)),
   after the `UnwarmedPtPath(20000)` call and **inside** the `using` window,
   wait for `"UnwarmedPtPath"` (2 s bound). Rewrite the stale
   "whether the runtime got round to compiling … is a scheduling outcome"
   comment — compilation is in-window by construction; only delivery races —
   and reword the self-skip arm to name the residual (bounded-wait timeout,
   i.e. a delivery stall), keeping it a skip, never a failure.
3. The wait also closes a decode gap for free: with delivery in-window, the
   map's `trackBytes` snapshot and `ImageNow` at close now include the fresh
   method, so the close-time versioned decode resolves `UnwarmedPtPath` from
   the tracked image rather than leaning on the trace-or-truncated fallback.

**Code.** `bindings/dotnet/hwtrace/HwTrace.cs` (one public method on
`AsmTrace`), `bindings/dotnet/hwtrace/HwTraceProgram.cs` (the PT prong check).
No C-core change, no new tier symbol (`check-bindings-parity` unaffected).

**Tests.** Off PT silicon: the check self-skips at `Available(IntelPt)` exactly
as today — plain `make docker-hwtrace-dotnet` (and `docker-hwtrace-dotnet9`)
stay green with the PT prong naming the gate. On the PT box:
`docker run --cap-add=PERFMON … asmtest-dotnet make hwtrace-dotnet-test` must
print `ok … unwarmed/PT compose: >=1 method JIT'd inside the window` (not the
SKIP) stably across ≥3 consecutive runs, plus one
`make docker-hwtrace-dotnet-ambient-stress` pass for the concurrency lane.

**Docs.** `CHANGELOG.md` `### Fixed`. The plan's T4 honest note gets a dated
"closed by" line pointing here.

**Done when.** The premise line is `ok` (never SKIP, never `not ok`) on ≥3
consecutive PT-box suite runs, and every non-PT lane is byte-identically green.

> **Landed 2026-07-23, measured on the i7-8559U PT box (the implementing
> agent).** `AsmTrace.WaitMethodObserved(nameSubstring, timeoutMs=2000)` +
> the in-window wait in the PT prong. Privileged suite
> (`docker run --cap-add=PERFMON … make hwtrace-dotnet-test`): **3 consecutive
> fully-captured runs, exit 0, `1..231`, 231 ok / 0 `not ok` each**, the
> premise line `ok … >=1 method JIT'd inside the window (got 1)` every run
> (was: SKIP on every run this box had ever done) — the suite grew 229→231
> under caps because the two prong checks now number instead of skipping. The
> resolve check reports `320032 insns, truncated=True` — the wait's captured
> instructions fill the AUX ring, and the check's trace-or-truncated contract
> asserts on the resolved 320 k. Plain `docker-hwtrace-only-dotnet` `1..229`
> exit 0 (prong self-skips at `Available`); `docker-hwtrace-dotnet9` `1..229`
> (PT skip on **permission**, not a missing lib); ambient stress `1..576`, 25
> iterations, exit 0; `docker-fmt-check` + `docker-docs` clean. Marked `☑` —
> `✅` is an independent validating agent's stamp, per the README role split.

## Constraints & gates

- **Hardware gate (validation only):** the live leg needs the bare-metal Intel
  PT box (the i7-8559U qualifies); everywhere else the prong self-skips at
  `Available(IntelPt)` before reaching any of this.
- The never-flake rule stands: a delivery stall past the bound must stay a
  named self-skip, not a `not ok`.
- Honor the role split: the implementer measures; an independent validating
  agent stamps `✅`.
