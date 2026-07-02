# Batch H — test-suite-quality fixes (2026-07-02)

Findings #37, #38, #39 from `docs/analysis/2026-07-02-code-review.md`. AMD Zen 5 /
Linux x86-64. Each: validated against current code, fix applied, verified.

## #37 (Med) — expect.sh runs the negative suite unfiltered without --timeout

- **Validated:** Real. `tests/expect.sh:173,175` ran `$NEG` (`--jobs=4` / `-j4`)
  with no `--filter` and no `--timeout`, so `neg.timeout`'s infinite spin loop ran
  in both invocations until the runner's default 10s alarm. Baseline
  `make check` measured ~21s user / ~22s wall. With `ASMTEST_TIMEOUT=0` exported
  the alarm is never armed and the run hangs forever.
- **Changed:**
  - `tests/expect.sh` — appended `--timeout=1` to both unfiltered runs
    (`expect_fail_msg "-j4 still reports failures" ... --jobs=4 --timeout=1`,
    `expect_fail_re "-j4 contains a crash" ... -j4 --timeout=1`). `--timeout`
    overrides `ASMTEST_TIMEOUT`, so it bounds the spin and defeats the env-var
    hang. Added an explanatory comment.
  - `tests/negative.c:8-9` — updated the header contract to match: the harness may
    run the suite unfiltered *as long as it passes a bounded `--timeout`*.
- **Verified:** `make check` -> 36 passed / 0 failed, wall time ~4.3s (user 3.1s),
  down from ~22s. Hang gone: `timeout 30 env ASMTEST_TIMEOUT=0 sh tests/expect.sh`
  exits 0 (was 124), `36 passed, 0 failed`.

## #38 (Low) — expect.sh's --shuffle coverage is tautological

- **Validated:** Real. The sole `--shuffle` assertion ran the identical command
  twice (`--shuffle --seed=123`) and checked outputs are equal. The identity
  permutation (a no-op shuffle) passes it, so a dropped Fisher-Yates loop or
  regressed `--shuffle`/`--seed` parsing would not be caught.
- **Changed:** `tests/expect.sh` — kept the same-seed determinism check and added a
  second assertion: `--shuffle --seed=123` order must differ from the serial
  (registration-order) run. Seed 123 was verified to produce a non-identity
  permutation of the current 26-test positive suite, so this is deterministic (a
  fixed seed), not flaky. Added a note on picking another seed if the suite shrinks
  to make 123 map to identity.
- **Verified:** New assertion `ok 28 - --shuffle reorders vs serial (seed=123)`
  passes in `make check`. Confirmed out of band that seeds 1..30 (and 123) all
  reorder vs serial, so the chosen seed is not a knife-edge.

## #39 (Low) — test_drtrace.c 'coverage accumulates' check can never fail

- **Validated:** Real. `examples/test_drtrace.c:109` asserted
  `asmtest_emu_trace_blocks_len(tr) >= blocks_before` — `>=` on a
  monotonically-non-decreasing counter can never fail, even if the second traced
  call recorded nothing. The discriminating fact (the dec-path block at offset
  `0xe`, documented at `:43-44`) was never asserted covered.
- **Changed:** `examples/test_drtrace.c` — after the second call `fn(60,60)`
  (`120 > 100` -> dec path, unreached by the first `20+22` run) added
  `CHECK(asmtest_trace_covered(tr, 0xe), "re-running covers the dec-branch block
  (offset 0xe)")` and tightened the accumulation check from `>=` to strict `>`.
  Signature confirmed: `int asmtest_trace_covered(const asmtest_trace_t *, uint64_t)`
  (`include/asmtest_trace.h:85`, `src/trace.c:135`), which matches a recorded block
  by its start offset — so `0xe` discriminates the dec block precisely.
- **Verified:** Needs the drtrace tier (DYNAMORIO_HOME). Host has no local DR
  (`make drtrace-test` self-skips: `# SKIP: DynamoRIO not found`). Ran it under
  real DynamoRIO via `make docker-drtrace`: the C harness reports
  `ok 8 - re-running covers the dec-branch block (offset 0xe)` and
  `ok 9 - re-running the region accumulates coverage`, `18 passed, 0 failed`; the
  Python drtrace/drgate suites (3 + 4) also pass.

## Files modified

- `tests/expect.sh` (#37, #38)
- `tests/negative.c` (#37, comment)
- `examples/test_drtrace.c` (#39)
