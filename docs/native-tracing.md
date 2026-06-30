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
| **Single-step trace** | EFLAGS.TF → SIGTRAP (debug exception) | native blocks + instructions | **any x86-64 Linux** (no PMU/perf/privilege) |

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

### Symbol mode (trace a named function, no markers)

When the code under test is a **named exported function**, register it by symbol
instead of bracketing every call site. `asmtest_dr_register_symbol(name, max_len,
trace)` has the client resolve `name`'s entry PC (via `dr_get_proc_address`, across
all loaded modules) and records every execution of blocks in `[entry, entry +
max_len)` — recording is **always-on** for the range, so there are no begin/end
markers to balance:

```c
asmtest_trace_t *tr = asmtest_trace_new(/*insns*/0, /*blocks*/64);
asmtest_dr_register_symbol("asmtest_symbol_demo", 256, tr);

long r = asmtest_symbol_demo(3, 4);     /* no begin/end — just call it */

assert(r == 10 && asmtest_trace_covered(tr, 0));
asmtest_dr_unregister_region("asmtest_symbol_demo");   /* unregister by the same name */
```

`asmtest_symbol_demo` is a small exported fixture (computes `a*2 + b`) that ships in
`libasmtest_drapp` so every language binding shares one resolvable symbol for its
symbol-mode test. `max_len` bounds the function — pass its size or a generous upper
bound; registered ranges must not overlap. Symbol mode is best-effort: explicit
markers remain more robust for inlined or generated code, where a symbol may not have
a single stable entry PC. Every language wrapper exposes it as `register_symbol(...)`
(plus a `symbol_demo(a, b)` helper that calls the fixture).

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

Two accessors read the recorded offsets back as lists (every language wrapper
exposes them under these names):

- `block_offsets()` — the distinct basic-block start offsets, in first-seen order.
  Backed by `blocks_len()` + the per-index `block_at` accessor.
- `insn_offsets()` — the ordered instruction-offset stream actually stored (one
  entry per executed instruction, in execution order). Its length is `insns_len`,
  which is **capped at the trace's `insns_cap`** and so can be smaller than
  `insns_total()` (the uncapped count of instructions executed). Empty unless the
  trace was allocated in instruction mode.

For the two-block routine `mov; add; cmp; jle; dec; ret` called so the `jle` is
taken (the `dec` skipped), `insn_offsets()` is exactly `[0x0, 0x3, 0x6, 0xc,
0x11]` and `block_offsets()` contains the entry block `0` — the values the binding
tests assert.

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
completed). The Intel-PT/CoreSight hardware capture **cannot run on standard CI** —
it needs a self-hosted bare-metal runner. The **single-step backend below removes
that limitation** for x86-64: it records the same offsets on any Linux x86-64 host,
including CI and containers, with full automated regression protection.

---

## Single-step tier (EFLAGS.TF — the universal x86-64 backend)

The single-step backend (`ASMTEST_HWTRACE_SINGLESTEP`) is the **portable** member of
the hardware tier: it produces the **same exact, complete** `asmtest_trace_t` offsets
as Intel PT — byte-for-byte the Unicorn/DynamoRIO instruction and block streams — but
needs **no PMU, no `perf_event`, no privilege, no decoder library, and no specific
CPU**. It runs on **any x86-64 Linux host**: Intel, AMD of any generation (including
Zen 2, which has no branch-trace facility at all), VMs, standard CI, and a **plain
unprivileged container** (no `--privileged`, no `CAP_PERFMON`, no seccomp changes).

### How it works

With `EFLAGS.TF` set, the CPU raises a trap-class `#DB` **after every instruction**,
which Linux delivers as `SIGTRAP`. `asmtest_hwtrace_begin(name)` installs a `SIGTRAP`
handler and arms `TF` on the calling thread; the handler records each in-region `RIP`
(offsets outside the registered region — callees, the begin/end glue — are stepped but
not recorded). `asmtest_hwtrace_end(name)` clears `TF`, restores the handler, and runs
a post-pass that derives the block partition from fall-through discontinuities (a new
block at the entry and after every taken branch), using the Capstone length-decoder —
the same single-entry/ends-at-branch model the other backends use. The handler itself
does only async-signal-safe work (a bounds-checked store of each offset); all Capstone
decoding happens in the post-pass, never in the handler.

