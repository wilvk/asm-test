# Analysis: improving AMD hardware tracing

*Status: analysis / findings. This document is a consolidated, cross-checked
answer to a single question — **how can asm-test's AMD hardware-trace path be
improved?** — derived from the source of record ([src/amd_backend.c](../../src/amd_backend.c),
[src/hwtrace.c](../../src/hwtrace.c)'s AMD gating/capture chain,
[include/asmtest_hwtrace.h](../../include/asmtest_hwtrace.h),
[src/ptrace_backend.c](../../src/ptrace_backend.c)) cross-referenced against the
existing roadmaps ([AMD LBR plan](../plans/amd-lbr-trace-plan.md),
[Zen 2 single-step plan](../plans/zen2-singlestep-trace-plan.md),
[hardware-trace plan](../plans/hardware-trace-plan.md)) and against external
primary sources (AMD APM Vol 2 / PPR, the Linux `arch/x86/events/amd/` and
`arch/x86/kernel/step.c` sources, LWN, and the perf man pages). Companion docs:
[trace-parity-matrix](trace-parity-matrix.md) (what works where today) and
[jit-runtime-tracing](jit-runtime-tracing.md) (the foreign-JIT forward-look). This
is analysis / findings, not shipped behaviour; where a claim was adversarially
verified against a primary source the verdict is stated inline.*

> **Provenance.** Every "capability is real" statement below was checked against a
> named primary source (kernel source file, AMD APM/PPR section, or LWN write-up).
> Verdict tags: **[confirmed]** the mechanism and its benefit hold; **[real,
> qualified]** the capability is real but the naive framing overstates it (the
> qualification is folded into the text); **[dead end]** refuted against a primary
> source. Generation/kernel gates are stated because several facilities that look
> uniform across "Zen 2/3/4/5" are in fact Zen-4-and-later or a specific kernel
> version.

---

## Summary

The governing fact, **confirmed** and worth stating first because it bounds every
option: **AMD ships no Intel-PT / Arm-CoreSight-ETM equivalent** — no continuous,
packetized, AUX-delivered control-flow trace on any Zen part through Zen 5
(mid-2026), and none on the public Zen 6 / Zen 7 roadmap. There is no BTS-to-memory
either (GDB removed `record-btrace bts` on AMD after confirming AMD's `DebugCtl`
drives only the four legacy LBR MSRs). The 16-entry branch stack
([`AMD_LBR_DEPTH`](../../src/amd_backend.c)) is a **silicon ceiling**, not a
software choice — CPUID `0x80000022` EBX reports 16 on every shipping Zen 4/5 part,
and the field is only *read*, never configured.

So "improving AMD hardware tracing" is **not** the search for a hidden continuous
facility — that is a verified dead end. It is two concrete programmes:

1. **Squeeze the sampled 16-entry window harder and more correctly** — capture it
   deterministically at the region boundary instead of by `sample_period=1`
   PMI-flood, and stop trusting a window that hardware never froze.
2. **Add a cheaper *complete-flow* fallback than per-instruction single-step** — one
   that also covers Zen 2 (which has no branch hardware at all) and runs rootless.

Both are buildable today. One of them — the BTF block-step tier — also **corrects a
factual error in the repo's own** [Zen 2 single-step plan](../plans/zen2-singlestep-trace-plan.md).

---

## The headline correction: BTF block-step is available on x86

[zen2-singlestep-trace-plan.md](../plans/zen2-singlestep-trace-plan.md) files the W3
"DEBUGCTL.BTF branch-granular step" idea under *research-only*, on the stated
grounds that *"`PTRACE_SINGLEBLOCK` is wired only on PowerPC/s390 … unwired on
x86."*

