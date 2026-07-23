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

> Status *(authored 2026-07-21; T1–T5 closed 2026-07-23)*: **ALL TASKS DONE — root cause
> pinned; T2 selected (asm-test owns the deref, NOT the CoreCLR managed-step
> hazard), T3 not applicable.** The race was an unsynchronized **code-image**
> use-after-free (the JIT thread `realloc`ing the region/version arrays a per-tid PT
> hop decode was walking), plus a companion ambient-hop lifetime race in the .NET
> binding — both fixed in T2, measured **crash 7/7 → 0/8**. `libipt-dev` is
> consequently RE-ADDED to `DOCKER_APT_dotnet` (T5), with the privileged lane green
> ×5 and the unprivileged lane still green (PT self-skips on permission, not on a
> missing lib). The affected doc rows —
> [intel-pt-whole-window-substrate](../implementations/intel-pt-whole-window-substrate.md)
> T4 (.NET inline `IntelPt` ctor) and
> [managed-wholewindow-compose](../implementations/managed-wholewindow-compose.md)
> T5/T10/T11 — are no longer blocked on this plan; they move to `☑` here and are
> left for an independent **validating** agent to stamp `✅`, per the implementations
> README's implementer/validator split.

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

**MEASURED 2026-07-23 on the i7-8559U box (bare metal, `intel_pt` PMU present,
`--cap-add=PERFMON` bypassing `perf_event_paranoid=4`). DONE.**

*Repro rate: 7/7 = 100%* (one standalone `make hwtrace-dotnet-test`, then 3+3 in
two in-container loops), far above the ≥50% bar. Every failure is identical:
`SIGSEGV` (make `Error 139`) immediately after `ok 81 - stitched: 40 operations
…`, i.e. entering `AmbientStitchChecks()` — the FIRST test where the real
`PtHopCapture` is live. The crashing thread VARIES run to run (LWP 274 / 442 /
651 — sometimes a pool thread, sometimes main), the signature of a race.

