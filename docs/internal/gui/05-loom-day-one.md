# The Loom, day-one rung (desktop GUI Phase 2 flagship) — implementation

> **Sources.** Actioned from [desktop-gui-plan.md](../plans/desktop-gui-plan.md) — section
> **"Flagship interaction concept: the Loom"** (Pitch, Interaction model, **Day-one rung**,
> Honest limits) and **"Phasing" Phase 2**, under constraints D5/D7. Written 2026-07-23. If this
> doc and a source disagree, this doc wins (sources may be stale); if the CODE and this doc
> disagree, re-verify before implementing.
>
> Siblings (D11): [03-desktop-shell.md](03-desktop-shell.md) owns `desktop/` scaffolding,
> `mk/desktop.mk` (`desktop`, `desktop-render`, `desktop-test`, `docker-desktop`), the ImGui
> shell, the **Workspace** document model and the headless null-backend test harness;
> [04-replay-views.md](04-replay-views.md) owns the client-side **slice-closure** and
> **first-divergence** helpers; [02-exporters-and-readers.md](02-exporters-and-readers.md) owns
> the `.asmtrace` reader and `make asmtrace-golden`;
> [01-asmtrace-format.md](01-asmtrace-format.md) owns event kinds + provenance. Symbols from
> those docs are marked **(new — 0N)**; everything else cited exists at HEAD `a460d40`.

## Why this work exists

The Loom is the plan's flagship: a recorded run as a spacetime fabric — horizontal = trace step,
vertical = location lanes — where every value is a worldline you can select, walk, audit, and
fork. The **day-one rung** needs no new engine work: the emulator L0 producer fills
`asmtest_valtrace_t` with per-step operand values, the pure L1 pass yields last-writer def-use
edges, and the assemble/emulator/snapshot APIs make one-fact forks deterministic. This doc turns
those structs into the fabric model, renderer + honesty chrome, selection-as-lineage, the lane
annex, the fork mechanic, and the committed golden looms that pin it all.

## What already exists (verified 2026-07-23)

