# AMD branchsnap depth fix, LBR tiling validation, freeze-probe cleanup, and the Zen 3 BRS story (doc honesty now, silicon-gated capture arm) — implementation

> **Sources.** Actioned from
> [amd-review-followup-plan.md](../archive/plans/amd-review-followup-plan.md) (P2, P4,
> P5), [2026-07-17-amd-hardware-review.md](../analysis/2026-07-17-amd-hardware-review.md)
> (§2.1, §2.2, §2.4),
> [2026-07-17-dataflow-tier-open-followups.md](../analysis/2026-07-17-dataflow-tier-open-followups.md)
> (item 2), [amd-tracing-plan.md](../plans/amd-tracing-plan.md) (Improvement
> Phase 6, the tiling residual), and
> [trace-parity-matrix.md](../analysis/trace-parity-matrix.md). Written
> 2026-07-17. If this doc and a source disagree, this doc wins (sources may
> be stale); if the CODE and this doc disagree, re-verify before implementing.

## Why this work exists

Three user-visible lies and two validation gaps live in the AMD LBR tier
today: a provably complete 15-branch snapshot window is flagged `truncated`
(forcing a real second in-process execution of the routine under test); a test
binary prints a freeze-gate diagnostic whose both branches are false; and the
guides, the public header, and the parity matrix all tell a Zen 3 owner that
AMD branch capture works on their part, when the kernel provably rejects every
open this tree performs. This doc fixes the depth check, retires the dead
freeze probe, makes every Zen 3 claim true (the live-capture floor is **Zen 4**
until the raw-`0xc4` BRS arm ships), runs the two outstanding live
verifications on the Zen dev box, and fully specifies the silicon-gated Zen 3
BRS capture arm for whenever a Family 19h host exists.

## What already exists (verified 2026-07-17)

The landed substrate, with the lines this doc's tasks touch:

- [src/branchsnap.c](../../../src/branchsnap.c) — deterministic BPF boundary
  snapshot (LBR-on + HW breakpoint per exit + `bpf_get_branch_snapshot`).
  Lines 344-360 build `arr[] = [synthetic boundary edge] + [use hardware
  entries]` and call `asmtest_amd_decode(arr, n_dec, …)` with
  `n_dec = use + 1` (the T1 bug). Lines 419-426 record the tiling consumer's
  CALL-target boundary assumption as "still not literally either validated
  case" (T7). `asmtest_amd_tile_begin` is at line 507.
- [src/amd_backend.c](../../../src/amd_backend.c) — the shared decoder.
  `asmtest_amd_decode_reach` (line 506) applies the depth ceiling at lines
  525-526: `if ((int)nbr >= asmtest_amd_lbr_depth()) trace->truncated = true;`
  — it counts whatever it is handed, hardware slot or not.
  `asmtest_amd_freeze_available` is defined at line 100 (stub at 725) with
  **zero live consumers** (T4). The CPUID `0x80000022` reader
  `asmtest_amd_lbr_depth` is at line 166 — the in-tree Zen 3/Zen 4
  discriminator T8 needs. `amd_replay` appends block 0 unconditionally at
  line 267 (deliberate; do not "fix").
- [src/amd_backend.h](../../../src/amd_backend.h) — the internal (non-ABI)
  contract header for the four AMD TUs. Freeze-probe decl at lines 32-34;
  `asmtest_amd_decode_reach` decl at 120-129 ("not spuriously truncated").
- [src/hwtrace.c](../../../src/hwtrace.c) — sampled capture. `amd_branch_probe`
  (line 247) opens `PERF_TYPE_HARDWARE` /
  `PERF_COUNT_HW_BRANCH_INSTRUCTIONS`, `sample_period = 1` (lines 253-255) and
  maps `EINVAL`/`EOPNOTSUPP` to `AMD_NOHW` (line 278); the capture open in
  `hwtrace_begin_amd` (755, attr at 805-820) and the two survey opens
  (1347-1349, 1608-1610) use the same generic event. `PERF_TYPE_RAW` and
  `0xc4` appear **nowhere** in `src/` or `include/` (grep verified) — the
  Zen 3 BRS arm was never built. The Tier-A exit-anchor completeness check
  lives in `asmtest_amd_ring_parse_decode` (890; comment at 1041-1084) and
  runs on **every** part, freeze bit or not.
- [examples/test_branchsnap.c](../../../examples/test_branchsnap.c) — the
  snapshot suite: `snap_default_run` with the a89a1cb path-specific
  `covered(want_off) && !covered(other_off)` assertions (lines 85-131,
  call sites 363-366), and the Phase 9 tail-`jmp` non-eviction fixtures with
  negative/positive controls and a recorded mutation procedure (133-613).
- [examples/test_branchtile.c](../../../examples/test_branchtile.c) — the E6
  tiling suite (cold-leaf checkpoint, negative control, depth-equality,
  differential oracle). It asserts the island's newest edge and completeness,
  but **no pre-call-history survival** — the T7 gap.
