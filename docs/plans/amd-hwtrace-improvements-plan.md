# asm-test — AMD hardware-trace improvements: implementation plan

This plan turns the recommendations of the
[AMD hardware-trace improvement analysis](../analysis/amd-hardware-trace-improvements.md)
into buildable phases. Every phase produces or refines an `asmtest_trace_t`
([include/asmtest_trace.h](../../include/asmtest_trace.h)) — the same ordered
instruction offsets and branch-normalized block offsets the Unicorn emulator, the
DynamoRIO native tier, Intel PT, and the existing AMD LBR / single-step backends
already fill — so no new public trace struct or field is introduced and overflow /
loss continues to collapse onto the existing `truncated` bit. Block offsets are
computed with the **same** branch-edge normalization as
[src/pt_backend.c](../../src/pt_backend.c) (split at every branch target and after
every branch instruction) so a reconstructed trace stays byte-identical to the other
tiers.

This plan is a **sibling** of the [AMD LBR snapshot plan](amd-lbr-trace-plan.md)
(the backend most of these phases extend), the
[Zen 2 single-step plan](zen2-singlestep-trace-plan.md) (whose W2 out-of-process
ptrace stepper the block-step phase extends — and whose "W3 is blocked on x86" claim
this plan **corrects**), and the [hardware-trace plan](hardware-trace-plan.md). It
exists because the analysis established that AMD's hardware ceiling is fixed: the
wins are not a new trace facility but squeezing the sampled 16-entry window harder
and adding a cheaper *complete-flow* fallback than per-instruction single-step.

> Status legend: **planned** unless noted; forward-look phases are tagged
> *(forward-look)*. Update this file as phases land, the way
> [amd-lbr-trace-plan.md](amd-lbr-trace-plan.md) and
> [zen2-singlestep-trace-plan.md](zen2-singlestep-trace-plan.md) track theirs. The
> house rule holds: **no untested hardware code** — a lane that cannot self-validate
> on its target silicon self-skips (`available()` → 0) rather than shipping unproven.

---

## The governing constraint

The analysis is definitive and bounds this plan: **AMD ships no Intel-PT /
Arm-CoreSight-ETM equivalent** and no BTS-to-memory on any Zen part through Zen 5, and
none is on the Zen 6/7 roadmap. The 16-entry branch stack (`AMD_LBR_DEPTH`,
[src/amd_backend.c:48](../../src/amd_backend.c), duplicated at
[src/hwtrace.c:58](../../src/hwtrace.c)) is a silicon ceiling — CPUID `0x80000022`
EBX only *reports* it. So none of these phases chase a continuous trace. They fall in
two programmes:

- **Squeeze the sampled window** — capture the 16-entry LBR deterministically at the
  region boundary, stop trusting a window the hardware never froze, drop
  speculative/wrong-path edges, and harden the Tier-B stitch — all within the
  shipped AMD LBR backend ([src/amd_backend.c](../../src/amd_backend.c),
  [src/hwtrace.c](../../src/hwtrace.c) `hwtrace_begin_amd` / `hwtrace_end_amd`).
- **A cheaper complete-flow fallback** — a **BTF block-step** mode of the W2
  out-of-process ptrace stepper: one trap per taken branch instead of one per
  instruction, on *every* Zen (including Zen 2, which has no branch hardware),
  rootless. This is the marquee item and the one that corrects the Zen 2 plan.

The AMD LBR tier stays a fast/non-perturbing complement, never a default; the
`truncated` bit continues to route the caller to DynamoRIO (no depth ceiling) for
whole-program reach. No phase changes that contract.

---

## Implementation status

**All phases planned.** Nothing here has landed. The recommendations, their external
verification, and the confirmed dead ends live in the
[analysis doc](../analysis/amd-hardware-trace-improvements.md); this plan is the
implementation decomposition. Build order follows the analysis priorities:
Phase 0 (gating) → Phase 1 (freeze correctness) → Phase 2 (BTF block-step) →
Phase 3 (software-event snapshot) are the P0/near-term work; Phases 4–5 are P1
refinements; Phases 6–7 are forward-look.