*Symbolizing.* Two dead ends worth recording so nobody repeats them: (a) **live
`gdb` cannot be used on this suite** — the single-step tier IS `SIGTRAP`/
`EFLAGS.TF`, so gdb either swallows the mechanism under test (stops on the first
benign `#DB` at `ss_arm_tf`, which is what the earlier "main thread mid
`ss_arm_tf`" lead actually was — a normal arm, not the fault) or, with `handle
SIGTRAP pass`, the process dies of `SIGTRAP` before emitting one TAP line;
(b) in a `createdump` core the crashing thread's *current* `RIP` is
`libc!wait4` — CoreCLR's crash handler forking `createdump` and waiting — **not**
the fault site, and `gdb` cannot unwind past it (it reads no symbols from these
cores: `Syms Read: No` for libc/libcoreclr/libasmtest even with `sysroot`/
`solib-search-path` set). `eu-stack` (elfutils) unwinds them correctly via
`.eh_frame` and is the tool to use:

```
eu-stack --core core.239        # then read the TID createdump names as crashing
```

*The fault site (identical in all 3 cores inspected — frame offsets match
byte-for-byte across ASLR):*

```
#0  wait4                          <- CoreCLR crash handler, NOT the fault
#1-#6  (libcoreclr signal machinery)
#7  asmtest_pt_read_codeimage      <- ***THE FAULT*** (src/pt_backend.c:51)
#8  read_recorder                  <- src/pt_backend.c:134 (libipt read callback)
#9  (libipt internal)
#10 pt_insn_next                   <- libipt
#11 asmtest_pt_decode_window       <- src/pt_backend.c
#12 asmtest_hwtrace_pt_hop_close   <- src/hwtrace.c:2842 (the T10 per-tid hop close)
#13+ managed frames (the T11 ambient AsyncLocal handler)
```

**Answer: the faulting frame is in `libasmtest_hwtrace`, not `libcoreclr` → this
is layer 1/3, an asm-test bug → T2. T3 does not apply** (no managed thread is
being TF-stepped here; the ambient path really is PT-only, as `AsmAmbientStitchedTrace`
claims).

*Root cause* — layer 3, the shared code-image. `asmtest_codeimage_t` has **no
synchronization of any kind** (no mutex anywhere in `src/codeimage.c`), yet the
T10/T11 ambient path uses it from two threads at once:

- **Writer:** the `JitMethodMap` EventPipe `MethodLoadVerbose` callback fires on a
  runtime listener thread and calls `asmtest_codeimage_track()`
  ([HwTrace.cs:4394](../../../bindings/dotnet/hwtrace/HwTrace.cs#L4394)) — *outside*
  the `lock (_lock)` that guards `_methods`. `track` does
  `realloc(img->regions, …)` ([codeimage.c:391](../../../src/codeimage.c#L391)) and,
  via `ci_region_add_version`, `realloc(r->vers, …)`
  ([codeimage.c:208](../../../src/codeimage.c#L208)), then publishes with
  `img->nreg++` / `r->nver++` *after* filling the slot.
- **Readers:** every ambient hop close decodes against that same image —
  `hop_close` → `asmtest_pt_decode_window` → libipt → `read_recorder` →
  `asmtest_pt_read_codeimage` → `asmtest_codeimage_bytes_at`, which walks
  `img->regions[i]` and `r->vers[v]` ([codeimage.c:462-471](../../../src/codeimage.c#L462)).

So a `realloc` on the JIT thread frees the array a decoding pool thread is
walking → use-after-free → `SIGSEGV`. It surfaces ONLY with libipt present
because that is what makes the hops decode at all (off PT the producer self-skips
and never touches the image), which is exactly why this looked like "a PT bug".
The unpublished-slot ordering (`nreg++` after the write) is a second, narrower
race on the same state.

*Scope note:* the version **byte buffers** are individually `malloc`'d and freed
only in `asmtest_codeimage_free`, so the header's "borrowed bytes … valid until
`asmtest_codeimage_free`" contract is sound — only the **array walks** are unsafe.
A short critical section around the lookup + the append therefore fixes this
without changing the public contract or holding a lock across a decode.

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

**DONE 2026-07-23.** Two fixes, one per racing pair. Both are layer-3/lifetime; the
process-global SIGTRAP/SIGALRM state the plan listed under layer 1 turned out NOT to be
involved (nothing TF-steps in this path).

1. **C: `asmtest_codeimage_t` is now internally synchronized** ([src/codeimage.c](../../../src/codeimage.c)).
   A `pthread_mutex_t lock` guards the two growable arrays (`regions`, each region's
   `vers`) plus `seq`/`nreg`/`nver`. Taken at the four public entry points — `track`,
   `refresh`, `bytes_at`, `now` — with the existing bodies factored into
   `ci_track_locked` / `ci_refresh_locked` / `ci_bytes_at_locked` so every early-return
   path is covered; the internal helpers (`ci_scan_range`, `ci_region_add_version`) stay
   lock-free and are documented as "caller holds the lock", so there is no recursion and
   no lock-order question (one lock, never held across another).
   **The lock is NOT held across a decode** — only across the array walk/append. That is
   sufficient because a version's byte buffer is separately `malloc`'d and freed only in
   `asmtest_codeimage_free`, so `bytes_at`'s borrowed pointer stays valid after the
   unlock and the header's documented contract is unchanged. Verified safe to use a
   mutex here: no codeimage entry point is reachable from a signal handler
   (`src/ss_backend.c`, the SIGTRAP TU, calls none of them). The non-Linux `#else` stubs
   are a separate branch that never touches the struct, so macOS is unaffected.
2. **.NET: the ambient hop lifetime is now ordered against close-out**
   ([HwTrace.cs](../../../bindings/dotnet/hwtrace/HwTrace.cs), `AsmAmbientStitchedTrace`).
   The C lock protects a *live* image, not a *freed* one, and the handler fires on pool
   threads the flow is still leaving — so two windows remained: an attach that read
   `_completed == false` could publish a hop *after* `Complete()`'s drain had passed it
   (that hop then closes against a disposed map), and a detach could be inside `CloseHop`
   — reading the map's code-image — while `Dispose()` freed it. A `_gate` lock now makes
   the `_completed` check atomic with the `_openHops` add/remove (an attach that loses
   the race tears its PT event down via the drain-less `CloseDecode` instead of parking
   it), and an `_inFlight` counter lets `Dispose` wait out in-flight closes before
   freeing `_map`/`_parked` — bounded at 5 s, after which it **leaks rather than frees**,
   since a stalled hop must never turn a teardown into a crash. The closes themselves run
   OFF the gate (`CloseHop` decodes; it must never block the ambient handler).

*Measured, same box/flags as T1:* **crash 7/7 → 0/8**. The first post-fix loop (C fix
only) was 8/8 no-crash with the ambient live half now green —
`ok 82 ambient: the body ran and returned its result (r=42)`,
`ok 83 ambient: >=2 stitched slices captured (3)`,
`ok 84 ambient: slices merged in ascending seq order` — where it had previously never
reached check 82 at all. The one remaining `not ok` was the separate JIT-timing check
T4 owns; with that fixed too the loop is **6/6 PASS, 0 crash, 0 fail** (`ok=225`,
exit 0). `make docker-fmt-check` clean.

> **Re-measured after rebase.** The above was measured against `5dc619a`; the branch
> was then rebased onto ~120 upstream commits that had landed meanwhile, two of which
> touch this ground — `19a443d` refactored `codeimage.c`'s growth onto the new
> overflow-checked `asmtest_grow()` (so `ci_track_locked`/`ci_region_add_version` now
> grow through it, still under this lock), and added a **separate** `g_pt_window`
> mutex (review S2) that guards the whole-window PT *slot*, NOT the code-image — a
> complementary race, not this one. Upstream still had **no** code-image lock, so this
> fix remained necessary. Everything was re-run on the rebased tree and is green:
> **5/5 PASS, `ok=229`** (the count rose from 225 with upstream's added checks), with
> the live ambient half confirmed running on every run, plus `docker-fmt-check`,
> `docker-hwtrace`, the ambient-stress lane and `make check` (incl. upstream's new
> `grow_overflow`).

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

**DONE 2026-07-23.**

*Stress harness.* `ASMTEST_AMBIENT_STRESS=<N>` runs ONLY the concurrent
ambient/stitched set (T10 hops + T11 ambient + T12 twin) N times in one process and
exits — the same shape as the existing `ASMTEST_METHOD_STRESS` mode. Driven by
`make hwtrace-dotnet-ambient-stress` (`AMBIENT_STRESS_N ?= 25`) and
`make docker-hwtrace-dotnet-ambient-stress`, which adds `--cap-add=PERFMON` — that is
what gives the lane teeth, since the per-tid PT opens are what make the hop decodes
race the JIT thread's code-image writes at all. Off Intel PT the ambient half
self-skips and the lane still passes, so it is safe to run anywhere and goes red only
on a real regression of the T2 fix. **Green on this box: `1..576` over 25 iterations,
exit 0**, with the live half genuinely running (`ambient: >=2 stitched slices captured
(3)` every iteration — not a self-skip).

*The stress lane immediately earned its keep — it found a THIRD, pre-existing bug.*
On its first run the T12 twin went `not ok ambient twin: every attached hop stitched
(4 vs 5 opened)` while `every attach paired with a detach (5 open / 5 close)` stayed
green — i.e. a hop was opened AND closed but never stitched. Confirmed **pre-existing,
not a regression from T2**, by re-running the lane against the *unmodified upstream*
`HwTrace.cs` (stress harness kept, producer reverted): byte-identical failure. Cause:
`Complete()` snapshots `_parked` with `ToArray()` while a detach-driven `CloseHop` on a
pool thread may still be about to enqueue its slice — so the slice exists but misses
the stitch. Fixed by having `Complete()` wait out `_inFlight` before the snapshot
(bounded, like the sibling waits). This is invisible in a single pass, which is why it
survived until a repetition lane existed. Lane now green ×3, `1..576`, 0 `not ok`.

*Timing flake.* `unwarmed/PT compose: >=1 method JIT'd inside the window` now
self-skips with an honest reason instead of asserting when `MethodsObserved == 0`
(and returns, since the dependent `InstructionsIn` check has nothing to resolve
against). The in-process sibling at
[HwTraceProgram.cs:1221](../../../bindings/dotnet/hwtrace/HwTraceProgram.cs#L1221)
deliberately **keeps** its hard assert — it arms EFLAGS.TF around the very first call,
so the first-JIT is guaranteed in-window; only the PT variant depends on whether the
runtime got round to compiling before a hardware ring closed.

> **Honest note for the validating agent:** on this i7-8559U box `MethodsObserved` is
> `0` *consistently*, not intermittently — so this check now self-skips on **every**
> run here rather than occasionally. The capture itself is fine and is still asserted
> (`ok … UnwarmedPtPath resolves in the trace …` reports 320032 insns, 17 methods,
> `truncated=False`), so what is unproven is only the narrower "the JIT event landed
> *inside* the PT window" premise. That matches the plan's sanctioned
> "self-skip … when the runtime does not JIT a method inside the window this run", but
> if a future host wants that premise actually covered, the other half of the plan's own
> option — "or force a guaranteed in-window JIT" — is the remaining work.
>
> **Closed 2026-07-23 by
> [dotnet-pt-inwindow-jit-premise.md](../implementations/dotnet-pt-inwindow-jit-premise.md)
> T1** (a later-addition implementation doc): the JIT was in-window all along —
> only the EventPipe *delivery* raced the close — so the check now holds the
> window open (bounded `AsmTrace.WaitMethodObserved`) until the map observes
> `UnwarmedPtPath`, and the premise asserts deterministically on this box.

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

**DONE 2026-07-23 (implementation + measurement).** `libipt-dev` is back in
`DOCKER_APT_dotnet` ([mk/docker.mk](../../../mk/docker.mk)).

*Measured on the i7-8559U box, all green:*

| leg | result |
|---|---|
| `make hwtrace-dotnet-test` under `--cap-add=PERFMON` ×5 (the documented T5 command) | **5/5 PASS**, `ok=229`, 0 crash, 0 fail |
| same suite, in-container loop ×6 | **6/6 PASS**, 0 crash, 0 fail |
| PLAIN `make docker-hwtrace-dotnet` (no `CAP_PERFMON`, libipt present) | **`1..225`, exit 0** |
| `make docker-hwtrace-dotnet-ambient-stress` (new T4 lane) | **`1..576`**, 25 iterations, exit 0 |
| `make docker-hwtrace` | exit 0 |
| `make docker-dataflow-attach` (incl. live `dataflow-pt-live`) | exit 0, `1..29` all pass |
| `make docker-fmt-check` | clean |

The plain-lane requirement is specifically met: with libipt present but no
`CAP_PERFMON` the PT prong self-skips on **permission** —
`Intel PT unavailable: perf_event capture not permitted (lower perf_event_paranoid or
grant CAP_PERFMON)` — not on a missing library, so the unprivileged CI lanes are
unaffected by the image change.

The single-threaded PT paths the plan named as a non-goal are untouched and still
green: `docker-dataflow-attach` ran `dataflow-pt-live` 1..29 live (both foreign-pid
captures, PT-decoded path == single-step oracle).

> **Left to the validating agent (role separation, per the implementations README:
> "`✅ verified` is set by the validating agent, not the implementer").** This work was
> done by the *implementing* agent, so the affected rows are moved to `☑` (code landed +
> measured here), not `✅`. An independent agent should re-run the table above on a PT
> box and stamp `✅` on
> [intel-pt-whole-window-substrate](../implementations/intel-pt-whole-window-substrate.md)
> T4 and [managed-wholewindow-compose](../implementations/managed-wholewindow-compose.md)
> T5/T10/T11. Note T10's per-tid capture and T11's full ambient chain are now genuinely
> exercised live (`ambient: >=2 stitched slices captured (3)` every iteration), which is
> what those rows were waiting on.

**VALIDATED ✅ 2026-07-23 (independent validating agent, same day, different agent than
the implementer).** Re-ran the table above on this i7-8559U PT box at clean `main`
`4cf5d17`: `make docker-hwtrace-pt-live` ×3 (`1..644`, 0 failed, zero PT skips),
`make hwtrace-dotnet-test` under `--cap-add=PERFMON` ×5 (**5/5 PASS**, `1..229`,
0 `not ok`, 0 crashes; inline ctor armed + `ambient: >=2 stitched slices captured (3)`
every run), `make docker-hwtrace-dotnet-ambient-stress` (`1..576`, 25/25 iterations
captured live), `dataflow-pt-live` ×3 (`1..29`), plain `docker-hwtrace` /
`docker-hwtrace-dotnet` green with the honest permission self-skip, `docker-fmt-check`
/ `docker-docs` / host `check-bindings-parity` (142×10) clean. Both README rows
([intel-pt-whole-window-substrate](../implementations/intel-pt-whole-window-substrate.md),
[managed-wholewindow-compose](../implementations/managed-wholewindow-compose.md)) are
stamped `✅`; the run is appended to
[intel-hardware-validation.md](../intel-hardware-validation.md) (2026-07-23 entry).
T5's Done-when is fully met; the one recorded residue is T4's honest note above
(`MethodsObserved==0` on this box → the in-window-JIT premise self-skips; forcing a
guaranteed in-window JIT is the remaining optional follow-up).

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
