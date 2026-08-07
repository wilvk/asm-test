# asm-test — asmspy (out-of-process tracer CLI): macOS port plan

A phased roadmap for giving [asmspy](../../../cli/) a **Darwin body**. asmspy is
implemented and validated on Linux x86-64 and AArch64 ([asmspy-plan.md](asmspy-plan.md));
on macOS it does not build at all, and `make cli` says so rather than trying:

```
# SKIP cli: asmspy is a Linux-only out-of-process tracer (ptrace / process_vm_readv
#   / personality / /proc); this host is Darwin.
#   Nothing to install — this is an OS gate, not a missing dependency. On macOS
#   the single-step tracer is the Mach-exception tier: make mach-stepper-test.
```

That gate ([mk/cli.mk:211-237](../../../mk/cli.mk#L211-L237), and its sibling for
`shared-asmspy` at [mk/cli.mk:183-193](../../../mk/cli.mk#L183-L193)) is correct
today and this plan does not remove it — it makes the Darwin branch *build
something* instead of skipping.

> Status legend: **planned** unless noted. Update as phases land.

> **CURRENT STATUS 2026-08-07 — 0 of 5 phases. Nothing cut.** This is the initial
> draft, written against a verified working tree on the Intel-macOS dev host
> (`Darwin x86_64`, Capstone 5.0.9 + ncursesw both present via pkg-config, so the
> toolchain is **not** the blocker). No code has been written. The go/no-go is
> **Phase S1**; **Phase S3 is the phase most likely to be cut permanently** and is
> written to be cuttable — read [The dominant risk](#the-dominant-risk-stated-first)
> before scheduling anything.

---

## Why this work exists

Two payoffs, one of which is not obvious:

1. **Trace macOS processes.** Today the only way to run asmspy from a Mac is
   `make docker-cli`, which traces processes *inside a Linux container*. That is
   genuinely useful and is the recommended path — but it cannot attach to a
   process running on the host.
2. **It is what unblocks the desktop GUI on macOS.** The GUI already has a real
   Darwin build path (Quartz + `-framework OpenGL`, [mk/desktop.mk:772-779](../../../mk/desktop.mk#L772-L779))
   and it reaches the engines **solely through the `asmspy --serve` subprocess** —
   never by linking libasmspy (D9, recorded in [desktop/src/vm_compat.cpp:23](../../../desktop/src/vm_compat.cpp#L23)).
   So the GUI on macOS is not blocked on GUI work; it is blocked on there being an
   `asmspy --serve` to talk to. A macOS asmspy that only ever implemented Phase S1
   would already give the GUI a live host to enumerate and fingerprint.

## The dominant risk, stated first

**macOS has no `PTRACE_SYSCALL`.** There is no kernel stop on syscall entry/exit,
no `PTRACE_GET_SYSCALL_INFO`, and nothing that reports one. This is not a missing
API to be looked up — it is a design difference, and it is load-bearing for a
large fraction of the view family: `--log`, `--stream`, `--follow`, `--procs`,
syscall argument decoding (Theme E's ~40-syscall table), and fd→endpoint
enrichment all hang off the syscall stop.

The three candidate routes, none free:

| Route | Verdict |
|---|---|
| **Single-step and decode the `syscall` instruction at RIP** | The only route that needs no privilege beyond what Phase S2 already has, and asmspy *already single-steps*. But it is orders of magnitude slower than a kernel stop — a syscall-heavy target becomes unusable — and it observes syscall **entry** naturally while **exit** needs the step after the instruction retires. **Presumed route; must be measured before S3 is scheduled.** |
| **DTrace (`dtrace -n 'syscall:::entry /pid == N/'`)** | Gives the real thing cheaply, but is SIP-restricted (needs SIP disabled or a `com.apple.security.get-task-allow` target), shells out to another process, and would make asmspy's syscall view a *different mechanism* from its trace view — two engines that can disagree. |
| **`EXC_SYSCALL` / `EXC_MACH_SYSCALL` Mach exceptions** | Names suggest a fit; **unverified** whether modern XNU delivers BSD syscalls through them for an arbitrary target. Research task in S0, not an assumption. |

**Plan consequence.** S3 is placed last, is independently cuttable, and every
earlier phase is specified so it is useful with S3 absent. If S3 is cut, macOS
asmspy ships the *code* views (trace / insns / graph / tree / watch / dataflow)
and the *topology* views, and refuses the syscall views with a measured reason —
which is a legitimate shape, not a degraded one. What it must never do is emit a
plausible-looking syscall log that is wrong; that is exactly the failure the
i386-tracee gate exists to prevent (Theme F, [libasmspy.h:325-329](../../../cli/libasmspy.h#L325-L329)).

## What already exists on macOS

Verified against the working tree 2026-08-07:

- **A working out-of-process Mach single-stepper.** [src/mach_backend.c](../../../src/mach_backend.c)
  (43 KB, gated `#if defined(__x86_64__) && defined(__APPLE__)` at [line 21](../../../src/mach_backend.c#L21))
  implements `task_for_pid` → Mach exception port → `EFLAGS.TF` via
  `thread_set_state`, plus `x86_DEBUG_STATE64` for hardware watchpoints. It uses
  `mach_vm_read` / `mach_vm_write` / `mach_vm_protect` / `task_threads` /
  `thread_suspend` — i.e. **every primitive Phase S2 needs is already written and
  exercised here.** Entry points ([include/asmtest_mach.h:70-119](../../../include/asmtest_mach.h#L70-L119)):
  `asmtest_mach_available` / `_skip_reason` / `_trace_call` / `_trace_attached` /
  `_run_to`. Its implementation doc is [macos-oop-mach-stepper.md](../archive/implementations/macos-oop-mach-stepper.md)
  (✅ closed), and `make mach-stepper-test` runs it on this host.
- **The codesign/entitlement harness.** Built for the stepper (its T6) — the
  `task_for_pid` permission problem is **already solved once** and must be reused,
  not re-solved.
- **A validated Intel-macOS host and CI leg.** This dev box, plus nightly
  `macos-15-intel` jobs in [ci.yml](../../../.github/workflows/ci.yml) (never
  target `macos-13` — retired 2025-12-08, [_positions.md #6](../implementations/_positions.md)).
- **The toolchain.** Measured on this host: `pkg-config --exists ncursesw` → yes;
  Capstone **5.0.9** → yes; `ncurses.h` in the CLT SDK. `CLI_MISSING`
  ([mk/cli.mk:32-38](../../../mk/cli.mk#L32-L38)) would be **empty** here. The
  only thing stopping the build is the OS gate above.

## The finding that makes this tractable

**[cli/libasmspy.h](../../../cli/libasmspy.h) is already OS-neutral.** Its ~950
lines declare the whole engine surface in `pid_t` / `uint64_t` / sink callbacks —
`asmspy_proclist`, `asmspy_fingerprint`, `asmspy_procinfo`, `asmspy_symtab_*`,
`asmspy_jitmap_*`, `asmspy_resolve`, `asmspy_ptrace_sample`, and the eight
`asmspy_engine_*` engines. Not one of them takes a `struct user_regs_struct`, a
`/proc` path, or a `PTRACE_*` request.

So this is **a second implementation behind an existing header**, not an API
redesign. Two leaks to fix in S0 and they are the whole list:

- `asmspy_elf_class(pid)` ([libasmspy.h:329](../../../cli/libasmspy.h#L329)) — the
  name is a format, not a question. The question is "is this tracee an ABI we
  would misdecode?"; rename to `asmspy_tracee_abi` (or similar) and let each body
  answer it from ELF `EI_CLASS` or Mach-O `cputype`.
- `asmspy_hwdebug_reason()` ([libasmspy.h:946](../../../cli/libasmspy.h#L946)) —
  already just a string; verify no Linux vocabulary leaks into its contract text.

Conversely, [cli/asmspy_arch.h](../../../cli/asmspy_arch.h) is **not** the seam it
looks like. It abstracts *architecture* (x86-64 vs AArch64 register access) while
being unconditionally Linux underneath — it includes `<elf.h>`, `<sys/ptrace.h>`,
`<sys/user.h>` and `<sys/uio.h>` at [lines 26-32](../../../cli/asmspy_arch.h#L26-L32)
and its accessors are `PTRACE_GETREGSET` transfers. The Darwin body needs an
**OS** axis crossed with the existing arch axis. Do not try to widen
`asmspy_arch.h` in place; give the OS its own file (`asmspy_arch_mach.h`) selected
by the same `#if defined(__APPLE__)` split `src/mach_backend.c` already uses.

## Concrete blockers, measured

Counts taken 2026-08-07 over the four engine TUs (20,632 lines total:
[asmspy.c](../../../cli/asmspy.c) 9,597, [asmspy_engine.c](../../../cli/asmspy_engine.c) 5,773,
[asmspy_proc.c](../../../cli/asmspy_proc.c) 2,660, [asmspy_ptracesample.c](../../../cli/asmspy_ptracesample.c) 2,602):

| Linux facility | Count | Darwin replacement | Difficulty |
|---|---|---|---|
| `PTRACE_*` constants | **403** refs, 47 distinct requests | see the request table below | — |
| `ptrace()` call sites | **127** (engine 95, ptracesample 23, front end 9) | Mach | — |
| `process_vm_readv` / `_writev` | **22** in engine TUs | `mach_vm_read_overwrite` / `mach_vm_write` | mechanical |
| `/proc/<pid>/...` paths | **61** (resolver 44) | `sysctl` / libproc / `mach_vm_region_recurse` | mechanical |
| `Elf64_*` / `Elf32_*` | **43**, all in the resolver | Mach-O `LC_SYMTAB` / `nlist_64` / dSYM | **rewrite** |
| `pthread_timedjoin_np` | **3**, all in [asmspy.c](../../../cli/asmspy.c) (`:6573`, `:6739`, `:7512`) | condvar + a join flag, or a self-pipe | trivial |
| `<linux/futex.h>`, `<linux/perf_event.h>`, `<sys/user.h>`, `<asm/ptrace.h>`, `<sys/prctl.h>`, `<elf.h>` | — | drop / replace | mechanical |

The `PTRACE_*` requests, grouped by how hard the Darwin answer is:

- **Solved already in `mach_backend.c`** — `SINGLESTEP`, `PEEKTEXT`/`POKETEXT`,
  `GETREGS`/`SETREGS`/`GETREGSET`/`SETREGSET`, `POKEUSER` (debug registers).
- **Mechanical** — `ATTACH`/`DETACH`/`CONT` → `task_for_pid` + exception port +
  `task_resume`/`thread_resume`; `TRACEME` → the launch path re-shapes around
  `posix_spawn` with `POSIX_SPAWN_START_SUSPENDED`.
- **No analogue, needs a redesign** — `SEIZE`/`INTERRUPT`/`LISTEN` and the whole
  group-stop model (`PTRACE_EVENT_STOP`, `O_TRACESYSGOOD`); `O_TRACECLONE` /
  `O_TRACEFORK` / `O_TRACEVFORK` / `O_TRACEEXEC` and their `PTRACE_EVENT_*` +
  `GETEVENTMSG`; `SYSCALL` / `GET_SYSCALL_INFO`.
- **Not portable, and correctly so** — nothing here; `--sample` is AMD IBS via
  `perf_event_open` and is out of scope (see below).

### The two redesigns, named

**1. Group-stop.** asmspy's engines are written against `SEIZE`/`INTERRUPT`/`LISTEN`
semantics: attach without stopping, interrupt on demand, and *listen* so a
group-stop is not converted into a resume. Mach gives `task_suspend` /
`thread_suspend` (a counted suspend, not a stop reason) and exception messages.
The port must define its own stop model on top and prove it, because the engines
assume things the Mach primitives do not provide. Related: the user guide records
a Linux 3.4+ floor precisely because `SEIZE`/`INTERRUPT` are the load-bearing pair.

**2. Child following.** `--follow` and the post-attach clone tracking in the
sampler depend on `PTRACE_O_TRACECLONE`/`TRACEFORK` delivering an event *at the
moment* a task appears. Mach has no such event; the fallback is polling
`task_threads` (threads) and `sysctl` (processes), which is racy by construction.
**This is where the copy-on-write int3 hazard lives** — the defect
[cli/forkhot_victim.c](../../../cli/forkhot_victim.c) exists to catch, where a
fork inside an armed window hands the child a private copy of a planted `int3`
and the child dies of `SIGTRAP` with no tracer. A polled approximation of
child-following on macOS **can kill the target's children**. So: the perf-free
picker's arming path ([asmspy_ptracesample.c](../../../cli/asmspy_ptracesample.c))
must either be proven safe under polling or must refuse to arm on Darwin. Refusing
is the acceptable answer; approximating is not.

---

## Phase S0 — Split the implementation behind the header *(planned, Linux-only, no behaviour change)*

**Goal.** Make room for a Darwin body without writing one. Nothing about Linux
behaviour changes; this phase is a pure refactor whose success criterion is
**byte-identical `make cli-smoke` output** before and after.

Steps:

1. Rename the two format-leaking symbols above (`asmspy_elf_class`, and audit
   `asmspy_hwdebug_reason`'s contract text) across the header, the four TUs, the
   tests and the GUI's serve wire. This is the only public-API change in the
   whole plan.
2. Move the Linux bodies to `_linux`-suffixed TUs behind the existing header, with
   the `#if defined(__linux__)` / `#else` shape [src/ptrace_backend.c](../../../src/ptrace_backend.c)
   already uses (its `#else` at the end returns `ENOSYS`/`EUNAVAIL` from every
   entry point). Add the `#else` stub file so libasmspy **links** on Darwin and
   every engine returns a "no Darwin body yet" code.
3. Open the `mk/cli.mk` OS gate *one notch*: on Darwin, build the pure modules and
   their tests only. These already compile anywhere and their own comments say so —
   `test_logview`, `test_graphsort`, `test_ghash`, `test_treefilter`,
   `test_autoregion`, `test_arch`, `test_sha256`, `test_asmtrace`, `test_view`.
   That is **nine tests that have never run on macOS** and can, today, with no
   Mach code at all.
4. Add a `cli-smoke-darwin` target that runs exactly those, and wire it into the
   nightly `macos-15-intel` leg.

**Research task, do it here, before S3 is scheduled:** determine by experiment
whether `EXC_SYSCALL` / `EXC_MACH_SYSCALL` can deliver an arbitrary target's BSD
syscalls on current macOS. Record the answer in this doc either way — a measured
"no" is what lets S3 be cut cleanly instead of staying a permanent open question.

**Acceptance.** `make cli-smoke` on Linux byte-identical to before. `make cli` on
Darwin builds `libasmspy.a` and the nine pure tests, all green, and the CLI's
engine-backed subcommands report an unimplemented reason rather than crashing.

## Phase S1 — The attach-free substrate *(planned — this is the go/no-go)*

**Goal.** Everything asmspy can answer **without `task_for_pid`**: process
listing, the fingerprint, the process snapshot, module enumeration, symbol
resolution. On Linux these are the `/proc` + ELF resolver
([asmspy_proc.c](../../../cli/asmspy_proc.c)); on macOS they are `sysctl` /
libproc / Mach-O.

Why this is the go/no-go rather than the stepper: the stepper is already proven
(`mach_backend.c`), whereas **nothing in this tree has ever read a Mach-O symbol
table**, and the resolver is 2,660 lines of ELF with 43 struct references. If the
Mach-O resolver does not come out clean, the rest of the plan has no way to name
an address, and a tracer that cannot name anything is not worth the remaining
phases.

| Linux source | Darwin replacement |
|---|---|
| `/proc` scan → `asmspy_proclist` | `sysctl KERN_PROC_ALL` / `proc_listpids` |
| `/proc/<pid>/stat`, `status`, `cmdline`, `exe` | `proc_pidinfo` (`PROC_PIDTBSDINFO`, `PROC_PIDTASKINFO`), `proc_pidpath` |
| `/proc/<pid>/maps` | `mach_vm_region_recurse` + `proc_regionfilename` |
| `/proc/<pid>/task/*` | `proc_pidinfo(PROC_PIDLISTTHREADS)` |
| `/proc/<pid>/fd/*` + `/proc/<pid>/net/{tcp,udp,unix}` | `proc_pidfdinfo` — **better than Linux here**, it returns socket endpoints directly instead of needing the `socket:[inode]` → `/proc/net` join |
| ELF `.symtab` / `.dynsym` / PLT | Mach-O `LC_SYMTAB` + `nlist_64`, `LC_DYSYMTAB`, stub sections |
| `.gnu_debuglink` + `.note.gnu.build-id` ([asmspy_proc.c:614-646](../../../cli/asmspy_proc.c#L614-L646)) | `.dSYM` bundles keyed by `LC_UUID` — same two-step "separate debug info, key **verified** not trusted" discipline |
| `__cxa_demangle` | unchanged (libc++abi) |
| perf-map / jitdump JIT resolve | unchanged — both are file formats, not kernel interfaces |

**Note the permission asymmetry, and treat it as a feature.** `test_procinfo`'s
key assertion is **negative**: the snapshot must succeed against a target we hold
no ptrace permission for ([mk/cli.mk:580-589](../../../mk/cli.mk#L580-L589)). On
macOS that property is *stronger* — libproc needs no `task_for_pid` at all — so
this phase delivers real function on an unsigned, unprivileged binary. Port
`test_procinfo` and `test_symtab` as the acceptance tests; both already exist and
both are about behaviour, not mechanism.

**Acceptance.** `asmspy --list`, `--procs`, `--info --json`, `--syms` all work on
a macOS host against a target the user does not own. `test_symtab`'s four edge
cases (one byte past a function, the gap between two, a zero-size symbol, an
address below the first) pass against a Mach-O binary. `--info --json` reports a
Darwin-shaped verdict — and specifically does **not** report a `perf_event_open`
probe, which is meaningless here; the GUI's schema-parity gate has to be told.

## Phase S2 — Attach, step, and the code views *(planned)*

**Goal.** `--trace`, `--insns`, `--graph`, `--tree`, `--watch`, and `--dataflow`
on a Mach target — the views that need registers, memory and single-stepping but
**not** syscall stops and **not** child following.

This is the phase that reuses `src/mach_backend.c` rather than rewriting it. The
work is to lift its internals (exception port setup, the receive loop, the
`EFL_TF` arm, the `x86_DEBUG_STATE64` watchpoint encoder) from the three-entry-point
`asmtest_mach_*` surface into something the engines can drive step-by-step, then
implement the Darwin `asmspy_arch_mach.h` accessors on top of
`thread_get_state(x86_THREAD_STATE64)`.

Sub-steps, in dependency order:

1. `asmspy_arch_mach.h` — `regs_read`/`regs_write`/`pc`/`sp`/`ret`/`lr` over
   `x86_THREAD_STATE64`. Pure, so it gets a `test_arch` sibling that runs
   anywhere, in the same "the pure module carries the burden" discipline
   `test_autoregion` uses.
2. Memory reads — `mach_vm_read_overwrite`, plus `mach_vm_protect` around
   breakpoint planting (Linux `POKETEXT` writes through read-only pages; Mach does
   not, so the protect/restore is a **new** requirement, not a translation).
3. The stop model — attach, interrupt, resume, detach, with the exception port as
   the stop source. Prove detach leaves the target **running and unmodified**
   (every planted `int3` restored), which is the property
   [asmspy_engine.c](../../../cli/asmspy_engine.c)'s detach path exists to
   guarantee.
4. The region engine (`asmspy_engine_region`) on top.

**Acceptance.** The existing `--trace` smoke assertions from
[cli/cli_smoke.sh](../../../cli/cli_smoke.sh) pass against Darwin-built victims.
Detach is proven clean by a victim that keeps running and produces correct output
afterwards. `--watch` arms a real `x86_DEBUG_STATE64` watchpoint per-thread —
[cli/watch_victim.c](../../../cli/watch_victim.c) already tests exactly the
per-thread property (a leader-only arm traps none of the worker's writes).

**Gate note.** This phase is where the `task_for_pid` permission gate first
bites. Reuse the stepper's codesign harness; self-skip with the same
`ASMTEST_MACH_EPERM`-shaped reason when neither the entitlement nor root is
available. Never fail — record.

## Phase S3 — Syscall observation *(planned; CUTTABLE — read the dominant risk)*

**Goal.** `--log`, `--stream`, syscall argument decoding, fd enrichment.

Do not schedule this until S0's research task has answered the `EXC_SYSCALL`
question and S2 has produced a **measured** single-step throughput number on a
real target. The decision rule, written now so it is not re-litigated later:

- If `EXC_SYSCALL` works for arbitrary targets → implement it; this becomes an
  ordinary phase.
- Else if measured single-step throughput makes a syscall-light target usable →
  implement the decode-at-RIP route, and have `--log` **print its cost model** and
  refuse (or warn hard) above a measured syscall rate. A view that is honest about
  being 100× slower is fine; one that silently makes the target unusable is not.
- Else → **cut it.** `--log` / `--stream` / `--follow` on Darwin return a measured
  reason naming the mechanism gap. Record the cut in this doc and in the user
  guide, and stop.

The syscall **name table** is a separate, smaller, fully-diagnosed problem.
[cli/gen-syscall-names.sh](../../../cli/gen-syscall-names.sh) generates from the
*compiling host's* `<sys/syscall.h>` by scraping `__NR_*` macros. **Measured on
this host 2026-08-07:** macOS defines **455 `SYS_*`** macros and **zero
`__NR_*`**, so the generator takes its own guard —

```
gen-syscall-names.sh: no __NR_* macros from cc <sys/syscall.h>
```

— and hard-fails the build. That is the guard working exactly as designed (its
comment says a silently-empty table would degrade every syscall to `syscall#N`
with no other symptom), and it means the fix is small and known rather than
exploratory: teach the `awk` to accept either prefix, and have the `SC(name)`
expansion in [asmspy_engine.c](../../../cli/asmspy_engine.c) emit
`[SYS_<name>]` on Darwin instead of `[__NR_<name>]`. **Do this in S0, not S3** —
it is a build-level fix, it is independent of whether S3 ever happens, and
leaving it means the Darwin build of the resolver TU cannot link at all
(`asmspy_proc.o` includes that table too, not just the engine).

The **argument-shape** table (Theme E, ~40 syscalls) is a different matter: it is
genuinely per-ABI and would need a Darwin column. That belongs to S3 and dies
with it if S3 is cut.

## Phase S4 — Child following, `--serve`, and the GUI *(planned; depends on S2, not S3)*

**Goal.** `--serve` on Darwin, which is what the desktop GUI consumes, plus an
honest answer on child following.

- **`--serve`** is a `unix(7)` socket ([asmspy.c](../../../cli/asmspy.c) uses
  `<sys/un.h>`) — portable as-is. The wire carries whatever the engines produce,
  so serve-on-Darwin is mostly a matter of the engines behind it existing. The
  serve-session recording test (`test_serve_record`, [mk/cli.mk:344-345](../../../mk/cli.mk#L344-L345))
  is pure and ports immediately.
- **`--follow` and post-attach clone tracking** — per the redesign section above,
  the acceptable Darwin answer is a **refusal with a measured reason**, not a
  polled approximation. The perf-free picker must refuse to arm rather than risk
  the copy-on-write `int3` killing a child it cannot see being born.
- **GUI wiring** — the desktop's `procinfo` fixture and schema-parity gate assume
  Linux-shaped host fields (`host.perf_ok` among them, per commit `3e2e8cea`). A
  Darwin host verdict has to be added to the schema, not smuggled through the
  Linux one.

---

## Task order & parallelism

```
S0 (refactor + 9 pure tests on macOS)   <- start here, no Mach knowledge needed
 |
 +--> S1 (sysctl/libproc/Mach-O)        <- GO/NO-GO. Independent of S2.
 |     |
 |     +--> S4 serve + GUI host         <- useful with S1 alone
 |
 +--> S2 (Mach attach/step/code views)  <- can run concurrently with S1;
       |                                   needs S1 only to NAME addresses
       +--> S3 (syscalls)               <- gated on S0's research + S2's measurement
       +--> S4 child-following refusal
```

S1 and S2 are genuinely independent and are the natural two-agent split: S1 is
file-format and libproc work with no privilege gate, S2 is Mach work that reuses
an existing TU and hits the codesign gate immediately.

## Constraints & gates

- **Host requirement (not a skippable dependency).** A macOS x86-64 host. One
  exists ([intel-macos-x86_64-de7ec54c](../../../benchmarks/boxes/intel-macos-x86_64-de7ec54c/features.json))
  and CI's Intel-mac leg is `macos-15-intel`. macOS cannot run in this project's
  Linux containers, so — unlike CLAUDE.md's "add it to the `Dockerfile.*`" rule —
  the Mach frameworks and `codesign` come from host Xcode CLT. This is a host
  lane that self-skips off Darwin. It is the same carve-out the Mach stepper and
  the DynamoRIO macOS port already operate under.
- **`task_for_pid` — soft gate, already solved once.** Entitlement
  (`com.apple.security.cs.debugger`, ad-hoc self-sign) or root, or a target
  carrying `get-task-allow`. Reuse the stepper's T6 harness. Self-skip with the
  reason when absent; exit 0.
- **SIP is a hard boundary.** asmspy on macOS **cannot** attach to SIP-protected
  or hardened-runtime binaries lacking `get-task-allow`. Linux's
  `PR_SET_PTRACER_ANY` opt-in — which is how every victim in [cli/](../../../cli/)
  makes unprivileged attach work — has no Darwin counterpart. A finished port
  reaches a strictly smaller set of processes than the Linux one, permanently.
  **Say this in the user guide; do not let it be discovered.**
- **Apple Silicon (arm64 macOS) is out of scope and hardware-gated.** The Mach
  substrate this builds on is x86-scoped (`x86_THREAD_STATE64`, `EFL_TF`,
  `EXC_I386_SGL`, `x86_DEBUG_STATE64`) and this dev host is Intel. arm64 needs
  `arm_thread_state64`, `EXC_ARM_BREAKPOINT`, `SPSR.SS` stepping and the arm64
  debug-register encoding — a future extension, gated out by the same
  `__x86_64__ && __APPLE__` guard `mach_backend.c` uses.

## Out of scope

- **`--sample`** — AMD IBS-Op via `perf_event_open`. macOS exposes no
  `perf_event_open`, and IBS is AMD hardware. Already self-skips on every
  non-AMD host; on Darwin it self-skips for two reasons instead of one. Recorded,
  not scheduled.
- **The `--info --json` perf verdict** — `<linux/perf_event.h>` and the
  `__NR_perf_event_open` probe are Linux-shaped questions. Darwin gets a Darwin
  verdict (S1), not a translation of this one.
- **The golden `.asmtrace` corpus** — generated under the Unicorn emulator from
  host-arch assembly and already `x86_64 + libunicorn` gated
  ([mk/cli.mk:648-679](../../../mk/cli.mk#L648-L679)); the committed corpus stays
  authoritative and is regenerated only from `make docker-cli`.
- **A macOS window picker** — already explicitly a non-goal in the GUI work
  ([mk/desktop.mk:792-798](../../../mk/desktop.mk#L792-L798)).

## Risks and open points

1. **Mach-O symbol resolution quality is unproven here** (S1, the go/no-go).
   Stripped release binaries with only `LC_DYSYMTAB` indirect symbols may resolve
   far less than a Linux `.dynsym` does. Measure against real system binaries
   before declaring S1 done — "it works on my `-g` test victim" is not the test.
2. **The stop model is a redesign, not a translation** (S2). Suspend counts are
   not stop reasons; the engines assume `SEIZE`/`LISTEN` semantics in more places
   than the `PTRACE_*` count suggests.
3. **Single-step throughput on Mach is unmeasured.** Every exception is a Mach
   message round-trip rather than a `waitpid`. If it is much worse than Linux,
   it re-scopes S3 *and* bounds S2's usefulness on hot regions. Measure in S2 and
   record the number here.
4. **The 32-bit refusal has a Darwin analogue and it must not be forgotten.** The
   i386 gate exists because misreading the ABI produces confidently-wrong output
   with rc=0. On macOS the equivalent question is `cputype` (and, on a future
   arm64 host, a Rosetta-translated x86-64 target — which is a *third* case
   neither existing gate covers). Answer it in S1, refuse in S2.
5. **`asmspy_ptracesample.c`'s exhaustiveness gate must survive the port.** Its
   `-Werror=switch -Werror=switch-enum` ([mk/cli.mk:91-100](../../../mk/cli.mk#L91-L100))
   is a correctness mechanism that already caught four rounds of the same defect
   class. If the Darwin body adds a stop kind, that gate is what makes every
   unrevisited site a build failure. Do not weaken it to get the port to compile.

## Phasing summary

| Phase | Delivers | Depends on | Gate |
|---|---|---|---|
| **S0** | Header split, `#else` stubs, 9 pure tests green on macOS | — | none |
| **S1** | `--list` / `--procs` / `--info` / `--syms` on Darwin | S0 | none (no `task_for_pid`) |
| **S2** | `--trace` / `--insns` / `--graph` / `--tree` / `--watch` / `--dataflow` | S0 (S1 to name addresses) | codesign / root |
| **S3** | `--log` / `--stream` — **or a recorded cut** | S0 research + S2 measurement | mechanism, possibly permanent |
| **S4** | `--serve` + GUI host; child-following refusal | S2 | — |
