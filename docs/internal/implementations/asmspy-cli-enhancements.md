# asmspy CLI: TUI hot-edge drill-in, syscall-argument content decode, and test-coverage gaps — implementation

> **Sources.** Actioned from [asmspy-plan.md](../plans/asmspy-plan.md) (Theme E
> rows at lines 127–128, Theme D row at line 114, Theme H row at line 290).
> Written 2026-07-17. If this doc and a source disagree, this doc wins (sources
> may be stale); if the CODE and this doc disagree, re-verify before
> implementing.

## Why this work exists

asmspy already names every syscall with exact arity and resolves fds to real
endpoints, but the socket/control syscalls still render their most interesting
argument as raw hex (`connect(fd=4<TCP …>, 0x7ffc…, 16)`), the two safest rich
TUI views — hot edges (mode 7) and data flow (mode 9) — are not connected, and
four rows of the test-coverage grab-bag (group-stop, negative-`n`, region edge
aggregation, decoder breadth) still have no regression guard. After this work an
operator can read `connect(fd=4<…>, {AF_INET, 127.0.0.1:8080}, 16)`, press
Enter on a hot edge and land in a value/def-use trace of that function, and the
branches that already run live in production have smoke tests that fail when
they regress.

## What already exists (verified 2026-07-17)

The asmspy CLI lives entirely under [cli/](../../../cli/):

- [cli/asmspy.c](../../../cli/asmspy.c) — TUI + headless subcommands + `main`.
  The menu's mode numbers are 1-based; internally they are 0-based
  (`asmspy.c:4070-4087`), so menu "7) Hot edges" is internal mode 6
  (`:4902-4929`, `run_sample_view` at `:3364-3518`) and menu "9) Data flow" is
  internal mode 8 (`:4937-4973`, `run_dataflow_view` at `:3714`, which passes
  `V.tid = 0` at `:3722` — no thread picker). The call-graph drill-in idiom is
  `run_graph_detail` (`:2558`) invoked from the `KEY_ENTER` case at
  `:3029-3031`; the process-tree drill-in is at `:3276-3283`.
- [cli/asmspy_engine.c](../../../cli/asmspy_engine.c) — the ptrace engines and
  the syscall formatter. The per-syscall argument-shape table is
  `arg_shape()` (`:569-749`) over the `argcls_t` classes (`:516-535`); one
  class is rendered by `ap_arg()` (`:1024-1084`) from `format_syscall()`
  (`:1097`, known-shape loop at `:1136-1153` including the conditional-arity
  `A_MODE` drop at `:1142`). Target memory is read with the `rd()` +
  `process_vm_readv` pattern used by `ap_sigset` (`:966`), `ap_iovec` (`:989`,
  which takes its count from the NEXT argument — the precedent for length-carrying
  pointer args), and `ap_timespec` (`:1010`). Today: `ioctl` is
  `SHAPE(A_FD, A_HEX, A_HEX)` (`:650`), `fcntl` `SHAPE(A_FD, A_INT, A_HEX)`
  (`:654`), `connect`/`bind` `SHAPE(A_FD, A_HEX, A_INT)` (`:678`/`:682`),
  `accept` (`:690`), `sendto` (`:694`), `recvfrom` (`:698`), `futex`
  `SHAPE(A_HEX, A_INT, A_INT, A_HEX, A_HEX, A_INT)` (`:706`), `fstat`
  `SHAPE(A_FD, A_HEX)` (`:646`) — arity + fd only, contents undecoded.
  `stat`/`lstat`/`newfstatat`/`statx` have no shape entry at all; their path
  decodes via `path_kind()` (`:230-289`) and the buffer stays a raw word.
  `execve`/`execveat` are deliberately absent (`:224-228`: formatting happens
  at syscall EXIT, when a successful exec has replaced the address space the
  strings lived in) — keep them out. The job-control group-stop branch
  (`PTRACE_EVENT_STOP` + `PTRACE_LISTEN`) exists in all the tabled engines
  (`:1999-2012` syscalls, `:2899-2908` stream, `:3313` graph, `:3595` tree,
  `:3858` procs, `:4274-4278` region) and none has a smoke test.
- [cli/asmspy.h](../../../cli/asmspy.h) — the contract.
  `asmspy_engine_dataflow(pid, only_tid, base, len, max, stop, sink, ctx)` at
  `:331-333` takes an address range and races every thread to its entry (doc
  block `:299-330`); `asmspy_sample_edge_t` at `:502-509` carries
  `from_addr`/`to_addr`/`count`/`mispred`/`is_return` and **no tid**;
  `asmspy_symtab_at` (`:142`) and `asmspy_jitmap_at` (`:186`) are the pure
  reverse lookups.
- [cli/asmspy_autoregion.h](../../../cli/asmspy_autoregion.h) — the pure,
  header-only region-picking module (`asmspy_auto_resolve_fn` callback at
  `:83`, `asmspy_autoregion_rank` at `:125`), unit-tested by
  [cli/test_autoregion.c](../../../cli/test_autoregion.c). `auto_resolve_sym`
  in [cli/asmspy.c:1860-1874](../../../cli/asmspy.c) shows the ELF→JIT layered
  resolve the drill-in should reuse.
- [cli/cli_smoke.sh](../../../cli/cli_smoke.sh) — the headless end-to-end
  smoke. `expect_badarg` at `:51-63` (line 61 rejects a negative **pid**;
  nothing tests a negative **count**), the syscall-arg-decode block at
  `:1363-1421` (drives [cli/argdecode_victim.c](../../../cli/argdecode_victim.c)
  and asserts rendered text), the fd→endpoint block at `:1423-1454` (drives
  [cli/sock_victim.c](../../../cli/sock_victim.c), whose loop deliberately
  avoids `sendto` because it was undecoded — `sock_victim.c:80-83`), and the
  `--trace work` block at `:623-629`, which asserts a callee line exists but
  **not** that aggregation merged the five calls into one line with count 5.
- [mk/cli.mk](../../../mk/cli.mk) — `cli-smoke` prerequisites at `:341-353`
  (every fixture and unit test is listed there), fixture rule pattern at
  `:178-192` (`argdecode_victim`, `sock_victim`), `docker-cli` at `:364` and
  `docker-cli-ibs` at `:391` (same image, plus `--cap-add=PERFMON
  --security-opt seccomp=unconfined` so the IBS sampler actually runs on an
  AMD host).

Prove the baseline is green before touching anything:

```sh
make docker-cli          # builds asmtest-cli and runs cli/cli_smoke.sh in it
# expect: the unit tests print PASS, every block runs, and the container
# exits 0 with no "SMOKE FAIL:" line
make docker-cli-ibs      # on an AMD IBS host only: same smoke with perf access
# expect: the --sample/--auto blocks run their else-branches (no "# SKIP --sample")
```

