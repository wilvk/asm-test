# The test runner

Each suite is a self-contained binary whose `main()` is provided by the
framework. It discovers the registered tests, runs them with isolation and
timeouts, and reports results. This page covers its command-line interface and
robustness model.

## Command-line interface

```sh
./build/test_arith                        # run every test, colored TAP output
./build/test_arith --list                 # list tests without running them
./build/test_arith --filter='*overflow*'  # run a glob-matched subset
./build/test_arith --shuffle --seed=123    # run in a reproducible random order
./build/test_arith --timeout=5             # per-test timeout in seconds (0 = off)
./build/test_arith --no-fork               # run in-process (no per-test isolation)
./build/test_arith -j4                     # run up to 4 tests at once (ordered)
./build/test_arith --format=junit          # JUnit XML instead of TAP, for CI
./build/test_bench --bench                 # time BENCH cases (see Benchmarks)
```

| Flag | Effect |
|---|---|
| `--list` | Print the test names and exit |
| `--filter=GLOB` | Run only tests whose `suite`, `name`, or `suite.name` matches the glob |
| `--shuffle` | Randomize test order (Fisher–Yates) to surface order dependencies |
| `--seed=N` | Seed for `--shuffle`; the chosen seed is printed for replay |
| `--timeout=SEC` | Per-test timeout (also `ASMTEST_TIMEOUT`); `0` disables it |
| `--no-fork` | Run tests in-process instead of one child per test |
| `-jN` | Run up to `N` tests concurrently (order preserved in the report) |
| `--format=tap\|junit` | Output format; TAP (colored) is the default |
| `--color=auto\|always\|never` | Colorize; `auto` (default) colors a tty and honors `NO_COLOR` |
| `--fail-fast` | Stop at the first failing test (forces serial; the TAP plan moves to the end of the stream, covering exactly what ran) |
| `--repeat=N` | Run the selection `N` times — with `--shuffle`/`--seed`, the flake-hunting loop |
| `--shard=K/N` | Run the `K`-th of `N` round-robin slices (1-based) — split one suite across CI jobs with no test lost or duplicated |
| `--fail-if-no-tests` | Exit nonzero when the selection is empty (e.g. a typo'd `--filter`) |
| `--record-dir=DIR` | Arm per-test `.asmtrace` recording into `DIR` (also `ASMTEST_RECORD_DIR`) — see [Record mode](#record-mode) |
| `--bench` | Run `BENCH` cases instead of tests — see [Benchmarks](benchmarks.md) |
| `--bench-format=text\|json` | Benchmark output: human text (default) or machine-readable JSON — see [Benchmarks](benchmarks.md) |
| `--help`, `-h` | Print the usage summary and exit |

(sec-record-mode)=

## Record mode

`--record-dir=DIR` (or `ASMTEST_RECORD_DIR`) arms recording: while it is set,
every test has a *recording path* of its own —
`<DIR>/<suite>.<name>.asmtrace` — and a failing test's report names whatever a
producer actually wrote there.

```console
$ ./build/test_ct_eq --record-dir=build/rec
...
not ok 2 - ct_eq.no_secret_dependent_branch
  ---
  at:  examples/test_ct_eq.c:142
  msg: |
    ASSERT_EQ(after, baseline): 5 != 4
  recording: build/rec/ct_eq.no_secret_dependent_branch.asmtrace
  ...
```

The `recording:` (and, when a producer supplies one, `step:`) keys are
**additive** YAML in the TAP failure block, and are appended to the `<failure>`
/ `<error>` element *text* in JUnit — never as new attributes, so an existing
report parser keeps working.

Three properties are deliberate:

- **The runner records nothing itself.** It links no engine. It arms a
  directory and carries a producer's note; a suite that already links the
  emulator tier calls `asmtest_rec_emu()`
  ([`include/asmtest_rec.h`](https://github.com/wilvk/asm-test/blob/main/include/asmtest_rec.h))
  and that is what writes the file. A suite with **no** producer glue accepts
  `--record-dir`, writes nothing, and emits **no** `recording:` key — the
  honest degrade, not a `recording: (none)` line nobody can open.
- **A directory that cannot be created is a hard failure** (exit 2). A run that
  was asked to record and silently recorded nothing is the exact outcome this
  flag exists to prevent.
- **A recording noted by a test that then crashes still reaches the report.**
  The note travels back over the same pipe as the failure message, so it
  survives a crash or a timeout. When the child dies *before* reporting, the
  synthesized verdict names no recording at all — the runner never guesses.

Two producer entry points make this work, both declared in `asmtest.h`:

```c
/* Non-NULL iff recording is armed AND a test is running. */
const char *asmtest_record_path(void);
/* "this test wrote this recording; the failing step is `step` (-1 = unknown)". */
void asmtest_note_recording(const char *path, long step);
```

The step id is `-1` in v1: "this test wrote this file", not "this step is to
blame". A viewer opens the last step when a failure carries no id.

## Isolation and robustness

By default **each test runs in its own `fork()`ed child** guarded by an
`alarm()` timeout. This is what lets the framework test deliberately hostile or
buggy code without falling over:

- An **infinite loop** hits the alarm and is reported as a timeout — the run
  continues with the next test.
- A **segfault / bus error** (`SIGSEGV`, `SIGBUS`, …) in the routine under test
  becomes a reported failure, not a dead runner.
- A **`SIGABRT`-class corruption** that kills the child before it can report is
  reconstructed by the parent from the `wait()` status
  (`killed by signal …` / `timed out … (killed)`).

The child ships its outcome (PASS/FAIL/SKIP plus message and location) back to
the parent over a pipe:

> **Diagram:** [Runner fork-per-test lifecycle](../reference/diagrams.md#runner-fork-per-test-lifecycle)

Run the demo to watch a hang and a crash get contained:

```sh
make demo-robust
```

### `--no-fork`

`--no-fork` runs tests in the same process. An in-process signal handler still
catches `SIGSEGV`/`SIGBUS` and still times out hangs via the same alarm, but a
`SIGABRT`-class crash takes the whole runner down — which is exactly why forking
is the default. Use `--no-fork` when you need a debugger to see the crash in the
test process, or in an environment without `fork()`.

### Parallelism

`-jN` runs up to `N` tests at once (each still in its own child). The report
preserves a stable order regardless of completion order, so output stays
deterministic.

## Guard-page buffers

For routines that write through pointers, the framework offers allocations backed
by `mmap` guard pages, so an out-of-bounds access **faults precisely** (and is
then caught and reported by the isolation layer above) instead of silently
corrupting memory:

- `asmtest_guarded_alloc(n)` / `asmtest_guarded_free(...)` — a trailing guard
  page, so a one-past-the-end write (`buf[n]`) faults.
- `asmtest_guarded_alloc_under(n)` / `asmtest_guarded_free_under(...)` — a
  leading guard page, so an underrun (`buf[-1]`) faults.

Combine these with [`ASSERT_MEM_EQ`](assertions.md#memory) to test both *what* a
routine writes and that it stays *in bounds*.

## Output formats

The default is **colored TAP** — human-readable and consumable by any TAP
harness. `--format=junit` emits suite-grouped JUnit XML for CI systems that
ingest it (Jenkins, GitLab, GitHub test reporters). See [CI & Docker](../reference/ci.md) for
how the project wires this into its own pipeline.