Two **documentation corrections** the analysis surfaced ship alongside Phase 0/2 (see
Deliverables): the `PTRACE_SINGLEBLOCK`-unwired-on-x86 claim in
[zen2-singlestep-trace-plan.md](zen2-singlestep-trace-plan.md), and the "AMD LBR is
finished, no forward-look" framing in [native-tracing.md](../native-tracing.md) /
[hardware-tracing.md](../hardware-tracing.md).

---

## Phase 0 — Feature detection & gating (`src/hwtrace.c`) *(planned)*

**Goal.** Detect the per-uarch/per-kernel capabilities the later phases depend on, so
every new lane self-skips cleanly instead of misbehaving on the wrong silicon.

**Work.**
- Add a CPUID reader to the existing `#if defined(__x86_64__)` block near
  `amd_branch_probe` ([src/hwtrace.c:180](../../src/hwtrace.c)). There is **no**
  reusable cpuid helper in hwtrace.c (vendor detection is `/proc/cpuinfo`-based via
  `vendor_is`, [hwtrace.c:120](../../src/hwtrace.c)); reuse the `<cpuid.h>` /
  `__get_cpuid`/`__get_cpuid_count` pattern already used in
  [src/asmtest.c:434](../../src/asmtest.c). Read leaf `0x80000022`: EAX bit 1 =
  `AMD_LBR_V2`, **EAX bit 2 = `AMD_LBR_PMC_FREEZE`** (the freeze-on-PMI gate Phase 1
  needs), EBX = `lbr_v2_stack_sz` (the runtime depth for the P2 depth-detection
  hygiene item).
- **Runtime depth detection.** Replace the hardcoded `AMD_LBR_DEPTH` 16 (duplicated
  at [amd_backend.c:48](../../src/amd_backend.c) and
  [hwtrace.c:58](../../src/hwtrace.c)) with a value read from CPUID EBX (fallback 16).
  Because it drives the Tier-A/Tier-B split ([hwtrace.c:568](../../src/hwtrace.c)),
  the stitch `out_cap` ([hwtrace.c:570](../../src/hwtrace.c)), and the overflow flag
  ([amd_backend.c:117](../../src/amd_backend.c)), the decoder must take depth as a
  **parameter** rather than reading a local macro — a cross-TU signature change that
  must be mirrored in the non-Linux stubs ([amd_backend.c:239/261](../../src/amd_backend.c)).
  Ships as a no-op today (every part reports 16) but removes the assumption.
- Add skip-reason strings in `asmtest_hwtrace_skip_reason`
  ([hwtrace.c:245-284](../../src/hwtrace.c)) for any new gated outcome (e.g. a
  freeze-unsupported note), following the existing AMD strings at
  [hwtrace.c:264-274](../../src/hwtrace.c).

**Acceptance.** `make hwtrace-test` still passes on the Zen 5 dev box
(`make docker-hwtrace-amd`); a debug print of the detected freeze bit + depth matches
`amd_lbr_v2` (freeze present, depth 16). No behavior change yet — this phase only
adds detection the later phases consume.

---

## Phase 1 — Freeze-on-PMI window-trust gate (`src/hwtrace.c`) *(planned)*

**Goal.** Stop trusting a 16-entry window the hardware never froze. Freeze-on-PMI is
**not** universal on Zen 4 (gated on the Phase-0 CPUID `0x80000022` EAX[2] bit); when
absent, the LBR keeps advancing past the overflow point, so a captured window can
silently *not* end at region exit — yet `hwtrace_end_amd` trusts it today.

**Work.**
- In `hwtrace_end_amd` ([hwtrace.c:449](../../src/hwtrace.c)), when the Phase-0 freeze
  bit is **absent**, treat a Tier-A window whose newest in-region branch does not
  reach the region exit as `truncated` rather than complete (consistent with the
  existing "no in-region branches" truncation at
  [hwtrace.c:588-589](../../src/hwtrace.c)), so the caller re-resolves under
  `CEILING_FREE` / falls to DynamoRIO.
