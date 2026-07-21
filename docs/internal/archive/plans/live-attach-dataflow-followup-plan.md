# asm-test — live-attach data-flow: post-implementation follow-up plan

The next steps **after** [live-attach-dataflow-plan.md](live-attach-dataflow-plan.md) lands
its native + JIT-aware live-attach value producer and the asmspy Data flow window. That plan
deliberately ships the **conservative core**: direct single-step value capture (proven, but
it perturbs the stepped thread), scoped to a region, with managed memory def-use left GC-
uncanonicalized. This plan lands the improvements that were held out of the critical path —
each one either **reduces perturbation**, **raises fidelity on managed targets**, or
**broadens the target set** — ordered so the highest-value, lowest-risk work comes first.

> **Status (2026-07-21): everything this plan owns is LANDED or brief-owned — plan archived.**
> F5's hardware gate has been met: commit `c7b4ef7` +
> [intel-hardware-validation.md](../../intel-hardware-validation.md) record `make dataflow-pt-live`
> **29/29, stable ×3**, on a bare-metal Intel Core i7-8559U, with the F5 replay **byte-identical
> to the emulator L0 at zero single-steps** on both walks — the plan's own exit criterion. The
> brief [dataflow-pt-replay-tier.md](../../implementations/dataflow-pt-replay-tier.md) is ✅ 5/5.
> The remaining carryovers are brief-owned: F3's arm64 live validation by
> [asmspy-aarch64-support.md](../../implementations/asmspy-aarch64-support.md) (◐ 5/7); F7's
> def-use/slice + code-image carryover CLOSED by
> [dataflow-bindings-slice-codeimage.md](../../implementations/dataflow-bindings-slice-codeimage.md)
> ✅ 4/4. The 2026-07-17 reconciliation below is retained for provenance.

> Status *(reconciled 2026-07-17, after F1 increment 2)*: **F3 LANDED (asmspy `--watch`); F1
> increments 1+2 LANDED (pure-method block-step tier + vector breadth — the YMM/ZMM carryover is
> CLOSED, with AVX-instruction replay recorded as an upstream Unicorn/QEMU gate); F4 increments
> 1+2 LANDED — its exit criterion is MET on a live attach; **F2 increment 1 LANDED — the syscall
> half of record-and-inject, exit criterion MET**; **F6 increment 1 LANDED — the gap barrier,
> with whole-process DECLINED on measured arithmetic rather than deferred**; **F7 LANDED — all
> ten bindings, every one on a real attach.** ~~So F5 alone is open, and only on hardware.~~
> *(Superseded 2026-07-21: F5's oracle match ran green on bare-metal Intel PT — see the status
> note above.)* The two bets this plan was written around
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
> half proven by a negative control. ~~F5 remains **hardware-gated** on bare-metal Intel PT and
> self-skips; the dev boxes are AMD, so it is a ceiling to demonstrate elsewhere, not a default
> path.~~ *(Demonstrated 2026-07-21 on the bare-metal Core i7-8559U — commit `c7b4ef7`;
> off-PT hosts still self-skip.)* House rule unchanged: foreign targets are never killed; every
> tier self-skips where its substrate is absent.

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

## F1 — Block-step + emulator-replay value optimization *(**increments 1+2 LANDED** — increment 2 closes the vector-breadth carryover 2026-07-17; **HOST-gated, not CI-gatable**; F2 increment 1 has since LANDED on top)*

> **UPDATE 2026-07-17 — INCREMENT 2 LANDED: vector breadth (the YMM/ZMM carryover) is CLOSED,
> and the honest boundary is narrower than the carryover assumed.** `make docker-dataflow-attach`
> / `make dataflow-blockstep-test` — **65/65** (47 at first landing; +18 after the adversarial
> review round recorded below), deterministic. The carryover read "YMM/ZMM boundary seeding";
> what is actually achievable was **measured, not assumed**, and it splits three ways:
>
> | width | seeded into the replay? | replayed? | captured into the trace? |
> |---|---|---|---|
> | **XMM 128** | **YES** (all 16 + **MXCSR**, each verified by read-back) | **YES** — legacy SSE | yes |
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
> ~~**Carryover:** F2 (impure via record-inject) — now **unblocked**~~ — **F2 increment 1 LANDED
> 2026-07-17**: the replay it rides on was indeed unchanged, and F2 displaced exactly the ONE
> fallback caller flagged here (`!pure`, syscall half only); `!replayable` is untouched. One bounded
> follow-on: revisit the pin the day QEMU's AVX TCG reaches a Unicorn release — at which point
> YMM/ZMM seeding becomes *observable* and this increment's deliberate XMM-only choice should be
> revisited. (A BMI allowlist was considered and **measured to be worth nothing**; see the
> review round below.)

