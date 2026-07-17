# asm-test - DynamoRIO production taint tier (whole-process, in-band managed taint): implementation plan

This is the standalone child plan for **goal (b)** of the data-flow effort — production-grade
taint over live JIT'd managed (.NET) code under DynamoRIO. It splits out of Phase 5 of the
parent [data-flow-tracing-plan.md](data-flow-tracing-plan.md), whose Phase-5 section is now a
pointer stub retaining only the landed-Increment-1 status; the detail lives here. The
source-of-truth analysis note for every overhead figure, launch-vs-attach decision, and
integration-model claim below is [data-flow-capture.md](../../analysis/data-flow-capture.md); the
standalone-DynamoRIO-plan precedent this file's shape follows is
[dynamorio-native-trace-plan.md](dynamorio-native-trace-plan.md).

The shipped tier (Increment 1, **LANDED 2026-07-13**) is an in-process, clean-call **L0 VALUE**
producer: `libasmtest_drval_client.so` records, per instrumented instruction, the GP register
file (`dr_get_mcontext`) plus each explicit memory SOURCE operand's effective address/value
(`decode` + `opnd_compute_address` + `dr_safe_read`) into an app-owned `at_drval_t`,
cross-validated against the emulator oracle
([dataflow_dr_client.c:1](../../../../src/dataflow_dr_client.c#L1)). This plan does **not** extend
that client — it *re-platforms* it. The spine of the remaining work is a departure from
DynamoRIO's raw BSD **core API** onto its standard **extension stack**
(`drmgr`/`drreg`/`umbra`/`drx_buf`) with inlined instrumentation, which the client header itself
names as "the Phase-5 END goal" ([dataflow_dr_client.c:20](../../../../src/dataflow_dr_client.c#L20)).
Everything else — in-band tag propagation, the launch-under-DR container, GC-move shadow remap,
whole-process breadth, SIMD taint — hangs off that re-platform. The whole point is the overhead
band: an inlined+buffered DBI client sits at the **greenfield ~10–50×** versus ptrace
single-step's **~10³–10⁵×** ([data-flow-capture.md:180](../../analysis/data-flow-capture.md#L180)),
so this is the only substrate on which whole-process managed taint is affordable at all.

The plan is written **exit-criteria-first**: each increment is framed by the committed
end-state — *a seed at a source is detected at a sink over real JIT'd managed code, taint
survives a GC, and overhead is measured against a budget* — and works backward to the smallest
buildable, docker-CI-checkable slice, with the emulator L0 oracle
([dataflow_emu.c](../../../../src/dataflow_emu.c), the reference oracle for the live capture tiers)
as the standing cross-validation reference at every step. Each increment also carries an explicit
**Effort.** sizing (relative, not absolute) so the eight-increment sequence can be scheduled — the
re-architecting-tier precedent [dynamorio-native-trace-plan.md](dynamorio-native-trace-plan.md)
closes each phase the same way.

> Status legend: **Increment 1 — in-band L0 VALUE producer** *(LANDED 2026-07-13)* — the
> clean-call per-instruction register/memory value snapshot
> ([dataflow_dr_client.c](../../../../src/dataflow_dr_client.c#L1), capture ABI
> [dataflow_dr.h](../../../../src/dataflow_dr.h#L87)), cross-validated against the emulator oracle
> ([dataflow_emu.c](../../../../src/dataflow_emu.c)), driven in-process by the `dr_valtrace` C
> harness. **Increment 2 — extension-load probe** *(LANDED 2026-07-13)* — the prebuilt
> `drmgr`/`drreg`/`drx` (the `drx_buf` API lives in `drx.h`) load cleanly under DR's private
> loader on glibc 2.39 with the pinned DR 11.91.20630; the documented blocker does **not**
> reproduce, so **option (c) version-pin** is chosen — no build-from-source/static-link owed.
> The license question resolved to a **split**: `drmgr`/`drreg`/`drx` are BSD (DR core), but
> **`umbra` is LGPL-2.1** (it ships in the Dr. Memory Framework, not DR core — only `drfuzz`/
> `drltrace` are its BSD carve-outs), contradicting the plan's permissive-umbra assumption.
> Probe [drclient/probe_extensions.c](../../../../drclient/probe_extensions.c); findings
> [dr-extension-load-probe-findings.md](../../analysis/dr-extension-load-probe-findings.md); gate
> `make docker-drext-probe` + CI `drext-probe`. **Increment 3 — inlined L0 value client (CORE)**
> *(LANDED 2026-07-13)* — [dataflow_dr_client_inlined.c](../../../../src/dataflow_dr_client_inlined.c)
> re-platforms the clean-call recorder onto `drmgr`/`drreg`/`drx_buf` and passes the emulator-oracle
> cross-check identically to the clean-call client (`make dr-valtrace-inlined-test`, wired into
> `drtrace-test`); `rflags`/dead-register slots are documented clean-call-only divergences; the
> `make dr-valtrace-bench` microbenchmark shows a ~2.6× per-instruction capture-cost drop.
> **Increments 4–9** *(**ALL LANDED** 2026-07-13/14)* — 4 in-band tag propagation
> (`dst_tag = ∪ src_tags`) + shadow-concurrency policy + seed/sink API *(**COMPLETE 2026-07-13** —
> hand-rolled BSD 2-level shadow, per-thread reg tags, inline union/broadcast, create-on-touch
> stores, per-byte memory-tag union, the guarded-inline sink skip, and all three sink kinds:
> branch-condition (1), mem-len (0), call-arg (2))*; 5 launch-under-DR
> container (`drrun -c … -- dotnet app.dll`) + out-of-process oracle-diff validator
> *(**COMPLETE 2026-07-14** — native launch + shm + validator, dotnet tiered-JIT coexistence,
> concurrent-writer stress, managed seed→sink over real JIT'd code)*; 6
> whole-process breadth + method-range scoping *(**COMPLETE 2026-07-14** — native multi-range
> set + boundary policy + inscount cost bound, dotnet method-load auto-registration)*;
> 7 GC-move shadow remap *(**COMPLETE 2026-07-14** —
> profiler-fed live remap + full seed→move→sink survival via a DR-API-free raw-mmap leaf allocator)*;
> 8 XMM/YMM SIMD taint *(**COMPLETE 2026-07-14** — XMM/SSE + YMM/AVX register + memory taint, a
> shared Capstone AVX-store-access oracle fix, and the SIMD-vs-scalar overhead delta; ZMM/VSIB/
> lane-precise deferred)*; 9
> managed seed→sink validation + measured overhead + CI *(**COMPLETE 2026-07-14** — record-free
> production propagation put production at ~11× bare (IN the ~10-50× band), the HARD BAND GATE + the
> GC-survival CI job landed; all exit criteria MET — the plan is ready to archive)*. Update this file as increments land, the
> way [dynamorio-native-trace-plan.md](dynamorio-native-trace-plan.md) tracks its
> own; keep the parent [data-flow-tracing-plan.md](data-flow-tracing-plan.md) Phase-5 stub's
> status tag in sync with this child.

---

## Goals and non-goals

**Goals**

- Re-platform the DynamoRIO data-flow tier off the per-instruction clean-call recorder
  ([dataflow_dr_client.c:173](../../../../src/dataflow_dr_client.c#L173)) onto the standard
  extension stack — `drmgr` phased pass ordering, `drreg` scratch-register/flag reservation,
  `umbra` byte-granular shadow memory, `drx_buf` buffered per-thread trace — i.e. the
  drcachesim/memtrace idiom plus operand values plus inline tag propagation
  (`dst_tag = ∪ src_tags`), targeting the realistic **~10–50×** DBI band
  ([data-flow-capture.md:194](../../analysis/data-flow-capture.md#L194)). This stack is named as
  the Phase-5 END goal in the client header itself
  ([dataflow_dr_client.c:20](../../../../src/dataflow_dr_client.c#L20)).
- **Defeat the private-loader/glibc blocker first.** The whole re-platform is gated on getting
  the prebuilt release extensions to load under DR's private loader (or side-stepping it) — the
  single documented reason the tier stays on raw core API today
  ([drclient/CMakeLists.txt:19](../../../../drclient/CMakeLists.txt#L19),
  [dataflow_dr_client.c:16](../../../../src/dataflow_dr_client.c#L16)).
- Ship in-band L2 taint: a per-byte tag shadow propagated inline as `dst_tag = ∪ src_tags`, with
  a **process-global shadow concurrency policy** for the multithreaded managed runtime and a
  managed-facing **seed/sink** surface (a taint source at e.g. a `read()` buffer, a sink at a
  `memcpy` length or a branch condition).
- Stand up a **launch-under-DR container** (`drrun -c libasmtest_drval_client.so -- dotnet app.dll`)
  — a genuinely new in-tree integration path; no `drrun` launcher exists today
  ([data-flow-capture.md:203](../../analysis/data-flow-capture.md#L203)) — together with the
  **out-of-process oracle-diff validator** that consumes its results channel.
- Make taint **survive .NET GC compaction** via `umbra`-shadow remap on GC-move events, and
  extend to **whole-process breadth** beyond the single registered region, scoped to registered
  method ranges to bound cost.
- A managed **seed→sink end-to-end validation** over real JIT'd .NET code with **overhead
  measured against an explicit budget** on a representative workload, wired into Docker + CI —
  the first overhead number on this repo (all cited figures are external literature).
- Stay **LGPL-clean**: the target stack does not need `drwrap`, so the tier stays on BSD DR core
  + permissive extensions (see License posture, below).

**Non-goals**

- **External attach** (`drrun -attach` / `dr_inject_*`) over managed processes. Rejected for
  three cited reasons: attach freezes the runtime at an arbitrary state (threads in syscalls, on
  runtime locks, mid-GC, deep in JIT'd code), the arbitrary-state half is not mitigable the way
  the signal half is, and a clean managed attach needs research-grade safepoint coordination
  ([data-flow-capture.md:226](../../analysis/data-flow-capture.md#L226)). Attach also means
  abandoning the cooperative `dr_app_*` model for the `dr_inject` injector — a re-architecture,
  not a flag. Launch owns the process from a clean start.
- Pulling **`drwrap`** back in for function wrapping. The current client already does its own
  marker/arg resolution via `dr_get_proc_address` + a SysV-arg clean call
  ([dataflow_dr_client.c:64](../../../../src/dataflow_dr_client.c#L64),
  [:156](../../../../src/dataflow_dr_client.c#L156)); the re-platform must preserve that, never
  adopt `drwrap_wrap`/`drwrap_get_arg`.
- **Re-attach cycling.** One DR lifecycle per process stays the contract — the header flags
  in-process re-attach as unreliable
  ([asmtest_drtrace.h:86](../../../../include/asmtest_drtrace.h#L86)); launch-under-DR fits it.
- macOS / arm64 / Windows. Linux x86-64 only, matching the pinned DR image
  (`DR_VERSION=11.91.20630`, [Dockerfile.drtrace:35](../../../../Dockerfile.drtrace#L35)).
- Any claim of the ~10–50× band for SIMD-heavy code until Increment 8 measures it — even libdft's
  low numbers *skip* XMM/SSE/MMX (a coverage tradeoff), so the band may not hold once `umbra`
  covers vector state ([data-flow-capture.md:356](../../analysis/data-flow-capture.md#L356)).
- Replacing the shipped ptrace L0 tier or the emulator oracle — those remain the correctness
  references, not competitors.

---

## Design overview

| Piece | Already present | Gap |
|---|---|---|
| Instrumentation model | Per-instruction **clean call** `dr_insert_clean_call(…, on_step, …)` inserted before every in-range instr ([dataflow_dr_client.c:173](../../../../src/dataflow_dr_client.c#L173)) on raw `dr_register_bb_event` ([:227](../../../../src/dataflow_dr_client.c#L227)) | Replace with **inlined** instrumentation under `drmgr` pass ordering + `drreg` scratch regs/flags; the clean call per instr is precisely what makes the shipped increment slow ([data-flow-capture.md:198](../../analysis/data-flow-capture.md#L198)) |
| BB event | Raw `dr_register_bb_event(event_bb)` ([dataflow_dr_client.c:227](../../../../src/dataflow_dr_client.c#L227)); `event_bb` at [:149](../../../../src/dataflow_dr_client.c#L149) | `drmgr` phased events (app2app / analysis / insertion) so value capture + tag propagation compose as separate, ordered passes |
| Extension stack | **None** — raw BSD core API only, by policy ([drclient/CMakeLists.txt:19-21](../../../../drclient/CMakeLists.txt#L19)) | `drmgr`+`drreg`+`umbra`+`drx_buf`, blocked today by the private-loader/glibc load failure — Increment 2 must defeat it |
| Value capture | Register file (`dr_get_mcontext`, [:89](../../../../src/dataflow_dr_client.c#L89)) + explicit memory **source** EA/value (`decode`+`opnd_compute_address` [:122](../../../../src/dataflow_dr_client.c#L122)+`dr_safe_read`) into app-owned `at_drval_t` ([dataflow_dr.h:87](../../../../src/dataflow_dr.h#L87)) | `drx_buf` buffered per-thread flush to amortize the store cost (the memtrace pattern) |
| Taint / shadow | None (values only) | `umbra` byte-granular tag shadow, one `at_tag_t` per app byte; inline `dst_tag = ∪ src_tags`; seed/sink surface. The shadow is **process-global**, written concurrently by every instrumented thread — tolerated-benign-race byte-store policy, set in Increment 4 |
| Operand coverage | Explicit memory **source** loads; far/segmented `fs:`/`gs:`, VSIB vector-gather EA, and store (post-instruction) values all skipped ([dataflow_dr_client.c:104-107](../../../../src/dataflow_dr_client.c#L104)) | Store **tags** (a store's *location* already enters def-use even though its value is skipped, [dataflow_dr.h:60-62](../../../../src/dataflow_dr.h#L60)); then SIMD lanes (Increment 8) |
| Region registration | One region, learned from an app-emitted marker's SysV args `rdi/rsi/rdx` ([on_marker :64](../../../../src/dataflow_dr_client.c#L64), re-instrument via `dr_delay_flush_region` [:70](../../../../src/dataflow_dr_client.c#L70), resolved by PC [:188](../../../../src/dataflow_dr_client.c#L188)) | Whole-process breadth scoped to **registered method ranges**; a launched `dotnet app.dll` never calls the C-harness marker, so a new registration mechanism is needed |
| Integration model | In-process cooperative `dr_app_*`; header flags in-process re-attach as unreliable ([asmtest_drtrace.h:86](../../../../include/asmtest_drtrace.h#L86)); driven by the self-init harness `dr_valtrace` via `ASMTEST_DRVAL_CLIENT` ([native-trace.mk:201](../../../../mk/native-trace.mk#L201)) | **Launch-under-DR** container over a live `dotnet` — no `drrun` launcher in-tree; plus an out-of-process validator to consume the results channel |
| Signal coexistence | **Mitigated** — `event_signal` → `DR_SIGNAL_DELIVER` in both clients ([dataflow_dr_client.c:213-217](../../../../src/dataflow_dr_client.c#L213); rationale in the control client [drtrace_client.c:386](../../../../src/drtrace_client.c#L386)) so .NET null-check `SIGSEGV` reaches the runtime | Carries over unchanged to launch; the **code-cache/JIT-collision half** is only *asserted* solved by launch, not demonstrated — a hypothesis first tested in Increment 5 |
| GC-move survival | Phase-4 `GcMoveMap` captures `GCBulkMovedObjectRanges` (DETECTION landed 2026-07-13) | **DONE (Increment 7, 2026-07-14)** — the in-process `MovedReferences2` profiler feeds exact `{old,new,len}` ranges to `at_gc_remap_live` at the GC fence; a DR-API-free raw-mmap leaf allocator carries the tag across never-touched destination leaves, so a seed survives a compacting GC end-to-end (`dr-taint-gcmove-survival-test`) |
| Oracle-diff validation | In-process `dr_valtrace` replays `at_drval_t` through the shared spine and diffs the emulator oracle in the same address space | Under launch the client no longer hosts the diff — a **separate app-side validator** must drain the shm channel and run the replay/diff out-of-process (Increment 5) |
| Overhead evidence | None on this repo's managed workload; all figures external literature | Increment 9 must measure on a representative `dotnet` workload against an explicit budget |

**Contract.** The re-platform keeps the *capture ABI* (`at_drval_t`,
[dataflow_dr.h:87-96](../../../../src/dataflow_dr.h#L87)) and the app-side replay /
cross-validation path stable, and changes only *how* the client fills it (inlined + buffered
instead of clean-call append) plus *adds* the taint shadow and seed/sink surface. Increment 3
must reproduce `at_drval_t` **byte-identically** and be regression-gated against the emulator
oracle ([dataflow_emu.c](../../../../src/dataflow_emu.c)) *before* any taint semantics land; taint
is an **additive** shadow surface, not a change to the value record. Because the in-band capture
path feeds the same record-building code as the scoped ptrace producer
([dataflow_ptrace.c](../../../../src/dataflow_ptrace.c)) and the emulator oracle, every increment
ships with an oracle-diff gate so the inlined/taint path never silently diverges from the
offline slice ([dataflow.c](../../../../src/dataflow.c), the shared L1/L2 spine). Through Increment 4
that diff runs **in-process** (the `dr_valtrace` harness replays and diffs in the same address
space); from Increment 5 the diff *consumer* moves **out-of-process** — a separate app-side
validator drains the shared-memory results channel and runs the same replay/diff — but the gate
itself is unchanged.

**Target stack — each component replaces one hand-rolled thing.** (1) **`drmgr`** replaces the
raw `dr_register_bb_event` ([:227](../../../../src/dataflow_dr_client.c#L227)) with drmgr's *phased*
instrumentation event (app2app / analysis / insertion pass ordering) so value capture and tag
propagation compose as separate, ordered passes instead of one monolithic `event_bb`.
(2) **`drreg`** reserves scratch GP registers and arithmetic flags so the value/tag work is
emitted as **inline** machine instructions rather than a full clean call per instruction — the
clean call is exactly the cost the shipped increment pays and the re-platform removes.
(3) **`umbra`** provides the byte-granular shadow holding one `at_tag_t` per application byte,
propagated inline as `dst_tag = ∪ src_tags`. (4) **`drx_buf`** provides a buffered per-thread
trace that batches emitted operand-value/address/tag records and flushes periodically — the same
pattern drcachesim/memtrace use to amortize the store cost.

**License posture** *(RESOLVED by Increment 2 — see
[dr-extension-load-probe-findings.md](../../analysis/dr-extension-load-probe-findings.md)).* The
picture is a **split**, not the uniform "avoid only `drwrap`" the plan first assumed:
- `drmgr`, `drreg`, and `drx` (which is where the `drx_buf` trace-buffer API lives — there is
  **no** separate `drx_buf` extension) are DR-**core** `ext/` extensions covered by DR's primary
  **BSD** license. The re-platform (Increment 3) can adopt them and stay LGPL-clean, **as long as
  the client keeps doing its own PC-resolved marker/arg resolution**
  ([:64](../../../../src/dataflow_dr_client.c#L64), [:156](../../../../src/dataflow_dr_client.c#L156))
  rather than `drwrap_wrap`/`drwrap_get_arg`.
- **`umbra` is LGPL-2.1** — it is **not** a DR-core extension; it ships under `drmemory/drmf/`
  as part of the Dr. Memory Framework (primary license LGPL; only `drfuzz`/`drltrace` are BSD
  carve-outs, `umbra` is not), and `umbra.h` carries the LGPL-2.1 header. So `umbra` sits with
  `drwrap`, not with the BSD stack. The earlier "`drmgr`/`drreg`/`umbra`/`drx_buf` are not
  `drwrap` → all LGPL-clean" reasoning was **wrong for `umbra`**.

Consequence: the byte-granular tag shadow (Increments 4/7) must either **hand-roll a BSD
direct-mapped shadow** (DR-core `dr_raw_mem_alloc` + a scale-down map) — *recommended*, keeps the
tier fully BSD — or **accept LGPL-2.1 for `umbra`** (dynamic-link relink obligation; static-link
inherits the stricter form, [dynamorio-native-trace-plan.md:914](dynamorio-native-trace-plan.md#L914)).
Increment 4 now carries that decision explicitly instead of assuming permissive `umbra`.

---

## Public API / capture-ABI sketch

The value capture ABI already exists and is unchanged: `at_drval_t` / `at_vstep_t` / `at_vmem_t`
in [dataflow_dr.h:63-96](../../../../src/dataflow_dr.h#L63), plus the marker symbol
`AT_DRVAL_MARKER_SYM` ([dataflow_dr.h:33](../../../../src/dataflow_dr.h#L33)). Taint adds a **new
seed/sink surface**. Everything below is **proposed** (new header
`include/asmtest_taint.h`, **does not exist yet**) and, like `dataflow_dr.h`, must stay
`<stdint.h>`/`<stddef.h>`-only so the client can include it alongside `dr_api.h` — no
`<stdbool.h>`, whose `bool` clashes with DynamoRIO's own
([dataflow_dr.h:20](../../../../src/dataflow_dr.h#L20)).

```c
/* PROPOSED — include/asmtest_taint.h (does not exist yet). Byte-granular tag-shadow +
 * seed/sink surface for the re-platformed DR taint client, shared by the DR client and
 * the app-side driver. Mirrors the dependency-free discipline of dataflow_dr.h. */

/* Tag width. Start with a 1-byte union-of-sources tag per app byte (bit0 = tainted;
 * remaining bits reserved for a small source-id set / color) so the umbra shadow is a
 * straight 1:1 byte map — the cheapest scale before widening to a multi-color bitset.
 * Monotone within a seed epoch: a union tag only ever GAINS bits, never loses them,
 * which is what makes the tolerated-benign-race write policy (Increment 4) sound. */
typedef uint8_t at_tag_t;                    /* PROPOSED */
#define AT_TAG_CLEAN 0u

/* App-emitted markers the client resolves by PC (dr_get_proc_address), exactly as the
 * value client resolves AT_DRVAL_MARKER_SYM today (dataflow_dr_client.c:188). NO drwrap
 * — the client reads SysV arg registers at the marker PC itself. Under launch-under-DR
 * the seed/sink config instead arrives via drrun client options (Increment 5). */
#define AT_TAINT_SEED_SYM "asmtest_dr_taint_seed_marker"   /* PROPOSED  rdi=base rsi=len rdx=at_tag_t */
#define AT_TAINT_SINK_SYM "asmtest_dr_taint_sink_marker"   /* PROPOSED  rdi=at_taint_report_t* */

/* A seeded source: [base, base+len) bytes get `color` in the umbra shadow at seed time. */
typedef struct at_taint_seed {               /* PROPOSED */
    uint64_t base;
    uint64_t len;
    at_tag_t color;
    uint8_t  pad[7];
} at_taint_seed_t;

/* A sink: when a tainted value reaches a watched sink operand (a memcpy length arg, a
 * branch condition), the client appends one record. `off` is the sink instruction's
 * region offset; `tag` is the union that arrived; `depth` counts seed->sink propagation
 * steps so the app-side oracle diff can compare the taint graph to the offline slice. */
typedef struct at_taint_hit {                /* PROPOSED */
    uint64_t off;      /* sink instruction offset within a registered range */
    uint64_t ea;       /* sink operand effective address (0 for reg/branch) */
    uint64_t seed_off; /* where the arriving tag was first seeded */
    at_tag_t tag;      /* union tag observed at the sink */
    uint8_t  kind;     /* 0 = mem-len arg, 1 = branch cond, 2 = call arg ... */
    uint32_t depth;    /* propagation steps seed->sink (for the oracle diff) */
} at_taint_hit_t;

/* App-owned report buffer the client fills in place — same append/truncate discipline
 * as at_drval_t (fixed cap, `truncated` on overflow, *_total keeps counting). Under
 * launch-under-DR this is NOT a same-address-space pointer; it is backed by a POSIX
 * shared-memory segment named via a drrun client option (Increment 5), drained by a
 * separate app-side validator process. */
typedef struct at_taint_report {             /* PROPOSED */
    at_taint_hit_t *hits;
    size_t hits_cap;
    size_t hits_len;
    uint64_t hits_total;
    uint8_t truncated;
} at_taint_report_t;
```

Marker/argument resolution stays **`drwrap`-free**: seeds/sinks arm by PC via
`dr_get_proc_address` ([dataflow_dr_client.c:188](../../../../src/dataflow_dr_client.c#L188)) + a
`drmgr` insertion-phase equivalent of today's `on_marker`/`on_step` SysV-arg clean call
([:64](../../../../src/dataflow_dr_client.c#L64),
[:156](../../../../src/dataflow_dr_client.c#L156)) — never `drwrap_wrap`/`drwrap_get_arg`.

---

## Increment 2 - Defeat the private-loader blocker (extension-load probe) *(LANDED 2026-07-13)*

**Outcome: the blocker does not reproduce.** On glibc 2.39 (Ubuntu 24.04) with the pinned DR
11.91.20630, the prebuilt `drmgr`/`drreg`/`drx` load cleanly under the private loader
(130588 instructions instrumented over `/bin/true`), so **option (c) version-pin** is chosen — no
build-from-source or static-link is owed. The `__memcpy_chk` symptom did not recur. The license
question resolved to a **split**: `drmgr`/`drreg`/`drx` are BSD, but **`umbra` is LGPL-2.1** (Dr.
Memory Framework), which reshapes Increment 4's shadow-memory choice (see License posture above).
Full record: [dr-extension-load-probe-findings.md](../../analysis/dr-extension-load-probe-findings.md).
The subsections below are the as-planned scope, retained for provenance.

The foundation. Nothing downstream can load `drmgr`/`drreg`/`drx` (`drx_buf`) until the
private-loader/glibc failure is empirically characterized and one fix chosen. This is a
throwaway, pure-docker slice — no hardware, no dotnet, **zero risk to the shipped
Increment-1 tier**, which stays on the raw core API untouched until Increment 3 regression-proves
its replacement.

- Build a throwaway probe client *(proposed `drclient/probe_extensions.c`)* that
  `use_DynamoRIO_extension()`s each of `drmgr`, `drreg`, `umbra`, `drx_buf` (explicitly **not**
  `drwrap`) and calls one API from each, running a trivial `/bin/true`-class inscount workload
  in the in-process DR smoke image `Dockerfile.drtrace`, under the DR release it pins
  (`DR_VERSION=11.91.20630`, [Dockerfile.drtrace:35](../../../../Dockerfile.drtrace#L35) — the same
  pin the per-language bindings image [Dockerfile.drtrace-lang:26](../../../../Dockerfile.drtrace-lang#L26)
  carries).
- Pick one of four options, in preference order:
  - **(a) build-from-source** — build the four extensions from source against the pinned DR so
    they link the host glibc rather than DR's shipped prebuilts. *Recommended default:* most
    robust and CI-reproducible in the pinned image.
  - **(b) static-link** — statically link the extensions into the client, side-stepping the
    private loader entirely. Stays LGPL-clean here (no `drwrap` in the set) but is heavier to
    package; fallback.
  - **(c) version-pin** — keep the prebuilts if the loader probe passes under the CI image's
    glibc. Cheapest, but *unverified* — the blocker is documented generically ("modern glibc")
    with no tested version boundary, so (c) is viable only if the probe passes.
  - **(d) accept `drwrap` LGPL-2.1** — **rejected**; only forced if you additionally want
    `drwrap`'s function wrapping, which the target stack does not need.
  Run (c)'s cheap loader test first: if the pinned 11.91.20630 prebuilts happen to load, this
  increment collapses to a one-line `use_DynamoRIO_extension()`; otherwise commit to (a).
- Reproduce or rule out the concrete `__memcpy_chk` resolution symptom on record
  ([macos-drtrace-plan.md:466](../../plans/macos-drtrace-plan.md#L466)) and record the observed glibc
  boundary the docs never pin.
- **Confirm `umbra`/`drx_buf` licensing is BSD/permissive** (only `drwrap` is called out as LGPL
  anywhere in-repo) and record it in the tier's license note — the LGPL-clean claim depends on
  it.

**Exit criteria** *(MET 2026-07-13):* the `make docker-drext-probe` target builds and runs the
probe under the pinned DR image and prints a load-success line for `drmgr`+`drreg`+`drx`
(`drx_buf` is drx's trace-buffer API, not a separate extension) with a non-zero
instrumented-instruction count (130588 over `/bin/true`) ✅; the chosen option **(c) version-pin**
and the observed glibc **2.39** are recorded in-tree
([dr-extension-load-probe-findings.md](../../analysis/dr-extension-load-probe-findings.md)) ✅; the
BSD stack links **no `drwrap` and no LGPL object**, and the license finding is recorded — with the
correction that **`umbra` is LGPL-2.1** (so it is *excluded* from the BSD gate, opt-in behind
`PROBE_UMBRA`) ✅; CI gained a `drext-probe` job that fails red if any of `drmgr`/`drreg`/`drx`
fails to load ✅; the untouched clean-call client still builds through the additively-edited
`drclient/CMakeLists.txt` (`make drtrace-client` reconfirmed; `make dr-valtrace-test`
[native-trace.mk:191](../../../../mk/native-trace.mk#L191) unchanged) ✅.

**Effort.** **S** — a throwaway probe plus one `make`/CI target; the risk was *discovery* (which
of a/b/c loads, the glibc boundary, and the true license of each extension), not code volume.
Realized as S; the load blocker did not reproduce and the umbra=LGPL split was the load-bearing
discovery.

## Increment 3 - Re-platform the L0 value client onto inlined instrumentation *(LANDED 2026-07-13)*

**Outcome.** The inlined client [dataflow_dr_client_inlined.c](../../../../src/dataflow_dr_client_inlined.c)
(`libasmtest_drval_client_inlined.so`) re-platforms the clean-call recorder onto the BSD-clean
extension stack — `drmgr` phased instrumentation, `drreg` scratch regs + aflags, `drx_buf` trace
buffer — and fills the SAME `at_drval_t`. Driven by the SAME `dr_valtrace` harness via
`ASMTEST_DRVAL_CLIENT` (`make dr-valtrace-inlined-test`), it passes all 14 checks **identically to
the clean-call client, including both emulator-oracle slice cross-checks** (stable over 5/5 runs),
now wired into the `drtrace-test` CI gate alongside the clean-call client. The shipped clean-call
`asmtest_drval_client` is untouched as oracle/fallback.

**Microbenchmark (`make dr-valtrace-bench`, [dr_valtrace_bench.c](../../../../examples/dr_valtrace_bench.c)
+ [scripts/dr_valtrace_bench.sh](../../../../scripts/dr_valtrace_bench.sh)):** over a 120002-step
looping fixture (a back-edge + a flag-dependent branch + ~30 `drx_buf` flushes — correctness the
tiny oracle fixture skips; the routine's return value is asserted), the isolated capture window
(`ASMTEST_DRVAL_BENCH`, excluding the symmetric DR init + replay) is **~184 ns/insn clean-call vs
~70 ns/insn inlined — a ~2.6× per-instruction capture-cost drop (~62% less), stable across runs**.
The direction-of-travel check is met (this is the capture-path speedup, not the ~10–50× whole-tier
taint claim — that is Increment 9). Kept an informational `make` target, not a hard CI gate (a
timing assertion is CI-noise-prone; the inlined client's *correctness* is gated by
`dr-valtrace-inlined-test`).

Two **principled divergences** surfaced (documented in the client header), both semantically
irrelevant — the oracle gate passes identically:
- **`rflags` value is clean-call-only** (stored 0). Full `rflags` can only be read inline via
  `pushfq`, which writes the app red zone (`[rsp-8]`) — and the fixture stores its live value
  there, so `pushfq` would corrupt it. `lahf`/`seto` give only the arith subset, not `mc.xflags`.
  Flag def-use *locations* still enter the graph; only the flag *value* is absent.
- **Dead register slots are not literal.** `dr_get_mcontext` snapshots the whole register file;
  `drreg` treats a dead register as free scratch, so `drreg_get_app_value` returns a scratch value
  for a never-consumed (dead-on-entry) slot. Every value the def-use graph/slices actually consume
  matches. Byte-for-byte identity of the full literal register file is therefore a clean-call-only
  property (like `rflags`); the *semantic* value trace is identical.

Key implementation lessons (for Increments 4+): `drreg_get_app_value(X,Y)` restores `X` in place,
so the register backing the `drx_buf` pointer must be captured last via a pointer copy; the trace
buffer's `update_buf_ptr` clobbers arithmetic flags, so reserve `aflags` **only** around it (late)
so its `lahf`/`rax` spill does not perturb the register capture.

**Post-landing adversarial review** (a 4-dimension find → verify → judge workflow, run before
Increment 4 rides on this client). Two confirmed **inlined-specific** defects were **fixed** and
re-validated (oracle 14/14, bench ~2.6×): (a) a captured memory operand whose base/index aliases
the `drx_buf`-pointer register would let `drreg_restore_app_values` restore it in place and destroy
the buffer pointer (memory corruption; latent — needs drreg to pick a base/index reg as scratch) —
now skipped conservatively; (b) the inline value load dereferenced no-load memory operands
(`lea`/`nop [mem]`/`prefetch`), crashing on an unmapped EA the app never touches — now gated by
`instr_reads_memory`, bounding the "assumes valid EA" divergence to genuine loads (where the app
faults too). Two **shared** (both value producers), latent, design/ABI-level findings are
**documented** in-code as known limitations, deferred to the increment that owns their real fix:
the deferred-write model mis-values a `call`'s `rsp`/`rip` across an out-of-region callee (proper
fix = call-out step-over / whole-process capture; [dataflow_dr.c](../../../../src/dataflow_dr.c)
`build_valtrace`), and high-byte sub-registers (`AH`/`BH`/`CH`/`DH`) fold to the low byte (needs a
sub-register byte-offset in the record ABI; `snap_gp`). The subsections below are the as-planned
scope, retained for provenance.

Swap the recorder, not the semantics: reproduce the exact `at_drval_t` capture the clean-call
client produces, but inlined + buffered, and re-validate against the emulator oracle before any
taint rides on top. Still no taint, no dotnet — the current in-process C harness and
emulator-oracle cross-check ([native-trace.mk:201](../../../../mk/native-trace.mk#L201)) stay the
test bed. Land as a *new* CMake target so the shipped `asmtest_drval_client` stays intact as the
fallback/oracle during the swap.

- Move `event_bb` ([dataflow_dr_client.c:149](../../../../src/dataflow_dr_client.c#L149)) into
  `drmgr`'s analysis + insertion phases; emit the GP-snapshot and memory-source EA/value capture
  ([on_step :77](../../../../src/dataflow_dr_client.c#L77)) as **inline** `drreg`-scratch
  instrumentation instead of the `on_step` clean call
  ([:173-177](../../../../src/dataflow_dr_client.c#L173)).
- Route captured records through a **`drx_buf`** per-thread buffer with periodic flush, replacing
  the direct append into `at_drval_t`. Preserve the honest-overflow/`truncated` discipline of the
  ABI ([dataflow_dr.h:95](../../../../src/dataflow_dr.h#L95)).
- **Re-express marker/arg resolution without `drwrap`:** keep resolving `AT_DRVAL_MARKER_SYM` by
  PC ([:188](../../../../src/dataflow_dr_client.c#L188)) and reading `rdi/rsi/rdx` at the marker PC,
  now as a `drmgr` insertion-phase equivalent of the current SysV-arg clean call
  ([:156](../../../../src/dataflow_dr_client.c#L156)) — do **not** silently pull `drwrap_get_arg`
  back in.
- Keep the existing operand exclusions honest and unchanged for now (far/segmented `fs:`/`gs:`,
  VSIB gather EA, store post-values, [:104-107](../../../../src/dataflow_dr_client.c#L104)); this
  increment is a performance/architecture swap only. Keep the `DR_SIGNAL_DELIVER` handler
  ([:213-217](../../../../src/dataflow_dr_client.c#L213)) unchanged.

**Exit criteria:** the inlined+buffered client passes the existing `dr_valtrace`
cross-validation against the emulator oracle ([dataflow_emu.c](../../../../src/dataflow_emu.c)) —
same green CI gate, now on the extension stack — via `make dr-valtrace-inlined-test`, wired into
`drtrace-test` ✅ (**MET 2026-07-13**; 14/14, both oracle slice cross-checks, 5/5 stable). Records
match the clean-call client for every def-use-consumed field (byte-identical on live reads,
memory EA/values, and deferred writes valued from the next LIVE snapshot); `rflags` value and
dead-register slots are principled clean-call-only divergences (above), not captured inline ✅. No
taint ✅. The `make dr-valtrace-bench` microbenchmark shows a measurable per-instruction
capture-cost drop (~2.6× / ~62% on the isolated capture window over a 120k-step looping fixture) ✅
(**MET 2026-07-13**). Increment 3 fully closed.

**Effort.** **L** — the central re-platform lift and the highest-risk single increment: every
operand-capture path moves from a clean call to inlined `drreg`-scratch + `drx_buf` code and must
reproduce `at_drval_t` **byte-identically** under the existing oracle gate. Correctness bar is
exacting even though no new semantics land.

## Increment 4 - In-band taint: BSD shadow + concurrency policy + seed/sink API *(FULLY LANDED 2026-07-13 — exit criteria met + all hardening done; Increment 5 next)*

First taint semantics: a byte-granular shadow and inline `dst_tag = ∪ src_tags`, driven
by the seed/sink surface, validated on a **native in-process fixture** — still docker,
still no dotnet, so the launch container (Increment 5) is a separable change.

> **Design (locked 2026-07-13 by a 3-way design panel; winner: *hand-rolled BSD 2-level
> create-on-touch shadow behind a `tag_ptr` seam*).** The ABI is **LANDED**:
> [include/asmtest_taint.h](../../../../include/asmtest_taint.h) (`at_tag_t` = 1-byte union tag,
> bit0 = tainted + up-to-7 colors; `AT_TAINT_SEED_SYM`/`AT_TAINT_SINK_SYM` PC-resolved markers;
> `at_taint_seed_t`/`at_taint_hit_t`/`at_taint_report_t`; `<stdint.h>`-only, no `<stdbool.h>`).
> Build plan:
> - **Same file, additive `-DASMTEST_TAINT`.** Ship `libasmtest_drtaint_client.so` from the SAME
>   [dataflow_dr_client_inlined.c](../../../../src/dataflow_dr_client_inlined.c) under the flag; with
>   the flag off the TU compiles byte-for-byte to the Increment-3 value client, so
>   `dr-valtrace-inlined-test` stays provably untouched (stronger than a second TU that can drift).
> - **Shadow (BSD, DR-core only — the umbra-swap seam kept tiny).** 1:1 byte scale, 2-level
>   create-on-touch: a static directory `at_tag_t **g_dir` (2^47 / LEAF_SPAN pointers,
>   `dr_raw_mem_alloc`'d once, demand-zero) → 1 MiB (`LEAF_SPAN = 1<<20`) leaves allocated
>   zero-filled on first touch, installed by an **atomic CAS** (the one mandatory-atomic mutation;
>   the CAS loser frees its spare). `tag_ptr(ea) { i=ea>>20; lf=g_dir[i]; if(!lf) lf=leaf_alloc(i);
>   return lf + (ea & (LEAF_SPAN-1)); }` — canonical user VA (0..2^47) only; covers the raw C
>   stack, so the fixture needs no arena crutch. This IS the localized growth path to Inc5/umbra
>   (swap `tag_ptr`/`leaf_alloc` at the same scale; propagation untouched).
> - **Registers (per-thread, no concurrency policy needed).** A flat `at_tag_t` reg-tag file in a
>   `drmgr` TLS slot, zeroed in the thread-init event, indexed by the **DR reg id canonicalized to
>   its 64-bit container** (whole-register tags this increment; eflags is a location too, so
>   `cmp`→`Jcc` flag-carried flow reaches a branch sink). Uses Increment 3's DR-native operand walk
>   — do **NOT** link Capstone/`asmtest_operands` into the client (Inc2 proved only
>   `drmgr`/`drreg`/`drx` load; the Capstone-in-client link + full-span reservation are exactly the
>   risks that sank the rejected "enumerator-anchored" approach). Enumerator congruence is instead a
>   *validation guard* — the oracle diff proves the DR walk matches the slicer's set.
> - **Propagation — inline, a SECOND `drmgr` insertion pass ordered AFTER value capture via
>   `drmgr_priority_t`, no hot-path clean call.** (1) union sources into a `drreg` scratch `s_t`:
>   `xor s_t,s_t`; per src reg `or s_t, byte[tls_regfile+id]`; per src integer-mem reuse Inc3's
>   app-restored base/index `lea` then the `tag_ptr` sequence, OR the operand's low tag byte(s).
>   (2) broadcast `s_t` to every dst (reg → `mov byte[tls+id],s_t`; integer-mem store *location* →
>   `tag_ptr`, store `s_t`). (3) **step witness** (the oracle-diff wiring): extend `raw_step_t` with
>   one `uint8_t step_tainted` set inline `or byte[buf+off], s_t`, surfaced via a parallel
>   `dv->step_taint[]` app array — `at_vstep_t`/`at_drval_t` ABI stays byte-identical. Bracket the
>   propagation block in its OWN `drreg_reserve_aflags` (distinct from the late one around
>   `update_buf_ptr`); bump `drreg` slots 5→7; on `drreg` failure **degrade gracefully** (skip
>   taint for that step = conservative miss), never corrupt `at_drval_t`.
> - **Seed/sink — rare PC-resolved clean calls (the `on_marker` pattern, no drwrap), off the hot
>   path.** `on_seed` paints `tag_ptr(base..+len)=color` at seed time (pre-traced-code, no
>   concurrency); `on_sink` inline-loads the watched operand's tag and, if nonzero, a guarded clean
>   call appends one `at_taint_hit_t`.
> - **Validation.** New `make docker-taint-native` / `make dr-taint-native-test` (mirror
>   `docker-drtrace` / `dr-valtrace-inlined-test`, self-skip without DR), a hand-written
>   `examples/dr_taint.c` fixture (seed a buffer → derive through GP regs AND integer memory incl. a
>   natural stack spill/reload → sink) whose client tainted-step set (from `dv->step_taint`) is
>   asserted **EQUAL to `asmtest_slice_forward(seed_step)`** from the emulator oracle
>   (`slices_equal` discipline), plus a **negative control** (unseeded → empty set) and an inscount
>   sanity (no hot-path clean call).
>
> **First slice (recommended):** ABI (done) + the 2-level shadow + per-thread reg-tag file + the
> inline union/broadcast pass + `step_tainted` + **`on_seed` only** (defer `on_sink`/`at_taint_hit_t`
> to the next slice), built as `libasmtest_drtaint_client.so` and oracle-diffed by `dr_taint.c`
> against the forward slice (+ negative control). Smallest thing that builds in the pinned DR docker
> lane reusing Inc3's proven DR-native walk + EA `lea` (no Capstone-in-client, no full-span
> reservation), is oracle-checkable, and leaves the Inc3 value gate provably untouched.
>
> **First slice LANDED 2026-07-13.** Shipped exactly as scoped: the BSD 2-level create-on-touch
> shadow (`g_dir` → 1 MiB leaves over `dr_raw_mem_alloc`, atomic-CAS leaf install), a per-thread
> reg-tag TLS file (16 GP containers + eflags), the inline `dst_tag = ∪ src_tags` propagation +
> `step_tainted` witness (a phase of the value-capture insertion pass, placed after the mem loop and
> before the buffer advances, so the witness rides the same `drx_buf` record via `dv->step_taint[]`),
> and `on_seed`, all additive under `-DASMTEST_TAINT` in the SAME
> [dataflow_dr_client_inlined.c](../../../../src/dataflow_dr_client_inlined.c). Validated by
> `make docker-taint-native` / `make dr-taint-native-test`
> ([examples/dr_taint.c](../../../../examples/dr_taint.c)): the seeded run's client taint set is
> **EQUAL to `asmtest_slice_forward(seed_step)`** (8/8, incl. the emulator-oracle forward-slice
> cross-check), the unseeded **negative control reports zero tainted steps** (5/5), the inline gate
> proves the only clean calls are the two markers, and — the key invariant — the flag-OFF value
> client stays **byte-identical** (`dr-valtrace-inlined-test` still 14/14). Two documented first-slice
> simplifications, each a safe under-approximation (a conservative MISS, never corruption/false
> positive), at their sites in the client: (1) memory operand tags use the operand's LOW byte (seeds
> paint every byte, so the store/reload share it) — per-byte multi-byte union is next; (2) inline
> store-tag broadcast is branchless *write-if-leaf-present-else-drop* (cmov to a throwaway on a null
> leaf) with the seed buffer's leaf and each thread's stack leaves pre-touched at seed time — general
> create-on-touch-on-store via a first-touch slowpath clean call is next.
>
> **Sink slice LANDED 2026-07-13 — Increment 4 exit criteria MET.** Added the seed/sink surface's
> other half: `on_sink_register` (PC-resolved `asmtest_dr_taint_sink_marker`, rdi = `at_taint_report_t*`)
> and a branch-condition sink (`kind = 1`) that appends one `at_taint_hit_t` at each in-region
> conditional branch whose eflags tag is tainted, via a transparent clean call reading this thread's
> reg-tag file (off the per-instruction propagation path; `seed_off`/`depth` left 0 and filled app-side
> by the harness's def-use BFS, exactly as the ABI specifies). New `dr_taint.c` scenarios `sink`
> (tainted seed → `add` taints the flag → `jz` sink → **one hit at off 0x10, tag tainted, kind 1, in
> `forward(seed)`, app-side seed_off 0x00 + depth 4**, 11/11) and `sink-negative` (unseeded → **zero
> hits**, 2/2), plus the first-slice seeded/negative still 8/8 + 5/5 and flag-off `dr-valtrace-inlined-test`
> still 14/14 — all green in the fresh `make docker-taint-native` image. This meets the Increment 4
> exit criteria (seed a color, derive through GP regs + integer memory, the sink fires with the correct
> `seed_off`→`off` and `depth`, the tag graph is diffed against the emulator L2 slicer, a negative
> control reports zero hits, propagation is inline).
>
> **Create-on-touch-on-store LANDED 2026-07-13.** Replaced the first-slice pre-touch-plus-drop store
> policy with real create-on-touch: the inline store-tag fast path writes the tag when the leaf
> exists, else a first-touch SLOWPATH clean call (`on_store_slow`, a conditional `jz` over a
> transparent `dr_insert_clean_call`) allocates the leaf and writes it — so arbitrary store targets
> (the managed heap, Increment 5) are handled with **no pre-touch**, and the slowpath is taken at most
> once per 1 MiB page (the fast path is an inline store; the inline gate now allows 5 clean-call sites,
> all rare/off the per-instruction path). The fragile `pre_touch_stack` hack is **deleted** — its proof
> is that the stack-spill fixtures still pass, plus a new `heapstore` scenario (5/5) that flows taint
> through a store to a **fresh, never-touched heap buffer** and back (would return clean under the old
> drop policy). Landed first-try (the conditional-clean-call construct held); flag-off value client
> still 14/14; all 31/31 in the fresh docker image.
>
> **Per-byte memory-tag union LANDED 2026-07-13 — all Increment-4 hardening now done.** Memory operand
> tags are byte-granular: a source read unions all `size` shadow bytes (`emit_shadow_or` loops the
> operand's bytes) and a store writes all `size` bytes (`emit_shadow_store` fast path + `on_store_slow`
> per-byte); registers keep whole-register 1-byte tags (SIMD is Increment 8). Each leaf is allocated one
> guard page larger than its 1 MiB span, so a per-byte access STRADDLING a leaf boundary reads/writes
> the guard instead of faulting past the mmap (the straddling bytes miss the next leaf = a conservative
> miss, never a fault or false positive). A new `highbyte` scenario (4/4) seeds ONLY the high 4 bytes of
> an 8-byte buffer, then loads all 8: the load's low-byte tag is clean, so this passes only under the
> per-byte union (a low-byte-only read would taint nothing). flag-off value client still 14/14; all
> 35/35 in the fresh docker image; landed first-try. The guarded inline sink skip + other sink kinds
> (mem-len / call-arg) remained as generality follow-ons at this point, not correctness gaps — **both
> have since LANDED** (see the follow-on note below). **Increment 4 is
> complete; Increment 5 (launch-under-DR container + out-of-process validator) is the next major push.**
>
> **Generality follow-ons LANDED (both).** (1) **Guarded inline sink skip** *(landed with the
> Increment-9 overhead work, commit `a56b7f4`)* — the sink now inline-tests the watched tag with
> drreg-reserved aflags (so the app flags the branch reads are preserved) and only makes the
> `on_sink` clean call when tainted; a clean branch pays a TLS load + test, not a call. (2) **All
> three sink kinds** *(commit `2befa39`)* — `emit_guarded_sink`
> ([dataflow_dr_client_inlined.c:1049](../../../../src/dataflow_dr_client_inlined.c#L1049)) is
> generalized to take a watched reg-tag index + kind and `on_sink(off, ea, kind, rt_idx)` reads
> `rf[rt_idx]` bounds-checked
> ([:997](../../../../src/dataflow_dr_client_inlined.c#L997)), with the `event_insert` dispatch at
> [:1926](../../../../src/dataflow_dr_client_inlined.c#L1926) selecting: `cbr` → eflags (**kind 1**,
> branch condition), `call` → SysV arg0 `rdi` (**kind 2**, call-arg), `OP_rep_movs` → count register
> `rcx` (**kind 0**, mem-len). Categories are mutually exclusive, at most one fires, and the ONE
> shared clean-call site keeps the inline gate at exactly 5. New `dr_taint.c` modes
> `callarg`/`callarg-negative` + `memlen`/`memlen-negative` are wired into `dr-taint-native-test`;
> mem-len asserts the sink is in `forward(seed)` at depth 1 (`rcx` is a machine source of rep-movs),
> call-arg asserts the arg's defining move is in `forward(seed)` (a direct call does not machine-read
> its args, so that sink is calling-convention-based). Flag-off value client still 14/14.
>
> **Concurrency (committed, all approaches agreed):** tolerated-benign-race single-byte tag stores
> (aligned `at_tag_t` writes atomic on x86-64; union monotone within a seed epoch → a lost update
> is a conservative MISS, never a false clean→tainted flip). Reg tags are per-thread (never race).
> The only mandatory-atomic mutation is the leaf CAS install; bulk mutations (seed paint, Inc7 GC
> remap) are fenced/quiesced, not raced. Rejected: a global hot-path lock (blows the ~10–50× band);
> per-thread memory shadows (cannot express cross-thread managed flows).

> **License decision (from Increment 2): RESOLVED — hand-rolled BSD shadow, no `umbra`.** Increment
> 2 found `umbra` is **LGPL-2.1** (Dr. Memory Framework), not permissive
> ([dr-extension-load-probe-findings.md](../../analysis/dr-extension-load-probe-findings.md)), so the
> locked design above hand-rolls a BSD 2-level shadow over DR-core `dr_raw_mem_alloc` (option (i)) —
> the tier stays fully BSD. The `dst_tag = ∪ src_tags` semantics and concurrency policy are
> provider-independent; only the shadow provider is fixed to BSD. The as-planned subsections below
> (which still say "`umbra` shadow") predate this decision — read "`umbra` shadow" as "the BSD
> byte-granular shadow" throughout; they are retained for provenance.

- Allocate a shadow of `at_tag_t` per app byte (1 byte/byte, `AT_TAG_CLEAN = 0`), via the chosen
  provider (BSD direct-map, or `umbra` if (ii)). On
  each instrumented instruction, compute `dst_tag = ∪ src_tags` **inline** in the `drmgr`
  insertion pass over the register + memory operands the enumerator already walks
  ([dataflow_operands.c](../../../../src/dataflow_operands.c), the shared enumerator behind
  [dataflow_dr.h](../../../../src/dataflow_dr.h)); key the memory-dst shadow on the same
  `opnd_compute_address` EA math the value path uses
  ([dataflow_dr_client.c:122](../../../../src/dataflow_dr_client.c#L122)).
- **Shadow concurrency policy — a first-class design decision, not an edge case.** `drx_buf` is
  per-thread, but the `umbra` tag shadow is a *single process-global byte map* every instrumented
  thread writes via inline `dst_tag = ∪ src_tags`, and .NET is heavily multithreaded (app threads
  + GC threads). This increment commits to **tolerated-benign-race single-byte tag stores**:
  naturally-aligned `at_tag_t` byte writes are atomic on x86-64, and a union tag is *monotone
  within a seed epoch* (it only ever gains bits), so a lost update degrades to a conservative
  *miss*, never a false `clean→tainted` flip; the only bulk shadow mutations are seed-time paint
  and the GC-move remap (Increment 7). **Rejected alternatives, recorded:** a global hot-path
  lock (blows the ~10–50× band) and per-thread shadows (cannot express the cross-thread flows a
  managed runtime routinely creates). The single-threaded native fixture here cannot exercise the
  race — the policy is *stated and implemented* now, and *stress-tested against real concurrent
  writers* in Increment 5's launched dotnet workload.
- Ship the proposed `include/asmtest_taint.h` seed/sink ABI: a seed marker paints `[base,base+len)`
  shadow with a color; a sink marker appends an `at_taint_hit_t` (with `depth`) when a tainted
  value reaches a watched sink operand — resolved by PC exactly as the value marker is, **no
  `drwrap`**.
- Handle **store tags**: the value client skips store *values*, but a store's *location* already
  enters def-use ([dataflow_dr.h:60-62](../../../../src/dataflow_dr.h#L60)), so propagate the
  source-union tag into the store's destination-address shadow.
- Scope taint to GP + integer-memory operands this increment; the current Increment-1 EXCLUSIONS
  (`fs:`/`gs:`, VSIB gather EA, store values,
  [:104-107](../../../../src/dataflow_dr_client.c#L104)) remain known tag gaps, and **XMM/YMM is
  explicitly deferred to Increment 8** (even libdft's low overhead partly comes from *skipping*
  XMM/SSE/MMX, so SIMD is real work, not free).

**Exit criteria:** a new native docker lane *(proposed `make docker-taint-native`)* seeds a color
on a known buffer, runs a hand-written fixture that copies/derives through GP regs and integer
memory, and the sink fires with the correct `seed_off`→`off` and `depth`; the tag graph is
**diffed against the emulator L2 slicer** (the shared spine [dataflow.c](../../../../src/dataflow.c)
driven by the [dataflow_emu.c](../../../../src/dataflow_emu.c) oracle) on the same fixture; a
**negative control** (unseeded run, or seed not reaching the sink) reports **zero hits**;
propagation is emitted inline (no clean call in the hot path — verifiable by an inscount sanity
check); the shadow-concurrency policy is documented in-tree with its rejected alternatives. Green
in CI with no hardware.

**Effort.** **M–L** — first taint semantics, the shadow-concurrency policy, and the seed/sink ABI
together, but all on a single-threaded native fixture, so it carries no launch/managed/JIT risk.
The concurrency *policy* is cheap to state; its *validation* is deferred to Increment 5 where real
concurrency exists.

## Increment 5 - Launch-under-DR container (drrun -c … -- dotnet app.dll) + out-of-process validator *(ALL exit criteria MET — native launch + dotnet JIT-coexistence + concurrency-stress LANDED 2026-07-13, managed seed→sink LANDED 2026-07-14; **Increment 5 COMPLETE**)*

> **Managed seed→sink LANDED 2026-07-14 — exit criterion (3) MET; Increment 5 is now COMPLETE.**
> `make dr-taint-managed-test` (folded into `Dockerfile.taint-dotnet`): the last exit criterion —
> a taint seed flowing through **REAL JIT'd managed code** to a sink, reported **out of process** — by
> composing Increment 6's method-range auto-registration with this increment's shm channel + validator.
> A native P/Invoke shim ([taint_managed_shim.c](../../../../examples/taint_managed_shim.c),
> `libtaint_managed_shim.so`) exports the seed/sink marker symbols the client resolves by PC (the
> client's `event_module_load` resolves them when the .so loads), maps the shm channel, and holds a
> **native** seed buffer (a stable address — painting a GC-movable managed object is Increment 7). The
> managed workload ([taint_managed/Program.cs](../../../../examples/taint_managed/Program.cs)) P/Invokes the
> shim to seed, then loops calling a `[MethodImpl(NoInlining)]` `HotSeedSink(ptr)` that reads the seeded
> buffer and uses it as a **loop bound** — a real conditional branch (an `if (x==k)` folds to a
> branchless `cmov`, so no branch sink fires; a loop's `cmp i,x` is the reliable tainted-eflags branch).
> `methodscan=Hot` auto-registers HotSeedSink; the seeded load→cmp→branch trips the branch-condition
> sink and the hit crosses to a SEPARATE [taint_managed_validator](../../../../examples/taint_managed_validator.c).
> **Seeded run: the sink reports a tainted `kind=1` hit over JIT'd managed code (4/4); unseeded negative
> control: ZERO hits (2/2)** — deterministic over repeated runs, clean exit (no hang, riding the
> Increment-6 exit-hang fix). Validation is STRUCTURAL, not the emulator oracle diff the native lanes use
> (the L0 emulator cannot replay JIT'd .NET code — the full shared-fixture oracle cross-check is
> Increment 9). **Client UNCHANGED** (the seed/sink markers + methodscan already existed; the feature is
> pure workload + shim + validator + lane). Validated in a fresh `docker-taint-dotnet` image alongside the
> coexistence (2/2) + methods (3/3) lanes.

> **Concurrent-writer stress LANDED 2026-07-13 — exit criterion (4) MET; the Increment-4 race policy is
> now VALIDATED, not just stated.** `make dr-taint-stress-test` /
> [taint_stress.c](../../../../examples/taint_stress.c): `drrun -c <taint client>.so -- ./taint_stress`
> launches N=8 threads released together by a barrier, ALL seeding a disjoint buffer + running the
> branch-sink fixture at once — so the process-global tag shadow takes concurrent leaf-CAS installs
> (nearby seed buffers share a leaf; each thread's stack spill first-touches its own) and concurrent
> single-byte tag stores, and the sink report takes concurrent appends. Result (deterministic over
> repeated runs): **exactly N sink hits, every one correct (offset/tag/kind), no crash, no hang, no
> false clean→tainted flip, no lost/corrupted hit** — confirming aligned at_tag_t byte stores are
> atomic, the leaf CAS is the one mandatory-atomic mutation, and per-thread reg tags never race. One
> small principled client change (under `-DASMTEST_TAINT`, so the flag-off value client stays 14/14):
> the sink-report append is now thread-safe — an atomic fetch-add reserves a unique disjoint slot
> (`hits_total` is the true count; `hits_len` a best-effort mirror). Chained into `drtrace-test` + the
> `docker-taint-native` lane (48/48 green: native 35 + launch 9 + stress 4). **Only exit criterion (3)
> remains** — instrumenting REAL JIT'd managed code with a managed seed→sink — which needs managed
> method ranges, wired by Increment 6's method-load events; the shm channel + out-of-process validator
> + the atomic report are the reusable plumbing it will ride on.


> **dotnet JIT/code-cache coexistence LANDED 2026-07-13 — the plan's risk concentration is RETIRED.**
> `make dr-taint-dotnet-test` / `make docker-taint-dotnet` runs
> `drrun -c libasmtest_drtaint_client.so -- dotnet taint_hello.dll`: a managed workload
> ([examples/taint_hello/](../../../../examples/taint_hello/)) whose hot method tiers up (tier-0 → tier-1)
> mid-run — so DR's code cache must handle .NET's tiered JIT **rewriting live code** — runs to
> **completion, exit 0, no swallowed SIGSEGV, no SIGTRAP/crash, no hang** (2/2 in a fresh docker image
> built from `Dockerfile.taint-dotnet` = DR + the .NET SDK). This is the **first in-tree demonstration
> that DR coexists with .NET tiered-JIT** — the "hypothesis under test" the plan flagged as the risk
> concentration — and it held on the **first attempt**, with the taint client UNCHANGED (single build,
> no launched-runmode target). Meets exit criterion (2). Wired: a new CI `taint` job runs
> `docker-taint-native` + `docker-taint-dotnet` (additive to the in-process `drtrace` job, per the
> plan). Notes: the workload here is a plain managed program (no C markers), so the client's region
> gate keeps it un-instrumented for taint — this slice proves DR-vs-runtime COEXISTENCE; **instrumenting
> real JIT'd managed code with a managed seed→sink (exit criterion 3, needs a managed shim) and the
> concurrent-writer stress (exit criterion 4) are the next slices.**


> **First slice LANDED 2026-07-13 (native launcher de-risk).** The launcher mechanics + shm transport
> + out-of-process validator are proven on a NATIVE workload (no dotnet/JIT yet): `make
> dr-taint-launch-test` runs `drrun -c libasmtest_drtaint_client.so -- ./taint_workload`, and the
> launched client (running under DR from a CLEAN START) seeds a buffer, propagates taint inline, and
> writes the branch-condition sink hit into a **POSIX shared-memory** channel
> ([include/asmtest_taint_shm.h](../../../../include/asmtest_taint_shm.h)); a SEPARATE
> [taint_validator](../../../../examples/taint_validator.c) process drains it and oracle-diffs the hit
> against the emulator forward slice **out of process** (9/9 green in the fresh `make
> docker-taint-native` image; chained into `drtrace-test` so the CI `drtrace` job runs it too, oracle
> auto-skipped there without libunicorn). **The build-mode question is RESOLVED: a SINGLE build** — the
> same `configure_DynamoRIO_client` `.so` works unmodified under `drrun -c` (no launched-runmode CMake
> target needed), and **ZERO client changes** were required (the markers + synchronous sink append
> already work under launch; only the report is shm-backed, and cross-process reads go by offset, never
> the producer-space pointers). **Exit criterion (1) — "a `drrun … -- <native workload>` lane produces
> a non-empty value/taint trace, drained and oracle-diffed by the out-of-process validator" — is fully
> MET:** BOTH channels ride the shm segment — the SYNCHRONOUS sink report (written by the sink clean
> call, present when the fixture returns) and the drx_buf-buffered VALUE / taint trace (flushed by the
> client's exit event at PROCESS EXIT, complete once `drrun` returns). The validator diffs the sink hit
> AND the full taint SET (`{steps[i].off : step_taint[i]}` == the emulator forward slice) out of
> process. [taint_workload.c](../../../../examples/taint_workload.c) is self-contained (defines the marker
> symbols, maps shm, mmaps an RWX fixture) — no in-process `dr_init`/`dr_start`, since DR owns the
> process. **Next slices:** the `dotnet` launch + first real JIT/code-cache coexistence test (the
> plan's risk concentration), and a concurrent-writer stress validating the Increment-4 race policy.

The new integration path — there is **no** `drrun`/`dr_inject` launcher in-tree today (a clean
grep returns zero matches, [data-flow-capture.md:203](../../analysis/data-flow-capture.md#L203)).
Launch owns the `dotnet` process from a clean start so DR never takes over a runtime that already
installed its signal handlers / JIT'd code mid-run — the code-cache/JIT-collision half the
in-process model cannot solve. De-risk the launcher mechanics on a native workload first, then a
minimal dotnet one. This is also where the shadow-concurrency policy (Increment 4) meets real
concurrent writers.

- **Image shape (decide, don't defer): a single merged Dockerfile** — the DR install lives in
  `Dockerfile.drtrace` (no dotnet), the .NET SDK lives in the bindings image; the launch lane
  needs both **plus the `drrun` binary** from the tarball's `bin64` on `PATH`. Recommend one
  merged Dockerfile over a compose for CI determinism.
- Invocation `drrun -c libasmtest_drval_client.so -- dotnet app.dll`. The `DR_SIGNAL_DELIVER`
  mitigation carries over unchanged ([dataflow_dr_client.c:213](../../../../src/dataflow_dr_client.c#L213));
  the arbitrary-state problem does not arise because DR owns the process from a clean start.
  **Attach stays out of scope** (record why).
- **New region/method-range registration:** a launched `dotnet app.dll` never calls the C-harness
  marker ([:64](../../../../src/dataflow_dr_client.c#L64)). Recommend `drrun` client-option seed/sink
  config **plus a small injected managed shim** that reports the seeded buffer's address (the
  managed source of method ranges is method-load events, wired in Increment 6).
- **Cross-address-space results channel (transport):** back the report buffer with a **POSIX
  shared-memory** segment named via a `drrun` client option (the in-process `at_drval_t*` /
  `at_taint_report_t*` pointer is not valid across the separately-launched process).
- **Out-of-process oracle-diff validator (the shm *consumer*):** in-process today, `dr_valtrace`
  replays `at_drval_t`/hit records through the shared enumerator and spine
  ([dataflow_operands.c](../../../../src/dataflow_operands.c) → [dataflow.c](../../../../src/dataflow.c))
  and diffs the emulator oracle ([dataflow_emu.c](../../../../src/dataflow_emu.c)) in the same
  address space. Under launch the client can no longer host that diff, so name an explicit
  **separate app-side validator process** — the evolved `dr_valtrace` harness, *not* the launched
  `dotnet` — that attaches to the same shm segment, drains the records, and runs the L1/L2 replay
  + oracle diff out-of-process. The launched client is the *producer*; this validator is the
  *consumer*. This is the wiring the "oracle-diff gate at every step" (Contract) and Increment 9
  depend on; it does not exist in-tree and lands here.
- **Client build-mode fallback branch:** verify whether the same `libasmtest_drval_client.so`
  works unmodified under `drrun -c` vs the in-process `configure_DynamoRIO_client` build
  ([drclient/CMakeLists.txt:18](../../../../drclient/CMakeLists.txt#L18)). **If a distinct
  launched-runmode build IS required**, it lands *within this increment* as a second small CMake
  target (a `configure_DynamoRIO_client` launched-runmode variant) — an **additive ~S** build
  change, not a re-architecture; the in-process build stays intact as the oracle/fallback. State
  this branch now so the increment is fully scoped either way.
- Treat "launch sidesteps the JIT collision" as a **hypothesis under test** — this is the first
  in-tree exercise of DR's code cache coexisting with .NET tiered-JIT recompilation
  ([data-flow-capture.md:203](../../analysis/data-flow-capture.md#L203)).

**Exit criteria:** a `drrun -c <client>.so -- <native workload>` lane produces a non-empty
value/taint trace in docker, drained and oracle-diffed by the out-of-process validator; a
`drrun … -- dotnet <hello>.dll` lane runs a trivial managed workload to completion under DR with
**no swallowed .NET `SIGSEGV`**, no SIGTRAP/crash, and **no DR code-cache/JIT collision** (first
in-tree demonstration of coexistence with .NET tiered-JIT); the client instruments real JIT'd
managed code, writes hits to the shared-memory channel, and a seed→sink over a managed buffer is
reported through it and validated out-of-process; a **concurrent-writer stress** over the
process-global shadow (multiple managed threads deriving through seeded state) shows no false
`clean→tainted` flip, confirming the Increment-4 race policy; the client build-mode question is
resolved (single build, or a launched-runmode CMake target landed); a new CI job runs the
drrun-over-dotnet smoke, additive to the in-process `make drtrace-test` job.

**Effort.** **L** — a wholly new integration path with broad, mostly net-new surface: the merged
DR + .NET-SDK image, the `drrun` launcher, the shm transport, the out-of-process validator, the
managed shim, and the first real JIT-coexistence test. The build-mode fallback is small if it
triggers; the risk concentration is the un-demonstrated JIT/code-cache coexistence.

## Increment 6 - Whole-process breadth + method-range scoping *(COMPLETE — native multi-range mechanism + boundary policy + inscount scoping LANDED 2026-07-13; dotnet method-load auto-registration LANDED 2026-07-14, commit 47a4721)*

> **Native multi-range mechanism LANDED 2026-07-13 — the range-set core + boundary policy +
> inscount cost bound.** The single `g_region` is generalized to a published SET of registered
> ranges (`g_regions[AT_MAX_REGIONS]` + release-stored `g_nregions`) sharing one capture buffer
> (`g_drval`) and ONE offset origin (`g_origin` = the lowest registered base, so a multi-range
> trace shares the emulator oracle's blob-relative offset space). The bb-build gate is now
> `in_scope(ipc,&off)` — membership in the range set (default), or the whole window spanning them
> under the `scope=whole` client option. The region marker (`on_marker`) APPENDS instead of
> overwriting; everything downstream (shadow, propagation, sink, shm) is unchanged, and with a
> single registered range the client is behaviourally identical to Increments 1-5 (the value +
> taint + launch + stress lanes stay green, inline gate still exactly 5 clean calls).
> `make dr-taint-multirange-test` / `docker-taint-native` (now the 4th native lane): a launched
> native workload (`examples/taint_multirange.c`) registers TWO disjoint ranges around an
> **un-instrumented gap** and carries the taint across it through the **process-global stack
> shadow** (store in range A, reload in range B). The `taint_multirange_validator` oracle-diffs the
> taint set OUT OF PROCESS: 9/9 green — range-count = 2, the sink fires at the range-B branch, and
> the client's captured taint set (range A + range B steps only, the gap absent) equals the
> emulator forward slice across the gap. The **cost bound is demonstrated**: `scope=whole`
> instruments 13 instructions (the whole window incl. the gap) vs `scope=ranges` 7 (the two ranges
> only) — the client reports both via a grep-able `ASMTEST_TAINT_INSCOUNT` line at exit.
> **Boundary policy (validated + documented):** the shadow is process-global and the reg-tag file
> is per-thread, so a tag written in a scoped range PERSISTS through an un-instrumented gap
> unchanged (neither propagated nor cleared) — EXACT precisely when the gap does not transform the
> tagged location (the fixture's gap touches only a scratch register, never the carried
> `[rsp-8]`), an under/over-approximation otherwise. **Still remaining for Increment 6:**
> auto-registering ranges from .NET **method-load events** on a launched dotnet workload (the
> perfmap/`MethodLoadVerbose` addr-channel) so range-count > 1 arises WITHOUT a per-range C marker
> — the slice that unblocks Increment 5's managed seed→sink.

Move beyond Increment 1's single registered region to whole-process taint, but bound cost by
scoping the expensive per-operand shadow work to registered method ranges (the ~10–50× band
assumes we are not tag-tracking the entire runtime).

- Generalize the single `g_region` (the unlocked region read on the hot path,
  [dataflow_dr_client.c:165-167](../../../../src/dataflow_dr_client.c#L165)) to a set of registered
  method ranges, re-instrumented via `dr_delay_flush_region`
  ([:70](../../../../src/dataflow_dr_client.c#L70)) as ranges arrive.
- Auto-register ranges under launch from .NET **method-load events** (reuse the Phase-4 PC→method
  identity channel; `MethodLoadVerbose` event id **143**,
  [data-flow-tracing-plan.md:199](data-flow-tracing-plan.md#L199)) so a launched dotnet workload
  populates ranges without the C-harness marker.
- Keep propagation **correct across un-instrumented gaps**: the shadow is process-wide even
  though *instrumentation* is scoped, so a tag written inside a scoped range must persist when it
  flows through un-instrumented library code and re-enters a scoped range — define and validate a
  conservative boundary policy explicitly.

**Exit criteria:** a launched dotnet workload instruments **multiple** JIT'd method ranges
(count > 1) auto-registered from method-load events, verified in docker, with a seed→sink path
spanning two registered ranges (un-instrumented code between them) still reporting the hit; a
scoping toggle demonstrably reduces instrumented-instruction count vs whole-process (an inscount
delta proving the cost bound is real); the oracle diff still matches; CI asserts range-count > 1.

**Effort.** **M** — generalizing one region to a range set plus method-load auto-registration
reuses the Phase-4 PC→method addr-channel, so the plumbing is known; the subtle work is the
conservative un-instrumented-gap boundary policy and proving the scoping cost bound with an
inscount delta.

## Increment 7 - GC-move umbra shadow remap *(**COMPLETE 2026-07-14** — Slice 1 live-wiring + **Slice 2 full seed→move→sink SURVIVAL**: a taint seed on a GC-movable managed object survives a compacting GC and the sink fires at the object's NEW address; the DR-API-free raw-mmap leaf allocator carries the tag across a never-touched destination leaf)*

> **UPDATE 2026-07-14 — Slice 2 (full seed→move→sink SURVIVAL) LANDED; Increment 7 COMPLETE.** The
> Slice-1 conservative miss (a move into a never-touched destination leaf DROPPED the tag) is removed:
> `at_gc_remap_live`'s destination paint now uses `tag_ptr_create_raw` — a **bare `mmap` SYSCALL**
> (`raw_mmap_anon`, inline `syscall`, no libc/errno, DR-API-free), installed into the SAME `g_dir` as
> the hot-path `dr_raw_mem_alloc` leaves (a leaf is a leaf; leaves are never freed after install — only
> a losing CAS spare is, each by its own allocator, so provenances never cross). Proven two ways:
> - **Synthetic (deterministic, dotnet-free):** `dr-taint-gcremap-test` gained T5-T7 exercising the LIVE
>   `at_gc_remap_live` (not the DR-API `at_gc_remap`): a tag survives into a never-touched NEW leaf (T5),
>   per-byte colours move 1:1 into a fresh leaf (T6), and an unseeded move materializes NO destination
>   leaf / conjures no phantom taint (T7) — the shadow-level present-at-new + absent-at-old coherence.
>   Now **13/13** (was 7/7); `docker-drtrace` **161/161**, value client still **14/14 byte-identical**.
> - **Live end-to-end (the de-risk):** new `dr-taint-gcmove-survival-test` lane
>   ([taint_gcmove_managed](../../../../examples/taint_gcmove_managed/) + the shim's `shim_seed_at`): seed a
>   GC-movable managed `byte[]` at its briefly-pinned current address, force compacting gen2 GCs that
>   RELOCATE it (measured `moved=1`, old→new ~44 MB apart), the profiler feeds every moved range to
>   `at_gc_remap_live` at the fence, and the instrumented `GcMoveSink` (`methodscan=MoveSink`) reads the
>   MOVED object → the branch-condition sink FIRES with the tainted tag at the NEW address (**4/4**); the
>   `noseed` negative control reports ZERO hits (no phantom taint at the freshly-mmap'd leaf, **2/2**).
>   **This is the FIRST run to call `raw_mmap_anon` from the profiler's dcontext-less app-code thread on a
>   REAL GC fence — 20,022 ranges remapped, no crash — empirically confirming the bare-mmap claim** (the
>   load-bearing Slice-1 finding was that DR heap APIs crash there). Wired into `docker-gcprofiler-probe`
>   (now 3 lanes). Remaining conservative miss (never a crash/false positive): a move range wider than the
>   1 MiB static snapshot is skipped rather than truncated.

> **UPDATE 2026-07-14 — live wiring (Slice 1) LANDED.** The profiler→client GC-move path is wired
> end-to-end and validated under DR (`make dr-gcmove-live-test` / `docker-gcprofiler-probe`): the DR
> taint client, under the new `gcmove` option, publishes the address of its `at_gc_remap` entry to a
> POSIX-shm handshake ([asmtest_taint_gcmove.h](../../../../include/asmtest_taint_gcmove.h)); the
> in-process `MovedReferences2` profiler ([examples/gcprofiler_probe/](../../../../examples/gcprofiler_probe/))
> reads it and feeds every moved `{old,new,len}` range to the client at the GC fence. Measured: **60,021
> real compacting-GC move ranges remapped under DR across 120 GC events, workload completed, no
> crash/hang**; flag-off value client still 14/14, gcremap selftest still 7/7, docker-drtrace 155/155.
> **LOAD-BEARING FINDING baked into the design:** DR heap/lock APIs — `dr_mutex_lock`, `dr_global_alloc`,
> `dr_raw_mem_alloc` — **CRASH when called from the profiler's app-code thread** (they need the client
> `dcontext`, which only exists inside a DR event/clean call; only `dr_fprintf` survives). So the live
> remap is a **DR-API-FREE** variant `at_gc_remap_live` (plain atomic spinlock + a static snapshot +
> `tag_ptr_lookup` only — NO `tag_ptr_create`/leaf alloc); the original DR-API `at_gc_remap` stays for the
> client-init-context synthetic selftest. **Slice-1 limitation (conservative, never a crash/false-positive):**
> the live remap does NOT create a destination leaf, so a move into an as-yet-untouched NEW leaf is a
> conservative MISS. **Slice 2 (full seed→move→sink survival)** needs a **raw-syscall leaf allocator**
> (a bare `mmap` syscall, DR-API-free) so the tag is carried across a never-touched destination, plus the
> managed seed→move→sink choreography + the coherence canary. Details:
> [gc-move-range-extraction-findings.md](../../analysis/gc-move-range-extraction-findings.md).

> **UPDATE 2026-07-14 — triple-extraction mechanism researched; recommendation changed.**
> The Phase-4 `{old,new,len}` extraction has now been researched (deep-research
> investigation, 23/25 claims 3-0 verified) and the recommended mechanism is **no longer**
> the out-of-process EventPipe/nettrace parser this section assumes below. It is a **native
> in-process CLR profiler using `ICorProfilerCallback4::MovedReferences2`**, which delivers
> the runtime's own per-range `{oldObjectIDRangeStart, newObjectIDRangeStart,
> cObjectIDRangeLength}` parallel arrays directly. Why it wins: it is **in-process /
> native** (same address space as the DR'd target, so it calls the already-landed
> `at_gc_remap` **directly and synchronously** — no cross-process hop), delivers the
> **exact** triple with no schema reconstruction, is **guaranteed-complete** (no
> drop/truncate, unlike EventPipe under load), fires **only on real moves**, and fires at a
> **fully-suspended-EE fence** — a natural world-stop for the bulk remap. **Tradeoff:** it
> requires the `COR_PRF_MONITOR_GC` mask, which **disables background/concurrent GC** in the
> target (forces blocking collections) — clean fences, but an observable change to the
> target's GC behaviour. **Go/no-go gate: PASSED (GO, 2026-07-14).** The one untested risk —
> profiler-vs-DynamoRIO coexistence on Linux — was probed and **confirmed working**: a minimal
> `MovedReferences2` profiler ([examples/gcprofiler_probe/](../../../../examples/gcprofiler_probe/)),
> run under `drrun -c <taint client> -- dotnet <compacting-GC workload>`, loaded + Initialized
> and delivered the **exact `{old,new,len}` ranges under DR (same count as native, no crash/hang)** —
> `make dr-gcprofiler-probe` / `make docker-gcprofiler-probe`. So the profiler path is adoptable;
> the EventPipe fallback (separate helper over the POSIX shm channel) is retained only for a later
> constraint (e.g. not wanting `COR_PRF_MONITOR_GC` to disable background GC). The in-proc
> `EventListener` is a confirmed **dead end** (scalar count only, no `Values` array). Full
> analysis, candidates, sources, the probe result, and caveats:
> [gc-move-range-extraction-findings.md](../../analysis/gc-move-range-extraction-findings.md).
> The as-planned text below is **superseded on the extraction mechanism** (EventPipe → in-proc
> profiler) but otherwise stands. *(This note's closing line originally read "the increment remains
> hard-blocked until the coexistence probe is green" — that condition was MET by the probe recorded
> in this same note, and Increment 7 went on to complete via Slices 1 and 2 above. Not blocked.)*

Make taint survive .NET GC compaction: when the GC moves an object, its shadow tags must move
with it, or a compacting GC silently drops/aliases taint. **Hard dependency:** this needs Phase
4's concrete `GCBulkMovedObjectRanges` `{OldRangeBase, NewRangeBase, RangeLength}` triple (via
EventPipe), which is **still deferred** — the in-proc `EventListener` landed only the DETECTION
feed (`GcMoveMap`) and does not surface the `Values` struct-array
([data-flow-tracing-plan.md:193](data-flow-tracing-plan.md#L193)). *(Superseded 2026-07-14: the
recommended extraction mechanism is now the `ICorProfilerCallback4::MovedReferences2` in-process
profiler, not out-of-process EventPipe — see the update note above and
[gc-move-range-extraction-findings.md](../../analysis/gc-move-range-extraction-findings.md).)*

- On each GC-move event, remap the `umbra` shadow for every moved range: copy tags from
  `[OldRangeBase, +RangeLength)` to `[NewRangeBase, +RangeLength)` so post-compaction reads see
  the pre-move taint. The remap is a **bulk shadow mutation** (not a hot-path byte store), so it
  must be sequenced against the Increment-4 concurrent-writer policy — quiesce or fence the remap
  against in-flight tag stores so a move cannot race a derive.
- Add a **coherence check, not just event consumption:** a missed `GCBulkMovedObjectRanges` event
  silently aliases pre/post-move taint. Ship a canary tag on a known object deliberately forced
  to move, asserting the tag is readable at the **new** address and **absent** at the old.
- **Partial-progress path while Phase 4 is deferred:** land the remap code path behind a disabled
  flag plus a unit test over synthetic `{old,new,len}` triples, so the chain does not fully stall
  on the external blocker.

**Exit criteria (full): MET 2026-07-14.** A launched dotnet workload seeds a color on a GC-movable
managed object, forces a compacting Gen-2 GC that relocates it (`moved=1`), and the branch-condition
sink still fires with the tainted tag at the object's **new** address (`dr-taint-gcmove-survival-test`
seeded 4/4); the `noseed` negative control reports zero hits (no phantom taint at the moved-into leaf,
2/2); the shadow-level present-at-new + absent-at-old coherence is proven deterministically by the
synthetic T5-T7 (`dr-taint-gcremap-test` 13/13). Gated in `docker-gcprofiler-probe` (3 lanes). Phase 4
was resolved not by the deferred EventPipe `{old,new,len}` feed but by the in-process
`MovedReferences2` profiler (the go/no-go note above), so the increment shipped the full live remap +
survival, not the disabled-flag placeholder the original text anticipated.

**Effort.** **M** for the code (delivered): the DR-API-free raw-mmap leaf allocator
(`tag_ptr_create_raw` / `raw_mmap_anon`) + the managed seed→move→sink choreography + the survival lane.
The original "externally hard-blocked on Phase 4" framing is resolved — the profiler path (Slice 1)
surfaced the triple in-process, and Slice 2 carried the tag across the move.

## Increment 8 - XMM/YMM (SIMD) taint *(first slice XMM/SSE c5431f8; **YMM/AVX breadth + overhead + Capstone oracle fix LANDED 2026-07-14 — Increment 8 COMPLETE** for the scoped SIMD surface; ZMM / VSIB / lane-precise flow explicitly deferred)*

> **UPDATE 2026-07-14 — YMM/AVX breadth LANDED; Increment 8 exit criteria MET.** The reg-tag file
> now carries 32 per-byte lanes per 256-bit vector slot (an XMM operand is the low 16, since XMMn
> aliases YMMn[127:0]); `rt_ymm_base`/`rt_vec_base` map YMM0-15 to the same slots XMM0-15 use, the
> emit helpers are width-parameterized (16 for XMM, 32 for YMM), and `taint_mem_size` admits 32-byte
> AVX loads/stores — so taint flows through a **YMM** register AND an **AVX** 32-byte vectorized copy,
> not just SSE. New fixtures `ymm_copy` (oracle-diffed forward slice `{0,1,2,3}`) + `ymm_sink` (a
> seeded YMM lane reaches a branch sink, `kind=1`, depth 4) with negative controls, AVX-gated
> (`__builtin_cpu_supports("avx")`) so they skip cleanly on a non-AVX CPU. `dr-taint-simd-test`:
> ymm-copy 5/5 (ORACLE DIFF), ymm-negative 5/5, ymm-sink 7/7, ymm-sink-negative 2/2; SSE lanes + all
> other lanes unchanged; value client still **14/14 byte-identical**; `docker-drtrace` 0 failures.
> **BONUS BUG FIX (shared, load-bearing):** Capstone (through 5.0) mis-reports the DESTINATION memory
> operand of a **VEX/EVEX vector STORE** as `access=READ` (the SSE `movdqu [m],xmm` is correct
> `WRITE`), so the def-use oracle silently dropped every AVX store→load edge — the ymm-copy forward
> slice came back `{0,1,2}` (missing the reload). Fixed in the shared operand enumerator
> ([dataflow_operands.c](../../../../src/dataflow_operands.c)): a MOV-family instruction's operand[0] (the
> Intel-order destination) is written, never read (`x86_move_store_dest`); scoped to the mov/vmov
> mnemonic + operand-0 so GP/SSE moves are idempotent and loads/RMW/compares are untouched. Host
> `make dataflow-test` 180/180 (incl. the pre-existing ptrace YMM tests) confirms no regression.
> **SIMD overhead delta (the "on vs off" exit criterion), reported SEPARATELY from the scalar band:**
> `dr-taint-overhead-test` gained an AVX2 (YMM) hot-loop variant (`simd` arg → `simd_hot_loop`,
> `target("avx2")`). Measured (1M iters): scalar full-taint ≈ **428× bare**, **SIMD full-taint ≈ 785×
> bare** — SIMD taint runs ~1.8× costlier per the per-byte-lane granularity note (a 32-byte YMM
> source/dest = 32 union/broadcast ops + a 32-byte shadow access vs 1 for a GP reg). Reported so the
> cost is EXPLICIT rather than silently blowing the scalar band; the lane asserts the monotonic
> `SIMD full-taint > bare` (`ok 2`) + prints the tradeoff. Gated in `docker-taint-native` (the
> overhead + SIMD lanes). **DEFERRED, all documented in-code:** ZMM (512-bit) upper lanes; VSIB
> vector-gather EA math; lane-precise (sub-register) SIMD flow (pshufd / narrow-write masking) — the
> per-byte storage is the forward-compatible seam for that; and the VEX.128 zero-upper semantics (a
> conservative over-taint, never a miss).

Genuine research, not a checkbox — even **libdft punts on XMM/SSE/MMX**, and its cited low
overhead (1.14–6.03× utils) is partly *that coverage tradeoff*
([data-flow-capture.md:356](../../analysis/data-flow-capture.md#L356)) — so the ~10–50× band may not
hold once `umbra` covers vector state. Scoped as its own increment with its own overhead
measurement.

- Extend the shadow + `dst_tag = ∪ src_tags` rules to XMM/YMM registers and SSE/AVX
  loads/stores/shuffles; handle the VSIB vector-gather EA math the value client currently skips
  ([dataflow_dr_client.c:104-107](../../../../src/dataflow_dr_client.c#L104)). Decide lane
  granularity (per-byte vs per-lane tags) explicitly — per-byte matches the integer shadow but
  multiplies shadow traffic.
- **Re-measure** overhead with SIMD taint on vs off; if the band does not hold, scope SIMD taint
  behind a flag and record the coverage/cost tradeoff explicitly rather than silently
  under-covering.

**Exit criteria: MET 2026-07-14.** A native docker fixture flows taint through an XMM **and** a YMM
register and an SSE **and** an AVX vectorized copy/reduce, the sink fires with the expected tag —
SIMD taint is not silently dropped (`dr-taint-simd-test`: SSE copy/sink + `ymm-copy`/`ymm-sink`, all
oracle-diffed, with negative controls); the SIMD-on vs SIMD-off overhead delta is reported separately
from the scalar band (`dr-taint-overhead-test` SIMD block, ~785× bare vs the scalar ~428×); CI gates
the SIMD taint fixtures green with the tradeoff documented (both lanes in `docker-taint-native`).
Scoped to XMM/YMM integer-lane taint; ZMM / VSIB / lane-precise flow are deferred with an in-code
rationale, not silently under-covered.

**Effort.** **L** (delivered) — genuine research (SIMD taint is under-solved industry-wide); the real
work turned out to include a shared Capstone AVX-store-access bug fix in the operand enumerator (the
def-use oracle silently dropped AVX store→load edges) that gates the oracle-diff for every vector
store. Carried its own overhead re-measurement (SIMD ~1.8× the scalar band per the per-byte-lane
traffic) rather than inheriting the scalar band, exactly as the increment anticipated.

## Increment 9 - Managed seed→sink end-to-end validation + overhead + CI *(**COMPLETE 2026-07-14** — record-free production propagation put production at ~11× bare (IN the ~10-50× band); the HARD BAND GATE + GC-survival CI wiring landed; all exit criteria MET)*

> **UPDATE 2026-07-14 — hard band gate + GC-survival CI wiring LANDED; Increment 9 COMPLETE.**
> `dr-taint-overhead-test` now HARD-GATES the band (`ok 3`: build-FAILS if `prod-taint > BAND_MAX=50×
> bare`; measured ~11×, so ~4.5× noise headroom — the ratio prod/bare is runner-speed-independent, and
> a real regression such as re-adding the drx_buf record → ~187× trips it). `docker-gcprofiler-probe`
> (the GC-move survival lane, Increment 7 Slice 2) is wired into CI as the new **`taint-gcmove` job**,
> so "taint survives a GC" is now build-gated. The other exit clauses were already gated: managed
> seed→sink detection (`docker-taint-dotnet`), native prod detection (`dr-taint-prod-test`), and the
> shared-fixture emulator cross-check (`dr-taint-launch-test`, out-of-process `taint_validator` over
> `taint_sink_chain`). Validated: `docker-taint-native` 0 failures (band gate `ok 3` green at ~11×).
> **All Increment-9 exit criteria are MET → the taint-tier plan is complete** (ready to archive per the
> archive rule; the parent Phase-5 stub updates to LANDED in the same change).

> **UPDATE 2026-07-14 — lever 1 (record-free production propagation) LANDED; production is IN the band.**
> The `prod` client option now takes a SEPARATE, RECORD-FREE emit path (`event_insert` branches to
> `emit_taint_phase_prod` and returns): NO drx_buf record at all — no buffer-pointer load/advance, no
> GP register-file snapshot, no memory value/size/valid stores, no `off`/`mem_n`/`mem_ea` stores, and
> no `step_taint` witness. Each memory-source EA is computed INLINE via `lea` for the shadow read
> instead of round-tripping through a record; the reg-tag unions, shadow read/write, and guarded sink
> are unchanged. It reserves ~4 GPR + aflags (no buffer pointer) vs the value path's 6. The non-prod
> value+propagation path is BYTE-UNCHANGED (a second, smaller emit path), so the flag-off value client
> stays **14/14** and every oracle-diffed taint lane is unaffected. Because production keeps no witness,
> correctness is SINK-based: new `make dr-taint-prod-test` (in `drtrace-test`) runs the launched sink
> fixture under `drrun -c <client> prod` and the validator's `prod` mode asserts the seeded run reports
> exactly one tainted `kind=1` branch hit (**6/6**) and the `noseed` negative reports ZERO (**3/3**) — a
> seed reaching a sink is the end-to-end proof taint propagated. **MEASURED (`dr-taint-overhead-test`,
> 1M iters): full-taint ≈ 452× bare → PROD ≈ 11× bare — IN the plan's ~10-50× band.** This CORRECTS the
> earlier decomposition (which put the record skeleton at ~60% and predicted ~75×): the drx_buf record
> was the *bulk* of production cost, and removing it ALONE reaches the band — the direct-mapped shadow
> (lever 2) is a further optimization, **not needed to enter the band**. `docker-drtrace` 0 failures
> (189 ok). What remains for full Increment 9: the band-threshold HARD CI gate (build-fail on non-
> detection or overhead-regression) + the shared-fixture emu cross-check; GC-survival already landed
> (Increment 7).

> **Overhead-measurement first slice LANDED 2026-07-14 — the FIRST in-repo overhead number, with a
> 4-way DECOMPOSITION.** `make dr-taint-overhead-test` ([taint_overhead.c](../../../../examples/taint_overhead.c),
> folded into `Dockerfile.taint-native`): a hot loop (seeded load + integer arithmetic, branchless body
> so the only branch is the loop back-edge) timed with an internal `CLOCK_MONOTONIC` window, run four
> ways — bare native, DR code-cache baseline (regions=0), the inlined VALUE client (value capture only),
> and the TAINT client (value + taint). **Measured (2M iters, -O2): DR-baseline ≈ 1.0× bare (DR's
> steady-state on a hot loop is near-native); value-capture ≈ 300× bare; taint-propagation adds only
> ≈ 1.4× over value capture; whole-tier ≈ 437× bare.** The load-bearing FINDING (and a CORRECTION of
> this slice's first cut, which wrongly blamed the per-branch sink clean call): the whole-tier cost is
> **DOMINATED by the L0 VALUE-capture recording** — a full register-file snapshot per instruction, an
> **oracle-validation** feature — **NOT by taint**; propagation itself is cheap. **The PRODUCTION build
> (client option `prod`) LANDED 2026-07-14** — it drops that value-capture recording (the GP register-file
> snapshot + the memory value/size/valid stores), keeping only the mem-source EA + tag propagation +
> shadow + sink, so taint semantics are IDENTICAL (the out-of-process taint-set oracle still passes 9/9;
> the taint set is the `step_taint` witness + offset, never the GP values; the flag-off value client stays
> 14/14). **Measured: full-taint ≈ 423× → PROD-taint ≈ 216× bare — a ~2× win, but NOT yet in the ~10-50×
> band.** A CORRECTION of this note's earlier optimism ("the band is reachable by just dropping the value
> trace"). **A decomposition of the remaining `prod` cost (throwaway `dbg_noprop` measurement toggle,
> not committed) pinpoints the lever: of `prod` ≈ 187× bare, the drx_buf RECORD SKELETON is ~60% (≈113×)
> and tag PROPAGATION (reg-tag unions + shadow + witness) is only ~40% (≈74×).** So the biggest lever to
> the band is **NOT the shadow — it is ELIMINATING THE drx_buf RECORD** in production: the value trace AND
> the `step_taint` witness are validation-only, so a true production build keeps neither and computes the
> mem-source EA inline (`lea`) for propagation instead of round-tripping it through a record. That alone
> takes `prod` from ~187× toward ~75× (propagation-only + baseline); a **direct-mapped shadow** then
> attacks the residual ~74× propagation. So Increment-9-proper's sequence is: (1) drx_buf-free / record-
> free production propagation, (2) direct-mapped shadow, (3) the band-threshold HARD gate. Also landed
> alongside: the **guarded-inline-sink-skip**
> (the Increment-4 follow-on) — the branch-condition sink now inline-tests the eflags tag (drreg-reserved
> aflags around it, so the app flags the branch reads are preserved) and only makes the on_sink clean call
> when tainted; a clean branch pays a TLS load + test, not a clean call. It is a correctness-preserving
> efficiency refinement (all sink lanes still green: native sink 11/11, managed seed→sink 4/4), not the
> band lever (the value trace is). INFORMATIONAL: wall-clock ratios are noise-prone (as the Increment-3
> microbench notes), so the lane asserts only the monotonic structural fact `T_taint >= T_value > T_dr >=
> T_bare` and REPORTS the decomposition. The **managed seed→sink** half of this increment's exit criteria
> is already met by Increment 5 ([taint_managed](../../../../examples/taint_managed/)), and the production
> propagation-only build now landed (above); what remains for full Increment 9 is to bring `prod` into the
> ~10-50× band via the measured lever sequence — **(1) a record-free production propagation path** (drop
> the drx_buf record + witness, inline `lea` for the EA — the ~60% chunk), **(2) a direct-mapped shadow**
> (the residual propagation) — then GC-survival (Increment 7 — now unblocked, profiler path), the
> shared-fixture emulator cross-check, and the hard band + non-detection CI gate.

> **Implementation spec — record-free production propagation (LANDED 2026-07-14 as `emit_taint_phase_prod`).**
> The spec below is the one that was implemented; it landed exactly as scoped and OUTPERFORMED its own
> estimate — removing the drx_buf record alone took production to ~11× bare (IN the band), not the ~75×
> the decomposition predicted, because the record skeleton was the bulk of production cost, not ~60%. The
> direct-mapped shadow (lever 2) is therefore a further optimization, not a band prerequisite. Retained
> as the record of the change (a precise scope for this delicate `event_insert` path).
> - **What to remove under `prod`:** the entire drx_buf record path — the per-instruction buffer-pointer
>   load (`drx_buf_insert_load_buf_ptr`) + advance (`drx_buf_insert_update_buf_ptr`), the `off`/`mem_n`/
>   `mem_ea` stores, and the `step_taint` witness store. Production needs neither the value trace nor the
>   taint SET; only the shadow state + the sink.
> - **What propagation must then do itself:** compute each memory-source EA INLINE via `lea` over the
>   drreg-app-restored base/index (exactly the pattern `emit_shadow_store` and the Increment-8 >8-byte
>   SIMD source path already use) instead of reading `mem_ea` from the record. Reg-tag unions, the shadow
>   read/write, and the guarded sink are unchanged.
> - **Register plan:** no `s_ptr` (no buffer). Reserve scratch for EA (`s_a`), shadow work (`s_b`), the
>   union accumulator (`s_t`), and the reg-tag base (`t_rf`), plus aflags around the phase — ~4 GPR +
>   aflags (vs the current 6). On any drreg failure, degrade to skipping taint for that step (a
>   conservative miss), never corrupt the shadow.
> - **Structure:** in `prod`, branch to a self-contained propagation path that does NOT run the value
>   pass at all; keep the current value+propagation path for non-prod (so the flag-off value client and
>   the oracle-diffed taint lanes are byte-unchanged). This is a second, smaller emit path, not an edit
>   of the shared one.
> - **Validation (note: no witness ⇒ no taint-SET oracle):** correctness rests on the SINK — the native
>   `sink`/`sink-negative` lanes and the managed seed→sink must still fire/stay-zero under `prod`
>   (a seed reaching a sink is the end-to-end proof taint propagated). Add a `prod` variant of those
>   asserts. Then re-measure via the overhead bench — expect `prod` ~187× → ~75× bare.
> - **Risk:** this is the tier's most delicate code (inline drreg/EA management around the app
>   instruction); do it fresh, sink-validate before committing, and revert rather than ship a
>   sink-that-passes-but-mis-propagates. The direct-mapped shadow (lever 2) is a separate, later change
>   to the shadow accessor only.

The Phase-5 exit gate the whole plan works backward from: everything above composed into a real
managed data-flow assertion with a measured cost, replacing the offset-only dotnet smoke
([bindings/dotnet/drtrace/](../../../../bindings/dotnet/drtrace/)).

- A launched dotnet workload with a taint **seed at a source** (a `read()`/`recv()`/stream
  buffer) **detected at a sink** (a `memcpy` length or a branch condition) over **real JIT'd
  managed code**, with taint **surviving a GC** (Increment 7), the verdict produced by the
  out-of-process validator (Increment 5).
- **Overhead measured against an explicit budget.** No number exists on *this repo's* managed
  workload today — all cited figures are external literature: the greenfield ~10–50× band, libdft
  1.14–6.03× utils / 1.25–4.83× servers, TaintTrace ~5.5×, bare-DR inscount ~15× on SPEC CPU
  2017, fast-path Taint Rabbit 1.7× / Sdft 1.58× / HardTaint ~9%
  ([data-flow-capture.md:356-360](../../analysis/data-flow-capture.md#L356)). Set the budget at the
  ~10–50× band, report the measured slowdown vs a bare `dotnet` run, and flag if SIMD taint
  (Increment 8) pushes past it. This is the number that justifies the tier over ptrace
  single-step's ~10³–10⁵× ([data-flow-capture.md:180](../../analysis/data-flow-capture.md#L180)).
- **CI wiring:** a new `dr-taint` docker lane / ci.yml job replacing the in-process
  `make drtrace-test` smoke with the `drrun` invocation over a live dotnet workload, asserting
  seed→sink + GC-survival, with a **hard gate** that build-fails on non-detection or overhead
  regression past a set threshold; the seed→sink verdict is cross-checked against the emulator
  L0/L2 oracle on a shared fixture by the out-of-process validator.

**Exit criteria: MET 2026-07-14.** CI gates, across the taint-tier lanes, every clause:
- **seed→sink over real JIT'd managed code** — `dr-taint-managed-test` (seeded reports a tainted
  `kind=1` branch hit 4/4; unseeded 0 hits) in `docker-taint-dotnet` (CI `taint` job); build-FAILS on
  non-detection. The native `dr-taint-prod-test` / `dr-taint-launch-test` gate detection too.
- **taint survives a GC** — `dr-taint-gcmove-survival-test` (a seed on a GC-movable managed object
  survives a compacting GC; sink fires at the NEW address 4/4; noseed 0 hits 2/2) in
  `docker-gcprofiler-probe`, now wired into CI as the **`taint-gcmove` job**.
- **overhead in the ~10–50× band, build-FAILS on regression** — `dr-taint-overhead-test` `ok 3`
  HARD BAND GATE (`prod-taint <= BAND_MAX=50× bare`; measured ~11×) in `docker-taint-native` (CI
  `taint` job). NB the overhead is measured on the native hot-loop proxy `taint_overhead` (the clean
  per-instruction isolation the first slice established), not a full dotnet run — a dotnet-workload
  number would be dominated by the scope=ranges ~1× baseline plus JIT/GC noise; the record-free
  production per-instruction cost is the meaningful figure and it is band-gated.
- **verdict cross-checked vs the emulator oracle on a shared fixture** —
  `dr-taint-launch-test`'s out-of-process `taint_validator` runs the emulator
  ([dataflow_emu.c](../../../../src/dataflow_emu.c)) on the shared native `taint_sink_chain` fixture and
  diffs BOTH the sink hit AND the full taint SET vs the emulator forward slice (the record-free
  `prod` path is validated sink-only, since it keeps no witness). In the CI `drtrace` job.

This validates beyond the offset-only `bindings/dotnet/drtrace/` smoke, matching
[data-flow-tracing-plan.md:215](data-flow-tracing-plan.md#L215). Per the archive rule, on landing this
plan moves to `docs/internal/archive/plans/` and the parent Phase-5 stub is updated to LANDED.

**Effort.** **M** in code — mostly composition of 5–8 plus the CI/threshold wiring — but gated on
those increments landing; the real work here is the measurement harness and the budget/threshold
hard gate, not new instrumentation.

---

## Implementation status (landed)

- ✅ **Increment 1 — in-band L0 VALUE producer** *(LANDED 2026-07-13)*:
  `libasmtest_drval_client.so` — a clean-call per-instruction producer capturing the GP register
  file (`dr_get_mcontext`) + explicit memory SOURCE effective addresses/values (`decode` +
  `opnd_compute_address` + `dr_safe_read`), cross-validated against the emulator oracle. Source
  [dataflow_dr_client.c:1-28](../../../../src/dataflow_dr_client.c#L1); capture ABI
  [dataflow_dr.h:63-96](../../../../src/dataflow_dr.h#L63); oracle
  [dataflow_emu.c](../../../../src/dataflow_emu.c); driven in-process via `ASMTEST_DRVAL_CLIENT`
  ([native-trace.mk:201](../../../../mk/native-trace.mk#L201)), gated by
  `make dr-valtrace-test` (target defined at [native-trace.mk:191](../../../../mk/native-trace.mk#L191)).
- ✅ **Increment 2 — extension-load probe** *(LANDED 2026-07-13)*: the prebuilt
  `drmgr`/`drreg`/`drx` load under DR's private loader on glibc 2.39 / pinned DR 11.91.20630
  (blocker does not reproduce → **option (c) version-pin**); license resolved to a split
  (`drmgr`/`drreg`/`drx` BSD, **`umbra` LGPL-2.1**). Probe
  [drclient/probe_extensions.c](../../../../drclient/probe_extensions.c); findings
  [dr-extension-load-probe-findings.md](../../analysis/dr-extension-load-probe-findings.md); gates
  `make drext-probe` / `make docker-drext-probe` ([native-trace.mk](../../../../mk/native-trace.mk),
  [docker.mk](../../../../mk/docker.mk)) + CI `drext-probe`.
- ✅ **Increment 3 — inlined L0 value client** *(LANDED 2026-07-13)*:
  `libasmtest_drval_client_inlined.so`
  ([dataflow_dr_client_inlined.c](../../../../src/dataflow_dr_client_inlined.c)) re-platforms the
  clean-call recorder onto `drmgr`/`drreg`/`drx_buf` and fills the same `at_drval_t`; passes the
  `dr_valtrace` emulator-oracle cross-check identically to the clean-call client (14/14, 5/5
  stable) via `make dr-valtrace-inlined-test`, wired into the `drtrace-test` CI gate. `rflags`
  value + dead-register slots are documented clean-call-only divergences. `make dr-valtrace-bench`
  ([dr_valtrace_bench.c](../../../../examples/dr_valtrace_bench.c)) shows a ~2.6× per-instruction
  capture-cost drop on the isolated capture window.
- ✅ Increments 4–9 — LANDED (this document; see the per-increment sections above).

## Validation notes

> *These were code-checked when the plan was written, against the **Increment-1 clean-call client**
> ([dataflow_dr_client.c](../../../../src/dataflow_dr_client.c)) — which is deliberately **untouched**
> as the oracle/fallback, so each observation about it still holds today. The re-platformed client
> ([dataflow_dr_client_inlined.c](../../../../src/dataflow_dr_client_inlined.c)) is the one that moved
> onto `drmgr`/`drreg`/`drx_buf` (Increment 3) and carries the taint surface under `-DASMTEST_TAINT`
> (Increments 4-9). The integration-model note below is updated where the tier has since moved past
> it; the rest is retained as written.*

- *Code-checked:* the hot path is a per-instruction clean call
  ([dataflow_dr_client.c:173-177](../../../../src/dataflow_dr_client.c#L173)) on raw
  `dr_register_bb_event` ([:227](../../../../src/dataflow_dr_client.c#L227)); the client uses raw
  core API only, **no** `use_DynamoRIO_extension()`, with the private-loader blocker recorded
  verbatim in the build file ([drclient/CMakeLists.txt:19-21](../../../../drclient/CMakeLists.txt#L19))
  and the client header ([dataflow_dr_client.c:16-23](../../../../src/dataflow_dr_client.c#L16),
  which also names the `drreg`+`umbra` stack as the Phase-5 END goal).
- *Code-checked:* the signal handler returns `DR_SIGNAL_DELIVER`
  ([:213-217](../../../../src/dataflow_dr_client.c#L213)); region registration is marker-driven from
  SysV args ([on_marker :64-71](../../../../src/dataflow_dr_client.c#L64),
  [event_bb insertion :156](../../../../src/dataflow_dr_client.c#L156), PC-resolved
  [:188](../../../../src/dataflow_dr_client.c#L188)); SIMD/far/VSIB/store operands are skipped
  ([:104-107](../../../../src/dataflow_dr_client.c#L104)).
- *Code-checked:* the emulator oracle is [dataflow_emu.c](../../../../src/dataflow_emu.c) ("the
  reference oracle for the live capture tiers"), **not** [dataflow.c](../../../../src/dataflow.c),
  which is the shared L1/L2 (def-use / slicer) spine. *(As written: "no `drrun`/`dr_inject`
  launcher exists in-tree — the only DR entry is the in-process `dr_app_*` cooperative lifecycle
  ([asmtest_drtrace.h:78-93](../../../../include/asmtest_drtrace.h#L78))". **Superseded 2026-07-14** —
  Increment 5 landed the launch-under-DR path (`drrun -c <client> -- <app>`, `make
  dr-taint-launch-test` / `dr-taint-dotnet-test`), and the ATTACH tier
  ([dynamorio-attach-tier-plan.md](dynamorio-attach-tier-plan.md)) has since added the cooperative
  `dr_app_*` mid-run attach/detach AND the **external** `drrun -attach <pid>` injector path. All
  three DR integration models now exist in-tree.)* In-process, `dr_valtrace`
  hosts the replay/diff in the same address space; the out-of-process validator (Increment 5)
  landed as [taint_validator.c](../../../../examples/taint_validator.c).
- *Docs-checked:* the extension stack, overhead band, and launch-vs-attach framing are the
  authoritative analysis note
  ([data-flow-capture.md:180-241](../../analysis/data-flow-capture.md#L180),
  [:356-363](../../analysis/data-flow-capture.md#L356)); the license separability of `drwrap` is
  [dynamorio-native-trace-plan.md:224-226](dynamorio-native-trace-plan.md#L224)
  and the static-link nuance [:914](dynamorio-native-trace-plan.md#L914); the
  `__memcpy_chk` loader symptom is [macos-drtrace-plan.md:466](../../plans/macos-drtrace-plan.md#L466); the
  `MethodLoadVerbose` event id 143 addr-channel and the GC-move deferral are
  [data-flow-tracing-plan.md:199](data-flow-tracing-plan.md#L199) and
  [:193](data-flow-tracing-plan.md#L193).

## Risks and open points

> **Register status as of 2026-07-14 (all nine increments landed).** This register was written
> before Increment 2; each entry is **retained as written for provenance** and annotated inline with
> its outcome. Nine of twelve are **RESOLVED** by the increments that landed. The three that remain
> live are *standing constraints*, not open work: keep marker/arg resolution `drwrap`-free, keep the
> shadow's benign-race policy intact, and do not regress `DR_SIGNAL_DELIVER` coexistence.

- **The private-loader blocker is documented, never solved.** Every source records it
  generically ("modern glibc") with no tested version boundary
  ([drclient/CMakeLists.txt:19-21](../../../../drclient/CMakeLists.txt#L19),
  [macos-drtrace-plan.md:466](../../plans/macos-drtrace-plan.md#L466)); no build-from-source / static-link /
  version-pin has been attempted. Increment 2 must turn it into an empirical yes/no per option —
  it is the single-point dependency for the entire plan, and if none of (a)/(b)/(c) loads cleanly
  the re-platform stalls.
  **→ RESOLVED (Increment 2, 2026-07-13): the blocker does not reproduce.** The prebuilt
  `drmgr`/`drreg`/`drx` load cleanly under the private loader on glibc **2.39** with the pinned DR
  11.91.20630 (130588 instructions instrumented over `/bin/true`); the `__memcpy_chk` symptom did
  not recur. **Option (c) version-pin** chosen — no build-from-source/static-link owed. Gate
  `make docker-drext-probe` + CI `drext-probe`; findings
  [dr-extension-load-probe-findings.md](../../analysis/dr-extension-load-probe-findings.md).
- **License angle.** Only `drwrap` is LGPL-2.1 and is separable from the target stack; options
  (a)/(b)/(c) keep the tier LGPL-clean, but **static-link (b) carries the stricter obligation**
  ([dynamorio-native-trace-plan.md:914](dynamorio-native-trace-plan.md#L914)) —
  it only bites if an LGPL object is in the set, which it is not, so (b) stays clean but must
  contain no LGPL object. `umbra`/`drx_buf` licensing is **not asserted anywhere in-repo**;
  Increment 2 must confirm them BSD/permissive before the license-clean claim stands.
  **→ RESOLVED (Increment 2, 2026-07-13), and the assumption was WRONG for `umbra`.** The picture is
  a **split**: `drmgr`/`drreg`/`drx` (which is where the `drx_buf` trace-buffer API lives — there is
  **no** separate `drx_buf` extension) are DR-**core** `ext/` extensions under DR's primary **BSD**
  license, but **`umbra` is LGPL-2.1** — it ships under `drmemory/drmf/` as part of the Dr. Memory
  Framework (only `drfuzz`/`drltrace` are its BSD carve-outs), so `umbra` sits with `drwrap`, not
  with the BSD stack. Consequence, taken in Increment 4: the tag shadow is a **hand-rolled BSD
  2-level create-on-touch shadow** over DR-core `dr_raw_mem_alloc` — **no `umbra` is linked**, and
  the tier is fully BSD. The BSD gate excludes `umbra` (opt-in behind `PROBE_UMBRA`).
- **Marker/arg resolution must stay `drwrap`-free.** The re-platform must re-express the SysV-arg
  marker capture ([:64](../../../../src/dataflow_dr_client.c#L64),
  [:156](../../../../src/dataflow_dr_client.c#L156)) in the `drmgr` world without reaching for
  `drwrap_get_arg`, or the tier silently reacquires the LGPL-2.1 obligation (option d) it is
  trying to avoid.
  **→ STANDING CONSTRAINT — held through Increment 9.** No `drwrap` is linked anywhere in the tier.
  Markers stay PC-resolved via `dr_get_proc_address`, and the ATTACH tier's marker-*less* config
  reaches the same end by client options + module+offset resolution + `dr_nudge`, still without
  `drwrap`. Keep it that way.
- **Process-global shadow under a multithreaded runtime.** The `umbra` tag shadow is one
  process-wide byte map every instrumented .NET thread writes concurrently (app + GC threads);
  `drx_buf` is per-thread but the shadow is not. The committed policy is **tolerated-benign-race
  single-byte tag stores** — aligned `at_tag_t` writes are atomic on x86-64, and a union tag is
  monotone within a seed epoch, so a lost update is a conservative *miss*, never a false taint —
  decided and implemented in Increment 4 and first stress-tested against real concurrent writers
  in Increment 5. A global hot-path lock is rejected (blows the band); per-thread shadows are
  rejected (cannot express cross-thread flows). The GC-move remap (Increment 7) is the one bulk
  shadow mutation that must be fenced against in-flight stores.
  **→ STANDING CONSTRAINT — policy VALIDATED (Increment 5, 2026-07-13), no longer merely stated.**
  (Read "`umbra` shadow" as "the hand-rolled BSD shadow" per the Increment-2 license split.)
  `make dr-taint-stress-test` releases N=8 threads through a barrier, all seeding disjoint buffers
  and running the sink fixture at once — concurrent leaf-CAS installs, concurrent single-byte tag
  stores, concurrent report appends. Result, deterministic over repeated runs: **exactly N hits,
  every one correct, no crash/hang, no false clean→tainted flip, no lost hit.** One change it forced:
  the sink-report append is now thread-safe (atomic fetch-add reserves a disjoint slot). The
  ATTACH tier is the first place these threads are ones we did not spawn, and the policy held there
  too.
- **Scale vs the ~10–50× band.** The band is the *greenfield* figure, not the shipped tier
  ([data-flow-capture.md:194-195](../../analysis/data-flow-capture.md#L194)); it holds only for a
  properly inlined+buffered client. Increment 3's re-platform is what earns it, and Increment 9
  is the first place it is measured on this repo — with a budget/threshold, not a bare report.
  **→ RESOLVED (Increment 9, 2026-07-14): the band HOLDS for production, and is now ENFORCED.**
  Measured `dr-taint-overhead-test`: production (record-free) ≈ **11× bare**, inside the ~10–50×
  band. The surprise was *where* the cost lived — not taint (propagation adds only ~1.4× over value
  capture) but the **L0 value-capture recording**, an oracle-validation feature. `BAND_MAX ?= 50`
  ([native-trace.mk:1292](../../../../mk/native-trace.mk#L1292)) hard-gates it: `ok 3` build-FAILS if
  prod-taint exceeds 50× bare (~4.5× noise headroom; the prod/bare ratio is runner-speed-independent).
- **SIMD is under-solved industry-wide.** libdft's cheapness partly comes from *skipping*
  XMM/SSE/MMX ([data-flow-capture.md:356-360](../../analysis/data-flow-capture.md#L356)); Increment 8
  may blow the band and forces an explicit coverage/cost tradeoff rather than a silent gap.
  **→ RESOLVED (Increment 8, 2026-07-14): measured, and reported SEPARATELY rather than folded into
  the scalar band.** SIMD full-taint ≈ **785× bare** vs scalar full-taint ≈ **428×** — ~1.8× costlier,
  the direct consequence of per-byte lane granularity (a 32-byte YMM operand = 32 union/broadcast ops
  + a 32-byte shadow access vs 1 for a GP reg). The tradeoff is explicit in the lane output, not a
  silent gap. ZMM / VSIB / lane-precise flow are deferred with an in-code rationale.
- **GC-move coherence + external block.** A missed `GCBulkMovedObjectRanges` event silently
  aliases pre/post-move taint; Increment 7 needs a coherence canary, and it is **hard-blocked** on
  Phase 4's still-deferred `{old,new,len}` extraction
  ([data-flow-tracing-plan.md:193](data-flow-tracing-plan.md#L193)) — mitigated by the disabled-
  flag + synthetic-triple partial path so the chain does not fully stall.
  **→ RESOLVED (Increment 7, 2026-07-14): the external block never had to be cleared.** Phase 4's
  deferred EventPipe feed was **side-stepped**, not waited on: an in-process
  `ICorProfilerCallback4::MovedReferences2` profiler delivers the exact `{old,new,len}` triple
  synchronously at a suspended-EE fence, calling `at_gc_remap_live` directly. The coherence canary
  landed as the synthetic T5-T7 (`dr-taint-gcremap-test` 13/13: present-at-new, per-byte colours move
  1:1, and an unseeded move conjures no phantom taint), plus the live
  `dr-taint-gcmove-survival-test` (seed survives a compacting gen2 GC that relocates the object;
  sink fires at the NEW address 4/4; noseed 0 hits). CI job `taint-gcmove`. Residual conservative
  miss (never a crash/false positive): a move range wider than the 1 MiB static snapshot is skipped.
- **DR-over-managed heaviness / JIT collision is asserted, not demonstrated.** "Launch owns the
  process from a clean start, so DR's code cache coexists with .NET tiered-JIT" has no in-tree
  evidence ([data-flow-capture.md:203-209](../../analysis/data-flow-capture.md#L203)); Increment 5 is
  the first real test and may surface tiered-recompilation/dynamic-codegen problems.
  **→ RESOLVED (Increment 5, 2026-07-13): coexistence DEMONSTRATED, and it held first try.**
  `make dr-taint-dotnet-test` runs `drrun -c libasmtest_drtaint_client.so -- dotnet taint_hello.dll`
  over a workload whose hot method tiers up (tier-0 → tier-1) mid-run — so DR's code cache must
  handle .NET rewriting live code — to completion, exit 0, no swallowed SIGSEGV, no SIGTRAP, no hang,
  with the client UNCHANGED. This retired the plan's stated risk concentration. *(Note the converse
  is NOT true for attach: DR **seizing** an already-running runtime is fatal — see the ATTACH tier's
  Increment 6 NO-GO. Launch-from-a-clean-start is what makes managed viable.)*
- **Out-of-process validator wiring is net-new.** In-process, `dr_valtrace` replays and diffs the
  oracle in the same address space; once the client is a separately-launched dotnet process
  writing to shm, a *distinct* app-side validator must own the replay/diff. The plan pins the shm
  *transport* and names the validator as the *consumer* (Increment 5), but neither exists in-tree
  and both are first exercised there.
  **→ RESOLVED (Increment 5, 2026-07-13):** both landed and are reused verbatim by the ATTACH tier.
  Transport [include/asmtest_taint_shm.h](../../../../include/asmtest_taint_shm.h) (consumer reads by
  OFFSET, never producer pointers); consumer [taint_validator.c](../../../../examples/taint_validator.c),
  which drains the channel and diffs BOTH the sink hit AND the full taint SET against the emulator
  forward slice out of process (`dr-taint-launch-test` 9/9).
- **Single-step-crashes-JIT inheritance.** The tier already carries hard-won managed-signal
  lessons (TF-armed threads die under managed runtimes; the dotnet GH-runner SIGTRAP; Go's missing
  `runtime.LockOSThread`). Inlined DBI is not single-step, but any per-thread instrumentation
  state under a runtime that spawns/parks threads inherits the same class of hazard — keep the
  taint client strictly inline and do not regress `DR_SIGNAL_DELIVER` coexistence.
- **Launch-lane wiring, now decided but unproven.** The plan commits to a merged DR + .NET-SDK
  image with `drrun` on `PATH`, `drrun` client-option registration + a managed shim, a POSIX
  shared-memory results channel, and an out-of-process validator (Increment 5); a launched-runmode
  CMake variant is scoped as a fallback if the in-process client build does not carry over. None
  of this exists in-tree and each is first exercised there.
  **→ RESOLVED (Increment 5, 2026-07-13/14): all of it landed, and the fallback never triggered.**
  The merged image is `Dockerfile.taint-dotnet` (DR + .NET SDK); the shm channel, managed shim
  ([taint_managed_shim.c](../../../../examples/taint_managed_shim.c)), and out-of-process validator all
  shipped. **The build-mode question resolved to a SINGLE build** — the same
  `configure_DynamoRIO_client` `.so` works unmodified under `drrun -c`, so **no launched-runmode
  CMake variant was needed** and **zero client changes** were required for the launch path.
- **No in-repo overhead number.** Every cited figure is external literature; the tier's cost
  claim is unproven on this repo's managed workload until Increment 9 measures it against the
  budget.
  **→ RESOLVED (Increment 9, 2026-07-14): the repo now has its own numbers, and they are gated.**
  `dr-taint-overhead-test` measures a 4-way decomposition on the native hot-loop proxy
  ([taint_overhead.c](../../../../examples/taint_overhead.c)): DR-baseline ≈ 1.0× bare (DR's
  steady-state on a hot loop is near-native), value-capture ≈ 300×, full-taint ≈ 437–452×, **PROD
  (record-free) ≈ 11× bare**. Measured on the native proxy rather than a full dotnet run by design —
  a dotnet number would be dominated by the `scope=ranges` ~1× baseline plus JIT/GC noise, so the
  record-free per-instruction cost is the meaningful figure, and it is band-gated (`BAND_MAX=50`).

## Recommended first milestone

**Increment 2 — defeat the private-loader blocker — LANDED 2026-07-13.** It was the smallest
independently-landable slice and the single-point dependency gating everything downstream, and it
resolved cleanly: the pinned DR 11.91.20630 prebuilt `drmgr`/`drreg`/`drx` load under the private
loader on glibc 2.39 (option (c) version-pin), and the license question resolved to a split
(`drmgr`/`drreg`/`drx` BSD; **`umbra` LGPL-2.1**). Artifacts:
[drclient/probe_extensions.c](../../../../drclient/probe_extensions.c),
`make docker-drext-probe` / CI `drext-probe`, findings in
[dr-extension-load-probe-findings.md](../../analysis/dr-extension-load-probe-findings.md).

*(As written, the next step from here was: "**Next: Increment 3 — re-platform the L0 value client
onto inlined `drmgr`/`drreg`/`drx_buf` instrumentation** … the **L** central lift and highest-risk
single increment … Before its shadow work (Increment 4) begins, settle the umbra/BSD-shadow license
decision the probe surfaced." Retained for provenance.)*

**Status 2026-07-14 — the sequence is finished; there is no "next" increment.** Increment 3 landed
that day (the re-platform passed the oracle gate 14/14 with a ~2.6× capture-cost drop), the
umbra/BSD-shadow license decision was settled in favour of the **hand-rolled BSD shadow** (no
`umbra` linked), and Increments 4–9 followed. **All nine increments are LANDED and every exit
criterion is MET** — production taint runs at ~11× bare inside the ~10–50× band with a hard CI gate
(`BAND_MAX=50`), a seed reaches a sink over real JIT'd managed code, and taint survives a compacting
GC. Per the archive rule this plan is **ready to archive** to `docs/internal/archive/plans/`, with
the parent [data-flow-tracing-plan.md](data-flow-tracing-plan.md) Phase-5 stub updated to LANDED in
the same change.

**Follow-on work, all optional and none blocking** (each documented at its site above and in-code):
lever 2's **direct-mapped shadow** (a further optimization on the residual propagation cost — *not*
required for the band, since lever 1 alone reached ~11×); **ZMM / VSIB / lane-precise SIMD** flow;
and the known ABI-level limitations (`rflags` value + dead-register slots are clean-call-only; a
`call`'s `rsp`/`rip` is mis-valued across an out-of-region callee; high-byte sub-registers fold to
the low byte).