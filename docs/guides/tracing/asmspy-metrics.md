# asmspy metrics and observability

Every number, token, and field [`asmspy`](asmspy.md) reports — what it measures,
what values it can take, and **which architectures can observe it at all**.

Use this page when you are consuming asmspy output rather than reading it: piping
`--json` to `jq`, parsing an `.asmtrace` recording, wiring a dashboard, or
deciding whether a metric you need exists on the host you have.

> **Read the [asmspy guide](asmspy.md) first** for what each view *is*. This page
> is the field-by-field reference behind it.

## The three output channels

Every headless view writes the same measurements through up to three channels,
and **the field names differ between them** — the human line is for a terminal,
`--json` is a single object shaped for `jq`, and the `.asmtrace` NDJSON is the
recording contract shared with the desktop GUI and the corpus recorder.

| Channel | How | Shape | Numbers rendered as |
|---|---|---|---|
| Human text | default | one line per event / a rendered table | decimal counts, `0x…` addresses |
| `--json` | `--json` on a view that supports it | **one** JSON object on stdout | addresses as `"0x…"` **strings** |
| `.asmtrace` | `--record=<f>`, or `--json` on `--log`/`--stream`, or `--serve` | NDJSON: a header line, event lines, an `end` footer | addresses as **decimal** u64 numbers |

Concretely: a `--graph --json` node carries `"addr":"0x401136"` while the same
node in a `--record` file carries `"addr":4198710`. A `--sample --json` edge names
its endpoints `from`/`from_name`; the recorded `survey` event names them
`from_addr`/`from`. Neither is a bug — they are two contracts — but a consumer
must pick one and not assume the other.

`--json` is available on `--log`, `--stream`, `--graph`, `--tree`, `--procs`,
`--sample`, `--watch` and `--dataflow`. `--dot` (Graphviz) is available on
`--graph`, `--tree` and `--procs`. `--record=<f>` works on **every** headless
view.

## Fidelity metrics — the ones that ride every view

These are not per-view measurements; they are asmspy's statement about *how much
to trust* the measurements. They appear in the `.asmtrace` header and `end`
footer (and, for the statistical sampler, in `--json` too).

| Metric | Values | Meaning |
|---|---|---|
| `provenance.backend` | `ptrace-syscalls`, `ptrace-stream`, `ptrace-region`, `ptrace-tree`, `ptrace-graph`, `ptrace-procs`, `ptrace-dataflow`, `hwdebug-watch`, `ibs-op` | Which engine produced the file. |
| `provenance.exact` | `true` \| `false` | `true` = every event in the window was observed; `false` = a **sample**. Only `--sample` is `false`. |
| `provenance.trust` | `exact` \| `statistical` (asmspy emits these two; the vocabulary also has `weak`/`strong`) | The tier word for the same fact. |
| `provenance.window` | `{base,len}` | Present when the capture was scoped to a region (`--trace`, `--dataflow`). |
| `provenance.skip` | `{code,reason}` | Present when the run **skipped**: the positive skip code plus the *measured* reason string. A skipped run still writes a closed, valid file. |
| `arch` (header) | `"x86_64"` \| `"aarch64"` | The **recording host's** architecture. asmspy traces same-machine processes, so this is also the tracee's. |
| `code` (header) | `{name,sha256,len}` | Routine identity — a SHA-256 of the region's live bytes. Emitted by `--dataflow --record` only; omitted where there are no stable bytes. |
| `end.events` | u64 | Event lines written before the footer. |
| `end.truncated` | `true` \| `false` | Something was dropped: a ring overflowed, a line did not fit, or emission was paused. |
| `end.drops.lost` | u64 | Samples the kernel dropped (`--sample` only; the ptrace engines are tail-drop and report `0`). |
| `end.drops.throttled` | `true` \| `false` | The kernel throttled the sample rate (`--sample` only). |
| `end.steps_total` | u64 | Steps the value trace **saw**, counting past the ring cap — the *M* in "N of M". `--dataflow` only. |

**A file with no `end` line is a torn recording.** There is no `atexit` rescue;
a reader must say "torn" rather than present a prefix as complete.

### Skip codes

A **positive** return is a fact about the *target or the host*; a **negative** one
is a failure of the tracer. They are never mixed.

