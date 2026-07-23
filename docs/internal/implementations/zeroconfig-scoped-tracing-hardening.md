# Zero-config scoped tracing: in-process guards, hygiene assertions, and doc-tail — implementation

> **Sources.** Actioned from
> [scoped-tracing-zeroconfig-plan.md](../archive/plans/scoped-tracing-zeroconfig-plan.md)
> (items ZC-SS-GUARDS, ZC-HYGIENE, ZC-DOCS; especially the §Z1.1 correction of
> 2026-07-16 and §Z5.2/§Z5.4) and
> [call-descent-plan.md](../archive/plans/call-descent-plan.md) (the §L3
> denylist / instruction-budget / watchdog contract). Written 2026-07-17. If this
> doc and a source disagree, this doc wins (sources may be stale); if the CODE and
> this doc disagree, re-verify before implementing.

## Why this work exists

`using (new AsmTrace())` — the zero-config, region-free whole-window scope — ships
and renders a real trace on any x86-64 Linux, but the in-process single-step tier
that powers it took the descent tier's "step into everything" semantics **without
any of its safety guards**: a window over code that reaches a blocking libc call
(a `read` on an empty pipe, a `poll`) hangs the tier for as long as the call
blocks, bounded only by the capture ring's memory, never by time. This doc ports
the three out-of-process guards (deny regions, instruction budget,
`ITIMER_REAL`/`SIGALRM` watchdog) onto the in-process whole-window path, closes
the three hygiene assertions the plan's own test table flags as drafted but never
built (lazy arm claims no slot, auto-name resolution, no malloc/lock under the
SIGTRAP handler), and lands the three doc-tail bullets still verifiably absent
from the user-facing docs. Everything here is pure software with no hardware or
credential gate — the only validation gate is an x86-64 Linux host to run the
lanes on.

## What already exists (verified 2026-07-17)

The landed substrate, all verified against the working tree today:

- [src/ss_backend.c](../../../src/ss_backend.c) — the in-process `EFLAGS.TF`
  single-step backend. The SIGTRAP handler `ss_on_sigtrap` (`:202-233`) has a
  whole-window branch (`:215-222`) that records the **absolute** RIP of every
  executed instruction into a bounded sparse-mmap ring (`SS_WINDOW_CAP` = 1<<20,
  `:126-128`), sets `overflow` when full, and **unconditionally re-asserts TF**
  (`SS_SET_TF(uc)`, `:232`). The handler is malloc/lock-free by contract (file
  header, `:199-201`). `grep -E 'budget|watchdog|deny|ITIMER|SIGALRM'
  src/ss_backend.c` returns **zero hits** — none of the three guards exist on
  this path; the only self-truncation is the ring.
- Frame machinery in the same file: `ss_frame_t` (`:148-162`, with `whole_window`
  and `arm_tid` fields), initial-exec TLS `tls_frames`/`tls_depth`/`tls_gen_ctr`
  (`:166-169`), the `g_arm_refcount`/`g_old_sa` SIGTRAP install-once/restore-once
  discipline (`:178-181`, enforced inside `ss_push_frame` at `:299-317`),
  `asmtest_ss_begin_window` (`:341-344`), `ss_normalize`'s whole-window
  overflow→`truncated` propagation (`:360-375`), `asmtest_ss_end` (`:430-458`),
  `asmtest_ss_self_tid` (`:574`), and `asmtest_ss_frame_lookup` (`:590-614`).
  The ss-layer prototypes are declared **locally in
  [src/hwtrace.c](../../../src/hwtrace.c)** (`:100-110`), not in a public header.
- The guards to mirror, out-of-process:
  [src/descent.c](../../../src/descent.c) — `asmtest_descent_set_insn_budget`
  (`:80`), `asmtest_descent_set_watchdog_ms` (`:85`),
  `asmtest_descent_deny_region` (`:107`); ABI in
  [include/asmtest_ptrace.h](../../../include/asmtest_ptrace.h) `:301-336`;
  conservative defaults in
  [include/asmtest_descent_internal.h](../../../include/asmtest_descent_internal.h)
  `:77-79` (max depth 8, budget 4096, watchdog 2000 ms); the real watchdog
  mechanism in [src/ptrace_backend.c](../../../src/ptrace_backend.c)
  `:977-1013` (`descend_watchdog_arm` at `:991` — repeating `setitimer(ITIMER_REAL)`
  plus a SIGALRM handler installed **without** `SA_RESTART` so a blocked `waitpid`
  returns `EINTR`; old disposition and old itimer saved and restored on disarm).
