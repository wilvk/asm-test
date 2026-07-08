# Analysis: tracing backend decision matrix

*Status: analysis / findings. A decision aid for choosing among asm-test's trace backends
given a **host** (arch × OS), a **language binding**, and a **capture goal**. Derived from
the source of record — [src/hwtrace.c](../../../src/hwtrace.c)'s gating chain,
[include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h), the out-of-process ptrace
tier ([include/asmtest_ptrace.h](../../../include/asmtest_ptrace.h),
[src/ptrace_backend.c](../../../src/ptrace_backend.c)), the AMD MSR-direct tier
([src/msr_lbr.c](../../../src/msr_lbr.c)), the cross-tier orchestrator
([src/trace_auto.c](../../../src/trace_auto.c)), and [mk/native-trace.mk](../../../mk/native-trace.mk).
Narrative guides: [tracing index](../../guides/tracing/index.md),
[native runtime tracing](../../guides/tracing/native-tracing.md),
[hardware tracing](../../guides/tracing/hardware-tracing.md),
[scoped tracing](../../guides/tracing/scoped-tracing.md). Companion analysis:
[trace parity matrix](trace-parity-matrix.md) (what works where today),
[data-flow capture](data-flow-capture.md). Roadmaps:
[hardware-trace plan](../plans/hardware-trace-plan.md), [AMD LBR plan](../plans/amd-tracing-plan.md),
[auto-escalating trace](../plans/auto-escalating-trace-plan.md),
[MSR-direct LBR](../plans/amd-msr-direct-lbr-plan.md).*

## Summary

Backend choice is a product of **three independent axes**, resolved in order:

1. **Arch × OS decides what exists.** Intel PT is bare-metal-Intel-Linux only; AMD LBR is
   AuthenticAMD-Zen3+-Linux only; ARM CoreSight is AArch64-Linux-with-a-board only; in-process
   single-step is x86-64 Linux **and** macOS; a VEH stepper covers x86-64 Windows; ptrace
   block-/single-step is Linux (x86-64, aarch64 for per-insn); DynamoRIO is Linux-x86-64; the
   **Unicorn emulator is the universal floor** (and the *only* option for RISC-V / ARM32
   guests, which run virtually on any host).
2. **Language class decides what is SAFE.** The in-process EFLAGS.TF / `#DB` single-step tier
   is exact and cheap but **fatal on a managed-runtime thread** (a TF-armed thread that blocks
   `SIGTRAP` — as glibc `pthread_create`, CLR exception dispatch, and GC all do — is
   force-killed, exit 133). So native-leaf callers may use it freely; managed-JIT capture must
   not, and routes to lazy-arm-around-a-native-call, out-of-process ptrace, or hardware trace.
3. **Capture goal decides fast-vs-complete.** AMD's 16-entry LBR ceiling means its tiers are a
   *fast accelerator for small routines*, never a general tracer; loops and long routines need
   a **ceiling-free** tier (DynamoRIO / block-step / single-step). The
   [`asmtest_trace_call_auto`](../plans/auto-escalating-trace-plan.md) orchestrator makes that
   call automatically (fast where the window fits, escalate to complete on `truncated`).

**Bottom line:** *native leaf on x86-64 Linux* → in-process single-step (or `call_auto` to
auto-escalate); *AMD Zen 5, small routine, want zero perturbation* → deterministic BPF
snapshot; *loop / long routine* → DynamoRIO if installed else block-step; *Intel bare metal* →
Intel PT; *managed JIT method* → lazy-arm (named) or out-of-process stealth (whole window);
*non-x86/non-ARM guest, or any host with no native tier* → the emulator.

---

## Matrix 1 — Tier availability by (arch, OS)

