# Hardware tracing

The **hardware-trace tier** ([asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h),
`src/hwtrace.c`) records which instructions and basic blocks a routine actually
executes on the **real CPU** with near-zero *capture* overhead: the processor
emits a compressed branch-trace packet stream into a kernel ring, and a software
decoder reconstructs the ordered instruction stream afterward. It is one member
of asm-test's native-trace family — see [Native runtime tracing](native-tracing.md)
for how it sits alongside the DynamoRIO and emulator tiers and for the
out-of-process / foreign-JIT toolkit built on the same single-step machinery.

Like every other backend, it fills the **same** `asmtest_trace_t` shape (ordered
instruction offsets, distinct basic-block offsets, totals, a truncation bit — see
[Execution traces](traces.md)), so a test can switch backends without changing how
it reads coverage, and the optional [Capstone annotation layer](../disassembly.md)
renders any backend's offsets back to instruction text.

> **Diagram:** [Trace and coverage backends](../../reference/diagrams.md#trace-and-coverage-backends)

This tier is **optional, advanced, and self-skipping**: it is kept out of the core
`libasmtest` and the `libasmtest_emu` superset, builds only when its toolchain is
present (the PT/CoreSight decoders are only linked when libipt/OpenCSD are found;
the AMD and single-step backends need no extra library), and degrades to a clear
"skipped" message on hosts that cannot run it. There is no DynamoRIO or `drwrap`
dependency — libipt and OpenCSD are BSD.

---

## The four backends

All four backends fill one `asmtest_trace_t` (one offset basis, one block
partition) and gate behind one `asmtest_hwtrace_available()` predicate, so a
caller can pick whichever the host can run without changing the rest of its code.

| Backend (`asmtest_trace_backend_t`) | Mechanism | Decoder | Runs where | Completeness ceiling |
|---|---|---|---|---|
| `ASMTEST_HWTRACE_INTEL_PT` | Intel PT TNT/TIP/PSB packets → kernel AUX ring (`perf_event_open`) | libipt | bare-metal **Intel** x86-64 + perf privilege | ring size |
| `ASMTEST_HWTRACE_AMD_LBR` | AMD Zen 4–5 LbrExtV2 branch stack (Zen 3 BRS exists in silicon but this tree cannot open it yet) | built-in | bare-metal **AMD** (Zen 4+) + perf branch-stack | 16-deep stack per sample; **Tier-B stitching** decodes past it — ceiling is the data ring (`data_size`) |
| `ASMTEST_HWTRACE_CORESIGHT` | ARM ETM/ETE waypoints → AUX ring | OpenCSD | specific **AArch64** boards (scaffold) | ring size |
| `ASMTEST_HWTRACE_SINGLESTEP` | `EFLAGS.TF` → `#DB` → `SIGTRAP` after every instruction | Capstone length-decoder | **any x86-64 Linux or macOS** (no PMU/perf/privilege/decoder) | none — exact + complete |

The PT and CoreSight backends observe *out of band* and are the recommended
backends for JIT/GC-heavy managed runtimes (JVM, .NET, Node), where in-process
[DynamoRIO](native-tracing.md#dynamorio-tier) collides with the runtime's signal
and code-cache machinery. AMD LBR delivers its branch stack only at a PMU sample,
so it captures branch-heavy routines well and honestly marks a too-fast single-shot
routine `truncated`. The **single-step backend is the universal floor** — it
produces the same exact, complete offsets on every x86-64 Linux host (Intel, AMD
of any generation, VMs, CI, plain unprivileged containers); see
[Single-step: the portable backend](#single-step-the-portable-backend) below.

> The Intel PT capture + libipt decode and the AMD LBR backend are implemented and
> live-verified (AMD LBR on a Zen 5 Ryzen 9 9950X via `make docker-hwtrace-amd`).
> The CoreSight backend is a documented scaffold pending AArch64 board access — it
> always self-skips until completed.

Beside these four *exact* backends sits one deliberately separate, *statistical*
producer — the [AMD IBS-Op lane](#statistical-edges-out-of-band-the-amd-ibs-op-lane)
(sampled hot control-flow edges, out of band and unprivileged, on any Zen
including Zen 2, where every exact hardware backend self-skips). It fills its own
shape, never `asmtest_trace_t`, so it is documented below rather than in the table.

---

## Availability and self-skip

The PT/CoreSight backends need bare metal: Intel PT is Intel-x86-64-only (absent on
AMD, ARM, Apple Silicon, and almost all cloud/VM/CI guests); CoreSight self-hosted
trace needs a specific AArch64 board (Juno, Kria, Jetson, Pixel, …) and is
prohibited in KVM guests. Both also need `perf_event_paranoid` lowered (effectively
`-1` for a default-size PT buffer) or `CAP_PERFMON` — a host knob the process cannot
grant itself.

`asmtest_hwtrace_available(backend)` collapses that whole chain into a single
detect-and-skip predicate. It returns `1` only when **(a)** this build links the
backend's decoder, **(b)** the right CPU/ISA/vendor is present, **(c)** the kernel
exposes the backend's PMU (`intel_pt` / `cs_etm`), and **(d)** `perf_event` capture
is permitted for this process — and `0` (self-skip) otherwise, which is the common
case off bare metal. `asmtest_hwtrace_skip_reason()` fills a human-readable reason
for the skip message, e.g. *"no intel_pt PMU (needs bare-metal Intel; absent on
AMD/VM)"*.

The idiom is to gate every use on `available()` so a suite self-skips cleanly
instead of failing on a host that lacks the backend:

```c
#include "asmtest_hwtrace.h"

if (asmtest_hwtrace_available(ASMTEST_HWTRACE_INTEL_PT)) {
    asmtest_hwtrace_options_t opts = {.backend = ASMTEST_HWTRACE_INTEL_PT};
    asmtest_hwtrace_init(&opts);
    asmtest_hwtrace_register_region("add2", base, len, tr);
    asmtest_hwtrace_begin("add2");
    fn(20, 22);                          /* runs on the real CPU; PT captures it */
    asmtest_hwtrace_end("add2");         /* drains the AUX ring + decodes into tr */
    asmtest_hwtrace_shutdown();
} else {
    char why[128];
    asmtest_hwtrace_skip_reason(ASMTEST_HWTRACE_INTEL_PT, why, sizeof why);
    /* SKIP(why) — e.g. "no intel_pt PMU (needs bare-metal Intel; absent on AMD/VM)" */
}
```

---

## The region lifecycle

The tier reuses the native-trace begin/end region model. The five calls are:

| Call | Role |
|---|---|
| `asmtest_hwtrace_init(opts)` | bring the chosen backend up; size the AUX/data rings |
| `asmtest_hwtrace_register_region(name, base, len, trace)` | bind a named code range to an app-owned `asmtest_trace_t` |
| `asmtest_hwtrace_begin(name)` | enable capture for that region |
| `asmtest_hwtrace_end(name)` | disable capture and decode the captured packets into the trace |
| `asmtest_hwtrace_shutdown()` | tear the backend down |

`asmtest_hwtrace_options_t` controls the rings: `aux_size` (trace ring bytes,
rounded up to a power-of-two pages, `0` → 64 KB), `data_size` (base perf ring, `0` →
8 KB for PT/CoreSight; the AMD backend defaults it to 256 KB, and it bounds the Tier-B
stitched capture — raise it, and `kernel.perf_event_max_sample_rate` /
`kernel.perf_cpu_time_max_percent=0` on the runner, to extend reach), `snapshot` (on
Intel PT, nonzero maps a circular snapshot ring instead of a linear drain —
capture-side only so far: `end()` decodes the linear ring, and the circular-ring walk
from `aux_tail` is a named follow-up; on the AMD backend it opts the markers into the
**deterministic boundary LBR snapshot** instead of the sampled path), and an optional
`object_hint` path for hardware address filters. The AMD-specific reach knobs
(`lbr_period`, `branch_filter`, the snapshot, ring sizing) are covered in
[Tuning AMD LBR window reach](amd-lbr-tuning.md).

Only the `backend` field is required; the rest default sensibly when zero-initialized.

> **MVP limitation.** Capture state is a single process-global slot — only **one**
> region may be active at a time (a `begin` while another is active is ignored), and
> the markers are not yet per-thread. Bracket one registered routine per begin/end
> pair.

### Scoped-tracing primitives (the shared-core `§0` surface)

The scope constructs in the language bindings (`using`/RAII/`with`/`defer`) are thin
shims over three additional entry points that live in the C core so every binding
inherits them once:

| Call | Role |
|---|---|
| `int asmtest_hwtrace_try_begin(name)` | like `begin`, but returns `0` on success and a negative `ASMTEST_HW_*` on a busy slot (`ESTATE`) or an unregistered name (`EINVAL`) — the signal a scope shim needs instead of a silent no-op. `begin` is now a thin wrapper that discards this status, so the shipped `void` ABI is unchanged. |
| `int asmtest_hwtrace_arm_tid(void)` | the OS thread id (`SYS_gettid`) that armed the active capture, or `-1` when idle — a shim can assert its close runs on the arming thread. |
| `int asmtest_hwtrace_render(name, buf, buflen)` | render the region's recorded instruction offsets to Capstone disassembly text. `snprintf` semantics: pass `buf=NULL, buflen=0` to size, then allocate and call again; returns the non-negative would-be length, or a negative `ASMTEST_HW_*` on a name miss / absent Capstone. A truncated or over-capacity trace renders as a labelled prefix, never as complete. |

Two correctness rules the scope shims rely on:

- **Register-then-begin under the same name.** `begin`/`try_begin` key on the region
  name, so a self-naming scope must `register_region` under its generated name first.
- **`register_region` is idempotent by name.** A repeat registration of a name that
  already has a slot refreshes that slot in place (its `[base, len)` + trace) instead
  of appending, so a scope object that registers on *every* construction reuses one
  slot — a looped or sprinkled scope never exhausts the 32-entry table, which now
  counts **distinct** scope sites.

**Thread-scope backstop.** `end` compares the closing thread against `arm_tid` and
flags the trace `truncated` on a mismatch, so a scope whose work hopped OS threads
(`await` / `go func()` / a thread-pool continuation) is never emitted as complete.

---

## Single-step: the portable backend

`ASMTEST_HWTRACE_SINGLESTEP` is the member of this tier that runs **everywhere**. It
produces the **same exact, complete** `asmtest_trace_t` offsets as Intel PT —
byte-for-byte the Unicorn/DynamoRIO instruction and block streams — but needs no
PMU, no `perf_event`, no privilege, no decoder library, and no specific CPU.

With `EFLAGS.TF` set, the CPU raises a trap-class `#DB` after every instruction,
which Linux delivers as `SIGTRAP`. `begin(name)` installs a `SIGTRAP` handler and
arms `TF` on the calling thread; the handler records each in-region `RIP` (offsets
outside the registered region — callees, the begin/end glue — are stepped but not
recorded). `end(name)` clears `TF`, restores the handler, and derives the block
partition from fall-through discontinuities (a new block at the entry and after
every taken branch) with the Capstone length-decoder. The handler does only
async-signal-safe work (a bounds-checked store of each offset); all decoding happens
in the post-pass.

The trade is **exact-cheap (a hardware trace ring) vs. exact-slow (a fault per
instruction)** — roughly a couple of microseconds per instruction. So single-step is
the right tool for **small registered routines** (asm-test's common case): a handful
to a few hundred instructions cost microseconds to low milliseconds. Unlike AMD LBR
there is **no depth ceiling** — a loop of any length reconstructs exactly. It is
**not** for whole-program or hot-loop tracing; that is the DynamoRIO tier's job.

```c
#include "asmtest_hwtrace.h"

if (asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
    asmtest_hwtrace_options_t opts = {.backend = ASMTEST_HWTRACE_SINGLESTEP};
    asmtest_hwtrace_init(&opts);
    asmtest_hwtrace_register_region("add2", base, len, tr);
    asmtest_hwtrace_begin("add2");
    fn(20, 22);                          /* stepped on the real CPU */
    asmtest_hwtrace_end("add2");         /* trace already filled; blocks normalized */
    asmtest_hwtrace_shutdown();
}
```

The supported target is a **well-behaved compute routine**: no in-routine
`POPF`/`IRET`/signal handlers or self-modifying code, which break naive stepping and
are flagged `truncated` rather than emitted as complete (the same "registered bytes
must be stable" contract the PT/AMD backends carry). Single active region, single
thread, like the rest of the tier.

An **out-of-process** sibling (`asmtest_ptrace.h`) gets the same exact offsets a
different way — a tracer parent `PTRACE_SINGLESTEP`s a forked or attached tracee — so
it touches none of the target's signal disposition or code cache. That is the path
for managed runtimes and the only single-step form on AArch64, and it carries the
foreign-JIT resolution toolkit (`/proc/maps`, perf-maps, binary jitdump, the
time-aware code-image recorder). It is documented in full under
[Native runtime tracing](native-tracing.md#out-of-process-variant-w2--ptrace).

By default that tracer steps **over** the call-outs a method makes (runtime helpers, GC
barriers, PLT stubs) — keeping the trace to the method's own body. **Call descent** is an
opt-in that follows those calls instead, recording each as an edge (level 1) or descending
into it as a nested frame (levels 2–3). See
[Call descent levels](native-tracing.md#call-descent-levels). Descending into *known* method
regions (level 2) is a sound default; **level 3 — descend into everything — is default-off and
best-effort on a live managed runtime**: single-stepping a helper that holds a GC / JIT /
loader lock can perturb or **deadlock** sibling runtime threads, which no watchdog fully
prevents. Reserve L3 for the fork path or a frozen/post-mortem target — see the L3 hazards in
[analysis/jit-runtime-tracing.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/analysis/jit-runtime-tracing.md).

---

## Block normalization

Hardware records branch *decisions* only, so the block partition a PT/CoreSight
decoder yields is **coarser** than Unicorn/DynamoRIO basic blocks (a decoded block
can span direct branches). The backend therefore **normalizes**: it walks the
reconstructed per-instruction stream and starts a new block at the first instruction
and after every branch — the same single-entry/ends-at-branch model the other
backends use. The single-step backend derives the identical partition from
fall-through discontinuities. The upshot is that **block offsets match across every
backend** for the same routine.

---

## Auto-selecting a backend

Because all four backends fill the same trace and self-skip identically, the
**most-faithful available** one can be chosen for the host without hard-coding an
enum. Two calls do that:

- `asmtest_hwtrace_resolve(policy, out, cap)` writes the available backends,
  most-faithful first (`Intel PT > AMD LBR > single-step > CoreSight`), and returns
  the count.
- `asmtest_hwtrace_auto(policy)` returns just the head as an `int` — a valid
  `asmtest_trace_backend_t` when `>= 0`, or a negative `ASMTEST_HW_*` status
  (`ASMTEST_HW_EUNAVAIL`) when no backend is available.

The `policy` is one of:

- **`ASMTEST_HWTRACE_BEST`** — the most faithful backend the host can run.
- **`ASMTEST_HWTRACE_CEILING_FREE`** — the same, but skipping the one backend with a
  bounded completeness ceiling (AMD LBR: Tier-B stitching decodes past the 16-deep
  stack, but capture is still bounded by the data ring and PMI throttling). Re-resolve
  under this after a trace comes back `truncated`, so the second attempt has no such
  ceiling.

```c
#include "asmtest_hwtrace.h"

int b = asmtest_hwtrace_auto(ASMTEST_HWTRACE_BEST);   /* >=0 backend, or <0 status */
if (b >= 0) {
    asmtest_hwtrace_options_t opts = {.backend = (asmtest_trace_backend_t)b};
    asmtest_hwtrace_init(&opts);
    asmtest_hwtrace_register_region("fn", base, len, tr);
    asmtest_hwtrace_begin("fn");  fn(20, 22);  asmtest_hwtrace_end("fn");
    asmtest_hwtrace_shutdown();

    /* Dynamic fallback: if the chosen backend could not record the whole path
     * (AMD LBR's data ring could not hold the stitched run, a PT ring
     * overflowed), re-resolve to a ceiling-free backend and re-run. */
    if (tr->truncated) {
        int b2 = asmtest_hwtrace_auto(ASMTEST_HWTRACE_CEILING_FREE);
        if (b2 >= 0 && b2 != b) { /* re-init under b2, begin/call/end again */ }
    }
}
```

On **any x86-64 Linux host the cascade is non-empty** — the single-step backend is
the floor — so `auto()` never fails there; it only returns a negative status off
x86-64 Linux (and a non-CoreSight host). On a bare-metal Intel host `auto(BEST)`
resolves to Intel PT; on a Zen 4+ host with perf permitted it resolves to AMD LBR
(`CEILING_FREE` falling to single-step); otherwise it resolves to single-step.

This call orchestrates only the **hardware tier's own** backends — one library, one
API. To extend the choice *across* the hardware, DynamoRIO, and emulator tiers
(which cross a real-CPU vs. virtual-guest fidelity line), use
`asmtest_trace_auto` — see
[Cross-tier orchestration](native-tracing.md#cross-tier-orchestration-over-all-three-tiers).

---

## The zero-config whole-window scope (region-free)

Everything above brackets a **registered region** — a `[base, len)` you named. The
zero-config whole-window scope drops even that: no `NativeCode`, no `[base, len)`,
just *"trace whatever the calling thread runs inside this block"*. It is the
aspirational `using (new AsmTrace())` form, and it works today on any x86-64 Linux
host with zero setup. The full design is in the internal plan,
[scoped-tracing-zeroconfig-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/scoped-tracing-zeroconfig-plan.md).

The C surface is three calls:

- `asmtest_hwtrace_begin_window(trace, &scope)` — arm a region-free capture on the
  calling thread; `trace->insns[]` will hold **absolute** addresses (not
  base-relative offsets).
- `asmtest_hwtrace_end_window(scope, trace)` — close it (and normalize).
- `asmtest_hwtrace_render_window(scope, buf, buflen)` — disassemble the recorded
  absolute addresses from live self-memory (valid for non-moving native code).

```c
asmtest_trace_t *tr = asmtest_trace_new(4096, 0);
asmtest_hwtrace_scope_t scope = {0xffffffffu, 0, -1};
if (asmtest_hwtrace_begin_window(tr, &scope) == ASMTEST_HW_OK) {
    fn(20, 22);                          /* whatever runs here is captured */
    asmtest_hwtrace_end_window(scope, tr);
}
```

In .NET the same scope is the empty constructor:

```csharp
using (new AsmTrace()) {
    Fn(20, 22);
}
```

### The WEAK / STRONG / CEILING ladder

The whole-window scope resolves to one of three tiers, most-portable first. Because
the region-free frame has no `[base, len)` to *step over*, "capture whatever ran
here" is descent-into-everything — so the WEAK tier needs the same safety guards the
out-of-process descent tier has, which it now carries.

| Tier | Backend | Fidelity | Needs | State |
|---|---|---|---|---|
| **WEAK** | in-process single-step (`EFLAGS.TF`) | exact, descends into every call; honestly noisy | any x86-64 Linux, unprivileged | **ships today** |
| **STRONG** | whole-window Intel PT (`asmtest_pt_decode_window`) | exact, low-perturbation | bare-metal Intel PT + `perf_event_paranoid < 0` or `CAP_PERFMON` | decode **synthetic-validated**; live capture forward-look |
| **CEILING** | AMD LBR sampled survey (`IsStatistical`) | a hot-method histogram, **never** an exact whole window | Zen 4+ bare metal + `CAP_PERFMON` | ships as a statistical survey |

- **WEAK** — the single-step tier records the absolute RIP of every instruction the
  thread runs. It is CI-runnable on any x86-64 Linux for a **native leaf** (pointing
  single-step at live managed code is forbidden — it fights the runtime's SIGTRAP/JIT).
  It is honestly noisy (the runtime's own machinery is stepped too) and **bounded**:
  a capture ring caps memory, a per-frame **instruction budget** (default 4× the ring)
  and a wall-clock **watchdog** (default 10 s) cap runaway/blocking windows, and an
  opt-in **deny-region** set ends the capture at a blocking libc entry. Overflow or any
  guard firing flags the trace `truncated`; `asmtest_hwtrace_begin_window_ex` configures
  the guards per window and `asmtest_hwtrace_window_guard` reports which one fired.
- **STRONG** — on an inited Intel PT tier `begin_window` instead arms a real
  whole-window PT capture. The decode is **validated on a synthetic PT-packet fixture**
  (see below); its **live** capture is forward-look. Off bare-metal Intel PT (VMs,
  containers, every AMD host) the arm self-skips cleanly.
- **CEILING** — AMD LBR is shipped as a **sampled hot-method survey** (`IsStatistical`),
  never an exact whole window: the exact-whole-window contract cannot be met by a
  sampled 16-deep branch stack. It needs **Zen 4+** bare metal (LbrExtV2) with
  `CAP_PERFMON`. The complete AMD capture path is instead the **out-of-process ptrace
  stepper** (block-step mode — landed) and the **eBPF boundary LBR snapshot** (landed;
  Zen 4/5, Linux ≥ 6.10), which reads the 16-entry window deterministically at scope
  boundaries. Callers wanting the quiet sampled complement use the explicit
  `asmtest_hwtrace_sample_begin_amd` / `new AsmTrace(HwBackend.AmdLbr)` path.

### Synthetic decode-validation (no silicon required)

The STRONG tier's PT decode is de-risked without Intel PT hardware.
`asmtest_pt_encode_fixture` ([src/pt_backend.c](../../../src/pt_backend.c))
hand-assembles a **valid PT byte stream** with libipt's own packet encoder, and
`test_wholewindow_decode` decodes it end-to-end through `asmtest_pt_decode_window`
with **libipt at build time and no PT hardware** — the `docker-hwtrace` image installs
`libipt-dev` for exactly this. That exercises the decode *logic* (packet walk, IP
reconstruction against a code-image), but **not** a live AUX ring's PSB cadence or
`aux_tail` wrap: it is necessary, not sufficient, for trusting the live path, which
remains gated on real Intel PT silicon.

---

## W^X executable memory

A caller — notably a language binding — often needs to turn raw host-native machine
code into callable, W^X-correct executable memory before registering and tracing it.
`asmtest_hwtrace_exec_alloc(bytes, len, &base_out, &len_out)` does the
mmap/mprotect dance (`PROT_NONE` → RW to copy the bytes in → RX, icache flushed) and
returns the executable address; cast `base_out` to a function pointer to call it, and
free it with `asmtest_hwtrace_exec_free(base, len)`. It is self-contained — no
dependency on the DynamoRIO tier's `asmtest_exec_alloc`.

```c
void *code; size_t clen;
if (asmtest_hwtrace_exec_alloc(bytes, n, &code, &clen) == ASMTEST_HW_OK) {
    asmtest_hwtrace_register_region("blob", code, clen, tr);
    asmtest_hwtrace_begin("blob");
    ((long (*)(long, long))code)(20, 22);
    asmtest_hwtrace_end("blob");
    asmtest_hwtrace_exec_free(code, clen);
}
```

---

## Running it

```sh
make hwtrace-test             # C smoke test — runs the single-step backend LIVE here
make hwtrace-bindings-test    # every language wrapper, live (Python + the nine others)
make docker-hwtrace           # C + Python in a PLAIN unprivileged container
make docker-hwtrace-bindings  # every language wrapper, in plain containers
make docker-hwtrace-amd       # AMD LBR, live (PERFMON cap + seccomp=unconfined)
```

`make hwtrace-test` executes the single-step backend live on the dev host (where
Intel PT and AMD LBR self-skip), asserting the same `[0x0, 0x3, 0x6, 0xc, 0x11]` /
`{0, 0x11}` partition the other backends produce, plus a 20-trip loop (62
instructions, past a single 16-deep LBR window) to prove completeness. This is the
hardware tier's first regression that **runs** on standard CI instead of
self-skipping. The Intel-PT/CoreSight hardware capture cannot run on standard CI — it
needs a self-hosted bare-metal runner — but the single-step backend removes that
limitation for x86-64, recording the same offsets on CI and in containers with full
automated regression protection. The live foreign-JIT lanes
(`make docker-hwtrace-jit`, `…-jit-dotnet`, `…-jit-java`, `…-jit-jitdump`) are
described in [Native runtime tracing](native-tracing.md#out-of-process-variant-w2--ptrace).

---

## Language wrappers

Every binding ships an `hwtrace` wrapper alongside its `drtrace` one, exposing the
same small surface — `HwTrace.available/init/shutdown`, per-trace
`register`/`region`/`covered`/block+instruction accessors, `resolve(policy)` /
`auto(policy)` for backend selection, and a `NativeCode` for materializing
host-native bytes via `asmtest_hwtrace_exec_alloc`. Each wrapper dlopens
`libasmtest_hwtrace` (resolved from `$ASMTEST_HWTRACE_LIB`, else the repo `build/`)
and defaults to the single-step backend, so `available()` is true and the test
**traces live** on any x86-64 Linux — including CI and plain containers. The
out-of-process / foreign-JIT toolkit is surfaced through every wrapper too. The set
is held in sync by the binding function-parity gate.

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

See [Language bindings](../../bindings/index.md) for the shared binding overview.

---

## Status codes

The tier returns these (negative) statuses; `ASMTEST_HW_OK` is `0`:

| Code | Meaning |
|---|---|
| `ASMTEST_HW_EINVAL` | bad argument |
| `ASMTEST_HW_ESTATE` | wrong lifecycle state (e.g. begin without init) |
| `ASMTEST_HW_EUNAVAIL` | backend / PMU / privilege unavailable |
| `ASMTEST_HW_ENOSYS` | decoder library not compiled in |
| `ASMTEST_HW_EFULL` | trace storage full |
| `ASMTEST_HW_EDECODE` | capture / decode failure |

---

## Tracing live managed code (JIT runtimes)

Everything above traces a native leaf whose bytes sit still. A managed runtime
(.NET/CoreCLR, the JVM, Node/V8) is the hard case: the code you want to trace is
the runtime's own **live JIT output**, emitted at an address that moves as the
method re-tiers, on threads the GC and JIT coordinate with. Two rules follow, and
they set the whole managed-tier contract.

**Single-step is a per-thread footgun against a runtime.** EFLAGS.TF is a
per-thread flag, but the `SIGTRAP` disposition it raises is process-wide — and a
managed runtime's own signal layer (CoreCLR's PAL) already owns it. Arming TF on a
runtime-scheduled thread also risks putting a sibling into runaway stepping, and it
fights the JIT's relocating bytes. So the faithful managed paths are **out-of-band
hardware trace** (Intel PT / AMD LBR + the code-image recorder) or the **§D3
concealed out-of-process ptrace stepper**, never in-process single-step against the
runtime's threads.

One collision is *fatal by kernel design*, not merely noisy: **a TF-armed thread
must not execute code that blocks `SIGTRAP`** — glibc's `pthread_create` blocks all
signals around `clone()`, and the `#DB` the very next instruction raises is then a
synchronous signal delivered while blocked, which the kernel force-resets to
`SIG_DFL` and kills the process with (exit 133; no handler can run). On a slow host
a long-stepped window over runtime machinery hits this deterministically: CoreCLR's
tiered-compilation background worker idle-exits after ~4 s and is **respawned via
`pthread_create` on whichever thread next enqueues JIT work** — the armed thread,
if the window is still open. **`AsmTrace.Method()` removes this by construction** —
Option B, the *lazy-arm* path: it mints a native pointer to the method body and does
`arm → call → disarm` entirely in native code (`asmtest_hwtrace_call_scoped`), so the
runtime's machinery is never under TF and cannot spawn a thread in-window. The residual
exposure is the **whole-window** form (`new AsmTrace()`) on a runtime thread, where
arbitrary code runs in-window by definition: there the .NET self-test lanes pin the
worker resident (`DOTNET_TC_BackgroundWorkerTimeoutMs`, see `hwtrace_dotnet_env` in
`mk/native-trace.mk`), the same mitigation applies to any app arming an in-process
whole managed window, and the §D3 out-of-process form is immune (the thread is never
TF-armed). See
[managed-singlestep-lazy-arm-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/managed-singlestep-lazy-arm-plan.md).

**The whole-scope vs one-method fork.** Hardware trace captures everything cheaply
and filters at decode; a stepper must decide per instruction what to step into. So
the scope's promise degrades differently by shape:

- **Whole-window** (`new AsmTrace()` in the .NET binding) — "trace whatever runs
  here." Clean under PT; under single-step it is best-effort and **expected to
  self-truncate** on a live runtime (the ~1M runtime instructions around a managed
  leaf overflow the window). Honest by design: the raw `Addresses` are exposed, and
  `byMethod`/`withRundown` labelling attributes them to managed methods.
- **One JIT'd method** (`AsmTrace.Method(HotPath)`) — "trace this method's body."
  A region + **lazy-arm** call: reliable, exact offsets, and **managed-safe by
  construction** (only the body runs under TF — see the footgun note above). It costs
  the extra knowledge the empty scope avoids — resolve the body's address (via the
  `MethodLoadVerbose` listener, or the jitdump rundown for a warm/R2R method) and keep
  it un-inlined. A signature the `(long…)->long` shim set cannot express, or
  `outOfProcess: true`, routes through the out-of-process stepper instead.

**Temporal bytes.** Because a tiering method is re-emitted at a possibly-reused
address, the managed labelling decodes each captured address against the code-image
version **live in the window** (the recorder the `byMethod` map feeds), not whatever
bytes are mapped at render time — so a body that moves *after* the scope still
renders what actually ran.

### Opt-in safe-managed routing

The shipping default for `new AsmTrace()` on a managed thread is the
convention-mitigated in-process single-step above (worker pinned resident,
`Truncated` exposed) — chosen because it runs on any x86-64 Linux with no privilege.
When you would rather **refuse** the in-process `EFLAGS.TF` arm outright on a managed
process and route to a crash-proof tier, set `ASMTEST_WHOLEWINDOW_SAFE_MANAGED=1` (or
pass `safeManaged: true` to the .NET empty ctor — the parameter wins over the env).
`asmtest_hwtrace_managed_runtime_present()` scans `/proc/self/maps` for the CoreCLR /
JVM / Mono images to decide "is this a managed process".

With the policy active on a managed process, the native
`asmtest_hwtrace_begin_window` returns the distinct `ASMTEST_HW_EMANAGED` status (not
`ASMTEST_HW_EUNAVAIL` — so a caller tells a *policy refusal* from a *genuinely absent
tier*), and the .NET empty ctor routes the window instead of arming TF:

| Order | Route | When | `ww.Route` |
|---|---|---|---|
| 1 | **Intel PT** whole-window | bare-metal Intel PT + `perf_event_paranoid < 0` / `CAP_PERFMON` | `pt` |
| 2 | **§D3 out-of-process stepper** (`outOfProcess: true`) | otherwise, where ptrace is permitted | `oop` |
| 3 | **Honest self-skip** | neither available — `SkipReason` names both misses | *(unarmed)* |

In-process TF is **never** selected on this path (`ww.Route` is `pt` / `oop`, never
`inproc`). With the policy inactive (the default), the ctor behaves exactly as
today — `ww.Route == "inproc"` and every pre-existing scope is byte-identical.

The `oop` route inherits the inline out-of-process window's contract: its region-free
stepper single-steps the arming thread, so it is best-effort for a **resident/offloaded**
managed body (the [crashproof-showdown](https://github.com/wilvk/asm-test/blob/main/examples/dotnet/crashproof-showdown/Program.cs)
shape) and can abort if held open across a background GC. The **fully robust** managed
whole-window remains the range-based `AsmTrace.Window(Action)` factory (its stepper runs
the runtime at native speed and steps only the published range); reach for it when the
body does substantial managed work on the arming thread.

The .NET binding is the reference: see
[docs/bindings/dotnet.md](../../bindings/dotnet.md) for the `AsmTrace` /
`AsmTrace.Method` surface and the §D0.1/§D0.2 method-naming path. The async-hop
model (following one logical operation across `await` thread hops) is a further,
opt-in layer not covered here.

## Statistical edges out of band: the AMD IBS-Op lane

The four backends above produce **exact** traces — ordered offsets that can back
a parity assertion. asm-test also carries one **statistical** hardware producer:
the AMD **IBS-Op** lane ([asmtest_ibs.h](../../../include/asmtest_ibs.h),
`src/ibs_backend.c`). IBS (Instruction-Based Sampling) tags one retired
instruction per sampling window, and when the tagged op is a taken branch the
sample carries both its **source and target address** — a real, sampled
`from → to` control-flow edge. Aggregated over a window this yields a hot-edge /
hot-block histogram of a running process.

Three properties make it worth having as its own lane:

- **It works where the exact AMD backend cannot.** The branch stack the
  `AMD_LBR` backend needs is Zen 4+ LbrExtV2 silicon (Zen 3 BRS exists but this
  tree cannot open it yet); on a **Zen 2** host every exact
  hardware backend self-skips. IBS ships on every Zen, so it is the one
  hardware branch source on that class of machine.
- **It is out of band and unprivileged.** It attaches `perf_event_open` to any
  same-uid pid — **no ptrace, no single-step, no elevated privilege** (the
  kernel's `swfilt` software filter makes user-only sampling open at the
  default `perf_event_paranoid=2`). The target runs at full speed, completely
  unperturbed — exactly the safe observation mode for the managed/JIT targets
  the section above declares hostile to in-process stepping.
- **It is honest about being statistical.** It fills its own shape
  (`asmtest_ibs_survey_t`: aggregated edges plus `samples` / `branch_samples` /
  `lost` / `throttled` provenance), **never** an `asmtest_trace_t`, and is not
  a member of the exact-backend cascade — a sampled edge proves that edge *was*
  taken; absence proves nothing, so it can never back a parity assertion.

```c
#include <asmtest_ibs.h>

int  asmtest_ibs_available(void);           /* 0 off-AMD / no IBS / no swfilt   */
const char *asmtest_ibs_skip_reason(void);  /* why, exactly                     */
int  asmtest_ibs_survey_pid(pid_t tid, unsigned ms, const asmtest_ibs_opts_t *,
                            asmtest_ibs_survey_t *out);      /* one thread     */
int  asmtest_ibs_survey_process(pid_t pid, unsigned ms, const asmtest_ibs_opts_t *,
                                asmtest_ibs_survey_t *out);  /* every thread   */
void asmtest_ibs_survey_free(asmtest_ibs_survey_t *);
```

`survey_process` opens one perf channel per thread of the target (with a
mid-window rescan for threads spawned after start) and merges everything into
one histogram sorted by count. Limits, stated rather than hidden: a thread born
*and* dead within one window can be missed; the kernel throttles sustained
sampling, so a **longer window** — not a shorter period — is how you buy more
cold-edge recall; and fidelity is always statistical.

Three consumers ship today. The interactive one is **`asmspy --sample` / TUI
mode 7** ([asmspy](asmspy.md)) — the hot-edge view that is safe on a live JIT.
The AMD **whole-window statistical survey** (`asmtest_hwtrace_sample_window_amd`)
**falls back to an IBS-Op survey when the branch stack is absent** — so on a
Zen 2 host it now returns a real hot-method histogram instead of `EUNAVAIL`
(set `ASMTEST_FORCE_IBS_SURVEY=1` to force the IBS path on branch-stack hosts
for cross-validation; the branch-stack path is byte-identical otherwise).

And **`asmtest_trace_call_auto`'s `ASMTEST_TRACE_IBS_PRECOVER` bit**
(`include/asmtest_trace_auto.h`) points an IBS-Op survey at the block-step
tier's own decode cost rather than at picking a trace region: when the
cross-tier auto cascade falls all the way to the rootless BTF block-step rung
(no branch-record hardware completed the capture), the bit forks a bounded
~30ms warm-up child, surveys it, and hands the resulting covered-block
histogram to the block-step reconstructor as a pre-cover table — a pure
decode-cost memoization (see the pre-cover entry above), never a change to
what gets recorded. This is the one place a *statistical* producer feeds an
*exact* one, and the boundary is exact: pre-cover can only skip re-decoding a
block it has already reconstructed once from real bytes: it can never
fabricate or drop an instruction. On a live Zen 2 host a hot loop's
branch-probe decode calls dropped to zero once the warm-up survey covered its
two basic blocks. The bit is opt-in and a proven no-op wherever it does not
apply — most hosts reach a complete trace before the block-step rung, since
the in-process single-step backend a few paragraphs up is itself a
ceiling-free floor.

The lane self-validates like everything else: `examples/ibs_probe.c` prints the
capability table (and the precise skip reason elsewhere), `make ibs-test` runs
the pure-decoder unit tests on any host plus the live out-of-band capture tests
on AMD, and `make docker-hwtrace-ibs` is the containerized lane (user-only IBS
works unprivileged in a container; the perf syscall must be allowed by seccomp).

## Known limitations

- **Single active region, single thread** in the MVP — capture state is one
  process-global slot; bracket one routine per begin/end pair.
- **Whole-window managed capture needs PT/LBR or the ptrace stepper, never in-process
  single-step** (above) — the whole-window single-step form works but self-truncates
  on a runtime and perturbs the stepped thread's timing, and an in-process TF window
  over code that blocks `SIGTRAP` (`pthread_create`, any `sigprocmask` block) is
  **fatal by kernel design**. `AsmTrace.Method()` is **not** subject to this — it
  lazy-arms only the body (Option B, the footgun note above). The clean whole-window
  PT path is hardware-gated.
- **Bare-metal capture needs perf privilege.** Intel PT needs a bare-metal Intel
  host; AMD LBR needs a Zen 4+ host with the perf branch stack permitted. Neither
  runs under standard CI's default sandbox (the tier self-skips). AMD LBR samples its
  branch stack at the PMU, so it `truncated`s a too-fast single-shot routine —
  single-step is the deterministic in-process backend for that case. (The
  [IBS-Op lane](#statistical-edges-out-of-band-the-amd-ibs-op-lane) is the
  exception on the privilege axis — user-only, unprivileged — but it is
  statistical, never exact.)
- **AMD LBR's 16-deep stack is lifted by Tier-B window stitching**; the remaining
  ceiling is the data ring (extend via `data_size`) plus PMI throttling. On a stitch
  gap or sample loss the trace comes back `truncated` — re-resolve under
  `CEILING_FREE`. The reach levers are documented in
  [Tuning AMD LBR window reach](amd-lbr-tuning.md).
- **CoreSight is a scaffold** pending AArch64 board access — it always self-skips.
- **Single-step is x86-64 Linux/macOS** (Windows/AArch64 planned); the
  out-of-process ptrace form adds AArch64. It targets well-behaved compute routines
  with stable bytes — self-modifying code, `POPF`/`IRET`, and in-routine signal
  handlers are flagged `truncated`, not emitted as complete.

---

## See also

- [Native runtime tracing](native-tracing.md) — the full native-trace family: the
  DynamoRIO tier, the out-of-process W2 / ptrace stepper, the foreign-JIT resolution
  toolkit, the time-aware code-image recorder, and cross-tier auto-selection.
- [Execution traces](traces.md) — the shared `asmtest_trace_t` shape and the
  emulator trace model.
- [Disassembly](../disassembly.md) — rendering recorded offsets back to instruction text.
- [Language bindings](../../bindings/index.md) — driving the tier from another language.
- [asmspy](asmspy.md) — the interactive front-end, including the `--sample`
  statistical hot-edge view built on the IBS lane.
- [asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) — the API header.
- [asmtest_ibs.h](../../../include/asmtest_ibs.h) — the statistical IBS-Op lane's header.
