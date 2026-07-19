# Implementation documents — index

This directory holds **implementation-ready specifications** for the work that
remains open across the project's active plans and analysis notes. Each file is
a self-contained brief for **one coherent task set**: a junior developer can
clone the repo, open exactly one document, and implement it end to end (code +
tests + docs) with no other context.

> **Provenance.** These 31 documents were generated on 2026-07-18 by extracting
> every open item from the 12 active plans in [`../plans/`](../plans/) and the
> analysis notes in [`../analysis/`](../analysis/), verifying each item's status
> against the working tree (git history + file/symbol presence) rather than the
> source doc's own claim, then grouping the survivors by workstream. 225 tasks
> across 31 docs. Items the plans marked open but the repo showed already landed
> were dropped; contradictions between sources were resolved once and recorded.

## How to use these

1. Pick one document (the table below groups them by area and flags
   dependencies). Read it top to bottom — every task carries **Goal / Steps /
   Code / Tests / Docs / Done-when**, exact repo paths, the existing pattern to
   mirror, and the commands that prove each step.
2. Read [`_conventions.md`](_conventions.md) once — it states the repo-wide
   rules every doc relies on (build/test entry points, the version-pinning
   dependency rule, changelog/docs conventions) so the individual docs don't
   repeat them.
3. If a claim in a doc disagrees with the code, **re-verify before
   implementing** — the tree moves; the docs were verified on 2026-07-18.

## Binding cross-document positions

The source plans contradicted each other (and occasionally the code) in 12
places — e.g. whether Zen 3 can open AMD branch records (it cannot in this tree;
the floor is Zen 4), whether the `trace_call_auto` completeness bug is fixed (it
is), which macOS CI image is current (`macos-15-intel`, not the retired
`macos-13`). Every one of those is resolved once in
[`_positions.md`](_positions.md), and every document here already conforms to
it. Read it before editing any doc so a change doesn't reintroduce a refuted
claim.

## The documents

Grouped by workstream. **Depends on** lists only *hard* build/code dependencies
(a doc that cannot start until another lands); soft "coordinate with" couplings
are noted inside each doc's *Task order & parallelism* section, not here.

**Status** tracks task completion as `done/total`; update the cell as tasks
land. Legend: ☐ not started (`0/N`) · ◐ in progress · ☑ complete (`N/N`, code
landed) · ✅ verified (`N/N`, exercised end-to-end on the target — not just unit
tests; see the repo's verify-before-done rule). A doc reaches ✅ only once its
hardware/credential-gated legs have actually run, or are recorded as gated.

## Working process for implementing agents

Implementation and validation are split across agents. If you are **implementing**
a doc, follow this loop so the status table always reflects reality and every step
is on `main`:

1. **Mark in progress, then push.** Before writing any code for a task, set the
   doc's Status cell to `◐ done/total (T<n> wip)` in the table below, commit that
   change alone (`docs(implementations): mark <doc> T<n> in progress`), and push to
   `main`. This claims the task so a second agent does not double-work it.
2. **Implement.** Do the task end to end — code + tests + docs + `CHANGELOG.md` —
   per the doc's Steps. Format through the pinned path (`make docker-fmt` or
   `make fmt CLANG_FORMAT=clang-format-18`).
