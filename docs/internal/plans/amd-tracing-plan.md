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
[DynamoRIO native-trace plan](../archive/plans/dynamorio-native-trace-plan.md).

> Status legend: **planned** unless noted; forward-look phases are tagged
> *(forward-look)*. Update this file as phases land, the way
> [hardware-trace-plan.md](hardware-trace-plan.md) and
> [inline-asm-keystone-plan.md](../archive/plans/inline-asm-keystone-plan.md) track theirs. The house
> rule holds: **no untested hardware code** — a lane that cannot self-validate on its
> target silicon self-skips (`available()` → 0) rather than shipping unproven.

> **Status (2026-07-12): everything landed except one hardware-blocked phase.**
> Part I (the LBR snapshot backend, Phases 0–5) shipped; Part III Improvement
> Phases 0–5 and 8–9 landed and are validated on the Zen 5 dev box, and Phase 7
> (IBS-Op) is **superseded + landed** via
> [zen2-ibs-tracing-plan.md](../archive/plans/zen2-ibs-tracing-plan.md). The single open item is
> **Improvement Phase 6 (BRS period-adjust)** — hardware-blocked on a Zen 3 host
> neither dev box provides, per the house rule above — which is why this plan
> stays in `plans/` rather than `archive/plans/` until that silicon is available.

> **Amended 2026-07-16 — the deterministic snapshot is MULTI-exit now; several sections
> below still say "single-exit".** Landed in `e9ca70e`: `hwtrace_begin_amd` plants **one HW
> execution breakpoint per region exit, up to `ASMTEST_AMD_MAX_EXITS == 4`**
> ([src/amd_backend.h:57](../../../src/amd_backend.h)) via `asmtest_amd_all_exits` +
> `asmtest_amd_snapshot_begin_multi` ([src/amd_backend.h:69](../../../src/amd_backend.h)),
> so **whichever** ret/tail-call a multi-exit routine leaves through hits a boundary. The
> default-on gate is now `amd_nexit >= 1 && … amd_nexit <= ASMTEST_AMD_MAX_EXITS`
> ([src/hwtrace.c:777-783](../../../src/hwtrace.c) — **not** `hwtrace.c:655`, the line this
> plan cites throughout; the file has moved under it). A region with **>4** exits, or any
> arm failure, still falls through to the sampled path unchanged. **This partly obsoletes
> Improvement Phase 8's stated rationale** — the MSR rung is justified below as covering
> "the multi-exit tiny routine the snapshot cannot", which now holds only for **>4**-exit
> regions; the rung itself remains correct and shipped for that residual plus every
> arm-failure fallback (see the amendment at Phase 8). The *hardware* status is unchanged:
> **Improvement Phase 6 (BRS period-adjust) remains the one open item, hardware-blocked on
> Zen 3 (Family 19h) BRS silicon** — the dev boxes are a Ryzen 9 9950X (Family 1Ah, Zen 5)
> and a Ryzen 9 4900HS (Family 17h, Zen 2), so neither can validate it.

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

The 16-entry stack ([`AMD_LBR_DEPTH`](../../../src/amd_backend.c), duplicated at
[src/hwtrace.c:58](../../../src/hwtrace.c)) is a **silicon ceiling**, not a software
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
([include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h)) ships with: the AMD
vendor + branch-stack-probe gating in [src/hwtrace.c](../../../src/hwtrace.c)
(`amd_branch_probe`, `vendor_is("AuthenticAMD")`, AMD `skip_reason` strings); the
data-ring (non-AUX) `PERF_SAMPLE_BRANCH_STACK` capture (`hwtrace_begin_amd` /
`hwtrace_end_amd`); and the reconstruction backend
[src/amd_backend.c](../../../src/amd_backend.c) (`asmtest_amd_decode`), which replays the
registered bytes through the Capstone layer (`asmtest_disas`) between branch records,
normalizes blocks at branch edges (identical to
[pt_backend.c](../../../src/pt_backend.c)), filters aborted entries, and flags
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
[src/amd_backend.c](../../../src/amd_backend.c), proven by `test_amd_stitch` against an
18-iteration loop past the 16-deep window); only the live multi-sample capture wiring
(bounded by perf ring size + throttling) and the MSR-direct snapshot remain forward-look
(see Phase 5 below).

---

## Phase 0 — Vendor gating & feature detection *(LANDED)*

**Goal.** Extend the detect-and-skip chain so `asmtest_hwtrace_available(AMD_LBR)`
returns 1 only on an AMD host that actually exposes usable branch records.

**Work.**

- Add `ASMTEST_HWTRACE_AMD_LBR` to `asmtest_trace_backend_t` in
  [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h).
- Add an `amd_matches()` arm to [`cpu_matches`](../../../src/hwtrace.c): require
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

## Phase 1 — Branch-record capture *(LANDED)*

**Goal.** Capture the routine's taken-branch records into a host-side buffer around the
begin/end markers, on the calling thread (`pid=0`).

**Key divergence from the PT capture path.** Intel PT records flow through the **AUX**
mmap; AMD branch records flow through the **base (data)** ring as `PERF_RECORD_SAMPLE`
records carrying a `struct perf_branch_stack` (`__u64 nr` + `struct perf_branch_entry
entries[]`, each `{from, to, flags}`). So [hwtrace.c](../../../src/hwtrace.c)'s
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
  [`asmtest_hwtrace_begin`/`end`](../../../src/hwtrace.c) is reused unchanged.

**Acceptance.** For a registered routine with a known handful of taken branches, the
captured branch array on a Zen 3+/Zen 4 host contains exactly those `from→to` pairs, in
order, ending at the region exit.

---

## Phase 2 — LBR reconstruction backend (`src/amd_backend.c`) *(LANDED)*

**Goal.** Turn the ordered `perf_branch_entry[]` into the same offset stream the PT
backend produces, by replaying asm-test's registered bytes between branches.

**Decoder interface.** PT's decoder takes raw `(aux, aux_len, base, len, trace)`; the
AMD decoder takes `(const struct perf_branch_entry *br, size_t nbr, base, len, trace)`.
Add `asmtest_amd_decode(...)` alongside `asmtest_pt_decode` / `asmtest_cs_decode` in
[hwtrace.c](../../../src/hwtrace.c)'s dispatch.

**Reconstruction.** Unlike libipt (which carries its own instruction decoder), the AMD
path must walk instructions itself to find boundaries and offsets, so it **reuses the
project's existing Capstone layer** (the same dependency
[src/disasm.c](../../../src/disasm.c) already uses for annotation):

