# Analysis: trace parity matrix (hardware, OS, microarchitecture, language)

*Status: analysis / findings. This document is a consolidated, cross-checked
reference for which of asm-test's trace backends work on which hardware, operating
system, CPU microarchitecture, and language binding. It is derived from the source
of record — [src/hwtrace.c](../../src/hwtrace.c)'s gating chain,
[include/asmtest_hwtrace.h](../../include/asmtest_hwtrace.h), and
[mk/native-trace.mk](../../mk/native-trace.mk) — and the shipped/planned status of
each tier. Narrative docs: [native runtime tracing](../native-tracing.md),
[emulator traces](../traces.md), [portability](../portability.md). Roadmaps:
[hardware-trace](../plans/hardware-trace-plan.md),
[AMD LBR](../plans/amd-lbr-trace-plan.md),
[Zen 2 single-step](../plans/zen2-singlestep-trace-plan.md),
[DynamoRIO native-trace](../plans/dynamorio-native-trace-plan.md).*

## Summary

asm-test has **one universal trace tier and a fragmented set of native ones**, all
filling the same `asmtest_trace_t` shape (ordered instruction offsets, distinct
basic-block offsets, totals, a truncation bit), so a test swaps backends without
changing how it reads coverage.

- **DynamoRIO** is the only vendor- and microarchitecture-independent *native*
  backend shipping today (Linux x86-64, every Intel + every Zen).
- **Hardware trace splits strictly by vendor/uarch:** Intel → Intel PT; AMD
  Zen 3 / Zen 4 / Zen 5 → AMD LBR (16-taken-branch cap; live-verified on a Zen 5
  Ryzen 9 9950X); AMD Zen 2 → nothing yet (its branch-stack `perf_event_open`
  returns `EOPNOTSUPP`).
- The **emulator (Unicorn)** tier is the universal floor for every OS, architecture,
  and language the native tiers do not reach (Windows-x64, macOS, AArch64, and the
  managed runtimes in-process DynamoRIO cannot take over).

Two facts worth stating up front because they are easy to get wrong:

- **Java traces *live*** on the DynamoRIO tier (the verified-live set is
  cpp/ruby/java/lua/zig/rust/go), but its make target is wrapped in the "Failed to
  take over all threads" → SKIP downgrade because the JVM is intermittent —
  distinct from Node/.NET, which **always** self-skip.
- The **single-step (Trap Flag) backend now ships** (Phases 0–4, Linux x86-64):
  `asmtest_trace_backend_t` is `{INTEL_PT, CORESIGHT, AMD_LBR, SINGLESTEP}`
  ([asmtest_hwtrace.h](../../include/asmtest_hwtrace.h)), and `src/ss_backend.c`
  drives `EFLAGS.TF`→`#DB`/`SIGTRAP` to record every executed `RIP` **live on any
  x86-64 Linux host** (no PMU, no perf, no privilege, no decoder beyond Capstone).
  Only the *cross-OS* variants (Windows VEH, macOS-Intel, out-of-process `ptrace`
  W2, AArch64) remain Phase-5 *(planned)* — so single-step rows below read
  *implemented* for Linux x86-64 and carry *(planned)* only where they name a
  Phase-5 front. See the [Zen 2 single-step plan](../plans/zen2-singlestep-trace-plan.md).

---

## Matrix 1 — Backend capability overview

| Backend | Mechanism | Decoder lib | License | Capture overhead | Completeness | Status |
|---|---|---|---|---|---|---|
| **Emulator** | Unicorn virtual CPU (isolated guest) | Capstone (annotate) | (Unicorn) | n/a (interpreted) | exact, unbounded | **implemented** |
| **DynamoRIO** | software DBI, native code cache | none | DR core BSD | low (cached) | exact, unbounded | **implemented** |
| **Intel PT** | continuous branch-trace AUX ring | libipt | BSD | near-zero | exact, unbounded (ring) | **implemented** |
| **AMD LBR** | 16-deep branch stack snapshot | Capstone (replay) | BSD | low (few PMIs) | exact ≤16 taken branches; else `truncated`→fallback | **impl. Ph0–4; live capture verified on Zen 5** (Ryzen 9 9950X, `amd_lbr_v2`) |
| **CoreSight** | ETM/ETE waypoints | OpenCSD | BSD | near-zero | decoder coarser; normalized to match | **scaffold** — always self-skips |
| **Single-step** | `EFLAGS.TF` → `#DB`/`SIGTRAP` | Capstone (block mode) | n/a | ~2.3 µs/insn (Linux) | exact, unbounded | **implemented** (Ph0–4, Linux x86-64; cross-OS Ph5 planned) |

