# asm-test — live-attach data-flow: post-implementation follow-up plan

The next steps **after** [live-attach-dataflow-plan.md](../archive/plans/live-attach-dataflow-plan.md) lands
its native + JIT-aware live-attach value producer and the asmspy Data flow window. That plan
deliberately ships the **conservative core**: direct single-step value capture (proven, but
it perturbs the stepped thread), scoped to a region, with managed memory def-use left GC-
uncanonicalized. This plan lands the improvements that were held out of the critical path —
each one either **reduces perturbation**, **raises fidelity on managed targets**, or
**broadens the target set** — ordered so the highest-value, lowest-risk work comes first.

> Status *(reconciled 2026-07-16)*: **F3 LANDED (asmspy `--watch`); F1 increment 1 LANDED
> (pure-method block-step tier); F4 UNBLOCKED and now the top candidate; F2/F5–F7 + F1 vector
> breadth open.** The two bets this plan was written around have both been settled in its
> favour. F1 was the marquee item and carried a spike increment because its
> value-reconstruction claim was the one genuinely **unproven** bet in the whole design — that
> spike came back **GO** (byte-identical to single-step by literal `memcmp`, ~6× fewer stops),
> so the design holds and F2/F5, which build on the replay, did not fall away with it. F3 was
> the cheapest high-value item (a near-zero-perturbation targeted mode from a one-line DR7
> change) and landed as `asmspy --watch`. **F4 is no longer runtime-gated** — its EventPipe
> blocker was disproved and the profiler feed it needs is proven and shipping in the taint
> tier, leaving wiring plus one honest spike (attaching a profiler to a process we did not
> launch); see F4 below. F5 remains **hardware-gated** on bare-metal Intel PT and self-skips;
> the dev boxes are AMD, so it is a ceiling to demonstrate elsewhere, not a default path.
> House rule unchanged: foreign targets are never killed; every tier self-skips where its
> substrate is absent.

---

## Goals and non-goals

**Goals** — take the shipped single-step live-attach tier and: (F1/F2) cut its perturbation
by an order of magnitude via block-step + emulator replay, escalating to record-and-inject
for OS-interacting code; (F3) add a near-zero-perturbation hardware-watchpoint targeted mode;
(F4) make managed **memory** def-use survive GC compaction on a live attach; (F5) add the
least-perturbing PT-derived value path where the silicon allows; (F6) broaden toward
whole-process; (F7) expose it through the language bindings.

**Non-goals** — production whole-process **taint** over managed code (that remains the
DynamoRIO tier, [dynamorio-taint-tier-plan.md](../archive/plans/dynamorio-taint-tier-plan.md)); this plan
stays on the out-of-band observe-don't-instrument side. Windows / macOS attach (Linux
x86-64 first, AArch64 where the primitive exists).

---

## F1 — Block-step + emulator-replay value optimization *(**increment 1 LANDED 2026-07-15** — pure-method value tier; F2/vector breadth planned)*

> **UPDATE 2026-07-15 — INCREMENT 1 LANDED.** The spike is promoted to a real tier `src/dataflow_blockstep.c`:
> `PTRACE_SINGLEBLOCK` per taken branch → full `GETREGS` boundary snapshot → Unicorn replay of each
> straight-line block (guest mapped at real addresses, memory faulted via `process_vm_readv`, a landing pad
> for the region-exit `ret`); a **purity static-scan** routes pure methods to replay and impure ones to the
> single-step fallback; a **coherence canary** truncates on any replay-input divergence; oracle validation is
> rsp-relative. New `examples/test_dataflow_blockstep.c` — **22/22** on host (real ptrace + Unicorn 2.0.1):
> block-step+replay is **byte-identical** (literal `memcmp`) to single-step on `loop_poly` (303→50 stops,
> 6.06×) and `mem_chain` (6→1, 6.00×), impure `cpuid` falls back, the canary truncates. Carryover: YMM/ZMM
> boundary seeding, F2 (impure via record-inject). Increment-0 spike evidence retained below.

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

## F3 — Hardware data-watchpoint targeted mode *(**LANDED 2026-07-15** — asmspy --watch; AArch64 analog planned)*

