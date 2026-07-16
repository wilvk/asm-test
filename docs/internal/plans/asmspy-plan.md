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

> **Complete as of 2026-07-16** — this table is the whole shipped surface: **11
> headless subcommands** (`--list`, `--syms`, `--log`, `--trace`, `--dataflow`,
> `--stream`, `--graph`, `--tree`, `--procs`, `--sample`, `--watch`) and **9 TUI
> modes** (1 Syscall log · 2 Assembly & funcs · 3 Live stream · 4 Call graph ·
> 5 Call tree · 6 Process tree · 7 Hot edges · 8 Process details · 9 Data flow),
> reached from the process picker (`asmspy` with no args). *(Correction: mode 8
> "Process details" and the `--list`/`--syms` helpers shipped but were missing from
> this table, which billed itself as the full family — added 2026-07-16.)*
>
> Cross-cutting TUI keys: `space` freezes a live snapshot; up/down·PgUp/PgDn·Home/End
> scroll; `Tab` toggles a view option or moves pane focus; `F3` refreshes; `Enter`
> drills in; `b`/ESC goes back. Headless: a **negative `n`** runs until the target
> exits or you interrupt (Ctrl-C).

| View | Headless | TUI | Engine | Mechanism | Safe on any target? |
|---|---|---|---|---|---|
| Syscall log (strace-ish, fd→path) | `--log` | mode 1 | `asmspy_engine_syscalls` | `PTRACE_SYSCALL`, all threads | **yes** |
| Function assembly + call-graph edges | `--trace` | mode 2 | `asmspy_engine_region` | `run_to` + `trace_attached_ex` (region single-step) | leader only |
| Whole-process instruction stream | `--stream` | mode 3 | `asmspy_engine_stream` | single-step, all threads | **no** (single-step) |
| Aggregated call graph (inv/calls/fanout, `[int]`/`[EXT]`/`[?]`, sortable) | `--graph` | mode 4 | `asmspy_engine_graph` | single-step, all threads | **no** (single-step) |
| Live call **tree** (indented by depth; TUI two-pane w/ assembly) | `--tree` | mode 5 | `asmspy_engine_tree` | single-step, all threads | **no** (single-step) |
| Process/thread **topology** (procs+threads+children, drill-in) | `--procs` | mode 6 | `asmspy_engine_procs` | `PTRACE_SYSCALL` **or** single-step, whole tree | **yes** in `--count=syscalls` |
| **Statistical hot edges** (IBS-Op `from→to`, named endpoints, misp/ret tags, `--json`) | `--sample` | mode 7 | `asmspy_engine_sample` | **AMD IBS-Op statistical sampling, OUT OF BAND** (no ptrace/step) | **yes** — the only *rich* view safe on any target (incl. a live JIT) |
| **Process details** / fingerprint (runtime badge — JVM/.NET/CPython/Node·V8/Go/…, thread makeup, notable modules, ELF traits) | — | mode 8 | `asmspy_fingerprint` | `/proc` + the mapped ELF only — **no ptrace attach at all** | **yes** — nothing is attached |
| **Data flow** (L0 values + L1 def-use + L2 slice nav) | `--dataflow` | mode 9 | `asmspy_engine_dataflow` | JIT-aware scoped-ptrace L0 value producer (`attach_jit`: SEIZE-all-threads-and-race + signal split), UI-side slice navigation | **no** (single-step); worker-thread + managed targets now reached |
| **Data watchpoint** (who-wrote-this-field + value) | `--watch` | — | `asmspy_engine_watch` | x86 HW data watchpoint (DR0-3, per-thread arm), native speed between hits | **yes** — near-zero perturbation |

Two attach-free helpers complete the surface (no engine — they feed the views):

