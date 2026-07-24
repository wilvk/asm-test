# `tests/golden-asmtrace/` — the `.asmtrace` golden corpus

The flat `*.asmtrace` files here are **generated**: `make asmtrace-golden` runs
[tools/asmtrace_record.c](../../tools/asmtrace_record.c) over the
conformance-corpus routines under the deterministic emulator L0 value producer
and writes one recording per routine (`df_step` + `df_edge` events, with
`disasm` where Capstone is linked). `make asmtrace-golden-check` regenerates
into a temporary directory and `diff`s against what is committed, so a change to
field order — or to the recorded values — fails as a byte diff instead of
passing quietly. Both are wired into `make cli-smoke`.

`dishonest/` is **hand-authored and never regenerated**. Each file encodes one
way a recording can be less than it looks — a filled buffer (`truncated`),
dropped and throttled samples (`dropped`), content withheld at record time
(`redacted`), and a producer that died mid-record (`torn`) — and each carries a
`note` event stating what a reader must conclude from it. They exist because
honesty in this format is a set of *fields*, and a field nothing ever reads is
not a guarantee: `cli/test_asmtrace.c`'s `fixture.*` checks replay these files
and assert each fact survives the reader, and the desktop viewer's tests replay
the same files to assert the banner / provenance chrome / redaction default.

**The `docker-cli` image is authoritative for regeneration.** Golden bytes
include Capstone disassembly text, and the image pins Capstone 5.0.1 while a
host's apt Capstone 4.x renders some instructions differently — regenerating on
such a host would produce a diff that is about the disassembler, not the code.
Regenerate with `make docker-cli` (or inside the image) and commit the result.
The recorder is also x86-64-only, because the corpus routines are host-arch
assembly; elsewhere the targets print the architecture gate and skip.
