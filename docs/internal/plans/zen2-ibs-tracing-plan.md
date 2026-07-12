# asm-test — Zen 2 IBS statistical tracing lane

The buildable plan for asm-test's **AMD IBS** (Instruction-Based Sampling) tracing lane —
the one hardware branch-tracing facility that actually exists on **Zen 2**, where every
branch-stack path self-skips. It turns the empirical finding in
[2026-07-12-zen2-ibs-tracing-review.md](../analysis/2026-07-12-zen2-ibs-tracing-review.md)
into work: IBS-Op delivers **statistical from→to control-flow edges, out of band, against a
live process, unprivileged**, and can observe a JIT/managed runtime without single-stepping
it (which can crash it).

It is a **sibling** of the [AMD LBR plan](amd-tracing-plan.md) (BRS / LbrExtV2 — absent on
Zen 2), the [single-step plan](zen2-singlestep-trace-plan.md) (the exact Zen 2 path), the
[hardware-trace plan](hardware-trace-plan.md) (Intel PT / CoreSight), and the
[asmspy plan](asmspy-plan.md) (the interactive tracer that gains the flagship view here).

> **Status legend: _planned_ unless a phase is marked _(landed)_; forward-look phases are
> tagged _(forward-look)_.** Update this file as phases land. The house rule holds:
> **no untested hardware code** — a lane that cannot self-validate on its target silicon
> self-skips (`available()` → 0) rather than shipping unproven. And the IBS invariant:
> **IBS is statistical and must never feed the exact `insns[]`/`blocks[]` parity
> contract** — it is a separate diagnostic producer, not a member of the fidelity cascade.

> **Landed 2026-07-12 — Phases 0 + 1** (the pure decoder + out-of-band capture engine,
> the non-TUI foundation). New surface: [include/asmtest_ibs.h](../../../include/asmtest_ibs.h),
> [src/ibs_backend.c](../../../src/ibs_backend.c) (wired into `libasmtest_hwtrace` via
> `HWTRACE_OBJS` in [mk/native-trace.mk](../../../mk/native-trace.mk)),
> [examples/ibs_probe.c](../../../examples/ibs_probe.c) (capability probe) and
> [examples/test_ibs.c](../../../examples/test_ibs.c) (synthetic-decoder unit tests +
> live out-of-band capture test), run by `make ibs-test` and folded into `make hwtrace-test`.
> The raw-record byte layout was **empirically re-confirmed on this Zen 2 host** before
> coding (PERF_SAMPLE_IP == `reg[1]` cross-check); the live test captures the spin-loop's
> back-edge out of band from a separate thread (unprivileged, `paranoid=2`). The current
> header exposes `asmtest_ibs_available`/`_skip_reason`/`_decode_op`/`_survey_pid`/`_survey_free`;
> **the `opts` `cnt_ctl`/`callchain`/`system_wide` fields are deferred to Phase 5** (declared
> here as the target surface, not yet shipped — every shipped symbol has a real, tested body).

> **Landed 2026-07-12 — Phase 2** (whole-process coverage). New symbol
> `asmtest_ibs_survey_process(pid, ms, opts, out)` in the same TU: refactors the single-tid
> capture path into a reusable per-thread **channel** (one `perf_event_open` + mmap ring per
> tid), enumerates `/proc/<pid>/task`, opens a channel per pre-existing thread, drains them
> together into one merged histogram, and does **one mid-window rescan** of `task/` to catch
> threads spawned after start (the residual born-and-dead-within-window race and the
> privileged system-wide remedy are documented in the header, not hidden). Thread count is
> capped at `IBS_MAX_CHANS` (512) to bound fd/mmap use. `examples/test_ibs.c` gains a
> whole-process test — three distinct hot loops on three worker threads, one out-of-band
> `survey_process(0, …)`, asserting the merged survey carries edges from ≥2 workers' code
> windows (something no single-tid survey could produce): reliably recovers 3/3 in practice,
> `>=2` gate for throttle-robustness. Phase 3 (the `asmspy --sample` view, now unblocked) is
> the next deliverable and is **out of scope for this pass** (TUI).

## Relationship to the existing AMD plan (Phase 7)

This plan **refines and supersedes** [amd-tracing-plan.md](amd-tracing-plan.md) **Improvement
Phase 7 — IBS-Op complementary coverage lane** *(forward-look)*, which is correct in spirit
but wrong on two mechanics that change the whole cost/benefit:

- Phase 7 says the lane "needs `CAP_PERFMON`/`CAP_SYS_ADMIN`" and reads the target via
  `MSR_AMD64_IBSBRTARGET`. **Measured false on Zen 2 / Linux 6.14:** the kernel `swfilt`
  software-filter bit (`ibs_op/format/swfilt` = `config2:0`) makes user-only IBS open at
  `perf_event_paranoid=2` **unprivileged**, and the branch **target arrives in the perf
  `PERF_SAMPLE_RAW` record** (`reg[7]` = `IbsBrTarget`, delivered whenever
  `IBS_CAPS_BRNTRGT` is set — it is here) with **no MSR access at all**.
- Everything else Phase 7 says still holds and is carried forward verbatim: IBS is
  statistical, never an ordered/complete path, never feeds parity; `rand_en` is an
  IBS-*Fetch* knob (not Op); the target is valid only for retired taken branches; the lane
  is a *separate diagnostic producer*, not a cascade rung.

When Phase 3 below lands, mark amd-tracing-plan Phase 7 as superseded-by-this-plan and drop
the `CAP_PERFMON`/MSR framing there.

## The mechanism (empirically established — the de-risking core)

All of the following was reproduced on the Zen 2 host (repro programs archived with the
review). This is the exact substrate the phases build on.

**Detection.** `ibs_op` PMU present at `/sys/bus/event_source/devices/ibs_op/type` (type 11
here; must be read at runtime — it is dynamic). `swfilt` support = the file
`.../ibs_op/format/swfilt` exists. Capabilities = `CPUID Fn8000_001B` EAX: bit 2 `OpSam`
(op sampling), bit 5 `BrnTrgt` (branch target), bit 4 `OpCnt` (`cnt_ctl`), bit 7
`RipInvalidChk`. On this box EAX = `0x3FF` (all present).

**Capture (per thread).** `perf_event_open` with:

```
attr.type          = <ibs_op type read from sysfs>
attr.sample_period = 0x4000..0x10000   /* period, NOT in config; multiple of 16 */
attr.config2       = 1                  /* swfilt (config2:0): enables exclude_kernel */
attr.config        = (1<<19)            /* optional cnt_ctl=1: sample per dispatched op */
attr.exclude_kernel= 1
attr.sample_type   = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_RAW
                     [ | PERF_SAMPLE_CALLCHAIN ]   /* optional caller context */
perf_event_open(&attr, target_tid, /*cpu*/-1, -1, 0)   /* target_tid=0 => self */
```

then `mmap` a data ring, `IOC_ENABLE`, drain `PERF_RECORD_SAMPLE` records. Each sample's
`PERF_SAMPLE_RAW` payload is `{u32 size; u64 regs[8];}`:

```
reg[1] IbsOpRip     = branch SOURCE (the retired op's RIP)
reg[7] IbsBrTarget  = branch TARGET (when IBS_CAPS_BRNTRGT)
reg[2] IbsOpData    : bit34 Return, bit35 BrnTaken, bit36 BrnMisp, bit37 BrnRet
```

A sample with `BrnRet==1 && BrnTaken==1` is a real taken edge `{IbsOpRip → IbsBrTarget}`.

**Established facts / limits** (see review §2, §6): out-of-band attach to a *separate*
same-uid pid works and does not perturb it; `inherit=1` cannot carry a per-task ringbuffer
(`EINVAL`) so whole-process needs one event+mmap per tid, or a system-wide per-cpu set
(needs `paranoid<=0`/`CAP_PERFMON`); kernel-inclusive/system-wide is `EACCES` at
`paranoid=2`; density is throttle-bounded (governor auto-lowers the sample rate under
sustained NMI cost — shortening the period does not densify).

## Design

**A separate producer, not a `asmtest_trace_backend_t` member.** The four existing hwtrace
backends produce exact `asmtest_trace_t` offsets and are cascade-eligible. IBS produces
*statistical edges*, a different data shape, so it gets its own small header/API rather than
an enum value in `asmtest_trace_backend_t` (which would also touch the ~10 hand-mirrored
bindings — the F27/F36 ABI-churn hazard — for no benefit). New surface:

```c
/* include/asmtest_ibs.h (new) — statistical IBS survey, Linux/x86-64 AMD only. */
typedef struct { uint64_t from, to; uint64_t count;
                 unsigned taken, mispred, is_return; } asmtest_ibs_edge_t;
typedef struct { asmtest_ibs_edge_t *edges; size_t n;   /* aggregated, sorted by count */
                 uint64_t samples, branch_samples, lost; /* provenance for honesty */
                 int throttled; } asmtest_ibs_survey_t;

int  asmtest_ibs_available(void);            /* 0 + skip reason off IBS/AMD/Linux   */
const char *asmtest_ibs_skip_reason(void);   /* "no ibs_op PMU", "no swfilt", ...   */
int  asmtest_ibs_survey_pid(pid_t tid, unsigned ms, const asmtest_ibs_opts_t *,
                            asmtest_ibs_survey_t *out);           /* one tid         */
int  asmtest_ibs_survey_process(pid_t pid, unsigned ms, const asmtest_ibs_opts_t *,
                                asmtest_ibs_survey_t *out);       /* all tids        */
void asmtest_ibs_survey_free(asmtest_ibs_survey_t *);
/* Pure, host-testable: raw IBS Op record bytes -> decoded edge. No hardware. */
int  asmtest_ibs_decode_op(const void *raw, size_t raw_len, asmtest_ibs_edge_t *out);
```

New TU `src/ibs_backend.c`, compiled into `libasmtest_hwtrace` (Linux-only; self-skips
elsewhere). It needs **no extra library** — raw `perf_event_open` + the existing Capstone
length-decoder only for the optional Phase 6 block normalization. Wire it into the hwtrace
object list in [mk/](../../../mk/) alongside `hwtrace.c`/`amd_backend.c`, and into
[mk/cli.mk](../../../mk/cli.mk) so asmspy can link it.

## Phases

### Phase 0 — Capability probe + availability gate + a checked-in repro *(landed)*

**Goal.** `asmtest_ibs_available()` / `asmtest_ibs_skip_reason()` and a self-skipping
`examples/ibs_probe.c` so every downstream phase can gate cleanly and CI can prove the skip.