| Code | Name | Meaning | Exit status |
|---|---|---|---|
| 1 | `ASMSPY_REGION_NEVER_RAN` | The region was watched and did not execute in the window (not "never runs"). | 1 |
| 2 | `ASMSPY_SAMPLE_UNAVAIL` | AMD IBS-Op unavailable, or perf refused the open. | 0 (`# SKIP`) |
| 3 | `ASMSPY_DATAFLOW_UNAVAIL` | The value producer is not built here (off Linux x86-64, or no Capstone). | 0 (`# SKIP`) |
| 4 | `ASMSPY_WATCH_UNAVAIL` | No hardware data watchpoint: wrong architecture, or arming refused. | 0 (`# SKIP`) |
| 5 | `ASMSPY_ETRACEE_I386` | The tracee is a 32-bit (i386) process — **refused before any attach**. | 1 |

The `# SKIP` line carries the **measured** reason, not a list of suspects — for a
refused watchpoint it distinguishes "regset absent" from "zero slots" from "slots
present but unreservable", because those three send you to three different places.

## Per-view metrics

### `--list` / the TUI process picker

Pure `/proc`; no attach, no ptrace. Identical on every architecture asmspy builds
for.

| Metric | Column | Values | Meaning |
|---|---|---|---|
| pid | `PID` | int | Process id. |
| string density | `STR` | 0–1000 (per-mille) | Alphanumeric byte fraction of a sample of the process's readable, non-code mappings. Ranks "string-rich" processes first. **`--list scan` only**, and only for processes you may attach to; `0` otherwise. |
| cpu | `CPU` | u64 jiffies | CPU time (utime + stime) consumed **during a 150 ms sampling window** — a delta, not a total. **`--list active` / `--list scan` only**; `0` under the default pid sort. |
| user | `USER` | string | Owner username, or the numeric uid when unresolvable. |
| attachable | `ATT` | `yes` \| `-` | Same euid as you (or you are root). The TUI marks non-attachable rows `!`. |
| command | `COMMAND` | string | `argv` joined with spaces, or `[comm]` for a kernel thread. |
| runtime badge | (TUI only) | `JVM`, `py`, `node`, `jit`, … or empty | Cheap badge from argv0/comm plus the presence of a perf-map. The headless `--list` does not print it. |

### `--syms`

| Metric | Values | Meaning |
|---|---|---|
| addr | u64, printed 12-hex | Runtime address in the target (module load bias already applied). |
| size | u64 bytes | Symbol extent; `0` = unknown, which makes the symbol unusable as a `--trace`/`--dataflow` region. |
| name | string | `STT_FUNC` name, C++-demangled. |
| module | string | Basename of the backing ELF, or `jit` for a perf-map/jitdump method. |

Reverse lookup is exact-extent only: an address in the gap between two functions
resolves to **nothing** rather than to a confidently wrong neighbour.

### Process details (TUI mode `8`)

A no-attach fingerprint from `/proc` plus the mapped ELF — it works where ptrace
would be denied. Every field is architecture-neutral.

| Metric | Values |
|---|---|
| `runtime` | `JVM`, `.NET`, `CPython`, `Node/V8`, `Ruby`, `Perl`, `Mono`, `Erlang/BEAM`, `Go`, `PHP`, `native`, `?` |
| `evidence` | what identified it, e.g. `libjvm.so`, `.note.go.buildid` |
| `jitting` | bool — `/tmp/perf-<pid>.map` exists (actively JIT-compiling) |
| `threads` | int, from `Threads:` in `/proc/<pid>/status` |
| `rss_kb` | KiB, from `VmRSS` |
| `tracer_pid` | pid — `0` = untraced, else who already has it |
| `seccomp` | `0` off, `1` strict, `2` filtered, `-1` unknown |
| `elf_class` | `32` \| `64` \| `0` (unreadable). **`32` means asmspy will refuse to attach.** |
| `pie` / `static_linked` / `interp` | bool / bool / loader basename |
| `threadnames` | up to 6 distinct per-task `comm` names, with a "more were dropped" bit |
| `modules` | up to 10 notable mapped library basenames, with a "more were dropped" bit |

### `--log` — syscalls

