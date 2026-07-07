# asm-test — Plans vs. codebase: remaining-items analysis (2026-07-04)

> **2026-07-07 update — most of §1 has since landed.** This document is a
> snapshot; the text below is preserved as written on 2026-07-04. Landed since:
> the **AMD tracing Part III software items** (P3-0/1/2/3/4/5 — spec filter,
> stitch guard, runtime depth, freeze probe, block-step tiers, and even the
> "blocked" P3-3 eBPF LBR snapshot, validated live on the Zen 5 box), the
> **call-descent Phase 5 built-in default denylist**
> (`asmtest_descent_use_default_denylist`), **macOS clean-room Track E**
> (release.yml smokes under `clean-env.sh`, `docs/clean-room-testing.md`) with
> Tracks C/D written-per-plan (unvalidated: no tart/KVM host here), the
> **DynamoRIO plan's stale heading markers** reconciled, and from the review
> backlog: R5, A6, A7, E7, E5, P4, P2, K1 (pending a CI run), and N4 — see the
> [2026-07-04 review's status tables](../reviews/2026-07-04-repo-review.md) for
> the authoritative disposition. **Further landed 2026-07-07:** K1 validated on
> a real Actions run (all 45 actioned review items closed) and BOTH single-step
> Phase 5 fronts — macOS-Intel and the **Windows x86-64 VEH front-end**
> (`src/ss_win64.c`, `make win64-ss-test`, Wine + real-Windows CI) — leaving the
> AArch64 ptrace tracer as the only forward-look front. Still open from §1: the
> review's P1/P3 expansions (P1 user-gated behind the first real tag; P3 the
> large RISC-V native host tier).
> §2 (hardware/privilege-blocked) and §3 (maintainer/credential actions —
> the first real tag remains user-gated) are unchanged.

**Scope:** a cross-check of all 11 active plans in [docs/plans/](../plans/) plus the
still-open items from the [2026-07-04 repo review](../reviews/2026-07-04-repo-review.md)
against the actual codebase (`src/`, `include/`, `mk/`, `Makefile`, `tests/`,
`bindings/`, `.github/`), to determine what remains to be implemented.

**Method:** one reviewer per plan (or related pair) read the plan's status legend,
then verified each phase/track against source with `file:line` evidence —
distinguishing *shipped* from *planned-not-implemented*, *partial*, and
*blocked-on-hardware/privilege/upstream*.

## Bottom line

The **core framework and every tracing tier's software half are shipped and
CI-gated.** Across all 11 plans, almost nothing in *committed scope* is
unimplemented — the remaining work is (1) a concrete backlog from the 2026-07-04
review, (2) forward-look phases blocked on hardware/silicon/upstream, and (3) a
handful of maintainer/credential actions.

---

## 1. Actionable now — real, unblocked work

### From the 2026-07-04 review

Largest concrete backlog (~28 verified items still open after the step-1/step-2
batches landed in `59adb74`/`817cc72`). Highest-leverage:

| Cluster | Items |
|---|---|
| CI leverage | K1 (cache Keystone/Capstone builds — ~20× rebuild/push), K2 (wire `hwtrace-bindings-test`/`codeimage-test` into CI), K3 (clang-tidy/gcov cover 1 of 20 TUs), K4 (ASan/UBSan the emu/trace tiers), K5 (`.build-flags` sentinel missing from PIC/native-trace objects), K6 (install native-trace headers), K7 (docs drift) |
| Defect | E3 (register-preload advertised, no API — add or retract the claim) |
| Runner/DX | R4 (JUnit `<error>`/`time=`/`<system-out>`), R5 (`--fail-fast`/`--repeat`/shard), R6 (bench dispersion + JSON), R7 (`NO_COLOR`/`--color`) |
| Emu/tooling | E4 (persist fuzz corpus), E6 (read watchpoints), E7 (shrink failing inputs), E5 (snapshot/restore), A6 (`ASM_MIXCALL`), A7 (FP reference models) |
| Docs | D2 (README→docs funnel), D4 (API-ref post-1.0 surface), D5 (troubleshooting/FAQ), P4 (comparison page) |
| Finder-only leads (re-verify first) | N3 (Lua rock/Java jar not truly publishable), N4 (wide-arity capture unreachable from bindings), N5 (Zig no importable module), N6 (corpus-parity tripwire), X1–X4 (self-test gaps: TEARDOWN unasserted, negative paths run with exit ignored, SIGILL/SIGFPE/SIGBUS untested, JUnit XML escaping unvalidated) |

