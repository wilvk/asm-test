# Live-feed completion roadmap — which visualizations still can't be driven from a live process

> **A family-overview follow-on** (like [27](27-extension-roadmap.md) for its
> extension family), from a 2026-07-29 audit of the whole desktop against the
> question *"can this visualization be produced from a **live** process, or only
> from a recorded/emulator golden?"* Authored after docs 36/37 anchored the live 3D
> plane. **Not a brief** — it maps the remaining closable gaps onto per-gap briefs
> (39+) and records the permanent gates so no one re-opens them.
>
> **Headline.** For a live **x86-64 attach** the pipeline is already close to
> comprehensive: Summary, Canvas, Slice (def-use), Timeline, Loom
> (fabric/biography/zeroization), the Scrubber (with `fpenv`), and the full Observer
> deck plus the 3D terrain/trajectory/HUD are all fed live off a growing recording
> ([25](25-live-model-wiring.md)/[26](26-live-regstate-producer.md) wired everything
> Observer wasn't; `--mem` lights the 3D rich rung; `--steps` arms the Scrubber
> ring; [36](36-anchor-the-3d-plane.md)/[37](37-region-tag-on-df-step.md) anchor the
> live 3D plane). The remaining **closable** live-feed gaps are a short list.

## Closable gaps (priority order)

**Verified 2026-07-29 against the code — TWO audit claims were over-stated and are
corrected below.** This roadmap is self-contained; cut a per-gap brief here (38+n)
when picking one up.

