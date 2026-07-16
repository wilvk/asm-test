# asm-test — live-attach data-flow: post-implementation follow-up plan

The next steps **after** [live-attach-dataflow-plan.md](../archive/plans/live-attach-dataflow-plan.md) lands
its native + JIT-aware live-attach value producer and the asmspy Data flow window. That plan
deliberately ships the **conservative core**: direct single-step value capture (proven, but
it perturbs the stepped thread), scoped to a region, with managed memory def-use left GC-
uncanonicalized. This plan lands the improvements that were held out of the critical path —
each one either **reduces perturbation**, **raises fidelity on managed targets**, or
**broadens the target set** — ordered so the highest-value, lowest-risk work comes first.

> Status *(reconciled 2026-07-17, after F1 increment 2)*: **F3 LANDED (asmspy `--watch`); F1
> increments 1+2 LANDED (pure-method block-step tier + vector breadth — the YMM/ZMM carryover is
> CLOSED, with AVX-instruction replay recorded as an upstream Unicorn/QEMU gate); F4 increments
> 1+2 LANDED — its exit criterion is MET on a live attach; F2/F5–F7 open.** The two bets this plan was written around
> have both been settled in its favour. F1 was the marquee item and carried a spike increment
> because its value-reconstruction claim was the one genuinely **unproven** bet in the whole
> design — that spike came back **GO** (byte-identical to single-step by literal `memcmp`, ~6×
> fewer stops), so the design holds and F2/F5, which build on the replay, did not fall away with
> it. F3 was the cheapest high-value item (a near-zero-perturbation targeted mode from a
> one-line DR7 change) and landed as `asmspy --watch`. **F4 is DONE for its stated criterion**:
> two spikes settled its open questions (a profiler CAN attach to a process we did not launch;
> the `step` stamp MUST come from the profiler-sampled S0, because the "counter freezes across
> the fence" assumption measured **FALSE**), increment 1 landed the join, and increment 2 chained
> the same-window GC collapse it landed with — `make docker-gccanon-attach`, 37 assertions, each
> half proven by a negative control. F5 remains **hardware-gated** on bare-metal Intel PT and
> self-skips; the dev boxes are AMD, so it is a ceiling to demonstrate elsewhere, not a default
> path. House rule unchanged: foreign targets are never killed; every tier self-skips where its
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

## F1 — Block-step + emulator-replay value optimization *(**increments 1+2 LANDED** — increment 2 closes the vector-breadth carryover 2026-07-17; **HOST-gated, not CI-gatable**; F2 unblocked)*

> **UPDATE 2026-07-17 — INCREMENT 2 LANDED: vector breadth (the YMM/ZMM carryover) is CLOSED,
> and the honest boundary is narrower than the carryover assumed.** `make docker-dataflow-attach`
> / `make dataflow-blockstep-test` — **47/47**, deterministic (60/60 repeat runs in-container,
> 40/40 host). The carryover read "YMM/ZMM boundary seeding"; what is actually achievable was
> **measured, not assumed**, and it splits three ways:
>
> | width | seeded into the replay? | replayed? | captured into the trace? |
> |---|---|---|---|
> | **XMM 128** | **YES** (all 16, each verified by read-back) | **YES** — legacy SSE | yes |
> | **YMM 256** | no — unreachable by construction | **NO** — upstream-blocked | **YES**, real, from silicon |
> | **ZMM 512** (incl. zmm16-31) | no — unreachable by construction | **NO** — upstream-blocked | **YES**, real, from silicon |
>
> **AVX-instruction replay is an UPSTREAM GATE, and this was checked rather than inherited.**
> Unicorn **2.1.3** (the latest release, 2025-03-07) was **built from source and probed**: it
> still vendors **QEMU 5.0.1**, QEMU only gained AVX TCG in **7.2** (`decode-new.c.inc` /
> `emit.c.inc`, absent from Unicorn's tree), and `vaddps ymm` / `vaddps zmm` still return
> `UC_ERR_INSN_INVALID`. So this is a capability that does not exist in any release — not an
> installable dependency, and the one case CLAUDE.md calls a legitimate stop. **The pin is
> deliberately NOT bumped**: 2.1.3's only relevant fix (the ZMM register file) is unreachable
> anyway, because with VEX/EVEX gated off the replay no replayed instruction can read YMM-upper
> or ZMM. Seeding them would be unobservable, hence untestable, hence vacuous — so the tier
> seeds XMM only and takes YMM/ZMM **values** from hardware instead.
>
> **Two SILENT-WRONGNESS traps were found by measurement; `UC_ERR_OK` proves nothing.**
> 1. **VEX-128 is mis-executed as legacy SSE by every released Unicorn.** Differential against
>    this silicon, same inputs: real `vpaddd xmm0,xmm1,xmm2` → `11 22 33 44`; Unicorn →
>    `110 220 330 440` (it drops `VEX.vvvv`, runs the 2-operand form, and skips the mandatory
>    upper-128 zeroing) — **returning `UC_ERR_OK`**. It does not fail; it lies. So the
>    replayability gate is an **encoding-level** rule (any VEX/EVEX prefix byte), which also
>    conservatively gates VEX-GP (BMI) that Unicorn *does* run correctly: over-gating costs only
>    the perturbation win, under-gating costs correctness.
> 2. **Capstone's AVX metadata cannot carry that gate.** `vpbroadcastq zmm0,xmm0` decodes with
>    correct mnemonic and operands yet `cs_regs_access` reports it touching **no registers** and
>    it belongs to **no** `X86_GRP_AVX/AVX2/AVX512` group. A metadata-based gate would pass EVEX
>    straight into a mis-executing replay. The encoding rule is exact on x86-64 (C4/C5 = VEX,
>    62 = EVEX) and is validated on 22 hand-checked encodings.
> 3. **Unicorn 2.0.1's ZMM registers are unplumbed**: `uc_reg_write(ZMM0)` returns `UC_ERR_OK`
>    and stores **nothing** (reads back zeros, not even aliasing XMM0/YMM0). Fixed in 2.1.x.
>    Hence every seed is verified by **read-back**, and `info.uc_vec_width` reports what the
>    linked library actually holds rather than what its enum advertises.
>
> **The bug this increment exists to kill, reproduced first.** A region whose input arrives in a
> vector register replayed against a **zeroed** Unicorn vector file, and the GP-only canary was
> blind to it whenever the value reached **memory** rather than a GP register. Measured on the
> `sse_live_in` fixture (live-in `xmm0=7`, `xmm1=5`; region stores `xmm0` to memory):
> oracle **12**, seeded replay **12**, unseeded replay **0** — with `value_valid=1`, `rc=OK`,
> `truncated=0`. A confidently-reported wrong value. The suite asserts that divergence as a
> **negative control**, and a third run shows the new vector canary catching it (`truncated`).
>
> **Also fixed: a pre-existing increment-1 bug that made this lane a 27% coin flip on bare
> metal.** `process_vm_readv` is **atomic per iovec**, and the replay's stack window
> `[rsp-0x1000, rsp+0x2000)` routinely overruns the top of the tracee's `[stack]` VMA — so the
> whole read failed on a healthy tracee and increment 1 "recovered" by **zeroing the window**,
> after which the replayed `ret` popped 0 and the capture died `UC_ERR_FETCH_UNMAPPED`. It fired
> on **11/40 container runs and 0/40 host runs** (`docker-dataflow-attach` was already red on
> main for this reason, reporting `rc=-4`); it survived increment 1 precisely because the lane
> self-skips on GitHub's BTF-masked runners, so nothing ever exercised it. Reads are now
> per-page, and `opts.stack_hi_pad` forces the window 2 MiB past the stack top to make the case
> **deterministic** instead of a layout coin flip: 60/60 in-container after the fix. Relatedly, a
> Unicorn fault mid-replay now returns `FAULT` + `truncated` instead of falling through to the
> `ETRACE` initializer — a **divergence that used to masquerade as the "no ptrace here"
> self-skip**.
>
> **Every claim carries a negative control, and the suite is mutation-proven** — 8 mutants of the
> tier, each caught by exactly the assertions that claim the corresponding capability and no
> others: vector values never captured → 41,46; XSTATE drops `YMM_Hi128` → 41,46; drops
> `ZMM_Hi256` → 46 only; seeding a no-op that claims success → 29,31; VEX/EVEX gate disabled →
> 12 checks; stack read all-or-nothing → 34; vector canary removed → 33; Unicorn fault → ETRACE
> → 42,47.
>
> **No public struct or ABI changed.** Vector values ride the `wide[]` side buffer that
> `asmtest_valtrace.h` already documents for exactly this ("wider values (XMM/YMM, up to 64
> bytes) spill to the valtrace's `wide[]`"); `at_val_rec_t` already carried `wide`/`wide_off`/
> `size`. Only the **producer-local** opts/info structs grew (`region_off`, `stack_hi_pad`, the
> vector test hooks; `vec_width`/`vec_nregs`/`uc_vec_width`/`vec_seeded`), and they cross no FFI
> boundary — the tier ships no header and its suite re-declares them.
>
> **Carryover:** F2 (impure via record-inject) — now **unblocked**; the replay it rides on is
> unchanged and the fallback it must displace is the same single-step path. Two bounded,
> honestly-scoped follow-ons: allowlisting VEX-GP (BMI) back onto the replay after verifying all
> 13 (`andn` is already measured faithful), and revisiting the pin the day QEMU's AVX TCG reaches
> a Unicorn release — at which point YMM/ZMM seeding becomes *observable* and this increment's
> deliberate XMM-only choice should be revisited.

> **UPDATE 2026-07-17 — F1 increment 1 CANNOT be CI-gated on GitHub runners, and now we know why.**
> It was an orphan until 2026-07-17 (`dataflow-blockstep-test` was defined at
> [mk/dataflow.mk](../../../mk/dataflow.mk) but omitted from the `dataflow-test` aggregate, and
> `grep blockstep .github/` returned zero). It is now chained into the aggregate, and the aggregate
> runs in CI via `docker-dataflow-attach` — but the block-step tier **self-skips there**:
>
> ```
> # SKIP live block-step/replay: ptrace unavailable (seccomp/yama) or
>        PTRACE_SINGLEBLOCK non-functional (BTF masked)
> ```
>
> GitHub's runners are **VMs whose hypervisor masks BTF/DEBUGCTL**, so `PTRACE_SINGLEBLOCK` is
> non-functional. This is not a container/seccomp issue — the lane already runs
> `seccomp=unconfined` and it still skips. Note `test_dataflow_ptrace`'s 91 assertions **do** run
> there: they drive `PTRACE_SINGLEBLOCK`'s sibling `PTRACE_SINGLESTEP`, which is not masked. Only
> block-step is affected.
>
> **So F1 increment 1's 22/22 is gated on BARE METAL ONLY** (verified on a Zen 5 host). The CI gate
> allows this one skip **by name** and fails on every other — a blanket allowance would re-open the
> vacuous-green hole the gate exists to close. Anyone wanting real CI coverage for F1 needs a
> bare-metal runner; that is the same constraint the AMD/PT lanes carry, arrived at from a different
> direction.

> **UPDATE 2026-07-15 — INCREMENT 1 LANDED.** The spike is promoted to a real tier `src/dataflow_blockstep.c`:
> `PTRACE_SINGLEBLOCK` per taken branch → full `GETREGS` boundary snapshot → Unicorn replay of each
> straight-line block (guest mapped at real addresses, memory faulted via `process_vm_readv`, a landing pad
> for the region-exit `ret`); a **purity static-scan** routes pure methods to replay and impure ones to the
> single-step fallback; a **coherence canary** truncates on any replay-input divergence; oracle validation is
> rsp-relative. New `examples/test_dataflow_blockstep.c` — **22/22** on host (real ptrace + Unicorn 2.0.1):
> block-step+replay is **byte-identical** (literal `memcmp`) to single-step on `loop_poly` (303→50 stops,
> 6.06×) and `mem_chain` (6→1, 6.00×), impure `cpuid` falls back, the canary truncates. ~~Carryover: YMM/ZMM
> boundary seeding~~ — **CLOSED by increment 2, see above** (and note this 22/22 was, on bare metal, a ~27%
> coin flip that increment 2 diagnosed and fixed); F2 (impure via record-inject) remains.
> Increment-0 spike evidence retained below.

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
  free reuse), giving a real ground-truth register snapshot at every branch. *(Both halves are
  now landed: `GETREGS` in increment 1, `NT_X86_XSTATE` in increment 2 — the latter paying for
  the read only where the region's scan says it touches vector state. Note the XSTATE component
  offsets MUST come from `CPUID.0xD`; they are not constants, and this Zen 5's layout
  (576/832/896/1408) differs from the commonly-quoted one because its XCR0 omits MPX.)*
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

## F2 — Record-and-inject OS-interaction fidelity *(planned — **UNBLOCKED 2026-07-17**: F1 increments 1+2 have landed)*

> **F1 increment 2 does not change what F2 must do**, and is worth reading before starting it for
> three reasons. (1) The **single-step fallback F2 exists to displace now has two callers**, not
> one: `!scan.pure` (a syscall already retired on the real cpu by the boundary) and
> `!scan.replayable` (a VEX/EVEX encoding no released Unicorn executes). F2 addresses only the
> **first** — an impure AVX region stays single-stepped, because record-and-inject cannot make
> Unicorn execute an instruction it has no decoder for. Do not report F2 as removing "the
> fallback". (2) `info.reason` now distinguishes them (a mnemonic vs `"avx"`), so F2's tests can
> assert *which* gate fired. (3) The record-and-inject path must keep the **vector** boundary
> state coherent across an injected syscall exactly as it does the GP file — the canary already
> covers XMM, and `xstate_read` gives the full-width snapshot to inject from.

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

## F4 — Live GC-move canonicalization for managed memory def-use *(**INCREMENTS 1+2 LANDED 2026-07-16** — exit criterion met on a live attach; increment 1's one open limitation is now CLOSED, see below)*

> **UPDATE 2026-07-16 — INCREMENT 1 LANDED; the exit criterion is MET.**
> `make docker-gccanon-attach` (increment 1's lane was 7/7; the same command now also runs
> increment 2's `--selftest` and its 2- and 3-GC-per-window phases — 37 assertions, see the
> limitation block below), `examples/gccanon_attach/`: a plain `dotnet` victim (no
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
> **~~OPEN LIMITATION~~ — CLOSED 2026-07-16 by INCREMENT 2 (the composition).** The limitation was
> real and is now fixed, reproduced as a failing case first. It read: the region-gated step counter
> freezes across the call-out, so **every GC in one call-out window is stamped with the same S0**,
> `asmtest_gcmove_canon` applies at most one relocation per step-group, and **two GCs in one window
> collapse into a single batch and under-forward a twice-moved object** (A→B→C canonicalizes to B
> while the load reads C). Increment 1 choreographed exactly one GC per window and checked it via
> `gc_seq`; that choreography was the least representative part of the lane.
>
> **The fix is CHAINING, not separating** — and the freeze that causes the problem is what licenses
> it. `step` is the only ordering information the trace carries, so two GCs sharing one means **no
> record can lie between them**: every record is before *all* the window's moves or after *all* of
> them, and the correct canonical address for a pre-window record is the **composition** of the
> moves (A→B→C ⇒ A→C). That is now **measured, not assumed** (`ok 9`): the profiler samples the live
> trace's `steps_len` *and* `recs_len` at the start **and** end of every fence, and for the window's
> GCs — selected by `gc_seq`, so the victim's out-of-window fragmentation GCs cannot pollute it —
> all of them are identical, so nothing was appended across the fences **or the gaps between them**.
> (Structurally it could not be otherwise: [dataflow_ptrace.c](../../../src/dataflow_ptrace.c) runs
> a call-out via int3 + `PTRACE_CONT` and records nothing over the helper, so the producer is blocked
> in `waitpid` for the whole window. The samples travel a 20 µs mirror, so they corroborate that
> structural fact rather than replace it.)
>
> **`asmtest_gcmove_canon` is UNTOUCHED.** Its one-relocation-per-batch rule is load-bearing, not an
> oversight — it stops a relocated address being re-relocated by a *sibling* of the same GC whose old
> span the compaction slid a new range over. Relaxing it or iterating within a batch would fix the
> two-GC case by breaking the one-GC case. So the fix lives on the **feed** side, where the collapse
> is caused: `gccanon_compose` (in the lane's tracer) pre-composes each boundary's GCs in `gc_seq`
> order into ONE batch whose ranges are **already chained A→C**, and hands that to the shipping
> transform. Composed OLD ranges stay disjoint because part 2 subtracts the earlier GC's **domain**
> (subtracting its *image* instead both breaks disjointness and deviates — a mutant that does so is
> caught by the oracle on 884/691,200 probes). Partial overlaps — an earlier range's image landing
> only *partly* inside a later one, routine because the runtime coalesces adjacent objects into one
> range and the next GC may split the run — are **split exactly**, since a delta is uniform within a
> range; no truncation is needed. The conservative-truncation path — for a GC whose own old ranges
> overlap, which would break the composition's proof — drops **both** sides, a missing edge rather
> than a guessed one. It **never fired on live data** (the feed's per-GC old ranges were disjoint
> across ~90k ranges), so it is covered by a dedicated `--selftest` case instead: an independent
> review found the first cut compared only *adjacent* ranges, which silently let a long range's
> non-adjacent overlapper survive and **forward** the doubly-claimed address (`0x55` → `0x9005`) —
> a false edge, the exact thing the policy forbids. Fixed with a running max of ends; the new case
> fails against the old code.
>
> **The spec, and why this is not a second opinion about GC semantics:** the composed batch must make
> `asmtest_gcmove_canon` compute exactly what it *would* have computed for the same GCs had the
> counter given them distinct boundaries. That is an identity, and it is checked against the shipping
> transform itself — `build_separated` re-stamps the same GCs into a scaled step space where canon's
> own multi-batch walk chains them the long way. It agrees on the live feed (`ok 11`) and, in
> `gccanon_tracer --selftest` (pure, no dotnet/ptrace), on **691,200 probes** over 300 randomized
> feeds of up to **5 GCs at one boundary** — every address of a synthetic heap, both sides of the
> boundary — with the collapsed feed disagreeing on 16,250/25,600 as that oracle's own negative
> control. Three mutants (drop part 2; subtract the image; ignore a split) are all caught.
>
> **What the composition does NOT fix** (unchanged from increment 1, and inherent to the transform's
> address-identity model): a pre-window record touching memory that a GC then slides a *live* object
> into aliases that object. That exists in the one-GC case too and is retired only by real object
> identity (`GCBulkType`/`Node`/`Edge`), which
> [asmtest_valtrace.h](../../../include/asmtest_valtrace.h#L316) explicitly defers.

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

> **MET 2026-07-16 for the first clause** — `docker-gccanon-attach`: the def-use edge survives a
> real compaction on a live attach (negative control: missing without the transform), with no false
> alias forged. **Increment 2 removed the caveat** ("exactly one GC per call-out window"): the lane
> now runs a pure `--selftest` plus one phase per `GCCANON_PHASES` value (**1, 2 and 3** compacting
> GCs choreographed into ONE window), and at 2 and 3 the object really is moved that many times
> — `0x…d700 → 0x…7ed8 → 0x…5a98 → 0x…3658` on 44/44 windows — with the collapse reproduced as a
> FAILING case (the edge still missing, under-forwarded to the intermediate) before the chained feed
> restores it at the true final address. 37 assertions green over two consecutive runs. The
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
