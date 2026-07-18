# asm-test — single-step (Trap Flag) native-trace backend: implementation plan

A phased roadmap for a **debug-exception** native-trace backend that delivers
**exact, complete** `asmtest_trace_t` offsets on hardware that has *no* branch-trace
facility at all — most importantly **AMD Zen 2** (Family 17h), the project's primary
dev host. Where the [Intel PT](hardware-trace-plan.md) and
[AMD LBR](amd-tracing-plan.md) backends reconstruct the instruction stream from a
hardware *trace* (a PT AUX ring, or a 16-deep branch stack), this backend
reconstructs it from the x86 **single-step debug exception** (`#DB`) driven by the
`EFLAGS.TF` trap flag — a baseline ISA feature present on *every* x86 CPU since
AMD64 launched, requiring no PMU, no `perf_event`, no privilege, and no decoder
library. The output is identical to the other backends: instruction offsets
matching the Unicorn emulator, the DynamoRIO native tier, and Intel PT, and block
offsets that match after the same branch-edge normalization.

This plan is a **sibling** of the [hardware-trace plan](hardware-trace-plan.md)
(Intel PT / ARM CoreSight), the [AMD LBR plan](amd-tracing-plan.md) (Zen 3 BRS /
Zen 4 LbrExtV2), and the [DynamoRIO native-trace plan](../archive/plans/dynamorio-native-trace-plan.md).
It exists because of a hardware fact established while validating the AMD LBR
backend: **Zen 2 has no branch-trace facility of any kind** — no Intel-PT-style ring
(AMD has none on any Zen part), no BRS (Zen 3), no LbrExtV2 (Zen 4); its legacy LBR
is depth-1 (a single `from→to` pair, not wired to perf branch-stack, so a
branch-stack open returns `EOPNOTSUPP`). The one out-of-band HW facility that *does*
run on Zen 2, IBS, is purely statistical. So the only route to an **exact** native
trace on a Zen 2 box (short of the DynamoRIO software-DBI tier) is to step the CPU
one instruction at a time and record each `RIP`. This plan specifies that backend,
its single hard limit (overhead, not completeness), and its portability story.

> Status legend: **planned** unless noted. Update this file as phases land, the way
> [hardware-trace-plan.md](hardware-trace-plan.md) and
> [amd-tracing-plan.md](amd-tracing-plan.md) track theirs.

---

## The governing constraint

A trace facility (PT, BRS, LbrExtV2) records control flow into a buffer at
near-zero overhead; the decoder reconstructs the path afterward. The trap flag gets
the **same exact path** by a different means: with `EFLAGS.TF` set, the CPU raises a
trap-class `#DB` *after every instruction*, which the OS delivers as `SIGTRAP`; the
handler reads `RIP` and records it. So the trade is **not** "exact vs. nothing" — it
is **exact-cheap (trace ring) vs. exact-slow (a fault per instruction)**:

- **Completeness.** Every retired instruction is observed; there is **no depth
  ceiling** (unlike AMD LBR Tier-A's 16 taken branches) and no AUX-ring overflow.
  Loops of any length reconstruct exactly. This is the property Zen 2 cannot get
  from hardware trace.
- **Overhead.** One kernel round-trip per instruction — roughly **2.3 µs/instruction
  on Linux** (≈435k insn/s; Whitham's measurement), ~6× that on Windows. For
  asm-test's registered routines — small, branch-light, deterministic compute
  kernels of a handful to a few hundred instructions, exactly the Tier-A common case
  — total capture cost is microseconds to low milliseconds, i.e. irrelevant.

From this follows the backend's **operating envelope**: single-step is the exact,
zero-dependency, unprivileged backend for **small registered routines** on any x86
host (Intel, pre-Zen3 AMD, VM, CI), and the *only* exact in-process option on Zen 2.
It is **not** the right tool for tracing whole programs or hot long-running loops —
there the DynamoRIO tier (native-speed code cache) wins. The two are complementary:
single-step has no install/dependency and no ceiling; DynamoRIO has no per-step tax.

---

## Implementation status

> **Correction, amended 2026-07-12 (there are TWO dev hosts).** This plan was written
> assuming the project's primary dev host is an AMD **Zen 2** box with no branch
> facility — making single-step "the only exact native option here." An earlier
> revision of this note then over-corrected to "the dev host is a Zen 5, not Zen 2."
> Both hosts exist and both run the suites (see the two AMD records under
> [benchmarks/boxes/](../../../benchmarks/boxes/)): a **Ryzen 9 9950X (Family 0x1A,
> Zen 5, `amd_lbr_v2`)**, on which **AMD LBR is available and live-verified** (see the
> [AMD LBR plan](amd-tracing-plan.md) and
> [trace-parity-matrix](../analysis/trace-parity-matrix.md)), and a **Ryzen 9 4900HS
> (Family 0x17, Zen 2)** with no branch stack at all, on which the premise holds
> literally and the [IBS statistical lane](../archive/plans/zen2-ibs-tracing-plan.md) was built and
> validated. The single-step backend's value is **unchanged** — it is the portable,
> no-PMU/perf/privilege, depth-unbounded backend that runs on *any* x86-64 Linux host
> (including the true Zen 2, VMs, plain containers, and standard CI) and is the
> deterministic in-process path for tiny single-shot routines that AMD LBR's sampling
> cannot snapshot. "This Zen 2 host" phrasing below is literal on the 4900HS and reads
> as "this x86-64 Linux dev host" on the 9950X: single-step runs live on both.

**Phases 0–4 implemented (Linux/x86-64).** The `ASMTEST_HWTRACE_SINGLESTEP` backend
ships: gating ([src/hwtrace.c](../../../src/hwtrace.c)), the stepper + block
normalization ([src/ss_backend.c](../../../src/ss_backend.c)), the begin/end dispatch
arm, the Makefile wiring (`ss_backend.o` in `HWTRACE_OBJS` + the PIC/`shared-hwtrace`
variant), a **live** cross-backend parity test in
[examples/test_hwtrace.c](../../../examples/test_hwtrace.c) (`make hwtrace-test`, plus a
20-trip loop proving no depth ceiling), an `hwtrace` language wrapper + live test for
**all ten bindings** (`make hwtrace-bindings-test`), and **plain-container** Docker
lanes (`make docker-hwtrace` / `docker-hwtrace-bindings` — no privilege, no
`CAP_PERFMON`, no seccomp change). The existing hardware-trace backends (Intel PT /
CoreSight / AMD LBR) are unaffected — this is a fourth, perf-free backend behind the
same `asmtest_hwtrace_*` API, selected by enum exactly as PT and AMD LBR are.

**Phase 5, W2 (out-of-process `ptrace`) now ships for Linux x86-64**
([asmtest_ptrace.h](../../../include/asmtest_ptrace.h), `src/ptrace_backend.c`). Two
entry points: `asmtest_ptrace_trace_call` forks its own tracee and traces a code blob;
`asmtest_ptrace_trace_attached(pid, base, len, &result, trace)` traces a region in a
**separate, already-running process attached to from the outside** — the
foreign-process primitive a managed-runtime tracer builds on. It single-steps the
target from its current ptrace-stop (the caller owns `PTRACE_ATTACH`/`DETACH`) and
reads the region bytes from the target via `process_vm_readv` (no shared mapping).
Both reconstruct the *same* exact offsets out of band, verified byte-for-byte against
the in-process stepper — `trace_call` including a 62-instruction loop, `trace_attached`
by attaching to a child that never called `PTRACE_TRACEME` — live in a plain
unprivileged container by `make hwtrace-test`. The **region resolvers**
`asmtest_proc_region_by_addr` (`/proc/<pid>/maps`) and `asmtest_proc_perfmap_symbol`
(`/tmp/perf-<pid>.map`, the JIT text format V8/Node/.NET/OpenJDK emit) discover the
`(base, len)` to hand `trace_attached`, completing the managed-runtime flow (resolve →
attach → trace → detach) end to end in the live test. The **macOS-Intel front-end has
since landed**: the identical in-process EFLAGS.TF stepper drives it, because XNU
delivers the resulting `#DB` as a BSD `SIGTRAP` (an `EXC_BREAKPOINT` that falls through
to the signal path when no Mach exception port claims it), so re-asserting `TF` in the
saved thread state re-arms stepping across `sigreturn` exactly as on Linux. The only
platform deltas are the feature-test macro (`_DARWIN_C_SOURCE`) and the mcontext field
access (`uc_mcontext->__ss.__rip`/`__rflags` vs. Linux's `gregs[REG_RIP]`/`[REG_EFL]`),
isolated behind two shims in [src/ss_backend.c](../../../src/ss_backend.c); the
[src/hwtrace.c](../../../src/hwtrace.c) facade runs the full single-step lifecycle on
x86-64 Darwin, validated live by `make hwtrace-test` there (61 pass) with the Linux
tier unchanged (`make docker-hwtrace`, 178 pass). The **Windows x86-64 VEH
front-end has since landed** in the win64 native tier
([src/ss_win64.c](../../../src/ss_win64.c), `asmtest_win64_ss_trace_call`): the same
EFLAGS.TF mechanism, delivered as `EXCEPTION_SINGLE_STEP` to a Vectored Exception
Handler that records the in-region offsets and re-arms TF in the resumed CONTEXT —
with stepping ENDED BY THE HANDLER at the call's return landing rather than by a
popfq disarm, because NtContinue does not reproduce the popf trap-suppression the
POSIX steppers rely on. Its fixtures are the Win64-ABI twins of the Linux suite's
ROUTINE/LOOP blobs, so the identical expected streams ([0x0,0x3,0x6,0xc,0x11]; 62
steps across the loop back-edge) prove front-end parity — validated under Wine
(`make win64-ss-test`, in `win64-check`) and on real Windows in CI.

**The AArch64 ptrace tracer and the binary jitdump reader have both since landed**
(amended 2026-07-16; an earlier revision of this paragraph filed both as forward-look and
the Phase-5 detail below has recorded them as *Done* since). The **AArch64** tracer ships
in [src/ptrace_backend.c](../../../src/ptrace_backend.c) — `MDSCR_EL1.SS` is kernel-only,
so out-of-process ptrace is its *only* single-step form; it rides the same
`PTRACE_SINGLESTEP` seam as x86-64, reading the PC + return register via
`PTRACE_GETREGSET`/`NT_PRSTATUS` (AArch64 has no `PTRACE_GETREGS`) and decoding block
lengths with `ASMTEST_ARCH_ARM64` Capstone. The **binary jitdump** reader ships as
`asmtest_jitdump_find` ([asmtest_ptrace.h](../../../include/asmtest_ptrace.h)) and is
validated against all three real runtimes (V8, HotSpot, CoreCLR) — richer than the text
perf-map, which remains the portable lowest common denominator. See the Phase 5 detail
below for both.

**The one genuinely remaining Phase-5 front is the AArch64 live single-step *stream*** —
the tracer is written and its code fixtures are decode- and execute-validated under qemu,
but qemu-user does not emulate the ptrace tracer/tracee relationship at all, so the
capture itself **cannot be validated without real AArch64 silicon** (Apple Silicon /
Linux-ARM64 / Windows-on-ARM — no dev box here provides it). `asmtest_ptrace_available()`
is a cached, hang-proof self-probe that returns 0 under emulation, so the stepper
self-skips there exactly as the PT/CoreSight tiers self-skip off their hardware. The ten
binding wrappers gate on the same `available()`, so their live AArch64 fixtures are a
small follow-on alongside that hardware. The **in-process BTF** variant (W3) has since
landed as a raw-MSR pinned-envelope tier — the kernel-helper blocker this paragraph
once cited was not load-bearing after all; see W3 below.

---

## Phase 0 — Backend gating & feature detection *(shipped; macOS-Intel added)*

**Goal.** Make `asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)` return 1 on
any x86-64 Linux **or macOS** host (no privilege, no PMU, no decoder library required)
and self-skip elsewhere with a specific reason. *(Shipped: the gate is
`HWTRACE_HAVE_SINGLESTEP` = `__x86_64__ && (__linux__ || __APPLE__)`; the skip reason
off-platform reads "single-step backend is x86-64 Linux/macOS only (Windows/AArch64
planned)".)*

**Work.**

- Add `ASMTEST_HWTRACE_SINGLESTEP` to `asmtest_trace_backend_t` in
  [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h).
- In [src/hwtrace.c](../../../src/hwtrace.c)'s gating chain, give the new backend its
  own arm that **bypasses** `pmu_type()`/`perf_permitted()`/`decoder_present()`
  (none apply): `cpu_matches` requires `__x86_64__`; availability is otherwise
  unconditional on `__linux__`. Block-coverage mode additionally needs the Capstone
  length-decoder (`asmtest_disas_available()`, already linked into the tier via
  `disasm.o`); instruction-only mode does not.
- Extend `asmtest_hwtrace_skip_reason()` with strings: "single-step backend is Linux
  x86-64 only (Windows/macOS planned)" off Linux/x86-64, and "built without Capstone
  (single-step block normalization)" when only instruction mode is available.

**Acceptance.** `make hwtrace-test` reports the single-step backend **available** on
this Zen 2 host (where PT and AMD LBR both self-skip), and self-skips with the
specific reason on non-x86-64 / non-Linux builds.

---

## Phase 1 — The stepper (`src/ss_backend.c`) *(LANDED)*

**Goal.** Drive the registered routine under `TF` and capture the ordered in-region
`RIP` stream into the active region's trace, in-process, on the calling thread.

**Mechanism.**

- `begin(name)` installs a `SIGTRAP` `sigaction` (saving the prior disposition),
  marks the named region active, then sets `EFLAGS.TF` for the calling thread
  (`pushfq; or $0x100,(%rsp); popfq` — `TF` is bit 8 and is freely settable at CPL3
  in long mode; unlike `IF` it is not IOPL-gated). The instruction *following* the
  `popf` is the first to trap.
- On each `#DB`, the kernel delivers `SIGTRAP`; the handler reads `RIP` from
  `uc_mcontext.gregs[REG_RIP]`. `TF` is a trap-class flag: the saved `RIP` is the
  **instruction about to execute** (the prior one just retired), so recording the
  current `RIP` records each executed instruction exactly once.
- The handler **re-asserts `TF`** in the saved `uc_mcontext` eflags so `sigreturn`
  resumes stepping, and records **conditionally**: append the offset only when
  `base ≤ RIP < base+len` for the active region (mirrors the DynamoRIO client's
  "instrument always, record conditionally" gating and the AMD decoder's in-region
  skip). Execution that leaves the region into a callee is stepped but not recorded;
  a later `RIP` re-entering the region resumes recording — so callees and the
  begin/end glue never pollute the trace.
- `end(name)` clears `TF`, restores the prior `SIGTRAP` disposition, and deactivates
  the region. The trace is already populated (no post-pass decode, unlike PT/AMD).

**Region scoping note.** `TF` is armed across the whole `begin … call … end` window,
so the few glue instructions between the marker and the call are stepped (and
filtered out by the in-region test). Keep the bracket tight, exactly as the tier's
single-active-region MVP already advises.

**Acceptance.** For the shared fixture routine (`mov; add; cmp; jle; dec; ret`,
[examples/test_hwtrace.c](../../../examples/test_hwtrace.c)) called so the `jle` is
taken, `insn_offsets()` is exactly `[0x0, 0x3, 0x6, 0xc, 0x11]` — byte-for-byte the
Unicorn/DynamoRIO/PT/AMD result — captured live on this Zen 2 host.

---

## Phase 2 — Block-boundary parity *(LANDED)*

**Goal.** Produce the same normalized block partition the other backends do, from
the single-step instruction stream.

Single-step yields instructions, not branch edges, so blocks are derived: a block
starts at the **region entry** and at any `RIP` that is **not the fall-through** of
the previously recorded in-region instruction. Fall-through is
`prev_RIP + len(prev)`, where `len` comes from the Capstone length-decoder
(`asmtest_disas`, the same dependency [src/amd_backend.c](../../../src/amd_backend.c)
uses) — so a control transfer (taken branch, or region re-entry) is detected as a
discontinuity and opens a new block via `trace_append_block`. This reproduces the
single-entry/ends-at-branch model **identically to
[pt_backend.c](../../../src/pt_backend.c)** without needing hardware branch flags.

**Acceptance.** Single-step `blocks[]` == DynamoRIO `blocks[]` == PT/AMD `blocks[]`
for the shared fixtures (reusing the existing `hwtrace-test` cross-backend parity
harness; here it runs **live**, not against synthetic input).

---

## Phase 3 — Robustness & honest truncation *(LANDED)*

**Goal.** Never emit a corrupt or partial trace as if complete.

- **Undecodable / desync.** If `asmtest_disas` cannot decode at a recorded `RIP`
  (self-modifying or relocated code), set `trace->truncated = true` and stop — same
  loss bit PT/AMD use.
- **In-routine flag/control hazards.** A routine that itself executes `POPF`/`IRET`
  (can clear `TF`) or raises its own signal breaks naive stepping. Detect a cleared
  `TF` on entry to the handler and either re-arm or flag truncation; document that
  the supported target is a pure-compute routine (no in-routine signal handlers, no
  `POPF`/`IRET`/`syscall`). This is the same "registered bytes must be well-behaved"
  contract PT/AMD already carry, made explicit for the stepper.
- **Syscalls.** A `syscall` inside the region traps on return; the handler steps over
  it normally (the instruction stream stays exact). No special handling for the
  common case; documented as a known edge for syscall-heavy routines.

**Acceptance.** A routine with self-modifying bytes sets `truncated`; a clean
compute routine does not, across repeated runs (determinism).

---

## Phase 4 — Build, API, and binding surface *(LANDED)*

**Goal.** Wire the backend into the build and expose it through the existing API and
every language wrapper with zero new surface.

- **Makefile.** Add `ss_backend.o` to `HWTRACE_OBJS` and the PIC variant to
  `libasmtest_hwtrace` (mirroring `amd_backend.o`); no new external library, so no
  `LIBIPT_*`-style probe is needed — only the existing Capstone link for block mode.
- **API.** No new entry points: `asmtest_hwtrace_init({.backend =
  ASMTEST_HWTRACE_SINGLESTEP})` + the existing `register_region`/`begin`/`end`. The
  hwtrace `begin`/`end` dispatch gains a single-step arm alongside the AMD one.
- **Bindings.** Because the backend is selected by enum behind the unchanged
  `asmtest_hwtrace_*` C API, every binding gets it for free; add one self-test per
  binding that selects the single-step backend (the first hardware-tier test that
  actually runs **live on the Zen 2 CI host**, not just self-skips).

**Acceptance.** `make hwtrace-test` and `make docker-drtrace` (or a new
`docker-hwtrace`) exercise the single-step backend live on x86-64 Linux; the
cross-language wrappers trace and assert coverage on the same host.

---

## Phase 5 — Cross-OS and cross-arch portability *(mostly LANDED; two fronts remain — see below)*

The CPU mechanism (`TF` → `#DB`) is x86-universal, but the OS plumbing is per-OS and
the in-process variant does not exist on ARM. Each is an additive front-end behind a
swappable "stepper" seam designed in Phase 1; none is required for the shippable
Linux/x86-64 backend.

> **Status (2026-07-16; W3 corrected 2026-07-18).** This phase was originally filed
> *(forward-look)* in full; most of it has since shipped and the marker is corrected
> here. **Landed:** the Windows x86-64 VEH front-end, the macOS-Intel front-end, the
> whole **W2** out-of-process ptrace programme — `trace_call` / `trace_attached` /
> `_versioned` / `run_to`, call-depth awareness, the hardware-breakpoint `run_to` on
> **both** x86-64 and AArch64, the binary jitdump reader, the region resolvers, the
> code-image recorder, the AArch64 tracer, the out-of-process **BTF block-step** trio,
> the per-binding surface across all ten bindings, six live real-JIT lanes (V8 /
> CoreCLR / HotSpot × perf-map and jitdump), and — as of 2026-07-18 — the **in-process
> BTF** variant (W3, see below): a raw-MSR pinned-envelope tier with per-trap re-arm and
> honest truncation
> ([inproc-btf-block-step.md](../implementations/inproc-btf-block-step.md)); the
> "kernel helper / uapi patch" blocker this status note previously cited was not
> load-bearing. **One front remains:**
> - **AArch64 live single-step *stream*** — blocked on **real AArch64 hardware** (Apple
>   Silicon / Linux-ARM64 / Windows-on-ARM). The tracer is written; qemu-user cannot
>   validate it (it does not emulate the ptrace tracer/tracee relationship), so it
>   self-skips there.

- **Windows (x86-64)** *(shipped — see the status note above)*. Same `TF`, delivered
  as `EXCEPTION_SINGLE_STEP` to a Vectored Exception Handler (the classic technique);
  ~6× the Linux per-step cost. Landed in the
  [win64 native tier](../archive/plans/win64-native-tier-plan.md) as
  `src/ss_win64.c` + `make win64-ss-test` (Wine + real-Windows CI).
- **macOS (Intel).** The BSD signal layer delivers `SIGTRAP` in-process like Linux;
  out-of-process needs Mach exception ports + the `com.apple.security.cs.debugger`
  entitlement. Slots into the [macOS clean-test plan](macos-clean-test-plan.md).
- **W2 — out-of-process `ptrace` single-step + foreign-process tracing.** The
  out-of-band managed-runtime path: in-process DynamoRIO cannot take over a JIT/GC
  runtime's threads, IBS only samples statistically, and Intel PT is Intel-only — so on
  AMD (and for exact ARM64 single-step) this `ptrace` route is the way in. A tracer
  parent `PTRACE_SINGLESTEP`s the target and reads the program counter per stop,
  reconstructing the same exact stream as the in-process stepper. The Linux core has
  landed in stages on **x86-64 and AArch64**
  ([asmtest_ptrace.h](../../../include/asmtest_ptrace.h), `src/ptrace_backend.c`):
  - _Done._ `asmtest_ptrace_trace_call` — fork a tracee, single-step it, reconstruct
    the same offsets out of band (verified byte-for-byte incl. a 62-instruction loop).
  - _Done._ `asmtest_ptrace_trace_attached(pid, base, len, …)` — trace a region in a
    **separate, externally-attached** process; reads the target's bytes via
    `process_vm_readv` (no shared mapping); the caller owns the `PTRACE_ATTACH`/
    `DETACH` policy. Verified by attaching to a child that never called
    `PTRACE_TRACEME`.
  - _Done._ Region resolvers — `asmtest_proc_region_by_addr` (`/proc/<pid>/maps`, an
    address → its executable region) and `asmtest_proc_perfmap_symbol`
    (`/tmp/perf-<pid>.map`, the JIT text format → a method's `(base,len)` by name) — so
    the full flow **resolve → attach → trace → detach** runs end to end, live, on a
    foreign process.
  - _Done._ **Run-to-method** — `asmtest_ptrace_run_to(pid, addr)`, the glue that closes
    the *uncontrolled-timing* gap. `trace_attached` requires the target stopped **at** the
    region entry; against a real managed runtime you do not control **when** the program
    calls the method, so you cannot just attach at the right instant. `run_to` plants a
    software breakpoint at `addr` (`PTRACE_POKETEXT`, which patches even an r-x text page
    the way a debugger does — `int3` on x86, `brk #0` on AArch64), `PTRACE_CONT`s until the
    program **itself** next calls in, then removes the breakpoint and rewinds the PC,
    leaving the target stopped exactly at `addr` — the precondition `trace_attached`
    expects. `trace_attached` was also made robust to **either** entry convention (it now
    records the initial in-region PC, so a stop *at* the entry yields the identical stream
    as a stop *before* it). The complete real-JIT flow — **publish a perf-map → resolve by
    name → attach → run_to → trace** with **no cooperative go-flag** — is verified live in
    a plain unprivileged container (`make hwtrace-test`, `test_run_to_and_trace`).
  - _Done._ **Call-depth awareness** — `trace_call`/`trace_attached` no longer require a
    pure-compute *leaf*. The old model treated the first step out of the region as the
    return, so a routine that called a **runtime helper** (GC barrier, allocation, PLT
    stub — what real managed-runtime methods do) truncated at the first call. The stepper
    now decodes the region-exit instruction (`asmtest_disas_is_call`, Capstone
    `CS_GRP_CALL`): a **call** out of the region is run to its return address at **native
    speed** (the shared `run_until` breakpoint-cont, not a per-instruction step through the
    callee) and recording resumes after it; only a genuine return/tail-jump ends the
    trace. The region's own instructions are recorded, the helper is skipped, and the real
    return is still found. Verified live (`test_ptrace_callout`: a region that calls an
    out-of-region helper). Falls back to leaf-only without Capstone (the previous
    contract).
  - _Done._ **Real-runtime validation lanes (three engines)** — one argv-driven harness
    (`examples/jit_trace.c`) points the whole pipeline at a live JIT, not a fixture:
    - `make docker-hwtrace-jit` — **Node.js (V8)**: `node --perf-basic-prof
      --no-turbo-inlining` on a hot function; resolves the optimized method from V8's real
      perf-map, attaches to the live multi-threaded GC'd runtime, `run_to`s the entry, and
      single-steps one invocation — recovering the **actual TurboFan machine code** for
      `(a+b)|0` (frame setup, stack-limit check, smi type-guards, `add edx, ecx`, `ret`;
      29 instructions).
    - `make docker-hwtrace-jit-dotnet` — **.NET (CoreCLR)**: traces `Program::Add`,
      recovering the JIT's `lea eax, [rdi+rsi]; ret`. `DOTNET_TieredCompilation=0` gives a
      stable single compilation; `[MethodImpl(NoInlining)]` keeps it a real call target.
      It traces .NET's **W^X** code heap **as-shipped** (no `DOTNET_EnableWriteXorExecute=0`)
      via the hardware-breakpoint fallback below.
    - `make docker-hwtrace-jit-java` — **OpenJDK (HotSpot)**: traces `Hot.asmtjit`,
      recovering the C2 nmethod's `lea eax, [rsi+rdx]` (the `a+b` body, plus the nmethod
      entry barrier and stack-bang). `-XX:-TieredCompilation` gives one stable C2 body and
      `-XX:CompileCommand=dontinline,Hot.asmtjit` keeps it a real call target. Two HotSpot-
      only wrinkles, both absorbed by the harness: HotSpot does not stream a perf-map, so
      the lane drives `jcmd <pid> Compiler.perfmap` (JDK 17+) to dump one for the live
      process; and the `java` launcher runs `main()` on a **secondary** OS thread, so the
      harness finds the spinning loop thread by CPU-time delta and attaches that tid — an
      `int3` trap on a thread no tracer owns kills the process.

    All three are honest by construction: a watchdog bounds the step so a re-tiered/moved
    address self-skips rather than hangs; the resolve + attach checks (library vs. the
    runtime's real perf-map line and a real `/proc/maps`) are firm while the trace is
    asserted-or-skipped, so the lanes never flake. This closes the loop the W2 path was
    built for: tracing a foreign JIT's generated code on AMD, where Intel PT is
    unavailable and in-process DynamoRIO cannot seize the runtime's threads.
    - `make docker-hwtrace-jit-jitdump` — **binary jitdump path** (`asmtest_jitdump_find`)
      against a real V8 `jit-<pid>.dump` (`node --perf-prof`). Unlike the text perf-map
      (address/size/name), a jitdump carries the JIT's recorded **code bytes** — the byte
      source a branch-trace decoder needs — so this recovers a method's bytes and validates
      them three ways: the address agrees with V8's own perf-map (two independent V8
      outputs), the bytes disassemble to real x86-64, and they match the **live** code at
      that address (the temporal capture guarantee). This is the first validation of
      `asmtest_jitdump_find` against real output rather than a synthetic fixture.
    - `make docker-hwtrace-jit-java-jitdump` — a **second jitdump producer**: OpenJDK
      **HotSpot** via the perf JVMTI agent (`libperf-jvmti.so`, from `linux-tools`, loaded
      with `-agentpath`). A different runtime *and* encoder than V8 — it names methods in
      JVM descriptor form (`LHot;asmtjit(II)I`) and interleaves debug/unwinding records the
      reader skips — so it exercises `asmtest_jitdump_find` on genuinely independent output.
      Same three checks, with the cross-check against HotSpot's own `jcmd Compiler.perfmap`
      address (two independent HotSpot outputs). Self-skips if the agent is absent.
    - `make docker-hwtrace-jit-dotnet-jitdump` — a **third jitdump producer**: .NET
      **CoreCLR**, which writes a real `/tmp/jit-<pid>.dump` *natively* (no agent) under
      `DOTNET_PerfMapEnabled=1`, naming the method identically in the perf-map and the
      jitdump — so it reuses the same `trace_jitdump` path as V8 (parameterized by runtime).
      Recovers `Program::Add`'s bytes (`lea eax,[rdi+rsi]; ret`) and runs the same four
      checks. The binary jitdump reader is now validated against all three managed runtimes
      (V8, HotSpot, CoreCLR).
  - _Done._ **Hardware-breakpoint `run_to` (W^X JIT code)** — `run_until` (shared by
    `run_to` and the call-out step-over) defaults to a software `int3` but transparently
    falls back to an **x86-64 hardware execution breakpoint** (DR0 + DR7 via
    `PTRACE_POKEUSER`) when `PTRACE_POKETEXT` is refused — i.e. when the executable page is
    not writable, the case for a hardened **W^X** JIT code heap (.NET's default
    double-maps it, so POKETEXT fails with `EIO`). A hardware breakpoint writes no code, so
    it traces W^X code as-shipped, and is per-thread, so it never traps a sibling runtime
    thread the way a process-wide `int3` can. Software stays the default (no debug-register
    budget, no risk to the existing tests); `ASMTEST_PTRACE_HW_BP` forces the hardware path,
    which `test_ptrace_callout("hardware bp", …)` uses to validate it deterministically on
    ordinary memory (real DR0/DR7, in a plain container).
  - _Done — AArch64 hardware breakpoint._ *(Amended 2026-07-16: this was previously filed
    here as "a separate follow-on; there `run_to` is software-only for now", and named the
    interface `NT_ARM_HW_BKPT` — a constant that does not exist. Both are corrected.)*
    AArch64 reaches hardware execution breakpoints through the **`NT_ARM_HW_BREAK`**
    regset (`struct user_hwdebug_state`: `dbg_info` + `dbg_regs[]`), not debug-register
    `POKEUSER` like x86. It ships in [src/ptrace_backend.c](../../../src/ptrace_backend.c)
    as the same `set_hw_bp`/`clear_hw_bp` seam the x86-64 path uses, so `run_until` is
    arch-neutral: software `int3`/`brk` by default, hardware fallback on a `POKETEXT`
    refusal (the W^X JIT-text case), on **both** arches. The A64 control word is a 4-byte
    aligned EL0 execution breakpoint (`E=1`, `PMC=0b10`, `BAS=0b1111`). `set_hw_bp` fails
    when the host exposes no breakpoint slots — qemu-user emulates none — so `run_until`
    returns `ETRACE` there and the caller self-skips rather than hanging; like the AArch64
    stream itself, the live path is **pending real AArch64 hardware**.

  - _Done._ Binary jitdump reader — `asmtest_jitdump_find` parses the `jit-<pid>.dump`
    image format CoreCLR/HotSpot/V8 emit. Richer than the text perf-map: it carries the
    code **bytes** and per-method load **timestamps**, so a method re-emitted at a
    reused address (tiered/OSR) resolves to the **latest** body — the *temporal*
    same-address-different-bytes problem the [JIT runtime tracing
    analysis](../analysis/jit-runtime-tracing.md) centres on. (It is also the byte
    source the [hardware-trace plan, Phase 2](hardware-trace-plan.md) PT-attach path
    needs.) Endianness auto-detected; non-`LOAD` records skipped; verified by a
    synthetic 2-record-plus-re-JIT fixture.

  - _Done._ **Time-aware code-image recorder + versioned tracing** — the jitdump reader
    above resolves the *latest* body at a reused address, but a foreign JIT's bytes can
    change *during* a trace; `trace_attached`'s single `process_vm_readv` then decodes
    against the wrong bytes. `asmtest_codeimage` ([asmtest_codeimage.h](../../../include/asmtest_codeimage.h),
    `src/codeimage.c`) is the general fix: a userspace `PERF_RECORD_TEXT_POKE` that
    records a **timestamped code-image timeline** — `track`/`refresh` snapshot a region's
    bytes as versions (change detection via cross-process **soft-dirty + `PAGEMAP_SCAN`**,
    bytes via `process_vm_readv`), and `asmtest_codeimage_bytes_at(addr, when)` returns the
    bytes that were live at trace-position `when`. The stepper consumes it via
    `asmtest_ptrace_trace_attached_versioned(pid, base, len, img, when, …)` (the existing
    `trace_attached` is the `img == NULL` case). An **optional eBPF emission detector**
    (CO-RE on `mprotect`/`mmap`/`memfd_create`, PID-namespace-filtered via
    `bpf_get_ns_current_pid_tgid`, `bpf_ringbuf`) snapshots on the `PROT_EXEC` edge;
    built only with `clang`+`libbpf`+`bpftool` (`-DASMTEST_HAVE_LIBBPF`), self-skipping to
    the soft-dirty fallback otherwise. Live-validated: the same-address-different-bytes
    proof and the versioned W2 trace in `make codeimage-test` / `make hwtrace-test` (any
    x86-64 Linux, no privilege), the eBPF detector in `make docker-hwtrace-codeimage` (a
    `--cap-add=BPF,PERFMON` container — not privileged). This is approach #2 of the [JIT
    runtime tracing analysis](../analysis/jit-runtime-tracing.md) and the byte-source half
    of [hardware-trace Phase 2](hardware-trace-plan.md); the remaining Phase-2 piece is
    feeding this timeline into the Intel PT decoder (needs PT hardware — this host is AMD).

  - _Done._ Per-binding surface — the `asmtest_ptrace_*` / `asmtest_proc_*` /
    `asmtest_jitdump_*` C surface is exposed across **all ten** language bindings (a
    `Ptrace` class, or `HwTrace.ptrace_*` methods where idiomatic, surfacing
    `available`/`skip_reason`, `trace_call`, `trace_attached`, `trace_attached_versioned`,
    `run_to`, `region_by_addr`, `perfmap_symbol`, `jitdump_find` with a `JitMethod` value
    type), as is the `asmtest_codeimage_*` recorder (a `CodeImage` wrapper:
    `available`/`skip_reason`, `new`/`free`, `track`, `refresh`, `now`, `bytes_at`, and the
    `bpf_*` probes), each wrapping the C symbols of the already-loaded `libasmtest_hwtrace`;
    the live-testable subset is self-tested in every binding (`run_to`'s FFI round-trip via
    a NULL-addr → `EINVAL` probe + the code-image recorder's track→`bytes_at` round-trip,
    since a live foreign attach is impractical from a managed harness — the C suite covers
    that) and the surface is covered by the binding function-parity gate (now **51 tier
    symbols × 10 bindings**, validated live in all ten `docker-hwtrace-<lang>` lanes).
  - _Done — AArch64 tracer (Linux x86-64 **and** AArch64)._ ARM64 single-step
    (`MDSCR_EL1.SS`) is kernel-only, so out-of-process `ptrace` is its **only**
    single-step form (Apple Silicon, Windows-on-ARM, Linux/ARM64). The stepper rides the
    same `PTRACE_SINGLESTEP` seam on both arches, differing only in two seams: PC + the
    integer return register are read via `PTRACE_GETREGSET`/`NT_PRSTATUS` (AArch64 has no
    `PTRACE_GETREGS`), and block lengths decode with `ASMTEST_ARCH_ARM64` Capstone; the
    fork/SIGSTOP/step/wait control flow and the SysV/AAPCS64 register-arg call are one
    body. The `/proc` + jitdump **code-region readers** (`asmtest_proc_*`,
    `asmtest_jitdump_find`) — pure file parsing — became Linux-**any-arch** in the same
    change and are **validated live on AArch64** (in a `linux/arm64` container, where
    they pass). The single-step **trace capture** itself awaits a real AArch64 host:
    `asmtest_ptrace_available()` is a cached, hang-proof self-probe that returns 0 under
    qemu-user — which does not emulate the ptrace tracer/tracee relationship at all (a
    blocking `waitpid` for a `PTRACE_TRACEME` child never returns there) — so the stepper
    self-skips on emulation exactly as the PT/CoreSight tiers self-skip off their
    hardware. The AArch64 code fixtures are decode- and execute-validated under qemu;
    only the live single-step *stream* is pending real Apple-Silicon / Linux-ARM64 /
    Windows-on-ARM hardware. (The ten binding wrappers call the same C entry points and
    gate on `available()`, so they self-skip under emulation with no per-binding change;
    their own live AArch64 fixtures are a small follow-on alongside real-hardware
    validation.)
- **W3 — BTF branch-granular step.** `DEBUGCTL.BTF=1` + `TF=1` traps **only on taken
  branches** — one fault per branch (the AMD LBR waypoint set, no 16-entry ceiling),
  feedable into [amd_backend.c](../../../src/amd_backend.c)'s replay loop as `(from,to)`
  pairs. **The out-of-process form SHIPPED** (AMD plan P3-2): `PTRACE_SINGLEBLOCK`
  *is* wired on x86 (this plan's earlier "PowerPC/s390 only" claim was wrong —
  corrected by [src/ptrace_backend.c](../../../src/ptrace_backend.c)'s
  `asmtest_ptrace_trace_call_blockstep` / `_attached_blockstep`), behind a hang-proof
  functional probe because some hypervisors (GitHub-hosted runners included) mask
  `DEBUGCTL.BTF` and silently degrade SINGLEBLOCK to per-instruction stepping —
  `blockstep_available()` self-skips there. **The in-process form (no ptrace child)
  has since LANDED** as the raw-MSR pinned-envelope tier
  ([inproc-btf-block-step.md](../implementations/inproc-btf-block-step.md),
  `asmtest_ss_btf_available()` / `asmtest_ss_btf_trace()` in
  [src/ss_btf.c](../../../src/ss_btf.c)): the same thread-pinned `/dev/cpu/N/msr`
  route [src/msr_lbr.c](../../../src/msr_lbr.c) uses, with per-trap re-arm (BTF is a
  hardware one-shot the CPU clears on every `#DB`) and honest truncation on any
  observed context switch (Linux does not preserve a user-written BTF across one). The
  robust, context-switch-proof general form remains kernel-coupled and already ships
  as `PTRACE_SINGLEBLOCK` above — no non-ptrace block-step uapi exists upstream, so the
  raw-MSR tier is deliberately scoped to the pinned small-leaf-routine envelope, not a
  replacement for it.

---

## Deliverables (Phases 0–4)

- `ASMTEST_HWTRACE_SINGLESTEP` enum + a no-perf gating arm in
  `available()`/`skip_reason()` in
  [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) /
  [src/hwtrace.c](../../../src/hwtrace.c).
- `src/ss_backend.c`: the `TF`/`SIGTRAP` stepper + Capstone-based block
  normalization, its own TU like `pt_backend.c`/`amd_backend.c`, plus the `begin`/
  `end` dispatch arm in `hwtrace.c`.
- Makefile knobs: `ss_backend.o` in `HWTRACE_OBJS` and the PIC variant in
  `libasmtest_hwtrace` (no new external dependency).
- A **live** (not synthetic) cross-backend parity test added to `hwtrace-test`, and a
  per-binding live single-step test — the tier's first regression that runs on the
  Zen 2 CI host instead of self-skipping.

## Validation (single-step / Trap Flag)

- `EFLAGS.TF` single-step `#DB`: baseline x86, Intel SDM Vol 3 §17.3.1 / AMD APM
  Vol 2 §13.1.4; user-mode settable at CPL3 in long mode (not IOPL-gated like `IF`).
- Linux delivers the `#DB` as `SIGTRAP`; `ptrace(PTRACE_SINGLESTEP)` uses the same
  hardware `TF`. No `perf_event_paranoid` / `CAP_PERFMON` needed.
- Per-instruction overhead ≈2.3 µs on Linux (Jack Whitham, "x86 single step mode —
  how slow is it?", ~435k insn/s); ~13.8 µs on Windows.
- AMD `DEBUGCTL.BTF` branch single-step (Phase 5 W3): AMD APM Vol 2 bit 1 of
  `DebugCtlMSR`; Linux wires `PTRACE_SINGLEBLOCK` on x86 via
  `user_enable_block_step()` (the shipped blockstep tier uses it; a functional
  probe catches hypervisors that mask `DEBUGCTL.BTF` and silently degrade it).
- Unlike PT/AMD, capture **runs on this Zen 2 host** and on standard CI — the first
  hardware-tier backend with full automated regression protection.

## Risks and open points (single-step)

- **Overhead is the defining limit, not completeness.** Exact for any length, but a
  per-instruction kernel round-trip makes whole-program or hot-loop tracing
  impractical — that is the DynamoRIO tier's job. State the small-routine envelope;
  do not sell single-step as a general profiler.
- **`TF` armed across the begin→call glue.** The handler's in-region filter keeps the
  trace clean, but a large gap between `begin()` and the call wastes steps; keep the
  bracket tight (already the MVP guidance).
- **Trap-class `RIP` is the *next* instruction.** Record the current trap's `RIP`
  (about-to-execute) — verify the entry instruction (offset 0) is captured and the
  out-of-region return is not, against the fixture, before trusting parity.
- **In-routine `POPF`/`IRET`/signals/self-modifying code** break naive stepping;
  detect cleared `TF` / undecodable bytes and flag `truncated`. Supported target is a
  well-behaved compute routine — the same contract PT/AMD carry for "registered bytes
  must be stable," made explicit.
- **Reentrancy / threads.** Single active region, single thread, like the rest of the
  hwtrace MVP; the `SIGTRAP` handler and `TF` are per-thread, so a sibling thread is
  neither traced nor disturbed (a property the in-process DynamoRIO tier lacks).
