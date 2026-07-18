# Native trace-point to IL/bytecode/source-line attribution — implementation

> **Sources.** Actioned from
> [il-bytecode-attribution.md](../analysis/il-bytecode-attribution.md) (the
> feasibility analysis and its "single cheapest first step"),
> [jit-runtime-tracing.md](../analysis/jit-runtime-tracing.md) (the jitdump /
> temporal-bytes background), and
> [data-flow-capture.md](../analysis/data-flow-capture.md) (the "IL-to-native
> map or data flow stays at assembly level" boundary). Written 2026-07-17. If
> this doc and a source disagree, this doc wins (sources may be stale); if the
> CODE and this doc disagree, re-verify before implementing.

## Why this work exists

asm-test can attach to a live JIT runtime and tell you *which native
instructions* of a managed method ran — but it names managed code only at
**method** level. A captured native point cannot today be tied to a source
line, a .NET IL offset, or a JVM bytecode index, even though the runtimes
already hand us the data: the jitdump files we parse carry
`JIT_CODE_DEBUG_INFO` records that the reader currently **skips**, CoreCLR
raises a `MethodILToNativeMap` event we never subscribe, and HotSpot's JVMTI
`CompiledMethodLoad` delivers an address→bci map we never capture. This doc
ingests those three existing feeds — cheapest first — so a traced native
offset resolves to `file:line`, an IL offset, or a bci, version-keyed so a
re-JITted method resolves against the body that was live at capture time.

## What already exists (verified 2026-07-17)

- [include/asmtest_trace.h](../../../include/asmtest_trace.h#L135) — the only
  generic address→semantics hook: `emu_line_entry_t { uint64_t offset;
  uint32_t line; }` and `emu_line_map_t` (lines 135–143), consumed by
  `emu_line_lookup` / `emu_trace_source_report` / `emu_trace_lcov_source`
  (declared lines 145–151). The row carries **no file id, no IL offset, no
  bci** — that is the schema gap this doc closes. Implementations are in
  [src/trace.c](../../../src/trace.c#L243) (`emu_line_lookup` :243,
  `emu_trace_source_report` :313); nearest-preceding-row lookup semantics.
- [src/ptrace_backend.c](../../../src/ptrace_backend.c#L134) — the binary
  jitdump reader `asmtest_jitdump_find` (Linux arm :134–256, arch-independent,
  any Linux; `#else` ENOSYS stub for non-Linux :277). It parses the 40-byte
  v1 file header, auto-detects endianness from the magic, and **skips every
  record whose id is not `JIT_CODE_LOAD`** (:190–194) — including the
  `JIT_CODE_DEBUG_INFO` (id 2) records that carry per-address line info.
  Latest-timestamp LOAD wins (re-JIT at a reused address).
- [include/asmtest_ptrace.h](../../../include/asmtest_ptrace.h#L422) —
  `asmtest_jitdump_entry_t` (:422–431) and the `asmtest_jitdump_find`
  prototype (:446) with its caller-owned-buffer convention. NOTE: this header
  is in the bindings-parity gate's `TIER_HEADERS`
  ([scripts/check-bindings-parity.sh](../../../scripts/check-bindings-parity.sh)),
  so any new function declared here must either be wrapped by all ten
  bindings or exempted in
  [scripts/bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt).
  `include/asmtest_trace.h` is NOT in that list.
- [examples/test_hwtrace.c](../../../examples/test_hwtrace.c#L6217) — the
  synthetic jitdump writer `write_jit_load` (:6217) and reader self-test
  `test_jitdump_reader` (:6243), which already writes a non-LOAD record to
  exercise skipping and asserts latest-body-wins. Runs on any Linux under
  `make hwtrace-test` (called from `main` :8311).
- [examples/jit_trace.c](../../../examples/jit_trace.c#L416) — the argv-driven
  live-runtime harness. `trace_jitdump` (:429) drives V8 (`jitdump` mode:
  `node --perf-prof --perf-basic-prof`) and CoreCLR (`dotnet-jitdump` mode:
  `DOTNET_PerfMapEnabled=1`); `trace_jitdump_java` (:595) drives HotSpot via
  the perf JVMTI agent (`libperf-jvmti.so`), whose jitdump "interleaves
  debug/unwinding records the reader must skip" (comment :578–593) and only
  flushes on clean JVM shutdown (SIGTERM, not SIGKILL). All lanes self-skip
  honestly when the runtime is absent or does not cooperate.
- Make targets ([mk/native-trace.mk](../../../mk/native-trace.mk#L2219)):
  `hwtrace-jit-jitdump` (:2226), `hwtrace-jit-dotnet-jitdump` (:2256),
  `hwtrace-jit-java-jitdump` (:2315, resolves `PERF_JVMTI` by glob :2214).
  Docker lanes ([mk/docker.mk](../../../mk/docker.mk#L518)):
  `docker-hwtrace-jit-jitdump`, `docker-hwtrace-jit-dotnet-jitdump`,
  `docker-hwtrace-jit-java-jitdump` run them in the per-language images. The
  java image installs `openjdk-25-jdk-headless linux-tools-generic`
  (`DOCKER_APT_java`, [mk/docker.mk](../../../mk/docker.mk#L148) — the
  linux-tools package ships `libperf-jvmti.so`); the dotnet image installs
  `dotnet-sdk-8.0` (:149).
- [include/asmtest_codeimage.h](../../../include/asmtest_codeimage.h) — the
  time-aware code-image recorder: `asmtest_codeimage_now()` returns the
  monotonic capture sequence, `asmtest_codeimage_bytes_at(img, addr, when)`
  answers "bytes live at addr as of sequence `when`" (when==0 → latest).
  This is the version timeline the attribution registry (T4) keys against.
- [bindings/dotnet/hwtrace/HwTrace.cs](../../../bindings/dotnet/hwtrace/HwTrace.cs#L3873)
  — `JitMethodMap : EventListener` (:3873) already enables provider
  `Microsoft-Windows-DotNETRuntime` with `JitKeyword 0x10` (:3877, enabled
  :4055) and parses `MethodLoadVerbose*` payloads by name (:4058–4085).
  `GcMoveMap` (:4262) shows the byte[]-blob fallback for struct-array
  payloads an in-proc `EventListener` may deliver (:4330–4345).
  `DiagnosticsIpc` (:4364) already implements `EnablePerfMap(JitDump)` over
  the diagnostics socket. The **`0x20000` IL-map keyword is subscribed
  nowhere** — `grep -rn 0x20000 bindings/ src/ include/` and
  `grep -rn "il_offset\|bci\|MethodILToNativeMap" include/ src/` both return
  zero hits (verified 2026-07-17).
- [src/hwtrace.c](../../../src/hwtrace.c#L2852) — `perfmap_symbol_at` (:2852)
  reads only `start size name` from the text perf-map: the method-level naming
  ceiling this doc raises. `asmtest_hwtrace_render_versioned` (:2486) is the
  existing codeimage-versioned consumer to mirror.

Prove the baseline green before touching anything (host may be macOS; all
lanes are Docker-first per CLAUDE.md):

```
make docker-hwtrace                     # C smoke incl. test_jitdump_reader → "ok" TAP lines, exit 0
make docker-hwtrace-jit-jitdump         # V8: "recovered real V8 method from jit-<pid>.dump ..."
make docker-hwtrace-jit-java-jitdump    # HotSpot: "recovered real HotSpot method from the agent's jitdump ..."
make docker-hwtrace-jit-dotnet-jitdump  # CoreCLR: "recovered real CoreCLR method ..." (lane prints CHECK passes)
make check-bindings-parity              # "parity OK" (no drift), runs on the host
```

## Tasks

### T1 — Parse the jitdump `JIT_CODE_DEBUG_INFO` records the reader skips  (S, depends on: none)

**Goal.** `asmtest_jitdump_debug_find()` returns a named method's per-address
source-line entries from a jitdump, and a bridge helper turns them into the
existing `emu_line_map_t` shape, proven by a synthetic-file self-test.

**Steps.**

1. Declare the new API in
   [include/asmtest_ptrace.h](../../../include/asmtest_ptrace.h#L446),
   directly below `asmtest_jitdump_find` (:446), mirroring its doc-comment
   style and caller-owned-buffer convention.
2. Implement it in
   [src/ptrace_backend.c](../../../src/ptrace_backend.c#L134) inside the same
   `#if defined(__linux__)` readers block (:44), and add an ENOSYS stub in the
   `#else` block next to the existing one (:277–290).
3. Add the parity-gate exemptions (step 5 of **Code**) to
   [scripts/bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt).
4. Extend the self-test in
   [examples/test_hwtrace.c](../../../examples/test_hwtrace.c#L6243) and run
   `make docker-hwtrace`.

**Code.**

```c
/* One source-attribution entry recovered from a jitdump JIT_CODE_DEBUG_INFO
 * record, offset-relative to the matched method's code_addr. */
typedef struct {
    uint64_t off;       /* entry addr - code_addr (byte offset into the body) */
    uint32_t line;      /* 1-based source line                                */
    uint32_t discrim;   /* column discriminator (V8 writes the column here)   */
    char     file[128]; /* NUL-terminated source file, truncated to fit       */
} asmtest_jitdump_debug_t;

int asmtest_jitdump_debug_find(const char *path, pid_t pid, const char *name,
                               asmtest_jitdump_entry_t *out,
                               asmtest_jitdump_debug_t *dbg, size_t dbg_cap,
                               size_t *dbg_len);
size_t asmtest_jitdump_debug_line_map(const asmtest_jitdump_debug_t *dbg,
                                      size_t n, emu_line_entry_t *rows,
                                      size_t cap);
```

Parser rules (all layout facts are from the perf jitdump spec — see Research
notes):

1. Reuse the existing header/record framing: 40-byte file header (magic
   `0x4A695444`, byte-swapped `0x4454694A` → set `swap`), 16-byte record
   prefix `{id u32, total_size u32, timestamp u64}`; read fields with the
   existing `jd_rd32`/`jd_rd64` (:120–128) so cross-endian dumps keep
   working. **Byte-swap every debug-entry field including `discrim`** (perf's
   own reader does — `tools/perf/util/jitdump.c:324`).
2. Single pass, "pending debug" model: on `id == 2` (`JIT_CODE_DEBUG_INFO`,
   body `{code_addr u64, nr_entry u64}` then `nr_entry` × `{addr u64,
   lineno u32, discrim u32, file char[] NUL}`), parse into a scratch buffer
   keyed by `code_addr`, replacing any previous pending record for the same
   `code_addr`. The spec **requires** the DEBUG_INFO record to be written
   *before* the JIT_CODE_LOAD it describes, so on a matching `id == 0` LOAD
   (same name test as `asmtest_jitdump_find`), adopt the pending record whose
   `code_addr` equals the LOAD's `code_addr` — and, exactly like the byte
   path, let the **latest matching LOAD win** so a re-JITted method resolves
   to its newest map.
3. Convert entry addresses to offsets (`addr - code_addr`); drop entries
   outside `[code_addr, code_addr + code_size)` (defensive: some encoders
   emit prologue markers). Copy at most `dbg_cap` entries; always set
   `*dbg_len` (0 when the method exists but carries no debug info — return
   `ASMTEST_PTRACE_OK` in that case so callers can distinguish "no method"
   (`ENOENT`) from "no debug info"). Malformed record → `EINVAL`, matching
   the sibling's error contract.
4. `asmtest_jitdump_debug_line_map` sorts a copy ascending by `off`, dedups
   equal offsets (keep the last, matching latest-wins), fills at most `cap`
   `emu_line_entry_t{offset,line}` rows, and returns the row count — the
   bridge that makes `emu_trace_source_report` / `emu_trace_lcov_source`
   work on a traced JIT method **today**, with zero schema change.
5. Parity gate: add to `scripts/bindings-parity-allow.txt`, with a comment in
   the file's existing style:

   ```
   # Jitdump debug-info readers (native-il-bytecode-attribution): C-tier only
   # until a binding grows an attribution surface; remove per-binding when wrapped.
   ALL asmtest_jitdump_debug_find
   ALL asmtest_jitdump_debug_line_map
   ```

**Tests.** Extend
[examples/test_hwtrace.c](../../../examples/test_hwtrace.c#L6243): add a
`write_jit_debug()` helper next to `write_jit_load` (:6217) that writes an
id-2 record (`total_size` = 16 + 16 + Σ(16 + strlen(file)+1)), and a new
`test_jitdump_debug_reader()` called from `main` next to
`test_jitdump_reader()` (:8311). Synthesize: DEBUG_INFO(code_addr 0x1000,
entries {0x1000,line 10,"a.java"} {0x1004,11} {0x1010,12}) → LOAD(ts 2,
0x1000) → DEBUG_INFO(0x2000, entries {0x2000,20} {0x2008,21}) → LOAD(ts 3,
0x2000, same name) → LOAD(ts 4, other name, no debug). Assert: (a) the named
method resolves with `dbg_len == 2`, offsets `{0,8}`, lines `{20,21}` —
latest body's map won; (b) `file` round-trips; (c) the no-debug method
returns OK with `dbg_len == 0`; (d) missing name → `ENOENT`; (e) the bridge
yields an ascending 2-row `emu_line_map_t` and `emu_line_lookup(&map, 0xb)`
returns line 21. Failure looks like a `not ok` TAP line from the lane's
`CHECK`; a pass adds `ok` lines to `make docker-hwtrace` output.

**Docs.** Internal-only at this task (user-facing guide lands in T7); no
changelog yet (T7 carries the single user-visible entry).

**Done when.**

- `make docker-hwtrace` passes with the new `jitdump-debug:` CHECK lines.
- `make check-bindings-parity` still exits 0 (exemptions consumed, none stale).
- `make check` and `make fmt-check` pass (header edit is C11-clean;
  clang-format applied).

### T2 — Validate the debug-info reader against all three live producers  (S, depends on: T1)

**Goal.** The V8, HotSpot, and CoreCLR jitdump lanes print an offset→file:line
table for the recovered method and CHECK the reader's behavior against each
real encoder.

**Steps.**

1. In [examples/jit_trace.c](../../../examples/jit_trace.c#L416), add a
   `print_debug_table()` helper: call `asmtest_jitdump_debug_find` with the
   same `(path, pid, name)` the byte recovery used, print
   `    +0x<off>  <file>:<line>[.<discrim>]` per entry, and return `dbg_len`.
2. Wire it into `trace_jitdump` (:429, after the four existing CHECKs) and
   `trace_jitdump_java` (:595, step D after byte validation).
3. Per-producer assertions (this is the encoder-diversity point of the task):
   - **V8** (`jitdump` mode, `node --perf-prof`): CHECK `dbg_len >= 1` and
     every `off < e.code_size` with offsets non-decreasing. V8 writes line
     *and column* (column lands in `discrim`); print both.
   - **HotSpot** (`java-jitdump`, libperf-jvmti): CHECK `dbg_len >= 1`, file
     contains `Hot.java` (the agent resolves it via `GetSourceFileName`), and
     offsets in-range. The agent buffers and flushes on clean shutdown — the
     existing SIGTERM flow (:687–707) already handles this; read debug info
     from the same flushed dump path `find_java_jitdump(pid)` returned.
   - **CoreCLR** (`dotnet-jitdump`): CHECK the call returns
     `ASMTEST_PTRACE_OK` — and print an informational line either way.
     CoreCLR's PAL jitdump writer emits only `JIT_CODE_LOAD` (debug-info is a
     standing runtime TODO — [il-bytecode-attribution.md](../analysis/il-bytecode-attribution.md)
     "It is not in the jitdump"), so today this prints
     `# CoreCLR jitdump carries no debug-info records (expected; IL route is T5)`.
     Do NOT hard-assert `dbg_len == 0`: a future SDK that starts emitting is
     a pass with a table, not a failure.
4. Run each lane: `make docker-hwtrace-jit-jitdump`,
   `docker-hwtrace-jit-java-jitdump`, `docker-hwtrace-jit-dotnet-jitdump`.

**Code.** ~60 lines in `examples/jit_trace.c`; a `static asmtest_jitdump_debug_t
dbg[256];` caller buffer follows the existing fixed-buffer style (`jbytes[1024]`).
No make-target changes: the existing lanes pick the new checks up.

**Tests.** The lanes ARE the tests (live-runtime validation, self-skipping per
the existing pattern only when the runtime/agent is genuinely absent — which
in the per-language images it is not). A pass adds e.g.
`ok ... v8-jitdump-debug: 5 entries resolve inside the method body` and the
printed table; a failure is a `not ok` from CHECK with the encoder named.

**Docs.** Internal-only; the user-facing story lands in T7.

**Done when.**

- All three docker lanes pass; V8 and HotSpot print non-empty tables; the
  CoreCLR lane prints the expected-absence note (or a table, never a failure).
- On a host without Docker/node/java the corresponding `hwtrace-jit-*` host
  targets still self-skip with the existing printed reasons (no new gates).

### T3 — The attribution schema: `asmtest_srcmap` (file id + kind + IL/bci values)  (M, depends on: none)

**Goal.** A widened, backend-neutral attribution row — offset → {kind, value,
file, column} where kind ∈ {source-line, IL-offset, bytecode-index} — with
lookup and trace-report helpers, unit-tested, without touching the shipped
`emu_line_map_t` ABI.

**Steps.**

1. Add the schema to
   [include/asmtest_trace.h](../../../include/asmtest_trace.h#L151), directly
   after the `emu_line_*` block (:151), so the two hooks read as one family.
   `asmtest_trace.h` is not parity-gated (verified against
   `check-bindings-parity.sh` `TIER_HEADERS`), so no allowlist edits.
2. Implement in [src/trace.c](../../../src/trace.c#L243) next to
   `emu_line_lookup` (:243), mirroring its nearest-preceding-row semantics
   and `emu_trace_source_report`'s (:313) report shape.
3. Unit-test in `examples/test_hwtrace.c` (pure C, no OS gate — same file as
   T1's test so the whole attribution surface is exercised by one lane).

**Code.**

```c
/* Attribution kinds: what `value` means for a row. */
#define ASMTEST_SRC_LINE 1 /* value = 1-based source line                  */
#define ASMTEST_SRC_IL   2 /* value = .NET IL offset (pseudo-offsets pass) */
#define ASMTEST_SRC_BCI  3 /* value = JVM bytecode index                   */
/* .NET pseudo-IL-offsets (ICorDebugInfo), passed through unmodified: */
#define ASMTEST_SRC_IL_NO_MAPPING (-1)
#define ASMTEST_SRC_IL_PROLOG     (-2)
#define ASMTEST_SRC_IL_EPILOG     (-3)

typedef struct {
    uint64_t offset;  /* native byte-offset from the method/routine base   */
    int32_t  value;   /* line / IL offset / bci per kind (pseudo allowed)  */
    uint32_t kind;    /* ASMTEST_SRC_*                                     */
    uint32_t file_id; /* index into files[]; UINT32_MAX = no file          */
    uint32_t col;     /* column / discriminator; 0 = none                  */
} asmtest_srcmap_entry_t;

typedef struct {
    const asmtest_srcmap_entry_t *entries; /* ascending by .offset */
    size_t count;
    const char *const *files; /* file_id -> path; may be NULL   */
    size_t files_count;
} asmtest_srcmap_t;

const asmtest_srcmap_entry_t *asmtest_srcmap_lookup(const asmtest_srcmap_t *m,
                                                    uint64_t off);
size_t asmtest_srcmap_report(const asmtest_trace_t *covered,
                             const asmtest_srcmap_t *m, FILE *out);
size_t asmtest_srcmap_from_jitdump(const asmtest_jitdump_debug_t *dbg, size_t n,
                                   asmtest_srcmap_entry_t *rows, size_t cap,
                                   const char **file_out);
```

- `lookup`: last row with `offset <= off` owns it (identical loop shape to
  `emu_line_lookup`). This encodes the documented granularity ceiling: the
  answer is the **enclosing** debug point, never a per-instruction claim.
- `report`: like `emu_trace_source_report` but prints kind-labelled values
  (`line 21`, `IL_0x1a`, `PROLOG`, `bci 7`) and per-file grouping when
  `files` is set; returns the number of covered-but-unattributed blocks so a
  caller can assert attribution coverage.
- `from_jitdump` (declared in `asmtest_ptrace.h` would drag the ptrace header
  into trace.h — declare it in `asmtest_ptrace.h` instead, implement in
  `src/ptrace_backend.c`, and exempt it in the parity allowlist like T1's
  symbols): kind `ASMTEST_SRC_LINE`, `col = discrim`, single-file common case
  via `*file_out` (first entry's file; rows whose file differs get
  `file_id = UINT32_MAX` and the report prints them unattributed — the
  flattened-inline caveat the analysis records).

**Tests.** In `examples/test_hwtrace.c`: static row tables for each kind;
assert lookup boundaries (before-first → NULL, between rows → preceding row,
past-last → last row — mirror the `emu_line_lookup` test shape in
[examples/test_emu.c](../../../examples/test_emu.c#L314)); assert pseudo-IL
rows report their names; assert `asmtest_srcmap_report` returns 0 when every
covered block resolves and N when rows are removed. Build a trace with the
public append path (`trace_append_insn` is internal — fill a stack
`asmtest_trace_t` the way `test_jitdump_reader` fills structs). Run
`make docker-hwtrace`.

**Docs.** Internal-only here; T7 documents the schema publicly. No ABI
manifest change: the new structs are caller-constructed, not
trampoline-filled, so [scripts/gen-manifest.c](../../../scripts/gen-manifest.c)
is untouched (it covers emu/regs structs only).

**Done when.**

- `make docker-hwtrace` passes with the new srcmap CHECK lines.
- `grep -n asmtest_srcmap include/asmtest_trace.h` shows the schema; `make
  check-header-portability` (strict-C11 include gate) passes.
- `make check-bindings-parity` passes (only the two ptrace-header symbols
  needed new exemptions).

### T4 — Version-keyed attribution registry on the codeimage sequence  (M, depends on: T3)

**Goal.** `asmtest_srcreg_*` stores per-method srcmaps stamped with a capture
sequence and resolves `(addr, when)` to the row live at that time — the same
temporal contract as `asmtest_codeimage_bytes_at`.

**Steps.**

1. Declare in [include/asmtest_trace.h](../../../include/asmtest_trace.h#L151)
   below the srcmap block; implement in [src/trace.c](../../../src/trace.c)
   (malloc-backed, like the file's existing helpers).
2. Mirror the query semantics of
   [include/asmtest_codeimage.h](../../../include/asmtest_codeimage.h)
   `asmtest_codeimage_bytes_at` exactly: candidate = registration whose
   `[code_addr, code_addr+code_size)` contains `addr` with the **largest
   `when` ≤ requested** (`when == 0` → latest); no candidate → `ENOENT`-style
   NULL/0 return.
3. Unit-test, then add the codeimage-composition check.

**Code.**

```c
typedef struct asmtest_srcreg asmtest_srcreg_t;
asmtest_srcreg_t *asmtest_srcreg_new(void);
void asmtest_srcreg_free(asmtest_srcreg_t *reg); /* NULL-safe */
int asmtest_srcreg_add(asmtest_srcreg_t *reg, uint64_t code_addr,
                       uint64_t code_size, uint64_t when, /* codeimage seq */
                       const asmtest_srcmap_entry_t *rows, size_t n,
                       const char *file); /* rows/file are COPIED */
int asmtest_srcreg_resolve(const asmtest_srcreg_t *reg, uint64_t addr,
                           uint64_t when, asmtest_srcmap_entry_t *row_out,
                           uint64_t *method_base_out, const char **file_out);
```

`when` is a plain monotonic u64: producers stamp it from
`asmtest_codeimage_now(img)` when a codeimage is live, or pass their own
sequence (event order) when not. Within a registration, resolution is
`asmtest_srcmap_lookup(map, addr - code_addr)`. Same-address re-JIT is two
registrations differing only in `when` — the exact case
`asmtest_jitdump_find`'s latest-wins rule solves for bytes, made queryable at
any point in time here.

**Tests.** In `examples/test_hwtrace.c`: (a) register the same
`[0x1000,0x1040)` twice (`when` 3 with line rows, `when` 7 with bci rows);
assert `resolve(0x1008, 5)` returns the `when`-3 row, `resolve(0x1008, 0)`
and `(…, 9)` the `when`-7 row, `resolve(0x1008, 2)` fails, and an address
outside any range fails. (b) Composition check (Linux-gated like
`test_jitdump_reader`): create `asmtest_codeimage_new(0)`, `track` a
malloc'd buffer, record `s0 = asmtest_codeimage_now`, `srcreg_add(..., s0,
...)`, mutate the buffer, `refresh`, `srcreg_add(..., asmtest_codeimage_now,
...)`, and assert resolving at `s0` returns the first map — proving the two
sequence spaces compose (self-skips via
`asmtest_codeimage_available()`/`skip_reason` where soft-dirty is absent,
the recorder's own documented gate).

**Docs.** Internal-only; T7 covers the temporal story in one paragraph.

**Done when.**

- `make docker-hwtrace` passes the srcreg unit + composition checks.
- `make docker-hwtrace-codeimage` (the codeimage-focused lane) still passes —
  no recorder behavior changed, only a consumer added.

### T5 — .NET: subscribe `MethodILToNativeMap` (keyword 0x20000) and resolve IL offsets in-proc  (M, depends on: none; richer with T3/T4 landed)

**Goal.** A new `IlToNativeMap : EventListener` in the .NET binding maps a
captured absolute address to `(method, native_off, il_offset)` from CoreCLR's
own JIT events, validated by the dotnet self-test lane.

**Steps.**

1. In [bindings/dotnet/hwtrace/HwTrace.cs](../../../bindings/dotnet/hwtrace/HwTrace.cs#L3873),
   add `public sealed class IlToNativeMap : EventListener` modeled line-for-line
   on `JitMethodMap` (:3873): same provider constant, same
   construct-before-JIT contract (in-proc listeners get **no rundown** — only
   methods JIT'd after enable), same `Freeze()`/binary-search shape, same
   never-throw callback discipline.
2. Enable BOTH keywords at Verbose:
   `EnableEvents(src, EventLevel.Verbose, (EventKeywords)(0x10 | 0x20000));`
   — `MethodILToNativeMap` (event id 190) is Verbose(5) under
   `JittedMethodILToNativeMapKeyword 0x20000`, while `MethodLoadVerbose`
   (id 143) is Informational(4) under 0x10, so one Verbose subscription
   carries both (wire facts in Research notes).
3. `OnEventWritten`: collect two streams keyed by `MethodID`:
   - `MethodLoadVerbose*` → `(MethodID, ReJITID, MethodStartAddress,
     MethodSize, name)` — reuse the existing payload-by-name loop (:4070–4085).
   - `MethodILToNativeMap` → payload `MethodID u64, ReJITID u64, MethodExtent
     u8, CountOfMapEntries u16, ILOffsets u32[], NativeOffsets u32[],
     ClrInstanceID u16`. In-proc array payloads are runtime-inconsistent:
     handle `int[]`/`uint[]`/`IEnumerable` AND the raw `byte[]`-blob form,
     following the documented `GcMoveMap` precedent (:4330–4345). For the
     blob form the two parallel u32 arrays follow the fixed-size prefix
     fields; parse little-endian with `BitConverter`.
4. Join at `Freeze()`, not per-event (callback ordering across threads is not
   guaranteed): for each load record, attach the map with equal
   `(MethodID, ReJITID)`. Record the analysis's verified caveat in the class
   doc-comment: **ReJITID does not distinguish tiers** (Tier0/Tier1/OSR share
   ReJITID 0 at different addresses), so identity is the **code range** —
   which is exactly what the join produces (each load's address range gets
   its own map). The self-test pins `DOTNET_TieredCompilation=0` state via
   the existing harness env, making the join unambiguous there.
5. Expose:
   `public bool TryResolve(ulong addr, out string method, out uint nativeOff, out int ilOffset)`
   — binary-search frozen ranges; `ilOffset` is the entry with the largest
   `NativeOffset <= addr - start` (enclosing-point semantics, matching T3's
   lookup); pseudo-offsets `-1/-2/-3` pass through with the T3 names in
   `ToString()`.

**Code.** ~180 lines of C#; no native-side change (this task's product is the
managed-side map; feeding an `asmtest_srcreg` from C# is follow-on work once
a binding-level attribution surface exists — see Out of scope).

**Tests.** In
[bindings/dotnet/hwtrace/HwTraceProgram.cs](../../../bindings/dotnet/hwtrace/HwTraceProgram.cs#L50),
add a section using the existing `Check()` style: construct `IlToNativeMap`,
JIT a fresh tiny method (the file's `TinyManaged` pattern —
`RuntimeHelpers.PrepareMethod` or a first call), `Freeze()`, then
`TryResolve` the method's start address and a mid-body address. `Check`:
resolution succeeds; `ilOffset >= 0 || ilOffset >= -3`; `nativeOff` for the
mid-body address is > 0; an address outside any JIT'd method fails. Run
`make docker-hwtrace-dotnet` (executes `hwtrace-dotnet-test` →
`dotnet run --project bindings/dotnet/hwtrace/hwtrace.csproj` in the
`asmtest-dotnet` image, [mk/native-trace.mk](../../../mk/native-trace.mk#L2575)).
A failure prints the existing `FAIL:` line with the description; a pass adds
`ok` lines. If the in-proc payload arrives empty on the pinned SDK (the known
EventListener array-payload hazard), the byte[]-blob branch is the fix, not a
skip — a persistent empty payload after both branches is a bug to fix, not
gate (no hardware/credential is involved).

**Docs.** T7's guide subsection gets the .NET paragraph; changelog in T7.

**Done when.**

- `make docker-hwtrace-dotnet` passes with the new IL-map Checks.
- `make docker-hwtrace-dotnet9` (forward-drift lane) still passes or
  self-skips exactly as before this change.

### T6 — JVM: a minimal JVMTI agent for true bytecode-index attribution, plus the `java-bci` lane  (M, depends on: T3, T4)

**Goal.** An in-tree JVMTI agent captures `CompiledMethodLoad`'s address→bci
map (inline-aware where `compile_info` provides it) to a sidecar file, and a
new `hwtrace-jit-java-bci` lane ingests it into `asmtest_srcreg` and proves a
native address resolves to a bci.

**Steps.**

1. New file `examples/jvmti_bci_agent.c` (test-support artifact, like
   [examples/jit_trace.c](../../../examples/jit_trace.c)):
   - `Agent_OnLoad`: `GetEnv(JVMTI_VERSION_1_2)`; `AddCapabilities` with
     `can_generate_compiled_method_load_events` = 1 and (best-effort, ignore
     failure) `can_get_line_numbers`, `can_get_source_file_name`;
     `SetEventCallbacks` + `SetEventNotificationMode(JVMTI_ENABLE,
     JVMTI_EVENT_COMPILED_METHOD_LOAD, NULL)`.
   - `CompiledMethodLoad(jvmti, method, code_size, code_addr, map_length,
     map, compile_info)`: open `/tmp/asmtest-bci-<pid>.map` append; if
     `compile_info != NULL`, walk it as a `jvmtiCompiledMethodLoadRecordHeader`
     chain (`#include <jvmticmlr.h>` — ships in the JDK include dir) looking
     for `kind == JVMTI_CMLR_INLINE_INFO`; for each `PCStackInfo` write the
     **leaf** frame (`methods[0]`/`bcis[0]`). Else fall back to the flat
     `jvmtiAddrLocationMap` (`location` **is** the bci on HotSpot —
     `GetJLocationFormat` == `JVMTI_JLOCATION_JVMBCI`; entry range extends to
     the next entry's start). Emit one text line per point:
     `"%llx %ld %d %s%s\n"` = addr, bci, line (from `GetLineNumberTable`,
     −1 if unavailable), class-sig + method-name (`GetMethodDeclaringClass` +
     `GetClassSignature` + `GetMethodName`; `Deallocate` every JVMTI string).
     The map may be `NULL` (it is optional) — write nothing, never crash.
2. Build rule in [mk/native-trace.mk](../../../mk/native-trace.mk#L2267) next
   to the java lanes: locate the JDK from the toolchain
   (`JAVA_HOME := $(shell dirname $$(dirname $$(readlink -f $$(command -v javac))))`),
   then
   `$(BUILD)/libasmtest_bci_agent.so: examples/jvmti_bci_agent.c` compiled
   with `$(CC) $(CFLAGS) -shared -fPIC -I$(JAVA_HOME)/include
   -I$(JAVA_HOME)/include/linux $< -o $@`. The rule (like `hwtrace-jit-java`)
   is only reachable from lanes that already require `javac`.
3. New harness mode `java-bci` in `examples/jit_trace.c` (mirror
   `trace_jitdump_java` :595): fork `java -agentpath:$(BUILD)/libasmtest_bci_agent.so
   -XX:-TieredCompilation -XX:CompileCommand=dontinline,Hot.asmtjit -cp <cp> Hot`;
   resolve `asmtjit`'s `(addr,size)` from the jcmd perf-map exactly as the
   existing lane does (`java_perfmap_refresh`); poll the sidecar for lines
   whose addr falls in `[addr, addr+size)`; ingest them as
   `asmtest_srcmap_entry_t{off, value=bci, kind=ASMTEST_SRC_BCI}` rows into an
   `asmtest_srcreg_t` (stamp `when` = 1; single version in this controlled
   lane); CHECK ≥1 row, offsets ascending, every `bci >= 0`, and
   `asmtest_srcreg_resolve(reg, addr_of_first_row, 0, ...)` returns it. Print
   the addr→bci(:line) table. Kill/reap the JVM (the sidecar is written live,
   per-event — no clean-shutdown dependency, unlike the perf agent's
   buffered jitdump).
4. Targets: `hwtrace-jit-java-bci` in `mk/native-trace.mk` (deps:
   `$(BUILD)/jit_trace $(BUILD)/libasmtest_bci_agent.so`, plus the
   `javac -d $(BUILD)/jit_java examples/jit_java/Hot.java` compile step the
   sibling lanes use) and `docker-hwtrace-jit-java-bci: docker-java` in
   [mk/docker.mk](../../../mk/docker.mk#L521) mirroring
   `docker-hwtrace-jit-java-jitdump` (:527). Add both to the `.PHONY` lists
   and the docker.mk lane comment block (:425–432 style).
5. Dependency check per CLAUDE.md: the java image's
   `openjdk-25-jdk-headless` ships `$JAVA_HOME/include/jvmti.h` +
   `jvmticmlr.h` (JDK packages carry headers; JRE packages do not). Verify
   in-image with `make docker-build-java` then
   `docker run --rm asmtest-java sh -c 'ls $(dirname $(dirname $(readlink -f $(which javac))))/include/'`.
   If absent, extend `DOCKER_APT_java` in
   [mk/docker.mk](../../../mk/docker.mk#L146) — never self-skip for a
   missing installable header.

**Code.** Agent ≤ ~180 lines of C; harness mode ≤ ~120 lines; two make rules.
The agent is test-support (never shipped, never linked into libasmtest), so
it adds no bindings-parity or packaging obligations.

**Tests.** The `docker-hwtrace-jit-java-bci` lane is the test. Pass:
`ok ... java-bci: N address->bci points inside asmtjit resolve via srcreg` +
the printed table. Failure: `not ok` naming which invariant broke. Host
without a JDK: the host target self-skips with the existing
"`java exited early (JDK not installed?)`"-style reason; the docker lane must
NOT skip.

**Docs.** T7's guide subsection gets the JVM paragraph; changelog in T7.

**Done when.**

- `make docker-hwtrace-jit-java-bci` passes end-to-end (agent built in-image,
  ≥1 bci row inside `asmtjit`, srcreg resolution CHECKed).
- `make docker-hwtrace-jit-java` and `docker-hwtrace-jit-java-jitdump` are
  unchanged-green (no shared-file regressions).

### T7 — User-facing docs and changelog  (S, depends on: T1–T6)

**Goal.** A user can discover the whole attribution surface from the tracing
guide, and the changelog records the feature.

**Steps.**

1. In [docs/guides/tracing/native-tracing.md](../../../docs/guides/tracing/native-tracing.md#L484),
   after the existing jitdump paragraphs (:484–520), add a subsection
   **"From native offsets to source lines, IL offsets, and bytecode
   indexes"** covering: `asmtest_jitdump_debug_find` + the `emu_line_map_t`
   bridge (works today for V8 `--perf-prof` and HotSpot via libperf-jvmti —
   both launch/agent-time feeds); the `asmtest_srcmap`/`asmtest_srcreg`
   schema with the enclosing-point granularity ceiling and the
   codeimage-sequence version key; the .NET `IlToNativeMap` listener
   (construct-before-JIT, no in-proc rundown, ReJITID-does-not-key-tiers
   caveat); the `java-bci` JVMTI lane; and the honest boundary: interpreted
   code (Ignition/CPython/YARV/PUC-Lua) keeps its bytecode index in VM state
   the PC stream cannot see — attribution here covers **JIT-compiled** code
   only. Link the analysis via GitHub blob URL (published pages must not
   relative-link into `docs/internal/`):
   `https://github.com/wilvk/asm-test/blob/main/docs/internal/analysis/il-bytecode-attribution.md`.
2. List the new lane in the guide's lane enumeration (:708–730 region) and in
   the docker.mk/native-trace.mk help comments if any lane list there was not
   already updated in T6.
3. Append one entry under `## [Unreleased]` → `### Added` in
   [CHANGELOG.md](../../../CHANGELOG.md) (single entry for the whole
   feature, matching the file's long-form style: what a native point now
   resolves to, per runtime, and which lanes prove it).
4. Build: `make docker-docs` (Sphinx `-W`: warnings are failures).

**Code.** Docs only.

**Tests.** `make docker-docs` exits 0 (fail-on-warning is the assertion);
`make check-header-portability` runs the `ASM_FUNC` paren-use docs gate — run
it too. Neither gate validates the GitHub blob URL: a bad relative link into
`docs/internal/` fails the Sphinx `-W` build, but the absolute blob URL itself
must be eyeballed.

**Docs.** This IS the docs task.

**Done when.**

- `make docker-docs` green; the new subsection renders in the built guide.
- `CHANGELOG.md` has the entry; `git grep -n "il_offset" docs/guides` finds
  the published description.

## Task order & parallelism

- Three independent start points: **T1** (jitdump reader), **T3** (schema),
  **T5** (.NET listener — pure C#). Three people can start concurrently.
- Ordered chains: T1 → T2; T3 → T4 → T6; everything → T7.
- Critical path: **T3 → T4 → T6 → T7** (schema → registry → JVM bci lane →
  docs). T1/T2 and T5 hang off the sides and can land in any order relative
  to it.

```
T1 ── T2 ──────────────┐
T3 ── T4 ── T6 ────────┼── T7
T5 ────────────────────┘
```

## Constraints & gates

- **No new third-party fetches, no digest changes.** Every dependency is
  already in-tree or in-image: `libperf-jvmti.so` via the pinned-distro
  `linux-tools-generic` apt package and JDK headers via
  `openjdk-25-jdk-headless` (both in `DOCKER_APT_java`,
  [mk/docker.mk](../../../mk/docker.mk#L148)); `dotnet-sdk-8.0` in the dotnet
  image (satisfies every .NET surface used — event 190 and the already-wired
  EnablePerfMap IPC are .NET 8 features). If an expected in-image file is
  missing, extend the image per the CLAUDE.md rule — a missing installable
  dependency is never a self-skip.
  [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt)
  is untouched (nothing new is downloaded).
- **Real gates (record + self-skip, host targets only):** none of this work
  needs hardware. All lanes run under plain `docker run` (ptrace of one's own
  child). The only host-side self-skips are the existing ones: missing
  runtime/JDK/node on a bare host, and `asmtest_codeimage_available() == 0`
  (no soft-dirty/PAGEMAP_SCAN) for T4's composition check — print the
  recorder's own `skip_reason` there. The docker lanes must never skip.
- **Enablement asymmetry is a documented property, not a gate:** V8 debug
  info requires launch-time `--perf-prof`; the HotSpot perf agent flushes
  only on clean shutdown; .NET's IL-map keyword is attach/in-proc-time. State
  these in T7's guide text; do not engineer around them here.
- **Parity gate discipline:** every new symbol in `include/asmtest_ptrace.h`
  (T1's two, T3's `asmtest_srcmap_from_jitdump`) gets an `ALL` exemption with
  a dated comment; `include/asmtest_trace.h` additions need none. Stale
  exemptions fail the gate, so remove each line in the same commit that wraps
  the symbol in a binding.
- **ABI/formatting:** `emu_line_map_t` and every existing export are
  unchanged (compat shims in `asmtest_trace.h` :100–120 stay intact);
  `make fmt` before committing (CI gates on `fmt-check`).

## Research notes (verified 2026-07-17)

- **Jitdump format** (perf spec,
  <https://raw.githubusercontent.com/torvalds/linux/master/tools/perf/Documentation/jitdump-specification.txt>):
  header = magic u32 `0x4A695444` "JiTD" (written `'J','i','T','D'` so readers
  detect endianness), version u32 (=1), total_size u32, elf_mach u32, pad1
  u32, pid u32, timestamp u64, flags u64 (bit 0 =
  `JITDUMP_FLAGS_ARCH_TIMESTAMP`). Record header `{id u32, total_size u32,
  timestamp u64}`; ids: 0 LOAD, 1 MOVE, 2 DEBUG_INFO, 3 CLOSE, 4
  UNWINDING_INFO. DEBUG_INFO body: `code_addr u64, nr_entry u64`, then
  entries `{addr u64, lineno u32 (1-based), discrim u32 (column
  discriminator, 0 default), name char[] NUL (source file)}`. The spec
  REQUIRES DEBUG_INFO be emitted BEFORE the JIT_CODE_LOAD it describes.
  LOAD body: `pid u32, tid u32, vma u64, code_addr u64, code_size u64,
  code_index u64, name NUL, code bytes`. The `jit-<pid>.dump` filename and
  PROT_EXEC-mmap convention come from producers/perf's `jit_detect`, not the
  spec text.
- **perf's reference consumer**
  (<https://raw.githubusercontent.com/torvalds/linux/master/tools/perf/util/jitdump.c>):
  byte-swaps `entries[n].discrim` on cross-endian (line 324); `perf inject
  --jit` emits one ELF per function named `jitted-%d-%<code_index>.so`
  (lines 461/591;
  <https://raw.githubusercontent.com/torvalds/linux/master/tools/perf/Documentation/perf-inject.txt>).
- **.NET events**
  (<https://learn.microsoft.com/en-us/dotnet/fundamentals/diagnostics/runtime-method-events>):
  provider `Microsoft-Windows-DotNETRuntime`; `MethodLoad_V1/V2` = 141,
  `MethodLoadVerbose_V1/V2` = 143 (V2 adds `ReJITID u64`; payload MethodID
  u64, ModuleID u64, MethodStartAddress u64, MethodSize u32, MethodToken u32,
  MethodFlags u32, three unicode strings, ReJITID u64, ClrInstanceID u16);
  `MethodILToNativeMap` = **190**, keyword
  `JittedMethodILToNativeMapKeyword 0x20000`, level Verbose(5); payload
  MethodID u64, ReJITID u64, MethodExtent u8, CountOfMapEntries u16,
  ILOffsets u32[], NativeOffsets u32[], ClrInstanceID u16. Correction to the
  analysis-era premise: **143 is MethodLoadVerbose on BOTH Framework and
  Core** — the Framework-vs-Core id split (136 vs 141,
  <https://learn.microsoft.com/en-us/dotnet/framework/performance/method-etw-events>)
  applies to non-verbose `MethodLoad`, and the Framework doc's non-verbose
  ids are internally inconsistent (doc-sourced only).
- **.NET perf-map/jitdump control**: `DiagnosticsClient.EnablePerfMap
  (PerfMapType)` ships in .NET 8+ (IPC commands EnablePerfMap 0x0405 /
  DisablePerfMap 0x0406; PerfMapType DISABLED=0, ALL=1, JITDUMP=2,
  PERFMAP=3;
  <https://github.com/dotnet/diagnostics/blob/main/documentation/design-docs/ipc-protocol.md>,
  <https://github.com/dotnet/diagnostics/blob/main/src/Microsoft.Diagnostics.NETCore.Client/DiagnosticsClient/DiagnosticsClient.cs>,
  <https://learn.microsoft.com/en-us/dotnet/core/diagnostics/microsoft-diagnostics-netcore-client>);
  env equivalent `DOTNET_PerfMapEnabled` 0/1/2/3, jitdump to
  `/tmp/jit-<pid>.dump`
  (<https://learn.microsoft.com/en-us/dotnet/core/runtime-config/debugging-profiling>).
  Already implemented in-tree as `DiagnosticsIpc` (HwTrace.cs :4364). The
  repo's dotnet lanes pin `dotnet-sdk-8.0` — satisfies all of the above.
- **JVMTI**: `CompiledMethodLoad(method, code_size, code_addr, map_length,
  jvmtiAddrLocationMap* map {start_address void*, location jlocation; range
  extends to the next entry's start_address-1}, compile_info void*)` — from
  OpenJDK's jvmti.xml (the spec source of truth;
  <https://raw.githubusercontent.com/openjdk/jdk/master/src/hotspot/share/prims/jvmti.xml>;
  Oracle's rendered page truncated on fetch). `jvmticmlr.h`
  (<https://raw.githubusercontent.com/openjdk/jdk/master/src/java.base/share/native/include/jvmticmlr.h>):
  kinds `JVMTI_CMLR_DUMMY=1`, `JVMTI_CMLR_INLINE_INFO=2`; header `{kind,
  majorinfoversion, minorinfoversion, next}`;
  `jvmtiCompiledMethodLoadInlineRecord {header, numpcs jint, pcinfo
  PCStackInfo*}` with `PCStackInfo {pc, numstackframes, methods[], bcis[]}` —
  `methods[0]/bcis[0]` is the leaf. The kernel's agent
  (<https://raw.githubusercontent.com/torvalds/linux/master/tools/perf/jvmti/libjvmti.c>)
  registers COMPILED_METHOD_LOAD + DYNAMIC_CODE_GENERATED, requires
  `can_generate_compiled_method_load_events`, optionally
  `can_get_line_numbers` + `can_get_source_file_name`, and is built/installed
  as `libperf-jvmti.so` by perf's Makefile
  (<https://raw.githubusercontent.com/torvalds/linux/master/tools/perf/Makefile.perf>)
  — the Ubuntu `linux-tools-generic` artifact the java image already
  installs.
- **V8**: `--perf-prof` writes the jitdump ("enable experimental annotate
  support for perf profiler"), `--perf-prof-path` sets its directory,
  `--perf-prof-annotate-wasm` adds wasm source-map annotation,
  `--perf-basic-prof` writes the text perf-map
  (<https://raw.githubusercontent.com/v8/v8/main/src/flags/flag-definitions.h>
  L4122–4156). The writer
  (<https://raw.githubusercontent.com/v8/v8/main/src/diagnostics/perf-jit.cc>)
  uses magic `0x4A695444`, version 1, events kLoad=0..kUnwindingInfo=4, and
  puts the **column in the spec's discrim slot**; enablement is launch-only
  (<https://v8.dev/docs/linux-perf>). Flag text is from V8 main (2026-07);
  the repo pins no Node/V8 version, so T2 asserts structure (in-range,
  ordered), not exact line values.
- **Repo alignment**: no .NET/JDK/Node pins in
  `scripts/third-party-digests.txt` (only dynamorio 11.91.20630, keystone
  0.9.2, capstone 5.0.1, zig 0.13.0) — consistent with "no new fetches" above.

## Out of scope

- **Whole-window managed composition** (labelling a whole-window .NET trace,
  PT tiers, tiering-knob control): [managed-wholewindow-compose.md](managed-wholewindow-compose.md),
  which explicitly delegates "IL↔native / bytecode attribution
  (MethodILToNativeMap consumers)" to this doc.
- **The PT capture substrate and foreign-pid PT attach**:
  [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md),
  [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md).
- **Data-flow value capture and taint** (the L0/L1/L2 sinks this attribution
  will eventually label): [dataflow-producer-correctness.md](dataflow-producer-correctness.md),
  [dataflow-pt-replay-tier.md](dataflow-pt-replay-tier.md),
  [pin-libdft-taint-oracle.md](pin-libdft-taint-oracle.md).
- **GC object identity for memory def-use** (GCBulkType/Node/Edge):
  [dataflow-f4-object-identity.md](dataflow-f4-object-identity.md).
- **Ptrace/blockstep stepper correctness fixes** (si_code handling etc.):
  [ptrace-blockstep-tracer-correctness.md](ptrace-blockstep-tracer-correctness.md).
- **Interpreter-state probes** (CPython `instr_ptr`, Ignition frame-slot,
  YARV `cfp->pc` reads): genuinely open follow-on work, deliberately NOT
  tasked here — the analysis shows it needs a state probe, a different
  mechanism from everything above; no sibling doc owns it yet.
- **Per-binding attribution surfaces** (exposing srcmap/srcreg through the
  ten language bindings, and feeding `asmtest_srcreg` from the .NET
  listener): follow-on once this C surface stabilizes; the parity-allowlist
  entries added in T1/T3 are the tracked debt.
