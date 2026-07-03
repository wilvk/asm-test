# Features & support matrix

A single-page catalogue of everything asm-test does, followed by the matrices
that show **what is available where** — across architectures, operating systems,
execution tiers, emulator guests, and language bindings. Each feature links to
the guide that covers it in depth.

## Feature list

### Test framework & runner

- **Auto-discovered `TEST(suite, name)`** cases with a provided `main()` — no
  manual registration. ([Writing tests](writing-tests.md))
- **Fixtures**: per-suite `SETUP` / `TEARDOWN`, and `SKIP(reason)`.
- **Per-test isolation**: each test runs in a forked child so a crash or hang is
  contained; `--no-fork` opts into the in-process model. ([The runner](runner.md))
- **Timeouts**: per-test `alarm()` (`--timeout=SEC`) turns an infinite loop into
  a reported failure.
- **Parallelism**: `-jN` runs up to N tests concurrently; output stays in
  registration order.
- **Runner CLI**: `--filter=GLOB` (over `suite`, `name`, or `suite.name`),
  `--list`, `--shuffle` / `--seed`, `--timeout`, `--bench` / `--bench-reps`.
- **Reporting**: colored, TAP-style output by default; `--format=junit` emits
  JUnit XML for CI; nonzero exit on any failure. ([CI](ci.md))

### Assertions

- **Signed integer**: `ASSERT_TRUE/FALSE`, `ASSERT_EQ/NE/LT/LE/GT/GE`.
- **Unsigned**: `ASSERT_UEQ/UNE/ULT/ULE/UGT/UGE`.
- **Strings & memory**: `ASSERT_STREQ`, `ASSERT_MEM_EQ` (hexdump diff).
- **Floating-point** (ULP-aware): `ASSERT_FP_EQ`, `ASSERT_FP_NEAR`, plus lane
  forms `ASSERT_DEQ/DNEAR` and `ASSERT_FEQ/FNEAR`.
- **SIMD**: `ASSERT_VEC_EQ` (and `ASSERT_VEC256_EQ`).
- **Registers / flags / ABI**: `ASSERT_REG_EQ`, `ASSERT_FLAG_SET/CLEAR`
  (CF/PF/ZF/SF/OF), `ASSERT_ABI_PRESERVED`. ([Assertions](assertions.md))

### ABI capture & call models

Drive a routine through the **real calling convention** and snapshot the result.
([ABI capture](abi-capture.md), [Floating-point & SIMD](floating-point-simd.md))

- **Register/flags capture** via `ASM_CALLn(&regs, fn, …)` into a `regs_t`.
- **Arbitrary integer arity** via `ASM_CALLN` (stack-spills the overflow per ABI).
- **Struct return** via `ASM_SRET` (hidden result pointer for large structs).
- **Struct-by-value parameters** (small via eightbytes, large via
  `asm_call_capture_bigstruct`).
- **Floating-point** via `ASM_FCALLn` / `ASM_FCALLN` (captures `regs.fret`).
- **128-bit SIMD** via `ASM_VCALLn` / `ASM_VCALLN` (captures the vector file).
- **256-bit AVX2** via `ASM_VCALL256n` (x86-64; self-skips without AVX2).

### Differential / property testing

- `ASSERT_MATCHES_REF{1,2,3}(fn, ref, gen, n)` — fuzz `n` random inputs through
  the routine and a C reference model, report the first disagreement.
- Seedable splitmix64 RNG (`asmtest_rng_*`); fixed seed by default, overridable
  with `ASMTEST_SEED`. ([Property testing](property-testing.md))

### Robustness

- **Guard-page buffers** (`asmtest_guarded_alloc`) so a one-past-the-end access
  faults instead of corrupting silently.
- **Crash/hang containment** — a fatal signal (SIGSEGV/SIGBUS/…) or timeout in a
  buggy routine becomes a reported failure, and the run continues.

### Benchmarking

