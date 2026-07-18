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
| [amd-ibs-backend-honesty.md](amd-ibs-backend-honesty.md) — IBS backend honesty, record sizing, ABI guards, validation gate | 7 | ◐ 5/7 (T1 verified live) | — |
| [amd-branchsnap-lbr-docs.md](amd-branchsnap-lbr-docs.md) — branchsnap depth fix, LBR tiling validation, freeze-probe cleanup, Zen 3 BRS story | 8 | ◐ 7/8 (T1/T4/T5/T6 landed; T2/T3/T7 code landed, live Zen 5 BPF validation gated; **T8 BLOCKED**: needs Zen 3 Family 19h silicon — arm must not merge untested per CLAUDE.md) | — (soft: shares the Zen 3 story with the sibling above) |
| [ptrace-blockstep-tracer-correctness.md](ptrace-blockstep-tracer-correctness.md) — int3 si_code, rep-prefix, SP-aware step-over, IBS pre-cover | 8 | ☐ 0/8 | — |

### Data-flow tier
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [dataflow-producer-correctness.md](dataflow-producer-correctness.md) — gap barrier, sub-register aliases, undefined flags, F2 record-and-inject | 8 | ☐ 0/8 | — |
| [dataflow-bindings-slice-codeimage.md](dataflow-bindings-slice-codeimage.md) — def-use/slice surface + code-image arg across bindings | 4 | ☐ 0/4 | — |
| [dataflow-f4-object-identity.md](dataflow-f4-object-identity.md) — real object identity via GCBulkType/Node/Edge | 6 | ☐ 0/6 | — |
| [dataflow-pt-replay-tier.md](dataflow-pt-replay-tier.md) — F5: PT + code-image + Unicorn-replay value tier | 5 | ☐ 0/5 | **intel-pt-attach-foreign-pid** |

### Intel PT & CoreSight hardware trace
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md) — PT whole-window capture substrate, STRONG ladder, inline ctor | 5 | ☐ 0/5 | — |
| [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md) — PT attach-to-foreign-PID capture, facade dispatch, HV/EPT frontier | 5 | ☐ 0/5 | **intel-pt-whole-window-substrate** |
| [coresight-live-decode.md](coresight-live-decode.md) — CoreSight live OpenCSD decode tree (AArch64 board-gated) | 5 | ☐ 0/5 | — |

### Intel Pin / SDE oracle lanes
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [pin-sde-future-isa-lane.md](pin-sde-future-isa-lane.md) — SDE future/absent-ISA test lane | 8 | ☐ 0/8 | — |
| [pin-xed-trace-tier.md](pin-xed-trace-tier.md) — XED-decoded Pin trace tier + shared pintool substrate | 9 | ☐ 0/9 | — |
| [pin-probe-mode-capture.md](pin-probe-mode-capture.md) — Pin probe-mode argument/return capture | 7 | ☐ 0/7 | **pin-xed-trace-tier** |
| [pin-libdft-taint-oracle.md](pin-libdft-taint-oracle.md) — libdft64 differential oracle for the DR taint tier | 7 | ☐ 0/7 | **pin-xed-trace-tier** |

### asmspy CLI
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [asmspy-cli-enhancements.md](asmspy-cli-enhancements.md) — TUI hot-edge drill-in, syscall-arg content decode, coverage gaps | 9 | ☐ 0/9 | — |
| [asmspy-aarch64-support.md](asmspy-aarch64-support.md) — single-step engine abstraction + NT_ARM_HW_WATCH watchpoints | 7 | ☐ 0/7 | — |

### Single-step & block-step tiers
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [inproc-btf-block-step.md](inproc-btf-block-step.md) — W3: in-process BTF branch-granular single-step | 6 | ☐ 0/6 | — |
| [macos-oop-mach-stepper.md](macos-oop-mach-stepper.md) — macOS out-of-process single-step via Mach exception ports | 7 | ◐ 1/7 | — |
| [aarch64-ptrace-single-step-validation.md](aarch64-ptrace-single-step-validation.md) — AArch64 ptrace stream validation + binding fixtures | 6 | ☐ 0/6 | — |

### Scoped / managed whole-window tracing
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [zeroconfig-scoped-tracing-hardening.md](zeroconfig-scoped-tracing-hardening.md) — in-process guards, hygiene assertions, doc-tail | 9 | ☐ 0/9 | — |
| [managed-wholewindow-compose.md](managed-wholewindow-compose.md) — live compose, safe managed-arm routing, ambient PT stitching | 12 | ☐ 0/12 | **intel-pt-whole-window-substrate** (PT prongs only; D3-stepper prongs are independent) |

### macOS
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [macos-cleanroom-lanes.md](macos-cleanroom-lanes.md) — tart arm64 / Docker-OSX x86 shakedowns + sshpass containerization | 6 | ☐ 0/6 | — |
| [macos-dynamorio-port.md](macos-dynamorio-port.md) — DynamoRIO native-trace port M0–M2 (gated on upstream DR macOS release) | 11 | ☐ 0/11 | — |

### Architecture ports
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [aarch64-sve-capture.md](aarch64-sve-capture.md) — AArch64 SVE wide-vector capture | 8 | ☐ 0/8 | — |
| [riscv-native-tier.md](riscv-native-tier.md) — native RISC-V (rv64) host tier | 7 | ☐ 0/7 | — |

### CI, distribution & infrastructure
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [distribution-packaging.md](distribution-packaging.md) — language-registry go-live + system package-manager coverage | 13 | ☐ 0/13 | — |
| [benchmarks-ci-followups.md](benchmarks-ci-followups.md) — windows/macOS-Intel legs, nightly auto-commit, BM_MODEL_COST | 6 | ☐ 0/6 | — |
| [self-hosted-ci-runners.md](self-hosted-ci-runners.md) — self-hosted runner lanes for hardware-gated tiers | 6 | ☐ 0/6 | **macos-cleanroom-lanes** (soft coordinate: amd-ibs, coresight, substrate) |
| [libfuzzer-afl-shim.md](libfuzzer-afl-shim.md) — libFuzzer/AFL harness shim (demand-gated) | 5 | ☐ 0/5 | — |

### Correctness & attribution
| Document | Tasks | Status | Depends on |
|---|---|---|---|
| [code-review-plausible-triage.md](code-review-plausible-triage.md) — triage & fix the 2026-07-02 review's still-present findings | 8 | ☐ 0/8 | — |
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