| Helper | Headless | TUI | Backend | Mechanism | Safe on any target? |
|---|---|---|---|---|---|
| Process **list/picker** (`active` = recent CPU; `scan` = string-rich memory first) | `--list [active\|scan]` | the entry picker screen | `asmspy_proclist` / `asmspy_sort_t` | `/proc` walk (+ a memory probe under `ASMSPY_SORT_SCAN`) | **yes** |
| Resolved **function symbols** (ELF + PLT `name@plt`, C++-demangled; `addr`/`size`/`name`/`module`, optional substring filter) | `--syms <pid> [filter]` | feeds every view's symbol picker | `asmspy_symtab_load` | ELF symtab/dynsym + **separate debug info** (`.gnu_debuglink`/build-id, CRC- and id-verified — see Theme A) + PLT; no attach. **Static symbols only — `--syms` does *not* layer the JIT/perf-map** (`cmd_syms` calls `asmspy_symtab_load` alone; `asmspy_resolve`'s ELF→JIT→refresh chain lives in the single-step engines + `--sample`), so managed frames are absent here even when `--stream`/`--graph` name them | **yes** |

> The **Data flow** and **Data watchpoint** views landed **2026-07-15** via the
> [live-attach data-flow plan](../archive/plans/live-attach-dataflow-plan.md) (Inc 6 headless `--dataflow` +
> Inc 7 TUI mode 9) and its followup **F3** (`--watch`); `asmspy_engine_watch` per-thread-arms
> across `/proc/<pid>/task/*`. **Live-attach Increment 5 (`attach_jit` + the signal split)
> LANDED 2026-07-15**: the Data-flow view now SEIZEs every thread and races whichever one
> first enters the region — reaching a routine that runs on a worker thread (as managed
> methods almost always do), same as any native leaf — and detects/delivers the target's own
> trap (a JIT self-check, a debugger breakpoint) rather than swallowing or single-stepping
> through it, so the `runtime_is_managed` hard gate is removed from both the headless and TUI
> call sites. Two disclosed gaps remain: a genuinely W^X-enforced JIT page's entry breakpoint
> has no hardware-breakpoint fallback yet (self-skips cleanly, `DF_PTRACE_ETRACE`), and a
> method re-JIT'd/moved *mid-capture* decodes the live snapshot (the versioned-decode `img`
> plumbing isn't wired into asmspy's call site yet) — both were pre-existing, explicitly
> flagged deferrals, not new bugs. Validated by 11 new checks in
> `examples/test_dataflow_ptrace.c` (signal-detection/delivery + worker-targeting, ASan/UBSan-
> clean) and `make docker-cli`; not yet validated against a real dotnet/JVM process (no such
> asmspy-specific harness exists in-repo).

Cross-cutting, landed: **PLT stub resolution** (`name@plt`, tagged `[EXT]`); the **crash-safe
two-phase detach** (stop all threads, then release all — fixes a fatal-SIGTRAP-on-detach when
stepping a JIT); **whole-process-tree following** in `--procs` (`TRACEFORK`/`TRACEVFORK` +
a `/proc` PPid rescan for pre-existing children, `TRACEEXEC`); **Tab toggles a view option /
F3 refreshes**; headless subcommands under [cli-smoke](../../../cli/cli_smoke.sh).

## Roadmap (remaining work)

### Theme A — Symbol resolution (biggest correctness/usefulness wins)

