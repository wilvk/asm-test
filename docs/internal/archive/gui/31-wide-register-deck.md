# R4: the wide / FP / vector register deck — implementation

> **Root R4 of the [extension roadmap](27-extension-roadmap.md).** Both `regstate`
> producers capture GPRs + rip + rflags only; the XMM/YMM/MXCSR/FP-env deck is a
> documented v1 omission for both alike. This root closes it once, for both, via
> the descriptor mechanism — the on-ramp is already laid.
>
> Depends on the `wide[]` serialization format from
> [R1](28-schema-freeze-completion.md) T3 (the register lanes spill to the same
> side buffer). Coordinate the new `fpenv` kind with the Phase-3 freeze (D5).
>
> Authored 2026-07-28, verified against HEAD `da566c9`.
>
> **LANDED 2026-07-28 (T1–T3), both docker lanes green.** What shipped, with the
> faithful deviations recorded:
> - **T1** — `asmtest_regfile_t` carries `xmm[16]` + `mxcsr` + `has_vec`; both
>   producers emit them behind one opt-in (`asmtrace_record --fpregs`, `asmspy
>   --dataflow --fpregs`, serve `fpregs:true`), off by default so the golden corpus
>   is byte-unchanged (D6). One field-order owner (`asmtrace_regstate_vec_append`)
>   serializes each XMM as a hex `bytes` string (R1 T3 convention), MXCSR as a u32.
>   The emulator reads XMM from its per-step ring + MXCSR from a new parallel ring
>   (`emu_step_mxcsr_at`, `emu_x86_regs_t` ABI untouched); the live ring reads all 16
>   XMM + MXCSR in one `PTRACE_GETFPREGS` **captured at the pre-state instant**
>   (`open_step`), not live at append time. `cli/test_regstate_parity.c` asserts the
>   two XMM decks byte-agree (via a zero-extending `movq xmm0, rdi` so the whole
>   128-bit register is base-independent — a scalar op would leave the upper lane at
>   basis-dependent entry state). **Deviation:** only 128-bit XMM ships; YMM high
>   halves / AVX-512 remain a further descriptor row (as the Non-goals foretold).
> - **T2** — `fpenv` promoted reserved → defined: one event per held step decoding
>   MXCSR into `round`/`ftz`/`daz`/`sticky[]`, emitted by both producers under the
>   same opt-in, paired 1:1 with `regstate`, absent (never faked) where unarmed.
>   **Deviation:** SSE MXCSR only (x87 deferred); the dedicated FP-env *panel* is a
>   draw-half follow-up — today `mxcsr` renders as a named register field and the
>   XMM hex strings are inert (the Scrubber index skips non-integer `values`).
> - **T3** — `asmtest_dataflow_emu_run_fp` marshals SSE-class doubles into xmm0..7,
>   and the value producer now captures XMM operand values into `wide[]` (matched by
>   reg id, not the enumerator's 0 size). The `fp-scale-add` golden (`addsd`, args
>   1.5/2.25) shows the whole FP computation: `df_step` xmm bytes 1.5+2.25→3.75, the
>   vector `regstate` deck, and `fpenv` — the FP analogue of add_signed, without a
>   flag. Its 5-byte routine has no 64-byte-window over-read, so its `code` sha is
>   stable (unlike the abixray fixtures, which re-churned on this root's layout).

## Why this work exists

Three deferred items are the same missing capture:

1. **The FP-env / MXCSR panel** — "GP + rip + rflags; no MXCSR — the Wave 1 gap
   the plan records" ([09](09-teaching-producers.md) register ring). The plan's
   expansion-intake table reserves an FP-environment panel (rounding mode, sticky
   flags, FTZ/DAZ) against an `fpenv` kind.
2. **Vector lanes in the Scrubber** — the register deck shows 18 u64 scalars; XMM
   worldlines cannot be shown because they are not captured.
3. **FP/vector-argument corpus routines** — "INTEGER-ARG routines only … widening
   the corpus to FP/vector routines needs a producer change, not a table entry"
   ([asmtrace_record.c:63-65](../../../../tools/asmtrace_record.c#L63)).

All three are one omission stated in the capture struct itself: "The XMM/YMM/FP
vector deck is a v1 omission for BOTH producers alike (a wide value is not a bare
u64, exactly the df_step `wide` limit); the descriptor mechanism absorbs it later,
for both at once" ([asmtest_valtrace.h:95-97](../../../../include/asmtest_valtrace.h#L95)).

## What already exists (verified 2026-07-28)

- **The descriptor on-ramp is already laid.** A `regstate` names a manifest struct
  row `"<struct>@<arch>/<abi>"`; `vec512_t` is now in the manifest, "which closes
  the precondition for a 512-bit register deck"
  ([asmtrace-schema.md:342-359](../../gui/asmtrace-schema.md#L342)). Recordings may embed the
  descriptor so a viewer renders the deck with no manifest on disk.
- **The reserved kind exists.** `fpenv` — "FP/SIMD environment + wide register
  state" ([asmtrace-schema.md:321](../../gui/asmtrace-schema.md#L321)).
- **The scalar deck names a vector slot already.** `emu_x86_regs_t` still names
  `xmm` in the manifest so `values` can extend without a new descriptor id
  (engine survey; schema regstate descriptor section).
- **The wide buffer exists** and R1 T3 defines its serialization: `wide[]` /
  `wide_off`, up to 64 bytes
  ([asmtest_valtrace.h:59-60,133-136](../../../../include/asmtest_valtrace.h#L59)).
- **The live engine already reads XMM/YMM — on demand only.** The `--dataflow`
  ptrace path reads GP + rip + rflags every step and XMM/YMM via XSTATE only when
  an operand needs it (cited by [26](26-live-regstate-producer.md)); the emulator
  can read XMM from Unicorn directly.
- **Both regstate emitters** — `emit_regstate`
  ([asmtrace_record.c:160-179](../../../../tools/asmtrace_record.c#L160)) and the live
  `asmtrace_regstate_body` ([cli/asmtrace_ndjson.c:240-268](../../../../cli/asmtrace_ndjson.c#L240)),
  both writing the same 18-field scalar body.

## Tasks

### T1 — the `regfile` vector extension + descriptor  (L)

**Goal.** Extend the per-step register file to carry the XMM/YMM vector deck +
MXCSR, described by a manifest descriptor so no reader changes.

**Steps.**
1. Extend `asmtest_regfile_t`
   ([asmtest_valtrace.h:98-102](../../../../include/asmtest_valtrace.h#L98)) with the
   16 XMM/YMM registers + MXCSR, spilling the wide lanes to `wide[]` (a register
   value > 8 bytes is exactly the `wide` case R1 T3 serializes).
2. Extend the manifest struct row so the descriptor carries the new fields; bump
   the descriptor id only if the layout changes incompatibly (prefer an additive
   `values` extension keyed by name, as the schema notes for `xmm`).
3. Emit the new lanes from **both** producers behind one opt-in (`--fpregs` on the
   recorder; `fpregs:true` on serve, echoed in `serve_params_json` like `steps`),
   off by default so the golden corpus is unchanged until armed.

**Done when.** An armed recording carries per-step XMM/MXCSR lanes described by
the embedded descriptor; an unarmed recording is byte-identical to today (D6); a
cross-producer parity test (extending `cli/test_regstate_parity.c`) asserts the
emulator and live vector decks agree.

### T2 — the `fpenv` event (rounding / sticky / FTZ-DAZ)  (M)

**Goal.** Define `fpenv` and emit the FP-environment fields the panel needs.

**Steps.**
1. Promote `fpenv` from reserved to defined in
   [asmtrace-schema.md](../../gui/asmtrace-schema.md): rounding mode, sticky exception
   flags, FTZ/DAZ, from MXCSR (and x87 control/status where present). Fix field
   order (D6); coordinate with the freeze (D5).
2. Emit it from both producers under the T1 opt-in (the environment is part of the
   wide register state the same flag arms).
3. Consumer: an FP-environment panel in the state views + an FP-env divergence
   class in the diff (both are new draw-half surfaces over the pure model — a
   small follow-up once the fields land).

**Done when.** An FP routine's recording shows rounding mode + sticky flags per
step; a low-fidelity fixture (armed but FP-env unread) degrades gracefully.

### T3 — SSE-class argument marshalling in the corpus  (M)

**Goal.** Let the corpus recorder run FP/vector-argument routines, retiring the
"integer-arg only" limit.

**Steps.**
1. Extend the value producer's arg seeding
   ([dataflow_emu.c:272-283](../../../../src/dataflow_emu.c#L272)) to marshal SSE-class
   arguments into XMM0–XMM7 per the SysV ABI, alongside the existing integer table.
2. Extend the `rec_routine_t` table
   ([asmtrace_record.c:63-74](../../../../tools/asmtrace_record.c#L63)) with FP/vector
   worked examples, arming `--fpregs` for them.
3. Golden: a committed FP worked example (mirroring how `add_signed` carries the
   `regstate` worked example without `--steps`).

**Done when.** An FP-argument routine records with correct XMM args and a vector
deck; `make asmtrace-golden` covers it byte-stably.

## Unblocks / downstream

- FP-env / MXCSR panel (Wave 1) and vector worldlines in the Scrubber, both with
  a small new draw-half over the extended pure model.
- FP/vector corpus routines (T3) — the Learn ladder and the golden corpus grow
  past integer leaves.
- **Shares the `wide[]` serialization with [R1](28-schema-freeze-completion.md)
  T3** — land R1 T3's format first; this root reuses it for register lanes, "for
  both [producers] at once."

## Non-goals / acknowledged limits

- The vector deck is captured only when **armed**; unarmed recordings stay
  GPR-only, faithfully (the descriptor tells the reader which lanes are present).
- No AVX-512 mask/zmm capture beyond what `vec512_t` describes; wider ISA state is
  a further descriptor row, not this root.
- FP-env is architectural state (MXCSR/x87), not microarchitectural timing — the
  usual native→virtual fidelity line holds.

## Cross-references

Depends on [R1](28-schema-freeze-completion.md) T3 (`wide[]` serialization),
[R5](32-per-guest-value-producer.md) for non-x86-64 decks. Schema/D5
[01](01-asmtrace-format.md); producers
[asmtrace_record.c](../../../../tools/asmtrace_record.c) (emulator) +
[cli/asmtrace_ndjson.c](../../../../cli/asmtrace_ndjson.c) (live);
consumers [09](09-teaching-producers.md) (ring/scrubber), [04](04-replay-views.md)
(diff). Golden/fidelity D6/D7.
