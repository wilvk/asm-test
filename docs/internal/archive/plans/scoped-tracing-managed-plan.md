# asm-test — Scoped-tracing managed-JIT tier (Node → JVM → .NET): implementation plan

The **hard** half of the [scoped in-process tracing
plan](scoped-inprocess-tracing-plan.md) — extending the scope model to the three
**JIT-managed** runtimes where the scope must capture the runtime's **own live JIT
output**, not a separately-materialised native leaf. This is new-item **7** from the
umbrella's
[what-is-new list](scoped-inprocess-tracing-plan.md#what-already-ships-vs-what-is-new):
`MethodLoadVerbose` address resolution, `AsyncLocal`/piece-D async-hop stitching,
and the concealed out-of-process ptrace stepper scope.

The analysis is blunt that this is "the real project," medium-plus effort, and the
one place the four qualifications stop being knobs and become a redesign (the
thread → logical-operation model change). It is deliberately sequenced **last**.

> **Status: partially landed.** The **§D4 shared merge core** (`asmtest_hwtrace_stitch`
> + the slice/bound ABI) and the **version-aware render** (`asmtest_hwtrace_render_versioned`)
> are implemented and host-tested (`test_stitch_slices`, `test_render_versioned`). The
> **Node (§D1)** `scope`/`using`-fallback and **Java (§D2)** try-with-resources
> `AsmTrace` scope constructs over a native leaf are implemented + tested in their
> `hwtrace-<lang>-test` lanes. **Landed since (2026-07):** §D0.1 `MethodLoadVerbose`
> labelling (`JitMethodMap`) and the §D0.2 dependency-free jitdump rundown
> (`DiagnosticsIpc`) ship in the .NET binding as `new AsmTrace(byMethod:, withRundown:)`
> — commits 69ace98, d4e7deb, 8bf7d52, 4aaf473 — exercised by `hwtrace-dotnet-test` and
> the `examples/dotnet/*` lanes. **Forward-look (not yet landed):** §D0.3's named-method
> form and §D0.4 `AsyncLocal` stitching (both slated near-term), the §D1/§D2 per-runtime
> async-hop hooks, and the §D3 live-JIT cross-process address channel — the parts that
> need PT hardware / a second process to validate. See
> [docs/scoped-tracing-implementation.md](../../../scoped-tracing-implementation.md).
>
> Status legend: **planned**, forward-look. The clean managed path needs the
> shared-core libipt glue (PT hardware to validate); the ptrace fallback runs on
> ordinary CI-adjacent hosts (Zen 2 / Docker) but exercises a second process.

**Hard dependencies:**

- [shared-core §1](scoped-tracing-core-plan.md#1--per-thread-hwtrace-state-planned-analysis-phase-c-before-the-managed-bindings)
  (per-thread hwtrace state) — a managed runtime is multithreaded by construction;
  the process-global slot cannot serve it.
- [shared-core §2](scoped-tracing-core-plan.md#2--libipt-decode-against-self-code-image-glue-planned-forward-look-analysis-phase-c)
  (libipt decode-against-self-code-image glue) — the *clean* in-process path for
  live JIT code is PT/LBR + recorder bytes; this slice is its first consumer.
- [bindings slice](../../archive/plans/scoped-tracing-bindings-plan.md) — the `.NET AsmTrace` reference
  construct; this slice adds `.NET`'s managed-code capability on top and builds the
  **Node** and **Java** scope constructs (rated "medium," same tier as .NET).

**Dependency granularity.** The §1/§2 dependency is blanket for the slice but not
uniform per sub-phase: **§D3** (a single-thread ptrace stepper) and **§D4's
host-testable merge test** need only **§1** (per-thread state) — **not** §2's
PT-hardware-gated libipt glue — so the two *CI-protective* deliverables can land right
after §1. Only the clean in-process PT path (§D0–§D2 live capture) is a first consumer
of §2.

---

## Why this tier is different

Pointing **single-step** at live managed code is a documented footgun: it swaps the
process-wide SIGTRAP disposition the runtime's PAL also owns
([src/ss_backend.c:129](../../../../src/ss_backend.c#L129)), can put a sibling runtime
thread into runaway stepping (the handler re-asserts TF in whatever context it
interrupted, [src/ss_backend.c:107](../../../../src/ss_backend.c#L107)), fights the JIT's
relocating/self-modifying bytes, and adds ~100–1000× slowdown on exactly the thread
the GC/JIT coordinate with. So tracing the runtime's own execution requires **either**:

1. **Out-of-band hardware trace** (Intel PT / AMD LBR) + the self code-image
   recorder for bytes — the [shared-core §2](scoped-tracing-core-plan.md#2--libipt-decode-against-self-code-image-glue-planned-forward-look-analysis-phase-c)
   clean path. Privilege- and bare-metal-gated (never most cloud/CI, never macOS).
2. **The concealed out-of-process ptrace stepper** on hosts with no hardware trace
   (Zen 2, Docker-on-Mac) — exact, state-safe, timing-intrusive; a second process
   hidden behind the scope façade.

Both are already half-built in the tree; this slice wires them behind the scope
constructs and adds the async-hop stitching.

---

## The whole-scope vs one-method choice (sets every sub-phase's contract)

The structural fork from the analysis: **hardware trace captures everything cheaply
and filters at decode; a stepper must decide per instruction what to step into.** So
the scope's promise degrades differently by backend:

- **Whole-window ("trace whatever runs in this scope").** Clean under PT (the
  decoder already region-filters, [src/pt_backend.c:108](../../../../src/pt_backend.c#L108));
  under a stepper it is descent-**L3** (`DESCEND_ALL`) — default-off,
  denylist/budget/watchdog-guarded, **expected to self-truncate** on a live runtime
  ([call-descent-plan.md](../../archive/plans/call-descent-plan.md)). Best-effort by design.
- **One JIT'd method ("trace this method's full body").** A region + step-over
  (descent OFF / `DESCEND_KNOWN`) — reliable, and already shipping via the W2
  attached path. It costs the extra knowledge the empty scope avoids: keep the
  method un-inlined + hot, and resolve its address.

This slice ships **both**: the empty-scope whole-window form (PT-preferred,
stepper-best-effort) and the named-method form (`AsmTrace.Method(HotPath)` and its
per-language analogues).

---

## §D0 — .NET managed-code capability (closing the leaks on .NET 8+) *(§D0.1/§D0.2 landed; §D0.3/§D0.4 slated near-term)*

> **Status, by requirement (updated 2026-07-06).** The `AsmTrace` native-leaf reference
> shim ships (see the bindings slice). **§D0.1 landed** as `JitMethodMap` — an in-proc
> `MethodLoadVerbose` `EventListener` labelling the single-step whole-window capture
> (`new AsmTrace(byMethod: true)`). A deliberate divergence from this section's framing:
> the map attributes captured ABSOLUTE addresses **post-hoc** with live-memory
> disassembly rather than feeding `asmtest_codeimage_track` for a PT decode — that seam
> stays forward-look with §Z2/§Z3, and the shipped labelling is version-blind (accepted
> for non-re-tiering windows; the in-process single-step posture itself is an accepted
> decision, see the zero-config plan's §Z1 routing note). **§D0.2 landed**
> dependency-free: `DiagnosticsIpc` hand-rolls the `DOTNET_IPC_V1`
> `EnablePerfMap(JitDump)` wire command instead of taking a `DiagnosticsClient`
> dependency (see [dotnet-perfmap-rundown-plan.md](../../archive/plans/dotnet-perfmap-rundown-plan.md)).
> **§D0.3 named-method form LANDED (2026-07):** `AsmTrace.Method(Delegate)` resolves a
> method's own JIT'd body (PrepareMethod + the `MethodLoadVerbose` listener, jitdump
> rundown for a warm/R2R body), registers it as a region, and traces it with step-over
> — reliable exact offsets where the whole-window form is best-effort. `Invoke(args…)`
> is the library's own `NoInlining|NoOptimization` call site. **§D3 managed increment
> LANDED:** `AsmTrace.Method(..., outOfProcess: true)` routes `Invoke` through
> `asmtest_hwtrace_stealth_trace` so the body is stepped out of band (the calling thread
> is never TF-armed); self-skips where Yama refuses the attach. **§Z3 versioned
> labelling LANDED:** the `byMethod` map feeds a self code-image
> (`asmtest_codeimage_track`) and close-time disassembly decodes against the window-live
> version — so a body that re-tiers/moves after the window still renders what ran.
> **§D0.4 `AsyncLocal` async-hop stitching: still forward-look** (the §D4 merge *core* is
> done + host-tested, but the live per-runtime hook needs bare-metal Intel PT to
> validate the cross-thread merge; deferred). Same shape for the Node (§D1) / JVM (§D2)
> async-hop hooks.

Builds directly on the bindings slice's `AsmTrace` construct. On **.NET 8+** the
analysis shows leak 2 and leak 3 dissolve and leak 1 shrinks to "you must name the
method."

**§D0.1 — `MethodLoadVerbose` address resolution (retires the fragile path).**
`PrepareMethod` + `GetFunctionPointer` stopped being a stable address contract **as of
.NET 7** (the break landed in .NET 7 itself — [dotnet/runtime#83042](https://github.com/dotnet/runtime/issues/83042) —
so it affects .NET 7+): `GetFunctionPointer` returns a precode/indirection stub, not the
JIT'd body start, independent of tiering (which further relocates the body via OSR).
Instead, hang an in-process `EventListener` (the `NativeRuntimeEventSource`
surface) at arm time that consumes `MethodLoadVerbose_V2` under `JITKeyword` (0x10),
carrying `MethodStartAddress` / `MethodSize` / `ReJITID`, and maintain a
name→(address, size, version) map. Feed each (address, size) into the self
code-image recorder (`asmtest_codeimage_track`,
[include/asmtest_codeimage.h:90](../../../../include/asmtest_codeimage.h#L90)) — the byte
source the shared-core §2 decoder reads. New code lives in the .NET binding
(`bindings/dotnet/hwtrace/`); a new `AsmTrace.Method(Delegate|MethodInfo)` static in
[HwTrace.cs](../../../../bindings/dotnet/hwtrace/HwTrace.cs). **Two in-process caveats
(confirmed against the runtime): (i)** an in-proc `EventListener` sees only methods
JIT'd **after** it enables `JITKeyword` — there is **no** in-proc rundown of
already-jitted methods the way an out-of-proc EventPipe session gets one on stop, so the
pre-arm set is §D0.2's `EnablePerfMap` rundown or the self recorder, not this listener;
**(ii)** handling a `MethodLoadVerbose` can itself trigger a JIT (reentrancy), so the
callback must be allocation-light and re-entrancy-safe (do the `codeimage_track` on a
copied `(address, size)`, not inline work that re-enters the JIT).

**§D0.2 — Pre-arm rundown.** Methods JITted *before* arm are covered by a
self-connected `DiagnosticsClient.EnablePerfMap(PerfMapType.JitDump)` (the only
signature is `EnablePerfMap(PerfMapType)` — there is **no** `sendExisting` parameter;
the open question is now **resolved against the runtime**: the coreclr `PerfMap::Enable`
path *does* rundown already-JIT'd methods on enable — it walks loaded R2R assemblies and
the JIT code heap (`CodeHeapIterator` → `LogJITCompiledMethod`,
[coreclr/vm/perfmap.cpp](https://github.com/dotnet/runtime/blob/main/src/coreclr/vm/perfmap.cpp))
before logging new methods forward — so pre-arm methods **are** captured, with the self
recorder as the version-independent fallback. Self-connecting `DiagnosticsClient` to its
own pid works for a lightweight command like this, but there are **known self-tracing
deadlock reports** for heavier self-operations (EventPipe rundown, self-dump — e.g.
dotnet/runtime #45518, where reading rundown events can itself trigger a JIT); the caveat
lives in the issue tracker, not formal docs. `EnablePerfMap` is light, but this is a
reason to keep the self recorder as
the no-IPC path), whose jitdump the shipped `asmtest_jitdump_find`
([include/asmtest_ptrace.h:331](../../../../include/asmtest_ptrace.h#L331)) /
`asmtest_proc_perfmap_symbol` ([:303](../../../../include/asmtest_ptrace.h#L303)) already
read. The design does **not** depend on this for the pure in-process case (the self
recorder needs none of it); it is the lowest-overhead byte source when enableable.

**§D0.3 — Named-method form.** *(LANDED 2026-07 —
[HwTrace.cs](../../../../bindings/dotnet/hwtrace/HwTrace.cs) `AsmTrace.Method`.)*
`using var t = AsmTrace.Method(HotPath); var r = t.Invoke(data); …t.Path;` — `Invoke`
is the library's own call site, pinned `NoInlining | NoOptimization`, so the
standalone JIT'd body is what runs; the arm-time listener resolves the address and
keeps resolving as tiering moves it; decode is against the version live in the
window (temporal-bytes rule). `DOTNET_TieredCompilation=0` and `[MethodImpl(NoInlining)]`
become *noise* preferences, not correctness preconditions — with the profiler-ReJIT
`RequestReJITWithInliners` escape hatch documented but not default.
**As shipped:** `Method(Delegate)` forces the standalone JIT with
`RuntimeHelpers.PrepareMethod`, resolves `(address, size)` from a fresh
`MethodLoadVerbose` listener (a warm/R2R body PrepareMethod can't re-emit is found via
the §D0.2 jitdump-rundown fallback), registers that body as a region, and arms with the
§Z0 auto-init. Resolution or arm failure self-skips (`Armed` false, `SkipReason` set) —
it never throws. The resolved body is the version live at arm time; following a
*mid-scope* tier-up is the §Z3/§D0.4 forward-look, not this form. `outOfProcess: true`
routes `Invoke` through the §D3 stealth stepper (below).

**§D0.4 — `AsyncLocal<ScopeId>` async-hop stitching (the flagship Q1 mechanism).**
This is the analysis's *canonical* enabler for following a logical operation across
`await`/continuation thread hops, and it is the .NET realisation of the shared
[§D4 model](#d4--async-hop-stitching-piece-d-the-shared-logical-operation-model). In
the .NET binding, the hook is an `AsyncLocal<ScopeId>` constructed with a
value-changed handler: the handler fires **when the `AsyncLocal` value changes on a
thread** (including the execution-context restore on a resuming thread), and
`AsyncLocalValueChangedArgs.ThreadContextChanged` is the **bool that distinguishes an
EC-driven change from an explicit `.Value` set** — it is *not* itself the fire trigger.
That EC-restore-on-resume is precisely the "a continuation just resumed here, possibly
on a new thread" signal the raw perf machinery lacks (note the handler does **not** fire
when the value is unchanged across the hop, so the `ScopeId` must actually differ or be
(re)set at the boundary). The handler disables capture when the flow leaves a thread and
re-arms a fresh per-thread PT event on the resuming one; §D4 owns the merge. Default
stays the honest thread-scope-with-mismatch-flag; the stitched mode is an explicit
opt-in.

**§D0 tests.** Extend [examples/dotnet/jit_dotnet/Program.cs](../../../../examples/dotnet/jit_dotnet/Program.cs)
and the `jit_trace` harness ([examples/jit_trace.c](../../../../examples/jit_trace.c)); run
under the existing `docker-hwtrace-jit-dotnet` lane
([mk/docker.mk:222-272](../../../../mk/docker.mk#L222), recipe
[mk/native-trace.mk:261-325](../../../../mk/native-trace.mk#L261)). Assert the listener map
resolves a JIT'd method to (address, size), that a tier-up produces a fresh event at
a new address, and that decode against the version-live bytes matches ground truth.
PT-host lanes self-skip; the ptrace fallback (§D3) runs the exactness check.

---

## §D1 — Node scope construct + async-hop stitching *(planned)*

**Construct.** Node's `region(name, fn)` ships at
[bindings/node/hwtrace.js:439](../../../../bindings/node/hwtrace.js#L439) (on `class HwTrace`,
[:328](../../../../bindings/node/hwtrace.js#L328)). Add a `using`-compatible scope via
`Symbol.dispose` — `using t = new AsmTrace()` — over the same
register-then-`try_begin` / `end` pair, with the shared-core render-on-close and tid
assert. The `using` **declaration** ships unflagged in **Node 24+** (Node 24 GA bundles V8 13.6
and enables Explicit Resource Management itself; upstream it was default-on in the V8
engine at v13.8 / Chrome 138 — Chrome had turned it on earlier at Chromium 134 on V8 13.4,
which is why Node 24 on V8 13.6 still had to flip it manually); on
Node 22 it needs `--harmony-explicit-resource-management`, so gate the sugar on Node 24
and keep the existing `region(name, fn)` callback form as the version-independent
fallback (it needs no `using`). **The `Symbol.dispose` well-known symbol is itself only
native from Node 18.18 / 20.4** (not the whole 18.0 floor), so key the manual-dispose
method off a guarded `const kDispose = Symbol.dispose ?? Symbol.for('nodejs.dispose');`
(Node uses `Symbol.for('nodejs.dispose')` internally) rather than assuming
`Symbol.dispose` exists across the 18 floor. Arm hook is the existing top-level `require` IIFE
([hwtrace.js:172-282](../../../../bindings/node/hwtrace.js#L172)); keep it lazy-first-scope.

**Async-hop stitching (piece D) — weaker for Node than for .NET, by construction.**
Node's per-`ScopeId` hook for the shared
[§D4 model](#d4--async-hop-stitching-piece-d-the-shared-logical-operation-model) is
`AsyncLocalStorage` / `async_hooks` — **neither appears in the binding today**
(verified). But Node's threading model changes what the hook can do: **normal
`await`/Promise/timer continuations resume on the same single JS thread**, so there is
*no OS-thread hop to follow* — the per-thread PT event never moves, and `async_hooks`
`before`/`after` only reconstruct **logical-async ordering on one thread**, not a
cross-thread stitch. The one real OS-thread boundary, `worker_threads`, is exactly where
`AsyncLocalStorage` **does not help**: each Worker has its **own** `async_hooks`/ALS
subsystem and the parent store is **not** propagated across the boundary — so ALS is
**not** the Node analog of .NET's cross-thread-flowing `AsyncLocal` (where thread-pool
continuations genuinely hop OS threads *and* the value flows with them). Net: on Node the
async machinery mostly *orders same-thread async*; genuine cross-thread (Worker /
libuv-pool) work **escapes** and is a **disclosed gap**, flagged via the §0.2 tid assert,
not stitched. Live JS needs PT/LBR or the ptrace stepper (single-step is unsafe against
the V8 runtime). Explicit opt-in; default stays the honest thread-scope with a mismatch
flag.

**§D1 tests.** Extend the `hwtrace-node-test` lane
([mk/native-trace.mk:559-612](../../../../mk/native-trace.mk#L559)) with a `using`-scope
case over a native leaf (works today), and an opt-in async-hop case asserting slices
stitch by `ScopeId` (PT/ptrace host; self-skips otherwise).

---

## §D2 — JVM scope construct *(planned)*

**Construct.** Java's `region(String, Runnable)` ships at
[bindings/java/HwTrace.java:779](../../../../bindings/java/HwTrace.java#L779) on the
`AutoCloseable NativeTrace` ([:758](../../../../bindings/java/HwTrace.java#L758)); add a
try-with-resources `AsmTrace implements AutoCloseable` over register-then-`try_begin`
/ `close`-end, with render-on-close + tid assert. Arm hook is the existing class
`static{}` ([HwTrace.java:279](../../../../bindings/java/HwTrace.java#L279)); keep it
lazy-first-scope. Address resolution for JIT'd methods reuses the **jitdump agent**
(`-agentpath libperf-jvmti.so` for HotSpot) to emit a `jit-<pid>.dump`, read
**in-process** via the shipped `asmtest_jitdump_find`
([src/ptrace_backend.c](../../../../src/ptrace_backend.c)) — the same in-process *reader* the
V8/CoreCLR jitdumps already exercise, feeding the recorder. **Two caveats the "same
reader" framing hides:** (i) `libperf-jvmti.so` is a **Linux perf-tools** artifact
(`tools/perf/jvmti`), not a HotSpot/OpenJDK-shipped agent, and is often absent from distro
packages — an external build dependency §D2 must provision; and (ii) it writes to
`$JITDUMPDIR|$HOME|cwd + /.debug/jit/<mkdtemp-random>/jit-<pid>.dump`, **not** the
`/tmp/jit-<pid>.dump` default `asmtest_jitdump_find` assumes when `path == NULL` — so §D2
must **discover** that randomized directory (glob `$HOME/.debug/jit/*/jit-<pid>.dump`, or
set `JITDUMPDIR` and glob the subdir) and pass the resolved path explicitly. Prefer the
self code-image recorder where possible. (`perf inject --jit` is the
jitdump format's canonical **offline** consumer — it merges a jitdump into a recorded
`perf.data` using the external `perf` binary — and is **not** used here; it would
duplicate the in-process reader and contradict the clean in-process PT thesis.)

**Async-hop.** The JVM's hook for the shared
[§D4 model](#d4--async-hop-stitching-piece-d-the-shared-logical-operation-model) is
**JVMTI** (or bytecode-agent instrumentation of the executor) — `ScopedValue` only
*propagates* a binding across `StructuredTaskScope` forks and has **no** value-changed
callback that fires as the flow moves on/off a thread, so it is **not** the JVM analog
of .NET's `AsyncLocal` value-changed handler; §D4 owns the merge. (`ScopedValue` /
`StructuredTaskScope` are also **preview** APIs at the JDK 22 the binding targets —
`--enable-preview` — a further reason the load-bearing hook is JVMTI/agent, not them.)
This is the **least-proven** of the three per-runtime hooks. Scoped to the opt-in async form, same
posture as Node/.NET. Live managed JIT needs PT/LBR or ptrace; DynamoRIO self-skips
(guarded) and is never used against the runtime.

**§D2 tests.** Extend the `hwtrace-java-test` lane and the
[examples/jit_java/](../../../../examples/jit_java/) fixture under `docker-hwtrace-jit-java`
([mk/docker.mk:222-272](../../../../mk/docker.mk#L222)); assert a JIT'd method resolves and
decodes; PT lanes self-skip, ptrace fallback checks exactness.

---

## §D3 — The concealed out-of-process ptrace stepper scope *(stepper + standalone-binary bundling + package embedding DONE; live-JIT address channel forward-look)*

> **Status: reverse-attach stealth stepper + standalone-binary bundling landed +
> CI-runnable.** `asmtest_hwtrace_stealth_trace`
> ([src/hwtrace.c](../../../../src/hwtrace.c)) reverse-attaches to the caller
> (`prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)` + `PTRACE_SEIZE`), `run_to`s it to the
> region entry, and single-steps the region out of band (reusing
> `asmtest_ptrace_trace_attached`), with a shared shadow trace copied back to the
> caller. The stepping body + bundled-binary discovery now live in
> [src/stealth_helper.c](../../../../src/stealth_helper.c) so the SAME code runs either as an
> in-process forked child (the fallback) or as the **standalone `asmtest-stealth-helper`
> binary** — a real separate process the managed packages can ship — which the caller
> discovers via a dladdr-sibling lookup (mirroring
> [drtrace_app.c](../../../../src/drtrace_app.c)'s `dr_bundled_lib`) or the
> `ASMTEST_STEALTH_HELPER` override and hands the shared scratch over a memfd
> (survives `execv`, unlike the anonymous mapping the fork path inherits).
> `test_ptrace_scoped_stealth` (checks 158–166) asserts **both** paths reconstruct the
> identical `[0,3,6,c,11]` offsets on any ptrace-capable Linux, self-skips where Yama
> refuses the attach, and an `alarm()` watchdog means it never hangs CI. Build/install:
> the `$(BUILD)/asmtest-stealth-helper` target + `install-stealth-helper` (next to
> `libasmtest_hwtrace`, where the sibling lookup resolves), in
> [mk/native-trace.mk](../../../../mk/native-trace.mk). **Package embedding is now also done:**
> the helper is staged into every native payload slot beside `libasmtest_hwtrace`,
> `$ORIGIN`-rpath'd so it resolves the co-vendored Capstone in-package
> ([scripts/package-native.sh](../../../../scripts/package-native.sh)), copied into every
> managed payload (NuGet `runtimes/<rid>/native`, npm/Maven/gem/rock via `emu_lib_slots`,
> the Python wheel `_libs/`), and asserted present + `$ORIGIN`-rpath'd (and not leaked
> into a darwin slot) by a fail-closed `package-libs-verify` gate
> ([mk/bindings.mk](../../../../mk/bindings.mk)). Clean-room-verified on this Linux host: a
> full `package-libs` slot passes the complete-set gate, `ldd asmtest-stealth-helper`
> resolves the in-slot `libcapstone.so.5`, and a `dlopen` of the bundled
> `libasmtest_hwtrace.so` discovers the helper as its dladdr sibling with the environment
> scrubbed. **The cross-process address channel LANDED (2026-07):** a header-only SPSC
> ring ([include/asmtest_addr_channel.h](../../../../include/asmtest_addr_channel.h)) the parent
> publishes each JIT'd method's `(base, len, version)` into and the stepper drains between
> steps, plus `asmtest_ptrace_trace_attached_windowed`
> ([src/ptrace_backend.c](../../../../src/ptrace_backend.c)) — a whole-window multi-region
> capture that single-steps an attached tracee across a window frame and records the
> ABSOLUTE address of every instruction in the frame OR any channel-published region,
> following calls into published methods and eliding the runtime noise between them.
> `test_ptrace_windowed` ([examples/test_hwtrace.c](../../../../examples/test_hwtrace.c)) proves
> the cross-process handoff with a driver blob calling two channel-published leaves — the
> stepper records both without knowing their addresses at fork time. **Forward-look:** the
> .NET-binding composition that publishes from the `JitMethodMap` listener into a shared
> channel and drives the windowed helper end-to-end against a live CoreCLR (needs a live
> managed runtime to validate the noisy whole-window path).

The hardware-free path, hidden behind the same scope constructs, for hosts with no
PT/LBR (Zen 2 — [src/hwtrace.c:183](../../../../src/hwtrace.c#L183) classifies it
no-hardware — and Docker-on-Intel-Mac). The W2 tracer already exists
(`src/ptrace_backend.c`, `asmtest_ptrace_trace_attached_versioned`, paired with the
self code-image recorder) and touches none of the target's signals, code bytes, or
memory protections — state-safe on every axis except timing (~1000× on the stepped
thread while siblings run native).

**Concealment.** The empty constructor spawns a small **bundled tracer helper**,
`prctl(PR_SET_PTRACER, helper)` so a Yama `ptrace_scope=1` host permits the attach (the
helper is the caller's *child*, so the nomination is what makes the reverse attach
legal), the helper `PTRACE_SEIZE`s the calling thread and steps it from a sentinel at
the scope open to a sentinel at the scope close; `Dispose()`/`close()` joins the helper
and renders via the **version-aware** render variant `asmtest_hwtrace_render_versioned`
(pinned in [Core §1](scoped-tracing-core-plan.md#1--per-thread-hwtrace-state-planned-analysis-phase-c-before-the-managed-bindings);
§0.3's `render` is region-scoped and version-blind, but the stepped managed bytes
tier/move, so the plain primitive would render stale text). Developer footprint stays **import + scope**. Works on Zen 2 and
inside Docker on an Intel Mac — the default seccomp profile permits `ptrace(2)` when the
**host kernel is ≥ 4.8**; older kernels (or a stripped profile) need `CAP_SYS_PTRACE`.

**Byte source (foreign, not self).** Because the helper is a *separate process*
attached to the parent, its recorder reads the **parent's** JIT bytes — the
**foreign-pid** code-image recorder (`asmtest_codeimage_new(parent_pid)` +
`process_vm_readv`/soft-dirty), **not** the `pid == 0` self recorder. The parent's
`MethodLoadVerbose` listener (§D0.1) is the only party that knows the JIT method
addresses, so the helper needs a **cross-process channel** (or a shared/serialized
recorder) to learn them. This is the load-bearing new work the "the tracer exists"
framing understates.

**Contract.** Whole-scope capture rides descent-L3 (best-effort, self-truncating);
method-scoped capture is reliable. Non-intrusive to *state*, intrusive to *timing* —
the scoped thread crawls while siblings run free, so locks it holds stall them (the
perturbation, not deadlock, form of the L3 hazard). Document this prominently.

**§D3 deliverables.**

- A bundled helper binary + spawn/`PR_SET_PTRACER`/`PTRACE_SEIZE`/sentinel-step glue,
  reusing `asmtest_ptrace_trace_attached_versioned` against the **foreign-pid**
  recorder (above). This is the
  [zen2-singlestep-trace-plan.md](../../plans/zen2-singlestep-trace-plan.md) W2 machinery in a
  scope wrapper — no new *tracer*, but the reverse-attach protocol (`PR_SET_PTRACER` +
  `PTRACE_SEIZE` of the parent), the sentinels, and the cross-process address channel
  **are** new.
- The helper's **build target + install path** in
  [mk/native-trace.mk](../../../../mk/native-trace.mk) (reusing `ptrace_backend.o`), its
  **runtime discovery** (dladdr-on-own-symbol sibling lookup, matching the DynamoRIO
  payload's [src/drtrace_app.c](../../../../src/drtrace_app.c) pattern), and its **bundling**
  into each managed package (NuGet / npm / Maven) — the packaging + supply-chain surface
  the risks list flags.
- Scope-façade wiring in each managed binding so `new AsmTrace()` on a no-hardware
  host silently routes to the helper instead of self-skipping.

**§D3 tests.** A new C harness case in
[examples/test_hwtrace.c](../../../../examples/test_hwtrace.c) (`test_ptrace_scoped_stealth`)
alongside the existing `test_ptrace_versioned`
([:1153](../../../../examples/test_hwtrace.c#L1153)) / `test_ptrace_attach`
([:852](../../../../examples/test_hwtrace.c#L852)): spawn the helper, SEIZE self, step
between sentinels, assert exact offsets vs `asmtest_disas` ground truth. Runs on any
ptrace-capable Linux (the Zen 2 / Docker target class) — **not** hardware-gated, so
it is CI-runnable in the `hwtrace` job / a Docker lane with `--cap-add=SYS_PTRACE`
(the `docker-hwtrace-codeimage` lane already runs with that cap,
[mk/docker.mk:294-301](../../../../mk/docker.mk#L294)).

---

## §D4 — Async-hop stitching (piece D): the shared logical-operation model

> **Status: shared merge core landed (host-testable half).** The slice/bound ABI
> (`asmtest_hwtrace_slice_t`, `asmtest_hwtrace_slice_bound_t`) and the pure ordered
> merge `asmtest_hwtrace_stitch` are implemented in
> [src/hwtrace.c](../../../../src/hwtrace.c) / declared in
> [include/asmtest_hwtrace.h](../../../../include/asmtest_hwtrace.h); `test_stitch_slices`
> (checks 6–10) passes with no PT hardware and no real threads — the CI-runnable
> deliverable that closes the async-hop merge test hole. **Forward-look:** the
> per-runtime value-changed hooks (.NET `AsyncLocal` / Node `AsyncLocalStorage` / JVM
> JVMTI) that drive live capture→tag→merge remain a disclosed gap (a PT/ptrace host).

*(planned; explicit opt-in — the deepest piece in the set.)* This is the analysis's
Q1 redesign: the one qualification that is **not a knob but a model change** — the
scope stops being a *thread* window and becomes a **stitched trace of one logical
operation**. It is factored out here because .NET (§D0.4), Node (§D1), and the JVM
(§D2) share the *same* merge core and differ only in the per-runtime hook that
signals a thread hop.

**The per-runtime hook (differs) — fires when the logical flow moves on/off a thread:**

| Runtime | Hook |
|---|---|
| .NET | `AsyncLocal<ScopeId>` value-changed (`AsyncLocalValueChangedArgs.ThreadContextChanged`) |
| Node | `AsyncLocalStorage` / `async_hooks` `init`/`before`/`after` (orders **same-thread** async only; does **not** propagate across `worker_threads`, so genuine OS-thread hops escape — see §D1) |
| JVM | JVMTI callback / executor bytecode-agent instrumentation (`ScopedValue` only *propagates* context — not a value-changed hop signal); least-proven |

**The merge core (shared) — the concrete algorithm:**

1. **Open.** Allocate a monotonic `ScopeId`; open a per-thread PT event (the Core §1
   per-thread state) on the calling thread; tag its state slot with `(ScopeId,
   seq=0)` and the start version from the code-image recorder
   (`asmtest_codeimage_now`, [include/asmtest_codeimage.h:102](../../../../include/asmtest_codeimage.h#L102)).
2. **Leaving thread A** (hook fires): `PERF_EVENT_IOC_DISABLE` A's event; park A's
   slice keyed `(ScopeId, seq, tid=A, start-version)`.
3. **Resuming on thread B** (hook fires): open a **fresh** per-thread PT event (own
   AUX ring) on B, enable it, tag `(ScopeId, seq+1)`. PT per-thread mode has **no
   inheritance**, so following a hop is exactly "open a new per-thread event on the
   resuming thread" — and it stays per-thread, needing **no privilege bump**. (The
   per-CPU alternative `pid=-1, cpu=N` also captures hops but needs
   `CAP_PERFMON` (Linux 5.8+; or the broader `CAP_SYS_ADMIN`) **or**
   `perf_event_paranoid < 1`, plus TID demux from `ITRACE_START`/sched records —
   rejected as heavier.)
4. **Close.** Collect all slices for `ScopeId` and order by `seq`. Each slice was
   **already decoded** against the code-image version live *in its own window* at the
   moment it was disabled (the temporal-bytes rule, Core §2's recorder-backed
   callback), so step 4 is a pure ordered **concatenation** — no re-decode — into one
   instruction stream, with slice boundaries (which thread, which version) returned in
   the companion `bounds` array. Decoding at disable time, not at close, is what keeps
   the merge host-testable from synthetic pre-decoded slices.

**Native substrate (new, shared).** A language-agnostic C helper in `src/hwtrace.c`
performs step 4's ordered merge over slices the per-thread state (Core §1) accumulates;
the per-language hook only does arm/disarm + `ScopeId` tagging. The Core §1 per-thread
state struct gains a `ScopeId` + `seq` field so slices are attributable at merge. The
slice ABI and merge signature are **pinned here** (the flagship CI-runnable deliverable
depends on both being concrete):

```c
typedef struct {
    uint64_t         scope_id;   /* logical-operation id */
    uint32_t         seq;        /* hop order within the scope */
    int              tid;        /* thread the slice ran on */
    uint64_t         version;    /* code-image version live in this window (asmtest_codeimage_now) */
    asmtest_trace_t  trace;      /* offsets ALREADY decoded against `version` at disable time */
} asmtest_hwtrace_slice_t;

typedef struct {
    size_t   insn_off;           /* offset into `out` where this slice begins */
    uint64_t scope_id;
    uint32_t seq;
    int      tid;
    uint64_t version;
} asmtest_hwtrace_slice_bound_t;

int asmtest_hwtrace_stitch(const asmtest_hwtrace_slice_t *slices, size_t n,
                           asmtest_trace_t *out,
                           asmtest_hwtrace_slice_bound_t *bounds, size_t *nbounds);
```

**`stitch` concatenates; it does not decode.** Each slice is decoded against its own
`version` *at the moment it is disabled* (Core §2's recorder-backed callback), so by the
time it reaches `stitch` it already carries offsets. `stitch` is then a pure ordered
merge by `seq` — which is exactly what makes `test_stitch_slices` host-testable from
synthetic pre-decoded slices with no PT hardware and no real threads. Because
`asmtest_trace_t` ([include/asmtest_trace.h:44](../../../../include/asmtest_trace.h#L44)) has
**no** field for per-slice boundaries, the "(which thread, which version)" annotations
the contract promises are returned in the companion `bounds` array (one entry per
slice), **not** smuggled into `out`.

**Multi-slice output contract.** The scope's result becomes a *list of thread-window
slices for one logical operation*; the rendered `Path` concatenates them in `seq`
order. The **synchronous single-slice case is the degenerate one** — identical
output to today, so non-async callers see no change. The developer still writes only
the scope; the multi-slice contract is internal until they read `Path`.

**Backends.** PT only (clean, per-thread, no privilege bump). **Single-step is
forbidden here** — arming TF on runtime-scheduled threads deepens the
SIGTRAP-ownership collision (the analysis: "buildable, not advisable"). The §D3
ptrace stepper follows a **single** thread between sentinels, so it **cannot**
exercise cross-thread stitching — which is exactly why the merge is validated by a
host-testable unit test, not the ptrace lane.

**§D4 tests.**

- `test_stitch_slices` (**host-testable, CI-runnable — closes the async-hop test
  hole**) — construct synthetic `asmtest_hwtrace_slice_t` values (offset lists with
  monotonic `seq` + distinct `version`s), call `asmtest_hwtrace_stitch`, assert the
  concatenated ordered stream equals the expected logical-operation sequence and that
  the `bounds` array attributes each run to its `(tid, version)`. No PT hardware, no
  real threads — it validates the merge algorithm itself, mirroring the §2
  reconstruction-half posture. Lives in [examples/test_hwtrace.c](../../../../examples/test_hwtrace.c).
- Per-runtime opt-in async case (.NET/Node/JVM) that drives a real `await`/continuation
  hop and asserts slices stitch by `ScopeId` — **self-skips** off a PT/ptrace host
  (the honest known validation gap: the live hook has no CI coverage; the merge core
  does).

**§D4 effort.** The merge helper + host-testable unit test ~2–3 days; the per-runtime
hooks ~2–4 days each (folded into §D0.4/§D1/§D2). Live end-to-end validation is
forward-look on PT hardware.

**§D4 risk.** This is the real engineering of the whole plan set — a model change,
gated behind an explicit opt-in, and it must **never** emit a partial (un-stitched)
trace as complete; the Core §0.2 arming-thread assert is the backstop that flags any
unhandled hop as `truncated`. **Coverage honesty:** the hook-side `ScopeId`/`seq`/
`version` *tagging* is exercised by **neither** CI-protective test — `test_stitch_slices`
builds correct tags itself, and §D3 is single-thread — so the live capture→tag→merge
chain ships with no CI coverage. Add a **fake-hook harness** that drives the merge from
a scripted hop sequence (not pre-tagged slices) to cover the hook→merge seam; the live
per-runtime hook remains a disclosed forward-look gap.

**§D4 cost (feasibility, disclose in docs).** Decoding each slice *at disable time*
(what keeps `stitch` a pure host-testable merge) puts a PT drain + libipt decode **and** a
fresh `perf_event_open` + AUX-ring `mmap` on the resuming thread **on every hop** — i.e.
inside the `AsyncLocal` value-changed handler, which on .NET fires on **every** EC restore
where the `ScopeId` differs. A workload that awaits in a tight loop would pay that per
continuation. This is why the stitched mode is **explicit opt-in**, not the default, and
why the default stays the honest thread-scope-with-mismatch-flag; the docs must set the
"stitching extends the *window*, not the bandwidth economics" expectation (same posture as
the Core §3 drain caveat). Mitigations to evaluate: reuse a per-thread event across hops on
the same thread rather than re-opening, and batch-decode at close where the AUX ring is
still intact (trading the host-testable-at-disable property for lower per-hop cost).

---

## Docs

- **Managed-tier guide** — a new section in
  [docs/guides/tracing/hardware-tracing.md](../../../guides/tracing/hardware-tracing.md)
  (or the scoped-tracing guide from the bindings slice) on tracing live managed code:
  the PT/LBR-vs-ptrace fork, why single-step is forbidden here, the whole-scope-vs-
  method contract, and the §D4 async-hop model — its explicit opt-in and the
  multi-slice output contract (a logical operation returns an ordered set of
  per-thread slices, not one thread window).
- **.NET page** — extend [docs/bindings/dotnet.md](../../../bindings/dotnet.md) with the
  `AsmTrace.Method(HotPath)` named form, the .NET-8 leak-closing (events/rundown),
  and the tiering/inlining notes.
- **Node / Java pages** — [docs/bindings/node.md](../../../bindings/node.md) (the
  `using`+`Symbol.dispose` scope, `AsyncLocalStorage` opt-in) and
  [docs/bindings/java.md](../../../bindings/java.md) (try-with-resources, JVMTI/`ScopedValue`
  opt-in).
- **Reference** — [docs/reference/features.md](../../../reference/features.md) (managed-tier
  matrix rows), [docs/reference/portability.md](../../../reference/portability.md) (Zen 2 /
  Docker-on-Mac / macOS degradation), and
  [docs/internal/analysis/trace-parity-matrix.md](../../analysis/trace-parity-matrix.md) (managed
  decode parity status).

---

## Build & CI

- .NET/Node/Java scope + capability code is source-only in the existing binding
  trees; no new shared lib. The §D4 `asmtest_hwtrace_stitch` merge helper compiles
  into the existing `hwtrace.o`; the ptrace helper (§D3) is a new small binary built
  alongside the native-trace objects in
  [mk/native-trace.mk](../../../../mk/native-trace.mk) (reusing `ptrace_backend.o`,
  [:206-211](../../../../mk/native-trace.mk#L206)).
- CI: the managed lanes run under the existing `docker-hwtrace-jit*` targets
  ([mk/docker.mk:222-272](../../../../mk/docker.mk#L222)). **Two parts gate on ordinary CI**
  (the real automated protection in this slice): the §D3 ptrace-stealth test (the
  `hwtrace` job or a `--cap-add=SYS_PTRACE` Docker lane, no PT hardware) and the §D4
  `test_stitch_slices` host-testable merge unit test (`make hwtrace-test`,
  [.github/workflows/ci.yml:247](../../../../.github/workflows/ci.yml)). The PT clean-path
  lanes and the live per-runtime async-hop cases self-skip off bare-metal Intel PT /
  a PT-or-ptrace host, as everywhere.

---

## Effort & risks

**Effort.** §D0 (.NET events + named form + `AsyncLocal` hook) ~5–7 days; §D1 (Node
`using` + `AsyncLocalStorage` hook) ~4–6 days; §D2 (JVM) ~4–6 days; §D3
(ptrace-stealth scope) ~4–6 days for the tracer-plumbing (the reverse-attach protocol —
`PR_SET_PTRACER` + `PTRACE_SEIZE` of the parent — the sentinels, and the cross-process
address channel; the single-step *stepper* exists, but this protocol is new),
**plus a separately-budgeted ~3–5 days for the per-ecosystem bundling/packaging +
install/discovery** (NuGet / npm / Maven — the supply-chain surface the risks list flags);
§D4 (shared `asmtest_hwtrace_stitch` merge core + host-testable unit test) ~2–3 days,
the per-runtime hooks folded into §D0/§D1/§D2. The clean PT path's live validation and
the live per-runtime async-hop cases are forward-look, gated on PT hardware.

**Risks.**

- **The async-hop redesign is a model change, not a knob** — "thread window" becomes
  "stitched trace of a logical operation." It is the deep engineering here; keep it
  behind an explicit opt-in and never let a hop silently emit a partial trace as
  complete (the shared-core §0.2 tid assert is the backstop).
- **Managed single-step is forbidden** — the slice must route managed-code capture to
  PT/LBR or the §D3 ptrace helper, never `src/ss_backend.c` against the runtime.
- **The clean PT path needs bare-metal Intel PT to validate** (this dev host is AMD).
  It ships self-skipping; the §D3 ptrace path is the exactness check that *does* run
  on the Zen 2 / Docker target class.
- **Address resolution is runtime-version-fragile off .NET 8 / older JVMs.** The
  self code-image recorder is the version-independent fallback; the event/jitdump
  paths are optimisations, not correctness dependencies.
- **The ptrace helper is a second process + a bundled binary** — a packaging and
  supply-chain surface (mirrors the bundled DynamoRIO/hwtrace payload the project
  already ships and clean-room-verifies).

## Sources

The managed-tier analysis (JIT-hostility, the whole-scope-vs-method fork, closing
the leaks on .NET 8+, the concealed ptrace path, and piece-D async-hop stitching):
[the scoped `using` analysis](../../analysis/scoped-inprocess-tracing.md#closing-the-leaks-on-net-8)
and its
[case-by-case](../../analysis/scoped-inprocess-tracing.md#the-scoped-model-case-by-case)
and
[four-qualifications](../../analysis/scoped-inprocess-tracing.md#qualification-1--the-threadasync-boundary-the-deep-one)
sections. Foreign-JIT and W2 background:
[jit-runtime-tracing.md](../../analysis/jit-runtime-tracing.md),
[zen2-singlestep-trace-plan.md](../../plans/zen2-singlestep-trace-plan.md),
[hardware-trace-plan.md](../../plans/hardware-trace-plan.md).