On a non-x86-64 host `make cli`/`make cli-smoke` print `# SKIP` and exit 0 —
that is the architecture gate, not a failure.

## Tasks

Task numbering: T1–T4 are the four decode families of plan item
**E-syscall-arg-contents**; T5 is **H-tui-hotedge-drill**; T6–T9 are the four
rows of **D-test-coverage-gaps**.

### T1 — Decode sockaddr contents for the socket-syscall family  (M, depends on: none)

**Goal.** `connect`/`bind`/`sendto` render their `struct sockaddr *` argument as
`{AF_INET, 127.0.0.1:8080}` / `{AF_INET6, [::1]:8080}` / `{AF_UNIX,
"/path"}`, and `accept`/`recvfrom` do the same for their OUT pointer on
success, asserted by rendered-text smoke.

**Steps.**

1. In [cli/asmspy_engine.c](../../../cli/asmspy_engine.c), extend `ap_arg()`'s
   signature to take the syscall's `long ret` (it currently gets
   `(b, cap, o, pid, cl, e, i)` — `format_syscall` at `:1147` is its only
   caller and already holds `ret`). OUT-buffer classes need it; every existing
   class ignores it.
2. Add two classes to `argcls_t` (`:516-535`): `A_SOCKADDR` (IN: pointer whose
   byte length is the NEXT argument, the `A_IOVEC` precedent at `:1066-1067`)
   and `A_SOCKADDR_OUT` (OUT: pointer whose length lives BEHIND the next
   argument, a `socklen_t *`; decode only when `ret >= 0`, else print the raw
   pointer).
3. Write `ap_sockaddr(char *b, size_t cap, size_t o, pid_t pid, uint64_t addr,
   long long len)` next to `ap_sigset`. Behavior: `addr == 0` → `NULL`;
   `rd()` failure → `0x%llx` (the `ap_timespec` fallback idiom at
   `:1017-1018`). Read `min(len, sizeof(struct sockaddr_storage))` bytes into
   a local `struct sockaddr_storage`. Switch on the leading `sa_family`
   (first 2 bytes):
   - `AF_INET`: cast to `struct sockaddr_in`, render
     `{AF_INET, %s:%u}` with `inet_ntop(AF_INET, &sin_addr, …)` and
     `ntohs(sin_port)`;
   - `AF_INET6`: `struct sockaddr_in6`, `{AF_INET6, [%s]:%u}`, and append
     `%<scope>` only when `sin6_scope_id != 0`;
   - `AF_UNIX`: `struct sockaddr_un`; a first path byte of `\0` is an
     abstract socket → `{AF_UNIX, "@<name>"}`; empty (len == 2) →
     `{AF_UNIX}`; otherwise `{AF_UNIX, "<sun_path>"}` (bound `strnlen` by
     `len - 2`, never trust a NUL to be present);
   - anything else: `{family=%u, len=%lld}` — an honest raw form, never a
     guessed name.
   Use the host's `<netinet/in.h>`, `<sys/un.h>`, `<arpa/inet.h>` — the tracee
   is x86-64 like the tracer (i386 is refused at attach), so host layouts are
   the target's layouts. No new dependency.
4. Wire the classes in `ap_arg()`: `A_SOCKADDR` calls
   `ap_sockaddr(…, v, (long long)scarg(e, i + 1))`; `A_SOCKADDR_OUT` first
   requires `ret >= 0`, then `rd()`s a `socklen_t` from `scarg(e, i + 1)` and
   passes that as `len` (an `addrlen` of pointer-value 0 → print the raw
   pointer).
5. Update the shape table: `connect`/`bind` →
   `SHAPE(A_FD, A_SOCKADDR, A_INT)`; `sendto` →
   `SHAPE(A_FD, A_HEX, A_SIZE, A_HEX, A_SOCKADDR, A_INT)`; `accept` →
   `SHAPE(A_FD, A_SOCKADDR_OUT, A_HEX)`; `recvfrom` →
   `SHAPE(A_FD, A_HEX, A_SIZE, A_HEX, A_SOCKADDR_OUT, A_HEX)`; add
   `#ifdef __NR_accept4 … SHAPE(A_FD, A_SOCKADDR_OUT, A_HEX, A_INT)`. Also
   change `socket`'s first slot (`:674`) to a new tiny `A_SOCKFAM` class that
   renders the domain via the same AF-name switch (`AF_UNIX`/`AF_INET`/
   `AF_INET6`/`AF_NETLINK`, else the number) — one reused table, and it gives
   the smoke a line to pin.
6. Extend [cli/sock_victim.c](../../../cli/sock_victim.c)'s loop so
   sockaddr-bearing calls happen INSIDE the traced window (today `bind`/
   `connect`/`accept` all run once, before the smoke attaches): each iteration
   (a) `sendto(ufd, "u", 1, 0, (struct sockaddr *)&u, sizeof u)` — replacing
   the workaround the file's own comment at `:80-83` documents (delete that
   comment, keep the self-connect so `write` still exercises fd→endpoint);
   (b) create a throwaway `AF_INET`/`SOCK_DGRAM` socket, `connect()` it to
   `127.0.0.1:<the TCP port>` and `close()` it — a UDP connect sends no
   packet and cannot fail on loopback; (c) once per iteration, `connect()` a
   new TCP client to `lfd`'s port, `accept()` it on `lfd` into a
   `struct sockaddr_in` + `socklen_t`, and close both ends.
7. In [cli/cli_smoke.sh](../../../cli/cli_smoke.sh)'s sock_victim block
   (`:1423-1454`), raise the `--log` budget from 80 to 300 (the loop now makes
   ~4× the calls per iteration) and add the assertions under **Tests**.
8. `make docker-cli` after each step compiles; the smoke must pass at the end.

**Code.** All in `cli/asmspy_engine.c` (decoder), `cli/sock_victim.c`
(fixture), `cli/cli_smoke.sh` (assertions). No header/API change: the shape
table and `ap_*` helpers are file-static.

**Tests.** Extend the sock_victim block of `cli/cli_smoke.sh`:

