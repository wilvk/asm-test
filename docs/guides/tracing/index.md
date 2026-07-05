# Tracing

asm-test can record **which code a routine actually executed** — an ordered
instruction trace and distinct basic-block coverage. Several backends fill the
**same** `asmtest_trace_t` shape (ordered instruction offsets, distinct
basic-block offsets, totals, a truncation bit), so a test can switch backends
without changing how it reads coverage, and the optional
[Capstone annotation layer](../disassembly.md) renders any backend's recorded
offsets back to instruction text.

This section covers the trace substrate from the isolated emulator through the
native, real-CPU tiers:

| Page | Backend(s) | Records | Runs where |
|---|---|---|---|
| [Execution traces and coverage](traces.md) | Unicorn emulator | guest blocks + instructions, coverage unions, source-line maps, lcov | any host |
| [Native runtime tracing](native-tracing.md) | DynamoRIO (software DBI) + single-step (`EFLAGS.TF`) | native blocks + instructions, in-process | DynamoRIO: Linux x86-64; single-step: x86-64 Linux **or macOS** |
| [Hardware tracing](hardware-tracing.md) | Intel PT / AMD LBR / ARM CoreSight / single-step | native blocks + instructions, near-zero capture overhead | bare-metal Intel / AMD / AArch64 (single-step: x86-64 Linux/macOS) |
| [Scoped tracing](scoped-tracing.md) | any hardware backend, cross-language | *import + scope* over a region → the assembly that executed, rendered on close | x86-64 Linux/macOS (single-step) |

Start with the [emulator trace model](traces.md) for the shared trace API,
coverage helpers, and reporting — the concepts carry across every backend. The
two native tiers are **optional, advanced, and self-skipping**: they are kept out
of the core `libasmtest` and the `libasmtest_emu` superset, build only when their
toolchain is present, and degrade to a clear "skipped" message otherwise.

> **Diagram:** [Trace and coverage backends](../../reference/diagrams.md#trace-and-coverage-backends)

```{toctree}
:maxdepth: 1
:hidden:

traces
native-tracing
hardware-tracing
scoped-tracing
```