| Item | Sev | Eff |
|---|---|---|
| ~~Resolve **JIT/perf-map** (`/tmp/perf-<pid>.map`, jitdump) in stream/graph/tree — managed frames (Node/V8 JS, **.NET**, **Java**) render as `[?]`/`0x..` today; one resolver addition names all three (they emit the same perf-map format)…must refresh during the trace~~ — **landed**: `asmspy_jitmap_t` + `asmspy_resolve` (ELF→JIT→rate-limited refresh-on-miss) in all three single-step engines; JIT frames render `name [jit]` (stream/tree) and `[JIT]` (graph); `jit_victim` smoke proves it. (`.NET`/Java need the runtime launched with the perf-map flag/agent — asmspy can't enable it from outside.) *binary jitdump reader **landed 2026-07-12**: tier-1 source (maps-discovered `jit-<pid>.dump`, `CODE_LOAD`/`CODE_MOVE`-aware, exact sizes; ASan/UBSan-fuzzed against hostile files); the text perf-map is tier 2* | **high** | L |
| ~~Separate-debug / `.gnu_debuglink` / build-id resolution for stripped distro binaries~~ — **landed 2026-07-16**: a stripped binary with separated debug info resolved **nothing** before this; now `load_module_syms` recovers it through the two keys GDB and the perf tools use, in GDB's order — **build-id** (`.note.gnu.build-id` → `<debugdir>/.build-id/ab/cdef….debug`) then **`.gnu_debuglink`** (a filename + CRC-32 → `<dir>/`, `<dir>/.debug/`, `<debugdir>/<dir>/`). `<debugdir>` defaults to `/usr/lib/debug` and is overridable via **`ASMSPY_DEBUG_DIR`** (the analogue of gdb's `set debug-file-directory`; it is also what lets the smoke test the global-tree and `.build-id` paths hermetically instead of needing root to write a system path). **Both keys are VERIFIED, never trusted** — a debuglink candidate must match the recorded CRC-32 and a build-id candidate must carry the same build-id — because for a tracer a *confidently wrong* name (the classic "-dbg package one version behind") is worse than no name. New precedence: own `.symtab` > **verified separate-debug `.symtab`** > `.dynsym`, and exactly one of the three is read — so no module contributes duplicate entries and a module carrying its own `.symtab` never pays for a debug-file search. Refactor: the symbol walk came out of `load_module_syms` into `scan_elf_syms(base, flen, want, …)` so it can run over a second mapped ELF; the bias from the **running** module applies to the debug file unchanged (it is the same ELF with the contents carved out, so section addresses and `st_value`s are preserved). Five findings worth carrying: (1) `objcopy --add-gnu-debuglink=<path>` records only the **basename** (verified against binutils) — so the search is directory+basename, and taking the basename defensively also keeps a hostile ELF's `../../` out of the candidate paths; (2) the debuglink CRC is the plain **zlib/IEEE CRC-32**, verified byte-for-byte against what objcopy records (GDB's `gnu_debuglink_crc32` is the same function); (3) `.gnu_debuglink` is **non-alloc so `strip --strip-all` removes it** — `--add-gnu-debuglink` must run *after* the strip, while `.note.gnu.build-id` is `SHF_ALLOC` and survives, which is why build-id is the key distros actually index by; (4) a stripped **executable**'s `.dynsym` does not carry its locally-called functions at all (only what dynamic linking needs), which is what makes the smoke's negative control real; (5) the CRC table is built per call — ~2 k iterations against a multi-MB file read is free, and it keeps the hash a pure function with no shared state for the UI and tracer threads to race. Guarded by **7 smoke assertions** (`cli/debuglink_victim.c` + `cli_smoke.sh`): a negative control (stripped, no debug file → resolves nothing), all three debuglink paths, a `--trace ret=51` on the recovered symbol (proving the address is the real **runtime** address — a bias bug would resolve a name that never hits), CRC-mismatch rejected, build-id resolved, foreign-build-id rejected. All three checks are **mutation-proven** to fail when the corresponding check is removed. Known limit (shared with gdb/perf, not new): the search keys off the `/proc/<pid>/maps` path, so a **deleted** or foreign-mount-namespace binary finds no debug file even though asmspy still maps the exe itself via `/proc/<pid>/exe`; **debuginfod is out of scope** (a network fetch does not belong in an attach path) | med | **M** |
| ~~C++ **demangling** via `__cxa_demangle` at the single `sym_push` chokepoint (guard on `_Z`, demangle before appending `@plt`, add `-lstdc++`) — reads through picker/graph/tree/stream at once~~ — **landed**: `demangle_dup()` at the `sym_push` chokepoint (peels/re-appends `@plt`), `-lstdc++` on the link line, `cpp_victim.cpp` smoke assertion | med | **S** |

