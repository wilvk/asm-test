# Native runtime tracing

asm-test has three ways to observe which code a routine actually executes. The
[emulator trace model](traces.md) records coverage inside Unicorn's **virtual
CPU**. This page covers the two **native** tiers, which trace code as it runs on
the **real CPU, in-process**:

| Tier | Backend | Records | Runs where |
|---|---|---|---|
| Emulator trace ([traces.md](traces.md)) | Unicorn | guest blocks + instructions | any host |
| **DynamoRIO native trace** | DynamoRIO (software DBI) | native blocks + instructions | Linux x86-64 |
| **Hardware trace** | Intel PT / ARM CoreSight | native blocks + instructions | bare-metal Intel / AArch64 |

All three fill the **same** `asmtest_trace_t` shape (ordered instruction offsets,
distinct basic-block offsets, totals, a truncation bit) — see
[asmtest_trace.h](traces.md). A test can switch backends without changing how it
reads coverage, and the optional [Capstone annotation layer](disassembly.md)
renders any backend's recorded offsets back to instruction text.

Both native tiers are **optional, advanced, and self-skipping**: they are kept out
of the core `libasmtest` and the `libasmtest_emu` superset, build only when their
toolchain is present, and degrade to a clear "skipped" message otherwise.

---

## DynamoRIO vs Unicorn

The emulator tier copies a routine's bytes into an **isolated** virtual CPU and
runs them there: it gives precise faults-as-data, cross-ISA execution, and
register/memory preloading, but it never touches the host CPU. The DynamoRIO tier
is the opposite trade: it traces code running **natively inside the real process**
— native or Keystone-generated host-native machine code — so what you measure is
what the CPU actually did, at native register/ABI fidelity.

Use the emulator tier for cross-ISA work, isolation, and fault injection; use the
DynamoRIO tier when you specifically want to trace real in-process native
execution. DynamoRIO does **not** replace Unicorn (no cross-ISA, no isolated
guest memory, no faults-as-data).

---

## DynamoRIO tier

### Architecture

Two cooperating libraries (built by `make shared-drtrace` and
`make drtrace-client`):

- **`libasmtest_drapp`** — the app-facing API ([asmtest_drtrace.h](api-reference.md)).
  It owns the lifecycle state machine, exposes the begin/end region markers, and
  brings DynamoRIO up in-process.
- **`libasmtest_drclient.so`** — a DynamoRIO client loaded in-process that observes
  the markers, instruments registered code ranges, and reconciles coverage into
  the app-owned trace.

The client uses **only DynamoRIO's BSD core API** (`dr_register_bb_event`, clean
calls, `dr_get_proc_address`) — deliberately not the drmgr/drwrap/drreg
extensions. Two reasons: the prebuilt release extensions fail to load under
DynamoRIO's private loader on modern glibc, and avoiding `drwrap` keeps the tier
free of its **LGPL-2.1** obligation (DynamoRIO core is BSD). The begin/end/register
functions are real exported symbols; the client resolves their PCs and inserts
clean calls that read the SysV argument registers — the "drmgr-only" scheme,
chosen here for robustness.

### Startup lifecycle

DynamoRIO reads its client configuration from `DYNAMORIO_OPTIONS` at
**library-load time** (its constructor runs before `main`). So `libasmtest_drapp`
does **not** link `libdynamorio`; instead `asmtest_dr_init()` sets the options and
only then `dlopen()`s `libdynamorio`, so the constructor sees the freshly-set
client path. The lifecycle is an enforced state machine:

```
UNINIT --init--> INIT --start--> STARTED --stop--> STOPPED --shutdown--> SHUTDOWN
                                    ^                  |
                                    +------- start ----+
```

```c
#include "asmtest_drtrace.h"

asmtest_drtrace_options_t opts = {0};
opts.client_path = "build/libasmtest_drclient.so"; /* or $ASMTEST_DRCLIENT */
asmtest_dr_init(&opts);   /* dr_app_setup + configure client (no takeover yet) */
asmtest_dr_start();       /* dr_app_start  (DynamoRIO takes over)              */
/* ... register regions, trace ... */
asmtest_dr_shutdown();    /* dr_app_stop_and_cleanup (back to native)         */
```

