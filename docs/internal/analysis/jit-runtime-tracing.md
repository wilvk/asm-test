# Analysis: tracing JIT-generated assembly at runtime

*Status: analysis / findings. This document records a research investigation,
not shipped behaviour. The corresponding implementation roadmap is the
[hardware-trace plan](../plans/hardware-trace-plan.md) (Phase 1 hardware trace,
and the Phase 2 foreign-JIT forward-look it adds), a sibling of the
[DynamoRIO native-trace plan](../plans/dynamorio-native-trace-plan.md). It is the
sequel to the DynamoRIO plan's [Language runtime support](../plans/dynamorio-native-trace-plan.md#language-runtime-support)
section: that section explains why in-process DBI fights managed runtimes; this
one asks how to trace a **foreign** JIT's generated code anyway. For the in-process,
**cooperative** face of the same machinery — wrapping a region of your own managed code
in a `using` block, non-intrusively — see the sibling
[scoped-inprocess-tracing.md](scoped-inprocess-tracing.md).*

> **Update — approach #2's core has shipped.** The "time-aware code-image recorder"
> (below, [§2](#2-general-case--ebpf--userfaultfd-time-aware-recorder-no-runtime-cooperation))
> — the piece this document calls *"the innovative, buildable core of a generic tracer"* —
> is now implemented as `asmtest_codeimage` ([include/asmtest_codeimage.h](../../../include/asmtest_codeimage.h),
> `src/codeimage.c`): a userspace `PERF_RECORD_TEXT_POKE` that builds the timestamped
> code-image timeline cross-process via **soft-dirty + `PAGEMAP_SCAN`** (the practical,
> foreign-process-capable equivalent of the uffd-WP-async sketch below — note uffd-WP
> needs the *owning* process to register, so it cannot watch a foreign JIT), with bytes
> read via `process_vm_readv`. The W2 ptrace stepper consumes it
> (`asmtest_ptrace_trace_attached_versioned`), and the **eBPF emission detector** (§2's
> "detect emission" bullet — a CO-RE program on `mprotect`/`mmap`/`memfd_create`) ships as
> the optional, container-validated acceleration layer. What remains forward-look is the
> hardware half: feeding this byte timeline into the **Intel PT / libipt** decoder
> ([hardware-trace plan Phase 2](../plans/hardware-trace-plan.md)), which needs PT
> hardware (this dev host is AMD); the recorder is already paired with the on-host
> ptrace stepper instead. Read the rest of this document as the design rationale; the
> [CHANGELOG](../../../CHANGELOG.md) tracks the shipped surface.

## Question

Given full control of the OS (root/admin), is there a way **around** the
JIT-hostility of dynamic binary instrumentation that can be **attached to a
running process** and **trace the actual machine code a JIT generates**, with the
**most detailed** output that is also the **least invasive**? Consider innovative
approaches.

This is a different and harder problem than the rest of the DynamoRIO plan. There,
asm-test *generates the code itself* (via Keystone, into its own `mmap` region),
so the bytes are known and stable — the plan's "saving grace". Here the code is
emitted by a **foreign JIT** (JVM, V8, CoreCLR, the CPython 3.13+ JIT, LLVM ORC,
…) inside a live process, and may be patched, freed, and have its address reused
while we watch.

## Determination

**Yes — stop instrumenting and start observing.** The way around JITs is
**hardware trace** (Intel PT on x86-64, ARM CoreSight ETM on AArch64) for the
execution stream, attached to a live PID with `perf_event_open` / `perf record
-p <PID>`, paired with a **time-aware capture of the JIT's code bytes** so the
trace can be decoded back into the generated assembly.

- Hardware trace is **out-of-band**: no software code cache, no hijacked SIGSEGV,
  no W^X page-fault thrashing — it avoids all three collisions that make DBI
  fragile inside managed runtimes. Overhead is ~2–20 % (typically single-digit),
  zero when detached, and it needs **no cooperation** from the target.
  ([Jane St magic-trace], [easyperf])
- The single hard problem is that PT/ETM record **only control-flow decisions,
  not instruction bytes**; the decoder must be handed the exact bytes that were
  live **at that point in the trace**. For a JIT that is fundamentally a
  *temporal* problem (same address, different bytes over time). Solving it well
  is where the engineering — and the innovation — lives.

The rest of this document develops the reframe, the temporal problem, four ranked
approaches (all attach-at-runtime, all instruction-level), a recommended pipeline,
and the prior art.

## Why hardware trace is the way around JITs

Every DBI engine (DynamoRIO, Pin, Frida Stalker) *instruments*: it copies the
target's code into a software code cache, recompiling basic blocks on the fly,
and must own SIGSEGV and trap writes to W+X pages to keep that cache consistent.
A JIT does exactly the same things for its own purposes (emits, patches, and
frees executable memory; uses SIGSEGV for null checks / safepoints / WASM bounds
checks), so the two engines collide head-on. That collision is documented at
length in the plan's [Language runtime support] section.

Hardware trace sidesteps the collision by **not touching the code at all**. The
CPU records branch outcomes to a memory buffer (Intel PT: conditional branches as
one bit each in TNT packets, indirect branches as TIP packets; ARM ETM is the
analog), with no instrumentation in the executed path. ([easyperf],
[perf-intel-pt])

| Property | DBI (DynamoRIO/Pin/Frida) | Hardware trace (PT / ETM) |
|---|---|---|
| Mechanism | recompile into software code cache | CPU records control flow out-of-band |
| Owns SIGSEGV? | yes (collides with JIT/GC) | no |
| W^X fault thrash on JIT pages? | yes | no |
| Cooperation from target? | none, but perturbs it heavily | none, minimal perturbation |
| Overhead | 2×–50× depending on tool/work | ~2–20 %, 0 % detached |
| Attach to running PID? | Pin yes; DR experimental; Frida yes (ptrace) | **yes** (`perf record -p`) |
| Detail | instruction/block | **instruction/block + timing** |
| Gets JIT code bytes for free? | yes (it copies them) | **no — must be supplied** |

The last row is the whole catch.

## The one hard problem — and it is temporal

PT/ETM emit no instruction bytes. To turn the branch stream into assembly the
decoder (Intel **libipt**, ARM **OpenCSD**) needs a *code image* — the bytes at
each executed address. The `perf-intel-pt` man page is explicit: "the executed
images are needed — which makes use in JIT-compiled environments, or with
self-modified code, a challenge." When the bytes are missing, libipt returns
`-pte_nomap` ("no memory mapped at this address"). ([perf-intel-pt], [libipt #57])

The subtle part: **libipt has no temporal model.** It assumes one static image
per (address-space, address); its `pt_image` is fed via `pt_image_add_file()`, an
image-section cache, or a read-memory callback `pt_image_set_callback()` — none of
which carries a time dimension. ([libipt howto]) For a JIT:

- code at an address **changes** (in-place patching, on-stack replacement);
- code is **freed** and the **address reused** for unrelated code;
- a single late snapshot (`gcore`, one `/proc/PID/mem` read) therefore returns
  **wrong bytes** for any address that changed or was reused during the trace.

The kernel's own answer for *kernel* self-modifying code is
`PERF_RECORD_TEXT_POKE` (Linux 5.8), which records **both old and new bytes** at
an address with a timestamp so the PT decoder can switch images at the right
point. There is **no userspace equivalent** — so a foreign-JIT tracer must
reproduce TEXT_POKE semantics in userspace: build a **timestamped code-image
timeline** and feed the version that was live at each trace position.
([perf_event_open(2)], [TEXT_POKE series])

Everything innovative below is a way to build that timeline cheaply, at runtime.

## Approaches, ranked

All four attach at runtime and yield instruction-level detail; they differ in how
they obtain correct bytes, in invasiveness, and in maturity.

| # | Approach | How it gets the bytes | Invasiveness | Maturity |
|---|---|---|---|---|
| 1 | **PT + runtime-enabled jitdump** | turn the JIT's own emitter on in a live process, then `perf inject --jit` | lowest | **production** |
| 2 | **PT + eBPF/uffd time-aware capture** | out-of-band byte recorder, no runtime cooperation | very low | **buildable** (novel glue) |
| 3 | **Hypervisor: host-PT + EPT execute-trap capture** | EPT execute-only views dump freshly-emitted pages; host PT for the stream | lowest *to the guest*, agentless | **research-grade** |
| 4 | **Deterministic record/replay** (TTD, rr) | re-execute / emulate → bytes reproduced by construction | high | production (TTD = Windows) |

### 1. Cooperative — jitdump, enabled at runtime

The clean path when the runtime emits a **jitdump** (`jit-<pid>.dump`): its
`JIT_CODE_LOAD` records carry the *raw byte encoding* of each compiled function,
and `perf inject --jit` materialises them as per-function ELF images the PT
decoder consumes. A plain `/tmp/perf-<pid>.map` is **symbols only, no bytes** and
is insufficient for instruction decode. ([jitdump spec], [perf-inject])

The non-obvious finding: jitdump often does **not** need a launch flag.

| Runtime | Bytes? | Enable on a *running* process? | Code stable after capture? |
|---|---|---|---|
| **.NET / CoreCLR** | jitdump = bytes | **Yes (best):** `DiagnosticsClient.EnablePerfMap(PerfMapType.JitDump)` over the diagnostic port; CoreCLR re-emits already-JITted methods (`sendExisting=true`). `DOTNET_PerfMapEnabled` is launch-only. | code bodies not relocated; tier/OSR/ReJIT make a *new* body at a new address |
| **JVM / HotSpot** | jitdump agent = bytes; perf-map-agent = symbols | **Yes:** attach a jitdump JVMTI agent via `jcmd <pid> JVMTI.agent_load`, then `GenerateEvents(COMPILED_METHOD_LOAD)` replays every existing nmethod (mind JEP 451 `-XX:+EnableDynamicAgentLoading`) | individual nmethod stable; re-JIT = unload + new load |
| **V8 / Node** | `--perf-prof` = bytes; `--perf-basic-prof` = symbols | **No** — perf logger wired once at isolate init; CLI/`NODE_OPTIONS` only | `--perf-prof` disables code compaction, so addresses stay stable |
| **CPython 3.12+** | `-X perf` = symbols; `-X perf_jit` = jitdump (trampolines) | in-process (`sys.activate_stack_trampoline`); external only via PEP 768 (3.14+, root) | default build has no JIT — little to capture |
| **GDB JIT iface** | in-memory ELF = bytes | **Yes:** walk `__jit_debug_descriptor` on attach (V8, LLVM ORC, CoreCLR implement it; HotSpot does not) | re-registered on relocation |

### 2. General case — eBPF + userfaultfd time-aware recorder (no runtime cooperation)

When the JIT will not emit jitdump, build the timestamped image yourself. This is
the innovative, buildable core of a generic tracer:

- **Detect emission.** eBPF on `mprotect(PROT_EXEC)` / `mmap` / `memfd_create`, or
  a uprobe on the runtime's own code-publish symbol, or a USDT probe (HotSpot
  ships `compiled__method__load`). eBPF **cannot** uprobe anonymous JIT memory
  directly — uprobes are inode+offset based — so hook the syscall or a real
  `.so` symbol, not the JIT region. ([Quarkslab uprobe])
- **Grab the bytes.** From a userspace helper via `process_vm_readv` (you have
  `CAP_SYS_PTRACE`), or in-kernel via `bpf_probe_read_user` into a ring buffer
  (mind the 512-byte stack limit; use a per-CPU array or `bpf_dynptr`).
- **Beat the write-after-protect race and catch overwrite/reuse.** A page can be
  patched *after* it is made executable, so a snapshot at the PROT_EXEC moment may
  be stale. Use `userfaultfd` write-protect **async** mode + the `PAGEMAP_SCAN`
  ioctl (Linux ≥ 6.7) — or soft-dirty PTEs (`/proc/PID/clear_refs` + pagemap bit
  55) as a coarse fallback — to re-snapshot a page **only when it actually
  changes**, stamping each version. ([userfaultfd], [PAGEMAP_SCAN], [soft-dirty])
- **Feed the decoder** through libipt's `pt_image_set_callback`, returning the
  version live at the current trace position; re-snapshot at PT **PSB/OVF**
  boundaries and on `-S` SIGUSR2 (flight-recorder) to bound staleness.
- The GDB JIT interface (a uprobe on `__jit_debug_register_code`, harvesting each
  published in-memory ELF) is a clean alternative publish hook where implemented.

eBPF is **sideband only** — it tells you *when/where* code appears; it cannot
trace the instruction stream (it is event-driven and the verifier forbids
per-instruction following). Hardware trace remains the sole source of the
executed-instruction stream; the two are complementary. ([ebpf verifier])

### 3. Frontier — hypervisor / EPT-assisted (most innovative, most stealthy)

Run the workload in a **Xen** guest and attach to it live:

- **Host-controlled Intel PT** of the guest per-vCPU (PT-in-VMX, `kvm_intel
  pt_mode=1` on KVM; host buffers) → the dense instruction stream into
  host-owned memory at near-native speed, with no in-guest agent. ([LWN PT-VMX])
- **Xen altp2m execute-only EPT views** to **execute-trap freshly-emitted JIT
  pages** and capture the exact bytes the moment they first run — invisible to
  the JIT/GC (the guest sees its pages as ordinary RWX; only fetches trap, at the
  EPT layer). ([Xen altp2m])
- **DRAKVUF already ships both halves**: the `codemon` plugin does EPT
  execute-trap code dumps and handles self-modifying code (execute-trap +
  write-trap toggle), and the `ipt` plugin records Intel PT with PTWRITE CR3/TID
  markers — agentless, attach-to-running-VM. ([DRAKVUF IPT], [DRAKVUF codemon])

The genuinely **unpublished** piece is wiring the EPT-captured bytes *into* the PT
decoder. The same idea has prior art at other layers — **FlowJIT** (SPE 2018)
execute-traps in the OS page-fault handler and feeds a PT decoder; **libxdc**
(kAFL/Nyx) captures-on-execute via a hardware breakpoint then feeds its decoder —
but nobody has done the capture at the EPT/hypervisor layer. That composition is a
sound, buildable research contribution. ([FlowJIT], [libxdc])

Precondition: the target must run in a VM (Xen for production altp2m; KVM
introspection exists only out-of-tree). This is the heaviest to stand up but the
least perturbing to the guest and the most resistant to anti-instrumentation.

### 4. Deterministic record/replay (solves the bytes problem by construction)

- **WinDbg TTD** emulates and records the executed bytes directly, so it natively
  handles .NET/JVM/self-modifying code — but it is Windows-only and heavy.
  ([TTD])
- **Mozilla rr** re-executes deterministically (retired-branch counter + syscall
  record), so JIT bytes reproduce on replay — but it is per-thread/serialised and
  a record/replay model rather than "observe a live process". ([rr])

These eliminate the temporal bytes problem by re-running the code, at the cost of
much higher overhead and a different operating model.

## Recommended pipeline (Linux x86-64, root, attach at runtime)

1. **Stream.** `perf record -e intel_pt//u -p <PID> --per-thread -S` (snapshot /
   flight-recorder; dump on SIGUSR2). Per-thread avoids the extra sideband and
   privilege that per-CPU decode needs.
2. **Bytes.** If .NET/JVM → enable jitdump live (§1) and `perf inject --jit`.
   Otherwise → run the eBPF + uffd-WP **code-image recorder** (§2) producing a
   timestamped byte timeline.
3. **Decode.** libipt (or **libxdc**, ~15–30× faster) with a time-keyed image
   callback → ordered instruction offsets + basic blocks; filter speculative
   paths (`pt_block.speculative`); map overflow (`OVF`) to the existing
   `truncated` bit.
4. **Render assembly.** Feed the recovered bytes through **Capstone** — already
   wrapped in asm-test ([src/disasm.c]) — so the output is the actual
   disassembled, executed JIT instructions with timing. PTWRITE can inject region
   markers.

This is the maximum detail obtainable out-of-band (full instruction stream + the
real generated assembly + timing), attachable to a running process, with
single-digit-% perturbation and none of the DBI collisions.

**ARM.** The picture mirrors x86 exactly: `cs_etm` ↔ `intel_pt`, OpenCSD ↔
libipt, both need the code image ("ELF files **or** the JIT code cache"), and
`perf inject --jit` is architecture-independent (the jitdump header carries
`elf_mach`). ([CoreSight], [OpenCSD])

## Data provided

The implementation fills one shared record — `asmtest_trace_t`, the same sink
every backend uses (emulator, DynamoRIO, PT/foreign-JIT) — plus, for the JIT case,
the generated code and its disassembly. The output is **control-flow complete**,
not a register/memory snapshot (see the boundary below).

**Core trace record** (`asmtest_trace_t`; offsets are `off = ip - base`, byte
offsets from the registered region):

| Field(s) | Data |
|---|---|
| `insns[]`, `insns_len`, `insns_total` | **ordered instruction trace** — each executed instruction's offset in execution order; `len` = entries kept, `total` = instructions executed (counts past the buffer cap) |
| `blocks[]`, `blocks_len`, `blocks_total` | **basic-block coverage** — the *distinct* block-start offsets entered (deduped); `len` = distinct blocks, `total` = every block entry (a loop counts each pass) |
| `truncated` | a buffer filled and entries were dropped; PT/ETM overflow (`OVF` / `PERF_AUX_FLAG_TRUNCATED`) maps onto this bit |

**The generated assembly** (the distinctive Phase-10 payload):

- the JIT's **machine-code bytes**, time-versioned (the code-image timeline from
  "The one hard problem") — a data product in their own right;
- **disassembled instructions** (mnemonic + operands) rendered from those bytes
  through Capstone (`emu_trace_disasm` / `emu_trace_report_disasm`), so the output
  reads as assembly rather than bare offsets.

**Derived / rendered outputs** (existing backend-neutral helpers): `asmtest_trace_covered(off)`
(was a block reached), `emu_trace_report` (human-readable report),
`emu_coverage_uncovered` (diff against a universe — what was *not* hit),
`emu_trace_lcov` / `emu_trace_lcov_source` (lcov for CI), and
`emu_trace_source_report` (source-line attribution via a caller-supplied line map).

**PT-specific extras** (available beyond the current struct, as extensions):
timing (PT TSC/CYC/MTC → per-region / per-branch cycle or wall-clock),
speculative-path filtering (`pt_block.speculative` dropped), per-thread
attribution, and PTWRITE CR3/TID markers in the hypervisor variant (§3).

**What it does *not* provide (boundary).** PT/ETM record **control flow, not data
values**. You get which instructions ran, in what order, and the code bytes — not
the contents of `rax` or of memory at each step. Register/memory state remains the
**emulator tier's** job (`emu_result_t` registers, memory watchpoints) or would
require a DBI memory-event mode / PTWRITE injection. The implementation likewise
does not provide data-flow/taint, and libipt block boundaries are not identical to
Unicorn/DynamoRIO basic blocks (a libipt block can span direct branches), so
cross-backend block parity needs a normalization step or a documented difference.

## Caveats and preconditions

- **The temporal correctness problem is fundamental.** Only time-aware capture
  (§2/§3) or replay (§4) is correct; a single snapshot silently corrupts on
  free/reuse. Treat any tool that ignores this as lossy.
- **Intel PT needs Intel bare-metal** (Broadwell+ / Goldmont+); it is absent on
  most cloud and GitHub-hosted VMs unless the host opts in. CoreSight needs
  specific bare-metal AArch64 boards.
- **Privilege.** `perf_event_paranoid` lowered (effectively `-1` for a
  default-size PT buffer) or `CAP_PERFMON`; the eBPF+uffd recorder needs
  `CAP_BPF` + `CAP_SYS_PTRACE` — all available under the "admin" assumption, but
  the process cannot grant them to itself.
- **PT bandwidth is high** (~100 MB/s encoded, ~10× decoded) even though CPU cost
  is low — snapshot/flight-recorder mode is the practical default.
- **The EPT→PT bridge (§3) is research-grade** — its components are production
  (DRAKVUF), the wiring is not.
- **Maturity flags from the research:** the eBPF-uprobe-on-`__jit_debug_register_code`
  technique is sound inference, not a documented recipe; `bpf_probe_read_user_dynptr`
  is very recent (verify kernel version); "JitFuzz" as a PT-JIT-reconstruction
  paper was not found; the EPT-capture→PT-decoder composition is "narrowly novel"
  (FlowJIT/libxdc are near prior art), not unprecedented.

## Relationship to asm-test

This extends the hardware-trace plan's Phase 1 (Intel PT / CoreSight) from "trace
code asm-test generated itself — bytes already known, the *saving grace*" to
"attach to a live **foreign** JIT and reconstruct its generated assembly." Three
components are new:

1. **Runtime attach to a PID** — `perf_event_open -p` against an existing process,
   rather than self-tracing `pid=0` around asm-test's own region.
2. **A time-aware code-image recorder** — jitdump-live or eBPF+uffd — that
   *replaces* the saving grace (asm-test no longer owns the bytes).
3. **Reuse of the existing Capstone layer** to render the recovered bytes — no new
   decoder API leaks into the bindings, consistent with the plan's report-format
   reuse.

The hardware-trace plan tracks this as its forward-looking **Phase 2**; this
document is its detailed treatment.

## When to use L3 call descent — and why it is hazardous on a live runtime

The out-of-process single-step tracer can optionally **descend** into the call-outs it
would otherwise step over (see [native-tracing.md](../../guides/tracing/native-tracing.md#call-descent-levels)).
Levels 0–2 are safe defaults: level 2 (`DESCEND_KNOWN`) only single-steps callees whose
region you named in an allow-set, so it never wanders into runtime internals. **Level 3
(`DESCEND_ALL`) is different, and default-off for a reason.** On a live managed runtime it is
**best-effort and expected to perturb — and it can deadlock the target**:

- **Cross-thread lock inversion is real and not fully mitigable.** The attach is single-tid,
  so sibling CoreCLR/JVM/V8 threads run free while the tracer single-steps one thread ~1000×
  slower than native. If the descended code holds a lock a sibling needs — a GC alloc
  slow-path, a JIT-compile helper, `_dl_runtime_resolve` under the loader lock, a malloc arena
  — the sibling stalls, and a per-thread watchdog cannot break a lock the sibling now spins on.
- **GC / re-JIT can move code out from under the stepper**, desyncing cached frame bytes and
  invalidating shadow-stack return addresses. Descent self-truncates (never crashes) on a
  PC/executable-mapping mismatch, but the trace past that point is lost.
- **Blocking syscalls park `waitpid` indefinitely** — the backend-owned `ITIMER_REAL`
  watchdog (SA_RESTART cleared → `EINTR`) is the only escape, and it terminates the descent,
  it doesn't resume it.

So L3's bounding — the default denylist (GC / JIT-compile / PLT `ld.so` resolver / blocking
libc), the conservative instruction budget, the watchdog — is **damage limitation, not a
guarantee**. Use L3 on a **safe-by-construction** target: the fork path (a controlled
single-threaded callee), a paused / single-threaded / post-mortem process, or a live runtime
whose sibling threads you have frozen (itself not free of deadlock risk if a frozen thread
holds a runtime lock). The guarded `jit_trace *-descend-all` demo lane exists to exercise this
path and is **expected to self-skip** (truncate — never hang, never corrupt) when it trips a
guard; it asserts the guards fire, not that L3 is transparent.

## Sources

Hardware trace & decode: [perf-intel-pt], [perf-inject], [jitdump spec],
[libipt howto], [libipt #57], [libxdc], [easyperf], [Jane St magic-trace],
[CoreSight], [OpenCSD].

Kernel capture primitives: [perf_event_open(2)], [TEXT_POKE series],
[userfaultfd], [PAGEMAP_SCAN], [soft-dirty], [Quarkslab uprobe], [ebpf verifier].

Runtime jitdump enablement: [dotnet diag IPC], [JVMTI], [V8 linux-perf],
[CPython perf], [GDB JIT].

Hypervisor / replay prior art: [Xen altp2m], [DRAKVUF IPT], [DRAKVUF codemon],
[LWN PT-VMX], [FlowJIT], [TTD], [rr].

[perf-intel-pt]: https://man7.org/linux/man-pages/man1/perf-intel-pt.1.html
[perf-inject]: https://man7.org/linux/man-pages/man1/perf-inject.1.html
[jitdump spec]: https://github.com/torvalds/linux/blob/master/tools/perf/Documentation/jitdump-specification.txt
[libipt howto]: https://github.com/intel/libipt/blob/master/doc/howto_libipt.md
[libipt #57]: https://github.com/intel/libipt/issues/57
[libxdc]: https://github.com/nyx-fuzz/libxdc/blob/master/readme.md
[easyperf]: https://easyperf.net/blog/2019/08/23/Intel-Processor-Trace
[Jane St magic-trace]: https://blog.janestreet.com/magic-trace/
[CoreSight]: https://docs.kernel.org/trace/coresight/coresight.html
[OpenCSD]: https://github.com/Linaro/OpenCSD
[perf_event_open(2)]: https://www.man7.org/linux/man-pages/man2/perf_event_open.2.html
[TEXT_POKE series]: https://www.spinics.net/lists/kernel/msg3465847.html
[userfaultfd]: https://docs.kernel.org/admin-guide/mm/userfaultfd.html
[PAGEMAP_SCAN]: https://man7.org/linux/man-pages/man2/PAGEMAP_SCAN.2const.html
[soft-dirty]: https://docs.kernel.org/admin-guide/mm/soft-dirty.html
[Quarkslab uprobe]: https://blog.quarkslab.com/defeating-ebpf-uprobe-monitoring.html
[ebpf verifier]: https://docs.ebpf.io/linux/concepts/verifier/
[dotnet diag IPC]: https://raw.githubusercontent.com/dotnet/diagnostics/main/documentation/design-docs/ipc-protocol.md
[JVMTI]: https://docs.oracle.com/en/java/javase/21/docs/specs/jvmti.html
[V8 linux-perf]: https://v8.dev/docs/linux-perf
[CPython perf]: https://docs.python.org/3/howto/perf_profiling.html
[GDB JIT]: https://sourceware.org/gdb/current/onlinedocs/gdb.html/Registering-Code.html
[Xen altp2m]: https://xenproject.org/blog/stealthy-monitoring-with-xen-altp2m/
[DRAKVUF IPT]: https://github.com/tklengyel/drakvuf/wiki/Intel-Processor-Trace
[DRAKVUF codemon]: https://raw.githubusercontent.com/tklengyel/drakvuf/main/src/plugins/codemon/codemon.cpp
[LWN PT-VMX]: https://lwn.net/Articles/749820/
[FlowJIT]: https://onlinelibrary.wiley.com/doi/full/10.1002/spe.2567
[TTD]: https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/time-travel-debugging-overview
[rr]: https://rr-project.org/
[src/disasm.c]: ../../../src/disasm.c
[Language runtime support]: ../plans/dynamorio-native-trace-plan.md#language-runtime-support