### Theme B — Cross-engine asymmetries

| Item | Sev | Eff |
|---|---|---|
| ~~Region sampler (mode 2) attaches only the thread-group leader — silently empty for a function that runs on a worker thread; seize all threads and arm the region across them~~ — **landed 2026-07-16**: `asmspy_engine_region` now SEIZEs every thread (`PTRACE_O_TRACECLONE`, so one spawned mid-run can win a later round) and races them to the entry, sampling **whichever arrives first**; `--trace` gained `--tid=<t>` to pin one, mirroring the other engines. This closes the shape that mattered — a **managed method almost never runs on the leader**, so the view was structurally blind to the JIT'd code asmspy exists to show. Reuses the design of the data-flow tier's oracle-validated race (`dfp_seize_all`/`dfp_run_to_multi`, live-attach Increment 4) rather than inventing a second one, reimplemented over asmspy's own `thr_tab` per the standing precedent that an engine stays in `cli/` and leaves `src/ptrace_backend.c` untouched (as `asmspy_engine_watch` does). Two findings worth carrying: (1) `--tid` **cannot** narrow the SEIZE — a shared int3 would kill an unseized thread that reached it — so it pins via a **per-thread hardware execution breakpoint** (the alternative, stepping hot non-target threads back over a shared int3, was MEASURED not to converge); (2) the entry trap and the debug registers both survive `PTRACE_DETACH` and are both refused on a *running* thread, so the disarm must stop each thread first — skipping that still passes an immediate liveness check and then kills the target seconds later by SIGTRAP (exit 133) on its next arrival. `cli_smoke.sh` asserts the worker sample, both `--tid` directions, and survival past a settle; `make docker-cli` → cli-smoke PASS. Carryover: the entry breakpoint is `POKETEXT`-only in the any-thread path (no DR0 fallback for a W^X JIT page — it self-skips), and the TUI has no thread picker (mode 9's follow-on). | med | L |
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
| ~~Graph sort comparator (`gnode_cmp`) ordering/tiebreaks unasserted — extract into a testable header + `test_graphsort.c`~~ — **landed 2026-07-12**: `cli/asmspy_graphsort.h` + `cli/test_graphsort.c` (ties included), wired into the smoke | med | M |
| `asmspy_symtab_at` reverse-lookup edge cases; multi-thread `[tid]` tagging for graph/tree; attach-failure + `REGION_NEVER_RAN`; negative-`n` "until exit"; `--tree` depth > 1; post-attach clone-following; job-control group-stop; syscall-decoder breadth; region edge aggregation; ~~a pure TUI view-model (`test_view.c`)~~ **(landed 2026-07-15: `cli/asmspy_dataview.h` + `cli/test_view.c`, unit-testing the Data-flow annotation/slice/def-use logic, wired into `cli_smoke.sh`)** | low | S–M each |
| **Already landed, previously unrecorded here** (bookkeeping, no work implied) — **`cli/test_logview.c`** (`cli/asmspy_logview.h`, `mk/cli.mk:133-134`) unit-tests the TUI scrollback viewport math with no ncurses, so it runs anywhere the smoke does; **`cli/test_jitdump.c`** (`mk/cli.mk:154-157`) unit-tests the binary jitdump reader + the two-tier JIT resolve chain, linking `asmspy_proc.o` directly. Both are `cli-smoke` prereqs (`mk/cli.mk:159-165`) — so the landed view-model surface is four tests (`test_view`, `test_logview`, `test_jitdump`, `test_graphsort`), not one | — | — |

### Theme E — TUI / UX & interop