- `BENCH(suite, name)` — one measured call, auto-calibrated repeat count,
  min/median/mean **cycles per call** (`rdtsc` / `cntvct_el0`); `BENCH_USE(x)`
  defeats dead-code elimination. ([Benchmarks](benchmarks.md))

### Emulator tier (optional, Unicorn)

Run a routine in a virtual CPU to do what a real call can't. ([Emulator](emulator.md))

- **Five guests**: x86-64 System V, x86-64 Win64 ABI, AArch64, RISC-V RV64, ARM32
  — all run on any host.
- **Full register file** mid-routine (`max_insns` single-step), incl. a
  callee-saved register clobbered and restored.
- **Preload** arbitrary registers and memory (`emu_map` / `emu_write` / `emu_read`).
- **Precise faults** as data: `faulted`, `fault_addr`, `fault_kind` (READ/WRITE/FETCH).
- **FP & vector** marshalling/capture (`emu_call_fp` / `emu_call_vec`).
- **Execution trace & basic-block coverage** (`emu_call_traced`), with union
  across inputs, lcov export, and **source-line coverage** via a caller-supplied
  line map. ([Traces](tracing/traces.md))
- **Mid-execution guards**: memory-write watchpoints and block-entry register
  invariants (catch corruption even when restored before return).
- **Coverage-guided fuzzing** (`emu_fuzz_cover1`) and **mutation testing**
  (`emu_mutation_test1`), contained inside the emulator.
- **Fault/trace disassembly** (optional, Capstone): annotate offsets with the
  instruction text (`emu_disas`, `emu_fault_describe`, `…_disasm`). `emu_disas`
  is exposed through every language binding (`disas`/`disas_available`), via
  `libasmtest_emu` (the full superset lib carries it). ([Disassembly](disassembly.md))

### Native runtime trace tiers (optional, Linux)

Trace code as it runs **natively, in-process** — a third family of execution tiers
alongside native capture and the emulator. Every backend fills the same
`asmtest_trace_t` shape as the emulator trace and self-skips when its toolchain or
hardware is absent. All are exposed through **all ten language wrappers**
(`drtrace` / `hwtrace`) and held in sync by the binding function-parity gate.
([Native runtime tracing](tracing/native-tracing.md))

- **DynamoRIO native trace**: in-process attach (`dr_app_*`), begin/end region
  markers, basic-block + instruction coverage for native or Keystone-generated
  host-native code (W^X executable memory). BSD-only (raw DynamoRIO core API; no
  drwrap/LGPL). Linux x86-64.
- **Hardware trace** — four backends behind one API and one `available()` gate:
  **Intel PT** (`perf_event_open` + libipt) and **AMD LBR** (Zen 3 BRS / Zen 4–5
  LbrExtV2, 16-deep stack with Tier-B window stitching past it, live-verified on
  Zen 5) on bare metal; an **ARM CoreSight**
  (OpenCSD) scaffold; and **single-step** (`EFLAGS.TF` → `SIGTRAP`) — the universal
  backend recording the same exact offsets on **any x86-64 Linux** host (CI,
  containers, VMs) with no PMU / perf / privilege / decoder. Branch-boundary block
  normalization keeps the partition identical across backends.
- **Out-of-process single-step (W2, `ptrace`)**: a tracer parent single-steps a
  tracee, so it touches none of the target's signals or code cache — the path for
  **managed runtimes** (JVM/.NET/Node) and the only single-step form on **AArch64**.
  It comes with a foreign-JIT toolkit: attach to a live PID, `run_to` a method
  (software *or* hardware breakpoint), resolve regions from `/proc/maps`, perf-maps,
  or binary **jitdump**, and a time-aware **code-image recorder** (`asmtest_codeimage`)
  for correct bytes under re-JIT / address reuse. Validated against live V8 and CoreCLR.
