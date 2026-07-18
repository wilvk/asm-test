# Triage and fix the 2026-07-02 review's still-present 'plausible' findings — implementation

> **Sources.** Actioned from
> [2026-07-02-code-review.md](../analysis/2026-07-02-code-review.md) (the
> "Plausible (one verifier upheld, one dissented)" section, lines 837–850).
> Written 2026-07-17. If this doc and a source disagree, this doc wins (sources
> may be stale); if the CODE and this doc disagree, re-verify before
> implementing.

## Why this work exists

The 2026-07-02 adversarial code review produced 54 confirmed findings, which
were all remediated, plus 9 "plausible" findings that survived one verifier but
not both — and that section was never triaged. Code reads on 2026-07-17 show
two of the nine were independently fixed since, and the other **seven are still
present**: a parallel-runner failure path that reports never-run tests as
PASSED, a `--filter` glob matcher that selects different tests on Win64 than on
POSIX, a guard-page allocator that hands back a pointer into its own guard page
for sizes near `SIZE_MAX`, signed-overflow UB in the fuzzer's nudge mutation, a
Zig conformance test that makes the AVX trampolines read past the end of a
stack array, a Windows crash handler that hijacks the test thread's stack when
any other thread faults, and a doc line that tells users `--emu` installs less
than it does. This doc turns each verified lead into a fix with a regression
test, then records the triage disposition in the review document itself.

## What already exists (verified 2026-07-17)

- [src/asmtest.c](../../../src/asmtest.c) — the framework runner.
  `run_parallel` (lines 1447–1518) with the non-EINTR `poll()` `break` at
  1485–1488; `round_up_page` + the guarded allocators at 508–560;
  `ST_PASS` is enum value 0 (line 28); the parallel path pre-zeroes
  `results[]` at 2382–2383; `reap_child` at 1326–1349 synthesizes results
  from wait status; `spawn_job` at 1402–1441 sets `r->suite`/`r->name` only
  for tests it actually launches.
- [src/glob_match.c](../../../src/glob_match.c) — the portable `--filter`
  matcher the Win64 runner uses instead of `fnmatch`
  ([src/platform.h](../../../src/platform.h) lines 15–44 shows the split:
  POSIX uses `fnmatch(pattern, str, 0)`, `_WIN32` uses `asmtest_glob_match`).
- [src/fuzz.c](../../../src/fuzz.c) — `emu_fuzz_cover1`'s corpus nudge at
  line 59 (`in = base + asmtest_rng_range(&rng, -4, 4)`). The repo's
  UB-avoidance idiom for exactly this problem is already written out in
  `asmtest_rng_range` itself ([src/asmtest.c](../../../src/asmtest.c)
  lines 614–631: do the arithmetic in `uint64_t`).
- [bindings/zig/src/conformance.zig](../../../bindings/zig/src/conformance.zig)
  — 2-element `vargs` arrays at lines 419 and 439; the C-side pattern to
  mirror is `ASM_VCALL256_2` in
  [include/asmtest.h](../../../include/asmtest.h) lines 933–941
  (`vec256_t asmtest_va_[8] = {(v0), (v1)};` — always 8 slots). The
  trampoline unconditionally loads `vargs[0..256)`
  ([src/capture.s](../../../src/capture.s) lines 1131–1138, `vmovdqu
  0(%rcx)…224(%rcx)`).
- [src/platform_win32.c](../../../src/platform_win32.c) — the `--no-fork`
  runner facility: process-global `rt_armed` (line 372), `rt_veh_cb`
  (395–415) which longjmp-redirects **whichever thread faulted** to
  `rt_landing` (389–393) and thence onto the single global
  `asmtest_win32_test_recover` buffer (line 368); `asmtest_win32_test_begin`
  (469–486). Contrast the correctly thread-scoped `asmtest_win32_guard`
  facility with `__thread tls_armed`/`tls_recover` (282–284, 320–343).
- [docs/guides/emulator.md](../../guides/emulator.md) line 23: `make deps
  DEPS_ARGS=--emu     # install libunicorn (and only that)` — contradicted by
  [scripts/install-deps.sh](../../../scripts/install-deps.sh) line 38 (usage:
  `--emu … unicorn + capstone + pkg-config`) and line 70 (the flag mapping).
  [docs/getting-started/installation.md](../../getting-started/installation.md)
  line 47 already carries the corrected phrasing to mirror.
