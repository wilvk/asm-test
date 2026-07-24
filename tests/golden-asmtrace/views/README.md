# `tests/golden-asmtrace/views/` — replay-view fixtures (hand-authored)

Like [`../dishonest/`](../dishonest/) and [`../export/`](../export/), and unlike
the flat `*.asmtrace` files in the parent, **nothing here is generated**.
`make asmtrace-golden` writes only flat `*.asmtrace` files into the parent and
never descends into a subdirectory, and `make asmtrace-golden-check` compares
only those flat files, so regenerating the corpus leaves these untouched.

These exist because the generated corpus cannot produce them. The recorder runs
the deterministic emulator L0 producer, which measures executed **steps** — so
it emits `trace` and `df_step`/`df_edge` events and, deliberately, no
`coverage`: block starts are not recoverable from an offset stream without
instruction lengths, and reconstructing them would be a guess wearing a
measurement's clothes. Nor can a clean deterministic run produce a truncated
buffer, a mixed address basis, or two runs that diverge. Each file below is one
such shape, and each carries a `note` event stating what a reader must conclude
from it.

| Fixture | What it pins |
|---|---|
| `loop-coverage.asmtrace` | heat > 1 on a loop body; the gutter marks the two **block** starts, not every executed instruction |
| `trunc-trace.asmtrace` | a filled trace buffer — `blocks_total`/`insns_total` exceed what is recorded, so the canvas banner names both numbers |
| `trunc-dataflow.asmtrace` | dropped operand records: steps 2 and 3 have no `df_step`, so their rows are blank (never offset 0) and cones are lower bounds |
| `mixed-basis.asmtrace` | one `rel` and one `abs` event in one stream — the canvas refuses with a placard and draws **no rows** |
| `no-disasm.asmtrace` | D10 absence: no `disasm` anywhere, so every view degrades to bare offsets and says so |
| `pair-a` / `pair-b.asmtrace` | the same routine down two branch arms: a three-instruction shared prefix, then patient zero at step 3 |

They are consumed by `desktop/test/test_canvas.cpp`, `test_timeline.cpp`,
`test_slice_view.cpp`, `test_diff.cpp` and `test_diff_view.cpp`, whose expected
dumps live under `desktop/test/expected/` and are byte-compared
(`UPDATE_GOLDEN=1 make desktop-test` regenerates them).