> **UPDATE 2026-07-15 — LANDED as `asmspy --watch`.** `asmspy --watch <pid> <addr|func+off> [--rw] [--len=N]
> [--json]` arms an x86 data watchpoint (DR0-3; DR7 R/W=01 write / 11 r/w, LEN 1/2/4/8) via `PTRACE_POKEUSER`,
> `PTRACE_CONT`s the target, and at each `#DB` reports read/write + the value (`process_vm_readv`) + the
> faulting PC resolved to function+offset. **Per-thread arming across `/proc/<pid>/task/*` + `PTRACE_O_TRACECLONE`**
> (the spike's mandated fix) — verified capturing writes on a WORKER thread (tid ≠ leader). New
> `asmspy_engine_watch` lives in `cli/`; `src/ptrace_backend.c` untouched. Two-phase detach so the target
> survives; self-skips on POKEUSER-refused / qemu / non-x86-64. `make docker-cli` cli-smoke PASS. Carryover:
> the AArch64 `NT_ARM_HW_WATCH`/`DBGWCR` analog. Increment-0 spike evidence retained below.

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

## F4 — Live GC-move canonicalization for managed memory def-use *(**INCREMENT 1 LANDED 2026-07-16** — exit criterion met on a live attach; one open limitation, see below)*

> **UPDATE 2026-07-16 — INCREMENT 1 LANDED; the exit criterion is MET.**
> `make docker-gccanon-attach` (7/7, `examples/gccanon_attach/`): a plain `dotnet` victim (no
> `CORECLR_*` env) → an attach-mode `MovedReferences2` profiler → ranges stamped with a
> profiler-sampled **S0** → the **shipping** scoped L0 producer
> (`asmtest_dataflow_ptrace_attach_jit`) over one managed region invocation →
> `asmtest_gcmove_canonicalize` → `asmtest_defuse_build`. It links the real tier and the real
> transform, not copies, and modifies no shipping source.
> `asmtest_gcmove_canonicalize` now has a **live caller** (previously: its unit test only).
>
> The proof is the **negative control** (`raw_edge=0, canon_edge=1`): WITHOUT canonicalization the
> store(step 5) → load(step 7) def-use edge is **missing** from the live trace — the GC-aliasing
> bug is real and the capture provokes it — and WITH canonicalization the edge **appears** at the
> canonical address. The lane also asserts no false alias is forged for an object reusing the
> vacated slot.
>
> **Two prior spikes settled the design** and are recorded in
> [f4-attach-profiler-probe-findings.md](../analysis/f4-attach-profiler-probe-findings.md) (the
> feed: a profiler CAN attach to a process we did not launch) and
> [f4-gc-fence-freeze-probe-findings.md](../analysis/f4-gc-fence-freeze-probe-findings.md) (the
> stamp: `step` MUST come from the profiler-sampled S0, because the "step counter freezes across
> the fence" assumption measured FALSE — single-stepping a futex-blocked thread is what un-blocks
> it).
>
> **The ~19.8 s GC stall that probe found never materialised here, for a structural reason worth
> recording:** the region's `call` becomes a **call-out step-over** (`dfp_run_to` = int3 +
> `PTRACE_CONT` + signal forwarding), so the traced thread runs at **native speed** through the GC
> and parks in preemptive mode — no hijack, no `SIGRTMIN`. The stall needs a *continuously
> single-stepped* thread, which this scoped tier never does. **Region scoping is what makes F4
> reachable, not an obstacle.**
>
> **OPEN LIMITATION (the honest next question).** The region-gated step counter freezes across the
> call-out, so **every GC in one call-out window is stamped with the same S0**, and
> `asmtest_gcmove_canon` applies at most one relocation per step-group. **Two GCs in one window
> would silently collapse into a single batch and under-forward a twice-moved object** (A→B→C
> canonicalizes to B while the load reads C), presenting as a missing edge that looks like a
> transform bug. The lane choreographs exactly one GC per window and `ok 7` **checks** that via a
> `gc_seq` field rather than assuming it — but that choreography is the least representative part
> of the lane, and a high-GC-rate target could put two GCs in one window. Separating them needs a
> finer boundary than a frozen region-gated counter can express.

