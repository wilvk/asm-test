# Analysis: trace parity matrix (hardware, OS, microarchitecture, language)

*Status: analysis / findings. This document is a consolidated, cross-checked
reference for which of asm-test's trace backends work on which hardware, operating
system, CPU microarchitecture, and language binding. It is derived from the source
of record — [src/hwtrace.c](../../../src/hwtrace.c)'s gating chain,
[include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h), the out-of-process
ptrace tier ([include/asmtest_ptrace.h](../../../include/asmtest_ptrace.h),
[src/ptrace_backend.c](../../../src/ptrace_backend.c)), and
[mk/native-trace.mk](../../../mk/native-trace.mk) — and the shipped/planned status of
each tier. Narrative docs: [native runtime tracing](../../guides/tracing/native-tracing.md),
[emulator traces](../../guides/tracing/traces.md), [portability](../../reference/portability.md). Roadmaps:
[hardware-trace](../plans/hardware-trace-plan.md),
[AMD LBR](../plans/amd-tracing-plan.md),
[Zen 2 single-step](../plans/zen2-singlestep-trace-plan.md),
[DynamoRIO native-trace](../plans/dynamorio-native-trace-plan.md). Host-specific,
live-verified instantiation (Apple Intel / macOS): [Apple Intel host trace coverage](2026-07-08-apple-intel-host-trace-coverage.md).*

## Summary

asm-test has **one universal trace tier and a fragmented set of native ones**, all
filling the same `asmtest_trace_t` shape (ordered instruction offsets, distinct
basic-block offsets, totals, a truncation bit), so a test swaps backends without
changing how it reads coverage.

- **DynamoRIO** is the only vendor- and microarchitecture-independent *native*
  backend shipping today (Linux x86-64, every Intel + every Zen).
- **Hardware trace splits strictly by vendor/uarch:** Intel → Intel PT; AMD
  Zen 3 / Zen 4 / Zen 5 → AMD LBR (Tier-A 16-branch window + Tier-B stitching;
  live-verified on a Zen 5 Ryzen 9 9950X, plus the Part III software layer — spec/
  wrong-path filtering, stitch decodable-distance guard, CPUID `0x80000022` runtime
  depth, freeze probe, and the eBPF boundary LBR snapshot). AMD Zen 2 has **no
  branch-record facility** (`perf_event_open` returns `EOPNOTSUPP`), but is no
  longer uncovered: the single-step tier, the out-of-process ptrace stepper, and
  the `PTRACE_SINGLEBLOCK` block-step tier (AMD plan P3-2, one `#DB` per taken
  branch on bare metal) all run there.
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
  ([asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h)), and `src/ss_backend.c`
  drives `EFLAGS.TF`→`#DB`/`SIGTRAP` to record every executed `RIP` **live on any
  x86-64 Linux host** (no PMU, no perf, no privilege, no decoder beyond Capstone).
  The **out-of-process `ptrace` variant (W2) now ships for Linux x86-64 and AArch64**
  ([asmtest_ptrace.h](../../../include/asmtest_ptrace.h), `src/ptrace_backend.c`): a
  tracer parent `PTRACE_SINGLESTEP`s a forked tracee and reconstructs the *same*
  exact offsets out of band (the managed-runtime path). The AArch64 arm rides the same
  seam (PC via `PTRACE_GETREGSET`/`NT_PRSTATUS`, `ASMTEST_ARCH_ARM64` length decode);
  its single-step *stream* can **only** be validated on real AArch64 hardware (not under
  qemu-user, which cannot emulate ptrace — the backend self-probes and self-skips there),
  so the stream is **pending real hardware** while the rest is code-implemented and
  build/self-skip-validated; its `/proc`+jitdump readers run on any Linux arch and are
  validated live on AArch64.
  The Phase-5 *cross-OS* fronts have since landed — macOS-Intel in-proc (live-verified:
  `make hwtrace-test` single-steps all 62 insns of a 20-trip loop on an x86-64 macOS host)
  and the Windows x86-64 VEH front (`win64-ss-test`) — so single-step rows below read
  *implemented* on Linux x86-64/AArch64, macOS-Intel, and Windows x86-64; only the AArch64
  *live stream* remains hardware-pending. See the
  [Zen 2 single-step plan](../plans/zen2-singlestep-trace-plan.md).
