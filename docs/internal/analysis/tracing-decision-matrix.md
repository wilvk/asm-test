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
[auto-escalating trace](../archive/plans/auto-escalating-trace-plan.md),
[MSR-direct LBR](../archive/plans/amd-msr-direct-lbr-plan.md).*

## Summary

Backend choice is a product of **three independent axes**, resolved in order:

1. **Arch × OS decides what exists.** Intel PT is bare-metal-Intel-Linux only; AMD LBR is
   AuthenticAMD-Zen3+-Linux only; ARM CoreSight is AArch64-Linux-with-a-board only; in-process
   single-step is x86-64 Linux **and** macOS; a VEH stepper covers x86-64 Windows; ptrace
   block-/single-step is Linux (x86-64, aarch64 for per-insn); DynamoRIO is Linux-x86-64; the
   **Unicorn emulator is the universal floor** (and the *only* option for RISC-V / ARM32
   guests, which run virtually on any host). **Host provisioning then gates live capture** of
   the hardware-trace tiers — a generic VM/CI host records none of them, this repo's
   `docker-hwtrace-*` lanes each unlock one with minimal caps, and a full-permission bare-metal
   host (e.g. the Zen 5 dev box) records everything its silicon supports ([Matrix 1b](#matrix-1b--provisioning-generic-vs-docker-lane-vs-full-permission)).
2. **Language class decides what is SAFE.** The in-process EFLAGS.TF / `#DB` single-step tier
   is exact and cheap but **fatal on a managed-runtime thread** (a TF-armed thread that blocks
   `SIGTRAP` — as glibc `pthread_create`, CLR exception dispatch, and GC all do — is
   force-killed, exit 133). So native-leaf callers may use it freely; managed-JIT capture must
   not, and routes to lazy-arm-around-a-native-call, out-of-process ptrace, or hardware trace.
3. **Capture goal decides fast-vs-complete.** AMD's 16-entry LBR ceiling means its tiers are a
   *fast accelerator for small routines*, never a general tracer; loops and long routines need
   a **ceiling-free** tier (DynamoRIO / block-step / single-step). The
   [`asmtest_trace_call_auto`](../archive/plans/auto-escalating-trace-plan.md) orchestrator makes that
   call automatically (fast where the window fits, escalate to complete on `truncated`).

**Bottom line:** *native leaf on x86-64 Linux* → in-process single-step (or `call_auto` to
auto-escalate); *AMD Zen 5, small routine, want zero perturbation* → deterministic BPF
snapshot; *loop / long routine* → DynamoRIO if installed else block-step; *Intel bare metal* →
Intel PT; *managed JIT method* → lazy-arm (named) or out-of-process stealth (whole window);
*non-x86/non-ARM guest, or any host with no native tier* → the emulator.

---

## Matrix 1 — Tier availability by (arch, OS)

This first matrix is the **arch/OS/vendor** view — does the tier's silicon + code exist for this
host category at all. Availability *also* depends on **host provisioning** (privilege / perf /
device access), which [Matrix 1b](#matrix-1b--provisioning-generic-vs-docker-lane-vs-full-permission)
breaks out — because a ⚙️ tier that self-skips on a generic VM captures fine on the same box with
the right caps. Legend: ✅ live-capturable on a correctly-provisioned real host of this arch/OS (a
runtime functional probe may still self-skip under emulation / a quirked VM — noted inline) · ⚙️
**compiled and always linked, but live capture is gated on PROVISIONING** — it self-skips on a
*generic* host (VM / CI / plain `docker run`) and comes live at the **Docker-lane** or
**full-permission** level (Matrix 1b), because it needs specialized silicon (a specific vendor's
PMU, or a CoreSight board) plus perf/BPF/MSR privilege · ❌ not applicable (wrong arch/OS, hard
stub). Columns: **x64/L** x86-64 Linux · **x64/mac** x86-64 macOS · **x64/win** x86-64 Windows ·
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
| **ptrace per-insn single-step** | ✅ rootless | ❌ | ❌ | ✅ real host (self-skips under qemu-user) | ❌ | ❌ | exact, complete, ~1 trap/insn |
| **in-proc single-step** (EFLAGS.TF / `#DB`) | ✅ no privilege | ✅ (XNU #DB→SIGTRAP) | ❌ (use VEH) | ❌ | ❌ | ❌ | exact, complete, **unsafe on managed threads** |
| **Win64 VEH single-step** | ❌ | ❌ | ✅ (`AddVectoredExceptionHandler`) | ❌ | ❌ | ❌ | exact, complete |
| **Unicorn emulator** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ **only tier here** | exact, complete, **virtual guest** (not the real CPU) |
| **`asmtest_trace_call_auto`** (orchestrator) | ✅ (LBR→block-step→single-step) | ✅ (single-step) | — | ✅ (ptrace single-step) | — | — | picks the best *available* of the above |

### Matrix 1b — provisioning: generic vs docker-lane vs full-permission

The ⚙️ cells above vary with **host provisioning**, not just arch/OS. Three levels, for an
x86-64 Linux host *of the right vendor* (aarch64 CoreSight is the same story on an ARM board):

- **Generic** — a cloud VM, a hosted CI runner, or a **plain `docker run`** (default seccomp, no
  added caps): no bare-metal PMU passthrough, no perf/BPF/MSR privilege. Only the no-privilege
  tiers capture; every hardware-trace tier self-skips.
- **Docker lane** — this repo's `docker-hwtrace-*` targets on a bare-metal host, each granting the
  **minimal caps its tier needs** (never blanket `--privileged` except the MSR lane, which needs
  the device node): the specific AMD/Intel tier comes live per lane.
- **Full-permission bare-metal** — the host **directly** (e.g. the Ryzen 9 9950X dev box): root /
  all caps, `perf_event_paranoid` lowered, the `msr` module loaded. **Every tier the silicon
  supports captures live — nothing self-skips for the box's own (arch, vendor).**

| Tier (x86-64 Linux) | Generic (VM / CI / plain `docker run`) | Docker lane (bare-metal + minimal caps) | Full-permission bare-metal |
|---|---|---|---|
| **Intel PT** | ❌ no `intel_pt` PMU in the guest | ✅ on an **Intel** host + `--cap-add=PERFMON --security-opt seccomp=unconfined` | ✅ (Intel bare metal, `perf_event_paranoid` lowered) |
| **AMD LBR — sampled / WindowHot** | ❌ no branch-stack perf | ✅ **`docker-hwtrace-amd`** (`seccomp=unconfined --cap-add=PERFMON`) | ✅ (AuthenticAMD Zen 3+) |
| **AMD LBR — BPF snapshot** | ❌ no `CAP_BPF`, no libbpf | ✅ **`docker-hwtrace-codeimage`** (`--cap-add=BPF,PERFMON,SYS_PTRACE seccomp=unconfined`, libbpf image) | ✅ (Zen 4/5, `CAP_BPF`) |
| **AMD LBR — MSR-direct** | ❌ no `/dev/cpu/N/msr` | ✅ **`docker-hwtrace-msr`** (`--privileged`, for the per-CPU device nodes) | ✅ (`CAP_SYS_ADMIN` + the `msr` module) |
| **ARM CoreSight** (aarch64) | ❌ no `cs_etm` PMU in the guest | ✅ on an aarch64 **CoreSight board** + `--cap-add=PERFMON` | ✅ on the board |
| **DynamoRIO** | ✅ if `libasmtest_drapp` is installed | ✅ **`drtrace`** lane (no special caps) | ✅ |
| **BTF block-step** | ✅ rootless (self-skips only masked-`DEBUGCTL.BTF` VMs) | ✅ (plain `docker run`) | ✅ |
| **ptrace / in-proc single-step** | ✅ (no privilege) | ✅ | ✅ |
| **Unicorn emulator** | ✅ | ✅ | ✅ |

> **On the full-permission Zen 5 dev box (x86-64 Linux, AuthenticAMD, all caps, `msr` module
> loaded) NOTHING in its arch/vendor self-skips** — every AMD LBR tier (sampled, BPF snapshot,
> MSR-direct, WindowHot) captures live, exactly as validated this cycle. The only ❌ there is
> **Intel PT**, and that is a *silicon* absence (wrong vendor), not a provisioning one. The ⚙️
> marks in Matrix 1 are therefore the **generic-host** reading; read the Docker-lane or
> full-permission column for a properly-provisioned host.

### Why ⚙️ (compiled but self-skips), per item

**This is by design, and it is correct — not a packaging gap.** asm-test compiles *every*
backend into the binary unconditionally (each is its own TU behind `#if` host guards with a
self-skipping `#else` stub, so the symbol **always links** and the reconstruction/decode + the
gating chain are **always host-testable** — e.g. the AMD decoder is validated with synthetic
`perf_branch_entry[]` on any x86-64 Linux). What gates is **live capture**, at *runtime*, via
each backend's `available()` probe — because the house rule is **"no untested hardware code": a
tier that cannot self-validate on its target silicon returns `available() → 0` rather than
faulting or emitting an unproven trace.** So a ⚙️ cell means "the code is here and the decode is
proven, but *this generic host* lacks the specific silicon/privilege to record a live trace."
Distinguish it from a ✅ cell that carries an *inline* self-skip caveat (block-step, aarch64
ptrace): those run live on a real host and self-skip only under emulation / a quirked VM.

The reason for **each ⚙️** (source of record: `asmtest_hwtrace_available` / `_skip_reason`,
[src/hwtrace.c](../../../src/hwtrace.c)):

| ⚙️ item | Runs live only when… | Self-skips because (on a generic host) |
|---|---|---|
| **Intel PT** (x64/L) | `GenuineIntel` **and** an `intel_pt` sysfs PMU node exists **and** a disabled `perf_event_open` is permitted (bare metal, `perf_event_paranoid` lowered) | absent on **AMD**, in **VMs / cloud CI** (no `intel_pt` PMU exposed to the guest), and without perf privilege |
| **AMD LBR — sampled** (x64/L) | `AuthenticAMD` Zen 3+ **and** a branch-stack `perf_event_open` succeeds (`CAP_PERFMON` / paranoid lowered) | absent on **Intel**, on **Zen 2** (no branch facility → `EOPNOTSUPP`), in **VMs**, and without `CAP_PERFMON` |
| **AMD LBR — BPF snapshot** (x64/L) | LbrExtV2 + `perfmon_v2` + kernel ≥ 6.10 **and** `CAP_BPF`+`CAP_PERFMON` at load **and** the image was built with libbpf | non-Zen4/5, old kernel, missing caps, or a build **without the BPF toolchain** (the `#else` stub self-skips) — then the marker path falls back to the sampled tier |
| **AMD LBR — MSR-direct** (x64/L) | `amd_lbr_v2` **and** `/dev/cpu/N/msr` opens `O_RDWR` (`CAP_SYS_ADMIN` + the `msr` module) | no `amd_lbr_v2`, `msr` module not loaded, or no `CAP_SYS_ADMIN` — the higher-privilege niche vs the BPF snapshot |
| **AMD LBR — WindowHot survey** (x64/L) | same branch-stack substrate as sampled (`CAP_PERFMON`, Zen 3+) | same as sampled — no AMD branch facility / no perf privilege |
| **ARM CoreSight** (a64/L) | a `cs_etm` sysfs PMU node exists (a **CoreSight-capable board**) + perf permitted + OpenCSD linked | most aarch64 hosts (phones, cloud servers, Apple Silicon) **have no CoreSight PMU exposed**; VMs don't expose it |

So every ⚙️ is a **bare-metal, vendor-/board-specific hardware-trace tier** that needs its PMU +
perf/BPF/MSR privilege — exactly the tiers a generic desktop/VM/CI host cannot record on. This is
also why the hosted `hwtrace` CI lane validates **decode only** (capture self-skips on cloud
VMs), and the AMD lanes (`docker-hwtrace-amd`, `-codeimage`, `-msr`) run live only on the
self-hosted Ryzen 9 9950X dev box. The two ✅-with-caveat runtime probes are the exceptions that
prove the rule: **BTF block-step** self-skips only on a VM whose hypervisor masks the
`DEBUGCTL.BTF` bit ([`probe_singleblock`](../../../src/ptrace_backend.c)), and **aarch64 ptrace
single-step** self-skips only under **qemu-user** (which doesn't emulate the ptrace tracer/tracee
relationship) — both work live on a real host of their arch, needing no special silicon.

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
whole-window trio but their live-managed capture rides a native leaf (Class-2 posture) — and
their crash-proof out-of-process whole-window family shipped on 2026-07-09
(`stealthWindow`/`stealthWindowed` → `asmtest_hwtrace_stealth_trace_windowed`, commit
`272bd65`; exercised in [bindings/node/test_hwtrace.js](../../../bindings/node/test_hwtrace.js)
~:766 and [bindings/java/HwTraceTest.java](../../../bindings/java/HwTraceTest.java)
~:959-985). *(Corrected 2026-07-21 — this note previously ended "with the crash-proof OOP
family still forward-look", which was stale.)*

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
  [scoped-tracing-managed-plan.md](../archive/plans/scoped-tracing-managed-plan.md) and
  [managed-singlestep-posture-plan.md](../archive/plans/managed-singlestep-posture-plan.md).
