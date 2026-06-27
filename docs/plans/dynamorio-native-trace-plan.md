# asm-test - DynamoRIO in-process native tracing: implementation plan

A phased roadmap for adding an optional **native runtime tracing** tier backed by
DynamoRIO. This complements the existing Unicorn emulator trace tier: Unicorn
traces isolated guest bytes; DynamoRIO traces code as it runs natively inside the
real process.

The user-facing requirement for this plan is **in-process attach**, not a
`drrun`-launched helper process. Language wrappers should be able to initialize
the runtime at app startup, mark a trace region, execute native or
Keystone-generated host code, and read back the covered basic blocks or
instructions.

> Status legend: **planned** unless noted. Update this file as phases land, the
> way [inline-asm-keystone-plan.md](inline-asm-keystone-plan.md) and
> [win64-native-tier-plan.md](win64-native-tier-plan.md) track theirs.

---

## Validation notes

This plan was validated against the current asm-test codebase and the relevant
DynamoRIO documentation.

**DynamoRIO documentation checked.**

- `dr_app.h` documents the Application Interface for "running portions of a
  program under its control": `dr_app_setup`, `dr_app_start`, `dr_app_stop`,
  `dr_app_stop_and_cleanup`, and friends. It also states that `dr_app_setup`
  must precede other app API calls, `dr_app_start` must be called from the same
  thread as setup, and on Linux thread/signal-handler assumptions can make late
  takeover unpredictable.
  https://dynamorio.org/dr__app_8h.html
- The deployment guide confirms that the Application Interface is the supported
  way to run a subset of an application under DynamoRIO, and notes that runtime
  options are supplied through `drconfig`, `drrun`, or `dr_register_process`.
  It also documents limited static-link support via
  `configure_DynamoRIO_static` and `use_DynamoRIO_static_client`, with the major
  caveat that static mode loses library isolation.
  https://dynamorio.org/page_deploy.html
- The client build guide recommends CMake with `find_package(DynamoRIO)`,
  `configure_DynamoRIO_client`, and `use_DynamoRIO_extension` for extensions.
  https://dynamorio.org/page_build_client.html
- `drmgr` exposes `drmgr_register_bb_instrumentation_event`, which is the right
  hook family for basic-block analysis and instrumentation.
  https://dynamorio.org/group__drmgr.html
- `drwrap` can wrap functions with pre/post callbacks and can locate exported
  functions with `dr_get_proc_address`; it also notes that internal functions
  use `drsyms`. The docs also call out the separate LGPL 2.1 license for
  `drwrap`, which affects packaging.
  https://dynamorio.org/page_drwrap.html
- `drsyms` provides symbol information for ELF, Mach-O, PE/COFF, PDB, and DWARF
  line info. It is optional for this plan because marker APIs and explicit code
  registration should work without debug symbols.
  https://dynamorio.org/page_drsyms.html
- `drcov` confirms DynamoRIO's natural coverage shape: executed basic blocks,
  per-process logs, and lcov conversion.
  https://dynamorio.org/page_drcov.html

**asm-test code checked.**

- The existing trace contract is `emu_trace_t`: ordered instruction offsets,
  distinct basic-block offsets, dynamic totals, and a truncation bit in
  [include/asmtest_emu.h](../../include/asmtest_emu.h). DynamoRIO should reuse
  this shape or a source-compatible extracted form.
- Unicorn fills that trace via `UC_HOOK_CODE` and `UC_HOOK_BLOCK` in
  [src/emu.c](../../src/emu.c). DynamoRIO's `drmgr` basic-block hooks map
  naturally to the block side; instruction tracing is a separate heavier mode.
- Dynamic-language bindings already use opaque trace handles in
  [src/ffi.c](../../src/ffi.c), so the native tracing tier should follow that
  handle/accessor style instead of exposing struct layout everywhere.
- Keystone already assembles text to bytes in
  [include/asmtest_assemble.h](../../include/asmtest_assemble.h) and
  [src/assemble.c](../../src/assemble.c). Native DynamoRIO tracing needs an
  additional executable-memory path for host-native bytes; cross-ISA Keystone
  output remains emulator-only.
