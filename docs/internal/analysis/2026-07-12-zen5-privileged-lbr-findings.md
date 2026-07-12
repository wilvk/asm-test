# Zen 5 privileged-capture findings — first live AMD LBR / IBS run

Review date: 2026-07-12. Host: **AMD Ryzen 9 9950X** (Zen 5, `amd_lbr_v2` +
`perfmon_v2` + `ibs`), Linux 6.17, `perf_event_paranoid=4`, no `CAP_PERFMON` on
the host. Captured via the new `make docker-hwtrace-privileged` lane
(`--cap-add=PERFMON` alone, default seccomp) — the **first** time the exact AMD
branch-stack (LbrExtV2) and live IBS capture paths have actually run here rather
than self-skipping.

## 1. What now runs and passes

With `CAP_PERFMON` the previously-skipping tests all execute and pass:

- `test_hwtrace` **389/389** (unprivileged baseline in the same image: 367 + 12
  SKIPs). Live AMD LBR capture, Tier-B stitch beyond one 16-deep window,
  over-ring honest truncation, `#2A` period reach (period=1/4), concurrent
  per-thread perf fds, `sample_window` (`nips=1024`, in-loop=1021, truncated=1),
  the IBS-Op survey fallback + begin/end split.
- `test_ibs` **23/23** ×2. Live IBS out-of-band `survey_pid` (retired
  taken-branch edges, spin-loop back-edge) and whole-process `survey_process`
  (3/3 worker functions covered).
- `ibs_probe`: IBS-Op **AVAILABLE**, OpSam + BrnTrgt capable.

`CAP_PERFMON` alone suffices — no `--privileged`, no `SYS_ADMIN`, no
`seccomp=unconfined`. The unprivileged baseline (no cap) proves the cap is
necessary; the privileged run proves it is sufficient.

Still expected-skipped: Intel PT (wrong vendor), AMD MSR-direct (needs
`/dev/cpu/N/msr` + `CAP_SYS_ADMIN` — a separate `docker-hwtrace-msr` lane).

## 2. ~~OPEN~~ RESOLVED finding — `trace_call_auto` AMD-LBR rung mis-reports completeness

**Severity: correctness (flaky, can pass vacuously). RESOLVED 2026-07-12.**

> **Fixed 2026-07-12.** Root cause: `asmtest_amd_ring_parse_decode`'s Tier-A
> completeness check — "trust a single window as complete only if it CONTAINS the
> region-exit branch" — was gated behind `!asmtest_amd_freeze_available()`. On a
> freeze-capable part (Zen 5, `freeze=1`) the check was skipped, so when the
> sample-period=1 capture picked the *richest-in-region* window (an arbitrary
> mid-run fragment that never held the exit — `best_nr=4`, no exit edge), a
> 6-of-77-instruction reconstruction passed as complete → no escalation. Fix
> (`src/hwtrace.c`): the exit-presence requirement now runs on **every** part
> (freeze does not make a non-exit richest window complete); combined with the
> existing `best_nr >= depth` overflow flag this is the airtight "complete iff a
> non-overflowed exit-anchored window exists" invariant. `test_call_auto` case (b)
> hardened with a `CEILING_FREE` honest-count baseline so a dropped-branch
> fragment reported complete is now a HARD failure. Verified deterministic across
> **16 privileged AMD runs** (every one escalates `backend=3 insns=77`; zero
> `insns=3 truncated=0`). Both cosmetic misreports in §3 also fixed.

`examples/test_hwtrace.c` `test_call_auto` case (b) drives a loop with **25
taken back-edges** through `asmtest_trace_call_auto` on `backend=AMD_LBR`. A
16-deep LbrExtV2 window cannot hold 25 back-edges, so the rung *should* mark the
result truncated and the cascade should escalate to block-step.

Observed live on Zen 5 (two runs, **intermittent**):

- lbr-priv implementer's run: `rc=0 result=25 used.backend=2 insns=3
  truncated=0 (LBR window sufficed)` — escalation did **not** fire; the check
  passed only vacuously (it asserts `!truncated && covered`, both true on the
  short read). The basic case also undercounted: `backend=2 insns=3` vs the
  single-step baseline `insns=5`.
- verifier's independent run: escalation **did** fire (`backend=3 insns=77`).

So the `asmtest_trace_call_auto` AMD-LBR rung's completeness heuristic
**intermittently misreads a freeze-on-PMI tail window as a complete capture** —
`truncated=0` on a window that demonstrably dropped branches. The direct
live-capture path is honest here (`sample_window` reports `truncated=1` on the
same shape), so the bug is **rung-specific to the `call_auto` LBR path**, not the
decoder.

Why it went unseen: every host in CI either lacks AMD LBR (self-skips) or is the
unprivileged dev box (self-skips at `paranoid=4`). The `docker-hwtrace-privileged`
lane is the first place this rung runs live, and it is **not yet in CI**.

### Suggested next step

- Make the `call_auto` LBR rung derive truncation the same way the direct
  capture path does (the honest `sample_window`/Tier-B truncation signal),
  rather than a rung-local heuristic — a statistical/partial LBR window must set
  `truncated` so the cascade escalates.
- Harden `test_call_auto` case (b) so a short read is a **failure**, not a
  vacuous pass: assert the reconstructed `insns` count matches the single-step
  baseline (or that `truncated` is set), so the flake becomes a hard signal.
- Consider gating the privileged lane in CI (a self-hosted AMD runner or a
  documented manual pre-release step) so the exact AMD paths get continuous
  coverage.

## 3. Cosmetic misreports (low priority)

- `examples/test_hwtrace.c` final PT-gated capture+decode block prints
  `# SKIP hwtrace AMD capture (AMD LBR): available` — the skip text names the
  wrong reason when only PT is missing.
- `ibs_probe` prints a static `unprivileged (paranoid=2)` explanation string on
  a `paranoid=4` host.
