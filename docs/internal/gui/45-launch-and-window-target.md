# Two new home-menu entries: launch a process, and target one by window-pick

> **Sources.** Not cut from a prior analysis doc — a fresh two-feature request:
> add a Home-rail option that launches a new process and traces it from birth,
> and a second option that targets an already-running process by dragging a
> crosshair onto its window. Both land beside the existing
> Learn/Open/Capture/Author CTAs (`draw_home_rail`,
> `desktop/src/ui/shell.cpp:575-635`). Read
> [_conventions.md](../implementations/_conventions.md) first; D1–D11 live in
> this directory's [README](README.md). **Prerequisites: none** — Part B
> (window-pick) is pure desktop/platform code touching nothing under `src/` or
> `cli/`; Part A (launch) is new engine surface but depends on no other
> in-flight brief.
>
> Authored 2026-07-31 against HEAD `5ef06e5`. Every file:line below was read
> directly against that tree while writing this doc (not carried over from an
> earlier survey) — if a citation disagrees with the code when you implement,
> the code wins; re-verify, then fix this doc in the same change.
>
> **Status (2026-07-31) — ☐ 0/9.** Not started.

## Why this work exists

Today the Home rail has exactly one way onto a live target: **Capture a live
process** (`mode_cta(Mode::Capture)`, `ui/mode.h:49-50`), which lands on the
Processes pane — a searchable table of `/proc`, scanned once, picked by PID
(`draw_processes_pane`, `ui/inspect_door.cpp:976+`). That covers "the process
is already running and I can find it in a list." It does not cover two other
common starts:

1. **"I have a binary/command I want to run and watch from its very first
   instruction"** — there is no launch path anywhere in the tree. Every
   headless CLI mode, and the `--serve` wire protocol's `start` command, take
   a `pid` (`cli/asmspy.c` usage text, `serve_parse_start`,
   `cli/asmspy.c:4123+`); attaching necessarily begins wherever the target
   already happens to be in its execution, and the earliest syscalls/calls/
   dataflow are gone before the tracer catches up.
2. **"I can see the process on my screen right now (a window), I just don't
   know or want to look up its PID"** — the closest tool-culture analogy is
   Spy++'s or Process Explorer's crosshair-drag window finder. Nothing like it
   exists here; picking a target is text/table-driven only.

Both close a real gap in the "attach by PID from a table" model without
replacing it — the Processes pane stays the ground truth for "what's running
and can I attach to it," these are two more ways to arrive at a `selected_pid`
(or, for launch, no PID until the target exists).

## What already exists (verified 2026-07-31)

- **The Home rail is a fixed sequence of CTA calls, not a data table.**
  `draw_home_rail` (`ui/shell.cpp:575-635`) builds each button through a local
  `cta(Mode m, const char *caption)` lambda (`shell.cpp:581-592`) that calls
  `shell_select_mode(s, m)` on click; the four calls are hand-written at
  `shell.cpp:595-603` (Learn, Open, Capture, Author, in that order). Adding a
  new top-level entry means adding a `Mode` value and one more `cta(...)` line
  here — there is no registry to extend elsewhere.
- **`Mode`** (`ui/mode.h:20`) is `{Learn, Open, Capture, Author, Inspect}`;
  `mode_cta` (`mode.h:43-57`) supplies the button label and `mode_preset`
  (`mode.h:27-39`) the `LayoutPreset` (`ReplayInspect`/`Author`/
  `LiveObserver`) each mode leads with. `Inspect` is the live-session posture
  `Capture` transitions into once a session is up (`mode.h:17-19`) — both map
  to `LayoutPreset::LiveObserver`.
- **`shell_select_mode`** (`shell.cpp:445-485`) is the one seam every CTA
  click runs through: sets `s.mode`, logs a line (`shell_log_push`,
  `shell.h:486`, `ToastKind::Info`), applies the mode's default pane set
  (`shell_apply_mode_panes`), and per-mode side effects — for
  `Capture`/`Inspect` that's `s.inspect.want_autoconnect = true` +
  `s.inspect.want_focus_processes = true` (`shell.cpp:465-480`), which the
  docked shell consumes once to connect the serve host from saved Settings and
  bring the Processes pane forward.
- **`InspectState`** (`ui/doors.h:143+`) is the live-workflow state: a
  `LiveSession session` (§ below), `asmspy_path`/`ssh_host` char buffers
  (`doors.h:148-149`), the scanned `rows`/`selected_pid` (`doors.h:155-157`),
  and a family of one-shot `want_open_*`/`want_focus_*` bools
  (`doors.h:206-223`) that let the engine-free door code (which cannot reach
  `ShellState`) request a pane reveal that `draw_shell` consumes once per
  frame. Both new features add state here, following that same shape.
- **Attach today, end to end:** `inspect_scan` (`inspect_door.cpp:28-40`)
  calls `list_processes()` (`/proc`-only, `live/inspect.cpp:315-373`) into
  `s.rows`; a Processes-row double-click or its context menu calls
  `inspect_attach_full_detail(InspectState&, long pid)`
  (`inspect_door.cpp:203-222`), which sets `selected_pid`, picks
  `LiveMode::Auto`, connects the host if needed (`inspect_connect`,
  `inspect_door.cpp:42-51`), and calls `inspect_request_start`
  (`inspect_door.cpp:81-121`) — which sends
  `s.session.send_start(mode_name(s.want), s.selected_pid,
  inspect_start_params(s))` (`inspect_door.cpp:116-117`, `LiveSession::send_start`
  declared `live/session.h:103`). **Both new features end at exactly this
  call** — launch supplies a PID it just created instead of one picked from a
  row; window-pick supplies a PID resolved from a screen point instead of a
  table selection. Neither needs a new attach path once a PID exists.
