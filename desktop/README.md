# `desktop/` — the asmtest desktop GUI

A [Dear ImGui](https://github.com/ocornut/imgui) shell over the `.asmtrace`
document model: a viewer (and, in the full build, an author/inspect host) for the
recordings every headless asmspy mode and the Author-mode recorder produce. This
directory holds the app skeleton — the two binaries, the build integration, the
document model, and the reuse seam onto the asmspy view-model, implemented from
[docs/internal/gui/03-desktop-shell.md](../docs/internal/gui/03-desktop-shell.md)
— plus the **replay views** over a recording
([04-replay-views.md](../docs/internal/gui/04-replay-views.md)) and the
**backend-completeness panel**
([02-exporters-and-readers.md](../docs/internal/gui/02-exporters-and-readers.md)).
The Loom and the live observer surfaces land in the sibling docs 05–09.

## Two binaries, one license split

| Binary | What it is | License |
|---|---|---|
| `build/asmtest-desktop` | the **full app** — links the Author-tier engines (Unicorn/Keystone/Capstone) so it can drive the emulator/assemble/valtrace tiers | **GPL-2.0 as a whole**: it links the bundled GPL-2.0 engines, so the combined work is GPL-2.0 |
| `build/asmtest-viewer` | the **render-only viewer** — opens and replays `.asmtrace` recordings with **zero engine dependencies** (built with `-DASMTEST_DESKTOP_RENDER_ONLY=1`) | **permissive**: it links no engine object or lib (`ldd` shows no libunicorn/keystone/capstone), so it stays distributable without the engine copyleft |

This split (plan §D7 / D4) is enforced by the build, not by convention: the
viewer's link line carries no engine object, and Pin/SDE/libdft64 are never
bundled into any desktop artifact.

## Building

From a bare host, one command installs everything and builds both binaries:

```
make desktop-setup          # host packages -> pinned engine source builds -> build
make desktop-setup-render   # the viewer alone: app backends, no engines
```

It runs [`scripts/install-deps.sh --desktop`](../scripts/install-deps.sh) (GLFW +
GL + unicorn + pkg-config + git/cmake), then the pinned
[Capstone](../scripts/build-capstone.sh) and [Keystone](../scripts/build-keystone.sh)
source builds — neither has a distro package, so the package manager alone would
leave `make desktop` still gated — and finally builds the two binaries through a
**recursive `$(MAKE)`**: the dependency gates are `$(shell)` probes expanded when
make *reads* [`mk/desktop.mk`](../mk/desktop.mk), so only a second make process
sees the deps that were just installed. Every step is idempotent, so re-running
on a set-up host is a plain incremental build.

To build only, in a container (no host deps needed):

```
make docker-desktop     # build both binaries + run the headless tests in a container
```

The container has no display, so that lane never *runs* the GUI — it builds both
binaries and runs the headless tests. Individual targets, on a host that already
has the toolchain:

```
make desktop            # build build/asmtest-desktop (needs GLFW/GL + the engines)
make desktop-render     # build build/asmtest-viewer  (needs GLFW/GL only)
make desktop-test       # headless null-backend tests (only a C++17 compiler)
```

When a dependency is missing, `desktop` / `desktop-render` print the apt line and
the `make docker-desktop` pointer and fail — never a raw compiler error.
`desktop-test` needs no display, no GL and no engines, so it runs anywhere.

## Running

Both binaries take no arguments ([`src/main.cpp`](src/main.cpp)) and open a
window, so they need a display (`DISPLAY` or `WAYLAND_DISPLAY`):

```
./build/asmtest-desktop     # full app
./build/asmtest-viewer      # render-only viewer
```

Recordings are opened from inside: the home screen's **Learn** door opens the
recording dialog, which takes a path to an `.asmtrace` file — a `Workspace::open`
error renders verbatim there, never as a silent no-op. The committed corpus lives
in [`tests/golden-asmtrace/`](../tests/golden-asmtrace/), and every headless
asmspy mode writes one with `--record=<f>`. The key map is the table under
[The replay views](#the-replay-views) below.

Formatting: `make desktop-fmt` reformats `desktop/**` with the repo
[`.clang-format`](../.clang-format); `make desktop-fmt-check` reports drift and is
informational (desktop/ stays out of the CI-gated format set, D8).

## Pinned dependencies

Both are MIT, both are compiled **into** the binaries (bundled), and both are
fetched pinned and digest-verified the way the native engines are — the digests
live in [`scripts/third-party-digests.txt`](../scripts/third-party-digests.txt)
and the license texts under [`licenses/`](../licenses/):

| Dependency | Version | Fetch script | License file |
|---|---|---|---|
| Dear ImGui | 1.91.9 | [`scripts/fetch-imgui.sh`](../scripts/fetch-imgui.sh) | `licenses/DearImGui-LICENSE.txt` |
| nlohmann/json | 3.11.3 | [`scripts/fetch-json.sh`](../scripts/fetch-json.sh) | `licenses/nlohmann-json-LICENSE.MIT` |

The app backends are GLFW + OpenGL3 (`libglfw3-dev` / `libgl1-mesa-dev`, added by
[`Dockerfile.desktop`](../Dockerfile.desktop)); the headless tests use ImGui's
`example_null` pattern and link neither. Nothing under `build/` is committed
(D1) — the fetch scripts repopulate it, and `make docker-desktop` refetches
inside the image.

## Layout

```
desktop/
  src/
    main.cpp            shared entry point for both binaries (compile-time
                        ASMTEST_DESKTOP_RENDER_ONLY guard)
    vm_compat.cpp       compiles the asmspy view-model headers as C++17 so the
                        desktop reuses the TUI's tested inline logic (plan D5)
    doc/
      recording.{h,cpp} .asmtrace NDJSON -> Recording (events by kind + mandatory
                        provenance + the honesty facts a reader must surface)
      workspace.{h,cpp} the SET of open recordings (plan D3)
      streams.{h,cpp}   Recording -> typed view streams (trace / dataflow /
                        survey), decoded once at open
    walkthrough.{h,cpp} the Learn door's player model: a recording's ordered
                        `stop:true` notes -> stops + navigation (pure)
    capview.{h,cpp}     the capability panel's view-model: the library's own
                        status strings, verbatim; probes nothing itself
    author_vm.{h,cpp}   the Author door's model: the assembler diagnostic
                        passthrough, the fault-card mapping, the arch gate
    nav.{h,cpp}         the deep-link router: `asmtrace-link:v=...&rec=...`
                        parse/format + view dispatch (plan D4's spine)
    analysis/
      slice.{h,cpp}     backward/forward def-use closures over RECORDED edges,
                        semantics pinned to src/dataflow.c's slicer
      diff.{h,cpp}      two-recording alignment: coverage / heat / hot-edge
                        deltas and the first divergence (plan D3)
    data/
      features_data.*   asmfeatures / box-record / bench-report readers
      perf_history.*    the append-only perf-history.jsonl reader + box scan
    live/
      session.{h,cpp}   the capture HOST: spawn `asmspy --serve` (locally or
                        over ssh) and turn its stream into growing Recordings.
                        Links no engine — the tracer is the subprocess (D9)
      budget.{h,cpp}    the D6 concurrency budget as a pure decision table:
                        one ptrace jack per target, sample is out of band
      inspect.{h,cpp}   the Inspect door's two decisions: attachability (WHY a
                        target cannot be traced, and what would fix it) and the
                        `--auto` evidence labels (entry vs residency)
      ptslice.{h,cpp}   the PT-replay slice: the gate (capture needs PT silicon,
                        REPLAY does not), the stitch+codeimage input assembly,
                        and the one place the PT producer is re-declared
    loom/
      fabric.{h,cpp}    the spacetime fabric: lanes, worldline spans, hops,
                        knots, reads — engine-free, in BOTH binaries
      feed.{h,cpp}      the replay feeder (a decoded Recording -> the C structs
                        the builder consumes) + the live/fork passthrough
      fabric_plan.*     the pure draw plan: zoom collapse, byte rows, and every
                        honesty prim (torn edge, fade-out, born-untraced, badge)
      lineage.*         selection = lineage: generation walk, biography, and the
                        window-bounded zeroization audit
      annex.*           the lane inspector's cross-feed join — corroboration,
                        never contradiction (a two-enumerator verdict)
      take_view.*       the fork UX: shared-prefix alignment, patient zero, the
                        three-way dim/hot/neutral verdict, dashed tails
      forks.{h,cpp}     one-fact takes via the C library — the ONLY loom TU that
                        links the engines, so full app only (D4)
      fabric_imgui.cpp  the thin ImGui half (paints prims; draws the panel)
    views/
      canvas.*          per-offset heat, block gutter, basis refusal
      timeline.*        per-step values via cli/asmspy_dataview.h's grammar
      slice_view.*      the layered def-use DAG + cones
      diff_view.*       the A/B summary panel, every row a deep link
      completeness*     the tier x backend capability table
      observer.*        what every LIVE view shows first: provenance chrome,
                        the honesty banner, and the session skip (verbatim)
      syscalls.*        the syscall stream — payloads redacted BY DEFAULT
      watch.*           the watchpoint timeline (3-valued direction, a value
                        that was never read back, the refused-arm reason)
      topo.*            processes as cards, `inv` never shown without its unit
      hotedges.*        the statistical edge table — edges, never a flame graph
      tree.*            the call tree + the engine-side filter panel
      region.*          the region trace as discrete invocations (never a scrub)
      disasm.*          bytes-as-of-trace-time, off the codeimage timeline
      *_draw.cpp        the thin ImGui half of each view (draws only)
    ui/
      shell.{h,cpp}     the home screen (three doors), open dialog, and tab strip
                        — backend-free ImGui draws the null backend can drive
      doors.h           the three doors' shared state structs
      learn_door.cpp    the walkthrough card list + player (both binaries)
      author_door.cpp   assemble -> run -> fault card (full app only)
      capability_panel.cpp  what this host can do and why not (probes in the
                        app; shows the recording's provenance in the viewer)
  test/
    test_null_render.cpp   ImGui builds + renders headlessly (example_null)
    test_recording.cpp     the loader's reject rules + honesty accounting (D7)
    test_shell.cpp         shell banner behaviour + a 3-frame null render smoke
    test_golden.cpp        opens every committed golden recording (schema gate)
    test_slice.cpp         the closure rule, engine-free
    test_slice_diff.cpp    the SAME closures vs the real C slicer (links
                           dataflow.o — the one test-half exception to D4)
    test_nav.cpp           link round-trip, rejections, router refusals
    test_diff.cpp          alignment, refusals, and the bounded-verdict rule
    test_canvas.cpp        heat, gutter, basis refusal, truncation banner
    test_timeline.cpp      annotation literals shared with the TUI
    test_slice_view.cpp    cone styles, deterministic lanes, generation walk
    test_diff_view.cpp     panel rows, deep links that parse back
    test_data_readers.cpp  the three envelopes + a torn append-only line
    test_completeness_view.cpp  the cell rule + verbatim skip_reason goldens
    test_walkthrough.cpp   stop ordering, anchors, the beyond-window refusal
    test_capview.cpp       the two UI laws, on synthetic status data
    test_author_vm.cpp     the diagnostic passthrough + fault-card mapping
    gen_walkthroughs.c     the walkthrough generator (C; make asmtrace-walkthroughs)
    test_loom_fabric.cpp   the fabric model (links fabric.o and NOTHING else)
    test_loom_plan.cpp     zoom semantics: ribbon collapse, byte rows, chips
    test_loom_chrome.cpp   every honesty string, asserted VERBATIM (D7)
    test_loom_lineage.cpp  selection, generations, biography, zeroization audit
    test_loom_parity.cpp   the generation walk's closure vs the real C slicer
                           (links dataflow.o — the second D4 test-half exception)
    test_loom_annex.cpp    the place join and the two-verdict law
    test_loom_takeview.cpp the fork verdict truth table, on hand-built pairs
    test_loom_golden.cpp   the four committed golden looms, replayed end to end
    test_loom_draw.cpp     the painter + the panel under the null backend
    test_loom_forks.cpp    the fork ENGINE — full build only (unicorn+keystone),
                           run by `make docker-desktop`
    test_obs_syscalls.cpp  default-redaction, per-row reveal, the two absences
    test_obs_watch.cpp     the three directions + the verbatim refused-arm
    test_obs_topo.cpp      cards, the tree-wide jack, the drill-in link
    test_obs_hotedges.cpp  ranking, the always-on chrome, evidence grades
    test_obs_tree.cpp      the filter refusals + what goes on the wire
    test_obs_region.cpp    the invocation split, incl. one cut off mid-flight
    test_obs_disasm.cpp    bytes at a TIME: the historical version, or unknown
    test_obs_ptslice.cpp   the PT gate + input assembly, with NO producer linked
    test_obs_draw.cpp      the deck under the null backend, refusal paths included
    test_ptslice_run.cpp   the replay itself — needs unicorn+capstone, not PT
    fixtures/              hand-authored .asmtrace + features/perf-history JSON
    expected/              byte-compared view dumps (UPDATE_GOLDEN=1 rewrites)
    golden/                byte-compared completeness renders
```

The `.asmtrace` grammar, kind registry and golden corpus are **not** owned here —
they belong to
[docs/internal/gui/asmtrace-schema.md](../docs/internal/gui/asmtrace-schema.md)
and [`tests/golden-asmtrace/`](../tests/golden-asmtrace/). See the
[GUI implementation docs](../docs/internal/gui/README.md) for the full plan.

## The replay views

Every view is built in two halves. `<view>.cpp` is a **pure builder**: a
function from the decoded streams to a view model, plus a deterministic text
dump. `<view>_draw.cpp` renders that model with ImGui and decides nothing. The
split is enforced by the link line — the view tests link only the pure half, so
a builder that reached for ImGui would fail to build its own test — and the
dumps are byte-compared against `desktop/test/expected/`, which is what makes
the rules below checkable rather than aspirational.

**Trace canvas.** One row per distinct offset. *Heat* is how many times that
offset appears in the ordered instruction stream — exact trace events only,
never a statistical stream folded in. The *gutter* marks **block starts**,
because block-granular is what a `coverage` event measures; marking every
executed instruction "covered" would claim per-instruction coverage the data
does not carry. The disassembly column is the recorded `disasm` string (D10);
absent, the row shows its bare offset and says so, dimmed.

**The basis rule.** `basis` is `"rel"` (offsets from the routine entry) or
`"abs"` (absolute addresses), it is mandatory, and a reader may never default
it. A recording carrying both draws **no rows at all** — just a placard naming
the two bases. The offsets are not comparable, so every row of a merged canvas
would be mis-attributed; refusing is the only honest output.

**The truncation banner.** Whenever a recording is truncated, torn, or dropped
samples, a non-collapsible banner names the numbers: *heat computed over N of M
instructions*, *K of L blocks*, the drop counters. A heat map over 4% of a run
looks exactly like one over all of it, which is precisely why the banner cannot
be dismissed.

**Operand timeline.** Per step: the disassembly (or offset), the captured
values, and the def-use in/out counts. The value annotations come from
[`cli/asmspy_dataview.h`](../cli/asmspy_dataview.h)'s `asmspy_df_annotate` —
the same inline helper the TUI calls, over a hand-filled `asmtest_valtrace_t`
view of the recording's own vectors — so the two frontends cannot drift into
different dialects of `->0x2a`. `test_timeline.cpp` pins two annotation strings
as literals to keep it that way. A step index no `df_step` event covered is
rendered as **unknown**, never as an instruction at offset 0.

**Slice explorer.** Click a step: the backward cone is everything that produced
the value there, the forward cone everything it goes on to affect. Both are
computed **client-side from recorded `df_edge` events** (plan D5), so the
render-only viewer slices with no engine linked. The layout is **layered by
step index, never force-directed** — the x column is the step's rank and each
arc takes the lowest free lane in a fixed order, so identical input gives an
identical picture every time. Over a truncated recording the cones are **lower
bounds** and the banner says so: an edge that was never recorded cannot be
followed, so a small cone may just be one we could not see the rest of.

**Diff, and the two-recording model.** Plan D3: every view takes one **or two**
recordings. With a B attached the canvas shows an A/B heat delta and a union
gutter, the timeline marks every row past the divergence *unaligned* (never
drawn as agreement), and the slice explorer shows A alone with a status line —
a merged two-recording graph needs the Wave-2 state-diff producer, and until it
exists inventing one would be worse than showing nothing. The diff panel itself
reports coverage deltas, heat deltas, hot-edge deltas under a **statistical**
chip, and the divergence card — *patient zero*, the mechanism
[05-loom-day-one.md](../docs/internal/gui/05-loom-day-one.md) consumes for its
fork comparison.

Three diff rules are structural. A pair that cannot be aligned (different
address bases, different architectures, no comparable stream) is **refused**
with a reason and produces no numbers. A verdict over a truncated recording is
**bounded**: "no divergence observed within the recorded window", never
"identical". And routine identity is **not verifiable in v1** — the `.asmtrace`
header carries no code-bytes hash, so every diff states plainly that the reader
is the one asserting the two recordings are of the same routine. Pretending to
have checked would be exactly the false confidence this tree exists to avoid.

**Deep links.** Every position is addressable:

```
asmtrace-link:v=slice&rec=add_signed.asmtrace&step=4
```

`v` is `canvas|timeline|slice|diff`, `rec` is a recording's basename, and
`rec_b`/`step`/`off` are optional. Parse and format round-trip byte-stably;
unknown keys are ignored (a link from a newer build still navigates here); a
link naming a recording that is not open is **refused with a reason**, never a
silent no-op. Every view registers one handler and every keyboard binding routes
through the router, so a link and a keypress land identically.

| Keys | Action |
|---|---|
| `1` `2` `3` `4` | canvas / timeline / slice explorer / diff |
| `j` `k`, `Down` `Up` | next / previous row or step |
| `PgDn` `PgUp` | page down / up |
| `Ctrl+G` | go to a step or offset |
| `Enter` | open the slice explorer at the selected step |
| `b` `f` | light the backward / forward cone |
| `c` | clear the cones |
| `[` `]` | walk one dependence generation back / forward |
| `d` | attach or detach a second recording (diff) |
| `x` | swap A and B |
| `n` `p` | next / previous divergence |
| `y` | copy a deep link to this position |

The table is `dt_nav_bindings()` in [`src/nav.cpp`](src/nav.cpp) — the help
overlay and the key map read the same data, so they cannot drift.

## The data layer and the backend-completeness panel

[`src/data/`](src/data/) reads what three existing producers already emit — the
live [`asmfeatures`](../tools/asmfeatures.c) sweep, the committed
[`benchmarks/boxes/`](../benchmarks/boxes/) records, and each box's append-only
`perf-history.jsonl` — so the completeness view needs a reader library and one
table, not new probes. Two properties drive the API:

- **"not measured" is not "measured zero".** `asmfeatures` prints JSON `null`
  for `complete` / `trace_insns` / `insns_truth` when it did not measure them,
  and `bench-report.sh`'s substitute probe row omits those keys entirely. Both
  spellings map to `std::nullopt` and render as an em dash. Defaulting them to
  `0`/`false` would turn "we never ran this" into "it captured nothing".
- **a torn line is normal.** `perf-history.jsonl` is appended by a process that
  can be killed mid-write, so an unparseable final line is skipped and
  **counted** (`PerfHistory::skipped`), never fatal and never silent.

The panel renders tier x backend x arch with `skip_reason` **verbatim** —
wrapped, never clipped, never paraphrased — and makes truncation loud:
`trace_insns < insns_truth` (the AMD-LBR plateau) or `complete:false` appends
` TRUNCATED` to the cell in the warn colour. Row order is the producer's
narrative order, never re-sorted. It never runs a sweep itself: it shows a
committed box record or an `asmfeatures` output you point it at, because a view
that silently probed would be reporting on a different machine than the box row
it is drawn under.

## The Loom

[`src/loom/`](src/loom/) is the Phase-2 flagship: a recorded run as a
**spacetime fabric** — horizontal is trace step, vertical is location lanes, and
every value is a worldline you can select, walk, audit and fork.

The model is engine-free and lives in both binaries. A fabric is built from what
a producer already yields — an `asmtest_valtrace_t` (per-step operand values)
and its last-writer def-use graph — into four things:

- **lanes**, in a fixed deck order taken from the producer's own tables (the
  SysV argument registers, then the rest of `df_zero_gp`'s order, then rsp/rip/
  eflags), with memory coalesced into **bands** on first touch;
- **spans** — one per write record, ending at the next write that overlaps it;
- **hops** — the def-use edges resolved onto those worldlines;
- **knots** — the steps that both read and write, where worldlines cross.

Five refusals are structural rather than cosmetic, and each is a test:

- **A statistical producer can never appear as fabric.** `loom_fabric_build`
  returns `false` with a reason and *no partial fabric*: a sample cannot say
  what a value was at a step. Statistical feeds reach the **lane annex** and
  stop there.
- **A value the producer never captured stays hollow** — outline, no chip. The
  route is known, the value is not, and the two must not look alike.
- **A worldline still live at the last recorded step gets a fade-out**, never a
  death cap. The fabric knows nothing overwrote it; it does not know the value
  ended.
- **A worldline whose first record is a read is born of untraced state** and
  carries a glyph saying its provenance starts at instrumentation.
- **The zeroization audit is bounded by the traced window and says so** in its
  own title: "clear" means *not overwritten within the traced window*, and
  untraced state and post-trace writes are invisible to it.

The **lane annex** joins what companion recordings in the workspace saw about
the same place — watch hits and taint hits by bytes, source-map rows and IBS
survey edges by code range. Its verdict enum has exactly **two** enumerators,
`corroborates` and `unconfirmed`, and a test with a `default`-less switch keeps
it that way: a statistical feed's silence proves nothing, and two exact
producers with different windows will honestly disagree without either being
wrong. Nothing the annex prints ever says "contradicts".

**Forks** are the counterfactual: change exactly one fact — an entry argument or
the routine's source — re-run from entry, weave the result beside the base.
Alignment is shared-prefix over per-step `insn_off` (04's
`dt_first_divergence`, so the Loom and the diff view can never place patient
zero differently), and the per-step verdict has three states: `dim` (dependence
without consequence), `hot` (a captured difference), and `neutral` whenever
either side never captured the value — equality of unknown values is never
claimed. An argument edit that changes no offsets says "aligned end-to-end"
rather than inventing a patient zero. `forks.cpp` is the only file here that
links the engines, so it compiles into the full app alone (D4), and every take
is bracketed by `emu_snapshot`/`emu_restore` because mapped memory deliberately
persists across `emu_call_*` — without the bracket take N would inherit take
N−1's dirt and the verdicts would depend on click order.

The four committed golden looms live in
[`tests/golden-asmtrace/`](../tests/golden-asmtrace/) (`loom-df-chain`,
`loom-sum-via-rbx`, `loom-truncated`, `loom-fork-demo`) and are regenerated by
`make asmtrace-golden`; `loom-df-chain` and `loom-sum-via-rbx` are the Learn
door's fabric walkthroughs.

**One doc-vs-code correction.** 05's draw plan specifies a truncation banner
reading "trace truncated: N of M steps recorded". The shipped v1 schema carries
no dataflow step **total** — the `end` footer has a truncated flag and nothing
else — so a replayed recording usually cannot supply M. Printing "6 of 6" would
claim the run ended where the buffer did, so the banner names the gap instead
("this feed did not record the total") and only prints the N-of-M form when a
feed really supplies both. A header total is a Phase-3-freeze item.

## The four doors

### Learn

`src/ui/learn_door.cpp` lists the recordings in `$ASMTRACE_LEARN_DIR` (default:
[`tests/golden-asmtrace/walkthroughs/`](../tests/golden-asmtrace/walkthroughs/))
and plays them. A walkthrough is **not** a document beside a recording — it IS
a recording, with ordered `stop:true` notes in it, so the player is a reader of
the same format every other view reads and a story cannot drift away from the
run it narrates.

The ladder is the one the docs already define:

| Walkthrough | Story |
|---|---|
| `square.asmtrace` | the quickstart routine, and the §5 "now break it" failure (expected 5, got 4) |
| `demo-fail.asmtrace` | why is rax wrong — `imax_wrong`'s inverted `cmovg`, framed expected `rax=4` / got `rax=3` |
| `ct_eq.asmtrace` | the capstone: three secret-differing inputs, a coverage union that does not grow, and the leaky control that does |
| `square-truncated.asmtrace` | the D7 dishonesty fixture — the same run recorded with the window cut short |

Regenerate with `make asmtrace-walkthroughs` **inside `make docker-desktop`**
(the pinned Keystone is what makes assembly reproducible). The target writes the
recordings, then writes a second copy to a temp directory and `cmp`s them, so a
non-deterministic generator fails at regeneration rather than in a future diff.

The truncated fixture earns its place: its last anchored stop points at a step
the recording does not contain, and the player must render *"stop is beyond the
recorded window"* and refuse to navigate. Silently clamping to the last recorded
step would show a reader one instruction while narrating another.

### Author (full app only)

Type assembly, pick an arch and dialect, run it on the emulator, see faults as
**data**. Three rules, all pinned by `test_author_vm`:

- **The assembler's diagnostic survives verbatim.** `asm_result_t.err` already
  names the common trap ("assembler skipped N of M statements (check the syntax
  argument)" — AT&T text under the Intel default). Rewriting it into "assembly
  failed" would delete the one sentence that says what to do. The source box is
  never cleared.
- **A fault is a card**: kind, address, the faulting instruction where Capstone
  is linked (bare offsets otherwise, D10), and the register file — under
  *"the emulator turned this into data; on real hardware this would have been a
  crash"*.
- **v1 runs the x86-64 guest only**, and the arch row says so. The other three
  assemble and show their bytes with the limit labelled, rather than a greyed
  button with no reason.

`desktop-render` shows a static tile naming the boundary
(*"Author mode requires the full (GPL-2.0) build"*) and links neither Keystone
nor Unicorn.

### This host — the capability panel

One panel rendering what this machine can do and *why not*, straight from the
library's status APIs. **The GUI never re-probes**: it calls
`asmtest_trace_resolve`, `asmtest_hwtrace_status` and the IBS reason functions
once at open (and on an explicit Refresh) and renders exactly what they said.

Two UI laws, both asserted in `test_capview` against synthetic data — which is
what lets a container with no PT, no LBR and no AMD silicon test them:

1. **A greyed row always shows its machine reason, verbatim** — plus the stage
   name and `perf_event_paranoid`, because "the PMU is absent" and "perf refused
   us" need different remedies. The IBS row carries **both** of its reasons,
   labelled `substrate:` and `last capture:`; they answer different questions and
   are never collapsed.
2. **Never auto-fall back across the native→virtual line.** A `[ ] native only`
   toggle re-resolves under `ASMTEST_TRACE_NATIVE_ONLY`; when the cascade comes
   back empty the panel says the library returns EUNAVAIL rather than silently
   downgrading, and offers the crossing as an explicit choice.

**A D9 note, stated rather than glossed.** D9 says the desktop app never links
the ptrace *engines*, and the capability cascade's objects include
`ptrace_backend.o`. The distinction the panel relies on is capture vs **query**:
nothing in the app calls a capture entry point — the Observer's capture host is
still the `asmspy --serve` subprocess — and what is linked is the availability
cascade a panel that "never re-probes" has to be able to ask. A panel shipped
without it would have to invent its own probes, which is exactly what D2 exists
to prevent. The render-only viewer links none of it and shows the **loaded
recording's** provenance instead, saying so.

## Live sessions — the Inspect door

`src/live/` is how the desktop captures, and the shape of it is the whole
design: **the GUI never links a tracer**. It spawns `asmspy --serve` as a
subprocess and speaks the NDJSON control protocol over its pipes
([`docs/internal/gui/asmtrace-schema.md`](../docs/internal/gui/asmtrace-schema.md),
*Serve protocol*). Locally that is `asmspy --serve`; remotely it is
`ssh <host> asmspy --serve` — **the same code path**, which is why remote
capture needed no second implementation and why macOS/Windows can host a session
against a Linux target at all.

The payoff is visible in the license split: **Inspect is in both binaries**.
`asmtest-viewer` lists processes, attaches, and streams a live session while its
`ldd` stays free of every engine, because reading `/proc` and framing NDJSON are
things a viewer can do and tracing is not (D4/D9).

### What a session is

A session's events **are** a recording's events. Between the `session started`
and the terminal lifecycle event, the host emits that mode's own provenance
header, its ordinary `.asmtrace` events, and an `end` footer — so a live capture
is a `Recording` like any other and every view already reads it. `LiveSession`
holds a growing one and hands back completed ones.

Three honesty rules ride in `session.cpp` rather than in the UI:

- **A host that dies mid-session leaves a TORN recording.** Not an error dialog,
  not a discarded buffer: what arrived is kept, and the recording says it is
  incomplete. `fake_serve.sh`'s `torn` mode reproduces it on demand.
- **A malformed line is counted, never fatal.** A live host is not a file; one
  bad line must not discard a session, and pretending it parsed would be worse.
- **A skip is a success.** `state:"skip"` means the tracer worked and had
  nothing to report, so it lives in its own field and renders in its own colour.
  Showing it as an error is the single most common way this fact gets
  misreported.

### The patch bay — one ptrace jack per target

`budget.h` is a pure table, and `test_budget` walks **all 100 mode pairs**. The
rule it encodes is a fact about the kernel, not a policy: a tracee has exactly
one tracer, and every ptrace view SEIZEs either all of the target's threads or
(for `procs`, which follows forks) its whole descendant **tree**. `sample` is
the one free slot — AMD IBS-Op reads out of band, attaches nothing, and can run
alongside anything.

So an occupied jack renders *"paused — another live view holds the tracer"*
naming the holder and what it is doing, and starting a second view offers an
explicit **swap** — never a silent one, because a swap stops someone else's
capture. The serve loop refuses a concurrent `start` too, but that refusal is a
backstop: the point of deciding client-side is that the user sees an occupied
jack instead of pulling a lever that returns an error.

### Seeing why not

Every process row carries a verdict *and its reason*, in the row — not a toast,
not an error the user has to provoke by trying. `attach_verdict` is pure and
`test_inspect` checks every combination, including the ones no single host can
produce, because the failure mode here is not a crash: it is telling someone to
raise a Yama `ptrace_scope` when the real problem is that **no privilege can
help**.

The facts are ordered by which one dominates:

| Fact | Verdict | Why it comes first |
|---|---|---|
| our own pid / a kernel thread | No | nothing to trace |
| a 32-bit (i386) tracee | No | asmspy decodes against the x86-64 syscall table — it would produce confident nonsense. **No capability fixes this** (`ASMSPY_ETRACEE_I386`) |
| already traced | No | a tracee has exactly one tracer; the remedy is stopping the other one |
| `CAP_SYS_PTRACE` held | Yes | it overrides uid **and** `ptrace_scope`, so testing it later would report a uid problem that does not apply |
| `ptrace_scope=3` | No | one-way: it cannot be lowered without a reboot, so offering any other remedy would be a lie |
| `ptrace_scope=2` | No | capability-only |
| different uid | No | remedy: that user, or the capability |
| `ptrace_scope=1` | **Unknown** | it permits a descendant or a `PR_SET_PTRACER` opt-in, and whether the target opted in is *not readable from outside* — an honest Unknown beats a confident Yes that fails at attach |
| same uid, scope 0 or Yama absent | Yes | |

### The `--auto` front door, and its evidence

`mode:"auto"` picks a hot region instead of making you name one, and it reports
**what it picked and how good the evidence was**. That distinction is
load-bearing, not chrome:

- **`entry`** — an AMD IBS-Op branch whose target is a function's start. This is
  a direct observation of the event the capture waits for.
- **`residency`** — the portable software-clock fallback. A PC histogram says a
  function was *executing*, which is a **different claim**. A function entered
  once and never re-entered is the top residency winner, and the capture's entry
  breakpoint would never fire on it.

So a residency pick is labelled *"WEAKER EVIDENCE — residency, not entry"*
together with that consequence, and when the server walks past a candidate that
was never seen entering, each `pick` event surfaces with a note saying the
refusal is about *that candidate* and **not a fact about the target**. A silent
substitution would be the front door claiming a measurement it never made.

### Testing it without a tracer

`desktop/test/fixtures/fake_serve.sh` speaks the protocol and emits canned
events, so `test_live_session` and `test_inspect` run on any machine: no ptrace,
no permissions, no victim, no AMD silicon. That is not a compromise — it is what
lets the awkward shapes (a refusal mid-session, a skip, a torn stream, a
never-ran candidate walk) be tested *on demand* rather than by luck. The real
`asmspy --serve` is covered end to end on the other side of the seam, by the
serve section of `make cli-smoke`.

## The live Observer views

Seven views over a live session — and over any recording of one, which is the
same statement twice: they are built from the document model, so the Inspect
door and a replayed `.asmtrace` tab draw the *same* deck from the *same* code.
That is what makes every rule below testable on a machine with no target to
attach to, no AMD silicon and no Intel PT.

**Syscalls.** The schema splits each syscall in two — a payload-free `line`
(with `<path>` / `<N bytes>` placeholders) and the decoded content in a separate
`payload`. So the payload column is **redacted by default**, revealed per row,
and the session-wide reveal takes a second confirmation that names what it is
about to put on screen. The two ways a payload can be missing never read alike:
*hidden by this renderer* and *withheld at record time* (`provenance.redacted` —
the bytes are not in the file at all, and revealing cannot conjure them). There
is deliberately no tid filter: the syscalls engine takes no `only_tid`, so a
filter here would hide rows rather than narrow the capture, and the view says so
where the control would have been.

**Watchpoint timeline.** `is_write` has **three** values — write, read, and
*undecodable* (the trap fired, the faulting instruction did not decode) — and
the third keeps its own word, because collapsing it into either other one
invents a measurement. `value_ok` is separate from `value`: a value that was
never read back renders as *"(not read back)"*, never as `0`. A refused arm is
**not an error**: it is a successful session that had nothing to report, and its
reason is `asmspy_hwdebug_reason()`'s string verbatim — the arm64 class of host
that advertises debug slots and then refuses to reserve one says exactly that,
because "watchpoint unavailable" would send an operator to the wrong place.

**Topology.** Tasks fold into one card per process, ordered by pid (an ordering
that moves while you read it is one you cannot point at). Two facts ride along:
the `procs` engine SEIZEs the whole **descendant tree**, so the ptrace jack is
held for every process below — stated up front rather than discovered as an
unexplained refusal elsewhere — and `inv` counts *syscalls* or *calls* depending
on the mode, so the number is never shown without its unit. Drill-in is a deep
link through the router, so "show me this process" is pasteable.

**Hot edges.** IBS-Op samples are retired **branches**: a from-address and a
to-address. Nothing in them observed a call stack, so there is no flame graph
here and there will not be one — frames would be inferred ancestry drawn in the
same ink as measurement. The provenance chrome (`samples`, `branch_samples`,
`lost`, `throttled`, sampler, window) is always visible, and the ranking is
deterministic so two screenshots can be compared. The evidence grade is stated,
not implied: an IBS-Op edge is a **direct observation** of control arriving
somewhere; a software-clock survey is **residency**, which says a function was
*executing* — a weaker and different claim. A survey is `exact:false` by
construction, so a recording claiming otherwise is reported as a producer defect
and still rendered as statistical.

**Call tree + filter panel.** The day-one place the GUI exceeds the TUI, at zero
engine cost: `asmspy_tree_filter_t` (depth / focus / module) has always existed
and the TUI passes NULL. The filter is **engine-side by design** — it bounds
what the engine *emits* while it keeps tracking every call and return, so the
depths on the surviving lines stay true. The panel refuses the combinations the
protocol refuses, in the protocol's own words (`"tid" pins ONE task; "follow"
adds child processes — drop one`), so nobody learns a rule by having a command
bounce. What the view shows as *running* is the server's `started` params echo,
never the client's own request replayed back to itself.

**Invocations.** A region capture is not a continuous timeline: the engine waits
at the region's entry, records one invocation, and waits again — and between
them the target ran unobserved for an unknown time. So this pages between
numbered snapshots and **never scrubs**, and each snapshot renders through 04's
canvas. The split is read from the stream's order (a `coverage` event closes the
invocation before it), so a capture cut off mid-invocation is kept and marked
*open* rather than shown as a short run. Two warnings ride with it because they
are properties of the capture: the single-step **crawl**, and — when the
recording carries `codeimage` versions — the steer to the out-of-band IBS survey
for JIT code.

**Disassembly, as of a time.** A JIT patches, frees and reuses code addresses,
so bytes read after the fact are not the bytes that ran. The `codeimage` event
carries the producer's timestamped versions into the recording, and this pane
resolves an address at trace time `t` to the version with the **greatest `when`
≤ t** — never the newest, never the next one along. Where no version qualifies,
the bytes are **unknown**: showing the succeeding method's code for the
preceding method's trace is exactly the failure the mechanism exists to prevent.
It never re-reads live memory. Absent a code image it falls back to the recorded
`disasm` strings (D10) and **labels the row as the weaker source**.

### The PT-replay slice (full app)

On a PT host a def-use slice can be captured with **zero single-steps of the
target**: Intel PT records the control-flow path in hardware, and the F5
producer replays that path through the emulator against the recorded code image
to reconstruct the operand values the hardware never captured. The result is an
ordinary def-use stream, so the slice explorer and the Loom draw it unchanged.

Two things are worth being precise about. First, the **two grades of evidence in
one view**: the path is measured, the values are reconstructed, and the
disclosure says both. Second, **the gate has two levels** — *capture* needs PT
silicon (self-skipping with the library's measured reason), while *replay* needs
only Unicorn and Capstone, because the path was decoded when it was captured and
recorded as `stitch` beside the `codeimage` bytes it was decoded against. That
is why `test_ptslice_run` replays a recorded slice on hosts with no Intel PT at
all, instead of the whole feature living behind hardware nobody in CI has.

The producer ships **no public header, on purpose** (a value-trace producer is a
tier, not part of the shared sink API), so `ptslice.cpp` re-declares its entry
points exactly as the tier's own smoke drivers do — in one place, with the
layout self-check asserted at init, and a mismatch **refuses to run** rather than
reporting telemetry it cannot trust. Promoting that surface to a public header is
a library decision this work does not make.
