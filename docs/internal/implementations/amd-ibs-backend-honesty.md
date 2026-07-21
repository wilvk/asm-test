# AMD IBS backend: honesty, record sizing, ABI guards, and the validation gate — implementation

> **Sources.** Actioned from
> [amd-review-followup-plan.md](../archive/plans/amd-review-followup-plan.md) (P1, P3,
> P6, P7, P8), the
> [2026-07-17 AMD hardware review](../analysis/2026-07-17-amd-hardware-review.md)
> (§2.3, §2.5, §2.6, §2.7, §4 row 3),
> [asmspy-plan.md](../plans/asmspy-plan.md) Theme E's REMAINING row (line 131),
> [amd-hardware-validation.md](../amd-hardware-validation.md) (the doc T2
> rewrites), and the archived
> [zen2-ibs-tracing-plan.md](../archive/plans/zen2-ibs-tracing-plan.md).
> Written 2026-07-17. If this doc and a source disagree, this doc wins (sources
> may be stale); if the CODE and this doc disagree, re-verify before
> implementing.

## Why this work exists

The AMD IBS statistical-sampling lane is the project's "honest gaps" tier — it
exists to say truthfully what it saw and what it could not. Today it lies in
four places: `ibs_probe` prints **AVAILABLE** on hosts where no sampling can
open; the ring-loss heuristic is sized ~10× too small for callchain-enabled
records, so samples can vanish with `lost==0 && throttled==0`; the exported
pure decoder trusts a register slot whose meaning depends on an undocumented
kernel append-order guarantee; and the one manual checklist that gates AMD
releases instructs a maintainer to shrug off a genuine regression. This doc
fixes all of it, plus two hygiene items (the header nobody installs, the
options fields nothing ever tested).

## What already exists (verified 2026-07-17)

The IBS lane is complete and landed (8/8 phases of the archived
[zen2-ibs-tracing-plan.md](../archive/plans/zen2-ibs-tracing-plan.md)):

- [include/asmtest_ibs.h](../../../include/asmtest_ibs.h) — the public surface:
  `asmtest_ibs_available` / `_skip_reason` / `_unavail_reason` (the
  errno-carrying reason, landed `bb76680`), the pure `asmtest_ibs_decode_op`,
  `_survey_pid` / `_survey_process` / `_survey_free`, block normalization, and
  the `asmtest_ibs_opts_t` additive-ABI options struct (`sample_period`,
  `flags`, `struct_size`, `period_jitter`).
- [src/ibs_backend.c](../../../src/ibs_backend.c) — one TU, two halves: a pure,
  every-platform section (decoder at `:100-131`, raw-layout macros at
  `:46-61`), and a `#if defined(__linux__) && defined(__x86_64__)` live-capture
  section (`:257` to `:1685`, stubs after). The probe (`ibs_probe()`,
  `:332-371`) is CPUID + sysfs only; `ibs_chan_open` (`:779` onward) records
  the real `perf_event_open` errno into `g_open_errno` (`:796`).
- [src/ibs_backend.h](../../../src/ibs_backend.h) — INTERNAL header (no ABI
  promise): the window primitives `asmtest_ibs_window_begin`/`_end` and the
  IBS-Fetch lane. `examples/test_ibs.c` and `examples/ibs_probe.c` already
  compile with `-Isrc` and include it
  ([mk/native-trace.mk:2094-2099](../../../mk/native-trace.mk)).
- [examples/ibs_probe.c](../../../examples/ibs_probe.c) — capability probe;
  [examples/test_ibs.c](../../../examples/test_ibs.c) — pure decoder tests +
  five live tests that self-skip off AMD/IBS.
- Make targets: `make ibs-test`
  ([mk/native-trace.mk:2104-2109](../../../mk/native-trace.mk)) runs
  `ibs_probe` then `test_ibs`; `make hwtrace-test` (`:2127-2134`) folds both
  in. Docker lanes: `make docker-hwtrace-ibs`
  ([mk/docker.mk:567-573](../../../mk/docker.mk)), `make
  docker-hwtrace-privileged` (`:584-590`, `--cap-add=PERFMON`, runs
  `hwtrace-test ibs-test`), and `make docker-cli-ibs`
  ([mk/cli.mk:390-395](../../../mk/cli.mk)) for the asmspy `--sample` smoke.
- The gate doc: [docs/internal/amd-hardware-validation.md](../amd-hardware-validation.md)
  — a single commit (`43066e0`), never revisited, three of its checks now
  wrong (see T2).

**Prove the baseline before touching anything** (from the repo root, on any
host, AMD or not):

```sh
make ibs-test              # expect: "== ibs-test ==", ibs_probe output, then test_ibs
                           # "1..N" with 0 "not ok"; off-AMD the live tests print
                           # "# SKIP IBS ..." lines (measured 2026-07-17: 39 ok, 6 skip)
make hwtrace-test          # expect: test_hwtrace plan green, then the same ibs output
make docker-fmt-check      # expect: silence (clang-format clean) — CI gates on this
```

**On formatting, read this first or the very first pre-flight step will mislead
you.** Canonical style is **clang-format 18** — CI installs and pins
`clang-format-18` and runs `make fmt-check CLANG_FORMAT=clang-format-18`
([.github/workflows/ci.yml:1082-1085](../../../.github/workflows/ci.yml)). But the
`Makefile`'s `fmt`/`fmt-check` default to an **unpinned** `CLANG_FORMAT ?= clang-format`
([Makefile:708](../../../Makefile)), so a newer host clang-format (e.g. 22.x) flags a
clean tree — dozens of files — as "drift". That is a version mismatch, **not** a broken
checkout, and running plain `make fmt` with it rewrites those files in a non-canonical
style CI then rejects. Always format through the pinned path: the Docker lanes
`make docker-fmt-check` / `make docker-fmt`
([mk/docker.mk:72,78](../../../mk/docker.mk)) bake in clang-format 18, or pass the pin
explicitly — `make fmt-check CLANG_FORMAT=clang-format-18` /
`make fmt CLANG_FORMAT=clang-format-18`.

Repo conventions the tasks touch: core rules live in `Makefile`, target groups
in `mk/*.mk` (edit targets where they live); format through the pinned path
`make docker-fmt` (or `make fmt CLANG_FORMAT=clang-format-18` — canonical style
is clang-format 18, see the baseline note above; plain `make fmt` with an
unpinned newer clang-format rewrites files CI will reject);
user-visible changes append under `## [Unreleased]` in
[CHANGELOG.md](../../../CHANGELOG.md); `docs/internal/**` is excluded from the
published Sphinx build. Per [CLAUDE.md](../../../CLAUDE.md), only **hardware**
and **credentials** justify a self-skip — here the only real gate is AMD Zen
silicon with the `ibs_op` PMU.

