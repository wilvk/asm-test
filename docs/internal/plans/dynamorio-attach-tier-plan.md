# asm-test - DynamoRIO ATTACH tier (in-band data-flow / taint of an already-running native process): implementation plan

Take the in-band DynamoRIO data-flow / taint tier — today a **launch-under-DR** producer
(`drrun -c libasmtest_drtaint_client.so -- <app>`, DR owns the process from a clean start;
[dynamorio-taint-tier-plan.md](dynamorio-taint-tier-plan.md), Increments 4-5 LANDED) — and
make it **attach to a process that is already running**, instrument a scoped region in
band, drain the results out of process, and **detach**, leaving the target running native.

This is the *third* DR integration model, and the one the taint tier deliberately did not
use ([data-flow-capture.md:211-241](../analysis/data-flow-capture.md#L211)):

| Model | Mechanism | Who owns it | Cost | Status |
|---|---|---|---|---|
| In-process cooperative | `dr_app_setup`/`dr_app_start` | the process, on itself | ~10-50x (DBI) | shipped (`dr_valtrace`/`dr_taint`) |
| Launch-under-DR | `drrun -c <client> -- <app>` | DR, from a clean start | ~10-50x (DBI) | LANDED (taint Inc 5) |
| **External attach** | `dr_inject_process_attach(pid)` / `drrun -attach <pid>` | **an injector, into a live PID** | **~10-50x while armed, ~0 detached** | **this plan** |

**Why now, and why native.** The analysis conclusion is explicit: DR-attach is *credible
for native already-running targets* — it keeps the DBI overhead band and, once detached,
costs the target nothing — while a clean **managed** attach "really wants safepoint
coordination … parking managed threads at GC-safe points before takeover"
([data-flow-capture.md:227-240](../analysis/data-flow-capture.md#L227)). So the taint
plan's [rejection of external attach](dynamorio-taint-tier-plan.md) was scoped to **managed
processes**; for a **native** process it is the *right* tool — the only in-band model that
can data-flow a program you did not start and then let go of it. The pragmatic split the
analysis lands on is the frame for this whole plan: **launch-under-DR (or ptrace) for
managed; DR-attach for native; ptrace-attach + emulator replay when the value cost must
stay off the live thread** ([data-flow-capture.md:240](../analysis/data-flow-capture.md#L240)).

**What is already built and reused unchanged** (this is why the tier is mostly net-new
*wiring*, not net-new capture). The in-band client is attach-agnostic: it instruments
whatever DR hands it, however DR got into the process. So the taint tier's landed pieces
carry over verbatim —
- the taint client `libasmtest_drtaint_client.so`
  ([dataflow_dr_client_inlined.c](../../../src/dataflow_dr_client_inlined.c), `-DASMTEST_TAINT`):
  BSD tag shadow, per-thread reg tags, inline `dst_tag = ∪ src_tags`, create-on-touch
  stores, byte-granular tags, the branch-condition sink;
- the cross-process results channel [asmtest_taint_shm.h](../../../include/asmtest_taint_shm.h)
  (sink report + value/taint trace; consumer reads by OFFSET, never producer pointers);
- the out-of-process validator [taint_validator.c](../../../examples/taint_validator.c)
  (drains + oracle-diffs against the emulator forward slice);
- the **thread-safe** sink-report append (atomic fetch-add slot, landed with the
  concurrent-writer stress) — already multi-writer ready.

The genuinely new surface is: **(a)** getting DR into a running native process and back
out cleanly, **(b)** configuring seed/sink/region for a target that **never calls the
C-harness markers**, and **(c)** proving it on DR's *experimental* external-attach path.

---

## Goals and non-goals

**Goals**

- **De-risk the attach + detach mechanics first, on the non-experimental path.** Prove
  in-band taint capture of an *already-running* native process across a *takeover →
  scoped-capture → detach → continue-native* lifecycle, using the proven `dr_app_*`
  cooperative API before touching the experimental external injector.
- **Empirically test DR's external attach** (`dr_inject_process_attach` / `drrun -attach`)
  on the pinned DR 11.91 — the same "documented-but-never-tested blocker" discipline the
  extension-load probe (taint Inc 2) applied. Attach is DR-flagged *experimental*; find
  out what actually works before building on it.
- **Marker-less configuration.** A process we did not launch does not call
  `asmtest_dr_taint_seed_marker` etc. Deliver seed/sink/region config via `drrun` client
  options (static) + `dr_nudge` (dynamic arm/disarm), so the target needs no cooperation.
- **Reuse the results channel + validator verbatim.** Attach is a new *producer entry*;
  the shm transport and out-of-process oracle diff are unchanged.
- **Clean detach** (DR fully removed, code cache torn down, target runs native afterward),
  and — new relative to launch — **attach/detach cycling** on one long-lived process.
- Keep the whole tier **native, docker-CI-checkable, no hardware**, mirroring the taint
  tier's lane discipline.

**Non-goals (deferred, with reasons)**

- **Managed (dotnet/JVM) external attach.** Stays out of the critical path for the reasons
  the taint plan and analysis cite: threads frozen at an arbitrary state (in syscalls, on
  runtime locks, mid-GC, deep in JIT'd code) and the SIGSEGV/SIGTRAP handler collision.
  A managed attach that is not a crash needs **GC-safepoint coordination** — parking
  managed threads via the runtime's diagnostics IPC or an injected managed helper — which
  is a research increment here, not a milestone. The **default managed path stays
  launch-under-DR** (LANDED) or **ptrace-attach out-of-band**
  ([live-attach-dataflow-plan.md](live-attach-dataflow-plan.md)).
- **Duplicating the ptrace live-attach tier.** That plan attaches *out-of-band* (ptrace
  single-step, works on managed + native at far higher per-step cost) and produces L0
  **value / def-use**, not the in-band **taint shadow**. This plan is its low-overhead,
  native, in-band complement — the two share the analysis/spine, not the mechanism.
- **`drwrap`.** Marker/arg + config resolution stays PC-resolved (`dr_get_proc_address`)
  + client-option/nudge, never `drwrap_wrap` — same constraint the taint client holds.
- **Whole-process taint under attach.** Scoped-region attach first; whole-process +
  method-range scoping rides on taint Increment 6.

---

## Design overview

**Two attach mechanisms, used in order of risk.**

1. **Cooperative in-process attach/detach — `dr_app_setup_and_start` / `dr_app_stop_and_cleanup`.**
   The target itself brings DR (and the client) up *after it has been running native code*,
   arms a scoped window, then tears DR back down and continues native. The target
   cooperates (calls the app API), so this is not "attach to a foreign PID" — but it *is*
   the takeover-of-a-running-process + clean-detach lifecycle, on a **proven, non-
   experimental** API. It isolates the two hardest questions (does mid-run takeover work?
   does detach leave a clean, native, still-running process?) from the *experimental*
   injector. This is the first milestone.

2. **External attach — `dr_inject_process_attach(process_id_t pid, void **data, char **app_name)`
   / `drrun -attach <pid>`.** A separate injector process seizes a running foreign PID
   (on Linux, a ptrace-based takeover that redirects the running threads into DR's code
   cache) and injects `libdynamorio.so` + the client. DR-flagged *experimental*, so it gets
   an empirical probe (Increment 2) before anything is built on it.

**Marker-less config (the launch tier's markers do not apply).** The launched workload
calls `asmtest_dr_valcapture_marker(region, len, drval)` / `_seed_marker` / `_sink_marker`;
an attached foreign process calls nothing. Replace that with:
- **`drrun` client options / `dr_config` client args** — static config parsed in
  `dr_client_main(argc, argv)`: the region `[pc, pc+len)` to instrument (a module+offset or
  absolute PC), the seed `{base,len,color}`, the sink kind, and the shm segment name.
- **`dr_nudge_client` / `dr_nudge_pid`** — a dynamic, asynchronous signal to the attached
  client to *arm* (start capturing / paint the seed at a chosen instant) or *disarm/detach*.
  This is how an attach tool says "capture the next N iterations, now."
- The client already resolves the region + writes results identically; only the *source*
  of `g_region` / `g_report` / the seed changes from a marker clean call to option+nudge.

**Results + oracle: unchanged.** Producer writes the sink report + (at detach) the value/
taint trace into the POSIX-shm segment named by the client option; the separate
`taint_validator` drains it and oracle-diffs against the emulator forward slice out of
process — exactly as the launch tier does. The atomic report append already tolerates the
attached target's real threads.

**Detach is the new lifecycle primitive.** Launch = one lifecycle to process exit; attach
= take over mid-run, capture a bounded window, then **release**. `dr_app_stop_and_cleanup`
(cooperative) / the injector's detach (external) must flush the code cache, free the shadow
directory + reg-tag TLS, and hand the threads back to native execution with correct state.
Detach correctness (and repeatability) is a first-class exit criterion, not an afterthought.

**Concurrency.** An attached process has its *own* real threads already running. The
shadow's tolerated-benign-race single-byte-store policy + atomic leaf CAS + per-thread reg
tags + atomic report append (all LANDED + stress-validated in taint Inc 5) are what make
this sound; attach is the first tier where those threads are *not* ones we spawned.

---

## Increment 1 - Cooperative attach + detach on a running native process *(recommended first milestone)*

De-risk takeover-mid-run + clean detach with the **proven** `dr_app_*` API, no external
injector. A native workload runs a loop **natively** for a while, then `dr_app_setup_and_start`
brings up DR + the taint client on itself, arms a scoped region (a seed→derive→branch-sink
fixture) for a bounded window whose sink hits land in the shm report, then
`dr_app_stop_and_cleanup` detaches and the loop continues **natively** to exit.

- New `examples/taint_attach_coop.c` (launched *without* `drrun` — it self-attaches):
  before/after the armed window it runs the same fixture with DR *absent* (no capture);
  during the window DR is up and the client captures.
- Reuses `libasmtest_drtaint_client.so` via the `dr_app_*` path (the existing
  `asmtest_drtrace.h` lifecycle already wraps `dr_app_setup/start`; extend it with a
  `stop_and_cleanup` detach entry), the shm report, and `taint_validator`.

**Exit criteria:** a new `make docker-taint-attach` / `dr-taint-attach-coop-test` lane:
the process runs native → attaches → the sink fires for the armed iterations only (report
count == armed count, all tainted, correct offset) → detaches → runs native to exit 0;
`dr_app_running_under_dynamorio()` is false before/after and true during; the taint set is
oracle-diffed out of process; no crash across the attach and the detach boundary. Green in
CI, no hardware. **Effort: M** (reuses the client + shm + validator; new = the detach
lifecycle + the "native-then-armed-then-native" harness).

---

## Increment 2 - External-attach empirical probe (native) *(**LANDED 2026-07-14 — GO**; the experimental blocker is RETIRED for native)*

> **UPDATE 2026-07-14 — probe LANDED; verdict GO.** DR external attach to an already-running
> native process WORKS on the pinned DR 11.91.20630: `drrun -attach <pid> -c <client>` takes the
> running victim over, the client's `drmgr` bb event instruments its live code (**1.6 billion
> instructions executed** in the probe), the victim KEEPS RUNNING (heartbeats continued past the
> attach) and exits native, all with `--cap-add=SYS_PTRACE`. Artifacts:
> [attach_probe_victim.c](../../../examples/attach_probe_victim.c),
> [attach_probe.c](../../../drclient/attach_probe.c) (opt-in `-DASMTEST_BUILD_ATTACH_PROBE`),
> `make dr-taint-attach-probe` / `make docker-taint-attach-probe`. **Load-bearing gotchas** (full
> record: [dr-attach-probe-findings.md](../analysis/dr-attach-probe-findings.md)): (1) `-attach <pid>`
> must PRECEDE `-c <client>` — drrun parses everything after `-c` as client options, so the
> mis-ordered `-c <client> -attach <pid>` fails with a misleading `ERROR: no app specified` (reads as
> "unsupported"; it is not — `drrun -h` lists `-attach`); (2) `--cap-add=SYS_PTRACE` is required AND
> SUFFICIENT — it bypasses the container's `yama/ptrace_scope=1` (no seccomp-unconfined needed), which
> matters because the injector and victim are siblings, not parent/child; (3) `dr_inject.h`'s
> `dr_inject_process_attach` + `libdrinjectlib.a` ship too, so a custom injector is available if a
> later increment needs finer control, but the `drrun -attach` front-end works and is simplest.
> **Increments 3-5 (native attach data-flow/taint end-to-end) are UNBLOCKED**; managed attach
> (Increment 6) stays research-gated (this probe is native-only).

The extension-load-probe move, for attach: a throwaway probe — NOT a product artifact —
that attaches to a **separate, already-running native process** and reports what DR's
experimental external attach actually does on the pinned DR 11.91.

- `examples/attach_probe_victim.c`: a plain native loop (spins on a volatile, prints a
  heartbeat), started first and left running.
- `drclient/attach_probe.c` or a small injector using **`dr_inject_process_attach(pid, …)`
  → `dr_inject_process_inject` → `dr_inject_process_run`** (or simply `drrun -attach <pid>`
  with the probe client), asserting: DR takes the victim over, the client's bb event sees a
  **non-zero** instrumented instruction count, the victim **keeps running** (heartbeat
  continues), and detach returns it to native.
- Record findings in `docs/internal/analysis/dr-attach-probe-findings.md` (glibc / DR
  version / ptrace-seize behaviour / any `-late` vs `-early` interaction), exactly as the
  extension-load probe did — this is the yes/no that gates Increments 3-5.

**Exit criteria: MET 2026-07-14 (GO).** `make docker-taint-attach-probe` runs `drrun -attach`
over the running victim in a container with `--cap-add=SYS_PTRACE` and prints `ATTACH PROBE OK` —
takeover + non-zero instrumentation (1.6 B insns) + victim-survival + native exit all held
(`client_reached=1 takeover_ok=1 pre_beats=20 post_beats=100 victim_end=1 attach_rc=0`); self-skips
without DynamoRIO. **Effort: M-L (delivered)** — a new injector harness against the experimental
API; the risk was entirely in DR's attach maturity, and the probe measured it as WORKING for the
native case on the pinned DR 11.91.20630.

---

## Increment 3 - Marker-less seed/sink/region config (client options + nudge) *(**LANDED** — config half 2026-07-14; interactive nudge arm/disarm 2026-07-15)*

> **UPDATE 2026-07-14 — marker-less CONFIG landed (validated under launch, first try).** The DR taint
> client now learns region/seed/report ENTIRELY from client options + runtime module+offset
> resolution — no marker clean calls: `region=<mod>+0x<off>,<len>`, `seed=<mod>+0x<off>,<len>,<color>`,
> `shm=/<name>`. The offsets resolve against the named module's runtime base in `event_module_load` /
> `resolve_all_modules` (`apply_ml_config`); the client OWNS the results shm, opened with bare
> open/ftruncate/mmap-MAP_SHARED syscalls (`open_ml_shm`, DR-API-free like the GC-move leaf allocator)
> and published `done` at exit. A new `examples/taint_markerless_victim.c` carries the
> `taint_sink_chain` fixture + seed as STATIC globals (PIE, so `nm` offset == module offset) and fires
> NO markers. `make dr-taint-markerless-test` (in `drtrace-test`): `drrun -c <client> region=<victim>+off
> seed=<victim>+off shm=/name -- victim` produces the IDENTICAL oracle-diffed result as the
> marker-based launch lane — **seeded 8/8 incl. the full out-of-process TAINT-SET oracle diff (taint set
> == emulator forward slice); noseed negative 3/3 (zero hits)**; value client still 14/14 byte-identical.
> `taint_validator` gained a `markerless` mode (skips the app-set `result` check; skips the seeded emu
> oracle for the noseed control). **Nudge slice LANDED 2026-07-15:** `dr_register_nudge_event` +
> `handle_nudge` decode the low byte of the nudge arg as an opcode — ARM (resolve the configured region,
> `register_range`+`on_seed`, flush so fragments rebuild WITH instrumentation), DISARM (clear + flush),
> SNAPSHOT (publish shm `done`). A new `nudge` client option starts DISARMED so ARM genuinely opens the
> window; `in_scope()` gained a `g_armed` gate (taint-only; flag-off value client byte-identical). The
> handler runs in a valid DR nudge context (DR documents `dr_flush_region` as nudge-callable — not the
> foreign app-code GC thread the live-remap path guards). `make dr-taint-nudge-test` /
> `docker-taint-attach`: a mid-run ARM→capture→DISARM round-trip **5/5** + never-armed control **2/2**,
> victim survives; coop 8/8 + validator 9/9 unbroken. The external-attach (`drrun -attach` +
> `drnudgeunix`) compose is the trivial follow-on — identical client code.

An attached foreign target calls no markers, so the client learns *what* to instrument
from outside.

- Extend `dr_client_main(argc, argv)` (under `-DASMTEST_TAINT`, additive; flag-off value
  client untouched) to parse client options: `region=<module>+<off>,<len>` or
  `region=0x<pc>,<len>`; `seed=0x<base>,<len>,<color>`; `sink=branch`; `shm=/<name>`.
- Wire `drmgr_register_nudge_event` so a `dr_nudge_pid(pid, client_id, ARM)` /
  `... DISARM` from the attach tool arms the seed paint + capture window and later triggers
  the value-trace drain + `dr_app_stop_and_cleanup`-equivalent — the "capture now, for this
  long" control an interactive attach needs.
- `g_region` / `g_report` / the seed are populated from options/nudge instead of the marker
  clean calls; everything downstream (shadow, propagation, sink, shm) is unchanged.

**Exit criteria:** the coop lane (Inc 1) driven entirely by client options + a nudge (no
marker calls) produces the identical oracle-diffed result; a nudge arms/disarms a bounded
window mid-run. **Effort: M.**

---

## Increment 4 - Native attach data-flow/taint end-to-end + out-of-process oracle *(**LANDED 2026-07-14** — first full external-attach taint capture)*

> **UPDATE 2026-07-14 — external-attach taint capture LANDED.** Composed Increment 2 (external
> attach GO) + Increment 3 (marker-less config): the UNCHANGED taint client is injected into a
> SEPARATE, already-RUNNING native victim via `drrun -attach <pid> -c <client>` and configured
> entirely by options (`region=<mod>+off seed=<mod>+off shm=/name`, resolved by module+offset) — a
> producer ATTACHED to a process it did not start. `make dr-taint-attach-test` (in the
> `--cap-add=SYS_PTRACE` external-attach image alongside the probe): the victim
> (`taint_markerless_victim attach`) loops the seeded fixture for ~12 s; the client seizes it mid-run,
> seeds + registers the region, and its post-seed runs trip the branch-condition sink into the
> client-owned shm. `taint_validator` (`attach` mode) drains + gates it — **seeded 5/5: ≥1 tainted
> `kind=1` sink hit from the attached capture, sink off 0x10, victim SURVIVED attach + detach and
> exited native; noseed negative 2/2 (zero hits)**. SINK-based (the attach window captures a VARIABLE
> number of post-seed runs, so the exact-count + single-run taint-SET oracle are relaxed vs the launch
> lane; the marker-less LAUNCH lane keeps the full oracle diff). No client changes beyond Increment 3.

Compose 2 + 3: attach to a **separate long-running native workload** that loops over a
known region with a seeded buffer, capture the in-band taint set + value/taint trace via
the reused client, drain over shm, and oracle-diff out of process — the launch tier's
`dr-taint-launch-test`, but the producer is *attached to a process it did not start*.

**Exit criteria: MET 2026-07-14.** `make dr-taint-attach-test` (in the SYS_PTRACE external-attach
image) attaches to the running victim, the client's seeded capture reports a tainted `kind=1` sink
hit at 0x10 crossing shm (the seed propagated through the attached capture), and the victim survives
attach + detach + exits native; the negative control (unseeded) reports zero hits. The taint-SET emu
oracle diff is exercised on the marker-less LAUNCH lane (`dr-taint-markerless-test`, 8/8) — the same
config path; the attach lane, capturing a variable multi-run window, gates SINK-based. **Effort: L
(delivered)** — the first full external-attach capture, gated on (and unblocked by) Increment 2's GO.

---

## Increment 5 - Detach correctness + attach/detach cycling *(**COMPLETE 2026-07-14** — mid-run detach returns the victim to native; K-round re-attach cycling + leak assertion landed)*

> **UPDATE 2026-07-14 (2/2) — K-ROUND CYCLING + LEAK ASSERTION LANDED → Increment 5 COMPLETE.**
> `make dr-taint-cycle-test` (4th lane in the external-attach image) seizes ONE long-lived native
> victim (`taint_markerless_victim attach <secs>`) K=3 times: each round `drrun -attach <pid>` takes
> it over marker-less, captures a window into a per-round client-owned shm, then `drconfig -detach
> <pid>` releases it — and the lane asserts PER ROUND (a) the attached window captured a tainted
> `kind=1` sink hit (attach worked), (b) the victim is alive + its heartbeats ADVANCE after the
> detach (returned to native between rounds), and (c) the surviving native victim does not accumulate
> memory. **RESULT: external DR re-attach is RELIABLE on this DR — 3/3 rounds took over, captured,
> and detached-to-native on the same pid, victim exited clean (`rc=0`)** — so the "in-process
> re-attach is unreliable" caveat ([asmtest_drtrace.h:87](../../../include/asmtest_drtrace.h#L87))
> does NOT extend to the EXTERNAL attach path. **Leak assertion — and a real leak it caught + FIXED:**
> `event_exit` (fires on each detach) freed the 1 GiB shadow DIRECTORY + reg-tag TLS + drx buffers but
> NOT the installed 1 MiB shadow LEAVES, so cycling orphaned ~2 leaves (~2 MiB) per round (measured
> native VmSize growth +2124 kB/round). Fixed by an installed-leaf registry (a lock-free atomic-index
> table recording each leaf + its allocator provenance so `event_exit` frees each with its matching
> deallocator — `dr_raw_mem_free` for hot-path leaves, `raw_munmap` for the bare-mmap GC-path leaves;
> [dataflow_dr_client_inlined.c](../../../src/dataflow_dr_client_inlined.c)). Post-fix round-over-round
> growth dropped to **+68 kB/round** (a fixed one-time ~2.4 MiB DR-attach VA footprint that external
> detach does not fully return is paid once in round 1). The lane gates it two ways: round 1 must not
> jump a shadow-DIRECTORY scale (~1 GiB), rounds ≥2 must not grow a shadow-LEAF scale (~1 MiB) over the
> prior round. All additive under `-DASMTEST_TAINT` (flag-off value client still 14/14 byte-identical;
> `docker-drtrace` 200 ok incl. taint-set oracle + gcremap 13/13). Also: heartbeat cadence densified
> (every 16 beats ≈ 0.3 s) so the return-to-native signal is robust across the ~2 s post-detach window
> the attach/detach/cycle lanes sample; the victim gained an optional duration arg for the longer
> K-round window (default 12 s unchanged). External-attach docker image now 4 lanes: probe + attach +
> detach + cycle.

> **UPDATE 2026-07-14 (1/2) — DETACH correctness (return-to-native) LANDED.** The take-over-and-LET-GO half
> of the contract. Unlike `dr-taint-attach-test` (which stays attached until the victim exits),
> `make dr-taint-detach-test` attaches to the running victim (background `drrun -attach`), captures a
> ~4 s window, then DETACHES MID-RUN via `drconfig -detach <pid>` and asserts the victim RETURNS TO
> NATIVE: its heartbeats keep advancing AFTER the detach (measured 5 → 10 past detach) and it exits
> cleanly (`victim_rc=0`, uncorrupted) — DR restored its register/flag state and removed the
> instrumentation. The attach-window capture is checked too (≥1 tainted `kind=1` hit). The detach
> mechanism is `drconfig -detach <pid>` (in the pinned DR; needs CAP_SYS_PTRACE like the attach).

Launch's "one lifecycle per process" contract is *replaced* by attach's take-over-and-let-go.

- Verify detach flushes DR's code cache, frees the shadow directory (`dr_raw_mem_free`) +
  the reg-tag TLS + the drx buffers, and returns threads to native with correct register/
  flag state (the target's own computation, sampled across the detach boundary, is
  bit-identical to a never-attached run).
- **Attach/detach cycling:** attach → capture → detach → (native) → attach again on the
  *same* PID, N times, results intact each window — the capability the taint plan's header
  flagged as *unreliable* for the in-process re-attach path
  ([asmtest_drtrace.h:86](../../../include/asmtest_drtrace.h#L86)); attach is where it must
  either work or be documented as bounded.

**Exit criteria: MET 2026-07-14.** `make dr-taint-cycle-test` shows K=3 attach/detach rounds over one
victim with correct per-window capture and a native, uncorrupted target between and after (victim
exits `rc=0`); the leak check confirms the shadow (directory + leaves) + TLS + drx buffers are freed
each detach (round-over-round native VmSize flat at ~+68 kB after the leaf-free fix). **Effort: M
(delivered).** External re-attach proved RELIABLE (not merely bounded) on the pinned DR.

---

## Increment 6 - Managed attach: GC-safepoint spike *(**PROBE LANDED 2026-07-14 — NO-GO**; kill criterion exercised, managed stays launch/ptrace)*

> **UPDATE 2026-07-14 — managed-attach go/no-go PROBE landed; verdict NO-GO.** The bounded
> spike (the Increment-2 probe move, for managed): `make dr-taint-managed-attach-probe` starts a
> plain long-running `dotnet` process (`examples/managed_attach_victim`, a managed heartbeat loop
> whose hot method tiers up), injects DR + the minimal counting client (`drclient/attach_probe.c`,
> reused verbatim) via `drrun -attach <pid>` mid-run, and detaches. **RESULT: DR external attach
> DELIVERS + starts the client inside the running .NET process (`dr_client_main` runs — injection
> works exactly as for native), but the TAKEOVER immediately trips glibc stack-smashing detection
> and the process dies with SIGSEGV (rc 139)** — no managed heartbeat progresses past the seize,
> reproducibly across two runs (`client_reached=1 takeover_ok=0 crash_text=1 victim_rc=139`). This
> is the arbitrary-state-takeover hazard: DR seizing the runtime's many threads (GC / tiered-comp /
> finalizer / JIT) at arbitrary points does not preserve their stack/TLS/guard invariants. **Kill
> criterion MET** — record it and keep managed on **launch-under-DR / ptrace** (both landed/planned);
> native external attach is unaffected (GO, Increments 2-5). The research tail (diagnostics-IPC
> safepoint park, `DR_SIGNAL_DELIVER` pass-through, `-late` posture) was deliberately NOT chased —
> a bounded spike, not the XL effort. Findings + the exact failure mode:
> [dr-managed-attach-probe-findings.md](../analysis/dr-managed-attach-probe-findings.md). Docker:
> `make docker-taint-managed-attach-probe` (DR + .NET SDK, `--cap-add=SYS_PTRACE`); THROWAWAY
> diagnostic, not in the main CI gate.
>
> **UPDATE 2026-07-14 (2/2) — Option-1 cheap experiments swept; still NO-GO; Option-2 plan written.**
> A bounded sweep (parameterized via `PROBE_DROPS` / `PROBE_CLIENT_ARGS` on the probe lane) confirmed
> the NO-GO is fundamental: a **`noinstr` seize-only control** (take the process over with ZERO
> instrumentation) crashes **identically** → it is the SEIZE of arbitrary managed state, not the
> per-instruction client; `-no_mangle_app_seg` removes the `%fs` stack-canary symptom but the process
> still SIGSEGVs (segment/TLS handling is one facet of a deeper takeover incompatibility);
> `-thread_private` / `-native_exec` don't help; and **no DR release helps** (newest cronbuild
> 11.91.20644 identical; newest stable 11.3.0 fails differently — exit 255 — but still no survival).
> Option 1 is exhausted. The one credible remaining path — **park all managed threads at GC-safe
> points BEFORE the seize** — was planned + then EXECUTED as a spike
> ([dynamorio-managed-attach-safepoint-plan.md](dynamorio-managed-attach-safepoint-plan.md)):
> **Increment 1 GO** (a co-loaded profiler's `SuspendRuntime`/`ResumeRuntime` cycles the runtime
> clean, natively and under DR), but **Increment 2 NO-GO** — a runtime with ALL managed threads
> parked at GC-safe points **still crashes identically** at the DR seize (stack-smashing → SIGSEGV
> rc 139; `ResumeRuntime` never runs). Decisive: the fatal takeover is the **native** runtime threads
> DR also seizes, which a managed suspend cannot park — so **both options are exhausted**; managed
> stays launch-under-DR / ptrace. Lanes: `make docker-suspendprof-probe`.

The hard, explicitly-fragile direction — treated as a **spike with a kill criterion**, not
a milestone. Attach a native DR client to a **running dotnet** process and survive it.

- The blocker is arbitrary-state takeover + the runtime's own SIGSEGV/SIGTRAP handlers
  (prior managed-tracing SIGTRAP history is a live warning:
  [[dotnet-scoped-tracing-review]], [[java-stealth-sigtrap]], [[go-fulltest-flaky-crash]]).
- Approaches to evaluate: request a runtime pause via **.NET diagnostics IPC** (the same
  channel the hwtrace dotnet work used) so threads are at GC-safe points before the seize;
  `DR_SIGNAL_DELIVER` pass-through for the runtime's handlers; `-late` injection posture.
- **Kill criterion:** if a trivial `dotnet` process cannot survive DR external attach +
  detach without swallowing a .NET signal or crashing after a bounded spike, record the
  finding and **stay with launch-under-DR / ptrace-attach for managed** (both already
  landed / planned) — do not force it.

**Exit criteria (if reached):** a `drrun -attach <dotnet-pid>` (or injector) window
captures a seed→sink over a managed buffer and the process continues, validated out of
process; OR a findings doc that closes managed-attach as not-viable-on-this-DR with the
concrete failure mode. **Effort: XL / research.**

---

## Increment 7 - asmspy `--attach-dataflow` surface *(optional, mirrors the ptrace plan)*

Surface native attach-based data-flow in the asmspy CLI (a `--attach-dataflow <pid>`
subcommand + a TUI window), the DR-attach counterpart of the ptrace live-attach plan's
asmspy increments. Shares the JSON/def-use rendering; differs only in the producer. Kept
last + optional; the capture tiers above stand without a UI.

---

## Risks and open points

- **DR external attach is experimental** — Increment 2 is the go/no-go. If it does not work
  on the pinned DR, Increments 4-6 stall and the tier is the cooperative-attach half
  (Increment 1) only, still a real capability (take over a native process we control, mid-
  run, and detach).
- **Detach state fidelity.** Redirecting seized threads back to native with exact register/
  flag/cache state is where a DBI detach bug would corrupt the target — hence Increment 5's
  bit-identity check.
- **`SYS_PTRACE` in CI.** The Linux seize likely needs the capability; the hwtrace docker
  lanes already grant caps in CI, so the pattern exists — but a plain-runner lane may have
  to self-skip.
- **Managed attach may simply not be viable** on this DR; that is an accepted outcome
  (Increment 6's kill criterion), not a failure — the managed default stays launch/ptrace.

## Recommended first milestone

**Increment 1** (cooperative attach + detach on a native process) — the low-risk half. It
reuses every landed taint asset (client, shm, validator, atomic report), touches only the
`dr_app_stop_and_cleanup` detach lifecycle, runs entirely in docker with no experimental
API and no hardware, and *already* demonstrates the headline capability: DR takes over an
already-running native process, data-flows a scoped region in band, and lets it go — with
the result oracle-diffed out of process. Increment 2's probe then decides how far the
*external* (foreign-PID) attach can go.