| Metric | Channel | Values | Meaning |
|---|---|---|---|
| syscall name | all | string | From the **compiling host's own** `<sys/syscall.h>` (generated, never hand-maintained), so it cannot lag the kernel. |
| arity | human | exact for shaped calls | 49 syscalls have a declared argument shape; an unmodelled call prints its first three raw words followed by `...` rather than claiming an arity. |
| return value | human | signed long | `rax` (x86-64) / `x0` (AArch64). |
| `line` | `.asmtrace` `syscall` | string | The **payload-free** line: same name, fds, flag words, counts and return, with content replaced by `<path>`/`<sockaddr>`/`<N bytes>`. |
| `payload` | `.asmtrace` `syscall` | string, ≤ 200 bytes decoded | The decoded string the call carried. Present only when there was one — which is what lets a reader redact content without losing the call. |
| `[tid]` prefix | human | int | Present once more than one thread is followed. |

The 25 argument classes that decode precisely: raw word, signed int, size, fd
(resolved to its endpoint), dirfd (`AT_FDCWD`), path, open flags, octal mode,
mmap prot, mmap flags, clone flags, signal number, sigset bitmask,
`rt_sigprocmask` how, iovec **contents**, timespec, `lseek` whence, socket family,
`sockaddr` in/out **contents**, `ioctl` request, `fcntl` command, `futex` op,
`stat` result contents, `statx` result contents.

An fd resolves through `/proc/<pid>/fd` to a path, or through the target's **own
network namespace** to `TCP 127.0.0.1:40730->127.0.0.1:54681` /
`TCP LISTEN 127.0.0.1:8080` / `unix:/tmp/foo.sock`. A pipe stays `pipe:[inode]`.

### `--trace` — region samples

| Metric | Channel | Values | Meaning |
|---|---|---|---|
| `sample #N` | human | 1-based | Which captured invocation this is. |
| `ret` | human | signed long | The region's return value (`rax` / `x0`). |
| insns recorded | human `N insns` | u64 | Distinct instruction offsets recorded. |
| insns executed | human `(M executed)` | u64 | Total instructions retired in the region — the loop-inclusive count. |
| blocks | human, `coverage.blocks[]` | u64 offsets | Distinct basic-block entry offsets. |
| `blocks_total` / `insns_total` | `coverage` | u64 | Totals the engine counted, including anything the arrays could not hold. |
| per-instruction heat | human `N×` | u32 | How many times that offset executed. Capped at 512 displayed offsets. |
| per-callee count | human `N×` | u32 | Calls made to that callee, ranked most-called first. Capped at 256 edges. |
| call site | human `+0xoff` | u64 | Lowest observed call-site offset for that callee. |
| `truncated` | both | bool | The trace or the call record overflowed. |
| `basis` | `.asmtrace` `trace`/`coverage` | `"rel"` | Offsets are **relative to the region base**. Mandatory — a reader may never default it. |
| `off` / `disasm` | `.asmtrace` `trace` | u64 / string | One executed instruction. `disasm` is present only when Capstone is linked. |

### `--stream` — live instruction stream

Text only: `function+0xoff [module]  <disasm>`, prefixed `[tid]` once more than
one thread is followed. The `.asmtrace` `stream` event carries a single `text`
field — the engine hands the front end a formatted line and nothing else, so the
recording states that faithfully rather than inventing fields it never measured.

### `--graph` — whole-process call graph

Per node:

| Metric | `--json` | `.asmtrace` `graph` | Values |
|---|---|---|---|
| entry address | `addr` (`"0x…"` string) | `addr` (decimal u64) | u64 |
| name | `name` | `name` | resolved symbol, demangled, or `0x…` |
| module | `module` | `module` | basename, `jit`, or `?` |
| class | `kind` | `kind` | `internal` \| `external` \| `jit` \| `unknown` |
| times called | `invocations` | `invocations` | u64 |
| calls made | `out_calls` | `out_calls` | u64 |
| distinct callees | `fanout` | `fanout` | u32 |

Per edge: `{caller, callee, count}` in `--json` (addresses as `"0x…"`),
`{from, to, count}` in the recording (decimal). Edges are keyed by **entry
address, not node index**, so a consumer may re-sort or filter the node array
without invalidating them.