- Prefer the Phase-3 software-event snapshot (which stops the LBR in software) over
  PMI sampling on freeze-absent parts once Phase 3 lands; until then, the honest
  `truncated` is the correct degrade.
- Keep the probe/capture attr divergence in mind: `amd_branch_probe`
  ([hwtrace.c:186-189](../../src/hwtrace.c)) omits `exclude_hv` while
  `hwtrace_begin_amd` ([hwtrace.c:423-424](../../src/hwtrace.c)) sets it; any
  attr-touching change here must stay mirrored or `available()` and `begin()` disagree.

**Acceptance.** On a freeze-present host (`amd_lbr_v2`, the dev box) behavior is
unchanged and `make docker-hwtrace-amd` still reconstructs the branch-heavy loop; a
unit fixture simulating a freeze-absent, exit-missing window sets `truncated` instead
of emitting a short "complete" trace. The freeze-**absent** branch cannot be exercised
live on the Zen 5 box (which reports freeze present), so it ships behind the Phase-0
self-skip + this synthetic fixture and stays **pending real freeze-absent Zen 4
hardware**, per the house "no untested hardware code" rule (see Risks).

---

## Phase 2 — BTF block-step mode in the W2 ptrace stepper (`src/ptrace_backend.c`) *(planned)*

**Goal.** A complete, exactly-ordered in-region trace at **one `#DB` per taken
branch** (≈ per basic block) instead of one per instruction — on *every* Zen
(including Zen 2, which has no branch hardware), **rootless** (ptrace of your own
child; no `CAP_PERFMON`). This is the correct, hardware-clean form of the recommended
block-step tier, and it **corrects** the Zen 2 plan's claim that
`PTRACE_SINGLEBLOCK` is unwired on x86.

**Why it lives in the W2 ptrace backend, not the hwtrace `SINGLESTEP` backend or a new
`asmtest_trace_backend_t` member.** BTF requires setting `DEBUGCTL.BTF`, a ring-0 MSR
— the in-process `ss_backend.c` (EFLAGS.TF) path *cannot* do this. The kernel sets it
on the tracer's behalf only via `PTRACE_SINGLEBLOCK` → `user_enable_block_step()` →
`set_task_blockstep()` (`arch/x86/kernel/step.c`). So block-step is inherently an
**out-of-process ptrace** capability, exactly where W2 lives, and is exposed through
the `asmtest_ptrace_*` surface (as W2 single-step already is) rather than as a
hwtrace backend enum member (the fixed 4-member set at
[asmtest_hwtrace.h:54-60](../../include/asmtest_hwtrace.h) — INTEL_PT / CORESIGHT /
AMD_LBR / SINGLESTEP — is the *in-process/decoder* tier; the W2 stepper is not one of
them).

**Work.**
- Thread a `step_mode` (single-step vs block-step) through the shared bodies
  `trace_attached_impl` ([ptrace_backend.c:764](../../src/ptrace_backend.c)) and the
  `trace_call` tracee loop ([ptrace_backend.c:633](../../src/ptrace_backend.c)) rather
  than duplicating the loop. In block-step mode issue `PTRACE_SINGLEBLOCK` in place of
  `PTRACE_SINGLESTEP`. `PTRACE_SINGLEBLOCK` (request `33`) is frequently **absent from
  glibc's `<sys/ptrace.h>`** even though the x86 kernel wires it, so provide a local
  `#ifndef PTRACE_SINGLEBLOCK` / `#define PTRACE_SINGLEBLOCK 33` (or include
  `<linux/ptrace.h>`) — otherwise the build fails looking like an unsupported host
  rather than a missing constant.
- **Reconstruct intra-block instructions.** A `PTRACE_SINGLEBLOCK` stop lands only at
  the **target** of each taken control transfer — one PC per basic block, not per
  instruction, and `#DB` is trap-class so the stop RIP is the *target*, not the
  source. To feed the *same* `normalize()` / `trace_append_insn` contract
  ([ptrace_backend.c:452](../../src/ptrace_backend.c)) as the per-insn path, walk
  forward from the previous block-entry PC with `asmtest_disas(PTRACE_TRACE_ARCH, …)`
  lengths ([the only reconstruction primitive](../../include/asmtest_trace.h), also
  used by `normalize`), appending each reconstructed insn offset, then feed that
  **reconstructed per-instruction stream** (not the block-entry PCs) to `normalize`,
  which re-derives `blocks[]` — so `blocks[]` parity follows from `insns[]` being
  correct. This is the load-bearing extra work; a decode desync (`asmtest_disas`
  length 0) sets `truncated` exactly as `normalize` does at
  [ptrace_backend.c:465-468](../../src/ptrace_backend.c).
