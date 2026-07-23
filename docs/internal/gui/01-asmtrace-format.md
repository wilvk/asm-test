# `.asmtrace` recording contract + record modes — implementation

> **Sources.** Actioned from [desktop-gui-plan.md](../plans/desktop-gui-plan.md) — section **D1
> "The `.asmtrace` contract"**, **Phasing → Phase 1**, and its four matching engine-work-item rows
> (NDJSON serializers per sink + record mode; JSON mode for `--log`/`--stream`; `vec512_t`
> manifest row; payload-free `format_syscall` line variant). Written 2026-07-23. If this doc and a
> source disagree, this doc wins (sources may be stale); if the CODE and this doc disagree,
> re-verify against code before implementing. The doc set's shared decisions D1–D11 (this
> directory's README) are binding. Siblings: [02-exporters-and-readers.md](02-exporters-and-readers.md)
> consumes the schema; [03-desktop-shell.md](03-desktop-shell.md) the golden corpus;
> [07-serve-live-host.md](07-serve-live-host.md) streams the same events.

## Why this work exists

The GUI plan's central inversion is "the repo needs a protocol, not renderings": every viewer,
exporter, and live feed is a reader or writer of one NDJSON recording format. None of it exists
yet — no `.asmtrace`, no `--record`, no JSON mode for `--log`/`--stream` anywhere in
[cli/asmspy.c](../../../cli/asmspy.c) (re-verified at HEAD `a460d40`). This root doc of the
nine-doc set defines the **draft** schema (the v1 *freeze* is a Phase-3 checkpoint, not here),
lands record modes as new sinks beside asmspy's existing print/JSON sinks, closes the
state-descriptor precondition (`vec512_t` manifest row), and commits the golden corpus —
including the dishonesty fixtures (D7) — that every sibling's tests replay. The honesty culture
is enforced by the format: provenance is mandatory on every stream; truncation/drop/redaction
are fields, not renderer discipline.

## What already exists (verified 2026-07-23)