The human row renders as `[int]`/`[EXT]`/`[JIT]`/`[?]` plus
`inv=… calls=… fanout=… [module]`. `--sort` takes `invocations` or `fanout`
(`functions-called` is a synonym).

### `--tree` — live call tree

| Metric | `--json` | `.asmtrace` `call` | Values |
|---|---|---|---|
| emission order | `seq` | (line order) | 0-based |
| thread | `tid` | `tid` | int |
| call depth | `depth` | `depth` | int, 0 = top |
| callee entry | `addr` (`"0x…"`) | `addr` (decimal) | u64 |
| name / module | `name` / `module` | `name` / `module` | string |

`depth` is the height of a real per-thread **return-address stack** keyed on the
stack pointer, not a push/pop counter — so a `ret`, a `longjmp` over ten frames
and a C++ unwind all pop correctly, and a signal handler (which runs *below* the
interrupted frame) leaves the frames beneath it intact. Under `--focus=<sym>` the
depth is **re-based** so the focused function sits at 0.

Filters (`--depth`, `--focus`, `--module`) bound what is **printed**, never what
is tracked — so depths stay true and `n` counts surviving lines. The `--dot`
export additionally aggregates a per-node `entered=` count and per-edge call
counts.

### `--procs` — process/thread topology

| Metric | Values | Meaning |
|---|---|---|
| `tid` / `tgid` / `ppid` | int | Task, thread-group (process), parent process. |
| `leader` | bool | `tid == tgid` — the process's main thread. |
| `comm` | string | `/proc/<tid>/comm`. |
| `exe` | string | Process exe basename (leader tasks only). |
| `inv` | u64 | **The count whose meaning switches** — see `count`/`mode`. |
| `count` (`--json`) / `mode` (`.asmtrace`) | `syscalls` \| `calls` | What `inv` counts. Always emitted alongside it, because a bare number that silently changes meaning is exactly what an exporter must not produce. |

`--count=syscalls` (default) runs near full speed and is safe on any target;
`--count=calls` single-steps, so the whole tree crawls.

The exports carry the **flat task list**, not a rendered tree: the forest is
derivable from `tgid` + `ppid`, and exporting box-drawing glyphs would throw
information away.

### `--sample` — statistical hot edges (AMD IBS-Op)

Per edge:

| Metric | `--json` | `.asmtrace` `survey` | Values |
|---|---|---|---|
| source address | `from` (`"0x…"`) | `from_addr` (decimal) | u64 |
| target address | `to` (`"0x…"`) | `to_addr` (decimal) | u64 |
| source name | `from_name` | `from` | `"func+0xNN [module]"` or `0x…` |
| target name | `to_name` | `to` | same |
| samples on this edge | `count` | `count` | u64 |
| mispredicted | `mispred` | `mispred` | u32, ≤ `count` |
| retired a return | `is_return` | `is_return` | u32, ≤ `count` |

Provenance, emitted once per window:

| Metric | Values | Meaning |
|---|---|---|
| `samples` | u64 | Total IBS-Op samples drained. |
| `branch_samples` | u64 | Of those, retired **taken branches** — the ones that became edges. |
| `lost` | u64 | Samples the kernel dropped. |
| `throttled` | bool | The kernel throttled the sampling rate: lengthen `ms` rather than densifying. |
| `sampler` | `"ibs-op"` | Which sampler ran. |

Derived in the human view: `[misp N%]` is `mispred * 100 / count`; `[ret]` marks
`is_return`. Sorting is by `count` (default) or `mispred` (TUI `Tab`).

**This view is always `exact:false`.** It proves an edge *was* taken; absence
proves nothing, and cold code may be missing from a short window.

### `--watch` — hardware data watchpoint