- **Block-terminator rule — get this exactly right.** BTF traps **only on taken
  transfers**, so the recorded next-stop PC is the taken branch's *target* (an
  arbitrary address, generally **not** the block's fall-through), and a block may
  contain *not-taken* conditional branches mid-run (a not-taken `Jcc` raises no `#DB`).
  The walk therefore must **not** terminate on "PC == next-stop"; it terminates at the
  control-transfer instruction that actually reaches next-stop: unconditional branches
  / indirect branches / `ret` always end the block (their taken target *is* next-stop),
  while a direct conditional ends it only when its static taken target == next-stop —
  otherwise that `Jcc` was not taken, so append it and keep walking the fall-through.
- **Same-target-conditional ambiguity → `truncated`.** Two direct conditionals to the
  *same* target in one block (first not taken, second taken — `je T; …; je T`) are
  statically indistinguishable: BTF gives no signal for *which* was taken, so a greedy
  "first `Jcc` whose target == next-stop" terminates the block early and desyncs
  `insns[]`. When more than one candidate terminator in the straight-line run targets
  next-stop, treat the block as ambiguous and set `truncated` (the block-step analogue
  of the documented POPF/IRET single-step edge) rather than guessing.
- Reuse the two existing arch seams unchanged — `read_pc_ret`
  ([ptrace_backend.c:322](../../src/ptrace_backend.c)) to read the target RIP at each
  `#DB`, and `PTRACE_TRACE_ARCH` ([ptrace_backend.c:302](../../src/ptrace_backend.c))
  for the Capstone arch — and preserve the initial-in-region-PC capture
  ([ptrace_backend.c:815](../../src/ptrace_backend.c)) so offset 0 is not missed.
- **Filter non-branch stops.** The `#DB` also fires on interrupts/exceptions (AMD APM
  Vol 2 §13.2), so a stop whose RIP is still inside the current straight-line run (not
  a control-transfer target) must be skipped, not treated as a block edge.
- **Arch asymmetry.** `PTRACE_SINGLEBLOCK` is an x86-only kernel feature; on aarch64
  there is no equivalent, so the block-step entry must fall back to the existing
  per-insn `PTRACE_SINGLESTEP` path (a naive `#define STEP_REQUEST` swap is wrong).
  Add an x86-only `asmtest_ptrace_blockstep_available()` probe distinct from the
  aarch64 `probe_singlestep` self-skip ([ptrace_backend.c:387](../../src/ptrace_backend.c)).
- **Public surface.** Add, mirroring the existing `_call` / `_attached` family:
  `asmtest_ptrace_blockstep_available`, `asmtest_ptrace_trace_call_blockstep`,
  `asmtest_ptrace_trace_attached_blockstep` to
  [include/asmtest_ptrace.h](../../include/asmtest_ptrace.h), with matching ENOSYS
  stubs in the non-supported-host block ([ptrace_backend.c:915](../../src/ptrace_backend.c))
  for symbol parity. (New signature over an opts-flag on the existing entries, which
  would ABI-break the shipped 4-symbol family.) Follow the `ASMTEST_PTRACE_*` return
  codes ([asmtest_ptrace.h:60](../../include/asmtest_ptrace.h)). **Scoped out:** no
  `_trace_attached_versioned_blockstep` mirror of the JIT/time-correct-bytes lane
  ([ptrace_backend.c:948](../../src/ptrace_backend.c)) — the HW-attributed
  managed-runtime lane is Phase 3's eBPF snapshot; add a versioned block-step only if a
  rootless JIT-on-Zen-2 need surfaces.

