# Data-flow producer and block-step replay correctness, plus F2 record-and-inject for impure instructions — implementation

> **Sources.** Actioned from
> [2026-07-17-dataflow-tier-open-followups.md](../analysis/2026-07-17-dataflow-tier-open-followups.md)
> (item 1, item 4),
> [live-attach-dataflow-followup-plan.md](../archive/plans/live-attach-dataflow-followup-plan.md)
> (F1's pin follow-on, F2's increment-2 carryover, F6's known-limits list), and
> [2026-07-15-blockstep-value-spike.md](../analysis/2026-07-15-blockstep-value-spike.md)
> (gotchas 5 and 6). Written 2026-07-17. If this doc and a source disagree, this
> doc wins (sources may be stale); if the CODE and this doc disagree, re-verify
> before implementing. All file/line pointers below were re-verified against the
> working tree on 2026-07-17.

## Why this work exists

The live-attach data-flow tier can currently *lie in three narrow ways and
forfeit its win in two more*: a helper stepped over by the scoped producer can
silently fabricate a def-use edge (the value is honest, only the edge lies, so
no existing oracle can see it); the block-step replay compares and records
architecturally-**undefined** EFLAGS bits as if silicon defined them; and a
sub-register write (`r8d`, `ah`) is invisible to the last-writer map. Separately,
a `rdtsc`/`cpuid`/`rdrand` anywhere in a region — and a constant-pool island
anywhere in a JIT region — each throw away the ~6× perturbation win F1/F2
earned, falling back to whole-region single-step. This doc closes all five, and
records the one upstream gate (Unicorn AVX TCG) with a sentinel so its trigger
cannot be missed.

## What already exists (verified 2026-07-17)

- [src/dataflow_ptrace.c](../../../src/dataflow_ptrace.c) — the scoped
  single-step value producer (ships **no header**; suites re-declare its
  surface). Key internals at current line numbers:
  - the call-out step-over design comment, *"(recording NOTHING over the
    helper)"* — :59–69 and :998–1008; the call-out branch of `dfp_step_loop`
    (:874) is at :1009–1034, using `dfp_is_callout` (:829) and `dfp_run_to`
    (:1083, int3 + `PTRACE_CONT`).
  - the F6 **windowed** gap-barrier machinery this doc extends to the scoped
    path: `dfp_riskset` (:457–469), `dfp_risk_flag`/`dfp_risk_add`
    (:516–560), the `finalize_step` feed (:672–675, active only when
    `c->risk != NULL` — "NULL on every scoped path — this is a no-op there"),
    `dfp_risk_snap` (:2079), `dfp_emit_gap` (:2100), `dfp_alias_shape`
    (:1965 — "Deliberately covers EXACTLY the ids gp_value resolves … gp_value
    has no case for R8D/R8W/R8B..R15D/R15W/R15B"), `dfp_alias_slice` (:2033),
    and the windowed loop's gap entry/exit sites (:2324–2361).
  - `gp_value` (:230) — folds sub-registers to their 64-bit container; has
    full alias cases for rax…rsp but **only the 64-bit id** for r8…r15.
  - the telemetry struct `asmtest_dfwin_info_t` (:115–138) with its layout
    guard `asmtest_dataflow_ptrace_win_info_layout` (:2376).
- [src/dataflow.c](../../../src/dataflow.c) — the shared def-use builder.
  `emit_read` queries the last-writer map with the **raw** Capstone register id
  (`lw_get(map, 0, rec->reg, …)`, :219); `apply_write` keys registers raw and
  memory **per byte** (:244–252). No sub-register canonicalization exists.
- [src/dataflow_blockstep.c](../../../src/dataflow_blockstep.c) — the F1/F2
  block-step + Unicorn-replay tier (no header; suite re-declares):
  - `asmtest_blockstep_opts_t` (:250–277, with test hooks `no_vec_seed`,
    `no_mxcsr_seed`, `no_syscall_inject`, `inject_divergence`, …) and
    `asmtest_blockstep_info_t` (:280–305); the **info** layout guard
    `asmtest_dataflow_blockstep_info_layout` (:315–320). There is **no opts
    layout guard** today.
  - `EFLAGS_STEP_BITS` (TF|RF, :353) and `EFLAGS_ARITH_MASK` `0xCD5`
    "CF PF AF ZF SF DF OF" (:355–356); the canary `regs_coherent` compares
    `eflags & EFLAGS_ARITH_MASK` at :1529–1530 — **AF (bit 4) is compared,
    never masked**. `gp_value`'s EFLAGS case masks only TF/RF (:739–740).
  - the impurity split `DFB_IMP_SYSCALL / DFB_IMP_INT80 / DFB_IMP_OTHER`
    (:534–553); the per-step decode + syscall injection in `step_block`
    (:1538, injection at :1575–1593); `DFB_IMP_OTHER` returns `-1` — *"no
    boundary exists to record from — truncate"* — at :1596. `grep -c DR0
    src/dataflow_blockstep.c` → 0: no debug-register plumbing here yet.
  - `region_scan` (:1132) — a linear byte sweep; its comment (:1106–1131)
    concedes *"A production classifier would follow the JIT method-map's real
    instruction extents"*. `dfb_scan_t` (:987–999), `scan_replay_ok` (:1003),
    the encoding gate `insn_is_vex_evex` (:1036–1044).
  - the forward pass `capture_blockstep` (:1617; `PTRACE_SINGLEBLOCK` loop
    from :1713) and the shared capture core `finalize_step`/`open_step`/
    `capture_at` (:831/:857/:935) used by BOTH the replay and the single-step
    oracle path — the byte-identical property rests on that sharing.
- [src/ptrace_backend.c](../../../src/ptrace_backend.c):477–499 —
  `DR_OFFSET(n)` + `set_hw_bp`/`clear_hw_bp`: the DR0/DR7 exec-breakpoint
  plumbing (static to that file).
  [cli/asmspy_engine.c](../../../cli/asmspy_engine.c):2110–2134
  (`RGN_DR_OFFSET`, arm/disarm) and :4124 (`asmspy_engine_watch`, F3's
  per-thread DR0-3/`PTRACE_POKEUSER` pattern) are the two existing local
  copies of that pattern to mirror.
- Suites: [examples/test_dataflow_blockstep.c](../../../examples/test_dataflow_blockstep.c)
  (119 checks; fixtures deliberately avoid xor-to-zero — :139 — and include
  `island` :316, `imp_vec` :337, `blind_rdtsc` :477; layout-guard check at
  :1897–1904) and
  [examples/test_dataflow_ptrace.c](../../../examples/test_dataflow_ptrace.c)
  (126 checks; `call_region`/`callout_helper` fixtures :138–159, `test_callout`
  :1544 asserting **exactly 4** in-region steps at offsets {0,3,5,8} and a
  step0→step2 rax edge; `test_window_survey` :1013).
- Lanes: `make dataflow-blockstep-test`
  ([mk/dataflow.mk](../../../mk/dataflow.mk):433–440, chained into the
  `dataflow-test` aggregate at :390) and `make docker-dataflow-attach`
  ([mk/docker.mk](../../../mk/docker.mk):56–61, image
  [Dockerfile.dataflow-attach](../../../Dockerfile.dataflow-attach) with apt
  `libcapstone-dev libunicorn-dev`, runs `make WERROR=1 dataflow-test` under
  `seccomp=unconfined`). CI gate:
  [.github/workflows/ci.yml](../../../.github/workflows/ci.yml):640–678 —
  self-skips are failures except `live block-step/replay` **by name** (BTF is
  hypervisor-masked on GitHub VMs), assertion floor **285**; on bare metal the
  8 suites total **389** (ptrace 126, blockstep 119 — per commit `2221953`).
- **Prove the baseline before touching anything:**
  `make docker-dataflow-attach` — expect every suite's `1..N` footer, zero
  `not ok`, and on a VM exactly one `# SKIP live block-step/replay…` line;
  on the bare-metal Zen 5 box expect **389 ok / 0 skips**. Host-side:
  `make dataflow-blockstep-test` → `119/119` (or its clean
  `# SKIP … no libunicorn` if Unicorn is absent — install via the docker lane
  instead of the host, per CLAUDE.md).

## Tasks

### T1 — Complete the producer register map: r8–r15 sub-register aliases  (S, depends on: none)

**Goal.** `gp_value` and `dfp_alias_shape` resolve every GP sub-register alias
Capstone can emit on x86-64, so alias locations stop degrading surveys to
`truncated`.

**Steps.**
1. In [src/dataflow_ptrace.c](../../../src/dataflow_ptrace.c) extend `gp_value`
   (:230): add `X86_REG_R8D/R8W/R8B` … `X86_REG_R15D/R15W/R15B` cases folding
   to their containers (mirror the existing `EAX/AX/AL/AH → rax` shape at
   :233–238).
2. Extend `dfp_alias_shape` (:1965) with the same ids: `R8D..R15D` →
   `{shift 0, width 4}`, `R8W..R15W` → `{0,2}`, `R8B..R15B` → `{0,1}`. Keep the
   high-byte ids (`AH/BH/CH/DH` → `{shift 8, width 1}`) as they are; r8–r15
   have no high-byte forms.
3. Make the identical additions to the blockstep tier's own `gp_value`
   ([src/dataflow_blockstep.c](../../../src/dataflow_blockstep.c):657) — it is
   a separate copy with the same gap.
4. `make docker-dataflow-attach` after each file.

**Code.** Pure switch-case additions; no signature changes. Update the two
comments that document the gap
([src/dataflow_ptrace.c](../../../src/dataflow_ptrace.c):1960–1964 and the F6
known-limits row it mirrors) to say the gap is closed.

**Tests.** In
[examples/test_dataflow_ptrace.c](../../../examples/test_dataflow_ptrace.c),
add a scoped fixture whose region writes `r8d` then reads `r8` (e.g.
`mov r8d, esi; add rax, r8; ret`-shaped bytes) and assert (a) the `r8d` write
record has `value_valid` set with the correct value, (b) `!v->truncated`.
Failure today: the write record's value is missing (`gp_value` returns false).
Pass: value present, no truncation.

**Docs.** `CHANGELOG.md` `[Unreleased]` → `Fixed`: "the scoped/windowed
data-flow producers resolve r8d–r15b sub-register aliases". Internal-only
otherwise (the tier ships no public header).

**Done when.**
- `make docker-dataflow-attach` green, new checks included, no new skips.
- `grep -n "R8D" src/dataflow_ptrace.c src/dataflow_blockstep.c` shows both
  `gp_value` copies and `dfp_alias_shape` carry the cases.

### T2 — Gap barrier for the scoped call-out step-over (DFP-CALLOUT-1)  (M, depends on: T1)

**Goal.** A helper stepped over by `dfp_step_loop` can no longer fabricate a
def-use edge: the at-risk set is snapshotted before `dfp_run_to`, diffed at
return, and one synthetic GAP step carries exactly what the helper changed —
landing **in the same diff** as the oracle-suite updates it disturbs.

**Steps.**
1. Wire the existing risk set into the scoped paths: in
   [src/dataflow_ptrace.c](../../../src/dataflow_ptrace.c), allocate a
   `dfp_riskset` for scoped captures (mirror the window path's
   `calloc` at :2454) and set `c->risk`, so the `finalize_step` feed
   (:672–675) starts populating it. All scoped entry points (`_run`,
   `attach`, `attach_pid*`, `attach_jit`) share `dfp_step_loop`, so one wiring
   site covers them.
2. Make overflow **deferred** in scoped mode: `dfp_risk_flag` (:516) today
   sets `vt->truncated` eagerly, which is correct for windows (elision is
   guaranteed) but would falsely truncate a scoped capture that never calls
   out. Split it: always set `risk->overflow`; set `vt->truncated` immediately
   only in `win_mode`, otherwise record a pending flag that the call-out
   branch promotes to `truncated` at the first gap (and that is discarded at a
   gap-free exit).
3. In the call-out branch (:1009–1034): before `dfp_run_to`, capture
   `gap_pre = regs` (the callee-entry state already in hand), a
   `dfp_vecsnap`, and `dfp_risk_snap(c)`; `gap_pc = regs.rip` (the helper's
   entry — outside the region by construction). After `dfp_run_to` succeeds
   and before `skip_step = 1; continue;`, do `PTRACE_GETREGS` + `dfp_vecsnap`
   and call `dfp_emit_gap` with those pre/post pairs.
4. Make `dfp_emit_gap`'s `info` parameter NULL-tolerant (scoped paths have no
   `asmtest_dfwin_info_t`); guard the two `info->gap_*` increments.