| Item | Sev | Eff |
|---|---|---|
| ~~Call-graph view (mode 4) neither scrollable nor pausable — only the top `rows-3` visible, re-sorts every frame~~ — **landed**: `space` freezes the snapshot (a `gpaused` flag stops the sink overwriting it) and up/down·PgUp/PgDn·Home/End scroll a `gtop` window over the frozen, sorted nodes; a finished tracer is scrollable too, and `space` resumes the live top-anchored re-sort. Verified end-to-end through the ncurses TUI (pty-driven). Mirrors the log-view pause/scroll idiom | med | M |
| ~~Region view (mode 2) disasm/functions panes don't scroll — overflow off-screen~~ — **landed**: `space` freezes the sample (an `rpaused` flag stops the sink re-sampling) and up/down·PgUp/PgDn·Home/End scroll the focused pane; `Tab` moves focus between the ASSEMBLY and FUNCTIONS panes (each with its own `atop`/`ftop` offset and a `(scroll)` overflow hint), a finished sample stays scrollable, and `space` resumes the live top-anchored re-sample. Verified end-to-end through the ncurses TUI (pty-driven). Same idiom as the call-graph pause/scroll | med | M |
| ~~Call tree has no depth cap / symbol focus / module filter — a firehose on busy processes~~ — **landed 2026-07-17**: `--tree --depth=<d>` (`tree -L`: d levels), `--focus=<sym>` (root the tree at a symbol's **subtree** and re-base its depth to 0), `--module=<m>` (keep only that module's callees) — all substring-matched, composing as AND with `focus` applied first because it is the only one that establishes *scope*: `--focus=work --module=libc` means "the libc calls work() makes", not "libc calls, and separately work()". The decision came out into **`cli/asmspy_treefilter.h`** (pure, header-only, the `asmspy_graphsort.h`/`asmspy_dataview.h` precedent) + **`cli/test_treefilter.c`** (19 checks). The governing invariant, and the thing every review should re-check: **filtering bounds what is EMITTED, never what is TRACKED** — the engine pushes/pops the per-thread depth on every call and return regardless of the verdict, and `max` therefore counts *surviving* lines. A filter fed only the surviving calls drifts from its first suppression on (mutation M6 below demonstrates exactly this: the cap suppresses `helper` at depth 1, the un-pushed counter falls back to 0, and the *next* `helper` escapes the cap at depth 0). `--depth=0` is a **usage error, not "unlimited"** (it can only ever print nothing; omitting the flag is how you ask for unlimited). Guarded end-to-end in `cli_smoke.sh` against `spy_victim`, whose shape supplies a real control for each case — `usleep@plt` runs at the **same real depth 0** as `work`, so "`--focus=work` dropped it" cannot pass by cutting deep lines, and `helper` unfiltered only ever appears indented two columns, so "printed at column 0" cannot pass without a true re-base. **Mutation-proven (6/6 caught, all run):** cap ignored → unit 3 FAILs + `helper` leaks the cap; no re-base → unit 5 FAILs + `helper` renders indented; focus never closes → unit 6 FAILs + `usleep@plt` leaks into `--focus=work`; module ignored → unit 4 FAILs + `[spy_victim]` leaks into `--module=libc`; `max` counting raw calls → `--depth=1 8` yields **2** lines and `--module=libc` yields **0 bytes**; depth pushed only on emit → `helper` leaks the cap. The two engine-level mutations are caught only by the e2e, correctly — they are outside the pure filter's scope. **TUI mode 5 is deliberately unchanged** (passes `filter = NULL`, asserted identical to a zeroed filter): it has no filter widget, and a text-entry ncurses prompt is a separate, CI-undrivable piece of work — the headless flags carry the feature | med | M |
| ~~No per-thread (tid) filter for stream/tree/graph — can't isolate one worker or cut single-step slowdown on others~~ — **landed**: `--stream`/`--tree`/`--graph --tid=<t>` seizes/steps only that thread (`seize_one`/`seize_for_engine`), leaving the rest of the process at full speed; `tid_victim` (distinct alpha/beta functions) smoke proves the other thread's code never appears | med | M |
| **DOT/JSON export** — *landed for `--graph`*: `--graph --json` emits nodes (addr/name/module/kind/counts) **and edges** (address-keyed caller→callee + count — `gedge_t` now crosses the sink via `asmspy_gedge_t`/`graph_emit`), and `--graph --dot` emits a Graphviz digraph (kind-coloured nodes, count-labelled edges; `dot -Tsvg`-validated in the smoke). `--tree --json/--dot` **landed 2026-07-12** (mirrors the `--graph` exporters). Still open: `--json`/`--dot` for `--procs` | low | M |
| Syscall arg decoding is a hardcoded subset (flags/structs/vectors/sigsets → hex); the always-3-hex-slots default is a small standalone blemish | low | L |
| fd→path shows `socket:[inode]`/`pipe:[inode]`, not the endpoint (enrich via `/proc/<pid>/net`); graph node/edge lookups are O(n) per call (hash index; single-stepping dominates so secondary); `usage()` accepts the `functions-called` sort synonym (`cli/asmspy.c:4577`) but omits it from the `--sort=` `bad_arg` text (`:4580`, which names only `invocations`/`fanout`) — the `<sym|0xADDR[:LEN]>` half of this item is **done** (documented at `cli/asmspy.c:4403`/`:4405`) | low | S–M |