- The window surface: `asmtest_hwtrace_begin_window` /
  `asmtest_hwtrace_end_window` / `asmtest_hwtrace_render_window`
  ([src/hwtrace.c](../../../src/hwtrace.c) `:2554` / `:2594` / `:2625`;
  declarations [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h)
  `:442-459`; the 12-byte `asmtest_hwtrace_scope_t` with the §Z4 `arm_tid` carry
  at `:340-344`). The region registry is `MAX_REGIONS` = 32
  ([src/hwtrace.c:497](../../../src/hwtrace.c#L497)).
- Tests, all in [examples/test_hwtrace.c](../../../examples/test_hwtrace.c)
  (TAP-style `CHECK(cond, msg)` macro at `:145`, summary `1..N` + `# N passed,
  0 failed` at `:8390`): `test_wholewindow_ss_descend` (`:7254`),
  `test_asynchop_flag` (`:7467`), `test_crossthread_handle_collision` (`:7647`),
  `test_zeroctor_scope_hygiene` (`:7777` — as built it covers **only** nested
  LIFO close with exact inner offsets, SIGTRAP installed-once/restored-once, and
  300-cycle churn; there is no lazy-arm/no-slot, auto-name, or no-malloc/lock
  assertion in its body), `test_wholewindow_banner` (`:7920`),
  `test_zeroctor_managed_compose` (`:8042`).
- The .NET binding: the parameterless ctor is at
  [bindings/dotnet/hwtrace/HwTrace.cs](../../../bindings/dotnet/hwtrace/HwTrace.cs)
  `:2105-2130` (the plan's `:1915` anchor is stale — the file grew; resolve by
  symbol). `_name = ScopeName(member, line)` (`:2111`, format `"member:line"` per
  `ScopeName` at `:3476-3480`) and the public `Name` property (`:2020`). The .NET
  self-test [bindings/dotnet/hwtrace/HwTraceProgram.cs](../../../bindings/dotnet/hwtrace/HwTraceProgram.cs)
  already asserts a **weak** form of auto-name at `:198`
  (`ww.Name.Contains(":")`).
- Lanes: `hwtrace-test` ([mk/native-trace.mk:2128](../../../mk/native-trace.mk#L2128)),
  `docker-hwtrace` ([mk/docker.mk:442](../../../mk/docker.mk#L442); the image's CMD
  runs `make hwtrace-test && make codeimage-test && make hwtrace-python-test`),
  `hwtrace-dotnet-test` ([mk/native-trace.mk:2575](../../../mk/native-trace.mk#L2575)).
- Bindings parity gate: every symbol added to `include/asmtest_hwtrace.h` must be
  wrapped by all ten bindings or exempted in
  [scripts/bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt)
  (`ALL <symbol>` syntax), enforced by
  [scripts/check-bindings-parity.sh](../../../scripts/check-bindings-parity.sh)
  via `make check-bindings-parity`.
- Doc-tail absences (all re-verified by grep today):
  `grep -Ei 'synthetic|tier ladder|region-free'
  docs/guides/tracing/hardware-tracing.md` → none;
  `grep -Ei 'synthetic|whole-window' docs/internal/analysis/trace-parity-matrix.md`
  → none; `grep -E 'SkipReason|provision' docs/reference/troubleshooting.md
  docs/reference/portability.md` → none (troubleshooting has only the raw
  `asmtest_hwtrace_skip_reason` mention at
  [docs/reference/troubleshooting.md:70](../../reference/troubleshooting.md)).
  The landed half of the plan's doc list is the zero-config section at
  [docs/scoped-tracing-implementation.md:122](../scoped-tracing-implementation.md)
  — the tone/content model for Task T7.

**Prove the baseline green before touching anything** (from the repo root, on an
x86-64 Linux Docker host per the CLAUDE.md docker-first rule):

```
make docker-hwtrace        # expect: "1..N" then "# N passed, 0 failed" for
                           # test_hwtrace, then codeimage-test and the python lane
make check-bindings-parity # expect: silence / exit 0
make docker-docs           # expect: "docker-docs: built docs/_build/html/index.html"
```

On a non-x86-64 or non-Linux host the whole-window cases inside `test_hwtrace`
print `# SKIP … x86-64 Linux only` and the suite still ends `0 failed` — that is
the correct baseline there.

## Tasks

### T1 — Add the deny-region and instruction-budget guards to the whole-window SIGTRAP path  (M, depends on: none)

**Goal.** `ss_on_sigtrap`'s whole-window branch gains a per-frame instruction
budget and a process-global deny-region table, both malloc/lock-free in the
handler, such that either guard firing stops TF re-assertion (stops stepping) and
is reported as `truncated`.

**Steps.**

1. In [src/ss_backend.c](../../../src/ss_backend.c), extend `ss_frame_t`
   (`:148-162`) with guard state — keep it small, the TLS budget note at
   `:130-135` is binding:
   `uint64_t steps; uint64_t insn_budget; volatile sig_atomic_t guard;`
   (~24 bytes × `SS_MAX_FRAMES` = 8 → under 200 bytes of extra static TLS).
   Define guard codes in the same file:
   `enum { SS_GUARD_NONE = 0, SS_GUARD_RING, SS_GUARD_BUDGET, SS_GUARD_DENY, SS_GUARD_WATCHDOG };`
2. Add the process-global deny table (NOT per-frame — it must not bloat static
   TLS): `static struct { uint64_t base, len; } g_deny[SS_MAX_DENY];` with
   `#define SS_MAX_DENY 32` and `static volatile uint32_t g_deny_len;`. Writes
   happen only in normal context under the existing `g_ss_lock` (`:181`), entry
   first, then the length (append-only publish, so the lock-free handler read is
   safe). Clear custom entries when `g_arm_refcount` drops to 0 in
   `asmtest_ss_end` (`:447-457`).
3. Add a lazily-populated **default deny set** (blocking libc entries, resolved
   once per process in normal context via `dlsym(RTLD_DEFAULT, …)`, 64-byte
   extent per entry): `read`, `poll`, `select`, `epoll_wait`, `nanosleep`,
   `usleep`, `sleep`, `wait4`, `waitpid`, `accept`, `connect`, `recvmsg`,
   `sem_wait`. This mirrors the fork-path half of
   `asmtest_descent_use_default_denylist`
   ([include/asmtest_ptrace.h:322-336](../../../include/asmtest_ptrace.h#L322));
   deliberately **exclude `write`** (Console/log output inside managed windows
   would otherwise end nearly every capture at the first print — record the
   divergence in a comment). The default set is **opt-in** (see T3): the
   in-process deny semantics below is "stop the capture", which is stronger than
   the descent tier's "step over", and default-on would change the shipped .NET
   `byMethod` lanes (whose stepped JIT paths legitimately touch waits).
4. Rework the whole-window branch of `ss_on_sigtrap` (`:215-222`):
   - if `f->guard != SS_GUARD_NONE`, skip the frame (already stopped);
   - deny check: if `rip` falls in any `g_deny[0..g_deny_len)` entry, set
     `f->guard = SS_GUARD_DENY` (do not record this RIP) — capture ends, the
     denied call then runs at native speed;
   - budget: `f->steps++; if (f->steps >= f->insn_budget) f->guard = SS_GUARD_BUDGET;`
   - ring: keep the existing record/overflow logic; on the first overflow also
     set `f->guard = SS_GUARD_RING` **but keep stepping** (ring overflow bounds
     memory, not perturbation — the shipped behaviour, unchanged).
5. Gate the TF re-assert (`:232`): re-assert iff at least one frame on this
   thread still wants stepping — any region frame (`!f->whole_window`), or any
   whole-window frame with `guard == SS_GUARD_NONE || guard == SS_GUARD_RING`.
   When no frame wants it, do **not** call `SS_SET_TF(uc)`; stepping ceases and
   the window's remainder runs at full speed until `end_window`.
6. Propagate in `ss_normalize` (`:360-375`): the whole-window arm sets
   `t->truncated = true` when `fr->overflow` **or** `fr->guard != SS_GUARD_NONE`.
   The frame's `guard` scalar survives the pop (only `stream` is freed, `:444-445`)
   so T3's accessor can read it render-on-close style.
7. Thread the budget through `ss_push_frame` (`:253`): a new `insn_budget`
   parameter (0 ⇒ default `SS_WINDOW_BUDGET_DEFAULT` = `4ull * SS_WINDOW_CAP`,
   i.e. 4,194,304 steps — chosen so the shipped runaway-loop test, which runs
   ~1.8 M instructions, still hits the **ring** first and stays byte-identical;
   `UINT64_MAX` ⇒ disabled). Region frames pass "disabled". Build after each
   step: `make hwtrace-test` on Linux, or `make docker-hwtrace`.

**Code.** All in `src/ss_backend.c`; no public-header change in this task (T3
owns the surface). The handler additions are integer compares/writes only — no
allocation, no locks, no syscalls; put a comment on the deny loop stating the
worst case (32 compares per trap) is noise against the ~1000× single-step cost.
Mirror the intent comments from `descend_watchdog_arm`'s neighbours in
[src/ptrace_backend.c:977-1013](../../../src/ptrace_backend.c#L977) where the
semantics differ (in-process "stop capture" vs out-of-process "step over").

**Tests.** Covered by T4 (this task alone must keep the existing suite green:
`make docker-hwtrace` still ends `0 failed`, and `test_wholewindow_ss_descend`'s
runaway case still truncates on the ring).

**Docs.** Internal-only at this stage; user-facing docs land in T7/T9 and the
changelog entry in T3.

**Done when.**

- `grep -nE 'SS_GUARD_|g_deny' src/ss_backend.c` shows the new state, and
  `grep -cE 'malloc|pthread_mutex_lock' `-style inspection of `ss_on_sigtrap`'s
  body confirms the handler gained no allocation/lock calls.
- `make docker-hwtrace` → `0 failed` with no new SKIPs.
- `make fmt-check` passes (run `make fmt` after editing).

### T2 — Port the ITIMER_REAL/SIGALRM watchdog onto the whole-window arm  (S, depends on: T1)

**Goal.** A whole-window frame armed with a watchdog is bounded in wall-clock
time even when the stepped code blocks in a syscall: the SIGALRM breaks the block
(`EINTR`), the next trap observes the expiry, stepping stops, and the trace is
`truncated` with guard `SS_GUARD_WATCHDOG`.

**Steps.**

1. In `src/ss_backend.c`, add
   `static volatile sig_atomic_t g_ww_alarm_fired;`, a SIGALRM handler
   `ss_window_alarm` that only sets the flag, and
   `ss_window_watchdog_arm(uint32_t ms)` / `ss_window_watchdog_disarm(void)`
   mirroring `descend_watchdog_arm`/`_disarm`
   ([src/ptrace_backend.c:991-1013](../../../src/ptrace_backend.c#L991)) exactly:
   `sigaction` with `sa_flags = 0` (**no** `SA_RESTART`, so a blocked syscall
   returns `EINTR`), repeating `setitimer(ITIMER_REAL, {ms, ms}, &saved)`, and
   save/restore of BOTH the previous SIGALRM disposition and the previous itimer.
2. Refcount it like the SIGTRAP install: arm on the first whole-window frame
   that requests a watchdog (under `g_ss_lock`), disarm on the last such frame's
   pop in `asmtest_ss_end`. Clear `g_ww_alarm_fired` on arm.
3. In `ss_on_sigtrap`'s whole-window branch (T1 step 4), before recording:
   `if (g_ww_alarm_fired && f->guard == SS_GUARD_NONE) f->guard = SS_GUARD_WATCHDOG;`
4. Thread `watchdog_ms` through `ss_push_frame` alongside T1's budget
   (0 ⇒ default `SS_WINDOW_WATCHDOG_MS_DEFAULT` = 10000; `UINT32_MAX` ⇒
   disabled). Ten seconds, not descent's 2000 ms, deliberately: this default is a
   CI-hang bound, not a perf knob, and must sit far above the shipped runaway
   test's few seconds of legitimate stepping so existing assertions stay
   deterministic.

**Code.** Note in comments the two honest limitations: (a) `ITIMER_REAL` SIGALRM
is delivered process-wide — in a multi-threaded process the kernel may deliver it
to a thread other than the blocked stepping one; the flag + the repeating
interval make expiry eventually observed at the next trap, but breaking a
*blocked* syscall is only guaranteed when the stepping thread can receive
SIGALRM (single-threaded tests do; a `timer_create`+`SIGEV_THREAD_ID` upgrade is
the recorded hardening if a real multi-thread consumer needs it). (b) The
`EINTR` is visible to the traced code — the same intrusiveness class as
single-stepping itself; the trace is flagged, never silently wrong.

**Tests.** Covered by T4's `test_wholewindow_watchdog`.

**Docs.** With T3's changelog entry; user-facing text in T9's provisioning/
troubleshooting update ("a hung zero-config window is bounded by a 10 s
watchdog by default").

**Done when.**

- `grep -n 'ITIMER_REAL\|SIGALRM' src/ss_backend.c` shows arm/disarm + handler.
- `make docker-hwtrace` → `0 failed`; an interactive sanity run (Linux):
  a scratch program arming `begin_window`, then `read(2)` on an empty pipe,
  returns within ~watchdog_ms instead of hanging, with `truncated` set.
- SIGALRM disposition and itimer probed before/after a windowed run are
  byte-identical (asserted properly in T4).

### T3 — Public guard surface, defaults, and bindings-parity exemptions  (S, depends on: T1, T2)

**Goal.** Callers can configure the guards per window through one additive C
entry point, plain `asmtest_hwtrace_begin_window` gets safe defaults (budget +
watchdog on, denylist off), and a post-close accessor reports which guard fired.

**Steps.**

1. In [include/asmtest_hwtrace.h](../../../include/asmtest_hwtrace.h), next to
   the §Z0/§Z1 window block (`:417-459`), add:

   ```c
   typedef struct { const void *base; size_t len; } asmtest_hwtrace_deny_t;
   typedef struct {
       size_t struct_size;            /* F27 idiom: sizeof as compiled into caller */
       uint64_t insn_budget;          /* 0 = default (4x ring); UINT64_MAX = off  */
       uint32_t watchdog_ms;          /* 0 = default (10000);   UINT32_MAX = off  */
       int use_default_denylist;      /* nonzero: blocking-libc entry set          */
       const asmtest_hwtrace_deny_t *deny;  /* extra regions, copied at begin      */
       size_t deny_len;               /* > SS-layer cap (32) => ASMTEST_HW_EINVAL  */
   } asmtest_hwtrace_window_guards_t;
   int asmtest_hwtrace_begin_window_ex(asmtest_trace_t *trace,
                                       const asmtest_hwtrace_window_guards_t *g,
                                       asmtest_hwtrace_scope_t *out);
   int asmtest_hwtrace_window_guard(asmtest_hwtrace_scope_t handle);
   ```

   Document the `struct_size` copy-min-and-zero-fill behaviour by mirroring the
   `asmtest_hwtrace_options_t` comment block (same header, the F27/F36 idiom).
   `asmtest_hwtrace_window_guard` returns `0..4` mapping the `SS_GUARD_*` codes
   (declare matching `ASMTEST_HW_GUARD_*` enum constants in the header) or
   `ASMTEST_HW_EINVAL` on a stale/foreign handle — it resolves through
   `asmtest_ss_frame_lookup` exactly as `asmtest_hwtrace_render_window`
   ([src/hwtrace.c:2625](../../../src/hwtrace.c#L2625)) does, so it works
   render-on-close style on the arming thread after `end_window`.
2. Implement both in `src/hwtrace.c` beside `begin_window` (`:2554`);
   `asmtest_hwtrace_begin_window` becomes a thin `begin_window_ex(trace, NULL,
   out)` forwarder. Extend the local ss prototypes (`:100-110`) for the widened
   `asmtest_ss_begin_window` signature; add link-compatible stubs to the
   non-x86-64 `#else` arm of `ss_backend.c` (`:616-686`) for any new ss symbol.
3. Bindings parity: the gate requires all ten bindings to wrap every new
   `asmtest_hwtrace_*` symbol. Wiring ten shims is out of proportion for a
   C-level power knob, so add to
   [scripts/bindings-parity-allow.txt](../../../scripts/bindings-parity-allow.txt),
   following its `ALL asmtest_asm_exec_native` precedent and comment style:

   ```
   # Whole-window guard knobs (zeroconfig-scoped-tracing-hardening). The zero-config
   # path gets the defaults through plain begin_window, which every binding already
   # wraps; the _ex/guard-query knobs are C-level until a binding needs them.
   ALL asmtest_hwtrace_begin_window_ex
   ALL asmtest_hwtrace_window_guard
   ```

4. Run `make check-bindings-parity` (must stay silent) and
   `make docker-hwtrace`.

**Code.** Defaults live in one place (the ss layer, T1/T2); `begin_window_ex`
only validates (`struct_size`, `deny_len`) and forwards. Do not touch
`asmtest_trace_t` or `asmtest_hwtrace_scope_t` — no ABI change to anything the
ten bindings mirror.

**Tests.** T4 exercises the `_ex` path and the accessor. This task adds one
regression `CHECK` inside `test_zeroctor_scope_hygiene`: a plain
`begin_window`/`end_window` over the 5-insn `ROUTINE` still yields the exact
capture with `truncated == false` and `asmtest_hwtrace_window_guard(handle) == 0`
— proving defaults change nothing for well-behaved windows.

**Docs.** Append to `CHANGELOG.md` under `## [Unreleased]` / `### Added`: the
whole-window guard trio (deny regions / instruction budget / watchdog), the
defaults, and `asmtest_hwtrace_begin_window_ex` + `asmtest_hwtrace_window_guard`.
Header comments are the reference docs at this layer; guide text lands in T7/T9.

**Done when.**

- `make check-bindings-parity` exits 0 with the two new exemptions.
- `make docker-hwtrace` → `0 failed`.
- `nm build/libasmtest_hwtrace.so | grep window` (on Linux) lists both new
  symbols.

### T4 — Assert the guards fire: extend `test_wholewindow_ss_descend`, add `test_wholewindow_watchdog`  (M, depends on: T3)

**Goal.** Each of the four stop reasons (ring, budget, deny, watchdog) is
observed firing by a host-testable case in `examples/test_hwtrace.c`, and the
existing ring assertion becomes deterministic again under the new defaults.

**Steps.**

1. In `test_wholewindow_ss_descend`
   ([examples/test_hwtrace.c:7254](../../../examples/test_hwtrace.c#L7254)):
   - **Pin the runaway ring case** (`:7392-7423`): switch it to
     `asmtest_hwtrace_begin_window_ex` with
     `{ .insn_budget = UINT64_MAX, .watchdog_ms = UINT32_MAX }` so the bounded
     ring is provably the only stopper on any host speed; add
     `CHECK(asmtest_hwtrace_window_guard(oscope) == ASMTEST_HW_GUARD_RING, …)`.
   - **Budget case**: re-run the same `AMD_LOOP` fixture with
     `{ .insn_budget = 1000, .watchdog_ms = UINT32_MAX }`; assert `truncated`,
     `guard == ASMTEST_HW_GUARD_BUDGET`, and `tro->insns_len` is ≥ 1000 and well
     under the ring cap (stepping stopped ≈ at the budget; exact equality is not
     promised because the REP-collapse in `ss_normalize` may drop repeats).
   - **Deny case**: reuse the existing caller/callee pair (`WW_CALLER` /
     `WW_CALLEE`, `:7263-7291`); arm with
     `{ .deny = &(asmtest_hwtrace_deny_t){kp, sizeof WW_CALLEE}, .deny_len = 1 }`
     and assert the trace contains the caller's `cb+0x0`/`cb+0xa` but **none** of
     the callee's `kb+…` IPs, `truncated` is set, and
     `guard == ASMTEST_HW_GUARD_DENY` — the discriminator against run 1, where
     the same callee IS captured.
2. Add `static void test_wholewindow_watchdog(void)` directly after it, called
   from `main` right after `test_wholewindow_ss_descend()` (`:8247`), guarded
   `#if defined(__linux__) && defined(__x86_64__)` with the house
   `# SKIP <subject>: <reason>` prints:
   - probe SIGALRM disposition + `getitimer(ITIMER_REAL)` before;
   - `pipe(fds)`; arm `begin_window_ex` with `{ .watchdog_ms = 300 }`; call a
     tiny C helper that does `read(fds[0], buf, 1)` (blocks — no writer) and
     tolerates `EINTR` by returning; `end_window`;
   - assert: the call returned (the suite did not hang — wrap the whole case in
     the harness `alarm()` belt like the neighbouring live cases), `truncated`
     set, `guard == ASMTEST_HW_GUARD_WATCHDOG`, wall-clock for the window
     `< 5 s` (`clock_gettime` bracket);
   - assert SIGALRM disposition and itimer after full teardown equal the before
     probe (the descent-style save/restore held);
   - close both pipe ends.
3. Because the process is single-threaded at that point, `ITIMER_REAL` delivery
   reaches the blocked stepping thread — state that in the test comment (it is
   the T2 limitation made explicit).
4. Run `make docker-hwtrace` — the count grows by the new `CHECK`s, still
   `0 failed`. Then `make docker-hwtrace-bindings` and `make hwtrace-dotnet-test`
   (or its docker wrapper) to confirm no .NET whole-window assertion regressed
   under the new defaults.

**Code.** Test-only; mirror the fixture and self-skip patterns already inside
`test_wholewindow_ss_descend` (its `ss_map_exec` + `INIT_OPTS` usage). A failure
prints the standard `not ok` line naming the guard that did not fire; a pass adds
green `ok` lines to the TAP stream.

**Tests.** This task **is** the tests for T1–T3. The one unassertable corner:
watchdog expiry racing a genuinely-blocked *multi-threaded* stepper — recorded as
a comment, not a test (see T2's limitation note).

**Docs.** Internal-only (tests); no changelog entry beyond T3's.

**Done when.**

- `make docker-hwtrace` → `0 failed`, and the run demonstrably includes the new
  cases (grep the TAP output for `watchdog` / `budget` / `deny`).
- On a non-Linux/non-x86-64 host the new cases print `# SKIP` and the suite
  still passes.
- Total suite wall-clock grows by ≲ 2 s (the watchdog case's 300 ms + slack).

### T5 — Build the lazy-arm/no-slot and no-malloc/no-lock hygiene assertions  (S, depends on: none)

**Goal.** `test_zeroctor_scope_hygiene` actually asserts the two C-side hygiene
claims the plan drafted: arming machinery appears only at the first window (no
SIGTRAP install at init, no region-registry slot ever), and the SIGTRAP handler
path performs no allocation and takes no lock.

**Steps.**

1. **Lazy arm / no slot**, in `test_zeroctor_scope_hygiene`
   ([examples/test_hwtrace.c:7777](../../../examples/test_hwtrace.c#L7777)):
   - after `asmtest_hwtrace_init` but before the first `begin_window`, probe
     `sigaction(SIGTRAP, NULL, &sa_postinit)` and `CHECK` it equals the pre-init
     disposition — init installs nothing; only the first `begin_window` does
     (the existing `sa_mid` probe already covers "installed while armed");
   - after the existing 300-cycle churn, `CHECK` that
     `asmtest_hwtrace_register_region("hyg_slots", p, sizeof ROUTINE, tr_in)`
     still succeeds — the registry holds `MAX_REGIONS` = 32 slots
     ([src/hwtrace.c:497](../../../src/hwtrace.c#L497)), so if each of the 300
     windows had claimed one, registration would have failed long before; this
     is the observable for "a window claims no slot".
2. **No malloc/lock under the handler**: add counting interposers at the top of
   `examples/test_hwtrace.c`, guarded `#if defined(__linux__) && defined(__GLIBC__)`:
   definitions of `malloc`/`calloc`/`realloc`/`free` and `pthread_mutex_lock`
   that bump C11 `_Atomic` counters and forward via
   `dlsym(RTLD_NEXT, …)` — with the classic static-arena bootstrap for the
   allocations `dlsym` itself performs before the real symbol is resolved
   (a small static buffer served until the function pointer is non-NULL).
   The test binary already links `-ldl` (the `hwtrace-test` link line in
   [mk/native-trace.mk](../../../mk/native-trace.mk) carries it).
3. In the hygiene test, bracket a windowed call:
   snapshot the counters, `begin_window` … `fn(20, 22)` … snapshot again
   **before** `end_window` (normalize in `asmtest_ss_end` legitimately runs
   Capstone/trace pools in normal context — it must stay outside the bracket).
   `CHECK` both deltas are zero AND that the window really trapped
   (`tr->insns_len > 0` afterwards) so the zero is not vacuous. The process is
   single-threaded at that point, so a zero delta is attributable to
   the leaf (pure asm) + the handler — i.e. the handler path.
   Note in a comment that `begin_window` itself allocates (mmap of the ring)
   **before** the bracket opens — the claim under test is the *handler*, not the
   arm.
4. If the interposition proves fragile on a lane (musl images, sanitizers), the
   recorded fallback is a `mallinfo2()` `uordblks` before/after compare — weaker
   (a malloc+free pair cancels) but still catches a net-allocating handler.
   Prefer the interposers; keep them `#ifdef`-scoped so non-glibc builds compile
   and print `# SKIP hygiene no-malloc: glibc interposition unavailable`.
5. `make docker-hwtrace` (Debian/glibc image → the interposed path runs).

**Code.** Test-only. The interposers live in `examples/test_hwtrace.c` above
`main`, ~40 lines, commented as the ZC-HYGIENE observable — do not export them.

**Tests.** Failure modes look like:
`not ok … hygiene: SIGTRAP untouched by init` (an eager install regression),
`not ok … hygiene: 300 windows claimed no region slot` (a slot leak),
`not ok … hygiene: no malloc under the armed window (delta=N)` (an allocation
snuck into `ss_on_sigtrap`). A pass is three more `ok` lines in the existing
test's TAP stream.

**Docs.** Internal-only, no user-facing docs — hygiene tests assert existing
contracts; nothing user-visible changes.

**Done when.**

- `make docker-hwtrace` → `0 failed` with the new `CHECK`s present in the
  output.
- Mutation check (manual, once): adding a `malloc(1)` into `ss_on_sigtrap`'s
  whole-window branch and rerunning makes exactly the no-malloc `CHECK` fail —
  proving the probe sees the handler.

### T6 — Strengthen the auto-name assertion in the .NET lane  (S, depends on: none)

**Goal.** The `[CallerMemberName]`/`[CallerLineNumber]` auto-name is asserted to
resolve to the actual call site, not merely to contain a colon.

**Steps.**

1. In [bindings/dotnet/hwtrace/HwTraceProgram.cs](../../../bindings/dotnet/hwtrace/HwTraceProgram.cs),
   replace the weak assert at `:198`
   (`Check(ww.Name.Contains(":"), …)`) with a call-site-exact one: capture the
   expected values at the ctor line —

   ```csharp
   int ctorLine = new System.Diagnostics.StackTrace(true).GetFrame(0).GetFileLineNumber(); // or hardcode via a nameof-based helper
   ```

   Simpler and robust (no PDB dependency): wrap the scope construction in a tiny
   local method `static AsmTrace OpenScopeForNameTest() => new AsmTrace(emit: false);`
   and assert `t.Name.StartsWith("OpenScopeForNameTest:")` — `ScopeName` is
   `$"{member}:{line}"` ([HwTrace.cs:3476-3480](../../../bindings/dotnet/hwtrace/HwTrace.cs#L3476)),
   so the member half is exact and the line half is asserted numeric (`int.TryParse`
   of the suffix). Keep the assert inside the existing `if (ww.Armed)` /
   self-skip shape so non-Linux hosts stay green.
2. Also assert the name reached the **handle side**, not just the C# field: on
   an armed scope, `Name` is what keys the §Z5 presentation — assert it is
   non-empty even when `Armed == false` (the ctor names the scope before the
   arm attempt, [HwTrace.cs:2111](../../../bindings/dotnet/hwtrace/HwTrace.cs#L2111)).
3. Run `make hwtrace-dotnet-test` (Linux + .NET SDK) or the docker dotnet lane
   `make docker-hwtrace-dotnet`, which runs exactly `docker run asmtest-dotnet
   make hwtrace-dotnet-test` (generated by the `docker_hwtrace_lang_rule`
   template, [mk/docker.mk:638-643](../../../mk/docker.mk#L638)). Note
   `make docker-dotnet` runs only the generic .NET binding smoke test and does
   **not** exercise this assertion.

**Code.** Test-only C# change; ~10 lines.

**Tests.** A regression (auto-name defaulting, a broken `ScopeName` truncation)
now fails with the observed name in the message
(`AsmTrace(): auto-name resolves to OpenScopeForNameTest:<line> (got …)`).

**Docs.** Internal-only — the auto-name behaviour is already documented in the
binding's XML docs.

**Done when.**

- `make hwtrace-dotnet-test` prints the new `ok` line and exits 0 on Linux;
  on hosts where the whole-window tier self-skips, the `SkipReason` branch keeps
  the lane green.

### T7 — Guide: the three-tier region-free ladder + the synthetic decode-validation mode  (S, depends on: none; lands cleanly before or after T1–T4)

**Goal.** [docs/guides/tracing/hardware-tracing.md](../../guides/tracing/hardware-tracing.md)
documents the zero-config whole-window scope: the WEAK/STRONG/CEILING ladder and
how the PT decode is validated without silicon.

**Steps.**

1. Insert a new `## The zero-config whole-window scope (region-free)` section
   after `## Auto-selecting a backend` (`:244`) and before
   `## W^X executable memory` (`:301`). Mirror the tone and honesty of the
   zero-config section at
   [docs/scoped-tracing-implementation.md:122](../scoped-tracing-implementation.md);
   this is a **published** page, so link the internal plan via its GitHub blob
   URL (`https://github.com/wilvk/asm-test/blob/main/docs/internal/archive/plans/scoped-tracing-zeroconfig-plan.md`),
   exactly as that page already does — never a relative link into
   `docs/internal/`.
2. Content: the C surface (`asmtest_hwtrace_begin_window` / `_end_window` /
   `_render_window`), the .NET `using (new AsmTrace())` form, then the ladder as
   a short table:
   - **WEAK** — in-process single-step, descends into everything, any x86-64
     Linux, unprivileged, ships today; honestly noisy and self-truncating
     (bounded ring; after T1–T4 also deny/budget/watchdog-bounded — phrase this
     to match whatever has landed when the section is written);
   - **STRONG** — whole-window Intel PT: the decode
     (`asmtest_pt_decode_window`) is **validated on a synthetic PT-packet
     fixture** and its live capture is forward-look — needs bare-metal Intel PT
     plus `perf_event_paranoid < 0` or `CAP_PERFMON`; on every other host the
     stub self-skips;
   - **CEILING** — AMD LBR, shipped as a **sampled hot-method survey**
     (`IsStatistical`), never an exact whole window; needs **Zen 4+** bare metal
     with `CAP_PERFMON`. The complete AMD capture is the out-of-process ptrace
     stepper (block-step mode — landed), and the eBPF boundary LBR snapshot
     (landed; Zen 4/5, Linux ≥ 6.10) reads the 16-entry window deterministically
     at scope boundaries. Write **Zen 4+**, never "Zen 3+", and write P2/P3 as
     landed, never "when it lands".
3. The synthetic decode-validation paragraph: `asmtest_pt_encode_fixture`
   ([src/pt_backend.c:305](../../../src/pt_backend.c#L305)) hand-assembles a
   valid PT byte stream that `test_wholewindow_decode` decodes end-to-end with
   **libipt at build time and no PT hardware** (the `docker-hwtrace` image
   installs `libipt-dev` for exactly this); state plainly that the fixture
   exercises decode logic but not a live AUX ring's PSB cadence/`aux_tail`
   wrap — necessary, not sufficient, for trusting the live path.
4. Build: `make docker-docs` (Sphinx `-W`, so any broken link fails the build).

**Code.** None — docs only.

**Tests.** `make docker-docs` is the gate; additionally
`grep -Ei 'synthetic|tier ladder|region-free' docs/guides/tracing/hardware-tracing.md`
flips from zero hits to nonzero.

**Docs.** This task IS the docs. Add a `CHANGELOG.md` `### Added` bullet
(zero-config whole-window guide section) — one combined entry may cover T7+T9.

**Done when.**

- `make docker-docs` succeeds with the new section rendered.
- The section states the CEILING floor as Zen 4+ and the block-step/eBPF
  snapshot upgrades as landed (grep the new text to confirm no "Zen 3+" and no
  "when it lands").

### T8 — Parity matrix: whole-window decode's validated-on-synthetic / live-forward-look status  (S, depends on: none)

**Goal.** [docs/internal/analysis/trace-parity-matrix.md](../analysis/trace-parity-matrix.md)
records that the whole-window PT decode is synthetic-validated only.

**Steps.**

1. Add a short `### Whole-window (region-free) scope` subsection under
   `## Matrix 3 — x86 native trace × CPU vendor / microarchitecture` (`:127`),
   where per-tier caveats already live (internal doc — relative links fine).
2. State, citing code: the region-free single-step whole-window path is live on
   any x86-64 Linux (`asmtest_hwtrace_begin_window`,
   [src/hwtrace.c:2554](../../../src/hwtrace.c#L2554)); the whole-window PT
   decode `asmtest_pt_decode_window` is exercised **only** by the synthetic
   fixture (`asmtest_pt_encode_fixture` →
   `test_wholewindow_decode`, wherever libipt is built) and has **never run
   against live Intel PT silicon** — live capture is forward-look, and hosts
   built without libipt compile the `ENOSYS` stub and provide no de-risk.
   If T1–T4 have landed, add one sentence that the in-process whole-window tier
   is guard-bounded (deny/budget/watchdog); otherwise omit.
3. Keep any AMD statement in the new text at the **Zen 4+** floor; do not touch
   the file's existing Zen 3 rows — correcting them belongs to
   [amd-branchsnap-lbr-docs.md](amd-branchsnap-lbr-docs.md).

**Code.** None.

**Tests.** `docs/internal/**` is excluded from the Sphinx build, so the check is
`grep -i 'synthetic' docs/internal/analysis/trace-parity-matrix.md` → nonzero,
plus a read-through for the two binding positions above.

**Docs.** Internal-only; no changelog entry (internal analysis docs are not
user-visible).

**Done when.**

- The subsection exists under Matrix 3 with the code citations resolving
  (spot-check the line anchors against the tree).
- No "Zen 3+" and no "planned" language for block-step/eBPF snapshot in the
  added text.

### T9 — Reference docs: SkipReason codes, the one-time provisioning table, and the Linux-only floor  (S, depends on: none)

**Goal.** [docs/reference/troubleshooting.md](../../reference/troubleshooting.md)
and [docs/reference/portability.md](../../reference/portability.md) tell a
developer why a zero-config scope self-skipped and the one-time grant that fixes
it.

**Steps.**

1. In `troubleshooting.md`, extend the `## Hardware-trace tiers` section
   (`:65-76`, around the existing `asmtest_hwtrace_skip_reason` stub at `:70`)
   with two additions:
   - **The skip-reason table.** The reason strings are static literals produced
     by `hw_classify` ([src/hwtrace.c:350-420](../../../src/hwtrace.c#L350));
     tabulate them with their meaning + fix: `built without libipt` /
     `built without OpenCSD` / the two Capstone variants (build-time decoder
     absent — install the pinned decoder, see the Dockerfiles);
     `not a GenuineIntel x86-64 host` / `not an AArch64 host` /
     `single-step backend is x86-64 Linux/macOS only …` (wrong CPU/OS — real
     gates); `no intel_pt PMU (needs bare-metal Intel; absent on AMD/VM)` /
     `no cs_etm PMU …` (PMU absent); the two permission strings
     (`perf_event capture not permitted …`, `perf branch-stack not permitted …`)
     → the provisioning table below. For the AMD no-hardware row, describe the
     gate as **"AMD LBR live capture needs Zen 4+ (LbrExtV2)"** — do not copy
     the current C literal's "Zen 3 BRS" clause; Zen 3 BRS was never opened by
     this tree and the literal itself is being aligned by
     [amd-branchsnap-lbr-docs.md](amd-branchsnap-lbr-docs.md). Mention the .NET
     surfaces: `AsmTrace.SkipReason` (per-scope) and `HwTrace.DegradationNote()`
     (the composed ladder message).
   - **The one-time provisioning table** (from the plan's §Z5.4): whole-window
     PT / AMD LBR → `sudo sysctl kernel.perf_event_paranoid=-1` **or**
     `setcap cap_perfmon+ep <binary>` (in a container: `--cap-add=PERFMON`);
     unprivileged PT still arms with a 128 KiB AUX ring — stated, not failed;
     ptrace stealth fallback → Yama `PR_SET_PTRACER` or `CAP_SYS_PTRACE`
     (default Docker seccomp permits `ptrace(2)` on host kernel ≥ 4.8); the eBPF
     emission slicer → `CAP_BPF` + kernel BTF (cross-link the existing
     `## eBPF code-image detector` section at `:86-90`).
2. Still in that section, add the **Linux-only floor** sentence: the zero-config
   whole-window facility is Linux-only across every binding; off Linux the scope
   records nothing and says why (`SkipReason`), never hard-fails.
3. In `portability.md`, under `## Supported targets` (`:7`), add a short
   scoped-tracing paragraph with the precise split: region-scoped single-step
   runs on x86-64 **Linux and macOS (Intel)**; the region-FREE whole-window
   scope (`begin_window`, the empty ctor) is **Linux x86-64 only** (the
   `begin_window` body is `#if defined(__linux__)`,
   [src/hwtrace.c:2563](../../../src/hwtrace.c#L2563)); everywhere else the same
   code self-skips with a reason. Cross-link the troubleshooting provisioning
   table.
4. `make docker-docs` (both files are published Sphinx pages; `-W` gates broken
   links).

**Code.** None.

**Tests.** `make docker-docs` green;
`grep -E 'SkipReason|provision' docs/reference/troubleshooting.md docs/reference/portability.md`
flips from zero to nonzero.

**Docs.** This task IS the docs; covered by the shared T7/T9 changelog bullet.

**Done when.**

- Both pages build and render the new tables.
- The AMD row reads Zen 4+ (no "Zen 3"), the provisioning table lists all three
  grant families, and the Linux-only floor appears in both files.

## Task order & parallelism

```
T1 ─► T2 ─► T3 ─► T4        (the guards spine — strictly ordered; critical path)
T5                          (independent — C hygiene tests)
T6                          (independent — .NET auto-name)
T7, T8, T9                  (independent of everything and of each other;
                             T7/T8 read slightly better AFTER T4 lands, but
                             each contains phrasing guidance for either order)
```

Two people can work concurrently: one on the T1–T4 spine, one on
T5/T6/T7/T8/T9 in any order. The only cross-file collision risk is
`examples/test_hwtrace.c` (T4 and T5 both edit it — coordinate or sequence
those two).

## Constraints & gates

- **No hardware or credential gate for the work itself** — everything here is
  software and docs. The one *validation* gate: the whole-window single-step
  lanes execute only on **x86-64 Linux** (in Docker is fine; the ss backend
  compiles to `ENOSYS` stubs elsewhere, and qemu-user cannot single-step). On a
  non-x86-64 host (e.g. an Apple-silicon laptop) the new tests must self-skip
  with printed reasons and the suite must still end `0 failed`; final
  validation happens on the x86-64 CI lanes (`docker-hwtrace` and friends).
  Record in the PR which lanes actually executed vs self-skipped.
- **CLAUDE.md dependency rule**: no new installable dependency is introduced by
  this doc (glibc `dlsym`/`RTLD_NEXT` and `-ldl` are already in the build; the
  `docker-hwtrace` image already carries `libipt-dev`). If an implementer finds
  a lane missing something installable, extend that `Dockerfile.*` with a pinned
  version — never a self-skip.
- **ABI discipline**: nothing in T1–T3 may change `asmtest_trace_t`,
  `asmtest_hwtrace_scope_t`, or any struct the ten bindings mirror. New public
  symbols follow the `struct_size` (F27) idiom and get `ALL` parity exemptions
  with a dated comment.
- **Handler discipline**: every addition to `ss_on_sigtrap` must be
  async-signal-safe — no malloc, no locks, no syscalls; T5's interposer test is
  the enforcement.
- **Formatting/changelog**: `make fmt` before committing (`fmt-check` is
  CI-gated); user-visible changes (T3, T7+T9) append under `## [Unreleased]` in
  `CHANGELOG.md`.
- **Binding positions honored throughout** (do not reintroduce the refuted
  claims): AMD LBR live-capture floor is **Zen 4+**; BTF block-step and the eBPF
  boundary snapshot are **landed** primitives; there is **one PT arm** (owned by
  [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md)) — T7/T8
  describe the shipped synthetic-validated decode and must not spec any parallel
  PT implementation.

## Research notes (verified 2026-07-17)

No external research was needed or assigned for this doc: every fact it depends
on is in-tree and was re-verified against the working tree today (file paths,
line anchors, make targets, and the grep-verified absences listed under "What
already exists"). The systems facts the guards rely on — `EFLAGS.TF` semantics,
`ITIMER_REAL`/`SIGALRM` without `SA_RESTART` interrupting blocked syscalls with
`EINTR`, initial-exec TLS async-signal-safety — are already encoded and battle-
tested in the two in-tree implementations being mirrored
([src/ss_backend.c](../../../src/ss_backend.c),
[src/ptrace_backend.c:977-1013](../../../src/ptrace_backend.c#L977)), which is
exactly why the tasks mirror rather than invent.

## Out of scope

- **The STRONG whole-window PT capture arm** (perf-AUX open/mmap/drain/decode,
  the WEAK/STRONG/CEILING auto-selection ladder wiring, `pt_begin_window`) —
  [intel-pt-whole-window-substrate.md](intel-pt-whole-window-substrate.md); foreign-pid
  extension in [intel-pt-attach-foreign-pid.md](intel-pt-attach-foreign-pid.md).
- **The live managed (§Z3 live half / §Z4 stitching escalation) compose** —
  [managed-wholewindow-compose.md](managed-wholewindow-compose.md).
- **AMD LBR documentation truth-up** (the Zen 4+ floor corrections across
  guides/headers/matrix, the freeze-gate retirement, the silicon-gated capture
  arm) — [amd-branchsnap-lbr-docs.md](amd-branchsnap-lbr-docs.md); IBS window-lane
  honesty — [amd-ibs-backend-honesty.md](amd-ibs-backend-honesty.md).
- **In-process block-step (BTF) as a cheaper whole-window record mode** —
  [inproc-btf-block-step.md](inproc-btf-block-step.md); this doc's guards land on
  the per-instruction TF path and must not block or pre-empt that work.
- **Out-of-process stepper correctness** (the descent tier itself, si_code
  handling) — [ptrace-blockstep-tracer-correctness.md](ptrace-blockstep-tracer-correctness.md).