## Tasks

### T1 — Make `ibs_probe` and `make ibs-test` stop over-claiming availability  (S, depends on: none)

**Goal.** `examples/ibs_probe.c` prints **AVAILABLE** only after a real
`perf_event_open` succeeded, and every live-test EUNAVAIL skip line in
`examples/test_ibs.c` prints the real reason from
`asmtest_ibs_unavail_reason()` instead of a hardcoded guess.

**Steps.**
1. Read [examples/ibs_probe.c](../../../examples/ibs_probe.c). Today `main()`
   gates only on `asmtest_ibs_available()` (`:50`) — CPUID + sysfs — then
   prints `"IBS-Op statistical edge sampling: AVAILABLE"` (`:55`) and `"A
   live, out-of-band from->to edge survey is buildable on this host"`
   (`:70-71`). On a locked-down AMD host (perf blocked by
   paranoid/seccomp) that is **measured false**
   ([asmspy-plan.md:131](../plans/asmspy-plan.md)). Insert a real open attempt
   between the substrate check and the AVAILABLE print.
2. Do the same for the Fetch half: `report_fetch()` prints `"IBS-Fetch
   front-end coverage: AVAILABLE"` (`:39`) from
   `asmtest_ibs_fetch_available()` alone.
3. In [examples/test_ibs.c](../../../examples/test_ibs.c), replace the five
   hardcoded EUNAVAIL skip strings (`:355-357`, `:482-484`, `:539-541`,
   `:619-621`, `:677-679`) with the real reason.
4. `make ibs-test` off-AMD (unchanged output — the substrate skip path is
   untouched), then `make docker-fmt` (pinned clang-format 18 — see the baseline
   note; plain `make fmt` needs `CLANG_FORMAT=clang-format-18`).

**Code.**
- `examples/ibs_probe.c`: after the `asmtest_ibs_available()` gate, attempt a
  short real capture on the calling thread — the open/enable is what is under
  test, not sample density:

  ```c
  asmtest_ibs_survey_t s;
  int rc = asmtest_ibs_survey_pid(0, 50, NULL, &s);
  asmtest_ibs_survey_free(&s);
  if (rc == ASMTEST_IBS_EUNAVAIL) {
      printf("# IBS-Op: substrate present but sampling is BLOCKED — %s\n",
             asmtest_ibs_unavail_reason());
      /* ... still exit 0: a blocked host is a skip case, not a failure */
  }
  ```

  Only on `rc == ASMTEST_IBS_OK` print AVAILABLE and the "survey is buildable"
  lines. Mirror the shape in `report_fetch()` with
  `asmtest_ibs_survey_fetch_pid(0, 50, NULL, &fs)` (declared in the internal
  [src/ibs_backend.h](../../../src/ibs_backend.h), already included) —
  `asmtest_ibs_unavail_reason()` serves both lanes because `ibs_chan_open`
  records `g_open_errno` for every open ([src/ibs_backend.c:796](../../../src/ibs_backend.c)).
