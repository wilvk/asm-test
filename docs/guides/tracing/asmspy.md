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
   `PgUp`/`PgDn` to move, `Enter` to select, `q` to quit. `Tab` toggles the
   order between **PID** and **most recently active** (a short per-process CPU
   sample); `r` re-scans the list (re-sampling activity). Processes you cannot
   `ptrace` are marked with `!`. (`Tab`/`r` act when the filter is empty.)
2. **Mode select** — `1` for the syscall log, `2` for assembly & functions.
3. **Symbol picker** (mode 2 only) — the target's resolved function symbols,
   filterable; `Enter` picks the function to trace, `r` reloads the symbols
   (picking up newly-mapped libraries or fresh JIT code).
4. **Live view** — for the syscall log, a **split** feed: the syscall stream on
   the left, and the strings it carried (paths and read/write buffers, decoded
   up to 200 bytes) on the right. For assembly, two panes — the disassembly, **each instruction
   prefixed with its execution count** (so a hot loop body stands out), and the
   functions called, **ranked most-called first** — refreshing each time the
   function runs. Press `b` to go back to this process's options, or `q`/`ESC`
   for the process list.

The tracing runs on a dedicated thread so the UI stays responsive; quitting
detaches cleanly and leaves the target running untouched.

## Headless subcommands

The same engine drives four non-interactive subcommands — for scripts, CI, or
when you already know the pid. This is what `make cli-smoke` exercises.

```bash
asmspy --list [active]             # list processes (active = order by recent CPU, adds a CPU column)
asmspy --syms  <pid> [filter]      # resolved function symbols (addr, size, name, module)
asmspy --log   <pid> [n]           # stream n syscalls with decoded data (default 20)
asmspy --trace <pid> <sym> [n]     # n live samples of a function (default 3)
```

**Syscall log** — decodes `openat` paths, `write`/`read` buffers (up to 200
bytes each), `close`, and names common calls (the rest print as
`syscall#<nr>(args)`):

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
