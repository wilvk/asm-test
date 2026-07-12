# asm-test — asmspy (interactive process tracer): roadmap

> **Context (2026-07-12).** `asmspy` is the ncurses + headless out-of-process tracer in
> [cli/](../../../cli/): attach to any running Linux/x86-64 process and watch its syscalls,
> a function's assembly + call-graph, a whole-process instruction stream, an aggregated
> call graph, a live call tree, or the process/thread topology — all out of band via the
> ptrace attach seam. User guide: [docs/guides/tracing/asmspy.md](../../guides/tracing/asmspy.md).
> Sources: [asmspy.c](../../../cli/asmspy.c) (TUI + headless + main),
> [asmspy_engine.c](../../../cli/asmspy_engine.c) (the engines + detach),
> [asmspy_proc.c](../../../cli/asmspy_proc.c) (/proc + ELF/PLT resolver),
> [asmspy.h](../../../cli/asmspy.h) (contract); smoke: [cli/cli_smoke.sh](../../../cli/cli_smoke.sh).
>
> This is the first dedicated asmspy plan (every sibling tier had one; asmspy did not — the
> gap the roadmap audit flagged as **Theme G**). It records what has LANDED and the remaining
> work so the view family stays coherent rather than accreting one-off flags.

> Status legend: **landed** unless marked *(planned)*. Severity/effort on roadmap items are
> the values an adversarial audit (2026-07-12, 55 agents, 47 verified items) confirmed.

## The view family (current — all landed)

| View | Headless | TUI | Engine | Mechanism | Safe on any target? |
|---|---|---|---|---|---|
| Syscall log (strace-ish, fd→path) | `--log` | mode 1 | `asmspy_engine_syscalls` | `PTRACE_SYSCALL`, all threads | **yes** |
| Function assembly + call-graph edges | `--trace` | mode 2 | `asmspy_engine_region` | `run_to` + `trace_attached_ex` (region single-step) | leader only |
| Whole-process instruction stream | `--stream` | mode 3 | `asmspy_engine_stream` | single-step, all threads | **no** (single-step) |
| Aggregated call graph (inv/calls/fanout, `[int]`/`[EXT]`/`[?]`, sortable) | `--graph` | mode 4 | `asmspy_engine_graph` | single-step, all threads | **no** (single-step) |
| Live call **tree** (indented by depth; TUI two-pane w/ assembly) | `--tree` | mode 5 | `asmspy_engine_tree` | single-step, all threads | **no** (single-step) |
| Process/thread **topology** (procs+threads+children, drill-in) | `--procs` | mode 6 | `asmspy_engine_procs` | `PTRACE_SYSCALL` **or** single-step, whole tree | **yes** in `--count=syscalls` |

Cross-cutting, landed: **PLT stub resolution** (`name@plt`, tagged `[EXT]`); the **crash-safe
two-phase detach** (stop all threads, then release all — fixes a fatal-SIGTRAP-on-detach when
stepping a JIT); **whole-process-tree following** in `--procs` (`TRACEFORK`/`TRACEVFORK` +
a `/proc` PPid rescan for pre-existing children, `TRACEEXEC`); **Tab toggles a view option /
F3 refreshes**; headless subcommands under [cli-smoke](../../../cli/cli_smoke.sh).

## Roadmap (remaining work)

### Theme A — Symbol resolution (biggest correctness/usefulness wins)

