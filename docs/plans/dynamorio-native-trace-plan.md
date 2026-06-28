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
- `drwrap` can wrap functions with pre/post callbacks, read their arguments with
  `drwrap_get_arg`, and locate exported functions with `dr_get_proc_address`; it
  also notes that internal functions use `drsyms`. The MVP uses `drwrap` to
  capture the marker-call arguments (see Phase 2), so its separate LGPL 2.1
  license is accepted as an up-front packaging decision rather than a Phase 7
  caveat.
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
  `KEYSTONE_*`, `CAPSTONE_*` pkg-config `CFLAGS`/`LIBS`, with `CAPSTONE_DEF :=
  -DASMTEST_HAVE_CAPSTONE` auto-detected and degrading at runtime). The real
  shared-library precedent is the single superset `libasmtest_emu` (the
  `shared-emu` target, Makefile ~line 348) standing apart from the lean core
  `libasmtest` (`shared`); there is **no** `libasmtest_emu_full` target today —
  the emu/emu+asm/emu+full split was collapsed into one superset. DynamoRIO and
  the hardware-trace tiers add their own separate optional shared libraries and
  never become a core dependency.

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

A further group of trace producers needs no new trace shape at all. Phase 1's
`asmtest_trace_t` / `src/trace.c` substrate is the shared *sink*, and Unicorn
hooks, the DynamoRIO `drmgr` client, Intel PT (via libipt), and ARM CoreSight
(via OpenCSD) are interchangeable *backends* that all fill the same offsets. The
begin/end markers are backend-neutral. The Capstone annotation layer is *offset-
based* (it renders recorded offsets from caller-supplied code bytes) but today is
`emu_*`/`emu_arch_t`-typed and declared in `asmtest_emu.h`; Phase 1 makes it
backend-neutral *by name*. With that, a caller can switch backends without
changing test code. Phase 9 adds the two hardware-trace backends; the rest of
this plan builds the DynamoRIO one.

Do **not** rely on ordinary shared C globals between the app library and the
DynamoRIO client. DynamoRIO clients may be privately loaded. Communication
should flow through explicit app-code marker calls that the client wraps with
`drwrap` (reading their arguments with `drwrap_get_arg`), and through app-owned
trace buffers whose addresses are passed through registration marker calls.

**Licensing decision (up front).** The marker argument-capture mechanism uses the
`drwrap` extension, which DynamoRIO licenses under LGPL 2.1 separately from the
rest of the framework. The native trace tier therefore takes on `drwrap`'s
LGPL-2.1 obligation as a hard packaging decision from the MVP onward, not a late
symbol-mode concern. This is acceptable because the tier already ships as a
separate optional artifact (`libasmtest_drclient`) that the core and
`libasmtest_emu` never link. `drsyms` (Phase 7) carries its own considerations
and stays separately deferred.

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

Compatibility path. `emu_trace_t` is currently an untagged
`typedef struct {...} emu_trace_t;` body in `include/asmtest_emu.h` (lines
165-181), referenced by ~15 `emu_*_call_traced` signatures and the FFI
accessors. To avoid a conflicting redefinition, Phase 1 **deletes that struct
body** from `asmtest_emu.h`, moves the canonical definition (renamed
`asmtest_trace_t`, given a tag `struct asmtest_trace`) into the new
`include/asmtest_trace.h`, then has `asmtest_emu.h` `#include` it and add:

```c
typedef asmtest_trace_t emu_trace_t;
```

Only the *type definition* moves to `asmtest_trace.h`; the helper
*implementations* move from `src/emu.c`/`src/ffi.c` into `src/trace.c`
(enumerated in Phase 1). `emu_trace_t` keeps its name, so all existing signatures
and bindings compile unchanged.

`asmtest_trace_covered` returns `int` to match the existing FFI accessor
`asmtest_emu_trace_covered(const emu_trace_t *, unsigned long long off)`; the
existing `bool emu_trace_covered(const emu_trace_t *, uint64_t off)` C helper
(used by the `ASSERT_BLOCK_COVERED` macro) stays exported unchanged.
`asmtest_trace_report` is the canonical form of the existing
`emu_trace_report(const emu_trace_t *, FILE *)` (no `asmtest_emu_trace_report`
exists today).

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

**Lifecycle and options notes.** `asmtest_dr_init` performs `dr_app_setup` and
client configuration but **not** `dr_app_start`; `asmtest_dr_start` performs
`dr_app_start` (so the Design-overview diagram's single box expands to
`init = setup + configure`, then a separate `start`). `client_path` and
`dynamorio_home` apply only to the **dynamic** client-loading variant; if Phase 0
selects the static client they are ignored — mark them "dynamic-loading variant
only". `mode` (blocks vs insns) is a **per-trace** property and also appears on
`register_region`/`trace_new`; the process-init `mode` is only the default.

