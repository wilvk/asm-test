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

> For a focused, standalone reference to this tier — the four backends, the
> `available()` gating chain, the region lifecycle, backend auto-selection, the
> W^X exec-memory helper, and the per-language wrappers — see
> [Hardware tracing](hardware-tracing.md). This section covers it in the context of
> the wider native-trace family.

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
tracee that runs the routine, reading the program counter from the child's register
file at each stop and reconstructing the trace in the parent (no shared memory — the parent
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
kernel-only with no in-process form. It runs on **Linux x86-64 and AArch64** off one
body — the AArch64 arm reads the PC + return register via `PTRACE_GETREGSET`/`NT_PRSTATUS`
(no `PTRACE_GETREGS` there) and decodes block lengths with `ASMTEST_ARCH_ARM64` Capstone.
(The AArch64 single-step *stream* can **only** be validated on a real AArch64 host —
not under qemu-user, which cannot emulate the ptrace tracer/tracee relationship — so it
is code-implemented and build/self-skip-validated, with the live stream **pending real
hardware**; `available()` self-probes and self-skips under qemu. The `/proc`+jitdump
readers, being pure file parsing, run on any Linux arch and are validated live on AArch64.) The supported target is a
deterministic, single-threaded routine (≤6 integer args) that **may call out to helpers**
outside the registered region — call-outs (runtime helpers, GC barriers, PLT stubs) are
**stepped over at native speed** and not recorded, so a real method that calls helpers
traces correctly, not just a pure-compute leaf (the stepper decodes the region-exit
instruction via Capstone's is-call query; without Capstone it falls back to leaf-only).
`make hwtrace-test` exercises it live, including a region that calls an out-of-region
helper (it all works in a **plain unprivileged container** — ptrace of one's own child
needs no extra capability).

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
same `[0x0, 0x3, 0x6, 0xc, 0x11]` stream.

The last piece — finding *which* `(base, len)` to trace in a live process — is two
resolvers that read it from the OS the way `perf` and a debugger do:

- `asmtest_proc_region_by_addr(pid, addr, &base, &len)` walks `/proc/<pid>/maps` for
  the executable mapping containing `addr` and returns its extent — the "I have one
  address inside a foreign routine, give me the whole region to trace" step.
- `asmtest_proc_perfmap_symbol(pid, name, &base, &len)` parses `/tmp/perf-<pid>.map`,
  the de-facto text format (`<hex start> <hex size> <symbol>`) that V8/Node, .NET, and
  OpenJDK (+perf-map-agent) write so `perf` can symbolize **generated code**, and
  returns a JIT method's `(base, len)` by name.

For JITs that emit the richer **binary jitdump** image (`jit-<pid>.dump` — CoreCLR,
HotSpot, V8; what `perf inject --jit` consumes), `asmtest_jitdump_find(path, pid, name,
&entry, bytes, cap, &len)` resolves a method to its `(code_addr, code_size)` **and its
recorded native code bytes** — which the text perf-map cannot give. Because each
`JIT_CODE_LOAD` record is timestamped, a method re-emitted at a reused address (tiered
or OSR recompilation) resolves to the **latest** body — the temporal
same-address-different-bytes problem the
[JIT runtime tracing analysis](https://github.com/wilvk/asm-test/blob/main/docs/analysis/jit-runtime-tracing.md)
centres on. The bytes
matter because a hardware/branch trace records only control flow; the decoder must be
handed the exact bytes that were live, and jitdump carries them.

One piece sits between resolution and tracing: `trace_attached` needs the target stopped
*at* the method entry, but a real managed runtime calls a JIT method on its own schedule,
so you cannot attach at exactly the right instant. `asmtest_ptrace_run_to(pid, addr)`
closes that **uncontrolled-timing** gap — it plants a software breakpoint at `addr`
(`PTRACE_POKETEXT`: `int3` on x86-64, `brk` on AArch64, which patches an r-x text page the
way a debugger does), `PTRACE_CONT`s until the program *itself* next calls in, then
removes the breakpoint and rewinds the PC, leaving the target stopped exactly at `addr`
ready for `trace_attached` (which also records the entry instruction from either stop
convention).

So the full managed-runtime flow is: resolve the method's region from the runtime's
jitdump or perf-map (or an address via `/proc/maps`) → `PTRACE_ATTACH` → `run_to` →
`trace_attached` → `PTRACE_DETACH`. `make hwtrace-test` runs it end to end: it discovers
a foreign process's region from `/proc/<pid>/maps` using only an interior address and
traces *that* region (no hardcoded base), and — with **no cooperative go-flag** — has a
child publish a perf-map and call its routine in a loop while the tracer resolves it by
name, `run_to`s the entry, and traces that invocation to the exact `[0,3,6,c,11]` stream;
it also parses a JIT perf-map entry by name and reads a jitdump image — skipping
non-`LOAD` records, picking the latest re-JIT body, and recovering the recorded code
bytes. The text perf-map remains the portable lowest common denominator for JITs that
only emit symbols.

**Against real runtimes.** One argv-driven harness (`examples/jit_trace.c`) points the
whole pipeline at a live JIT — not a fixture — for **three** runtimes:

- `make docker-hwtrace-jit` (Node.js / **V8**, `asmtest-node` image): spawns
  `node --perf-basic-prof --no-turbo-inlining` on a hot function, resolves the optimized
  method from V8's real perf-map, attaches, `run_to`s the entry, and single-steps one
  invocation — recovering the actual **TurboFan** machine code for `(a+b)|0` (frame setup,
  stack-limit check, smi type-guards, `add edx, ecx`, `ret`; 29 instructions).
  `--no-turbo-inlining` keeps the function a real standalone callable body (else TurboFan
  inlines the tiny function and its perf-map entry is never actually called).
- `make docker-hwtrace-jit-dotnet` (.NET / **CoreCLR**, `asmtest-dotnet` image): builds a
  tiny console app and traces its `Program::Add`, recovering the JIT's `lea eax, [rdi +
  rsi]; ret`. `DOTNET_TieredCompilation=0` gives a single compilation at a stable address
  (no churn) and `[MethodImpl(NoInlining)]` keeps it a real call target. Notably it traces
  .NET's **W^X** code heap **as-shipped** (no `DOTNET_EnableWriteXorExecute=0`): the JIT
  code is double-mapped so a software breakpoint (`PTRACE_POKETEXT`) is refused with
  `EIO`, and `run_to` transparently falls back to a **hardware** execution breakpoint
  (x86-64 debug registers DR0/DR7), which writes no code. Hardware breakpoints are also
  per-thread, so they never trap a sibling runtime thread the way a process-wide `int3`
  can. The fallback engages automatically when `POKETEXT` is refused; `ASMTEST_PTRACE_HW_BP`
  forces it (the `hwtrace-test` suite uses that to exercise the hardware path
  deterministically on ordinary memory). AArch64's hardware-breakpoint ptrace interface
  is a separate follow-on; there `run_to` is software-only for now.
- `make docker-hwtrace-jit-java` (OpenJDK / **HotSpot**, `asmtest-java` image): compiles a
  one-method hot loop and traces its `Hot.asmtjit`, recovering the C2 JIT's `lea eax, [rsi
  + rdx]` (the `a + b` body, wrapped by HotSpot's nmethod entry barrier and stack-bang).
  `-XX:-TieredCompilation` gives a single C2 compilation at a stable address and
  `-XX:CompileCommand=dontinline,Hot.asmtjit` keeps it a standalone call target (else C2
  inlines the tiny method into the caller's compiled loop and its nmethod is never
  entered — the trap the V8 lane dodges with `--no-turbo-inlining`). HotSpot needs two
  things the other two don't, both handled in the harness. First, it does **not** stream a
  perf-map as it JITs, so the lane drives `jcmd <pid> Compiler.perfmap` (JDK 17+) to
  materialize `/tmp/perf-<pid>.map` for the *live* process — a second, on-demand perf-map
  producer the library's parser is now exercised against. Second, the `java` launcher runs
  Java `main()` on a **secondary** OS thread, not the primordial thread V8/CoreCLR use, so
  the harness identifies the spinning loop thread by a short CPU-time sample and
  `PTRACE_ATTACH`es exactly *that* tid — a software-breakpoint (`int3`) trap delivered on a
  thread no tracer owns is fatal to the process. `asmtjit` is `static`, so its verified
  entry sits at the nmethod's `code_begin` (no receiver inline-cache check) — exactly the
  address the perf-map reports, which is where `run_to` plants the breakpoint.

These three are honest by construction: a watchdog alarm bounds the single-step so a
re-tiered/moved address self-skips instead of hanging, and the trace is asserted when the
runtime cooperates and skipped (never failed) when it does not, while the resolution and
attach checks — which validate the library against the runtime's real perf-map line and a
real `/proc/maps` — stand on their own.

Three further lanes validate the **binary jitdump** byte source (`asmtest_jitdump_find`)
against real output from **three independent producers** (V8, HotSpot, CoreCLR):

- `make docker-hwtrace-jit-jitdump` (Node.js / **V8**): runs `node --perf-prof`, which
  writes a real `jit-<pid>.dump`, and recovers a method's **recorded code bytes** from it
  (the byte source a branch-trace decoder must be handed — unlike the text perf-map, which
  carries only address + size + name). It validates the recovered bytes three ways: the
  address agrees with V8's own perf-map (two independent V8 outputs on the same
  compilation), the bytes disassemble to real x86-64 instructions, and they match the
  **live** code at that address (so the jitdump truly captured the running bytes — the
  temporal same-address-different-bytes guarantee jitdump exists for). `asmtest_jitdump_-
  find` matches by exact name, so the lane takes the name from the easy-to-parse text
  perf-map (V8 emits the same string in both).
- `make docker-hwtrace-jit-java-jitdump` (OpenJDK / **HotSpot**): a *second, independent*
  jitdump producer — a different runtime **and** a different encoder. HotSpot has no native
  jitdump, so the lane loads the perf project's JVMTI agent (`libperf-jvmti.so`, from
  `linux-tools` — a userspace agent, so a kernel-version-mismatched copy still runs) with
  `-agentpath`; it records every C2 method to a real `jit-<pid>.dump` under
  `$JITDUMPDIR/.debug/jit/<session>/`. This stresses `asmtest_jitdump_find` on output it did
  not see from V8: methods are named in JVM **descriptor** form (`LHot;asmtjit(II)I`, not
  V8's symbol), and `JIT_CODE_DEBUG_INFO`/unwinding records are interleaved between the
  `JIT_CODE_LOAD` records the reader must skip past. Unlike V8 (which writes its jitdump
  record-by-record), the agent **buffers** and flushes the tail only when it is unloaded on
  a clean JVM shutdown — the `perf record` workflow reads the dump post-exit. So the lane
  resolves asmtjit's address from the live `jcmd Compiler.perfmap` and snapshots its live
  bytes, then **`SIGTERM`s** the JVM (the orderly shutdown flushes the dump; `SIGKILL`
  would lose it) before reading the completed dump. The recovered bytes are checked three
  ways — they disassemble to real x86-64, match the **live** nmethod snapshot, and the
  jitdump `code_addr`/size agree with HotSpot's perf-map (two independent HotSpot outputs).
  The lane self-skips cleanly when the agent or Capstone is absent.
- `make docker-hwtrace-jit-dotnet-jitdump` (.NET / **CoreCLR**): the third producer, and the
  simplest. Unlike HotSpot, CoreCLR writes a real `/tmp/jit-<pid>.dump` **natively** (no
  agent) under `DOTNET_PerfMapEnabled=1`, record-by-record, naming the method *identically*
  in the perf-map and the jitdump — so it reuses the exact same `trace_jitdump` path as V8
  (one routine parameterized by runtime and method name). It recovers `Program::Add`'s
  recorded bytes (`lea eax,[rdi+rsi]; ret`) and runs the same four checks. With this, the
  binary jitdump reader is validated against all three managed runtimes.

### Time-aware code-image recorder (the foreign-JIT byte source)

`trace_attached` reads the region's bytes with a **single** `process_vm_readv` snapshot.
For a live JIT that is wrong the moment the code is patched, freed, or has its address
reused mid-trace: a late snapshot returns whatever bytes are there *now*, not the bytes
that were live when the trace ran. The kernel solves this for its own self-modifying code
with `PERF_RECORD_TEXT_POKE` (old+new bytes, timestamped); `asmtest_codeimage`
([asmtest_codeimage.h](../include/asmtest_codeimage.h)) is the userspace equivalent — a
**timestamped code-image timeline**.

```c
#include "asmtest_codeimage.h"

asmtest_codeimage_t *img = asmtest_codeimage_new(pid);   /* 0 = self */
asmtest_codeimage_track(img, base, len);   /* snapshot v0, arm change detection */
uint64_t t0 = asmtest_codeimage_now(img);  /* a logical timestamp */
/* ... the JIT re-emits code at `base` ... */
asmtest_codeimage_refresh(img);            /* re-snapshot changed pages as a new version */

const uint8_t *bytes; size_t n;
asmtest_codeimage_bytes_at(img, base, t0, &bytes, &n);   /* the bytes live AT t0 */
asmtest_codeimage_free(img);
```

Change detection is **soft-dirty** — arm by clearing the soft-dirty PTE bit
(`/proc/<pid>/clear_refs`), detect set bits via the `PAGEMAP_SCAN` ioctl
(`PAGE_IS_SOFT_DIRTY`) where available else by parsing `/proc/<pid>/pagemap` — which works
**cross-process**, the foreign-JIT case. (`PAGEMAP_SCAN`'s precise write-protect-async mode
is *not* used: it requires the owning process to register the range with `userfaultfd`, so
it cannot monitor a foreign JIT.) The W2 stepper consumes the timeline:

```c
/* like trace_attached, but decode against the bytes that were live at `t0` */
asmtest_ptrace_trace_attached_versioned(pid, base, len, img, t0, &result, trace);
```

An **optional eBPF emission detector** (built only with `clang`+`libbpf`+`bpftool`, i.e.
`-DASMTEST_HAVE_LIBBPF`) tells the recorder *when* code appears so it can snapshot on the
`PROT_EXEC` edge instead of polling: a CO-RE program on `mprotect`/`mmap`/`memfd_create`,
filtered to the target's PID namespace (`bpf_get_ns_current_pid_tgid`, so it is correct
inside containers), draining `{addr,len,kind,…}` events from a `bpf_ringbuf`:

```c
if (asmtest_codeimage_bpf_available()) {        /* self-skips without libbpf / CAP_BPF */
    asmtest_codeimage_watch_bpf(img);
    asmtest_codeimage_poll_bpf(img, 0);         /* non-blocking drain (interleaves stepping) */
    asmtest_codeimage_event_t ev;
    while (asmtest_codeimage_next(img, &ev)) { /* ev.addr was just published; snapshot it */ }
}
```

Validation: the same-address-different-bytes temporal proof and the versioned W2 trace run
live (no privilege) in `make codeimage-test` and `make hwtrace-test` on any x86-64 Linux
host; the eBPF detector runs in `make docker-hwtrace-codeimage` — a `--cap-add=BPF,PERFMON`
container (not `--privileged`) that observes a real `mprotect(PROT_EXEC)` edge. On a host
without soft-dirty / libbpf, the recorder / detector self-skip, so nothing here is a hard
dependency of the base tier.

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

The **out-of-process / foreign-process toolkit** above is exposed through every wrapper
too — a `Ptrace` class (or, where idiomatic, `HwTrace.ptrace_*` methods) surfacing
`available`/`skipReason`, `traceCall`, `traceAttached`, `runTo`, `regionByAddr`,
`perfmapSymbol`, and `jitdumpFind` (with a `JitMethod` value type carrying the recorded
code bytes). `runTo(pid, addr)` runs an already-attached target forward to a resolved
method via a software breakpoint — the step that makes a JIT method traceable when you
don't control its call timing — leaving it stopped at `addr` for `traceAttached`. Each
binding's hwtrace test exercises the live-testable subset — out-of-process `traceCall`
to the same `[0,3,6,c,11]` stream, `/proc/maps` and perf-map resolution, a
binary-jitdump round-trip, and `runTo`'s FFI round-trip (a NULL-addr → `EINVAL` probe;
the live foreign attach is covered by the C suite) — and `asmtest_ptrace.h` is covered by
the `check-bindings-parity` gate (51 tier symbols × 10 bindings). All ten are validated
live in plain unprivileged containers.

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