- `examples/test_ibs.c`: each EUNAVAIL branch becomes
  `printf("# SKIP IBS live capture: %s\n", asmtest_ibs_unavail_reason());`
  (keep each site's distinct prefix). The system-wide site (`:677-679`) keeps
  its "needs CAP_PERFMON / paranoid<=0" context and appends the reason.

**Tests.** `make ibs-test` is itself the test surface. Off-AMD: byte-for-byte
unchanged skip behavior (substrate reason). On a locked-down AMD host
(to exercise the blocked path, build the image once first — `make
docker-hwtrace-ibs`, or its `docker build -f Dockerfile.hwtrace … -t asmtest-hwtrace`
line ([mk/docker.mk:567-573](../../../mk/docker.mk)) — then run `docker run --rm
asmtest-hwtrace make ibs-test`; the `docker-hwtrace-ibs` target itself hardcodes
`--security-opt seccomp=unconfined` ([mk/docker.mk:570](../../../mk/docker.mk)) and
so cannot show the blocked path — the plain `docker run` is what does): `ibs_probe`
must NOT
print AVAILABLE and must print the EACCES/EPERM one-liner from
`asmtest_ibs_unavail_reason()` (the string at
[src/ibs_backend.c:393-396](../../../src/ibs_backend.c) names
paranoid/CAP_PERFMON/seccomp). A failure looks like: AVAILABLE printed with no
successful open (the pre-fix behavior).

**Docs.** CHANGELOG `### Fixed`: "`ibs_probe` and the `ibs-test` live skips now
attempt a real perf open and report the real refusal reason instead of
claiming AVAILABLE from the CPUID/sysfs substrate probe alone."

**Done when.**
- Off-AMD `make ibs-test` exits 0, all pure checks `ok`, skips print a
  non-empty reason.
- `grep -n "AVAILABLE" examples/ibs_probe.c` shows every AVAILABLE print
  reachable only after a successful open attempt.
- `grep -n "unavail_reason" examples/test_ibs.c examples/ibs_probe.c` hits all
  the sites above.
- On AMD (`make docker-hwtrace-privileged` on a Zen host): `ibs_probe` prints
  AVAILABLE via the real-open path and `test_ibs` live tests run (hardware
  gate — record the observed output; off-AMD the lane self-skips cleanly).
- Lands **in the same commit as T2** — the plan pairs them: each is the
  other's evidence.

### T2 — Repair the AMD validation checklist (the only AMD gate)  (S, depends on: T1 — same diff)

**Goal.** [docs/internal/amd-hardware-validation.md](../amd-hardware-validation.md)
stops inverting a regression signal, stops gating on a probe string T1 just
made non-vacuous, and records its own staleness hazard.

**Steps.**
1. Read the doc. Three defects, all verified in the working tree today:
   - `:91-113` — section "OPEN finding to watch — `call_auto` AMD-LBR rung"
     says the bug is "**not yet fixed**". It was fixed in `5d8e0d2`;
     [2026-07-12-zen5-privileged-lbr-findings.md](../analysis/2026-07-12-zen5-privileged-lbr-findings.md)
     §2 marks it `~~OPEN~~ RESOLVED`, deterministic across 16 privileged runs
     (every one escalates `backend=3 insns=77`).
   - `:124-126` — instructs treating `truncated=0` as "the known open finding,
     not a clean pass". Post-fix, that result must never occur: the stale text
     tells a maintainer to shrug off a genuine **regression**.
   - `:121-122` — gates on `ibs_probe` printing **AVAILABLE** and `test_ibs`
     passing `1..23` twice. The AVAILABLE gate was vacuous pre-T1, and the
     plan count is stale (currently 39 checks off-AMD; counts drift).
2. Rewrite those three places (edits below). Leave the rest of the doc (the
   lane table, prerequisites, run instructions) alone — including the `:116`
   "On an AMD Zen 3+/4/5 host" prerequisite line: its "Zen 3+" → "Zen 4+" floor
   correction is owned by
   [amd-branchsnap-lbr-docs.md](amd-branchsnap-lbr-docs.md) (the global "Zen 4+
   LBR floor" position), not this doc — do not touch it here.
3. `make docs` is not needed — `docs/internal/**` is excluded from the Sphinx
   build; this is an internal doc edit only.

**Code (doc edits).**
- Retitle `## OPEN finding to watch — call_auto AMD-LBR rung` to
  `## RESOLVED (5d8e0d2) — call_auto AMD-LBR rung`, cut the "not yet fixed" /
  "intermittent" body down to a short note linking the findings doc, and state
  the new expectation: case (b) drives 25 taken back-edges, a 16-deep window
  cannot hold them, so escalation **must** fire.
- Replace the `:124-126` checklist bullet with: "Inspect the
  `# call_auto escalate:` line: escalation MUST fire (`backend=3 insns=77`).
  A `truncated=0` here is a **REGRESSION** — file it and do not tag; it is
  not a known issue."
- Replace the `:121-122` bullet with a gate that cannot pass vacuously:
  "`test_ibs` reports its full plan (current count) with 0 `not ok` **and the
  live IBS tests actually ran** (no `# SKIP IBS live capture` /
  `# SKIP IBS whole-process capture` lines); `ibs_probe` reports AVAILABLE via
  its real-open path (T1); the `# whole-process:` line reads
  `3/3 worker functions covered` — record the observed count"
  (the print lives at
  [examples/test_ibs.c:500-504](../../../examples/test_ibs.c); the test's own
  assert is `>=2` for throttle robustness, the checklist records the
  observation).
- Append one meta-line to the checklist intro: "This checklist's own staleness
  is the failure mode it guards against: every item states the **observation
  to record**, never the expected verdict. If an observation contradicts an
  item, the item is stale — fix the doc in the same change that records the
  run."

**Tests.** No testable code surface (a markdown gate doc). Manual
verification: `grep -n "not yet fixed\|known open finding\|known false-green"
docs/internal/amd-hardware-validation.md` returns nothing;
`grep -n "REGRESSION" docs/internal/amd-hardware-validation.md` hits the new
bullet.

**Docs.** This IS the doc. CHANGELOG: covered by T1's entry (same commit); add
"the AMD manual-validation checklist no longer inverts the `call_auto`
regression signal" to it.

**Done when.**
- The three greps above pass.
- The doc treats `trace_call_auto` as FIXED everywhere (global position: on
  AMD validation, `truncated=0` where escalation must fire is a REGRESSION
  signal).
- Landed in the same commit as T1.

### T3 — Size `IBS_MAX_RECORD` for the callchain worst case; gate callchain out of the window lane  (S, depends on: none)

**Goal.** The near-full-ring loss heuristic bounds the true worst-case record
(~1.2 KB with callchain, not 112 bytes), and the window lane cannot enable
callchain at all.

**Steps.**
1. Read [src/ibs_backend.c:281-283](../../../src/ibs_backend.c):

   ```c
   /* Largest single record we parse: header + IP + TID + RAW(size + caps + 8 regs). */
   #define IBS_MAX_RECORD                                                         \
       (sizeof(struct perf_event_header) + 8u + 8u + 4u + IBS_RAW_MIN_BYTES + 16u)
   ```

   That is 112 bytes — the **pre-callchain** layout (`68b53850`);
   `PERF_SAMPLE_CALLCHAIN` (`a266b91`) never touched it. With callchain the
   kernel defaults `sample_max_stack` to 127 and a record reaches 1192 bytes
   (arithmetic in Research notes). The heuristic uses it at `:596`, `:1126`,
   `:1579`.
2. Pin the stack depth: in `ibs_fill_attr` (`:745-761`), when `cfg->callchain`
   is set, also set `a->sample_max_stack`. Without this no fixed bound is
   sound — the sysctl can be raised to 655360 on a tuned host.
3. Make the bound callchain-aware and thread it through `ibs_drain`.
4. Gate the window lane: `asmtest_ibs_window_begin`
   (`:1320-1350`) currently passes opts straight through
   (`ibs_cfg_from_opts(opts, &w->cfg);` at `:1333`). Clear callchain there,
   mirroring the fetch lane (`:1639-1646`), which already does exactly this.
5. Add the internal bound seam + tests (below), run `make ibs-test`,
   `make docker-fmt` (pinned clang-format 18; plain `make fmt` needs
   `CLANG_FORMAT=clang-format-18`).

**Code.**
- New macros next to `IBS_MAX_RECORD`:

  ```c
  /* Pin the kernel's default (PERF_MAX_STACK_DEPTH) explicitly: the sysctl is
   * tunable to 640K frames, so an unpinned open makes ANY fixed bound unsound.
   * On a host tuned BELOW this the open fails -EOVERFLOW — a loud, reported
   * failure (unavail_reason) instead of silent ring loss: the right trade. */
  #define IBS_CALLCHAIN_MAX_STACK 127u
  /* Worst-case callchain record: header+ip+tid (24) + (1+nr)*8 with
   * nr <= max_stack + 8 context markers + RAW wire 80 (9 regs, u64-padded)
   * + the same 16-byte slack as the base bound. */
  #define IBS_MAX_RECORD_CALLCHAIN                                             \
      (24u + 8u * (1u + IBS_CALLCHAIN_MAX_STACK + 8u) + 80u + 16u)
  ```

  (= 1208.) Correct the stale `:281` comment: the base bound stays 112 —
  still valid for every no-callchain lane, including a 9-register
  BrTarget+OpData4 record (true size 104; see Research notes).
- `static size_t ibs_max_record(int has_callchain)` returning
  `IBS_MAX_RECORD_CALLCHAIN` or `IBS_MAX_RECORD`; in `ibs_drain` (`:596`)
  replace `IBS_MAX_RECORD` with `ibs_max_record(has_callchain)` — the
  parameter already exists. `sw_drain` (`:1126`) and `ibs_fetch_drain`
  (`:1579`) keep the base bound (their lanes never enable callchain; the
  fetch lane clears it, `sw` records are 24 bytes).
- `ibs_fill_attr`: inside the existing `if (cfg->callchain)` block add
  `a->sample_max_stack = IBS_CALLCHAIN_MAX_STACK;`.
- `asmtest_ibs_window_begin`, after `:1333`:

  ```c
  /* No callchain in the window lane: no in-tree consumer decodes it, and a
   * callchain-sized record stream can silently overrun the single end-of-window
   * drain (the ring stays full -> loss with lost==0 && throttled==0). Mirrors
   * the fetch lane's cfg clear. See docs/internal/archive/plans/
   * zen2-ibs-tracing-plan.md Phase 5 and amd-review-followup-plan.md P3. */
  w->cfg.callchain = 0;
  ```

- Internal test seam in [src/ibs_backend.h](../../../src/ibs_backend.h)
  (Linux/x86-64 only, like the window primitives; document "INTERNAL test
  seam, no ABI"): `size_t asmtest_ibs_max_record(int has_callchain);` — a
  one-line wrapper over `ibs_max_record`.

**Tests.** Extend [examples/test_ibs.c](../../../examples/test_ibs.c) inside
its existing `#if defined(__linux__) && defined(__x86_64__)` section (a new
`test_record_bound(void)` called from `main`):
- `asmtest_ibs_max_record(0) == 112` (the base bound is unchanged — no
  regression for the fetch/sw lanes);
- `asmtest_ibs_max_record(1) >= 24 + 8*136 + 80` (= 1192 — the ABI worst case
  at the pinned depth: this is the callchain-sized `dsz` headroom assertion
  the plan demands; a pre-fix build returns 112 here and the check prints
  `not ok`);
- `asmtest_ibs_max_record(1) < 64 * 4096 / 4` (sanity: the bound leaves the
  256 KiB ring usable).
On AMD, the existing `test_live_phase5` (callchain-ON survey, `:588` onward)
still passes — the survey lanes still honor an explicit
`ASMTEST_IBS_OPT_CALLCHAIN`, now with a sound loss bound. Off this host the
new checks still run (they are pure arithmetic); only off-Linux/x86-64 they
are absent with the rest of the gated section.

**Docs.** CHANGELOG `### Fixed`: "IBS ring-loss heuristic now bounds the
callchain worst-case record (was 112 bytes, ~10× short — silent sample loss
with `lost==0 && throttled==0`); the internal window lane no longer opens
with callchain." T4 carries the header-text half.

**Done when.**
- `make ibs-test` green everywhere; new `test_record_bound` checks `ok`.
- `sed -n '1330,1340p' src/ibs_backend.c` shows the `w->cfg.callchain = 0;`
  clear with the comment.
- `grep -n "sample_max_stack" src/ibs_backend.c` hits `ibs_fill_attr`.
- On AMD (`make docker-hwtrace-privileged`): `test_live_phase5` passes live
  (hardware gate; elsewhere the callchain live path self-skips cleanly as
  today).

### T4 — Stop advertising an unwired callchain consumer in the public header  (S, depends on: T3)

**Goal.** [include/asmtest_ibs.h](../../../include/asmtest_ibs.h) no longer
promises "statistical call-graph context" for a knob whose output nothing in
the tree consumes.

**Steps.**
1. Read `include/asmtest_ibs.h:76-78`. The `ASMTEST_IBS_OPT_CALLCHAIN` comment
   promises "a frame-pointer caller stack per sample … for statistical
   call-graph context". Verified: no in-tree consumer decodes the callchain
   block — `ibs_sample_raw` ([src/ibs_backend.c:556-563](../../../src/ibs_backend.c))
   **parses past** it to reach RAW; the only users of the flag are the
   `test_live_phase5` live test and the deferral recorded in `a266b91`'s
   commit message ("the optional 5b asmspy `--graph` consumer is deferred").
2. Rewrite the comment. Keep the `#define` itself — removing it breaks
   source-compatibility for any external caller and the phase-5 test; the fix
   is honest labelling, not an ABI break.
3. `make ibs-test` (compile check), `make docker-fmt` (pinned clang-format 18;
   plain `make fmt` needs `CLANG_FORMAT=clang-format-18`).

**Code.** Replace the `:76-78` comment block with:

```c
/* attach a frame-pointer caller stack per sample (PERF_SAMPLE_CALLCHAIN +
 * exclude_callchain_kernel). HONESTY NOTE: no in-tree consumer decodes the
 * callchain — the drain parses PAST it to reach the RAW payload — so today
 * this flag buys no call-graph output; it costs ring bandwidth (records grow
 * to ~1.2 KB worst case) and a kernel get_callchain_buffers allocation that
 * can fail perf_event_open with ENOMEM. The internal window lane ignores it
 * (see ibs_backend.c). A future consumer is recorded in
 * docs/internal/archive/plans/zen2-ibs-tracing-plan.md Phase 5b. */
```

**Tests.** Compile-only surface: `make ibs-test` (the phase-5 test still
compiles and passes/skips). Manual check:
`grep -n "call-graph context" include/asmtest_ibs.h` returns nothing.

**Docs.** CHANGELOG `### Changed`: "`ASMTEST_IBS_OPT_CALLCHAIN` is documented
as consumer-less: it enables kernel-side capture only; nothing in the tree
decodes the stack, and the window lane ignores it." No user-guide change —
[docs/guides/tracing/hardware-tracing.md](../../guides/tracing/hardware-tracing.md)
never mentions the flag (verified by grep).

**Done when.**
- The grep above is empty; the new note is in place.
- The header cites the **archived** plan path
  (`docs/internal/archive/plans/zen2-ibs-tracing-plan.md`), not `plans/`.

### T5 — Validate the record's own caps word in the pure decoder; record the append-order dependency  (S, depends on: none)

**Goal.** `asmtest_ibs_decode_op` checks the payload's leading caps `u32`
(BrnTrgt, bit 5) before trusting `reg[7]`, the kernel append-order dependency
is written down where the old comment asserted the opposite, and the
RipInvalidChk read is gated on caps bit 7 — folding in the review's residual
hedge (§4 row 3 is refuted-impact, do-not-re-raise; this is hardening, not a
bug fix).

**Steps.**
1. Read [src/ibs_backend.c:85-131](../../../src/ibs_backend.c). Three facts:
   `ld_u32` is `__linux__ && __x86_64__`-gated (`:89-95`) because it *was*
   unused off-Linux (`-Werror=unused-function`); the decoder never reads the
   caps word (`IBS_RAW_REG_OFF` treats it as padding); the comment at
   `:105-106` says "A record shorter than that lacks BrnTrgt (or is
   truncated)" — the one inference the kernel disproves (a
   `BRNTRGT=0/OPDATA4=1` record is byte-identical in length, 68, to
   `BRNTRGT=1/OPDATA4=0`, with `reg[7] = IbsOpData4`).
2. Un-gate `ld_u32`: move it out of the `#if` next to `ld_u64` and delete the
   `:85-88` rationale comment (the decoder now calls it on every platform, so
   the unused-function hazard is gone — note that hazard was real: this file
   compiles on macOS/arm64 via `HWTRACE_OBJS`).
3. Add caps checks + the dependency comment to the decoder (below).
4. Add the IBSFFV (bit 0) check to `ibs_probe()` (`:332-371`), mirroring the
   OpSam/BrnTrgt checks at `:346-355`.
5. Update the decoder contract text in
   [include/asmtest_ibs.h:133-142](../../../include/asmtest_ibs.h) (EDECODE
   now also covers "caps say no branch-target register").
6. Extend the pure tests, run `make ibs-test` on this host (the pure section
   runs everywhere), `make docker-fmt` (pinned clang-format 18; plain `make fmt`
   needs `CLANG_FORMAT=clang-format-18`).

**Code.**
- Decoder, after the length check at `:107-108`:

  ```c
  /* The record's OWN caps word (a copy of the kernel's ibs_caps). Length alone
   * CANNOT identify reg[7]: the kernel appends IbsBrTarget (cap bit 5) then
   * IbsOpData4 (cap bit 10) positionally and cap-conditionally, so
   * BRNTRGT=0/OPDATA4=1 is byte-identical in length (68) to
   * BRNTRGT=1/OPDATA4=0 with a different reg[7]. Because BrTarget is appended
   * strictly FIRST, bit 5 set forces reg[7] = IbsBrTarget — verified against
   * Linux v6.12 arch/x86/events/amd/ibs.c:1084-1099 and v6.14 :1165-1181.
   * The live path is additionally protected by ibs_probe()'s CPUID bit-5
   * gate, but this exported decoder is documented host-independent and can be
   * fed foreign/synthetic records, so it must not rely on that. A shipping
   * part with OpData4 set and BrnTrgt clear is LIKELY nonexistent (reasoned
   * from family docs, not proven — AMD primary source for bits 8-10 was
   * unobtainable); do not claim "cannot happen". Calibration: perf's own
   * amd-sample-raw.c:192-216 reads *(rip+6) with NO caps check and no length
   * floor — this decoder is deliberately stricter than upstream convention. */
  uint32_t caps = ld_u32(p);
  if (!(caps & (1u << 5))) /* BrnTrgt: reg[7] is not a branch target */
      return ASMTEST_IBS_EDECODE;
  ```

- Gate the bit-38 read (same edit site — the amd-followup-2 fold-in):

  ```c
  /* IbsOpData bit 38 (RipInvalid) is architecturally defined only when CPUID
   * Fn8000_001B_EAX[7] (RipInvalidChk) is set; the kernel consults it only
   * behind that cap (v6.12 ibs.c:1068). Every family that sets BrnTrgt=1 also
   * documents RipInvalidChk=1 (review §4 row 3, BKDG citations) — LIKELY
   * redundant here, but the caps word is already in hand and a reserved bit
   * must not drop samples. */
  int rip_invalid = (caps & (1u << 7)) ? bit(data, IBS_OPDATA_RIP_INVALID) : 0;
  ```

- `ibs_probe()`: before the OpSam check insert

  ```c
  if (!(caps & (1u << 0))) { /* IBSFFV: feature flags valid */
      g_avail = 0;
      g_reason = "IBS feature flags not valid (CPUID 8000_001B EAX[0] clear)";
      return;
  }
  ```

  Rationale comment: when bit 0 is clear the kernel substitutes
  `IBS_CAPS_DEFAULT` (AVAIL|FETCHSAM|OPSAM, **BrnTrgt clear**), so our CPUID
  bit-5 read would disagree with the caps the kernel actually samples with
  (plausible under VM CPUID synthesis) — records would be 60 bytes and every
  decode `EDECODE`.
- `include/asmtest_ibs.h:141`: extend the EDECODE line to
  "raw_len too short to hold the branch-target register, or the record's own
  caps word records no branch target (BrnTrgt clear)".

**Tests.** Extend `test_decode()` in
[examples/test_ibs.c](../../../examples/test_ibs.c) — pure, runs on every
host. Generalize the fixture builder with a caps parameter (keep `build_raw`
delegating with `0x3ff` so the existing eight checks are untouched — they
carry self-consistent caps and must pass unchanged):
- **68-byte collision, wrong side**: caps `0x7df` (bit 5 clear, bit 10 set —
  the `BRNTRGT=0/OPDATA4=1` shape), taken-branch bits set, `0xdeadbeef` in
  slot 7 → `ASMTEST_IBS_EDECODE`, and assert the edge was NOT emitted with
  `to == 0xdeadbeef` (the misread the caps check prevents).
- **Append order, right side**: a 76-byte record, caps `0x7ff` (bits 5 and
  10), slot 7 = target, slot 8 = a dummy OpData4 → `ASMTEST_IBS_OK` with
  `e.to == target` (BrTarget appended before OpData4).
- **bit-7 gate**: caps `0x37f` (bit 7 clear), data with bit 38 + taken →
  `ASMTEST_IBS_OK` (reserved bit ignored); caps `0x3ff` (bit 7 set), same
  data → `ASMTEST_IBS_NOEDGE` (existing semantics preserved).
A failing pre-fix build renders the first new check `not ok` (the old decoder
returns OK with the bogus target). The `ibs_probe` bit-0 branch has no
reachable test host (no silicon/VM in CI synthesizes bit0-clear+bit5-set);
manual verification is code review of the branch plus the existing
`test_available()` stability checks — say so in the commit message.

**Docs.** CHANGELOG `### Fixed`: "the pure IBS-Op decoder now validates the
record's own caps word (BrnTrgt) before trusting the branch-target register —
two 68-byte record shapes are length-identical and only caps disambiguates;
`asmtest_ibs_available()` now requires CPUID IBSFFV so it cannot disagree with
the caps the kernel samples with."

**Done when.**
- `make ibs-test` green on this (non-AMD) host with the new pure checks `ok`.
- `grep -n "ld_u32" src/ibs_backend.c` shows it defined outside any `#if`.
- `grep -rn "RipInvalid" src/` now hits the new comment/gate (it hit nothing
  before this task).
- The comment says **likely**, never "unreachable"/"cannot happen".
- Existing fixtures (`0x3ff`, `0x81bff`) pass byte-for-byte unchanged.

### T6 — Exercise `sample_period` / `period_jitter` and the `struct_size` ABI guard for the first time  (S, depends on: none)

**Goal.** The `/16` rounding, the `<16 → IBS_DEFAULT_PERIOD` clamp
([src/ibs_backend.c:654-662](../../../src/ibs_backend.c)) and the additive-ABI
`struct_size` guard (`:698-704`) run against non-default values in a pure
test; the stale `opts.system_wide` phrasing in the public header is fixed.

**Steps.**
1. Verify the gap: [examples/test_ibs.c:226-234](../../../examples/test_ibs.c)
   (`test_opts_abi`) asserts the INIT macro zero-fills — nothing in the tree
   sets `sample_period` or `period_jitter` to a non-zero value, ever.
2. `ibs_period` and the jitter/struct_size resolution live inside the
   platform-gated section and are `static`. Refactor for testability without
   widening the public ABI: expose two INTERNAL seams in
   [src/ibs_backend.h](../../../src/ibs_backend.h) and implement them in the
   **pure** (ungated) part of `ibs_backend.c` — they are plain arithmetic, no
   CPUID, no perf:

   ```c
   /* INTERNAL test seams (pure, no ABI promise): the resolved sample period
    * (/16 rounding + <16 clamp) and jitter fraction (additive struct_size
    * guard) exactly as the live attr fill will use them. */
   uint64_t asmtest_ibs_effective_period(const asmtest_ibs_opts_t *opts);
   unsigned asmtest_ibs_effective_jitter(const asmtest_ibs_opts_t *opts);
   ```

3. Move `ibs_period` (and the `IBS_DEFAULT_PERIOD` / `IBS_JITTER_FRAC`
   defines) above the `#if` at `:257`; implement `effective_period` as a
   wrapper and `effective_jitter` with the exact logic now at `:688-704`
   (default `IBS_JITTER_FRAC`; `ASMTEST_IBS_OPT_NO_JITTER` → 0; caller's
   `period_jitter` honored only when
   `struct_size >= offsetof(asmtest_ibs_opts_t, period_jitter) + sizeof(unsigned)`).
   Refactor `ibs_cfg_from_opts` to call both, so the live path and the test
   share one implementation — the point is that the tested code IS the
   shipped code.
4. Fix [include/asmtest_ibs.h:166-168](../../../include/asmtest_ibs.h): the
   `survey_process` residual-race note ends "(a later opts.system_wide
   phase)" — that phase landed in `a266b91` as a flag, not a field. Replace
   with "(the `ASMTEST_IBS_OPT_SYSTEM_WIDE` flag on `asmtest_ibs_opts_t.flags`
   — needs CAP_PERFMON / `perf_event_paranoid<=0`)".