3. **Mark complete, then push.** Bump the Status cell (`◐ done+1/total`, or `☑ N/N`
   when the doc's last non-gated task lands), and commit the implementation together
   with the status bump (`<type>(<area>): <task summary> (<doc> T<n>)`). Push to
   `main`.
4. **Hardware/credential-gated legs.** Where a task's *validation* needs silicon or
   credentials this host lacks (e.g. Zen 3 BRS, a bare-metal PT box, publish creds),
   implement everything implementable behind the gate, mark the task complete with a
   `(gated: <reason>)` note in the Status cell, and record the exact gate in the
   commit message. **Validation of these legs is a separate agent's job** — do not
   silently self-skip a leg that this host *can* run.

`✅ verified` is set by the **validating** agent, not the implementer: it is
reached only after the doc's live legs actually run green on the target hardware.

Everything below started ☐ not started — these are freshly authored specs.

### AMD hardware tracing
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [amd-ibs-backend-honesty.md](amd-ibs-backend-honesty.md) — IBS backend honesty, record sizing, ABI guards, validation gate | 7 | ☑ 7/7 (T1 verified live) | — |
| [amd-branchsnap-lbr-docs.md](amd-branchsnap-lbr-docs.md) — branchsnap depth fix, LBR tiling validation, freeze-probe cleanup, Zen 3 BRS story | 8 | ◐ 7/8 (T1/T4/T5/T6 landed; T2/T3/T7 code landed, live Zen 5 BPF validation gated; **T8 BLOCKED**: needs Zen 3 Family 19h silicon — arm must not merge untested per CLAUDE.md) | — (soft: shares the Zen 3 story with the sibling above) |
| [ptrace-blockstep-tracer-correctness.md](ptrace-blockstep-tracer-correctness.md) — int3 si_code, rep-prefix, SP-aware step-over, IBS pre-cover | 8 | ☑ 8/8 (T1 [all 3 drivers], T2, T3, T4, T5, T6 landed & verified: `make docker-hwtrace` 436/436 (+4 for T4's re-entrant call-out differential — confirmed red, with the correct-invocation result flipping to the inner one, when the depth check is reverted), `docker-hwtrace-jit-dotnet-bcl`/`-jit-java` green on real CoreCLR/HotSpot call-out step-overs, `make docker-docs` clean; T7 code landed: `asmtest_bs_precover_build`/`_free` (`include/asmtest_blockstep_internal.h`) memoizes `blockstep_reconstruct`'s decode; **correction to the prior gate note**: on this session's host/lane BTF is *not* masked — `make docker-hwtrace-privileged` ran T7's LOOP_X86 differential + hostile-leader legs live (not skipped): `probe_calls 81 -> 5 (hits 19)`, all green; T8 done & live-verified: `ASMTEST_TRACE_IBS_PRECOVER` (0x4) wired into `asmtest_trace_call_auto`'s block-step rung (`src/trace_auto.c`, new `build_ibs_precover`) — forks a bounded ~30ms warm-up child, surveys it out-of-band via `asmtest_ibs_survey_process`, builds+installs a T7 precover table around the one block-step call, any survey/build failure degrading silently to the plain rung; `make docker-hwtrace` 550/550 (bit-as-no-op path: on this AMD host, which lacks LbrExtV2, `asmtest_trace_call_auto`'s rung 1 always resolves to the in-process SINGLESTEP HWTRACE backend before the cascade ever reaches block-step, so the with/without-bit differential correctly asserts byte-identical output rather than a shrink — a structural cascade fact, not a T8 defect); `make docker-hwtrace-privileged` (CAP_PERFMON) 561/561 with the live mechanism actually engaged (not a no-op): a direct live-IBS-survey-primed block-step differential shows `probe_calls 101 -> 0 (hits=25, leaders=2)`; both counts reproduced natively on this Zen 2 host outside Docker too; `make check` clean) | — |

### Data-flow tier
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [dataflow-producer-correctness.md](dataflow-producer-correctness.md) — gap barrier, sub-register aliases, undefined flags, F2 record-and-inject | 8 | ☑ 8/8 (T1, T2, T3, T4 done & live-verified; T3 on the Zen 5 box — `make docker-dataflow-attach` 498/498, `dataflow-blockstep-test` 180/180, all 0 skips; adversarially reviewed — one confirmed MAJOR finding (32-bit GP writes weren't modeled as zero-extending the full 64-bit container, fabricating a stale-writer edge for the upper half) fixed with a teeth-checked regression test before landing; T5 done & live-verified on the Zen 5 box — DFB_IMP_HWREC split out of DFB_IMP_OTHER, `region_scan` reports `hwrec_off[4]`/`nhwrec`/`hwrec_overflow` (capped at the 4-slot architectural DR count), new local `dfb_arm_hw_bp`/`dfb_clear_hw_bps` DR0-3 plumbing (mirrors ptrace_backend.c/asmspy_engine.c without widening the backend's single-slot API), `capture_blockstep` arms one slot per scanned site and absorbs a DR6 hit with one single-step to snapshot the real post-retirement boundary (`info.hw_hits`); forward-pass-only per the doc — `step_block` still refuses to inject (T6's job), so verdicts are byte-for-byte unchanged (probed `cs_regs_access` first: Capstone 5.0.1 already reports the complete write set for all 5 hwrec mnemonics, so no producer-local supplement was needed, unlike `syscall`); `make dataflow-blockstep-test` 185/185 (was 180/180, +5: 2 scan-level + 3 live forward-pass, including a forced-replay case proving a real DR hit with `hw_hits=1` while the verdict still truncates exactly as before, and the force_singlestep=1 Done-when check `hw_hits==0`); `make docker-dataflow-attach` 506/506 across all 8 suites, 0 skips; T8 done — the named upstream sentinel (`run_avx_tcg_sentinel_case`, near the `uc_vec_width` probes) stands up two raw `uc_engine`s and asserts VEX-256 `vaddps ymm` returns `UC_ERR_INSN_INVALID` and VEX-128 `vpaddd xmm0,xmm1,xmm2` still drops VEX.vvvv (xmm0 comes back old-xmm0+xmm2, not xmm1+xmm2); trigger confirmed NOT met (Unicorn 2.1.4, qemu/VERSION 5.0.1); doc pointer grepped in both the suite and the `insn_is_vex_evex` gate comment; no pin/gate/seeding change shipped; `make dataflow-blockstep-test` 186/186 (+1), `make docker-dataflow-attach` 507/507 across all 8 suites, 0 skips; T6 done & live-verified on this Zen 2 host — `step_block`'s DFB_IMP_HWREC arm now injects the boundary's recorded write set (generic over `c->cur`'s Capstone-reported records via a new `uc_gp_container` Capstone-id→Unicorn-container map, mirroring `gp_value`'s read-side grouping — no per-mnemonic table needed) and terminates the block there, exactly like `syscall`/`int 0x80`; `region_scan`'s `injectable` widened to admit HWREC (subject to the existing 4-slot DR0-3 cap — a 5th+ site sets `hwrec_overflow` and names the reason `hwrec-overflow`); new opts hook `no_hw_record` skips arming, reproducing the pre-T6 fail-closed truncation on demand; `blind_rdtsc` now replays with real injected values (its rax/rdx overwrite no longer matters) and stays the witness for the per-step decode via `no_hw_record` rather than `force_replay` (naturally injectable now); `sc_then_cpuid`→`sc_then_sysenter` and `imp_vec`/`imp_cpuid`'s two single-step-fallback assertions were updated (`force_singlestep` pins the fallback path; is_pure() separately confirmed unchanged) since cpuid alone is now injectable, not merely gated; new live fixtures `hwrec_cpuid` (byte-identical vs oracle), `hwrec_rdtsc2` (two-site monotonicity, same-capture independent-oracle value check), `hwrec_rdrand_jc` (CF injection proven via same-capture self-consistency — not vec_compare, since rdrand draws a genuinely random value per forked capture), `hwrec_coldpath` (per-block claim: an unreached site costs nothing, `hw_hits==0 injected==0` yet `pure==1`), and a live run of `hwrec_5site` (overflow fallback, reason=`hwrec-overflow`); `make dataflow-blockstep-test` 191/191 (was 186/186, +5), stable across 5 consecutive runs; T7 done & live-verified on this Zen 2 host — new `asmtest_blockstep_extent_t` (`{off,len}`, blob-absolute) + opts `extents`/`nextents` (NULL/0 = today's whole-region sweep, unchanged); `region_scan` split into a per-extent inner sweep (`region_scan_extent`, one `cs_insn` allocation shared across all extents) whose verdicts aggregate across extents, called once per extent instead of once over the whole buffer — bytes outside every extent are never fetched, so an embedded island between two extents costs nothing; `run()` validates extents sorted/non-overlapping/inside `[region_off, code_len)` before any tracee spawns (`DF_BLOCKSTEP_EINVAL` otherwise) and converts blob-absolute to region-relative in a scratch array scoped to one call; the public `is_pure`/`is_replayable`/`is_injectable` classifiers stay whole-blob by design (extents are a `run()`-only capability, documented as such); new fixture `island_sse` (the existing `island` fixture's exact byte shape, with legacy-SSE `paddq` swapping in for `island`'s VEX-128 `vpaddq` — same 4-byte length, so all offsets are unchanged — since a genuine VEX-128 stays gated by the encoding rule regardless of extents and can never demonstrate T7's OWN claim) proves the positive case: WITHOUT extents it desyncs exactly like `island` (fail-closed, reason="decode", the negative control); WITH extents hopping the island's 2 data bytes it is byte-identical to the single-step oracle with stops cut 5→2; `make dataflow-blockstep-test` 199/199 (was 191/191, +8), stable across 5 consecutive runs; `make docker-dataflow-attach` 520/520 across all 8 suites, 0 skips; `make check` 54/54; `make docker-fmt-check` clean on both touched files; `make docker-docs` clean) | — |
| [dataflow-bindings-slice-codeimage.md](dataflow-bindings-slice-codeimage.md) — def-use/slice surface + code-image arg across bindings | 4 | ✅ 4/4 (T1, T2, T3: T2 wraps `defuse`/`forward_slice`/`backward_slice` (by-pointer seed) + the record-append surface in all seven remaining bindings — Ruby (Fiddle pack), Lua (LuaJIT ffi struct), Zig/Rust (extern/repr(C) struct), Go (cgo dlsym), Java (FFM MemorySegment), .NET (raw IntPtr, sidesteps the non-blittable-bool problem); T3 adds the counted TAP assertions over that surface to all seven `test_dataflow.<lang>` suites — a hand-built r10→r11→r12 register chain (`forward_slice(0)`/`backward_slice(2)` both `{0,1,2}`, register-only so it runs even where live-attach self-skips) and, over the shared live `df_chain` capture, the **memory** def-use edge (step1 store → step2 load) these seven could never slice before T1's by-pointer seed: `forward_slice(0)` and `backward_slice(4)` both `{0,1,2,3,4}`, excluding the trailing `ret`; anti-vacuity checked by temporarily inverting `put_mem`'s `is_write` in `src/dataflow_operands.c` (scratch build, reverted before commit, `git diff` clean) — the register-only chain checks stayed `ok` while both memory-edge checks went `not ok` (`forward_slice(0)` collapsed to `{0,1}`, `backward_slice(4)` to `{2,3,4}`), then confirmed `ok` again after rebuilding the revert; all seven `docker-dataflow-<lang>` lanes green at their new 40/40 (was 36/36), 0 skips, 0 failures (Ruby/Go/Rust/.NET cross-checked locally too); T4 done & live-verified — **widened to all ten bindings** (T4's own text names python/dataflow.py and node/dataflow.js alongside the seven, unlike T1-T3's seven-binding-scoped slice surface): a `CodeImage` wrapper (`asmtest_codeimage_new`/`track`/`now`/`bytes_at`/`free`) and `ValueTrace.attach_pid_versioned` land in Python, C++ (RAII), Node (mirrors the existing hwtrace-binding class), Ruby, Lua, Zig, Rust, Go, Java, and .NET; `attach_jit` no longer unconditionally passes NULL/null/nil for the versioned-decode `img` (C++ already threaded it structurally — this adds the first real-img test); every binding's live test tracks a recorder over a real victim's published region and decodes an `attach_pid_versioned` capture through it (result/step-count tied to that run's own args); all ten `docker-dataflow-<lang>` lanes green with the new assertions (`libasmtest_dataflow` self-test + live-img-thread checks), 0 skips — the container's soft-dirty page tracking was live (not a no-op `# SKIP`) in every lane, so this is the software tier's actual codeimage path, not a gated stand-in; no hardware/credential gate applies to this doc) | — |
| [dataflow-f4-object-identity.md](dataflow-f4-object-identity.md) — real object identity via GCBulkType/Node/Edge | 6 | ☑ 6/6 (T1: `asmtest_objid_*` + `test_dataflow_objid` 27/27; T2: post-capture `GCCANON_MOVE_POST` feed, `docker-gccanon-attach` 37/37 with `post_moves`>0 every phase; T3: EventPipe GCBulkNode/Edge/Type `gccanon_dumper`, live run nodes=15660 index_gaps=0 reason=Induced, `System.Int64[]` joined; T4: snapshot-space spike GO=POST (`GCBulkNode.Address==NewRangeBase`; dump GC's 15001 ranges all post-witness, 0 pre) + findings note; T5: alias fixture (LOH-compaction slide + accumulate-no-reuse so the traced object reaches the post-detach snapshot) + heap-snapshot objid join lane, `docker-gccanon-attach` green x2 — selftest 1..5, numeric 1..10/1..11/1..11 (37 landed, unchanged), alias 1..6 (false store/load edge FORGED by address identity, SEVERED by object identity, no collateral); numeric-phase objid calibration scoped out (SOH churn vs its 37 assertions — alias phase carries the live objid proof, T1 unit proves preservation); T6: retired the `likewise deferred` comment (points at the increment-4 block), CHANGELOG `[Unreleased]` Added entry, F4 plan UPDATE block + snapshot-space findings cited, `make docker-docs` clean, `make dataflow-test` 209 green) | — |
| [dataflow-pt-replay-tier.md](dataflow-pt-replay-tier.md) — F5: PT + code-image + Unicorn-replay value tier | 5 | ☐ 0/5 | **intel-pt-attach-foreign-pid** |

### Intel PT & CoreSight hardware trace
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md) — PT whole-window capture substrate, STRONG ladder, inline ctor | 5 | ☑ 5/5 (T1–T5 landed & Docker-verified on this AMD host: `make docker-hwtrace` 556/556 (T1 pt_aux_* one-arm refactor + pt_begin/end_window pair; T2 begin_window PT arm + `base_ip_out`; T3 WEAK/STRONG ladder + runtime decode-trust probe, resolves to WEAK here, `pt_window_trusted`==`codeimage_available` in the libipt image), `hwtrace-dotnet-test` 191/191 (T4 `new AsmTrace(HwBackend.IntelPt)` inline ctor: `PtWindowCtx`/`Kind.PtWindow`, self-skip names the PT gate, leaked-scope finalizer clean), `docker-fmt-check` + `docker-docs` clean; T5 `test_pt_live_selfjit` + `asmtest_hwtrace_pt_set_filter` + `make hwtrace-pt-live` landed & self-skipping — `hwtrace-pt-live` **fails-on-no-PMU verified in-container** (`not ok`, Error 1). **LIVE PT capture GATED: bare-metal Intel PT + `perf_event_paranoid<0`/`CAP_PERFMON`** (no reachable box exposes `intel_pt`); a validating agent runs `make hwtrace-pt-live` + `make hwtrace-dotnet-test` on a PT box to reach ✅) | — |
| [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md) — PT attach-to-foreign-PID capture, facade dispatch, HV/EPT frontier | 5 | ☐ 0/5 | **intel-pt-whole-window-substrate** |
| [coresight-live-decode.md](coresight-live-decode.md) — CoreSight live OpenCSD decode tree (AArch64 board-gated) | 5 | ☐ 0/5 | — |

### Intel Pin / SDE oracle lanes
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [pin-sde-future-isa-lane.md](pin-sde-future-isa-lane.md) — SDE future/absent-ISA test lane | 8 | ☐ 0/8 | — |
| [pin-xed-trace-tier.md](pin-xed-trace-tier.md) — XED-decoded Pin trace tier + shared pintool substrate | 9 | ☑ 9/9 (T1 done: `scripts/fetch-pin.sh` fetches/verifies/caches the pinned Pin 4.2-99776-g21d818fa2 gcc-linux kit + vendors its license, digest recomputed twice, tamper-gate proven; T2, T3 landed: `Dockerfile.pintool` (in-image `fetch-pin.sh` digest gate + DynamoRIO for the T7 arm) + `docker-pintool`; `mk/pintool.mk` (x86-64/Linux arch gate mirroring `mk/cli.mk`), `pintool/{makefile,makefile.rules}` out-of-kit stub derived from the kit's MIT-SPDX scaffolding, null `asmtest_pintool.cpp` — built locally (`make pintool-tool`), loads + runs `/bin/true` rc=0, arch gate prints `1..0 # skipped`; **T4, T5, T6 landed & locally verified on this x86-64 host** against the pinned 4.2 kit — `pintool/pintool_shm.h` fixed offset-only channel + `examples/pin_trace_workload.c` (exports `asmtest_trace_begin/_end`, W^X ROUTINE, `result=7109`, `nm -D` marker present, native run leaves `insns_total=0`, `include/` clean); the real `asmtest_pintool.cpp` (RTN_FindByName marker gating, runtime block-head derivation, append/dedup/truncate discipline) + `examples/pin_trace_validator.c` + `pintool-test` → `make pintool-test` **9/9 ok** incl. `pin/single-step insn+block offsets byte-identical`, insns `[0,3,6,c,11,0,3,6,c,e,11]`, blocks `[0,0x11,0xe]`, totals 11/3/4; tool proven additive (plain `pin --` ⇒ zero counters); files clang-format-18-clean; **T7 landed & verified** — `make docker-pintool` **11/11 ok** incl. the full three-way parity `pin/single-step insn+block offsets byte-identical` AND `pin/DynamoRIO insn+block offsets byte-identical` (PIN-2's exit criterion), with the in-image `fetch-pin: verified` digest gate firing. **Doc correction:** `Dockerfile.pintool` builds Capstone from pinned source (the doc said omit it) — `asmtest_hwtrace_available(SINGLESTEP)` gates on the disassembler for block normalization (`src/hwtrace.c` decoder_present), so without Capstone the single-step parity arm could only self-skip in-container; per CLAUDE.md the dependency is added where the work runs. **T8 landed** — `pintool/pin_apx_fixture.h` hand-encoded REX2/EGPR (`mov/add r16,rsi; ret`) APX routine + `examples/pin_apx_decode.c` (ungated XED decode assertion), `pin_apx_workload.c`, `pin_apx_validator.c` (positive complete-trace + fork-isolated DR negative control), `pintool-apx-test` appended to `pintool-test`. **Doc correction:** the 4.2 kit tags a REX2-promoted legacy MOV/ADD as iclass MOV/ADD, ext BASE — not the `XED_EXTENSION_APX*` enums the doc expected — so the "genuinely APX" proof uses XED's purpose-built `xed_classify_apx()` (kit-verified: 3/4 insns APX). `make docker-pintool` runs the ungated decode assertion green (2/2 ok) on the non-APX CI host; **APX-silicon execution halves (Pin complete-trace + DR #6226 decoder-error negative control) GATED: no reachable APX-F CPU** — a real hardware gate (running r16/r17 code is a #UD), self-skips with the printed reason. **T9 landed** — CHANGELOG `[Unreleased]` Pin-lane entry, both stale "VNNI still breaks" claims corrected (analysis + plan docs, and a pre-existing CHANGELOG occurrence) to cite DR #5440 closed 2022-04-25 (PR #5444) with PIN-2's case resting on APX #6226 alone; `git grep "VNNI still break"` now hits only the spec meta-refs; `make docker-docs` clean. **Full lane green: `make docker-pintool` 11/11 parity + 2/2 apx decode.**) | — |
| [pin-probe-mode-capture.md](pin-probe-mode-capture.md) — Pin probe-mode argument/return capture | 7 | ◐ 4/7 (T4 pointed-to buffer capture: `-ptrcap` cap (default 4096) + page-clamp + mapped-range validation — valid `buf` captured 4027B page-clamped, `buf=0x1` REFUSED (sz=0) with victim surviving rc=0. **Doc correction:** `PIN_SafeCopy` is JIT-mode-only in this kit, so validation is done against `/proc/self/maps` — which in probe mode IS the target's map — the design note's actual requirement) | **pin-xed-trace-tier** |
| [pin-libdft-taint-oracle.md](pin-libdft-taint-oracle.md) — libdft64 differential oracle for the DR taint tier | 7 | ☐ 0/7 | **pin-xed-trace-tier** |

### asmspy CLI
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [asmspy-cli-enhancements.md](asmspy-cli-enhancements.md) — TUI hot-edge drill-in, syscall-arg content decode, coverage gaps | 9 | ☑ 9/9 (all landed, `make docker-cli` cli-smoke PASS; T1-T4 sockaddr/ioctl/fcntl/futex/stat decode, T6-T9 group-stop/negative-n/aggregation/breadth smokes all CI-green. T5's live mode-7→Enter→mode-9 drill-in is pty-driven + AMD-IBS-gated so its interactive leg is manual-only — its DECISION logic (`asmspy_edge_drill`, 6 checks) is unit-tested in `test_autoregion`, which this doc also fixed to actually RUN in the smoke (was built-but-never-executed). Not yet ✅: T5's live drill-in needs a manual pty run on an AMD IBS host) | — |
| [asmspy-aarch64-support.md](asmspy-aarch64-support.md) — single-step engine abstraction + NT_ARM_HW_WATCH watchpoints | 7 | ☐ 0/7 | — |

### Single-step & block-step tiers
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [inproc-btf-block-step.md](inproc-btf-block-step.md) — W3: in-process BTF branch-granular single-step | 6 | ☑ 6/6 (`make docker-hwtrace-msr` live-verified by the implementer on this Zen 2 host — both fixtures byte-identical to the single-step baseline, 10/10 stable runs, zero truncation; `make docker-docs` clean) | — |
| [macos-oop-mach-stepper.md](macos-oop-mach-stepper.md) — macOS out-of-process single-step via Mach exception ports | 7 | ☑ 7/7 (`make mach-stepper-test` 25/25 live-verified by the implementer on this host, both breakpoint paths + both self-skip legs; `make docker-docs` clean) | — |
| [aarch64-ptrace-single-step-validation.md](aarch64-ptrace-single-step-validation.md) — AArch64 ptrace stream validation + binding fixtures | 6 | ☐ 0/6 | — |

### Scoped / managed whole-window tracing
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [zeroconfig-scoped-tracing-hardening.md](zeroconfig-scoped-tracing-hardening.md) — in-process guards, hygiene assertions, doc-tail | 9 | ☐ 0/9 | — |
| [managed-wholewindow-compose.md](managed-wholewindow-compose.md) — live compose, safe managed-arm routing, ambient PT stitching | 12 | ☐ 0/12 | **intel-pt-whole-window-substrate** (PT prongs only; D3-stepper prongs are independent) |

### macOS
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [macos-cleanroom-lanes.md](macos-cleanroom-lanes.md) — tart arm64 / Docker-OSX x86 shakedowns + sshpass containerization | 6 | ◐ 2/6 (T4 done: `Dockerfile.sshpass` + `make docker-sshpass` build `asmtest-sshpass` (sshpass 1.09-1, verified against a live noble apt-cache policy check, not just the doc's claim); `scripts/docker-osx-bindings.sh` runs every ssh call through it, no host sshpass/sudo; stdin piping through the shim verified (`echo hello \| docker run --rm -i asmtest-sshpass sh -c 'cat \| wc -c'` -> `6`); off-KVM, `make docker-osx-bindings` builds the sshpass image first then still fails fast at the existing `/dev/kvm` guard (dependency ordering confirmed live). T5 done: `DOCKER_OSX_IMAGE` defaults to `:latest` (`docker manifest inspect` confirms `:latest` resolves and `:ventura` 404s, live); added `DOCKER_OSX_DISK` prebuilt-disk support with the conditional `-v/-e IMAGE_PATH` docker-run args (both branches — disk set vs. unset vs. a nonexistent path — unit-verified in isolation since the KVM guard makes the live path unreachable on this host); one-time-install recipe condensed into the script header; docs/clean-room-testing.md Track D row + notes updated (host col drops sshpass, notes the tag reality + DOCKER_OSX_DISK requirement); `user`/`alpine` no longer promised as shipped-in credentials. Both gated tasks (T1-T3 Apple Silicon, T6 bare-metal KVM) remain untouched — this Intel Mac has neither; `make docker-docs` clean) | — |
| [macos-dynamorio-port.md](macos-dynamorio-port.md) — DynamoRIO native-trace port M0–M2 (gated on upstream DR macOS release) | 11 | ☐ 0/11 | — |

### Architecture ports
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [aarch64-sve-capture.md](aarch64-sve-capture.md) — AArch64 SVE wide-vector capture | 8 | ☑ 8/8 (T1–T7 landed & validated live under qemu-user TCG on this x86-64 host: `svec_t`/`spred_t` + HWCAP_SVE probe (T1), `asm_call_capture_sve` AAPCS64 trampoline + GAS/NASM cross-arch stubs (T2, `nm` shows both symbols on both arches), `ASM_SVCALL_*` self-skip macros + VL-aware `ASSERT_SVEC/SPRED_EQ` (T3), `sve_addd` corpus + VL-agnostic test (T4), manifest pins `svec_t`=256/`spred_t`=32 native+win64 (T5), `docker-sve-sweep` lane (T6), user docs + CHANGELOG (T7). `make docker-test DOCKER_PLATFORM=linux/arm64` runs `simd.sve_adds_doubles_at_any_vl` as a real SVE `ptrue`/`fadd` execution (not a skip) at the default **VL=64 B (512-bit)**, and `make docker-sve-sweep` passes it at **VL 16/48/128/256 B** (VQ 1/3/8/16, incl. the non-power-of-two 48 B) by steering `QEMU_CPU=…,sve-max-vq=N,sve-default-vector-length=-1`; `make test`/`make check`/`make ASM_SYNTAX=nasm test`/`make docker-docs`/`fmt-check` all green. Built the arm64 image with `DEPS_ARGS=--pkgconfig` (the SIMD suite links no optional engine; mirrors the `docker-riscv64` light-TCG-image precedent). **T8 gated: real AArch64+SVE silicon for execution sign-off** (Graviton3/3E/4, NVIDIA Grace, or A64FX-class) — no such host in this environment and qemu TCG is not accepted as the silicon sign-off; a validating agent runs `make test && make check` + `make docker-sve-sweep` on SVE hardware to reach ✅. Source plan Track D + analysis gate line updated to match.) | — |
| [riscv-native-tier.md](riscv-native-tier.md) — native RISC-V (rv64) host tier | 7 | ☑ 7/7 (all landed; rv64 lane green: `make docker-riscv64` under qemu + the `test-riscv64` CI Actions leg, ~3m29s; `make docker-docs`/`make test`/`make check` clean) | — |

### CI, distribution & infrastructure
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [distribution-packaging.md](distribution-packaging.md) — language-registry go-live + system package-manager coverage | 13 | ◐ 7/13 live + T1-T6 credential-gated (system-package stream **T7-T13 COMPLETE & verified**: `make package-source` + 5 specs under `packaging/` [Homebrew/Debian/AUR/vcpkg/Conan], aggregate `make docker-syspkg` GREEN, additive `syspkg` CI job, user docs + submission runbook, `make docker-docs` clean. Registry go-live scaffolding landed where implementable: **T1** all 5 names verified free (404, no collision → manifests unchanged); **T4** OIDC trusted-publishing in `release.yml` — dedicated PyPI `pypi-publish` job (`pypa/gh-action-pypi-publish`, collects every leg's wheel), crates.io `rust-lang/crates-io-auth-action`, npm `--provenance`, job-scoped `id-token: write` (YAML-validated, tag-gated so dispatch-safe); **T5** manylinux_2_28 floor recorded (container build gated on CI dispatch); **T6** `bindings/java/pom.xml` Central metadata + dormant source/javadoc/gpg/central-publishing plugins. **T2/T3** no-code. GATE: PyPI/npm/crates.io/RubyGems/NuGet/Maven-Central/LuaRocks accounts+tokens, registry trusted-publisher registration, the `v1.1.0` tag push, and a release dispatch — all credentials this host lacks; do not publish. Validation of the gated legs is a separate credentialed agent's job.) | — |
| [benchmarks-ci-followups.md](benchmarks-ci-followups.md) — windows/macOS-Intel legs, nightly auto-commit, BM_MODEL_COST | 6 | ◐ 2/6 (T5, T6 landed & verified on this Intel-mac host. T5: `asmtest_disas_class` {OTHER/MEM/BRANCH/MULDIV} classifier (BRANCH>MULDIV>MEM>OTHER precedence; MULDIV a model-grade mnemonic match; MEM a per-arch detail walk incl. RISC-V behind the Capstone-5 API macro), `make emu-test` 55/55 native (capstone 5.0.9) + no-Capstone `docker-test` all-OTHER degrade both green, my files clang-format-18-clean. T6: `emu-bench` records insn offsets (cap 4096) and emits a `model_cost` sibling per case (weights OTHER1/MEM3/BRANCH2/MULDIV8) — hand-verified add3 x86_64/win64=5, arm64/riscv64/arm32=4, sum_to_10=43, sum_to_100=403; `bench-golden-check` filters `model_cost` from the golden compare + a new anti-vacuity check (fresh has model rows ⇒ every insns row needs a sibling — force-fail rc=1 confirmed, orphan named); `bench-compare` renders a separate `## Model cost (a MODEL, not silicon)` matrix, never mixed with counts/cycles; **also filtered the golden WRITE in bench-report.sh** (beyond the doc's file list) so a future `make bench-record` can't pollute `golden/emu-insns.json` with Capstone-version-dependent rows; CI `benchmarks` job gains the K1 pinned-Capstone build (the `benchmarks-macos-x86` half lands with T1). Verified: `make bench-check` OK(7), `make emu-bench`/`bench-report`/`bench-compare` all green with the hand values, `make docker-bench` **no-Capstone path** emits 0 model rows + all gates pass, `make docker-docs` clean, ci.yml YAML-valid. **Doc correction:** T6's "`docker-bench` carries model rows (the CI image has Capstone)" is inaccurate — `asmtest-ci` has NO Capstone, so `docker-bench` is the no-Capstone path; the real GitHub-Actions `benchmarks` job carries model rows via the added K1 step. T2 wip) | — |
| [self-hosted-ci-runners.md](self-hosted-ci-runners.md) — self-hosted runner lanes for hardware-gated tiers | 6 | ☐ 0/6 | **macos-cleanroom-lanes** (soft coordinate: amd-ibs, coresight, substrate) |
| [libfuzzer-afl-shim.md](libfuzzer-afl-shim.md) — libFuzzer/AFL harness shim (demand-gated) | 5 | ◐ 4/5 (T1 emu_cover_hits seam + T2 libFuzzer + T3 AFL++ harnesses landed; all verified in a clang-18/afl++4.09c fuzz image: libFuzzer baseline exit 0/crash artifact; AFL native forkserver (`fuzz-afl`) replay exit 0 + `afl-fuzz -V 15` finds 1 crash; AFL aflpp_driver reuse (`fuzz-afl-driver`) also finds 1 crash (persistent+deferred forkserver). Two doc corrections re-verified against code: (1) clang-18 libFuzzer requires the parallel PC table; (2) this AFL++ ships NO `__sanitizer_cov_*` runtime, so both AFL paths feed `__afl_area_ptr` via a plain-compiled map helper (afl-clang-fast rewrites an in-harness `__afl_area_ptr` ref to an undefined `.2`). T4 landed: `Dockerfile.fuzz` (clang 18 + libclang-rt-18-dev + afl++ 4.09c on asmtest-bindings-base) + `docker-fuzz` (mk/docker.mk) + `fuzz-shim-test` (mk/fuzz.mk) + help lines. **`make docker-fuzz` green end-to-end**: libFuzzer baseline exit 0 (cov 11/ft 13) + crash artifact, AFL native replay exit 0, `afl-fuzz -V 15` finds 1 crash — no self-skip, both engines genuinely steer to the planted crash. Only T5 (docs/changelog) remains) | — |

### Correctness & attribution
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [code-review-plausible-triage.md](code-review-plausible-triage.md) — triage & fix the 2026-07-02 review's still-present findings | 8 | ☑ 8/8 | — |
| [native-il-bytecode-attribution.md](native-il-bytecode-attribution.md) — native trace-point → IL/bytecode/source-line attribution | 7 | ☐ 0/7 | — |

## Parallelism & the critical path

**25 of the 31 documents are fully independent** — no shared code, no cross-doc
dependency — and can be assigned to different developers and implemented
concurrently. There are only three true ordered chains:

1. **Intel PT** — [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md)
   must land first (it builds the one perf-AUX `intel_pt` capture helper and the
   `begin_window` PT arm). Then
   [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md) extends that
   arm to foreign PIDs, and
   [dataflow-pt-replay-tier.md](dataflow-pt-replay-tier.md) consumes the
   foreign-PID capture. [managed-wholewindow-compose.md](managed-wholewindow-compose.md)'s
   PT prongs also wait on the substrate, but its managed-stepper prongs are
   Docker-testable in parallel now.
2. **Intel Pin** — [pin-xed-trace-tier.md](pin-xed-trace-tier.md) creates the
   shared `Dockerfile.pintool` + `scripts/fetch-pin.sh` substrate, so
   [pin-probe-mode-capture.md](pin-probe-mode-capture.md) and
   [pin-libdft-taint-oracle.md](pin-libdft-taint-oracle.md) build on it.
   [pin-sde-future-isa-lane.md](pin-sde-future-isa-lane.md) is independent (it
   owns the APX-capable pinned assembler the Pin lanes reuse — a soft coupling,
   not a blocker).
3. **CI** — [self-hosted-ci-runners.md](self-hosted-ci-runners.md) wires CI
   jobs that invoke the clean-room lanes, so
   [macos-cleanroom-lanes.md](macos-cleanroom-lanes.md) should be green first.

**Soft coordination (shared files, not hard dependencies — sequence to avoid
merge conflicts):** the two AMD docs share the Zen 3 story;
`dataflow-producer-correctness` and the F2 work inside it both edit
`src/dataflow_blockstep.c`; the three CI-touching docs
(`macos-dynamorio-port`, `self-hosted-ci-runners`, `benchmarks-ci-followups`)
all edit `.github/workflows/ci.yml` and must uniformly use `macos-15-intel`.
Each doc's *Task order & parallelism* section spells out its own soft couplings.

## Hardware & credential gates

Per [CLAUDE.md](../../../CLAUDE.md), a missing **installable** dependency is
never a blocker — it is added to the relevant `Dockerfile.*` + `docker-*` rule
with a pinned version. Only **hardware** (specific CPU generations, Intel PT,
CoreSight, Apple silicon) and **credentials** are legitimate self-skip gates.
Many docs implement fully in Docker and gate only their *live-validation* leg on
real silicon (e.g. the AMD docs implement and unit-test everywhere but validate
on the Zen 5 dev box; the Intel PT docs need a bare-metal PT host; the registry
publish steps in `distribution-packaging` need credentials). Each doc's
*Constraints & gates* section names its exact gate and what to record when it
blocks validation.