### From the plans

- **Call-descent — one real gap:** Phase 5 promised a *built-in default denylist*
  (GC/JIT/PLT/libc); only the caller-supplied mechanism shipped. Safety currently
  rests on budget+watchdog+default-off. (Two other limitations — signal-frame
  SP-pop suspension, tail-call keep-open — are honestly documented as deferred.)
- **AMD tracing Part III** (all software, runs on ordinary CI): P3-2 BTF block-step
  (`PTRACE_SINGLEBLOCK`) is the highest impact-to-effort; P3-0 CPUID `0x80000022`
  depth reader (currently hardcoded `16` in `src/hwtrace.c` and `src/amd_backend.c`);
  P3-4 spec/wrong-path filtering in `amd_replay`; P3-5 stitch decodable-distance
  invariant.
- **Single-step Phase 5 fronts** (pure software): Windows x86-64 VEH single-step,
  macOS-Intel front-end.
- **macOS clean-room Track E:** `release.yml` smoke blocks still use `cd /tmp &&
  env -u` instead of `source scripts/clean-env.sh`; `docs/clean-room-testing.md`
  missing; Tracks C/D (`scripts/osx-vm.sh`, `docker-osx-bindings`) can be *written*
  here but not run.
- **Housekeeping:** the DynamoRIO plan's per-phase `*(planned)*` heading tags are
  stale (Phases 0–8 are actually shipped) — reconcile the markers.

---

## 2. Blocked on hardware / silicon / privilege / upstream

- **Hardware trace live capture:** Intel PT live AUX capture (needs bare-metal
  Intel PT host), ARM CoreSight live OpenCSD decode (`cs_decoder_present()`
  hard-returns 0 — needs a CoreSight board), Phase 2 PT-attach-to-live-PID.
- **AMD:** P3-1 freeze-absent (Zen 4), P3-3 eBPF LBR snapshot
  (`CAP_PERFMON`/`CAP_BPF`), P3-6 BRS (Zen 3), P3-7 IBS (Zen 2), Part I MSR-direct
  (`CAP_SYS_ADMIN`).
- **AArch64:** live single-step trace stream + HW-breakpoint `run_to` validation
  (qemu-user can't emulate the ptrace tracer/tracee relationship — self-skips).
- **W3 BTF branch-step:** needs a kernel helper / uapi patch.
- **Wide-vector SVE** (post-v1 Track D): needs an SVE host; emu wide-vector blocked
  upstream (bundled Unicorn rejects AVX).
- **macOS DynamoRIO port (entire `macos-drtrace-plan`):** BLOCKED-upstream — no
  macOS DynamoRIO release has ever existed; correctly parked at its Step-0 gate.
- **P3 RISC-V native host tier** (review): large, needs the binfmt lane.

---

## 3. Maintainer / credential actions (not code)

- **P4 / expansion Track B — cut the first real tag.** `git tag -l` is **empty**:
  `VERSION` = `1.1.0` but no `v1.x` tag ever existed, so `release.yml`'s real
  trigger has only run as `workflow_dispatch` dry-runs. This is the single item
  multiple plans assume is done but isn't.
- **post-v1 Track A / P1 / P2 (review) — go-live:** register per-ecosystem package
  names, add publish-token secrets, then push the tag. P1 (system package managers:
  brew/deb/AUR/vcpkg/conan) and P2 (consumer-facing GitHub Action / GitLab
  template) are sequenced behind that tag.

---

## What's fully done (no remaining scope)

The C core + self-tests (expansion Track A/C/D/E), all 10 language bindings +
conformance substrate, DynamoRIO tier Phases 0–8 (bindings/docs/CI included),
single-step W2 tracer (x86-64 + AArch64 code), AMD LBR Part I (Zen 5-validated),
Intel PT / CoreSight / AMD *reconstruction* cores, the Win64 tier, Capstone
disasm, fuzzing/mutation, watchpoint asserts, and the open-defects P0–P3 (Go
flaky crash, supply-chain pinning, Intel-mac payload, `-j` recursive-make race).
</content>
</invoke>