- [src/dataflow_emu.c](../../../src/dataflow_emu.c) — the emulator L0 producer. Entry point
  `asmtest_dataflow_emu_run(code, code_len, args, nargs, max_insns, vt)`
  ([:249](../../../src/dataflow_emu.c#L249)); it opens its **own** `uc_engine` per call
  ([:257](../../../src/dataflow_emu.c#L257)) and zeroes the guest deterministically (`df_zero_gp`,
  [:229–240](../../../src/dataflow_emu.c#L229): rax rbx rcx rdx rsi rdi rbp r8–r15, EFLAGS = 2) —
  **every call is hermetic**. SysV argument table `arg_regs` = rdi rsi rdx rcx r8 r9
  ([:273–276](../../../src/dataflow_emu.c#L273)). Sub-registers fold to their 64-bit container
  (`cap_x86_to_uc`, [:64–137](../../../src/dataflow_emu.c#L64)); EFLAGS is an ordinary modeled
  register ([:132–133](../../../src/dataflow_emu.c#L132)), so flag writes are `AT_LOC_REG`
  records. Register **write** values are deferred: `value_valid = false` pre-instruction
  ([:203](../../../src/dataflow_emu.c#L203)), filled at the next hook / emu-stop (`df_finalize`,
  [:152–166](../../../src/dataflow_emu.c#L152)); a memory value wider than 8 bytes stays invalid
  ([:222–226](../../../src/dataflow_emu.c#L222)) — the two **hollow thread** cases.
  `mem_space = AT_LOC_MEM_ABS` ([:254](../../../src/dataflow_emu.c#L254)).
- [include/asmtest_valtrace.h](../../../include/asmtest_valtrace.h) — pure header (stdlib +
  asmtest_trace.h only, [:29–33](../../../include/asmtest_valtrace.h#L29) — **safe in the
  render-only build**). `at_val_rec_t` ([:61–86](../../../include/asmtest_valtrace.h#L61));
  `asmtest_valtrace_t` ([:93–116](../../../include/asmtest_valtrace.h#L93)) with honest-overflow
  `truncated` ([:113](../../../include/asmtest_valtrace.h#L113)) and `*_total` counters
  ([:98–99](../../../include/asmtest_valtrace.h#L98)); `asmtest_defuse_edge_t` {from_step,
  to_step, loc} ([:171–178](../../../include/asmtest_valtrace.h#L171)); `asmtest_defuse_build`
  ([:190](../../../include/asmtest_valtrace.h#L190)) keys memory **per byte**
  ([:172–174](../../../include/asmtest_valtrace.h#L172)); slices
  [:206–225](../../../include/asmtest_valtrace.h#L206).
- [include/asmtest_emu.h](../../../include/asmtest_emu.h) — `emu_result_t`
  ([:84–91](../../../include/asmtest_emu.h#L84)); `emu_fault_kind_t`
  ([:77–82](../../../include/asmtest_emu.h#L77)); `emu_snapshot`/`emu_restore`
  ([:595–608](../../../include/asmtest_emu.h#L595)), with the load-bearing note that mapped
  memory **persists across `emu_call_*`** ([:586–594](../../../include/asmtest_emu.h#L586));
  `emu_call_traced` ([:178](../../../include/asmtest_emu.h#L178)); `emu_fault_describe`
  ([:510](../../../include/asmtest_emu.h#L510)); `EMU_CODE_BASE` ([:47](../../../include/asmtest_emu.h#L47)).
- [include/asmtest_assemble.h](../../../include/asmtest_assemble.h) — `asmtest_assemble`
  ([:92](../../../include/asmtest_assemble.h#L92)) with the **all-or-nothing** contract
  ([:83–91](../../../include/asmtest_assemble.h#L83)): a silently-dropped statement fails with
  "assembler skipped N of M statements".
- [include/asmtest_taint.h](../../../include/asmtest_taint.h) — dependency-free
  ([:24–25](../../../include/asmtest_taint.h#L24)); `at_taint_hit_t` {off, ea, seed_off, tag,
  kind, depth} ([:60–70](../../../include/asmtest_taint.h#L60)).
- [cli/asmspy.h](../../../cli/asmspy.h) — `asmspy_watch_hit_t` {hit_no, tid, pc, addr, is_write,
  value_ok, value_len, value, func, module, off} ([:573–585](../../../cli/asmspy.h#L573)). The
  header pulls `asmtest_ptrace.h` ([:28](../../../cli/asmspy.h#L28)) — **never include it in
  desktop code**; mirror the fields (watch hits reach the viewer only inside recordings, per D9).
- [include/asmtest_trace.h](../../../include/asmtest_trace.h) — pure. `asmtest_trace_t`
  ([:44–60](../../../include/asmtest_trace.h#L44)); `asmtest_srcmap_entry_t`
  ([:177–183](../../../include/asmtest_trace.h#L177)); `asmtest_srcreg_resolve`
  ([:241–243](../../../include/asmtest_trace.h#L241)).
- [examples/test_dataflow_emu.c](../../../examples/test_dataflow_emu.c) — `df_chain` fixture
  bytes + listing ([:38–45](../../../examples/test_dataflow_emu.c#L38)); hand-derived edges and
  slices ([:94–116](../../../examples/test_dataflow_emu.c#L94)); the idiom of re-declaring
  `asmtest_dataflow_emu_run` locally ([:25–27](../../../examples/test_dataflow_emu.c#L25)) — it
  has no public header; `forks.cpp` does the same.
- [bindings/conformance/corpus_routines.c](../../../bindings/conformance/corpus_routines.c) —
  `asmtest_corpus_routine` ([:32](../../../bindings/conformance/corpus_routines.c#L32));
  `"sum_via_rbx"` ([:37–38](../../../bindings/conformance/corpus_routines.c#L37)), x86-64 body at
  [examples/flags.s:45–52](../../../examples/flags.s#L45): `push rbx; mov rbx,rdi; add rbx,rsi;
  mov rax,rbx; pop rbx; ret` — a callee-save spill (memory band), an EFLAGS knot, a restore.
- Build context: `include mk/cli.mk` at [Makefile:929](../../../Makefile#L929) (mk/desktop.mk
  goes right after — 03's edit); the `docker-cli` rule shape `docker-desktop` mirrors at
  [mk/cli.mk:407–411](../../../mk/cli.mk#L407); the producer's pure-tier link set at
  [mk/dataflow.mk:354–355](../../../mk/dataflow.mk#L354). **Nothing exists yet** under `desktop/`
  or `tests/golden-asmtrace/` (verified by listing).

Baseline proof: `make dataflow-test` (or `make docker-dataflow`) — `test_dataflow_emu` prints
its `ok` lines; that producer output is this doc's raw material.

## Tasks

### T1 — Fabric data model: lanes, worldlines, hops, knots  (M, depends on: 02, 03)

**Goal.** A pure, ImGui-free model in `desktop/src/loom/fabric.h` + `.cpp`: `(asmtest_valtrace_t,
asmtest_defuse_t, provenance)` → lanes, spans, hops, knots — engine-free, in **both** binaries.

**Steps.** Create the header with the structs below; implement `loom_fabric_build` in
`fabric.cpp`; add both to 03's render-source list in `mk/desktop.mk` (**new — 03**). Two feeders,
one build path: replay — 02's reader materializes a recording's dataflow-step + defuse-edge
events back into an `asmtest_valtrace_t` + `asmtest_defuse_t` (plain structs; fill the arrays
directly) (**new — 02**); live/fork — T5 passes producer output unchanged.

**Code.**

```cpp
// desktop/src/loom/fabric.h — no ImGui, no engine headers.
struct loom_provenance_t {
    std::string producer;            // "dataflow-emu" today
    bool exact = false;              // statistical input NEVER weaves
    bool truncated = false;          // vt->truncated || recording drop counts
    bool isolated_guest = false;     // emulator producer -> badge
    std::vector<std::string> disasm; // per step; empty -> offsets (D10)
};
enum class loom_lane_kind { reg, mem_band };
struct loom_lane_t {
    loom_lane_kind kind;
    uint32_t reg;                    // folded 64-bit container Capstone id
    uint64_t lo, hi;                 // [lo,hi) bytes in vt->mem_space (bands)
    std::string name;                // "rdi", "eflags", "mem[0x20fff8..0x210000)"
    uint32_t first_touch_step;       // orders mem bands
};
struct loom_span_t {                  // one worldline interval
    uint32_t lane, t_write, t_end;    // t_end: next overlapping write; UINT32_MAX = alive at end
    size_t rec;                       // defining record in vt->recs (SIZE_MAX = synthetic)
    bool value_valid, born_untraced;  // hollow thread / synthetic entry span
    uint64_t value;
};
struct loom_hop_t  { uint32_t from_span, to_span, edge; };
struct loom_knot_t { uint32_t step; };  // >=1 read AND >=1 write at step
struct loom_fabric_t {
    std::vector<loom_lane_t> lanes;  std::vector<loom_span_t> spans;
    std::vector<loom_hop_t>  hops;   std::vector<loom_knot_t> knots;
    loom_provenance_t prov;  uint32_t steps = 0;
};
// false + err (never a partial fabric) when prov.exact is false — the
// "statistical producers can never appear as fabric" law, enforced here.
bool loom_fabric_build(const asmtest_valtrace_t *vt, const asmtest_defuse_t *g,
                       const loom_provenance_t &prov, loom_fabric_t *out, std::string *err);
```

Exact rules (as header comments):
- **Register deck order is fixed**, from the producer's own tables: the SysV argument registers
  rdi rsi rdx rcx r8 r9 (`arg_regs`, [src/dataflow_emu.c:273–276](../../../src/dataflow_emu.c#L273)),
  then the remaining GPs in `df_zero_gp` order — rax rbx rbp r10–r15
  ([:230–234](../../../src/dataflow_emu.c#L230)) — then rsp, rip, eflags (the rest of
  `cap_x86_to_uc`, [:109–133](../../../src/dataflow_emu.c#L109)). Only touched registers
  materialize; the order never changes. Lane keys fold sub-registers to the container with a
  Capstone-side copy of that switch (do not link the producer for it).
- **Memory lanes on first touch, coalesced into bands**: collect every `AT_LOC_MEM_*` record's
  `[addr, addr+size)`; sort; merge ranges whose gap ≤ 64 bytes; order bands by first-touch step;
  names show the range and label `vt->mem_space`. **Spans** are write-record granularity:
  `[W.step, next write on the same lane intersecting W's bytes)`; no successor → `UINT32_MAX`.
- **Born-of-untraced-state**: a lane whose first record is a read gets a synthetic
  `[0, first_write)` span, `born_untraced = true`, value from that read when valid
  (pre-instruction hooks capture source state, so entry args get real chips). **Hops**: per
  `g->edges[i]`, resolve the producing span on the edge's location lane; skip when unresolvable
  (never guess).

**Tests.** `desktop/test/test_loom_fabric.cpp` (headless, render-only, under `desktop-test`
(**new — 03**)): hand-fill a valtrace mirroring `df_chain`
([examples/test_dataflow_emu.c:38–45](../../../examples/test_dataflow_emu.c#L38)); assert deck
order (rdi before rsi before rdx/rcx/rax/rsp), exactly one mem band, the step-1 store span
carries chip 7, the hop mirroring edge 1→2 exists, an invalid-value write yields a hollow span,
and `exact=false` input fails with err naming the statistical rule.

**Docs.** Header comments (internal surface). **Done when.** `make desktop-test` prints the
fabric tests passing; `ldd build/desktop-render` shows no unicorn/keystone/capstone.

### T2 — Draw plan, zoom renderer, honesty chrome  (M, depends on: T1, 03)

**Goal.** A deterministic, testable draw plan plus a thin ImDrawList painter; zoom semantics and
the plan-mandated honesty marks asserted headlessly.

**Steps.**
1. `desktop/src/loom/fabric_plan.h` + `.cpp`: `loom_plan` is a pure function of (fabric, camera).
   Zoom rules: a span narrower than 3 px collapses into its lane group's `density_ribbon`
   (per-column live-span count → intensity); a mem band at ≥ 12 px per byte explodes into
   `byte_row` prims with per-byte last-writer derived from the same records; `value_chip` only
   when the span fits its text; hops only when both endpoints are visible.
2. Honesty chrome, as plan output: `prov.truncated` → one `torn_edge` prim at the right boundary,
   text `"trace truncated: N of M steps recorded"` (N = `steps_len`, M = `steps_total`);
   `t_end == UINT32_MAX` → `fade_out` ("alive at trace end") — **never** a thread-death cap;
   `born_untraced` → `born_untraced_glyph`, hover "born of untraced state — provenance starts at
   instrumentation"; `prov.isolated_guest` → persistent `guest_badge`, "isolated guest — emulator
   replay, not silicon". Statistical-never-woven already refuses in T1; test here that
   `exact=false, producer="ibs"` names the rule in `err`.
3. `desktop/src/loom/fabric_imgui.cpp` paints prims via `ImDrawList`
   (`AddRectFilled`/`AddLine`/`AddBezierCubic`/`AddText`); hollow spans = outline-only. One smoke
   under 03's null-backend harness.

**Code.**

```cpp
// desktop/src/loom/fabric_plan.h
enum class loom_prim {
    lane_header, span, span_hollow, hop, knot, value_chip, density_ribbon, byte_row,
    torn_edge, fade_out, born_untraced_glyph, guest_badge,               // honesty chrome
    take_dim, take_hot, take_dashed_tail, patient_zero, fault_card };    // forks (T6)
struct loom_prim_t { loom_prim kind; float x0,y0,x1,y1; uint32_t a,b; std::string text; };
struct loom_view_t { double step0, steps_per_px; int lane0; float lane_h, px_w, px_h; };
size_t loom_plan(const loom_fabric_t &f, const loom_view_t &v, std::vector<loom_prim_t> *out);
```

**Tests.** `test_loom_plan.cpp`: zoomed-out camera → ≥ 1 `density_ribbon`, 0 `byte_row`;
zoomed-in on the band → byte rows + chips; two identical calls → byte-identical vectors
(determinism, T7's seam). `test_loom_chrome.cpp`: truncated fixture → torn edge with correct
N/M; fade-out count == alive-at-end spans; badge iff `isolated_guest`; glyph on entry spans.

**Docs.** none. **Done when.** `make desktop-test` passes plan + chrome tests with the copy
strings above verbatim; `make desktop-render` builds and opens a fixture (manual smoke).

### T3 — Selection = lineage, generation walk, biography, zeroization audit  (M, depends on: T1, 04)

**Goal.** Click any (lane, step): the resident value's ancestry + descendant closure lights as
one thread-tree; `[`/`]` walk generations; a biography narrates; a zeroization-audit mode
answers "who still holds descendants at T".

**Steps.**
1. `desktop/src/loom/lineage.h` + `.cpp`: resolve (lane, step) → resident span → defining step;
   closure via 04's client-side helper over the recorded `asmtest_defuse_edge_t` array
   (**new — 04**; semantically identical to the `asmtest_slice_forward`/`_backward` BFS,
   [asmtest_valtrace.h:206–212](../../../include/asmtest_valtrace.h#L206), so full-build tests
   cross-check against the library).
2. `loom_plan` dims spans/hops outside the selection; `[`/`]` move a generation boundary (BFS
   depth recorded per step; keyboard via 03's shell).
3. Biography rows: **birth kind** (entry-seeded arg / born-untraced / ALU write [knot] / load /
   store), **every hop** disassembled from `prov.disasm[step]` (offset fallback, D10),
   **escapes** (a consumer that writes a mem band — "escapes to memory at step K"), **producer
   tier** (`prov.producer`, exact/replay), **truncation state** (window-bounded caveat).
4. **Zeroization audit** is a selection mode: forward closure F of a chosen birth ∩ residency —
   a lane lights iff its resident span at playhead T (last `t_write ≤ T < t_end`) has its
   defining step in F. Scrubbing re-evaluates residency only. UI copy (asserted in T7): title
   "zeroization audit — bounded to the traced window"; hover "'clear' means *not overwritten
   within the traced window* — untraced state and post-trace writes are invisible".

**Code.**

```cpp
struct loom_selection_t {
    uint32_t origin_step;  std::vector<uint32_t> steps;  // ascending, deduped
    std::vector<int> generation;    // BFS depth (negative = ancestors)
    int gen_lo, gen_hi, gen_view; };  // [ / ] move gen_view
bool loom_select(const loom_fabric_t &f, const asmtest_defuse_edge_t *e, size_t n,
                 uint32_t lane, uint32_t step, loom_selection_t *out);
size_t loom_audit(const loom_fabric_t &f, const asmtest_defuse_edge_t *e, size_t n,
                  uint32_t birth_step, uint32_t playhead, std::vector<uint32_t> *lit_lanes);
```

**Tests.** `test_loom_lineage.cpp` on the `df_chain` model: selecting (rax, step 4) yields
{0,1,2,3,4} excluding 5 — the hand-derived slice at
[examples/test_dataflow_emu.c:111–116](../../../examples/test_dataflow_emu.c#L111); `[` from
step 4 highlights {3}; the step-1 biography names "store", value 7, the load-after-store hop.
Audit: birth 0, playhead last → lit = {mem band, rcx, rdx, rax}; playhead 1 → {rax, mem band};
hollow residents light with the hollow flag (route known, value not).

**Docs.** none. **Done when.** lineage + audit tests pass render-only; the full-build
cross-check equals `asmtest_slice_backward`; both audit copy strings exist verbatim.

### T4 — Lane inspector annex: place-indexed cross-feed join  (M, depends on: T1, 02, 03)

**Goal.** Clicking a lane header joins what companion recordings in the Workspace recorded
about that place — under provenance chips, as corroboration, never contradiction.

**Steps.**
1. `desktop/src/loom/annex.h` + `.cpp`. Include `asmtest_taint.h` and `asmtest_trace.h` (pure);
   **never** `cli/asmspy.h` — mirror the watch-hit fields
   ([cli/asmspy.h:573–585](../../../cli/asmspy.h#L573)).
2. Entry sources, all via 02's reader over Workspace recordings (**new — 02**; kinds per 01):
   **watch hits** (tid, pc, addr, is_write, value/value_len/value_ok, func/module/off), **taint
   hits** (`at_taint_hit_t`, [include/asmtest_taint.h:60–70](../../../include/asmtest_taint.h#L60)),
   **srcmap rows** (`asmtest_srcmap_entry_t` from the recording; live `asmtest_srcreg_resolve`
   ([include/asmtest_trace.h:241–243](../../../include/asmtest_trace.h#L241)) is full-build-only),
   **IBS survey edges** (from/to + count + drop accounting) — **annex-only**, never woven.
3. Join keys — two spaces, each entry declares which: **data** (mem-band lane: watch `addr` ∈
   band; taint `ea` ∈ band) and **code** (step column / routine header: srcmap rows, taint `off`,
   IBS from/to within the routine's code range). Register lanes join watch hits by displayed
   `value` only, labeled "same value, different place — informational".
4. **Corroboration-never-contradiction** (exact): every entry renders one of exactly two labels —
   `"corroborates"` (an exact fabric record overlaps the same bytes) or `"recorded by <producer>
   — unconfirmed here"`. No third state exists: statistical absence proves nothing (timebase-free
   histograms cannot prove non-execution), and a mismatched exact companion still says
   "unconfirmed", never "conflicts". Statistical chips carry their drop/loss counts.

**Code.**

```cpp
enum class annex_kind { watch_hit, taint_hit, srcmap_row, ibs_edge };
enum class annex_verdict { corroborates, unconfirmed };  // deliberately closed
struct annex_entry_t {
    annex_kind kind; annex_verdict verdict; bool exact;
    std::string provenance;   // chip: "rec-B / asmspy --watch / exact"
    uint64_t addr; std::string text; };
size_t loom_annex_join(const workspace_t &ws /* new — 03 */, const loom_fabric_t &f,
                       uint32_t lane, std::vector<annex_entry_t> *out);
```

**Tests.** `test_loom_annex.cpp` with hand-built companion arrays: a watch hit inside the band →
`corroborates` when the fabric wrote there, `unconfirmed` when not; an IBS edge joins the code
key only; the verdict enum has exactly two enumerators (static_assert) and no output contains
"contradict".

**Docs.** none. **Done when.** annex tests pass render-only.

### T5 — Fork engine: one-fact takes via the C library  (M, depends on: T1 — FULL BUILD ONLY)

**Goal.** `loom_take_run`: apply exactly one edit (entry-arg or code-patch), re-run from entry
deterministically, return a new fabric input + fault data. Compiled **only** into `desktop`
(GPL side, D7/D9); `desktop-render` never sees this TU.

**Steps.**
1. `desktop/src/loom/forks.h` + `forks.cpp`; re-declare `asmtest_dataflow_emu_run` locally (no
   public header — the idiom at
   [examples/test_dataflow_emu.c:25–27](../../../examples/test_dataflow_emu.c#L25)).
2. Take procedure, in order: **code_patch** — `asmtest_assemble(ASM_X86_64, syntax,
   patched_source, EMU_CODE_BASE, &r)`; on `!r.ok` set `out->err = r.err`, return false — the
   loud-drop contract ([include/asmtest_assemble.h:83–93](../../../include/asmtest_assemble.h#L83))
   turns a skipped statement into a visible fork failure, never a fabric of code the user didn't
   write; free with `asmtest_asm_free`. **entry_arg** — copy base args; replace
   `args[edit.arg_index]`. **fabric** — `asmtest_valtrace_new` +
   `asmtest_dataflow_emu_run(code, len, args, nargs, 0, vt)`, hermetic per call (own engine
   [src/dataflow_emu.c:257](../../../src/dataflow_emu.c#L257), deterministic init
   [:229–240](../../../src/dataflow_emu.c#L229)). **fault card + offsets** — on the app's shared
   `emu_t`, bracketed: `emu_snapshot` once at fork-session open, `emu_restore` before **every**
   take, then `emu_call_traced(e, code, len, args, nargs, 0, &out->result, &out->trace)`;
   mandatory because mapped memory persists across `emu_call_*`
   ([include/asmtest_emu.h:586–594](../../../include/asmtest_emu.h#L586)) — without it take N
   inherits take N−1's dirt; `emu_snapshot_free` at session close. **edges** —
   `out->g = asmtest_defuse_build(vt)`.
3. Threading (exact): `loom_take_run` executes on 03's worker-job seam (**new — 03**); the shared
   `emu_t` + snapshot live on that one worker (all takes serialize through it); the UI thread
   receives the finished `loom_take_t` by move — no lock guards engine state. Panel copy (plan
   limit, restated): "forks re-run the emulator replay — an explicit crossing of the
   native→virtual line; never evidence about a live process or silicon timing".

**Code.**

```cpp
struct loom_edit_t {
    enum class kind { entry_arg, code_patch } k;
    int arg_index = 0; long arg_value = 0;          // entry_arg
    std::string patched_source; int syntax = 0; };  // code_patch (full listing)
struct loom_take_t {
    loom_edit_t edit; std::vector<uint8_t> code;
    asmtest_valtrace_t *vt = nullptr; asmtest_defuse_t *g = nullptr;
    emu_result_t result{}; asmtest_trace_t trace{}; // insns[] for alignment
    std::string err; };                             // loud, never silent
bool loom_take_run(emu_t *session, const emu_snapshot_t *base_state, const uint8_t *code,
                   size_t code_len, const long *args, int nargs, const loom_edit_t &edit,
                   loom_take_t *out);
```

**Tests.** `test_loom_forks.cpp` (full-build list only): `df_chain`, arg0 7→9 → 6-step fabric,
store chip 9; the same take twice → byte-identical `vt` (determinism); a listing with one
unparseable statement → false, `err` contains "skipped"; a faulting patch fills
`result.faulted` + `fault_addr`.

**Docs.** none. **Done when.** fork tests pass in the full-build half of `desktop-test`;
`desktop-render` still links with zero engine deps.

### T6 — Fork UX: takes gutter, patient zero, dead-dependency dimming, fault cards  (M, depends on: T5, T2, 04)

**Goal.** Render takes beside the base fabric: aligned prefix, first divergence flagged,
interventional dim/hot verdicts, dashed tails, fault cards.

**Steps.**
1. Takes gutter: one node per take — "(edited fact → new fabric)"; selecting a take weaves it
   beside the base.
2. **Alignment** = shared-prefix over per-step `insn_off` (the plan's day-one rule; sound for
   same-entry runs), via 04's first-divergence helper (**new — 04**): prefix length P = first
   differing index (or min length). **Patient zero**: when P < min(steps), a `patient_zero` prim
   at column P naming both offsets; when offsets never diverge, no flag — the gutter says
   "aligned end-to-end" (an arg edit on straight-line code diverges in values, not offsets; do
   not fake one). **Post-divergence tails**: take spans with `t_write >= P` render
   `take_dashed_tail`; hover "unaligned — never drawn as agreement".
3. **Cone of the edited fact**: entry_arg → seed every step reading the edited arg register with
   no in-trace producer edge into that read, then forward closure (T3's helper); code_patch →
   seed the first aligned step whose code bytes at its `insn_off` differ between original and
   patched bytes.
4. **Dimming** (exact, inside prefix ∩ cone): match write records across base/take by (kind,
   folded reg | addr, size). All matched pairs `value_valid` on both sides **and** byte-equal →
   `take_dim` (dependence without consequence). Any pair valid both sides and different →
   `take_hot`. Either side invalid → **neither** (neutral cone outline; equality of unknown
   values is never claimed). Cone steps with no writes: outline only.
5. **Fault cards**: `take.result.faulted` → `fault_card` ending the take's fabric:
   `emu_fault_kind_t` ([include/asmtest_emu.h:77–82](../../../include/asmtest_emu.h#L77)) +
   `fault_addr` + the `emu_fault_describe` line ([:510](../../../include/asmtest_emu.h#L510))
   (offset-only without Capstone — accept the degraded text).

**Tests.** Unit-level in `test_loom_forks.cpp` on hand-built pairs: prefix/P math; the three-way
verdict truth table (equal / different / either-invalid); dashed-tail threshold. End-to-end
differential is T7's.

**Docs.** none. **Done when.** the verdict table passes; a faulting take renders a fault card,
not a crash.

### T7 — Golden looms, render tests, fork differential  (M, depends on: T2, T3, T4, T6, 02)

**Goal.** Committed walkthrough-grade fixtures + tests pinning the rung, per D6/D7: byte-stable
goldens, a dishonesty fixture, a deterministic fork differential.

**Steps.**
1. Extend the `make asmtrace-golden` generator (**new — 02**) with four entries written to
   `tests/golden-asmtrace/`, committed: `loom-df-chain.asmtrace` — `df_chain` bytes
   ([examples/test_dataflow_emu.c:38–45](../../../examples/test_dataflow_emu.c#L38)), args
   {7, 5}; walkthrough #1, hand-derivable on paper. `loom-sum-via-rbx.asmtrace` —
   `asmtest_corpus_routine("sum_via_rbx")`
   ([bindings/conformance/corpus_routines.c:37](../../../bindings/conformance/corpus_routines.c#L37)),
   args {2, 3}; walkthrough #2, callee-save spill → stack band, EFLAGS knot, restore;
   host-routine bytes ⇒ generate in the pinned x86-64 docker lane only (the generator refuses
   elsewhere with a printed reason). `loom-truncated.asmtrace` — `df_chain` with `recs_cap = 4`
   so `truncated` flips ([asmtest_valtrace.h:113](../../../include/asmtest_valtrace.h#L113)): the
   D7 dishonesty fixture. `loom-fork-demo.asmtrace` — the base run (arg 3) of the fixture below.
2. Fork fixture `fork_demo` (bytes committed in generator + test with this listing as the
   comment; hand-verify encodings once against a disassembler):

```asm
; fork_demo(a)  [rdi=a] — one dimmed, one hot, one control divergence.
0x00  mov rdx, rdi      ; hot: value differs across takes
0x03  shr rdx, 63       ; DIMMED: 0 for both non-negative args
0x07  mov rax, rdi      ; hot
0x0a  cmp rax, 10       ; hot (EFLAGS value differs)
0x0e  jle 0x13          ; last aligned step
0x10  neg rax           ; runs only when a > 10
0x13  ret
```

3. Round-trip + render tests in `desktop-test` (render-only): each fixture → 02's reader →
   `loom_fabric_build` → assert: df-chain — deck order, the 1→2 hop, chip 7, fade-out on rax;
   sum-via-rbx — a stack band from the push, an EFLAGS lane, rbx biography "store → restore";
   truncated — `torn_edge` with the exact T2 copy, plus the T2/T3 copy-string checks (guest
   badge, born-untraced glyph on rdi/rsi, audit boundedness).
4. Fork differential (full build): base `fork_demo` arg 3, take `entry_arg` 3→11. Assert: aligned
   prefix = 5; `patient_zero` at index 5 (0x13 vs 0x10); step 1 (`shr`) → `take_dim`; step 0 →
   `take_hot`; take spans from index 5 dashed; the whole run twice → byte-identical plans. If the
   pinned Unicorn ever makes the takes' post-`shr` EFLAGS differ, the test must fail loudly —
   swap the fixture instruction and regenerate; never loosen the dim assertion.
5. Code-patch leg: patch `shr rdx, 63` → `shr rdx, 1` via `asmtest_assemble` (Intel syntax) →
   step 1 goes hot, cone seeds at step 1; a deliberately broken listing surfaces the loud-drop
   error in the fork card.

**Docs.** One `### Added` bullet under `## [Unreleased]` in [CHANGELOG.md](../../../CHANGELOG.md)
(extend the doc set's desktop entry if present). Fixtures referenced from 03's
`desktop/README.md` Learn-door list (**new — 03**).

**Done when.**
- `make asmtrace-golden && git diff --exit-code tests/golden-asmtrace/` is clean in the docker
  lane.
- `make desktop-test` passes every loom test render-only; the full-build half passes the
  differential; `make docker-desktop` runs the whole set green.

## Task order & parallelism

- **T1** first — everything consumes the fabric model. **T2**, **T3**, **T4** are then
  independent — three people in parallel. **T5** needs only T1 (full-build side); **T6** joins
  T5 + T2 + 04. **T7** last; its fixture generation (steps 1–2) can start once 02's generator
  exists.
- Critical path: **T1 → T2 → T6 → T7**. Sibling gates: 03's `mk/desktop.mk` + harness before
  anything builds; 02's reader before T1's replay feeder and T7; 04's helpers before T3/T6 (until
  04 lands, T3 may develop against `asmtest_slice_*` in the full build — the swap is one call
  site).

## Constraints & gates

- **License split (D4/D7/D9).** `forks.cpp` (anything touching `asmtest_assemble` / `emu_*` /
  `asmtest_dataflow_emu_run`) compiles only into the GPL `desktop` binary. Everything else must
  build into `desktop-render` with zero engine deps — `asmtest_valtrace.h`, `asmtest_trace.h`,
  `asmtest_taint.h` are the only library headers allowed there (all verified pure above). Never
  include `cli/asmspy.h`.
- **Exact-only fabric.** Statistical producers reach the annex only; the refusal lives in
  `loom_fabric_build` and is tested. Provenance/truncation come from the recording (01), never
  re-derived. **x86-64-guest-only**: the only L0 value producer is the x86-64 emulator producer;
  the panel states this rather than greying silently.
- **Forks never touch a live process.** Replay-only, emulator-only, labeled (T5 step 4). Day-one
  alignment is shared-prefix only — per-step semantic alignment is the Reweave growth rung.
  **Determinism is a test subject**: hermetic producer runs + the snapshot/restore bracket make
  T5/T7 assertions possible — never reuse an unbracketed `emu_t`.
- **Tooling per [CLAUDE.md](../../../CLAUDE.md)**: everything runs in the `docker-desktop` lane
  (03); goldens regenerate there. No hardware or credential gate exists in this rung — nothing
  here may self-skip. **Style (D8)**: the repo `.clang-format` applies to `desktop/`;
  `make docker-fmt-check` (clang-format 18 canonical, per
  [_conventions](../implementations/_conventions.md)).

## Out of scope

- **Every growth rung**: Reweave / mid-run edits (no resume-from-state API), per-step register
  ring + O(1) scrub (Phase 4), descriptor-driven decks (Phase 1), secret-class multiverse /
  FP-env badges (Wave 1), Observer looms via PT replay or ptrace dataflow (Phase 3), per-tid lane
  decks, blame deep-links (Wave 2).
- **Cross-thread value hops and syscall-born threads** — the valtrace has no tid dimension and no
  structured buffer-EA syscall kind; nothing here may fake either. **arm64-guest fabrics** — no
  arm64-code value producer exists; the fabric is x86-64-guest-only and says so.
- **The `.asmtrace` schema** (01, and D5's `docs/internal/gui/asmtrace-schema.md`),
  **reader/generator** (02), **shell/Workspace/docker lane** (03), **generic diff +
  slice-explorer views** (04) — consumed here, owned there. **Live capture of any kind** —
  Observer mode is 07/08's territory; every loom here is a recording or local emulator replay.
