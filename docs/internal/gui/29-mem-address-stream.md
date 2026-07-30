# R2: the `mem[]` address-stream producer — implementation

> **Root R2 of the [extension roadmap](27-extension-roadmap.md).** One reserved
> schema kind — `mem` — with **no producer today**, blocking three otherwise-built
> views. This is producer + schema work; the consumers are already inert-ready and
> flip on when the kind appears.
>
> Authored 2026-07-28, verified against HEAD `da566c9`.

## Why this work exists

`mem` is a registry row claimed by [10](10-spacetime-3d-overview.md) with the
intended payload `{step,ea,size,rw}` (per-step memory access), but no v1 producer
emits it ([asmtrace-schema.md:320](asmtrace-schema.md#L320)). Three deferred
views all wait on exactly this one stream:

1. **The 3D "rich rung"** — per-access data-density terrain. The consumer is
   already written and gated at runtime on the kind being present, staying inert
   until it appears: "there is no producer yet, so a real recording never carries
   it — the path stays inert. Absent it, data cells stay flat and the note says
   so, never a silent zero"
   ([terrain.cpp:124-130](../../../desktop/src/space/terrain.cpp#L124)).
2. **The secret-class CT-invariance multiverse** — two secret-class runs with
   first-divergence highlighted needs a `mem[]` stream to compare access
   addresses ([05](05-loom-day-one.md) growth-rung; plan Wave 1).
3. **Misaligned / uninitialized-access views** — the plan's expansion rows keyed
   on per-access effective addresses.

The effective address is **already resolved at run time** for operands — the
valtrace record's `addr` field is "resolved to the effective address by a PRODUCER
at RUN time" ([asmtest_valtrace.h:56-74](../../../include/asmtest_valtrace.h#L56)).
So both producers already compute the EA per access; nothing carries it to the
sink as a first-class per-access stream.

## What already exists (verified 2026-07-28)

- **The reserved kind + its consumer.** `mem` row at
  [asmtrace-schema.md:320](asmtrace-schema.md#L320); the 3D consumer's gate,
  `mem_present` / `mem_note`, at
  [terrain.cpp:124-130](../../../desktop/src/space/terrain.cpp#L124).
- **The EA is computed on both producers.** Emulator L0: `df_on_mem`
  (`UC_HOOK_MEM_READ_AFTER` / `UC_HOOK_MEM_WRITE`) fires per access with the
  address ([dataflow_emu.c:295-297](../../../src/dataflow_emu.c#L295)); the record
  carries `addr`, `size`, `is_write`
  ([asmtest_valtrace.h:61-86](../../../include/asmtest_valtrace.h#L61)). Live
  ptrace: operand resolution indexes the read register file (`gp_value`, cited by
  [26](26-live-regstate-producer.md)).
- **The normalization space is on the sink.** `mem_space`
  (`AT_LOC_MEM_ABS` / `AT_LOC_MEM_OFF`,
  [asmtest_valtrace.h:139-140](../../../include/asmtest_valtrace.h#L139)) already
  distinguishes absolute vs routine-relative EAs — the same normalization the
  `mem` event must carry so a reader can fold addresses to region offsets.
- **The single writer.** [`cli/asmtrace_ndjson.c`](../../../cli/asmtrace_ndjson.c)
  owns field order; a new `mem` body joins next to `df_step`.

## Tasks

### T1 — define the `mem` kind and its writer  (S)

**Goal.** Move `mem` from reserved to defined: `{"k":"mem","step":int,"ea":int,
"size":int,"rw":"r"|"w","space":"abs"|"off"}`.

**Steps.**
1. Define the fields in [asmtrace-schema.md](asmtrace-schema.md) (append under the
   defined kinds; update the reserved-kinds table to point at this doc, mirroring
   how `codeimage` was promoted). Fix field order for D6.
2. Add the writer body in `cli/asmtrace_ndjson.c`, emitting `space` from the
   sink's `mem_space` so a reader can normalize.
3. Determinism: `ea` is an integer in decimal; in region/`off` mode it is the
   routine-relative offset already available, so absolute base addresses do not
   leak into golden bytes.

**Done when.** The writer round-trips a hand-built `mem` event byte-stably;
schema updated; a reader ignores it gracefully before T2 lands (Compatibility
rule).

### T2 — emit `mem` from the emulator L0 producer  (M)

**Goal.** Have `asmtest_dataflow_emu_run` emit one `mem` event per memory access,
in step order, alongside its `df_step` stream.

**Steps.**
1. In `df_on_mem` ([dataflow_emu.c:295-297](../../../src/dataflow_emu.c#L295)),
   record `{step, ea, size, rw}` per access into a caller-owned buffer (a new
   optional sink array, armed like the regfile ring — off by default, faithful
   overflow via a `truncated` flag).
2. Serialize the buffer through the shared sink loop next to `df_step`.
3. Gate it behind an opt-in (`--mem` on the recorder), off by default so the
   golden corpus is unchanged unless armed — the same discipline `--steps` uses
   ([asmtrace_record.c:57-61](../../../tools/asmtrace_record.c#L57)).

**Done when.** A `--mem` recording of a load/store routine carries one `mem` per
access; the 3D rich rung lights up (`mem_present` true, data cells carry density)
with **no desktop change**; a torn `mem` capture degrades to the coarse rung.

### T3 — emit `mem` from the live `--dataflow` engine  (M)

**Goal.** The live ptrace/serve path emits `mem` from the EA it already resolves
per access, so a live capture drives the rich rung too (the doc-25/26 live-parity
pattern).

**Steps.**
1. Thread a `mem` capture into the live dataflow loop where operand EAs are
   resolved; reuse the T1 writer body and the T2 sink array.
2. Add the serve/`--record` opt-in (`mem:true`), echoed in `serve_params_json`
   exactly as `steps` is ([26](26-live-regstate-producer.md) T-serve).
3. Parity: a cross-producer test asserting the emulator and live `mem` streams
   agree on a shared fixture, mirroring `cli/test_regstate_parity.c`.

**Done when.** A live `--dataflow --mem` capture drives the rich rung live and
torn/perturbing under the labelled banner; parity test green in both docker lanes.

## Unblocks / downstream

- **3D rich rung** ([10](10-spacetime-3d-overview.md)) — the coarse rung already
  ships; T2/T3 flip it to per-access density with no consumer change.
- **CT-invariance multiverse** ([05](05-loom-day-one.md) growth-rung) — the `mem`
  stream is the address channel the two secret-class runs are compared on; the
  divergence view is a new consumer (a follow-up brief, not this root).
- **Misaligned / uninit-read** expansion rows become derivable from `{ea,size}`.

## Non-goals / acknowledged limits

- `mem` carries the **effective address and width**, not the bytes read/written
  (that is the `wide[]` value channel, [R1](28-schema-freeze-completion.md) T3) and
  not taint (`taint` is a separate reserved kind).
- No timestamps — the ordering axis stays the per-step ordinal (plan's killed
  producer-timestamp decision; see the [roadmap](27-extension-roadmap.md)
  out-of-scope list).
- Absolute EAs are meaningful only within one capture's address space; the reader
  normalizes via `space` and never compares raw absolute EAs across recordings.
- **The two producers capture memory by different mechanisms, and `mem` inherits
  the difference** (discovered 2026-07-28 implementing T2/T3): the emulator L0 uses
  Unicorn's hardware memory hooks, so it sees EVERY access including implicit stack
  traffic (a `ret` popping the return address, a `push`/`pop`, a `call`); the live
  ptrace producer enumerates Capstone operands, so it records only EXPLICITLY-
  encoded memory operands. The live `mem` stream is therefore a **subset** of the
  emulator's — the same split `df_step.ops` already carries, since both streams are
  projections of the same operand records. The parity test
  ([`cli/test_mem_parity.c`](../../../cli/test_mem_parity.c)) asserts the sound
  invariant: every live access matches an emulator access on `(step,size,rw)`, the
  emulator may carry additional implicit accesses, and the effective addresses
  differ by base.

## Cross-references

Schema/D5 [01](01-asmtrace-format.md); consumer [10](10-spacetime-3d-overview.md)
([terrain.cpp](../../../desktop/src/space/terrain.cpp)); producers
[dataflow_emu.c](../../../src/dataflow_emu.c) (emulator) and the `--dataflow`
engine (live, D9). Golden/fidelity D6/D7.