`start` must run on the same thread as `init` (DynamoRIO requires it); out-of-order
calls return a defined error rather than invoking DynamoRIO (whose ordering
violations are undefined behaviour). Inside a JIT/GC-heavy runtime the supported
model is to keep DynamoRIO **stopped** and bracket only the call into the routine
with `start`/`stop` (see [Language runtimes](#language-runtimes)).

### Marker regions and coverage

Register a non-overlapping native code range and bracket the call with begin/end
markers; coverage accumulates into the app-owned trace:

```c
asmtest_trace_t *tr = asmtest_trace_new(/*insns*/0, /*blocks*/64);
asmtest_dr_register_region("add2", code_base, code_len, tr);

asmtest_trace_begin("add2");
long r = ((long (*)(long, long))code_base)(20, 22);
asmtest_trace_end("add2");

assert(asmtest_trace_covered(tr, 0));   /* entry block covered */
```

Markers must be balanced; `asmtest_dr_marker_error()` reports any `end` without a
matching `begin`. A registered range is instrumented unconditionally but **records
conditionally** — the same bytes also run outside a begin/end window — so recording
is gated per-thread by whether a region is active.

### Host-native generated code

DynamoRIO traces code running natively, so it needs real executable host memory —
distinct from the emulator's guest load address. `asmtest_exec_alloc()` maps
**W^X-correct** executable memory (`mmap` `PROT_NONE` → `mprotect` RW to copy →
`mprotect` RX, icache flushed) for raw machine-code bytes; `asmtest_asm_exec_native()`
assembles host-native assembly text with Keystone (two-pass, at the real run
address so PC-relative targets resolve) and materializes it the same way. Free with
`asmtest_exec_free()` (unregister the range first so the client drops its cached
translation).

```c
asmtest_exec_code_t code;
asmtest_exec_alloc(bytes, len, &code);          /* raw bytes */
/* or, with Keystone: asmtest_asm_exec_native("mov rax, rdi\nadd rax, rsi\nret", 0, &code); */
asmtest_dr_register_region("gen", code.base, code.len, tr);
```

### Block vs instruction mode

Block coverage is the default and cheap. Instruction mode (ordered offsets) is
opt-in per trace: allocate the trace with `insns_cap > 0`
(`asmtest_trace_new(64, 64)`) and the client records each executed instruction's
offset. It is heavier; treat it as diagnostic rather than the default coverage
path.

### Language wrappers

**Every** binding ships a native-trace wrapper exposing the **same** small
surface: a process-wide `NativeTrace` lifecycle (`available`, `initialize`,
`shutdown`, `marker_error`), per-trace recorders (`register`, a scoped begin/end
`region`, `covered`, block/instruction totals), and a `NativeCode` for
materializing host-native bytes and calling into them. Each wrapper **dlopens
`libasmtest_drapp` at run time** (resolved from `$ASMTEST_DRAPP_LIB`, else the
repo's `build/`) and reads `$ASMTEST_DRCLIENT` for the client `.so`, so the core
binding never link-depends on DynamoRIO and `available()` returns false — a clean
self-skip — wherever the tier isn't built.

| Language | Wrapper module / header | Test target |
|---|---|---|
| Python | `asmtest.drtrace` | `drtrace-python-test` |
| C++ | `bindings/cpp/asmtest_drtrace.hpp` (`asmtest::NativeTrace`) | `drtrace-cpp-test` |
| Rust | `asmtest::drtrace` | `drtrace-rust-test` |
| Go | `asmtest` (`drtrace.go`) | `drtrace-go-test` |
| Node | `bindings/node/drtrace.js` | `drtrace-node-test` |
| Java | `DrTrace` (Panama FFM) | `drtrace-java-test` |
| .NET | `Asmtest.DrTrace` / `Asmtest.NativeTrace` | `drtrace-dotnet-test` |
| Ruby | `Asmtest::DrTrace` | `drtrace-ruby-test` |
| Lua | `bindings/lua/drtrace.lua` | `drtrace-lua-test` |
| Zig | `bindings/zig/src/drtrace.zig` | `drtrace-zig-test` |

The surfaces are deliberately idiomatic per language (RAII scope guards in C++ /
Rust, a `region(name, fn)` callback in Go / Node / Lua, `try`/`finally` markers
in Java / .NET, a `region {}` block in Ruby) but map one-to-one onto the same C
entry points. A few names dodge language keywords — Ruby uses
`NativeTrace.start`/`.create` (`initialize`/`new` are reserved) and C++ uses
`create` (not `new`).

All ten wrappers are verified against a **real in-process DynamoRIO** in Docker
(`make docker-drtrace-bindings`). The C++, Ruby, Java, Lua, Zig, Rust, and Go
lanes trace live and assert coverage; **Node and .NET self-skip** there, because
in-process DynamoRIO can't take over a JIT/GC runtime's background threads
(`dr_app_start` aborts with *"Failed to take over all threads"*). That is the
[managed-runtime](#language-runtimes) limitation — the wrappers themselves are
complete and identical in surface; the recommended backend for Node/.NET (and a
flaky JVM) is hardware-trace (Intel PT), which observes out-of-band.

Python is the canonical reference. Build the libraries
(`make shared-drtrace drtrace-client DYNAMORIO_HOME=...`) and point the env at
them (`ASMTEST_DRCLIENT`, and `ASMTEST_DR_LIB` or `dynamorio_home=`):

```python
from asmtest.drtrace import NativeTrace, NativeCode

if NativeTrace.available():
    NativeTrace.initialize(client="build/libasmtest_drclient.so",
                           dynamorio_home="/opt/DynamoRIO")
    code = NativeCode.from_bytes(b"\x48\x89\xf8\x48\x01\xf0\xc3")  # mov rax,rdi; add rax,rsi; ret
    trace = NativeTrace.new(blocks=64, instructions=0)
    trace.register("add", code)
    with trace.region("add"):
        result = code.call(20, 22)
    assert result == 42 and trace.covered(0)
    NativeTrace.shutdown()
```

`make drtrace-<lang>-test DYNAMORIO_HOME=...` runs one binding's wrapper suite and
`make drtrace-bindings-test` runs them all. The Python module is named
`asmtest.drtrace` (not `asmtest.native`, which would collide with the private
ctypes loader `asmtest._native`).

### Running the tests

```sh
make drtrace-test DYNAMORIO_HOME=/path/to/DynamoRIO-Linux-<ver>   # C smoke
make drtrace-python-test DYNAMORIO_HOME=/path/to/DynamoRIO        # Python wrapper
make drtrace-rust-test   DYNAMORIO_HOME=/path/to/DynamoRIO        # one binding (any of the langs)
make drtrace-bindings-test DYNAMORIO_HOME=/path/to/DynamoRIO      # every language wrapper
make docker-drtrace                                              # C + Python, in Docker
make docker-drtrace-bindings                                     # every language wrapper, in Docker
```

Without `DYNAMORIO_HOME` every target self-skips with a clear message. The native
trace tests run as a **standalone `--no-fork`, single-job harness**, outside the
forking runner: in-process DynamoRIO attach is hostile to per-test `fork()`
isolation and the `-jN` pool.

---

## Hardware-trace tier (Intel PT / ARM CoreSight)

The hardware tier records the same `asmtest_trace_t` offsets with near-zero
*capture* overhead: the CPU emits a compressed branch-trace packet stream (Intel
PT, or ARM ETM/ETE waypoints) into a kernel AUX ring via `perf_event_open`, and
after the region runs a software decoder (**libipt** for PT, **OpenCSD** for
CoreSight) replays asm-test's own registered code bytes between the branch
waypoints to reconstruct the instruction and block streams. There is no DynamoRIO
or `drwrap` dependency (libipt and OpenCSD are BSD).

Hardware records branch *decisions* only, so the block partition a decoder yields
is **coarser** than Unicorn/DynamoRIO basic blocks (a decoded block can span
direct branches). The PT backend therefore **normalizes**: it walks the
reconstructed per-instruction stream and starts a new block at the first
instruction and after every branch — the same single-entry/ends-at-branch model
the other backends use — so block offsets match.

### Availability and self-skip

This tier is **mostly bare-metal-Linux and even-more-optional**. Intel PT is
Intel-x86-64-only — absent on AMD, ARM, Apple Silicon, and almost all cloud/VM/CI
guests; CoreSight self-hosted trace needs a specific AArch64 board (Juno, Kria,
Jetson, Pixel, …) and is prohibited in KVM guests. Both need
`perf_event_paranoid` lowered (effectively `-1` for a default-size PT buffer) or
`CAP_PERFMON` — a host knob the process cannot grant itself.

`asmtest_hwtrace_available(backend)` encodes the **full gating chain** — decoder
library present, right CPU/vendor, the `intel_pt`/`cs_etm` PMU present, and
`perf_event` capture permitted — and returns 0 (self-skip) otherwise, which is the
common case. `make hwtrace-test` self-skips with the specific reason
(`asmtest_hwtrace_skip_reason`), e.g. *"no intel_pt PMU (needs bare-metal Intel;
absent on AMD/VM)"*.

```c
#include "asmtest_hwtrace.h"

if (asmtest_hwtrace_available(ASMTEST_HWTRACE_INTEL_PT)) {
    asmtest_hwtrace_options_t opts = {.backend = ASMTEST_HWTRACE_INTEL_PT};
    asmtest_hwtrace_init(&opts);
    asmtest_hwtrace_register_region("add2", base, len, tr);
    asmtest_hwtrace_begin("add2");
    fn(20, 22);
    asmtest_hwtrace_end("add2");        /* captures + decodes into tr */
    asmtest_hwtrace_shutdown();
}
```

The Intel PT capture and libipt decode are implemented; the CoreSight backend is a
documented scaffold pending AArch64 board access (it always self-skips until
completed). The hardware tier **cannot run on standard CI** — it needs a
self-hosted bare-metal runner — so it ships with materially weaker automated
regression protection than the Unicorn tier, by design.

---

## Language runtimes

In-process DynamoRIO attach is robust for a plain native process; the difficulty
is entirely about **managed language runtimes**, which collide with in-process
DynamoRIO on three fronts: signal ownership (both want SIGSEGV), JIT/GC vs DR
code-cache consistency, and pre-existing threads at takeover.

- **CPython** is the supported managed target (GIL-serialized, no default JIT):
  init on the main thread, keep `faulthandler` off, use `forkserver`/`spawn` +
  `os.register_at_fork`, and avoid the free-threaded and experimental-JIT builds.
- **JVM / .NET / Node** run concurrent background JIT/GC and are best-effort. Prefer
  the **hardware-trace (Intel PT) backend** for these where bare-metal PT is
  available: it observes out-of-band without intercepting signals or perturbing
  JITed code, sidestepping all three collisions.

The full root-cause analysis and per-runtime fix matrix live in the
[DynamoRIO native-trace plan](https://github.com/wilvk/asm-test/blob/main/docs/plans/dynamorio-native-trace-plan.md#language-runtime-support).

---

## Known limitations

- **Linux x86-64 only** for now (macOS/AArch64 are follow-ups).
- **Native faults stay native faults** — no Unicorn-style precise guest faults.
  Continue using guard pages and runner isolation where possible.
- **Non-overlapping regions** in the MVP; the active region is per-thread.
- **Concurrency contract (MVP).** Register all regions *before* tracing starts: the
  DynamoRIO client reads its region table unlocked on the per-basic-block hot path
  (locking every block would defeat the cheap-default design), so registering or
  unregistering a region while another thread is executing traced code is
  unsupported. Each `asmtest_trace_t` is bound to one thread's region activation —
  traces are not thread-safe shared collectors; do not read a trace, or enter its
  region on another thread, while a thread is inside it. (The hardware-trace tier
  is single-active-region in the MVP — see its header.)
- **fork after `start` is unsupported.** DynamoRIO takes over all threads; forking
  while started (e.g. Python `multiprocessing` with the default `fork` start
  method) is not supported — use `forkserver`/`spawn`, or fork before `start`.
- **DynamoRIO must be installed separately** (no pkg-config); set `DYNAMORIO_HOME`.
  It is a software DBI engine and runs on Intel and AMD alike.
- **Hardware capture is unverifiable off bare metal** — it cannot run on AMD, VMs,
  or standard CI; that is expected and the tier self-skips.
