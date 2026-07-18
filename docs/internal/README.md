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
| `implementations/` | **Implementation-ready** briefs — the open items from `plans/` + `analysis/`, verified against the tree and grouped into one self-contained, cold-startable document per task set (see [`implementations/README.md`](implementations/README.md)) |
| `reviews/` | **Open** repo reviews — findings not yet fully actioned |
| `archive/plans/`, `archive/reviews/` | Completed plans and fully-actioned reviews |

**The archive rule:** when a plan's work has landed (or a review's findings are
all actioned), move the file to the matching `archive/` subdirectory in the same
change that completes it. "Done" material never sits in `plans/` or `reviews/`.

Two conventions keep the published site's `-W` (fail-on-warning) build green:

- Published pages link **into** this tree only via GitHub blob URLs, never
  relative/doc cross-references (an xref into an excluded file warns).
- Files here may link anywhere with ordinary relative links — they render on
  GitHub only.
