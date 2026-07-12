# Zen 2 hardware-tracing review — IBS is the one unused lever

Review date: 2026-07-12. Host: **AMD Ryzen 9 4900HS** (family 0x17, model 0x60 =
Renoir / **Zen 2**), Linux 6.14.3, `perf_event_paranoid=2`, no `CAP_PERFMON`, no
passwordless sudo — i.e. the unprivileged rootless envelope the tiers must live in.

This is an empirical review: every claim below was produced by compiling and running a
`perf_event_open` probe on *this* box and decoding the raw records, not by reading specs.
The repro programs are in the session scratchpad (`hwprobe2.c`, `ibsraw.c`, `ibscaps.c`,
`ibsattach.c`); the plan that turns this into work is
[zen2-ibs-tracing-plan.md](../plans/zen2-ibs-tracing-plan.md).

## 1. Executive summary

Every hardware-assisted trace tier in the tree **self-skips on this Zen 2 host**, so the
machine only ever falls back to software single-stepping (~1000× slowdown, and unsafe on a
live JIT). Yet this CPU has exactly one usable branch-tracing facility — **AMD IBS**
(Instruction-Based Sampling) — and it is **completely unused**: there is zero IBS code in
the repo; it appears only in planning docs.

The headline finding is that IBS is **more capable, and far cheaper to reach, than the
existing plans assumed.** On this box IBS-Op delivers a *statistical from→to control-flow
edge* per sample — branch source **and** branch target — **out of band, against a separate
live process, with no elevated privilege**, and it does so without perturbing the target's
control flow. That is precisely the capability the single-step views lack: a way to observe
control flow on a JIT/managed runtime that single-stepping can crash.

## 2. What this host can actually do (measured)

| Facility | Result on this box | How determined |
|---|---|---|
| Intel PT | absent (wrong vendor) | no `intel_pt` PMU node |
| AMD branch stack (Zen 3 BRS / Zen 4+ LbrExtV2) | **`EOPNOTSUPP` (95)** — no branch hardware | `perf_event_open` of `PERF_SAMPLE_BRANCH_STACK` fails; no `amd_lbr_v2`/`perfmon_v2` flags |
| MSR-direct 16-deep LBR | absent (LbrExtV2-only) | Zen 2 exposes only the 4 legacy `DebugCtl` LBR MSRs (a confirmed dead end) |
| **IBS Op** (retire-side sampler) | **works, unprivileged, per-pid** | `ibs_op` PMU type 11; `swfilt`(`config2:0`)+`exclude_kernel` → 2730+ samples |
| **IBS Op → branch EDGES** | **works** — `IbsOpRip`→`IbsBrTarget` | `IBS_CAPS_BRNTRGT`=1; `reg[7]` tracks the target 100% (1240/1240 taken samples) |
| **IBS Op out-of-band on another pid** | **works, target unperturbed** | attached to a separate uncooperative child → 5405 samples / 863 edges; target stayed alive |
| IBS Fetch (front-end / i-cache view) | works | `ibs_fetch` type 10; `swfilt` → 4095 samples |
| IBS + `PERF_SAMPLE_CALLCHAIN` | works | 5461 samples with a user-space stack unwind each |
| `cnt_ctl=1` (dispatched-op sampling) | works | `config:19` → more uniform than the default cycle-count sampling |
| `inherit=1` (auto-follow threads) | **fails** (per-task ringbuffer mmap `EINVAL`) | whole-process needs one event+mmap **per tid** |
| kernel-inclusive / system-wide IBS | **`EACCES` (13)** | needs `perf_event_paranoid<=0` or `CAP_PERFMON` |

### The IBS Op raw record (empirically decoded)

Opened with `type=ibs_op`, `sample_period≈0x4000`, `config2=1` (`swfilt`),
`exclude_kernel=1`, `sample_type=IP|RAW`. Each `PERF_RECORD_SAMPLE` carries a raw block of
**8 `u64` registers**:

```
reg[0] IbsOpCtl     reg[1] IbsOpRip (branch SOURCE)   reg[2] IbsOpData
reg[3] IbsOpData2   reg[4] IbsOpData3                 reg[5] IbsDcLinAd
reg[6] IbsDcPhysAd  reg[7] IbsBrTarget (branch TARGET, when IBS_CAPS_BRNTRGT)
```

`IbsOpData` (`reg[2]`) carries the branch resolution bits: `BrnRet` (bit 37, "this op
retired a branch"), `BrnTaken` (bit 35), `BrnMisp` (bit 36), `Return` (bit 34). In a
branch-heavy loop ~50% of tagged ops were branches, and for every taken branch `reg[7]` was
a code address that reconstructed the loop's back-edges and forward skips exactly — i.e.
`{reg[1] → reg[7]}` is a real, statistically-sampled control-flow edge.

## 3. Correction to the settled analysis

The consolidated AMD plan's **Phase 7**
([amd-tracing-plan.md](../plans/amd-tracing-plan.md), "IBS-Op complementary coverage lane")
states the IBS edge lane "needs `CAP_PERFMON`/`CAP_SYS_ADMIN` (no user/kernel filter)" and
sketches reading the target via `MSR_AMD64_IBSBRTARGET` directly. **Both privilege
assumptions are refuted on this kernel:**

1. **User-only IBS needs no privilege.** IBS hardware cannot filter by privilege at the
   source, but the kernel's `swfilt` bit (software post-filter, `ibs_op/format/swfilt` =
   `config2:0`, present since ~6.2) makes `exclude_kernel=1` work by dropping kernel-domain
   samples in software. With `swfilt` set, IBS-Op opens and samples at `paranoid=2`,
   unprivileged, per-pid. Without `swfilt`, `exclude_kernel` returns `EINVAL` (confirmed).
2. **The branch target arrives in the perf record — no MSR read.** The perf IBS driver
   already reads `IbsBrTarget` in its NMI handler and delivers it as `reg[7]` of
   `PERF_SAMPLE_RAW` whenever `IBS_CAPS_BRNTRGT` is set (it is, here). No `/dev/cpu/N/msr`,
   no `CAP_SYS_ADMIN`.

Phase 7's other cautions **hold and were re-confirmed**: IBS is statistical (never an
ordered/complete path, must never feed the `insns[]`/`blocks[]` parity contract);
`rand_en` is an IBS-*Fetch* knob, not Op; the target is valid only for retired taken
branches. What changes is the **privilege floor and the delivery path** — which is exactly
what makes an *unprivileged, out-of-band* asmspy view feasible on this host.

## 4. The gap

The AMD tier (`src/amd_backend.c`, `src/hwtrace.c`, `src/branchsnap.c`, `src/msr_lbr.c`) is
built entirely around the 16-deep branch stack (Zen 3 BRS / Zen 4/5 LbrExtV2). Its decoder
is validated on Zen 2 with **synthetic** `perf_branch_entry[]` inputs, but every *capture*
path returns `EUNAVAIL`/`EOPNOTSUPP` here. The statistical whole-window survey
(`asmtest_hwtrace_sample_window_amd`, `hwtrace.c:976`) likewise returns `EUNAVAIL` on Zen 2
because it arms the branch stack (this is review item **F6**). So the one branch-tracing
facility this silicon has sits idle while the machine single-steps everything.

## 5. Improvement options (ranked)

