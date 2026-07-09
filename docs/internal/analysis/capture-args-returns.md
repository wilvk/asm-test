# Analysis: capturing function arguments and return values

Status: draft — an engineering analysis of approaches to capture and trace
values passed between a process and libraries (entry/exit arguments, buffers,
returns, and related memory).

This note summarizes practical capture techniques, trade-offs, limitations,
and recommended prototypes. It is intended to guide implementing low-level
instrumentation in `asm-test` and to provide a reference for choosing a
capture strategy during analysis, debugging, malware research, or profiling.

## Goals

- Capture function entry/exit arguments and return values for selected
  functions or all library calls.
- Capture pointed-to buffers/structs where feasible (strings, serialized data).
- Support live attach and process-launch instrumentation modes.
- Produce structured trace artifacts compatible with the project's trace
  formats (future `asmtest_valtrace_t`) and with offline analysis tools.

## Approaches

1. LD_PRELOAD / wrapper library
   - Mechanism: supply wrapper functions that call the real symbol and log
     arguments and return values (optionally reading pointed-to memory).
   - Pros: simple, low development cost, excellent visibility into arg
     semantics, works without ptrace or kernel privileges.
   - Cons: must be injected at process startup (or `dlopen`ed into a running
     process), misses non-dynamic-symbol calls (direct syscall, static
     linking), potential ABI/version fragility.

2. Dynamic Binary Instrumentation (DBI)
   - Mechanism: run the process under a DBI harness (DynamoRIO, Frida, Pin)
     and insert instrumentation at function entry/exit or basic blocks.
   - Pros: low-overhead compared to ptrace, supports full-value reads
     (registers/XMM/etc), works for JITed and self-modifying code if launched
     under DBI.
   - Cons: intrusive (process must be started under DBI or injected), runtime
     compatibility concerns (signals, code cache interaction).

3. ptrace attach + memory/reg reads
   - Mechanism: attach to a running PID, set breakpoints or use single-step /
     block-step to stop at call/ret sites, read `PTRACE_GETREGS` / `GETFPREGS`
     and `process_vm_readv` to fetch pointed memory.
   - Pros: works for live attach without prior instrumentation, exact ordering
     and values possible.
   - Cons: very high overhead per event, complex multi-thread handling,
     requires elevated privileges or ptrace_scope relaxation.

4. Uprobes / eBPF probes
   - Mechanism: attach kernel probes to user-space function entry/exit (`uprobes`)
     and run BPF to snapshot selected arguments or pointers (`bpf_probe_read_user`).
   - Pros: low overhead, can be used in production for limited captures,
     safe kernel-enforced execution limits.
   - Cons: verifier restrictions (limits on loops/large reads), limited
     ability to dereference complex pointers, kernel capability required.

5. PLT/GOT patching / inline trampolines
   - Mechanism: patch PLT entries or write trampolines into code to intercept
     calls and record arguments before shipping to the real callee.
   - Pros: flexible, can be low-latency; works on already-running processes
     with careful code-patching (requires writing to code pages).
   - Cons: invasive, needs careful synchronization, PIC/relocation handling,
     and SIGSEGV/ETW-like side-effects risk.

## What to capture (practical data model)

- Registers: general-purpose (RDI/RSI/RDX/RCX/R8/R9 on AMD64), return regs
  (RAX/XMM0), flags when relevant.
- XMM/YMM/ZMM registers for floating/SIMD arguments and returns.
- Memory pointed to by arguments: strings, buffers, and small structs; respect
  configurable size limits and sampling to bound cost.
- Stack-based arguments beyond register-passed ones (read via `process_vm_readv`).
- Timestamps, thread-id, and call-site IP to attribute where the call occurred.

Be defensive: never trust pointer validity. Validate against the target's
mapped ranges and limit reads (configurable cap, e.g., 4 KiB per pointer).

## Trade-offs & recommendations

- For development and controlled analysis, **LD_PRELOAD** is the fastest path
  to get rich semantic traces for a small set of functions.
- For live-attach fidelity, **ptrace** provides the straightest mapping at the
  cost of performance; use it for short captures or correctness validation.
- For production or higher-throughput monitoring, **uprobes/eBPF** strikes a
  good balance (limited derefs, low overhead).
- For full-scale, long-running instrumentation of arbitrary code, prefer a
  **DBI** client (DynamoRIO) started with the process to capture values at
  acceptable overhead.

## Integration with `asm-test`

- Emit captured events into a structured trace format (suggested prototype:
  `asmtest_valtrace_t`) compatible with `asmtest_trace_t` so analysis passes
  (def-use, taint) can be applied uniformly.
- Add a `tools/instrument/preload-logger.c` LD_PRELOAD prototype that logs a
  small default set (`read`, `write`, `send`, `recv`, `fread`, `fwrite`, and a
  user-specified function list) and a `tools/instrument/ptrace-attach.c`
  example for live attach tests.
- Add config knobs for maximum buffer read size, sampling frequency, and
  `ASMTEST_VALTRACE_DIR` output location.

## Safety & privacy

- Capturing pointed memory can expose secrets; require consent and lab
  isolation. Treat logs as sensitive artifacts and support encryption/signing.
- Avoid unbounded memory reads (caps and sampling required).

## Prototype plan (MVP)

1. Add `docs/internal/analysis/capture-args-returns.md` (this document).
2. Implement `tools/instrument/preload-logger.c` (LD_PRELOAD) that logs
   function entry/exit and small buffers into `build/traces/` as JSON lines.
3. Implement `tools/instrument/ptrace-attach.c` that demonstrates live attach
   capture for a short target run (uses `PTRACE_SINGLEBLOCK` where available).
4. Wire a small parser that converts these logs into a `asmtest_valtrace_t`
   placeholder so downstream analysis (L1 def-use) can exercise on CI.

## Next steps / open questions

- Which capture prototype do we want first: LD_PRELOAD (fastest) or DBI
  (most versatile)? The design above favors LD_PRELOAD for a quick win.  
- Define the `asmtest_valtrace_t` schema and storage semantics (truncate,
  sampling, and JSON vs binary).  
- Decide safe defaults for max-pointer read sizes and whether to include
  sensitive data redaction in the logger.

---

Authored by: repository analysis assistant