> **UPDATE 2026-07-17 — ADVERSARIAL REVIEW round: 7 defects found, all reproduced by EXECUTION
> and all fixed. `make docker-dataflow-attach` → 65/65.** The review's unifying finding is the
> one worth carrying forward: **`region_scan` is a single point of failure feeding BOTH the
> replayability gate AND — via `touches_vec` — the vector seed and the vector canary, and it
> failed OPEN in three different ways.** They were never independent defences: when the scan was
> wrong, the gate let the instruction through *and* removed the check that would have caught the
> lie. Every gate now fails **closed**.
>
> | # | defect | now |
> |---|---|---|
> | **HIGH 1** | a decoder DESYNC left both verdicts optimistic — a constant-pool island (routine in the JIT method-maps this tier targets) makes a `movabs` swallow a following VEX prefix; measured `replayable=1 touches_vec=0, remaining=5/17`, so a VEX-128 reached Unicorn ungated AND unwitnessed | `remaining != 0` ⇒ `replayable=0 ("decode")`, `touches_vec=1`. **LANDED 2026-07-18** (`dataflow-producer-correctness.md` T7): a caller-vouched real-instruction-extent list (`asmtest_blockstep_extent_t`) lets `region_scan` skip an island's bytes entirely rather than merely fail closed on them — `island_sse` (the same island shape, with a replayable legacy-SSE instruction on the far side) recovers to `replayable=1` and a byte-identical replay when given extents that hop it, while staying the same fail-closed `"decode"` verdict without them |
> | **HIGH 2** | **MXCSR was never seeded**, so the replay used Unicorn's default rounding. Measured on the legacy-SSE path — the one the whole perturbation win rests on: `divsd` under RC=toward-zero gave oracle `0x3fc9999999999999` vs replay `0x3fc999999999999a` at `rc=OK, truncated=0, pure=1`. Non-default rounding is not exotic (`-ffast-math`'s crtfastmath.o; JIT/managed runtimes) | MXCSR seeded + read-back-verified, and its **control** bits added to the canary. Legitimate only because Unicorn was verified to **honour** it (matches silicon under RN and RZ); had it merely stored the value, the honest move was to gate FP regions |
> | **HIGH 3** | the impurity early-`break` truncated the sweep, so vector instructions AFTER a `cpuid` were unseen ⇒ `touches_vec=0` ⇒ **no `xstate_read` on the single-step fallback** ⇒ every vector record `value_valid=0` at `rc=OK`. That is the headline "values in the trace" deliverable failing on the exact path ALL AVX code is routed to | only the *purity* answer is settled early; the sweep runs on |
> | **MED 4** | the PUBLIC `is_replayable()` answered **1, reason=NULL** for `cpuid; vpaddq xmm0,xmm1,xmm2; ret` — telling a caller "Unicorn can faithfully replay this" about the instruction this tier calls a silent liar. `run()` masked it; the API was still wrong | purity and replayability are now independent verdicts with independent reasons |
> | **MED 5** | **the central design claim was unfalsifiable by its own suite.** Swapping the encoding gate for the Capstone AVX-group gate the commit explicitly rejects **passed all 47 checks** — every scanned region happened to contain a metadata-visible instruction, and the one that proves the gap (`vpbroadcastq zmm0,xmm0`) sat in the entry *glue*, which `region_scan` never scans | two regions gated ONLY by the encoding rule: `vex_bmi` (`andn`, no hardware gate, pins the rule on every x86-64 box) and `evex_invis` (every region instruction metadata-invisible). The metadata swap now **fails 4 checks** |
> | **LOW 6** | the `cs_open`-failure path's comment said "assume the worst" while the code left `replayable=1` — failing closed only by downstream accident | says what it means |
> | **LOW 7** | Hi16_ZMM (zmm16-31) reassembly was uncovered — deleting it passed all 47 | covered by a region computing through `zmm16` |
>
> **MEASURED COST OF FAILING CLOSED** (this repo's own sources, 224 functions, four builds, via
> the shipped `is_replayable`): `-O2`/`-O3` decline **0%** — baseline x86-64 emits no VEX at all;
> `-O3 -mavx2` / `-march=native` decline **17.3%**, and **all** of those contain genuine vector
> VEX/EVEX that no released Unicorn can run anyway. The **decode** (fail-closed) verdict fired on
> **0 of 224** — compilers put constants in `.rodata`, so the island case is JIT-specific, which
> is precisely this tier's target. The deliberate **BMI over-gate cost 0 functions**, so the
> allowlist follow-on is retired rather than deferred.
>
> **Five further candidates were REFUTED** under the same discipline and correctly NOT acted on
> (XOP `0x8F` handling; `vec_coherent` on a failed XSTATE read; MMX live-in; the per-boundary
> re-seed; check 36 comparing single-step against itself). One review note is worth recording as
> a method lesson: the MXCSR negative control initially used `no_vec_seed`, which zeroes the XMM
> file too — so the region computed `0.0/0.0 = NaN` and the control "passed" by re-proving the
> XMM bug rather than the rounding one. It now uses a dedicated `no_mxcsr_seed` hook that leaves
> `vec_seeded=16` and only `mxcsr_seeded=0`, so the divergence can have exactly one cause.