**Work.** Read `ibs_op` PMU type from sysfs; confirm `swfilt` format file exists; read
`CPUID Fn8000_001B` EAX (`OpSam`+`BrnTrgt` required for the edge lane); check
`perf_event_paranoid`. Emit precise skip reasons: `"not AMD"`, `"no ibs_op PMU"`,
`"no swfilt (kernel < ~6.2)"`, `"no IBS_CAPS_BRNTRGT"`, `"paranoid too high for
kernel-inclusive"` (the last is informational — user-only still works). Land
`examples/ibs_probe.c` (the review's `hwprobe2.c`/`ibscaps.c` distilled) that prints the
capability table and self-skips with a `# SKIP` line off IBS.

**Acceptance.** On this Zen 2: reports available + `BrnTrgt`. On non-AMD / VM / CI: reports
the specific skip reason and exits 0 (skip, not fail). `make hwtrace-test` stays green.

### Phase 1 — Core out-of-band capture engine (`src/ibs_backend.c`) *(landed)*

**Goal.** `asmtest_ibs_survey_pid` + the pure `asmtest_ibs_decode_op`: attach IBS-Op to one
tid, drain the ring for `ms` milliseconds, decode raw records into an aggregated edge
histogram, tolerate `PERF_RECORD_LOST` (record it in `survey.lost`).

**Work.** Open the event per the mechanism above (`swfilt`+`exclude_kernel`, user-only);
mmap a data ring (default 256KB, matching the AMD backend's ring sizing); parse
`PERF_RECORD_SAMPLE` honouring the exact `sample_type` field order (IP, TID, then the RAW
block); handle ring wrap with a linear copy; decode `reg[1]/reg[7]/reg[2]` → edge; drop
`RipInvalid` ops; aggregate `{from,to}` into a hash → sorted-by-count array; set
`survey.throttled` if `max_sample_rate` was auto-lowered during capture.

**Test.** `asmtest_ibs_decode_op` is a **pure function** validated with synthetic raw
records (the same host-independent pattern as `test_amd_reconstruction`) — runs and passes
in CI on **any** host, Zen 2 or not. Live capture self-skips off IBS. On this Zen 2:
`examples/test_ibs.c` self-profiles a known control-flow fixture and asserts its hot
back-edge appears in the top edges.

### Phase 2 — Whole-process coverage (`asmtest_ibs_survey_process`) *(landed)*

**Goal.** Cover every thread of a target, not just one tid.

**Work.** Enumerate `/proc/<pid>/task`, open one event+mmap per tid, drain them together,
merge histograms; rescan `task/` once mid-window to pick up threads spawned after start
(documenting the residual race — a thread born and dead inside one window can be missed).
Document the clean fix as an opt-in: a **system-wide per-CPU** IBS event set (covers all
present+future threads) filtered to the target's pids in software — gated behind
`asmtest_ibs_opts_t.system_wide`, self-skipping with a clear reason unless
`paranoid<=0`/`CAP_PERFMON`.

**Acceptance.** A multi-threaded fixture: per-tid mode recovers the hot edges of every
pre-existing worker; the race and the system-wide remedy are documented, not hidden.

### Phase 3 — asmspy `--sample` view (the flagship deliverable) *(landed)*

> **Landed 2026-07-12 — the full `--sample` view: headless + TUI mode 7.**
> `asmspy_engine_sample(pid, ms, stop, syms, jit, sink, ctx)` in
> [cli/asmspy_engine.c](../../../cli/asmspy_engine.c) drives `asmtest_ibs_survey_process`
> **out of band** (no ptrace, no single-step) and resolves both endpoints of each hot edge
> through `asmspy_resolve` (ELF symtab → JIT perf-map), so managed frames are named. Headless
> `asmspy --sample <pid> [ms] [--json]` ([cli/asmspy.c](../../../cli/asmspy.c) `cmd_sample`)
> prints the edge histogram — `count  from -> to` with `[misp N%]`/`[ret]` tags and honest
> `branch/total samples`, `throttled` provenance — or a machine-readable JSON export; it
> **self-skips** (`# SKIP`, exit 0) off IBS. The **TUI mode 7** (menu item "7) Hot edges
> (sample)") runs the same engine on a tracer thread, showing a live hot-edge table that is
> pausable + scrollable + Tab-sortable (count / mispredicts) like the call-graph view — the
> only rich TUI view that never ptraces or single-steps (gated up front off IBS with a clear
> message). New busy victim [cli/sample_victim.c](../../../cli/sample_victim.c) (a hot
> `hot_spin()`) + a `--sample` smoke in [cli/cli_smoke.sh](../../../cli/cli_smoke.sh) prove
> the hot function is named out of band and that JSON resolves it (self-skips off IBS); the
> TUI view was driven end-to-end through a pty+pyte harness (menu → filter → mode 7 →
> Tab/space), asserting `HOT EDGES` + `hot_spin` render and the sort/pause keys work.
> Verified live on this Zen 2: both surfaces name `hot_spin`'s back-edge without perturbing
> the target — the exact case the single-step views risk crashing on a JIT.

**Goal.** A new asmspy view that shows a live statistical hot-edge / hot-block profile of
any target **out of band** — the safe view for JITs where stream/graph/tree single-step is
dangerous. This is the highest-value item and the reason to do the lane.

**Work.** Add `asmspy_engine_sample(pid, ms, stop, syms, jit, sink, ctx)` to
[cli/asmspy_engine.c](../../../cli/asmspy_engine.c) built on `asmtest_ibs_survey_process`
(no ptrace, no single-step). Resolve edge endpoints through the existing
`asmspy_resolve` ([cli/asmspy.h](../../../cli/asmspy.h)) so ELF symbols **and** JIT
perf-map methods (Node/.NET/Java) are named — the perf-map resolver already exists and this
view is where it pays off without the single-step crash risk. Add the headless `--sample`
subcommand to [cli/asmspy.c](../../../cli/asmspy.c) and a TUI **mode 7** (hot functions /
edges, sortable by sample count, with taken/mispredict rates). Extend
[cli/cli_smoke.sh](../../../cli/cli_smoke.sh) with a `--sample` smoke that self-skips off
IBS. Update the view-family table in [asmspy-plan.md](asmspy-plan.md): a new row —
mechanism "IBS-Op statistical sampling", **"safe on any target? yes (out-of-band, no
single-step)"** — the only rich view with that property.

**Acceptance.** `asmspy --sample <pid>` on a busy target prints named hot functions/edges
without single-stepping it; run against a Node/.NET process it survives and (with the
runtime's perf-map enabled) names managed frames — the exact case the single-step views
risk crashing.

### Phase 4 — Survey fallback (fixes F6) + honest tiering *(planned)*

**Goal.** Make the statistical whole-window survey produce a result on Zen 2 instead of
`EUNAVAIL`, and label statistical output so it is never mistaken for exact.

**Work.** Give `asmtest_hwtrace_sample_window_amd`
([src/hwtrace.c](../../../src/hwtrace.c):976, which returns `EUNAVAIL` when the branch-stack
`perf_open` fails at :1000) an **IBS-Op fallback** via `asmtest_ibs_survey_*`, returning a
hot-method/edge histogram flagged `fidelity = STATISTICAL`. This closes review **F6** and
feeds the F22/F26/F37 rung-discriminator work (a statistical producer must self-identify).
IBS remains **out of** `asmtest_trace_call_auto`'s exact cascade.

**Acceptance.** The survey returns a populated statistical histogram on Zen 2; the exact
cascade is untouched; no statistical result can reach the parity assertions.

### Phase 5 — Sampling-quality + call-graph enrichments *(forward-look)*

**Goal.** Better coverage per sample.

**Work.** (a) `cnt_ctl=1` (dispatched-op sampling, `config:19`) as the default — more
uniform instruction coverage than cycle-count sampling (verified available). (b)
`PERF_SAMPLE_CALLCHAIN` (`exclude_callchain_kernel=1`) to attach a frame-pointer caller
stack per sample → statistical **call-graph** edges, pairing with asmspy's `--graph`
model (verified available). (c) Period jitter to avoid aliasing against periodic loops
(note: unlike the LBR `#2A` period-spacing finding, statistical aliasing with a loop period
is a real sampling bias here, not a window-reach question).

### Phase 6 — Edge → basic-block normalization (optional) *(forward-look)*

**Goal.** Lift sampled edges to covered basic-block offsets for parity-adjacent reporting.

**Work.** Between two sampled edge endpoints, replay the target's own bytes through the
existing Capstone length-decoder (reusing the `amd_backend.c` replay idea) to mark the
straight-line blocks between waypoints as covered — explicitly **partial/statistical**
coverage (marks blocks *seen*, never blocks *not* executed). Gated on Capstone; self-skips
without it.

### Phase 7 — IBS Fetch lane (front-end coverage) *(forward-look)*

**Goal.** Complement Op's retire-side view with fetch-side coverage: `ibs_fetch` (type 10,
`swfilt`, verified working) gives fetch address + i-cache/ITLB miss + fetch-completed. Same
engine shape as Phase 1; lower priority than the edge lane.

## Test strategy

- **Host-independent (runs in all CI, incl. non-AMD):** `asmtest_ibs_decode_op` unit tests
  over synthetic raw IBS-Op records; `asmtest_ibs_available()` returns the correct skip
  reason and everything self-skips cleanly (`# SKIP` lines, `hwtrace-test` green).
- **Live, Zen 2 only:** `examples/test_ibs.c` + the `--sample` smoke, self-profiling and
  out-of-band-against-a-child fixtures asserting known hot edges appear and the target is
  unperturbed. Guarded to self-skip off IBS so it is safe in the shared CI matrix.
- **Docker:** add `docker-hwtrace-ibs` (mirroring `docker-hwtrace-amd`/`-msr` in
  [mk/docker.mk](../../../mk/docker.mk)) running the live IBS fixtures on this host's PMU —
  user-only IBS works in an unprivileged container at `paranoid=2`; the system-wide variant
  needs `--cap-add=PERFMON`. Per [CLAUDE.md](../../../CLAUDE.md), prefer this lane over host
  installs.

## Honest limitations (carry into the code + user docs)

Statistical, not exact — never proves non-execution, never feeds parity. Per-tid capture
races thread creation (until the privileged system-wide variant). Throttle-bounded density
(governor auto-lowers the rate under sustained IBS NMI cost; shortening the period does not
help). Retired-op tagging quirks (fused branches; drop `RipInvalid`). See review §6.

## Dead ends (do not attempt on Zen 2)

Direct-MSR / BPF-snapshot / 16-deep LBR (silicon absent, `EOPNOTSUPP`); legacy `DebugCtl`
1-deep LBR (needs root; GDB dropped `record-btrace bts` on AMD for its insufficiency); any
exact/ordered trace from IBS (statistical by construction); Intel PT / CoreSight (wrong
vendor/arch). See review §7.

## Sequencing

Phase 0 → 1 → 3 is the critical path to the flagship `asmspy --sample` view (the payoff);
Phase 2 and 4 harden it; Phases 5–7 are enrichment. Phase 1's pure decoder lands and is
CI-tested everywhere before any hardware-gated capture, honouring the no-untested-hardware
rule.
