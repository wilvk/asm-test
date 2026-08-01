# Internal working documents

Everything under `docs/internal/` is **project working material** — plans,
analysis notes, and repo reviews — kept in-tree for history and context but
**excluded from the published documentation site** (see `exclude_patterns` in
[`../conf.py`](../conf.py)). The user-facing docs live in the rest of `docs/`.

Layout:

| Directory | Contents |
|---|---|
| `plans/` | **Active** implementation plans — work not yet landed or still in flight |
| `analysis/` | Design/analysis notes and investigations |
| `implementations/` | **Implementation-ready** briefs with **open tasks** — the open items from `plans/` + `analysis/`, verified against the tree and grouped into one self-contained, cold-startable document per task set (see [`implementations/README.md`](implementations/README.md)) |
| `gui/` | The desktop-GUI workstream's briefs with **open work** — same format as `implementations/`, one directory because the docs share binding build decisions (see [`gui/README.md`](gui/README.md)) |
| `reviews/` | **Open** repo reviews — findings not yet fully actioned |
| `archive/plans/`, `archive/reviews/` | Completed plans and fully-actioned reviews |
| `archive/implementations/`, `archive/gui/` | Briefs whose tasks are all complete. Each directory's `README.md` **stays** in the live tree as the single index for the whole family, linking into the archive |

**The archive rule:** when a plan's work has landed (or a review's findings are
all actioned, or an implementation brief's tasks are all done), move the file to
the matching `archive/` subdirectory in the same change that completes it.
"Done" material never sits in `plans/`, `reviews/`, `implementations/` or
`gui/`. Two clarifications, from the 2026-08-01 sweep that first applied this to
the brief directories:

- **The index READMEs do not move.** `implementations/README.md` and
  `gui/README.md` are the inventory and dependency map for their whole family,
  complete *and* open; they stay put and link into `archive/`. Same for
  `implementations/_conventions.md` and `_positions.md`, which every brief
  depends on, and `gui/asmtrace-schema.md`, which is a live format reference.
- **Roadmap docs stay while they point at unbuilt work**, even when they have no
  tasks of their own — a family overview whose children are all done is
  archivable, but one still listing uncut phases or open gaps is not.

Two conventions keep the published site's `-W` (fail-on-warning) build green:

- Published pages link **into** this tree only via GitHub blob URLs, never
  relative/doc cross-references (an xref into an excluded file warns).
- Files here may link anywhere with ordinary relative links — they render on
  GitHub only.
