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
[`read_pc_ret`](../../../src/ptrace_backend.c#L431) reads only PC (→ the trace), the
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
can produce L0 feeds them. **Caveat (added by the 2026-07-12 cross-reference):** the
"pure pass" claim holds cleanly only for **register** def-use. **Memory** def-use is not
tier-neutral — the last-writer map keys on raw addresses, and under a moving GC (.NET) an
address changes meaning across a compaction, so coherent memory flow needs
`(object, field)` canonicalization from GC-move events (see "GC object motion" below).
Register def-use is a straight offline pass; memory def-use is gated on runtime metadata.

### The shared piece (build once)

- A value-trace record — call it `asmtest_valtrace_t`: per step, an instruction offset
  (already have) plus a small array of `{location, size, value, is_write}`, where
  `location` is a register id or a memory address. Same append/dedup/truncate discipline
  and caller-owned buffers as `asmtest_trace_t`, so a value trace can overflow honestly
  (`truncated`) exactly like a control trace.
  - **Address-space normalization contract (added 2026-07-12).** The existing sink is
    *not* address-uniform: [`emu.c`](../../../src/emu.c#L113) appends `(address - base)`
    routine-relative offsets, while the single-step backend stores **absolute** addresses
    in whole-window mode but **offsets** in region mode
    ([`ss_backend.c`](../../../src/ss_backend.c)). A value trace mixes an effective
    **memory** address (inherently absolute) with instruction offsets, so `location` must
    carry its space explicitly (tagged: reg-id / absolute-addr / routine-offset) and the
    L1 linker must normalize per capture **mode**. This contract was implicit in the
    original note and is called out here so it is fixed before the sink is laid out.
- **L1 linker:** a `last-writer` map (`reg → step`, `mem-byte → step`); at each step,
  for every source location emit an edge `(producer_step → this_step)`. Online or
  offline; a straight pass.
- **L2:** seed a taint set on chosen inputs, propagate along L1 edges (forward), or walk
  edges backward from a value (backward slice).

The operand model L0 needs is already within reach: Capstone **detail mode is on**
(seven `cs_option(CS_OPT_DETAIL, ON)` sites) and operands are already iterated in
[`disasm.c`](../../../src/disasm.c#L375-L391) (it extracts `X86_OP_IMM` immediates +
instruction groups today; the operand loop also has an AArch64 arm). Enumerating the
`X86_OP_REG` / `X86_OP_MEM` read-set and write-set (base/index/scale/disp) is an
extension of code that already exists, not new machinery. **Two additions the original
note glossed (2026-07-12):** (1) read-vs-write **direction** is *not* derivable from the
operand list alone — it needs the per-operand `.access` field (`CS_AC_READ`/`CS_AC_WRITE`)
or `cs_regs_access`; (2) **implicit** operands (`eflags`, `rsp` on push/pop/call/ret,
string-op counters) are absent from `operands[]` and require `detail->x86.eflags` +
`regs_read[]`/`regs_write[]`. A faithful read/write set is therefore larger than "iterate
`X86_OP_REG`/`X86_OP_MEM`", and a hot-path enumerator should hold one persistent `csh`
rather than the per-call `cs_open`/`cs_close` the current helpers use.

## What each tier must add to produce L0

The ranking here is **not** the same as for control flow. Ordered by practicality.

### 1. Emulator tier (Unicorn) — least work, but *replay*, not observation

The hooks already exist: [`on_code`](../../../src/emu.c#L105) fires once per
instruction, and the [memory read/write hooks](../../../src/emu.c#L139) already
receive `(type, address, size, value)` — today used only for watchpoints and then
discarded.

**Required:** at each `on_code`, decode operands → read source registers via
`uc_reg_read` (the code hook fires *before* the instruction, so an in-hook `uc_reg_read`
reads the *source* state); take memory values from the mem hooks firing between code
hooks; capture destination values at the *next* code hook. Persist into
`asmtest_valtrace_t`; run L1/L2. **Correction (2026-07-12):** un-discarding the value in
[`on_mem_access`](../../../src/emu.c#L145) is *necessary but not sufficient* — a plain
`UC_HOOK_MEM_READ` does **not** populate the `value` argument (reads deliver 0), so
load-value capture needs `UC_HOOK_MEM_READ_AFTER`; stores use `UC_HOOK_MEM_WRITE`. The
mem hooks are also only armed today when a watchpoint is set
([`emu.c`](../../../src/emu.c#L388)), so they must be installed unconditionally for a
value trace.

**Fidelity catch (already documented):** the emulator re-executes *extracted bytes in
a virtual guest* — it does not observe the live process. For .NET you resolve the
JIT'd method bytes (jitdump / the code-image recorder) and replay; any value sourced
from the real heap, a runtime helper, or a syscall is wrong unless that state is also
seeded/modelled. Good for a self-contained method; **not** a faithful view of a real
run. CI-runnable, no hardware.

### 2. Out-of-process ptrace stepper — the only **out-of-band** tier that captures data flow of the real live .NET run

This is the important one for managed runtimes. It reads the tracee's actual state, so
its values are real, and — unlike the DynamoRIO tier below, which also observes real
managed values but *in-band* via recompilation — it is **out-of-band**, so it does not
fight the runtime's signal / JIT / code-cache machinery. That out-of-band-ness, not
"only tier that sees live values", is its distinguishing property. Most of the plumbing
is present: [`read_pc_ret`](../../../src/ptrace_backend.c#L431) already issues
`PTRACE_GETREGS` — which copies the **entire** `struct user_regs_struct` every step, so
capturing the full GP file adds ~0 syscalls; only the extraction is selective today — and
[`process_vm_readv`](../../../src/ptrace_backend.c#L315) is already used to read tracee
memory.

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

**Cost:** brutal. The literature puts naive single-step at **~10³–10⁵×** on the stepped
thread (each step is ~4 context switches; a lean in-process C stepper sits near the low
end, ~1000×, but 100× is not achievable for a true single-step loop — the original note's
"100–1000×" floor was optimistic). A full `GETREGS` + `GETFPREGS`/`NT_X86_XSTATE` + N
memory peeks + detail-decode *per instruction* multiplies that further. The natural
mitigation is the **scoped model** — bound value capture to a small `using` scope so the
blast radius is a region, not a run. Within that scope it is the only real, **out-of-band**
observed data flow of live managed code available on a host without Intel PT.

### 3. DynamoRIO DBI tier — the industry-standard substrate

DBI is what real taint / data-flow engines are built on (libdft / Triton on Pin;
DynamoRIO is the equivalent). The tier already
[instruments registered ranges](../../../include/asmtest_drtrace.h#L17). A DBI client
*can* emit operand values + memory addresses inline at ~10–50× rather than ptrace's
~1000× — but that figure describes **greenfield** work, not the shipped tier. **Reality
check (2026-07-12):** the current client
([`drtrace_client.c`](../../../src/drtrace_client.c)) records offsets via
`dr_insert_clean_call` per block/instruction and deliberately **excludes `drreg`** (raw
core API only). Per-instruction operand capture bolted onto that clean-call pattern would
run far worse than 10–50× until it is rebuilt with inlined instrumentation, buffered
writes, and hand-rolled scratch-register management. For .NET the documented constraint is
that *in-process* DBI collides with the runtime's signal / code-cache machinery. Running
DynamoRIO as the **process container from launch** (`drrun` over the whole `dotnet`
process) would sidestep that — but note this path is **aspirational**: there is no `drrun`
launcher in-tree today (the shipped tier is purely the in-process `dr_app_setup/start`
path, which the header itself flags as unreliable for re-attach). The signal-collision
half *is* already mitigated (`DR_SIGNAL_DELIVER` for JVM/.NET null-check `SIGSEGV`). This
remains the most credible substrate for production-grade data flow over real managed
execution — the *target*, not current capability.

### 4. Hardware tier (Intel PT / AMD LBR / CoreSight) — cannot, alone

The silicon emits *control-flow* packets and no operand-value packets **by default**
(PT does also emit timing/paging/mode/power packets — "branch only" is a simplification;
the load-bearing fact is that no *value* channel exists in the trace). Three escape
hatches:

- **PTWRITE** (Intel) injects a chosen value into the PT stream — but the value only
  appears if the program *executes* a `PTWRITE` instruction (and the config bits are set),
  i.e. the code must be *instrumented* to emit it, which puts you back in DBI /
  recompilation. Alder-Lake+ only; costs cycles per emission.
- **Statistical value channels (partial middle ground, 2026-07-12).** Not full data flow,
  but not "no values" either: Intel **PEBS** copies the GP register file + EFLAGS + IP and
  the sampled *data linear address* into its buffer per sample; AMD **IBS-op** records the
  sampled load/store *data address* (address, not value). These give sparse, sampled
  operand-address flow without instrumentation — worth noting the original note omitted
  them. (CoreSight ETM has **no** PTWRITE equivalent, so on ARM instrumentation or replay
  are the *only* data-flow options — a portability asymmetry.)
- **PT control-flow + emulator replay + input capture** — take the exact real path from
  PT, replay it in the emulator seeded with an initial register/memory snapshot,
  capturing every non-deterministic input (syscalls, RDTSC, shared-memory loads) to
  re-derive each value. This is the deterministic record/replay approach — **PANDA
  `taint2`** and Microsoft **TTD/iDNA** are the exemplars (correction: **`rr` does *not*
  belong here** — it uses neither Intel PT nor an emulator; it re-executes the native
  binary via retired-branch PMU + ptrace, a different mechanism). Highest fidelity at
  lowest capture overhead, most engineering. It is exactly the "hardware gives control
  flow × emulator gives values" composition
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
  addresses to `(object, field)` identity by hooking GC move events; otherwise the def-use
  map aliases pre- and post-move locations. The concrete events (verified 2026-07-12):
  ETW/EventPipe `GCBulkMovedObjectRanges` carries `OldRangeBase` / `NewRangeBase` /
  `RangeLength` (under `GCHeapSurvivalAndMovementKeyword`) — enough to remap a *raw
  address*; mapping to `(object, field)` **identity** additionally needs
  `GCBulkType`/`Node`/`Edge`. **EventPipe** is the cross-platform (Linux/macOS) equivalent
  of Windows ETW and carries the same GC/JIT rundown events, so this is reachable on Linux
  — it does not require Windows. Method identity comes from `MethodLoadVerbose` (event id
  **143** on modern .NET Core/5+; the older Framework `136`/`141` ids differ — a parser
  must key off the live coreclr manifest).
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

Build order, each step naming the existing code it extends:

1. **Shared L0 sink `asmtest_valtrace_t`** — mirror the `asmtest_trace_t` pattern
   ([`asmtest_trace.h`](../../../include/asmtest_trace.h#L44)) with per-record
   `{tagged_location, value, size, is_write}` and `valtrace_append_*` fill points beside
   [`trace_append_insn`](../../../src/trace.c#L24). **Define the address-space
   normalization contract first** (reg-id / absolute / routine-offset; key off capture
   mode — the offset-vs-absolute split above).
2. **Operand read/write enumerator** on the existing Capstone detail decode — extend the
   already-iterating operand loop at [`disasm.c`](../../../src/disasm.c#L375) from
   IMM-only to the full read/write set (`.access` for direction, `regs_read`/`regs_write`
   + `x86.eflags` for implicit operands, `x86_op_mem` for effective addresses); one
   persistent `csh`.
3. **L1 last-writer / def-use** — pure pass for registers; memory gated behind GC-move
   events, backed by a hash/interval map (not the O(n) block-dedup scan).
4. **L2 slicer / taint** over L1 (adopt fast-path techniques if overhead matters).
5. **Emulator L0 first (CI demo)** — least work; extend
   [`on_code`](../../../src/emu.c#L108) + un-discard the value in
   [`on_mem_access`](../../../src/emu.c#L145) (with `UC_HOOK_MEM_READ_AFTER`), armed
   unconditionally. Replay, not observation — label it so.
6. **Scoped ptrace L0 (live .NET, out-of-band)** — the GP file is already fetched at
   [`read_pc_ret`](../../../src/ptrace_backend.c#L431); add the sink, one `NT_X86_XSTATE`
   read for XMM/YMM, `fs_base`/`gs_base` math, per-operand
   [`process_vm_readv`](../../../src/ptrace_backend.c#L315); bound by a `using` scope.
7. **DynamoRIO L0 (production target, largest lift)** — replace the clean-call recorder
   ([`drtrace_client.c`](../../../src/drtrace_client.c)) with inlined instrumentation +
   buffered writes + scratch-register management, a `drrun`-from-launch container, and
   managed-execution validation beyond the offset-only dotnet smoke test.
- **Hardware alone:** no — only via PTWRITE instrumentation, PEBS/IBS *statistical*
  address sampling (partial), or PT-path + emulator replay + input capture.

None of this is shipped; it is scoped here so the effort and fidelity trade-offs are on
record before any of it is committed to a plan.

## Cross-reference & validation (2026-07-12)

This note was re-reviewed against the code, the sibling docs, and the online literature
by a 19-agent adversarial cross-reference (9 finders → 9 skeptical verifiers →
synthesis). The structural thesis held; the corrections above were folded in inline. The
external-fact and overhead numbers, with sources:

- **DBI taint overheads.** libdft **1.14–6.03×** (utils) / 1.25–4.83× (servers) — but it
  *skips XMM/SSE/MMX*, so its low cost is partly a coverage tradeoff (directly relevant to
  the XMM/YMM requirement here). Bare DynamoRIO `inscount` ≈ **15× on SPEC CPU 2017**;
  TaintTrace ≈ **5.5×** — so the "DBI value-logging 10–50×" band is defensible. Fast-path
  work trends down (Taint Rabbit 1.7×, Sdft 1.58×, HardTaint ~9% via *selective hardware
  tracing* — the thread most relevant to this repo's HW-trace tier).
- **Corrected numbers.** ptrace single-step is **~10³–10⁵×**, not 100–1000× (each step is
  ~4 context switches). "Triton = hundreds of ×" was wrong for its *taint* mode (~29–37×);
  the big numbers belong to full symbolic execution (SymCC 127×, KLEE up to 10⁴×). `rr` is
  *not* a PT+emulator-replay engine (it re-executes natively via the retired-branch PMU);
  PANDA `taint2` / TTD are the correct exemplars for that bucket.
- **eBPF / ETW (auxiliaries).** Confirmed: eBPF has no
  per-instruction hook and cannot single-step (uprobe = one `INT3` at an address; the
  verifier forbids per-instruction following — the in-tree
  [`codeimage.bpf.c`](../../../bpf/codeimage.bpf.c) says so); the eBPF `struct pt_regs`
  context is **GP-only** (no XMM/YMM), so eBPF is a *coarse chosen-point* value tap
  (uprobe + `bpf_probe_read_user`), never L0. ETW is Windows-only but **EventPipe** is its
  cross-platform twin, so the .NET GC-move / method-identity metadata (the L1 memory
  prerequisite) is reachable on Linux. Two slips in an earlier take were corrected: the
  repo is **not** wholesale Linux-only (it ships a Win64 *runner* port — only the
  data-flow/hwtrace/bpf tiers are Linux-only), and a .NET DynamoRIO *harness* does exist
  (`bindings/dotnet/drtrace/`), though it validates only offset recording, not data flow.
- **Gaps surfaced.** Read/write direction needs `.access` (not derivable from the operand
  list); implicit operands (`eflags`, `rsp`, string counters) aren't in `operands[]`;
  Capstone DIET builds drop `.access`/`op_str`; a value trace's memory last-writer wants a
  hash/interval map, not the O(n) dedup scan; SIMD data flow is expensive and under-solved
  industry-wide (even libdft punts on it). IBS is a statistical MEMORY.md source, not one
  of the four in-tree HW-trace tiers (INTEL_PT/CORESIGHT/AMD_LBR/SINGLESTEP).