- [examples/test_hwtrace.c](../../../examples/test_hwtrace.c) — pure synthetic
  AMD decode tests (e.g. a 16-entry full-window decode at line 408,
  `test_amd_spec_filter` at 427 — the pattern for T1's pure test) and
  `test_amd_freeze_probe` (229-240) with the false PRESENT/ABSENT printf (T4).
- [mk/native-trace.mk](../../../mk/native-trace.mk) — `branchsnap-test`
  (line 2067), `branchtile-test` (2079), `hwtrace-test` (2128).
- [Dockerfile.hwtrace-codeimage](../../../Dockerfile.hwtrace-codeimage) —
  carries the BPF toolchain (clang, llvm, libbpf-dev, bpftool via pinned
  Ubuntu distro packages, lines 22-27) and runs
  `make codeimage-test && make branchsnap-test && make branchtile-test`
  (line 29). The lane `docker-hwtrace-codeimage`
  ([mk/docker.mk](../../../mk/docker.mk) line 598) runs it with
  `--cap-add=BPF --cap-add=PERFMON --cap-add=SYS_PTRACE`,
  `seccomp=unconfined`, memlock unlimited.

**Prove the baseline green before touching anything.** `make help` lists every
target (source of truth). Then:

1. `make check` — framework self-tests; exits 0.
2. On any Linux x86-64 host: `make branchsnap-test` builds and either runs
   live or prints `# SKIP branchsnap: substrate absent (needs AMD LbrExtV2 +
   perfmon_v2 + kernel >= 6.10)` and exits 0 (off AMD this skip is correct).
3. On the AMD Zen 5 dev box (Ryzen 9 9950X): `make docker-hwtrace-codeimage`
   — all three suites run **live** and end green (`ok - branchtile: E6 …`).
   This is the lane every live task below uses.

## Tasks

### T1 — Gate the branchsnap depth check on hardware slots, not `use + 1`  (S, depends on: none)

**Goal.** A trimmed snapshot window with `use == 15` hardware entries plus the
synthetic boundary edge decodes with `truncated == false`; `use == 16` still
truncates.

**Steps.**

1. In [src/amd_backend.c](../../../src/amd_backend.c), rename the body of
   `asmtest_amd_decode_reach` (line 506) to a new internal entry that takes
   the hardware count separately:
   ```c
   int asmtest_amd_decode_reach_hw(const struct perf_branch_entry *br,
                                   size_t nbr, size_t hw_nbr,
                                   const void *base, size_t len,
                                   asmtest_trace_t *trace, int *reached_exit);
   ```
   The only behavioral change: the ceiling at lines 525-526 compares
   `(int)hw_nbr >= asmtest_amd_lbr_depth()`. Reimplement
   `asmtest_amd_decode_reach` as a thin wrapper passing `hw_nbr = nbr`
   (so [src/hwtrace.c](../../../src/hwtrace.c):1039 and every other caller is
   byte-identical in behavior). Mirror the declaration in
   [src/amd_backend.h](../../../src/amd_backend.h) next to
   `asmtest_amd_decode_reach` (120-129) inside the same
   `__linux__ && __x86_64__` branch, and mirror the non-Linux stub exactly the
   way the header's own comment prescribes for the existing decode prototypes.
2. In [src/branchsnap.c](../../../src/branchsnap.c) line 359, replace
   `asmtest_amd_decode(arr, n_dec, …)` with
   `asmtest_amd_decode_reach_hw(arr, n_dec, use, g_bsnap.drain.base,
   g_bsnap.drain.len, trace, NULL)`. The synthetic boundary edge (a
   deterministic completion, not a hardware slot — comment block (b) at
   318-329) no longer inflates the ceiling.
3. Extend the comment block (a) at lines 302-316 with one sentence recording
   that the ceiling counts **hardware slots only** and why (`n_dec` may hold
   one extra synthetic edge).
4. Add the pure test (below), then `make fmt && make hwtrace-test`.

**Code.** As above. Do not change `asmtest_amd_decode`'s public shape; do not
touch the trim logic. The review's alternative ("drop the synthetic append and
let `decode_reach`'s tail-fill complete to the exit") is legitimate but changes
the decode path for every existing fixture — the `hw_nbr` parameter is the
minimal fix and is what this doc prescribes.

**Tests.** In [examples/test_hwtrace.c](../../../examples/test_hwtrace.c), add
`test_amd_decode_hw_ceiling()` next to `test_amd_spec_filter` (line 427),
mirroring its synthetic-`perf_branch_entry` pattern (add the forward decl for
the new symbol beside the existing ones near line 51). Region bytes: 14 ×
`{0xEB, 0x00}` (`jmp +0` at offsets 0x00…0x1A) + `0xC3` (ret at 0x1C).
Entries, newest-first: one synthetic boundary edge `{from = base+0x1C,
to = 0}`, then the 14 jmp edges, then the entry-call edge `{from = base-5,
to = base}` — `nbr = 16`. Assert:

- `asmtest_amd_decode_reach_hw(arr, 16, /*hw_nbr=*/15, …)` →
  `truncated == false`, `insns_total == 15` (14 jmps + ret).
- the same call with `hw_nbr = 16` → `truncated == true` (boundary pinned).
- `asmtest_amd_decode(arr, 16, …)` (the wrapper) → `truncated == true`
  (wrapper semantics unchanged).

Failure looks like `not ok - …` from the `CHECK` macro and a nonzero exit;
pass adds three `ok` lines to `make hwtrace-test` on any x86-64 Linux host
(pure decode — needs Capstone, which every docker lane carries; the test
self-skips off x86-64 Linux like its siblings).

**Docs.** Append to `CHANGELOG.md` under `## [Unreleased]` / `### Fixed`: the
snapshot path no longer flags a complete 15-branch window truncated (which
escalated to a real re-execution via
[src/trace_auto.c](../../../src/trace_auto.c):225).

**Done when.**

- `make hwtrace-test` passes with the three new assertions on a non-AMD Linux
  host (pure part) — no live hardware needed.
- `grep -n "n_dec, use" src/branchsnap.c` shows the new call;
  `asmtest_amd_decode(` no longer appears in branchsnap.c.
- `make check` and `make fmt-check` stay green.

### T2 — Live `use == 15` / `use == 16` boundary fixtures on Zen 5  (S, depends on: T1)

**Goal.** The T1 boundary is pinned by live captures: a 14-taken-branch chain
reconstructs `!truncated` and a 15-taken-branch chain still truncates, on real
LbrExtV2.

**Steps.**

1. In [examples/test_branchsnap.c](../../../examples/test_branchsnap.c), after
   the multi-exit block (ends line 381), add a block gated on
   `rc == ASMTEST_HW_OK` exactly like its neighbors (the whole-tier self-skip
   contract). Two fixtures driven through `asmtest_amd_snapshot_trace` (the
   direct entry the file's first test uses, lines 249-250):
   - `JMP14`: 14 × `{0xEB, 0x00}` + `0xC3`; exit_off `0x1C`. The 16-deep
     frozen window holds the 14 in-region jmp edges + the entry-call edge
     (`to = base`, region-involved) + one pre-entry glue edge → trimmed
     `use == 15`, `n_dec == 16` with the boundary edge — exactly the spurious
     trip point. Assert `truncated == false` and `insns_total == 15`.
   - `JMP15`: 15 × `{0xEB, 0x00}` + `0xC3`; exit_off `0x1E`. All 16 window
     slots are region-involved → `use == 16`. Assert `truncated == true`
     (honest ceiling: an older in-region edge may have been evicted).
2. Frame the fixture comment per the review: this is a **near-saturated
   window**, not a "tiny routine" (the review refuted that framing — shipped
   fixtures run `use` ≈ 1-4 and the trim already rescues them).
3. Confirm the trim count empirically once:
   `ASMTEST_HWTRACE_DEBUG=1 make branchsnap-test` on the Zen box prints
   `snapshot_end: decode best_nr=16 … trimmed=15 boundary=1` for JMP14
   ([src/branchsnap.c](../../../src/branchsnap.c):355-358).
4. Run `make docker-hwtrace-codeimage` on the Zen 5 dev box.

**Code.** Test-only; no production change.

**Tests.** The fixtures are the test. Failure: `not ok - branchsnap use15:
truncated=1 …` (before T1 the JMP14 fixture fails exactly this way — run it
once against the unfixed tree to watch it fail, which proves it
discriminates). Pass: two new `ok` lines inside the codeimage lane output.

**Docs.** Internal-only, no user-facing docs — a test-coverage addition.

**Done when.**

- `make docker-hwtrace-codeimage` on the Zen 5 dev box shows both fixtures
  `ok` and exits 0.
- Off AMD (this authoring host), the same target prints the substrate `# SKIP`
  and exits 0 — the lane self-skips cleanly.
- The pre-fix failure of JMP14 was observed and is noted in the test comment.

### T3 — Live-verify the a89a1cb multi-exit assertion rewrite  (S, depends on: none)

**Goal.** The path-specific assertions `covered(want_off) &&
!covered(other_off)` (a89a1cb, verified so far only by compilation and the
ENOSYS stub path) pass against real LBR captures on both exits.

**Steps.**

1. On the Zen 5 dev box run `make docker-hwtrace-codeimage`. The image already
   carries the BPF toolchain
   ([Dockerfile.hwtrace-codeimage](../../../Dockerfile.hwtrace-codeimage):22-27)
   — the "missing clang/libbpf-dev" that blocked the original session applied
   to a bare-host run; the Docker lane is the prescribed route (CLAUDE.md:
   dependencies live in the image, not the host), and no image change is
   needed.
2. Observe the two lines from `snap_default_run`
   ([examples/test_branchsnap.c](../../../examples/test_branchsnap.c):118-123):
   `branchsnap multi-exit path-A(ret@0x08): … want(0x05)=1, other(0x09)=0,
   truncated=0` and the mirrored path-B line, then
   `ok - branchsnap multi-exit: default-on snapshot covers BOTH exits …`.
3. Record the outcome: in
   [2026-07-17-dataflow-tier-open-followups.md](../analysis/2026-07-17-dataflow-tier-open-followups.md)
   item 2, replace "verified by compilation and the ENOSYS stub path only"
   with a dated live-verified note (host, lane, both exit lines).
4. If either assertion **fails** live, that is the news the repo's
   "verify before declaring done" memory rule exists for: the offsets
   0x05/0x09 are then wrong against the real capture — fix the offsets in
   `snap_default_run`'s call sites (lines 363-366) against the observed
   coverage, never weaken the assertion back to `covered(0)`.

**Code.** None expected.

**Tests.** This task IS the test run. Failure: `not ok - branchsnap
multi-exit: a default-on path missed its boundary` and lane exit 1. Pass: the
`ok` line above.

**Docs.** The dated note in the follow-ups analysis doc (step 3). No
changelog entry (no behavior change).

**Done when.**

- The codeimage lane is green on the Zen 5 box with both path lines showing
  `want=1, other=0, truncated=0`.
- The analysis doc carries the dated live-verified note.
- On non-AMD hosts nothing changes (lane still self-skips).

### T4 — Retire `asmtest_amd_freeze_available`; make every freeze statement true  (S, depends on: none)

**Goal.** The dead freeze probe is deleted, the false PRESENT/ABSENT printf is
gone, and the three plan sections asserting a live freeze gate state what
shipped.

**Steps.**

1. Delete the definition + comment block at
   [src/amd_backend.c](../../../src/amd_backend.c):92-117 and the stub at
   line 725; delete the declaration + comment at
   [src/amd_backend.h](../../../src/amd_backend.h):32-34. (Retire, not wire:
   an `nm` scan found zero undefined references; the gate was deliberately
   removed in 5d8e0d2 after Zen 5 disproved the theory and replaced by the
   unconditional exit-anchor check — a resurrected consumer would re-add the
   vacuous-pass hazard that check fixed.)
2. In [examples/test_hwtrace.c](../../../examples/test_hwtrace.c): remove the
   hand-copied forward decl at line 57 and the freeze section of
   `test_amd_freeze_probe` (231-240) — both printf branches
   (`"PRESENT (single-window Tier-A trusted)"` / `"ABSENT (Tier-A window
   trusted only if it captured the region exit)"`) are false: Tier-A is
   exit-anchored on **every** part
   ([src/hwtrace.c](../../../src/hwtrace.c):1041-1084). Keep the
   snapshot-substrate checks (241+); rename the function
   `test_amd_snapshot_substrate_probe` and update the call in `main`.
3. Reword the historical mention at
   [src/hwtrace.c](../../../src/hwtrace.c):1051-1053 ("was once gated behind
   `!asmtest_amd_freeze_available()`") to "was once gated behind the
   freeze-availability CPUID probe (removed)" so the symbol greps clean.
4. Correct [amd-tracing-plan.md](../plans/amd-tracing-plan.md):
   - Part II #3 (560-576): annotate that the freeze **gate** was removed in
     5d8e0d2 and the probe retired; the surviving behavior is the
     unconditional exit-presence check, and it lives in
     `asmtest_amd_ring_parse_decode`, not `hwtrace_end_amd`.
   - Improvement Phase 1 status note (993-999): same correction (the
     "freeze-absent branch pending hardware" caveat is moot — the check no
     longer branches on freeze).
   - Deliverables bullet (1727-1731): strike "freeze-gated" and the
     `hwtrace_end_amd` attribution.
5. `make fmt && make hwtrace-test && make check`.

**Code.** Deletions plus comment/doc edits only.

**Tests.** `make hwtrace-test` still passes (the renamed probe test asserts
the substrate probe's 0/1 + cached stability, unchanged). Add nothing new: the
observable is `grep -rn asmtest_amd_freeze_available src include examples
mk` → **zero hits**. There is no other testable surface — dead-code removal.

**Docs.** `CHANGELOG.md` `### Fixed`: the AMD freeze-probe diagnostic that
printed a false trust statement is removed; Tier-A completeness is
exit-anchored on every part.

**Done when.**

- The grep above returns nothing; the build links (`make hwtrace-test`
  compiles all four AMD TUs).
- `test_hwtrace` output no longer contains "freeze-on-PMI"
  PRESENT/ABSENT lines.
- The three plan sections read correctly against
  [src/hwtrace.c](../../../src/hwtrace.c):890/1041-1084.

### T5 — Zen 3 BRS: make the parity matrix and the AMD plan true  (S, depends on: none)

**Goal.** No internal analysis/plan doc claims this tree can open Zen 3 BRS;
Phase 6 is rescoped as the only way BRS opens at all; the plan's one false
kernel claim (`amd_brs_adjust_period`) is corrected.

**Steps.**

1. [trace-parity-matrix.md](../analysis/trace-parity-matrix.md):
   - Line 133 (Matrix 3, AMD LBR row): replace `✓ BRS` in the Zen 3 column
     with: facility exists in silicon (BRS), **this tree cannot open it** —
     probe and capture open the generic `PERF_COUNT_HW_BRANCH_INSTRUCTIONS`
     (renders as raw `0x00c2`) with `sample_period = 1`, but the kernel's
     `amd_brs_hw_config` demands exactly raw `0xc4` with a fixed period ≥ 17,
     so every open fails `-EINVAL` and is reported as `AMD_NOHW`
     ([src/hwtrace.c](../../../src/hwtrace.c):275-278). Point at T8 for the
     arm that would open it.
   - Line 113 (Matrix 2): `✓ (Zen 3+)` → `✓ (Zen 4+)`.
   - The prose at 139-144 ("Zen 3 uses **BRS** …"): qualify with the same
     cannot-open statement.
2. [amd-tracing-plan.md](../plans/amd-tracing-plan.md):
   - Phase 0 (158-186): the bullet at 174-176 ("… falling back to the BRS
     event") describes an arm that was **never built** — annotate it
     `(NOT landed — see Improvement Phase 6)` so the phase's `(LANDED)` header
     no longer covers it; in the acceptance at 183-185, `Zen 3+/Zen 4` →
     `Zen 4+`.
   - Improvement Phase 6 (1314-1345): reframe the Goal — it is **not** a
     throughput win over a working `sample_period=1` baseline (the kernel
     disallows that baseline on Zen 3 entirely); it is the only way BRS opens
     at all. Keep the existing (correct) caveat that `sample_period=1` is
     load-bearing for Tier-B, so BRS must be a distinct Tier-A mode.
   - Same section, line 1328-1329: the sentence "the kernel already programs
     `period − lbr_nr` via `amd_brs_adjust_period`" cites a symbol that does
     not exist at v6.10 or v6.14 — correct it to: the hard gate is
     `amd_brs_hw_config` (`sample_period <= x86_pmu.lbr_nr` → `-EINVAL`) and
     the period adjustment is `amd_pmu_limit_period` in
     `arch/x86/events/amd/core.c` (see Research notes).
   - Add one line recording the in-tree discriminator: CPUID `0x80000022` is
     already read at [src/amd_backend.c](../../../src/amd_backend.c):166.
3. `make docs` (or `make docker-docs`) is unaffected — these are
   `docs/internal/**` files, excluded from the published build — but run it
   anyway to prove no regression.

**Code.** None — documentation only. The runtime skip-string split
(`AMD_NOHW` vs "BRS present, capture mode unsupported") is **T8's** and stays
silicon-gated.

**Tests.** No testable surface (internal docs). Manual verification:
`grep -n 'BRS' docs/internal/analysis/trace-parity-matrix.md` shows only the
qualified cell; `grep -n 'brs_adjust' docs/internal/plans/amd-tracing-plan.md`
returns nothing.

**Docs.** This task is docs. No changelog entry (internal docs are not
user-visible).

**Done when.**

- The two greps above hold.
- Phase 0's landed header no longer covers the BRS bullet; Phase 6's Goal no
  longer presumes a working Zen 3 baseline.
- No doc anywhere in `docs/internal/` claims `amd_brs_adjust_period` exists.

### T6 — Sweep every remaining "Zen 3+" claim in guides and headers to the Zen 4 floor  (S, depends on: T5)

**Goal.** Every user-facing statement of the AMD LBR live-capture floor reads
**Zen 4+**, with one canonical sentence (matching T5's parity-matrix wording)
explaining the Zen 3 situation where the guide discusses it.

**Steps.** Correct these verified sites (grep
`-rn 'Zen 3' docs/guides docs/*.md include/ | grep -v amd_tracing_review.md`
afterward to confirm none is missed — the list below was verified 2026-07-17;
the one excluded file is the dated historical audit page called out in step 9):

1. [docs/guides/tracing/hardware-tracing.md](../../guides/tracing/hardware-tracing.md)
   lines 38 (backend table: "bare-metal AMD (Zen 3+)"), 290, 473, 533 — floor
   becomes Zen 4+; line 38's cell gains the short form "Zen 3 BRS exists in
   silicon but this tree cannot open it yet".
2. [docs/guides/tracing/amd-lbr-tuning.md](../../guides/tracing/amd-lbr-tuning.md)
   line 137 ("Zen 3+ silicon" requirement row) → Zen 4+.
3. [docs/guides/tracing/native-tracing.md](../../guides/tracing/native-tracing.md)
   line 993 ("Zen 3+/4/5 host") → Zen 4/5.
4. [docs/scoped-tracing-implementation.md](../../scoped-tracing-implementation.md)
   lines 106, 148, 241, 255 (four "Zen 3+" gate statements) → Zen 4+.
5. [hardware-trace-plan.md](../plans/hardware-trace-plan.md) lines 59-60
   ("live capture needs Zen 3+") → Zen 4+.
6. [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h) — three
   sites, not just the one the plan enumerates: line 69 (the
   `ASMTEST_HWTRACE_AMD_LBR` enum comment), line 431 ("forward-look
   (bare-metal Intel PT / Zen 3+)"), line 581 ("no Zen 3+/BRS or LbrExtV2").
   Reword each to Zen 4+ LbrExtV2, with line 69 keeping a parenthetical
   "(Zen 3 BRS: silicon facility, not yet openable — see amd-tracing-plan
   Improvement Phase 6)".
7. Source-comment stragglers that make the same false capture claim:
   [src/amd_backend.c](../../../src/amd_backend.c) lines 6 and 19 ("The
   capture needs an AMD Zen 3 (BRS) / Zen 4 / Zen 5 (LbrExtV2) host") →
   Zen 4/5, noting the Zen 3 arm is unbuilt.
   ([src/branchsnap.c](../../../src/branchsnap.c):21 already says "Zen 3 BRS
   remain unmeasured" — honest; leave it.)
8. Do **not** touch the runtime strings in
   [src/hwtrace.c](../../../src/hwtrace.c) (244, 275-278, 386, 1192, 1359,
   2586): the skip-reason split is runtime behavior and belongs to T8's
   silicon-gated arm — changing the strings without the probe retry would
   trade one wrong message for another.
9. **Historical audit page — deliberately not rewritten.**
   [docs/amd_tracing_review.md](../../amd_tracing_review.md) is a dated,
   orphaned audit snapshot (`orphan: true`, "_Revision 2026-07-09_") that
   Sphinx builds but the toctree does not link. Its two `Zen 3` mentions —
   line 18 ("no Zen 3 BRS", already true of the Zen 2 host) and line 68
   ("branch-stack only via BRS (Zen 3) / LbrExtV2 (Zen 4+)", the imprecise
   implication this sweep otherwise kills) — record what the 2026-07-09 review
   said, not live guidance. Rewriting a dated review would falsify the
   historical record, and this file is outside the "guides and headers" sweep
   (global position 2 scopes the correction to guides, headers, and the parity
   matrix). Leave it untouched; the confirmation and done-when greps exclude it
   via `| grep -v amd_tracing_review.md` so a junior dev is not left guessing
   whether these two hits are in scope — they are not.
10. `make docs` (Sphinx, `-W` fail-on-warning) — the guides are published
   pages; the build must stay warning-clean. Off-host, `make docker-docs`.

**Code.** Header/guide comment text only; zero behavior change.

**Tests.** `make docs` green is the gate for the published pages. Manual
check: `grep -rn 'Zen 3' docs/guides docs/*.md include/ | grep -v
amd_tracing_review.md` returns only lines that state the corrected floor or the
explicit cannot-open explanation (the excluded file is the dated 2026-07-09
audit page, deliberately left as historical record — step 9).

**Docs.** This task is docs. `CHANGELOG.md` `### Fixed`: guides and the
public header no longer claim AMD LBR live capture works on Zen 3; the floor
is Zen 4 until the BRS arm ships.

**Done when.**

- The grep above (with the historical audit page excluded) shows no
  unqualified "Zen 3+" capture claim.
- `make docs` (or `make docker-docs`) exits 0 with no new warnings.
- Wording matches T5's parity-matrix sentence (one canonical explanation,
  not eight paraphrases).

### T7 — Validate the third #DB non-eviction shape: the CALL-target checkpoint  (S, depends on: none)

**Goal.** Measured evidence, on live Zen 5, that a #DB execution breakpoint at
a CALL target does not evict recent branch history before
`bpf_get_branch_snapshot` reads it — the third boundary shape after the
validated `ret` and tail-`jmp` exits.

**Steps.**

1. In [examples/test_branchtile.c](../../../examples/test_branchtile.c), add a
   `preamble_leaf()` — `__attribute__((noinline))`, same shape as `cold_leaf`
   (77-80) — and call it in `body()` (103-110) **immediately before**
   `cold_leaf`. The call/ret pair plants two known retired edges 2-3 slots
   deep in the window frozen at `cold_leaf`'s entry breakpoint.
2. Add check 3b after the existing check 3 (273-296): the island prefix
   `ips[0..ntiled)` must contain `(uint64_t)&preamble_leaf` — disjoined with
   `tile_truncated` (never the survey-wide `truncated`; the file's own
   comment at 274-280 explains why that term is the only honest escape).
   Unlike island[0] (present **by hardware construction** — the file's
   comment at 16-18), the preamble entry is only present if the 2-3
   next-newest slots survived the #DB: this is the discriminating
   non-eviction evidence the existing depth-equality cannot provide (16 stale
   endpoints would also satisfy `ntiled == islands * depth`).
3. Mutation evidence, Phase-9 style (a documented one-off local run, never
   committed — mirror the procedure recorded at
   [amd-tracing-plan.md](../plans/amd-tracing-plan.md):1871-1876): in
   `btile_on_event` ([src/branchsnap.c](../../../src/branchsnap.c):476-497),
   temporarily `continue` on entries whose `e.to` equals the preamble entry
   PC. Run the lane: check 3b must flip to `not ok` while the negative
   control (check 2) stays green — proving 3b can fail. Revert, record the
   observed flip in the new check's comment.
4. Run `make docker-hwtrace-codeimage` on the Zen 5 dev box (the lane runs
   `branchtile-test`; `docker-hwtrace-dotnet-amd` additionally exercises the
   managed-entry consumer but is not required for this shape measurement).
5. On green: update the boundary-assumption comment at
   [src/branchsnap.c](../../../src/branchsnap.c):419-426 ("still not
   literally either validated case" → validated live on Zen 5 for all three
   shapes: `ret` exit, tail-`jmp` exit, CALL-target checkpoint) and the
   plan's closing residual at
   [amd-tracing-plan.md](../plans/amd-tracing-plan.md):1878-1884.

**Code.** Test additions plus the two comment updates; no production change.

**Tests.** Check 3b is the test. Failure prints `not ok - branchtile:
island missing the pre-call preamble entry … (tile_truncated=0)` — which,
live, would mean the CALL-target #DB **does** evict history and the tiling
consumer's inherited assumption is false (escalate: that is a finding, not a
test bug). Pass: one new `ok` line in the codeimage lane.

**Docs.** Internal-only (the two comment/plan updates in step 5); no
user-facing docs — tiling semantics are unchanged, only newly evidenced.

**Done when.**

- `make docker-hwtrace-codeimage` green on the Zen 5 box with check 3b `ok`
  across all `TRIALS` trials.
- The mutation run's flip was observed and is recorded in the comment.
- branchsnap.c:419-426 and the plan's residual paragraph state the
  three-shapes-validated fact (Zen 4 and Zen 3 BRS coverage remain honestly
  unmeasured there).
- Off AMD, `branchtile-test` still self-skips (exit 0).

### T8 — The Zen 3 BRS capture arm: raw `0xc4` probe retry + distinct Tier-A mode  (M, depends on: T5; HARDWARE-GATED on Zen 3 / Family 19h silicon)

**Goal.** On a Zen 3 host, `asmtest_hwtrace_available(AMD_LBR)` returns 1 via
a raw-`0xc4` BRS open, a ≤16-branch routine captures in a single fixed-period
window, and every other host's behavior is byte-identical.

**Steps.** (Write against the kernel facts in Research notes; do not merge
enabled until executed on Family 19h silicon — house rule: no untested
hardware code. Everything below is safe to prepare behind the probe.)

1. **Probe retry.** In `amd_branch_probe`
   ([src/hwtrace.c](../../../src/hwtrace.c):247-279): when the generic open
   fails with `EINVAL`/`EOPNOTSUPP`, retry with
   `type = PERF_TYPE_RAW`, `config = 0xc4` (`AMD_FAM19H_BRS_EVENT` =
   `RETIRED_TAKEN_BRANCH_INSTRUCTIONS`), `freq = 0`, `sample_period = 33`
   (any fixed value > 16 satisfies the kernel gate; 33 leaves a 16-branch
   budget after the kernel's `limit_period` subtraction),
   `sample_type = PERF_SAMPLE_BRANCH_STACK`,
   `branch_sample_type = PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_ANY`.
   BRS permits **no** filtering beyond privilege-level bits —
   `ASMTEST_AMD_REDUCED_FILTER`
   ([src/amd_backend.h](../../../src/amd_backend.h):110-113) must never be
   applied on the BRS path. Success → new outcome `AMD_OK_BRS` in the enum at
   line 241.
2. **Skip-reason split.** In `hw_classify` (the `v.reason` block around
   378-390): a raw-`0xc4` retry that itself fails keeps today's
   `"no AMD branch records (needs Zen 4+ LbrExtV2)"` (wording per T6); a
   retry that fails only because of an unsupported attr combination on a
   BRS-capable kernel reports the new
   `"AMD BRS present, capture mode unsupported"` string. Thread the same
   distinction through `asmtest_hwtrace_status` / `asmtest_hwtrace_skip_reason`
   ([include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h)).
3. **Distinct Tier-A BRS mode.** In `hwtrace_begin_amd`
   ([src/hwtrace.c](../../../src/hwtrace.c):755, attr at 805-820): when the
   probe said `AMD_OK_BRS`, open the raw event with the fixed period —
   **never** `sample_period = 1` (the kernel rejects it), and therefore
   **never** Tier-B stitching (which is load-bearing on the period-1 overlap
   and stays untouched, per the plan's own caveat). One window per region;
   decode through the existing `asmtest_amd_decode_reach`; `truncated`
   honesty and the depth ceiling (16 — the CPUID `0x80000022` leaf is absent
   on Zen 3 and `asmtest_amd_lbr_depth()` already falls back to 16,
   [src/amd_backend.c](../../../src/amd_backend.c):160-175) apply unchanged.
   Keep the two survey opens (1347-1349, 1608-1610) generic — they self-skip
   on Zen 3 exactly as today until someone extends them.
4. **Discrimination.** Zen 3 selects BRS mode purely by probe outcome
   (`amd_lbr_v2` cpuinfo flag absent + raw `0xc4` open succeeds); no family
   sniffing. Record in a comment that CPUID `0x80000022`
   (amd_backend.c:166) is the architectural discriminator the review named.
5. **Tests.** Extend `test_hwtrace.c`: off-Zen 3 hosts must observe zero
   behavior change (probe still `AMD_OK` on Zen 4/5 — the retry never fires
   because the generic open succeeds; still `AMD_NOHW` on Intel/Zen 2 — the
   raw open fails the same way). On a Zen 3 host: a ≤16-branch fixture
   reconstructs with `truncated == 0` through the marker path; a >16-branch
   fixture reports `truncated == 1` (no Tier-B on BRS). Validation lane:
   `make docker-hwtrace-amd` on the Zen 3 host.
6. Docs on landing: flip the T5/T6 statements ("cannot open" → "opens via the
   raw-0xc4 BRS Tier-A mode, Linux ≥ 5.19 with `CONFIG_PERF_EVENTS_AMD_BRS`"),
   restore the parity-matrix Zen 3 cell to a qualified ✓, changelog `### Added`.

**Code.** As above; all new lines live behind the `AMD_OK_BRS` probe outcome.

**Tests.** As step 5. Until silicon exists, the merged-disabled state is:
code may be reviewed but the arm must not be enabled/merged — a lane that can
only self-skip is not a test, and this is one of the two legitimate self-skip
categories (hardware).

**Docs.** Step 6, only on landing.

**Done when.**

- Executed on an AMD Family 19h host: `make hwtrace-test` (or
  `make docker-hwtrace-amd`) shows the AMD LBR tier available, the
  ≤16-branch fixture `truncated == 0`, and the skip-string split observable.
- On the Zen 5 and Zen 2 dev boxes, before/after outputs of
  `make docker-hwtrace-amd` and `make hwtrace-test` are identical.
- Until then: this task is **blocked**; record the gate (below) and do not
  ship the arm.

## Task order & parallelism

- **Independent starts:** T1, T3, T4, T5, T7 can all begin concurrently
  (different files or doc-only).
- **Ordered:** T1 → T2 (the live fixtures assert the fixed behavior);
  T5 → T6 (the sweep reuses T5's canonical wording); T5 → T8 (the plan must
  stop claiming a working baseline before the arm reframes it).
- **One dev-box session:** T2, T3, T7 all run inside
  `make docker-hwtrace-codeimage` on the Zen 5 box — batch them into one
  sitting.
- **Critical path:** T1 → T2 (the only correctness bug). T8 is off the
  critical path entirely — it waits on silicon that does not exist here.

## Constraints & gates

- **Real hardware gates (legitimate self-skips per CLAUDE.md):**
  - T2/T3/T7 live legs: bare-metal AMD Zen 4/5 with CAP_BPF + CAP_PERFMON —
    the dev Ryzen 9 9950X (Zen 5) provides it; hosted CI self-skips (the
    runner gap is [self-hosted-ci-runners.md](self-hosted-ci-runners.md)'s
    scope).
  - T8: AMD Zen 3 / Family 19h BRS silicon. Neither dev box qualifies
    (Ryzen 9 9950X = Zen 5 Family 1Ah; Ryzen 9 4900HS = Zen 2 Family 17h).
    When blocked, record in the task's plan section: the exact attr the arm
    would open, the kernel constraints it satisfies (Research notes), and
    that the arm is unmerged pending a Fam19h run.
- **Not gates:** the BPF toolchain (clang/llvm/libbpf-dev/bpftool) is already
  in [Dockerfile.hwtrace-codeimage](../../../Dockerfile.hwtrace-codeimage) as
  pinned distro packages — never install it on the host, never self-skip for
  its absence; run the docker lane.
- **House rules in force:** no untested hardware code (T8 stays behind its
  gate); every user-visible change appends to `CHANGELOG.md` `[Unreleased]`;
  `make fmt` before committing (CI gates on `fmt-check`); published guides
  must keep `make docs -W` clean (T6).

## Research notes (verified 2026-07-17)

Kernel facts for T5's corrections and T8's design, verified against pinned
tags v6.10 and v6.14 (byte-identical constraint logic at both; v6.10 matches
the repo's own kernel floor documented in
[Dockerfile.hwtrace-dotnet-amd](../../../Dockerfile.hwtrace-dotnet-amd)
line 24):

- **The BRS gate** — `amd_brs_hw_config()` in `arch/x86/events/amd/brs.c`
  returns `-EINVAL` unless: the event is sampling; the event's
  `AMD64_RAW_EVENT_MASK`-masked config equals `AMD_FAM19H_BRS_EVENT`; it is
  **not** frequency mode; and `sample_period > x86_pmu.lbr_nr` (i.e.
  `sample_period <= 16` → `-EINVAL`; `amd_brs_detect()` sets `lbr_nr = 16`).
  `amd_brs_setup_filter()` additionally returns `-EOPNOTSUPP` without
  `lbr_nr` and `-EINVAL` unless the branch filter, privilege bits aside, is
  exactly `PERF_SAMPLE_BRANCH_ANY` (no filtering).
  https://raw.githubusercontent.com/torvalds/linux/v6.10/arch/x86/events/amd/brs.c
  https://raw.githubusercontent.com/torvalds/linux/v6.14/arch/x86/events/amd/brs.c
- **`AMD_FAM19H_BRS_EVENT` = `0xc4`** (`RETIRED_TAKEN_BRANCH_INSTRUCTIONS`),
  defined in `arch/x86/events/perf_event.h` (not the uapi/asm header).
  https://raw.githubusercontent.com/torvalds/linux/v6.10/arch/x86/events/perf_event.h
- **Why the generic probe fails:** `amd_perfmon_event_map` renders
  `PERF_COUNT_HW_BRANCH_INSTRUCTIONS` as `0x00c2` (≠ `0xc4`), and this tree's
  `sample_period = 1` fails the `> 16` gate — `-EINVAL` twice over
  (`arch/x86/events/amd/core.c`, both tags; on BRS parts
  `amd_pmu_branch_hw_config` is wired to `amd_brs_hw_config` via
  `amd_core_pmu_init`'s `else if (!amd_brs_init())` branch — Zen 4+ takes the
  LBRv2 branch instead).
  https://raw.githubusercontent.com/torvalds/linux/v6.10/arch/x86/events/amd/core.c
  https://raw.githubusercontent.com/torvalds/linux/v6.14/arch/x86/events/amd/core.c
- **`amd_brs_adjust_period` does not exist** at v6.10 or v6.14 (confirmed
  absent from brs.c, events/perf_event.h, and amd/core.c at both tags). It
  was added by commit ada543459cab (2022-03-22) and removed by 3c27b0c6ea48
  ("perf/x86/amd: Fix AMD BRS period adjustment") within the same merge
  window; the replacement is `amd_pmu_limit_period()`:
  `if (has_branch_stack(event) && *left > x86_pmu.lbr_nr) *left -= x86_pmu.lbr_nr;`
  — a subtraction, not a gate. The hard gate is `amd_brs_hw_config` above.
  This is why T5 corrects the plan's Phase 6 sentence.
  https://github.com/torvalds/linux/commit/3c27b0c6ea48
  https://github.com/torvalds/linux/commit/28f0f3c44b5c
- **Kconfig:** `CONFIG_PERF_EVENTS_AMD_BRS`, bool "AMD Zen3 Branch Sampling
  support", `depends on PERF_EVENTS && CPU_SUP_AMD` — a kernel built without
  it has no BRS regardless of silicon (T8's skip-string split must not call
  that "no hardware" either; it is the same `EOPNOTSUPP` class as pre-5.19).
  https://raw.githubusercontent.com/torvalds/linux/v6.10/arch/x86/events/Kconfig
- **Provenance:** founding commit ada543459cab ("perf/x86/amd: Add AMD Fam19h
  Branch Sampling support", Stephane Eranian): "16-deep saturating buffer in
  MSR registers", "no branch type filtering", period "greater than 16 (BRS
  depth)", "fixed and not frequency mode".
  https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/ada543459cab7f653dcacdaba4011a8bb19c627c
  The AMD PPR citation (§2.1.13 "Branch Sampling", doc 55898 Rev 0.50, Family
  19h Model 01h Rev B1) comes from the patch posting
  (https://lwn.net/Articles/877245/) and AMD's official page
  (https://www.amd.com/en/support/tech-docs/processor-programming-reference-ppr-for-amd-family-19h-model-01h-revision-b1);
  the section title/subsections were verified only via a search index — AMD
  ships the PDF inside a zip — so cite the section number as "per the patch
  posting", not as independently read.
- **Caveat:** the v6.10 `amd_pmu_hw_config` early
  `has_branch_stack && !lbr_nr → -EOPNOTSUPP` line was verbatim-confirmed
  only at v6.14 (v6.10 confirmed for the static_call path and event-map
  entries); nothing in this doc depends on the difference.

## Out of scope

- **The AMD validation-checklist repair
  (P1 → [amd-ibs-backend-honesty.md#T2](amd-ibs-backend-honesty.md#T2)),
  `IBS_MAX_RECORD`/callchain
  (P3 → [amd-ibs-backend-honesty.md#T3](amd-ibs-backend-honesty.md#T3)),
  installing `asmtest_ibs.h`
  (P6 → [amd-ibs-backend-honesty.md#T7](amd-ibs-backend-honesty.md#T7)), the
  IBS caps append-order note
  (P7 → [amd-ibs-backend-honesty.md#T5](amd-ibs-backend-honesty.md#T5)), and
  the `sample_period`/`period_jitter` test gap
  (P8 → [amd-ibs-backend-honesty.md#T6](amd-ibs-backend-honesty.md#T6))** —
  all owned by
  [amd-ibs-backend-honesty.md](amd-ibs-backend-honesty.md), including
  rewriting `amd-hardware-validation.md` (remember: post-5d8e0d2, a
  `truncated=0` where escalation must fire is a **regression** signal, not a
  known finding). The P-numbers are the followup plan's; the `#T<n>` anchors
  are that doc's own tasks (its T1-T7), per the cross-doc reference etiquette.
- **The self-hosted Zen CI runner (P9)** — the structural fix for every
  "self-skips in hosted CI" note above:
  [self-hosted-ci-runners.md](self-hosted-ci-runners.md).
- **The `ibs_probe` misreport** paired with P1's acceptance:
  [asmspy-cli-enhancements.md](asmspy-cli-enhancements.md).
- **Whole-window capture ambitions on AMD** — a recorded hardware dead end
  (declined in [amd-tracing-plan.md](../plans/amd-tracing-plan.md)); the PT
  whole-window substrate is
  [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md).
