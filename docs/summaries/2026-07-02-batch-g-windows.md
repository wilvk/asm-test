# Batch G — Win64 runner bugs (findings #33–#36)

Scope: the native Win64 runner tier — `src/platform_win32.c`,
`src/platform_win32.h`, and the Win64 `run_one` in `src/asmtest.c`. Build path is
the mingw-w64 cross-compile (`Dockerfile.win64` / `make win64-runner-test`); the
host is Linux x86-64, so runtime validation on a real Windows host is still
required (Wine is faithful for computation/ABI but not for the timer-queue /
`SetThreadContext` / kernel-wait edges these findings touch).

Build command used (inside the `asmtest-win64` image, repo mounted at `/src`):

```
docker run --rm -v "$PWD":/src -w /src asmtest-win64 make win64-runner-test
```

Result: **builds clean**; `platform_win32.c` compiles with **zero warnings**
under the target's `-Wall -D__USE_MINGW_ANSI_STDIO=1` flags. The suite `.exe`
links and, as a best-effort smoke under Wine, all four modes pass
(isolation / `-jN` / `--no-fork` / `--bench`): `5 passed, 2 failed` as expected,
including the `--no-fork` watchdog and crash-contained paths. (The pre-existing
`%zu`-format and unused-variable warnings in `asmtest.c` are unrelated and not
introduced here.)

---

## #33 (Med) — Teardown skipped on body fail/skip/crash/timeout

**Validated.** The Win64 `run_one` collapsed setup+body+teardown into one
`__builtin_setjmp` scope, so any assert/`SKIP`/crash/timeout longjmp'd straight
past `run_hooks(suite, 1)` — teardown never ran. Diverges from the POSIX
`run_one`, which runs teardown whenever setup completed.

**Changed.** `src/asmtest.c` (Win64 `run_one`) rewritten to mirror the POSIX
three-scope structure: setup → body → teardown, each re-establishing the recovery
point. Teardown runs whenever setup succeeded; a teardown crash/timeout/failure
marks the test failed, a `SKIP()` in teardown downgrades a pass to skip, and
teardown does **not** run when setup itself failed/skipped (POSIX parity). Added
`win32_set_reason_msg()` helper to set the crash/timeout message in each branch.

To give teardown the same crash/timeout protection as the body (an assertion
disarms the facility via `asmtest_win32_test_disarm`), added
`asmtest_win32_test_rearm()` in `src/platform_win32.c`/`.h`, called before the
body and teardown phases. The one-shot watchdog timer, if unfired, keeps covering
the whole-test deadline across the re-arm — matching the always-installed POSIX
handler + still-pending `alarm`.

**Build-checked.** Yes.

**Residual.** `tests/win64/suite_win64.c` registers no `SETUP`/`TEARDOWN`, so this
is not runtime-exercised by the existing suite; real-host verification of a
teardown-bearing suite is the remaining step.

---

## #34 (Med) — Recovery resumes with the faulting routine's `EFLAGS` (DF not cleared)

**Validated.** All three redirect sites (`asmtest_win32_veh`, `rt_veh_cb`,
`rt_timer_cb`) rewrote only `Rsp`/`Rip` and resumed; `EFlags`/`MxCsr` survived
from the faulting routine. A routine that ran `std` (a descending string copy —
exactly the kind of asm this framework tests, and a guard-page overrun with
`DF=1` is designed-for) would leave `DF=1` for the recovered C code, and the
CRT's `rep movs/stos` would then write backwards. POSIX doesn't have this: the
kernel enters a signal handler with `DF=0`.

**Changed.** `src/platform_win32.c`: added `win32_normalize_abi_state(CONTEXT*)`
that clears `DF` (`EFlags &= ~0x400`) and resets both `MxCsr` and
`FltSave.MxCsr` to `0x1F80`. Called in all three redirect paths before setting
`Rip`. The watchdog path (`rt_timer_cb`) now fetches/sets
`CONTEXT_CONTROL | CONTEXT_FLOATING_POINT` (was `CONTEXT_CONTROL` only) so `MxCsr`
is actually restored.

**Build-checked.** Yes (confirmed both `MxCsr` and `FltSave.MxCsr` are valid
mingw x64 `CONTEXT` fields).

**Residual.** The `__builtin_setjmp` 5-word buffer still does not save/restore
`xmm6–15` / `MxCsr` the way a real Win64 `setjmp` does; the ABI-state fix
addresses the DF/`MxCsr` hazard the finding calls out, but full callee-saved-XMM
preservation across the longjmp is out of scope here (unchanged from before).