5. Update the disturbed assertions in the same diff (see Tests) and re-run
   `make docker-dataflow-attach`; on the Zen 5 box also
   `make dataflow-blockstep-test` and `make docker-gccanon-attach` (its F4
   lane counts steps across call-out windows).

**Code.** Registers compare **the alias's own bit slice** via
`dfp_alias_shape`/`dfp_alias_slice` (T1 widened the coverage), memory per
byte, exactly as `dfp_emit_gap` already does — reuse it unchanged apart from
the NULL-`info` guard. Fail closed on anything undecidable (`truncated`), per
the barrier's existing contract (:453–456). Do not make the barrier blanket:
F6 measured that a blanket invalidation deletes true cross-gap edges (the
plan's M3 mutant), so precision is load-bearing.

**Tests.** In
[examples/test_dataflow_ptrace.c](../../../examples/test_dataflow_ptrace.c):
- `test_callout` (:1544) changes honestly and MUST be updated: the helper's
  `ret` pops the return address, so `rsp` (recorded by the `call`'s own write)
  differs between gap entry and exit → one GAP step is now appended at the
  helper's entry pc. Assert the new stream: 5 steps, the GAP step's `insn_off`
  equal to the helper address (outside `[base, base+9)`), carrying an `rsp`
  write with the post-return value; the rax edge becomes step0 → step3 and
  must still exist (the helper preserves rax — no rax GAP record may appear).
- Add the fabricated-edge fixture this item exists for: a region that writes
  `rcx` before the call, a helper that clobbers `rcx`, and a post-call read of
  `rcx`. Assert the read's edge goes to the **GAP step**, not the stale
  in-region writer, while the value assertion alone would still pass — the
  discriminating check F6's M1/M2 mutants proved necessary.
- Add a negative control: with the helper preserving everything at risk, no
  GAP step is appended (stream identical to today's minus none).
- Recalibrate the ci.yml floor comment's per-suite totals if counts change
  (pattern: commit `2221953`); the 285 floor itself only ever rises.

**Docs.** `CHANGELOG.md` `[Unreleased]` → `Fixed`. Append a "LANDED" note to
item 1 of
[2026-07-17-dataflow-tier-open-followups.md](../analysis/2026-07-17-dataflow-tier-open-followups.md)
in the same style as its items 2/3.

**Done when.**
- The clobber fixture's edge lands on the GAP step; disabling the barrier
  (temporary local mutation) fabricates the edge while values stay green.
- `make docker-dataflow-attach` green (updated counts, 0 unexpected skips);
  `make docker-gccanon-attach` green (37 assertions, adjusted if step
  numbering shifted).
- Bare-metal re-verification of the blockstep suite recorded in the commit
  message (`make dataflow-blockstep-test` on the Zen 5 box — the hardware
  gate below).

### T3 — Sub-register-aware last-writer resolution in the shared builder  (M, depends on: T1; coordinate with T2)

**Goal.** `asmtest_defuse_build` resolves cross-alias register def-use (write
`eax`, read `ax`; write `r8d`, read `r8`) instead of missing the edge, keyed
per container-byte exactly as memory already is.

**Steps.**
1. In [src/dataflow.c](../../../src/dataflow.c), add a canonicalization helper
   `static int reg_slice(uint32_t reg, uint32_t *container, uint16_t *off,
   uint16_t *len)` mapping a Capstone GP id to (64-bit container id, byte
   offset, byte length) — same shape as `dfp_alias_shape`
   ([src/dataflow_ptrace.c](../../../src/dataflow_ptrace.c):1965) but keyed to
   container+bytes. Ids it cannot map (vector, segment, `X86_REG_EFLAGS`
   stays whole) fall through to today's raw-id behavior.
2. Change `apply_write` (:244) to `lw_put` one entry per container byte for
   mappable registers (namespace them apart from memory keys — the map already
   carries an `is_mem` axis in `lw_put(map, 0|1, …)`), and `emit_read` (:215)
   to collect distinct producers across the read's byte range, mirroring the
   memory loop at :223–241.
3. Run the pure suites first (`make dataflow-test` inside the docker lane) —
   this changes **edges only**, never records, so the F1/F2 byte-identical
   record comparisons are untouched by construction; slice-level oracles may
   legitimately gain edges and their assertions are updated in this diff.
4. Add fixtures (below), then `make docker-dataflow-attach`.

**Code.** Keep `AH` exact: byte offset 1 of the rax container, so a write to
`ah` must produce an edge to a later `ah`/`ax`/`eax`/`rax` read but not to an
`al` read. This is the builder-side twin of the barrier's slice compare and
must agree with it — a divergence between `reg_slice` and `dfp_alias_shape`
is itself a bug; add a comment cross-referencing the two.

**Tests.** In [examples/test_dataflow.c](../../../examples/test_dataflow.c)
(the pure builder suite): synthetic traces asserting (a) write-`eax` →
read-`ax` produces the edge; (b) write-`ah` → read-`al` produces **no** edge;
(c) write-`r8d` → read-`r8` produces the edge (the T1 fixture's builder half).
In [examples/test_dataflow_ptrace.c](../../../examples/test_dataflow_ptrace.c):
the bundle-mandated live fixtures — a windowed survey whose glue clobbers a
**sub-register alias** the survey recorded (assert the barrier emits the
alias-sliced GAP record and the post-gap read resolves to it), and a
**vector-clobber-across-gap** fixture (glue clobbers an XMM reg the survey
recorded; assert the GAP step carries the `wide[]` record — F6 known-limit 4,
currently unexercised).

**Docs.** `CHANGELOG.md` `[Unreleased]` → `Fixed` ("cross-alias register
def-use edges resolved"). Annotate the F6 known-limits rows (3) and (4) in
[live-attach-dataflow-followup-plan.md](../archive/plans/live-attach-dataflow-followup-plan.md)
as closed by this doc.

**Done when.**
- All three synthetic-builder cases pass; case (b) is the discriminator (a
  container-collapsing implementation fails it).
- Both live fixtures pass in `make docker-dataflow-attach` (the windowed
  survey path runs on VMs — it uses `PTRACE_SINGLESTEP`, not BTF).

### T4 — Undefined-EFLAGS classification: mask the canary, normalize captured flags (BSVS-1)  (M, depends on: none)

**Goal.** The block-step tier neither compares nor records
architecturally-undefined EFLAGS bits as if defined: an explicit
mnemonic+count table masks them from the coherence canary and from captured
EFLAGS records on **both** value paths, preserving byte-identity.

**Steps.**
1. In [src/dataflow_blockstep.c](../../../src/dataflow_blockstep.c) add
   `static uint64_t dfb_undef_flags(const cs_insn *in, const struct
   user_regs_struct *gp, uint64_t *defined_written)`: returns the EFLAGS bits
   this instruction leaves undefined, and via `defined_written` the bits it
   writes with defined values. Small explicit table keyed on `insn->id`
   (~30 mnemonics, from Intel SDM Vol 1 App A / AMD APM Tab F-1 — see
   Research notes), including at minimum:

   | class | undefined | notes |
   |---|---|---|
   | `and/or/xor/test` | AF | CF/OF cleared, SF/ZF/PF defined |
   | `mul/imul (1-op)` | SF ZF AF PF | CF/OF defined |
   | `imul (2/3-op)` | SF ZF AF PF | CF/OF defined |
   | `div/idiv` | CF OF SF ZF AF PF | all six |
   | `bsf/bsr` | CF OF SF AF PF | ZF defined |
   | `shl/shr/sal/sar` | count-dependent | count 0: writes nothing; count 1: AF only; count>1: AF+OF; shl/shr count≥width: +CF |
   | `rol/ror/rcl/rcr` | count-dependent | count 0: nothing; count 1: defined; count>1: OF |
   | `bt/bts/btr/btc` | OF SF AF PF | CF defined |
   | `lzcnt/tzcnt/popcnt` | none | all written defined |

   Resolve shift/rotate counts from the **immediate** when present, else from
   the live `cl` in `gp` (the replay and silicon executed the same seeded
   count, so the runtime value is exact, not a guess); if `gp` is NULL fall
   back to the conservative union for that mnemonic.
2. Per-step record normalization: stash the open step's undefined mask in
   `cap_ctx` (`c->cur_undef`, set where the step is opened/decoded) and mask
   it out of the EFLAGS **write** record's value in the shared core
   (`finalize_step`, :831) — both `capture_singlestep` and the replay flow
   through it, so byte-identity is preserved and the record stops presenting
   Unicorn's (or silicon's) arbitrary choice as fact. Extend the
   `EFLAGS_STEP_BITS` comment (:349–354): this is the same "documented
   normalization" contract, per-instruction instead of global.
3. Canary masking: in `step_block` (:1538) accumulate
   `undef_acc = (undef_acc & ~defined_written) | undefined` per replayed
   instruction; store it in `cap_ctx`, and change the boundary compare
   (:1529–1530) to `(eflags & EFLAGS_ARITH_MASK & ~undef_acc)` on both sides.
   Reset `undef_acc` at each block seed.
4. Add the tier's **first opts layout guard**:
   `asmtest_dataflow_blockstep_opts_layout(size_t *size, size_t *last_off)`
   (mirror :315–320), because this task appends opts test hooks and the suite
   re-declares the struct — the exact silent-skew hazard the info guard
   exists for. Suite asserts it before building any opts.
5. New opts hooks (appended at the end of the struct): `no_undef_mask`
   (disable both mask sites — negative control) and `inject_flag_bit` (flip a
   chosen EFLAGS bit in Unicorn's computed end-of-block state before the
   canary — makes the mask's discrimination testable on hardware where
   Unicorn and silicon happen to agree, which the spike measured they do
   here).
6. `make docker-dataflow-attach` (the table tests run on VMs; only the live
   half self-skips).

**Code.** The table is the authority; Capstone's `cs_x86.eflags` may be added
as a **debug cross-check only** — it is unreliable exactly where this tier is
sensitive (VEX forms carry mask 0, `KORTESTB` has corrupt data, `LZCNT`
omits `MODIFY_CF`, count-dependence is inexpressible; see Research notes),
and this repo already measured its sibling metadata lying for AVX
([src/dataflow_operands.c](../../../src/dataflow_operands.c):121–132). Never
assert the **value** of an undefined bit anywhere: silicon varies by vendor
and generation.

**Tests.** In
[examples/test_dataflow_blockstep.c](../../../examples/test_dataflow_blockstep.c):
- Table unit checks (run everywhere Capstone exists, VM included): assemble
  ~15 representative encodings as byte arrays, decode, assert the
  (undefined, defined_written) pair per row above — including `shl reg, 1` vs
  `shl reg, 5` vs `shl reg, cl`, and `xor eax,eax` → AF undefined.
- A **xor-to-zero fixture** (the surface :139 deliberately dodges today):
  region uses `xor eax,eax`; assert replay is used (`info.pure==1`), not
  truncated, and byte-identical to single-step — with the AF bit of every
  post-`xor` EFLAGS record asserted **0** on both paths (the normalization is
  observable regardless of what silicon chose).
- Canary discrimination (live half, bare-metal): `inject_flag_bit=AF` after a
  xor block → **no** truncation (masked); `inject_flag_bit=ZF` → truncation
  (defined bits still guarded). `no_undef_mask` + `inject_flag_bit=AF` →
  truncation (proves the mask, not luck, is what tolerates AF).

**Docs.** `CHANGELOG.md` `[Unreleased]` → `Fixed`. Annotate gotcha 5 of
[2026-07-15-blockstep-value-spike.md](../analysis/2026-07-15-blockstep-value-spike.md)
as landed.

**Done when.**
- All table checks and the xor fixture pass in `make docker-dataflow-attach`
  on a VM; the three canary-discrimination checks pass on the Zen 5 box.
- `grep -n "EFLAGS_ARITH_MASK" src/dataflow_blockstep.c` shows the canary
  compare taking the accumulated undef mask into account.

### T5 — F2 increment 2, forward half: record rdtsc/rdrand/cpuid via DR0-3 exec breakpoints  (M, depends on: T4 (same file — sequencing only))

**Goal.** The forward block-step pass gains a synthetic boundary at each
`rdtsc`/`rdtscp`/`rdrand`/`rdseed`/`cpuid` site — a hardware exec breakpoint
plus one single-step — so the retired values exist to inject; BTF gives these
no boundary (measured, :542–546), which is why this is a new primitive, not a
decoder change.

**Steps.**
1. Split `DFB_IMP_OTHER` (:552): add `DFB_IMP_HWREC` for
   rdtsc/rdtscp/rdrand/rdseed/cpuid; `sysenter` alone stays `DFB_IMP_OTHER`
   (no sane return-to-next-instruction contract — keep today's refusal).
2. Extend `dfb_scan_t` with the sites: `uint64_t hwrec_off[4]; size_t
   nhwrec; int hwrec_overflow;` filled by `region_scan`'s sweep. More than 4
   **distinct** sites → `hwrec_overflow=1` → the region keeps today's
   whole-region single-step fallback (4 is the architectural DR slot count —
   an honest, documented cap).
3. Add local DR plumbing to
   [src/dataflow_blockstep.c](../../../src/dataflow_blockstep.c):
   `DFB_DR_OFFSET(n)` + `dfb_arm_hw_bp(pid, slot, addr)` /
   `dfb_clear_hw_bps(pid)` via `PTRACE_POKEUSER` on `u_debugreg[]`, DR7
   per-slot `L=1, R/W=00, LEN=00` — mirror
   [cli/asmspy_engine.c](../../../cli/asmspy_engine.c):2110–2134 (the
   backend's `set_hw_bp` at
   [src/ptrace_backend.c](../../../src/ptrace_backend.c):485 is static and
   single-slot; do not widen its API). The tracee is `spawn_tracee`'s own
   child, so POKEUSER is permitted.
4. In `capture_blockstep` (:1617): after the entry boundary, arm one slot per
   scanned site (absolute `rbase + off`). In the stop loop, at each SIGTRAP,
   read DR6 (`PTRACE_PEEKUSER u_debugreg[6]`): bits 0–3 set → a **site hit**
   (rip == site, pre-execution — DR exec breakpoints fault before retiring).
   Clear DR6, then `PTRACE_SINGLESTEP` once, `waitpid`, `GETREGS` (+
   `xstate_read` when `want_vec`): that post-retirement snapshot **is** the
   next boundary `S_next`, carrying the instruction's real outputs. Feed it
   through the existing per-block flow (replay the pending block to the site,
   T6 injects there; reseed from `S_next`). A DR6 with only BS (bit 14) set
   is the ordinary BTF block boundary — unchanged path.
5. Disarm all slots on every exit path (region return, fault, truncation)
   before `reap` — `PTRACE_DETACH` never clears DRs (measured in the F3 work,
   [cli/asmspy_engine.c](../../../cli/asmspy_engine.c):2443 note), and
   hygiene here keeps the pattern honest even though the child is reaped.
6. Supply the architectural write set producer-locally where Capstone
   under-reports these instructions, exactly as F2-inc1 did for `syscall`
   (:96–101 comment): verify with a quick probe what `cs_regs_access` reports
   for each of the five; add the missing implicit writes (`rax/rdx` for
   rdtsc, `+rcx` for rdtscp, `rax/rbx/rcx/rdx` for cpuid, dest reg + **CF**
   for rdrand/rdseed) at the one shared `open_step` site so oracle and replay
   grow identical records.
7. `make dataflow-blockstep-test` on the Zen 5 box after each step (the live
   loop is unreachable on VMs).

**Code.** Forward-pass and scan changes only; `step_block` still returns `-1`
for an un-injected site (T6 lifts that). New info fields appended:
`uint64_t hw_hits` (site boundaries taken) — update
`asmtest_dataflow_blockstep_info_layout`'s `last_off` to the new final field
and the suite's re-declaration **in the same diff**.

**Tests.** Forward-only assertions in
[examples/test_dataflow_blockstep.c](../../../examples/test_dataflow_blockstep.c):
with a `rdtsc`-bearing fixture and `force_singlestep=0` but T6 not yet landed,
the capture must truncate exactly as today (unchanged behavior — this task
alone must not change verdicts); with `force_singlestep=1`, `info.hw_hits==0`
(no arming on the fallback). The load-bearing new check: a scan-level unit
test that `region_scan` reports the right `nhwrec`/offsets for a
multi-site fixture and `hwrec_overflow` for a 5-site one. These run on VMs.

**Docs.** Internal-only at this step (the increment is user-visible only once
T6 lands); no changelog entry yet.

**Done when.**
- Scan unit checks green on `make docker-dataflow-attach` (VM).
- On the Zen 5 box, a temporary diagnostic (or `hw_hits` telemetry via a
  T6-less run) shows DR hits being taken and boundaries snapshotted; suite
  totals unchanged otherwise.

### T6 — F2 increment 2, replay half: inject recorded values, gate per block  (M, depends on: T5)

**Goal.** A region containing `rdtsc`/`rdtscp`/`rdrand`/`rdseed`/`cpuid`
keeps block-step + replay: the replay injects each site's recorded post-state
from the synthetic boundary instead of executing it, and the region-level gate
admits such regions.

**Steps.**
1. In `step_block` (:1538), handle `DFB_IMP_HWREC` exactly like the syscall
   arm (:1575–1593): copy the instruction's architectural write set from the
   boundary snapshot `next` into Unicorn (for rdrand/rdseed also CF from
   `next->gp.eflags` — coordinate with T4's mask: CF is **defined** by
   rdrand, so it must remain canary-checked), advance rip past the site,
   `c->injected++`, and terminate the block at the boundary — the same
   "leave nothing to witness" structure: no replayed instruction runs on
   stale post-site state, because the next block reseeds from `S_next`.
2. Lift the gate: `scan_replay_ok` (:1003) admits regions whose impurities
   are all syscall/int80/**hwrec** (and `nhwrec <= 4`); `scan_reason` names
   `sysenter` or the overflow case when declining. The per-step decode
   remains independent of the scan (F1's central lesson, :79–88): an
   un-recorded hwrec site reached by the replay (island-hidden from the
   scan) still returns `-1` → `truncated` — fail closed.
3. Add opts hook `no_hw_record` (skip arming — the replay then reaches the
   site with no boundary and must truncate; proves the primitive is
   load-bearing, the `blind_rdtsc` discipline applied to the new path).
   Opts guard from T4 updated.
4. Fixtures + oracles (below), then `make dataflow-blockstep-test` on the
   Zen 5 box and `make docker-dataflow-attach` (new scan/unit checks run on
   VMs; live checks self-skip there).

**Code.** Keep `blind_rdtsc` (:477) semantics intact as a regression fixture:
it must now **replay with injection** and stay byte-identical (its rax/rdx
overwrite no longer matters — the injected values are real). Update its
comments/assertions accordingly rather than deleting the fixture: it remains
the witness that the per-step decode, not the canary, guards this path.

**Tests.** In
[examples/test_dataflow_blockstep.c](../../../examples/test_dataflow_blockstep.c):
- `cpuid` fixture (deterministic per host): byte-identical replay vs
  single-step oracle, `info.injected > 0`, `info.hw_hits > 0`, stops reduced.
- `rdtsc` fixture: two forked captures cannot byte-compare a timestamp —
  reuse F2-inc1's independent same-capture oracle (`sc_clock_gettime`
  pattern, plan F2 section): the replay's recorded write value must equal
  the same capture's real returned rax; plus monotonicity across two rdtsc
  sites in one region (second > first).
- `rdrand` fixture consuming CF (`jc`): replay follows the real branch
  (control-flow agreement with the recorded boundary proves CF injection).
- Cold-path fixture: a branch that skips the `rdtsc` block → full replay win,
  `injected==0`, `hw_hits==0`, `info.pure==1` — the per-BLOCK claim, i.e. the
  region is no longer punished for an unexecuted site.
- Negative controls: `no_hw_record` → `truncated` (attributably — same
  region single-steps to the correct value); 5-site fixture → single-step
  fallback with `reason` naming the cap.

**Docs.** `CHANGELOG.md` `[Unreleased]` → `Added` ("block-step replay covers
rdtsc/rdtscp/rdrand/rdseed/cpuid via hardware exec-breakpoint
record-and-inject; per-block gating"). Annotate the F2 carryover paragraph in
[live-attach-dataflow-followup-plan.md](../archive/plans/live-attach-dataflow-followup-plan.md)
and item 4 of the
[followups analysis](../analysis/2026-07-17-dataflow-tier-open-followups.md)
as landed.

**Done when.**
- All new fixtures green in `make dataflow-blockstep-test` on the Zen 5 box
  (recorded in the commit message with the ok-count); scan/table checks green
  on the VM lane; the lane still self-skips cleanly off bare metal with the
  one named `# SKIP live block-step/replay` reason.
- ci.yml per-suite totals comment updated (floor unchanged or raised).

### T7 — Extents-driven region scan: island-bearing JIT regions stay replay-eligible (BSVS-2)  (M, depends on: T4 (opts guard), T5 (scan struct — sequencing))

**Goal.** `region_scan` can sweep the region's **real instruction extents**
instead of one linear byte run, so a constant-pool island no longer forces the
fail-closed `decode` verdict — replay survives exactly the tier's target
domain.

**Steps.**
1. Producer-local type in
   [src/dataflow_blockstep.c](../../../src/dataflow_blockstep.c):
   `typedef struct { uint64_t off, len; } asmtest_blockstep_extent_t;` and two
   opts fields appended: `const asmtest_blockstep_extent_t *extents; size_t
   nextents;` (NULL/0 = whole region, today's behavior). Validate in
   `asmtest_dataflow_blockstep_run` (:1995): sorted, non-overlapping, inside
   `[region_off, code_len)` — else `DF_BLOCKSTEP_EINVAL`. Update the T4 opts
   layout guard + suite re-declaration in the same diff.
2. `region_scan` (:1132) gains `(extents, nextents)`: sweep each extent
   independently; `remaining != 0` **within an extent** still fails closed
   (`replayable=0 "decode"`, `touches_vec=1` — the HIGH-1 rule is preserved,
   it just stops firing on bytes the caller vouched are data); verdicts,
   `touches_vec`, and T5's site list aggregate across extents. Bytes outside
   every extent are never decoded.
3. The replay/per-step decode needs no change: `step_block` follows real
   control flow and never fetches an island. The public
   `is_pure`/`is_replayable`/`is_injectable` remain whole-blob (document
   that in their comments — extents are a `run()` capability).
4. Rewrite the "production classifier would" paragraph (:1129–1131) to
   describe the landed mechanism and its caller contract (the JIT method map
   — [src/dataflow_method.c](../../../src/dataflow_method.c) /
   the addr-channel that publishes bodies — is where a managed integration
   gets its extents; the tier stays agnostic about their provenance).
5. `make docker-dataflow-attach` (scan checks run on VMs), live run on the
   Zen 5 box.

**Code.** Keep the aggregation conservative: any extent failing closed fails
the region closed. An empty extent list entry (`len==0`) is EINVAL, not a
skip.

**Tests.** In
[examples/test_dataflow_blockstep.c](../../../examples/test_dataflow_blockstep.c),
reuse the existing `island` fixture (:316):
- With extents that hop the island: `region_scan` yields `replayable=1`, and
  the full capture is **byte-identical** to single-step with stops reduced —
  the exact replay forfeiture gotcha 6 named, recovered.
- Without extents: the existing fail-closed checks (:984–1023) stay untouched
  and green — they are now the negative control proving extents are what
  changed the verdict.
- EINVAL checks for unsorted/overlapping/out-of-range extents.

**Docs.** `CHANGELOG.md` `[Unreleased]` → `Added`. Annotate gotcha 6 of the
[spike doc](../analysis/2026-07-15-blockstep-value-spike.md) and the HIGH-1
row's island note in the
[follow-up plan](../archive/plans/live-attach-dataflow-followup-plan.md) as addressed.

**Done when.**
- Island-with-extents fixture byte-identical on the Zen 5 box; scan verdict
  checks green on the VM lane; no change to any existing check without an
  extents argument.

### T8 — Unicorn AVX-TCG upstream watch: sentinel, trigger conditions, pin-bump playbook  (S, depends on: none)

**Goal.** The day a Unicorn release ships QEMU ≥ 7.2 TCG, the repo finds out
from a failing named check — not from memory — and the bump follows a written
playbook; until then the gate stays exactly as is (the trigger is **not** met
as of 2026-07-17, see Research notes).

**Steps.**
1. Add one explicitly-named sentinel check to
   [examples/test_dataflow_blockstep.c](../../../examples/test_dataflow_blockstep.c)
   (near the `uc_vec_width` probes): stand up a raw `uc_engine`, execute
   `vaddps ymm0,ymm1,ymm2` (`C5 F4 58 C1`) and assert it returns
   `UC_ERR_INSN_INVALID`; execute VEX-128 `vpaddd xmm0,xmm1,xmm2` with
   distinct xmm0/xmm1/xmm2 seeds and assert the result shows vvvv **dropped**
   (xmm0 = old-xmm0+xmm2, the measured 2-operand lie). Check text: `"upstream
   sentinel: Unicorn still cannot run AVX — on FAILURE see
   docs/internal/implementations/dataflow-producer-correctness.md T8"`.
   This makes the existing implicit dependency on Unicorn's brokenness (the
   `vex128_liar` negative controls) explicit and self-documenting.
2. Add the same pointer to the `insn_is_vex_evex` comment block (:1022–1035).
3. Record the trigger in this doc (done here): re-probe **only** when the
   Unicorn tree's `qemu/VERSION` is ≥ 7.2 **and**
   `qemu/target/i386/tcg/decode-new.c.inc` exists in it. A 2.2.0 release or
   PR #2143 merging is NOT the trigger — both target QEMU 5.1.0, which still
   predates AVX TCG.
4. Playbook, executed only when the trigger fires: (a) re-run the probes —
   `vaddps ymm` and `zmm` must execute AND VEX-128 `vaddps` must honor vvvv
   (three-operand semantics) before anything is relaxed; (b) bump
   `UNICORN_VERSIONS` in
   [scripts/fetch-corresponding-source.sh](../../../scripts/fetch-corresponding-source.sh):20;
   (c) if the distro `libunicorn-dev` in
   [Dockerfile.dataflow-attach](../../../Dockerfile.dataflow-attach):20 /
   [Dockerfile.taint-attach](../../../Dockerfile.taint-attach):28 lags, build
   the pinned release from source following
   [scripts/build-capstone.sh](../../../scripts/build-capstone.sh)'s pattern
   with a `tarball-sha256` line in
   [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt)
   (CLAUDE.md pinning rule); (d) relax `insn_is_vex_evex` gating only for
   encodings the re-probe proved, add YMM/ZMM seeding (read-back-verified,
   like XMM), extend the seeding/breadth assertions
   (`info.vec_seeded` / `info.uc_vec_width` checks), and flip the sentinel to
   assert the new capability.

**Code.** Step 1–2 only; steps 3–4 are documentation and future work behind a
genuine upstream gate — the one legitimate stop CLAUDE.md allows (a
capability that exists in no release is not an installable dependency).

**Tests.** The sentinel IS the test. It runs wherever the blockstep suite
builds (VM lanes included — it needs Unicorn, not ptrace/BTF). A failure is
the intended alarm, and its message routes the reader here.

**Docs.** `CHANGELOG.md` `[Unreleased]` → `Added` (one line: upstream
sentinel). Annotate the F1 "revisit the pin" carryover in the
[follow-up plan](../archive/plans/live-attach-dataflow-followup-plan.md) with a
pointer to this task.

**Done when.**
- `make docker-dataflow-attach` green with the sentinel counted; grep finds
  the T8 pointer in both the suite and the gate comment.
- No pin, gate, or seeding change shipped now (trigger unmet).

## Task order & parallelism

- **Producer/builder track** (dataflow_ptrace.c + dataflow.c):
  T1 → T2 → T3. T2 and T3 both reshape suite expectations over the same
  builder and oracles — land them as consecutive diffs, full lane run
  between, never interleaved with another author's edits to those files.
- **Blockstep track** (dataflow_blockstep.c): T4 → T5 → T6 → T7. The order
  is file-conflict sequencing plus real dependencies (T4 introduces the opts
  guard T5–T7 extend; T6 needs T5; T7 aggregates T5's site list per extent).
- T8 is independent and can land any time.
- The two tracks touch disjoint files and can proceed **in parallel** by two
  people. Critical path: T4 → T5 → T6 (the F2-inc2 deliverable).

## Constraints & gates

- **Bare-metal x86-64 gate (real hardware gate — self-skip is legitimate).**
  `PTRACE_SINGLEBLOCK` needs DEBUGCTL.BTF, which hypervisors mask: every live
  blockstep check (T4 canary discrimination, T5/T6 end-to-end, T7 live
  captures) runs only on bare metal (the Zen 5 dev box qualifies); the CI
  lane's skip is allowed **by name only**
  ([ci.yml](../../../.github/workflows/ci.yml):640–649). Everything
  unit-testable (flag table, scan verdicts, layout guards, sentinel, builder
  fixtures) runs on VM lanes and must not hide behind that skip. The scoped
  single-step paths (T1–T3) run fully on VMs.
- **DR slots**: 4 hardware breakpoints exist; T5/T6's >4-distinct-sites cap
  is architectural, documented, and falls back honestly.
- **Upstream gate (T8)**: no Unicorn release runs AVX TCG; the pin is
  deliberately not bumped until the written trigger fires. Any future bump
  follows the CLAUDE.md pinning rule (pinned version + SHA-256 digest line).
- **No-header convention**: every change to `asmtest_blockstep_opts_t`,
  `asmtest_blockstep_info_t`, or `asmtest_dfwin_info_t` updates the matching
  layout-guard function (size AND final-field offset) and the suite's
  re-declaration in the **same diff** — size alone is proven insufficient
  (tail-padding skew).
- **Never assert undefined-flag values.** Undefined EFLAGS bits vary across
  silicon generations and vendors (and AMD documents BSF/BSR dest-unchanged
  where Intel says undefined) — fixtures may only assert masked/normalized
  values or divergence-tolerance, never a specific undefined bit value.
- **Oracle determinism**: the two-forked-tracee byte-identical oracle cannot
  compare nondeterministic values (rdtsc/rdrand) — those use the same-capture
  independent memory oracle; `cpuid` is deterministic per host and may
  byte-compare.
- When suite counts change, update the ci.yml per-suite totals comment
  (:662–671); the 285 floor only ever rises (commit `2221953` pattern).
- Formatting `make fmt` before commit; changelog entries under
  `## [Unreleased]`, one header each.

## Research notes (verified 2026-07-17)

- **Undefined-flag ground truth.** Intel SDM Vol 1 Appendix A "EFLAGS
  Cross-Reference" is the canonical table
  (<https://cdrdv2-public.intel.com/843827/253665-sdm-vol-1-dec-24.pdf>, via
  <https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html>);
  AMD's equivalent is APM Vol 3 doc 24594, Table F-1 in rev 3.28
  (<https://archive.org/stream/advancedmicrodevices_24594_3.28/24594_djvu.txt>;
  current rev <https://docs.amd.com/v/u/en-US/24594_3.37>). They agree on the
  masks used in T4 (MUL: SF/ZF/AF/PF undefined; DIV: all six; BSF/BSR: ZF
  defined, rest undefined) — but AMD additionally documents BSF/BSR dest
  **unchanged** on source==0 where Intel says undefined. Shift/rotate
  undefinedness is **count-dependent** (count 0 affects nothing; OF defined
  only for 1-bit forms; AF undefined for any non-zero count; CF undefined for
  SHL/SHR count ≥ width): SDM-derived per-instruction pages
  <https://www.felixcloutier.com/x86/sal:sar:shl:shr>,
  <https://www.felixcloutier.com/x86/rcl:rcr:rol:ror>,
  <https://www.felixcloutier.com/x86/lzcnt>. Real silicon varies across
  generations on these bits: <https://www.sandpile.org/x86/flags.htm>.
- **Capstone 5.0.1's eflags metadata is not a usable oracle here.** The masks
  live in the same auto-generated table as the per-operand access bits
  (`arch/X86/X86MappingInsnOp.inc`,
  <https://github.com/capstone-engine/capstone/blob/5.0.1/arch/X86/X86MappingInsnOp.inc>),
  carried forward from a legacy hand-curated table by
  `suite/synctools/mapping_insn_op-arch.py` — not derived from LLVM, XED, or
  the SDM
  (<https://github.com/capstone-engine/capstone/blob/5.0.1/suite/synctools/mapping_insn_op-arch.py>).
  Confirmed defects in the pinned tree: VEX forms of flag-writing
  instructions carry mask 0 (vptest/vcomiss report *no* flags); `KORTESTBrr`
  holds a register-enum in its flags field; LZCNT omits `MODIFY_CF`; PTEST
  uses the typo constant `RESET_0F`
  (<https://github.com/capstone-engine/capstone/issues/1894>); count
  dependence is collapsed both directions. Upstream: the v6 auto-sync
  regressed `test` entirely
  (<https://github.com/capstone-engine/capstone/issues/2576>), and a redesign
  is proposed (<https://github.com/capstone-engine/capstone/issues/2776>);
  see also <https://github.com/capstone-engine/capstone/issues/1696>,
  <https://github.com/capstone-engine/capstone/issues/2079>,
  <https://www.capstone-engine.org/changelog.html>. It is broadly right for
  legacy GPR ops (spot-checked ~20 mnemonics incl. MUL/DIV/BSF/AND/ADC), so
  a debug cross-check is fine; the explicit table is the authority. This
  matches the repo's own measured finding that the same pipeline mis-reports
  AVX store operands
  ([src/dataflow_operands.c](../../../src/dataflow_operands.c):121–132).
  Intel XED is the strongest machine-readable alternative if ever needed
  (<https://intelxed.github.io/ref-manual/group__FLAGS.html>).
- **Unicorn/AVX upstream state — trigger NOT met.** Latest release is
  **2.1.4** (2025-09-09;
  <https://api.github.com/repos/unicorn-engine/unicorn/releases/latest>);
  `qemu/VERSION` is **5.0.1** on both `master` and `staging`
  (<https://raw.githubusercontent.com/unicorn-engine/unicorn/master/qemu/VERSION>,
  <https://raw.githubusercontent.com/unicorn-engine/unicorn/staging/qemu/VERSION>).
  The only active rebase track, PR #2143, targets QEMU **5.1.0**
  (<https://github.com/unicorn-engine/unicorn/pull/2143>; still pending per
  <https://github.com/unicorn-engine/unicorn/issues/2275>,
  <https://github.com/unicorn-engine/unicorn/discussions/2252>) — and 5.1.0
  still predates AVX TCG, which QEMU gained in **7.2**
  (<https://www.qemu.org/2022/12/14/qemu-7-2-0/>;
  `decode-new.c.inc`/`emit.c.inc` present in
  <https://github.com/qemu/qemu/tree/v7.2.0/target/i386/tcg>). Upstream
  corroboration of the hazards: open issue #1879 (256-bit vmovups →
  `UC_ERR_INSN_INVALID`, CPUID avx:0;
  <https://github.com/unicorn-engine/unicorn/issues/1879>), #1359, and the
  historical VEX mis-execution #1074
  (<https://github.com/unicorn-engine/unicorn/issues/1074>). No upstream
  issue tracks the VEX-128 vvvv drop on the QEMU-5.0.1 base — the
  authoritative record is this repo's probe
  ([src/dataflow_blockstep.c](../../../src/dataflow_blockstep.c):171–192).
  Repo alignment: `UNICORN_VERSIONS=2.1.4`
  ([scripts/fetch-corresponding-source.sh](../../../scripts/fetch-corresponding-source.sh):20);
  Unicorn intentionally has no digests entry (it is consumed as a distro
  binary); [CHANGELOG.md](../../../CHANGELOG.md):22–24 already records the
  `vaddps ymm → UC_ERR_INSN_INVALID` fact.

## Out of scope

- **`src/ptrace_backend.c` control-flow reconstructors** — application-int3
  classification, `rep` honesty, SP-aware `run_until`, IBS pre-cover:
  [ptrace-blockstep-tracer-correctness.md](ptrace-blockstep-tracer-correctness.md)
  (its T4 owns the re-entrancy-aware call-out in the backend; the scoped
  producer's own FIRST-arrival re-entrancy caveat at
  [src/dataflow_ptrace.c](../../../src/dataflow_ptrace.c):1004–1008 is a
  documented limit neither doc changes).
- **F5 (PT + code-image + replay)**:
  [dataflow-pt-replay-tier.md](dataflow-pt-replay-tier.md); the PT substrate
  itself: [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md).
- **The seven bindings' def-use/slice half and the code-image binding
  surface** (by-value `at_val_rec_t` marshalling):
  [dataflow-bindings-slice-codeimage.md](dataflow-bindings-slice-codeimage.md).
- **F4 object identity** (GCBulkType/Node/Edge — the landed address-identity
  F4 is correct, not defective):
  [dataflow-f4-object-identity.md](dataflow-f4-object-identity.md).
- **F3's AArch64 watchpoint analog** and asmspy surfaces:
  [asmspy-aarch64-support.md](asmspy-aarch64-support.md),
  [asmspy-cli-enhancements.md](asmspy-cli-enhancements.md).
- **In-process BTF block-step**:
  [inproc-btf-block-step.md](inproc-btf-block-step.md).
- **Running future-ISA AVX assembly under an emulator lane** (SDE):
  [pin-sde-future-isa-lane.md](pin-sde-future-isa-lane.md) — unrelated to
  the Unicorn replay pin.
