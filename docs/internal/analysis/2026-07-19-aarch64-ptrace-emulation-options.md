# Running AArch64 with a *working* ptrace on an x86 host — options analysis

> **Status: research record, no shipping code.** This note answers a single
> question raised while reviewing the AArch64 validation story: on this x86-64
> AMD Zen 2 host (no arm64 silicon), what would it take to run the out-of-process
> **ptrace single-step** tier for real, rather than have it self-skip? It ships
> **no source, Makefile, or Dockerfile change**. The verification that replaces a
> test is: this note cites the primary sources listed at the end and is reviewed
> against them. It is the background for
> [aarch64-ptrace-single-step-validation.md](../implementations/aarch64-ptrace-single-step-validation.md)
> and [self-hosted-ci-runners.md](../implementations/self-hosted-ci-runners.md),
> and it explains *why the tree already chose native arm64 CI over emulation*.

## The one fact that decides everything

`ptrace(2)` — the `PTRACE_TRACEME`/attach relationship, the stop/`waitpid`
handshake, and `PTRACE_SINGLESTEP` — is a **kernel service backed by CPU debug
hardware**, not a userspace facility. On AArch64 the single step is driven by the
architectural **software-step state machine** (`MDSCR_EL1.SS` + `PSTATE.SS`): the
kernel arms it, the CPU retires exactly one instruction and traps, the kernel
turns that into a `SIGTRAP` ptrace-stop. So any option that gives a *working*
ptrace must supply a **real (or faithfully emulated) arm64 kernel + CPU debug
architecture**. Emulating only the *instruction set* is not enough.

That single fact splits the options cleanly:

- **ISA-only emulation → ptrace cannot work.** `qemu-user` (linux-user / binfmt /
  TCG), and therefore Docker `buildx` multi-arch and Lima "Fast mode", all fall
  here. This is what `probe_singlestep()` in
  [src/ptrace_backend.c](../../../src/ptrace_backend.c) detects and self-skips on.
- **Real or full-system-emulated kernel → ptrace works.** `qemu-system-aarch64`
  (full-system TCG) and native arm64 silicon fall here.

## 1. qemu-user is a dead end — by design, not a missing feature *(high confidence)*

QEMU's own user-mode docs state plainly that *"the user space emulator hasn't
implemented ptrace"*, and direct users to the built-in gdb stub instead.
Maintainer Paul Brook (qemu-devel, 2008): it is unimplemented *"because it's
extremely hard (read: nearly impossible) to implement properly"* — passing the
guest's ptrace through to the host kernel would expose QEMU's **own x86 state**,
not the emulated **arm64** state, so the entire tracer/tracee relationship would
have to be re-emulated across an IPC channel. Real-world confirmation: gdb under
`qemu-user-static` fails with `warning: ptrace: Function not implemented`
(ENOSYS). A `do_ptrace` stub exists but implements no functional stops or
single-step; TCG also cannot offer user-mode watchpoints because it does not
track every memory access. **No recent QEMU release has changed this.** Docker
buildx/multiarch and Lima "Fast mode" are this same mechanism and inherit the
limitation — do not treat "we already run arm64 containers here" as a path to the
ptrace tier.

This is exactly the behaviour the backend already encodes: `qemu-user emulates
none` of the arm64 breakpoint slots (`set_hw_bp`), and `probe_singlestep()` uses
a `WNOHANG` deadline precisely because a blocking `waitpid` for a
`PTRACE_TRACEME` child **never returns** under qemu-user.

## 2. qemu-system-aarch64 (full-system TCG) — works, with caveats *(high confidence)*

This is the *only* emulation path that makes the ptrace tier run for real. Since
**August 2014** (Peter Maydell's `target-arm: Implement ARMv8 debug single-step`
series) QEMU's `target-arm` implements the three-state software-step machine
(Inactive; Active-not-pending, `PSTATE.SS=1`; Active-pending, `PSTATE.SS` clear).
The cover letter states its purpose verbatim:

> "This is necessary to support running gdb or gdbserver inside a Linux guest,
> because Linux assumes the presence of this (mandatory) architectural feature
> and uses it to implement PTRACE_SINGLESTEP for 64-bit debuggees."