| Metric | `--json` | `.asmtrace` `watch` | Values |
|---|---|---|---|
| hit index | `hit` | `hit_no` | 1-based |
| thread | `tid` | `tid` | int |
| program counter | `pc` (`"0x…"`) | `pc` (decimal) | u64 — see the per-arch note below |
| watched address | (top-level `addr`) | `addr` | u64 |
| direction | `dir`: `w` \| `r` \| `?` | `is_write`: `1` \| `0` \| `-1` | **Tri-state.** `?`/`-1` means the faulting instruction did not decode — a real outcome that must not collapse into either direction. |
| value | `value`: `"0x…"` \| `null` | `value` (decimal) + `value_ok` (bool) | The watched bytes read back **after** the access, host-endian. |
| width | `bytes` | `value_len` | 1, 2, 4 or 8 |
| location | `func`, `off`, `module` (omitted when unresolved) | same | Resolved through the ELF symtab then the JIT perf-map. |
| session scope | top-level `pid`, `addr`, `len`, `mode` (`write` \| `readwrite`) | header `window` | What was armed. |

Between hits the target runs at **native speed** — no single-stepping, no code
patching.

### `--dataflow` — scoped value trace + def-use

Top-level (`--json`):

| Metric | Values | Meaning |
|---|---|---|
| `func` / `base` | string / `"0x…"` | The captured region. |
| `result` | signed long | Return value of the invocation. |
| `steps` | size | In-region instructions captured. |
| `records` | size | Operand records across all steps. |
| `truncated` | bool | `--max=<n>` (or the producer's backstop) cut the capture; the **prefix** is returned and says so. It is a bound, not an error. |

Per step (`trace[]` in `--json`, `df_step` in a recording):

| Metric | Values | Meaning |
|---|---|---|
| `step` | 0-based | Step ordinal within this invocation. |
| `off` | u64 | Offset from the region base — **always** region-relative, and it carries no `basis` field. |
| `rbase` | u64, optional | The absolute base `off` is relative to. Omitted entirely (never `null`, never `0`) when unknown. |
| `when` | u64, optional | The `codeimage` logical timestamp whose bytes were live for this invocation. **`--serve` only.** Key on `(rbase, when)` together, never `when` alone. |
| `disasm` | string | Present only when Capstone is linked. |
| `ops[]` | see below | The operand records for this step. |

Per operand:

| Metric | `--json` | `.asmtrace` | Values |
|---|---|---|---|
| direction | `rw`: `r` \| `w` | `write`: bool | Read set vs write set. |
| location class | `loc`: `reg` \| `mem` | `space`: `reg` \| `abs` \| `off` | Register, absolute memory, or region-relative memory. |
| register id | `reg` | `reg` | **Capstone register id — an architecture-specific namespace.** x86-64 and AArch64 ids overlap numerically and mean different registers; resolve against the recording's `arch`. |
| addressing terms | `addr` | `base`, `index`, `scale`, `disp`, `addr` | Memory operands only; omitted for a register operand. |
| width | `size` | `size` | bytes |
| value | `value`: `"0x…"` | `value` + `value_valid` | Omitted when not captured. |
| wide | `wide`: bool | `wide` + `bytes` | `true` for XMM/YMM: the integer `value` is omitted and the bytes ride in `bytes` (lowercase hex, ≤ 64 bytes) when the producer serialized them. |

Def-use (L1): `defuse[]` in `--json` is `{from, to}` step pairs; the recording's
`df_edge` additionally carries `loc` — the consumer's read record, in the same
operand shape — so an edge names *which value* flowed.

Opt-in event streams (all absent by default, all `--record`/`--serve` only):

| Flag | Kind | Fields | Notes |
|---|---|---|---|
| `--steps` | `regstate` | `desc` + `values` (16 GPRs, `rip`, `rflags`) | Pre-state register file per step, 1:1 with `df_step`. Descriptor `user_regs@x86_64/sysv`. `rsp`/`rbp`/`rip` are the target's **real ASLR'd** values — compare per-step *changes*, not absolutes. |
| `--fpregs` | `regstate` (extended) + `fpenv` | `xmm0..15`, `mxcsr` | One `PTRACE_GETFPREGS` per step. 128-bit only; YMM is not carried. Implies the ring. |
| `--mem` | `mem` | `step`, `ea`, `size`, `rw`, `space` | A projection of the memory operands `df_step` already carries, lifted to a per-access address stream. Address and width, **not** bytes. |
| `--blame` | `blame` | `step`, `off`, `loc`, `cone[]`, `born_untraced` | Backward def-use cone, **including the sink**. `born_untraced:true` = no traced producer (argument, constant, pre-existing state) — provenance starts at instrumentation, and the cone is the sink alone, never empty. |
| `--statediff` | `statediff` | `step`, `changed{}`, `computed` | Register delta vs the previous held step. `computed:false` on the first held step, with an empty `changed` — a full delta there would claim everything changed. |
| `--continuous` | `df_invocation` | `pass`, `result`, `steps_total`, `truncated` | Delimits each re-armed pass; every pass restarts `df_step` at step 0, so a reader needs the marker to segment them. |

### `--auto` — what the picker chose, and on what evidence

`--auto` replaces the region argument with an out-of-band sample. Its decision is
itself reported (on stderr headless, as a `session state:"pick"` event under
`--serve`):

| Metric | Values | Meaning |
|---|---|---|
| `sampler` | `ibs-op` \| `sw-clock` | Which sampler actually ran after `auto` resolved. |
| `evidence` | `entry` \| `residency` \| `idle` | **The load-bearing field.** `entry` = a branch was observed landing on the function's first byte. `residency` = an IP histogram said time was spent there, which is *not* entry evidence. `idle` = nothing qualified. |
| `func`, `base`, `len` | string, u64, u64 | The region handed to the capture. |
| `weight` | u64 | Entry samples (`entry`) or residency samples (`residency`). |
| `sites` | u32 | Distinct call sites observed arriving. **`entry` only.** |
| `attempt` / `of` | int / int | 1-based candidate index and how many are ranked — the `sw-clock` path walks up to 3. |
| window | ms | `--window=<ms>`, default 400. An empty window is retried, not treated as a verdict. |

An idle target gets a genuine refusal (`no function was observed being ENTERED`),
never a guess.

## Observability by architecture

asmspy builds on **Linux only**, on **x86-64** and **AArch64**. There is no macOS,
BSD or Windows body, and no 32-bit body — a 32-bit tracee is refused at attach
(`ASMSPY_ETRACEE_I386`) rather than decoded against the wrong syscall table.

| View | x86-64 (Intel) | x86-64 (AMD Zen) | AArch64 | Gated by |
|---|---|---|---|---|
| `--list`, `--syms`, process details | ✅ | ✅ | ✅ | `/proc` + ELF only |
| `--log` syscalls | ✅ | ✅ | ✅ | host syscall table (see below) |
| `--stream` | ✅ | ✅ | ✅ | ptrace single-step |
| `--graph`, `--tree` | ✅ | ✅ | ✅ | ptrace single-step |
| `--procs` | ✅ | ✅ | ✅ | `PTRACE_SYSCALL` / single-step |
| `--trace` (region) | ✅ | ✅ | ✅ | software breakpoint + single-step |
| `--trace --tid=<t>` | ✅ DR0–3 | ✅ DR0–3 | ✅ `NT_ARM_HW_BREAK` | a free per-thread hardware **execution** breakpoint slot |
| `--watch` | ✅ DR0–3 | ✅ DR0–3 | ✅ `NT_ARM_HW_WATCH` | a free hardware **data** watchpoint slot |
| `--sample` (hot edges) | ❌ `# SKIP` | ✅ | ❌ `# SKIP` | AMD IBS-Op |
| `--dataflow` (+ `regstate`/`mem`/`blame`/`statediff`/`fpenv`) | ✅ | ✅ | ❌ `# SKIP` | live value producer: Linux **x86-64** + Capstone |
| `--dataflow --auto --sampler=ibs` | ❌ | ✅ | ❌ | AMD IBS-Op |
| `--dataflow --auto --sampler=sw` | ✅ | ✅ | ❌ | `PERF_COUNT_SW_TASK_CLOCK`, x86-64 build only |
| `--serve`, `--record`, `--launch` | ✅ | ✅ | ✅ | the underlying view |

Every ❌ above is a **clean self-skip**: `# SKIP` with the measured reason and
exit status 0 for `--sample`, `--dataflow` and `--watch`. None of them is a crash,
and none of them silently reports zeros.

### What differs between x86-64 and AArch64 even where both work

These do not change a metric's *name* or *meaning*, but they change what you can
conclude from it.

| Metric / behaviour | x86-64 | AArch64 |
|---|---|---|
| syscall number source | `orig_rax` (the shadow register) | `x8` at the entry stop (no `orig_` shadow) |
| syscall argument registers | `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` | `x0`–`x5` |
| syscall **name set** | the x86-64 table | the AArch64 table — calls that do not exist there (`open`, `access`, `stat`, `fork`, …) simply never appear; the `openat`/`faccessat`/`newfstatat` forms do |
| return value (`ret`, `result`) | `rax` | `x0` |
| region entry breakpoint | `int3` (1 byte); the stop lands **past** it, so every observation rewinds the PC | `brk #0` (4 bytes); the stop lands **at** the instruction — nothing to rewind |
| call-frame identity (`--tree` depth, `--graph` attribution) | return address pushed on the stack; the frame is `(return address, rsp - 8)` | `bl` writes the return address to `x30` (LR) and leaves SP unchanged; the frame is `(entry LR, sp)` |
| PLT stub naming | `.plt` reserves 1 header entry; `.plt.sec` (CET) reserves none | `.plt` reserves a 2-entry (32-byte) header |
| single-step teardown | `EFLAGS.TF` is writable, so a pending step is cleared directly | no user-writable step bit; the engine must **drain** a queued step trap and skip threads parked inside a syscall, or the trap outlives the detach and kills the target seconds later |
| watchpoint `pc` | the `#DB` fires **after** the access, so the raw PC is the *following* instruction; the engine walks back to name the accessing one when it decodes | the exception fires **on** the accessing load/store, so `pc` is that instruction |
| watchpoint encoding | DR0–3 + DR7 `R/W`/`LEN` fields | `DBGWVR` (8-byte-aligned base) + `DBGWCR` `BAS` byte-select; the window may not cross an 8-byte boundary |
| `reg` ids in `--dataflow` | Capstone x86 ids | *n/a* — the live value producer does not run here |
| disassembly (`disasm` text) | x86-64 syntax | AArch64 syntax; the same field, decoded for the host arch |

Both architectures require a `--watch` address aligned to `--len`, which also
satisfies the AArch64 no-crossing rule for every legal length.

## Host gates that are not architecture

An architecture ✅ above still needs the host to cooperate.

| Gate | Affects | Requirement |
|---|---|---|
| `ptrace` rights | every view except `--sample` and process details | same-uid **and** `ptrace_scope=0`, or `CAP_SYS_PTRACE`, or the target opted in via `PR_SET_PTRACER` |
| kernel floor | attach | Linux 3.4+ for `PTRACE_SEIZE`. An older kernel fails at the `ptrace` call and *looks like* a permission denial — check `uname -r` first |
| `PTRACE_GET_SYSCALL_INFO` | `--log` entry/exit split | Linux 5.3+; below that it falls back to an entry/exit toggle, which can desync when a thread is seized mid-syscall |
| `perf_event_open` | `--sample`, `--dataflow --auto` | same-uid at the default `perf_event_paranoid=2`; in a container either `--security-opt seccomp=unconfined` or **`--cap-add=PERFMON`** (which also bypasses a raised sysctl) |
| IBS `swfilt` | `--sample` | Linux ≳ 6.2 — the user-only filter that makes IBS unprivileged |
| Capstone | `disasm` fields, `--dataflow`, watchpoint direction decode | linked at build time; without it offsets and counts are still exact, the text is simply absent |
| soft-dirty / `PAGEMAP_SCAN` | `codeimage` events under `--serve` | Linux ≥ 6.7. Where absent, the session emits the measured reason as a `note` and captures anyway — a recording with no `codeimage` is normal |
| free debug-register slots | `--watch`, `--trace --tid` | qemu-user exposes none; some hypervisors report slots and then refuse to reserve one (`ENOSPC`) — the skip reason names which |
| W^X JIT pages | `--trace`, `--dataflow` | a genuinely W^X-enforced page refuses the entry breakpoint and self-skips as exactly that, not as "never executed" |

## Related

- [asmspy — the interactive process tracer](asmspy.md) — what each view is and how to drive it.
- [Hardware tracing](hardware-tracing.md) — the C surface behind `--sample` (`asmtest_ibs_survey_process`) and its fidelity contract.
- [Data-flow tracing](data-flow.md) — the value-trace / def-use tier `--dataflow` wraps.
- [Features & support matrix](../../reference/features.md) — the whole framework's arch × OS × language coverage.