Legend: ✅ live-capturable · ⚙️ compiled but self-skips here (needs bare-metal PMU / caps /
perf, or the host isn't the right vendor) · ❌ not applicable (wrong arch/OS, hard stub).
Columns: **x64/L** x86-64 Linux · **x64/mac** x86-64 macOS · **x64/win** x86-64 Windows ·
**a64/L** aarch64 Linux · **a64/mac** aarch64 macOS · **rv/arm** riscv64 / arm32 (guests only).

| Tier | x64/L | x64/mac | x64/win | a64/L | a64/mac | rv/arm | Fidelity |
|---|---|---|---|---|---|---|---|
| **Intel PT** | ⚙️ Intel + `intel_pt` PMU + perf | ❌ | ❌ | ❌ | ❌ | ❌ | exact, complete, real CPU |
| **AMD LBR — sampled + Tier-B stitch** | ⚙️ AMD Zen3+ + `CAP_PERFMON` | ❌ | ❌ | ❌ | ❌ | ❌ | exact, **16-branch ceiling** |
| **AMD LBR — BPF boundary snapshot** (#3) | ⚙️ AMD Zen4/5 + `CAP_BPF` + kernel ≥6.10 | ❌ | ❌ | ❌ | ❌ | ❌ | exact, tiny-routine, near-zero overhead |
| **AMD LBR — MSR-direct** ([msr_lbr.c](../../../src/msr_lbr.c)) | ⚙️ AMD `amd_lbr_v2` + `CAP_SYS_ADMIN` + `msr` module | ❌ | ❌ | ❌ | ❌ | ❌ | exact, tiny-routine, **zero interrupts** |
| **AMD LBR — WindowHot survey** | ⚙️ AMD Zen3+ + `CAP_PERFMON` | ❌ | ❌ | ❌ | ❌ | ❌ | **statistical** (hot-method histogram, not a path) |
| **ARM CoreSight** | ❌ | ❌ | ❌ | ⚙️ `cs_etm` PMU (a board) | ❌ | ❌ | exact, complete, real CPU |
| **DynamoRIO** (in-proc DBI) | ✅ (dlopen `libasmtest_drapp`) | ❌ | ❌ | ❌ | ❌ | ❌ | exact, complete, native-speed cache |
| **BTF block-step** (`PTRACE_SINGLEBLOCK`) | ✅ rootless (self-skips masked-BTF VMs) | ❌ | ❌ | ❌ (no aarch64 form → falls to per-insn) | ❌ | ❌ | exact, complete, ~1 trap/branch |
| **ptrace per-insn single-step** | ✅ rootless | ❌ | ❌ | ⚙️ (self-skips under qemu-user) | ❌ | ❌ | exact, complete, ~1 trap/insn |
| **in-proc single-step** (EFLAGS.TF / `#DB`) | ✅ no privilege | ✅ (XNU #DB→SIGTRAP) | ❌ (use VEH) | ❌ | ❌ | ❌ | exact, complete, **unsafe on managed threads** |
| **Win64 VEH single-step** | ❌ | ❌ | ✅ (`AddVectoredExceptionHandler`) | ❌ | ❌ | ❌ | exact, complete |
| **Unicorn emulator** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ **only tier here** | exact, complete, **virtual guest** (not the real CPU) |
| **`asmtest_trace_call_auto`** (orchestrator) | ✅ (LBR→block-step→single-step) | ✅ (single-step) | — | ✅ (ptrace single-step) | — | — | picks the best *available* of the above |

> The ⚙️ hardware rows are why the hosted `hwtrace` CI lane validates **decode only** — live
> capture self-skips on cloud VMs (no bare-metal PMU / no `perf_event_paranoid` lowering / no
> AMD-or-Intel-specific silicon). The AMD lanes (`docker-hwtrace-amd`, `-codeimage`, `-msr`)
> run live only on the self-hosted Ryzen 9 9950X dev box.

---

## Matrix 2 — Language class → what is SAFE / preferred

The traced artifact is always **native machine code**; the language differs in *what thread
arms the capture* and *whether that thread is a managed-runtime thread that will block
`SIGTRAP`*. Three classes:

| Class | Bindings | In-proc single-step (T1)? | Preferred tier |
|---|---|---|---|
| **1. Native-compiled leaf** | C, C++, Rust, Zig, **Go** | ✅ safe — the armer runs pure native code | **T1 in-proc single-step** (`begin/end` or `call_scoped_ex`) |
| **2. Scripting-FFI (native leaf)** | Python, Ruby, Lua, Node | ✅ safe — GIL/single-thread armer, leaf doesn't re-enter the interpreter | **T1 in-proc single-step** over the native leaf |
| **3. Managed-JIT whole-window** | .NET, Java, Node/**V8** | ❌ **FORBIDDEN — force-kills the thread** | **B** lazy-arm (named method) · **T2** OOP stealth (whole window) · **T3** PT/LBR-survey |

**Class-1 caveat — Go:** still native, but `EFLAGS.TF` is per-OS-thread while the Go scheduler
multiplexes goroutines across `M`s; the armed window must `runtime.LockOSThread` (the resolved
`go-fulltest-flaky-crash` was a *missing* LockOSThread, the same per-thread-TF root cause as
the managed case — not a C bug).

**Class-2 caveat — Node's split personality:** as an FFI caller of a native leaf it is Class 2
(safe T1); when it targets **V8-JIT'd JavaScript** it becomes Class 3 (T1 forbidden against the
V8 runtime).

**Class-3 constraint (the why):** a TF-armed thread that masks `SIGTRAP` gets the next
instruction's `#DB` delivered as a masked *synchronous* signal, which the kernel force-resets
to `SIG_DFL` → **exit 133, no handler**. glibc `pthread_create` (CLR's tiering worker respawns
through it), CLR two-pass exception dispatch, and GC all do this on the armed thread. So managed
capture uses one of: **(B)** arm TF *only* around a reverse-P/Invoke `call` of the raw
fn-pointer in native code (`call_scoped` — the runtime is never stepped, sound by construction);
**(C)** out-of-process ptrace stealth (`stealth_trace[_windowed]` — the `#DB` goes to the
*tracer*, immune to the tracee's signal mask); or **hardware trace** (Intel PT clean; AMD
routes to the OOP stepper for exact, or the LBR WindowHot survey for a cheap statistical
histogram). Today only **.NET** wraps the full Class-3 machinery; Java/Node have the in-process
whole-window trio but their live-managed capture rides a native leaf (Class-2 posture) with the
crash-proof OOP family still forward-look.

---

## Matrix 3 — The decision: preferred backend per (goal × language × host)

Read as: pick your **row** (goal + language class), then your **host column**.

### 3a. Exact trace of a native leaf (Class 1 & 2 — C/C++/Rust/Zig/Go/Python/Ruby/Lua/Node-FFI)

| Goal | x86-64 Linux | x86-64 macOS | x86-64 Windows | aarch64 Linux | aarch64 macOS | Why |
|---|---|---|---|---|---|---|
| **Small routine, minimal perturbation** | AMD: BPF snapshot (#3) / MSR-direct; Intel: PT | in-proc single-step | VEH single-step | in-proc? no → ptrace single-step | emulator | LBR snapshots are near-zero overhead but tiny-only; PT is unbounded |
| **Loop / long / branchy** | **DynamoRIO** (if installed) → **BTF block-step** | in-proc single-step | VEH single-step | ptrace **per-insn** single-step (no block-step form on aarch64) | emulator | ceiling-free; DR native-speed, block-step rootless |
| **Don't want to choose** | **`asmtest_trace_call_auto`** (LBR→block-step→single-step) | in-proc single-step | VEH single-step | ptrace single-step | emulator | auto-escalates on `truncated` |
| **Zero setup, any host, guest ISA (RISC-V/ARM32)** | emulator | emulator | emulator | emulator | emulator | virtual guest — universal floor |

### 3b. Managed-JIT capture (Class 3 — .NET, Java, Node/V8)

| Goal | Intel host | AMD host | Notes |
|---|---|---|---|
| **One named JIT'd method, exact** | PT, or **B lazy-arm** (`AsmTrace.Method`) | **B lazy-arm** (in-proc, exact); **C OOP stealth** for signatures B can't shim | B steps only the reverse-P/Invoke `call`, never the runtime |
| **Whole window of managed code, exact** | **Intel PT** (clean) | **C OOP stealth windowed** (`stealth_trace_windowed`) — crash-proof | in-process whole-window is opt-in/best-effort only (arbitrary code runs in-window) |
| **Whole window, cheap / hot-spots** | PT | **AMD-LBR WindowHot survey** (statistical histogram) | not an exact path — profiling shape |
| **Any host, isolated** | emulator | emulator | virtual — loses the real JIT/GC behavior |

> The **in-process EFLAGS.TF single-step tier never appears in 3b** — it is the one tier that is
> categorically wrong for a managed thread.

---

## Ranked tradeoffs (the tiers, best→worst for an exact complete real-CPU trace)

| Rank | Tier | Exact | Complete | Overhead | Needs |
|---|---|---|---|---|---|
| 1 | **DynamoRIO** | ✓ | ✓ no ceiling | Low (native cache) | DR library (Linux x86-64) |
| 2 | **BTF block-step** | ✓ | ✓ no ceiling | Medium (~1 trap/branch) | rootless; Linux x86-64 |
| 3 | **Intel PT** | ✓ | ✓ no ceiling | Very low | bare-metal Intel + perf |
| 4 | **AMD LBR BPF snapshot** (#3) | ✓ | ✓ **≤16 branches** | Near-zero | CAP_BPF, Zen 4/5 |
| 5 | **AMD LBR MSR-direct** | ✓ | ✓ **tiny only** | Zero interrupts | CAP_SYS_ADMIN + `msr` |
| 6 | **AMD LBR sampled + stitch** | ✓ | branch-heavy that fits ring/throttle | High (PMI flood) | CAP_PERFMON, Zen 3+ |
| 7 | **in-proc single-step** | ✓ | ✓ | Medium (1 trap/insn) | no privilege — **but unsafe on managed threads** |
| 8 | **ptrace per-insn single-step** | ✓ | ✓ | High (1 trap/insn + ptrace) | rootless; Linux |
| 9 | **AMD LBR WindowHot survey** | ✗ statistical | histogram | Low | CAP_PERFMON — *profiling, not tracing* |
| 10 | **Unicorn emulator** | ✓ | ✓ | N/A | none — but **virtual guest**, not the real CPU |

*(ARM CoreSight ≈ Intel PT where a board exists; Win64 VEH ≈ in-proc single-step on Windows. The
AMD levers measured this cycle: #2B reduced filter buys **1.86×** per window; #2A period-spacing
gives no gain on loops.)*

## How the code resolves this for you

- `asmtest_trace_resolve` / `asmtest_trace_auto` ([trace_auto.c](../../../src/trace_auto.c))
  return the host's descending-fidelity cascade (Intel PT → AMD LBR → DynamoRIO → single-step →
  CoreSight → emulator), honoring `BEST` / `CEILING_FREE` / `NATIVE_ONLY` policy.
- `asmtest_trace_call_auto` **runs** it: fastest exact tier, escalate to a ceiling-free tier on
  `truncated`. This is the one-call answer for **3a "don't want to choose"** and is validated
  live escalating LBR→block-step on the Zen 5 box.
- The managed bindings pick their Class-3 posture per
  [scoped-tracing-managed-plan.md](../plans/scoped-tracing-managed-plan.md) and
  [managed-singlestep-posture-plan.md](../plans/managed-singlestep-posture-plan.md).
