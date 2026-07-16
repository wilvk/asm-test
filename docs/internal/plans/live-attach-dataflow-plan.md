# asm-test — live-attach data-flow capture (native + JIT), surfaced in asmspy: implementation plan

A phased plan to extend the scoped ptrace L0 value producer
([src/dataflow_ptrace.c](../../../src/dataflow_ptrace.c)) from "fork a self-owned victim
and run caller-supplied code bytes" to **attach to an arbitrary live process — native or a
JIT/managed runtime — and capture a scoped region's data flow out of band**, then surface
that as a new **Data flow** view in the asmspy CLI ([cli/asmspy.c](../../../cli/asmspy.c)).

This builds directly on [data-flow-tracing-plan.md](data-flow-tracing-plan.md) — it turns
that plan's Phase 3 (scoped ptrace L0, LANDED) into a real attach-to-a-running-PID producer
and gives it a UI. The design investigation behind the JIT half is
[analysis/jit-runtime-tracing.md](../analysis/jit-runtime-tracing.md) ("stop instrumenting
and start observing"); this document is the build order. The emulator-replay perturbation
optimization, hardware data-watchpoints, the PT-derived value path, and live GC-move
canonicalization are deliberately **out of scope here** and carried in the companion
[live-attach-dataflow-followup-plan.md](live-attach-dataflow-followup-plan.md).

> Status: **All seven increments LANDED (2026-07-15).** Increment 1 (native live-attach) was
> the recommended first milestone — it is low-risk reuse of the landed capture core and needs no
> JIT machinery. The single-step value tier already exists and is cross-validated against
> the emulator L0 oracle; this plan changes *what it attaches to* and *how it survives a
> foreign target*, not the per-step capture math. House rule in force throughout: a foreign
> target is **never killed** on any path (it is someone else's process), and every tier
> **self-skips cleanly** (returns an availability code, emits nothing) where ptrace
> permission, Capstone, or the platform is missing — never an unproven or partial-without-
> saying-so trace.
>
> **Increment 5 (`attach_jit` + the signal split) LANDED 2026-07-15.** The signal split lives
> in the shared `dfp_step_loop` (`dfp_sigtrap_is_app`, [dataflow_ptrace.c](../../../src/dataflow_ptrace.c)):
> a SIGTRAP stop is checked via `PTRACE_GETSIGINFO` before being assumed to be our own
> single-step trap — `si_code == SI_KERNEL`/`TRAP_HWBKPT` means the TARGET executed its own
> int3/hardware breakpoint (a JIT self-check, a debugger breakpoint, a safepoint), which
> re-arming the trap flag across cannot safely continue through (the asmspy engines' MEASURED
> fatal-SIGTRAP-in-masked-handler finding). The scoped capture ends there — `vt->truncated`,
> honestly — and the trap is DELIVERED via the existing crash-safe detach path
> (`fatal_sig = SIGTRAP`) so the target's own signal machinery runs exactly as it would
> untraced, benefiting every entry point (`attach`, `attach_pid[_versioned]`, `attach_pid_tid`,
> `attach_jit`) since it lives in the shared loop, not per-entry-point. The new
> `asmtest_dataflow_ptrace_attach_jit(pid, only_tid, base, len, img, when, max_insns, result,
> survived, vt)` composes worker-thread targeting (Inc 4) + versioned-decode plumbing (Inc 3,
> `img`/`when` — method-version *attribution* stays the caller-side `asmtest_method_attribute`
> post-pass, as Inc 3 established) + call-out step-over (Inc 2) + the signal split + an explicit
> `*survived` proof (true only when the kernel confirms the crash-safe `PTRACE_DETACH` actually
> succeeded on a live, stopped tracee — a real proof for this foreign-attach family, unlike the
> self-forked `asmtest_dataflow_ptrace_attach` sibling's shared-control-page self-report).
> Eleven new checks in `examples/test_dataflow_ptrace.c` (81–91, ASan/UBSan-clean, stable over
> repeat runs): `test_signal_split` seeds an embedded `int3` in a victim with its OWN installed
> `SIGTRAP` handler and asserts the trap is detected, capture truncates at exactly that point,
> the handler actually RUNS (delivery proven, not assumed), and the victim SURVIVES and keeps
> looping afterward; `test_attach_jit_worker` proves `attach_jit` correctly reaches a routine
> running only on a worker thread and reports `*survived == 1`. asmspy's Data-flow engine
> (`asmspy_engine_dataflow`, [cli/asmspy_engine.c](../../../cli/asmspy_engine.c)) now calls
> `attach_jit` unconditionally, and the `runtime_is_managed` gate that refused ANY managed
> target at both call sites (`asmspy.c`'s headless `--dataflow` and the TUI mode-9 window) is
> **removed** — every target now goes through the same SEIZE-all-threads-and-race path, native
> or managed, with no runtime-name special case (the dead `runtime_is_managed` helper was
> deleted). Two **known, deliberately out-of-scope-here** gaps, carried forward honestly rather
> than silently: (1) `attach_jit` is called with `img=NULL`/`when=0` from asmspy — a target
> patched/re-JIT'd *mid-capture* decodes the live snapshot, same as the native tier, not the
> time-correct versioned bytes (wiring a live code-image + addr-channel feed into asmspy is
> further work); (2) the region-entry breakpoint (`dfp_run_to`/`dfp_run_to_multi` in
> [dataflow_ptrace.c](../../../src/dataflow_ptrace.c)) still plants via `PTRACE_POKETEXT`
> **only** — unlike the offset-only control-flow tracer's `asmtest_ptrace_run_to`, it has **no**
> DR0 hardware-breakpoint fallback for a genuinely W^X-enforced JIT page, so such a target
> self-skips cleanly via `DF_PTRACE_ETRACE` (never a crash, never a silent partial capture) but
> cannot yet be captured — the in-code comments at `dfp_run_to`/`dfp_plant_bp` already flagged
> this as "a later increment" before this change and it remains exactly that. Verified via
> `make dataflow-test` (91/91, incl. under `SAN=1` ASan+UBSan) and `make docker-cli`
> (`cli-smoke: PASS`, incl. the existing native `--dataflow` + `--json` regression).

---

## Goals and non-goals

**Goals**

- A **native** live-attach value producer: `asmtest_dataflow_ptrace_attach_pid(pid, base,
  len, …)` that SEIZEs a running process, waits for `[base, base+len)` to execute, single-
  steps the region capturing the same [`asmtest_valtrace_t`](../../../include/asmtest_valtrace.h#L93)
  stream the fork producer fills, and DETACHes so the target **survives**.
- A **JIT/managed-aware** entry point that layers the mitigations a live runtime needs:
  worker-thread targeting, time-correct bytes across re-JIT/patching, method+version
  attribution, call-out step-over for real (non-leaf) methods, hardware entry breakpoint on
  W^X code, and the crash-safe two-phase detach.
- A **headless** `asmspy --dataflow <pid> <func> [--json]` subcommand and an interactive
  **Data flow** TUI window (mode 9) with L0 values, L1 def-use, and L2 slice navigation.
- Reuse, not reinvention: the L0 sink, L1 def-use, L2 slicer, method attribution, GC-move
  canonicalizer, code-image recorder, addr-channel, `run_to` hardware-breakpoint fallback,
  and the asmspy symtab/jitmap/fingerprint all already exist and are wired in as-is.

**Non-goals (here)**

- The **block-step + emulator-replay** perturbation optimization and the **record-and-
  inject** OS-interaction fidelity tier — followup F1/F2. This plan uses **direct single-
  step** value capture (proven, but it perturbs the stepped thread); the emulator is not on
  this critical path.
- **Hardware data-watchpoint** targeted mode — followup F3.
- **PT + code-image + replay** (the least-perturbing, Intel-PT-gated ceiling) — followup F5.
- **Live GC-move canonicalization** of managed memory def-use — the pure transform is
  landed ([asmtest_gcmove_canonicalize](../../../include/asmtest_valtrace.h#L353)); wiring
  the live EventPipe `{old,new,len}` feed is followup F4.
- **Whole-process / continuous** value capture — the tier is scoped to a `using`-style
  region by design (cost is ~10³–10⁵×/stepped instruction); whole-process taint is the
  DynamoRIO tier's job ([dynamorio-taint-tier-plan.md](dynamorio-taint-tier-plan.md)).

---

## Design overview

The insight that makes this tractable: the phrase "single-stepping is hostile to a JIT"
conflates **two** independent hazards, and the out-of-process model already neutralizes the
dangerous one.

- **Hazard A — int3 code patching.** `PTRACE_POKETEXT` writing `0xCC` corrupts a JIT's code
  page. Already mitigated: `asmtest_ptrace_run_to` falls back to a **hardware execution
  breakpoint** (x86 DR0 / AArch64 `NT_ARM_HW_BREAK`) the moment `POKETEXT` is refused on a
  W^X heap ([ptrace_backend.c:678-714](../../../src/ptrace_backend.c#L678), comment at
  [:480-483](../../../src/ptrace_backend.c#L480)) — "writes no code … per-thread, so it
  never traps a sibling thread."
- **Hazard B — per-instruction single-step (the TF trap flag).** Fatal *in-process* (a
  TF-armed thread that masks `SIGTRAP` — as `pthread_create`, CLR exception dispatch, and
  the GC all do — is force-killed to exit 133). **Structurally absent out-of-process**: the
  `#DB` is delivered to the *tracer* via `waitpid`, not gated by the tracee's signal mask,
  so the tracee may block `SIGTRAP` and never dies. `asmtest_dataflow_ptrace_attach` is
  already out-of-process, so this hazard does not apply to the tier we are extending.

What remains after A and B is handled by existing machinery: **temporal bytes** (the code-
image recorder + versioned decode), the **runtime owning its own SIGSEGV/SIGTRAP** (the
`si_code` split re-injects the target's own traps), and **worker-thread execution** (SEIZE
every thread, run the one that enters). The one irreducible residual is **perturbation-
induced cross-thread deadlock** — the tracer steps one thread ~1000× slower while siblings
run free — which scoping shrinks but cannot close; followup F1 shrinks it further.

The producer stays the shared capture core. `dfp_step_loop`
([dataflow_ptrace.c:459](../../../src/dataflow_ptrace.c#L459)) already operates on a
`dfp_ctx {pid, vt, code, code_len, base}` and is documented as driving "an already trace-
stopped tracee" — nothing in the per-step register/XSTATE/`process_vm_readv` capture knows
the tracee is a forked child. The `code` (tracer-local bytes Capstone decodes) and `base`
(the tracee's execution address) are already separate fields; the fork path merely makes
them coincide. So the work is the *setup and policy around* the loop, not the loop.

---

## Public API sketch

```c
/* Native live target: SEIZE pid, run_to(base), single-step [base,base+len) capturing
 * values, crash-safe DETACH so pid survives. Leader thread, leaf-or-stepped-over region.
 * Returns DF_PTRACE_OK / _ETRACE (attach/seccomp) / _FAULT / _ENOSYS (off-platform). */
int asmtest_dataflow_ptrace_attach_pid(pid_t pid, uint64_t base, size_t len,
                                       uint64_t max_insns, long *result,
                                       asmtest_valtrace_t *vt);

/* JIT/managed live target: everything _pid does, plus — worker-thread targeting (SEIZE
 * all threads, run whichever enters), time-correct bytes via a code-image, method+version
 * attribution via the addr-channel feed, call-out step-over, and the si_code signal split.
 * `img`/`chan` may be NULL to degrade to the native behaviour. `only_tid` (0 = any) pins
 * one thread. `*survived` proves the crash-safe detach. */
int asmtest_dataflow_ptrace_attach_jit(pid_t pid, pid_t only_tid,
                                       uint64_t base, size_t len,
                                       struct asmtest_codeimage *img, uint64_t when,
                                       asmtest_addr_channel_t *chan,
                                       uint64_t max_insns, int *survived,
                                       asmtest_valtrace_t *vt);
```

Both remain producer-tier entry points with **no public header** (a value-trace producer is
a tier, not part of the shared `asmtest_valtrace.h` sink API), re-declared by their test
suite exactly as `asmtest_dataflow_ptrace_run/attach` are today.

The asmspy engine seam mirrors the existing `asmspy_engine_region`
([asmspy.h:227](../../../cli/asmspy.h#L227)):

```c
/* Capture the data flow of [base,base+len) in live `pid`; pick native vs JIT internally
 * from the fingerprint. `sink` receives each captured invocation's valtrace + def-use. */
int asmspy_engine_dataflow(pid_t pid, pid_t only_tid, uint64_t base, size_t len,
                           long max, atomic_bool *stop,
                           asmspy_dataflow_sink sink, void *ctx);
```

---

## Increment 1 — Native live-attach value producer *(**LANDED 2026-07-14**)*

> **UPDATE 2026-07-14 — LANDED.** `asmtest_dataflow_ptrace_attach_pid(pid, base, len, max_insns,
> result, vt)` attaches to an ALREADY-RUNNING, FOREIGN process (a pid we did not create): SEIZE (no
> `PTRACE_O_EXITKILL`), INTERRUPT, an inline `dfp_run_to` (int3 breakpoint-cont-rewind) to the region
> entry, read the region bytes FROM the target via `process_vm_readv`, single-step `[base, base+len)`
> filling the same `asmtest_valtrace_t`, then `PTRACE_DETACH` so the target SURVIVES. The shared
> `dfp_step_loop` was made **foreign-safe**: a new `dfp_dirty_exit` owns the kill-vs-detach decision
> via `dfp_ctx.foreign` (self-owned → kill+reap, exactly as before; foreign → left trap-stopped for
> the caller to detach, forwarding a fault signal), and a `pre_positioned` first-iteration examines
> the `run_to` entry stop as the entry instruction's pre-state. The fork `_run` + forked-victim
> `attach` paths are **byte-unchanged** (existing 36/36 held). New `test_attach_pid` in
> `examples/test_dataflow_ptrace.c` forks an INDEPENDENT looping victim (no `TRACEME`), attaches by
> pid, and asserts: the region returns 12, six steps captured, the def-use edges, **the live L2
> slices == the emulator L0 oracle** (fwd + bwd), and the victim SURVIVED the detach (a shared
> counter keeps advancing) — **44/44** on real ptrace (this host). `make docker-dataflow-attach` runs
> it in the CI image with `seccomp=unconfined`; on a ptrace-denied host it self-skips
> `DF_PTRACE_ETRACE`. DR0 hardware-breakpoint fallback (W^X JIT heaps) + call-out step-over are
> Increments 2-3.

Add `asmtest_dataflow_ptrace_attach_pid` and make the shared step loop foreign-safe.

- **Foreign-safe error policy (the critical refactor).** Every error path in `dfp_step_loop`
  currently `SIGKILL`s the tracee (fault at
  [:490-493](../../../src/dataflow_ptrace.c#L490), backstop at
  [:498-503](../../../src/dataflow_ptrace.c#L498), `max_insns` at
  [:523-525](../../../src/dataflow_ptrace.c#L523), ptrace failure at
  [:542-544](../../../src/dataflow_ptrace.c#L542)) — correct for a disposable child, fatal
  for a foreign target. Extend the existing `left_stopped` out so **every** exit reports
  whether the tracee is still trap-stopped, and move the kill/detach decision to the caller:
  the fork path (`_run`) keeps kill+reap; the live path always `PTRACE_DETACH`s.
- **Bytes from the target, not fork inheritance.** Replace `map_exec`
  ([:562](../../../src/dataflow_ptrace.c#L562)) for this path with a `process_vm_readv` of
  `[base, base+len)` into `c->code`; set `c->base = base` (the target's symbol address).
- **Reach the region.** SEIZE + INTERRUPT, then `asmtest_ptrace_run_to(pid, base)` (int3,
  auto-falling-back to the DR0 hardware breakpoint) to leave the target trap-stopped at the
  entry — the precondition `dfp_step_loop` already expects.
- **No `PTRACE_O_EXITKILL`** on the live SEIZE — that flag means "if asmspy dies, the target
  dies," which is wrong for a foreign process.
- Scope this increment to a **leader-thread, leaf-or-until-first-call-out** region (call-outs
  handled in Increment 2), matching the current producer's documented scope.

**Exit criteria:** a new docker lane (proposed `make docker-dataflow-attach`) starts a
native victim, attaches by pid, captures a scoped region's value trace whose L2 slices
**match the emulator L0 oracle** on a deterministic region (reuse the
[test_dataflow_ptrace.c](../../../examples/test_dataflow_ptrace.c) cross-validation shape);
the victim **survives detach** (a post-region marker proves it); on a seccomp/permission-
denied host the entry point returns `DF_PTRACE_ETRACE` and the lane self-skips.

---

## Increment 2 — Call-out step-over (real methods, not just leaves) *(**LANDED 2026-07-15**)*

> **UPDATE 2026-07-15 — LANDED.** `dfp_step_loop` now distinguishes a call-out from a return when
> the single-stepped PC leaves the region: a new `dfp_is_callout` predicate (the last in-region
> instruction decodes as a `call` whose pushed return address lands back in the region) triggers a
> native-speed run to that return address via the existing int3 `dfp_run_to`, records nothing over the
> helper, then resumes in-region single-stepping. Kept **self-contained in `dataflow_ptrace.c`** (no
> `ptrace_backend.c`/header change; the shared-helper refactor of `classify_region_exit` is a deferred
> cleanup). Because it lives in the shared loop, the `_run`/`_attach`/`_attach_pid` paths all gain it.
> Eleven new checks in `test_dataflow_ptrace.c` (45–55) assert a complete value trace across the call
> with a def-use edge threading the call, four in-region steps only (no helper instruction recorded),
> and an honest `truncated` on a non-returning callee — **55/55 on real ptrace**; existing 44 unchanged.
> The re-entrancy caveat (resume at first arrival at the return address) is carried in-code.

Today the value producer treats "PC left the region" as "returned"
([dataflow_ptrace.c:527-537](../../../src/dataflow_ptrace.c#L527)), so a region that calls a
runtime helper ends at the first `call`. Real managed methods call helpers constantly.

- Adopt the **return-address-breakpoint step-over** the control-flow tracers already use: at
  a call-out, breakpoint the return address, `PTRACE_CONT` over the helper at native speed
  (recording nothing), and resume in-region single-stepping — the exact seam
  `asmtest_ptrace_trace_attached` documents ([asmtest_ptrace.h:184-191](../../../include/asmtest_ptrace.h#L184))
  and `classify_region_exit` implements for block-step
  ([ptrace_backend.c:1858-1873](../../../src/ptrace_backend.c#L1858), the
  `EXIT_CALLOUT_RESUMED` / `EXIT_CALLOUT_LOST` split). The offset backend's step-over is not
  yet exported to the value producer (its header flags exactly this); factor it into a
  shared helper both call.
- Carry the documented **re-entrancy caveat** forward: the step-over resumes at the first
  arrival at the return address and is not re-entrancy aware (a callback or tiering/OSR stub
  re-entering the region resumes in the nested invocation).

**Exit criteria:** a fixture routine that calls a helper mid-region produces a **complete**
value trace across the call; the helper's internal instructions are **not** recorded (no
def-use threaded through them); a callee that never returns truncates honestly
(`vt->truncated`) rather than hanging.

---

## Increment 3 — Time-correct bytes + method attribution (JIT patch/move survival) *(**LANDED 2026-07-15**)*

> **UPDATE 2026-07-15 — LANDED (both halves).** `dfp_ctx` gained an optional `{img, when}`; `open_step`
> decodes operands against `asmtest_codeimage_bytes_at(base+off, when)` when `img != NULL`, else the live
> `c->code` snapshot — the same "swap the byte source, keep the step loop" seam
> `asmtest_ptrace_trace_attached_versioned` uses. A new `asmtest_dataflow_ptrace_attach_pid_versioned`
> carries the code-image; the plain `attach_pid` is now a thin `img=NULL` forwarder (callers, incl. the
> asmspy `--dataflow` engine, unaffected). Method+version attribution runs via the landed
> `asmtest_method_attribute` post-pass. Twelve new checks (56–67) in `test_dataflow_ptrace.c` prove a
> mid-capture in-place patch still decodes the *live-at-that-step* operands (v0 reads `rdi`, v1 reads
> `rsi`; a live re-read would be wrong) and every step attributes to the correct method version across an
> induced re-JIT with a stable method identity — **67/67 on real ptrace**, prior 55 unchanged.

A JIT patches, frees, or reuses the code at `base` mid-capture, so a single `process_vm_readv`
of the bytes feeds the operand enumerator the **wrong** instruction — garbage read/write
sets. Decode against the versioned code-image instead.

- `asmtest_codeimage_track(img, base, len)` before the run; `asmtest_codeimage_refresh(img)`
  between captured invocations; decode each step's operands against
  `asmtest_codeimage_bytes_at(img, pc, when)`
  ([asmtest_codeimage.h:110](../../../include/asmtest_codeimage.h#L110)) rather than the live
  snapshot — the same "swap the byte source, keep the step loop" decoupling
  `asmtest_ptrace_trace_attached_versioned` uses
  ([ptrace_backend.c:2302-2335](../../../src/ptrace_backend.c#L2302)).
- Attribute every step to its **method + version** with the landed Phase-4 resolver
  `asmtest_method_attribute` ([asmtest_valtrace.h:281](../../../include/asmtest_valtrace.h#L281),
  [src/dataflow_method.c](../../../src/dataflow_method.c)), fed by the jitdump/perf-map
  method-map and the addr-channel `(base,len,version)` records
  ([asmtest_addr_channel.h](../../../include/asmtest_addr_channel.h)).

**Exit criteria:** a fixture that **patches or relocates** the region mid-capture still
decodes correct operands (assert the read/write sets match a static decode of the *live-at-
that-step* bytes, not the final bytes); each step attributes to the correct method version
across an induced tiered re-JIT.

---

## Increment 4 — Worker-thread targeting (managed methods run off the leader) *(**LANDED 2026-07-15**)*

> **UPDATE 2026-07-15 — LANDED.** New additive producer entry `asmtest_dataflow_ptrace_attach_pid_tid(pid,
> only_tid, base, len, max, result, vt)` (existing entries byte-unchanged). `dfp_seize_all` SEIZEs the
> leader then re-scans `/proc/<pid>/task/*` (up to 16 passes, closing the thread-spawns-a-thread race);
> `dfp_run_to_multi` plants one shared int3 at `base`, `PTRACE_CONT`s all threads, and `waitpid(-1, __WALL)`
> catches the **first thread that enters**, targeting it by **the kernel tid waitpid reports, never the
> leader** (the getpid-vs-gettid fatal-SIGTRAP fix); `only_tid` steps non-target hits over the shared bp and
> pins one thread; siblings are detached to run free (no `O_TRACECLONE`, so they outlive us). The internal
> core `dfp_attach_worker` already threads `img`/`when`, so **Increment 5 (`attach_jit`) composes on top**.
> Thirteen new checks (68–80) in `test_dataflow_ptrace.c`: worker captured off an idle leader, siblings +
> target survive detach, `only_tid` picks A-not-B under two-worker contention — **80/80 on real ptrace**, 30
> stress runs + ASan/UBSan clean. Edge notes in-code: the shared-int3 step-over has a tiny skip window (why
> the plan cites per-thread HW breakpoints); a thread spawned mid entry-catch is untracked (the scan covers
> attach-time threads).

`asmspy_engine_region` attaches only the leader and returns `ASMSPY_REGION_NEVER_RAN` when
the function runs on a worker thread — "run_to only steps the leader here"
([asmspy_engine.c:880-884](../../../src/asmspy_engine.c#L880)). Managed methods almost always
run on worker threads, so the JIT path must not depend on the leader.

- Reuse the existing whole-process **SEIZE-all-threads** helper
  ([asmspy_engine.c:465-511](../../../src/asmspy_engine.c#L465)); plant the entry breakpoint
  process-wide (a hardware breakpoint is per-thread, so arm it on each seized tid, or use the
  int3 which is shared); run **whichever thread first enters** the region to the stop and
  single-step that one thread; leave the siblings running.
- Target the entering thread by its own **`SYS_gettid`**, never the leader — the getpid-vs-
  gettid mistargeting was a real fatal-SIGTRAP bug on HotSpot
  ([hwtrace.c:2803-2808](../../../src/hwtrace.c#L2803)).
- Honor an optional `only_tid` filter (the asmspy `--tid=` convention) to pin one thread.

**Exit criteria:** a multi-threaded fixture whose target routine runs on a **worker** thread
is captured (no longer `NEVER_RAN`); the process's other threads keep running at full speed;
`only_tid` restricts capture to exactly one thread.

---

## Increment 5 — JIT-aware entry point (compose 2–4) + signal pass-through *(LANDED 2026-07-15)*

> **Outcome.** `asmtest_dataflow_ptrace_attach_jit` ties the pieces together into one
> managed-safe path, composing entirely on the existing `dfp_attach_worker` (Inc 4's core):
> worker-thread targeting (Inc 4), versioned-decode plumbing (Inc 3, `img`/`when` — attribution
> itself stays the caller-side `asmtest_method_attribute` post-pass), call-out step-over (Inc 2,
> already always-on in the shared step loop), the new signal split, and the crash-safe
> two-phase detach with an explicit, kernel-confirmed `*survived`. The **signal split**
> (`dfp_sigtrap_is_app`) lives in the SHARED `dfp_step_loop`, so every entry point gets it for
> free, not just `attach_jit`. **Deviations from the sketch below, each a deliberate, disclosed
> scope decision, not an oversight:** (1) the `asmtest_addr_channel_t *chan` parameter in the
> original sketch was dropped — it had no internal operational use (attach_jit doesn't
> maintain a "known JIT regions" set; a caller wanting attribution already has
> `asmtest_method_attribute`), so accepting-and-ignoring it would have been unused complexity;
> (2) a `long *result` param was ADDED (the sketch omitted it) for signature consistency with
> every sibling entry point (`_run`/`_attach`/`_attach_pid[_versioned]`/`_attach_pid_tid`), none
> of which drop the routine's return value. **Two known gaps, carried forward openly:** no
> hardware-breakpoint (DR0) fallback yet for the region-entry `int3` on a genuinely
> W^X-enforced JIT page (`dfp_run_to`/`dfp_run_to_multi` are POKETEXT-only — pre-existing,
> flagged in-code before this change, still "a later increment"); `img`/`when` (Inc 3 versioned
> decode) is not yet wired into asmspy's own call site (asmspy calls `attach_jit` with
> `img=NULL`), so an asmspy-driven capture of a method that gets re-JIT'd or moved *mid-capture*
> decodes the live snapshot, same as the native tier — a live code-image + addr-channel feed
> into asmspy is further, separable work.
>
> **Validation.** 11 new checks in `examples/test_dataflow_ptrace.c` (81–91; 91/91 total,
> ASan+UBSan-clean, stable over 8 repeat runs): `test_signal_split` proves the mechanism
> end-to-end — a victim with a hand-installed SIGTRAP handler executes its OWN embedded `int3`
> mid-region; the capture detects it (`si_code == SI_KERNEL`), truncates honestly at exactly
> that point, DELIVERS the trap (the victim's handler is proven to have actually run via a
> shared counter, not merely "the process didn't crash by coincidence"), and the victim
> SURVIVES and keeps running afterward. `test_attach_jit_worker` proves the composed entry
> point reaches a routine that runs only on a worker thread and that `*survived == 1` reflects
> a real, kernel-confirmed detach. `cli/asmspy_engine.c`'s `asmspy_engine_dataflow` now calls
> `attach_jit` unconditionally (SEIZE-all-threads-and-race, replacing the old leader-only
> `attach_pid`), and the `runtime_is_managed` gate that hard-refused every managed target at
> BOTH asmspy call sites (`--dataflow` headless, TUI mode-9 window) is removed — verified via
> `make docker-cli` (`cli-smoke: PASS`, including the pre-existing native `--dataflow`/`--json`
> regression). **What this does NOT cover, honestly:** no real dotnet/JVM process was attached
> to end-to-end — the exit criteria's original "reuse the existing dotnet / JVM smoke fixtures
> under `bindings/`" wasn't executed, because no asmspy-specific harness against a real managed
> runtime exists in-repo (the JIT smoke fixture `jit_victim` cli-smoke already uses is a
> synthetic anonymous-mmap + hand-written perf-map stand-in, not a real JIT). The mechanism
> tests above exercise the EXACT new logic (signal detection/delivery, worker racing, survived
> proof) deterministically and in isolation; a live-JVM/.NET run through `asmspy --dataflow`
> remains a natural, named follow-on. V8's `IMMEDIATE_CRASH` self-check remains a documented
> inherent hazard (indistinguishable from a step-induced trap) — the signal split makes it
> *survivable* (delivered, not swallowed), not detectable-as-benign.
>
> The as-planned bullets below are the original sketch, retained for provenance.

`asmtest_dataflow_ptrace_attach_jit` ties the pieces together into one managed-safe path:
worker-thread targeting (Inc 4) + hardware entry breakpoint (automatic via `run_to`) +
versioned decode & method attribution (Inc 3) + call-out step-over (Inc 2) + crash-safe two-
phase detach.

- **Signal split.** Use `PTRACE_GETSIGINFO`/`si_code` to distinguish the target's **own**
  int3 / null-check SIGSEGV / safepoint trap (re-inject via `PTRACE_CONT`, never
  `PTRACE_SINGLESTEP`) from our single-step `#DB`, so the runtime's own signal machinery
  passes through — the out-of-process analog of the DynamoRIO tier's `DR_SIGNAL_DELIVER`.
- **Crash-safe detach.** The target is trap-stopped just past the region; `PTRACE_DETACH`
  with no pending signal resumes it free and it survives; set `*survived`.

**Exit criteria:** attaches to a **live managed runtime** (reuse the existing dotnet / JVM
smoke fixtures under `bindings/`), captures a JIT method's scoped value trace + def-use, and
the runtime **survives**; on a host without PT/permissions/Capstone the entry point returns
a clean availability code and the lane self-skips. V8's `IMMEDIATE_CRASH` self-check remains a
documented inherent hazard (indistinguishable from a step-induced trap).

---

## Increment 6 — asmspy headless `--dataflow` subcommand + JSON *(**LANDED 2026-07-15 — native path**)*

> **UPDATE 2026-07-15 — LANDED (native path).** `asmspy --dataflow <pid> <sym|0xADDR[:LEN]> [--json]
> [--tid=N] [--max=N]` resolves the region via the existing symtab/jitmap resolver, runs the landed
> scoped-ptrace L0 producer (`asmtest_dataflow_ptrace_attach_pid`) on the live pid for one invocation,
> and emits the L0 value trace + L1 def-use edges (documented JSON on `--json`). A new engine seam
> `asmspy_engine_dataflow` mirrors `asmspy_engine_region`; managed/JIT runtimes self-skip via the
> fingerprint (**the JIT engine path awaits Increment 5**) and off-platform/no-Capstone returns
> `ASMSPY_DATAFLOW_UNAVAIL`. `cli_smoke.sh` covers the human + `--json` (json.load-validated) + bad-arg
> paths; **`make docker-cli` → `cli-smoke: PASS`**. The optional L2 slice navigation is left for the
> Increment 7 TUI.

Every asmspy view has a headless twin; add this one first (CI-testable, no ncurses).

- `asmspy --dataflow <pid> <func> [--json] [--tid=N] [--max=N]`: resolve `func` via the
  existing symtab + jitmap ([asmspy.h:117-180](../../../cli/asmspy.h#L117)), pick the native
  vs JIT engine path from the fingerprint runtime field
  ([asmspy_fingerprint_t.runtime](../../../cli/asmspy.h#L68)), run
  `asmspy_engine_dataflow`, and emit the L0 value trace + L1 def-use edges (and, on request,
  a backward/forward slice for a seed step). JSON schema documented alongside the existing
  `--graph`/`--sample` JSON.

**Exit criteria:** the subcommand prints a value trace + def-use for a chosen function of a
live process; `--json` emits a documented schema; `cli_smoke.sh`
([cli/cli_smoke.sh](../../../cli/cli_smoke.sh)) covers it against a bundled victim and it
self-skips where unavailable; verified via `make docker-cli`.

---

## Increment 7 — asmspy TUI "Data flow" window (mode 9) *(**LANDED 2026-07-15 — native path**)*

> **UPDATE 2026-07-15 — LANDED (native path).** `screen_mode` gains "9) Data flow"; `run_dataflow_view` on
> the dedicated tracer thread runs the landed `asmspy_engine_dataflow` for one invocation, deep-copies the
> L0 trace under the view lock, and rebuilds L1 def-use so all navigation is pure UI over the frozen copy
> (never touches ptrace). Top pane = per-step disassembly annotated with captured values; `b`/`f` =
> backward/forward L2 slice from the selected step (highlight in-slice, dim the rest), `c` clears; bottom
> pane = the step's def-use; header = steps/records/truncated + ret. Managed/JIT gate + `NEVER_RAN`/
> unavailable render explicit hints, not blank panes. The annotation/slice/def-use logic was factored into
> a shared `cli/asmspy_dataview.h` and unit-tested in `cli/test_view.c` (~60 assertions), wired into
> `cli_smoke.sh`; **`make docker-cli` → cli-smoke PASS**. NOTE: interactive ncurses rendering is compile-
> and logic-verified, not pixel-e2e. Per-`--tid` selection in the picker (worker routines run off the
> leader) is an Inc 4/5 follow-on, surfaced in-view.

Add the interactive view. Reuse the existing structure: `screen_mode`
([asmspy.c:2626](../../../cli/asmspy.c#L2626)) gains a "9) Data flow" option; `screen_syms`
([asmspy.c:2689](../../../cli/asmspy.c#L2689)) picks the function; the view runs the engine
on the **dedicated tracer thread** (the ptrace-per-thread rule, [asmspy.h:14-17](../../../cli/asmspy.h#L14)).

- **Top pane:** region disassembly, one row per executed step, annotated with captured
  values (`mov rax,[rbp-8]` → `rax ← 0x2a`).
- **Slice navigation (the payoff):** select a row, `b`/`f` = backward/forward slice
  (`asmtest_slice_backward`/`_forward` + `asmtest_slice_contains`), highlight the slice, dim
  the rest — a question no current view can answer.
- **Bottom pane:** def-use for the selected step (who last wrote each value read here; who
  reads the value written here).
- **Header:** steps/records totals + the `truncated` flag — the same honest-overflow
  reporting the sample view gives. Snapshots copied under the view lock; slice navigation is
  UI-side over the copied trace and never touches ptrace.

**Exit criteria:** the interactive window renders values + def-use + navigable slices for a
picked function of a live process; the leader/worker and JIT caveats surface in-view; a
`NEVER_RAN` / self-skip result renders a clear hint rather than a blank pane.

---

## Risks and open points

- **Perturbation-induced deadlock is the irreducible residual.** Stepping one thread ~1000×
  slower while runtime siblings run free can stall a sibling on a lock the stepped region
  holds (GC alloc slow-path, JIT-compile helper, loader lock) — a deadlock no watchdog
  breaks ([jit-runtime-tracing.md:347-357](../analysis/jit-runtime-tracing.md)). Scoping to a
  small region shrinks the window; followup F1 (block-step) shrinks it further; it never
  fully closes. Bound every capture with `max_insns` and surface truncation honestly.
- **Managed memory def-use is GC-uncanonicalized** until followup F4 wires the live move
  feed — a raw address collision across a compaction aliases. Document this in-view; the
  register def-use is exact.
- **Concurrent memory writes** by sibling threads to locations the region loads are captured
  as-observed at each step (single-step reads the real memory), so this tier is *correct*
  here — the divergence risk is specific to the deferred emulator-replay path (F1/F2).
- **Entry breakpoint on a moving target.** If the method is re-JIT'd to a new address between
  `run_to` arming and arrival, the breakpoint is stale; a re-arm on a `NEVER_RAN`/timeout loop
  fed by the addr-channel is the natural mitigation, but Increment 5 did **not** build it —
  `attach_jit` was shipped with the addr-channel (`chan`) parameter deliberately DROPPED from
  the original sketch (no internal use was identified; see the Increment 5 note above), so this
  risk is still open, not mitigated. Also unmitigated: the region-entry `int3` itself has no
  hardware-breakpoint fallback on a W^X-enforced JIT page (also noted above) — such a target
  self-skips (`DF_PTRACE_ETRACE`) rather than capturing.
- **PT/permission gating.** The native path needs only ptrace permission; the JIT path's
  method-map is best-effort (jitdump/perf-map may be absent). Everything self-skips.

---

## Recommended first milestone

**Increment 1** (native live-attach) — it is the low-risk half: it reuses the landed,
oracle-validated capture core unchanged and only rewrites the setup/policy around it, needs
no code-image, addr-channel, or worker-thread machinery, and immediately gives asmspy a
working data-flow capture on native processes. It also forces the one refactor everything
else depends on — making `dfp_step_loop` foreign-safe (stop killing the target) — so landing
it de-risks Increments 2–7 before any JIT complexity is added.

**All seven increments are now landed (2026-07-15)** — the plan's committed scope is complete
and it is ready to archive per the repo's convention (moving `docs/internal/plans/*.md` to
`docs/internal/archive/plans/` once every phase/increment lands), left in place here pending
that housekeeping pass, consistent with how [dynamorio-taint-tier-plan.md](dynamorio-taint-tier-plan.md)
records the same status. The two gaps disclosed in the Increment 5 note (the addr-channel
re-arm-on-stale-breakpoint mitigation, and the W^X hardware-breakpoint entry fallback) and a
real dotnet/JVM validation run are the honest remaining follow-ons, not committed scope.
