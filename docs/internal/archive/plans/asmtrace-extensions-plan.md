# asm-test — AsmTrace (.NET) whole-window extensions: roadmap

The `.NET` `AsmTrace` scope has grown a family of whole-window forms, each a different
answer to "trace a whole block of managed C#" under a different fidelity / crash-safety /
overhead trade. This plan captures what has LANDED and the remaining extensions, so the
family stays coherent rather than accreting one-off flags.

> Status legend: **landed** unless marked *(planned)* / *(forward-look)*. All forms live in
> [bindings/dotnet/hwtrace/HwTrace.cs](../../../../bindings/dotnet/hwtrace/HwTrace.cs); the native
> substrate is in [src/hwtrace.c](../../../../src/hwtrace.c), [src/ptrace_backend.c](../../../../src/ptrace_backend.c),
> [src/stealth_helper.c](../../../../src/stealth_helper.c), [src/amd_backend.c](../../../../src/amd_backend.c).
> Related: [managed-singlestep-posture-plan.md](managed-singlestep-posture-plan.md),
> [managed-wholewindow-oop-plan.md](managed-wholewindow-oop-plan.md),
> [amd-tracing-plan.md](../../plans/amd-tracing-plan.md).

## The AsmTrace whole-window family (current)

| Form | Tier | Fidelity | Crash-proof? | Overhead | Managed C#? |
|---|---|---|---|---|---|
| `using (new AsmTrace())` / `(byMethod:,withRundown:)` | in-process single-step | exact (self-truncating) | **No** — TF/SIGTRAP, exit-133 on `pthread_create`/exception | ~µs/insn | best-effort |
| `using (new AsmTrace(code))` | region (single-step / **AMD LBR** if inited) | exact | No (in-proc) | low | native leaf only |
| `AsmTrace.Method(delegate)` | region lazy-arm / §D3 oop | exact | oop: **yes** | low / high | one method |
| `AsmTrace.Window(() => {…})` | out-of-process ptrace | exact (block's own code + mid-window JIT, published live by §E3) | **Yes** | ~100–1000×/stop | whole block |
| `AsmTrace.WindowHot(() => {…})` | **AMD LBR statistical** | sampled hot-method histogram (+ §E6: exact ~16-branch islands at up to 4 optional `tileCheckpoints` — still a SURVEY, not a trace) | **Yes** | near-native (a few PMIs) | whole block |
| `using (new AsmTrace(HwBackend.AmdLbr)) {…}` | **AMD LBR statistical** (inline) | sampled hot-method histogram (richest managed attribution — deep BCL named) | **Yes** | near-native | whole block |
| `AsmTrace.WindowHybrid(() => {…})` | **survey (AMD LBR) → exact (ptrace) on the hot slice** | exact per-instruction on the hot methods; cold elided (`Survey` = pass-1 histogram) | **Yes** | survey near-native + exact only on the hot slice | whole block |

The three crash-proof whole-window forms are complementary: `Window` is exact-but-slow (§E3's
sibling-thread publish landed, so mid-window JIT is no longer elided), the AMD forms
(`WindowHot` delegate + `new AsmTrace(HwBackend.AmdLbr)` inline) are statistical-but-near-native
and — because the block runs at native speed — give the RICHEST managed attribution (deep BCL
named). The extensions below close the remaining gaps. See
[asmtrace-inline-using-plan.md](asmtrace-inline-using-plan.md) for the inline-`using`
conformance of each form.

> **This plan is CLOSED (2026-07-17).** E1–E7 have all landed; E6's branchsnap-at-managed-
> checkpoints tiling was the last open fragment. What remains below is the record of what was
> built and — for E4 and the non-goals — what was deliberately declined.

## Extensions

### E1 — `AsmTrace.WindowHybrid` — **LANDED**

Compose the two crash-proof forms: run `WindowHot` first (cheap statistical survey → the
hot methods), take the smallest method prefix reaching a `hotFraction` of the sample weight,
resolve each to its `[base,len)` via `_map`, then run `AsmTrace.Window(body)` **publishing
only those hot regions** to the shared channel instead of the full `EnumerateManagedCodeRanges()`
set. The stepper already step-overs unpublished regions (`in_region_set`,
[ptrace_backend.c](../../../../src/ptrace_backend.c)), so this gives **exact per-instruction
capture on the hot managed slice and cheap step-overs for the cold million instructions —
with no new native stepper code.** Degrades to plain `Window` when LBR is unavailable.
- API: `public static AsmTrace WindowHybrid(Action body, double hotFraction = 0.9, bool
  byMethod = true, bool withRundown = true, int rundownSettleMs = 300, …)`, with a `Survey`
  property exposing the pass-1 `WindowHot` result. Implemented in
  [HwTrace.cs](../../../../bindings/dotnet/hwtrace/HwTrace.cs) as managed glue over the injectable
  `RunWindowOutOfProcess(body, regionsOrNull)` seam (null == the byte-identical all-managed
  `Window` publish); the hot-set prefix is the pure/testable `HotPrefix`. Pass-2 resolution
  folds in the rundown jitdump so the survey's already-JIT'd hot methods resolve to their
  pass-2 regions (cross-spelling name match). Demo: [examples/dotnet/windowhybrid](../../../../examples/dotnet/windowhybrid/).
- Caveat: runs `body` **twice** (survey + exact) — the body must be deterministic enough that
  pass-1's hot set applies to pass-2; non-idempotent bodies use `WindowHot` (survey) or
  `Window` (exact, one pass). A LONG hot loop under the exact pass must avoid mid-window
  re-JIT/OSR (which re-enters the runtime under single-step and aborts) — the demo pins
  `TieredCompilation=false` for that reason; a modest hot loop needs no such knob.
- Degrade paths (all self-skip cleanly, never throw): no AMD LBR → full exact `Window` (publish
  all managed ranges); no ptrace → `Window` self-skips; survey armed-but-empty or no hot region
  resolves → full exact `Window`.

### E2 — `AsmMethod.Weight` (make the statistical semantic explicit) — **LANDED**

Today a `WindowHot` scope overloads `AsmMethod.Count` as a sample weight (documented on
`IsStatistical`). Added an explicit `long Weight` on `AsmMethod` so the meaning is in the type,
not just the docs: for a statistical scope `Weight` = endpoint-hit weight and `Count` is
documented `== Weight`; for an exact scope `Weight == Count`. `Disassembly` is already empty on
`IsStatistical` scopes (verified — `AttributeAddresses` gates it on `!IsStatistical`); a scope
`WeightIn(nameSubstring)` companion is the honest statistical analog of `InstructionsIn`
(numerically identical, but named for the sampled-weight meaning so a caller's intent reads
right on a survey). NB: `InstructionsIn` is intentionally NOT made throw/empty on statistical
scopes — the shipped `amdhot`/`crashproof-survey` examples rely on it returning the weight; the
honesty is delivered by `Weight`/`WeightIn` + tightened docs instead. The native `render_window`
refuse-a-statistical-trace item is not needed at the managed layer (statistical scopes never
render a `Path`).

### E3 — sibling-thread live JIT publish (close the `Window` deep-BCL gap) *(LANDED 2026-07-12)*

`AsmTrace.Window` (OOP) captures the block's OWN code + already-mapped R2R BCL, but methods
JIT'd FRESH mid-window (first-call generic instantiations, local functions) land outside the
pre-window coarse ranges and are elided. The built-but-OFF live publish
(`JitMethodMap.SetPublishChannel`) can't run on the stepped thread — firing the EventPipe
callback under single-step re-enters the runtime and aborts it (SIGABRT, observed). The safe
form: drain the runtime's `MethodLoad` events on a **separate, un-stepped sibling thread**
(a second `EventListener` or an `EventPipe` session) and publish each `(base,len)` to the
shared channel from there. The stepper drains it live. This lifts `Window` from
"block's-own-code" to deep-BCL parity with the in-process form. Effort: ~2–3d; the risk is
the sibling listener's own latency vs. the stepped thread's progress (publish-before-execute
ordering). See [managed-wholewindow-oop-plan.md](managed-wholewindow-oop-plan.md).

### E4 — inline `using (new AsmTrace(outOfProcess: true))` OOP form — **LANDED (`578caed`)**

> **Consolidated in [asmtrace-inline-using-plan.md](asmtrace-inline-using-plan.md) R4** — the
> same item (the async stop-flag split of the OOP stepper), which **shipped there ahead of
> R1–R3**: a `volatile int stop` in the shared stealth scratch, a begin/end split of
> `stealth_trace_windowed`, and a **distinct** ctor `new AsmTrace(bool outOfProcess)`
> ([HwTrace.cs:2369](../../../../bindings/dotnet/hwtrace/HwTrace.cs#L2369)) routed through
> `Kind.OopInlineWindow`. The inline-using plan is authoritative; this entry is a pointer.
> **The verdict below still stands** — it was built, but it is *strictly worse* than the
> factory, so `AsmTrace.Window` remains the recommended OOP entry point for exactly the
> reasons sketched here.

The OOP whole-window is a **delegate** today (`AsmTrace.Window(() => {})`) because a call
frame delimits the window (`win_base`/`win_ret`). Supporting the bare inline `using` shape
needs the ASYNC model: the ctor forks the helper and returns (the ctor/block/Dispose then run
single-stepped), and Dispose sets a **stop-flag** the helper polls (no `win_ret`). This is a
new native begin/end split of `stealth_trace_windowed` + stop-flag polling, and it steps the
ctor/Dispose machinery too — strictly more fragile surface, with the SAME deep-BCL attribution
limit as `Window`. Verdict: **syntax-only benefit; not worth the async native path** unless
the bare-`using` ergonomics become a hard requirement. Keep the delegate form as the OOP
entry point. (The in-process `using (new AsmTrace())` already provides the inline shape for
the best-effort tier.)

### E5 — AutoFDO-faithful weighting for `WindowHot` — **LANDED**

Today `WindowHot` weights each sampled branch-target endpoint equally. The AutoFDO/BOLT model
weights the basic block spanning `[to_i, from_{i+1}]` (MCF-style block frequencies, [BOLT](https://arxiv.org/pdf/1807.06735)),
so branchy code is not over-weighted vs. straight-line hot code. Additive fidelity upgrade
behind the same `WindowHot` surface. Effort: ~3d.

### E6 — snapshot-tiling / IBS producers into the `WindowHot` surface — **LANDED (both producers)**

Feed additional AMD facilities into the same sampled-endpoint surface: the deterministic
`bpf_get_branch_snapshot` boundary snapshot ([branchsnap.c](../../../../src/branchsnap.c)) at a
handful of managed checkpoints (method entry/exit, throw/catch) for **exact 16-branch islands**
on Zen 4/5 + `CAP_BPF`; and **IBS** op-sampling for statistical PC coverage to fill gaps.
Both are optional producers into `WindowHot.Addresses`, not new APIs. Effort: ~3–4d each.

**Landed (the IBS producer).** IBS-Op feeds the survey surface exactly as sketched — no new
API, a producer into the same `ips[]` endpoint stream behind
`asmtest_hwtrace_sample_window_amd`. On Zen 2 the branch stack (BRS/LbrExtV2) does not exist,
so the branch-stack open fails and the survey core falls back to `sample_window_ibs`
([hwtrace.c:1344](../../../../src/hwtrace.c)), which flattens each sampled `{from->to}` edge's
target `count` times — reconstructing the per-sample endpoint stream the branch-stack path
emits, so the caller's bucket-by-method hot histogram is identical in shape. Still purely
STATISTICAL (it never feeds the exact `insns[]`/`blocks[]` parity cascade), and
`ASMTEST_FORCE_IBS_SURVEY` forces the IBS path even where the branch stack works, for
cross-validation on Zen 3+/CI.

**Landed (the branchsnap tiling).** The boundary-snapshot substrate already shipped as a
**separate region API** — `AmdSnapshot.Trace(code, exitOff, body, tr)`
([HwTrace.cs](../../../../bindings/dotnet/hwtrace/HwTrace.cs),
[examples/dotnet/amd-snapshot](../../../../examples/dotnet/amd-snapshot/)): one `[base,len)`,
breakpoints at its exits, the RICHEST window decoded to an exact in-region stream. Tiling is the
OTHER consumer of that same BPF machinery and inverts all three axes: absolute **checkpoints**
(no region, no decode), **every** island (coverage is their union), and **endpoint PCs** merged
into the survey's `ips[]` — a producer into `WindowHot.Addresses`, not a new API, exactly as
this entry specified.

- Native: `asmtest_amd_tile_begin` / `_end` ([branchsnap.c](../../../../src/branchsnap.c), its own
  state slot; the single-active invariant is enforced across both slots) plant one HW execution
  breakpoint per checkpoint and merge every frozen window's retired branch targets.
  `asmtest_hwtrace_tile_window_amd` ([hwtrace.c](../../../../src/hwtrace.c)) is the survey core
  threaded with checkpoints. Kept OFF the public tier header, mirroring how E5's `_weighted`
  producer is wired, so the bindings-parity surface is unchanged.
- Composition: `tile_begin` deliberately does **not** open its own LBR event — the survey's
  branch-stack event already powers the hardware, so tiling adds breakpoints and *not* a second
  LBR user that could multiplex the survey it exists to enrich. **One** pass of the body feeds
  both producers. Tiles drain FIRST, so a checkpoint's exact island can never be crowded out of
  the bounded buffer by tens of thousands of sampled endpoints; `ntiled` records the split.
- .NET: `AsmTrace.WindowHot(…, tileCheckpoints:)` + `TiledIslands` / `TiledAddresses`, and
  `AsmTrace.Checkpoints(methods)` to resolve managed methods to entry PCs. Demo:
  [examples/dotnet/amd-tile](../../../../examples/dotnet/amd-tile/).
- **Honest boundary.** Tiled coverage is **SAMPLED / PARTIAL** — at each checkpoint HIT the ~16
  most recently retired branch targets, EXACTLY; between hits, nothing but what the sampler
  caught. It is **not** an exact whole-window trace; that stays a hardware dead end on AMD and a
  documented non-goal. Legitimate for `WindowHot` precisely because that surface is a
  hot-address histogram with no completeness claim — `IsStatistical` stays TRUE for a tiled
  scope (a better-covered SURVEY, never an exact trace), and tiled endpoints never feed the
  exact `insns[]`/`blocks[]` parity cascade.
- **Two truncation signals, and the split is load-bearing** (`tile_truncated` /
  `AsmTrace.TiledTruncated` vs. the survey-wide `truncated` / `Truncated`). Neither is set for
  the inherent 16-deep island bound — that is the producer's defined granularity, and an
  always-true flag makes every assert disjoined with it vacuous. The reason they are SEPARATE
  is sharper, and was found by adversarial review: this repo's rule *"assert covered OR
  truncated, never bare covered"* is sound **only when the truncation term is causally tied to
  the property asserted**. Applied mechanically it becomes a vacuity vector — `truncated` is
  owned by the SAMPLER (it fires when the sampler's 256 KiB ring runs near-full), which says
  nothing about whether an island kept its endpoints. Disjoining island content with it was
  escapable, and was escaped: clobbering island data + growing the hot loop until the ring
  filled made the managed lane exit **0 GREEN** while printing `covered? TILED=False`. The
  defence "we measured truncated=0" is **not** sufficient — it holds only for today's fixture
  size, and a 20–30% branch-count drift (a different Zen part, a JIT change) would pin it true
  and silently retire the assert forever. An island-content assert must disjoin with
  `tile_truncated` only.
- **Boundary assumption.** Tiling inherits the Zen 4/5 property Phase 9 validated for the region
  path: a `#DB` execution breakpoint does not evict the recent branch history before the BPF
  helper reads it. Phase 9 validated it for a `ret` boundary; the tail-`jmp` boundary assumption
  is untouched here. A managed method-entry checkpoint is neither — it is a CALL target, nearer
  the validated shape (the call retires, then the breakpoint fires on the entry instruction) but
  not literally it, so the test asserts the entry edge is present rather than assuming it.
- `blockWeighted` (E5) and `tileCheckpoints` (E6) are refused in combination, loudly: E5 weights
  the straight-line span between adjacent sampled branches, which is undefined across an
  island's boundary.
- **Measured live on Zen 5** (Ryzen 9 9950X, kernel 6.17), not self-skipped —
  `make docker-hwtrace-codeimage` (native, [examples/test_branchtile.c](../../../../examples/test_branchtile.c)):
  islands=1, **ntiled=16 == islands × lbr_depth**, nips=8528, tile_truncated=0; negative control
  (a never-executed checkpoint) islands=0/ntiled=0; differential 5/5 tiled vs **0/5** untiled @
  period=50000. `make docker-hwtrace-dotnet-amd` (managed): a JIT'd `ColdLeaf` entry, 16 island
  endpoints from 1 island, covered TILED=True / UNTILED=False, `ColdLeaf` resolved distinct from
  the later-JIT'd `ColdLeafExtra`, `Over(int32)`/`Over(string)` not conflated.
- **The island assertion is the depth equality**, not `ntiled >= 1`. The weak form is satisfied
  by island[0] alone — which is present by HARDWARE CONSTRUCTION (the breakpoint sits on the
  entry, so the newest retired edge is necessarily the call that reached it) and therefore can
  never fail, leaving 15 of the 16 endpoints E6 exists to add unverified. Proven: dropping every
  other entry (`i += 2`) halved ntiled 16 → 8 with every weak check still green. Do NOT try to
  assert "the `hot_work` call site appears in the island" — the body calls it 200k times with a
  64-iteration inner loop, so `&hot_work` is >16 branches back and is legitimately ABSENT; the
  island is [0]=the entry, [1..15]=three distinct inner-loop branch addresses.
- **A managed checkpoint is not a C function entry**, and that distinction was not free — it
  bit TWICE, the same way. The failure class: *an address that is real, arms cleanly, and is
  not the requested method's.* A hardware breakpoint makes that class severe, because the
  wrong body still yields `TiledIslands==1, TiledAddresses==16` — indistinguishable from
  success, with the caller's coverage silently absent.
  1. `MethodHandle.GetFunctionPointer()` returns a **precode stub**: armed
     (`tile_begin: armed ncp=1`), never fired, `islands=0`.
  2. Resolving by bare-name substring + newest-wins (`TryResolveHotMethod`'s ladder, correct
     for LABELLING a sampled address where a wrong guess costs one mis-attributed sample)
     silently arms the WRONG body: `Program::ColdLeaf` is a substring of
     `Program::ColdLeafExtra`, and overloads share `Program::Over`.
  `AsmTrace.Checkpoints` therefore resolves through the **rundown jitdump** (E1's path) with a
  signature-qualified key (`Type::Method(int32)`), anchors on `(` so a prefix cannot collide,
  and **REFUSES** — reporting via `LastCheckpointSkips` — when more than one distinct method
  still matches. Tier bodies of the *same* method are not an ambiguity (newest runs); two
  different methods are, and there is no honest way to pick. This is why the managed claim
  needed its own live lane rather than inheriting the native test's result.

Gated on Zen 4/5 + `CAP_BPF` + `CAP_PERFMON` + a BPF-toolchain build + Linux >= 6.10 (it
self-skips otherwise). `ASMTEST_TILE_REQUIRE=1` turns those self-skips into FAILURES for the one
lane built to satisfy the gate (`docker-hwtrace-dotnet-amd` sets it) — the `CLEANROOM_ONLY=<lang>`
pattern, so the lane cannot go green by skipping the thing it exists to prove.

### E7 — AMD-LBR **region** `using` example — **LANDED**

`using (new AsmTrace(code))` traces a native `NativeCode` blob on the AMD LBR tier — the
region ctor respects a pre-inited backend (`begin_scope` → `try_begin` → `hwtrace_begin_amd`);
the only requirement is `HwTrace.Init(HwBackend.AmdLbr)` first (no auto-init on the region
form). [examples/dotnet/amdlbr](../../../../examples/dotnet/amdlbr/) demonstrates it (with a retry,
since AMD region sampling is non-deterministic per attempt), self-skipping off Zen/`CAP_PERFMON`.
Distinct from `amdhot`, which is the region-FREE statistical whole-window (also LANDED, as the
inline `using (new AsmTrace(HwBackend.AmdLbr))` — see
[asmtrace-inline-using-plan.md](asmtrace-inline-using-plan.md)).

## Priority

1. ~~**E1 `WindowHybrid`**~~ — **LANDED** (the exact-on-hot-slice composition; all reuse).
2. ~~**E2 `AsmMethod.Weight`**~~ — **LANDED** (alongside E1).
3. ~~**E3 sibling-thread publish**~~ — **LANDED** (closes the `Window` deep-BCL gap, the one honest-partial in the family).
4. ~~**E7 AMD region example**~~ — **LANDED**.
5. ~~**E5**~~ — **LANDED**. ~~**E6**~~ — **LANDED**, both producers: the IBS survey fallback
   (Zen 2) and the branchsnap tiled at managed checkpoints into `WindowHot.Addresses`.
6. ~~**E4 inline-`using` OOP**~~ — **LANDED** (`578caed`, shipped as the inline-using plan's
   R4). Built, but *strictly worse* than the factory: `AsmTrace.Window` stays the recommended
   OOP entry point.

## Non-goals

- Exact whole-window on AMD (hardware dead end — see [amd-tracing-plan.md](../../plans/amd-tracing-plan.md)).
- Making the in-process whole-window crash-proof (impossible in-process — see [managed-singlestep-posture-plan.md](managed-singlestep-posture-plan.md)).
- A dedicated permissioned CI lane for the AMD dotnet examples — they self-skip in the plain
  lane and are validated on a self-hosted Zen runner (mirroring `docker-hwtrace-amd`); a
  `docker-hwtrace-dotnet-amd` lane is optional operational plumbing, not a capability.
  **Superseded for E6 (2026-07-17), and only for E6.** The reasoning above holds for the SAMPLED
  AMD examples: their native siblings validate the capability, so a skipped dotnet demo costs
  nothing. It does not survive a feature whose entire claim is about MANAGED checkpoints —
  `docker-hwtrace-dotnet-amd` now exists (`Dockerfile.hwtrace-dotnet-amd`: the .NET SDK + the
  eBPF toolchain in one image, which no other lane had) because a JIT'd method entry is NOT a C
  function entry, and the difference was load-bearing: the obvious `GetFunctionPointer()`
  address armed cleanly and never fired. That is a capability question, not plumbing — per
  CLAUDE.md, a lane that cannot test a feature gets extended.