- `grep -qE 'connect\(fd=[0-9]+<[^>]*>, \{AF_INET, 127\.0\.0\.1:'"$SKPORT"'\}'`
  — the IN sockaddr, with the port DERIVED from the run (the block already
  extracts `$SKPORT` from the victim's stderr);
- `grep -q '{AF_UNIX, "/tmp/asmspy_sock_victim.sock"}'` — the `sendto` path;
- `grep -qE 'accept\(fd=[0-9]+<[^>]*>, \{AF_INET, 127\.0\.0\.1:[0-9]+\}' ` —
  the OUT sockaddr filled in on success;
- `grep -qE 'socket\(AF_INET, '` — the domain name on `socket`;
- negative control: `printf '%s\n' "$skout" | grep -E '^(connect|bind|sendto)\(' | grep -q ', 0x7f'`
  must FAIL — no socket-family call may still render its sockaddr as a raw
  pointer.

A failure prints `SMOKE FAIL: <reason>` and exits 1; a pass prints the block's
summary line. Mutation check (run once, do not commit): revert the `connect`
shape entry to `A_HEX` — the first assertion must fail.

**Docs.** Update the "Argument decoding is a curated subset" honesty bullet in
[docs/guides/tracing/asmspy.md](../../../docs/guides/tracing/asmspy.md)
(~line 626) and the decode paragraph (~line 385): `sockaddr` moves from the
not-decoded list to the decoded list. Append to `CHANGELOG.md` under
`## [Unreleased]` / `### Added`. Strike the sockaddr clause of the plan's
Theme E remainder rows (lines 127–128) with a landed note, as every other
landed row in that file does.

**Done when.**
- `make docker-cli` passes with the five new assertions active.
- The identical smoke run with the shape-table change reverted fails (spot
  mutation check).
- The user guide no longer lists sockaddr contents as undecoded.

### T2 — Name ioctl requests and fcntl commands  (M, depends on: T1)

**Goal.** `ioctl` renders `TIOCGWINSZ`-style names (or an `_IOC(dir, type, nr,
size)` decomposition for unknown requests) and `fcntl` renders `F_*` command
names with correct conditional arity, asserted by rendered-text smoke.

**Steps.**

1. Add `A_IOCTLREQ` and `A_FCNTLCMD` to `argcls_t` and wire them in `ap_arg()`
   (they need only the register value, not `ret`).
2. `ap_ioctlreq(b, cap, o, unsigned long long v)`: first probe a small
   `#ifdef`-guarded name table built from the host's `<sys/ioctl.h>` macros —
   `TCGETS`, `TCSETS`, `TIOCGWINSZ`, `TIOCSWINSZ`, `TIOCGPGRP`, `TIOCSPGRP`,
   `FIONREAD`, `FIONBIO`, `FIOCLEX`, `FIONCLEX` (each entry
   `#ifdef`-guarded, the `oflag_tab` pattern at `:860-880`). On a miss,
   decompose per `include/uapi/asm-generic/ioctl.h`: `nr = v & 0xff`,
   `type = (v >> 8) & 0xff`, `size = (v >> 16) & 0x3fff`, `dir = (v >> 30) & 3`
   — render `_IOC(<dir>, '<type-char>', 0x<nr>, <size>)` with `dir` spelled
   from `{_IOC_NONE, _IOC_WRITE, _IOC_READ, _IOC_READ|_IOC_WRITE}` and the
   type shown as a character when printable (`0x20 <= type < 0x7f`), else hex.
   Never print a guessed name for an unknown request.
3. `ap_fcntlcmd(b, cap, o, long long v)`: a switch over the host `<fcntl.h>`
   macros (`_GNU_SOURCE` is already in effect in this file): `F_DUPFD`,
   `F_GETFD`, `F_SETFD`, `F_GETFL`, `F_SETFL`, `F_GETLK`, `F_SETLK`,
   `F_SETLKW`, `F_SETOWN`, `F_GETOWN`, `F_SETSIG`, `F_GETSIG`,
   `F_DUPFD_CLOEXEC`, and `#ifdef`-guarded `F_SETLEASE`, `F_GETLEASE`,
   `F_NOTIFY`, `F_SETPIPE_SZ`, `F_GETPIPE_SZ`, `F_ADD_SEALS`, `F_GET_SEALS`;
   default prints the number.
4. Shape table: `ioctl` → `SHAPE(A_FD, A_IOCTLREQ, A_HEX)`; `fcntl` →
   `SHAPE(A_FD, A_FCNTLCMD, A_HEX)`.
5. Conditional arity for `fcntl`, mirroring the `A_MODE` mechanism at
   `format_syscall:1141-1143`: after computing `n`, if `sh.c[1] == A_FCNTLCMD`
   and the cmd register is one of the argument-less commands (`F_GETFD`,
   `F_GETFL`, `F_GETOWN`, `F_GETSIG`, `F_GETLEASE`, `F_GETPIPE_SZ`,
   `F_GET_SEALS`), drop the third slot — printing a register the kernel
   ignored is the exact defect the conditional-`A_MODE` fix removed.
6. Extend [cli/argdecode_victim.c](../../../cli/argdecode_victim.c)'s loop:
   `fcntl(fd, F_GETFL)`; `fcntl(fd, F_SETFD, FD_CLOEXEC)`;
   `ioctl(fd, TIOCGWINSZ, &ws)` (fails `ENOTTY` on a regular file — the
   *request* still decodes, which is what is asserted); and one deliberately
   unknown request, `ioctl(fd, _IOC(_IOC_READ, 0xab, 1, 4), &dummy)`
   (`#include <sys/ioctl.h>` provides `_IOC`).
7. Add the assertions below to the argdecode block of `cli/cli_smoke.sh`
   (`:1363-1421`); `make docker-cli`.

**Code.** `cli/asmspy_engine.c`, `cli/argdecode_victim.c`, `cli/cli_smoke.sh`
only.

**Tests.** In the argdecode block, using its existing `ad_has` helper
(`:1386-1390`):

- `ad_has 'fcntl(fd=' "fcntl with a resolved fd"`;
- `ad_has 'F_GETFL) = ' "an argument-less fcntl cmd with NO third slot"` —
  the `) = ` directly after the cmd is the conditional-arity assertion;
- `ad_has 'F_SETFD, ' "a cmd that takes an argument keeps its slot"`;
- `ad_has 'TIOCGWINSZ' "a named ioctl request"`;
- `ad_has "_IOC(_IOC_READ, 0xab, 0x1, 4)" "an unknown ioctl request
  decomposed, not named"` — this is the form step 2's rule derives for the
  fixture's request: `dir` = `_IOC_READ`, `type` = 0xab renders as hex (0xab
  fails the `0x20 <= type < 0x7f` printable test, so it is NOT shown as a
  character), `nr` = 1 renders as `0x1`, `size` = 4. `ad_has` greps with `-qF`
  (fixed string), so the parentheses and the literal match verbatim.

Failure/pass shape is the block's existing one (`SMOKE FAIL` vs. the summary
echo). Mutation check: revert `ioctl`'s second slot to `A_HEX` → the
`TIOCGWINSZ` assertion fails.

**Docs.** Same three touch points as T1 (guide honesty bullet + decode
paragraph, `CHANGELOG.md` `### Added`, plan rows 127–128 clause strike).