- [cli/asmspy.c](../../../cli/asmspy.c) — the sink pairs the record sinks sit beside:
  [`log_print_sink`](../../../cli/asmspy.c#L796),
  [`stream_print_sink`](../../../cli/asmspy.c#L815),
  [`tree_print_sink`](../../../cli/asmspy.c#L836) /
  [`tree_capture_sink`](../../../cli/asmspy.c#L859),
  [`graph_capture_sink`](../../../cli/asmspy.c#L1021),
  [`sample_capture_sink`](../../../cli/asmspy.c#L1148) (the **one-window contract** — `stop ==
  NULL` runs exactly one window — at [:1214](../../../cli/asmspy.c#L1214)),
  [`topo_capture_sink`](../../../cli/asmspy.c#L1288),
  [`region_print_sink`](../../../cli/asmspy.c#L1410), [`watch_sink`](../../../cli/asmspy.c#L1622),
  the one-shot [`dataflow_render_sink`](../../../cli/asmspy.c#L1805). Per-subcommand option loops
  in [`main`](../../../cli/asmspy.c#L5278) (`--log` at [:5307](../../../cli/asmspy.c#L5307)); the
  [`usage`](../../../cli/asmspy.c#L5218) block.
- [cli/asmspy.h](../../../cli/asmspy.h) — sink signatures + skip codes:
  [`asmspy_syscall_sink`](../../../cli/asmspy.h#L210) (line + **separate** `str` payload — the
  payload channel exists; a payload-free *line* does not),
  [`asmspy_tree_call_t`](../../../cli/asmspy.h#L404),
  [`asmspy_sample_edge_t`](../../../cli/asmspy.h#L502) +
  [`asmspy_sample_sink`](../../../cli/asmspy.h#L516) (samples / branch_samples / lost /
  throttled), [`asmspy_watch_hit_t`](../../../cli/asmspy.h#L573),
  [`asmspy_task_t`](../../../cli/asmspy.h#L461); skip codes at [:236](../../../cli/asmspy.h#L236)
  / [:524](../../../cli/asmspy.h#L524) / [:286](../../../cli/asmspy.h#L286) /
  [:555](../../../cli/asmspy.h#L555) / [:562](../../../cli/asmspy.h#L562); reasons via
  [`asmspy_hwdebug_reason`](../../../cli/asmspy.h#L611) and
  [`asmspy_strerror`](../../../cli/asmspy.h#L618).
- [`format_syscall`](../../../cli/asmspy_engine.c#L1671) builds the full line **embedding**
  decoded buffers, paths, and sockaddrs ([`ap_sockaddr`](../../../cli/asmspy_engine.c#L1182)); one
  call site at [:2773](../../../cli/asmspy_engine.c#L2773). T4 splits it.
- [`asmtest_trace_t`](../../../include/asmtest_trace.h#L44): offsets are region-relative by
  contract ([:42](../../../include/asmtest_trace.h#L42)), **but** the region-free whole-window
  scope fills the same struct with **absolute** addresses
  ([include/asmtest_hwtrace.h:453](../../../include/asmtest_hwtrace.h#L453)) — both bases verified
  real, hence the mandatory `basis` tag. Stitched per-tid slice sets:
  [`asmtest_hwtrace_stitch_handles`](../../../include/asmtest_hwtrace.h#L641). Disassembly:
  [`asmtest_disas`](../../../include/asmtest_trace.h#L275),
  [`emu_disas`](../../../include/asmtest_emu.h#L479) (D10; "" without Capstone).
- [include/asmtest_valtrace.h](../../../include/asmtest_valtrace.h) —
  [`at_val_rec_t`](../../../include/asmtest_valtrace.h#L61),
  [`asmtest_valtrace_t`](../../../include/asmtest_valtrace.h#L93),
  [`asmtest_defuse_edge_t`](../../../include/asmtest_valtrace.h#L171),
  [`asmtest_defuse_build`](../../../include/asmtest_valtrace.h#L190).
- [`asmtest_dataflow_emu_run`](../../../src/dataflow_emu.c#L249) — the emulator L0 producer
  (x86-64 guest, deterministic: zeroed GP file, fixed bases). **No public header** — consumers
  re-declare it ([examples/test_dataflow_emu.c:25](../../../examples/test_dataflow_emu.c#L25));
  built at [mk/dataflow.mk:62](../../../mk/dataflow.mk#L62), link shape at
  [:354](../../../mk/dataflow.mk#L354).
- [scripts/gen-manifest.c](../../../scripts/gen-manifest.c) — [`vec256_t` has a
  row](../../../scripts/gen-manifest.c#L79); **`vec512_t` has none** (verified by reading the
  whole file) though the type exists ([include/asmtest.h:124](../../../include/asmtest.h#L124)) —
  the T2 gap.
- [tools/asmfeatures.c](../../../tools/asmfeatures.c) — the JSON-emit style
  ([`json_str`](../../../tools/asmfeatures.c#L47), `{"features":[...]}` at
  [:368](../../../tools/asmfeatures.c#L368)); the producer of `result`-kind features rows.
- [`asmtest_corpus_routine`](../../../bindings/conformance/corpus_routines.c#L32) (name → routine
  pointer); the 64-byte-window convention for a corpus routine under the emulator:
  [conformance.c:317](../../../bindings/conformance/conformance.c#L317).
- [mk/cli.mk](../../../mk/cli.mk) — where the new sinks compile: pattern rule
  ([:49](../../../mk/cli.mk#L49)), [`ASMSPY_OBJS`](../../../mk/cli.mk#L58), `cli-smoke`
  ([:366](../../../mk/cli.mk#L366), running [cli/cli_smoke.sh](../../../cli/cli_smoke.sh) at
  [:401](../../../mk/cli.mk#L401)), a headless-unit-test rule to copy ([`test_view`,
  :301](../../../mk/cli.mk#L301)), [`docker-cli`](../../../mk/cli.mk#L407) (bindings base: pinned
  Capstone 5.0.1 + libunicorn,
  [Dockerfile.bindings-base:44](../../../Dockerfile.bindings-base#L44)).
- [Makefile](../../../Makefile): the hand-maintained [`help`](../../../Makefile#L91) list;
  `mk/cli.mk` included at [:929](../../../Makefile#L929). IBS reasons:
  [`asmtest_ibs_skip_reason`](../../../include/asmtest_ibs.h#L127) /
  [`asmtest_ibs_unavail_reason`](../../../include/asmtest_ibs.h#L137).
- Test wiring: [tests/expect.sh](../../../tests/expect.sh) covers the framework runner only, so
  this doc's checks wire into **cli_smoke.sh**. Changelog anchor:
  [CHANGELOG.md:7](../../../CHANGELOG.md#L7). Baseline proof before touching anything:
  `make docker-cli` is green end to end.

## Tasks

### T1 — Draft schema doc `asmtrace-schema.md`  (M, depends on: none)

**Goal.** The binding written contract every sibling codes against.

**Steps.**
1. Create `docs/internal/gui/asmtrace-schema.md` (D5: lives here until the Phase-3 freeze;
   `docs/internal` is outside the Sphinx `-W` build — no toctree work) with sections, in order:
   *Envelope*, *Provenance*, *Event kinds (v1)*, *Reserved kinds*, *State descriptors*,
   *Determinism rules*, *Compatibility rules*, *Example* (one complete small recording). Every v1
   kind names its C source struct with a file:line link.

**Code.** The normative content, summarized:

- **Envelope.** Line 1 is the header object; every later line one event.
  `{"asmtrace":1,"container":"ndjson","producer":{"name":...,"version":"<ASMTEST_VERSION>"},"provenance":{...},"arch":"x86_64"}`
  plus optional `"pid"`/`"cmd"`/`"ts"` (all omitted in deterministic mode); readers reject
  `asmtrace` > 1 with a named reason. `"container":"zstd-frames"` is **reserved, not implemented
  in v1** — a v1 reader must refuse it by name, never misparse it.
- **Provenance (mandatory).**
  `{"backend":str,"exact":bool,"trust":"exact"|"statistical"|"weak"|"strong","window":{...}?,"skip":{"code":N,"reason":str}?,"redacted":bool?}`.
  `skip.code` is the positive asmspy skip code; `skip.reason` the measured string. Statistical
  streams MUST set `"exact":false`; readers never merge them into exact kinds.
- **Event kinds (v1)** — field `"k"` selects:
  - `trace` — `{"basis":"rel"|"abs","kind":"insn"|"block","off":u64,"disasm":str?}`; `basis`
    mandatory (both bases verified real, above); `disasm` = D10.
  - `coverage` — `{"basis",...,"blocks":[u64],"blocks_total","insns_total","truncated"}`.
  - `syscall` — `{"line":str,"payload":str?,"tid":int?}`; `line` is **payload-free** (T4);
    `payload` is the separated channel renderers default-redact (D7).
  - `stream` — `{"text":str}`; the `--stream` engine hands a formatted line only
    ([cli/asmspy.h:336](../../../cli/asmspy.h#L336)) — recorded honestly as text.
  - `call` — the [`asmspy_tree_call_t`](../../../cli/asmspy.h#L404) fields (tid/depth/addr/name/module).
  - `graph` / `topo` — snapshots of gnode+gedge / asmspy_task_t fields.
  - `survey` — `{"sampler":"ibs-op"|"sw-clock","edges":[sample-edge
    fields],"samples","branch_samples","lost","throttled"}`; always `exact:false`.
  - `watch` — the [`asmspy_watch_hit_t`](../../../cli/asmspy.h#L573) fields incl. `value_ok`/`value_len`.
  - `df_step` — `{"step","off","disasm"?,"ops":[at_val_rec_t fields +
    "space":"reg"|"abs"|"off"]}`; `df_edge` — `{"from","to","loc":{...}}` per
    [`asmtest_defuse_edge_t`](../../../include/asmtest_valtrace.h#L171).
  - `regstate` — `{"desc":"<id>","values":{...}}`; a descriptor **reference**, never a register list.
  - `result` — test/bench/features rows, exactly the [asmfeatures](../../../tools/asmfeatures.c#L368) shape.
  - `note` — annotation `{"text",off?,step?,stop?}`; ordered stops make a walkthrough a recording.
  - `stitch` — per-tid PT slice set `{"tid","slices":[{"version","offs":[u64]}]}` mirroring
    [`asmtest_hwtrace_stitch_handles`](../../../include/asmtest_hwtrace.h#L641); **defined in v1,
    no v1 writer** (producer arrives with [07-serve-live-host.md](07-serve-live-host.md)).
  - `end` — footer `{"events","truncated","drops":{...},"skip":{...}?}`; a file **without** an
    `end` event is a torn recording and readers must say so.
- **Reserved kinds** (registry rows only, fields unfrozen): `mem`, `fpenv`, `statediff`, `blame`,
  `fuzzstats`, `taint`, `take` (take/edit provenance, the Loom fork mechanic). Additions are new
  registry rows under the ignore-unknown-kinds rule — never a v2 envelope.
- **State descriptors.** A descriptor id names a struct row of `asmtest_abi.json` (e.g.
  `"emu_x86_regs_t@x86_64/sysv"`); a recording may embed descriptor objects in its header
  (`"descriptors":[...]`) to be self-contained. Precondition: T2.
- **Determinism (D6).** Field order is fixed as documented; the writer emits fields in that order;
  deterministic mode omits `ts`/`pid`/`cmd`. Golden regeneration must be byte-identical.
- **Compatibility.** Ignore unknown kinds; ignore unknown fields; reject newer majors; `basis` and
  `exact` may never be defaulted by a reader.

**Tests.** None of its own — T3/T7 test the schema by construction.

**Docs.** This task *is* a doc.

**Done when.**
- All eight sections exist; every v1 kind links its C source struct file:line.
- Its embedded example passes T7's `test_asmtrace` fixture parser.

### T2 — `vec512_t` manifest row  (S, depends on: none)

**Goal.** Close the named descriptor precondition: `vec512_t` is absent from the ABI manifest.

**Steps.**
1. In [scripts/gen-manifest.c](../../../scripts/gen-manifest.c), after the `vec256_t` block
   ([:79](../../../scripts/gen-manifest.c#L79)), add the mirror block (below).
2. `make manifest`; commit the regenerated `asmtest_abi.json` with the source change.

**Code.**
```c
    BEGIN(vec512_t); /* AVX-512 wide-vector capture (asm_call_capture_vec512) */
    FIELD(vec512_t, u8);
    FIELD(vec512_t, u64);
    FIELD(vec512_t, f64);
    end_struct();
```
(`vec512_t`: [include/asmtest.h:124](../../../include/asmtest.h#L124); no header change.)

**Tests.** `make manifest && python3 -c "import json;m=json.load(open('asmtest_abi.json'));assert
any(s['name']=='vec512_t' and s['size']==64 for s in m['asmtest_abi']['structs'])"`.

**Docs.** The schema doc's descriptor section cites the row.

**Done when.**
- The python one-liner passes; `git diff` shows only the new block + the regenerated JSON.

### T3 — Shared NDJSON writer TU (`asmtrace_emit`)  (M, depends on: T1)

**Goal.** One tested writer both asmspy (`--record`, T5) and the Author-mode recorder (T6) link,
so field order — and therefore byte stability — has exactly one owner.

**Steps.**
1. Create `cli/asmtrace_ndjson.h` + `cli/asmtrace_ndjson.c`. **Pure C11 + stdio** — no
   ptrace/ncurses/Capstone — so T6's tools binary compiles it anywhere the emulator runs. Built
   via the cli pattern rule ([mk/cli.mk:49](../../../mk/cli.mk#L49)); add
   `$(BUILD)/asmtrace_ndjson.o` to [`ASMSPY_OBJS`](../../../mk/cli.mk#L58).
2. Implement per the schema doc: header, per-kind emit, `end` footer, JSON escaping (mirror
   [`json_escape`](../../../cli/asmspy.c#L304): escape `"` `\` and control bytes as `\uXXXX`).
3. Add `cli/test_asmtrace.c`, a headless CHECK-style unit test built like
   [`test_view`](../../../mk/cli.mk#L301) (own rule, `-Icli`, links only `asmtrace_ndjson.o`), run
   from [cli/cli_smoke.sh](../../../cli/cli_smoke.sh) beside the other unit tests.

**Code.**
```c
/* cli/asmtrace_ndjson.h — the .asmtrace NDJSON writer.
 * Contract: docs/internal/gui/asmtrace-schema.md. New file, new symbols. */
typedef struct {
    const char *backend;     /* "ptrace-syscalls", "emu-l0", "ibs-op", ... */
    int exact;               /* 1 exact, 0 statistical                     */
    const char *trust;       /* "exact"|"statistical"|"weak"|"strong"      */
    int skip_code;           /* 0 = none; else the positive ASMSPY_* code  */
    const char *skip_reason; /* NULL = none (measured string, borrowed)    */
    int redacted;            /* 1 when payloads are withheld at record     */
} asmtrace_prov_t;

typedef struct {
    FILE *f;
    int deterministic;         /* omit ts/pid/cmd (golden mode) */
    unsigned long long events; /* event lines emitted so far    */
    int truncated;             /* sticky; folded into `end`     */
} asmtrace_writer_t;

int asmtrace_open(asmtrace_writer_t *w, const char *path, int deterministic);
/* Header line. producer = "asmspy" / "asmtrace_record". 0 / -1 on I/O. */
int asmtrace_header(asmtrace_writer_t *w, const char *producer,
                    const asmtrace_prov_t *prov, long pid, const char *cmd);
/* One event line: {"k":"<kind>",<body>}\n. `body` = PRE-FORMATTED JSON
 * fields (no leading comma) built with asmtrace_escape'd strings. */
int asmtrace_emit(asmtrace_writer_t *w, const char *kind, const char *body);
int asmtrace_emitf(asmtrace_writer_t *w, const char *k, const char *fmt, ...);
void asmtrace_escape(char *dst, size_t cap, const char *src);
/* Writes the `end` event (events/truncated/drops/skip), then fclose. A
 * writer closed without this leaves a TORN recording — deliberate (a
 * crash mid-record must be visible): no atexit magic. */
int asmtrace_close(asmtrace_writer_t *w, unsigned long long lost,
                   int throttled, const asmtrace_prov_t *skip_update);
```

**Tests.** In `cli/test_asmtrace.c` (standalone TAP `CHECK` main): `writer.header_line_first`,
`writer.field_order_fixed` (two identical writes → `cmp`-identical files), `writer.escape_edges`
(quote, backslash, newline, 0x01), `writer.deterministic_omits_ts_pid`,
`writer.end_event_counts`, `writer.torn_without_close`, plus a line-oriented re-read of its own
output asserting `"asmtrace":1` and every `"k"` written (the full reader library is
[02-exporters-and-readers.md](02-exporters-and-readers.md)'s).

**Docs.** Header comment points at the schema doc.

**Done when.**
- `make cli-smoke` runs `test_asmtrace` green on x86-64 and via `make docker-cli
  DOCKER_PLATFORM=linux/arm64` on arm64.
- Two consecutive writes of the same events are byte-identical.

### T4 — Payload-free syscall line + `--json` for `--log`/`--stream`  (M, depends on: T3)

**Goal.** The redaction gate: a syscall line variant carrying **no** buffer bytes, paths, or
sockaddr contents — and NDJSON output for the two text-only streaming modes (verified text-only
today: their sinks just `printf` the line, see the What-exists citations).

**Steps.**
1. Extend [`format_syscall`](../../../cli/asmspy_engine.c#L1671) with a second output buffer —
   `format_syscall(char *b, size_t cap, char *pf, size_t pfcap, char *sout, size_t scap, ...)`.
   `pf` is the payload-free rendering: the helpers that print *content* — `ap_data` (buffer
   bytes), path arguments, [`ap_sockaddr`](../../../cli/asmspy_engine.c#L1182) — write
   placeholders (`<N bytes>`, `<path>`, `<sockaddr>`) into `pf`, while `b` keeps today's full
   text; syscall name, fds, flag words, counts, and return value stay identical in both.
2. Widen the sink typedef ([cli/asmspy.h:210](../../../cli/asmspy.h#L210)) to `(void *ctx, const
   char *line, const char *pf_line, const char *str)`. Exactly two implementations update:
   [`log_print_sink`](../../../cli/asmspy.c#L796) and the TUI's
   [`live_syscall_sink`](../../../cli/asmspy.c#L2526) (both ignore `pf_line`).
3. Add `--json` to `--log` and `--stream` (option loops at [:5307](../../../cli/asmspy.c#L5307) /
   [:5402](../../../cli/asmspy.c#L5402)): stdout receives **the same NDJSON a `--record` file
   gets** — header, `syscall`/`stream` events, `end` — via a T3 writer bound to stdout. One
   schema, one writer; `asmspy --log <pid> --json > x.asmtrace` *is* a recording. Update
   [`usage`](../../../cli/asmspy.c#L5218).
4. Extend the `argdecode_victim` leg of [cli/cli_smoke.sh](../../../cli/cli_smoke.sh): assert
   JSON-mode `"line"` values contain `<path>` / `<N bytes>` and do **not** contain the known
   payload strings the victim writes, while `"payload"` does.

**Code.** The `--log` record/json sink emits `{"k":"syscall","line":"<escaped pf>","payload":
"<escaped str>"}` (payload omitted when `str == NULL`), echoing the full `line` to stdout only in
plain `--log` mode. Context: `typedef struct { asmtrace_writer_t *w; int echo; } log_rec_ctx;`

**Tests.** Step 4, plus: `--log <pid> 5 --json | head -1` greps `"asmtrace":1`; the last line
greps `"k":"end"`.

**Docs.** Usage block; the schema doc already describes the split (T1).

**Done when.**
- `make cli-smoke` green including the new argdecode assertions.
- A `--json` run's stdout parses as one header + N events + one `end`.
- No `argdecode_victim` payload string appears in any `"line"` field.

### T5 — `--record=<file>` across the headless subcommands  (M, depends on: T3, T4)

**Goal.** Every headless mode can write a `.asmtrace` recording, as **new sinks beside** the
existing print/JSON sinks — the Phase-1 acceptance line.

**Steps.**
1. Add `--record=<path>` to every headless subcommand's option loop in
   [`main`](../../../cli/asmspy.c#L5278) (`--log --trace --dataflow --stream --graph --tree
   --procs --sample --watch`); each `cmd_*` gains a `const char *record` parameter (NULL = off).
2. In each `cmd_*`: `asmtrace_open` + `asmtrace_header` **before attach** (static provenance:
   producer `"asmspy"`, backend, exact/trust), tee the engine sink into a record sink beside the
   existing one, then `asmtrace_close` with measured drops. Backends: `ptrace-<mode>` per ptrace
   view (all `exact:true`); `ibs-op` / `sw-clock` (`exact:false`, `trust:"statistical"`).
3. Kind mapping (shapes from T1): `--log` → `syscall` (T4 sink); `--stream` → `stream`; `--trace`
   → per-invocation `trace` events (`basis:"rel"`, offsets from the region sink's `tr`; `disasm`
   via [`asmtest_disas`](../../../include/asmtest_trace.h#L275) where available — D10) + one
   `coverage` per invocation; `--tree` → `call` (copy name/module — transient, the
   [`tree_capture_sink`](../../../cli/asmspy.c#L859) discipline); `--graph` / `--procs` → one
   final `graph` / `topo` snapshot at detach; `--sample` → one `survey` (the one-window
   contract); `--watch` → `watch` per hit; `--dataflow` → `df_step` + `df_edge` (mirror
   [`dataflow_render_sink`](../../../cli/asmspy.c#L1805)'s walk).
4. **Skips are recordings too:** when an engine returns a positive skip code, the recording still
   closes cleanly — the `end` event carries `skip:{code,reason}` with the measured reason
   ([`asmtest_ibs_unavail_reason`](../../../include/asmtest_ibs.h#L137),
   [`asmspy_hwdebug_reason`](../../../cli/asmspy.h#L611), `asmspy_strerror`). Exit codes
   unchanged.
5. Update [`usage`](../../../cli/asmspy.c#L5218) and add a `--record` note to the cli lines of
   [`make help`](../../../Makefile#L91).

**Code.** Tee-context shape, one per mode (`--trace` shown): `typedef struct { const
asmspy_symtab_t *syms; asmtrace_writer_t *w; } region_rec_ctx;` — `region_record_sink` calls
`region_print_sink(...)`, then emits the trace/coverage events from `tr`/`code`/`base` in the
same sink call. **Threading rule:** record sinks run on the engine's tracer thread like the print
sinks they wrap (the ptrace per-thread rule, [cli/asmspy.h:14](../../../cli/asmspy.h#L14)); one
engine, one sink thread, one file — no lock; the two-phase detach machinery is untouched.

**Tests.** New cli_smoke block per mode against the existing victims:
`--record=$tmp/<mode>.asmtrace`, then grep the header line, one mode-appropriate `"k":"..."`, and
a final `"k":"end"`. For `--sample` on a non-AMD host the assertion is the **skip recording**
(`"skip"` in `end`, exit 0); `--watch` mirrors it where arming is refused.

**Docs.** Usage + help (step 5). User-facing guide pages wait for the freeze.

**Done when.**
- Every headless subcommand accepts `--record=` and produces a parseable recording in cli-smoke
  (`make docker-cli` green).
- A skip run produces a *closed* recording carrying the measured reason.
- `--record` and `--json` compose (same events, file + stdout).

### T6 — `tools/asmtrace_record.c` + `make asmtrace-golden`  (M, depends on: T1, T3)

**Goal.** The Author-mode corpus recorder behind D6: run conformance-corpus routines under the
deterministic emulator L0 producer and emit `.asmtrace` with `df_step`/`df_edge` events + optional
`disasm` (D10) — what `make asmtrace-golden` runs.

**Steps.**
1. Create `tools/asmtrace_record.c`: re-declare
   [`asmtest_dataflow_emu_run`](../../../src/dataflow_emu.c#L249) (no public header — the
   [test_dataflow_emu precedent](../../../examples/test_dataflow_emu.c#L25)), resolve each
   routine via [`asmtest_corpus_routine`](../../../bindings/conformance/corpus_routines.c#L32),
   copy the **64-byte window** from the routine pointer (the
   [conformance emu convention](../../../bindings/conformance/conformance.c#L317)), run on fixed
   args, build the graph with
   [`asmtest_defuse_build`](../../../include/asmtest_valtrace.h#L190), and write one
   deterministic-mode recording per routine (producer `"asmtrace_record"`, backend `"emu-l0"`).
2. Fixed routine table, in this order: `add_signed {40,2}`, `sum_via_rbx {40,2}`, `clobbers_rbx
   {40,2}`, `sum3 {1,2,3}`, `set_carry {}`, `clear_carry {}` — integer-arg routines only (the
   producer marshals integer args).
3. Per step, attach `disasm` via [`emu_disas`](../../../include/asmtest_emu.h#L479) over the
   copied window; without Capstone the field is omitted (D10 degradation) — but the golden lane
   always has pinned Capstone (gate below).
4. Build rules in [mk/cli.mk](../../../mk/cli.mk): `$(BUILD)/corpus_routines.o` (plain
   `$(CC) $(CFLAGS) -c` of `bindings/conformance/corpus_routines.c`), then
   `$(BUILD)/asmtrace_record` linking `asmtrace_record.o asmtrace_ndjson.o corpus_routines.o
   add.o flags.o dataflow.o dataflow_operands.o dataflow_emu.o emu.o trace.o disasm.o` with
   `$(UNICORN_LIBS) $(CAPSTONE_LIBS)` (the
   [test_dataflow_emu link shape](../../../mk/dataflow.mk#L354)).
5. Targets: `asmtrace-golden` (build + run, writing `tests/golden-asmtrace/*.asmtrace`) and
   `asmtrace-golden-check` (regenerate into a temp dir, `diff -r` against the committed corpus).
   Gate both to an x86-64 host with libunicorn (the corpus `.s` routines are host-arch — the
   conformance emu leg's gate); print the gate reason otherwise. The **authoritative** lane is
   `docker-cli`: host Capstone 4.x renders different disasm text
   ([mk/cli.mk:400](../../../mk/cli.mk#L400)) and must not regenerate goldens.
6. Add `asmtrace-golden` to [`make help`](../../../Makefile#L91).

**Code.**
```c
/* tools/asmtrace_record.c — Author-mode conformance-corpus recorder.
 * Deterministic by construction: asmtest_dataflow_emu_run zeroes the GP file
 * and uses fixed bases; the writer runs deterministic (no ts/pid/cmd). */
int main(int argc, char **argv); /* argv[1] = output directory */
```

**Tests.** `make asmtrace-golden && make asmtrace-golden-check`, twice (self-consistency).

**Docs.** Header comment; help line (step 6).

**Done when.**
- `make asmtrace-golden` writes one `.asmtrace` per table row, each with `df_step` (incl.
  `disasm`) + `df_edge` events and a clean `end`.
- `make asmtrace-golden-check` is byte-clean on consecutive runs inside the `docker-cli` image.

### T7 — Golden corpus + dishonesty fixtures + stability checks  (M, depends on: T5, T6)

**Goal.** The committed `tests/golden-asmtrace/` corpus (D6) including the dishonesty fixtures
(D7), with the byte-stability check wired into the existing cli-smoke entry point.

**Steps.**
1. Run `make asmtrace-golden` in the `docker-cli` image; commit the output (flat, one file per
   routine: `add_signed.asmtrace`, ...).
2. Hand-author `tests/golden-asmtrace/dishonest/` (committed, never regenerated; each header's
   `"note"` names its purpose): `truncated.asmtrace` (`end` has `"truncated":true`; `coverage`
   shows `blocks_total` > `len(blocks)`), `dropped.asmtrace` (`survey` with
   `"lost":12345,"throttled":true`, `exact:false`), `redacted.asmtrace` (`syscall` events with
   placeholder lines, **no** `payload` field, `"redacted":true`), `torn.asmtrace` (no `end`).
3. Extend `cli/test_asmtrace.c` with fixture checks reading these files:
   `fixture.truncated_flag_surfaces`, `fixture.dropped_counts_surface`,
   `fixture.redacted_has_no_payload`, `fixture.torn_detected` — the reader-level half of D7. The
   banner/provenance-chrome/redaction *renderer* assertions live in
   [03-desktop-shell.md](03-desktop-shell.md) / [08-observer-views.md](08-observer-views.md),
   replaying these same files.
4. Wire into cli-smoke: the fixture checks ride `test_asmtrace` (T3); add an
   `asmtrace-golden-check` invocation to the [`cli-smoke` recipe](../../../mk/cli.mk#L366) under
   the T6 gate (real run in `docker-cli`; printed arch-gate reason elsewhere).
5. Add one live round-trip to cli_smoke: record `--trace` on `spy_victim` twice and assert the two
   files are identical after dropping the header line (`tail -n +2 | cmp` — the header carries
   `ts`/`pid`), proving the live writer shares the golden writer's field-order stability.

**Tests.** Steps 3–5 are the tests. Failure modes: a field-order change breaks
`asmtrace-golden-check` (byte diff — intended); a reader regression breaks a named `fixture.*`
check.

**Docs.** `tests/golden-asmtrace/README.md` (3 short paragraphs: what regenerates the flat files;
`dishonest/` is never regenerated; the docker-lane-authoritative rule).

**Done when.**
- `git ls-files tests/golden-asmtrace` shows the corpus + 4 dishonesty fixtures + README.
- `make docker-cli` is green including `asmtrace-golden-check` and all `fixture.*` checks.

### T8 — Changelog  (S, depends on: T5, T6, T7)

**Goal.** The user-visible record of the new surface.

**Steps.**
1. Append one `### Added` bullet under `## [Unreleased]` in
   [CHANGELOG.md](../../../CHANGELOG.md#L7): `--record=<file>` on every asmspy headless
   subcommand; `--json` NDJSON mode for `--log`/`--stream` with payload-free syscall lines; the
   draft `.asmtrace` schema; `make asmtrace-golden`; the `vec512_t` ABI-manifest row.

**Tests / Docs.** This task is the doc.

**Done when.** The bullet exists; `make docker-docs` still builds (the schema doc is under
`docs/internal/**`, excluded from Sphinx, per [_conventions.md](../implementations/_conventions.md)).

## Task order & parallelism

- **T1** and **T2** are independent roots; start both immediately (T2 is a half-day).
- **T3** needs T1's field tables. **T4** and **T6** both need only T3 (+T1) and touch disjoint
  files (engine formatter vs tools/), so two people can work them concurrently.
- **T5** follows T3 and consumes T4's syscall split; **T7** integrates T5 + T6; **T8** closes.

Critical path: **T1 → T3 → T5 → T7 → T8**. Off-path: T2 (any time before the descriptor section is
reviewed), T4 and T6 in parallel with each other.

## Constraints & gates

- **Draft, not freeze.** The schema ships marked *draft*; the v1 freeze is the named Phase-3
  checkpoint after siblings 02/03/07 have consumed it. Additive registry rows are cheap; a field
  rename must regenerate the golden corpus in the same change.
- **Determinism is docker-lane-authoritative.** Golden bytes depend on the pinned Capstone 5.0.1
  disasm text; regenerate only in the `docker-cli` image
  ([mk/cli.mk:407](../../../mk/cli.mk#L407)).
- **Real gates vs installable deps** (per [CLAUDE.md](../../../CLAUDE.md) /
  [_conventions.md](../implementations/_conventions.md)): the recorder's x86-64-host requirement
  is an architecture gate (host-arch corpus `.s`) — record and skip with a reason off x86-64;
  libunicorn/Capstone are installable and already in the lane image — never a self-skip. AMD IBS
  hardware for a live `survey` is a hardware gate; T5 tests the **skip recording** on non-AMD
  hosts; hardware-backed record smokes ride `hw.yml` later
  ([07-serve-live-host.md](07-serve-live-host.md)).
- **Threading/detach.** See T5's Code note: tracer-thread sinks, no lock, detach untouched.
- **Licensing (D4/D9).** Everything lands in `cli/` + `tools/`, linking only what asmspy already
  links; nothing in `desktop/`; the render-only viewer reads these files with zero engine deps.
- **Formatting.** The repo `.clang-format` applies (D8); `make docker-fmt-check` before review.

## Out of scope

- **Readers, exporters, zstd container** — the reader library, speedscope/Perfetto exporters, and
  any compressed-container implementation:
  [02-exporters-and-readers.md](02-exporters-and-readers.md) (the envelope only *reserves*
  `"container":"zstd-frames"`).
- **Renderer-side honesty chrome** (banner, provenance chips, redaction reveal UX):
  [03-desktop-shell.md](03-desktop-shell.md) / [08-observer-views.md](08-observer-views.md),
  replaying this doc's fixtures.
- **`--serve`** — the same events live: [07-serve-live-host.md](07-serve-live-host.md).
- **Producers for reserved kinds** (`mem`, `fpenv`, `statediff`, `blame`, `taint`, `take`) and
  the `stitch` writer — expansion waves / Phase 3. Likewise **structuring the `--stream` line**
  into fields (engine work; v1 records the text honestly).
- **Runner record mode** (per-test recordings + failure deep links) — the plan's separate medium
  engine item, owned by [04-replay-views.md](04-replay-views.md)'s phase.
- **FP/vector corpus routines in the golden set** — the emulator L0 producer marshals integer
  args; widening the recorder is future work alongside the descriptor-driven register deck.
