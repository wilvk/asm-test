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

Verdict summary: no working macOS PT capture exists because a from-scratch kernel
driver is required — Windows and Linux ship or have theirs; macOS ships none. But the
barrier is *smaller* than "write a full PT driver" once scoped to asm-test's use case,
as the CPUID evidence below shows. The structure such a driver would take, and the
concrete paths, follow.

---

## Deep dive: the structure a macOS Intel-PT capture would take

PT is two halves: **capture** (program the CPU, collect the raw packet stream) and
**decode** (replay packets into control flow). asm-test already has the decode half,
OS-independently — [src/pt_backend.c](../../../src/pt_backend.c) over **libipt**. Only
the **capture** half is macOS-missing. So the question is narrow: what would a macOS
capture driver look like, and how much of it is actually hard?

### The three reference structures

| Project | OS | Approach | What it teaches |
|---|---|---|---|
| **simple-pt** (Andi Kleen) | Linux | from-scratch minimal driver | the anatomy — a kernel module owns per-CPU trace buffers (`pt_buffer_order`, up to 511 ToPA entries), programs the RTIT MSRs, fields the buffer-full PMI, exposes config via module params; user tools `sptcmd`/`sptdump` collect, `sptdecode` reconstructs via libipt |
| **WindowsIntelPT** (Allievi) | Windows | from-scratch driver | the closest analog to a macOS kext — a hand-rolled `WindowsPtDriver` that programs the same MSRs + ToPA + PMI on an OS with no `perf` infrastructure, per-process/per-core |
| **winipt** (Ionescu) | Windows | *wrap the OS driver* | the easy path macOS **cannot** take: since Win10 1809 Windows **ships** `ipt.sys`, so winipt is just a user-mode IOCTL wrapper (`libipt`/`libiptnt`) + `ipttool`. macOS ships no PT driver → nothing to wrap |
| **DirectHW.kext** (coreboot/flashrom) | macOS | privileged primitive | the only macOS building block — an IOKit user client giving userspace `rdmsr`/`wrmsr` + physical-memory mapping. Supplies the MSR/physmem access, none of the PT orchestration |

The pattern is clear: **Windows is easy** because the OS ships the driver (`winipt`) or
someone wrote one (`WindowsIntelPT`); **Linux is easy** because `perf`'s `intel_pt` PMU
*is* the driver (simple-pt is the minimal alternative). **macOS has neither** — you must
supply the driver, and DirectHW is the only primitive to build it on.

### What *this box's* silicon makes optional (CPUID.14H, measured on the i7-8559U)

```
Intel PT present ........ yes  (CPUID.07H.EBX[25])
Single-Range Output ..... yes  (CPUID.14H.0:ECX[2])    -> no ToPA table, no PMI
ToPA / ToPA multi ....... yes  (ECX[0] / ECX[1])
IP Filtering / TraceStop  yes  (EBX[2]); num ranges = 2 -> trace ONLY the routine
CR3 filtering ........... yes  (EBX[0])
```

Two of these collapse the hardest parts of a general PT driver:

- **Single-Range Output** (`IA32_RTIT_CTL.ToPA = 0`) sends packets to one **contiguous
  physical buffer** (`IA32_RTIT_OUTPUT_BASE` + `_MASK_PTRS`) — **no ToPA table and no
  buffer-full PMI**. For a short routine a few-MB buffer never wraps, so you poll
  `IA32_RTIT_STATUS` / the output offset instead of servicing an interrupt. That removes
  the PMI — the piece most likely to collide with XNU's `kpc`/`kperf` PMC subsystem
  (which owns the perfmon LVT).
- **IP filtering** (2 ranges) lets you set `IA32_RTIT_ADDR0_A/B` to bracket exactly the
  routine's `[entry, end)` so packets are generated **only in-range**. That sidesteps the
  context-switch problem entirely: even if the thread is descheduled mid-window, nothing
  outside the routine is traced — no need to hook the scheduler to save/restore RTIT state.

What genuinely remains OS-level: allocate the contiguous physical buffer (DirectHW can
map it), arm/disarm the MSRs around the call, keep the thread on one CPU for the window,
and read the buffer out. That is a **capture window**, not a full tracing subsystem.

## Ways forward

Ordered by effort. All require **SIP disabled** (and, to load a kext, **Reduced
Security**), and target Intel Macs only (Apple Silicon has no PT).