- **Backend auto-selection**: `asmtest_hwtrace_auto` / `resolve` pick the host's
  most-faithful available backend, and `asmtest_trace_auto` extends the cascade across
  all three tiers (Intel PT → AMD LBR → DynamoRIO → single-step → CoreSight → emulator),
  with a `NATIVE_ONLY` policy that guards the native→emulator fidelity line.

### In-line assembler (optional, Keystone)

- Pass a routine as assembly **text** instead of a compiled address, then run it
  in the emulator or assemble to machine-code bytes (multi-arch). Available
  through every language binding. ([Bindings overview](bindings.md))

### Portability & assemblers

- One source set across **x86-64 / AArch64**, **Linux / macOS**, via the
  `ASM_FUNC` / `ASM_ENDFUNC` ELF-vs-Mach-O shim. ([Portability](portability.md))
- Two assembler backends: **GAS** (default, all targets) and **NASM**
  (Intel-syntax, x86-64 only).

### Language bindings

- Ten bindings (Python, .NET, Go, Rust, C++, Zig, Node, Java, Ruby, Lua), each a
  reusable module exposing capture + emulator + optional in-line assembler, plus
  Tier-2 assertions. ([Bindings](bindings.md))

---

## Matrix 1 — Architecture × operating system (native build)

The native tier (capture trampoline on the real CPU) supports:

| | Linux | macOS |
|---|---|---|
| **x86-64** | ✓ | ✓ |
| **AArch64** | ✓ | ✓ (Apple Silicon) |

CI runs every suite on all four combinations (`ubuntu-latest`,
`ubuntu-24.04-arm`, `macos-latest`, `macos-13`). The **NASM** backend adds
Intel-syntax sources on **x86-64 only** (both OSes); on AArch64, GAS is the only
backend. The emulator guests (Matrix 3) run on **any** of these hosts regardless
of guest ISA.

## Matrix 2 — Capability × execution tier

What each tier can observe. The native tier is the real CPU after `ret`; the
emulator is a virtual CPU you can stop mid-routine.

| Capability | Native capture | Emulator tier |
|---|---|---|
| Return value, post-return GP registers | ✓ | ✓ |
| Condition flags (CF/ZF/…) | ✓ | ✓ (x86/arm32; see Matrix 3) |
| Callee-saved (ABI) preservation check | ✓ (sentinels) | ✓ (+ mid-routine) |
| FP / SIMD return capture | ✓ | ✓ |
| **Mid-routine** register state | ✗ | ✓ |
| Precise memory fault (address + kind) | ✗ (guard page → signal) | ✓ |
| Preload arbitrary registers/memory | ✗ | ✓ |
| Branch / basic-block coverage | ✗ | ✓ |
| Mid-execution write/register guards | ✗ | ✓ (x86-64) |
| Coverage-guided fuzzing / mutation testing | property testing (fixed gen) | ✓ (x86-64) |
| Non-host architecture / Win64 ABI | ✗ | ✓ |
| Differential / property testing | ✓ | pairs with fuzzing |
| Benchmarks (cycles/call) | ✓ | ✗ |

## Matrix 3 — Emulator guests × capability

All five guests share the `*_open → *_map/_write → *_call → result` shape, the
fault hooks, and trace/coverage. They differ in the argument/return registers and
in which extra paths exist.

| Capability | x86-64 SysV | x86-64 Win64 | AArch64 | RISC-V RV64 | ARM32 |
|---|---|---|---|---|---|
| Integer call & GP register read | ✓ | ✓ | ✓ | ✓ | ✓ |
| Source of routine bytes | built fn | built fn | raw bytes | raw bytes | raw bytes |
| FP args / return | ✓ | — | ✓ | ✓ (scalar) | ✓ |
| Vector args / return | ✓ (XMM) | — | ✓ (NEON) | ✗¹ | ✓ (NEON) |
| Flags register | ✓ (rflags) | ✓ (rflags) | PSTATE | ✗² | ✓ (cpsr) |
| Execution trace & coverage | ✓ | ✓ | ✓ | ✓ | ✓ |
| Fault/trace disassembly (Capstone) | ✓ | ✓ | ✓ | ✓³ | ✓ |
| Mid-execution guards (watch / reg) | ✓ | ✓ | — | — | — |
| Coverage-guided fuzz / mutation | ✓ | — | — | — | — |