- Already-fixed siblings from the same list (no task needed, recorded in T8):
  [src/drtrace_app.c](../../../src/drtrace_app.c) lines 69–75 now documents
  the `dr_lib_path`/`asmtest_dr_available` bare-soname divergence explicitly,
  and [src/drtrace_client.c](../../../src/drtrace_client.c) lines 208–221
  tracks begin/end depth unconditionally with a `truncated` flag past
  `MAX_DEPTH`.
- Test substrate: `make check` runs
  [tests/expect.sh](../../../tests/expect.sh) over the
  `tests_positive`/`tests_negative` meta-suites (`SELFTESTS`, Makefile line
  188; `check` rule lines 342–343); the Wine lane targets live in
  [mk/win64.mk](../../../mk/win64.mk) (`win64-filter-test` 97–102,
  `win64-guard-test` 70–75, `win64-seh-test` 106–111, `win64-check` 184–187,
  `docker-win64` 220–224); the sanitizer lane is `make sanitize` (Makefile
  614–630, emu tier included when libunicorn is present) with the Docker
  wrapper `docker-sanitize` ([mk/docker.mk](../../../mk/docker.mk) 66–67);
  the Zig lane is `zig-test` ([mk/bindings.mk](../../../mk/bindings.mk)
  157–161) / `docker-zig`, generated per-language by the `docker_lang_rule`
  template in mk/docker.mk (130–200; the image fetches a pinned Zig `0.13.0`
  tarball with per-arch SHA-256 checks, 154–172).

Prove the baseline is green before touching anything (host has Docker + make):

```sh
make docker-test        # builds the CI image, runs `make test && make check` — expect "N passed, 0 failed"
make docker-win64       # the Wine lane end to end — expect "win64 runner: … all verified" and exit 0
```

## Tasks

### T1 — Make `run_parallel` survive a non-EINTR `poll()` failure without reporting unrun tests as PASSED  (S, depends on: none)

**Goal.** A `poll()` failure other than EINTR in the parallel scheduler must
never produce a false-green result or a NULL suite/name in TAP output; every
selected test still runs and reports honestly.

**Steps.**
1. Open [src/asmtest.c](../../../src/asmtest.c) at `run_parallel` (1447).
   Today, `if (poll(...) < 0) { if (errno == EINTR) continue; break; }`
   (1485–1488) abandons the loop with children in flight and tests unspawned;
   `main` pre-zeroed `results[]` (2382–2383), `ST_PASS == 0` (line 28), so
   every abandoned slot is counted as a pass and `print_tap_result`
   (1528–1537) prints the NULL `suite`/`name` of never-spawned tests.
2. Replace the `break` with a degrade-to-blocking-reaps mode (poll is only an
   efficiency layer; reading each child's pipe in slot order is still
   correct). Add before the `while (done < n)` loop:
   `int poll_ok = 1;` and
   `int inject = getenv("ASMTEST_DEBUG_FAIL_POLL") != NULL;` (test hook, step 4).
3. Rework the poll block: when `poll_ok` is set, call poll (or simulate a
   one-shot failure when `inject` is set: `rc = -1; errno = ENOMEM;
   inject = 0;`); on `rc < 0 && errno != EINTR`, print one warning to stderr —
   `asmtest: poll failed (%s); falling back to blocking reaps` — and set
   `poll_ok = 0` instead of breaking. In the reap loop (1492–1513), when
   `poll_ok == 0` treat every in-flight slot as ready (`re = POLLIN;` instead
   of the `pfds` revents lookup); `read_full` then blocks per child until it
   writes or dies, `reap_child` (1326) already synthesizes crash/timeout
   results from wait status, and the outer loop keeps spawning the remaining
   tests. No other control flow changes.
4. The `ASMTEST_DEBUG_FAIL_POLL` env knob exists solely so the self-tests can
   reach this branch; document it in a comment as internal (not in `--help`).
5. Run `make test && make check` (or `make docker-test`), then the new
   expect.sh cases below.

**Code.** All in `src/asmtest.c` `run_parallel`; ~15 changed lines, no new
files, no API change.

**Tests.** Extend [tests/expect.sh](../../../tests/expect.sh) next to the
existing `-j4` block (lines 194–207):

