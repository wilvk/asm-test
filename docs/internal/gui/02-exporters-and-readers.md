# `.asmtrace` exporters & completeness-data readers (GUI Phase 1 tail) — implementation

> **Sources.** Actioned from [desktop-gui-plan.md](../plans/desktop-gui-plan.md):
> Phasing **Phase 1** ("speedscope + Perfetto exporters; readers for
> features/bench artifacts"), the engine-work-items row "speedscope / Perfetto
> exporters", design **D1**, the view-catalog row **Backend completeness**, and
> the UX "Professional workflows" exporters bullet. Written 2026-07-23. If this
> doc and a source disagree, this doc wins (sources may be stale); if the CODE
> and this doc disagree, re-verify before implementing.
>
> **Implemented 2026-07-24 (T1–T6).** Three corrections were made where this
> doc disagreed with the shipped code, and the code won (see *Schema
> reconciliation* below): the event-kind selector is `"k"`, not `"ev"`;
> call-tree events are kind **`call`** with an integer `addr`, not `tree`; and
> the address basis is `"rel"` / `"abs"`, not `"region"`. Truncation and drops
> ride the `end` footer (a missing footer means TORN), not a `provenance`
> event. The fixture and expected-output names below are the ones that shipped.
>
> Read [\_conventions.md](../implementations/_conventions.md) first; the
> doc-set shared decisions (D1–D11) live in this directory's README and bind
> every task. Siblings: [01-asmtrace-format.md](01-asmtrace-format.md)
> (schema, serializers, golden corpus), [03-desktop-shell.md](03-desktop-shell.md)
> (desktop/ skeleton, `mk/desktop.mk`, test harness),
> [07-serve-live-host.md](07-serve-live-host.md) (live record modes).

## Why this work exists

Recordings are the unit of collaboration: capture `.asmtrace` in CI or a
container, render anywhere. Industry viewers already exist for the two
commodity render shapes — **speedscope** (evented call profiles) and
**Perfetto** (timeline/counter tracks) — so Phase 1 ships a dependency-free
exporter instead of rebuilding those views; the plan's acceptance is literal:
"exporters open in their tools". Separately, the **backend completeness** view
renders data three producers already emit (the live asmfeatures sweep, the
committed `benchmarks/boxes/` records, the `perf-history.jsonl` trend lines),
so the desktop app needs a small tested reader library plus one table view,
not new probes. Honesty is load-bearing in both halves: statistical survey
events are **never** exported as stacks, and the completeness panel renders
`skip_reason` verbatim with truncated captures marked loudly (D7).

## What already exists (verified 2026-07-23)

- [tools/asmfeatures.c](../../../tools/asmfeatures.c) — the capability sweep.
  Its emit function [`row()`, :65](../../../tools/asmfeatures.c#L65) prints
  exactly the keys `tier, backend, arch, scope, available, skip_reason,
  fidelity, complete, trace_insns, insns_truth` plus optional `note`
  ([:95](../../../tools/asmfeatures.c#L95)); the three measured fields print
  JSON `null` when unmeasured ([:83–94](../../../tools/asmfeatures.c#L83));
  envelope `{"features":[…]}` ([:368](../../../tools/asmfeatures.c#L368)).
  [main, :357](../../../tools/asmfeatures.c#L357) emits rows in order:
  emulator guests, native capture ladder
  ([:417–474](../../../tools/asmfeatures.c#L417)), `native-oop`, static
  hwtrace backends, DynamoRIO, disasm.
- [scripts/bench-report.sh](../../../scripts/bench-report.sh) — merges the
  producers into `{"schema":"asmtest-bench-report/v1","system":…,
  "performance":…,"features":[…]}`
  ([:160–165](../../../scripts/bench-report.sh#L160)); the 12 `system` fields
  at [:146–159](../../../scripts/bench-report.sh#L146). `--record` persists
  `benchmarks/boxes/<box_id>/features.json`
  ([:210–212](../../../scripts/bench-report.sh#L210)) and appends one
  `perf-history.jsonl` line per run — keys `timestamp, commit, os, arch,
  unit, virtualized, native:[{name,median,unit}]`
  ([:217–221](../../../scripts/bench-report.sh#L217)). Where asmfeatures is
  not built, a substitute probe row **omits** the measured keys
  ([:117–125](../../../scripts/bench-report.sh#L117)).
- [benchmarks/boxes/](../../../benchmarks/boxes/) — nine committed box dirs.
  Verified against
  [amd-linux-x86_64-9e05f0f2/features.json](../../../benchmarks/boxes/amd-linux-x86_64-9e05f0f2/features.json):
  `{"system":…,"features":[…]}` with null-`insns_truth` emulator rows exactly
  as `row()` emits; its `perf-history.jsonl` matches the shape above.
- [src/trace.c](../../../src/trace.c) —
  [`emu_trace_lcov`, :225](../../../src/trace.c#L225) needs only distinct
  block offsets + a name: `TN:` / `SF:<name>` / `DA:<offset>,1` per sorted
  block / `LF:n` `LH:n` / `end_of_record`
  ([:229–237](../../../src/trace.c#L229)).
  [`emu_trace_lcov_source`, :338](../../../src/trace.c#L338) also needs an
  `emu_line_map_t` — data no Phase 1 recording carries (see Out of scope).
- [cli/asmspy.c](../../../cli/asmspy.c) — the DOT exporters the offline modes
  mirror: [`dot_escape`, :322](../../../cli/asmspy.c#L322) (escapes `"`, `\`);
  [`tree_export_dot`, :895](../../../cli/asmspy.c#L895) aggregates the
  temporal call log via a per-thread **shadow stack**
  ([:890–894](../../../cli/asmspy.c#L890)), clamped 64 deep, node fills
  [`tree_fill`, :882](../../../cli/asmspy.c#L882);
  [`tree_export_json`, :981](../../../cli/asmspy.c#L981) names the structured
  fields `seq,tid,depth,addr,name,module`
  ([:988–991](../../../cli/asmspy.c#L988)) — the field-name precedent for
  01's tree kind.
- [cli/asmspy.h](../../../cli/asmspy.h) —
  [`asmspy_tree_call_t`, :404–413](../../../cli/asmspy.h#L404): the tree
  engine emits **entries only** (no leave/return callback —
  [`asmspy_tree_sink`, :418](../../../cli/asmspy.h#L418)); `--focus` re-bases
  depth ([:406–409](../../../cli/asmspy.h#L406)).
  [`asmspy_sample_edge_t`, :502–510](../../../cli/asmspy.h#L502) is the
  survey shape ("STATISTICAL, never exact").
- Build wiring: help target at [Makefile:91](../../../Makefile#L91)
  (hand-maintained echo list); `include mk/cli.mk` at
  [Makefile:929](../../../Makefile#L929) — D3 puts `include mk/desktop.mk`
  right after it. Docker-rule pattern:
  [`docker-cli`, mk/cli.mk:407–411](../../../mk/cli.mk#L407); tool-binary
  pattern: [mk/bench.mk:43](../../../mk/bench.mk#L43).
- Greenfield (confirmed absent today): no `desktop/`, `mk/desktop.mk`,
  `tests/golden-asmtrace/`, or `tools/asmtrace_export.c`; nothing matches
  `asmtrace` in any makefile. Every such name below is **new**.

## Tasks

### T1 — `asmtrace_export`: NDJSON reader + speedscope backend  (M, depends on: none)

**Goal.** A dependency-free C tool (`tools/asmtrace_export.c`, new): recording
in, speedscope evented profile out — startable before any sibling lands.

**Steps.**
1. Create `tools/asmtrace_export.c` (single TU, C11, libc only — no engine
   objects, no Capstone, no JSON library; builds on any host). **C11 means it:
   `getline` and `strdup` are POSIX, not C11, so the TU carries a four-line
   line reader and an `xstrdup` rather than a feature-test macro.**
2. Line reader: line 1 is the header; require `"asmtrace":1` (newer major →
   exit 2 + reason; unknown header fields ignored). Detect the zstd magic
   `28 B5 2F FD` at byte 0 and refuse: "compressed container: not supported by
   this exporter yet — decompress first" (container reserved in plan D1).
3. Minimal flat-JSON field scanner for event lines: extract known
   string/int/bool fields by key; **skip unknown fields structurally** (track
   brace/bracket depth + in-string state honoring `\"`); unescape `\"` `\\`
   `\n` `\t` `\/` `\b` `\f` `\r` and `\uXXXX` (the writer escapes control
   bytes that way — schema *Determinism rules* 4), fail loudly on anything
   else;
   ignore unknown `"ev"` kinds (D1 forward-compat rule).
4. Speedscope backend over tree events (algorithm below).
5. Create `mk/desktop.mk` **if 03 has not landed yet** (shared per D3;
   whichever of 02/03 lands first creates it plus the include directly after
   [Makefile:929](../../../Makefile#L929)). Add `$(BUILD)/asmtrace_export:
   tools/asmtrace_export.c | $(BUILD)` (`$(CC) $(CFLAGS)`) and a phony
   `asmtrace-export` alias.

**Code.** CLI surface (all modes, incl. T2/T3; default output stdout):

```
asmtrace_export --speedscope [--out=FILE] REC.asmtrace
asmtrace_export --chrome     [--heat-cap=N] [--out=FILE] REC.asmtrace   (T2)
asmtrace_export --lcov       [--name=SF]    [--out=FILE] REC.asmtrace   (T3)
asmtrace_export --dot-tree   [--out=FILE]   REC.asmtrace                (T3)
exit 0 = wrote output; 1 = I/O or parse error; 2 = honest refusal
```

Consumed input subset — **as shipped**, reconciled against 01's schema
([asmtrace-schema.md](asmtrace-schema.md)), which landed first and therefore
wins. The event-kind selector is `"k"`; call-tree entries are kind `call` with
an **integer** `addr`; `basis` is `"rel"` or `"abs"`; and truncation/drops ride
the `end` footer rather than a separate provenance event:

```
{"asmtrace":1,"container":"ndjson","producer":{...},"provenance":{...},"arch":"x86_64","pid":4242}
{"k":"call","tid":4242,"depth":0,"addr":4198710,"name":"main","module":"exe"}
{"k":"trace","basis":"rel","kind":"insn","off":18,"disasm":"…"}   # disasm optional (D10)
{"k":"coverage","basis":"rel","blocks":[0,12,18],"blocks_total":5,"insns_total":37,"truncated":false}
{"k":"survey", ...}                            # statistical — NEVER a stack
{"k":"end","events":37,"truncated":false,"drops":{"lost":0,"throttled":false}}
```

A recording with **no `end` line is TORN** and is treated as incomplete
everywhere the footer's `truncated` would be. `basis` is mandatory and is never
defaulted: an event without one is a parse error, not a guess.

Speedscope output — exactly the file-format fields (one profile per tid;
frames deduped by `name + " [" + module + "]"`):

```json
{"$schema": "https://www.speedscope.app/file-format-schema.json",
 "name": "<input basename>", "exporter": "asmtrace_export", "activeProfileIndex": 0,
 "shared": {"frames": [{"name": "main [exe]"}]},
 "profiles": [{"type": "evented", "name": "tid 7", "unit": "none", "startValue": 0,
   "endValue": 12, "events": [{"type": "O", "frame": 0, "at": 0},
                              {"type": "C", "frame": 0, "at": 12}]}]}
```

Two subtle rules, spelled out:

- **Enter-only streams → open/close pairs.** The tree engine emits entries
  only ([cli/asmspy.h:415–419](../../../cli/asmspy.h#L415)); closes are
  synthesized from depth transitions, the shadow-stack discipline of
  [`tree_export_dot`](../../../cli/asmspy.c#L890): per tid keep a stack of
  open frames `(frame_index, depth)`; on a tree event at depth `d`, per-tid
  ordinal `t`: emit `"C"` at `t` for every open frame with `depth >= d`
  (innermost first — speedscope closes most-recent-first), then `"O"` at `t`
  and push; at end-of-stream close everything at `endValue = last ordinal +
  1`. A depth jumping deeper by more than 1 is legal (`--focus` re-bases
  depths) — just push.
- **The time axis is the per-tid event ordinal.** No producer feed carries
  timestamps (plan, "Killed in grounding"); `"unit":"none"` says so honestly.

Honesty chrome: provenance `truncated:true` or `lost > 0` appends
`" (truncated)"` to each affected profile's `name`. No tree events → exit 2:
`"refusing --speedscope: no call-tree events in this recording"`; with survey
events present, extend with `"survey events are statistical histograms, not
stacks (edges are not stacks)"`. No mode ever consumes survey events.

**Tests.** The T4 fixture suite; hand-run against those fixtures meanwhile.

**Docs.** File-top comment block is the doc (repo idiom, cf.
[tools/asmfeatures.c:1–25](../../../tools/asmfeatures.c#L1)): modes, exit
codes, ordinal-time rule, refusals. Sphinx docs wait for the Phase-3 freeze.

**Done when.**
- `make asmtrace-export` builds `build/asmtrace_export` with libc only.
- `build/asmtrace_export --speedscope tests/golden-asmtrace/export/tree-small.asmtrace | python3 -m json.tool` succeeds.
- The output opens in speedscope (drag onto https://www.speedscope.app — manual, once).
- `build/asmtrace_export --speedscope tests/golden-asmtrace/export/survey-only.asmtrace; echo $?` prints the refusal reason and `2`.

### T2 — Chrome Trace Event backend for Perfetto  (S, depends on: T1)

**Goal.** `--chrome`: B/E duration events from tree events, counter events
from per-offset heat, loadable in Perfetto (ui.perfetto.dev).

**Steps.**
1. Add `--chrome`, reusing T1's reader and open/close algorithm.
2. Emit the JSON-object form `{"traceEvents":[…],"displayTimeUnit":"ms",
   "otherData":{…}}` with exactly these event keys:
   - `{"name":"main [exe]","cat":"tree","ph":"B","ts":0,"pid":1,"tid":7,
     "args":{"addr":"0x401000","depth":0}}` and the matching `"ph":"E"` at
     the close ordinal — T1's synthesis rule, B/E strictly nested per tid.
   - counter: `{"name":"heat","cat":"trace","ph":"C","ts":3,"pid":1,
     "args":{"0x12":2}}` — while scanning ordered `trace` events keep
     `hits[off]`; at each trace event's ordinal emit one counter event with
     that offset's new cumulative count (each offset is its own series under
     the "heat" track — the multi-series `args` form). Cap distinct offsets
     at `--heat-cap` (default 256); past it, drop new offsets and record
     `"heat_offsets_dropped": N` in `otherData` — never silently.
3. `otherData` honesty block: `{"ts_unit":"event ordinal — the producers
   record no timestamps","truncated":bool,"lost":N,"heat_offsets_dropped":N}`.
4. Refusals as T1 (neither tree nor trace events → exit 2; survey-only → the
   edges-are-not-stacks reason).

**Code.** No new structures — one emit helper per `ph` kind. `pid` = recorded
pid if the header carries one, else `1`; counter events carry no `tid`.

**Tests.** In T4: golden byte-compares of both `.chrome.json` files;
`python3 -m json.tool` validity; grep for `ts_unit`; refusal exit codes.

**Docs.** Extend the file-top comment (keys, `--heat-cap`, series rule).

**Done when.**
- `build/asmtrace_export --chrome tests/golden-asmtrace/export/tree-small.asmtrace | python3 -m json.tool` succeeds.
- The file loads in Perfetto with one track per tid + a "heat" counter track (manual, once).
- `make asmtrace-export-test` passes its chrome-mode assertions.

### T3 — lcov + tree-DOT offline regeneration  (S, depends on: T1)

**Goal.** Regenerate offline, from a recording, the two passthrough exports
that today exist only at capture time — block-offset lcov and the `--tree`
DOT graph — so "capture in CI, run genhtml / dot later" needs no re-run.

**Steps.**
1. `--lcov`: distinct block offsets from `coverage` events (union of `blocks`
   arrays); if none, derive from `trace` events' offsets. Emit
   **byte-identically** the [src/trace.c:229–237](../../../src/trace.c#L229)
   record: `TN:`, `SF:<--name or "routine">`, `DA:<offset-decimal>,1`
   ascending, `LF:<n>`, `LH:<n>`, `end_of_record`. Refuse (exit 2) if
   `rel`- and `abs`-basis events are mixed — different bases must never
   merge into one SF record (the plan's named attribution failure mode).
2. `--dot-tree`: re-implement [`tree_export_dot`](../../../cli/asmspy.c#L895)'s
   aggregation over recorded tree events — same per-thread shadow stack
   (caller = latest strictly-shallower entry, same tid; depth clamped to 63,
   [:937](../../../cli/asmspy.c#L937)), same output: the
   `digraph asmspy { rankdir=LR; …` header
   ([:957–958](../../../cli/asmspy.c#L957)), addr-keyed nodes labelled
   `name\n[module] entered=N`, `tree_fill` colours (jit `#fff3c4`, `?`
   `#ffe0e0`, named `#e8f0ff`), count-labelled edges, `dot_escape`d labels.
   Copy those two static helpers into the exporter TU — `asmspy.c` is not a
   library and must not be linked.
3. Truncated provenance: `--dot-tree` appends a `# truncated recording`
   trailer comment; `--lcov` warns on stderr (format stays pristine).

**Code.** No new structures; ~120 lines in the exporter TU.

**Tests.** In T4: `trace-heat.info` + `tree-small.dot` golden compares and a
sanity grep (`^DA:` count == `LF:` value); graphviz is not a repo dep —
byte-exact goldens suffice.

**Docs.** File-top comment: both modes + the basis-mixing refusal.

**Done when.**
- `build/asmtrace_export --lcov --name=corpus tests/golden-asmtrace/export/trace-heat.asmtrace` matches its golden byte-for-byte.
- `build/asmtrace_export --dot-tree tests/golden-asmtrace/export/tree-small.asmtrace` matches its golden byte-for-byte.
- A mixed-basis fixture exits 2 with the basis reason.

### T4 — Fixtures, golden tests, make/help/CHANGELOG wiring  (S, depends on: T1, T2, T3)

**Goal.** Committed hand-authored fixtures + a test script pinning every
exporter mode and refusal, wired as `make asmtrace-export-test`.

**Steps.**
1. Create `tests/golden-asmtrace/export/` (new; the parent dir is D6's golden
   corpus, owned by 01 — this subdir is **hand-authored, never regenerated**.
   01's `asmtrace-golden` already writes only flat `*.asmtrace` files into the
   parent and `asmtrace-golden-check` compares only those, so no change to
   those targets is needed; `export/README.md` records that.) Fixtures as
   shipped: `tree-small.asmtrace` (two tids; multi-frame depth jump-down;
   re-based jump-up), `tree-truncated.asmtrace` (`end` footer with
   `truncated:true, lost:3` — a D7 dishonesty fixture), `trace-heat.asmtrace`
   (loop: repeated trace offsets + one coverage event),
   `trace-truncated.asmtrace` (the lcov stderr-warning path),
   `survey-only.asmtrace`, `mixed-basis.asmtrace`, `future-major.asmtrace`
   (`{"asmtrace":2}`), `zstd-container.asmtrace` (the reserved compressed
   container, by magic), `unknown-kind.asmtrace` (an unknown kind AND an
   unknown nested field on a known one); beside them, the expected outputs
   `tree-small.{speedscope.json,chrome.json,dot}`,
   `trace-heat.{chrome.json,info}`, `tree-truncated.{speedscope.json,dot}`,
   `trace-truncated.info`, `unknown-kind.speedscope.json`.
2. Add `scripts/test-asmtrace-export.sh` (POSIX sh, `set -eu`): per mode, run
   the exporter, validate with `python3 -m json.tool` (python3 is on every CI
   leg — [scripts/bench-report.sh:13–14](../../../scripts/bench-report.sh#L13)),
   `cmp` against the expected file; assert exit 2 + a reason-grep for the
   three refusal fixtures; grep `" (truncated)"` and `ts_unit` in the
   respective outputs (D7). `UPDATE_GOLDEN=1` rewrites expected files.
3. In `mk/desktop.mk`: `asmtrace-export-test: $(BUILD)/asmtrace_export`
   running the script. Once 03's `desktop-test` exists, add it as a
   prerequisite there (one line; do not block on 03); it runs inside 03's
   `docker-desktop` image for free — only cc + python3 needed, so no lane
   self-skips.
4. Help echo lines under [Makefile:91](../../../Makefile#L91) for both new
   targets (D3), and one `### Added` bullet under `## [Unreleased]` in
   [CHANGELOG.md](../../../CHANGELOG.md).
5. When 01's corpus generator lands, add one canary line: `--speedscope` over
   one generated corpus recording, assert exit 0 (guard with "file exists" —
   a bonus assertion, not a self-skip of the suite).

**Tests.** This task *is* the tests.

**Docs.** `tests/golden-asmtrace/export/README.md`.

**Done when.**
- `make asmtrace-export-test` exits 0 and prints one OK line per fixture/mode pair.
- Corrupting one byte of `tree-small.speedscope.json` makes it fail (byte-exact — D6's byte-stability discipline).
- `make help` lists both targets; `CHANGELOG.md` has the bullet.

### T5 — completeness-data readers under `desktop/src/data/`  (M, depends on: 03's skeleton)

**Goal.** A tested C++17 library loading the three completeness data sources
into typed rows via the pinned nlohmann/json (D2) — engine-free (D4).

**Steps.**
1. Create `desktop/src/data/features_data.{h,cpp}` and
   `desktop/src/data/perf_history.{h,cpp}` (new; `desktop/` and its build
   rules come from [03-desktop-shell.md](03-desktop-shell.md)).
2. `load_features` accepts all **three** committed envelopes: bare asmfeatures
   stdout `{"features":[…]}`; the box file `{"system":…,"features":[…]}`; the
   full report (`"schema":"asmtest-bench-report/v1"`) — dispatch on present
   keys. Missing keys and JSON `null` both map to `std::nullopt` (the probe
   fragment omits measured keys entirely); unknown fields ignored.
3. `load_perf_history` over an `std::istream` of JSONL: parse each line
   independently; a malformed line — including a torn final line of the
   append-only file — is **skipped and counted**, never fatal.
4. `scan_boxes(repo_root)`: enumerate `benchmarks/boxes/<box_id>/`
   (std::filesystem), sorted by `box_id`.
5. Register both TUs in 03's `mk/desktop.mk` object lists (both app binaries
   + the test binary).

**Code.**

```cpp
// desktop/src/data/features_data.h        namespace asmdesk::data
struct FeatureRow {                        // one row() emission, verbatim keys
    std::string tier, backend, arch, scope;
    bool available = false;
    std::string skip_reason;               // "" when available
    std::string fidelity;                  // "virtual-exact" | "native" | "n/a" | ""
    std::optional<bool>         complete;  // null / absent when unmeasured
    std::optional<std::int64_t> trace_insns, insns_truth;
    std::optional<std::string>  note;      // workload label; absent on most rows
};
struct BoxSystem {                         // the 12 keys of bench-report.sh:146-159
    std::string box_id, os, os_version, arch, cpu, uarch, vendor, cc,
                asmtest_version, commit, timestamp;
    bool virtualized = false;
};
struct FeaturesDoc { std::optional<BoxSystem> system; std::vector<FeatureRow> features; };
FeaturesDoc load_features(const nlohmann::json &doc);      // throws std::runtime_error
FeaturesDoc load_features_file(const std::string &path);   // path + reason in what()
// desktop/src/data/perf_history.h
struct PerfPoint { std::string name; double median = 0; std::string unit; };
struct PerfLine  { std::string timestamp, commit, os, arch, unit;
                   bool virtualized = false; std::vector<PerfPoint> native; };
struct PerfHistory { std::vector<PerfLine> lines; std::size_t skipped = 0; };
PerfHistory load_perf_history(std::istream &in);
struct BoxRecord { std::string box_id, dir; bool has_features, has_history; };
std::vector<BoxRecord> scan_boxes(const std::string &repo_root);
```

**Tests.** `desktop/test/test_data_readers.cpp` (plain `main()`, nonzero on
first failure printing `FAIL <what>`; registered in 03's `desktop-test` —
headless, no display, no GL): loads the **committed real record**
[amd-linux-x86_64-9e05f0f2/features.json](../../../benchmarks/boxes/amd-linux-x86_64-9e05f0f2/features.json)
asserting `system->box_id` and first row `{tier=="emulator", arch=="x86_64",
trace_insns==4, !insns_truth}`; crafted fixtures under
`desktop/test/fixtures/` — `features-bare.json`, `features-report.json`,
`features-probe.json` (missing measured keys), `features-unknown-keys.json`,
`perf-history-torn.jsonl` (`skipped==1`); `scan_boxes(".")` finds ≥ 9 boxes
incl. the amd one.

**Docs.** Header comments name the producing scripts/lines; two-line
data-layer paragraph in `desktop/README.md` (03's file).

**Done when.**
- `make desktop-test` runs `test_data_readers` green on a display-less host.
- `make desktop-render` still links (the readers pull no engine object — D4).
- Deleting the `note` key from a fixture row does not fail the load.

### T6 — backend-completeness panel + golden-render test  (M, depends on: T5)

**Goal.** The desktop table view: tier × backend × arch, `skip_reason`
verbatim, `trace_insns` vs `insns_truth` — with a headless golden-render test.

**Steps.**
1. Create `desktop/src/views/completeness_model.h` (pure view-model — no
   ImGui, no I/O; the [cli/asmspy_logview.h](../../../cli/asmspy_logview.h)
   pure-header-under-test pattern) and `desktop/src/views/completeness.cpp`
   (ImGui rendering, registered in 03's shell as "Backend completeness").
2. View-model rules (all tested):
   - **Row order = producer order** ([main, :357](../../../tools/asmfeatures.c#L357)
     already emits the narrative order); never re-sort alphabetically.
   - **status**: `"ok"` when `available`, else the `skip_reason` **verbatim**
     — never truncated, never paraphrased (plan D2 UI law; CoreSight renders
     as its skip_reason).
   - **completeness** cell: `trace_insns` absent → `"—"`; `insns_truth` absent
     → `"<trace_insns> insns"`; both → `"<trace_insns>/<insns_truth>"`; append
     `" TRUNCATED"` whenever `complete == false` **or** `trace_insns <
     insns_truth` (the AMD-LBR plateau made loud), `" complete"` when
     `complete == true` and the counts agree.
   - Header line: `box_id — cpu — os_version — commit timestamp` from
     `BoxSystem` when present, else `"live sweep (this host)"`.
3. ImGui rendering: `ImGui::BeginTable`, columns Tier | Backend | Arch |
   Scope | Status | Completeness | Note; unavailable rows greyed
   (`ImGuiCol_TextDisabled`), reason fully readable (wrap, don't clip);
   TRUNCATED cells in the warn colour. A combo fed by `scan_boxes` switches
   between box records and a user-chosen `asmfeatures` output file — the
   panel itself never probes.
4. Implement `render_completeness_text(table)` — fixed-width plain-text
   rendering of the same view-model (the golden-test surface).

**Code.**

```cpp
// desktop/src/views/completeness_model.h  (pure; includes only data/ + <string>)
struct CompletenessRow {        // status = "ok" | skip_reason VERBATIM;
    std::string tier, backend, arch, scope, status, completeness, note;
    bool available = false;     // completeness/note = "" when absent
};
struct CompletenessTable {
    std::string box_label;
    std::vector<CompletenessRow> rows;   // producer order
    std::size_t n_rows = 0, n_available = 0, n_measured = 0, n_complete = 0;
};
CompletenessTable build_completeness(const asmdesk::data::FeaturesDoc &doc);
std::string render_completeness_text(const CompletenessTable &t);
```

**Tests.** `desktop/test/test_completeness_view.cpp` (headless; in
`desktop-test`): committed amd box record → `render_completeness_text` →
byte-compare against committed `desktop/test/golden/completeness-amd.txt`
(`UPDATE_GOLDEN=1` regenerates); dishonesty fixture
`desktop/test/fixtures/features-dishonest.json` (an `available:false` row with
a multi-clause skip_reason; a capture row `trace_insns:16, insns_truth:242,
complete:false`) → the render contains the skip_reason **byte-for-byte** and
`"16/242 TRUNCATED"` (D7); unit asserts on the cell rule's four branches.

**Docs.** One paragraph in `desktop/README.md`;
[08-observer-views.md](08-observer-views.md) links here for its capability
chrome rather than re-deriving the cell rule.

**Done when.**
- `make desktop-test` runs `test_completeness_view` green headlessly.
- `make desktop` opens the panel showing committed-or-loaded rows (manual).
- Editing a fixture's skip_reason by one character fails the golden test.

## Task order & parallelism

- **T1 → {T2, T3} → T4**: the exporter chain; T2 and T3 touch disjoint parts
  of the TU and can run in parallel after T1. All four are startable **today**
  — the only 01 coupling is T1's field-name contract (reconcile whichever
  lands second; T4 step 5 is the standing canary).
- **T5 → T6** gates on 03's skeleton; T5 works against committed box files
  the moment 03's build exists. The two chains are independent — two people
  can work them concurrently.

## Constraints & gates

- **Honesty rules are hard requirements**: survey events never become stacks
  (exit 2 + reason); truncation/loss always surfaces (profile-name suffix,
  `otherData`, TRUNCATED cells, DOT trailer); `skip_reason` verbatim; the
  ordinal axis is never labelled as time. Each has a test in T4/T6 (D7).
- **Dependency-free means it**: `tools/asmtrace_export.c` links libc only —
  no engine objects, no Capstone (D10's optional `disasm` strings are why),
  no JSON library. The desktop readers use only the pinned nlohmann/json
  (D2) and stay linkable into `desktop-render` (D4).
- **No self-skips**: every test here runs headless with cc + python3
  (exporter) or the 03 toolchain — per [CLAUDE.md](../../../CLAUDE.md) there
  is no hardware or credential gate, so no lane may skip.
- **Schema reconciliation** (**settled 2026-07-24**): 01 landed first, so its
  shipped schema is binding and this doc was corrected to match — `"k"` not
  `"ev"`, kind `call` not `tree`, integer `addr` not a hex string, basis
  `"rel"`/`"abs"` not `"region"`, truncation on the `end` footer with a missing
  footer meaning TORN. Any future 01 rename lands with the matching fixture +
  this-doc update in one change; until the Phase-3 freeze expect additive change
  only (unknown-kind/field tolerance is mandatory).
- **`mk/desktop.mk` is shared property** (D3): additive rules only; whichever
  of 02/03 lands first creates it + the include after
  [Makefile:929](../../../Makefile#L929); both add help echo lines at
  [Makefile:91](../../../Makefile#L91). The repo
  [.clang-format](../../../.clang-format) applies to C and C++ alike (D8).
- **Formats pinned by goldens**: speedscope and Chrome Trace are stable
  public formats; the T4 goldens pin our output byte-for-byte, so any format
  change is a reviewable diff.

## Out of scope

- **`emu_trace_lcov_source` offline regeneration** — needs `emu_line_map_t`
  offset→line rows ([include/asmtest_trace.h:138–143](../../../include/asmtest_trace.h#L138)),
  which no Phase 1 event carries; if 01 adds a srcmap kind, `--lcov-source`
  is a small follow-up. Block-offset lcov (T3) already serves genhtml.
- **`--graph` / `--procs` DOT offline regeneration** — the live emitters
  ([cli/asmspy.c:1048](../../../cli/asmspy.c#L1048),
  [:1325](../../../cli/asmspy.c#L1325)) keep working at capture time, but
  their snapshot event kinds have **no producer until the record modes land**
  (Phase 3, [07-serve-live-host.md](07-serve-live-host.md)); an offline
  emitter today would be untestable against real recordings — exactly the
  lane CLAUDE.md forbids. Revisit when 07 lands; T3 is the template.
- **SARIF export** — plan schedules it "later alongside the Wave 4 items".
- **Compressed/framed container reading** — reserved in plan D1 for PT-scale
  recordings (Phase 3); the exporter refuses it honestly (T1 step 2).
- **Diff/compare in the completeness panel** — the diff primitive is plan D3,
  owned by [04-replay-views.md](04-replay-views.md); `scan_boxes` already
  returns every box, so a compare column can come later without reader changes.
- **Writing `.asmtrace`** — serializers are 01's; this doc only reads.
- **A speedscope/Perfetto importer or live streaming export** — the exporter
  is batch; live is `--serve` ([07-serve-live-host.md](07-serve-live-host.md)).