**1. Build the IBS-Op statistical edge lane (highest leverage).** A new out-of-band
backend that turns IBS samples into `{from, to, taken, mispredict, return}` edges,
aggregated into a hot-edge / hot-block histogram. Three payoffs, all unprivileged:
   - **A safe hardware view for managed runtimes (.NET / JVM / Node).** asmspy's
     stream/graph/tree views single-step, which can crash a live JIT (a documented hazard).
     IBS observes retired ops out-of-band with zero control-flow perturbation — proven here
     against a separate process that ran untouched. A new `asmspy --sample` view is the
     safe HW alternative on exactly the targets single-step is dangerous on.
   - **A block-step accelerator.** IBS pre-covers hot edges, shrinking (not bounding) the
     residual the exact block-step / DynamoRIO tier must walk — the Phase-7
     "coverage-confirmer" role, now realizable without the privilege Phase 7 assumed.
   - **Fixes F6** — gives the whole-window survey a real result on Zen 2 instead of
     `EUNAVAIL`.

**2. Solve whole-process coverage via system-wide IBS (one privilege drop).** `inherit=1`
can't carry a ringbuffer, so per-tid events race JIT/GC thread creation. The clean fix is a
per-CPU IBS event set covering all threads (incl. future ones), filtered by pid — but that
needs `paranoid<=0` / `--cap-add=PERFMON`. Worth a documented opt-in plus a docker lane;
without it, stay per-tid and state the coverage hole honestly.

**3. Tier it honestly in `trace_auto.c`.** Add IBS as a distinct **STATISTICAL** fidelity
producer (never feeding the exact parity contract) — which also motivates the review's
F22/F26/F37 "rung/mechanism discriminator" so a statistical result is never mistaken for an
exact one.

**4. IBS Fetch lane (lower priority).** Front-end fetch-address + i-cache/ITLB-miss
coverage; complements Op's retire-side view and catches code the op-sampler missed.

## 6. Honest limitations (record, do not fight)

- **Statistical, not exact.** IBS tags one retired op per NMI window; cold edges are
  under-covered and coverage is probabilistic in the cold tail. It can *never* prove "block
  X did not execute" — the one thing an exact coverage oracle needs. Never feed parity.
- **Per-tid capture races thread creation** (until the system-wide variant, which needs
  privilege). A sibling thread that starts mid-capture gets a late/partial event.
- **Throttle-limited density.** `perf_event_max_sample_rate` is currently 63000/s/cpu but
  is auto-lowered by the `perf_cpu_time_max_percent=25` governor under sustained IBS NMI
  cost (previously observed at 4000/s/cpu). You cannot densify by shortening the period —
  it just throttles; more cold-edge recall needs longer runtime only.
- **Retired-op tagging quirks.** Fused/folded branches and the `RipInvalidChk` case need
  handling; `IBS_CAPS_RIPINVALIDCHK`=1 here lets us detect an invalid RIP and drop it.

## 7. Dead ends (do not pursue on Zen 2)

- Direct-MSR / BPF-snapshot / 16-deep LBR — the silicon is absent (`EOPNOTSUPP` confirmed).
- Legacy `DebugCtl` 1-deep LBR — needs root, and GDB already dropped `record-btrace bts` on
  AMD for exactly this insufficiency.
- Any *exact/ordered* trace from IBS — statistical by construction.
- Intel PT / CoreSight — wrong vendor / arch.

## 8. Housekeeping note

The AMD *plan* docs describe the dev box as a Ryzen 9 9950X (Zen 5) while the AMD *review*
describes it as Zen 2. The machine this review ran on is unambiguously Zen 2 (Ryzen 9
4900HS, family 0x17). Either there are two hosts or one of those references is stale; worth
reconciling when the plan below lands.

> **Resolved 2026-07-12: there are two hosts.** The benchmark box records under
> [benchmarks/boxes/](../../../benchmarks/boxes/) carry both — `amd-linux-x86_64-f39fe67d`
> is the Ryzen 9 9950X (zen5) and `amd-linux-x86_64-9e05f0f2` is the Ryzen 9 4900HS
> (zen1-2, this review's machine). The single-host wording in
> [amd-tracing-followup-plan.md](../plans/amd-tracing-followup-plan.md) and
> [zen2-singlestep-trace-plan.md](../plans/zen2-singlestep-trace-plan.md) has been amended
> to the two-host reality.