That state machine lives in **full-system emulation only** — linux-user has no
guest kernel to arm it. A genuine arm64 Ubuntu kernel boots under `-M virt -cpu
cortex-a57` (pure TCG software on an x86 host; KVM is available only when the host
is *itself* aarch64). It is CI-drivable headless: `-nographic` for the console
plus a `cloud-localds` NoCloud seed disk to inject SSH keys, then ssh in and run
`make hwtrace-test`.

**Caveats that bear directly on our tiers:**

- The 2014 series shipped **software single-step only**. Maydell: *"I have
  breakpoint and watchpoint support next on my todo list … this is sufficient to
  get a functional gdb, because gdb defaults to software breakpoints."* Hardware
  breakpoint/watchpoint — the prerequisite for guest **`NT_ARM_HW_BREAK`**, i.e.
  our T2 arm-and-fire path — came later and has weaker fidelity than real silicon.
- Documented single-step **correctness bugs** have existed: Launchpad #1838913
  (2019, single-step exceptions incorrectly routed to EL1) and a 2022 qemu-devel
  *"Possible bug in Aarch64 single-stepping"* thread. Not re-verified against the
  current release; treat emulated single-step as *plausible but not blessed*.
- **Slow** — pure TCG, no KVM acceleration possible on x86. The actual slowdown
  factor for a real `PTRACE_SINGLESTEP` suite on this box is unmeasured (see open
  questions).

## 3. Native arm64 silicon — the low-friction route the tree already uses *(high confidence)*

A real arm64 kernel + CPU is where ptrace and single-step just work, no probe
needed. This is precisely what the existing `hwtrace-arm64` CI job targets.

- **GitHub-hosted runners**: `ubuntu-24.04-arm` / `ubuntu-22.04-arm` labels.
  **Free for public repos** (public preview 2025-01-16, GA 2025-08-07; 4-vCPU
  Cobalt-100 machines, image managed by Arm). **Available in private repos since
  2026-01-29** at **$0.005/min** for the Linux 2-core tier (scaling 2→64-core,
  $0.005–$0.098/min).
- ⚠️ Correction verified during this research: these runners are native-**ISA**
  but still **virtualized VMs** — an over-strong "native, no virtualization"
  claim was refuted 3-0. Irrelevant to ptrace: a real guest kernel is what makes
  it work, and they have one.
- **Cloud / physical alternatives** (self-hosted-runner candidates): AWS Graviton,
  Azure Cobalt, Oracle **Ampere A1 Always Free** (⚠️ **halved to 2 OCPU / 12 GB**
  effective 2026-06-15), Hetzner/Scaleway Ampere; Raspberry Pi 4/5, Ampere
  workstations, Apple Silicon (Asahi or macOS). All give a genuine arm64 kernel
  where ptrace works.

## 4. CoreSight ETM hardware trace — real-silicon-only *(medium confidence)*

Full-system qemu does **not** rescue the CoreSight tier. QEMU's `virt` machine
documents its full device set (PCIe/CXL, PL011, PL061, SMMUv3, GICv2m/ITS, SBSA
watchdog, virtio-mmio, …) with **no mention of CoreSight, ETM, or ETB**, and
models no trace macrocell. This is absence-of-evidence rather than proof of
impossibility, but it means ETM capture/decode requires **real arm64 silicon with
CoreSight** — the self-hosted lane in
[coresight-live-decode.md](../implementations/coresight-live-decode.md), never
emulation.

## What this means for the tree

| Tier | qemu-user (today) | qemu-system TCG | Native arm64 |
| --- | --- | --- | --- |
| out-of-process ptrace single-step | ❌ self-skips | ✅ software-step works (slow) | ✅ `hwtrace-arm64` job |
| `NT_ARM_HW_BREAK` (T2 arm-and-fire) | ❌ 0 slots | ⚠️ late/weaker fidelity | ✅ |
| CoreSight ETM | ❌ | ❌ not modeled | ✅ real silicon only |

