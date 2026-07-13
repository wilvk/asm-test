# asm-test - DynamoRIO production taint tier (whole-process, in-band managed taint): implementation plan

This is the standalone child plan for **goal (b)** of the data-flow effort — production-grade
taint over live JIT'd managed (.NET) code under DynamoRIO. It splits out of Phase 5 of the
parent [data-flow-tracing-plan.md](data-flow-tracing-plan.md), whose Phase-5 section is now a
pointer stub retaining only the landed-Increment-1 status; the detail lives here. The
source-of-truth analysis note for every overhead figure, launch-vs-attach decision, and
integration-model claim below is [data-flow-capture.md](../analysis/data-flow-capture.md); the
standalone-DynamoRIO-plan precedent this file's shape follows is
[dynamorio-native-trace-plan.md](../archive/plans/dynamorio-native-trace-plan.md).

The shipped tier (Increment 1, **LANDED 2026-07-13**) is an in-process, clean-call **L0 VALUE**
producer: `libasmtest_drval_client.so` records, per instrumented instruction, the GP register
file (`dr_get_mcontext`) plus each explicit memory SOURCE operand's effective address/value
(`decode` + `opnd_compute_address` + `dr_safe_read`) into an app-owned `at_drval_t`,
cross-validated against the emulator oracle
([dataflow_dr_client.c:1](../../../src/dataflow_dr_client.c#L1)). This plan does **not** extend
that client — it *re-platforms* it. The spine of the remaining work is a departure from
DynamoRIO's raw BSD **core API** onto its standard **extension stack**
(`drmgr`/`drreg`/`umbra`/`drx_buf`) with inlined instrumentation, which the client header itself
names as "the Phase-5 END goal" ([dataflow_dr_client.c:20](../../../src/dataflow_dr_client.c#L20)).
Everything else — in-band tag propagation, the launch-under-DR container, GC-move shadow remap,
whole-process breadth, SIMD taint — hangs off that re-platform. The whole point is the overhead
band: an inlined+buffered DBI client sits at the **greenfield ~10–50×** versus ptrace
single-step's **~10³–10⁵×** ([data-flow-capture.md:180](../analysis/data-flow-capture.md#L180)),
so this is the only substrate on which whole-process managed taint is affordable at all.

The plan is written **exit-criteria-first**: each increment is framed by the committed
end-state — *a seed at a source is detected at a sink over real JIT'd managed code, taint
survives a GC, and overhead is measured against a budget* — and works backward to the smallest
buildable, docker-CI-checkable slice, with the emulator L0 oracle
([dataflow_emu.c](../../../src/dataflow_emu.c), the reference oracle for the live capture tiers)
as the standing cross-validation reference at every step. Each increment also carries an explicit
**Effort.** sizing (relative, not absolute) so the eight-increment sequence can be scheduled — the
re-architecting-tier precedent [dynamorio-native-trace-plan.md](../archive/plans/dynamorio-native-trace-plan.md)
closes each phase the same way.

> Status legend: **Increment 1 — in-band L0 VALUE producer** *(LANDED 2026-07-13)* — the
> clean-call per-instruction register/memory value snapshot
> ([dataflow_dr_client.c](../../../src/dataflow_dr_client.c#L1), capture ABI
> [dataflow_dr.h](../../../src/dataflow_dr.h#L87)), cross-validated against the emulator oracle
> ([dataflow_emu.c](../../../src/dataflow_emu.c)), driven in-process by the `dr_valtrace` C
> harness. **Increment 2 — extension-load probe** *(LANDED 2026-07-13)* — the prebuilt
> `drmgr`/`drreg`/`drx` (the `drx_buf` API lives in `drx.h`) load cleanly under DR's private
> loader on glibc 2.39 with the pinned DR 11.91.20630; the documented blocker does **not**
> reproduce, so **option (c) version-pin** is chosen — no build-from-source/static-link owed.
> The license question resolved to a **split**: `drmgr`/`drreg`/`drx` are BSD (DR core), but
> **`umbra` is LGPL-2.1** (it ships in the Dr. Memory Framework, not DR core — only `drfuzz`/
> `drltrace` are its BSD carve-outs), contradicting the plan's permissive-umbra assumption.
> Probe [drclient/probe_extensions.c](../../../drclient/probe_extensions.c); findings
> [dr-extension-load-probe-findings.md](../analysis/dr-extension-load-probe-findings.md); gate
> `make docker-drext-probe` + CI `drext-probe`. **Increments 3–9** *(planned)* — 3 re-platform
> the L0 client onto inlined `drmgr`/`drreg`/`drx_buf` instrumentation
> (regression-gated byte-identical vs the oracle, no taint yet); 4 in-band tag propagation
> (`dst_tag = ∪ src_tags`) + shadow-concurrency policy + seed/sink API; 5 launch-under-DR
> container (`drrun -c … -- dotnet app.dll`) + out-of-process oracle-diff validator; 6
> whole-process breadth + method-range scoping; 7 GC-move `umbra` remap *(hard-gated on the
> still-deferred Phase-4 concrete `{old,new,len}` triple extraction)*; 8 XMM/YMM SIMD taint; 9
> managed seed→sink validation + measured overhead + CI. Update this file as increments land, the
> way [dynamorio-native-trace-plan.md](../archive/plans/dynamorio-native-trace-plan.md) tracks its
> own; keep the parent [data-flow-tracing-plan.md](data-flow-tracing-plan.md) Phase-5 stub's
> status tag in sync with this child.

---

## Goals and non-goals

**Goals**

- Re-platform the DynamoRIO data-flow tier off the per-instruction clean-call recorder
  ([dataflow_dr_client.c:173](../../../src/dataflow_dr_client.c#L173)) onto the standard
  extension stack — `drmgr` phased pass ordering, `drreg` scratch-register/flag reservation,
  `umbra` byte-granular shadow memory, `drx_buf` buffered per-thread trace — i.e. the
  drcachesim/memtrace idiom plus operand values plus inline tag propagation
  (`dst_tag = ∪ src_tags`), targeting the realistic **~10–50×** DBI band
  ([data-flow-capture.md:194](../analysis/data-flow-capture.md#L194)). This stack is named as
  the Phase-5 END goal in the client header itself
  ([dataflow_dr_client.c:20](../../../src/dataflow_dr_client.c#L20)).
- **Defeat the private-loader/glibc blocker first.** The whole re-platform is gated on getting
  the prebuilt release extensions to load under DR's private loader (or side-stepping it) — the
  single documented reason the tier stays on raw core API today
  ([drclient/CMakeLists.txt:19](../../../drclient/CMakeLists.txt#L19),
  [dataflow_dr_client.c:16](../../../src/dataflow_dr_client.c#L16)).
- Ship in-band L2 taint: a per-byte tag shadow propagated inline as `dst_tag = ∪ src_tags`, with
  a **process-global shadow concurrency policy** for the multithreaded managed runtime and a
  managed-facing **seed/sink** surface (a taint source at e.g. a `read()` buffer, a sink at a
  `memcpy` length or a branch condition).
- Stand up a **launch-under-DR container** (`drrun -c libasmtest_drval_client.so -- dotnet app.dll`)
  — a genuinely new in-tree integration path; no `drrun` launcher exists today
  ([data-flow-capture.md:203](../analysis/data-flow-capture.md#L203)) — together with the
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
  ([data-flow-capture.md:226](../analysis/data-flow-capture.md#L226)). Attach also means
  abandoning the cooperative `dr_app_*` model for the `dr_inject` injector — a re-architecture,
  not a flag. Launch owns the process from a clean start.
- Pulling **`drwrap`** back in for function wrapping. The current client already does its own
  marker/arg resolution via `dr_get_proc_address` + a SysV-arg clean call
  ([dataflow_dr_client.c:64](../../../src/dataflow_dr_client.c#L64),
  [:156](../../../src/dataflow_dr_client.c#L156)); the re-platform must preserve that, never
  adopt `drwrap_wrap`/`drwrap_get_arg`.
- **Re-attach cycling.** One DR lifecycle per process stays the contract — the header flags
  in-process re-attach as unreliable
  ([asmtest_drtrace.h:86](../../../include/asmtest_drtrace.h#L86)); launch-under-DR fits it.
- macOS / arm64 / Windows. Linux x86-64 only, matching the pinned DR image
  (`DR_VERSION=11.91.20630`, [Dockerfile.drtrace:35](../../../Dockerfile.drtrace#L35)).
- Any claim of the ~10–50× band for SIMD-heavy code until Increment 8 measures it — even libdft's
  low numbers *skip* XMM/SSE/MMX (a coverage tradeoff), so the band may not hold once `umbra`
  covers vector state ([data-flow-capture.md:356](../analysis/data-flow-capture.md#L356)).
- Replacing the shipped ptrace L0 tier or the emulator oracle — those remain the correctness
  references, not competitors.

---

## Design overview

| Piece | Already present | Gap |
|---|---|---|
| Instrumentation model | Per-instruction **clean call** `dr_insert_clean_call(…, on_step, …)` inserted before every in-range instr ([dataflow_dr_client.c:173](../../../src/dataflow_dr_client.c#L173)) on raw `dr_register_bb_event` ([:227](../../../src/dataflow_dr_client.c#L227)) | Replace with **inlined** instrumentation under `drmgr` pass ordering + `drreg` scratch regs/flags; the clean call per instr is precisely what makes the shipped increment slow ([data-flow-capture.md:198](../analysis/data-flow-capture.md#L198)) |
| BB event | Raw `dr_register_bb_event(event_bb)` ([dataflow_dr_client.c:227](../../../src/dataflow_dr_client.c#L227)); `event_bb` at [:149](../../../src/dataflow_dr_client.c#L149) | `drmgr` phased events (app2app / analysis / insertion) so value capture + tag propagation compose as separate, ordered passes |
| Extension stack | **None** — raw BSD core API only, by policy ([drclient/CMakeLists.txt:19-21](../../../drclient/CMakeLists.txt#L19)) | `drmgr`+`drreg`+`umbra`+`drx_buf`, blocked today by the private-loader/glibc load failure — Increment 2 must defeat it |
| Value capture | Register file (`dr_get_mcontext`, [:89](../../../src/dataflow_dr_client.c#L89)) + explicit memory **source** EA/value (`decode`+`opnd_compute_address` [:122](../../../src/dataflow_dr_client.c#L122)+`dr_safe_read`) into app-owned `at_drval_t` ([dataflow_dr.h:87](../../../src/dataflow_dr.h#L87)) | `drx_buf` buffered per-thread flush to amortize the store cost (the memtrace pattern) |
| Taint / shadow | None (values only) | `umbra` byte-granular tag shadow, one `at_tag_t` per app byte; inline `dst_tag = ∪ src_tags`; seed/sink surface. The shadow is **process-global**, written concurrently by every instrumented thread — tolerated-benign-race byte-store policy, set in Increment 4 |
| Operand coverage | Explicit memory **source** loads; far/segmented `fs:`/`gs:`, VSIB vector-gather EA, and store (post-instruction) values all skipped ([dataflow_dr_client.c:104-107](../../../src/dataflow_dr_client.c#L104)) | Store **tags** (a store's *location* already enters def-use even though its value is skipped, [dataflow_dr.h:60-62](../../../src/dataflow_dr.h#L60)); then SIMD lanes (Increment 8) |
| Region registration | One region, learned from an app-emitted marker's SysV args `rdi/rsi/rdx` ([on_marker :64](../../../src/dataflow_dr_client.c#L64), re-instrument via `dr_delay_flush_region` [:70](../../../src/dataflow_dr_client.c#L70), resolved by PC [:188](../../../src/dataflow_dr_client.c#L188)) | Whole-process breadth scoped to **registered method ranges**; a launched `dotnet app.dll` never calls the C-harness marker, so a new registration mechanism is needed |
| Integration model | In-process cooperative `dr_app_*`; header flags in-process re-attach as unreliable ([asmtest_drtrace.h:86](../../../include/asmtest_drtrace.h#L86)); driven by the self-init harness `dr_valtrace` via `ASMTEST_DRVAL_CLIENT` ([native-trace.mk:201](../../../mk/native-trace.mk#L201)) | **Launch-under-DR** container over a live `dotnet` — no `drrun` launcher in-tree; plus an out-of-process validator to consume the results channel |
| Signal coexistence | **Mitigated** — `event_signal` → `DR_SIGNAL_DELIVER` in both clients ([dataflow_dr_client.c:213-217](../../../src/dataflow_dr_client.c#L213); rationale in the control client [drtrace_client.c:386](../../../src/drtrace_client.c#L386)) so .NET null-check `SIGSEGV` reaches the runtime | Carries over unchanged to launch; the **code-cache/JIT-collision half** is only *asserted* solved by launch, not demonstrated — a hypothesis first tested in Increment 5 |
| GC-move survival | Phase-4 `GcMoveMap` captures `GCBulkMovedObjectRanges` (DETECTION landed 2026-07-13) | Concrete `{old,new,len}` triple extraction + `umbra` remap **deferred** ([data-flow-tracing-plan.md:193](data-flow-tracing-plan.md#L193)) — Increment 7 hard-depends on it |
| Oracle-diff validation | In-process `dr_valtrace` replays `at_drval_t` through the shared spine and diffs the emulator oracle in the same address space | Under launch the client no longer hosts the diff — a **separate app-side validator** must drain the shm channel and run the replay/diff out-of-process (Increment 5) |
| Overhead evidence | None on this repo's managed workload; all figures external literature | Increment 9 must measure on a representative `dotnet` workload against an explicit budget |

**Contract.** The re-platform keeps the *capture ABI* (`at_drval_t`,
[dataflow_dr.h:87-96](../../../src/dataflow_dr.h#L87)) and the app-side replay /
cross-validation path stable, and changes only *how* the client fills it (inlined + buffered
instead of clean-call append) plus *adds* the taint shadow and seed/sink surface. Increment 3
must reproduce `at_drval_t` **byte-identically** and be regression-gated against the emulator
oracle ([dataflow_emu.c](../../../src/dataflow_emu.c)) *before* any taint semantics land; taint
is an **additive** shadow surface, not a change to the value record. Because the in-band capture
path feeds the same record-building code as the scoped ptrace producer
([dataflow_ptrace.c](../../../src/dataflow_ptrace.c)) and the emulator oracle, every increment
ships with an oracle-diff gate so the inlined/taint path never silently diverges from the
offline slice ([dataflow.c](../../../src/dataflow.c), the shared L1/L2 spine). Through Increment 4
that diff runs **in-process** (the `dr_valtrace` harness replays and diffs in the same address
space); from Increment 5 the diff *consumer* moves **out-of-process** — a separate app-side
validator drains the shared-memory results channel and runs the same replay/diff — but the gate
itself is unchanged.

**Target stack — each component replaces one hand-rolled thing.** (1) **`drmgr`** replaces the
raw `dr_register_bb_event` ([:227](../../../src/dataflow_dr_client.c#L227)) with drmgr's *phased*
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
[dr-extension-load-probe-findings.md](../analysis/dr-extension-load-probe-findings.md)).* The
picture is a **split**, not the uniform "avoid only `drwrap`" the plan first assumed:
- `drmgr`, `drreg`, and `drx` (which is where the `drx_buf` trace-buffer API lives — there is
  **no** separate `drx_buf` extension) are DR-**core** `ext/` extensions covered by DR's primary
  **BSD** license. The re-platform (Increment 3) can adopt them and stay LGPL-clean, **as long as
  the client keeps doing its own PC-resolved marker/arg resolution**
  ([:64](../../../src/dataflow_dr_client.c#L64), [:156](../../../src/dataflow_dr_client.c#L156))
  rather than `drwrap_wrap`/`drwrap_get_arg`.
- **`umbra` is LGPL-2.1** — it is **not** a DR-core extension; it ships under `drmemory/drmf/`
  as part of the Dr. Memory Framework (primary license LGPL; only `drfuzz`/`drltrace` are BSD
  carve-outs, `umbra` is not), and `umbra.h` carries the LGPL-2.1 header. So `umbra` sits with
  `drwrap`, not with the BSD stack. The earlier "`drmgr`/`drreg`/`umbra`/`drx_buf` are not
  `drwrap` → all LGPL-clean" reasoning was **wrong for `umbra`**.

Consequence: the byte-granular tag shadow (Increments 4/7) must either **hand-roll a BSD
direct-mapped shadow** (DR-core `dr_raw_mem_alloc` + a scale-down map) — *recommended*, keeps the
tier fully BSD — or **accept LGPL-2.1 for `umbra`** (dynamic-link relink obligation; static-link
inherits the stricter form, [dynamorio-native-trace-plan.md:914](../archive/plans/dynamorio-native-trace-plan.md#L914)).
Increment 4 now carries that decision explicitly instead of assuming permissive `umbra`.

---

## Public API / capture-ABI sketch

The value capture ABI already exists and is unchanged: `at_drval_t` / `at_vstep_t` / `at_vmem_t`
in [dataflow_dr.h:63-96](../../../src/dataflow_dr.h#L63), plus the marker symbol
`AT_DRVAL_MARKER_SYM` ([dataflow_dr.h:33](../../../src/dataflow_dr.h#L33)). Taint adds a **new
seed/sink surface**. Everything below is **proposed** (new header
`include/asmtest_taint.h`, **does not exist yet**) and, like `dataflow_dr.h`, must stay
`<stdint.h>`/`<stddef.h>`-only so the client can include it alongside `dr_api.h` — no
`<stdbool.h>`, whose `bool` clashes with DynamoRIO's own
([dataflow_dr.h:20](../../../src/dataflow_dr.h#L20)).

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
`dr_get_proc_address` ([dataflow_dr_client.c:188](../../../src/dataflow_dr_client.c#L188)) + a
`drmgr` insertion-phase equivalent of today's `on_marker`/`on_step` SysV-arg clean call
([:64](../../../src/dataflow_dr_client.c#L64),
[:156](../../../src/dataflow_dr_client.c#L156)) — never `drwrap_wrap`/`drwrap_get_arg`.

---

## Increment 2 - Defeat the private-loader blocker (extension-load probe) *(LANDED 2026-07-13)*

**Outcome: the blocker does not reproduce.** On glibc 2.39 (Ubuntu 24.04) with the pinned DR
11.91.20630, the prebuilt `drmgr`/`drreg`/`drx` load cleanly under the private loader
(130588 instructions instrumented over `/bin/true`), so **option (c) version-pin** is chosen — no
build-from-source or static-link is owed. The `__memcpy_chk` symptom did not recur. The license
question resolved to a **split**: `drmgr`/`drreg`/`drx` are BSD, but **`umbra` is LGPL-2.1** (Dr.
Memory Framework), which reshapes Increment 4's shadow-memory choice (see License posture above).
Full record: [dr-extension-load-probe-findings.md](../analysis/dr-extension-load-probe-findings.md).
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
  (`DR_VERSION=11.91.20630`, [Dockerfile.drtrace:35](../../../Dockerfile.drtrace#L35) — the same
  pin the per-language bindings image [Dockerfile.drtrace-lang:26](../../../Dockerfile.drtrace-lang#L26)
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
  ([macos-drtrace-plan.md:466](macos-drtrace-plan.md#L466)) and record the observed glibc
  boundary the docs never pin.
- **Confirm `umbra`/`drx_buf` licensing is BSD/permissive** (only `drwrap` is called out as LGPL
  anywhere in-repo) and record it in the tier's license note — the LGPL-clean claim depends on
  it.

**Exit criteria** *(MET 2026-07-13):* the `make docker-drext-probe` target builds and runs the
probe under the pinned DR image and prints a load-success line for `drmgr`+`drreg`+`drx`
(`drx_buf` is drx's trace-buffer API, not a separate extension) with a non-zero
instrumented-instruction count (130588 over `/bin/true`) ✅; the chosen option **(c) version-pin**
and the observed glibc **2.39** are recorded in-tree
([dr-extension-load-probe-findings.md](../analysis/dr-extension-load-probe-findings.md)) ✅; the
BSD stack links **no `drwrap` and no LGPL object**, and the license finding is recorded — with the
correction that **`umbra` is LGPL-2.1** (so it is *excluded* from the BSD gate, opt-in behind
`PROBE_UMBRA`) ✅; CI gained a `drext-probe` job that fails red if any of `drmgr`/`drreg`/`drx`
fails to load ✅; the untouched clean-call client still builds through the additively-edited
`drclient/CMakeLists.txt` (`make drtrace-client` reconfirmed; `make dr-valtrace-test`
[native-trace.mk:191](../../../mk/native-trace.mk#L191) unchanged) ✅.

**Effort.** **S** — a throwaway probe plus one `make`/CI target; the risk was *discovery* (which
of a/b/c loads, the glibc boundary, and the true license of each extension), not code volume.
Realized as S; the load blocker did not reproduce and the umbra=LGPL split was the load-bearing
discovery.

## Increment 3 - Re-platform the L0 value client onto inlined instrumentation *(planned)*

Swap the recorder, not the semantics: reproduce the exact `at_drval_t` capture the clean-call
client produces, but inlined + buffered, and re-validate against the emulator oracle before any
taint rides on top. Still no taint, no dotnet — the current in-process C harness and
emulator-oracle cross-check ([native-trace.mk:201](../../../mk/native-trace.mk#L201)) stay the
test bed. Land as a *new* CMake target so the shipped `asmtest_drval_client` stays intact as the
fallback/oracle during the swap.

- Move `event_bb` ([dataflow_dr_client.c:149](../../../src/dataflow_dr_client.c#L149)) into
  `drmgr`'s analysis + insertion phases; emit the GP-snapshot and memory-source EA/value capture
  ([on_step :77](../../../src/dataflow_dr_client.c#L77)) as **inline** `drreg`-scratch
  instrumentation instead of the `on_step` clean call
  ([:173-177](../../../src/dataflow_dr_client.c#L173)).
- Route captured records through a **`drx_buf`** per-thread buffer with periodic flush, replacing
  the direct append into `at_drval_t`. Preserve the honest-overflow/`truncated` discipline of the
  ABI ([dataflow_dr.h:95](../../../src/dataflow_dr.h#L95)).
- **Re-express marker/arg resolution without `drwrap`:** keep resolving `AT_DRVAL_MARKER_SYM` by
  PC ([:188](../../../src/dataflow_dr_client.c#L188)) and reading `rdi/rsi/rdx` at the marker PC,
  now as a `drmgr` insertion-phase equivalent of the current SysV-arg clean call
  ([:156](../../../src/dataflow_dr_client.c#L156)) — do **not** silently pull `drwrap_get_arg`
  back in.
- Keep the existing operand exclusions honest and unchanged for now (far/segmented `fs:`/`gs:`,
  VSIB gather EA, store post-values, [:104-107](../../../src/dataflow_dr_client.c#L104)); this
  increment is a performance/architecture swap only. Keep the `DR_SIGNAL_DELIVER` handler
  ([:213-217](../../../src/dataflow_dr_client.c#L213)) unchanged.

**Exit criteria:** the inlined+buffered client produces **byte-identical `at_drval_t` records**
to the clean-call client on the existing value-trace fixtures and passes the existing
`make dr-valtrace-test` cross-validation against the emulator oracle
([dataflow_emu.c](../../../src/dataflow_emu.c)) — the same green CI gate, now on the extension
stack; a `make`-driven microbenchmark in the pinned DR Docker lane shows a measurable
per-instruction cost drop vs the clean-call path (direction-of-travel check, not the 10–50× claim
yet). No taint.

**Effort.** **L** — the central re-platform lift and the highest-risk single increment: every
operand-capture path moves from a clean call to inlined `drreg`-scratch + `drx_buf` code and must
reproduce `at_drval_t` **byte-identically** under the existing oracle gate. Correctness bar is
exacting even though no new semantics land.

## Increment 4 - In-band taint: umbra shadow + concurrency policy + seed/sink API *(planned)*

First taint semantics: a byte-granular shadow and inline `dst_tag = ∪ src_tags`, driven
by the proposed seed/sink surface, validated on a **native in-process fixture** — still docker,
still no dotnet, so the launch container (Increment 5) is a separable change.

> **License decision (from Increment 2): do not assume `umbra`.** Increment 2 found `umbra` is
> **LGPL-2.1** (Dr. Memory Framework), not the permissive core extension the plan first assumed
> ([dr-extension-load-probe-findings.md](../analysis/dr-extension-load-probe-findings.md)). Before
> this increment builds the shadow, choose: **(i)** hand-roll a BSD direct-mapped shadow over
> DR-core `dr_raw_mem_alloc` (recommended — keeps the tier fully BSD; read "`umbra` shadow" below
> as "byte-granular shadow" on this path), or **(ii)** accept LGPL-2.1 for `umbra`. The
> `dst_tag = ∪ src_tags` semantics and the concurrency policy below are identical either way; only
> the shadow *provider* changes.

- Allocate a shadow of `at_tag_t` per app byte (1 byte/byte, `AT_TAG_CLEAN = 0`), via the chosen
  provider (BSD direct-map, or `umbra` if (ii)). On
  each instrumented instruction, compute `dst_tag = ∪ src_tags` **inline** in the `drmgr`
  insertion pass over the register + memory operands the enumerator already walks
  ([dataflow_operands.c](../../../src/dataflow_operands.c), the shared enumerator behind
  [dataflow_dr.h](../../../src/dataflow_dr.h)); key the memory-dst shadow on the same
  `opnd_compute_address` EA math the value path uses
  ([dataflow_dr_client.c:122](../../../src/dataflow_dr_client.c#L122)).
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
  enters def-use ([dataflow_dr.h:60-62](../../../src/dataflow_dr.h#L60)), so propagate the
  source-union tag into the store's destination-address shadow.
- Scope taint to GP + integer-memory operands this increment; the current Increment-1 EXCLUSIONS
  (`fs:`/`gs:`, VSIB gather EA, store values,
  [:104-107](../../../src/dataflow_dr_client.c#L104)) remain known tag gaps, and **XMM/YMM is
  explicitly deferred to Increment 8** (even libdft's low overhead partly comes from *skipping*
  XMM/SSE/MMX, so SIMD is real work, not free).

**Exit criteria:** a new native docker lane *(proposed `make docker-taint-native`)* seeds a color
on a known buffer, runs a hand-written fixture that copies/derives through GP regs and integer
memory, and the sink fires with the correct `seed_off`→`off` and `depth`; the tag graph is
**diffed against the emulator L2 slicer** (the shared spine [dataflow.c](../../../src/dataflow.c)
driven by the [dataflow_emu.c](../../../src/dataflow_emu.c) oracle) on the same fixture; a
**negative control** (unseeded run, or seed not reaching the sink) reports **zero hits**;
propagation is emitted inline (no clean call in the hot path — verifiable by an inscount sanity
check); the shadow-concurrency policy is documented in-tree with its rejected alternatives. Green
in CI with no hardware.

**Effort.** **M–L** — first taint semantics, the shadow-concurrency policy, and the seed/sink ABI
together, but all on a single-threaded native fixture, so it carries no launch/managed/JIT risk.
The concurrency *policy* is cheap to state; its *validation* is deferred to Increment 5 where real
concurrency exists.

## Increment 5 - Launch-under-DR container (drrun -c … -- dotnet app.dll) + out-of-process validator *(planned)*

The new integration path — there is **no** `drrun`/`dr_inject` launcher in-tree today (a clean
grep returns zero matches, [data-flow-capture.md:203](../analysis/data-flow-capture.md#L203)).
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
  mitigation carries over unchanged ([dataflow_dr_client.c:213](../../../src/dataflow_dr_client.c#L213));
  the arbitrary-state problem does not arise because DR owns the process from a clean start.
  **Attach stays out of scope** (record why).
- **New region/method-range registration:** a launched `dotnet app.dll` never calls the C-harness
  marker ([:64](../../../src/dataflow_dr_client.c#L64)). Recommend `drrun` client-option seed/sink
  config **plus a small injected managed shim** that reports the seeded buffer's address (the
  managed source of method ranges is method-load events, wired in Increment 6).
- **Cross-address-space results channel (transport):** back the report buffer with a **POSIX
  shared-memory** segment named via a `drrun` client option (the in-process `at_drval_t*` /
  `at_taint_report_t*` pointer is not valid across the separately-launched process).
- **Out-of-process oracle-diff validator (the shm *consumer*):** in-process today, `dr_valtrace`
  replays `at_drval_t`/hit records through the shared enumerator and spine
  ([dataflow_operands.c](../../../src/dataflow_operands.c) → [dataflow.c](../../../src/dataflow.c))
  and diffs the emulator oracle ([dataflow_emu.c](../../../src/dataflow_emu.c)) in the same
  address space. Under launch the client can no longer host that diff, so name an explicit
  **separate app-side validator process** — the evolved `dr_valtrace` harness, *not* the launched
  `dotnet` — that attaches to the same shm segment, drains the records, and runs the L1/L2 replay
  + oracle diff out-of-process. The launched client is the *producer*; this validator is the
  *consumer*. This is the wiring the "oracle-diff gate at every step" (Contract) and Increment 9
  depend on; it does not exist in-tree and lands here.
- **Client build-mode fallback branch:** verify whether the same `libasmtest_drval_client.so`
  works unmodified under `drrun -c` vs the in-process `configure_DynamoRIO_client` build
  ([drclient/CMakeLists.txt:18](../../../drclient/CMakeLists.txt#L18)). **If a distinct
  launched-runmode build IS required**, it lands *within this increment* as a second small CMake
  target (a `configure_DynamoRIO_client` launched-runmode variant) — an **additive ~S** build
  change, not a re-architecture; the in-process build stays intact as the oracle/fallback. State
  this branch now so the increment is fully scoped either way.
- Treat "launch sidesteps the JIT collision" as a **hypothesis under test** — this is the first
  in-tree exercise of DR's code cache coexisting with .NET tiered-JIT recompilation
  ([data-flow-capture.md:203](../analysis/data-flow-capture.md#L203)).

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

## Increment 6 - Whole-process breadth + method-range scoping *(planned)*

Move beyond Increment 1's single registered region to whole-process taint, but bound cost by
scoping the expensive per-operand shadow work to registered method ranges (the ~10–50× band
assumes we are not tag-tracking the entire runtime).

- Generalize the single `g_region` (the unlocked region read on the hot path,
  [dataflow_dr_client.c:165-167](../../../src/dataflow_dr_client.c#L165)) to a set of registered
  method ranges, re-instrumented via `dr_delay_flush_region`
  ([:70](../../../src/dataflow_dr_client.c#L70)) as ranges arrive.
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

## Increment 7 - GC-move umbra shadow remap *(planned — hard-blocked on Phase 4)*

Make taint survive .NET GC compaction: when the GC moves an object, its shadow tags must move
with it, or a compacting GC silently drops/aliases taint. **Hard dependency:** this needs Phase
4's concrete `GCBulkMovedObjectRanges` `{OldRangeBase, NewRangeBase, RangeLength}` triple (via
EventPipe), which is **still deferred** — the in-proc `EventListener` landed only the DETECTION
feed (`GcMoveMap`) and does not surface the `Values` struct-array
([data-flow-tracing-plan.md:193](data-flow-tracing-plan.md#L193)).

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

**Exit criteria (full):** a launched dotnet workload seeds a color on a heap object, forces a
compacting Gen-2 GC that relocates it, and the sink downstream still fires with the original
`seed_off` at the object's **new** address with **no** stale tag at the old address; the
coherence canary passes; gated in the launch-under-DR CI lane **once Phase 4's `{old,new,len}`
extraction lands**. Until then, the increment ships only the disabled-flag remap path + the
synthetic-triple unit test green in CI, and the plan says so rather than faking a remap.

**Effort.** **M** for the code, but **externally hard-blocked** on Phase 4's still-deferred
`{old,new,len}` extraction, so the landable slice *now* is only the disabled-flag remap path + the
synthetic-triple unit test (**S**). The full remap + canary is unblocked only when Phase 4 surfaces
the triple.

## Increment 8 - XMM/YMM (SIMD) taint *(planned)*

Genuine research, not a checkbox — even **libdft punts on XMM/SSE/MMX**, and its cited low
overhead (1.14–6.03× utils) is partly *that coverage tradeoff*
([data-flow-capture.md:356](../analysis/data-flow-capture.md#L356)) — so the ~10–50× band may not
hold once `umbra` covers vector state. Scoped as its own increment with its own overhead
measurement.

- Extend the shadow + `dst_tag = ∪ src_tags` rules to XMM/YMM registers and SSE/AVX
  loads/stores/shuffles; handle the VSIB vector-gather EA math the value client currently skips
  ([dataflow_dr_client.c:104-107](../../../src/dataflow_dr_client.c#L104)). Decide lane
  granularity (per-byte vs per-lane tags) explicitly — per-byte matches the integer shadow but
  multiplies shadow traffic.
- **Re-measure** overhead with SIMD taint on vs off; if the band does not hold, scope SIMD taint
  behind a flag and record the coverage/cost tradeoff explicitly rather than silently
  under-covering.

**Exit criteria:** a native docker fixture that flows taint through an XMM/YMM register and an
SSE/AVX vectorized copy/reduce shows the sink firing with the expected tag — SIMD taint is not
silently dropped; the SIMD-on vs SIMD-off overhead delta is reported separately from the scalar
band; CI gates the SIMD taint fixture green with the tradeoff documented.

**Effort.** **L** — genuine research (SIMD taint is under-solved industry-wide), with a real risk
of blowing the band and a per-byte-vs-per-lane granularity call that multiplies shadow traffic;
carries its own overhead re-measurement rather than inheriting the scalar band. Given equal ORDER
weight to earlier increments but not equal scope — sequenced late for exactly this reason.

## Increment 9 - Managed seed→sink end-to-end validation + overhead + CI *(planned — the committed exit state)*

The Phase-5 exit gate the whole plan works backward from: everything above composed into a real
managed data-flow assertion with a measured cost, replacing the offset-only dotnet smoke
([bindings/dotnet/drtrace/](../../../bindings/dotnet/drtrace/)).

- A launched dotnet workload with a taint **seed at a source** (a `read()`/`recv()`/stream
  buffer) **detected at a sink** (a `memcpy` length or a branch condition) over **real JIT'd
  managed code**, with taint **surviving a GC** (Increment 7), the verdict produced by the
  out-of-process validator (Increment 5).
- **Overhead measured against an explicit budget.** No number exists on *this repo's* managed
  workload today — all cited figures are external literature: the greenfield ~10–50× band, libdft
  1.14–6.03× utils / 1.25–4.83× servers, TaintTrace ~5.5×, bare-DR inscount ~15× on SPEC CPU
  2017, fast-path Taint Rabbit 1.7× / Sdft 1.58× / HardTaint ~9%
  ([data-flow-capture.md:356-360](../analysis/data-flow-capture.md#L356)). Set the budget at the
  ~10–50× band, report the measured slowdown vs a bare `dotnet` run, and flag if SIMD taint
  (Increment 8) pushes past it. This is the number that justifies the tier over ptrace
  single-step's ~10³–10⁵× ([data-flow-capture.md:180](../analysis/data-flow-capture.md#L180)).
- **CI wiring:** a new `dr-taint` docker lane / ci.yml job replacing the in-process
  `make drtrace-test` smoke with the `drrun` invocation over a live dotnet workload, asserting
  seed→sink + GC-survival, with a **hard gate** that build-fails on non-detection or overhead
  regression past a set threshold; the seed→sink verdict is cross-checked against the emulator
  L0/L2 oracle on a shared fixture by the out-of-process validator.

**Exit criteria:** CI runs a launched dotnet workload under DR where a seed at a source is
detected at a sink over real JIT'd managed code, **taint survives a GC**, and the **measured
overhead is reported against the ~10–50× budget** (build fails on non-detection or on overhead
regressing past the threshold); the verdict is cross-checked against the emulator oracle
([dataflow_emu.c](../../../src/dataflow_emu.c)) on a shared fixture — matching
[data-flow-tracing-plan.md:215](data-flow-tracing-plan.md#L215) and validating beyond the
offset-only `bindings/dotnet/drtrace/` smoke. On landing, this plan moves to
`docs/internal/archive/plans/` per the archive rule and the parent Phase-5 stub is updated to
LANDED in the same change.

**Effort.** **M** in code — mostly composition of 5–8 plus the CI/threshold wiring — but gated on
those increments landing; the real work here is the measurement harness and the budget/threshold
hard gate, not new instrumentation.

---

## Implementation status (landed)

- ✅ **Increment 1 — in-band L0 VALUE producer** *(LANDED 2026-07-13)*:
  `libasmtest_drval_client.so` — a clean-call per-instruction producer capturing the GP register
  file (`dr_get_mcontext`) + explicit memory SOURCE effective addresses/values (`decode` +
  `opnd_compute_address` + `dr_safe_read`), cross-validated against the emulator oracle. Source
  [dataflow_dr_client.c:1-28](../../../src/dataflow_dr_client.c#L1); capture ABI
  [dataflow_dr.h:63-96](../../../src/dataflow_dr.h#L63); oracle
  [dataflow_emu.c](../../../src/dataflow_emu.c); driven in-process via `ASMTEST_DRVAL_CLIENT`
  ([native-trace.mk:201](../../../mk/native-trace.mk#L201)), gated by
  `make dr-valtrace-test` (target defined at [native-trace.mk:191](../../../mk/native-trace.mk#L191)).
- ✅ **Increment 2 — extension-load probe** *(LANDED 2026-07-13)*: the prebuilt
  `drmgr`/`drreg`/`drx` load under DR's private loader on glibc 2.39 / pinned DR 11.91.20630
  (blocker does not reproduce → **option (c) version-pin**); license resolved to a split
  (`drmgr`/`drreg`/`drx` BSD, **`umbra` LGPL-2.1**). Probe
  [drclient/probe_extensions.c](../../../drclient/probe_extensions.c); findings
  [dr-extension-load-probe-findings.md](../analysis/dr-extension-load-probe-findings.md); gates
  `make drext-probe` / `make docker-drext-probe` ([native-trace.mk](../../../mk/native-trace.mk),
  [docker.mk](../../../mk/docker.mk)) + CI `drext-probe`.
- ⬜ Increments 3–9 — planned (this document).

## Validation notes

- *Code-checked:* the hot path is a per-instruction clean call
  ([dataflow_dr_client.c:173-177](../../../src/dataflow_dr_client.c#L173)) on raw
  `dr_register_bb_event` ([:227](../../../src/dataflow_dr_client.c#L227)); the client uses raw
  core API only, **no** `use_DynamoRIO_extension()`, with the private-loader blocker recorded
  verbatim in the build file ([drclient/CMakeLists.txt:19-21](../../../drclient/CMakeLists.txt#L19))
  and the client header ([dataflow_dr_client.c:16-23](../../../src/dataflow_dr_client.c#L16),
  which also names the `drreg`+`umbra` stack as the Phase-5 END goal).
- *Code-checked:* the signal handler returns `DR_SIGNAL_DELIVER`
  ([:213-217](../../../src/dataflow_dr_client.c#L213)); region registration is marker-driven from
  SysV args ([on_marker :64-71](../../../src/dataflow_dr_client.c#L64),
  [event_bb insertion :156](../../../src/dataflow_dr_client.c#L156), PC-resolved
  [:188](../../../src/dataflow_dr_client.c#L188)); SIMD/far/VSIB/store operands are skipped
  ([:104-107](../../../src/dataflow_dr_client.c#L104)).
- *Code-checked:* the emulator oracle is [dataflow_emu.c](../../../src/dataflow_emu.c) ("the
  reference oracle for the live capture tiers"), **not** [dataflow.c](../../../src/dataflow.c),
  which is the shared L1/L2 (def-use / slicer) spine; no `drrun`/`dr_inject` launcher exists
  in-tree — the only DR entry is the in-process `dr_app_*` cooperative lifecycle
  ([asmtest_drtrace.h:78-93](../../../include/asmtest_drtrace.h#L78)). In-process, `dr_valtrace`
  hosts the replay/diff in the same address space; the out-of-process validator (Increment 5) is
  net-new.
- *Docs-checked:* the extension stack, overhead band, and launch-vs-attach framing are the
  authoritative analysis note
  ([data-flow-capture.md:180-241](../analysis/data-flow-capture.md#L180),
  [:356-363](../analysis/data-flow-capture.md#L356)); the license separability of `drwrap` is
  [dynamorio-native-trace-plan.md:224-226](../archive/plans/dynamorio-native-trace-plan.md#L224)
  and the static-link nuance [:914](../archive/plans/dynamorio-native-trace-plan.md#L914); the
  `__memcpy_chk` loader symptom is [macos-drtrace-plan.md:466](macos-drtrace-plan.md#L466); the
  `MethodLoadVerbose` event id 143 addr-channel and the GC-move deferral are
  [data-flow-tracing-plan.md:199](data-flow-tracing-plan.md#L199) and
  [:193](data-flow-tracing-plan.md#L193).

## Risks and open points

- **The private-loader blocker is documented, never solved.** Every source records it
  generically ("modern glibc") with no tested version boundary
  ([drclient/CMakeLists.txt:19-21](../../../drclient/CMakeLists.txt#L19),
  [macos-drtrace-plan.md:466](macos-drtrace-plan.md#L466)); no build-from-source / static-link /
  version-pin has been attempted. Increment 2 must turn it into an empirical yes/no per option —
  it is the single-point dependency for the entire plan, and if none of (a)/(b)/(c) loads cleanly
  the re-platform stalls.
- **License angle.** Only `drwrap` is LGPL-2.1 and is separable from the target stack; options
  (a)/(b)/(c) keep the tier LGPL-clean, but **static-link (b) carries the stricter obligation**
  ([dynamorio-native-trace-plan.md:914](../archive/plans/dynamorio-native-trace-plan.md#L914)) —
  it only bites if an LGPL object is in the set, which it is not, so (b) stays clean but must
  contain no LGPL object. `umbra`/`drx_buf` licensing is **not asserted anywhere in-repo**;
  Increment 2 must confirm them BSD/permissive before the license-clean claim stands.
- **Marker/arg resolution must stay `drwrap`-free.** The re-platform must re-express the SysV-arg
  marker capture ([:64](../../../src/dataflow_dr_client.c#L64),
  [:156](../../../src/dataflow_dr_client.c#L156)) in the `drmgr` world without reaching for
  `drwrap_get_arg`, or the tier silently reacquires the LGPL-2.1 obligation (option d) it is
  trying to avoid.
- **Process-global shadow under a multithreaded runtime.** The `umbra` tag shadow is one
  process-wide byte map every instrumented .NET thread writes concurrently (app + GC threads);
  `drx_buf` is per-thread but the shadow is not. The committed policy is **tolerated-benign-race
  single-byte tag stores** — aligned `at_tag_t` writes are atomic on x86-64, and a union tag is
  monotone within a seed epoch, so a lost update is a conservative *miss*, never a false taint —
  decided and implemented in Increment 4 and first stress-tested against real concurrent writers
  in Increment 5. A global hot-path lock is rejected (blows the band); per-thread shadows are
  rejected (cannot express cross-thread flows). The GC-move remap (Increment 7) is the one bulk
  shadow mutation that must be fenced against in-flight stores.
- **Scale vs the ~10–50× band.** The band is the *greenfield* figure, not the shipped tier
  ([data-flow-capture.md:194-195](../analysis/data-flow-capture.md#L194)); it holds only for a
  properly inlined+buffered client. Increment 3's re-platform is what earns it, and Increment 9
  is the first place it is measured on this repo — with a budget/threshold, not a bare report.
- **SIMD is under-solved industry-wide.** libdft's cheapness partly comes from *skipping*
  XMM/SSE/MMX ([data-flow-capture.md:356-360](../analysis/data-flow-capture.md#L356)); Increment 8
  may blow the band and forces an explicit coverage/cost tradeoff rather than a silent gap.
- **GC-move coherence + external block.** A missed `GCBulkMovedObjectRanges` event silently
  aliases pre/post-move taint; Increment 7 needs a coherence canary, and it is **hard-blocked** on
  Phase 4's still-deferred `{old,new,len}` extraction
  ([data-flow-tracing-plan.md:193](data-flow-tracing-plan.md#L193)) — mitigated by the disabled-
  flag + synthetic-triple partial path so the chain does not fully stall.
- **DR-over-managed heaviness / JIT collision is asserted, not demonstrated.** "Launch owns the
  process from a clean start, so DR's code cache coexists with .NET tiered-JIT" has no in-tree
  evidence ([data-flow-capture.md:203-209](../analysis/data-flow-capture.md#L203)); Increment 5 is
  the first real test and may surface tiered-recompilation/dynamic-codegen problems.
- **Out-of-process validator wiring is net-new.** In-process, `dr_valtrace` replays and diffs the
  oracle in the same address space; once the client is a separately-launched dotnet process
  writing to shm, a *distinct* app-side validator must own the replay/diff. The plan pins the shm
  *transport* and names the validator as the *consumer* (Increment 5), but neither exists in-tree
  and both are first exercised there.
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
- **No in-repo overhead number.** Every cited figure is external literature; the tier's cost
  claim is unproven on this repo's managed workload until Increment 9 measures it against the
  budget.

## Recommended first milestone

**Increment 2 — defeat the private-loader blocker — LANDED 2026-07-13.** It was the smallest
independently-landable slice and the single-point dependency gating everything downstream, and it
resolved cleanly: the pinned DR 11.91.20630 prebuilt `drmgr`/`drreg`/`drx` load under the private
loader on glibc 2.39 (option (c) version-pin), and the license question resolved to a split
(`drmgr`/`drreg`/`drx` BSD; **`umbra` LGPL-2.1**). Artifacts:
[drclient/probe_extensions.c](../../../drclient/probe_extensions.c),
`make docker-drext-probe` / CI `drext-probe`, findings in
[dr-extension-load-probe-findings.md](../analysis/dr-extension-load-probe-findings.md).

**Next: Increment 3 — re-platform the L0 value client onto inlined `drmgr`/`drreg`/`drx_buf`
instrumentation** (byte-identical `at_drval_t` under the existing emulator-oracle gate, no taint
yet). It is now unblocked: the BSD extension stack demonstrably loads, so the swap needs no loader
workaround. It is the **L** central lift and highest-risk single increment — see its section
above. Before its shadow work (Increment 4) begins, settle the umbra/BSD-shadow license decision
the probe surfaced.