- The Makefile keeps optional tiers dependency-gated (`UNICORN_*`,
  `KEYSTONE_*`, `CAPSTONE_*`) and has precedent for separate shared libraries
  such as `libasmtest_emu_full`. DynamoRIO should follow that pattern and never
  become a core dependency.

---

## Goals and non-goals

**Goals.**

1. Provide an optional in-process native tracing backend using DynamoRIO's
   Application Interface.
2. Let language wrappers initialize tracing at app startup and record only
   explicit regions.
3. Reuse asm-test's offset-based trace model: offsets from a registered code
   range or symbol base, distinct block coverage, ordered instruction trace when
   requested, totals, and truncation.
4. Support native host routines and Keystone-generated **host-native** assembly
   materialized into executable memory.
5. Keep DynamoRIO out of the core framework, normal emulator library, and
   Keystone-only assembly path unless the optional native trace tier is built.

**Non-goals.**

- Replacing Unicorn. DynamoRIO cannot provide cross-ISA execution, isolated guest
  memory, or Unicorn's faults-as-data model.
- Making in-process attach the default runner mode. It is an advanced optional
  backend, especially inside Python, Node, JVM, or .NET runtimes.
- Tracing arbitrary source lines in phase 1. Source reporting should reuse the
  existing caller-supplied line map concept after offset coverage works.
- Full memory-reference tracing in the first slice. Memory events are valuable
  but have much higher overhead and complexity than block coverage.

---

## Design overview

```
language/app startup
    |
    v
asmtest_dr_init(client path/options)
    |
    v
dr_app_setup() + configure client + dr_app_start()
    |
    v
trace_begin("region") -> native code runs -> trace_end("region")
    |
    v
trace buffers contain offsets from registered code ranges
```

Two libraries are needed:

- `libasmtest_drapp`: a normal application/library API called by C and language
  wrappers. It initializes DynamoRIO, registers code ranges, exposes begin/end
  marker functions, owns app-visible trace handles, and shuts down the runtime.
- `libasmtest_drclient`: a DynamoRIO client built with DynamoRIO's CMake flow.
  It observes marker calls, tracks active regions in client-local state, and
  inserts block/instruction instrumentation through `drmgr`.

Do **not** rely on ordinary shared C globals between the app library and the
DynamoRIO client. DynamoRIO clients may be privately loaded. Communication
should flow through explicit app-code marker calls that the client wraps or
observes, and through app-owned trace buffers whose addresses are passed through
registration marker calls.

The default execution model should be:

1. `asmtest_dr_init()` is called once, early in process startup.
2. DynamoRIO remains active but mostly idle.
3. `asmtest_trace_begin(name)` and `asmtest_trace_end(name)` mark regions.
4. The client records only when the current thread is inside an active region.
5. `asmtest_dr_shutdown()` stops and cleans up, normally at app exit.

---

## Public API sketch

New header: `include/asmtest_trace.h`.

```c
typedef struct {
    uint64_t *insns;
    size_t insns_cap;
    size_t insns_len;
    uint64_t insns_total;

    uint64_t *blocks;
    size_t blocks_cap;
    size_t blocks_len;
    uint64_t blocks_total;

    bool truncated;
} asmtest_trace_t;

asmtest_trace_t *asmtest_trace_new(size_t insns_cap, size_t blocks_cap);
void asmtest_trace_free(asmtest_trace_t *t);
int asmtest_trace_covered(const asmtest_trace_t *t, uint64_t off);
void asmtest_trace_report(const asmtest_trace_t *t, FILE *out);
```

Compatibility path:

```c
typedef asmtest_trace_t emu_trace_t;
```

`include/asmtest_emu.h` keeps the existing `emu_trace_t` name and accessors, but
the implementation moves common allocation/reporting/coverage logic into
`src/trace.c`.

New header: `include/asmtest_drtrace.h`.

