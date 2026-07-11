# asmspy — the interactive process tracer

`asmspy` is a small **ncurses front-end** over asm-test's out-of-process tracer.
Point it at any running process and watch, live and out of band:

- its **syscalls with data** — the buffers and paths crossing the kernel
  boundary (a mini `strace`);
- a chosen function's **assembly + call-graph** — the disassembled instructions
  that executed and the functions it called, resampled each time the target
  calls it; or
- a **whole-process live instruction stream** — every instruction as it runs,
  resolved to its function and disassembled.

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
   by recency. `r` re-scans (re-sampling). Processes you cannot `ptrace` are
   marked with `!`. (`Tab`/`r` act when the filter is empty.)
2. **Mode select** — `1` syscall log, `2` a function's assembly & call-graph,
   `3` the whole-process live instruction stream, `4` the whole-process call
   graph (caller/callee invocation counts, sortable).
3. **Symbol picker** (mode 2 only) — the target's resolved function symbols,
   filterable; `Enter` picks the function to trace, `r` reloads the symbols
   (picking up newly-mapped libraries or fresh JIT code).
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
     target's own executable vs. a shared/system library. Press **`s`** to
     toggle the sort between *most-invoked* and *most functions called*
     (fan-out). Built from the same whole-process single-step, so it crawls too.

   In the two log feeds (syscall log, live stream) press **`space`** to **pause**
   and scroll back through history — `↑`/`↓`, `PgUp`/`PgDn`, `Home`/`End` move
   through the buffered lines (up to 2048); `space` again (or `End`, or scrolling
   past the newest line) resumes the live tail. The status bar shows `[PAUSED
   line/total]` while frozen. Scrolling still works after the target exits, so
   you can read the final history.

   Press `b` to go back to this process's options, or `q`/`ESC` for the process
   list.

The tracing runs on a dedicated thread so the UI stays responsive; quitting
detaches cleanly and leaves the target running untouched.

## Headless subcommands

The same engine drives five non-interactive subcommands — for scripts, CI, or
when you already know the pid. This is what `make cli-smoke` exercises.

```bash
asmspy --list [active|scan]        # list processes; active=recent CPU, scan=string-rich memory first
asmspy --syms   <pid> [filter]     # resolved function symbols (addr, size, name, module)
asmspy --log    <pid> [n]          # stream n syscalls with decoded data (default 20)
asmspy --trace  <pid> <sym> [n]    # n live samples of a function (default 3)
asmspy --stream <pid> [n]          # stream n instructions live: function + disassembly (default 20)
asmspy --graph  <pid> [n] [--sort=invocations|fanout]  # whole-process call graph over n calls (default 200)
```

A **negative `n`** streams until the target exits or you interrupt. Malformed
arguments (a non-numeric or non-positive pid, an unknown `--list` sort) are
rejected with exit status 2 rather than coerced.

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

Direct calls are attributed exactly; an indirect call (`call rax`) is attributed
to wherever the next step lands, so the graph is best-effort at signal
boundaries. Whole-process single-stepping is slow, so the target crawls while
the graph is built (and resumes full speed on detach). In the TUI (mode `4`) the
same view refreshes live and **`s`** toggles the sort.

**Syscall log** — **every** syscall is named (the table is generated from the
host's own `<sys/syscall.h>`, so it never lags the kernel), `write`/`read`
buffers are decoded up to 200 bytes, and path arguments are decoded for both the
`open`/`stat`/`access`-style calls and the `openat`/`newfstatat`-style
`(dirfd, path)` family. A `read`/`write` file descriptor is resolved to the file
it points at (like `strace -y`), read from `/proc/<pid>/fd` — a regular file
shows its path, a socket or pipe its `socket:[inode]` / `pipe:[inode]`. Calls
whose arguments asmspy does not model print their name with raw hex arguments:

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
  falls back to an entry/exit toggle. (The region sampler still steps one thread —
  it traces a single named function per invocation.)
- **One tracer thread.** `ptrace` is per-thread — every `ptrace` call and
  `waitpid` for a tracee must come from the thread that attached it — so a single
  dedicated tracer thread owns the whole ptrace lifecycle while the ncurses UI
  thread only reads shared buffers and handles input. A quit wakes a blocked
  `waitpid` via `pthread_kill(SIGALRM)`.
- **Symbol resolution.** The library resolves JIT methods (perf-map / jitdump)
  and module extents, but has no reader for ordinary ELF symbols, so asmspy
  carries its own: it parses the `.symtab`/`.dynsym` of every ELF mapped into the
  target and offsets each `STT_FUNC` by that module's load bias. That table
  powers both the symbol picker (forward, by name) and the call-graph pane
  (reverse, by address).

## Permissions

Attaching to a process you did not start needs the usual `ptrace` rights — the
same rule as the [scoped / foreign-attach tracing](hardware-tracing.md) tiers:

- same-uid **and** `ptrace_scope=0` (`/proc/sys/kernel/yama/ptrace_scope`), or
- **`CAP_SYS_PTRACE`** (root / capability), or
- the target opted in via `prctl(PR_SET_PTRACER, …)`.

Otherwise the attach fails and asmspy shows the reason (the process list marks
non-attachable targets with `!`). In a default container, add
`--cap-add=SYS_PTRACE` (or have the target opt in, as the example victims do).

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
- **Call-graph attribution is best-effort.** `--graph` builds a whole-process
  caller→callee graph by single-stepping every thread — so the target crawls
  while it runs. Direct calls are exact and PLT stubs resolve to `name@plt`, but
  an indirect call (`call rax`) is attributed to wherever the next step lands, so
  attribution can slip at a signal boundary. It counts *calls*, not a nesting
  tree — use `--trace` for one function's own call-graph edges.
- **Argument decoding is a subset.** Every syscall is *named*, and paths plus
  `write`/`read` buffers are decoded — but other argument types (flags, structs,
  vectors, signal sets) print as raw hex, and a syscall taking no arguments
  still shows three hex slots. For exhaustive syscall tracing use `strace`; for
  kernel-side IPC/file payloads use `bpftrace`. asmspy is the *in-tree,
  asm-focused* view, not a general strace replacement.
```