- **The only `fork`/`exec` in the desktop app spawns the RPC host, not a
  target.** `LiveSession::start` (`live/session.cpp:62-150`) forks and
  `execvp`s `asmspy --serve` (local) or `ssh -T <host> asmspy --serve`
  (remote) (`session.cpp:80-134`) and pipes NDJSON over the child's stdio —
  this is D9 (`docs/internal/gui/README.md:77-79`): "the desktop app never
  links the ptrace engines." **Nothing under `desktop/` may itself
  `PTRACE_TRACEME`/`PTRACE_SEIZE` a target** — launch's actual fork+trace must
  happen inside `asmspy` (wherever `--serve` is running, local or remote),
  which is also what makes launch ssh-transparent for free: the same spec
  that already runs the capture host locally or over ssh
  (`LiveSession::Spec`, `session.h`) decides where the launched target runs
  too.
- **The wire protocol's `start` command is the only place a PID enters the
  engine, and the reply already carries one back.** Server dispatch:
  `cmd_serve`'s command loop in `cli/asmspy.c`, the `"start"` branch at
  `asmspy.c:4495-4568`. `serve_parse_start` (`asmspy.c:4123+`) fills
  `serve_params_t s.p` (mode, `pid`, region) from the wire JSON; on success the
  server replies with an ack (`"cmd":"start"`, `asmspy.c:4543-4544`) **and**
  a `session`/`started` event that already includes `"pid":%d`
  (`asmspy.c:4545-4548`) before spawning the tracer thread
  (`pthread_create(..., serve_tracer, &s)`, `asmspy.c:4561`). A `launch`
  command that doesn't know its PID until after it forks can reuse this
  **exact same reply shape** — the client-visible protocol for "which PID am
  I now tracing" already exists; nothing new needs adding to the schema.
