# asm-test — AMD LBR snapshot native-trace backend: implementation plan

A phased roadmap for an **AMD-specific** hardware-assisted native-trace backend
built on AMD's branch-record facilities — **BRS** (Zen 3 / Fam19h) and
**LbrExtV2** (Zen 4) — exposed through Linux perf `PERF_SAMPLE_BRANCH_STACK`. Like
the Intel PT backend it produces `asmtest_trace_t` offsets reusing the same
registered-region begin/end markers: instruction offsets identical to the Unicorn
emulator, the DynamoRIO native tier, and Intel PT, and block offsets that match
after the same branch-edge normalization step.

This plan is a **sibling** of the
[hardware-trace plan](hardware-trace-plan.md) (Intel PT / ARM CoreSight) and of the
[DynamoRIO native-trace plan](dynamorio-native-trace-plan.md). It exists because of
a hardware fact: **AMD has no Intel PT equivalent** — there is no continuous,
compressed control-flow trace ring on any Zen part. The closest achievable native
trace on AMD is *reconstruction from the shallow (16-entry) branch-record stack*.
This plan specifies that closest-achievable backend, its hard limits, and the
DynamoRIO fallback for everything beyond them.

> Status legend: **planned** unless noted. Update this file as phases land, the way
> [hardware-trace-plan.md](hardware-trace-plan.md) and
> [inline-asm-keystone-plan.md](inline-asm-keystone-plan.md) track theirs.

---

## The governing constraint

Intel PT records *every* branch decision continuously into a kernel AUX ring you
drain at `end()`, so the decoder reconstructs a complete instruction path of
arbitrary length at near-zero overhead. AMD has no such ring. Its branch-record
facilities are a **fixed-depth (16-entry) stack** — the last 16 taken branches —
captured into a perf *sample* on a PMU interrupt:

- **BRS** — AMD Family 19h "Zen 3", PPR §2.1.13: up to 16 consecutive taken
  branches; opt-in; surfaced via perf `branch-brs` / `PERF_SAMPLE_BRANCH_STACK`.
- **LbrExtV2** — Zen 4, mainline since Linux 6.1: a 16-deep Last Branch Record with
  hardware branch filtering (kernel/user/cond/call/ret/...), freeze-on-PMI, and
  branch-speculation flags, surfaced through the same perf branch-stack API.
- **IBS** (Instruction-Based Sampling) is the third AMD facility but is purely
  *statistical* — a coverage histogram, never an ordered complete path — so it is
  out of scope for parity offsets and is not pursued here.

From this follows the **fundamental result this plan is bounded by**: on AMD you can
match Intel PT on trace **completeness** *or* on near-zero **overhead**, but not
both for an arbitrary routine. The two operating points:

- **Tier A — routines within the 16-taken-branch window.** A single branch-stack
  snapshot holds *all* of the routine's taken branches, so reconstruction is
  **exact and complete** — bit-for-bit identical `insns[]`/`blocks[]` to the PT,
  DynamoRIO, and Unicorn backends — at small absolute overhead (a handful of PMIs).
  **This is the backend this plan ships.**
- **Tier B — longer / looping routines.** Completeness requires *stitching*
  overlapping 16-entry stacks sampled at every taken branch (`sample_period = 1`),
  which keeps completeness but sacrifices the low-overhead property. Forward-look
  only (Phase 5); the supported path beyond the window is the **DynamoRIO
  fallback**, which already produces the most accurate and detailed trace on AMD.

---

## Implementation status

