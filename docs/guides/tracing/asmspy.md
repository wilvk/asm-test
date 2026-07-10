# asmspy — the interactive process tracer

`asmspy` is a small **ncurses front-end** over asm-test's out-of-process tracer.
Point it at any running process and watch, live and out of band:

- its **syscalls with data** — the buffers and paths crossing the kernel
  boundary (a mini `strace`), or
- a chosen function's **assembly + call-graph** — the disassembled instructions
  that executed and the functions it called, resampled each time the target
  calls it.

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

To build natively, install the toolchain first (a bare host is usually missing
both — `make cli` will tell you exactly what it needs and point you here):

```bash
sudo apt-get install -y libncurses-dev   # the TUI
make deps                                # Capstone (+ emulator deps)
make cli                                 # -> build/asmspy
```

For interactive use inside the container (the TUI needs a real terminal):

```bash
make docker-cli                              # builds the image
docker run --rm -it asmtest-cli bash         # then, inside the container:
  ./build/attach_victim &                    #   a demo process to watch
  ./build/asmspy                             #   drive the TUI
```

## The interactive TUI

Run `asmspy` with no arguments. It walks four screens:

1. **Process picker** — every process, filterable as you type; arrow keys /
   `PgUp`/`PgDn` to move, `Enter` to select, `q` to quit. Processes you cannot
   `ptrace` are marked with `!`.
2. **Mode select** — `1` for the syscall log, `2` for assembly & functions.
3. **Symbol picker** (mode 2 only) — the target's resolved function symbols,
   filterable; `Enter` picks the function to trace.
4. **Live view** — a scrolling syscall feed, or two panes (assembly + functions
   called) that refresh each time the function runs. `q`/`ESC` returns.

The tracing runs on a dedicated thread so the UI stays responsive; quitting
detaches cleanly and leaves the target running untouched.

## Headless subcommands

The same engine drives four non-interactive subcommands — for scripts, CI, or
when you already know the pid. This is what `make cli-smoke` exercises.

```bash
asmspy --list                      # list processes (ATT column = attachable)
asmspy --syms  <pid> [filter]      # resolved function symbols (name, size, addr, module)
asmspy --log   <pid> [n]           # stream n syscalls with decoded data (default 20)
asmspy --trace <pid> <sym> [n]     # n live samples of a function (default 3)
```

**Syscall log** — decodes `openat` paths, `write`/`read` buffers, `close`, and
names common calls (the rest print as `syscall#<nr>(args)`):

```text
$ asmspy --log 1234 6
openat(AT_FDCWD, "/tmp/notes.txt", 0x241) = 3
write(fd=3, "hello from pid 1234\n", 20) = 20
close(fd=3) = 0
openat(AT_FDCWD, "/tmp/notes.txt", 0x0) = 3
read(fd=3, 64) = "hello from pid 1234\n" [20]
close(fd=3) = 0
```

**Assembly & functions** — one sample is the disassembled instructions that
executed (distinct offsets, in address order), the return value, and the
functions the region called, each resolved to a name:

```text
$ asmspy --trace 1234 work 1
tracing work @ 0x5598...51e3 (71 bytes) in pid 1234

sample #1   ret=35   54 insns (54 executed), 3 blocks
  assembly:
    +0x0     endbr64
    +0x5     mov rbp, rsp
    ...
    +0x29    call 0x5598...51c9
    ...
    +0x46    ret
  functions called:
    +0x29    ->  helper  [spy_victim]
```

## How it works

asmspy is glue over primitives documented elsewhere; the interesting parts:

- **Attach seam.** It `PTRACE_ATTACH`es the pid you choose (it does *not* spawn
  it), then drives the library's
  [`asmtest_ptrace_run_to`](native-tracing.md#out-of-process-variant-w2--ptrace)
  + `asmtest_ptrace_trace_attached_ex` per sample, or a `PTRACE_SYSCALL`
  entry/exit loop for the syscall stream. On quit it `PTRACE_DETACH`es.
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
- **Syscall decoding is a subset.** `openat`/`write`/`read`/`close` are decoded
  with data; other calls print number + raw args. For exhaustive syscall
  tracing use `strace`; for kernel-side IPC/file payloads use `bpftrace`. asmspy
  is the *in-tree, asm-focused* view, not a general strace replacement.
```
