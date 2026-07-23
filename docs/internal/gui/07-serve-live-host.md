# `asmspy --serve` + the desktop live-session host — implementation

> **Sources.** Actioned from [desktop-gui-plan.md](../plans/desktop-gui-plan.md):
> Phasing **Phase 3** ("`--serve` control loop", "the Inspect door with both
> `--auto` samplers", time-slicing per the corrected budget), design **D6**
> (concurrency budget) and **D9** (the serve-subprocess capture host, per this
> directory's README), and the engine-work-items row "`--serve` control loop
> over stdout/unix socket". Written 2026-07-24. If this doc and a source
> disagree, this doc wins (sources may be stale); if the CODE and this doc
> disagree, re-verify before implementing.
>
> Read [\_conventions.md](../implementations/_conventions.md) first; shared
> decisions D1–D11 live in this directory's README. Siblings:
> [01-asmtrace-format.md](01-asmtrace-format.md) (the `asmtrace_emit` writer TU
> + event kinds — `--serve` streams **exactly** those events),
> [03-desktop-shell.md](03-desktop-shell.md) (Workspace model, session UI
> shell), [08-observer-views.md](08-observer-views.md) (the views these
> sessions feed). Symbols from siblings are marked **(new — 0N)**; everything
> else cited exists at HEAD `a460d40`.

## Why this work exists

Every live Observer feature stands on one seam: the asmspy engines already fire
typed sinks on a dedicated tracer thread; `--serve` swaps 01's NDJSON
serializers in for the ncurses sinks and adds a control channel. Per **D9**,
the desktop app **never links the ptrace engines** — the capture host is the
`asmspy` binary itself, spawned as a subprocess locally or reached over ssh.
That gives one code path for local and remote capture, keeps every hard-won
engine guarantee (two-phase detach, own-`int3` delivery, JIT map refresh,
one-tracer-thread rule) inside the binary that already tests them, and lets
even the render-only viewer host live sessions.

## What already exists (verified 2026-07-24)

- **The one-tracer-thread contract** — every ptrace call + `waitpid` must come
  from the attaching thread; engines run start-to-finish on one thread
  ([cli/asmspy.h:14](../../../cli/asmspy.h#L14)).
- **The TUI's session pattern to mirror** — `run_live_view`
  ([cli/asmspy.c:2714](../../../cli/asmspy.c#L2714)) spawns a tracer thread and
  tears it down by setting the stop flag then `pthread_kill(th, SIGALRM)` to
  wake a blocked `waitpid` ([cli/asmspy.c:3207](../../../cli/asmspy.c#L3207),
  [:3372–3373](../../../cli/asmspy.c#L3372), and the dataflow view's identical
  teardown at [:4159–4162](../../../cli/asmspy.c#L4159)).
- **Engine API shape** — uniform `(pid, [only_tid], [follow], max/ms,
  atomic_bool *stop, [syms/jit/filter], typed sink, void *ctx)`; the syscalls
  engine has **no tid parameter**
  ([cli/asmspy.h:218–219](../../../cli/asmspy.h#L218)).
- **Typed self-skip codes** — `ASMSPY_REGION_NEVER_RAN`,
  `ASMSPY_SAMPLE_UNAVAIL`, `ASMSPY_DATAFLOW_UNAVAIL`, `ASMSPY_WATCH_UNAVAIL`,
  `ASMSPY_ETRACEE_I386` + `asmspy_strerror` (declared across
  [cli/asmspy.h](../../../cli/asmspy.h); i386 refusal is pre-attach).
- **The `--auto` front door, both samplers** — AMD IBS entry-edge ranking
  `auto_pick` ([cli/asmspy.c:1955](../../../cli/asmspy.c#L1955)); portable
  sw-clock **residency** sampler `auto_pick_sw`
  ([:2089](../../../cli/asmspy.c#L2089)); selection + the 3-candidate
  `NEVER_RAN` walk ([:2200–2214](../../../cli/asmspy.c#L2200), the walk below
  it). Residency is *not* entry evidence — the weaker-evidence label is part of
  the front door's contract.
- **Ptrace-consumer set** — `--watch` SEIZEs every thread; the topology engine
  SEIZEs the whole descendant tree (`seize_process_tree` feeding
  `asmspy_engine_procs`, [cli/asmspy_engine.c](../../../cli/asmspy_engine.c));
  only IBS sampling and the sw-clock survey coexist with a ptrace view.
- **No serve mode exists** — no `--serve` in
  [cli/asmspy.c](../../../cli/asmspy.c); greenfield.
- **Smoke pattern + victims** — [cli/cli_smoke.sh](../../../cli/cli_smoke.sh)
  builds and drives victims (`auto_victim` start/kill flow at
  [:379–382](../../../cli/cli_smoke.sh#L379)); `spy_victim.c`, `auto_victim.c`
  et al. live in [cli/](../../../cli/).

## Tasks

### T1 — Serve protocol spec  (S, depends on: 01)

**Goal.** A normative *Serve protocol* section appended to
[asmtrace-schema.md](asmtrace-schema.md) (file owned by 01 — append-only): the
control channel and lifecycle events, so the desktop host and the smoke test
are written against a spec, not the C code.

**Steps.**
1. Append a `## Serve protocol` section to the schema doc.
2. Define the **control channel** (NDJSON commands, one per line, on stdin or
   the unix socket from `--serve=<path>`):
   `{"cmd":"start","mode":"log|stream|trace|dataflow|tree|graph|procs|sample|watch|auto","pid":N,params...}`
   (params mirror each subcommand's flags: `tid`, `follow`, `max`, `ms`,
   `depth`/`focus`/`module` for tree, `addr`/`len`/`rw`/`n` for watch),
   `{"cmd":"pause"}`, `{"cmd":"stop"}` (end the mode, stay attached-idle:
   detach fully — one engine run per start), `{"cmd":"quit"}`.
3. Define **lifecycle events** (new `"k"` values, added to the schema's v1 kind
   list as serve-only): `{"k":"session","state":"started|stopped|skip","mode":...,
   "skip":{"code":N,"reason":str}?}` — engine skip codes and `asmspy_strerror`
   text flow through verbatim; `{"k":"err","reason":str}` for refused commands
   (budget violations, tid XOR follow, unknown mode).
4. State the two protocol laws: **events between `start` and the terminal
   `session` event are exactly 01's record-mode events** for that mode
   (same serializers, same provenance header per session); **sorting/filtering
   stay client-side** except the tree filter, which is engine-side by design
   ([cli/asmspy.h](../../../cli/asmspy.h) `asmspy_tree_filter_t`).

**Code.** Spec only. **Tests.** None (T6 exercises it). **Docs.** The section
itself.

**Done when.** The section exists; 03's loader parses `session`/`err` kinds
(one fixture added to its unit tests); 01's owner sign-off recorded in the
section header comment.

### T2 — `--serve` loop in `cli/asmspy.c`  (M, depends on: T1; 01's T3 writer TU)

**Goal.** `asmspy --serve[=<socket>]` — read commands, run **one engine
session at a time** on a dedicated tracer thread, stream 01's events.

**Steps.**
1. Add the flag to the dispatch in `main` (beside the other subcommands) and a
   `cmd_serve` function.
2. Session start: validate the budget (T4 rules) and flag matrix (tid XOR
   follow; auto XOR tid; module/sampler only with auto — mirror the existing
   refusals in the arg parsing), then `pthread_create` a tracer thread that
   calls the one engine for the mode with the NDJSON sinks from 01's writer TU
   (**new — 01**) as `sink`/`ctx`, exactly as `run_live_view` structures it
   ([cli/asmspy.c:2714](../../../cli/asmspy.c#L2714)).
3. Stop/quit: set the session's `atomic_bool`, then loop
   `pthread_kill(th, SIGALRM)` + timed-join, copying the teardown at
   [:3372–3373](../../../cli/asmspy.c#L3372) — never bypass the engine's
   two-phase detach.
4. Emit `session` lifecycle events on every transition; a positive engine
   return (skip code) becomes `state:"skip"` with `asmspy_strerror` text; a
   negative return becomes `err`.
5. Mode switching = full stop (detach) then a new `start` — no engine reuse
   across modes.
6. `--serve=<path>`: `unix(7)` `SOCK_STREAM` listener, one client at a time;
   default is stdin/stdout. No TLS, no auth — the socket is filesystem-
   permissioned, and ssh is the remote transport (T3).

**Code.** One `serve_session_t { mode, params, atomic_bool stop, pthread_t th,
bool running }` static in `cli/asmspy.c`; command parsing reuses 01's minimal
JSON-read idiom (no new dependency — the C side stays dependency-free).

**Tests.** T6. **Docs.** `--serve` line in the usage block + one paragraph in
[docs/guides/tracing/asmspy.md](../../guides/tracing/asmspy.md) (+ CHANGELOG
`Added`).

**Done when.**
- `printf '{"cmd":"start","mode":"log","pid":%d}\n' $PID | ./build/asmspy --serve`
  streams syscall events for the victim and a Ctrl-D (EOF = quit) leaves the
  victim running (verify with `kill -0`).
- A second `start` without `stop` is refused with `err` naming the budget rule.

### T3 — Desktop session host  (M, depends on: T1; 03)

**Goal.** `desktop/src/live/session.h/.cpp` (**new**): spawn `asmspy --serve`
as a subprocess (`fork`/`execvp` + pipes) or connect over ssh
(`ssh <host> asmspy --serve`), feed events into the Workspace as a growing
Recording, send commands from the UI.

**Steps.**
1. `LiveSession` class: spawn/connect, non-blocking reads on the event pipe
   into 03's NDJSON loader (**new — 03**) line-by-line; command writer;
   `poll()` pumped from the UI thread once per frame.
2. Process-exit and pipe-EOF handling: the session becomes a *torn* recording
   (01's missing-`end` rule) and the UI says so.
3. Both binaries host sessions (D9 consequence — no engine linked); the
   asmspy path is configurable (default: `asmspy` on `$PATH`, then
   `./build/asmspy`).
4. Unit test with a **fake serve script** `desktop/test/fixtures/fake_serve.sh`
   emitting canned lifecycle + mode events, then EOF; assert session-state
   transitions and torn-recording marking.

**Code.** Keep it POSIX (`fork`/`pipe`/`fcntl` O_NONBLOCK); Windows live
hosting is out of scope (plan: Windows Observer = none in v1).

**Tests.** the fake-serve unit test under `desktop-test`. **Docs.**
`desktop/README.md` session section (03's file — append).

**Done when.** `desktop-test` passes the fake-serve case; a manual run against
a real victim shows live syscall rows.

### T4 — Budget enforcement + patch-bay UI  (S, depends on: T3)

**Goal.** The D6 budget as interaction physics: per target **tree**, one
ptrace jack ({log, stream, trace, dataflow, tree, graph, watch, procs} —
procs/watch included) plus free slots for IBS sample and sw-clock survey.

**Steps.**
1. `desktop/src/live/budget.h` (**new**): pure functions
   `budget_can_start(mode, active_modes)` — table-driven from the engine facts
   (watch SEIZEs all threads; procs SEIZEs the descendant tree, so the jack is
   per-tree while a topology session runs).
2. UI: an occupied jack renders the blocking session and
   "paused — another live view holds the tracer"; starting queues or swaps
   (explicit user action, never silent).
3. Unit-test the table (every mode pair) — this is the doc's cheapest
   highest-value test.

**Code/Tests/Docs.** As above; table test in `desktop-test`.

**Done when.** every mode-pair decision matches D6 (including procs and watch
as consumers), and the serve loop's own refusal (T2) never fires in normal UI
use because the client blocks first.

### T5 — Inspect door: attach picker + `--auto` front door  (M, depends on: T3, T4)

**Goal.** The plan's front door: pick a process, see *why not* when attach is
impossible, and land on a hot function via `--auto` with honest evidence
labels.

**Steps.**
1. Process list (parse `/proc` client-side: pid, comm, uid) with an
   attachability probe per row — surface Yama `ptrace_scope`
   (read `/proc/sys/kernel/yama/ptrace_scope`), missing CAP_SYS_PTRACE, and
   the i386 refusal (`ASMSPY_ETRACEE_I386` lifecycle skip) as first-class
   explanations, not error toasts.
2. `start` with `mode:"auto"`: serve runs the existing selection
   ([cli/asmspy.c:2200–2214](../../../cli/asmspy.c#L2200)) — IBS entry-edge
   ranking where available, else sw-clock residency with the **weaker-evidence
   label** ("residency is not entry evidence") carried in the `session` event's
   params echo; the `NEVER_RAN` 3-candidate walk surfaces as successive
   `session` events.
3. Hot-ranked target picking mirrors `SYMS_SORT_HOT`
   ([cli/asmspy.c:4258](../../../cli/asmspy.c#L4258)) — IBS-only; the designed
   fallback is the sw-clock survey, labeled.

**Tests.** fake-serve fixtures for both sampler paths + the never-ran walk.
**Docs.** Inspect-door section in `desktop/README.md`.

**Done when.** on a non-AMD host the door works end-to-end with the residency
label visible; unattachable rows say why.

### T6 — Serve smoke  (M, depends on: T2)

**Goal.** A `cli_smoke.sh`-style end-to-end proof: serve against a live
victim, well-formed events, clean detach.

**Steps.**
1. Extend [cli/cli_smoke.sh](../../../cli/cli_smoke.sh) (new section, same
   idiom as the `auto_victim` block at
   [:379–409](../../../cli/cli_smoke.sh#L379)): start `spy_victim`, drive
   `--serve` over a pipe: `start` log → events → `stop` → `start` stream →
   `quit`.
2. Assert: every line parses as JSON with a known `"k"`; a `session` event
   brackets each mode; the victim is still alive after quit (`kill -0`); a
   second concurrent `start` was refused with `err`.
3. Wire into the existing cli test target so `make cli-test` (and the docker
   lane) runs it.

**Done when.** `make cli-test` green with the serve section; the victim
survives; the refusal case fires.

## Task order & parallelism

T1 → {T2, T3 (against fake-serve)} in parallel → T4 → T5; T6 follows T2.
Critical path: T1 → T2 → T6. 08's views consume T3's sessions; nothing in 08
blocks T1–T6.

## Constraints & gates

- **Never bypass the engine teardown** — stop flag + `SIGALRM` + join is the
  only way out of a session (the contract at
  [cli/asmspy.h:14](../../../cli/asmspy.h#L14)).
- Linux x86-64 + AArch64 only (the engines' envelope); arm64 hw-debug refusals
  surface via lifecycle skip events (08 renders them).
- The syscalls engine takes no tid — the protocol must not accept `tid` for
  `mode:"log"` (refuse with `err`; the optional engine task is the plan's
  work-item row, not this doc).
- No new C dependencies in `cli/`; the desktop side uses only what 03 vendored.
- Sensitive payloads: serve emits the same payload-separated syscall events as
  record mode (01) — redaction stays a renderer duty (08), the wire carries
  `redacted:false` provenance honestly.

## Out of scope

- Windows/macOS live hosting (no engine); multiple concurrent clients;
  authentication on the unix socket (filesystem perms + ssh are the model);
  PT-capture panels (08); `only_tid` for the syscalls engine (plan work-item,
  optional).