**Acceptance.** `make hwtrace-test` (or a new `blockstep-test` mirroring the W2
single-step test) shows the block-step stream is **byte-identical** — `insns[]` and
`blocks[]` — to the per-instruction `PTRACE_SINGLESTEP` path on the existing loop
fixtures, with a materially lower stop count; runs live under a **plain** `docker run`
(no `--cap-add`), and self-skips on aarch64/non-x86 with a clear reason.

---

## Phase 3 — Software-event / eBPF on-demand LBR snapshot (`src/hwtrace.c` + optional BPF) *(planned)*

**Goal.** Read the 16-entry LbrExtV2 stack **deterministically at the region
boundary** via `bpf_get_branch_snapshot()` (`amd_pmu_v2_snapshot_branch_stack`),
eliminating the two live Zen-5 failure modes the [AMD LBR plan](amd-lbr-trace-plan.md)
documents: the tiny single-shot routine "too fast to be sampled in-region," and the
post-routine-glue contamination that forced the richest-in-region heuristic
([hwtrace.c:465-474, 542-548](../../src/hwtrace.c)). Also the natural HW-attributed
managed-runtime lane on AMD (a uprobe on a JITted method), which today falls to W2
ptrace single-step.

**Work.**
- Add a BPF program (uprobe/`fentry` at region entry/exit) that calls the snapshot
  helper and delivers the frozen window to userspace, decoded by the existing
  `asmtest_amd_decode` ([amd_backend.c:104](../../src/amd_backend.c)) — no
  `sample_period=1` ring, no throttle exposure, no richest-window scan.
- **Optional-library lane.** Gate the BPF object exactly like the existing codeimage
  eBPF detector: set `-DASMTEST_HAVE_LIBBPF` + skeleton only when `pkg-config libbpf`
  **and** `clang` **and** `bpftool` are all present
  ([mk/native-trace.mk:144-165](../../mk/native-trace.mk)); otherwise compile the TU
  without the `-D`, invoke no BPF toolchain, and have the lane's `available()` return
  0 (self-skip) — the same shape as `CODEIMAGE_SKEL` / `LIBIPT_DEF`.
- **Gates.** Zen 4/5 (perfmon v2) only — Zen 3 BRS and Zen 2 do not route through
  `amd_pmu_v2_handle_irq` and return no snapshot; Linux ≥ 6.10; `CAP_PERFMON` +
  `CAP_BPF`. All expressed as `available()`-time self-skips with reasons.

**Acceptance.** A new `make docker-hwtrace-lbr-snapshot` lane (mirroring
`docker-hwtrace-codeimage` at [mk/docker.mk:254](../../mk/docker.mk): dedicated
Dockerfile with clang+libbpf+bpftool, `--cap-add=BPF --cap-add=PERFMON
--cap-add=SYS_PTRACE --security-opt seccomp=unconfined`, BTF kernel — **not**
`--privileged`) captures a **tiny single-shot routine** that the sampling path marks
`truncated`, and reconstructs it complete. Self-skips cleanly without libbpf / the
caps / a Zen 4+ host.

---

## Phase 4 — LbrExtV2 speculation-bit filtering (`src/amd_backend.c`) *(planned)*

**Goal.** Drop speculative / wrong-path phantom edges before replay. Today
`amd_replay` filters only aborts (`if (e->abort) continue;`,
[amd_backend.c:67-68](../../src/amd_backend.c)) and `amd_edge_eq`
([amd_backend.c:124](../../src/amd_backend.c)) explicitly ignores every other flag.

**Work.**
- In `amd_replay`, extend the abort guard to also skip records whose
  `perf_branch_entry.spec` == `PERF_BR_SPEC_WRONG_PATH`. Dropping such records is
  *expected*, not a desync, so it must **not** set `truncated` (unlike the four
  existing truncation sites).
- **Kernel-version guard.** The `spec` 2-bit field only exists on newer
  `<linux/perf_event.h>`; gate the field access on struct/kernel availability, not
  merely the `__linux__`/`__x86_64__` block, so older-header builds still compile.