### Theme F — Portability & build

| Item | Sev | Eff |
|---|---|---|
| Single-step engines are **x86-64-hardcoded** (`rip`/`eflags`-TF/`orig_rax`, `ASMTEST_ARCH_X86_64`) though the disassembler already does ARM64 — needs a reg/single-step/detach abstraction + `PSTATE.SS` | med | L |
| No architecture gate — on arm64 `make cli` emits raw `SYS_open undeclared` / `no member rip` instead of self-skipping like other tiers | low | S |
| 32-bit/i386 tracees silently mishandled — pragmatic fix: read `/proc/<pid>/exe` `EI_CLASS` at attach and refuse with a clear message | low | S |
| ~~CI tests asmspy only against apt Capstone 4, never the pinned 5.0.1 that docs/docker ship; no documented kernel-version floor (a too-old-kernel `SEIZE` failure misreports as a permission problem)~~ — **landed (both halves; marker corrected 2026-07-16 — this row had gone stale)**: the `cli` job builds the **pinned Capstone 5.0.1 from source** (`.github/workflows/ci.yml`, cached per the K1 pattern), explicitly *"NOT apt's Capstone 4 — the older library silently degrades some disassembly, so the smoke must run against what users actually get"*; the **kernel floor is documented** in the user guide ([asmspy.md](../../guides/tracing/asmspy.md) — `PTRACE_SEIZE`+`PTRACE_INTERRUPT` need **Linux 3.4+**, and the guide names the too-old-kernel failure so it isn't misread as a permission problem) | low | S |

### Theme G — Process/docs

- ~~No dedicated asmspy plan/roadmap doc~~ — **this document** closes it.

## Top priorities

1. ~~**Resolve JIT/perf-map symbols in stream/graph/tree** *(A, high, L)* — the crash-safe
   detach was built to whole-process single-step V8/Node, yet on that flagship target every
   dominant JIT'd JS frame renders `[?]`/`0x..`; the same fix names **.NET** and **Java**
   managed frames too.~~ — **landed**: `asmspy_resolve` layers a refreshed perf-map over the
   ELF symtab in all three single-step engines (see Theme A). **All three original Top-3 have
   now landed.** Next candidates: ~~jitdump (bytes-accurate, handles tiered recompilation)~~ (**landed 2026-07-12**) and
   the remaining Theme E items (call-tree depth cap / focus / module filter; the
   call-graph and region-pane pause+scroll have since landed).
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
