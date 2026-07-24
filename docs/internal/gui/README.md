# Desktop GUI implementation documents — index

This directory holds the **implementation-ready specifications** for
[desktop-gui-plan.md](../plans/desktop-gui-plan.md): ten self-contained
briefs (nine core + one growth-rung companion), each for one coherent task set,
written so a junior developer can clone the repo, open exactly one document, and
implement it end to end (code + tests + docs) with no other context. Format and
rules follow [../implementations/](../implementations/README.md) — read
[\_conventions.md](../implementations/_conventions.md) once before starting.

> **Provenance.** Generated 2026-07-23/24 from the GUI plan (itself
> cross-reviewed against HEAD `a460d40` the same day), with every cited
> file:line re-verified against the tree at authoring time. Docs 01–06 went
> through independent factual + implementability review during authoring;
> 07–09 were authored against the same charters with the citations verified
> inline. If a claim disagrees with the code when you implement, the code
> wins — re-verify, then fix the doc in the same change. (Docs 02, 03 and 04
> each carry a dated banner recording exactly which of their claims lost to the
> code; 04's fourth correction — **the v1 schema has no routine-identity
> field** — is an open Phase-3-freeze item for 01, not a settled one.)

## Binding shared decisions (D1–D11)

Every doc conforms to these; they are stated once here.

- **D1 — layout.** All GUI code lives under `desktop/`: `desktop/src/`
  (C++17), `desktop/test/` (headless tests), `desktop/README.md`. Third-party
  code is fetched into `build/`, never committed.
- **D2 — pinned deps.** Dear ImGui via `scripts/fetch-imgui.sh` (pinned
  release, SHA-256 in `scripts/third-party-digests.txt`, MIT license vendored
  under `licenses/`); nlohmann/json single header the same way. App backends:
  GLFW + OpenGL3 (`libglfw3-dev` in `Dockerfile.desktop`); headless tests use
  the ImGui `example_null` pattern.
- **D3 — Makefile.** A new `mk/desktop.mk`, included from the top-level
  Makefile right after `mk/cli.mk`, owns `desktop`, `desktop-render`,
  `desktop-test`, `docker-desktop` (rule mirrors `mk/cli.mk`'s docker rule);
  new user-visible targets get `make help` echo lines. Shared property:
  additive rules only; whichever doc lands first creates it.
- **D4 — licensing.** The full app links bundled Unicorn/Keystone → GPL-2.0
  as a whole; `desktop-render` builds with **zero engine deps** and stays
  permissively distributable. Pin/SDE/libdft64 are never bundled.
- **D5 — schema home.** The `.asmtrace` draft schema lives at
  [asmtrace-schema.md](asmtrace-schema.md) (owned by 01, append-only for
  other docs) until the Phase-3 freeze.
- **D6 — golden corpus.** `tests/golden-asmtrace/`, regenerated
  deterministically by `make asmtrace-golden` from the conformance corpus,
  committed, byte-stable.
- **D7 — honesty is tested.** The corpus includes dishonesty fixtures
  (truncated/dropped/redacted); renderer tests assert banners, provenance
  chrome, and redaction defaults.
- **D8 — style.** The repo `.clang-format` (Language: Cpp) covers desktop/
  C++; `fmt-check` stays informational.
- **D9 — capture host.** Observer capture is the `asmspy` binary via
  `--serve` (local subprocess or ssh); the desktop app never links the ptrace
  engines. Author mode links the C library tiers (emulator/valtrace/assemble).
- **D10 — offline disasm.** Producers may attach a `disasm` string to
  trace/dataflow events at record time so the render-only viewer needs no
  Capstone; absence degrades to offsets.
- **D11 — naming.** The nine docs and their cross-reference names are exactly
  the filenames below.

## The documents

**Status** tracks task completion as `done/total`; update the cell as tasks
land (legend as in [../implementations/README.md](../implementations/README.md):
☐ · ◐ · ☑ · ✅).

| Doc | Area | Tasks | Depends on | Status |
|---|---|---|---|---|
| [01-asmtrace-format.md](01-asmtrace-format.md) | `.asmtrace` schema, record modes, golden corpus | 8 | — | ✅ 8/8 |
| [02-exporters-and-readers.md](02-exporters-and-readers.md) | speedscope/Perfetto exporters, completeness readers | 6 | 01 (03 for T5–T6) | ✅ 6/6 |
| [03-desktop-shell.md](03-desktop-shell.md) | desktop/ skeleton, deps, mk/desktop.mk, document model | 8 | 01 (corpus, for T7) | ✅ 8/8 |
| [04-replay-views.md](04-replay-views.md) | canvas, operand timeline, slice explorer, diff, deep links | 8 | 01, 03 | ✅ 8/8 |
| [05-loom-day-one.md](05-loom-day-one.md) | the Loom fabric, lineage, lane annex, forks | 7 | 02 (reader), 03, 04 | ✅ 7/7 |
| [06-doors-and-learning.md](06-doors-and-learning.md) | Learn/Author doors, ct_eq, capability panel, runner record mode | 7 | 01–04 | ✅ 7/7 |
| [07-serve-live-host.md](07-serve-live-host.md) | extract `libasmspy`, `--serve` wrapper, session host, budget patch-bay, Inspect door | 7 | 01, 03 | ✅ 7/7 |
| [08-observer-views.md](08-observer-views.md) | live views: syscalls, watch, topo, hot edges, tree filters, codeimage, PT slice | 8 | 07, 04, 01 | ✅ 8/8 |
| [09-teaching-producers.md](09-teaching-producers.md) | per-step register ring, scrubber, ABI x-ray, blame socket | 5 | 01, 03, 04, 06 | ☐ 0/5 |
| [10-spacetime-3d-overview.md](10-spacetime-3d-overview.md) | 3D memory-terrain + execution-trajectory overview surface (**growth-rung companion**) | 7 | 01, 03, 04, 07, 08 | ☐ 0/7 |

71 tasks across 10 docs. Suggested start order: 01 and 03 in parallel (03's
T1–T6 need no corpus), then 02/04, then 05/06/07 in parallel, then 08, then
09 (09-T1 — the emulator ring — is engine-only and can start any time).
**01–08 have landed (2026-07-24); 09 is next.** 07 shipped `libasmspy` (the
tracer engine as a linkable tier), `asmspy --serve` and its normative protocol,
and the desktop's live capture host; 08 shipped the seven live views over those
sessions, the `codeimage` kind (defined in the schema, produced by `--serve`,
consumed by a bytes-as-of-trace-time pane) and the PT-replay slice. One schema
consequence worth carrying into the Phase-3 freeze: `codeimage` is the first
reserved kind to be **defined**, and it pairs with `stitch` by version — a
decoded PT path is only meaningful against the bytes it was decoded against. **Doc 10
is a growth-rung companion, not scheduled against Phase 1–4** — it consumes the
core docs' feeds and blocks nothing; start it only after 04/07/08 land, and only
its coarse rung is buildable before the Wave-1 `mem[]` stream.

## Plan phase mapping

Phase 1 = 01 + 02(T1–T4); Phase 2 = 03 + 04 + 05 + 06 + 02(T5–T6);
Phase 3 = 07 + 08 (+ the schema freeze checkpoint); Phase 4 = 09.
Doc 10 (the 3D overview) is a **growth-rung companion** outside the Phase 1–4
mapping — it reunifies the plan's killed Terrane + Observatory as an overview
surface, is gated on the Wave-1 `mem[]` stream for its rich rung, and is
prioritized only on demand from the RE/security and perf personas.
The plan's standing expansion-intake table stays in the plan; each wave item
lands as a new brief here in the same format.
