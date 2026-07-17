# asmspy — the interactive process tracer

`asmspy` is a small **ncurses front-end** over asm-test's out-of-process tracer.
Point it at any running process and watch, live and out of band:

- its **syscalls with data** — the buffers and paths crossing the kernel
  boundary (a mini `strace`);
- a chosen function's **assembly + call-graph** — the disassembled instructions
  that executed and the functions it called, resampled each time the target
  calls it; or
- a **whole-process live instruction stream** — every instruction as it runs,
  resolved to its function and disassembled; or
- its **statistical hot control-flow edges** — sampled by the CPU itself (AMD
  IBS-Op), **without ptrace and without single-stepping**, so it is safe on
  targets the single-step views can destabilize (a live JIT); or
- the **data flowing through one function** — a scoped value trace of a single
  invocation (each executed instruction with the register/memory values it
  read and wrote) plus a def-use graph over it; `--auto` picks the function
  the process is entering most often right now, so you need not name one; or
- **who writes a field** — a hardware data watchpoint that names each toucher
  and the value, at native speed between hits.

It is the interactive companion to the C tracer demos
([`examples/attach_trace.c`](https://github.com/wilvk/asm-test/blob/main/examples/attach_trace.c),
[`examples/syscall_log.c`](https://github.com/wilvk/asm-test/blob/main/examples/syscall_log.c)):
the same [out-of-process ptrace seam](native-tracing.md#out-of-process-variant-w2--ptrace),
wrapped in a terminal UI, plus a headless mode for scripting and CI. It lives in
[`cli/`](https://github.com/wilvk/asm-test/tree/main/cli).

> **Linux x86-64 only.** asmspy is built on `ptrace` single-stepping and
> `/proc`; there is no Windows or macOS build. The tracing *engine* it drives is
> the same one documented under [Hardware tracing](hardware-tracing.md) and
> [Native runtime tracing](native-tracing.md).

## Build

asmspy links the hardware-trace tier (for `run_to` / `trace_attached` +
disassembly) and needs **Capstone** and **libncurses-dev**. Those live in the
container images, so the recommended path needs nothing on your host:

```bash
make docker-cli          # build the asmtest-cli image and run the headless smoke
```

### Run it on your own host (native build)

A bare host is usually missing both deps; `make cli` self-skips with exactly this
guidance rather than a raw compiler error. To build natively (Debian/Ubuntu):

```bash
# 1. ncurses (the TUI) — apt is fine
sudo apt-get install -y libncurses-dev

# 2. Capstone (disassembly). The project pins and builds Capstone 5.0.1 from
#    source — Ubuntu's apt only ships Capstone 4 — so use the repo's own script
#    (installs to /usr/local, runs ldconfig, idempotent):
scripts/build-capstone.sh

# 3. build + run
make cli            # -> build/asmspy   (self-contained binary; no LD_LIBRARY_PATH)
./build/asmspy      # interactive TUI
```

If the loader can't find `libcapstone.so.5`, prefix once with
`LD_LIBRARY_PATH=/usr/local/lib`.

> **`make deps` does not build Capstone on Debian/Ubuntu.** Capstone is a pinned
> *source* build, so on apt hosts `make deps` only prints a one-line pointer to
> `scripts/build-capstone.sh` (to stderr — easy to miss) rather than installing
> it. Run `scripts/build-capstone.sh` directly, as in step 2 above.

**Verify the build** — the smoke target runs natively too; it builds the demo
victims and drives every subcommand end to end:

```bash
make cli-smoke      # prints "cli-smoke: PASS"
```

> **Permissions on your host.** The default Yama `ptrace_scope=1` lets you attach
> only to processes you started (or that opted in). To watch arbitrary processes,
> run asmspy as root (`sudo ./build/asmspy`) or lower the gate for the session:
> `echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope`. See
> [Permissions](#permissions) below.

### Interactively in a container

The TUI needs a real terminal, so run the image with `-it`:

```bash
make docker-cli                              # builds the image
docker run --rm -it --cap-add=SYS_PTRACE asmtest-cli bash   # then, inside:
  ./build/attach_victim &                    #   a demo process to watch
  ./build/asmspy                             #   drive the TUI
```

## The interactive TUI

Run `asmspy` with no arguments. It walks four screens:

1. **Process picker** — every process, filterable as you type; arrow keys /
   `PgUp`/`PgDn` to move, `Enter` to select, `q` to quit. `Tab` cycles the sort:
   **PID**, **most recently active** (a short per-process CPU sample), and a
   **quick string-scan** — samples each process's readable memory and ranks the
   most string-rich (highest alphanumeric density, the `STR` column) first, then
   by recency. `F2` toggles the flat list to a **process tree** (each root's
   children nested under it with `├─`/`└─` connectors — selection and filtering
   still work per row); `F3` re-scans (re-sampling). Processes you cannot
   `ptrace` are marked with `!`. (`Tab` acts when the filter is empty.)
2. **Mode select** — `1` syscall log, `2` a function's assembly & call-graph,
   `3` the whole-process live instruction stream, `4` the whole-process call
   graph (caller/callee invocation counts, sortable), `5` the whole-process live
   call tree (indented by call depth), `6` the process/thread topology (the whole
   process tree — threads and child processes — with per-task counts), `7` the
   statistical **hot edges** sampler (AMD IBS-Op, out of band — the only rich
   view that never ptraces or single-steps), `8` the **process details**
   fingerprint (runtime badge, thread makeup, notable modules — nothing is
   attached at all), `9` the **data flow** view (a scoped value trace + def-use
   of one function invocation).
3. **Symbol picker** (modes 2 and 9 — the views that trace one chosen
   function) — the target's resolved function symbols, filterable; `Enter`
   picks the function to trace, `F3` reloads the symbols (picking up
   newly-mapped libraries or fresh JIT code). **`Tab`** cycles the row order —
   **address** (the default), **hot edges** (a live AMD IBS-Op sample ranking
   by entry-arrival count, the same rule `--dataflow --auto` picks with — an
   IBS-less host falls back to address order with an explanation), and
   **name** (alphabetical) — mirroring the process picker's Tab-cycle sort.
4. **Live view** — depends on the mode:
   - *Syscall log* — a **split** feed: the syscall stream on the left, and the
     strings it carried (paths and read/write buffers, decoded up to 200 bytes)
     on the right.
   - *Assembly & functions* — two panes: the disassembly, **each instruction
     prefixed with its execution count** (so a hot loop body stands out), and the
     functions called, **ranked most-called first**, refreshing each time the
     function runs.
   - *Live stream* — a single scrolling feed of **every instruction as it
     executes**: its `function+offset [module]` and disassembly. Whole-process
     single-stepping is slow, so the target crawls while streamed (and resumes
     full speed on detach).
   - *Call graph* — one row per function seen calling or being called, each with
     its **invocation count** (times called), **calls made**, and **fan-out**
     (distinct functions it calls), plus an `[int]`/`[EXT]` marker for the
     target's own executable vs. a shared/system library. Press **`Tab`** to
     toggle the sort between *most-invoked* and *most functions called*
     (fan-out). Built from the same whole-process single-step, so it crawls too.
   - *Call tree* — a **two-pane** view: on the left, a live feed of function
     **entries indented by call depth** (`-> function [module]`; a `call` steps
     in and indents, a `ret` steps back out); on the right, the **disassembly of
     the selected function**. Press **`space`** to pause the feed, then the arrow
     / `PgUp`/`PgDn` / `Home`/`End` keys move a selection cursor through the
     functions and the right pane disassembles whichever is highlighted (read
     live from the target). Same whole-process single-step, so it crawls.
   - *Process tree* — the whole **process/thread topology** rooted at the target:
     each process node (`node <pid> [exe]`), its threads (`tid <tid> (comm)`), and
     its child processes nested underneath, each annotated with a live
     **invocation count**. **`Tab`** toggles what `inv` counts — **syscalls**
     (near full speed, safe on any target) or **calls** (single-step, richer but
     the whole tree crawls). Arrow keys select a node; **`Enter`** *drills in*,
     opening a live call graph of that node's process (the topology detaches
     first, so the drill-in re-attaches cleanly). Pre-existing child processes are
     discovered at attach; ones forked later are followed live.
   - *Hot edges (sample)* — a live table of the target's **hottest control-flow
     edges**, sampled out of band by AMD IBS-Op (see the `--sample` subcommand
     below): per edge a sample
     count, both endpoints resolved to `function [module]` (ELF symbols *and*
     JIT perf-map methods), and `[misp N%]`/`[ret]` tags, with honest provenance
     in the header (`branch/total samples`, `THROTTLED`, window size). **`Tab`**
     toggles the sort (sample count / mispredicts). The target is **never
     ptraced and never single-stepped** — it runs at full speed — so this is
     the safe view for a live JIT. Needs an AMD host with IBS (the menu says
     why when unavailable).
   - *Process details* — a **no-attach fingerprint** of the target: a runtime
     badge (JVM / .NET / CPython / Node·V8 / Go / …), its thread makeup,
     notable modules, and ELF traits — read from `/proc` and the mapped ELF
     only, so it works even where ptrace would be denied.
   - *Data flow* — pick a function (the same picker as mode 2); asmspy
     captures **one invocation** and shows each executed instruction with the
     **values** it read and wrote (registers and memory), with slice
     navigation over the def-use graph — select an operand and walk to the
     instruction that defined it. Single-step based, so the same JIT cautions
     as the other stepping views apply.

   In the two log feeds (syscall log, live stream) press **`space`** to **pause**
   and scroll back through history — `↑`/`↓`, `PgUp`/`PgDn`, `Home`/`End` move
   through the buffered lines (up to 2048); `space` again (or `End`, or scrolling
   past the newest line) resumes the live tail. The status bar shows `[PAUSED
   line/total]` while frozen. Scrolling still works after the target exits, so
   you can read the final history. The **call graph**, **hot edges**, and
   **assembly & functions** views pause + scroll the same way: `space` freezes a
   stable snapshot (scrolling up auto-pauses), the same keys move through it —
   in the two-pane region view `Tab` switches which pane scrolls — and `space`
   resumes the live view.

   Press `b` to go back to this process's options, or `q`/`ESC` for the process
   list.

The tracing runs on a dedicated thread so the UI stays responsive; quitting
detaches cleanly and leaves the target running untouched.

## Headless subcommands

The same engine drives every view as a non-interactive subcommand — for
scripts, CI, or when you already know the pid. This is what `make cli-smoke`
exercises.

```bash
asmspy --list [active|scan]        # list processes; active=recent CPU, scan=string-rich memory first
asmspy --syms   <pid> [filter]     # resolved function symbols (addr, size, name, module)
asmspy --log    <pid> [n] [--follow]                   # stream n syscalls with decoded data (default 20)
asmspy --trace  <pid> <sym|0xADDR[:LEN]> [n] [--tid=<t>]   # n live samples of a function (default 3;
                                   # samples whichever thread reaches it first, --tid pins one)
asmspy --dataflow <pid> <sym|0xADDR[:LEN]|--auto> [--json] [--tid=<t>] [--max=<n>] [--module=<m>] [--sampler=ibs|sw]
                                   # value trace + def-use of ONE invocation; --auto picks the target
asmspy --stream <pid> [n] [--tid=<t>] [--follow]       # stream n instructions live (default 20)
asmspy --graph  <pid> [n] [--sort=invocations|fanout] [--json|--dot] [--tid=<t>] [--follow]
                                   # whole-process call graph over n calls (default 200)
asmspy --tree   <pid> [n] [--depth=<d>] [--focus=<sym>] [--module=<m>] [--json|--dot] [--tid=<t>] [--follow]
                                   # whole-process live call tree (default 40 lines)
asmspy --procs  <pid> [n] [--count=syscalls|calls] [--json|--dot]   # process/thread topology, live counts
asmspy --sample <pid> [ms] [--json]                    # statistical hot edges via AMD IBS-Op (default 300 ms)
asmspy --watch  <pid> <sym|sym+off|0xADDR> [n] [--rw] [--len=1|2|4|8] [--json]
                                   # hardware DATA watchpoint: who touches a field, and the value
```

A **negative `n`** streams until the target exits or you interrupt. Malformed
arguments (a non-numeric or non-positive pid, an unknown `--list` sort) are
rejected with exit status 2 rather than coerced. On `--stream`/`--tree`/`--graph`,
**`--tid=<t>`** seizes and steps **only thread `t`** (no clone-following), so you
can isolate one worker while the process's other threads keep running at full
speed — a filtered stream drops the `[tid]` prefix, since only one thread is
followed.

**`--follow`** (on `--log`/`--stream`/`--graph`/`--tree`) additionally traces
child **processes** the target forks while traced, like `strace -f`. Each
followed process gets its **own** symbol table and fd namespace — a forked
child shares neither with its parent, so without the re-keying its syscalls
and frames would be decoded against the wrong process. `--follow` and `--tid`
are mutually exclusive by design: one says *exactly this task*, the other
*and everything it spawns*.

**Live stream** — every instruction as it runs, resolved to its function and
disassembled (`function+offset [module]  <disasm>`):

```text
$ asmspy --stream 1234 6
clock_nanosleep+0x5a [libc.so.6]             mov rbx, rax
clock_nanosleep+0x5d [libc.so.6]             mov eax, ebx
clock_nanosleep+0x5f [libc.so.6]             cmp ebx, -0x16
clock_nanosleep+0x62 [libc.so.6]             jne 0x787c95b15a89
hotfn+0x22          [attach_victim]          mov rax, qword ptr [rbp - 8]
hotfn+0x26          [attach_victim]          and eax, 1
```

**Call graph** — a **whole-process caller→callee graph** accumulated from the
live single-step: `--graph` watches every thread, attributes each `call` to the
function it lands in, and tallies, per function, how many times it was **called**
(`inv`), how many calls it **made** (`calls`), and how many **distinct functions
it calls** (`fanout`). Each row is tagged `[int]` (the target's own executable)
or `[EXT]` (a shared/system library). `n` bounds the number of calls recorded
before it reports (a **negative `n`** runs until the target exits); `--sort`
picks the ranking — `invocations` (most-called first) or `fanout` (most functions
called first):

```text
$ asmspy --graph 1234 200 --sort=invocations
call graph — 7 functions, sorted by invocations (pid 1234)
[int] helper                         inv=95      calls=0       fanout=0     [spy_victim]
[int] work                           inv=19      calls=95      fanout=1     [spy_victim]
[EXT] usleep@plt                     inv=19      calls=0       fanout=0     [spy_victim]
[int] main                           inv=0       calls=38      fanout=2     [spy_victim]
```

A call into a shared library goes through the **PLT**, so asmspy resolves each
stub to `name@plt` (from the module's `.rela.plt` relocations) and tags it
`[EXT]` — the stub lives in the caller's image but the call *leaves* to a
library. An address no symbol covers (JIT, stripped code) is shown `[?]`.

Direct calls are attributed exactly. An indirect call (`call rax`) is resolved
at the next stop and **verified against the machine state the call actually
left behind** — the stack pointer must have dropped exactly one slot and the
return address on the stack must be the call's own fall-through — so a signal
arriving in that one-instruction window cannot fabricate an edge into its
handler; the call simply re-arms and resolves for real after `sigreturn`.
Whole-process single-stepping is slow, so the target crawls while
the graph is built (and resumes full speed on detach). In the TUI (mode `4`) the
same view refreshes live and **`Tab`** toggles the sort.

The graph also exports: **`--graph <pid> [n] --json`** emits the nodes (entry
address, resolved/demangled name, module, an `internal`/`external`/`jit`/`unknown`
kind token, and the three counts) *plus* an `edges` array of
`{caller, callee, count}` records (addresses as `0x` strings) — pipe it to `jq`
or a visualizer; **`--dot`** emits a Graphviz digraph (kind-coloured nodes,
count-labelled caller→callee edges), so
`asmspy --graph <pid> --dot | dot -Tsvg -o graph.svg` renders the live call
graph as a picture.

**Call tree** — the same whole-process single-step, rendered as the call tree
unfolding live: one line per function **entry**, indented by the calling
thread's current **call depth**. Depth is a real per-thread **return-address
stack** — a frame is live exactly while the thread's stack pointer sits at or
below the slot its `call` pushed — so a `ret`, a `longjmp` over ten frames,
and a C++ unwind all pop correctly and the indentation cannot drift, while a
signal handler (which runs *below* the interrupted frame) correctly leaves the
frames beneath it intact. `n` bounds the number of call
lines (negative = until the target exits); each line is prefixed `[tid]` once
more than one thread is followed:

```text
$ asmspy --tree 1234 8
-> work [spy_victim]
  -> helper [spy_victim]
  -> helper [spy_victim]
-> usleep@plt [spy_victim]
  -> __nanosleep [libc.so.6]
    -> clock_nanosleep [libc.so.6]
-> work [spy_victim]
  -> helper [spy_victim]
```

Where `--graph` *aggregates* calls into per-function counts, `--tree` preserves
the **nesting and order** — the real-time companion to the flat graph. In the
TUI (mode `5`) the same feed scrolls live, with **`space`** to pause and scroll
back through history.

On a busy process the tree is a firehose; three substring-matched filters cut
it: **`--depth=<d>`** keeps `d` levels (like `tree -L`), **`--focus=<sym>`**
roots the output at a symbol's subtree and re-bases its depth to 0, and
**`--module=<m>`** keeps only that module's callees. They compose as AND with
`focus` applied first — `--focus=work --module=libc` means *the libc calls
`work()` makes*, not *libc calls, and separately work()*. Filtering bounds
what is **printed**, never what is tracked, so depths stay true and `n` counts
the surviving lines. (`--depth=0` is a usage error, not "unlimited" — omit the
flag for unlimited.) `--tree --json`/`--dot` export the same feed
machine-readably, mirroring the `--graph` exporters.

**Process/thread topology** — `--procs` draws the target's whole process tree —
each process node, its threads, and its child processes nested underneath with
`├─`/`└─`/`│` connectors (like `pstree`) — each task annotated with a live
count. `--count=syscalls` (the default) counts near full speed and is safe on
any target; `--count=calls` single-steps for richer counts, so the whole tree
crawls while it runs. Pre-existing children are discovered at attach; ones
forked later are followed live. In the TUI (mode `6`) `Tab` toggles the count
mode and `Enter` drills into a node's live call graph. **`--procs --json`**
exports the flat **task** list (`tid`/`tgid`/`ppid`/`leader`/`comm`/`exe`/
`inv`, plus a `count` mode marker — the forest the human view draws is
derivable from `tgid`+`ppid`); **`--procs --dot`** renders it with processes
as boxes (solid parent→child edges) and threads as dashed ellipses, so the two
kinds of "child" stay distinguishable.

**Statistical hot edges** — `--sample` is the odd one out, and deliberately so:
it never attaches `ptrace` and never single-steps. Instead it asks the CPU
itself — **AMD IBS-Op** (Instruction-Based Sampling) — to tag retired
instructions across every thread of the target, and keeps the taken branches:
each sample carries the branch's **source and target address**, giving a
statistical `from → to` control-flow edge. asmspy aggregates a window
(default 300 ms) into a hot-edge histogram with both endpoints resolved through
ELF symbols *and* the runtime's JIT perf-map, so managed Node/.NET/Java frames
are named — and the target runs at **full speed, completely unperturbed**,
which makes this the one rich view that is safe on exactly the targets the
single-step views can destabilize (a live JIT):

```text
$ asmspy --sample 1234 500
statistical hot edges (AMD IBS-Op, out of band) — 12 edges, 812/1604 branch/total samples (pid 1234)
statistical, not exact: an edge here WAS taken; absence proves nothing.
     391  hot_spin+0x1c [sample_victim]     -> hot_spin+0x8 [sample_victim]
     102  hot_spin+0x2e [sample_victim]     -> hot_spin [sample_victim]
      41  hot_spin+0x33 [sample_victim]     -> main+0x51 [sample_victim]  [ret]
```

`--json` emits the same histogram machine-readably (edges plus the
`samples`/`branch_samples`/`lost`/`throttled` provenance). The view is
**statistical**: it proves an edge *was* taken, never that code did *not* run,
and cold code may not appear in a short window — lengthen `ms` for more recall
(the kernel throttles the sampling rate, so a longer window beats a denser
one). It requires an AMD host with IBS (any Zen, Linux ≥ ~6.2 for the `swfilt`
user-only filter) but **no elevated privilege** — user-only sampling opens at
the default `perf_event_paranoid=2`, unlike most of perf; on any other host
`--sample` prints `# SKIP` and exits 0. In the TUI it is mode `7`. The
underlying library API is `asmtest_ibs_survey_process()` — see
[Hardware tracing](hardware-tracing.md) for the C surface and its honesty
contract.

**Syscall log** — **every** syscall is named (the table is generated from the
host's own `<sys/syscall.h>`, so it never lags the kernel), and ~40 common
syscalls decode their arguments precisely, with **exact arity**: `open`/
`openat` flags (+ octal mode only when a creating flag is set), `mmap`/
`mprotect` prot+flags, `clone` flags, signal numbers and sigset **bitmasks**,
`readv`/`writev` iovec **contents**, timespecs, `lseek` whence, socket
families. `write`/`read` buffers are decoded up to 200 bytes, and path
arguments are decoded for both the `open`/`stat`/`access`-style calls and the
`openat`/`newfstatat`-style `(dirfd, path)` family. A file descriptor is
resolved to what it points at (like `strace -y`), read from `/proc/<pid>/fd` —
a regular file shows its path, and a **socket resolves through the target's
own network namespace to its endpoint**: `TCP 127.0.0.1:40730->127.0.0.1:54681`,
`TCP LISTEN 127.0.0.1:8080`, `unix:/tmp/foo.sock`. (A pipe stays
`pipe:[inode]` — its peer is not knowable from `/proc` without scanning every
process's fds.) A syscall asmspy does not model prints its first three raw
words followed by `...` — it no longer asserts an arity it never established,
and a no-argument syscall shows none:

```text
$ asmspy --log 1234 7
access("/tmp/notes.txt", 0x0, 0x0) = 0
openat(AT_FDCWD, "/tmp/notes.txt", 0x241) = 3
write(fd=3</tmp/notes.txt>, "hello from pid 1234\n", 20) = 20
close(fd=3) = 0
openat(AT_FDCWD, "/tmp/notes.txt", 0x0) = 3
read(fd=3</tmp/notes.txt>, 64) = "hello from pid 1234\n" [20]
close(fd=3) = 0
```

(`close`'s fd shows no path: by the time asmspy formats the exit, the descriptor
is already gone — the honest thing to show.)

**Assembly & functions** — one sample is the disassembled instructions that
executed (distinct offsets, in address order), the return value, and the
functions the region called — each resolved to a name and **ranked by call
count** (most-active first, so a hot callee sorts to the top):

```text
$ asmspy --trace 1234 work 1
tracing work @ 0x5598...51e3 (71 bytes) in pid 1234

sample #1   ret=35   54 insns (54 executed), 3 blocks
  assembly:                                      # <count>× per instruction
       1×  +0x0     endbr64
       1×  +0x5     mov rbp, rsp
    ...
       5×  +0x29    call 0x5598...51c9            # the loop body runs 5×
    ...
       1×  +0x46    ret
  functions called:                              # ranked most-called first
       5×  +0x29    ->  helper  [spy_victim]
```

The region argument is a **function name**, an **address a sized symbol covers**
(`0x5598...51e3` — the symbol's extent is used), or an **explicit range**
`0xADDR:LEN` / `0xADDR+LEN` (`LEN` bytes from `ADDR`, base-0). The explicit form
needs no symbol, so it reaches stripped code, a PLT stub, or a JIT region — feed
it an address from `--syms`, a `/proc/<pid>/maps` dump, or a disassembler:

```text
$ asmspy --trace 1234 0x5598000051e3:71     # same region, resolved by address
$ asmspy --trace 1234 0x7f00c0de00:0x40     # 64 bytes of code no symbol covers
```

**Data flow** — `--dataflow` captures **one invocation** of one function and
shows what flowed through it: every executed instruction with the **values**
it read and wrote (register and memory operands), plus a def-use graph over
the trace — in the TUI (mode `9`) you can select an operand and walk to the
instruction that defined it. The region argument is the same
`sym | 0xADDR[:LEN]` form `--trace` takes; `--json` exports the trace,
`--tid=<t>` pins the capture to one thread, and `--max=<n>` bounds the steps
captured — a cap smaller than the invocation returns the truncated prefix and
says so (`"truncated":true` in JSON); it is a bound, not an error. A function
that is not being called right now is reported honestly rather than waited on
forever: the entry wait is bounded (default 10 s; `ASMTEST_DF_ENTRY_WAIT_MS`
overrides, `0` waits without bound) and answers
`hotfn not seen entering in pid 1234 (waited 10000 ms)`.

**`--auto` — trace what the process is doing**, no symbol needed. With
`--auto` in place of the symbol, asmspy first samples the target **out of
band** (the `--sample` machinery: AMD IBS-Op, no ptrace, no perturbation,
~400 ms), ranks the hottest **entry** edge, and traces the winner:

```text
$ asmspy --dataflow 1234 --auto
--auto: entered_often [auto_victim] — 60 entry samples from 1 call site (of 1 candidate in 400 ms)
data flow — entered_often @ 0x... of pid 1234   ret=7   10 steps, 24 records
```

The rule deliberately ranks **entries**, not hot instructions: a branch that
lands on a function's first byte is a direct observation of the exact event
the capture waits for, while the hottest *address* is usually a loop body
inside a function entered once, before you attached (`main`, every event
loop) — a breakpoint there never fires. A JIT'd/managed method can win too:
the pick resolves through the ELF symtab *and* the JIT perf-map/jitdump. An
idle target — most targets, most of the time — gets an honest refusal
(`no function was observed being ENTERED`), not a guess. `--module=<m>` scopes
the pick (without it a hot libc routine often wins); `--auto` cannot combine
with `--tid` — the sampler carries no thread id, so pinning could only arm a
breakpoint on a thread that may never arrive.

Off AMD-IBS hosts (Intel, VMs, containers with perf allowed), `--auto` falls
back to a **portable software-clock sampler** (`PERF_COUNT_SW_TASK_CLOCK` +
`PERF_SAMPLE_IP`: no PMU at all; `--sampler=ibs|sw` forces either side). The
portable rule is honestly **weaker**: an IP histogram measures *residency* —
where time is spent — and residency's winner is often exactly the
entered-once-never-returns shape the entry rule rejects. So the sw path ranks
up to three candidates and **walks** them: a winner that is never seen
entering is refused at the bounded entry wait (an honest statement about that
candidate, reported as such), and the next-ranked candidate — usually the hot
callee — is tried:

```text
$ asmspy --dataflow 1234 --auto --sampler=sw
--auto[sw-clock]: grind_forever [auto_victim] — 391 residency samples at 7 offsets (of 3 candidates in 400 ms). Residency is NOT entry evidence; up to 3 candidates will be tried
data-flow capture of grind_forever @ 0x... (74 bytes) in pid 1234
--auto[sw-clock]: grind_forever was not seen ENTERING — a residency winner that never re-enters is the rule's known failure shape; trying candidate 2/3: entered_often (203 samples)
data flow — entered_often @ 0x... of pid 1234   ret=...   10 steps, 24 records
```

Where perf refuses the open entirely (Docker's default seccomp profile blocks
`perf_event_open`; `perf_event_paranoid` > 2 without `CAP_PERFMON`), both
samplers print a `# SKIP` naming the real errno and exit 0.

**Data watchpoint** — `--watch` answers *who touches this field, and with
what value*: it arms an x86 hardware **data** watchpoint (DR0-3; writes by
default, `--rw` adds reads; width `--len=1|2|4|8`) on an address in **every**
thread of the target, and reports each toucher and the value — while the
target runs at **native speed** between hits (no single-stepping, no code
patching; near-zero perturbation). The address can be a data symbol,
`sym+off`, or a raw `0xADDR`. x86-64 only (debug registers); it self-skips
where arming is refused (qemu-user, some sandboxes).

## How it works

asmspy is glue over primitives documented elsewhere; the interesting parts:

- **Attach seam.** It attaches to the pid you choose (it does *not* spawn it),
  then drives the library's
  [`asmtest_ptrace_run_to`](native-tracing.md#out-of-process-variant-w2--ptrace)
  + `asmtest_ptrace_trace_attached_ex` per sample, or a `PTRACE_SYSCALL`
  entry/exit loop for the syscall stream. On quit it detaches.
- **Whole-process thread following (syscall + instruction streams).** The syscall
  log and the live instruction stream both `PTRACE_SEIZE` *every* thread of the
  target and, via `PTRACE_O_TRACECLONE`, every thread it later spawns, then drive
  them all from one `waitpid(-1, __WALL)` loop — so a multi-threaded server shows
  the syscalls (or instructions) of all its workers, not just the main thread.
  Each line is prefixed `[tid]` once more than one thread is followed. For
  syscalls, entry-vs-exit is read from `PTRACE_GET_SYSCALL_INFO` (Linux 5.3+), so
  seizing a thread mid-syscall does not desync the pairing; on older kernels it
  falls back to an entry/exit toggle. (The region sampler seizes every thread
  too, racing them all to the traced function's entry and sampling **whichever
  arrives first** — a managed method almost never runs on the thread-group
  leader — with `--tid=<t>` to pin one thread via a per-thread hardware
  breakpoint.)
- **One tracer thread.** `ptrace` is per-thread — every `ptrace` call and
  `waitpid` for a tracee must come from the thread that attached it — so a single
  dedicated tracer thread owns the whole ptrace lifecycle while the ncurses UI
  thread only reads shared buffers and handles input. A quit wakes a blocked
  `waitpid` via `pthread_kill(SIGALRM)`.
- **Two-phase detach (crash-safe on JIT runtimes).** A whole-process single-step
  view leaves every thread stopped mid-instruction. Detaching them one at a time
  would let an already-released thread run while its siblings are still frozen
  mid-step; a JIT / managed runtime (V8/Node, the JVM, …) can catch that transient
  cross-thread inconsistency in an internal self-check and `IMMEDIATE_CRASH` via
  `int3` — a **fatal SIGTRAP that kills the whole target**. So detach is two-phase:
  interrupt and stop *every* thread first (clearing any armed single-step), then
  release them all at once — the same all-at-once semantics the kernel uses when a
  tracer dies (which is why a killed asmspy leaves the target unharmed). The
  single-step engines additionally **drain any queued debug exception** before
  releasing: a step that completes across a blocking syscall defers its trap
  until the syscall returns, so a parked worker thread would otherwise carry
  the pending trap through detach and die from it seconds later.
- **A target's own breakpoints are delivered, not swallowed.** The single-step
  engines distinguish their own step traps from an `int3` the target executed
  itself (a JIT's or debugger's software breakpoint) by the stop's `si_code`,
  and deliver the latter back so the target's own SIGTRAP handler runs — a
  self-breakpointing target keeps working while traced. (Delivering a trap
  suspends fine-grained stepping of that thread until its next stop, so a
  target that breakpoints in a loop may never reach a fixed `n` in batch mode —
  interrupt with `Ctrl-C`.)
- **Symbol resolution.** asmspy carries its own ELF reader — it parses the
  `.symtab`/`.dynsym` of every ELF mapped into the target and offsets each
  `STT_FUNC` by that module's load bias — and layers the runtime's **JIT
  perf-map** on top (`/tmp/perf-<pid>.map`, the text format V8/Node emit under
  `--perf-basic-prof`, .NET under `DOTNET_PerfMapEnabled=1`, and OpenJDK via
  perf-map-agent). Resolution tries the ELF table first, then the JIT map, and
  on a double miss re-reads the map (rate-limited) — a running JIT keeps
  compiling, so a one-shot snapshot would go stale. JIT frames render
  `name [jit]` in the stream/tree views and `[JIT]` in the graph. That combined
  table powers both the symbol picker (forward, by name) and every view's
  reverse lookup (by address).

## Permissions

Attaching to a process you did not start needs the usual `ptrace` rights — the
same rule as the [scoped / foreign-attach tracing](hardware-tracing.md) tiers:

- same-uid **and** `ptrace_scope=0` (`/proc/sys/kernel/yama/ptrace_scope`), or
- **`CAP_SYS_PTRACE`** (root / capability), or
- the target opted in via `prctl(PR_SET_PTRACER, …)`.

Otherwise the attach fails and asmspy shows the reason (the process list marks
non-attachable targets with `!`). In a default container, add
`--cap-add=SYS_PTRACE` (or have the target opt in, as the example victims do).

> **Kernel floor.** asmspy attaches with `PTRACE_SEIZE` (+ `PTRACE_INTERRUPT`),
> which needs **Linux 3.4+** (2012); the syscall stream's entry/exit split adds
> `PTRACE_GET_SYSCALL_INFO` (Linux 5.3+) and falls back to a toggle below that.
> On a kernel too old for `SEIZE` the attach fails at the `ptrace` call itself —
> which can *look like* a permission denial — so check `uname -r` before chasing
> Yama or `CAP_SYS_PTRACE`.

`--sample` (and `--dataflow --auto`, which samples first) is the exception: it
uses `perf_event_open`, not `ptrace`, so Yama's `ptrace_scope` does not
apply — it can sample any **same-uid** process at the default
`perf_event_paranoid=2`, no capability needed. In a container the perf syscall
must be allowed: either `--security-opt seccomp=unconfined`, or
**`--cap-add=PERFMON`** under the default profile — which also bypasses a
raised `perf_event_paranoid` (even the "no unprivileged perf at all" `=4`
setting), so prefer the capability over lowering a host sysctl. When sampling
cannot open, the `# SKIP` line names the **real** reason (permission vs
seccomp vs missing PMU), so trust what it says over guesswork.

## Limitations

- **Region tracing is per-invocation.** A sample is captured when the target
  *calls* the chosen function; a rarely-called function updates rarely, and a
  function that never returns (an event loop) never completes a sample — trace a
  callee it invokes instead. The target is briefly stopped between samples.
- **Leaf/helper model.** The traced region should be a deterministic,
  single-threaded routine; call-outs to helpers are stepped over at native speed
  and surfaced as call-graph edges (see the
  [trace_attached contract](native-tracing.md#out-of-process-variant-w2--ptrace)
  for the re-entrancy caveat).
- **The single-step views crawl.** `--graph`/`--tree`/`--stream` single-step
  every thread, so the target crawls while they run (and resumes full speed on
  detach). Direct calls are exact, PLT stubs resolve to `name@plt`, and
  indirect-call attribution is **verified** against the post-call stack state,
  so a signal cannot fabricate an edge into its handler; the residual signal
  cost is cosmetic (in `--tree`, handler frames render one level deep until
  `sigreturn`, because the interrupted call's frame is genuinely still live).
- **Argument decoding is a curated subset.** Every syscall is *named* with
  exact arity, and ~40 decode precisely (flags, sigsets, iovec contents,
  timespecs, socket families, paths, buffers) — but struct *contents* (`stat`
  buffers, `sockaddr`s), `ioctl`/`fcntl`/`futex` command decoding, and
  `execve` vectors are not decoded, and the undescribed remainder prints three
  raw words + `...`. For exhaustive syscall tracing use `strace`; for
  kernel-side IPC/file payloads use `bpftrace`. asmspy is the *in-tree,
  asm-focused* view, not a general strace replacement.
- **Data flow and watchpoints have hardware-shaped edges.** `--dataflow`
  reaches worker threads and JIT regions (the entry race seizes all threads,
  and a target's own traps are delivered, not swallowed), but a genuinely
  W^X-enforced JIT page refuses the entry breakpoint (reported as exactly
  that — a self-skip, not "never executed") and a method re-JIT'd *mid-capture*
  decodes from the live snapshot. `--watch` is x86-64-only (DR0-3 debug
  registers) and self-skips where arming is refused (qemu-user, some
  sandboxes).
- **`--sample` is statistical and AMD-only.** It reports edges that *were*
  sampled — it can never prove code did not run, cold paths may be missing from
  a short window, and per-thread sampling can miss a thread born and dead
  within one window. It self-skips (`# SKIP`, exit 0) on non-AMD hosts, in VMs
  without IBS, and on kernels without the `swfilt` user-only filter.