```c
typedef struct asmtest_drtrace asmtest_drtrace_t;

typedef enum {
    ASMTEST_DRTRACE_BLOCKS,
    ASMTEST_DRTRACE_INSNS,
    ASMTEST_DRTRACE_EVENTS,
} asmtest_drtrace_mode_t;

typedef struct {
    const char *dynamorio_home;
    const char *client_path;
    const char *client_options;
    asmtest_drtrace_mode_t mode;
} asmtest_drtrace_options_t;

int asmtest_dr_init(const asmtest_drtrace_options_t *opts);
int asmtest_dr_start(void);
int asmtest_dr_stop(void);
void asmtest_dr_shutdown(void);

int asmtest_dr_register_region(const char *name, void *base, size_t len,
                               asmtest_trace_t *trace);
int asmtest_dr_unregister_region(const char *name);

void asmtest_trace_begin(const char *name);
void asmtest_trace_end(const char *name);
```

The marker functions must stay real exported application functions, not macros,
so the DynamoRIO client can wrap or instrument them.

---

## Phase 0 - Configuration and lifecycle spike

**Goal.** Prove the in-process attach path before building the full trace system.

**Deliverables.**

- A tiny C program that calls `dr_app_setup`, configures or registers
  `libasmtest_drclient`, calls `dr_app_start`, executes a known function, and
  calls `dr_app_stop_and_cleanup`.
- A documented decision for the dynamic client-loading mechanism:
  `dr_register_process`/configuration files if using dynamic DR libraries, or
  the static client path if dynamic app API cannot reliably load a client in the
  desired packaging model.
- Explicit lifecycle rules:
  - initialize once per process;
  - call setup/start from the same thread;
  - initialize as early as wrapper/app startup permits;
  - do not fork after DR has started in phase 1 tests;
  - do not run the initial native trace tests with `-j`.

**Acceptance.** A minimal program runs under DynamoRIO control without `drrun`,
the client receives a basic-block callback, and teardown returns the process to
native execution without crashing.

**Effort.** 2-4 days. This phase gates all later phases.

---

## Phase 1 - Extract a generic trace substrate

**Goal.** Make trace allocation/reporting engine-neutral so Unicorn and
DynamoRIO can share it.

**Deliverables.**

- `include/asmtest_trace.h` with `asmtest_trace_t` and generic trace helpers.
- `src/trace.c` holding:
  - append ordered instruction offset;
  - append/deduplicate block offset;
  - allocation/free/accessors for opaque FFI handles;
  - report, uncovered, lcov, and source-line helpers currently tied to
    `emu_trace_t`.
- `include/asmtest_emu.h` includes `asmtest_trace.h` and aliases
  `emu_trace_t` to preserve source compatibility.
- Existing `asmtest_emu_trace_*` FFI functions remain as wrappers around generic
  trace helpers.

**Acceptance.** `make emu-test` and binding conformance still pass with no public
Unicorn behavior change.

**Effort.** 1-2 days.

---

## Phase 2 - DynamoRIO client: marker regions and block coverage

**Goal.** Record native basic-block coverage for explicit begin/end regions.

**Client design.**

- Build `libasmtest_drclient` as a DynamoRIO client with CMake:
  `find_package(DynamoRIO)`, `configure_DynamoRIO_client`, and
  `use_DynamoRIO_extension(... drmgr)`.
- Initialize `drmgr` in `dr_client_main`.
- Wrap or observe these app marker functions:
  - `asmtest_dr_register_region(name, base, len, trace)`
  - `asmtest_trace_begin(name)`
  - `asmtest_trace_end(name)`
- Keep client-local registries:
  - region name -> code range and app-owned trace buffer;
  - per-thread active region stack or active region pointer;
  - mode flags.
- Use `drmgr_register_bb_instrumentation_event` to insert a lightweight clean
  call at each basic-block entry.
- In the clean call, if the thread has an active region and the block PC falls
  inside that region's registered code range, record `pc - base` into the
  app-owned `asmtest_trace_t`.

**Important constraint.** Phase 2 should record only the thread that entered the
region unless the API explicitly opts into all-thread recording. This avoids
surprising language runtimes with unrelated VM/helper-thread blocks.

**Acceptance.**

- A C test registers a compiled routine range, starts a named region, calls the
  routine, ends the region, and sees block offset `0` covered.
- Re-running with the same trace accumulates coverage, matching the existing
  `emu_trace_t` semantics.
- Truncation works when the caller supplies a tiny block buffer.

**Effort.** 3-5 days after Phase 0.