¹ Unicorn exposes no RVV registers, so RISC-V has no vector path.
² RISC-V has no flags register.
³ RISC-V disassembly needs **Capstone ≥ 5**; older builds self-skip that guest.

## Matrix 4 — Language bindings

All ten bindings are at **full capability parity** — integer/FP/vector capture
(incl. AVX2 256-bit), the x86-64 emulator plus all cross-arch guests
(arm64/riscv/arm) and Win64, execution trace/coverage, mid-execution guards,
coverage-guided fuzzing and mutation testing, the in-line assembler
**and disassembler** (both carried by the superset `libasmtest_emu`), and Tier-2
assertions. They differ in FFI mechanism, how the optional tiers are gated, and
packaging maturity.

| Language | FFI mechanism | Optional-tier gating | Published package |
|---|---|---|---|
| Python | `ctypes` | runtime probe (`asm_available()` / `disas_available()`) | ✓ (wheel / `pyproject.toml`) |
| .NET | P/Invoke | runtime probe (`AsmAvailable` / `DisasAvailable`) | not yet⁴ |
| Go | `cgo` | runtime probe | not yet⁴ |
| Rust | `extern` + build script | runtime probe | not yet⁴ |
| C++ | direct `#include` | **build-time** (`-DASMTEST_ENABLE_ASM` / `_DISAS`) | not yet⁴ |
| Zig | `@cImport` | on by default (build-time, no flag) | not yet⁴ |
| Node.js | `koffi` | runtime probe | not yet⁴ |
| Java | FFM (Panama) | runtime probe | not yet⁴ |
| Ruby | `Fiddle` | runtime probe | not yet⁴ |
| Lua | LuaJIT `ffi` | runtime probe | not yet⁴ |

⁴ Ships the same reusable module and assertions today (consumed via the built
shared libs, as `make <lang>-test` wires it); per-platform packaging is tracked
in [Packaging the bindings](packaging.md). Python is the reference/turnkey binding.

The Python binding reads struct layout from the `asmtest_abi.json` manifest; the
other nine go through opaque-handle accessors, so none mirror `regs_t`. Every
binding is kept honest by the shared **conformance corpus** (one set of canonical
routines + expected captures every language must reproduce).

## Matrix 5 — Optional dependencies

The base framework needs only a C compiler and `make`. Each tier below is
**optional and self-skipping** — absent its dependency, those entry points
degrade gracefully (the assembler/disassembler probe returns false; the helpers
fall back to bare offsets) and the core stays dependency-free.

| Tier | Dependency | Scope | Without it |
|---|---|---|---|
| Core framework + native capture | C compiler + make | all targets | — (always available) |
| NASM (Intel-syntax) backend | `nasm` | x86-64 only | use the GAS backend |
| Emulator tier | `libunicorn` | all guests | emulator suites skip |
| In-line assembler | `libkeystone` | all bindings | `asm_available()` → false, calls self-skip |
| Fault/trace disassembly | `libcapstone` (≥ 5 for RISC-V) | emulator diagnostics | helpers print bare offsets |

Install the optional native deps with `make deps DEPS_ARGS=--emu` (Unicorn +
Capstone) plus `libkeystone` for the in-line assembler; `make shared-emu` then
builds the superset `libasmtest_emu` with all three tiers.

## Where next

- [Examples](examples.md) — these features in action, by use case and audience.
- [Emulator tier](emulator.md) — the full guest API and diagnostics.
- [Execution traces and coverage](tracing/traces.md) — trace buffers, coverage unions,
  source-line maps, and lcov export.
- [Bindings](bindings.md) — the per-language capability map and setup.
- [Portability](portability.md) — the architecture/OS/assembler details.
