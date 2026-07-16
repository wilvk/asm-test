# asm-test - data-flow tracing (L0 value trace / L1 def-use / L2 taint): implementation plan

A phased roadmap for adding **data-flow tracing** on top of the existing control-flow
substrate. Today every backend fills the shared
[`asmtest_trace_t`](../../../include/asmtest_trace.h#L44) — *which instructions ran*
(ordered offsets + basic blocks). This plan adds *how data moved*: a per-step **value
trace** (L0), a **def-use graph** (L1), and **taint / slicing** (L2), with the analysis
layers shared across every tier exactly as `asmtest_trace_t` is shared today.

This operationalizes the design investigation in
[analysis/data-flow-capture.md](../analysis/data-flow-capture.md) (and its 2026-07-12
adversarial cross-reference), which is the source of truth for the fidelity/overhead
trade-offs; this document is the build order. The two committed end-goals are **(a) real
live values on an attached, running process** (out-of-band ptrace tier) and **(b)
production-grade taint over live managed runtimes** (DynamoRIO tier) — so the plan carries
through the DynamoRIO and .NET-interpretability phases rather than stopping at the CI demo.

> Status legend (**reconciled 2026-07-16** — every phase re-verified against the code; the
> previous legend materially under-reported Phases 4 and 5):
>
> **Phases 0–3 *(LANDED 2026-07-12)*** — the shared spine + CI proving
> ground (`include/asmtest_valtrace.h`, `src/dataflow.c` / `src/dataflow_operands.c` /
> `src/dataflow_emu.c`, `make dataflow-test`, 53 checks; the Unicorn L0 producer validated
> live) and the scoped ptrace L0 live out-of-band producer, cross-validated against the
> emulator L0 oracle (RIP-relative EA, gs-base, XMM/YMM wide values, live SEIZE+detach).
> Phase 3 has since grown its two committed extensions, both complete: the **live-attach**
> producer + asmspy Data-flow view (all seven increments,
> [live-attach-dataflow-plan.md](../archive/plans/live-attach-dataflow-plan.md)) and the followup tier's
> **F1 block-step + emulator replay** and **F3 hardware data-watchpoint** modes
> ([live-attach-dataflow-followup-plan.md](live-attach-dataflow-followup-plan.md)).
>
> **Phase 4 (.NET interpretability): Increments 1–3 LANDED 2026-07-12/13; the `{old,new,len}`
> wiring LANDED 2026-07-16 as followup F4** — the pure host-independent PC→method+version
> resolver (`src/dataflow_method.c`), runtime-helper summary edges, and the GC-move *detection*
> feed (`GcMoveMap` from `GCBulkMovedObjectRanges`). The triple extraction's stated blocker
> ("needs an out-of-proc EventPipe/nettrace consumer") was an *assumption*, and it was disproved
> 2026-07-14: a native in-process `ICorProfilerCallback4::MovedReferences2` profiler delivers the
> exact triples at a fully-suspended-EE GC fence, was proven to coexist with DynamoRIO
> (`make docker-gcprofiler-probe`), and already ships on the DR side — taint Increment 7 wires it
> through a POSIX-shm handshake into `at_gc_remap_live`
> ([dataflow_dr_client_inlined.c:737](../../../src/dataflow_dr_client_inlined.c#L737)),
> remapping 60,021 real ranges on a live compacting GC. See
> [gc-move-range-extraction-findings.md](../analysis/gc-move-range-extraction-findings.md).
> **That feed is now pointed at the landed transform on a live attach.** The ptrace-tier
> canonicalizer `asmtest_gcmove_canonicalize`
> ([dataflow_gcmove.c:74](../../../src/dataflow_gcmove.c#L74)) — for which this legend previously
> recorded "no caller outside its unit test" — is driven by
> [gccanon_tracer.c:1285](../../../examples/gccanon_attach/gccanon_tracer.c#L1285) over an
> attach-mode profiler feed, with same-window multi-GC chaining. `make docker-gccanon-attach`,
> 37 assertions, negative control included. See
> [live-attach-dataflow-followup-plan.md](live-attach-dataflow-followup-plan.md) F4.
>
> **Phase 5 (DynamoRIO taint tier): ALL NINE INCREMENTS LANDED** *(1–3 2026-07-13; 4–9 through
> 2026-07-15)* — in-band tag propagation, the launch-under-DR container, GC-move shadow remap,
> SIMD taint, and the overhead work are **complete**, not planned. Production (record-free)
> propagation measures **~11× bare**, inside the target ~10–50× band, and is held there by a
> build-failing CI gate (`BAND_MAX = 50`, [mk/native-trace.mk:1292](../../../mk/native-trace.mk#L1292)).
> Details in the standalone [dynamorio-taint-tier-plan.md](../archive/plans/dynamorio-taint-tier-plan.md).
>
> **Phase 6 (bindings/docs/CI) LANDED 2026-07-13.**
>
> **Remaining implementable work across this plan: none** *(reconciled 2026-07-17)*. Phase 4's
> `{old,new,len}` wiring — the last open item this legend carried — landed 2026-07-16 as followup
> F4; Phases 3 and 5 are the two target tiers and both are delivered. Work that continues *near*
> this plan lives in the followup tier
> ([live-attach-dataflow-followup-plan.md](live-attach-dataflow-followup-plan.md)): F2, F5–F7 and
> F1's vector breadth, plus F4's optional extension to full object identity
> (`GCBulkType`/`Node`/`Edge`), which the address-identity model defers by design
> (`asmtest_valtrace.h`). Update this file as phases land, the way
> [dynamorio-native-trace-plan.md](../archive/plans/dynamorio-native-trace-plan.md) tracks its own.

---

## Goals and non-goals

**Goals**

- A shared L0 value-trace sink (`asmtest_valtrace_t`) with the same caller-owned-buffer,
  append/dedup/truncate discipline as `asmtest_trace_t`, so any tier can fill it and a
  value trace overflows honestly (`truncated`).
- Tier-neutral L1 (last-writer def-use) and L2 (forward/backward slice, taint) passes built
  **once**, over L0.
- L0 producers for three tiers: **emulator** (CI proving ground + oracle), **scoped ptrace
  stepper** (real live out-of-band values — goal a), **DynamoRIO** (production in-band taint
  over managed code — goal b).
- The .NET interpretability layer that makes managed taint meaningful: native-PC→method
  identity, GC-move address canonicalization, optional IL→native variable mapping.

**Non-goals**

- Whole-run value capture on the ptrace tier — it is scoped to a `using` region by design
  (cost is ~10³–10⁵×). Whole-process is the DynamoRIO tier's job.
- Hardware-only data flow — the silicon has no operand-value channel; only PTWRITE
  instrumentation, PEBS/IBS *statistical* address sampling, or PT-path + replay (deferred;
  see the analysis note).
- macOS / Windows for the capture tiers (ptrace/DR here are Linux-x86-64 first, as the rest
  of the stack is). EventPipe metadata is cross-platform and does not require Windows.

## Design overview

L0/L1/L2 is **fully greenfield** (`rg -i 'valtrace|def.?use|taint|dataflow'` → zero), but
the scaffolding each tier needs already exists:

| Piece | Already present | Gap |
|---|---|---|
| Operand decode | Capstone `CS_OPT_DETAIL` on; operand loop iterates `operands[]` ([disasm.c:375](../../../src/disasm.c#L375)) | reads only `X86_OP_IMM`; no read/write-set, no `.access`, no implicit operands |
| Emulator values | mem hook gets `(type,addr,size,value)` ([emu.c:145](../../../src/emu.c#L145)); `uc_reg_read` available | value discarded; hooks armed only under a watchpoint ([emu.c:388](../../../src/emu.c#L388)) |
| ptrace values | full `GETREGS` every step ([ptrace_backend.c:431](../../../src/ptrace_backend.c#L431)); `process_vm_readv` ([:315](../../../src/ptrace_backend.c#L315)) | extracts only rip/rax/rsp; no XMM/YMM; no per-operand mem peek; no `gs:` math |
| DR instrumentation | instruments registered ranges ([asmtest_drtrace.h:17](../../../include/asmtest_drtrace.h#L17)) | clean-call offset recorder, `drreg` excluded; no operand/value/taint; no launch container |
| Managed metadata | code-image recorder ([asmtest_codeimage.h](../../../include/asmtest_codeimage.h)); jitdump/MethodLoadVerbose addr-channel | no GC-move canonicalization; no var map |

**Address-space normalization contract (must be fixed in Phase 0).** The existing sink is
not address-uniform — [emu.c:113](../../../src/emu.c#L113) appends routine-relative
offsets, the single-step backend stores absolute in whole-window mode but offsets in region
mode. A value trace mixes an effective **memory** address (absolute) with instruction
offsets, so every `location` must carry its space tag and the L1 linker normalizes per
capture mode.

## Public API sketch

```c
/* asmtest_valtrace.h — the shared L0 sink (mirrors asmtest_trace.h discipline). */
typedef enum { AT_LOC_REG, AT_LOC_MEM_ABS, AT_LOC_MEM_OFF } at_loc_kind_t;

typedef struct {
    at_loc_kind_t kind;
    uint32_t      reg;       /* Capstone reg id when AT_LOC_REG (incl. seg for gs:/fs:) */
    uint64_t      addr;      /* absolute or routine-offset effective address (MEM)      */
    uint16_t      size;      /* width in bytes (up to 64 for AVX-512)                    */
    bool          is_write;  /* read-set vs write-set (from Capstone .access)            */
    uint64_t      value;     /* inline for <=8B; wider (XMM/YMM) spills to wide[]        */
} at_val_rec_t;

typedef struct asmtest_valtrace {
    uint64_t     *insn_off;  /* per-step instruction offset (parallels asmtest_trace_t)  */
    at_val_rec_t *recs;      /* flattened operand records, caller-owned                  */
    uint8_t      *wide;      /* side buffer for >8B values                               */
    size_t recs_cap, recs_len; uint64_t recs_total;
    bool     truncated;
    at_loc_kind_t mem_space; /* normalization contract for this capture                  */
} asmtest_valtrace_t;

void asmtest_valtrace_append(asmtest_valtrace_t *v, uint64_t off,
                             const at_val_rec_t *recs, size_t n);

/* Operand read/write-set enumerator — one persistent csh, detail on. */
size_t asmtest_operands(asmtest_arch_t arch, const uint8_t *code, size_t len,
                        uint64_t off, at_val_rec_t *reads, size_t *nreads,
                        at_val_rec_t *writes, size_t *nwrites);

/* L1 / L2 — tier-neutral passes over an L0 trace. */
asmtest_defuse_t *asmtest_defuse_build(const asmtest_valtrace_t *v);
asmtest_slice_t  *asmtest_slice_forward(const asmtest_defuse_t *g, at_val_rec_t seed);
asmtest_slice_t  *asmtest_slice_backward(const asmtest_defuse_t *g, at_val_rec_t sink);
```

## Phase 0 - Shared value-trace sink + operand enumerator *(LANDED 2026-07-12)*

The tier-neutral spine — highest leverage, mostly extension of existing code.

- Add `asmtest_valtrace_t` + `asmtest_valtrace_append` beside
  [`trace_append_insn`](../../../src/trace.c#L24) (same buffer/truncate discipline). Encode
  the **address-space tag** in every record.
- Extend the operand loop at [disasm.c:375](../../../src/disasm.c#L375) from IMM-only to the
  full read/write set: consume per-operand `.access` (`CS_AC_READ`/`WRITE`) for **direction**;
  `x86_op_mem{base,index,scale,disp,segment}` for effective addresses; `regs_read[]` /
  `regs_write[]` + `detail->x86.eflags` for **implicit** operands (`eflags`, `rsp` on
  push/pop/call/ret, string counters). Hold **one persistent `csh`** (not per-call
  `cs_open`/`cs_close`). Guard against Capstone DIET builds (no `.access`/`op_str`).
- Arm arms for x86-64 and arm64 (both already decode); ARM32/RISCV64 stubbed.

**Exit criteria:** unit test `test_operands.c` asserts the read/write-set (incl. implicit
`eflags`/`rsp` and a `gs:`-segmented mem operand) for a hand-picked instruction table on
both arches; `asmtest_valtrace_append` round-trips + truncates under a fixture; no per-op
`cs_open` in the hot path (grep gate).

## Phase 1 - L1 def-use + L2 slicer *(LANDED 2026-07-12)*

Pure passes, testable against synthetic L0 fixtures with **no producer** yet.

- **L1:** a last-writer map (`reg → step`; `mem-byte → step`) backed by a **hash/interval**
  structure (not the O(n) block-dedup scan). At each step, for every read location emit an
  edge `(producer_step → this_step)`. Register def-use is exact; memory def-use keys on the
  normalized address space and is flagged "GC-uncanonicalized" until Phase 4.
- **L2:** seed a taint set on chosen inputs → forward slice ("what X influences"); walk edges
  backward from a value → backward slice ("what produced this").

**Exit criteria:** `test_defuse.c` / `test_slice.c` reconstruct known def-use + forward/
backward slices over synthetic traces (incl. a load-after-store chain and a register move
chain); a documented alias case (two addresses that collide pre-canonicalization) is asserted
as a known limitation.

## Phase 2 - Emulator L0 producer (CI proving ground + oracle) *(LANDED 2026-07-12)*

Least-work producer; proves L0→L1→L2 end-to-end with no hardware; doubles as the reference
oracle for the live tiers.

- Read source regs via `uc_reg_read` in [`on_code`](../../../src/emu.c#L108) (fires
  pre-instruction → source state). Un-discard the value in
  [`on_mem_access`](../../../src/emu.c#L145); install mem hooks **unconditionally** (today
  only under a watchpoint, [emu.c:388](../../../src/emu.c#L388)). Loads need
  `UC_HOOK_MEM_READ_AFTER` (plain `READ` delivers value 0); stores use `UC_HOOK_MEM_WRITE`;
  capture destination regs at the next code hook.

**Exit criteria:** a self-contained routine's forward/backward slice matches a hand-derived
expectation in CI, no hardware; output is labelled **replay, not observation**; emulator L0
is wired as the cross-check oracle in the ptrace tier's tests.

## Phase 3 - Scoped ptrace L0 (real live values, out-of-band) — *goal (a)* *(LANDED 2026-07-12)*

The differentiated capability: real data flow on an **attached, running** process, bounded
to a `using` region.

- Per step, feed the sink from the **full** GP file (the `GETREGS` at
  [:431](../../../src/ptrace_backend.c#L431) already fetches it — extraction only). Add one
  `PTRACE_GETFPREGS` / `GETREGSET NT_X86_XSTATE` read for **XMM/YMM**.
- Decode read/write locations (Phase 0); for each mem operand compute EA `base+index*scale+disp`
  from the just-read regs, add **segment-base** resolution (`fs_base`/`gs_base` from `GETREGS`)
  for `gs:`-relative .NET TLS, then [`process_vm_readv`](../../../src/ptrace_backend.c#L315) the
  address; capture writes at the next stop.
- Bound cost with the scoped model (the call-descent machinery at
  [asmtest_ptrace.h:243](../../../include/asmtest_ptrace.h#L243) already frames the region).
- *Optional* Phase 3b: ptrace-snapshot → **emulator replay** for the value work off the live
  thread — valid only for **OS-interaction-free** regions (a syscall/futex/vDSO/signal in the
  region has no kernel under the emulator); anything else escalates to full input-capture
  record/replay. Nice-to-have, not on the critical path. **Realized 2026-07-15** as followup
  **F1 increment 1** ([live-attach-dataflow-followup-plan.md](live-attach-dataflow-followup-plan.md)) —
  the block-step + Unicorn-replay value tier `src/dataflow_blockstep.c`: purity-gated, byte-identical
  to single-step, ~6× fewer stops; impure regions fall back to single-step (F2 record-inject is the
  input-capture escalation).

**Exit criteria:** attaches to a live victim, captures a scoped region's value trace whose
slices match the emulator oracle on a deterministic region; XMM/YMM and a `gs:`-relative
access are covered; the target **survives detach** (reuse the crash-safe two-phase detach);
cost is documented per-region, not per-run.

## Phase 4 - .NET interpretability layer (managed taint prerequisite) *(Increments 1-3 LANDED; live GC-move DETECTION feed LANDED 2026-07-13; the `{old,new,len}` EXTRACTION blocker was disproved 2026-07-14 — the profiler route is proven and shipping on the DR side, so what remains is wiring it to the ptrace-side canonicalizer, followup F4)*

> **UPDATE 2026-07-14/16 — the deferral reason recorded here was wrong.** This phase deferred
> the concrete `{old, new, len}` extraction on the grounds that the in-proc `EventListener`
> does not surface the manifest `Values` struct-array, and therefore that an out-of-process
> EventPipe/nettrace consumer — "its own lift" — was required. The premise about
> `EventListener` is true; **the conclusion was not**. A deep-research investigation followed
> by a coexistence probe (GO, 2026-07-14) established a better in-process native mechanism
> that bypasses EventPipe entirely: an `ICorProfilerCallback4::MovedReferences2` profiler,
> which receives the exact per-range triples at a **fully-suspended-EE GC fence** — a stronger
> coherence position than any event-stream consumer, since no mutator can race the remap.
> It is not hypothetical: taint Increment 7 ships it, and it drives `at_gc_remap_live`
> ([dataflow_dr_client_inlined.c:737](../../../src/dataflow_dr_client_inlined.c#L737)) under
> DynamoRIO across 60,021 real move ranges. Findings:
> [gc-move-range-extraction-findings.md](../analysis/gc-move-range-extraction-findings.md).
>
> The residue is therefore narrow and named. The DR tier's shadow remap is **done**; the
> *ptrace* tier's pure transform `asmtest_gcmove_canonicalize`
> ([dataflow_gcmove.c:74](../../../src/dataflow_gcmove.c#L74)) is **landed and unit-tested but
> has no live caller** — its only callers are `examples/test_dataflow_gcmove.c`. Pointing the
> proven profiler feed at that transform is followup **F4**
> ([live-attach-dataflow-followup-plan.md](live-attach-dataflow-followup-plan.md)), and it is
> ordinary wiring work, not a research lift. Note the two tiers legitimately need different
> plumbing even from the same profiler: the DR client is in-process with the target (shm
> handshake, DR-API-free remap at the fence), whereas the ptrace tier is out-of-process and
> post-pass, so it consumes the triples stamped with a value-trace step boundary and runs the
> canonicalization over a captured trace before `asmtest_defuse_build`.

Raw L0 gives `rdx ← load @0x7f…`; managed taint needs method + object identity.

- **PC → method identity + version:** reuse the code-image recorder
  ([asmtest_codeimage.h](../../../include/asmtest_codeimage.h)) + the jitdump /
  `MethodLoadVerbose` (event id **143** on modern .NET) addr-channel to attribute values to
  the right method version across tiered re-JIT.
- **GC-move canonicalization (the hard one):** consume EventPipe/ETW
  `GCBulkMovedObjectRanges` (`OldRangeBase`/`NewRangeBase`/`RangeLength`) to remap the L1
  memory last-writer / taint shadow across compactions → `(object, field)` identity;
  `GCBulkType`/`Node`/`Edge` for full identity. EventPipe is the cross-platform feed (no
  Windows needed). — *Superseded as to mechanism: see the 2026-07-14/16 update above. The
  detection feed built this way landed, but the triples come from an in-process
  `MovedReferences2` profiler, not from EventPipe. This bullet is retained for provenance.*
- **Runtime-helper edges:** descend into allocation/write-barrier/generic-dict helpers (they
  are ordinary instrumented blocks) or model them as summary edges.
- *Optional:* IL→native variable map (debug info / PDB) to report `total depends on prices[i]`
  instead of registers.

**Exit criteria:** a managed value's def-use survives an induced GC compaction without
aliasing pre/post-move addresses; a value is attributed to the correct method+version after a
tiered re-JIT.

## Phase 5 - DynamoRIO production taint tier (whole-process, in-band) — *goal (b)* *(**ALL NINE INCREMENTS LANDED** — 1–3 2026-07-13, 4–9 through 2026-07-15; goal (b) delivered)*

> **UPDATE 2026-07-16 — this stub is reconciled; it previously stopped at Increment 3.** Every
> item the paragraph below called "the deferred remainder" has since landed, and the phase's
> exit criteria are met end-to-end. Increment 4 built the BSD 2-level shadow and the inline
> `dst_tag = ∪ src_tags` propagation plus the seed/sink ABI; Increment 5 the launch-under-DR
> container (`drrun -c … -- dotnet app.dll`), the POSIX-shm out-of-process validator, and a
> managed seed→sink over real JIT'd code; Increment 6 multi-range scoping + .NET method-load
> auto-registration; Increment 7 the GC-move shadow remap, live-wired from an in-process
> `MovedReferences2` profiler (this also disproved the Phase-4 blocker this stub used to cite
> as hard-gating it — see the Phase 4 update above); Increment 8 SIMD taint (XMM/SSE, then
> YMM/AVX); Increment 9 the overhead work. **Overhead, measured in-repo** (`dr-taint-overhead-test`):
> DR baseline ≈1× bare, full taint ≈437–452×, production (value-trace-free) ≈216×, and
> **record-free production propagation ≈11× bare — inside the target ~10–50× band**, held by a
> build-failing gate (`BAND_MAX = 50`, [mk/native-trace.mk:1292](../../../mk/native-trace.mk#L1292)).
> The decisive finding was that the cost was never the taint: per-instruction *value capture*
> (oracle-validation machinery) and the `drx_buf` record skeleton dominated, and propagation
> itself adds only ~1.4×. Deliberate deferrals, documented in-code: ZMM upper lanes, VSIB
> gather effective addresses, lane-precise sub-register flow. The direct-mapped shadow
> ("lever 2") was **not** built and is no longer required — eliminating the production record
> overshot the band target on its own.

The largest lift — production-grade taint over live JIT'd managed code — is a re-architecture of
the current tier, not an extension, and has its own standalone plan:
**[dynamorio-taint-tier-plan.md](../archive/plans/dynamorio-taint-tier-plan.md)**.

Increment 1 (the in-band L0 VALUE producer `libasmtest_drval_client.so`, a clean-call
per-instruction register/memory value snapshot cross-validated against the emulator oracle,
[dataflow_dr_client.c](../../../src/dataflow_dr_client.c)) **LANDED 2026-07-13**. Increment 2 (the
extension-load probe [drclient/probe_extensions.c](../../../drclient/probe_extensions.c)) **LANDED
2026-07-13** and settled the gating question: the prebuilt `drmgr`/`drreg`/`drx` load under DR's
private loader on glibc 2.39 (blocker does not reproduce → **option (c) version-pin**), and the
license resolved to a **split** — `drmgr`/`drreg`/`drx` are BSD, but **`umbra` is LGPL-2.1** (Dr.
Memory Framework), so the byte-granular shadow must hand-roll a BSD map or explicitly accept LGPL
([dr-extension-load-probe-findings.md](../analysis/dr-extension-load-probe-findings.md)). Increment
3 (CORE) then re-platformed the value client onto inlined `drmgr`/`drreg`/`drx_buf` instrumentation
([dataflow_dr_client_inlined.c](../../../src/dataflow_dr_client_inlined.c)), oracle-validated
identically to the clean-call client (`make dr-valtrace-inlined-test`). What this paragraph used
to call "the deferred remainder" — in-band tag propagation (`dst_tag = ∪ src_tags`, the ~10–50×
band), the launch-under-DR container (`drrun -c … -- dotnet app.dll`), the shadow remap on GC
moves, whole-process breadth, and SIMD taint — was decomposed into Increments 4–9 there and **has
since landed in full** (see the 2026-07-16 update above). The one causal claim worth correcting
rather than just re-dating: the GC-move remap was described here as "hard-gated on the deferred
Phase 4 `{old,new,len}` extraction," and it was not — Increment 7 sourced the triples from an
in-process `MovedReferences2` profiler instead, which is also what disproved the Phase-4 deferral.
The signal half was already mitigated (`DR_SIGNAL_DELIVER`); external attach stays out of scope
for managed, and that boundary held — the DR attach tier's Increment 6 probed managed attach
twice (external seize, then a safepoint-parked variant) and closed it **NO-GO** both times, the
fatal takeover being the runtime's *native* threads
([dynamorio-managed-attach-safepoint-plan.md](../archive/plans/dynamorio-managed-attach-safepoint-plan.md)).

**Exit criteria — MET 2026-07-15.** A launched `dotnet` workload runs under DR with a taint seed
at a source detected at a sink over real JIT'd managed code (Increment 5); taint survives a
compacting GC (Increment 7, live-wired); overhead is measured on a representative workload and
band-gated in CI at ~11× bare (Increment 9); it validates well beyond the offset-only dotnet
smoke (`bindings/dotnet/drtrace/`). See
[dynamorio-taint-tier-plan.md](../archive/plans/dynamorio-taint-tier-plan.md) for the full increment sequence.

## Phase 6 - Bindings, docs, CI *(LANDED 2026-07-13: libasmtest_dataflow shared lib; ALL 10 language bindings (Python/C++/Node/Ruby/Lua/Zig/Rust/Go/Java/.NET) wrap the pure gcmove+method helpers, each `make dataflow-<lang>-test`-validated (host or docker); Python/C++/Node also wrap the full L0/L1/L2 ValueTrace pipeline; data-flow guide page; `dataflow` CI job gates python+cpp, and a `dataflow-bindings` matrix job gates the other seven in their pinned toolchain images. Phase 6 fully landed — only hardware/upstream/credential-gated forward-look items remain across the plan set)*

- Wrap `asmtest_valtrace_t` + slice accessors in the dynamic-FFI bindings (opaque-handle
  pattern, as `asmtest_trace_t` is wrapped).
- User guide under `docs/guides/tracing/`; promote the relevant parts of the analysis note.
- CI: the emulator L0 slice test (no hardware) on every run; ptrace/DR tiers gated like the
  other native tiers; assert the crash-safe detach survivor in Phase 3.

## Risks and open points

- **Scale.** A value trace is far larger than a control trace (several operands × up to 64B
  vs one 8B offset) — every producer needs ring buffers / streaming / a bounded scope.
- **SIMD data flow is expensive and under-solved** industry-wide (even libdft skips XMM/SSE);
  the XMM/YMM path (`NT_X86_XSTATE`) is real work, not a checkbox.
- **GC-move canonicalization is runtime-coupled** and forces managed L1 to depend on EventPipe
  timing; a missed move event silently aliases. Needs a coherence check.
- **DR-over-managed is heavy engineering** and launch-only; the `drreg`/`umbra` rebuild is a
  substantial departure from the current raw-core-API client.
- **Single-stepping a live JIT can still crash it** (inherent; the scoped ptrace tier inherits
  the asmspy hazard) — prefer the scoped model and the crash-safe detach.

## Recommended first milestone

**Phases 0–2**: the shared spine (`asmtest_valtrace_t` + operand enumerator), the L1/L2
passes, and the emulator producer — a deterministic, CI-runnable, no-hardware demonstration
of L0→L1→L2 with the emulator as oracle. This is common to both end-goals, de-risks the
analysis layers before any expensive capture work, and gates Phases 3 (live ptrace) and 5
(DR managed taint).
