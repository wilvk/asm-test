# Analysis: non-intrusive in-process managed-runtime tracing — the scoped `using` model

*Status: analysis / findings. This document records a research investigation, not
shipped behaviour. It is the in-process, **cooperative** sibling of
[jit-runtime-tracing.md](jit-runtime-tracing.md): that document asks how to attach
to a **foreign** JIT from outside and reconstruct its generated assembly; this one
asks the narrower, friendlier question a developer actually types — can I wrap a
region of my own managed code in a `using` block and get the assembly that ran,
without perturbing the runtime, and without writing anything beyond the import and
the block? The shipped surface it leans on is the hardware-trace tier
([include/asmtest_hwtrace.h](../../include/asmtest_hwtrace.h), `src/hwtrace.c`), the
time-aware code-image recorder ([include/asmtest_codeimage.h](../../include/asmtest_codeimage.h),
`src/codeimage.c`), and the .NET binding
([bindings/dotnet/hwtrace/HwTrace.cs](../../bindings/dotnet/hwtrace/HwTrace.cs)).*

## Question

Can a managed runtime (.NET here, but the shape generalises) be traced
**non-intrusively** by bracketing a region with a `using` statement and capturing the
**assembly-language path** executed inside it? And can the developer-visible code be
reduced to the **package import plus the `using` statement** — ideally an **empty
constructor**, assuming no knowledge and handling everything?

```csharp
using AsmTest;                 // (1) the package import

using (new AsmTrace())         // (2) the using statement — empty ctor, no config
{
    HotPath(data);             // whatever runs here: its real JIT'd asm is captured
}
```

That is the whole ask. No launch flags, no `perf record`, no external harness, no
region name, no read-back call. The scope opens, the code runs at native speed, the
scope closes, and the assembly that executed appears.

## Determination

