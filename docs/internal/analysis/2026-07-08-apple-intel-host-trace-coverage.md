# Analysis: trace-tier coverage on an Apple Intel host (2026-07-08)

*Status: analysis / findings, empirical. A point-in-time, live-verified answer to
"which of asm-test's trace tiers can actually be exercised on an Apple **Intel**
Mac?" — the host-specific instantiation of the capability tables in
[trace-parity-matrix.md](trace-parity-matrix.md). Every "verified" claim below was
run on the box described; every "impossible" claim was probed, not assumed. Source
of record for the gating logic: [src/hwtrace.c](../../../src/hwtrace.c),
[src/ss_backend.c](../../../src/ss_backend.c), [mk/native-trace.mk](../../../mk/native-trace.mk).*

## The host

| | |
|---|---|
| CPU | Intel Core **i7-8559U** (Coffee Lake, 8th-gen, 4C/8T); `machdep.cpu.leaf7_features` includes **`IPT`** — the silicon has Intel PT |
| OS | **macOS 14.7.5** (build 23H527), `x86_64` (`RELEASE_X86_64`) |
| Docker | Docker Desktop **28.5.1**, LinuxKit kernel **6.10.14-linuxkit**, `x86_64`, under Apple **Virtualization.framework** |
| Toolchain present | `cc`/`clang`, `nasm`, `pkg-config`; `unicorn` + `keystone` + `capstone` all resolve via `pkg-config` |

Two facts about this host determine everything else: it is **Intel, not AMD** (so
AMD LBR can never run here) and **macOS, not Linux** (so the Linux-native tiers do
not run on bare metal — only inside a Linux container/VM). It is, however, the
*exact* hardware the **macOS-Intel single-step** front needs.

## Bottom line

Three tiers, by where the work can happen:

| Tier | Runs here? | What |
|---|---|---|
| **A — native on this box** | ✅ verified | core framework, emulator (Unicorn, 5 guest ISAs), **macOS-Intel single-step hardware trace** (live capture), quality lanes, most bindings |
| **B — Docker Linux (`linux/amd64`, native speed)** | ✅ verified | DynamoRIO native trace, hwtrace single-step + ptrace, full CI matrix, bindings, docs |
| **C — impossible on this box** | ❌ hardware-gated | AMD LBR (no AMD silicon), Intel PT live (VM hides the PMU; macOS has no PT interface), ARM CoreSight / AArch64 stream, RISC-V native tier |

---

## Tier A — native on this hardware (verified)

- **Core** — `make test` (arithmetic/SIMD/struct suites) and `make emu-test`
  (**52 / 52**, incl. AArch64 / RISC-V / ARM32 / Win64 guests) pass. AVX-512 lanes
  self-skip (not on this CPU); everything else runs.
- **macOS-Intel single-step hardware trace** — the one real-CPU native trace backend
  this host runs. `make hwtrace-test` → **87 passed / 0 failed** with *live* capture,
  not merely self-skips:
  - `ok 26 — single-step yields the exact live instruction stream [0,3,6,c,11]`
  - `ok 27 — single-step block partition {0, 0x11} matches PT/AMD/DynamoRIO`
  - `ok 31 — single-step captures all 62 insns of a 20-trip loop (no depth ceiling)`
  - `ok 33 — single-step loop trace complete past LBR's 16-branch window`
  - `ok 34–59 — call_scoped / call_scoped_fp / stitch_handles captured **live**`

  The Linux-only variants (nested/concurrent single-step, ptrace W2, whole-window,
  AMD sample) self-skip cleanly. This confirms the "62-insn loop, live, on this box"
  claim in [scoped-tracing-implementation.md](../../scoped-tracing-implementation.md)
  and resolves the parity-matrix drift (below).
- Also native here: quality lanes (`sanitize`, `tidy`, `fmt-check`), `macos-clean-test`,
  the emulator/Keystone/Capstone tiers, and the language-binding suites that build
  against the host `libunicorn`.

### Machine-specific regression found and fixed (commit `5eb145d`)

