# Desktop GUI 06 — doors & learning (Learn/Author doors, capability panel, failure deep links) — implementation

> **Sources.** Actioned from
> [desktop-gui-plan.md](../plans/desktop-gui-plan.md) — "UX: low barrier to
> entry…" (the three doors), design change 3 (failures as the navigation
> spine), design D2 (capability discovery), and the engine work items "Ship
> `ct_eq` as a real example", "Runner record mode", "Walkthrough content for
> the Learn ladder". Written 2026-07-23. If this doc and a source disagree,
> this doc wins (sources may be stale); if the CODE and this doc disagree,
> re-verify before implementing. Shared decisions D1–D11 live in this
> directory's README; repo-wide rules:
> [\_conventions.md](../implementations/_conventions.md). Siblings:
> [01-asmtrace-format.md](01-asmtrace-format.md) (schema, walkthrough event
> kind), [02-exporters-and-readers.md](02-exporters-and-readers.md) (C
> writer/reader), [03-desktop-shell.md](03-desktop-shell.md) (shell, door
> frame, `mk/desktop.mk`, `Dockerfile.desktop`),
> [04-replay-views.md](04-replay-views.md) (replay views + deep-link router).

> **Implemented 2026-07-24 (7/7). Five claims lost to the code and were fixed in
> place:**
> 1. **T1's data page** at `0x00200000` is the emulator's own STACK base
>    (`src/emu.c:15`), so `emu_map` refuses it. The suite maps `0x00300000`.
> 2. **T2's stop fields.** 01's `note` kind carries only `text`/`off`/`step`/
>    `stop` — there was no `title`/`expected`/`got`. Three optional fields were
>    appended to that kind in [asmtrace-schema.md](asmtrace-schema.md) (owned
>    here, omitted when absent, ignored by any reader that predates them).
> 3. **T3's assembly** cannot use GNU as's numeric local labels (`jge 2f` /
>    `jmp 1b`): the pinned Keystone rejects them outright
>    ("KS_ERR_ASM_LABEL_INVALID"). The generator uses named labels — the same
>    jumps to the same places, a different label spelling — and says so.
> 4. **T4/T5/T6's test files** are `.cpp`, not `.c`: the view-models they drive
>    are C++17 headers.
> 5. **T6 vs D9.** The capability cascade's objects include `ptrace_backend.o`,
>    which D9's "the desktop app never links the ptrace engines" appears to
>    forbid. The distinction taken is capture vs QUERY — nothing in the app
>    calls a capture entry point, and a panel that "never re-probes" must be
>    able to ask the availability cascade. Recorded here rather than glossed;
>    the render-only viewer links none of it.
>
> One measured note worth keeping: the loud-drop guarantee catches statements
> the assembler **skips**, not text it accepts and reads differently than the
> author meant. `mov rax, [` assembles cleanly under the pinned Keystone (3
> statements, 14 bytes) — a fixture that conflated the two would be testing a
> promise nothing makes.

## Why this work exists

The GUI's first-run promise is "no blank IDE". The **Learn door** plays
corpus-seeded golden walkthroughs — the ladder the docs already define
(quickstart square → `demo-fail` → `ct_eq`) — no deps, no root, any OS. The
**Author door** is the pedagogy: text → `asmtest_assemble` (loud-drop-safe
since `ba5bd46`) → emulator run → faults rendered as data, never crashes. The
**capability panel** renders what this host can do and *why not*, verbatim
from the library's status APIs — the GUI never re-probes. The **runner record
mode** makes every test failure a deep link into a recording (failure →
recording → step), so the Wave-2 blame producer later lands as a producer,
not a UI redesign. Two prerequisites are code, not GUI: `ct_eq` exists only
as illustrative doc text (verified: no `ct_eq` symbol in `examples/`, `src/`,
or `include/` at HEAD), and the runner has no record mode. Both land here.

## What already exists (verified 2026-07-23)

