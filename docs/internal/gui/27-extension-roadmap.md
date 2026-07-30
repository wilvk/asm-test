# Extension roadmap: unblocking the deferred GUI views — overview

> **What this is.** A family overview (like [11-imgui-addons.md](11-imgui-addons.md)
> for its addon family), not a brief. It maps every DEFERRED / BLOCKED / REFUSED
> capability in the desktop GUI onto a small set of **root prerequisites** — one
> engine or `.asmtrace`-schema change apiece — and each root gets a full
> implementation brief ([28](28-schema-freeze-completion.md)–[33](33-backward-attribution-producers.md)).
> The point of the mapping: the ~30 "not day-one / acknowledged-limit / Wave N / future
> work" markers scattered across docs 01–26 and
> [desktop-gui-plan.md](../plans/desktop-gui-plan.md) are **not** 30 independent
> problems. They collapse onto six roots. Unblock a root, and several stuck views
> light up at once — most of them with **zero desktop change**, because the
> consumers already degrade gracefully and flip live when the producer appears (the
> doc-25/26 pattern).
>
> **Authored 2026-07-28**, verified against HEAD `da566c9`; every cited file:line
> re-verified against the tree. If a claim disagrees with the code when you
> implement, the code wins — re-verify, then fix the doc in the same change.

## Why this work exists

The GUI was built faithful: where a producer does not carry a fact, the view says
so instead of faking it (D7). That discipline generated a large, precise
inventory of gaps — each `absent` placard, each `TORN` banner, each "not a
day-one feature" is a pointer at a missing producer or schema field. This
roadmap turns that inventory into an ordered build plan.

Three things must **not** be planned — they already shipped, and a brief would
duplicate landed work:

- **Live `regstate` producer** ([26](26-live-regstate-producer.md), ✅ 5/5, landed
  2026-07-28): the Scrubber goes live under `--dataflow --steps` / serve
  `steps:true`. The offline register ring + Scrubber ([09](09-teaching-producers.md))
  landed 2026-07-26.
- **Graded-fidelity `severity` schema field** ([23](23-graded-truth-layer.md) T1,
  landed 2026-07-27) — the one Phase-3-freeze schema item already **closed** with
  01-owner sign-off ([asmtrace-schema.md](asmtrace-schema.md) owner sign-off note).
- **Live model wiring** ([25](25-live-model-wiring.md), ✅ 7/7) — Loom / Slice /
  Timeline / 3D go live; the last tail, the per-tid trajectory overlay (T6 →
  [10](10-spacetime-3d-overview.md) T5), **landed 2026-07-28** (`build_trajectories`
  weaves the live single-step `df_step` offset stream as a region-relative,
  per-tid path). The 3D-render-tail row below is retired.

## The six roots

| # | Root prerequisite | Brief | Size | Views it unblocks |
|---|---|---|---|---|
| **R1** | **Schema freeze-completion** — `code` header (routine hash), footer `steps_total`, serialize the `wide[]` operand buffer | [28](28-schema-freeze-completion.md) | S–M | diff refusing wrong-routine pairs (04 T6); N-of-M truncation banners (05 T2); `df_step` `[wide]` → bytes (streams) |
| **R2** | **`mem[]` address-stream producer** — the reserved `mem` kind, no producer today | [29](29-mem-address-stream.md) | M–L | 3D "rich rung" (10); secret-class CT-invariance multiverse (05 growth-rung); misaligned / uninit-read views |
| **R3** | **Resume-from-state seam + Reweave** — route the value producer through the existing `emu_snapshot`/`emu_restore` | [30](30-resume-from-state-and-reweave.md) | L | synthesize-per-step-registers ("not a day-one feature"); edit-at-step-K counterfactuals; mid-run state editing; Loom fork-from-step-K |
| **R4** | **Wide / FP / vector register deck** — `fpenv` kind + XMM/YMM/MXCSR capture + SSE-class arg marshalling | [31](31-wide-register-deck.md) | L | FP-env / MXCSR panel (Wave 1); vector lanes in the Scrubber; FP/vector-arg corpus routines |
| **R5** | **Per-guest value producer** — `dataflow_emu.c` is arch-hardwired to x86-64; arm64 first | [32](32-per-guest-value-producer.md) | L | arm64 value fabrics; arm64 Author-mode run/trace; partial N-ISA braids |
| **R6** | **Backward-attribution producers** — the reserved `blame` + `statediff` kinds; sockets already built | [33](33-backward-attribution-producers.md) | M–L | blame cones (04/09); slice-diff / two-recording state-diff rendering (04) |

## The dependency graph

```
R1 (freeze: identity + totals + wide-operand)
 ├─ prerequisite for → R6 (statediff needs routine identity to pair two recordings)
 ├─ shares wide[] plumbing + descriptor mechanism with → R4
 └─ closes 04-diff / 05-banner fidelity gaps directly

R2 (mem[] producer) ── shared prerequisite for THREE views (10 rich rung, 05 CT multiverse, misaligned)

R3 (resume-from-state) ── the headline; keystone already exists on emu_t (snapshot+ring),
                          the value producer just bypasses it. Enables the "not a day-one" fallback.

R4 (wide register deck) ── builds on R1's wide[] serialization; descriptor on-ramp (vec512_t) already laid

R5 (per-guest producer) ── independent axis (arch), demand-gated

R6 (blame + statediff) ── blame is single-recording; statediff pairs two → needs R1 identity
```

