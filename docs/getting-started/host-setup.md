# Host setup for tracing

Out-of-band tracing needs permissions a hardened desktop Linux does not grant
by default. This page is a ladder, not a single setting: each rung buys more
capability for a larger cost, and the rungs are ordered by that cost, cheapest
first. Climb only as far as the tier your target actually needs — the first
rung costs nothing, and a surprising amount of what you'd reach for tracing on
a stock Ubuntu desktop is already reachable without touching a single kernel
setting.

Nothing here is required to *build* asm-test or to run the emulator and
single-step tiers — those need no privilege at all. What follows is about
**attaching to a process you did not launch** (`asmspy --dataflow`, `--trace`,
`--tree`, `--log`, `--stream`, `--graph`, `--watch`, and the desktop GUI's live
capture) and the **PMU-backed hardware tiers** (AMD IBS, Intel PT, AMD LBR).

## The ladder

### Rung 0: nothing

**Buys:** attach to a process that already opted in, on a completely
unmodified host.
**Costs:** nothing — no sysctl, no setcap, no sudo.

Ubuntu's default `kernel.yama.ptrace_scope = 1` refuses `PTRACE_ATTACH` (and
everything asmspy's attach modes need) from a non-descendant, *unless the
target itself opted in* by calling `prctl(PR_SET_PTRACER, ...)`. Browsers do
exactly this from their crash reporter, so most of a running Firefox is
attachable today, on an untouched host, with a named region:

```bash
./build/asmspy --list           # find a content-process pid
./build/asmspy --log <pid> 20   # or --dataflow <pid> <sym>, --trace <pid> <sym>
```

Measured directly on this machine, live, with an ordinary snap-packaged
Firefox running and `kernel.yama.ptrace_scope` at its default of `1`: a
read-only `open("/proc/<pid>/mem", O_RDONLY)` probe — the same check the
kernel runs for `PTRACE_ATTACH`, `PTRACE_MODE_ATTACH_FSCREDS`, and one that
changes no state — against all 13 Firefox-family processes present found
**10 of 13 attachable**. The 3 refusals are exactly the processes that never
call `PR_SET_PTRACER`: the top-level `firefox` process, its `crashhelper`,
and the `forkserver`. Every process spawned *under* the forkserver — Socket,
RDD, Privileged Content, WebExtensions, Utility, both Isolated Web Content
processes, and all three Web Content processes present at measurement time —
was attachable. This is a live count on this box at the time of writing, not
a fixed property of Firefox; the exact number moves with however many
tabs/processes are open, but the *shape* — parent/crashhelper/forkserver
refuse, everything under the forkserver accepts — is structural, not
incidental.

Confirmed further than the probe: `asmspy --log` against one of those
content processes (an `Isolated Web Content` process) streamed its real,
live syscalls and detached cleanly:

```
[8581] restart_syscall(0x713e18c471f4, 0x89, 0x0, ...) = -110
[8581] futex(0x713e18c47198, FUTEX_WAKE_PRIVATE, 1, 0x0, 0x713e29cf8e30, 0) = 0
[8529] restart_syscall(0x713e18c11950, 0x2, 0xffffffff, ...) = 1
[8529] read(fd=43<pipe:[49793]>, 1) = "\xfa" [1]
```

This rung does not reach the browser's own top-level process, and it does
not reach a program that never calls `PR_SET_PTRACER` — most non-browser
targets don't. If that's your situation, the next rung reaches further for
the same zero cost; don't jump straight to loosening `ptrace_scope` (rung 3)
for this.

### Rung 1: `--sampler=ptrace`

**Buys:** `--auto` (pick-the-hot-function, no symbol named) with zero
privilege — no `perf_event_open` call at all.
**Costs:** nothing.

`--auto`'s default picker tries AMD IBS, then a portable software-clock perf
sampler, before falling back to a pure ptrace+`/proc` sampler that confirms
every candidate with a real `int3` arrival before using it. Where
`perf_event_open` is refused outright — `perf_event_paranoid=4`, or a plain
`docker run` with no `--cap-add` — force that fallback directly instead of
walking the chain:

```bash
./build/asmspy --dataflow <pid> --auto --sampler=ptrace
```

Verified green inside a plain `docker run` (no `--cap-add`, no seccomp
change) as part of the plan that shipped this flag, and reproduced live on
this host at `perf_event_paranoid=4`:

```
--auto[ptrace-pc]: entered_often [auto_victim] — 4 entry samples from 1 call
site (of 2 candidates in 400 ms, CONFIRMED by int3 arrival, no perf); up to 2
will be tried
data-flow capture of entered_often @ 0x... (31 bytes) in pid ...
```

This rung is orthogonal to attach permission, not a substitute for it: it
answers "which function?" without perf, but still needs the target to be
attachable at all (rung 0), or a higher rung if it isn't.

### Rung 2: `sudo setcap cap_perfmon+ep <installed asmspy>`

**Buys:** the PMU-backed samplers (AMD IBS, `--sample`, the software-clock
`--auto` fallback) on one binary, without touching any sysctl.
**Costs:** real risk. Read this whole rung before running the command, not
just the command.

```bash
sudo setcap cap_perfmon+ep /path/to/asmspy
```

**Grant `cap_perfmon` only. Never `cap_sys_ptrace`.** The desktop GUI drives
this exact binary from a one-click process picker with no per-target
confirmation gate. A copy holding `cap_sys_ptrace` would let that picker
attach to *any* same-user process unconditionally — turning a convenience
feature into a credential-theft primitive against your own ssh-agent or
password manager. `cap_perfmon` alone grants PMU access; it does not grant
attach rights beyond what Yama already allows.

Three things worth knowing before using this rung:

- **File capabilities live in a `security.capability` xattr on the binary
  file itself, not in the source tree.** `make cli` relinks `build/asmspy`
  and produces a fresh file with no xattr, so **the grant does not survive a
  rebuild** — re-run `setcap` on the binary you actually invoke after every
  rebuild, or it will silently fall back to no PMU access.
- **This rung needs Task 10's `PR_SET_DUMPABLE` guard (already shipped in
  `main()`), or it produces perf access and no codeimage.** A file-capability
  `execve` marks the resulting process non-dumpable; that flips `/proc/self/*`
  to `root:root`, and `codeimage.c`'s `open("/proc/self/clear_refs", ...)`
  then fails `EACCES`. The visible symptom is a capped binary that gets perf
  data but never produces a codeimage — which looks like a disabled kernel
  config (`CONFIG_MEM_SOFT_DIRTY`) even though that config is enabled. The
  shipped guard restores dumpability early in `main()`, before any capture
  runs.
- **`AT_SECURE=1` strips `LD_LIBRARY_PATH`.** A file-capability execve is a
  secure-exec in glibc's sense. A non-standard build of `asmspy` that
  depends on `LD_LIBRARY_PATH` to find a shared library will not find it the
  same way once capped.

**This rung was never verified end-to-end by the work that added the guard
above.** Nobody working on it had `sudo` on the machine involved. The guard
was verified through a test lever that forces the non-dumpable state via a
self-`prctl()` call — that is real, production `main()` code running
end-to-end, and it genuinely goes RED without the guard and GREEN with it —
but it proves the guard's *own logic*, not that a real `setcap`'d `execve`
actually trips the kernel's non-dumpable path the same way. Neither an
unprivileged pre-exec `PR_SET_DUMPABLE(0)` + plain `execve()` nor an
unprivileged `unshare(CLONE_NEWUSER)` reproduced the real trigger when tried
as substitutes, so this remains an open measurement, not a verified one. If
you run this rung, the check worth doing is:

```bash
/path/to/capped-asmspy --list &
stat -c '%U:%G' /proc/$!    # expect your user, not root:root
```

### Rung 3: `kernel.yama.ptrace_scope = 0`