The repo already picked the right **primary** path: native `ubuntu-24.04-arm`,
free/cheap and already green, guarded by the anti-vacuity assert in
[.github/workflows/ci.yml](../../../.github/workflows/ci.yml) so a qemu-style
self-skip is a red build. The *only* thing a `qemu-system-aarch64` lane would add
is a **local, no-hardware** way to run the ptrace single-step tier for real on
this Zen box — useful for iterating without waiting on CI, but slow, still weak on
hardware-breakpoint fidelity, and no help at all to CoreSight. Given the hosted
runners already cover it, a full-system qemu lane is **marginal** and is not
recommended as more than a developer convenience unless the open questions below
resolve favourably.

## Open questions (before betting on the emulated path)

- What is the measured pure-TCG slowdown of `qemu-system-aarch64` single-stepping
  a debuggee on this host — is a realistic `PTRACE_SINGLESTEP` suite tolerable in
  CI wall-clock?
- Are the 2019/2022 single-step correctness bugs fixed in the current QEMU
  release, and does in-guest `PTRACE_SINGLESTEP` pass the kernel's own arm64
  ptrace/software-step selftests under TCG?
- How complete is current `NT_ARM_HW_BREAK` (hardware breakpoint/watchpoint)
  emulation in full-system mode versus the software-step-only 2014 baseline — can
  guest ptrace hardware watchpoints be tested under emulation at all?
- Is there any way to *synthesize* CoreSight ETMv4 packet streams for decoder
  testing (a replay fixture) short of real silicon, given `virt` models no trace
  macrocell?

## Caveats on this record

Cloud pricing and free tiers move fast — the Oracle Ampere A1 halving (effective
2026-06-15) and GitHub arm64 private-repo pricing ($0.005/min, 2-core) are current
as of mid-2026 and will drift; re-verify before relying. The
ptrace-impossibility and single-step-purpose findings rest on strong primary
sources (QEMU docs + the ARM maintainer on qemu-devel); the full-system
boot/cloud-init mechanics are corroborated by official Ubuntu/kernel.org/cloud-init
docs but partly lean on a community gist; the CoreSight-ETM finding is an
absence-of-evidence inference from one primary doc (hence *medium* confidence).

## Primary sources (verified 2026-07-19)

- qemu-user has no ptrace — [QEMU user-mode docs](https://www.qemu.org/docs/master/user/main.html);
  Paul Brook, [qemu-devel 2008-09](https://lists.nongnu.org/archive/html/qemu-devel/2008-09/msg01141.html);
  [multiarch/qemu-user-static#165](https://github.com/multiarch/qemu-user-static/issues/165)
  (`ptrace: Function not implemented`).
- ARMv8 debug single-step in full-system QEMU — Peter Maydell,
  [`[PATCH 00/11] target-arm: Implement ARMv8 debug single-step`, qemu-devel 2014-08](https://lists.gnu.org/archive/html/qemu-devel/2014-08/msg01291.html);
  [QEMU gdb / debug docs](https://qemu-project.gitlab.io/qemu/system/gdb.html).
- Single-step correctness bugs — [Launchpad #1838913](https://bugs.launchpad.net/qemu/+bug/1838913) (2019).
- Full-system boot + headless CI mechanics —
  [kernel.org arm64-under-QEMU HOWTO (Will Deacon)](https://www.kernel.org/pub/linux/kernel/people/will/docs/qemu/qemu-arm64-howto.html);
  [cloud-init QEMU tutorial](https://docs.cloud-init.io/en/latest/tutorial/qemu.html).
- GitHub-hosted arm64 runners —
  [GA for public repos, 2025-08-07](https://github.blog/changelog/2025-08-07-arm64-hosted-runners-for-public-repositories-are-now-generally-available/);
  [private-repo availability, 2026-01-29](https://github.blog/changelog/2026-01-29-arm64-standard-runners-are-now-available-in-private-repositories/);
  [Actions runner pricing](https://docs.github.com/en/billing/reference/actions-runner-pricing).
- Oracle Ampere A1 free-tier halving (2026-06-15) — [InfoQ](https://www.infoq.com/news/2026/07/oracle-cloud-free-tier-limits/).
- CoreSight not modelled by `virt` — [QEMU `virt.rst`](https://github.com/qemu/qemu/blob/master/docs/system/arm/virt.rst).
- Lima foreign-arch full-system vs Fast mode — [Lima multi-arch docs](https://lima-vm.io/docs/config/multi-arch/).
