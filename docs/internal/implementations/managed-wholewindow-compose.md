# Managed-runtime whole-window: live compose, safe managed-arm routing, and ambient PT stitching — implementation

> **Sources.** Actioned from
> [scoped-tracing-zeroconfig-plan.md](../plans/scoped-tracing-zeroconfig-plan.md)
> (§Z1.1 routing note, §Z3 live half, §Z4 escalation) and
> [zen2-singlestep-trace-plan.md](../archive/plans/zen2-singlestep-trace-plan.md) (§W2/§D3
> stepper, W3 block-step). Written 2026-07-17. If this doc and a source disagree,
> this doc wins (sources may be stale); if the CODE and this doc disagree,
> re-verify before implementing.

## Why this work exists

`using (new AsmTrace()) { HotPath(data); }` — the zero-config whole-window scope —
already renders real traces of managed .NET code, but no lane has ever proved it
against a method that is **genuinely compiled inside the window** (with the JIT and
GC running alongside it), the region-free arm still steps managed code in-process
with `EFLAGS.TF` (accepted by convention, with the safe routing to the
out-of-process stepper left as roadmap), and the ambient "stitch a logical async
operation across thread hops with zero touch" form has no producer at all. This doc
builds those three things: the live unwarmed compose lane, the safe managed-arm
routing, and the AsyncLocal-driven decode-at-disable per-thread PT stitching.

## What already exists (verified 2026-07-17)

The landed substrate, by file:

- [src/hwtrace.c](../../../src/hwtrace.c) — the region-free arm:
  `asmtest_hwtrace_begin_window` (:2554, single-step only; every other backend
  returns `ASMTEST_HW_EUNAVAIL`; **no managed detection of any kind**),
  `asmtest_hwtrace_end_window` (:2594, flags `truncated` on a cross-thread close
  via the handle-carried `arm_tid`), `asmtest_hwtrace_render_window` (:2625,
  live-memory render), `asmtest_hwtrace_render_versioned` (:2486, codeimage
  decode-at-version), `asmtest_hwtrace_call_scoped_ex` (:2419),
  `asmtest_hwtrace_stitch_handles` (:2754, the binding-blittable stitch bridge),
  and the §D3 out-of-process window `asmtest_hwtrace_stealth_window_begin`/`_end`
  (:3275/:3344 — forks a helper that reverse-attaches and steps the arming thread
  out of band).
- [src/stealth_helper.c](../../../src/stealth_helper.c) —
  `asmtest_stealth_helper_run_window_async` (:165) drives the **per-instruction**
  `asmtest_ptrace_trace_attached_window_stop` (:199). No block-step form of the
  stop-flag window exists yet.
- [src/ptrace_backend.c](../../../src/ptrace_backend.c) — the landed blockstep
  family (global position: AMD Part III P2/P3 are LANDED, not planned):
  `asmtest_ptrace_trace_call_blockstep` (:1708),
  `asmtest_ptrace_trace_attached_blockstep` (:1887),
  `asmtest_ptrace_trace_attached_windowed` (:2181),
  `asmtest_ptrace_trace_attached_windowed_blockstep` (:2380),
  `asmtest_ptrace_trace_attached_window_stop` (:2526). Declarations in
  [include/asmtest_ptrace.h](../../../include/asmtest_ptrace.h) :108–:174.
- [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) — window ABI
  (:442/:450/:458), slice ABI `asmtest_hwtrace_slice_t` (:479) /
  `asmtest_hwtrace_slice_bound_t` (:487), `asmtest_hwtrace_stitch` (:495) /
  `_stitch_handles` (:508), `asmtest_hwtrace_attribute_window` (:690), and the
  `struct_size`-negotiated options struct (:76 — the extension seam new options
  ride).
- [src/codeimage.c](../../../src/codeimage.c) — the soft-dirty gate
  `asmtest_codeimage_available` (:298; stub :490) and `asmtest_codeimage_track`
  (:382), which returns `ASMTEST_CI_EUNAVAIL` when soft-dirty/`PAGEMAP_SCAN` is
  absent (:386).