```sh
# A poll() failure degrades to blocking reaps: same results, no false greens.
par_pf=$(ASMTEST_DEBUG_FAIL_POLL=1 "$POS" -j4 2>/dev/null | grep '^ok')
if [ "$par_pf" = "$serial" ]; then ok "-j4 survives poll failure (degraded reaps)"; else bad "-j4 survives poll failure (degraded reaps)"; fi
expect_contains "poll-failure warning printed" "falling back to blocking reaps" env ASMTEST_DEBUG_FAIL_POLL=1 "$POS" -j4
expect_fail_msg "-j4 still fails honestly under poll failure" "ASSERT_EQ" env ASMTEST_DEBUG_FAIL_POLL=1 "$NEG" --jobs=4 --timeout=1
```

Pre-fix, the first case fails (truncated/false-green `ok` lines, `(null)`
names); post-fix all three pass inside `make check`'s `# N passed, 0 failed`
summary.

**Docs.** Append a bullet under the existing `### Fixed` header in the
`## [Unreleased]` section of [CHANGELOG.md](../../../CHANGELOG.md) (that header
is at line 979; `## [Unreleased]` already carries `### Added`, `### Changed`,
and `### Fixed` subsections, so do not add a new header): "parallel runner
(`-jN`): a non-EINTR `poll()` failure no longer abandons the run and reports
never-run tests as passed; the scheduler degrades to blocking reaps."

**Done when.**
- `make docker-test` passes with the three new expect.sh cases counted.
- `ASMTEST_DEBUG_FAIL_POLL=1 build/tests_positive -j4` output is
  byte-identical (in `^ok` lines) to a serial run and exits 0.
- Reverting only the src change makes the new expect.sh cases fail.

### T2 — Bring `glob_match` to fnmatch(flags=0) parity: unterminated `[`, in-class escapes, trailing backslash  (M, depends on: none)

**Goal.** `asmtest_glob_match` agrees with POSIX `fnmatch(pattern, str, 0)` on
every pattern the runner's `--filter` can receive, so a filter selects the
same tests on Win64 as on Linux/macOS.