**Buys:** attach to the browser's own top-level process — rung 0 already
reaches everything spawned under it.
**Costs:** more than it buys here. The top-level browser process runs no
page JS, so this rung rarely gets you the process you actually wanted, for
the same host-wide tradeoff as the full drop-in below.

```bash
sudo sysctl kernel.yama.ptrace_scope=0
```

Narrower still than this: run the tracer as root, or grant it
`CAP_SYS_PTRACE` directly (one binary, not the whole host) — but that binary
then carries the exact credential-theft shape rung 2 warns against, so
reserve it for a hand-invoked CLI you control, never a GUI-driven copy. In a
container, `docker run --cap-add=SYS_PTRACE --cap-add=PERFMON` grants both
without touching the host at all.

### Rung 4: the global drop-in

**Buys:** everything on this page at once — every PMU tier, attach to any
same-user process, kernel symbol resolution — set once, persistently.
**Costs:** the largest grant here. Read the security tradeoff below before
running it.

```bash
sudo tee /etc/sysctl.d/90-asmtest-tracing.conf >/dev/null <<'EOF'
# asm-test: host settings for out-of-band tracing.
#
# The 90- prefix matters: this file must sort AFTER /etc/sysctl.d/10-ptrace.conf
# and 10-kernel-hardening.conf, which set the distro defaults it overrides.

# Attach to any process owned by the same user, without that process opting in
# first. Ubuntu ships 1 (descendants only, or a target that called
# PR_SET_PTRACER), which denies every asmspy attach mode against a process you
# did not launch yourself.
kernel.yama.ptrace_scope = 0

# Allow PMU access: AMD IBS (--sample, --auto --sampler=ibs) and Intel PT /
# AMD LBR whole-window capture. Ubuntu's compiled-in default is 4, which blocks
# all of them. No file sets this, so this line is the only place it changes.
kernel.perf_event_paranoid = -1

# Per-user perf ring budget. The 516 KB default leaves an unprivileged Intel PT
# capture with a 128 KiB AUX ring, which truncates on a busy window.
kernel.perf_event_mlock_kb = 2048

# Let perf and the trace decoders resolve kernel symbols instead of reporting
# every kernel address as 0x0.
kernel.kptr_restrict = 0
EOF

sudo sysctl --system
```

`sysctl --system` applies the file immediately — no reboot — and the file
makes it survive reboots.

#### Verify

```bash
sysctl kernel.yama.ptrace_scope kernel.perf_event_paranoid \
       kernel.perf_event_mlock_kb kernel.kptr_restrict
```

Expected afterwards:

```
kernel.yama.ptrace_scope = 0
kernel.perf_event_paranoid = -1
kernel.perf_event_mlock_kb = 2048
kernel.kptr_restrict = 0
```

#### What each one unlocks

| Setting | Typical default | Set to | Unlocks |
|---|---|---|---|
| `kernel.yama.ptrace_scope` | `1` (Ubuntu, via `10-ptrace.conf`) | `0` | attaching to any same-user process; without it, every asmspy attach mode fails against a process you did not launch and that did not opt in (rung 0) |
| `kernel.perf_event_paranoid` | `4` (Ubuntu compiled-in) | `-1` | AMD IBS (`--sample`, `--auto --sampler=ibs`), Intel PT and AMD LBR capture |
| `kernel.perf_event_mlock_kb` | `516` | `2048` | a larger AUX ring, so PT/LBR capture truncates less often on busy windows. Raise further if you still see truncation |
| `kernel.kptr_restrict` | `1` (via `10-kernel-hardening.conf`) | `0` | kernel symbol resolution in traces that cross into the kernel |

#### The security tradeoff, stated plainly

These lower system hardening, and the first one is not cosmetic:

**`ptrace_scope = 0` means any process running as your user can read and write
the memory of any other process running as your user** — including your browser
and anything holding a credential. Yama's default of `1` exists precisely to
stop a compromised process from doing that. Setting `0` is the classic Linux
behaviour and is a reasonable choice on a development machine you control. It is
**not** appropriate on a shared box, a server, or a machine that runs untrusted
code.

