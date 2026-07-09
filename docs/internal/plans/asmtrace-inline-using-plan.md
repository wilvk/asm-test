# asm-test — AsmTrace inline-`using` conformance: plan

Make the inline scope shape

    using (ww = new AsmTrace(...)) { code }   // ctor arms, code runs inline, Dispose ends+renders

work for EVERY trace form, IN ADDITION to the delegate factories (`AsmTrace.Window`,
`AsmTrace.WindowHot`, `AsmTrace.Method`). All refs in
[bindings/dotnet/hwtrace/HwTrace.cs](../../../bindings/dotnet/hwtrace/HwTrace.cs) /
[src/hwtrace.c](../../../src/hwtrace.c). Companion to
[asmtrace-extensions-plan.md](asmtrace-extensions-plan.md).

> Status: **AMD-LBR inline LANDED**; the rest is roadmap.

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
| **Intel PT** `new AsmTrace(HwBackend.IntelPt)` | START/STOP (perf AUX) | forward-look | same ctor; self-skips `EUNAVAIL` until a bare-metal PT begin/end pair + libipt decode-on-close is wired |
| **Out-of-process** (today `AsmTrace.Window`) | COORDINATION | not built | feasible via async stop-flag split, but *strictly worse* — see below |
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
  ([examples/dotnet/amdhot](../../../examples/dotnet/amdhot/) now uses the inline form) — and it
  gives the RICHEST managed attribution of the family: the block runs at native speed, so the
  JIT listener + rundown name the deep BCL (LINQ `Enumerable`, iterators) the OOP stepper elides.
- **Caveat:** the perf event is per-OS-thread, so the block must run synchronously (an
  `await`/thread hop would sample the wrong thread) — documented on the ctor; the `_armTid`
  hop-guard flags it, never crashes.

## Roadmap

**R1 — the `_kind` refactor (do before more forms).** Replace the `_wholeWindow`/`_oopWindow`
(now +`_amdWindow`) bools with one `enum Kind` and a `switch` in Dispose. Zero-behavior; keeps
Dispose from accreting parallel bools. Not required for the AMD form (which shipped with a
minimal `_amdWindow` flag) but is the clean spine before PT/OOP cases.

**R2 — Intel PT inline (forward-look).** Same backend-keyed ctor; add `pt_begin_window` (perf
AUX `intel_pt` + ENABLE) / `pt_end_window` (DISABLE + libipt decode → fill `Addresses`,
`IsStatistical=false`). Reserve the ctor path now (self-skips `EUNAVAIL`); wire when bare-metal
PT is available to validate. PT is the *ideal* inline backend (hardware start/stop, exact,
near-native).

**R3 — SingleStep via the unified ctor (optional ergonomics).** Make `new
AsmTrace(HwBackend.SingleStep)` forward to the existing whole-window arming (extract it into a
shared private method) so the backend-keyed ctor covers single-step too, instead of directing
to `new AsmTrace()`. Cosmetic.

**R4 — Out-of-process inline (build reluctantly, LAST).** Add a `volatile int stop` to the
shared stealth scratch, split `stealth_trace_windowed` into begin/end, replace `pc == win_ret`
with `*stop` in a new loop variant, and add a **distinct** ctor (`new AsmTrace(bool
outOfProcess)`) — never a peer bool on the backend-keyed ctor (its lifecycle is a live helper
context + stop→waitpid→read-back, a different teardown). It is crash-proof (ptrace-stops) but
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