**Planned — nothing built yet.** Today AMD is handled correctly by *self-skip*:
[`cpu_matches`](../../src/hwtrace.c) pins Intel PT to `GenuineIntel`, so
`asmtest_hwtrace_available(INTEL_PT)` returns 0 on AMD ("no intel_pt PMU") and the
caller falls through to the DynamoRIO native tier (fully validated on the AMD dev
host) or the Unicorn emulator. This plan is **additive**: it introduces a new
`ASMTEST_HWTRACE_AMD_LBR` backend that, when present and within its window, gives a
hardware-native trace on AMD; it does **not** change the Intel-PT gating, which is
correct and must stay.

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
- Replace the PMU-node probe: AMD branch records are **not** a `pmu_type()` AUX PMU
  like `intel_pt`. The real capability probe is *attempt-and-close* — open a
  sampling `perf_event` with `branch_sample_type = PERF_SAMPLE_BRANCH_USER |
  PERF_SAMPLE_BRANCH_ANY` and `sample_period = 1`; success means LBR/BRS is usable,
  `EINVAL`/`EACCES` means skip. Distinguish Zen 3 BRS (needs the opt-in `branch-brs`
  event) from Zen 4 LbrExtV2 (works with a generic sampling event) by trying
  LbrExtV2 first and falling back to the BRS event.
- `asmtest_amd_decoder_present()` returns 1 only when the backend TU is compiled in
  (it depends on Capstone for the instruction-length walk — see Phase 2).
- Extend `asmtest_hwtrace_skip_reason()` with AMD-specific strings ("not an
  AuthenticAMD x86-64 host", "no AMD branch records (needs Zen 3 BRS / Zen 4
  LbrExtV2)", "perf branch-stack not permitted").

**Acceptance.** `make hwtrace-test` self-skips on Intel, on AMD without privilege,
and on non-AMD hosts, each with the specific reason; returns available only on a
Zen 3+/Zen 4 host with perf branch sampling permitted.

---

## Phase 1 — Branch-record capture *(planned)*

**Goal.** Capture the routine's taken-branch records into a host-side buffer around
the begin/end markers, on the calling thread (`pid=0`).

**Key divergence from the PT capture path.** Intel PT records flow through the
**AUX** mmap; AMD branch records flow through the **base (data)** ring as
`PERF_RECORD_SAMPLE` records carrying a `struct perf_branch_stack` (`__u64 nr` +
`struct perf_branch_entry entries[]`, each `{from, to, flags}`). So
[hwtrace.c](../../src/hwtrace.c)'s `aux_offset`/`aux_size` mmap is **not** used for
this backend; instead `begin()` mmaps only the data ring and `end()` walks
`PERF_RECORD_SAMPLE` records out of it.

**Capture strategy (Tier A).** Set `sample_period = 1` on a taken-branch event so a
sample is emitted at *every* taken branch. Because a Tier-A routine has ≤16 taken
branches, this is only a handful of interrupts (small absolute overhead) and
sidesteps the "no clean snapshot-at-`end()`" problem entirely: the sample emitted at
the routine's *last* in-region branch already holds the complete ≤16-entry history
(freeze-on-PMI keeps it consistent on LbrExtV2). `end()` selects the in-region
sample with the greatest coverage and hands its branch array to the decoder.

- Use `PERF_SAMPLE_BRANCH_USER` to scope to EL0 userspace (drop kernel/runtime
  noise), matching PT's `exclude_kernel`.
- The `attr.disabled=1` → `IOC_ENABLE`/`IOC_DISABLE` bracket from
  [`asmtest_hwtrace_begin`/`end`](../../src/hwtrace.c) is reused unchanged.

**Acceptance.** For a registered routine with a known handful of taken branches, the
captured branch array on a Zen 3+/Zen 4 host contains exactly those `from→to`
pairs, in order, ending at the region exit.

---

## Phase 2 — LBR reconstruction backend (`src/amd_backend.c`) *(planned)*

**Goal.** Turn the ordered `perf_branch_entry[]` into the same offset stream the PT
backend produces, by replaying asm-test's registered bytes between branches.

**Decoder interface.** PT's decoder takes raw `(aux, aux_len, base, len, trace)`;
the AMD decoder takes `(const struct perf_branch_entry *br, size_t nbr, base, len,
trace)`. Add `asmtest_amd_decode(...)` alongside `asmtest_pt_decode` /
`asmtest_cs_decode` in [hwtrace.c](../../src/hwtrace.c)'s dispatch.

**Reconstruction.** Unlike libipt (which carries its own instruction decoder), the
AMD path must walk instructions itself to find boundaries and offsets, so it
**reuses the project's existing Capstone layer** (the same dependency
[src/disasm.c](../../src/disasm.c) already uses for annotation):