5. Add `test_period_resolution()` to `test_ibs.c` (pure section, runs on every
   host), run `make ibs-test`, `make docker-fmt` (pinned clang-format 18; plain
   `make fmt` needs `CLANG_FORMAT=clang-format-18`).

**Code.** Test assertions (each is a `CHECK` line; expected values follow the
code exactly — mask to /16 first, then clamp):
- `NULL` opts → `0x4000`; `sample_period = 0` → `0x4000`.
- `1` → `0x4000` (masks to 0, clamps); `15` → `0x4000` (same).
- `16` → `16`; `17` → `16`; `24` → `16`; `0x4001` → `0x4000` (non-multiple
  rounds down); `0x1234` → `0x1230`.
- jitter: legacy caller (`struct_size = 0`, `period_jitter = 4`) → `8`
  (default — tail not covered: **the additive-ABI guard doing its job**);
  `struct_size = sizeof(asmtest_ibs_opts_t)`, `period_jitter = 4` → `4`;
  same + `ASMTEST_IBS_OPT_NO_JITTER` → `0`; INIT + nothing set → `8`.
- keep `test_opts_abi` untouched (INIT zero-fill and `sizeof == 32` are still
  the contract).

**Tests.** As above — `make ibs-test` on any host; a failure prints
`not ok N - period: ...`. No live capture involved (this is the plan's point:
the paths had **never executed** against non-default values anywhere).

