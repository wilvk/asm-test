# asm-test — Zero-config whole-window scoped tracing (the empty-ctor `using (new AsmTrace())` form): implementation plan

> **Reconciliation note (2026-07-07):** moved back to `plans/` from `archive/` — this plan
> is **not fully landed** and should not have been archived under the "done ⇒ archive" rule.
> The region-free **WEAK** single-step path (§Z0 + §Z1) ships and is host-tested, but the
> STRONG whole-window PT / AMD-LBR tiers (§Z2), the full managed compose (§Z3), and
> default-arm escalation (§Z4) remain **forward-look**, gated on a synthetic Intel-PT
> fixture / bare-metal PT hardware and a live .NET runtime. The per-phase status block
> below is accurate.

The **capstone slice** of the scoped-tracing set. It sequences the remaining work to
collapse the shipped `AsmTrace` constructor down to its aspirational **empty** form —
`using (new AsmTrace()) { HotPath(data); }` — where an arbitrary managed method's
executed JIT assembly is captured and rendered on scope close with **no configuration,
no `NativeCode`, and no `[base,len)`**. The developer-visible surface reduces to the
**package import plus the scope**; everything else is discovered, armed, decoded, and
degraded honestly behind it.

This is a **sibling** of the three scoped-tracing slice plans
([scoped-tracing-core-plan.md](../archive/plans/scoped-tracing-core-plan.md),
[scoped-tracing-bindings-plan.md](../archive/plans/scoped-tracing-bindings-plan.md),
[scoped-tracing-managed-plan.md](scoped-tracing-managed-plan.md)) and of
[hardware-trace-plan.md](../plans/hardware-trace-plan.md). Like the umbrella
[scoped-inprocess-tracing-plan.md](../archive/plans/scoped-inprocess-tracing-plan.md), it **adds no new
capture primitive or decoder** — it **repackages** shipped machinery into a region-free
surface and composes existing seams. Read
[Analysis: the scoped `using` model](../analysis/scoped-inprocess-tracing.md) first for
*why* it works and the per-binding feasibility matrix; this document is the *how* and
*in what order*.

