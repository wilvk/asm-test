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
> **Implemented 2026-07-24 (T0–T6). Corrections this doc lost to the code, in
> the sibling docs' format — the code won each time and this doc was fixed in
> the same change:**
>
> 1. **`make cli-test` does not exist.** T0's Tests/Done-when named it; the cli
>    target is **`make cli-smoke`** (mk/cli.mk), which is what the new
>    `test_libasmspy` and the T6 serve section were wired into.
> 2. **The `--auto` evidence label cannot ride in the `started` event's params
>    echo** (T5 step 2). `started` is emitted before the tracer thread runs, and
>    which sampler AUTO resolves to — and therefore what grade of evidence the
>    pick carries — is not known until the picker has run. The protocol
>    therefore gained a fourth lifecycle state, **`session state:"pick"`**, one
>    per candidate attempted; the `NEVER_RAN` walk is successive `pick` events,
>    which is what T5 already expected ("successive `session` events").
> 3. **A session's slice must be FILTERED to be a valid recording** (T1 law 1).
>    `session` brackets a session from outside the `[header … end]` range, but
>    `cmd`/`err` are emitted when they happen and so land *inside* it, while the
>    `end` footer counts recording events only. Slicing without dropping the
>    three serve-only kinds yields more lines than the footer declares — which
>    is not corruption, and a reader must not treat it as such. Stated
>    normatively in the schema, and asserted both in `cli-smoke` and in the
>    desktop loader test.
> 4. **`asmspy_strerror` had no case for `ASMSPY_SAMPLE_UNAVAIL`** — a real
>    defect this work surfaced rather than a doc error. That positive skip code
>    fell through to the default `"attach failed"`, which is wrong twice over
>    (the IBS sampler is out of band and attaches nothing). Fixed in
>    `asmspy_engine.c`; serve additionally sources every skip `reason` from the
>    **measuring** source, as the schema requires.
> 5. **Inspect is in BOTH binaries, not the full app only.** 03's shell had it
>    disabled in `desktop-render` beside Author. Author links Keystone/Unicorn
>    and must be; Inspect links *nothing* — it reads `/proc` itself and captures
>    through the `asmspy --serve` subprocess — so gating it would have hidden
>    exactly the property D9 exists to buy.
> 6. **03's loader is whole-stream, not incremental.** T3's "feed events into
>    03's NDJSON loader line-by-line" describes an API `load_recording` does not
>    have. `LiveSession::feed_line` is a state machine mirroring its per-line
>    rules, and it reuses `load_recording` verbatim for the *header's* reject
>    rules so a newer major / missing provenance is refused identically on the
>    wire and in a file.
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
serializers in for the ncurses sinks and adds a control channel.

