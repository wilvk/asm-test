# Data-flow tracing (values, def-use, slicing)

The tracing backends elsewhere in this section answer *which instructions ran*.
The **data-flow tier** answers *which values flowed where*: it records the
operand-level values a run touched (an **L0 value trace**), builds a
**def-use graph** from them (**L1**), and answers reachability queries over that
graph — forward and backward **slices** (**L2**).

It is a pure analysis pipeline layered on the same tier-neutral capture idea as
the execution traces: a **producer** fills an `asmtest_valtrace_t`, and the
analysis runs on whatever producer filled it, so the same def-use + slicing code
works across capture mechanisms.

## The pipeline

| Layer | What it is | Key API |
|---|---|---|
| **L0 — value trace** | Per executed step: the instruction offset, and each operand's location (register or memory), width, and captured value. | `asmtest_valtrace_new` / `_append` |
| **L1 — def-use graph** | The last-writer relation over the L0 stream: an edge from the step that *defined* a value to each step that *used* it, keyed by location (register id or absolute address). | `asmtest_defuse_build` |
| **L2 — slice** | Reachability over the def-use edges: a **forward** slice ("what does this value influence?") or **backward** slice ("what produced this value?"). | `asmtest_slice_forward` / `_backward` / `_contains` |

A def-use edge forms wherever a later step *reads* a location a previous step
*wrote* — through a register **or** through memory (a store then a load at the
same address). Independent chains stay disjoint: no spurious cross-links.

## Producers

The analysis is producer-agnostic. Three producers fill the same value trace:

- **Emulator (L0 oracle)** — replay under Unicorn; deterministic, runs in CI,
  used to cross-validate the live producers.
- **Scoped ptrace** — single-step a routine (fork or attach) and read live
  register + memory values out of band. Real values; the same
  `asmtest_valtrace_t` the analyzers consume.
- **DynamoRIO (in-band)** — instrument a target under real DR and capture
  per-instruction operand values in-band; the whole-process analog of the
  scoped producer. (Software DBI: Intel or AMD, no privilege.)
- **Intel Pin probe-mode** — splice a *probe* (a jump) at a named routine's entry
  and exit and read its SysV argument / return registers plus a pointed-to buffer
  at **native speed** (no code cache), into the same `at_val_rec_t` records. A
  test/oracle-only lane, cross-checked against the ptrace producer — see below.

## Pin probe-mode argument/return capture (test/oracle only)

`make pin-probe-test` (part of `make docker-pintool`, x86-64 Linux) runs an Intel
Pin **probe-mode** tool that captures the **values** a function is called with and
returns, at native speed: Pin splices a jump at the routine's first bytes into a
small trampoline, and the application runs natively between probes with no software
code cache. It records the SysV integer/FP **argument** registers at entry and the
**return** register(s) + flags at exit as `at_val_rec_t` records, and — for a
pointer argument — up to a **4 KiB** default cap (the `-ptrcap` knob) of the buffer
it points at. The capture is proven by diffing it against the out-of-process ptrace
single-step stepper on the same routine: two independent producers must agree on
the argument and return registers.

- **Never trust a pointer.** A captured pointer is validated against the target's
  mapped ranges before any read, and the read is clamped to both the 4 KiB cap and
  the end of the containing mapping, so an invalid or short pointer is **refused**
  (a zero-length record) rather than faulting the target. (`PIN_SafeCopy` is
  JIT-mode-only, so probe mode validates against `/proc/self/maps` — which, running
  in the application's own address space, *is* the target's map.)
- **Captured buffers are sensitive.** A pointed-to buffer may contain secrets
  (keys, tokens, PII); treat a probe capture as a sensitive artifact.
- **Honest refusals.** A routine Pin cannot probe — too short to hold the
  up-to-14-byte probe, or non-relocatable — is reported as an explicit per-target
  **skip with a reason** (`too short` / `not relocatable` / `not found`), never a
  silent miss. Pin publishes no machine-readable refusal reason, so the tool
  synthesizes one from `RTN_Size` and the probed-insertion pre-checks.
- **Scope.** x86-only and **test/oracle-only**: Intel Pin is proprietary freeware
  (digest-verified at test time, never linked into `libasmtest`, `libasmtest_emu`,
  or any shipped package), exactly as DynamoRIO is handled. Design note:
  [capture-args-returns.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/analysis/capture-args-returns.md).

## Managed-runtime identity

For managed (.NET) targets, two pure helpers attribute raw addresses to stable
identities, so a slice survives the runtime moving code and objects:

- **Method identity + version** (`asmtest_method_resolve_pc`) — resolve a PC to
  its owning JIT method + version, newest-version-wins across a tiered re-JIT.
- **GC-move canonicalization** (`asmtest_gcmove_canon` /
  `asmtest_gcmove_canonicalize`) — remap heap addresses across a compacting GC
  (fed live by `GCBulkMovedObjectRanges`) so a value's def-use survives the move
  without pre/post-move false aliasing.

- **Runtime-helper summary edges** (`asmtest_defuse_build_summarized`) — model a
  recognized CoreCLR helper call (allocation, write-barrier, generic-dict) as a
  summary edge (inputs → outputs), so caller data-flow connects *across* the
  helper without instrumenting its body.

## Language bindings

The analysis library `libasmtest_dataflow` (`make shared-dataflow`) is wrapped by
language bindings. Each exposes the pure helpers and a `ValueTrace` over the full
L0→L1→L2 pipeline:

| Language | Module | Build + test |
|---|---|---|
| Python | `asmtest.dataflow` (ctypes) | `make dataflow-python-test` |
| C++ | `bindings/cpp/asmtest_dataflow.hpp` (header-only) | `make dataflow-cpp-test` |
| Node | `bindings/node/dataflow.js` (koffi) | `make dataflow-node-test` |

```python
from asmtest import dataflow as df

with df.ValueTrace() as vt:
    vt.step(0x00, writes=[(df.LOC_REG, 10)])                 # def r10
    vt.step(0x03, reads=[(df.LOC_REG, 10)], writes=[(df.LOC_REG, 11)])
    vt.step(0x06, reads=[(df.LOC_REG, 11)], writes=[(df.LOC_REG, 12)])
    vt.forward_slice(0)    # -> {0, 1, 2}  (what r10 influences)
    vt.backward_slice(2)   # -> {0, 1, 2}  (what produced r12)
```

```{toctree}
:maxdepth: 1
:hidden:
```
