# asm-test — .NET managed multi-threaded live-PT concurrency fix plan

The one remaining blocker after Intel PT was validated on real silicon (commits
`c7b4ef7`, `2d01a7f`; recorded in
[intel-hardware-validation.md](../intel-hardware-validation.md)). The
single-threaded PT paths — whole-window self-capture (`hwtrace-pt-live` 631/631)
and foreign-pid attach (`dataflow-pt-live` 29/29) — are green and stable on the
i7-8559U box. The **.NET managed multi-threaded live-PT suite is not**: once the
dotnet image carries `libipt-dev` (so the T10/T11 ambient-hop / stitched-trace
tests run live instead of self-skipping), `make hwtrace-dotnet-test` under
`--cap-add=PERFMON` becomes **non-deterministically racy**. This plan lands the
fix so that leg reaches `✅`, then re-enables `libipt-dev` in the dotnet image
(the CLAUDE.md "add the dependency where the work runs" step that was reverted so
no racy privileged lane ships).

> Status *(authored 2026-07-21)*: **NOT STARTED.** Root cause characterized but
> not yet pinned to a source line. The `libipt-dev` dotnet-image add is currently
> REVERTED on `main` (`mk/docker.mk` `DOCKER_APT_dotnet := dotnet-sdk-8.0`); it is
> re-added in P3 once the race is fixed. The affected doc rows —
> [intel-pt-whole-window-substrate](../implementations/intel-pt-whole-window-substrate.md)
> T4 (.NET inline `IntelPt` ctor) and
> [managed-wholewindow-compose](../implementations/managed-wholewindow-compose.md)
> T5/T10/T11 — stay `◐` on THIS plan, not on hardware (the box qualifies) and not
> on a missing install.

---

## Symptom, as measured

Reproduction (bare-metal Intel PT box + Docker; `sg docker -c` on this host):

```
# in mk/docker.mk: DOCKER_APT_dotnet := dotnet-sdk-8.0 libipt-dev
make docker-build-dotnet
docker run --rm --cap-add=PERFMON --security-opt seccomp=unconfined \
  -e DOTNET_TC_BackgroundWorkerTimeoutMs=36EE80 \
  asmtest-dotnet make hwtrace-dotnet-test          # run it several times
```

Observed across runs (all with the SAME image + flags — non-deterministic):

- The inline `new AsmTrace(HwBackend.IntelPt)` ctor itself works: `ok
  AsmTrace(IntelPt): armed on Intel PT silicon — captured 863180 instructions`,
  PT window EXACT (not statistical). So the arm + capture + decode is fine.
- One run **SIGSEGVs** — createdump (run WITHOUT gdb, so no signal-handler
  interference): `Crashing thread … signal 11 (000b)`, `NT_SIGINFO … signo 11
  code 0080 errno 0000 addr (nil)` — a **NULL-pointer dereference**, `SI_KERNEL`.
  The crash point VARIES run to run (once after `ok 23`, once after `ok 81`).
- Another run completed all 227 checks but flaked one timing-sensitive compose
  check: `not ok unwarmed/PT compose: >=1 method JIT'd inside the window (got 0)`.
