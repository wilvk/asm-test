# asm-test — live-attach data-flow: post-implementation follow-up plan

The next steps **after** [live-attach-dataflow-plan.md](live-attach-dataflow-plan.md) lands
its native + JIT-aware live-attach value producer and the asmspy Data flow window. That plan
deliberately ships the **conservative core**: direct single-step value capture (proven, but
it perturbs the stepped thread), scoped to a region, with managed memory def-use left GC-
uncanonicalized. This plan lands the improvements that were held out of the critical path —
each one either **reduces perturbation**, **raises fidelity on managed targets**, or
**broadens the target set** — ordered so the highest-value, lowest-risk work comes first.

> Status: **F1 + F3 spikes done (increment 0 — both GO, 2026-07-15); full landings + F2/F4–F7 planned.** F1 is the marquee item (the
> perturbation win that makes stepping a live JIT genuinely practical) and carries a spike
> increment because its value-reconstruction claim is the one genuinely **unproven** bet in
> the whole design. F3 is the cheapest high-value item (a near-zero-perturbation targeted
> mode from a one-line DR7 change). F4/F5 are hardware-/runtime-gated and self-skip. House
> rule unchanged: foreign targets are never killed; every tier self-skips where its substrate
> is absent.

---

## Goals and non-goals

**Goals** — take the shipped single-step live-attach tier and: (F1/F2) cut its perturbation
by an order of magnitude via block-step + emulator replay, escalating to record-and-inject
for OS-interacting code; (F3) add a near-zero-perturbation hardware-watchpoint targeted mode;
(F4) make managed **memory** def-use survive GC compaction on a live attach; (F5) add the
least-perturbing PT-derived value path where the silicon allows; (F6) broaden toward
whole-process; (F7) expose it through the language bindings.

**Non-goals** — production whole-process **taint** over managed code (that remains the
DynamoRIO tier, [dynamorio-taint-tier-plan.md](dynamorio-taint-tier-plan.md)); this plan
stays on the out-of-band observe-don't-instrument side. Windows / macOS attach (Linux
x86-64 first, AArch64 where the primitive exists).

---

## F1 — Block-step + emulator-replay value optimization *(**spike GO 2026-07-15** — increment 0 proven; full landing planned)*

> **SPIKE (increment 0) — GO, 2026-07-15.** On the oracle fixtures the block-step + Unicorn-replay value
> trace is **byte-identical** (literal `memcmp` of the record arrays, not just slice-equal) to true
> single-step: `loop_poly` 303 steps / 854 records, `mem_chain` 6/16. Stop-count drops **6.06×** / **6.00×**
> (tracking mean block length, as predicted). The purity static-scan classifies 7/7 fixtures (pure loop/mem
> vs syscall/rdtsc/cpuid/rdrand/int0x80); a coherence-canary injected replay-input divergence is detected
> at the next real boundary → `truncated`. Probe `examples/blockstep_value_spike.c`, findings
> `docs/internal/analysis/2026-07-15-blockstep-value-spike.md`. Next: wire the replay into a
> `dataflow_blockstep.c` value tier (full `GETREGS` per boundary, purity-gated, rsp-relative oracle diff,
> canary-truncated). Gotchas: map a one-page landing pad for the exit `ret`; validate rsp-relative (two
> forks differ by an absolute-address delta); undefined-flag (xor AF) divergence is a documented
> theoretical risk, not observed here.

Direct single-step traps on **every** instruction; that density is exactly what widens the
cross-thread deadlock window on a live runtime. Decouple values from stops.

