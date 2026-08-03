# Host setup for tracing

Out-of-band tracing needs permissions a hardened desktop Linux does not grant by
default. This page sets them once, persistently, and says what each one buys and
what it costs.

Nothing here is required to *build* asm-test or to run the emulator and
single-step tiers — those need no privilege at all. These settings unlock
**attaching to a process you did not launch** (`asmspy --dataflow`, `--trace`,
`--tree`, `--log`, `--stream`, `--graph`, `--watch`, and the desktop GUI's live
capture) and the **PMU-backed hardware tiers** (AMD IBS, Intel PT, AMD LBR).

## The settings

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

`sysctl --system` applies the file immediately — no reboot — and the file makes
it survive reboots.

### Verify

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

### What each one unlocks

| Setting | Typical default | Set to | Unlocks |
|---|---|---|---|
| `kernel.yama.ptrace_scope` | `1` (Ubuntu, via `10-ptrace.conf`) | `0` | attaching to any same-user process; without it every asmspy attach mode fails against a process you did not launch |
| `kernel.perf_event_paranoid` | `4` (Ubuntu compiled-in) | `-1` | AMD IBS (`--sample`, `--auto --sampler=ibs`), Intel PT and AMD LBR capture |
| `kernel.perf_event_mlock_kb` | `516` | `2048` | a larger AUX ring, so PT/LBR capture truncates less often on busy windows. Raise further if you still see truncation |
| `kernel.kptr_restrict` | `1` (via `10-kernel-hardening.conf`) | `0` | kernel symbol resolution in traces that cross into the kernel |

## The security tradeoff, stated plainly

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

If you would rather not take these system-wide, see the per-process alternatives
below — they are narrower and cost nothing globally.

### Reverting

```bash
sudo rm /etc/sysctl.d/90-asmtest-tracing.conf
sudo sysctl --system
```

## Narrower alternatives

Prefer these if you do not want to relax the host:

| Instead of | Do this | Scope |
|---|---|---|
| `ptrace_scope = 0` | have the target call `prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, …)` | one process, opted in by itself. Every victim program in `cli/` does this, which is why they stay attachable on an unmodified host |
| `ptrace_scope = 0` | run the tracer as root, or grant `CAP_SYS_PTRACE` | one tracer binary |
| `perf_event_paranoid = -1` | `sudo setcap cap_perfmon+ep <binary>` | one binary |
| either, in a container | `docker run --cap-add=SYS_PTRACE --cap-add=PERFMON` | one container |

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
system and weakens every process on it. There is no reason to pay that when the
per-process flag exists.

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