1. Start at the region entry (`base` / the begin marker PC). Branch records give
   ordered `from_i` (the branch instruction's address) and `to_i` (its target).
2. For each branch *i*: linearly decode instructions with Capstone from the current
   IP up to and including `from_i`, appending each in-range offset via
   `trace_append_insn`; then continue at `to_i`.
3. Mark a block start at the entry and after every branch (i.e. at every `to_i`) —
   **identical normalization to [pt_backend.c](../../src/pt_backend.c)** — via
   `trace_append_block`, so block offsets match Unicorn/DR/PT.
4. Filter mispredicted/aborted/speculative entries using the `perf_branch_entry`
   flags (LbrExtV2 reports speculation), the AMD analogue of PT's
   `insn.speculative` filter.

**Gating.** Compile under `-DASMTEST_HAVE_AMD_LBR` (predicated on Capstone being
present, since reconstruction needs the length-decoder). Without it the TU compiles
decoder-free and `asmtest_amd_decoder_present()` returns 0, so
`asmtest_hwtrace_available(AMD_LBR)` self-skips — exactly like the libipt/OpenCSD
gating.

**Acceptance.** For a deterministic, single-threaded, ≤16-taken-branch registered
routine, `insns[]` is byte-for-byte identical to the Unicorn/DynamoRIO/PT output
for the same code, and block offset 0 is recorded.

---

## Phase 3 — Block-boundary parity *(planned)*

**Goal.** Prove the normalized block partition matches the other backends.

This is the same non-trivial step the PT plan budgets: an LBR/PT "block" implied by
consecutive branches is coarser than a Unicorn/DR basic block, so blocks are
recomputed from the reconstructed `insns[]` by splitting at every branch *target*
and after every branch *instruction*. Unit-test the AMD backend's `blocks[]` against
the DynamoRIO tier's block set for the same routine (cross-backend parity test,
reusing the PT parity harness).

**Acceptance.** AMD `blocks[]` == DynamoRIO `blocks[]` == PT `blocks[]` for the
shared fixture routines (within the Tier-A window).

---

## Phase 4 — Window-overflow truncation & DynamoRIO fallback routing *(planned)*

**Goal.** Make the 16-branch ceiling *safe and honest*, never silently wrong.

- **Detect overflow.** If the routine executed more taken branches than the stack
  depth (16), the snapshot is incomplete. Detect this — the captured array is full
  (`nr == depth`) and its earliest `to` does not reach the region entry — and set
  `trace->truncated = true` (the same loss bit PT uses for `OVF`/AUX-full). Never
  emit a partial trace as if complete.
- **Route the fallback.** Document and (where a caller orchestrates backends) wire
  the rule: AMD_LBR is attempted for within-window routines; on overflow or
  unavailability the trace is produced by the **DynamoRIO native tier** (or Unicorn),
  which has no depth ceiling and is the most accurate/detailed AMD trace. The
  hardware tier is a fast/non-perturbing *complement* for small routines, never a
  replacement.

**Acceptance.** A routine that exceeds 16 taken branches sets `truncated` (and the
orchestrating caller falls back to DynamoRIO); a within-window routine does not.

---

## Phase 5 — Tier B (stitching) & MSR-direct snapshot *(forward-look)*

Two optional extensions, neither required for the shippable Tier-A backend:

- **LBR stitching for arbitrary length.** Sample at every taken branch
  (`sample_period = 1`) and splice the overlapping 16-entry stacks (15-entry
  overlap) into one gapless taken-branch sequence, reconstructing a complete trace
  past the window. Keeps completeness, loses near-zero overhead, and risks perf
  sample-rate throttling (`kernel.perf_event_max_sample_rate`) dropping ≥16
  consecutive samples → a real gap. Only worth it for hardware-attributed traces of
  long routines where DynamoRIO is undesirable; otherwise DynamoRIO is simpler and
  more reliable.
- **MSR-direct snapshot.** Read the LBR/BRS MSRs directly around the region to get
  an exact Tier-A snapshot with *zero* interrupts (vs Phase 1's per-branch PMIs).
  Needs privilege (a kernel helper / `CAP_SYS_ADMIN` or a tiny module), so it is a
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
- Makefile knobs mirroring `LIBIPT_*`: `-DASMTEST_HAVE_AMD_LBR` (predicated on
  Capstone), `amd_backend.o` in `HWTRACE_OBJS`, and the PIC variant in
  `libasmtest_hwtrace`.
- A cross-backend parity unit test (AMD vs DynamoRIO vs PT block/insn sets) added to
  the `hwtrace-test` fixtures.

## Validation (AMD branch records)

- AMD BRS: Fam19h Zen 3, PPR §2.1.13 (16-deep taken-branch sampling); Linux
  `PERF_EVENTS_AMD_BRS`, opt-in `branch-brs` event.
- AMD LbrExtV2: Zen 4, mainline Linux 6.1+; 16-deep LBR with hardware branch
  filtering, freeze-on-PMI, and speculation flags via `PERF_SAMPLE_BRANCH_STACK`.
- Capture needs `perf_event_paranoid` lowered or `CAP_PERFMON`, as with PT.
- Cannot run on standard CI (cloud VMs do not reliably expose AMD LBR/BRS to
  guests); a self-hosted bare-metal Zen 3+/Zen 4 runner is required, with the same
  standing operational cost and security caveats the PT plan documents.
  - AMD Fam19h BRS (LWN): https://lwn.net/Articles/877245/
  - `CONFIG_PERF_EVENTS_AMD_BRS`: https://cateee.net/lkddb/web-lkddb/PERF_EVENTS_AMD_BRS.html
  - AMD LbrExtV2 (LWN): https://lwn.net/Articles/904482/
  - AMD Zen 4 LbrExtV2 (Phoronix): https://www.phoronix.com/news/AMD-Zen-4-LbrExtV2

## Risks and open points (AMD LBR)

- **The 16-branch ceiling is the defining limit.** Most real routines loop past it;
  Tier A is genuinely useful only for small, branch-light registered routines —
  which is exactly asm-test's common case, but must be stated, not assumed. Beyond
  it, DynamoRIO is the answer, not this backend.
- **No clean "snapshot now."** Solved for Tier A by `sample_period = 1` (a sample at
  every branch, cheap because branches are few); if a future variant uses a coarse
  period instead, the in-region sample may miss the final branches — verify the
  selected sample actually reaches the region exit before trusting it.
- **Reconstruction needs the exact registered bytes** (like PT) *and* a correct
  length-decoder (Capstone); self-modifying or relocated code silently corrupts
  offsets. Speculative/mispredicted entries must be filtered via the
  `perf_branch_entry` flags before recording.
- **Vendor/uarch detection is coarser than PT's PMU node.** There is no single
  sysfs `type` file; capability is established by attempting the branch-stack open.
  Distinguishing Zen 3 BRS (opt-in event) from Zen 4 LbrExtV2 (generic event) is a
  try-and-fallback, which must be unit-tested on both uarches or documented as
  Zen 4-first.
- **Completeness vs overhead is unresolvable on AMD.** This backend deliberately
  picks completeness-for-small-routines; the plan does **not** claim PT parity for
  arbitrary code, and Phase 5's stitching is explicitly the worse-overhead trade.