- **Gen gate.** Spec/valid bits are LbrExtV2 (Zen 4/5, Linux ≥ 6.1) only; a no-op on
  Zen 3 BRS (retired-only, no spec bits). If wrong-path records are instead filtered
  during window assembly in `hwtrace_end_amd`, do it there — not in `amd_edge_eq`,
  whose from+to-only overlap semantics the stitcher depends on.

**Acceptance.** `make docker-hwtrace-amd` reconstructs a branch fixture containing a
synthesized wrong-path entry without the phantom edge; behavior is unchanged where the
`spec` field is unavailable or all entries are correct-path.

---

## Phase 5 — Tier-B stitch hardening + throttle config (`src/amd_backend.c`, `src/hwtrace.c`) *(planned)*

**Goal.** Reduce silent mis-stitches and extend Tier-B reach before honest
truncation, without pretending an unavailable HW facility exists (Intel's
`hw_idx`-based stitch cannot port — AMD sets `hw_idx ≡ 0`; this is a confirmed dead
end).

**Work.**
- **Decodable-distance invariant.** In `asmtest_amd_stitch`'s smallest-overlap loop
  ([amd_backend.c:173-188](../../src/amd_backend.c)), after a from+to overlap `match`
  and before accepting the shift, require that the straight-line span between
  consecutive stitched edges is Capstone-decodable with reconstructed instruction
  count == static decode distance; reject the shift and keep searching otherwise. This
  needs `base`/`base_ip`/`len` threaded into `asmtest_amd_stitch` (which has no code
  access today) — a signature change mirrored in the non-Linux stub
  ([amd_backend.c:249](../../src/amd_backend.c)) and the call site
  ([hwtrace.c:576](../../src/hwtrace.c)), and it inherits the Capstone
  decoder-present gating.
- **Ring / throttle hardening.** Grow the AMD data ring
  (`g_opts.data_size` / `round_pages` default `64*1024`,
  [hwtrace.c:433](../../src/hwtrace.c)) and document requiring
  `kernel.perf_event_max_sample_rate` raised and `kernel.perf_cpu_time_max_percent=0`
  on the self-hosted runner. Must **not** remove the `PERF_RECORD_LOST` /
  `PERF_RECORD_THROTTLE` → `lost` detection ([hwtrace.c:503-505](../../src/hwtrace.c)),
  which is the only "run did not fit" signal surviving windows cannot show. Fix the
  `data_size` header comment ([asmtest_hwtrace.h:65](../../include/asmtest_hwtrace.h)),
  which says `0=8KB` but is backend-dependent — the AMD default is 64 KB
  ([hwtrace.c:433](../../src/hwtrace.c)) while Intel PT keeps 8 KB
  ([hwtrace.c:645](../../src/hwtrace.c)); document **both** defaults rather than flipping
  the single number (which would then be wrong for Intel PT).

**Acceptance.** A synthetic self-overlapping-loop fixture that the current
smallest-overlap heuristic mis-stitches now either stitches correctly or reports an
honest gap (`truncated`); `make docker-hwtrace-amd`'s 20 000-trip loop reconstructs
further before truncating with the enlarged ring.

---

## Phase 6 — BRS period-adjust single-window Tier-A capture (`src/hwtrace.c`) *(forward-look)*

**Goal.** On **Zen 3 BRS**, for a routine known to take ≤ ~16 taken branches, replace
the `sample_period=1` PMI-per-branch flood with a single fixed-period frozen overflow
(the kernel already programs `period − lbr_nr` via `amd_brs_adjust_period`), cutting
Tier-A capture from O(branches) interrupts to one and removing the throttle/ring
exposure.

**Work / caveat.** `sample_period=1` is **load-bearing** for the current design: the
Tier-B stitch and the richest-in-region heuristic
([hwtrace.c:465-474, 561-585](../../src/hwtrace.c);
[amd_backend.c:129-131](../../src/amd_backend.c)) assume a sample at *every* taken
branch so consecutive 16-deep windows overlap by 15 edges. A fixed BRS period breaks
that overlap. So this is **not** a blanket change to `hwtrace_begin_amd`
([hwtrace.c:414](../../src/hwtrace.c)) — it must be a **distinct Tier-A capture mode**,
selected only when the region is known-small and the Tier-B path is not needed, and
kept off Zen 4/5 (where the Phase-3 software-event snapshot is the better lever). The
`period=1` appears in two places (probe [hwtrace.c:186](../../src/hwtrace.c), capture
[hwtrace.c:420](../../src/hwtrace.c)); a mode switch must keep them coherent or justify
the divergence.