- **No fork+`execvp`-of-an-arbitrary-external-binary exists anywhere in the
  engine either — the nearest thing is a different pattern for a different
  purpose.** `asmtest_dataflow_ptrace_run` (`src/dataflow_ptrace.c:1374`,
  called from CI-fixture/parity tests like `cli/test_regstate_parity.c:116`)
  and `ptrace_backend.c`'s several `PTRACE_TRACEME` sites
  (`src/ptrace_backend.c:979,2014,2211,2579,3724,4166`, backing the
  out-of-process single-step trace backend, top-of-file comment
  `ptrace_backend.c:1-23`) all fork a child that runs a **JIT stub jumping
  into an in-memory code buffer** (`const uint8_t *code, size_t code_len`) —
  the Author-mode / deterministic-fixture idiom, not "launch this external
  command line." The actual `fork()`+`ptrace(PTRACE_TRACEME,...)`+`waitpid`
  handshake shape (`ptrace_backend.c:1062`'s comment: "Blocking waitpid for
  the initial post-fork PTRACE_TRACEME handshake") is the one piece worth
  cloning; the exec target is not.
- **Production PID-attach is `PTRACE_SEIZE`, implemented per-mode in
  `cli/asmspy_engine.c`.** The whole-process modes (log/stream/graph/tree/
  procs) seize every thread via `seize_threads`/`seize_one`
  (`asmspy_engine.c:2151-2200`, walking `/proc/<pid>/task` and
  `PTRACE_O_TRACECLONE`-following new ones); the region-sample engine
  (`--sample`) seizes every thread and races a shared breakpoint
  (`asmspy_engine.c:2903-2919`'s comment explains why — worker-thread
  targeting, mirroring `src/dataflow_ptrace.c`'s `dfp_seize_all`). **A
  process already under `PTRACE_TRACEME` cannot be `PTRACE_SEIZE`d again** —
  `live/inspect.cpp:95`'s own comment states the rule plainly: "a tracee has
  exactly one tracer." T2 below has to thread that fact through these entry
  points rather than re-run them unmodified.
- **Yama `ptrace_scope=1` (the common distro default) already can't promise
  attach will work — launch sidesteps exactly that uncertainty.**
  `attach_verdict` (`live/inspect.cpp:133-150`) returns `Attach::Unknown` for
  scope 1 unless the target already opted in via `PR_SET_PTRACER`, because
  "whether we are a descendant is knowable, but whether the target opted in
  is not readable from outside" (`inspect.cpp:134-137`). A **launched** child
  unambiguously *is* asmspy's descendant by construction, so this whole
  branch never applies to it. (Scope 3, "disabled kernel-wide"
  (`inspect.cpp:113`) may or may not also block a direct child's
  `PTRACE_TRACEME` depending on the kernel's exact enforcement — verify this
  against real kernel behavior at implementation time rather than assuming;
  everything below scope 3 is safe to assume permits it.)
- **The icon glyphs for the crosshair affordance are already baked into the
  font atlas.** `fonts.cpp:8-9` merges Codicons and FontAwesome6 into the one
  ImGui font (`fonts.cpp:67-79`, gated on the TTF being readable, same
  pattern already used for e.g. `ICON_CI_INFO`/`WARNING`/`ERROR` at
  `views/canvas_draw.cpp:43-49`). `ICON_FA_CROSSHAIRS` is defined at
  `build/addons/imguinotify-*/IconsFontAwesome6.h:384`; `ICON_CI_TARGET` at
  `build/addons/iconfontcppheaders-*/IconsCodicons.h:511`. Either renders
  today from any pane with no new atlas work.
- **No window-picker precedent exists anywhere in the tree.** `IsMouseDragging`
  has exactly two call sites in `desktop/src` — 3D orbit-camera drag
  (`shell.cpp:937`) and slice-view canvas pan (`views/slice_view_draw.cpp:148`)
  — and neither is a drag-out-of-the-window idiom. `SetMouseCursor`,
  `ImGuiMouseCursor`, `BeginDragDropSource`, and any X11 header
  (`X11/Xlib.h`, `XQueryPointer`, `_NET_WM_PID`, …) are used nowhere in
  `desktop/src`. This is genuinely new platform code, not a wiring task.
- **The app already treats X11-vs-Wayland as an honestly-degraded axis, not a
  silent one — the precedent to follow.** `main.cpp:137-150`'s window-icon
  setup: "[on Wayland] there is no per-window icon protocol... `glfwSetWindowIcon`
  below is a no-op" — stated in a comment, not hidden. `mk/desktop.mk:691-698`
  branches the whole build/run epilogue on `Darwin` (Quartz, no
  `DISPLAY`/`WAYLAND_DISPLAY` at all). The window-picker should degrade the
  same honest way: a real capability on X11, a stated unavailable elsewhere,
  never a control that looks live and silently does nothing.
- **The pane-registration pattern (for the new Launch pane, T4) is
  fully established and this doc reuses it verbatim** — see
  [19-dockable-panes-keystone.md](19-dockable-panes-keystone.md). Concretely:
  declare `extern const char *const kPaneXxx` (`ui/layout.h:70-97`, e.g.
  `kPaneConnect`/`kPaneProcesses`/`kPaneCapture` at `layout.h:87-89`), define
  the literal string (`layout.cpp:10-22`, e.g. `layout.cpp:17-19`), dock it
  in the default split (`layout.cpp:64-137`, e.g. `layout.cpp:105-107`),
  register a `PaneDef{name, default_open, context_ok, unavailable_hint}` in
  `kManagedPanes[]` (`shell.cpp:2032-2064`), decide which mode(s) open it by
  default in `mode_wants_pane` (`shell.cpp:2089-2125`), and `Begin()`/`End()`
  it in `draw_shell`'s docked block guarded by `pane_shown(s, kPaneXxx)` —
  View ▸ Panels then lists it automatically (the loop over `kManagedPanes` at
  `shell.cpp` that this doc did not need to re-read; `19`'s brief documents it
  fully).
- **The file-open-dialog precedent for "browse to an executable" already
  exists — it's the exact shape, just repointed.** `draw_open_dialog`
  (`shell.cpp:1492-1518`) uses `ImGuiFileDialog::Instance()` with
  `IGFD::FileDialogConfig` (`shell.cpp:1496-1502`) to pick a `.asmtrace` file;
  `author_door.cpp:300-323` and `inspect_door.cpp:778-792` reuse the same
  library for save-with-confirm-overwrite. Launch's "Browse…" button is the
  open-dialog form again, with no extension filter (or `"*"`) instead of
  `.asmtrace`.

## Part A — Launch a process (T1–T5)

### T1 — The launch primitive + wire `launch` command (L)

**Goal.** `asmspy --serve` accepts a new `{"cmd":"launch","mode":...,
"argv":[...]}` command that forks a child, `PTRACE_TRACEME`s + `execvp`s the
requested command, and feeds the resulting PID into the SAME per-mode engine
path `start` already uses — landing first with an **honestly-flagged interim
fidelity gap** (below), not full from-birth capture (that's T2).

**Steps.**
1. `cli/asmspy.c`: add a `launch` branch parallel to the `"start"` dispatch
   (`asmspy.c:4495-4568`). New wire fields: `"argv":["path","arg1",...]`
   (required, non-empty) and optional `"cwd"`; reuse the existing `"mode"`
   field verbatim (`serve_mode_name`/`SM_*`, same enum `start` already
   parses) so a client's mode selector needs no new vocabulary.
2. New helper (e.g. `serve_launch_target(const char **argv, const char *cwd,
   pid_t *out_pid, char *err, size_t errlen)`): `fork()`; child calls
   `ptrace(PTRACE_TRACEME, 0, NULL, NULL)`, `chdir(cwd)` if given, `execvp`,
   `_exit(127)` on failure — mirror `session.cpp:108-134`'s existing
   fork/pipe/exec shape (this file already does a fork+execvp cleanly; the
   only additions are the `PTRACE_TRACEME` call and no stdio piping, since
   the target's own stdio should inherit the server's, not asmspy's NDJSON
   channel). Parent `waitpid`s for the post-exec stop, mirroring the
   handshake `ptrace_backend.c:1062`'s comment already documents (that file's
   own version execs an in-process stub — same waitpid shape, different exec
   target). A child that `_exit(127)`s (exec failed — bad path, ENOENT,
   permission) must surface as a clean `serve_err(&s, "launch", "exec
   failed: <path>: <reason>")`, never a hang.
3. **Interim step (this task): `PTRACE_DETACH` the freshly-TRACEME'd child
   immediately after the exec stop**, letting it resume as an ordinary
   running process, then set `s.p.pid` to it and fall through into the
   **existing, unmodified** `"start"` machinery (`asmspy.c:4540-4567`) —
   i.e., a SEIZE-based attach races to catch up with a process that has
   already been running, briefly, on its own. This is deliberately the
   cheap/low-risk version — **it does not deliver "from the very first
   instruction"** (see Non-goals) and must say so: do not report this
   session's trust tier the same as a mode's normal `"exact"`
   (`SERVE_MODES[].trust`, `asmspy.c:3166-3167,3978`) without a distinguishing
   note — reuse the existing skip/reason vocabulary (the pattern at
   `asmspy.c:4519-4537`'s `ASMSPY_ETRACEE_I386` skip) rather than inventing a
   new fidelity concept; the exact mechanism is an implementer decision, the
   requirement is that a client can tell "this session may have missed
   startup events" from the reply, not have to assume it.
4. Reply shape: reuse the ack + `session`/`started` shape verbatim
   (`asmspy.c:4543-4548`), which already carries `"pid"` — a launch client
   needs no new field to learn the spawned PID.
5. `--launch` as a headless CLI flag too (mirrors every other mode's `--foo
   <pid>` having a `--serve` sibling): `asmspy --launch <mode> -- <cmd>
   [args...]`, following the existing `usage()` text block
   (`asmspy.c:7542-7636`) format and argument-parsing precedent
   (`main`, `asmspy.c:7639+`).

**Tests.** A `cli_smoke`-style test (mirrors the project's existing
host-testable-without-hardware bar, per CLAUDE.md — this needs no PT/IBS
silicon, just `fork`/`ptrace`, which every CI lane already has) that launches
a trivial fixture victim (reuse one of `cli/*_victim.c`) via `--launch log`,
asserts it traces the target's syscalls, and that a bad path (`--launch log
-- /no/such/binary`) replies with a clean error, not a hang or crash.

**Done when.** `{"cmd":"launch",...}` produces a running, traced session
whose `session`/`started` event carries the real spawned PID, indistinguishable
downstream from an attach-based session except for the stated interim-fidelity
note; `--launch` works headless the same way.

### T2 — True from-birth fidelity: skip the SEIZE, keep the TRACEME stop (L)

**Goal.** Close T1's interim gap: the launched child never runs untraced, not
even briefly — every instruction/syscall from the process's true entry point
is captured, which is the entire premise of "launch" over "attach quickly
after starting by hand."

**Steps.**
1. Replace T1 step 3's detach-then-reattach with: keep the child stopped at
   its post-exec `PTRACE_TRACEME` stop, install the SAME `PTRACE_O_*` options
   each mode's SEIZE path would have installed (`PTRACE_O_EXITKILL` at
   minimum, per `dataflow_ptrace.c:1530-1533`'s comment on why EXITKILL
   matters), and hand the pid to that mode's engine entry point **flagged as
   "already ours"** so it builds its thread table from the one known tid
   instead of calling `ptrace(PTRACE_SEIZE, ...)` again (which would fail —
   `inspect.cpp:95`, "a tracee has exactly one tracer").
2. Enumerate every SEIZE/ATTACH call site in `cli/asmspy_engine.c` serving
   the modes launch should support (grep `PTRACE_SEIZE\|PTRACE_ATTACH` fresh
   at implementation time — this doc verified `seize_threads`/`seize_one`,
   `asmspy_engine.c:2151-2200`, and the region-sample engine's seize-all-race,
   `asmspy_engine.c:2903-2919`, as two concrete sites; do not assume this list
   is exhaustive). Thread an `already_traced` (or equivalent) parameter
   through each, branching to "tab the known tid, skip SEIZE, still do
   `PTRACE_INTERRUPT`-equivalent setup if the mode needs it" — same shape as
   `seize_threads`'s existing per-thread bookkeeping (`thr_get`/`thr_tab_t`,
   `asmspy_engine.c:2151-2160`), minus the syscall that would now fail.
3. Continue the child from its TRACEME stop only once the engine's normal
   "ready to watch" point is reached (mirror wherever the SEIZE path already
   does its first `PTRACE_CONT`/`PTRACE_SYSCALL` after setup) — so nothing
   about the recorded event stream differs from an attach except that it now
   starts at the process's actual first instruction instead of wherever
   attach happened to land.
4. Retire T1 step 3's interim fidelity caveat once this lands — the recorded
   session is honestly exact, same as any other whole-process attach.

**Tests.** Extend T1's `cli_smoke` launch test: assert the recorded event
stream's FIRST event is plausibly early (e.g., for `--launch log`, the
target's very first syscall, not one already several calls in) — compare
against a golden captured by having the SAME victim binary self-report its
own first syscall via a marker, or by comparing against an emulator/static
oracle if one already exists for the fixture (check `docs/internal/
implementations/_conventions.md`'s oracle-comparison convention before
inventing a new one).

**Done when.** A launched target's recorded session provably starts at (or
provably before any syscall/call the fixture makes past) the process's true
entry — no detach/reseize gap remains, and T1's interim trust caveat is
removed from the reply.

### T3 — Desktop client plumbing (S)

**Goal.** `InspectState`/`LiveSession` can send a `launch` command and learn
the resulting PID, mirroring `send_start`/`selected_pid` exactly.

**Steps.**
1. `live/session.h`/`.cpp`: `LiveSession::send_launch(const std::string
   &mode, const std::vector<std::string> &argv, const std::string &cwd,
   const nlohmann::json &params)` — same shape as `send_start`
   (`session.h:103`, `session.cpp:171+`), building the `{"cmd":"launch",...}`
   line instead of `{"cmd":"start",...}`.
2. `ui/doors.h`'s `InspectState`: new fields alongside `region`/`selected_pid`
   — `char launch_cmd[512]`, `char launch_args[512]`, `char launch_cwd[512]`
   (mirror the existing fixed-buffer style at `doors.h:148-149,174`, not
   `std::string`, to match the file's own convention for ImGui-editable
   text). No `want_focus_launch`-style flag needed yet — T4 wires that.
3. `inspect_door.cpp`: `inspect_launch_full_detail(InspectState&)` mirroring
   `inspect_attach_full_detail` (`inspect_door.cpp:203-222`) but calling
   `send_launch` instead of `send_start` with a PID, and — since there is no
   PID yet at connect time — setting `selected_pid` only once the `session`/
   `started` event's `"pid"` arrives (find wherever the client already parses
   `session` events for `state:"started"` — likely near where `active`/
   `awaiting_started` are updated, `inspect_door.cpp:195-201` is one such
   site — and thread the PID through there for the launch case specifically,
   since attach already knows its PID before this event and must not be
   changed to depend on it).

**Tests.** `test_inspect`-level unit test of `inspect_launch_full_detail`
against a fake/null session transport (mirror whatever existing test drives
`inspect_attach_full_detail` headlessly, if one exists — `desktop/test/
test_inspect.cpp`).

**Done when.** Calling the new function with a command/args produces the
right `launch` wire line and, once a `session started` event with a `pid`
arrives, `selected_pid` reflects it — provable without a real `asmspy`
process (a fake transport / recorded fixture line is enough).

### T4 — `Mode::Launch` + the home-rail CTA + `kPaneLaunch` (M)

**Goal.** A fifth Home-rail entry, "Launch a process", opens a form (command,
arguments, working directory, an engine-mode selector) and a "Launch & trace"
button — landing on the SAME Capture/Observer machinery every attach already
uses.

**Steps.**
1. `ui/mode.h`: add `Launch` to `enum class Mode` (`mode.h:20`); `mode_cta`
   returns e.g. `"Launch a new process"` (`mode.h:43-57`); `mode_preset`
   returns `LayoutPreset::LiveObserver` (same as Capture/Inspect, `mode.h:
   27-39`) — it is the same live-workflow layout, not a new preset.
2. `shell_select_mode` (`shell.cpp:445-485`): a `case Mode::Launch:` branch —
   likely `s.show_inspect = true` (same windowed-shell flag Capture sets,
   `shell.cpp:471`) plus a NEW one-shot flag (e.g.
   `s.inspect.want_focus_launch`) instead of `want_focus_processes`, so the
   docked shell brings the Launch pane forward rather than Processes. Launch
   does NOT auto-connect the same way (`want_autoconnect`) — connecting
   before the user has typed a command to launch is premature; connect only
   when "Launch & trace" is pressed (`inspect_connect` inside
   `inspect_launch_full_detail`, T3).
3. `ui/shell.cpp`'s home rail (`shell.cpp:594-603`): one more `cta(Mode::Launch,
   "start a fresh process and trace it from the first instruction")` line,
   placed after `Mode::Capture`'s (same "or:" grouping, `shell.cpp:598-600`)
   since it's an alternate way into the same live workflow, not a fifth
   unrelated task.
4. New pane: `kPaneLaunch` declared/defined per the pattern in "What already
   exists" above (`layout.h`/`layout.cpp`), docked near `kPaneConnect`/
   `kPaneProcesses` (`layout.cpp:105-107`'s left-rail placement), registered
   in `kManagedPanes[]` (`shell.cpp:2032-2064`) with a `pctx_always`-style
   gate (it needs no prior selection, unlike `pctx_capture`), and opened by
   default for `Mode::Launch` in `mode_wants_pane` (`shell.cpp:2089-2125`).
5. `draw_launch_pane(InspectState&)` (new, in `inspect_door.cpp` or a new
   `launch_door.cpp` if it grows large): a command text field + "Browse…"
   button using `ImGuiFileDialog` exactly like `draw_open_dialog`
   (`shell.cpp:1492-1518`) but with no extension filter, an arguments field,
   a working-directory field, the same mode selector the Processes pane's
   context menu already offers ("Attach & trace (full detail)" / "Trace a
   function… (dataflow)", `inspect_door.cpp:1117-1134`, generalized to a
   shared widget both panes call if the code is identical enough — verify
   before extracting, do not force a shared widget that doesn't actually
   match), and a "Launch & trace" button calling
   `inspect_launch_full_detail` (T3).
6. Wire the standalone/windowed door composer too (`draw_inspect_door`,
   `doors.h:409`+, called from the single-window shell) — it stacks the same
   pane bodies inline (per "What already exists" §6 in the original research,
   and the existing Connect/Processes/Capture precedent), so
   `draw_launch_pane` must work standalone with no dock-node dependency, same
   as its siblings.

**Tests.** `test_shell.cpp`: entering `Mode::Launch` reveals `kPaneLaunch`
(and not `kPaneProcesses`); typing a command and pressing "Launch & trace"
(against the null/test backend) issues the expected `send_launch` call —
mirror whatever existing `test_shell` case proves the Capture CTA reaches
`inspect_attach_full_detail` via a row click, if one exists, as the pattern
to clone for Launch's button.

**Done when.** The Home rail shows five CTAs; clicking Launch opens
`kPaneLaunch` with no Processes-pane detour; a filled-in command reaches
`send_launch` with the right argv/cwd/mode; `View ▸ Panels` lists Launch like
every other managed pane, for free.

### T5 — End-to-end proof + docs (S)

**Goal.** One `docker-desktop`-lane and one `cli-smoke`-lane test prove the
whole chain — Home rail → Launch pane → wire `launch` → traced-from-birth
recording — the way `docs/internal/gui/39`'s `auto_pick` walk or `44`'s
terrain reskin close with a full-stack assertion, not just per-task unit
tests.

**Steps.** A `cli_smoke` case launching a real fixture victim through the
full `asmspy --serve` NDJSON protocol (not the in-process helper T1/T2's
tests use) and asserting the recorded `.asmtrace` stream is well-formed and
starts at the target's true entry; a `test_shell` case confirming the desktop
side renders the resulting live session exactly like an attach would (same
Observer deck, same Log entries) once `selected_pid` resolves. Update
`docs/internal/gui/README.md`'s table with this doc's row once landed
(follow the exact style of the doc-39/40/41 entries already there).

**Done when.** Both lanes are green in CI with no hardware gate (launch needs
no PT/IBS/debug-register silicon — only `fork`/`ptrace`, which every runner
has); README's doc index carries doc 45's row.

## Part B — Target a running process by window-pick (T6–T9)

### T6 — Platform window→PID resolver, X11-only (M)

**Goal.** A small, pure-ish platform module answers "which PID owns the
window at this screen point," honestly refusing (not silently failing) on
Wayland or macOS.

**Steps.**
1. New `desktop/src/platform/window_picker.h`/`.cpp` (new directory — this is
   genuinely new platform surface, not an addition to `ui/` or `live/`):
   `bool window_picker_supported();` (true only when an X11 display is
   actually reachable — `XOpenDisplay(NULL)` succeeds; do not just check
   `getenv("DISPLAY")`, which can be stale under pure Wayland/XWayland-absent
   sessions) and `struct PickedWindow { bool ok; long pid; std::string
   title; std::string why_not; };` plus `PickedWindow
   resolve_window_at_screen_point(int x, int y);`.
2. X11 implementation: from the root window, walk `_NET_CLIENT_LIST_STACKING`
   (top-to-bottom) or recurse `XQueryTree`, hit-test each top-level's
   geometry (`XGetWindowAttributes` + `XTranslateCoordinates`) against
   `(x,y)`, take the topmost match; read `_NET_WM_PID` off it — if absent on
   the hit window (common: the point may land on a WM decoration/frame
   subwindow, not the client), walk up/down the hierarchy to the nearest
   window that carries the property, same as any EWMH-aware tool
   (`wmctrl`/`xdotool`) has to.
3. `PickedWindow.why_not` covers: not supported (no X11 display), no window
   at that point, window has no readable `_NET_WM_PID` (a non-EWMH client —
   real, must be stated, not silently mapped to pid 0), and — a case worth
   deciding explicitly, not assuming — a resolved PID that is not on the SAME
   host `asmspy --serve` is running against (only meaningful for a LOCAL
   capture; T8 gates this at the call site rather than here, so the resolver
   itself stays a pure "what's at this point" function).
4. `#ifdef`/build-time guard so this compiles to a stub returning
   `supported()==false` on non-Linux or when X11 dev headers are absent —
   mirror how `inspect.cpp:299-313`'s `local_inspect_unavailable` already
   degrades non-Linux hosts for process listing; this is the same posture,
   one layer further into windowing.

**Tests.** A headless test is the wrong bar here (X11 needs a real or virtual
display) — see T9 for the Xvfb-backed integration test. A pure test IS
possible for whatever part parses `_NET_WM_PID`/window geometry from
synthetic X11 property data if the implementation separates "talk to a live
display" from "interpret an XEvent/property blob" — worth doing if it falls
out naturally, not a hard requirement.

**Done when.** `window_picker_supported()` is accurate (true on a live X11
session including XWayland, false under pure Wayland/no display/macOS);
`resolve_window_at_screen_point` correctly identifies a real window's owning
PID against a live X server (proven in T9, not here).

### T7 — The crosshair drag affordance on the Home rail (S)

**Goal.** A crosshair icon button on the Home rail, greyed with a stated
reason when unsupported; dragging it — even outside the app's own window —
tracks the OS cursor as a crosshair until release.

**Steps.**
1. `ui/shell.cpp`'s home rail: an icon button (`ICON_FA_CROSSHAIRS` or
   `ICON_CI_TARGET`, both already in the merged atlas — "What already
   exists" above) near the Capture CTA. Disabled (ImGui `BeginDisabled`) with
   a tooltip when `!window_picker_supported()` **or** `s.inspect.ssh_host[0]
   != '\0'` (window-pick only means anything for a LOCAL capture target —
   state this in the tooltip, same `unavailable_hint` idiom `kManagedPanes`
   already uses, `shell.cpp:2032-2064`).
2. Drag start: `ImGui::IsItemActive() && ImGui::IsMouseDragging(
   ImGuiMouseButton_Left)` on the button (same idiom as the two existing
   `IsMouseDragging` sites, `shell.cpp:937`, `slice_view_draw.cpp:148`, just
   not gated to "inside a 3D viewport") sets a new `bool s.picking_window`
   and swaps the OS cursor via `glfwSetCursor(window, crosshair_cursor)`
   using GLFW's **built-in standard cursor** `glfwCreateStandardCursor(
   GLFW_CROSSHAIR_CURSOR)` (cached once at startup, not per-frame) — no need
   to rasterize the FontAwesome glyph into a cursor bitmap, GLFW already
   ships this shape.
3. While `s.picking_window` is true, each frame: track the pointer in
   **screen** coordinates, not ImGui's window-relative `GetMousePos()` (which
   goes stale once the cursor leaves the app's own window bounds — most
   window systems keep delivering motion to the window that grabbed the
   button-down, but GLFW's cursor-pos API is specified relative to the
   window and its behavior once the cursor is outside those bounds needs
   verifying against GLFW's actual guarantee before relying on it; the safe
   choice is to read the global pointer position directly from
   `window_picker`'s X11 handle, same display connection T6 opens, rather
   than trust `glfwGetCursorPos` beyond the window edge).
4. On `ImGui::IsMouseReleased(ImGuiMouseButton_Left)` while
   `s.picking_window`: restore the default cursor, clear the flag, call T6's
   `resolve_window_at_screen_point` at the release point — T8 continues from
   here.

**Tests.** GLFW cursor/global-pointer behavior needs a real or virtual
display (T9); the pane-level state machine (`picking_window` flips true on
drag-start, false on release, independent of whether a window was actually
found) is unit-testable against synthetic ImGui IO state the same way
existing `test_shell` interaction cases drive button clicks.

**Done when.** Dragging the crosshair button changes the OS cursor
immediately and it stays a crosshair even once the pointer leaves the app's
own window (manual/visual verification acceptable, matching this codebase's
existing bar for GL/OS-visual-only correctness, e.g. `test_scene_fbo`'s "GL
smoke ran" level); the control is visibly disabled with a stated reason under
SSH capture or no X11.

### T8 — Resolve on release → reuse the existing attach path (S)

**Goal.** Releasing the drag over a real window attaches to its owning
process through the SAME code path a Processes-row click already uses — no
new attach logic.

**Steps.**
1. On the `PickedWindow` T7's release handler gets: if `!ok`, push a log line
   via `shell_log_push(s, picked.why_not, ToastKind::Warning)`
   (`shell.h:486`, same mechanism `shell_select_mode` already uses,
   `shell.cpp:449`) — never a silent no-op.
2. If `ok`: call `inspect_attach_full_detail(s.inspect, picked.pid)`
   (`inspect_door.cpp:203-222`) **unchanged** — this already connects the
   host from saved Settings if needed, sets `LiveMode::Auto`, and requests a
   start; then call `shell_select_mode(s, Mode::Capture)` (or set `s.mode`
   directly plus whatever `shell_select_mode` does beyond the door call, if
   calling it twice would double-log) so the user lands in the Capture
   layout with the target already attached, exactly as a Processes double-
   click does today.
3. A resolved PID that turns out not attachable (the existing
   `attach_verdict`/`ProcRow` machinery, `inspect.h:171`) is not this
   feature's problem to re-diagnose — `inspect_attach_full_detail`'s existing
   failure path (`inspect_door.cpp:216-221`, falls back to revealing Connect
   with `host_error`) already handles it identically to a table-driven
   attach.

**Tests.** `test_shell.cpp`: a synthetic "picked window resolved to PID N"
event drives the same assertion an existing Processes-row-click test already
makes (that `inspect_attach_full_detail` was reached with the right PID and
the Capture layout is active) — if such a test exists, extend it with a
second entry point rather than duplicating the assertion body.

**Done when.** A successful pick attaches exactly as a table pick would (same
downstream state, same pane reveal); a failed pick logs a stated reason and
touches no session state.

### T9 — Build dependency + Xvfb-backed integration test (M)

**Goal.** The X11 dev headers T6 needs are a pinned, documented image
dependency (per this repo's CLAUDE.md: "a missing dependency is not a
blocker... add it where the work runs"), and the picker's correctness is
proven against a REAL (virtual) display with a second real window, not
asserted from reading the code.

**Steps.**
1. `Dockerfile.desktop`: add `libx11-dev` (and `libxext-dev` if
   `XTranslateCoordinates`/geometry calls need it) alongside the existing
   `libglfw3-dev libgl1-mesa-dev libgl1-mesa-dri libegl1-mesa-dev`
   (`Dockerfile.desktop:34`) — an apt package pin, matching CLAUDE.md's "apt,
   where a pinned distro package suffices" pattern (the same doc's own
   example: `libipt-dev`). GLFW already links X11 transitively at the
   shared-library level; this adds the Xlib **development headers** the new
   `window_picker.cpp` compiles against directly.
2. Extend whichever `docker-desktop*` test lane runs `desktop-test`
   (`mk/desktop.mk`) with `xvfb-run` (or an already-backgrounded `Xvfb`, if
   the lane already starts one for the existing headless GL tests — check
   before adding a second) plus a tiny second X11 client window to target —
   a minimal `xterm`, or (cheaper, no extra package) a bare
   `XCreateSimpleWindow`+`XMapWindow` C helper compiled as a test fixture,
   with a known PID (`getpid()` inside it) written somewhere the test can
   read back (stdout, a temp file) to compare against
   `resolve_window_at_screen_point`'s result.
3. This is a real hardware-adjacent gate ONLY in the CLAUDE.md sense of
   "cannot be installed" — Xvfb and a dummy X11 client are both installable
   software, not hardware or credentials, so per CLAUDE.md this must NOT
   self-skip; if the current CI fleet has no lane image with X11 at all,
   extend one (the `docker-desktop` image already has GLFW/GL, which itself
   needs a display for anything beyond render-to-FBO — check whether that
   lane already runs under Xvfb for its existing GL smoke tests before
   assuming a new display setup is needed).

**Tests.** The Xvfb-backed test itself, described above: point at the known
dummy window's screen coordinates, assert the resolved PID matches; point at
empty desktop background, assert `ok == false` with a stated `why_not`; on a
build/CI configuration without X11 headers at all (if any remain after step
1), assert `window_picker_supported() == false` rather than a link/compile
failure — confirm this degrade path is exercised, not just declared.

**Done when.** `libx11-dev` is a pinned apt line in `Dockerfile.desktop`; the
new Xvfb test lane is green and genuinely exercises window→PID resolution
against a live (virtual) display, not a mock; no lane self-skips this feature
for a reason CLAUDE.md would call installable.

## Constraints & gates

- **D9 unchanged, and load-bearing for Part A.** The desktop process itself
  must never call `PTRACE_TRACEME`/`PTRACE_SEIZE` — every fork+trace step in
  T1/T2 happens inside `asmspy` (`cli/asmspy.c`/`cli/asmspy_engine.c`),
  wherever `--serve` is actually running (local or, transparently, over
  ssh). `desktop/`'s `ldd` must stay exactly as engine-free after this brief
  as before it.
- **D4 unchanged for Part B.** `desktop/src/platform/window_picker.*` links
  X11 only — no Unicorn/Keystone/Capstone, no ptrace. It must build into
  BOTH `asmtest-desktop` and the render-only `asmtest-viewer` identically
  (the same engine-free closure `list_processes()`'s `/proc` reads already
  respect for process enumeration — this extends that same posture to window
  enumeration).
- **No `.asmtrace` schema change.** Part A's `launch` wire command is a new
  **request**, not a new **event/record kind** — the `session`/`started`
  reply already carries `pid` (`asmspy.c:4545-4548`) with no field added.
  Part B touches no wire format at all. `make asmtrace-golden-check` should
  show zero diff.
- **Window-pick is local-capture-only, enforced at the UI gate (T7), not
  buried in the resolver (T6).** `s.inspect.ssh_host` non-empty disables the
  crosshair with a stated reason — the resolved PID is meaningless against a
  remote `asmspy --serve`, and T6's resolver has no way to know it's being
  asked the wrong question, so the gate belongs where the ssh state is
  visible.
- **Honest degrade, not silent no-op, everywhere a platform gap exists** —
  Wayland/macOS window-pick, T1's interim launch-fidelity caveat before T2
  lands, an unreadable `_NET_WM_PID`. Each must be a stated reason a user can
  read, mirroring this codebase's existing fidelity vocabulary (D7) rather
  than a control that looks available and does nothing, or a recording that
  claims more truth than it has.

## Non-goals / acknowledged limits

- **No environment-variable editor for Launch v1.** The launched target
  inherits whatever environment `asmspy --serve` itself runs in (matching
  what a plain shell launch would default to); an explicit env-var UI is a
  reasonable follow-up, not required for the feature to be useful.
- **Launch does not solve "the region I asked to trace lives in a shared
  library the dynamic linker hasn't mapped yet."** The symbol/address-scoped
  modes (`trace`, `dataflow` by name, `watch`) inherit whatever behavior the
  existing attach path already has for a target whose named region isn't
  mapped at attach time (a "not found" refusal) — Launch does not newly
  break this and does not newly fix it. The whole-process modes (`log`,
  `stream`, `graph`, `tree`, `procs`, `sample`) have no such dependency and
  are the natural v1 focus; consider defaulting the Launch pane's mode
  selector to one of those rather than `--auto`/a named region, and treat a
  scoped launch as "works if the symbol happens to already be resolvable,
  same honesty as attach" rather than a promised capability.
- **No Windows window-picker, no macOS window-picker.** X11 only (T6); both
  degrade to a stated-unavailable control (T7), same posture as the existing
  window-icon precedent (`main.cpp:137-150`). A macOS implementation would
  need `CGWindowListCopyWindowInfo` and is out of scope here — and would be
  moot for local capture anyway, since the ptrace engines this app attaches
  through are Linux-only (`inspect.cpp:299-313`).
- **No ssh-remote window-picker.** The crosshair targets windows on the
  machine running the desktop app; a remote `asmspy --serve` traces whatever
  PID it's given regardless of where that PID came from, but resolving "what
  window is at this LOCAL screen point" can never name a PID on a different
  host. Not a gap to close, a category error to avoid building toward.
- **No "run to a breakpoint through process init" UI.** Launch takes the
  target from its TRACEME exec-stop straight into whichever engine mode was
  picked, the same way attach takes a target from whatever state it happens
  to be in — no new debugger-style stepping UI is added.

## Cross-references

[README.md](README.md) (D1–D11, the pane-registration pattern this doc
reuses); [19-dockable-panes-keystone.md](19-dockable-panes-keystone.md) (the
`kPane*` pattern T4 follows exactly); [07-serve-live-host.md](07-serve-live-host.md)
(the original `--serve`/`LiveSession`/Inspect-door design T1–T3 extend);
[39-auto-capture-reliability.md](39-auto-capture-reliability.md) (the most
recent brief to touch `serve_parse_start`/session-lifecycle plumbing T1
extends, and the model for "no hardware gate, pure tests only" T1/T2's tests
should follow); [11-imgui-addons.md](11-imgui-addons.md) (D2 addon-admission
rule — `ImGuiFileDialog` is already admitted and reused as-is by T4, no new
addon needed for either feature).