`perf_event_paranoid = -1` exposes PMU data, which has been used as a side
channel. `kptr_restrict = 0` reveals kernel addresses, which weakens KASLR
against a local attacker.

If you would rather not take these system-wide, rungs 0-3 above are narrower
and cost nothing globally.

#### Reverting

```bash
sudo rm /etc/sysctl.d/90-asmtest-tracing.conf
sudo sysctl --system
```

## AppArmor and snap confinement are not on this ladder

It is tempting to add "disable AppArmor" or "get out of the snap sandbox" as
a rung here, especially against a snap-packaged Firefox refusing an attach.
**Don't.** It is not what is refusing the attach, and the tree's own
"confined" remedy text (`cli/asmspy_proc.c`'s `asmspy_target_is_confined` and
the message it feeds into `attach_remedy`) has sent people chasing this fix
for the wrong reason before: it names "confined" as the reason *the
launch-from-asmspy alternative* won't work on such a target, not as the
reason attach itself failed. The attach failure is Yama, every time, on this
page — rungs 0, 1 and 3 above.

Two independent facts back this up:

- **AppArmor gets out of the way for an unconfined tracer.** The kernel's
  `security/apparmor/task.c`, in `profile_tracee_perm`, early-returns when
  the *tracer* — not the target — carries no non-trivial AppArmor label,
  which is the ordinary case for asmspy run from an unconfined shell.
- **Snap's own Firefox profile grants ptrace anyway.** Verified live on this
  host: `/var/lib/snapd/apparmor/profiles/snap.firefox.firefox` line 37
  reads `#include <abstractions/base>`, and
  `/etc/apparmor.d/abstractions/base` grants, unconditionally, at lines 132
  and 137:
  ```
  ptrace (readby),
  ptrace (tracedby),
  ```
  A confined Firefox has already told AppArmor tracing it is fine. This
  host's live Firefox reads `snap.firefox.firefox (enforce)` from
  `/proc/<pid>/attr/current` — genuinely confined, in enforce mode — and was
  still 10-of-13 attachable per rung 0's measurement above. Confinement was
  never the gate.

If an attach is refused, read what `asmspy --info` or the GUI's attach
message actually names: Yama's `ptrace_scope`, not AppArmor. Chasing
AppArmor or snap-confinement settings for an attach failure is chasing the
wrong subsystem.

## Two things worth knowing

**These are not namespaced.** `kernel.yama.ptrace_scope` and
`kernel.perf_event_paranoid` are host-wide: a container inherits the host's
values and cannot raise or lower them from inside. That is why a Docker lane
still needs `--cap-add`, and why setting them on the host affects every
container too.

**Do not disable ASLR system-wide.** For reproducible addresses across runs —
comparing two recordings, or regenerating documentation screenshots — use
`setarch -R` on the single process instead:

```bash
setarch -R ./build/scenes_victim
```

`kernel.randomize_va_space = 0` would achieve the same thing for the whole
system and weakens every process on it. There is no reason to pay that when
the per-process flag exists.

## Checking what your hardware actually supports

Permissions are only half of it; some tiers need specific silicon. To see
whether the PMUs are present at all:

```bash
ls /sys/bus/event_source/devices/ | grep -E 'ibs|intel_pt|cs_etm'
```

- `ibs_op` / `ibs_fetch` — AMD IBS is present (needs an AMD host)
- `intel_pt` — Intel PT is present (needs bare-metal Intel; absent on AMD, VMs and most containers)
- `cs_etm` — ARM CoreSight is present (needs a CoreSight-capable AArch64 board)

If a PMU is absent, no permission change will produce it — that is a hardware
gate, and asm-test reports it as one. See the hardware-trace tiers section of
[Troubleshooting](../reference/troubleshooting.md) for how to read a skip reason
and tell a fixable permission problem from a real hardware limit.
