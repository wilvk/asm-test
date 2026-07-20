# Analysis: capability-expansion ideation, independently reviewed

*Status: analysis / ideation, adversarially reviewed. Given a "how could the
functionality be expanded?" prompt, an initial list of seven expansion
directions was produced, then **independently re-verified against the code**,
adversarially critiqued, and re-ideated from scratch by six fresh lenses before
synthesis. The corrected finding inverts the initial premise. Review date:
2026-07-20 — every file:line below is a snapshot as of that date and, like the
[AMD hardware review](2026-07-17-amd-hardware-review.md), may drift. Derived from
the source of record —
[include/asmtest_emu.h](../../../include/asmtest_emu.h),
[include/asmtest.h](../../../include/asmtest.h),
[include/asmtest_trace.h](../../../include/asmtest_trace.h),
[include/asmtest_valtrace.h](../../../include/asmtest_valtrace.h),
[src/emu.c](../../../src/emu.c),
[src/asmtest.c](../../../src/asmtest.c),
[src/fuzz.c](../../../src/fuzz.c),
[src/disasm.c](../../../src/disasm.c),
[src/dataflow_blockstep.c](../../../src/dataflow_blockstep.c),
[src/drtrace_app.c](../../../src/drtrace_app.c),
[tools/emu_bench.c](../../../tools/emu_bench.c),
[scripts/bench-golden-check.py](../../../scripts/bench-golden-check.py) and the
[ct_eq example](../../getting-started/examples.md). Companion docs:
[post-v1 expansion plan](../plans/post-v1-expansion-plan.md) (Tracks A–F, the
directions already landed), [desktop-GUI ideation](2026-07-20-desktop-gui-ideation.md)
(the sibling "renderings vs protocol" inversion).*

## Summary

The most valuable expansion work is **not a new tier — it is finishing the one
asm-test already claims: full-state capture**. The initial seven proposals were
mostly new tiers (constant-time, symbolic, litmus, MSan-for-asm, …); code
verification found six of the seven **oversold** and one **physically
impossible**, while the highest value-to-effort items were nowhere on the list.

Two whole dimensions of architectural state are **captured and verified
internally but withheld from the user-facing structs**:

- **The FP status word (MXCSR / FPCR).** [include/asmtest_emu.h:62](../../../include/asmtest_emu.h#L62)
  (`emu_x86_regs_t`) exposes GP + rip + rflags + XMM data but **no `mxcsr`**;
  native `regs_t` at [include/asmtest.h:171](../../../include/asmtest.h#L171) is
  the same. Yet [src/dataflow_blockstep.c:149](../../../src/dataflow_blockstep.c#L149)
  already **seeds and verifies** MXCSR's rounding / FTZ / DAZ bits internally —
  the `uc_reg_read/write UC_X86_REG_MXCSR` path is proven to work; it is simply
  not surfaced.
- **The memory-address channel.** `on_mem_access` already receives every
  *valid* access's effective address at [src/emu.c:145](../../../src/emu.c#L145)
  (used today only for watchpoints), but `asmtest_trace_t` records execution-order
  offsets with **no `mem[]` address stream** ([include/asmtest_trace.h:44](../../../include/asmtest_trace.h#L44)).

One sentence: **the substrate — a virtual CPU with total-state readback, a
def-use slice engine, a sandboxed mutation engine, and one shared trace basis
across every backend — is richer than the assertions currently wired to it**, so
the biggest wins are small assertion layers over infrastructure already built and
CI-proven, not new engines.

## Method

Seven initial directions (R1–R7 below) were each put through a verify → critique
pipeline: one agent fact-checked every concrete claim against the code with
file:line evidence, a second adversarially decided keep / reshape / kill on the
*verified* facts. In parallel, six lenses (security, performance,
developer-experience, research/correctness, ecosystem, pedagogy) ideated fresh,
told **not** to restate R1–R7. A completeness critic then hunted for the axis
nobody named, and a synthesizer produced one unified ranking. Every factual claim
here traces to a code read, not to the initial proposal's own wording.

## How the seven initial recommendations held up

| # | Initial claim | Verdict | Correction the code forced |
|---|---|---|---|
| R1 | Constant-time tier (trace + taint + dudect timing) | **keep, reshaped** | `bench_measure` collapses samples into per-round min/median/mean ([src/asmtest.c:2087](../../../src/asmtest.c#L2087)), destroying the distribution a Welch/dudect t-test needs; Unicorn is not cycle-accurate, so no emulated leg sees **latency** leaks. The strong, cheap leg is deterministic **address + control-flow invariance**. Drop the "constant-time" banner. |
| R2 | llvm-mca / port-pressure, assertable | **keep, reshaped** | mca consumes assembly **text, not bytes** — a mandatory Capstone-decode→reassemble round-trip. SDE `-mix` is process-wide, not per-block ([tools/sde_mix_report.c](../../../tools/sde_mix_report.c)). It is a **report backend** for the existing `BM_MODEL_COST` lane, **never a gate**. |
| R3 | Perf-regression gate as a new assertion | **mostly duplicate** | A failing CI gate on the *deterministic count* metric already exists ([scripts/bench-golden-check.py](../../../scripts/bench-golden-check.py) / `make bench-check`). Cycles were **deliberately** left as trend (host noise; only the one non-virtualized macOS-Intel box is comparable). Survivor: a coarse **nightly ~2× cycle-cliff** detector. |
| R4 | Concurrency / atomics litmus tier | **KILL** | Physically impossible on the only engine — Unicorn/QEMU-TCG is **sequentially consistent, no store buffer**, so it cannot produce the relaxed outcomes litmus tests exist to detect. `capture.s`/emu also assume one entry + one `EMU_RET_MAGIC` return sentinel. Largest name-vs-reality gap in the set. |
| R5 | Bounded symbolic equivalence (Triton) | **keep, reshaped, low** | The reference is C **source**, so you must compile it and symex *that* — the honest guarantee is "matches **one compilation** of the ref," branch-free scalar-integer only, solver timeout → self-skip. New un-vendored Z3/Bitwuzla + Boost. |
| R6 | MSan-for-asm (shadow memory) | **keep, reshaped, low** | `on_mem_access` observes memory events only ([src/emu.c:145](../../../src/emu.c#L145)); a memory-only shadow **cannot see register→register** uninitialized flow — MSan's signature finding. Catches only "read of a never-written *memory* byte." Seeding ABI setup / return addr / preloaded regions is a false-positive minefield. Do not call it MSan. |
| R7 | Binary-import front door | **keep, reshaped, low** | "Test bytes" is ~zero novelty (emu already takes `(bytes,len)`). The load-bearing problem is **relocations**: `asmtest_exec_alloc` is a bare mmap+memcpy ([src/drtrace_app.c:492](../../../src/drtrace_app.c#L492)); only `asmtest_asm_exec_native` survives PC-relative code because it **re-assembles from source** ([src/drtrace_app.c:515](../../../src/drtrace_app.c#L515)), which a lifted blob cannot. Honest v1 rejects on any relocation → self-contained PIC leaf routines only. |

## The unified ranking

Ordered by verified value-to-effort. Items marked **(new)** came from the fresh
lenses / completeness critic, not the initial seven.

### Wave 1 — expose withheld state + cheap always-on static gates (S/M, low risk)

1. **FP-environment / MXCSR-FPCR assertions (new) — top pick.**
   `ASSERT_FP_EXCEPTIONS_EQ`, `ASSERT_NO_SPURIOUS_FP_EXCEPTION`,
   `ASSERT_ROUNDING_HONORED`, `ASSERT_NAN_PAYLOAD_PRESERVED`. Catches a SIMD/crypto
   routine that passes `ASSERT_FP_NEAR` on every value yet clobbers the caller's
   rounding mode or raises a spurious sticky flag. Plumbing proven at
   [src/dataflow_blockstep.c:149](../../../src/dataflow_blockstep.c#L149). Add
   `mxcsr` to the captured struct; native tier reads via `stmxcsr`. **M.**
2. **Secret-dependent-trace invariance (reshaped R1).** Add a `mem[]` address
   stream to `asmtest_trace_t` + `ASSERT_CT_TRACE_INVARIANT` asserting the
   `(insns[] + mem-address)` sequence is byte-identical across N caller-supplied
   secret input classes, from a shared `emu_snapshot`. Kills the weak `ct_eq`
   block-**count** check ([examples.md:452](../../getting-started/examples.md#L452),
   which passes any address leak that preserves block count). **M.**
3. **Static ISA-baseline gate (new).** `ASSERT_ISA_BASELINE(fn, x86_64_v2)` /
   `ASSERT_NO_INSN_GROUP(AVX512)` — Capstone-decode ([src/disasm.c](../../../src/disasm.c))
   the bytes, fail on the first instruction above a declared feature baseline.
   Catches the accidental-AVX512-SIGILL-on-customer-silicon bug invisible to CI on
   a different CPU. **S, always-on, no hardware.**
4. **Worst-case stack-depth bound (new).** `ASSERT_STACK_DEPTH_LE` — track the
   lowest-written stack byte in the existing write hook across a fuzz sweep.
   First-order correctness for interrupt/firmware/coroutine asm. **S.**
5. **Auto-vectorization / vector-width verification (new, perf lens).**
   `ASSERT_VECTOR_WIDTH` — inspect the retired trace's register classes
   (XMM/YMM/ZMM) to prove the vector path *actually executed* for these inputs,
   catching a scalar fallback or a toolchain-bump de-vectorization. Distinct from
   #3 (static presence vs actual execution). **M.**
6. **Per-test instruction/byte budget (new, perf lens)** + **reduce-and-capture**
   (fuzz failure → compilable, seedless `TEST()`, reusing the shrinker in
   [src/fuzz.c](../../../src/fuzz.c)). **S each.**

### Wave 2 — foundational engines other items build on

7. **Full-state observational-equivalence oracle (new).** `ASSERT_EQUIV(A,B,gen,n)`
   diffing return + flags + written memory of two raw-byte impls (fuzzed /
   exhaustive small-domain / **cross-compiler** as an input mode). `ASSERT_MATCHES_REFn`
   only samples rax against a C model today. Mutation-adequacy and a superoptimizer
   verifier build on this. **M.**
8. **Golden-state snapshot testing (new).** `EXPECT_SNAPSHOT` / `--update-snapshots`,
   reusing the bench-golden emit/compare machinery for a per-test state file. **M.**
9. **Backward-slice "blame-the-instruction" failure diffs (new).** The def-use
   slice engine ([include/asmtest_valtrace.h:190](../../../include/asmtest_valtrace.h#L190))
   is built and CI-proven but never wired to ordinary assertion failures:
   *"rax is wrong because offset 0x1f `add %rsi,%rax` overwrote it."* Doubles as a
   classroom explainer. **L.**

### Wave 3 — built on Wave 2 or comparable value (opportunistic)

Metamorphic / algebraic tier (`ASSERT_INVOLUTION` / `ASSERT_INVERSE_OF`, no
reference model needed) · crypto KAT importer (Wycheproof / NIST CAVP) ·
mutation-adequacy gate (surface the existing `emu_mutation_test1`,
[src/fuzz.c:122](../../../src/fuzz.c#L122)) · critical-path / loop-carried-dependency
length (the **latency** axis vs `BM_MODEL_COST`'s throughput axis, over the
existing def-use DAG) · misaligned-access detector · secret-residue / zeroization
audit · reshaped **R6** (uninit-read) / **R2** (llvm-mca backend) / **R7** (object
slicer).

### Wave 4 — ecosystem + stretch

SARIF 2.1.0 export of emu memory-safety faults (pair with a DWARF line-map
producer) · self-contained HTML trace/coverage visualizer · reshaped **R5**
(Triton prove-or-counterexample) · reshaped **R3** (nightly cycle-cliff).

**Deferred (sound but XL / demand-gated):** Spectre-v1 gadget finder · ARM MTE
modelling · superoptimizer verifier (strictly follows the equivalence oracle).

## The kill: R4 (concurrency / litmus tier)

Recorded here so it is not re-raised. Its headline capability — deciding
x86-TSO / ARM / RISC-V weak-memory litmus outcomes — is **physically impossible**
on Unicorn/QEMU-TCG, which is sequentially consistent with no store buffer. This
is a memory-model limitation that [CLAUDE.md](../../../CLAUDE.md)'s "add the dep"
rule cannot rescue (it is neither an installable dependency nor a hardware gate —
it is the engine's semantics). The salvageable core (an SC interleaving explorer)
needs an entry/stack-per-thread-context convention that does not exist
(`capture.s`/emu assume a single entry + one `EMU_RET_MAGIC` sentinel) plus
combinatorial interleaving with no partial-order reduction, and even then delivers
only narrow SC data-race detection overlapping the taint tier's documented
concurrency residual and asmspy's per-tid traces. Worst effort-to-payoff in the
set. Drop it.

## Top pick and rationale

**FP-environment / MXCSR-FPCR assertions.** It is the single clearest case of the
project's defining strength — inspect *every* dimension of architectural state
through the real ABI / vCPU — left unexploited, and the best verified
value-to-effort in the set. The user-facing capture structs demonstrably withhold
the FP status word ([include/asmtest_emu.h:62](../../../include/asmtest_emu.h#L62);
[include/asmtest.h:171](../../../include/asmtest.h#L171)), yet the codebase already
proves MXCSR is load-bearing and that read/write works
([src/dataflow_blockstep.c:149](../../../src/dataflow_blockstep.c#L149)). The
audience — SIMD/FP/crypto asm authors — is exactly who brings code here, and the
bugs it catches (wrong rounding mode, spurious/dropped sticky flags, FTZ/DAZ
denormal mishandling, NaN-payload corruption) pass `ASSERT_FP_NEAR` and every
other value-equality test yet are silent IEEE-754 violations. Effort is M, it
self-skips cleanly on guests without an FP status word, and it needs no new
hardware, credential, or heavy dependency.

It edges out the reshaped R1 (priority 8) because R1 carries scope-creep and a
partly-overclaimed "constant-time" banner, whereas FP-env is a clean, unclaimed
axis with feasibility already demonstrated in-tree. The deep pattern: the two
highest-ranked items are the **same move** — exposing state (the FP status word,
the memory-address channel) that is captured internally but withheld from users.

## What this does not cover / gates

- **Real-silicon timing** (the R1 dudect leg, any latency-leak claim) is
  host-noise-gated, not installable — keep it a clearly-labelled experiment, never
  a soundness guarantee.
- **The taint-precise legs** (R6 residue set, secret→address sink) ride the DR
  taint tier: Linux-x86-64, self-skip elsewhere.
- **R5 (Triton)** introduces un-vendored Z3/Bitwuzla + Boost and only ever proves
  `fn == one compilation of the C ref`, not `fn == spec`.