**Acceptance.** *(forward-look — deferred until a Zen 3 host is available; the dev box
is Zen 5.)* On a Zen 3 BRS host, a ≤16-branch routine reconstructs identically to the
`sample_period=1` path with a single PMI; the Tier-B stitch path is untouched.

---

## Phase 7 — IBS-Op complementary coverage lane (`src/hwtrace.c` + new backend) *(forward-look)*

**Goal.** The **only** hardware branch source on **Zen 2** (which has no branch
stack): a statistical, precise-IP source→target edge via `MSR_AMD64_IBSBRTARGET`
(gated on `IBS_CAPS_BRNTRGT`), used as a *coverage-confirmer* / hot-edge pre-cover to
shrink — not bound — the Phase-2 block-step / DynamoRIO residual, and as an
indirect-branch-target resolver on Zen 3/4/5.

**Work / caveat.** IBS is **statistical** (one tagged op per NMI, worse edge-yield
than LBR) and cannot produce an ordered, complete path, so it is explicitly *not* a
replacement for the branch stack and must never feed the `insns[]`/`blocks[]` parity
contract as if complete. It needs `CAP_PERFMON`/`CAP_SYS_ADMIN` (no user/kernel
filter). If exposed, it is a *separate diagnostic producer*, not a member of the
fidelity cascade. Two raw proposals were refuted in verification and must be avoided:
`rand_en` is an IBS-*Fetch* knob (not Op), and the branch target is capability-gated
and valid only for retired taken branches.

**Acceptance.** *(forward-look — deferred; low priority.)* On a Zen 2 host, IBS-Op
edges mark a subset of region basic blocks as covered and reduce the number of blocks
the block-step fallback must walk; self-skips without `IBS_CAPS_BRNTRGT` / `CAP_PERFMON`.

---

## Deliverables (Phases 0–5)

- **hwtrace.c:** a `<cpuid.h>` leaf-`0x80000022` reader (freeze bit + depth) near
  `amd_branch_probe`; runtime `AMD_LBR_DEPTH` threaded as a decoder parameter across
  both TUs; freeze-gated Tier-A `truncated` logic in `hwtrace_end_amd`; enlarged AMD
  data ring; new/updated skip-reason strings; the software-event snapshot capture
  path.
- **amd_backend.c:** spec/wrong-path filter in `amd_replay`; `base`/`len`-threaded
  `asmtest_amd_stitch` with the decodable-distance invariant; depth-as-parameter on
  `asmtest_amd_decode` / `asmtest_amd_decode_stitched` — all mirrored in the non-Linux
  stub block ([amd_backend.c:233-272](../../src/amd_backend.c)) and hwtrace.c's forward
  decls ([hwtrace.c:45-53](../../src/hwtrace.c)).
- **ptrace_backend.c + asmtest_ptrace.h:** the block-step `step_mode` through
  `trace_attached_impl` / `trace_call`; three new public symbols
  (`asmtest_ptrace_blockstep_available`, `_trace_call_blockstep`,
  `_trace_attached_blockstep`) with non-supported-host stubs.
- **Build/test:** the optional libbpf lane knob mirroring `CODEIMAGE_SKEL`; a
  `Dockerfile.hwtrace-lbr-snapshot` + `docker-hwtrace-lbr-snapshot` target; a
  block-step host test + docker lane (plain `docker run`); host self-skip additions to
  [examples/test_hwtrace.c](../../examples/test_hwtrace.c).
