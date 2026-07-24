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
    views/
      canvas.*          per-offset heat, block gutter, basis refusal
      timeline.*        per-step values via cli/asmspy_dataview.h's grammar
      slice_view.*      the layered def-use DAG + cones
      diff_view.*       the A/B summary panel, every row a deep link
      completeness*     the tier x backend capability table
      *_draw.cpp        the thin ImGui half of each view (draws only)
    ui/
      shell.{h,cpp}     the home screen (three doors), open dialog, and tab strip
                        — backend-free ImGui draws the null backend can drive
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
