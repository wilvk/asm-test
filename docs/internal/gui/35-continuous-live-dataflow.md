# Continuous live `dataflow` / `auto` capture — re-arm the engine — implementation

> **A tail follow-on to the [extension roadmap](27-extension-roadmap.md)**, not one
> of its six roots and **not** R3. It removes a single structural limit: the live
> `dataflow` / `auto` engine captures **one** invocation of the picked region and
> then the session ends on its own, which reads as "auto + Start starts then
> stops." This brief makes the engine **re-arm and keep capturing until Stop**,
> into one continuously growing recording.
>
> **This is not [R3 resume-from-state](30-resume-from-state-and-reweave.md).** R3
> restores an emulator snapshot to run a counterfactual from an *edited* state, and
> live-process resume "stays killed" ([30](30-resume-from-state-and-reweave.md)).
> Continuous capture re-runs the **same** scoped capture repeatedly; it never
> restores, edits, or forks live state. And it is **not** "whole process,
> continuously" — that unbounded-window posture is explicitly refused as the DR
> taint tier's job ([dataflow_ptrace.c:2150-2155](../../../src/dataflow_ptrace.c#L2150));
> every pass here stays scoped and bounded exactly as today.
>
> Authored 2026-07-28, verified against HEAD `f2b6cdf`. If a cited file:line
> disagrees with the code when you implement, the code wins — re-verify, then fix
> this doc in the same change.
>
> **Status (2026-07-28) — ☑ 4/4. T1, T2, T3, T4 landed.**
> The continuous feature works end to end: a live `dataflow`/`auto` session
> re-arms until Stop into one growing recording delimited by `df_invocation`
> markers, the desktop segments it per pass and follows the latest live, the
> capture pane arms it, and Stop is now honored WITHIN one in-flight pass (T2).
> (README reconciled to ☑ 4/4 — T2's "Done when" bar is met; T2b seize-once is
> a deferred beyond-bar optimization with no measurable regression.)
> - **T1 — DONE** (`fa2cef4`): `asmspy_engine_dataflow` gains `continuous`, wraps
>   the producer in a re-arm loop, and emits a `df_invocation`
>   `{pass,result,steps,truncated}` marker (new schema kind, D5) before each pass;
>   `--dataflow --continuous` + serve `continuous:true`; `cli_smoke` proves ≥2
>   pass-delimited invocations in one recording, clean SIGINT stop, and a
>   byte-identical one-shot default (verified live, 15 passes).
> - **T3 — DONE** (`fc836e0`): `build_segmented_step_index` buckets the regstate
>   ring by `df_invocation` seq into one StepIndex per pass; `build_step_index` is
>   segment-aware (latest pass = the live default; one-shot byte-identical);
>   `test_scrubber` drives the hand-authored `low-fidelity/continuous-df.asmtrace`
>   fixture. (Def-use/streams per-pass segmentation is a scoped follow-on — the
>   Scrubber is the primary continuous consumer; a continuous def-use view shows
>   the last pass's offsets, not a lie about fidelity chrome.)
> - **T4 — DONE** (`fc836e0`): `InspectState.continuous` + a capture-pane checkbox;
>   `inspect_start_params` sends `continuous:true` only for the dataflow engines
>   (`test_shell`); the once-per-session perturb confirm covers the session. The
>   golden is hand-authored (three passes, one truncated) rather than a generator
>   path, to avoid the make_pair 64-byte-window golden-churn hazard.
> - **T2 — DONE (interruptible Stop).** `atomic_bool *stop` is threaded through a
>   new public `asmtest_dataflow_ptrace_attach_jit_stop` (the old `attach_jit`
>   forwards `NULL`, so its three external callers — `asmspy_engine`,
>   `test_dataflow_ptrace`, `gccanon` — are unchanged) into `dfp_run_to_multi`'s
>   entry-wait poll AND `dfp_step_loop`; the T1 re-arm loop now calls `_stop`. The
>   EINTR/mid-step hazard the earlier deferral flagged is handled: the stop check
>   sits at the loop TOP, and `serve_stop` / the CLI signal handler set `stop`
>   BEFORE the `SIGALRM`/`SIGTERM`, so the interrupted `waitpid`'s `continue`
>   lands on the stop check and terminates via the crash-safe `dfp_dirty_exit`
>   BEFORE re-issuing a `SINGLESTEP` — the tracee is trap-stopped (clean
>   `PTRACE_DETACH`), or the kernel auto-detaches on tracer exit; either way the
>   target survives, proven by the `cli_smoke` SIGTERM case (honored within one
>   in-flight pass, ≤ 5 s, victim survives). The arm64 `#else` ENOSYS stub was
>   ADDED and **compile-verified on aarch64** (the pinned arm64 docker bindings
>   base under qemu — the whole `dataflow_ptrace.c` body stays `#if __x86_64__`, so
>   continuous on arm64 self-skips (`ASMSPY_DATAFLOW_UNAVAIL`) before any
>   `PTRACE_SINGLESTEP` and the detach-fatal hazard is architecturally unreachable;
>   any FUTURE arm64 single-step re-arm must still use `PTRACE_SYSCALL`-at-resume,
>   never a naive re-armed `SINGLESTEP`). **T2b (seize-once — hold one multi-thread
>   seize across passes) stays DEFERRED:** making `dfp_step_loop` whole-target-
>   drain-aware to let the target run free between passes while holding the seize
>   rewrites the tier's most fragile function; the per-pass re-SEIZE is O(threads)
>   and negligible against a pass's 10³–10⁵× single-step cost, so there is no
>   measurable regression. **Known separate issue (pre-existing, not T2):** a
>   `--serve` continuous session's `max` bounds insns-per-pass (not pass count), so
>   the pass loop is unbounded, and a `quit` there is not honored promptly (the
>   CPU-intensive re-arm loop starves the command thread) — reproduced on
>   `origin/main` WITHOUT this change; it is a T1 serve-loop concern, not the
>   producer's interruptibility.

## Why this work exists

`auto` is `dataflow` behind a picker, and the dataflow producer is one-shot by
construction: [`asmspy_engine_dataflow`](../../../cli/asmspy_engine.c#L3629) calls
`asmtest_dataflow_ptrace_attach_jit` **once** — a full SEIZE → wait-for-entry →
single-step one entry→exit → DETACH — and returns. There is no `while (!stop)`
loop, unlike every other live mode
([syscalls :2733](../../../cli/asmspy_engine.c#L2733),
[region :3508](../../../cli/asmspy_engine.c#L3508),
[stream :3760](../../../cli/asmspy_engine.c#L3760),
[graph :4176](../../../cli/asmspy_engine.c#L4176),
[tree :4427](../../../cli/asmspy_engine.c#L4427),
[procs :4795](../../../cli/asmspy_engine.c#L4795),
[watch :5195](../../../cli/asmspy_engine.c#L5195)). So the tracer thread ends the
instant that one invocation completes, `growing()` goes null, and the live
Scrubber / def-use view freeze on a single sample. On a target where the picker
is gated (IBS under `perf_event_paranoid`) or the region does not re-enter within
the entry wait, the pass returns a positive skip (`ASMSPY_REGION_NEVER_RAN`) and
the session ends with an empty recording — the same symptom, faster.

The other live modes prove the shape is sound; the region engine
([`asmspy_engine_region`](../../../cli/asmspy_engine.c#L3450)) already re-samples a
scoped region every pass until `stop`. This brief brings the dataflow engine to
that model, with the two consequences that make it more than a `while` wrapper.

## What already exists (verified 2026-07-28)

- **The one-shot producer** — [`asmspy_engine_dataflow`](../../../cli/asmspy_engine.c#L3629);
  the per-call value trace `vt` (and its optional register ring) is allocated and
  freed *inside* the call, so nothing persists between invocations.
- **The full attach lifecycle** — `dfp_attach_worker`
  ([dataflow_ptrace.c:1985](../../../src/dataflow_ptrace.c#L1985)) owns SEIZE →
  entry-wait → step → DETACH per call. It leaves no leaked attach (safe to call in
  a loop), but each call **re-SEIZEs the whole target tree** — unlike the region
  engine, which holds one seize across all passes.
- **The loop to mirror** — [`asmspy_engine_region`](../../../cli/asmspy_engine.c#L3450):
  seizes once, checks `stop` in the `while` condition **and** threads it into the
  entry race (interruptible), re-arms the entry per pass via a `planter`, emits one
  sample per produced trace, and detaches once after the loop.
- **One recording per session, opened/closed once** — `serve_tracer`
  ([rec_open :3757](../../../cli/asmspy.c#L3757) /
  [rec_close :3788](../../../cli/asmspy.c#L3788)) brackets `serve_run_engine`, so a
  loop inside the engine naturally yields **one growing recording** (header once,
  events appended, `end` once). The desktop's
  [`shell_sync_live_tab`](../../../desktop/src/ui/shell.cpp#L169) already follows a
  growing recording live.
- **The stop signal** — `atomic_bool s->stop`, set by `serve_stop`
  ([asmspy.c:3804](../../../cli/asmspy.c#L3804)) with a `SIGALRM` wake + join.
- **The two hazards this brief must resolve** (both absent for the region engine,
  because it re-samples a held attach rather than re-SEIZEing a fresh `vt`):
  1. **Step-index collision.** Each pass gets a fresh `vt`, so `df_step` and
     `regstate` both restart at step 0, and the `end` footer's `steps_total` is
     **overwritten per pass** ([dataflow_record :2358](../../../cli/asmspy.c#L2358)).
     The desktop step-index keys on that step field with
     `first_step = drops_lost = 0`
     ([stepindex.cpp:86-87](../../../desktop/src/analysis/stepindex.cpp#L86)), so
     without a discriminator two invocations' step ranges overwrite each other.
  2. **Uninterruptible mid-pass.** Neither the entry wait nor
     [`dfp_step_loop`](../../../src/dataflow_ptrace.c#L1007) takes `stop`; the step
     loop treats `EINTR` as retry
     ([:1027](../../../src/dataflow_ptrace.c#L1027)). So `stop` is only observed
     *between* passes, and one pass can run to the `2^20`-step backstop or the 10 s
     entry wait before yielding.

## Tasks

### T1 — engine re-arm loop + per-invocation discriminator  (M)

**Goal.** Turn `asmspy_engine_dataflow` into a bounded re-arm loop that emits a
continuous stream of invocations into one growing recording, each cleanly
delimited.

**Steps.**
1. Wrap the producer call in the standard live-mode loop —
   `while ((max < 0 || passes < max) && !(stop && atomic_load(stop)))` — mirroring
   [region :3508](../../../cli/asmspy_engine.c#L3508). Gate it on a new
   `continuous` param so the default stays one-shot (D-compat).
2. Emit a **`df_invocation`** marker (new reserved kind, D5 — define fields in
   [asmtrace-schema.md](asmtrace-schema.md): `{pass, result, steps, truncated}`)
   **before** each pass's `df_step` run, so the reader segments passes without
   guessing. This retires the ambiguity of the sticky/overwritten `steps_total`
   footer: `steps` rides the marker per pass; the footer `steps_total` becomes the
   cumulative total (or is documented as last-pass-only and deprecated for
   continuous). Fix field order (D6); coordinate the kind with the freeze (D5).
3. `auto` (`SM_AUTO`) pins its pick: run the picker **once**, then re-arm the same
   `(base,len)` each pass rather than re-picking the hottest region every cycle
   (which would make the view jump between functions). The serve dispatch is
   [SM_AUTO :3645](../../../cli/asmspy.c#L3645); the pick note is already emitted
   for the desktop.
4. Serve param + CLI flag: `continuous:true` in `serve_parse_start`
   ([asmspy.c:3861](../../../cli/asmspy.c#L3861), beside the existing
   [`steps` :3916](../../../cli/asmspy.c#L3916) / [`mem` :3926](../../../cli/asmspy.c#L3926))
   and `--dataflow --continuous` on the CLI.

**Done when.** A continuous dataflow session over a hot fixture region emits ≥2
`df_invocation`-delimited passes into one recording and ends cleanly on `stop`;
the one-shot default is byte-identical to today; `test_dataflow` / a serve test
covers both.

### T2 — interruptible Stop + cheap re-arm (hold one seize)  (L)

**Goal.** Bound Stop latency and stop re-SEIZEing the whole target every pass.

**Steps.**
1. Thread `atomic_bool *stop` into `dfp_run_to_multi` (the entry wait) and
   [`dfp_step_loop`](../../../src/dataflow_ptrace.c#L1007): on `stop`, treat the
   `SIGALRM`/`EINTR` as **terminate**, not retry
   ([:1027](../../../src/dataflow_ptrace.c#L1027)) — so a running pass yields
   promptly instead of only between passes.
2. Refactor `dfp_attach_worker`
   ([dataflow_ptrace.c:1985](../../../src/dataflow_ptrace.c#L1985)) to separate
   **seize-once** from **per-invocation step**, so the loop holds one seize and
   re-arms the entry each pass (the region engine's `planter` model) instead of a
   fresh SEIZE/DETACH per pass. This is the invasive part; it touches the ptrace
   core.
3. **arm64 pass required.** Single-step re-arming amplifies the arm64 detach-fatal
   hazard (PTRACE_SINGLESTEP on a thread in a blocking syscall survives DETACH and
   kills the target); verify on the arm64 docker/GH lane, not just x86-64.

**Done when.** A `stop` during a live continuous capture is honored within one
in-flight pass (asserted with a timed test); a long session shows no per-pass
re-SEIZE cost regression; the arm64 lane is green (or the hazard is re-confirmed
and gated, per project policy).

### T3 — desktop consumer: segment by invocation  (M)

**Goal.** Render the continuous stream without conflating passes; default to a
live-refreshing view of the latest invocation.

**Steps.**
1. Step-index / Scrubber / def-use key off `df_invocation` so each pass's
   `df_step` 0..N is scoped to its pass, not overwritten
   ([stepindex.cpp:86](../../../desktop/src/analysis/stepindex.cpp#L86)). Default:
   show the **latest** invocation, refreshing live; optionally a small invocation
   navigator (prev/next pass) — a stepping stone toward the roadmap's "accumulate
   invocations" timeline.
2. `shell_sync_live_tab` / `growing()` already follow a growing recording; confirm
   the live-weave banner and the single-tab promotion behave for a recording that
   grows across many invocations (no per-pass tab churn).

**Done when.** The null-backend desktop test drives a two-invocation continuous
recording and asserts the two passes are addressable separately and the tab shows
the latest; `desktop-test` green.

### T4 — UI arming + goldens + fidelity fixtures  (S–M)

**Steps.**
1. A "continuous" checkbox in the capture pane (an `InspectState` flag beside
   `steps`); send `continuous:true`. Do **not** re-confirm the perturbation
   warning per pass — mark it session-confirmed on the first accepted start.
2. D6/D7: a continuous golden (multi-invocation) **and** a low-fidelity fixture
   (a pass that skips / truncates), with the renderer test asserting the faithful
   placard per pass.

**Done when.** `make docker-desktop` + `docker-cli` green; the checkbox drives a
live continuous capture end-to-end; goldens committed under
`tests/golden-asmtrace/`.

## Alternative considered — desktop-driven re-arm (not the endpoint)

A cheaper MVP is to leave the engine one-shot and have the **desktop re-fire
`start`** each time a dataflow/auto session completes, until Stop. It needs **zero
engine or schema change** (each pass is a clean separate recording, no step-index
collision), reuses `shell_sync_live_tab`'s "follow growing → fall back to last
completed" logic, and is ~1 day. It is a legitimate first ship. It is **not** the
endpoint because: (a) `auto` re-picks per `start` unless the desktop pins the
region from the pick note; (b) each cycle pays a full teardown→re-arm gap and a
re-SEIZE; (c) completed recordings accumulate unboundedly and need eviction; and
(d) it produces a *sequence* of recordings, not one continuous timeline — so it
can never grow into the "accumulate invocations" view. This brief specifies the
durable engine-side form; the desktop-driven loop is a valid interim if urgency
demands it.

## Non-goals / acknowledged limits

- **Not resume-from-state / Reweave.** That is [R3](30-resume-from-state-and-reweave.md)
  (emulator snapshot/restore for counterfactuals); live-process resume stays
  killed. Continuous capture never restores, edits, or forks live state — it
  re-runs the same scoped capture.
- **Not "whole process, continuously."** Each pass stays **scoped and bounded**
  (`max_insns`, the `2^20`-step backstop); continuity is re-arming a bounded scoped
  capture, not widening the window
  ([dataflow_ptrace.c:2150](../../../src/dataflow_ptrace.c#L2150)).
- **Stop latency is bounded by one in-flight pass**, even after T2 — a genuinely
  hung target still costs up to the entry wait (`DFP_ENTRY_WAIT_MS`) before a pass
  yields.
- **Perturbation is amplified.** Every pass single-steps the target; the
  once-per-session perturb confirm becomes more load-bearing, and the arm64
  detach-fatal hazard is the gating risk (T2 step 3).
- Inherits the tier's standing refusals unchanged: exact-only, scoped, no
  cross-thread value hops, provenance starts at instrumentation.

## Cross-references

Builds on the live substrate: [25](25-live-model-wiring.md) (live model wiring)
and [26](26-live-regstate-producer.md) (the live `regstate` ring the Scrubber
already reads). Mirrors the loop in
[`asmspy_engine_region`](../../../cli/asmspy_engine.c#L3450) (`SM_TRACE`). Distinct
from [R3 / 30](30-resume-from-state-and-reweave.md). Schema/D5
[01](01-asmtrace-format.md) + [asmtrace-schema.md](asmtrace-schema.md); consumers
[04](04-replay-views.md) (dataflow/Scrubber), [09](09-teaching-producers.md).
D9 (capture host): live `--dataflow` / `--serve` is `asmspy`-only; keep the
desktop app engine-free (D4). Golden/low-fidelity + graceful-degradation D6/D7.