| # | Gap | Closes | Effort | Reality |
|---|---|---|---|---|
| L1 | live-dataflow serve leg on **arm64** (`cli/dataflow_ptrace.c` is entirely `#if __x86_64__`) — re-arm via `PTRACE_SYSCALL`-at-resume, **never** naive SINGLESTEP | Scrubber, live `df_step` 3D trajectory, value fabric, Loom/Slice/Timeline on **arm64 hosts** — a whole platform | L | **REAL, biggest unlock.** Load-bearing hazard: arm64 `PTRACE_SINGLESTEP` is detach-fatal (SPSR.SS) and qemu-user can't single-step. R5/doc 32 closed only the emulator/Author arm64 producer. |
| L2 | `decode_streams` becomes `df_invocation`-aware (mirror the Scrubber's `build_segmented_step_index`, `analysis/stepindex.cpp`) | per-pass Slice/Loom/Timeline on a **continuous** live capture (today they flatten passes to the last/conflated one) | M | **REAL degrade.** The producer already emits `df_invocation`-delimited passes (35 T1); only the Scrubber consumes them. Host-testable, no engine change. |
| L3 | the precomputed `blame` + `statediff` **KINDS** over the asmspy serve leg (R6/doc 33 landed them recorder/emulator-only) | reproducible/deep-linkable `blame`/`statediff` artifacts from a live attach | M | **LOWER value — the VIEWS already work live.** Corrected: the backward-cone *view* derives client-side over the live `df_edge` graph (`b`/`f` keys); the statediff *diff* is `dt_statediff_build` over two recordings' `regstate`, and a live recording is a valid leg. Only the *precomputed kinds* (a convenience) are recorder-only. |
| L4 | build `libasmtest_dataflow` on macOS/Darwin | Author-mode value-fabric capture on macOS | M | REAL (Author mode, produced not attached). |
| L5 | ARM32 + RISC-V run/trace in Author mode (another `df_guest` instance) | Author value fabric for ARM32/RISC-V guests | M | REAL, narrow audience. |
| L6 | [37](37-region-tag-on-df-step.md) T4 (`when`: bytes-live-at-step) on the serve/observer disasm path | Observer Disassembly disasm-at-`when` refinement (JIT/self-modifying targets) | M | **severable, deferred** — no view breaks (reader-rule-2 fallback is sound). |
| L7 | make `auto` reliably capture ([39](39-auto-capture-reliability.md)): a pure candidate walk (armed on BOTH samplers), empty-window retry, a settable sample window, `continuous` through a quiet region, and the session-lifecycle repairs | reliable live `auto` capture — kills the *"start + arm, it starts then stops, `refused: no session is running`"* complaint | S | **CLOSED (doc 39).** A PORTABLE fallback, not a hardware gate: the audit had recorded only the AMD-IBS *survey stream* as gated and missed that the region *pick* has a sw-clock fallback and is therefore closable. Proven by pure `test_autoregion` cases (no CI lane has AMD silicon) + serve/desktop smokes. |

**Corrected NOT-a-gap (audit over-claimed).** *"Carry `tid` on `df_step` to light
live 3D convergence"* is **not closable and not real**: the live dataflow engine is
**single-threaded by scope** (`src/dataflow_ptrace.c:59` — "a deterministic,
single-threaded routine"; it single-steps whichever thread first traps the entry, so
every `df_step` is one thread). Emitting `tid` would still yield one trajectory and
zero convergence marks. The Convergence layer is meaningful only for a **multi-thread
`trace`** recording, whose events already carry `tid` — and that already works live.
A live dataflow capture being one trajectory is a permanent scope fact, not a wire gap.

**Also corrected: the 3D `mem` rich rung is NOT inert.** Two stale in-code comments
(`terrain.cpp`, `trajectory.cpp`) claimed `mem` "has no producer yet"; R2/doc 29
landed the live `--dataflow --mem` / serve `mem:true` producer, so a live capture with
`--mem` lights the rich-rung data spurs. Fixed in the same pass as this roadmap.

**Sequencing.** L2 is the cleanest self-contained desktop win (host-testable, no
engine change) — do it first. L1 is the largest single unlock (a whole platform) but
L-effort and hazard-gated. L3's *views* already work live, so it is low priority
(precomputed-kind convenience). L4/L5 are Author-mode platform breadth. L6 is the
severable [37](37-region-tag-on-df-step.md)
tail — no view breaks without it (reader-rule-2 fallback stays sound).

## Permanent gates — recorded, NOT briefed

These are hardware or credential gates, or deliberate design refusals; a brief there
would fight physics or the design. They are marked so no one re-opens them.

- **AMD IBS survey / 3D statistical residency / Observer Hot-edges.** Serve `sample`
  needs AMD Zen IBS-Op silicon + `perf_event_paranoid`; a non-AMD host self-skips
  (`skip.code=2`). The sw-clock fallback exists only for `auto`'s region *pick*, not
  for a survey stream. (Validated live on the Zen 5 box — see
  [amd-hardware-validation](../amd-hardware-validation.md).)
- **Intel-PT observer + the PT-replay slice.** No serve mode captures PT/CoreSight;
  `ptslice` *replays* a recorded stitch path (replay needs no silicon, but *capturing*
  stitch needs Intel PT, and `stitch` has no v1 writer). Gated behind
  `ASMTEST_DESKTOP_HAVE_PT_REPLAY`.
- **Observer Watch.** Needs HW debug registers + privilege; the canonical honest
  refusal is arm64 advertising 4 slots yet returning `ENOSPC`. Widely available on
  x86-64; permanent where absent.
- **Signed installers / notarization.** Credential gate.
- **`auto`/IBS per-tid attribution.** A stated design refusal (statistical absence
  proves nothing), not a bug.

## Not real gaps (excluded deliberately)

- **Diff** and **ABI x-ray** are inherently two-recording / authored-pair artifacts;
  a single live process is at most one leg. [25](25-live-model-wiring.md) descoped a
  live-vs-file diff on purpose.
- **Backends** reads a committed features file, not trace data.
- **Loom Takes / reweave** are emulator-only **forever** by design
  ([05](05-loom-day-one.md)/[30](30-resume-from-state-and-reweave.md)) — forks never
  touch a live process.

## What "maximum from a live process" means after 36/37

On a live **x86-64** attach with the appropriate opt-ins, a single process can now
drive: the Summary, the trace Canvas, the Slice def-use explorer, the operand
Timeline (+ minimap), the Loom fabric/biography/zeroization, the register Scrubber
(+ wide `fpenv`), the 3D overview (terrain + anchored trajectory + HUD, with `--mem`
rich spurs), and the whole Observer deck (syscalls, watch, topology, tree, graph,
invocations, codeimage disassembly). The **closable** remainder is L1–L7 above; the
rest is physics.
