# Block-step reconstruction — two pre-existing defects, reproduced (2026-07-17)

**Defect 1 (application `int3`): Status FIXED (T1, [ptrace-blockstep-tracer-correctness.md](../implementations/ptrace-blockstep-tracer-correctness.md))** in the region + attached block-step drivers — an app int3 is now recorded up to the trap byte, marked truncated, and the signal forwarded (region: PTRACE_CONT; attached: left in the delivery-stop). The windowed driver's leg co-lands with T2.**

**Defect 2 (`rep`-prefixed string ops): Status FIXED (T3, same doc)** — `bs_record_run` and `window_block_walk` now detect a `rep`-prefixed string op (via `asmtest_disas_is_rep_string`) and downgrade the block to BS_AMBIGUOUS, so the capture is honestly truncated instead of silently recording the op once where per-instruction stepping records it N times.**

**Status (below is the original filing): OPEN. Both are real, both reproduce against shipped `main` code, neither is fixed.**
Filed so they are not lost: they were found while adversarially reviewing the W-1 windowed
block-step driver, and were deliberately left out of that change because they are **not**
introduced by it — they are properties of the block-step reconstruction *approach*, shared by
every driver that uses it.

## Scope: who is affected

Affected — the **Capstone-only static reconstructors** in `src/ptrace_backend.c`:
- `blockstep_reconstruct` (region form) → backs `asmtest_ptrace_trace_call_blockstep` and
  `asmtest_ptrace_trace_attached_blockstep`
- `window_block_walk` (windowed form, added `ee696e0`) → backs
  `asmtest_ptrace_trace_attached_windowed_blockstep`

**NOT affected — the F1 data-flow block-step value tier** (`src/dataflow_blockstep.c`). It has
its own driver and does **not** call `blockstep_reconstruct`. Its `step_block`
([dataflow_blockstep.c:679](../../../src/dataflow_blockstep.c#L679)) does not statically guess
the terminator: it *executes* each instruction through Unicorn with real register/flag state and
reads back RIP, so a not-taken `Jcc` falls through correctly and a taken one is recognised as the
real boundary. It even carries an explicit divergence detector. **Emulator replay is immune to
this whole class**, at the cost of needing an emulator that can execute the instruction — which
is exactly F1's separate AVX gate. Worth remembering when weighing the two designs.

A third defect of the same family — same-target-conditional ambiguity — **was** fixed in
`ee696e0`; see [amd-tracing-plan.md](../plans/amd-tracing-plan.md) Part III Phase 2.

---

## Defect 1 — an in-window `SIGTRAP` is misclassified as a BTF trap (int3 / breakpoints)

**Root cause.** The drivers assume "a `SIGTRAP` stop is *my* block-step `#DB`". In the windowed
driver this is literally `at_mode = (sig != SIGTRAP)` (`src/ptrace_backend.c`); the region driver
has the same assumption at its `if (WSTOPSIG(status) != SIGTRAP)` gate. Neither consults
`PTRACE_GETSIGINFO`. The idiom is file-wide (lines 734, 1130, 1381, 1659, 1821, 1976, 2491, 2640
on `main`), so this is an assumption of the whole ptrace tier, not of one driver.

**Consequence.** An `int3` raises `SIGTRAP`. `int3` is not in Capstone's JUMP/CALL/RET/IRET
groups, so `asmtest_disas_is_branch` returns 0, the walk steps *through* it, and the block tail is
recorded twice — instructions that never executed are appended, `truncated` stays false.

**Reproduced** (fixture `53 48 89 fb CC 48 89 d8 48 01 c0 5b c3`), identically through the
**shipped** region driver on `main` and through the new windowed one:

```
per-instruction  rc=0 res=42 trunc=0 n=7 : +0 +1 +4 +5 +8 +11 +12
block-step       rc=0 res=42 trunc=0 n=11: +0 +1 +4 +5 +8 +11 +12 +5 +8 +11 +12
```

**Why it matters.** This driver's stated purpose is "the rootless managed-runtime completeness
fallback". JVM safepoint polls and .NET use `int3`. This is the target domain, not a corner.

**Interaction with the ambiguity fix (`ee696e0`).** The scan now hunts a terminator targeting
`int3+1`; where a candidate exists the result surfaces as **ambiguous → truncated** rather than a
clean double-record. Less wrong, still wrong. The fix neither addresses nor masks this.

**Also note:** the per-instruction *oracle* swallows it too — `trace_attached_window_loop`
(`:1976`) only sets `pending_sig` for non-`SIGTRAP`, so an application `int3`'s signal is never
re-injected. A real JVM safepoint would be broken by the oracle as well. So this is not a
block-step-only bug.

**Fix shape.** Consult `PTRACE_GETSIGINFO` `si_code` to separate `TRAP_TRACE`/`TRAP_BRANCH` (our
`#DB`) from `TRAP_BRKPT` (the application's `int3`), and forward the latter. Must land in
`blockstep_reconstruct`, the region driver, and `window_block_walk` together. There is precedent
in-tree: the data-flow Inc5 work already did an si_code signal split.

---

## Defect 2 — `rep`-prefixed string ops are reconstructed once but retire N times

**Root cause.** Reconstruction is architecturally blind to any non-branch instruction that
retires more than once. BTF records only control transfers, so between two stops the walk
advances by instruction *length* — `rep stosq` with `RCX=8` is walked once.

**Confirmed:** a ptrace harness single-stepping `rep stosq` (RCX=8) produces **8** consecutive TF
stops with RIP parked on the same instruction, RCX decrementing 8→1 (Intel SDM Vol 3 §18.3.1.4).
`asmtest_disas_is_branch` (`src/disasm.c:225-250`) tests only JUMP/CALL/RET/IRET, so the walk
classifies it straight-line and records it once. The per-instruction oracle
(`src/ptrace_backend.c:1998-2000`) has no dedup and records it 64 times. The asymmetry is real;
existing fixtures (add/sub chains, `movabs`/`call`/`dec`/`jnz`) cannot see it.

**Why it matters.** `Array.Clear` / inlined `memset` in JIT'd code — again the target domain.

**Fix shape.** This one is arguably a *contract* bug rather than a code bug: the header promises
the block-step form "reconstruct[s] the identical per-instruction stream"
(`include/asmtest_ptrace.h:119` on `main` for the region form; the windowed form restates it).
Either bound that promise explicitly to exclude `rep`-prefixed string ops, or detect the `rep`
prefix and set `truncated` (consistent with the ambiguity rule: honest degradation over a
silently wrong stream). Do **not** try to infer the iteration count statically — it is `RCX` at
entry, which the reconstructor does not have.

---

## Why the suites missed both

Neither is a "vacuous green" in the strict sense — no test passes for the wrong reason. Both are
**non-exhaustive fixtures**: the block-step oracles compare block-step against per-instruction
over fixtures containing neither an `int3` nor a `rep` prefix, so the differential is honest but
blind. The lesson matching this repo's history is that a differential oracle is only as good as
the shapes its fixture generates — 6,410 identical instructions proved nothing about the shapes
that were absent.

Both repros are ready-made and cheap; each is a fixture plus an existing differential harness.