1. Start at the region entry (`base` / the begin marker PC). Branch records give ordered
   `from_i` (the branch instruction's address) and `to_i` (its target).
2. For each branch *i*: linearly decode instructions with Capstone from the current IP
   up to and including `from_i`, appending each in-range offset via `trace_append_insn`;
   then continue at `to_i`.
3. Mark a block start at the entry and after every branch (i.e. at every `to_i`) —
   **identical normalization to [pt_backend.c](../../../src/pt_backend.c)** — via
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

## Phase 3 — Block-boundary parity *(LANDED)*

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

## Phase 4 — Window-overflow truncation & DynamoRIO fallback routing *(LANDED)*

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
  - _Done._ The stitching core ships in [src/amd_backend.c](../../../src/amd_backend.c):
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
    [examples/test_hwtrace.c](../../../examples/test_hwtrace.c) `test_amd_stitch` synthesizes
    the `sample_period=1` windows of an 18-iteration loop, stitches them back to the
    gapless 18-edge sequence, and proves the decode is **complete** (all 55 instructions,
    two blocks, not truncated) where a single Tier-A 16-window honestly truncates — plus a
    gap-detection case.
  - _Done — live wiring (Zen 5-validated)._ `hwtrace_end_amd`
    ([hwtrace.c](../../../src/hwtrace.c)) now collects **every** branch-stack sample in the
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
- **MSR-direct snapshot — LANDED** (see [amd-msr-direct-lbr-plan.md](../archive/plans/amd-msr-direct-lbr-plan.md)).
  Read the LbrExtV2 `FROM`/`TO` MSRs directly (`/dev/cpu/N/msr`) around the region for an
  exact Tier-A snapshot with *zero* interrupts (vs Phase 1's per-branch PMIs), decoded by the
  shared `asmtest_amd_decode`. Needs `CAP_SYS_ADMIN` + the `msr` module (a self-hosted-runner
  optimization, not portable). `asmtest_amd_msr_trace` / `asmtest_amd_msr_available`
  ([src/msr_lbr.c](../../../src/msr_lbr.c)); validated live on the Zen 5 dev box
  (`make docker-hwtrace-msr`, `--privileged`): a tiny routine reconstructs complete despite
  the userspace freeze-syscall glue (thinned by a user-only `LBR_SELECT`), honestly
  `truncated` beyond the surviving window. The deterministic BPF boundary snapshot (Part II
  #2) stays the cleaner-boundary default where `CAP_BPF` is available.

---

## Deliverables (Phases 0–4)

- `ASMTEST_HWTRACE_AMD_LBR` enum + AMD arms in `available()`/`skip_reason()` in
  [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) /
  [src/hwtrace.c](../../../src/hwtrace.c).
- Data-ring (non-AUX) branch-record capture in [src/hwtrace.c](../../../src/hwtrace.c),
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
([src/amd_backend.c](../../../src/amd_backend.c), [src/hwtrace.c](../../../src/hwtrace.c)'s AMD
gating/capture chain, [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h),
[src/ptrace_backend.c](../../../src/ptrace_backend.c)) cross-referenced against external
primary sources (AMD APM Vol 2 / PPR, the Linux `arch/x86/events/amd/` and
`arch/x86/kernel/step.c` sources, LWN, and the perf man pages). Companion docs:
[trace-parity-matrix](../analysis/trace-parity-matrix.md) (what works where today),
[jit-runtime-tracing](../analysis/jit-runtime-tracing.md) (the foreign-JIT forward-look), and
[2026-07-09-amd-tracing-review](../analysis/2026-07-09-amd-tracing-review.md) (the follow-up
review pass over the landed Phases 0–5 whose verified findings feed the
[Newly surfaced](#newly-surfaced-2026-07-09-review) subsection and Part III Phases 8–9).
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
`PTRACE_SINGLESTEP` machinery in [src/ptrace_backend.c](../../../src/ptrace_backend.c).

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
| **P1** | **Consume LbrExtV2 `spec`/`valid` bits** before replay/stitch | [amd_backend.c](../../../src/amd_backend.c) filters only `abort` and *notes* it ignores spec flags — drop `PERF_BR_SPEC_WRONG_PATH` phantom edges | Zen 4/5, Linux ≥6.1 | **LANDED** (2026-07, Phase 4) | Small |
| **P1** | **Harden Tier-B throttle/ring config** (larger data ring; raise `kernel.perf_event_max_sample_rate`, set `kernel.perf_cpu_time_max_percent=0` on the runner) | Extends stitch reach before the kernel drops the newest samples — zero fidelity change | Zen 3/4/5 | **LANDED** (ring 64KB→256KB, Phase 5) | Small |
| **P1** | **Add a decodable-distance invariant to the stitcher** | The smallest-overlap heuristic can splice non-contiguous edges after a dropped/throttled sample; require the spliced adjacency be real straight-line code, else honest gap | Zen 3/4/5 | **LANDED** (2026-07, Phase 5 — honestly scoped: catches dropped-sample splices, not byte-decodable phase aliases) | Small–Med |
| **P2** | **IBS-Op complementary coverage lane** (esp. Zen 2) | Only HW branch source on Zen 2 (statistical precise-IP source→target); coverage-confirmer to shrink block-step/DR residual | statistical, not ordered | **SUPERSEDED + LANDED** by [zen2-ibs-tracing-plan.md](../archive/plans/zen2-ibs-tracing-plan.md) (Phases 0–4, 2026-07-12). That plan corrects the mechanics: the branch target arrives in the perf `PERF_SAMPLE_RAW` record (`reg[7] IbsBrTarget`), so the lane is **user-only + unprivileged** at `paranoid=2` via the kernel `swfilt` bit — **no `CAP_PERFMON` and no `MSR_AMD64_IBSBRTARGET`** read. Shipped: `asmtest_ibs_*` + `asmspy --sample` / TUI mode 7 | Medium |
| **P2** | **Runtime depth from CPUID `0x80000022` EBX** instead of `#define AMD_LBR_DEPTH 16` | Future-proofing hygiene (a no-op today — every shipping part reports 16) | Zen 4/5 | **LANDED** (2026-07, Phase 0 — EBX[9:4] `lbr_v2_stack_sz`) | Tiny |
| **P0** | **Cascade composition** (escalate to the MSR read before block-step) | Sampled-window truncation drops straight to the ~1000× block-step tier though an MSR-direct LBR read would complete a too-fast tiny routine first — the boundary snapshot is already default-on in `hwtrace_begin_amd` for single-exit regions (Matrix 2 #3; **widened to 1..4 exits 2026-07-12**), but the MSR path is in neither the marker path nor the auto cascade | **Zen 4/5 + `msr` access** (MSR); the rung self-skips elsewhere | **LANDED** (2026-07-10, Phase 8) — see [Newly surfaced](#newly-surfaced-2026-07-09-review) | Medium |

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
[`hwtrace_end_amd`](../../../src/hwtrace.c). A `uprobe`/`fentry` BPF program at region
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
> ([src/amd_backend.c](../../../src/amd_backend.c)) reports the hardware+kernel floor (AMD
> `amd_lbr_v2` + `perfmon_v2` CPU flags + Linux ≥ 6.10). `asmtest_amd_snapshot_trace()`
> ([src/branchsnap.c](../../../src/branchsnap.c)) is the capture: it enables the LBR, plants a
> **hardware execution breakpoint** at the region exit, and a CO-RE BPF program
> ([bpf/branchsnap.bpf.c](../../../bpf/branchsnap.bpf.c), `SEC("perf_event")` attached to the
> breakpoint) calls `bpf_get_branch_snapshot()` to read the frozen 16-entry stack at that
> ONE deterministic point. The raw `perf_branch_entry` array is decoded by the SAME
> `asmtest_amd_decode` the sampled path uses (in-region-filtered, so kernel-entry entries
> drop out) — reconstruction unchanged and already tested. **Design note:** a HARDWARE
> breakpoint (not a uprobe/fentry) is the trigger, because the region is anonymous
> executable memory (a JIT/mmap blob) with no inode+offset for a uprobe; the empirical
> **feasibility spike settled the open risk** — a `#DB` breakpoint does NOT evict the
> region's in-region branches before the helper reads them on this AMD part (the freeze
> path holds). **Validated:** `test_branchsnap` ([examples/test_branchsnap.c](../../../examples/test_branchsnap.c),
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
`asmtest_amd_freeze_available()` ([src/amd_backend.c](../../../src/amd_backend.c)) probes the
bit (cached); the Tier-A single-window decode in `hwtrace_end_amd`
([src/hwtrace.c](../../../src/hwtrace.c)) now trusts a freeze-less window as complete **only
if it actually captured the region-exit branch** (an entry with `from` in-region and `to`
outside), else flags `truncated` — the honest "do not assume the window reached exit"
posture. On a freeze-capable part the check is skipped (behavior unchanged).
`test_amd_freeze_probe` ([examples/test_hwtrace.c](../../../examples/test_hwtrace.c)) asserts
a definite/stable answer and prints this host's support (the dev Zen 5 / Ryzen 9 9950X
reports freeze **PRESENT**, so the gate is a no-op there — confirming the concern is
Zen-4-specific, not universal). Preferring the software-event snapshot (path #2) where
freeze is absent remains folded into that item.

### Newly surfaced (2026-07-09 review)

A follow-up pass over the now-landed Phases 0–5 —
[2026-07-09-amd-tracing-review](../analysis/2026-07-09-amd-tracing-review.md) — surfaced one
composition win (MSR) and a correctness/hygiene cluster. Unlike Phases 6–7, **none of it was
hardware-blocked**: all of it was buildable and self-validating on the dev Zen 5 (Ryzen 9
9950X), and landed 2026-07-10 as Part III **Phases 8 (MSR-rung cascade composition) and 9
(correctness & hygiene)**.

**Cascade composition — the near-term win [real, MSR-scoped].**
`asmtest_trace_call_auto` ([trace_auto.c:134](../../../src/trace_auto.c)) escalates fast
sampled hwtrace → BTF block-step → per-instruction single-step. When the fast AMD_LBR tier
returns a truncated window, its completion gate falls through
([trace_auto.c:175](../../../src/trace_auto.c)) straight into the ~1000× block-step round-trip
([trace_auto.c:181](../../../src/trace_auto.c)). One exact, low-interrupt AMD path is skipped
there: the MSR-direct read `asmtest_amd_msr_trace` ([msr_lbr.c:110](../../../src/msr_lbr.c)),
which is in neither the marker path nor the cascade. It is strictly better-targeted than
AMD_LBR's `sample_period=1` richest-window guess for exactly the too-fast-to-sample case that
truncates — and, reading the frozen stack around ANY region rather than at one planted
breakpoint, it also completes a *multi*-exit tiny routine, which the boundary snapshot cannot
(see below). The frozen boundary snapshot `asmtest_amd_snapshot_trace`
([branchsnap.c:246](../../../src/branchsnap.c)) is **not** the gap: `hwtrace_begin_amd` already
runs it BY DEFAULT for single-exit regions ([hwtrace.c:655](../../../src/hwtrace.c), Matrix 2
#3 — landed), and the fast tier reaches it through the marker path, so on the single-exit case
the snapshot already ran in the fast tier. A re-run rung gated on the same `amd_nret == 1`
would be redundant — if that snapshot completed there is no truncation to escalate, and if it
overflowed the 16-deep stack, re-running cannot help. The snapshot's genuine improvement is
WIDENING that default to the tail-call exit (the Phase-9 item below), not a cascade rung. An
MSR rung inserted at the fast-tier fall-through
([trace_auto.c:177](../../../src/trace_auto.c)) completes the too-fast case without paying the
block-step tier. The qualification is real: the entry does not fork — it runs `run_fn`
in-process ([msr_lbr.c:159](../../../src/msr_lbr.c)) — so the rung is a **second in-process
re-run**: non-idempotent side effects double and a crashing routine takes down the tracer, a
genuine semantic gap versus the fork-sandboxed steppers below it. It must therefore gate on
`asmtest_amd_msr_available()`, and skip the whole rung under `CEILING_FREE`
([trace_auto.c:74](../../../src/trace_auto.c)) since the MSR read shares AMD_LBR's 16-deep
ceiling.

> **Amendment (2026-07-16) — the multi-exit premise above expired; the rung did not.** This
> analysis is preserved as written on 2026-07-09. Two of its load-bearing claims have since
> been overtaken by `e9ca70e`, which widened the default-on snapshot from one breakpoint at
> the last exit to **one per exit, up to `ASMTEST_AMD_MAX_EXITS == 4`**
> ([src/hwtrace.c:777-783](../../../src/hwtrace.c); the `hwtrace.c:655` line cited above is
> stale):
> - *"it also completes a multi-exit tiny routine, which the boundary snapshot cannot"* —
>   now true only for a region with **more than 4** exits. At 1..4 exits the snapshot plants
>   a breakpoint at **every** exit, so whichever one the run leaves through is caught.
> - *"The snapshot's genuine improvement is WIDENING that default to the tail-call exit (the
>   Phase-9 item below)"* — that widening landed (Phase 9) and was then generalised again by
>   the multi-exit arm, which subsumes it: `asmtest_amd_all_exits` counts both `ret`-class
>   and region-leaving direct tail-`jmp` exits.
>
> **The rung remains correct, shipped, and worth keeping** — its residual is real, just
> smaller than this paragraph implies: regions with **>4** exits (which stay on the sampled
> path by default), plus every snapshot **arm failure** (no BPF toolchain, missing
> `CAP_BPF`/`CAP_PERFMON`, a debugger holding a needed debug register, no decodable exit),
> plus non-Zen-4/5 substrates where the snapshot is unavailable outright. The rest of the
> reasoning above — the in-process re-run caveat, the `asmtest_amd_msr_available()` gate, the
> `CEILING_FREE` exclusion — is unaffected and describes the shipped code. See the Phase 8
> amendment for the as-shipped framing.

**The correctness & hygiene cluster.** One real bug and four divergence risks, all in the AMD
TUs:

- **MSR speculative-entry leak — real trace bug [confirmed]. FIXED (Phase 9, 2026-07-10);
  the `msr_lbr.c:175` line ref below is pre-fix — the decode was extracted to the pure
  `asmtest_amd_msr_decode_entry`, where the test now reads `if (!((to >> 63) & 1))` at
  [msr_lbr.c:99](../../../src/msr_lbr.c).** In `asmtest_amd_msr_trace` the
  validity test `int valid = ((t >> 63) & 1) || ((t >> 62) & 1)`
  ([msr_lbr.c:175](../../../src/msr_lbr.c)) admits a spec-only wrong-path entry (TO[63]=0,
  TO[62]=1) whose `.spec` field stays zero from the earlier `memset`, so `amd_replay`'s
  `PERF_BR_SPEC_WRONG_PATH` filter ([amd_backend.c:186](../../../src/amd_backend.c)) never
  fires and a phantom edge enters the reconstruction. On Zen 4/5 LbrExtV2 the hardware really
  does record wrong-path branches, so this is data-dependent, not theoretical. Dropping the
  `|| spec` term skips the entry at source — strictly better than setting `.spec`, since that
  filter is `#ifdef ASMTEST_HAVE_PERF_BR_SPEC`-gated and may be compiled out. Narrow trigger
  (`asmtest_amd_msr_trace` needs `CAP_SYS_ADMIN` + the `msr` module) but a genuine
  wrong-trace, not hygiene.
- **Duplicated reduced-filter macro** (pure hygiene). `ASMTEST_AMD_REDUCED_FILTER` is
  hand-copied byte-identically in [hwtrace.c:595](../../../src/hwtrace.c) and
  [branchsnap.c:52](../../../src/branchsnap.c) (carrying a "kept in sync" note) — future-drift
  risk; fold into one shared header.
- **Forward-decl fragility** (latent UB). The `amd_*` prototypes are forward-declared inline in
  [hwtrace.c:91](../../../src/hwtrace.c) (and re-copied into branchsnap.c/msr_lbr.c) with no
  shared header, so a future signature change in amd_backend.c would compile and link cleanly
  yet be UB at the call boundary. A shared `src/amd_backend.h` (mirroring the existing
  `stealth_helper.h` internal-header pattern) turns that into a compile error.
- **Local error-code redefs** (pure hygiene). [amd_backend.c:32](../../../src/amd_backend.c)
  and [ss_backend.c:67](../../../src/ss_backend.c) locally re-`#define` `ASMTEST_HW_*` codes
  that duplicate `include/asmtest_hwtrace.h`; values match today, divergence risk only.
- **Unchecked `PERF_EVENT_IOC_ENABLE`** (robustness). The arm ioctl is fire-and-forget at
  every site ([hwtrace.c:727](../../../src/hwtrace.c),
  [branchsnap.c:211](../../../src/branchsnap.c)); a failed enable yields an empty ring, but the
  AMD sampled and survey paths already convert that to an honest `truncated`, so on the AMD
  tiers this is robustness, not a correctness fix.

**Tail-call snapshot widening. LANDED (Phase 9, 2026-07-10) and since GENERALISED — see the
note below.** The default-on boundary snapshot is gated to single-exit
regions (`amd_nret == 1`, [hwtrace.c:655](../../../src/hwtrace.c)), and its exit deriver
recognizes only `ret`-class instructions — so a genuinely single-exit routine that leaves via
a tail-call `jmp` never gets the deterministic default. Extending the exit predicate to a
region-LEAVING direct uncond `jmp` widens the default with no new risk (a missed boundary
already truncates rather than corrupts). This — not a cascade re-run rung — is the snapshot's
genuine composition improvement (the MSR rung above covers the multi-exit tiny case in the
meantime).

> **Superseded 2026-07-16.** The tail-`jmp` predicate landed as specced in Phase 9
> (`amd_last_ret_off` → `asmtest_amd_last_exit_off`), and `e9ca70e` then generalised the
> whole gate: the deriver became `asmtest_amd_all_exits`, which collects **every** exit
> (`ret`-class *and* region-leaving direct tail-`jmp`) and the arm plants a breakpoint at
> each, up to `ASMTEST_AMD_MAX_EXITS == 4`
> ([src/hwtrace.c:777-783](../../../src/hwtrace.c)). So the `nret == 1` gate described here
> no longer exists, and the parenthetical "the MSR rung covers the multi-exit tiny case in
> the meantime" now applies only to regions with **>4** exits and to snapshot arm failures.

**Correction to the review's §1.2 [real, qualified].** The review flagged the perf-ring
pointer casts — `uint64_t nr = *(uint64_t *)body` and the sibling header/entry casts in the
three drain loops ([hwtrace.c:828](../../../src/hwtrace.c)) — as a misalignment hazard. That
over-states it: both drain TUs compile `#if defined(__linux__) && defined(__x86_64__)`-only,
`buf` is malloc-aligned, and perf records are 8-byte-multiple sized, so nothing misaligns on
x86-64. The only real issue is `-fstrict-aliasing` / UBSan-lane UB, fixed with the
`memcpy`-into-a-local pattern — it matters for the sanitize build, not the shipped one.

These landed as Part III **Phase 8** (MSR-rung cascade composition, `src/trace_auto.c`) and
**Phase 9** (correctness & hygiene, `src/hwtrace.c` + `src/branchsnap.c` + `src/msr_lbr.c` +
new `src/amd_backend.h`), both *(landed 2026-07-10)* on the dev Zen 5.

## Matrix 2 — Squeezing the existing window (P1 detail)

These keep asm-test's PMU-window architecture — which the research **confirms is at the
AMD hardware ceiling** — but remove its sharpest edges.

| Lever | Mechanism | Why it helps | Caveat |
|---|---|---|---|
| **BRS period-adjust** (Zen 3) | Fixed `sample_period ≈ N−16` (min 17); the kernel already programs `period − lbr_nr` (`amd_brs_adjust_period`) and BRS freezes/holds the NMI until the 16-branch buffer saturates | **One** PMI delivers the complete ≤16 window at region exit, versus `sample_period=1`'s one-PMI-per-branch flood that trips `perf_event_max_sample_rate` throttling and the non-overwrite ring | Zen 3 BRS **only** (forward-capture, fixed mode, period > 16). On Zen 4/5 the better lever is the software-event snapshot (P0 #2), not this |
| **`spec`/`valid` filtering** (Zen 4/5) | `perf_branch_entry.spec` carries `PERF_BR_SPEC_WRONG_PATH`; the LbrExtV2 driver passes wrong-path entries through to userspace | Drops speculative/wrong-path phantom edges before `amd_replay`, which today filters only `abort` and *explicitly notes* (amd_backend.c comment) that it ignores every other flag | Wrong-path entries are relatively uncommon, so this is a precision refinement, not a step-change. LbrExtV2/Linux ≥6.1 only — a no-op on Zen 3 BRS (retired-only, no spec bits) |
| **Throttle/ring hardening** | Larger `data_size` ring; `sysctl kernel.perf_event_max_sample_rate` up, `kernel.perf_cpu_time_max_percent=0` on the self-hosted runner | The Tier-B live path is bounded by ring size + throttling (a 20 000-trip loop already truncates); more headroom = longer gapless stitch before honest truncation | Operational, self-hosted-runner only; extends reach, does not remove the ceiling |
| **Decodable-distance stitch check** | For each stitched boundary, assert reconstructed instruction count between consecutive branch targets == statically-decoded byte distance; reject a wrong minimal-shift match, else emit an honest gap | AMD sets `hw_idx ≡ 0` (register renaming keeps From[0]=TOS), so Intel's exact index-based overlap count **cannot** be ported — the current smallest-overlap heuristic can silently mis-stitch a self-overlapping loop (an open question in amd_backend.c). This is the AMD-available substitute check | Improves correctness for the common looping case; does not extend depth |
| **Period-spaced Tier-B stitching** (Zen 4/5) — *#2A* | Set `sample_period = depth − overlap` (e.g. 16−4=12) instead of `1`, so consecutive 16-deep LbrExtV2 windows overlap by `overlap` and still stitch gaplessly at ~P× fewer PMIs | The `sample_period=1` PMI-per-branch flood is the dominant Tier-B throttle/ring truncation cause; spacing the PMIs cuts throttling exposure ~P× → **extends the exact window ~P×** before the run stops fitting. Generalizes the Zen 3 BRS period-adjust lever to LbrExtV2 *multi-window* stitching | LbrExtV2 (Zen 4/5). An over-large period simply yields a stitch gap → honest truncation, never silent corruption. Not for tiny single-shot routines (a spaced PMI may never fire in-region — keep `lbr_period=0` there) |
| **Slot-efficient branch filtering** (Zen 4/5) — *#2B* — **LANDED (SCOPE-SAFE)** | Reduced HW branch filter `cond \| ind_jmp \| any_call \| any_return`, dropping only the **direct unconditional jmp** (statically decodable) so the 16 LBR slots stretch further; `amd_replay` follows the dropped jmp from the region bytes for a byte-identical trace | A direct uncond jmp has a static target the decoder can follow, so recording it wastes a slot; dropping it stretches each window with **no** exactness loss — a reach multiplier orthogonal to #2A | Landed as SCOPE-SAFE: dropping direct **call** too (the original `ind_call` framing) was rejected — an out-of-region-callee return strands pre-call code, a silent-corruption risk. Opt-in `branch_filter`; the unified decoder is inert on the default filter. Live reach-gain pending a perf-permitted Zen host |

### Window-size levers — status (2026-07-08)

- **#2A period-spaced Tier-B stitching — LANDED (opt-in), live-validation pending; host-validated stitch + documented caveat.**
  New `asmtest_hwtrace_options_t.lbr_period` ([include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h)):
  `0` keeps the exact `sample_period=1` Tier-A/B path (default, unchanged);
  `>1` sets the branch-retired period, clamped to `[1, depth-1]` in
  `hwtrace_begin_amd` ([src/hwtrace.c](../../../src/hwtrace.c)) so an overlap always
  remains. The whole extension leans on the *existing, validated* stitch + gap/`lost`
  detection (`asmtest_amd_stitch` / `asmtest_amd_decode_stitched`): insufficient overlap
  becomes a stitch gap → `truncated`, so a bad `lbr_period` degrades honestly rather than
  corrupting. **Host-independent validation added** (`test_amd_stitch_period_spaced`,
  [examples/test_hwtrace.c](../../../examples/test_hwtrace.c)): synthetic period-spaced
  (P=4) windows of a **distinct-edge** path stitch back to the exact full sequence — the
  smallest-overlap heuristic finds the true shift because distinct edges disambiguate the
  alignment. **Correctness caveat (why the default stays `lbr_period=0`):** a
  *self-similar* loop whose every taken edge is identical gives the heuristic no way to tell
  1 iteration from P, so `period>1` silently **undercounts** it (the test asserts this to
  make the limitation concrete). `period=1` is the only universally-exact value; `>1` is a
  coverage/throttle trade the caller opts into for distinct-edge hot paths. **Live-measured
  on Zen 5** (`test_amd_reach_period`, `docker-hwtrace-amd` with `CAP_PERFMON`): on the
  self-similar `AMD_LOOP`, `period=4` reconstructs **fewer** instructions than `period=1`
  (231 vs 297) — the undercount, confirmed on real hardware, **not a reach gain**. This
  sharpens the caveat into a *finding*: **every loop is edge-self-similar** (its branch
  offsets, hence from/to edges, repeat each iteration), so period-spacing never helps a loop
  — its reach benefit is confined to **distinct-edge straight-line paths, which are
  inherently short**. So `lbr_period>1` is a narrow lever (default stays 0); the throughput
  win it targets is better served on Zen 4/5 by the #2B reduced filter, which *does* extend
  a loop's per-window reach (measured 1.86× below). The tested default path is untouched.
- **#2B slot-efficient branch filtering — LANDED (opt-in), SCOPE-SAFE + unified decoder; live reach-gain pending.**
  Shipped as the SCOPE-SAFE form: the reduced HW filter drops **only direct unconditional
  `jmp`** (`ASMTEST_AMD_REDUCED_FILTER = COND | IND_JUMP | ANY_CALL | ANY_RETURN`,
  [src/hwtrace.c](../../../src/hwtrace.c) / [src/branchsnap.c](../../../src/branchsnap.c)),
  keeping every call recorded so the decoder's in-region `from_off` anchor is preserved.
  SCOPE-MAX (also dropping direct call) was rejected: a dropped call to an out-of-region
  callee strands the pre-call in-region code behind an out-of-region return edge, which is
  not cleanly reconstructable without speculative decoding — a silent-corruption risk the
  house rule forbids. The decoder is **unified, no flag**: `amd_replay`
  ([src/amd_backend.c](../../../src/amd_backend.c)) follows a direct uncond `jmp`'s static
  target (`asmtest_disas_branch_target` + new `asmtest_disas_is_uncond_jump`) only when one
  is encountered *mid-straight-line-walk* (`o != from_off`), which under the DEFAULT full
  filter can never happen (a taken jmp is the recorded `from`) — so the follow path is
  provably dead code on the tested default and the trace stays byte-identical. Opt-in via
  `asmtest_hwtrace_options_t.branch_filter` (default 0 = `BRANCH_ANY`, unchanged); the
  capture retries with the full filter on `EOPNOTSUPP`/`EINVAL` so the tier stays available.
  Applies to both the sampled and the deterministic-snapshot exact paths (the snapshot is
  the biggest beneficiary — one frozen window spans more of the routine); the statistical
  WindowHot survey keeps `BRANCH_ANY`. **Host-independent validation**
  (`test_amd_reduced_filter` F1–F5, [examples/test_hwtrace.c](../../../examples/test_hwtrace.c)):
  a dropped in-region jmp reconstructs byte-identically to the full stack that keeps it,
  dropped-back-edge cycles terminate (step bound), region-exit-leaving jmps truncate, and
  chained jmps follow through. Two independent adversarial reviews confirmed the classify/
  follow logic exhaustive over every x86-64 CTI and the default path byte-identical.
  **Live-validated on Zen 5** (Ryzen 9 9950X, `docker-hwtrace-codeimage`): `test_branchsnap`'s
  `branchsnap #2B` case captures a routine with a direct uncond jmp (target 0x08) plus a kept
  conditional anchor through the DETERMINISTIC snapshot with `branch_filter=1`, and the
  reconstruction covers the jmp's target block 0x08 — i.e. `amd_replay` followed the dropped
  jmp from the region bytes on real LbrExtV2 (robust either way: if perf rejects the
  type-filter combo the fallback records the jmp and the trace is identical). **Reach-gain
  measured on Zen 5** (`test_branchsnap`'s `branchsnap #2B reach`): a loop whose body has a
  direct uncond jmp plus a conditional back-edge reconstructs **1.86× more executed
  instructions per 16-deep window** under the reduced filter (65 vs 35) — because it records
  one taken branch per iteration (the kept jnz) instead of two (jnz + the dropped jmp), so a
  single frozen snapshot window spans ~2× the iterations. Deterministic (no sampling
  variance), `reduced >= full` always (fallback ties). This is the AMD Zen 4/5 window-stretch
  lever that #2A cannot provide for loops (see above).
- **#3 deterministic-snapshot default — LANDED (single-exit 2026-07-08; widened to
  MULTI-exit 2026-07-12, `e9ca70e`).**
  The Phase-3 boundary snapshot (Part II #2) was previously opt-in only (`opts.snapshot`).
  `hwtrace_begin_amd` ([src/hwtrace.c:777-783](../../../src/hwtrace.c)) now also selects it
  **by default** on the substrate that supports it (`asmtest_amd_snapshot_available()`).
  *As first shipped* the default was gated to a **SINGLE-exit** region (`nret == 1`),
  because the snapshot planted ONE breakpoint at the last ret: a *multi*-exit routine
  returning via an earlier ret would miss it and honestly truncate with no fall-through
  (snapshot mode is committed), so gating to a lone ret kept the boundary guaranteed-hit.
  **That restriction is gone.** The arm now plants **one HW execution breakpoint per exit,
  up to `ASMTEST_AMD_MAX_EXITS == 4`** — the x86 debug-register budget — via
  `asmtest_amd_all_exits` + `asmtest_amd_snapshot_begin_multi`
  ([src/amd_backend.h:57,69](../../../src/amd_backend.h)), so whichever exit the run leaves
  through hits a boundary; the gate is `amd_nexit >= 1 && … amd_nexit <=
  ASMTEST_AMD_MAX_EXITS`. A BPF-side drop counter drives an honest truncated-on-drop
  contract. A region with **more than 4** exits stays on the sampled path by default (not
  enough debug registers to cover every boundary); an explicit `opts.snapshot` is still
  honored for any region and keeps the legacy last-exit best-effort there. The arm is
  **all-exits-or-nothing**: any failure (no BPF toolchain / caps, a debugger holding a
  needed debug register, no decodable exit) falls through to the sampled path unchanged.
  Validated: `branchsnap markers` (`docker-hwtrace-codeimage`) routes `add2` through the
  snapshot begin/end on the Zen 5 dev box.
- **Pre-existing hygiene note (not this work) — RESOLVED 2026-07-12 (`e9ca70e`), see the
  amendment under this item.** ~~the 7 field-by-field bindings (Python,
  Rust, Go, Node, Ruby, Java, Lua) still mirror `asmtest_hwtrace_options_t` only through
  `object_hint`, so `asmtest_hwtrace_init`'s by-value copy over-reads the `lbr_period` +
  `branch_filter` tail (Zig/C++/.NET track the full struct). This 8-byte over-read predates
  this work (it shipped with `lbr_period`); `branch_filter`'s addition does not grow it and
  its garbage-value consequence is benign (the reduced filter is fidelity-neutral and
  retries on rejection). Closing it fully means appending both int fields to those 7
  layouts — a focused, individually-lane-validated follow-up.~~

  **Closed by the options ABI flag day** (`e9ca70e`), which fixed the class of bug rather
  than the two fields: `asmtest_hwtrace_options_t` now **leads with `size_t struct_size`**
  ([include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h), first member) and
  `asmtest_hwtrace_init` copies `min(struct_size, sizeof)` then zero-fills the tail, so a
  short caller can no longer be over-read. A caller that does not self-describe
  (`struct_size == 0`, or too small to reach `backend`) is rejected with **EINVAL** rather
  than silently losing a trailing field. All seven field-by-field bindings now mirror the
  full struct — `lbr_period`, `branch_filter` **and** `struct_size` are present in
  `bindings/python/asmtest/hwtrace.py`, `bindings/rust/src/hwtrace.rs`,
  `bindings/go/hwtrace.go`, `bindings/node/hwtrace.js`, `bindings/ruby/hwtrace.rb`,
  `bindings/java/HwTrace.java`, and `bindings/lua/hwtrace.lua` — with parity green and zero
  allow-list changes.

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

> **Superseded by [zen2-ibs-tracing-plan.md](../archive/plans/zen2-ibs-tracing-plan.md) (landed 2026-07-12).**
> The paragraph below is kept for history but was **wrong on the privilege model**: the branch
> target is delivered in the perf `PERF_SAMPLE_RAW` record (`reg[7] IbsBrTarget`), so with the
> kernel `swfilt` bit the lane runs **user-only and unprivileged** at `paranoid=2` — **no
> `CAP_PERFMON`/`CAP_SYS_ADMIN` and no `MSR_AMD64_IBSBRTARGET` read**. It shipped as the
> `asmtest_ibs_*` API + `asmspy --sample` (headless) and TUI mode 7.

IBS-Op is worth exactly one thing: it is the **only hardware branch source on Zen 2**,
and it carries a real precise-IP source→target edge (`IbsOpRip` → `IbsBrTarget`,
with `op_brn_ret`/`op_brn_taken`/`op_brn_misp` bits) — **capability-gated** on
`IBS_CAPS_BRNTRGT` (CPUID `Fn8000_001B` EAX[5], present on all Zen but must be probed).
But it is **statistical**: one tagged micro-op per counter period, so it yields a sparse,
probabilistic edge set — never an ordered, complete path — and its per-NMI edge yield is
*lower* than LBR's ~16 records per interrupt. ~~It also requires
`CAP_SYS_ADMIN`/`CAP_PERFMON` (IBS PMUs have no user/kernel filter).~~ **Corrected: user-only
IBS opens unprivileged via `swfilt` — see the superseding plan.** So the honest role is
a **coverage-confirmer / hot-edge pre-cover** that shrinks (does not bound) the block-step
/ DynamoRIO residual, or an indirect-branch-target resolver — not a replacement for the
branch stack. (Two raw research proposals here were themselves wrong and were caught in
verification: `rand_en` is an IBS-*Fetch* knob, not Op; and IBS-Op does carry a branch
target, but only capability-gated and only for retired taken branches.)

## Documentation corrections this analysis implies

- [zen2-singlestep-trace-plan.md](zen2-singlestep-trace-plan.md): the
  `PTRACE_SINGLEBLOCK`-unwired-on-x86 claim is wrong (see the headline correction); it is
  what currently strands BTF block-step in "research only."
- [native-tracing.md](../../guides/tracing/native-tracing.md) / [hardware-tracing.md](../../guides/tracing/hardware-tracing.md):
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
and the stitcher's overlap check to sharpen the tier asm-test already ships. With all
three landed, the remaining near-term work is **composition, not capture**: wire the
shipped MSR-direct read into the auto cascade before block-step and close the
correctness/hygiene cluster the 2026-07-09 review surfaced (see
[Newly surfaced](#newly-surfaced-2026-07-09-review)).

---

# Part III — Improvement roadmap (planned)

This turns the Part II findings into buildable phases. Every phase produces or refines an
`asmtest_trace_t` ([include/asmtest_trace.h](../../../include/asmtest_trace.h)) — the same
ordered instruction offsets and branch-normalized block offsets the Unicorn emulator, the
DynamoRIO native tier, Intel PT, and the existing AMD LBR / single-step backends already
fill — so no new public trace struct or field is introduced and overflow / loss continues
to collapse onto the existing `truncated` bit. Block offsets are computed with the
**same** branch-edge normalization as [src/pt_backend.c](../../../src/pt_backend.c) so a
reconstructed trace stays byte-identical to the other tiers. The block-step phase extends
the [single-step plan](zen2-singlestep-trace-plan.md)'s W2 out-of-process ptrace stepper
(and **corrects** its "W3 is blocked on x86" claim); the window phases extend Part I's
shipped LBR backend.

## Implementation status

**Phases 0–5 and 8–9 landed; Phase 7 superseded + landed via
[zen2-ibs-tracing-plan.md](../archive/plans/zen2-ibs-tracing-plan.md); Phase 6 remains forward-look
(hardware-blocked).** The
[2026-07-09-amd-tracing-review](../analysis/2026-07-09-amd-tracing-review.md) surfaced two
further phases that — *unlike* the hardware-blocked Phases 6–7 — need no new silicon and
landed on this Zen 5 dev box (2026-07-10): **Phase 8** composes the fidelity cascade so a
truncated sampled window escalates to the shipped MSR-direct exact path *before* the ~1000×
block-step slowdown (the boundary snapshot already runs default-on in the fast tier, so it
is deliberately *not* a rung — see Part II [Newly surfaced](#newly-surfaced-2026-07-09-review)),
and **Phase 9** is a correctness & hygiene cluster (including the real MSR speculative-edge
leak that today lets phantom wrong-path edges through). Build order followed the Part II
priorities: Phase 0 (gating) → Phase 1 (freeze correctness) → Phase 2 (BTF block-step) →
Phase 3 (software-event snapshot) were the P0/near-term work; Phases 4–5 are the P1
refinements. As of **2026-07-06** all of them ship and are validated on the Zen 5 dev box
(Ryzen 9 9950X, `amd_lbr_v2`):

- **Phase 0 (runtime depth).** `asmtest_amd_lbr_depth()`
  ([src/amd_backend.c](../../../src/amd_backend.c)) reads CPUID `0x80000022` EBX[9:4]
  (`lbr_v2_stack_sz`) — the true branch-stack depth — replacing the hardcoded 16 in the
  Tier-A/Tier-B overflow split, the stitch bound, and the LOST heuristic
  ([src/hwtrace.c](../../../src/hwtrace.c)). A no-op today (every shipping Zen reports 16 —
  the dev box confirms 16) that removes the assumption. *(The freeze-bit reader this phase
  also scoped shipped earlier with Phase 1.)*
- **Phase 1 (freeze gate)** — landed (see Part II #3).
- **Phase 2 (BTF block-step)** — landed, now including the attached variant
  `asmtest_ptrace_trace_attached_blockstep` (all three public symbols; wrapped in all ten
  bindings), [src/ptrace_backend.c](../../../src/ptrace_backend.c).
- **Phase 3 (software-event snapshot)** — landed, including the `snapshot: true`
  begin/end marker-path opt-in ([src/branchsnap.c](../../../src/branchsnap.c) +
  [src/hwtrace.c](../../../src/hwtrace.c)); see Part II #2.
- **Phase 4 (LbrExtV2 spec filtering)** — landed (below).
- **Phase 5 (stitch decodable-distance guard + ring hardening)** — landed (below).

**Phase 6 (BRS period-adjust, Zen 3 only) stays forward-look**: it requires silicon
neither dev host has (Zen 3 BRS), so per the house "no untested hardware code" rule it
is not implemented — a lane that cannot self-validate on its target silicon must not
ship unproven. It remains fully specified below for when that hardware is available.
*(Phase 7, once grouped here as hardware-blocked, has since landed — unprivileged, on
the Zen 2 host — via [zen2-ibs-tracing-plan.md](../archive/plans/zen2-ibs-tracing-plan.md).)*

Two **documentation corrections** Part II surfaced shipped alongside Phase 0/2 (see
Deliverables): the `PTRACE_SINGLEBLOCK`-unwired-on-x86 claim in
[zen2-singlestep-trace-plan.md](zen2-singlestep-trace-plan.md), and the "AMD LBR is
finished, no forward-look" framing in [native-tracing.md](../../guides/tracing/native-tracing.md) /
[hardware-tracing.md](../../guides/tracing/hardware-tracing.md).

## Improvement Phase 0 — Feature detection & gating (`src/hwtrace.c`) *(landed 2026-07-06)*

**Goal.** Detect the per-uarch/per-kernel capabilities the later phases depend on, so
every new lane self-skips cleanly instead of misbehaving on the wrong silicon.

**Work.**
- Add a CPUID reader to the existing `#if defined(__x86_64__)` block near
  `amd_branch_probe` ([src/hwtrace.c:180](../../../src/hwtrace.c)). There is **no** reusable
  cpuid helper in hwtrace.c (vendor detection is `/proc/cpuinfo`-based via `vendor_is`,
  [hwtrace.c:120](../../../src/hwtrace.c)); reuse the `<cpuid.h>` /
  `__get_cpuid`/`__get_cpuid_count` pattern already used in
  [src/asmtest.c:434](../../../src/asmtest.c). Read leaf `0x80000022`: EAX bit 1 =
  `AMD_LBR_V2`, **EAX bit 2 = `AMD_LBR_PMC_FREEZE`** (the freeze-on-PMI gate Phase 1
  needs), EBX = `lbr_v2_stack_sz` (the runtime depth for the P2 depth-detection hygiene
  item).
- **Runtime depth detection.** Replace the hardcoded `AMD_LBR_DEPTH` 16 (duplicated at
  [amd_backend.c:48](../../../src/amd_backend.c) and [hwtrace.c:58](../../../src/hwtrace.c))
  with a value read from CPUID EBX (fallback 16). Because it drives the Tier-A/Tier-B
  split ([hwtrace.c:568](../../../src/hwtrace.c)), the stitch `out_cap`
  ([hwtrace.c:570](../../../src/hwtrace.c)), and the overflow flag
  ([amd_backend.c:117](../../../src/amd_backend.c)), the decoder must take depth as a
  **parameter** rather than reading a local macro — a cross-TU signature change that must
  be mirrored in the non-Linux stubs ([amd_backend.c:239/261](../../../src/amd_backend.c)).
  Ships as a no-op today (every part reports 16) but removes the assumption.
- Add skip-reason strings in `asmtest_hwtrace_skip_reason`
  ([hwtrace.c:245-284](../../../src/hwtrace.c)) for any new gated outcome (e.g. a
  freeze-unsupported note), following the existing AMD strings at
  [hwtrace.c:264-274](../../../src/hwtrace.c).

**Acceptance.** `make hwtrace-test` still passes on the Zen 5 dev box
(`make docker-hwtrace-amd`); a debug print of the detected freeze bit + depth matches
`amd_lbr_v2` (freeze present, depth 16). No behavior change yet — this phase only adds
detection the later phases consume.

## Improvement Phase 1 — Freeze-on-PMI window-trust gate (`src/hwtrace.c`) *(landed 2026-07-06)*

> **Status (2026-07-06): LANDED** — see Part II #3 for the as-shipped shape
> (`asmtest_amd_freeze_available` + the exit-branch trust gate in `hwtrace_end_amd`). The
> freeze-**absent** branch remains covered by the synthetic fixture only, **pending real
> freeze-absent Zen 4 hardware** (this Zen 5 box reports freeze PRESENT), per the house
> "no untested hardware code" rule — as the Acceptance below records.

**Goal.** Stop trusting a 16-entry window the hardware never froze. Freeze-on-PMI is
**not** universal on Zen 4 (gated on the Phase-0 CPUID `0x80000022` EAX[2] bit); when
absent, the LBR keeps advancing past the overflow point, so a captured window can silently
*not* end at region exit — yet `hwtrace_end_amd` trusts it today.

**Work.**
- In `hwtrace_end_amd` ([hwtrace.c:449](../../../src/hwtrace.c)), when the Phase-0 freeze bit
  is **absent**, treat a Tier-A window whose newest in-region branch does not reach the
  region exit as `truncated` rather than complete (consistent with the existing "no
  in-region branches" truncation at [hwtrace.c:588-589](../../../src/hwtrace.c)), so the
  caller re-resolves under `CEILING_FREE` / falls to DynamoRIO.
- Prefer the Phase-3 software-event snapshot (which stops the LBR in software) over PMI
  sampling on freeze-absent parts once Phase 3 lands; until then, the honest `truncated`
  is the correct degrade.
- Keep the probe/capture attr divergence in mind: `amd_branch_probe`
  ([hwtrace.c:186-189](../../../src/hwtrace.c)) omits `exclude_hv` while `hwtrace_begin_amd`
  ([hwtrace.c:423-424](../../../src/hwtrace.c)) sets it; any attr-touching change here must
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
> [src/ptrace_backend.c](../../../src/ptrace_backend.c)) landed earlier; the **attached
> variant `asmtest_ptrace_trace_attached_blockstep`** — the deliverable this section
> scoped as its third symbol — now also ships, block-stepping a SEPARATE,
> externally-attached process from its current stop (foreign bytes via
> `process_vm_readv`, target never killed, left stopped past the region for the caller),
> so a live JIT/managed method traces rootless at a fraction of the stops. All three are
> wrapped in **all ten bindings** (parity gate green at 98 symbols) and validated:
> `test_ptrace_attach_blockstep` ([examples/test_hwtrace.c](../../../examples/test_hwtrace.c))
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
[asmtest_hwtrace.h:54-60](../../../include/asmtest_hwtrace.h) — INTEL_PT / CORESIGHT /
AMD_LBR / SINGLESTEP — is the *in-process/decoder* tier; the W2 stepper is not one of
them).

**Work.**
- Thread a `step_mode` (single-step vs block-step) through the shared bodies
  `trace_attached_impl` ([ptrace_backend.c:764](../../../src/ptrace_backend.c)) and the
  `trace_call` tracee loop ([ptrace_backend.c:633](../../../src/ptrace_backend.c)) rather than
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
  ([ptrace_backend.c:452](../../../src/ptrace_backend.c)) as the per-insn path, walk forward
  from the previous block-entry PC with `asmtest_disas(PTRACE_TRACE_ARCH, …)` lengths
  ([the only reconstruction primitive](../../../include/asmtest_trace.h), also used by
  `normalize`), appending each reconstructed insn offset, then feed that **reconstructed
  per-instruction stream** (not the block-entry PCs) to `normalize`, which re-derives
  `blocks[]` — so `blocks[]` parity follows from `insns[]` being correct. This is the
  load-bearing extra work; a decode desync (`asmtest_disas` length 0) sets `truncated`
  exactly as `normalize` does at [ptrace_backend.c:465-468](../../../src/ptrace_backend.c).
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

  > **This rule was specified here and then NOT implemented — for as long as the block-step
  > tier has shipped. Fixed 2026-07-17 (`ee696e0`).** Both reconstructors did exactly the
  > greedy thing this bullet warns against, so the dual-guard shape (`je T; …; je T` — the
  > `||` short-circuit that JIT'd managed code emits constantly) ended the block at a
  > *not-taken* branch and silently dropped instructions that really executed, returning
  > `rc=OK` with `truncated=false`. Reproduced through the shipped
  > `asmtest_ptrace_trace_call_blockstep`: per-instruction `0 3 6 8 11 14 18 21 24` vs
  > block-step `0 3 6 21 24`.
  >
  > It survived because the differential oracles could not see it: every block-step fixture
  > had **one** conditional per block, so no two branches ever shared a target — a
  > non-exhaustive fixture, not a fake oracle. It was found by adversarial review of the W-1
  > windowed driver (which had faithfully mirrored the shipped bug), not by the suite.
  >
  > Now implemented in **both** `blockstep_reconstruct` (region form) and `window_block_walk`
  > (windowed): count candidates, hard-stop at the first always-taken instruction, and on >1 —
  > or on any decode failure / ceiling hit, since uniqueness then cannot be *proven* — record
  > the definite prefix and set `truncated`. A `ret`/indirect hard-stops the scan but is not
  > itself a candidate, else every taken conditional followed by the function's `ret` would go
  > ambiguous. Cost is decoding, not stops: the measured 6.4x stop reduction is unchanged.
  > `trace_auto` gains from this for free — it escalates on `!truncated`, so ambiguous code
  > now falls through to the exact per-instruction tier instead of returning a short stream as
  > complete. Covered by dual-guard fixtures in both oracles plus an ordered-subsequence
  > assert (block-step may never invent an address); mutation-verified — restoring the greedy
  > rule fails `ok 336` and `ok 369`.
- Reuse the two existing arch seams unchanged — `read_pc_ret`
  ([ptrace_backend.c:322](../../../src/ptrace_backend.c)) to read the target RIP at each
  `#DB`, and `PTRACE_TRACE_ARCH` ([ptrace_backend.c:302](../../../src/ptrace_backend.c)) for
  the Capstone arch — and preserve the initial-in-region-PC capture
  ([ptrace_backend.c:815](../../../src/ptrace_backend.c)) so offset 0 is not missed.
- **Filter non-branch stops.** The `#DB` also fires on interrupts/exceptions (AMD APM
  Vol 2 §13.2), so a stop whose RIP is still inside the current straight-line run (not a
  control-transfer target) must be skipped, not treated as a block edge.
- **Arch asymmetry.** `PTRACE_SINGLEBLOCK` is an x86-only kernel feature; on aarch64 there
  is no equivalent, so the block-step entry must fall back to the existing per-insn
  `PTRACE_SINGLESTEP` path (a naive `#define STEP_REQUEST` swap is wrong). Add an x86-only
  `asmtest_ptrace_blockstep_available()` probe distinct from the aarch64 `probe_singlestep`
  self-skip ([ptrace_backend.c:387](../../../src/ptrace_backend.c)).
- **Public surface.** Add, mirroring the existing `_call` / `_attached` family:
  `asmtest_ptrace_blockstep_available`, `asmtest_ptrace_trace_call_blockstep`,
  `asmtest_ptrace_trace_attached_blockstep` to
  [include/asmtest_ptrace.h](../../../include/asmtest_ptrace.h), with matching ENOSYS stubs in
  the non-supported-host block ([ptrace_backend.c:915](../../../src/ptrace_backend.c)) for
  symbol parity. (New signature over an opts-flag on the existing entries, which would
  ABI-break the shipped 4-symbol family.) Follow the `ASMTEST_PTRACE_*` return codes
  ([asmtest_ptrace.h:60](../../../include/asmtest_ptrace.h)). **Scoped out:** no
  `_trace_attached_versioned_blockstep` mirror of the JIT/time-correct-bytes lane
  ([ptrace_backend.c:948](../../../src/ptrace_backend.c)) — the HW-attributed managed-runtime
  lane is Phase 3's eBPF snapshot; add a versioned block-step only if a rootless
  JIT-on-Zen-2 need surfaces.

**Acceptance.** `make hwtrace-test` (or a new `blockstep-test` mirroring the W2
single-step test) shows the block-step stream is **byte-identical** — `insns[]` and
`blocks[]` — to the per-instruction `PTRACE_SINGLESTEP` path on the existing loop
fixtures, with a materially lower stop count; runs live under a **plain** `docker run` (no
`--cap-add`), and self-skips on aarch64/non-x86 with a clear reason.

## Improvement Phase 3 — Software-event / eBPF on-demand LBR snapshot (`src/hwtrace.c` + optional BPF) *(landed 2026-07-06)*

> **Status (2026-07-06): LANDED — including the marker-path opt-in follow-up.** The
> deterministic capture (`asmtest_amd_snapshot_trace`, [src/branchsnap.c](../../../src/branchsnap.c))
> and its substrate probe landed earlier. The **`snapshot: true` opt-in** this section's
> follow-up scoped now ships: the capture is split into `asmtest_amd_snapshot_begin` /
> `asmtest_amd_snapshot_end` (a single process-global armed slot, matching hwtrace.c's
> single-active-region invariant), and `hwtrace_begin_amd`/`hwtrace_end_amd`
> ([src/hwtrace.c](../../../src/hwtrace.c)) route the **ordinary region begin/end markers**
> there when `opts.snapshot` is set — deriving the exit breakpoint from the region's last
> `ret` (`amd_last_ret_off`) and falling back to the `sample_period=1` path on any arm
> failure (no BPF toolchain/caps/LbrExtV2, no decodable ret). So a caller gets the
> deterministic boundary snapshot through the plain begin/end surface, not just the
> standalone entry point. Validated: `test_branchsnap` grew a marker-path case (the tiny
> single-shot routine reconstructs its entry block through `begin`/`end`), green in
> `make docker-hwtrace-codeimage`; the clean fallback is asserted in `test_amd_live`
> (`make docker-hwtrace-amd`, built without libbpf → honest sampled result). `opts.snapshot`
> is documented for both backends in [asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h).

**Goal.** Read the 16-entry LbrExtV2 stack **deterministically at the region boundary** via
`bpf_get_branch_snapshot()` (`amd_pmu_v2_snapshot_branch_stack`), eliminating the two live
Zen-5 failure modes Part I documents: the tiny single-shot routine "too fast to be sampled
in-region," and the post-routine-glue contamination that forced the richest-in-region
heuristic ([hwtrace.c:465-474, 542-548](../../../src/hwtrace.c)). Also the natural
HW-attributed managed-runtime lane on AMD (a uprobe on a JITted method), which today falls
to W2 ptrace single-step.

**Work.**
- Add a BPF program (uprobe/`fentry` at region entry/exit) that calls the snapshot helper
  and delivers the frozen window to userspace, decoded by the existing `asmtest_amd_decode`
  ([amd_backend.c:104](../../../src/amd_backend.c)) — no `sample_period=1` ring, no throttle
  exposure, no richest-window scan.
- **Optional-library lane.** Gate the BPF object exactly like the existing codeimage eBPF
  detector: set `-DASMTEST_HAVE_LIBBPF` + skeleton only when `pkg-config libbpf` **and**
  `clang` **and** `bpftool` are all present
  ([mk/native-trace.mk:144-165](../../../mk/native-trace.mk)); otherwise compile the TU
  without the `-D`, invoke no BPF toolchain, and have the lane's `available()` return 0
  (self-skip) — the same shape as `CODEIMAGE_SKEL` / `LIBIPT_DEF`.
- **Gates.** Zen 4/5 (perfmon v2) only — Zen 3 BRS and Zen 2 do not route through
  `amd_pmu_v2_handle_irq` and return no snapshot; Linux ≥ 6.10; `CAP_PERFMON` + `CAP_BPF`.
  All expressed as `available()`-time self-skips with reasons.

**Acceptance.** A new `make docker-hwtrace-lbr-snapshot` lane (mirroring
`docker-hwtrace-codeimage` at [mk/docker.mk:254](../../../mk/docker.mk): dedicated Dockerfile
with clang+libbpf+bpftool, `--cap-add=BPF --cap-add=PERFMON --cap-add=SYS_PTRACE
--security-opt seccomp=unconfined`, BTF kernel — **not** `--privileged`) captures a **tiny
single-shot routine** that the sampling path marks `truncated`, and reconstructs it
complete. Self-skips cleanly without libbpf / the caps / a Zen 4+ host.

> **Superseded 2026-07-16 — `docker-hwtrace-lbr-snapshot` was never built and does not
> exist.** The dedicated lane specced here proved unnecessary: the existing
> `docker-hwtrace-codeimage` image already carries the clang+libbpf+bpftool toolchain and
> the BPF/PERFMON caps, so it simply runs `branchsnap-test` after `codeimage-test`, and that
> is where this acceptance is met on the Zen 5 dev box. The Deliverables section records the
> same decision; the stale "adds `docker-hwtrace-lbr-snapshot`" line under Validation is
> corrected there. **Do not add the lane** — `make docker-hwtrace-codeimage` is the one to
> run.

## Improvement Phase 4 — LbrExtV2 speculation-bit filtering (`src/amd_backend.c`) *(landed 2026-07-06)*

> **Status (2026-07-06): LANDED and validated on the Zen 5 dev box.** `amd_replay`
> ([src/amd_backend.c](../../../src/amd_backend.c)) now drops a record whose
> `perf_branch_entry.spec == PERF_BR_SPEC_WRONG_PATH` (a speculative, never-retired
> phantom edge) right after the existing `abort` guard — and dropping it is expected, so
> it does **not** set `truncated`. The `spec` bitfield only exists on Linux ≥ 6.1
> headers, so access is gated on `-DASMTEST_HAVE_PERF_BR_SPEC`, set by a
> `-fsyntax-only` struct-member probe in [mk/native-trace.mk](../../../mk/native-trace.mk)
> (mirroring `LIBIPT_DEF`); without it the filter compiles out — a no-op, exactly as on
> Zen 3 BRS (retired-only, no spec bits) and non-Linux. `amd_edge_eq` is left untouched
> (its from+to-only overlap semantics the stitcher depends on). Validated
> host-independently by `test_amd_spec_filter`
> ([examples/test_hwtrace.c](../../../examples/test_hwtrace.c)): a wrong-path phantom whose
> target would add a spurious block is dropped, leaving a byte-identical trace and no
> truncation — green in `make docker-hwtrace-amd` on the Ryzen 9 9950X.

**Goal.** Drop speculative / wrong-path phantom edges before replay. Today `amd_replay`
filters only aborts (`if (e->abort) continue;`, [amd_backend.c:67-68](../../../src/amd_backend.c))
and `amd_edge_eq` ([amd_backend.c:124](../../../src/amd_backend.c)) explicitly ignores every
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
> ([src/amd_backend.c](../../../src/amd_backend.c)) now takes `base`/`base_ip`/`len` (threaded
> through the [hwtrace.c](../../../src/hwtrace.c) call site and the non-Linux stub) and, before
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
> The AMD data ring default grew 64KB → 256KB ([hwtrace.c](../../../src/hwtrace.c)), extending
> gapless stitch reach before the kernel drops the newest samples; the
> `PERF_RECORD_LOST`/throttle → `lost` detection is unchanged. (3) **`data_size` doc fix.**
> The [asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) comment now documents both backend
> defaults (Intel PT 8KB, AMD 256KB) instead of the stale single "0=8KB". Validated by
> `test_amd_stitch_decodable` ([examples/test_hwtrace.c](../../../examples/test_hwtrace.c)) — a
> decodable contiguous splice is accepted, an indecodable one yields an honest gap — plus the
> existing stitch/drain fixtures staying green (no over-rejection); `make docker-hwtrace-amd`
> on the Ryzen 9 9950X.

**Goal.** Reduce silent mis-stitches and extend Tier-B reach before honest truncation,
without pretending an unavailable HW facility exists (Intel's `hw_idx`-based stitch cannot
port — AMD sets `hw_idx ≡ 0`; this is a confirmed dead end).

**Work.**
- **Decodable-distance invariant.** In `asmtest_amd_stitch`'s smallest-overlap loop
  ([amd_backend.c:173-188](../../../src/amd_backend.c)), after a from+to overlap `match` and
  before accepting the shift, require that the straight-line span between consecutive
  stitched edges is Capstone-decodable with reconstructed instruction count == static decode
  distance; reject the shift and keep searching otherwise. This needs `base`/`base_ip`/`len`
  threaded into `asmtest_amd_stitch` (which has no code access today) — a signature change
  mirrored in the non-Linux stub ([amd_backend.c:249](../../../src/amd_backend.c)) and the call
  site ([hwtrace.c:576](../../../src/hwtrace.c)), and it inherits the Capstone
  decoder-present gating.
- **Ring / throttle hardening.** Grow the AMD data ring (`g_opts.data_size` / `round_pages`
  default `64*1024`, [hwtrace.c:433](../../../src/hwtrace.c)) and document requiring
  `kernel.perf_event_max_sample_rate` raised and `kernel.perf_cpu_time_max_percent=0` on the
  self-hosted runner. Must **not** remove the `PERF_RECORD_LOST` / `PERF_RECORD_THROTTLE` →
  `lost` detection ([hwtrace.c:503-505](../../../src/hwtrace.c)), which is the only "run did not
  fit" signal surviving windows cannot show. Fix the `data_size` header comment
  ([asmtest_hwtrace.h:65](../../../include/asmtest_hwtrace.h)), which says `0=8KB` but is
  backend-dependent — the AMD default is 64 KB ([hwtrace.c:433](../../../src/hwtrace.c)) while
  Intel PT keeps 8 KB ([hwtrace.c:645](../../../src/hwtrace.c)); document **both** defaults
  rather than flipping the single number (which would then be wrong for Intel PT).

**Acceptance.** A synthetic self-overlapping-loop fixture that the current smallest-overlap
heuristic mis-stitches now either stitches correctly or reports an honest gap (`truncated`);
`make docker-hwtrace-amd`'s 20 000-trip loop reconstructs further before truncating with the
enlarged ring.

## Improvement Phase 6 — BRS period-adjust single-window Tier-A capture (`src/hwtrace.c`) *(forward-look — HARDWARE-BLOCKED on Zen 3 / Family 19h BRS)*

> **The one open item in this plan (re-confirmed 2026-07-16).** It is blocked on silicon,
> not on effort or design: BRS exists **only on AMD Family 19h "Zen 3"**, and neither dev box
> is one — they are a **Ryzen 9 9950X (Family 1Ah, Zen 5, `amd_lbr_v2`)** and a **Ryzen 9
> 4900HS (Family 17h, Zen 2, no branch stack at all)**. Zen 4/5 do not use BRS (they use
> LbrExtV2, where the Phase-3 software-event snapshot is the better lever anyway), and Zen 2
> has no branch hardware to period-adjust. Per the house "no untested hardware code" rule it
> is **not implemented** — verified absent: no `amd_brs` / `branch-brs` / `brs_adjust` /
> `brs_period` symbol exists anywhere in `src/` or `include/`. It stays fully specified below
> for whenever a Zen 3 host is available, and is the sole reason this plan remains in
> `plans/` rather than `archive/plans/`.

**Goal.** On **Zen 3 BRS**, for a routine known to take ≤ ~16 taken branches, replace the
`sample_period=1` PMI-per-branch flood with a single fixed-period frozen overflow (the
kernel already programs `period − lbr_nr` via `amd_brs_adjust_period`), cutting Tier-A
capture from O(branches) interrupts to one and removing the throttle/ring exposure.

**Work / caveat.** `sample_period=1` is **load-bearing** for the current design: the Tier-B
stitch and the richest-in-region heuristic ([hwtrace.c:477-486, 584-614](../../../src/hwtrace.c);
[amd_backend.c:130-135](../../../src/amd_backend.c)) assume a sample at *every* taken branch so
consecutive 16-deep windows overlap by 15 edges. A fixed BRS period breaks that overlap. So
this is **not** a blanket change to `hwtrace_begin_amd` ([hwtrace.c:426](../../../src/hwtrace.c))
— it must be a **distinct Tier-A capture mode**, selected only when the region is known-small
and the Tier-B path is not needed, and kept off Zen 4/5 (where the Phase-3 software-event
snapshot is the better lever). The `period=1` appears in two places (probe
[hwtrace.c:186](../../../src/hwtrace.c), capture [hwtrace.c:420](../../../src/hwtrace.c)); a mode
switch must keep them coherent or justify the divergence.

**Acceptance.** *(forward-look — deferred until a Zen 3 host is available; the dev box is
Zen 5.)* On a Zen 3 BRS host, a ≤16-branch routine reconstructs identically to the
`sample_period=1` path with a single PMI; the Tier-B stitch path is untouched.

## Improvement Phase 7 — IBS-Op complementary coverage lane (`src/hwtrace.c` + new backend) *(SUPERSEDED + LANDED via [zen2-ibs-tracing-plan.md](../archive/plans/zen2-ibs-tracing-plan.md))*

> **Superseded by [zen2-ibs-tracing-plan.md](../archive/plans/zen2-ibs-tracing-plan.md) (Phases 0–4 landed
> 2026-07-12).** Two mechanics below were **refuted empirically on the Zen 2 host** and are
> corrected in the superseding plan: the branch target arrives in the perf
> `PERF_SAMPLE_RAW` record (`reg[7] IbsBrTarget`, delivered whenever `IBS_CAPS_BRNTRGT` is
> set) — **no `MSR_AMD64_IBSBRTARGET` read** — and with the kernel `swfilt` bit
> (`ibs_op/format/swfilt`) user-only IBS opens **unprivileged at `perf_event_paranoid=2`**
> — **no `CAP_PERFMON`/`CAP_SYS_ADMIN`**. Shipped: the `asmtest_ibs_*` API
> (`include/asmtest_ibs.h`, `src/ibs_backend.c`), `asmspy --sample` + TUI mode 7, and the
> whole-window survey fallback. The statistical caveats below hold and are carried forward
> verbatim. Kept for the record; do not implement from this section.

**Goal.** The **only** hardware branch source on **Zen 2** (which has no branch stack): a
statistical, precise-IP source→target edge ~~via `MSR_AMD64_IBSBRTARGET`~~ (gated on
`IBS_CAPS_BRNTRGT`), used as a *coverage-confirmer* / hot-edge pre-cover to shrink — not
bound — the Phase-2 block-step / DynamoRIO residual, and as an indirect-branch-target
resolver on Zen 3/4/5.

**Work / caveat.** IBS is **statistical** (one tagged op per NMI, worse edge-yield than
LBR) and cannot produce an ordered, complete path, so it is explicitly *not* a replacement
for the branch stack and must never feed the `insns[]`/`blocks[]` parity contract as if
complete. ~~It needs `CAP_PERFMON`/`CAP_SYS_ADMIN` (no user/kernel filter).~~ **Corrected:
user-only IBS opens unprivileged via `swfilt` — see the superseding plan.** If exposed, it is
a *separate diagnostic producer*, not a member of the fidelity cascade. Two raw proposals
were refuted in verification and must be avoided: `rand_en` is an IBS-*Fetch* knob (not Op),
and the branch target is capability-gated and valid only for retired taken branches.

**Acceptance.** *(met by the superseding plan.)* On a Zen 2 host, IBS-Op edges
mark a subset of region basic blocks as covered ~~and reduce the number of blocks the
block-step fallback must walk~~ (the block-step pre-cover integration remains that plan's
forward-look Phase 6); self-skips without `IBS_CAPS_BRNTRGT`.

> **Cross-reference corrected 2026-07-16.** The parenthetical above is wrong on two counts.
> [zen2-ibs-tracing-plan.md](../archive/plans/zen2-ibs-tracing-plan.md) has **no forward-look phase** — all
> eight (0–7) are landed — and its **Phase 6 is *edge → basic-block normalization***, which
> shipped as `asmtest_ibs_normalize_blocks` / `asmtest_ibs_blocks_free`
> ([include/asmtest_ibs.h](../../../include/asmtest_ibs.h)). Marking region blocks as
> statistically covered is therefore **done**. What is *not* owned by any phase in either
> plan is the narrower **block-step pre-cover integration** — feeding that covered-block set
> into the Phase-2 block-step / DynamoRIO fallback to shrink its residual. It remains
> unscheduled and unowned; it is pure software (no silicon gate) if it is ever wanted.

## Improvement Phase 8 — Cascade composition: MSR-direct escalation rung before block-step (`src/trace_auto.c`) *(landed 2026-07-10)*

> **Status (2026-07-10): LANDED and validated on the Zen 5 dev box.** The MSR-direct rung ships
> in `asmtest_trace_call_auto` ([src/trace_auto.c](../../../src/trace_auto.c)), inserted after the
> fast-tier completion gate and before the BTF block-step tier. It runs only when the fast sampled
> tier came back truncated/absent, is gated on `asmtest_amd_msr_available()` (a true `/dev/cpu/N/msr`
> privilege probe that also self-skips off x86-64 Linux), and is excluded under
> `ASMTEST_TRACE_CEILING_FREE` (it shares AMD_LBR's 16-deep ceiling). A small `msr_call_closure` +
> `void(void*)` trampoline recovers the routine's `long` result across `asmtest_amd_msr_trace`'s
> callback boundary; `call_auto_reset` runs before the attempt and any miss/failure sets `truncated`
> and falls through to block-step (never an early return on a failed tier). The in-process re-run
> caveat holds — a second real execution, like the fast begin/end tier it sits beside. **Validated**
> in the privileged MSR lane (`make docker-hwtrace-msr`, `--privileged`): a tiny routine the sampled
> tier truncates is reconstructed complete by the rung (`used->backend == AMD_LBR`, zero PMU
> interrupts) *before* block-step, `CEILING_FREE` excludes it, and the existing `trace_call_auto`
> cases are unchanged — `test_call_auto` case (c),
> [examples/test_hwtrace.c](../../../examples/test_hwtrace.c); 361/361 green on the Ryzen 9 9950X.

> **Amended 2026-07-16 — the rung's *rationale* narrowed; the rung itself is unchanged and
> stays.** This phase was motivated (Part II, and the Work section below) by the MSR read
> being the only path that could complete a **multi-exit** tiny routine, since the
> default-on boundary snapshot planted a single breakpoint at the region's last exit.
> `e9ca70e` removed that limitation: the snapshot now plants **one breakpoint per exit, up
> to `ASMTEST_AMD_MAX_EXITS == 4`** ([src/hwtrace.c:777-783](../../../src/hwtrace.c)), so
> the fast tier already catches 1..4-exit routines through the marker path. **The rung is
> still correct and still earns its place** — its residual is simply smaller and more
> precisely stated than the text below claims:
> - regions with **more than 4** exits (deliberately left on the sampled path by default —
>   there are only four x86 debug registers);
> - any snapshot **arm failure**: no BPF toolchain, missing `CAP_BPF`/`CAP_PERFMON` at load,
>   a debugger already holding a needed debug register (the arm is all-exits-or-nothing), or
>   no decodable exit;
> - substrates where the snapshot is unavailable outright but `/dev/cpu/N/msr` is not — the
>   snapshot needs Zen 4/5 perfmon-v2 + Linux ≥ 6.10 + BPF caps, a strictly narrower floor
>   than the MSR read's root/`CAP_SYS_ADMIN` + `msr` module.
>
> In all three the fast tier still returns truncated and the rung still completes the
> too-fast tiny routine with zero PMU interrupts before block-step's ~1000× cost. Read every
> "multi-exit" justification below as "**>4**-exit, or snapshot-unavailable". Nothing about
> the shipped code, its gating, or its acceptance changes.

**Goal.** Insert an **MSR-direct escalation rung** into `asmtest_trace_call_auto` between the
fast sampled-hwtrace tier and the BTF block-step tier. When the fast tier returns a
**truncated** AMD LBR capture — `ran == 1` but `trace->truncated == true`, so the completion
gate at [trace_auto.c:175](../../../src/trace_auto.c) falls through — the region was too fast
to be sampled in-region yet its retired path may still fit one 16-deep MSR read. Try the
standalone deterministic entry `asmtest_amd_msr_trace`
([msr_lbr.c:110](../../../src/msr_lbr.c)) *before* dropping to the block-step tier, so the
too-fast tiny routine is reconstructed complete with **zero PMU interrupts** instead of paying
block-step's fork-per-attempt, ~1000× stop cost. This is the Part II
[Newly surfaced](#newly-surfaced-2026-07-09-review) composition gap, deliberately
**MSR-scoped** (below).

**Work.**
- **Insert the rung at exactly one point** — after the fast-tier completion gate
  `if (ran && !trace->truncated) return ASMTEST_HW_OK;`
  ([trace_auto.c:175](../../../src/trace_auto.c)) and before the `(2) … BTF block-step` block
  ([trace_auto.c:181](../../../src/trace_auto.c)). It therefore runs **only** when the fast
  tier came back truncated or absent, and its own truncation/failure must **fall through into
  block-step** — never `return` early on its own miss.
- **Why the boundary snapshot is NOT a rung (deliberate scope).** *(As written 2026-07-09;
  the "single-exit" framing is now "1..4-exit" — see the amendment above. The conclusion —
  the snapshot is not a rung — is unchanged and if anything stronger, since the default-on
  gate is wider.)* The fast tier drives the
  marker path, and `hwtrace_begin_amd` already selects the Phase-3 boundary snapshot **by
  default** for a single-exit region on the supporting substrate
  ([hwtrace.c:655](../../../src/hwtrace.c), Matrix 2 #3 — landed) — so on the single-exit case
  the snapshot already ran *inside the fast tier*. A cascade re-run gated on the same
  `amd_nret == 1` substrate is provably redundant: if that snapshot completed there is no
  truncation to escalate, and if it overflowed the 16-deep stack, re-running cannot help. The
  snapshot's genuine improvement is *widening the default-on gate to tail-call exits* — the
  Phase 9 item — not a rung here. The MSR read is different in kind: it is in **neither** the
  marker path nor the cascade today, needs no exit breakpoint (it freezes via `wrmsr` after
  the routine returns), and so also covers the **multi-exit** tiny routine the snapshot's
  single planted breakpoint cannot.
- **`run_fn(void *)` trampoline.** `asmtest_amd_msr_trace` takes an out-of-line
  `void (*run_fn)(void *)` that *it* invokes ([msr_lbr.c:159](../../../src/msr_lbr.c)) — it
  does **not** take `(base, len, args)`, and `run_fn` returns `void`. So the rung needs a
  small closure `struct { const void *code; const long *args; int nargs; long result; }` plus
  a `static void` trampoline that calls the existing `call_auto_invoke(code, args, nargs)`
  ([trace_auto.c:115](../../../src/trace_auto.c)) and stores its `long` into the closure —
  the only way to recover the routine's result across the void callback. This inherits the
  fast tier's integer-SysV-only limitation (`long a[6]`); FP/xmm-argument routines remain
  unsupported, same as today.
- **Re-run semantics + honesty rule.** The fast tier already executed `code(args)` once
  in-process (`ran = 1` even when truncated), and `asmtest_amd_msr_trace` invokes `run_fn`
  **in-process** — not fork-isolated like the block-step/single-step tiers that follow. An
  MSR attempt is therefore a **second real execution in the tracer's own address space**:
  non-idempotent side effects happen again and a `SIGSEGV` in the routine crashes the whole
  tracer — a semantic gap versus the fork-sandboxed tiers below it, but *no worse than* the
  fast begin/end tier the rung sits beside. Mirror the ptrace pattern exactly: call
  `call_auto_reset(trace)` ([trace_auto.c:126](../../../src/trace_auto.c)) **before** the
  attempt so the truncated fast-tier trace's leftover `insns[]`/`blocks[]`/`truncated` bit
  cannot contaminate the MSR result, and on a failed or truncated attempt set
  `trace->truncated = true` and continue — a failed tier must never be read as
  empty-yet-complete. Set `used->backend` only on a *complete* win.
- **`ASMTEST_TRACE_CEILING_FREE` skips the rung.** The MSR read is the same 16-deep LbrExtV2
  Tier-A family with the same window-completeness ceiling as `AMD_LBR`, which `CEILING_FREE`
  already drops from the fast pick (the `hp` mapping,
  [trace_auto.c:151](../../../src/trace_auto.c)) and from the static cascade
  ([trace_auto.c:74](../../../src/trace_auto.c)). Wrap the rung in
  `if (!(policy & ASMTEST_TRACE_CEILING_FREE)) { … }` so the ceiling-free contract excludes
  it in lock-step with `AMD_LBR`.
- **Gate on the genuine privilege probe; stay honest about reach.**
  `asmtest_amd_msr_available` ([msr_lbr.c:93](../../../src/msr_lbr.c)) opens `/dev/cpu/N/msr`
  `O_RDWR`, so it is a true privilege/device gate (root / `CAP_SYS_ADMIN` + the `msr`
  module) — when it returns 1 the path is genuinely present; it returns 0 off x86-64 Linux,
  so the rung self-skips cleanly on every other lane. Reach caveat: the user-space glue
  branches between `run_fn` returning and the freezing `wrmsr`
  ([msr_lbr.c:12-18](../../../src/msr_lbr.c)) occupy the newest slots of the same 16-deep
  window, so the read completes only **very small** routines — and the existing
  nothing-in-region check already converts a miss to an honest `truncated`
  ([msr_lbr.c:190-193](../../../src/msr_lbr.c)), which then falls through to block-step.

**Why this is worth a rung.** For the too-fast-to-sample tiny routine — including the
**multi-exit** one that the default-on snapshot must skip *(read: the **>4**-exit one, or
any region whose snapshot arm failed / whose substrate lacks it — see the 2026-07-16
amendment above; at 1..4 exits the fast tier now catches it)* — this rung reconstructs the
trace complete with zero PMU interrupts instead of falling straight to fork-isolated
block-step, avoiding its roughly three-orders-of-magnitude stop-count cost on hosts with
`msr` access (the dev box, self-hosted runners). The scope is honest: it is a narrow,
privilege-gated rung, not a new tier — but the entry point already ships, so the
composition is cheap.

**Acceptance.** A multi-exit tiny fixture routine that the sampled fast tier marks
`truncated` is reconstructed **complete** by `asmtest_trace_call_auto` via the MSR rung
**without** invoking block-step (assert `used->backend` and that the block-step/single-step
tiers were not reached); the rung self-skips cleanly to block-step where `/dev/cpu/msr` is
absent (unprivileged lanes, non-Zen-4+/non-x86 hosts) and is skipped entirely under
`ASMTEST_TRACE_CEILING_FREE`; and every existing `trace_call_auto` lane — including the
truncated-then-block-step and pure single-step floors — is **unchanged**. Validated in the
privileged AMD lane on the Zen 5 dev box (the `docker-hwtrace-amd` shape plus the `msr`
device), self-skipping in hosted CI.

> **Note (2026-07-16) on re-running this acceptance.** It was met as written on 2026-07-10
> (`test_call_auto` case (c)), when *any* multi-exit region bypassed the default-on snapshot.
> Since `e9ca70e` a 2..4-exit region is caught by the snapshot **inside the fast tier**, so
> it no longer truncates and would no longer reach the rung. A fixture written to re-prove
> this acceptance today must therefore use a region the fast tier genuinely cannot complete —
> **more than 4 exits**, or a substrate/arm-failure case where the snapshot does not run —
> otherwise it asserts the wrong tier. The shipped rung and its existing test are unaffected;
> this is a note for whoever next touches the fixture.

## Improvement Phase 9 — Correctness & hygiene cluster (2026-07-09 review) (`src/amd_backend.h`, `src/hwtrace.c`, `src/msr_lbr.c`) *(landed 2026-07-10)*

> **Status (2026-07-10): LANDED and validated on the Zen 5 dev box.** Both `[correctness]` fixes
> and the hygiene cluster ship. (1) **MSR spec leak fixed at the source:** the raw FROM/TO decode is
> extracted to a pure `asmtest_amd_msr_decode_entry` ([src/msr_lbr.c](../../../src/msr_lbr.c)) that
> keeps only retired entries (`TO[63]`), dropping the spec-only wrong-path slot (`TO[62]=1, TO[63]=0`)
> the downstream `PERF_BR_SPEC_WRONG_PATH` filter could never catch — verified against the kernel's
> `struct branch_entry` bit layout. (2) **Tail-call snapshot widening:** the default-on gate's exit
> deriver `amd_last_ret_off` → `asmtest_amd_last_exit_off` ([src/hwtrace.c](../../../src/hwtrace.c))
> now counts a region-leaving direct uncond `jmp` as an exit, guarding out indirect and in-region
> jmps. *(Generalised further 2026-07-12 by `e9ca70e`: the deriver is now `asmtest_amd_all_exits`,
> collecting **every** exit — `ret`-class and tail-`jmp` alike — and the arm plants a breakpoint at
> each, up to `ASMTEST_AMD_MAX_EXITS == 4`, so the `nret == 1` gate this item widened no longer
> exists. `asmtest_amd_last_exit_off` survives as the >4-exit / explicit-`opts.snapshot`
> last-exit fallback. The Work item below describes the 2026-07-10 shape.)* (3) **Shared internal header:** new [src/amd_backend.h](../../../src/amd_backend.h) is the one
> source for the `amd_*` decls, the folded `ASMTEST_AMD_REDUCED_FILTER`, and the re-exported
> `ASMTEST_HW_*` codes — the four hand-copied inline decl blocks, the duplicated macro, and the local
> error-code redefs (amd_backend.c, ss_backend.c) are deleted, so a signature/constant drift is now a
> compile error. (4) `PERF_EVENT_IOC_ENABLE` is return-checked at all six arm sites and the four
> perf-ring drain loops read the header/`nr` via `memcpy` (strict-aliasing). **Validated**
> host-independently by `test_amd_msr_spec_filter` + `test_amd_tailcall_exit`
> ([examples/test_hwtrace.c](../../../examples/test_hwtrace.c)) and live: `make docker-hwtrace-amd`
> 352/352, `make docker-hwtrace-codeimage` branchsnap green (the real libbpf path — reduced-filter
> macro from the shared header, ioctl checks), bindings-parity OK (114×10).

**Goal.** Close the two correctness gaps the 2026-07-09 AMD review surfaced — the MSR-direct path
admitting speculative wrong-path LBR entries that `amd_replay`'s `PERF_BR_SPEC_WRONG_PATH` filter
can never see, and the deterministic-snapshot default-on gate skipping a genuinely single-exit
*tail-call* routine — and consolidate the surrounding hygiene (a byte-duplicated branch-filter
macro, inline `amd_*` forward declarations with no shared header, locally re-`#define`d error codes,
unchecked `PERF_EVENT_IOC_ENABLE`, and strict-aliasing perf-ring casts) behind one new internal
header so a future signature or constant drift becomes a **compile error** rather than silent UB.
Every item is value-preserving on the shipped build except the two `[correctness]` fixes; the
refactors are a no-op the test output must confirm.

**Work.** *(As specced 2026-07-09; all line refs below are pre-fix. As shipped, the raw
FROM/TO decode was extracted to the pure `asmtest_amd_msr_decode_entry`, so the validity
test now lives at [msr_lbr.c:99](../../../src/msr_lbr.c) and reads `if (!((to >> 63) & 1))
continue;` — the `|| spec` term dropped exactly as prescribed here.)*
- **[correctness] Drop speculative wrong-path entries at the MSR source.** In the MSR-direct capture
  ([msr_lbr.c:175](../../../src/msr_lbr.c)) the validity test is
  `int valid = ((t >> 63) & 1) || ((t >> 62) & 1); /* valid || spec */` — `TO[63]` is retired-valid
  and `TO[62]` is speculative (wrong-path), so the `|| spec` term **admits** a spec-only slot
  (`TO[63]==0, TO[62]==1`). Lines 179-184 then store its `from`/`to` but never set `br[n].spec`,
  which `memset(br, 0, sizeof br)` ([msr_lbr.c:165](../../../src/msr_lbr.c)) already zeroed — so
  the downstream drop in `amd_replay` (`if (e->spec == ASMTEST_PERF_BR_SPEC_WRONG_PATH) continue;`,
  [amd_backend.c:186](../../../src/amd_backend.c)) can never fire and the phantom edge leaks
  into the reconstruction. On Zen 4/5 LbrExtV2 (which records wrong-path branches) this is a genuine
  wrong-trace bug, reachable via `asmtest_amd_msr_trace` (`CAP_SYS_ADMIN` + the `msr` module) and
  data-dependent on a spec-only slot landing in the window. Fix at the source: drop the
  `|| ((t >> 62) & 1)` so line 175 reads `int valid = (t >> 63) & 1;` — a spec-only entry then fails
  `if (!valid) continue;` (176-177) and is skipped where it is read. **Prefer this over setting
  `br[n].spec`**: the downstream filter and the `ASMTEST_PERF_BR_SPEC_WRONG_PATH` constant live
  entirely inside `#ifdef ASMTEST_HAVE_PERF_BR_SPEC`
  ([amd_backend.c:162-164, 185-190](../../../src/amd_backend.c)), so on a pre-6.1-header build
  `perf_branch_entry` has no `.spec` member — a set-spec fix would both fail to help and fail to
  compile there, while the skip is correct under every build config.
- **[correctness] Extend the single-exit snapshot default to region-leaving tail-call jmps.** The
  deterministic-snapshot default-on gate
  (`if ((g_opts.snapshot || (asmtest_amd_snapshot_available() && amd_nret == 1)) && exit_off != (size_t)-1)`,
  [hwtrace.c:654-656](../../../src/hwtrace.c)) fires by default only when the region has exactly
  one exit and a valid exit offset, both derived from `amd_last_ret_off`
  ([hwtrace.c:614](../../../src/hwtrace.c)), whose sole exit predicate is `asmtest_disas_is_ret`
  ([hwtrace.c:625](../../../src/hwtrace.c)). A routine that exits via a tail-call `jmp target`
  (target *outside* `[base, base+len)`) has **zero** ret-class instructions, so it returns
  `(size_t)-1` with `*nret==0` — both halves of the gate fail and a genuinely single-exit routine is
  forced onto the sampled richest-window path, the exact "too-fast tiny routine honestly truncated"
  case the snapshot exists to fix ([branchsnap.c:1-12](../../../src/branchsnap.c)). Extend
  `amd_last_ret_off` in place (its only caller is `hwtrace_begin_amd`,
  [hwtrace.c:653](../../../src/hwtrace.c); renaming it `amd_last_exit_off` / `amd_nret`→
  `amd_nexit` and updating the [hwtrace.c:636-651](../../../src/hwtrace.c) comment block to say
  "last region-exit (ret or region-leaving direct jmp)" makes the broadened semantics honest):
  alongside the `is_ret` check, count offset `o` as an exit when
  `asmtest_disas_is_uncond_jump(...)==1` **AND** `asmtest_disas_branch_target(...)==1` **AND** the
  decoded target leaves the region (`tgt < base_ip || tgt >= base_ip + len`, the same predicate
  already at [amd_backend.c:255](../../../src/amd_backend.c) /
  [:360](../../../src/amd_backend.c)). Both guards are load-bearing:
  `asmtest_disas_is_uncond_jump` also returns 1 for an *indirect* `jmp r/m`
  ([disasm.c:304](../../../src/disasm.c)), and `asmtest_disas_branch_target` returns 1 only for
  a decodable-immediate *direct* branch ([disasm.c:419](../../../src/disasm.c)) — an
  indirect/jump-table tail call is unprovable and correctly stays on the sampled fallback (no
  regression: it gets `nret==0` today). An *in-region* direct uncond jmp (target inside the region)
  is an ordinary loop/forward branch and must **not** count, or a loopy single-ret region would be
  misclassified multi-exit. Both exit kinds bump the *same* counter, so the caller's `== 1` still
  means single-exit; a region with one `ret` **and** one tail-`jmp` is correctly two exits and
  default-on is withheld. **Timing subtlety (flag, don't guess):** the planted `HW_BREAKPOINT_X` at
  `base+exit_off` ([branchsnap.c:131](../../../src/branchsnap.c),
  [:176-179](../../../src/branchsnap.c)) is a `#DB` that fires when execution *reaches* the exit
  CTI, before it transfers, so the frozen stack read by `bpf_get_branch_snapshot`
  ([branchsnap.bpf.c:35](../../../bpf/branchsnap.bpf.c)) never contains the exit CTI's own edge —
  identical for a `ret` and a tail-`jmp`, and `amd_replay` never needs that edge (it stops at the
  last recorded branch's target, [amd_backend.c:289-291](../../../src/amd_backend.c)). The Zen-5
  non-eviction property (a `#DB` not evicting in-region branches before the snapshot read,
  [branchsnap.c:14-15](../../../src/branchsnap.c)) is validated for a `ret` exit and *expected*
  to hold for a tail-`jmp` by the identical mechanism, but the plan carries it as an assumption to
  re-confirm on the target substrate.
- **[hygiene / latent-UB] Introduce a shared `src/amd_backend.h` internal header.** The `amd_*`
  prototypes are forward-declared inline — in `hwtrace.c` a `perf_branch_entry`-typed block under
  `#if defined(__linux__) && defined(__x86_64__)` ([hwtrace.c:90-119](../../../src/hwtrace.c):
  `asmtest_amd_decode` 91, `_stitch` 96, `_decode_stitched` 101, `_snapshot_begin` 116, `_snapshot_end`
  118) plus the unconditional `_decoder_present`/`_freeze_available`/`_snapshot_available` at
  [hwtrace.c:152-159](../../../src/hwtrace.c), and the same decls hand-copied into
  [branchsnap.c:29-32](../../../src/branchsnap.c) and
  [msr_lbr.c:28-31](../../../src/msr_lbr.c). A signature change in `amd_backend.c` would compile
  and link cleanly yet be UB at the call boundary. Create `src/amd_backend.h` mirroring the
  `#ifndef`-guarded, ships-nothing-in-`include/`, no-ABI-promise pattern of
  [src/stealth_helper.h](../../../src/stealth_helper.h), reproducing `amd_backend.c`'s **exact**
  platform split: the `perf_branch_entry`-typed prototypes under the `__linux__ && __x86_64__` guard
  (matching the definitions at [amd_backend.c:294](../../../src/amd_backend.c) /
  [400](../../../src/amd_backend.c) / [483](../../../src/amd_backend.c)) with the
  `void*`-typed stubs in the `#else` (matching [amd_backend.c:506](../../../src/amd_backend.c) /
  [516](../../../src/amd_backend.c) / [532](../../../src/amd_backend.c)) — exactly why
  `hwtrace.c` guards them today — and the stable-signature `_lbr_depth` / `_decoder_present` /
  `_freeze_available` / `_snapshot_available` / `_snapshot_begin` / `_snapshot_end` unconditional.
  Have it `#include "asmtest_hwtrace.h"` so it re-exports the `ASMTEST_HW_*` codes, then include it
  from `hwtrace.c`, `amd_backend.c`, `branchsnap.c`, and `msr_lbr.c`, deleting all four inline decl
  blocks; a definition/decl mismatch then becomes a compile error.
- **[hygiene] Fold the duplicated `ASMTEST_AMD_REDUCED_FILTER` into the new header.** The reduced
  branch-filter macro is defined byte-identically twice —
  [hwtrace.c:595-598](../../../src/hwtrace.c) (used at
  [hwtrace.c:691](../../../src/hwtrace.c)) and
  [branchsnap.c:52-55](../../../src/branchsnap.c) (used at
  [branchsnap.c:155](../../../src/branchsnap.c)), carrying the explicit "kept in sync" hand-note
  ([hwtrace.c:593](../../../src/hwtrace.c), [branchsnap.c:47-51](../../../src/branchsnap.c)).
  No live defect — the copies are identical today — but the risk is silent future drift. Define it
  once in `src/amd_backend.h` under `#if defined(__linux__) && defined(__x86_64__)` with
  `#include <linux/perf_event.h>` inside that guard so the `PERF_SAMPLE_BRANCH_*` tokens resolve, and
  delete both textual copies.
- **[hygiene] Reuse `asmtest_hwtrace.h`'s error codes instead of local `#define`s.** `amd_backend.c`
  re-`#define`s three codes ([amd_backend.c:32-34](../../../src/amd_backend.c):
  `ASMTEST_HW_OK`/`ENOSYS`/`EDECODE`) and `ss_backend.c` four
  ([ss_backend.c:67-70](../../../src/ss_backend.c): `OK`/`EINVAL`/`EFULL`/`ENOSYS`), both
  duplicating [asmtest_hwtrace.h:51-57](../../../include/asmtest_hwtrace.h); the two are the
  outliers because each includes only `asmtest_trace.h`
  ([amd_backend.c:27](../../../src/amd_backend.c),
  [ss_backend.c:62](../../../src/ss_backend.c)) whereas `branchsnap.c`/`msr_lbr.c` already pull
  `asmtest_hwtrace.h` and do not redefine. Values match exactly today (divergence risk only). For
  `amd_backend.c` the new `src/amd_backend.h` re-exports them (finding above); `ss_backend.c` is not
  an AMD TU, so give it a direct `#include "asmtest_hwtrace.h"` purely for the shared `ASMTEST_HW_*`
  codes (its `asmtest_ss_*` prototypes are declared inline in `hwtrace.c` and stay there —
  `asmtest_hwtrace.h` does not declare them). Delete both local blocks.
- **[hygiene / robustness] Check the `PERF_EVENT_IOC_ENABLE` return at every arm site.** The enable
  ioctl is fire-and-forget at [hwtrace.c:727](../../../src/hwtrace.c) (AMD sampled ring),
  [hwtrace.c:1021](../../../src/hwtrace.c) (sample-window survey),
  [hwtrace.c:1158](../../../src/hwtrace.c) (sample_begin),
  [hwtrace.c:1366](../../../src/hwtrace.c) (Intel PT / CoreSight AUX), and
  [branchsnap.c:211](../../../src/branchsnap.c) /
  [213](../../../src/branchsnap.c) (LBR-on + exit-breakpoint) — a failed `ENABLE` on an
  already-mapped ring yields an empty capture. Gate each:
  `if (ioctl(fd, PERF_EVENT_IOC_ENABLE, 0) != 0) { <existing teardown>; return <EUNAVAIL/-1>; }`
  (branchsnap must fail both, running `bsnap_teardown()`). Mostly robustness: the AMD sampled and
  both survey paths already convert an empty ring to an **honest** `truncated`
  (`best==NULL` / `insns_total==0` / `n==0`), so the one exposed edge is the PT/CoreSight site
  ([hwtrace.c:1366](../../../src/hwtrace.c)), which has no empty→truncated backstop and relies
  on decode-non-OK or the overflow flag. The paired `RESET`/`DISABLE` ioctls stay unchecked (benign).
- **[hygiene] Replace the perf-ring pointer casts with `memcpy` — a strict-aliasing / sanitize-lane
  concern, NOT a misalignment crash.** Each of the three drain functions reads
  `struct perf_event_header *h = (…)(buf+off)`, `uint64_t nr = *(uint64_t *)body`, and
  `struct perf_branch_entry *e = (…)(body+sizeof(uint64_t))`: `hwtrace_end_amd`
  ([hwtrace.c:828, 834](../../../src/hwtrace.c); it has two such loops, 798 and 820),
  `sample_window_amd`, and the begin/end `sample_end_amd` split. All are
  `#if defined(__linux__) && defined(__x86_64__)`-only, `buf` is `malloc`'d (16-byte aligned) and
  perf records are 8-byte-multiple sized, so on x86-64 nothing misaligns — this **corrects the
  review's §1.2 severity**: the only real issue is `-fstrict-aliasing` / UBSan-lane UB, invisible on
  the shipped build. Mirror the local-struct byte-copy already used in `aux_data_ring_truncated`
  ([hwtrace.c:1412-1414](../../../src/hwtrace.c)) — but since these linear `malloc`'d buffers
  never wrap, a plain `memcpy` suffices:
  `struct perf_event_header h; memcpy(&h, buf+off, sizeof h);` and
  `uint64_t nr; memcpy(&nr, body, sizeof nr);`.

**Acceptance.** The existing `make docker-hwtrace-amd` / `docker-hwtrace-codeimage` lanes stay
**byte-identical** green (the shared header, folded macro, reused error codes, and `memcpy` reads are
a no-op refactor whose values and signatures match today, confirmed by unchanged test output); a new
host-independent unit fixture drives the MSR spec filter and asserts a spec-only
(`TO[62]=1, TO[63]=0`) slot is dropped so no phantom edge enters the reconstruction; and a
snapshot-default fixture over a single-exit tail-call routine (one region-leaving direct `jmp`, no
`ret`) takes the deterministic snapshot and reconstructs complete where the sampled path truncates,
while a two-exit (`ret` + tail-`jmp`) and an indirect-tail-`jmp` region both correctly stay on the
sampled fallback. Correctness-only; no behavior change for `ret`-exit or explicit `opts.snapshot`
regions. The MSR fix ships behind the existing `asmtest_amd_msr_trace` self-skip, and the tail-`jmp`
non-eviction assumption is re-confirmed on a Zen 4/5 host per the house "no untested hardware code"
rule (see Risks).

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
  ([examples/test_hwtrace.c](../../../examples/test_hwtrace.c)) run under a plain `docker run`;
  the snapshot marker-path test in [examples/test_branchsnap.c](../../../examples/test_branchsnap.c)
  (BPF lane); all self-skipping. *(The `docker-hwtrace-lbr-snapshot` split-Dockerfile lane was
  not needed: the existing `docker-hwtrace-codeimage` BPF-toolchain image already builds +
  runs branchsnap-test.)*
- **Bindings:** the three `asmtest_ptrace_*` block-step symbols wrapped (whole-word) in each
  of the 10 bindings' `hwtrace.*` wrappers so
  [scripts/check-bindings-parity.sh](../../../scripts/check-bindings-parity.sh) stays green (116
  tier symbols × 10 bindings as of 2026-07-16; 98 when this item was written);
  `asmtest_amd_snapshot_trace` and the whole-window trio carry
  allow-list exemptions (C-level / managed-tier reach). No new `asmtest_trace_choice_t` field
  (the `3 * sizeof(int)` static-assert at
  [asmtest_trace_auto.h:83-84](../../../include/asmtest_trace_auto.h) holds).
- **Docs:** correct the `PTRACE_SINGLEBLOCK` claim in
  [zen2-singlestep-trace-plan.md](zen2-singlestep-trace-plan.md) (Phase 2 makes it real);
  update the "AMD LBR is finished" framing in [native-tracing.md](../../guides/tracing/native-tracing.md) /
  [hardware-tracing.md](../../guides/tracing/hardware-tracing.md); refresh the
  [trace-parity-matrix](../analysis/trace-parity-matrix.md) AMD rows.

## Validation (per lane, self-skipping)

- **Host, privilege-free:** `make hwtrace-test` and the block-step test run under a plain
  `docker run` (Phase 2 needs only ptrace of its own child) and self-skip on
  non-x86/absent-capability hosts — the first AMD-adjacent lane that runs *live* on ordinary
  CI rather than needing bare-metal AMD.
- **Bare-metal AMD:** `make docker-hwtrace-amd` (`--security-opt seccomp=unconfined
  --cap-add=PERFMON`) validates Phases 1, 4, 5 on the Zen 5 dev box; Phase 3 is validated by
  **`make docker-hwtrace-codeimage`** (BPF/PERFMON caps), which runs `branchsnap-test` after
  `codeimage-test`. *(Corrected 2026-07-16: this line previously named a
  `docker-hwtrace-lbr-snapshot` lane. It was never built and no such target exists — the
  codeimage image already carries the clang+libbpf+bpftool toolchain, so a second one was
  unnecessary. See Deliverables and the Phase 3 note.)* The MSR-direct path (Phase 5, Phase 8)
  additionally needs `make docker-hwtrace-msr` (`--privileged`, for `/dev/cpu/N/msr`).
  **None of these three AMD-hardware lanes runs in hosted CI** (`.github/workflows/ci.yml`
  carries `docker-hwtrace-codeimage`, but its branchsnap step self-skips there without an AMD
  host + caps); they need a self-hosted bare-metal AMD runner, so the AMD hardware path is
  validated on the dev box, not in CI.
- **Cross-tier parity:** every reconstructed trace is asserted byte-identical (`insns[]` +
  `blocks[]`) to the existing single-step / DynamoRIO output on shared fixtures, using the
  same branch-edge normalization as `pt_backend.c` — the parity harness Part I already
  established.
- **Binding parity:** `make check-bindings-parity` stays green after the three new ptrace
  symbols land.

## Risks and open points (AMD improvements)

- **Binding-parity cost of block-step.** Three new `asmtest_ptrace_*` symbols must be wrapped
  in all 10 bindings or the gate fails; an intentional gap needs a
  [bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt) entry (stale entries
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
  (`g_fd`/`g_base_map`/`g_active`, [hwtrace.c:610](../../../src/hwtrace.c)) is a single
  process-global slot reset in `hwtrace_end_amd`; any new freeze/snapshot state must be
  single-instance and reset there too.
- **Hardware coverage.** Live validation exists only on Zen 5 (`amd_lbr_v2`). Zen 3 BRS
  (Phase 6) and freeze-absent Zen 4 (Phase 1) paths are code-implemented and
  self-skip-validated but remain **pending real hardware**, per the house "no untested
  hardware code" rule.
- **MSR-rung re-run semantics (Phase 8).** The MSR rung re-executes the routine **in-process**
  (not fork-isolated like the block-step/single-step tiers below it): non-idempotent side
  effects run again and a faulting routine crashes the tracer — acceptable only because it
  matches the fast begin/end tier the rung sits beside, and `asmtest_trace_call_auto` already
  re-runs the routine per tier. The honesty pattern (`call_auto_reset` before the attempt;
  `truncated` on any miss; fall through, never early-return) is the guard — get it wrong and a
  failed MSR attempt reads as an empty-yet-complete trace.
- **Tail-`jmp` boundary non-eviction (Phase 9).** The Zen-5-validated property that a planted
  `#DB` breakpoint does not evict the region's in-region branches before the BPF helper reads
  the frozen stack was established with a `ret`-exit fixture; the tail-call widening *expects*
  it to hold identically for a region-leaving `jmp` (same mechanism, the breakpoint fires when
  execution reaches the CTI, before it transfers) but carries it as an **assumption to
  re-confirm live** on the target substrate before the widened default ships enabled.
