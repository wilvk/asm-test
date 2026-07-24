# asm-test — Desktop GUI plan

> **Context (2026-07-23).** The committed plan derived from two adversarially-reviewed
> ideation docs — [desktop-GUI ideation](../analysis/2026-07-20-desktop-gui-ideation.md)
> and [capability-expansion ideation](../analysis/2026-07-20-capability-expansion-ideation.md)
> — after cross-reviewing both against the tree at HEAD (`51b8247`). Every constraint
> and correction below was re-verified against the code on 2026-07-23; the
> [cross-review record](#cross-review-record-2026-07-23) at the end lists what drifted
> since the ideation snapshots and what was corrected. Companion state:
> [post-v1 expansion plan](post-v1-expansion-plan.md) (Tracks A–F),
> [asmspy plan](asmspy-plan.md) (the TUI/CLI view family this GUI extends),
> [tracing decision matrix](../analysis/tracing-decision-matrix.md).
>
> **Product goal.** A desktop GUI with a **low barrier to entry** (first value with no
> root, no hardware, no attach), that **facilitates deep inspection of binaries**
> (live processes, emulated routines, recorded traces), **teaches assembly** (the
> classroom story made visual), and is **functional for professional users**
> (SIMD/crypto authors, perf engineers, reverse engineers, CI operators) — with
> explicit expansion sockets for the capability waves as they land.

> Status legend: everything here is **planned** unless marked otherwise. Update this
> file as phases land, the way [post-v1-expansion-plan.md](post-v1-expansion-plan.md)
> tracks its tracks; archive it when done, per the
> [archive rule](../README.md).
>
> **Pointer (2026-07-24):** the implementation-ready breakdown of this plan —
> ten briefs (nine core + one growth-rung companion), 70 tasks, with the binding
> build decisions (the GUI ships under `desktop/`, integrated via
> `mk/desktop.mk`) — lives in [docs/internal/gui/](../gui/README.md). The
> companion, [10-spacetime-3d-overview.md](../gui/10-spacetime-3d-overview.md),
> reunifies the killed [Terrane and Observatory](#flagship-interaction-concept-the-loom)
> concepts as a **3D overview surface** (memory terrain + execution
> trajectories, "3D to find, 2D to read") — an orientation view you drill *out
> of* into the 2D reading views, gated on the Wave-1 `mem[]` stream for its rich
> rung.

## Summary

The GUI is a **renderer + driver over feeds that already exist** — the ideation doc's
central inversion ("the repo needs a protocol, not renderings") survives cross-review
and is adopted here: define the **`.asmtrace` recording format first**, ship the
def-use slice explorer on the emulator producer second, add live `--serve` third, and
fund teaching producers fourth.

The cross-review question — **would the capability expansions change the GUI design?**
— has a precise answer: **the architecture survives, but three expansion themes are
design inputs, not future add-ons.** They force (1) a versioned, manifest-driven
event schema instead of one hard-coded to today's structs, (2) run-vs-run **diffing
as a first-class primitive** rather than a later feature, and (3) a **failure →
trace → slice navigation spine** that upgrades test results from "commodity
dashboard, skip" to the front door of Author mode. Additionally, three days of repo
drift since the ideation snapshots (a silicon-validated PT-replay dataflow producer,
stitched per-tid managed PT slices, the arm64 watchpoint arming gate, macOS
DynamoRIO) materially amend the view catalog and platform matrix — all folded in
below.

The plan's flagship interaction concept is [**the Loom**](#flagship-interaction-concept-the-loom):
a recorded run as a spacetime fabric of value worldlines — select any value to
light its full lineage, open any location's cross-instrument annex, fork the run
on one edited fact and see which dependents actually cared. It composes the
catalog's views into one surface, has a day-one rung entirely on existing feeds,
and ties each growth rung to a named planned item.

## Verdict: do the expansions change the GUI design?

### What survives unchanged

- **Protocol before renderings.** `.asmtrace` (NDJSON events + metadata header)
  first; `--serve` is "the same events, live". *(Was "verified still greenfield"
  when this plan was written; as of 2026-07-24 the `.asmtrace` writer,
  `--record` on every headless mode, and JSON mode for `--log`/`--stream` have
  **landed** — [01-asmtrace-format.md](../gui/01-asmtrace-format.md). `--serve`
  is still greenfield: [07-serve-live-host.md](../gui/07-serve-live-host.md).)*
- **Dear ImGui + C ABI (Tracy model), not a webview.** The packaging manifests
  independently confirm it: of the ten bindings, **only Python ships the def-use/
  slice *wrapper module* in a published artifact** ([bindings/python/asmtest/dataflow.py](../../../bindings/python/asmtest/dataflow.py)
  is in the wheel — but the wheel bundles no `libasmtest_dataflow.so`, so even
  Python's published slice surface needs an out-of-band build of the analysis
  lib; npm/gem/rock/CMake all exclude their in-tree dataflow modules —
  [bindings/node/package.json](../../../bindings/node/package.json) `files`,
  [bindings/ruby/asmtest.gemspec](../../../bindings/ruby/asmtest.gemspec),
  [bindings/lua/asmtest-1.1.0-1.rockspec](../../../bindings/lua/asmtest-1.1.0-1.rockspec),
  [bindings/cpp/CMakeLists.txt](../../../bindings/cpp/CMakeLists.txt) — and Rust/Go/
  Java/Zig/.NET have smoke-driver-only surfaces). The C ABI reaches everything;
  the pure view-model headers ([cli/asmspy_logview.h](../../../cli/asmspy_logview.h),
  [asmspy_treefilter.h](../../../cli/asmspy_treefilter.h),
  [asmspy_dataview.h](../../../cli/asmspy_dataview.h),
  [asmspy_autoregion.h](../../../cli/asmspy_autoregion.h)) `#include` directly under
  their existing unit tests.
- **Author / Observer product split.** Author mode drives the library
  (emulator, valtrace, artifacts) everywhere; Observer mode is live attach on
  Linux x86-64 + AArch64 via the asmspy engines.
- **The crown jewel.** The def-use slice explorer remains the differentiated view
  no other tool offers, seeded by the conformance corpus
  ([bindings/conformance/](../../../bindings/conformance/)).
- **The expansion wave rankings themselves.** Re-verified at HEAD: **no Wave 1–4
  item has landed** (no `mxcsr`/`fpcr` in any user-facing struct, no `mem[]`
  stream in [include/asmtest_trace.h](../../../include/asmtest_trace.h), no
  `ASSERT_CT_*`/`ASSERT_EQUIV` symbols in `include/`, and no FP-*environment*
  assertion family — `ASSERT_FP_EXCEPTIONS_EQ`/`ASSERT_ROUNDING_HONORED` etc.
  are absent; the long-standing `ASSERT_FP_EQ`/`ASSERT_FP_NEAR` *value* asserts
  in [include/asmtest.h](../../../include/asmtest.h) are not the Wave 1 FP-env
  item), so the GUI schedules *sockets* for them, not dependencies on them.

### The three design-level changes the expansions force

1. **The schema must be extensible and manifest-driven, or every wave item is a
   breaking change.** The expansion doc's two highest-ranked items are the same
   move — exposing state that is captured internally but withheld from user-facing
   structs (MXCSR/FPCR; the memory-address channel). A `.asmtrace` v1 that
   hard-codes today's `emu_x86_regs_t` shape would need a v2 the day Wave 1 lands.
   Therefore: **register-state events carry a state-descriptor reference, not a
   fixed struct**; descriptors extend the `asmtest_abi.json` idiom (layouts as
   data), and the event-kind registry reserves kinds for FP-environment state,
   memory-address streams, and wide/scalable vector state up front. (Work item:
   `vec512_t` is absent from the manifest today — add it before freezing
   descriptors.)
2. **Diff is a primitive, not a feature.** Three separately-ranked expansion
   items — plus the GUI ideation doc's own before/after-optimisation recording
   diff — are all "compare two runs of the same code": the full-state
   observational-equivalence oracle (`ASSERT_EQUIV`, Wave 2), golden-state
   snapshot testing (Wave 2), and secret-dependent-trace invariance (Wave 1). One diff engine over `.asmtrace`
   recordings (align two event streams, classify divergence: value / address /
   control-flow / FP-env) serves all four when their producers land, and serves
   coverage/hot-edge/cycles diffs today. Designing the viewer around
   single-recording rendering and bolting comparison on later would rebuild this
   four times.
3. **Test results become the navigation spine, not a commodity dashboard.** The
   ideation doc scored the test dashboard "minimal or skip" — correct as a
   *dashboard*. But Wave 2's backward-slice "blame the instruction" wires
   assertion failures to the slice engine, which means a failing test is the
   natural *entry point* into the crown-jewel view: click a failure → its
   recording → the trace step that defined the bad value → the backward cone.
   The GUI therefore treats every failure as a deep link from day one (failure →
   recording → step), so the blame integration is a producer landing, not a UI
   redesign. This doubles as the teaching story: "why is rax wrong" is the
   classroom explainer.

### Drift riders (repo movement since 2026-07-20 that amends the design)

- **The slice explorer has a second, Observer-mode, zero-perturbation producer.**
  The F5 PT-replay tier ([src/dataflow_pt.c](../../../src/dataflow_pt.c) — the
  producer landed 2026-07-19, a day *before* the ideation doc, which missed it;
  its live foreign-pid wiring (`68a2fe7`) landed ~2.6 h after the doc, and the
  tier was silicon-validated on the i7-8559U PT box on 2026-07-21) replays a
  PT-captured foreign-pid path through the value engine with **zero single-steps
  of the target**, byte-identical to the emulator L0 oracle. On PT hosts the
  crown-jewel view is live-capable without perturbation. The view must be
  producer-agnostic from the start.
- **Concurrency budget corrected.** The ideation doc's constraint 2 said IBS
  *and watchpoints* coexist with a ptrace view. Wrong on watch:
  `asmspy_engine_watch` **is itself a ptrace tracer** (it SEIZEs every thread —
  [cli/asmspy_engine.c](../../../cli/asmspy_engine.c) `seize_threads` in the watch
  engine), so the kernel's one-tracer rule applies — and the topology engine is
  a ptrace tracer too (`asmspy_engine_procs` SEIZEs the whole descendant tree). The corrected
  budget per target: **exactly one of** {syscalls, stream, graph, tree, trace,
  dataflow, watch, topology} live at a time, **plus** any of: AMD IBS-Op sampling (perf, no ptrace),
  the portable software-clock survey (perf, no ptrace), and — new since the doc —
  **foreign-pid Intel PT capture** (`asmtest_hwtrace_pt_attach_*`, zero
  single-steps) on PT hosts.
- **A sixth renderable shape exists.** The managed whole-window compose work
  (✅ 12/12) emits **stitched sets of per-tid PT slices** per scope
  (`stitch_handles` — the .NET ambient AsyncLocal escalation). The ideation doc's
  five-shape substrate table predates it. The `.asmtrace` registry reserves a
  stitched multi-slice event kind before the v1 freeze, plus the armed
  trust-ladder tier (WEAK/STRONG) in provenance.
- **The watchpoint timeline is host-gated on AArch64.** On the available arm64
  host class, `NT_ARM_HW_WATCH` advertises 4 slots yet `PTRACE_SETREGSET` returns
  ENOSPC (commit `9184c14`); asmspy now names the measured refusal via
  `asmspy_hwdebug_reason()`. The "sleeper hit" view ships with a per-host arming
  probe and renders the measured reason — Observer-on-AArch64 is real, but this
  view inside it is x86-64-only on such hosts.
- **macOS Author mode is stronger than the doc assumed.** The DynamoRIO tier now
  builds and runs on macOS Intel via the pinned fork (nightly `drtrace-macos`
  lane), so the macOS-Intel backend column is emulator + Mach + DR — but **Apple
  Silicon remains emulator-only** ([include/asmtest_mach.h](../../../include/asmtest_mach.h)
  is x86-64 Darwin only), and the platform matrix below says so honestly.
- **Hardware-backed GUI CI now exists to ride — asymmetrically.** `hw.yml`
  (05:00 UTC nightly + owner dispatch, `HW_RUNNER_*` variable gates + owner-only
  actor guard) has a **standing** bare-metal Intel PT lane in REQUIRE mode (the
  ephemeral-runner loop deployed 2026-07-23, `HW_RUNNER_INTEL_PT` left at `1`)
  and a **live-validated but one-shot** AMD Zen LBR+IBS lane (`HW_RUNNER_AMD_ZEN`
  sits at `0` between operator sessions; a standing Zen runner is a recorded,
  deferred choice). `.asmtrace` record-mode smokes over live hardware capture
  ride the PT lane unattended; the Zen leg runs when the operator powers it.
- **The in-app assembly box is now safe to build.** `asmtest_assemble` fails
  loudly when Keystone silently drops a statement (commit `ba5bd46`) — removing
  the trap where the GUI would render a trace of code the user never wrote.

## Personas × product goals

| Persona | Mode | Day-one journey |
|---|---|---|
| **Learner / student** | Author | Open app → Learn door → corpus-seeded golden walkthroughs (no deps, no root, any OS) → write a routine in the assembly box → trace + coverage + operand timeline. Later: ABI x-ray, register scrubber. |
| **Instructor** | Author + artifacts | The [classroom](../../guides/classroom.md) rubric flow with visual failure explanations; `demo-fail`/`demo-robust` as guided tours; recordings as handouts (a graded run is a shareable file). |
| **SIMD / crypto / kernel asm author** | Author | Bytes or `.s` in → full-state capture views (incl. wide vectors; FP-env when Wave 1 lands) → slice explorer on a wrong lane → diff two recordings across a change. |
| **Perf engineer** | Observer + recordings | Attach → hot-function front door (IBS entry-edge on AMD; sw-clock residency elsewhere, labeled as weaker evidence) → hot-edge graph, heat canvas → record → diff before/after. |
| **Reverse engineer / security researcher** | Observer | Attach (incl. symbol-less `0xADDR:LEN` regions, PLT, JIT) → syscall stream with redacted-by-default payloads → watchpoint timeline → taint seed→sink view (DR tier, Linux x86-64) → dataflow slices via PT replay on PT hosts. |
| **CI operator** | Recordings only | Capture `.asmtrace` in docker lanes / CI (incl. `hw.yml` hardware lanes), render on the workstation; backend-completeness view over the committed `benchmarks/boxes/` features data. |
| **Compiler / libc / runtime author** | Author + Observer | Conformance-corpus goldens as reference behavior; managed-runtime views (srcmap IL/bci attribution, stitched per-tid PT timelines) for JIT work. |
| **Windows developer** | Author + recordings | The Win64 native tier ([src/ss_win64.c](../../../src/ss_win64.c) VEH stepper, `asmtest_abi_win64.json`) produces recordings; the viewer builds and renders on Windows. **Live attach on Windows is out of scope for v1** — the engines are ptrace/Mach. |

### Platform honesty matrix (rendered in-app, never glossed)

| Platform | Author mode | Observer mode (live attach) |
|---|---|---|
| Linux x86-64 | full (emulator, valtrace, DR, hwtrace incl. PT/LBR/IBS where hardware allows) | full view family; PT-replay dataflow on PT hosts |
| Linux AArch64 | emulator (x86-64 + arm64 guests), SVE capture; **valtrace/slice for x86-64 guest code only — no arm64-code value producer exists** (every L0 producer is Linux-x86-64-only) | ptrace view family **minus live dataflow** ([src/dataflow_ptrace.c](../../../src/dataflow_ptrace.c) is compile-gated to Linux x86-64 + Capstone; asmspy renders `ASMSPY_DATAFLOW_UNAVAIL` as a clean skip); **watchpoints / hw-breakpoint entry probes host-gated** (render `asmspy_hwdebug_reason()`) |
| macOS Intel | emulator, Mach stepper, DR (pinned fork) | none (no ptrace attach engine) — capture via SSH/docker, render locally |
| macOS Apple Silicon | **emulator only** (isolated guest, not the real CPU — say so) | none |
| Windows | viewer + Win64-tier recordings | none (v1) |

The matrix is not static text: it renders live from
[include/asmtest_trace_auto.h](../../../include/asmtest_trace_auto.h)'s resolver,
`asmtest_hwtrace_status` (failing stage, EPERM-vs-EUNAVAIL, `perf_event_paranoid`,
human reason), `asmtest_ibs_available`/`_skip_reason`, and the features sweep
([tools/asmfeatures.c](../../../tools/asmfeatures.c)) — **the GUI never re-probes
what the library already answers.**

## Design

### D1. The `.asmtrace` contract (the keystone, now expansion-ready by construction)

NDJSON event log + JSON metadata header, exactly as the ideation doc argued —
amended with the three expansion-driven properties:

- **Versioned envelope.** `{"asmtrace": 1, ...}` header; readers reject newer
  majors, ignore unknown event kinds and unknown fields (forward-compatible by
  rule, so a Wave-1-era recording opens in a v1 viewer minus the new panels).
  The envelope reserves an optional **compressed/framed container** (zstd-framed
  NDJSON with a chunk index, still inspectable via a decompress tool) — PT-scale
  producers (whole-window captures, stitched per-tid slices) will not fit
  uncompressed line-delimited text at interactive open latency, and Tracy's
  compact format is the precedent; plain NDJSON stays valid for small
  recordings and hand-authoring.
- **Event-kind registry with reserved kinds.** v1 ships: trace (insns/blocks,
  with an explicit **basis tag** — region-relative vs absolute, because both
  coexist in `asmtest_trace_t` today and attribution at the wrong basis is a
  verified failure mode), coverage, syscall (payload separated from formatted
  line — the natural redaction gate), call-tree event (structured
  `asmspy_tree_call_t`, no line re-parsing), graph snapshot, topology snapshot,
  IBS/sw-clock survey (statistical, never merged into exact kinds), watch hit,
  dataflow step + defuse edge, register state, test/bench/features results,
  **annotation/walkthrough** (step-anchored lesson text + ordered stops — the
  Learn door's curriculum artifact, so a walkthrough is itself a recording).
  Reserved now, produced later: **mem-address event** (Wave 1 CT invariance),
  **FP-environment state** (Wave 1 top pick), **state-diff record** (Wave 2
  equivalence oracle / snapshots), **slice-blame attachment** on failure events
  (Wave 2), **stitched per-tid slice set** (landed producer — must be in v1
  after all), **mutation/fuzz stats**, **taint hit** (`at_taint_hit_t` shape),
  **take/edit provenance** (which recording an experimental fork derives from
  and what single fact was edited — reserved for [the Loom](#flagship-interaction-concept-the-loom)'s
  fork mechanic, distinct from the Wave 2 state-diff kind, which stays with the
  equivalence oracle).
- **State descriptors, not hard-coded registers.** Register-state events
  reference a descriptor (name/width/class per register, extending the
  `asmtest_abi.json` layouts-as-data idiom) so MXCSR/FPCR, `vec512`, SVE-VL
  arrive as new descriptor rows, not schema surgery. Precondition: add
  `vec512_t` to the manifest.
- **Provenance is mandatory on every stream**: producer + backend, exact vs
  statistical, truncation flags, drop/loss accounting (`lost`, `THROTTLED`,
  ring-drop counts, IBS EUNAVAIL-on-OOM), window parameters, skip reasons,
  trust-ladder tier where applicable. The honesty culture is enforced by the
  format, not by renderer discipline.

Everything else is a reader or writer of this: record modes are NDJSON
serializer sinks beside the existing ncurses/JSON sinks in
[cli/asmspy.c](../../../cli/asmspy.c); `--serve` streams the same events; the
Wave 4 "self-contained HTML trace/coverage visualizer" becomes **the web
renderer of `.asmtrace`** (one schema, two renderers — not a second rendering
stack); SARIF/speedscope/Perfetto are exporters beside it.

### D2. Capability discovery

One code path answers "what can this host do and why not": the trace-auto
resolver cascade + status APIs above, plus the typed engine skip codes
(`ASMSPY_REGION_NEVER_RAN`, `_SAMPLE_UNAVAIL`, `_DATAFLOW_UNAVAIL`,
`_WATCH_UNAVAIL`, `_ETRACEE_I386`). Two UI laws follow:

- A greyed-out backend always shows its machine reason verbatim (the
  self-skip-shows-why culture).
- The **native→virtual fidelity line is a first-class UI concept**: the library
  already refuses to silently downgrade from real-CPU backends to the emulator
  (`ASMTEST_TRACE_NATIVE_ONLY` gate; resolve returns EUNAVAIL rather than
  degrade). The GUI mirrors this — dropping to the emulator is an explicit,
  labeled user choice, never a silent fallback.

### D3. Diff as a first-class primitive

The viewer's document model is "a set of recordings", and every view accepts one
or two. v1 diffs what exists today: coverage union/delta, hot-edge delta,
per-offset heat delta, cycles trend (extending the golden + `perf-history.jsonl`
culture). The same alignment engine picks up state-diff records (equivalence
oracle, snapshots) and mem-address invariance verdicts when their producers land.

### D4. The navigation spine

Every artifact deep-links into the inspection views: failure → recording →
step → slice cone (when the blame producer lands); uncovered block → trace
canvas position; watch hit → offending store, disassembled; survey edge →
frozen graph; topology node → per-process views. The spine is the low-barrier
mechanism: beginners never face a blank multi-panel IDE — they arrive at a
view *from a question*.

### D5. Stack and process model

- **Viewer:** C/C++ + Dear ImGui, Tracy-style. `#include`s the pure view-model
  headers under their existing tests. Two caveats found in review:
  [cli/asmspy_graphsort.h](../../../cli/asmspy_graphsort.h) `#include`s
  `asmspy.h` and uses file-scope qsort latches (single-threaded by design) — the
  GUI serializes sorts through one thread or lifts the comparator context before
  multi-panel use; and [cli/asmspy_autoregion.h](../../../cli/asmspy_autoregion.h)
  uses `asmspy_sample_edge_t` in its signatures without including `asmspy.h` —
  an undeclared include-after ordering dependency the viewer TU must honor (or
  the edge struct gets lifted into a standalone header).
  For **replay**, the viewer computes backward/forward slice closures directly
  from recorded defuse-edge events — no `libasmtest_dataflow` link needed to
  *render* a recording on any OS; the analysis lib is required only for fresh
  local capture.
- **Capture host:** the asmspy engines compiled in directly (there is no library
  target for [cli/asmspy_engine.c](../../../cli/asmspy_engine.c) — confirmed
  still true), driven on **one dedicated tracer thread per session** exactly as
  the TUI's `run_live_view` does (engine start-to-finish on the attaching
  thread; wake blocked `waitpid` via `pthread_kill(SIGALRM)`); the two-phase
  detach and own-`int3` delivery guarantees come along untouched.
- **Author-mode engine host:** the C library direct (emulator, valtrace,
  hwtrace, codeimage), using the opaque-handle accessor families that already
  exist for exactly this (no struct mirroring; `asmtest_abi.json` where
  mirroring is faster). Known FFI trap to respect: the 12-byte hwtrace scope
  handle spans two SysV argument registers.
- **JIT/live disassembly panes are versioned by construction:** bytes come from
  [include/asmtest_codeimage.h](../../../include/asmtest_codeimage.h)
  (`bytes_at(addr, when)` — the time-aware code-image recorder) and the
  versioned render APIs, never from a bare `process_vm_readv` snapshot, so a
  patched/moved/freed JIT method renders the bytes that were live when the
  trace ran. The soft-dirty/`PAGEMAP_SCAN` (Linux ≥ 6.7) gate self-skips
  visibly like everything else.

### D6. Concurrency budget (corrected)

Per target pid: **one ptrace consumer** (any of syscalls / stream / graph /
tree / trace / dataflow / **watch** / **topology**) **+** IBS sampling **+**
sw-clock survey **+** foreign-pid PT capture (PT hosts). The topology engine
SEIZEs the whole descendant tree, so it conflicts with a ptrace view on *any*
process in that tree, not just the root pid — the budget is per-tree while a
topology view runs. Live multi-panel ptrace views are
time-sliced with explicit "paused — another live view holds the tracer" states;
replay tiles everything. Per-view flag matrix enforced in the UI: `--tid` exists
for trace/dataflow/stream/tree/graph (not the syscall engine — its signature has
no tid; engine work if wanted), `--follow` for log/stream/tree/graph, tid XOR
follow, auto XOR tid.

### D7. Licensing (a shipping constraint, stated up front)

The Author-mode engines are **GPL-2.0-only** (bundled Unicorn and Keystone —
[licenses/README.md](../../../licenses/README.md)); a GUI binary linking them is
GPL-2.0 as a whole. Decision: **accept GPL-2.0-only for the full app**, and keep
the **render-only viewer buildable without the engines** (it reads `.asmtrace`,
links only permissive code) — preserving a permissive redistribution path for
the viewer and a trivial contributor on-ramp. Pin/SDE/libdft64 remain test-lane
only and are **never** bundled into any GUI artifact.

## View catalog (ideation verdicts carried, with corrections)

| View | Verdict | Change vs ideation doc |
|---|---|---|
| Def-use slice explorer | **Build — crown jewel** | Now **two producers**: emulator L0 (cross-platform, deterministic) + PT replay ([src/dataflow_pt.c](../../../src/dataflow_pt.c), Observer, zero-perturbation, PT hosts). Producer-agnostic view. |
| Operand-value timeline | Build now | unchanged ([cli/asmspy_dataview.h](../../../cli/asmspy_dataview.h) computes annotations) |
| Trace canvas (heat + blocks + coverage) | Build (Author); Observer = "invocation #N" snapshots | JIT targets render via codeimage `bytes_at(addr, when)` |
| Hot-edge graph (IBS) | Build | unchanged; never a flame graph (edges, not stacks) |
| Call graph / tree | Build as sorted list + drill-in | tree **filter panel** (depth/focus/module) is a day-one GUI-exceeds-TUI win — engine accepts filters, TUI passes NULL |
| Watchpoint timeline | Build — promoted | **per-host arming probe**; renders `asmspy_hwdebug_reason()` where arming is refused (arm64 ENOSPC class) |
| Syscall stream | Build | redact-by-default needs formatter work: the engine hands the payload as a *separate* sink argument, but the formatted line still embeds buffer contents / paths / sockaddrs — the `.asmtrace` syscall serializer needs a payload-free line variant (work item below); no tid pinning without engine work |
| Process topology | Build | unchanged |
| Backend completeness | Build | richer: features sweep + committed `benchmarks/boxes/` records + trace-auto resolver; CoreSight renders as skip_reason (the one dark column) |
| Test results | **Upgraded: navigation spine** | failures are deep links into recordings from day one; blame cones when Wave 2 lands |
| Managed stitched timeline | **New** | per-tid PT slice sets from the compose/stitch producer; trust-ladder tier in chrome |
| Taint seed→sink | **New (Observer, DR tier, Linux x86-64)** | `at_taint_hit_t` {off, ea, seed_off, tag, kind, depth} for the RE/security persona; oracle-validated tier |
| Register time-travel scrubber | Build **after funding the producer** | unchanged (no per-step register ring exists — verified) |
| ABI x-ray (teaching) | Ideate — high leverage | unchanged; corpus-seeded, serves [classroom](../../guides/classroom.md) |
| **The Loom** (flagship composition) | Build — day-one rung in Phase 2 | composes slice explorer + operand timeline + diff into one interaction surface — [own section below](#flagship-interaction-concept-the-loom) |

## Flagship interaction concept: the Loom

*Provenance: six independently-ideated concepts (spatial/cartographic,
temporal/cinematic, causal/provenance, tangible/pedagogic, comparative/
multiverse, systems/hardware lenses), each adversarially grounded against the
code, then synthesized — the same verify-then-keep method as the ideation docs.
The causal/provenance core scored highest on grounded feasibility; the
surviving elements of the other five are grafted in below; the killed elements
are recorded at the end so they are not re-raised.*

**Pitch.** A recorded run rendered as a **spacetime fabric**: horizontal =
trace step, vertical = **location lanes** (a register deck, then memory lanes
allocated on first touch and coalesced into stack/buffer bands). Every value is
a **worldline** — a thread occupying (lane, [t\_write, t\_next\_write))
intervals built from the valtrace's operand records (`at_val_rec_t` read/write
records with resolved effective addresses and values), with def-use edges drawn
as diagonal hops between lanes and ALU ops as knot vertices carrying mnemonic
glyphs. Zoomed out, bands bundle into density ribbons; zoomed in, per-byte rows
with value chips (hollow thread where the route is known but the value is not).

**The unique question it answers.** For any value in a run: *where did it live
and for how long, by what route did it travel from input to output, what else —
from every other instrument — touched those same places, and, when you change
exactly one fact, which of its dependents actually cared?* The slice explorer
answers static dependence; the operand timeline annotates steps; the diff
primitive compares two runs — nothing else composes **residency × route ×
place × intervention** into one selectable object.

**Interaction model:**

- **Selection = lineage.** Click any (lane, time) point: the resident value's
  full ancestry + descendant closure (`asmtest_slice_backward`/`_forward`, or
  the same closure computed client-side from recorded defuse edges) lights up
  as one thread-tree; everything else dims to grey fabric. `[`/`]` walk
  generations; a biography panel narrates birth, every hop (disassembled, with
  srcmap labels where available), escapes, producer tier, truncation state. A
  **zeroization audit** is a selection mode: forward closure ∩ last-writer
  residency at playhead T lights every lane still holding descendants of a
  selected birth — honestly bounded to the traced window.
- **Lane inspector (place-indexed cross-feed annex).** Clicking a lane header
  joins everything every other instrument recorded about that address, from
  shipped address-keyed structs: trace offsets, operand effective addresses,
  `asmspy_watch_hit_t` (tid + value + direction), `at_taint_hit_t`
  (seed/tag/kind/depth), `asmtest_srcreg_resolve` rows, IBS from/to edges —
  each entry under a provenance chip. **Statistical evidence appears only in
  the annex**, only as corroboration or "unconfirmed", never woven as threads
  and never rendered as contradiction (timebase-free histograms cannot prove
  non-execution).
- **Forks (day one: entry-arg and code-patch only).** A takes gutter
  accumulates experiments as (edited fact → new fabric) nodes. Committing an
  edit re-runs from entry — patched code via `asmtest_assemble` (loud-drop-safe),
  arguments via entry seeding, state reset via `emu_snapshot`/`emu_restore` —
  weaving a second fabric beside the first from the same deterministic
  producer. The two fabrics align over the shared prefix (step-index alignment,
  trivially sound for same-entry runs); the first differing trace offset is
  flagged **patient zero**; a faulting take ends in a fault card
  (`emu_result_t` kind + faulting EA); post-divergence tails render dashed
  "unaligned — never drawn as agreement".
- **Dead-dependency dimming.** Inside the aligned prefix, steps in the forward
  cone of the edited fact whose values are byte-identical in both takes dim
  (dependence without consequence); steps whose values changed render hot — the
  interventional answer drawn on the observational fabric. This is what
  separates "the slice says it depends" from "it actually mattered".
- **Honesty chrome.** Torn edge on truncation; fade-out (never thread-death)
  for values alive at trace end; a "born of untraced state" glyph; producer-tier
  outline colors; the isolated-guest badge on emulator fabrics; a standing rule
  that statistical producers can never appear as fabric.

**Day-one rung (existing feeds only — Phase 2, Author mode, x86-64 guest).**
Fabric + lineage + biography from the emulator L0 valtrace producer; lane
inspector joining companion recordings of the same routine; entry-arg/code-patch
forks with prefix dead-dependency dimming (all on today's public API); seeded by
conformance-corpus golden looms behind the Learn door — no root, no attach,
deterministic. Fresh capture is Linux-only until the darwin dataflow item;
recordings render anywhere.

**Growth rungs (each tied to a named item):** Phase 1 state descriptors →
descriptor-driven register deck (ABI-class ordering, vector/FP lanes); Phase 4
per-step register ring → O(1) scrub + complete now-column, and the ABI x-ray
plays a call as a thread journey across the deck; **Reweave** (mid-run
counterfactuals) → a new engine work item (edit-at-step-K + deterministic
re-run inside the dataflow producer — `emu_snapshot` cannot reach the dataflow
guest — plus the per-step alignment engine it funds), with edits that leave the
modeled world ending the take at a loud cut; Wave 1 mem[] stream + `ct_eq` →
the secret-class multiverse (N fabrics overlaid, CT violation at its first
route split); Wave 1 FP-env → rounding/sticky-flag badges on knots; Phase 3 →
Observer looms via PT replay (zero perturbation, purity-gated) or the
single-ptrace dataflow engine, with the D6 budget rendered as a one-jack
patch-bay; stitched per-tid slice sets → per-tid lane decks showing
control-flow occupancy (never cross-thread value hops — valtrace has no tid
dimension); Wave 2 blame → failures deep-link into the fabric with the bad
value's thread pre-selected (the D4 spine ends here, not at a table).

**Honest limits (what the Loom must never pretend):** exact-only fabric;
lifetimes are trace-relative ("still holding the key" = "not overwritten within
the traced window"); value fabrics are x86-64-guest-only until a per-guest
valtrace producer exists; no syscall-born threads until a structured buffer-EA
syscall kind exists; no cross-thread value hops, ever; forks never touch a live
process (replay-only, emulator-only, an explicit labeled crossing of the
native→virtual line — never evidence about silicon timing); day-one alignment
is shared-prefix only; provenance starts at instrumentation (pre-existing state
has no ancestry); scale is routine/invocation, not whole-process hours.

**Killed in grounding (recorded so they are not re-raised):** a versioned
address-space atlas as the core (no producer exposes the map layers it needs);
a live-Observer-first "observatory" (inverts the phasing; no producer-side
timestamps in any feed); N-ISA lockstep braids with value anchors (valtrace is
x86-64-guest-only; N-way semantic alignment is unfunded); mid-run state editing
as day-one work (no resume-from-state API exists); syscall-buffer thread births
(the sink is a formatted line, capped, with no buffer EA).

## UX: low barrier to entry, learning, professional depth

**First run — three doors, no blank IDE:**

1. **Learn** — corpus-seeded golden recordings open instantly (no deps, no root,
   no attach, any OS). The curriculum ladder the docs already define:
   quickstart square → `demo-fail`/`demo-robust` (contained crash theater) →
   the RPN VM → `ct_eq` constant-time story. (Prerequisite: `ct_eq` is
   docs-illustrative only today — **ship it as a real example** before building
   its walkthrough.) The ABI x-ray joins the ladder in Phase 4.
2. **Author** — an assembly box (`asmtest_assemble`, loud-drop-safe) or raw
   bytes → run on the emulator → trace canvas, operand timeline, slice explorer.
   Faults are data, never crashes; that is the pedagogy.
3. **Inspect** — the attach picker with the `--auto` front door, **branching by
   substrate**: AMD IBS entry-edge ranking (arrival evidence) vs the portable
   sw-clock residency sampler elsewhere, with the weaker-evidence label surfaced
   ("residency is not entry evidence") and the 3-candidate NEVER_RAN walk. The
   picker diagnoses *why* a process is unattachable (Yama `ptrace_scope`,
   CAP_SYS_PTRACE, seccomp'd perf, i386 refusal) — the permission wall is the
   real first-trace barrier, so it gets a first-class explanation surface, not
   an error toast.

**Honesty culture as UX law** (inherited, enforced by schema + chrome):
truncation is loud; statistical ≠ exact, always visually distinct with
provenance in the chrome; self-skip shows why; crawl warnings on single-step
views, live-JIT targets steered to IBS; sensitive captures redacted by default
(paths and sockaddrs included); clean detach guaranteed (never bypass the
two-phase machinery); every bounded ring reports its drops; **no silent
native→virtual downgrade**.

**Professional workflows:** keyboard-first navigation; recordings as the unit of
collaboration (capture in CI/container/SSH, render anywhere, diff two);
exporters (DOT and lcov exist; speedscope + Perfetto in Phase 1; SARIF later
alongside the Wave 4 items); everything the GUI shows is recordable and
therefore scriptable — GUI and headless never drift because they consume the
same events.

## DX: how the GUI itself is built and tested

- View-model semantics stay under the existing unit tests
  ([cli/test_view.c](../../../cli/test_view.c), `test_graphsort.c`,
  `test_treefilter.c`); GUI-specific view-models follow the same pure-header +
  test pattern.
- **Golden-recording tests:** a committed corpus of `.asmtrace` files (generated
  from the conformance corpus, deterministic) drives renderer tests —
  schema-stability is CI-enforced the way `asmtest_abi.json` layouts are. The
  corpus deliberately includes **dishonesty fixtures** — recordings carrying
  truncated/dropped/redacted data — and the renderer tests assert the banner,
  the provenance chrome, and the redaction defaults, so the honesty-culture UX
  laws are tested behavior, not aspiration.
- Capture smokes ride existing lanes: docker lanes for record-mode round-trips;
  `hw.yml` nightly for hardware-backed capture (PT REQUIRE-mode, AMD Zen), per
  the established gate pattern.
- Dependencies follow [CLAUDE.md](../../../CLAUDE.md): everything installable
  goes in a `Dockerfile.*` + `docker-*` target (an ImGui build lane), pinned;
  only hardware and credentials self-skip.
- The render-only viewer builds with no engine deps (fast contributor loop);
  the full app adds the engines.
- **Distribution honesty:** v1 ships as build-from-source + the docker lane,
  consistent with the repo's current posture (nothing is on any registry yet);
  signed/notarized per-OS installers are deferred until the bindings' registry
  go-live proves the credential path. Per-door artifact requirements: **Learn**
  works in either binary (recordings only), **Author** needs the full GPL app,
  **Inspect** needs the full app on Linux.

## Phasing

1. **Phase 1 — the contract.** *(Status 2026-07-24: the recording half is
   **done** — [01-asmtrace-format.md](../gui/01-asmtrace-format.md) landed the
   draft schema, the writer TU, `--record` on every headless mode, `--json` for
   `--log`/`--stream`, the `vec512_t` row, and the committed golden corpus with
   its dishonesty fixtures. What remains of this phase is the reader/exporter
   half: [02-exporters-and-readers.md](../gui/02-exporters-and-readers.md)
   T1–T4.)*
   `.asmtrace` v1 schema doc + NDJSON serializer
   sinks + record modes in [cli/asmspy.c](../../../cli/asmspy.c); JSON modes for
   `--log`/`--stream`; state descriptors (+ `vec512_t` manifest addition);
   speedscope + Perfetto exporters; readers for features/bench artifacts.
   Golden-recording corpus committed. *Acceptance:* every headless mode can
   record; recordings round-trip; exporters open in their tools; schema doc
   published as a **draft** with reserved kinds and the forward-compat rules
   binding. (The v1 **freeze** is deliberately not here: it is a named
   checkpoint at the end of Phase 3, after the golden corpus, the Phase 2
   viewer, and one `hw.yml` hardware round-trip have all consumed the format —
   the drift-riders section above is the proof that freezing before consumers
   exist invites a v2.)
2. **Phase 2 — the viewer (Author mode).** ImGui shell; replay of any
   recording; def-use slice explorer (emulator producer) + operand timeline +
   trace canvas + diff (coverage/heat/hot-edge); **the Loom's day-one rung**
   (fabric + selection-lineage + entry-arg/code-patch forks with prefix
   dead-dependency dimming); the Learn and Author doors; test-failure deep
   links. Work item: darwin build of `libasmtest_dataflow`
   (the analysis lib is Linux-only today — its platform map has no darwin
   name), required for fresh macOS Author-mode capture (replay-slicing works
   from recorded defuse edges everywhere, per D5). *Acceptance:* a student on
   any OS opens a golden walkthrough and slices a wrong value with no root or
   deps; two recordings diff; `make demo-fail` produces a recording and
   clicking its failure in the GUI opens that recording at the failing step.
3. **Phase 3 — live (Observer mode).** `--serve` control loop (select pid +
   mode + params, pause, stop; sorting/filtering client-side); the Inspect door
   with both `--auto` samplers; syscall stream, watchpoint timeline (arming
   probe + measured-reason rendering), region view as invocation snapshots,
   topology, hot edges; tree filter panel; time-sliced ptrace modes with
   IBS/sw-clock/PT-capture alongside per the corrected budget; PT-replay slice
   view on PT hosts; codeimage-versioned JIT disassembly. *Acceptance:* attach →
   hot function → trace → record → detach leaves the target untouched; a JIT
   method mid-retier renders its historical bytes; `hw.yml` smoke records PT
   live; syscall payloads render **redacted by default** with explicit reveal;
   a whole-window PT recording (millions of events) opens interactively via the
   chunk-indexed container; **the `.asmtrace` v1 freeze checkpoint passes
   here**, all consumers having exercised the draft.
4. **Phase 4 — teaching producers.** Per-step emulator register-capture ring
   (`UC_HOOK_CODE`, opt-in) → the real scrubber and the Loom's complete
   now-column; ABI x-ray on top (SysV vs Win64 contrast, eightbyte
   classification, corpus-seeded — played as a thread journey across the Loom's
   register deck); blame-the-instruction integration when the Wave 2 producer
   lands (failures pre-select the bad value's thread in the fabric); the
   **Reweave** engine item below if demand holds. *Acceptance:* scrub
   any corpus routine step-by-step with the full register file; the classroom
   walkthrough animates a call.
- **Standing — expansion intake.** Each ranked wave item slots into a reserved
  or existing event kind + a view increment per the table below; where a later
  item needs a new kind, it lands as an additive registry row under the
  ignore-unknown-kinds rule — never a v2 envelope.

### Engine work items (small, ordered, updated from the ideation doc)

| Item | Where | Size |
|---|---|---|
| ~~`.asmtrace` NDJSON serializers per sink + record mode~~ **DONE** (01-T3/T5) | [cli/asmtrace_ndjson.c](../../../cli/asmtrace_ndjson.c) + [cli/asmspy.c](../../../cli/asmspy.c) | small |
| ~~JSON mode for `--log`/`--stream`~~ **DONE** (01-T4) | [cli/asmspy.c](../../../cli/asmspy.c) | small |
| `--serve` control loop over stdout/unix socket | [cli/asmspy.c](../../../cli/asmspy.c) | small |
| speedscope / Perfetto exporters | new tool or `cli/` | small |
| ~~`vec512_t` manifest row (descriptor precondition)~~ **DONE** (01-T2) | [scripts/gen-manifest.c](../../../scripts/gen-manifest.c) | small |
| Ship `ct_eq` as a real example | `examples/` | small |
| Darwin build of `libasmtest_dataflow` | `mk/dataflow.mk` + binding platform maps | small–medium |
| Per-step register-capture ring (opt-in) | [src/emu.c](../../../src/emu.c) (`UC_HOOK_CODE`) | small–medium |
| graphsort comparator-context lift (or serialized sorts) | [cli/asmspy_graphsort.h](../../../cli/asmspy_graphsort.h) | small |
| ~~Payload-free `format_syscall` line variant (the redaction gate)~~ **DONE** (01-T4; the fd-backing path had to be redacted too) | [cli/asmspy_engine.c](../../../cli/asmspy_engine.c) | small |
| Runner record mode: per-test `.asmtrace` + failure events carrying recording path + step id | [src/asmtest.c](../../../src/asmtest.c) (riding the emulator/valtrace tiers the tests drive) | medium |
| Walkthrough content for the Learn ladder (authoring, not code) | golden-recording corpus | small–medium |
| *(opportunistic)* arm64-guest emulator L0 value producer | `src/dataflow_emu.c` guest seam | medium, demand-gated |
| *(Phase 4+, demand-gated)* Reweave: edit-at-step-K + deterministic re-run + per-step alignment | `src/dataflow_emu.c` | medium–large |
| *(optional)* `only_tid` for the syscalls engine | [cli/asmspy_engine.c](../../../cli/asmspy_engine.c) | small |

### Expansion intake (what the GUI does when each wave item lands)

| Expansion item (wave) | GUI socket already reserved | View increment |
|---|---|---|
| FP-env / MXCSR-FPCR assertions (W1, top pick) | FP-env state kind + descriptor row | FP-environment panel (rounding mode, sticky flags, FTZ/DAZ) in state views; FP-env divergence class in diff |
| Secret-dependent-trace invariance + `mem[]` stream (W1) | mem-address event kind | CT invariance verdict view: two secret-class runs, first-divergence highlighted on the canvas |
| Static ISA-baseline gate (W1) | badge on trace/disasm events | per-instruction ISA-class badges; baseline-violation highlighting |
| Stack-depth bound (W1) | scalar result on run events | stack-depth gauge on the canvas |
| Vector-width verification (W1) | register-class data already in dataflow events | executed-width badge (scalar-fallback detector) |
| Insn/byte budget + reduce-and-capture (W1) | run-stats fields | budget bar; "open reduced repro" deep link |
| Full-state equivalence oracle (W2) | state-diff record kind | side-by-side A/B state diff view (the diff primitive's native producer) |
| Golden-state snapshots (W2) | state-diff record kind | snapshot drift view; update-snapshot action |
| Backward-slice blame (W2) | slice-blame attachment on failure events | failure → backward cone, one click (the spine completes) |
| Metamorphic / KAT / mutation-adequacy / latency-axis (W3) | test + stats kinds | verdict chips; mutation killed/survived dashboard (accessors exist) |
| SARIF export + HTML visualizer (W4) | `.asmtrace` is the shared schema | HTML visualizer = web renderer of `.asmtrace`; SARIF as exporter |
| Object-slicer front door R7 (W3) | new source descriptor on recordings | "open object file" path (honest v1: self-contained PIC leaf routines only) |
| Misaligned-access detector (W3) | mem-address event kind | alignment-fault chips on the trace canvas |
| Secret-residue / zeroization audit (W3) | dataflow + mem-address kinds | residue lanes at end-of-trace (bounded to the traced window) |
| Uninit-read, reshaped R6 (W3) | mem-address + watch-hit kinds | never-written-byte-read markers (memory-only, honestly labeled) |
| llvm-mca report backend, reshaped R2 (W3) | run-stats fields (report, never a gate) | port-pressure annex panel beside the canvas |
| Triton prove-or-counterexample, reshaped R5 (W4) | test-result kind + counterexample attachment | verdict chip; counterexample opens as a recording |

## Out of scope

- **Windows live attach** (no engine; Win64 tier feeds recordings instead).
- **Arbitrary on-disk binary import** until R7 lands (Observer attach + Author
  bytes/asm are the v1 inspection doors; the GUI must not fake a loader).
- **Web frontend as a live driver** — unchanged stance: if reach demands it,
  a web renderer reads `.asmtrace`/speedscope files; the C-direct backend is
  not reimplemented.
- **IBS flame graphs** (edges, not stacks), **live force-directed layouts**
  (frozen snapshots only), **custom shells for commodity views** (JUnit
  dashboards, capability tables — existing renderers serve them).
- **Concurrency/litmus visualization** — the engine cannot produce relaxed
  outcomes (R4 kill stands).

## Cross-review record (2026-07-23)

Verified against HEAD `51b8247` by independent re-reads of
[cli/](../../../cli/), [include/](../../../include/), [src/](../../../src/),
[bindings/](../../../bindings/), the guides, and the implementation-doc index.

**Ideation-doc claims that still hold:** batch-JSON/text-stream split
(constraint 1); no per-step register producer (3); IBS edges-not-stacks (4);
asmspy has no PT backend (5 — all recent PT work is library/CI-side); the Rust
crate still lacks a dataflow module (6-corrected); per-invocation region view
(7); no `.asmtrace`/`--serve`/JSON-log-stream anywhere; no Wave 1–4 expansion
item landed; TUI passes NULL filters; engine not a linkable library.

**Corrections this plan makes to its sources:**

1. Constraint 2 as written was wrong: `--watch` is itself a ptrace tracer and
   cannot coexist with another ptrace view; and foreign-pid PT capture (landed
   post-doc) does coexist. See [D6](#d6-concurrency-budget-corrected).
2. The five-shape substrate table is now six shapes (stitched per-tid PT slice
   sets).
3. The slice-explorer producer list gained PT replay (F5).
4. "Python is the only binding that ships the slicer" is true at the published-
   artifact level, but its slicer is **not** manifest-driven (it hand-declares
   `at_val_rec_t` like every other binding); Node/Ruby/Lua/C++ have in-tree
   dataflow modules **excluded from their published artifacts**; Rust/Go/Java/
   Zig/.NET are smoke-driver-only. The C-ABI decision is thereby *stronger*
   than the doc stated.
5. `libasmtest_dataflow` is Linux-only today — the doc's "cross-platform slice
   explorer" needs the darwin build item above for *fresh local capture*; until
   it lands, macOS Author mode captures via a Linux container/SSH, and
   recordings replay-slice anywhere (per D5).
6. The Observer-mode arch claim needs the arm64 watchpoint/hw-breakpoint gate
   rider (ENOSPC arming refusal, measured and named).
7. The expansion doc's `src/drtrace_app.c:492/515` cites drifted to `:559/:575`
   (now behind the platform exec-memory seam); its substance (relocation
   problem) holds.
8. Library-level exact out-of-process steppers exist beyond asmspy
   ([include/asmtest_ptrace.h](../../../include/asmtest_ptrace.h) Linux
   x86-64+AArch64, [include/asmtest_mach.h](../../../include/asmtest_mach.h)
   macOS x86-64, [src/ss_win64.c](../../../src/ss_win64.c) Windows) — the
   recording story covers macOS-Intel and Windows through them, which the
   ideation doc's asmspy-centric Observer framing missed.
9. GPL-2.0-only shipping consequence made explicit ([D7](#d7-licensing-a-shipping-constraint-stated-up-front)).

**Sibling-doc fixes landed with this plan:** the stale "GUI/TUI front-end" line
in [post-v1-expansion-plan.md](post-v1-expansion-plan.md) (a TUI has shipped;
this plan supersedes the line); the tracing-hub row for asmspy
([docs/guides/tracing/index.md](../../guides/tracing/index.md)) which still said
x86-64-only after the AArch64 support landed (corrected to "x86-64 + AArch64
Linux" with the AMD-only hot-edge qualifier, so the row doesn't swing to
overclaim); the same staleness in [asmspy-plan.md](asmspy-plan.md)'s context
line; and the intro of [licenses/README.md](../../../licenses/README.md), whose
"Pin is the one test-lane-only exception" contradicted its own table (SDE, the
Pin 3.20 kit, and libdft64 are equally test/oracle-only, never bundled).
