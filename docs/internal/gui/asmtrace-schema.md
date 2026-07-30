# `.asmtrace` — recording contract (**draft**)

> **Status: draft, not frozen.** The v1 *freeze* is the named Phase-3 checkpoint
> in [desktop-gui-plan.md](../plans/desktop-gui-plan.md), after
> [02-exporters-and-readers.md](02-exporters-and-readers.md),
> [03-desktop-shell.md](03-desktop-shell.md) and
> [07-serve-live-host.md](07-serve-live-host.md) have consumed it. Until then a
> field may still move — but a move must regenerate the golden corpus
> (`make asmtrace-golden`) in the same change, because the bytes are the test.
>
> **Ownership (D5).** This file is created and owned by
> [01-asmtrace-format.md](01-asmtrace-format.md); other docs **append** rows to
> the kind registry (07's `session`/`cmd`/`err`, 08's `codeimage`) and never
> rewrite what is here. If this file and the CODE disagree, the code wins:
> re-verify, then fix this file in the same change.

One format carries every recording in the tree: what asmspy captured live, what
the Author-mode recorder produced under the emulator, what a `--serve` session
streams, and what the desktop viewer replays. There is no second "viewer
format" — a viewer is a reader of this, an exporter is a writer of something
else *from* this.

Two properties are load-bearing and are enforced by the format rather than left
to renderer discipline:

- **Provenance is mandatory.** Every stream states which backend produced it and
  whether it is exact. A reader can always answer "how do you know?".
- **A fidelity loss is a field, not an omission.** Truncation, drops, throttling,
  redaction and a torn (unterminated) file are all *representable* and therefore
  *testable* — see
  [`tests/golden-asmtrace/`](../../../tests/golden-asmtrace/README.md), whose
  four hand-authored `low-fidelity/` fixtures each carry a `note` event stating
  what a reader must conclude from them.

## Envelope

A recording is **NDJSON**: UTF-8, one JSON object per line, `\n`-terminated, no
trailing commas, no comments. **Line 1 is the header**; every later line is one
event.

```
{"asmtrace":1,"container":"ndjson","producer":{"name":"asmspy","version":"1.1.0"},"provenance":{...},"arch":"x86_64","pid":4242,"cmd":"./victim"}
```

Header fields, in this order:

| Field | Type | Required | Meaning |
|---|---|---|---|
| `asmtrace` | int | yes | Format major. A reader **rejects** `> 1` by name. |
| `container` | str | yes | `"ndjson"` in v1. |
| `producer` | obj | yes | `{"name":str,"version":str}` — `name` is `"asmspy"` or `"asmtrace_record"`; `version` is [`ASMTEST_VERSION`](../../../include/asmtest.h#L44). |
| `provenance` | obj | yes | See below. |
| `arch` | str | yes | `"x86_64"`, `"aarch64"`, … (the *recording host* arch). |
| `code` | obj | no | `{"name":str,"sha256":str,"len":int}` — the identity of the recorded routine's bytes (see *Routine identity* below). Omitted by a producer without stable bytes. |
| `descriptors` | array | no | Embedded state descriptors (see *State descriptors*). |
| `pid` | int | no | Traced pid. **Omitted in deterministic mode.** |
| `cmd` | str | no | Traced command line. **Omitted in deterministic mode.** |
| `ts` | int | no | Unix epoch seconds at open. **Omitted in deterministic mode.** |

`"container":"zstd-frames"` is **reserved and not implemented in v1**. A v1
reader must refuse it *by name* ("zstd-frames container is not supported by this
reader") and must never attempt to parse it as NDJSON — a misparse of a
compressed file is exactly the silent-wrong-answer this format exists to
prevent.

## Provenance

Mandatory on the header; the same object shape is reused wherever a per-event
override is later needed.

```json
{"backend":"ptrace-syscalls","exact":true,"trust":"exact","skip":{"code":2,"reason":"..."},"redacted":false}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `backend` | str | yes | Measured producer id: `ptrace-syscalls`, `ptrace-stream`, `ptrace-region`, `ptrace-tree`, `ptrace-graph`, `ptrace-procs`, `ptrace-dataflow`, `hwdebug-watch`, `ibs-op`, `sw-clock`, `emu-l0`. |
| `exact` | bool | yes | `true` = every event in the window was observed; `false` = a **sample**. |
| `trust` | str | yes | `"exact"` \| `"statistical"` \| `"weak"` \| `"strong"` — the tier vocabulary already used by the trace tiers. |
| `window` | obj | no | `{"base":u64,"len":u64}` when the capture was scoped to a region. |
| `skip` | obj | no | `{"code":int,"reason":str}` — `code` is the **positive** asmspy skip code, `reason` the *measured* string. |
| `redacted` | bool | no | `true` when payload content was withheld **at record time** (it is not in the file at all). |

Rules:

- A statistical stream MUST set `"exact":false` and `"trust":"statistical"`. A
  reader MUST NOT merge its events into an exact kind's data
  (a `survey` edge is evidence an edge was *seen*, never that one was *not
  taken*).
- `skip.code` is the positive engine code, not an errno:
  [`ASMSPY_REGION_NEVER_RAN`](../../../cli/libasmspy.h#L271) 1,
  [`ASMSPY_SAMPLE_UNAVAIL`](../../../cli/libasmspy.h#L559) 2,
  [`ASMSPY_DATAFLOW_UNAVAIL`](../../../cli/libasmspy.h#L321) 3,
  [`ASMSPY_WATCH_UNAVAIL`](../../../cli/libasmspy.h#L590) 4,
  [`ASMSPY_ETRACEE_I386`](../../../cli/libasmspy.h#L597) 5. `reason` comes from the
  measuring source — [`asmtest_ibs_unavail_reason`](../../../include/asmtest_ibs.h#L137),
  [`asmspy_hwdebug_reason`](../../../cli/libasmspy.h#L646),
  [`asmspy_strerror`](../../../cli/libasmspy.h#L653) — never a guess.
- **A skip is a recording.** A run that skipped still writes a header and a
  clean `end`; the `end` carries the skip. An empty file is a *bug*; a
  skip-carrying file is *data*.

## Routine identity — the `code` header

```json
{"...":"...","arch":"x86_64","code":{"name":"add_signed","sha256":"…64 hex…","len":64}}
```

An **optional** header object naming the recorded routine by the **SHA-256 of
its bytes**, so a consumer can prove two recordings are — or refuse them as not —
the same routine (28 R1 T1). `dt_diff_build` ([04-replay-views.md](04-replay-views.md)
T6) refuses a pair whose `code.sha256` differ; when either side omits `code` it
keeps its faithful "identity not checked" caveat.

| Field | Type | Required | Meaning |
|---|---|---|---|
| `name` | str | yes | The routine name (informational; the hash is authoritative). |
| `sha256` | str | yes | Lowercase-hex SHA-256 of the hashed bytes. |
| `len` | int | yes | The number of bytes hashed. |

Rules:

- **A `code` header is emitted only where the producer holds stable bytes.** The
  Author-mode corpus recorder hashes its fixed 64-byte routine window; the live
  `--dataflow` producer hashes the resolved region it read. A producer that
  genuinely lacks stable bytes (a live attach with no fixed window) **omits the
  object** — never a zero or placeholder hash.
- The hash is over the producer's captured window, so `len` is the **window**
  length, not necessarily the routine's true extent. A `code` match proves the
  **bytes** match, not that two runs used the same ABI or arguments — the diff
  still states what it does and does not verify.
- `sha256` is a real FIPS-180-4 SHA-256 (the producer's
  [`cli/asmtrace_sha256.h`](../../../cli/asmtrace_sha256.h), a self-contained
  pure-C digest, not the `asmspy_ghash.h` hash-table router), so a cross-language
  reader can recompute and compare it.

## Event kinds (v1)

The field `"k"` selects the kind and is always first. Field order below is
**normative** — the writer emits exactly this order (see *Determinism rules*).
Optional fields are marked `?` and are **omitted entirely** when absent, never
emitted as `null`.

### `trace` — one executed instruction or block

```json
{"k":"trace","basis":"rel","kind":"insn","off":18,"disasm":"add eax, esi"}
```

`basis` is **mandatory** and may never be defaulted by a reader: the region
scope fills [`asmtest_trace_t`](../../../include/asmtest_trace.h#L44) with
offsets *relative* to the registered routine ([the struct's contract](../../../include/asmtest_trace.h#L42)),
while the region-free whole-window scope fills the *same* struct with
**absolute** addresses ([asmtest_hwtrace.h:453](../../../include/asmtest_hwtrace.h#L453)).
Both are real; a reader that assumes one silently mislocates the other.
`disasm` is D10 (offline disassembly): producers may attach the instruction text
at record time so a render-only viewer needs no Capstone; absent, a reader
degrades to bare offsets and says so.

### `coverage` — the distinct-block set of one invocation

```json
{"k":"coverage","basis":"rel","blocks":[0,12,40],"blocks_total":5,"insns_total":37,"truncated":false}
```

`blocks` is the de-duplicated block-start set actually recorded;
`blocks_total`/`insns_total` are the totals *seen* — they count past the
buffer caps, which is what makes `blocks_total > len(blocks)` a faithful
truncation signal rather than a lost fact.

### `syscall` — one decoded syscall

```json
{"k":"syscall","line":"openat(AT_FDCWD, <path>, O_RDONLY) = 3","payload":"/etc/passwd","tid":4243}
```

`line` is the **payload-free** rendering: syscall name, fds, flag words, counts
and return value are identical to the full text, but every *content* helper
writes a placeholder (`<path>`, `<sockaddr>`, `<N bytes>`). The decoded content
travels in the separate `payload` field, which renderers **default-redact**
(D7). When the recording was made with redaction on, `payload` is absent
entirely and the header's `provenance.redacted` is `true`.

Redaction covers everything the engine reads *out of the target*: buffer bytes,
path arguments, sockaddrs, iovec contents — and the **path an fd resolves to**
(the engine renders `fd=3</tmp/x>` like `strace -y`; the number is structure,
the target of the link is content). `tid` is optional and **v1 writers omit
it**: the engine tags a multi-threaded stream by prefixing `"[tid] "` to the
line itself, so the recording carries the fact where the engine actually
produces it rather than duplicating it into a field the writer would have to
re-derive.

### `stream` — one single-stepped instruction line

```json
{"k":"stream","text":"work+0x12 [victim]  add eax, esi"}
```

The `--stream` engine hands the front-end a formatted line only
([`asmspy_stream_sink`](../../../cli/libasmspy.h#L371)), so v1 records the text
faithfully rather than inventing fields it did not measure. Structuring the line
is engine work, out of scope here.

### `call` — one call-tree entry

```json
{"k":"call","tid":4242,"depth":2,"addr":4198710,"name":"helper","module":"spy_victim"}
```

Fields mirror [`asmspy_tree_call_t`](../../../cli/libasmspy.h#L448). `name`/`module`
are transient in the sink and MUST be copied by the writer.

### `graph` — whole-process call-graph snapshot

```json
{"k":"graph","nodes":[{"addr":4198710,"name":"work","module":"spy_victim","kind":"internal","invocations":3,"out_calls":6,"fanout":2}],"edges":[{"from":4198710,"to":4198800,"count":6}]}
```

Node fields mirror [`asmspy_gnode_t`](../../../cli/libasmspy.h#L400) plus the
machine-readable class token `kind` (`internal` \| `external` \| `jit` \|
`unknown`) the JSON exporter already computes; edge fields mirror
[`asmspy_gedge_t`](../../../cli/libasmspy.h#L409), keyed by entry **address** (not
node index) so a consumer may sort/filter nodes without invalidating edges. One
snapshot is written at detach.

### `topo` — process/thread topology snapshot

```json
{"k":"topo","mode":"syscalls","tasks":[{"tid":4242,"tgid":4242,"ppid":4200,"leader":true,"comm":"victim","exe":"spy_victim","inv":91}]}
```

Task fields mirror [`asmspy_task_t`](../../../cli/libasmspy.h#L505); `mode` is what
`inv` counts (`"syscalls"` \| `"calls"`).

### `survey` — statistical hot-edge histogram

```json
{"k":"survey","sampler":"ibs-op","edges":[{"from_addr":4198710,"to_addr":4198800,"from":"work+0x12 [victim]","to":"helper","count":812,"mispred":3,"is_return":0}],"samples":10442,"branch_samples":9001,"lost":0,"throttled":false}
```

Edge fields mirror [`asmspy_sample_edge_t`](../../../cli/libasmspy.h#L544); the
four trailing counters are the sink's fidelity channel. **Always `exact:false`.**
`lost`/`throttled` are the drop record: a survey that dropped samples says so.

### `watch` — one hardware data-watchpoint hit

```json
{"k":"watch","hit_no":1,"tid":4242,"pc":4198750,"addr":6295624,"is_write":1,"value_ok":true,"value_len":4,"value":42,"func":"work","module":"watch_victim","off":18}
```

Fields mirror [`asmspy_watch_hit_t`](../../../cli/libasmspy.h#L620). `is_write` is
`1` write / `0` read / `-1` undecodable — the third value is a real measurement
outcome and MUST NOT be collapsed into either other. `func`/`module` are omitted
when unresolved.

### `df_step` — one executed step's operand values (L0)

```json
{"k":"df_step","step":0,"off":0,"rbase":1048576,"disasm":"mov eax, edi","ops":[{"space":"reg","reg":35,"size":4,"write":false,"value_valid":true,"value":40}]}
```

Field order: `step`, `off`, `rbase?`, `disasm?`, `ops`.

Operand objects mirror [`at_val_rec_t`](../../../include/asmtest_valtrace.h#L61)
with the enum rendered as a token: `space` is `"reg"` (`AT_LOC_REG`) \| `"abs"`
(`AT_LOC_MEM_ABS`) \| `"off"` (`AT_LOC_MEM_OFF`). Operand field order:
`space`, `reg`, `base`, `index`, `scale`, `disp`, `addr`, `size`, `write`,
`value_valid`, `wide`, `bytes`, `value`. Memory addressing terms
(`base`/`index`/`scale`/`disp`/`addr`) are omitted for a register operand;
`value` is omitted when `value_valid` is false. A value wider than 8 bytes sets
`"wide":true` and omits `value`; its bytes ride in **`bytes`** (28 R1 T3) — a
lowercase-hex string of the operand's `size` bytes in memory order, present when
the producer serialized the [`wide`](../../../include/asmtest_valtrace.h#L133)
side buffer (bounded, ≤ 64 bytes). `bytes` is **omitted** when the producer had
no side buffer or the value was not captured, and a reader then degrades to a
`[wide]` placeholder — the bytes are never invented.

**`off` is region-relative, and carries no `basis`.** Unlike `trace`, a `df_step`
states no `basis` field: `off` is **always** an offset from the session's scoped
region base (`pc - base_ip`,
[dataflow_ptrace.c](../../../src/dataflow_ptrace.c)), by definition. An
absolute-offset producer is **out of this contract** and must not be wired to
`df_step`.

**`rbase` — the region base `off` is relative to** ([37](37-region-tag-on-df-step.md),
2026-07-29). An optional u64 giving the absolute base address of the code span
`off` is an offset within, so a reader resolves the step's PC as `rbase + off` and
its span as the `codeimage` whose `base == rbase` — no wire-order inference, no
single-span requirement. It is **omitted entirely — never `null`, never
0-as-unknown** — when the producer does not know a base (address 0 is never a
mapped code span, so `base == 0` means "not known"). Resolution rule (normative):
(1) **with `rbase`**, the PC is `rbase + off` and the span is the matching
`codeimage`; (2) **without `rbase`**, [36](36-anchor-the-3d-plane.md)'s single-span
anchor is the documented **permanent fallback** — exactly one `codeimage` code span
⇒ `base + off`, zero or ≥2 ⇒ refuse with the stated reason; (3) a reader **never**
guesses `rbase` from wire order or the nearest preceding `codeimage` (seq order is
"steps then image" — the refresh emits `codeimage` *after* its invocation); (4)
`rbase` present but matching no `codeimage` span ⇒ placement is sound (the producer
stated the base) but there are no bytes — place it, report "no code image for this
span"; (5) a per-event `rbase` always wins over the recording-wide anchor, and a
recording mixing tagged and untagged events resolves each by its own rule.
Additive optional field on a known kind ⇒ no envelope bump, no break; `rbase` is
`"region base"`, not the operand `base` register inside `ops[]`, so the two
substring-scanning readers (the conformance `field()` and `cli_smoke.sh`) do not
collide.

### `df_edge` — one last-writer def-use edge (L1)

```json
{"k":"df_edge","from":0,"to":2,"loc":{"space":"reg","reg":35,"size":4,"write":false,"value_valid":true,"value":40}}
```

Mirrors [`asmtest_defuse_edge_t`](../../../include/asmtest_valtrace.h#L178):
the value written at step `from` is read at step `to` through `loc` (the
consumer's read record, same operand shape as `df_step.ops`).

### `df_invocation` — one continuous-capture pass delimiter (35)

```json
{"k":"df_invocation","pass":0,"result":42,"steps":8,"truncated":false}
```

A live **continuous** `dataflow` / `auto` capture (asmspy `--dataflow
--continuous`, serve `continuous:true`) re-arms the *same* scoped region and
re-samples it until Stop, appending every pass into ONE growing recording. A
single `df_invocation` marker is emitted **before** each pass's `df_step` block,
so a reader segments the passes without guessing: every `df_step` / `regstate` /
`fpenv` / `mem` / `df_edge` event after a marker (up to the next marker or `end`)
belongs to that pass. Field order: `pass`, `result`, `steps`, `truncated`.

- `pass` is the 0-based invocation ordinal within this session.
- `result` is that invocation's routine return value (the same `result` the
  one-shot `--dataflow` JSON already reports).
- `steps` is the step count THIS pass saw (`vt->steps_total`), authoritative
  **per pass**: each pass gets a fresh value trace, so `df_step.step` restarts at
  0 every pass and two passes' step ranges would otherwise collide. The marker is
  the per-pass step-range discriminator the desktop keys on
  ([stepindex](../../../desktop/src/analysis/stepindex.cpp), which otherwise
  assumes one monotonic run). The footer's `steps_total` is overwritten per pass
  and so names only the LAST pass; the per-pass `steps` is the authority.
- `truncated` is this pass's own truncation (`vt->truncated`) — a pass whose
  operand buffers filled flips it, and a reader tears that pass's chrome without
  contaminating its neighbours.

Emitted **only** in continuous mode; a one-shot capture (the default) emits no
`df_invocation` and is byte-identical to before. On the **first** pass, reaching
the entry wait without the region running ends the session (like the region
engine's idle bailout) and emits no marker — a region never seen entering is a
genuine NEVER_RAN. But once a continuous session has produced at least one pass,
a later quiet window (the pinned region not entered for one entry wait) is
**armed-and-waiting, not a verdict** (39 T4): the session re-arms and keeps
capturing until Stop, and the quiet window is surfaced as a marker with
`steps:0` (an empty pass — `vt` genuinely holds nothing, so it is a marker, not
fabricated data) so a reader shows *"armed, region quiet"* rather than inferring
the lull from a gap. A hand-authored *skip* low-fidelity fixture may likewise carry
a `steps:0` marker to exercise the per-pass placard.

### `mem` — one memory access (address stream)

```json
{"k":"mem","step":2,"ea":2097144,"size":8,"rw":"w","space":"abs"}
```

One effective-address access per event, in step order — the per-access channel
the 3D rich rung ([10-spacetime-3d-overview.md](10-spacetime-3d-overview.md))
keys on. `step` is the executing step (index into the `trace` / `df_step`
stream); `ea` is the resolved effective address; `size` the access width in
bytes; `rw` is `"r"` (read) or `"w"` (write); `space` is `"abs"`
(`AT_LOC_MEM_ABS`) \| `"off"` (`AT_LOC_MEM_OFF`) — the same normalization a
`df_step` memory operand's `addr` carries, so a reader folds a `mem` EA to a
region offset the identical way. Field order: `step`, `ea`, `size`, `rw`,
`space`.

`mem` is a **projection of the operand records `df_step` already carries** (a
memory operand's resolved `addr` + `size` + `write`), lifted to a first-class
per-access stream so a consumer need not walk `df_step.ops` to reconstruct the
address trace. Both producers resolve the EA at run time already
([`src/dataflow_emu.c`](../../../src/dataflow_emu.c) `df_on_mem`, the live
[`src/dataflow_ptrace.c`](../../../src/dataflow_ptrace.c) `resolve_ea`); this
kind carries it. It is **opt-in** (the recorder's `--mem`, serve `mem:true`) and
absent by default, so a recording without it is normal — the rich rung stays
coarse and says so, never a silent zero. It carries the **address and width, not
the bytes** (that is `df_step`'s `wide`/`bytes` value channel) and no timestamps
(the ordering axis is the per-step ordinal).

### `regstate` — a register-file snapshot, by descriptor reference

```json
{"k":"regstate","desc":"emu_x86_regs_t@x86_64/sysv","values":{"rax":42,"rbx":0}}
```

`desc` is a **reference** to a state descriptor (see below), never an inline
register list — that is what lets one viewer render an x86-64, AArch64 or RISC-V
register deck without knowing any of them at compile time.

### `blame` — a value's backward attribution (def-use cone)

```json
{"k":"blame","step":4,"off":17,"loc":{"space":"reg","reg":35,"size":0,"write":true,"value_valid":true,"value":12},"cone":[{"step":0,"off":0,"kind":"insn"},{"step":4,"off":17,"kind":"insn"}],"born_untraced":false}
```

Attributes the value produced at `step` (instruction offset `off`, identified by
the optional operand `loc`) to the instruction(s) that produced it: `cone` is the
**ascending backward def-use slice** — the least set of producing steps reachable
from the sink along last-writer edges — **including the sink step itself**. Each
cone entry is a `{step, off, kind}` triple mirroring the `srcmap` row shape;
`kind` is `"insn"` for a traced producer. `loc` reuses the `df_step`/`df_edge`
operand shape (omitted when the sink writes no register). Field order: `step`,
`off`, `loc`, `cone`, `born_untraced`.

`born_untraced` is the fidelity verdict (33 R6 T1): `true` when the value has **no
traced producer** inside the window — the backward slice reached only the sink,
because it was read from an argument, a constant, or pre-existing state. The cone
is then the **sink alone** — never empty. This is *provenance starts at
instrumentation*, the same worldline-fidelity the Loom's lineage carries
([05-loom-day-one.md](05-loom-day-one.md)); a reader distinguishes it from
"nothing happened" and never invents ancestry. `blame` is a **pure derived pass**
over the `df_edge` graph a recording already carries (`asmtest_slice_backward` —
the same slicer the TUI and desktop cones use), **opt-in** (`--blame`), absent by
default. It is **exact-only** and never crosses threads (hard refusals). A cone
over heap memory is sound only after GC-canonicalization; the golden corpus is
register/stack-arg only.

### `statediff` — the step-to-step architectural-state delta

```json
{"k":"statediff","step":2,"changed":{"rax":12,"rip":4099},"computed":true}
```

The register-file delta between step `step` and the previous held step: `changed`
carries only the registers whose value **differs** from the predecessor, each with
its NEW value, in descriptor order. It is the wire form of the Scrubber's
per-field change-highlight (the pure `pf->value != f.value` logic,
[stepindex.cpp](../../../desktop/src/analysis/stepindex.cpp)), lifted into the
producer so a **two-recording merged view** has an exact per-step basis. `step` is
absolute (past any evicted prefix) so two recordings align truncation-robustly.
Field order: `step`, `changed`, `computed`.

`computed` is the fidelity flag (33 R6 T2): `false` when there is **no known
predecessor** — the first held step (whose true predecessor is either genuine step
0 or an evicted step the ring cannot distinguish) — with an empty `changed`. A
full delta there would be a D7 lie ("everything changed"); `computed:false` says
the delta was not derivable. One `statediff` pairs **1:1 with its `regstate`** (it
rides the same per-step ring), **opt-in** (`--statediff`, alongside `--steps`),
absent by default. It is architectural (the register file), not memory-content
diff (that rides `mem` / `wide`), and is **derived** from an existing recording,
not a new execution.

### `result` — a test / bench / features row

```json
{"k":"result","tier":"emu","backend":"unicorn","arch":"x86_64","scope":"routine","available":true,"skip_reason":"","fidelity":"exact","complete":true,"trace_insns":37,"insns_truth":37}
```

Exactly the row shape [`tools/asmfeatures.c`](../../../tools/asmfeatures.c#L368)
already emits, so a features report *is* a recording. `complete`,
`trace_insns`, `insns_truth` are `null` when not measured — the one place a
`null` is normative, because "not measured" is distinct from "measured zero".

### `note` — a human annotation

```json
{"k":"note","text":"the carry flag is set here","off":18,"step":4,"stop":true}
```

`off`/`step`/`stop` are optional. Ordered `stop:true` notes are what turn a
recording into a **walkthrough**: a reader plays to each stop in file order.

A stop may additionally carry `title`, `expected` and `got` — all optional,
omitted when absent, and meaningful only alongside `stop:true`
([06-doors-and-learning.md](06-doors-and-learning.md), which owns them). `text`
stays the body. The expected/got pair is what lets the Learn door frame a
failure as *this is what should have happened, and this is what did* without a
player inventing the comparison; a reader that does not know the fields ignores
them like any other unknown key. Field order for a stop:
`text`, `off`, `step`, `stop`, `title`, `expected`, `got`.

### `stitch` — a per-tid PT slice set

```json
{"k":"stitch","tid":4242,"slices":[{"version":1,"offs":[0,12,40]}]}
```

Mirrors [`asmtest_hwtrace_stitch_handles`](../../../include/asmtest_hwtrace.h#L641).
**Defined in v1 with no v1 writer** — the producer arrives with
[07-serve-live-host.md](07-serve-live-host.md). Defining it now keeps the kind
id and field names stable for readers written before the producer exists.

### `end` — the footer

```json
{"k":"end","events":37,"truncated":false,"drops":{"lost":0,"throttled":false},"steps_total":37,"skip":{"code":2,"reason":"IBS-Op is an AMD feature; this host is GenuineIntel"}}
```

`events` counts the event lines **before** this one (the header is not an
event, and `end` does not count itself). `skip` is present only when the run
skipped. `steps_total` (optional, 28 R1 T2) is the total step count the producer
**saw**, counting past any ring cap — the *M* a truncation banner reads as
"N of M". It is the only source of *M* for a truncate-when-full (tail-drop) ring,
which passes `drops.lost = 0`; a reader that predates the field ignores it and
keeps its older wording. Field order: `events`, `truncated`, `drops`,
`steps_total`, `skip`.

**A file without an `end` event is a TORN recording**, and a reader MUST say so
rather than presenting a partial recording as complete. This is deliberate and
has no `atexit` rescue: a producer killed mid-record leaves a file that *looks*
torn because it *is* torn.

## Reserved kinds

Registry rows only — the id is claimed, the fields are **not** frozen, and there
is no v1 producer. A reader ignores them like any unknown kind (see
*Compatibility rules*); a future doc lands one by defining its fields here.

| Kind | Intended payload | Claimed by |
|---|---|---|
| `fpenv` | FP/SIMD environment + wide register state | expansion wave |
| `fuzzstats` | corpus/coverage counters from a fuzz run | expansion wave |
| `taint` | taint labels propagated through a step | expansion wave |
| `srcmap` | one source line-map row `{off,value,kind,file,col}`, mirroring [`asmtest_srcmap_entry_t`](../../../include/asmtest_trace.h#L177) | [05-loom-day-one.md](05-loom-day-one.md) |
| `take` | take/edit provenance (the Loom fork mechanic) | [05-loom-day-one.md](05-loom-day-one.md) |
| `codeimage` | captured code bytes at a version | [08-observer-views.md](08-observer-views.md) |

`session` / `cmd` / `err` were reserved here for 07 and are now **defined** — see
*Serve protocol* below. They are **serve-only**: they appear on a live control
stream, never inside a `.asmtrace` file written by `--record`.

`codeimage` was reserved here for 08 and is now **defined** — see *`codeimage` —
captured code bytes at a version* at the end of this file. Unlike the three
above it is an ordinary **recording** event: it rides inside the `[header … end]`
range and the footer counts it.

`mem` was reserved here for 10 and is now **defined** — see *`mem` — one memory
access (address stream)* above (29 R2). An ordinary opt-in recording event.

`df_invocation` (35) is **defined** — see *`df_invocation` — one continuous-capture
pass delimiter* above. An ordinary recording event emitted only by a **continuous**
live capture, delimiting its re-armed passes; absent in a one-shot recording.

`blame` (reserved for 09) and `statediff` (expansion wave) are now **defined** —
see *`blame` — a value's backward attribution* and *`statediff` — the step-to-step
architectural-state delta* above (33 R6). Both are ordinary opt-in recording
events, pure derived passes over a recording's own `df_edge` / `regstate` streams.

`fpenv` (expansion wave) is now **defined** — see *`fpenv` — the FP/SIMD
environment* below (31 R4 T2). An ordinary opt-in recording event, a pure derived
decode of the MXCSR the wide `regstate` deck captures under `--fpregs`.

Adding a kind is a **new registry row under the ignore-unknown-kinds rule** —
never a new envelope major.

## State descriptors

A descriptor names a struct row of the generated ABI manifest
[`asmtest_abi.json`](../../../scripts/gen-manifest.c) (built by `make manifest`;
the file itself is generated, not committed), in the form
`"<struct>@<arch>/<abi>"` — e.g. `emu_x86_regs_t@x86_64/sysv`, taking `arch` and
`abi` from the manifest's own `host_arch` / `abi` fields.

A recording MAY embed the descriptors it references in its header so it is
**self-contained** — a viewer with no manifest on disk still renders the deck:

```json
"descriptors":[{"id":"vec512_t@x86_64/sysv","size":64,"align":8,"fields":[{"name":"u8","offset":0,"size":64}]}]
```

Field objects are copied verbatim from the manifest row. `vec512_t` is in the
manifest as of this doc's task set (it was missing; AVX-512 capture had no
describable row), which closes the precondition for a 512-bit register deck.

## Determinism rules

The golden corpus (D6) is byte-compared, so byte stability is a contract, not a
nicety:

1. **Field order is fixed** by this document, per kind, and the writer emits it —
   there is exactly one writer TU ([`cli/asmtrace_ndjson.c`](../../../cli/asmtrace_ndjson.c))
   so field order has exactly one owner.
2. **Deterministic mode** (`asmtrace_open(..., deterministic=1)`) omits `ts`,
   `pid` and `cmd` from the header. Everything else is already a function of the
   input.
3. **Numbers are integers**, printed in decimal, no exponent, no `-0`. Booleans
   are `true`/`false`, never `1`/`0` (the numeric `is_write` tri-state is an
   `int` *by design* and is documented as such).
4. **Strings are escaped minimally and identically**: `"` and `\` as `\"` `\\`,
   bytes `< 0x20` as `\u00XX` (lowercase hex), everything else verbatim. The
   escaper is shared with the writer, so two writers cannot disagree.
5. **Regenerating a golden file must be byte-identical.** The authoritative lane
   is the `docker-cli` image: golden bytes include Capstone disassembly text and
   the pinned Capstone 5.0.1 renders differently from a host's apt 4.x.

## Compatibility rules

- **Ignore unknown kinds.** A reader skips a line whose `"k"` it does not know,
  counts it, and reports the count — never a hard error.
- **Ignore unknown fields.** Additive fields on a known kind are not a break.
- **Reject a newer major.** `asmtrace > 1` is refused by name, not
  best-efforted.
- **Refuse an unknown container by name.** Notably `zstd-frames` (reserved).
- **Never default `basis` or `exact`.** Both encode a fact that cannot be
  inferred from the data; a reader missing either MUST refuse the event rather
  than guess.
- **A missing `end` is torn**, and is reported as such.

## Known v1 gaps — the freeze checklist

Appended by consumers as they hit them; each is a decision the Phase-3 freeze
has to make explicitly rather than inherit.

- ~~**No routine identity.**~~ **CLOSED 2026-07-28 (28 R1 T1).** The `code`
  header object (`{"name":str,"sha256":str,"len":int}`, see *Routine identity*
  above) now carries the SHA-256 of the recorded bytes: the corpus recorder
  hashes its fixed 64-byte window and the live `--dataflow` producer hashes the
  region it read, so `dt_diff_build` refuses a pair whose hashes differ and keeps
  the accurate caveat only when a side omits `code`. Raised 2026-07-24 by 04.
- **No block starts from the L0 producer.** `coverage` is defined and the
  region tiers write it, but the emulator L0 value producer measures executed
  *steps* and has no block information, so the generated corpus carries `trace`
  and `df_step` events and no `coverage`. Block starts cannot be recovered from
  an offset stream without instruction lengths, so the recorder emits none
  rather than guessing. Raised 2026-07-24 by 04.
- ~~**The wide side buffer is not serialised.**~~ **CLOSED 2026-07-28 (28 R1
  T3).** A >8-byte operand now emits its bytes in the `bytes` hex field (see
  `df_step` above) when the producer carries the `wide` side buffer; a reader
  renders the bytes and degrades to `[wide]` only when they are genuinely absent.
- ~~**`df_step` states no region.**~~ **CLOSED 2026-07-29 (37 T1).** `df_step`
  now carries an optional `rbase` (see *`df_step`* above): the producer knows the
  region base as it writes the offset, so it states it, and a reader resolves the
  span as `rbase + off` instead of deriving it from a single `codeimage` (which a
  live `auto` candidate walk — several spans — made unrecoverable). 36's
  single-span anchor remains the permanent fallback for pre-37 recordings and for
  rel `trace`, which 37 deliberately does not tag. Raised 2026-07-29 by 36.

## Example

A complete, minimal recording — the reference a reader is tested against
(`cli/test_asmtrace.c` extracts exactly this block from this file and parses it):

```json
{"asmtrace":1,"container":"ndjson","producer":{"name":"asmtrace_record","version":"1.1.0"},"provenance":{"backend":"emu-l0","exact":true,"trust":"exact"},"arch":"x86_64"}
{"k":"note","text":"add_signed(40,2) under the deterministic emulator"}
{"k":"df_step","step":0,"off":0,"rbase":1048576,"disasm":"mov eax, edi","ops":[{"space":"reg","reg":19,"size":4,"write":false,"value_valid":true,"value":40},{"space":"reg","reg":35,"size":4,"write":true,"value_valid":true,"value":40}]}
{"k":"df_step","step":1,"off":2,"rbase":1048576,"disasm":"add eax, esi","ops":[{"space":"reg","reg":35,"size":4,"write":false,"value_valid":true,"value":40},{"space":"reg","reg":43,"size":4,"write":false,"value_valid":true,"value":2},{"space":"reg","reg":35,"size":4,"write":true,"value_valid":true,"value":42}]}
{"k":"df_edge","from":0,"to":1,"loc":{"space":"reg","reg":35,"size":4,"write":false,"value_valid":true,"value":40}}
{"k":"trace","basis":"rel","kind":"insn","off":0,"disasm":"mov eax, edi"}
{"k":"trace","basis":"rel","kind":"insn","off":2,"disasm":"add eax, esi"}
{"k":"coverage","basis":"rel","blocks":[0],"blocks_total":1,"insns_total":3,"truncated":false}
{"k":"end","events":7,"truncated":false,"drops":{"lost":0,"throttled":false}}
```

## Serve protocol

> **Owned by [07-serve-live-host.md](07-serve-live-host.md)** (T1), appended
> under this file's D5 append-only rule; the envelope, provenance and event
> kinds above are unchanged and remain 01's. **01 owner sign-off: the serve
> protocol adds no field to any existing kind and no new envelope major — it
> defines the three kinds already reserved for 07 in the registry above and
> otherwise only *carries* v1 events. Recorded 2026-07-24.**

`asmspy --serve` turns the recording format into a **live session**: instead of
writing one mode's events to a file, it reads commands and streams the events of
whichever mode is running. There is no second wire format — a serve stream is the
same NDJSON, and slicing one session out of it yields a valid `.asmtrace` file.

### Transport

Commands and events are both NDJSON, one object per line.

| Form | Commands in | Events out |
|---|---|---|
| `asmspy --serve` | stdin | stdout |
| `asmspy --serve=<path>` | the `unix(7)` `SOCK_STREAM` connection | the same connection |

The socket is **filesystem-permissioned and unauthenticated by design** — that
is the whole security model, plus `ssh <host> asmspy --serve` as the remote
transport. **One client at a time.** EOF on the command channel means `quit`.

### The control channel

```json
{"cmd":"start","mode":"log","pid":4242,"follow":false,"max":200}
{"cmd":"pause","on":true}
{"cmd":"stop"}
{"cmd":"quit"}
```

| Command | Meaning |
|---|---|
| `start` | Begin ONE engine session (see the mode table). Refused with `err` while another session is running. |
| `pause` | `{"on":true}` suspends **emission**, not tracing; `{"on":false}` resumes. Events produced while paused are **counted and reported**, never silently dropped (see below). |
| `stop` | End the running session. This is a **full detach** — one engine run per `start`, no engine reuse and no attached-idle state. |
| `quit` | `stop` if running, then exit. |

Modes and their parameters — each row is one `libasmspy` engine, and the
parameter list is exactly that engine's signature
([`cli/libasmspy.h`](../../../cli/libasmspy.h)):

| `mode` | Engine | Parameters | Emits |
|---|---|---|---|
| `log` | `asmspy_engine_syscalls` | `follow`, `max` | `syscall` |
| `stream` | `asmspy_engine_stream` | `tid`, `follow`, `max` | `stream` |
| `trace` | `asmspy_engine_region` | `tid`, `base`, `len`, `max` | `trace`, `coverage` |
| `dataflow` | `asmspy_engine_dataflow` | `tid`, `base`, `len`, `max` | `df_step`, `df_edge` |
| `tree` | `asmspy_engine_tree` | `tid`, `follow`, `max`, `depth`, `focus`, `module` | `call` |
| `graph` | `asmspy_engine_graph` | `tid`, `follow`, `max` | `graph` |
| `procs` | `asmspy_engine_procs` | `max`, `count` (`"syscalls"`\|`"calls"`) | `topo` |
| `sample` | `asmspy_engine_sample` | `ms` | `survey` |
| `watch` | `asmspy_engine_watch` | `addr`, `len`, `rw`, `max` | `watch` |
| `auto` | the `--auto` selection, then `asmspy_engine_dataflow` | `module`, `sampler` (`"ibs"`\|`"sw"`\|`"auto"`), `max` | `df_step`, `df_edge` |

Omitted parameters take the subcommand default. **Unknown modes, unknown
parameters and out-of-range values are refused with `err`, never coerced** —
`atoi("nginx")` is `0`, and a `pid` of 0 is not a diagnosis.

### Refusals — the flag matrix, verbatim from the CLI

The serve loop makes the *same* refusals the argument parser makes, for the same
reasons, so the two front ends cannot disagree about what is legal:

- **`tid` with `follow`** — `tid` pins ONE task, `follow` adds child processes.
- **`tid` on `mode:"auto"`** — the IBS sampler carries no tid, so it physically
  cannot attribute an entry to a thread; pinning to a thread that may never
  enter the picked region is a hang generator, not a preference.
- **`tid` on `mode:"log"`, `"procs"`, `"sample"` or `"watch"`** — those engines
  take no `only_tid` parameter. (The syscalls engine gaining one is a separate
  plan work-item, not this protocol's to anticipate.)
- **`module` or `sampler` without `mode:"auto"`** — both scope the *automatic*
  pick; on a named region they would be no-ops that read like filters.
- **`depth` < 1** — `depth:0` asks for a tree with no levels. Omit the parameter
  for unlimited.
- **A second `start` while a session runs** — the budget rule (D6): one ptrace
  jack per target tree.

### Lifecycle events

Three serve-only kinds, claimed in *Reserved kinds* above and defined here.

#### `session` — a state transition

```json
{"k":"session","state":"started","mode":"log","pid":4242,"params":{"follow":false,"max":200}}
{"k":"session","state":"stopped","mode":"log","events":200,"reason":"max"}
{"k":"session","state":"skip","mode":"sample","skip":{"code":2,"reason":"IBS-Op is an AMD feature; this host is GenuineIntel"}}
{"k":"session","state":"pick","mode":"auto","pick":{"sampler":"sw-clock","evidence":"residency","func":"event_loop","base":94207306414080,"len":320,"weight":41,"attempt":1,"of":3}}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `state` | str | yes | `"started"` \| `"pick"` \| `"stopped"` \| `"skip"`. |
| `mode` | str | yes | The mode this transition concerns. |
| `pid` | int | on `started` | The attached pid. |
| `params` | obj | on `started` | The **effective** parameters, echoed after defaulting — so a client never has to guess what its omissions became. |
| `events` | int | on `stopped` | Event lines this session emitted. |
| `reason` | str | on `stopped` | `"max"` \| `"stop"` \| `"exit"` \| `"quit"` — why the engine returned. |
| `paused_dropped` | int | no | Events produced while paused and therefore **not emitted**. Present whenever non-zero. |
| `skip` | obj | on `skip` | `{"code":int,"reason":str}` — the **positive** engine skip code and `asmspy_strerror`'s (or the measuring source's) text, verbatim. |

`state:"skip"` is a *successful* session that had nothing to report — the same
distinction the `end` footer's `skip` makes. It is never an error, and a client
that renders it as one is wrong.

#### `session` `state:"pick"` — what `mode:"auto"` chose, and on what evidence

`mode:"auto"` picks a region to capture instead of being told one, so it must
say **what it picked and how good the evidence was**. One `pick` event is
emitted per candidate attempted, between `started` and the terminal event.

| `pick` field | Type | Meaning |
|---|---|---|
| `sampler` | str | `"ibs-op"` or `"sw-clock"` — which sampler actually ran, after `"auto"` resolved. |
| `evidence` | str | `"entry"`, `"residency"`, or `"idle"` — **the load-bearing field**, see below. |
| `func` | str | The chosen function's name (or `"0x…"`). |
| `base` / `len` | u64 | The region handed to the capture engine. |
| `weight` | u64 | Entry samples (`entry`) or residency samples (`residency`). |
| `sites` | u32 | Distinct call sites observed arriving (`entry` only). |
| `attempt` / `of` | int | 1-based candidate index and how many are ranked. |

**`evidence` is not a synonym for `sampler`, and a client must not treat it as
decoration.** The capture engine arms a breakpoint at the region's *entry* and
waits for a thread to arrive, so the only evidence of the right *type* is a
direct observation of that same event — an IBS-Op branch whose target is a
symbol's start. That is `"entry"`.

`"residency"` is the portable fallback: a software-clock PC histogram says a
function was *executing*, which is a different claim. A function entered once
and never re-entered is the top residency winner and an entry breakpoint there
can never fire again — the rule's known failure shape. So a client showing a
`residency` pick **must label it as weaker evidence**, and successive `pick`
events with rising `attempt` are the server walking the ranked candidates after
a `REGION_NEVER_RAN`, which is a genuine refusal about *that candidate* and not
a fact about the target.

`"idle"` (39 T3) is the third value, and it is **not a pick** — it is an empty
sample window reported on this same channel: the sampler ran and *nothing*
qualified as a region this window, so the pick will re-sample. Its `func` is the
sentinel `"(idle window)"`, its `weight`/`sites` are 0, and its `attempt`/`of`
are the **window** retry (not a candidate ordinal). A client must render it as
*"idle window N of M — re-sampling"*, never as an entry/residency observation:
`evidence:"idle"` exists precisely so the wire does not claim an observation that
did not happen. An idle window is a retry, not a verdict; only after the bounded
window budget is exhausted does the session end `REGION_NEVER_RAN`.

#### `err` — a refused command

```json
{"k":"err","reason":"--tid pins ONE task; --follow adds child processes — drop one","cmd":"start"}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `reason` | str | yes | Human text naming the rule that refused it. |
| `cmd` | str | no | The `cmd` that was refused, when it parsed far enough to know. |

An `err` **never** ends a running session and never exits the loop: the client
is expected to correct and retry.

#### `cmd` — the accepted-command echo

```json
{"k":"cmd","cmd":"start","mode":"log"}
```

Emitted for each command the server **accepted**, so a captured serve stream
states what was asked for and not merely what came back. A refused command
produces `err` instead, never both.

### The two protocol laws

1. **A session's events are exactly `--record`'s events.** Between a
   `state:"started"` event and the terminal `session` event, the server emits
   the mode's own **provenance header line**, then that mode's record-mode
   events through the *same* serializers
   ([`cli/asmtrace_ndjson.c`](../../../cli/asmtrace_ndjson.c)), then the `end`
   footer. There is no serve-specific event body anywhere. A client that
   extracts `[header … end]` **and drops the three serve-only kinds** therefore
   holds a **valid `.asmtrace` recording**, and the golden-corpus readers parse
   it with no serve awareness at all.

   Two consequences worth stating rather than discovering:

   - A serve stream is **not itself** a `.asmtrace` file. It is a sequence of
     them, bracketed by lifecycle events. Header lines carry `asmtrace`; every
     other line carries `k`.
   - **The slice must be filtered.** `session` brackets a session from
     *outside* the `[header … end]` range, but `cmd` and `err` are emitted the
     moment they happen — which can be *during* a session, so they land inside
     that range. They are not recording events and the `end` footer's `events`
     count does **not** include them. A client that slices without filtering
     will therefore see more lines than the footer declares and must not read
     that as a corrupt recording: it is a control stream, correctly reporting
     both things. Drop `session`/`cmd`/`err`, and `end.events` matches exactly.

2. **Sorting and filtering are client-side.** The server streams; the client
   ranks, sorts, redacts and hides. The one exception is the **tree filter**
   (`depth`/`focus`/`module`), which is engine-side by design — it bounds what
   the engine *emits* while still tracking every call and return, so the surviving
   lines' depths stay true. Nothing else may migrate server-side: a server that
   sorted would be deciding what the operator is allowed to see.

### Fidelity rules specific to serve

- **A pause is a recorded gap.** Events produced while `pause` is on are not
  emitted, so the session's `end` is marked `truncated` and the terminal
  `session` event carries `paused_dropped`. A gap the client asked for is still
  a gap, and the recording says so.
- **A torn session is a torn recording.** If the server dies or the pipe breaks
  mid-session, the last session has no `end` — which the reader already reports
  as torn (*Compatibility rules*). No `atexit` rescue, for the same reason 01
  has none.
- **Payloads are separated, not withheld.** Serve emits the same
  payload-separated `syscall` events record mode does, with
  `"redacted":false` stated faithfully in the header. Redaction is a **renderer**
  duty ([08-observer-views.md](08-observer-views.md)); the wire never pretends
  content was withheld when it was not.

## `codeimage` — captured code bytes at a version

> **Owned by [08-observer-views.md](08-observer-views.md)** (T7), appended under
> this file's D5 append-only rule. It **defines the kind already reserved for 08
> in *Reserved kinds* above** and adds no field to any existing kind and no new
> envelope major. **01 owner sign-off: recorded 2026-07-24.**

```json
{"k":"codeimage","base":4198400,"len":16,"version":1,"when":3,"bytes":"f30f1efa554889e5"}
```

A tracked code region's bytes **as they were at a moment**, so a trace of
self-modifying or JIT-compiled code can be disassembled against the bytes that
were live when it ran — not against whatever is at that address by the time
someone looks.

| Field | Type | Required | Meaning |
|---|---|---|---|
| `base` | u64 | yes | Start address of the snapshotted span (absolute, in the target). |
| `len` | u64 | yes | Its length in bytes. `bytes` is exactly `2 * len` hex characters. |
| `version` | u64 | yes | 0-based version index for that span: 0 is the `track()` snapshot, each later one a `refresh()` that found the pages changed. |
| `when` | u64 | yes | The **logical timestamp** the version was recorded at — [`asmtest_codeimage_now`](../../../include/asmtest_codeimage.h#L102)'s monotonic capture sequence, the same clock a trace position is stamped against. |
| `bytes` | str | yes | Lowercase hex, two characters per byte, no separators. |

Mirrors [`asmtest_codeimage_bytes_at`](../../../include/asmtest_codeimage.h#L110)
on the wire, and the **resolution rule is the same one**, stated normatively
because a reader that gets it wrong is silently wrong:

- To disassemble address `a` at trace time `t`, take the version whose span
  contains `a` with the **greatest `when` ≤ `t`** — never the newest version,
  and never the first.
- If no version satisfies that, the bytes at `a` are **unknown**. Not zero, not
  the nearest later version: a JIT address is reused, and showing the *next*
  method's bytes for the *previous* method's trace is precisely the failure this
  kind exists to prevent.
- A reader **never re-reads live memory** to fill the gap. For a replayed
  recording the process is long gone; for a live one the bytes at that address
  have already been established as the wrong ones.

**Producer.** `asmspy --serve` emits `codeimage` events for a region-scoped
session (`mode:"trace"` / `"dataflow"` / `"auto"`) when
[`asmtest_codeimage_available()`](../../../include/asmtest_codeimage.h#L72) is
true: version 0 before the engine attaches, then one per refresh as invocations
are captured. When the recorder is unavailable the session emits a `note`
carrying the **measured** skip reason
([`asmtest_codeimage_skip_reason`](../../../include/asmtest_codeimage.h#L76) —
the soft-dirty / `PAGEMAP_SCAN` gate, Linux ≥ 6.7) and captures without it. A
recording with no `codeimage` events is therefore normal, and means the viewer
falls back to the recorded `disasm` strings (D10) or to bare offsets.

## `regstate` descriptor — `emu_x86_regs_t@x86_64/sysv`

> **Owned by [09-teaching-producers.md](09-teaching-producers.md)** (T2),
> appended under this file's D5 append-only rule. It gives the `regstate` kind
> (defined above under *Event kinds*) its first concrete state descriptor — the
> field list a viewer renders the deck from — and **adds no field to any existing
> kind and no new envelope major.** The kind, the `{"desc","values"}` shape and
> the descriptor-reference rule are unchanged and remain 01's.

The per-step register ring
([include/asmtest_emu.h](../../../include/asmtest_emu.h#L601) — `emu_step_capture`)
is the first `regstate` producer: `asmtrace_record --steps=<cap>`
([tools/asmtrace_record.c](../../../tools/asmtrace_record.c)) arms it, and after
the run emits one `regstate` event per held pre-state, referencing this
descriptor.

**Descriptor id.** `emu_x86_regs_t@x86_64/sysv` — the `emu_x86_regs_t` struct row
of [`asmtest_abi.json`](../../../scripts/gen-manifest.c#L120) (arch `x86_64`, abi
`sysv`), the full x86-64 emulator register file
([include/asmtest_emu.h:62](../../../include/asmtest_emu.h#L62)).

**`values` fields.** The 16 general-purpose registers plus `rip` and `rflags`,
in `emu_x86_regs_t` **declaration order**, each a decimal `u64`:

```
rax rbx rcx rdx rsi rdi rbp rsp r8 r9 r10 r11 r12 r13 r14 r15 rip rflags
```

```json
{"k":"regstate","desc":"emu_x86_regs_t@x86_64/sysv","values":{"rax":42,"rbx":0,"rcx":0,"rdx":0,"rsi":2,"rdi":40,"rbp":0,"rsp":2162680,"r8":0,"r9":0,"r10":0,"r11":0,"r12":0,"r13":0,"r14":0,"r15":0,"rip":1048582,"rflags":2}}
```

- The 128-bit **XMM file is a documented v1 omission** — a wide value is not a
  bare JSON integer (exactly the `df_step` `wide` limit above), and no MXCSR is
  captured; the descriptor mechanism absorbs an FP/vector deck later. The struct
  row in the manifest still names `xmm`, so a future wide-register producer can
  extend `values` without a new descriptor id.
- A producer **MAY emit a subset** of these fields — a reader renders the fields
  present in `values` by name and shows the rest as unrecorded, per the
  ignore-unknown / render-what-is-present rules. The `desktop/test`
  walkthrough fixtures ([tests/golden-asmtrace/walkthroughs/](../../../tests/golden-asmtrace/walkthroughs/))
  carry a single end-state `regstate` with only `rax rbx rcx rdx rsi rdi`; the
  ring producer carries the full file above, per step.

**Order, dropping and truncation (D7).** The ring producer emits held pre-states
**oldest first**. The full register file is snapshotted BEFORE each instruction;
when more steps run than the ring holds, the **earliest** entries are evicted, so
the held events are steps `[dropped, dropped + count)`. The `end` footer then
carries the truncation faithfully: `"truncated":true` and the evicted count in
`drops.lost`, so a reader offsets the first held step by `drops.lost` and renders
the missing prefix as a torn edge — never as step 0. A recording with no
`regstate` events simply had the ring disarmed (`--steps` defaults to 0), the
normal case.

## `regstate` descriptor — `user_regs@x86_64/sysv` (live ptrace producer)

> **Owned by [26-live-regstate-producer.md](26-live-regstate-producer.md)**,
> appended under this file's D5 append-only rule. It gives the `regstate` kind its
> **second** concrete producer — the LIVE, single-stepped `asmspy --dataflow`
> engine — under a distinct descriptor id, and **adds no field to any existing
> kind and no new envelope major.** The kind, the `{"desc","values"}` shape and the
> descriptor-reference rule are unchanged and remain 01's.

The scoped ptrace value engine
([src/dataflow_ptrace.c](../../../src/dataflow_ptrace.c)) already reads the full
integer register file (`PTRACE_GETREGS`) as each in-region instruction's PRE-STATE.
With the boolean `--steps` opt-in
([cli/asmspy.c](../../../cli/asmspy.c) `cmd_dataflow`; serve `steps:true`) it
carries that pre-state to the sink, which emits one `regstate` event per `df_step`,
so the desktop **Scrubber** time-travels a LIVE capture — and, because serve and
`asmspy --dataflow --record` share the sink, a saved `--dataflow --steps` file
gains the ring too.

**Descriptor id.** `user_regs@x86_64/sysv` — named for the ptrace source (the
kernel's `struct user_regs_struct`), so the id is truthful that these are the **real
architectural** registers captured perturbingly (single-step), not emulated. The
`values` **field names are identical** to `emu_x86_regs_t@x86_64/sysv`'s (the 16
GPRs + `rip` + `rflags`, `struct user_regs_struct.eflags` folded to `rflags`), so
one Scrubber deck renders both producers unchanged — the consumer keys on the field
**names**, not the id
([desktop/src/analysis/stepindex.cpp](../../../desktop/src/analysis/stepindex.cpp)).

```json
{"k":"regstate","desc":"user_regs@x86_64/sysv","values":{"rax":42,"rbx":0,"rcx":0,"rdx":0,"rsi":2,"rdi":40,"rbp":0,"rsp":140737488347000,"r8":0,"r9":0,"r10":0,"r11":0,"r12":0,"r13":0,"r14":0,"r15":0,"rip":94476548243590,"rflags":514}}
```

- **Real ASLR'd addresses.** Unlike the emulator's fixed `0x200000` stack base,
  the live `rsp`/`rbp`/`rip` are the target's actual ASLR'd values. A cross-producer
  parity check therefore compares per-step register **changes** (which register each
  step wrote, to what operand-visible value), not absolute values (26 T5.2).
- **XMM/YMM/FP omitted**, exactly as the emulator descriptor — a wide value is not a
  bare JSON integer; the descriptor mechanism absorbs a vector deck later, for both
  producers at once.

**Order, dropping and truncation (D7) — differs from the emulator ring.** The live
producer does **not** drop-oldest. Its bound is the value trace's `steps_cap`
(truncate-when-**full**: the earliest `steps_cap` steps are kept, later ones
dropped), so the held `regstate` events are steps `[0, count)` — the **same** steps
as the held `df_step` events, emitted **interleaved and in step order**, one
`regstate` right after its `df_step`. The `end` footer carries `"truncated":true`
when the tail was dropped, but `drops.lost` stays **0** (no prefix was evicted), so
a reader offsets the first held step by `drops.lost == 0` and pairs `regstate[i]`
with `df_step[i]` at step `i`. The tear here is the **missing tail** (a still-growing
or over-cap capture), not a missing prefix. A live `log`/`trace`/`watch`/`sample`
session never single-steps, so it carries no `regstate` at all — the Scrubber says
so, distinctly from a `--steps`-less exact capture.

## `regstate` wide deck — the XMM/MXCSR extension (R4)

> **Owned by [31-wide-register-deck.md](31-wide-register-deck.md)** (T1), appended
> under this file's D5 append-only rule. It **extends the `values` object** of BOTH
> existing `regstate` descriptors (`emu_x86_regs_t@x86_64/sysv` and
> `user_regs@x86_64/sysv`) with the wide FP/vector deck — it **adds no new kind, no
> new descriptor id, and no new envelope major**, exactly as those descriptors
> foretold ("the struct row still names `xmm`, so a future wide-register producer
> can extend `values` without a new descriptor id"). The `{"desc","values"}` shape,
> the descriptor-reference rule, and the render-what-is-present rule are unchanged.

The XMM/MXCSR omission recorded in both descriptors above **closes here, as an
opt-in**: `asmtrace_record --fpregs` (the emulator ring) and `asmspy --dataflow
--fpregs` / serve `fpregs:true` (the live ring) add, to each `regstate` event's
`values` object, the 16 XMM registers and MXCSR — appended after `rflags`:

```json
{"k":"regstate","desc":"user_regs@x86_64/sysv","values":{"rax":42,"...":0,"rip":1048582,"rflags":2,"xmm0":"0000000000000000000000000000c05f","xmm1":"00000000000000000000000000000000","...":"...","xmm15":"00000000000000000000000000000000","mxcsr":8064}}
```

- **`xmm0`..`xmm15`** — each a **lowercase-hex string of the register's 16 bytes**
  (little-endian in memory order, the same wide-value convention `df_step`'s
  `bytes` uses, since a 128-bit value is not a bare JSON integer). A future YMM/AVX
  extension lengthens the string; a reader keys on the field name, not the width.
- **`mxcsr`** — the 32-bit SSE control/status word, a **plain decimal integer**
  (it is 32-bit, so it needs no hex string). It is the source of the `fpenv` event
  (see below).
- **Off by default (D6).** A recording written without the opt-in carries **none**
  of these fields and is byte-identical to a pre-R4 `regstate` — the golden corpus
  is unchanged unless a fixture or `--fpregs` arms it. The `has_vec`/`--fpregs`
  gate means a deck's *absence* is a truthful "not measured", never a rendered zero.
- **Both producers, one field-order owner.** The emulator's `emit_regstate` reads
  the XMM file from its per-step ring (already captured) and MXCSR from a parallel
  ring; the live ring reads all 16 XMM + MXCSR in one `PTRACE_GETFPREGS` per step.
  Both serialize through the single `asmtrace_regstate_vec_append`
  ([cli/asmtrace_ndjson.c](../../../cli/asmtrace_ndjson.c)), so the two producers
  spell the deck identically — the property `cli/test_regstate_parity.c` pins:
  **XMM is base-INDEPENDENT** (a function of the inputs, so the two producers
  byte-agree once written), while MXCSR, like `rip`/`rsp`, may differ by basis.
- **Consumer today.** The Scrubber index skips non-integer `values` fields
  ([desktop/src/analysis/stepindex.cpp](../../../desktop/src/analysis/stepindex.cpp)),
  so the hex XMM strings are **inert** until an FP-deck panel renders them (a T2
  follow-up), while `mxcsr` (an integer) already renders as a named register.

## `regstate` descriptor — `emu_arm64_regs_t@aarch64/aapcs64` (R5 arm64 ring)

> **Owned by [32-per-guest-value-producer.md](32-per-guest-value-producer.md)**
> (T2, the regstate/Scrubber half), appended under this file's D5 append-only
> rule. It gives the `regstate` kind its **third** concrete descriptor — the
> emulator's per-step AArch64 register ring — and **adds no field to any
> existing kind and no new envelope major.** The kind, the `{"desc","values"}`
> shape and the descriptor-reference rule are unchanged and remain 01's.

The AArch64 analogue of the x86-64 emulator ring
([include/asmtest_emu.h](../../../include/asmtest_emu.h) —
`emu_arm64_step_capture`), scoped to the separate `emu_arm64_t` handle (arm64
has always been its own guest handle type, distinct from the x86-64 `emu_t`;
this ring is the one Track F seam it gains — `emu_arm64_t` still has no
snapshot/restore, that stays an x86-64 `emu_t` / Reweave concern). The corpus
recorder ([tools/asmtrace_record.c](../../../tools/asmtrace_record.c)) bakes a
ring cap into the arm64 golden fixtures directly (there is no `--steps`-style
CLI flag for it yet, mirroring how the arm64 value fabric is exercised via
byte-literal fixtures rather than the host-arch corpus loop); after the run it
emits one `regstate` event per held pre-state, referencing this descriptor.

**Descriptor id.** `emu_arm64_regs_t@aarch64/aapcs64` — the `emu_arm64_regs_t`
struct row of [`asmtest_abi.json`](../../../scripts/gen-manifest.c#L127) (arch
`aarch64`, abi `aapcs64`), the full AArch64 emulator register file
([include/asmtest_emu.h:213](../../../include/asmtest_emu.h#L213)).

**`values` fields.** The 31 general-purpose registers `x0`..`x30` (AAPCS64
gives `x29`/`x30` their FP/LR roles, but the descriptor names the physical
register — exactly as the x86-64 descriptor names `rbp`/`rsp` by register, not
role) plus `sp`, `pc`, and `nzcv`, in `emu_arm64_regs_t` **declaration order**,
each a decimal `u64`:

```
x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 x10 x11 x12 x13 x14 x15 x16 x17 x18 x19 x20 x21 x22 x23 x24 x25 x26 x27 x28 x29 x30 sp pc nzcv
```

The pre-state of `arm64-df-chain.asmtrace`'s first held step (args `x0=7,
x1=5`; `x30`/`sp`/`pc` are the guest's fixed entry state, not yet the routine's
own values):

```json
{"k":"regstate","desc":"emu_arm64_regs_t@aarch64/aapcs64","values":{"x0":7,"x1":5,"x2":0,"x3":0,"x4":0,"x5":0,"x6":0,"x7":0,"x8":0,"x9":0,"x10":0,"x11":0,"x12":0,"x13":0,"x14":0,"x15":0,"x16":0,"x17":0,"x18":0,"x19":0,"x20":0,"x21":0,"x22":0,"x23":0,"x24":0,"x25":0,"x26":0,"x27":0,"x28":0,"x29":0,"x30":15728640,"sp":2162672,"pc":1048576,"nzcv":0}}
```

- **No vector/NEON deck** — like the x86-64 descriptor's original v1 omission,
  a wide value is not a bare JSON integer; `emu_arm64_regs_t.v[32]` is not
  captured here (a further descriptor row, mirroring the x86-64 wide-deck
  extension above), exactly as the arm64 value fabric's own scope stayed
  integer-only ([32](32-per-guest-value-producer.md) T2).
- **The reader has no arm64-specific field-order table.**
  `stepindex_reg_order()`
  ([desktop/src/analysis/stepindex.cpp](../../../desktop/src/analysis/stepindex.cpp))
  still lists only the x86-64 18 names; an arm64 `values` object's field names
  all fall through its generic "any other integer key, sorted" path. The
  Scrubber therefore renders every field — time-travel works end to end — just
  in a lexicographic rather than a hand-curated order (`nzcv` before `pc`
  before `sp` before `x0` `x1` `x10`...). Cosmetic, not correctness: the same
  kind of follow-on the value fabric's `loom_reg_name` degradation was left as,
  not a blocker for this one.
- **No live (ptrace) arm64 producer exists.** This descriptor has exactly one
  producer today, the emulator ring; a live AArch64 single-step engine is a
  separate host concern ([32](32-per-guest-value-producer.md) Non-goals).

**Order, dropping and truncation (D7).** Identical discipline to the x86-64
emulator ring: held pre-states are emitted **oldest first**, each snapshotted
BEFORE its instruction; when more steps run than the ring holds, the
**earliest** entries are evicted and the `end` footer carries
`"truncated":true` plus the evicted count in `drops.lost`, so a reader offsets
the first held step by `drops.lost` and renders the missing prefix as a torn
edge. A recording with no `regstate` events simply had the arm64 ring unarmed
— the `arm64-df-chain.asmtrace` golden without one is the normal case (the
un-augmented value-fabric-only recording R5 T2 originally shipped).

## `fpenv` — the FP/SIMD environment (rounding / sticky / FTZ-DAZ)

> **Owned by [31-wide-register-deck.md](31-wide-register-deck.md)** (T2), appended
> under this file's D5 append-only rule. It **promotes the reserved `fpenv` kind
> to defined** (see *Reserved kinds*), giving it its first fields. An ordinary
> opt-in recording event — it rides inside `[header … end]`, the footer counts it,
> and it is **derived** from the MXCSR the wide deck (above) already captures, so it
> **adds no new capture and no new envelope major**. Off by default (the `--fpregs`
> opt-in arms it with the deck), so a recording without it is normal.

One `fpenv` event per held step (paired with that step's `regstate`, like
`statediff`), decoding the step's **MXCSR** into the FP-environment fields a panel
needs — the SSE rounding mode, the sticky exception flags, and the flush-to-zero /
denormals-are-zero bits:

```json
{"k":"fpenv","step":1,"mxcsr":8064,"round":"nearest","ftz":false,"daz":false,"sticky":[]}
```

| Field | Type | Meaning |
|---|---|---|
| `step` | int | the step this environment is the pre-state of (pairs 1:1 with `regstate`) |
| `mxcsr` | int | the raw 32-bit MXCSR (the source of every field below; a reader can re-derive) |
| `round` | string | the RC field (MXCSR[14:13]): `"nearest"` \| `"down"` \| `"up"` \| `"zero"` |
| `ftz` | bool | flush-to-zero (MXCSR[15]) |
| `daz` | bool | denormals-are-zero (MXCSR[6]) |
| `sticky` | array | the set exception flags (MXCSR[5:0]), each of `"ie" "de" "ze" "oe" "ue" "pe"` in bit order |

- **Derived, not re-measured.** Every field is a pure function of `mxcsr`, carried
  decoded so a viewer needs no MXCSR bit knowledge; `mxcsr` rides alongside so the
  decode is auditable. x87 control/status is **not** carried (the corpus is SSE;
  an x87 fixture would extend this row, not replace it).
- **Graceful degradation (D7).** `fpenv` is emitted only where the wide deck is armed
  and MXCSR was actually read; a step whose MXCSR is unrecorded emits **no** `fpenv`
  (never a fabricated default), exactly as a disarmed ring emits no `regstate`.

## `severity` — the derivable fidelity-chrome tier (optional)

> **Owned by [01-asmtrace-format.md](01-asmtrace-format.md)**, appended under this
> file's D5 append-only rule at the request of
> [23-graded-truth-layer.md](23-graded-truth-layer.md) (T1, F5). It adds ONE
> **optional** provenance field, **derivable** from fields this schema already
> carries, and **no new envelope major** and **no field to any existing kind**.
> The field GRADES how loud a reader renders fidelity chrome; it gates **no truth
> off** — every fidelity field still renders regardless of the tier (D7). Because
> it is derivable and optional, an old recording with no `severity` still grades:
> a reader computes the tier from the fidelity facts. **01 owner sign-off: recorded
> 2026-07-27 (the Phase-3-freeze checkpoint, D5).**

```json
{"backend":"ptrace-syscalls","exact":true,"trust":"exact","severity":"neutral"}
```

An optional `provenance` string, one of `"neutral" | "caution" | "integrity"`,
naming the tier a reader renders this recording's fidelity chrome at. It is the
**dominant** (loudest) tier over the recording's active fidelity signals.

| Field | Type | Required | Meaning |
|---|---|---|---|
| `severity` | str | no | `"neutral"` \| `"caution"` \| `"integrity"` — the fidelity-chrome tier. Absent → the reader **derives** it (below). |

**The tier is derivable.** A reader that does not find `severity` computes it from
the existing fidelity fields, so grading is a property of the data, not of the
producer:

- **integrity** — the recording is **torn** (no `end` footer), a **mixed-basis
  refusal**, or a **drop on an EXACT capture** (`drops.lost > 0` / `throttled`
  with `exact:true` — its addresses become UNKNOWN). Rendered loud (red) and
  **non-collapsible**, exactly as the refusal path always was.
- **caution** — **truncated-but-usable** (`end.truncated:true` with a usable
  prefix, not torn) or a **paused gap** (`paused_dropped`). Rendered amber, and a
  reader **may collapse** it to a chip after first read (its text is unchanged, so
  it is never gone).
- **neutral** — a **skip** (a successful session with nothing to report,
  schema:98), a **statistical** survey (`trust:"statistical"`, including one that
  dropped — sampling drops are expected and the survey never claimed
  completeness), **redacted-by-policy** (`redacted:true`), a **bounded window**,
  or the **coarse-provenance rung**. Rendered as a quiet chip, no warn colour.

When the field IS present it is honoured verbatim (a producer that graded at
capture time overrides the derivation). Two readings must never disagree, so a
producer that emits `severity` MUST emit the tier its own fidelity fields derive
to. The rule is the same one `codeimage` states: a reader that gets it wrong is
silently wrong. The reference mapper is
[`fidelity_severity`](../../../desktop/src/ui/fidelity.h) — pure, derivable, and
pinned against the committed low-fidelity fixtures
([tests/golden-asmtrace/low-fidelity/](../../../tests/golden-asmtrace/low-fidelity/)).

## `df_step` region tag — the `rbase` extension (37)

> **Owned by [37-region-tag-on-df-step.md](37-region-tag-on-df-step.md)** (T1),
> appended under this file's D5 append-only rule. It **adds one optional field**
> (`rbase`) to the existing `df_step` kind (see *`df_step`* above for the field,
> the normative order `step, off, rbase?, disasm?, ops`, the omit-when-unknown
> rule, and the five-part resolution rule) — **no new kind, no new descriptor, no
> new envelope major**. A `rbase == 0` (omitted) `df_step` is byte-identical to a
> pre-37 recording, which is why the append is a break-free additive change under
> *Ignore unknown fields*, and why 36's single-`codeimage`-span anchor stays the
> permanent fallback for every untagged recording. **01 owner sign-off: `rbase`
> adds an optional field to a known kind, stated by the producer that already
> holds the base, and no reader in the tree rejects it — the pre-37 corpus and the
> deliberately-untagged hand-authored fixtures remain valid, and the freeze
> checklist item *"`df_step` states no region"* is closed by it. Recorded
> 2026-07-29.**
