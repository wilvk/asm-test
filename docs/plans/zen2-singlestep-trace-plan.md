# asm-test — single-step (Trap Flag) native-trace backend: implementation plan

A phased roadmap for a **debug-exception** native-trace backend that delivers
**exact, complete** `asmtest_trace_t` offsets on hardware that has *no* branch-trace
facility at all — most importantly **AMD Zen 2** (Family 17h), the project's primary
dev host. Where the [Intel PT](hardware-trace-plan.md) and
[AMD LBR](amd-lbr-trace-plan.md) backends reconstruct the instruction stream from a
hardware *trace* (a PT AUX ring, or a 16-deep branch stack), this backend
reconstructs it from the x86 **single-step debug exception** (`#DB`) driven by the
`EFLAGS.TF` trap flag — a baseline ISA feature present on *every* x86 CPU since
AMD64 launched, requiring no PMU, no `perf_event`, no privilege, and no decoder
library. The output is identical to the other backends: instruction offsets
matching the Unicorn emulator, the DynamoRIO native tier, and Intel PT, and block
offsets that match after the same branch-edge normalization.

This plan is a **sibling** of the [hardware-trace plan](hardware-trace-plan.md)
(Intel PT / ARM CoreSight), the [AMD LBR plan](amd-lbr-trace-plan.md) (Zen 3 BRS /
Zen 4 LbrExtV2), and the [DynamoRIO native-trace plan](dynamorio-native-trace-plan.md).
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
> [amd-lbr-trace-plan.md](amd-lbr-trace-plan.md) track theirs.

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

> **Correction (dev host is Zen 5, not Zen 2).** This plan was written assuming the
> project's primary dev host is an AMD **Zen 2** box with no branch facility — making
> single-step "the only exact native option here." The host is in fact a **Ryzen 9
> 9950X (Family 0x1A, Zen 5, `amd_lbr_v2`)**, on which **AMD LBR is available and
> live-verified** (see the [AMD LBR plan](amd-lbr-trace-plan.md) and
> [trace-parity-matrix](../analysis/trace-parity-matrix.md)). The single-step
> backend's value is **unchanged** — it is the portable, no-PMU/perf/privilege,
> depth-unbounded backend that runs on *any* x86-64 Linux host (including a true
> Zen 2, VMs, plain containers, and standard CI) and is the deterministic in-process
> path for tiny single-shot routines that AMD LBR's sampling cannot snapshot. Read
> the "this Zen 2 host" phrasing below as "this x86-64 Linux dev host": single-step
> runs live there regardless of microarchitecture.

**Phases 0–4 implemented (Linux/x86-64).** The `ASMTEST_HWTRACE_SINGLESTEP` backend
ships: gating ([src/hwtrace.c](../../src/hwtrace.c)), the stepper + block
normalization ([src/ss_backend.c](../../src/ss_backend.c)), the begin/end dispatch
arm, the Makefile wiring (`ss_backend.o` in `HWTRACE_OBJS` + the PIC/`shared-hwtrace`
variant), a **live** cross-backend parity test in
[examples/test_hwtrace.c](../../examples/test_hwtrace.c) (`make hwtrace-test`, plus a
20-trip loop proving no depth ceiling), an `hwtrace` language wrapper + live test for
**all ten bindings** (`make hwtrace-bindings-test`), and **plain-container** Docker
lanes (`make docker-hwtrace` / `docker-hwtrace-bindings` — no privilege, no
`CAP_PERFMON`, no seccomp change). The existing hardware-trace backends (Intel PT /
CoreSight / AMD LBR) are unaffected — this is a fourth, perf-free backend behind the
same `asmtest_hwtrace_*` API, selected by enum exactly as PT and AMD LBR are.

**Phase 5, W2 (out-of-process `ptrace`) now ships for Linux x86-64**
([asmtest_ptrace.h](../../include/asmtest_ptrace.h), `src/ptrace_backend.c`). Two
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
attach → trace → detach) end to end in the live test. The remaining Phase-5 fronts
(Windows VEH, macOS-Intel, and the **AArch64** ptrace tracer — whose `MDSCR_EL1.SS` is
kernel-only, so out-of-process is its *only* single-step form) stay forward-look, as
does the richer **binary jitdump** code-image reader (the text perf-map is the portable
lowest common denominator already supported).

---

## Phase 0 — Backend gating & feature detection *(planned)*

**Goal.** Make `asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)` return 1 on
any Linux x86-64 host (no privilege, no PMU, no decoder library required) and
self-skip elsewhere with a specific reason.

**Work.**

- Add `ASMTEST_HWTRACE_SINGLESTEP` to `asmtest_trace_backend_t` in
  [include/asmtest_hwtrace.h](../../include/asmtest_hwtrace.h).
- In [src/hwtrace.c](../../src/hwtrace.c)'s gating chain, give the new backend its
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