DynamoRIO core is BSD (the tier deliberately avoids `drwrap`'s LGPL-2.1); libipt and
OpenCSD are BSD. Licensing for the optional emulator dependencies (Unicorn,
Keystone) is not asserted here.

---

## Matrix 2 — Host OS × architecture support

| Backend | x86-64 Linux | x86-64 macOS | x86-64 Windows | AArch64 Linux | AArch64 macOS |
|---|---|---|---|---|---|
| Emulator (Unicorn) | ✓ | ✓ | ✓ | ✓ | ✓ |
| DynamoRIO | ✓ | ✗ (follow-up) | ✗ | ✗ (follow-up) | ✗ |
| Intel PT | ✓ bare-metal | ✗ | ✗ | — | — |
| AMD LBR | ✓ (Zen 3+) | ✗ | ✗ | — | — |
| CoreSight | — | — | — | scaffold (board) | ✗ |
| Single-step | ✓ **shipped** | macOS-Intel (Ph5) | ✗ → Ph5 (VEH, ~6×) | ptrace-only (W2) | ptrace-only (W2) |

Windows-x64 and all of macOS/AArch64 are served **only by the emulator tier** today
(`emu_call_win64_traced` for the Win64 ABI). The two implemented native tiers are
Linux-x86-64-only.

---

## Matrix 3 — x86 native trace × CPU vendor / microarchitecture (Linux x86-64)

| Backend | Intel | AMD Zen 2 (Fam17h) | AMD Zen 3 (Fam19h) | AMD Zen 4 / Zen 5 | Self-skip reason when absent |
|---|---|---|---|---|---|
| DynamoRIO | ✓ | ✓ | ✓ | ✓ | — (vendor-independent DBI) |
| Intel PT | ✓ | ✗ | ✗ | ✗ | `no intel_pt PMU (needs bare-metal Intel; absent on AMD/VM)` |
| AMD LBR | ✗ | ✗ **no facility** | ✓ BRS | ✓ LbrExtV2 **(live-verified, Zen 5)** | `no AMD branch records (needs Zen 3 BRS / Zen 4 LbrExtV2)` |
| Single-step | ✓ | ✓ | ✓ | ✓ | (none — no PMU/perf/privilege needed) |

Zen 2's branch-stack `perf_event_open` returns `EOPNOTSUPP` → `AMD_NOHW`
([hwtrace.c `amd_branch_probe`](../../src/hwtrace.c)); its legacy LBR is depth-1 and
not wired to perf branch-stack. So on Zen 2 the only exact native options are
**DynamoRIO** and **single-step** (both shipping today). Zen 3 uses **BRS** (Family 19h,
opt-in `branch-brs` event); Zen 4 and **Zen 5** use **LbrExtV2** (mainline Linux 6.1+);
both are a fixed 16-entry stack, so the AMD backend is exact only within a
16-taken-branch window (Tier A) and sets `truncated` to route longer routines to
DynamoRIO.

**Live-verified on Zen 5.** The AMD LBR capture+decode path was run on a **Ryzen 9
9950X (Family 0x1A, Zen 5, `amd_lbr_v2`)** — the project's actual dev host
([test_amd_live](../../examples/test_hwtrace.c), `make docker-hwtrace-amd`). Two
findings from that first real-hardware run:

- It **works**: for a branch-heavy routine a PMU sample fires *inside* the region, so
  LbrExtV2 delivers a full 16-deep in-region branch stack and the decoder
  reconstructs it exactly (loop-body block + ordered offsets, `truncated` once the
  routine exceeds 16 taken branches — Tier A as specified).
- perf gives the 16-deep stack **only at a sample**, so a **tiny single-shot routine
  is too fast to be sampled in-region** and the capture honestly sets `truncated`
  (the dynamic-fallback signal) rather than emitting an empty trace. This corrected a
  capture bug: `hwtrace_end_amd` kept the *last* perf sample (all post-routine glue
  branches for a small routine, decoding to nothing yet reported complete); it now
  keeps the sample **richest in in-region branches** and flags `truncated` when none
  is found.

---

## Matrix 4 — Emulator guest ISAs (run on any host)

| Guest ABI | Traced-call entry point |
|---|---|
| x86-64 System V | `emu_call_traced` |
| x86-64 Win64 | `emu_call_win64_traced` |
| AArch64 | `emu_arm64_call_traced` |
| RISC-V RV64 | `emu_riscv_call_traced` |
| ARM32 | `emu_arm_call_traced` |

The emulator is host-independent because it never touches the real CPU; it is also
the only tier offering cross-ISA guests, isolated guest memory, and faults-as-data.

---

## Matrix 5 — Language bindings × backend

| Language | Emulator trace | DynamoRIO (Linux x86-64) | Hardware tier |
|---|---|---|---|
| C (native) | ✓ | ✓ | ✓ (C API) |
| Python (ref) | ✓ | ✓ live (CPython supported) | via C |
| C++ | ✓ | ✓ live | via C |
| Rust | ✓ | ✓ live | via C |
| Go | ✓ | ✓ live | via C |
| Ruby | ✓ | ✓ live | via C |
| Lua | ✓ | ✓ live | via C |
| Zig | ✓ | ✓ live | via C |
| Java | ✓ | ⚠ live but guarded (intermittent JVM threads) | recommended via PT / W2 |
| Node | ✓ | ✗ self-skip (JIT/GC threads) | recommended via PT / W2 |
| .NET | ✓ | ✗ self-skip (JIT/GC threads) | recommended via PT / W2 |

All 10 wrappers expose an identical surface and dlopen `libasmtest_drapp` at run
time, so `available()` self-skips cleanly wherever the tier is not built. The
Node/.NET gap is the managed-runtime takeover limit (`dr_app_start` aborts with
*"Failed to take over all threads"*), not a missing wrapper.

---

## Matrix 6 — Concurrency / region model (limits)

| Backend | Region activation scope | Active regions | Thread safety |
|---|---|---|---|
| Emulator | per emulator handle / call | n/a | not a shared collector; one trace per worker |
| DynamoRIO | **per-thread** | multiple (non-overlapping); register before start | trace bound to one thread's activation |
| Hardware (PT / AMD) | **process-global single slot** | **one at a time** (begin while active is ignored) | per-thread `TF` for single-step only |

---

## Matrix 7 — Recommendation: target → backend

| Target | Primary | Fallback | Rationale |
|---|---|---|---|
| Intel bare-metal, small routine | **Intel PT** | DynamoRIO | complete + near-zero overhead |
| Intel/AMD, long or looping routine | **DynamoRIO** | — | native-speed code cache, no depth ceiling, vendor-independent |
| AMD Zen 3 / Zen 4, small routine | **AMD LBR (Tier A)** | DynamoRIO (on `truncated`) | HW-attributed, exact within 16 branches |
| AMD Zen 2 | **DynamoRIO** | single-step | no branch facility exists on Zen 2 |
| Any x86, exact + unprivileged + on CI | **single-step** | DynamoRIO | no PMU/perf/privilege; only depth-unbounded HW-tier path runnable on CI |
| Managed runtime (JVM/.NET/Node), Intel | **Intel PT** | — | observes out-of-band; no thread takeover, no signal collision |
| Managed runtime, AMD | **W2 out-of-proc `ptrace` single-step** *(planned)* | DynamoRIO (best-effort) | PT is Intel-only; in-proc DR cannot seize JIT/GC threads |
| Windows-x64 / macOS / AArch64 (any) | **Emulator (Unicorn)** | — | only tier covering these hosts today; CoreSight is a scaffold |
| Cross-ISA / isolation / faults-as-data | **Emulator (Unicorn)** | — | the only tier with isolated guests + 5 ISAs + precise faults |

The managed-runtime-on-AMD path is **W2 (out-of-process `ptrace` single-step)**, not
W3 (`DEBUGCTL.BTF` branch-granular step, which is ring-0-blocked in portable form).
See the [Zen 2 single-step plan, Phase 5](../plans/zen2-singlestep-trace-plan.md).

---

## Fallback model

The matrices above are static capability snapshots. This section is the dynamic
question: given a concrete `(OS, hardware, language)` tuple, **how does a caller
fall from the most faithful backend down to one that actually runs** — and what does
each step cost?

### What makes a fallback cascade possible — and where it stops

Every tier reduces to two primitives that make a cascade tractable:

- **A static capability probe.** `asmtest_hwtrace_available(backend)` and each
  binding's `NativeTrace.available()` self-skip cleanly with a specific reason
  ([hwtrace.c `asmtest_hwtrace_skip_reason`](../../src/hwtrace.c)). A cascade is just
  these probes tried in priority order, terminating at the emulator — which is always
  available.
- **A dynamic completeness signal.** `asmtest_trace_t.truncated` is set when a
  backend could not record the whole path: AMD LBR window overflow (>16 taken
  branches), Intel PT AUX-ring overflow, or single-step desync. It is the *runtime*
  fallback trigger, distinct from the *init-time* availability probe.

Both are safe to act on because **every backend normalizes to the same offset basis**
(byte offset from routine entry `0`) and the **same block partition** (single-entry /
ends-at-branch). A test asserting `covered(0)` or `block_offsets() == [...]` reads
identically no matter which backend produced the trace — that data-shape parity is
exactly what lets one backend stand in for another without rewriting assertions.

**The boundary the cascade must not cross silently.** Data shape is preserved across
every fallback; **execution fidelity is not.** The four native tiers (PT, AMD LBR,
DynamoRIO, single-step) all trace *real in-process native execution*; the emulator
traces *isolated guest bytes on a virtual CPU*. For the deterministic, branch-light
compute kernels that are asm-test's common case the two are **coverage-equivalent**,
but they diverge for anything environment-dependent (real addresses, syscalls, native
faults, self-modifying code). So a native→native fallback (e.g. PT→DynamoRIO) is
transparent, while a native→emulator fallback crosses a semantic line and should be an
**explicit caller opt-in**, not an automatic last resort.

| Fallback step | Trace shape | Execution fidelity | Treat as |
|---|---|---|---|
| native → native (PT → LBR → DynamoRIO → single-step) | identical | preserved (real CPU, in-process) | transparent |
| native → emulator | identical | **changes** (virtual CPU, isolated guest) | opt-in; coverage-equivalent only for pure-compute routines |

### Static vs dynamic fallback

| Trigger | Signal | Fires at | Canonical example |
|---|---|---|---|
| Static (capability) | `available()` == 0 + skip reason | init | PT on AMD; DynamoRIO on macOS; LBR on Zen 2 |
| Dynamic (completeness) | `trace.truncated` == true | after `end()` | AMD LBR routine exceeding 16 taken branches |

### Matrix 8 — Hardware / microarchitecture fallback chain (Linux x86-64, native runtime)

| CPU | Resolution order (first `available()` wins) |
|---|---|
| Intel, bare-metal | Intel PT → DynamoRIO → single-step → emulator |
| Intel, VM / cloud | DynamoRIO → single-step → emulator *(PT self-skips: no `intel_pt` PMU)* |
| AMD Zen 3 / Zen 4, bare-metal, ≤16 branches | AMD LBR → DynamoRIO → single-step → emulator |
| AMD Zen 3 / Zen 4, routine > 16 branches | *(LBR sets `truncated`)* → DynamoRIO → single-step → emulator |
| AMD Zen 2 | DynamoRIO → single-step → emulator *(no branch facility exists)* |
| AMD, VM / cloud | DynamoRIO → single-step → emulator |

### Matrix 9 — OS / architecture fallback chain

| OS / arch | Resolution order |
|---|---|
| Linux x86-64 | *(full native cascade — see Matrix 8)* → emulator |
| Linux AArch64 | CoreSight *(scaffold → self-skips)* → out-of-proc ptrace single-step (W2, *planned*) → emulator |
| macOS Intel | single-step (macOS-Intel, *planned* Ph5) → emulator |
| macOS Apple Silicon | **emulator only** |
| Windows x64 | single-step (VEH, *planned* Ph5) → emulator |

Today every non-Linux-x86-64 row collapses to the **emulator** in practice, because
the native tiers above it are either follow-ups or scaffolds. The Win64 ABI is traced
by the emulator via `emu_call_win64_traced`.

### Matrix 10 — Language-runtime fallback chain (within a host that has native tiers)

| Runtime class | Resolution order |
|---|---|
| Native / compiled (C, C++, Rust, Go, Zig, Ruby, Lua) | in-process DynamoRIO (+ HW trace where the hardware row allows) → emulator |
| CPython (GIL-serialized, no JIT) | in-process DynamoRIO *(supported managed target)* → emulator |
| JVM | DynamoRIO *(best-effort, guarded)* → out-of-band Intel PT / W2 ptrace *(planned)* → emulator |
| Node / .NET | out-of-band Intel PT (Intel) / W2 ptrace single-step *(planned)* → emulator *(in-process DynamoRIO self-skips: cannot take over JIT/GC threads)* |

The language axis is **orthogonal** to hardware: it does not change *whether* a trace
facility exists, only whether *in-process* attach is viable. A managed JIT/GC runtime
forces the cascade toward **out-of-band** observation (hardware trace, or the planned
out-of-process ptrace stepper) before reaching the emulator floor.

### Composed resolution

The three axes compose as a filter pipeline, not three independent choices:

```
resolve(os, arch, vendor, uarch, runtime, routine_profile):
    chain = []
    if os == Linux and arch == x86-64:
        if runtime is managed (Node/.NET/JVM):
            # in-process DBI is hostile → prefer out-of-band first
            if vendor == Intel: chain += [INTEL_PT]
            chain += [PTRACE_SINGLESTEP_W2]          # planned; out-of-band, any x86
            if runtime == JVM:  chain += [DYNAMORIO] # best-effort, guarded
        else:                                        # native / GIL-serialized
            if routine is small and branch-light:
                if vendor == Intel:                  chain += [INTEL_PT]
                elif vendor == AMD and uarch >= Zen3: chain += [AMD_LBR]   # Tier A
            chain += [DYNAMORIO]                     # vendor/uarch-independent, no ceiling
            chain += [SINGLESTEP]                    # shipped; exact, unprivileged, CI-safe
    chain += [EMULATOR]                              # universal floor (always available)

    for b in chain:
        if available(b): return b        # static capability fallback

# After end(): if trace.truncated, re-resolve from the next ceiling-free backend
# (DynamoRIO → single-step → emulator). This is the dynamic completeness fallback.
```

### Matrix 11 — Worked examples (composed tuples → resolved chain)

| OS / arch | CPU | Language | Resolves to (chain) |
|---|---|---|---|
| Linux x86-64 | Intel bare-metal | Rust | **Intel PT** → DynamoRIO → emulator |
| Linux x86-64 | Intel cloud VM | Python | **DynamoRIO** → emulator *(PT self-skips)* |
| Linux x86-64 | AMD Zen 4 bare-metal | C, small kernel | **AMD LBR** → DynamoRIO → emulator |
| Linux x86-64 | AMD Zen 4 bare-metal | C, looping kernel | LBR `truncated` → **DynamoRIO** → emulator |
| Linux x86-64 | AMD Zen 5 (dev host) bare-metal | C, looping kernel | **AMD LBR** *(live-verified)* → DynamoRIO → single-step → emulator |
| Linux x86-64 | AMD Zen 2 | Go | **DynamoRIO** → single-step → emulator *(no branch facility)* |
| Linux x86-64 | Intel bare-metal | Node | **Intel PT** → emulator *(DynamoRIO self-skips)* |
| Linux x86-64 | AMD Zen 3 | .NET | W2 ptrace *(planned)* → **emulator** *(no PT on AMD; DR self-skips)* |
| Linux AArch64 | — | Java | **emulator** *(CoreSight scaffold; no in-proc stepper on ARM)* |
| macOS Apple Silicon | — | any | **emulator** only |
| Windows x64 | any | .NET | **emulator** (`emu_call_win64_traced`) |

### What is automated, and what is still missing

The framework ships the **primitives** — per-backend `available()`, the `truncated`
completeness bit, and a single `asmtest_trace_t` shape every backend fills.

**The hardware tier now also ships the orchestrator over its own backends.**
`asmtest_hwtrace_resolve(policy, out, cap)` walks `{Intel PT, AMD LBR, single-step,
CoreSight}` in descending-fidelity order and returns those that `available()`, and
`asmtest_hwtrace_auto(policy)` returns the single best pick ready to `init`
([hwtrace.c](../../src/hwtrace.c), [asmtest_hwtrace.h](../../include/asmtest_hwtrace.h)).
The `policy` encodes the static *and* the dynamic fallback: `ASMTEST_HWTRACE_BEST`
picks the most faithful backend; `ASMTEST_HWTRACE_CEILING_FREE` drops the one
fixed-window backend (AMD LBR, 16 taken branches) and is what a caller re-resolves
under after a trace comes back `truncated`. On any x86-64 Linux host the cascade is
non-empty (single-step is the floor), so it never fails to resolve. This is exactly
the within-hardware-tier portion of Matrix 8.

**What is still missing is the *cross-tier* front-end.** `asmtest_hwtrace_resolve`
stops at the hardware tier's library boundary: the DynamoRIO tier
(`libasmtest_drapp`, located by `DYNAMORIO_HOME`) and the emulator tier
(`libasmtest_emu`) are separate libraries with their own call APIs, and a fall to
the emulator crosses a fidelity line (real CPU → isolated guest). So a full
`asmtest_trace_auto(...)` spanning all of Matrix 8–10 — with a flag to forbid the
native→emulator crossing — remains a follow-up; deliberately, the cross-tier fall
stays an explicit, fidelity-aware integrator decision rather than an automatic last
resort (consistent with the [AMD LBR plan, Phase 4](../plans/amd-lbr-trace-plan.md)
overflow→DynamoRIO routing rule, which is specified for "where a caller orchestrates
backends").

---

## Packaging & target-architecture coverage

There is a fourth axis the OS/hardware/language matrices do not capture: **what an
installed package actually ships.** It matters because the answer collapses the
fallback cascade dramatically — for a `pip install` / `npm install` / `gem install`
consumer, only **one** trace tier is present out of the box on **every** platform.

### What each package carries

Per [packaging.md](../packaging.md), bindings reach the framework two ways and that
decides what ships:

- **dlopen bindings** (Python, Ruby, Lua, Node, Java, .NET) bundle the prebuilt
  **`libasmtest_emu`** — the superset: capture trampoline + opaque-handle FFI +
  **emulator** + Keystone assembler + Capstone disassembler, with Unicorn/Keystone/
  Capstone vendored and rpath-rewritten so the package is self-contained.
- **link / source bindings** (Rust, Zig, C++, Go) ship source and build against
  `libasmtest` / `libasmtest_emu` on the consumer's host.

Crucially, **neither native trace tier is in any package.** DynamoRIO ships as
`libasmtest_drapp` + `libasmtest_drclient.so`, the hardware tier as
`libasmtest_hwtrace`; none is bundled by `make <lang>-package`. The wrappers dlopen
`libasmtest_drapp` at run time from `$ASMTEST_DRAPP_LIB` (else the repo `build/`), so
`NativeTrace.available()` returns false until the consumer **builds them separately**
and wires the env — exactly the clean self-skip the fallback model relies on.

### Matrix 12 — Trace tier × packaging

| Trace tier | Carrying library | In dlopen packages? | In source packages? | Otherwise obtained by |
|---|---|---|---|---|
| **Emulator** | `libasmtest_emu` (superset) | ✅ bundled, every platform slot | built from source | — (always present) |
| **DynamoRIO** | `libasmtest_drapp` + `libasmtest_drclient.so` | ❌ not bundled | ❌ | `make shared-drtrace drtrace-client` + `DYNAMORIO_HOME` + env (no pkg-config) |
| **Hardware** (PT / AMD LBR / CoreSight) | `libasmtest_hwtrace` | ❌ not bundled | ❌ | `make shared-hwtrace` + libipt/OpenCSD + bare metal + perf privilege |

Why the native tiers stay out: DynamoRIO is a large runtime with **no pkg-config**
(located by `DYNAMORIO_HOME`), impractical to vendor; the hardware tier needs
bare-metal + lowered `perf_event_paranoid` that a package cannot grant. Their
**BSD** licensing (DR core, libipt, OpenCSD) is therefore moot for the package
license — only the bundled `libasmtest_emu` matters, which makes every dlopen package
effectively **GPL-2.0** (Unicorn/Keystone GPL-2.0; Capstone + asm-test BSD/MIT).

### Matrix 13 — Packaging platform slots × trace tiers reachable

The CI `payloads` matrix stages four native slots (`<os>-<arch>`); there is **no
Windows package slot** (the Win64 tier is cross-compile + Wine, not a published
payload).

| Platform slot | Built by (CI runner) | Emulator (packaged) | DynamoRIO (BYO build) | Hardware (BYO build) |
|---|---|---|---|---|
| `linux-x86_64` | ubuntu-latest | ✅ | buildable | PT (Intel) / AMD LBR (Zen 3+), bare-metal |
| `linux-aarch64` | ubuntu-24.04-arm | ✅ | ✗ (DR is x86-64 only) | CoreSight scaffold only |
| `darwin-arm64` | macos-latest | ✅ | ✗ | ✗ |
| `darwin-x86_64` | macos-13 (nightly) | ✅ | ✗ | ✗ |
| `windows-x64` | — (no slot) | source / Win64 tier only | ✗ | ✗ |

Per-ecosystem the slot is named conventionally — Java `native/<os>-<arch>/`, .NET
RIDs (`osx-arm64`, `linux-x64`, …), Python per-platform wheel tags repaired by
`auditwheel`/`delocate`. The **emulator guest ISAs (Matrix 4) are independent of the
slot**: a `linux-x86_64` package still traces AArch64/RISC-V/ARM32 *guests*, because
the guest runs inside Unicorn, not on the host CPU.

### Packaging as the fourth fallback axis

Composing this with Matrix 8–11: an **installed package's effective cascade has a
single rung — the emulator — on every platform**, because no native tier is bundled.
The native tiers re-enter the cascade only when the consumer *leaves the package* and
builds them, and even then only on `linux-x86_64` (DynamoRIO; PT/AMD LBR bare-metal).
So three distinct "reachability" surfaces stack up:

| Surface | What it can trace |
|---|---|
| **Platform capability** (Matrix 2–3) | every native tier the OS/CPU physically supports |
| **Source checkout** (`make shared-drtrace` / `shared-hwtrace`) | the native tiers too, on `linux-x86_64`, with the toolchain installed |
| **Installed package** (`pip`/`npm`/`gem`/…) | **emulator only**, on all four slots |

The practical consequence: native-trace fidelity is a **build-from-source decision**,
not an install-a-package one. A consumer who needs DynamoRIO or Intel PT must take the
source/link path (or stage the libs and set `ASMTEST_DRAPP_LIB`/`ASMTEST_DRCLIENT`/
`ASMTEST_DR_LIB`), and only on a `linux-x86_64` host; everyone else — and every
out-of-the-box package install — lands on the emulator floor.

---

## One-line synthesis

DynamoRIO is the only vendor- and uarch-independent native backend shipping today
(Linux x86-64, every Intel + Zen); hardware trace splits strictly
**Intel → PT / Zen 3-5 → LBR (16-cap, live-verified on Zen 5) / Zen 2 → nothing-yet**;
the emulator is the
universal floor for every OS, architecture, and language the native tiers do not
reach. Fallback between them is cheap and transparent **as data** (one trace shape,
one offset basis) but must be deliberate **as fidelity** — native→native is free,
native→emulator trades real-CPU execution for an isolated guest and should be opt-in.
And packaging tightens the floor further: only the emulator tier is bundled, so every
out-of-the-box package install — on all four platform slots — starts and ends on the
emulator unless the consumer builds a native tier from source on `linux-x86_64`.