**Docs.** CHANGELOG `### Added`: "pure tests for the IBS sample-period
rounding/clamp and the additive-ABI `struct_size` guard (first coverage)."
CHANGELOG `### Fixed`: "`asmtest_ibs.h` no longer describes the shipped
system-wide capture flag as a future phase."

**Done when.**
- `make ibs-test` green on this host with the new checks `ok`.
- `grep -n "opts.system_wide" include/asmtest_ibs.h` is empty;
  `grep -n "ASMTEST_IBS_OPT_SYSTEM_WIDE" include/asmtest_ibs.h` hits both the
  flag define and the survey_process note.
- `grep -n "effective_period\|effective_jitter" src/ibs_backend.c` shows
  `ibs_cfg_from_opts` calling the seams (one implementation, not a copy).

### T7 — Install `asmtest_ibs.h` with its siblings; prove installed headers compile  (S, depends on: none)

**Goal.** `make install` and `make install-shared-hwtrace` ship
`include/asmtest_ibs.h`, `make uninstall` removes it, and the clean-room lane
compiles the guide's `#include <asmtest_ibs.h>` snippet against a fresh
install so the omission class (not just this instance) is closed.

**Steps.**
1. Verify the omission (all checked 2026-07-17): the static `install:` target
   ([Makefile:526-534](../../../Makefile)) copies ten `asmtest_*.h` headers
   plus `asm.h`/`asm_nasm.inc` — `asmtest_ibs.h` absent; `uninstall:`
   (`:539-546`) mirrors it; `install-shared-hwtrace`
   ([mk/native-trace.mk:2822-2833](../../../mk/native-trace.mk)) copies five
   headers at `:2829-2831` — absent again — while
   [docs/guides/tracing/hardware-tracing.md:489](../../guides/tracing/hardware-tracing.md)
   tells users `#include <asmtest_ibs.h>`.