**Done when.**
- `make docker-cli` passes with all five assertions active.
- `fcntl(fd=…, F_GETFL) = 0x…` lines show exactly two argument slots.

### T3 — Name futex operations  (S, depends on: T1)

**Goal.** `futex` renders its op as `FUTEX_WAIT`/`FUTEX_WAKE_PRIVATE`/… with
`FUTEX_PRIVATE_FLAG` and `FUTEX_CLOCK_REALTIME` masked out and re-rendered,
never silently dropped.

**Steps.**

1. Add `A_FUTEXOP` to `argcls_t`; include `<linux/futex.h>` (shipped by
   linux-libc-dev, already in the image — `__NR_futex` is already used at
   `:704`).
2. `ap_futexop(b, cap, o, long long v)`: split `cmd = v & ~(FUTEX_PRIVATE_FLAG
   | FUTEX_CLOCK_REALTIME)`; name `cmd` via a switch over `FUTEX_WAIT`,
   `FUTEX_WAKE`, `FUTEX_FD`, `FUTEX_REQUEUE`, `FUTEX_CMP_REQUEUE`,
   `FUTEX_WAKE_OP`, `FUTEX_LOCK_PI`, `FUTEX_UNLOCK_PI`, `FUTEX_TRYLOCK_PI`,
   `FUTEX_WAIT_BITSET`, `FUTEX_WAKE_BITSET`, `FUTEX_WAIT_REQUEUE_PI`,
   `FUTEX_CMP_REQUEUE_PI`, and `#ifdef FUTEX_LOCK_PI2` for the newest one.
   Append `_PRIVATE` when the private bit was set (strace's rendering);
   append `|FUTEX_CLOCK_REALTIME` when that bit was set; an unknown `cmd`
   prints the raw number plus the flag suffixes — the leftover-bits-print-as-hex
   discipline `ap_flagset` documents at `:840-843`.
3. Shape table: `futex` →
   `SHAPE(A_HEX, A_FUTEXOP, A_INT, A_HEX, A_HEX, A_INT)` (only slot 1
   changes; arg 4's meaning is op-dependent — timeout or val2 — and stays an
   honest raw word, deliberately out of scope per the bundle).
4. Extend `argdecode_victim.c`'s loop: a static `int futex_word = 0;` and
   `syscall(SYS_futex, &futex_word, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, NULL,
   NULL, 0)` — returns 0 waiters immediately, no blocking.
5. Assert and run `make docker-cli`.

**Code.** `cli/asmspy_engine.c`, `cli/argdecode_victim.c`, `cli/cli_smoke.sh`.

**Tests.** `ad_has 'FUTEX_WAKE_PRIVATE, 1' "futex op name with the private flag
folded in"` in the argdecode block, plus a negative control:
`printf '%s\n' "$adout" | grep -E '^futex\(' | grep -q ', 129, '` must fail
(129 = `FUTEX_WAKE|FUTEX_PRIVATE_FLAG` as a bare int — the pre-change
rendering). Mutation check: skip the private-flag masking → the assertion
fails (the op would render as the unknown number 129).

**Docs.** Same three touch points as T1.

**Done when.** `make docker-cli` passes; a `futex(…)` line in the smoke output
shows a `FUTEX_*` token.

### T4 — Decode stat/statx result buffers  (M, depends on: T1)

**Goal.** `fstat`/`stat`/`lstat`/`newfstatat`/`statx` render their result
buffer as `{st_mode=S_IFREG|0644, st_size=18}` on success (raw pointer on
failure), and the path-decoding behavior they already had is preserved.

**Steps.**

1. Add `A_STATBUF` and `A_STATXBUF` to `argcls_t`. Both are OUT classes: in
   `ap_arg()` they require `ret == 0` (T1's `ret` plumbing), else print the
   raw pointer.
2. `ap_statbuf(b, cap, o, pid, addr)`: `rd()` a host `struct stat`
   (`<sys/stat.h>`; tracer and tracee share the x86-64 ABI — i386 tracees are
   refused before attach). Render `{st_mode=<TYPE>|%04o, st_size=%lld}` where
   `<TYPE>` comes from an `S_IFMT` switch (`S_IFREG`, `S_IFDIR`, `S_IFCHR`,
   `S_IFBLK`, `S_IFIFO`, `S_IFSOCK`, `S_IFLNK`; unknown → the raw high bits
   in octal). Two fields only — a trace line is a line, not a record dump;
   mode and size are what a reader greps for.
3. `ap_statxbuf(…)`: `rd()` a `struct statx` (`<sys/stat.h>` on current glibc;
   fall back to `<linux/stat.h>` types if the build's glibc lacks it). Honor
   the mask: render `stx_mode` only when `stx_mask & (STATX_TYPE|STATX_MODE)`
   covers it and `stx_size` only under `STATX_SIZE`; a field the kernel did
   not fill is omitted, not invented.
4. Shape table: `fstat` → `SHAPE(A_FD, A_STATBUF)`; add new entries `stat` and
   `lstat` → `SHAPE(A_PATH, A_STATBUF)`; `newfstatat` →
   `SHAPE(A_DIRFD, A_PATH, A_STATBUF, A_HEX)`; `statx` →
   `SHAPE(A_DIRFD, A_PATH, A_INT, A_HEX, A_STATXBUF)` — each
   `#ifdef __NR_…`-guarded like every entry. Note: giving these syscalls shape
   entries takes them off the `path_kind()` fallback (`:262-278` routes
   `newfstatat`/`statx` today); the known-shape loop still fills the string
   pane for `A_PATH` args (`:1148-1150`), so the path decode the guide
   documents is preserved — verify the smoke's existing path assertions still
   pass.
5. Extend `argdecode_victim.c`'s loop, AFTER the writev/close of the 18-byte
   file: `struct stat st; syscall(SYS_stat, TMP, &st);` (pins the `stat`
   entry — glibc's `stat()` wrapper may route to `newfstatat`), plus
   `fstat(fd, &st)` on the open fd inside the writev block, plus
   `struct statx sx; syscall(SYS_statx, AT_FDCWD, TMP, 0, STATX_BASIC_STATS,
   &sx);` (`#include <linux/stat.h>` for `STATX_BASIC_STATS` if needed).
6. Assert and run `make docker-cli`.

**Code.** `cli/asmspy_engine.c`, `cli/argdecode_victim.c`, `cli/cli_smoke.sh`.

**Tests.** In the argdecode block:

- `ad_has '{st_mode=S_IFREG|0644, st_size=18}' "a stat buffer's contents"` —
  18 is DERIVED from the fixture's own writev (9+9 bytes) and 0644 from its
  open, so the assertion cannot pass against a stale decode;
- `ad_has 'statx(AT_FDCWD, ' "statx dirfd + path"` and
  `ad_has 'stx_size=18' "a statx buffer honoring its mask"`;
- negative control: a FAILED stat must not render a struct — the fixture adds
  `syscall(SYS_stat, "/nonexistent-asmspy", &st)` and the block asserts
  `grep -E 'stat\("/nonexistent-asmspy", 0x[0-9a-f]+\) = -' ` (raw pointer,
  negative return).

Mutation check: drop the `ret == 0` gate → the negative control fails (a
struct rendered from a buffer the kernel never filled).

**Docs.** Same three touch points as T1; the guide's `stat` example paragraph
(~line 388-391) gains the struct rendering.

**Done when.** `make docker-cli` passes with all three assertions; failed stats
still render a raw pointer.

### T5 — TUI mode 7 (hot edges) → Enter → mode 9 (data flow) drill-in  (M, depends on: none)

**Goal.** In the frozen hot-edges view, Enter on a selected edge opens the
data-flow view scoped to the function containing that edge's `to_addr` (falling
back to `from_addr`), reusing the existing drill-in idiom — no new UI concept.

**Steps.**

1. Add the pure decision to
   [cli/asmspy_autoregion.h](../../../cli/asmspy_autoregion.h):

   ```c
   /* Resolve ONE selected hot edge to a drillable region. Tries to_addr
    * (where control went), then from_addr; an endpoint qualifies only if it
    * resolves AND has size > 0 (the zero-size vacuity rule above). Returns
    * 0 and fills base/size/name/module, or -1 when neither endpoint names a
    * sized function. Pure — same resolve callback as the rank. */
   static inline int asmspy_edge_drill(const asmspy_sample_edge_t *e,
                                       asmspy_auto_resolve_fn resolve,
                                       void *ctx, uint64_t *base,
                                       uint64_t *size, const char **name,
                                       const char **module);
   ```

   The `size > 0` guard is load-bearing, not hygiene: `asmspy_symtab_at`'s
   zero-size branch matches exact-start only, and the data-flow engine needs a
   real `len` (`resolve_region` in `cli/asmspy.c:1394-1426` refuses unsized
   symbols for the same reason). Unlike `--auto`'s rank, the drill does NOT
   require `to_addr == start` — the operator picked a concrete edge; a
   mid-function `to_addr` (a loop back-edge) still names the function that
   contains it, which is the region they asked about.
2. Extend [cli/test_autoregion.c](../../../cli/test_autoregion.c) with checks
   for `asmspy_edge_drill` (see **Tests**). Run
   `make docker-cli` — the unit test runs first in the smoke (`cli_smoke.sh`
   runs `test_autoregion` via the `cli-smoke` prereqs in `mk/cli.mk:341-353`).
3. In `run_sample_view` (`cli/asmspy.c:3364-3518`), add a selection cursor
   `sel` alongside `top`, mirroring the call-graph view's `gsel`
   (`:3004-3027`): arrow keys move `sel` when frozen, navigation into the list
   auto-freezes (the `:2984-2991` idiom), and the renderer highlights row
   `top+r == sel` with `A_REVERSE` (the symbol picker's highlight style).
4. Add the Enter case, mirroring `:3029-3031`: when frozen and `V.snap.n > 0`,
   copy `V.snap.v[sel]` under `V.mu`, build the layered resolver — a local
   struct over the view's `syms` + `jit` exactly like `auto_resolve_sym`
   (`:1860-1874`, `asmspy_symtab_at` then `asmspy_jitmap_at`) — and call
   `asmspy_edge_drill`. On success: compose a title
   (`"asmspy — data flow of %s @ 0x%llx (pid %d, from hot edge)"`) and call
   `run_dataflow_view(pid, base, (size_t)size, title, name)` (`:3714` — it is
   self-contained: own tracer thread, own modal loop; the sampler keeps
   running out of band underneath, which is safe — it never ptraces, so the
   two cannot collide on the target). On its return, redraw the sample view
   (stay frozen). On failure: a one-line status message on the bottom row
   ("edge endpoints resolve to no sized function — cannot drill in"), the
   pattern of the mode-6 IBS-unavailable message (`:4906-4914`).
5. Update the bottom-row hints (`:3438-3450`) to advertise
   `Enter: data-flow drill-in` in the paused/finished states.
6. About the plan's "doubles as mode 9's missing thread picker": say it in a
   comment, honestly — `asmspy_sample_edge_t` carries no tid (`asmspy.h:502-509`;
   the IBS drain drops it), so the drill-in cannot and does not pick a THREAD.
   What it provides is the effect a thread picker was wanted for:
   `asmspy_engine_dataflow` races every thread to the chosen region
   (`asmspy.h:299-330`), so drilling into hot worker-thread code captures
   whichever worker actually runs it. Mode 9's entry path keeps `V.tid = 0`
   (`:3722`) — unchanged.
7. Manual verification (the TUI is pty-driven and not CI-drivable — the
   documented pattern for every landed TUI row in the plan): on an AMD IBS
   host, `./build/asmspy` → pick a busy victim (`./build/auto_victim` from the
   smoke fixtures is ideal: `entered_often` is hot) → mode 7 → `space` →
   arrows → Enter on an `entered_often` edge → the data-flow view captures and
   renders; `b` returns to the still-frozen hot-edges table. On a non-AMD
   host, mode 7 shows the IBS-unavailable message before any drill-in — no
   new gate.

**Code.** `cli/asmspy_autoregion.h` (+~40 lines), `cli/test_autoregion.c`
(+checks), `cli/asmspy.c` (`run_sample_view` selection + Enter, ~60 lines).
No engine or header-contract change.

**Tests.** The pure half carries the burden (the sibling-precedent split: the
sampler beneath is AMD hardware, the decision must be tested everywhere). New
`test_autoregion` checks, against the hand-built resolver table the file
already uses:

- `to_addr` inside a sized symbol → that symbol's `(base, size, name)`;
- `to_addr` unresolved, `from_addr` inside a sized symbol → the fallback fires;
- `to_addr` at a zero-size symbol's start, `from_addr` unresolved → **-1**
  (the vacuity control — a zero-size symbol must not win on a technicality);
- neither endpoint resolves → -1;
- a mid-function `to_addr` (start + 4) → still that symbol (drill ≠ rank).

A failing check prints the test's `FAIL` line and the binary exits nonzero,
which fails `cli-smoke` before any ptrace block runs. The TUI wiring itself has
no automated surface — record the manual pty run (step 7) in the commit
message, as the landed TUI rows did.

**Tests note (mutation).** Delete the `size > 0` guard → the zero-size check
fails; swap the try-order to from-first → the first two checks fail.

**Docs.** [docs/guides/tracing/asmspy.md](../../../docs/guides/tracing/asmspy.md)
"Hot edges (sample)" TUI bullet (~line 165) gains: freeze, select, and
`Enter` drills into a data-flow capture of the edge's function.
`CHANGELOG.md` `### Added`. Strike the plan's Theme H row (line 290).

**Done when.**
- `make docker-cli` passes with the new `test_autoregion` checks.
- Manual pty run on an AMD IBS host: mode 7 → Enter → a data-flow capture of
  the selected edge's function, and `b` returns to the frozen table.
- On a non-AMD host the view degrades exactly as today (message, no drill-in).

### T6 — Smoke the job-control group-stop branch  (S, depends on: none)

**Goal.** SIGSTOP/SIGCONT against a SEIZE'd victim exercises the
`PTRACE_EVENT_STOP` → `PTRACE_LISTEN` branch and asserts the victim really
suspends (honoring the stop) and really resumes.

**Steps.**

1. New block in `cli/cli_smoke.sh` (place it after the thr-OOM block, before
   the fd→endpoint section). The block starts **its own** `threads_victim`
   and assigns the pid to a **fresh** variable `GSPID` — do NOT reuse
   `$TVPID`: at this insertion point `$TVPID` is still the empty placeholder
   from `:70` (the existing `threads_victim` instance is not started until
   `:1692`, well after this block), so a fresh name avoids any confusion with
   that later, unrelated victim. Add `${GSPID:+"$GSPID"}` to the trap's kill
   list and `"$BUILD/gstop.log"` to its `rm -f` list at `:88`.
   `threads_victim` is multi-threaded — a group-stop stops every thread, so the
   branch fires once per tid. Settle 1 s, then run
   `timeout 60 "$ASM" --log "$GSPID" -1 > "$BUILD/gstop.log" 2>/dev/null &`
   and capture the asmspy pid. **Use `-1` (run until the target exits), not a
   fixed line budget:** `threads_victim`'s three workers each loop on
   `getpid()` + a 0.1 ms `nanosleep` (`threads_victim.c:32-39`), emitting
   syscalls at well over 400 lines/s, so a small budget would be exhausted
   during the settle — before the `kill -STOP` ever lands — and the tracer
   would already have exited, making steps 4–5 vacuous. `-1` keeps the tracer
   alive for the whole STOP/CONT cycle; the victim is killed explicitly in
   step 6.
2. Sleep 1 (lines flow), `kill -STOP "$GSPID"`, sleep 1.
3. Assert the victim is genuinely stopped:
   `grep -qE '^State:.[tT]' /proc/$GSPID/status` — under LISTEN the tracee
   stays in its group-stop (t/T); the mutation this guards (resuming with
   `PTRACE_SYSCALL` instead of `PTRACE_LISTEN`, the exact pre-fix hazard the
   branch comment at `asmspy_engine.c:1999-2004` names) leaves it running
   (S/R) and the assertion fails.
4. Assert asmspy is still alive (`kill -0` on its pid) — a tracer that dies on
   a group-stop event is the other regression shape. Under `-1` the tracer
   exits only when the victim does, so a live tracer here is unambiguous.
5. Assert the feed is actually paused: record `wc -l < "$BUILD/gstop.log"`,
   sleep 1, re-count — the count must not grow while stopped (all threads are
   in group-stop; nothing can make a syscall).
6. `kill -CONT "$GSPID"`, sleep 1, then re-count the log lines: the count
   **must now grow** past its stopped-state value — resumed threads making
   syscalls again is the proof the LISTEN'd threads woke (LISTEN wakes on
   SIGCONT; a branch that left them stopped forever would leave the count
   frozen). Then `kill "$GSPID"` and `wait` on the asmspy pid under the timeout
   guard: with all tracees gone it must exit **0** (the same until-exit
   contract T7 pins), never rc 124.
7. Repeat steps 1–6 once with a fresh `threads_victim` (reassigning `$GSPID`)
   under `--stream "$GSPID" -1` — the single-step engines' LISTEN branch
   (`:2899-2908`) takes the `stepped=1` path and is a different resume shape.
   `--stream` emits even more lines per syscall than `--log`, so `-1` +
   explicit kill is doubly the right shape here. Kill the victim; the `GSPID`
   and `gstop.log` cleanup added in step 1 already covers the trap.

**Code.** `cli/cli_smoke.sh` only. No product change.

**Tests.** This task IS the test. Failure looks like
`SMOKE FAIL: group-stop: victim state is S — LISTEN branch not honoring the
stop` (etc.); pass prints one summary line per engine. The step-3 and step-6
assertions are each other's control: state-t without later log-growth-after-CONT
would be a stuck tracee; log-growth-after-CONT without state-t would be a
run-through.

**Docs.** Internal-only, no user-facing docs — the behavior is already
documented in the engine comment; this adds its guard. Strike the
"job-control group-stop" clause in the plan's Theme D row (line 114) with a
landed note. `CHANGELOG.md`: no entry (test-only, not user-visible).

**Done when.**
- `make docker-cli` passes with both new sub-blocks running (no skip path —
  nothing here is hardware-gated).
- Manually reverting `PTRACE_LISTEN` to `PTRACE_SYSCALL` in the syscalls
  engine makes the state assertion fail (mutation check, do not commit).

### T7 — Smoke negative-`n` "run until exit"  (S, depends on: none)

**Goal.** `--log <pid> -1` and `--stream <pid> -1` return promptly with rc 0
when the target exits, guarded by timeout — the documented until-exit contract
(`usage()` at `asmspy.c:5094`; `parse_count` at `:5031-5039` accepts
negatives; the engines break out when every tracee is gone,
`asmspy_engine.c:1926-1933`).

**Steps.**

1. New fixture `cli/exit_victim.c` — the one victim that EXITS on its own
   (every existing victim loops forever, which is exactly why this row was
   never testable). Shape, mirroring `argdecode_victim.c`: `prctl(PR_SET_PTRACER,
   PR_SET_PTRACER_ANY, …)`, print `ready`, then ~400 iterations of
   `nanosleep({0, 5ms})` + `getpid()` (≈2 s of decodable syscalls, few enough
   instructions that a single-step run finishes in seconds), then `return 0`.
   The 2 s runtime against the smoke's 1 s settle guarantees the attach
   happens while it is still alive.
2. Build rule in `mk/cli.mk` next to `argdecode_victim` (`:178-183` pattern),
   and add `$(BUILD)/exit_victim` to the `cli-smoke` prerequisites
   (`:341-353`).
3. Smoke block: start `exit_victim`, sleep 1, then
   `set +e; out=$(timeout 60 "$ASM" --log "$EVPID" -1 2>&1); rc=$?; set -e`.
   Assert `rc -ne 124` ("negative-n --log did not return when the target
   exited") and `rc -eq 0`; assert the output contains `nanosleep(` or
   `clock_nanosleep(` lines (it traced, not just attached-and-returned).
4. Repeat with a fresh `exit_victim` and `--stream "$EVPID" -1`: assert rc 0
   within the timeout and that disassembly lines are present (the `:119-120`
   grep pattern). A fresh victim per run matters — the first one is gone.
5. Do NOT wait on the victim pids afterwards (they exited under trace); guard
   the trap line additions accordingly.

**Code.** `cli/exit_victim.c` (new, ~35 lines), `mk/cli.mk` (rule + prereq),
`cli/cli_smoke.sh` (block).

**Tests.** This task IS the test. Failure: rc 124 (never returned — the
regression shape) or rc nonzero (exit mishandled as an attach failure). Pass:
two summary lines. The rc-124 assertion is the load-bearing one: a `max < 0`
regression that spins on ECHILD forever is caught only by the timeout.

**Docs.** Internal-only, no user-facing docs (the flag is already documented
in `usage()` and the guide). Strike the "negative-`n`" clause of the plan's
Theme D row (line 114). No changelog entry (test-only).

**Done when.**
- `make docker-cli` passes with both negative-`n` runs green.
- `exit_victim` is in the `cli-smoke` prereq list and the trap cleanup.

### T8 — Assert mode-2 region call-graph edge AGGREGATION  (S, depends on: none)

**Goal.** The `--trace work` smoke pins that five calls to `helper` in one
invocation aggregate into exactly ONE "functions called" line with count 5 —
not five count-1 lines, which the current assertions would accept.

**Steps.**

1. The aggregation lives in `region_render`
   (`cli/asmspy.c:158-204`): edges merge per callee into `edge_agg_t` with a
   count, rendered `%4u×  +0x%-4llx  ->  %s  [%s]` and printed under
   `functions called:` (`:1375`). `spy_victim.c`'s `work(5)` calls `helper`
   exactly 5 times per invocation (`spy_victim.c:21-26`) — the expected count
   is derivable, not plausible.
2. In the `--trace $WVPID work 2` block (`cli_smoke.sh:623-629`), which
   already asserts presence (`grep -qE '^ *[0-9]+.*->.*helper'`), add:
   - exact count: `printf '%s\n' "$out" | grep -qE '^ *5×.*-> *helper' ||
     fail "region edges: work(5)'s five helper calls did not aggregate to a
     single 5× line"` (the `×` is the UTF-8 multiplication sign
     `region_render` emits in `%4u×` — grep matches it as a literal byte
     sequence, no special handling needed);
   - merge: `[ "$(printf '%s\n' "$out" | grep -c -- '-> *helper')" -le 2 ] ||
     fail "region edges: helper appears more than once per sample — edges not
     aggregated by callee"` (`-le 2` because the run captures 2 samples, one
     `functions called` section each).
3. `make docker-cli`.

**Code.** `cli/cli_smoke.sh` only.

**Tests.** This task IS the test. The two assertions are each other's control:
an aggregation-broken mutant (append per edge, never merge) emits five `1×`
lines — passes the old presence grep, fails BOTH new ones; a count-miscounting
mutant emits one line with the wrong number — fails the first only.

**Docs.** Internal-only. Strike the "region edge aggregation" clause of the
plan's Theme D row (line 114). No changelog entry.

**Done when.** `make docker-cli` passes; temporarily changing `agg[k].count++`
to a no-op in `region_render` (mutation check, do not commit) fails the exact-count
assertion.

### T9 — Decoder-breadth assertions for the already-landed shape set  (S, depends on: T1–T4 land first to avoid smoke-block churn)

**Goal.** The decode classes that landed with the shape table but have no
rendered-text assertion (`readv` contents, `dup2`, `ftruncate`, `close`'s fd
resolution on this fixture, `clock_nanosleep`'s four-arg shape, a second
arity-0 syscall) get one pin each, so a table-entry typo cannot land silently.

**Steps.**

1. Extend `argdecode_victim.c`'s loop: after the writev block, reopen the file
   `O_RDONLY`, `readv(rf, iov2, 2)` into two 9-byte buffers (asserts the
   READ-side iovec contents at exit), `dup2(rf, 17)`, `close(17)`; reopen
   `O_WRONLY` and `ftruncate(wfd, 4)`; call `syscall(SYS_getppid)` (a second
   `A_END` shape). `clock_nanosleep` is already driven — glibc routes the
   existing `nanosleep()` there (the block's comment at `:1403` says so).
2. Add `ad_has` assertions (see **Tests**) to the argdecode block.
3. `make docker-cli`.

**Code.** `cli/argdecode_victim.c`, `cli/cli_smoke.sh`.

**Tests.**

- `ad_has 'readv(fd=' "readv with a resolved fd"` and
  `ad_has '["iovec-one", "iovec-two"], 2) = 18' "readv CONTENTS at exit"`
  (the same bytes writev wrote — derived, not plausible);
- `ad_has 'dup2(fd=' "dup2 fd class"` and `ad_has ', 17) = 17' "dup2's plain
  int second arg and return"`;
- `ad_has 'ftruncate(fd=' "ftruncate"` and `ad_has ', 4) = 0' "its A_SIZE arg"`;
- `ad_has 'getppid() = ' "a second arity-ZERO shape"`;
- `ad_has 'clock_nanosleep(0, 0, {0.002000000}' "the 4-arg clock_nanosleep
  shape"`.

Failure/pass shape is the block's existing one.

**Docs.** Internal-only. Strike the "syscall-decoder breadth" clause of the
plan's Theme D row (line 114). No changelog entry.

**Done when.** `make docker-cli` passes with all breadth assertions active.

## Task order & parallelism

- **T1 first among the decode tasks** — it carries the one shared plumbing
  change (`ap_arg` gains `ret`). T2, T3, T4 then only add classes and table
  entries and can be done by three people concurrently (they touch disjoint
  helpers; the shape table and `argcls_t` are append-mostly — coordinate the
  enum tail).
- **T5 is fully independent** of everything else (different files).
- **T6, T7, T8 are independent** of each other and of T1–T5 (smoke/fixture
  only) and can run concurrently.
- **T9 last** — it edits the same smoke block and fixture as T2–T4; landing it
  after them avoids three-way churn in `argdecode_victim.c`.

Critical path: T1 → (T2 | T3 | T4) → T9. Everything else is parallel.

```
T1 ─┬─ T2 ─┐
    ├─ T3 ─┼─ T9
    └─ T4 ─┘
T5          (independent)
T6, T7, T8  (independent)
```

## Constraints & gates

- **No new dependencies.** Every header used (`<netinet/in.h>`, `<sys/un.h>`,
  `<arpa/inet.h>`, `<sys/ioctl.h>`, `<linux/futex.h>`, `<linux/stat.h>`) ships
  with glibc/linux-libc-dev already present in `Dockerfile.cli`'s toolchain.
  If a build surprises you, the CLAUDE.md rule applies: add the package to
  `Dockerfile.cli` pinned, never self-skip.
- **AMD IBS is a real hardware gate** for LIVE mode-7 population (T5's manual
  verification) — `docker-cli-ibs` on an AMD host runs it; everywhere else the
  view self-skips with a real reason and the pure `test_autoregion` checks
  still run. Record "verified on <host CPU> via make docker-cli-ibs" in the
  T5 commit message; if no AMD host is available, record that the pure checks
  and the non-AMD degradation path were verified and the live drill-in was
  not.
- **The ncurses TUI is pty-driven and not CI-drivable** (the plan's documented
  pattern for every landed TUI row) — T5's interactive wiring is manually
  verified; its decision logic is unit-tested everywhere.
- **i386 tracees are out of scope by design** — refused at attach with
  `ASMSPY_ETRACEE_I386` before any decode runs, so host-layout struct reads
  (T1, T4) are safe.
- **execve/execveat argv/envp stay undecoded** (`asmspy_engine.c:224-228`) —
  formatting happens at syscall EXIT, when a successful exec has destroyed the
  strings. Do not "fix" this in passing.
- Run `make fmt` before committing (clang-format, CI-gated via `fmt-check`).

## Research notes (verified 2026-07-17)

Kernel facts are from the GitHub mirror of Linux **v6.12**
(git.kernel.org's plain-text views were Anubis-blocked); post-6.12 additions
are not covered. Struct sizes below were computed from the quoted layouts, not
independently compiled — which is why the tasks read structs via the host
headers rather than hardcoded offsets.

- **sockaddr family field**: userspace-visible `struct sockaddr` starts with a
  2-byte `sa_family_t` and keeps plain `char sa_data[14]`; AF_* values:
  `AF_UNSPEC` 0, `AF_UNIX` 1, `AF_INET` 2, `AF_INET6` 10, `AF_NETLINK` 16 —
  <https://raw.githubusercontent.com/torvalds/linux/v6.12/include/linux/socket.h>
  (kernel-internal header; the AF_* values are identical userspace-visible).
- **sockaddr_in** (16 bytes): `sin_family`, `sin_port` (network byte order),
  `sin_addr`, zero padding —
  <https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/linux/in.h>.
- **sockaddr_in6** (28 bytes): family, port, `sin6_flowinfo` (4),
  `sin6_addr` (16), `sin6_scope_id` (4) —
  <https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/linux/in6.h>.
- **sockaddr_un** (110 bytes): family + `sun_path[108]`; an abstract socket's
  path begins with a NUL byte —
  <https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/linux/un.h>.
- **ioctl request encoding**: `_IOC_NRBITS` 8, `_IOC_TYPEBITS` 8,
  `_IOC_SIZEBITS` 14, `_IOC_DIRBITS` 2; nr = bits 0–7, type = 8–15,
  size = 16–29, dir = 30–31; `_IOC_NONE` 0, `_IOC_WRITE` 1, `_IOC_READ` 2 —
  <https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/asm-generic/ioctl.h>.
  (The named-request VALUES — TCGETS etc. — are deliberately NOT hardcoded;
  T2 takes them from the host's `<sys/ioctl.h>` macros.)
- **fcntl commands**: `F_DUPFD` 0 … `F_GETOWN` 9, `F_SETSIG` 10,
  `F_GETSIG` 11 in
  <https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/asm-generic/fcntl.h>;
  the 1024-range Linux-specific ones (`F_SETLEASE` 1024, `F_GETLEASE` 1025,
  `F_NOTIFY` 1026, `F_DUPFD_CLOEXEC` 1030, `F_SETPIPE_SZ` 1031,
  `F_GETPIPE_SZ` 1032, `F_ADD_SEALS` 1033, `F_GET_SEALS` 1034) in
  <https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/linux/fcntl.h>.
- **futex**: ops `FUTEX_WAIT` 0 through `FUTEX_CMP_REQUEUE_PI` 12 and
  `FUTEX_LOCK_PI2` 13; `FUTEX_PRIVATE_FLAG` 128, `FUTEX_CLOCK_REALTIME` 256;
  the wire value is `op | flags` —
  <https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/linux/futex.h>.
  strace's decoder masks the two flag bits off before naming the op and
  renders the private form as `FUTEX_*_PRIVATE` —
  <https://raw.githubusercontent.com/strace/strace/master/src/futex.c>
  (strace master, unversioned — decode style precedent only).
- **struct stat, x86-64** (144 bytes): `st_dev`, `st_ino`, `st_nlink` (8 bytes
  each) precede `st_mode` (offset 24, 4 bytes); `st_size` at offset 48 —
  <https://raw.githubusercontent.com/torvalds/linux/v6.12/arch/x86/include/uapi/asm/stat.h>
  (the generic layout differs — offset facts are x86-64-only, another reason
  T4 reads via the host struct:
  <https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/asm-generic/stat.h>).
- **struct statx** (256 bytes): `stx_mask` u32 first; `stx_mode` u16 at offset
  28; `stx_ino`/`stx_size` u64 at 32/40; mask bits `STATX_TYPE` 0x1,
  `STATX_MODE` 0x2, `STATX_SIZE` 0x200, `STATX_BASIC_STATS` 0x7ff —
  <https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/linux/stat.h>.

## Out of scope

- **AArch64/ARM asmspy support** — the engines are x86-64-hardcoded and gated;
  owned by [asmspy-aarch64-support.md](asmspy-aarch64-support.md).
- **The `--sample`/`--auto` lanes themselves** (`docker-cli-ibs`, the sw-clock
  sampler, `--dataflow --auto`) — all landed; this doc only builds on them.
- **`execve` argv/envp decoding** — deliberately excluded (exit-time
  formatting; the address space is gone), per the plan and the code comment.
- **Naming a pipe's peer endpoint** — requires scanning every process's fds;
  rejected in the landed fd→endpoint work, stays rejected.
- **The remaining ~700 undescribed syscalls** — they keep the honest
  three-words-plus-`...` default; curating more shapes is future Theme E work,
  not this doc's.
- **IBS reason-string consumers outside asmspy** (`examples/ibs_probe.c`,
  `make ibs-test` vacuous-green paths) — an AMD-tier concern owned by
  [amd-ibs-backend-honesty.md](amd-ibs-backend-honesty.md).
- **The ptrace block-step tracer and dataflow producer internals** — sibling
  territory: [ptrace-blockstep-tracer-correctness.md](ptrace-blockstep-tracer-correctness.md),
  [dataflow-producer-correctness.md](dataflow-producer-correctness.md).