- **Bindings:** the three new `asmtest_ptrace_*` symbols wrapped (whole-word) in each
  of the 10 bindings' `hwtrace.*` wrappers so
  [scripts/check-bindings-parity.sh](../../scripts/check-bindings-parity.sh) stays
  green (the tier-symbol × 10 count grows by three); no new `asmtest_trace_choice_t`
  field (the `3 * sizeof(int)` static-assert at
  [asmtest_trace_auto.h:83-84](../../include/asmtest_trace_auto.h) must hold).
- **Docs:** correct the `PTRACE_SINGLEBLOCK` claim in
  [zen2-singlestep-trace-plan.md](zen2-singlestep-trace-plan.md) (Phase 2 makes it
  real); update the "AMD LBR is finished" framing in
  [native-tracing.md](../native-tracing.md) / [hardware-tracing.md](../hardware-tracing.md);
  refresh the [trace-parity-matrix](../analysis/trace-parity-matrix.md) AMD rows.

## Validation (per lane, self-skipping)

- **Host, privilege-free:** `make hwtrace-test` and the block-step test run under a
  plain `docker run` (Phase 2 needs only ptrace of its own child) and self-skip on
  non-x86/absent-capability hosts — the first AMD-adjacent lane that runs *live* on
  ordinary CI rather than needing bare-metal AMD.
- **Bare-metal AMD:** `make docker-hwtrace-amd` (`--security-opt seccomp=unconfined
  --cap-add=PERFMON`) validates Phases 1, 4, 5 on the Zen 5 dev box; Phase 3 adds
  `docker-hwtrace-lbr-snapshot` (BPF/PERFMON/PTRACE caps).
- **Cross-tier parity:** every reconstructed trace is asserted byte-identical
  (`insns[]` + `blocks[]`) to the existing single-step / DynamoRIO output on shared
  fixtures, using the same branch-edge normalization as `pt_backend.c` — the parity
  harness the AMD LBR plan already established.
- **Binding parity:** `make check-bindings-parity` stays green after the three new
  ptrace symbols land.

## Risks and open points (AMD improvements)

- **Binding-parity cost of block-step.** Three new `asmtest_ptrace_*` symbols must be
  wrapped in all 10 bindings or the gate fails; an intentional gap needs a
  [bindings-parity-allow.txt](../../scripts/bindings-parity-allow.txt) entry (stale
  entries also fail). Prefer wrapping over exempting.
- **Block-step reconstruction fidelity.** The intra-block insn reconstruction (walking
  `asmtest_disas` lengths between `#DB` targets) is the load-bearing new work; get it
  wrong and `insns[]` diverges from the other tiers while `blocks[]` can still look
  right. The byte-parity acceptance test is the guard, and the block-terminator rule +
  same-target-conditional ambiguity (both spelled out in Phase 2) are the specific
  traps. Self-modifying / non-well-behaved routines (POPF/IRET, in-routine signals),
  and the ambiguous same-target-`Jcc` block, remain the documented edge and set
  `truncated`.
- **BRS period vs stitching conflict (Phase 6).** A fixed BRS period breaks the
  `sample_period=1` overlap the Tier-B stitch and richest-in-region heuristic depend
  on; it must be a separate Tier-A-only mode, hence forward-look.
- **`spec`-field kernel gating (Phase 4).** The field is newer-kernel-only; access
  must be struct-availability-guarded, not just arch-guarded.
- **base_ip remap (Phase 5).** `amd_replay` assumes captured branch IPs live at the
  same virtual address as the `base` buffer it disassembles; a mismatch silently
  yields a degenerate trace with no `truncated`. Any record-stream change must preserve
  that IP-space identity.
- **Single-active-region invariant.** All AMD capture state
  (`g_fd`/`g_base_map`/`g_active`, [hwtrace.c:610](../../src/hwtrace.c)) is a single
  process-global slot reset in `hwtrace_end_amd`; any new freeze/snapshot state must be
  single-instance and reset there too.
- **Hardware coverage.** Live validation exists only on Zen 5 (`amd_lbr_v2`). Zen 3
  BRS (Phase 6) and freeze-absent Zen 4 (Phase 1) paths are code-implemented and
  self-skip-validated but remain **pending real hardware**, per the house "no untested
  hardware code" rule.