`make hwtrace-test` **did not compile on this host** before this session — a
regression from the §D3 out-of-process whole-window work (`a8f4f5e9`, `64223dd`,
2026-07-06→08). Ten ptrace / whole-window test functions in
[examples/test_hwtrace.c](../../../examples/test_hwtrace.c) were guarded
`#if defined(__x86_64__)` — **true on macOS Intel** — so their Linux-only bodies
(`fork`/`ptrace`/`PTRACE_TRACEME`/`waitpid`, whose headers sit behind
`#if defined(__linux__)`) compiled on macOS and failed with 9 undeclared-symbol
errors. The fix aligned all ten to the file's own convention,
`#if defined(__linux__) && defined(__x86_64__)` (29 sibling functions already used
it), routing macOS to the existing `#else` SKIP stubs, plus corrected five stubs
that printed "x86-64 only" on an x86-64 host.

**Why this matters as a finding:** CI cannot catch it — the Linux lanes compile the
guarded bodies fine, and the macOS packaging slot (`darwin-x86_64`, macos-13 nightly
per [trace-parity-matrix.md Matrix 13](trace-parity-matrix.md)) does not run
`hwtrace-test`. Only a build on real Intel-macOS hardware surfaces it. This host is
the natural regression anchor for the macOS-Intel single-step front.

---

## Tier B — Docker Linux lanes (verified)

Docker Desktop's default platform on this Intel Mac is **`linux/amd64` — native, no
QEMU** — so the Linux half of the matrix runs at native speed.

- **DynamoRIO native trace** — `make docker-drtrace` (Ubuntu 24.04 + DynamoRIO
  11.91.20630) is **green**: in-process DR trace **18 passed / 0 failed** (C),
  `drtrace-python` **3 / 3**, and the managed-host gate `drgate` **4 / 4**. This is the
  only vendor/uarch-independent native backend, and macOS cannot run it natively —
  Docker is how you exercise it on this box.
- **hwtrace in Linux** — `make docker-hwtrace` runs the single-step + ptrace tiers
  live; the PT / AMD / CoreSight decoders compile in and self-skip.
- Also here: `docker-ci` (full x86-64 matrix), `docker-bindings` / `docker-<lang>`,
  `docker-docs`, `codeimage`.

What self-skips even in Docker: **`docker-hwtrace-amd`** (no AMD silicon under the VM)
and **Intel PT** (see below).

---

## Tier C — impossible on this box, and why

### AMD LBR
No AMD silicon exists to expose — not natively, not in the VM. All AMD-LBR work
(the recent Zen-focused commits) can only be validated on a real Zen 3+ host.

### Intel PT — the interesting "no"

The CPU has PT (`IPT` in `leaf7_features`), yet it cannot be exercised here:

| Probe | Result |
|---|---|
| Docker container, unprivileged | `intel_pt` flag in `/proc/cpuinfo`: **0** · PMUs: `breakpoint kprobe msr software tracepoint uprobe` · `intel_pt` PMU: **ABSENT** |
| Docker container, **`--privileged`** | `intel_pt` flag: **0** · `intel_pt` PMU: **still ABSENT** |
| macOS native | no `perf_event_open`, no public PT interface at all |

The `--privileged` row is decisive: it rules out `perf_event_paranoid` / caps /
seccomp. The block is **below the container** — Docker Desktop's LinuxKit VM runs
under Apple's Virtualization.framework, which virtualizes **no PMU** (note the guest
gets no `cpu` core PMU either), so the PT CPUID leaf is masked before the guest
kernel sees it and no `intel_pt` PMU is registered. asmtest's PT backend therefore
returns `available()==0` ("no intel_pt PMU") and self-skips — nothing to init. This
is exactly why the matrix marks PT **`✓ bare-metal`** only. No macOS-hosted VM helps
(Fusion/Parallels/UTM would hit the same Hypervisor.framework wall).

**Ways to actually exercise this box's PT silicon — all require leaving macOS:**

- **Bare-metal Linux** (an Ubuntu **live USB** is the zero-commitment option): then
  `ls /sys/bus/event_source/devices/intel_pt` exists, and `perf record -e intel_pt// --
  <prog>`, **magic-trace** (Jane Street), **ptxed/ptdump** (Intel libipt),
  `gdb` `record btrace pt`, and **honggfuzz** IPT mode all work. asm-test's own PT lane
  (`make hwtrace-test`) would light up there too.
- **Boot Camp Windows 10** (this 2018 MBP supports it): **winipt** (Alex Ionescu's
  `ipt.sys` interface) and **WinAFL** Intel-PT mode.

### macOS-native Intel PT via a kext — "research-grade", quantified

Programming `IA32_RTIT_CTL` (MSR `0x570`) from a kext is *possible in principle* but
no working macOS PT capture exists, for concrete reasons:

- **The primitives exist.** `wrmsr` + physical-memory mapping from kernel space is
  routine on Intel Macs: **DirectHW.kext** (coreboot/flashrom) exposes userspace
  `rdmsr`/`wrmsr` + physmem; **AppleIntelInfo.kext** (Piker-Alpha) and **msr.kext**
  (relan) read `IA32_*` MSRs. All require **SIP disabled**.
- **PT is far more than one MSR.** You must allocate a physically-contiguous buffer,
  build a **ToPA** (Table of Physical Addresses) in `IA32_RTIT_OUTPUT_BASE` /
  `_MASK_PTRS`, program `IA32_RTIT_CTL` (TraceEn + filters), field the **ToPA-full
  PMI**, and handle context switches — XNU does **not** save/restore the RTIT MSRs
  across a thread switch, so without hooking the scheduler you trace whatever runs on
  the core. You would be writing the first macOS PT driver — the part Linux's
  `intel_pt` PMU and Windows' `ipt.sys` already do.
- **Modern macOS fights it.** Apple's sanctioned replacement, **DriverKit** (`.dext`),
  runs in userspace and **cannot execute `wrmsr`** — so the only route is a **legacy
  KEXT**, deprecated since Catalina and refused since Big Sur unless booted in
  **Reduced Security** + SIP off.
- **Nobody walks it.** The PT ecosystem is Linux/Windows/hypervisor (kAFL, PTfuzz,
  PTrix, Trail of Bits' honeybee, WinIPT/WinAFL); searching the GitHub
  `processor-trace` topic and PT-on-macOS surfaces **no macOS project**. The tell is
  **Pishi** (POC2024, macOS *kernel* coverage fuzzing): it **skipped hardware tracing**
  for static binary rewriting — Intel PT is irrelevant on Apple Silicon, CoreSight is
  undocumented there, and Intel Macs are EOL, so there is no incentive to build it.

Verdict: days-to-weeks of from-scratch kernel work behind a SIP-off Reduced-Security
legacy kext on EOL hardware, with no code to fork — to reproduce what a Linux live USB
gives in ten minutes. Not worth it.

Sources: [Trail of Bits — honeybee + Intel PT](https://blog.trailofbits.com/2021/03/19/un-bee-lievable-performance-fast-coverage-guided-fuzzing-with-honeybee-and-intel-processor-trace/) ·
[Pishi (POC2024)](https://r00tkitsmm.github.io/fuzzing/2024/11/08/Pishi.html) ·
[DirectHW](https://github.com/flashrom/directhw/blob/master/macosx/DirectHW/DirectHW.c) ·
[AppleIntelInfo.kext](https://pikeralpha.wordpress.com/2016/09/14/appleintelinfo-kext-v1-7/) ·
[msr.kext](https://github.com/relan/msr.kext) ·
[Intel SDM Vol.3 — IA32_RTIT_CTL / ToPA](https://xem.github.io/minix86/manual/intel-x86-and-64-manual-vol3/o_fe12b1e2a880e0ce-1700.html) ·
[Apple — kexts deprecated / Reduced Security](https://support.apple.com/guide/security/securely-extending-the-kernel-sec8e454101b/web) ·
[Apple — System Extensions & DriverKit](https://support.apple.com/guide/deployment/system-extensions-in-macos-depa5fb8376f/web) ·
[Intel PT virtualization (KVM) — LWN](https://lwn.net/Articles/737839/)

---

## Doc reconciliation done this session

[trace-parity-matrix.md](trace-parity-matrix.md) contradicted itself on the
single-step Phase-5 fronts: Matrix 1 said macOS-Intel/Windows were *implemented*
while the narrative, Matrix 2, and Matrix 9 still said *"planned."* Verified shipped
here (87/0, live capture) and reconciled Matrices 2 & 9 + the narrative to match.

## One-line synthesis

On an Apple **Intel** host the trace story is: **native** = core + emulator +
macOS-Intel single-step (live); **Docker** = the full Linux native cascade minus the
two hardware backends (DynamoRIO runs, PT/AMD self-skip); **impossible** = AMD LBR
(wrong vendor) and Intel PT live (the VM hides the PMU and macOS has no PT interface;
a native macOS PT kext is real-but-uneconomical research). To see this box's PT
silicon actually trace, boot bare-metal Linux — not a VM.