**Bracketed takeover for managed hosts (decided).** `start`/`stop` are not only
an init-once step. Inside a JIT/GC-heavy runtime (JVM, .NET, Node) the supported
model is to keep DynamoRIO **stopped** (native execution) for almost the whole
process and call `asmtest_dr_start` / `asmtest_dr_stop` to **bracket only the
call into the registered routine**, so the runtime spends ~all its life running
natively and DR never translates the runtime's churning JIT/GC code. This is the
central mitigation for the per-language fragility documented in
[Language runtime support](#language-runtime-support); its cost — repeated
all-thread takeover, which is the fragile part on Linux — is what the Phase 0
spike must measure. For CPython and native callers the cheaper persistent-`STARTED`
model is fine because nothing is churning code concurrently.

---

## Phase 0 - Configuration and lifecycle spike *(planned)*

**Goal.** Prove the in-process attach path before building the full trace system.

**Deliverables.**

- A tiny C program that calls `dr_app_setup`, configures or registers
  `libasmtest_drclient`, calls `dr_app_start`, executes a known function, and
  calls `dr_app_stop_and_cleanup`.
- A documented decision for the dynamic client-loading mechanism:
  `dr_register_process`/configuration files if using dynamic DR libraries, or
  the static client path if dynamic app API cannot reliably load a client in the
  desired packaging model.
- A lifecycle **state machine** enforced in `libasmtest_drapp`, not left as
  etiquette. A single guarded global holds `{state, owning_thread_id}` with
  states `UNINIT -> INIT -> STARTED -> STOPPED -> SHUTDOWN`:
  - `asmtest_dr_init` records the calling (setup) thread; a second `init` is a
    no-op returning an "already initialized" code.
  - `asmtest_dr_start` must run on the setup thread (DR requires it) and only
    from `INIT`/`STOPPED`; out-of-order calls return a clear error code rather
    than invoking DR (whose ordering violations are UB).
  - `register_region`/`begin`/`end` before `STARTED`, `stop` without `start`,
    and `shutdown` while a region is active each return a defined error.
  - `asmtest_dr_shutdown` runs `dr_app_stop_and_cleanup` and must run **before**
    the process tears down its threads / language runtime (ahead of, not via,
    `atexit` ordering).
  - **fork**: forking after `start` is unsupported; install a `pthread_atfork`
    child handler that disables tracing in the child (Python `multiprocessing`,
    pre-fork servers), or document hard-unsupported.
  - initialize as early as startup permits, on the process **main thread**,
    before the heavy runtime spins up worker/GC threads.
  - the standard runner must run these tests `--no-fork` and without `-j` (see
    Phase 3 runner ownership).
- A **managed-host gate**, run **first against CPython** (the friendliest runtime
  — GIL-serialized, no default JIT). Two measurements decide whether the
  in-process model survives a real language runtime:
  - a **takeover/detach micro-benchmark**: time repeated `asmtest_dr_start` /
    `asmtest_dr_stop` brackets around one call, to cost the bracketed-takeover
    model (see the Public-API lifecycle note);
  - a **signal-chaining check**: the client registers `dr_register_signal_event`
    returning `DR_SIGNAL_DELIVER`, and the test confirms the host's own SIGSEGV
    path (a deliberate faulting probe the runtime recovers from) still works while
    DR is started — DR intercepts all signals and never lets SIGSEGV be blocked,
    so chaining, not suppression, is the only viable coexistence.
  If CPython passes both, proceed; if not, route managed runtimes to the Phase 9
  hardware-trace backend earlier (see
  [Language runtime support](#language-runtime-support)).

**Acceptance.** A minimal program runs under DynamoRIO control without `drrun`,
the client receives a basic-block callback, the clean call can write through an
app-passed pointer into app-owned memory (the architecture's hinge — prove it
here), and teardown returns the process to native execution without crashing.
Additionally, the CPython managed-host gate passes: a generated routine is traced
from inside a live CPython process with the host's SIGSEGV path still functional.

**Effort.** 2-4 days. This spike gates the DynamoRIO-dependent phases (2-8).
Phase 1 (trace-substrate extraction) has **no** DynamoRIO dependency and may
proceed before or in parallel with this spike.

---

## Phase 1 - Extract a generic trace substrate *(planned)*

**Goal.** Make trace allocation/reporting engine-neutral so Unicorn and
DynamoRIO can share it.

**Deliverables.**

- `include/asmtest_trace.h` owning the canonical `asmtest_trace_t` (tagged
  `struct asmtest_trace`), the `emu_line_map_t`/`emu_line_entry_t` types, and the
  generic helper declarations.
- `src/trace.c` holding:
  - `trace_append_insn(t, off)` and `trace_append_block(t, off)` — the
    append+truncate+dedup logic **factored out** of the Unicorn callbacks, where
    it is currently inlined in `on_code`/`on_block` (`src/emu.c` ~lines 86-124).
    After extraction both the Unicorn hooks and the DynamoRIO reconciliation call
    these helpers (block append dedups against `blocks[]`, sets `truncated` on
    overflow).
  - the opaque-handle allocation/free/accessors currently in `src/ffi.c`
    (`asmtest_emu_trace_new/free/insns_total/blocks_len/blocks_total/truncated/
    block_at/covered`), kept **Unicorn-free** exactly as today (`src/ffi.c`
    re-implements the dedup scan precisely so it does not pull in `emu.o`).
  - the coverage/report helpers currently in `src/emu.c`, moved verbatim and kept
    exported under their existing names: `bool emu_trace_covered`,
    `void emu_trace_report`, `size_t emu_coverage_uncovered`,
    `void emu_trace_lcov`, `const emu_line_entry_t *emu_line_lookup`,
    `size_t emu_trace_source_report`, and `void emu_trace_lcov_source`.
- The `*_disasm` annotation helpers (`emu_trace_disasm`, `emu_trace_report_disasm`,
  `emu_coverage_uncovered_disasm`) stay in `src/disasm.c` (Capstone-gated). To
  make the annotation layer backend-neutral *by name* — not merely offset-based —
  also relocate their declarations and `emu_arch_t` to `asmtest_trace.h` (or a
  neutral header) so non-emulator backends can call them without including
  `asmtest_emu.h`, keeping thin `emu_*`-named aliases for compatibility.
- `include/asmtest_emu.h` deletes the `emu_trace_t` struct body, `#include`s
  `asmtest_trace.h`, and adds `typedef asmtest_trace_t emu_trace_t;`.
- Existing `emu_trace_covered` (bool) and the `asmtest_emu_trace_*` FFI accessors
  stay exported (as wrappers where needed) so `ASSERT_BLOCK_COVERED`, the
  bindings, and the ABI manifest are unaffected.

**Acceptance.** `make emu-test` and binding conformance still pass with no public
Unicorn behavior change.

**Effort.** 1-2 days.

---

## Phase 2 - DynamoRIO client: marker regions and block coverage *(planned)*

**Goal.** Record native basic-block coverage for explicit begin/end regions.

**Client design.**

- Build `libasmtest_drclient` as a DynamoRIO client with CMake:
  `find_package(DynamoRIO)`, `configure_DynamoRIO_client`, and
  `use_DynamoRIO_extension(... drmgr drwrap)`.
- Initialize `drmgr` and `drwrap` in `dr_client_main`.
- **Argument capture mechanism (decided): use `drwrap`.** Resolve each marker's
  exported address with `dr_get_proc_address` and `drwrap_wrap` it with a
  pre-callback that reads the arguments by position with `drwrap_get_arg`:
  - `asmtest_dr_register_region(name, base, len, trace)` -> args 0..3
  - `asmtest_trace_begin(name)` -> arg 0
  - `asmtest_trace_end(name)` -> arg 0

  This is why the markers must stay real exported functions (see Public API).
  The alternative `drmgr`-only scheme (resolve the marker entry PCs and read the
  SysV/AAPCS64 argument registers by hand in an inserted clean call) was
  rejected: it hard-codes the calling convention and re-implements what
  `drwrap_get_arg` already provides. The cost of this decision is the `drwrap`
  LGPL-2.1 dependency, accepted up front (see the licensing note in the Design
  overview and Phase 8).
- Keep client-local registries in **client-owned** memory (never share app
  globals):
  - a region table: `register_region(name, base, len, trace)` assigns a small
    integer **region id**, `dr_strdup`s `name` into client memory (the app may
    free its copy after the call), and stores `{base, len, trace*}`;
    `register_region` returns the id. The `name -> id` lookup runs only at
    begin/end, never on the hot path.
  - per-thread state in a **drmgr TLS slot** (`drmgr_register_tls_field` /
    `drmgr_get_tls_field`): a small stack of active region ids.
  - a mode flag (blocks vs insns).
- Use `drmgr_register_bb_instrumentation_event` for instrumentation (not a
  per-block clean call — see below).

**Region model (decided).** Regions form a per-thread **stack**. `begin(name)`
pushes the region id; `end(name)` must match the **top** of the stack and pops
it. A mismatched `end` is an error surfaced as **data** — a client-side counter
plus a one-shot flag the app can read — not a silent correction. Overlapping
registered ranges resolve **innermost-active-region wins** (a block PC is
attributed to the topmost active region whose range contains it). An abandoned
`begin` (early return / `longjmp` / exception in the app) leaves its activation
on the stack until a matching `end` or `asmtest_dr_shutdown`; document that
markers must be balanced, ideally via a scoped wrapper in each binding.

**Block instrumentation (decided).** Do **not** use a per-block clean call with a
linear dedup scan — that pays a full context switch plus an O(n) scan on every
basic block of natively running code, defeating the "cheap default". Instead, at
`bb_instrumentation` time, when the block's **application** PC
(`instr_get_app_pc` of the first instruction — *not* the code-cache PC) falls
inside a registered range, inline a single store: increment a per-block "seen"
flag + hit counter in a client-side per-region table (or append the offset to a
client-side per-thread buffer). Reconcile the **distinct** block offsets and
totals into the app-owned `asmtest_trace_t` at `trace_end` (drcov-style
end-of-region reconciliation), preserving `emu_trace_t` accumulate/dedup/
truncation semantics without any per-block scan. Record `off = app_pc - base`.

**Trace-buffer ownership and concurrency (decided).** The app allocates the
`asmtest_trace_t` and its `insns`/`blocks` buffers via `asmtest_trace_new` and
frees them; the client only writes through the passed-in pointer, and only at
`trace_end` reconciliation (clean-call writes into app memory are valid — the
client runs in the app address space; only client globals are privately
isolated). One trace buffer is bound to **one** thread's region activation: the
contract is that `asmtest_trace_covered`/`asmtest_trace_report` must not be
called, and the same trace must not be entered on another thread, while any
thread is inside that trace's region. This keeps the unlocked `len`/`total`/
`truncated` writes safe and matches the existing "traces are not thread-safe
shared collectors" rule. All-thread recording is a later, explicitly-locked
opt-in.

**Important constraint.** Phase 2 records only the thread that entered the region
(per-thread TLS stack, `pid=0`/per-thread event scope), never unrelated
VM/helper-thread blocks.

**Acceptance.**

- A C test registers a compiled routine range, starts a named region, calls the
  routine, ends the region, and sees block offset `0` covered.
- Re-running with the same trace accumulates coverage, matching the existing
  `emu_trace_t` semantics.
- Truncation works when the caller supplies a tiny block buffer.

**Effort.** 3-5 days after Phase 0.

---

## Phase 3 - Application API and runner tests *(planned)*

**Goal.** Provide the normal-library API that C tests and language wrappers will
call.

**Deliverables.**

- `src/drtrace_app.c`: normal C library side, linked by callers.
- `include/asmtest_drtrace.h`: public API from the sketch above.
- `examples/test_drtrace.c`: native trace smoke tests.
- Makefile targets. `drtrace-test` and `shared-drtrace` follow the existing
  `<thing>-test` and `shared-<tier>` conventions. `drtrace-client` is a **new
  target shape** (no existing `*-client` target) because the DR client is a
  CMake-built `.so`, not a `$(CC)`-built object — document it as the lone
  CMake-driven sub-build the Makefile shells out to.
- Build knobs. `DYNAMORIO_DIR=/path/to/DynamoRIO/cmake` (the `find_package`
  config dir) and `DYNAMORIO_HOME=/path/to/DynamoRIO` (runtime root). These are
  the project's **first** `*_DIR`/`*_HOME` knobs — every existing optional dep
  uses pkg-config `<DEP>_CFLAGS`/`<DEP>_LIBS`. The new shape is unavoidable
  because DynamoRIO ships **no** pkg-config file and requires
  `find_package(DynamoRIO)`; call it out as a deliberate new convention. (libipt
  and OpenCSD in Phase 9 *do* ship pkg-config, so they keep the `*_CFLAGS`/
  `*_LIBS` style.)

**Runner integration (owned here).** The asm-test runner's headline features are
per-test `fork` isolation and a `-jN` pool, both hostile to an in-process DR
attach. Phase 3 **owns** the decision rather than deferring it: the native-trace
tests ship as a **standalone `--no-fork`, single-job smoke harness**
(`examples/test_drtrace.c` run directly), outside the forking runner. Whether DR
can be initialized inside forked children for the normal suite is explicitly
**out of scope** and tracked as a follow-up, not a blocker — mirroring how the
win64 tier gave its runner port its own phase + decision gate.

**Acceptance.**

- `make drtrace-test` self-skips with a clear message when DynamoRIO is absent.
- With DynamoRIO installed, `make drtrace-test` records native block coverage for
  at least one existing x86-64 routine.

**Effort.** 2-3 days.

---

## Phase 4 - Host-native Keystone executable code *(planned)*

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
- Assemble at the **actual host allocation address**, not the emulator's guest
  `EMU_CODE_BASE` (`0x00100000`) that every current assemble path hard-codes, so
  PC-relative and branch targets resolve when the code runs natively.
- Allocate executable memory **W^X-correctly**: `mmap(PROT_NONE)` then `mprotect`
  RW to copy bytes, then `mprotect` RX (on macOS arm64, `MAP_JIT` +
  `pthread_jit_write_protect_np`); flush the icache with `__builtin___clear_cache`
  where required. The existing guard-page `mmap`/`mprotect` helper in
  `src/asmtest.c` only maps `PROT_NONE` and is **not** reusable here — this
  host-executable path is net-new.
- Register the generated code range with `asmtest_dr_register_region`.
- Make the caller responsible for invoking the code through an ABI-correct
  function pointer.
- On free, `asmtest_asm_exec_free` must `asmtest_dr_unregister_region` and tell
  DynamoRIO to drop any cached translation of the range (`dr_flush_region`)
  **before** the memory is reused/unmapped — the native analogue of the
  emulator's code-cache flush. Treat unregister + flush + free as one ordered
  step.
- Cross-ISA assembly remains emulator-only.

**Acceptance.**

- A C test assembles `"mov rax, rdi; add rax, rsi; ret"` on x86-64, registers
  the executable range, traces the call under DynamoRIO, and verifies block
  offset `0` plus return value `42`.

**Effort.** 2-4 days.

---

## Phase 5 - Instruction trace mode *(planned)*

**Goal.** Add ordered instruction offsets for native regions.

**Design.**

- Keep block coverage as the default mode.
- In `ASMTEST_DRTRACE_INSNS`, insert instrumentation for every app instruction
  whose containing block may fall within a registered region.
- Record `pc - base` into `trace->insns` and increment `insns_total`.
- Preserve the existing truncation semantics.
- Keep Capstone as the **offset-based** annotation path (made backend-neutral by
  name in Phase 1): DynamoRIO observes the native application PCs/offsets, while
  Capstone renders the recorded offsets back to readable instruction text from
  the registered or generated code bytes. Do not tie public trace diagnostics to
  DynamoRIO's internal decoder.
- Close one real gap: the report-level helpers (`emu_trace_report_disasm`,
  `emu_trace_disasm`, `emu_coverage_uncovered_disasm`) currently hard-code
  `EMU_CODE_BASE` for PC-relative operand resolution (via `print_block_line`),
  whereas `emu_disas` takes an explicit `base_addr`. For native/HW regions at a
  different runtime base the printed *offsets* are correct but operand text
  resolves against the wrong base — so add a `base_addr` parameter to those
  report helpers (defaulting to `EMU_CODE_BASE` for the emulator) here.

**Caveat.** This mode can be expensive, especially inside language runtimes.
Expose it as opt-in and document it as diagnostic rather than the default
coverage path.

**Acceptance.** A fixed generated routine reports a deterministic ordered
instruction offset sequence on x86-64.

**Effort.** 2-3 days.

---

## Phase 6 - Language wrapper surface *(planned)*

**Goal.** Give dynamic languages an ergonomic in-process API while keeping the
advanced nature visible.

Wrapper shape:

```python
from asmtest.drtrace import NativeTrace

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

**Naming note.** The public module is `asmtest.drtrace` (`asmtest.runtime` is an
acceptable alternative) — **not** `asmtest.native`, which is one underscore from
the existing private loader `asmtest._native` and would conflate the ctypes
loader with the tracing API. `assemble_exec_native` is net-new and sits alongside
the existing `asmtest.assemble`; the new `NativeTrace`/region API is distinct from
the existing emulator-tier `asmtest.Trace` coverage recorder.

Initial bindings:

1. Python, because it is the easiest place to validate startup/lifecycle and
   generated code ergonomics.
2. Node or Ruby next, to prove dynamic-FFI portability.
3. Rust/Zig/C++ bind the C API directly once the ABI is frozen. **Freeze
   trigger:** the `asmtest_drtrace.h` / `asmtest_trace.h` surface is declared
   stable at the **end of Phase 5** (blocks + insns modes shipped); the
   static-language bindings begin then.

**Acceptance.** Python wrapper can initialize DR in-process, trace a generated
host-native assembly function, and read block coverage without launching
`drrun`.

**Effort.** 3-6 days for the first binding, then 1-2 days per additional dynamic
binding.

---

## Phase 7 - Optional symbol/function mode *(planned)*

**Goal.** Trace a named native function without explicit begin/end markers.

**Design.**

- Use exported symbols through `dr_get_proc_address`.
- Use internal symbols through optional `drsyms`, gated by availability and
  license/package constraints.
- Use `drwrap_wrap` for function entry/exit to toggle the active region.

**Caveats.**

- `drwrap` is already a dependency from the Phase 2 MVP, so symbol mode adds no
  new license obligation beyond it; the remaining new consideration here is the
  optional `drsyms` dependency for internal symbols.
- Wrapping may be less robust for inlined functions, hand-written labels, or
  generated code than explicit region markers.

**Acceptance.** A compiled exported function can be traced by symbol name with no
manual region calls in the test body.

**Effort.** 2-4 days.

---

## Phase 8 - Docs, packaging, and CI *(planned)*

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
- Update `docs/index.md` and `docs/bindings.md` after wrapper APIs land, and
  cross-link the existing `docs/traces.md` (emulator trace model) with the new
  `docs/native-tracing.md` so the two coexist rather than fragment.

**Packaging.**

- Keep the core `libasmtest` and the superset `libasmtest_emu` DynamoRIO-free.
- Add a separate two-library distribution (`libasmtest_drapp` plus
  `libasmtest_drclient`), built like the existing optional shared libs but kept
  out of the superset. Because all ten bindings currently `dlopen`
  `libasmtest_emu` via `ASMTEST_LIB`, keeping these tiers out of it means the
  native-trace API is **not** reachable through the existing binding load path —
  the bindings need a separate, explicitly-opted-in load of `libasmtest_drapp`
  (and `libasmtest_hwtrace`). State this so it is a deliberate choice, not a
  surprise.
- Reuse the existing optional Capstone reporting/disassembly layer for native
  traces, so Unicorn, DynamoRIO, and HW traces share report formats and no
  backend-specific decoder API leaks into language bindings.
- If static DynamoRIO support is added, keep it as a separate experimental
  artifact because DynamoRIO documents reduced library isolation in static mode.

**CI.**

- Add a `drtrace` CI job only on Linux x86-64 at first.
- Cache or install a pinned DynamoRIO release.
- Run `make drtrace-test`.
- Keep normal tests independent of DynamoRIO.
- Hardware-assisted trace (Phase 9) needs a *separate* self-hosted bare-metal
  runner job and is allowed to be absent; it never gates normal tests.

**Effort.** 2-4 days.

---

## Phase 9 - Hardware-assisted trace backends (Intel PT, ARM CoreSight) *(planned)*

**Goal.** Record native block and instruction coverage with near-zero *capture*
overhead on capable hardware — Intel PT on bare-metal Intel x86-64, ARM CoreSight
on bare-metal AArch64 — producing the same `asmtest_trace_t` offsets as Unicorn
and DynamoRIO, reusing the registered-region markers, with no DynamoRIO or
`drwrap` dependency. This is an even-more-optional, mostly-bare-metal-Linux tier:
a fast-path complement to DynamoRIO, never a replacement and never a default —
**except** for JIT/GC-heavy managed runtimes (JVM, .NET, Node), where it is the
*recommended* backend because it sidesteps DynamoRIO's signal and code-cache
collisions entirely (see [Language runtime support](#language-runtime-support)).

**Why it fits.** Both Intel PT and ARM CoreSight, after decode, yield exactly the
two dimensions `asmtest_trace_t` already carries: ordered instruction offsets and
distinct basic-block offsets, each as `off = ip - base` from a registered range.
Hardware records branch *decisions* only — it emits nothing per sequential
instruction — so the per-instruction and per-block stream is *reconstructed* by
the decoder replaying asm-test's own registered code bytes between branch
waypoints. asm-test always holds those bytes, so the one hard precondition is
always met. Overflow (PT `OVF` / `PERF_AUX_FLAG_TRUNCATED`, CoreSight overflow)
maps onto the existing `truncated` bit, so no new public trace shape is needed.

**Backends.**

- **Intel PT.** `perf_event_open` with `attr.type` from
  `/sys/bus/event_source/devices/intel_pt/type` and `pid=0` self-traces the
  calling thread; a second `mmap` exposes the AUX buffer the CPU fills with
  TNT/TIP/PSB packets. libipt decodes them against the registered bytes
  (`pt_image_add_cached` over `[base, base+len)`): `pt_insn_next` yields ordered
  instructions for `insns[]`; the block decoder's `pt_block.ip` + `ninsn` feed
  `blocks[]`. A read-write AUX mapping is a linear buffer that must be drained
  (overflow is dropped and flagged); a read-only mapping is circular, the basis
  of low-overhead snapshot mode.
- **ARM CoreSight.** `perf_event_open` for `cs_etm` (per-thread) captures ETM/ETE
  waypoint trace to a sink (TMC-ETR/ETB, or a per-CPU TRBE on ARMv9 ETE);
  OpenCSD deformats and decodes it against the same bytes into instruction
  ranges, giving `insns[]`/`blocks[]`. `FEAT_TRF` exception-level filtering
  (`E0TRE`) scopes a `/u` capture to EL0 userspace, cutting runtime/kernel noise.

**Region mapping (the ergonomic win).** The registered `[base, base+len)` maps to
one *hardware* address-range filter — Intel PT's `IA32_RTIT_ADDRn_A/B` comparators
(Linux `perf --filter 'filter <off>/<len>@<obj>'`), or CoreSight's ETM/ETE address
comparators with identical `perf --filter` syntax — so the CPU emits packets only
inside the region with no inserted instrumentation at all. The begin/end markers
realize two equivalent styles: *range* style installs the filter and begin/end
merely `ioctl(PERF_EVENT_IOC_ENABLE/DISABLE)` the AUX capture around the call
(preferred for a single registered routine); *address* style uses hardware
`start`/`stop` on the marker PCs to bracket a sub-range. The markers stay real
exported functions so address style can name their PCs and the API stays
backend-neutral, but `drwrap` is *not* needed here. Per-thread scoping is just
`pid=0`, replacing the DynamoRIO per-thread active-region stack.

Two limits drive a software fallback. Hardware comparators are few — Intel PT
reports its count via CPUID leaf `14H` `RANGECNT` / `nr_addr_filters` (often 2),
CoreSight a small fixed number — so beyond that, distinct simultaneous regions
fall back to *software* post-filtering of decoded IPs by `[base, base+len)`.
Keystone-generated executable memory (Phase 4) has no backing object file, but
`perf` filter addresses are object-relative, so generated regions also use the
software post-filter rather than a hardware filter.

**Decode and annotation reuse.** libipt (PT) and OpenCSD (CoreSight) are *new*
optional *decoders* — the reverse of Capstone: they turn the hardware packet
stream into the offset stream. The existing Capstone layer (`src/disasm.c`,
`emu_trace_report_disasm` and friends) is reused *unchanged* to *annotate* those
offsets, so Unicorn, DynamoRIO, PT, and CoreSight share one report format and no
hardware-specific decoder leaks into language bindings. The registered bytes flow
twice from one source: into libipt/OpenCSD to reconstruct offsets, then into
Capstone to render text.

**Deliverables.**

- `include/asmtest_hwtrace.h`: an `asmtest_trace_backend_t` enum
  (`INTEL_PT`, `CORESIGHT`), an options struct (backend, AUX/DATA buffer sizes,
  snapshot-vs-linear mode, optional object-file hint), and
  `asmtest_hwtrace_available/init/register_region/shutdown` plus the shared
  `asmtest_trace_begin/end` markers.
- `src/pt_backend.c` (libipt) and `src/cs_backend.c` (OpenCSD), each its own
  translation unit, plus a Linux-only perf-AUX capture helper.
- `examples/test_hwtrace.c`: a smoke test that self-skips when unavailable.
- Makefile knobs `LIBIPT_CFLAGS/LIBIPT_LIBS` and `OPENCSD_CFLAGS/OPENCSD_LIBS`
  (pkg-config auto-detect, mirroring `CAPSTONE_*`), defines `-DASMTEST_HAVE_LIBIPT`
  / `-DASMTEST_HAVE_OPENCSD`, and targets `hwtrace-test` and `shared-hwtrace`
  (a separate `libasmtest_hwtrace`, never linked by core or `libasmtest_emu`).
- `asmtest_hwtrace_available()`: the detect-and-skip routine encoding the full
  gating chain (decoder lib present, PMU node present, right CPU/ISA/vendor, perf
  privilege, not a restricted VM guest).

**Acceptance.**

- `make hwtrace-test` self-skips with a clear message — the common case — when the
  decoder library, the `intel_pt`/`cs_etm` PMU, the right CPU/host, or the
  `perf_event` privilege is absent.
- On a bare-metal Intel x86-64 Linux host with PT and `perf_event_paranoid`
  lowered, tracing a registered routine (or a Phase 4 Keystone host-native
  routine) records block offset `0` and a deterministic ordered `insns[]` that
  matches the Unicorn/DynamoRIO output for the same code.
- A tiny AUX buffer or a hot loop sets `truncated` via overflow.
- CoreSight acceptance is identical, explicitly opt-in on a named bare-metal
  AArch64 board.

**Licensing.** libipt is BSD and OpenCSD is BSD-3-Clause, so this tier carries no
LGPL obligation (unlike `drwrap`). OpenCSD is C++, unlike the otherwise-C optional
tiers; keep it isolated in its own translation unit and the separate
`libasmtest_hwtrace`.

**CI.** Hardware trace cannot run on standard CI: GitHub-hosted runners are
virtualized Azure VMs that do not expose Intel PT to guests, and cloud ARM exposes
no `cs_etm`. A real job needs a *self-hosted* bare-metal Intel x86-64 (PT) or
known-good AArch64 board (CoreSight) runner with `perf_event_paranoid` lowered —
an explicit, separate, allowed-to-be-absent job that never gates normal tests.

**Effort.** PT capture + libipt decode 5-8 days on available hardware; CoreSight +
OpenCSD a further 5-8 days, gated on board access. Both depend on Phase 1
(substrate) and Phase 5 (instruction-mode semantics).

**Validation (hardware trace).** Intel PT ships since Broadwell and
Goldmont/Apollo Lake; the Linux `intel_pt` PMU since ~4.3, with address-range
filtering added in 4.7. `CAP_PERFMON` (Linux 5.8+) or a low enough
`perf_event_paranoid` is required, and a usable default-size AUX capture for an
unprivileged process effectively needs `perf_event_paranoid = -1` or
`CAP_PERFMON` — a host knob the process cannot set itself. PT is in practice
unavailable on standard cloud VMs and GitHub-hosted runners (the hypervisor does
not expose it), though PT is itself virtualizable when the host opts in. ARM
`FEAT_ETE` requires `FEAT_TRF` and can pair with either a `TRBE` sink or a legacy
CoreSight sink (ETR/ETB); both `FEAT_ETE` and `FEAT_TRBE` are optional from
Armv9.0. KVM clears `TRFCR_EL1` and does not advertise the trace filter to guests,
so cloud ARM VMs (Graviton, Altra) cannot self-host trace, and Apple Silicon does
not expose CoreSight to the OS; self-hosted capture is realistic only on specific
dev boards/phones (Juno, ZCU102/Kria, Jetson, Pixel) with `CONFIG_CORESIGHT*` and
`perf` built against OpenCSD.

- Intel PT (Linux perf): https://perf.wiki.kernel.org/index.php/Perf_tools_support_for_Intel%C2%AE_Processor_Trace
- libipt decoder library: https://github.com/intel/libipt
- ARM CoreSight / cs_etm (Linux perf): https://www.kernel.org/doc/html/latest/trace/coresight/coresight.html
- OpenCSD decoder library: https://github.com/Linaro/OpenCSD

---

## Language runtime support

In-process DynamoRIO attach is robust for a plain native process; the difficulty
is entirely about **managed language runtimes**. This section records the root
cause, the design reframe that makes it tractable, and a per-runtime fix matrix.
It was derived from the DynamoRIO, HotSpot, CoreCLR, V8, and CPython sources
listed at the end.

**Root cause — three collisions.** A managed runtime fights in-process DR on:

1. **Signal ownership.** DR intercepts *all* signals by default
   (`-intercept_all_signals`), installs one process-wide master handler,
   virtualizes the app's `sigaction`, and **never blocks SIGSEGV/SIGBUS** (it
   needs them for safe-reads and code-cache consistency). Every managed runtime
   *also* uses SIGSEGV on a hot correctness path (null checks, safepoint polls,
   WASM bounds checks, hardware→managed exception translation). Both want to be
   the primary SIGSEGV owner.
2. **JIT/GC vs. DR code-cache consistency (the dominant one).** To keep its code
   cache correct, DR write-protects W+X pages and catches the resulting SIGSEGV to
   flush stale fragments — the *same* mprotect+fault mechanism a JIT emitter and a
   generational-GC write barrier use. JITs generate, move (V8 compacting GC), and
   free/reuse code addresses at runtime, so DR thrashes (RO ↔ sandbox).
3. **Pre-existing threads + takeover.** `dr_app_start` takes over **all** threads
   and assumes they share signal handlers (pthreads). Managed runtimes are
   already multithreaded before attach (GC, finalizer, diagnostics, libuv pool,
   background JIT), and DR has open bugs in the takeover/signal window.

**The reframe.** Recording a narrow region is **not** the same as instrumenting a
narrow region: once `dr_app_start` runs, DR translates *everything* on every
taken-over thread. Three things shrink the blast radius:

- **Bracket the active window** with `start`/`stop` around only the call into the
  registered routine, so the runtime runs natively almost always (see the
  bracketed-takeover note under Public API).
- **Chain signals, don't swallow them**: the client registers
  `dr_register_signal_event` and returns `DR_SIGNAL_DELIVER` so the runtime's own
  SIGSEGV path still fires (`-no_intercept_all_signals` does **not** stop DR
  intercepting SIGSEGV; there is no `-no_sigsegv`).
- **The saving grace**: asm-test's traced bytes are Keystone-emitted into
  asm-test's **own** `mmap` region (Phase 4), not the runtime's GC-managed JIT
  heap, so they never move or get freed and never trigger collision #2. The
  collision only comes from runtime code running *while DR is started* — exactly
  what bracketing minimizes. CPython wins because the GIL means no concurrent
  native code and the default build has **no JIT**, so a bracketed window is
  genuinely quiet; JVM/.NET/V8 run concurrent background GC/JIT, so even a small
  window can catch code churn on another taken-over thread.

**Per-runtime fix matrix.**

| Runtime | Core conflict | Concrete fix | Verdict |
|---|---|---|---|
| **CPython** | Almost none by default | Init on the main thread at `Py_Initialize` (or `Py_InitializeEx(0)` to skip CPython's signal setup); keep `faulthandler`/Dev Mode off; use `forkserver`/`spawn` + `os.register_at_fork(after_in_child=disable)`; avoid the free-threaded (PEP 703) and `--enable-experimental-jit` (`PYTHON_JIT=1`) builds | **Viable / robust** — vindicates the Phase 6 Python-first choice |
| **Node / V8** | JIT generates+moves+frees code; SIGSEGV owned by the WASM trap handler; libuv+V8 threads start early | Trace only non-V8 native regions (a `.node` addon or asm-test's own `mmap`'d code — already the model); `--jitless` + `--predictable --single-threaded-gc` + `--disable-wasm-trap-handler`; tune `UV_THREADPOOL_SIZE`, avoid `worker_threads` | **Conditional** — only with V8 locked down or tracing strictly outside V8's heap |
| **JVM** | SIGSEGV = null checks + safepoint polls; tiered JIT flushes the code cache | `LD_PRELOAD=libjsig.so` (the sanctioned signal-chaining broker) + `-Xrs`; trace only native/JNI leaf code (avoid the code cache and poll pages); `-XX:+PreserveFramePointer`; never cache JITed addresses across `CompiledMethodUnload` (code is invalidated+reused, not moved); last resort `-Xint` | **Best-effort** — async-profiler's JVMTI+libjsig model is the supported analogue |
| **.NET (CoreCLR)** | SIGSEGV = null-ref + stack-overflow + HW→managed translation; **no libjsig equivalent**; OSR/tiered/ALC-unload churn addresses | Bootstrap from **native** before EE startup (managed `Main` is already too late — finalizer/GC/EventPipe threads exist); `DOTNET_TieredCompilation=0`, avoid OSR (`DOTNET_TC_QuickJitForLoops=0`) and collectible `AssemblyLoadContext`; `DOTNET_DefaultDiagnosticPortSuspend=1` for a clean attach window; bracket with `dr_app_stop` around managed code | **Hardest / not robust** — prefer out-of-process or hardware trace |

**.NET is strictly harder than the JVM** for one specific reason: HotSpot ships
`libjsig.so` to chain a third-party SIGSEGV handler behind its own, but CoreCLR
has **no** such facility — it captures the previous handler once at init and
forwards only to that, so either install order loses. Existence proof and its
price: `pyda` runs CPython *as* a DR client but needed "nontrivial patches for
both DynamoRIO and CPython."

**Consequence for the plan.** The DynamoRIO tier targets **CPython + native/C
callers**. For **JVM/.NET/Node**, prefer the **Phase 9 Intel PT backend**: it
reads branch packets without intercepting signals or perturbing JITed code,
sidestepping all three collisions. This reframes Phase 9 from an optional fast
path into the *correct* backend for the hard managed runtimes (where bare-metal
PT is available).

**Sources.** DynamoRIO [dr_app.h](https://dynamorio.org/dr__app_8h.html),
[transparency.html](https://dynamorio.org/transparency.html),
[signal.c](https://github.com/DynamoRIO/dynamorio/blob/master/core/unix/signal.c),
[optionsx.h](https://raw.githubusercontent.com/DynamoRIO/dynamorio/master/core/optionsx.h),
[pyda](https://github.com/ndrewh/pyda); HotSpot
[signals_posix.cpp](https://github.com/openjdk/jdk/blob/master/src/hotspot/os/posix/signals_posix.cpp),
Oracle [signal-chaining](https://docs.oracle.com/en/java/javase/11/vm/signal-chaining.html),
[async-profiler](https://github.com/async-profiler/async-profiler); CoreCLR
[PAL signal.cpp](https://github.com/dotnet/runtime/blob/main/src/coreclr/pal/src/exception/signal.cpp),
[runtime#43642](https://github.com/dotnet/runtime/issues/43642),
[code-versioning](https://github.com/dotnet/runtime/blob/main/docs/design/features/code-versioning.md);
V8 [jitless](https://v8.dev/blog/jitless),
[embedded-builtins](https://v8.dev/blog/embedded-builtins),
[trap-handler.h](https://github.com/v8/v8/blob/master/src/trap-handler/trap-handler.h);
CPython [signal](https://docs.python.org/3/library/signal.html),
[PEP 744](https://peps.python.org/pep-0744/),
[multiprocessing](https://docs.python.org/3/library/multiprocessing.html).

---

## Risks and open points

- **Client-loading in app-api mode.** The first spike must settle whether dynamic
  `dr_register_process`/configuration is sufficient for language wrappers, or
  whether a static-client build is needed for reliable in-process attach.
- **Language runtime fragility.** Python, Node, JVM, and .NET each collide with
  in-process DR in runtime-specific ways (signal ownership, JIT/GC code-cache
  consistency, threads started before attach). This is large enough to have its
  own section: see [Language runtime support](#language-runtime-support) for the
  root cause, the record-window-vs-instrument-window reframe, and the per-runtime
  fix matrix with viability verdicts. Summary: CPython is robust; JVM/.NET/Node
  are best-effort and should prefer the Phase 9 hardware-trace backend.
- **Thread semantics.** The MVP should record only the current region-entering
  thread. All-thread tracing needs explicit API and locking.
- **Private loader transparency.** Do not share C globals between app and client.
  Use marker calls and app-owned buffers.
- **Licensing.** `drwrap` (LGPL 2.1) is accepted in the MVP because the marker
  argument-capture mechanism depends on it (see Phase 2); the native trace tier
  ships as a separate optional artifact, so this does not affect the core or
  `libasmtest_emu`. `drsyms` carries its own licensing/packaging considerations
  and stays deferred until the optional symbol mode (Phase 7) needs it.
- **drwrap robustness (symbol mode).** `drwrap` post-callbacks are not guaranteed
  on abnormal exits (`longjmp`, exceptions) — unwind detection is documented as
  best-effort (`DRWRAP_UNWIND_ON_EXCEPTION` helps but is not guaranteed). A
  symbol-mode region (Phase 7) can be entered and never closed, leaking an open
  activation on the shared per-thread stack; treat symbol mode as best-effort and
  keep explicit begin/end markers the robust path. Symbol-mode toggling pushes/
  pops the **same** per-thread stack as manual markers and inherits the Phase 2
  balance contract.
- **ABI stability.** The `asmtest_drtrace.h`/`asmtest_trace.h` C ABI churns
  through Phases 0-5 (options struct, lifecycle, modes). It is declared **frozen
  at the end of Phase 5**; the static-language bindings (Phase 6 step 3) start
  only then.
- **macOS and AArch64.** DynamoRIO docs and build notes are strongest for Linux
  and Windows. Start Linux x86-64 only; treat macOS/AArch64 as follow-up.
- **Fault model.** Native faults remain native faults. Continue using asm-test
  guard pages and runner isolation where possible; do not promise Unicorn-style
  precise guest faults.
- **Overhead.** Basic-block coverage should be the default. Instruction and
  memory-event modes must be opt-in.
- **Hardware-trace availability (Phase 9).** Intel PT is Intel-x86-64-only and
  absent on AMD, ARM, Apple Silicon, and almost all cloud/VM/GitHub-hosted
  guests; CoreSight self-hosted trace is realistic only on specific bare-metal
  AArch64 boards and is prohibited in KVM guests. The tier is opt-in and cannot
  be a default or a portable CI gate.
- **Hardware-trace privilege (Phase 9).** Both backends need `perf_event_paranoid`
  lowered (effectively `-1` for a default-size PT buffer) or
  `CAP_PERFMON`/`CAP_SYS_ADMIN`, plus `perf_event_mlock_kb`/`RLIMIT_MEMLOCK` for
  large AUX buffers — host knobs the process cannot grant itself.
  `asmtest_hwtrace_available()` must detect-and-skip on exactly this.
- **Hardware-trace fidelity (Phase 9).** Decode is impossible without the exact
  registered code bytes (libipt returns `-pte_nomap`); self-modifying or relocated
  code silently corrupts offsets. Speculative/aborted paths
  (`pt_block.speculative`) must be filtered before recording. libipt block
  boundaries are not identical to Unicorn/DynamoRIO basic blocks (a libipt block
  can span direct branches), so cross-backend block-offset parity needs a
  normalization step or a documented difference. Finite comparators and file-less
  Keystone memory force a software IP post-filter fallback for some regions.

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