## Phase 1 — The stepper (`src/ss_backend.c`) *(planned)*

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
[examples/test_hwtrace.c](../../examples/test_hwtrace.c)) called so the `jle` is
taken, `insn_offsets()` is exactly `[0x0, 0x3, 0x6, 0xc, 0x11]` — byte-for-byte the
Unicorn/DynamoRIO/PT/AMD result — captured live on this Zen 2 host.

---

## Phase 2 — Block-boundary parity *(planned)*

**Goal.** Produce the same normalized block partition the other backends do, from
the single-step instruction stream.

Single-step yields instructions, not branch edges, so blocks are derived: a block
starts at the **region entry** and at any `RIP` that is **not the fall-through** of
the previously recorded in-region instruction. Fall-through is
`prev_RIP + len(prev)`, where `len` comes from the Capstone length-decoder
(`asmtest_disas`, the same dependency [src/amd_backend.c](../../src/amd_backend.c)
uses) — so a control transfer (taken branch, or region re-entry) is detected as a
discontinuity and opens a new block via `trace_append_block`. This reproduces the
single-entry/ends-at-branch model **identically to
[pt_backend.c](../../src/pt_backend.c)** without needing hardware branch flags.

**Acceptance.** Single-step `blocks[]` == DynamoRIO `blocks[]` == PT/AMD `blocks[]`
for the shared fixtures (reusing the existing `hwtrace-test` cross-backend parity
harness; here it runs **live**, not against synthetic input).

---

## Phase 3 — Robustness & honest truncation *(planned)*

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

## Phase 4 — Build, API, and binding surface *(planned)*

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

## Phase 5 — Cross-OS and cross-arch portability *(forward-look)*

The CPU mechanism (`TF` → `#DB`) is x86-universal, but the OS plumbing is per-OS and
the in-process variant does not exist on ARM. Each is an additive front-end behind a
swappable "stepper" seam designed in Phase 1; none is required for the shippable
Linux/x86-64 backend.

- **Windows (x86-64).** Same `TF`, delivered as `EXCEPTION_SINGLE_STEP` to a Vectored
  Exception Handler (the classic technique); ~6× the Linux per-step cost. Slots into
  the [win64 native tier](win64-native-tier-plan.md).
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
  ([asmtest_ptrace.h](../../include/asmtest_ptrace.h), `src/ptrace_backend.c`):
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

  - _Done._ Binary jitdump reader — `asmtest_jitdump_find` parses the `jit-<pid>.dump`
    image format CoreCLR/HotSpot/V8 emit. Richer than the text perf-map: it carries the
    code **bytes** and per-method load **timestamps**, so a method re-emitted at a
    reused address (tiered/OSR) resolves to the **latest** body — the *temporal*
    same-address-different-bytes problem the [JIT runtime tracing
    analysis](../analysis/jit-runtime-tracing.md) centres on. (It is also the byte
    source the [hardware-trace plan, Phase 2](hardware-trace-plan.md) PT-attach path
    needs.) Endianness auto-detected; non-`LOAD` records skipped; verified by a
    synthetic 2-record-plus-re-JIT fixture.

  - _Done._ Per-binding surface — the `asmtest_ptrace_*` / `asmtest_proc_*` /
    `asmtest_jitdump_*` C surface is exposed across **all ten** language bindings (a
    `Ptrace` class, or `HwTrace.ptrace_*` methods where idiomatic, surfacing
    `available`/`skip_reason`, `trace_call`, `trace_attached`, `region_by_addr`,
    `perfmap_symbol`, `jitdump_find` with a `JitMethod` value type), each wrapping the
    seven C symbols from the already-loaded `libasmtest_hwtrace`; the live-testable
    subset is self-tested in every binding and the surface is now covered by the binding
    function-parity gate (36 tier symbols × 10 bindings).
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
  feedable into [amd_backend.c](../../src/amd_backend.c)'s replay loop as `(from,to)`
  pairs. Blocked on x86 in portable form: `PTRACE_SINGLEBLOCK` is wired only on
  PowerPC/s390, and `DEBUGCTL` is a ring-0 MSR — so this needs a small kernel helper
  module or a uapi patch. Self-hosted-runner / research only, same bucket as the AMD
  plan's "MSR-direct snapshot."

---

## Deliverables (Phases 0–4)

- `ASMTEST_HWTRACE_SINGLESTEP` enum + a no-perf gating arm in
  `available()`/`skip_reason()` in
  [include/asmtest_hwtrace.h](../../include/asmtest_hwtrace.h) /
  [src/hwtrace.c](../../src/hwtrace.c).
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
  `DebugCtlMSR`; Linux `user_enable_block_step()` exists but `PTRACE_SINGLEBLOCK` is
  unwired on x86 (PowerPC `0x100` / s390 only).
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
