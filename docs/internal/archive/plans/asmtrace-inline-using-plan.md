# asm-test — AsmTrace inline-`using` conformance: plan

Make the inline scope shape

    using (ww = new AsmTrace(...)) { code }   // ctor arms, code runs inline, Dispose ends+renders

work for EVERY trace form, IN ADDITION to the delegate factories (`AsmTrace.Window`,
`AsmTrace.WindowHot`, `AsmTrace.Method`). All refs in
[bindings/dotnet/hwtrace/HwTrace.cs](../../../../bindings/dotnet/hwtrace/HwTrace.cs) /
[src/hwtrace.c](../../../../src/hwtrace.c). Companion to
[asmtrace-extensions-plan.md](asmtrace-extensions-plan.md).

> Status (2026-07-21): **COMPLETE — everything in this plan's scope is landed; plan archived.**
> **AMD-LBR inline LANDED**; **R4 (out-of-process inline) LANDED** (`578caed`, ahead of
> R1–R3); **R1 (`_kind` refactor) + R3 (SingleStep via the unified ctor) LANDED**;
> **R2 (Intel PT inline) LANDED and SILICON-VALIDATED** (`c7b4ef7` +
> [intel-hardware-validation.md](../../intel-hardware-validation.md)): the
> `new AsmTrace(HwBackend.IntelPt)` ctor arms the native `asmtest_hwtrace_pt_begin_window`/
> `_end_window` pair (a finalizable `PtWindowCtx`, a `Kind.PtWindow` Dispose that decodes on
> close), exact and not statistical — and on 2026-07-21 it ran on real silicon: `make
> hwtrace-pt-live` 631/631 across four consecutive runs on a bare-metal i7-8559U, with the
> inline ctor itself armed on silicon ("captured 863180 instructions", PT window EXACT). Off
> PT hardware the ctor still self-skips with a `SkipReason` that names the PT availability
> gate. The sole residual is NOT hardware: stable **.NET multi-threaded live-PT validation**
> is a managed-concurrency race owned by
> [dotnet-managed-pt-concurrency-plan.md](../../plans/dotnet-managed-pt-concurrency-plan.md).

## The general requirement + the two backend classes

Conforming to the inline shape means: **the ctor arms a capture that runs concurrently with
the lexically-following block (no call frame to delimit it), and Dispose ends it, fills
`Addresses`, and renders** — reusing the single shared `AttributeAddresses(img, when)` seam
(it branches on `IsStatistical`, so a new form needs no new attribution code). Whether a form
can meet it is decided by **how its backend is armed**:

- **START/STOP (out-of-band) — trivial.** Arm and disarm are two independent operations with
  no handshake: set/clear `EFLAGS.TF` (single-step), or perf `ENABLE`/`DISABLE` (AMD LBR,
  Intel PT). Ctor arms, block runs, Dispose ends. Nothing coordinates the block.
- **COORDINATION — hard.** The out-of-process reverse-attach stepper must single-step the
  calling thread *while* it runs the block. Today the delegate's call frame IS the window
  (`run_to` the entry, exit at `pc == win_ret`). An inline block has no frame, so it needs an
  ASYNC begin/end split with a stop-flag — and that split re-exposes the ctor/Dispose to the
  stepper.

## Per-form status

| Form | Class | Inline `using`? | Notes |
|---|---|---|---|
| Whole-window single-step `new AsmTrace()` / `(byMethod:,withRundown:)` | START/STOP (TF) | **YES (was already)** | best-effort on managed code (exit-133 hazard) |
| Region `new AsmTrace(code)` | START/STOP | **YES (was already)** | traces a native routine; works on any inited backend (single-step / AMD LBR) |
| **AMD-LBR statistical** `new AsmTrace(HwBackend.AmdLbr)` | START/STOP (perf) | **YES — LANDED** | one-cut native begin/end split + a backend-keyed ctor; *sheds* the delegate marshaling |
| **Intel PT** `new AsmTrace(HwBackend.IntelPt)` | START/STOP (perf AUX) | **YES — LANDED (silicon-validated 2026-07-21, `c7b4ef7`)** | same ctor; arms the native `asmtest_hwtrace_pt_begin_window`/`_end_window` pair (finalizable `PtWindowCtx`, `Kind.PtWindow` decode-on-close), exact not statistical; off bare-metal Intel PT it self-skips with a `SkipReason` that names the PT gate (no longer `"forward-look (not wired)"`) — validated live via `make hwtrace-pt-live` (631/631 ×4 on a bare-metal i7-8559U, [intel-hardware-validation.md](../../intel-hardware-validation.md)) |
| **Out-of-process** `new AsmTrace(outOfProcess: true)` | COORDINATION | **YES — LANDED (R4)** | built via the async stop-flag split, but *strictly worse* than the `AsmTrace.Window` factory — keep the factory; see R4 |
| Named-method `AsmTrace.Method` | — | **N/A** | category mismatch — see below |

## What LANDED (AMD-LBR inline)

- Native: `asmtest_hwtrace_sample_window_amd` split into `asmtest_hwtrace_sample_begin_amd`
  (open + `ENABLE`, return a `{fd, base_map, base_sz}` ctx) / `asmtest_hwtrace_sample_end_amd`
  (`DISABLE` + drain + free). The monolith (used by `WindowHot`) is kept.