That premise is **incorrect [confirmed]**. Linux implements BTF block-step
generically in [`arch/x86/kernel/step.c`](https://github.com/torvalds/linux/blob/master/arch/x86/kernel/step.c):
`set_task_blockstep()` sets `DEBUGCTLMSR_BTF` + `TIF_BLOCKSTEP`, and `enable_step()`
sets `EFLAGS.TF` — the exact `BTF=1 && TF=1` pairing AMD APM Vol 2 §13.2 requires.
It is reached from userspace via `user_enable_block_step()` → **`PTRACE_SINGLEBLOCK`,
which *is* supported on mainline x86**. There is no Intel-vs-AMD vendor check, and
BTF has been baseline AMD64 since APM rev 3.07 (2002), so it is present on Zen
2/3/4/5. `PTRACE_SINGLEBLOCK` is merely *undocumented* in the `ptrace(2)` man page —
undocumented ≠ unwired. It needs only ptrace of one's own child: **no `CAP_PERFMON`,
works under any `perf_event_paranoid`.**

This unlocks a **block-step tier**: one trap-class `#DB` per **taken branch** (≈ per
basic block) rather than one per instruction. For asm-test's compute kernels that is
typically a **4–10× reduction in stops** versus the current per-instruction
single-step tier, while producing an identically-shaped, exactly-ordered,
ceiling-free `asmtest_trace_t` — and it is the **only exact real-CPU option on Zen
2**, where no branch-record hardware exists. It is a modest extension of the
already-shipping `PTRACE_SINGLESTEP` machinery in
[src/ptrace_backend.c](../../src/ptrace_backend.c).

**Qualifications to build in [real, qualified]:**

- The `#DB` also fires on **interrupts and exceptions**, not only program branches
  (APM §13.2), so trap count ≥ taken-branch count and is load-dependent; the
  reconstructor must discard interrupt/exception stops.
- `#DB` is **trap-class**: the stop `RIP` is the branch **target**, not the source.
  The source is recovered by disassembling the fall-through range up to the
  terminating branch (asm-test already has the region offsets + Capstone).
- BTF (and TF) **auto-clear on each `#DB`** — re-arm every step.
- It is *complete-at-moderate-overhead*, **not** "cheap": each block still costs a
  full ptrace tracer round-trip (~4 context switches), orders of magnitude above
  DynamoRIO's in-code-cache basic-block instrumentation, and it perturbs timing
  heavily. Position it in the cascade as the **rootless / Zen-2 / managed-runtime
  completeness fallback**, above per-instruction single-step and below DynamoRIO —
  not as a low-overhead tier.

This is the correct, hardware-clean version of the "block-cache single-step"
(INT3-on-terminators) idea, which the verification pass knocked down: software INT3
patching costs ~2 traps per block (restore + step-over + re-poke), fights W^X, and
is silently clobbered by self-modifying / JIT code — all problems BTF avoids by
letting the CPU do the block-stepping.

---

## Matrix 1 — Prioritized improvements

| # | Improvement | What it fixes | Gen / kernel gate | Verdict | Effort |
|---|---|---|---|---|---|
| **P0** | **BTF block-step tier** (`PTRACE_SINGLEBLOCK`) | Complete ordered flow at ~1 trap/branch; only exact real-CPU option on **Zen 2**; rootless (no `CAP_PERFMON`) | all Zen; any Linux x86-64 | real (corrects the plan) | Medium |
| **P0** | **Software-event / eBPF on-demand LBR snapshot** (`bpf_get_branch_snapshot` / `amd_pmu_v2_snapshot_branch_stack`) | Kills both live Zen-5 failure modes: tiny-single-shot "too fast to sample," and post-glue window contamination that forced the "richest-in-region" heuristic; HW-attributed managed-runtime lane | **Zen 4/5**, Linux **≥6.10**, `CAP_PERFMON`/`CAP_BPF` | real, gated | Medium |
| **P0** | **Probe `X86_FEATURE_AMD_LBR_PMC_FREEZE`** (CPUID `0x80000022` EAX[2]) and gate window-trust on it | Silent-correctness bug: freeze-on-PMI is **not** universal on Zen 4; without it the 16-entry window drifts past the overflow point and may not reach region exit | Zen 4/5 | confirmed gap | Small |
| **P1** | **BRS period-adjust single-window capture** (fixed period ≈ N−16) | Replaces `sample_period=1` — the dominant Tier-B throttle / ring-overflow truncation cause — with **one** frozen overflow for ≤16-branch routines | **Zen 3 BRS**, Linux ≥5.19 | SUPPORTED | Small–Med |
| **P1** | **Consume LbrExtV2 `spec`/`valid` bits** before replay/stitch | [amd_backend.c](../../src/amd_backend.c) filters only `abort` and *notes* it ignores spec flags — drop `PERF_BR_SPEC_WRONG_PATH` phantom edges | Zen 4/5, Linux ≥6.1 | real, modest gain | Small |
| **P1** | **Harden Tier-B throttle/ring config** (larger data ring; raise `kernel.perf_event_max_sample_rate`, set `kernel.perf_cpu_time_max_percent=0` on the runner) | Extends stitch reach before the kernel drops the newest samples — zero fidelity change | Zen 3/4/5 | operational | Small |
| **P1** | **Add a decodable-distance invariant to the stitcher** | Closes the open question in amd_backend.c: the smallest-overlap heuristic can silently *mis-stitch* a self-overlapping loop; cross-check reconstructed insn count == static decode distance, else honest gap | Zen 3/4/5 | closes a silent-wrong risk | Small–Med |
| **P2** | **IBS-Op complementary coverage lane** (esp. Zen 2) | Only HW branch source on Zen 2 (precise-IP source→target via `MSR_AMD64_IBSBRTARGET`, gated on `IBS_CAPS_BRNTRGT`); coverage-confirmer to shrink block-step/DR residual | all Zen, `CAP_PERFMON` | statistical, not ordered | Medium |
| **P2** | **Runtime depth from CPUID `0x80000022` EBX** instead of `#define AMD_LBR_DEPTH 16` | Future-proofing hygiene (a no-op today — every shipping part reports 16) | Zen 4/5 | zero downside, low value | Tiny |

### The three to build first

**1 — BTF block-step (P0).** Highest impact-to-effort, and the only path that gives
Zen 2 and rootless CI a complete real-CPU trace cheaper than per-instruction
stepping. See the correction above. Slots into the cascade as
`AMD_LBR (16-cap) → software-event snapshot → BTF block-step → DynamoRIO → per-insn single-step`.

**2 — Software-event LBR snapshot (P0).** The one item that *fixes documented live
failures*. The [AMD LBR plan](../plans/amd-lbr-trace-plan.md)'s own Zen-5 run
recorded two: a tiny routine truncates because perf only delivers the stack at a PMU
sample (never fires in-region), and a capture bug where post-routine glue evicted the
routine's branches from the 16-deep window — forcing the fragile "keep the
richest-in-region sample" heuristic in [`hwtrace_end_amd`](../../src/hwtrace.c). A
`uprobe`/`fentry` BPF program at region **entry/exit** that calls
`bpf_get_branch_snapshot()` reads the frozen 16-entry stack **deterministically at
the boundary** — no `sample_period=1`, no throttle exposure, no richest-window
guessing, and the snapshot is taken *before* the glue runs. It is also a
HW-attributed managed-runtime lane on AMD, where the plan currently routes Node/.NET
straight to W2 ptrace single-step (Intel PT being unavailable). Merged upstream 2024
(`amd_pmu_v2_snapshot_branch_stack`, wired into `perf_snapshot_branch_stack`); the
kernel already inlines the freeze path (`__amd_pmu_lbr_disable`) so instrumentation
does not evict real entries. **Gates:** Zen 4/5 (perfmon v2) only — Zen 3 BRS and
Zen 2 do not go through `amd_pmu_v2_handle_irq`; Linux ≥6.10 (a 6.6.y backport was
still being requested in Jan 2026); `CAP_PERFMON`/`CAP_BPF`.

**3 — Freeze-availability probe (P0).** Smallest change, real correctness. The 2024
kernel fix made `DEBUGCTLMSR_FREEZE_LBRS_ON_PMI` conditional on
`X86_FEATURE_AMD_LBR_PMC_FREEZE` (CPUID `0x80000022` EAX[2]) precisely because *"this
may not be the case for all Zen 4 processors."* Without freeze the recorded stack
keeps advancing after the overflow transitions to CPL0, so a PMI window can silently
**not** end at region exit — yet the current AMD capture path trusts it. Probe the
bit at init; where freeze is absent, prefer the software-event snapshot (path #2,
which stops LBR in software) and do not assume a PMI window reaches the region exit.

---

## Matrix 2 — Squeezing the existing window (P1 detail)

These keep asm-test's PMU-window architecture — which the research **confirms is at
the AMD hardware ceiling** — but remove its sharpest edges.

| Lever | Mechanism | Why it helps | Caveat |
|---|---|---|---|
| **BRS period-adjust** (Zen 3) | Fixed `sample_period ≈ N−16` (min 17); the kernel already programs `period − lbr_nr` (`amd_brs_adjust_period`) and BRS freezes/holds the NMI until the 16-branch buffer saturates | **One** PMI delivers the complete ≤16 window at region exit, versus `sample_period=1`'s one-PMI-per-branch flood that trips `perf_event_max_sample_rate` throttling and the non-overwrite ring | Zen 3 BRS **only** (forward-capture, fixed mode, period > 16). On Zen 4/5 the better lever is the software-event snapshot (P0 #2), not this |
| **`spec`/`valid` filtering** (Zen 4/5) | `perf_branch_entry.spec` carries `PERF_BR_SPEC_WRONG_PATH`; the LbrExtV2 driver passes wrong-path entries through to userspace | Drops speculative/wrong-path phantom edges before `amd_replay`, which today filters only `abort` and *explicitly notes* (amd_backend.c comment) that it ignores every other flag | Wrong-path entries are relatively uncommon, so this is a precision refinement, not a step-change. LbrExtV2/Linux ≥6.1 only — a no-op on Zen 3 BRS (retired-only, no spec bits) |
| **Throttle/ring hardening** | Larger `data_size` ring; `sysctl kernel.perf_event_max_sample_rate` up, `kernel.perf_cpu_time_max_percent=0` on the self-hosted runner | The Tier-B live path is bounded by ring size + throttling (a 20 000-trip loop already truncates); more headroom = longer gapless stitch before honest truncation | Operational, self-hosted-runner only; extends reach, does not remove the ceiling |
| **Decodable-distance stitch check** | For each stitched boundary, assert reconstructed instruction count between consecutive branch targets == statically-decoded byte distance; reject a wrong minimal-shift match, else emit an honest gap | AMD sets `hw_idx ≡ 0` (register renaming keeps From[0]=TOS), so Intel's exact index-based overlap count **cannot** be ported — the current smallest-overlap heuristic can silently mis-stitch a self-overlapping loop (an open question in amd_backend.c). This is the AMD-available substitute check | Improves correctness for the common looping case; does not extend depth |

---

## Matrix 3 — Confirmed dead ends (do not invest)

All refuted against a named primary source. Stop spending fallback complexity here.

| Idea | Verdict | Why |
|---|---|---|
| AMD PT / BTS-to-memory / CoreSight-ETM equivalent | **dead end** | None exists on any Zen; none announced. AMD `DebugCtl` drives only the 4 legacy LBR MSRs; GDB disabled `record-btrace bts` on AMD |
| Port Intel's `--stitch-lbr` | **dead end** | It is a *call-stack* technique keyed on `PERF_SAMPLE_BRANCH_HW_INDEX`; AMD's `hw_idx ≡ 0` gives no wrap counter. asm-test's edge-matching stitch is already the ceiling |
| Two concurrent hardware-filtered branch-stack events | **dead end** | One shared `LBR_SELECT` per core; the second event silently reprograms the filter. Time-multiplexing **halves** coverage, not doubles it |
| `>16` branch-stack depth | **dead end** | Silicon ceiling on all shipping parts (CPUID reports 16). Would need hypothetical future LbrExtV3 |
| Callstack-LBR on AMD | **dead end** | `PERF_SAMPLE_BRANCH_CALL_STACK` → `LBR_NOT_SUPP` → `-EOPNOTSUPP` |
| `precise_ip` on the branch-stack event | **dead end** | On AMD `precise_ip` redirects to the IBS PMU (statistical single-op), not the branch stack |
| IBS as an *ordered* trace / IBS-fed-BOLT CFG | **dead end** (for ordered flow) | IBS is one tagged op per NMI (worse edge-yield than LBR); production BOLT/Propeller feed on branch *stacks*, not IBS. Useful only as sparse coverage (P2) |
| AMD "Smart Trace Buffer" (STB) | **dead end** | An SoC power-management / firmware-failure debug buffer (`amd_pmc` driver, DebugFS `stb_read`), not instruction flow |
| rr-style record/replay | **dead end** (for this model) | Needs a root MSR `SpecLockMap` workaround + whole-process replay; incompatible with the per-region, unprivileged contract |

---

## Notes on IBS (why it is P2, not P0)

IBS-Op is worth exactly one thing: it is the **only hardware branch source on Zen
2**, and it carries a real precise-IP source→target edge (`IbsOpRip` →
`MSR_AMD64_IBSBRTARGET`, with `op_brn_ret`/`op_brn_taken`/`op_brn_misp` bits) —
**capability-gated** on `IBS_CAPS_BRNTRGT` (CPUID `Fn8000_001B` EAX[5], present on
all Zen but must be probed). But it is **statistical**: one tagged micro-op per
counter period, so it yields a sparse, probabilistic edge set — never an ordered,
complete path — and its per-NMI edge yield is *lower* than LBR's ~16 records per
interrupt. It also requires `CAP_SYS_ADMIN`/`CAP_PERFMON` (IBS PMUs have no
user/kernel filter). So the honest role is a **coverage-confirmer / hot-edge
pre-cover** that shrinks (does not bound) the block-step / DynamoRIO residual, or an
indirect-branch-target resolver — not a replacement for the branch stack. (Two raw
research proposals here were themselves wrong and were caught in verification:
`rand_en` is an IBS-*Fetch* knob, not Op; and IBS-Op does carry a branch target, but
only capability-gated and only for retired taken branches.)

---

## Documentation corrections this analysis implies

- [zen2-singlestep-trace-plan.md](../plans/zen2-singlestep-trace-plan.md): the
  `PTRACE_SINGLEBLOCK`-unwired-on-x86 claim is wrong (see the headline correction);
  it is what currently strands BTF block-step in "research only."
- [native-tracing.md](../native-tracing.md) / [hardware-tracing.md](../hardware-tracing.md):
  AMD LBR is presented as *finished, no forward-look*. That is now inaccurate — the
  software-event snapshot fixes real documented live failures, and the
  freeze-availability probe closes a silent-correctness gap. The AMD row has genuine
  near-term work, not just a completed capability.

---

## One-line synthesis

AMD has no continuous-trace hardware and never will on the current roadmap
(**confirmed**), so the wins are all about the sampled window and the fallbacks
around it: **build a BTF block-step tier** (complete flow at ~1 trap/branch, works on
Zen 2, rootless — and it corrects the repo's own plan), **capture the 16-entry LBR
deterministically at the region boundary via a software-event/eBPF snapshot** (fixing
the two documented live Zen-5 failure modes), and **probe freeze-on-PMI availability
before trusting a window** — then tune BRS period, `spec`-bit filtering, ring/throttle
config, and the stitcher's overlap check to sharpen the tier asm-test already ships.