- The ladder's source stories:
  [quickstart.md:22](../../getting-started/quickstart.md#L22) (the square
  routine; auto-discovery [:58](../../getting-started/quickstart.md#L58);
  "see a failure" + `make demo-fail`
  [:77](../../getting-started/quickstart.md#L77));
  [examples/test_failure_demo.c:11](../../../examples/test_failure_demo.c#L11)
  (`imax_wrong`, "returns the minimum (a bug)"; built by `demo-fail`,
  [Makefile:814](../../../Makefile#L814); classroom pitch
  [classroom.md:153](../../guides/classroom.md#L153));
  [examples.md:440](../../getting-started/examples.md#L440) (the `ct_eq`
  section, explicitly illustrative — the union-coverage CT sketch
  [:452](../../getting-started/examples.md#L452) asserts the block set never
  grows [:465](../../getting-started/examples.md#L465)).
- Suite discovery: `test_*.c` + same-named `.s` link via the pattern rule
  [Makefile:332](../../../Makefile#L332); `SUITE_EXCLUDES`
  [Makefile:60](../../../Makefile#L60) keeps tier suites out of `make test`.
  Emu precedent: `test_emu` link rule
  [Makefile:882](../../../Makefile#L882) (adds `emu.o trace.o disasm.o
  fuzz.o`, links `$(UNICORN_LIBS) $(CAPSTONE_LIBS)`), `emu-test`
  [Makefile:889](../../../Makefile#L889), `docker-emu`
  [mk/docker.mk:72](../../../mk/docker.mk#L72); help echoes
  [Makefile:91](../../../Makefile#L91) / [:112](../../../Makefile#L112).
- Emulator: `emu_call_traced`
  [include/asmtest_emu.h:178](../../../include/asmtest_emu.h#L178) —
  re-running with the same trace struct **accumulates**
  ([:172](../../../include/asmtest_emu.h#L172)), the CT union mechanism;
  `emu_map`/`emu_write` [:101](../../../include/asmtest_emu.h#L101);
  `emu_fault_kind_t` (READ/WRITE/FETCH) + `emu_result_t{ok, uc_err, faulted,
  fault_addr, fault_kind, regs}` [:77](../../../include/asmtest_emu.h#L77);
  `emu_disas_available`/`emu_disas`
  [:471](../../../include/asmtest_emu.h#L471); host-arch gate
  `REQUIRE_X86_HOST` [examples/test_emu.c:34](../../../examples/test_emu.c#L34).
- Assembler: `asmtest_assemble(arch, syntax, source, addr, out)`
  [include/asmtest_assemble.h:92](../../../include/asmtest_assemble.h#L92);
  `asm_result_t{bytes, len, stat_count, ok, err[160]}`
  [:62](../../../include/asmtest_assemble.h#L62). **Loud-drop guarantee**:
  `count_statements` is a never-over-counting lower bound
  ([src/assemble.c:107](../../../src/assemble.c#L107)); any shortfall fails
  the whole assemble with "assembler skipped N of M statements…"
  ([src/assemble.c:237](../../../src/assemble.c#L237)).
- Capability APIs: `asmtest_trace_resolve` / `asmtest_trace_auto`
  [include/asmtest_trace_auto.h:166](../../../include/asmtest_trace_auto.h#L166)
  / [:175](../../../include/asmtest_trace_auto.h#L175);
  `ASMTEST_TRACE_NATIVE_ONLY`
  [:141](../../../include/asmtest_trace_auto.h#L141); choice struct {tier,
  backend, fidelity, mechanism}
  [:106](../../../include/asmtest_trace_auto.h#L106);
  `asmtest_hwtrace_status_t{available, code, stage, perf_event_paranoid,
  probe_errno, reason[160]}`
  [include/asmtest_hwtrace.h:182](../../../include/asmtest_hwtrace.h#L182)
  via `asmtest_hwtrace_status`
  [:203](../../../include/asmtest_hwtrace.h#L203); `asmtest_ibs_available`
  [include/asmtest_ibs.h:117](../../../include/asmtest_ibs.h#L117), substrate
  `asmtest_ibs_skip_reason` [:127](../../../include/asmtest_ibs.h#L127) vs
  capture `asmtest_ibs_unavail_reason`
  [:137](../../../include/asmtest_ibs.h#L137) — different questions.
- Runner report path (T7's spike re-verifies this map): `asmtest_fail` sets
  `asmtest_msg`/`asmtest_loc_*` and longjmps
  ([src/asmtest.c:119](../../../src/asmtest.c#L119)); crash/timeout set the
  same globals ([:1069](../../../src/asmtest.c#L1069)); `run_one`
  ([:1138](../../../src/asmtest.c#L1138)) → `wire_result_t` over the fork
  pipe ([:1285](../../../src/asmtest.c#L1285)) via `capture_wire`
  ([:1293](../../../src/asmtest.c#L1293)) / `apply_wire` into `test_result_t`
  ([:1274](../../../src/asmtest.c#L1274)); TAP failure YAML (`at:` + `msg:`)
  in `print_tap_result` ([:1734](../../../src/asmtest.c#L1734), `at:` at
  [:1745](../../../src/asmtest.c#L1745)); JUnit `<failure>`/`<error>`
  emission in `render_junit` ([:1980](../../../src/asmtest.c#L1980)); CLI
  parsing near `--format=` ([:2321](../../../src/asmtest.c#L2321)), env
  precedence idiom ([:2359](../../../src/asmtest.c#L2359)).
- Self-tests: [tests/expect.sh](../../../tests/expect.sh) black-boxes the
  runner; negative cases are one-line failing TESTs
  ([tests/negative.c:72](../../../tests/negative.c#L72)).
- Pure view-model test pattern to mirror:
  [cli/asmspy_logview.h](../../../cli/asmspy_logview.h) under
  [cli/test_view.c](../../../cli/test_view.c) (plain C binary, no display).
- **Nothing GUI exists yet**: no `desktop/`, `mk/desktop.mk`,
  `tests/golden-asmtrace/`, `--record-dir`, or `ct_eq` symbol. Anything below
  not cited above is new — introduced here or by the named sibling.

Baseline proof: `make docker-emu` and `make check` both green.

## Tasks

### T1 — Ship `ct_eq` as a real example suite  (S, depends on: none)

**Goal.** Turn the illustrative `ct_eq` docs section into a committed,
lane-tested suite — `examples/ct_eq.s` + `examples/test_ct_eq.c` — with
block-coverage **union across secret-differing inputs** as the assertion. The
plan's named prerequisite for the `ct_eq` walkthrough (T3).

**Steps.**
1. Create `examples/ct_eq.s` via the `ASM_FUNC` shim (x86-64 + AArch64
   bodies, like [quickstart.md:22](../../getting-started/quickstart.md#L22)):
   `ct_eq` — XOR-accumulate loop over the public length, branchless collapse
   (`cmp $1,%eax; sbb %eax,%eax; neg %eax` on x86; `cset x0, eq` on a64) —
   and `leaky_eq`, the naive early-exit compare (the negative control).
2. Create `examples/test_ct_eq.c` (skeleton below). Native correctness tests
   run everywhere; the CT-proof tests drive host-compiled routine bytes
   through the emulator, so gate with the `REQUIRE_X86_HOST` idiom copied
   from [examples/test_emu.c:34](../../../examples/test_emu.c#L34).
3. Makefile wiring (the suite links the emu tier; the
   [Makefile:332](../../../Makefile#L332) pattern rule cannot serve it): add
   `test_ct_eq` to `SUITE_EXCLUDES` ([Makefile:60](../../../Makefile#L60));
   explicit link rule + `ct-eq-test` run target beside `emu-test`
   ([Makefile:882](../../../Makefile#L882)); extend `docker-emu`
   ([mk/docker.mk:72](../../../mk/docker.mk#L72)) to
   `$(_docker_run) sh -c 'make emu-test ct-eq-test'`; help echo under
   "Optional tiers" ([Makefile:112](../../../Makefile#L112)).
4. Update [examples.md:440](../../getting-started/examples.md#L440): replace
   "(illustrative …)" with the shipped-suite pointer, mirroring the
   qadd/qmul "Ships as …" lines above it.

**Code.** Three tests: `TEST(ct_eq, correct_on_native)`;
`TEST(ct_eq, no_secret_dependent_branch)` — `emu_map` a data page at
`0x00200000`, `emu_write` the two 16-byte buffers per variant (equal /
differ@0 / differ@15), run all three via `emu_call_traced(E, (void *)ct_eq,
96, args, 3, 0, &r, &tr)` into the **same** `emu_trace_t` (it accumulates),
capture `baseline = tr.blocks_len` after run 1 and
`ASSERT_EQ(tr.blocks_len, baseline)` at the end (union never grew); and
`TEST(ct_eq, leaky_eq_control_grows_coverage)` — same variants through
`leaky_eq`, asserting the union **did** grow (the assertion has teeth).
Link rule: `$(BUILD)/test_ct_eq: $(FRAMEWORK_OBJS) $(BUILD)/ct_eq.o
$(BUILD)/emu.o $(BUILD)/trace.o $(BUILD)/test_ct_eq.o`, linked with
`$(UNICORN_LIBS)`.

**Tests.** `make ct-eq-test` green on x86-64; invert the union assertion once
by hand to confirm the control fails. `make docker-emu` runs both suites —
no self-skip except the honest non-x86-64-host gate.

**Docs.** The examples.md edit; one `### Added` CHANGELOG bullet.

**Done when.**
- `make ct-eq-test` prints all `ct_eq.*` tests `ok` on x86-64.
- `make docker-emu` runs `emu-test` **and** `ct-eq-test` green.
- The examples.md ct_eq section no longer says "illustrative";
  `make docker-docs` stays `-W`-clean.

### T2 — Walkthrough generator + square & demo-fail goldens  (M, depends on: siblings 01, 02)

**Goal.** A deterministic C generator emitting the Learn door's first two
walkthroughs — quickstart **square** and **demo-fail** ("why is rax wrong")
— as committed `.asmtrace` goldens carrying 01's annotation/walkthrough
events (ordered stops, step-anchored text, expected-vs-got framing).

**Steps.**
1. Create `desktop/test/gen_walkthroughs.c` (C, full-build tool: links
   `assemble.o`, `emu.o`, `trace.o`, `disasm.o`, 02's writer objects). Per
   walkthrough: assemble the embedded AT&T source via
   `asmtest_assemble(ASM_X86_64, ASM_SYNTAX_ATT, src, EMU_CODE_BASE, &res)`
   (loud-drop guarantees the recording is of exactly the source), run with
   `emu_call_traced`, attach a `disasm` string per step via `emu_disas`
   where available (D10), write trace + register-state + walkthrough events
   through 02's writer. Stories are static stop tables (below); a stop is
   semantically `{ordinal, step_anchor (-1 = unanchored), title, body,
   expected, got}` — exact field names per 01's walkthrough kind.
2. Add target `asmtrace-walkthroughs` to `mk/desktop.mk` (03's file; if
   implementing first, create a minimal `mk/desktop.mk` and `include` it
   after [Makefile:929](../../../Makefile#L929) per D3): build the
   generator, write to `tests/golden-asmtrace/walkthroughs/`, run a second
   time to a temp dir and `cmp` (byte-stability gate, D6). No timestamps, no
   absolute paths, fixed event order — determinism rules per 01.
3. Commit `tests/golden-asmtrace/walkthroughs/square.asmtrace` +
   `demo-fail.asmtrace` (plain NDJSON — small recordings stay uncompressed).

**Code.** The stories (the deliverable):

- **square.asmtrace** — `movq %rdi,%rax; imulq %rdi,%rax; ret`, one run
  `square(2)`. Stops: (1) step 0 — "SysV: the argument arrives in rdi, the
  answer must end in rax"; (2) step 1 — "rax = rax * rdi"; (3) step 2 — ret;
  (4) unanchored — the quickstart §5 failure framing: expected `5` (the
  deliberately edited assertion,
  [quickstart.md:77](../../getting-started/quickstart.md#L77)), got `4`.
- **demo-fail.asmtrace** — `mov %rdi,%rax; cmp %rsi,%rdi; cmovg %rsi,%rax;
  ret` (the emulator retelling of `imax_wrong`,
  [test_failure_demo.c:11](../../../examples/test_failure_demo.c#L11)), one
  run `imax_wrong(3, 4)`. Stops: step 0 — rax starts as a; step 1 — the
  compare sets the flags; step 2 — **expected `rax=4`, got `3`**: "the
  predicate is inverted — cmovg selects the smaller value"; step 3 — "this
  is the failure `make demo-fail` reports; in the GUI a failing test
  deep-links to exactly this step" (the spine, T7).

**Tests.** The double-run `cmp`; regeneration inside the docker-desktop lane
leaves `git diff --exit-code` clean (pinned Keystone makes assembly
reproducible — golden regeneration is defined **in-lane**).

**Docs.** `desktop/README.md` "Walkthrough authoring" (stop table →
recording; where goldens live; the in-lane regeneration rule).

**Done when.**
- `make asmtrace-walkthroughs` writes both recordings; its `cmp` passes.
- Both files round-trip byte-stable through 02's reader/writer.
- Regenerated inside `make docker-desktop`: golden dir diff-clean.

### T3 — `ct_eq` walkthrough recording  (S, depends on: T1, T2)

**Goal.** The ladder's capstone: `ct_eq.asmtrace` — the constant-time story
over real runs: three secret-differing inputs, a coverage union that does not
grow, and the leaky control that does.

**Steps.**
1. Extend `gen_walkthroughs.c`: assemble the same AT&T source as
   `examples/ct_eq.s` (keep the two textually identical; cross-comment), run
   equal / differ-at-first / differ-at-last into one accumulating trace
   ([asmtest_emu.h:172](../../../include/asmtest_emu.h#L172)), then the same
   three via `leaky_eq`. Trace events carry 01's invocation numbering.
2. Stops: (1) the XOR-accumulate idiom; (2) the loop branch — "depends on the
   *public* length only"; (3) the branchless collapse; (4) on the coverage
   event — "block set after run 1 = N; after runs 2 and 3 **still N**: no
   branch depended on the secret"; (5) the control — expected "no new
   blocks", got "+1 block at the early-exit offset": a CT violation.
3. Commit `tests/golden-asmtrace/walkthroughs/ct_eq.asmtrace`; T2's
   byte-stability gate covers it automatically.

**Tests / Docs / Done when.** T2's gates over three files; the Learn door
(T4) lists all three; `desktop/README.md` ladder table gains the row.

### T4 — Learn door: walkthrough player  (M, depends on: T2, T3; siblings 01, 02, 03, 04)

**Goal.** The first door: bundled-walkthrough list; the player renders
ordered stops with step-anchored text beside the replay views,
expected-vs-got framed, honesty chrome enforced (D7).

**Steps.**
1. `desktop/src/walkthrough.h` — **pure view-model header** (C++17, no
   ImGui), mirroring [cli/asmspy_logview.h](../../../cli/asmspy_logview.h) /
   [cli/test_view.c](../../../cli/test_view.c): consumes events 02's reader
   parsed, exposes stops + navigation.
2. `desktop/src/learn_door.cpp` — ImGui panel registered with 03's door
   frame: card list (title + stop count + provenance chip; recordings found
   in the walkthroughs dir bundled beside the binary, overridable via
   `ASMTRACE_LEARN_DIR`); player pane: stop text, prev/next, expected-vs-got
   side-by-side; an anchored stop navigates the replay views through 04's
   router.
3. Honesty chrome (D7, tested): add dishonesty fixture
   `tests/golden-asmtrace/walkthroughs/square-truncated.asmtrace` (generated:
   the square recording with the truncation flag set and one stop anchored
   past the recorded window). The player must render the truncation banner
   and "stop is beyond the recorded window" — never silently clamp.
4. Headless tests `desktop/test/test_walkthrough.c` (includes the header
   directly; no display/GL): stop ordering, anchor resolution, expected/got
   extraction, truncated-fixture verdicts. Wire into `desktop-test` (03).

**Code.** `walkthrough.h` (new): `struct wt_stop { int ordinal; long
step_anchor /* -1 = unanchored */; std::string title, body, expected, got; }`
and `struct wt_model { std::vector<wt_stop> stops; int cur; bool truncated;
bool anchor_in_window(int) const; }` — built from 02's parsed events (exact
loader signature per 02); `anchor_in_window` returning false drives the
"beyond recorded window" verdict.

**Tests.** `make desktop-test` runs `test_walkthrough` green with the three
walkthroughs + truncated fixture as inputs (no network, no display).

**Docs.** `desktop/README.md` "Learn door": ladder, fixture policy,
`ASMTRACE_LEARN_DIR`.

**Done when.**
- `make desktop-test` passes headless (verify inside `make docker-desktop`).
- Manual: the door lists all three; opening square and clicking stop 2 moves
  the replay views to step 1.
- The truncated fixture's banner + per-stop refusal are asserted by
  `test_walkthrough`, not just eyeballed.

### T5 — Author door: assembly box → run → record → replay  (M, depends on: siblings 02, 03, 04)

**Goal.** The second door, **full build only** (D4/D9): type assembly, pick
arch + dialect, assemble loud-drop-safe, run on the emulator, see faults as
fault cards, land in the replay views on a recording of what just ran.

**Steps.**
1. `desktop/src/author_door.cpp`, compiled only into the `desktop` binary
   (full-build define from 03's `mk/desktop.mk`; `desktop-render` shows a
   static tile: "Author mode requires the full (GPL-2.0) build"). Widgets:
   multiline source box, arch selector (the four
   [asm_arch_t](../../../include/asmtest_assemble.h#L33) values), dialect
   selector (the five [asm_syntax_t](../../../include/asmtest_assemble.h#L51)
   values, default Intel per the header), integer-args row (0–6), Run.
2. Assemble via [asmtest_assemble](../../../include/asmtest_assemble.h#L92)
   at `EMU_CODE_BASE`. On failure render `asm_result_t.err` **verbatim** in
   an error strip — the loud-drop message
   ([src/assemble.c:239](../../../src/assemble.c#L239)) already names the
   common trap (AT&T text under the Intel default). Never clear the source.
3. Run: v1 executes the **x86-64 guest only** via
   [emu_call_traced](../../../include/asmtest_emu.h#L178); other arches
   assemble and show bytes + a labeled "run/trace is x86-64-only in v1" (the
   plan's honest limit). Cap `max_insns` (default 1<<20) so a spin cannot
   hang the UI; run on a worker thread, marshal the result back.
4. Fault cards, not errors: when `emu_result_t.faulted`, render fault kind
   (READ/WRITE/FETCH, [emu_fault_kind_t](../../../include/asmtest_emu.h#L77)),
   `fault_addr`, the faulting step via
   [emu_disas](../../../include/asmtest_emu.h#L479) when available (degrade
   to offsets, D10), and the register file. Card copy: "the emulator turned
   this into data; on real hardware this would have been a crash".
5. Record every run through 02's writer into the session recordings dir
   (provenance: producer=emulator, fidelity=virtual, isolated-guest badge
   per 01), then open it via 04's router — the door's exit is always a
   recording. Headless view-model test `desktop/test/test_author_vm.c` over
   a new `desktop/src/author_vm.h`: assemble-error passthrough (loud-drop
   text survives verbatim), fault-card field mapping from a synthetic
   `emu_result_t`, the arch-gating table.

**Tests.** `desktop-test` runs `test_author_vm` (Keystone + Unicorn are in
`Dockerfile.desktop` per 03/D2 — never self-skips in-lane).

**Docs.** `desktop/README.md` "Author door": the GPL boundary, the fidelity
label, the fault-card contract.

**Done when.**
- `make desktop-test` green incl. `test_author_vm`, in `make docker-desktop`.
- Manual: the square routine (AT&T) under dialect=Intel produces the loud
  diagnostic, not a silent wrong trace; under AT&T it assembles, runs, and
  lands in replay views on a fresh recording; `movq (%rax), %rbx` with rax=0
  renders a READ fault card at 0.
- `make desktop-render` builds with zero engine deps and shows the static
  tile (`ldd build/desktop-render` lists no unicorn/keystone/capstone).

### T6 — Capability panel  (M, depends on: sibling 03)

**Goal.** One panel rendering what this host can do and why not — straight
from the library's status APIs ("the GUI never re-probes"), with the
native→virtual fidelity line as an explicit, labeled choice.

**Steps.**
1. `desktop/src/capview.h` (pure view-model) +
   `desktop/src/capability_panel.cpp` (ImGui, full build; in desktop-render
   the panel shows the **loaded recording's** provenance instead, and says so
   — a render-only viewer probes nothing).
2. Data collection, once at open (+ explicit Refresh): the cascade from
   `asmtest_trace_resolve(policy, out, cap)`
   ([asmtest_trace_auto.h:166](../../../include/asmtest_trace_auto.h#L166)),
   each row showing tier / backend / fidelity / mechanism; per hardware
   backend (the four
   [asmtest_trace_backend_t](../../../include/asmtest_hwtrace.h#L72) values)
   `asmtest_hwtrace_status()` — stage name
   ([ASMTEST_HW_STAGE_*](../../../include/asmtest_hwtrace.h#L174)),
   EPERM-vs-EUNAVAIL from `code`, `perf_event_paranoid`, and `reason[160]`
   **verbatim** (UI law: a greyed row always shows its machine reason); IBS
   row: `asmtest_ibs_available()` + **both** reasons, labeled "substrate:"
   ([skip_reason](../../../include/asmtest_ibs.h#L127)) vs "last capture:"
   ([unavail_reason](../../../include/asmtest_ibs.h#L137)) — they answer
   different questions; never collapse them.
3. The fidelity line: a rule labeled "native → virtual fidelity line"
   separates real-CPU rows from the emulator row (isolated-guest badge). A
   `[ ] native only` toggle re-resolves under
   [ASMTEST_TRACE_NATIVE_ONLY](../../../include/asmtest_trace_auto.h#L141);
   when the cascade empties, render "no native tier on this host — the
   library returns EUNAVAIL rather than silently downgrading; untick to
   allow the virtual floor (explicit choice)". Never auto-fallback.
4. Headless tests `desktop/test/test_capview.c`: feed synthetic status /
   choice arrays into the view-model (no hardware dependency); assert an
   EPERM row contains stage name, paranoid level, and the verbatim reason;
   the NATIVE_ONLY-empty case yields the refusal line; STATISTICAL rows
   carry the "sampled, never exact" chip.

**Code.** `capview.h` (new): `struct cap_row { std::string label, reason;
int stage, code, fidelity; bool available; }` and
`std::vector<cap_row> capview_build(const asmtest_trace_choice_t *cascade,
size_t n, const asmtest_hwtrace_status_t st[4], int ibs_avail,
const char *ibs_substrate, const char *ibs_capture);` — pure, testable
without probing hardware.

**Tests.** `make desktop-test` runs `test_capview` on synthetic data —
deterministic on every host including CI containers.

**Docs.** `desktop/README.md` "Capability panel": the two UI laws and where
each string comes from.

**Done when.**
- `make desktop-test` green incl. `test_capview` (in `make docker-desktop`).
- Manual on a Linux dev box: single-step available; PT/LBR greyed with real
  reasons; IBS with both labeled reasons; native-only toggling never silently
  falls back to the emulator.

### T7 — Runner record mode: `--record-dir` + failure deep links  (M, depends on: T1; siblings 02, 04)

**Goal.** The engine-side producer for design change 3: `--record-dir=DIR`
arms per-test `.asmtrace` recording; a failing test's TAP/JUnit output
carries recording path + failing step id; the GUI's failure list deep-links
via 04's router. **Scoped to emulator-tier tests first**; everywhere else the
flag is accepted and simply produces no recordings (the honest degrade).
Subtlest task: **do the spike before any code.**

**Steps.**
1. **Spike (no code):** re-verify the report-path map from "What already
   exists" against HEAD, confirming: (a) `wire_result_t` crosses only a
   same-binary fork pipe (safe to extend); (b) crash/timeout failures flow
   through the same globals ([src/asmtest.c:1069](../../../src/asmtest.c#L1069)),
   so a noted recording survives a crash; (c) the parallel pool fills
   `results[]` in registration order. Record the map (with any drifted
   lines) in the PR description before proceeding.
2. Runner plumbing (`src/asmtest.c` + `include/asmtest.h`): `options_t` +=
   `const char *record_dir;`, parsed beside `--format=`
   ([:2321](../../../src/asmtest.c#L2321)), env fallback
   `ASMTEST_RECORD_DIR` with the `ASMTEST_TIMEOUT` precedence idiom
   ([:2359](../../../src/asmtest.c#L2359)); create the directory at arm time
   (the file's existing `_WIN32` split), **hard-fail exit 2 on failure** —
   never silently record nothing. New public API (declare near
   `asmtest_fail`, [include/asmtest.h:656](../../../include/asmtest.h#L656)):

   ```c
   /* Non-NULL iff --record-dir is armed and a test is running:
    * "<dir>/<suite>.<name>.asmtrace" for the CURRENT test. */
   const char *asmtest_record_path(void);
   /* A producer declares "this test wrote this recording; the failing
    * step is `step` (-1 = unknown)". */
   void asmtest_note_recording(const char *path, long step);
   ```

   The runner sets current-test context and clears the note at the top of
   `run_one` ([:1138](../../../src/asmtest.c#L1138)) — the clear matters on
   `--no-fork`, where globals outlive tests. `wire_result_t` += `char
   rec_path[256]; long rec_step;`; `test_result_t` likewise; snapshot in
   `capture_wire`, copy in `apply_wire`, zero in the reaper's synthesized
   outcomes.
3. Emission: in `print_tap_result`'s failure block after the `at:` line
   ([:1745](../../../src/asmtest.c#L1745)) print `recording: <path>` and,
   when `rec_step >= 0`, `step: <n>` — additive YAML keys, existing
   expect.sh substring pins stay green. JUnit: append
   `&#10;recording: <path>&#10;step: <n>` to the `<failure>`/`<error>` text
   via the existing `xml_print_escaped`
   ([:1980](../../../src/asmtest.c#L1980)) — schema-valid, no new
   attributes. `usage()` ([:1807](../../../src/asmtest.c#L1807)) gains the
   flag line.
4. Producer glue (new `src/asmtrace_rec.c` + `include/asmtest_rec.h`):
   `int asmtest_rec_emu(const emu_trace_t *tr, const emu_result_t *res,
   const void *code, size_t code_len);` — no-op returning 0 when
   `asmtest_record_path()` is NULL; else serializes trace + result +
   provenance through 02's writer to that path and calls
   `asmtest_note_recording(path, -1)`. It links 02's writer objects, so
   **only suites that already link the emu tier add it** — `FRAMEWORK_OBJS`
   stays engine-free, which is the whole scoping mechanism. First adoption:
   `test_ct_eq` (T1) calls it after its union runs. Step id stays -1 in v1 —
   04's router opens at the last step when a failure carries none; the
   Wave-2 blame producer later supplies real ids through the same seam. GUI
   consumption (parsing, deep-linking) is 04's; this doc defines only the
   emitted format.
5. Tests, engine-free first: add to
   [tests/negative.c](../../../tests/negative.c#L72) a case calling
   `asmtest_note_recording("fake.asmtrace", 7)` then failing; extend
   [tests/expect.sh](../../../tests/expect.sh) to assert the TAP block
   contains `recording: fake.asmtrace` and `step: 7`, and the JUnit output
   both lines. End-to-end: `ct-eq-test` gains a leg running
   `./build/test_ct_eq --record-dir=build/rec --filter='ct_eq.*'` and
   asserting `build/rec/ct_eq.no_secret_dependent_branch.asmtrace` exists
   and round-trips through 02's reader.

**Tests.** `make check` (expect.sh additions) + the `ct-eq-test` leg, inside
existing lanes (`docker-test`, `docker-emu`). No self-skips: the plumbing
test needs no engine at all.

**Docs.** `docs/guides/runner.md` gains `--record-dir` (`-W`-clean); one
`### Added` CHANGELOG bullet.

**Done when.**
- The spike map is written down and matches HEAD (or documents drift).
- `make check` green including the new recording/step TAP+JUnit assertions.
- The `ct-eq-test --record-dir` leg produces a real `.asmtrace` that 02's
  reader round-trips; a locally forced ct_eq failure prints `recording:` in
  its TAP YAML block.
- A suite with no glue (e.g. `test_arith`) accepts `--record-dir`, produces
  no recordings, and emits no `recording:` key — verified by hand.

## Task order & parallelism

- **T1** first — independent, unblocks T3 and T7's adoption suite. **T2**
  needs 01's walkthrough kind + 02's writer (start against 02's draft, pin
  field names when 01 settles). **T3** follows T1 + T2; **T4** follows
  T2/T3 + 03/04.
- **T5** and **T6** are independent of each other and of T4 once 03's shell
  exists — three people can work T4/T5/T6 concurrently. **T7** is
  independent of T2–T6; only its GUI consumption waits on 04. Spike first.

Critical path: **T1 → T2 → T3 → T4**; off it: T5, T6, T7.

## Constraints & gates

- **Licensing (D4/D9).** T4 ships in both binaries (recordings only); T5's
  door and T6's probing exist **only** in the full GPL-2.0 `desktop` binary;
  `desktop-render` keeps zero engine deps (T5's `ldd` check is the gate).
  The Author door never links a ptrace engine.
- **No self-skipping lanes.** Everything here is installable (Unicorn,
  Keystone, ImGui, GLFW) and runs inside `docker-emu` / `docker-desktop` per
  the CLAUDE.md rule. Honest skips only: `REQUIRE_X86_HOST` on tests driving
  host-compiled bytes (host-ISA gate) and T6's hardware rows rendering
  measured reasons.
- **Determinism (D6).** Goldens regenerate byte-identically **in-lane**
  (pinned Keystone); the double-run `cmp` is the gate. Never hand-edit a
  committed `.asmtrace`.
- **Honesty is tested behavior (D7).** The truncated-walkthrough fixture and
  player assertions (T4), verbatim-reason assertions (T6), and loud-drop
  passthrough assertion (T5) are requirements, not polish.
- **Cross-doc naming.** `asmtest_record_path`, `asmtest_note_recording`,
  `asmtest_rec_emu`, `wt_model`, `capview_build`, `ct-eq-test`,
  `asmtrace-walkthroughs`, and the `recording:`/`step:` TAP keys are **new,
  owned by this doc**. Writer/reader entry points are 02's; router +
  failure-list parsing are 04's; `mk/desktop.mk`/`Dockerfile.desktop`/door
  frame are 03's; walkthrough kind + determinism rules are 01's.
- **Formatting.** Repo `.clang-format` applies to `desktop/` C++ (D8); run
  `make docker-fmt` before review.

## Out of scope

- **Replay views and the router** ([04](04-replay-views.md)) — called, not
  built here; the Loom day-one rung is [05](05-loom-day-one.md); the
  **Inspect door / attach picker** is Phase 3 ([08](08-observer-views.md)).
- **Blame-the-instruction step ids** — Wave 2; T7 reserves the seam
  (`asmtest_note_recording`'s `step`) and stops.
- **Record mode beyond the emulator tier** (native/hwtrace/ptrace suites) —
  later producers behind the same runner seam; the demo-fail *story* ships
  as T2's golden instead.
- **Non-x86-64 guest runs in the Author door** — assemble-only in v1
  (valtrace/replay is x86-64-guest-only per the plan).
- **ct_eq mem-address CT verdicts** (Wave 1 `mem[]` stream) — the
  block-union assertion is the shipped v1 oracle, exactly as documented.
