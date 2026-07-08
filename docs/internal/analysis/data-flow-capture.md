# Analysis: what would data-flow capture require?

*Status: analysis / findings. This document records a design investigation, not
shipped behaviour. It is the sequel question to the control-flow tracing story: the
shipped tiers record **which instructions ran** (the shared
[`asmtest_trace_t`](../../../include/asmtest_trace.h) — ordered instruction offsets
+ basic blocks), and this note asks what it would take to also record **how data
moved** through them. For the tiers themselves see the siblings
[jit-runtime-tracing.md](jit-runtime-tracing.md) (the foreign-JIT / hardware-trace
face) and [scoped-inprocess-tracing.md](scoped-inprocess-tracing.md) (the
cooperative `using`-block face); for where the boundary is drawn today see the
published [scoped-tracing guide](../../guides/tracing/scoped-tracing.md) ("**not**
register/memory values per step").*

## The boundary today

Every backend — the single-step / out-of-process ptrace stepper
([`asmtest_ptrace.h`](../../../include/asmtest_ptrace.h)), the hardware tier (Intel
PT / AMD LBR / CoreSight, [`asmtest_hwtrace.h`](../../../include/asmtest_hwtrace.h)),
and the DynamoRIO DBI tier ([`asmtest_drtrace.h`](../../../include/asmtest_drtrace.h))
— fills the same [`asmtest_trace_t`](../../../include/asmtest_trace.h#L44), which
holds **only** control flow: `insns[]` (ordered instruction offsets), `blocks[]`
(distinct basic-block starts), totals, and a `truncated` bit. No field carries a
register or memory **value**. The ptrace stepper's
[`read_pc_ret`](../../../src/ptrace_backend.c#L401) reads only PC (→ the trace), the
return register (→ the scalar return-value out-param), and SP/LR (→ block/descent
bookkeeping). The only tier that yields register/memory values per step is the
**emulator** (Unicorn, [`emu_result_t`](../../../include/asmtest_emu.h)) — and it does
so by *replay*, not by observing a live run.

So "report on data flows" is not a small knob on the existing trace; it is a second
capability. This note breaks down what it would take.

## "Data flow" is three layers, not one

The requirements differ sharply by which of these the caller actually wants:

| Layer | What it is | Where it lives |
|---|---|---|
| **L0 — value trace** | per executed instruction, the *values* it read/wrote (register operands + memory operands **with their effective addresses**) | a new capture sink + per-tier producers |
| **L1 — def-use graph** | for each operand read, an edge back to the instruction that last wrote that storage location — the actual "flow" | one shared analysis pass over L0 |
| **L2 — taint / slicing** | mark inputs, propagate along L1 edges → forward slice ("which outputs depend on X") / backward slice ("what produced this value") | shared pass over L1 |

**L1 and L2 are tier-independent** — pure passes over an L0 value trace. This mirrors
the existing architecture: today every backend fills one shared `asmtest_trace_t`;
data flow adds a second shared sink and two analysis passes over it, and any tier that
can produce L0 feeds them.

### The shared piece (build once)

- A value-trace record — call it `asmtest_valtrace_t`: per step, an instruction offset
  (already have) plus a small array of `{location, size, value, is_write}`, where
  `location` is a register id or a memory address. Same append/dedup/truncate discipline
  and caller-owned buffers as `asmtest_trace_t`, so a value trace can overflow honestly
  (`truncated`) exactly like a control trace.
- **L1 linker:** a `last-writer` map (`reg → step`, `mem-byte → step`); at each step,
  for every source location emit an edge `(producer_step → this_step)`. Online or
  offline; a straight pass.
- **L2:** seed a taint set on chosen inputs, propagate along L1 edges (forward), or walk
  edges backward from a value (backward slice).

The operand model L0 needs is already within reach: Capstone **detail mode is on**
and operands are already parsed in [`disasm.c`](../../../src/disasm.c#L315-L343) (it
extracts IMM operands + instruction groups today). Enumerating the `X86_OP_REG` /
`X86_OP_MEM` read-set and write-set (base/index/scale/disp) is an extension of code
that already exists, not new machinery.

## What each tier must add to produce L0

The ranking here is **not** the same as for control flow. Ordered by practicality.

### 1. Emulator tier (Unicorn) — least work, but *replay*, not observation

The hooks already exist: [`on_code`](../../../src/emu.c#L105) fires once per
instruction, and the [memory read/write hooks](../../../src/emu.c#L139) already
receive `(type, address, size, value)` — today used only for watchpoints and then
discarded.

**Required:** at each `on_code`, decode operands → read source registers via
`uc_reg_read`; take memory values from the mem hooks firing between code hooks;
capture destination values at the *next* code hook (Unicorn's code hook fires
*before* the instruction executes). Persist into `asmtest_valtrace_t`; run L1/L2.

**Fidelity catch (already documented):** the emulator re-executes *extracted bytes in
a virtual guest* — it does not observe the live process. For .NET you resolve the
JIT'd method bytes (jitdump / the code-image recorder) and replay; any value sourced
from the real heap, a runtime helper, or a syscall is wrong unless that state is also
seeded/modelled. Good for a self-contained method; **not** a faithful view of a real
run. CI-runnable, no hardware.

### 2. Out-of-process ptrace stepper — the only tier that captures data flow of the **real live .NET run**

This is the important one for managed runtimes. It reads the tracee's actual state, so
its values are real, and it is out-of-band, so it does not fight the runtime's signal /
JIT / code-cache machinery. Most of the plumbing is present:
[`read_pc_ret`](../../../src/ptrace_backend.c#L401) already issues `PTRACE_GETREGS`,
and `process_vm_readv` is already used to read tracee memory.

**Required to add:**

- Per step: read the **full** GP register file (the `GETREGS` call is already there — it
  currently pulls only rip/rax/rsp), **plus XMM/YMM** (`PTRACE_GETFPREGS` /
  `GETREGSET NT_X86_XSTATE`, not read at all today) for SIMD/FP data flow.
- Capstone-detail decode of the about-to-execute instruction → its read/write
  locations. For each **memory** operand, compute the effective address from the
  just-read registers (`base + index*scale + disp`), then `process_vm_readv` that
  address for the value read; capture writes at the next stop.
- **.NET wrinkle:** managed thread-local access is `gs:`-relative, so effective-address
  computation must add segment-base resolution (`fs_base` / `gs_base` from `GETREGS`).
- Feed `asmtest_valtrace_t`; run L1/L2.

**Cost:** brutal. The stepper is already ~100–1000× on the stepped thread; a full
`GETREGS` + `GETFPREGS` + N memory peeks + detail-decode *per instruction* multiplies
that. The natural mitigation is the **scoped model** — bound value capture to a small
`using` scope so the blast radius is a region, not a run. Within that scope it is the
only real, observed data flow of live managed code available on a host without Intel PT.

### 3. DynamoRIO DBI tier — the industry-standard substrate

DBI is what real taint / data-flow engines are built on (libdft / Triton on Pin;
DynamoRIO is the equivalent). The tier already
[instruments registered ranges](../../../include/asmtest_drtrace.h#L17); it can emit
operand values + memory addresses inline at ~10–50× rather than ptrace's ~1000×. For
.NET the documented constraint is that *in-process* DBI collides with the runtime's
signal / code-cache machinery — but running DynamoRIO as the **process container from
launch** (`drrun` over the whole `dotnet` process) sidesteps that. The work is a
drtrace client that records the value trace instead of just offsets. This is the most
credible substrate for production-grade data flow over real managed execution.

### 4. Hardware tier (Intel PT / AMD LBR / CoreSight) — cannot, alone

The silicon emits *branch* packets; there is no operand-value channel. Two escape
hatches, both heavy:

- **PTWRITE** (Intel) injects a chosen value into the PT stream — but the code must be
  *instrumented* to emit it, which puts you back in DBI / recompilation.
- **PT control-flow + emulator replay + input capture** — take the exact real path from
  PT, replay it in the emulator seeded with an initial register/memory snapshot,
  capturing every non-deterministic input (syscalls, RDTSC, shared-memory loads) to
  re-derive each value. This is deterministic record/replay (rr / PANDA-style): highest
  fidelity at lowest capture overhead, most engineering. It is exactly the "hardware
  gives control flow × emulator gives values" composition
  [jit-runtime-tracing.md](jit-runtime-tracing.md) already gestures at.

## The .NET-specific hard parts (beyond raw capture)

Raw L0 gets you `rdx ← load @0x7f… depends on the store at +0x2f`. Turning that into
something a .NET developer can use adds real work:

- **JIT'd / moving code — partly solved.** The code-image recorder
  ([`asmtest_codeimage.h`](../../../include/asmtest_codeimage.h)) +
  `asmtest_hwtrace_render_versioned` already give time-correct bytes and method identity
  (jitdump / `MethodLoadVerbose`). Reuse them to attribute values to the right method
  version.
- **GC object motion — the genuinely hard one.** A raw memory address changes meaning
  when the GC compacts. Coherent *memory* data flow across a GC needs to canonicalize
  addresses to `(object, field)` identity by hooking GC move events (EventPipe / ETW GC
  events); otherwise the def-use map aliases pre- and post-move locations.
- **Runtime-helper edges.** Values flow through allocation / write-barrier / generic-
  dictionary helpers. Either descend into them (the
  [call-descent machinery](../../../include/asmtest_ptrace.h#L243) already exists) or
  model them as summary data-flow edges.
- **Asm values → managed variables.** To report `total depends on prices[i]` instead of
  registers, you need the JIT's IL-to-native variable map (rich debug info / PDB).
  Without it, data flow stays at the assembly level.

## Scale

A value trace is far larger than a control-flow trace — several operands × up to 16–32
bytes per step versus one 8-byte offset. Any producer needs streaming / ring buffers /
sampling, or a bounded scope. The **scoped `using`-block model is the natural bound**:
cap data-flow capture to a small region and the size problem stays contained.

## Bottom line / smallest viable path

- The **analysis layers (L1/L2) are tier-neutral and modest** — one value-trace struct
  + a last-writer map + a slicer. Build that once, shared, exactly as `asmtest_trace_t`
  is shared today.
- **Fastest to a demo:** the **emulator tier** — hooks and operand decoding already
  exist; it is replay, honest about its limits, CI-runnable with no hardware.
- **Only faithful view of live .NET:** the **out-of-process ptrace stepper**, scoped
  tight to bound cost; needs full-register + XMM + operand-memory capture and
  segment-base effective-address math on top of existing plumbing.
- **Production-grade real-process data flow:** a **DynamoRIO client** run as the process
  container.
- **Hardware alone:** no — only via PTWRITE instrumentation or PT-path + emulator replay
  + input capture.

None of this is shipped; it is scoped here so the effort and fidelity trade-offs are on
record before any of it is committed to a plan.
