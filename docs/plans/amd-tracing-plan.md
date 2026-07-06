# asm-test — AMD native tracing: LBR backend + improvements

The single home for asm-test's **AMD-specific** hardware-assisted native tracing. It
consolidates what were three overlapping documents — the AMD LBR snapshot backend
plan, the improvement analysis, and the improvement implementation plan — into one:

- **[Part I — AMD LBR snapshot backend](#part-i--amd-lbr-snapshot-backend-shipped)**
  — the shipped backend built on AMD's branch-record facilities (BRS / LbrExtV2),
  Phases 0–5.
- **[Part II — Improvement analysis](#part-ii--improvement-analysis)** — the
  cross-checked findings on how to improve the AMD path (prioritized matrices,
  corrections, confirmed dead ends).
- **[Part III — Improvement roadmap](#part-iii--improvement-roadmap-planned)** — those
  findings turned into buildable phases.

Like the Intel PT backend it produces `asmtest_trace_t` offsets reusing the same
registered-region begin/end markers: instruction offsets identical to the Unicorn
emulator, the DynamoRIO native tier, and Intel PT, and block offsets that match after
the same branch-edge normalization step. It is a **sibling** of the
[hardware-trace plan](hardware-trace-plan.md) (Intel PT / ARM CoreSight), the
[single-step plan](zen2-singlestep-trace-plan.md) (Trap Flag `#DB`), and the
[DynamoRIO native-trace plan](dynamorio-native-trace-plan.md).

> Status legend: **planned** unless noted; forward-look phases are tagged
> *(forward-look)*. Update this file as phases land, the way
> [hardware-trace-plan.md](hardware-trace-plan.md) and
> [inline-asm-keystone-plan.md](../archive/plans/inline-asm-keystone-plan.md) track theirs. The house
> rule holds: **no untested hardware code** — a lane that cannot self-validate on its
> target silicon self-skips (`available()` → 0) rather than shipping unproven.

---

## The governing constraint

This is the hardware fact everything below is bounded by, **confirmed** against the
kernel sources, AMD APM Vol 2 / PPR, and LWN: **AMD ships no Intel-PT /
Arm-CoreSight-ETM equivalent** — no continuous, packetized, AUX-delivered control-flow
trace on any Zen part through Zen 5 (mid-2026), and none on the public Zen 6 / Zen 7
roadmap. There is no BTS-to-memory either (GDB removed `record-btrace bts` on AMD
after confirming AMD's `DebugCtl` drives only the four legacy LBR MSRs). Intel PT
records *every* branch decision continuously into a kernel AUX ring you drain at
`end()`; AMD has no such ring. Its branch-record facilities are a **fixed-depth
(16-entry) stack** — the last 16 taken branches — captured into a perf *sample* on a
PMU interrupt:

- **BRS** — AMD Family 19h "Zen 3", PPR §2.1.13: up to 16 consecutive taken branches;
  opt-in; surfaced via perf `branch-brs` / `PERF_SAMPLE_BRANCH_STACK`.
- **LbrExtV2** — Zen 4, mainline since Linux 6.1: a 16-deep Last Branch Record with
  hardware branch filtering (kernel/user/cond/call/ret/...), freeze-on-PMI, and
  branch-speculation flags, surfaced through the same perf branch-stack API.
- **IBS** (Instruction-Based Sampling) is the third AMD facility but is purely
  *statistical* — a coverage histogram, never an ordered complete path — so it is out
  of scope for parity offsets (see Part II).

The 16-entry stack ([`AMD_LBR_DEPTH`](../../src/amd_backend.c), duplicated at
[src/hwtrace.c:58](../../src/hwtrace.c)) is a **silicon ceiling**, not a software
choice — CPUID `0x80000022` EBX only *reports* it. From this follows the **fundamental
result**: on AMD you can match Intel PT on trace **completeness** *or* on near-zero
**overhead**, but not both for an arbitrary routine. The two operating points:

- **Tier A — routines within the 16-taken-branch window.** A single branch-stack
  snapshot holds *all* of the routine's taken branches, so reconstruction is **exact
  and complete** — bit-for-bit identical `insns[]`/`blocks[]` to the PT, DynamoRIO, and
  Unicorn backends — at small absolute overhead (a handful of PMIs). **This is the
  backend Part I ships.**
- **Tier B — longer / looping routines.** Completeness requires *stitching* overlapping
  16-entry stacks sampled at every taken branch (`sample_period = 1`), which keeps
  completeness but sacrifices the low-overhead property (Phase 5); the supported path
  beyond the window is the **DynamoRIO fallback**, which has no depth ceiling and is the
  most accurate/detailed AMD trace.

So "improving AMD hardware tracing" (Parts II–III) is **not** the search for a hidden
continuous facility — that is a verified dead end. It is two concrete programmes:
**squeeze the sampled 16-entry window harder and more correctly**, and **add a cheaper
*complete-flow* fallback than per-instruction single-step** (one that also covers Zen 2,
which has no branch hardware at all, and runs rootless). The AMD LBR tier stays a
fast/non-perturbing complement, never a default; the `truncated` bit continues to route
the caller to DynamoRIO for whole-program reach. No phase changes that contract. On
**pre-Zen3** AMD (e.g. Zen 2) the exact in-process backend is instead the
[single-step plan](zen2-singlestep-trace-plan.md)'s Trap-Flag stepper.

---

# Part I — AMD LBR snapshot backend (shipped)

## Implementation status

**Phases 0–4 implemented.** `ASMTEST_HWTRACE_AMD_LBR`
([include/asmtest_hwtrace.h](../../include/asmtest_hwtrace.h)) ships with: the AMD
vendor + branch-stack-probe gating in [src/hwtrace.c](../../src/hwtrace.c)
(`amd_branch_probe`, `vendor_is("AuthenticAMD")`, AMD `skip_reason` strings); the
data-ring (non-AUX) `PERF_SAMPLE_BRANCH_STACK` capture (`hwtrace_begin_amd` /
`hwtrace_end_amd`); and the reconstruction backend
[src/amd_backend.c](../../src/amd_backend.c) (`asmtest_amd_decode`), which replays the
registered bytes through the Capstone layer (`asmtest_disas`) between branch records,
normalizes blocks at branch edges (identical to
[pt_backend.c](../../src/pt_backend.c)), filters aborted entries, and flags
window-overflow (`nbr >= 16`) as `truncated`. Built into `libasmtest_hwtrace` and
`hwtrace-test`.

**Validation.** The reconstruction, block parity, and overflow-truncation (Phases 2–4)
are validated host-independently with **synthetic** `perf_branch_entry[]` inputs
(examples/test_hwtrace.c `test_amd_reconstruction`): a Tier-A branch stack reconstructs
the exact same `insns[]`/`blocks[]` the PT/DynamoRIO backends produce
(`{0,3,6,0xc,0x11}`, blocks `{0,0x11}`), and a full 16-entry stack sets `truncated`.

**Live capture (Phase 1) is verified on a Zen 5 host** — the project's actual dev box
is a **Ryzen 9 9950X (Family 0x1A, Zen 5, `amd_lbr_v2`)**, not Zen 2 as earlier
revisions of this plan assumed. `test_amd_live` (`make docker-hwtrace-amd`, which runs
the hwtrace image with `--security-opt seccomp=unconfined --cap-add=PERFMON` so
`perf_event_open` is permitted) exercises the real `PERF_SAMPLE_BRANCH_STACK` capture +
decode: a branch-heavy loop is reconstructed from the live 16-deep LbrExtV2 stack
(loop-body block, `truncated` past 16 branches), and a tiny single-shot routine
honestly `truncated`s (perf delivers the stack only at a PMU sample, so a fast
single-run routine is never sampled in-region). That run also fixed a capture bug:
`hwtrace_end_amd` kept the *last* perf sample — all post-routine glue for a small
routine, decoding to nothing yet flagged complete — and now keeps the sample **richest
in in-region branches**, setting `truncated` when none is found. Capture still
self-skips on **Zen 2** (no branch facility — `EOPNOTSUPP`), non-AMD, VMs, and CI's
default sandbox; the Intel-PT gating is unchanged (AMD still self-skips PT).

**Phase 5 Tier-B stitching is now implemented as a host-validated algorithm**
(`asmtest_amd_stitch` + `asmtest_amd_decode_stitched` in
[src/amd_backend.c](../../src/amd_backend.c), proven by `test_amd_stitch` against an
18-iteration loop past the 16-deep window); only the live multi-sample capture wiring
(bounded by perf ring size + throttling) and the MSR-direct snapshot remain forward-look
(see Phase 5 below).

---

## Phase 0 — Vendor gating & feature detection *(planned)*

**Goal.** Extend the detect-and-skip chain so `asmtest_hwtrace_available(AMD_LBR)`
returns 1 only on an AMD host that actually exposes usable branch records.

**Work.**

- Add `ASMTEST_HWTRACE_AMD_LBR` to `asmtest_trace_backend_t` in
  [include/asmtest_hwtrace.h](../../include/asmtest_hwtrace.h).
- Add an `amd_matches()` arm to [`cpu_matches`](../../src/hwtrace.c): require
  `__x86_64__` and `AuthenticAMD` from `/proc/cpuinfo` (mirror of the existing
  `GenuineIntel` probe).
- Replace the PMU-node probe: AMD branch records are **not** a `pmu_type()` AUX PMU like
  `intel_pt`. The real capability probe is *attempt-and-close* — open a sampling
  `perf_event` with `branch_sample_type = PERF_SAMPLE_BRANCH_USER |
  PERF_SAMPLE_BRANCH_ANY` and `sample_period = 1`; success means LBR/BRS is usable,
  `EINVAL`/`EACCES` means skip. Distinguish Zen 3 BRS (needs the opt-in `branch-brs`
  event) from Zen 4 LbrExtV2 (works with a generic sampling event) by trying LbrExtV2
  first and falling back to the BRS event.
- `asmtest_amd_decoder_present()` returns 1 only when the backend TU is compiled in (it
  depends on Capstone for the instruction-length walk — see Phase 2).
- Extend `asmtest_hwtrace_skip_reason()` with AMD-specific strings ("not an
  AuthenticAMD x86-64 host", "no AMD branch records (needs Zen 3 BRS / Zen 4
  LbrExtV2)", "perf branch-stack not permitted").

**Acceptance.** `make hwtrace-test` self-skips on Intel, on AMD without privilege, and
on non-AMD hosts, each with the specific reason; returns available only on a Zen 3+/Zen
4 host with perf branch sampling permitted.

---

## Phase 1 — Branch-record capture *(planned)*

**Goal.** Capture the routine's taken-branch records into a host-side buffer around the
begin/end markers, on the calling thread (`pid=0`).

**Key divergence from the PT capture path.** Intel PT records flow through the **AUX**
mmap; AMD branch records flow through the **base (data)** ring as `PERF_RECORD_SAMPLE`
records carrying a `struct perf_branch_stack` (`__u64 nr` + `struct perf_branch_entry
entries[]`, each `{from, to, flags}`). So [hwtrace.c](../../src/hwtrace.c)'s
`aux_offset`/`aux_size` mmap is **not** used for this backend; instead `begin()` mmaps
only the data ring and `end()` walks `PERF_RECORD_SAMPLE` records out of it.

**Capture strategy (Tier A).** Set `sample_period = 1` on a taken-branch event so a
sample is emitted at *every* taken branch. Because a Tier-A routine has ≤16 taken
branches, this is only a handful of interrupts (small absolute overhead) and sidesteps
the "no clean snapshot-at-`end()`" problem entirely: the sample emitted at the routine's
*last* in-region branch already holds the complete ≤16-entry history (freeze-on-PMI
keeps it consistent on LbrExtV2). `end()` selects the in-region sample with the greatest
coverage and hands its branch array to the decoder.

- Use `PERF_SAMPLE_BRANCH_USER` to scope to EL0 userspace (drop kernel/runtime noise),
  matching PT's `exclude_kernel`.
- The `attr.disabled=1` → `IOC_ENABLE`/`IOC_DISABLE` bracket from
  [`asmtest_hwtrace_begin`/`end`](../../src/hwtrace.c) is reused unchanged.

**Acceptance.** For a registered routine with a known handful of taken branches, the
captured branch array on a Zen 3+/Zen 4 host contains exactly those `from→to` pairs, in
order, ending at the region exit.

---

## Phase 2 — LBR reconstruction backend (`src/amd_backend.c`) *(planned)*

**Goal.** Turn the ordered `perf_branch_entry[]` into the same offset stream the PT
backend produces, by replaying asm-test's registered bytes between branches.

**Decoder interface.** PT's decoder takes raw `(aux, aux_len, base, len, trace)`; the
AMD decoder takes `(const struct perf_branch_entry *br, size_t nbr, base, len, trace)`.
Add `asmtest_amd_decode(...)` alongside `asmtest_pt_decode` / `asmtest_cs_decode` in
[hwtrace.c](../../src/hwtrace.c)'s dispatch.

**Reconstruction.** Unlike libipt (which carries its own instruction decoder), the AMD
path must walk instructions itself to find boundaries and offsets, so it **reuses the
project's existing Capstone layer** (the same dependency
[src/disasm.c](../../src/disasm.c) already uses for annotation):

1. Start at the region entry (`base` / the begin marker PC). Branch records give ordered
   `from_i` (the branch instruction's address) and `to_i` (its target).
2. For each branch *i*: linearly decode instructions with Capstone from the current IP
   up to and including `from_i`, appending each in-range offset via `trace_append_insn`;
   then continue at `to_i`.
3. Mark a block start at the entry and after every branch (i.e. at every `to_i`) —
   **identical normalization to [pt_backend.c](../../src/pt_backend.c)** — via
   `trace_append_block`, so block offsets match Unicorn/DR/PT.
4. Filter mispredicted/aborted/speculative entries using the `perf_branch_entry` flags
   (LbrExtV2 reports speculation), the AMD analogue of PT's `insn.speculative` filter.

**Gating.** Compile under `-DASMTEST_HAVE_AMD_LBR` (predicated on Capstone being
present, since reconstruction needs the length-decoder). Without it the TU compiles
decoder-free and `asmtest_amd_decoder_present()` returns 0, so
`asmtest_hwtrace_available(AMD_LBR)` self-skips — exactly like the libipt/OpenCSD gating.

**Acceptance.** For a deterministic, single-threaded, ≤16-taken-branch registered
routine, `insns[]` is byte-for-byte identical to the Unicorn/DynamoRIO/PT output for the
same code, and block offset 0 is recorded.

---

## Phase 3 — Block-boundary parity *(planned)*

**Goal.** Prove the normalized block partition matches the other backends.

This is the same non-trivial step the PT plan budgets: an LBR/PT "block" implied by
consecutive branches is coarser than a Unicorn/DR basic block, so blocks are recomputed
from the reconstructed `insns[]` by splitting at every branch *target* and after every
branch *instruction*. Unit-test the AMD backend's `blocks[]` against the DynamoRIO tier's
block set for the same routine (cross-backend parity test, reusing the PT parity
harness).

**Acceptance.** AMD `blocks[]` == DynamoRIO `blocks[]` == PT `blocks[]` for the shared
fixture routines (within the Tier-A window).

---

## Phase 4 — Window-overflow truncation & DynamoRIO fallback routing *(planned)*

**Goal.** Make the 16-branch ceiling *safe and honest*, never silently wrong.

- **Detect overflow.** If the routine executed more taken branches than the stack depth
  (16), the snapshot is incomplete. Detect this — the captured array is full (`nr ==
  depth`) and its earliest `to` does not reach the region entry — and set
  `trace->truncated = true` (the same loss bit PT uses for `OVF`/AUX-full). Never emit a
  partial trace as if complete.
- **Route the fallback.** Document and (where a caller orchestrates backends) wire the
  rule: AMD_LBR is attempted for within-window routines; on overflow or unavailability
  the trace is produced by the **DynamoRIO native tier** (or Unicorn), which has no depth
  ceiling and is the most accurate/detailed AMD trace. The hardware tier is a
  fast/non-perturbing *complement* for small routines, never a replacement.

**Acceptance.** A routine that exceeds 16 taken branches sets `truncated` (and the
orchestrating caller falls back to DynamoRIO); a within-window routine does not.

---

## Phase 5 — Tier B (stitching) & MSR-direct snapshot

- **LBR stitching for arbitrary length. _Algorithm + live wiring shipped (Zen 5-validated)._**
  Sample at every taken branch (`sample_period = 1`) and splice the overlapping 16-entry
  stacks (15-entry overlap) into one gapless taken-branch sequence, reconstructing a
  trace past the window — host-validated complete, and now wired into the live capture
  (escalating Tier-A → Tier-B on window overflow, with all loss flagged).
  - _Done._ The stitching core ships in [src/amd_backend.c](../../src/amd_backend.c):
    `asmtest_amd_stitch(samples, nrs, n_samples, out, cap, &gap)` merges the windows in
    execution order — for each new window it takes the **smallest shift that still
    overlaps the accumulated tail** (the contiguous, largest-overlap assumption) and
    appends only the genuinely new edges, so a loop's repeated identical edges stitch
    correctly (each window contributes one). Lost overlap (≥ a full window dropped to
    throttling) sets `*gap`. `asmtest_amd_decode_stitched(...)` then replays the stitched
    sequence through the shared `amd_replay` loop **without** the 16-entry overflow flag
    (stitching, not window depth, established completeness), honoring `gap` as the loss
    signal. The `amd_replay` body is factored out of `asmtest_amd_decode`, so Tier-A and
    Tier-B share one decoder.
  - _Done._ Host-validated without hardware (like the Tier-A reconstruction):
    [examples/test_hwtrace.c](../../examples/test_hwtrace.c) `test_amd_stitch` synthesizes
    the `sample_period=1` windows of an 18-iteration loop, stitches them back to the
    gapless 18-edge sequence, and proves the decode is **complete** (all 55 instructions,
    two blocks, not truncated) where a single Tier-A 16-window honestly truncates — plus a
    gap-detection case.
  - _Done — live wiring (Zen 5-validated)._ `hwtrace_end_amd`
    ([hwtrace.c](../../src/hwtrace.c)) now collects **every** branch-stack sample in the
    perf data ring (time order) alongside the single richest-in-region window. When that
    window overflowed (`best_nr >= 16` — the routine took more taken branches than the
    stack is deep) it escalates from Tier-A to Tier-B: `asmtest_amd_stitch` splices the
    windows and `asmtest_amd_decode_stitched` decodes the result past the ceiling. The
    small-routine path (`best_nr < 16`) is unchanged. Completeness is gated on the precise
    loss signals: a stitch **gap**, OR a `PERF_RECORD_LOST`/`PERF_RECORD_THROTTLE` record
    (the data ring is non-overwrite, so on overflow the kernel drops the *newest* samples
    and emits `LOST` — the signal the surviving windows alone cannot show, since they
    stitch gaplessly yet are missing the tail). Either → honestly `truncated`.
    **Validated live on a Zen 5** (Ryzen 9 9950X, `make docker-hwtrace-amd`): a
    20000-trip loop reconstructs ~290 instructions — ≈95 stitched taken branches, far past
    a single 16-deep window (which caps ~49) — and stays truncated, as the two hardware
    realities this plan flagged require: the perf **data-ring size** (a long loop emits
    more `sample_period=1` samples than the ring holds → drops) and **sample-rate
    throttling** (`kernel.perf_event_max_sample_rate` → `≥16`-consecutive drops). So the
    live path is **complete only for runs whose taken-branch count fits the ring and
    survives throttling**; beyond that it stitches as far as it can and flags the loss,
    with DynamoRIO (no ceiling) the answer for whole-program reach. The *complete*,
    drop-free reconstruction is proven host-independently by `test_amd_stitch` (synthetic
    `sample_period=1` windows → the exact 55-instruction trace, not truncated).
- **MSR-direct snapshot.** Read the LBR/BRS MSRs directly around the region to get an
  exact Tier-A snapshot with *zero* interrupts (vs Phase 1's per-branch PMIs). Needs
  privilege (a kernel helper / `CAP_SYS_ADMIN` or a tiny module), so it is a
  self-hosted-runner optimization, not a portable path.

---

## Deliverables (Phases 0–4)

- `ASMTEST_HWTRACE_AMD_LBR` enum + AMD arms in `available()`/`skip_reason()` in
  [include/asmtest_hwtrace.h](../../include/asmtest_hwtrace.h) /
  [src/hwtrace.c](../../src/hwtrace.c).
- Data-ring (non-AUX) branch-record capture in [src/hwtrace.c](../../src/hwtrace.c),
  gated `__linux__` + `__x86_64__`.
- `src/amd_backend.c`: the Capstone-based reconstruction + branch-edge block
  normalization, its own TU like `pt_backend.c`/`cs_backend.c`.
- Makefile knobs mirroring `LIBIPT_*`: `-DASMTEST_HAVE_AMD_LBR` (predicated on Capstone),
  `amd_backend.o` in `HWTRACE_OBJS`, and the PIC variant in `libasmtest_hwtrace`.
- A cross-backend parity unit test (AMD vs DynamoRIO vs PT block/insn sets) added to the
  `hwtrace-test` fixtures.

## Validation (AMD branch records)

- AMD BRS: Fam19h Zen 3, PPR §2.1.13 (16-deep taken-branch sampling); Linux
  `PERF_EVENTS_AMD_BRS`, opt-in `branch-brs` event.
- AMD LbrExtV2: Zen 4, mainline Linux 6.1+; 16-deep LBR with hardware branch filtering,
  freeze-on-PMI, and speculation flags via `PERF_SAMPLE_BRANCH_STACK`.
- Capture needs `perf_event_paranoid` lowered or `CAP_PERFMON`, as with PT.
- Cannot run on standard CI (cloud VMs do not reliably expose AMD LBR/BRS to guests); a
  self-hosted bare-metal Zen 3+/Zen 4 runner is required, with the same standing
  operational cost and security caveats the PT plan documents.
  - AMD Fam19h BRS (LWN): https://lwn.net/Articles/877245/
  - `CONFIG_PERF_EVENTS_AMD_BRS`: https://cateee.net/lkddb/web-lkddb/PERF_EVENTS_AMD_BRS.html
  - AMD LbrExtV2 (LWN): https://lwn.net/Articles/904482/
  - AMD Zen 4 LbrExtV2 (Phoronix): https://www.phoronix.com/news/AMD-Zen-4-LbrExtV2

## Risks and open points (AMD LBR)

- **The 16-branch ceiling is the defining limit.** Most real routines loop past it;
  Tier A is genuinely useful only for small, branch-light registered routines — which is
  exactly asm-test's common case, but must be stated, not assumed. Beyond it, DynamoRIO
  is the answer, not this backend.
- **No clean "snapshot now."** Solved for Tier A by `sample_period = 1` (a sample at
  every branch, cheap because branches are few); if a future variant uses a coarse period
  instead, the in-region sample may miss the final branches — verify the selected sample
  actually reaches the region exit before trusting it.
- **Reconstruction needs the exact registered bytes** (like PT) *and* a correct
  length-decoder (Capstone); self-modifying or relocated code silently corrupts offsets.
  Speculative/mispredicted entries must be filtered via the `perf_branch_entry` flags
  before recording.
- **Vendor/uarch detection is coarser than PT's PMU node.** There is no single sysfs
  `type` file; capability is established by attempting the branch-stack open.
  Distinguishing Zen 3 BRS (opt-in event) from Zen 4 LbrExtV2 (generic event) is a
  try-and-fallback, which must be unit-tested on both uarches or documented as
  Zen 4-first.
- **Completeness vs overhead is unresolvable on AMD.** This backend deliberately picks
  completeness-for-small-routines; the plan does **not** claim PT parity for arbitrary
  code, and Phase 5's stitching is explicitly the worse-overhead trade.

---

# Part II — Improvement analysis

*Status: analysis / findings. A consolidated, cross-checked answer to **how can
asm-test's AMD hardware-trace path be improved?** — derived from the source of record
([src/amd_backend.c](../../src/amd_backend.c), [src/hwtrace.c](../../src/hwtrace.c)'s AMD
gating/capture chain, [include/asmtest_hwtrace.h](../../include/asmtest_hwtrace.h),
[src/ptrace_backend.c](../../src/ptrace_backend.c)) cross-referenced against external
primary sources (AMD APM Vol 2 / PPR, the Linux `arch/x86/events/amd/` and
`arch/x86/kernel/step.c` sources, LWN, and the perf man pages). Companion docs:
[trace-parity-matrix](../analysis/trace-parity-matrix.md) (what works where today) and
[jit-runtime-tracing](../analysis/jit-runtime-tracing.md) (the foreign-JIT forward-look).
Where a claim was adversarially verified against a primary source the verdict is stated
inline.*

> **Provenance.** Every "capability is real" statement below was checked against a named
> primary source (kernel source file, AMD APM/PPR section, or LWN write-up). Verdict
> tags: **[confirmed]** the mechanism and its benefit hold; **[real, qualified]** the
> capability is real but the naive framing overstates it (the qualification is folded
> into the text); **[dead end]** refuted against a primary source. Generation/kernel
> gates are stated because several facilities that look uniform across "Zen 2/3/4/5" are
> in fact Zen-4-and-later or a specific kernel version.

## Summary

The governing fact bounds every option (stated in full under
[The governing constraint](#the-governing-constraint) above): AMD has no continuous
control-flow trace, and the 16-entry branch stack is a silicon ceiling. So "improving
AMD hardware tracing" is **not** the search for a hidden continuous facility — that is a
verified dead end. It is two concrete programmes:

1. **Squeeze the sampled 16-entry window harder and more correctly** — capture it
   deterministically at the region boundary instead of by `sample_period=1` PMI-flood,
   and stop trusting a window that hardware never froze.
2. **Add a cheaper *complete-flow* fallback than per-instruction single-step** — one that
   also covers Zen 2 (which has no branch hardware at all) and runs rootless.

Both are buildable today. One of them — the BTF block-step tier — also **corrects a
factual error in the repo's own** [single-step plan](zen2-singlestep-trace-plan.md).

## The headline correction: BTF block-step is available on x86

[zen2-singlestep-trace-plan.md](zen2-singlestep-trace-plan.md) files the W3 "DEBUGCTL.BTF
branch-granular step" idea under *research-only*, on the stated grounds that
*"`PTRACE_SINGLEBLOCK` is wired only on PowerPC/s390 … unwired on x86."*

That premise is **incorrect [confirmed]**. Linux implements BTF block-step generically
in [`arch/x86/kernel/step.c`](https://github.com/torvalds/linux/blob/master/arch/x86/kernel/step.c):
`set_task_blockstep()` sets `DEBUGCTLMSR_BTF` + `TIF_BLOCKSTEP`, and `enable_step()` sets
`EFLAGS.TF` — the exact `BTF=1 && TF=1` pairing AMD APM Vol 2 §13.2 requires. It is
reached from userspace via `user_enable_block_step()` → **`PTRACE_SINGLEBLOCK`, which
*is* supported on mainline x86**. There is no Intel-vs-AMD vendor check, and BTF has been
baseline AMD64 since APM rev 3.07 (2002), so it is present on Zen 2/3/4/5.
`PTRACE_SINGLEBLOCK` is merely *undocumented* in the `ptrace(2)` man page — undocumented ≠
unwired. It needs only ptrace of one's own child: **no `CAP_PERFMON`, works under any
`perf_event_paranoid`.**

This unlocks a **block-step tier**: one trap-class `#DB` per **taken branch** (≈ per
basic block) rather than one per instruction. For asm-test's compute kernels that is
typically a **4–10× reduction in stops** versus the current per-instruction single-step
tier, while producing an identically-shaped, exactly-ordered, ceiling-free
`asmtest_trace_t` — and it is the **only exact real-CPU option on Zen 2**, where no
branch-record hardware exists. It is a modest extension of the already-shipping
`PTRACE_SINGLESTEP` machinery in [src/ptrace_backend.c](../../src/ptrace_backend.c).

**Qualifications to build in [real, qualified]:**

- The `#DB` also fires on **interrupts and exceptions**, not only program branches
  (APM §13.2), so trap count ≥ taken-branch count and is load-dependent; the
  reconstructor must discard interrupt/exception stops.
- `#DB` is **trap-class**: the stop `RIP` is the branch **target**, not the source. The
  source is recovered by disassembling the fall-through range up to the terminating
  branch (asm-test already has the region offsets + Capstone).
- BTF (and TF) **auto-clear on each `#DB`** — re-arm every step.
- It is *complete-at-moderate-overhead*, **not** "cheap": each block still costs a full
  ptrace tracer round-trip (~4 context switches), orders of magnitude above DynamoRIO's
  in-code-cache basic-block instrumentation, and it perturbs timing heavily. Position it
  in the cascade as the **rootless / Zen-2 / managed-runtime completeness fallback**,
  above per-instruction single-step and below DynamoRIO — not as a low-overhead tier.

This is the correct, hardware-clean version of the "block-cache single-step"
(INT3-on-terminators) idea, which the verification pass knocked down: software INT3
patching costs ~2 traps per block (restore + step-over + re-poke), fights W^X, and is
silently clobbered by self-modifying / JIT code — all problems BTF avoids by letting the
CPU do the block-stepping.

## Matrix 1 — Prioritized improvements

| # | Improvement | What it fixes | Gen / kernel gate | Verdict | Effort |
|---|---|---|---|---|---|
| **P0** | **BTF block-step tier** (`PTRACE_SINGLEBLOCK`) | Complete ordered flow at ~1 trap/branch; only exact real-CPU option on **Zen 2**; rootless (no `CAP_PERFMON`) | all Zen; any Linux x86-64 | real (corrects the plan) | Medium |
| **P0** | **Software-event / eBPF on-demand LBR snapshot** (`bpf_get_branch_snapshot` / `amd_pmu_v2_snapshot_branch_stack`) | Kills both live Zen-5 failure modes: tiny-single-shot "too fast to sample," and post-glue window contamination that forced the "richest-in-region" heuristic; HW-attributed managed-runtime lane | **Zen 4/5**, Linux **≥6.10**, `CAP_PERFMON`/`CAP_BPF` | real, gated | Medium |
| **P0** | **Probe `X86_FEATURE_AMD_LBR_PMC_FREEZE`** (CPUID `0x80000022` EAX[2]) and gate window-trust on it | Silent-correctness bug: freeze-on-PMI is **not** universal on Zen 4; without it the 16-entry window drifts past the overflow point and may not reach region exit | Zen 4/5 | **LANDED** (2026-07) | Small |
| **P1** | **BRS period-adjust single-window capture** (fixed period ≈ N−16) | Replaces `sample_period=1` — the dominant Tier-B throttle / ring-overflow truncation cause — with **one** frozen overflow for ≤16-branch routines | **Zen 3 BRS**, Linux ≥5.19 | SUPPORTED | Small–Med |
| **P1** | **Consume LbrExtV2 `spec`/`valid` bits** before replay/stitch | [amd_backend.c](../../src/amd_backend.c) filters only `abort` and *notes* it ignores spec flags — drop `PERF_BR_SPEC_WRONG_PATH` phantom edges | Zen 4/5, Linux ≥6.1 | **LANDED** (2026-07, Phase 4) | Small |
| **P1** | **Harden Tier-B throttle/ring config** (larger data ring; raise `kernel.perf_event_max_sample_rate`, set `kernel.perf_cpu_time_max_percent=0` on the runner) | Extends stitch reach before the kernel drops the newest samples — zero fidelity change | Zen 3/4/5 | **LANDED** (ring 64KB→256KB, Phase 5) | Small |
| **P1** | **Add a decodable-distance invariant to the stitcher** | The smallest-overlap heuristic can splice non-contiguous edges after a dropped/throttled sample; require the spliced adjacency be real straight-line code, else honest gap | Zen 3/4/5 | **LANDED** (2026-07, Phase 5 — honestly scoped: catches dropped-sample splices, not byte-decodable phase aliases) | Small–Med |
| **P2** | **IBS-Op complementary coverage lane** (esp. Zen 2) | Only HW branch source on Zen 2 (precise-IP source→target via `MSR_AMD64_IBSBRTARGET`, gated on `IBS_CAPS_BRNTRGT`); coverage-confirmer to shrink block-step/DR residual | statistical, not ordered — **forward-look (needs Zen 2)** | Medium |
| **P2** | **Runtime depth from CPUID `0x80000022` EBX** instead of `#define AMD_LBR_DEPTH 16` | Future-proofing hygiene (a no-op today — every shipping part reports 16) | Zen 4/5 | **LANDED** (2026-07, Phase 0 — EBX[9:4] `lbr_v2_stack_sz`) | Tiny |

### The three to build first

**1 — BTF block-step (P0).** Highest impact-to-effort, and the only path that gives
Zen 2 and rootless CI a complete real-CPU trace cheaper than per-instruction stepping.
See the correction above. Slots into the cascade as
`AMD_LBR (16-cap) → software-event snapshot → BTF block-step → DynamoRIO → per-insn single-step`.

**2 — Software-event LBR snapshot (P0).** The one item that *fixes documented live
failures*. Part I's own Zen-5 run recorded two: a tiny routine truncates because perf
only delivers the stack at a PMU sample (never fires in-region), and a capture bug where
post-routine glue evicted the routine's branches from the 16-deep window — forcing the
fragile "keep the richest-in-region sample" heuristic in
[`hwtrace_end_amd`](../../src/hwtrace.c). A `uprobe`/`fentry` BPF program at region
**entry/exit** that calls `bpf_get_branch_snapshot()` reads the frozen 16-entry stack
**deterministically at the boundary** — no `sample_period=1`, no throttle exposure, no
richest-window guessing, and the snapshot is taken *before* the glue runs. It is also a
HW-attributed managed-runtime lane on AMD, where the plan currently routes Node/.NET
straight to W2 ptrace single-step (Intel PT being unavailable). Merged upstream 2024
(`amd_pmu_v2_snapshot_branch_stack`, wired into `perf_snapshot_branch_stack`); the kernel
already inlines the freeze path (`__amd_pmu_lbr_disable`) so instrumentation does not
evict real entries. **Gates:** Zen 4/5 (perfmon v2) only — Zen 3 BRS and Zen 2 do not go
through `amd_pmu_v2_handle_irq`; Linux ≥6.10 (a 6.6.y backport was still being requested
in Jan 2026); `CAP_PERFMON`/`CAP_BPF`.

> **Status (2026-07): LANDED and validated on this host.** Both the substrate probe and
> the deterministic-snapshot CAPTURE ship. `asmtest_amd_snapshot_available()`
> ([src/amd_backend.c](../../src/amd_backend.c)) reports the hardware+kernel floor (AMD
> `amd_lbr_v2` + `perfmon_v2` CPU flags + Linux ≥ 6.10). `asmtest_amd_snapshot_trace()`
> ([src/branchsnap.c](../../src/branchsnap.c)) is the capture: it enables the LBR, plants a
> **hardware execution breakpoint** at the region exit, and a CO-RE BPF program
> ([bpf/branchsnap.bpf.c](../../bpf/branchsnap.bpf.c), `SEC("perf_event")` attached to the
> breakpoint) calls `bpf_get_branch_snapshot()` to read the frozen 16-entry stack at that
> ONE deterministic point. The raw `perf_branch_entry` array is decoded by the SAME
> `asmtest_amd_decode` the sampled path uses (in-region-filtered, so kernel-entry entries
> drop out) — reconstruction unchanged and already tested. **Design note:** a HARDWARE
> breakpoint (not a uprobe/fentry) is the trigger, because the region is anonymous
> executable memory (a JIT/mmap blob) with no inode+offset for a uprobe; the empirical
> **feasibility spike settled the open risk** — a `#DB` breakpoint does NOT evict the
> region's in-region branches before the helper reads them on this AMD part (the freeze
> path holds). **Validated:** `test_branchsnap` ([examples/test_branchsnap.c](../../examples/test_branchsnap.c),
> `make branchsnap-test`) drives a tiny single-shot routine — the exact case the
> `sample_period=1` path self-truncates — and asserts the boundary snapshot reconstructs
> its entry block; green on the dev Zen 5 (Ryzen 9 9950X) in the
> `docker-hwtrace-codeimage` lane (BPF + PERFMON caps), which now runs `branchsnap-test`
> after `codeimage-test`. **Remaining (follow-up):** a `snapshot: true` opt-in on
> `hwtrace_begin_amd` so the region begin/end markers route to this path automatically
> (today it is the standalone `asmtest_amd_snapshot_trace` entry point). **Note:** the AMD
> hardware lanes (`docker-hwtrace-amd`, `docker-hwtrace-codeimage`'s branchsnap step) are
> not in `.github/workflows/ci.yml` — they need a self-hosted AMD runner with the caps, so
> the AMD hardware path is validated on the dev box, not hosted CI.

**3 — Freeze-availability probe (P0). LANDED (2026-07).** Smallest change, real
correctness. The 2024 kernel fix made `DEBUGCTLMSR_FREEZE_LBRS_ON_PMI` conditional on
`X86_FEATURE_AMD_LBR_PMC_FREEZE` (CPUID `0x80000022` EAX[2]) precisely because *"this may
not be the case for all Zen 4 processors."* Without freeze the recorded stack keeps
advancing after the overflow transitions to CPL0, so a PMI window can silently **not** end
at region exit — yet the current AMD capture path trusts it. **As shipped:**
`asmtest_amd_freeze_available()` ([src/amd_backend.c](../../src/amd_backend.c)) probes the
bit (cached); the Tier-A single-window decode in `hwtrace_end_amd`
([src/hwtrace.c](../../src/hwtrace.c)) now trusts a freeze-less window as complete **only
if it actually captured the region-exit branch** (an entry with `from` in-region and `to`
outside), else flags `truncated` — the honest "do not assume the window reached exit"
posture. On a freeze-capable part the check is skipped (behavior unchanged).
`test_amd_freeze_probe` ([examples/test_hwtrace.c](../../examples/test_hwtrace.c)) asserts
a definite/stable answer and prints this host's support (the dev Zen 5 / Ryzen 9 9950X
reports freeze **PRESENT**, so the gate is a no-op there — confirming the concern is
Zen-4-specific, not universal). Preferring the software-event snapshot (path #2) where
freeze is absent remains folded into that item.

## Matrix 2 — Squeezing the existing window (P1 detail)

These keep asm-test's PMU-window architecture — which the research **confirms is at the
AMD hardware ceiling** — but remove its sharpest edges.

| Lever | Mechanism | Why it helps | Caveat |
|---|---|---|---|
| **BRS period-adjust** (Zen 3) | Fixed `sample_period ≈ N−16` (min 17); the kernel already programs `period − lbr_nr` (`amd_brs_adjust_period`) and BRS freezes/holds the NMI until the 16-branch buffer saturates | **One** PMI delivers the complete ≤16 window at region exit, versus `sample_period=1`'s one-PMI-per-branch flood that trips `perf_event_max_sample_rate` throttling and the non-overwrite ring | Zen 3 BRS **only** (forward-capture, fixed mode, period > 16). On Zen 4/5 the better lever is the software-event snapshot (P0 #2), not this |
| **`spec`/`valid` filtering** (Zen 4/5) | `perf_branch_entry.spec` carries `PERF_BR_SPEC_WRONG_PATH`; the LbrExtV2 driver passes wrong-path entries through to userspace | Drops speculative/wrong-path phantom edges before `amd_replay`, which today filters only `abort` and *explicitly notes* (amd_backend.c comment) that it ignores every other flag | Wrong-path entries are relatively uncommon, so this is a precision refinement, not a step-change. LbrExtV2/Linux ≥6.1 only — a no-op on Zen 3 BRS (retired-only, no spec bits) |
| **Throttle/ring hardening** | Larger `data_size` ring; `sysctl kernel.perf_event_max_sample_rate` up, `kernel.perf_cpu_time_max_percent=0` on the self-hosted runner | The Tier-B live path is bounded by ring size + throttling (a 20 000-trip loop already truncates); more headroom = longer gapless stitch before honest truncation | Operational, self-hosted-runner only; extends reach, does not remove the ceiling |
| **Decodable-distance stitch check** | For each stitched boundary, assert reconstructed instruction count between consecutive branch targets == statically-decoded byte distance; reject a wrong minimal-shift match, else emit an honest gap | AMD sets `hw_idx ≡ 0` (register renaming keeps From[0]=TOS), so Intel's exact index-based overlap count **cannot** be ported — the current smallest-overlap heuristic can silently mis-stitch a self-overlapping loop (an open question in amd_backend.c). This is the AMD-available substitute check | Improves correctness for the common looping case; does not extend depth |

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

## Notes on IBS (why it is P2, not P0)

IBS-Op is worth exactly one thing: it is the **only hardware branch source on Zen 2**,
and it carries a real precise-IP source→target edge (`IbsOpRip` → `MSR_AMD64_IBSBRTARGET`,
with `op_brn_ret`/`op_brn_taken`/`op_brn_misp` bits) — **capability-gated** on
`IBS_CAPS_BRNTRGT` (CPUID `Fn8000_001B` EAX[5], present on all Zen but must be probed).
But it is **statistical**: one tagged micro-op per counter period, so it yields a sparse,
probabilistic edge set — never an ordered, complete path — and its per-NMI edge yield is
*lower* than LBR's ~16 records per interrupt. It also requires
`CAP_SYS_ADMIN`/`CAP_PERFMON` (IBS PMUs have no user/kernel filter). So the honest role is
a **coverage-confirmer / hot-edge pre-cover** that shrinks (does not bound) the block-step
/ DynamoRIO residual, or an indirect-branch-target resolver — not a replacement for the
branch stack. (Two raw research proposals here were themselves wrong and were caught in
verification: `rand_en` is an IBS-*Fetch* knob, not Op; and IBS-Op does carry a branch
target, but only capability-gated and only for retired taken branches.)

## Documentation corrections this analysis implies

- [zen2-singlestep-trace-plan.md](zen2-singlestep-trace-plan.md): the
  `PTRACE_SINGLEBLOCK`-unwired-on-x86 claim is wrong (see the headline correction); it is
  what currently strands BTF block-step in "research only."
- [native-tracing.md](../guides/tracing/native-tracing.md) / [hardware-tracing.md](../guides/tracing/hardware-tracing.md):
  AMD LBR is presented as *finished, no forward-look*. That is now inaccurate — the
  software-event snapshot fixes real documented live failures, and the freeze-availability
  probe closes a silent-correctness gap. The AMD row has genuine near-term work, not just
  a completed capability.

## One-line synthesis

AMD has no continuous-trace hardware and never will on the current roadmap
(**confirmed**), so the wins are all about the sampled window and the fallbacks around
it: **build a BTF block-step tier** (complete flow at ~1 trap/branch, works on Zen 2,
rootless — and it corrects the repo's own plan), **capture the 16-entry LBR
deterministically at the region boundary via a software-event/eBPF snapshot** (fixing the
two documented live Zen-5 failure modes), and **probe freeze-on-PMI availability before
trusting a window** — then tune BRS period, `spec`-bit filtering, ring/throttle config,
and the stitcher's overlap check to sharpen the tier asm-test already ships.

---

# Part III — Improvement roadmap (planned)

This turns the Part II findings into buildable phases. Every phase produces or refines an
`asmtest_trace_t` ([include/asmtest_trace.h](../../include/asmtest_trace.h)) — the same
ordered instruction offsets and branch-normalized block offsets the Unicorn emulator, the
DynamoRIO native tier, Intel PT, and the existing AMD LBR / single-step backends already
fill — so no new public trace struct or field is introduced and overflow / loss continues
to collapse onto the existing `truncated` bit. Block offsets are computed with the
**same** branch-edge normalization as [src/pt_backend.c](../../src/pt_backend.c) so a
reconstructed trace stays byte-identical to the other tiers. The block-step phase extends
the [single-step plan](zen2-singlestep-trace-plan.md)'s W2 out-of-process ptrace stepper
(and **corrects** its "W3 is blocked on x86" claim); the window phases extend Part I's
shipped LBR backend.

## Implementation status

**Phases 0–5 landed; Phases 6–7 remain forward-look (hardware-blocked).** Build order
followed the Part II priorities: Phase 0 (gating) → Phase 1 (freeze correctness) →
Phase 2 (BTF block-step) → Phase 3 (software-event snapshot) were the P0/near-term work;
Phases 4–5 are the P1 refinements. As of **2026-07-06** all of them ship and are
validated on the Zen 5 dev box (Ryzen 9 9950X, `amd_lbr_v2`):

- **Phase 0 (runtime depth).** `asmtest_amd_lbr_depth()`
  ([src/amd_backend.c](../../src/amd_backend.c)) reads CPUID `0x80000022` EBX[9:4]
  (`lbr_v2_stack_sz`) — the true branch-stack depth — replacing the hardcoded 16 in the
  Tier-A/Tier-B overflow split, the stitch bound, and the LOST heuristic
  ([src/hwtrace.c](../../src/hwtrace.c)). A no-op today (every shipping Zen reports 16 —
  the dev box confirms 16) that removes the assumption. *(The freeze-bit reader this phase
  also scoped shipped earlier with Phase 1.)*
- **Phase 1 (freeze gate)** — landed (see Part II #3).
- **Phase 2 (BTF block-step)** — landed, now including the attached variant
  `asmtest_ptrace_trace_attached_blockstep` (all three public symbols; wrapped in all ten
  bindings), [src/ptrace_backend.c](../../src/ptrace_backend.c).
- **Phase 3 (software-event snapshot)** — landed, including the `snapshot: true`
  begin/end marker-path opt-in ([src/branchsnap.c](../../src/branchsnap.c) +
  [src/hwtrace.c](../../src/hwtrace.c)); see Part II #2.
- **Phase 4 (LbrExtV2 spec filtering)** — landed (below).
- **Phase 5 (stitch decodable-distance guard + ring hardening)** — landed (below).

**Phases 6 (BRS period-adjust, Zen 3 only) and 7 (IBS-Op, Zen 2 / statistical) stay
forward-look**: both require silicon this dev box does not have (Zen 3 BRS; Zen 2 with
`CAP_PERFMON`), so per the house "no untested hardware code" rule they are not
implemented — a lane that cannot self-validate on its target silicon must not ship
unproven. They remain fully specified below for when that hardware is available.

Two **documentation corrections** Part II surfaced shipped alongside Phase 0/2 (see
Deliverables): the `PTRACE_SINGLEBLOCK`-unwired-on-x86 claim in
[zen2-singlestep-trace-plan.md](zen2-singlestep-trace-plan.md), and the "AMD LBR is
finished, no forward-look" framing in [native-tracing.md](../guides/tracing/native-tracing.md) /
[hardware-tracing.md](../guides/tracing/hardware-tracing.md).

## Improvement Phase 0 — Feature detection & gating (`src/hwtrace.c`) *(landed 2026-07-06)*

**Goal.** Detect the per-uarch/per-kernel capabilities the later phases depend on, so
every new lane self-skips cleanly instead of misbehaving on the wrong silicon.

**Work.**
- Add a CPUID reader to the existing `#if defined(__x86_64__)` block near
  `amd_branch_probe` ([src/hwtrace.c:180](../../src/hwtrace.c)). There is **no** reusable
  cpuid helper in hwtrace.c (vendor detection is `/proc/cpuinfo`-based via `vendor_is`,
  [hwtrace.c:120](../../src/hwtrace.c)); reuse the `<cpuid.h>` /
  `__get_cpuid`/`__get_cpuid_count` pattern already used in
  [src/asmtest.c:434](../../src/asmtest.c). Read leaf `0x80000022`: EAX bit 1 =
  `AMD_LBR_V2`, **EAX bit 2 = `AMD_LBR_PMC_FREEZE`** (the freeze-on-PMI gate Phase 1
  needs), EBX = `lbr_v2_stack_sz` (the runtime depth for the P2 depth-detection hygiene
  item).
- **Runtime depth detection.** Replace the hardcoded `AMD_LBR_DEPTH` 16 (duplicated at
  [amd_backend.c:48](../../src/amd_backend.c) and [hwtrace.c:58](../../src/hwtrace.c))
  with a value read from CPUID EBX (fallback 16). Because it drives the Tier-A/Tier-B
  split ([hwtrace.c:568](../../src/hwtrace.c)), the stitch `out_cap`
  ([hwtrace.c:570](../../src/hwtrace.c)), and the overflow flag
  ([amd_backend.c:117](../../src/amd_backend.c)), the decoder must take depth as a
  **parameter** rather than reading a local macro — a cross-TU signature change that must
  be mirrored in the non-Linux stubs ([amd_backend.c:239/261](../../src/amd_backend.c)).
  Ships as a no-op today (every part reports 16) but removes the assumption.
- Add skip-reason strings in `asmtest_hwtrace_skip_reason`
  ([hwtrace.c:245-284](../../src/hwtrace.c)) for any new gated outcome (e.g. a
  freeze-unsupported note), following the existing AMD strings at
  [hwtrace.c:264-274](../../src/hwtrace.c).

**Acceptance.** `make hwtrace-test` still passes on the Zen 5 dev box
(`make docker-hwtrace-amd`); a debug print of the detected freeze bit + depth matches
`amd_lbr_v2` (freeze present, depth 16). No behavior change yet — this phase only adds
detection the later phases consume.

## Improvement Phase 1 — Freeze-on-PMI window-trust gate (`src/hwtrace.c`) *(planned)*

**Goal.** Stop trusting a 16-entry window the hardware never froze. Freeze-on-PMI is
**not** universal on Zen 4 (gated on the Phase-0 CPUID `0x80000022` EAX[2] bit); when
absent, the LBR keeps advancing past the overflow point, so a captured window can silently
*not* end at region exit — yet `hwtrace_end_amd` trusts it today.

**Work.**
- In `hwtrace_end_amd` ([hwtrace.c:449](../../src/hwtrace.c)), when the Phase-0 freeze bit
  is **absent**, treat a Tier-A window whose newest in-region branch does not reach the
  region exit as `truncated` rather than complete (consistent with the existing "no
  in-region branches" truncation at [hwtrace.c:588-589](../../src/hwtrace.c)), so the
  caller re-resolves under `CEILING_FREE` / falls to DynamoRIO.
- Prefer the Phase-3 software-event snapshot (which stops the LBR in software) over PMI
  sampling on freeze-absent parts once Phase 3 lands; until then, the honest `truncated`
  is the correct degrade.
- Keep the probe/capture attr divergence in mind: `amd_branch_probe`
  ([hwtrace.c:186-189](../../src/hwtrace.c)) omits `exclude_hv` while `hwtrace_begin_amd`
  ([hwtrace.c:423-424](../../src/hwtrace.c)) sets it; any attr-touching change here must
  stay mirrored or `available()` and `begin()` disagree.

**Acceptance.** On a freeze-present host (`amd_lbr_v2`, the dev box) behavior is unchanged
and `make docker-hwtrace-amd` still reconstructs the branch-heavy loop; a unit fixture
simulating a freeze-absent, exit-missing window sets `truncated` instead of emitting a
short "complete" trace. The freeze-**absent** branch cannot be exercised live on the Zen 5
box (which reports freeze present), so it ships behind the Phase-0 self-skip + this
synthetic fixture and stays **pending real freeze-absent Zen 4 hardware**, per the house
"no untested hardware code" rule (see Risks).

## Improvement Phase 2 — BTF block-step mode in the W2 ptrace stepper (`src/ptrace_backend.c`) *(landed 2026-07-06)*

> **Status (2026-07-06): LANDED — all three public symbols now ship.** The core
> block-step (`asmtest_ptrace_blockstep_available`, `asmtest_ptrace_trace_call_blockstep`
> and the `blockstep_reconstruct` intra-block walk in
> [src/ptrace_backend.c](../../src/ptrace_backend.c)) landed earlier; the **attached
> variant `asmtest_ptrace_trace_attached_blockstep`** — the deliverable this section
> scoped as its third symbol — now also ships, block-stepping a SEPARATE,
> externally-attached process from its current stop (foreign bytes via
> `process_vm_readv`, target never killed, left stopped past the region for the caller),
> so a live JIT/managed method traces rootless at a fraction of the stops. All three are
> wrapped in **all ten bindings** (parity gate green at 98 symbols) and validated:
> `test_ptrace_attach_blockstep` ([examples/test_hwtrace.c](../../examples/test_hwtrace.c))
> reuses the true-external-attach harness and asserts the block-step stream is
> byte-identical to the per-instruction attached tracer; runs live under a plain
> `docker run` (ptrace of its own child, no caps) in `make docker-hwtrace-amd` (272/272)
> and self-skips on aarch64 / where `PTRACE_SINGLEBLOCK` is unwired.

**Goal.** A complete, exactly-ordered in-region trace at **one `#DB` per taken branch**
(≈ per basic block) instead of one per instruction — on *every* Zen (including Zen 2,
which has no branch hardware), **rootless** (ptrace of your own child; no `CAP_PERFMON`).
This is the correct, hardware-clean form of the recommended block-step tier, and it
**corrects** the single-step plan's claim that `PTRACE_SINGLEBLOCK` is unwired on x86.

**Why it lives in the W2 ptrace backend, not the hwtrace `SINGLESTEP` backend or a new
`asmtest_trace_backend_t` member.** BTF requires setting `DEBUGCTL.BTF`, a ring-0 MSR —
the in-process `ss_backend.c` (EFLAGS.TF) path *cannot* do this. The kernel sets it on the
tracer's behalf only via `PTRACE_SINGLEBLOCK` → `user_enable_block_step()` →
`set_task_blockstep()` (`arch/x86/kernel/step.c`). So block-step is inherently an
**out-of-process ptrace** capability, exactly where W2 lives, and is exposed through the
`asmtest_ptrace_*` surface (as W2 single-step already is) rather than as a hwtrace backend
enum member (the fixed 4-member set at
[asmtest_hwtrace.h:54-60](../../include/asmtest_hwtrace.h) — INTEL_PT / CORESIGHT /
AMD_LBR / SINGLESTEP — is the *in-process/decoder* tier; the W2 stepper is not one of
them).

**Work.**
- Thread a `step_mode` (single-step vs block-step) through the shared bodies
  `trace_attached_impl` ([ptrace_backend.c:764](../../src/ptrace_backend.c)) and the
  `trace_call` tracee loop ([ptrace_backend.c:633](../../src/ptrace_backend.c)) rather than
  duplicating the loop. In block-step mode issue `PTRACE_SINGLEBLOCK` in place of
  `PTRACE_SINGLESTEP`. `PTRACE_SINGLEBLOCK` (request `33`) is frequently **absent from
  glibc's `<sys/ptrace.h>`** even though the x86 kernel wires it, so provide a local
  `#ifndef PTRACE_SINGLEBLOCK` / `#define PTRACE_SINGLEBLOCK 33` (or include
  `<linux/ptrace.h>`) — otherwise the build fails looking like an unsupported host rather
  than a missing constant.
- **Reconstruct intra-block instructions.** A `PTRACE_SINGLEBLOCK` stop lands only at the
  **target** of each taken control transfer — one PC per basic block, not per instruction,
  and `#DB` is trap-class so the stop RIP is the *target*, not the source. To feed the
  *same* `normalize()` / `trace_append_insn` contract
  ([ptrace_backend.c:452](../../src/ptrace_backend.c)) as the per-insn path, walk forward
  from the previous block-entry PC with `asmtest_disas(PTRACE_TRACE_ARCH, …)` lengths
  ([the only reconstruction primitive](../../include/asmtest_trace.h), also used by
  `normalize`), appending each reconstructed insn offset, then feed that **reconstructed
  per-instruction stream** (not the block-entry PCs) to `normalize`, which re-derives
  `blocks[]` — so `blocks[]` parity follows from `insns[]` being correct. This is the
  load-bearing extra work; a decode desync (`asmtest_disas` length 0) sets `truncated`
  exactly as `normalize` does at [ptrace_backend.c:465-468](../../src/ptrace_backend.c).
- **Block-terminator rule — get this exactly right.** BTF traps **only on taken
  transfers**, so the recorded next-stop PC is the taken branch's *target* (an arbitrary
  address, generally **not** the block's fall-through), and a block may contain *not-taken*
  conditional branches mid-run (a not-taken `Jcc` raises no `#DB`). The walk therefore must
  **not** terminate on "PC == next-stop"; it terminates at the control-transfer instruction
  that actually reaches next-stop: unconditional branches / indirect branches / `ret`
  always end the block (their taken target *is* next-stop), while a direct conditional ends
  it only when its static taken target == next-stop — otherwise that `Jcc` was not taken, so
  append it and keep walking the fall-through.
- **Same-target-conditional ambiguity → `truncated`.** Two direct conditionals to the
  *same* target in one block (first not taken, second taken — `je T; …; je T`) are
  statically indistinguishable: BTF gives no signal for *which* was taken, so a greedy
  "first `Jcc` whose target == next-stop" terminates the block early and desyncs `insns[]`.
  When more than one candidate terminator in the straight-line run targets next-stop, treat
  the block as ambiguous and set `truncated` (the block-step analogue of the documented
  POPF/IRET single-step edge) rather than guessing.
- Reuse the two existing arch seams unchanged — `read_pc_ret`
  ([ptrace_backend.c:322](../../src/ptrace_backend.c)) to read the target RIP at each
  `#DB`, and `PTRACE_TRACE_ARCH` ([ptrace_backend.c:302](../../src/ptrace_backend.c)) for
  the Capstone arch — and preserve the initial-in-region-PC capture
  ([ptrace_backend.c:815](../../src/ptrace_backend.c)) so offset 0 is not missed.
- **Filter non-branch stops.** The `#DB` also fires on interrupts/exceptions (AMD APM
  Vol 2 §13.2), so a stop whose RIP is still inside the current straight-line run (not a
  control-transfer target) must be skipped, not treated as a block edge.
- **Arch asymmetry.** `PTRACE_SINGLEBLOCK` is an x86-only kernel feature; on aarch64 there
  is no equivalent, so the block-step entry must fall back to the existing per-insn
  `PTRACE_SINGLESTEP` path (a naive `#define STEP_REQUEST` swap is wrong). Add an x86-only
  `asmtest_ptrace_blockstep_available()` probe distinct from the aarch64 `probe_singlestep`
  self-skip ([ptrace_backend.c:387](../../src/ptrace_backend.c)).
- **Public surface.** Add, mirroring the existing `_call` / `_attached` family:
  `asmtest_ptrace_blockstep_available`, `asmtest_ptrace_trace_call_blockstep`,
  `asmtest_ptrace_trace_attached_blockstep` to
  [include/asmtest_ptrace.h](../../include/asmtest_ptrace.h), with matching ENOSYS stubs in
  the non-supported-host block ([ptrace_backend.c:915](../../src/ptrace_backend.c)) for
  symbol parity. (New signature over an opts-flag on the existing entries, which would
  ABI-break the shipped 4-symbol family.) Follow the `ASMTEST_PTRACE_*` return codes
  ([asmtest_ptrace.h:60](../../include/asmtest_ptrace.h)). **Scoped out:** no
  `_trace_attached_versioned_blockstep` mirror of the JIT/time-correct-bytes lane
  ([ptrace_backend.c:948](../../src/ptrace_backend.c)) — the HW-attributed managed-runtime
  lane is Phase 3's eBPF snapshot; add a versioned block-step only if a rootless
  JIT-on-Zen-2 need surfaces.

**Acceptance.** `make hwtrace-test` (or a new `blockstep-test` mirroring the W2
single-step test) shows the block-step stream is **byte-identical** — `insns[]` and
`blocks[]` — to the per-instruction `PTRACE_SINGLESTEP` path on the existing loop
fixtures, with a materially lower stop count; runs live under a **plain** `docker run` (no
`--cap-add`), and self-skips on aarch64/non-x86 with a clear reason.

## Improvement Phase 3 — Software-event / eBPF on-demand LBR snapshot (`src/hwtrace.c` + optional BPF) *(landed 2026-07-06)*

> **Status (2026-07-06): LANDED — including the marker-path opt-in follow-up.** The
> deterministic capture (`asmtest_amd_snapshot_trace`, [src/branchsnap.c](../../src/branchsnap.c))
> and its substrate probe landed earlier. The **`snapshot: true` opt-in** this section's
> follow-up scoped now ships: the capture is split into `asmtest_amd_snapshot_begin` /
> `asmtest_amd_snapshot_end` (a single process-global armed slot, matching hwtrace.c's
> single-active-region invariant), and `hwtrace_begin_amd`/`hwtrace_end_amd`
> ([src/hwtrace.c](../../src/hwtrace.c)) route the **ordinary region begin/end markers**
> there when `opts.snapshot` is set — deriving the exit breakpoint from the region's last
> `ret` (`amd_last_ret_off`) and falling back to the `sample_period=1` path on any arm
> failure (no BPF toolchain/caps/LbrExtV2, no decodable ret). So a caller gets the
> deterministic boundary snapshot through the plain begin/end surface, not just the
> standalone entry point. Validated: `test_branchsnap` grew a marker-path case (the tiny
> single-shot routine reconstructs its entry block through `begin`/`end`), green in
> `make docker-hwtrace-codeimage`; the clean fallback is asserted in `test_amd_live`
> (`make docker-hwtrace-amd`, built without libbpf → honest sampled result). `opts.snapshot`
> is documented for both backends in [asmtest_hwtrace.h](../../include/asmtest_hwtrace.h).

**Goal.** Read the 16-entry LbrExtV2 stack **deterministically at the region boundary** via
`bpf_get_branch_snapshot()` (`amd_pmu_v2_snapshot_branch_stack`), eliminating the two live
Zen-5 failure modes Part I documents: the tiny single-shot routine "too fast to be sampled
in-region," and the post-routine-glue contamination that forced the richest-in-region
heuristic ([hwtrace.c:465-474, 542-548](../../src/hwtrace.c)). Also the natural
HW-attributed managed-runtime lane on AMD (a uprobe on a JITted method), which today falls
to W2 ptrace single-step.

**Work.**
- Add a BPF program (uprobe/`fentry` at region entry/exit) that calls the snapshot helper
  and delivers the frozen window to userspace, decoded by the existing `asmtest_amd_decode`
  ([amd_backend.c:104](../../src/amd_backend.c)) — no `sample_period=1` ring, no throttle
  exposure, no richest-window scan.
- **Optional-library lane.** Gate the BPF object exactly like the existing codeimage eBPF
  detector: set `-DASMTEST_HAVE_LIBBPF` + skeleton only when `pkg-config libbpf` **and**
  `clang` **and** `bpftool` are all present
  ([mk/native-trace.mk:144-165](../../mk/native-trace.mk)); otherwise compile the TU
  without the `-D`, invoke no BPF toolchain, and have the lane's `available()` return 0
  (self-skip) — the same shape as `CODEIMAGE_SKEL` / `LIBIPT_DEF`.
- **Gates.** Zen 4/5 (perfmon v2) only — Zen 3 BRS and Zen 2 do not route through
  `amd_pmu_v2_handle_irq` and return no snapshot; Linux ≥ 6.10; `CAP_PERFMON` + `CAP_BPF`.
  All expressed as `available()`-time self-skips with reasons.

**Acceptance.** A new `make docker-hwtrace-lbr-snapshot` lane (mirroring
`docker-hwtrace-codeimage` at [mk/docker.mk:254](../../mk/docker.mk): dedicated Dockerfile
with clang+libbpf+bpftool, `--cap-add=BPF --cap-add=PERFMON --cap-add=SYS_PTRACE
--security-opt seccomp=unconfined`, BTF kernel — **not** `--privileged`) captures a **tiny
single-shot routine** that the sampling path marks `truncated`, and reconstructs it
complete. Self-skips cleanly without libbpf / the caps / a Zen 4+ host.

## Improvement Phase 4 — LbrExtV2 speculation-bit filtering (`src/amd_backend.c`) *(landed 2026-07-06)*

> **Status (2026-07-06): LANDED and validated on the Zen 5 dev box.** `amd_replay`
> ([src/amd_backend.c](../../src/amd_backend.c)) now drops a record whose
> `perf_branch_entry.spec == PERF_BR_SPEC_WRONG_PATH` (a speculative, never-retired
> phantom edge) right after the existing `abort` guard — and dropping it is expected, so
> it does **not** set `truncated`. The `spec` bitfield only exists on Linux ≥ 6.1
> headers, so access is gated on `-DASMTEST_HAVE_PERF_BR_SPEC`, set by a
> `-fsyntax-only` struct-member probe in [mk/native-trace.mk](../../mk/native-trace.mk)
> (mirroring `LIBIPT_DEF`); without it the filter compiles out — a no-op, exactly as on
> Zen 3 BRS (retired-only, no spec bits) and non-Linux. `amd_edge_eq` is left untouched
> (its from+to-only overlap semantics the stitcher depends on). Validated
> host-independently by `test_amd_spec_filter`
> ([examples/test_hwtrace.c](../../examples/test_hwtrace.c)): a wrong-path phantom whose
> target would add a spurious block is dropped, leaving a byte-identical trace and no
> truncation — green in `make docker-hwtrace-amd` on the Ryzen 9 9950X.

**Goal.** Drop speculative / wrong-path phantom edges before replay. Today `amd_replay`
filters only aborts (`if (e->abort) continue;`, [amd_backend.c:67-68](../../src/amd_backend.c))
and `amd_edge_eq` ([amd_backend.c:124](../../src/amd_backend.c)) explicitly ignores every
other flag.

**Work.**
- In `amd_replay`, extend the abort guard to also skip records whose
  `perf_branch_entry.spec` == `PERF_BR_SPEC_WRONG_PATH`. Dropping such records is
  *expected*, not a desync, so it must **not** set `truncated` (unlike the four existing
  truncation sites).
- **Kernel-version guard.** The `spec` 2-bit field only exists on newer
  `<linux/perf_event.h>`; gate the field access on struct/kernel availability, not merely
  the `__linux__`/`__x86_64__` block, so older-header builds still compile.
- **Gen gate.** Spec/valid bits are LbrExtV2 (Zen 4/5, Linux ≥ 6.1) only; a no-op on Zen 3
  BRS (retired-only, no spec bits). If wrong-path records are instead filtered during window
  assembly in `hwtrace_end_amd`, do it there — not in `amd_edge_eq`, whose from+to-only
  overlap semantics the stitcher depends on.

**Acceptance.** `make docker-hwtrace-amd` reconstructs a branch fixture containing a
synthesized wrong-path entry without the phantom edge; behavior is unchanged where the
`spec` field is unavailable or all entries are correct-path.

## Improvement Phase 5 — Tier-B stitch hardening + throttle config (`src/amd_backend.c`, `src/hwtrace.c`) *(landed 2026-07-06)*

> **Status (2026-07-06): LANDED and validated on the Zen 5 dev box.** Three parts ship:
> (1) **Decodable-distance guard.** `asmtest_amd_stitch`
> ([src/amd_backend.c](../../src/amd_backend.c)) now takes `base`/`base_ip`/`len` (threaded
> through the [hwtrace.c](../../src/hwtrace.c) call site and the non-Linux stub) and, before
> accepting a smallest-overlap shift, checks that the adjacency it would splice — the tail's
> newest branch target → the first newly-appended branch source — is real straight-line code
> (`amd_span_decodable`: a forward Capstone length-walk that lands exactly on the source).
> An indecodable splice is rejected in favor of a larger shift, else an honest gap.
> **Honest scope (corrects this plan's earlier overclaim):** on an *internally-consistent*
> hardware branch-stack window, from+to adjacency already implies byte adjacency, so the
> guard is a no-op there; it does **not** — and byte-level decodability *cannot* — catch a
> control-flow *phase* alias (a byte-decodable-but-wrong stitch). What it does catch is a
> **dropped/throttled-sample** mis-stitch whose smallest-overlap match splices non-contiguous
> edges (a backwards/overshoot span), converting a silently-wrong stitch into an honest gap —
> complementing the replay-side desync→`truncated` already downstream. (2) **Ring hardening.**
> The AMD data ring default grew 64KB → 256KB ([hwtrace.c](../../src/hwtrace.c)), extending
> gapless stitch reach before the kernel drops the newest samples; the
> `PERF_RECORD_LOST`/throttle → `lost` detection is unchanged. (3) **`data_size` doc fix.**
> The [asmtest_hwtrace.h](../../include/asmtest_hwtrace.h) comment now documents both backend
> defaults (Intel PT 8KB, AMD 256KB) instead of the stale single "0=8KB". Validated by
> `test_amd_stitch_decodable` ([examples/test_hwtrace.c](../../examples/test_hwtrace.c)) — a
> decodable contiguous splice is accepted, an indecodable one yields an honest gap — plus the
> existing stitch/drain fixtures staying green (no over-rejection); `make docker-hwtrace-amd`
> on the Ryzen 9 9950X.

**Goal.** Reduce silent mis-stitches and extend Tier-B reach before honest truncation,
without pretending an unavailable HW facility exists (Intel's `hw_idx`-based stitch cannot
port — AMD sets `hw_idx ≡ 0`; this is a confirmed dead end).

**Work.**
- **Decodable-distance invariant.** In `asmtest_amd_stitch`'s smallest-overlap loop
  ([amd_backend.c:173-188](../../src/amd_backend.c)), after a from+to overlap `match` and
  before accepting the shift, require that the straight-line span between consecutive
  stitched edges is Capstone-decodable with reconstructed instruction count == static decode
  distance; reject the shift and keep searching otherwise. This needs `base`/`base_ip`/`len`
  threaded into `asmtest_amd_stitch` (which has no code access today) — a signature change
  mirrored in the non-Linux stub ([amd_backend.c:249](../../src/amd_backend.c)) and the call
  site ([hwtrace.c:576](../../src/hwtrace.c)), and it inherits the Capstone
  decoder-present gating.
- **Ring / throttle hardening.** Grow the AMD data ring (`g_opts.data_size` / `round_pages`
  default `64*1024`, [hwtrace.c:433](../../src/hwtrace.c)) and document requiring
  `kernel.perf_event_max_sample_rate` raised and `kernel.perf_cpu_time_max_percent=0` on the
  self-hosted runner. Must **not** remove the `PERF_RECORD_LOST` / `PERF_RECORD_THROTTLE` →
  `lost` detection ([hwtrace.c:503-505](../../src/hwtrace.c)), which is the only "run did not
  fit" signal surviving windows cannot show. Fix the `data_size` header comment
  ([asmtest_hwtrace.h:65](../../include/asmtest_hwtrace.h)), which says `0=8KB` but is
  backend-dependent — the AMD default is 64 KB ([hwtrace.c:433](../../src/hwtrace.c)) while
  Intel PT keeps 8 KB ([hwtrace.c:645](../../src/hwtrace.c)); document **both** defaults
  rather than flipping the single number (which would then be wrong for Intel PT).

**Acceptance.** A synthetic self-overlapping-loop fixture that the current smallest-overlap
heuristic mis-stitches now either stitches correctly or reports an honest gap (`truncated`);
`make docker-hwtrace-amd`'s 20 000-trip loop reconstructs further before truncating with the
enlarged ring.

## Improvement Phase 6 — BRS period-adjust single-window Tier-A capture (`src/hwtrace.c`) *(forward-look)*

**Goal.** On **Zen 3 BRS**, for a routine known to take ≤ ~16 taken branches, replace the
`sample_period=1` PMI-per-branch flood with a single fixed-period frozen overflow (the
kernel already programs `period − lbr_nr` via `amd_brs_adjust_period`), cutting Tier-A
capture from O(branches) interrupts to one and removing the throttle/ring exposure.

**Work / caveat.** `sample_period=1` is **load-bearing** for the current design: the Tier-B
stitch and the richest-in-region heuristic ([hwtrace.c:477-486, 584-614](../../src/hwtrace.c);
[amd_backend.c:130-135](../../src/amd_backend.c)) assume a sample at *every* taken branch so
consecutive 16-deep windows overlap by 15 edges. A fixed BRS period breaks that overlap. So
this is **not** a blanket change to `hwtrace_begin_amd` ([hwtrace.c:426](../../src/hwtrace.c))
— it must be a **distinct Tier-A capture mode**, selected only when the region is known-small
and the Tier-B path is not needed, and kept off Zen 4/5 (where the Phase-3 software-event
snapshot is the better lever). The `period=1` appears in two places (probe
[hwtrace.c:186](../../src/hwtrace.c), capture [hwtrace.c:420](../../src/hwtrace.c)); a mode
switch must keep them coherent or justify the divergence.

**Acceptance.** *(forward-look — deferred until a Zen 3 host is available; the dev box is
Zen 5.)* On a Zen 3 BRS host, a ≤16-branch routine reconstructs identically to the
`sample_period=1` path with a single PMI; the Tier-B stitch path is untouched.

## Improvement Phase 7 — IBS-Op complementary coverage lane (`src/hwtrace.c` + new backend) *(forward-look)*

**Goal.** The **only** hardware branch source on **Zen 2** (which has no branch stack): a
statistical, precise-IP source→target edge via `MSR_AMD64_IBSBRTARGET` (gated on
`IBS_CAPS_BRNTRGT`), used as a *coverage-confirmer* / hot-edge pre-cover to shrink — not
bound — the Phase-2 block-step / DynamoRIO residual, and as an indirect-branch-target
resolver on Zen 3/4/5.

**Work / caveat.** IBS is **statistical** (one tagged op per NMI, worse edge-yield than
LBR) and cannot produce an ordered, complete path, so it is explicitly *not* a replacement
for the branch stack and must never feed the `insns[]`/`blocks[]` parity contract as if
complete. It needs `CAP_PERFMON`/`CAP_SYS_ADMIN` (no user/kernel filter). If exposed, it is
a *separate diagnostic producer*, not a member of the fidelity cascade. Two raw proposals
were refuted in verification and must be avoided: `rand_en` is an IBS-*Fetch* knob (not Op),
and the branch target is capability-gated and valid only for retired taken branches.

**Acceptance.** *(forward-look — deferred; low priority.)* On a Zen 2 host, IBS-Op edges
mark a subset of region basic blocks as covered and reduce the number of blocks the
block-step fallback must walk; self-skips without `IBS_CAPS_BRNTRGT` / `CAP_PERFMON`.

## Deliverables (Improvement Phases 0–5)

*As shipped (2026-07-06). Two small design deltas from the original decomposition, both
noted inline: runtime depth is a shared cached accessor rather than a decode-function
parameter (lower blast radius, no ABI churn on the internal decode signatures — depth is
a pure cached function of the CPU, not a mutable global), and the freeze/depth CPUID
readers live in `amd_backend.c` (`asmtest_amd_freeze_available` / `asmtest_amd_lbr_depth`,
next to the other CPUID probes) rather than `hwtrace.c`.*

- **hwtrace.c:** consumes `asmtest_amd_lbr_depth()` (runtime, from CPUID `0x80000022`
  EBX[9:4]) in place of the hardcoded `AMD_LBR_DEPTH` for the Tier-A/B split, stitch cap,
  and LOST heuristic; freeze-gated Tier-A `truncated` logic in `hwtrace_end_amd`; AMD data
  ring default 64KB → 256KB; the software-event snapshot capture path; threads
  `base`/`base_ip`/`len` into the `asmtest_amd_stitch` call.
- **amd_backend.c:** spec/wrong-path filter in `amd_replay` (gated on
  `ASMTEST_HAVE_PERF_BR_SPEC`); `base`/`base_ip`/`len`-threaded `asmtest_amd_stitch` with
  the decodable-distance guard (`amd_span_decodable`); `asmtest_amd_lbr_depth()` cached
  CPUID accessor and the runtime-depth overflow check in `asmtest_amd_decode` — all
  mirrored in the non-Linux stub block and hwtrace.c's forward decls.
- **ptrace_backend.c + asmtest_ptrace.h:** three public block-step symbols —
  `asmtest_ptrace_blockstep_available`, `asmtest_ptrace_trace_call_blockstep`, and
  `asmtest_ptrace_trace_attached_blockstep` (the attached variant reuses the
  `blockstep_reconstruct` intra-block walk and `classify_region_exit` call-out handling,
  reading foreign bytes via `process_vm_readv` exactly as `trace_attached_impl` does) — each
  with a non-supported-host stub. *(Delta from the original sketch: the block-step loops are
  their own functions rather than a `step_mode` flag threaded through the single-step bodies;
  the reconstruction differs enough — trap-class target vs. per-instruction RIP — that
  sharing the loop obscured more than it saved.)*
- **branchsnap.c + hwtrace.c:** the `snapshot: true` marker-path opt-in —
  `asmtest_amd_snapshot_begin` / `_end` (armed single-slot) called from
  `hwtrace_begin_amd`/`hwtrace_end_amd`, with `amd_last_ret_off` deriving the exit
  breakpoint and a clean fallback to the sampled path.
- **Build/test:** the block-step host + attached-block-step tests
  ([examples/test_hwtrace.c](../../examples/test_hwtrace.c)) run under a plain `docker run`;
  the snapshot marker-path test in [examples/test_branchsnap.c](../../examples/test_branchsnap.c)
  (BPF lane); all self-skipping. *(The `docker-hwtrace-lbr-snapshot` split-Dockerfile lane was
  not needed: the existing `docker-hwtrace-codeimage` BPF-toolchain image already builds +
  runs branchsnap-test.)*
- **Bindings:** the three `asmtest_ptrace_*` block-step symbols wrapped (whole-word) in each
  of the 10 bindings' `hwtrace.*` wrappers so
  [scripts/check-bindings-parity.sh](../../scripts/check-bindings-parity.sh) stays green (98
  tier symbols × 10 bindings); `asmtest_amd_snapshot_trace` and the whole-window trio carry
  allow-list exemptions (C-level / managed-tier reach). No new `asmtest_trace_choice_t` field
  (the `3 * sizeof(int)` static-assert at
  [asmtest_trace_auto.h:83-84](../../include/asmtest_trace_auto.h) holds).
- **Docs:** correct the `PTRACE_SINGLEBLOCK` claim in
  [zen2-singlestep-trace-plan.md](zen2-singlestep-trace-plan.md) (Phase 2 makes it real);
  update the "AMD LBR is finished" framing in [native-tracing.md](../guides/tracing/native-tracing.md) /
  [hardware-tracing.md](../guides/tracing/hardware-tracing.md); refresh the
  [trace-parity-matrix](../analysis/trace-parity-matrix.md) AMD rows.

## Validation (per lane, self-skipping)

- **Host, privilege-free:** `make hwtrace-test` and the block-step test run under a plain
  `docker run` (Phase 2 needs only ptrace of its own child) and self-skip on
  non-x86/absent-capability hosts — the first AMD-adjacent lane that runs *live* on ordinary
  CI rather than needing bare-metal AMD.
- **Bare-metal AMD:** `make docker-hwtrace-amd` (`--security-opt seccomp=unconfined
  --cap-add=PERFMON`) validates Phases 1, 4, 5 on the Zen 5 dev box; Phase 3 adds
  `docker-hwtrace-lbr-snapshot` (BPF/PERFMON/PTRACE caps).
- **Cross-tier parity:** every reconstructed trace is asserted byte-identical (`insns[]` +
  `blocks[]`) to the existing single-step / DynamoRIO output on shared fixtures, using the
  same branch-edge normalization as `pt_backend.c` — the parity harness Part I already
  established.
- **Binding parity:** `make check-bindings-parity` stays green after the three new ptrace
  symbols land.

## Risks and open points (AMD improvements)

- **Binding-parity cost of block-step.** Three new `asmtest_ptrace_*` symbols must be wrapped
  in all 10 bindings or the gate fails; an intentional gap needs a
  [bindings-parity-allow.txt](../../scripts/bindings-parity-allow.txt) entry (stale entries
  also fail). Prefer wrapping over exempting.
- **Block-step reconstruction fidelity.** The intra-block insn reconstruction (walking
  `asmtest_disas` lengths between `#DB` targets) is the load-bearing new work; get it wrong
  and `insns[]` diverges from the other tiers while `blocks[]` can still look right. The
  byte-parity acceptance test is the guard, and the block-terminator rule +
  same-target-conditional ambiguity (both spelled out in Phase 2) are the specific traps.
  Self-modifying / non-well-behaved routines (POPF/IRET, in-routine signals), and the
  ambiguous same-target-`Jcc` block, remain the documented edge and set `truncated`.
- **BRS period vs stitching conflict (Phase 6).** A fixed BRS period breaks the
  `sample_period=1` overlap the Tier-B stitch and richest-in-region heuristic depend on; it
  must be a separate Tier-A-only mode, hence forward-look.
- **`spec`-field kernel gating (Phase 4).** The field is newer-kernel-only; access must be
  struct-availability-guarded, not just arch-guarded.
- **base_ip remap (Phase 5).** `amd_replay` assumes captured branch IPs live at the same
  virtual address as the `base` buffer it disassembles; a mismatch silently yields a
  degenerate trace with no `truncated`. Any record-stream change must preserve that IP-space
  identity.
- **Single-active-region invariant.** All AMD capture state
  (`g_fd`/`g_base_map`/`g_active`, [hwtrace.c:610](../../src/hwtrace.c)) is a single
  process-global slot reset in `hwtrace_end_amd`; any new freeze/snapshot state must be
  single-instance and reset there too.
- **Hardware coverage.** Live validation exists only on Zen 5 (`amd_lbr_v2`). Zen 3 BRS
  (Phase 6) and freeze-absent Zen 4 (Phase 1) paths are code-implemented and
  self-skip-validated but remain **pending real hardware**, per the house "no untested
  hardware code" rule.