- Drive the region with **`PTRACE_SINGLEBLOCK`** — one `#DB` per taken branch, an order of
  magnitude fewer stops (the library already block-steps for control flow,
  [asmtest_ptrace_trace_attached_blockstep](../../../include/asmtest_ptrace.h#L125),
  [ptrace_backend.c:1761](../../../src/ptrace_backend.c#L1761); flagged as an unapplied
  perturbation lever).
- At each boundary add a **`GETREGS` + XSTATE** read (the existing block-step reads only
  PC + the return register via `read_pc_ret`,
  [ptrace_backend.c:1829](../../../src/ptrace_backend.c#L1829) — this is *new* capture, not
  free reuse), giving a real ground-truth register snapshot at every branch.
- **Replay the straight-line block** through the Unicorn L0 producer
  ([src/dataflow_emu.c](../../../src/dataflow_emu.c)), seeded with that real register state
  and memory faulted in lazily from the tracee, to reconstruct the per-instruction values
  *between* boundaries. The endpoints are always real observations, so replay only ever
  fills a bounded pure interior — the plan's Phase-3b idea
  ([data-flow-tracing-plan.md:183-186](data-flow-tracing-plan.md#L183)), promoted to the
  primary perturbation win.
- **Region-granularity purity classification** to avoid emulating through the OS: static-scan
  the method's (time-correct) bytes once for `syscall`/`sysenter`/`int 0x80`/`rdtsc`/
  `rdtscp`/`rdrand`/`rdseed`/`cpuid`; a **pure** method gets block-step + replay, an
  **impure** one falls back to direct single-step (F2 lifts that restriction). This
  sidesteps the ordering trap — block-step advances the *real* process, so a syscall in a
  block has already run by the boundary — by classifying per region up front, never
  retroactively.

**Spike (increment 0):** capture one deterministic block **both ways** — block-step + replay
vs. true single-step — on the oracle fixture and assert the value traces are **byte-
identical**. If they diverge, the optimization does not land and the base plan's single-step
tier is the permanent path; nothing else here depends on F1 succeeding.

**Exit criteria:** on a pure method, the block-step + replay value trace is byte-identical to
the single-step trace on the cross-validation oracle; the measured **stop count** drops
proportionally to mean block length; an impure method is correctly detected and stepped;
a concurrent-memory divergence (a load of a byte a sibling rewrote between snapshot and
execution) is **detected** at the next real boundary and dropped to `truncated`, never
silently wrong.

---

## F2 — Record-and-inject OS-interaction fidelity *(planned — depends on F1)*

Let block-step + replay cover **impure** methods too, instead of falling back to single-step.

- During the forward block-step pass, also trap at syscalls and record their effect: `rax`
  plus the memory bytes the kernel wrote (asmspy already decodes syscall args/results, so the
  decode is reused). During replay, when the emulator reaches that syscall, **inject** the
  recorded result and memory delta rather than executing it — the rr / WinDbg-TTD model, the
  "full input-capture record/replay" escalation the base plan names
  ([data-flow-tracing-plan.md:184-186](data-flow-tracing-plan.md#L184)).
- Same treatment for nondeterministic instructions (`rdtsc`/`rdrand`/`cpuid`): record the
  real retired value on the forward pass, inject on replay.

**Exit criteria:** a method containing a `read()`/`clock_gettime` replays with **correct**
post-syscall values via injected effects and matches the single-step oracle; the concurrency
residual (a sibling writing shared memory the region loads, with no syscall to anchor the
value) is documented as the remaining limit and detected via the real endpoints.

---

## F3 — Hardware data-watchpoint targeted mode *(**spike GO 2026-07-15** — proven; full landing planned)*

> **SPIKE — GO, 2026-07-15.** Data watchpoints work exactly as bet: a WRITE watchpoint (`DR7=0x90001`:
> R/W0=01, LEN0=8B) on a chosen global captured all 16 stores with the correct value (`process_vm_readv`)
> and faulting PC (`GETREGS`), matching an independent ground-truth 4/4; R/W=11 also traps reads (16 vs 8
> hits). Perturbation is within noise (~2M loop iterations between hits with zero traps) — projected
> ~2,000,000× fewer stops / ≥4,000× less wall-time than single-stepping the same workload. Caps: 4 slots,
> 1/2/4/8-byte aligned, self-skips under qemu (zero slots). Probe `examples/watchpoint_spike.c`, findings
> `docs/internal/analysis/2026-07-15-hw-watchpoint-spike.md`. Recommended landing: `asmspy --watch <pid>
> <addr|func+off> [--rw] [--len]` reusing this DR7/DR6 plumbing, **per-thread arming across
> `/proc/<pid>/task/*`** (mandatory for a live multi-threaded target), a one-insn decode for read/write
> labelling, and the AArch64 `NT_ARM_HW_WATCH`/`DBGWCR` analog.

The repo wires only **execution** breakpoints today (`set_hw_bp` hard-codes `DR7 = 0x1`,
i.e. `R/W0 = 00`, [ptrace_backend.c:485-493](../../../src/ptrace_backend.c#L485)). The x86
debug registers also support **data watchpoints**: `R/Wn = 01` (write) or `11` (read/write)
and `LENn` = 1/2/4/8 bytes — **4** of them, touching **no code**, running at **native speed
between hits**. This is the only non-code-modifying, non-single-step primitive that observes
memory data flow, and enabling it is a one-line DR7 change plus a value read at the stop.

- New asmspy mode / `--watch <pid> <addr|func+off> [--rw]`: arm a data watchpoint on a chosen
  memory location (an object field, a stack slot), `PTRACE_CONT` the target, and at each `#DB`
  report the access (read vs. write, the value via `process_vm_readv`, and the faulting PC
  resolved to a function). Answers "**who touched this field, and with what value**" — a
  data-flow question nothing else in asmspy can, at near-zero perturbation.
- AArch64 analog via the `NT_ARM_HW_WATCH` regset (mirrors the existing `NT_ARM_HW_BREAK`
  path, [ptrace_backend.c:502-550](../../../src/ptrace_backend.c#L502)).

**Exit criteria:** watch a chosen location on a live process and report every read/write with
its value and function at native speed; the 4-location / 1-2-4-8-byte caps are documented and
enforced; self-skips under qemu-user (which emulates zero breakpoint slots,
[ptrace_backend.c:527-529](../../../src/ptrace_backend.c#L527)).

---

## F4 — Live GC-move canonicalization for managed memory def-use *(planned — runtime-gated)*

The base plan leaves managed **memory** def-use GC-uncanonicalized: a store before a
compaction (old address) and its matching load after (new address) look unrelated, so the
edge is lost, and an object reusing the vacated address forges a false edge. The **pure
transform is already landed** — `asmtest_gcmove_canonicalize`
([asmtest_valtrace.h:353](../../../include/asmtest_valtrace.h#L353)),
[src/dataflow_gcmove.c](../../../src/dataflow_gcmove.c) — what is missing is the live feed.

- Extract concrete `{old_base, new_base, len}` triples from EventPipe
  `GCBulkMovedObjectRanges`, stamped with the value-trace step boundary the compaction takes
  effect at, and run `asmtest_gcmove_canonicalize` on the captured trace before
  `asmtest_defuse_build`. This closes the gap the memory notes flag: the `GcMoveMap` captures
  the event but "the in-proc EventListener does not surface the Values struct-array," so the
  triple extraction + remap wiring is the concrete deferred work
  ([data-flow-tracing-plan.md:193](data-flow-tracing-plan.md#L193)).
- Full object identity via `GCBulkType`/`Node`/`Edge` is a further step.

**Exit criteria:** a managed value's **memory** def-use survives an induced GC compaction
without pre/post-move aliasing on a **live attach** (not just the landed pure unit test);
a value is attributed to the correct method+version after a tiered re-JIT (already landed for
control, re-validated for the live data path).

---

## F5 — PT + code-image + replay: the least-perturbing ceiling *(planned — Intel-PT-gated)*

The conceptual minimum-perturbation path from the design doc ("stop instrumenting and start
observing"): reconstruct the **exact executed instruction stream** out of band via Intel PT /
CoreSight ([asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h)), supply time-correct bytes
from the code-image recorder, then **replay that exact path** through the emulator to derive
values — zero code touch, zero single-step, owns no signals, never faults a JIT page. This is
to the JIT what the base plan's single-step is to native: the "safe on any JIT, when the
silicon allows" tier, gated exactly as the AMD-only IBS view is.

**Exit criteria:** on an Intel-PT bare-metal host, a method's value trace is derived from a PT
stream + code-image replay with **no single-step of the target**; it matches the single-step
oracle on a deterministic region; off PT hardware (most cloud/CI) it self-skips cleanly. Note
the boundary the analysis doc is explicit about: PT gives control flow + bytes, **never
values** — the values come entirely from the replay, so F5 inherits F1/F2's OS-interaction
tiering.

---

## F6 — Toward whole-process / continuous data flow *(planned — bridges to the DR tier)*

Lift the scoped-region bound. The windowed steppers + the addr-channel already follow a live
JIT's methods across a whole window for control flow
([asmtest_ptrace_trace_attached_windowed](../../../include/asmtest_ptrace.h#L142)); extend
the value producer onto that windowed frame for a **survey** (sampled) def-use over a whole
window rather than one region. Where the ask is production whole-process **taint**, this hands
off to [dynamorio-taint-tier-plan.md](dynamorio-taint-tier-plan.md) (whose in-band L0 value
producer already landed) rather than pushing single-step past its cost envelope.

**Exit criteria:** a windowed capture produces a def-use survey spanning multiple JIT'd
method ranges of a live process; the hand-off boundary to the DR taint tier is documented
(single-step/replay for scoped exactness; DR for whole-process breadth).

---

## F7 — Live-attach data-flow in the language bindings *(planned)*

Wrap the new attach entry points in the dynamic-FFI bindings, the way the L0/L1/L2 ValueTrace
pipeline is already wrapped for Python/C++/Node
([data-flow-tracing-plan.md:237](data-flow-tracing-plan.md#L237)) — opaque-handle pattern,
each gated by a `make dataflow-<lang>-test` lane (host or docker) as the other seven bindings
already are for the pure helpers.

**Exit criteria:** at least Python/C++/Node expose live-attach data-flow capture over an
attached pid; `make dataflow-<lang>-test` covers it; the seven docker-gated bindings wrap at
least the native path in their pinned toolchain images.

---

## Risks and open points

- **F1 is a real bet, not a certainty.** Its correctness rests on the emulator and the real
  CPU agreeing on a straight-line block. They do for OS-free integer/vector code (the whole
  reason the cross-validation oracle exists), but the spike is a genuine go/no-go — if it
  fails, the base plan's single-step tier is the permanent path and F2/F5 (which build on the
  replay) fall away with it.
- **F3 is the safest big win** and the least coupled — it depends on nothing in F1, needs no
  emulator, and gives a qualitatively new capability (targeted field-watch) at near-zero
  perturbation. Consider landing it **before** F1 if perturbation on a specific object, rather
  than whole-region tracing, is the pressing user need.
- **F4 is hard-gated on a runtime feed** the .NET in-proc EventListener does not currently
  surface; it may need an out-of-process EventPipe consumer, which is its own lift.
- **F5's hardware is absent** on most cloud/GitHub-hosted VMs (Intel PT needs Intel bare
  metal); it is a ceiling to demonstrate on the right box, not a default path.
- **The deadlock residual persists** across all of these — block-step and replay *shrink* the
  perturbation window; only a fully out-of-band stream (F5) or a paused/single-threaded target
  removes it entirely.

---

## Recommended first milestone

**F3 (hardware data-watchpoint targeted mode)** if the immediate need is watching specific
data with minimal perturbation — it is cheap, decoupled, and safe. **F1's spike increment**
if the need is making whole-region JIT tracing practical — it is the highest-leverage item but
must prove itself first. Both are independent of each other, so they can proceed in parallel;
defer F2/F5 until the F1 spike is green, and treat F4/F6/F7 as runtime-/scope-/binding-gated
follow-ons.