---

## #35 (Low) — Watchdog vs `test_end` race → double timer delete + false TIMEOUT

**Validated.** If the deadline fires just as the test completes, `rt_timer_cb`
could win the disarm race and hijack the main thread *inside*
`DeleteTimerQueueTimer(..., INVALID_HANDLE_VALUE)`, abandoning that delete
mid-call. `rt_timer` was never nulled, so `run_one` re-entered `test_end` and
issued a **second** `DeleteTimerQueueTimer` on the same timer (MSDN: invalid
double-delete / UAF), and flipped a passing test to TIMEOUT.

**Changed.** `src/platform_win32.c`:
- `test_end` swaps `rt_timer` out **atomically** (`InterlockedExchangePointer`)
  before deleting, so exactly one caller ever deletes it — the double-delete/UAF
  is closed even if a hijack abandons the call mid-delete and `run_one`
  re-enters (the abandoned `DeleteTimerQueueTimer` still completes its work before
  the redirected `Rip` takes effect on return to user mode).
- Added an `rt_finishing` flag set at `test_end` entry (before retiring the arm);
  `rt_timer_cb`, after winning the arm race, checks it and **declines to hijack**
  a thread that has already entered `test_end`. This is the "never hijack a thread
  already finishing" mitigation and removes the common false-TIMEOUT.

**Build-checked.** Yes.

**Residual.** A genuine boundary race remains: if the watchdog wins the arm
exchange *before* the main thread sets `rt_finishing`, a test that completes in
the same instant can still be reported TIMEOUT (a TOCTOU inherent to a deadline
firing exactly at completion). This is now only a rare mislabel — the
crash/UAF (the severe half) is fully eliminated by the atomic swap. Needs
real-host confirmation.

---

## #36 (Low) — `--no-fork` watchdog can't recover a kernel-blocked test

**Validated.** `SetThreadContext` only takes effect on return to user mode, so a
test blocked in a kernel wait (`WaitForSingleObject` on a never-signaled handle,
`Sleep(INFINITE)`, a deadlocked lock) never gets diverted and the runner hangs
forever despite `--timeout`. POSIX `alarm`+`SIGALRM` interrupts the blocking
syscall, so this is a capability regression of the port.

**Changed (mitigation — not a full fix).** `src/platform_win32.c`: added an
`rt_landing_reached` flag set by `rt_landing()`. After the watchdog redirects and
resumes the thread, it **polls** for the landing pad (`RT_LANDING_POLL_MS` × up to
`RT_LANDING_POLL_ITERS`, ~1s ceiling). A user-mode/spin-loop hang reaches the pad
almost immediately and the watchdog returns normally; a kernel-blocked thread
never does, so the watchdog **fails hard** — prints a diagnostic to stderr and
`ExitProcess(ASMTEST_WIN32_HANG_EXIT)` (code `124`, a new documented constant) —
instead of wedging the whole run. Documented the limitation in
`src/platform_win32.h` (the `begin` contract) and `docs/win64.md` (Caveats).

**Build-checked.** Yes; the Wine smoke shows the spin-loop `hang_timed_out` test
still recovers cleanly under `--no-fork` (the poll does not false-fire).

**Residual (by design).** A full in-process fix is not safely achievable: only
*alertable* waits can be broken (via `QueueUserAPC`), and most real hangs
(non-alertable `WaitForSingleObject`, `Sleep`) cannot be; `TerminateThread` is
unsafe (leaks locks/heap). The mitigation converts an infinite wedge into a
bounded, distinctively-coded process exit. The **default forked mode** remains the
recommended containment for tests that may block, and is documented as such.

---

## Files changed

- `src/platform_win32.c` — ABI-state normalize helper (#34); `rt_finishing` +
  atomic timer swap (#35); `rt_landing_reached` poll + hard-exit (#36);
  `asmtest_win32_test_rearm` (#33); `<stdio.h>` include.
- `src/platform_win32.h` — `ASMTEST_WIN32_HANG_EXIT`, `asmtest_win32_test_rearm`
  decl, updated `begin` contract doc (#36 limitation).
- `src/asmtest.c` — Win64 `run_one` rewritten to three scopes + `win32_set_reason_msg`
  (#33).
- `docs/win64.md` — DF/`MxCsr` normalize note (#34); two new Caveats
  (kernel-wait watchdog limit #36; SETUP/TEARDOWN parity #33).

Not edited: `docs/analysis/2026-07-02-code-review.md` (status table owned by the
parent). `src/capture_win64.asm` needed no change.
