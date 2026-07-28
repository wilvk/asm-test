# R1: schema freeze-completion — routine identity, step totals, wide values — implementation

> **Root R1 of the [extension roadmap](27-extension-roadmap.md).** Three open
> Phase-3-freeze items, all in the single `.asmtrace` writer, none needing a new
> engine. Each closes a standing GUI honesty gap where a view today states a limit
> instead of a finding. This is the cheapest root and a prerequisite for
> [31 (wide register deck)](31-wide-register-deck.md) and
> [33 (statediff)](33-backward-attribution-producers.md).
>
> **Coordinate with the Phase-3 schema freeze (D5).** The schema is owned by
> [01](01-asmtrace-format.md) and append-only until the freeze; these three items
> are the [freeze checklist](asmtrace-schema.md#known-v1-gaps--the-freeze-checklist)
> itself. Land them through the freeze checkpoint, not around it.
>
> Authored 2026-07-28, verified against HEAD `da566c9`.

## Why this work exists

Three GUI views today degrade honestly because a fact the render layer wants is
not on the wire — even though the producer already **has** it in memory:

1. **The diff cannot verify two recordings are the same routine.** `dt_diff_build`
   ([04](04-replay-views.md) T6) checks only `basis` + `arch` and prints, in every
   diff, that routine sameness is *the reader's assertion, not a finding*
   ([test_diff.cpp](../../../desktop/test/test_diff.cpp) pins "v1 cannot refuse a
   wrong-routine pair").
2. **Truncation banners cannot say N-of-M.** The Loom's banner ([05](05-loom-day-one.md)
   T2) and the Scrubber's tear both want the total step count; the `end` footer
   carries `truncated` (bool) but no total, so they name the gap instead of the
   ratio.
3. **`df_step` wide operands render as `[wide]`, not bytes.** An operand > 8 bytes
   sets `"wide":true` and omits `value`; the reader shows a placeholder.

In all three the number/bytes already exist — `steps_total` is measured
([asmtest_valtrace.h:115](../../../include/asmtest_valtrace.h#L115)), the routine
bytes are copied into a fixed 64-byte window
([asmtrace_record.c:55](../../../tools/asmtrace_record.c#L55), `REC_WINDOW`), and
the wide operand bytes are spilled to the valtrace `wide[]` side buffer
([asmtest_valtrace.h:59-60](../../../include/asmtest_valtrace.h#L59)). Nothing
serializes them.

## What already exists (verified 2026-07-28)

- **One writer TU.** [`cli/asmtrace_ndjson.c`](../../../cli/asmtrace_ndjson.c) is
  the sole owner of field order (schema Determinism rule 1). Header fields are
  emitted at `:91-116`; the `end` footer at `:270-288`; `df_step` operand bodies
  (`op_body`, the `"wide":true` / omit-`value` branch) at `:199-202`.
- **The freeze checklist** names all three with the fix pre-sketched
  ([asmtrace-schema.md:400-418](asmtrace-schema.md#L400)): "A `code` header object
  (`{"name":str,"sha256":str,"len":int}`) would close it; the corpus recorder
  already copies a fixed 64-byte window and could hash it."
- **The corpus recorder holds the bytes.** `REC_WINDOW = 64`
  ([asmtrace_record.c:55](../../../tools/asmtrace_record.c#L55)); the routine is
  mapped at `REC_CODE_BASE` (`:51`) and `memcpy`'d into the window before the run.
- **The totals are measured but never written.** `asmtest_valtrace_t.steps_total`
  ("steps seen (counts past `steps_cap`)",
  [asmtest_valtrace.h:115](../../../include/asmtest_valtrace.h#L115)) and
  `recs_total` (`:131`); both advance past the cap so overflow is honest.
- **The wide buffer is populated but not emitted.** `at_val_rec.wide` /
  `wide_off` and the sink's `wide[]` / `wide_len`
  ([asmtest_valtrace.h:81-83,133-136](../../../include/asmtest_valtrace.h#L81)).

## Tasks

### T1 — `code` header object (routine identity)  (S)

**Goal.** Add an optional header object `code:{"name":str,"sha256":str,"len":int}`
where the producer has the routine bytes.

**Steps.**
1. Extend `asmtrace_open` (or the header struct it serializes) with an optional
   code-identity triple; emit it in `cli/asmtrace_ndjson.c` header block
   (`:91-116`) after `provenance`, omitted when absent (like `descriptors`).
   `sha256` reuses the repo's existing hash tier (no new dependency).
2. Fill it in the corpus recorder from the `REC_WINDOW` bytes it already copies
   ([asmtrace_record.c](../../../tools/asmtrace_record.c)); `name` = the routine
   name from the `rec_routine_t` table, `len` = the byte length hashed.
3. Fill it in the live `--dataflow` path where the routine window is known;
   where a producer genuinely lacks stable bytes (a live attach with no fixed
   window), **omit** the object — never emit a zero hash.
4. Consumer: `dt_diff_build` ([04](04-replay-views.md) T6) upgrades from "identity
   is the reader's assertion" to a hard refusal when both recordings carry `code`
   and the `sha256` differs; when either lacks it, it keeps the current honest
   caveat. No new UI — the refusal rides the existing diff-mismatch placard.

**Done when.** `desktop-test` diff over a `code`-bearing golden pair refuses a
mismatched-routine pair and accepts a matching one; a `code`-less recording keeps
the caveat. Golden bytes updated deterministically (D6).

### T2 — `steps_total` footer field (N-of-M)  (S–M)

**Goal.** Serialize the total step count so truncation banners read "N of M".

**Steps.**
1. Add `steps_total` to the `end` footer in `cli/asmtrace_ndjson.c` (`:270-288`),
   sourced from `vt->steps_total`. Additive field — readers ignore unknown fields
   (schema Compatibility rule).
2. **The medium part is the live tail-drop case.** The emulator drop-oldest ring
   already reconstructs the total (`held + drops.lost`); the scene recorder does
   too (`lost = nsteps - kept`). But the live ptrace dataflow ring is
   truncate-when-full (drops the **tail**) and passes `lost=0`, so today only
   `truncated:true` is known. Make that producer count past-cap steps into
   `vt->steps_total` (the field exists; the counter must actually advance in the
   tail-drop path) so `steps_total` is meaningful for a live file too.
3. Consumers: the Loom banner ([05](05-loom-day-one.md) T2) prints the exact
   N-of-M form when the footer supplies `steps_total`, keeping its
   "total unknown" wording only when the field is absent; the Scrubber tear
   annotates with the same total.

**Done when.** A truncated golden's banner prints the real ratio; a
`steps_total`-less recording keeps the current honest wording;
`test_loom_plan` / `test_scrubber` updated.

### T3 — serialize the `wide[]` side buffer  (S–M)

**Goal.** Emit the bytes of a `>8`-byte operand instead of `[wide]`.

**Steps.**
1. In `op_body` (`cli/asmtrace_ndjson.c:199-202`), when `rec.wide` and
   `rec.value_valid`, emit the `size` bytes at `vt->wide + rec.wide_off` as a
   fixed lowercase-hex string (a new additive field, e.g. `"bytes":"…"`) rather
   than omitting `value`. Keep the `"wide":true` flag for readers that skip it.
2. Freeze decision to record in [asmtrace-schema.md](asmtrace-schema.md): the
   `df_step` wide-value gap moves from "documented v1 limit" to defined; the field
   is bounded (≤ 64 bytes, the `wide[]` element cap).
3. Consumer: `desktop/src/doc/streams.h` renders the hex (or a decoded
   little-endian view) instead of `[wide]`; absence still degrades to `[wide]`.

**Done when.** A golden carrying an XMM operand shows its bytes; the dishonesty
fixture (wide-but-invalid) still degrades honestly; `test_golden` updated.

## Unblocks / downstream

- Directly closes three [freeze-checklist](asmtrace-schema.md#known-v1-gaps--the-freeze-checklist)
  items (identity, wide buffer) plus the step-total gap raised by 05.
- **T1 is a prerequisite for [R6 statediff](33-backward-attribution-producers.md)**
  (pairing two recordings needs identity) and strengthens any two-recording view.
- **T3 shares the `wide[]` plumbing with [R4](31-wide-register-deck.md)** — the
  register deck's vector lanes spill to the same buffer; land T3's serialization
  format so R4 reuses it verbatim (the code comment at
  [asmtest_valtrace.h:95-97](../../../include/asmtest_valtrace.h#L95) says the
  descriptor mechanism "absorbs it later, for both at once").

## Non-goals / honest limits

- **The fourth freeze-checklist item — "no block starts from the L0 producer"** —
  is out of scope here: it needs block information the value producer does not
  measure, not a serialization change. Leave it on the checklist for the freeze to
  confirm or fund separately.
- A `code` hash proves the **bytes** match, not that two runs used the same ABI or
  arguments — the diff keeps stating what it does and does not verify.

## Cross-references

Schema home [01](01-asmtrace-format.md) / D5; consumers
[04](04-replay-views.md) (diff), [05](05-loom-day-one.md) (banner),
`desktop/src/doc/streams.h`. Golden/honesty contract D6/D7.