> **UPDATE 2026-07-17 — F1 increment 1 CANNOT be CI-gated on GitHub runners, and now we know why.**
> It was an orphan until 2026-07-17 (`dataflow-blockstep-test` was defined at
> [mk/dataflow.mk](../../../../mk/dataflow.mk) but omitted from the `dataflow-test` aggregate, and
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
> coin flip that increment 2 diagnosed and fixed); ~~F2 (impure via record-inject) remains~~ —
> **F2 increment 1 LANDED 2026-07-17** (syscall record-and-inject; rdtsc/cpuid still gated).
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
  [asmtest_ptrace_trace_attached_blockstep](../../../../include/asmtest_ptrace.h#L125),
  [ptrace_backend.c:1761](../../../../src/ptrace_backend.c#L1761); flagged as an unapplied
  perturbation lever).
- At each boundary add a **`GETREGS` + XSTATE** read (the existing block-step reads only
  PC + the return register via `read_pc_ret`,
  [ptrace_backend.c:1829](../../../../src/ptrace_backend.c#L1829) — this is *new* capture, not
  free reuse), giving a real ground-truth register snapshot at every branch. *(Both halves are
  now landed: `GETREGS` in increment 1, `NT_X86_XSTATE` in increment 2 — the latter paying for
  the read only where the region's scan says it touches vector state. Note the XSTATE component
  offsets MUST come from `CPUID.0xD`; they are not constants, and this Zen 5's layout
  (576/832/896/1408) differs from the commonly-quoted one because its XCR0 omits MPX.)*
- **Replay the straight-line block** through the Unicorn L0 producer
  ([src/dataflow_emu.c](../../../../src/dataflow_emu.c)), seeded with that real register state
  and memory faulted in lazily from the tracee, to reconstruct the per-instruction values
  *between* boundaries. The endpoints are always real observations, so replay only ever
  fills a bounded pure interior — the plan's Phase-3b idea
  ([data-flow-tracing-plan.md:183-186](data-flow-tracing-plan.md#L183)), promoted to the
  primary perturbation win.
- **Region-granularity purity classification** to avoid emulating through the OS: static-scan
  the method's (time-correct) bytes once for `syscall`/`sysenter`/`int 0x80`/`rdtsc`/
  `rdtscp`/`rdrand`/`rdseed`/`cpuid`; a **pure** method gets block-step + replay, an
  **impure** one falls back to direct single-step (F2 lifts that restriction **for the syscall
  half** — see F2 below; the ordering trap this bullet names is what BTF's free post-syscall
  boundary resolves). This
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

## F2 — Record-and-inject OS-interaction fidelity *(**INCREMENT 1 LANDED 2026-07-17** — the syscall half; the exit criterion is MET. `rdtsc`/`cpuid`/`rdrand` remain gated on a PRIMITIVE, not a decode — see below. **HOST-gated, not CI-gatable**, exactly as F1)*

> **UPDATE 2026-07-17 — LANDED, and the design is far SMALLER than this plan assumed, because
> one measured fact removed most of it.** `make docker-dataflow-attach` / `make
> dataflow-blockstep-test` — **118/118** (65/65 before F2), deterministic, **0 skips**, on both
> the host and in-container.
>
> **THE MEASUREMENT THAT DECIDED EVERYTHING.** `PTRACE_SINGLEBLOCK` (DEBUGCTL.BTF) **already
> traps `syscall` and `int 0x80`** — they are control transfers — and **does not** trap
> `rdtsc`/`rdtscp`/`rdrand`/`rdseed`/`cpuid`, which are not. Probed on this Zen 5: one
> SINGLEBLOCK over `mov eax,39; syscall` lands at **syscall+2 with the kernel's return already
> in rax**; the same probe over `cpuid`/`rdtsc` runs straight through to the `ret`. Three
> consequences, and they are the whole increment:
>
> | | |
> |---|---|
> | **The syscall boundary is FREE** | The forward pass ALREADY stops immediately after the syscall retires, with the real result in the real registers and the kernel's memory delta already in the real process. F2 adds **no stop**: `sc_pread` 9→2 in-region stops, `sc_loop` 34→10. The perturbation win is kept, not traded for fidelity. |
> | **Nothing is fabricated; there is NO syscall table** | This plan expected a decoder that knows which argument is an output buffer and how many bytes the kernel filled ("a syscall that fills a buffer is the hard case"). **None is needed.** rax/rcx/r11 are read from the real boundary snapshot; the kernel's MEMORY delta is delivered by the per-boundary ground-truth re-snapshot **F1 already performs**. There is zero per-syscall knowledge in the tier. |
> | **No memory is injected — structurally** | Because the syscall **terminates** the block, no replayed instruction runs after it, so Unicorn's stale memory can never be read; the next block is seeded from the post-syscall snapshot. This matters because the coherence canary compares **registers only** — it has no memory comparison. F2's answer is not to add a witness but to **leave nothing to witness**. Exposure is zero by construction, not by checking. |
>
> **THE PLAN'S "reuse asmspy's syscall decoder" PREMISE DOES NOT HOLD, and did not need to.**
> asmspy's decoder is a NUMBER→name table plus a path-argument formatter **for display**
> ([cli/gen-syscall-names.sh](../../../../cli/gen-syscall-names.sh)); it has no model of output
> buffers, which is the only thing record-and-inject would have needed from it. It is also in
> `cli/`, which ships no such surface. Nothing was reused, and the tier is **safer** for it: a
> per-syscall table is exactly the fabrication surface this tier exists to avoid.
>
> **WHAT IS SUPPORTED vs GATED — a hardware boundary, not a preference.**
> - **Supported (injected):** `syscall` and `int 0x80`, *any* number — including ones that fill a
>   buffer (`pread`), ones whose result is consumed (`getppid`), and the plan's own named example
>   `clock_gettime`. The tier never looks at the syscall number.
> - **Gated to single-step:** `rdtsc`/`rdtscp`/`rdrand`/`rdseed`/`cpuid`/`sysenter`. **Not an
>   oversight and not a decode gap**: BTF gives no boundary at which their retired value could be
>   recorded, so record-and-inject has nothing to record. Reaching them needs a different
>   **primitive** — a hardware exec breakpoint at the site (the DR0-3 plumbing F3 already
>   landed), or single-stepping to it — which is the natural **increment 2**, and is why this
>   plan's "same treatment for rdtsc/rdrand/cpuid" bullet is *deferred rather than done*.
> - **Refused at runtime:** a syscall that does not return to the next instruction. `execve` is
>   the reachable case (the address space is replaced) and it **truncates**.
>
> **F2 DISPLACES ONLY ONE OF THE TWO CALLERS OF THE FALLBACK**, exactly as F1's author flagged.
> `!scan.pure` is now lifted for the syscall half; `!scan.replayable` (a VEX/EVEX encoding no
> released Unicorn executes) is **untouched and unrelated** — record-and-inject cannot give
> Unicorn a decoder it does not have. An impure AVX region is still single-stepped.
>
> **"UC_ERR_OK" PROVES NOTHING — MEASURED AGAIN, ON NEW INSTRUCTIONS.** Unicorn **executes**
> `syscall` and returns `UC_ERR_OK`, simply advancing rip with rax still holding the syscall
> NUMBER; **executes** `rdtsc` returning a FABRICATED counter (`0x132f2_1638543f`); **executes**
> `cpuid` returning zeros. Only `rdrand` faults. So `step_block` decodes at its **own pc** and
> refuses — a defence deliberately NOT sharing region_scan's answer, which is F1's central
> lesson applied: region_scan is a linear sweep that **desyncs on a constant-pool island** (its
> own `island` fixture proves it), so an impurity hidden behind one is invisible to it, and the
> gate would pass the instruction through *and* remove the scan-derived witness.
>
> **THE INJECTION IS THREE REGISTERS, AND THE CANARY STILL SEES THE REST.** Only rax/rcx/r11
> (rax alone for `int 0x80` — measured: an interrupt gate leaves rcx/r11 untouched) and rip.
> Those are necessarily **tautological** at the syscall boundary — compared against the snapshot
> they were copied from — and that is stated rather than glossed. Every *other* register, rsp and
> the arithmetic flags remain genuinely checked, so the canary still validates the block's whole
> pure prefix. **r11 cannot be computed honestly**: measured, it returns as the pre-syscall
> rflags OR'd with **TF (0x100)** — the ptrace stepping bit — so "computing" it would mean
> modelling the debug mechanism's own perturbation of the value it reports.
>
> **THE TRACE RECORDS THE SYSCALL'S DEF, which it otherwise could not.** Measured: the shared
> operand enumerator reports `syscall`/`int 0x80` as touching **no registers at all** (0 reads, 0
> writes) — Capstone models the instruction, not the ABI. So the kernel's result would never
> appear in the value trace, and the L1 def-use edge from a `read()` to its consumer could not
> exist. `open_step` supplies the architectural write set producer-locally, on the ONE path both
> value sources share, so the oracle and the replay grow identical records automatically.
>
> **MUTATION-PROVEN — 8 mutants, each caught by the checks claiming that capability.** Boundary
> rule → check 72 *(and the detail is the point: unguarded, `execve` reports **injected=1** — a
> fabricated injection for a syscall that never returned. The capture still truncated, but only
> via the canary, by downstream accident — the LOW-6 pattern F1's review named. The guard turns
> an accidental catch into an explicit refusal.)*; per-step decode → the `blind_rdtsc` NC only
> *(a fixture that overwrites **both** of rdtsc's outputs so the canary is blind — a naive one
> would have been saved by the canary and proved nothing)*; syscall write set → 6; `int 0x80`
> forging rcx/r11 defs → exactly 53; `injectable` settled at the first impurity → **exactly 14**;
> F2's gate reverted to F1's → 10; rcx/r11 uninjected → 4; one-shot injection → `sc_loop`.
>
> **THE ORACLE CONSTRAINT WORTH INHERITING.** The byte-identical oracle captures the region in
> **two separate forked tracees**, so it can only judge a syscall whose result is the same in
> both — which rules out `getpid` and, notably, **`clock_gettime`, this plan's own example**.
> Hence `getppid` (returns the *test's* pid: equal in both children AND externally checkable) and
> `pread` with an **explicit offset** into a seeded `memfd` (immune to the shared file offset the
> two children inherit — a plain `read()` would have given the second child different bytes and
> silently broken the oracle rather than the code). `clock_gettime` is covered by a **second
> oracle that needs no determinism**: the replay's recorded LOAD value vs the REAL tracee's own
> returned rax from the same capture — independent sources, agreeing iff the replay's
> post-syscall memory matched reality. That is also the answer to "no memory comparison in the
> canary".
>
> **KNOWN LIMITS (what the fixtures cannot produce).** (1) No fixture uses a **blocking** syscall
> (futex/poll/read-on-empty-pipe); the boundary arrives after the syscall returns either way, but
> it is not demonstrated — and `futex` is where the concurrency residual actually bites, since a
> wake means a sibling changed memory the boundary snapshot may not reflect. (2) A kernel write
> **outside the replay's stack window** (heap/mmap) is memory the guest never mapped — this is
> **F1's scope, inherited, not introduced by F2** — and is now *proven* to fail closed rather than
> assumed to (`sc_pread_heap`: the replay truncates; the same region single-steps to the correct
> value, so the truncation is attributable to the memory scope and not to a broken fixture).
> (3) No multi-threaded/concurrent-memory fixture — the plan's stated residual, unchanged.
>
> **Carryover:** increment 2 — `rdtsc`/`cpuid`/`rdrand` via a hardware exec breakpoint at the
> impure site (per-BLOCK rather than per-REGION gating), which would let a region with a `rdtsc`
> on a cold path keep the replay for every other block. F5 inherits all of this tiering.
>
> **LANDED 2026-07-18** (`dataflow-producer-correctness.md` T5+T6): the DR exec-breakpoint
> boundary (T5) and its record-and-inject into the replay (T6) are both in
> `src/dataflow_blockstep.c`. The per-block claim is exact: `hwrec_coldpath` (a runtime branch
> that skips the only `rdtsc` site) replays byte-identically with `hw_hits==0 injected==0` —
> the region is no longer punished for a site its real run never reaches.

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

> **MET 2026-07-17 for the syscall half, on both clauses.** `sc_pread` — a `pread64` whose kernel
> buffer-fill the region then LOADS — replays **byte-identically** to the single-step oracle (9
> steps, 18 records, raw `memcmp` identical) while cutting in-region stops 9→2; `sc_clock_gettime`
> replays with the real kernel value, checked by the independent memory oracle since two captures
> at two different times cannot be byte-compared. A region storing a **sentinel** into the buffer
> the kernel is about to overwrite makes a stale-snapshot replay return `0x1111111111111111`
> rather than unpredictable red-zone garbage — so the fixture can distinguish its own failure
> mode. The concurrency residual is unchanged and documented above; the endpoints still detect it.
> **`rdtsc`/`rdrand`/`cpuid` are NOT met and are deliberately deferred** — they need a hardware
> exec breakpoint, not a decoder (BTF does not trap them; measured).
>
> **MET 2026-07-18** (`dataflow-producer-correctness.md` T5+T6): the hardware exec breakpoint
> (T5) gives the forward pass the missing boundary, and T6 injects its recorded post-state into
> the replay — `rdtsc`/`rdtscp`/`rdrand`/`rdseed`/`cpuid` are all injectable now, subject to the
> same 4-slot DR0-3 cap syscall/int80 never needed. `hwrec_cpuid` replays byte-identically to
> the single-step oracle; `hwrec_rdrand_jc` proves CF (not merely the destination register) was
> injected; two-site monotonicity holds for `rdtsc`. `make dataflow-blockstep-test` 191/191.

---

## F3 — Hardware data-watchpoint targeted mode *(**LANDED 2026-07-15** — asmspy --watch; AArch64 analog LANDED since — remaining arm64 validation owned by [asmspy-aarch64-support.md](../../implementations/asmspy-aarch64-support.md) ◐ 5/7)*

> **UPDATE 2026-07-15 — LANDED as `asmspy --watch`.** `asmspy --watch <pid> <addr|func+off> [--rw] [--len=N]
> [--json]` arms an x86 data watchpoint (DR0-3; DR7 R/W=01 write / 11 r/w, LEN 1/2/4/8) via `PTRACE_POKEUSER`,
> `PTRACE_CONT`s the target, and at each `#DB` reports read/write + the value (`process_vm_readv`) + the
> faulting PC resolved to function+offset. **Per-thread arming across `/proc/<pid>/task/*` + `PTRACE_O_TRACECLONE`**
> (the spike's mandated fix) — verified capturing writes on a WORKER thread (tid ≠ leader). New
> `asmspy_engine_watch` lives in `cli/`; `src/ptrace_backend.c` untouched. Two-phase detach so the target
> survives; self-skips on POKEUSER-refused / qemu / non-x86-64. `make docker-cli` cli-smoke PASS. ~~Carryover:
> the AArch64 `NT_ARM_HW_WATCH`/`DBGWCR` analog.~~ — **LANDED since**: the analog ships at
> [cli/asmspy_engine.c:4998-5021](../../../../cli/asmspy_engine.c#L4998) (`NT_ARM_HW_WATCH`
> `DBGWVR`/`DBGWCR` slot-0 arming); the remaining arm64 **live validation** is owned by
> [asmspy-aarch64-support.md](../../implementations/asmspy-aarch64-support.md) (◐ 5/7).
> Increment-0 spike evidence retained below.

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
i.e. `R/W0 = 00`, [ptrace_backend.c:485-493](../../../../src/ptrace_backend.c#L485)). The x86
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
  path, [ptrace_backend.c:502-550](../../../../src/ptrace_backend.c#L502)).

**Exit criteria:** watch a chosen location on a live process and report every read/write with
its value and function at native speed; the 4-location / 1-2-4-8-byte caps are documented and
enforced; self-skips under qemu-user (which emulates zero breakpoint slots,
[ptrace_backend.c:527-529](../../../../src/ptrace_backend.c#L527)).

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
> [f4-attach-profiler-probe-findings.md](../../analysis/f4-attach-profiler-probe-findings.md) (the
> feed: a profiler CAN attach to a process we did not launch) and
> [f4-gc-fence-freeze-probe-findings.md](../../analysis/f4-gc-fence-freeze-probe-findings.md) (the
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
> (Structurally it could not be otherwise: [dataflow_ptrace.c](../../../../src/dataflow_ptrace.c) runs
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
> [asmtest_valtrace.h](../../../../include/asmtest_valtrace.h#L316) explicitly defers.

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
> `at_gc_remap_live` ([dataflow_dr_client_inlined.c:732](../../../../src/dataflow_dr_client_inlined.c#L732))
> through a POSIX-shm handshake and remapped 60,021 real ranges on a live compacting GC under
> DynamoRIO, with `make docker-gcprofiler-probe` proving profiler/DR coexistence. Findings:
> [gc-move-range-extraction-findings.md](../../analysis/gc-move-range-extraction-findings.md).
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
([asmtest_valtrace.h:353](../../../../include/asmtest_valtrace.h#L353)),
[src/dataflow_gcmove.c](../../../../src/dataflow_gcmove.c), whose only callers today are its unit
test [examples/test_dataflow_gcmove.c](../../../../examples/test_dataflow_gcmove.c) — what is
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

> **UPDATE 2026-07-19 — the "further step" (full object identity) LANDED (increment 4).** The one
> residual address identity could not express — a pre-window record touching memory that a GC then
> slides a live object into *aliases* that object — is now retired. A heap snapshot of
> `{Address, Size, TypeID}` nodes from the runtime's `GCBulkNode`/`GCBulkEdge`/`GCBulkType` events
> (a new EventPipe `gccanon_dumper`) is joined with the `MovedReferences2` feed, so each memory
> record keys on *(object, offset)* where the snapshot has evidence and degrades to address identity
> where it does not (`asmtest_objid_canonicalize`, `src/dataflow_objid.c`; pure unit suite
> `test_dataflow_objid`, 27/27). `docker-gccanon-attach` gains an `alias` phase: the false store→load
> edge address identity forges (a doomed object stored at X, a live object slid onto X and loaded
> there) is reproduced as the NEGATIVE CONTROL and then SEVERED by object identity — green over two
> consecutive runs. The snapshot-space convention (GCBulkNode addresses are POST-relocation, so the
> dump GC's own ranges belong in the translation set) was **measured, not assumed** — see
> [f4-objid-snapshot-space-findings.md](../../analysis/f4-objid-snapshot-space-findings.md). Increments
> 1+2 correctly shipped ADDRESS identity; this is the next increment atop them, not a fix to them.

---

## F5 — PT + code-image + replay: the least-perturbing ceiling *(**LANDED + SILICON-VALIDATED 2026-07-21** — decode→replay bridge CI-validated 2026-07-19; live capture validated on bare-metal Intel PT, commit `c7b4ef7`; [dataflow-pt-replay-tier.md](../../implementations/dataflow-pt-replay-tier.md) ✅ 5/5)*

The conceptual minimum-perturbation path from the design doc ("stop instrumenting and start
observing"): reconstruct the **exact executed instruction stream** out of band via Intel PT /
CoreSight ([asmtest_hwtrace.h](../../../../include/asmtest_hwtrace.h)), supply time-correct bytes
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

> **STATUS 2026-07-19 (actioned as
> [dataflow-pt-replay-tier.md](../../implementations/dataflow-pt-replay-tier.md)).** The
> decode→rebase→materialize→replay **bridge is LANDED and CI-validated with no PT
> hardware**: `src/dataflow_pt.c` (`asmtest_dataflow_pt_replay_path` +
> `asmtest_dataflow_pt_replay`) fills the shared `asmtest_valtrace_t` **byte-identically to
> the emulator L0 oracle** on the canonical ROUTINE, and its def-use (L1) + backward slice
> (L2) **equal** the oracle's — F5 is a drop-in L0 producer. It opens **no** perf event
> (position 9): it consumes a captured AUX blob + a code-image, driven in CI by the
> **synthetic** `asmtest_pt_encode_fixture` (libipt's own encoder). It inherits F1/F2's
> purity/replayability verdicts and **truncates honestly** on an impure / VEX-EVEX /
> nondeterministic region (no single-step fallback), a per-step path cross-check catching
> a divergence. Lanes: `make dataflow-pt-test` (native, Unicorn) + `make docker-dataflow-pt`
> (the libipt+Unicorn image; `libipt-dev` added to `Dockerfile.dataflow-attach`).
>
> **The LIVE foreign-pid half (T4) is wiring-complete but hardware-unvalidated**, gated now on
> ONE un-installable prerequisite: bare-metal Intel PT silicon (the `intel_pt` PMU with
> `perf_event_paranoid < 0` / `CAP_PERFMON`). Its second gate is **resolved** — the sibling
> [intel-pt-attach-foreign-pid.md](../../implementations/intel-pt-attach-foreign-pid.md) landed
> ☑5/5, so its `asmtest_hwtrace_pt_attach_begin/track/poll/end` capture arm is in the tree, and
> `examples/test_dataflow_pt.c`'s live case now CONSUMES it (2026-07-20): a runtime
> `asmtest_hwtrace_available(ASMTEST_HWTRACE_INTEL_PT)` probe replaces the old never-defined
> compile-time gate, and where PT is present it forks a victim, captures ONE in-region
> invocation with zero single-steps, subtracts the region base from the decoded absolute IPs,
> replays through F5, and oracle-matches against both the emulator L0 and the `force_singlestep`
> block-step ground truth. Until a bare-metal Intel box runs the oracle match, this status stays
> **wiring-complete, not validated** (verify-before-declaring-done); the fail-not-skip target
> is `make dataflow-pt-live` (`ASMTEST_REQUIRE_PT=1`), which reddens a supposed-PT box whose
> PMU is silently hidden. Proving command on silicon:
> `make dataflow-pt-live` (or `ASMTEST_REQUIRE_PT=1 make dataflow-pt-test`) on the Intel-PT runner.
>
> **VALIDATED 2026-07-21 — the oracle match RAN on silicon.** Commit `c7b4ef7` +
> [intel-hardware-validation.md](../../intel-hardware-validation.md): `make dataflow-pt-live` →
> **29/29, stable across 3 consecutive runs**, on a bare-metal Intel Core i7-8559U (Coffee
> Lake). Both live walks (`live(20,22)`, `live(200,1)`) captured one foreign-pid region
> invocation with **zero single-steps of the target**, the PT-decoded path equalled the
> single-step oracle path, and the F5 replay value trace was **byte-identical to the emulator
> L0** — the exit criterion above, met as written. The brief
> [dataflow-pt-replay-tier.md](../../implementations/dataflow-pt-replay-tier.md) is ✅ 5/5.

---

## F6 — Toward whole-process / continuous data flow *(**INCREMENT 1 LANDED 2026-07-17** — the WINDOWED survey; the exit criteria are MET. "Whole-process / continuous" is **declined with a measured number**, not deferred — see the ceiling below. **HOST-gated, not CI-gatable**, exactly as F1/F2)*

Lift the scoped-region bound. The windowed steppers + the addr-channel already follow a live
JIT's methods across a whole window for control flow
([asmtest_ptrace_trace_attached_windowed](../../../../include/asmtest_ptrace.h#L142)); extend
the value producer onto that windowed frame for a **survey** (sampled) def-use over a whole
window rather than one region. Where the ask is production whole-process **taint**, this hands
off to [dynamorio-taint-tier-plan.md](dynamorio-taint-tier-plan.md) (whose in-band L0 value
producer already landed) rather than pushing single-step past its cost envelope.

**Exit criteria:** a windowed capture produces a def-use survey spanning multiple JIT'd
method ranges of a live process; the hand-off boundary to the DR taint tier is documented
(single-step/replay for scoped exactness; DR for whole-process breadth).

> **MET 2026-07-17 on both clauses — and the second clause is now a NUMBER, not a
> judgement call.** `asmtest_dataflow_ptrace_attach_window` (in the scoped ptrace producer,
> `src/dataflow_ptrace.c`) run_to's a window frame, steps until it returns to its caller, and
> records values for every instruction in the frame OR any body published on the
> address-channel — one valtrace whose backward slice from a sink in M2 reaches through the
> frame into M1, i.e. **a def-use survey spanning three ranges of a live process**, which
> then SURVIVES the detach. A method published while the window is **already open** is picked
> up and recorded (the channel is drained at every stop, not sampled at entry). **Zero diff
> under `include/`** — the tier ships no header, so its suite re-declares the entry and its
> telemetry, exactly as F1's does. Lane: `make docker-dataflow-attach`, **123/123**, ran for
> real (not skipped) in-container; the F1/F2 blockstep suite is unchanged at 118/118.
>
> **THE LIFT'S REAL CONTENT IS NOT THE LIFT — IT IS WHAT THE LIFT BREAKS.** A windowed survey
> **elides the runtime glue** between managed methods (that elision is the entire point: the
> glue is the noise a managed capture exists to drop). Elision is also precisely where a
> def-use graph starts **FABRICATING EDGES**: an in-region step writes a location, the elided
> glue overwrites it, a later in-region step reads it — and the shared last-writer builder,
> having no record of the glue, hands the edge to the **stale in-region writer**. Note what
> this defeats: **the VALUE at the read is honest** (it is read from silicon), so no
> value-level oracle, and no byte-identical cross-producer comparison, can catch it. It is
> only the *edge* that lies. Mutation-proven: with the barrier's register half disabled the
> fabricated edge appears **while the value assertion still passes**.
>
> **The bound that makes it fixable.** A fabricated edge REQUIRES a prior *recorded* write to
> that location — a location the survey never wrote has no last writer and yields no edge at
> all. So the **at-risk set is exactly the set of locations the survey has recorded a write
> to**: finite, observable, and cheap to re-check. At each gap entry it is snapshotted; at gap
> exit it is diffed and **one synthetic GAP step** is appended whose write set is precisely
> what the glue changed. Registers compare the **alias's own bit slice** (the last-writer map
> keys raw Capstone ids with *no* sub-register canonicalization — `src/dataflow.c:219` — so a
> gap that changes `AH` must not shadow a live `AL` edge); memory compares **per byte**
> (`src/dataflow.c:251`). It **fails closed** — over its caps, or on any location it cannot
> decide, it flags `risk_overflow`/`truncated` rather than guessing. Both directions are
> proven: disabling the barrier fabricates edges (M1/M2); making it a **blanket** invalidation
> **deletes the true cross-method edge the exit criterion rests on** (M3), so its precision is
> load-bearing, not decorative.
>
> **THE CEILING, MEASURED — and why "whole-process / continuous" is declined rather than
> deferred.** The scoped tier's cost model does not survive the lift and cannot be made to. A
> window pays **one ptrace round-trip per instruction the target retires inside it, recorded
> or not**: the tracer must see every instruction just to learn whether it is in a region.
> The glue is elided from the RECORD but never from the BILL. Proven as an exact identity,
> not a benchmark: two windows differing only in glue length — **2000 extra glue iterations
> cost exactly 4000 extra stops while `recorded` stays pinned at 16**. Subtracting the two
> runs cancels SEIZE/run_to/detach and leaves the price of a stop:
>
> | | measured (bare-metal Zen 5, kernel 6.17) |
> |---|---|
> | per-stop cost | **~2.7 µs** (2730 ns in-container; 2721–3167 ns across host runs) |
> | window vs native, same frame | **~34,000x** (33,811x in-container) |
> | DR taint tier, in-band, whole-process | **~11x** (already landed, measured) |
>
> Those two tiers are **~3,000x apart**, and the gap is structural, not an optimization
> backlog: nothing in this producer can stop paying ~2.7 µs for an instruction it must
> classify. An unbounded window is an unbounded bill. **So the hand-off is not a preference,
> it is arithmetic** — ptrace for a BOUNDED window answered exactly off a live process with
> no code modification; **DynamoRIO for whole-process breadth**
> ([dynamorio-taint-tier-plan.md](dynamorio-taint-tier-plan.md)). Lifting
> the window further does not approach whole-process; it just makes the bill bigger while
> re-opening every hazard region scoping dodges.
>
> **KNOWN LIMITS (what the fixtures cannot produce).** (1) **Single-threaded target, leader
> only.** Like `attach_pid`, this SEIZEs the thread-group leader and surveys the window IT
> runs; siblings run free. The barrier is sound against **the elided glue of the surveyed
> thread** — which is NOT the same as sound against a concurrent mutator. **The plan's
> concurrency residual is unchanged and explicitly NOT closed here**, and widening the seize
> re-opens the deadlock / GC-fence-stall hazards (~19.8s, measured) that scoping dodges. (2)
> **`decode_fail` is a fail-closed branch asserted ZERO but never fired**: the fixtures cannot
> produce a window PC whose bytes will not read or decode (that needs a JIT to free a page
> mid-window). (3) **Sub-register aliases `gp_value` cannot resolve** (`R8D`/`R8W`/`R8B`…) put
> a location at risk the barrier must then decline to decide (→ `truncated`). That was a
> **pre-existing gap in this producer's register map**, surfaced by F6, not introduced by it —
> **CLOSED** by
> [dataflow-producer-correctness.md T1](../../implementations/dataflow-producer-correctness.md#T1):
> `gp_value` and `dfp_alias_shape` now fold R8D/R8W/R8B..R15D/R15W/R15B to their 64-bit
> container in both the scoped producer and the blockstep tier's own copy. (4) **No vector
> clobber across a gap** is
> exercised: the barrier diffs XMM/YMM (one batched snapshot per gap) but no fixture makes the
> glue clobber a vector register the survey recorded. **CLOSED** by
> [dataflow-producer-correctness.md T3](../../implementations/dataflow-producer-correctness.md#T3):
> `test_window_vec_gap` (`examples/test_dataflow_ptrace.c`) makes the glue clobber a live `xmm0`
> the survey recorded and asserts the GAP step carries the real post-glue value out of `wide[]`,
> and the post-gap read resolves to it rather than the stale pre-gap write. (T3 also closed a
> distinct, previously-undocumented gap in the SHARED BUILDER — `src/dataflow.c`'s def-use
> resolution keyed registers by raw Capstone id with no sub-register canonicalization, so even
> with the barrier above fully correct a cross-alias read like `write eax` / `read ax` produced
> no edge at all; see T3 for the fix and its own fixtures.) (5) The glue-tax **ratio** (314x) is a
> property of the fixture's chosen glue:method mix and means nothing on its own — the
> transferable numbers are the per-stop cost and the exact 2-stops-per-glue-iteration identity.
>
> **A HAZARD OF THIS TIER'S OWN CONVENTION, found the hard way.** A value-trace producer
> "ships no header", so every suite **re-declares** its telemetry struct. A field added on one
> side only does not fail to compile — it **silently shifts every later field**, corrupting the
> exact numbers the tier is judged by. This cost three green checks during development.
> `asmtest_dataflow_ptrace_win_info_layout()` now exports size **and the final field's
> offset**, and callers assert both: size alone is provably insufficient — a mutation adding a
> `uint32_t` to the caller's copy landed in **tail padding**, left `sizeof` unchanged, and the
> size-only guard passed on a struct that was already skewed. **F1's and F2's suites re-declare
> `asmtest_blockstep_info_t` under exactly this convention and have no such guard.**

---

## F7 — Live-attach data-flow in the language bindings *(**LANDED 2026-07-17** — all TEN bindings, each on a REAL attach)*

> **UPDATE 2026-07-17 — LANDED, and wider than the exit criteria asked.** All **ten** bindings
> wrap `asmtest_dataflow_ptrace_attach_pid` / `_pid_tid` / `_jit` on the opaque-handle pattern
> (methods on each language's `ValueTrace`, which owns the `asmtest_valtrace_t*`), and every one
> of them **really attaches to a live foreign process** — none stops at "the symbol resolves".
> Per-lane totals via `make docker-dataflow-<lang>` (new, in [mk/dataflow.mk](../../../../mk/dataflow.mk)):
> python 17, cpp 48, node 44, ruby 36, lua 36, zig 36, rust 36, go 36, java 36, dotnet 36 —
> 20+ live assertions per lane, 0 skips, 0 failures.
>
> **What made it possible at all:** the attach entry points live in the scoped ptrace PRODUCER,
> and `libasmtest_dataflow` deliberately bundled only the *pure analysis* objects — so there was
> nothing for a binding to dlopen. `pic/dataflow_ptrace.o` + `pic/codeimage.o` now ship in that
> lib. The old "producers are separate tiers" split is kept where it earns its keep (the OBJECT
> level, which is what lets the pure suites run everywhere) and dropped at the `.so` level, where
> it was never load-bearing: the producer self-stubs to `ENOSYS` off Linux x86-64, so the lib
> still links on every host, and Capstone/libbpf were already this lib's dependencies.
>
> **The shared victim** ([bindings/dataflow_victim.c](../../../../bindings/dataflow_victim.c)) is what
> keeps ten lanes honest with one fixture: same `df_chain` bytes as the native suite, publishes
> `base=/len=/pid=` on stdout, loops calling the region, bumps a counter file so a caller can
> prove it SURVIVED the detach. Its `a`/`b` come from **argv**, which is the anti-vacuity hinge —
> the expected result is a property of the run, so a second victim with different args (17+25=42)
> catches any wrapper that hardcodes an answer. Confirmed: a stub returning `(OK, 12)` reddens
> three tests; one patched byte in the victim's fixture reddens the result asserts in every lane.
>
> **Two findings worth keeping:**
> 1. **A managed runtime cannot attach to its own direct child.** The JVM (and .NET) reap children
>    on a dedicated reaper thread blocked in `waitpid()`. A ptrace-stop of a traced child is
>    reportable to **any thread in the tracer's thread group**, so that reaper races the producer's
>    own `waitpid`, eats the stop, and the producer blocks **forever** — the lane hangs with the
>    victim parked in state `t` (observed: TracerPid = the FFM downcall's thread, another JVM thread
>    in `do_wait`). The Java/.NET lanes therefore spawn the victim **detached** (`sh -c '... &'`, so
>    it reparents to init and the runtime's reaper never sees it); the attach still works because
>    the victim calls `PR_SET_PTRACER_ANY`. Python/Node/Ruby/Lua/Zig/Rust/Go/C++ are unaffected —
>    none reaps on a concurrent thread while a blocking downcall is in flight. This is a **real
>    constraint of the tier for any managed host**, not a test artifact.
> 2. **The parity script cannot see any of this.** Its surface comes from the *tier headers*
>    (`TIER_HEADERS`), and a value-trace producer ships **no header** by design — so all three
>    attach entry points are invisible to it, in all ten bindings, and no allow-list entry is
>    needed or possible. Nothing but the per-lane assertions cross-checks these signatures. That
>    is tolerable here only because F7's surface passes **no struct by value** (every argument is a
>    scalar or an opaque pointer), so the SysV eightbyte cliff cannot arise; what can arise is a
>    wrong arg slot — `attach_jit` takes ten args, six in registers and four on the stack — and the
>    `result == a+b` / `survived == 1` assertions are what catch it.
>
> **Correction to the record:** the claim that all ten bindings already wrapped "the full
> valtrace→defuse→slice pipeline (`ValueTrace`)" was **false**. Only python/cpp/node did; the other
> seven wrapped `gcmove_canon` + `method_resolve_pc` and nothing else (16/16 = 8 + 8). Those seven
> now have a minimal `ValueTrace` (the opaque handle + attach + step/record counts), so the name
> means the same thing everywhere. ~~The def-use/slice half stays unwrapped in them for a real
> reason: the slice seed crosses **by value** as an `at_val_rec_t`, which Fiddle (and friends) have
> no type for. That is a pre-existing gap, not F7's — and the natural next increment.~~ —
> **CLOSED**: [dataflow-bindings-slice-codeimage.md](../../implementations/dataflow-bindings-slice-codeimage.md)
> ✅ 4/4 (its T2 wraps defuse/forward/backward-slice in all seven).

Wrap the new attach entry points in the dynamic-FFI bindings, the way the L0/L1/L2 ValueTrace
pipeline is already wrapped for Python/C++/Node
([data-flow-tracing-plan.md:237](data-flow-tracing-plan.md#L237)) — opaque-handle pattern,
each gated by a `make dataflow-<lang>-test` lane (host or docker) as the other seven bindings
already are for the pure helpers.

**Exit criteria:** at least Python/C++/Node expose live-attach data-flow capture over an
attached pid; `make dataflow-<lang>-test` covers it; the seven docker-gated bindings wrap at
least the native path in their pinned toolchain images.

> **MET 2026-07-17, on all three clauses and beyond the first.** Python/C++/Node capture over an
> attached pid; `make dataflow-<lang>-test` covers it for all ten (each with a `docker-dataflow-<lang>`
> lane in its pinned image); and the seven docker-gated bindings do more than "wrap the native
> path" — they exercise a real attach against a live victim, with the same 20-assertion battery.
> ~~Carryover: the def-use/slice surface for the seven (blocked on by-value `at_val_rec_t`
> marshalling, not on F7), and `attach_pid_versioned`'s code-image argument, which every binding
> passes as NULL because the code-image recorder is its own binding surface.~~ — **CLOSED** by
> [dataflow-bindings-slice-codeimage.md](../../implementations/dataflow-bindings-slice-codeimage.md)
> ✅ 4/4: T2 wraps defuse/forward/backward-slice in all seven, and T4 lands the `CodeImage`
> surface plus a real `img` argument to `attach_pid_versioned` in all ten.

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
  *(Demonstrated 2026-07-21 on the bare-metal Core i7-8559U — commit `c7b4ef7`,
  [intel-hardware-validation.md](../../intel-hardware-validation.md); CI still self-skips.)*
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
> pure software; ~~**F2** extends F1's replay to impure methods~~ (**increment 1 LANDED** — the
> syscall half; increment 2, rdtsc/cpuid via a hw exec breakpoint, is the open remainder);
> **F6** is scope-gated; ~~**F5**
> waits on silicon this project does not have~~ (**validated 2026-07-21** on a bare-metal
> Intel box — commit `c7b4ef7`, `make dataflow-pt-live` 29/29 ×3).
>
> The original recommendation is retained below for provenance.

**F3 (hardware data-watchpoint targeted mode)** if the immediate need is watching specific
data with minimal perturbation — it is cheap, decoupled, and safe. **F1's spike increment**
if the need is making whole-region JIT tracing practical — it is the highest-leverage item but
must prove itself first. Both are independent of each other, so they can proceed in parallel;
defer F2/F5 until the F1 spike is green, and treat F4/F6/F7 as runtime-/scope-/binding-gated
follow-ons.
