# asm-test - Hardware-assisted & foreign-JIT trace backends: implementation plan

A phased roadmap for two **hardware-assisted** native-trace backends — Intel PT
on bare-metal x86-64 and ARM CoreSight on bare-metal AArch64 — and the
forward-look capability of tracing a **foreign** JIT's generated code in a live
process. Both produce `asmtest_trace_t` offsets reusing the same registered-region
markers: instruction offsets identical to the Unicorn emulator and the DynamoRIO
native tier, and block offsets that match after a normalization step (a decoded
hardware block can span direct branches — see
[Risks](#risks-and-open-points-hardware-trace)).

This plan is a **sibling** of the
[DynamoRIO native-trace plan](dynamorio-native-trace-plan.md): it depends on that
plan's Phase 1 (the engine-neutral trace substrate) and Phase 5 (instruction-mode
semantics), reuses its begin/end markers, and is the backend the native-trace
plan's [Language runtime support](dynamorio-native-trace-plan.md#language-runtime-support)
matrix routes JIT/GC-heavy managed runtimes toward. It was split out of that plan
because it pulls in entirely different dependencies (libipt, OpenCSD,
`perf_event_open`, eBPF) and is even-more-optional and mostly bare-metal; keeping
it inline made shipping DynamoRIO read as if it required shipping CoreSight.
Throughout this plan, **native-trace Phase N** means a phase of the
[DynamoRIO native-trace plan](dynamorio-native-trace-plan.md); an unqualified
**Phase 1 / Phase 2** is one of *this* plan's own two phases.

> Status legend: **planned** unless noted. Update this file as phases land, the
> way [inline-asm-keystone-plan.md](../archive/plans/inline-asm-keystone-plan.md) and
> [win64-native-tier-plan.md](../archive/plans/win64-native-tier-plan.md) track theirs.

---

## Implementation status

Phase 1 is **implemented** for **Intel PT**: the `perf_event_open` AUX capture
(`src/hwtrace.c`), the libipt instruction decode + branch-boundary block
normalization (`src/pt_backend.c`), the full `asmtest_hwtrace_available()` gating
chain, and the `hwtrace-test` / `shared-hwtrace` targets all ship behind
`asmtest_hwtrace.h`. **ARM CoreSight** (`src/cs_backend.c`) is now **split like the
AMD backend**: its decoder-independent **reconstruction core**
(`asmtest_cs_reconstruct`) — ordered ETM/ETE instruction *ranges* → the same
instruction/block partition the PT backend produces — is **implemented and
host-validated** with synthetic ranges (`examples/test_hwtrace.c`
`test_cs_reconstruction`, the CoreSight analogue of `test_amd_reconstruction`),
asserting byte-for-byte parity with the PT/AMD/single-step backends over the shared
fixture. Only the **live OpenCSD decode tree** (`ocsd_create_dcd_tree` + ETMv4/ETE
decoder + memory accessor feeding ranges to the core) remains: it needs libopencsd
*and* a real AArch64 CoreSight board to write and validate, so per the project's
"no untested hardware code" rule it is not yet implemented and
`asmtest_cs_decoder_present()` returns 0 — the tier still self-skips on every host
until the tree is completed on a board, but the half that the board glue will feed
is now proven.

Overflow/loss now uses the precise signal: `asmtest_hwtrace_end` scans the perf
data ring for `PERF_RECORD_AUX` records and sets `truncated` on
`PERF_AUX_FLAG_TRUNCATED` (with the AUX-ring-full heuristic as a backstop).

A **third, AMD-specific** hardware backend was added as a sibling plan,
[amd-tracing-plan.md](amd-tracing-plan.md) (`ASMTEST_HWTRACE_AMD_LBR`,
`src/amd_backend.c`): AMD has no PT equivalent, so it reconstructs from the 16-deep
branch-record stack (Zen 3 BRS / Zen 4 LbrExtV2). Its reconstruction is
host-validated with synthetic branch records; live capture needs Zen 3+.

**Validation caveat.** The libipt API usage is verified against upstream
`intel-pt.h`, and the gating/self-skip path is exercised on every host — but the
live PT **capture** cannot run on AMD CPUs, VMs, or standard CI (no `intel_pt`
PMU), exactly as this plan predicts. On those hosts `make hwtrace-test` self-skips
with the specific reason. Capture is validated only on bare-metal Intel PT with
`perf_event_paranoid` lowered. User-facing docs:
[native-tracing.md](../../guides/tracing/native-tracing.md#hardware-trace-tier-intel-pt--arm-coresight).

Phase 2 (attach-to-foreign-JIT) remains **planned, forward-look** (it is research-
grade: PT-attach-to-PID + a time-aware jitdump/eBPF code-image recorder, then the
hypervisor/EPT frontier — see the analysis doc); it needs PT hardware to build and
validate and is intentionally not implemented as untested code.

Phase 2's **code-image building blocks have, however, already landed** out of the
[single-step plan's W2 work](zen2-singlestep-trace-plan.md) (Phase 5) and are reusable
here: the code-region resolvers `asmtest_proc_region_by_addr` (`/proc/<pid>/maps`) and
`asmtest_proc_perfmap_symbol` (`/tmp/perf-<pid>.map`), **and** `asmtest_jitdump_find` —
the binary jitdump reader that recovers a JIT method's recorded **code bytes** and load
timestamps (the bytes a branch-trace decoder must be handed).

**The time-aware code-image recorder itself now ships** —
`asmtest_codeimage` ([asmtest_codeimage.h](../../../include/asmtest_codeimage.h),
`src/codeimage.c`), the byte-source half of Phase 2 and approach #2 of the
[JIT-runtime-tracing analysis](../analysis/jit-runtime-tracing.md). It is the userspace
`PERF_RECORD_TEXT_POKE` the decoder's image callback needs: a **timestamped code-image
timeline** built cross-process from soft-dirty + `PAGEMAP_SCAN` (bytes via
`process_vm_readv`), queryable by `asmtest_codeimage_bytes_at(img, addr, when, …)`, plus
an **optional eBPF emission detector** (a CO-RE program on
`mprotect`/`mmap`/`memfd_create`, PID-namespace-filtered) that snapshots on the
`PROT_EXEC` edge. It is live-validated on x86-64 Linux (`make codeimage-test`, the
versioned W2 trace in `make hwtrace-test`, and the eBPF lane `make
docker-hwtrace-codeimage`) and is already paired with the on-host W2 ptrace stepper
(`asmtest_ptrace_trace_attached_versioned`) — the AMD-host route that does not need PT.

What Phase 2 still needs on top of all this is the **hardware half**:
**PT-attach-to-live-PID** capture (`perf record -p <pid>` / `perf_event_open` on a
running process) and feeding the recorder's (or jitdump's) bytes into the libipt decoder
at the right point in the trace — which needs PT hardware to build and validate, so it
stays the forward-look.

---

## Phase 1 - Hardware-assisted trace backends (Intel PT, ARM CoreSight) *(planned)*

**Goal.** Record native block and instruction coverage with near-zero *capture*
overhead on capable hardware — Intel PT on bare-metal Intel x86-64, ARM CoreSight
on bare-metal AArch64 — producing the same `asmtest_trace_t` offsets as Unicorn
and DynamoRIO, reusing the registered-region markers, with no DynamoRIO or
`drwrap` dependency. This is an even-more-optional, mostly-bare-metal-Linux tier:
a fast-path complement to DynamoRIO, never a replacement and never a default —
**except** for JIT/GC-heavy managed runtimes (JVM, .NET, Node), where it is the
*recommended* backend because it sidesteps DynamoRIO's signal and code-cache
collisions entirely (see the native-trace plan's
[Language runtime support](dynamorio-native-trace-plan.md#language-runtime-support)).

**Why it fits.** Both Intel PT and ARM CoreSight, after decode, yield exactly the
two dimensions `asmtest_trace_t` already carries: ordered instruction offsets and
distinct basic-block offsets, each as `off = ip - base` from a registered range.
The instruction offsets are identical to Unicorn/DynamoRIO; the **block** offsets
are not free — a libipt/OpenCSD decoded block can span direct branches, so block
boundaries need a normalization step before they match Unicorn/DR basic blocks
(see [Risks](#risks-and-open-points-hardware-trace)).
Hardware records branch *decisions* only — it emits nothing per sequential
instruction — so the per-instruction and per-block stream is *reconstructed* by
the decoder replaying asm-test's own registered code bytes between branch
waypoints. asm-test always holds those bytes, so the one hard precondition is
always met. Overflow maps onto the existing `truncated` bit, so no new public
trace shape is needed — though these are *distinct* loss points: the PT `OVF`
packet signals on-chip PT-buffer overflow, while `PERF_AUX_FLAG_TRUNCATED` (and
the CoreSight equivalent) signals the perf AUX ring buffer filling; both mean
trace was lost and both collapse onto the one bit.

**Backends.**

- **Intel PT.** `perf_event_open` with `attr.type` from
  `/sys/bus/event_source/devices/intel_pt/type` and `pid=0` self-traces the
  calling thread; a second `mmap` exposes the AUX buffer the CPU fills with
  TNT/TIP/PSB packets. libipt decodes them against the registered bytes
  (`pt_image_add_cached` over `[base, base+len)`): `pt_insn_next` yields ordered
  instructions for `insns[]`; the block decoder's `pt_block.ip` + `ninsn` feed
  `blocks[]`. A read-write AUX mapping is a linear buffer that must be drained
  (overflow is dropped and flagged); a read-only mapping is circular, the basis
  of low-overhead snapshot mode.
- **ARM CoreSight.** `perf_event_open` for `cs_etm` (per-thread) captures ETM/ETE
  waypoint trace to a sink (TMC-ETR/ETB, or a per-CPU TRBE on ARMv9 ETE);
  OpenCSD deformats and decodes it against the same bytes into instruction
  ranges, giving `insns[]`/`blocks[]`. `FEAT_TRF` exception-level filtering
  (`E0TRE`) scopes a `/u` capture to EL0 userspace, cutting runtime/kernel noise.

**Region mapping (the ergonomic win).** The registered `[base, base+len)` maps to
one *hardware* address-range filter — Intel PT's `IA32_RTIT_ADDRn_A/B` comparators
(Linux `perf --filter 'filter <off>/<len>@<obj>'`), or CoreSight's ETM/ETE address
comparators with identical `perf --filter` syntax — so the CPU emits packets only
inside the region with no inserted instrumentation at all. The begin/end markers
realize two equivalent styles: *range* style installs the filter and begin/end
merely `ioctl(PERF_EVENT_IOC_ENABLE/DISABLE)` the AUX capture around the call
(preferred for a single registered routine); *address* style uses hardware
`start`/`stop` on the marker PCs to bracket a sub-range. The markers stay real
exported functions so address style can name their PCs and the API stays
backend-neutral, but `drwrap` is *not* needed here. Per-thread scoping is just
`pid=0`, replacing the DynamoRIO per-thread active-region stack.

Two limits drive a software fallback. Hardware comparators are few — Intel PT
reports its count via CPUID leaf `14H` `RANGECNT` / `nr_addr_filters` (often 2),
CoreSight a small fixed number — so beyond that, distinct simultaneous regions
fall back to *software* post-filtering of decoded IPs by `[base, base+len)`.
Keystone-generated executable memory (the native-trace plan's Phase 4) has no
backing object file, but `perf` filter addresses are object-relative, so generated
regions also use the software post-filter rather than a hardware filter.

**Decode and annotation reuse.** libipt (PT) and OpenCSD (CoreSight) are *new*
optional *decoders* — the reverse of Capstone: they turn the hardware packet
stream into the offset stream. The existing Capstone layer (`src/disasm.c`,
`emu_trace_report_disasm` and friends) is reused *unchanged* to *annotate* those
offsets, so Unicorn, DynamoRIO, PT, and CoreSight share one report format and no
hardware-specific decoder leaks into language bindings. The registered bytes flow
twice from one source: into libipt/OpenCSD to reconstruct offsets, then into
Capstone to render text.

**Deliverables.**

- `include/asmtest_hwtrace.h`: an `asmtest_trace_backend_t` enum
  (`INTEL_PT`, `CORESIGHT`), an options struct (backend, AUX/DATA buffer sizes,
  snapshot-vs-linear mode, optional object-file hint), and
  `asmtest_hwtrace_available/init/register_region/shutdown` plus the shared
  `asmtest_trace_begin/end` markers.
- `src/pt_backend.c` (libipt) and `src/cs_backend.c` (OpenCSD), each its own
  translation unit, plus a Linux-only perf-AUX capture helper.
- `examples/test_hwtrace.c`: a smoke test that self-skips when unavailable.
- Makefile knobs `LIBIPT_CFLAGS/LIBIPT_LIBS` and `OPENCSD_CFLAGS/OPENCSD_LIBS`
  (pkg-config auto-detect, mirroring `CAPSTONE_*`), defines `-DASMTEST_HAVE_LIBIPT`
  / `-DASMTEST_HAVE_OPENCSD`, and targets `hwtrace-test` and `shared-hwtrace`
  (a separate `libasmtest_hwtrace`, never linked by core or `libasmtest_emu`).
- `asmtest_hwtrace_available()`: the detect-and-skip routine encoding the full
  gating chain (decoder lib present, PMU node present, right CPU/ISA/vendor, perf
  privilege, not a restricted VM guest).

**Acceptance.**

- `make hwtrace-test` self-skips with a clear message — the common case — when the
  decoder library, the `intel_pt`/`cs_etm` PMU, the right CPU/host, or the
  `perf_event` privilege is absent.
- On a bare-metal Intel x86-64 Linux host with PT and `perf_event_paranoid`
  lowered, tracing a registered routine (or a native-trace-plan Phase 4 Keystone
  host-native routine) records block offset `0` and a deterministic ordered
  `insns[]` that matches the Unicorn/DynamoRIO output for the same code.
- A tiny AUX buffer or a hot loop sets `truncated` via overflow.
- CoreSight acceptance is identical, explicitly opt-in on a named bare-metal
  AArch64 board.

**Licensing.** libipt is BSD and OpenCSD is BSD-3-Clause, so this tier carries no
LGPL obligation (unlike `drwrap`). OpenCSD is C++, unlike the otherwise-C optional
tiers; keep it isolated in its own translation unit and the separate
`libasmtest_hwtrace`.

**CI.** Hardware trace cannot run on standard CI: GitHub-hosted runners are
virtualized Azure VMs that do not expose Intel PT to guests, and cloud ARM exposes
no `cs_etm`. A real job needs a *self-hosted* bare-metal Intel x86-64 (PT) or
known-good AArch64 board (CoreSight) runner with `perf_event_paranoid` lowered —
an explicit, separate, allowed-to-be-absent job that never gates normal tests.
This carries a **standing operational cost** that must be owned, not assumed: a
self-hosted runner that executes untrusted PR code with `perf_event_paranoid`
lowered (and possibly `CAP_PERFMON`/`CAP_SYS_ADMIN`) is a real security exposure,
so gate it to trusted branches / maintainer-approved runs only, and accept that
these tiers ship with materially weaker automated regression protection than the
Unicorn tier.

**Effort.** PT capture + libipt decode 5-8 days on available hardware (this range
**includes** the block-boundary normalization step from
[Risks](#risks-and-open-points-hardware-trace) — budget ~1-2 of those days for it
and its cross-backend parity tests); CoreSight + OpenCSD a further 5-8 days, gated
on board access. **Hard dependency, not a soft sibling link:** both backends
cannot produce parity offsets until **native-trace Phase 1** (the trace substrate
*and* its backend-neutral renames — `asmtest_trace_*`, `asmtest_arch_t` — and the
`base_addr` report-helper fix) and **native-trace Phase 5** (instruction-mode
semantics) have landed. This plan reads as standalone and is *packaged*
independently, but it is **sequenced after** roughly the first half of the
DynamoRIO plan; it cannot ship block/instruction parity before then.

**Validation (hardware trace).** Intel PT ships since Broadwell and
Goldmont/Apollo Lake; the Linux `intel_pt` PMU since 4.2, with address-range
filtering added in 4.7. `CAP_PERFMON` (Linux 5.8+) or a low enough
`perf_event_paranoid` is required, and a usable default-size AUX capture for an
unprivileged process effectively needs `perf_event_paranoid = -1` or
`CAP_PERFMON` — a host knob the process cannot set itself. PT is in practice
unavailable on standard cloud VMs and GitHub-hosted runners (the hypervisor does
not expose it), though PT is itself virtualizable when the host opts in. ARM
`FEAT_ETE` requires `FEAT_TRF` and can pair with either a `TRBE` sink or a legacy
CoreSight sink (ETR/ETB); both `FEAT_ETE` and `FEAT_TRBE` are optional from
Armv9.0. KVM clears `TRFCR_EL1` and does not advertise the trace filter to guests,
so cloud ARM VMs (Graviton, Altra) cannot self-host trace, and Apple Silicon does
not expose CoreSight to the OS; self-hosted capture is realistic only on specific
dev boards/phones (Juno, ZCU102/Kria, Jetson, Pixel) with `CONFIG_CORESIGHT*` and
`perf` built against OpenCSD.

- Intel PT (Linux perf): https://perf.wiki.kernel.org/index.php/Perf_tools_support_for_Intel%C2%AE_Processor_Trace
- libipt decoder library: https://github.com/intel/libipt
- ARM CoreSight / cs_etm (Linux perf): https://www.kernel.org/doc/html/latest/trace/coresight/coresight.html
- OpenCSD decoder library: https://github.com/Linaro/OpenCSD

---

## Phase 2 - Attach-to-foreign-JIT tracing *(byte-source recorder done; PT-attach decode forward-look)*

**Goal.** Trace the machine code a *foreign* JIT (JVM, V8, CoreCLR, the CPython
3.13+ JIT, LLVM ORC) generates inside a **running** process — attached at runtime,
least-invasively, at instruction granularity. This is the capability the
native-trace plan's
[Language runtime support](dynamorio-native-trace-plan.md#language-runtime-support)
matrix routes the hard managed runtimes toward. **Full treatment:**
[Analysis: tracing JIT-generated assembly at runtime](../analysis/jit-runtime-tracing.md).

**Approach (decided direction).** Stop instrumenting, start observing: hardware
trace (Intel PT / CoreSight) for the execution stream + a **time-aware capture of
the JIT's code bytes** for decode. PT/ETM are out-of-band (no code cache, no
signal hijack, no W^X faults — none of the three collisions), attach to a live
PID via `perf_event_open -p`, and reuse the Phase 1 substrate and the Capstone
annotation layer. The one hard problem is *temporal* — the same address holds
different bytes over time as the JIT patches/frees/reuses code — so a single
snapshot is wrong; the decoder needs the bytes that were live at each trace
position.

**Deliverables (when scheduled).**

- Runtime attach: `perf_event_open` against an existing PID (vs Phase 1's `pid=0`
  self-trace of asm-test's own region).
- _Done._ A **time-aware code-image recorder** that replaces Phase 1's "asm-test owns
  the bytes" saving grace — `asmtest_codeimage` ([asmtest_codeimage.h](../../../include/asmtest_codeimage.h),
  `src/codeimage.c`): a userspace `PERF_RECORD_TEXT_POKE` building a timestamped
  code-image timeline cross-process (soft-dirty + `PAGEMAP_SCAN`, bytes via
  `process_vm_readv`), queryable by `asmtest_codeimage_bytes_at(addr, when)`, with an
  **optional eBPF emission detector** (CO-RE on `mprotect`/`mmap`/`memfd_create`,
  PID-namespace-filtered, `bpf_ringbuf`) for snapshotting on the `PROT_EXEC` edge.
  Live-validated (`make codeimage-test`, `make docker-hwtrace-codeimage`), exposed across
  all ten bindings, and already wired into the W2 stepper
  (`asmtest_ptrace_trace_attached_versioned`). This is the foreign-process equivalent of
  approach #2's "eBPF + userfaultfd-WP" sketch (uffd-WP needs the owning process to
  register, so soft-dirty is the cross-process primitive). Runtime-enabled **jitdump**
  (a) — .NET `DiagnosticsClient.EnablePerfMap(JitDump)` / JVM jitdump agent + `perf inject
  --jit` — and feeding the recorder's bytes into libipt's `pt_image_set_callback` keyed
  to trace position remain the PT-hardware half below.
- libipt (or libxdc) decode; reuse the Capstone layer to render recovered bytes —
  no new decoder API in the bindings.
- The hypervisor/EPT frontier as the research-grade, maximum-stealth option:
  DRAKVUF (Xen VMI) supports *both* host-side Intel PT guest-execution tracing
  (via Xen `vmtrace`) *and*, separately, stealthy altp2m execute-only EPT views
  for hidden breakpoints — two distinct out-of-VM mechanisms, not a single fused
  PT+altp2m technique.

**Acceptance.** Attach to a running CPython/.NET/JVM process, capture and decode at
least one JIT-generated routine into a deterministic disassembled instruction
trace that matches a ground-truth disassembly of the same bytes.

**Status.** The **byte-source half is implemented and live-validated** — the
`asmtest_codeimage` time-aware recorder + its eBPF emission detector, paired with the W2
ptrace stepper (the AMD-host route that needs no PT). What remains forward-look is the
**PT hardware half**: attach-to-live-PID PT capture + libipt decode keyed to the
recorder's timeline, which needs PT hardware to build and validate. Distinct from the
Phase 0-8 work of the native-trace plan, which traces code asm-test generates itself;
depends on this plan's Phase 1 (PT substrate) and **native-trace Phase 5**
(instruction-mode semantics). See the analysis doc for the ranked approaches, per-runtime
enablement matrix, prior art, and caveats.

**Effort.** PT-attach + jitdump-live slice 3-5 days on PT hardware; the eBPF+uffd
time-aware recorder a further 1-2 weeks; the hypervisor/EPT frontier is
research-grade.

---

## Risks and open points (hardware trace)

- **Hardware-trace availability.** Intel PT is Intel-x86-64-only and
  absent on AMD, ARM, Apple Silicon, and almost all cloud/VM/GitHub-hosted
  guests; CoreSight self-hosted trace is realistic only on specific bare-metal
  AArch64 boards and is prohibited in KVM guests. The tier is opt-in and cannot
  be a default or a portable CI gate. AMD has **no Intel PT equivalent** (no
  continuous control-flow trace ring on Zen); the closest AMD-native approximation
  and its hard 16-branch limit are specified in the sibling
  [AMD LBR snapshot plan](amd-tracing-plan.md), with DynamoRIO as the fallback
  beyond that window.
- **Hardware-trace privilege.** Both backends need `perf_event_paranoid`
  lowered (effectively `-1` for a default-size PT buffer) or
  `CAP_PERFMON`/`CAP_SYS_ADMIN`, plus `perf_event_mlock_kb`/`RLIMIT_MEMLOCK` for
  large AUX buffers — host knobs the process cannot grant itself.
  `asmtest_hwtrace_available()` must detect-and-skip on exactly this.
- **Hardware-trace fidelity.** Decode is impossible without the exact
  registered code bytes (libipt returns `-pte_nomap`); self-modifying or relocated
  code silently corrupts offsets. Speculative/aborted paths
  (`pt_block.speculative`) must be filtered before recording. Finite comparators
  and file-less Keystone memory force a software IP post-filter fallback for some
  regions.
- **Block-offset parity is not free — the normalization step is real work, not a
  footnote.** A libipt/OpenCSD decoded block ends only at a *taken* branch or a
  trace discontinuity, so it can span fall-through and even direct branches; the
  hardware block partition is therefore strictly **coarser** than the
  Unicorn/DynamoRIO basic-block partition, and the raw `pt_block.ip`/`ninsn`
  stream does **not** yield offsets that match the other backends. The concrete
  normalization: take the per-instruction stream the decoder already reconstructs
  (`pt_insn_next` / the OpenCSD instruction-range output), then **re-split into
  basic blocks at every branch *target* and after every branch instruction**,
  recomputing DR/Unicorn-equivalent boundaries from `insns[]` rather than trusting
  the decoder's coarser blocks. This is mechanical but non-trivial (it duplicates
  the basic-block-boundary logic the emulator gets for free from Unicorn's block
  hook) and must be unit-tested against the DynamoRIO tier's block set for the same
  routine. The **instruction** offsets need no such step — they are identical to
  Unicorn/DynamoRIO for a deterministic, non-speculative, single-threaded routine
  (the acceptance-test case); the *block* offsets are correct only **after**
  normalization. The alternative is to ship a documented per-backend block
  difference, but the stated goal is parity, so budget the normalization. Its cost
  is folded into the Phase 1 effort estimate below.