- Managed: `new AsmTrace(HwBackend backend, int period = 16, …)` — for `AmdLbr` it arms the
  sampler in the ctor and drains+attributes in a Dispose `_amdWindow` branch; a finalizable
  `AmdSampler` holder releases the fd+mapping of a leaked scope (only AMD scopes finalize).
  `SingleStep`/`IntelPt` self-skip with a directive `SkipReason`. `WindowHot(Action)` stays as
  the delegate sibling.
- Validated live on a Ryzen 9 9950X (Zen 5) via the `CAP_PERFMON` lane
  ([examples/dotnet/amdhot](../../../../examples/dotnet/amdhot/) now uses the inline form) — and it
  gives the RICHEST managed attribution of the family: the block runs at native speed, so the
  JIT listener + rundown name the deep BCL (LINQ `Enumerable`, iterators) the OOP stepper elides.
- **Caveat:** the perf event is per-OS-thread, so the block must run synchronously (an
  `await`/thread hop would sample the wrong thread) — documented on the ctor; the `_armTid`
  hop-guard flags it, never crashes.

## Roadmap

**R1 — the `_kind` refactor (LANDED).** Replace the `_wholeWindow`/`_oopWindow`
(now +`_amdWindow`) bools with one `enum Kind` and a `switch` in Dispose. Zero-behavior; keeps
Dispose from accreting parallel bools. Not required for the AMD form (which shipped with a
minimal `_amdWindow` flag) but is the clean spine before PT/OOP cases.

**R2 — Intel PT inline (LANDED — silicon-validated 2026-07-21, `c7b4ef7`).** Same backend-keyed
ctor; the native `pt_begin_window` (perf AUX `intel_pt` + ENABLE) / `pt_end_window` (DISABLE +
libipt decode → fill `Addresses`, `IsStatistical=false`) pair shipped with
intel-pt-whole-window-substrate T4, and the stale framing an earlier revision carried here is
gone three ways: [pt_backend.c](../../../../src/pt_backend.c) is no longer fixture-less (the
synthetic `asmtest_pt_encode_fixture` AUX stream validates the end-to-end libipt decode on any
host), the facade wiring exists (`asmtest_pt_decode_window` dispatched at
[hwtrace.c:2458](../../../../src/hwtrace.c)), and the live-silicon run happened: per `c7b4ef7` +
[intel-hardware-validation.md](../../intel-hardware-validation.md), `make hwtrace-pt-live` passed
631/631 across four consecutive runs on a bare-metal i7-8559U, and the inline
`new AsmTrace(HwBackend.IntelPt)` ctor itself armed on silicon ("captured 863180
instructions", PT window EXACT) — PT is indeed the *ideal* inline backend (hardware
start/stop, exact, near-native). Off PT hardware the ctor self-skips with a `SkipReason` that
names the PT availability gate. The one residual on the .NET leg is NOT hardware: the managed
multi-threaded live-PT concurrency race, owned by
[dotnet-managed-pt-concurrency-plan.md](../../plans/dotnet-managed-pt-concurrency-plan.md).

**R3 — SingleStep via the unified ctor (LANDED).** Make `new
AsmTrace(HwBackend.SingleStep)` forward to the existing whole-window arming (extract it into a
shared private method) so the backend-keyed ctor covers single-step too, instead of directing
to `new AsmTrace()`. Cosmetic.

**R4 — Out-of-process inline (LANDED, `578caed`).** Shipped ahead of R1–R3: added a `volatile
int stop` to the shared stealth scratch, split `stealth_trace_windowed` into begin/end, replaced
`pc == win_ret` with `*stop` in a new loop variant, and added a **distinct** ctor (`new
AsmTrace(bool outOfProcess)` — [HwTrace.cs:2369](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L2369)),
never a peer bool on the backend-keyed ctor (its lifecycle is a live helper context +
stop→waitpid→read-back, a different teardown). It is crash-proof (ptrace-stops) but
**strictly worse** than `AsmTrace.Window`: it re-exposes the ctor/Dispose to the stepper
(harness-frame attribution pollution), demands thread-affinity across an arbitrary block
(fragile under `await`), and buys *only* the `using` syntax for **zero capture-quality gain**
(same channel, same deep-BCL limit). Build only if bare-`using` OOP is an explicit requirement;
otherwise keep the factory. **Full spec:**
[oop-inline-using-window-plan.md](oop-inline-using-window-plan.md).

## What should NOT conform

- **`AsmTrace.Method` (managed named-method) — a category mismatch.** It arms TF lazily across
  ONLY the body's native-dispatched call frame (`call_scoped`, arm→call→disarm), never the
  caller's setup. An inline `using (new AsmTrace(HotPath)) { HotPath(x,y); }` cannot reproduce
  that — the C# call is JIT-emitted managed, so arming TF across the block IS the crash-prone
  whole-window path lazy-arm exists to avoid. Its inline analog is the region ctor (native) or
  `using var t = AsmTrace.Method(f); t.Invoke(x,y);`. Keep it a factory + `Invoke`.
- **The OOP form should stay the recommended factory** even after an inline sibling — the
  factory's call frame isolates stepping to exactly the block; inline pollutes both window ends
  for no gain.
- **The unified ctor must not swallow the OOP form behind a peer bool** — its lifecycle differs
  fundamentally from the in-band arm-a-register/drain-a-buffer forms.