Two structural moves make that clean. **First (T0),** the engine —
`asmspy_engine.c` plus the `/proc`/ELF resolver and the pure view-model headers
— is **extracted into a linkable library, `libasmspy`**, mirroring how every
other tier already ships (`libasmtest_dataflow` / `_emu`); today it is compiled
straight into the binary with no public header. **Second (T2),** `--serve`
becomes a **thin wrapper** over that library. Per **D9** the desktop app still
**never links** it — the capture host is the `asmspy` binary spawned as a
subprocess (locally or over ssh). So extraction is a *packaging* change, not a
*boundary* change: it keeps every hard-won guarantee (two-phase detach,
own-`int3` delivery, JIT map refresh, one-tracer-thread rule) inside the tested
code — now with its own ABI and independent tests — while the subprocess seam
still gives one code path for local and remote capture, cross-platform reach
(macOS/Windows can only ever subprocess-over-ssh), and a render-only viewer that
hosts live sessions without linking any engine. Extraction is deliberately *not*
re-implementation: the plan's constraint 6 rejected *re-writing* attach
elsewhere, which this does not do.

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
- **No library target for the engine (the T0 gap)** — `asmspy_engine.o` +
  `asmspy_proc.o` are linked straight into the binary as `ASMSPY_OBJS`
  ([mk/cli.mk:58](../../../mk/cli.mk#L58)), which notes the producer "ships no
  public header … on purpose"; the engine is deeply Linux-only (~473 ptrace /
  `user_regs` refs, [mk/cli.mk:84](../../../mk/cli.mk#L84)). Every *other* tier
  already ships a shared library via the `shlib_*` helpers
  ([Makefile:251–263](../../../Makefile#L251)) + a `shared-<tier>` target — e.g.
  `shared-dataflow` ([mk/dataflow.mk:132–138](../../../mk/dataflow.mk#L132)) —
  the exact precedent T0 mirrors. The sink typedefs (`asmspy_*_sink`,
  [cli/asmspy.h:210](../../../cli/asmspy.h#L210) onward) are already the
  library's callback boundary.
- **Smoke pattern + victims** — [cli/cli_smoke.sh](../../../cli/cli_smoke.sh)
  builds and drives victims (`auto_victim` start/kill flow at
  [:379–382](../../../cli/cli_smoke.sh#L379)); `spy_victim.c`, `auto_victim.c`
  et al. live in [cli/](../../../cli/).

## Tasks

### T0 — Extract `libasmspy` (the engine as a linkable library)  (M, depends on: none)

**Goal.** Turn the asmspy engine
([cli/asmspy_engine.c](../../../cli/asmspy_engine.c)) + the `/proc`/ELF resolver
([cli/asmspy_proc.c](../../../cli/asmspy_proc.c)) + the pure view-model headers
into a real library — **`libasmspy`** (a static `.a` the CLI and tests link,
plus a shared `.so` mirroring `libasmtest_dataflow`) with **one clean public
header** — so `--serve` (T2) and any future consumer link the *tested* engine
instead of re-declaring or re-implementing it, and the engine gains its own ABI
and a standalone test. This is a **move, not a rewrite**: the engine functions,
sinks, and the one-tracer-thread contract are byte-for-byte the existing code.
The desktop app still never links it (D9) — T0 is packaging.

**Steps.**
1. **Header split.** [cli/asmspy.h](../../../cli/asmspy.h) today mixes the engine
   API with CLI/TUI-only declarations. Carve out `cli/libasmspy.h` (**new**)
   carrying *only* the engine surface — the `asmspy_*_sink` typedefs, the
   `asmspy_engine_*` signatures, `asmspy_tree_filter_t`, `asmspy_strerror`, the
   skip-code enum, and the resolver/`psym`/`jitmap` API from `asmspy_proc` — and
   leave the CLI/TUI bits in `asmspy.h`, which now `#include`s the public header.
   The dependency-free, unit-tested view-model headers
   ([asmspy_logview.h](../../../cli/asmspy_logview.h),
   [asmspy_treefilter.h](../../../cli/asmspy_treefilter.h),
   [asmspy_dataview.h](../../../cli/asmspy_dataview.h),
   [asmspy_autoregion.h](../../../cli/asmspy_autoregion.h)) join the public
   surface. **Note the known coupling:** `asmspy_graphsort.h` `#include`s
   `asmspy.h` and uses file-scope qsort latches — 03-T4 (**new — 03**) lifts
   that; until it lands, graphsort stays CLI-side, out of the library.
2. **Static + shared targets** in [mk/cli.mk](../../../mk/cli.mk) (or a new
   `mk/asmspy-lib.mk` `include`d beside it), mirroring the `shared-dataflow`
   block ([mk/dataflow.mk:132–138](../../../mk/dataflow.mk#L132)) and the
   `shlib_*` helpers ([Makefile:251–263](../../../Makefile#L251)) exactly:
   - `$(BUILD)/libasmspy.a` = `ar rcs` over `asmspy_engine.o` + `asmspy_proc.o`
     (the current `ASMSPY_OBJS` minus `asmspy.o`).
   - `shared-asmspy: $(call shlib_dev,libasmspy)` built from `pic/` objects via
     `$(call shlib_real,libasmspy)` / `$(call shlib_ldflags,libasmspy)`.
     **Linux-only** — the engine's ~473 ptrace refs mean the `.so` self-skips
     off-Linux with a printed reason, like the other Linux-only tiers.
3. **Relink the CLI:** `asmspy` (TUI + headless) links `libasmspy.a` in place of
   the loose objects; the victims and TUI stay in `cli/`. Behaviour is
   unchanged, so **every existing cli test passes untouched**
   ([cli/cli_smoke.sh](../../../cli/cli_smoke.sh), `test_symtab`, `test_jitdump`,
   the view-model tests).
4. **Standalone smoke** `cli/test_libasmspy.c` (**new**): links *only*
   `libasmspy.h` + `libasmspy.a` and drives one engine against `spy_victim` —
   proving the library is self-contained (no hidden dependency on `asmspy.c`).
   Mirror the existing `test_symtab` link rule
   ([mk/cli.mk:353](../../../mk/cli.mk#L353)).

**Code.** No logic change — the diff is header partitioning + build rules. If any
symbol must move out of `asmspy.c` to satisfy the link (a helper the engine
calls), move it into `asmspy_engine.c`/`asmspy_proc.c`, never duplicate it.

**Tests.** Existing `make cli-smoke` green and unchanged; the new `test_libasmspy`
links against the public header + `.a` only and passes; `make shared-asmspy`
produces the `.so` on Linux (x86-64 **and** arm64) and self-skips elsewhere.

**Docs.** A "libasmspy" note in
[docs/guides/tracing/asmspy.md](../../guides/tracing/asmspy.md) (the engine is
now a linkable tier, like the emulator/dataflow libs) + CHANGELOG `Added`.

**Done when.**
- `make cli-smoke` is green with **no behaviour change** (a pure repackaging).
- `test_libasmspy` links against *only* `cli/libasmspy.h` + `libasmspy.a`.
- `make shared-asmspy` builds the `.so` on Linux and self-skips off-Linux.
- Nothing in `desktop/` links `libasmspy` (grep the desktop build — D9 preserved:
  the desktop app reaches the engine only through the `--serve` subprocess).

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

### T2 — `--serve` as a thin wrapper over `libasmspy`  (M, depends on: T0, T1; 01's writer TU)

**Goal.** `asmspy --serve[=<socket>]` — read commands, run **one engine
session at a time** on a dedicated tracer thread, stream 01's events. The serve
loop is a **thin wrapper**: it links `libasmspy` (T0) and drives its public
engine API; it embeds no engine logic of its own.

**Steps.**
1. Add the flag to the dispatch in `main` (beside the other subcommands) and a
   `cmd_serve` function, in `cli/asmspy.c` — which now links `libasmspy.a`
   (T0), so `cmd_serve` calls the engines through `cli/libasmspy.h` exactly like
   the TUI does.
2. Session start: validate the budget (T4 rules) and flag matrix (tid XOR
   follow; auto XOR tid; module/sampler only with auto — mirror the existing
   refusals in the arg parsing), then `pthread_create` a tracer thread that
   calls the one `libasmspy` engine for the mode with the NDJSON sinks from 01's
   writer TU (**new — 01**) as `sink`/`ctx`, exactly as `run_live_view`
   structures it ([cli/asmspy.c:2714](../../../cli/asmspy.c#L2714)).
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
3. Wire into the existing cli test target so `make cli-smoke` (and the docker
   lane) runs it.

**Done when.** `make cli-smoke` green with the serve section; the victim
survives; the refusal case fires.

## Task order & parallelism

T0 (extract `libasmspy`) and T1 (protocol spec) are independent and start
immediately; T2 (the `--serve` wrapper) needs both. Then T3 (against fake-serve)
in parallel → T4 → T5; T6 follows T2. Critical path: T0 → T2 → T6. 08's views
consume T3's sessions; nothing in 08 blocks T0–T6. **T0 is worth landing on its
own** even before the GUI — it makes the engine an independently testable,
reusable library (a future binding surface), and turns `--serve` into a wrapper
instead of a second copy of the engine's drive loop.

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
