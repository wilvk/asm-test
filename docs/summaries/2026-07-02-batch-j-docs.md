# Batch J — doc-accuracy fixes (findings #49–#54)

Date: 2026-07-02. Machine: AMD Zen 5 / Linux x86-64.

## #49 — quickstart claims auto-discovery of `examples/test_*.c` (High) — FIXED
Validated: `Makefile:50-57` defines `SUITES` as a hardcoded 14-entry list with a
hand-written link rule per suite (e.g. `Makefile:238 $(BUILD)/test_arith: ...`).
`grep 'SUITES +=' Makefile mk/*.mk` finds nothing — no wildcard discovery.
Changes:
- `docs/quickstart.md`: replaced the "the Makefile discovers `examples/test_*.c`
  … automatically" paragraph with instructions to append the binary to `SUITES`
  and add a matching link rule.
- `Makefile`: added a comment above `SUITES` noting suites are not auto-discovered
  and both the list entry and a link rule must be added.

## #50 — emulator.md AArch64 x0–x7 vs emu.c x0..x5 (Med) — SKIPPED (deferred)
Per coordinator instruction: NOT edited. The underlying code (#24) is being fixed
in another batch to marshal x0..x7, which makes the existing "x0–x7" statement in
`docs/emulator.md:150` correct as-is. Confirmed current `src/emu.c:637-651` still
marshals only x0..x5 (`arg_regs[6]`, loop `i < 6`); left the doc untouched so it
matches the incoming code fix.

## #51 — ASMTEST_SEED not read by --shuffle (Med) — FIXED (code)
Validated: `src/asmtest.c` shuffle seed derived only from `--seed`/`opt.seed` or
`time^pid`; the only `getenv("ASMTEST_SEED")` was in `asmtest_seed()` (property-
test RNG). Chose the doc's preferred wiring per the finding.
Change: `src/asmtest.c` shuffle block now falls back to `ASMTEST_SEED`
(decimal/0x-hex via `strtoull`) when `--seed` is absent, else `time^pid`.
`docs/api-reference.md:137` (`ASMTEST_SEED` … "and `--shuffle`") is now accurate
— no doc edit needed.
Verified (host build `make build/test_arith`):
- `ASMTEST_SEED=0x1234 ./build/test_arith --shuffle` → `# shuffle seed=0x1234`
  (reproducible across runs).
- `--seed=0x99` overrides the env → `# shuffle seed=0x99`.
- No env / no `--seed` → random per-run seed (varies).

## #52 — CHANGELOG stale `libasmtest_emu_full` / `shared-emu-full` (Low) — FIXED
Validated: `grep -rn 'shared-emu-full|libasmtest_emu_full' Makefile mk/` → none.
`shared-emu` (`Makefile:394-408`) links emu.o + assemble.o + disasm.o + unicorn +
keystone + capstone into `libasmtest_emu` (the superset), matching
`api-reference.md:163-173`. `make asm-test` (`Makefile:685`) builds a test suite,
not a separate lib.
Change: `CHANGELOG.md` — rewrote the Track C disassembly bullet (dropped
`libasmtest_emu_full`/`make shared-emu-full`, described the single superset
`libasmtest_emu` built by `make shared-emu`); fixed a second `libasmtest_emu_full`
reference in the same bullet; and rewrote the in-line-assembler bullets that said
the tier was "kept separate from `libasmtest_emu`" / bindings "self-skip against
the Keystone-free `libasmtest_emu`" to reflect that the assembler is folded into
`libasmtest_emu`.

## #53 — bindings.md inverts C++ tier gating (Low) — FIXED
Validated: `bindings/cpp/asmtest.hpp:22-27` guards emu/asm includes behind
`#ifdef ASMTEST_ENABLE_EMU` / `ASMTEST_ENABLE_ASM`, and the disas wrapper behind
`#ifdef ASMTEST_ENABLE_DISAS` — opt-in, default OFF. The doc's "compile in by
default … opt-outs" was inverted for C++.
Change: `docs/bindings.md:130` reworded — Zig compiles tiers in by default; the
C++ binding gates emu/asm/disas behind opt-in
`-DASMTEST_ENABLE_EMU`/`_ASM`/`_DISAS`.

## #54 — ci.md understates arm64 jobs (Low) — FIXED
Validated `.github/workflows/ci.yml`: `test` (os incl. `ubuntu-24.04-arm`, L33),
`emu` (L161), `asm` (L186), and `package-libs`/payloads (L446) all run on
`ubuntu-24.04-arm`. Doc said only `test`+`emu`.
Change: `docs/ci.md:42` now lists `test`, `emu`, `asm`, plus the `payloads`
(`package-libs`) leg, and adds `make docker-asm DOCKER_PLATFORM=linux/arm64`
(target confirmed at `mk/docker.mk:46`).

## Files modified
- `docs/quickstart.md`
- `Makefile`
- `src/asmtest.c`
- `CHANGELOG.md`
- `docs/bindings.md`
- `docs/ci.md`
- `docs/summaries/2026-07-02-batch-j-docs.md` (this note)