---

## Phase 3 - Application API and runner tests

**Goal.** Provide the normal-library API that C tests and language wrappers will
call.

**Deliverables.**

- `src/drtrace_app.c`: normal C library side, linked by callers.
- `include/asmtest_drtrace.h`: public API from the sketch above.
- `examples/test_drtrace.c`: native trace smoke tests.
- Makefile targets:
  - `drtrace-client`
  - `drtrace-test`
  - `shared-drtrace`
- Build knobs:
  - `DYNAMORIO_DIR=/path/to/DynamoRIO/cmake`
  - `DYNAMORIO_HOME=/path/to/DynamoRIO`

**Runner notes.**

- Initial tests should run `--no-fork` and without `-j`, or use a custom smoke
  binary outside the standard runner. The normal runner's fork isolation is a
  bad first interaction point for DR app startup.
- Later phases can decide whether initializing DR inside forked children is safe
  enough for a normal asm-test suite.

**Acceptance.**

- `make drtrace-test` self-skips with a clear message when DynamoRIO is absent.
- With DynamoRIO installed, `make drtrace-test` records native block coverage for
  at least one existing x86-64 routine.

**Effort.** 2-3 days.

---

## Phase 4 - Host-native Keystone executable code

**Goal.** Let Keystone-generated host-native bytes be traced by DynamoRIO.

**Design.**

The existing Keystone path assembles strings to bytes for the emulator. Native
DynamoRIO tracing needs a new executable-memory layer:

```c
typedef struct {
    void *base;
    size_t len;
} asmtest_exec_code_t;

int asmtest_asm_exec_native(const char *src, int syntax,
                            asmtest_exec_code_t *out);
void asmtest_asm_exec_free(asmtest_exec_code_t *code);
```

Implementation notes:

- Assemble only for the host ISA in phase 4: x86-64 first, AArch64 later.
- Allocate writable memory, copy bytes, switch to executable with `mprotect` or
  platform equivalent, and flush the instruction cache where required.
- Register the generated code range with `asmtest_dr_register_region`.
- Make the caller responsible for invoking the code through an ABI-correct
  function pointer.
- Cross-ISA assembly remains emulator-only.

**Acceptance.**

- A C test assembles `"mov rax, rdi; add rax, rsi; ret"` on x86-64, registers
  the executable range, traces the call under DynamoRIO, and verifies block
  offset `0` plus return value `42`.

**Effort.** 2-4 days.

---

## Phase 5 - Instruction trace mode

**Goal.** Add ordered instruction offsets for native regions.

**Design.**

- Keep block coverage as the default mode.
- In `ASMTEST_DRTRACE_INSNS`, insert instrumentation for every app instruction
  whose containing block may fall within a registered region.
- Record `pc - base` into `trace->insns` and increment `insns_total`.
- Preserve the existing truncation semantics.
- Keep Capstone as the backend-neutral annotation path: DynamoRIO observes the
  native PCs/offsets, while Capstone renders the recorded offsets back to
  readable instruction text from the registered or generated code bytes. Do not
  tie public trace diagnostics to DynamoRIO's internal decoder.

**Caveat.** This mode can be expensive, especially inside language runtimes.
Expose it as opt-in and document it as diagnostic rather than the default
coverage path.

**Acceptance.** A fixed generated routine reports a deterministic ordered
instruction offset sequence on x86-64.

**Effort.** 2-3 days.

---

## Phase 6 - Language wrapper surface

**Goal.** Give dynamic languages an ergonomic in-process API while keeping the
advanced nature visible.

Wrapper shape:

```python
from asmtest.native import NativeTrace

NativeTrace.initialize(
    dynamorio_home="/opt/dynamorio",
    client="./build/libasmtest_drclient.so",
)

trace = NativeTrace.new(blocks=64, instructions=0)
code = asmtest.assemble_exec_native("""
    mov rax, rdi
    add rax, rsi
    ret
""")

trace.register("ks_add", code)
with trace.region("ks_add"):
    result = code.call2(20, 22)

assert result == 42
assert trace.covered(0)
NativeTrace.shutdown()
```

Initial bindings:

1. Python, because it is the easiest place to validate startup/lifecycle and
   generated code ergonomics.