- A gdb pass (`handle SIGSEGV nostop pass` / `handle SIGABRT stop`) caught the
  abort with the **main thread mid `ss_arm_tf`** ([ss_backend.c:353](../../../src/ss_backend.c#L353))
  reached via `asmtest_hwtrace_begin("add2")` → `asmtest_ss_begin` — i.e. a
  single-step TF window was armed on a managed thread while other threads ran
  concurrent hop work. (gdb shifts .NET signal timing, so treat this as a lead,
  not the definitive frame — the createdump crashing tid differs from the gdb
  main thread.)

The through-line: it only appears when the **concurrent, multi-threaded** T10/T11
paths run live (they spawn PT hops across async/thread-pool threads and are inert
without libipt), and it is a NULL deref that moves around — a **race**, and/or the
inherent hazard of single-stepping a managed thread while the runtime is live on
other threads. It is **not** in the single-threaded decode path this project
already fixed.

## Goals / non-goals

**Goals.** Make `make hwtrace-dotnet-test` (with `libipt-dev` + `--cap-add=PERFMON`
on a bare-metal Intel PT host) pass **stably across many consecutive runs**, then
re-add `libipt-dev` to the dotnet image and flip the gated .NET PT rows to `✅`.

**Non-goals.** Any change to the single-threaded C PT paths (`hwtrace-pt-live` /
`dataflow-pt-live`) — they are validated and must stay byte-identical. Chasing PT
on non-Intel or virtualized hosts (a real hardware gate).

## The suspect surface (where to look)

The concurrency lives in three layers; the fix depends on which one owns the NULL
deref (P0 decides):

1. **The process-global single-step handler** ([src/ss_backend.c](../../../src/ss_backend.c)).
   The SIGTRAP handler is installed once on the arm-refcount `0->1` transition and
   restored on `1->0`, under `g_ss_lock`, saving `g_old_sa`
   ([ss_backend.c:535-565](../../../src/ss_backend.c#L535)). `ss_on_sigtrap`
   ([ss_backend.c:369](../../../src/ss_backend.c#L369)) runs lock-free on each
   `#DB`, reading `g_armed`, per-thread `tls_depth`/`tls_frames`, the lock-free
   deny table (`g_deny`/`g_deny_len`), the `g_ww_alarm_fired` watchdog flag, and
   writing `f->stream[f->stream_len++]`. Candidate races: (a) the SIGALRM
   window-watchdog save/restore (`g_ww_old_sa`/`g_ww_old_it`,
   [ss_backend.c:325-347](../../../src/ss_backend.c#L325)) is process-global but
   armed/disarmed per-window — two managed threads opening/closing windows
   concurrently can interleave the `sigaction(SIGALRM)` save/restore and leave a
   stale handler; (b) `f->stream`/the frame's trace buffer freed (trace `Dispose`
   on one thread) while `ss_on_sigtrap` still writes it on another (`addr (nil)` if
   the pointer is nulled first); (c) `g_old_sa` captured/restored while the .NET
   runtime concurrently changes the SIGTRAP/SIGSEGV disposition.

2. **The single-stepping of a managed thread itself.** The compose doc already
   documents that stepping a managed body aborts CoreCLR; even stepping a *native*
   region on a managed thread perturbs that thread. If P0 shows the fault inside
   `libcoreclr`, the fix is structural: the T10/T11 live path must be **PT-only**
   (pure hardware, zero single-step) on managed threads, never arming an
   in-process TF window on a thread the runtime owns.

3. **The PT hop / codeimage sharing** ([src/hwtrace.c](../../../src/hwtrace.c)
   `pt_hop_open`/`pt_hop_close`, and `asmtest_codeimage_*`). If multiple ambient
   hops open per-tid PT captures concurrently and share a codeimage or the single
   process-global PT arm, a NULL/torn `img` or ctx at decode is possible. Verify
   the "one PT arm" invariant holds per-tid and each hop owns its ctx/img.

## Tasks

### T1 — Deterministic repro + pin the exact fault site  (S, no gate beyond the PT box)

Make the crash reproduce reliably and symbolize it.

- Add `libipt-dev` back to `DOCKER_APT_dotnet` locally; build; run
  `hwtrace-dotnet-test` in a loop (`for i in $(seq 1 30)`) under `--cap-add=PERFMON`
  and confirm the failure rate.
- Capture a core with heap and **symbolize the crashing thread** against the `-g`
  `libasmtest_hwtrace.so`:
  `-e DOTNET_DbgEnableMiniDump=1 -e DOTNET_DbgMiniDumpType=2 -e DOTNET_DbgMiniDumpName=/cores/core.%p`,
  mount a host dir at `/cores`, then in the same image
  `gdb -batch -ex 'file /usr/bin/dotnet' -ex 'core-file <core>' -ex 'thread apply all bt'`
  with `set solib-search-path /src/build`. The decisive question: is the faulting
  frame in `libasmtest_hwtrace` (→ layer 1 or 3, an asm-test bug) or in
  `libcoreclr` (→ layer 2, the managed-step hazard)?
- Record the answer here (it selects T2 vs T3 vs T4). **Done when** the fault
  site is a named function+line and the repro is ≥50% per run.

### T2 — If asm-test owns the deref: fix the concurrency/lifetime bug  (M, depends on T1 → layer 1/3)

- Serialize or make robust the identified shared state. Most likely the SIGALRM
  window-watchdog and SIGTRAP `g_old_sa` save/restore need the SAME 0↔N refcount
  discipline the handler install already uses, or the watchdog must move to a
  per-thread timer (`timer_create`/`SIGEV_THREAD_ID`) so concurrent windows don't
  race one process-global `sigaction(SIGALRM)`.
- If it is trace-buffer lifetime, the frame must not outlive its trace: the .NET
  binding must keep the trace/codeimage alive until every hop that writes it has
  disarmed (order `Dispose` after all hops closed), or the C side must copy out
  under the frame's own guard.
- **Done when** the T1 repro loop is green ×50 and `helgrind`/TSan-style reasoning
  (or a documented lock-order argument) shows the shared state is now safe.

### T3 — If CoreCLR owns the deref: make the managed live-PT path PT-only  (M, depends on T1 → layer 2)

- Audit the T10/T11 ambient/stitched path so a managed thread is **never**
  single-stepped when PT is available: the hop capture is pure `pt_hop_open`/
  `_close` (hardware AUX, no TF), and any single-step fallback is disabled on
  managed threads (the `AsmAmbientStitchedTrace` class already claims "PT-ONLY by
  construction, never in-process TF single-step" — verify that holds under
  concurrency and that no sibling test in the same process arms a TF window on a
  runtime thread while hops are live).
- If a test legitimately needs a native single-step window (e.g. the region-keyed
  `add2` case), ensure it runs on a dedicated non-runtime thread, or serialize it
  against the concurrent hop tests.
- **Done when** the repro loop is green ×50 and no managed thread is TF-stepped
  with PT present.

### T4 — Stress regression + never-flake the JIT-timing check  (S, depends on T2/T3)

- Add a stress harness that runs the concurrent ambient/stitched compose set N×
  in one process (the compose doc's own pattern) so a future regression trips CI —
  e.g. a `hwtrace-dotnet-stress` extension, or an in-suite loop gated by an env
  var, that a self-hosted PT runner runs.
- Fix the separate `unwarmed/PT compose: >=1 method JIT'd inside the window`
  flake: it must self-skip (never `not ok`) when the runtime does not JIT a method
  inside the window this run, per the compose doc's never-flake-on-timing rule —
  or force a guaranteed in-window JIT.
- **Done when** the stress harness is green across repeated runs and the timing
  check never emits `not ok`.

### T5 — Re-enable libipt in the dotnet image + validate + flip statuses  (S, depends on T4; gate: the PT box)

- Re-add `libipt-dev` to `DOCKER_APT_dotnet` ([mk/docker.mk](../../../mk/docker.mk));
  confirm the PLAIN (no-CAP_PERFMON) `docker-hwtrace-dotnet` stays green (PT prong
  self-skips on permission, not on a missing lib) and the CI lanes are unaffected.
- On the PT box run `make hwtrace-dotnet-test` under `--cap-add=PERFMON` ≥5×
  green, and the managed-compose PT prongs (T5/T11) live.
- Flip [intel-pt-whole-window-substrate](../implementations/intel-pt-whole-window-substrate.md)
  to `✅` (its C substrate is already validated; this closes T4's .NET inline arm)
  and [managed-wholewindow-compose](../implementations/managed-wholewindow-compose.md)
  T5/T10/T11 to `✅`; append the run to
  [intel-hardware-validation.md](../intel-hardware-validation.md); CHANGELOG entry.
- **Done when** those rows are `✅` and the dotnet image carries libipt with a
  stable privileged lane.

## Task order & gates

`T1 → (T2 | T3) → T4 → T5`. T1 is the fork: symbolizing the core decides whether
this is an asm-test lifetime/lock bug (T2) or the inherent managed-single-step
hazard (T3); do not guess. Everything is gated on the **bare-metal Intel PT box**
(this i7-8559U qualifies) plus the dotnet image carrying `libipt-dev` — both
installable/available here, so this is not a hardware gate, only work not yet done.

## References

- Landed PT decode fix: commits `c7b4ef7` (`fix(pt): …`), `2d01a7f` (`docs(pt): …`).
- Evidence + revert record: [intel-hardware-validation.md](../intel-hardware-validation.md).
- Single-step backend: [src/ss_backend.c](../../../src/ss_backend.c)
  (`ss_on_sigtrap` 369, arm/disarm 535/708, watchdog 325).
- PT hop / whole-window: [src/hwtrace.c](../../../src/hwtrace.c),
  [src/pt_backend.c](../../../src/pt_backend.c).
- Managed compose hazards: [managed-wholewindow-compose.md](../implementations/managed-wholewindow-compose.md).
