# Live Observer views — implementation

> **Sources.** Actioned from [desktop-gui-plan.md](../plans/desktop-gui-plan.md):
> Phasing **Phase 3** (view list), the view-catalog rows *Syscall stream*,
> *Watchpoint timeline*, *Process topology*, *Hot-edge graph*, *Call graph /
> tree* (filter panel), the D6 budget, the codeimage bullet in D5, and the
> drift riders (PT-replay producer; arm64 watchpoint gate). Written
> 2026-07-24. If this doc and a source disagree, this doc wins (sources may be
> stale); if the CODE and this doc disagree, re-verify before implementing.
>
> **Implemented 2026-07-24 (T1–T8). Corrections this doc lost to the code, in
> the sibling docs' format — the code won each time and this doc was fixed in
> the same change:**
>
> 1. **`Recording` could not answer "which invocation was this in".** T6's
>    snapshots are delimited by the producer's `[trace…][coverage]` ORDER, and
>    03's loader groups events *by kind*, which throws that order away. Splitting
>    on anything else (say "a new invocation starts at offset 0") silently merges
>    two invocations of a routine whose first block runs twice. `Event` therefore
>    gained a `seq` — its position in the stream — filled identically by the file
>    loader and by the live host, so a replayed file and a live session split the
>    same way.
> 2. **A live session's lifecycle is NOT in its recording.** T1/T2/T5 read the
>    `started` params echo and the skip, and 07-T3 deliberately keeps
>    `session`/`cmd`/`err` *out* of the growing Recording (they are not recording
>    events; the footer does not count them) while the file loader — which cannot
>    tell them apart — keeps them in `by_kind`. Both are right, so the builders
>    take an explicit `ObsLifecycle` source and default to the recording's own.
> 3. **The tracer's own int3 dirties the page.** T7's "emit `codeimage` events on
>    refresh" produced one version per invocation for a region that never
>    changed: the region engine arms and removes an entry breakpoint, and that
>    write sets the soft-dirty bit. A byte-identical snapshot is not a new
>    version — emitting it anyway attributes the capture's own perturbation to
>    the target and buries a real JIT patch in the noise. Suppressed, and
>    asserted in `cli-smoke`.
> 4. **T8's gate was one level too coarse.** "Full build only; self-skips
>    everywhere PT is absent" is right about CAPTURE and wrong about REPLAY:
>    replaying a recorded path needs Unicorn + Capstone and no PT silicon at all,
>    because the path was decoded at capture time (`stitch`) and the bytes
>    recorded with it (`codeimage`). So the desktop test replays a recorded slice
>    on hosts with no Intel PT, and `mk/desktop.mk` gained a second, narrower gate
>    (`DESKTOP_REPLAY_MISSING`) rather than hiding the test behind Keystone too.
> 5. **04's router could not name a process.** T3's drill-in is "a deep link to
>    the per-process view", and `dt_link` had no `pid` and no Observer view names.
>    Both added — `pid` last in the textual form, so every link written before it
>    stays byte-identical.
>
> Read [\_conventions.md](../implementations/_conventions.md) first; shared
> decisions D1–D11 live in this directory's README. Siblings:
> [07-serve-live-host.md](07-serve-live-host.md) (sessions these views
> consume), [04-replay-views.md](04-replay-views.md) (deep-link router,
> canvas), [01-asmtrace-format.md](01-asmtrace-format.md) (event kinds).
> Symbols from siblings are marked **(new — 0N)**; everything else cited
> exists at HEAD `a460d40`.

## Why this work exists

Phase 3's promise is the TUI's view family, exceeded: the same engines, driven
over 07's sessions, rendered with the chrome the honesty culture demands —
redaction by default, measured skip reasons, statistical-vs-exact separation —
plus the two things the TUI never had (the tree filter panel, and
codeimage-versioned JIT disassembly). Every view here is a consumer of
recorded-or-live events; each works identically on a replayed recording, which
is how its tests run without hardware.

## What already exists (verified 2026-07-24)

