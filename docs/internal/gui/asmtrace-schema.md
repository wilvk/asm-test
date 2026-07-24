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
- **Dishonesty is a field, not an omission.** Truncation, drops, throttling,
  redaction and a torn (unterminated) file are all *representable* and therefore
  *testable* — see
  [`tests/golden-asmtrace/`](../../../tests/golden-asmtrace/README.md), whose
  four hand-authored `dishonest/` fixtures each carry a `note` event stating
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
  [`ASMSPY_REGION_NEVER_RAN`](../../../cli/asmspy.h#L236) 1,
  [`ASMSPY_SAMPLE_UNAVAIL`](../../../cli/asmspy.h#L524) 2,
  [`ASMSPY_DATAFLOW_UNAVAIL`](../../../cli/asmspy.h#L286) 3,
  [`ASMSPY_WATCH_UNAVAIL`](../../../cli/asmspy.h#L555) 4,
  [`ASMSPY_ETRACEE_I386`](../../../cli/asmspy.h#L562) 5. `reason` comes from the
  measuring source — [`asmtest_ibs_unavail_reason`](../../../include/asmtest_ibs.h#L137),
  [`asmspy_hwdebug_reason`](../../../cli/asmspy.h#L611),
  [`asmspy_strerror`](../../../cli/asmspy.h#L618) — never a guess.
- **A skip is a recording.** A run that skipped still writes a header and a
  clean `end`; the `end` carries the skip. An empty file is a *bug*; a
  skip-carrying file is *data*.

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
buffer caps, which is what makes `blocks_total > len(blocks)` an honest
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
([`asmspy_stream_sink`](../../../cli/asmspy.h#L336)), so v1 records the text
honestly rather than inventing fields it did not measure. Structuring the line
is engine work, out of scope here.

### `call` — one call-tree entry

```json
{"k":"call","tid":4242,"depth":2,"addr":4198710,"name":"helper","module":"spy_victim"}
```

Fields mirror [`asmspy_tree_call_t`](../../../cli/asmspy.h#L413). `name`/`module`
are transient in the sink and MUST be copied by the writer.

### `graph` — whole-process call-graph snapshot

```json
{"k":"graph","nodes":[{"addr":4198710,"name":"work","module":"spy_victim","kind":"internal","invocations":3,"out_calls":6,"fanout":2}],"edges":[{"from":4198710,"to":4198800,"count":6}]}
```

Node fields mirror [`asmspy_gnode_t`](../../../cli/asmspy.h#L365) plus the
machine-readable class token `kind` (`internal` \| `external` \| `jit` \|
`unknown`) the JSON exporter already computes; edge fields mirror
[`asmspy_gedge_t`](../../../cli/asmspy.h#L374), keyed by entry **address** (not
node index) so a consumer may sort/filter nodes without invalidating edges. One
snapshot is written at detach.

### `topo` — process/thread topology snapshot

```json
{"k":"topo","mode":"syscalls","tasks":[{"tid":4242,"tgid":4242,"ppid":4200,"leader":true,"comm":"victim","exe":"spy_victim","inv":91}]}
```

Task fields mirror [`asmspy_task_t`](../../../cli/asmspy.h#L470); `mode` is what
`inv` counts (`"syscalls"` \| `"calls"`).

### `survey` — statistical hot-edge histogram

```json
{"k":"survey","sampler":"ibs-op","edges":[{"from_addr":4198710,"to_addr":4198800,"from":"work+0x12 [victim]","to":"helper","count":812,"mispred":3,"is_return":0}],"samples":10442,"branch_samples":9001,"lost":0,"throttled":false}
```

Edge fields mirror [`asmspy_sample_edge_t`](../../../cli/asmspy.h#L509); the
four trailing counters are the sink's honesty channel. **Always `exact:false`.**
`lost`/`throttled` are the drop record: a survey that dropped samples says so.

### `watch` — one hardware data-watchpoint hit

```json
{"k":"watch","hit_no":1,"tid":4242,"pc":4198750,"addr":6295624,"is_write":1,"value_ok":true,"value_len":4,"value":42,"func":"work","module":"watch_victim","off":18}
```

Fields mirror [`asmspy_watch_hit_t`](../../../cli/asmspy.h#L585). `is_write` is
`1` write / `0` read / `-1` undecodable — the third value is a real measurement
outcome and MUST NOT be collapsed into either other. `func`/`module` are omitted
when unresolved.

### `df_step` — one executed step's operand values (L0)

```json
{"k":"df_step","step":0,"off":0,"disasm":"mov eax, edi","ops":[{"space":"reg","reg":35,"size":4,"write":false,"value_valid":true,"value":40}]}
```

Operand objects mirror [`at_val_rec_t`](../../../include/asmtest_valtrace.h#L61)
with the enum rendered as a token: `space` is `"reg"` (`AT_LOC_REG`) \| `"abs"`
(`AT_LOC_MEM_ABS`) \| `"off"` (`AT_LOC_MEM_OFF`). Operand field order:
`space`, `reg`, `base`, `index`, `scale`, `disp`, `addr`, `size`, `write`,
`value_valid`, `wide`, `value`. Memory addressing terms
(`base`/`index`/`scale`/`disp`/`addr`) are omitted for a register operand;
`value` is omitted when `value_valid` is false. A value wider than 8 bytes sets
`"wide":true` and omits `value` (the wide side buffer is not serialized in v1 —
a documented v1 limit, not a silent drop).

### `df_edge` — one last-writer def-use edge (L1)

```json
{"k":"df_edge","from":0,"to":2,"loc":{"space":"reg","reg":35,"size":4,"write":false,"value_valid":true,"value":40}}
```

Mirrors [`asmtest_defuse_edge_t`](../../../include/asmtest_valtrace.h#L178):
the value written at step `from` is read at step `to` through `loc` (the
consumer's read record, same operand shape as `df_step.ops`).

### `regstate` — a register-file snapshot, by descriptor reference

```json
{"k":"regstate","desc":"emu_x86_regs_t@x86_64/sysv","values":{"rax":42,"rbx":0}}
```

`desc` is a **reference** to a state descriptor (see below), never an inline
register list — that is what lets one viewer render an x86-64, AArch64 or RISC-V
register deck without knowing any of them at compile time.

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
{"k":"end","events":37,"truncated":false,"drops":{"lost":0,"throttled":false},"skip":{"code":2,"reason":"IBS-Op is an AMD feature; this host is GenuineIntel"}}
```

`events` counts the event lines **before** this one (the header is not an
event, and `end` does not count itself). `skip` is present only when the run
skipped.

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
| `mem` | per-step memory access `{step,ea,size,rw}` | [10-spacetime-3d-overview.md](10-spacetime-3d-overview.md) |
| `fpenv` | FP/SIMD environment + wide register state | expansion wave |
| `statediff` | step-to-step architectural state delta | expansion wave |
| `blame` | attribution of a value to a source location | [09-teaching-producers.md](09-teaching-producers.md) |
| `fuzzstats` | corpus/coverage counters from a fuzz run | expansion wave |
| `taint` | taint labels propagated through a step | expansion wave |
| `srcmap` | one source line-map row `{off,value,kind,file,col}`, mirroring [`asmtest_srcmap_entry_t`](../../../include/asmtest_trace.h#L177) | [05-loom-day-one.md](05-loom-day-one.md) |
| `take` | take/edit provenance (the Loom fork mechanic) | [05-loom-day-one.md](05-loom-day-one.md) |
| `codeimage` | captured code bytes at a version | [08-observer-views.md](08-observer-views.md) |
| `session` / `cmd` / `err` | live-session lifecycle (serve-only) | [07-serve-live-host.md](07-serve-live-host.md) |

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

- **No routine identity.** The Envelope names the producer, the provenance, the
  arch and (outside deterministic mode) the pid and cmd — but nothing that says
  *which code this is*. A consumer that compares two recordings therefore
  cannot verify they are of the same routine: `dt_diff_build`
  ([04-replay-views.md](04-replay-views.md) T6) checks basis and arch, and
  states in every diff that routine identity is the reader's assertion rather
  than a finding. A `code` header object (`{"name":str,"sha256":str,"len":int}`)
  would close it; the corpus recorder already copies a fixed 64-byte window and
  could hash it. Raised 2026-07-24 by 04.
- **No block starts from the L0 producer.** `coverage` is defined and the
  region tiers write it, but the emulator L0 value producer measures executed
  *steps* and has no block information, so the generated corpus carries `trace`
  and `df_step` events and no `coverage`. Block starts cannot be recovered from
  an offset stream without instruction lengths, so the recorder emits none
  rather than guessing. Raised 2026-07-24 by 04.
- **The wide side buffer is not serialised.** `df_step.ops[]` marks a >8-byte
  value `"wide":true` and omits `value` (see `df_step` above). A reader renders
  it as `[wide]` and cannot show the bytes. Documented as a v1 limit at
  authoring time; listed here so the freeze either closes it or confirms it.

## Example

A complete, minimal recording — the reference a reader is tested against
(`cli/test_asmtrace.c` extracts exactly this block from this file and parses it):

```json
{"asmtrace":1,"container":"ndjson","producer":{"name":"asmtrace_record","version":"1.1.0"},"provenance":{"backend":"emu-l0","exact":true,"trust":"exact"},"arch":"x86_64"}
{"k":"note","text":"add_signed(40,2) under the deterministic emulator"}
{"k":"df_step","step":0,"off":0,"disasm":"mov eax, edi","ops":[{"space":"reg","reg":19,"size":4,"write":false,"value_valid":true,"value":40},{"space":"reg","reg":35,"size":4,"write":true,"value_valid":true,"value":40}]}
{"k":"df_step","step":1,"off":2,"disasm":"add eax, esi","ops":[{"space":"reg","reg":35,"size":4,"write":false,"value_valid":true,"value":40},{"space":"reg","reg":43,"size":4,"write":false,"value_valid":true,"value":2},{"space":"reg","reg":35,"size":4,"write":true,"value_valid":true,"value":42}]}
{"k":"df_edge","from":0,"to":1,"loc":{"space":"reg","reg":35,"size":4,"write":false,"value_valid":true,"value":40}}
{"k":"trace","basis":"rel","kind":"insn","off":0,"disasm":"mov eax, edi"}
{"k":"trace","basis":"rel","kind":"insn","off":2,"disasm":"add eax, esi"}
{"k":"coverage","basis":"rel","blocks":[0],"blocks_total":1,"insns_total":3,"truncated":false}
{"k":"end","events":7,"truncated":false,"drops":{"lost":0,"throttled":false}}
```