- **Time-aware code-image recorder (`asmtest_codeimage`).** The byte-source half of
  foreign-JIT tracing: a userspace `PERF_RECORD_TEXT_POKE` that time-versions a process's
  code (cross-process **soft-dirty + `PAGEMAP_SCAN`**, bytes via `process_vm_readv`) so the
  W2 stepper (`asmtest_ptrace_trace_attached_versioned`) decodes against the bytes that
  were live when the method ran — correct under re-JIT/address-reuse, where a single late
  snapshot is wrong. An optional **eBPF emission detector** (CO-RE on
  `mprotect`/`mmap`/`memfd_create`, PID-namespace-filtered) snapshots on the `PROT_EXEC`
  edge; it self-skips without `libbpf`/`CAP_BPF` (the soft-dirty poll is the fallback).
  Implemented + live-validated on Linux x86-64 (`make codeimage-test`, `hwtrace-test`); the
  eBPF lane runs in a `--cap-add=BPF,PERFMON` container (`make docker-hwtrace-codeimage`).
  Pairs with the on-host ptrace stepper today; feeding the timeline into the **Intel PT**
  decoder is the hardware-half forward-look. See the
  [JIT-runtime-tracing analysis](jit-runtime-tracing.md).

---

## Matrix 1 — Backend capability overview

