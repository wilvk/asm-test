# GUI Process Details tab, fed by a new `asmspy --info`

**Date:** 2026-08-03
**Status:** design approved, ready for implementation planning

## Goal

Add a docked pane to `asmtest-desktop` that answers *"what is this process, and
what can I do with it?"* for whichever process is selected in the Processes
table, refreshing automatically as the selection changes.

Two deliverables:

1. `asmspy --info <pid> [--json]` — a new one-shot CLI subcommand that gathers a
   rich process snapshot quickly and exits. **It never calls ptrace.**
2. A `Process details` pane in the desktop shell that spawns that command on
   selection change, parses its output, and renders it.

## Measured facts this design rests on

Verified against the real binaries and this host on 2026-08-03, before the spec
was written. Three of them changed the design.

| Fact | How it was measured | Consequence |
|---|---|---|
| `asmspy_fingerprint()` already exists and is **ptrace-free** | [`cli/libasmspy.h:119-121`](../../../cli/libasmspy.h#L119-L121) — *"Reads only /proc and the mapped ELF — no ptrace, no attach"* | the bulk of the payload is already written; the new command composes rather than invents |
| A full symbol load of the heaviest process on this box costs **20 ms** | `time ./build/asmspy --syms 1206130` — VS Code, 5162 maps, 61530 symbols | "quickly" is achievable with the *whole* snapshot; no need for opt-in sections (YAGNI) |
| `/proc/<pid>/task/<tid>/wchan` is readable with **no ptrace permission** | read a sibling process's `wchan` under `ptrace_scope=1` → `hrtimer_nanosleep` | "what is it doing now" is answerable per-thread at zero attach cost |
| `/proc/<pid>/syscall` requires ptrace *permission* (not an attach) | same test → `EPERM` for a non-descendant under `ptrace_scope=1` | the richer pc/sp/args row degrades gracefully and must state its reason, never blank out |
| `kernel.yama.ptrace_scope = 1` on this host | `/proc/sys/kernel/yama/ptrace_scope` | permission-limited fields are the **normal** case in testing, not an edge case |
| The desktop does **not** link `libasmspy` | `DESKTOP_ENGINE_OBJ` at [`mk/desktop.mk:700`](../../../mk/desktop.mk#L700) carries no `asmspy_proc.o`; D9 | the desktop cannot call `asmspy_fingerprint()` in-process — a subprocess spawn is the only path that respects D9 |
| The Processes table lists **local** `/proc` regardless of the configured ssh host | [`inspect_door.cpp:30`](../../../desktop/src/ui/inspect_door.cpp#L30) calls `list_processes()` unconditionally; `ssh_host` is used only by `inspect_connect` | probing remotely when `ssh_host` is set would show an unrelated remote process under a locally-picked pid — v1 must probe locally and say so |
| The selection state already exists | `InspectState::selected_pid`, [`doors.h:157`](../../../desktop/src/ui/doors.h#L157), set at [`inspect_door.cpp:1240`](../../../desktop/src/ui/inspect_door.cpp#L1240) | no new selection mechanism; the pane observes what the table already sets |

## Component 1 — `asmspy --info <pid> [--json]`

A new subcommand in `cli/asmspy.c`, gathering into a new struct in
`cli/asmspy_proc.c` beside `asmspy_fingerprint()`, whose facts it reuses.

**Human text by default; `--json` emits a valid `.asmtrace` recording** — a
provenance header line (`tier: "proc-snapshot"`, `fidelity: "exact"`, the target
pid), one `procinfo` event, then the `end` footer, through the existing
[`cli/asmtrace_ndjson.c`](../../../cli/asmtrace_ndjson.c) serializers.
This matches every other mode's `--json` contract ("`--log <pid> --json >
x.asmtrace` IS a recording"), so `asmspy --info <pid> --json > x.asmtrace` is one
too, and the desktop consumes it with its existing `Recording` parser instead of
a bespoke JSON path.

`procinfo` is registered as a new row under the schema's own *"Adding a kind is a
new registry row under the ignore-unknown-kinds rule"* rule. It does not collide
with `topo`, which is the **engine-produced** topology snapshot and seizes the
descendant tree; `procinfo` seizes nothing.

### The hard rule

**`--info` never calls ptrace.** Not "attaches briefly" — never. This is what
makes it safe to fire automatically while the operator browses a process list,
and safe to poll on a timer. Any future field that would need an attach belongs
to a different command, not this one.

### Payload

Ordered by what changes an operator's next action. Every list has a hard cap and
sets an explicit `truncated` flag; a cut list is a **stated** absence, never a
silent one (D7).

| Section | Fields | Why it earns space |
|---|---|---|
| `identity` | pid, ppid, pgid, sid, real+effective uid/gid with resolved names, comm, `argv[]` (cap **64** args / **4 KiB**), exe (+ `(deleted)`), cwd, state, start time, elapsed | "Is this the process I meant?" — the table only shows `comm`, and the cmdline is what actually distinguishes twenty `code` processes |
| `runtime` | `asmspy_fingerprint()` verbatim: runtime badge + **evidence**, jitting, elf_class, pie, static, PT_INTERP, build-id | decides which tracing tier applies at all |
| `threads[]` | per task (cap **64**): tid, comm, state, **wchan**, and where permitted the current **syscall** (nr, name, args, pc, sp) with pc resolved through the symbol table | the live "what is it doing right now", with no attach |
| `counters` | utime/stime jiffies, RSS / VmSize / peak, io read/write bytes, fd count, threads, oom_score, nice — **raw, plus a monotonic timestamp** | emitting raw counters rather than a rate keeps the command sleep-free; the client derives rates from consecutive snapshots (and the schema's law 2: the client derives) |
| `trace` | the attach verdict with why/remedy, **plus per-mode capability**: which asmspy modes will work here and why not (i386 → dataflow refuses; no AMD IBS → sample refuses; already-traced → all refuse) | the most asmspy-specific value in the pane: "what can I actually do with this process", answered before you try |
| `code` | total resolved STT_FUNC count, has-symtab vs dynsym-only, JIT method count and its source (jitdump vs perf-map) | tells you *in advance* whether a trace shows names or raw addresses |
| `modules[]` | mapped ELFs (cap **64**, ranked by symbol count descending so the ones a trace will actually resolve against sort first): name, base, size, exec, build-id, symbol count; plus whole-process total anon-exec bytes | anon-exec bytes are the JIT surface the ELF symtab cannot see |
| `containment` | pid/net/mnt/user namespace ids vs ours, cgroup path, seccomp mode, no_new_privs, dumpable | a pid in another namespace does not mean what the local table implies |
| `children[]` | direct children (pid, comm), cap **32**, from a `/proc` ppid scan | cheap, and **never** the `--procs` engine, which SEIZEs the whole descendant tree |

### Cost budget

The snapshot carries a wall-clock budget (250 ms). Any section that overruns is
emitted with `truncated: true` and the UI states it. The measurement above says
the realistic cost is ~20 ms, so the budget is a guard against a pathological
target, not an expected path.

## Component 2 — the `Process details` pane

New pane constant `kPaneDetails = "Process details"` in
[`desktop/src/ui/layout.cpp`](../../../desktop/src/ui/layout.cpp), registered in
the pane table at [`shell.cpp:3172`](../../../desktop/src/ui/shell.cpp#L3172).

Its context gate is **`selected_pid > 0` only** — deliberately not
`pctx_capture`'s `host_started && selected_pid > 0`, because `--info` is a
one-shot spawn rather than a serve session. The tab therefore works before a host
is ever connected, which is precisely when the operator is deciding whether this
is the right process. Gate text when unselected mirrors the existing idiom: *"pick
a process in the Processes pane first"*.

### Files

Following the `session.h` split — the interesting half needs no subprocess:

- `desktop/src/live/procinfo.{h,cpp}` — pure model: parse a `procinfo` event into
  `ProcInfo`; derive rates from two snapshots; the mode-capability labels; the
  debounce/cache state machine. No ImGui, no fork.
- `desktop/src/ui/details_pane.cpp` — draw only.

### Layout

Two sections open by default (the ones that change what you do next); the rest
collapsed.

```
┌─ Process details ────────────────────────────────────────────┐
│ pid 1206130  code             [Node/V8]   ● running          │
│ /usr/share/code/code --type=renderer --enable-crashpad …     │
│ read 0.4s ago · attach-free (no ptrace) · [Refresh] [x] auto │
├──────────────────────────────────────────────────────────────┤
│ ▼ Can I trace this?                                          │
│    attach   YES                                              │
│    modes    log ✓  stream ✓  tree ✓  graph ✓  procs ✓        │
│             dataflow ✓  watch ✓  sample ✗ needs an AMD IBS…  │
│    names    61530 symbols · 218 modules · 1402 JIT methods   │
│             → a trace of this process will show names        │
├──────────────────────────────────────────────────────────────┤
│ ▼ What is it doing now                          12 threads   │
│    tid       comm             state  in                      │
│    1206130   code             S      futex_wait              │
│    1206144   V8 DefaultWorke  S      poll_schedule_timeout   │
│    1206151   Chrome_ChildIOT  R      — (running)             │
├──────────────────────────────────────────────────────────────┤
│ ▼ Resources                                                  │
│    cpu  14.2% ▇▇▇▁      rss 612 MB ▇▇▇▇▇▁     fds 184        │
│    io   1.2 MB/s r · 0 B/s w                                 │
├──────────────────────────────────────────────────────────────┤
│ ▶ Identity      uid will · ppid 1206101 · started 14:02      │
│ ▶ Code surface  218 modules · 12 MB anon-exec (JIT)          │
│ ▶ Containment   same namespaces · seccomp filtered           │
│ ▶ Children      3                                            │
└──────────────────────────────────────────────────────────────┘
```

Magnitudes reuse the existing `dt_cell_magnitude_bar` / `dt_magnitude_frac`
idiom rather than bare integers (the UX review's top finding).

### Lifecycle — automatic on selection

Each frame `want_pid` mirrors `selected_pid`. On a change:

1. Kill any in-flight child immediately.
2. Arm a **250 ms debounce**. Arrowing down the table therefore fires one probe
   at the row you stop on, not one per row.
3. On expiry, spawn `asmspy --info <pid> --json` and read its pipe
   non-blockingly from the frame loop. The UI thread never waits on the child.

Results are cached keyed on `(pid, start_time)` — stat field 22, so pid reuse
cannot serve a stale card — in a bounded LRU of 32. Re-selecting a previously
probed process renders instantly from cache while a refresh runs behind it.

Because the snapshot is attach-free, keeping it live is free: while the pane is
visible and the window focused, re-probe every **2 s** and derive %CPU / IO rates
from the previous snapshot. A hidden pane or unfocused window stops the timer. A
child that has not exited within **2 s** is killed and the pane states that it
timed out.

The asmspy binary is resolved with the existing
[`resolve_asmspy_path()`](../../../desktop/src/live/session.h#L164) — `$PATH`,
then `./build/asmspy` — the same resolution Connect uses.

### Refusals and absences

Each is rendered in place, never as a blank or a toast that disappears:

- **No asmspy found** — the same message and build hint the Connect pane uses.
- **pid exited between selection and probe** — *"pid N exited"*, an outcome, not
  an error.
- **Permission-limited field** (`/proc/<pid>/syscall` under Yama) — the field
  states its reason inline; the section is never silently empty.
- **Timeout / truncation** — stated with what was cut.

### The ssh gap, stated rather than papered over

The Processes table lists **local** processes even when Connect points at an ssh
host. Probing remotely whenever `ssh_host` is set would render remote pid 1234's
details under a locally-picked pid 1234 — a confidently wrong answer.

So v1 **always probes locally**, and when an ssh host is configured the pane
states plainly that the pid is local while the capture host is elsewhere. The
spawn carries the ssh-prefix capability as a `Spec` field, so enabling it is a
one-line change once a remote process list exists. **That remote process list is
separate work and explicitly out of scope here.**

## Verification

**CLI.** `cli/test_procinfo.c` against the existing `spy_victim`: asserts the
field set, types, caps and truncation flags. A golden pinning the **key set** —
not the values, which are host-specific — so the schema cannot drift silently. An
`--info` case in [`cli/cli_smoke.sh`](../../../cli/cli_smoke.sh). Lane: `make
docker-cli` (built from `Dockerfile.cli`).

**Desktop.** `desktop/test/test_procinfo.cpp` over checked-in JSON fixtures in
`desktop/test/fixtures/`: parse → model; rate derivation across two snapshots;
cache keying including pid reuse; the debounce state machine driven by an
injected clock; refusal, timeout and truncation rendering. A null-backend draw
test covering no-selection / full snapshot / refused. Lanes: `make
docker-desktop`, `make desktop-ui-test`.

Permission-limited fields are exercised deliberately, not skipped: this host runs
`ptrace_scope=1`, so the degraded path is the default one under test.

**Docs.** Define `procinfo` in
[`docs/internal/gui/asmtrace-schema.md`](../../internal/gui/asmtrace-schema.md)
(registry row plus a field table), add `--info` to the `usage()` text in
`cli/asmspy.c` and to
[`docs/guides/tracing/asmspy.md`](../../guides/tracing/asmspy.md), and add a
CHANGELOG entry. `make docker-docs` is warnings-as-errors.

## Non-goals

- **Any ptrace attach from this pane.** Explicitly excluded; see the hard rule.
- **A remote process list.** The ssh mismatch is stated, not fixed.
- **Replacing the Processes table's columns.** The table stays the picker; this
  pane is the detail view for whatever it selected.
- **Opt-in sections / partial fetches.** The measured 20 ms cost removes the
  motivation.