Two structural facts make this cheaper than it looks:

- **The consumers are already inert-ready.** The 3D rich rung is gated at runtime
  on the `mem` kind being present and stays inert until it appears
  ([terrain.cpp:124-130](../../../desktop/src/space/terrain.cpp#L124)); the diff
  states routine identity is the reader's assertion and will refuse a bad pair the
  moment a `code` header exists; the Scrubber keys on register field names and
  extends to a vector deck through the descriptor mechanism with no reader change.
  Most of R1/R2/R4 is **producer + schema only**.
- **The R3 keystone is already half-built.** `emu_snapshot` / `emu_restore`
  ([asmtest_emu.h:623-636](../../../include/asmtest_emu.h#L623)) and the per-step
  register ring ([asmtest_emu.h:601-610](../../../include/asmtest_emu.h#L601))
  already coexist on `emu_t` by design — snapshot/restore deliberately do not
  clear the ring. The value producer `asmtest_dataflow_emu_run` simply opens its
  **own** `uc_engine` ([dataflow_emu.c:257](../../../src/dataflow_emu.c#L257)) and
  never touches `emu_t`, so it cannot reach either. R3 is mostly a re-hosting job.

## Suggested sequence

1. **R1 first.** Cheapest (S–M), no new engine, and it is a prerequisite for R4
   and R6 and closes standing 04/05 fidelity gaps. It is literally "action the
   [freeze checklist](asmtrace-schema.md#known-v1-gaps--the-freeze-checklist)."
   Must coordinate with the **Phase-3 schema freeze** (D5) — 01 owns the schema.
2. **R2 and R6 in parallel** — both are new producers over an existing recording,
   independent of each other. R6's `statediff` leg waits on R1's `code` header.
3. **R4** — builds on R1's `wide[]` serialization; the descriptor on-ramp
   (`vec512_t` in the manifest, `fpenv` reserved) is already laid.
4. **R3** — the largest and the headline (it is what "not a day-one feature"
   gates), but lower urgency than the cheap fidelity wins. Phase-4+, demand-gated.
5. **R5** — independent arch axis; demand-gated on a non-x86-64 persona.

## The tail — smaller independents (no full brief; expand on demand)

These are real but do not cluster onto a shared root; several are gated on one of
the roots above and become small follow-ups once it lands.

| Item | Where noted | Gate |
|---|---|---|
| ~~Per-tid trajectory overlay (3D)~~ — **✅ landed 2026-07-28** (`build_trajectories` weaves the live single-step `df_step` offset stream as a region-relative, per-tid path) | [25](25-live-model-wiring.md) T6 → [10](10-spacetime-3d-overview.md) T5 | ~~per-tid stitched slice sets (exist); 3D render tail~~ (done) |
| `--lcov-source` (source-line lcov) | [02](02-exporters-and-readers.md) T-exporters | the reserved `srcmap` kind producer |
| Offline `--graph` / `--procs` DOT regeneration | [02](02-exporters-and-readers.md) | Phase-3 record modes (07's snapshot kinds offline) |
| SARIF export | [02](02-exporters-and-readers.md) | Wave 4 |
| `zstd-frames` container reading | [asmtrace-schema.md](asmtrace-schema.md) (reserved, refused by name) | PT-scale container format (Phase 3, D1) |
| Darwin build of `libasmtest_dataflow` | [desktop-gui-plan.md](../plans/desktop-gui-plan.md) | fresh macOS Author-mode capture (replay already works everywhere) |
| Signed / notarized installers | [desktop-gui-plan.md](../plans/desktop-gui-plan.md) | bindings-registry credential path |

## Out of scope by design — a plan here would fight the design

The plan's **Acknowledged limits** and **Killed in grounding** lists
([desktop-gui-plan.md:476-492](../plans/desktop-gui-plan.md#L476)) are not a
backlog — they are load-bearing refusals. Do not write briefs to "unblock" these:
exact-only fabric; trace-relative lifetimes; no cross-thread value hops; forks
never touch a live process; provenance starts at instrumentation; statistical
absence proves nothing; the engine cannot produce relaxed (litmus) outcomes;
IBS `mode:"auto"` cannot attribute a tid. One killed item — **producer-side
timestamps** (the "live observatory") — is technically a small field but was
killed as *inverting the phasing*; reopening it is a product decision, not an
engineering one, so it stays out of this roadmap until that decision is made.

## Coordination notes

- **D5 (schema home).** R1, R4, and R6 all add or define schema rows. The schema
  is append-only until the Phase-3 freeze and **owned by 01**; land R1's freeze
  items through the freeze checkpoint, and land R4/R6 kinds as new registry-row
  definitions under the ignore-unknown-kinds rule (never a new envelope major).
- **D6/D7 (golden + fidelity).** Every new producer ships a golden fixture **and**
  a low-fidelity fixture (torn / dropped / absent), and the renderer test asserts
  the graceful-degradation placard — the same contract docs 05/09/10 already meet.
- **D9 (capture host).** Producer work splits by host exactly as today: the
  emulator L0 / valtrace tiers are linked by Author mode (R3, R5, the emulator
  legs of R1/R4); the ptrace `--dataflow` / `--serve` engine is `asmspy`-only (the
  live legs of R2/R4/R6). Keep the desktop app engine-free (D4).