2. Audit result (run: `grep -rn '#include ["<]asmtest' docs/guides/`): the
   guides reference `asmtest.h`, `asmtest_emu.h`, `asmtest_hwtrace.h`,
   `asmtest_drtrace.h`, `asmtest_codeimage.h`, `asmtest_ptrace.h`,
   `asmtest_trace_auto.h`, and `asmtest_ibs.h`. Every one except
   `asmtest_ibs.h` is already in the install list — **this is the only
   omission**; state that in the commit message.
3. Add `include/asmtest_ibs.h` to all three lists (edit targets where they
   live: `Makefile` for install/uninstall, `mk/native-trace.mk` for
   `install-shared-hwtrace`).
4. Close the class: extend
   [scripts/clean-room-test.sh](../../../scripts/clean-room-test.sh) (226
   lines today, bindings-only) with a header-install compile section, and run
   it via the existing `make clean-room-test`
   ([mk/bindings.mk:601-602](../../../mk/bindings.mk)).
5. `make install PREFIX=/tmp/asmtest-t7 && ls /tmp/asmtest-t7/include`, then
   `make uninstall PREFIX=/tmp/asmtest-t7`, then `make clean-room-test`.

**Code.** In `scripts/clean-room-test.sh`, add a header-install section after the
per-binding dispatch (the sequential `should_run <lang> && <lang>_clean_test` lines
at [scripts/clean-room-test.sh:202-207](../../../scripts/clean-room-test.sh) — there
is no loop) and before the summary print. Follow the script's own style, **not** an
invented one: it runs under `set -u` with a `fail=1` counter and reports through
`fail_b <binding> <msg>` / `pass_b <binding> <msg>` (`:44-46`) — there is **no**
`fail()` function, and using the `hdr` pseudo-binding's own throwaway prefix (`$WORK`,
never the per-section `$dest`, which may be unset under `ASMTEST_CLEANROOM_ONLY`) keeps
`set -u` from aborting:

```sh
# C header install check: every header the published guides tell users to
# include must ship with `make install` and compile standalone. Its own fresh
# prefix ($WORK, always set — do not reuse a per-binding $dest).
if command -v cc >/dev/null 2>&1; then
  hdr_prefix="$WORK/c-prefix"
  make -C "$ASMTEST_REPO_ROOT" install PREFIX="$hdr_prefix" >/dev/null
  cat > "$WORK/hdr_check.c" <<'EOF'
#include <asmtest.h>
#include <asmtest_emu.h>
#include <asmtest_hwtrace.h>
#include <asmtest_drtrace.h>
#include <asmtest_codeimage.h>
#include <asmtest_ptrace.h>
#include <asmtest_trace_auto.h>
#include <asmtest_ibs.h>
int main(void) { return asmtest_ibs_available() * 0; }
EOF
  if cc -I"$hdr_prefix/include" -fsyntax-only "$WORK/hdr_check.c"; then
    pass_b hdr "guide-referenced headers compile from a fresh install"
  else
    fail_b hdr "guide-referenced headers do not compile from a fresh install"
  fi
fi
```

The `fail_b` path sets the script's `fail=1`, so a header regression now actually
fails the run (the plain `|| fail "..."` shape this doc originally sketched would have
hit command-not-found, been swallowed under `set -u` without `set -e`, and exited 0 —
the vacuous gate this whole doc exists to kill).

(`-fsyntax-only`: the static install ships only `libasmtest.a`, so no link
step — the header presence and self-containedness is what is under test.)

**Tests.** The clean-room section IS the test: pre-fix it fails with
`asmtest_ibs.h: No such file or directory`; post-fix it is silent.
Additionally: `make install PREFIX=/tmp/asmtest-t7` → `asmtest_ibs.h` listed
in `/tmp/asmtest-t7/include/`; `make uninstall PREFIX=/tmp/asmtest-t7` →
gone. `make docker-clean-room` runs the same script per-language in Docker
(prefer the docker lane per CLAUDE.md; the host run needs only `cc`+`make`).

**Docs.** CHANGELOG `### Fixed`: "`make install` /
`make install-shared-hwtrace` now ship `asmtest_ibs.h` — the hardware-tracing
guide's documented `#include <asmtest_ibs.h>` could not previously compile
against an installed package." The guide itself needs no change.

**Done when.**
- The three lists contain the header; install/uninstall round-trips cleanly.
- `make clean-room-test` passes with the new section (and fails if the
  header line is removed again — mutation check worth doing once by hand).
- No new lane self-skips: this task has no hardware gate anywhere.

## Task order & parallelism