**Steps.**
1. Open [src/glob_match.c](../../../src/glob_match.c). Three divergences
   (matcher vs. glibc/POSIX `fnmatch` with `flags=0`):
   - `match_class` (10–42) parses to end-of-string when a class has no
     closing `]`, treating the remainder as members; fnmatch treats an
     unterminated `[` as a **literal** character.
   - `match_class` treats `\` inside a class as an ordinary member; fnmatch
     without `FNM_NOESCAPE` honors escaping there (`"[\\]]"` matches `"]"`).
   - A lone trailing `\` (where `pattern[1] == '\0'`) fails the escape branch
     (66–75) and falls through to the literal-character comparison
     (`else if (*pattern == *str)`, line 76), matching a literal backslash;
     POSIX says a pattern ending in an unescaped backslash **shall not match**.
2. Add a helper `static const char *class_end(const char *p)` — `p` points
   just past `[`; skip an optional `!`/`^`, then an optional leading literal
   `]`; scan forward honoring `\x` pairs; return the pointer to the closing
   `]` or NULL. In `asmtest_glob_match`'s `[` branch (55–65), call it first;
   on NULL fall through to the literal-character comparison (so `[` matches
   itself and backtracking still works).
3. In `match_class`, honor `\` escapes: an escaped character is a plain
   member (and a valid range endpoint); keep the existing range and negation
   logic otherwise.
4. In the escape branch, add: `if (*pattern == '\\' && pattern[1] == '\0')
   return 0;` before the literal fallthrough — and mirror the check in the
   trailing-pattern loop after `str` is exhausted (the existing tail already
   rejects it since `*pattern != '\0'`; verify with the test table).
5. Create `tests/glob_parity.c`: a table of `{pattern, str}` cases run
   through both `asmtest_glob_match` and the host's `fnmatch(p, s, 0)`,
   asserting `glob == (fnmatch == 0)` for each; print `ok/FAIL` per case and
   exit nonzero on any mismatch (mirror the style of
   [tests/win64/test_glob.c](../../../tests/win64/test_glob.c)). Include at
   minimum: every case already in `test_glob.c`; the divergent cases
   `("test[", "test[")`, `("a[bc", "a[bc")`, `("[\\]]", "]")`,
   `("a\\", "a\\")`, `("a\\", "a")`; plus `("[]", "[]")`, `("[!ab", "[!ab")`,
   `("*[", "x[")`, and escaped-range cases like `("[a\\-c]", "-")`.
6. Wire it into `check` in the [Makefile](../../../Makefile): add a build
   rule `$(BUILD)/tests_glob_parity: tests/glob_parity.c src/glob_match.c`
   compiled with `$(CFLAGS) -Isrc`, add it to `SELFTESTS` (line 188), and
   prepend `@$(BUILD)/tests_glob_parity` to the `check` recipe (342–343).
   POSIX-only is fine — `check` never runs in the mingw lane.
7. Extend `tests/win64/test_glob.c` with the same divergent cases,
   expectations hard-coded to the fnmatch answers (Wine lane has no fnmatch).
8. Run `make check` (host or `make docker-test`) and `make docker-win64`
   (which runs `win64-filter-test`, mk/win64.mk 97–102).

**Code.** `src/glob_match.c` (~35 lines changed/added), new
`tests/glob_parity.c`, Makefile check wiring, `tests/win64/test_glob.c`
additions. Update the header comment in
[src/glob_match.h](../../../src/glob_match.h) ("matching fnmatch with flags 0")
to state parity now includes unterminated-`[`, in-class escapes, and the
trailing-backslash non-match rule.

**Tests.** As in steps 5–7. Pre-fix, `tests_glob_parity` fails on the
divergent rows; post-fix `make check` and `make docker-win64` are green.

**Docs.** CHANGELOG `### Fixed`: "`--filter` on Win64: the portable glob
matcher now matches POSIX `fnmatch` on unterminated `[`, backslash escapes
inside classes, and trailing backslashes." No guide changes —
[docs/guides/win64.md](../../guides/win64.md) already describes `--filter` as
fnmatch-equivalent, which this makes true.

**Done when.**
- `make check` runs `tests_glob_parity` and passes.
- `make docker-win64` passes including the extended `win64-filter-test`.
- Reverting only the matcher change makes both fail on the new cases.

### T3 — Reject guard-page allocations that overflow the page rounding (POSIX and Win32 twins)  (S, depends on: none)

**Goal.** `asmtest_guarded_alloc`/`_alloc_under` return NULL for any `n` whose
page-rounding would wrap, instead of returning a pointer into the guard page.

**Steps.**
1. In [src/asmtest.c](../../../src/asmtest.c) `round_up_page` (508–511):
   `(n + pg - 1)` wraps for `n > SIZE_MAX - pg + 1`, `usable` collapses to
   `pg` via the `== 0` branch, the 2-page mmap succeeds, and
   `base + (usable - n)` (line 526) wraps into the `PROT_NONE` page. Change
   `round_up_page` to return 0 on overflow: at the top,
   `if (n > SIZE_MAX - 2 * (size_t)pg) return 0;` (conservative: also leaves
   room for the `usable + pg` guard-page addition in the callers, and no such
   allocation could ever succeed anyway). Keep the `usable == 0 ? pg :
   usable` mapping for `n == 0`.
2. In all four callers (`asmtest_guarded_alloc` 513, `asmtest_guarded_free`
   529, `asmtest_guarded_alloc_under` 538, `asmtest_guarded_free_under` 554):
   after `usable = round_up_page(n, pg);` add `if (usable == 0) return
   NULL;` (allocs) / `return;` (frees — defensive; a wrapped size can never
   have been allocated).
3. Apply the identical change to the Win32 twin in
   [src/platform_win32.c](../../../src/platform_win32.c): `round_up_page`
   (33–36) and its **three** callers — `asmtest_guarded_alloc` (40–55),
   `asmtest_guarded_free` (57–64), `asmtest_guarded_alloc_under` (68–83).
   Unlike the POSIX side, the Win32 `asmtest_guarded_free_under` (85–92)
   computes `base = p - pg` directly and never calls `round_up_page`, so it
   needs no change.
4. Run `make test && make check`, then `make docker-win64` for the Win32 side.

**Code.** ~12 lines across the two files; no API change (`NULL` on failure is
already the documented contract — mmap failure returns NULL today).

**Tests.**
- Host: add to [tests/positive.c](../../../tests/positive.c) (style: the
  existing `TEST(posit, …)` cases):

  ```c
  TEST(posit, guarded_alloc_rejects_size_overflow) {
      ASSERT_TRUE(asmtest_guarded_alloc((size_t)-1) == NULL);
      ASSERT_TRUE(asmtest_guarded_alloc((size_t)-4096) == NULL);
      ASSERT_TRUE(asmtest_guarded_alloc_under((size_t)-1) == NULL);
  }
  ```

  Pre-fix the first call returns a non-NULL pointer into the guard page (any
  dereference faults); post-fix all assertions hold under `make check`.
- Wine lane: add the same three `CHECK(asmtest_guarded_alloc((size_t)-1) ==
  NULL, …)` cases to
  [tests/win64/test_guard_win64.c](../../../tests/win64/test_guard_win64.c)
  (CHECK macro already there), exercised by `win64-guard-test`.

**Docs.** CHANGELOG `### Fixed`: "guard-page allocators return NULL instead of
a guard-page pointer for sizes within a page of `SIZE_MAX`." Internal-only
otherwise — the public API contract (NULL on failure) is unchanged.

**Done when.**
- `make check` passes with the new positive case; `make docker-win64` passes
  with the extended guard test.
- Reverting only the allocator change makes the new assertions fail (POSIX:
  non-NULL returned).

### T4 — Remove the signed-overflow UB in the fuzz corpus nudge  (S, depends on: none)

**Goal.** `emu_fuzz_cover1`'s nudge mutation is UB-free for any `lo`/`hi`,
including `LONG_MIN`/`LONG_MAX` boundary ranges, and the sanitize lane proves
it.

**Steps.**
1. In [src/fuzz.c](../../../src/fuzz.c) line 59, `in = base +
   asmtest_rng_range(&rng, -4, 4);` overflows (UB) when `base` sits within 4
   of `LONG_MAX`/`LONG_MIN` — which the fresh-draw path guarantees for
   boundary ranges. Mirror the unsigned idiom `asmtest_rng_range` itself uses
   ([src/asmtest.c](../../../src/asmtest.c) 614–631):

   ```c
   in = (long)((unsigned long)base +
               (unsigned long)asmtest_rng_range(&rng, -4, 4)); /* nudge */
   ```

   Unsigned wrap is defined; the existing clamp at lines 60–63 then folds any
   wrapped value back into `[lo, hi]`. Behavior for non-boundary ranges is
   bit-identical.
2. Add a boundary-range test (step below), then run `make docker-emu` and
   `make docker-sanitize` (Makefile `sanitize`, 614–630, includes `emu-test`
   under UBSan `halt_on_error=1` when libunicorn is present — it is in the CI
   image).

**Code.** One line in `src/fuzz.c` plus a comment citing the rng_range idiom.

**Tests.** Add to [examples/test_emu.c](../../../examples/test_emu.c) next to
`TEST(emu, fuzz_coverage_beats_fixed_vector)` (597–619), same fixtures:

```c
TEST(emu, fuzz_boundary_ranges_are_defined) {
    uint64_t blocks[16];
    emu_trace_t uni = {0};
    uni.blocks = blocks;
    uni.blocks_cap = 16;
    emu_fuzz_stat_t st;
    /* Ranges hugging LONG_MAX / LONG_MIN force nudges at the extremes; the
     * assertion is completion — UBSan (make sanitize) is the real oracle. */
    ASSERT_TRUE(emu_fuzz_cover1(E, CLASSIFY3, sizeof CLASSIFY3, LONG_MAX - 8,
                                LONG_MAX, 500, 0xC0FFEEULL, &uni, &st));
    uni.blocks_len = 0;
    ASSERT_TRUE(emu_fuzz_cover1(E, CLASSIFY3, sizeof CLASSIFY3, LONG_MIN,
                                LONG_MIN + 8, 500, 0xC0FFEEULL, &uni, &st));
}
```

(`<limits.h>` via the existing includes; add if missing.) Pre-fix,
`make docker-sanitize` halts with `signed integer overflow` in `fuzz.c`;
post-fix both `docker-emu` and `docker-sanitize` are green.

**Docs.** CHANGELOG `### Fixed`: "emulator fuzzing: corpus nudge no longer has
signed-overflow UB at `LONG_MIN`/`LONG_MAX` range extremes."

**Done when.**
- `make docker-emu` passes with the new test.
- `make docker-sanitize` passes (emu tier included, not skipped).
- Reverting only the fuzz.c change makes `docker-sanitize` fail on the new
  test with a UBSan `signed integer overflow` report.

### T5 — Give the Zig vec256/vec512 conformance tests full 8-slot vargs arrays  (S, depends on: none)

**Goal.** The Zig conformance suite passes 8-element vector-arg arrays to
`asm_call_capture_vec256/512`, eliminating the 192/384-byte stack over-read.

**Steps.**
1. [include/asmtest.h](../../../include/asmtest.h) documents (305–321) and
   [src/capture.s](../../../src/capture.s) implements (1131–1138 for ymm)
   trampolines that unconditionally load 8 vector-arg slots; the C macros
   always build `vec256_t asmtest_va_[8] = {(v0), (v1)};` (933–941). The Zig
   tests build 2-element arrays instead
   ([bindings/zig/src/conformance.zig](../../../bindings/zig/src/conformance.zig)
   lines 419 and 439).
2. Replace line 419 (`var varr = [_]c.vec256_t{ a, b };`) with:

   ```zig
   var varr: [8]c.vec256_t = std.mem.zeroes([8]c.vec256_t);
   varr[0] = a;
   varr[1] = b;
   ```

   and line 439 identically with `[8]c.vec512_t`. This mirrors the C macro
   (unused slots zero-initialized). No other changes — `&varr` still decays
   to the C pointer.
3. Run `make docker-zig` (the generated per-language lane in
   [mk/docker.mk](../../../mk/docker.mk) 130–200, driving `zig-test` from
   [mk/bindings.mk](../../../mk/bindings.mk) 157–161).

**Code.** Six lines in `bindings/zig/src/conformance.zig`.

**Tests.** The existing tests ARE the test — `vec256.add4d (AVX2)` and
`vec512.add8d (AVX-512)` already assert the captured lanes; the fix changes
what memory the trampoline reads, and the assertions (`out[0].f64[i]`) still
hold because slots 0–1 are unchanged. The over-read itself has no in-tree
detector (Zig stack arrays, no ASan interop in this lane), so the manual
verification is: `make docker-zig` passes on an AVX2 runner both before and
after, and the diff review confirms 8 slots. State this honestly in the commit
message.

**Docs.** Internal-only, no user-facing docs — a test fixture correction, no
API or behavior change. CHANGELOG `### Fixed` one-liner: "Zig conformance:
vec256/vec512 capture tests no longer under-fill the 8-slot vargs array."

**Done when.**
- `make docker-zig` passes (vec256 test executes on an AVX2 host; vec512
  self-skips without AVX-512F — both outcomes acceptable).
- `grep -n "\[8\]c.vec256_t" bindings/zig/src/conformance.zig` and the vec512
  twin both match.

### T6 — Scope the Win32 `--no-fork` VEH crash handler to the test thread  (M, depends on: none)

**Goal.** A fatal exception on any thread other than the test thread is no
longer redirected through the runner's global recovery buffer (which longjmps
the faulting thread onto the test thread's stack); only the armed test
thread's own faults are converted to test failures.

**Steps.**
1. In [src/platform_win32.c](../../../src/platform_win32.c): the runner
   facility's `rt_armed` (372) is process-global and `rt_veh_cb` (395–415)
   checks only `is_fatal_exc` + `InterlockedExchange(&rt_armed, 0)` before
   redirecting the **current** (faulting) thread to `rt_landing` (389), which
   `__builtin_longjmp`s on the global `asmtest_win32_test_recover` buffer —
   a buffer whose frame lives on the test thread's stack. The sibling
   `asmtest_win32_guard` facility already solves this with `__thread`
   state (282–284).
2. Add `static volatile DWORD rt_test_tid;` next to `rt_armed` (372). In
   `asmtest_win32_test_begin` (469), set `rt_test_tid =
   GetCurrentThreadId();` before `rt_armed = 1;`.
3. In `rt_veh_cb`, after the `is_fatal_exc` check but **before** the
   `InterlockedExchange(&rt_armed, 0)` consume (399), add:
   `if (GetCurrentThreadId() != rt_test_tid) return
   EXCEPTION_CONTINUE_SEARCH;` — a foreign-thread fault must not disarm the
   facility either. A declined fault then takes the OS's normal
   unhandled-exception path (process exit), the same honest outcome the
   forked mode gets from child death. The watchdog `rt_timer_cb` (419–467)
   needs no change — it already targets `rt_test_thread` explicitly.
4. Add `tests/win64/test_veh_scope_win64.c` with two argv-selected scenarios
   (CHECK-macro style of
   [tests/win64/test_seh_win64.c](../../../tests/win64/test_seh_win64.c)):
   - `main`: `asmtest_win32_test_begin(5000);` then
     `if (__builtin_setjmp(asmtest_win32_test_recover) == 0)` fault via a
     NULL write; on the recovery path CHECK
     `asmtest_win32_test_reason == ASMTEST_WIN32_REASON_CRASH`
     ([src/platform_win32.h](../../../src/platform_win32.h) line 71);
     `asmtest_win32_test_end();` print PASSED/FAILED, exit accordingly.
     (Regression guard: the tid gate must not break same-thread catches.)
   - `foreign`: `asmtest_win32_test_begin(0);` set the setjmp point; spawn a
     `CreateThread` worker that does an unguarded NULL write; `Sleep(10000)`
     on the main thread; if the sleep completes print `SURVIVED-FOREIGN-FAULT`;
     if the recovery path runs print `MAIN-HIJACKED`. Post-fix the correct
     outcome is process death inside the sleep (nonzero exit, neither marker).
5. Add a `win64-veh-scope-test` target to [mk/win64.mk](../../../mk/win64.mk)
   (mirror `win64-seh-test`, 106–111: same `$(WIN64_CC) … src/platform_win32.c
   tests/win64/test_veh_scope_win64.c` compile; `.PHONY` line). Recipe: run
   scenario `main` directly (exit code gates); then run scenario `foreign` as
   `timeout 30 $(WINE) … foreign > $(WIN64_BUILD)/veh_scope.out 2>&1 || rc=$$?`
   and **fail** the target if `rc` is 0 or 124 (timeout) or the output
   contains `MAIN-HIJACKED` or `SURVIVED-FOREIGN-FAULT`. Append the target to
   `win64-check` (184–187).
6. Run `make docker-win64`.

**Code.** ~6 lines in `src/platform_win32.c`; new test file; mk/win64.mk
target.

**Tests.** As in steps 4–5. Pre-fix, scenario `foreign` is undefined behavior
(two threads on one stack): observed as the `MAIN-HIJACKED` marker, a hang
(timeout, rc 124), or a corrupt crash after the marker — all of which the
recipe rejects. Post-fix it is a deterministic fast nonzero exit. Scenario
`main` passes before and after.

**Docs.** CHANGELOG `### Fixed`: "Win64 `--no-fork`: a fault on a non-test
thread no longer hijacks the test thread's recovery stack; it takes the normal
unhandled-exception path." [docs/guides/win64.md](../../guides/win64.md): if
it documents `--no-fork` crash containment, add one sentence that containment
covers the test thread only (forked mode contains all threads via child
death); skip if no such section exists.

**Done when.**
- `make docker-win64` passes with `win64-veh-scope-test` in `win64-check`.
- Reverting only the platform_win32.c change makes `win64-veh-scope-test`
  fail (marker printed or timeout).
- On this Mac host the lane self-skips only in the sense that `docker-win64`
  is x86-64-only (see Constraints); CI's x86-64 runner executes it.

### T7 — Fix the emulator guide's `--emu` install claim  (S, depends on: none)

**Goal.** [docs/guides/emulator.md](../../guides/emulator.md) stops telling
users that `make deps DEPS_ARGS=--emu` installs "libunicorn (and only that)".

**Steps.**
1. `--emu` maps to unicorn + capstone + pkg-config
   ([scripts/install-deps.sh](../../../scripts/install-deps.sh) line 70; its
   usage text at line 38 says so), with capstone deliberately deferred to
   `scripts/build-capstone.sh` on every package manager (lines 110–113,
   147–153). [docs/getting-started/installation.md](../../getting-started/installation.md)
   line 47 already states this correctly.
2. Edit line 23 of `docs/guides/emulator.md` to mirror it:

   ```sh
   make deps DEPS_ARGS=--emu     # unicorn + pkg-config (Capstone is source-built: scripts/build-capstone.sh)
   ```

3. Rebuild the docs: `make docker-docs` (Sphinx `-W`, fail-on-warning;
   [mk/docs.mk](../../../mk/docs.mk) 56–57).

**Code.** One line of Markdown.

**Tests.** No testable surface (a comment inside a fenced shell block).
Manual verification: `make docker-docs` exits 0 and the rendered
`guides/emulator.html` shows the new comment;
`grep -rn "and only that" docs --include='*.md'` (excluding `_build/` and the
review doc, which quotes the bug historically) returns nothing.

**Docs.** This IS the doc change. CHANGELOG `### Fixed`: "docs: the emulator
guide no longer claims `--emu` installs only libunicorn."

**Done when.**
- `make docker-docs` passes.
- The grep above is clean outside `docs/_build/` and
  `docs/internal/analysis/`.

### T8 — Record the triage disposition in the review document  (S, depends on: T1–T7)

**Goal.** The review's "Plausible" section carries a dated triage table so no
future sweep re-discovers these nine findings as unowned.

**Steps.**
1. In [docs/internal/analysis/2026-07-02-code-review.md](../analysis/2026-07-02-code-review.md),
   directly under the section intro (lines 837–840), insert a short
   "Triage (2026-07-17)" table: one row per finding →
   disposition. Seven rows point at this doc's tasks (T1–T7 by name and the
   fixing commit once landed); two rows record "independently fixed before
   triage": the `asmtest_dr_available` cascade comment
   ([src/drtrace_app.c](../../../src/drtrace_app.c) 69–75 now documents the
   bare-soname divergence) and the DR client begin/end depth desync
   ([src/drtrace_client.c](../../../src/drtrace_client.c) 208–221,
   unconditional depth + `truncated` flag).
2. Do not rewrite the findings themselves — they are the historical record;
   the table is additive.

**Code.** None.

**Tests.** None (internal doc). Manual verification: the table lists all nine
bullets from lines 842–850, each with a disposition.

**Docs.** Internal-only (docs/internal is excluded from the published Sphinx
build), so no `make docs` implications and relative links are fine.

**Done when.**
- Every one of the nine plausible findings has a disposition row.
- `make docker-docs` still passes (the file is not in the published set, but
  the tree must stay warning-clean).

## Task order & parallelism

T1–T7 are mutually independent and can be done concurrently by different
people; T8 lands last, after the others' commits exist to cite.

- T1 and T3 both edit `src/asmtest.c` (disjoint regions — trivial merge).
- T2, T3, and T6 all touch the Wine lane (`tests/win64/*`, `mk/win64.mk`) —
  coordinate the `win64-check` list edit in T6.
- Critical path: T6 (new lane target + UB-shaped pre-fix behavior makes it the
  slowest to validate) → T8.

```
T1  T2  T3  T4  T5  T6  T7   (parallel)
  \  |   |   |   |  /   /
           T8
```

## Constraints & gates

- **No new dependencies.** Every task uses toolchains already in the tree's
  Docker images (CI image, win64 image, zig image); nothing to pin.
- **Wine lane platform gate (T2/T3/T6).** `docker-win64` is x86-64-only — an
  x86-64 PE will not run under Wine in linux/arm64 emulation
  ([mk/win64.mk](../../../mk/win64.mk) 218–224). On an Apple-silicon host,
  rely on Docker's amd64 emulation of the Linux image where it works, or on
  the x86-64 CI runner; real-Windows behavior beyond Wine (T6's
  unhandled-exception exit path) is verifiable on any Windows box but Wine is
  the accepted in-tree oracle. Record "verified under Wine (docker-win64,
  linux/amd64)" in the T6 commit.
- **AVX-512 hardware gate (T5).** The vec512 conformance test runtime
  self-skips without AVX-512F (`asmtest_cpu_has_avx512f() == 0` →
  `error.SkipZigTest`); that is a legitimate hardware gate. The vec256 path
  runs on any AVX2 CI runner.
- **UBSan as oracle (T4).** The boundary-range test only *proves* the fix
  under `make docker-sanitize`; a plain `make docker-emu` run passes either
  way (the wrapped value is clamped). Both lanes must be run.

## Research notes (verified 2026-07-17)

No external research was needed; all facts are repo-verified. The one
standards-adjacent claim: POSIX `fnmatch()` (flags 0) treats a pattern ending
in an unescaped backslash as a non-match, an unterminated `[` as a literal,
and honors backslash escaping when `FNM_NOESCAPE` is unset — this doc pins
behavior to the runner's actual POSIX oracle, `fnmatch(pattern, str, 0)` on
the CI image's glibc, via the T2 parity harness rather than via a standards
citation, so any glibc/POSIX divergence resolves in favor of what
`--filter` actually does on Linux.

## Out of scope

- The 54 confirmed findings of the same review — all previously remediated;
  see [2026-07-02-code-review.md](../analysis/2026-07-02-code-review.md).
- The two refuted findings (clean-room `eval` interpolation, SECURITY.md
  1.0.x) — recorded as refuted in the review doc, no action.
- The two independently-fixed plausible findings (drtrace_app cascade
  comment, drtrace_client depth) — T8 records them; no code task here.
- DR/drtrace behavior work in general —
  [pin-xed-trace-tier.md](pin-xed-trace-tier.md) and
  [macos-dynamorio-port.md](macos-dynamorio-port.md) own that ground.
- Emulator-tier feature work (the fuzz fix here is UB-removal only) —
  [pin-sde-future-isa-lane.md](pin-sde-future-isa-lane.md) owns future-ISA
  execution.
- Runner CLI additions — [asmspy-cli-enhancements.md](asmspy-cli-enhancements.md)
  is unrelated (asmspy, not the test runner) but is the closest sibling a
  reader might confuse with T1.
