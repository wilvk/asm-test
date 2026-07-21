# Data-flow bindings: def-use/slice surface and code-image argument across the language wrappers — implementation

> **Sources.** Actioned from
> [live-attach-dataflow-followup-plan.md](../archive/plans/live-attach-dataflow-followup-plan.md)
> (§F7, the two carryovers named at
> [its close](../archive/plans/live-attach-dataflow-followup-plan.md): "the def-use/slice
> surface for the seven … and `attach_pid_versioned`'s code-image argument, which
> every binding passes as NULL"). Written 2026-07-17. If this doc and a source
> disagree, this doc wins (sources may be stale); if the CODE and this doc
> disagree, re-verify before implementing.

## Why this work exists

F7 landed live-attach data-flow capture in all ten language bindings, but it
landed **unevenly**. Three bindings (Python, C++, Node) expose the whole
`valtrace → def-use → slice` pipeline; the other **seven** (Ruby, Lua, Zig, Rust,
Go, Java, .NET) stop at the producer — they can capture a value trace off a live
process, count its steps, but they cannot ask the two questions the tier exists to
answer: *what produced this value* (backward slice) and *what does it influence*
(forward slice). The reason is a single FFI wart: the slice seed crosses the
boundary **by value** as a 72-byte `at_val_rec_t`, and Ruby's Fiddle (and friends)
have no way to express a by-value aggregate argument. This doc removes that wart
with a by-pointer seed entry point, then wraps the def-use + slice surface in the
seven.

The second gap: **every** binding passes `NULL` for the versioned-decode
code-image when it drives `attach_jit`. That is correct for a region whose bytes
never change, but a live JIT that re-compiles, frees, or reuses a code address
mid-capture then decodes operands from *whatever bytes happen to be live at read
time*, not the bytes that were live when the trace ran. This doc wraps the
code-image recorder (`asmtest_codeimage.h`) into the bindings so an FFI caller can
build a real `img`/`when` and get time-correct operand decode.

## What already exists (verified 2026-07-17)

The C substrate is complete; this is a bindings-only lift plus one small
lib-side convenience entry point.

- **The pure analysis pipeline** is in
  [src/dataflow.c](../../../src/dataflow.c): `asmtest_defuse_build` /
  `asmtest_defuse_free` (L1 last-writer graph,
  [asmtest_valtrace.h:190-191](../../../include/asmtest_valtrace.h#L190)), and the
  L2 slicers `asmtest_slice_forward` / `asmtest_slice_backward` /
  `asmtest_slice_free` / `asmtest_slice_contains`
  ([asmtest_valtrace.h:206-216](../../../include/asmtest_valtrace.h#L206)). Both
  slicers take their seed **by value** as `at_val_rec_t`
  ([asmtest_valtrace.h:206-212](../../../include/asmtest_valtrace.h#L206)) but read
  **only** `seed.step` — [src/dataflow.c:356-364](../../../src/dataflow.c#L356)
  forwards straight to `slice_dir(g, seed.step, dir)`. That one fact is what makes
  a by-pointer variant trivial and lossless.
- **`at_val_rec_t`** ([asmtest_valtrace.h:61-86](../../../include/asmtest_valtrace.h#L61))
  is 72 bytes, 8-byte aligned. Field offsets (compiled and verified; identical on
  Linux x86-64): `kind` 0, `reg` 4, `base` 8, `index` 12, `scale` 16, pad 20-23,
  `disp` 24, `addr` 32, `size` 40, `is_write`/`value_valid`/`wide` at 42/43/44, pad
  45-47, `wide_off` 48, pad 52-55, `value` 56, `step` 64, tail pad 68-71. At 9
  eightbytes it is SysV MEMORY-class — always a stack copy — which is why an FFI
  that cannot express aggregates at all cannot call the by-value slicers.
- **The live-attach producer** [src/dataflow_ptrace.c](../../../src/dataflow_ptrace.c)
  exposes `asmtest_dataflow_ptrace_attach_pid` (leader),
  `_pid_tid` (worker-targeting), `_pid_versioned`
  ([:1319](../../../src/dataflow_ptrace.c#L1319), the `img`/`when` path), and
  `_jit` ([:1909](../../../src/dataflow_ptrace.c#L1909), worker + `img`/`when` +
  survival report). `_jit` already threads `img`/`when` down to the operand
  enumerator via [dfp_attach_worker:1787](../../../src/dataflow_ptrace.c#L1787); a
  binding passing a non-NULL `img` gets versioned decode with **no C change**.
- **The code-image recorder** [asmtest_codeimage.h](../../../include/asmtest_codeimage.h)
  ships `asmtest_codeimage_new` / `_track` / `_refresh` / `_now` / `_bytes_at` /
  `_free` (and the optional eBPF detector). Its object `pic/codeimage.o` is already
  linked into `libasmtest_dataflow` —
  [mk/dataflow.mk:119](../../../mk/dataflow.mk#L119) lists it in
  `DATAFLOW_SHLIB_OBJS` — so **every `asmtest_codeimage_*` symbol is already
  exported from the lib the seven bindings dlopen.** No Makefile change is needed
  to reach them.
- **The Python reference** for both halves: the slice surface at
  [bindings/python/asmtest/dataflow.py:361-390](../../../bindings/python/asmtest/dataflow.py#L361)
  (`_defuse` / `_slice` / `forward_slice` / `backward_slice`, marshalling a zeroed
  `_ValRec` with only `.step` set — [:368-382](../../../bindings/python/asmtest/dataflow.py#L368));
  and the code-image recorder already wrapped for the **hwtrace** binding as the
  `CodeImage` class at
  [bindings/python/asmtest/hwtrace.py:1547-1642](../../../bindings/python/asmtest/hwtrace.py#L1547).
  The `attach_jit` NULL-passing that this doc lifts is documented in place at
  [dataflow.py:329-341](../../../bindings/python/asmtest/dataflow.py#L329),
  [node/dataflow.js:288-302](../../../bindings/node/dataflow.js#L288), and
  [ruby/dataflow.rb:151-161](../../../bindings/ruby/dataflow.rb#L151).
- **The shared live victim** [bindings/dataflow_victim.c](../../../bindings/dataflow_victim.c)
  runs the `df_chain` fixture (`rax = rdi + rsi` through a store/load pair): six
  in-region instructions with a **real memory def-use edge step 1 → step 2** and a
  register chain, ret at step 5. The native suite asserts exactly this shape —
  `has_edge(g,1,2)`, `forward(step0) = backward(step4) = {0,1,2,3,4}`, excludes ret
  — at [examples/test_dataflow_ptrace.c:259-275](../../../examples/test_dataflow_ptrace.c#L259).
  That is the anti-vacuity target the seven bindings will replicate.
- **The docker lanes** are `make docker-dataflow-<lang>`
  ([mk/dataflow.mk:262-272](../../../mk/dataflow.mk#L262)); the host-side lanes are
  `make dataflow-<lang>-test` ([mk/dataflow.mk:178-225](../../../mk/dataflow.mk#L178)).
  Each pins its toolchain via `bindings/Dockerfile.lang` + the
  `DOCKER_APT_<lang>` / `DOCKER_SETUP_<lang>` knobs in
  [mk/docker.mk:140-175](../../../mk/docker.mk#L140).

**Prove the baseline green before touching anything.** From the repo root:

```
make shared-dataflow            # builds build/libasmtest_dataflow.so (or .dylib)
make dataflow-python-test       # reference lane: prints "ok N - ..." TAP lines, 0 failures
make docker-dataflow-ruby       # a docker-gated seven-lane: 36/36 ok, 0 skips, 0 failures
```

`make docker-dataflow-ruby` builds `asmtest-dataflow-ruby` from
`ubuntu:24.04` and runs `make dataflow-ruby-test` inside it; a green run ends with
36 `ok` lines. If any lane is red before you start, stop and fix the baseline — do
not build on top of it.

## Tasks

### T1 — Add a by-pointer slice-seed entry point to the lib  (S, depends on: none)

**Goal.** `libasmtest_dataflow` exports `asmtest_slice_forward_seed` and
`asmtest_slice_backward_seed` taking `const at_val_rec_t *`, byte-for-byte
equivalent to the by-value slicers, so an FFI that cannot pass a 72-byte aggregate
by value can still slice.

**Steps.**
1. In [include/asmtest_valtrace.h](../../../include/asmtest_valtrace.h), directly
   below the existing by-value declarations at
   [:206-212](../../../include/asmtest_valtrace.h#L206), add two prototypes:
   ```c
   /* By-pointer seed variants of asmtest_slice_forward / _backward. Only seed->step
    * is read (as by-value today), but a pointer argument crosses every FFI — the
    * by-value at_val_rec_t is SysV MEMORY-class, which Ruby Fiddle and other dynamic
    * FFIs cannot express as a value argument. A NULL seed is treated as step 0. */
   asmtest_slice_t *asmtest_slice_forward_seed(const asmtest_defuse_t *g,
                                               const at_val_rec_t *seed);
   asmtest_slice_t *asmtest_slice_backward_seed(const asmtest_defuse_t *g,
                                                const at_val_rec_t *seed);
   ```
2. In [src/dataflow.c](../../../src/dataflow.c), directly below
   `asmtest_slice_backward` at [:361-364](../../../src/dataflow.c#L361), add the
   two definitions. They forward to the same internal `slice_dir`:
   ```c
   asmtest_slice_t *asmtest_slice_forward_seed(const asmtest_defuse_t *g,
                                               const at_val_rec_t *seed) {
       return slice_dir(g, seed ? seed->step : 0, true);
   }
   asmtest_slice_t *asmtest_slice_backward_seed(const asmtest_defuse_t *g,
                                                const at_val_rec_t *seed) {
       return slice_dir(g, seed ? seed->step : 0, false);
   }
   ```
   Do **not** change the existing by-value functions — Python/C++/Node call them
   ([dataflow.py:77-80](../../../bindings/python/asmtest/dataflow.py#L77),
   [node/dataflow.js:90-91](../../../bindings/node/dataflow.js#L90),
   [cpp/asmtest_dataflow.hpp:226](../../../bindings/cpp/asmtest_dataflow.hpp#L226))
   and those FFIs handle by-value aggregates fine. This is purely additive.
3. `make shared-dataflow` — the new symbols land in the lib automatically because
   `src/dataflow.c` is already compiled to `pic/dataflow.o`
   ([mk/dataflow.mk:95](../../../mk/dataflow.mk#L95)); no Makefile edit.

**Code.** Two ~3-line functions and two prototypes. No new object, no new
dependency, no ABI change to any existing symbol.

**Tests.** Extend the pure C suite
[examples/test_dataflow.c](../../../examples/test_dataflow.c) (built by
`build/test_dataflow`, run first in `make dataflow-test`) with a parity assertion:
build a def-use graph over a small hand-built trace, then assert that for every
step index `k`, `asmtest_slice_forward_seed(g, &rec_with_step_k)` returns a slice
element-for-element equal to `asmtest_slice_forward(g, rec_with_step_k)`. That file
has no `slices_equal` helper — its existing slice tests (`test_defuse_slice` at
[test_dataflow.c:144](../../../examples/test_dataflow.c#L144)) compare the `->n`
count and probe `asmtest_slice_contains()` directly, so assert the same way here:
equal `->n`, then `asmtest_slice_contains()` agrees for both slices over every step
index `0..g->nsteps` (and the same for backward). A pass prints the new `ok` line;
a failure (e.g. if someone later reads a second seed field) reddens it. Verify:
```
make dataflow-test              # runs build/test_dataflow among others; all ok
```

**Docs.** Internal-only — no user-facing docs. This is a lib-internal convenience
entry point that only the bindings call; note it in the `CHANGELOG.md`
`## [Unreleased]` `Added` entry the later tasks write (one line: "by-pointer slice
seed entry points").

**Done when.**
- `nm -D build/libasmtest_dataflow.so | grep slice_.*_seed` shows both symbols
  (on Linux; `nm -gU build/libasmtest_dataflow.dylib` on macOS).
- `make dataflow-test` is green including the new parity assertion.
- `make dataflow-python-test` / `make dataflow-node-test` still pass (the by-value
  path is untouched).

### T2 — Wrap the def-use + slice surface in the seven bindings  (M, depends on: T1)

**Goal.** Ruby, Lua, Zig, Rust, Go, Java, and .NET each expose `ValueTrace`
methods to build the L1 def-use graph and take forward/backward slices, using
T1's by-pointer seed, so `ValueTrace` means the same thing in all ten bindings.

**Steps (per binding — mirror the Python `_defuse` / `_slice` shape at
[dataflow.py:361-390](../../../bindings/python/asmtest/dataflow.py#L361)).** Each
binding already declares `valtrace_new` / `_free` / `_steps` / `_recs` and the
three attach entry points; add declarations for `asmtest_valtrace_append`,
`asmtest_defuse_build`, `asmtest_defuse_free`, `asmtest_slice_forward_seed`,
`asmtest_slice_backward_seed`, `asmtest_slice_free`, `asmtest_slice_contains`, then
add `defuse()` (cached, invalidated on any new step/attach), `forward_slice(step)`,
and `backward_slice(step)` methods that return the set of reached step indices by
probing `slice_contains` over `0..steps`.

The **only** struct that must be marshalled is `at_val_rec_t`, and it crosses the
boundary **by pointer** in both places it appears: `asmtest_valtrace_append` takes
`const at_val_rec_t *recs` (an array —
[asmtest_valtrace.h:129](../../../include/asmtest_valtrace.h#L129)) and the seed is
now a pointer (T1). No aggregate is ever passed by value, so this is expressible in
every one of the seven FFIs. The per-language marshalling of the 72-byte record —
using the verified offsets from *What already exists* — is:

- **Ruby (Fiddle 1.1.1, apt `ruby` = ruby3.2 on `ubuntu:24.04`).** Fiddle has no
  by-value struct type and its `Importer` structs are pointer overlays — the exact
  reason the by-value slicer was unwrappable. With T1, build records and the seed
  as raw C-heap buffers via the existing `cbuf` helper
  ([dataflow.rb:38-42](../../../bindings/ruby/dataflow.rb#L38)) and `pack` the
  fields at their verified offsets. A single explicit template packs one 72-byte
  record: kind(`l<`,0), reg/base/index(`L<`×3, 4/8/12), scale(`l<`,16), pad(`x4`),
  disp(`q<`,24), addr(`q<`,32), size(`S<`,40), three bools(`C`×3, 42), pad(`x3`),
  wide_off(`L<`,48), pad(`x4`), value(`q<`,56), step(`L<`,64), pad(`x4`). The seed
  buffer is 72 zero bytes with `step` written at offset 64
  (`seed[64,4] = [k].pack("L<")`). Slice functions declared `[VOIDP, VOIDP], VOIDP`.
  `_Bool` is one byte, truth = 1 (SysV) — pack `is_write` as `1`/`0`.
- **Lua (LuaJIT 2.1, apt `luajit`).** LuaJIT's `ffi` already gives typed struct
  arrays (the binding uses it for `asmtest_gcmove_t` at
  [dataflow.lua:11-13](../../../bindings/lua/dataflow.lua#L11)). Add
  `at_val_rec_t`, `asmtest_defuse_t`/`asmtest_slice_t` as opaque, and the seven
  prototypes to the `ffi.cdef` block. Build records with `ffi.new("at_val_rec_t[?]", n)`
  and set fields by name; the seed is one `ffi.new("at_val_rec_t")` with `.step`
  set, passed by pointer (`&seed` via `ffi.new("at_val_rec_t[1]")`). Note LuaJIT
  **can** pass structs by value but never JIT-compiles such calls and its FFI is
  mandatory here (plain PUC Lua has no FFI); the by-pointer seed keeps the call on
  the compiled path and keeps the binding uniform with the others.
- **Zig (0.13.0, sha256-pinned tarball).** `extern struct` has C-ABI layout
  (already used for `GcMove`/`Method` at
  [test_dataflow.zig:9-10](../../../bindings/zig/src/test_dataflow.zig#L9)). Declare
  `const ValRec = extern struct { ... }` with `bool` fields (1-byte,
  C-compatible), an opaque `?*anyopaque` for the graph/slice handles, and the seven
  function-pointer types. Pass records as `[*]const ValRec`, the seed as
  `*const ValRec`.
- **Rust (rustc 1.75, apt `rustc`).** `#[repr(C)] struct ValRec` (as `GcMove` at
  [test_dataflow.rs:9-15](../../../bindings/rust/test_dataflow.rs#L9)); `bool` is
  size/align 1 with bit patterns `0x00`/`0x01`, C-compatible. Add the `extern "C"`
  block for the seven; seed passed `&ValRec`.
- **Go (cgo, apt `golang-go`).** The binding is cgo-only (no purego —
  [go.mod](../../../bindings/go/go.mod) has none). In the cgo preamble
  ([main.go:10-79](../../../bindings/go/cmd/dataflowsmoke/main.go#L10)) add a
  `df_valrec_t` struct and `dlsym`-loaded function pointers for the seven, plus
  thin `static` wrappers (the file's pattern — `df_attach_pid` etc.). Records built
  as `[]C.df_valrec_t`, seed as `&rec`. Keep to **line comments only** inside the
  preamble (the file's own warning at
  [main.go:35-39](../../../bindings/go/cmd/dataflowsmoke/main.go#L35)).
- **Java (FFM, final since JDK 22; apt `openjdk-25-jdk-headless`).** This is where
  the by-pointer seed pays off most: the by-value slicer would have needed a
  `StructLayout` with **explicit `paddingLayout` members** at 20-23, 45-47, 52-55,
  68-71 and canonical member layouts. The pointer form needs **none of that** — the
  seed is a `MemorySegment` of 72 bytes with `JAVA_INT` written at offset 64, and
  the `FunctionDescriptor` is `(ADDRESS, ADDRESS) -> ADDRESS`. Records for
  `append` are a native segment array; set fields with `VarHandle`s or raw
  `set(JAVA_INT, offset, ...)` at the verified offsets. Follow the existing FFM
  `Linker`/`SymbolLookup` setup in
  [TestDataflow.java:164-177](../../../bindings/java/TestDataflow.java#L164) (the
  `Linker.nativeLinker()`/`SymbolLookup.libraryLookup()` pair, then the
  `downcallHandle` calls).
- **.NET (P/Invoke; apt `dotnet-sdk-8.0`).** The binding uses `DllImport`
  ([Program.cs:31-69](../../../bindings/dotnet/dataflow_smoke/Program.cs#L31)). The
  by-pointer seed sidesteps C#'s non-blittable-`bool` problem entirely: no
  `[DisableRuntimeMarshalling]`, no `byte`-for-`bool` remapping. Declare the seed
  and record buffers as `IntPtr` into `Marshal.AllocHGlobal(72)` (write `step` at
  offset 64 via `Marshal.WriteInt32`), and the slice functions as
  `IntPtr asmtest_slice_forward_seed(IntPtr g, IntPtr seed)`. If a binding author
  prefers a typed `[StructLayout(LayoutKind.Sequential)]` record for `append`,
  declare the three `bool` fields as `byte` (blittable) rather than `System.Boolean`.

**Code.** Roughly 40-70 lines per binding: the seven new symbol declarations, the
72-byte record marshalling, and three `ValueTrace` methods. No new files.

**Tests.** Covered in T3 (kept separate so the wrapper and its assertions land as
reviewable units).

**Docs.** Update each binding's comment that currently says the slice surface "is
not wrapped … the slice seed crosses BY VALUE as an `at_val_rec_t`, which Fiddle
has no type for" — e.g.
[ruby/dataflow.rb:122-127](../../../bindings/ruby/dataflow.rb#L122) and
[lua/dataflow.lua:108-111](../../../bindings/lua/dataflow.lua#L108) — to state that
the def-use/slice surface is now wrapped via the by-pointer seed. `CHANGELOG.md`
entry is written in T3.

**Done when.**
- Each binding source declares the seven symbols and compiles/loads cleanly
  (`make dataflow-<lang>-test` gets past load; assertions arrive in T3).
- No binding declares a by-value `at_val_rec_t` argument anywhere (grep each
  binding: the record and seed appear only behind a pointer/array).

### T3 — Slice round-trip + live def-use assertions for the seven; update lane counts  (S, depends on: T2)

**Goal.** Each of the seven `test_dataflow.<lang>` suites asserts the L1/L2 surface
against both a hand-built trace and the live `df_chain` capture, so an empty or
mis-marshalled slice reddens the lane.

**Steps.**
1. **Hand-built chain** (mirrors the Python round-trip at
   [test_dataflow.py:113-114](../../../bindings/python/tests/test_dataflow.py#L113)):
   build a `ValueTrace`, `step` a register move chain `r10 → r11 → r12`
   (`step0` writes r10; `step1` reads r10 writes r11; `step2` reads r11 writes
   r12), then assert `forward_slice(0) == {0,1,2}` and `backward_slice(2) == {0,1,2}`.
   This exercises `append` (record marshalling) and both slicers independently of
   ptrace, so it runs even where the live attach self-skips.
2. **Live def-use edge** (the anti-vacuity assertion, mirrors
   [test_dataflow_ptrace.c:259-275](../../../examples/test_dataflow_ptrace.c#L259)):
   after the existing live `attach_pid` capture of the victim's `df_chain` (six
   steps, already asserted in each seven-lane suite — e.g.
   [test_dataflow.zig:204-207](../../../bindings/zig/src/test_dataflow.zig#L204)),
   build the def-use graph and assert `backward_slice(4)` **contains step 1**
   (the store) reached from step 4 through the load at step 2 — i.e. the
   **memory** def-use edge the seven could never test before — and that the slice
   **excludes** step 5 (the `ret`). Because the region is the same `df_chain`
   bytes as the native oracle, the expected slice `{0,1,2,3,4}` is known exactly.
3. Bump each lane's expected assertion count. The lanes today are python 17, cpp 48,
   node 44, ruby 36, lua 36, zig 36, rust 36, go 36, java 36, dotnet 36
   ([plan §F7 landing note](../archive/plans/live-attach-dataflow-followup-plan.md)). The
   suites self-count (`n`/`_n` incremented per `check`) and print `1..N`, so no
   hard-coded total needs editing — but any lane that prints a summary line
   ("36/36") must be updated to the new count, and the docker lane's green
   condition (`0 skips, 0 failures`) is unchanged.

**Code.** ~15-25 lines of test per binding, added to the existing
`test_dataflow.<lang>` file. No new test files, no new make targets — the assertions
ride the existing `dataflow-<lang>-test` / `docker-dataflow-<lang>` lanes.

**Tests.** The tasks *are* the tests. A pass adds `ok` lines and raises `1..N`; a
failure — an empty slice from a mis-packed record, or a missing memory edge from a
wrong `addr`/`is_write` offset — prints `not ok` and exits non-zero, reddening the
lane. On a host without ptrace the **hand-built** assertions still run (they need
no live attach); the **live** assertions self-skip exactly as the existing live
tests do (ETRACE is a hard failure, not a skip — see the lane note at
[mk/dataflow.mk:247-257](../../../mk/dataflow.mk#L247)).

**Docs.** Add one `CHANGELOG.md` `## [Unreleased]` `Added` bullet: "Def-use graph
and forward/backward slice surface in the Ruby, Lua, Zig, Rust, Go, Java and .NET
data-flow bindings (previously producer-only), via a by-pointer slice-seed entry
point." The user-facing binding pages under
[docs/bindings/](../../bindings/) do **not** currently document the data-flow tier
at all (verified: no `dataflow`/`ValueTrace` mention in any of the ten pages), so
there is no existing page section to correct — adding one is out of scope for this
task and tracked as future binding-doc work.

**Done when.**
- `make docker-dataflow-ruby` (and each other docker-gated lane) passes with the
  new higher `1..N`, 0 skips, 0 failures.
- `make dataflow-zig-test` on a host with ptrace shows the new `backward_slice`
  memory-edge assertion as `ok`.
- Flip one record's `is_write` offset in a scratch build and confirm the live
  memory-edge assertion goes `not ok` — proving the marshalling, not a constant,
  is under test.

### T4 — Wrap the code-image recorder and thread a real img/when through attach_jit  (M, depends on: none)

**Goal.** Every binding can build an `asmtest_codeimage_t`, track a region, and
pass its handle + a `when` sequence into `attach_jit` (and expose
`attach_pid_versioned`), so a live JIT's re-compiled region decodes from the bytes
that were live at trace time instead of NULL/live-snapshot bytes.

**Steps.**
1. **Wrap the recorder** in each binding's `dataflow` module, mirroring the Python
   `CodeImage` class already written for the hwtrace binding at
   [hwtrace.py:1547-1642](../../../bindings/python/asmtest/hwtrace.py#L1547).
   Declare and expose `asmtest_codeimage_new(pid)` → handle,
   `asmtest_codeimage_track(img, base, len)`, `asmtest_codeimage_refresh(img)`,
   `asmtest_codeimage_now(img)` → u64,
   `asmtest_codeimage_bytes_at(img, addr, when, &out, &out_len)`, and
   `asmtest_codeimage_free(img)`. These symbols are **already exported** from
   `libasmtest_dataflow` (T-note: `pic/codeimage.o` is in `DATAFLOW_SHLIB_OBJS`,
   [mk/dataflow.mk:119](../../../mk/dataflow.mk#L119)) — the binding only needs to
   declare them; no lib or Makefile change. Status codes to re-declare:
   `ASMTEST_CI_OK` 0, `ASMTEST_CI_ENOENT` -7
   ([asmtest_codeimage.h:54-64](../../../include/asmtest_codeimage.h#L54)). The
   eBPF detector (`_watch_bpf` / `_poll_bpf` / `_next`) is **out of scope** here —
   it is a separate optional surface that self-skips without libbpf/CAP_BPF; wrap
   only the always-available soft-dirty recorder.
2. **Expose `attach_pid_versioned`.** The producer's
   `asmtest_dataflow_ptrace_attach_pid_versioned(pid, base, len, max_insns, img,
   when, result, vt)` ([dataflow_ptrace.c:1319](../../../src/dataflow_ptrace.c#L1319))
   is wrapped by **no** binding today (only `_pid`/`_pid_tid`/`_jit` are). Add it
   as a `ValueTrace.attach_pid_versioned(pid, base, code_len, img, when,
   max_insns=0)` method taking the recorder handle. `img == NULL`/`0` degrades to
   exactly `attach_pid` behaviour ([:1394-1399](../../../src/dataflow_ptrace.c#L1394)).
3. **Thread img into `attach_jit`.** Each binding's `attach_jit` currently hard-codes
   `NULL`/`null`/`nil` for the `img` argument — e.g.
   [ruby/dataflow.rb:158](../../../bindings/ruby/dataflow.rb#L158),
   [lua/dataflow.lua:146](../../../bindings/lua/dataflow.lua#L146),
   [node/dataflow.js:297](../../../bindings/node/dataflow.js#L297),
   [python/dataflow.py:345](../../../bindings/python/asmtest/dataflow.py#L345). Add
   an optional `img` parameter (a recorder handle, default NULL) so the caller can
   pass a real code-image; keep the existing NULL default so present callers are
   unaffected.
4. `make dataflow-python-test` then each `make docker-dataflow-<lang>` to rebuild
   and re-run.

**Code.** ~30-50 lines per binding: the six recorder declarations + a small handle
wrapper class/table, the `attach_pid_versioned` method, and the new `img` parameter
on `attach_jit`. No C change, no Makefile change.

**Tests.** Add to each `test_dataflow.<lang>` suite a code-image assertion that
runs wherever soft-dirty is available (gate on `asmtest_codeimage_available()` —
the recorder self-reports; on a host/container without soft-dirty it prints a
`# SKIP` line, which is a legitimate host gate, not a self-skip of the feature):
1. **Recorder works:** `img = CodeImage(0)` (this process), `track(base, len)` over
   a small mapped `df_chain`-shaped buffer, assert `now() > 0` and that
   `bytes_at(base, now())` returns the exact tracked bytes — proving the wrapper
   marshals the `&out`/`&out_len` out-pointers correctly, not that a symbol
   resolved.
2. **img threads through the capture:** spawn the shared victim, build a code-image
   over *its* published `base`/`len`, and call the new
   `attach_pid_versioned(pid, base, len, img, when=now())` (or `attach_jit` with
   `img`). Assert the region still returns `a+b` and captures six steps — i.e. a
   non-NULL `img` does not break the capture and lands in the right argument slot
   (a dropped/misplaced pointer would corrupt `base`/`pid` and the result assert
   would catch it). The deep "decode follows the version across an in-place re-JIT"
   proof is **already** covered by the native C suite `test_versioned` at
   [test_dataflow_ptrace.c:1737-1914](../../../examples/test_dataflow_ptrace.c#L1737)
   (it patches the region and checks step 0 decodes the original register); the
   binding test's job is to prove the FFI plumbing carries `img`/`when`, not to
   re-prove the C decode semantics in seven languages.

**Docs.** Update the in-code comments that state "the versioned-decode code-image
… is passed NULL … Wrapping the code-image recorder is a separate binding surface"
(e.g. [python/dataflow.py:336-341](../../../bindings/python/asmtest/dataflow.py#L336),
[node/dataflow.js:291-292](../../../bindings/node/dataflow.js#L291),
[ruby/dataflow.rb:153-154](../../../bindings/ruby/dataflow.rb#L153)) to state the
recorder is now wrappable and `attach_jit`/`attach_pid_versioned` accept a real
`img`. Add a `CHANGELOG.md` `## [Unreleased]` `Added` bullet: "Code-image recorder
wrapper and versioned (time-correct) operand decode in the data-flow language
bindings — `attach_jit`/`attach_pid_versioned` accept a real `img`/`when` instead
of NULL."

**Done when.**
- `make docker-dataflow-<lang>` for each binding passes with the two new
  code-image assertions (`ok`), or a clean `# SKIP` where `codeimage_available()`
  is 0 (no soft-dirty in the container — a real host gate).
- A binding can, on a soft-dirty host, capture a region with a non-NULL `img` and
  get `rc == OK` and `result == a+b`.
- No binding still passes an unconditional NULL for `attach_jit`'s `img` when the
  caller supplied a recorder.

## Task order & parallelism

- **T1 → T2 → T3** is the critical path (the slice half): the by-pointer seed must
  exist before the seven can wrap slices, and the wrapper must exist before its
  tests. T1 is a one-day lib change; T2 is the bulk (seven bindings, one owner who
  knows each marshalling); T3 rides T2.
- **T4** (the code-image half) is **independent** of T1-T3 — it touches the same
  binding files but different symbols (recorder + `attach_jit` `img` arg, not
  slices), so a second person can do it concurrently. The only coordination is that
  both T2/T3 and T4 edit `bindings/<lang>/dataflow.*` and each
  `test_dataflow.<lang>`; land T1-T3 and T4 as separate commits per binding to keep
  merges clean, or sequence T4 after T3 if one person does both.
- Within T2/T3/T4, the seven bindings are mutually independent — Ruby's Fiddle work
  shares nothing with Java's FFM work — so they can be parallelised per language.

```
T1 ──▶ T2 ──▶ T3          (slice surface, critical path)
T4 ───────────────         (code-image arg, parallel; shares files, not symbols)
```

## Constraints & gates

- **No self-skip lanes.** Every toolchain here is an installable, pinned apt/tarball
  dependency already present in `bindings/Dockerfile.lang` via the
  `DOCKER_APT_<lang>` knobs ([mk/docker.mk:140-152](../../../mk/docker.mk#L140)) —
  Ruby 3.2, LuaJIT 2.1, Zig 0.13.0 (sha256-pinned), Rust 1.75, Go, OpenJDK 25,
  .NET SDK 8.0. There is **no hardware or credential gate** on this work: it is pure
  software on Linux x86-64. The one legitimate host gate is
  `asmtest_codeimage_available()` returning 0 (a container without soft-dirty page
  tracking) — the T4 code-image assertions `# SKIP` there and that is recorded as a
  host fact, not a feature self-skip. `ptrace` is a **capability**, not a gate: the
  lanes add `--cap-add=SYS_PTRACE` and the victim opts in via `PR_SET_PTRACER_ANY`,
  so an ETRACE is a misconfigured lane and reddens (never skips) — see the lane's
  own note at [mk/dataflow.mk:247-257](../../../mk/dataflow.mk#L247).
- **Pin discipline.** No version is bumped by this work; every image inherits its
  pins from `mk/docker.mk` and `bindings/Dockerfile.lang`. If a binding needs a
  package it lacks (it does not — all seven toolchains already run their F7 lanes),
  add it to `DOCKER_APT_<lang>` with a pinned version, never install on the host
  (CLAUDE.md rule).
- **ABI stability.** T1 is strictly additive; the by-value `asmtest_slice_forward` /
  `_backward` signatures Python/C++/Node depend on must not change.

## Research notes (verified 2026-07-17)

By-value struct marshalling per FFI, at the versions each Dockerfile pins — the
evidence for T1's by-pointer seed being the correct path rather than teaching each
FFI to pass a 72-byte aggregate:

- **The struct is a SysV MEMORY-class stack copy.** `at_val_rec_t` is 9 eightbytes
  (72 bytes); the SysV x86-64 psABI (Draft 0.99.6 §3.2.3,
  <https://refspecs.linuxbase.org/elf/x86_64-abi-0.99.pdf>) classifies any object
  larger than four eightbytes as MEMORY and passes it on the stack — no
  register-classification benefit a pointer would forfeit.
- **Ruby Fiddle 1.1.1** (bundled with ruby3.2, apt `ruby` on noble —
  <https://packages.ubuntu.com/noble/ruby>, <https://stdgems.org/fiddle/>): **no
  by-value struct support.** `Fiddle::Importer` struct classes are memory overlays
  passed as pointers; by-value struct arguments are unparseable — open since Oct
  2022 (<https://github.com/ruby/fiddle/issues/114>), and the NEWS through 1.1.8
  lists no by-value-struct feature (1.1.7 only added *bool struct fields*)
  (<https://github.com/ruby/fiddle/blob/master/NEWS.md>). This is the binding that
  makes by-pointer mandatory.
- **Java FFM** (final since JDK 22, JEP 454 — <https://openjdk.org/jeps/454>; apt
  `openjdk-25-jdk-headless`, noble universe/updates —
  <https://launchpad.net/ubuntu/noble/+source/openjdk-25>): by-value `StructLayout`
  args *are* supported, but require explicit `paddingLayout` members and canonical
  member layouts (<https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/lang/foreign/Linker.html>)
  — for `at_val_rec_t` that means modelling pads at 20-23, 45-47, 52-55, 68-71. The
  by-pointer seed removes all of it.
- **.NET `LibraryImport`** (apt `dotnet-sdk-8.0` = 8.0.129 —
  <https://packages.ubuntu.com/noble/dotnet-sdk-8.0>): passes by-value unmanaged
  structs, but C# `bool` is non-blittable by default; a by-value record would need
  assembly-level `[DisableRuntimeMarshalling]`
  (<https://learn.microsoft.com/en-us/dotnet/standard/native-interop/disabled-marshalling>)
  or `byte`-for-`bool`. The repo binding uses `DllImport`
  ([Program.cs:31-69](../../../bindings/dotnet/dataflow_smoke/Program.cs#L31)); the
  by-pointer seed sidesteps the blittability problem.
- **LuaJIT FFI** (apt `luajit` 2.1 — <https://packages.ubuntu.com/noble/luajit>):
  supports struct-by-value calls but never JIT-compiles them ("Calls to C functions
  with aggregates passed or returned by value" is on the NYI list —
  <https://luajit.org/ext_ffi_semantics.html>); plain PUC Lua has no FFI at all. The
  binding requires LuaJIT; by-pointer keeps the call compiled.
- **Go cgo** (<https://pkg.go.dev/cmd/cgo>): exposes `C.struct_at_val_rec` by value;
  the binding is cgo-only (no purego). **Zig** `extern struct` matches the C ABI
  (<https://ziglang.org/documentation/0.13.0/#extern-struct>) with 1-byte `bool`.
  **Rust** `repr(C)` is the FFI representation
  (<https://doc.rust-lang.org/reference/type-layout.html>) with `bool` size/align 1
  and bit patterns `0x00`/`0x01` (<https://doc.rust-lang.org/reference/types/boolean.html>).
  These four *could* pass the struct by value, but the by-pointer seed keeps the L2
  API uniform across all seven and matches the repo convention that the FFI layer
  mirrors no C struct by value (`bindings/ruby/asmtest.rb:5`,
  `bindings/lua/asmtest.lua` header).

**Caveats (do not treat as settled).** The offsets/size were compiled on the
darwin/arm64 host; field widths make the 72-byte layout identical on Linux x86-64
but that exact target was not recompiled — **re-verify offsets with a
`_Static_assert(offsetof(...))` or a one-off print on the Linux image before
trusting the Ruby `pack` template.** Fiddle's lack of by-value support is inferred
from issue #114 + absence in NEWS, not an explicit upstream statement. The exact
`openjdk-25-jdk-headless` binary version (25.0.1+8 vs 25.0.2+10) and whether the
noble apt install resolves it were not rebuilt to confirm.

## Out of scope

- The **producer** side of versioned decode and worker-targeting is done and shipped
  in [src/dataflow_ptrace.c](../../../src/dataflow_ptrace.c) — this doc only wraps it.
- The **object-identity** GC canonicalization (GCBulkType/Node/Edge) that F4's
  address-identity increment defers is
  [dataflow-f4-object-identity.md](dataflow-f4-object-identity.md), not here; the
  slice surface wrapped here consumes whatever the L1 def-use graph produces.
- The **F6 windowed survey** and its glue-elision barrier
  ([dataflow-producer-correctness.md](dataflow-producer-correctness.md)) are a
  producer concern; the bindings wrap the scoped attach entry points only.
- The **PT-derived value path** ([dataflow-pt-replay-tier.md](dataflow-pt-replay-tier.md))
  and **foreign-pid PT attach** ([intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md))
  are separate tiers; this doc adds no PT capture.
- The eBPF code-emission detector (`asmtest_codeimage_watch_bpf` and friends) is not
  wrapped here — only the always-available soft-dirty recorder (T4 step 1).