**Reuse that already exists.** The PT **decode** path
([src/pt_backend.c](../../../src/pt_backend.c) replaying packets into an
`asmtest_trace_t` via libipt) is OS-independent and in-tree. Anything that produces a raw
PT buffer on macOS feeds it (or `ptxed` offline). You build the capture half only.

**Path A — userspace feasibility spike (DirectHW, no custom kext).** *~Days.* Load
DirectHW.kext; from userspace map a contiguous physical buffer and `wrmsr` the RTIT MSRs:
`IA32_RTIT_OUTPUT_BASE` = buffer, `ToPA = 0` (single-range), `IA32_RTIT_ADDR0_A/B` = the
routine's `[entry, end)`, then `IA32_RTIT_CTL.TraceEn = 1`; pin the caller thread, invoke
the routine, clear `TraceEn`, read the buffer, decode with the in-tree libipt path. Skips
ToPA, PMI, and context-switch handling (single-range + IP-filter, both confirmed on this
CPU). Proves the silicon and decode end-to-end — a real answer to "does PT work here,"
not a product. Risk to validate: DirectHW's per-core `wrmsr` targeting and coexistence
with XNU's PMC use — test on an otherwise-idle core.

**Path B — a real XNU capture kext (simple-pt / WindowsIntelPT-shaped).** *~Weeks.* An
IOKit driver with a user client exposing `begin(entry, end)` / `end(&trace)` that mirrors
[include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h), so the existing PT
backend's `available()`/`begin`/`end` seam lights up on macOS exactly as on Linux.
Per-window: `thread_bind` to a CPU + an interrupt fence, single-range output (or
ToPA+PMI for unbounded capture — then you must claim the PMC LVT the way `kpc` does), arm
the RTIT MSRs, run, disarm, hand the buffer to the in-tree decoder. This is "write the
driver macOS doesn't ship," scoped down by the CPUID simplifications above.

**Path C — don't build it (recommended).** This box **already** traces natively via the
**single-step** backend — exact instruction stream, live, 87/0. PT's only edge over
single-step is capture *overhead* (near-zero vs ~2.3 µs/insn), irrelevant for the short,
deterministic routines asm-test targets. Under the framework's own fidelity model a
native→native swap (single-step ↔ PT) is transparent *as data*, so single-step is the
correct macOS-Intel answer and PT capture is a research curiosity here. Spend the effort
on the Linux path (`perf -e intel_pt//`), where the driver already exists, and keep
single-step as the shipped macOS-Intel tier.

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

## Sources

**PT capture implementations (structure):**
[simple-pt (Andi Kleen) — minimal Linux PT driver + libipt decode](https://github.com/andikleen/simple-pt) ·
[WindowsIntelPT (Allievi) — from-scratch Windows PT driver](https://github.com/intelpt/WindowsIntelPT) ·
[winipt (Ionescu) — user-mode wrapper over Windows' inbox ipt.sys](https://github.com/ionescu007/winipt) ·
[DirectHW — macOS userspace rdmsr/wrmsr + physmem kext](https://github.com/flashrom/directhw/blob/master/macosx/DirectHW/DirectHW.c) ·
[AppleIntelInfo.kext — reading IA32 MSRs on macOS](https://pikeralpha.wordpress.com/2016/09/14/appleintelinfo-kext-v1-7/) ·
[msr.kext — OS X MSR-reading kext](https://github.com/relan/msr.kext)

**Intel PT architecture:**
[Intel SDM Vol.3 — IA32_RTIT_CTL / ToPA / single-range output](https://xem.github.io/minix86/manual/intel-x86-and-64-manual-vol3/o_fe12b1e2a880e0ce-1700.html) ·
[Intel PT virtualization (KVM) — LWN](https://lwn.net/Articles/737839/)

**Why macOS routes around PT:**
[Pishi (POC2024) — macOS KEXT coverage fuzzing skips hardware tracing](https://r00tkitsmm.github.io/fuzzing/2024/11/08/Pishi.html) ·
[Trail of Bits — honeybee + Intel PT (Linux ecosystem)](https://blog.trailofbits.com/2021/03/19/un-bee-lievable-performance-fast-coverage-guided-fuzzing-with-honeybee-and-intel-processor-trace/)

**macOS kext / signing constraints:**
[Apple — securely extending the kernel (kexts deprecated / Reduced Security)](https://support.apple.com/guide/security/securely-extending-the-kernel-sec8e454101b/web) ·
[Apple — System Extensions & DriverKit (userspace replacement)](https://support.apple.com/guide/deployment/system-extensions-in-macos-depa5fb8376f/web)