The **only genuinely new** engineering is a region-free arm surface plus the arm-time
composition and honest-degradation UX that wire shipped-but-disconnected pieces together —
the [§Z0–§Z5 phases](#sequencing-dependency-correct-order) below own the split.

> **Status: partially reached — the region-free WEAK path now works end-to-end on any
> x86-64 Linux; three hardware/runtime gaps remain to the full managed target.**
> (1) ~~Region is mandatory~~ **DONE (§Z0 + §Z1 WEAK):** the region-free arm surface ships —
> `asmtest_hwtrace_begin_window`/`_end_window`/`_render_window`
> ([src/hwtrace.c](../../../src/hwtrace.c)) over `asmtest_ss_begin_window`
> ([src/ss_backend.c](../../../src/ss_backend.c)), with the parameterless `new AsmTrace()`
> ctor in the .NET reference shim. `using (new AsmTrace())` renders a real trace of a
> native leaf on this AMD host today (single-step WEAK tier; host-tested 201/0 + 33/0).
> The remaining gaps are all about the *managed / non-intrusive* target:
> (2) **Whole-window PT decode is stubbed/unvalidated:** the real
> `read_recorder`-backed body ([src/pt_backend.c:198](../../../src/pt_backend.c#L198)) is
> `#ifdef ASMTEST_HAVE_LIBIPT` ([:62](../../../src/pt_backend.c#L62)); every current CI/AMD
> host compiles the `ENOSYS` stub ([:266](../../../src/pt_backend.c#L266)) — **needs a
> synthetic PT-packet fixture (built with libipt) to trust, then bare-metal Intel PT to
> run**. (3) **Managed byte-resolution is partial:** the self recorder works
> ([src/codeimage.c:314](../../../src/codeimage.c#L314)) and
> `asmtest_hwtrace_render_versioned` exists
> ([src/hwtrace.c:1172](../../../src/hwtrace.c#L1172)), but nothing discovers a method's
> `(address,size,version)` (MethodLoadVerbose wiring is Managed §D0.1, to-be-added) and
> `AsmTrace.Dispose` still calls the version-**blind** `asmtest_hwtrace_render`
> ([:1106](../../../src/hwtrace.c#L1106),
> [HwTrace.cs:1016](../../../bindings/dotnet/hwtrace/HwTrace.cs#L1016)) — **needs a live
> .NET 8+ runtime**. (4) **Scope is a thread window:** `asmtest_hwtrace_end` merely
> flags `truncated` on a cross-thread close ([src/hwtrace.c:898-907](../../../src/hwtrace.c#L898))
> and `asmtest_ss_end` no-ops when the closing thread holds no frame
> ([src/ss_backend.c:279](../../../src/ss_backend.c#L279)); the merge core
> `asmtest_hwtrace_stitch` ([:1233](../../../src/hwtrace.c#L1233)) has **no upstream
> producer**. The whole facility is **Linux-only** across every binding; on this AMD host a
> clean no-op self-skip absent privilege (§Z1's *AMD fork*: with `CAP_PERFMON` a bounded LBR
> tail, with ptrace the complete §D3-stepper window). Full accounting + the
> Docker-can/can't matrix:
> [docs/scoped-tracing-implementation.md](../../scoped-tracing-implementation.md).
>
> Status legend: **planned** unless noted.

---

## The ask, and the determination

The developer-visible target — the entire footprint the empty ctor promises:

```csharp
using AsmTest;                     // the only import

using (new AsmTrace())             // empty ctor: no NativeCode, no [base,len); arms lazily
{
    HotPath(data);                 // an ARBITRARY managed method — zero developer knowledge
}                                  // its executed JIT asm is captured + rendered on Dispose
```

**Determination: a qualified yes — reachable, but only on a narrow host and only after
building §Z1–§Z4, and effectively a no-op self-skip on this AMD host and most others.** The
empty ctor renders a real, honest whole-window trace of arbitrary synchronous same-thread
managed code **only** on **Intel bare-metal Linux with a one-time privilege grant**
(`perf_event_paranoid < 0` / `CAP_PERFMON`, plus a live **.NET 8+** runtime for
MethodLoadVerbose), and only once its decode (`asmtest_pt_decode_window`,
[src/pt_backend.c:198](../../../src/pt_backend.c#L198)) — **currently unvalidated on the required
silicon** — is trusted behind §Z2's fixture. The one thing the empty ctor makes *easier* than
the attach case is the byte source: the self `pid==0` code-image recorder
([src/codeimage.c:314](../../../src/codeimage.c#L314)) reads the JIT's own bytes, so no
cross-process channel is needed synchronously. Everywhere else — this AMD host, VMs, Docker,
macOS, off Linux — the **same code self-skips and says why**, accepts a second-process ptrace
stealth stepper (timing-intrusive; the **complete** AMD form, block-step-cheapened once
[amd-tracing-plan.md](../plans/amd-tracing-plan.md) Part III Phase 2 lands), renders a bounded LBR tail
on Zen 3+ (`truncated`), or accepts intrusive single-step on native leaves only,
and **never hard-fails**. `Net:` the empty ctor is **Linux-only across every binding** and
**cannot be a default**.

---

## What already ships vs. what this plan adds

This plan adds **no new capture primitive**. Every way of getting instruction bytes off a
running thread — single-step `EFLAGS.TF`, Intel PT, AMD LBR, the out-of-process ptrace
stepper, the self `pid==0` code-image recorder — already exists and is owned by a sibling
slice plan. The zero-config work here is **repackaging**: a region-free arm surface, an
arm-time composition seam, an honest presentation, and the async-default decision.

### Consumed — owned/specified by the sibling slice plans (some themselves forward-look); referenced, not re-specified

See each slice plan for the contract. **State** flags rows that are *not yet built or
validated in their owning plan* — this table is not an inventory of existing, validated
code.

| Slice | Ref | Consumed deliverable | State |
|---|---|---|---|
| Core | §0 | Error-returning `try_begin` ([:739](../../../src/hwtrace.c#L739)); arm-tid record + `asmtest_hwtrace_arm_tid` + cross-thread `truncated` ([:844](../../../src/hwtrace.c#L844), [:898-907](../../../src/hwtrace.c#L898)); size-then-alloc render-on-close; idempotent-by-name `register_region` ([:432](../../../src/hwtrace.c#L432)) | shipped (arm-tid backstop is **region-keyed**; see §Z4) |
| Core | §1 | Per-thread hwtrace state + handle-keyed `begin_scope`/`render_scope`/`render_versioned` — [:1121](../../../src/hwtrace.c#L1121), [:1157](../../../src/hwtrace.c#L1157), [:1172](../../../src/hwtrace.c#L1172) | shipped |
| Core | §2 | Recorder-backed libipt whole-window decode glue (`read_recorder`→`asmtest_pt_decode_window`, **no** in-region filter — [:96](../../../src/pt_backend.c#L96), [:198](../../../src/pt_backend.c#L198)) + capture-side `PERF_EVENT_IOC_SET_FILTER` + the anon-JIT constraint | **planned** — `#ifdef`-gated, unvalidated end-to-end (§Z2 validates it); PT-hardware-gated |
| Core | §3.1 | `asmtest_hwtrace_symbolize_bucket` ([:1384](../../../src/hwtrace.c#L1384)) + address→name `asmtest_hwtrace_region_name` ([:1292](../../../src/hwtrace.c#L1292)) | shipped (host-tested by `test_symbolize_bucket`); the eBPF PROT_EXEC emission-slicer it composes is **planned/gated** (CAP_BPF + BTF) |
| Core | §3.2 | Snapshot drain (PT `aux_tail` / AMD `data_tail` — keep the tail, flag `truncated`) | reconstruction shipped; live drains **planned** |
| Bindings | — | `.NET AsmTrace : IDisposable` reference shim + `[ModuleInitializer]` lazy arm + `[CallerMemberName]`/`[CallerLineNumber]` auto-name; shim-shape invariants (no slot / no SIGTRAP at import, self-skip-clean, closing-tid assert) | shipped (region form) |
| Managed | §D0.1/§D0.2 | `MethodLoadVerbose_V2` in-proc listener → name→(addr,size,version) map feeding `asmtest_codeimage_track`; pre-arm `EnablePerfMap(JitDump)` rundown + self-recorder fallback | **planned** — needs a live runtime |
| Managed | §D0.4/§D4 | `AsyncLocal<ScopeId>` async-hop hook; `asmtest_hwtrace_stitch` merge core + slice/bound ABI ([:1233](../../../src/hwtrace.c#L1233), [asmtest_hwtrace.h:246](../../../include/asmtest_hwtrace.h#L246)) | hook **planned**; merge core shipped (host-tested) but **no upstream producer** |
| Managed | §D3 | Concealed out-of-process ptrace-stealth stepper (bundled `asmtest-stealth-helper`, reverse-attach) | stepper shipped; live-JIT address channel **planned** |
| AMD | Part III P2/P3 | **P2** BTF block-step mode for the W2/§D3 ptrace stepper (`asmtest_ptrace_*_blockstep` — one `#DB` per **taken branch**, ~4–10× fewer stops than per-insn, rootless, every Zen incl. Zen 2); **P3** eBPF `bpf_get_branch_snapshot` boundary capture (deterministic ≤16-entry LBR window at scope entry/exit; Zen 4/5, Linux ≥ 6.10) | **planned** — [amd-tracing-plan.md](../plans/amd-tracing-plan.md) Part III; consumed as upgrades when they land, **not** Z0–Z5 prerequisites |
| Substrate | — | Backend auto-selection + `available()`/`skip_reason()` self-probe + the self `pid==0` code-image recorder — [codeimage.c:310](../../../src/codeimage.c#L310), [:373](../../../src/codeimage.c#L373) | shipped |

### Net-new — what this plan owns (surface, integration, presentation)

Referenced elsewhere as **netNew #N**. Not one row is a new capture primitive.

| # | Net-new deliverable | Phase(s) | Why it is new *(no shipped path does it)* |
|---|---|---|---|
| 1 | Region-free whole-window **capture MODE** + C arm entry point (`asmtest_hwtrace_begin_window`, or a `WHOLE_WINDOW` mode flag permitting a NULL/zero-len range): opens the per-thread state with no registered region, bypasses `find_region`, never calls `register_region`, selects Core §2's decode + the lifted record policy | Z0 (surface) · Z1 (policy) · Z2 (PT decode select) | Every shipped begin keys on `find_region` [:486](../../../src/hwtrace.c#L486) and no-ops on a miss; `asmtest_ss_begin_ex` EINVALs on `base==NULL\|\|len==0` [ss_backend.c:162](../../../src/ss_backend.c#L162). A legitimately region-less arm does not exist. |
| 2 | Parameterless `AsmTrace()` ctor + the nine mirror binding shims calling the region-free arm (no `NativeCode`, lazy arm, auto-name, render-on-close, closing-tid assert) | Z0 | The shipped ctor **requires** a `NativeCode` [HwTrace.cs:997](../../../bindings/dotnet/hwtrace/HwTrace.cs#L997), obtainable only via `NativeCode.FromBytes` [:421](../../../bindings/dotnet/hwtrace/HwTrace.cs#L421). No empty-ctor form over an arbitrary managed method exists in any binding. |
| 3 | The arm-time **composition seam**: §D0.1 listener ⊕ Core §2 recorder image callback ⊕ `/proc/self/maps` DSO enumeration into one lazy arm (feeding `asmtest_codeimage_track`), plus the *managed*-tier `Dispose` swap onto `render_versioned` | Z3 | Each piece exists in isolation; the seam composing them for the region-free managed case is unbuilt, and `AsmTrace.Dispose` [:1016](../../../bindings/dotnet/hwtrace/HwTrace.cs#L1016) still calls the version-blind render [:1106](../../../src/hwtrace.c#L1106). |
| 4 | Single-step region-free **L3 DESCEND_ALL** degradation (descend every call; denylist + instruction-budget + `ITIMER_REAL`/`SIGALRM` watchdog; native-leaf/ptrace-only; managed routes to §D3) | Z1 | Shipped single-step only steps **known native leaves with a region** — `ss_on_sigtrap` records in-range offsets and silently drops the rest [ss_backend.c:143-148](../../../src/ss_backend.c#L143). A range-less "trace whatever ran" contract has no upstream. |
| 5 | The empty-ctor **honest-degradation UX** (skip_reason + §3.1 buckets + §3.2 banner through the scope's Path/sink; never emit partial-as-complete) **and** the zero-config async **DEFAULT** = honest thread-scope-with-mismatch-flag (stitching stays explicit opt-in) | Z4 (async default) · Z5 (presentation) | A ctor cannot detect whether its result was assigned, a whole-window trace is noisy/partial by nature, and the region-free arm carries no region record — so the "assume no knowledge, present honestly" contract is net-new integration. |
| 6 | The composed, actionable **privilege-detection developer message** assembled from the raw self-probe | Z5 | `available()`/`skip_reason()` return a reason **code** (consumed); the human-readable, provision-me message the empty-ctor promise rests on is unbuilt. |

> Read the net-new column as three kinds of work only — **surface** (rows 1, 2, 4),
> **integration** (row 3), **presentation** (rows 5, 6). No row introduces a new way to
> capture instructions.

---

## §Z0 — Parameterless ctor + region-free arm surface

> **Status: LANDED (C core + .NET reference shim).** The region-free arm ABI ships as
> `asmtest_hwtrace_begin_window` / `_end_window` / `_render_window`
> ([src/hwtrace.c](../../../src/hwtrace.c), [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h)),
> backed by a whole-window frame mode in `asmtest_ss_begin_window`
> ([src/ss_backend.c](../../../src/ss_backend.c)); the .NET reference shim gains the
> parameterless `new AsmTrace()` ctor + `SkipReason` (§Z5)
> ([bindings/dotnet/hwtrace/HwTrace.cs](../../../bindings/dotnet/hwtrace/HwTrace.cs)).
> Host-tested green on this AMD host: `test_wholewindow_singlestep` (`make docker-hwtrace`
> → 201/0) and the .NET `AsmTrace()` case (`make docker-hwtrace-dotnet` → 33/0). The §Z5
> self-skip scaffold landed with it (`SkipReason` is set + `Armed` false where no faithful
> backend exists). **Partially landed (2026-07-15):** the **cpp** and **python** empty-window shims
> ship (host-verified; `check-bindings-parity` green with the 5 unshimmed bindings still exempt).
> **Forward-look:** the remaining shims — **rust, go, zig, lua, ruby** (mechanical mirrors of
> the .NET reference) — and the STRONG whole-window capture behind the seam ([§Z1](#z1--region-free-whole-window-capture-mode-planned-forward-look)).

**Goal.** Deliver the single net-new *surface* that makes `using (new AsmTrace())`
compile and run — a legitimately **region-free arm** — without yet committing to a
capture mode. Everything below is ABI + shim; mode selection, record policy, and the
backend ladder are [§Z1](#z1--region-free-whole-window-capture-mode-planned-forward-look)'s.

**Today — a `[base,len)` is mandatory at every layer.** `asmtest_ss_begin_ex`
`EINVAL`s on a null range ([:162](../../../src/ss_backend.c#L162)); `ss_on_sigtrap` stores
`rip-base_ip` **only** in-range ([:143-148](../../../src/ss_backend.c#L143)); every
begin/end/render keys through `find_region` ([hwtrace.c:486](../../../src/hwtrace.c#L486)) so
`try_begin` ([:739](../../../src/hwtrace.c#L739)) forces a shim to `register_region`
([:432](../../../src/hwtrace.c#L432)) first; and the .NET ctor
([HwTrace.cs:997](../../../bindings/dotnet/hwtrace/HwTrace.cs#L997)) can only obtain a range
via `NativeCode.FromBytes` ([:421](../../../bindings/dotnet/hwtrace/HwTrace.cs#L421)), which
mmaps **pre-supplied** bytes. There is no path from "an arbitrary managed method ran here"
to a `[base,len)`.

### netNew #1 (SURFACE span) — the region-free C arm entry point

Add a **whole-window** arm that legitimately permits a `NULL` / zero-len range — either
**`asmtest_hwtrace_begin_window(const char *name)`** *(new — no line yet)*, a sibling of
`asmtest_hwtrace_try_begin`, or an **`ASMTEST_HWTRACE_WHOLE_WINDOW`** mode flag on
`asmtest_hwtrace_begin_scope` ([:1121](../../../src/hwtrace.c#L1121)). Contract:

1. **Bypasses `find_region`** ([:486](../../../src/hwtrace.c#L486)) and **never calls
   `register_region`** ([:432](../../../src/hwtrace.c#L432)) — there is no `[base,len)` to
   register. It still takes the auto-**name**, so the name→handle binding survives.
2. **Opens the per-thread state with no registered region** — pushes the Core §1
   per-thread block / (under a stepper) a range-less TLS frame, returning a `(idx,gen)`
   handle keyed to that block. A new range-permissive `ss` entry point *(new — no line
   yet)* **lifts** the `base==NULL||len==0` `EINVAL` of `asmtest_ss_begin_ex`
   ([:162](../../../src/ss_backend.c#L162)); the process-global PT/AMD/CS path opens its
   per-thread perf event with no address filter.
3. **Binds `arm_tid` + the trace pointer onto the per-thread handle/block.** The shipped
   cross-thread backstop in `asmtest_hwtrace_end` is **region-keyed** —
   `find_region(name); if (r != NULL && r->arm_tid != …) r->trace->truncated = true`
   ([:898-907](../../../src/hwtrace.c#L898)) — so on a region-free arm `find_region` returns
   NULL and it **cannot fire**. The region-free arm must therefore carry `arm_tid` and the
   trace pointer on its own handle so [§Z4](#z4--async-hop-honesty-default-opt-in-stitching-default-landed-stitching-forward-look)
   can re-implement the closing-vs-arming-tid check for this path (new integration, not
   the shipped region-keyed mechanism).
4. **Defers record-policy and backend selection to [§Z1](#z1--region-free-whole-window-capture-mode-planned-forward-look).**
   Until Z1 fills the mode, the seam routes to the Z5 self-skip/degradation stub — the
   whole surface contract is exercised with **zero hardware** and no tier ever compiles
   without an honest no-op path.

**Correctness constraints carried onto the new per-thread state:**

- **initial-exec TLS, malloc-free handler.** Any new region-free frame state must use
  `tls_model("initial-exec")` and stay malloc-free in the `SIGTRAP` handler, exactly as the
  shipped `tls_frames`/`tls_depth`/`tls_gen_ctr`
  ([ss_backend.c:25-27](../../../src/ss_backend.c#L25), [:73](../../../src/ss_backend.c#L73)) — a
  general-dynamic first-touch of a `__thread` var in a `dlopen`'d `.so` can route through
  `__tls_get_addr` and lazily `malloc`, which is not async-signal-safe. The empty ctor is the
  worst case (the `.so` is `dlopen`'d into a managed runtime via P/Invoke), and initial-exec
  draws on the limited **static-TLS surplus (~1–2 KiB; exhaustion fails a later `dlopen`)** —
  keep the new frame small and fixed-depth.
- **SIGTRAP arm-refcount discipline.** The arm/disarm must share the existing `g_arm_refcount`
  + `g_old_sa` discipline ([ss_backend.c:196-210](../../../src/ss_backend.c#L196)): install the
  process-wide disposition on the 0→1 transition, save the caller's original **only then**,
  restore on the last disarm — else a region-free scope nested with a region-scoped one
  double-installs or prematurely restores `SIGTRAP`.
- **Handle/block lifecycle.** With no `register_region` to lean on, the region-free path must
  specify its own recycling: bound frame-stack depth like `SS_MAX_FRAMES` (`EFULL` past the
  cap) and free the per-begin ring on `end`, so many construct/dispose cycles neither exhaust
  the frame stack nor leak the buffer.

### netNew #2 — the parameterless `AsmTrace()` ctor + nine mirror shims

Reduce the reference shim to the aspirational form and mirror it across the nine bindings.
Each empty scope **calls the region-free arm** (not register-then-begin); **arms lazily**
(`[ModuleInitializer]` / first-scope hook — **import claims no slot, installs no SIGTRAP**);
**keys on an auto-name** (`[CallerMemberName]`/`[CallerLineNumber]`, reusing `ScopeName`);
**renders on close** through Core §0.3 (until Z1, the Z5 self-skip notice, not a fabricated
trace); and **asserts closing tid == arming tid against the handle-carried `arm_tid`**
(netNew #1 point 3) — the region-keyed §0.2 backstop does **not** cover the region-free path,
so this check is re-implemented on the handle in Z0/Z4, not inherited for free.

**.NET reference delta** — the ctor at
[HwTrace.cs:997](../../../bindings/dotnet/hwtrace/HwTrace.cs#L997) drops its `NativeCode`
parameter, the `register_region` pair, and the `NativeCode.FromBytes` dependency,
becoming:

```csharp
public AsmTrace(bool emit = true,
                [CallerMemberName] string member = null,
                [CallerLineNumber] int line = 0)   // empty ctor: new AsmTrace()
```

`Dispose` ([:1016](../../../bindings/dotnet/hwtrace/HwTrace.cs#L1016)) keeps its
size-then-alloc render-on-close and `truncated` read. All ten shims drive the
error-returning region-free arm, **not** the legacy silent-no-op `asmtest_hwtrace_begin`
([:839](../../../src/hwtrace.c#L839)).

**The empty scope, per binding** — the region-free form drops only the argument on the shipped
construct: .NET `using (new AsmTrace())`, C++ RAII `asmtest::ScopedTrace t;`, Rust
`let _t = HwTrace::scope();`, Zig
`var s = Scope.begin(); defer s.deinit();`, Go `defer Scope().Close()` (+ `LockOSThread`),
Python `with HwTrace.scope():`, Ruby `HwTrace.scope { … }`, Lua `hwtrace.scope(fn)`, Node
`using _ = asmtrace()`, Java `try (var t = new AsmTrace()) { … }`. Auto-name, lazy arm,
render-on-close, and the closing-tid check are unchanged. The **§Z5 self-skip scaffold ships
with Z0** — a `skip_reason()` passthrough so the arm's seam has a clean no-op stub before any
capture tier exists.

**Depends on:** Core §0.1–§0.4 + §1
([scoped-tracing-core-plan.md](../archive/plans/scoped-tracing-core-plan.md#0--the-two-cheap-c-layer-fixes--shared-render-on-close-done));
the [bindings shim shape](../archive/plans/scoped-tracing-bindings-plan.md#the-shim-shape-one-pattern-ten-realisations);
[multi-language-bindings-plan.md](../archive/plans/multi-language-bindings-plan.md); the substrate
`available()`/`skip_reason()` self-probe.

**Gate.** The region-free arm ABI, lazy-arm-claims-no-slot, auto-name,
render-on-close-of-a-range-less-scope, and self-skip-clean contract all run in ordinary
CI — a new `static void test_zeroctor_scope(void)` in
[examples/test_hwtrace.c](../../../examples/test_hwtrace.c) (TAP via `CHECK`), on the
universal single-step backend with no PMU and no privilege, plus each per-binding lane in
`docker-hwtrace` / `docker-hwtrace-bindings`. The test also asserts the **handler path
takes no malloc/lock**, that nested region-free + region-scoped scopes install `SIGTRAP`
**exactly once and restore exactly once**, and that repeated construct/dispose neither
exhausts the frame stack nor leaks the stream buffer.

---

## §Z1 — Region-free whole-window capture mode *(planned, forward-look)*

> **Status: WEAK single-step tier LANDED; STRONG/CEILING forward-look.** Fills the
> record-policy/backend seam behind
> [§Z0](#z0--parameterless-ctor--region-free-arm-surface)'s arm. The **WEAK** single-step
> tier is implemented + host-tested green (`test_wholewindow_singlestep`, `docker-hwtrace`
> → 201/0): `asmtest_ss_begin_window` records ABSOLUTE RIPs into the shipped bounded ring
> (overflow → `truncated`), and `asmtest_hwtrace_render_window` disassembles them from live
> self memory — the **first** place `using (new AsmTrace())` renders a real, honestly-noisy
> trace end-to-end on a no-hardware host (validated for a native leaf on this AMD box). The
> **STRONG** PT tier stands up here but is **not believed until
> [§Z2](#z2--live-whole-window-decode-validation-synthetic-fixture-front-loaded-live-pt-forward-look)'s
> synthetic fixture is green**; the **CEILING** AMD LBR tier rides
> [amd-tracing-plan.md](../plans/amd-tracing-plan.md)'s shipped backend — a bounded *complement*,
> never a complete whole window: AMD's **complete-flow** whole-window tier is the §D3/W2
> ptrace stepper (block-step-cheapened when amd Part III Phase 2 lands — see *the AMD
> fork* below). Ships self-skipping; **Linux-only**.

**Goal.** Turn the range-less handle Z0 hands back into an actual capture, owning the lifted
**record policy** (netNew #1, POLICY span), its **versioned render** wiring, and the
single-step degradation contract (netNew #4). The crux splits the backends: **PT/LBR capture
everything cheaply and filter at *decode* time** — `asmtest_pt_decode_window`
([:198](../../../src/pt_backend.c#L198)) takes offsets from the first decoded IP with **no**
in-region filter — so a region-free window is native to them. **Single-step cannot**: its
recorder is driven per retired instruction and `ss_on_sigtrap` stores `rip − base_ip` **iff**
in-range ([:143-148](../../../src/ss_backend.c#L143)), so with no range it must replace the
filter with an **absolute-RIP record + later attribution** and, lacking a range to step
*over*, descend into every call — a best-effort, self-truncating **L3 `DESCEND_ALL`** walk.
Auto-selects one of three honesty tiers.

### The three honesty tiers (auto-selected behind Z0's arm)

| Tier | Backend | Region-free capture is… | Host it needs | CI protection |
|---|---|---|---|---|
| **WEAK** | single-step L3 `DESCEND_ALL` | a **degradation** (self-truncating by design) | any **x86-64 Linux**, native leaf only; managed → §D3 ptrace | `docker-hwtrace` (unprivileged) — **first end-to-end** |
| **STRONG** | PT recorder-backed whole-window | **native** (filter-at-decode) | **Intel bare-metal PT** + `paranoid < 0` / `CAP_PERFMON` (128 KiB ring if unprivileged) | self-hosted PT runner (trust-gated on §Z2); self-skips on AMD/ARM/VM/Docker |
| **CEILING** | AMD LBR | native but **~16-branch-bounded** — a quiet *complement*; a whole window always overruns it (`truncated`) | **Zen 3+** bare metal + `CAP_PERFMON` | `docker-hwtrace-amd` capped lane; Zen 2 → `EOPNOTSUPP` → ptrace |

The ladder prefers **STRONG** when `asmtest_pt_decoder_present`
([:65](../../../src/pt_backend.c#L65)) is true, an `intel_pt` PMU is present, *and* §Z2's fixture
is green; then **CEILING** on Zen 3+; else **WEAK**. On every current CI/AMD host the real PT
bodies are absent (`ASMTEST_HAVE_LIBIPT` guard [:62](../../../src/pt_backend.c#L62)), so it lands
on WEAK or §D3 — which is why the weak tier is the earliest developer-visible value.

**The AMD fork, stated plainly.** AMD ships no PT equivalent on any Zen — a verified dead
end, not a gap ([amd-tracing-plan.md](../plans/amd-tracing-plan.md), *the governing constraint*) — so
on AMD the ladder can never produce a *quiet complete* whole window. The honest choice is a
fork: **bounded-but-quiet** (CEILING LBR — an honest ~16-branch tail, always `truncated` for
a real window) versus **complete-but-intrusive** (the §D3/W2 ptrace stepper running L3 — the
same tier WEAK's managed routing already uses). A caller needing completeness re-resolves
under the shipped `ASMTEST_HWTRACE_CEILING_FREE` policy
([asmtest_hwtrace.h:108](../../../include/asmtest_hwtrace.h#L108)), which drops the
ceiling-bounded backend and lands on the stepper. Two planned
[amd-tracing-plan.md](../plans/amd-tracing-plan.md) Part III phases upgrade the two prongs and are
**consumed here as they land** (upgrades, not Z0–Z5 prerequisites): **Phase 2 BTF
block-step** (`PTRACE_SINGLEBLOCK` → one `#DB` per **taken branch** instead of per
instruction — ~4–10× fewer stops, byte-identical `insns[]`/`blocks[]`, rootless, every Zen
including Zen 2) makes the complete prong materially cheaper; **Phase 3's eBPF
`bpf_get_branch_snapshot`** (Zen 4/5, Linux ≥ 6.10, `CAP_PERFMON`+`CAP_BPF`) reads the LBR
window **deterministically at the scope boundary**, fixing the two documented live Zen-5
failure modes (a tiny window "too fast to sample"; post-scope-glue contamination).

#### §Z1.1 — WEAK: region-free single-step L3 `DESCEND_ALL` (netNew #4)

The cheapest **complete** end-to-end empty-ctor demo. Behind Z0's region-free single-step
handle:

- **Record-policy lift — a pre-allocated BOUNDED ring (new, `src/ss_backend.c`).** Replace
  `ss_on_sigtrap`'s in-region store with a whole-window policy recording **absolute RIPs**
  (attributed at render, not `rip − base_ip`) into the *same* malloc-free-in-signal-context
  design the handler already ships: a **fixed `SS_STREAM_CAP` = 65536-offset / 512 KiB
  buffer** ([:68](../../../src/ss_backend.c#L68)) `malloc`'d once in `begin_ex`, with an
  **overflow flag** set when full ([:143-148](../../../src/ss_backend.c#L143)). It must **not**
  grow in the handler. A `DESCEND_ALL` window emits far more RIPs than any leaf, so it
  **overflows near-instantly** and sets `trace.truncated` — this **buffer-overflow
  truncation** is distinct from the L3 budget/watchdog truncation below; both feed §Z5's
  never-emit-partial banner. Keep the shipped in-region path **byte-identical** for the
  region-scoped case (regression assert).
- **Render — versioned, against a self codeimage (new to Z1).** A region-free frame registers
  no region, so the name-keyed `asmtest_hwtrace_render`
  ([:1106](../../../src/hwtrace.c#L1106)) `find_region`-misses → `EINVAL`, and the handle-keyed
  `asmtest_hwtrace_render_scope` ([:1157](../../../src/hwtrace.c#L1157)) disassembles a single
  contiguous `[base,base+len)` window — useless for a `base=NULL`/`len=0` frame. An
  absolute-RIP ring can **only** be rendered by `asmtest_hwtrace_render_versioned`
  ([:1172](../../../src/hwtrace.c#L1172)) against a codeimage. So Z1 WEAK captures a self
  `pid==0` codeimage snapshot (`asmtest_codeimage_new(0)`,
  [codeimage.c:314](../../../src/codeimage.c#L314)) over the native leaf's live-mapped bytes and
  renders the ring through it. **This render swap + self-codeimage byte source are Z1
  prerequisites of the "first end-to-end value" claim, not deferred to §Z3** — §Z3 then owns
  only the *managed* method-discovery + tiered-version case, not the render mechanism.
- **`DESCEND_ALL` semantics.** With no range to step *over*, "capture whatever runs here"
  **is** descent level **L3** — riding [call-descent-plan.md](../archive/plans/call-descent-plan.md)'s §L3
  denylist + instruction-budget + `ITIMER_REAL`/`SIGALRM` watchdog, expected to self-truncate.
  Tests assert the guards fire, never that L3 is transparent.
- **Native-leaf / ptrace-only routing + gate.** Single-step is **forbidden against live
  managed code** (it swaps the process-wide `SIGTRAP` disposition CoreCLR's PAL owns), so the
  WEAK tier runs in-process **only on a native leaf** (any x86-64 Linux, unprivileged,
  `docker-hwtrace`); region-free **managed** degradation routes to the
  [**§D3** concealed ptrace-stealth helper running L3](scoped-tracing-managed-plan.md#d3--the-concealed-out-of-process-ptrace-stepper-scope-stepper--standalone-binary-bundling--package-embedding-done-live-jit-address-channel-forward-look)
  (`SYS_PTRACE` / Yama `PR_SET_PTRACER`; default Docker seccomp on host kernel ≥ 4.8, else
  `CAP_SYS_PTRACE`), which also backstops the cross-thread drop `asmtest_ss_end`
  ([:279](../../../src/ss_backend.c#L279)) still exhibits. The stepper steps per-instruction
  today; when [amd-tracing-plan.md](../plans/amd-tracing-plan.md) Part III Phase 2 lands
  (`asmtest_ptrace_trace_attached_blockstep`), this routing consumes **block-step** — one
  `#DB` per taken branch, ~4–10× fewer stops, byte-identical output — with no change to
  the seam.

  > **Decision (2026-07-06): the shipped .NET WEAK tier accepts in-process single-step of
  > managed windows.** The §D0.1/§D0.2 `byMethod`/`withRundown` empty-ctor scopes (the
  > .NET binding + the `examples/dotnet/*` lanes) single-step live managed code
  > in-process today, mitigated by convention rather than routing: the window is
  > single-threaded and kept tight, the rundown's socket I/O runs **before** arming
  > (never under TF), and overflow / cross-thread closes surface as `Truncated`, never
  > as a silent partial. The routing above — PT/LBR where the silicon exists, else the
  > §D3 stepper — remains the roadmap for the *safe* managed arm
  > (`test_wholewindow_ss_managed_routes` gates that seam when it lands), but "refuse
  > in-process single-step of managed code" is no longer a shipping precondition.

#### §Z1.2 — STRONG: PT recorder-backed whole-window arm

The out-of-band tier that makes the empty ctor honest *and* quiet. A per-thread `perf` AUX
event opened with **no** region, plus a self (`pid == 0`) code-image timeline
(`asmtest_codeimage_new(0)`, [codeimage.c:314](../../../src/codeimage.c#L314)) so `read_recorder`
→ `asmtest_pt_read_codeimage` ([pt_backend.c:47](../../../src/pt_backend.c#L47)) serves
version-live bytes at any IP. The record policy **mode-selects** Core §2's `read_recorder`
([:96](../../../src/pt_backend.c#L96), wired [:216](../../../src/pt_backend.c#L216)) over the
region-scoped `read_region` ([:74](../../../src/pt_backend.c#L74)), which returns `-pte_nomap`
outside `[base,len)` and **dies at the first out-of-region IP**
([:179-188](../../../src/pt_backend.c#L179)). This tier is **not trusted until
[§Z2](#z2--live-whole-window-decode-validation-synthetic-fixture-front-loaded-live-pt-forward-look)'s
synthetic fixture is green** — nothing in §Z1 forward-looks on an unproven decode.
**Gate:** Intel bare-metal PT + `perf_event_paranoid < 0` or `CAP_PERFMON` (128 KiB ring if
unprivileged); self-skips on AMD/ARM/VMs/Docker. Linux-only.

#### §Z1.3 — CEILING: AMD LBR

Auto-selected when LBR is present and PT is not. LBR is a bounded ~16-branch window; a
whole scope overruns it and the trace is flagged `truncated`.
[amd-tracing-plan.md](../plans/amd-tracing-plan.md)'s **Tier-B stitching** extends the *window*,
**not** the PMI-per-branch economics — so a Zen host honestly renders the tail and flags
the rest. This tier is therefore a fast, quiet **complement**, never AMD's whole-window
capture: the *complete* region-free trace on AMD is the §D3/W2 ptrace stepper (see *the
AMD fork* above), reached via `ASMTEST_HWTRACE_CEILING_FREE`
([asmtest_hwtrace.h:108](../../../include/asmtest_hwtrace.h#L108)) or when LBR is
absent/denied. Two [amd-tracing-plan.md](../plans/amd-tracing-plan.md) Part III upgrades are
consumed here as they land: **Phase 3's eBPF `bpf_get_branch_snapshot`** reads the
16-entry window **deterministically at the scope boundary** (no `sample_period=1` flood,
no richest-window heuristic — the two documented live Zen-5 failure modes; Zen 4/5,
Linux ≥ 6.10, `CAP_PERFMON`+`CAP_BPF`); **Phase 2 block-step** cheapens the
`CEILING_FREE` fallback this tier truncates into. **Gate.** Zen 3+ bare metal +
`CAP_PERFMON`, `docker-hwtrace-amd` capped lane; Zen 2 → `EOPNOTSUPP` → falls through to
§D3. Linux-only.

**§Z1 tests** (in [examples/test_hwtrace.c](../../../examples/test_hwtrace.c)):

- `test_wholewindow_ss_descend` (host-testable, `docker-hwtrace`) — arm the region-free
  single-step tier over a native leaf that calls out; assert the **bounded absolute-RIP
  ring** captured out-of-(former-)range IPs, that it renders through `render_versioned`
  against the self codeimage, that `DESCEND_ALL` descended, and that **both** truncation
  paths fire on a runaway (buffer-overflow flag; §L3 budget/watchdog) — never that the walk
  is transparent.
- `test_wholewindow_ss_managed_routes` — assert a region-free **managed** arm refuses
  in-process single-step and routes to §D3 (CI-runnable, `SYS_PTRACE`; alarm/ITIMER-guarded
  like `test_ptrace_scoped_stealth`).
- The STRONG PT arm's decode is exercised by §Z2's `test_wholewindow_decode`; the **live**
  whole-window PT/LBR smokes self-skip off their silicon
  (`asmtest_hwtrace_available()`/`asmtest_pt_decoder_present()` + why-string), wrapped in
  `#if defined(__linux__) && defined(__x86_64__)`, printing a specific `# SKIP` on this AMD
  host.

**§Z1 effort.** Record-policy lift + versioned-render wiring + `DESCEND_ALL` + the two
host-testable/CI-runnable tests ~5–7 days (WEAK is the value-first deliverable). STRONG PT
wiring ~2–3 days on top of Core §2 but **not trusted** until §Z2; CEILING is thin over the
shipped AMD backend.

---

## §Z2 — Live whole-window decode validation *(synthetic-fixture decode LANDED; live PT forward-look)*

> **Status: synthetic-fixture decode LANDED (`test_wholewindow_decode`); live PT forward-look.** The byte adapter this phase rides
> ships: `asmtest_pt_read_codeimage` ([src/pt_backend.c:47](../../../src/pt_backend.c#L47)) is
> libipt-**independent**, host-tested (`test_pt_image_from_codeimage`) by
> [Core §2](../archive/plans/scoped-tracing-core-plan.md#2--libipt-decode-against-self-code-image-glue-host-testable-half-done-live-pt-forward-look).
> The whole-window decoder `asmtest_pt_decode_window` ([:198](../../../src/pt_backend.c#L198))
> is now **exercised end-to-end on a synthetic packet stream**: `asmtest_pt_encode_fixture`
> ([:294](../../../src/pt_backend.c#L294)) hand-assembles a valid Intel-PT byte array that
> `test_wholewindow_decode` decodes with **no PT hardware** (where libipt is built; AMD/CI
> hosts without it compile the `#else` `ENOSYS` stub ([:266](../../../src/pt_backend.c#L266))
> and self-skip, as does the real body for want of an `intel_pt` PMU on the **live** half).
> **This phase is the trust-gate on §Z1's STRONG tier.** Requires
> **libipt at build (no PT hardware)** for the synthetic half; **bare-metal Intel PT** for the
> live half.

**Goal.** Prove `asmtest_pt_decode_window` is **honest on a known packet stream** — the
region-free recorder-backed decode reconstructs the exact executed path with no in-region
filter — *before* any downstream tier (§Z1 PT, §Z3, §Z4) is wired on top. The decoder's
doc-comment ([:191-197](../../../src/pt_backend.c#L191)) states the contract: *bytes for ANY
address, temporal-correct as of `when`, every instruction at an offset from the first IP.*

**Why it is new — the decode path exists, the exercise does not.**

| callback | serves bytes for | miss behaviour | wired at | role |
|---|---|---|---|---|
| `read_region` ([:74](../../../src/pt_backend.c#L74)) | only `[base_ip, base_ip+len)` | `-pte_nomap` **at the first out-of-region IP** — decoder dies ([:179-188](../../../src/pt_backend.c#L179)) | [:143](../../../src/pt_backend.c#L143) | shipped region-scoped decode |
| `read_recorder` ([:96](../../../src/pt_backend.c#L96)) | **any** executed IP, version-live at `when` | `-pte_nomap` only on a real recorder miss | [:216](../../../src/pt_backend.c#L216) | the region-free **whole-window** decode this phase validates |

The decoder body is written; the missing **input** is a raw Intel-PT packet byte array that
reaches it without silicon. This is the exact sub-task Core §2 flagged (*"no synthetic-PT
fixture or PT encoder in the tree"*), pulled forward as the plan's cheapest de-risk.

**Changes.**

- **Synthetic PT-packet fixture (new, `examples/`).** A checked-in (or encoder-produced) PT
  AUX blob — PSB + a short TIP/TNT walk over two or three basic blocks — paired with an
  `asmtest_codeimage` holding the bytes at the walk's IPs, driving `asmtest_pt_decode_window`
  end-to-end through libipt. Producing the blob needs a hand-assembled constant or a small new
  `asmtest_pt_encode_*` helper (libipt ships an encoder — userspace, hardware-free). It rides
  the already-passing `asmtest_pt_read_codeimage`; the only *new* coverage is the decode loop
  between AUX bytes and `asmtest_trace_t` offsets.
- **Build-gate honesty.** The real body compiles **only** under `ASMTEST_HAVE_LIBIPT` (guard
  [:62](../../../src/pt_backend.c#L62); `#else` stub [:252](../../../src/pt_backend.c#L252)), so the
  fixture needs **libipt at build** (no PT hardware) and **self-skips where libipt is absent**
  (probes `asmtest_pt_decoder_present()` [:65](../../../src/pt_backend.c#L65), prints `# SKIP …
  built without libipt`). The trust-gate lands on the x86_64 `hwtrace-test` lane; plain docker
  and libipt-less hosts self-skip and provide no de-risk.
- **Wire/validate Core §2's capture-side address filter (consumed, not owned here).**
  `PERF_EVENT_IOC_SET_FILTER` emits packets only for the traced window
  ([:184](../../../src/pt_backend.c#L184)), but Core §2's constraint holds: perf userspace filters
  resolve only **file-backed** VMAs, so the **anonymous** JIT / `exec_alloc` pages fall back to
  the shipped **decode-time** range filter (cutting the decoded stream, not capture bandwidth).
  The filter primitive is **Core §2's**; §Z2 only exercises it alongside the decode validation.

**§Z2 tests + posture** (split per the no-untested-hardware rule):

- `test_wholewindow_decode` (**host-testable where libipt is built, front-loaded**) — feed
  the synthetic PT AUX blob + paired codeimage to `asmtest_pt_decode_window`
  ([:198](../../../src/pt_backend.c#L198)) directly; assert the decoded insn offsets reproduce
  the known walk, that **no** in-region filter truncated it, and that `asmtest_disas` of the
  recorder-served bytes matches ground truth. Probes `asmtest_pt_decoder_present()` first and
  **self-skips on the `ENOSYS` stub** — so the trust-gate lands on the x86_64 `hwtrace-test`
  lane and libipt-less hosts provide no de-risk. The cheapest de-risk of the strong path.
- A **live** self-JIT PT smoke that arms the region-free capture and decodes its real AUX
  stream through the same body — **bare-metal Intel PT only**, self-hosted, trusted-branch-gated,
  allowed-to-be-absent; self-skips on AMD/ARM/VMs/Docker.

**§Z2 effort.** Fixture + test ~2–4 days (the cost is hand-assembling/encoding a minimal valid
PT stream); the live smoke ~3–5 days **on PT hardware**, forward-look until a bare-metal Intel
PT runner exists — the gate [hardware-trace-plan.md](../plans/hardware-trace-plan.md) already accepts.

---

## §Z3 — Arbitrary managed-method capture in the empty scope *(forward-look)*

> **Status: planned — netNew #3, a pure composition seam (no new capture primitive).** Every
> *piece* exists in isolation: the
> [Managed §D0.1](scoped-tracing-managed-plan.md#d0--net-managed-code-capability-closing-the-leaks-on-net-8-forward-look-needs-a-live-net-runtime--intel-pt-for-the-clean-path)
> `NativeRuntimeEventSource` listener, the self `pid==0` recorder + `asmtest_codeimage_track`
> ([codeimage.c:314](../../../src/codeimage.c#L314) / [:373](../../../src/codeimage.c#L373)), Core
> §2's `read_recorder` callback ([pt_backend.c:96](../../../src/pt_backend.c#L96)), and
> `asmtest_hwtrace_render_versioned` ([hwtrace.c:1172](../../../src/hwtrace.c#L1172), WEAK-tier
> wiring already introduced by §Z1). **Net-new is the arm-time wiring** composing them into
> **one** lazy arm for the *managed* case, so an *unwarmed* `HotPath(data)` — whose JIT and GC
> also run in the window — decodes against the **full executed image set** with zero developer
> knowledge.

**Depends on:** §Z0 · **either** §Z2 (validated PT decode) **or** §Z1's §D3 ptrace-L3 ·
Managed §D0.1 + §D0.2 + §D3 · Core §1 `render_versioned` (native-leaf-wired in §Z1) + Core §2
recorder callback · the `asmtest_codeimage` self-path + `track()` gated on
**`asmtest_codeimage_available()`** (soft-dirty / `PAGEMAP_SCAN`,
[codeimage.c:289](../../../src/codeimage.c#L289)).

**The arm-time composition (netNew #3).** The shipped ctor is *given* a `[base,len)`; the
empty ctor has **none**, so at close the decoded IPs are arbitrary — the JIT'd `HotPath`, the
RyuJIT that compiled it mid-window, GC helpers, CoreCLR `.so` text. The seam's job is to make
all of them resolvable to bytes, converging three byte sources on Core §2's `read_recorder`
callback inside Z0's lazy arm:

| Byte source | Supplies | State |
|---|---|---|
| §D0.1 listener → `name→(addr,size,version)` map | each method JIT'd **during** the window, fed into `track()` on a copied `(addr,size)` — closes the "records a known range, does not discover a method" gap | planned in Managed §D0.1 |
| self `pid==0` recorder + `asmtest_codeimage_track` | version-stamped bytes of every tracked JIT extent, read from own `/proc` via `process_vm_readv` ([:194](../../../src/codeimage.c#L194)) | self-path works ([:314](../../../src/codeimage.c#L314) / [:373](../../../src/codeimage.c#L373)); the gap was discovery, not read |
| `/proc/self/maps` file-backed DSO enumeration | the runtime's own text (CoreCLR, libc, GC) for IPs that leave JIT'd code | Core §2 |

**Soft-dirty availability gate.** The version-stamped capture requires
`asmtest_codeimage_available()` — a functional soft-dirty / `PAGEMAP_SCAN` probe
([codeimage.c:289](../../../src/codeimage.c#L289)); `asmtest_codeimage_track` returns
`ASMTEST_CI_EUNAVAIL` otherwise ([:373-378](../../../src/codeimage.c#L373)). On older kernels
(no `CONFIG_MEM_SOFT_DIRTY`) or hardened containers the **temporal-versioning half
self-skips with a skip reason**, and the versioned render **degrades to the
version-independent self-recorder fallback** (§D0.2). Its CI lane is
`docker-hwtrace-codeimage`.

**§D0.2 pre-arm rundown + version-independent fallback.** Methods JIT'd *before* the arm
are not seen by the in-proc listener; §D0.2's self-connected `EnablePerfMap(JitDump)` walks
the R2R + JIT heaps on enable to cover them, with the self recorder as the
version-independent fallback byte source.

**The `Dispose` render swap for the managed tier.** §Z1 already re-points *native-leaf*
render onto `render_versioned` against a self codeimage. §Z3 extends the swap to the .NET
managed path: `AsmTrace.Dispose` ([HwTrace.cs:1016](../../../bindings/dotnet/hwtrace/HwTrace.cs#L1016))
moves off the version-blind `asmtest_hwtrace_render` ([:1106](../../../src/hwtrace.c#L1106))
onto `render_versioned` ([:1172](../../../src/hwtrace.c#L1172)), fed by the §D0.1-populated,
soft-dirty-versioned code-image — so tiered/moved/freed JIT bytes render against the
version live in the window, not stale live bytes. This swap is correct **only once its
producer exists** (this seam); before then it would render `(no bytes @version)`.

**Noise is honest; warming is a knob, not a precondition.** The trace is **noisy by
construction** — the JIT that compiled the method and any GC that ran, interleaved with
`HotPath`; §Z3 *captures* that full set, [§Z5](#z5--privilege-detection-self-skip-and-honest-degradation-ux-every-host-never-hard-fails)
*labels* it. Because decode is against the **version live in the window**,
`DOTNET_TieredCompilation=0` / `[MethodImpl(NoInlining)]` only reduce noise (fewer tier-up
moves) — they are **not** a correctness precondition the way the region form's stable-blob
assumption was. Single-step stays **forbidden** against live managed code, so the managed arm
selects PT/LBR where the silicon exists, else routes to the §D3 stepper running L3.
*(Superseded in practice for the shipped WEAK tier — see the §Z1 routing decision note:
the .NET binding single-steps managed windows in-process today, convention-mitigated.
**Update 2026-07: the `render_versioned`/codeimage seam LANDED for the .NET `byMethod`
path** — the `JitMethodMap` now feeds a self code-image (`asmtest_codeimage_track` per
`MethodLoadVerbose`), and close-time disassembly decodes each captured address against
the window-live version (`asmtest_hwtrace_render_versioned` is bound; the map falls back
to live memory only for untracked native-runtime addresses). Labelling is no longer
version-blind for tracked managed bytes. The whole-window `render_window` text path
stays live-memory (native-leaf case); §D0.4 cross-thread stitching remains forward-look.)*

**§Z3 tests.** *Host-testable half:* `test_zeroctor_managed_compose` drives a **synthetic**
`MethodLoad` event stream through the `name→(addr,size)→track()` plumbing, asserts the
recorder is populated for the window, and asserts `Dispose` selects `render_versioned`
(a tier-up at a fresh address renders against the window-live version) — riding the
already-passing `test_pt_image_from_codeimage`. *Live half (gated, self-skipping):* the
[`docker-hwtrace-jit-dotnet`](../../../mk/docker.mk#L242) lane (inner recipe
[`hwtrace-jit-dotnet`](../../../mk/native-trace.mk#L327)) captures an unwarmed `HotPath`
whole-window on a live .NET 8+ runtime over **either** Intel PT (§Z2) **or** the §D3 ptrace
path, self-skipping when the runtime re-tiers/moves code, when soft-dirty is unavailable, or
when ptrace is denied. Linux-only.

---

## §Z4 — Async-hop honesty default, opt-in stitching *(default landed; stitching forward-look)*

> **Status: default + merge core landed; escalation forward-look.** The mismatch-flag
> **pattern** ships for the region-scoped path (Core §0.2): `asmtest_hwtrace_arm_tid`
> ([src/hwtrace.c:844](../../../src/hwtrace.c#L844)) + the cross-thread `truncated` flag in
> `asmtest_hwtrace_end` ([:898-907](../../../src/hwtrace.c#L898)). But that backstop is
> **region-keyed** (`find_region(name)` → NULL on a region-free arm), so the empty ctor
> must carry `arm_tid` + the trace pointer on its own per-thread handle (Z0 netNew #1
> point 3) and re-implement the closing-vs-arming check there — **new integration, not
> zero new code**. The opt-in merge **core** also ships and is host-tested
> (`test_stitch_slices`, [:1233](../../../src/hwtrace.c#L1233)). **Forward-look:** the first
> live **producer** of slices — §D0.4's `AsyncLocal` hook driving decode-at-disable
> per-thread PT events into `stitch` — which needs a live managed runtime + bare-metal
> Intel PT per-thread events (the §D3 single-thread ptrace stepper **cannot** exercise a
> hop) and therefore has **no CI coverage**.

The empty scope is a **thread window**: capture is armed on the constructing thread, and
`await` / `Task.Run` / thread-pool continuations run on *other* threads. The governing
decision — **what zero-config does about that** — is deliberately not "auto-stitch." Owns
**netNew #5 (async default span)**.

### The default: honest thread-scope-with-mismatch-flag

A synchronous open/close on one thread is byte-identical to a plain trace. A scope whose work
hopped is **flagged**, never silently dropped. The naive failure mode is `asmtest_ss_end`
([:279](../../../src/ss_backend.c#L279)), a **no-op** when the closer holds no frame. The empty
ctor instead compares the closing OS tid against the **handle-carried** `arm_tid` (Z0) and
sets `trace.truncated` on a mismatch, erring **false-truncated over false-complete**;
`AsmTrace.Dispose` reads it into `Truncated`
([HwTrace.cs:1016](../../../bindings/dotnet/hwtrace/HwTrace.cs#L1016)) and §Z5's banner explains
it. **A hopped continuation is flagged, never silently dropped, never auto-stitched** —
auto-stitching is rejected as the default because §D4's per-hop decode cost is real and the
live chain has no CI protection.

| | Default | Opt-in escalation (forward-look) |
|---|---|---|
| Model | thread window | stitched logical operation |
| Hopped continuation | flagged `truncated` | captured as a further slice |
| New work | region-free arm-tid carry (Z0/Z4) — the region-keyed §0.2 backstop can't fire | first live `slice_t` producer |
| Cost | cheap | per-hop decode-at-disable |
| CI protection | host-testable (handle-carried tid assert) | none — needs live runtime + Intel PT |
| Synchronous case | plain trace | byte-identical (degenerate single slice) |

### The opt-in escalation: wiring the first live slice producer into `stitch`

An **explicit opt-in, a redesign not a knob** — the scope becomes a **stitched trace of one
logical operation**, building the missing **upstream** for a merge core that has always
lacked one. `asmtest_hwtrace_stitch` ([:1233](../../../src/hwtrace.c#L1233)) is a pure,
seq-ordered concatenation of pre-decoded slices, but nothing emits `asmtest_hwtrace_slice_t`
([:246](../../../include/asmtest_hwtrace.h#L246)) today. The producer: §D0.4's
`AsyncLocal<ScopeId>` hook fires on a resuming thread's execution-context restore; on leaving
thread A it **decodes-at-disable** A's window against the version live in it, parking a
`(scope_id, seq, tid=A, version)` slice, and opens a **fresh** per-thread PT event on B
(`seq+1`) — decoding at disable, not close, keeps `stitch` a pure host-testable merge. At
close, slices are ordered by `seq` and concatenated with per-slice provenance in the companion
`asmtest_hwtrace_slice_bound_t` ([:254](../../../include/asmtest_hwtrace.h#L254)) array; the
synchronous single-slice case stays byte-identical. Single-step is **forbidden** here, and the
§D3 stepper follows a single thread so it **cannot** exercise a hop — which is why the
cross-thread merge is **Intel-PT-gated** and validated by a synthetic host test, not a live lane.

### §Z4 dependencies

- **Async DEFAULT:** only **Core §0.2's mismatch-flag pattern** + the **Z0 region-free
  arm-tid carry** (available from Z0's ctor onward). It works for native-leaf scopes and
  needs neither §Z3 nor any managed capture.
- **Opt-in ESCALATION:** **§Z3** (a single managed whole-window must be capturable before
  its hops can be sliced) **and §Z2** (trusted whole-window PT decode) — separately. The §D3
  satisfaction of §Z3 is **insufficient** for the escalation: §D3 cannot exercise a hop, so
  the cross-thread merge requires the **§Z2-validated per-thread PT path** even though §Z3
  may have shipped on §D3. Plus **Managed §D0.4** (hop hook) + **§D4** (stitch core + slice
  ABI) and **Core §1** (a fresh per-thread PT event per hop).

### §Z4 tests + gate

**SPLIT.** The honest default + the pure merge core are **host-testable on every host**:
`test_stitch_slices` (synthetic `slice_t` values → ordered stream + `bounds`) and the
default-flag assertion in `test_zeroctor_scope` (close from a spawned thread → `Truncated`
via the handle-carried arm-tid check). The per-binding opt-in async case (a real
`await`/continuation hop → slices stitch by `ScopeId`) and the whole **live hook→tag→merge
chain have no CI coverage** — they need a live managed runtime + bare-metal Intel PT
per-thread events (per-thread mode needs no privilege bump; the per-CPU alternative needs
`CAP_PERFMON` / `perf_event_paranoid < 1`). **Linux-only**; self-skips on
AMD/ARM/VMs/Docker/macOS.

---

## §Z5 — Privilege detection, self-skip, and honest degradation UX *(every host; never hard-fails)*

> **Status: planned; scaffolded at §Z0.** The raw self-probe already ships and is the sole
> input this phase consumes: `asmtest_hwtrace_available`
> ([src/hwtrace.c:247](../../../src/hwtrace.c#L247)) and `asmtest_hwtrace_skip_reason`
> ([:264](../../../src/hwtrace.c#L264), reason strings [:280-303](../../../src/hwtrace.c#L280))
> return per-backend codes; `asmtest_pt_decoder_present`
> ([pt_backend.c:65](../../../src/pt_backend.c#L65) real / [:254](../../../src/pt_backend.c#L254)
> stub) and `asmtest_amd_decoder_present`
> ([amd_backend.c:39](../../../src/amd_backend.c#L39) / [:245](../../../src/amd_backend.c#L245))
> gate PT/AMD; the fallback tiers carry their own reason strings
> ([asmtest_codeimage.h:76](../../../include/asmtest_codeimage.h#L76),
> [asmtest_ptrace.h:82](../../../include/asmtest_ptrace.h#L82)). The `.NET` shim already
> surfaces `Armed`, `Truncated`, and always populates `Path` on close. **Net-new here:**
> the `AsmTrace.SkipReason` scope-object surface (new field, no line yet), the **composed**
> developer message (new `asmtest_hwtrace_degradation_note`-style builder, to be added), and
> the never-emit-partial contract over the §3.1 bucket labels + §3.2 banner. On this AMD
> host the empty ctor is a **clean no-op absent privilege** — no `intel_pt` PMU
> ([:298-299](../../../src/hwtrace.c#L298)), single-step forbidden against live managed code —
> so unprivileged it records nothing and reports the §Z5.2 message; with `CAP_PERFMON` the
> CEILING LBR complement arms (bounded, `truncated`), and with ptrace permitted the §D3
> stepper renders the complete window (§Z1's *AMD fork*). Host-testable on **every** host;
> the facility it fronts is **Linux-only**.

The presentation capstone. The empty ctor promises capture from privilege/hardware the code
cannot grant itself, over a trace that is **noisy and partial by nature** — so honesty rests
here: detect what is available, degrade to a **recorded no-op that says why**, never present a
partial window as complete. Owns netNew **#5 (PRESENTATION)** and **#6** (the composed message);
it adds **no probe** — it *composes* the shipped self-probe over every tier §Z1–§Z4 stands up.

### §Z5.1 The self-probe → `SkipReason` passthrough *(scaffolded at §Z0)*

A net-new `AsmTrace.SkipReason` field (sibling of `Armed`/`Truncated`, exposed by all ten
shims) carries the raw `asmtest_hwtrace_skip_reason` code for the tier that *would* have
served this arm, empty when `Armed`. It is **composed across tiers, not single-backend**: the
whole-window form can be served by §Z1 STRONG PT, CEILING AMD LBR, the §D3 ptrace stepper, or
nothing — so `SkipReason` reflects the *auto-selection ladder's* verdict (why PT **and** the
fallback were skipped).

### §Z5.2 The composed privilege-detection message *(netNew #6)*

`skip_reason()` returns a *code*; the empty-ctor promise needs an *actionable* sentence a
developer can act on **once**. A message builder folds each tier's raw code into one composed
note surfaced through the scope's `Path`/sink and `SkipReason`:

| Host / condition | Raw `skip_reason` (shipped) | Composed empty-ctor message (net-new) |
|---|---|---|
| Intel bare metal, unprivileged | `perf_event capture not permitted` ([:302](../../../src/hwtrace.c#L302)) | whole-window PT needs `perf_event_paranoid<0` **or** `CAP_PERFMON`; falling back to ptrace stepper (timing-intrusive) — §Z5.4 |
| AMD / cloud / VM (this host) | `no intel_pt PMU…` ([:298-299](../../../src/hwtrace.c#L298)) | PT unavailable here; **Zen 3+ offers LBR** (bounded ~16-branch window, flags `truncated`); **complete** capture = the §D3 ptrace stepper (`CEILING_FREE`); else self-skip |
| AMD Zen 3+, unprivileged | `perf branch-stack not permitted…` ([:290-292](../../../src/hwtrace.c#L290)) | LBR present but not permitted; lower `perf_event_paranoid` or grant `CAP_PERFMON` |
| No PT/LBR, ptrace-capable | ptrace skip code ([asmtest_ptrace.h:82](../../../include/asmtest_ptrace.h#L82)) | degraded to the concealed §D3 ptrace stepper — a second process, ~1000× on the stepped thread (amd Part III P2 block-step cuts stops ~4–10× when it lands); L3 self-truncating |
| No PT/LBR, ptrace denied | ptrace skip code | records nothing; grant `CAP_SYS_PTRACE` or `PR_SET_PTRACER`, or run on Intel bare metal |
| Not Linux | `single-step backend is Linux x86-64 only…` ([:280-281](../../../src/hwtrace.c#L280)), codeimage `ENOSYS` ([codeimage.c:492](../../../src/codeimage.c#L492)) | the whole using-model is **Linux-only**; records nothing and says why |
| Async-hop detected | `trace.truncated` via arm-tid mismatch (handle-carried; §Z4) | captured the arming thread only; continuation hopped — stitching is opt-in (§Z4) |

Where multiple tiers self-skip, the message names the *ladder* ("PT unavailable (AMD); LBR
unavailable (Zen 2); using ptrace stepper"). **The composition is the net-new part, not the
detection.**

### §Z5.3 The never-emit-partial presentation contract *(netNew #5, PRESENTATION span)*

The render on close must label and banner, never present a prefix as the whole:

- **Bucket labels (Core §3.1).** Render through the shipped `asmtest_hwtrace_symbolize_bucket`
  ([:1384](../../../src/hwtrace.c#L1384)) + address→name `asmtest_hwtrace_region_name`
  ([:1292](../../../src/hwtrace.c#L1292)) — both host-tested by `test_symbolize_bucket` — so "31k
  in RyuJIT, 2k in GC, 7k in `HotPath`" is labelled. **Temporal** attribution (which tier ran
  *when*) uses §3.1's **eBPF PROT_EXEC emission-slicer** — gated on **CAP_BPF + kernel BTF**
  (`asmtest_codeimage_bpf_available` [codeimage.c:618](../../../src/codeimage.c#L618); reason
  "kernel BTF unavailable" [:584](../../../src/codeimage.c#L584)), correlated against per-IP PT
  positions — with the **soft-dirty version-timeline as the coarser fallback**; lane
  `docker-hwtrace-codeimage`.
- **Truncation banner (Core §3.2).** An overflowed window keeps its tail and banners
  `truncated`, never cap-as-complete — the same posture `trace.truncated`
  ([asmtest_trace.h:59](../../../include/asmtest_trace.h#L59)) already gives the shared render
  ([:1106](../../../src/hwtrace.c#L1106)).
- **Async-hop (default).** A cross-thread close rides the §Z4 handle-carried backstop —
  flagged, never dropped, never auto-stitched.

Together: **every** outcome is a labelled complete window, a labelled+bannered partial window,
or a self-skip note with `SkipReason`.

> **Shipped .NET presentation (decision, 2026-07-06).** The .NET whole-window scope
> defaults to **data-only**: `Path` stays empty and the caller reads
> `Addresses`/`Methods`/`Disassembly` (the `examples/dotnet/*` Report files own the
> presentation); the self-skip note lives in a dedicated `SkipReason` property, not in
> `Path`. The rendered form is **opt-in** via `new AsmTrace(renderPath: true)`, which
> routes through the native `render_window` (live-memory disassembly + the §3.2
> truncation banner). Both modes are supported by design; neither is a drift.

### §Z5.4 One-time provisioning + the Linux-only floor *(documented)*

The one honest lever the message points at: **whole-window PT / AMD LBR** →
`sysctl kernel.perf_event_paranoid=-1` **or** `setcap cap_perfmon+ep` (Intel bare metal / Zen 3+
respectively); **ptrace stealth fallback** → Yama `PR_SET_PTRACER` or `CAP_SYS_PTRACE` (default
Docker seccomp permits `ptrace(2)` on host kernel ≥ 4.8). Unprivileged PT still arms with a
**128 KiB ring**; the message states this rather than failing. Off Linux the lifecycle
self-skips to `(void)name` no-ops (codeimage stub `ENOSYS`,
[codeimage.c:492](../../../src/codeimage.c#L492)) — "records nothing, and says why", **never a
hard failure.**

**§Z5 tests** (all host-testable, every host, pure self-probe): `test_zeroctor_scope`
(no backend → `Armed == false`, `SkipReason` non-empty, `Path` is the self-skip note, no
exception); `test_degradation_message` (drive the composed builder from a synthetic
`(available, skip_reason)` tuple per §Z5.2 row; each string names the missing capability
**and** the one-time grant); `test_wholewindow_banner` (a synthetic
`truncated`/arm-tid-mismatch trace renders the §3.1 bucket labels + banner, never a partial
window without one). **Effort:** `SkipReason` near-free (Z0 scaffold); the message builder and
banner render ~2 days each over Core §3.1/§3.2. No forward-look, no hardware gate.

---

## Sequencing (dependency-correct order)

> **Reading order vs. build order.** The **§Z0–§Z5** numbers are the *content/reading*
> order — surface → capture mode → decode validation → managed → async → UX. The **build**
> spine below obeys the hard dependencies and, where a dependency leaves slack,
> **front-loads the cheapest host-testable de-risk** while handing the developer visible
> value as early as possible.

The build spine (per-phase detail in the §Z sections above; here only the ordering and the
load-bearing dependency edges):

- **§Z0 lands first, stands alone.** The region-free arm + the empty ctor across all ten
  bindings; carries `arm_tid` on the handle (the region-keyed §0.2 backstop cannot fire
  region-free); defers record-policy to Z1. Provable on any host; the **§Z5 self-skip
  scaffold ships with it**.
- **§Z2·synthetic front-loads in *parallel* with Z0** — depends **only on Core §2**, so it
  is the cheapest de-risk of the least-certain fact (the `#ifdef`-stubbed decode) and the
  **trust-gate on Z1's STRONG tier**. Host-testable **wherever libipt is built**.
- **§Z1 precedes Z2 in numbering but delivers value first.** Fills Z0's arm with the
  bounded-ring record policy + its self-codeimage `render_versioned` wiring. WEAK single-step
  L3 is where `using (new AsmTrace())` **first renders end-to-end** unprivileged; STRONG PT
  is **not trusted until Z2 is green**; CEILING is AMD LBR (bounded complement — the
  complete AMD form is the §D3 stepper via `CEILING_FREE`). Depends on Z0, Z2, Core §1/§2,
  call-descent §L3, amd §LBR (+ Part III P2 block-step / P3 boundary snapshot, consumed as
  they land), zen2 §W2 / §D3.
- **§Z3 after Z1 *and* (Z2 **or** Z1's §D3).** The arm-time managed composition (soft-dirty-gated
  `track()`) + the managed-tier `Dispose` swap. Needs .NET 8+.
- **§Z4 — split dependency.** The **DEFAULT** depends **only on Core §0.2 + the Z0 arm-tid
  carry** (no §Z3 needed). The opt-in **ESCALATION** depends on **§Z3 *and* §Z2** — the §D3
  satisfaction of §Z3 can't exercise a hop, so the cross-thread merge needs the Z2-validated
  per-thread PT path.
- **§Z5 last, but partly carried from Z0** (the `skip_reason` passthrough). Pure self-probe,
  every host; enforces the **never-a-hard-failure** invariant.

```text
  Core §2 ─► Z2·SYNTHETIC PT fixture ─┐ TRUST-GATE (host-testable where libipt built · FRONT-LOADED ‖ Z0)
                                       ▼   (Z1 STRONG PT tier not believed until green)
  Core §0/§1 + Bindings ─► Z0 region-free arm surface ─► Z1 CAPTURE MODE
     (10 shims; arm_tid carry;              ├─ WEAK single-step L3 DESCEND_ALL (any x86-64 Linux · FIRST end-to-end;
      Z5 self-skip scaffold)                │                                   bounded ring → render_versioned/self codeimage)
     call-descent §L3 / amd §LBR·P2·P3 /    ├─ STRONG PT recorder-backed        (Intel BM · decode validated in Z2)
     zen2 §W2·§D3 ──────────────────────────└─ CEILING AMD LBR complement       (Zen 3+ · bounded → truncated;
                                                                                 complete AMD = §D3 stepper via
                                                                                 CEILING_FREE [+P2 blockstep])
                                                        │
  Managed §D0.1/§D0.2/§D3 ─► Z3 managed capture ◄───────┘   (MethodLoadVerbose → codeimage_track[soft-dirty-gated]
                                    │                          ⊕ recorder ⊕ /proc/self/maps; managed Dispose→render_versioned)
  Managed §D0.4+§D4, Core §0.2 ─► Z4 async ◄──┘   (DEFAULT = thread-scope + arm-tid carry; ESCALATION needs Z3 AND Z2)
                                    │
  Core §3.1/§3.2 + inproc skip_reason ─► Z5 honest-degradation UX + privilege message
                                             (compose every tier's skip_reason · every host · scaffolded at Z0 · NEVER hard-fails)

  VALUE LADDER: Z0 surface ─► Z1 WEAK (FIRST end-to-end, any Linux) ─► Z2 clean decode ─► Z3 managed ─► Z4 async ─► Z5 UX
```

**New-item coverage → phase:** netNew #1 splits across **Z0** (SURFACE arm), **Z1** (POLICY —
bounded ring + versioned render), **Z2** (PT DECODE selection; the `PERF_EVENT_IOC_SET_FILTER`
filter is **consumed from Core §2**, not owned here); #2/#4 land in **Z0**/**Z1**; #3 in **Z3**;
#5/#6 in **Z4**/**Z5**. Earliest CI protection is the phase gate above / the Net-new table's why.

> **Gating honesty.** Only Z0, Z2's synthetic half (where libipt is built), Z1's
> native-leaf/ptrace tier, Z4's default + merge core, and Z5 are protected by ordinary CI; the
> strong PT/AMD and live managed/async chains are Intel-bare-metal/privilege-gated (see the
> CI-protection asymmetry note under Tests). **Z2 and (largely) Z4 own no net-new capture
> primitive** — by design they *consume* Core §2 and Managed §D0.4/§D4.

---

## Cross-cutting decisions

The umbrella's [shared
invariants](../archive/plans/scoped-inprocess-tracing-plan.md#cross-cutting-decisions-shared-by-all-three-slices),
re-stated as they *bind the empty ctor* — a scope with **no `[base,len)` at all**. Each is a
*consumed* posture.

> **Governing invariant (Z5, scaffolded at Z0).** `using (new AsmTrace())` is **never a hard
> failure** and never emits a partial whole-window trace *as complete* — the scope
> self-explains what it did and did not capture, everywhere, including off Linux.

- **Thread-scope, not logical-scope — and it says so (§Z4).** A cross-thread/awaited close
  does **not** silently drop the arming thread's work. Because the shipped §0.2 backstop is
  **region-keyed** (`find_region` → NULL region-free), the empty ctor carries `arm_tid` on its
  own handle (Z0) and flags `trace.truncated` there, rather than the no-frame result
  `asmtest_ss_end` ([:279](../../../src/ss_backend.c#L279)) gives on the wrong thread. **Default
  = honest-thread-scope-with-flag, not auto-stitching**; stitching through
  `asmtest_hwtrace_stitch` ([:1233](../../../src/hwtrace.c#L1233)) is an **explicit opt-in
  escalation**.

- **Whole-window is honest-but-noisy; region-scoped is clean (§Z1 mode, §Z5 presentation).**
  With no range to filter to, an unwarmed body traces the JIT compiling it, GC, and BCL
  alongside `HotPath` — the shipped in-region filter (`read_region`
  [:74](../../../src/pt_backend.c#L74); `ss_on_sigtrap` [:143-148](../../../src/ss_backend.c#L143))
  is exactly what the whole-window path lifts. Honesty is a presentation obligation: the noise
  is *labelled* via Core §3.1's shipped `symbolize_bucket` + `region_name` reverse resolver
  (and the CAP_BPF/BTF-gated emission-slicer for the temporal split) + a §3.2 banner, never
  silently mixed.

- **Single-step only against native leaves — *never* live managed code (§Z1 routing, §Z3
  enforcement).** Arming single-step swaps the **process-wide SIGTRAP disposition** CoreCLR's
  PAL owns and can put a sibling runtime thread into runaway stepping — so §Z1 WEAK is
  **native-leaf / ptrace-only** and region-free managed routes to the §D3 helper running L3.
  Tests assert the guards fire; L3 is never transparent.

- **Self-skip is the house style; the boundary is *which* instructions ran + their bytes, not
  values.** Every tier compiles and runs everywhere, degrading to a no-op that records *why*
  (stub ships with §Z0, enriched at §Z5). The empty ctor yields the executed instruction
  stream rendered *as of the version live in the window* — §Z1 wires `render_versioned` for
  the native leaf, §Z3 extends it to managed (re-pointing `AsmTrace.Dispose`
  [:1016](../../../bindings/dotnet/hwtrace/HwTrace.cs#L1016) off the version-blind render
  [:1106](../../../src/hwtrace.c#L1106)) — **not** per-step register/memory values (the emulator
  tier's job).

- **Linux-only, across every binding (§Z5 owns the off-Linux promise).** The whole facility —
  region-free arm, PT/LBR/ptrace tiers, the codeimage recorder (non-Linux `#else` → `ENOSYS`
  [codeimage.c:492](../../../src/codeimage.c#L492)) — is Linux-only, and on Linux the tiers are
  further host-gated, so the empty ctor **cannot be a default**: only on Intel bare metal with
  a one-time grant is the footprint genuinely *import + scope*; everywhere else the same code
  self-skips.

---

## Relationship to the other plans

This plan is the **capstone / fourth slice**: it owns no new capture primitive, only the
region-free arm **surface** (§Z0) and the arm-time **integration** (§Z1–§Z5).

- **[scoped-inprocess-tracing-plan.md](../archive/plans/scoped-inprocess-tracing-plan.md)** — the **umbrella**
  this plan caps; its aspirational-target companion, collapsing the region-required ctor to
  empty.
- **[scoped-tracing-core-plan.md](../archive/plans/scoped-tracing-core-plan.md)** — the **decode substrate**:
  Core §2's recorder-backed libipt glue (`read_recorder` [:96](../../../src/pt_backend.c#L96), no
  in-region filter, over `read_region` [:74](../../../src/pt_backend.c#L74); §Z2 also consumes its
  `PERF_EVENT_IOC_SET_FILTER`), §3.1's shipped `symbolize_bucket`/`region_name` + gated eBPF
  slicer, §3.2's drain, and §0/§1 (per-thread state, `render_versioned`).
- **[scoped-tracing-managed-plan.md](scoped-tracing-managed-plan.md)** — the managed-JIT
  capability: §D0.1/§D0.2 the `name→(addr,size,version)` producer §Z3 composes; §D3 the
  hardware-free managed degradation §Z1/§Z3 route to; §D0.4 + §D4 the stitch upstream §Z4 fills.
- **[scoped-tracing-bindings-plan.md](../archive/plans/scoped-tracing-bindings-plan.md)** — the **scope
  construct** + lazy arm + auto-name + closing-tid assert; §Z0 removes the required `NativeCode`
  argument across the reference shim and its nine mirrors.
- **[hardware-trace-plan.md](../plans/hardware-trace-plan.md)** — its **Phase 2** is the same libipt glue
  as Core §2, run self (`pid==0`); inherits its PT-hardware-gated forward-look posture.
- **[amd-tracing-plan.md](../plans/amd-tracing-plan.md)** — the AMD **LBR ceiling** §Z1 auto-selects on
  Zen 3+ (a bounded complement), plus the two Part III upgrades §Z1 consumes as they land:
  **Phase 2 BTF block-step** (the cheap complete-flow form of the §D3/W2 stepper — every Zen
  incl. Zen 2, rootless) and **Phase 3 eBPF boundary snapshot** (deterministic LBR window at
  scope entry/exit, Zen 4/5); **[zen2-singlestep-trace-plan.md](../plans/zen2-singlestep-trace-plan.md)** — its **W2** ptrace
  stepper is the CI-runnable fallback §Z1/§Z3 conceal; **[call-descent-plan.md](../archive/plans/call-descent-plan.md)**
  — the **L3 `DESCEND_ALL`** guards the stepper rides; **[multi-language-bindings-plan.md](../archive/plans/multi-language-bindings-plan.md)**
  — the shared-substrate + thin-shim pattern.

---

## Tests, Docs, and CI

Every net-new symbol compiles into the existing `libasmtest_hwtrace` objects and is exercised
from [examples/test_hwtrace.c](../../../examples/test_hwtrace.c) in the shipped TAP convention —
`static void test_<subject>(void)` cases via the `CHECK(c, msg)` macro. Source-only additions
landing in `make hwtrace-test` and the docker fan-out lanes already wired for the slices; no
new object file or pkg-config knob.

> **Governing rule (no-untested-hardware-code).** Nothing in Z1–Z4 ships a live path without
> **both** a synthetic-input host-testable twin **and** a self-skipping availability + platform
> gate: every live case probes `asmtest_hwtrace_available()` / `skip_reason()` /
> `asmtest_pt_decoder_present()` ([pt_backend.c:65](../../../src/pt_backend.c#L65)), is wrapped in
> `#if defined(__linux__) && defined(__x86_64__)`, prints `# SKIP <subject>: <reason>`, and
> carries an `alarm()`/ITIMER watchdog if it could hang — so `make hwtrace-test` self-skips
> cleanly on AMD / VMs / standard CI, never fails.

### Host-testable new tests

These run under ordinary CI — **no PT hardware, no privilege** (the Z2 synthetic decode
additionally needs **libipt at build**, self-skipping where absent). The pattern mirrors the
shipped `test_pt_image_from_codeimage`, `test_stitch_slices`, `test_amd_reconstruction`, and
`test_cs_reconstruction`.

| Test | Phase | Synthetic input → assertion | Lane(s) |
|---|---|---|---|
| `test_zeroctor_scope` | Z0 | Region-free arm opens a per-thread block with **no** registered region (bypasses `find_region` [:486](../../../src/hwtrace.c#L486), never calls `register_region` [:432](../../../src/hwtrace.c#L432)); lazy arm **claims no slot / installs no SIGTRAP**; auto-name resolves; render-on-close returns text; the handle-carried closing-vs-arming tid check fires; the handler path takes **no malloc/lock**; nested region-free + region-scoped scopes install/restore SIGTRAP **exactly once**; repeated construct/dispose neither exhausts the frame stack nor leaks the stream buffer | `docker-hwtrace`, `docker-hwtrace-bindings` |
| `test_wholewindow_ss_descend` | Z1 | The lifted **pre-allocated bounded absolute-RIP ring** replacing `ss_on_sigtrap`'s in-region filter ([:143-148](../../../src/ss_backend.c#L143)), rendered via `render_versioned` against a self codeimage, driving the **WEAK single-step L3 DESCEND_ALL** native leaf — asserts capture **and self-truncation** via **both** paths (buffer overflow flag; §L3 budget/watchdog), never that L3 is transparent | `hwtrace-test`, `docker-hwtrace` |
| `test_wholewindow_decode` | Z2 | A checked-in / encoder-produced **TIP/TNT/PSB stream** feeding `asmtest_pt_decode_window`'s real libipt body ([:198](../../../src/pt_backend.c#L198), guard `ASMTEST_HAVE_LIBIPT` [:62](../../../src/pt_backend.c#L62)) through `read_recorder` ([:96](../../../src/pt_backend.c#L96)) with **no in-region filter** — the only non-hardware route to this body, contrasting `read_region` ([:74](../../../src/pt_backend.c#L74), which dies at the first out-of-range IP [:179-188](../../../src/pt_backend.c#L179)). **Self-skips where libipt is absent** | `hwtrace-test` (front-loaded, ‖ Z0, x86_64) |
| `test_zeroctor_managed_compose` | Z3 | A **synthetic `MethodLoadVerbose` stream** (no runtime): the listener→(addr,size,version) map feeds `asmtest_codeimage_track` on the self path (`pid==0`, [:314](../../../src/codeimage.c#L314) / track [:373](../../../src/codeimage.c#L373), soft-dirty-gated); `Dispose` re-points from the version-blind render ([:1106](../../../src/hwtrace.c#L1106)) to `render_versioned` ([:1172](../../../src/hwtrace.c#L1172)) | `hwtrace-test` |
| `test_asynchop_flag` | Z4 | The honest **default**: a cross-thread/awaited close rides the **handle-carried** arm-tid so the trace is flagged `truncated` (not the region-keyed §0.2 backstop, which can't fire region-free) rather than silently dropping the arming thread's TLS — hop **flagged, never dropped, never auto-stitched** | `hwtrace-test` |
| `test_stitch_slices` *(shipped)* | Z4 | Existing pure seq-ordered merge over synthetic `slice_t` ([asmtest_hwtrace.h:246](../../../include/asmtest_hwtrace.h#L246)); the opt-in producer is wired **into** it | `hwtrace-test` |
| `test_degradation_message` / `test_wholewindow_banner` | Z5 | Pure self-probe on **any** host: composes every tier's `skip_reason()` + §3.1 bucket labels (`symbolize_bucket` / `region_name`) + §3.2 banner into the scope's `Path`; asserts the **actionable privilege message** and that the empty ctor is **never a hard failure** | `hwtrace-test` (every host) |

The Z5 self-skip scaffold ships **with Z0**; `test_zeroctor_scope` grows from a scaffold
assertion at Z0 into the full composed-message assertion at Z5. Per-binding coverage reuses
the shipped `hwtrace-<lang>-test` lanes, each self-skipping internally; process-global
single-step cases run serialized (`cargo --test-threads=1`).

### Forward-look live paths (ship self-skipping + gated)

Each consumes a host-testable twin above and adds nothing to the net-new capture list — the
primitives are Core §2 / Managed §D0/§D4. All are **Linux-only**.

| Live path | Phase | Real lane | Gate — self-skips otherwise |
|---|---|---|---|
| WEAK single-step L3, **managed** region-free | Z1 | `docker-hwtrace` (+ §D3 ptrace-stealth lane) | Routes to the concealed ptrace stepper (single-step **forbidden** against live managed code). Needs `SYS_PTRACE` / Yama `PR_SET_PTRACER` (default Docker seccomp on host kernel ≥ 4.8, else `CAP_SYS_PTRACE`) |
| STRONG PT recorder-backed whole-window arm | Z1 / Z2 | self-hosted **bare-metal Intel PT** runner (allowed-to-be-absent) | `intel_pt` PMU + `perf_event_paranoid<0` or `CAP_PERFMON`. **Not trusted until `test_wholewindow_decode` is green.** Self-skips on AMD/ARM/VMs/Docker |
| CEILING **AMD LBR** bounded complement | Z1 | `docker-hwtrace-amd` (`--cap-add=PERFMON`, [mk/docker.mk:280](../../../mk/docker.mk#L280)) | Zen 3+ bare metal + `CAP_PERFMON`; ~16-branch ring → flags `truncated` (complete AMD = §D3 stepper via `CEILING_FREE`). Zen 2 = `EOPNOTSUPP` → ptrace |
| Live arbitrary-managed-method whole-window | Z3 | [`docker-hwtrace-jit-dotnet`](../../../mk/docker.mk#L242) (inner recipe [`hwtrace-jit-dotnet`](../../../mk/native-trace.mk#L327); also `-jit-node` / `-jit-java`) | A live .NET 8+ runtime **plus** Intel PT (Z2) or §D3. Self-skips when the runtime re-tiers/moves code, when soft-dirty is unavailable, or when ptrace is denied |
| Version-timeline noise attribution (eBPF PROT_EXEC slicer) | Z3 / Z5 | `docker-hwtrace-codeimage` (CAP_BPF/CAP_PERFMON/SYS_PTRACE) | **CAP_BPF + kernel BTF** ([codeimage.c:618](../../../src/codeimage.c#L618)/[:584](../../../src/codeimage.c#L584)); degrades to the coarser soft-dirty version-timeline when absent |
| L3 DESCEND_ALL guard-fire (managed) | Z1 / Z3 | `docker-hwtrace-jit-dotnet-descend-all` (also `-java-descend-all`) | Asserts the denylist / budget / `ITIMER_REAL`+`SIGALRM` watchdog fire and the stepper self-truncates — **never** that L3 is transparent |
| Live async hop→tag→merge chain | Z4 | *(no CI coverage — forward-look)* | Live managed runtime + bare-metal Intel PT per-thread events (the §D3 single-thread stepper **cannot** exercise a hop) |
| Bundled `asmtest-stealth-helper` supply-chain surface | Z1 / Z3 | `package-libs-verify` ([release.yml:77](../../../.github/workflows/release.yml#L77), [mk/bindings.mk:474](../../../mk/bindings.mk#L474)) | Fail-closed: the helper + `libasmtest_hwtrace` present and `$ORIGIN`-rpath'd per platform |

> **CI-protection asymmetry (stated honestly).** Only Z0, Z2's synthetic half (where libipt is
> built), Z1's native-leaf/ptrace tier, Z4's default + merge core, and Z5 are guarded by
> ordinary CI. Z1's strong PT / AMD tiers, Z3/Z4's live managed/async chains, and the
> CAP_BPF/BTF eBPF slicer are silicon/privilege/runtime gated and self-skip on
> AMD/ARM/VMs/Docker/macOS — materially weaker protection than the emulator tier. On Intel bare
> metal with a one-time grant the footprint genuinely is *import + scope*; everywhere else the
> same code self-skips and says why.

### Docs to update

- **[../scoped-tracing-implementation.md](../../scoped-tracing-implementation.md)** — the summary
  opens with the aspirational `using (new AsmTrace())` snippet (lines 24–29) as if delivered,
  but the shipped ctor still **requires** a `NativeCode`
  ([HwTrace.cs:997](../../../bindings/dotnet/hwtrace/HwTrace.cs#L997)). Add a **"Zero-config /
  whole-window scope (forward-look)"** subsection stating plainly the empty-ctor form is *not
  built* (the region-free arm, the validated whole-window PT decode — `#ifdef`-stubbed to
  `ENOSYS` [pt_backend.c:266](../../../src/pt_backend.c#L266) on every current host — managed
  byte-discovery, and async stitching are this plan's Z0–Z5), and correct the top snippet's
  framing.
- **[scoped-inprocess-tracing-plan.md](../archive/plans/scoped-inprocess-tracing-plan.md)** umbrella slice table
  (lines 162–166) — add a fourth **"Zero-config / whole-window"** row pointing here; `Owns` =
  *the region-free arm surface + empty-ctor across all ten bindings, the whole-window
  capture-mode selection + synthetic-fixture decode validation, arm-time managed composition,
  the honest async default, and the honest-degradation UX*; `New-item coverage` = *1–6*. Note
  it adds **surface + integration only, no new capture primitive**.
- Extend [docs/guides/tracing/hardware-tracing.md](../../guides/tracing/hardware-tracing.md) with
  the three-tier region-free ladder and the synthetic-fixture decode-validation mode;
  [docs/internal/analysis/trace-parity-matrix.md](../analysis/trace-parity-matrix.md) with the
  whole-window decode's *validated-on-synthetic / live-forward-look* status; and
  [docs/reference/troubleshooting.md](../../reference/troubleshooting.md) /
  [portability.md](../../reference/portability.md) with the `SkipReason` codes, the one-time
  provisioning table, and the Linux-only floor.

---

## Risks and open points

- **The whole-window PT decode is the least-tested piece — and every strong-form tier rides
  it.** The real body ([:198](../../../src/pt_backend.c#L198)) sits behind `#ifdef
  ASMTEST_HAVE_LIBIPT`; every current host compiles the `ENOSYS` stub
  ([:266](../../../src/pt_backend.c#L266)), and the real body has never run against live Intel
  PT. **§Z2's synthetic fixture is the trust-gate** — but host-testable **only where libipt is
  built** (the x86_64 `hwtrace-test` lane); libipt-less hosts self-skip and provide no de-risk.

- **The region-free arm + lifted policy must be a no-op for existing callers.** They sit *next
  to* the shipped range-keyed path — `asmtest_ss_begin_ex` still `EINVAL`s on a null range,
  `try_begin` still keys on the registered name, `AsmTrace(NativeCode …)` stays byte-identical,
  `begin`/`end` stay `void` (the mode is additive). `test_zeroctor_scope` asserts the
  region-keyed contract is untouched.

- **The bounded record buffer overflows fast on a whole window — by design.** The lifted policy
  is a **fixed `SS_STREAM_CAP` (65536-offset / 512 KiB) ring**, malloc-free in signal context;
  it cannot grow in the handler. A `DESCEND_ALL` window overflows near-instantly and sets
  `trace.truncated` — **buffer-overflow truncation**, distinct from the §L3 budget/watchdog
  truncation. Both must surface through §Z5's banner; neither may be presented as complete.

- **The synthetic PT fixture cannot substitute for silicon.** It exercises the decode *logic*
  but not a real AUX ring's PSB cadence, `aux_tail` wrap, timing packets, or the anonymous-JIT
  `PERF_EVENT_IOC_SET_FILTER` fallback. The live smoke is a trusted-branch-gated forward-look on
  a **bare-metal Intel PT runner that does not yet exist** — as
  [hardware-trace-plan.md](../plans/hardware-trace-plan.md) accepts. Passing §Z2 is necessary, not
  sufficient, for *trusted*.

- **The async-hop default is new integration, not a freebie.** The region-keyed §0.2 backstop
  ([:898-907](../../../src/hwtrace.c#L898)) **cannot fire** on the region-free arm (`find_region` →
  NULL), so the empty ctor must carry `arm_tid` on its own handle and re-implement the check
  (Z0/Z4 wiring). Full stitching needs Z2's validated per-thread PT (the §D3 stepper cannot
  exercise a hop) and has **no CI coverage** — so it stays opt-in.

- **Managed single-step is a footgun.** Arming EFLAGS.TF swaps the process-wide SIGTRAP
  disposition CoreCLR's PAL owns, so §Z1 WEAK is native-leaf/ptrace-only and managed routes to
  §D3 running L3. Tests assert the §L3 guards *fire* and the stepper *self-truncates*
  (`docker-hwtrace-jit-dotnet-descend-all`).

- **Availability is a host property — so the empty ctor cannot be a default, and strong-path CI
  is materially weaker than the emulator tier.** Whole-window PT is Intel-bare-metal only;
  AMD LBR needs Zen 3+; the version-timeline needs `CONFIG_MEM_SOFT_DIRTY`; the eBPF slicer
  needs CAP_BPF + BTF. Only §Z0, §Z2's synthetic half, §Z1's native-leaf/ptrace tier, §Z4's
  default + merge-core, and §Z5 run under ordinary CI. The mitigation is the no-untested-hardware
  rule + the CI-runnable §D3 stepper (`docker-hwtrace-codeimage`, `SYS_PTRACE`); §Z5's composed
  privilege message ([netNew #6](#net-new--what-this-plan-owns-surface-integration-presentation))
  exists because the code cannot grant itself the hardware.

- **Open point — the managed render swap lands with §Z3, not before.** §Z1 wires
  `render_versioned` for the native-leaf tier against its self codeimage; re-pointing the
  *managed* `AsmTrace.Dispose` ([:1016](../../../bindings/dotnet/hwtrace/HwTrace.cs#L1016)) off the
  version-blind render ([:1106](../../../src/hwtrace.c#L1106)) is correct **only once §Z3's producer
  exists**, else it renders `(no bytes @version)`.

---

## Sources

Background — the scoped `using` model, the thread/async boundary, the whole-scope-vs-method
fork, temporal byte-versioning, and the full citation set — is in
[Analysis: the scoped `using` model](../analysis/scoped-inprocess-tracing.md#sources) and its
sibling [jit-runtime-tracing.md](../analysis/jit-runtime-tracing.md#sources).
