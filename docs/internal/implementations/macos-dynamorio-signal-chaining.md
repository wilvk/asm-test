# macOS DynamoRIO fork: fix signal chaining under attach (the drgate hang) — implementation

> **Sources.** Follow-up to
> [macos-dynamorio-fork-build.md](macos-dynamorio-fork-build.md) (the pinned
> fork + the three FB2 fixes this extends) and
> [macos-dynamorio-port.md](macos-dynamorio-port.md) (whose T9 measured and
> recorded the defect). The defect is recorded in
> [macos-drtrace-plan.md](../plans/macos-drtrace-plan.md) ("STATUS 2026-07-22
> (M2 bindings)") and user-visibly in
> [native-tracing.md](../../guides/tracing/native-tracing.md) ("signal
> chaining under an attached trace hangs on macOS"). Written 2026-07-22, all
> probe results below measured live that day on the macOS 14.7.5 / Intel dev
> host. If this doc and the code disagree, re-verify before implementing.

## Why this work exists

A signal raised while DynamoRIO is attached on macOS is not delivered to the
host runtime's handler; under CPython the process dies (or, under pytest,
wedges). This is the one red path found while landing the macOS port's M2
bindings: `bindings/python/tests/test_drgate.py::test_signal_chaining` is
Darwin-skipped with exactly this reason. Signal chaining is a documented
guarantee of the tier on Linux (the drgate suite exists to assert it survives
a real interpreter), so on macOS it is currently a **named limitation** in the
user guide. This doc removes the limitation at its root — a fourth root-caused
fix in the pinned fork (`wilvk/dynamorio`, branch `asmtest/macos-fixes`),
sibling of the three FB2 fixes — and un-skips the test.

## What is known (measured 2026-07-22, this host)

**The failing case, deterministic (3/3):** attach via the python binding
(`NativeTrace.initialize()`), `signal.signal(SIGUSR1, py_handler)`,
`os.kill(os.getpid(), SIGUSR1)` → the process dies with **SIGILL** (rc −4)
before `os.kill` returns. Under pytest the same case presents as a **hang**
(observed twice at T9 time, ≥90 s, process wedged; the plain-python form
crashes instead — both are the same defect wearing different hats).

**The crash site (lldb, stop-on-exec off, SIGUSR1 pass-through):**
`EXC_BAD_INSTRUCTION` on a `ud2` in the code cache. **SC2's debug run
re-attributed this** (see Research notes): it is not DR-planted — it is the
cache's faithful translation of **libsystem `_sigtramp`'s own `ud2` trap**,
the instruction Apple places immediately after `_sigtramp`'s call to
`sigreturn` ("sigreturn returned — cannot happen"). The defect is therefore:
under DR, the app's `sigreturn` syscall for a DR-delivered frame is
**rejected by the kernel and returns**, and `_sigtramp` falls into its trap.

**Four minimal C repros that all PASS (3–5 runs each), narrowing the
trigger:** the defect is *not* reproduced by (1) self-`kill()` at the syscall
boundary (`scratchpad` repro landing as SC1's in-tree guard), (2) an async
`SIGALRM` landing mid-spin in a direct-branch-only loop, (3) the same landing
in an indirect-call+ret loop, (4) a CPython-mimicking handler that performs a
`write()` syscall inside the handler after multiple `sigaction`
registrations. So plain in-cache asynchronous delivery — direct *and*
indirect exits — works; the CPython process adds the missing ingredient.
Untested-by-C candidates, in order of suspicion: the **alternate signal
stack** CPython installs at startup (faulthandler's `sigaltstack`), delivery
landing **inside DR's IBL/gencode routines** rather than a fragment body
(CPython's dispatch is indirect-branch-dense, so the window is wide), and
sheer cache/trace volume (trace fragments with inlined IBL).

**Fork-side anchors (pin `bbbcc40b8`):**
[core/unix/signal_macos.c](https://github.com/wilvk/dynamorio/blob/asmtest/macos-fixes/core/unix/signal_macos.c)
is 388 lines against `signal.c`'s ~9000 and carries explicit i#58 NYI stubs —
`save_fpstate` is a bare `ASSERT_NOT_IMPLEMENTED(false)` (line ~148), which a
**release build compiles to a silent no-op** and is on the app-handler
delivery path (the frame's FP state); macOS nudge signals are NYI (i#1286).
The receive-while-in-cache machinery is shared code
(`core/unix/signal.c`: `unlink_fragment_for_signal` ~4458, the
`interrupted_inlined_syscall`/delivery decision points ~4993–5088,
`ASSERT_NOT_REACHED`-adjacent gencode) whose macOS arm upstream **never
exercises** (macOS CI runs only the `OSX`-labeled quarter-suite; the
signal-delivery API tests are not in it — established in the fork-build doc).

**Baseline greens that must stay green:** `make drtrace-test-macos` 13/13 +
18/18; `drtrace-cpp-test`/`-ruby-test` PASS; `drtrace-python-test` 3+3
passed / 1 Darwin-skip; fork `api.startstop`/`api.detach` 10/10 exit-0 under
the i#58 output filter; `make docker-drtrace` (Linux) green.

## Tasks

### SC1 — In-tree guards for the paths that DO work + the repro knob  (S, depends on: none)

**Goal.** The four working delivery shapes are pinned by an always-on C
assertion in the macOS harness (so SC3's surgery cannot silently regress
them), and the skipped python test can be force-run with one env knob for the
red baseline.

**Steps.**
1. Add a signal-chaining section to
   [examples/test_drtrace_macos.c](../../../examples/test_drtrace_macos.c)
   (after the symbol-mode block, before shutdown): install a `SIGUSR1`
   handler via `sigaction` (plain `sa_handler`, empty mask, flags 0),
   self-`kill()`, poll ≤1 s for the flag, `CHECK(got == 1, "signal chained to
   the app handler under attach")`. Then an async case: `SIGALRM` via
   `setitimer` landing in a ≥50 ms indirect-call+ret spin (mirror the
   `steps[i & 1](acc)` scratch shape), `CHECK` the flag again. Both pass
   today against the pinned fork — they are guards, not the repro.
2. In [bindings/python/tests/test_drgate.py](../../../bindings/python/tests/test_drgate.py),
   widen the Darwin `skipif` condition to
   `sys.platform == "darwin" and not os.environ.get("ASMTEST_SIGCHAIN_DARWIN")`
   so `ASMTEST_SIGCHAIN_DARWIN=1` runs the red case on demand (the fix's
   acceptance flips it green before SC4 removes the skip entirely).
3. Run `make drtrace-test-macos DYNAMORIO_HOME=$(make -s dynamorio-macos)`
   twice — the extended harness must be all-`ok` (now 13+2 = 15 in the M0
   harness or keep the count in the generated-bytes one; either, but TAP
   count updated) — and `make docker-drtrace` once (the harness is
   Darwin-only; expect no Linux effect).

**Code.** Harness + one-line skipif change only. No library changes.

**Tests.** The harness IS the test. The python knob is verified by one
`ASMTEST_SIGCHAIN_DARWIN=1` run reproducing the failure (record rc/symptom).

**Docs.** None yet (SC4 carries the doc flips).

**Done when.** Extended harness green twice; the knob reproduces the red
case on demand; Linux lane untouched-green.

### SC2 — Debug fork build + name the first failing invariant  (M, depends on: none; unblocks SC3)

**Goal.** The release-mode `ud2` becomes a named DR internal assert
(file:line + message), captured verbatim — the FB2 method: diagnose in debug,
fix, validate in release.

**Steps.**
1. Teach [scripts/build-dynamorio-macos.sh](../../../scripts/build-dynamorio-macos.sh)
   an opt-in debug mode: `DR_MACOS_BUILD_TYPE=Debug` env var →
   `-DDEBUG=ON -DINTERNAL=ON` into a **separate** build dir
   (`<prefix>-debug`), commit-stamp suffixed, never the default and never
   what `make dynamorio-macos` prints (the pinned release flow must be
   byte-identical when the var is unset). Gate exactly like the release path
   (Darwin x86-64 only).
2. Build it at the current pin. Risk, recorded up front: a macOS debug build
   may fire *unrelated* pre-existing asserts before reaching the repro
   (upstream never runs this config). Whitelist/patch around only what
   blocks reaching the repro, recording each in the fork branch as separate
   commits (the FB2 pattern).
3. Run the python repro (`ASMTEST_SIGCHAIN_DARWIN=1` one-test pytest, and
   the plain-python script form) against the debug home
   (`ASMTEST_DR_LIB=<debug>/lib64/debug/libdynamorio.dylib` — confirm the
   actual debug subdir the build emits). Capture the assert text, the DR log
   (`-loglevel 2` if needed), and the backtrace.
4. Append the named invariant to this doc's Research notes (or correct the
   suspicion list if it points elsewhere — e.g. straight at `save_fpstate`).

**Code.** Build-script mode only (asm-test side); possible small fork
commits to make debug reach the repro.

**Tests.** The deliverable is the recorded assert. The release flow is
proven unaffected by re-running `make dynamorio-macos` (commit stamp
unchanged → no rebuild) and `make drtrace-test-macos` green.

**Docs.** This doc's Research notes updated with the named invariant.

**Done when.** A debug `libdynamorio.dylib` exists at the pin; the repro
under it produces a recorded, named assert (or a recorded debug-specific
divergence explaining why not); release flow byte-identical when the var is
unset.

### SC3 — Root-cause and fix in the fork  (L, depends on: SC2)

**Goal.** The python repro passes against a **release** fork build: the
signal chains to CPython's handler, no SIGILL, no wedge.

**Steps.**
1. From SC2's named invariant, root-cause in the fork. Prime suspects, in
   order: `save_fpstate`'s release no-op corrupting the delivery frame the
   app handler (and DR's own sigreturn-equivalent) consumes; the macOS
   sigframe/`_sigtramp` return contract vs DR's emulated delivery
   (`execute_handler_from_cache` and friends); the sigaltstack arm of frame
   placement; the unlink/redirect path for a thread interrupted inside DR
   gencode (IBL) rather than a fragment body.
2. Fix on `asmtest/macos-fixes`, one commit per root cause,
   upstream-shaped (as FB2's three were: measured baseline in the message,
   minimal diff, macOS-scoped `#ifdef MACOS` where the shared file is
   touched).
3. Green the ladder in order, all against a fresh **release** build at the
   candidate commit: (a) the SC1 C guards (still green); (b) the
   plain-python repro 5/5 (was SIGILL 3/3); (c)
   `ASMTEST_SIGCHAIN_DARWIN=1` pytest one-test 3/3 (was wedge/crash);
   (d) the full baseline set (harness 13+2/18, cpp/ruby/python lanes,
   fork `api.startstop`/`api.detach` 10/10 filtered, `make docker-drtrace`).
4. If the full fix proves out of reach in reasonable effort (macOS signal
   delivery is the least-maintained DR surface), the fallback is recorded
   honestly: fix what is fixable, and if chaining still cannot work, convert
   the guide/plan wording from "hangs" to the precise residual behavior and
   keep the skip — but only after SC2's invariant is named and chased, not
   before. A partial land is `◐`, not a silent re-scope.

**Code.** Fork-side only (plus nothing in asm-test until SC4).

**Tests.** The ladder above; each rung's counts recorded in the commit
messages.

**Docs.** None yet.

**Done when.** Release-build python repro + one-test pytest green
repeatedly; every baseline green; fix commits pushed to the fork branch.

### SC4 — Advance the pin, un-skip, flip the docs, CI-validate  (S, depends on: SC3)

**Goal.** The tree consumes the fixed fork: pin bumped, the Darwin skip
removed, the limitation language deleted, nightly CI rebuilding at the new
pin proven green.

**Steps.**
1. Bump the `dynamorio-fork` commit in
   [scripts/third-party-digests.txt](../../../scripts/third-party-digests.txt)
   (the refresh emitter path the fork-build doc landed); `make
   dynamorio-macos` rebuilds at the new pin (commit stamp changes).
2. Remove the Darwin `skipif` from `test_drgate.py::test_signal_chaining`
   entirely (and SC1's env knob with it); `make drtrace-python-test` on this
   host: **4+4 passed, 0 skipped**.
3. Flip the docs: delete the signal-chaining limitation sentence from
   [native-tracing.md](../../guides/tracing/native-tracing.md); append the
   resolution to the plan's M2-bindings STATUS block (dated, with the root
   cause named); CHANGELOG `### Fixed` entry naming the fork fix; update
   [macos-dynamorio-fork-build.md](macos-dynamorio-fork-build.md)'s fix
   list from three to four (one line, pointing here).
4. Full local re-validation: `make drtrace-test-macos` (twice),
   `drtrace-cpp-test`, `drtrace-ruby-test`, `drtrace-python-test`,
   `make docker-drtrace`, `make docker-docs`, `make docker-fmt-check`.
5. Dispatch CI (`gh workflow run ci.yml`): the `drtrace-macos` job's cache
   key includes the digests file, so the bump forces a **from-scratch fork
   build at the new pin on `macos-15-intel`** — the job green (with the SC1
   guards in the harness) is the end-to-end proof. Record the run id here
   and in the README row.

**Code.** Digest bump + skip removal; docs.

**Tests.** Step 4's set + the dispatched run.

**Docs.** As step 3.

**Done when.** Pin bumped; python lane 4+4/0-skip on macOS; limitation
language gone; dispatched `drtrace-macos` green at the new pin; README row
updated.

## Task order & parallelism

```
SC1 (guards + knob) ──┐
                      ├─ SC3 (root-cause + fix) ── SC4 (pin bump + un-skip + CI)
SC2 (debug build)  ───┘
```

SC1 and SC2 are independent and can run concurrently. SC3 is the critical
path and consumes both. SC4 is mechanical once SC3 is green.

## Constraints & gates

- **No hardware or credential gates.** Everything runs on this Intel-mac dev
  host; the CI leg is the existing dispatch-gated `drtrace-macos` job.
- **The fork branch is the fix site**, never asm-test workarounds (no
  app-side signal trampolines, no "don't use signals" contract): the tier's
  Linux contract is that chaining works, and the port doc's positions make
  macOS a peer, not a footnote. asm-test changes are limited to harness
  guards, the skip removal, the build-script debug mode, and docs.
- **Release-flow purity:** `make dynamorio-macos` output at an unchanged pin
  must be byte-identical throughout (commit-stamp reuse); the debug mode is
  opt-in via `DR_MACOS_BUILD_TYPE=Debug` only.
- **Honest fallback** (SC3 step 4): if full chaining is unreachable, the
  residual behavior is re-measured and re-documented precisely — but only
  after the debug invariant is named; the current "hangs" wording may not
  survive contact with the root cause either way.
- **Baseline discipline:** every green listed in "What is known" is re-run
  at SC3/SC4; a fix that trades the hang for any baseline regression does
  not land.

## Research notes (measured 2026-07-22 unless noted)

- Failing form: python 3.14.6 (Homebrew, non-hardened), SIGUSR1 self-kill
  under attach → SIGILL rc −4, 3/3, dying between `os.kill()` entry and
  return; pytest form wedges instead (≥90 s, killed manually, twice).
- lldb (stop-on-exec false; SIGUSR1 pass): `EXC_BAD_INSTRUCTION` at a
  planted `ud2` in anonymous (code-cache) memory, low bits `…033` across
  runs; following bytes `movq %rcx, %gs:0x870; popq %rcx; jmp` — ret-IBL
  exit-stub shape. Pre-delivery stop mid-fragment at `movabsq %rax,
  %gs:0x860` spill code. CPython re-execs itself (Homebrew shim →
  `Python.app/Contents/MacOS/Python`), hence stop-on-exec handling.
- Passing C probes (scratchpad `sigchain_repro.c` / `_async.c` /
  `_indirect.c` / `_pyish.c`, kept in the SC1 harness form): at-syscall
  self-kill 3/3; async SIGALRM mid-direct-spin 3/3; async mid-indirect
  call+ret spin 5/5; write()-inside-handler after SIGINT/SIGPIPE/SIGXFSZ +
  SIGUSR1 registrations 3/3. All with plain `sa_handler`, empty mask,
  flags 0 — CPython's `PyOS_setsig` shape.
- Fork pin `bbbcc40b8`: `core/unix/signal_macos.c` 388 lines;
  `save_fpstate` = `ASSERT_NOT_IMPLEMENTED(false)` (i#58) → release no-op on
  the delivery path; nudges NYI (i#1286); AArch64 SVE arms NYI (i#5383).
  Shared `core/unix/signal.c` ~9000 lines: `unlink_fragment_for_signal`
  ~4458, `interrupted_inlined_syscall` ~4557, delivery decisions ~4993–5088,
  a final unlink site ~8978.
- Upstream never runs this path on macOS: CI is the `OSX`-labeled
  quarter-suite (i#1815); the signal API tests are outside it (fork-build
  doc, "doc corrections"). So no upstream green ever contradicts the
  defect.
- CPython delta candidates ranked: startup `sigaltstack` (faulthandler
  reserves an alt stack); delivery landing inside DR gencode/IBL rather
  than a fragment (indirect-branch-dense dispatch); trace-fragment volume
  with inlined IBL. All three are debug-build-distinguishable (SC2).

**SC2 findings (executed same-day, 2026-07-22 — the invariant is named):**

- Debug build (`DR_MACOS_BUILD_TYPE=Debug`, pin unchanged) under the python
  repro reports: *"Application exception at PC `<_sigtramp+51>`. Signal 4
  delivered to application as default action."* — DR translated the cache
  fault back to the app PC. `atos`/lldb on the live shared cache identify
  `_sigtramp+51` (libsystem_platform) as **libsystem's own `ud2` trap**,
  placed directly after `_sigtramp`'s `call sigreturn`. So the app's
  `sigreturn` RETURNED — the kernel rejected the frame/token — which native
  code treats as impossible. The earlier "DR-planted ud2" reading in the
  probe notes is corrected by this.
- `_sigtramp` on macOS 14.7.5 x86-64 passes **three** args to `sigreturn`:
  `uctx` (rbx), `infostyle 0x1e`, and a **kernel validation token** (r12).
  DR's stale NYI comment at `core/unix/signal.c:3292` says "need to pass 2
  params to SYS_sigreturn" — it predates the token. DR's dispatcher DOES
  intercept `SYS_sigreturn` on macOS (`core/unix/os.c` ~8011 →
  `handle_sigreturn(dcontext, uctx, infostyle)`) — the token param is never
  read; `handle_sigreturn`'s strategy is "doctor the frame, then let the
  real sigreturn execute" (its own comment), so a token/frame the kernel
  won't validate surfaces exactly as the observed fall-through.
- The `heap.c:1995` vm-heap free-blocks debug assert fires later on the
  death path — secondary damage, not the invariant. A startup `curiosity:
  rex.w on OPSZ_6_irex10_short4` print is unattributed decode noise; do not
  anchor on it.
- **SC3's sharpened question:** why does the identical
  `_sigtramp → sigreturn` sequence succeed for all four C shapes but get
  kernel-rejected for the CPython shape — prime suspect: token/frame-address
  validation vs where DR (re)builds the app frame, with CPython's startup
  `sigaltstack` the ranked delta.

## Out of scope

- **macOS nudge support** (i#1286) — adjacent NYI, separate feature.
- **arm64-macOS signal work** — everything here is x86-64; arm64 stays
  behind [macos-dynamorio-port.md](macos-dynamorio-port.md) T8's gates.
- **Upstreaming the fix** — the fork's relationship to upstream is
  [macos-dynamorio-fork-build.md](macos-dynamorio-fork-build.md)'s concern;
  this doc lands it on the pinned branch only.
- **Multi-threaded takeover** (i#58's thread-list half) — unchanged;
  single-threaded delivery is the contract being fixed.
