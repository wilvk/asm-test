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
`EXC_BAD_INSTRUCTION` on a `ud2` in the code cache — the cache's translation
of **libsystem `_sigtramp`'s own `ud2` trap** (the instruction after its
`call sigreturn`). **This is only the release-build *face* of the defect;
SC3's symbolicated debug callstack re-locates the true cause** (see Research
notes → "SC3 diagnosis"): at delivery DR takes the signal's **default action
(terminate)** because `info->sighand->action[SIGUSR1]` is not recorded in
time, and the bug is **timing-sensitive / multi-thread-entangled** (the
pure-C reproducer hangs on macOS thread-takeover NYI, i#58). Treat the
`ud2`/sigreturn framing below as the symptom trail, not the final root
cause.

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

**SC3 diagnosis (executed 2026-07-22) — root cause re-located and the bug
class identified as a race, NOT a static sigreturn defect. Fix not yet
landed.** Two pieces of hard evidence, plus three eliminations:

- **Symbolicated debug callstack (no debugger)** — `atos` against the debug
  dylib resolves the crash path to, top-down:
  `main_signal_handler → record_pending_signal → execute_handler_from_cache
  → send_signal_to_client_and_handle_action →
  handle_client_action_from_cache (signal.c:4290) →
  execute_default_from_cache → execute_default_action (signal.c:7159) →
  dynamo_process_exit`. So the true mechanism is NOT the release `ud2`
  symptom (SC2's reading): at delivery DR takes the signal's **default
  action (terminate)** via the branch at `signal.c:4285`, which fires when
  `action == DR_SIGNAL_BYPASS || info->sighand->action[sig] == NULL ||
  handler == SIG_DFL`. The client returns `DR_SIGNAL_DELIVER` (the C cases
  prove it — same client), so the trigger is **`info->sighand->action[30]`
  being NULL/SIG_DFL at delivery** — DR has no record of CPython's SIGUSR1
  handler at the instant the signal is processed. The `_sigtramp` ud2 is the
  release-build face of the same termination; the `heap.c:1995` assert is
  exit-cleanup noise.
- **The bug is timing-sensitive (the load-bearing find).** Under lldb (SIGUSR1
  passed through, break on `handle_sigaction`), `handle_sigaction(sig=30)`
  **does** fire — DR *can* and *does* intercept CPython's
  `signal.signal(SIGUSR1)` registration — and with it recorded the
  default-action path does **not** run. Yet the process still reaches the
  `ud2` by another route under lldb. So DR's ability to have `action[30]`
  populated in time is **order-dependent**: without the debugger the signal
  is processed before/around the registration being committed; with it, the
  timing shifts. A race between the app's post-attach handler registration
  (recorded via the intercepted `SYS_sigaction`) and signal delivery/return
  on the macOS path — consistent with that path's NYI/undermaintained state
  (`save_fpstate` no-op, the stale sigreturn comment) — not a single static
  mispredicate.
- **Eliminations (pure-C repros, all PASS ⇒ none is the trigger):** an
  alternate signal stack + `SA_ONSTACK` (CPython's faulthandler shape),
  3/3; a `write()` syscall inside the handler after
  SIGINT/SIGPIPE/SIGXFSZ + SIGUSR1 registrations, 3/3; async in-cache
  delivery across direct- and indirect-branch spins, 3/3 and 5/5. The
  distinguishing ingredient is CPython's volume/threading/eval-loop timing,
  not any single API shape a small C program reproduces.
- **Where SC3's fix must look:** the ordering guarantee between
  `handle_sigaction` committing `info->sighand->action[sig]` and
  `record_pending_signal`/`send_signal_to_client_and_handle_action`
  consulting it on macOS (`core/unix/signal.c`), plus whether the macOS
  `_sigtramp`/sigreturn return path re-reads a handler that was installed
  post-attach.
- **A pure-C reproducer attempt tied the defect to macOS multi-thread
  support (i#58).** The candidate red gate — a background thread churning
  fragments while the main thread registers-then-self-kills SIGUSR1 in a
  loop — does not reach its signal loop: it **hangs at `pthread_create`
  under attach**, DR's macOS multi-thread takeover being NYI
  (`os_list_threads` returns 0, `thread_signal` NOT_IMPLEMENTED — the same
  i#58 the fork-build doc records). So a *single-threaded* C program cannot
  reproduce the defect (all such guards pass), and a *multi-threaded* one
  hits the thread-takeover NYI first. CPython is multi-threaded (GIL,
  faulthandler), so its signal delivery runs straight into that same
  undermaintained surface. **This re-scopes SC3:** it is very likely not a
  surgical "fourth fork fix" but part of macOS multi-thread signal delivery
  (i#58) — a substantial DR-core effort. **No fork fix is committed; SC3
  remains open at this diagnosis, with the scope corrected.** The honest
  fallback (SC3 step 4) is now the live option: the guide/plan wording is
  already precise ("signal chaining under an attached trace hangs on
  macOS"), the Darwin skip stays, and the residual is a named i#58-class
  limitation rather than a quick fix — pending a decision to take on the
  multi-thread signal work.

**SC3 diagnosis v2 (2026-07-22, later) — SUPERSEDES v1: the multi-thread
framing is REFUTED; the true mechanism is a signal interrupting DR gencode
that is not safely resumed on macOS. Single-threaded, with a deterministic
pure-C reproducer.** New hard evidence:

- **CPython is single-threaded at signal time — measured, not inferred.** A
  minimal `signal.signal(SIGUSR1, h)` + `os.kill` script reports
  `threading.active_count() == 1` and `ps -M <pid>` shows exactly **one** OS
  thread. So v1's "requires multi-thread / i#58" conclusion is WRONG: the
  `pthread_create` hang was a *separate* macOS NYI that my race reproducer
  tripped over, unrelated to the actual defect.
- **A deterministic single-threaded pure-C reproducer exists**
  (`scratchpad/sigchain_cpythonish.c`, to be folded into the SC1 harness /
  a `cli/*_victim`): install handlers for a few signals **before** attach
  (as CPython's interpreter init does), attach, build a little code, then
  `sigaction(SIGUSR1)` + self-`kill`. It SIGILLs **6/6**, no hang. It is
  **code-layout-sensitive**: a structurally-similar repro
  (`sigchain_repro.c`) passes — the difference is whether the signal happens
  to land while the thread is in a DR **gen routine/exit stub** vs at a clean
  syscall boundary. This is the red gate SC3 needs (no `pthread_create` NYI).
- **The handler IS recorded and IS delivered — v1's "action[30] NULL" was a
  Python-re-exec artifact.** The clean-C debug ASYNCH log (`-loglevel 3
  -logmask 0x10`, which finally flushes because pure C does not re-exec like
  the Python framework binary) shows, in order: `app installed 0x… as
  sigaction for signal 30` (recorded); `record_pending_signal(30) from gen
  routine or stub 0x…c42` (**the signal interrupts DR gencode**);
  `execute_handler_from_dispatch for signal 30` (delivered — the app handler
  runs, sets its flag); `rt_sigreturn()`; then `record_pending_signal(4)
  from cache pc 0x…` (a **SIGILL** now arises after the resume);
  `app signal handler is SIG_DFL: executing default action` → terminate. So
  DR delivers correctly, but **resuming the interrupted gencode after the
  handler faults**.
- **The fault is a DR-gencode `ud2` (lldb ground truth).** At the SIGILL:
  `-> ud2 ; movq %rcx, %gs:0x830 ; jmp 0x…` with `rcx` = the interrupted gen
  PC and `gs = 0x0` (selector; the TLS **base** is what matters). This is an
  exit-stub / IBL-entry shape (spill `rcx` to `%gs` TLS, jump). Execution
  resumed into DR's own generated code and hit a `ud2`; DR's macOS signal
  path does **not** recognize this as its own internal trap — it classifies
  the SIGILL as an app fault at a cache PC and takes the default action.
- **Code locus.** `find_next_fragment_from_gencode` (`core/unix/signal.c`
  ~4725) resolves the interrupted→next-fragment for **clean-call** and **IBL**
  gencode but NOT for exit stubs — the explicit `XXX: should also check fine
  stubs` gap at ~5030. For a stub interrupt it returns NULL, so
  `info->interrupted` stays NULL and DR resumes the **raw** interrupted gen
  PC rather than cleanly re-entering the app fragment. That resume path is
  fine on Linux but faults on macOS — prime macOS-specific suspects: the
  `%gs` TLS **base** not being restored across the app-handler/`sigreturn`
  for a resumed stub; `save_fpstate` being a release no-op on the frame path;
  and DR not treating its own gencode `ud2` as internal on the macOS SIGILL
  path. **This is a real, tractable single-threaded fix target** — SC3 is
  back in scope as a focused signal-path fix, not the i#58 multi-thread
  project v1 feared. (A parallel code-read workflow is confirming the exact
  culprit among those suspects; fix + validation against the C gate follow.)

**SC3 diagnosis v3 (2026-07-22, later) — CONFIRMED root cause via full runtime
instrumentation of a debug fork build. Supersedes v2's suspect list and the
investigation workflow's exit-stub guess.** The fix is a real macOS
sigreturn/context-restore feature, not a surgical patch. Method: a
deterministic single-threaded C reproducer (`sigchain_cpythonish.c`) against a
`DR_MACOS_BUILD_TYPE=Debug` build with added `LOG_ASYNCH` probes in
`record_pending_signal`, `find_next_fragment_from_gencode`, and
`execute_handler_from_dispatch`, plus lldb register dumps at the fault.

Confirmed sequence (every step measured, not inferred):

1. `kill(getpid(), SIGUSR1)` delivers the signal on **return from the `kill`
   syscall, while the thread is in DR's `do_syscall` gencode** — probe:
   `record_pending_signal(30) from gen routine or stub`, with
   `get_at_syscall()==1`, `is_after_syscall_address(pc)==1`, and
   `dcontext->asynch_target` already holding the correct post-syscall **app**
   PC. (This is NOT the exit-stub/IBL case the workflow synthesis proposed
   fixing — the `!get_at_syscall` guard at signal.c:5085 means
   `find_next_fragment_from_gencode` is never even called here.)
2. The signal is delayed and delivered at dispatch. **Delivery is CORRECT:**
   `execute_handler_from_dispatch` probe shows `use_sigcontext=0`,
   `next_tag == asynch_target == frame_xip == <app PC>`. `signal.c:6402` sets
   the delivered/resume frame's `SC_XIP` to `next_tag` (the app PC), and the
   app handler runs.
3. The app handler returns through libsystem **`_sigtramp`, which calls the
   macOS 3-arg `sigreturn(uctx, infostyle=0x1e, token)`.** DR intercepts
   `SYS_sigreturn`, `handle_sigreturn` doctors the frame (`SC_XIP =
   fcache_return`) and returns `true`, so **the real macOS `sigreturn` runs
   against DR's synthesized frame.**
4. **The resume faults**: lldb at the SIGILL shows `rip` on a DR gencode `ud2`
   (`ud2 ; movq %rcx,%gs:0x830 ; popq %rcx ; jmp`), with `rcx` = the
   interrupted `do_syscall` gencode PC and the register file in the exact
   `_sigtramp→sigreturn` shape (`rsi=0x1e`, `r12=0xffffff00` = a bogus token).
   macOS raises SIGILL; `main_signal_handler`→`handle_nudge_signal` (NUDGESIG
   is SIGILL on macOS) sees a genuine `ud2` and passes it to the app; the app
   has no SIGILL handler → default action → **terminate**.

**Root cause.** macOS `sigreturn` takes a kernel-minted per-delivery
**validation token** that DR cannot forge. DR delivers the app handler by
*synthesizing* a signal frame (`copy_frame_to_stack`) — no kernel token — and
then lets the app's `_sigtramp` issue the real `sigreturn` with a bogus token
against DR's doctored frame. On Linux `sigreturn` has no token, so synthetic
frames work (all the passing single-threaded C repros exercise this); on macOS
the token makes the synthesized return invalid, and the resume lands in DR
gencode. This is exactly why it is code-layout/timing sensitive: it only
manifests when the signal lands such that the app return threads back through
`_sigtramp`→`sigreturn` while DR's saved context still points at gencode.
`thread_set_self_context`'s macOS arm is itself NYI here (the
`ASSERT_NOT_IMPLEMENTED(false && "need to pass 2 params to SYS_sigreturn")` at
`core/unix/signal.c:3292` is on this path), confirming the surface is
unimplemented, not merely buggy.

**A fix attempt was made and measured** (not landed): translating the
interrupted `SC_XIP` to `asynch_target` early in `record_pending_signal`
*eliminated the `ud2`/SIGILL crash* (clean thread-exit reached) but caused a
native escape that skipped handler delivery (`got=0`) — proving the delivery
path is fine and the defect is squarely the return/resume mechanism. Reverted.

**The real fix (two options, both substantial DR-core macOS work):**
(a) **Capture and propagate the token** — save the kernel-minted token when
the original signal reaches `main_signal_handler`, thread it into the
synthesized app frame so the app's `_sigtramp` `sigreturn` validates; or
(b) **Never issue the real macOS `sigreturn`** — implement the macOS arm of
`thread_set_self_context`/`handle_sigreturn` to drive the return directly via
a DR context switch (make `handle_sigreturn` return `false` and restore state
itself), the way the VMX86 path already does, removing the token dependency.
Option (b) is the cleaner long-term shape and matches the fork's other FB
fixes; both need careful iteration against the C gate plus the full baseline
(Linux `docker-drtrace` neutrality, `api.startstop`/`api.detach`). This is a
bounded feature, not a one-line patch, and is **not landed** — SC3 stays open
here with the target now precisely identified (macOS synthetic-signal-frame
`sigreturn` token, NOT multi-thread i#58 and NOT the exit-stub-lookup gap).

## Out of scope

- **macOS nudge support** (i#1286) — adjacent NYI, separate feature.
- **arm64-macOS signal work** — everything here is x86-64; arm64 stays
  behind [macos-dynamorio-port.md](macos-dynamorio-port.md) T8's gates.
- **Upstreaming the fix** — the fork's relationship to upstream is
  [macos-dynamorio-fork-build.md](macos-dynamorio-fork-build.md)'s concern;
  this doc lands it on the pinned branch only.
- **Multi-threaded takeover** (i#58's thread-list half) — unchanged;
  single-threaded delivery is the contract being fixed.
