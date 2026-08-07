# asmspy's TUI gains the channels the GUI already has — implementation

> **Source.** Findings **#7, #8, #9** of the 2026-08-07 GUI/asmspy visualisation
> review, cut per [62-encoding-integrity-roadmap.md](62-encoding-integrity-roadmap.md) §3.
> **#7 contains a live bug**: selecting a row in the data-flow view erases its slice
> membership.
>
> Read [\_conventions.md](../implementations/_conventions.md) first; D1–D11 live in
> this directory's [README](README.md). **Prerequisites: none.** Touches only
> [cli/](../../../cli/) — no desktop file, no engine, no wire or schema change. It
> shares one *decision* with [63](63-magnitude-transform-and-denominator.md) T2 (the
> `log1p` transform) and not one line of code.
>
> Authored 2026-08-07 against `35ef821f`, every citation verified; re-checked at
> `3e2e8cea`, which touches no file cited here. If a cited `file:line` disagrees
> with the code, the code wins — re-verify, then fix this doc in the same change.
>
> **Status — ☐ 0/4.**

## Why this work exists

asmspy's TUI predates the visual language docs [23](../archive/gui/23-graded-truth-layer.md)
and [24](../archive/gui/24-one-visual-language.md) built for the desktop, and never
received any of it. Concretely, across all of [cli/](../../../cli/):