- **T1 → T2 are one unit** (one commit; the plan pairs them — each is the
  other's evidence). Do them first: P1 outranks every code fix because a
  correct fix behind a gate that cannot see it is not verified.
- **T3 → T4** are ordered (T4's header text describes T3's gating) and are
  best landed as one commit too.
- **T5, T6, T7 are fully independent** of everything — three people could do
  them concurrently. T5 and T3 both edit `src/ibs_backend.c` (different
  regions: decoder vs. drain/attr), so coordinate merges if parallel.

Critical path: `T1+T2` → `T3+T4`; total is a few days of work for one person.

## Constraints & gates

- **AMD Zen silicon with the `ibs_op` PMU is the only real gate.** The
  positive paths (AVAILABLE via a real open, live surveys, `3/3` worker
  coverage, live callchain phase-5) run only on an AMD host via
  `make docker-hwtrace-privileged` / `make docker-hwtrace-ibs` /
  `make docker-cli-ibs`. Everywhere else those paths self-skip with a printed
  reason — that is correct per CLAUDE.md (hardware gate). **Every deliverable
  in this doc except the positive-path observations is verifiable off-AMD**,
  which is the point of the honesty work: record in the commit message which
  Done-when items were observed on AMD and which await the next
  [amd-hardware-validation.md](../amd-hardware-validation.md) run.
- **No new dependencies**: no Dockerfile or pinning changes anywhere in this
  doc — every task uses the compilers and targets already in the images.
- **Do not lower host sysctls** for any lane: `CAP_PERFMON` bypasses
  `perf_event_paranoid` entirely (measured at paranoid=4; recorded in
  [mk/cli.mk:374-390](../../../mk/cli.mk)).
- Kernel-behavior comments added by T3/T5 must cite **pinned tags**
  (v6.12/v6.14), never `master` — the review re-pinned mid-pass after line
  drift, and the repo convention follows it.
- Global consistency: `trace_call_auto` is FIXED (`5d8e0d2`) — no doc or
  comment may describe it as open; `truncated=0` on the validation line is a
  regression signal. The archived IBS plan is cited at
  `docs/internal/archive/plans/zen2-ibs-tracing-plan.md`.

## Research notes (verified 2026-07-17)

Kernel facts below were verified against fetched pinned-tag sources
(github.com/torvalds/linux at v6.12 and v6.14), matching the review's pins.

- **Append order (T5's comment cites this).** In `perf_ibs_handle_irq`, under
  `PERF_SAMPLE_RAW` for `perf_ibs_op`, the kernel appends cap-conditionally
  and positionally: `IBS_CAPS_BRNTRGT` → `MSR_AMD64_IBSBRTARGET` **first**,
  then `IBS_CAPS_OPDATA4` → `MSR_AMD64_IBSOPDATA4`. v6.12
  `arch/x86/events/amd/ibs.c:1084-1099`
  (<https://github.com/torvalds/linux/blob/v6.12/arch/x86/events/amd/ibs.c#L1084-L1099>);
  v6.14 `:1165-1181`
  (<https://github.com/torvalds/linux/blob/v6.14/arch/x86/events/amd/ibs.c#L1165-L1181>).
- **The 68-byte collision (byte math).** Base op walk = 7 u64 regs
  (`MSR_AMD64_IBSOP_REG_COUNT` = 7,
  <https://github.com/torvalds/linux/blob/v6.12/arch/x86/include/asm/msr-index.h#L638>);
  raw frag = `u32 caps + 8*N`. Base record 60 bytes; BRNTRGT xor OPDATA4 →
  **68 bytes either way** (only caps disambiguates); both → 76. Struct:
  `perf_ibs_data { u32 size; union { u32 data[0]; u32 caps; }; u64 regs[8]; }`
  (<https://github.com/torvalds/linux/blob/v6.12/tools/arch/x86/include/asm/amd-ibs.h#L145-L152>).
- **perf's own decoder is weaker (calibration line in T5's comment).**
  `tools/perf/util/amd-sample-raw.c` is byte-identical at v6.12/v6.14;
  `amd_dump_ibs_op` prints IbsBrTarget from `*(rip + 6)` gated only on
  `op_data->op_brn_ret && *(rip+6)` — no caps read, no length floor
  (<https://github.com/torvalds/linux/blob/v6.12/tools/perf/util/amd-sample-raw.c#L192-L216>).
- **CPUID Fn8000_001B_EAX bits (T5's probe check).** Bit 0 IBSFFV "feature
  flags valid", 2 OpSam, 5 BrnTrgt, 7 RipInvalidChk — AMD CPUID Spec #25481
  rev 2.34
  (<https://kib.kiev.ua/x86docs/AMD/AMD-CPUID-Spec/25481-r2.34.pdf>; the
  amd.com canonical copy timed out during verification — mirror content
  matches the kernel's bit definitions exactly). Kernel: `IBS_CAPS_BRNTRGT`
  (1<<5), `IBS_CAPS_OPDATA4` (1<<10), `IBS_CAPS_RIPINVALIDCHK` (1<<7) at
  <https://github.com/torvalds/linux/blob/v6.12/arch/x86/include/asm/perf_event.h#L462-L473>;
  `__get_ibs_caps` returns `IBS_CAPS_DEFAULT` (AVAIL|FETCHSAM|OPSAM,
  **BrnTrgt clear**) when bit 0 is clear (v6.12 `ibs.c:1279-1298`). Rev 2.34
  marks bits 31:8 reserved, so bits 8-10 (incl. OpData4) were NOT verified
  against AMD-primary docs — kernel positions only. Hence "likely", never
  "proven", in T5's comment.
- **RipInvalid gating.** Both tags compute
  `check_rip = (perf_ibs == &perf_ibs_op && (ibs_caps & IBS_CAPS_RIPINVALIDCHK))`
  (v6.12 `ibs.c:1068`, v6.14 `:1149`) — the kernel consults bit 38 only
  behind CPUID bit 7, matching T5's gate.
- **Callchain sizing (T3's arithmetic).** `perf_copy_attr` defaults
  `sample_max_stack = sysctl_perf_event_max_stack` = `PERF_MAX_STACK_DEPTH` =
  127 (v6.12 `kernel/events/core.c:12454-12455`,
  <https://github.com/torvalds/linux/blob/v6.12/kernel/events/core.c#L12454-L12455>;
  same statement at v6.14 `:12596`). The sysctl is tunable up to 655360
  (v6.12 `kernel/sysctl.c:1984-2001`) — so a fixed bound is unsound unless
  the attr pins `sample_max_stack`; an explicit value above the sysctl fails
  the open with `-EOVERFLOW` (`kernel/events/callchain.c:129-131`), and
  `get_callchain_buffers` can fail `-ENOMEM`. Record layout: CALLCHAIN
  (`u64 nr; u64 ips[nr]`) **precedes** RAW (UAPI `perf_event.h:957-984`;
  `perf_output_sample` v6.12 `core.c:7471-7545`). Max `nr` = max_stack + 8
  context markers (`PERF_MAX_CONTEXTS_PER_STACK`,
  `callchain.c:23-31`). Totals for this repo's attr (IP|TID|RAW
  [+CALLCHAIN]): fixed 24 + callchain `8*(1+nr)` + RAW wire
  `round_up(4+4+8N, 8)` = 72 (N=8) or 80 (N=9). ABI worst at default
  sysctls: **24 + 8·136 + 80 = 1192**; the review's "~1032-1184" is the
  callchain block alone (nr=128) through the 8-reg worst. Without callchain
  the true record is 96 bytes (104 with 9 regs) — the existing 112 stays
  valid for the non-callchain lanes. `header.size` is u16, so 65535 is the
  absolute per-record ceiling. Caveat: whether any shipping CPU sets BRNTRGT
  and OPDATA4 **simultaneously** could not be verified (needs AMD PPRs or
  hardware); sizing for 9 regs is safe for the userspace parser either way.

## Out of scope

- **branchsnap depth-check fix (P2), Zen 3 BRS documentation truth (P4), the
  freeze-probe retirement + false printf (P5), and every "Zen 3+"→"Zen 4+"
  floor correction** — [amd-branchsnap-lbr-docs.md](amd-branchsnap-lbr-docs.md).
- **The self-hosted Zen runner (P9)** —
  [self-hosted-ci-runners.md](self-hosted-ci-runners.md). T2's checklist stays
  valuable regardless: a runner covers Zen 5, not Zen 2's IBS-only lane.
- **asmspy `--sample` / `--auto` UX around the same APIs** (already consumes
  `asmtest_ibs_unavail_reason`, landed `bb76680`) —
  [asmspy-cli-enhancements.md](asmspy-cli-enhancements.md).
- **The IBS-Op data channel** (DcLinAd/OpData2/OpData3 decoding) — a recorded,
  dated non-goal (review §3); no sibling doc owns it and none should.
- **Wiring an actual callchain consumer** (asmspy `--graph` 5b): deliberately
  NOT done here — T4 documents the deferral honestly instead of shipping an
  unconsumed capture path.