- **Syscall events carry the payload separately** from a payload-free line
  (01's `syscall` kind; the engine-side split is 01-T4). The engine has **no
  tid parameter** ([cli/asmspy.h:218–219](../../../cli/asmspy.h#L218)).
- **Watch hits** — [`asmspy_watch_hit_t`](../../../cli/asmspy.h#L573)
  (tid, value + `value_ok`/`value_len`, direction); the watch engine is itself
  a ptrace tracer; refused arming is *named*:
  `asmspy_hwdebug_reason()` (declared in [cli/asmspy.h](../../../cli/asmspy.h),
  measured behavior: arm64 hosts advertising 4 slots yet returning ENOSPC —
  commit `9184c14`).
- **Topology** — `asmspy_engine_procs` SEIZEs the whole descendant tree via
  `seize_process_tree` ([cli/asmspy_engine.c](../../../cli/asmspy_engine.c)),
  so it holds the one ptrace jack tree-wide.
- **Hot edges** — IBS surveys are edges, not stacks; `SYMS_SORT_HOT` is
  IBS-only ([cli/asmspy.c:4258](../../../cli/asmspy.c#L4258), gate at
  [:4359](../../../cli/asmspy.c#L4359)); the sw-clock survey is the portable
  fallback with weaker evidence.
- **Tree filters exist engine-side, TUI passes NULL** —
  `asmspy_tree_filter_t` (depth/focus/module,
  [cli/asmspy.h](../../../cli/asmspy.h);
  [cli/asmspy_treefilter.h](../../../cli/asmspy_treefilter.h) is the pure
  semantics header, unit-tested).
- **Code image** — [include/asmtest_codeimage.h](../../../include/asmtest_codeimage.h):
  `asmtest_codeimage_available` ([:72](../../../include/asmtest_codeimage.h#L72)),
  `_skip_reason` ([:76](../../../include/asmtest_codeimage.h#L76)),
  `_new(pid)` ([:81](../../../include/asmtest_codeimage.h#L81)),
  `_track` ([:90](../../../include/asmtest_codeimage.h#L90)),
  `_refresh` ([:97](../../../include/asmtest_codeimage.h#L97)),
  `_now` ([:102](../../../include/asmtest_codeimage.h#L102)),
  `_bytes_at(img, addr, len, when, out)`
  ([:110](../../../include/asmtest_codeimage.h#L110)); soft-dirty /
  `PAGEMAP_SCAN` gate (Linux ≥ 6.7) surfaces through
  available/skip_reason; optional BPF PROT_EXEC-edge watcher
  ([:153–173](../../../include/asmtest_codeimage.h#L153)).
- **PT-replay producer** — `asmtest_dataflow_pt_replay_path`
  ([src/dataflow_pt.c:383](../../../src/dataflow_pt.c#L383)); it ships **no
  public header on purpose** (the dataflow producers' deliberate pattern), so
  a GUI-reachable surface is a scoped decision, not an oversight (T8).
- **01 reserved no `codeimage` kind** — its reserved list is
  `mem, fpenv, statediff, blame, fuzzstats, taint, take`; this doc adds the
  kind (T7, schema append per 01's append-only rule). **Landed**: the kind is
  now *defined* in [asmtrace-schema.md](asmtrace-schema.md) (the section at the
  end of that file), produced by `asmspy --serve` for region sessions, and it is
  the first reserved kind to gain a producer.

## Tasks

### T1 — Syscall stream view  (M, depends on: 07)

**Goal.** `desktop/src/views/syscalls.cpp` (**new**): live/replay list of
`syscall` events; **payload column redacted by default**, per-row explicit
reveal; `--follow` toggle; no tid pinning (engine fact above — the UI never
offers it for this view).

**Steps.** Row model from the `syscall` kind (`line` is payload-free by
schema); reveal swaps in `payload` for that row only; a session-level
"reveal all" needs a second confirmation. Tests: fixture recordings with
payloads; golden-render asserts the default state shows no payload bytes and
the dishonesty fixture's `redacted` provenance renders its chip.

**Done when.** `desktop-test` proves default-redacted rendering; a live run
against `spy_victim` shows rows; reveal is per-row.

### T2 — Watchpoint timeline  (M, depends on: 07)

**Goal.** `desktop/src/views/watch.cpp` (**new**): arm → hits timeline
(tid, direction, value when `value_ok`), consuming the one ptrace jack (07's
budget), with refused arming rendered as the **verbatim**
`asmspy_hwdebug_reason()` string from the session skip event.

**Steps.** Start params (addr/len/rw/n) form; hit rows from `watch` events;
skip lifecycle renders the measured reason (the arm64 ENOSPC class is the
canonical fixture). Tests: fixture with hits; fixture with a skip session.

**Done when.** both fixtures render correctly; on x86-64 Linux a live watch on
`watch_victim` shows a hit row.

### T3 — Topology map  (M, depends on: 07, 04)

**Goal.** `desktop/src/views/topo.cpp` (**new**): fingerprint cards from
`topo` snapshots; drill-in = deep links (04's router) to per-process views;
UI marks the whole tree's jack as held while live (engine fact above).

**Done when.** fixture snapshot renders cards; drill-in navigates; budget UI
shows tree-wide hold.

### T4 — Hot edges + picker ranking  (M, depends on: 07, 04)

**Goal.** `desktop/src/views/hotedges.cpp` (**new**): ranked edge table +
frozen graph snapshot from `survey` events; provenance chrome always visible
(`samples`, `branch_samples`, `lost`, `throttled`, window, sampler); **never a
flame graph**; picker hot-ranking uses IBS where available, else the sw-clock
survey with the weaker-evidence label (07-T5's contract).

**Done when.** ibs-op and sw-clock fixtures render with distinct labels;
chrome fields all visible; no stack-shaped rendering exists.

### T5 — Tree view + filter panel  (M, depends on: 07, 04)

**Goal.** `desktop/src/views/tree.cpp` (**new**): live/replay call tree from
`call` events; a **filter panel** (depth 1–1000, focus, module) passed as
serve start params — the day-one GUI-exceeds-TUI win (engine accepts filters;
the TUI passes NULL). tid XOR follow enforced in the form.

**Done when.** fixture tree renders; filters round-trip through a live session
(smoke via 07's fake-serve extended with filter echo); XOR enforced.

### T6 — Region view: invocation snapshots  (S, depends on: 07, 04)

**Goal.** The live trace view as discrete "invocation #N" snapshots (never a
scrub), with the crawl warning on single-step modes and the live-JIT
steer-to-IBS hint — reusing 04's canvas for each snapshot's rendering.

**Done when.** two-invocation fixture pages between snapshots; warnings render.

### T7 — Codeimage-versioned disassembly pane  (M, depends on: 07; schema append)

**Goal.** JIT-safe bytes for live/recorded disassembly: a `codeimage` event
kind + a pane that renders bytes-as-of-trace-time, never a live re-read.

**Steps.**
1. Append the `codeimage` kind to [asmtrace-schema.md](asmtrace-schema.md)
   (01's file, append-only; this doc owns the kind):
   `{"k":"codeimage","base":u64,"len":u64,"version":u64,"when":u64,"bytes":hex}`
   — a tracked region snapshot at logical time `when`
   (`asmtest_codeimage_now`).
2. Serve-side (cli, full engine host): when a session targets a JIT region,
   arm `asmtest_codeimage_new/track/refresh` and emit `codeimage` events on
   refresh; gate on `asmtest_codeimage_available`, emitting the skip reason as
   a session note otherwise (Linux ≥ 6.7 soft-dirty gate).
3. Pane: `desktop/src/views/disasm.cpp` (**new**) resolves bytes for an
   address at event time from recorded `codeimage` events (client-side
   `bytes_at` equivalent over the version timeline); falls back to the
   recording's `disasm` strings (D10); **never** `read_live` for a replayed
   trace.

**Done when.** a fixture with two versions of one region renders the
historical bytes for a step between refreshes; the gate fixture renders the
skip reason.

### T8 — PT-replay slice view (gated)  (M, depends on: 07, 04; library exposure)

**Goal.** On PT hosts: a zero-perturbation live def-use slice via the F5
PT-replay producer, feeding the same fabric/slice views as replay.

**Steps.**
1. **Library exposure first (scoped decision):** add a declared surface for
   [`asmtest_dataflow_pt_replay_path`](../../../src/dataflow_pt.c#L383) —
   follow the dataflow producers' existing pattern (smoke drivers declare their
   own externs; the GUI's full build may do the same in one TU,
   `desktop/src/live/ptslice.cpp` (**new**), with the layout self-check the
   producers provide (`asmtest_dataflow_pt_info_layout`,
   [src/dataflow_pt.c:92](../../../src/dataflow_pt.c#L92)) asserted at init).
   Record in the doc: promoting a public header is a library decision this doc
   does NOT make.
2. The view itself is 04's slice explorer / 05's fabric over the produced
   valtrace — no new rendering; only the session plumbing + PT-host gating
   (features/status APIs say why not, rendered verbatim).
3. Full build only; self-skips (with reason) everywhere PT is absent.

**Done when.** on the PT box (`hw.yml`'s standing lane host class) a live
slice renders with zero single-steps of the target; everywhere else the gate
renders its reason; `desktop-test` covers the gated path with a fixture.

## Task order & parallelism

T1–T6 are independent once 07-T3 lands (all fixture-testable before any live
run). T7 needs its schema append first; T8 last (library exposure + hardware
gate). Suggested order: T1 → T5 → T2 → T3 → T4 → T6 → T7 → T8.

## Constraints & gates

- **Hardware/host gates** (self-skip with reason, never fake): IBS = AMD Zen;
  watch arming = per-host probe (arm64 ENOSPC class); codeimage = Linux ≥ 6.7
  soft-dirty; PT-replay = PT silicon (the `hw.yml` intel-pt lane is where its
  smoke can run unattended — a follow-up gate, not edited here).
- Statistical never renders as exact — `survey` data stays in its own visual
  family (table/heat), never woven into instruction listings.
- Redaction is renderer-enforced here but wire-honest (provenance `redacted`);
  the reveal action never mutates the recording.
- All views must render identically from a recording — that is how CI tests
  them without hardware.

## Out of scope

- Flame graphs (edges, not stacks — R4 of the view catalog's honesty rules);
  live force-directed layouts (frozen snapshots only); `only_tid` for the
  syscall engine (plan work-item); Windows/macOS live capture; editing
  `hw.yml` (recorded as the follow-up gate).