The trade is **exact-cheap (a hardware trace ring) vs. exact-slow (a fault per
instruction)** — roughly a couple of microseconds per instruction. So single-step is
the right tool for **small registered routines** (asm-test's Tier-A common case): a
handful to a few hundred instructions cost microseconds to low milliseconds. It is
**not** for whole-program or hot-loop tracing — that is the DynamoRIO tier's job
(native-speed code cache, no per-step tax). Unlike AMD LBR there is **no depth
ceiling**: a loop of any length reconstructs exactly.

```c
#include "asmtest_hwtrace.h"

if (asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
    asmtest_hwtrace_options_t opts = {.backend = ASMTEST_HWTRACE_SINGLESTEP};
    asmtest_hwtrace_init(&opts);
    asmtest_hwtrace_register_region("add2", base, len, tr);
    asmtest_hwtrace_begin("add2");
    fn(20, 22);                         /* stepped on the real CPU */
    asmtest_hwtrace_end("add2");        /* trace already filled; blocks normalized */
    asmtest_hwtrace_shutdown();
}
```

`asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)` returns 1 on x86-64 Linux when
Capstone is linked (for block normalization), and self-skips elsewhere with a specific
reason (e.g. *"single-step backend is Linux x86-64 only (Windows/macOS planned)"*).
The supported target is a **well-behaved compute routine** — no in-routine
`POPF`/`IRET`/signal handlers or self-modifying code, which break naive stepping and
are flagged `truncated` rather than emitted as complete (the same "registered bytes
must be stable" contract the PT/AMD backends carry). Single active region, single
thread, like the rest of the tier.

### Running it

```sh
make hwtrace-test          # C smoke test — runs the single-step backend LIVE here
make hwtrace-bindings-test # every language wrapper, live (Python + the nine others)
make docker-hwtrace        # C + Python in a PLAIN unprivileged container
make docker-hwtrace-bindings  # every language wrapper, in plain containers
```

`make hwtrace-test` executes the single-step backend live on this host (where Intel PT
and AMD LBR self-skip), asserting the same `[0x0, 0x3, 0x6, 0xc, 0x11]` /
`{0, 0x11}` partition the other backends produce, plus a 20-trip loop (62 instructions,
past LBR's 16-branch window) to prove completeness. This is the hardware tier's first
regression that **runs** on standard CI instead of self-skipping.

### Out-of-process variant (W2 — ptrace)

The stepper above is **in-process**: it installs a `SIGTRAP` handler and sets `TF` on
its own thread, so the traced routine and the collector share one process. The
out-of-process sibling (`asmtest_ptrace.h`, `src/ptrace_backend.c`) gets the **same
exact offsets** a different way — a tracer **parent** `PTRACE_SINGLESTEP`s a forked
tracee that runs the routine, reading `RIP` from the child's register file at each
stop and reconstructing the trace in the parent (no shared memory — the parent
observes every step). Block normalization is byte-identical to the in-process stepper,
so the output matches every other backend.

```c
#include "asmtest_ptrace.h"

long args[2] = {20, 22}, result = 0;
asmtest_trace_t *t = asmtest_trace_new(64, 64);
/* `code` is host-native bytes already executable in this process (the forked child
 * inherits the mapping). The parent traces; *result is the routine's RAX at ret. */
asmtest_ptrace_trace_call(code, len, args, 2, &result, t);   /* result == 42 */
```

Why a second single-step path: an out-of-band tracer touches none of the tracee's
signal disposition or code cache, so it is the exact path for a **JIT/GC managed
runtime** (JVM/.NET/Node) — where the in-process stepper's `SIGTRAP`/`TF` collide with
the runtime, exactly as in-process DynamoRIO cannot take over its threads — and the
recommended managed-runtime path on **AMD** (no Intel PT). It is also the **only**
single-step form possible on **AArch64**, whose single-step bit (`MDSCR_EL1.SS`) is
kernel-only with no in-process form; this implementation is Linux/x86-64, riding the
same `PTRACE_SINGLESTEP` seam the ARM64 tracer will. The supported target is the same
deterministic pure-compute routine (≤6 integer args, no calls out to other regions) as
the in-process stepper. `make hwtrace-test` exercises it live (it works in a **plain
unprivileged container** — ptrace of one's own child needs no extra capability).

`trace_call` forks its own tracee, which is enough to trace a code blob out of process
but is not yet the managed-runtime case. The building block for *that* is
`asmtest_ptrace_trace_attached(pid, base, len, &result, trace)`: it traces a region in
a **separate, already-running process you have attached to from the outside** — the
foreign path. The caller owns the attach/detach policy (which PID, and when:
`PTRACE_ATTACH`/`PTRACE_SEIZE` then wait for the stop before the call, `PTRACE_DETACH`
after), because those are integrator decisions; asm-test single-steps the target from
its current stop, **reads the region bytes from the target via `process_vm_readv`** (so
the tracer need not share the target's memory — the honest foreign-process read), and
records the same in-region offsets. `make hwtrace-test` exercises it live: a child that
never called `PTRACE_TRACEME` is attached externally and its region traced to the exact
same `[0x0, 0x3, 0x6, 0xc, 0x11]` stream. This is the foundation a managed-runtime
tracer builds on (attach to the JVM/.NET/Node process, resolve the JIT code region from
its maps/jitdump, trace it out of band); resolving a live runtime's generated-code
image is the larger follow-on layered on top of this primitive.

### Language wrappers

Every binding ships an `hwtrace` wrapper alongside its `drtrace` one, exposing the same
small surface — `HwTrace.available/init/shutdown`, per-trace `register`/`region`/
`covered`/block+instruction accessors, and a `NativeCode` for materializing
host-native bytes via `asmtest_hwtrace_exec_alloc`. Each wrapper dlopens
`libasmtest_hwtrace` (resolved from `$ASMTEST_HWTRACE_LIB`, else the repo `build/`) and
defaults to the single-step backend, so `available()` is true and the test **traces
live** on any x86-64 Linux — including in CI and plain containers, where the DynamoRIO
wrapper needs a DynamoRIO install and the PT/AMD wrappers self-skip.

| Language | Wrapper module / header | Test target |
|---|---|---|
| Python | `asmtest.hwtrace` | `hwtrace-python-test` |
| C++ | `bindings/cpp/asmtest_hwtrace.hpp` (`asmtest::HwTrace`) | `hwtrace-cpp-test` |
| Rust | `asmtest::hwtrace` | `hwtrace-rust-test` |
| Go | `asmtest` (`hwtrace.go`) | `hwtrace-go-test` |
| Node | `bindings/node/hwtrace.js` | `hwtrace-node-test` |
| Java | `HwTrace` (Panama FFM) | `hwtrace-java-test` |
| .NET | `Asmtest.HwTrace` | `hwtrace-dotnet-test` |
| Ruby | `Asmtest::HwTrace` | `hwtrace-ruby-test` |
| Lua | `bindings/lua/hwtrace.lua` | `hwtrace-lua-test` |
| Zig | `bindings/zig/src/hwtrace.zig` | `hwtrace-zig-test` |

## Auto-selecting a backend (the hardware-tier cascade)

All four hardware backends fill the same `asmtest_trace_t`, self-skip cleanly via
`asmtest_hwtrace_available()`, and share one `truncated` completeness bit — so the
**most faithful available** one can be chosen for the host without hard-coding an
enum. Two front-end calls do that:

```c
#include "asmtest_hwtrace.h"

/* Pick the best available backend for this host, init it, trace. */
int b = asmtest_hwtrace_auto(ASMTEST_HWTRACE_BEST);   /* >=0 backend, or <0 status */
if (b >= 0) {
    asmtest_hwtrace_options_t opts = {.backend = (asmtest_trace_backend_t)b};
    asmtest_hwtrace_init(&opts);
    asmtest_hwtrace_register_region("fn", base, len, tr);
    asmtest_hwtrace_begin("fn");  fn(20, 22);  asmtest_hwtrace_end("fn");
    asmtest_hwtrace_shutdown();

    /* Dynamic fallback: if the backend could not record the whole path (AMD LBR
     * overflowed its 16-branch window, a PT ring overflowed), re-resolve to a
     * ceiling-free backend and re-run the routine. */
    if (tr->truncated) {
        int b2 = asmtest_hwtrace_auto(ASMTEST_HWTRACE_CEILING_FREE);
        if (b2 >= 0 && b2 != b) { /* re-init under b2, begin/call/end again */ }
    }
}
```

`asmtest_hwtrace_resolve(policy, out, cap)` returns the **whole** cascade — the
available backends most-faithful first (`Intel PT > AMD LBR > single-step >
CoreSight`) — and `asmtest_hwtrace_auto(policy)` is the convenience that returns just
the head (or `ASMTEST_HW_EUNAVAIL`). The `policy`:

- **`ASMTEST_HWTRACE_BEST`** — the most faithful backend the host can run.
- **`ASMTEST_HWTRACE_CEILING_FREE`** — the same, but skipping the one backend with a
  fixed completeness window (AMD LBR, 16 taken branches). This is what you re-resolve
  under after a trace comes back `truncated`, so the second attempt has no depth
  ceiling.

On **any x86-64 Linux host the cascade is non-empty** — the single-step backend is
the floor — so `auto()` never fails there; it only returns a negative status off
x86-64 Linux (and non-CoreSight). On a bare-metal Intel host `auto(BEST)` resolves
to Intel PT; on this Zen 5 dev box (Ryzen 9 9950X, `amd_lbr_v2`) with perf
permitted it resolves to **AMD LBR** (live-verified), with `CEILING_FREE` falling to
single-step; without perf access (or on a Zen 2, which has no branch facility) it
resolves to single-step.

**Every language wrapper exposes it too.** Alongside `available`/`init`, each
binding's `HwTrace` surfaces `resolve(policy)` (the available cascade, as a
list/array of backend enums) and `auto(policy)` (the single best pick, or
`ASMTEST_HW_EUNAVAIL` when none), with `BEST` / `CEILING_FREE` policy constants —
so a Python/Rust/Go/… caller picks the host's most-faithful backend without
hard-coding an enum, exactly as the C API does. The names are idiomatic per language
(C++ uses `auto_select`, since `auto` is a keyword; Rust/C# expose a `Policy` enum
rather than loose constants), but map one-to-one onto `asmtest_hwtrace_resolve` /
`asmtest_hwtrace_auto`. Each wrapper's hwtrace self-test asserts the selection
invariants (only-available, descending-fidelity order, `CEILING_FREE` ⊆ `BEST` and
excludes AMD LBR, `auto` == head) and, where single-step is available, runs a live
traced call through the auto-picked backend.

**Scope.** This orchestrates the *hardware tier's own* backends — one library, one
API. The DynamoRIO tier (`libasmtest_drapp`) and the Unicorn emulator are separate
libraries with their own call APIs, and a fall to the emulator crosses a fidelity
line (real CPU → isolated guest).

### Cross-tier orchestration (over all three tiers)

`asmtest_trace_resolve(policy, out, cap)` / `asmtest_trace_auto(policy, &choice)`
(`asmtest_trace_auto.h`, `src/trace_auto.c`) extend the cascade *across* tiers. They
walk the full descending-fidelity order — **Intel PT → AMD LBR → DynamoRIO →
single-step → CoreSight → emulator** — and return `asmtest_trace_choice_t`
descriptors `{tier, backend, fidelity}` rather than a bare backend enum. DynamoRIO
ranks **above** single-step because its code cache runs at native speed while
single-step pays a per-instruction kernel round-trip; that interleaving is why this
is a distinct front-end and not just the hardware cascade with two tiers appended.

```c
#include "asmtest_trace_auto.h"

asmtest_trace_choice_t pick;
if (asmtest_trace_auto(ASMTEST_TRACE_BEST, &pick) == ASMTEST_HW_OK) {
    /* pick.tier ∈ {HWTRACE, DYNAMORIO, EMULATOR}; pick.backend valid for HWTRACE;
     * pick.fidelity is NATIVE except the emulator floor (VIRTUAL). Dispatch to that
     * tier's own init/begin/end. */
}
```

It calls `asmtest_hwtrace_available()` directly and **dlopen-probes**
`libasmtest_drapp` (via `$ASMTEST_DRAPP_LIB`, else the default soname) for the
DynamoRIO tier — so it hard-links neither the DynamoRIO nor the emulator library,
keeping the three decoupled. The `policy` bitmask composes three controls:

- `ASMTEST_TRACE_BEST` — most-faithful available; the emulator floor is allowed.
- `ASMTEST_TRACE_CEILING_FREE` — drop the one fixed-window backend (AMD LBR); the
  policy to **re-resolve under after a trace comes back `truncated`**.
- `ASMTEST_TRACE_NATIVE_ONLY` — **forbid the native→emulator crossing**. The emulator
  floor is dropped, so a host with no native tier (e.g. macOS arm64) resolves to
  *nothing* (`ASMTEST_HW_EUNAVAIL`) rather than silently downgrading real-CPU
  execution to an isolated guest. This keeps the one fidelity-line crossing an
  explicit, opt-in decision — now a policy flag instead of hand-rolled per caller.

On any x86-64 Linux host the cascade is non-empty under `BEST` (the emulator is the
universal floor) and under `NATIVE_ONLY` (single-step is the native floor). **Every
language wrapper exposes it** as `resolve_tiers`/`auto_tier` (Python/Rust/Go/Lua/Ruby)
or `resolveTiers`/`autoTier` (C++/Node/Java/.NET/Zig), each with a self-test of the
cross-tier invariants.

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
- **Hardware capture needs bare metal + perf privilege.** Intel PT needs a
  bare-metal Intel host; AMD LBR needs a Zen 3+/4/5 host with the perf branch-stack
  permitted. Neither runs under standard CI's default sandbox (the tier self-skips),
  but **AMD LBR is live-verified** on a Zen 5 host (Ryzen 9 9950X, `amd_lbr_v2`) via
  `make docker-hwtrace-amd` — the hwtrace image run with `--security-opt
  seccomp=unconfined --cap-add=PERFMON` so `perf_event_open` is allowed. Note AMD's
  branch stack is delivered only at a PMU sample, so AMD LBR captures branch-heavy
  routines and honestly `truncated`s a tiny single-shot routine (too fast to sample);
  single-step is the deterministic in-process backend for the latter.