| Item | Sev | Eff |
|---|---|---|
| ~~Resolve **JIT/perf-map** (`/tmp/perf-<pid>.map`, jitdump) in stream/graph/tree — managed frames (Node/V8 JS, **.NET**, **Java**) render as `[?]`/`0x..` today; one resolver addition names all three (they emit the same perf-map format)…must refresh during the trace~~ — **landed**: `asmspy_jitmap_t` + `asmspy_resolve` (ELF→JIT→rate-limited refresh-on-miss) in all three single-step engines; JIT frames render `name [jit]` (stream/tree) and `[JIT]` (graph); `jit_victim` smoke proves it. (`.NET`/Java need the runtime launched with the perf-map flag/agent — asmspy can't enable it from outside.) *jitdump (bytes-accurate, tiered-recompile-aware) not yet read — text perf-map only* | **high** | L |
| Separate-debug / `.gnu_debuglink` / build-id resolution for stripped distro binaries | med | M |
| ~~C++ **demangling** via `__cxa_demangle` at the single `sym_push` chokepoint (guard on `_Z`, demangle before appending `@plt`, add `-lstdc++`) — reads through picker/graph/tree/stream at once~~ — **landed**: `demangle_dup()` at the `sym_push` chokepoint (peels/re-appends `@plt`), `-lstdc++` on the link line, `cpp_victim.cpp` smoke assertion | med | **S** |

### Theme B — Cross-engine asymmetries

| Item | Sev | Eff |
|---|---|---|
| Region sampler (mode 2) attaches only the thread-group leader — silently empty for a function that runs on a worker thread; seize all threads and arm the region across them | med | L |
| No exec-stop re-resolution in stream/graph/tree — after a traced launcher `execve`s, the symtab + exebase go stale (`--procs` already sets `TRACEEXEC`; the others do not) | low | M |
| Child-process following limited to `--procs` — stream/graph/tree/syscalls do **not** follow forked children (`TRACEFORK` unset there); `strace -f` parity gap | low | S |

### Theme C — Signal & control-flow correctness

| Item | Sev | Eff |
|---|---|---|
| ~~App-delivered `SIGTRAP` is swallowed **and** mis-decoded as a single-step — distinguish via `PTRACE_GETSIGINFO` `si_code`~~ — **landed** (`sigtrap_is_app` + `deliver_app_sigtrap` in all four single-step engines; `int3_victim` smoke). Empirically corrected the plan's own hint: on x86-64 `TRAP_BRKPT` is **ours** (a step completing across a syscall), a real `int3` is `SI_KERNEL`, so the safe whitelist delivers only `SI_KERNEL`/`TRAP_HWBKPT`. Re-inject via `PTRACE_CONT`, never `SINGLESTEP` (re-arming TF fires a fatal `#DB` in the masked handler). Two follow-ups (both from an adversarial review): a batch `--count` run can't reach its budget on an int3-looping target (documented in `--help`); the region + syscall engines still swallow app SIGTRAPs (out of scope, lower impact) | med | M |
| Indirect-call attribution slips at signal boundaries — read the CALL target from regs/mem instead of inferring from where the next step lands | low | M |
| `--tree` depth is a best-effort shadow counter that tail-calls / `longjmp` / signals drift — lift to a real per-thread return-address stack (also enables nesting-aware aggregation) | low | M |
| `thr_get()` OOM ignored at resume sites — an untracked clone child can escape the two-phase detach (the seize path detaches on OOM; the resume paths don't) | low | S |

### Theme D — Test coverage

| Item | Sev | Eff |
|---|---|---|
| ~~**No regression test for the crash-safe two-phase detach** — the exact bug the fix closed has zero guard, and every smoke victim is killed right after tracing, so a regression that silently kills the target looks like success. Trace a multi-threaded victim, then assert it **survives** detach~~ — **landed** as a happy-path survival tripwire (single-step via `--stream`+`--tree`, then assert alive) in `cli_smoke.sh`. Honest scope: the historical fatal SIGTRAP reproduced *reliably only on a real V8/Node JIT* (per 6aaad45's own notes — simple victims survive even the pre-fix detach), so this guards gross regressions, not that JIT crash (which needs a live JIT, an inherent limit) | med | S |
| Graph sort comparator (`gnode_cmp`) ordering/tiebreaks unasserted — extract into a testable header + `test_graphsort.c` | med | M |
| `asmspy_symtab_at` reverse-lookup edge cases; multi-thread `[tid]` tagging for graph/tree; attach-failure + `REGION_NEVER_RAN`; negative-`n` "until exit"; `--tree` depth > 1; post-attach clone-following; job-control group-stop; syscall-decoder breadth; region edge aggregation; a pure TUI view-model (`test_view.c`) | low | S–M each |

### Theme E — TUI / UX & interop

| Item | Sev | Eff |
|---|---|---|
| Call-graph view (mode 4) neither scrollable nor pausable — only the top `rows-3` visible, re-sorts every frame | med | M |
| Region view (mode 2) disasm/functions panes don't scroll — overflow off-screen | med | M |
| Call tree has no depth cap / symbol focus / module filter — a firehose on busy processes | med | M |
| ~~No per-thread (tid) filter for stream/tree/graph — can't isolate one worker or cut single-step slowdown on others~~ — **landed**: `--stream`/`--tree`/`--graph --tid=<t>` seizes/steps only that thread (`seize_one`/`seize_for_engine`), leaving the rest of the process at full speed; `tid_victim` (distinct alpha/beta functions) smoke proves the other thread's code never appears | med | M |
| **DOT/JSON export** — *landed for `--graph`*: `--graph --json` emits nodes (addr/name/module/kind/counts) **and edges** (address-keyed caller→callee + count — `gedge_t` now crosses the sink via `asmspy_gedge_t`/`graph_emit`), and `--graph --dot` emits a Graphviz digraph (kind-coloured nodes, count-labelled edges; `dot -Tsvg`-validated in the smoke). Still open: `--json`/`--dot` for `--tree`/`--procs` | low | M |
| Syscall arg decoding is a hardcoded subset (flags/structs/vectors/sigsets → hex); the always-3-hex-slots default is a small standalone blemish | low | L |
| fd→path shows `socket:[inode]`/`pipe:[inode]`, not the endpoint (enrich via `/proc/<pid>/net`); graph node/edge lookups are O(n) per call (hash index; single-stepping dominates so secondary); `usage()` omits the `0xADDR+LEN` form and the `functions-called` sort synonym | low | S–M |

### Theme F — Portability & build

| Item | Sev | Eff |
|---|---|---|
| Single-step engines are **x86-64-hardcoded** (`rip`/`eflags`-TF/`orig_rax`, `ASMTEST_ARCH_X86_64`) though the disassembler already does ARM64 — needs a reg/single-step/detach abstraction + `PSTATE.SS` | med | L |
| No architecture gate — on arm64 `make cli` emits raw `SYS_open undeclared` / `no member rip` instead of self-skipping like other tiers | low | S |
| 32-bit/i386 tracees silently mishandled — pragmatic fix: read `/proc/<pid>/exe` `EI_CLASS` at attach and refuse with a clear message | low | S |
| CI tests asmspy only against apt Capstone 4, never the pinned 5.0.1 that docs/docker ship; no documented kernel-version floor (a too-old-kernel `SEIZE` failure misreports as a permission problem) | low | S |

### Theme G — Process/docs

- ~~No dedicated asmspy plan/roadmap doc~~ — **this document** closes it.

## Top priorities

1. ~~**Resolve JIT/perf-map symbols in stream/graph/tree** *(A, high, L)* — the crash-safe
   detach was built to whole-process single-step V8/Node, yet on that flagship target every
   dominant JIT'd JS frame renders `[?]`/`0x..`; the same fix names **.NET** and **Java**
   managed frames too.~~ — **landed**: `asmspy_resolve` layers a refreshed perf-map over the
   ELF symtab in all three single-step engines (see Theme A). **All three original Top-3 have
   now landed.** Next candidates: jitdump (bytes-accurate, handles tiered recompilation) and
   the graph/tree scroll/filter items in Theme E.
2. ~~**Add the crash-safe two-phase-detach regression test** *(D, med, S)*~~ — **landed**
   (a happy-path survival tripwire across `--stream`/`--tree`; see Theme D for the honest
   scope note on why a simple victim can't deterministically reproduce the V8 crash).
3. ~~**C++ symbol demangling at `sym_push`** *(A, med, S)*~~ — **landed**: one chokepoint
   change (`demangle_dup()`) makes the picker, graph, tree, and stream readable on any C++
   target at once; guarded by the `cpp_victim` smoke assertion.

## Known issues / open investigations

- **Fatal `SIGTRAP` single-stepping a real V8 process (VS Code / Claude Code extension host).**
  The two-phase detach fixed the reproducible detach-ordering crash, and a spawned Node
  (up to 10 threads, 49 s of single-stepping, worker threads + heavy churn) now survives
  `--graph`/`--stream`/`--tree`/`--procs --count=calls` every time. But a real V8 host was
  reported to still die with `SIGTRAP` — **not reproduced** with a spawned Node; more crash
  detail pending. Likely the inherent managed-runtime hazard below (V8 `IMMEDIATE_CRASH`/int3
  under perturbation) rather than a remaining asmspy bug. The `si_code` split (Theme C) — the
  named next diagnostic lever — has **landed** and sharpens this: V8's `IMMEDIATE_CRASH` int3
  reports `si_code == SI_KERNEL`, so asmspy now *faithfully delivers* it (via `PTRACE_CONT`)
  instead of swallowing it. A `SIGTRAP` death on a real V8 is therefore now confirmable as V8's
  *own* int3 delivered as it would be untraced — not an asmspy-injected signal. The split cannot
  separate a genuine V8 self-check from a step-perturbation-induced one (identical `SI_KERNEL`
  int3); that ambiguity is inherent (below). A detect-and-warn guard remains deliberately unbuilt.

## Managed runtimes (Node/V8, .NET, Java)

The safe views — **syscall log** and **`--procs --count=syscalls`** — already work on any
managed runtime today (they show the runtime's GC/JIT/finalizer/threadpool threads and child
processes with counts, no single-stepping). The **single-step** views (stream/graph/tree,
`--count=calls`, drill-in) run but (a) carry the crash risk below and (b) leave managed
methods unnamed until the **perf-map resolver** (Theme A) lands — one addition covers Node
(`--perf-basic-prof`), .NET (`DOTNET_PerfMapEnabled=1`), and Java (`perf-map-agent`), which
all emit `/tmp/perf-<pid>.map`. The runtime must be launched with that flag/agent; asmspy
cannot enable it from outside.

## Inherent limitations (record, do not schedule)

- **Single-stepping a live JIT/managed runtime (V8/Node, .NET/CoreCLR, JVM, Mono) can crash
  it.** They self-check and `int3`/abort when perturbed, and JIT/GC/deopt run under the
  stepper. The crash-safe detach removes one failure mode; it cannot make stepping a live JIT
  safe in general. Prefer the syscall-based views on these targets.
- **macOS / Windows unsupported.** ptrace `SEIZE` + `/proc` + `process_vm_readv` are
  Linux-kernel-specific; a macOS port is a full `mach_vm_read`/`task_for_pid`/`thread_get_state`
  rewrite. Out of scope by design.
- **`SIGTRAP` ambiguity.** A ptrace `#DB`/int3 and a genuine app `SIGTRAP` are fundamentally
  indistinguishable at the tracer; `si_code` heuristics reduce but cannot fully eliminate this.