- [bindings/dotnet/hwtrace/HwTrace.cs](../../../bindings/dotnet/hwtrace/HwTrace.cs)
  — the empty ctor `AsmTrace(bool emit = true, bool byMethod = false, bool
  withRundown = false, …)` (:2105) arming via `ArmWholeWindow` (:2140 →
  `asmtest_hwtrace_begin_window`, auto-initing single-step); the §D3 inline OOP
  ctor `AsmTrace(bool outOfProcess, …)` (:2605 →
  `asmtest_hwtrace_stealth_window_begin` + shared address channel); the static OOP
  `AsmTrace.Window(Action …)` (:2358); `JitMethodMap` (:3873 — the
  `MethodLoadVerbose` listener; its doc-comment at :3868–3871 records that an
  in-proc listener sees only methods JIT'd **after** enable, no rundown) with the
  §E3 mid-window live publisher `SetPublishChannel`/`PublishLoop` (:3910/:3955);
  and the explicit-Step software-tier stitch producer `AsmStitchedTrace` (:4953,
  `AsyncLocal<long>` scope-id carry, `call_scoped` per hop, integer-signature
  only).
- [bindings/dotnet/hwtrace/HwTraceProgram.cs](../../../bindings/dotnet/hwtrace/HwTraceProgram.cs)
  — the TAP self-suite (`Check` prints `ok N - …` :49–57) run by
  `hwtrace-dotnet-test`; already contains the §E3 mid-window-JIT OOP check
  `WindowLiveJitChecks` (:1014) over `MidWindowJitLoop` (:1004).
- [examples/test_hwtrace.c](../../../examples/test_hwtrace.c) — the C twins:
  `test_stitch_handles` (:2387), `test_stitch_hops_scripted` (:2997),
  `test_ptrace_windowed` (:4084), `test_ptrace_windowed_blockstep` (:4524),
  `test_wholewindow_singlestep` (:7030), `test_wholewindow_ss_descend` (:7254),
  `test_asynchop_flag` (:7467), `test_crossthread_handle_collision` (:7647),
  `test_zeroctor_managed_compose` (:8042 — the synthetic §Z3 half: a fake
  MethodLoad → `track()` → in-place re-JIT → `render_versioned` at `when0`/`when1`).
  `test_wholewindow_ss_managed_routes` does **not** exist (struck 2026-07-16
  because the seam it gates was not built; this doc builds it, so T9 revives it).
- Make lanes — [mk/native-trace.mk](../../../mk/native-trace.mk):
  `hwtrace-jit-dotnet` (:2233), `hwtrace_dotnet_env` (:2521 — pins
  `DOTNET_TC_BackgroundWorkerTimeoutMs=36EE80` so CoreCLR's tiering worker never
  idle-exits inside a stepped window), `hwtrace-dotnet-test` (:2575).
  [mk/docker.mk](../../../mk/docker.mk): `docker-hwtrace` (:442),
  `docker-hwtrace-jit-dotnet` (:473), the per-language fan-out
  `docker_hwtrace_lang_rule` (:643, `HWTRACE_DOCKER_LANGS` includes `dotnet`
  :433), and the pinned SDK `DOCKER_APT_dotnet := dotnet-sdk-8.0` (:149).
- Parity gate —
  [scripts/check-bindings-parity.sh](../../../scripts/check-bindings-parity.sh)
  requires every symbol in the tier headers (including `asmtest_hwtrace.h` and
  `asmtest_ptrace.h`) to be wrapped by all ten bindings; intentional omissions go
  in [scripts/bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt)
  (per-binding lines, e.g. the `asmtest_dr_under_dynamorio` block).

**Prove the baseline green before touching anything:**

```
make docker-hwtrace          # C TAP suite in a plain container → "1..N" + "# N passed, 0 failed"
make docker-hwtrace-dotnet   # .NET TAP self-suite ("ok N - …" lines, exit 0)
make check-bindings-parity   # parity gate exits 0
```

All three must pass unchanged on any x86-64 Linux Docker host, no privilege.

## Tasks

### T1 — Unwarmed in-process compose check in the .NET self-suite  (M, depends on: none)

**Goal.** A CI-runnable check proves the empty-ctor seam (JitMethodMap
`MethodLoadVerbose` → `asmtest_codeimage_track` → close-time versioned decode)
over a method whose **first JIT happens inside the window**, with GC also running
inside the window.

**Steps.**
1. In [bindings/dotnet/hwtrace/HwTraceProgram.cs](../../../bindings/dotnet/hwtrace/HwTraceProgram.cs),
   add `WholeWindowUnwarmedChecks()`, mirroring the shape of `WindowLiveJitChecks`
   (:1014) but over the **in-process** empty ctor. Register it in `Main`'s check
   sequence next to the other whole-window checks.
2. Add the fixture: `[MethodImpl(MethodImplOptions.NoInlining)] static long
   UnwarmedHotPath(int n)` — a small loop with an allocation (`new byte[32]` per
   K iterations) so GC work exists to trigger. **Nothing may call it before the
   scope** — that is the entire point; add a comment saying so (the §E3 fixture
   comment at :994–:1002 is the pattern).
3. Body: `using (var ww = new AsmTrace(byMethod: true)) { r =
   UnwarmedHotPath(20000); GC.Collect(0); }` then assert (each a `Check`):
   - the arithmetic result is correct (the scope never perturbs semantics);
   - if `!ww.Armed`: `ww.SkipReason.Length > 0` and print
     `# SKIP unwarmed compose: {ww.SkipReason}` and return — the single-step
     tier can legitimately be absent (non-x86-64, non-Linux);
   - `ww.MethodsObserved >= 1` — the map saw at least one method JIT'd
     **inside** the window (the unwarmed premise: `UnwarmedHotPath`'s
     `MethodLoadVerbose` fires after arm because nothing called it before);
   - `ww.InstructionsIn("UnwarmedHotPath") > 0 || ww.Truncated` — the
     covered-or-truncated posture `WindowLiveJitChecks` already uses (a 1<<20
     window ring can honestly truncate under the JIT+GC noise; a full miss on an
     untruncated capture is a regression);
   - `ww.Truncated || ww.Addresses.Length > ww.LabelledInstructions` — the
     window captured runtime (JIT/GC) instructions beyond the labelled method,
     i.e. the compose really ran against a noisy live window, not a warmed body.
4. Soft-dirty degradation: when `trackBytes` cannot version
   (`asmtest_codeimage_available()` false in-container), `JitMethodMap` falls back
   to live-memory labelling; the check must not assert version-specific text —
   that is T2's job under its own gate. Print
   `# NOTE unwarmed: codeimage versioning unavailable (soft-dirty)` when the map's
   image handle is zero, and keep the remaining asserts.
5. Run `make docker-hwtrace-dotnet` — the new `ok … unwarmed …` lines appear, exit
   0.

**Code.** ~120 lines of C#, no native changes. Use only existing public surface:
`Armed`, `SkipReason`, `MethodsObserved`, `Methods`, `InstructionsIn`,
`Addresses`, `LabelledInstructions`, `Truncated`.

**Tests.** This *is* the test. Failure looks like `not ok N - unwarmed compose:
method observed inside window (got 0)`; pass is the `ok` line. On a host without
the single-step tier the check prints `# SKIP` and asserts only the SkipReason
contract.

**Docs.** Append to `CHANGELOG.md` under `## [Unreleased]` → `Added`: "the .NET
whole-window self-suite gains an unwarmed-method live-compose check (JIT + GC
inside the window)". No user-facing page yet (T4 owns the lane docs).

**Done when.**
- `make docker-hwtrace-dotnet` passes with the new checks listed.
- Commenting out the pre-scope "nothing calls it" discipline (i.e. warming the
  method before the scope) flips the `MethodsObserved`-attribution expectation —
  verified once by hand, then restored.

### T2 — Mid-window re-tier: decode-at-version live check  (S, depends on: T1)

**Goal.** Prove the versioned seam live: drive the unwarmed method past tier-up
**inside** the window and assert close-time attribution survives the method's
address moving (a fresh `MethodLoadVerbose` at a new `MethodStartAddress`).

**Steps.**
1. Add `WholeWindowRetierChecks()` next to T1's check, gated on
   `Environment.GetEnvironmentVariable("ASMTEST_UNWARMED_RETIER") == "1"` (prints
   `# SKIP retier: ASMTEST_UNWARMED_RETIER not set` otherwise — the default suite
   stays fast and deterministic).
2. Fixture: call `UnwarmedRetierPath` (a second, never-before-called
   `NoInlining` method) in a loop of ≥ 64 calls inside the window — past the
   default `DOTNET_TC_CallCountThreshold=30` — then spin briefly. The lane (T4)
   sets `DOTNET_TC_AggressiveTiering=1` so promotion happens promptly inside the
   window instead of after it, and inherits `hwtrace_dotnet_env` (worker pinned
   resident, so the tier-up completes without a mid-window `pthread_create`).
3. Assert: EITHER `JitMethodMap` observed the method at **two** distinct start
   addresses inside the window (expose a `map.ObservedVersions(name)` count if the
   map does not already surface it — a ~10-line addition reading its `Entry` list)
   AND `ww.InstructionsIn("UnwarmedRetierPath") > 0 || ww.Truncated`; OR print
   `# SKIP retier: runtime did not re-tier inside the window` (tiering timing is
   the runtime's; never flake).
4. `ASMTEST_UNWARMED_RETIER=1 make hwtrace-dotnet-test` locally in the container
   (`make dev-dotnet`), then via T4's lane.

**Code.** C# only. Tier-up and OSR each raise a fresh `MethodLoadVerbose` with a
new `MethodStartAddress` (see Research notes), so the listener side needs no new
event plumbing — only the per-name version count.

**Tests.** Self-testing; the two-address assert is the live twin of
`test_zeroctor_managed_compose`'s synthetic re-JIT (:8042).

**Docs.** Internal-only: covered by T4's lane entry. No separate page.

**Done when.**
- With the env set in the T4 lane, the check either passes with two observed
  versions or prints the honest `# SKIP` — never `not ok` on tiering timing.
- Without the env, the suite output is unchanged.

### T3 — Unwarmed §D3 out-of-process prong  (S, depends on: T1)

**Goal.** The same unwarmed compose through the crash-proof out-of-process window
(`AsmTrace(outOfProcess: true)`), riding the landed §E3 live-publish channel, so
the compose is proven on the stepper that is safe for managed code.

**Steps.**
1. Add `WindowUnwarmedOopChecks()`: a third never-called `NoInlining` fixture, run
   under `using (var ww = new AsmTrace(outOfProcess: true)) { … }` with the same
   allocation + `GC.Collect(0)` inside the window.
2. Asserts mirror T1 plus the OOP-specific ones from `WindowLiveJitChecks`
   (:1014): on unarmed (`ptrace` denied) print `# SKIP` with the reason;
   otherwise `ww.LiveJitPublished >= 1` (the §E3 sibling published the fresh
   method's range mid-window) and covered-or-truncated attribution.
3. Run `make docker-hwtrace-dotnet` (Docker's default seccomp permits ptrace of
   own children on host kernel ≥ 4.8; the reverse-attach uses
   `PR_SET_PTRACER_ANY` — see `asmtest_hwtrace_stealth_window_begin` :3298).

**Code.** C# only; no native change — `stealth_window_begin`, the address
channel, and the publisher all exist.

**Tests.** Self-testing. Distinct from `WindowLiveJitChecks`: that check uses the
static `AsmTrace.Window(Action)` wrapper and does not force GC; this one uses the
inline ctor form (the empty-ctor-shaped scope the plan's §Z3 names) and asserts
the GC/JIT-noise premise on an unwarmed method.

**Docs.** Internal-only (same reason as T2).

**Done when.**
- `make docker-hwtrace-dotnet` shows the OOP unwarmed `ok` lines, or the
  `# SKIP … ptrace` line where denied, exit 0 either way.

### T4 — The `hwtrace-dotnet-unwarmed` lane + docker wrapper  (S, depends on: T1–T3)

**Goal.** One named, documented lane runs the whole unwarmed compose set with the
re-tier env armed, so "the live §Z3 half" has a lane a release checklist can name.

**Steps.**
1. In [mk/native-trace.mk](../../../mk/native-trace.mk), next to
   `hwtrace-dotnet-test` (:2575), add:
   ```make
   .PHONY: hwtrace-dotnet-unwarmed
   hwtrace-dotnet-unwarmed: shared-hwtrace
   	@echo "== hwtrace-dotnet-unwarmed (live unwarmed whole-window compose) =="
   	$(hwtrace_env) $(hwtrace_dotnet_env) ASMTEST_UNWARMED_RETIER=1 \
   	  DOTNET_TC_AggressiveTiering=1 \
   	  $(DOTNET) run --project bindings/dotnet/hwtrace/hwtrace.csproj
   ```
   (`DOTNET_TC_AggressiveTiering` pushes tier-up to happen fast — inside the
   window — instead of suppressing it; a quieting/forcing knob, never a
   correctness precondition. `TieredCompilation=0` must NOT be set here: the lane
   exists to have the JIT run mid-window.)
2. In [mk/docker.mk](../../../mk/docker.mk), next to `docker-hwtrace-dotnet9`
   (:650), add `docker-hwtrace-dotnet-unwarmed: docker-dotnet` running
   `make hwtrace-dotnet-unwarmed` in `asmtest-dotnet` with
   `$(DOCKER_RUNENV_dotnet)`, and add it to the `.PHONY` list.
3. Add both to `make help` (root [Makefile](../../../Makefile), the hwtrace block
   around :121).
4. Run `make docker-hwtrace-dotnet-unwarmed` — TAP output, exit 0.

**Code.** Make only.

**Tests.** The lane is the test aggregation; the retier check only asserts here
(env-gated).

**Docs.** `CHANGELOG.md` `Added` (one line, lane name). Extend
[docs/scoped-tracing-implementation.md](../../scoped-tracing-implementation.md)'s
zero-config section: the §Z3 live half now has a Docker-testable lane over the
in-process WEAK tier and the §D3 OOP stepper; only the PT prong remains
silicon-gated (T5).

**Done when.**
- `make docker-hwtrace-dotnet-unwarmed` passes on any x86-64 Linux Docker host.
- `make help` lists it; `make docker-hwtrace-dotnet` (default suite) is unchanged
  apart from T1/T3's always-on checks.

### T5 — PT prong of the live compose  (S, depends on: T1, intel-pt-whole-window-substrate#T4)

**Goal.** When the STRONG PT window tier arms (Intel bare metal + libipt +
privilege), the same unwarmed checks run over the PT-backed window; everywhere
else they self-skip with the ladder's reason.

**Steps.**
1. After [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md)#T4
   lands its `new AsmTrace(HwBackend.IntelPt)` inline ctor, add a PT variant to
   `WholeWindowUnwarmedChecks` that re-runs the fixture under that ctor when
   `HwTrace.Available(HwBackend.IntelPt)`, else prints
   `# SKIP unwarmed/PT: {HwTrace.SkipReason(HwBackend.IntelPt)}` (on the AMD dev
   hosts: "no intel_pt PMU (needs bare-metal Intel; absent on AMD/VM)",
   [src/hwtrace.c:402](../../../src/hwtrace.c#L402)).
2. Assertions are identical to T1 (the seam is backend-agnostic by design —
   `render_versioned` and the map do not care which tier captured the addresses).

**Code.** C# only, ~30 lines.

**Tests.** Self-skipping on every current CI/dev host — a real hardware gate.
Record the skip line in the lane output as the validation artifact until a PT
runner exists.

**Docs.** Internal-only; the substrate doc owns the PT-tier user docs.

**Done when.**
- On AMD/Docker: the specific `# SKIP unwarmed/PT` line prints, exit 0.
- The check is written so that a future PT runner needs zero code changes — only
  hardware.

### T6 — Managed-runtime probe + safe-managed refusal in the region-free arm  (S, depends on: none)

**Goal.** `asmtest_hwtrace_begin_window` can detect "a managed runtime lives in
this process" and, under an opt-in policy, refuse the in-process `EFLAGS.TF` arm
with an actionable reason instead of stepping code whose SIGTRAP disposition
CoreCLR's PAL owns.

**Steps.**
1. In [src/hwtrace.c](../../../src/hwtrace.c), add
   `int asmtest_hwtrace_managed_runtime_present(void)`: a one-shot cached scan of
   `/proc/self/maps` for `libcoreclr.so`, `libjvm.so`, or `libmono`, overridable
   by `ASMTEST_ASSUME_MANAGED=1` (forces 1) for the C test (T9). Declare it in
   [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) next to the
   window ABI (:442). Non-Linux stub returns 0.
2. In `asmtest_hwtrace_begin_window` (:2554), before the single-step arm: if
   `getenv("ASMTEST_WHOLEWINDOW_SAFE_MANAGED")` is `"1"` **and**
   `asmtest_hwtrace_managed_runtime_present()`, return a **distinct** policy-refusal
   sentinel `ASMTEST_HW_EMANAGED`, **not** bare `ASMTEST_HW_EUNAVAIL` — the
   skip-reason surface (step 3) needs to tell a managed-refusal apart from a
   genuinely absent tier, and `WholeWindowSkipReason` (:2204) keys purely on the
   return code, so the two cases must not share one. Define `ASMTEST_HW_EMANAGED`
   in [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) at the next
   free value beside `ASMTEST_HW_EUNAVAIL` (:55; -4 and -7 are unused), mirror the
   constant in the `.NET` `HwNative` block next to `ASMTEST_HW_EUNAVAIL` (:104),
   and extend the `begin_window` return-contract comment (:438) to name it.
   **The default (env unset) is byte-identical to today** — the 2026-07-06
   decision (the shipped .NET WEAK tier accepts convention-mitigated in-process
   single-step of managed windows) stays the shipping default; this policy is the
   roadmap's safe arm, opt-in.
3. Extend the skip-reason surface: `WholeWindowSkipReason` (:2204) is a pure
   `rc`→string switch, and `ArmWholeWindow` already feeds `begin_window`'s return
   straight into it (:2177) — so the refusal reason surfaces by adding **one new
   switch arm** keyed on the `ASMTEST_HW_EMANAGED` sentinel from step 2, reading
   `"managed runtime present; in-process TF window refused (safe-managed) — use
   the out-of-process window or the PT tier"`. This is exactly why step 2 returns
   a distinct code: the existing `ASMTEST_HW_EUNAVAIL` arm keeps its generic
   "single-step tier unavailable…" text for a genuinely absent tier and never
   consults any native reason string, so a bare EUNAVAIL refusal would surface the
   wrong (generic) message. Follow the `hw_classify` string style (:350–:417).
4. Parity: add nine per-binding exemption lines for
   `asmtest_hwtrace_managed_runtime_present` to
   [scripts/bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt)
   (all but `dotnet`, which wraps it in T7), mirroring the
   `asmtest_dr_under_dynamorio` block. `make check-bindings-parity` green.
5. `make hwtrace-test` (or `docker-hwtrace`) — existing window tests unchanged.

**Code.** ~60 lines of C. The probe must be async-signal-irrelevant (called only
at arm time, never in the handler) and cached (`static int g_managed = -1`).

**Tests.** T9 gates the seam. Additionally assert in
`test_zeroctor_scope_hygiene`'s neighborhood that with both envs **unset** the
arm still succeeds (a regression tripwire for the default).

**Docs.** `CHANGELOG.md` `Added`: the safe-managed window policy env. Extend
[docs/guides/tracing/hardware-tracing.md](../../guides/tracing/hardware-tracing.md)
(the whole-window section) with the `ASMTEST_WHOLEWINDOW_SAFE_MANAGED` knob and
when to use it.

**Done when.**
- `ASMTEST_WHOLEWINDOW_SAFE_MANAGED=1 ASMTEST_ASSUME_MANAGED=1` makes a
  region-free single-step arm return `ASMTEST_HW_EMANAGED` (the refusal sentinel,
  distinct from the absent-tier `EUNAVAIL`); unset envs are byte-identical to
  today (all existing lanes pass).
- `make check-bindings-parity` passes.

### T7 — Safe managed routing in the .NET empty ctor  (M, depends on: T6; PT prong also intel-pt-whole-window-substrate#T3)

**Goal.** With safe-managed routing active, `new AsmTrace()` routes a managed
window to PT where the silicon exists, else to the §D3 out-of-process stepper —
never in-process TF — completing the §Z1.1 roadmap ("PT/LBR where the silicon
exists, else the §D3 stepper").

**Steps.**
1. In [bindings/dotnet/hwtrace/HwTrace.cs](../../../bindings/dotnet/hwtrace/HwTrace.cs),
   factor the body of the `AsmTrace(bool outOfProcess, …)` ctor (:2605) into a
   private `ArmOopWindow(bool byMethod, bool withRundown, int rundownSettleMs)`
   so the empty ctor can reuse it verbatim (channel alloc,
   `EnumerateManagedCodeRanges` seeding, `SetPublishChannel` live publish,
   `stealth_window_begin`, thread affinity, teardown on failure). Behavior of the
   existing ctor is unchanged (refactor only — the OOP checks in
   `HwTraceProgram.cs` prove it).
2. Give the empty ctor (:2105) a routing consult: `bool safeManaged =
   Environment.GetEnvironmentVariable("ASMTEST_WHOLEWINDOW_SAFE_MANAGED") == "1"`
   plus an explicit `safeManaged: bool? = null` parameter (parameter wins). When
   active and `HwNative.asmtest_hwtrace_managed_runtime_present() != 0`:
   - if the PT window tier arms (the ladder consult that
     [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md)#T3
     exposes; until it lands, `HwTrace.Available(HwBackend.IntelPt)` is the
     probe), arm PT — one PT arm, owned by the substrate; this ctor only calls
     it, never reimplements it;
   - else if `Ptrace.Available()` (:1416), call `ArmOopWindow(...)` — `_kind =
     Kind.OopInlineWindow`, so the existing `Dispose` path handles close and
     attribution;
   - else self-skip: `Armed=false`, `SkipReason` composes both misses
     ("PT unavailable (…); ptrace unavailable (…) — in-process TF refused under
     safe-managed routing").
   In-process TF is **never** selected on this path; with routing inactive the
   ctor behaves exactly as today.
3. Add `asmtest_hwtrace_managed_runtime_present` to `HwNative` DllImports (:353
   block).
4. Add `.NET` suite checks `SafeManagedRoutingChecks()`: with the env set via
   `Environment.SetEnvironmentVariable` (process-local) — a scope over a managed
   body must report either `Armed` with an OOP/PT capture (assert
   `ww.SkipReason == ""` and the capture attributed or truncated) or the composed
   two-miss SkipReason; and must **never** have armed in-process TF (expose the
   chosen route as `ww.Route` string — `"pt" | "oop" | "inproc"` — a one-line
   property set at arm).
5. `make docker-hwtrace-dotnet` green.

**Code.** C# only (T6 supplied the native probe). Keep the routing table in one
method with a doc-comment quoting the §Z1.1 roadmap sentence.

**Tests.** Step 4's checks (Docker-testable: ptrace works in the container, so
the OOP route is exercised live; the PT route self-skips). The `Route == "oop"`
assert is the live routing gate; T9 is its C-core twin.

**Docs.** `CHANGELOG.md` `Added`. Extend the hardware-tracing guide's
whole-window section: routing table (PT → OOP stepper → honest skip) + the env /
ctor parameter. Note the default remains the convention-mitigated in-process form.

**Done when.**
- In Docker: `ASMTEST_WHOLEWINDOW_SAFE_MANAGED=1` suite run shows the scope armed
  via the OOP route (`Route == "oop"`), captures attributed-or-truncated, exit 0.
- Default run: `Route == "inproc"`, all pre-existing checks unchanged.

### T8 — Block-step the stealth window  (M, depends on: none; consumed by T7's OOP quality)

**Goal.** The §D3 out-of-process window steps one `#DB` per **taken branch**
instead of per instruction (~4–10× fewer stops — measured 4.6× on the windowed
fixture, per the header note at
[include/asmtest_ptrace.h:161](../../../include/asmtest_ptrace.h#L161)), making a
managed whole-window affordable.

**Steps.**
1. In [src/ptrace_backend.c](../../../src/ptrace_backend.c), add
   `asmtest_ptrace_trace_attached_window_stop_blockstep(pid_t pid,
   asmtest_addr_channel_t *chan, volatile int *stop, asmtest_trace_t *trace)` —
   the stop-flag form of `asmtest_ptrace_trace_attached_windowed_blockstep`
   (:2380), sharing its block-reconstruction + signal-fallback inner loop the way
   `_attached_window_stop` (:2526) shares the per-instruction windowed loop
   (:2073). Same contract: kernel-injected transfers fall back to the
   per-instruction loop for the window's remainder; the stream stays
   byte-identical. Declare it in
   [include/asmtest_ptrace.h](../../../include/asmtest_ptrace.h) next to :147;
   ENOSYS stub in the non-Linux block.
2. In [src/stealth_helper.c](../../../src/stealth_helper.c)
   `asmtest_stealth_helper_run_window_async` (:199): prefer the new entry when
   `asmtest_ptrace_blockstep_available()` and
   `getenv("ASMTEST_STEALTH_NO_BLOCKSTEP")` is not `"1"`; else the existing
   per-instruction call. (The probe is hang-proof and returns 0 under
   `DEBUGCTL.BTF`-masking hypervisors — the helper then silently uses the exact
   per-instruction path, per the landed blockstep posture.)
3. Parity: add an `ALL asmtest_ptrace_trace_attached_window_stop_blockstep` line
   to `bindings-parity-allow.txt` (only the stealth helper calls it; no binding
   wraps it), with a comment.
4. C test in [examples/test_hwtrace.c](../../../examples/test_hwtrace.c):
   `test_window_stop_blockstep_parity`, mirroring `test_ptrace_windowed_blockstep`
   (:4524) but through the stop-flag entry: trace the same windowed fixture via
   `_window_stop` and `_window_stop_blockstep`, assert identical `insns[]`,
   `blocks[]`, `truncated`; self-skip with `# SKIP` where
   `blockstep_available()==0`. Register it in `main` beside :8275.
5. `make hwtrace-test && make docker-hwtrace` green; then
   `make docker-hwtrace-dotnet` (the .NET OOP checks now ride blockstep where the
   host supports it — output identical by contract).

**Code.** ~150 lines C (mostly the shared-loop plumb-through), one env knob.

**Tests.** Step 4. Also run the T3 OOP unwarmed check with
`ASMTEST_STEALTH_NO_BLOCKSTEP=1` once (manual) to confirm A/B equality of
attribution counts on a fixed fixture.

**Docs.** `CHANGELOG.md` `Changed`: "the out-of-process whole-window stepper
block-steps where PTRACE_SINGLEBLOCK is functional (~4–10× fewer stops);
`ASMTEST_STEALTH_NO_BLOCKSTEP=1` forces per-instruction."

**Done when.**
- Parity test green in `docker-hwtrace`; self-skips (printed) on
  blockstep-masking hosts.
- `docker-hwtrace-dotnet` unchanged output, measurably faster wall-clock on the
  OOP checks (note the before/after in the commit message).

### T9 — Revive `test_wholewindow_ss_managed_routes`  (S, depends on: T6)

**Goal.** The C suite gates the safe-managed seam: with the policy armed, a
region-free arm in a "managed" process refuses in-process TF; with it unset, the
arm is unchanged.

**Steps.**
1. Add `test_wholewindow_ss_managed_routes` to
   [examples/test_hwtrace.c](../../../examples/test_hwtrace.c) near
   `test_wholewindow_singlestep` (:7030). The plan struck the original draft
   because the seam did not exist and a C binary has no managed runtime; T6 built
   the seam and gave the probe an override, so the revived test asserts the
   ROUTING MECHANICS with `setenv("ASMTEST_ASSUME_MANAGED", "1", 1)` +
   `setenv("ASMTEST_WHOLEWINDOW_SAFE_MANAGED", "1", 1)`. Put that honesty in the
   test comment: managed-ness is faked here; the live managed routing is the .NET
   suite's `SafeManagedRoutingChecks` (T7).
2. Asserts: (a) `asmtest_hwtrace_begin_window` returns `ASMTEST_HW_EMANAGED` (the
   T6 policy-refusal sentinel, not the absent-tier `EUNAVAIL`) and
   no SIGTRAP disposition was installed (probe `sigaction` before/after, the
   `test_zeroctor_scope_hygiene` technique); (b) after `unsetenv` of both, the
   same arm succeeds and captures the native leaf (the default-unchanged
   regression guard); (c) with only `ASSUME_MANAGED` set (policy off) the arm
   still succeeds — the probe alone must never change behavior.
3. Register in `main`; `make hwtrace-test` and `make docker-hwtrace` green.

**Code.** ~70 lines C.

**Tests.** This is the test. Failure: `not ok … safe-managed arm refused TF`
or a `sigaction` mismatch.

**Docs.** Internal-only — the plan's §Z1 test list is updated by the
zeroconfig hardening doc (see Out of scope); this doc's tests speak for
themselves.

**Done when.**
- `make docker-hwtrace` passes with the three asserts; count grows, 0 failed.

### T10 — Per-tid decode-at-disable PT hop surface  (M, depends on: intel-pt-whole-window-substrate#T1)

**Goal.** A native pair — open a per-thread PT event on a given tid; disable it,
drain the quiesced AUX ring, decode against the version live in the window — the
capture half of the §Z4 opt-in stitching escalation.

**Steps.**
1. In [src/pt_backend.c](../../../src/pt_backend.c) (inside the
   `ASMTEST_HAVE_LIBIPT` region), building **only** on the substrate's shared
   AUX open/mmap/drain/decode helpers (one PT arm — never a parallel PT
   implementation; parameterize the substrate's open helper with a `tid`
   argument, default self):
   ```c
   int asmtest_hwtrace_pt_hop_open(int tid, void **ctx_out);
   int asmtest_hwtrace_pt_hop_close(void *ctx, asmtest_codeimage_t *img,
                                    uint64_t when, asmtest_trace_t *out);
   ```
   `hop_open` opens `perf_event_open(pid=tid, cpu=-1, inherit=0)` with the
   `intel_pt` PMU type + an AUX ring. `inherit=0` is mandatory: an inherit=1
   per-task cpu==-1 event cannot mmap a sampling/AUX buffer, and per-thread PT
   mode has no inheritance — threads spawned later are silently untraced (see
   Research notes). `hop_close` issues `PERF_EVENT_IOC_DISABLE`, drains the AUX
   ring **after** the disable (overwrite-mode readers must disable before
   reading — the man-page contract), decodes via `asmtest_pt_decode_window`
   (:224) with the recorder callback against `img`/`when`, appends into `out`,
   closes fds, frees ctx.
2. ENOSYS/EUNAVAIL stubs in the `#else` block (:363 region) so every host links.
3. Declare both in `asmtest_hwtrace.h`; parity-exempt the nine non-dotnet
   bindings (T6 pattern).
4. Host-testable twin `test_pt_hop_surface` in `test_hwtrace.c`: where libipt is
   absent or no `intel_pt` PMU, `hop_open` returns the honest status (assert +
   `# SKIP`); where libipt is built, drive `hop_close`'s decode path directly by
   constructing a ctx around a synthetic AUX buffer from
   `asmtest_pt_encode_fixture` (:305) — the decode leg is then exercised with
   zero silicon, exactly like `test_wholewindow_decode` (:3160). Expose the
   internal ctx constructor under a test-only `asmtest_pt_hop_ctx_for_fixture`
   or make the twin drive `asmtest_pt_decode_window` through the same code path
   the real `hop_close` calls (either is acceptable; keep it honest about which
   leg is covered).
5. `make hwtrace-test` (self-skips or synthetic-decodes), `make docker-hwtrace`.

**Code.** ~200 lines C. No new perf plumbing beyond the tid parameter — reuse is
the point.

**Tests.** Step 4. The live per-tid capture is Intel-PT-hardware-gated (record in
Constraints; the T12 stub twin covers the consumer logic).

**Docs.** Internal-only until the ambient producer ships (T11 owns the guide
note).

**Done when.**
- On AMD/Docker: `# SKIP pt hop: …` (no intel_pt PMU / built without libipt),
  suite green.
- Where libipt is built (x86_64 `hwtrace-test` lane): the synthetic decode leg
  asserts the fixture walk decodes through the hop-close path.

### T11 — Ambient AsyncLocal stitching producer (.NET)  (L, depends on: T10)

**Goal.** The §Z4 opt-in escalation: `using (var op = new
AsmAmbientStitchedTrace()) { await …; }` captures each thread's execution as a
per-thread PT slice, decoding at each hop's **disable**, and stitches slices into
one logical-operation trace at close — zero calls in the user's body (unlike
`AsmStitchedTrace.Step`).

**Steps.**
1. New class `AsmAmbientStitchedTrace` in
   [bindings/dotnet/hwtrace/HwTrace.cs](../../../bindings/dotnet/hwtrace/HwTrace.cs),
   beside `AsmStitchedTrace` (:4953). Core: a static
   `AsyncLocal<AmbientScope>` constructed **with a valueChangedHandler** — the
   `AsyncLocal<T>(Action<AsyncLocalValueChangedArgs<T>>)` ctor — which the
   runtime invokes synchronously on the exact OS thread performing every
   execution-context transition (see Research notes):
   - `args.ThreadContextChanged == true && args.CurrentValue != null` →
     **thread-attach**: the logical flow landed on this thread; call
     `hop_open(gettid)` (expose the OS tid via a tiny `asmtest_ss_self_tid`
     P/Invoke — it is already exported from
     [src/ss_backend.c:574](../../../src/ss_backend.c#L574)), record a hop edge
     `(scope_id, seq++, tid)`.
   - `args.ThreadContextChanged == true && args.CurrentValue == null` →
     **thread-detach** (the pool thread is returning / the flow left): call
     `hop_close(ctx, img, when, trace)` — decode-at-disable against the version
     live in the window (`img` = the scope's `JitMethodMap` trackBytes codeimage;
     `when` = `asmtest_codeimage_now` at the disable) — and park the populated
     trace handle with its `(scope_id, seq, tid, version)`.
   - `ThreadContextChanged == false` (explicit `Value` set) → bookkeeping only.
   Handlers fire only when the boxed value differs across the transition, so use
   one boxed `AmbientScope` instance per operation (set in the ctor, cleared in
   `Complete`) — transitions between "this operation" and "no operation" then
   fire exactly the attach/detach pairs.
2. `Complete()`/`Dispose()`: close the current thread's open hop, then
   `asmtest_hwtrace_stitch_handles` (:2754) over the parked handles in seq order
   — the same call `AsmStitchedTrace.Complete` already makes; populate `Hops`,
   `Path` (per-slice `render_versioned` text), `Truncated`, `SkipReason`.
3. Capture goes through an internal seam `IHopCapture { Open(tid); CloseDecode(…) }`
   with the real implementation P/Invoking T10 and the availability probe
   deciding at ctor time: when PT per-thread events are unavailable the scope
   sets `SkipReason` ("ambient stitching needs Intel PT per-thread events —
   the explicit-Step AsmStitchedTrace works everywhere") and runs the body
   uninstrumented — never a hard failure, never in-process TF (single-step is
   forbidden here; the §D3 stepper follows a single thread and cannot exercise a
   hop, which is why this producer is PT-only).
4. Handler discipline: the callback runs inline on hot paths
   (`ExecutionContext.RunInternal`, the thread-pool dispatch loop), so it must be
   allocation-light and lock-free (a `ConcurrentQueue` of parked slices, an
   `Interlocked` seq); any exception must be swallowed (the `PublishLoop` :3958
   posture — a dead producer only loses stitching, never the process).
5. Suite check `AmbientStitchChecks()` (live half): construct the scope, run an
   `await Task.Run(...)` hop, `Complete`; on `SkipReason != ""` (every current
   host) assert the reason mentions PT and print `# SKIP`; on a PT host assert
   `Hops.Count >= 2`, seq-ordered tids, non-empty `Path`.

**Code.** ~300 lines C#. Do not modify `AsmStitchedTrace` — the explicit-Step
software tier remains the everywhere-runnable form; the ambient class is the
opt-in escalation the plan specifies.

**Tests.** Step 5 (self-skipping live) + T12 (host-testable logic twin). The full
live chain needs bare-metal Intel PT **and** a live managed runtime — a real
hardware gate with no CI coverage; say so in the class doc-comment.

**Docs.** `CHANGELOG.md` `Added`. Hardware-tracing guide: an "ambient stitched
operations (opt-in, Intel PT)" subsection — the default remains the honest
thread-window with the `Truncated` hop flag (never auto-stitch), per §Z4.

**Done when.**
- `make docker-hwtrace-dotnet`: `# SKIP ambient stitching: …PT…` on AMD/Docker,
  suite green.
- Mutation check once by hand: registering the handler and hopping WITHOUT PT
  must leave the body's results untouched (uninstrumented run).

### T12 — Host-testable ambient bookkeeping twin  (S, depends on: T11)

**Goal.** The hook→tag→merge chain's *logic* is CI-covered everywhere, leaving
only the PT capture itself hardware-gated.

**Steps.**
1. Add a stub `IHopCapture` (internal, test-only): `Open` records the tid,
   `CloseDecode` fills the trace with a scripted per-thread offset pattern —
   the .NET twin of `test_stitch_hops_scripted` (:2997).
2. Suite check `AmbientHookTwinChecks()`: with the stub injected, run
   `var op = new AsmAmbientStitchedTrace(captureForTest: stub)` over
   `await Task.Run(…)` + a second `Task.Run`; assert:
   - the handler fired an attach on a pool thread with `CurrentValue != null` and
     a detach with `CurrentValue == null` (the documented
     `ResetThreadPoolThread` pair — this asserts our reliance on the runtime
     contract, so a future runtime that changes it fails HERE, loudly, and
     `hwtrace-dotnet9-test` (:2597) catches drift on .NET 9);
   - parked slices are seq-dense, tids match the recording threads, and
     `stitch_handles` output preserves seq order with correct `bounds` —
     mirroring the C oracle's assertions;
   - a synchronous no-hop scope yields exactly one slice (byte-identical
     degenerate case, per §Z4's table).
3. `make docker-hwtrace-dotnet` green everywhere (no PT needed).

**Code.** ~120 lines C#.

**Tests.** This is the test; it is the "synthetic-input host-testable twin" the
plan's no-untested-hardware-code rule demands for T11's live path.

**Docs.** Internal-only (T11 carried the user-facing docs).

**Done when.**
- The twin passes in `docker-hwtrace-dotnet` on this AMD host and in plain
  Docker, and `docker-hwtrace-dotnet9` still passes (contract-drift tripwire).

## Task order & parallelism

Three independent tracks; two people can work tracks in parallel:

- **Track A (live compose, Docker-testable now):** T1 → {T2, T3} → T4, with T5
  branching off T1 (T5 extends T1's `WholeWindowUnwarmedChecks` — which Main runs
  in the default suite too — and also waits on intel-pt-whole-window-substrate#T4;
  its skip line is merely recorded in T4's lane output, so the T4 → T5 order is
  soft, not a hard dependency — matching T5's header set of T1 + substrate#T4).
- **Track B (safe managed routing, Docker-testable now):** T6 → {T7, T9}; T8 is
  independent and can land any time (T7 benefits from it but does not require
  it).
- **Track C (ambient PT stitching, substrate-gated):**
  intel-pt-whole-window-substrate#T1 → T10 → T11 → T12.

Critical path to "all Docker-testable work done": T1 → T3 → T4 alongside
T6 → T7. Critical path to the full §Z4 escalation: the PT substrate → T10 → T11
→ T12, with live validation blocked on Intel PT hardware.

## Constraints & gates

- **Real gates (self-skip, record the reason):** the PT prongs (T5, T10 live
  capture, T11 live chain) need bare-metal Intel PT + `perf_event_paranoid < 0`
  or `CAP_PERFMON`; T11's live hop additionally needs a live managed runtime
  (the single-thread §D3 stepper cannot exercise a hop). Soft-dirty
  (`CONFIG_MEM_SOFT_DIRTY`) absence degrades versioning — a host-kernel
  self-skip, printed, in T1/T2. `PTRACE_SINGLEBLOCK` may be masked by
  hypervisors — T8's probe self-skips there. No credentials are involved.
- **Not gates (CLAUDE.md rule):** .NET is installable — every dotnet lane runs in
  the `asmtest-dotnet` image (`DOCKER_APT_dotnet := dotnet-sdk-8.0`,
  [mk/docker.mk:149](../../../mk/docker.mk#L149)); ptrace works under default
  Docker seccomp (host kernel ≥ 4.8). No task in tracks A/B may self-skip for a
  missing installable dependency — extend the image instead.
- **Pinning:** no new third-party fetches anywhere in this doc; the only runtime
  dependency is the already-pinned apt `dotnet-sdk-8.0`. Tiering knob values cite
  release/8.0 sources (below); `DOTNET_TC_*`/`OSR_*` are unsupported internal
  config surface — the lanes must treat them as quieting/forcing knobs, never
  correctness preconditions (the seam decodes at-version by design).
- **ABI/parity:** every new tier-header symbol either gets ten binding wrappers
  or an explicit `bindings-parity-allow.txt` entry (T6/T8/T10 name theirs);
  `make check-bindings-parity` must stay green after each task.
- **One PT arm:** all PT work consumes the substrate helpers from
  [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md); this
  doc adds no parallel PT open/mmap/drain/decode path.
- **AMD LBR floor:** where any message or doc touched here mentions the LBR
  ceiling tier, the live-capture floor is **Zen 4+** (see
  [amd-branchsnap-lbr-docs.md](amd-branchsnap-lbr-docs.md)).

## Research notes (verified 2026-07-17)

- **.NET tiering/method-load knobs (release/8.0, matches the pinned
  `dotnet-sdk-8.0`).** `DOTNET_TieredCompilation` (default 1),
  `DOTNET_TC_QuickJit` (1), `DOTNET_ReadyToRun` (1; 0 forces BCL to JIT — more
  MethodLoad events), `DOTNET_TieredPGO` (1; 0 = one fewer code-version
  transition per hot method) —
  <https://learn.microsoft.com/en-us/dotnet/core/runtime-config/compilation>.
  Internal knobs (names verified in
  <https://github.com/dotnet/runtime/blob/release/8.0/src/coreclr/inc/clrconfigvalues.h>,
  lines 562–621): `DOTNET_TC_CallCountThreshold` (default 30 calls to tier-1),
  `DOTNET_TC_CallCountingDelayMs` (100), `DOTNET_TC_BackgroundWorkerTimeoutMs`
  (4000 — the repo already pins it to 0x36EE80 in `hwtrace_dotnet_env`),
  `DOTNET_TC_CallCounting` (1; 0 = tier-0 forever, stable address),
  `DOTNET_TC_AggressiveTiering` (0; 1 = promote fast — T4 uses it to pull
  tier-up INTO the window), `DOTNET_OSR_HitLimit` (10) /
  `DOTNET_TC_QuickJitForLoops` (env default 1 in 8.0 — the Learn page's
  "loops don't quick-jit" prose is stale for env-var behavior; source is
  authoritative). Tier-up and OSR each raise a fresh `MethodLoadVerbose` with a
  new `MethodStartAddress`
  ([scoped-inprocess-tracing.md](../analysis/scoped-inprocess-tracing.md):
  "TieredCompilation=0 is no longer needed to be right — only to be quiet",
  :665) — the listener already tolerates moves; knobs only control how many
  occur inside the window.
- **MethodLoadVerbose wire facts.** Provider `Microsoft-Windows-DotNETRuntime`;
  `MethodLoadVerbose` id 143 (V0/V1/V2), level **Informational (4)** per the
  manifest (not Verbose, despite one Learn-page sentence), keyword JitKeyword
  0x10; `MethodILToNativeMap` id 190 under keyword 0x20000 at Verbose(5). V2
  payload: MethodID u64, ModuleID u64, MethodStartAddress u64, MethodSize u32,
  MethodToken u32, MethodFlags u32, three unicode strings, ReJITID u64,
  ClrInstanceID u16. In-proc `EventListener`s get **no rundown** — only methods
  JIT'd after keyword enable (matches the `JitMethodMap` doc-comment); the
  out-of-proc rundown provider is `Microsoft-Windows-DotNETRuntimeRundown`
  (MethodDCStart/DCEndVerbose_V2 = 143/144). Sources:
  <https://github.com/dotnet/runtime/blob/release/8.0/src/coreclr/vm/ClrEtwAll.man>,
  <https://learn.microsoft.com/en-us/dotnet/fundamentals/diagnostics/runtime-method-events>,
  <https://learn.microsoft.com/en-us/dotnet/core/diagnostics/eventpipe>.
- **AsyncLocal valueChangedHandler semantics (T11's contract).** The
  `AsyncLocal<T>(Action<AsyncLocalValueChangedArgs<T>>)` ctor registers a
  handler called "whenever the current value changes on any thread";
  `ThreadContextChanged` is true when the change came from an execution-context
  transition. Runtime source (dotnet/runtime commit d099f07,
  `ExecutionContext.cs`): `RestoreChangedContextToThread` fires
  `OnValuesChanged` only when the boxed value differs across the transition,
  with `contextChanged:true`, from `RunInternal` (every await resumption),
  `RunFromThreadPoolDispatchLoop`, `RestoreInternal`, and
  `ResetThreadPoolThread` — the last resets a pool thread to the default
  context when a work item completes, yielding the attach (non-null
  CurrentValue on landing) / detach (null CurrentValue on return-to-pool) pair
  T11 keys on. Handlers run synchronously inline on the transitioning OS
  thread — a valid point to enable/disable that thread's per-tid PT event.
  Sources:
  <https://learn.microsoft.com/en-us/dotnet/api/system.threading.asynclocal-1.-ctor>,
  <https://learn.microsoft.com/en-us/dotnet/api/system.threading.asynclocalvaluechangedargs-1.threadcontextchanged>,
  <https://github.com/dotnet/runtime/blob/d099f075e45d2aa6007a22b71b45a08758559f80/src/libraries/System.Private.CoreLib/src/System/Threading/ExecutionContext.cs>.
- **Per-thread perf/PT rules (T10's contract).** `perf_event_open(pid>0,
  cpu==-1)` measures one task on any CPU; `inherit` applies only to new
  children, and "using it together with cpu == -1 prevents the creation of the
  mmap ring-buffer" — so per-tid PT AUX capture requires `inherit=0`.
  perf-intel-pt(1): "In per-thread mode an exact list of threads is traced.
  There is no inheritance." — new pool threads are invisible until opened
  per-tid (which is exactly what the attach callback does). Overwrite-mode AUX:
  "it is the consumer's job to disable measurement while reading" —
  decode-at-disable is the sanctioned read point; `PERF_EVENT_IOC_DISABLE`
  stops the event (group leader stops the group). Sources:
  <https://www.man7.org/linux/man-pages/man2/perf_event_open.2.html>,
  <https://man7.org/linux/man-pages/man1/perf-intel-pt.1.html>,
  <https://man7.org/linux/man-pages/man1/perf-record.1.html>,
  <https://linux.die.net/man/2/perf_event_open>.
- **Jit-byte recovery context (why the compose does not need jitdump here).**
  CoreCLR natively writes `/tmp/jit-<pid>.dump` under `DOTNET_PerfMapEnabled`
  (jitdump=2, perfmap=3; IPC commands EnablePerfMap 0x0405 / DisablePerfMap
  0x0406 in .NET 8's `Microsoft.Diagnostics.NETCore.Client`) — the repo's
  rundown path already uses this; the in-window byte source for T1/T2 is the
  trackBytes codeimage, not jitdump. Sources:
  <https://learn.microsoft.com/en-us/dotnet/core/runtime-config/debugging-profiling>,
  <https://github.com/dotnet/diagnostics/blob/main/documentation/design-docs/ipc-protocol.md>,
  <https://raw.githubusercontent.com/torvalds/linux/master/tools/perf/Documentation/jitdump-specification.txt>.

## Out of scope

- **The PT substrate itself** — perf-AUX open/mmap/drain/decode helpers, the
  STRONG `begin_window` PT arm, the WEAK/STRONG/CEILING ladder, the
  `pt_begin_window`/`pt_end_window` pair and the .NET `HwBackend.IntelPt` ctor:
  [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md).
- **PT attach to a foreign pid** (tracing another process's window):
  [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md).
- **Zero-config plan bookkeeping** (rewriting the plan's stale "Consumed" table —
  P2/P3 are landed — and the broader §Z docs/UX hardening):
  [zeroconfig-scoped-tracing-hardening.md](zeroconfig-scoped-tracing-hardening.md).
- **Correctness fixes to the existing ptrace/blockstep tracer** (si_code
  handling, forwarding app breakpoints via PTRACE_CONT, etc.):
  [ptrace-blockstep-tracer-correctness.md](ptrace-blockstep-tracer-correctness.md).
- **In-process BTF (no ptrace child; ring-0 DEBUGCTL)**:
  [inproc-btf-block-step.md](inproc-btf-block-step.md).
- **AMD LBR ceiling-tier docs and the Zen 4+ floor correction**:
  [amd-branchsnap-lbr-docs.md](amd-branchsnap-lbr-docs.md).
- **IL↔native / bytecode attribution** (MethodILToNativeMap consumers):
  [native-il-bytecode-attribution.md](native-il-bytecode-attribution.md).
