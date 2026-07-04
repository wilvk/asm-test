# asm-test — Scoped in-process tracing (the `using`/RAII/`with` model): implementation plan

A roadmap for the **cooperative, in-process, developer-ergonomics** face of the
tracing machinery: bracket a region of a program's *own* code with a scope
construct — `using (new AsmTrace()) { … }` in C#, RAII in C++/Rust, `with` in
Python, `defer` in Go/Zig, a block in Ruby/Lua/Node/Java — and get back the
**assembly-language path that executed inside it**, with the developer-visible
surface reduced to the **package import plus the scope**.

This plan implements the conclusions of
[Analysis: non-intrusive in-process managed-runtime tracing — the scoped `using`
model](../analysis/scoped-inprocess-tracing.md). Read that first for *why* it
works, the five-axis definition of "non-intrusive," the honest host
preconditions, and the per-binding feasibility matrix; this document is the *how*
and *in what order*.

It is a **sibling** of three shipped/planned plans and adds no new capture
primitive or decoder of its own — it **repackages** shipped machinery:

- the self, per-thread hwtrace region markers
  (`asmtest_hwtrace_begin`/`end`, [src/hwtrace.c:654](../../src/hwtrace.c#L654),
  [:762](../../src/hwtrace.c#L762)) and backend auto-selection
  (`asmtest_hwtrace_auto`, [src/hwtrace.c:327](../../src/hwtrace.c#L327));
- the self (`pid == 0`) time-aware code-image recorder
  ([include/asmtest_codeimage.h](../../include/asmtest_codeimage.h),
  `src/codeimage.c`) for the decode bytes;
- Capstone rendering (`src/disasm.c`);
- the per-language `region(name, fn)` scope helpers the ten bindings already ship.

The **only genuinely new** engineering is a small shared C/decode core plus thin
per-language shims — the split this plan's [three slice plans](#the-slice-plans)
own.

> Status legend: **planned** unless noted. Update this file (and the slice files)
> as slices land, the way
> [hardware-trace-plan.md](hardware-trace-plan.md) and
> [multi-language-bindings-plan.md](multi-language-bindings-plan.md) track theirs.

---

## The ask, and the determination

The developer-visible target (from the analysis doc's opening):

```csharp
using AsmTest;                 // (1) the package import

using (new AsmTrace())         // (2) the scope — empty ctor, no config
{
    HotPath(data);             // whatever runs here: its real JIT'd asm is captured
}
```

**Determination (verbatim from the analysis): yes — and most of the mechanism
already ships.** A scope is exactly an enable/disable window, and asm-test already
brackets a region with `begin`/`end` markers doing in-process, self-attached,
per-thread capture. Three host preconditions no API can turn into code — it can
only *hide* them and self-skip when absent:

1. **Non-intrusive + in-process ⇒ hardware trace.** Only out-of-band Intel PT /
   AMD LBR / ARM CoreSight pass all five non-intrusion axes. The universal
   fallback, EFLAGS.TF single-step, is exact but intrusive (a `SIGTRAP` per
   instruction on the traced thread).
2. **"Only import + using" rests on privilege + hardware the code cannot grant
   itself** (`perf_event_paranoid`/`CAP_PERFMON`; Intel bare metal for PT). The
   import *detects* this (`available()`/`skip_reason()`) and degrades the scope to
   a recorded no-op.
3. **The temporal-bytes problem is much easier in-process** — the tracing thread
   reads its **own** JIT memory; the code-image recorder already supports
   `pid == 0` (self, [include/asmtest_codeimage.h:78](../../include/asmtest_codeimage.h#L78)).

Net: on an Intel bare-metal host with a one-time privilege grant the footprint
genuinely **is** import + scope; everywhere else the same code compiles and
self-skips, or consciously accepts intrusiveness. **The whole using-model is
Linux-only across every binding** (off Linux the lifecycle self-skips to
`(void)name` no-ops and the emulator is the only tier) — a first-class property of
the facility, not a footnote.

---

## What already ships vs. what is new

**Already implemented (verified in the tree):**

- Self, per-thread PT / AMD / single-step region capture bracketed by
  `begin`/`end` (`asmtest_hwtrace_begin`/`end`, `src/hwtrace.c`).
- A .NET scope wrapper — `HwTrace.Region(name, Action)` with balanced markers —
  and a `region(name, fn)`-shaped helper in **all ten** bindings.
- Backend auto-selection + clean self-skip (`asmtest_hwtrace_auto`,
  `asmtest_trace_auto`, `available()`/`skip_reason()`).
- The self (`pid == 0`) time-aware code-image recorder for the bytes
  ([include/asmtest_codeimage.h](../../include/asmtest_codeimage.h), `src/codeimage.c`).
- In-process region resolution from an address (`asmtest_proc_region_by_addr`,
  [include/asmtest_ptrace.h:291](../../include/asmtest_ptrace.h#L291)) and Capstone
  rendering (`src/disasm.c`).

**New, to reach "empty ctor / import + scope, handle everything":**

1. A guaranteed-cleanup **scope construct** per binding (`IDisposable AsmTrace`,
   RAII `RegionScope`, `with AsmTrace()`, `defer`), a thin shim over
   register-then-`begin`/`end` with auto-name and default-sink emission on close.
2. An **arm hook** per binding — a real module-init where one exists
   (`[ModuleInitializer]`, `func init()`, `require`, `static{}`), a lazy-once
   magic-static where none does (C++/Rust/Zig) — recommended **lazy first-scope**
   everywhere so mere import claims no global slot / installs no SIGTRAP handler.
3. A shared **render-on-close** path (recorded offsets → Capstone text → emit)
   that **no** binding wires into scope-close today. The shared C `end`
   reconstructs the packet stream into `asmtest_trace_t` offsets for every backend
   ([src/hwtrace.c:809](../../src/hwtrace.c#L809)), but turning those offsets into
   disassembly *text* on close is unwired in all ten bindings (the .NET scope is
   `begin`/`try`/`finally`-`end` with no render,
   [bindings/dotnet/hwtrace/HwTrace.cs:602](../../bindings/dotnet/hwtrace/HwTrace.cs#L602)).
4. Cheap **C-layer fixes**: make `begin` **return an error** when a slot is
   active (today it silently no-ops, [src/hwtrace.c:656](../../src/hwtrace.c#L656)),
   **record the arming thread id** in `begin` so `end` can flag `truncated` on a
   same-thread mismatch (today neither is done), and make `register_region`
   **idempotent by name** so a scope object that registers on **every** construction
   reuses one slot instead of exhausting the process-global 32-entry table (Core §0.4).
5. **Per-thread hwtrace state** to replace the process-global single slot (the
   MVP contract, [include/asmtest_hwtrace.h:149](../../include/asmtest_hwtrace.h#L149)),
   lifting no-nesting / no-concurrency / no-multi-binding for all ten bindings at once.
6. The **libipt decode-against-self-code-image glue** plus **whole-window
   completeness** (noise attribution + snapshot drain) — the remaining forward-look
   piece. The recorder and Capstone rendering already exist, and libipt's image
   callback is already wired — but it is **region-scoped, not recorder-backed**:
   [src/pt_backend.c:93](../../src/pt_backend.c#L93) installs
   `pt_image_set_callback(image, read_region, …)` whose `read_region` returns
   `-pte_nomap` outside `[base, len)` ([:47](../../src/pt_backend.c#L47)), so
   whole-window decode needs a recorder-backed callback, the Q2 noise-attribution
   pass, and the Q3 snapshot drain. Shared with
   [hardware-trace-plan Phase 2](hardware-trace-plan.md#phase-2---attach-to-foreign-jit-tracing-byte-source-recorder-done-pt-attach-decode-forward-look).
7. Managed-tier extras: **.NET-8 `MethodLoadVerbose` address resolution** (retires
   the version-fragile `PrepareMethod` path), **`AsyncLocal`/`AsyncLocalStorage`/
   JVMTI piece-D async-hop stitching** (the shared logical-operation model across
   .NET, Node, and the JVM), and a **concealed out-of-process ptrace stepper**
   scope for hosts with no hardware trace (Zen 2, Docker-on-Mac).

---

## The slice plans

The analysis rescopes the roadmap item from *"a .NET `IDisposable AsmTrace`"* to
**"a cross-binding scoped-trace facility: one shared C/decode core + thin
per-language shims,"** with .NET as the reference shim rather than the deliverable.
This umbrella splits that into three focused slice plans, dependency-ordered:

| Slice | Plan | Owns | New-item coverage |
|---|---|---|---|
| **Core** | [scoped-tracing-core-plan.md](scoped-tracing-core-plan.md) | The two cheap C-layer fixes, per-thread hwtrace state, the shared render-on-close path, the arming-thread assert, the libipt decode-against-self-code-image glue, and whole-window completeness (Q2 noise attribution + Q3 snapshot drain) | 3, 4, 5, 6 |
| **Bindings** | [scoped-tracing-bindings-plan.md](scoped-tracing-bindings-plan.md) | The scope construct + auto-name + arm hook + thread-id assert across the **native / interpreter / Go** tiers (C++, Rust, Zig, Python, Ruby, Lua, Go), with **.NET as the reference shim shape** | 1, 2 |
| **Managed** | [scoped-tracing-managed-plan.md](scoped-tracing-managed-plan.md) | The **managed JIT tier** — Node → JVM → .NET — on the PT/ptrace clean path: `MethodLoadVerbose` address resolution, the shared `AsyncLocal`/`AsyncLocalStorage`/JVMTI piece-D async-hop stitching model, and the concealed ptrace stepper scope | 7 |

Each slice plan carries its own **code-change references, Tests, and Docs**
sections (the deliverable this work was scoped to produce); this umbrella is the
index and delegates all three to the slice plans.

### Sequencing (dependency-correct order)

The analysis's own phasing is A → B → C → D; the dependency-correct build order
interleaves them so the cheap de-risking fixes land first:

1. **Core §0 first** — the two C-layer fixes (`begin`-returns-error,
   record-arming-thread-id) + the shared render-on-close path. Cheap, and they
   de-risk *every* binding (each otherwise reinvents a nesting guard and a
   thread-id assert). Render-on-close unblocks the Bindings slice's emit-on-close.
2. **Bindings slice (analysis phases A + B)** — prove the shim shape on the
   **zero-hazard** tier first (**Python + C++**, then **Zig** near-free), then the
   **migration mitigation on Go** (`LockOSThread` already wired,
   [go full-test flaky-crash finding](../analysis/scoped-inprocess-tracing.md#the-hard-cases-called-honestly)).
   Ruby/Lua/Rust slot in alongside. .NET is written here as the *reference* shim.
3. **Core §1–§2 (analysis phase C)** — per-thread hwtrace state and the libipt
   decode-against-self-code-image glue land **before, not during, the managed
   bindings**. These are the single highest-leverage shared investments; the
   libipt glue is the same code
   [hardware-trace-plan Phase 2](hardware-trace-plan.md#phase-2---attach-to-foreign-jit-tracing-byte-source-recorder-done-pt-attach-decode-forward-look)
   needs and unblocks the clean managed path for every binding at once.
4. **Managed slice (analysis phase D)** — the hard cases **Node → JVM → .NET**,
   each on the PT/ptrace path with its own async-hop stitching.

```
Core §0 ──► Bindings (Python, C++, Zig, Go; Ruby, Lua, Rust; .NET ref)
   │                                                    │
   └──► Core §1 (per-thread state) ──► Core §2 (libipt glue) ──► Managed (Node → JVM → .NET)
```

The Managed edge is not uniform per sub-phase: the slice's two **CI-protective**
deliverables — the §D3 ptrace-stealth stepper and the §D4 `test_stitch_slices` merge
unit — depend only on **Core §1** (per-thread state), **not** §2's PT-hardware-gated
libipt glue, so they can land right after §1. Only the clean in-process PT path
(§D0–§D2 live capture) is a first consumer of §2.

---

## Cross-cutting decisions (shared by all three slices)

These hold across every slice and every binding; each slice plan restates only its
local specifics.

- **The scope is a *thread* scope, not a logical-operation scope.** The perf event
  follows the calling thread (`pid == 0, cpu == -1`,
  [src/hwtrace.c:684](../../src/hwtrace.c#L684)). Work that hops threads (`await`,
  `Task.Run`, `go func()`, thread-pool continuations) is **not captured** unless
  the Managed slice's async-hop stitching is active. **Every** shim must compare
  the closing thread id against the arming one and flag `truncated` on a mismatch —
  the same never-emit-partial-as-complete posture as the existing `truncated` bit
  ([include/asmtest_trace.h:59](../../include/asmtest_trace.h#L59)). This is what
  Core §0's *record-arming-thread-id* fix enables once, for all ten shims.
- **You get everything that ran — including the runtime.** An unwarmed body traces
  the JIT compiling it, GC, BCL plumbing. The empty-scope *whole-window* mode is
  honest but noisy; the *region-scoped* mode (used by the shipped decoders, which
  already drop out-of-region instructions —
  [src/pt_backend.c:108](../../src/pt_backend.c#L108),
  [src/ss_backend.c:99](../../src/ss_backend.c#L99)) is clean. Docs must set this
  expectation and point at the warm/`NoInlining`/tiering-pinned discipline in
  [examples/jit_dotnet/Program.cs](../../examples/jit_dotnet/Program.cs).
- **`begin` keys on the region name.** `end` closes the active slot regardless of
  name, but `begin` looks the name up via `find_region`
  ([src/hwtrace.c:413](../../src/hwtrace.c#L413), used at
  [:659](../../src/hwtrace.c#L659)) and silently no-ops on a miss. A self-naming,
  auto-named scope **must register-then-begin under the same generated name** — a
  correctness constraint every shim (including .NET) must honour. Because the scope
  object registers on **every** construction under a call-site-constant name,
  `register_region` **must be idempotent by name** (Core §0.4) — otherwise a looped or
  sprinkled scope exhausts the 32-entry table or aliases the first registration's
  trace.
- **Single-step is for controlled native code only.** Pointed at live managed code
  it swaps the process-wide SIGTRAP disposition the runtime's PAL also owns
  ([src/ss_backend.c:129](../../src/ss_backend.c#L129)) and can put sibling threads
  into runaway stepping. The Bindings slice uses it **only** against known native
  leaves (what the bindings trace today); the Managed slice uses PT/LBR or the
  out-of-process ptrace stepper instead.
- **Self-skip is the house style.** Every scope compiles and runs everywhere; where
  no faithful backend is available it degrades to a no-op that records why
  (`skip_reason()`), never a hard failure. Off Linux this is the norm.
- **The boundary:** you get *which* instructions ran, in what order, and the code
  bytes rendered to assembly — **not** register/memory values per step. Value
  capture stays the emulator tier's job (`emu_result_t`, watchpoints). Same
  boundary as [jit-runtime-tracing.md](../analysis/jit-runtime-tracing.md).

---

## Risks and open points

- **Availability is a host property, not code** (repeated from the hardware-trace
  and AMD plans). Intel PT is Intel-x86-64-bare-metal only; AMD LBR is Zen 3+;
  CoreSight needs a board; all need `perf_event_paranoid`/`CAP_PERFMON`. On
  everything else the scope self-skips or the developer accepts single-step
  (native leaves only) or the concealed ptrace stepper (a second process). The
  facility **cannot be a default** and its automated regression protection is
  materially weaker than the emulator tier's (no hardware-trace CI — see
  [hardware-trace-plan CI](hardware-trace-plan.md#phase-1---hardware-assisted-trace-backends-intel-pt-arm-coresight-planned)).
- **Per-thread state touches a shipped MVP contract.** Lifting the single
  process-global slot ([include/asmtest_hwtrace.h:149](../../include/asmtest_hwtrace.h#L149),
  [src/hwtrace.c:351](../../src/hwtrace.c#L351)) is invasive and must preserve the
  existing single-region behaviour under the old API. Core slice owns the migration
  and its regression tests.
- **The libipt decode-against-self-code-image glue needs PT hardware to validate**,
  exactly like [hardware-trace-plan Phase 2](hardware-trace-plan.md). It ships as
  self-skipping, hardware-gated code; the reconstruction half is host-testable with
  synthetic images the way `test_amd_reconstruction`/`test_cs_reconstruction`
  already are.
- **The async-hop redesign (piece D) is a model change, not a knob** — "thread
  window" becomes "stitched trace of a logical operation." It is the real
  engineering in the Managed slice and is gated behind an explicit opt-in; the
  default stays the honest thread-scope-with-mismatch-flag.
- **Managed single-step is a footgun.** The Managed slice must forbid pointing
  single-step at live managed code (Bindings slice already restricts single-step to
  native leaves); the escape hatch there is PT/LBR or out-of-process ptrace.
- **The whole-window PT experience is the least-complete, least-tested part of the
  set.** Its three pieces — the recorder-backed decode callback (Core §2), the Q2
  noise attribution (Core §3), and the Q3 snapshot drain (Core §3) — the first and
  third of which need bare-metal Intel PT to validate live (none in CI, and this
  dev host is AMD). Real automated protection for the managed path routes through
  the CI-runnable ptrace stepper (Managed §D3) and the host-testable reconstruction
  / symbolize / stitch-merge *unit* tests; the live PT path ships self-skipping and
  forward-look, exactly as [hardware-trace-plan.md](hardware-trace-plan.md) already
  accepts for its own capture.

---

## Relationship to the other plans

- **[hardware-trace-plan.md](hardware-trace-plan.md)** — supplies the PT/CoreSight
  capture substrate this plan brackets; its **Phase 2** libipt decode work is the
  *same* glue as this plan's Core §2 (build once, both benefit).
- **[amd-tracing-plan.md](amd-tracing-plan.md)** — the AMD LBR backend and Tier-B
  stitching this plan's AMD path rides; the data-ring ceiling and `CEILING_FREE`
  escape are its concern.
- **[zen2-singlestep-trace-plan.md](zen2-singlestep-trace-plan.md)** — the W2
  out-of-process ptrace stepper the Managed slice conceals behind the scope façade
  for Zen 2 / Docker-on-Mac.
- **[multi-language-bindings-plan.md](multi-language-bindings-plan.md)** — the
  shared-substrate + per-language-shim pattern this plan reuses; the ten `region()`
  helpers it produced are the Bindings slice's starting point.
- **[call-descent-plan.md](call-descent-plan.md)** — the L3 `DESCEND_ALL`
  denylist/budget/watchdog machinery the whole-scope stepper rides (best-effort).

## Sources

All background — hardware-trace / jitdump / temporal-bytes, the five-axis
non-intrusion definition, the per-binding feasibility matrix, and the full
citation set — is in
[Analysis: the scoped `using` model](../analysis/scoped-inprocess-tracing.md#sources)
and its sibling
[jit-runtime-tracing.md](../analysis/jit-runtime-tracing.md#sources).
