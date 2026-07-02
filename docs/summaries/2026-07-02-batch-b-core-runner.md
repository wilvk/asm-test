# Implementation summary — Batch B: core runner (findings #7–10)

*Source:* [2026-07-02 code review](../analysis/2026-07-02-code-review.md), findings 7–10.
*Validated:* x86-64 `make test` green; a purpose-built validation suite exercised
the four fixes and its `--format=junit` output parses as well-formed XML.

## #7 — SKIP() in SETUP/TEARDOWN reported as FAILURE (Medium)

`src/asmtest.c` `run_one` — the setup recovery point now captures the
`sigsetjmp` return code and returns `ST_SKIP` when it is `JMP_SKIP` (the
GTEST_SKIP-in-SetUp idiom), `ST_FAIL` otherwise. The teardown recovery
distinguishes `JMP_SKIP` (downgrades a passing test to skip; leaves an existing
fail as fail) from a real teardown failure/crash.

*Evidence:* `SETUP(s){ SKIP(...); }` now yields `ok N # SKIP` for every test in
the suite; `TEARDOWN(s){ SKIP(...); }` marks the passing test skipped — both were
red before.

## #8 — asmtest_regs_vec_f32 unbounded index → OOB heap read (Medium)

`src/ffi.c` — added the missing upper-bound check
`(size_t)index >= sizeof r->vec / sizeof r->vec[0]`, matching every other
accessor in the file. Exercised: `vec_f32(16)` and `vec_f32(10000000)` now return
0 with no out-of-bounds read / SIGSEGV (previously read past the 336-byte
`regs_t`).

## #9 — JUnit output not XML-safe (Low)

`src/asmtest.c` `xml_print_escaped` — C0 control bytes other than tab/newline/CR
(illegal in XML 1.0 even as entities) are now emitted as a visible `\xNN` escape;
`\r` is escaped as `&#13;`, `\t` passed through. The `<failure>` body now routes
`r->file` through `xml_print_escaped` so a path containing `&`/`<` cannot break
the document.

*Evidence:* a failure message carrying `0x1B`/`0x01` produces `\x1B\x01` in the
report and the whole document parses as well-formed XML (was rejected before).

## #10 — test stdout precedes the JUnit `<?xml?>` declaration (Low)

`src/asmtest.c` main — in `--format=junit` mode the real `stdout` fd is
redirected to `/dev/null` for the entire run phase (forked, parallel, and
in-process children all inherit it), then restored before `render_junit`. Test
`printf`s can no longer land ahead of the XML declaration. `src/platform.h` gained
`#include <fcntl.h>` (POSIX) for `open`.

*Evidence:* a test that prints `STDOUT-FROM-TEST-BODY` no longer leaks that text
into the report; the document begins with `<?xml …?>`. (POSIX runner; the Win64
re-exec runner's console handling is a separate concern.)