2. Node or Ruby next, to prove dynamic-FFI portability.
3. Rust/Zig/C++ can bind the C API directly after the ABI stabilizes.

**Acceptance.** Python wrapper can initialize DR in-process, trace a generated
host-native assembly function, and read block coverage without launching
`drrun`.

**Effort.** 3-6 days for the first binding, then 1-2 days per additional dynamic
binding.

---

## Phase 7 - Optional symbol/function mode

**Goal.** Trace a named native function without explicit begin/end markers.

**Design.**

- Use exported symbols through `dr_get_proc_address`.
- Use internal symbols through optional `drsyms`, gated by availability and
  license/package constraints.
- Use `drwrap_wrap` for function entry/exit to toggle the active region.

**Caveats.**

- `drwrap` carries LGPL 2.1 licensing separate from the rest of DynamoRIO.
- Wrapping may be less robust for inlined functions, hand-written labels, or
  generated code than explicit region markers.

**Acceptance.** A compiled exported function can be traced by symbol name with no
manual region calls in the test body.

**Effort.** 2-4 days.

---

## Phase 8 - Docs, packaging, and CI

**Docs.**

- New `docs/native-tracing.md` explaining:
  - DynamoRIO vs Unicorn;
  - in-process startup lifecycle;
  - marker regions;
  - generated host-native assembly;
  - block vs instruction mode;
  - optional Capstone annotation of native trace offsets;
  - language wrapper examples;
  - known limitations.
- Update `docs/features.md` with a third execution tier column: native runtime
  trace.
- Update `docs/index.md` and `docs/bindings.md` after wrapper APIs land.

**Packaging.**

- Keep the core and `libasmtest_emu` DynamoRIO-free.
- Add a separate `libasmtest_drtrace` or two-library distribution
  (`libasmtest_drapp` plus `libasmtest_drclient`).
- Reuse the existing optional Capstone reporting/disassembly layer for native
  traces where possible, so Unicorn and DynamoRIO traces share report formats
  and no DynamoRIO-only decoder API leaks into language bindings.
- If static DynamoRIO support is added, keep it as a separate experimental
  artifact because DynamoRIO documents reduced library isolation in static mode.

**CI.**

- Add a `drtrace` CI job only on Linux x86-64 at first.
- Cache or install a pinned DynamoRIO release.
- Run `make drtrace-test`.
- Keep normal tests independent of DynamoRIO.

**Effort.** 2-4 days.

---

## Risks and open points

- **Client-loading in app-api mode.** The first spike must settle whether dynamic
  `dr_register_process`/configuration is sufficient for language wrappers, or
  whether a static-client build is needed for reliable in-process attach.
- **Language runtime fragility.** Python, Node, JVM, and .NET processes have
  threads, signal handlers, JIT code, and native libraries. Initialize early and
  record narrow regions only.
- **Thread semantics.** The MVP should record only the current region-entering
  thread. All-thread tracing needs explicit API and locking.
- **Private loader transparency.** Do not share C globals between app and client.
  Use marker calls and app-owned buffers.
- **Licensing.** `drwrap` and `drsyms` carry licensing considerations. The marker
  and explicit-range MVP should avoid them until symbol mode is needed.
- **macOS and AArch64.** DynamoRIO docs and build notes are strongest for Linux
  and Windows. Start Linux x86-64 only; treat macOS/AArch64 as follow-up.
- **Fault model.** Native faults remain native faults. Continue using asm-test
  guard pages and runner isolation where possible; do not promise Unicorn-style
  precise guest faults.
- **Overhead.** Basic-block coverage should be the default. Instruction and
  memory-event modes must be opt-in.

---

## Recommended first milestone

The first shippable milestone should be:

1. `asmtest_trace_t` extracted and shared.
2. `libasmtest_drapp` plus `libasmtest_drclient` building on Linux x86-64.
3. In-process `dr_app_*` initialization from a C smoke test.
4. Marker-based region tracing for registered native code ranges.
5. Basic-block coverage only.
6. One Keystone host-native generated-code demo.

That proves the architecture without taking on symbol wrapping, instruction
traces, memory events, or all-language binding parity too early.
