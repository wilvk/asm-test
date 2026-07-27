# Wave 4: live `regstate` producer — the Scrubber goes live — implementation

> The one remaining gap from the 2026-07-27 live-vs-replay audit
> ([25-live-model-wiring.md](25-live-model-wiring.md) closed the rest). The
> register time-travel **Scrubber** is the only desktop view still replay-only,
> because its `regstate` (per-step register file) events are produced *only* by
> the emulator record tier (`tools/asmtrace_record.c` `emit_regstate`) — no
> `asmspy --serve` mode emits them. This brief adds a **live** `regstate`
> producer to the serve/`--dataflow` single-step engine, which already reads the
> whole register file every step; it just never carries it to the sink.
>
> **Consumer is already done.** The Scrubber + its `StepIndex`
> (`desktop/src/analysis/stepindex.cpp`) read `regstate` by field name, and doc
> 25 promotes a live capture into the workspace model — so a live `regstate`
> stream makes `si.present()` true and the Scrubber lights up with **zero desktop
> change** (beyond T4's honesty banner). This is a producer brief.

## Why this work exists

Single-stepping to resolve exact operand values means the live `dataflow`/`auto`
engine already does **one `PTRACE_GETREGS` per step**, reading the full
`struct user_regs_struct` (rax..r15, rip, eflags) as the instruction's
**pre-state** — the same pre-state semantics the emulator `regstate` uses
(`src/dataflow_ptrace.c:1032-1035`, handed to `open_step` at `:1043`; design note
`:11-13`). Operand resolution indexes into that already-read struct
(`gp_value`, `src/dataflow_ptrace.c:231-343`). But `asmtest_valtrace_t`
(`include/asmtest_valtrace.h:93-116`) stores only per-step instruction offsets +
operand records — **no register file** — so the `regs` snapshot dies at the
bottom of each loop iteration. The whole producer is: keep that snapshot in a
per-step buffer and serialize it as a `regstate` event next to each `df_step`.

This also realises the register half of doc 25's live promise: an exact live
`dataflow` capture already lights Loom / Slice / Timeline / 3D; `regstate`
completes the set by lighting the Scrubber (still perturbing + torn, labelled).

## What already exists (verified 2026-07-27)

- **The reg file is read every step** — `PTRACE_GETREGS` at
  `src/dataflow_ptrace.c:1032`; cost model `:41-42` ("one SINGLESTEP + one
  GETREGS per in-region instruction"). GP + rip + rflags always; XMM/YMM read
  **on demand only** (`:646-647`, XSTATE `:379`) — which exactly matches the
  emulator `regstate` v1 omission (GPRs+rip+rflags, no vector deck).
- **The reference emitter** — `emit_regstate` (`tools/asmtrace_record.c:160-178`)
  writes `{"desc":"…","values":{rax…r15,rip,rflags}}`, 16 GPR + rip + rflags as
  decimal u64. The live producer reproduces this body (mapping
  `user_regs_struct.eflags` → the descriptor's `rflags`).
- **The single sink** — the engine calls `sink(ctx, result, vt, …)` once at
  end-of-capture (`cli/asmspy_engine.c:3686`); `serve_dataflow_sink`
  (`cli/asmspy.c:3180`) → `dataflow_record` (`cli/asmspy.c:2274-2302`) loops the
  valtrace emitting one `df_step` per step (`:2281-2295`). This is where the
  `regstate` emit joins — and because **serve and `--record` share this sink**, a
  live capture *and* a saved `--dataflow --record` file both gain the ring.
- **The serve param path** — `serve_params_t` (`cli/asmspy.c:2932`), parsed in
  `serve_parse_start` (`:3759`, one `sj_*` block per key, e.g. `max` `:3811`),
  threaded at `:3541` (SM_DATAFLOW) / `:3604` (SM_AUTO), echoed in
  `serve_params_json` (`:3422`).
- **The bound** — the valtrace `steps_cap` (`cli/asmspy_engine.c:3656`, truncate-
  when-full, honest via `vt->truncated`) and the serve `paused` gate
  (`cli/asmspy.c:144`). **Not** the emulator's drop-oldest ring.
- **The arch gate is free** — `asmspy_engine_dataflow` already refuses i386 at
  `cli/asmspy_engine.c:3635` (`ASMSPY_ETRACEE_I386`); the engine is x86-64-fixed
  below it, arch-aligned with the x86-64-only `regstate` descriptor.
- **The consumer** — `build_step_index` reads `regstate` `values` by the fixed
  name order rax…rflags and appends unknown keys sorted (`stepindex.cpp:11`,
  `:42-70`); `desc` is stored as chrome. So a live producer emitting the same
  field names lights the Scrubber unchanged — even under a new descriptor id.

## Tasks

### T1 — capture the per-step register file in the live dataflow engine  (M)

Add an **optional** per-step `struct user_regs_struct` buffer to
`asmtest_valtrace_t` (parallel to `insn_off`, `include/asmtest_valtrace.h:93`),
armed by a new flag on the valtrace / engine call. Populate it from the `regs`
already read at `src/dataflow_ptrace.c:1032`, at the `open_step` seam
(`:745`/`:1043`) so it is the instruction's pre-state. Grow it in lockstep with
the step buffer, bounded by the existing `steps_cap` (`cli/asmspy_engine.c:3656`)
— overflow stays honest via `vt->truncated`. x86-64 only (the engine already
refuses i386 at `:3635`); when disarmed, nothing is captured and the cost is one
untaken branch per step.

**Done when.** an armed live `dataflow`/`auto` capture holds exactly one GP-reg
pre-state per held step; a disarmed capture holds none and is byte-identical to
today's output; over-cap truncation is reported, not silent.

### T2 — emit `regstate` from the shared dataflow sink  (S, depends on: T1)

In `dataflow_record` (`cli/asmspy.c:2281-2295`), when the buffer is armed, emit
one `regstate` event per step **in `df_step` order** from the captured file,
reusing `emit_regstate`'s exact body (`tools/asmtrace_record.c:160-178`) with
`eflags` → `rflags`. Emit under a new descriptor **`user_regs@x86_64/sysv`** with
the **same field names** as the emulator's (so the Scrubber renders it unchanged)
but an honest id naming the ptrace source; embed the descriptor in the header
(`descriptors`, asmtrace-schema.md). Because the sink is shared, this lands for
both live serve sessions and `asmspy --dataflow --record` files at once.

**Done when.** a live `dataflow --steps` capture's recording carries N `regstate`
events aligned 1:1 with its `df_step` events, field-compatible with an emulator
`--steps` recording, so `build_step_index` indexes it and the Scrubber time-
travels it — over the live capture (doc 25) and over the saved file.

### T3 — the opt-in, wired through CLI + serve  (S, depends on: T2)

Add the enable across the three coordinated sites: a `--steps` flag on
`asmspy --dataflow`/`--auto` (CLI), a `steps` bool on `serve_params_t`
(`cli/asmspy.c:2932`) parsed in `serve_parse_start` (a `sj_bool` block like
`follow` `:3803`), threaded into the engine at `:3541`/`:3604`, and echoed in
`serve_params_json`'s dataflow branch (`:3422`) so the `started` event advertises
the effective value. **Document the semantic difference** from the emulator's
`--steps=N` (which sizes a drop-oldest ring): here `--steps` is a boolean that
adds the register ring over the window already bounded by `--max`.

**Done when.** `--steps` (CLI) and `steps:true` (serve) turn the ring on, the
`started` echo shows it, and its omission leaves output byte-identical to today.

### T4 — honesty: descriptor, provenance, Scrubber banner  (S, depends on: T2)

The live `regstate` is the **real architectural** register file (ground truth,
more authoritative than emulation) captured **perturbingly** (single-step). Embed
the `user_regs@x86_64/sysv` descriptor; extend doc 25 T5's perturb+torn banner to
the Scrubber pane when the active tab is the still-growing live capture; and make
the Scrubber's absent-reason accurate for a live `dataflow` capture *without*
`--steps` ("re-run the capture with `--steps` — this live session did not record
the register ring") distinct from the emulator's. A statistical (`sample`)
session never single-steps, so it never carries `regstate` — say so.

**Done when.** the descriptor is embedded and named honestly; the live Scrubber
carries the perturb+torn caveat; the absent-reasons distinguish live-no-`--steps`
from emulator-no-`--steps` from statistical-can't.

### T5 — tests  (M, depends on: T2, T3)

1. **CLI conformance** — a live `asmspy --dataflow --steps` capture of a small
   fixture emits `regstate` events carrying all 18 fields, one per `df_step`,
   under the embedded descriptor (a golden or a structural assert in the cli
   test lane). Runs on any x86-64 Linux (no silicon gate).
2. **Emulator parity** — for a deterministic routine, the live register file
   matches the emulator's per-step file — comparing **per-step register CHANGES**
   (which register each step wrote, and to what operand-visible value), **not**
   absolute values, since the live process has a real ASLR'd `rsp`/`rbp`/`rip`
   while the emulator uses its fixed `0x200000` stack base. This is the load-
   bearing correctness check: the live ring must agree with the trusted emulator
   ring on the value semantics, differing only by base offsets.
3. **Desktop** — a fed live `dataflow`+`regstate` recording makes the Scrubber
   present (`si.present()`) and time-travels, extending the doc-25 `test_shell`
   live-tab block. Trivial once the producer emits regstate (the consumer is
   already tested via the emulator path).

**Done when.** all three pass in their lanes (`make docker-cli` /
`make docker-desktop`); the parity test proves value-semantic agreement with the
emulator modulo base offsets.

## Task order & parallelism

`T1` (capture) → `T2` (emit) are the spine and must be sequential. `T3` (opt-in)
and `T4` (honesty) both build on `T2` and parallelise. `T5` verifies `T2`+`T3`.

Order: `T1` → `T2` → (`T3` ∥ `T4`) → `T5`.

## Constraints & gates

- **Not hardware-gated — this is "add it," not a silicon gate.** `PTRACE_GETREGS`
  works on any x86-64 Linux; the reg file is *already read*. Per the repo's
  missing-dependency rule, a capability the lane can test is added, not self-
  skipped. The only real gate is **x86-64-guest** (arch, free at the engine head)
  — arm64/RISC-V `regstate` is a separate producer + descriptor (out of scope).
- **Perturbing + torn, and labelled (23 / 25 T5).** Single-step capture perturbs;
  a growing capture is torn. The values are real; the run is not pristine. The
  Scrubber says so (T4).
- **Field-compatible with the emulator.** Same 16 GPR + rip + rflags names, so
  one Scrubber deck renders both producers; only the descriptor id differs, and
  the consumer keys on names, not id.
- **Consumer unchanged.** No `StepIndex` / Scrubber model change; doc 25 already
  wires the tab. The only desktop edit is T4's banner.
- **Bound reuses the live path's own mechanisms** — valtrace `steps_cap` +
  serve `paused` gate; never the emulator's drop-oldest ring.
- **Zero-cost when off.** A disarmed capture is byte-identical to today.

## Out of scope

- **arm64 / RISC-V `regstate`.** No non-x86-64 value producer exists (05 §out of
  scope); each needs its own engine path + `@<arch>/<abi>` descriptor.
- **The XMM/YMM/FP vector deck.** A v1 omission for the emulator too (a wide value
  is not a bare JSON integer); the descriptor mechanism absorbs it later, for
  both producers at once.
- **Zero-perturbation `regstate`.** A full register file cannot be reconstructed
  from an Intel-PT control-flow trace alone; a non-perturbing ring would come from
  replaying the PT path under the emulator (ties to [08](08-observer-views.md) T8
  / the PT-replay producer) — a separate rung.
- **Converging the emulator's own `regstate` path onto a shared valtrace reg
  buffer.** The emulator keeps `emit_regstates` (`tools/asmtrace_record.c`)
  unchanged; a later unification is a nice-to-have, not this brief.
- **The DynamoRIO taint stepper.** It fills the same `asmtest_valtrace_t` but is
  not `PTRACE_GETREGS` single-step; a `regstate` ring for it is a follow-on.
