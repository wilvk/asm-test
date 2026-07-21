# Data-flow F5: PT + code-image + Unicorn-replay value tier — implementation

> **Sources.** Actioned from
> [live-attach-dataflow-followup-plan.md](../archive/plans/live-attach-dataflow-followup-plan.md)
> (§F5) and [hardware-trace-plan.md](../plans/hardware-trace-plan.md) (Phase 2,
> the code-image + recorder-backed decode). Written 2026-07-17. If this doc and a
> source disagree, this doc wins (sources may be stale); if the CODE and this doc
> disagree, re-verify before implementing.

## Why this work exists

There is today no data-flow value producer that reconstructs values **out of
band** — every shipped tier stops the target (`PTRACE_SINGLESTEP` on every
instruction, or `PTRACE_SINGLEBLOCK` once per taken branch). This doc adds the
least-perturbing ceiling: reconstruct the exact executed instruction stream from
an Intel PT trace (captured with **zero** single-steps), supply the bytes that
were live at trace time from the code-image recorder, and **replay that exact
path through the Unicorn emulator** to derive per-instruction values. The result
fills the same `asmtest_valtrace_t` every other producer fills, so the shared
def-use (L1) and slice (L2) analysis run over it unchanged. It matches the
single-step oracle on a deterministic region and self-skips cleanly where Intel
PT silicon is absent (the dev boxes are AMD; VMs and CI hide the PMU).

The honest boundary, stated up front and inherited from F1/F2: **PT gives control
flow and bytes, never values.** All values come from the replay. So a region that
consumes an unrecorded input — a syscall result, a concurrent sibling write, any
nondeterminism — cannot be reconstructed from PT alone and is **truncated
honestly**, never guessed.

**Ownership (binding, from the doc-set conflict resolution, position 9).** There
is exactly **one** Intel PT arm. The self-trace `perf_event_open`/AUX
open/mmap/drain helpers are owned by
[intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md);
[intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md) extends those
same helpers for foreign pids. **This tier opens no perf event and adds no PT
capture code.** It consumes a captured AUX blob plus a code-image and produces
values. Any reviewer who finds a second `perf_event_open` on the `intel_pt` PMU
in this tier's files should reject the change.

## What already exists (verified 2026-07-17)

The substrate this tier stands on is already in the tree:

- [src/pt_backend.c](../../../src/pt_backend.c) — the libipt decode backend,
  gated on `-DASMTEST_HAVE_LIBIPT` ([:65](../../../src/pt_backend.c#L65)); the
  `#else` block ([:361](../../../src/pt_backend.c#L361)) compiles `ENOSYS`
  stubs so it links on every host.
  - `asmtest_pt_decode_window(aux, aux_len, img, when, trace)`
    ([:224](../../../src/pt_backend.c#L224)) decodes a PT AUX stream against a
    **code-image** callback (`read_recorder`,
    [:99](../../../src/pt_backend.c#L99)) and fills an `asmtest_trace_t` with
    ordered instruction offsets **from the first decoded IP** (`base_ip`, kept
    local at [:246](../../../src/pt_backend.c#L246)). No production caller today.
  - `asmtest_pt_encode_fixture(buf, cap, base_ip, taken, out_len)`
    ([:305](../../../src/pt_backend.c#L305)) synthesizes a **valid PT AUX stream
    with libipt's own encoder** for the canonical ROUTINE walk
    (`mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret`), `taken`
    selecting the TNT bit — **no PT hardware required**. This is what makes the
    decode path CI-testable, and it is what this tier reuses to test the replay
    path in CI.
  - `asmtest_pt_read_codeimage` ([:50](../../../src/pt_backend.c#L50)) — the
    libipt-independent temporal byte adapter over
    `asmtest_codeimage_bytes_at`.
- [src/dataflow_emu.c](../../../src/dataflow_emu.c) — the **Unicorn L0 value
  producer**: `asmtest_dataflow_emu_run(code, code_len, args, nargs, max_insns,
  vt)` ([:253](../../../src/dataflow_emu.c#L253)) maps a code blob at a fixed
  guest base (`DF_CODE_BASE 0x00100000`,
  [:32](../../../src/dataflow_emu.c#L32)), runs it under Unicorn with a
  `UC_HOOK_CODE` + mem-read/write hooks that fill an `asmtest_valtrace_t`, and
  records **offsets** `off = address - base` ([:183](../../../src/dataflow_emu.c#L183)).
  This is the emulator this tier replays through, and it is the reference oracle
  everywhere in the data-flow design.
- [src/dataflow_blockstep.c](../../../src/dataflow_blockstep.c) — the F1/F2
  block-step + replay tier, which this tier inherits its purity/replayability
  tiering from. Re-usable public verdicts (the tier ships no header — its suite
  re-declares them):
  - `asmtest_dataflow_blockstep_is_pure(code, code_len, &reason)`
    ([:1908](../../../src/dataflow_blockstep.c#L1908)) — 1 if no
    OS-interacting/nondeterministic instruction.
  - `asmtest_dataflow_blockstep_is_replayable(code, code_len, &reason)`
    ([:1935](../../../src/dataflow_blockstep.c#L1935)) — 1 if Unicorn can
    faithfully execute it (`0`, `reason="vex/evex"` for any VEX/EVEX encoding —
    no released Unicorn runs AVX, and VEX-128 mis-executes **silently** with
    `UC_ERR_OK`).
  - The re-declared-struct **layout guard** convention:
    `asmtest_dataflow_blockstep_info_layout(size, last_off)`
    ([:315](../../../src/dataflow_blockstep.c#L315)) exports both `sizeof` **and
    the final field's offset**, because a tier that ships no header is
    re-declared by its suite and a field added on one side only lands in tail
    padding and silently skews every later field (F6 lost three green checks to
    exactly this). This tier follows the same convention.
- [src/codeimage.c](../../../src/codeimage.c) /
  [include/asmtest_codeimage.h](../../../include/asmtest_codeimage.h) — the
  time-aware code-image recorder: `asmtest_codeimage_new(pid)`
  ([:81](../../../include/asmtest_codeimage.h#L81); `pid==0` = self),
  `asmtest_codeimage_track/refresh/now/bytes_at`
  ([:90-112](../../../include/asmtest_codeimage.h#L90)), availability probe
  `asmtest_codeimage_available` ([:72](../../../include/asmtest_codeimage.h#L72)).
- [include/asmtest_valtrace.h](../../../include/asmtest_valtrace.h) — the shared
  sink/analysis: `asmtest_valtrace_append`
  ([:129](../../../include/asmtest_valtrace.h#L129)),
  `asmtest_valtrace_stash_wide` ([:135](../../../include/asmtest_valtrace.h#L135)),
  `asmtest_defuse_build` ([:190](../../../include/asmtest_valtrace.h#L190)),
  `asmtest_slice_forward`/`_backward`
  ([:206-212](../../../include/asmtest_valtrace.h#L206)). This tier fills the
  same `asmtest_valtrace_t` and touches none of this code.
- [mk/dataflow.mk](../../../mk/dataflow.mk) — the lane pattern this tier extends:
  `DF_HAVE_UNICORN` gate ([:332](../../../mk/dataflow.mk#L332)); the
  `dataflow-blockstep-test` lane ([:433-441](../../../mk/dataflow.mk#L433)),
  chained into the `dataflow-test` aggregate as a sub-make
  ([:390](../../../mk/dataflow.mk#L390)); the standalone-TAP explicit link rules
  that beat the root Makefile's generic `test_%` pattern
  ([:425-428](../../../mk/dataflow.mk#L425)).
- [Dockerfile.dataflow-attach](../../../Dockerfile.dataflow-attach) — the
  dataflow lane image: `ubuntu:24.04` + `libcapstone-dev` + `libunicorn-dev`,
  run by `make docker-dataflow-attach`
  ([mk/docker.mk:56-62](../../../mk/docker.mk#L56)) with
  `--security-opt seccomp=unconfined`. It does **not** yet carry `libipt-dev`
  (T2 adds it).
- **What does NOT exist** (bundle status, re-verified): `ls src/dataflow_pt.c` →
  `No such file`, and `grep -rnw "dataflow_pt" src/` = 0 (word-boundary match —
  do **not** use a bare `grep -rn "dataflow_pt" src/`, which returns 33 hits
  because `dataflow_pt` is a substring of the pre-existing `dataflow_ptrace.c`
  and `dataflow_dr*.c`; those are unrelated tiers, not a PT value producer);
  `grep 'Intel PT' src/dataflow*.c` = 0. There is no PT-derived value producer.
  `src/pt_backend.c` is control-flow-only, and `asmtest_pt_decode_window` has no
  production caller.

**Prove the baseline green before touching anything:**

```sh
make docker-dataflow-attach   # builds Dockerfile.dataflow-attach, runs
                              # `make dataflow-test` (includes the blockstep lane)
make docker-hwtrace           # builds Dockerfile.hwtrace (libipt), runs
                              # hwtrace-test incl. test_wholewindow_decode
```

Expected: each C harness ends `# N passed, 0 failed`, with `# SKIP` lines for the
live ptrace/PT paths on this AMD host and in containers. Also run `make check`
(framework self-tests) and `make help` (target list). If either lane is red on
`main`, stop and fix that first — do not build on a red baseline.

## Tasks

### T1 — The PT-path Unicorn replay core: `src/dataflow_pt.c`  (M, depends on: none)

**Goal.** A new value producer that, given an **ordered instruction-offset
stream** (the executed path), the region bytes, and seed inputs, replays the
region through Unicorn, cross-checks each executed offset against the supplied
path, and fills an `asmtest_valtrace_t` — byte-for-byte the same records the
emulator L0 produces for the same deterministic region and inputs.

**Steps.**

1. Create [src/dataflow_pt.c](../../../src/dataflow_pt.c). Mirror
   [src/dataflow_emu.c](../../../src/dataflow_emu.c)'s self-contained Unicorn
   client exactly: same guest layout constants (`DF_CODE_BASE 0x00100000`,
   `DF_STACK_BASE`, `DF_RET_MAGIC`), same SysV arg-register seeding, the same
   `df_on_code`/`df_on_mem` hook shape that fills the valtrace. **Do not extend
   `dataflow_emu.c`** — F5 is its own tier, like blockstep is; copy the pattern,
   do not couple.
2. Define the return codes and structs at the top of the file,
   **unconditionally** (outside the platform gate) so the ENOSYS stub and the
   real build report the same layout:

   ```c
   #define DF_PT_OK      0   /* clean replay; a complete value trace            */
   #define DF_PT_FAULT   1   /* path divergence / Unicorn fault: partial, truncated */
   #define DF_PT_EINVAL  (-1)/* bad arguments                                   */
   #define DF_PT_ENOSYS  (-3)/* off Linux x86-64 / no Capstone / no Unicorn     */

   typedef struct {
       int      pure;        /* the region-scan verdict carried through (T3)     */
       const char *reason;   /* why the replay was declined/truncated, else NULL */
       uint64_t steps;       /* instructions replayed into the trace             */
       uint64_t path_len;    /* offsets in the supplied PT path                  */
       uint64_t diverged_at; /* step index of the first path mismatch, or 0      */
       int      vec_seeded;  /* reserved: vector seeding parity with blockstep   */
   } asmtest_dataflow_pt_info_t;

   void asmtest_dataflow_pt_info_layout(size_t *size, size_t *last_off);
   ```

   Implement `asmtest_dataflow_pt_info_layout` to return
   `sizeof(asmtest_dataflow_pt_info_t)` and
   `offsetof(asmtest_dataflow_pt_info_t, vec_seeded)` — the same size+last_off
   guard [dataflow_blockstep.c:315](../../../src/dataflow_blockstep.c#L315) uses,
   for the same reason (a re-declared struct that skews silently).
3. The core entry (this tier ships **no header**; the suite re-declares it):

   ```c
   int asmtest_dataflow_pt_replay_path(const uint8_t *code, size_t code_len,
                                       const uint64_t *path, size_t path_len,
                                       const long *args, int nargs,
                                       asmtest_valtrace_t *vt,
                                       asmtest_dataflow_pt_info_t *info);
   ```

   `path[]` is the ordered sequence of executed instruction **offsets** into
   `code` (offset 0 = the region's first byte), i.e. exactly what a decoded PT
   trace yields. Behavior: set up Unicorn as `dataflow_emu.c` does, seed args,
   install the same hooks, run with `uc_emu_start(uc, DF_CODE_BASE, DF_RET_MAGIC,
   0, 0)`. In `df_on_code`, before recording the step, **cross-check** the
   current offset against `path[step]`: if it does not match, set
   `vt->truncated = true`, record `info->diverged_at`, stop the engine
   (`uc_emu_stop`), and return `DF_PT_FAULT`. A clean run to `DF_RET_MAGIC` whose
   executed offsets matched `path[]` step-for-step returns `DF_PT_OK`.
4. Guard the whole implementation body with the same platform gate blockstep
   uses (`#if defined(__linux__) && defined(__x86_64__) &&
   defined(ASMTEST_HAVE_CAPSTONE) && defined(ASMTEST_HAVE_UNICORN)`); the `#else`
   compiles a `DF_PT_ENOSYS` stub for `asmtest_dataflow_pt_replay_path` (the
   layout function stays outside the gate).
5. Add the build rule to [mk/dataflow.mk](../../../mk/dataflow.mk) next to the
   blockstep object ([:420](../../../mk/dataflow.mk#L420)), mirroring its
   `DFB_UNICORN_FLAGS` toggle:

   ```make
   $(BUILD)/dataflow_pt.o: src/dataflow_pt.c include/asmtest_valtrace.h \
                           include/asmtest_trace.h $(BUILD)/.build-flags | $(BUILD)
   	$(CC) $(CFLAGS) $(DFB_UNICORN_FLAGS) $(CAPSTONE_CFLAGS) $(CAPSTONE_DEF) -c $< -o $@
   ```

6. `make fmt` (clang-format is CI-gated via `fmt-check`), then build the object:
   `make build/dataflow_pt.o` (or the T5 test that links it).

**Code.** The replay must not compute branch conditions itself as the source of
truth — Unicorn does compute them (it executes the real instructions), and for a
**deterministic** region seeded with the recorded inputs its own resolution
**equals** the PT path. The cross-check in step 3 is therefore both the
correctness proof (the replay took the real path) and the nondeterminism detector
(a region whose branch depends on an unrecorded input diverges and truncates).
Record `off = address - DF_CODE_BASE` exactly as `df_on_code` does
([dataflow_emu.c:183](../../../src/dataflow_emu.c#L183)); the absolute guest base
is irrelevant because both the PT decode and the emulator key on offsets.

**Tests.** T5 owns the suite; T1's slice of it is `test_pt_replay_path_matches_emu`
in `examples/test_dataflow_pt.c`: build the offset path **by running the emulator
oracle first** (`asmtest_dataflow_emu_run` over the ROUTINE bytes and args
`{20,22}`), harvest its `insn_off[]` as the path, feed that path back into
`asmtest_dataflow_pt_replay_path`, and assert the two `asmtest_valtrace_t`s are
`memcmp`-identical over `recs`/`insn_off`/`wide` (the same byte-identical bar F1's
spike set). A negative control: perturb one offset in the path and assert the
replay returns `DF_PT_FAULT` with `vt->truncated` and a non-zero `diverged_at`.
This half needs **only Unicorn** (no libipt), so it runs wherever the blockstep
lane runs.

**Docs.** Internal-only at this stage (no user-facing surface until T4/T5 land the
lane); no CHANGELOG entry yet.

**Done when.**
- `make build/dataflow_pt.o` compiles clean (and the ENOSYS stub compiles on a
  non-x86-64 / no-Unicorn build — verify by `CFLAGS` without `-DASMTEST_HAVE_UNICORN`).
- `test_pt_replay_path_matches_emu` passes: the replayed trace is `memcmp`-identical
  to the emulator oracle on the deterministic ROUTINE; the perturbed-path control
  truncates.
- `make fmt-check` passes.

### T2 — Bridge the PT decode to the replay: `asmtest_dataflow_pt_replay`  (M, depends on: T1)

**Goal.** The full out-of-band entry: take a **captured AUX blob** + a code-image
+ a trace position `when` + seed inputs, decode the PT stream to the executed
offset path (against the time-correct bytes), materialize the region bytes from
the image at `when`, and drive T1's replay — the complete "PT + code-image +
replay" pipeline, CI-tested with the synthetic fixture (no PT hardware).

**Steps.**

1. Give the decode an offset-origin out-param so the caller can rebase. In
   [src/pt_backend.c](../../../src/pt_backend.c), add a NULL-tolerant
   `uint64_t *base_ip_out` to `asmtest_pt_decode_window` (real body
   [:224](../../../src/pt_backend.c#L224) **and** the ENOSYS stub
   [:375](../../../src/pt_backend.c#L375)), receiving the first decoded IP
   (currently the local `base_ip` at [:246](../../../src/pt_backend.c#L246)).
   Update the two existing callers in `test_wholewindow_decode`
   ([examples/test_hwtrace.c:3265](../../../examples/test_hwtrace.c#L3265) and
   [:3298](../../../examples/test_hwtrace.c#L3298)) to pass NULL — their
   assertions are unchanged — **and update the header-less prototype
   re-declaration of `asmtest_pt_decode_window` at
   [examples/test_hwtrace.c:113](../../../examples/test_hwtrace.c#L113) to the new
   6-parameter signature.** The suite ships no header and re-declares the symbol
   it links; those two callers live in that same TU, so a stale 5-parameter
   prototype makes them fail to compile with `too many arguments`. (Keep this in
   sync with [intel-pt-whole-window-substrate.md#T2](intel-pt-whole-window-substrate.md),
   which adds the same parameter and carries the same re-declaration.)

   > **Coordinate:** [intel-pt-whole-window-substrate.md#T2](intel-pt-whole-window-substrate.md)
   > adds the **same** `base_ip_out` parameter for the STRONG-tier window arm. If
   > that task has already landed, this parameter exists — reuse it, do **not**
   > add a second out-param. If it has not, add it here; it is source-incompatible
   > for direct C callers of the decode entry only (the facade/bindings do not
   > call it). Whichever doc lands the change owns the one CHANGELOG line.

2. In [src/dataflow_pt.c](../../../src/dataflow_pt.c), add the full entry (still
   header-less; the suite re-declares it):

   ```c
   int asmtest_dataflow_pt_replay(const uint8_t *aux, size_t aux_len,
                                  const asmtest_codeimage_t *img, uint64_t when,
                                  uint64_t region_base, size_t region_len,
                                  const long *args, int nargs,
                                  asmtest_valtrace_t *vt,
                                  asmtest_dataflow_pt_info_t *info);
   ```

   Body: (a) decode the AUX into an `asmtest_trace_t` via
   `asmtest_pt_decode_window(aux, aux_len, img, when, &trace, &base_ip)`; a
   negative decode rc (`ASMTEST_HW_EDECODE`) → `vt->truncated = true`, return
   `DF_PT_FAULT`. (b) The decoded `trace.insns[]` are offsets from `base_ip`;
   convert to offsets from `region_base` (`insns[i] + base_ip - region_base`),
   dropping any that fall outside `[0, region_len)` and truncating on an
   out-of-region IP (the region-boundary the plan names). (c) Materialize the
   region bytes: `asmtest_codeimage_bytes_at(img, (void*)region_base, when, &bytes,
   &avail)` and copy `min(avail, region_len)` — these are the **bytes live at
   `when`**, the code-image's whole reason for existing. (d) Call
   `asmtest_dataflow_pt_replay_path(bytes, region_len, path, path_len, args,
   nargs, vt, info)`.
3. This entry needs libipt at compile time (it calls `asmtest_pt_decode_window`).
   The object already links `pt_backend.o`'s decode when libipt is present; when
   libipt is absent, `asmtest_pt_decode_window` is the ENOSYS stub and
   `asmtest_dataflow_pt_replay` returns `DF_PT_FAULT`/truncated (no path to
   replay). Gate the *decode call site* on `ASMTEST_HAVE_LIBIPT` so a
   Unicorn-only build still links (T1's `_replay_path` stays usable directly).
4. **Add `libipt-dev` to the Unicorn-bearing dataflow image** so the full bridge
   is CI-testable. In [Dockerfile.dataflow-attach](../../../Dockerfile.dataflow-attach),
   add `libipt-dev` to the `apt-get install` line (it already installs
   `libcapstone-dev libunicorn-dev`). This is an **apt-pinned distro package**
   (Ubuntu noble `libipt-dev` 2.0.6-1build1 = libipt v2.0.6), exactly the form
   [Dockerfile.hwtrace](../../../Dockerfile.hwtrace) already uses (line 21) — the
   CLAUDE.md rule requires adding the dependency, not self-skipping, and an apt
   distro package needs no `scripts/third-party-digests.txt` line (only
   built-from-source deps do). With it installed, `native-trace.mk`'s header probe
   builds `pt_backend.o -DASMTEST_HAVE_LIBIPT`, so the synthetic-AUX bridge test
   runs. **This is the only file outside `src/`/`mk/`/`examples/` this doc
   touches, and it is a one-token apt addition.**
5. `make fmt`, then `make docker-dataflow-attach` (T5's lane runs inside it).

**Code.** The synthetic fixture is the CI key. `asmtest_pt_encode_fixture(buf,
cap, base_ip, taken, &len)` ([pt_backend.c:305](../../../src/pt_backend.c#L305))
produces a real PT AUX stream for the ROUTINE walk with **no PT PMU**; a scratch
`asmtest_codeimage_new(0)` tracking the same 18 ROUTINE bytes supplies the
temporal bytes. So `asmtest_dataflow_pt_replay(fixture_aux, …, img, when,
region_base=<tracked addr>, …, args={20,22}, …)` exercises decode → rebase →
materialize → replay end-to-end, on any host, exactly as
`test_wholewindow_decode` exercises the decode alone. `base_ip` in the fixture is
the tracked address, so the rebase is the identity in the test and non-trivial
only for a real capture whose window origin differs from the region base.

**Tests.** T5 owns the suite; T2's slice is `test_pt_replay_from_fixture`
(libipt+Unicorn gated): encode the taken fixture, track the ROUTINE bytes in a
self code-image, replay, and assert the value trace equals the emulator oracle for
`{20,22}` (result 42, `truncated == false`). A second case encodes the **not-taken**
fixture (args `{200,1}` → the `dec` at offset `0xe` runs) and asserts the replay
follows that path — the same TNT discriminator `test_wholewindow_decode` uses,
now proving the *replay* follows the decoded path and not a baked-in answer.

**Docs.** Add a `### Added` bullet under `## [Unreleased]` in
[CHANGELOG.md](../../../CHANGELOG.md) only once T5's lane is green: "Data-flow F5:
an out-of-band PT + code-image + Unicorn-replay value producer (silicon-gated live
capture; synthetic-AUX decode→replay validated in CI)." If T2 also lands the
`base_ip_out` decode change (i.e. the substrate doc has not), add the
source-incompatibility note there.

**Done when.**
- `make docker-dataflow-attach` is green with `libipt-dev` in the image and
  `test_pt_replay_from_fixture` passing (taken and not-taken).
- On a build **without** libipt, `asmtest_dataflow_pt_replay` returns
  `DF_PT_FAULT`/truncated and the object still links (grep the gate).
- `asmtest_pt_decode_window`'s existing callers still pass with the new
  `base_ip_out` argument (`make docker-hwtrace` green).

### T3 — Inherit F1/F2's OS-interaction tiering; truncate honestly  (S, depends on: T1)

**Goal.** Before replaying, decline any region PT+replay cannot faithfully
reconstruct — impure (OS-interacting/nondeterministic) or non-replayable
(VEX/EVEX) — and record **why**, because unlike the block-step tier F5 has **no
single-step fallback**: it is fully out-of-band, so a region it cannot replay is
truncated, not stepped.

**Steps.**

1. Link the blockstep verdicts into the F5 tier and its suite. Because
   `dataflow_blockstep.c` ships no header, add re-declarations at the top of
   [src/dataflow_pt.c](../../../src/dataflow_pt.c) (and the suite):

   ```c
   int asmtest_dataflow_blockstep_is_pure(const uint8_t *code, size_t code_len,
                                          const char **reason);
   int asmtest_dataflow_blockstep_is_replayable(const uint8_t *code,
                                                size_t code_len,
                                                const char **reason);
   ```

   Add `$(BUILD)/dataflow_blockstep.o` to the F5 test link line (T5) so the
   symbols resolve.
2. In `asmtest_dataflow_pt_replay` (and, when a path is supplied directly,
   `asmtest_dataflow_pt_replay_path`), run both gates over the materialized region
   bytes **before** seeding Unicorn. If `is_replayable` returns 0
   (`reason="vex/evex"`), set `info->reason`, `vt->truncated = true`, and return
   `DF_PT_FAULT` **without executing anything** — Unicorn mis-executes VEX-128
   silently (`UC_ERR_OK` with a wrong answer;
   [dataflow_blockstep.c:177-187](../../../src/dataflow_blockstep.c#L177)), so
   this gate is a correctness gate, not an optimization. If `is_pure` returns 0,
   set `info->pure = 0` and `info->reason` to the offending mnemonic and truncate:
   PT carries no retired value for a `syscall`/`rdtsc`/`cpuid`, so the emulator
   would fabricate one (measured: Unicorn runs `syscall` leaving the *number* in
   rax, `rdtsc` returns a fabricated counter, `cpuid` returns zeros —
   [dataflow_blockstep.c:79-83](../../../src/dataflow_blockstep.c#L79)). A `pure`
   region replays; an impure one is declined.
3. Document, in the file header and in `info->reason`, the **structural residual**
   that is inherent to PT+replay and cannot be lifted by more code:
   - **Syscall results** — F2's record-and-inject needs a *boundary* at which the
     kernel's retired value is read (`PTRACE_SINGLEBLOCK` gives it for free
     because `syscall` is a control transfer). F5 takes no ptrace stops at all, so
     there is no such boundary; the syscall's def cannot be injected. This is the
     one place F5 is **narrower** than F2, and it is a property of being fully
     out-of-band, not a gap to fill.
   - **Concurrency** — a sibling thread writing memory the region loads, with no
     syscall to anchor it, is unrecordable; the cross-check (T1) detects the
     resulting path divergence when it changes control flow, and truncates, but a
     divergence that only changes a *value* (not the path) is the documented
     residual the whole live-attach plan carries.
   - **Nondeterminism** — any input from an unrecorded source makes Unicorn's
     branch resolution diverge from the PT path → T1's cross-check truncates.

**Code.** No new capture or decode code — this task is purely the gate + honest
truncation + the residual documentation. The verdicts come from `region_scan`
inside `dataflow_blockstep.c`, which is a single, mutation-proven implementation;
F5 reuses it rather than growing a second scanner (which could disagree — F1's
central lesson).

**Tests.** T5 slice: `test_pt_gates` — an impure fixture (`cpuid` in the region)
asserts `asmtest_dataflow_pt_replay` returns `DF_PT_FAULT` with
`info->pure == 0`, `info->reason` naming `cpuid`, and `vt->truncated`; a VEX
fixture (`andn`/`vpaddd`) asserts `reason` names the VEX/EVEX gate and that
**nothing was executed** (steps == 0). Both run wherever Unicorn is present (the
gate needs Capstone, which the operand enumerator already requires).

**Docs.** Internal-only (the gate is not user-visible beyond "F5 truncates on
impure/AVX regions", which the tier docs in T5 summarize).

**Done when.**
- `test_pt_gates` passes: impure and VEX regions truncate with the correct reason
  and zero executed steps.
- Code review confirms F5 calls the blockstep verdicts and does **not** implement
  a second region scanner.

### T4 — Live foreign-pid end-to-end + single-step oracle match + gated lane  (L, depends on: T2, T3, and intel-pt-attach-foreign-pid.md#T1 (foreign-pid AUX capture) + #T2 (paired code-image))

**Goal.** On bare-metal Intel PT, capture a real AUX stream over a **foreign**
live process with **no single-step of the target**, decode + replay it through
the F5 pipeline, and assert the derived value trace matches the single-step
oracle on a deterministic region — self-skipping cleanly everywhere PT silicon is
absent.

**Steps.**

1. Consume the foreign-pid capture from the sibling doc. **This tier opens no
   perf event** (position 9): the foreign-pid AUX capture is owned by
   [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md), which
   extends the shared `pt_aux_open` helpers
   ([intel-pt-whole-window-substrate.md#T1](intel-pt-whole-window-substrate.md))
   for `pid > 0`. F5 calls that doc's capture entry to obtain **(a)** a linearized
   AUX blob + length and **(b)** the code-image the capture tracked, then feeds
   both to `asmtest_dataflow_pt_replay`. Treat the exact capture symbol as owned
   there; if its shape differs, adapt F5's call site — never add a parallel
   capture.
2. Add `test_pt_live_replay` to `examples/test_dataflow_pt.c`, guarded
   `#if defined(__linux__) && defined(__x86_64__)`, first probing PT availability
   (via the sibling capture's availability probe) and printing a specific reason
   on skip: `# SKIP pt live replay: no intel_pt PMU (needs bare-metal Intel;
   absent on AMD/VM/CI)`. Body: spawn a deterministic victim running the ROUTINE
   in a loop (reuse the [bindings/dataflow_victim.c](../../../bindings/dataflow_victim.c)
   shape — publishes `base=/len=/pid=` on stdout, args from argv so the expected
   result is a property of the run, not a hardcodable constant), capture its PT
   trace over one region invocation, replay, and assert (a) the value trace's
   region result equals `a+b`, (b) `truncated == false`, and (c) it matches the
   single-step oracle: capture the **same** region by
   `asmtest_dataflow_blockstep_run(code, len, args, nargs, &oracle_opts /*
   force_singlestep=1 */, …)` ([dataflow_blockstep.c:1995](../../../src/dataflow_blockstep.c#L1995))
   and compare **rsp-relative** (both traces are separate processes and differ by
   an absolute stack delta — normalize on the region-entry rsp / key on locations,
   exactly as the blockstep oracle does; see its file header
   [:114-119](../../../src/dataflow_blockstep.c#L114)).
3. Add the lane in [mk/dataflow.mk](../../../mk/dataflow.mk) mirroring
   `dataflow-blockstep-test` ([:433](../../../mk/dataflow.mk#L433)) — build where
   Unicorn is present, else a clean SKIP; at runtime self-skip where PT is absent.
   Add a **fail-not-skip** variant for a runner that claims PT
   (`ASMTEST_REQUIRE_PT=1` converts the availability skip into a `CHECK` failure,
   exactly as [intel-pt-whole-window-substrate.md#T5](intel-pt-whole-window-substrate.md)
   does for `hwtrace-pt-live`) so a silently-hidden PMU on a supposed-PT box goes
   red.
4. Wire the new lane's one-line description into `make help`
   ([Makefile:119](../../../Makefile#L119) area).
5. Add the docker lane `docker-dataflow-pt` in [mk/docker.mk](../../../mk/docker.mk)
   next to `docker-dataflow-attach` ([:56](../../../mk/docker.mk#L56)): it runs the
   **synthetic** half (T2/T3/T5) in the libipt+Unicorn image; the **live** half
   self-skips there (no PMU in a container on an AMD host). Add
   `test_dataflow_pt` to `SUITE_EXCLUDES` in the root
   [Makefile](../../../Makefile#L60) so the standalone-main suite stays out of
   `make test` (the same treatment `test_dataflow_blockstep` gets).
6. `make fmt`; `make docker-dataflow-pt` (must SKIP the live half cleanly); on PT
   silicon, run the fail-not-skip target.

**Code.** No Docker lane can run the live half — the gate is hardware (no
`intel_pt` PMU in VMs/containers on AMD hosts), one of the two legitimate
self-skip gates under the CLAUDE.md rule. This task delivers the test + target the
bare-metal runner ([self-hosted-ci-runners.md](self-hosted-ci-runners.md)'s scope)
will invoke, and **records the gate** rather than claiming validation. Until such a
box runs it, mark F5's plan status "wiring-complete, hardware-unvalidated," not
"validated" (verify-before-declaring-done).

**Tests.** This task IS the live test. Failure meanings on silicon: replay yields
zero steps → the decode's enable-event drain or the region rebase regressed;
oracle mismatch on a deterministic region → a seeding or hook bug; `truncated`
set on a pure region → a spurious path divergence (an unrecorded input or a decode
gap). On this AMD host the whole test prints one `# SKIP` line and the count is
unchanged.

**Docs.** Update the data-flow tier section of the user-facing tracing guide
[docs/guides/tracing/native-tracing.md](../../guides/tracing/native-tracing.md) —
that section is **created by T5** (the guide has no data-flow/valtrace section
before F5 lands it; today it documents only the DynamoRIO and Unicorn
control-flow tiers), so here you extend the section T5 added — once the smoke has
actually run on silicon: the PT-derived value path is wired and, when the box
exists, live-validated. Record the validation host. Update the F5 status block in
[live-attach-dataflow-followup-plan.md](../archive/plans/live-attach-dataflow-followup-plan.md)
**only after** it runs on silicon.

**Done when.**
- `make docker-dataflow-pt` green with the live half printing
  `# SKIP pt live replay: …` in the container and on this AMD host.
- The fail-not-skip target exists, appears in `make help`, and FAILS (not skips)
  when `ASMTEST_REQUIRE_PT=1` finds no PMU.
- `test_dataflow_pt` is in `SUITE_EXCLUDES` (it is a standalone-main suite).
- On a bare-metal Intel PT box (`perf_event_paranoid < 0` or `CAP_PERFMON`): the
  live replay matches the single-step oracle rsp-relative on the deterministic
  region with **zero** single-steps of the target. Until then, the gate is
  recorded in the plan status, not claimed as validated.

### T5 — Analysis-equivalence suite + docs: F5's valtrace is a first-class L0  (S, depends on: T1, T2, T3)

**Goal.** Prove F5's `asmtest_valtrace_t` plugs into the shared def-use (L1) and
slice (L2) analysis **unchanged** — the whole point of filling the shared sink —
and land the suite + user-facing docs.

**Steps.**

1. Create `examples/test_dataflow_pt.c` as the standalone-TAP suite (mirror
   [examples/test_dataflow_blockstep.c](../../../examples/test_dataflow_blockstep.c)'s
   structure: re-declare the F5 entry points + `asmtest_dataflow_pt_info_t` + the
   `DF_PT_*` codes, and **assert the layout guard** — call
   `asmtest_dataflow_pt_info_layout(&size, &last_off)` and check both against the
   suite's own `sizeof`/`offsetof`, the F6-hazard defence). Register the T1/T2/T3
   cases here.
2. Add the analysis-equivalence case `test_pt_defuse_slice_equiv`: over the
   ROUTINE region, build the def-use graph (`asmtest_defuse_build`) and a backward
   slice (`asmtest_slice_backward`) from the `ret`'s result over **both** the F5
   replay trace and the emulator oracle trace, and assert the edge sets and slice
   step-sets are identical. This proves F5 is a drop-in L0 producer, not a special
   case the analysis must know about.
3. Add the test link + object rules to [mk/dataflow.mk](../../../mk/dataflow.mk),
   mirroring the blockstep rules ([:420-441](../../../mk/dataflow.mk#L420)): the
   `$(BUILD)/test_dataflow_pt.o` compile knobs (Capstone + Unicorn cflags + libipt
   cflags where present), the explicit link rule (F5 object + `dataflow.o` +
   `dataflow_operands.o` + `dataflow_blockstep.o` + `pt_backend.o` + `codeimage.o`
   + libs), and the `dataflow-pt-test` lane gated on `DF_HAVE_UNICORN`. Chain it
   into the `dataflow-test` aggregate as a sub-make, exactly as
   `dataflow-blockstep-test` is chained ([:390](../../../mk/dataflow.mk#L390)) —
   one gate, in one place.
4. `make fmt`, `make docker-dataflow-pt`, and `make dataflow-test` (the aggregate,
   where Unicorn is present).

**Code.** No production code — this task is the suite + the make wiring + docs.
The equivalence assertion is the acceptance bar for "F5 fills the SAME
`asmtest_valtrace_t`."

**Tests.** The suite is the deliverable. A pass is `# N passed, 0 failed` with the
live case skipped off silicon; a failure is a `not ok` TAP line. The
equivalence case fails loudly if F5's records diverge from the emulator's in shape
(not just value), which would mean the shared analysis cannot treat it as an L0.

**Docs.** Add the `dataflow-pt-test` / `docker-dataflow-pt` targets to `make help`
and **create a new data-flow tier section** in the user-facing tracing guide
([docs/guides/tracing/native-tracing.md](../../guides/tracing/native-tracing.md)) —
the guide currently documents only the DynamoRIO and Unicorn control-flow tiers
and has **no** data-flow/valtrace/def-use section yet, so this task adds it:
F5 is the out-of-band, zero-single-step ceiling, silicon-gated for live capture
and CI-validated for the decode→replay bridge. Land the CHANGELOG `### Added`
bullet (from T2) here once the aggregate is green.

**Done when.**
- `make dataflow-test` runs `dataflow-pt-test` (via the aggregate sub-make) green,
  with the live case skipped.
- `test_pt_defuse_slice_equiv` passes: F5's def-use edges and backward slice equal
  the emulator oracle's over the ROUTINE.
- The layout-guard assertion passes (size **and** last-field offset match).
- `make help` lists the new targets; the tier guide documents F5.

## Task order & parallelism

- **Critical path:** T1 → T2 → T3 → T4 (live validation exercises everything).
  T5 (the suite + docs) is written alongside T1–T3 and green before T4's live run.
- **T1** is fully independent (Unicorn-only, hand-fed path) and can start
  immediately.
- **T3** depends only on T1 (the gate wraps the replay entry) and can proceed in
  parallel with T2 by a second person.
- **T4** additionally depends on
  [intel-pt-attach-foreign-pid.md#T1](intel-pt-attach-foreign-pid.md) (the
  foreign-pid AUX capture context) and
  [#T2](intel-pt-attach-foreign-pid.md) (the paired live code-image recorder)
  landing; its **silicon run** is hardware-gated and may land as a
  self-skipping test well before a bare-metal Intel box exists.

```
T1 ──> T2 ──> T3 ──┐
  └──> T3          ├──> T4 (live run: Intel-PT-gated + foreign-pid capture)
  └──────────────> T5 (suite + docs; green before T4's run)
```

## Constraints & gates

- **Hardware gate (real, self-skip allowed):** live PT capture needs bare-metal
  Intel silicon with the `intel_pt` PMU and `perf_event_paranoid < 0` /
  `CAP_PERFMON`. The dev boxes are AMD (Zen 5 / Zen 2); VMs, Docker, and
  GitHub-hosted runners hide the PMU. Every live path self-skips with a specific
  reason; the fail-not-skip target converts the skip to a failure only where PT is
  claimed. This is the **only** legitimate self-skip in the doc.
- **No parallel PT arm (position 9):** F5 opens no `perf_event_open`. Capture is
  owned by [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md) over
  the shared helpers from
  [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md).
- **Dependency added, not skipped (CLAUDE.md):** the CI decode→replay bridge needs
  libipt **and** Unicorn in one image; T2 adds `libipt-dev` (Ubuntu noble
  2.0.6-1build1 = libipt v2.0.6) to the Unicorn-bearing dataflow image as an
  apt-pinned distro package — the same form
  [Dockerfile.hwtrace](../../../Dockerfile.hwtrace) uses; apt distro packages need
  no `scripts/third-party-digests.txt` line.
- **No single-step fallback:** unlike the block-step tier, F5 is fully out-of-band,
  so an impure/non-replayable/nondeterministic region **truncates honestly** (T3).
  A `truncated == false` where a real divergence occurred would be a correctness
  regression, not an acceptable degradation.
- **License:** libipt is BSD; no new obligations. Unicorn/Capstone are already this
  tier's dependencies.
- **When the gate blocks validation, record:** the task landed
  wiring-complete-but-unvalidated (T4), the exact command to run on silicon, and do
  **not** update the plan status to "validated" until it has run
  (verify-before-declaring-done).

## Research notes (verified 2026-07-17)

This tier consumes a captured AUX blob and decodes it; the capture-side perf/AUX
mechanics are owned by the sibling PT docs. The facts F5's **decode + replay**
depend on:

- **libipt pin.** The decode uses libipt v2.0.6 (Ubuntu noble `libipt-dev`
  2.0.6-1build1) — <https://packages.ubuntu.com/noble/libipt-dev>. The same pin
  [Dockerfile.hwtrace](../../../Dockerfile.hwtrace) already carries.
- **The decode is CI-testable without PT hardware.** `asmtest_pt_encode_fixture`
  ([pt_backend.c:305](../../../src/pt_backend.c#L305)) builds a valid PT AUX stream
  with libipt's own **packet encoder** (userspace, no `intel_pt` PMU), and
  `asmtest_pt_decode_window` drives the real libipt instruction decoder over it.
  F5's bridge test reuses this to exercise decode → replay end-to-end in CI — the
  same posture as `test_wholewindow_decode`. What stays hardware-gated is real PT
  **capture** (owned by the sibling doc), not decode or replay.
- **libipt v2.0.6 `pt_insn` decode loop** (verified against the v2.0.6 tag header
  <https://raw.githubusercontent.com/intel/libipt/v2.0.6/libipt/include/intel-pt.h.in>;
  workflow <https://raw.githubusercontent.com/intel/libipt/master/doc/howto_libipt.md>):
  the decode `pt_backend.c` already implements — zero `pt_config`, `begin/end` =
  the linearized AUX snapshot (de-wrap before decode), `pt_insn_sync_forward` →
  `-pte_eos` when no PSB remains, `pt_insn_next` with pending events drained via
  `pt_insn_event` on `pts_event_pending` (`drain_events`,
  [pt_backend.c:131](../../../src/pt_backend.c#L131)), `-pte_nomap` = IP outside
  the image. F5 adds no libipt code — it calls `asmtest_pt_decode_window` and
  works on its output. The image is served via `pt_image_set_callback` backed by
  the code-image recorder (temporal-correct bytes), the tree's chosen form.
- **The AUX blob must be a linearized snapshot before decode.** `aux_head`/
  `aux_tail` follow the same semantics/ordering as `data_{head,tail}`; the consumer
  copies `[aux_tail, aux_head)` modulo `aux_size` (two memcpys across a wrap) into
  a contiguous buffer before handing it to libipt — <https://man7.org/linux/man-pages/man2/perf_event_open.2.html>,
  <https://raw.githubusercontent.com/torvalds/linux/v6.8/include/uapi/linux/perf_event.h>.
  The **de-wrap is the capture side's job** (the sibling doc); F5 receives an
  already-linearized blob.
- **Truncation signal.** `PERF_RECORD_AUX` (=11) carries `PERF_AUX_FLAG_TRUNCATED`
  (0x01) when the CPU dropped trace (perf_event.h:1261-1264). A truncated capture
  → an incomplete path → F5 sets `vt->truncated`. F5 reads the flag from the
  capture side's result, not from raw perf records.
- **Unicorn cannot execute AVX, and mis-executes VEX-128 silently.** No released
  Unicorn (through 2.1.3, built and probed 2026-07-17) runs a VEX/EVEX-encoded
  instruction — it still vendors QEMU 5.0.1, and QEMU gained AVX TCG only in 7.2;
  VEX-128 returns `UC_ERR_OK` with a **wrong** answer
  ([dataflow_blockstep.c:159-200](../../../src/dataflow_blockstep.c#L159)). Hence
  T3's replayability gate is an encoding-level rule and a **correctness** gate, and
  F5 truncates AVX regions rather than replaying them. This is an upstream
  capability gap, not an installable dependency (the CLAUDE.md legitimate-stop
  case).

## Out of scope

- **Foreign-pid PT capture** (opening the event on `pid > 0`, live AUX draining,
  `aux_watermark` tuning, `CAP_PERFMON`, de-wrapping the AUX ring):
  [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md) — F5 consumes
  its output.
- **The self-trace PT window arm, the WEAK/STRONG/CEILING ladder, and the shared
  `pt_aux_open`/decode helpers:**
  [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md).
- **The block-step + record-and-inject value tier (F1/F2)** whose purity/
  replayability verdicts F5 reuses: it lives in
  [src/dataflow_blockstep.c](../../../src/dataflow_blockstep.c); F5 links its
  verdicts, it does not re-scope them.
- **Provisioning/gating the bare-metal Intel runner:**
  [self-hosted-ci-runners.md](self-hosted-ci-runners.md).
- **ARM CoreSight live decode** (the other hardware trace source):
  [coresight-live-decode.md](coresight-live-decode.md).
- **Whole-process / continuous data flow** (F6, declined with a measured number)
  and the DynamoRIO taint tier hand-off: F5 stays scoped/windowed, out-of-band,
  and observe-don't-instrument.