> **UPDATE 2026-07-16 — F4's stated blocker is obsolete, and the plan's own risk note was
> wrong.** This item was written as "hard-gated on a runtime feed the .NET in-proc
> EventListener does not currently surface; it may need an out-of-process EventPipe consumer,
> which is its own lift." The `EventListener` limitation is real, but the conclusion drawn
> from it was not. A deep-research investigation plus a coexistence probe (GO, 2026-07-14)
> found a native in-process mechanism that **bypasses EventPipe entirely**: an
> `ICorProfilerCallback4::MovedReferences2` profiler, which receives the exact per-range
> `{old, new, len}` triples at a **fully-suspended-EE GC fence**. That fence is a strictly
> better coherence position than any event-stream consumer could reach, because no mutator
> thread can race the remap — which also retires the "a missed move event silently aliases"
> concern in the parent plan's risk list for this route.
>
> It is not speculative. The DR taint tier's Increment 7 **ships it**: the profiler feeds
> `at_gc_remap_live` ([dataflow_dr_client_inlined.c:732](../../../src/dataflow_dr_client_inlined.c#L732))
> through a POSIX-shm handshake and remapped 60,021 real ranges on a live compacting GC under
> DynamoRIO, with `make docker-gcprofiler-probe` proving profiler/DR coexistence. Findings:
> [gc-move-range-extraction-findings.md](../analysis/gc-move-range-extraction-findings.md).
>
> **So F4 is now "point a proven feed at a landed transform."** Both halves exist; only the
> join is missing. Note the two tiers need genuinely different plumbing from the same
> profiler, so this is not a copy of the DR path: the DR client is *in-process* with the
> target and remaps its live shadow at the fence (hence the DR-API-free constraint recorded
> there), whereas this tier is *out-of-process* and post-pass — it consumes triples stamped
> with the value-trace step boundary the compaction takes effect at, then canonicalizes a
> captured trace before `asmtest_defuse_build`. The realistic new risk is not the feed but
> **attaching a profiler to a process asmspy did not launch**: `CORECLR_ENABLE_PROFILING` is
> read at startup, so a live-attach target either needs the .NET attach-profiler path or the
> capture is limited to launched targets — that, not EventPipe, is the honest open question
> to spike first.

The base plan leaves managed **memory** def-use GC-uncanonicalized: a store before a
compaction (old address) and its matching load after (new address) look unrelated, so the
edge is lost, and an object reusing the vacated address forges a false edge. The **pure
transform is already landed** — `asmtest_gcmove_canonicalize`
([asmtest_valtrace.h:353](../../../include/asmtest_valtrace.h#L353)),
[src/dataflow_gcmove.c](../../../src/dataflow_gcmove.c), whose only callers today are its unit
test [examples/test_dataflow_gcmove.c](../../../examples/test_dataflow_gcmove.c) — what is
missing is the live feed.

- Extract concrete `{old_base, new_base, len}` triples **from an in-process
  `MovedReferences2` profiler** (per the update above — *not* from EventPipe, as originally
  written), stamped with the value-trace step boundary the compaction takes effect at, and run
  `asmtest_gcmove_canonicalize` on the captured trace before `asmtest_defuse_build`.
- Full object identity via `GCBulkType`/`Node`/`Edge` is a further step.

**Exit criteria:** a managed value's **memory** def-use survives an induced GC compaction
without pre/post-move aliasing on a **live attach** (not just the landed pure unit test);
a value is attributed to the correct method+version after a tiered re-JIT (already landed for
control, re-validated for the live data path).

> **MET 2026-07-16 for the first clause** — `docker-gccanon-attach` ok 3/4/5/6: the def-use edge
> survives a real compaction on a live attach (negative control: missing without the transform),
> with no false alias forged. Caveat as above: exactly one GC per call-out window. The
> method+version clause rides the already-landed Increment-3 attribution and is not re-proven by
> this lane.

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
off to [dynamorio-taint-tier-plan.md](../archive/plans/dynamorio-taint-tier-plan.md) (whose in-band L0 value
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
- **F4 is no longer gated on a runtime feed** *(corrected 2026-07-16; this bullet previously
  read "hard-gated … may need an out-of-process EventPipe consumer, which is its own lift")*.
  The EventPipe premise was right and the conclusion was wrong: an in-process
  `MovedReferences2` profiler delivers the exact triples at a suspended-EE GC fence, is proven
  to coexist with DR, and already ships in the taint tier. F4's remaining risk moved to a
  different place — attaching a profiler to a process we did **not** launch
  (`CORECLR_ENABLE_PROFILING` is start-up-read), which is what to spike first. See the F4
  update above.
- **F5's hardware is absent** on most cloud/GitHub-hosted VMs (Intel PT needs Intel bare
  metal); it is a ceiling to demonstrate on the right box, not a default path.
- **The deadlock residual persists** across all of these — block-step and replay *shrink* the
  perturbation window; only a fully out-of-band stream (F5) or a paused/single-threaded target
  removes it entirely.

---

## Recommended first milestone

> **UPDATE 2026-07-16 — both original recommendations are done; the milestone below is
> superseded.** F3 landed as `asmspy --watch` and F1's spike came back GO and was promoted to
> the `dataflow_blockstep.c` tier. **The recommended next milestone is now F4**, on three
> grounds: its blocker turned out to be imaginary (the feed is proven and shipping in the
> taint tier), it is the last correctness gap in the shipped live-attach tier rather than an
> optimization — managed memory def-use is silently GC-aliased today, which is the one place
> this tier can be *wrong* rather than merely slow or truncated — and the transform half is
> already landed and unit-tested. Sequence it as: spike the attach-a-profiler-to-a-running-pid
> question first (it is the only real unknown left, and a NO-GO there scopes F4 to launched
> targets rather than killing it), then wire triples → `asmtest_gcmove_canonicalize` →
> `asmtest_defuse_build`. After F4, **F7** (bindings) is the cheapest remaining item and is
> pure software; **F2** extends F1's replay to impure methods; **F6** is scope-gated; **F5**
> waits on silicon this project does not have.
>
> The original recommendation is retained below for provenance.

**F3 (hardware data-watchpoint targeted mode)** if the immediate need is watching specific
data with minimal perturbation — it is cheap, decoupled, and safe. **F1's spike increment**
if the need is making whole-region JIT tracing practical — it is the highest-leverage item but
must prove itself first. Both are independent of each other, so they can proceed in parallel;
defer F2/F5 until the F1 spike is green, and treat F4/F6/F7 as runtime-/scope-/binding-gated
follow-ons.