| Backend | Mechanism | Decoder lib | License | Capture overhead | Completeness | Status |
|---|---|---|---|---|---|---|
| **Emulator** | Unicorn virtual CPU (isolated guest) | Capstone (annotate) | (Unicorn) | n/a (interpreted) | exact, unbounded | **implemented** |
| **DynamoRIO** | software DBI, native code cache | none | DR core BSD | low (cached) | exact, unbounded | **implemented** |
| **Intel PT** | continuous branch-trace AUX ring | libipt | BSD | near-zero | exact, unbounded (ring) | **implemented** |
| **AMD LBR** | 16-deep branch stack snapshot (Tier-A) + `sample_period=1` window stitching (Tier-B) | Capstone (replay) | BSD | low (few PMIs) | Tier-A exact ≤16 taken branches; Tier-B stitches past 16 (bounded by ring size + throttling); else `truncated`→fallback | **impl. Part I Ph0–5 + Part III P3-0…P3-5** (spec filter, stitch guard, runtime depth, freeze probe, blockstep tiers, eBPF boundary snapshot); live capture + Tier-B verified on Zen 5 (Ryzen 9 9950X, `amd_lbr_v2`); forward-look: MSR-direct, BRS (Zen 3), IBS (Zen 2) |
| **CoreSight** | ETM/ETE waypoints | OpenCSD | BSD | near-zero | decoder coarser; normalized to match | **reconstruction core host-validated**; live OpenCSD decode awaits a board (self-skips) |
| **Single-step (in-proc TF)** | `EFLAGS.TF` → `#DB`/`SIGTRAP`, in-process | Capstone (block mode) | n/a | ~2.3 µs/insn (Linux) | exact, unbounded | **implemented** (Ph0–4 Linux x86-64 + Ph5 fronts: macOS-Intel in-proc, Windows x86-64 VEH `win64-ss-test`; **x86-64 only** — TF is an x86 flag) |
| **ptrace (out-of-proc step)** | out-of-process tracer: `PTRACE_SINGLESTEP` (per-insn) or `PTRACE_SINGLEBLOCK` block-step (per taken branch) over a forked/attached tracee | Capstone (block mode) | n/a | per-insn kernel round-trip; block-step ≈ one `#DB` per taken branch (cheaper) | exact, unbounded | **implemented** (Linux x86-64; AArch64 code-complete, *live stream* HW-pending — qemu-user can't emulate ptrace, self-skips). Adds the managed-runtime path + code-image `versioned` / whole-`window` capture |

DynamoRIO core is BSD (the tier deliberately avoids `drwrap`'s LGPL-2.1); libipt and
OpenCSD are BSD. Licensing for the optional emulator dependencies (Unicorn,
Keystone) is not asserted here. The **ptrace** tier — like DynamoRIO and the emulator —
is a *separate library* (`asmtest_ptrace_*`, its own `available()`/`skip_reason()`), **not**
a member of the hwtrace `asmtest_trace_backend_t` enum `{INTEL_PT, CORESIGHT, AMD_LBR,
SINGLESTEP}`; it is the out-of-process analog of the in-process single-step backend and the
only backend whose observer lives in a different process from the code under trace.

---

## Matrix 2 — Host OS × architecture support

| Backend | x86-64 Linux | x86-64 macOS | x86-64 Windows | AArch64 Linux | AArch64 macOS |
|---|---|---|---|---|---|
| Emulator (Unicorn) | ✓ | ✓ | ✓ | ✓ | ✓ |
| DynamoRIO | ✓ | ✗ (follow-up) | ✗ | ✗ (follow-up) | ✗ |
| Intel PT | ✓ bare-metal | ✗ | ✗ | — | — |
| AMD LBR | ✓ (Zen 3+) | ✗ | ✗ | — | — |
| CoreSight | — | — | — | scaffold (board) | ✗ |
| Single-step (in-proc TF) | ✓ **shipped** | ✓ **shipped** (Ph5, macOS-Intel) | ✓ Ph5 (VEH, ~6×) | ✗ (TF is x86-64 only) | ✗ |
| ptrace (out-of-proc step) | ✓ **shipped** | ✗ (Linux ptrace only) | ✗ | ✓ code; stream HW-pending | ✗ |

Windows-x64 and macOS are served **only by the emulator tier** today
(`emu_call_win64_traced` for the Win64 ABI). On AArch64 Linux the out-of-process W2
`ptrace` single-step tier is code-implemented and builds/self-skips correctly there
(its `/proc`+jitdump readers validate live), but its single-step *stream* awaits a real
AArch64 host — so for AArch64 the emulator remains the validated trace tier today.
The DynamoRIO native tier is Linux-x86-64-only.

---

## Matrix 3 — x86 native trace × CPU vendor / microarchitecture (Linux x86-64)

| Backend | Intel | AMD Zen 2 (Fam17h) | AMD Zen 3 (Fam19h) | AMD Zen 4 / Zen 5 | Self-skip reason when absent |
|---|---|---|---|---|---|
| DynamoRIO | ✓ | ✓ | ✓ | ✓ | — (vendor-independent DBI) |
| Intel PT | ✓ | ✗ | ✗ | ✗ | `no intel_pt PMU (needs bare-metal Intel; absent on AMD/VM)` |
| AMD LBR | ✗ | ✗ **no facility** | ✓ BRS | ✓ LbrExtV2 **(live-verified, Zen 5)** | `no AMD branch records (needs Zen 3 BRS / Zen 4 LbrExtV2)` |
| Single-step | ✓ | ✓ | ✓ | ✓ | (none — no PMU/perf/privilege needed) |

Zen 2's branch-stack `perf_event_open` returns `EOPNOTSUPP` → `AMD_NOHW`
([hwtrace.c `amd_branch_probe`](../../../src/hwtrace.c)); its legacy LBR is depth-1 and
not wired to perf branch-stack. So on Zen 2 the only exact native options are
**DynamoRIO** and **single-step** (both shipping today). Zen 3 uses **BRS** (Family 19h,
opt-in `branch-brs` event); Zen 4 and **Zen 5** use **LbrExtV2** (mainline Linux 6.1+);
both are a fixed 16-entry stack: Tier A is exact within one window, and Tier-B
stitching of the `sample_period=1` windows reconstructs past it (bounded by the data
ring + PMI throttling); a run the stitched capture cannot hold sets `truncated` to
route the routine to DynamoRIO.

**Live-verified on Zen 5.** The AMD LBR capture+decode path was run on a **Ryzen 9
9950X (Family 0x1A, Zen 5, `amd_lbr_v2`)** — the project's actual dev host
([test_amd_live](../../../examples/test_hwtrace.c), `make docker-hwtrace-amd`). Two
findings from that first real-hardware run:

- It **works**: for a branch-heavy routine a PMU sample fires *inside* the region, so
  LbrExtV2 delivers a full 16-deep in-region branch stack and the decoder
  reconstructs it exactly (loop-body block + ordered offsets, `truncated` once the
  routine exceeds 16 taken branches — Tier A as specified; Tier-B stitching has since
  lifted that >16 truncation, leaving the data ring as the ceiling).
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
| Hardware (PT / AMD) | **process-global single slot** | **one at a time** (begin while active is ignored) | n/a (no per-thread state) |
| Single-step (in-proc) | per-thread (`EFLAGS.TF`) | process-global SIGTRAP arm-refcount | per-thread TF, but runs **serially** — concurrent stepping collides on SIGTRAP |
| ptrace (out-of-proc) | **separate tracer process** over a forked/attached tracee | one tracee per tracer | no in-process signal collision — the observer lives *outside* the traced process (the managed-runtime path) |

---

## Matrix 7 — Recommendation: target → backend

| Target | Primary | Fallback | Rationale |
|---|---|---|---|
| Intel bare-metal, small routine | **Intel PT** | DynamoRIO | complete + near-zero overhead |
| Intel/AMD, long or looping routine | **DynamoRIO** | — | native-speed code cache, no depth ceiling, vendor-independent |
| AMD Zen 3 / Zen 4, small routine | **AMD LBR (Tier A; Tier-B stitches past 16 live)** | DynamoRIO (on `truncated`) | HW-attributed, exact within 16 branches; Tier-B extends reach, bounded by ring + throttling |
| AMD Zen 2 | **DynamoRIO** | single-step | no branch facility exists on Zen 2 |
| Any x86, exact + unprivileged + on CI | **single-step** | DynamoRIO | no PMU/perf/privilege; only depth-unbounded HW-tier path runnable on CI |
| Managed runtime (JVM/.NET/Node), Intel | **Intel PT** | — | observes out-of-band; no thread takeover, no signal collision |
| Managed runtime, AMD | **W2 out-of-proc `ptrace` single-step** *(Linux x86-64 + AArch64; AArch64 stream pending real hardware)* | DynamoRIO (best-effort) | PT is Intel-only; in-proc DR cannot seize JIT/GC threads |
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
  ([hwtrace.c `asmtest_hwtrace_skip_reason`](../../../src/hwtrace.c)). A cascade is just
  these probes tried in priority order, terminating at the emulator — which is always
  available.
- **A dynamic completeness signal.** `asmtest_trace_t.truncated` is set when a
  backend could not record the whole path: AMD LBR stitched-capture overflow (data
  ring full, stitch gap, or sample loss), Intel PT AUX-ring overflow, or single-step
  desync. It is the *runtime*
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
| Dynamic (completeness) | `trace.truncated` == true | after `end()` | AMD LBR run overflowing the stitched capture (data ring / stitch gap) |

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
| Linux AArch64 | CoreSight *(scaffold → self-skips)* → out-of-proc ptrace single-step (W2; code-implemented, stream HW-pending) → emulator |
| macOS Intel | single-step (macOS-Intel Ph5 — **shipped**, live-verified via `make hwtrace-test`) → emulator |
| macOS Apple Silicon | **emulator only** |
| Windows x64 | single-step (VEH Ph5 — **shipped**, `win64-ss-test`) → emulator |

The macOS-Intel and Windows-x64 rows now resolve to their **single-step** front before
the emulator floor (Phase-5, shipped). The remaining non-Linux-x86-64 rows (macOS Apple
Silicon, the AArch64 live stream, CoreSight) still collapse to the **emulator** in
practice, because the native tiers above them are either hardware-pending or scaffolds.
The Win64 ABI is also traced by the emulator via `emu_call_win64_traced`.

### Matrix 10 — Language-runtime fallback chain (within a host that has native tiers)

| Runtime class | Resolution order |
|---|---|
| Native / compiled (C, C++, Rust, Go, Zig, Ruby, Lua) | in-process DynamoRIO (+ HW trace where the hardware row allows) → emulator |
| CPython (GIL-serialized, no JIT) | in-process DynamoRIO *(supported managed target)* → emulator |
| JVM | DynamoRIO *(best-effort, guarded)* → out-of-band Intel PT / W2 ptrace *(shipped, Linux x86-64)* → emulator |
| Node / .NET | out-of-band Intel PT (Intel) / W2 ptrace single-step *(shipped, Linux x86-64)* → emulator *(in-process DynamoRIO self-skips: cannot take over JIT/GC threads)* |

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
            chain += [PTRACE_SINGLESTEP_W2]          # Linux x86-64 shipped; out-of-band
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
| Linux x86-64 | AMD Zen 3 | .NET | **W2 ptrace** *(out-of-band, shipped)* → emulator *(no PT on AMD; DR self-skips)* |
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
([hwtrace.c](../../../src/hwtrace.c), [asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h)).
The `policy` encodes the static *and* the dynamic fallback: `ASMTEST_HWTRACE_BEST`
picks the most faithful backend; `ASMTEST_HWTRACE_CEILING_FREE` drops the one
ceiling-bounded backend (AMD LBR — Tier-B stitching decodes past the 16-deep stack,
but capture stays ring-bounded) and is what a caller re-resolves
under after a trace comes back `truncated`. On any x86-64 Linux host the cascade is
non-empty (single-step is the floor), so it never fails to resolve. This is exactly
the within-hardware-tier portion of Matrix 8.

**The *cross-tier* front-end now ships too.** `asmtest_hwtrace_resolve` stops at the
hardware tier's library boundary; the DynamoRIO tier (`libasmtest_drapp`, located by
`DYNAMORIO_HOME`) and the emulator tier (`libasmtest_emu`) are separate libraries
with their own call APIs. `asmtest_trace_resolve(policy, out, cap)` /
`asmtest_trace_auto(policy, &choice)`
([asmtest_trace_auto.h](../../../include/asmtest_trace_auto.h),
[src/trace_auto.c](../../../src/trace_auto.c)) are the front-end **over all three** —
they walk the full descending-fidelity cascade of Matrix 8 (Intel PT → AMD LBR →
DynamoRIO → single-step → CoreSight → emulator, DynamoRIO ranking *above* single-step
because its code cache runs at native speed while single-step pays a per-instruction
kernel round-trip) and return `asmtest_trace_choice_t` descriptors `{tier, backend,
fidelity}`. It calls `asmtest_hwtrace_available()` directly and **dlopen-probes**
`libasmtest_drapp` (via `$ASMTEST_DRAPP_LIB`) for the DynamoRIO tier, so it
hard-links neither the DynamoRIO nor the emulator library — keeping the three
decoupled. Shipped in `libasmtest_hwtrace` and exposed through **every language
wrapper** (Python `resolve_tiers`/`auto_tier`, and the idiomatic spelling in each
other binding).

The `policy` bitmask encodes the three controls the cascade needs:
`ASMTEST_TRACE_BEST` (most-faithful available, emulator floor allowed),
`ASMTEST_TRACE_CEILING_FREE` (drop the one fixed-window backend, AMD LBR — the policy
to re-resolve under after a trace comes back `truncated`), and — crucially —
`ASMTEST_TRACE_NATIVE_ONLY`, **the flag that forbids the native→emulator crossing**.
That is the one boundary the fallback table above says must never be crossed
silently: under `NATIVE_ONLY` the emulator floor is dropped, so on a host with no
native tier (e.g. macOS arm64) the cascade resolves to *nothing*
(`ASMTEST_HW_EUNAVAIL`) rather than silently downgrading real-CPU execution to an
isolated guest. So the native→emulator fall stays an explicit, fidelity-aware
integrator decision (consistent with the
[AMD LBR plan, Phase 4](../plans/amd-tracing-plan.md) overflow→DynamoRIO routing
rule) — now expressed as a policy flag rather than left to each caller to hand-roll.

---

## Packaging & target-architecture coverage

There is a fourth axis the OS/hardware/language matrices do not capture: **what an
installed package actually ships.** For a long time the answer collapsed the fallback
cascade to a single rung — only the emulator tier was bundled, so a `pip install` /
`npm install` / `gem install` consumer had exactly **one** trace tier out of the box on
every platform. **That changed with the [bundle-native-trace-tiers
work](../archive/plans/bundle-native-trace-tiers-plan.md):** the two native tiers now ship
*inside* the packages on the **Linux** slots, so a fresh install runs
`NativeTrace` / `HwTrace` on a capable Linux host with no manual `make shared-*` and no
`DYNAMORIO_HOME`. The macOS slots still carry the emulator only — both native tiers are
Linux-only and self-skip there — so the answer is now **platform-dependent**, not a flat
"emulator everywhere".

### What each package carries

Per [packaging.md](../../reference/packaging.md), bindings reach the framework two ways and that
decides what ships:

- **dlopen bindings** (Python, Ruby, Lua, Node, Java, .NET) bundle the prebuilt
  **`libasmtest_emu`** — the superset: capture trampoline + opaque-handle FFI +
  **emulator** + Keystone assembler + Capstone disassembler, with Unicorn/Keystone/
  Capstone vendored and rpath-rewritten so the package is self-contained.
- **link / source bindings** (Rust, Zig, C++, Go) ship source and build against
  `libasmtest` / `libasmtest_emu` on the consumer's host.

Crucially, **both native trace tiers now ship in the six dlopen packages on the Linux
slots.** `make package-libs` stages `libasmtest_hwtrace` into **every** Linux slot (its
single-step + out-of-process ptrace paths work on any Linux host; the Intel PT / AMD
LBR / CoreSight decoders compile in but self-skip off the hardware they need) and — on
`linux-x86_64` only — the DynamoRIO tier (`libasmtest_drapp` + `libasmtest_drclient` +
the pinned `libdynamorio`, auto-fetched by
[`scripts/fetch-dynamorio.sh`](../../../scripts/fetch-dynamorio.sh)). Because the
Ruby/Node/Java/Lua/.NET packers copy the slot by `lib*.so*` glob they pick the libs up
for free; the Python wheel stages them explicitly and `auditwheel` leaves them alone —
they are already self-contained (`$ORIGIN` rpath, `libdynamorio` co-located next to
`drapp` and located at run time via `dladdr`). Each binding's `drtrace`/`hwtrace` loader
resolves the lib from the **bundled slot** first (env override → bundled → dev `build/`
→ system) and exposes a `library_path()` self-report, so `NativeTrace.available()` /
`HwTrace.available()` now returns true out of the box on a capable Linux host — no
separate build, no `DYNAMORIO_HOME`. The **source bindings** (Rust/Zig/C++/Go) carry no
payload, so their consumers still build `make shared-drtrace` / `shared-hwtrace` and
point `$ASMTEST_DRAPP_LIB` / `$ASMTEST_HWTRACE_LIB` themselves — the same clean
self-skip when a tier is absent.

### Matrix 12 — Trace tier × packaging

| Trace tier | Carrying library | In dlopen packages? | In source packages? | Otherwise obtained by |
|---|---|---|---|---|
| **Emulator** | `libasmtest_emu` (superset) | ✅ bundled, every platform slot | built from source | — (always present) |
| **DynamoRIO** | `libasmtest_drapp` + `libasmtest_drclient` + `libdynamorio` | ✅ bundled, `linux-x86_64` slot | ❌ (build it) | `make shared-drtrace drtrace-client` (source bindings / macOS; other slots self-skip) |
| **Hardware** (single-step + ptrace always; PT / AMD LBR / CoreSight decoders self-skip) | `libasmtest_hwtrace` | ✅ bundled, every Linux slot | ❌ (build it) | `make shared-hwtrace` (source bindings / macOS; decoders still need bare metal + perf) |

The two concerns that once kept the native tiers *out* are now handled at package-build
time rather than pushed onto the consumer: DynamoRIO has **no pkg-config** (it is located
by `DYNAMORIO_HOME`), so `fetch-dynamorio.sh` pins and vendors `libdynamorio` into the
slot with an `$ORIGIN` rpath and `dladdr` sibling-resolution; and the hardware tier's
bare-metal / `perf_event_paranoid` needs are a **run-time** gate, not a packaging one —
the lib ships on every Linux slot and each decoder self-skips where its hardware or
privilege is absent (single-step + ptrace always work). Licensing is unchanged in
character: DynamoRIO core (BSD-3), libipt (BSD), OpenCSD (BSD-3) and — in the full
hwtrace lane — libbpf (conveyed under its BSD-2 option) are all **permissive**, so the
package stays effectively **GPL-2.0** on the strength of the already-bundled
Unicorn/Keystone alone (Capstone + asm-test BSD/MIT); `collect-licenses.sh` emits each
note only when the lib is actually staged.

### Matrix 13 — Packaging platform slots × trace tiers reachable

The CI `payloads` matrix stages four native slots (`<os>-<arch>`); there is **no
Windows package slot** (the Win64 tier is cross-compile + Wine, not a published
payload).

| Platform slot | Built by (CI runner) | Emulator (packaged) | DynamoRIO (packaged) | Hardware (packaged) |
|---|---|---|---|---|
| `linux-x86_64` | ubuntu-latest | ✅ | ✅ bundled | ✅ bundled (single-step/ptrace live; PT/AMD/CoreSight decoders self-skip off their hw) |
| `linux-aarch64` | ubuntu-24.04-arm | ✅ | ✗ (DR is x86-64 only) | ✅ bundled (single-step/ptrace; stream HW-pending, CoreSight scaffold) |
| `darwin-arm64` | macos-latest | ✅ | ✗ (Linux-only → self-skips) | ✗ (Linux-only → self-skips) |
| `darwin-x86_64` | macos-13 (nightly) | ✅ | ✗ (Linux-only → self-skips) | ✗ (Linux-only → self-skips) |
| `windows-x64` | — (no slot) | source / Win64 tier only | ✗ | ✗ |

Per-ecosystem the slot is named conventionally — Java `native/<os>-<arch>/`, .NET
RIDs (`osx-arm64`, `linux-x64`, …), Python per-platform wheel tags repaired by
`auditwheel`/`delocate`. The **emulator guest ISAs (Matrix 4) are independent of the
slot**: a `linux-x86_64` package still traces AArch64/RISC-V/ARM32 *guests*, because
the guest runs inside Unicorn, not on the host CPU.

### Packaging as the fourth fallback axis

Composing this with Matrix 8–11: an **installed package's effective cascade now depends
on the slot.** On the **Linux slots** the native tiers are bundled, so the cascade is
multi-rung out of the box — `linux-x86_64` resolves DynamoRIO → single-step → emulator
(plus Intel PT / AMD LBR where the hardware and `perf_event_paranoid` allow), and
`linux-aarch64` resolves the out-of-process ptrace single-step tier → emulator (its
stream still HW-pending). On the **macOS slots** the native tiers are Linux-only and
self-skip, so the cascade there is still the single emulator rung. So the "reachability"
surfaces now stack up differently by slot:

| Surface | What it can trace |
|---|---|
| **Platform capability** (Matrix 2–3) | every native tier the OS/CPU physically supports |
| **Source checkout** (`make shared-drtrace` / `shared-hwtrace`) | the native tiers, on `linux-x86_64`, with the toolchain installed |
| **Installed package, Linux slot** (`pip`/`npm`/`gem`/…) | emulator **+ the bundled native tiers** — hwtrace on any Linux, DynamoRIO on `linux-x86_64` |
| **Installed package, macOS slot** | **emulator only** (native tiers are Linux-only → self-skip) |

The practical consequence has **flipped for Linux**: native-trace fidelity is no longer a
build-from-source decision on a Linux host — a plain `pip`/`npm`/`gem` install now
resolves DynamoRIO (on `linux-x86_64`) and the single-step/ptrace hardware tier (on any
Linux) with no extra steps. It remains a build-from-source decision for the **source
bindings** (Rust/Zig/C++/Go, which ship no payload) and on **macOS** (where both native
tiers are Linux-only). The env overrides
(`ASMTEST_DRAPP_LIB`/`ASMTEST_DRCLIENT`/`ASMTEST_DR_LIB`/`ASMTEST_HWTRACE_LIB`) still
take precedence, for an advanced user pointing at a hand-built lib.

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
And packaging no longer tightens the floor the way it once did: the native tiers now
ship in the Linux package slots (hwtrace on every Linux slot, DynamoRIO on
`linux-x86_64`), so an out-of-the-box install resolves the native cascade on a capable
Linux host — while the macOS slots, and the source-only bindings (Rust/Zig/C++/Go),
still start and end on the emulator unless a native tier is built from source.