- **There is no colour.** No `start_color`, `init_pair`, `COLOR_PAIR` or
  `use_default_colors` appears in any `cli/*.c` or `cli/*.h` — a measured absence, not
  an impression. Every distinction in the TUI rides on `A_BOLD`, `A_REVERSE` and
  `A_DIM`, and `A_BOLD` is already spent on headings and pane labels
  ([asmspy.c:7251,7365,7420](../../../cli/asmspy.c#L7251)).
- **There is no magnitude channel.** Every ranked view formats counts as bare digits.
- **The numerics are left-aligned**, discarding even the free digit-column-width cue
  a monospace grid gives for nothing.

**The bug.** Three attributes for four states forces an exclusive choice, and the
data-flow view makes it:

```c
attr_t a = A_NORMAL;                                     /* asmspy.c:7401 */
if (is_sel)                            a = A_REVERSE;
else if (rs == ASMSPY_DF_ROW_INSLICE)  a = A_BOLD;
else if (rs == ASMSPY_DF_ROW_DIMMED)   a = A_DIM;
```

The assignments are `=`, not `|=`. Moving the cursor through a backward slice is the
**core gesture** of this view — `b`/`f` seed a slice and up/down walk it
([asmspy.c:7424](../../../cli/asmspy.c#L7424)) — and the moment the cursor lands on a
row, that row stops saying whether it is in the slice. The pure decision function
`asmspy_df_rowstyle` is correct and unit-tested
([asmspy_dataview.h:139-145](../../../cli/asmspy_dataview.h#L139)); its answer is
discarded one line later by the renderer.

**The truncation.** `%-30.30s` keeps the **first** 30 characters
([asmspy.c:1519](../../../cli/asmspy.c#L1519), [:551](../../../cli/asmspy.c#L551),
[:590](../../../cli/asmspy.c#L590), [:1652](../../../cli/asmspy.c#L1652)). For
mangled C++, JIT method names from the perf-map, and the `func+0xNN [module]` form
[asmspy.c:2098](../../../cli/asmspy.c#L2098) builds, the distinguishing part is the
**tail** — so two hot edges into different methods of one class render as the same
string, in the view whose entire job is ranking them apart.

## What already exists (verified 2026-08-07)

- **The header-extraction discipline is the load-bearing convention here.** The TUI
  cannot be driven in CI, so its pure logic is factored into headers and tested
  headlessly: [asmspy_logview.h](../../../cli/asmspy_logview.h) (viewport math,
  `test_logview`), [asmspy_dataview.h](../../../cli/asmspy_dataview.h) (annotation,
  slice styling, def-use, `test_view`),
  [asmspy_graphsort.h](../../../cli/asmspy_graphsort.h), and
  [asmspy_treefilter.h](../../../cli/asmspy_treefilter.h). **Every task below follows
  it**: the decision is a pure function in a header, the renderer only applies it.
- **`asmspy_df_rowstyle`** already returns the three-state slice verdict as its own
  type ([asmspy_dataview.h:133-145](../../../cli/asmspy_dataview.h#L133)) — T1 needs
  no new decision, only a composition of it with selection.
- **UTF-8 is already established and already load-bearing.** `setlocale(LC_ALL, "")`
  runs before `initscr()` ([asmspy.c:8341-8346](../../../cli/asmspy.c#L8341)), and
  the process tree already draws box-drawing glyphs on that basis
  ([asmspy.c:779-784](../../../cli/asmspy.c#L779), `TG_TEE`/`TG_ELB`/`TG_PIPE`). The
  block-eighths bar T3 adds sits on the same guarantee.
- **The percentage form already exists.** `sample_edge_tag` computes
  `(mispred * 100) / count` ([asmspy.c:1503](../../../cli/asmspy.c#L1503)) — the
  number [63](63-magnitude-transform-and-denominator.md) T4 brings to the desktop.
- **The build lane.** `make cli-smoke` builds and runs every `cli/test_*`
  ([mk/cli.mk:770-790](../../../mk/cli.mk#L770)); the per-test rule pattern is
  [mk/cli.mk:473-485](../../../mk/cli.mk#L473). `make docker-cli`
  ([mk/cli.mk:815](../../../mk/cli.mk#L815)) is the containerised lane, and is how
  these run per [CLAUDE.md](../../../CLAUDE.md)'s tooling rule.
- **ncursesw is already linked** (`NCURSES_LIBS`, [mk/cli.mk:9](../../../mk/cli.mk#L9)),
  so `start_color` / `use_default_colors` need **no new dependency** — this brief adds
  nothing to `Dockerfile.cli`.

## Fidelity rules (binding on every task)

1. **Colour is an addition, never a replacement.** Every distinction must survive
   `has_colors() == false`. That is why T1 (a character-cell gutter) lands **before**
   T2 (colour) and is the channel the tests assert on: the terminal that refuses
   colour is the null backend of this surface, and the desktop's own rule is that the
   encoding survives it.
2. **The number is the truth; the bar is the gloss.** T3 keeps every exact count as
   digits. No column becomes bar-only.
3. **A transform is stated or it is not applied.** T3's log bars carry the transform
   in the pane header, as [63](63-magnitude-transform-and-denominator.md) T2 does for
   the desktop and `height_scale_note` does for the 3D pane.
4. **Never bar an absence.** A zero or absent count draws no bar — the same rule
   `dt_magnitude_frac`'s `value <= 0 -> 0` branch encodes
   ([theme.h:170](../../../desktop/src/ui/theme.h#L170)).
5. **Ellipsis is visible.** T4's middle-truncation inserts a literal marker; a
   silently shortened symbol is the defect, not the fix.
6. **Every new decision is a pure function in a header with a test.** A rendering
   rule that lives inside a `for` loop in `asmspy.c` cannot be tested, and the whole
   reason this bug survived is that the decision was correct and its application was
   not.

## Tasks

### T1 — Extract row decoration, compose it, and add a mono-safe gutter (M)

1. Create `cli/asmspy_rowfmt.h`, a pure header in the shape of
   [asmspy_dataview.h](../../../cli/asmspy_dataview.h) (static inline, no ncurses, no
   ptrace, no Capstone). It holds the decoration **decision**:

   ```c
   typedef struct {
       unsigned attrs;      /* A_* bits, composed — never exclusive */
       int pair;            /* colour pair id, 0 = none (T2 fills this) */
       const char *gutter;  /* the character-cell channel: ">", "|", " " */
   } asmspy_rowdec_t;

   static inline asmspy_rowdec_t
   asmspy_row_decorate(int is_sel, asmspy_df_rowstyle_t rs, int have_colour);
   ```
2. Compose, do not choose: selection contributes `A_REVERSE`, slice membership
   contributes `A_BOLD` (in-slice) or `A_DIM` (dimmed), and a selected in-slice row
   gets **both**. This is the bug fix.
3. Add the gutter as a leading character cell — `>` selected, `|` in-slice, space
   otherwise — so the distinction survives a terminal with no colour **and** one whose
   `A_BOLD` and `A_REVERSE` compose badly (rule 1). This is the second channel, and
   it is the one the tests assert.
4. Replace [asmspy.c:7401-7412](../../../cli/asmspy.c#L7401) with a call to it, and
   prefix the row with `dec.gutter`.
5. Add `cli/test_rowfmt.c` and wire it in: a build rule beside `test_logview`
   ([mk/cli.mk:473-476](../../../mk/cli.mk#L473)) and an entry in the `cli-smoke`
   prerequisite list ([mk/cli.mk:777-782](../../../mk/cli.mk#L777)). Assert all six
   `(is_sel, rowstyle)` combinations, and specifically that
   `(is_sel=1, INSLICE)` and `(is_sel=1, DIMMED)` differ — **the case that fails
   today**.

**Done when** `make docker-cli` runs `test_rowfmt` green and the selected-in-slice
case is asserted distinct from selected-out-of-slice.

### T2 — Colour pairs, mirroring the desktop's semantic axis (M)

1. After `initscr()` ([asmspy.c:8346](../../../cli/asmspy.c#L8346)), add a guarded
   init: `if (has_colors()) { start_color(); use_default_colors(); … }`.
   `use_default_colors()` keeps the user's own background, so a light-terminal user
   is not forced onto black — the same concern
   [theme.h:15-24](../../../desktop/src/ui/theme.h#L15) handles for the desktop.
2. Define the pairs as an enum in `asmspy_rowfmt.h`, one per **meaning**, named after
   the desktop accessor it mirrors so the two surfaces cannot drift: `warn`,
   `refuse`, `good`, `dim`, `hot`, `selected`. Give each one a doc comment stating
   its ONE meaning, exactly as [theme.h](../../../desktop/src/ui/theme.h) does — that
   comment discipline is what makes F14 enforceable.
3. Fill `asmspy_rowdec_t::pair` from `have_colour`; when `have_colour` is 0 the pair
   is 0 and T1's attrs + gutter carry everything (rule 1).
4. Apply the pairs at the sites that already have a *meaning* to express and are
   currently monochrome: the `THROTTLED` / `samples lost` chrome
   ([asmspy.c:6851-6855](../../../cli/asmspy.c#L6851)) → warn; the
   unavailable/attach-failure placards ([asmspy.c:7305-7320](../../../cli/asmspy.c#L7305))
   → refuse; the `[?]` / `[JIT]` / `[EXT]` module tags
   ([asmspy.c:546-550](../../../cli/asmspy.c#L546)) → dim. **Do not** repaint
   everything; a colour with no meaning is the drift this brief exists to avoid.
5. Extend `test_rowfmt.c` to assert `pair == 0` for every combination when
   `have_colour == 0`, which is the mono guarantee in one line.

**Done when** `test_rowfmt` pins the mono guarantee and a manual run under
`TERM=vt100` (no colour) is visually unchanged from today except for T1's gutter.

### T3 — A magnitude bar column, and right-aligned numerics (M)

1. Add to `asmspy_rowfmt.h`:

   ```c
   /* A block-eighths bar of `width` cells for v/max, log1p-scaled to match the
    * desktop's dt_magnitude_frac_log and terrain.cpp's height transform. Writes
    * "" when v == 0 or max == 0 — an absence is never barred. */
   static inline void asmspy_bar(char *out, size_t cap, unsigned long long v,
                                 unsigned long long max, int width);
   ```
   Use `U+2588`…`U+258F` for sub-cell resolution, on the UTF-8 guarantee the process
   tree already relies on ([asmspy.c:779-784](../../../cli/asmspy.c#L779)).
2. Prepend an 8-cell bar to the ranked rows, and **right-align every count**:
   - hot edges, [asmspy.c:1515-1521](../../../cli/asmspy.c#L1515) — bar against the
     window's max count; `%8llu` is already right-aligned, keep it.
   - call graph, [asmspy.c:546-554](../../../cli/asmspy.c#L546) — bar against max
     `invocations`; change `inv=%-7llu calls=%-7llu fanout=%-5u` to `%7llu`/`%5u`.
   - peers, [asmspy.c:579-596](../../../cli/asmspy.c#L579) — change `%-8llu` to `%8llu`.
   - process tree, [asmspy.c:801-807](../../../cli/asmspy.c#L801) — bar against max
     `inv` across the forest. The tree's glyph prefix already varies in width, so the
     bar goes at the **line end**, after `inv=`, where it stays column-aligned.
3. State the transform in each pane header, beside the existing counts line — e.g.
   [asmspy.c:6850-6855](../../../cli/asmspy.c#L6850) gains *"bar: log(1+count),
   full = N"* (rule 3), matching `height_scale_note`'s wording.
4. Assert in `test_rowfmt.c`: `v == 0` and `max == 0` both yield `""` (rule 4); a
   `{1, 2, 4000}` set yields three **distinct** non-empty bars — the case a linear
   scale fails; and the bar never exceeds `width` cells.

**Done when** `test_rowfmt` passes those four assertions and the ranked panes render
right-aligned counts.

### T4 — Ellipsize symbol names from the middle (S)

1. Add to `asmspy_rowfmt.h`:

   ```c
   /* Fit `s` into `width` display cells, dropping from the MIDDLE and inserting
    * "…" — the tail of a symbol (the method name, the +0xNN) is what
    * distinguishes it, so head-truncation loses exactly the wrong end. */
   static inline void asmspy_fit_mid(char *out, size_t cap, const char *s,
                                     int width);
   ```
   Count **display cells**, not bytes: the input can carry UTF-8 (a demangled C++
   name, a JIT method from the perf-map), and a byte-count split would cut a code
   point in half.
2. Replace `%-30.30s` / `%-34.34s` at [asmspy.c:1519](../../../cli/asmspy.c#L1519),
   [:551](../../../cli/asmspy.c#L551), [:590](../../../cli/asmspy.c#L590) and
   [:1652](../../../cli/asmspy.c#L1652) with a pre-fitted buffer.
3. Assert in `test_rowfmt.c`: a short name is unchanged; two long names differing
   only in their tail produce **different** output (the defect, stated as a test); a
   multi-byte input is never split mid-code-point; and output never exceeds `width`
   cells.

**Done when** those four assertions pass. The tail-differing case is the one that
fails today and is the reason this task exists.

## Testing

All headless, all through `make docker-cli` per [CLAUDE.md](../../../CLAUDE.md)'s
tooling rule. `make cli-smoke` builds and runs `test_rowfmt` alongside the existing
`test_logview` / `test_view` / `test_graphsort` set.

**No new dependency.** ncursesw is already linked
([mk/cli.mk:9](../../../mk/cli.mk#L9)) and `start_color` / `use_default_colors` are
part of it, so `Dockerfile.cli` is untouched. Nothing here self-skips: the TUI
rendering itself is untestable in CI (which is *why* the decisions move into a
header), but every decision this brief adds is asserted headlessly on every host.

No `.asmtrace` golden and no `tests/golden-asmtrace/` file is touched; no JSON on the
`--sample` / `--graph` / `--dataflow` wire changes. `survey_record`
([asmspy.c:1527](../../../cli/asmspy.c#L1527)) and every other recorder emit exactly
what they emit today — this brief is the interactive renderer only.

## Out of scope

- **The sparkline / window-history column** (review finding #10). It needs new
  retained state in `sample_snap` and has an unresolved fidelity question about
  dropped windows — see [62-encoding-integrity-roadmap.md](62-encoding-integrity-roadmap.md) §5.
  When it is cut, it reuses T3's `asmspy_bar` helper.
- **The headless `--sample` / `--dataflow` stdout renderers**
  ([asmspy.c:1638-1656](../../../cli/asmspy.c#L1638)). They are pipeline output, not
  a view; T4's middle-ellipsis is worth applying there
  ([asmspy.c:1652](../../../cli/asmspy.c#L1652), included above) but colour and bars
  are not — a bar in a piped stream is noise.
- **Restructuring any TUI screen.** Every task here is additive decoration over the
  existing layouts. No pane is moved, no key binding changes.
