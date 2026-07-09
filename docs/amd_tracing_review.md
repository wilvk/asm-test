# AMD Tracing – Review & Recommendations

## Overview
This document summarizes the audit of the AMD‑LBR tracing implementation in the `asm-test` repository, cross‑referencing the source code, design documentation, and Linux kernel/perf‑event best practices.

### Key Findings
| Area | Issue | Recommendation |
|------|-------|----------------|
| **Options struct size** | `asmtest_hwtrace_options_t` size may diverge from backend expectations (see CHANGELOG #385). | Add a `static_assert` to validate struct size and document each AMD field. |
| **Capability probes** | `asmtest_amd_snapshot_available()` returns a plain `0/1` without diagnostic details. | Return an enum and expose a status string (`asmtest_amd_snapshot_status`). |
| **Channel overflow** | `asmtest_addr_channel_t` writes are unchecked and can overflow when tracing long routines. | Guard writes with capacity checks and set `truncated` flag on overflow. |
| **Logging** | Minimal logging in AMD backend and stealth helper. | Introduce `asmtest_log_*` macros and emit messages for CPUID probes, depth detection, overflow, and fallback decisions. |
| **Performance** | `amd_replay()` decodes every instruction between branches, leading to repeated work for hot loops. | Add a small LRU cache of instruction lengths to avoid redundant Capstone calls. |
| **Stitching edge cases** | First window may be incomplete, yet is used unchanged, potentially causing false‑complete traces. | Detect incomplete first window, mark trace truncated, and request a snapshot at routine entry. |
| **Documentation gaps** | DESIGN.md mentions AMD64 ABI but lacks AMD‑LBR backend specifics; plan lacks failure‑path diagrams. | Add an “AMD LBR backend” subsection to DESIGN.md and a Mermaid flowchart in the AMD‑tracing plan. |
| **Permission handling** | No guidance on `perf_event_paranoid` or required capabilities. | Document the required sysctl and provide a helper script to verify/set it. |

### Action Plan (high‑level)
1. **Add static assert** – `include/asmtest_trace_auto.h`.
2. **Improve capability probes** – `src/amd_backend.c`.
3. **Guard channel writes** – `src/stealth_helper.c`.
4. **Introduce lightweight logger** – new `asmtest_log.h` and usage.
5. **Update docs** – add AMD backend section to DESIGN.md; add flowchart to `docs/internal/plans/amd-tracing-plan.md`.
6. **(Optional) Instruction‑length cache** – in `amd_replay()`.
7. **Add mock‑based unit tests** and CI dry‑run job.

---

For detailed code snippets, see the corresponding sections in this repository:
- `src/amd_backend.c` (capability probes, replay, stitching)
- `src/stealth_helper.c` (channel handling)
- `include/asmtest_trace_auto.h` (options struct)
- `DESIGN.md` (add AMD‑LBR subsection)
- `docs/internal/plans/amd-tracing-plan.md` (add failure‑path diagram).

*End of review.*