**Yes — and most of the mechanism already ships. The scoped model is the natural fit,
because a `using` block is exactly an enable/disable window, and asm-test already
brackets a region with `begin`/`end` markers that do in-process, self-attached,
per-thread capture.** The honest qualifications are three host preconditions that no
amount of API design can turn into code — it can only *hide* them and self-skip when
they are absent (which is precisely this codebase's posture everywhere else).

1. **"Non-intrusive" narrows you to hardware trace.** Intel PT (x86-64), AMD LBR, and
   ARM CoreSight observe **out-of-band**: the CPU records branch outcomes to a ring
   buffer with no code patching, no `SIGTRAP`, no W^X page-fault thrash — so they do
   **not** collide with the runtime's own JIT/GC/signal machinery (the collision that
   makes in-process DBI and single-step hostile to managed runtimes — see
   [jit-runtime-tracing.md](jit-runtime-tracing.md#why-hardware-trace-is-the-way-around-jits)).
   The universal in-process fallback, **EFLAGS.TF single-step**, is *exact* but the
   opposite of non-intrusive: a `#DB`/`SIGTRAP` per instruction, on the traced thread,
   colliding with the very machinery you are trying not to perturb. So *non-intrusive
   **and** in-process* means PT/LBR/CoreSight hardware.

2. **"Only import + using" is achievable in the code, but rests on privilege and
   hardware the code cannot grant itself.** `perf_event_open` for PT needs
   `perf_event_paranoid` lowered (`< 0` for a full-size buffer; unprivileged callers
   get a small 128 KiB AUX ring) or `CAP_PERFMON`; Intel PT needs an **Intel
   bare-metal** host (absent on AMD — this dev host is AMD — on ARM, and on nearly all
   cloud/CI VMs). The import can *detect* this (the shipped `available()` /
   `skip_reason()` self-probe) and degrade the scope to a no-op that records why, so the
   code still compiles and runs everywhere. It cannot make an AMD laptop grow Intel PT
   or elevate its own capabilities.

3. **The temporal-bytes problem still applies — but in-process it is much easier.** PT
   records control flow, **not** instruction bytes; the decoder must be handed the JIT's
   bytes that were live *during the window*. For a **foreign** JIT that is the hard part
   ([jit-runtime-tracing.md](jit-runtime-tracing.md#the-one-hard-problem--and-it-is-temporal));
   in-process, the tracing thread can read its **own** JIT memory, and the shipped
   code-image recorder already supports `pid == 0` (self) — snapshot-on-change of your
   own executable pages, no runtime cooperation, no launch flag
   ([include/asmtest_codeimage.h:76](../../include/asmtest_codeimage.h#L76)).

Net: on an Intel bare-metal host with a one-time privilege grant, the developer-visible
footprint genuinely **is** the import plus the `using` block. Everywhere else the same
code keeps compiling and self-skips (no PT), or you consciously accept intrusiveness
(single-step) or a separate tracer process (out-of-process ptrace — extra moving part,
though still not extra code *in the traced app*).

## What "non-intrusive" means here

The word carries the whole argument, so define it: **observing the program without
changing what the program does** — the traced code executes the same instructions, at
the same addresses, with the same signal/memory/thread machinery, as it would
unobserved; the only permitted effect is a small, bounded slowdown. That unpacks into
five axes, each one a specific way a tracer *can* intrude on a managed runtime:

1. **No code modification.** The JIT's emitted bytes execute unmodified at their
   original addresses — no `int3` patched in, no trampolines, no DBI recompilation into
   a software code cache. (DynamoRIO/Pin/Frida fail this axis; it is why they collide
   with JITs.)
2. **No signal/exception hijacking.** The tracer installs no handlers in the target.
   Managed runtimes *use* SIGSEGV for null checks, safepoints, and GC barriers — and
   CoreCLR's PAL registers its own SIGTRAP handler (`Debugger.Break` raises it) — so a
   tracer that owns those signals is fighting the runtime for them.
3. **No thread perturbation.** No stopping, single-stepping, or slowing one thread
   relative to its siblings — the lock-stall / lock-inversion hazard the
   [L3 descent analysis](jit-runtime-tracing.md#when-to-use-l3-call-descent--and-why-it-is-hazardous-on-a-live-runtime)
   documents.
4. **No memory-protection interference.** No toggling page permissions on the JIT's
   code heap (the W^X fault-thrash DBI needs to keep its cache coherent).
5. **Bounded timing overhead.** Small enough (~2–20 % for PT, zero when disabled) that
   timing-derived behaviour — tiering decisions, GC scheduling, race interleavings — is
   essentially preserved.

There is a sixth, softer axis: **deployment intrusiveness** — whether observing requires
changing *how the program is launched*. `DOTNET_PerfMapEnabled` is the canonical example:
it barely perturbs execution, but the env var is read only at process start — you must
restart with the flag set (.NET 8 added a runtime IPC toggle for the same facility; see
the jitdump leak below). The `using` model aims to be non-intrusive on this axis too:
attach nothing, restart nothing, cost confined to the scope.

Only **out-of-band hardware trace** passes all five execution axes, because the recording
happens in the CPU as a side effect of executing — the program cannot tell it is being
traced short of probing MSRs. One honest caveat: the `using` block itself is code *in*
the target, so the cooperative model is by definition not zero-footprint — but its
intrusion is confined to the scope boundaries (today's `begin` opens the perf event and
maps its rings each time, [src/hwtrace.c:671-704](../../src/hwtrace.c#L671-L704); with
the fd held open by an import-time initializer that steady state shrinks to the
enable/disable `ioctl` pair); the region *body* runs untouched at native speed. That is
the precise sense in which this document claims the model is non-intrusive.

## Why the `using` block is the right shape

A `using` statement is a scope with a guaranteed `Dispose()` at the closing brace —
i.e. a **balanced enable/disable pair**, which is exactly the hardware-trace region
model asm-test already ships:

| `using` lifecycle | Native mechanism (already implemented) |
|---|---|
| `new AsmTrace()` (ctor) | `asmtest_hwtrace_begin(name)` → `PERF_EVENT_IOC_ENABLE` on a per-thread self event ([src/hwtrace.c:457](../../src/hwtrace.c#L457), [:704](../../src/hwtrace.c#L704)) |
| block body runs | CPU streams PT/ETM/LBR packets to the AUX ring out-of-band; the JIT compiles and runs untouched |
| `Dispose()` (closing brace) | `asmtest_hwtrace_end(name)` → `PERF_EVENT_IOC_DISABLE` then decode the packets against the code image and render via Capstone ([src/hwtrace.c:462](../../src/hwtrace.c#L462), [:780](../../src/hwtrace.c#L780)) |

The perf event is opened **on the calling process itself** — `perf_open(&a, 0, -1, -1, 0)`,
i.e. `pid == 0` (this thread) / `cpu == -1` (follow it across CPUs)
([src/hwtrace.c:438](../../src/hwtrace.c#L438)) — so this is in-process self-tracing, not
a foreign attach. The `try/finally` that keeps `begin`/`end` balanced even on an
exception is already how the .NET binding's `HwTrace.Region(name, Action)` works
([bindings/dotnet/hwtrace/HwTrace.cs:602](../../bindings/dotnet/hwtrace/HwTrace.cs#L602));
turning that `Action` form into an `IDisposable` scope is a few lines, and the
`IDisposable` idiom is already pervasive in the bindings (`Regs`, `Emu`, `Trace`,
`Descent`, `EmuResult`, …).

## How an *empty* constructor "handles everything"

The zero-argument, zero-knowledge form is the *easiest* case, not the hardest, because
a scope that traces **whatever runs inside it** needs no method address at all — it just
brackets a capture window. Each responsibility maps to something that already exists or
is a thin shim:

- **Arm before first use, with no setup call.** A `[ModuleInitializer]` is guaranteed
  to run before anything in the package's assembly is touched — in practice when the
  first method mentioning an `AsmTest` type is itself JITted, since assemblies load
  lazily and a `using` *directive* alone emits nothing at runtime. So arming (probe
  availability, pick a backend, bring the tier up, wire the self code-image recorder)
  happens just before the first `new AsmTrace()`, and the developer still writes no
  setup — "arm on import" is the ergonomic effect, not the literal timing.
  (`[ModuleInitializer]` + `[CallerMemberName]`/`[CallerFilePath]`/`[CallerLineNumber]`
  are compiler-supplied C# features; the caller types nothing.)
- **Auto-name the region.** With no argument, `[CallerMemberName]`/`[CallerLineNumber]`
  give the scope a free, unique label — no name to invent.
- **Auto-select the backend.** `asmtest_hwtrace_auto` / `asmtest_trace_auto` already
  resolve the most-faithful available backend (Intel PT → AMD LBR → single-step →
  CoreSight → emulator) and self-skip when none is usable
  ([include/asmtest_hwtrace.h:111-120](../../include/asmtest_hwtrace.h#L111-L120);
  .NET `HwTrace.Auto` / `AutoTier`).
- **Supply the bytes with no cooperation.** The self code-image recorder (`pid == 0`)
  snapshots the process's own JIT pages on change; the PT decoder is handed the version
  live at each trace position. This is already the byte source for the out-of-process
  stepper's `_versioned` path — the in-process case reuses the same recorder pointed at
  self.
- **Emit with no read-back call.** Because the empty form captures no variable, the
  default "assume no knowledge" behaviour is to render the decoded path on `Dispose()`
  to a default sink (stdout, or `asmtrace-<member>.txt`) via the already-wrapped Capstone
  layer (`emu_disas`, `src/disasm.c`). A developer who *does* want the data reads it off
  the handle instead: `using var t = new AsmTrace(); … ; t.Path`.

So the "handle everything" promise is real for the **dynamic-path** interpretation
("show me the asm of whatever executed in this scope"). It only leaks when the developer
means something more specific (below).

## What can go inside the scope

Can the block body be *any valid C#*? Under the strong (hardware-trace) form, **yes —
because the capture does not understand C# at all.** The CPU records whatever
instructions the thread executes between `{` and `}`; there is no allowlist and no
interpretation of what the code *is*. Reflection-emitted code, `dynamic`, lambdas,
generics, exceptions and their unwinding, P/Invoke into native libraries, unsafe code —
all of it is just executed instructions, and the decoder renders whatever ran. That
language-blindness is *why* the whole-window model can assume no knowledge.

Four qualifications shape what comes back — the first is a real semantic trap:

1. **The scope is a *thread* scope, not a logical-operation scope.** The perf event
   follows the calling thread (`pid == 0, cpu == -1`). Work that hops threads —
   `await`, `Task.Run`, `Parallel.For`, thread-pool continuations — is **not captured**,
   and an `await` inside the block can resume on a *different* thread, so `Dispose()`
   runs somewhere other than where the constructor armed the event: the window silently
   keeps recording thread A while the logical operation continues on thread B. Any valid
   *synchronous* code is fully covered; async code contributes only the fragments that
   happen to run on the original thread. A production `AsmTrace` should compare
   `Environment.CurrentManagedThreadId` at `Dispose()` against the constructor's and
   flag the trace on a mismatch — the same never-emit-partial-as-complete posture as
   `truncated`.
2. **You get everything that ran — including the runtime itself.** If the code inside
   is not warm, the first call traces the *JIT compiling it* (thousands of RyuJIT
   instructions), then the tier-0 body, then perhaps an OSR transition; a GC triggered
   mid-scope is captured too. That is honest — it *is* the assembly path that executed —
   but noisy. The warm/`[MethodImpl(NoInlining)]`/tiering-pinned discipline in
   [examples/jit_dotnet/Program.cs](../../examples/jit_dotnet/Program.cs) exists
   precisely to get a *clean* single-method trace instead of the runtime's plumbing.
3. **The window has a bandwidth budget, not a semantic limit.** PT emits hundreds of
   MB/s per core encoded ([perf-intel-pt][perf-intel-pt]); a scope around seconds of hot
   code overflows the AUX ring or produces gigabytes (drain mode). The `snapshot` option
   maps a circular ring for exactly this (keep the tail, flag `truncated`), but its
   decode-side drain is a named follow-up — `end()` today decodes the linear ring only
   ([src/hwtrace.c:782-783](../../src/hwtrace.c#L782-L783)). Long-running
   or I/O-blocked code is still *valid* — a thread parked in a syscall emits nothing —
   but a huge dynamic window decodes to a huge trace.
4. **Scopes do not nest (today).** Capture state is a single process-global slot — "only
   ONE region may be active at a time (a begin while another is active is ignored)"
   ([include/asmtest_hwtrace.h:144-146](../../include/asmtest_hwtrace.h#L144-L146)) — so
   an `AsmTrace` inside an `AsmTrace`, or two threads scoping concurrently, degrades to
   the outer/first one. Lifting this means per-thread capture slots, a known MVP
   boundary.

Under the **single-step fallback** the answer flips to **no** — the envelope narrows
sharply. The scope body should be effectively single-threaded, non-blocking, and
allocation-light, because whole-scope stepping is descent-L3 territory: allocation
slow paths, contended locks, and blocking syscalls are the documented hazards
(denylist / budget / watchdog, self-truncating —
[jit-runtime-tracing.md, L3 section](jit-runtime-tracing.md#when-to-use-l3-call-descent--and-why-it-is-hazardous-on-a-live-runtime)).
"Any valid C#" is exactly what L3 cannot promise; "this one warm method region" is what
it can.

The one-line contract for the doc's opening example: `HotPath(data)` can be anything
compilable under hardware trace — but what comes back is the *thread's* instruction
stream, so the model is at its best bracketing synchronous, warmed, same-thread work,
and it must self-flag when an async hop or window overflow breaks those assumptions.

## Non-intrusiveness, backend by backend (honest ranking)

| Backend | In-process? | Intrusive? | Completeness | Precondition |
|---|---|---|---|---|
| **Intel PT** (self) | yes | **no** — out-of-band, ~2–20 % overhead, 0 detached | full instruction stream | Intel bare metal + `perf_event_paranoid`/`CAP_PERFMON` |
| **AMD LBR** (self) | yes | **no** | **16-taken-branch window only** — truncates on all but tiny regions (modelled as `trace.truncated` + `CEILING_FREE`) | AMD Zen 3+ bare metal |
| **ARM CoreSight** (self) | yes | **no** | full stream (PT-analog) | specific bare-metal AArch64 boards |
| **EFLAGS.TF single-step** (in-process) | yes | **yes** — `SIGTRAP`/insn, shares the thread, collides with GC/JIT | exact + complete | none (any x86-64 Linux) |
| **Out-of-process ptrace stepper** | **no** (separate tracer process) | doesn't touch tracee state, but ~1000× slower on the stepped thread | exact + complete | ptrace of the target |
| **DynamoRIO DBI tier** | yes, but only under the `drrun` launcher | **yes** — software code cache, owns signals, W^X interplay | complete | launch under DynamoRIO (deployment-intrusive); documented as hostile to managed runtimes |
| **Emulator tier (Unicorn)** | **no** — a virtual guest, not this process | n/a — it re-executes bytes you hand it; it does not observe the running program | complete, for the bytes handed to it | any host (the only tier that runs on macOS) |

The only rows that satisfy *non-intrusive **and** in-process **and** only-import-plus-using*
are the three hardware backends. Every other row fails at least one ask — but each fails
*differently*, and several remain useful under the scope. The next section walks all of
them.

## The scoped model, case by case

Each tracing mechanism in the tree, judged against the two asks: **non-intrusive** (the
axes above) and **only import + using** (zero developer knowledge).

### Intel PT / ARM CoreSight self-trace — the headline case

Passes every axis; the empty constructor is viable *because* out-of-band capture is
capture-everything-then-filter-at-decode: no per-instruction decisions, no method
address, no knowledge. The whole rest of this document is this case; its only ceilings
are the host preconditions (Intel/CoreSight bare metal + perf privilege).

### AMD LBR self-trace — scope works, window doesn't

The scope shape is identical (same `begin`/`end`, same self perf event), and it is
genuinely non-intrusive — but the 16-taken-branch window means anything beyond a tiny
leaf region comes back `truncated` (modelled honestly:
[src/hwtrace.c:483](../../src/hwtrace.c#L483) keeps the richest in-region sample, and the
`CEILING_FREE` policy exists to re-resolve past it). Treat it as a smoke-level scope —
"did we enter these blocks" — not a path recorder. Zen 3 BRS / Zen 4 LbrExtV2 only; the
probe explicitly classifies **Zen 2** as the no-hardware case
([src/hwtrace.c:182](../../src/hwtrace.c#L182)); Zen 2's IBS is statistical sampling and
cannot reconstruct a complete path.

### In-process TF single-step — three sub-cases

The backend already drives the same region markers, so the `using` shim needs zero
backend-specific work; it is "the only exact in-process option where no branch-trace
facility exists (e.g. AMD Zen 2)" ([src/ss_backend.c:13-14](../../src/ss_backend.c#L13-L14)).
What it is safe *for* depends on what is inside the scope:

- **(a) Controlled native code — works today.** When the scope body is a call into a
  known native region (asm-test-generated code, a P/Invoked leaf routine), this is
  exactly what `HwTrace.Region` + the SINGLESTEP backend already do live in the .NET
  binding self-test: exact, complete, any x86-64 Linux, no PMU/privilege/decoder. The
  intrusion budget is spent while *your* code runs, so the runtime never contends for it.
- **(b) Live managed code — possible to run, documented to collide.** The backend swaps
  the **process-wide** SIGTRAP disposition for the window
  ([src/ss_backend.c:128](../../src/ss_backend.c#L128), restored at
  [:212](../../src/ss_backend.c#L212)) — during the scope, asm-test owns a signal
  CoreCLR's PAL also claims. Once the window is armed, the handler re-asserts TF in
  whatever context it interrupted ([src/ss_backend.c:106](../../src/ss_backend.c#L106)), so a
  stray SIGTRAP from *another* runtime thread mid-window would put that thread into
  runaway single-step. The state is explicitly "single region, single thread — the
  hwtrace MVP contract" ([src/ss_backend.c:56](../../src/ss_backend.c#L56)); a
  multithreaded runtime violates it by construction. Add the ~100–1000× slowdown on a
  thread the GC/JIT coordinate with, and this is precisely the collision the
  out-of-process variant was built to avoid. Verdict: fails axes 2, 3, and 5; do not
  point it at managed code.
- **(c) Managed code, exactly — go out-of-process** (next case).

### Out-of-process ptrace stepper — three shapes, one of them scope-shaped

The W2 tracer touches none of the target's signals, code bytes, or memory protections —
state-safe on every axis except timing (~1000× on the stepped thread while siblings run
native). It ships in three usage shapes:

1. **Forked-child** (`asmtest_ptrace_trace_call`): call-shaped ("run this routine with
   these args in a fresh child"), for controlled native code. Safe and exact, but not a
   scope around *your* running code.
2. **Attached-foreign** (`run_to` + `trace_attached[_versioned]`, resolved via
   perf-map/jitdump): the shipped managed-runtime path the `jit_trace` harness drives —
   but it is an external tool attaching to your app, not a `using` statement inside it.
3. **Hidden behind the `using` façade — the buildable "single-step scoped model."**
   Nothing stops the empty constructor from concealing the second process: spawn a small
   bundled tracer helper, `prctl(PR_SET_PTRACER, helper)` so a Yama
   `ptrace_scope=1` host permits the attach ([prctl(2)][prctl], [Yama][yama]), helper
   `PTRACE_SEIZE`s the calling thread and steps it from a sentinel at `{` to a sentinel
   at `}`; `Dispose()` joins the helper and renders. The developer footprint stays
   **import + using**. It works on Zen 2 and inside Docker on an Intel Mac (modern
   default container profiles permit ptrace; older ones need `CAP_SYS_PTRACE` —
   [Docker seccomp][docker-seccomp]). The honest cost: non-intrusive to *state*,
   intrusive to *timing* — the scoped thread crawls while siblings run free, so locks it
   holds stall them (the perturbation, not deadlock, form of the L3 hazard).

### Whole scope vs. one method — the descent question

The structural difference between the hardware path and any stepper: **PT captures
everything cheaply and filters at decode time; a stepper must decide, per instruction,
what to step into.** So under a stepper the empty-constructor promise degrades:

- **"Trace whatever runs in this scope"** means descending into every call — runtime
  helpers, GC barriers, the PLT — which is descent level 3 (`DESCEND_ALL`): default-off,
  denylist/budget/watchdog-guarded, and *expected to self-truncate* on a live runtime
  ([jit-runtime-tracing.md, L3 section](jit-runtime-tracing.md#when-to-use-l3-call-descent--and-why-it-is-hazardous-on-a-live-runtime)).
  Best-effort by design.
- **"Trace this one JIT'd method"** is a region + step-over (descent OFF or
  `RECORD_EDGES`/`DESCEND_KNOWN`) — reliable, and shipping today via shape 2 above. It
  costs the extra knowledge the empty form was trying to avoid: keeping the method
  un-inlined, hot, and tiering-stable, then resolving its address.

So: with hardware trace the zero-knowledge scope is the *strong* form; with a stepper the
zero-knowledge scope is the *weak* (L3, best-effort) form and the method-scoped trace is
the strong one.

### DynamoRIO DBI tier — listed for completeness, not a candidate

In-process instrumentation with region markers exists (`asmtest_drtrace.h`), but the app
must be **launched under `drrun`** — failing the deployment axis and "only import +
using" outright — and in-process DBI's collisions with managed runtimes are the
documented premise of this whole line of work
([dynamorio-native-trace-plan.md](../plans/dynamorio-native-trace-plan.md#language-runtime-support)).
Use it for native-code coverage on hosts where it is already the chosen tier; never for
a scope inside a managed app.

### Emulator tier — answers a different question

Unicorn re-executes bytes **handed to it** in a virtual guest; it cannot observe this
process's threads, so a scope around live managed code records nothing of that code. It
is, however, the only tier that runs on macOS (including Apple Intel) and the only tier
that yields **register/memory values** per step. Its role in this story is *replay*:
resolve a JIT'd method's bytes (jitdump / code-image recorder), then re-execute them in
the emulator for a value-level view — an emulation of the code, not an observation of
the run.

### Byte sources are orthogonal to all of the above

Whichever mechanism supplies **control flow** (PT packets or stepper RIPs), something
must supply the **bytes** to render assembly: the text perf-map (symbols only — enough
to *find* a method, never to decode one), jitdump (bytes, but launch-only on .NET), the
self code-image recorder (bytes, no cooperation, no launch flag — this document's
default), or the GDB JIT interface where the runtime implements it. The scoped model
composes: hardware-or-stepper control flow × recorder-supplied bytes × Capstone
rendering.

## Can the four qualifications be fixed in code?

The qualifications above are not all equally fundamental. Three are engineering gaps
with buildable fixes — some already half-built or named as TODOs in the tree — and one
(the thread/async boundary) is a genuine model change. The answer differs between the
hardware and single-step backends, so both are covered.

| Qualification | Hardware trace (Intel PT) | In-process single-step |
|---|---|---|
| **1. Thread scope / async hop** | **Hard but possible** — model change: follow the logical operation, not the thread | **Possible, ill-advised** — same hook, but arming TF on runtime threads deepens the collision |
| **2. Runtime noise (JIT/GC)** | **Already half-done** — decode region-filters today; attribution via code-image events | **Output already clean** — handler region-filters; residual is *cost*, not noise |
| **3. Bandwidth / overflow** | **Yes** — the capture-side address filter is the named TODO; sizing/snapshot exist | **Partly** — buffer overflow handled; the real ceiling is per-instruction *time* |
| **4. Nesting / concurrency** | **Yes, nearly free** — per-thread fds + multi-range decode filter | **Yes** — per-thread TLS state + a range stack in the handler |

### Qualification 1 — the thread/async boundary (the deep one)

Both backends key on the calling thread, so both miss thread hops — but the *fix shape*
is shared and specific to the runtime, not the backend. .NET's `ExecutionContext` flows
across `await`/continuations, and `AsyncLocal<T>` constructed with a value-changed handler
invokes that handler **whenever the context switches onto or off a thread**
(`AsyncLocalValueChangedArgs.ThreadContextChanged`) — precisely the "a continuation just
resumed here, possibly on a new thread" hook the raw perf/TF machinery lacks
([ExecutionContext][ec-doc], [AsyncLocal][asynclocal]). So the buildable design is:
an `AsyncLocal<ScopeId>` marker whose handler **disables** capture when the logical flow
leaves a thread and **re-arms** it on whichever thread the continuation resumes on, then
**stitches** the per-thread slices by `ScopeId` at the end. The developer still writes
only the `using`; internally the scope becomes a set of thread-window slices for one
logical operation.

- **Under PT** this is clean: Intel PT per-thread mode traces an *exact list of threads
  with no inheritance* ([perf-intel-pt][perf-intel-pt]) — so following a hop means opening
  a fresh per-thread PT event (own AUX ring) on the resuming thread, which the
  value-changed hook triggers, then merging AUX streams by `ScopeId`. It stays
  per-thread, so it needs **no privilege bump**. The alternative — **per-CPU** mode
  (`pid=-1, cpu=N`) with TID demux from `PERF_RECORD_ITRACE_START`/sched records — also
  captures hops but needs `CAP_SYS_ADMIN`/`paranoid=-1` and heavier decode; the
  per-thread-plus-hook route is preferable.
- **Under single-step** the same hook can `ss_arm_tf()` on the resuming thread and make
  `g_base`/`g_stream`/`g_armed` thread-local — but arming TF on a thread the runtime
  scheduled means asm-test is now single-stepping runtime-owned threads, compounding the
  SIGTRAP-ownership collision this doc already flags. Buildable, not advisable on a live
  runtime; fine for a body you know is synchronous.

Verdict: possible, and the `AsyncLocal` value-changed hook is the elegant enabler — but
it converts "thread window" into "stitched logical-operation trace," which is where the
real engineering (and the multi-slice output contract) lives.

### Qualification 2 — runtime noise

Largely a non-issue once the scope is *region-scoped* rather than *whole-window*, and both
backends already filter to the region:

- **PT** already drops out-of-region instructions at decode
  ([src/pt_backend.c:108-116](../../src/pt_backend.c#L108-L116)) — the JIT/GC/runtime code
  that runs *outside* the registered range never reaches the trace. The remaining noise is
  only in the empty-ctor *whole-window* mode; three buildable refinements bound it:
  (a) program the capture-side IP **address filter** so the CPU emits packets only for the
  region (the named TODO at [src/pt_backend.c:129-134](../../src/pt_backend.c#L129-L134));
  (b) use the **code-image emission events** (mprotect/mmap `PROT_EXEC`, the eBPF detector
  in [asmtest_codeimage.h](../../include/asmtest_codeimage.h)) to timestamp *when* a
  method's bytes appeared, so the "JIT compiling HotPath" slice can be split from the
  "HotPath running" slice; (c) **symbolize and bucket** every IP against `/proc/self/maps`
  + the perf-map (`asmtest_proc_region_by_addr`, `asmtest_proc_perfmap_symbol`) so noise is
  *labelled* ("31k insns in RyuJIT, 2k in GC, 7k in HotPath") rather than silently mixed.
- **Single-step** already excludes noise from the *output* — the handler records only
  in-region RIPs ([src/ss_backend.c:95-103](../../src/ss_backend.c#L95-L103)) — so an
  unwarmed body's JIT/GC steps run **unrecorded**. The residual problem is *cost*, not
  output: it still traps on every one of those instructions at ~1000×. The fix is to reach
  the region warm — pre-warm before arming, or gate arming on region entry the way the
  out-of-process variant's `run_to` does — not more filtering.

### Qualification 3 — bandwidth / overflow

- **PT**: the structural fix is the same **address filter** as Q2 — a region filter cuts
  emitted bandwidth by orders of magnitude for a small hot region inside a large window.
  Buffer sizing (`aux_size`/`data_size`) and the circular **snapshot** option are already
  first-class in the options
  ([asmtest_hwtrace.h:62-68](../../include/asmtest_hwtrace.h#L62-L68)) — though snapshot
  is capture-side only so far: `end()` decodes the linear ring, and the circular-ring
  walk from `aux_tail` is its own named follow-up
  ([src/hwtrace.c:782-783](../../src/hwtrace.c#L782-L783)); PSB-period / cycle
  toggles trade detail for bytes. Overflow already maps onto `truncated`. All standard PT
  knobs — the only gate is PT hardware to validate the filter.
- **Single-step** has no I/O bandwidth; its budget is the fixed 512 KiB offset buffer
  (`SS_STREAM_CAP`), and overflow is already handled honestly (`g_overflow → truncated`,
  [src/ss_backend.c:99-102](../../src/ss_backend.c#L99-L102), [:201-202](../../src/ss_backend.c#L201-L202)).
  You can enlarge it, or make it a growable buffer sized between windows (never `malloc`
  in the handler — it must stay async-signal-safe). But the true ceiling is **time**: a
  long region at a trap per instruction, which no buffer change fixes. The honest answer
  there is "trace a smaller region," or fold loops (record body-once + count) at the cost
  of the ordered-stream contract.

### Qualification 4 — nesting / concurrency

The single process-global slot is an explicit MVP limitation
([asmtest_hwtrace.h:144-146](../../include/asmtest_hwtrace.h#L144-L146)), and the fix is the
one the header itself points at — **per-thread state**:

- **PT**: give each scoping thread its own per-thread event + AUX ring (per-thread mode
  supports exactly this). Nesting on one thread is nearly free because the decoder already
  range-filters — attribute the one AUX stream to *several* nested ranges at decode instead
  of one, or refcount enable/disable across the nest.
- **Single-step**: move `g_base`/`g_stream`/`g_armed`/`g_old_sa` into thread-local storage
  and replace the single `[base,len)` test with a small fixed-size **range stack** the
  handler walks (innermost match wins) — keeping it async-signal-safe (a TLS array, no
  allocation). The SIGTRAP disposition is process-wide, so concurrent scopes on different
  threads still share one handler, but per-thread `g_armed` + TLS state makes that safe.

### Net

Q2, Q3, and Q4 are ordinary engineering — and PT already ships the decode-side half of
Q2 and names the capture-side filter (which also solves Q3) as its own next step, while
single-step already handles the Q2 output and Q3 overflow cases. Q1 is the one that is not
a knob but a redesign: **the model has to stop being a thread window and become a stitched
trace of a logical operation**, enabled by the `AsyncLocal` context-flow hook. That fix
lives naturally under PT (per-thread, no privilege bump); under single-step it is
technically possible but pushes further into the intrusiveness this document already warns
against. In every case the developer-visible surface stays the import plus the `using` —
the work is entirely behind the constructor.

## How this lands on concrete hosts

The two hosts this analysis was asked about, plus the neighbours needed to complete the
picture:

| Host | Non-intrusive hardware path | Exact path under the scope | Notes |
|---|---|---|---|
| Intel bare-metal Linux | **Intel PT** | PT | the full model, as designed |
| AMD Zen 3/4/5 Linux | LBR (16-branch ceiling) | hidden ptrace stepper | no PT on AMD, ever |
| **AMD Zen 2 Linux** | **none** — no PT, no BRS/LbrExtV2 ([src/hwtrace.c:182](../../src/hwtrace.c#L182)); IBS is sampling-only | hidden ptrace stepper | the host class the W2 stepper was built for (zen2-singlestep-trace-plan) |
| **Apple Intel, macOS** | **none** — the CPU has PT silicon, but macOS exposes no `perf_event_open`/PT API and the whole tier is Linux-gated (perf probe returns 0 off Linux, [src/hwtrace.c:226-229](../../src/hwtrace.c#L226-L229); single-step likewise, [:154-164](../../src/hwtrace.c#L154-L164)) | none (emulator only — virtual, different question) | the OS, not the CPU, is the blocker |
| Apple Intel, bare-metal Linux | **Intel PT** | PT | pre-T2 Intel Macs boot standard Linux; full model |
| Linux VM / Docker on the Mac | none — hypervisors don't pass PT through | ptrace stepper or single-step inside the VM | matches the repo's `docker-*` flow |
| Bare-metal AArch64 with CoreSight | **CoreSight** | ptrace stepper (AArch64 is supported) | board-specific |
| Cloud / CI VMs | rarely (host must opt in) | ptrace stepper | self-skip is the norm |

## Where "only import + using" leaks — and what to do about it

Three honest edges, in decreasing order of how often they bite. (They are written
against the lowest common denominator; the next section shows how much dissolves when
**.NET 8+** can be assumed.)

1. **Tracing one *specific* method, completely, needs help the empty ctor can't give.**
   The empty scope captures the dynamic path through the window. If you instead want
   "the full body of `HotPath`, every block, guaranteed," you must (a) keep it hot and
   un-inlined so it exists as a standalone body — the examples do this with
   `[MethodImpl(MethodImplOptions.NoInlining)]` and `DOTNET_TieredCompilation=0`
   ([examples/jit_dotnet/Program.cs](../../examples/jit_dotnet/Program.cs)) — which is
   *extra* beyond "just using," and (b) resolve its native address.
2. **In-process method→address resolution is version-fragile on .NET.** Up to .NET 6,
   `RuntimeHelpers.PrepareMethod(m.MethodHandle)` then `m.MethodHandle.GetFunctionPointer()`
   returned the JIT'd entry; **this changed in .NET 7** (`PrepareMethod` no longer
   reliably forces the JIT, and the pointer can be a precode stub, not the body — see
   [dotnet/runtime#83042]). So the clean in-process address path is not a stable
   contract. This is a second reason the **whole-window** capture (which needs no
   address) is the robust "assume no knowledge" default; when an address *is* needed,
   fall back to resolving the region from `/proc/self/maps`
   (`asmtest_proc_region_by_addr`) around a captured sample IP, or from the perf-map.
3. **`.NET`'s own jitdump is launch-flag-shaped — but no longer strictly launch-only.**
   `DOTNET_PerfMapEnabled` is read at process start ([.NET runtime-config][dotnet-perfmap]),
   and a `[ModuleInitializer]` runs too late to set it. But since **.NET 8** the
   diagnostic IPC port accepts `EnablePerfMap`/`DisablePerfMap` process commands
   ([IPC protocol][dotnet-ipc]; `DiagnosticsClient.EnablePerfMap(PerfMapType.JitDump)`,
   [diagnostics client][dotnet-diag-client]) — and that port is a filesystem socket a
   process can connect to **on itself**, so the initializer *could* switch jitdump on at
   arm time with no launch flag — and the enable path passes `sendExisting = true`, so
   methods compiled *before* the toggle are enumerated and dumped too
   ([ds-rt-coreclr.h][dotnet-ds-rt], [perfmap.cpp][dotnet-perfmap-cpp]). The design
   still does **not** depend on it for the pure in-process case: it is .NET-8+-only,
   pulls in the diagnostics-client plumbing, and
   the temporal-bytes rule still applies across recompilation — the self code-image
   recorder needs none of that. (Jitdump remains the *lowest-overhead* byte source when
   you can enable it — at launch, or at arm time on .NET 8+ — per
   [jit-runtime-tracing.md](jit-runtime-tracing.md#1-cooperative--jitdump-enabled-at-runtime).)

Tiered/OSR recompilation can also move a method to a new address mid-run; the code-image
recorder's versioning is exactly the mechanism for tracking that, but a *stable* trace of
a *specific* hot method still benefits from pinning tiering off — again, extra beyond the
bare `using`.

## Closing the leaks on .NET 8+

Pinning the target at **.NET 8+** changes the leak ledger materially: leak 2 and leak 3
dissolve into supported, in-process, no-launch-flag mechanisms, and leak 1 shrinks to
its irreducible core — inlining.

### The runtime-events route — addresses without files, flags, or `PrepareMethod`

CoreCLR's JIT already announces every method it emits. The runtime provider
(`Microsoft-Windows-DotNETRuntime`) raises `MethodLoadVerbose_V2` under `JITKeyword`
(0x10) carrying **`MethodStartAddress`, `MethodSize`, namespace/name/signature, and
`ReJITID`** — and it is consumable **in-process** by a plain
`System.Diagnostics.Tracing.EventListener` (the `NativeRuntimeEventSource` surface): a
supported public API, no file, no IPC, no privilege, no launch flag
([method runtime events][dotnet-method-events], [in-process CLR event listeners][criteo-listeners]).
Each new code version (tier-up, OSR) raises a fresh event with the new address. So the
`[ModuleInitializer]` can hang a listener at arm time and maintain the
name→(address, size, version) map itself, feeding each (address, size) straight into
the self code-image recorder (`asmtest_codeimage_track`) — the byte-source composition
this document already wants, with the runtime *volunteering* the "what changed, where"
signal the recorder otherwise detects by page-scan. This retires the leak-2 workaround
wholesale: neither the broken `PrepareMethod`+`GetFunctionPointer` contract nor
perf-map file parsing is needed for anything JITted after arm.

Two companion keywords sharpen leak 1's diagnostics: `JITTracingKeyword` (0x1000)
raises `MethodJitInliningSucceeded/Failed` naming inliner and inlinee — the scope can
*know* `HotPath` was folded into its caller and report that instead of an empty
region — and `JittedMethodILToNativeMapKeyword` (0x20000) supplies IL↔native maps for
labelling which window instructions belong to which source method.

### Methods JITted *before* arm — the runtime rundown is real

The pre-arm gap is covered twice on .NET 8+:

- The diagnostic-IPC `EnablePerfMap` handler calls
  `PerfMap::Enable(type, /* sendExisting */ true)` ([ds-rt-coreclr.h][dotnet-ds-rt]):
  enabling at arm time **enumerates already-compiled code** — R2R methods of loaded
  assemblies and the JIT code heaps via `CodeHeapIterator`
  ([perfmap.cpp][dotnet-perfmap-cpp]). A self-connected `EnablePerfMap(JitDump)` thus
  yields a jitdump containing pre-arm methods, which the shipped
  `asmtest_jitdump_*` / `asmtest_proc_perfmap_symbol` parsers already read.
- Alternatively, a self EventPipe session opened with rundown requested replays
  `MethodDCEnd` events for existing methods. (An `EventListener` alone cannot get
  these — the rundown provider is not an `EventSource` — which is why the perfmap
  route above is the lighter default.)

### What leak 1 reduces to

With addresses event-supplied and bytes recorder-supplied, the two annotations lose
their *correctness* role:

- **`DOTNET_TieredCompilation=0` is no longer needed to be right — only to be quiet.**
  Movement is the versioned recorder's job by design: each recompilation announces
  itself (a fresh `MethodLoadVerbose` at a new address), the recorder snapshots the new
  bytes as a new version, and each trace slice decodes against the version live in its
  window — the temporal-bytes rule, event-driven instead of page-scan-driven. Pinning
  tiering off remains a *noise* preference (one stable body instead of several), not a
  correctness precondition.
- **`[MethodImpl(NoInlining)]` is the irreducible residue — but it is detectable,
  avoidable, and (at the heavy end) reversible.** *Detectable*: the inlining events
  above. *Avoidable*: whole-window capture does not care — the inlined copy's
  instructions are captured where they ran, and the IL↔native map attributes them; and
  a scope API handed the method reference can invoke it through a delegate from an
  internal helper pinned `NoInlining | NoOptimization`, so the standalone body executes
  with no annotation on *user* code (minopts code performs no guarded-devirtualization
  inlining, which is what could otherwise fold a hot monomorphic delegate target into
  the helper under .NET 8 dynamic PGO). *Reversible, at real cost*: since .NET Core 3 a
  profiler can attach at runtime — over the same self-connectable IPC
  (`AttachProfiler`) — and call `ICorProfilerInfo10::RequestReJITWithInliners`, which
  re-JITs the method **and every method it was already inlined into** and blocks future
  inlining of the re-JITted body (`COR_PRF_REJIT_BLOCK_INLINING`)
  ([RequestReJITWithInliners][dotnet-rejit-inliners], [ReJIT on attach][dotnet-rejit-attach]).
  That means shipping a native profiler component inside the package — buildable, but a
  heavier posture than the thin-shim model, so it is the documented escape hatch, not
  the default.

Net, on .NET 8+: **leak 2 disappears** (the runtime's own events supply addresses;
nothing version-fragile remains), **leak 3 disappears** (bytes come from the recorder
fed by events, or from a runtime-enabled jitdump that includes pre-arm methods), and
**leak 1 contracts** to "you must *name* the method" — which is not a workaround but
the ask itself — plus an inlining caveat the scope can detect, route around, or (with a
profiler shim) undo.

## What already ships vs. what is new

**Already implemented (verified in the tree):**

- Self, per-thread PT/AMD/single-step region capture bracketed by `begin`/`end`
  (`asmtest_hwtrace_begin`/`end`, `src/hwtrace.c`).
- A .NET scope wrapper — `HwTrace.Region(name, Action)` — with balanced markers
  ([bindings/dotnet/hwtrace/HwTrace.cs:602](../../bindings/dotnet/hwtrace/HwTrace.cs#L602)).
- Backend auto-selection + clean self-skip (`asmtest_hwtrace_auto`,
  `asmtest_trace_auto`, `available()`/`skip_reason()`).
- The self (`pid == 0`) time-aware code-image recorder for the bytes
  ([include/asmtest_codeimage.h](../../include/asmtest_codeimage.h), `src/codeimage.c`).
- In-process region resolution from an address (`asmtest_proc_region_by_addr`) and
  Capstone rendering (`emu_disas`, `src/disasm.c`).

**New to reach "empty ctor, handle everything":**

1. An `IDisposable AsmTrace` scope (a thin shim over `begin`/`end`, with
   `[CallerMemberName]` auto-naming and default-sink emission on `Dispose`).
2. A `[ModuleInitializer]` that arms the tier on import and wires the **self** code-image
   recorder as the **PT decoder's** image source. Today the recorder feeds the
   *out-of-process* stepper (`_versioned`); feeding libipt's image callback for the
   in-process self case is the same forward-look
   [jit-runtime-tracing.md](jit-runtime-tracing.md) flags as PT Phase 2.
3. The libipt decode-against-self-code-image glue (the remaining forward-look piece; the
   recorder and Capstone rendering already exist). Note the shipped decoder is
   region-scoped: with no image for an IP it stops at the first out-of-region
   instruction ([src/pt_backend.c:128-136](../../src/pt_backend.c#L128-L136)), so
   whole-window decode must hand libipt the *full* executed image set —
   recorder-tracked JIT pages plus the file-backed DSOs enumerable from
   `/proc/self/maps` — not just the JIT pages.
4. Optionally, for hosts with no hardware trace (Zen 2, Docker-on-Mac): the concealed
   out-of-process stepper scope — spawn a bundled tracer helper, `PR_SET_PTRACER`,
   `PTRACE_SEIZE` the scoped thread, step between the scope sentinels. Exact, state-safe,
   timing-intrusive; whole-scope capture rides descent L3 (best-effort), method-scoped
   capture is reliable.

## Boundary — what the scope does *not* give

Inherited directly from PT/ETM: you get **which** instructions ran, **in what order**,
and the **code bytes** (rendered to assembly) — **not** register or memory *values* at
each step. Data-value capture stays the emulator tier's job (`emu_result_t`, watchpoints)
or would need a PTWRITE/DBI memory-event mode. Basic-block boundaries from a branch
decoder are not identical to the emulator's, so cross-tier block parity needs the same
normalization step documented for the other backends. This is the same boundary
[jit-runtime-tracing.md](jit-runtime-tracing.md#data-provided) records.

## Caveats and preconditions (summary)

- **Non-intrusive + in-process ⇒ hardware trace ⇒ Intel PT bare metal** (or LBR/CoreSight
  with their own limits). On AMD/ARM/cloud the scope self-skips or you accept single-step
  (intrusive) or an out-of-process stepper (a second process).
- **Privilege is a host prerequisite, not code** — provision it once
  (`perf_event_paranoid`/`CAP_PERFMON`/container capability). The import detects and
  self-skips when it is missing.
- **AMD LBR truncates** past 16 taken branches; treat its trace as best-effort and
  re-resolve under `CEILING_FREE`.
- **.NET specifics:** jitdump's env var is launch-only, but the .NET 8+ runtime toggle
  (diagnostic-IPC `EnablePerfMap`, self-connectable) covers already-compiled methods
  too (`sendExisting`) — the self code-image recorder stays the default byte source;
  `PrepareMethod`+`GetFunctionPointer` is not a stable address contract post-.NET 7 (on
  .NET 8+ resolve addresses from the runtime's own `MethodLoadVerbose` events via an
  in-process `EventListener` instead); tiering can move code (track it via those same
  events + recorder versioning; pin it off only for a *quieter* single-method trace).
- **The temporal-bytes correctness rule holds:** decode against the version live during
  the window, not a late snapshot — the self recorder is what makes that correct.
- **In-process single-step is for controlled native regions only.** Pointed at live
  managed code it swaps a process-wide signal disposition the runtime also owns and can
  put sibling threads into runaway stepping — use the out-of-process shape there, and
  accept that whole-scope stepping is descent-L3 best-effort while method-scoped
  stepping is reliable.

## Relationship to asm-test

This is the in-process, cooperative, developer-ergonomics face of the same machinery the
hardware-trace plan and [jit-runtime-tracing.md](jit-runtime-tracing.md) develop for the
foreign-attach case. It adds no new decoder or capture primitive — it repackages the
shipped self-trace region markers, the self code-image recorder, backend auto-selection,
and Capstone rendering behind a single `IDisposable` whose only visible surface is a
constructor. The bindings already treat `IDisposable` scopes and `available()`/skip-reason
self-skip as the house style, so the ergonomics land inside existing conventions rather
than bolting a new model on.

## Sources

Shared hardware-trace / jitdump / temporal-bytes background and its full citation set:
[jit-runtime-tracing.md](jit-runtime-tracing.md#sources). Claims specific to the
in-process `using` framing:

- Intel PT self-monitoring (`pid == 0`, `cpu == -1`), address-range filtering, and the
  privileged-vs-unprivileged AUX buffer sizes: [perf-intel-pt(1)][perf-intel-pt],
  [perf_event_open(2)][perf-eo], [perf address range filtering][lwn-filter].
- `.NET` perf-map / jitdump: the env var is read at launch; since .NET 8 the diagnostic
  IPC port can toggle it at runtime (`EnablePerfMap` — self-connectable):
  [.NET debugging/profiling config][dotnet-perfmap],
  [dotnet/runtime#82142 (separate perfmap/jitdump)][dotnet-82142],
  [diagnostics IPC protocol — `EnablePerfMap` (since .NET 8)][dotnet-ipc],
  [DiagnosticsClient.EnablePerfMap / PerfMapType][dotnet-diag-client].
- In-process JIT'd-method address via `RuntimeHelpers.PrepareMethod` +
  `RuntimeMethodHandle.GetFunctionPointer` **and its .NET 7 behaviour change**:
  [PrepareMethod docs][dotnet-preparemethod], [dotnet/runtime#83042][dotnet-83042].
- The concealed-tracer attach path: [prctl(2)][prctl] (`PR_SET_PTRACER`),
  [Yama LSM ptrace_scope][yama], [Docker seccomp profiles][docker-seccomp].
- Following a logical operation across threads: Intel PT per-thread mode has no
  inheritance ([perf-intel-pt][perf-intel-pt]); .NET `ExecutionContext` flows across
  `await`, and `AsyncLocal<T>`'s value-changed handler fires on the thread-context switch
  ([ExecutionContext][ec-doc], [AsyncLocal][asynclocal]).
- Closing the leaks on .NET 8+: in-process method addresses from the runtime's own
  events ([method runtime events][dotnet-method-events] — `MethodLoadVerbose_V2` carries
  `MethodStartAddress`/`MethodSize`/`ReJITID`; [in-process CLR event listeners][criteo-listeners]);
  arm-time rundown of already-compiled code
  ([ds-rt-coreclr.h — `EnablePerfMap` passes `sendExisting = true`][dotnet-ds-rt],
  [perfmap.cpp][dotnet-perfmap-cpp]); runtime un-inlining via attach-time profiler ReJIT
  ([RequestReJITWithInliners][dotnet-rejit-inliners], [ReJIT on attach][dotnet-rejit-attach]).

[perf-intel-pt]: https://man7.org/linux/man-pages/man1/perf-intel-pt.1.html
[perf-eo]: https://www.man7.org/linux/man-pages/man2/perf_event_open.2.html
[lwn-filter]: https://lwn.net/Articles/684666/
[dotnet-perfmap]: https://learn.microsoft.com/en-us/dotnet/core/runtime-config/debugging-profiling
[dotnet-82142]: https://github.com/dotnet/runtime/pull/82142
[dotnet-ipc]: https://github.com/dotnet/diagnostics/blob/main/documentation/design-docs/ipc-protocol.md
[dotnet-diag-client]: https://learn.microsoft.com/en-us/dotnet/core/diagnostics/microsoft-diagnostics-netcore-client
[dotnet-method-events]: https://learn.microsoft.com/en-us/dotnet/fundamentals/diagnostics/runtime-method-events
[criteo-listeners]: https://medium.com/criteo-engineering/c-in-process-clr-event-listeners-with-net-core-2-2-ef4075c14e87
[dotnet-ds-rt]: https://github.com/dotnet/runtime/blob/main/src/coreclr/vm/eventing/eventpipe/ds-rt-coreclr.h
[dotnet-perfmap-cpp]: https://github.com/dotnet/runtime/blob/main/src/coreclr/vm/perfmap.cpp
[dotnet-rejit-inliners]: https://learn.microsoft.com/en-us/dotnet/core/unmanaged-api/profiling/icorprofilerinfo10-requestrejitwithinliners-method
[dotnet-rejit-attach]: https://github.com/dotnet/runtime/blob/main/docs/design/coreclr/profiling/ReJIT%20on%20Attach.md
[dotnet-preparemethod]: https://learn.microsoft.com/en-us/dotnet/api/system.runtime.compilerservices.runtimehelpers.preparemethod
[dotnet-83042]: https://github.com/dotnet/runtime/issues/83042
[prctl]: https://man7.org/linux/man-pages/man2/prctl.2.html
[yama]: https://docs.kernel.org/admin-guide/LSM/Yama.html
[docker-seccomp]: https://docs.docker.com/engine/security/seccomp/
[ec-doc]: https://learn.microsoft.com/en-us/dotnet/standard/asynchronous-programming-patterns/executioncontext-synchronizationcontext
[asynclocal]: https://learn.microsoft.com/en-us/dotnet/api/system.threading.asynclocal-1
