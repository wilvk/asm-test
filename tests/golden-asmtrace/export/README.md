# `tests/golden-asmtrace/export/` — exporter fixtures (hand-authored)

Unlike the flat `*.asmtrace` files in the parent directory, **nothing here is
generated**. These recordings are written by hand so that
[`tools/asmtrace_export.c`](../../../tools/asmtrace_export.c) has an input for
every shape it must handle — including the ones no producer emits (a mixed
address basis, a newer format major, a compressed container) and the ones that
are only interesting because they are *less* than they look (truncation, lost
samples, a statistical survey). `make asmtrace-golden` writes only flat
`*.asmtrace` files into the parent and never descends into this directory, and
`make asmtrace-golden-check` compares only those flat files, so regenerating the
corpus leaves these untouched.

The `*.speedscope.json` / `*.chrome.json` / `*.dot` / `*.info` files beside them
are the **expected outputs**, byte-compared by
[`scripts/test-asmtrace-export.sh`](../../../scripts/test-asmtrace-export.sh)
(`make asmtrace-export-test`). Regenerate them with
`UPDATE_GOLDEN=1 sh scripts/test-asmtrace-export.sh` and review the diff —
speedscope and Chrome Trace are stable public formats, so a change in these
bytes is a change in what third-party viewers will render.
