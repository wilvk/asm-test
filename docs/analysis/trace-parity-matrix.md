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
  Zen 3 / Zen 4 → AMD LBR (16-taken-branch cap); AMD Zen 2 → nothing yet (its
  branch-stack `perf_event_open` returns `EOPNOTSUPP`).
- The **emulator (Unicorn)** tier is the universal floor for every OS, architecture,
  and language the native tiers do not reach (Windows-x64, macOS, AArch64, and the
  managed runtimes in-process DynamoRIO cannot take over).

Two facts worth stating up front because they are easy to get wrong:

- **Java traces *live*** on the DynamoRIO tier (the verified-live set is
  cpp/ruby/java/lua/zig/rust/go), but its make target is wrapped in the "Failed to
  take over all threads" → SKIP downgrade because the JVM is intermittent —
  distinct from Node/.NET, which **always** self-skip.
- The **single-step (Trap Flag) backend is design-of-record only** — it is *not* in
  the source. `asmtest_trace_backend_t` is `{INTEL_PT, CORESIGHT, AMD_LBR}` today
  ([asmtest_hwtrace.h](../../include/asmtest_hwtrace.h)); rows marked *(planned)*
  below track the [Zen 2 single-step plan](../plans/zen2-singlestep-trace-plan.md).

---

## Matrix 1 — Backend capability overview

| Backend | Mechanism | Decoder lib | License | Capture overhead | Completeness | Status |
|---|---|---|---|---|---|---|
| **Emulator** | Unicorn virtual CPU (isolated guest) | Capstone (annotate) | (Unicorn) | n/a (interpreted) | exact, unbounded | **implemented** |
| **DynamoRIO** | software DBI, native code cache | none | DR core BSD | low (cached) | exact, unbounded | **implemented** |
| **Intel PT** | continuous branch-trace AUX ring | libipt | BSD | near-zero | exact, unbounded (ring) | **implemented** |
| **AMD LBR** | 16-deep branch stack snapshot | Capstone (replay) | BSD | low (few PMIs) | exact ≤16 taken branches; else `truncated`→fallback | **impl. Ph0–4**; live capture unverified on dev (Zen 2) |
| **CoreSight** | ETM/ETE waypoints | OpenCSD | BSD | near-zero | decoder coarser; normalized to match | **scaffold** — always self-skips |
| **Single-step** | `EFLAGS.TF` → `#DB`/`SIGTRAP` | Capstone (block mode) | n/a | ~2.3 µs/insn (Linux) | exact, unbounded | **planned** (not in code) |

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
| Single-step *(planned)* | ✓ | macOS-Intel (Ph5) | ✗ → Ph5 (VEH, ~6×) | ptrace-only (W2) | ptrace-only (W2) |

Windows-x64 and all of macOS/AArch64 are served **only by the emulator tier** today
(`emu_call_win64_traced` for the Win64 ABI). The two implemented native tiers are
Linux-x86-64-only.

---

## Matrix 3 — x86 native trace × CPU vendor / microarchitecture (Linux x86-64)

| Backend | Intel | AMD Zen 2 (Fam17h) | AMD Zen 3 (Fam19h) | AMD Zen 4 | Self-skip reason when absent |
|---|---|---|---|---|---|
| DynamoRIO | ✓ | ✓ | ✓ | ✓ | — (vendor-independent DBI) |
| Intel PT | ✓ | ✗ | ✗ | ✗ | `no intel_pt PMU (needs bare-metal Intel; absent on AMD/VM)` |
| AMD LBR | ✗ | ✗ **no facility** | ✓ BRS | ✓ LbrExtV2 | `no AMD branch records (needs Zen 3 BRS / Zen 4 LbrExtV2)` |
| Single-step *(planned)* | ✓ | ✓ | ✓ | ✓ | (none — no PMU/perf/privilege needed) |

Zen 2's branch-stack `perf_event_open` returns `EOPNOTSUPP` → `AMD_NOHW`
([hwtrace.c `amd_branch_probe`](../../src/hwtrace.c)); its legacy LBR is depth-1 and
not wired to perf branch-stack. So on Zen 2 the only exact native options are
**DynamoRIO** (today) or **single-step** (planned). Zen 3 uses **BRS** (Family 19h,
opt-in `branch-brs` event); Zen 4 uses **LbrExtV2** (mainline Linux 6.1+); both are
a fixed 16-entry stack, so the AMD backend is exact only within a 16-taken-branch
window (Tier A) and sets `truncated` to route longer routines to DynamoRIO.

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
| AMD Zen 2 | **DynamoRIO** | single-step *(planned)* | no branch facility exists on Zen 2 |
| Any x86, exact + unprivileged + on CI | **single-step** *(planned)* | DynamoRIO | no PMU/perf/privilege; only depth-unbounded HW-tier path runnable on CI |
| Managed runtime (JVM/.NET/Node), Intel | **Intel PT** | — | observes out-of-band; no thread takeover, no signal collision |
| Managed runtime, AMD | **W2 out-of-proc `ptrace` single-step** *(planned)* | DynamoRIO (best-effort) | PT is Intel-only; in-proc DR cannot seize JIT/GC threads |
| Windows-x64 / macOS / AArch64 (any) | **Emulator (Unicorn)** | — | only tier covering these hosts today; CoreSight is a scaffold |
| Cross-ISA / isolation / faults-as-data | **Emulator (Unicorn)** | — | the only tier with isolated guests + 5 ISAs + precise faults |

The managed-runtime-on-AMD path is **W2 (out-of-process `ptrace` single-step)**, not
W3 (`DEBUGCTL.BTF` branch-granular step, which is ring-0-blocked in portable form).
See the [Zen 2 single-step plan, Phase 5](../plans/zen2-singlestep-trace-plan.md).

---

## One-line synthesis

DynamoRIO is the only vendor- and uarch-independent native backend shipping today
(Linux x86-64, every Intel + Zen); hardware trace splits strictly
**Intel → PT / Zen 3-4 → LBR (16-cap) / Zen 2 → nothing-yet**; the emulator is the
universal floor for every OS, architecture, and language the native tiers do not
reach.
